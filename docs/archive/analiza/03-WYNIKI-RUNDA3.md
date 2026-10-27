# Runda III — T1 (CPU), T5 (host konsoli), crash T4, decyzja D3

**Data:** 2026-10-20 · **Wejście:** `T1_skrot.txt`, `T5_wynik_1_Con_Emu.json`, `T5_wynik_2_CMD.json`, `1_T4_wynik.json` (crash), uwagi użytkownika.

---

## 0. Streszczenie: trzy rzeczy, które się rozstrzygnęły

1. **CPU nie jest wąskim gardłem.** Dekodowanie 720p30 kosztuje **~25 % obu rdzeni**, nie 41-69 %, jak szacowałem w rundzie I. Pomyliłem się o ~3,5×. Realnie **osiągalne jest nawet 720p60 (~46 %)**. Bramka G0.1 **ZALICZONA**.
2. **ANSI daje ConEmu, nie conhost.** Dowód twardy w obu kierunkach: w ConEmu `ConEmuHk64.dll` jest załadowany w procesie i kolory działają; w czystym `cmd.exe` hook DLL-a nie ma, a bajt `ESC` **wpada do bufora konsoli jako znak** i font rysuje go jako `←`. D1 **POTWIERDZONA** (ConEmu wymagany).
3. **T4 zcrashował (0xC0000005) bez śladu** — bo buforował cały JSON do końca. Przepisany na v3 z **izolacją etapów** (każdy ryzykowny wywołanie w osobnym procesie) i przyrostowym zapisem. Bramka G0.2 **nadal otwarta**.

Plus: **D3 rozstrzygnięta** — openh264/T6 porzucone, **FFmpeg zostaje** (decyzja użytkownika; pomiar T1 potwierdza, że jest bardzo szybki na tym CPU). Pliki `T6_*` usunięte.

---

## 1. T1 — budżet CPU policzony od nowa

### 1.1 Metryka, którą trzeba czytać (uwaga na pułapkę nazwy)

Pole `cpu_percent_of_one_core_pair` w `T1_wynik.json` to **procent Z OBU rdzeni**
(200 % = pełne nasycenie 2 rdzeni), a nie „jednej pary jako 100 %". Nazwa jest myląca,
więc przeliczyłem wszystko na dwie wielkości, które się nie mylą:

- **ms CPU na klatkę** = `cpu_process_seconds × 1000 / liczba_klatek`
- **% obu rdzeni przy danej płynności** = `ms_cpu_na_klatkę × fps / 20`

### 1.2 Wyniki (klipy 8 s, H.264 High@5.1, yuv420p, `scenecut=0`)

| Źródło | Konfiguracja | fps | ms CPU/klatkę | **@30 fps** | **@60 fps** |
|---|---|---|---|---|---|
| 360p30 | `decode_only_1t` | 720,7 | 5,20 | **7,8 %** | 15,6 % |
| 360p30 | `scale800_nv12_2t` | 252,6 | 15,21 | 22,8 % | 45,6 % |
| 540p30 | `decode_only_1t` | 344,3 | 10,53 | **15,8 %** | 31,6 % |
| 540p30 | `scale800_nv12_2t` | 184,8 | 21,20 | 31,8 % | 63,6 % |
| 540p60 | `decode_only_1t` | 385,2 | 9,55 | 14,3 % | **28,7 %** |
| 540p60 | `to_nv12_2t` | 330,4 | 11,44 | 17,2 % | **34,3 %** |
| **720p30** | `decode_only_1t` | **211,1** | **16,52** | **24,8 %** | 49,5 % |
| **720p30** | `decode_only_2t` | 191,1 | 17,16 | 25,7 % | 51,5 % |
| **720p30** | `to_nv12_2t` | 191,5 | 19,23 | **28,9 %** | 57,7 % |
| 720p30 | `to_rgb24_2t_swconv` | 169,5 | 22,10 | 33,2 % | 66,3 % |
| 720p30 | `to_yuy2_2t_swconv` | 132,8 | 28,60 | 42,9 % | 85,8 % |
| 720p30 | `scale800_nv12_2t` | 133,4 | 28,33 | **42,5 %** | 85,0 % |
| **720p60** | `decode_only_1t` | 252,0 | 15,34 | 23,0 % | **46,0 %** |
| **720p60** | `to_nv12_2t` | 221,5 | 17,42 | 26,1 % | **52,3 %** |
| 720p60 | `to_yuy2_2t_swconv` | 147,4 | 26,52 | 39,8 % | 79,6 % |
| 1088p30 | `decode_only_1t` | 103,7 | 36,41 | **54,6 %** | 109,2 % |
| 1088p30 | `to_nv12_2t` | 93,1 | 40,43 | 60,6 % | 121,3 % |

