# Testy do uruchomienia na Dell E5500 (Windows 7 SP1 x64)

Cztery testy. Każdy zapisuje wynik do pliku `T*_wynik.json` **obok skryptu**.
Kolejność ma znaczenie: `T1` i `T2` rozstrzygają najwięcej, `T4` wymaga kompilatora.

**Wszystkie są bezpieczne i odwracalne.** Żaden nie wysyła nic do sieci, nie loguje
do NVIDIA, nie dotyka Twojego repo `KarnyNow`, nie zmienia rejestru (T3 tylko *czyta*
klucze `HKCU\Console` i tymczasowo ustawia tryb konsoli, przywracając go na końcu).

---

## Jak uruchomić

```bat
cd PULPIT\gdzies\kolejno   (katalog z tymi plikami)
python T1_decode_bench.py
python T2_toolchain_win7.py
python T3_console_diag.py
python T4_d3d9_present.py
```

Jeśli `python` nie jest na PATH, podaj pełną ścieżkę do interpretera (tego 3.8.x).
Każdy test tworzy swój katalog tymczasowy (`T1_tmp`, `T2_tmp`, `T4_tmp`) — można je
skasować po wszystkim.

**Ważne dla T3:** uruchamiaj w *prawdziwym* oknie konsoli (`cmd.exe`), **nie**
z przekierowaniem do pliku (`> log.txt`) i nie z IDE, które podstawia własny pty —
inaczej test uchwytu konsoli zwróci „stdout is not a console handle".

---

## Zasady ogólne (ważne)

**Uruchamiaj w ConEmu, nie w czystym `cmd.exe`.**
T5 rozstrzygnął to pomiarem: w ConEmu jest załadowany `ConEmuHk64.dll` i sekwencje ANSI
działają; w czystym `cmd.exe` hooka nie ma, a bajt `ESC` trafia do bufora konsoli jako znak
i font rysuje go jako `←` — skrypt wypisze śmieci i zniszczy layout (to jest dokładnie
to, co zobaczyłeś przy T3 w cmd). Decyzja **D1**: ConEmu jest wymaganym hostem.

**Nie przekierowuj wyjścia do pliku** (`> log.txt`) ani nie uruchamiaj z IDE, które
podstawia własny pty — testy konsolowe potrzebują prawdziwego uchwytu konsoli
(`GetStdHandle(STD_OUTPUT_HANDLE)`), inaczej zwrócą „stdout is not a console handle".

**Każdy test zapisuje wynik obok siebie** jako `T*_wynik.json`. Testy generujące dużo
danych (T1, T4, T6) zapisują dodatkowo mały `*_skrot.txt` — **jeśli JSON jest za duży,
żeby go załączyć do rozmowy, prześlij skrót**.

**Wszystkie testy są offline i odwracalne.** Żaden nie wysyła nic do sieci, nie loguje się
do NVIDIA, nie dotyka repo `NewKarnyNow`, nie zmienia rejestru (T3/T5 tylko *czytają*
`HKCU\Console` i tymczasowo ustawiają tryb konsoli, przywracając go na końcu).
Katalogi tymczasowe (`T1_tmp`, `T2_tmp`, `T4_tmp`) można skasować po wszystkim.

Jeśli `python` nie jest na PATH, podaj pełną ścieżkę: `C:\Python\Python38\python.exe`.

---

## T1 — `T1_decode_bench.py` (3-8 min) · **BRAMKA NR 1**

**Pytanie:** ile klatek H.264 na sekundę realnie dekoduje ten CPU i ile kosztuje
konwersja koloru?

Co robi:
1. Generuje syntetyczne klipy H.264 (High@5.1, yuv420p) w wariantach
   360p30 / 540p30 / 540p60 / 720p30 / 720p60 / 1088p30, z bitrate'em zbliżonym do
   tego, jaki sami będziemy negocjować (1,2-8 Mbps) — **nie** 75 Mbps.
