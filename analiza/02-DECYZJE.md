# Decyzje projektowe (register)

Zasada: każda decyzja ma **powód** i **warunek cofnięcia**. Nic nie jest „bo tak".

| # | Decyzja | Powód | Warunek cofnięcia |
|---|---|---|---|
| **D1** | **Host konsoli: ConEmu jest wymagany.** TUI = Textual/Rich bez zmian, z truecolor i myszą SGR. Klient przy starcie **wykrywa host** i jeśli VT nie działa — kończy z czytelnym komunikatem zamiast rysować krzaki. | T3 zmierzył, że ANSI działa (atrybuty `0x4`/`0x27` zastosowane, ESC skonsumowany), ale `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` zwraca `ERROR_INVALID_PARAMETER` — czyli to **nie** conhost parsuje. Poszlaki wskazują ConEmu (klucze w `HKCU\Console`, `GetNumberOfConsoleMouseButtons()=16`, bufor 105×32766). ConEmu jest już zainstalowany (`C:\Program Files\ConEmu`), więc koszt = skrót na pulpicie. Oszczędza ~600 linii własnego backendu. | Jeśli T5_wynik_2 (czysty `cmd.exe`) pokaże, że VT działa **też** w conhost → wymóg ConEmu znika (ale kod zostaje, bo nie szkodzi). Jeśli T5 pokaże, że ConEmu **nie** daje truecolor/myszy SGR → wracamy do dyskusji o backendzie Win32. |
| **D2** | **Docelowy tryb obrazu: decyduje pomiar T1.** Plan A 720p30, plan B 540p60, plan C 360p30. Klient negocjuje to w `ANNOUNCE` (`clientViewportWd/Ht`, `maxFPS`), więc przełącznik jest tani i odwracalny. | Panel jest teraz 60 Hz, więc znika argument „i tak nie wyświetlisz >40 kl/s". 720p60 = 55,3 Mpx/s = 83-138 % budżetu → poza zasięgiem. 720p30 = 41-69 % → granicznie, musi potwierdzić pomiar. | — (decyzja procesowa, nie techniczna) |
| **D3** | **Dekoder H.264: najpierw mierzymy oba** (T1 = ffmpeg/libavcodec, T6 = openh264 ze źródeł), potem wybieramy. Domyślny kierunek: openh264 (BSD-2) jeśli mieści się w budżecie, FFmpeg (GPL) jeśli jest istotnie szybszy i projekt zostaje prywatny. | Różnica jest niemierzalna „na oko": ffmpeg `h264` ma dojrzały asm SSSE3, openh264 ma własny asm składany przez **NASM** (niezależnie od `-march`). Na Meromie bez SSE4/AVX wynik może się różnić w obie strony. | — |

---

## Ustalenia techniczne wynikające z pomiarów (nie decyzje, tylko fakty)

### F1 — kanoniczny zestaw flag kompilatora

T2 wykazał dwie rzeczy, które muszą być wymuszone **globalnie** (nasz kod i każda
vendored biblioteka):

```
-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000
-march=core2 -msse3 -O2
-static -static-libgcc -static-libstdc++
-Wl,--major-subsystem-version,6.01 -Wl,--minor-subsystem-version,01
-Wl,--major-os-version,6.01 -Wl,--minor-os-version,01
```

| Bez tego | Co się stanie |
|---|---|
| `-D_WIN32_WINNT=0x0601` | mingw-w64 v14 domyślnie daje **0x0A00 (Win10)** na x64 i **0x0501 (WinXP)** na x86 → biblioteki warunkujące kod na tej macrosie wciągną `WaitOnAddress`, `GetSystemTimePreciseAsFileTime` itd. |
| `-march=core2 -msse3` | domyślnie `sse3=0 ssse3=0` → nasze pętle (NV12→YUY2, depaketyzacja, SRTP) zostaną na SSE2/C. Strata 20-40 %. |
| `-static-libstdc++`/`-static-libgcc` | zależność od `libstdc++-6.dll` z w64devkit, której na czystym Win7 nie ma |
| `--major-subsystem-version,6.01` | PE nagłówek mówi 5.2 (x64) / **4.0** (x86). Działa, ale to nieprawda w metadanych i może mylić narzędzia |