Pamięć: `FreePhysicalMemory` 1 314 940 kB przed testem, 1 472 232 kB po, z
`TotalVisibleMemorySize` 2 051 244 kB → **~1,25 GB wolnego**. Zapas jest.

### 1.3 Cztery wnioski, które zmieniają projekt

**(a) Mój szacunek z rundy I był ~3,5× zbyt pesymistyczny.**
Zakładałem 1,5-2,5 cyklu/piksel/klatkę. Pomiar daje **0,45-0,6 cyklu/piksel**
(720p30: 16,52 ms × 4000 Mcy/s ÷ 2 rdzenie = 33 Mcy na 0,9216 Mpx). Dekoder `h264`
z FFmpeg ma wyjątkowo dojrzały asm x86 i Merom z SSSE3 wystarcza aż nadto.
→ **Bramka G0.1 zaliczona z dużym zapasem.**

**(b) Skalowanie na CPU jest droższe niż samo dekodowanie.**
`720p30 decode_only` = 16,52 ms/klatkę, a `scale800_nv12` = 28,33 ms/klatkę.
Czyli `scale=1280:800` dokłada **~11,8 ms na klatkę** — przy 30 fps to **+18 pkt proc.**
obciążenia (24,8 % → 42,5 %). **To jest pojedynczo najdroższa operacja w całym torze
wideo i musi ją robić GPU.** Stąd waga T4 i stąd łańcuch fallbacków NV12 → YUY2 → RGB → GDI.

**(c) Konwersja koloru jest tańsza, niż sądziłem, ale kolejność zostaje ta sama.**
`to_nv12` = +2,7 ms/klatkę (niemal darmo — dekoder i tak wypuszcza NV12),
`to_rgb24` = +5,6 ms, `to_yuy2` = +12,1 ms. W rundzie I straszyłem, że NV12→RGB
„zabije projekt" (szacowałem +8-15 pkt proc.); realnie to **+8,4 pkt proc. przy 30 fps**
— nieprzyjemne, ale przeżywalne. Kolejność preferencji się nie zmienia:
**NV12 (zero kosztu) > RGB > YUY2**, a wszystko to i tak lepiej oddać GPU.

**(d) `threads=1` jest LEPSZE niż `threads=2`.**
720p30: 211,1 fps (1 wątek) vs 191,1 fps (2 wątki) vs 197,0 fps (2 wątki, `thread_type=frame`).
720p60: 252,0 vs 252,1 vs 250,4 — remis.
Na 2-rdzeniowym Meromie synchronizacja międzywątkowa dekodera kosztuje więcej niż zyskuje.

**To jest decyzja architektoniczna, nie ciekawostka:** dekodujemy **jednym wątkiem**,
a drugi rdzeń zostaje wolny na sieć (Mjolnir UDP + reorder + FEC), Opus, pętlę prezentacji
i proces Pythona z TUI. Właśnie dlatego `maxThreadCount ≤ 2` w Twoim `AppState` jest
słuszne, ale **`thread_count` dekodera powinien wynosić 1**, nie 2. Popraw to w ABI.

