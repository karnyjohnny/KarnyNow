# SETUP.md — środowisko, zależności i bramki testowe

> **Dla kogo jest ten plik.** Dla operatora (Win7, Dell Latitude E5500) **i** dla
> każdej przyszłej sesji asystenta, która otworzy to repo w nowym chacie. Czyta się
> go razem z `ORIENTATION.md` (co i dlaczego) oraz `START-HERE.md` (baza wiedzy
> o projekcie). Ten plik odpowiada wyłącznie na pytanie: **jak tu się buduje i testuje
> kod, co trzeba doinstalować i jak nie zmarnować wieczoru.**
>
> Zasada nadrzędna całego projektu: **piszemy do operatora idiotoodpornie** — krok po
> kroku, z dokładnymi komendami, z jawną informacją co sprawdzić i ile tego ma być.
> Żadnego „zrób jak uważasz". Jeśli instrukcja wymaga myślenia, jest zła.

---

## 1. Są DWA środowiska i nigdy ich nie myl

| | **SANDBOX** (Arena, Linux) | **RIG** (operator, Windows 7 SP1 x64) |
|---|---|---|
| co to | kontener, w którym powstaje kod i patch | Dell Latitude E5500, jedyna maszyna, która lata |
| persystencja | **tylko katalog workspace** przetrwa między rundami; wszystko co w `/usr`, `/tmp` i poza workspace **znika** | wszystko zostaje |
| kompilator | gcc 12 (host) + mingw-w64 x86_64/i686 (cross) | w64devkit / mingw GCC 16.2.0 przez CMake+Ninja |
| uruchamianie binarek Windows | **NIEMOŻLIWE** (tylko kompilacja i linkowanie) | tak — tu dzieje się prawda |
| bramka | `tools/cross_check.sh`, `tools/build_native_tests.sh`, `tools/check_summary_skeleton.py`, `tools/kn_launcher/test_launcher.py` | `tests/T10_build_and_test.py` (+ `T12`, `T13`) |
| wynik rundy | plik `.patch` z `git format-patch` | `logs/T10_skrot.txt` + logi lotów do repo `karnynow-logs` |

**Konsekwencja, którą trzeba pamiętać zawsze:** sandbox potrafi *udowodnić*, że kod się
kompiluje, linkuje i że matematyka jest poprawna. **Nie potrafi udowodnić, że sesja
poleci.** Dlatego każda runda kończy się poleceniem „REBUILD + T10 na rigu".

---

## 2. SANDBOX — postawienie środowiska od zera (nowy chat)

### 2.1 Jedna komenda na start

```bash
apt-get update && apt-get install -y --no-install-recommends \
  git ca-certificates build-essential cmake ninja-build pkg-config \
  gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 \
  python3 python3-pyqt5 python3-httpx libopus-dev
```

Co daje każdy pakiet i co się zepsuje bez niego:

| pakiet | po co | objaw braku |
|---|---|---|
| `build-essential` | `build_native_tests.sh` (host gcc, make) | bateria natywna nie ruszy |
| `cmake`, `ninja-build` | leg usrsctp w `cross_check.sh` (cross-build + link) | leg się pominie/wywali; bez usrsctp i tak się SKIP-uje |
| `gcc-mingw-w64-x86-64` | **bramka cross ×2**, cel x86_64 | `cross_check.sh` nie ruszy |
| `gcc-mingw-w64-i686` | druga noga cross (i686 wyłapuje inne błędy niż x64 — historycznie `port undeclared` w kn_net_mbedtls widział tylko i686) | cross-check nie jest „×2" |
| `python3-pyqt5` | `test_launcher.py` (GUI: okno, przyciski, model wariantów) | testy launchera padną na imporcie |
| `python3-httpx` | `kn_launcher/catalog.py` + mock transport w testach | testy katalogu padną |
| `libopus-dev` | `build_native_tests.sh` wykrywa `/usr/include/opus/opus.h` i kompiluje **prawdziwe** ciało `kn_opus` + linkuje `-lopus` | bez niego buduje się uczciwy stub i `test_knopus` sam pomija strumień wektorowy |