**Zakaz:** `-march=native` gdziekolwiek poza maszyną E5500. `-march=core2` jest bezpieczny
dokładnie dlatego, że nie generuje nic powyżej SSSE3 — a T7250 ma dokładnie SSE3+SSSE3.

### F2 — cel: x86_64

T2: obie architektury działają. x64 ma lepsze wartości domyślne (PE subsys 5.2 vs 4.0),
16 rejestrów XMM (istotne dla pętli NV12 i depaketyzacji RTP) i spójność z Pythonem 3.8 x64.
x86 zostaje jako plan B, gdyby RSS okazał się krytyczny przy 2 GB RAM (kod ten sam).

Sterowniki potwierdzone z wnętrza binarki: `igdumd64.dll` (x64) / `igdumdx32.dll` (x86),
`8086:2A42`, `d3d9_create=ok`, `wsastartup=0`.

### F3 — środowisko Pythona

```
3.8.20 (default, Jun 5 2026) [MSC v.1951 64 bit (AMD64)]   C:\Python\Python38\python.exe
ssl: OpenSSL 1.1.1w  11 Sep 2023
pip list: pip 25.0.1, setuptools 75.3.4     <- NIC wiecej
```

- Sufit „Python 3.8" potwierdzony (3.9+ nie działa na Win7), ale mamy **3.8.20**, nie 3.8.10.
- OpenSSL **1.1.1w** — nowszy niż w stockowym 3.8.10; spójne z Twoim wynikiem
  `POST login.nvidia.com/device/authorize → 400` (TLS działa).
- **Brak `rich` i `textual`** → potrzebny plan instalacji: pinowane wersje cp38 +
  lokalny katalog wheeli (ryzyko R15). To jest osobne zadanie, nie „pip install textual".

### F4 — wydajność konsoli nie jest ograniczeniem

| Pomiar T3 | Wartość |
|---|---|
| `WriteConsoleOutputW` pełne 80×25 | **0,1 ms** (~10 000 fps) |
| jedna linia | 0,05 ms |
| `WriteConsoleOutputCharacterW` jedna linia | 0,17 ms |
| zdarzenia myszy w 20 s | 146 (ruch `flags=1`, LPM `buttons=1`) |
| zdarzenia klawiatury | 25, VK 13/37-40/65/90/112 |
| Unicode round-trip (ramki, bloki, `▶✓`) | **identyczny**, 0 × `?` |
| `CreateConsoleScreenBuffer` + `SetConsoleActiveScreenBuffer` | działa (create/set/restore = true) |

Wniosek: nawet gdybyśmy musieli pisać własny renderer, pełne przepisanie ekranu 60×/s
kosztowałoby 6 ms/s. **TUI nie będzie wąskim gardłem.** Mysz działa → klikalna lista gier
jest realna.

Zastrzeżenie: `HKCU\Console` ma `FaceName = null` dla profilu domyślnego = **font rastrowy**,
który nie ma glifów ramek Unicode. Bufor przyjmuje kody poprawnie (stąd round-trip OK),
ale **na ekranie** mogą być prostokąty. Do potwierdzenia wizualnie; jeśli tak — albo
ustawiasz `Lucida Console`/`Consolas`, albo TUI dostaje przełącznik na ramki ASCII.

### F5 — openh264 wymaga NASM, i to jest twarde

`meson.build` openh264 (master, v2.7.0), linia ~152:

```meson
if cpu_family in ['x86', 'x86_64']
  nasm = find_program('nasm')      # <-- BEZ required:false
  use_asm_gen = true
  asm_gen = generator(nasm, output : '@BASENAME@.o',
                      arguments : ['-f', asm_format, '-i', asm_inc, '@INPUT@', '-o', '@OUTPUT@'] + asm_args)
```

`find_program('nasm')` bez `required: false` = **brak NASM przerywa konfigurację meson**,
a nie „buduje wolniej bez asm". Więc:

- **T6 bez zainstalowanego NASM się nie odbędzie.** Trzeba `nasm >= 2.10.06` (README openh264).
- To **nie jest** wada — bez asm-x86 openh264 na Meromie byłby kilkukrotnie wolniejszy
  i pomiar nic by nie mówił o ścieżce, której realnie używa OpenNOW
  (`openh264-rs` z `features=["source"]` kompiluje asm przez `nasm-rs`).

Dobra wiadomość z tego samego pliku: gałąź **Windows x86_64 NIE dodaje `-DHAVE_AVX2`**
(w przeciwieństwie do gałęzi unixowych, które dodają `-DHAVE_AVX2` dla `x86_64`).
Windows x64 dostaje tylko `asm_format='win64'` i `-DWIN64`. Czyli **na Windows nie ma
ryzyka, że openh264 wstawi instrukcje AVX2, których Merom nie ma**. To domyka jedno
z ryzyk, które rozważałem w rundzie I (R3 w `00-WERDYKT.md`, o AVX w nowoczesnych buildach).

Konsekwencja pomiarowa: **NASM nie dostaje `-march`** — pliki `.asm` są składane niezależnie
od flag kompilatora, a wybór jąder (SSE2/SSSE3/AVX2) odbywa się przez `X86_ASM` + dispatch
w czasie wykonania. Dlatego:
- `-march=core2 -msse3` wpływa **tylko** na część C/C++ openh264 (i na nasz kod),
- **T1 (ffmpeg) i T6 (openh264) są pomiarami komplementarnymi, nie zamiennymi** —
  ffmpeg buduje swój asm przez własne `.asm`/`yasm` z configiem CPU, openh264 przez NASM.
  Porównanie ich fps-ów na tym samym klipie jest uczciwe, ale *przyczyna* różnicy będzie inna.

### F6 — odświeżanie: 60 Hz, ale pilnujemy DPST

Zmiana 40 → 60 Hz w panelu Intela. Objaw „przywróciło się samo" na Intel GMA 4 na laptopie
to klasycznie **Intel Display Power Saving Technology** (na zasilaniu z baterii zbija
podświetlenie i częstotliwość odświeżania) albo profil zasilania AC/DC.

Dlaczego to ryzyko projektowe, nie ciekawostka: jeśli DPST zbije 60 → 40 **w trakcie sesji**,
pętla prezentacji liczona na 60 Hz zacznie gubić kadencję i dostaniesz judder nie do
zdiagnozowania bez pomiaru. Dlatego:

1. T4 ma teraz `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` → bieżący `dmDisplayFrequency`
   oraz **pełną listę trybów 1280×800** (`mode_1280x800_at_60hz_available`).
2. Klient C **nie zakłada stałego Hz**: mierzy interwał między `Present` i adaptuje
   (wzorzec z OpenNOW: `PresentationClock` ignoruje timestampy RTP i *uczy się* kadencji
   przyjazdu klatek — `docs/streamer-comparison/video.md` pkt 8).
3. Opcja do rozważenia: wymusić 1280×800@60 przez `ChangeDisplaySettingsW` na starcie sesji
   i przywrócić po `TEARDOWN`. **Zostawiam jako opcję jawną** — zmienia stan systemu, więc
   musi być włączana świadomie i odwracalna.
4. Do wyłączenia po Twojej stronie: DPST w panelu Intela dla trybu DC.

---

## Status bramek

| Bramka | Stan | Czego brakuje |
|---|---|---|
| G0.3 toolchain produkuje binarki działające na Win7 | ✅ **ZALICZONA** | — |
| G0.4 konsola/VT → wariant TUI | 🟡 częściowo (D1 podjęta warunkowo) | `T5_wynik_1.json` + `T5_wynik_2.json` |
| G0.1 CPU: czy 720p30 mieści się w ≤70 % | ⏳ | `T1` (powtórka — plik był ucięty) |
| G0.2 D3D9: czy NV12 `StretchRect` ze skalowaniem działa | ⏳ | `T4` (powtórka — błąd kompilacji naprawiony) |
| G0.5 openh264 vs ffmpeg | ⏳ | **NASM** + `T6` |