### 1.4 `-hwaccel dxva2` — potwierdzenie braku DXVA2 (i pułapka interpretacyjna)

Wszystkie warianty `dxva2_expected_to_fail` mają `rc=0`, ale **dramatycznie** niższe fps
niż dekodowanie programowe:

| Źródło | fps z `-hwaccel dxva2` | fps programowo (1t) | stosunek |
|---|---|---|---|
| 360p30 | 273,0 | 720,7 | 2,6× wolniej |
| 540p30 | 146,3 | 344,3 | 2,4× wolniej |
| 720p30 | **50,4** | 211,1 | **4,2× wolniej** |
| 720p60 | 70,8 | 252,0 | 3,6× wolniej |
| 1088p30 | 34,4 | 103,7 | 3,0× wolniej |

Do tego `cpu%` tych przebiegów to tylko 50-74 % (reszta to czekanie), a
`hwaccel_probe.fell_back_to_software=False` i `hwaccel_lines=[]` — czyli **ffmpeg nie
zameldował fallbacku**, mimo że ewidentnie nie dekodował sprzętowo.

Wniosek: `-hwaccel dxva2` na tej maszynie **marnuje czas na nieudane wywołania drivera**
i daje wynik 3-4× gorszy niż czysty software. Spójne z Twoim probe'em
(`QI(IDirectXVideoDecoderService)=0x80004002`). **W kliencie nie próbujemy DXVA2 w ogóle**
— nie jako fallback, tylko wcale. Oszczędza to kodu i czasu startu.

### 1.5 Zastrzeżenie uczciwości pomiaru

Klipy to `testsrc2` + `noise=alls=14:allf=t`, kodowane `-preset veryfast` z `scenecut=0`
i stałym `keyint=fps`. To **mniej wymagający** strumień niż prawdziwa rozgrywka
(więcej prostych makrobloków, przewidywalne wektory ruchu). Realny koszt dekodowania
treści growej będzie wyższy — przyjmijmy mnożnik **1,3-1,8×** i nie liczmy na więcej.

Nawet z mnożnikiem 1,8×: **720p30 = 44,6 % obu rdzeni**. Mieści się.
Przy mnożniku 1,8× i 60 fps: 720p60 = 82,8 % — **za dużo**, więc 60 fps zostaje
jako opcja eksperymentalna, nie domyślna.

### 1.6 Zrewidowany cel jakościowy (zastępuje §7 z `00-WERDYKT.md`)

| Tryb | Koszt (pomiar) | Koszt z mnożnikiem 1,8× | Status |
|---|---|---|---|
| **1280×720 @30** | 24,8 % (decode) / 28,9 % (z NV12) | ~45-52 % | **PLAN A** — zalecany domyślny |
| 1280×720 @60 | 46,0 % / 52,3 % | ~83-94 % | **opcja**, nie domyślna; ryzyko przy DPST |
| 960×540 @60 | 28,7 % / 34,3 % | ~52-62 % | **PLAN B** — jeśli 720p30 będzie szarpać |
| 960×540 @30 | 15,8 % / 17,6 % | ~28-32 % | bezpieczny minimum |
| 640×360 @30 | 7,8 % | ~14 % | tryb awaryjny |
| ~~1920×1088 @30~~ | 54,6 % | ~98 % | **odrzucone** — ekran ma 1280×800, więc to czyste marnowanie CPU i łącza |

**Decyzja D2 (zrewidowana): domyślnie 1280×720@30, przełączane w `ANNOUNCE`
(`clientViewportWd/Ht`, `maxFPS`), z 540p60 i 540p30 jako niższymi szczeblami.**
Warunek: skalowanie robi GPU (T4), nie CPU.

---

## 2. T5 — kto parsuje ANSI. Dowód w obie strony

