# Port OpenNOW → „KarnyNow-TUI": analiza wykonalności

**Data:** 2026-10-20
**Cel:** Dell Latitude E5500 · Core 2 Duo T7250 (2C/2T, 2 GHz, Merom) · 2 GB DDR2 · GMA 4500MHD (`8086:2A42`) · 1280×800 @ **40 Hz** · Windows 7 SP1 x64 (JG 2021) · mingw GCC 16.2.0 · Python (3.8.x)
**Źródło prawdy:** pełne źródła OpenNOW `main` (commit `a49b124`, 2026-10-18), pobrane i przejrzane w tej analizie. Wszystkie cytaty poniżej mają ścieżkę i linię.

---

## 0. Werdykt w pięciu zdaniach

1. **Port jest wykonalny, ale nie jako „port OpenNOW"** — jako **nowy, minimalny klient NVST napisany od zera w C**, który z OpenNOW bierze *wiedzę o protokole* (repo jest MIT, więc legalnie), a nie kod.
2. **OpenNOW nie uruchomi się na tej maszynie w ogóle** — nie przez GUI, tylko przez twardy wymóg D3D11 w prezenterze i Qt 6.8+ (Win10+). To nie jest ograniczenie do obejścia, tylko powód, dla którego Twój pomysł jest *jedyną* drogą.
3. **Największe ryzyko projektu to nie protokół, tylko CPU**: 2 × Merom @ 2 GHz bez DXVA2 musi programowo zdekodować H.264 i jeszcze coś z nim zrobić. To jest **mierzalne dziś** i od tego zależy cała reszta — dlatego test `T1` jest bramką nr 1.
4. **Warstwa Python/TUI ma własnego killera**: conhost Windows 7 **nie parsuje sekwencji ANSI/VT**, więc Textual (i `rich.Live`/`Layout`) nie zadziała „z pudełka". Są trzy obejścia, wszystkie opisane w §5.
5. **Protokół NVST jest już w pełni udokumentowany w repo** — łącznie z bajtowymi ramkami kontrolnymi, formatem NACK-v2, etykietami kanałów SCTP i pełnym ANNOUNCE SDP. To skraca etap „reverse engineering" do zera i jest największą wartością tego repo dla Ciebie.

**Rekomendacja:** idziemy, ale w kolejności *najpierw pomiar, potem kod*. Nie piszemy ani linii NVST w C, dopóki `T1`/`T2`/`T3` nie wrócą z maszyny.

---

## 1. Dlaczego OpenNOW nie jest punktem startowym (dowody ze źródeł)

### 1.1 Prezenter wymaga D3D11 — bez wyjątku

`crates/opennow-streamer-platform-windows/src/lib.rs:204-207`:

```rust
pub const fn software_backend_available(&self) -> bool {
    (self.h264_software_decode || self.h265_software_decode || self.av1_software_decode)
        && self.d3d11_presentation
}
```

Nawet **programowy** dekoder jest na Windows uzależniony od `d3d11_presentation`. A `d3d11_presentation = graphics.is_ok()` (`windows/mod.rs:434`), gdzie `graphics` to urządzenie D3D11.

Twój własny probe z maszyny: `has_d3d11: false`, `dxva2_query_ok: false`, `QI(IDirectXVideoDecoderService)=0x80004002` (`E_NOINTERFACE`).

**Wniosek:** na GMA 4500MHD `software_backend_available() == false` i `WindowsBackend` nie powstanie. OpenNOW zakończy start sesji błędem, zanim pojawi się jakakolwiek ramka. Zmiana GUI z Qt na TUI **nic tu nie zmienia** — to jest blokada w warstwie streamera, nie w warstwie UI.

Dodatkowo: `decoder.rs:507-509` odrzuca każdy MFT bez `MF_SA_D3D11_AWARE` („MFT is not D3D11-aware"), a to atrybut Windows 8+. Na Win7 nie istnieje.

### 1.2 Qt 6.8+ nie działa na Windows 7

README OpenNOW wymaga „Qt 6.8+ with Quick, Multimedia, and ShaderTools". Qt 6.x wymaga Windows 10 1809+. Do tego cała integracja wideo idzie przez **QRhi** i import natywnej tekstury do scene graph (`opennow-streamer/README.md`, rozdział „Qt GPU integration", kroki 1-5) — FFI „exposes no CPU image, encoded-frame callback, swap chain, window or Qt object".

To znaczy: **nie ma nawet interfejsu**, przez który TUI mogłoby odebrać klatkę. Zastąpienie Qt Pythonem wymagałoby napisania nowego backendu prezentacji od zera — czyli dokładnie tego, co proponujesz zrobić w C.

### 1.3 Rust na Meromie: ryzyko dodatkowe

Workspace wymaga `rust-version = 1.85`, `edition = 2024`, a zależnościami są m.in. `scuffle-av1`, `dav1d`-opodobne ścieżki i `oxideav`. Nowoczesne buildy Rust i biblioteki multimedialne coraz częściej zakładają AVX. **Merom nie ma AVX** (ma SSE3/SSSE3). To osobna oś awarii, której w C kontrolujesz jednym `-march=core2`.

### 1.4 Co z tego wynika

Port „1:1" jest niemożliwy. Ale to dobra wiadomość: **90 % wartości OpenNOW dla Ciebie to nie kod, tylko wiedza** — i ta wiedza jest w repo w formie dokumentów i testów, które można przeczytać i przepisać na C.

---

## 2. Co realnie jest do przeniesienia — mapa i wycena

Policzone na pobranym drzewie `main`:

| Obszar | Pliki | Linie (`.rs`) | Czy potrzebujesz | W C (szacunek) |
|---|---|---|---|---|
| `opennow-core` (auth, katalog, CloudMatch, ustawienia, updater, push, analytics, Discord) | `native/opennow-core/src` | **63 735** | ~20 % (auth + katalog + CloudMatch/kolejka) | Python, już masz |
| `opennow-streamer-transport` (NVST) | w tym `nvst.rs` **14 695** | **29 592** | **tak, wszystko** | 6-9 k linii C |
| `opennow-streamer-core` (RTSP, orkiestracja) | w tym `nvst_rtsp.rs` **3 921** | **17 235** | tak | 2,5-4 k linii C |
| `opennow-streamer-platform` (kolejki, dekoder, audio, prezentacja) | `media.rs` itd. | **22 373** | ~40 % | 2-3 k linii C |
| `opennow-streamer-platform-windows` (MF/D3D11/DXGI) | | **11 452** | **NIE — do kosza** | 0 (piszesz D3D9/GDI) |
| `opennow-streamer-hid` (gamepady, HID, Sony) | | **5 025** | nie (klawiatura+mysz) | ~300 linii |
| `opennow-streamer-ffi` (C ABI dla Qt) | | **4 245** | nie | własne, ~400 linii |
| `opennow-qt` | | **96 767** | **NIE** | Python TUI |
| Razem | | **~470 k** | | **~12-18 k linii C + ~3-5 k Pythona** |

