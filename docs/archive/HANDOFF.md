# HANDOFF — stan projektu „KarnyNow" (port OpenNOW na TUI + C)

> ## ⚠️ TEN PROJEKT MA NOWY DOKUMENT STARTOWY
>
> **Od rundy VI punktem wejścia dla agenta wdrażającego jest `BOOTSTRAP.md`** — napisany
> **po angielsku**, samowystarczalny, z pełną specyfikacją logowania (§7), rejestrem ryzyk,
> mapą badań i bramkami. Nie zawiera kodu do skopiowania (użytkownik chce, żeby drugi agent
> zaimplementował wszystko sam); zawiera natomiast wszystkie *fakty* i wskazówki „gdzie szukać".
>
> Ten plik (`HANDOFF.md`) pozostaje **dziennikiem badań i pamięcią sesji asystenta**:
> historia pomiarów, ewolucja decyzji, uzasadnienia. Czytaj go, gdy potrzebujesz wiedzieć
> *dlaczego* coś ustalono tak, a nie inaczej.
>
> Podział: **`BOOTSTRAP.md` = co robić** · **`HANDOFF.md` = jak do tego doszliśmy** ·
> **`analiza/04` i `analiza/05` = szczegóły bajtowe i weryfikacja faktów**.

> **Ten plik jest punktem wejścia badań.** Napisany tak, żeby agent AI w **nowej** rozmowie
> mógł przejąć pracę bez czytania historii czatu. Aktualizowany na początku i na końcu
> każdej sesji (sekcja 12 = dziennik).
>
> **Ostatnia aktualizacja:** 2026-10-20, **runda VI** — wyniki T4 v3 (crash zlokalizowany),
> T4 przepisane na v4 + przetestowana warstwa JSON, napisany `BOOTSTRAP.md`.
> Otwarta pozostaje **jedna** bramka: **G0.2** (ścieżka prezentacji D3D9) → patrz §10.

---

## 1. Czym jest projekt

Zbudować klienta **GeForce NOW** na sprzęt, na którym oficjalny klient i OpenNOW
**nie działają**:

- **GUI:** zamiast Qt6 → **TUI w Pythonie** (logowanie, lista gier, kolejka, launch)
- **Streaming:** zamiast Rusta → **czysty C** (NVST: transport, dekodowanie, audio, wejście, prezentacja)
- **Wzorzec do naśladowania:** [OpenCloudGaming/OpenNOW](https://github.com/OpenCloudGaming/OpenNOW), licencja **MIT**

**Nie jest to port kodu.** OpenNOW nie uruchomi się na tym sprzęcie (dowód w §5.1), więc
bierzemy z niego *wiedzę o protokole*, a nie implementację.

Zasady projektu (ustalone przez użytkownika, niepodlegające negocjacji):
- nie omijamy logowania, uprawnień, subskrypcji ani limitów kolejki
- nie kradniemy cookies/tokenów z innych klientów
- **nie wyłączamy walidacji TLS** — jeśli store certyfikatów blokuje, diagnozujemy, nie obchodzimy
- nie implementujemy własnej kryptografii (używamy sprawdzonych bibliotek)
- w trybie MOCK nie wysyłamy nic do sieci
- **najpierw pomiar, potem kod** — każdy etap ma mierzalną bramkę

---

## 2. Maszyna docelowa (= maszyna developerska)

| Element | Wartość | Konsekwencja |
|---|---|---|
| Model | Dell Latitude E5500 | — |
| CPU | Intel Core 2 Duo **T7250**, 2C/2T, Merom, 2,0 GHz, FSB 800 | **SSE3 + SSSE3, brak SSE4 i AVX** → `-march=core2 -msse3` |
| RAM | **2 GB** DDR2 (2×1 GB) | wolne ~1,25 GB; wszystkie bufory ograniczone |
| GPU | Intel **GMA 4500MHD**, PCI `8086:2A42` | **D3D 10.0 / SM 4.0 / OpenGL 2.1 — BRAK D3D11, BRAK DXVA2** |
| Sterownik | `8.15.10.2869` (= 15.17.57.2869), pliki `igdumd64.dll` / `igdumdx32.dll` | `Description` mówi „Intel(R) HD Graphics Family" — **nazwa jest niewiarygodna, PCI ID jest** |
| Ekran | 1280 × 800, **60 Hz** (wcześniej raportował 40 Hz — patrz §7 R9) | okno ≤ 1280×800; brak sensu w 1088p |
| System | Windows 7 SP1 **x64** (6.1.7601), modyfikacja JG 2021 | Python ≤ 3.8.x; conhost **bez** parsera ANSI |
| Konsola | **ConEmu** build 230724, `C:\Program Files\ConEmu\ConEmu`, sesja **podniesiona (ADMIN)** | ConEmu jest **wymagany** (decyzja D1) |
| Łącze | Wi-Fi 2,4 GHz lub tethering telefonu przez USB 2.0 | straty pakietów i jitter są normą → FEC i NACK muszą działać |
| Toolchain | **w64devkit**, GCC **16.2.0** (mingw-w64 **v14.0**), cmake 4.4.3, ninja 1.13.2, make 4.4.1, git 2.46.2 | patrz §6 F1 |
| Python | **3.8.20** x64, `C:\Python\Python38\python.exe`, ssl **OpenSSL 1.1.1w** | zainstalowane **tylko** pip 25.0.1 + setuptools 75.3.4 |
| FFmpeg | `2026-02-18-git-52b676bb29-essentials_build-www.gyan.dev`, statyczny, `--enable-dxva2 --enable-mediafoundation`, **`--enable-gpl`** | licencja **GPL** → cały klient GPL (zaakceptowane, D3) |
| Brak na PATH | `bash`, `powershell`, `py`, `nasm`, `dumpbin`, `cl`, `clang` | testy muszą być czystym Pythonem (`cmd` + `sh` są) |

**Uwaga:** `python` działa, `py` nie. Skrypty muszą być kompatybilne z **Python 3.8**
(bez `dict | dict`, bez `list[str]`, bez walrus w niezwykłych miejscach).

---

## 3. Architektura docelowa

```
┌──────────────────────────────┐        ┌───────────────────────────────────┐
│  karnynow.exe  (Python 3.8)  │        │  knvstd.exe  (C11, mingw)         │
│  MUSI startowac przez ConEmu │        │                                   │
│                              │ JSON   │  • CloudMatch → kontekst sesji    │
│  • TUI: Textual/Rich         │─lines─▶│  • WSS + RTSP (OPTIONS..PLAY)     │
│  • OAuth 2.0 device grant    │ stdout │  • Mjolnir UDP 49005: SRTP,       │
│  • GraphQL: katalog/bibliot. │◀───────│    GS 0x4753, reorder, RS-FEC,    │
│  • v2/serverInfo → vpcId     │ events │    NACK-v2 0x0317                 │
│  • v2/session (kolejka)      │        │  • bundle 49006: ICE + DTLS +     │
│  • vault DPAPI na tokeny     │        │    SCTP → Opus, input, RTCP       │
│  • detekcja hosta konsoli    │        │  • ffmpeg libavcodec h264 → NV12  │
│                              │        │    (threads=1!)                   │
└──────────────────────────────┘        │  • WASAPI (audio out)             │
         ▲                              │  • D3D9 NV12 StretchRect → GDI    │
         │                              │  • Raw Input → ramki NVB          │
   okno ConEmu                          └───────────────────────────────────┘
                                                     │
                                          osobne okno 1280×800 (WS_POPUP)
```

**Dlaczego dwa procesy, nie `ctypes`:**
1. OpenNOW ma dokładnie ten wzorzec i on działa — `docs/core-protocol.md`: newline-delimited
   UTF-8 JSON po dziedziczonym stdin/stdout, limit linii **1 MiB**, stderr na redagowaną
   diagnostykę, 5 s na handshake, `unknown/malformed/oversized` → rozłączenie zamiast
   niejednoznacznego stanu. **Kopiujemy tę specyfikację 1:1** (z `protocolVersion` i `capabilities`).
2. **Crash drivera jest realny, nie teoretyczny** — T4 v1 zcrashował z 0xC0000005 wewnątrz
   D3D9 (§7 R2). Rozdzielenie procesów sprawia, że crash prezentacji nie gubi tokenów OAuth.
3. GIL i `timeBeginPeriod`: pętla prezentacji potrzebuje twardego timera i priorytetu wątków.
4. Python nie dotyka pikseli ani pakietów → jego wersja (3.8, EOL) nie wpływa na tor medialny.

**Tryby pracy streamera** (żeby testować bez konta):
- `--loopback` : syntetyczne klatki H.264 z pliku → cały tor dekod+prezentacja
- `--capture`  : tylko Mjolnir → zrzuca access unity do `.264` (diagnoza bez dekodera)
- `--real`     : pełna sesja

---

## 4. Decyzje (register z warunkami cofnięcia)

| # | Decyzja | Powód | Warunek cofnięcia |
|---|---|---|---|
| **D1** | **ConEmu jest wymaganym hostem konsoli.** TUI = Textual/Rich. Klient przy starcie **wykrywa host** i jeśli VT nie działa — kończy z komunikatem i kodem 2, zamiast rysować krzaki. | T5: dowód twardy w obie strony. W ConEmu załadowany `ConEmuHk64.dll`, ANSI działa (atrybuty `0x4`/`0xC`/`0x9`/`0xF`). W czystym `cmd.exe` hooka nie ma, a bajt `ESC` wpada do bufora jako znak i font rysuje go jako `←`. **Trzeci powód (po D10):** w czystym conhost profil domyślny ma `FaceName=null` = font rastrowy **bez glifów ramek Unicode** — TUI byłoby nieczytelne nawet bez ANSI. ConEmu robi fallback glifów. | Gdyby ConEmu okazał się niedostępny → własny backend Win32 (`WriteConsoleOutputW` = 0,1 ms/ekran) + ramki ASCII. |
| **D2** | **Domyślnie 1280×720@30**, przełączane w `ANNOUNCE` (`clientViewportWd/Ht`, `maxFPS`). Niższe szczeble: 540p60, 540p30, 360p30. **1088p odrzucone** (ekran ma 1280×800). | T1: 720p30 = 24,8 % obu rdzeni; z mnożnikiem 1,8× za treść grową ≈ 45 %. 720p60 = 46 % (z mnożnikiem ~83 %) → za dużo na domyślne. | Jeśli praktyka pokaże, że 720p30 szarpie → 540p60 (28,7 %). |
| **D3** | **Dekoder: FFmpeg `libavcodec` h264** z istniejącego builda. **openh264/T6 PORZUCONE** — nie chcemy walczyć z budową (NASM + meson). Cały klient = **GPL**. | Decyzja użytkownika + pomiar T1 (211 fps dla 720p30 = 7× real-time). Build FFmpeg już działa na tej maszynie. | Gdyby GPL kiedyś przeszkadzał → openh264 (BSD-2), ale wymaga NASM ≥ 2.10.06. |
| **D4** | **`threads = 1` dla dekodera.** Popraw `thread_count ≤ 2` w ABI na **`== 1`**. | T1: 720p30 → 211 fps (1 wątek) vs 191 fps (2 wątki). Na 2-rdzeniowym Meromie synchronizacja kosztuje więcej niż zyskuje. Drugi rdzeń zostaje na sieć, Opus, prezentację i Pythona. | Jeśli pomiar na żywym strumieniu pokaże inaczej. |
| **D5** | **Nie próbujemy DXVA2 w ogóle** — ani jako głównej ścieżki, ani jako fallbacku. | T1: `-hwaccel dxva2` daje 3-4× **gorszy** wynik niż software (720p30: 50 fps vs 211 fps) i `rc=0`, więc wygląda jak sukces. Twój probe: `QI(IDirectXVideoDecoderService)=0x80004002`. | — (rozstrzygnięte pomiarem) |
| **D6** | **Skalowanie robi GPU, nie CPU.** Jeśli T4 wykluczy skalowanie na GPU → prezentacja **1:1** (720p w oknie 1280×800 = letterbox), nie stretch. | T1: `scale=1280:800` dokłada **11,8 ms/klatkę** = +18 pkt proc. (24,8 % → 42,5 %). To najdroższa pojedyncza operacja w torze wideo. | — |
| **D7** | **Prezentacja: łańcuch fallbacków probebowany raz na starcie sesji**: NV12 `StretchRect` → YUY2 → X8R8G8B8 → GDI `SetDIBitsToDevice`. Każdy krok poprzedzony `CheckDeviceFormatConversion`; przy odmowie **nie wywołujemy** i idziemy niżej. Przy AV — trwała degradacja, bez ponawiania. | Crash T4 v1 (0xC0000005) pokazał, że wywołanie wbrew sterownikowi zabija proces. | Wynik T4 v3 może skrócić łańcuch. |
| **D8** | **Docelowa architektura bitowa: x86_64**, `-static -static-libgcc -static-libstdc++`. x86 jako plan B przy problemach z RSS. | T2: oba działają, ale x64 ma lepsze domyślne (PE subsys 5.2 vs 4.0) i 16 rejestrów XMM. | Jeśli RSS przekroczy budżet przy 2 GB RAM. |
| **D10** | **TUI jest obsługiwane WYŁĄCZNIE klawiaturą.** Strzałki + Enter/Esc/litery. **Mysz nie jest wymagana** i nie projektujemy pod nią UI. `Textual(mouse=False)`. Resize bierzemy z `GetConsoleScreenBufferInfo` odpytywanego co tick pętli (nie ze zdarzeń). | Decyzja użytkownika: „nie przepadam za użyciem myszy w tui, byle lista gier była dobrze wyświetlona i strzałeczki są najlepsze". Pomiar T3 i tak potwierdza, że klawiatura daje komplet: VK 13 (Enter), 37-40 (strzałki), 65/90 (A/Z), 112 (F1). | — (preferencja użytkownika) |
| **D11** | **Resize TUI przez polling, nie przez zdarzenia.** Co tick pętli: `GetConsoleScreenBufferInfo` → porównaj `srWindow` → przerysuj, jeśli się zmieniło. | T7: `resize_reporting_works=false` (ConEmu nie wysyła `ESC[8;H;Wt` do aplikacji w tej konfiguracji), a T3 w 20 s nie złapał ani jednego zdarzenia typu 4. Polling kosztuje ~0 (jedno wywołanie API na tick). | Gdyby Textual i tak dostawał resize od ConEmu — polling jest wtedy nieszkodliwy. |
| **D12** ✅ **WYKONANE** | **Kod logowania użytkownika (4 pliki, ~3 000 linii) przejrzany i udokumentowany** → `analiza/05-PRZEGLAD-KODU-LOGOWANIA.md`, gdy użytkownik go przyśle — razem z działającym T4. Nie przepisujemy go, tylko **dokumentujemy wykonane w nim badania** i uzgadniamy z ustaleniami z OpenNOW (GFNJWT vs Bearer, `vpcId`, device grant). | Użytkownik: „mam działający system logowania… boję się wysłać, ale jeśli mogę, to przy kolejnym teście… przejrzysz i udokumentujesz badania jakie tam wykonano". | — |
| **D9** | **Klient nie zakłada stałego odświeżania.** Mierzy interwał między `Present` i adaptuje. Opcjonalnie (jawnie!) wymusza 1280×800@60 przez `ChangeDisplaySettingsW` na starcie sesji i przywraca po `TEARDOWN`. | Odświeżanie raz pokazało 40 Hz, raz 60 Hz — najpewniej Intel **DPST** zbija Hz na baterii. Wzorzec z OpenNOW: `PresentationClock` ignoruje timestampy RTP i uczy się kadencji przyjazdu klatek. | — |

---

## 5. Co już wiemy NA PEWNO (zmierzone lub wyczytane ze źródeł)

### 5.1 OpenNOW nie uruchomi się na tej maszynie — i to nie przez GUI

- `opennow-streamer-platform-windows/src/lib.rs:204-207`:
  ```rust
  pub const fn software_backend_available(&self) -> bool {
      (h264 || h265 || av1_software_decode) && self.d3d11_presentation
  }
  ```
  Nawet **programowy** dekoder wymaga D3D11. `d3d11_presentation = graphics.is_ok()`
  (`windows/mod.rs:434`).
- `windows/decoder.rs:507-509` odrzuca każdy MFT bez `MF_SA_D3D11_AWARE` (atrybut Win8+).
- Qt 6.8+ wymaga Windows 10 1809+; integracja wideo idzie przez **QRhi** i import natywnej
  tekstury do scene graph, a FFI „exposes no CPU image, encoded-frame callback, swap chain,
  window or Qt object".
- **Wniosek:** nie ma interfejsu, przez który TUI odebrałoby klatkę. Nowy klient w C jest
  jedyną drogą.

### 5.2 Zakres pracy (policzone na drzewie `main`, commit `a49b124`)

| Obszar | Linie `.rs` | Potrzebne | W C |
|---|---|---|---|
| `opennow-core` (auth, katalog, CloudMatch, settings, updater, push, analytics, Discord) | 63 735 | ~20 % | Python (już częściowo istnieje) |
| `opennow-streamer-transport` (w tym `nvst.rs` **14 695**) | 29 592 | **wszystko** | 6-9 k |
| `opennow-streamer-core` (w tym `nvst_rtsp.rs` **3 921**) | 17 235 | tak | 2,5-4 k |
| `opennow-streamer-platform` | 22 373 | ~40 % | 2-3 k |
| `opennow-streamer-platform-windows` (MF/D3D11/DXGI) | 11 452 | **NIE — do kosza** | 0 (piszemy D3D9/GDI) |
| `opennow-streamer-hid` | 5 025 | nie (tylko klawiatura+mysz) | ~300 |
| `opennow-streamer-ffi` | 4 245 | nie | własne ~400 |
| `opennow-qt` | 96 767 | **NIE** | Python TUI |
| **Razem** | **~470 k** | | **~12-18 k C + 3-5 k Pythona** |

Realny zakres: ~10-14 k linii Rusta (reszta to testy i gałęzie HEVC/AV1/HDR/mikrofon/
gamepad/nagrywanie/replay, których nie robimy) → **~8-12 k linii C**.

### 5.3 Protokół NVST — wszystko jest udokumentowane w repo

**Sygnalizacja: RTSP tunelowany przez WebSocket.** `nvst_rtsp.rs` → `RtspClient::connect`
robi `rtsp_endpoint_urls(endpoint)` → `(wss, target)`, potem `wss.into_client_request()`
(tungstenite). `rtsps://…` jest w praktyce `wss://…`, a ramki RTSP lecą jako wiadomości
tekstowe WebSocket. Potwierdzenie w CloudMatch: `metaData` zawiera `{"key":"wssignaling","value":"1"}`.

- Nagłówki: `X-GS-Version: 14.2`, `Host`, `x-nv-sessionid: <sessionId>` (`nvst_rtsp.rs:1078-1082`)
- Sekwencja: `OPTIONS` → `DESCRIBE` (`Accept: application/sdp`, `x-nv-abtesting: 2`)
  → `SETUP` (video) → `ANNOUNCE` (nasz SDP) → `PLAY` → keepalive co **2 s** → `TEARDOWN`
- `PLAY` musi zwrócić 200; **455 = failed startup**
- **Bramkowanie gotowości:** DTLS zakończony LUB uwierzytelnione media z negocjowanego peera.
  Samo ICE, wysłany pakiet czy pakiet nieuwierzytelniony **nie** wystarcza. Limit 20 s.

**SDP z `DESCRIBE`** (`nvst_rtsp.rs:1128-1150`): `general.iceUserNameFragmentV2`/`iceUsernameFragment`,
`general.icePasswordV2`/`iceUsernamePwd`, `general.dtlsFingerprintV2`/`dtlsFingerprint`,
`general.nativeRtcOnBundlePort` (**musi być `1`**, inaczej błąd `nvst-legacy-transport-unsupported`),
`general.rtcpOnSctp`, `general.enableUnifiedSocket`, `general.serverBundlePort`,
`general.rtcVideoOnNativeBundle`, `general.pingVersion` (domyślnie 6), `ri.hidDeviceMask`,
`a=control:` dla `video`.

**`ANNOUNCE`** — pełny SDP w `nvst_rtsp.rs:1520+`. Kluczowe dla nas (H.264, 8-bit 4:2:0):
```
s=NVIDIA Streaming Client
a=x-nv-video[0].clientViewportWd:1280 / clientViewportHt:720
a=x-nv-video[0].maxCodecProfile:3 (High) / maxCodecLevel:51
a=x-nv-video[0].packetSize:1280
a=x-nv-video[0].enableRtpNack:1 / rtpNackQueueLength:2048 / rtpNackQueueMaxPackets:1024 / rtpNackMaxPacketCount:64
a=x-nv-video[0].maxFPS:30
a=x-nv-video[0].initialBitrateKbps:<N> / initialPeakBitrateKbps:<N>
a=x-nv-vqos[0].bitStreamFormat:0        <- 0=H264, 1=HEVC, 2=AV1
a=x-nv-vqos[0].fec.enable:1 / repairPercent:20 / repairMinPercent:20 / repairMaxPercent:35
a=x-nv-vqos[0].dynamicStreamingMode:<0|1> / dfc.adjustResAndFps:<0|1>
a=x-nv-packetPacing.version:3 / mode:1 / numGroups:5 / maxDelayUs:2000
a=x-nv-ri.partialReliableThresholdMs:300
```
⚠️ **OpenNOW domyślnie wysyła 75 Mbps**, gdy brakuje `maxBitrateMbps`
(`docs/streamer-comparison/video.md`, „Gotchas"). Na tetheringu to katastrofa.
**Ustawiamy twardo 3-6 Mbps** dla 720p30.

**Wideo — socket „Mjolnir":**
- osobny UDP, port klienta **49005**; bundle (audio/input/RTCP) **49006**, serwer **5004**
- wideo **nie** jedzie po ICE bundle; `ANNOUNCE` ustawia `rtcVideoOnNativeBundle:0`
- każdy pakiet RTP ma **16-bajtowy nagłówek GameStream**, rozszerzenie RTP profil **`0x4753`**
  (SOF/EOF, indeks klatki, współrzędne FEC). Stałe z `nvst.rs:263-274`:
  `FLAG_CONTAINS_PIC_DATA=0x01`, `FLAG_EOF=0x02`, `FLAG_SOF=0x04`,
  `STREAM_PACKET_INDEX_MASK=0x00ff_ffff`, `GS_VIDEO_EXTENSION_PROFILE=0x4753`,
  `GFN_EXTENDED_FRAME_HEADER_BYTES=20`
- SRTP: `SRTP_AES_CM_128_HMAC_SHA1_80` (tag 10 B), AEAD `AES_GCM` (16 B) i `AES_GCM_8` (8 B)
  (`nvst.rs:81-86`). Etykiety kluczy: `GFN_SRTP_KEY_LABEL=0x00`, `SALT=0x02`,
  `SRTCP_KEY=0x03`, `SRTCP_SALT=0x05` (`nvst.rs:90-94`). Krypto **ręcznie** na
  aes/ctr/hmac/sha1/ghash — w C to samo na OpenSSL/mbedTLS, **bez libsrtp**
- reorder **1024** pakietów (max 2048), czekanie na lukę **52 ms**, bufor gniazda 8 MiB
  (→ **na tej maszynie zmniejsz do 1-2 MiB**)
- FEC: **Reed-Solomon** NVIDIA, naprawiany **przed** składaniem access unit
- NACK-v2: komenda ServerControl **`0x0317`** na `control_channel_partially_reliable`,
  format w `docs/mjolnir-nack-v2.md` (u8 version=2, u8 ordinal, u8 count,
  potem `count × (u16LE base + u64LE bitmap64)`). Limit 64 pakiety/komenda, 2048 oczekujących,
  3 próby, odstęp `RTT + 4 ms`, timeout 52 ms, max 647 B
- kluczowa klatka H.264 = NAL type **5**

**Audio:** Opus RTP po bundle, **PT 111**, RED **PT 63**, zegar **48 kHz**. Jitter buffer:
cel od **45 ms** i rośnie; kolejność reorder → RED recovery → decode; concealment max **100 ms**.
`MAX_OPUS_PACKET_BYTES=1275`, `MAX_REDUNDANT_AUDIO_BLOCKS=8`. Wyjście: **WASAPI** 48 kHz stereo
(OpenNOW przez SDL2 — my idziemy bezpośrednio w WASAPI).

**Wejście — kanały SCTP** (`nvst_input.rs:14-20`):
```
control_channel_reliable
custom_channel_reliable / custom_channel_partially_reliable
control_channel_partially_reliable   <- mysz relatywna, NACK-v2
control_channel_unreliable
input_channel_partially_reliable     <- wejście
cursor_channel
```
Ramki kontrolne **bajt po bajcie** w `docs/streamer-comparison/features.md`. Sekwencja aktywacji:

| # | Komenda | Payload |
|---|---|---|
| 1 | `0x020b` enable input false | `0b 02 0c 00 00 00 00 00 01 00 00 00 00 00 00 00` |
| 2 | `0x020d` device descriptor index 2 | 64 B (pełny hex w dokumencie) |
| 3 | `0x0308` cursor capture on | `08 03 01 00 01` |
| 4 | `0x030d` track cursor on | `0d 03 01 00 01` |
| 5 | `0x0320` window state **19** | `20 03 0c 00 00 00 00 00 13 00 00 00 00 00 00 00` |
| 6 | `0x0321` system state 0 | `21 03 0c 00 00 00 00 00 00 00 00 00 00 00 00 00` |
| 7 | `0x020b` enable input true | `0b 02 0c 00 00 00 00 00 01 00 00 00 01 00 00 00` |

- Nagłówek ServerControl = `u16LE` kod + `u16LE` długość. `0x0308` na drucie to `08 03`
  — **little-endian, nie czytaj jako big-endian**
- **Window state 19 jest wymagane**; zera sprawiają, że sesja wygląda na nieaktywną
- Keepalive: `0x0200`. Mysz relatywna: `0x0206` przez `control_channel_partially_reliable`
- NVB type 10 (`accel=0, speed=10`): **dokładne bajty nieznane**, OpenNOW rekonstruuje
  `0x0323`. Akceptujemy jako `UNVERIFIED`
- Przechwytywanie: Raw Input (`RIDEV_INPUTSINK`, wątek ABOVE_NORMAL) + okno wiadomości —
  wzorzec z `opennow-streamer-platform/src/windows_raw_input.rs` jest w 100 % przenośny na C

**CloudMatch i kolejka (to ląduje w Pythonie):**
- baza domyślna `https://prod.cloudmatchbeta.nvidiagrid.net/` (`cloudmatch.rs:18`)
- `GET {base}v2/serverInfo` → `requestStatus.serverId` = **vpcId** + `regional_bases`
- `POST {base}v2/session` — ciało z `build_create_body` (`cloudmatch.rs:1643-1760`):
  `appId`, `internalTitle`, `availableSupportedControllers:[2]`, `preferredController:2`,
  `clientIdentification:"GFN-PC"`, `deviceHashId`, `clientVersion:"30.0"`, `sdkVersion:"2.0"`,
  `streamerVersion:"14"`, `clientPlatformName`, `clientRequestMonitorSettings:[{monitorId,
  positionX/Y, widthInPixels, heightInPixels, framesPerSecond, sdrHdrMode, displayData, dpi:96}]`,
  `useOps:true`, `audioMode:2`, `metaData:[wssignaling=1, SubSessionId=<uuid>, surroundAudioInfo=2]`,
  `secureRTSPSupported:true`, `accountLinked`, `enablePersistingInGameSettings`,
  `requestedAudioFormat:0`, `userAge:25`, `clientTimezoneOffset`,
  `requestedStreamingFeatures:{reflex, bitDepth, cloudGsync, enabledL4S, mouseMovementFlags:0,
  trueHdr:false, supportedHidDevices:0, profile:0, fallbackToLogicalResolution:false,
  hidDevices:null, chromaFormat, prefilterMode/Sharpness/NoiseReduction:0, hudStreamingMode:0,
  dlssOverrides}`
- **KODEK NIE JEST WYSYŁANY DO CLOUDMATCH** (`cloudmatch.rs:1646-1649`: „The codec stays
  client-selected… never sent to CloudMatch"). Wybieramy go lokalnie i ogłaszamy w `ANNOUNCE`
  (`x-nv-vqos[0].bitStreamFormat`). **Backend nie pyta, czy mamy dekoder sprzętowy.**
  Jedyne twarde ograniczenia: H.264 = 8-bit 4:2:0 + pixel budget.
- `GET v2/session` = discovery aktywnych sesji; `DELETE v2/session/{id}` = sprzątanie
- **Kolejka** (`cloudmatch.rs:2655-2663`): `queuePosition` z czterech miejsc, pierwsze `> 0`:
  `queuePosition`, `seatSetupInfo.queuePosition`, `sessionProgress.queuePosition`,
  `progressInfo.queuePosition`
- **Statusy sesji** (`cloudmatch.rs:2698-2708`): `1=preparing, 2=ready, 3=streaming,
  4|5=paused, 6=resuming, 7=finished, >3=failed`
- **Postęp setupu** (`docs/session-start-progress.md`): `seatSetupInfo.seatSetupStep` →
  `0` łączenie, `1` oczekiwanie na rig + pozycja w kolejce, `2/3/4` konfiguracja riga,
  `5` sprzątanie poprzedniej sesji, `6` oczekiwanie na storage. **Kroki nie są liniowe**
  i nie da się z nich policzyć procentów.
- Autoryzacja: **`GFNJWT <id_token>`** (nie Bearer) dla `serverInfo`/GraphQL/CloudMatch;
  `Bearer access_token` **tylko** dla `login.nvidia.com/userinfo`. Nagłówki LCARS:
  `nv-client-id: ec7e38d4-03af-4b58-b131-cfb0495903ab`, `nv-client-type: NATIVE`,
  `nv-client-version: 2.0.87.131`, `nv-client-streamer: NVIDIA-CLASSIC`,
  `nv-device-os: WINDOWS`, `nv-device-type: DESKTOP`, `nv-device-make: GENERIC`,
  `nv-device-model: PC`, `x-nv-client-identity: GFN-PC`

**Logowanie (już zaimplementowane po stronie Pythona, potwierdzone na żywo 2026-10-17):**
OAuth 2.0 **Device Authorization Grant (RFC 8628)** — `POST login.nvidia.com/device/authorize`,
potem polling `POST login.nvidia.com/token` z `grant_type=urn:ietf:params:oauth:grant-type:device_code`.
**Brak redirect_uri, brak lokalnego serwera HTTP, PKCE nieużywane** (moduł `pkce.py` istnieje
i jest przetestowany, ale device grant go nie wymaga). Obsługa błędów pollingu:
`authorization_pending` = normalny stan, `slow_down` = +5 s do interwału (limit 3600 s),
`expired_token`/`access_denied` = koniec, **błąd sieci = NIE kończymy próby** (tethering gubi pakiety).
`client_token`: osobny grant na `login.nvidia.com/client_token`, jego brak jest **niekrytyczny**.

### 5.4 Budżet CPU (zmierzony, T1)

| Tryb | Koszt zmierzony | Z mnożnikiem 1,8× (treść growa) | Status |
|---|---|---|---|
| **1280×720 @30, decode 1t** | **24,8 %** obu rdzeni (16,5 ms/klatkę) | ~45 % | **PLAN A** |
| 1280×720 @30, z NV12 | 28,9 % | ~52 % | plan A z konwersją |
| 1280×720 @60, decode 1t | 46,0 % | ~83 % | opcja, nie domyślna |
| 960×540 @60, decode 1t | 28,7 % | ~52 % | **PLAN B** |
| 960×540 @30 | 15,8 % | ~28 % | bezpieczne minimum |
| 640×360 @30 | 7,8 % | ~14 % | awaryjny |
| ~~1920×1088 @30~~ | 54,6 % | ~98 % | **odrzucone** (ekran 1280×800) |

Operacje dodatkowe (720p30, ms CPU/klatkę ponad `decode_only_1t` = 16,52):
`to_nv12` +2,7 (niemal darmo) · `to_rgb24` +5,6 · `to_yuy2` +12,1 · **`scale=1280:800` +11,8**

Pamięć: ~1,25 GB wolnego z 2 GB.

### 5.5 Konsola (zmierzone, T3/T5)

| Pomiar | ConEmu | czysty `cmd.exe` |
|---|---|---|
| ANSI 16 kolorów | ✅ atrybuty zastosowane | ❌ `ESC` jako znak `←` |
| ANSI 256 / truecolor | ✅ sekwencje parsowane, ale **kwantyzowane do 16 kolorów** (`0xC`, `0x9`) | ❌ |
| alt-screen + adresowanie kursora | ✅ | ❌ |
| `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` | ❌ `ERROR_INVALID_PARAMETER (87)` | ❌ to samo |
| `ConEmuHk64.dll` w procesie | **TAK** | nie |
| zmienne `CONEMU*` | 23 szt., `CONEMUANSI=ON`, `CONEMUHOOKS=Enabled` | zero |
| `CreateConsoleScreenBuffer` + `SetConsoleActiveScreenBuffer` | ✅ (działa też w czystym conhost) | ✅ |
| `WriteConsoleOutputW` pełne 80×25 | **0,1 ms** | — |
| `ReadConsoleInputW` mysz | 146 zdarzeń w 20 s (ruch `flags=1`, LPM `buttons=1`) | — |
| `ReadConsoleInputW` klawiatura | 25 zdarzeń, VK 13/37-40/65/90/112 | — |
| Unicode round-trip (`─│┌┐└┘ █░▒▓ ▶✓`) | **identyczny**, 0 × `?` | — |
| `GetNumberOfConsoleMouseButtons()` | **16** (conhost zwraca 2 — to sygnatura hooka) | 2 |
| kodowania | CP **852** in/out, `stdout_encoding=utf-8` | to samo |
| okno | 105 × 28 komórek, bufor 105 × 32766 | — |

**Niepotwierdzone:** mysz SGR-1006 (`looks_like_sgr=false` w obu hostach — test był
niemiarodajny, `msvcrt.getwch` nie czyta poprawnie sekwencji wielobajtowych). Fallback:
nawigacja klawiaturą (VK kompletne).

**Ostrzeżenie o foncie:** `HKCU\Console` ma `FaceName = null` dla profilu domyślnego =
font rastrowy **bez glifów ramek Unicode**. Bufor przyjmuje kody poprawnie, ale na ekranie
mogą być prostokąty. → przełącznik `--ascii-boxes` o tym samym layoucie.

### 5.6 Toolchain (zmierzone, T2)

| Wariant | `_WIN32_WINNT` | PE subsys | Uruchamia się na Win7 |
|---|---|---|---|
| x64 `default` | **0x0A00 (Win10!)** | 5.2 | ✅ |
| x64 `win7` | 0x0601 | 5.2 | ✅ |
| x64 `win7_core2` | 0x0601, **sse3=1 ssse3=1** | 5.2 | ✅ |
| x64 `win7_subsys601` | 0x0601 | **6.1** | ✅ |
| x86 `default` | **0x0501 (WinXP!)** | **4.0** | ✅ |
| x86 `win7` | 0x0601 | 4.0 | ✅ |

15/15 wariantów `run_rc=0`, `runtime_os=6.1.7601`, `d3d9_create=ok`, `8086:2A42`,
`wsastartup=0`, `hard_blockers_present=[]`, `unknown_dlls=[]`.
Importy: wyłącznie `d3d9.dll, KERNEL32.dll, msvcrt.dll, USER32.dll, WS2_32.dll`.

**Domyślnie `sse3=0 ssse3=0`** — `-march=core2 -msse3` trzeba wymusić jawnie,
**i to dla każdej vendored biblioteki** (inaczej ich pętle zostaną na SSE2/C; strata 20-40 %).

---

## 6. Ustalenia techniczne (nie decyzje — fakty)

### F1 — kanoniczny zestaw flag

```make
CFLAGS  := -std=c11 -O2 -Wall -Wextra \
           -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000 \
           -march=core2 -msse3
CXXFLAGS:= $(CFLAGS) -fno-strict-aliasing
LDFLAGS := -static -static-libgcc -static-libstdc++ \
           -Wl,--major-subsystem-version,6.01 -Wl,--minor-subsystem-version,01 \
           -Wl,--major-os-version,6.01 -Wl,--minor-os-version,01
```

**ZAKAZ:** `-march=native` na jakiejkolwiek maszynie innej niż E5500.
**Uwaga:** `-march` nie wpływa na pliki `.asm` składane przez NASM/yasm — one mają
własny dispatch w runtime.

### F2 — stos bibliotek w C

| Zadanie | Biblioteka | Licencja | Uwagi |
|---|---|---|---|
| TLS + WSS | mbedTLS 3.6 LTS (albo OpenSSL 3.x) + **własny klient WebSocket** (~500 linii) | Apache-2.0 | RTSP-over-WSS jest trywialny: handshake + maskowanie + ramki |
| DTLS (bundle) | mbedTLS DTLS (`MBEDTLS_SSL_DTLS_SRTP`) | Apache-2.0 | potrzebny eksport kluczy SRTP |
| ICE/STUN | **własna implementacja** (~300 linii) albo libjuice (MPL-2.0) | MIT/MPL | OpenNOW i tak robi STUN binding ręcznie (`nvst.rs:248-259`: magic cookie, USERNAME, PRIORITY, MESSAGE-INTEGRITY, XOR-MAPPED-ADDRESS, FINGERPRINT). **Rekomenduję własną** — zero zależności |
| SCTP | **usrsctp** (BSD-3) albo libdatachannel (MPL-2.0, C++17 z C API) | BSD-3 | **najtrudniejszy element portu** (ryzyko R3). Zrobić spike „hello DTLS+SCTP" zanim cokolwiek innego |
| SRTP/SRTCP | **własna** na AES-CTR + HMAC-SHA1 + AES-GCM | MIT | jak w OpenNOW; okno replay `SRTP_REPLAY_WINDOW_PACKETS=2048` |
| RS-FEC | własna tablicowa GF(2^8) (~400 linii) | MIT | można odłożyć na etap 5, ale na tetheringu będzie potrzebny |
| Dekoder H.264 | **FFmpeg `libavcodec`** (D3) | **GPL** | `threads=1` (D4), bez `-hwaccel` (D5) |
| Audio Opus | libopus | BSD-3 | ~1-2 % CPU |
| Wyjście audio | **WASAPI** shared, event-driven | — | bez SDL |
| Prezentacja | **D3D9** → łańcuch D7 | — | NV12/YUY2/X8R8G8B8 `StretchRect`, fallback GDI |
| Wejście | Raw Input (`RIDEV_INPUTSINK`) | — | wzorzec z `windows_raw_input.rs` |
| JSON w C | **własny ograniczony parser** (~600 linii) | MIT | limity z Twojego ABI: `json_nodes:256`, `json_depth:32`, `json_string_bytes:4096` |
| TUI | Textual + Rich (Python 3.8, cp38) | MIT | **pinowane wersje** + lokalne wheele (R15) |

### F3 — gdzie w OpenNOW szukać wiedzy (kolejność czytania)

1. `docs/streamer-comparison/video.md` — cały tor wideo + zachowanie oficjalnego klienta
2. `docs/streamer-comparison/features.md` — **bajtowe ramki NVB** i sekwencja aktywacji
3. `docs/mjolnir-nack-v2.md` — format NACK-v2 `0x0317`
4. `docs/streamer-comparison/audio.md` — Opus/RED/jitter/WASAPI
5. `docs/streamer-comparison/mouse-input.md` + `controller-input.md`
6. `docs/streamer-comparison/rtsp-negotiation.md` — kolejność i bramkowanie PLAY
7. `docs/session-start-progress.md` — `seatSetupStep` (ekran kolejki w TUI)
8. `docs/core-protocol.md` — wzorzec IPC JSON-lines
9. `native/opennow-streamer/crates/opennow-streamer-transport/src/nvst.rs` — SRTP/GS/FEC/reorder
10. `native/opennow-streamer/crates/opennow-streamer-core/src/nvst_rtsp.rs` — RTSP + ANNOUNCE
11. `native/opennow-core/src/cloudmatch.rs:1643` — `build_create_body`
12. `native/opennow-streamer/crates/opennow-streamer-platform/src/windows_raw_input.rs` — Raw Input

Uwaga: `docs/streamer-comparison/README.md` obiecuje `video.md` i on **istnieje**
(w przeciwieństwie do listy katalogu w jednej z wcześniejszych wersji).

---

## 7. Ryzyka — aktualny rejestr

| # | Ryzyko | P | S | Stan / mitigacja |
|---|---|---|---|---|
| R1 | CPU nie wyrabia | — | — | ✅ **ZAMKNIĘTE** (T1: 24,8 % dla 720p30) |
| R2 | D3D9 NV12 odrzucony/crash | **wysokie** | **krytyczne** | 🟡 **częściowo zmierzone (runda VI):** `UpdateSurface` na NV12 **crashuje** (`0xC0000005`, x64 i x86) — ta ścieżka jest **martwa**. Wszystkie etapy `stretch` i `bench` przeszły bez crasha, ale ich **dane przepadły** przez błąd zapisu JSON (v3). T4 v4 mierzy ponownie. Mitigacja = D7 |
| R2b *(nowe)* | Warstwa zapisu diagnostyki gubi dane | **pewne (2×)** | wysokie | v1: buforowanie do końca → crash zabrał wszystko. v3: `fseek`/`ftell` po własnym wyjściu → **pliki 0-bajtowe**. v4: akumulator w RAM + przepisanie całości, **bez `fseek`**, plus `tools_test_json_layer.sh` testujący samą warstwę na Linuxie. **Zasada dla klienta: §7.12 `BOOTSTRAP.md`** |
| R3 | usrsctp/DTLS na mingw+Win7 | średnie | wysokie | ⏳ Etap 0: osobny spike „hello DTLS+SCTP" przed jakimkolwiek kodem NVST. Plan B: libdatachannel |
| R4 | brak VT w conhost | — | — | ✅ **ZAMKNIĘTE** (T5 + D1) |
| R5 | Python 3.8 EOL | pewne | średnie | Tokeny przez **DPAPI** (`CryptProtectData` z `ctypes`), nie plaintext |
| R6 | binarka GCC 16 nie wystartuje na Win7 | — | — | ✅ **ZAMKNIĘTE** (T2: 15/15) |
| R7 | CloudMatch odrzuci klienta bez HW dekodera | **niskie** | krytyczne | Kodek nie jest wysyłany do CloudMatch (§5.3). Bramka: pierwszy `POST v2/session` |
| R8 | seat wymaga `nativeRtcOnBundlePort != 1` | niskie | średnie | Powtarzamy błąd `nvst-legacy-transport-unsupported` zamiast wisieć |
| R9 | **Intel DPST zbija odświeżanie w trakcie sesji** | wysokie | średnie | D9: mierzymy interwał `Present`. T4 v3 ma `EnumDisplaySettingsW` + listę trybów 1280×800. **Po stronie użytkownika: wyłączyć DPST dla trybu DC w panelu Intela** |
| R10 | NVB type 10 — bajty nieznane | pewne | niskie | Rekonstruujemy `0x0323`, oznaczamy `UNVERIFIED` (OpenNOW robi tak samo) |
| R11 | OOM przy 2 GB RAM | średnie | wysokie | Bufor gniazda **1-2 MiB** zamiast 8 MiB; brak `ffmpeg.exe` jako procesu (tylko `libavcodec` w procesie); pomiar `WorkingSet` |
| R12 | GPL wciąga cały projekt | pewne | niskie | Zaakceptowane (D3), projekt prywatny |
| R13 | jitter/straty na tetheringu | wysokie | średnie | FEC RS + NACK-v2 (3 próby, 52 ms); `packetSize=1280` (nie 1500) |
| R14 | vendored biblioteki bez `-march=core2 -msse3` | średnie | wysokie | F1: jeden kanoniczny zestaw flag dla **wszystkiego**, weryfikowany przez `-dM` w buildzie |
| R15 | zależności Pythona (brak rich/textual, cp38, EOL) | średnie | średnie | Pinowany `requirements.txt` + lokalny katalog wheeli + test instalacji offline |
| R16 | font rastrowy konsoli bez glifów Unicode | wysokie | niskie | Przełącznik `--ascii-boxes`, ten sam layout |
| R17 | **skalowanie na CPU = +18 pkt proc.** | wysokie | wysokie | D6: skalowanie na GPU, albo prezentacja 1:1 (letterbox) |
| R18 | mysz SGR w ConEmu | — | — | ✅ **ZAMKNIĘTE PRZEZ DECYZJĘ D10** — TUI bez myszy, więc nieistotne. T7 zmierzył 0 zdarzeń (prawdopodobnie ConEmu konsumuje mysz dla własnego UI albo test leciał bez interakcji), ale to już nie ma znaczenia |
| R20 *(nowe)* | resize TUI nieprzychodzący jako zdarzenie | pewne | niskie | D11: polling `GetConsoleScreenBufferInfo` co tick |
| R21 | błędy sygnatur/nazw D3D9 w `.c` (sandbox bez kompilatora) | ~~wysokie~~ **niskie** | średnie | ✅ **ZMITIGOWANE (runda V):** sandbox ma `apt` → `apt-get install -y gcc-mingw-w64-x86-64` daje prawdziwy kompilator. `tools_verify_c.sh` kompiluje z `-Wall -Wextra`. **Zasada: żaden plik C nie wychodzi z sesji bez rc=0.** Dodatkowo `tools_check_d3d_calls.py` porównuje liczbę argumentów makr z `d3d9.h`. Koszt: trzy rundy zmarnowanych uruchomień użytkownika |
| R22 *(nowe)* | **`SessionInfo` zbyt ubogie dla klienta C** — 8 pól zamiast 31 z `session_info()` | **pewne** | **krytyczne dla etapu 2** | `analiza/05` §3: brakuje `mediaConnectionInfo`, `signalingUrl` (wss://), `iceServers`, `rtspsEndpoints[]`, `connectionInfo`, `streamingBaseUrl`, `finalizedStreamingFeatures`, `seatSetupStep`. Rekomendacja: skopiować kontrakt `session_info()` 1:1 jako `native_context` |
| R23 *(nowe)* | brak `RESUME` i sprzątania osieroconych sesji | pewne | średnie | `analiza/05` §4: `{"action":2,"data":"RESUME"}` (`cloudmatch.rs:1540-1598`) + `pending-session-cleanup.json` (`cloudmatch.rs:580-611`). Przy tetheringu każde zerwanie = nowa kolejka. **Etap 5, pozycja obowiązkowa** |
| R24 *(nowe)* | paginacja katalogu bez wiązania kursora z rewizją | średnie | średnie | `catalog.rs:404-408`: hash kontekstu z `[user_id, provider, generation, graphql, vpcId, cache_scope, revision, "en_US"]`; przy niezgodności błąd `catalog_changed`. Bez tego — cicha duplikacja/dziury w liście gier |
| R19 | truecolor kwantyzowany do 16 kolorów | pewne | niskie | `color_system="standard"` jawnie; UI bez subtelnych odcieni |

---

## 8. Struktura plików w workspace

```
/home/user/
├── HANDOFF.md                      ← TEN PLIK (punkt wejścia)
├── analiza/
│   ├── 00-WERDYKT.md               runda I: wykonalność, mapa NVST, budżet CPU (SZACUNEK — patrz 03)
│   ├── 01-WYNIKI-TESTOW.md         runda II: T2/T3 + poprawki T1/T4 + decyzja o 60 Hz
│   ├── 02-DECYZJE.md               rejestr decyzji D1-D3 + fakty F1-F6
│   ├── 03-WYNIKI-RUNDA3.md         runda III: T1 (CPU, POMIAR), T5 (host), crash T4, D2 zrewidowana
│   ├── 04-NVST-SPEC-BINARIA.md     ★ SPECYFIKACJA BAJTOWA NVST dla implementatora w C
│   │                                 (RTP+GS 0x4753, SRTP KDF/IV, FEC Cauchy GF(2^8),
│   │                                  NACK-v2, ramki NVB) + 20 pułapek + indeks linii
│   └── 05-PRZEGLAD-KODU-LOGOWANIA.md ★ PRZEGLĄD kodu użytkownika (D12): 32/32 twierdzeń
│                                     zweryfikowanych, 2 luki (SessionInfo, RESUME),
│                                     4 nowe fakty o paginacji katalogu, 10 rekomendacji
├── tests/
│   ├── README.md                   instrukcja wszystkich testów (z aktualizacją Rundy II)
│   ├── T1_decode_bench.py          ✅ uruchomiony — benchmark dekodowania ffmpeg
│   ├── T2_toolchain_win7.py        ✅ uruchomiony — mingw/Win7, parse PE
│   ├── T3_console_diag.py          ✅ uruchomiony — diagnostyka konsoli
│   ├── T4_d3d9_present.c           ⏳ v3 — do ponownego uruchomienia
│   ├── T4_d3d9_present.py          ⏳ v3 harness z izolacją etapów
│   ├── T5_host_detect.py           ✅ uruchomiony ×2 — ConEmu vs cmd (rozstrzygajacy)
│   └── T7_mouse_sgr.py             ✅ uruchomiony — mysz SGR/resize (0 zdarzen; patrz D10)
├── tools_check_d3d_calls.py        weryfikator liczby argumentow makr IDirect3D* vs d3d9.h
├── tools_verify_c.sh               ★ PRAWDZIWA kompilacja mingw w sandboxie (patrz §6 F7)
├── uploads/                        wyniki przesłane przez użytkownika
│   ├── T1_skrot.txt  T1_wynik.json (ucięty)  T2_wynik.json
│   ├── T3_wynik.json  T4_wynik.json (crash v1)  1_T4_wynik.json (crash v1)
│   └── T5_wynik_1_Con_Emu.json  T5_wynik_2_CMD.json
└── src/
    ├── opennow/                    PEŁNE źródła OpenNOW main (commit a49b124)
    └── hdr_d3d9.h hdr_d3d9types.h hdr_d3d9caps.h hdr_dxva2api.h
                                    prawdziwe nagłówki mingw-w64 (do weryfikacji nazw)
```

Repo użytkownika (poza sandboxem): `C:\Users\KarnyJohnny\Desktop\NewKarnyNow`
(wcześniej `Desktop\Projekty\KarnyNow`; 204 MB, 3680 plików źródłowych).
Testy uruchamia z `<repo>\tests\`.

**UWAGA: sandbox nie ma `curl`, `make`, `gcc`, `cmake`.** Ma `python3` (3.11) i `git`
oraz sieć przez `urllib`/`fetch_page`. Źródła OpenNOW pobrane przez
`codeload.github.com/.../tar.gz/refs/heads/main` (15 MiB, 2 s).

---

## 9. Plan etapowy i stan

| Etap | Zakres | Bramka | Stan |
|---|---|---|---|
| **0** | Pomiary (T1-T5, T7) | G0.1 CPU ✅, **G0.2 D3D9 ⏳**, G0.3 toolchain ✅, G0.4 konsola ✅ | **brakuje wyłącznie uruchomienia T4 v4** |
| **1** | Szkielet C offline: `knvstd --loopback` czyta `.264`, dekoduje (ffmpeg, threads=1), prezentuje w oknie D3D9 1280×800 z letterboxem, liczy fps/latencję/CPU/RSS. Plus WASAPI z syntetycznym PCM. | G1: stabilne 30 fps przez 10 min, CPU ≤ 45 %, brak wzrostu RSS | nie zaczęty |
| **2** | Protokół bez DTLS/SCTP: WSS+RTSP, Mjolnir, SRTP, GS header, AU assembly. `--capture` zrzuca AU do `.264` | G2: `OPTIONS→…→PLAY` = 200; AU składają się poprawnie (`ffprobe` na zrzuconym pliku); reorder/NACK-v2 działa przy sztucznie gubionych pakietach | nie zaczęty |
| **3** | Bundle: ICE + DTLS + SCTP + Opus + wejście. **Zacząć od spike'a „DTLS handshake + jeden kanał SCTP + echo"** | G3: dźwięk słyszalny; sekwencja `0x020b/0x020d/0x0308/0x030d/0x0320(19)/0x0321` przyjęta; kursor rusza się w chmurze | nie zaczęty |
| **4** | Python TUI: backend Textual/Rich, detekcja hosta ConEmu, ekran konta, lista gier (2000 pozycji w pamięci), ekran kolejki (`queuePosition` + `seatSetupStep`), launcher, vault DPAPI, logi 2 MiB × 3 | G4: login → katalog → launch → kolejka → *streaming* → gra reaguje → TEARDOWN → powrót do listy, **trzy razy z rzędu bez restartu** | nie zaczęty |
| **5** | Utwardzanie: RS-FEC, recovery (IDR `0x0301`, RFI `0x0302`, invalidation), rekonect, `dynamicStreamingMode`, bitrate cap 3-6 Mbps, prawdziwe statystyki w TUI (nie placeholdery — OpenNOW sam przyznaje, że LATENCY/PING/LOSS% są atrapami) | — | nie zaczęty |

---

## 10. NASTĘPNY KROK (co zrobić od razu po przejęciu)

1. **Poprosić użytkownika o uruchomienie T4 v4** (v3 nie skompilował się — runda IV;
   v3.1 skompilował się i uruchomił, ale warstwa zapisu JSON zgubiła dane — runda VI) i przesłanie `T4_wynik.json` **oraz** `T4_skrot.txt`:
   ```bat
   cd /d "%USERPROFILE%\Desktop\NewKarnyNow\tests"
   python T4_d3d9_present.py
   ```
   Harness uruchamia 14 osobnych procesów; jeśli któryś zcrashuje, dowiemy się który.
   Rozstrzyga bramkę **G0.2** i wybór ścieżki prezentacji (D7).
   **T4 v3.1 jest zweryfikowany statycznie**: `tools_check_d3d_calls.py` potwierdza, że
   wszystkie **71 wywołań makr `IDirect3D*`** ma zgodną z `d3d9.h` liczbę argumentów,
   a jedynym identyfikatorem spoza nagłówków jest `D3DFMT_NV12` (mamy własny `#ifndef`).
   Przed każdą kolejną edycją `.c` **uruchom ten weryfikator** — to jedyny sposób, żeby
   nie zgadywać sygnatur bez kompilatora w sandboxie:
   ```
   python3 tools_check_d3d_calls.py
   ```
2. **Gdy użytkownik przyśle kod logowania (D12)** — przejrzeć i udokumentować:
   uzgodnić `GFNJWT` vs `Bearer`, pochodzenie `vpcId`, obsługę `slow_down`/błędów sieci
   w pollingu, sposób przechowywania tokenów (sprawdzić, czy nie plaintext → zaproponować DPAPI).
3. **Uzupełnić `analiza/03-WYNIKI-RUNDA3.md`** o wyniki T4 v3.1 i zamknąć albo
   przekwalifikować R2/R17.
4. **Dopiero potem** wchodzić w etap 1 (szkielet C), bo bez ścieżki prezentacji nie da się
   napisać pętli wideo.
5. Równolegle, niezależnie od T4, można zacząć:
   - `requirements.txt` z pinowanymi wersjami cp38 (Textual/Rich/httpx/httpcore/h11/certifi/idna/
     sniffio/anyio/markdown-it-py/mdurl/pygments) + plan lokalnych wheeli (R15)
   - moduł detekcji hosta konsoli (kod w `analiza/03-WYNIKI-RUNDA3.md` §2.3)
   - poprawkę ABI: `thread_count` dekodera **1**, nie 2 (D4)

**Zasada pracy z użytkownikiem:** nie piszemy kodu produkcyjnego przed zamknięciem bramki G0.2.
Użytkownik uruchamia testy u siebie i odsyła pliki wynikowe. Jeśli plik jest duży,
zawsze generujemy też mały `*_skrot.txt`.

---

## 11. Czego NIE wiemy (uczciwa lista)

| Pytanie | Jak rozstrzygniemy |
|---|---|
| Czy D3D9 przyjmie NV12/YUY2 `StretchRect` ze skalowaniem na `igdumd64.dll` 8.15.10.2869 | **T4 v3** (najbliższy krok) |
| Ile ms kosztuje `StretchRect` + `Present` i czy mieści się w 16,6/33,3 ms | T4 v3 (`bench_*`) |
| Czy 60 Hz jest osiągalne dla 1280×800 i czy DPST je zbija | T4 v3 (`EnumDisplaySettingsW` + `modes_1280x800`) |
| Jaki jest `pitch` powierzchni NV12 (dekoder musi go znać) | T4 v3 (`surface_descs`) |
| ~~Czy mysz SGR-1006 działa w ConEmu~~ | ➖ **nieistotne** — D10: TUI bez myszy |
| Czy CloudMatch przydzieli sesję naszemu klientowi | pierwszy prawdziwy `POST v2/session` (etap 2) |
| Czy DTLS/SCTP przejdzie przez tethering | etap 3, po spike'u |
| Realna latencja end-to-end | etap 3; proxy = timestamp serwera vs `Present` (glass-to-glass wymaga kamery) |
| Dokładny format RS-FEC NVIDIA (macierz, rozmieszczenie pakietów naprawczych) | lektura ~600 linii `nvst.rs` w etapie 5 |
| Dokładne bajty NVB type 10 | **nieosiągalne** — OpenNOW też ich nie ma; używamy `0x0323` jako `UNVERIFIED` |

---

## 12. Dziennik sesji

### 2026-10-20 · runda I
Pobrano i przeanalizowano pełne źródła OpenNOW `main` (commit `a49b124`, 15 MiB).
Ustalono: OpenNOW nie uruchomi się na tym sprzęcie (D3D11 wymagany nawet dla dekodera
programowego; Qt6 = Win10+; FFI nie wystawia obrazu CPU). Zmapowano protokół NVST
(RTSP-over-WSS, Mjolnir SRTP + GS 0x4753, RS-FEC, NACK-v2 0x0317, kanały SCTP, Opus PT111/RED PT63,
bajtowe ramki NVB). Wyceniono zakres (~10-14 k linii Rusta → ~8-12 k C).
Policzono budżet CPU **szacunkowo** (1,5-2,5 cy/px) → 720p30 = 41-69 %.
Napisano testy T1-T4. Zarejestrowano ryzyka R1-R13.
→ `analiza/00-WERDYKT.md`

### 2026-10-20 · runda II
Wyniki T2 i T3 z maszyny. **T2 zaliczony**: 15/15 binarek działa na Win7; odkryto dwie pułapki
(domyślny `_WIN32_WINNT` = Win10 na x64 / WinXP na x86; brak SSE3/SSSE3 domyślnie).
**T3**: VT działa, ale `SetConsoleMode(VT)` = `ERROR_INVALID_PARAMETER` → hipoteza ConEmu.
T1 ucięty (JSON ~150 KB), T4 nie skompilowany (złe nazwy pól/makra D3D9).
Użytkownik poprawił odświeżanie 40 → 60 Hz (DPST). Zweryfikowano wszystkie nazwy D3D9
w prawdziwych nagłówkach mingw-w64, przepisano T4, dodano T5.
Podjęto decyzje D1 (ConEmu), D2 (rozdzielczość po pomiarze), D3 (dekoder po pomiarze).
→ `analiza/01-WYNIKI-TESTOW.md`, `analiza/02-DECYZJE.md`

### 2026-10-20 · runda III
**T1 zaliczony**: 720p30 = 24,8 % obu rdzeni (mój szacunek był ~3,5× zbyt pesymistyczny);
`threads=1` lepsze niż 2; `-hwaccel dxva2` daje 3-4× gorszy wynik przy `rc=0` (pułapka);
skalowanie na CPU = +11,8 ms/klatkę (najdroższa operacja w torze). **D2 zrewidowana** na 720p30.
**T5 rozstrzygnął D1**: w ConEmu załadowany `ConEmuHk64.dll` i ANSI działa (kwantyzowane do
16 kolorów); w czystym `cmd.exe` hooka brak, a `ESC` trafia do bufora jako znak `←`.
**T4 v1 zcrashował** (0xC0000005 na x64 i x86) bez śladu → przepisany na v3 z izolacją
etapów, przyrostowym zapisem i `CheckDeviceFormatConversion` przed każdym `StretchRect`.
**D3 rozstrzygnięta przez użytkownika**: openh264/T6 porzucone, FFmpeg zostaje.
Nowe decyzje D4-D9. Nowe ryzyka R17-R19. Usunięto pliki `T6_*`.
→ `analiza/03-WYNIKI-RUNDA3.md`

### 2026-10-20 · runda IV
**T7**: 0 zdarzeń (mysz SGR i resize nie dotarły do aplikacji). **Bez znaczenia** — użytkownik
zadeklarował, że nie używa myszy w TUI → **D10: TUI wyłącznie klawiaturą**, `Textual(mouse=False)`,
R18 zamknięte decyzją. Nowe **D11**: resize przez polling `GetConsoleScreenBufferInfo`,
bo `resize_reporting_works=false`.
**T4 v3 nie skompilował się** — trzy błędy, wszystkie po stronie asystenta (zgadywanie sygnatur
zamiast sprawdzenia w nagłówku):
1. `IDirect3DDevice9_CreateOffscreenPlainSurface` dostał **8** argumentów, makro ma **7** —
   w mingw-w64 ta metoda **nie ma parametru `Usage`** (`UINT Width, UINT Height, D3DFORMAT Format,
   D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle`). Wariant z
   `D3DUSAGE_RESTRICTED_CONTENT` jest więc **niewyrażalny** przez to API.
2. Typ `D3DUSAGE` **nie istnieje** w mingw-w64 (są tylko stałe `D3DUSAGE_*` = `__MSABI_LONG(...)`,
   a `CheckDeviceFormat` bierze `DWORD Usage`) — 4 rzuty `(D3DUSAGE)` do usunięcia.
3. `srcusage` był martwy (`-Wunused-but-set-variable`).
**Środek zaradczy (trwały):** napisano `tools_check_d3d_calls.py` — parsuje `#define
IDirect3D*(p,a,b,...)` z prawdziwego `d3d9.h` i porównuje liczbę argumentów w `.c`.
Po poprawkach: **71/71 zgodnych**, jedyny obcy identyfikator to `D3DFMT_NV12` (mamy `#ifndef`).
**Zmiana zakresu testu:** case 3 `stretch` przestał być „NV12 + RESTRICTED_CONTENT"
(niewyrażalny), a stał się **„NV12 w rozmiarze ekranu, StretchRect 1:1"** — czyli dokładnie
przypadkiem realnym dla klienta (dekod 1280×720 → okno 1280×800). Powierzchnia i `srcRect`
są teraz ekranowe dla tego case'u (`g_case_w`/`g_case_h`).
**Nowa wartość dodana:** napisano `analiza/04-NVST-SPEC-BINARIA.md` — bajtową specyfikację
NVST wyciągniętą z `nvst.rs` (RTP + rozszerzenie GS `0x4753`, layout 16-bajtowego ciała,
SRTP KDF/etykiety/IV dla CM i GCM, kolejność replay, FEC Cauchy'ego nad GF(2^8) z
wielomianem `0x11d`, NACK-v2, ramki NVB) wraz z **20 pułapkami** i indeksem 80 odniesień
do linii źródła. To jest dokument, który oszczędza implementatorowi w C czytania
14 695 linii Rusta.
Nowe decyzje **D10-D12**. Nowe ryzyka **R20-R21**.

### 2026-10-20 · runda V
**T4 v3 nie skompilował się ponownie** — `mark()` zostało usunięte razem ze starą warstwą
JSON przy przepisywaniu na v3 (użycia w 7 miejscach, definicji brak). Przywrócone.
**Przełom procesowy:** odkryto, że sandbox ma `apt` → zainstalowano `gcc-mingw-w64-x86-64`
(GCC 12.2) i **skompilowano T4 naprawdę**: `-Wall -Wextra`, zero błędów, zero ostrzeżeń,
binarka 275 kB. Dodano `tools_verify_c.sh` i fakt **F7**. Koniec pracy „na ślepo" nad kodem C.
**D12 wykonane** — przegląd 4 plików kodu użytkownika (`endpoints.py` 545 l., `oauth.py` 795 l.,
`gql_queries.py` 116 l., `adapter.py` 1551 l.):
- **32/32 twierdzeń z `evidence` potwierdzonych** wobec źródeł OpenNOW, **0 rozbieżności**.
  W tym trzy subtelności odwzorowane poprawnie: dwa różne profile urządzenia
  (LCARS `GENERIC`/`PC` vs CloudMatch `UNKNOWN`/`UNKNOWN`), `x-nv-client-identity`
  o dwóch różnych znaczeniach (`"GFN-PC"` vs UA Bifrost), oraz `Content-Type: text/plain`
  dla JSON-a w CloudMatch (`cloudmatch.rs:2268`).
- **LUKA 1 (krytyczna):** `_parse_session` zwraca 8 pól, a `session_info()` w OpenNOW
  (`cloudmatch.rs:2070-2102`) buduje ich **31**. Brakuje m.in. `mediaConnectionInfo`
  (peer bundle: audio/wejście/RTCP), `signalingUrl` (transformacja `rtsps://`→`wss://`),
  `iceServers` (z 3 fallbackami STUN), `rtspsEndpoints[]`, `connectionInfo`,
  `streamingBaseUrl`, `finalizedStreamingFeatures` (serwer **nadpisuje** żądane),
  `seatSetupStep`, `subSessionId`, `termination`. **Bez tego klient C nie wystartuje sesji.**
  Rekomendacja: skopiować kontrakt `session_info()` 1:1 jako `native_context` i przesłać
  przez IPC JSON-lines z redakcją w logach.
- **LUKA 2:** brak `RESUME` (`{"action":2,"data":"RESUME",...}`, `cloudmatch.rs:1540-1598`)
  i brak `pending-session-cleanup.json` (`cloudmatch.rs:580-611, 3527`). Przy tetheringu
  każde zerwanie = nowa kolejka, a osierocone sesje zajmują miejsce na koncie. Etap 5.
- **4 nowe fakty** znalezione przy weryfikacji (nie ma ich w kodzie użytkownika):
  wiązanie kursora z hashem kontekstu katalogu → błąd `catalog_changed`
  (`catalog.rs:404-408`; hash z `[user_id, provider, generation, graphql, vpcId,
  cache_scope, revision, "en_US"]`), `check_catalog_revision` **przed i po** każdym
  pobraniu strony (`catalog.rs:411,417`), cache języków **TTL 14 dni + `allow_stale`**
  (`catalog.rs:214`), `fetch_bounded_page` (`catalog.rs:409`).
- **Co jest wzorcowe i nie zmieniać:** dyscyplina `evidence`, rejestr `UNKNOWN_ENDPOINTS`,
  `NOT_IMPLEMENTED` zamiast zgadywania, higiena sekretów (`connectionInfo` nie do logów),
  „błąd sieci nie kończy próby logowania", limit `fetchCount ∈ 1..500`, oraz fakt że
  buga w `poll()` (parsowanie ciała tylko przy `status >= 400`) **znalazł test, nie przegląd**.
Nowe ryzyko **R21** podniesione do „wysokie/średnie" z mitigacją F7. Nowe **R22**:
`SessionInfo` zbyt ubogie dla klienta C (patrz LUKA 1) — wysokie/krytyczne dla etapu 2.

### 2026-10-20 · runda VI
**Wyniki T4 v3 z maszyny:** izolacja etapów **zadziałała dokładnie tak, jak miała**.
13 z 14 etapów przeszło z `run_rc=0` na x64 i x86; **crashował wyłącznie `updatesurf`**
(`0xC0000005 ACCESS_VIOLATION`, obie architektury). Czyli winnym jest
`UpdateSurface(SYSTEMMEM NV12 → DEFAULT NV12)` — a **nie** `StretchRect`, bo wszystkie pięć
case'ów `stretch` (RGB, YUY2, NV12, NV12+restricted, NV12→RGB) oraz cztery benchmarki
przeszły bez awarii.

**To jest ustalenie projektowe, nie tylko wynik testu:** ścieżka „dekoder pisze do pamięci
systemowej, potem `UpdateSurface` wgrywa na GPU" jest **martwa na tym sterowniku**. Klient
musi albo pisać bezpośrednio do lockowalnej powierzchni NV12 w `D3DPOOL_DEFAULT`, albo iść
w CPU→GDI. Wchodzi do **D7**.

**Druga awaria, tym razem po naszej stronie:** warstwa zapisu JSON w v3 używała mieszaniny
`fseek(0,SEEK_END)` / `ftell` / cofania się o 2 bajty i nadpisywania `"]}"`. W efekcie
**wszystkie pliki etapów miały 0 bajtów**, więc dane z 13 poprawnie wykonanych etapów
przepadły. Harness dodatkowo kasował pliki przed każdym uruchomieniem i czytał
`partial_head` spod złego klucza, więc nie było czego obejrzeć.

**Naprawa (v4):**
1. Warstwa zapisu przepisana: **akumulator w RAM + przepisanie całego pliku** przy każdym
   elemencie. Zero `fseek`/`ftell`. Plik jest zawsze poprawnym, zamkniętym JSON-em.
2. **Nowy test jednostkowy warstwy JSON** — `tools_test_json_layer.sh` wycina samą warstwę
   z pliku `.c`, kompiluje ją **na Linuxie** (bez D3D9) i waliduje wynik: 3 elementy,
   zagnieżdżone obiekty, tablice, escapowanie `x"y\z`, HRESULT jako hex, element z 2000 pól.
   **PASS (24 150 bajtów).** To jest ta sama lekcja co przy T1: testować nudne części na
   hoście, zanim zaangażuje się maszynę użytkownika.
3. `stage_updatesurf` rozbity na **6 podkroków** (`--case=0..5`: create SYSTEMMEM →
   create DEFAULT → `LockRect` → wypełnienie → `UnlockRect` → `UpdateSurface`), każdy
   w osobnym procesie. Następnym razem dowiemy się, **które dokładnie wywołanie** zabija
   driver.
4. Harness: 19 procesów na architekturę, pliki etapów **zachowane** w `T4_tmp\stages\`,
   `partial_head` czytany z właściwego miejsca i drukowany przy błędzie parsowania.
5. Podczas poprawek weryfikator złapał dwie moje własne wpadki: `fclose(g_f)` po usunięciu
   `g_f` oraz blok `json_begin` wklejony na początek pliku (skrypt łatający użył `find()`,
   który zwrócił `-1`, więc `s[-1:end]` wyciął ostatni znak i wkleił blok nie tam, gdzie
   trzeba). Obie poprawione; `tools_verify_c.sh` → **zero błędów, zero ostrzeżeń**.

**Nowy deliverable: `BOOTSTRAP.md`** (po angielsku, zgodnie z wymaganiem użytkownika co do
języka aplikacji). Samowystarczalny dokument startowy dla drugiego agenta: misja, sprzęt,
architektura, 13 decyzji z uzasadnieniem i warunkiem cofnięcia, **mapa badań** (gdzie w
OpenNOW szukać każdego tematu, z numerami linii), zweryfikowane fakty z pomiarów,
**specyfikacja logowania (§7, 12 podsekcji)**, 8 znanych pułapek, rejestr 22 ryzyk, zasady
pracy, plan etapowy z bramkami i 5 pytań do użytkownika.

**Dlaczego specyfikacja logowania jest tak rozbudowana:** użytkownik nie przetestuje
logowania, kolejki ani streamingu w sandboxie. Dostanie kilka prób na żywo, każda kosztuje
realny czas. Log jest **jedynym** przyrządem. Dlatego `BOOTSTRAP.md` §7 wymaga: jednego
strumienia zdarzeń renderowanego dwojako (tekst + JSONL), `correlation id` na każdą operację,
8 kanałów z rotacją 2 MiB × 3, **amplifikacji przy awarii** (podniesienie poziomu, flush,
bundle, self-test, komunikat na ekran), **bundla diagnostycznego** z `summary.json` na
wierzchu, **10 self-testów działających bez konta** (w tym wektory znanych odpowiedzi dla
KDF SRTP i dla Cauchy FEC), **pierścieniowych buforów surowych datagramów** (64 KiB/socket,
zrzucane tylko przy awarii) i redakcji **białą listą** na granicy logowania.

**D13 (nowa decyzja):** logowanie to podsystem pierwszej klasy, wdrażany w **etapie 1**,
nie doklejany później.

**Do uruchomienia przez użytkownika:** `python T4_d3d9_present.py` (wersja v4).