Stan referencyjny (Debian 12, tak było w rundzie slice 4r): Python **3.11.2**, gcc
**12.2.0**, cmake **3.25.1**, mingw **GCC 12-win32** (x86_64 i i686), httpx **0.23.3**,
PyQt5 obecne, `libopus-dev` obecne, **ninja BRAK** (nie przeszkadza, bo drzewo usrsctp
jest nieobecne i leg się SKIP-uje).

### 2.2 Zależności C: `third_party/` (gitignored, staging lokalny)

`cross_check.sh` i `build_native_tests.sh` **bezwarunkowo** kompilują
`third_party/tiny-aes-c/aes.c` i `third_party/crypto-algorithms-sha1/sha1.c`.
Klon repo ich nie zawiera. Najprostsza i **najpewniejsza** recepta — wołamy tę samą
funkcję, której używa rig, więc bajty są identyczne z przypiętymi i zweryfikowane SHA-256:

```bash
python3 -c "
import sys; sys.path.insert(0,'tests')
import T10_build_and_test as t
r=[]; print('staged ok:', t.ensure_vendor_files(r))
"
```

Oczekiwany wynik:

```
=== required source dependencies ===
PASS third_party/tiny-aes-c/aes.c downloaded and SHA-256 verified
PASS third_party/tiny-aes-c/aes.h downloaded and SHA-256 verified
PASS third_party/crypto-algorithms-sha1/sha1.c downloaded and SHA-256 verified
PASS third_party/crypto-algorithms-sha1/sha1.h downloaded and SHA-256 verified
staged ok: True
```

Przypięte wersje (te same co na rigu, patrz `README.md` §Third-party): tiny-AES-c
`23856752f` · crypto-algorithms `cfbde484` · usrsctp `c625e219` · mbedTLS 3.6.0
`2ca6c285` + framework `750634d3` · opus 1.5.2 (tarball, SHA-256 `65c1d2f7…a7ce1`).

**Nie klonuj tych repo „z HEAD"** — sandbox i rig muszą mieć te same bajty, inaczej
bramka w sandboxie nie mówi nic o rigu.