### 2.1 ConEmu (`T5_wynik_1_Con_Emu.json`)

```
drzewo procesow : python.exe(1868) <- cmd.exe(10212) <- ConEmuC64.exe(9944) <- ConEmu64.exe(9696)
moduly w procesie: 50, podejrzane = ["C:\Program Files\ConEmu\ConEmu\ConEmuHk64.dll"]
env              : 23 zmienne CONEMU*, CONEMUANSI=ON, CONEMUHOOKS=Enabled,
                   CONEMUBASEDIR=C:\Program Files\ConEmu\ConEmu, CONEMUBUILD=230724,
                   CONEMUISADMIN=ADMIN, ANSICON=105x32766 (105x28), ANSICON_DEF=7
console_title    : "Administrator: cmd - python  T5_host_detect.py"
SetConsoleMode(VT): call_ok=false, ERROR_INVALID_PARAMETER(87), flaga nie utrwalona
```

A mimo to ANSI **działa**, i to lepiej niż w 16 kolorach:

```
chars = "RED16ORANGE256TRUECOLORBOLD ..."        <- ESC skonsumowany
attrs = 0x4 ×5 | 0xC ×9 | 0x9 ×9 | 0xF ×4 | 0x7 ...
red16_ok=true  color256_distinct=true  truecolor_distinct=true  bold_flag=true
alt_screen: cell_at_10_20 = "ALTSCREEN_POS_OK"  -> adresowanie kursora DZIALA
```

Rozkodowanie: `\x1b[31m` → `0x4` (czerwony), `\x1b[38;5;208m` → `0xC` (jasnoczerwony),
`\x1b[38;2;0;200;255m` → `0x9` (jasnoniebieski), `\x1b[1m` → `0xF` (jasny).
Czyli hook ConEmu **parsuje sekwencje i mapuje je na 16-atrybutowy model konsoli**.

**Ważne ograniczenie, które z tego wynika:** truecolor *sekwencje* są akceptowane, ale
efekt jest **kwantyzowany do 16 kolorów** (0x9 zamiast prawdziwego RGB). Textual wykryje
`truecolor` z env i będzie wysyłał `\x1b[38;2;R;G;Bm`, a dostanie przybliżenie.
**Wizualnie: kolory będą, ale nie dokładne.** Dla TUI z listą gier to w zupełności
wystarcza; nie projektujmy jednak interfejsu opartego na subtelnych odcieniach.

### 2.2 Czysty `cmd.exe` (`T5_wynik_2_CMD.json`)

```
drzewo procesow : python.exe(864) <- cmd.exe(1376) <- Explorer.EXE(1920)
moduly w procesie: 49, podejrzane = []            <- BRAK ConEmuHk64.dll
env              : CONEMU* = [] (zero), ANSICON brak; tylko COMSPEC i PROMPT
chars = "←[31mRED16←[0m←[38;5;208mORANGE256←[0m←[38;2;0;200;255mTRUECOLOR←[0m←["
attrs = 0x7 wszedzie (nic nie zastosowane)
escape_leaked_as_text = true
alt_screen: cursor_addressing_ok = false, komorka (10,20) = "...d 256 : FalseKarnyNow\tests>ls"
                                          ^ czyli \x1b[?1049h NIC nie zrobil
```

Kluczowy szczegół: **w buforze nie ma bajtu `\x1b`** — jest `←` (U+2190). Conhost zapisał
bajt 0x1B do bufora jako znak, a font CP437/CP852 rysuje 0x1B jako strzałkę w lewo.
To jest **dokładnie** objaw z Textualize/textual#3378 („textual output is full of ANSI
sequences") i dokładnie to, co opisałeś jako „wywalił się skrypt" — T3 w czystym cmd
wypisał swoje sekwencje jako śmieci i zniszczył layout konsoli.

### 2.3 Wniosek i konsekwencje (D1 → POTWIERDZONA, nie warunkowa)

