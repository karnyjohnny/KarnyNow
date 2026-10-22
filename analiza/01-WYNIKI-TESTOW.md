# Runda II — wyniki testów z maszyny i co z nich wynika

**Data:** 2026-10-20 · **Wejście:** `T1_wynik.json` (ucięty), `T2_wynik.json`, `T3_wynik.json`, `T4_wynik.json` (błąd kompilacji) + Twoja uwaga o odświeżaniu 40 → 60 Hz.

---

## 0. Tabela wyników

| Test | Status | Rozstrzygnięcie |
|---|---|---|
| **T2** toolchain | ✅ **zaliczony** | GCC 16.2.0 + mingw-w64 v14 **produkuje binarki działające na Win7 x64**. Ale domyślnie celuje w **Windows 10** i **nie włącza SSE3/SSSE3** — obie rzeczy musimy wymusić flagami. |
| **T3** konsola | ✅ **zaliczony, wynik lepszy niż oczekiwano** | **ANSI/VT DZIAŁA** (16 kolorów potwierdzone pomiarem atrybutów, alt-screen + adresowanie kursora też). Mysz i klawiatura przez `ReadConsoleInputW` działają. Unicode round-trip OK. Zapis pełnego ekranu 80×25 = **0,1 ms**. |
| **T1** CPU/dekod | ⚠️ **do powtórki** | Plik ucięty przy 15 008 znaków — zachował się tylko wariant 360p30. Naprawiłem skrypt (kompaktowy JSON + osobny `T1_skrot.txt`). |
| **T4** D3D9 | ❌ **błąd kompilacji → naprawiony, do powtórki** | mój `.c` używał pól/makra, których nie ma w `d3d9.h` mingw-w64. Zweryfikowałem wszystkie nazwy w prawdziwym nagłówku i przepisałem. |
| **T5** (nowy) | 🔜 **do uruchomienia ×2** | Musimy ustalić, **kto** parsuje ANSI: ConEmu (którego masz zainstalowanego) czy sam conhost. Od tego zależy, czy TUI zadziała też w czystym `cmd.exe`. |

**Bramka G0.3 (toolchain) — ZALICZONA.**
**Bramka G0.4 (konsola) — ZALICZONA warunkowo** (czekam na T5).
**Bramki G0.1 (CPU) i G0.2 (D3D9) — otwarte**, potrzebuję powtórek T1 i T4.

---

## 1. T2: toolchain — co dokładnie zmierzyliśmy

### 1.1 Wszystko się kompiluje i **uruchamia** na Win7

15 wariantów (3 kompilatory × 5 zestawów flag), każdy `compile_rc=0`, każdy `run_rc=0` i każdy zwrócił komplet danych z wnętrza procesu:

```
runtime_os = 6.1.7601          <- Windows 7 SP1, zgadza się
d3d9_create = ok               <- D3D9 żyje
d3d9_vendor_device = 8086:2A42 <- GMA 4500MHD, potwierdzone z wnętrza binarki
d3d9_driver = igdumd64.dll     <- user-mode driver D3D (x64); x86: igdumdx32.dll
wsastartup = 0                 <- WinSock2 działa (UDP Mjolnir)
screen = 1280x800
```

`hard_blockers_present = []` i `unknown_dlls = []` **dla każdego wariantu**. Importy to wyłącznie
`d3d9.dll, KERNEL32.dll, msvcrt.dll, USER32.dll, WS2_32.dll` — wszystko obecne na Win7.

**Ryzyko R6 z rejestru ryzyk spada z „średnie/krytyczne" na „zamknięte".**

### 1.2 Pułapka nr 1: mingw-w64 v14 domyślnie celuje w **Windows 10**

Pomiar makr przy pustym wejściu (`gcc -dM -E -x c nul`) — kolumna `default_macros`
**nie zawiera** `_WIN32_WINNT`, a program diagnostyczny wydrukował wartość faktyczną:

| Kompilator | wariant | `_WIN32_WINNT` | `WINVER` | `NTDDI_VERSION` | PE `subsys` |
|---|---|---|---|---|---|
| x86_64 | `default` | **0x0A00 (Win10)** | 0x0A00 | 0x0A000010 | 5.2 |
| x86_64 | `win7` | 0x0601 | 0x0601 | 0x06010000 | 5.2 |
| x86_64 | `win7_subsys601` | 0x0601 | 0x0601 | 0x06010000 | **6.1** |
| i686 | `default` | **0x0501 (WinXP!)** | 0x0501 | 0x05010000 | **4.0** |
| i686 | `win7` | 0x0601 | 0x0601 | 0x06010000 | 4.0 |

**Konsekwencja (twarda, do wpisania w CMake/Makefile):**

```make
CFLAGS += -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000
LDFLAGS += -Wl,--major-subsystem-version,6.01 -Wl,--minor-subsystem-version,01 \
           -Wl,--major-os-version,6.01 -Wl,--minor-os-version,01
CFLAGS += -static -static-libgcc
```

Bez tego każda biblioteka, która warunkuje kod na `_WIN32_WINNT`, skompiluje się
w wariancie **Windows 10** — i dostaniemy `WaitOnAddress`/`GetSystemTimePreciseAsFileTime`
w importach. Tym razem się nie zdarzyło (bo probe był mały), ale przy usrsctp/mbedTLS
**zdarzy się na pewno**.

### 1.3 Pułapka nr 2: **domyślnie nie ma SSE3 ani SSSE3**

```
default      : compiled_sse3=0  compiled_ssse3=0  compiled_avx=0
win7         : compiled_sse3=0  compiled_ssse3=0  compiled_avx=0
win7_core2   : compiled_sse3=1  compiled_ssse3=1  compiled_avx=0   <- jedyny poprawny
```

x86_64 baseline to SSE2. Merom **ma** SSSE3, ale kompilator go nie użyje, dopóki nie
powiemy `-march=core2 -msse3`.

**Konsekwencja krytyczna dla wydajności:** to dotyczy **nie tylko naszego kodu**.
Każdą bibliotekę zewnętrzną (openh264, libopus, usrsctp, mbedTLS, ew. FFmpeg) musimy
zbudować **z tymi samymi flagami**, inaczej ich pętle SIMD zostaną na SSE2 albo na C.
Przy dekodowaniu H.264 różnica SSSE3 vs SSE2 to realnie 20-40 %.

Ustalamy więc kanoniczny zestaw flag projektu:

```
-march=core2 -msse3 -O2 -fno-plt -static -static-libgcc
-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000
```

`-march=core2` jest **bezpieczny**: nie generuje niczego powyżej SSSE3, czyli dokładnie
to, co T7250 ma. **Nie wolno** użyć `-march=native` na maszynie developerskiej innej niż E5500.

### 1.4 x64 czy x86?

Oba działają. x86 ma **gorsze** domyślne (`_WIN32_WINNT=0x0501`, PE subsys 4.0) i ładuje
`igdumdx32.dll` zamiast `igdumd64.dll`. Argumenty za x64: lepszy stan domyślny, 16 rejestrów
XMM (istotne dla naszych pętli NV12→YUY2 i depaketyzacji), spójność z Pythonem 3.8 x64.
Argument za x86: mniejszy ślad pamięci (~10-15 %) przy 2 GB RAM.

**Rekomendacja: x64**, z `-static`. Decyzja do potwierdzenia po T1 (jeśli RSS będzie
krytyczny, x86 zostaje jako plan B — kod jest ten sam).

### 1.5 Drobne poprawki do odnotowania

- `mingw64 = 14.0`, `gcc = 16.2.0`, toolchain to **w64devkit** (`C:\Programy\w64devkit`).
- Ostrzeżenia `-Wformat` w moim probe (`%04X` dla `DWORD` = `long unsigned int`) —
  kosmetyka, ale w kodzie produkcyjnym włączamy `-Werror=format` i pilnujemy `%lu`.
