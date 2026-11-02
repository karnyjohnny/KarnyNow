# ORIENTATION.md — o co tu chodzi, czemu testy, i co Cię ma interesować

> **Dla kogo.** Dla operatora i dla każdej nowej sesji asystenta. To jest warstwa
> „dlaczego i w jakim stanie jesteśmy". Warstwa „jak uruchomić" jest w `SETUP.md`,
> a baza wiedzy o projekcie w `START-HERE.md`.
>
> Kolejność czytania w nowym chacie: **`START-HERE.md` (ostatnie §) → ten plik →
> `SETUP.md` → `docs/RTSP.md` (ostatni §) → `docs/INTEGRATION.md` (ostatni slice).**

---

## 1. Co to jest KarnyNow i dlaczego C + PyQt5 (a nie to, co byśmy chcieli)

KarnyNow to **własny klient strumieniowania GeForce NOW**, napisany od zera: negocjacja
sesji przez CloudMatch (HTTPS), sterowanie RTSP, DTLS-SRTP dla mediów, SCTP dla wejścia
i kanału kontrolnego, dekodowanie H.264 (FFmpeg), prezentacja przez Direct3D 9, dźwięk
Opus/WASAPI, wejście (mysz/klawiatura) i pełna telemetria w JSON-lines.

Dlaczego taki dziwny stack:

| warunek | skutek |
|---|---|
| rig to **Windows 7 SP1 x64**, Dell Latitude E5500 | odpada wszystko, co porzuciło Win7 |
| **Rust nie działa** na tej maszynie | **nie możemy uruchomić OpenNOW** — a to jest referencyjny klient GFN w Ruście, którego semantykę, układy na drucie i goldeny testowe ten projekt reimplementuje (commit `a49b124ce31e`; **żaden kod OpenNOW nie jest kopiowany** do tego repo) |
| **PyQt6 nie działa** na Win7 | launcher jest w **PyQt5**, Python **3.8.20** |
| mingw-w64 GCC + `-static` + subsystem 6.01 | klient jest w **C11**, cel Win7 |

Stąd cały projekt: **C dla klienta, PyQt5 dla GUI, Python 3.8 dla harnessów.**
Każdy pomysł „a może przepiszymy to na X" musi najpierw odpowiedzieć na pytanie,
czy X w ogóle uruchamia się na Win7.

## 2. Drabina celów i gdzie jesteśmy

| cel | treść | stan |
|---|---|---|
| **G0** | klient startuje, loguje się, wchodzi w sesję | **ZROBIONE** |
| **G1** | ręczne logowanie + działający kursor w grze | **ZROBIONE** (F38/F39 dowiedzione na żywo w run 103) |
| **G2** | **utrzymać 60-minutową sesję free-tier** (`stop_reason:"time"` przy 3600 s) | **NIE OSIĄGNIĘTE od runu 61** |

G2 był osiągnięty **trzy razy w historii**: run 46 draw b (**1020,0 s**, `stop_reason:"time"`
— sesję zakończył nasz własny zegar, nie chmura), oraz **dwa keepery po 1200 s** w runach
60 i 61. Od tamtej pory: run 102 max 317,6 s → run 103 max 21,0 s → run 104 max 15,9 s →
run 105 max 14,7 s.

## 3. Dlaczego testy są osią projektu, a nie dodatkiem

Sandbox **nie uruchomi** binarki Windows. Rig uruchamia, ale **każdy lot na rigu kosztuje
operatora czas i kolejkę** (2–5 min samego queue, a chory lot i tak umiera w 12 s). Dlatego
obowiązuje trójwarstwowa weryfikacja i żaden kod nie jedzie na rig bez zielonych dwóch
pierwszych warstw:

| warstwa | gdzie | co dowodzi | koszt |
|---|---|---|---|
| **1. native** | sandbox, gcc host | matematyka i logika są poprawne (SRTP, FEC, ARX, OGG/Opus, input, freeze, bwe, CM…) — 25 suite'ów, tysiące checków, w tym goldeny KAT | sekundy–minuty |
| **2. cross** | sandbox, mingw x86_64 + i686 | kod **kompiluje się i linkuje** z flagami celu Win7 (`-std=c11 -O2 -Wall -Wextra -Werror -march=core2 -msse3`, `_WIN32_WINNT=0x0601`, `-static`, subsystem 6.01); i686 wyłapuje rzeczy, których x64 nie widzi | ~3 min |
| **3. T10** | **rig** | binarka **naprawdę działa** na Win7: self-testy D3D/dekodera/zegara/konsoli, bundel diagnostyczny, PE subsystem | ~10 min |
| **4. lot** | rig + konto GFN | jedyna prawda o tym, czy sesja żyje | wieczór |