| Fakt | Konsekwencja |
|---|---|
| VT daje **wyłącznie** `ConEmuHk64.dll` | Klient **musi** startować przez ConEmu. W czystym conhost TUI jest nieczytelne. |
| ConEmu build **230724**, `C:\Program Files\ConEmu\ConEmu`, `CONEMUHOOKS=Enabled` | Wiemy dokładnie, co wymagamy. Wersję i `CONEMUHOOKS` sprawdzamy przy starcie. |
| `ANSICON=105x32766 (105x28)` | ConEmu ustawia `ANSICON` — to dodatkowy, tani sygnał detekcji hosta obok `CONEMUANSI=ON`. |
| `CONEMUISADMIN=ADMIN`, tytuł `Administrator: cmd` | **Sesja jest podniesiona.** Dla nas korzystne: Raw Input, `SendInput`-free capture i `SetThreadPriority` działają bez walki z UIPI. Ale warto wiedzieć, że klient bez elevacji może się zachowywać inaczej. |
| truecolor kwantyzowany do 16 kolorów | Nie projektujemy UI opartego na odcieniach. Textual ustawiamy na `color_system="standard"` **jawnie**, żeby nie generował niepotrzebnych sekwencji 24-bitowych. |
| `sgr_mouse.looks_like_sgr = false` **w obu** hostach | **Nie rozstrzygnięte.** Mój test czytał stdin przez `msvcrt.getwch()`, który na Windows nie jest miarodajny dla sekwencji wielobajtowych. Klikalność listy gier trzeba zmierzyć osobno (T7). Fallback: nawigacja samą klawiaturą — `ReadConsoleInputW` z T3 pokazał komplet VK (strzałki, Enter, F1). |

**Detekcja hosta w kliencie (konkretny kod, do wbudowania w etap 4):**

```python
def detect_console_host():
    """Zwraca (host, vt_ok, powod). ConEmu jest wymagany - patrz analiza/03, §2."""
    import os, ctypes
    env = os.environ
    if env.get("CONEMUANSI", "").upper() == "ON" or env.get("CONEMUHOOKS") == "Enabled":
        return ("conemu", True, "CONEMUANSI/CONEMUHOOKS obecne")
    if env.get("ANSICON"):
        return ("ansicon", True, "ANSICON obecny")          # tez daje VT
    # twardy dowod: czy ConEmuHk64.dll jest zaladowany w TYM procesie
    try:
        mods = enumerate_loaded_modules()                    # EnumProcessModulesEx
        if any("conemuhk" in m.lower() for m in mods):
            return ("conemu", True, "ConEmuHk w liscie modulow")
    except Exception:
        pass
    return ("conhost", False,
            "Wykryto czysty conhost Windows 7: brak parsera ANSI. "
            "Uruchom przez ConEmu (C:\\Program Files\\ConEmu\\ConEmu64.exe). "
            "Dowod pomiarowy: analiza/03-WYNIKI-RUNDA3.md §2.2")
```

I zasada: **jeśli `vt_ok == False`, klient kończy z tym komunikatem i kodem 2**, zamiast
rysować krzaki. To jest dokładnie ta awaria, którą widać w T5_wynik_2 — tylko że u nas
będzie jawna i wyjaśniona.

---

## 3. Crash T4 — diagnoza i co z niego wynika dla kodu produkcyjnego

### 3.1 Fakty

```
x64: compile_rc=0, run_rc=3221225477 (= 0xC0000005 ACCESS_VIOLATION), probe=null, stderr=""
x86: compile_rc=0, run_rc=3221225477, probe=null, stderr=""
```

Ostrzeżenia kompilacji (nieszkodliwe, ale poprawione w v3):
`case value '842094158' not in enumerated type 'D3DFORMAT'` — bo `D3DFMT_NV12` to FOURCC
spoza enuma; `'jkey_raw' defined but not used`.