**Kluczowa obserwacja:** „NVST w czystym C" to nie jest przepisanie 470 k linii. Rdzeń, którego potrzebujesz, to `nvst.rs` + `nvst_rtsp.rs` + kawałek `media.rs` ≈ **36 k linii Rusta**, z czego ~40 % to testy, a kolejne ~20 % to gałęzie (HEVC/AV1/HDR/mikrofon/gamepad/nagrywanie/replay), których **nie implementujesz**. Realny zakres do przepisania: **~10-14 k linii Rusta → ~8-12 k linii C**.

To jest projekt na tygodnie, nie na miesiące — pod warunkiem, że nie utoniesz w DTLS/SCTP (patrz §6).

---

## 3. Co dokładnie trzeba zaimplementować w C (stan faktyczny protokołu)

Wszystko poniżej jest *wyczytane ze źródeł*, nie zgadnięte.

### 3.1 Sygnalizacja: RTSP **tunelowany przez WebSocket**

To jest najważniejsze zaskoczenie i zmienia projekt: `RtspClient::connect` (`nvst_rtsp.rs:~512`) robi `rtsp_endpoint_urls(endpoint)` → `(wss, target)`, a potem `wss.into_client_request()` z **tungstenite**. Czyli `rtsps://…` jest w praktyce `wss://…`, a ramki RTSP lecą jako wiadomości tekstowe WebSocket.

- Nagłówki: `X-GS-Version: 14.2`, `Host`, `x-nv-sessionid: <sessionId>` (`nvst_rtsp.rs:1078-1082`).
- Potwierdzenie w CloudMatch: `metaData` zawiera `{"key":"wssignaling","value":"1"}` (`cloudmatch.rs:1675`).
- Sekwencja: `OPTIONS` → `DESCRIBE` (z `Accept: application/sdp`, `x-nv-abtesting: 2`) → `SETUP` (video) → `ANNOUNCE` (nasz SDP) → `PLAY` → keepalive co **2 s** → `TEARDOWN`.
- `PLAY` musi zwrócić 200; 455 = failed startup. Gotowość transportu jest bramkowana: **DTLS zakończony LUB uwierzytelnione media z negocjowanego peera** — samo ICE, wysłany pakiet czy pakiet nieuwierzytelniony **nie** wystarcza (`docs/streamer-comparison/rtsp-negotiation.md`). Limit oczekiwania: 20 s.

**W C:** klient WebSocket over TLS to ~400-700 linii (handshake + maskowanie + ramki). Żadna biblioteka RTSP nie jest potrzebna.

### 3.2 SDP z `DESCRIBE` — pola, które muszą być parsowane

`nvst_rtsp.rs:1128-1150`:

| Atrybut SDP | Znaczenie |
|---|---|
| `general.iceUserNameFragmentV2` / `iceUsernameFragment` | ufrag ICE |
| `general.icePasswordV2` / `iceUsernamePwd` | hasło ICE |
| `general.dtlsFingerprintV2` / `dtlsFingerprint` | odcisk DTLS peera |
| `general.nativeRtcOnBundlePort` | **musi być `1`**, inaczej `nvst-legacy-transport-unsupported` |
| `general.rtcpOnSctp` | RTCP idzie kanałem SCTP `rtcp1`, nie RTP |
| `general.enableUnifiedSocket`, `general.serverBundlePort`, `general.rtcVideoOnNativeBundle` | routing socketów |
| `general.pingVersion` | wersja pingów (domyślnie 6) |
| `ri.hidDeviceMask` | maska urządzeń HID |
| `a=control:` dla `video` | URI do `SETUP` |

### 3.3 `ANNOUNCE` — pełny SDP klienta jest w repo, gotowy do przepisania

`nvst_rtsp.rs:1520-1600+`. Dla Ciebie istotne wartości (H.264, 8-bit 4:2:0):

```
s=NVIDIA Streaming Client
a=x-nv-video[0].clientViewportWd:1280
a=x-nv-video[0].clientViewportHt:720
a=x-nv-video[0].maxCodecProfile:3      <- High
a=x-nv-video[0].maxCodecLevel:51       <- 5.1
a=x-nv-video[0].maxH264Profile:3 / maxH264Level:51
a=x-nv-video[0].packetSize:1280
a=x-nv-video[0].enableRtpNack:1 / rtpNackQueueLength:2048
a=x-nv-video[0].framePacing.mode / feedbackMode
a=x-nv-video[0].refPicInvalidation:1
a=x-nv-general.clientSupportsIntraRefresh:1
a=x-nv-video[0].maxFPS:30
a=x-nv-video[0].initialBitrateKbps:<N> / initialPeakBitrateKbps:<N>
a=x-nv-vqos[0].bitStreamFormat:0       <- 0=H264, 1=HEVC, 2=AV1
a=x-nv-vqos[0].fec.enable:1 / repairPercent:20 / repairMinPercent:20 / repairMaxPercent:35
a=x-nv-vqos[0].dynamicStreamingMode:<0|1>
a=x-nv-packetPacing.version:3 / mode:1 / numGroups:5 / maxDelayUs:2000
a=x-nv-ri.partialReliableThresholdMs:300
```