Zasada, która z tego wynika i której nie wolno łamać: **jedna zmienna na lot.** Jeśli
w jednym locie zmieniasz build, flagi i porę dnia, nie dowiesz się niczego — i właśnie
dlatego run 105 był lotem **przeplatanym** (r46, native, r46, native…), a nie „najpierw
wszystkie A, potem wszystkie B". Pula się rusza pod nami: run 102 → 103 zdegradował
z 317,6 s do 21,0 s **bez żadnej zmiany w kodzie**.

## 4. Co ma interesować operatora w logach (i co ignorować)

Każdy lot pisze JSON-lines na stdout. Pola, które **rozstrzygają**:

| pole | gdzie | co mówi |
|---|---|---|
| **`["0101",N]`** (heartbeaty riga) | `ctrl_summary` / `ctrl_in` → `codes` | **WYROCZNIA.** Rig, który ma żyć, bije ~1 Hz od pierwszej sekundy. Keeper z run-46: **1001** heartbeatów (8 już po 10 s). Dziś HEAD dostaje **0–2** i umiera. To widać po 10 sekundach lotu, nie po 20 minutach |
| `stop_reason` + `wall_ms` | `summary` | `time` = KEEPER (nasz zegar); `ctrl_dead` / `rig_zombie` / `recovery_exhausted` = śmierć |
| `last_video_rel_ms`, `last_ctrl_rel_ms` | `freeze_witness` | **niezależne od naszych wyroków**: jak długo rig naprawdę nadawał. `wall_ms` jest zanieczyszczony tym, że to my podejmujemy decyzję o końcu |
| `frames_observed`, `frames_decoded`, `video` | `bwe`, `summary` | czy media szły i czy build w ogóle dekoduje |
| `"decoder"` | `summary` | `libavcodec …` = dobrze; **`pipeline-only` = build bez FFmpeg, lot się nie liczy** |
| `cm_created` → `status`/`phase`/`resumed` | event `cm_created` | `phase:"resuming"`/`resumed:1` = **wznowiliśmy cudzy martwy seat**, a nie wzięliśmy świeży z kolejki |
| `ready` → `rtsps`, `relearns` | event `ready` | który rig i ile hopów kontrolnych |
| `cm_release` → `rc`, `http_status`, `attempts`, `host_fallback` | event `cm_release` | czy zwolniliśmy seat. `rc:-2` = seat osierocony → następny draw może wznowić trupa |

Czego **nie** trzeba czytać: `pongs:0`. Keeper z run-46 miał `pings:504, pongs:0` —
rigi po prostu nie odpowiadają na nasze pingi kontrolne i to jest norma, nie objaw.

Do czytania logów służy `tools/bisect_row.py` (wyciąga z pliku jeden wiersz raportu;
Python 3.8, bez zależności):

```bat
python tools\bisect_row.py logs\bisect_a339b52_1.log
```

## 5. Stan wiedzy na dziś (slice 4r, commit `af826fb`)

### 5.1 Co jest **wykluczone** (każde pomiarem, nie zgadywaniem)

| hipoteza | czym zabita |
|---|---|
| „wina stabilnego device-id" (H1) | run 104: LOT A na stabilnym `557e89aa…` zmarł w 12,1 s, LOT B na **świeżym** `b7e4d2a1…` w 15,7 s — oba martwe |
| „wina `relearns`" | długie drawy z run-102 miały 17/5/22 relearns, krótkie 0–21; brak korelacji |
| „wina interactive input" | runy 60/61 zrobiły **dwa keepery 1200 s** mając interactive już w drzewie |
| **„wina nagłówków tożsamości F37”** | **run 105, przeplatany A/B 3×3, ten sam wieczór, to samo urządzenie:** r46 → 14,4 / 12,1 / 12,1 s, native → 14,5 / 12,1 / 14,7 s. Rozkłady pokrywają się całkowicie |
| „wina puli / pory dnia (cała historia)" | **oficjalny klient GFN dał dziś operatorowi godzinę w Endfield** — serwer, konto, sieć i gra są zdrowe |

### 5.2 Tabela ledger — najdłuższy lot na run