Brak jakiegokolwiek wyjścia wynikał z konstrukcji v1: cały JSON był trzymany w buforze
i zapisywany **dopiero na końcu**. Crash zabrał wszystko.

### 3.2 Najbardziej prawdopodobny winny

v1 wywoływał `StretchRect` i `UpdateSurface` na powierzchniach NV12 **bez sprawdzenia**,
czy sterownik na to pozwala. Na Intel GMA 4 ze starym driverem (8.15.10.2869) to klasyczny
sposób na AV wewnątrz `igdumd64.dll`. Poszlaki:
- Twój probe platformy: `has_d3d9=true`, ale `has_d3d9ex=false` i `dxva2_query_ok=false` —
  sterownik ma wąskie możliwości.
- `CheckDeviceFormat` dla NV12 mógł zwrócić OK (FourCC bywa akceptowany jako *format
  powierzchni*), podczas gdy `CheckDeviceFormatConversion(NV12 → X8R8G8B8)` zwraca
  `D3DERR_NOTAVAILABLE` — a `StretchRect` wymaga właśnie konwersji.

### 3.3 Co z tego wynika dla klienta produkcyjnego (to jest trwała lekcja)

**Zasada, która trafi do kodu streamera:**

```
1. CheckDeviceFormatConversion(src, dst) PRZED jakimkolwiek StretchRect.
2. Jesli odmowa -> NIE wywoluj. Przejdz na nastepny szczebel lancucha.
3. Lancuch fallbackow, probebowany RAZ na starcie sesji i zapisany:
      NV12 StretchRect  ->  YUY2 StretchRect  ->  X8R8G8B8 StretchRect  ->  GDI SetDIBits
4. Kazda operacja D3D9 objeta SEH (__try/__except na x64) + TestCooperativeLevel
   przed i po; przy AV trwale degradujemy do nastepnego szczebla, nie probujemy ponownie.
5. Prezentacja w OSOBNYM PROCESIE od logowania/katalogu - crash drivera nie gubi tokenow.
```

Punkt 5 jest istotny architektonicznie: crash w `igdumd64.dll` jest **realnym**
scenariuszem na tym sprzęcie, a nie teorią — właśnie go zaobserwowaliśmy. Rozdzielenie
procesów (Python TUI ↔ C streamer) dostajemy z architektury z `00-WERDYKT.md` §6,
więc ta awaria jest nieprzyjemna, ale niegroźna.

### 3.4 Co zmieniłem w T4 v3

| Zmiana | Cel |
|---|---|
| **Izolacja etapów**: harness uruchamia 14 osobnych procesów (`info`, `formats`, `device`, `surfaces`, `updatesurf`, `stretch --case=0..4`, `bench --which=0..3`) | Kod powrotu 0xC0000005 wskazuje **dokładnie** winną operację; wszystkie wcześniejsze wyniki zostają |
| **Przyrostowy zapis**: plik otwarty od początku, po każdym etapie dopisek + uszczelnienie `"]}"` + `fflush` | Plik na dysku jest **poprawnym JSON-em aż do miejsca crasha** |
| **`CheckDeviceFormatConversion` przed `StretchRect`**, z odmową wywołania i zapisaniem decyzji w JSON | Nie prowokujemy AV; jednocześnie mierzymy, czy sterownik w ogóle pozwala |
| **Kolejność od najbezpieczniejszej**: `stretch` case 0 = X8R8G8B8 (na pewno działa) → YUY2 → NV12 → NV12+RESTRICTED_CONTENT → NV12→RGB offscreen | Jeśli NV12 zabija driver, mamy już zmierzone RGB i YUY2, czyli **gotowy plan B** |
| `TestCooperativeLevel` przed każdym etapem | Wykrywa zgubione urządzenie zamiast crashować |
| `--iters` konfigurowalne (domyślnie 200, `--quick` = 40) | Mniej czasu w potencjalnie crashującym benchmarku |
| `surface_descs`: `GetDesc` + `LockRect` z `D3DLOCK_NOSYSLOCK` i bez → **pitch** | Dekoder musi znać pitch powierzchni, żeby poprawnie zapisać NV12 |
| Poprawione ostrzeżenia (`fmt_name` bez `case D3DFMT_NV12` w switchu po enumie, usunięty `jkey_raw`) | Czysta kompilacja z `-Wall -Wextra` |
| `bench` rozdzielony na 4 osobne procesy, w tym `bench_gdi` | Nawet jeśli NV12 i YUY2 padną, **zmierzymy GDI**, czyli fallback ostateczny |