- `ran_ok=False` w raporcie to **artefakt mojego skryptu**: `PROBE_OK` nie ma `=`, więc
  nie wpadł do słownika `run_kv`. Binarki działały (`run_rc=0`, pełne dane). Nie jest to
  sygnał problemu.

---

## 2. T3: konsola — wynik, którego się nie spodziewałem

### 2.1 VT działa. Dowód pomiarowy, nie domysł

```
chars_first_60 = "CZERWONY ZIELONE_TLO BOLD            ..."   <- ESC ZNIKNĄŁ
attrs_first_16 = 0x4 ×8, 0x7, 0x27 ×7, ...
baseline_attr_before_write = 0x7
escape_sequences_visible_as_text = false
color_applied_via_ansi = true
```

Rozkodowanie atrybutów (to jest najmocniejsza część dowodu):
- `0x4` = FOREGROUND_RED → `\x1b[31m` **zastosowany** na 8 znakach „CZERWONY"
- `0x27` = `0x20` (BACKGROUND_GREEN) + `0x07` → `\x1b[42m` **zastosowany** na „ZIELONE_TLO"
- `\x1b[1mBOLD` → `0x7` (w 16-kolorowym modelu bold = jasny odpowiednik, tu biały)

Czyli conhost/host **konsumuje** sekwencje i **ustawia atrybuty**. To nie jest „może zadziała".

### 2.2 A jednocześnie `SetConsoleMode(VT)` zawodzi

```
output_mode = 0x3                (PROCESSED_OUTPUT | WRAP_AT_EOL)
set_vt_call_ok = false
vt_flag_persisted = false
last_error_if_failed = 87        (ERROR_INVALID_PARAMETER)
```

**To jest kombinacja niemożliwa dla stockowego conhost Windows 7.** Stock conhost z Win7
nie zna flagi `ENABLE_VIRTUAL_TERMINAL_PROCESSING` (stąd `ERROR_INVALID_PARAMETER`) **i**
nie parsuje ANSI. Tu flagi nie zna, ale ANSI parsuje → **ktoś inny przechwytuje API konsoli**.

Poszlaki wskazujące na ConEmu:

| Poszlaka | Wartość | Interpretacja |
|---|---|---|
| `HKCU\Console` subkeys | `C:_Program Files_ConEmu_ConEmu_ConEmuC64.exe`, `ConEmu`, `Git Bash`, `Git CMD` | **ConEmu jest zainstalowany** w `C:\Program Files\ConEmu` |
| `GetNumberOfConsoleMouseButtons()` | **16** | conhost zwraca 2; 16 to wartość z hooka |
| `buffer_size` | 105 × **32766** | ogromny scrollback — typowe dla ConEmu, nie dla cmd |
| `input_mode_before` | `0x1a7` | zawiera bity spoza standardowego zestawu cmd |
| `window_cells` | 105 × 28 | ConEmu default |

**Hipoteza robocza (wysoka pewność, ale niepotwierdzona):** test leciał w ConEmu, a ConEmu
wstrzykuje `ConEmuHk64.dll` do procesów potomnych i przechwytuje `WriteConsole*`, sam
parsując ANSI. Stąd VT bez flagi VT.