2. Dekoduje każdy klip do `null` w kilku konfiguracjach i mierzy `-benchmark`:
   - `decode_only_1t`, `decode_only_2t`, `decode_only_2t_frame` (thread_type=frame)
   - `to_nv12_2t` — sam dekoder, wyjście NV12 (bez konwersji)
   - `to_rgb24_2t_swconv` — **z konwersją NV12→RGB na CPU** (to jest ścieżka, której
     OpenNOW używa w fallbacku programowym i której musimy uniknąć)
   - `to_yuy2_2t_swconv` — konwersja NV12→YUY2 (tania, ~0,1 cyklu/piksel)
   - `scale800_nv12_2t` — skalowanie do 1280×800 na CPU
   - `dxva2_expected_to_fail` — `-hwaccel dxva2`, oczekujemy porażki (potwierdzenie
     Twojego probe'u `E_NOINTERFACE`)
3. Liczy `derived_fps`, `cpu_percent`, `headroom_x` i `realtime_ok`.

**Na co patrzę w wyniku:**
- `verdict["720p30"].best_decode_fps` — musi być ≥ 30, najlepiej ≥ 40 (zapas).
- `verdict["720p30"].sw_rgb_penalty_pct` — jeśli > 30 %, ścieżka „dekoder → RGB na CPU"
  jest wykluczona i **musimy** mieć GPU-side CSC (patrz T4).
- `dxva2_expected_to_fail.rc != 0` — potwierdza brak DXVA2.

Warianty skrócone:
```bat
python T1_decode_bench.py --quick
python T1_decode_bench.py --res 1280x720 --fps 30 --seconds 12
python T1_decode_bench.py --skip-hwaccel
```

---

## T2 — `T2_toolchain_win7.py` (1-3 min) · **BRAMKA NR 2**

**Pytanie:** czy GCC 16.2.0 z mingw-w64 produkuje binarki, które **uruchamiają się**
na Windows 7, i jakie są jego domyślne `_WIN32_WINNT` / `WINVER`?

Co robi:
1. Wypisuje `default_macros` dla każdego kompilatora na PATH — to pokazuje, w jakie
   Windows toolchain celuje domyślnie.
2. Kompiluje `probe.c` w 5 wariantach flag (`default`, `win7`, `win7_static`,
   `win7_core2`, `win7_subsys601`), dla x64 i x86.
3. **Uruchamia każdą binarkę** i zbiera to, co sama o sobie zamelduje (wersja OS z
   `GetVersionEx`, `Direct3DCreate9`, `WSAStartup`, rozdzielczość).
4. Parsuje nagłówek PE: `MajorOperatingSystemVersion`, `MajorSubsystemVersion`,
   listę importowanych DLL-i i **oznacza DLL-e/API, których na Win7 nie ma**
   (`hard_blockers_present`).

**Na co patrzę w wyniku:**
- `builds[*].ran_ok == true` dla wariantu `win7_static` — jeśli tak, toolchain jest użyteczny.
- `pe.major_subsystem_version` — wartość `6.01` jest pożądana; `10.0` to sygnał,
  że domyślne flagi celują w Win10.
- `pe.hard_blockers_present` — **musi być pusta**. Jeśli zawiera np.
  `GetSystemTimePreciseAsFileTime` albo `SetThreadDescription`, musimy dodać
  `-D_WIN32_WINNT=0x0601` globalnie i/lub użyć starszego mingw.
- `unknown_dlls` — lista DLL-i spoza zbioru znanego z Win7.

Ten test rozstrzyga też przy okazji, **czy w ogóle mamy `-march=core2 -msse3`** bez
błędów oraz czy `d3d9.lib`/`ws2_32` linkują się z Twojego SDK.

---

## T3 — `T3_console_diag.py` (~35 s, w tym 20 s interakcji) · **BRAMKA NR 3**

**Pytanie:** czy conhost na Twoim Win7 (modyfikacja JG 2021) parsuje ANSI/VT, i czy
da się zbudować TUI bezpośrednio na Win32 Console API?

Co robi (i dlaczego to ważne):
- **C. test empiryczny ANSI** — wypisuje `\x1b[31mCZERWONY\x1b[0m`, a potem **czyta
  komórki konsoli** przez `ReadConsoleOutputCharacterW` / `ReadConsoleOutputAttribute`.
  Jeśli w odczycie widać `ESC`/`[31m` jako tekst → parsera VT nie ma (spodziewany
  wynik na Win7). Jeśli pierwsza komórka ma atrybut `0x4` (FOREGROUND_RED) → VT działa.
  To jest dowód, nie zgadywanie.
- **D. alternatywny bufor** — `CreateConsoleScreenBuffer` + `SetConsoleActiveScreenBuffer`
  (działa na Win7) vs `\x1b[?1049h` (nie działa bez VT). To rozstrzyga, czy TUI może
  mieć „czysty ekran" i czy po wyjściu przywróci poprzednią zawartość.
- **E. Unicode** — zapis/odczyt ramek `─│┌┐└┘`, bloków `█░▒▓`, `▶✓`. Jeśli roundtrip
  nie jest identyczny lub pojawiają się `?`, TUI musi używać ramek ASCII.
- **F. wejście** — 20 s interakcji: `WaitForSingleObject` + `ReadConsoleInputW`.
  **Kliknij kilka razy myszą w oknie konsoli** i wciśnij strzalki/Enter/F1/A/Z.
  Test raportuje, czy zdarzenia myszy w ogóle dochodzą i jakie maski przycisków widać.
  Bez tego nie da się zrobić „klikalnej" listy gier.
- **H. wydajność** — ile ms kosztuje `WriteConsoleOutputW` dla pełnych 80×25, jednej
  linii i przez `WriteConsoleOutputCharacterW`. To ustala, czy renderer różnicowy
  utrzyma 30 Hz odświeżania TUI.
- **I. rich/textual** — co wykrywa `rich._windows.get_windows_console_features()`
  (`vt`, `truecolor`) i `Console().legacy_windows`, plus snapshot `pip list`.
- **Rejestr** — odczyt `HKCU\Console` (FaceName, FontSize, CodePage,
  `VirtualTerminalLevel`, `ForceV2`). Na Win7 `VirtualTerminalLevel` zwykle w ogóle
  nie istnieje; jeśli JG 2021 coś tu dodał, chcę to zobaczyć.

**W trakcie testu F wykonaj te kliknięcia i klawisze** — inaczej `mouse_events = 0`
i nie będę wiedział, czy to brak wsparcia, czy brak danych.

---

---

## T4 — `T4_d3d9_present.py` + `T4_d3d9_present.c` · **BRAMKA G0.2 — WERSJA v3**

⚠️ **v1 zcrashował** (`run_rc = 3221225477` = `0xC0000005 ACCESS_VIOLATION`) na x64 **i** x86,
bez żadnego wyjścia — bo cały JSON był buforowany i zapisywany dopiero na końcu.
Najbardziej prawdopodobny winny: `StretchRect`/`UpdateSurface` na powierzchni NV12 wywołane
wbrew temu, co mówi `CheckDeviceFormatConversion`, na starym sterowniku `igdumd64.dll` 8.15.10.2869.

### Co robi v3 (i dlaczego wygląda dziwnie)

1. **Izolacja etapów.** Harness uruchamia **14 osobnych procesów**:
   `info`, `formats`, `device`, `surfaces`, `updatesurf`,
   `stretch --case=0..4`, `bench --which=0..3`.
   Kod powrotu `0xC0000005` wskazuje **dokładnie** winną operację, a wszystkie
   wcześniejsze wyniki zostają na dysku.
2. **Przyrostowy zapis.** Plik jest otwarty od początku; po każdym etapie następuje dopisek
   i uszczelnienie `"]}"` + `fflush`. Plik na dysku jest **poprawnym JSON-em aż do miejsca
   crasha** — więc nawet awaria daje dane.
3. **`CheckDeviceFormatConversion` PRZED każdym `StretchRect`.** Przy odmowie program
   **nie wywołuje** `StretchRect` i zapisuje decyzję w JSON. To nie jest ostrożność testu —
   to zasada, która musi trafić do klienta produkcyjnego (decyzja D7).
4. **Kolejność od najbezpieczniejszej:** `stretch` case 0 = X8R8G8B8 (na pewno działa)
   → 1 = YUY2 → 2 = NV12 → 3 = NV12+`RESTRICTED_CONTENT` → 4 = NV12→RGB offscreen.
   Jeśli NV12 zabija driver, mamy już zmierzone RGB i YUY2 = **gotowy plan B**.

### Uruchomienie

```bat
python T4_d3d9_present.py             :: x64 i x86, wszystkie etapy (2-4 min)
python T4_d3d9_present.py --arch x64  :: szybciej
python T4_d3d9_present.py --quick     :: 40 iteracji zamiast 200
```

Program tworzy pełnoekranowe okno 1280×800 i miga na nim wzorem — to normalne.
Okno pojawia się **14 razy na architekturę** (bo każdy etap to osobny proces);
całość powinna zająć kilkadziesiąt sekund na architekturę.

### Wyniki

- `T4_wynik.json` — scalony raport ze wszystkich etapów i architektur
- **`T4_skrot.txt`** — mały; **jeśli JSON będzie za duży do załączenia, prześlij ten plik**

### Co dodatkowo mierzy v3 (a nie mierzył v1)

- `surface_descs`: `GetDesc` + `LockRect` (z `D3DLOCK_NOSYSLOCK` i bez) → **pitch** powierzchni.
  Dekoder musi znać pitch, żeby poprawnie zapisać NV12.
- `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` → bieżący `dmDisplayFrequency`
  + **pełna lista trybów 1280×800** (`mode_1280x800_at_60hz_available`, `max_hz_seen`).
  Potrzebne, bo odświeżanie raz pokazało 40 Hz, raz 60 Hz (Intel DPST — ryzyko R9).
- `caps.StretchRectFilterCaps` + `srf_linear`/`srf_point` → czy skalowanie będzie LINEAR
  czy POINT (bez tego nie wiemy, czy obraz będzie rozmazany).
- `TestCooperativeLevel` przed każdym etapem i po każdym `StretchRect`.

---

## T5 — `T5_host_detect.py` · ✅ WYKONANY ×2 (wynik rozstrzygający)

Rozstrzygnął, **kto** parsuje ANSI. Uruchomiony raz w ConEmu i raz w czystym `cmd.exe`:

| | ConEmu | czysty `cmd.exe` |
|---|---|---|
| `ConEmuHk64.dll` w procesie | **TAK** | nie |
| zmienne `CONEMU*` | 23 szt., `CONEMUANSI=ON`, `CONEMUHOOKS=Enabled` | zero |
| ANSI 16 kolorów | ✅ atrybuty `0x4` | ❌ |
| ANSI 256 / truecolor | ✅ ale **kwantyzowane do 16 kolorów** (`0xC`, `0x9`) | ❌ |
| alt-screen + adresowanie kursora | ✅ | ❌ |
| bajt `ESC` w buforze | skonsumowany | **zapisany jako znak `←`** |

Wniosek: **VT daje wyłącznie `ConEmuHk64.dll`.** Decyzja D1 (ConEmu wymagany) POTWIERDZONA.
To jest dokładnie objaw z Textualize/textual#3378 na tej samej konfiguracji.

---

## T6 — ❌ ANULOWANY

Benchmark openh264 vs FFmpeg. **Decyzja D3: openh264 porzucony, FFmpeg zostaje.**
Powód: openh264 wymaga NASM (`meson.build` openh264 robi `find_program('nasm')`
**bez** `required:false`, więc brak NASM przerywa konfigurację — to nie jest
„zbuduje się wolniej"), a pomiar T1 pokazał, że FFmpeg `h264` i tak dekoduje 720p30
z 7-krotnym zapasem (211 fps). Pliki `T6_*` usunięte z `tests/`.

---

## Status testów

| Test | Stan | Rozstrzygnął |
|---|---|---|
| T1 | ✅ wykonany (wynik w `T1_skrot.txt`) | G0.1 CPU — **720p30 = 24,8 % obu rdzeni** |
| T2 | ✅ wykonany | G0.3 toolchain — 15/15 binarek działa na Win7 |
| T3 | ✅ wykonany (w ConEmu) | konsola: VT, mysz, Unicode, `WriteConsoleOutputW` 0,1 ms |
| T4 **v4** | ⏳ **DO URUCHOMIENIA** | G0.2 ścieżka prezentacji D3D9 |
| T5 | ✅ wykonany ×2 | G0.4 host konsoli — ConEmu wymagany |
| T6 | ❌ anulowany | — |
| T7 | ✅ wykonany (0 zdarzeń) → **D10/D11** | zamknięte decyzją: TUI bez myszy, resize przez polling |

---

## T7 — `T7_mouse_sgr.py` (~15 s) · rozstrzyga R18: czy lista gier może być klikalna

T3 potwierdził, że `ReadConsoleInputW` daje zdarzenia myszy (146 w 20 s), ale T5 pokazał
`looks_like_sgr=false` **w obu hostach** — i T5 był niemiarodajny, bo czytał stdin przez
`msvcrt.getwch()`, który na Windows nie skleja poprawnie sekwencji wielobajtowych.

T7 czyta **surowe znaki** i sam je skleja regexem, więc rozstrzyga:

1. czy ConEmu w ogóle wysyła raporty myszy po włączeniu trybów `1000/1002/1003`
2. w jakim formacie: **SGR** (`ESC[<b;x;yM`) czy **X10** (`ESC[M` + 3 bajty)
3. czy działają zdarzenia **resize** (`ESC[8;H;Wt`) — potrzebne do przerysowania TUI
4. interpretuje kody przycisków (LPM/PPM/SPM/kółko/SHIFT/CTRL/press/release)

**Uruchom w ConEmu** (nie w czystym cmd — patrz D1):

```bat
python T7_mouse_sgr.py
```

Przez 15 sekund: poruszaj myszą nad oknem, kliknij **LPM 2×** i **PPM 1×**, przewiń kółkiem,
i **zmień raz rozmiar okna** (przeciągnij krawędź ConEmu). Surowe sekwencje będą widoczne
na ekranie — to normalne.

Wpływ na projekt:
- **SGR + resize działają** → Textual z pełną myszą: klikalna lista gier, przewijanie kółkiem
- **tylko SGR** → mysz działa, ale resize bierzemy z `ReadConsoleInputW` (zdarzenie typu 4)
- **nic nie działa** → nawigacja wyłącznie klawiaturą (T3 potwierdził komplet VK: strzałki,
  Enter, F1, litery) — też akceptowalne, ale musimy to wiedzieć **zanim** narysujemy UI

Wynik: `T7_wynik.json`

### v3.1 — poprawka kompilacji (2026-10-20, runda IV)

v3 **nie skompilował się**. Trzy błędy, wszystkie z zgadywania sygnatur zamiast sprawdzenia
w nagłówku:

| Błąd | Prawda z `d3d9.h` mingw-w64 |
|---|---|
| `CreateOffscreenPlainSurface passed 8 arguments, but takes just 7` | metoda ma `(Width, Height, Format, Pool, ppSurface, pSharedHandle)` — **bez parametru `Usage`**. Wariant z `D3DUSAGE_RESTRICTED_CONTENT` jest **niewyrażalny** przez to API |
| `unknown type name 'D3DUSAGE'` (×4 rzuty) | typ `D3DUSAGE` **nie istnieje** w mingw-w64; są tylko stałe `D3DUSAGE_*` (= `__MSABI_LONG(...)`), a `CheckDeviceFormat` bierze `DWORD Usage` |
| `srcusage set but not used` | zmienna była martwa — usunięta |

**Zmiana zakresu testu:** case 3 `stretch` przestał być „NV12 + RESTRICTED_CONTENT"
(niewyrażalny), a stał się **„NV12 w rozmiarze ekranu, `StretchRect` 1:1"** — czyli dokładnie
przypadkiem realnym dla klienta (dekod 1280×720 → okno 1280×800).

**Środek zaradczy (trwały):** `tools_check_d3d_calls.py` w katalogu głównym workspace.
Parsuje `#define IDirect3D*(p,a,b,...)` z prawdziwego `d3d9.h` i porównuje liczbę argumentów
w `.c` — **bez kompilatora**. Po poprawkach: **71/71 wywołań zgodnych**, jedyny identyfikator
spoza nagłówków to `D3DFMT_NV12` (mamy własny `#ifndef`).

```bash
python3 tools_check_d3d_calls.py     # uruchamiac po KAZDEJ edycji T4_d3d9_present.c
```

---

## T7 — `T7_mouse_sgr.py` · ✅ WYKONANY (wynik: 0 zdarzeń) → decyzja D10

Zmierzone: `chunks=0`, `bytes_total=0`, `sgr_events=0`, `x10_markers=0`, `resize_events=[]`.
Host potwierdzony jako ConEmu (`CONEMUANSI=ON`, `CONEMUHOOKS=Enabled`, build `230724`,
`ConEmuHk64.dll` załadowany, `ANSICON=125x32766 (125x32)`).

Czyli: **ani mysz SGR, ani resize nie dotarły do aplikacji.**

**To nie blokuje projektu** — użytkownik zadeklarował, że nie używa myszy w TUI
(„strzałeczki są najlepsze"). Stąd:

- **D10**: TUI **wyłącznie klawiaturą**, `Textual(mouse=False)`. Strzałki + Enter/Esc/litery.
  T3 potwierdził komplet VK: 13 (Enter), 37-40 (strzałki), 65/90 (A/Z), 112 (F1).
- **D11**: **resize przez polling** `GetConsoleScreenBufferInfo` co tick pętli, nie ze zdarzeń
  (`resize_reporting_works=false`).

Test zostaje w repo jako dokumentacja pomiaru; nie trzeba go powtarzać.

---

## Runda VI — T4 v4 (2026-10-20)

### Co pokazał v3

Izolacja etapów **zadziałała**. 13 z 14 etapów przeszło z `run_rc=0` na x64 i x86.
**Crashował wyłącznie `updatesurf`** (`0xC0000005`, obie architektury), czyli
`UpdateSurface(SYSTEMMEM NV12 → DEFAULT NV12)`.

To jest **ustalenie projektowe**: ścieżka „dekoder pisze do pamięci systemowej, potem
`UpdateSurface` wgrywa na GPU" jest **martwa na tym sterowniku**. Wszystkie pięć case'ów
`stretch` (RGB, YUY2, NV12, NV12+restricted, NV12→RGB) i cztery benchmarki przeszły **bez
crasha** — czyli `StretchRect` prawdopodobnie nie jest problemem.

### Dlaczego mimo to nie mamy danych

Warstwa zapisu JSON w v3 używała `fseek(0,SEEK_END)` / `ftell` / cofania się o 2 bajty i
nadpisywania `"]}"`. W efekcie **wszystkie pliki etapów miały 0 bajtów**. Dodatkowo harness
kasował pliki przed każdym uruchomieniem i czytał `partial_head` spod złego klucza.

### Co zmieniono w v4

1. **Zapis JSON przepisany:** akumulator w RAM + **przepisanie całego pliku** przy każdym
   elemencie. Zero `fseek`/`ftell`. Plik jest zawsze poprawnym, zamkniętym JSON-em.
2. **Nowy test jednostkowy warstwy JSON** — `tools_test_json_layer.sh` (w katalogu głównym
   workspace) wycina samą warstwę z `.c`, kompiluje ją **na Linuxie bez D3D9** i waliduje:
   3 elementy, zagnieżdżone obiekty, tablice, escapowanie, HRESULT jako hex, element z 2000
   pól. Aktualnie: **PASS (24 150 bajtów)**.
3. **`updatesurf` rozbity na 6 podkroków** — `--case=0..5`:
   `create SYSTEMMEM` → `create DEFAULT` → `LockRect` → wypełnienie bufora →
   `UnlockRect` → `UpdateSurface`. Każdy w osobnym procesie, więc następny uruchomienie
   nazwie **dokładnie** to wywołanie, które zabija driver.
4. **Harness:** 19 procesów na architekturę, pliki etapów **zachowane** w
   `T4_tmp\stages\t4_<arch>_<etap>.json`, a przy błędzie parsowania drukowany jest
   początek pliku i jego rozmiar.

### Uruchomienie (bez zmian)

```bat
python T4_d3d9_present.py
```

Do odesłania: `T4_wynik.json` + `T4_skrot.txt`. Jeśli coś pójdzie nie tak, **nie kasuj**
`T4_tmp\stages\` — te pliki są najcenniejsze.

### Stan weryfikacji kodu C

```
tools_verify_c.sh         -> zero błędów, zero ostrzeżeń (-Wall -Wextra), binarka 276 kB
tools_check_d3d_calls.py  -> 71/71 wywołań makr IDirect3D* zgodnych z d3d9.h
tools_test_json_layer.sh  -> PASS (warstwa JSON przetestowana na Linuxie)
```

---

## T12 — test sieciowy bez powtarzania testów GPU

T12 buduje tylko `knvstd` i uruchamia `net.dns`, `net.tcp443`, `net.tls` oraz
`tls.chain`. Nie uruchamia testów jednostkowych, D3D, dekodera, konsoli ani
zegara. Używaj go do szybkich iteracji backendu sieciowego; pełny T10 nadal
pozostaje bramką po zmianach obejmujących cały build.

W ConEmu na Win7, dla testu osadzonego TLS:

```bat
set KN_TLS_BACKEND=mbedtls
python tests\T12_network.py
```

`T12_skrot.txt` podaje verdict i statusy czterech probe'ów; szczegóły,
ostrzeżenia kompilatora i ścieżkę do bundla zawiera `T12_wynik.txt`. Przy
mbedTLS wymagane są `net.tls=ok` dla trzech hostów i `tls.chain=ok` z czystym
łańcuchem. Przy Schannel TLS pozostaje findingiem, a T12 raportuje tylko PASS
DNS/TCP. POSIX uruchamia T12 wyłącznie jako `DRY-RUN ONLY` i nie daje wyniku
łączności.