---

## 4. Rejestr ryzyk — aktualizacja

| # | Ryzyko | Runda II | Runda III | Uzasadnienie |
|---|---|---|---|---|
| **R1** | CPU nie wyrabia | średnie/krytyczne | **ZAMKNIĘTE** | 720p30 = 24,8 %; z mnożnikiem 1,8× za treść grową = ~45 %. Zapas duży. |
| **R4** | brak VT w conhost | częściowo zamknięte | **ZAMKNIĘTE** | T5: dowód w obie strony. D1 potwierdzona, detekcja hosta zaprojektowana. |
| **R2** | D3D9 NV12 `StretchRect` odrzucony | wysokie/wysokie | **podwyższone do wysokie/krytyczne** | v1 zcrashował na AV — prawdopodobnie właśnie tu. v3 zmierzy to bezpiecznie i ma gotowy łańcuch fallbacków. |
| **R17** *(nowe)* | Skalowanie na CPU zjada ~18 pkt proc. | — | **wysokie/wysokie** | Pomiar: `scale=1280:800` dokłada 11,8 ms/klatkę. Jeśli T4 wykluczy skalowanie na GPU, 720p30→800 spada z 25 % do 43 %. Mitigacja: prezentacja 1:1 bez skalowania (720p w oknie 1280×800 = letterbox, nie stretch). |
| **R18** *(nowe)* | Mysz SGR w ConEmu niepotwierdzona | — | **średnie/niskie** | Test niemiarodajny (`msvcrt.getwch`). Fallback: nawigacja klawiaturą (VK kompletne w T3). Do zmierzenia w T7. |
| **R19** *(nowe)* | truecolor kwantyzowany do 16 kolorów | — | **pewne/niskie** | Zmierzone (0x9 zamiast RGB). Mitigacja: `color_system="standard"` jawnie; UI bez subtelnych odcieni. |
| **R15** | zależności Pythona (brak rich/textual) | średnie/średnie | **bez zmian** | `pip list` = 2 pakiety. Potrzebny pinowany `requirements.txt` cp38 + lokalne wheele. |
| **R9** | DPST zbija odświeżanie w trakcie sesji | zmienione | **bez zmian** | T4 v3 ma `EnumDisplaySettingsW` + listę trybów 1280×800 → potwierdzi, czy 60 Hz jest osiągalne. |

Ryzyka zamknięte wcześniej: **R6** (toolchain), **R3 częściowo** (openh264/NASM nieaktualne po D3).

---

## 5. Stan bramek

| Bramka | Stan |
|---|---|
| G0.1 CPU: czy 720p30 mieści się w ≤70 % | ✅ **ZALICZONA** (24,8 %, z zapasem 2,8×) |
| G0.3 toolchain produkuje binarki działające na Win7 | ✅ **ZALICZONA** (runda II) |
| G0.4 konsola/VT → wariant TUI | ✅ **ZALICZONA** (ConEmu wymagany, detekcja zaprojektowana) |
| G0.2 D3D9: ścieżka prezentacji | ⏳ **OTWARTA** — czeka na T4 v3 |
| G0.5 openh264 vs ffmpeg | ➖ **ANULOWANA** (D3: FFmpeg) |