**Krytyczna uwaga o bitrate:** domyślnie OpenNOW wysyła **75 Mbps**, gdy brakuje `maxBitrateMbps` (`docs/streamer-comparison/video.md`, „Gotchas"). Na Twoim łączu (tethering USB 2.0 / Wi-Fi 2,4 GHz) to przepis na katastrofę. W C ustawiamy twardo np. **3-6 Mbps** dla 720p30 i 2-4 Mbps dla 540p60 — i to jest *nasz* wybór, nie serwera.

### 3.4 Wideo: dedykowany socket „Mjolnir"

- Wideo **nie** jedzie po ICE bundle. Osobny UDP, preferowany port klienta **49005**; bundle (audio/input/RTCP) na **49006**, serwer **5004** (`docs/streamer-comparison/video.md`, sekcja o oficjalnym kliencie).
- `ANNOUNCE` ustawia `rtcVideoOnNativeBundle:0`.
- Każdy pakiet RTP wideo ma **16-bajtowy nagłówek GameStream** z rozszerzeniem RTP o profilu **`0x4753`** ("GS"), zawierający SOF/EOF, indeks klatki i współrzędne FEC. Stałe z `nvst.rs:263-274`: `FLAG_CONTAINS_PIC_DATA=0x01`, `FLAG_EOF=0x02`, `FLAG_SOF=0x04`, `STREAM_PACKET_INDEX_MASK=0x00ff_ffff`, `GS_VIDEO_EXTENSION_PROFILE=0x4753`, `GFN_EXTENDED_FRAME_HEADER_BYTES=20`.
- SRTP: `SRTP_AES_CM_128_HMAC_SHA1_80` (tag 10 B) oraz AEAD `AES_GCM` (tag 16 B) i `AES_GCM_8` (tag 8 B) — `nvst.rs:81-86`. Etykiety wyprowadzania kluczy: `GFN_SRTP_KEY_LABEL=0x00`, `SALT=0x02`, `SRTCP_KEY=0x03`, `SRTCP_SALT=0x05` (`nvst.rs:90-94`). Krypto jest **zaimplementowane ręcznie** na `aes`/`ctr`/`hmac`/`sha1`/`ghash` — czyli w C to samo na OpenSSL/mbedTLS, bez libsrtp.
- Reorder: okno **1024** pakietów (max 2048), czekanie na lukę **52 ms**, bufor gniazda **8 MiB**.
- FEC: **Reed-Solomon** NVIDIA, naprawiany *przed* składaniem access unit (`nvst.rs:211-215`, nagłówek modułu).
- NACK: prywatny **NACK-v2** jako komenda ServerControl **`0x0317`** na kanale `control_channel_partially_reliable`, format bajtowy w `docs/mjolnir-nack-v2.md` (wersja 2, ordinal, liczba rekordów, `u16LE` base + `u64LE` bitmapa 64). Limit: 64 pakiety/komenda, 2048 oczekujących, 3 próby, odstęp `RTT + 4 ms`, timeout śledzenia 52 ms.
- Kluczowa klatka H.264: NAL type **5**.

### 3.5 Audio

- Opus RTP po bundle, **PT 111**, RED **PT 63**, zegar **48 kHz** (`nvst.rs:275-276`, `docs/streamer-comparison/audio.md`).
- Jitter buffer: cel sieciowy startuje od **45 ms** i rośnie; kolejność: reorder → RED recovery → decode. concealment max **100 ms**.
- `MAX_OPUS_PACKET_BYTES = 1275`, `MAX_REDUNDANT_AUDIO_BLOCKS = 8`.
- Wyjście: **WASAPI**, 48 kHz stereo, prezenter pollowany; w OpenNOW przez SDL2 — w C idziemy bezpośrednio w WASAPI (mniej warstw, mniejsza latencja).

### 3.6 Wejście (to jest „gameplay", bez tego projekt jest bezwartościowy)

Kanały danych SCTP po DTLS na bundle, stałe etykiety (`nvst_input.rs:14-20`):

```
control_channel_reliable
custom_channel_reliable / custom_channel_partially_reliable
control_channel_partially_reliable      <- mysz relatywna, NACK-v2
control_channel_unreliable
input_channel_partially_reliable        <- wejście
cursor_channel
```

Ramki kontrolne są **udokumentowane bajt co do bajtu** w `docs/streamer-comparison/features.md`, wraz z kolejnością aktywacji sesji:

| # | Komenda | Payload |
|---|---|---|
| 1 | `0x020b` enable input false | `0b 02 0c 00 00 00 00 00 01 00 00 00 00 00 00 00` |
| 2 | `0x020d` device descriptor index 2 | 64 B (pełny hex w dokumencie) |
| 3 | `0x0308` cursor capture on | `08 03 01 00 01` |
| 4 | `0x030d` track cursor on | `0d 03 01 00 01` |
| 5 | `0x0320` window state **19** | `20 03 0c 00 00 00 00 00 13 00 00 00 00 00 00 00` |
| 6 | `0x0321` system state 0 | `21 03 0c 00 00 00 00 00 00 00 00 00 00 00 00 00` |
| 7 | `0x020b` enable input true | `0b 02 0c 00 00 00 00 00 01 00 00 00 01 00 00 00` |

Uwagi, które oszczędzą Ci dni debugowania:
- Nagłówek ServerControl to `u16LE` kod komendy + `u16LE` długość. `0x0308` na drucie to `08 03` — **little-endian, nie czytaj tego jako big-endian**.
- **Window state 19 jest wymagane.** Same zera sprawiają, że sesja wygląda na nieaktywną i serwer nie wysyła aktualizacji trybu kursora.
- Keepalive: `0x0200`.
- Mysz relatywna: `0x0206`, trasa `control_channel_partially_reliable`; oficjalny klient przy każdej zmianie fokusu wysyła `accel=0, speed=10` (NVB type 10) — **dokładne bajty type 10 nie są znane**, OpenNOW rekonstruuje `0x0323`. To jedyne miejsce, gdzie musimy zaakceptować „działa, ale nie wiemy dlaczego".
- Przechwytywanie: Raw Input (`RIDEV_INPUTSINK`, wątek o priorytecie ABOVE_NORMAL) + osobne okno wiadomości — wzorzec z `opennow-streamer-platform/src/windows_raw_input.rs` jest w 100 % przenośny na C (to czyste Win32 API).

### 3.7 CloudMatch i kolejka (to ląduje w Pythonie)

- `GET {streaming_base}v2/serverInfo` → `requestStatus.serverId` = **vpcId** + lista regionów (`regional_bases`). Baza domyślna: `https://prod.cloudmatchbeta.nvidiagrid.net/` (`cloudmatch.rs:18`).
- `POST {base}v2/session` z ciałem z `build_create_body` (`cloudmatch.rs:1643-1760`). **Mam pełną listę pól** — `appId`, `clientIdentification: "GFN-PC"`, `clientVersion: "30.0"`, `sdkVersion: "2.0"`, `streamerVersion: "14"`, `secureRTSPSupported: true`, `clientRequestMonitorSettings[0]` (geometria + fps + dpi 96), `useOps: true`, `audioMode: 2`, `metaData` (`wssignaling=1`, `SubSessionId`, `surroundAudioInfo=2`), `requestedStreamingFeatures` (reflex/bitDepth/chromaFormat/dlssOverrides/…), `userAge: 25`, `clientTimezoneOffset`.
- **Kodek NIE jest wysyłany do CloudMatch.** Jest wybierany lokalnie i ogłaszany dopiero w RTSP `ANNOUNCE` (`x-nv-vqos[0].bitStreamFormat`) — komentarz w `cloudmatch.rs:1646-1649`. To dla Ciebie ważna swoboda: **backend nie wie i nie pyta, czy masz dekoder sprzętowy.** Nie ma pola „mam DXVA2". Jedyne twarde ograniczenia to H.264 = 8-bit 4:2:0 i pixel-budget.
- `GET v2/session` = discovery aktywnych sesji; `DELETE v2/session/{id}` = sprzątanie.
- **Kolejka:** `queuePosition` czytane z czterech możliwych miejsc (`cloudmatch.rs:2655-2663`): `queuePosition`, `seatSetupInfo.queuePosition`, `sessionProgress.queuePosition`, `progressInfo.queuePosition`; brane jest pierwsze `> 0`.
- **Statusy sesji** (`cloudmatch.rs:2698-2708`): `1=preparing, 2=ready, 3=streaming, 4|5=paused, 6=resuming, 7=finished, >3=failed`.
- **Postęp setupu** (`docs/session-start-progress.md`): `seatSetupInfo.seatSetupStep` → `0` łączenie, `1` oczekiwanie na rig + pozycja w kolejce, `2/3/4` konfiguracja riga, `5` sprzątanie poprzedniej sesji, `6` oczekiwanie na storage. Kroki **nie są liniowe** i nie da się z nich policzyć procentów.
- Autoryzacja: **`GFNJWT <id_token>`** (nie Bearer) dla `serverInfo`/GraphQL/CloudMatch — tak, jak już ustaliłeś; `Bearer access_token` tylko dla `login.nvidia.com/userinfo`.

---

## 4. Rzeczywisty limit sprzętowy: budżet CPU

To jest **jedyna** rzecz, która może zabić projekt. Liczę jawnie, żeby było widać założenia.

### 4.1 Założenia

- Merom (Model 15 Stepping 13 = FSB 800, **SSE3 + SSSE3, brak SSE4/AVX**), 2 rdzenie, 2 GHz, bez HT.
- Dekoder: FFmpeg `h264` (asm SSSE3) albo openh264 (asm SSE2/SSSE3).
- Orientacyjna cena dekodowania H.264 baseline/main na zoptymalizowanym asm: **1,5-2,5 cyklu na piksel na klatkę** (realny przedział dla starego x86; dlatego mierzymy, a nie zgadujemy).

### 4.2 Rachunek

Dostępne: `2 rdzenie × 2 GHz = 4000 Mcy/s`, ale system + sieć + audio + prezentacja zabiorą realnie 25-35 %, więc **budżet na dekodowanie ≈ 2600-3000 Mcy/s**.

| Tryb | Piksele/s | Dekod @1,5 cy/px | Dekod @2,5 cy/px | Werdykt |
|---|---|---|---|---|
| 640×360 @30 | 6,9 Mpx/s | 26 % | 43 % | **bezpiecznie** |
| 960×540 @60 | 31,1 Mpx/s | 47 % | 78 % | ryzykownie |
| 1280×720 @30 | 27,6 Mpx/s | 41 % | **69 %** | **granicznie — do zmierzenia** |
| 1280×720 @60 | 55,3 Mpx/s | 83 % | 138 % | **nie** |
| 1920×1088 @30 | 62,7 Mpx/s | 94 % | 157 % | **nie** |

Do tego dochodzi **konwersja koloru** i to ona jest cichym zabójcą:
- `NV12 → RGB` w libswscale na Core2: ~0,4-0,7 cy/px → dla 720p30 to **dodatkowe 330-580 Mcy/s**, czyli +8…15 pkt proc. Ścieżka OpenNOW (`DecodedVideoFrame { rgb: Vec<u8> }`, `output.rs:127-131`) idzie dokładnie tak — **na tej maszynie to byłby gwóźdź do trumny**.
- `NV12 → YUY2` (tylko przeplot chromy, bez macierzy): ~0,1 cy/px → **~28 Mcy/s, pomijalne**.
- **Zero konwersji na CPU**, jeśli D3D9 zrobi `NV12 → RGB` i skalowanie w `StretchRect`/overlay na GPU.

### 4.3 Wniosek projektowy (twardy)

**Prezentacja musi być zrobiona tak, żeby CPU nie dotykał pikseli.** Kolejność preferencji:

1. **D3D9 + powierzchnia NV12 + `StretchRect`/overlay** → CPU robi tylko dekod, GPU robi CSC i skalowanie. Najlepszy scenariusz: 720p30 mieści się w ~45-70 % CPU.
2. **D3D9 + YUY2** (NV12→YUY2 na CPU, ~1 % budżetu) — jeśli sterownik odrzuci NV12 `StretchRect`.
3. **GDI + `StretchDIBits` z YUY2/BGR** — ostateczność; `BitBlt` 1280×720×32 na GMA4500 to ~5-15 ms/klatkę i tearing, ale przy 30 fps i tak jest ograniczony do 33 ms.

Dodatkowy prezent od sprzętu: **odświeżanie 40 Hz** oznacza, że i tak nie wyświetlisz więcej niż 40 kl/s. Celowanie w 60 fps jest marnowaniem CPU — **720p30 albo 540p30** to naturalny punkt pracy, a 40 Hz daje nawet lekki margines.

### 4.4 RAM

2 GB z Windows 7 x64 = realnie ~1,5-1,7 GB dostępne. Budżet:

| Pozycja | Szacunek |
|---|---|
| Windows 7 x64 + JG | 700-900 MB |
| Proces C (streamer): bufor gniazda wideo 8 MiB | 8 MB |
| reorder window 1024 × 1280 B + FEC | ~4 MB |
| dekoder H.264 720p (referencje + DPB, 2 wątki) | 40-80 MB |
| kolejka audio + jitter | ~4 MB |
| powierzchnie D3D9 (2-3 × 720p NV12 + backbuffer) | ~10 MB (VRAM współdzielony) |
| **Razem proces C** | **~70-110 MB** |
| Python 3.8 + TUI | 40-70 MB |

**Mieści się**, ale bez marginesu na eksperymenty. Dlatego: **żadnego FFmpeg jako procesu** (ffmpeg.exe z buforami to kolejne 50-150 MB), dekodowanie przez `libavcodec` w procesie albo openh264 w procesie. Bufor gniazda 8 MiB (taki jak w OpenNOW) na tej maszynie **zmniejsz do 1-2 MiB** — przy tetheringu i tak nie pomoże, a zje pamięć.

---

## 5. Warstwa Python/TUI — trzy twarde ograniczenia Win7

### 5.1 Python: sufit to 3.8.10

Python 3.9+ nie uruchamia się na Windows 7 („Python 3.9.8 cannot be used on Windows 7 or earlier" — python.org). **3.8.10** jest ostatnim wydaniem z instalatorem dla Win7 i **osiągnął EOL 2024-10-07** — nie dostaje poprawek bezpieczeństwa, a jego `ssl` to OpenSSL 1.1.1 (też EOL).

Konsekwencje praktyczne:
- Wszystkie zależności muszą być **cp38**. `pip` na Win7 z Pythonem 3.8 nadal działa, ale coraz więcej pakietów nie publikuje kół cp38 → trzeba **pinować wersje** i mieć `requirements.txt` z exact pinami + lokalny katalog wheeli.
- `cryptography`/`pyopenssl`: nie potrzebujesz. OAuth device grant nie wymaga własnej kryptografii — a Twój `pkce.py` już jest i (słusznie) nie jest używany.
- **Uczciwe ostrzeżenie:** trzymanie tokenów OAuth w procesie na EOL-owym interpreterze to ryzyko. Mitigacja: tokeny szyfrowane przez **DPAPI** (`CryptProtectData`) w pliku, a nie plaintext — DPAPI jest dostępny z `ctypes` bez żadnych zależności. Warto to dodać do `credential_vault` w Pythonie.

### 5.2 conhost Win7 **nie ma VT** — Textual odpada „z pudełka"

Potwierdzenie z pierwszej ręki: issue Textualize/textual#3378, „Broken output on old windows cmd", zgłoszone dokładnie na **Windows 7 x64 + cmd.exe + Python 3.8.10 + textual 0.38.1**. Objaw: „Colors are working fine with rich, however textual output is full of ANSI sequences". Diagnoza `rich` z tego zgłoszenia:

```
legacy_windows = True
color_system  = 'windows'
WindowsConsoleFeatures(vt=False, truecolor=False)
```

To znaczy:
- **`rich` działa** w trybie legacy — kolory tłumaczy na wywołania Win32 Console API. ✅
- **`rich.Live`, `Layout`, `Screen`, alt-screen, pozycjonowanie kursora — nie działają**, bo wymagają VT. ❌
- **Textual wymaga VT** → na czystym conhost Win7 jest bezużyteczny. ❌
- Rejestr `HKCU\Console\VirtualTerminalLevel=1` to mechanizm **Windows 10**; na Win7 nie ma czego włączyć, bo parsera VT w conhost po prostu nie ma. (Warto to jednak *zmierzyć* — masz modyfikację JG 2021, więc nic nie zakładam: test `T3` to sprawdza.)

### 5.3 Trzy drogi wyjścia — i moja rekomendacja

| Opcja | Co to daje | Koszt | Ryzyko |
|---|---|---|---|
| **A. Zewnętrzny emulator terminala** (ConEmu / Cmder / mintty) | Pełne ANSI → **Textual działa** bez zmian | 0 linii kodu | ConEmu/Cmder na Win7: działa, ale to dodatkowy proces (30-80 MB RAM!) i zewnętrzna zależność. mintty+winpty: lekkie, ale `winpty` bywa kapryśny z Pythonem. **Na 2 GB RAM to realny koszt.** |
| **B. Własny backend TUI na Win32 Console API przez `ctypes`** | `WriteConsoleOutputW` (pełna kontrola komórek + atrybuty), `ReadConsoleInputW` (KEY_EVENT/MOUSE_EVENT), własny renderer różnicowy | ~400-700 linii Pythona, **bez żadnych zależności** | Deterministyczne, działa w *każdym* conhost. Ograniczenie: **16 kolorów** (atrybuty), brak truecolor. Dla listy gier i statusu kolejki — w zupełności够. |
| **C. „Fake console": okno GDI + font monospace** | Pełna kontrola, 256/truecolor, szybkie odświeżanie | ~800-1200 linii C/Python | Przestaje być konsolą; sprzeczne z Twoim wymaganiem „przez konsolę". Odrzucam, chyba że B zawiedzie. |

**Rekomendacja: B, z `rich` jako silnikiem *formatowania*, nie *wyświetlania*.**

Konkretnie: `rich.console.Console(file=StringIO(), force_terminal=True, legacy_windows=False, color_system="standard")` renderuje widżety (Table/Panel/Text) do ciągu z ANSI; własny parser ANSI → komórki `CHAR_INFO` → `WriteConsoleOutputW`. Dostajesz cały ekosystem Rich (tabele, padding, wyrównania, kolory) **bez** zależności od VT w systemie. To ~200 linii parsera ANSI + ~200 linii renderer różnicowego.

Architektura ekranu:
```
┌ KarnyNow ────────────────────────────────────────┐
│ Konto: jan@… (ULTIMATE)  Region: EU-Central  ▮▮▯ │
│ ──────────────────────────────────────────────── │
│ > 1. Cyberpunk 2077            [READY]           │
│   2. Witcher 3                 [READY]           │
│   3. Baldur's Gate 3           [STORE:STEAM]     │
│ ──────────────────────────────────────────────── │
│ Kolejka: #7 · rig: configuring (step 3) · 00:42  │
│ [Enter] uruchom  [r] odśwież  [s] ustawienia     │
└──────────────────────────────────────────────────┘
```
Bez ramek Unicode, jeśli `T3` wykaże problem z fontem — fallback na ASCII (`+--+`, `|`).

---

## 6. Architektura docelowa

```
┌──────────────────────────────┐        ┌───────────────────────────────────┐
│  karnynow.exe  (Python 3.8)  │        │  knvstd.exe / knvst.dll  (C99)    │
│                              │        │                                   │
│  • TUI (Win32 Console API)   │ JSON   │  • CloudMatch → kontekst sesji    │
│  • OAuth 2.0 device grant    │─lines─▶│  • WSS + RTSP (OPTIONS..PLAY)     │
│    (login.nvidia.com)        │ stdout │  • Mjolnir: SRTP, GS 0x4753,      │
│  • GraphQL: katalog/bibliot. │◀───────│    reorder, RS-FEC, NACK-v2       │
│  • v2/serverInfo → vpcId     │ events │  • bundle: ICE + DTLS + SCTP      │
│  • v2/session (kolejka!)     │        │    → audio Opus, input, RTCP      │
│  • DPAPI vault na tokeny     │        │  • openh264/ffmpeg → NV12 (CPU)   │
│  • ustawienia, logi          │        │  • WASAPI (audio out)             │
│                              │        │  • D3D9 NV12 StretchRect (present)│
└──────────────────────────────┘        │  • Raw Input → ramki NVB          │
         ▲                              └───────────────────────────────────┘
         │ stdout użytkownika                          │
   konsola TUI                              osobne okno pełnoekranowe 1280×800
```

**Dlaczego dwa procesy, a nie `ctypes` w Pythonie:**
1. **Dokładnie ten wzorzec ma już OpenNOW i on działa** — `docs/core-protocol.md`: newline-delimited UTF-8 JSON po dziedziczonym stdin/stdout, limit linii **1 MiB**, stderr na redagowaną diagnostykę, 5 s na handshake, `unknown/malformed/oversized` → rozłączenie zamiast niejednoznacznego stanu. Masz gotową specyfikację protokołu IPC, przetestowaną w produkcji. Kopiujemy ją 1:1 (z `protocolVersion` i `capabilities`).
2. GIL i `timeBeginPeriod`: dekodowanie i pętla prezentacji potrzebują twardego timera i priorytetu wątków. W `ctypes` walczysz z interpreterem; w osobnym procesie masz czystą kontrolę.
3. Crash streamera nie zabija sesji logowania i nie gubi tokenów.
4. Python nie dotyka pikseli ani pakietów — więc jego wersja (3.8, EOL) nie ma wpływu na ścieżkę medialną.

**Tryby pracy streamera** (żeby dało się testować bez konta):
- `--loopback` : syntetyczne klatki H.264 z pliku → cały tor dekod+prezentacja (test `T1`/`T4` na żywym kodzie),
- `--capture` : tylko Mjolnir → zrzuca access unity do pliku `.264` (diagnoza bez dekodera),
- `--real` : pełna sesja.

---

## 7. Stos bibliotek w C i **licencje** (to jest decyzja, którą musisz podjąć)

| Zadanie | Propozycja | Licencja | Uwagi dla Win7/mingw |
|---|---|---|---|
| TLS + WebSocket(WSS) | **mbedTLS 3.6 LTS** + własny klient WS (~500 linii) | Apache-2.0 / GPL-2 | mbedTLS buduje się z mingw bez problemu, statycznie, bez zewnętrznych zależności. Alternatywnie OpenSSL 3.x (Apache-2.0), ale cięższy w budowie. |
| DTLS (bundle) | **mbedTLS DTLS** (ten sam stos) | jw. | Potrzebne: `DTLSv1_export_keying_material`-równoważnik do SRTP (w mbedTLS: `mbedtls_ssl_dtls_srtp_*`, dostępne od 2.x z `MBEDTLS_SSL_DTLS_SRTP`). |
| ICE/STUN | **libjuice** (czysty C, ~5 k linii) | MPL-2.0 | Autor libdatachannel; minimalny, bez zależności. Albo ręcznie — OpenNOW i tak implementuje STUN binding sam (`nvst.rs:248-259`: magic cookie, USERNAME, PRIORITY, MESSAGE-INTEGRITY, XOR-MAPPED-ADDRESS, FINGERPRINT). **Ręczna implementacja to ~300 linii i zero zależności** — rekomenduję. |
| SCTP (kanały danych) | **usrsctp** | BSD-3 | **To jest najtrudniejszy element całego portu.** usrsctp buduje się z mingw, ale wymaga starannej konfiguracji (`SCTP_STDINT_INCLUDE` itp.). Alternatywa: libdatachannel (MPL-2.0, C++17 z bindingami C) = mniej kodu, więcej zależności i C++ w łańcuchu. |
| SRTP/SRTCP | **własna implementacja** (jak w OpenNOW) na AES-CTR + HMAC-SHA1 + AES-GCM z mbedTLS | MIT (Twój kod) | OpenNOW robi dokładnie tak (`aes`,`ctr`,`hmac`,`sha1`,`ghash`). Nie bierzemy libsrtp — mniej kodu i pełna kontrola nad oknem replay (`SRTP_REPLAY_WINDOW_PACKETS = 2048`). |
| Reed-Solomon FEC | własna tablicowa implementacja GF(2^8) | MIT | ~400 linii. Format NV jest w `nvst.rs` (`NV_VIDEO_PACKET_LEN=16`, `NVST_FEC_RTP_HEADER_ALLOWANCE=16`). **Można odłożyć na etap 2** — bez FEC działa, tylko gorzej znosi straty (a na tetheringu straty będą). |
| Dekoder H.264 | **A:** `libavcodec` z Twojego builda FFmpeg · **B:** **openh264** (źródło, `OpenH264API::from_source`) | **A: GPL** (Twój build jest `--enable-gpl`) · **B: BSD-2** | **A jest szybszy** (dojrzały asm SSSE3), **B jest czysty licencyjnie** i OpenNOW używa go jako fallback („bundled OpenH264 plus SDL", `docs/streamer-comparison/video.md`). Uwaga: oficjalny *binarny* openh264 Cisco jest okrojony do Constrained Baseline — **budujemy ze źródeł**, wtedy profil High (`maxCodecProfile:3`) działa. |
| Audio Opus | **libopus** | BSD-3 | Dekodowanie 48 kHz stereo na Core2 to ~1-2 % CPU. Bezproblemowe z mingw. |
| Wyjście audio | **WASAPI** (shared, event-driven) | — | Dostępny od Visty, więc Win7 OK. Bez SDL — mniej warstw, mniejsza latencja. |
| Prezentacja | **D3D9** (`IDirect3DSurface9` NV12/YUY2 + `StretchRect`) → fallback **GDI** | — | `d3d9.lib`/`ddraw` są w SDK mingw. **Do zweryfikowania testem `T4`** — Twój probe pokazał `has_d3d9: true`, ale `StretchRect` NV12→X8R8G8B8 ze skalowaniem na GMA4 to osobna historia. |
| Wejście | **Raw Input** + `SendInput`-free (wysyłamy do chmury, nie wstrzykujemy lokalnie) | — | Wzorzec 1:1 z `windows_raw_input.rs`. |
| JSON w C | **własny, ograniczony parser** | MIT | OpenNOW ma własne limity (`json_nodes:256`, `json_depth:32`, `json_string_bytes:4096` w Twoim ABI). SDP i tak nie jest JSON-em. ~600 linii, bez zależności. |

### Decyzja licencyjna — trzy spójne warianty

| Wariant | Skład | Licencja wyniku | Kiedy wybrać |
|---|---|---|---|
| **L1 „czysty"** | mbedTLS + libjuice/własny STUN + usrsctp + openh264 + libopus + własny SRTP/FEC | **MIT/BSD/MPL/Apache** — wszystko dozwolone w zamkniętym i otwartym | jeśli kiedykolwiek chcesz to opublikować bez ujawniania źródeł |
| **L2 „pragmatyczny"** | jak L1, ale dekod = **Twój build FFmpeg (GPL)** | **GPL-3** całości | jeśli liczysz każdy megacykl (FFmpeg h264 jest szybszy od openh264) i projekt zostaje prywatny/GPL |
| **L3 „hybryda"** | L1 + FFmpeg jako **opcjonalny, ładowany dynamicznie** moduł dekodera (`LoadLibrary`) | szara strefa; w praktyce GPL dla modułu | **nie rekomenduję** — to prawnicze gdybanie bez zysku |

**Moja rekomendacja: zacznij od L2** (masz FFmpeg już zbudowany i przetestowany na maszynie — zero ryzyka budowy), **zmierz różnicę openh264 vs FFmpeg w `T1`**, i jeśli openh264 mieści się w budżecie → przejdź na L1. Decyzję podejmujesz na podstawie pomiaru, nie ideologii.

---

## 8. Ryzyka — rejestr z oceną

| # | Ryzyko | P | S | Mitigacja / bramka |
|---|---|---|---|---|
| R1 | **CPU nie wyrabia 720p30** (dekod + CSC) | średnie | **krytyczne** | `T1` mierzy oba dekodery i obie konwersje. Fallback: 960×540@30, potem 640×360@30. Ostatecznie: projekt ma sens tylko jako „granie w 360p", co może być nieakceptowalne → **decyzja po `T1`**. |
| R2 | **D3D9 NV12 `StretchRect` odrzucony przez sterownik 8.15.10.2869** | wysokie | wysokie | `T4` testuje NV12→X8R8G8B8, YUY2, overlay, i mierzy ms/klatkę. Fallback łańcuchowy: NV12 → YUY2 → GDI. |
| R3 | **usrsctp/DTLS na mingw+Win7 nie chce się zbudować lub działać** | średnie | wysokie | Etap 0: osobny spike „hello DTLS+SCTP" *zanim* cokolwiek innego. Jeśli polegnie → libdatachannel (C++ z C API) jako plan B. |
| R4 | **Brak VT w conhost** → TUI | **pewne** | średnie | §5.3 opcja B (własny backend Win32). `T3` potwierdza stan faktyczny na Twojej maszynie (JG 2021 może coś zmieniać). |
| R5 | **Python 3.8 EOL** + brak kół cp38 dla nowych wersji bibliotek | pewne | średnie | Pinowane wersje, lokalny katalog wheeli, zero zależności z C-extensions (czysty `ctypes` + stdlib). DPAPI na tokeny. |
| R6 | **GCC 16.2.0 wygeneruje binarkę, która nie wystartuje na Win7** (domyślny `_WIN32_WINNT`/`WINVER`, nowe API w CRT) | średnie | **krytyczne** | `T2`: `-D_WIN32_WINNT=0x0601`, statyczne libgcc/libstdc++, `-Wl,--major-subsystem-version,6.01`, test x64 **i** x86 (32-bitowa binarka na 64-bitowym Win7 = dodatkowy margines kompatybilności i mniejszy ślad RAM). |
| R7 | **CloudMatch odrzuci klienta bez HW dekodera** | **niskie** | krytyczne | Dowód ze źródeł: kodek nie jest wysyłany do CloudMatch, a `clientRequestMonitorSettings` opisuje *monitor*, nie dekoder. Bramka: pierwszy `POST v2/session` z `streamerVersion:"14"` zwraca sesję. |
| R8 | **Serwer wymaga `nativeRtcOnBundlePort:1`** i seat-y bez bundle są niedostępne | niskie | średnie | OpenNOW już ma tę walidację i błąd `nvst-legacy-transport-unsupported`; powtarzamy komunikat zamiast wisieć. |
| R9 | **40 Hz panel** → nierówna kadencja 30 fps (3:2-like judder) | pewne | niskie | Prezentacja bez vsync (jak OpenNOW: `DXGI_PRESENT_ALLOW_TEARING`), liczymy na to, że 40 Hz vs 30 fps daje akceptowalny drift. Do oceny wizualnej, nie pomiaru. |
| R10 | **NVB type 10 (mouse accel/speed) — bajty nieznane** | pewne | niskie | OpenNOW też ich nie ma i działa. Rekonstruujemy `0x0323`, oznaczamy w kodzie jako `UNVERIFIED`. |
| R11 | **2 GB RAM: OOM przy starcie sesji** | średnie | wysokie | Budżet z §4.4; bufor gniazda 1-2 MiB zamiast 8 MiB; brak ffmpeg.exe jako procesu; pomiar `WorkingSet` w `T1`. |
| R12 | **Licencja FFmpeg (GPL) wciąga cały projekt** | pewne (jeśli L2) | niskie | Świadoma decyzja §7. Projekt prywatny → GPL nie przeszkadza. |
| R13 | Tethering USB 2.0: jitter i straty → artefakty bez FEC | wysokie | średnie | FEC Reed-Solomon planujemy od razu (etap 2), NACK-v2 z limitem 3 prób i 52 ms — jak w OpenNOW. `packetSize` zostawiamy 1280 (mieści się w MTU), nie 1500. |

---

## 9. Plan etapowy z bramkami

Każdy etap kończy się **mierzalną** bramką. Nie wchodzimy dalej, jeśli bramka nie przechodzi.

### Etap 0 — Pomiary (0 kodu produkcyjnego) · **BRAMKA DECYZYJNA**
Uruchamiasz `T1`-`T4` z `tests/`. Wyniki rozstrzygają:
- **G0.1** czy 720p30 mieści się w ≤ 70 % CPU z NV12→GPU (a nie CPU RGB) → jeśli nie: schodzimy na 540p30 i pytamy, czy projekt ma sens.
- **G0.2** czy D3D9 przyjmie NV12/YUY2 `StretchRect` i ile to kosztuje ms/klatkę.
- **G0.3** czy toolchain produkuje binarkę startującą na Win7.
- **G0.4** czy conhost ma VT (prawie na pewno nie) → wybór wariantu TUI.

### Etap 1 — Szkielet C + pętla wideo offline (bez sieci)
`knvstd --loopback`: czyta plik `.264`, dekoduje, prezentuje w oknie D3D9 1280×800 z letterboxem (`fit_rect`), liczy fps/latencję/CPU. Plus WASAPI z syntetycznymPCM.
**Bramka G1:** stabilne 30 fps przez 10 minut, CPU ≤ zmierzonego progu, brak wzrostu RSS.

### Etap 2 — Protokół: WSS+RTSP, Mjolnir, SRTP, GS header, AU assembly
Na razie **bez** DTLS/SCTP (czyli bez audio i bez wejścia). Tryb `--capture` zrzuca access unity; tryb `--decode` dekoduje je na żywo.
**Bramka G2:** `OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY` zwraca 200, a `NvstVideoReceiver` składa poprawne access unity (weryfikacja: `ffprobe` na zrzuconym `.264`), reorder/NACK-v2 działa przy sztucznie gubionych pakietach (własny test z `sendto` w pętli z dropem).

### Etap 3 — Bundle: ICE + DTLS + SCTP + Opus + wejście
Najtrudniejszy etap (R3). Zaczynamy od spike'a „DTLS handshake + jeden kanał SCTP + echo" poza jakimkolwiek kodem NVST.
**Bramka G3:** dźwięk słyszalny, a `0x020b`/`0x020d`/`0x0308`/`0x030d`/`0x0320(19)`/`0x0321` wysłane w kolejności z `features.md` → serwer przyjmuje wejście (kursor się rusza w chmurze).

### Etap 4 — Python: TUI + integracja
Backend konsolowy Win32 (`ctypes`), ekran konta, lista gier (katalog 2000 pozycji w pamięci, jak w Twoim ABI), ekran kolejki (`queuePosition` + `seatSetupStep`), launcher sesji, vault DPAPI, logi 2 MiB × 3 na kanał.
**Bramka G4:** pełna ścieżka: login → katalog → launch → kolejka → *streaming* → gra reaguje na klawiaturę → TEARDOWN → powrót do listy. Trzy razy z rzędu bez restartu.

### Etap 5 — Utwardzanie
FEC Reed-Solomon, recovery (IDR `0x0301`, RFI `0x0302`, invalidation), rekonect, `dynamicStreamingMode` (Data Saver), bitrate cap 3-6 Mbps, statystyki w TUI (prawdziwe: fps, loss, RTT z ICE pair — **nie** placeholdery, które OpenNOW sam przyznaje w „Gotchas").

---

## 10. Czego potrzebuję od Ciebie teraz

Uruchom na laptopie cztery skrypty z `tests/` (opis w `tests/README.md`). Kolejność ma znaczenie — `T1` i `T2` rozstrzygają najwięcej.

Najważniejsze pytania, na które odpowiedzą:

1. **`T1`** — ile fps realnie dekoduje ten CPU (ffmpeg vs openh264, 1 vs 2 wątki, z konwersją i bez)? → **rozstrzyga rozdzielczość docelową i sens projektu**
2. **`T2`** — czy GCC 16.2.0 z mingw produkuje binarkę, która *uruchamia się* na Win7, i jakie są domyślne `_WIN32_WINNT`/`WINVER`? → **rozstrzyga, czy w ogóle mamy toolchain**
3. **`T3`** — czy conhost na Twoim JG 2021 ma VT, alt-screen, ile kolorów, jaki font i czy `ReadConsoleInputW` daje poprawne zdarzenia myszy? → **rozstrzyga wariant TUI**
4. **`T4`** — czy D3D9 na tym sterowniku przyjmie NV12/YUY2 `StretchRect` ze skalowaniem i ile to kosztuje? → **rozstrzyga ścieżkę prezentacji**

Do tego trzy decyzje, które możesz podjąć od razu albo po pomiarach (pytam w formularzu obok): wariant licencji (L1/L2), wariant TUI (A/B), i docelowa rozdzielczość, którą uznasz za „grywalną".

---

## Załącznik A — czego w tej analizie **nie** rozstrzygnąłem (uczciwa lista)

| Pytanie | Stan | Jak rozstrzygniemy |
|---|---|---|
| Czy GMA 4500MHD dekoduje AVC sprzętowo | **rozstrzygnięte: NIE** (Twój probe: `QI(IDirectXVideoDecoderService)=E_NOINTERFACE`) | zamknięte |
| Czy D3D9 `StretchRect` zrobi NV12→RGB ze skalowaniem na tym sterowniku | **NIEZNANE** | `T4` |
| Ile fps dekoduje T7250 | **NIEZNANE** (mam tylko rachunek rzędu wielkości) | `T1` |
| Czy GCC 16.2.0 celuje domyślnie w Win7 | **NIEZNANE** | `T2` |
| Czy JG 2021 coś zmienia w conhost | **NIEZNANE** | `T3` |
| Czy openh264 ze źródeł dekoduje High Profile poprawnie na Meromie | **NIEZNANE** | `T1` (wariant B) |
| Czy CloudMatch przydzieli sesję naszemu klientowi | **prawdopodobnie TAK** (§8 R7, dowód ze źródeł) | pierwszy `POST v2/session` w etapie 2 |
| Dokładne bajty NVB type 10 | **NIEZNANE i nieosiągalne** (OpenNOW też nie ma) | akceptujemy `0x0323` jako `UNVERIFIED` |
| Format RS-FEC NVIDIA w szczegółach (macierz, rozmieszczenie pakietów naprawczych) | **częściowo** — stałe są w `nvst.rs`, pełny algorytm trzeba przeczytać (~600 linii) | lektura `nvst.rs` w etapie 5 |

## Załącznik B — najcenniejsze pliki w OpenNOW dla tego portu

Czytaj w tej kolejności:

1. `docs/streamer-comparison/video.md` — cały tor wideo, łącznie z zachowaniem oficjalnego klienta
2. `docs/streamer-comparison/features.md` — **bajtowe ramki kontrolne NVB** i sekwencja aktywacji
3. `docs/mjolnir-nack-v2.md` — format NACK-v2 `0x0317`
4. `docs/streamer-comparison/audio.md` — Opus/RED/jitter/WASAPI
5. `docs/streamer-comparison/mouse-input.md` + `controller-input.md` — wejście
6. `docs/streamer-comparison/rtsp-negotiation.md` — kolejność i bramkowanie PLAY
7. `docs/session-start-progress.md` — `seatSetupStep` (ekran kolejki w TUI)
8. `docs/core-protocol.md` — wzorzec IPC JSON-lines do skopiowania
9. `native/opennow-streamer/crates/opennow-streamer-transport/src/nvst.rs` — SRTP/GS/FEC/reorder
10. `native/opennow-streamer/crates/opennow-streamer-core/src/nvst_rtsp.rs` — RTSP + pełny ANNOUNCE SDP
11. `native/opennow-core/src/cloudmatch.rs:1643` — `build_create_body` (ciało POST v2/session)
12. `native/opennow-streamer/crates/opennow-streamer-platform/src/windows_raw_input.rs` — wzorzec Raw Input

Licencja OpenNOW: **MIT** (`LICENSE`, „Copyright (c) 2025 Zortos") — przepisywanie i czerpanie z wiedzy jest dozwolone z zachowaniem noty.