| run | build | max | co wiemy |
|---|---|---|---|
| **46** | r46 (`ee75d37`) | **1174,8 s**, draw b **1020,0 s KEEPER** | 1001 heartbeatów, `libavcodec`, 30 571 klatek, `cm_release rc:0 gone:1` |
| 48 | zaraz po `interactive session` | 14 s / 14 s | ta sama sygnatura co dziś |
| **60/61** | era między `9409213` a `a339b52` | **dwa keepery 1200 s** | ostatni znany dobry stan |
| 100/101 | F35 browser | bramka reklamowa, seat WebRTC-only | `conn_unusable` |
| 102 | F37 (pierwszy lot) | 317,6 s | |
| 103 | F37, bez zmian w kodzie | 21,0 s | dowód, że pula się rusza |
| 104 | F40 | 15,9 s | `rig_zombie` odpalił się na żywo i **miał rację** |
| 105 | F41 | 14,7 s | A/B tożsamości: **negatywny** |
| **200** | **r46, ten sam binarny co run 46** | **87,3 s** | patrz niżej |

### 5.3 Run 200 — eksperyment, który zmienił obraz

Operator zbudował commit `ee75d37` (r46) i poleciał nim **dziś**, tym samym wieczorem co
run 105. Świeży seat z kolejki (pozycja **133**), rig `185-136-70-136`, 23 relearns.
Wynik **87,3 s** — czyli **6× dłużej niż którykolwiek draw HEAD**, ale **12× krócej niż
1020 s tego samego binarnego dwa dni wcześniej.**

Szczegóły: heartbeaty `0101` = **73** (~1 Hz do 68,8 s), kontrolna żyła do 68,8 s, wideo
do 85,2 s, `terminations:0` (serwer nigdy nie przysłał zakończenia), `pongs:0` (norma),
enkoder riga zgasł do zera (`kbps:[…705,374,0]`), `cm_release rc:-2 attempts:2`.
**Uwaga na ten pomiar:** build był **bez FFmpeg** (`decoder:"pipeline-only"`,
`video:"0x0"`, `video_present_end reason:"never_opened"`, `frames_decoded:0`) — klient
odbierał, ale nic nie konsumował. Ten lot trzeba powtórzyć z dekoderem.

### 5.4 Dwa osobne pytania — i dwie osobne metody

| | pytanie | metoda |
|---|---|---|
| **Q1** | dlaczego **ten sam binarny** r46 robił 1020 s, a dziś robi 87 s? | bisect **nie odpowie** — to nie nasz kod. Odpowie przechwycenie tego, co dziś wysyła **oficjalny** klient (DevTools → Network → POST create → *Copy as cURL* + Response). Skoro oficjalny klient ma dziś godzinę, jego żądanie create jest prawdą objawioną o obecnym kontrakcie |
| **Q2** | dlaczego dziś r46 robi 87 s, a HEAD 14 s? | **bisect po commitach** — to jest czysto nasz kod |

Podejrzani w Q2 (kolejność po wykluczeniu nagłówków): dodatki F37 w **body** create
(`srMode/srModel/srScale/rrModel/fgMode/fgModel`, obiekt `dlssOverrides`, `appLaunchMode`,
świeży `SubSessionId` na lot), ścieżka kursora/wejścia F39, oraz `kn_d3d.c` (+805 linii
między r46 a HEAD).

## 6. Protokół bisectu (eksperyment bieżący)

**Kolejność commitów** — od największej informacji, nie chronologicznie:

| # | commit | data | co to jest |
|---|---|---|---|
| 1 | `a339b52` | 11-01 14:04 | **ostatni przed F37**; tu jest era keeperów 1200 s z runów 60/61 |
| 2 | `0c63a49` | 11-01 15:23 | slice 4o = F36 + **F37** (tożsamość + body) |
| 3 | `9409213` | 10-31 23:24 | r49 |
| 4 | `ee75d37` | 10-31 17:01 | **r46** — kontrola, tym razem **z FFmpeg** i n=3 |
| 5 | `af826fb` | 11-02 15:04 | HEAD — kontrola górna, **tą samą komendą** |

Para 1+2 testuje granicę F37 wprost: **#1 zdrowy a #2 chory → F37 winny, wieczór
skończony.** Oba chore → idź do 3 i 4. Oba zdrowe → do 5, a potem `00a8151` (4p).

**Trzy zasady budowania:**
1. Każdy commit budujesz **dokładnie tak jak HEAD**, z tym samym FFmpeg. Bez wyjątków —
   inaczej mierzysz obecność dekodera, nie kod.
2. **Nie używaj launchera** przy starych buildach: wstrzykuje flagi, których tam nie ma,
   i proces wyjdzie z kodem 2 (`usage`). Sama linia komend.
3. Przed serią sprawdź w logu `"decoder"` (musi być `libavcodec …`) i `"video"`
   (musi być `1280x720`).