Drzewa opcjonalne (bez nich odnośne legi grzecznie się SKIP-ują i to jest stan
normalny, nie błąd):
- `third_party/mbedtls` → `tools/prepare_mbedtls_sandbox.sh` (instaluje też toolchain)
- `third_party/ffmpeg-headers` → `python3 tools/fetch_ffmpeg_headers_sandbox.py`
  (ściąga nagłówki FFmpeg z commitu `bf0e831606` — leg „kn_decode/kn_selftest vs
  REAL FFmpeg headers")
- `third_party/usrsctp` → tylko z `ninja-build`; leg cross-linkuje `test_knsctp_real`
- `third_party/opus` → alternatywa dla systemowego `libopus-dev`

### 2.3 BRAMKI — kanoniczne skrypty, w tej kolejności

Nie wymyślaj własnych zestawów flag. Bramką są **te cztery komendy**:

```bash
# 1) cross ×2 (x86_64 + i686): wszystkie obiekty kncore, wszystkie testowe .exe,
#    PEŁNE statyczne linkowanie knvstd_*.exe, paranoid leg.
#    -std=c11 -O2 -Wall -Wextra -Werror -march=core2 -msse3 + DEFS Win7 (0x0601)
sh tools/cross_check.sh
#    oczekiwane ostatnie zdanie: CROSS-CHECK PASS (zero warnings under -Wall -Wextra -Werror)

# 2) bateria natywna: 26 suite'ów + paranoid leg knvstd + kontrakt json-lines
#    -std=c11 -O1 -g -Wall -Wextra -Wpedantic
sh tools/build_native_tests.sh
#    oczekiwane ostatnie zdanie: NATIVE TESTS PASS

# 3) skeleton sumary — wymaga ŻYWEGO stdout z lotu (patrz §2.5)
python3 tools/check_summary_skeleton.py . /tmp/live_d1.log
#    oczekiwane: skeleton recursive keys: 113 / live recursive keys: 113 / PASS

# 4) launcher (PyQt5 + httpx), zawiera w sobie guard py3.8
python3 tools/kn_launcher/test_launcher.py
#    oczekiwane: kn_launcher tests: 168 checks, 0 failures -> PASS
```

Liczby referencyjne na stan slice 4r (commit `af826fb`): `test_kncm` **267/0**,
`test_kninput` **802/0**, `test_kndrawpath` 1051/0, `test_knfreeze` 865/0, `knbw` 387/0,
`test_knrx` 218/0, `test_knogg` 163/0, `test_knrtsp` 150/0, `test_knfec` 125/0,
`test_kngs` 125/0, `test_knselftest` 118/0, `test_kndiag` 109/0, `test_knopus` 105/0,
`test_knlog` 105/0, `test_knsock` 102/0, `test_knws` 88/0, `test_knsrtp` 77/0,
`test_knaudio` 74/0, `test_knd3d` 65/0, `test_knarx` 62/0, `test_knwire` 52/0,
`test_knring` 34/0, `test_knsctp` 13/0, `test_kndtls` 6/0, `test_kncert` 4/0
= **25 suite'ów**, skeleton **113** kluczy, launcher **156/0**.

**Bramka 1 i 2 NIE są redundantne.** `cross_check.sh` nie ma `-Wpedantic`, więc nie
widzi `-Woverlength-strings`; `build_native_tests.sh` ma. W slice 4r tekst helpa
rozepchał piąty `printf` w `usage()` do 4201 znaków (limit C99 = 4095) i **cross-check
przeszedł, a natywna bramka nie**. Uruchamiaj obie, zawsze.

### 2.4 Ręczne budowanie jednego testu (gdy nie chcesz całej baterii)

Lista źródeł `kncore` jest **jawnie wypisana w `CMakeLists.txt`** — bierz ją stamtąd,
nie z pamięci. Nazwy plików to `knlog.c`, `knring.c`, `knfec.c`… (**nie** `kn_log.c` —
taki plik nie istnieje i gcc wypluje `No such file or directory`):

```bash
SRC=$(awk '/^add_library\(kncore STATIC/,/^\)/' CMakeLists.txt \
      | grep -oE "streamer/src/[a-z0-9_]+\.c" | tr '\n' ' ')
gcc -std=gnu11 -O1 -o /tmp/test_kncm streamer/tests/test_kncm.c $SRC \
    third_party/tiny-aes-c/aes.c third_party/crypto-algorithms-sha1/sha1.c \
    -Istreamer/include -Istreamer/src -Ithird_party/tiny-aes-c \
    -Ithird_party/crypto-algorithms-sha1 -lm -lpthread -Wall -Wextra
/tmp/test_kncm | tail -2
```

Uwaga: **nie** dokładaj do tej listy plików spoza `kncore` (np. `knvstd_main.c`) i nie
mieszaj z testem, który sam includuje `.c` — `test_kninput.c` i `kn_input.c` razem dają
`multiple definition of kn_input_kat_*`.

### 2.5 Logi lotów: repo `karnynow-logs`

Logi operatora są w osobnym, **publicznym** repo `karnyjohnny/karnynow-logs` (foldery
`run103_logs/1/…`). Jak je czytać z sandboxa, żeby nie zabić sesji:

```python
# Ściągnij JEDEN plik przez raw (nie klonuj!)
import urllib.request
u="https://raw.githubusercontent.com/karnyjohnny/karnynow-logs/main/run103_logs/1/run103.log"
open("/tmp/live_d1.log","wb").write(urllib.request.urlopen(
    urllib.request.Request(u, headers={'User-Agent':'kn'}), timeout=120).read())
```

Pułapki sprawdzone na własnej skórze:
- **`curl` w sandboxie NIE ISTNIEJE** — używaj `urllib` z Pythona.
- **`git clone --filter=blob:none` na tym repo wpada w pętlę `Auto packing the
  repository in background` i kończy się timeoutem.** Nie rób tego. Albo API
  (`api.github.com/repos/…/contents/…`) + raw, albo zwykły shallow clone do `/tmp`.
- Duże pliki (log lotu bywa 5–13 MB) czytaj **Range'em**: nagłówek
  `{'Range':'bytes=5700000-'}` daje ogon z summary, `bytes=0-400000` daje początek
  z pierwszymi rekordami kontrolnymi. Nie ściągaj całości, jeśli potrzebujesz
  dwóch linii.
- Po użyciu **kasuj** (`rm -rf /tmp/…`) — patrz §2.6.

### 2.6 Porządki (to jest wymaganie operatora, nie kosmetyka)

Poprzedni sandbox **umarł** od nadmiaru plików (przeciążenie dysku po stronie
przeglądarki). Dlatego:

```bash
rm -rf third_party build logs          # artefakty bramek w repo (gitignored, ale ważą)
rm -rf /tmp/knb /tmp/lg /tmp/*.log     # wszystko co ściągnięte
find . -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null
git status --short                     # MUSI pokazać tylko te pliki, które celowo zmieniasz
```

Co przetrwa w snapshotcie workspace: wszystko **poza** `.git`-wykluczeniami platformy
(`build`, `dist`, `out`, `target`, `__pycache__`, `.venv`, `node_modules`, `.cache`…).
**`third_party/` NIE jest na liście wykluczeń** — więc zostawione tam drzewa vendorowe
zapiszą się w snapshotce i zaśmiecą workspace. Kasuj je przed końcem rundy.

Stan docelowy workspace po rundzie: `kn/` (repo) + **jeden** plik `.patch`. Nic więcej.

---

## 3. RIG — co tam się dzieje i czego oczekiwać

### 3.1 `tests/T10_build_and_test.py` — bramka docelowa (najważniejsza)

Czysty stdlib Pythona 3.8, uruchamiany **na rigu**:

```bat
cd /d "%USERPROFILE%\Desktop\KarnyNow"
python tests\T10_build_and_test.py
```

Co robi, po kolei:
1. **sam pobiera i weryfikuje SHA-256 zależności** (`ensure_vendor_files`:
   tiny-AES-c + crypto-algorithms), a potem przypięte tarballe: **mbedTLS 3.6.0**
   (+ framework), **usrsctp**, **opus 1.5.2**. Operator **nie musi nic doinstalowywać
   ręcznie** — to jest odpowiedź na pytanie „skąd się biorą zależności".
2. `cmake` configure + `ninja` build (oczekiwane **zero ostrzeżeń**),
3. uruchamia binarki testów jednostkowych,
4. `knvstd --list-selftests` i `--selftest=all` — **prawdziwe wykonanie Win32**
   (`clock.resolution`, `console.host`, `d3d.formats`, `d3d.present`,
   `decode.synthetic`); sandbox potrafi je tylko skompilować,
5. `knvstd --diagnostics` → bundel walidowany niezależnym readerem `zipfile`
   (summary.json pierwszy, CRC, wymagane wpisy, `build.json` pokazuje efektywny
   `_WIN32_WINNT`),
6. `tools/check_jsonl.py` po katalogu logów tego przebiegu,
7. sprawdzenie wersji PE subsystem w `knvstd.exe` (**musi być 6.01 = Win7**).

Wyniki: `logs\T10_wynik.txt` (pełny) i `logs\T10_skrot.txt` (mały verdict).
**Odsyłaj oba**; jeśli JSON jest za duży do czatu — sam skrót.

Oczekiwany verdict na stan slice 4r: **ALL PASS 89** (na buildzie F41). Dla porównania:
build z epoki run-46 dawał **87**. Liczba checków to szybki sposób, żeby potwierdzić,
**który commit naprawdę się zbudował** — przy bisekcie patrz na to zawsze.

**FFmpeg NIE jest pobierany przez T10.** Skrypt sonduje `KN_FFMPEG_PREFIX` i listę
ścieżek kandydujących; drzewo dev (statyczne, `--enable-static`, bo link `-static`
ignoruje `.dll.a`) dostarcza operator — zwyczajowo `build/ffmpeg/`.
**Sprawdzenie, czy build ma dekoder:** w logu lotu pole `"decoder"` musi być
`libavcodec …`, a `"video"` musi być `1280x720`. Jeśli widzisz `"pipeline-only"` i
`"0x0"` — build jest bez FFmpeg, **taki lot się nie liczy** (dokładnie tak wyglądał
run 200: `video_present_end reason:"never_opened"`, `frames_decoded:0`).

### 3.2 Pozostałe testy na rigu

| test | kiedy | koszt |
|---|---|---|
| `tests\T12_network.py` | szybka iteracja backendu sieciowego: buduje tylko `knvstd` i odpala `net.dns`, `net.tcp443`, `net.tls`, `tls.chain` | ~2 min |
| `tests\T13_udp_e2e.py` | cała ścieżka stage-2 lokalnie, bez konta GFN (nadajnik UDP → `--capture` → `--replay-capture` → `--decode`) | ~3 min |
| `tests\T1…T8` | bramki historyczne (dekoder, toolchain, konsola, D3D9, zależności Pythona) — **wykonane, wyników nie powtarzaj** bez potrzeby | patrz `tests/README.md` |

### 3.3 Launcher GUI (slice 5c)

Na rigu: **dwuklik w `launcher.bat`** w katalogu głównym repo (albo
`python -m tools.kn_launcher`) — nie trzeba już ręcznie pisać ścieżki modułu.
`.bat` znajduje Pythona przez `where python` (fallback `py -3`) i startuje GUI
z korzenia repo. Końce linii: blob w git jest LF, a `.gitattributes`
(`/launcher.bat text eol=crlf`) daje **CRLF przy każdym świeżym checkoucie**.
Uwaga: `git am` zostawia w drzewie wersję LF (apply nie robi konwersji eol) —
dla tego skryptu jest ona bezpieczna (brak własnych etykiet, tylko `goto :eof`);
CRLF od razu materializuje `del launcher.bat & git checkout -- launcher.bat`
(same `checkout-index -f` istniejącego pliku NIE nadpisuje — zmierzone na git 2.39;
świeży klon/repozytorium po `git am` od tego commitu ma już atrybuty, więc każdy
kolejny świeży checkout da CRLF).
Bateria sprawdza i bajty `.bat`, i przypięcie w `.gitattributes`.

Zmiany slice 5c (wszystkie kryte `test_launcher.py`, **168/0**):
- **Folder logów może mieć DOWOLNĄ nazwę** — `runB2a`, `runB545`, co chcesz
  (reguła `runNNN_logs` została jako sama *sugestia*). Nazwy, które nie mogą
  być katalogiem na Windows (`<>:"/\|?*`, kropka na początku/końcu, `con`,
  `nul` itd.), są odrzucane **z podanym powodem** w podpowiedzi nad polem.
- **Drawy zostają**: `logs\runB2a\1`, `logs\runB2a\2`, … — pamięć ostatniego
  runa (F42) działa też dla wolnych nazw, więc restarty doklejają drawy
  do tego samego folderu.
- **„owned only" zaznaczone domyślnie.**
- Pole parametrów ma **domyślnie wpisaną kartę B2**: `--cm-profile=browser
  --webrtc-answer=45 --session-queue-timeout=1500` (reszta starej receptury
  za nimi — edytujesz pod lot).
- Każdy draw zapisuje u siebie `console.log` (było `runNNN.log`),
  `wire_r1.txt` (`--wire-dump` dodawany automatycznie) i `manifest.txt`.

Zasady z `tests/README.md`, których trzeba przestrzegać:
- **ConEmu, nie czysty `cmd.exe`** (decyzja D1): bez `ConEmuHk64.dll` sekwencje ANSI
  niszczą layout.
- **Nie przekierowuj stdout do pliku** przy testach konsolowych (T3) — potrzebują
  prawdziwego uchwytu konsoli, inaczej „stdout is not a console handle".
  (Przy lotach `knvstd` przekierowanie **jest** wymagane i poprawne.)
- Python na rigu to **3.8.20**; każdy moduł launchera musi się pod tym parsować —
  pilnuje tego guard w `test_launcher.py` (`ast.parse(..., feature_version=(3,8))`
  + `from __future__ import annotations`).
- Koło w `cmd` zamiast w ConEmu, mysz w TUI: decyzje D10/D11 (tylko klawiatura,
  resize z pollingu).

---

## 4. Jak wygląda jedna runda (kontrakt dostawy)

1. **Kod + testy w sandboxie.** Zmiana, nowe checki w odpowiednim `streamer/tests/test_*.py|c`,
   wszystkie cztery bramki (§2.3) zielone.
2. **Dokumentacja.** `docs/RTSP.md` (nowy §, zapis ustaleń z logów), `START-HERE.md`
   (nowy §, streszczenie dla operatora), `docs/INTEGRATION.md` (nowy wiersz slice —
   kanon bramek i lista plików). Ten plik + `ORIENTATION.md` aktualizuj tylko wtedy,
   gdy zmieniło się środowisko albo metoda pracy.
3. **Commit na `main`** i `git format-patch -1 HEAD -o <workspace>` → **jeden** plik
   `.patch` do pobrania. Nie mamy zapisu na GitHub, więc patch jest jedyną dostawą.
4. **Weryfikacja patcha** na świeżym klonie: `git clone`, `git checkout <baza>`,
   `git config user.email/name` (bez tego `git am` pada: „Committer identity unknown"),
   `git am <patch>`, potem `diff -r --exclude=.git` z drzewem roboczym — musi być
   identyczne.
5. **Rig:** `git am` → REBUILD → `T10` (oczekiwane ALL PASS 89) → lot → logi do
   `karnynow-logs` → raport.

---

## 5. Ściągawka (kopiuj-wklej dla nowej sesji)

```bash
# 0) gdzie jestem
cd <workspace>/kn && git log --oneline -1 && git status --short

# 1) toolchain
apt-get update && apt-get install -y --no-install-recommends \
  git ca-certificates build-essential cmake ninja-build pkg-config \
  gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 \
  python3 python3-pyqt5 python3-httpx libopus-dev

# 2) przypięte zależności C (te same bajty co na rigu, SHA-256 zweryfikowane)
python3 -c "
import sys; sys.path.insert(0,'tests')
import T10_build_and_test as t
r=[]; print('staged ok:', t.ensure_vendor_files(r))
"

# 3) bramki
sh tools/cross_check.sh          # -> CROSS-CHECK PASS ... x2
sh tools/build_native_tests.sh   # -> NATIVE TESTS PASS (26 suite'ow)
python3 tools/kn_launcher/test_launcher.py   # -> 168 checks, 0 failures

# 4) skeleton (potrzebuje zywego stdout z karnynow-logs, patrz SETUP §2.5)
python3 tools/check_summary_skeleton.py . /tmp/live_d1.log   # -> 113 == 113 PASS

# 5) porzadki przed koncem rundy
rm -rf third_party build logs && find . -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null
git status --short               # tylko celowe zmiany
```