**Dlaczego to jest ważne:** jeśli tak jest, to **w czystym `cmd.exe` ANSI nie zadziała**
i Textual pokaże krzaki — dokładnie jak w Textualize/textual#3378 na tej samej konfiguracji
[1](https://github.com/Textualize/textual/issues/3378). T5 rozstrzyga to wprost: sprawdza
zmienne `ConEmu*`, **listę modułów załadowanych w procesie** (szuka `ConEmuHk64.dll`),
drzewo procesów i klasę okna konsoli.

### 2.3 Niezależnie od T5 — te wyniki są dobre

| Pomiar | Wartość | Znaczenie dla projektu |
|---|---|---|
| `CreateConsoleScreenBuffer` + `SetConsoleActiveScreenBuffer` | **działa** (create/set/restore = true) | Możemy mieć „czysty ekran" i przywrócić zawartość po wyjściu **bez** `\x1b[?1049h`. To działa nawet w czystym conhost. |
| `WriteConsoleOutputW` pełne 80×25 | **0,1 ms** (≈10 000 fps) | Renderer nie jest wąskim gardłem. Możemy przepisywać cały ekran 60×/s i nadal zostanie ~99 % budżetu. |
| jedna linia | 0,05 ms | |
| `ReadConsoleInputW` klawiatura | 25 zdarzeń, VK = 13, 37-40, 65, 90, 112 | strzałki, Enter, F1, A, Z — pełna nawigacja po liście gier |
| `ReadConsoleInputW` mysz | **146 zdarzeń**, `flags=1` (ruch), `buttons=1` (LPM) | klikalna lista gier działa; ruch myszy raportowany |
| Unicode round-trip | **identyczny**, 0 znaków `?` | ramki `─│┌┐└┘`, bloki `█░▒▓`, `▶✓` przechodzą przez bufor (czy font je rysuje — osobna kwestia, patrz §2.4) |
| `resize` events | 0 | w teście nie zmieniałeś rozmiaru okna — do potwierdzenia przy okazji |

### 2.4 Kodowanie i font

```
input_cp = 852, output_cp = 852     <- CP852 (DOS Latin-2), sensowne dla PL
stdout_encoding = utf-8, filesystem_encoding = utf-8
ScreenBufferSize (rejestr) = 19660880, WindowSize = 1638480
FaceName (default) = null, FontSize = 0
"Git CMD" -> FaceName = "Lucida Console"
```

`FaceName = null` dla domyślnego profilu oznacza **font rastrowy** — a ten **nie ma glifów**
ramek Unicode. Bufor przyjmuje kod znaku poprawnie (stąd round-trip OK), ale na ekranie
możesz widzieć prostokąty. **Do sprawdzenia wizualnie**: czy w T3 linijka z `─│┌┐└┘ █░▒▓ ▶ ✓`
wyglądała poprawnie, czy jak krzaki?

Jeśli krzaki → albo ustawiasz `Lucida Console`/`Consolas` w profilu konsoli, albo TUI używa
ramek ASCII (`+--+`, `|`). Projektuję TUI tak, żeby **obie wersje miały ten sam layout**
(przełącznik `--ascii-boxes`), więc to nie jest blokada.

### 2.5 Środowisko Pythona — lepsze niż zakładałem

```
version    = 3.8.20 (default, Jun 5 2026) [MSC v.1951 64 bit (AMD64)]
executable = C:\Python\Python38\python.exe
ssl        = OpenSSL 1.1.1w  11 Sep 2023
```

- **3.8.20**, nie 3.8.10 — czyli masz build nowszy niż ostatni oficjalny z instalatorem dla
  Win7, i on **działa**. Sufit „3.8" potwierdzony, ale margines większy.
- **OpenSSL 1.1.1w** (wrzesień 2023) — nowszy niż w stockowym 3.8.10 (1.1.1k). TLS 1.2/1.3
  dostępne, spójne z Twoim wcześniejszym wynikiem `POST login.nvidia.com/device/authorize → 400`.
- **`pip list` = 2 pakiety: pip 25.0.1, setuptools 75.3.4.** `rich` i `textual` **nie są
  zainstalowane**. To znaczy, że warstwę TUI dopiero zbudujemy — i że musisz mieć plan
  instalacji zależności na maszynie offline/EOL (pinowane wersje cp38 + lokalny katalog wheeli).

---

## 3. Twoja poprawka: 40 Hz → 60 Hz

Dzięki, to istotne — i ma **dwie** konsekwencje, nie jedną.

### 3.1 Zmiana budżetu CPU (na niekorzyść)

W `00-WERDYKT.md` §4.3 potraktowałem 40 Hz jako „prezent od sprzętu": skoro i tak nie
wyświetlisz więcej niż 40 kl/s, celowanie w 60 fps jest marnowaniem CPU. **Ten argument
znika.** Przy 60 Hz naturalnym celem staje się **720p60**, a to zupełnie inna liga:

| Tryb | Piksele/s | Dekod @1,5 cy/px | Dekod @2,5 cy/px | Ocena |
|---|---|---|---|---|
| 1280×720 @30 | 27,6 Mpx/s | 41 % | 69 % | granicznie |
| 960×540 @60 | 31,1 Mpx/s | 47 % | 78 % | ryzykownie |
| **1280×720 @60** | **55,3 Mpx/s** | **83 %** | **138 %** | **prawdopodobnie za dużo** |

Czyli realny wybór to **720p30** (płynność 30 fps na panelu 60 Hz = równy podział 2:1,
bez judder) albo **540p60** (płynniej, mniejszy obraz). **T1 rozstrzygnie, które.**
Podkreślam: 720p30 na panelu 60 Hz jest *lepsze* niż na 40 Hz — brak nierównej kadencji.

### 3.2 Dlaczego samo się przywróciło — i dlaczego to groźne

Objaw „ustawiłem 60, wróciło na 40" na Intel GMA 4 na laptopie to klasycznie jedna z dwóch rzeczy:

1. **Intel Display Power Saving Technology (DPST)** — w panelu Intela: *„Wyświetlanie →
   Ustawienia zasilania → Technologia oszczędzania energii wyświetlacza"*. Na zasilaniu
   z baterii obniża podświetlenie **i** częstotliwość odświeżania. Wyłącz to dla trybu DC.
2. **Profil zasilania / tryb grafiki** — przełączenie AC↔DC potrafi przywrócić inny tryb.

**Dlaczego to jest ryzyko projektowe, a nie ciekawostka:** jeśli DPST zbije odświeżanie
z 60 na 40 **w trakcie sesji**, nasza pętla prezentacji (liczona na 60 Hz) zacznie gubić
kadencję i dostaniesz judder, którego nie zdiagnozujesz bez pomiaru. Dlatego:

- T4 ma teraz **nową sekcję**: `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` → bieżący
  `dmDisplayFrequency`, plus **pełna lista trybów 1280×800** z `dmBitsPerPel` i Hz
  → dowiemy się, czy 60 Hz jest w ogóle osiągalne dla tej rozdzielczości i ilu bitów.
- W kliencie C **nie zakładamy stałego Hz**: mierzymy interwał między `Present` i adaptujemy
  (`PresentationClock` w OpenNOW robi podobnie — ignoruje timestampy RTP i *uczy się* kadencji
  przyjazdu klatek, `docs/streamer-comparison/video.md` pkt 8).
- Do rozważenia: na starcie sesji wymusić tryb 1280×800@60 przez `ChangeDisplaySettingsW`
  i przywrócić po `TEARDOWN`. Zostawiam jako opcję — zmienia stan systemu, więc musi być
  jawna i odwracalna.

---

## 4. T4: co było nie tak i co poprawiłem

Kompilacja padła na x64 i x86. Przyczyny (wszystkie po mojej stronie — zgadywałem nazwy
zamiast je sprawdzić):

| Błąd | Przyczyna | Poprawka |
|---|---|---|
| `unknown type name 'D3DUSAGE'` | w mingw-w64 `D3DUSAGE_*` to stałe (`__MSABI_LONG(0x800)`), **nie ma typu** `D3DUSAGE` | `static const DWORD usages[]` + rzut `(D3DUSAGE)` w miejscu użycia |
| `'D3DCAPS9' has no member named 'DeviceCaps'` | pola nazywają się `Caps` (D3DCAPS_*) i **`DevCaps`** (D3DDEVCAPS_*), a `DevCaps2` osobno | `caps.Caps` / `caps.DevCaps` / `caps.DevCaps2` / `caps.PresentationIntervals` |
| `macro 'IDirect3D9_CheckDeviceFormatConversion' passed 6 arguments, but takes just 5` | makro jest 5-arg `(p,a,b,c,d)`, a **API nie ma parametru DisplayFormat** (sprawdzone: `STDMETHOD(CheckDeviceFormatConversion)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)`) | usunąłem `dm.Format` z wywołania |
| `implicit declaration of 'IDirect3D9_CheckDeviceOverlayType'` | **tej funkcji w ogóle nie ma** w `d3d9.h` mingw-w64 (`grep -c` = 0) | sekcję overlay usunąłem; w jej miejsce `EnumDisplaySettingsW` (patrz §3.2) |
| `caps.StretchRectFilterCaps` | istniało, ale go nie czytałem | **dodałem** — mówi, czy `StretchRect` umie LINEAR/POINT (bez tego nie wiemy, czy skalowanie będzie rozmazane) |

Wszystkie nazwy zweryfikowałem w prawdziwych nagłówkach mingw-w64 (`d3d9.h`, `d3d9types.h`,
`d3d9caps.h` z gałęzi master), które pobrałem do `src/hdr_*`. Dodałem też:

- `Caps`, `Caps2`, `DevCaps`, `hw_rasterization`, `vertex_shader_version`,
  `pixel_shader_version`, `num_simultaneous_rts`
- `stretch_rect_filter_caps` + `srf_none/point/linear`
- bieżący tryb ekranu i lista trybów 1280×800 (`mode_1280x800_at_60hz_available`)
- **usunąłem** cały blok DXVA2 z ręcznymi indeksami vtable — Twój probe platformy już to
  rozstrzygnął (`QI(IDirectXVideoDecoderService)=0x80004002`), a ręczne indeksy COM to
  ryzyko crasha bez nowej informacji. Uczciwie: indeksy, których użyłem, były **błędne**
  (`GetDecoderProfileCount` to 11, nie 8), więc dobrze, że blok wyleciał.

Do tego w T1:
- usunąłem `all_runs` i pełny `probe` ffprobe (to one rozdęły JSON do ~150 KB → ucięcie);
  zostawiam `all_runs_wall_seconds` i `all_runs_fps` jako dwie listy liczb
- dodałem **wykrywanie cichego fallbacku** `-hwaccel dxva2` → software. W uciętym pliku
  wariant `dxva2_expected_to_fail` miał `rc=0` i `derived_fps=228` — to **nie jest** dowód,
  że DXVA2 zadziałał; to ffmpeg, który nie zainicjował hwaccel i zdekodował programowo.
  Teraz raportujemy `hwaccel_probe.fell_back_to_software` i linie z `hwaccel`/`dxva`.
- dodaję `T1_skrot.txt` (kilkadziesiąt linii) — **jeśli JSON znowu będzie za duży do
  załączenia, prześlij ten plik**

---

## 5. Co teraz — trzy uruchomienia

```bat
cd /d "%USERPROFILE%\Desktop\NewKarnyNow\tests"

:: 1. pomiar CPU (BRAMKA G0.1) - 3-8 min
python T1_decode_bench.py

:: 2. sciezka prezentacji (BRAMKA G0.2) - 1-2 min, mignie okno 1280x800
python T4_d3d9_present.py

:: 3. kto parsuje ANSI - uruchom DWA RAZY:
python T5_host_detect.py            :: w tym terminalu co zwykle (ConEmu?)
:: potem: Start -> cmd.exe
cd /d "%USERPROFILE%\Desktop\NewKarnyNow\tests"
python T5_host_detect.py            :: w czystym cmd.exe
```

Do odesłania: `T1_wynik.json` **albo** `T1_skrot.txt`, `T4_wynik.json`,
`T5_wynik_1.json`, `T5_wynik_2.json`.

Przy okazji dwie rzeczy, których nie zmierzy żaden skrypt:
1. **Czy w T3 linijka z ramkami `─│┌┐└┘ █░▒▓ ▶ ✓` wyglądała poprawnie, czy jak krzaki?**
   (rozstrzyga font konsoli → ramki Unicode vs ASCII)
2. **Czy T3/T1/T4 uruchamiałeś w ConEmu, czy w zwykłym cmd?** (potwierdzi albo obali
   hipotezę z §2.2 bez czekania na T5)

---

## 6. Zaktualizowany rejestr ryzyk

| # | Ryzyko | Było | Jest | Dlaczego |
|---|---|---|---|---|
| R6 | binarka GCC 16 nie wystartuje na Win7 | średnie/krytyczne | **ZAMKNIĘTE** | 15/15 wariantów `run_rc=0`, brak blokerów w importach |
| R4 | brak VT w conhost → TUI | pewne/średnie | **częściowo zamknięte** | VT działa w *tym* hoście; T5 ustali, czy zawsze. Nawet w najgorszym razie mamy `CreateConsoleScreenBuffer` + `WriteConsoleOutputW` (0,1 ms/ekran), czyli backend Win32 jest tani |
| R9 | 40 Hz panel → judder | pewne/niskie | **zmienione**: 60 Hz, ale ryzyko **DPST zbija Hz w trakcie sesji** | dodajemy pomiar kadencji w pętli prezentacji; T4 sprawdzi dostępność 60 Hz |
| R1 | CPU nie wyrabia | średnie/krytyczne | **bez zmian, ale cel się przesunął** | 60 Hz otwiera 720p60 = 83-138 % budżetu. Realny wybór: 720p30 albo 540p60 |
| R14 *(nowe)* | zależności bibliotek zbudowane bez `-march=core2 -msse3` → 20-40 % straty | — | **średnie/wysokie** | mitigacja: jeden kanoniczny zestaw flag dla *wszystkich* vendored bibliotek, weryfikowany przez `-dM` w buildzie |
| R15 *(nowe)* | Python: `pip list` = 2 pakiety, brak `rich`/`textual`; Python 3.8 EOL | — | **średnie/średnie** | mitigacja: pinowane `requirements.txt` (cp38), lokalny katalog wheeli, test instalacji offline na maszynie |
| R16 *(nowe)* | font rastrowy konsoli nie ma glifów ramek Unicode | — | **wysokie/niskie** | mitigacja: przełącznik `--ascii-boxes`, ten sam layout |

---

## 7. Decyzje, które już mogę podjąć na podstawie pomiarów

1. **Toolchain: x86_64-w64-mingw32-gcc 16.2.0, `-static -static-libgcc`, kanoniczny zestaw
   flag z §1.3.** Zamknięte.
2. **IPC Python↔C: JSON-lines po stdin/stdout, limit linii 1 MiB** (wzorzec z
   `docs/core-protocol.md`). Zamknięte — bo konsola i procesy działają, a to nic nie kosztuje.
3. **Prezentacja: D3D9, bez D3D11.** Otwarte tylko *którą ścieżką (NV12 `StretchRect` /
   YUY2 / GDI) — rozstrzygnie T4.
4. **TUI: Python 3.8.20 + Textual/Rich** — *jeśli* T5 potwierdzi, że ANSI działa też w czystym
   `cmd.exe` **albo** zaakceptujesz wymóg startu przez ConEmu. W przeciwnym razie backend
   Win32 (`WriteConsoleOutputW`, 0,1 ms/ekran — czyli tani). Otwarte do T5.
5. **Cel jakościowy obrazu: 720p30 jako plan A, 540p60 jako plan B.** Otwarte do T1.