**Komenda — identyczna dla każdego commitu** (ten zestaw flag istnieje we wszystkich
pięciu; niczego nie dodawaj — `--cm-profile`, `--device-id-file`, `--session-zombie-ms`,
`--wire-dump`, `--session-device` nie istnieją wszędzie):

```bat
build\knvstd.exe --session --session-token=<SWIEZY_TOKEN> --session-app=107057914 --session-seconds=120 --input-script=build\login_1h.txt --input-anti-afk=60000 --session-present --session-audio-play --session-release > logs\bisect_a339b52_1.log 2>&1
```

**Sondy, nie pełne loty:** `--session-seconds=120` i **3 drawy na commit**. Zdrowy build
da ~110 heartbeatów; chory da 0–2 i zdechnie w 12–15 s. Pełne `--session-seconds=1200`
lecisz **raz** — tylko na tym commicie, który wygrał, żeby złapać keepera.

Każdy draw przepuść przez `python tools\bisect_row.py logs\bisect_<commit>_<n>.log`
i wypełnij tabelę:

| commit | draw | seat | rig | heartbeaty `0101` | wall_ms | wideo do | kontrolna do | dekoder |
|---|---|---|---|---|---|---|---|---|
| | | | | | | | | |

Do raportu dochodzą: `create_request.txt` + `create_response.txt` z DevTools (Q1) oraz
jedno zdanie — ile minut dał oficjalny klient i czy to była przeglądarka, czy aplikacja.

## 7. Jak pracujemy (kontrakt między operatorem a asystentem)

- **Język i styl: po polsku, idiotoodpornie.** Krok po kroku, dokładne komendy, jawne
  „co sprawdzić i ile tego ma być". Bez wykładów o bezpieczeństwie tokenów (operator
  wyraźnie to zniósł z agendy).
- **Dostawa = jeden plik `.patch`** z `git format-patch` + dokumentacja. Nie mamy zapisu
  na GitHub — patch jest jedyną formą przekazania kodu. Baza patcha musi być jawnie
  podana (np. „nakłada się na `af826fb`").
- **Dokumentacja w tej samej rundzie co kod**: `docs/RTSP.md` (ustalenia z logów, nowy §),
  `START-HERE.md` (streszczenie dla operatora, nowy §), `docs/INTEGRATION.md` (wiersz
  slice + kanon bramek).
- **Logi lotów lądują w `karnyjohnny/karnynow-logs`** (publiczne, foldery `runN_logs/<draw>/`).
  Asystenta interesują: `T10_skrot.txt`, `T10_wynik.txt`, konsolowy `runN.log` każdego
  drawa, `wire_session.log`, manifest. Pliki `.264` są w `.gitignore` tamtego repo —
  dowody wideo wymagają `git add -f`.
- **Sandbox ma być chudy.** Poprzedni sandbox umarł od nadmiaru plików. Po każdej rundzie:
  `rm -rf third_party build logs`, skasować wszystko z `/tmp`, w workspace zostaje
  `kn/` + jeden `.patch`. Szczegóły: `SETUP.md` §2.6.
- **Uczciwość ponad narrację.** W tym projekcie trzy razy ogłoszono wyrok, który potem
  padł (H2 „loteria puli", potem „tożsamość F37"). Każde twierdzenie ma mieć pomiar,
  a jeśli pomiar jest z confoundem (jak run 200 bez dekodera) — confound musi być
  nazwany w tym samym zdaniu.

## 8. Ślepe zaułki (nie wracaj tu bez nowego pomiaru)

- Nie „naprawiaj" `pongs:0` — to norma.
- Nie generalizuj F40a na „cisza po heartbeacie" — F30 już to robi.
- Nie traktuj `relearns` ani `--device-id-file` jako zmiennej sprawczej — obie martwe.
- Nie wracaj do `--cm-profile=browser` jako drogi do G2: seat WebRTC (`:443 /nvst/`,
  usage 14 alp 5) w naszej implementacji dochodzi do bramki reklamowej i nie staje się
  strumieniowalny (run 101). Nasza ścieżka to native `:322`, usage 16.
- Nie planuj portu OpenNOW „na zaś": Rust nie uruchamia się na Win7, więc **referencyjny
  klient nie jest u nas osiągalny jako działający program** — jest tylko źródłem wiedzy
  o protokole. Port to tygodnie pracy, a sufit ustawia serwer, nie klient.
- Nie zakładaj z góry, że „wina puli": oficjalny klient działa dziś przez godzinę.
