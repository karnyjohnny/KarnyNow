# Przegląd istniejącego kodu logowania/katalogu/sesji (D12)

**Data:** 2026-10-20 · **Przejrzane pliki:** `endpoints.py` (545 l.), `oauth.py` (795 l.),
`gql_queries.py` (116 l.), `adapter.py` (1551 l.) — razem ~3 000 linii.
**Metoda:** każde twierdzenie opatrzone `evidence` w Waszym kodzie zostało **niezależnie
zweryfikowane** wobec źródeł OpenNOW `main` (commit `a49b124`), które mam lokalnie.
Poniżej: co się potwierdziło, co jest dobrze zrobione, i **gdzie są luki względem tego,
czego będzie potrzebował klient C**.

---

## 0. Werdykt w trzech zdaniach

Kod jest **bardzo dobrej jakości metodologicznej** — dyscyplina dowodów (`evidence` z
plikiem/symbolem/linią + `confidence`), jawny rejestr `UNKNOWN_ENDPOINTS` z opisem
najmniejszego testu rozstrzygającego, i udokumentowane poprawki znalezione **empirycznie**
na żywej maszynie (trzy kolejne HTTP 400) robią z tego najlepsze dostępne źródło wiedzy
o tym kontrakcie. **Nie znalazłem ani jednej rozbieżności** między Waszymi `evidence`
a stanem faktycznym w OpenNOW — wszystko się zgadza, w tym rzeczy subtelne.
Są natomiast **dwie realne luki**: `_parse_session` zwraca za mało, żeby klient C mógł
w ogóle wystartować sesję NVST (brakuje m.in. `mediaConnectionInfo`, `signalingUrl`,
`finalizedStreamingFeatures`), oraz ekran kolejki nie dostanie `seatSetupStep`.

---

## 1. Tabela weryfikacji — każde Wasze `evidence` sprawdzone

| Wasze twierdzenie | Gdzie w kodzie | Weryfikacja w OpenNOW | Wynik |
|---|---|---|---|
| `device_authorize` = `POST https://login.nvidia.com/device/authorize` | `endpoints.py:56-68` | `gfn.rs` `GfnEndpoints::default()` | ✅ |
| Flow to **device grant (RFC 8628)**, nie authorization-code+PKCE | `endpoints.py:38-53` | brak symboli `pkce`/`code_verifier`/`redirect_uri` w `native/opennow-core/src/` | ✅ |
| `scope` = `openid consent email tk_client age` | `endpoints.py:154` | `gfn.rs` | ✅ |
| **Usługi GFN używają `GFNJWT <token>`, nie `Bearer`** | `endpoints.py:330-347` | `cloudmatch.rs:2267`: `format!("GFNJWT {token}")` | ✅ |
| Token = `id_token`, a gdy brak → `access_token` | `adapter.py:541-548` (docstring `_service_headers`) | `cloudmatch.rs:2918-2923` `session_token()` | ✅ |
| Wyjątek: `login.nvidia.com/userinfo` działa z `Bearer` | `endpoints.py:347` | `gfn.rs` + Wasz pomiar żywy (HTTP 200, 2026-10-17) | ✅ |
| `DEFAULT_STREAMING_BASE` = `https://prod.cloudmatchbeta.nvidiagrid.net/` | `endpoints.py:246-256` | `cloudmatch.rs:18` | ✅ |
| `DEFAULT_STUN_SERVER` = `stun:s1.stun.gamestream.nvidia.com:19308` | `endpoints.py:303-313` | `cloudmatch.rs:19` | ✅ |
| Ścieżki CloudMatch to `v2/session` i `v2/serverInfo` | `endpoints.py:257-291` | `cloudmatch.rs:363, 962, 1003` | ✅ |
| `LCARS_CLIENT_ID` = `ec7e38d4-03af-4b58-b131-cfb0495903ab` | `endpoints.py:318` | `cloudmatch.rs:16` | ✅ |
| `GFN_CLIENT_VERSION` = `2.0.87.131` | `endpoints.py:319` | `cloudmatch.rs:17` | ✅ |
| `clientVersion:"30.0"`, `sdkVersion:"2.0"`, `streamerVersion:"14"` | `endpoints.py:320-323` | `cloudmatch.rs:1714-1716` | ✅ |
| UA Bifrost = `GFN-PC/30.0 (Windows NT 10.0) BifrostClientSDK/4.9 (38495286)` | `endpoints.py:372` | `cloudmatch.rs:3033` — **bajt w bajt** | ✅ |
| **CloudMatch: `Content-Type: text/plain`** (sic!) | `adapter.py:1040-1043` (komentarz przy `_service_headers(...)`) | `cloudmatch.rs:2268`: `HeaderValue::from_static("text/plain")` | ✅ |
| **Dwa różne profile urządzenia:** LCARS = `GENERIC`/`PC`, CloudMatch = `UNKNOWN`/`UNKNOWN` | `endpoints.py:403-410`, `adapter.py:565-567` | `gfn.rs:3661-3668` vs `cloudmatch.rs:3013-3023` | ✅ **i to jest rzecz, którą łatwo przeoczyć** |
| `x-nv-client-identity`: LCARS → `GFN-PC`, CloudMatch → **UA Bifrost** | `adapter.py:568-570` | `gfn.rs:3670` (static `"GFN-PC"`) vs `cloudmatch.rs:2284` (`&user_agent`) | ✅ kolejna subtelność odwzorowana poprawnie |
| `deviceHashId` w ciele i `x-device-id` w nagłówku muszą być **tym samym** ID | `adapter.py:1036-1040` | `cloudmatch.rs:2283` + `build_create_body:1712` | ✅ |
| `platform_name()` → `"Windows"` (nie `"WINDOWS"`) | `adapter.py:975-977` | `cloudmatch.rs:2987-2997` | ✅ |
| `app_launch_mode()` → domyślnie **1** | `adapter.py:995` | `cloudmatch.rs:2979-2985` | ✅ |
| `userAge: 25` | `adapter.py:1013` | `cloudmatch.rs:1740` | ✅ |
| `dlssOverrides`: `srMode:0 → srScale:100`, **`fgMultiplier` usunięty** gdy `fgMode != 1` | `adapter.py:1020-1027` | `dlss.rs:27-35` (`fixed_sr_scale`), `dlss.rs:72-78`, `dlss.rs:92-102` (`session_request` robi `.remove("fgMultiplier")`) | ✅ |
| `displayData: None` jest poprawne | `adapter.py:983` | `cloudmatch.rs:1602-1605`: `monitor_display_data()` zwraca `Value::Null` gdy `!hdr` | ✅ |
| **Query `keyboardLayout` + `languageCode` jest ZAWSZE wymagane** | `endpoints.py:396-401` | `language.rs:75-83` `append_session_preferences()` | ✅ znalezione po HTTP 400 |
| `vpcId` = `requestStatus.serverId` z `GET v2/serverInfo` | `endpoints.py:453-457` | `gfn.rs` `vpc_id()` / `verified_vpc()` | ✅ |
| Statusy sesji `1..7` | `endpoints.py:460-470` | `cloudmatch.rs:2698-2708` `session_phase()` | ✅ |
| `queuePosition` z 4 możliwych miejsc | `adapter.py:1443` | `cloudmatch.rs:2655-2663` | ✅ |
| `usage == 16` albo (`usage == 14` i `appLevelProtocol in (1,6)`) = sygnalizacja | `adapter.py:1466-1487` | `cloudmatch.rs:1961-1966` i `1990-1996` | ✅ |
| `usage in (2, 17)` = media | `endpoints.py:474` | `cloudmatch.rs:2027-2029` | ✅ |
| Klucze userinfo: `sub`, `email`, `preferred_username`, `picture`, `gfn_tier` | `endpoints.py:424-432` | `gfn.rs:2746-2792`, `3713-3730` | ✅ |
| `DEFAULT_MEMBERSHIP_TIER = "FREE"` | `endpoints.py:432` | `gfn.rs:2791`, `3726` (`unwrap_or("FREE")`) | ✅ |
| `GetLibraryApps`: 6 zmiennych, wszystkie `!` | `gql_queries.py:32-38` | `catalog.rs:161-167` | ✅ |
| `locale:"en_US"` (**z podkreśleniem**, nie myślnikiem) | `gql_queries.py:104`, `endpoints.py:445` | `catalog.rs:413` | ✅ |
| `sortString` = `variants.gfn.library.lastPlayedDate:DESC,computedValues.libraryAddedDate:DESC,sortName:ASC` | `endpoints.py:446-448` | `catalog.rs:413` — **bajt w bajt** | ✅ |
| `filters` = `{"variants":{"gfn":{"library":{"status":{"notEquals":"NOT_OWNED"}}}}}` | `endpoints.py:450-452` | `catalog.rs:415` | ✅ |
| Limit 512 języków w `catalog_languages` | `gql_queries.py:24-25` | `catalog.rs:191+` | ✅ |
| `validate_cloudmatch_response`: sukces to **nie tylko HTTP 200** — `requestStatus.statusCode == 1` | `adapter.py:1170-1171` (komentarz) i `1176-1194` (logika) | `cloudmatch.rs:2336-2349` | ✅ |
| Ścieżka NVST po WS = `/nvst/` | `endpoints.py:482` | `cloudmatch.rs:1890` (`unwrap_or("/nvst/")`) | ✅ |
| `NVST_SIGNALING_PATH`, `CONNECTION_USAGE_*`, `CODEC_WIRE {1:H264,2:H265,3:AV1}` | `endpoints.py:472-482` | `cloudmatch.rs:2050-2052`, `2174`, `2214-2216` | ✅ |

**Wynik: 32/32 potwierdzone, 0 rozbieżności.** Do tego trzy rzeczy, których sam bym nie
wymyślił bez przeczytania obu implementacji (profil urządzenia LCARS vs CloudMatch,
`x-nv-client-identity` o dwóch różnych znaczeniach, `Content-Type: text/plain` dla JSON-a).

---

## 2. Co jest wzorcowe i warto zachować jako standard projektu

1. **Dyscyplina dowodów.** Każdy wpis ma `evidence: {file, symbol, line_hint, confidence}`.
   Dzięki temu przegląd polega na *weryfikacji*, nie na *zgadywaniu intencji*. Proponuję
   podnieść to do rangi wymogu również w kodzie C.
2. **Jawny rejestr niewiedzy.** `UNKNOWN_ENDPOINTS` z opisem „najmniejszego testu, który by
   to rozstrzygnął" + zasada *„nic tu nie jest zmyślone"`. I co ważne: plik **wprost nazywa**
   zmyślone ścieżki z wcześniejszego briefu (`/v1/sessions/create`, `/status`, `/claim`) jako
   nieprawdziwe. To jest dokładnie ta sama zasada, którą stosuję w `HANDOFF.md` §11.
3. **Test znalazł buga, nie przegląd kodu** (`oauth.py:577-582`): wcześniejsza wersja
   parsowała JSON tylko w gałęzi `status >= 400`, więc przy poprawnym 200 `payload` zostawał
   pusty i flow kończył się `missing_access_token` **mimo otrzymania tokenów**. Wniosek
   wyciągnięty w komentarzu („testy pokrywają ścieżkę sukcesu tak samo dokładnie jak ścieżki
   błędów") jest słuszny i warto go wpisać do zasad projektu.
4. **Błąd sieci nie kończy próby logowania** (`oauth.py:566-575`) — uzasadnienie wprost
   odwołuje się do tetheringu przez USB. Poprawne i przemyślane.
5. **Higiena sekretów.** Logowany jest *fakt* i *rodzaje* tokenów (`token_kinds`), nigdy
   wartości; `connectionInfo` z poświadczeniami ICE jest jawnie **nie** kopiowane do
   `SessionInfo` ani do logów (`adapter.py:1163-1167`); `redaction.scrub()` na opisach błędów.
6. **`fetchCount` ograniczone do 1..500** z komentarzem o 2 GB RAM (`gql_queries.py:112-114`).
7. **Trzy metody otwarcia przeglądarki** (`webbrowser` → `os.startfile` → `rundll32`) i
   zasada, że brak przeglądarki **nie przerywa** logowania.

---

## 3. LUKA 1 (krytyczna): `_parse_session` nie daje klientowi C tego, czego ten potrzebuje

### 3.1 Diagnoza

`adapter.py:1163-1167` mówi: *„Rdzeń natywny dostanie tylko to, co potrzebne do łączenia,
przez osobny kanał."* — ale **ten kanał nie istnieje w przejrzanych plikach**, a
`models.SessionInfo` (sądząc po konstruktorze w `adapter.py:1218-1233`) niesie wyłącznie:

```
session_id, transport_mode, status, phase, rtsps_endpoint,
queue{position}, negotiated_codec, negotiated_width/height/fps
```

Tymczasem `session_info()` w OpenNOW (`cloudmatch.rs:1944-2105`) buduje obiekt o **31 polach** (`json!` block `cloudmatch.rs:2070-2102`)
i to jest dokładnie kontrakt, którym Qt zasila natywny streamer. Bez większości z nich
klient C **nie połączy się z sesją**.

### 3.2 Czego brakuje, z podziałem na „blokujące" i „potrzebne później"

**BLOKUJĄCE start sesji NVST** (bez tego etap 2/3 nie ruszy):

| Pole | Skąd w OpenNOW | Do czego służy w C |
|---|---|---|
| **`mediaConnectionInfo`** `{ip, port, usage}` | `cloudmatch.rs:2027-2029` + `media_connection_info()` `1866-1881` (usage ∈ {2,17}) | **peer bundle**: tam idą ICE/DTLS/SCTP → audio Opus, wejście, RTCP. Bez tego nie ma ani dźwięku, ani sterowania |
| **`signalingUrl`** | `cloudmatch.rs:1988-1990`: `rtsps_endpoints.first()` z zamianą `rtsps://` → **`wss://`** | klient WSS. Wasz `rtsps_endpoint` trzeba jeszcze przetransformować; RTSP w NVST **jedzie po WebSocket** |
| **`iceServers`** | `normalize_ice_servers()` `cloudmatch.rs:2630-2654` | lista STUN/TURN z `session.iceServerConfiguration.iceServers`, znormalizowana do `{urls[], username, credential}`. **Fallback gdy pusta**: `DEFAULT_STUN_SERVER`, `stun:stun.l.google.com:19302`, `stun:stun1.l.google.com:19302` |
| **`connectionInfo`** (cała tablica) | `cloudmatch.rs:2069` | wybór peera wideo (Mjolnir) + `bundle_endpoint()` po stronie streamera (`nvst_rtsp.rs:124`). Wymaga **osobnego, nie-logowanego kanału** |
| `rtspsEndpoints` (**liczba mnoga**) | `cloudmatch.rs:1983-1990` | `try_signaling_endpoints()` robi fallback po kolejnych endpointach przy błędach sieci/timeoutach (`nvst_rtsp.rs:975`). Jeden endpoint = brak odporności |
| `serverIp` / `signalingServer` | `cloudmatch.rs:1966-1979`, `2064-2065` | `signalingServer` = `serverIp` + `:443` gdy bez portu. `serverIp` ma łańcuch fallbacków: `ip` → `resourcePath` (host) → `sessionControlInfo.ip` → host bazy |
| **`streamingBaseUrl`** / `control_base` | `cloudmatch.rs:1979-1983`, `2063` | baza do keepalive/TEARDOWN/GET statusu. Pochodzi z `sessionControlInfo.ip` **tylko gdy port nieobecny lub == 443**, i musi przejść `trusted_learned_server_base()`; inaczej origin bazy zapasowej |
| **`finalizedStreamingFeatures`** | `cloudmatch.rs:2030-2033` | `features.extend(finalized)` — **serwer może nadpisać** to, co zażądaliśmy. Do tego `bitDepthSource`/`chromaFormatSource` = `"finalized"`. Bez tego klient C ogłosi w `ANNOUNCE` wartości, których serwer nie przyznał |

**POTRZEBNE, ale nieblokujące pierwszego połączenia:**

| Pole | Skąd | Po co |
|---|---|---|
| **`seatSetupStep`** | `cloudmatch.rs:1987`: `session.seatSetupInfo.seatSetupStep` | **ekran kolejki w TUI** — bez tego pokażesz tylko numer, bez „configuring rig / waiting for storage". Mapa znaczeń: `docs/session-start-progress.md` (0 łączenie, 1 oczekiwanie na rig, 2-4 konfiguracja, 5 sprzątanie, 6 storage; **kroki nie są liniowe i nie da się z nich liczyć procentów**) |
| `negotiatedStreamProfile` pełny + źródła | `cloudmatch.rs:2034-2061` | normalizacja kodeka `H264|AVC→H264`, `H265|HEVC→H265`, `AV1→AV1`; `codecSource` ∈ `server|unreported`; `enableHdr` + `enableHdrSource` |
| `reportedNativeTransport` | `cloudmatch.rs:105-107`, `2058` | `secureRTSPSupported == true` **lub** `streamerVersion == 14`. Rozróżnia sesję natywną od WebRTC |
| `subSessionId` | `cloudmatch.rs:2059` | potrzebny do `RESUME` |
| `termination` | `cloudmatch.rs:2061` | przy `status == 7`: `{source, status, sessionId, resumable:false}` — TUI musi wiedzieć, czy sesję da się wznowić |
| `adState` | `normalize_ad_state()` `cloudmatch.rs:2107+` | sesja może wymagać obejścia reklamy przed startem strumienia |
| `zone`, `serverLocation`, `gpuType` | `cloudmatch.rs:2062`, `2066-2067` | diagnostyka i informacje dla użytkownika w TUI |
| `appLaunchMode`, `enablePersistingInGameSettings` | `cloudmatch.rs:2068-2069` | echo z `sessionRequestData` |
| `clientId`, `deviceId` | `cloudmatch.rs:2073-2074` | streamer ich potrzebuje w nagłówkach LCARS |

### 3.3 Rekomendacja

**Nie projektujcie własnego kształtu `SessionInfo`.** Skopiujcie kontrakt `session_info()`
1:1 jako JSON i przekażcie go klientowi C przez stdin (wzorzec `docs/core-protocol.md`,
limit linii 1 MiB). Powody:

- to jest **sprawdzony w produkcji** kontrakt między warstwą kont a streamerem,
- klient C dostaje wszystko, czego potrzebuje, bez drugiej rundy „a gdzie jest X",
- `connectionInfo` i tak trzeba przekazać osobnym kanałem — więc niech to będzie ten sam
  kanał JSON-lines, z **redakcją w logach** (Wasza zasada z `adapter.py:1163` pozostaje
  słuszna: do loga idzie tylko `has_signaling_endpoint`, liczby i długości).

Konkretnie: dodać do `models.SessionInfo` (albo obok) pole `native_context: dict` będące
dokładnym odwzorowaniem JSON-a z `cloudmatch.rs:2070-2102`, i wystawić je w `diagnostics()`
**z redakcją** (`connectionInfo` → tylko `usage`/`port`, bez `ip`).

---

## 4. LUKA 2: nie ma wznowienia sesji ani sprzątania zawieszonych

OpenNOW ma oba mechanizmy, u Was ich nie widzę:

**Resume** (`cloudmatch.rs:1506`, `1540-1598`):
- `session_requires_resume(status)` — `cloudmatch.rs:1506`
- `build_resume_body(app_id, session, settings, device_id)` — ciało:
  ```json
  {"action":2, "data":"RESUME", "sessionRequestData":{...}}
  ```
  gdzie `sessionRequestData` jest **kopiowane z poprzedniej sesji** po kluczach z listy
  (`cloudmatch.rs:1564-1565` zawiera m.in. `streamerVersion`, `secureRTSPSupported`),
  a pomijane, gdy wartość jest `null` (`cloudmatch.rs:1594-1595`).

**Pending cleanup** (`cloudmatch.rs:3527`, `580-611`, `1014-1021`):
- plik `pending-session-cleanup.json` (limit `MAX_CLEANUP_RECORD_BYTES = 16 KiB`),
- `pending_cleanup(auth)` / `clear_cleanup(session_id)`,
- w `remote_sessions()`: jeśli jest pending cleanup i **nie ma go** na liście sesji
  zdalnych → dopisujemy go do wyniku, żeby UI mógł dokończyć sprzątanie.

**Dlaczego to ważne na tej maszynie:** przy tetheringu przez USB 2.0 rozłączenia będą
częste. Bez `RESUME` każde zerwanie łącza = nowa kolejka. Bez pending cleanup =
**osierocone sesje zajmujące miejsce** (i potencjalnie limit równoległych sesji konta).

Priorytet: to nie blokuje etapu 1-2, ale powinno być w etapie 5 („utwardzanie") jako
pozycja obowiązkowa, nie opcjonalna.

---

## 5. Nowe fakty z OpenNOW, których u Was jeszcze nie ma

Znalazłem je przy weryfikacji; wszystkie są w `catalog.rs` i dotyczą **paginacji katalogu**,
czyli miejsca, gdzie najłatwiej o trudno wykrywalny błąd.

### 5.1 Wiązanie kursora z rewizją katalogu — `catalog_changed`

`catalog.rs:403-408` (hash kontekstu: `404-405`):

```rust
self.endpoints.graphql, vpc, config_from_settings(settings)?...cache_scope, revision, "en_US"
/* to jest składowa hashu `context` */
if !page.cursor.is_empty() && params["catalogContext"] != context {
    return Err(ServiceError { code: "catalog_changed",
        message: "The catalog route changed. Restart the library refresh." });
}
```

Czyli: **kontekst trasy katalogu** to `SHA-256` z JSON-a
`[user_id, provider, generation, graphql_endpoint, vpcId, cache_scope, catalogRevision, "en_US"]`
(`catalog.rs:404-405`) — czyli zmienia się **również** przy przełączeniu konta albo nowej
`generation` sesji, nie tylko przy zmianie regionu. Jeśli podczas paginacji którykolwiek się zmieni (np. odświeżono
`vpcId`, albo serwer podniósł `catalogRevision`), dalsze używanie starego kursora jest
**błędem**, a nie pustą stroną. Komunikat: *„Restart the library refresh."*

**Konsekwencja dla Was:** `catalog_page()` powinien przyjmować i zwracać `catalog_context`,
a przy niezgodności rzucić `ApiError(code="catalog_changed")` z jasnym komunikatem.
Bez tego dostaniecie cichą duplikację albo dziury w liście gier po zmianie regionu/rewizji.

### 5.2 Sprawdzenie rewizji przed **i po** każdym pobraniu

`catalog.rs:411` i `417`:

```rust
self.check_scope(session, generation)?;
self.check_catalog_revision(revision)?;
let payload = self.catalog_document(...);          // <-- właściwe pobranie
self.check_scope(session, generation)?;
self.check_catalog_revision(revision)?;
```

Podwójne sprawdzenie (przed i po) łapie zmianę rewizji **w trakcie** pobierania strony.
Do tego `check_scope(session, generation)` pilnuje, czy konto/generacja się nie zmieniły
(przełączenie konta w środku paginacji).

### 5.3 Pamięć podręczna języków: TTL 14 dni + `allow_stale`

`catalog.rs:214`:

```rust
CachePolicy { ttl_ms: 14 * 24 * 60 * 60 * 1000, allow_stale: true }
```

Lista języków jest cache'owana na **14 dni** i może być podana *stale* (z odświeżeniem w tle).
Praktyczny skutek: **nie pobierajcie języków przy każdym starcie** — na tetheringu to
sekundy i transfer. Klucz cache: `["languages-v1", public_graphql, provider, cache_scope]`.

### 5.4 `fetch_bounded_page`

`catalog.rs:409`: strona jest pobierana przez `store_catalog_page::fetch_bounded_page(page.limit, ...)`.
Wasz limit `fetchCount ∈ 1..500` (`gql_queries.py:112-114`) jest spójny z tą ideą i
wystarczający — tylko warto wiedzieć, że klient referencyjny też ogranicza stronę,
a nie polega na serwerze.

---

## 6. Rekomendacje — uporządkowane priorytetem

| # | Co | Priorytet | Dlaczego teraz |
|---|---|---|---|
| **1** | Rozszerzyć `SessionInfo` o **`native_context`** = dokładny JSON z `session_info()` (`cloudmatch.rs:2070-2102`), w szczególności: `mediaConnectionInfo`, `signalingUrl` (wss://), `iceServers`, `rtspsEndpoints[]`, `connectionInfo` (kanał nie-logowany), `streamingBaseUrl`, `serverIp`, `finalizedStreamingFeatures`, `seatSetupStep`, `subSessionId`, `termination`, `reportedNativeTransport` | **krytyczny** | bez tego klient C nie wystartuje sesji; to jest *jedyna* rzecz, która blokuje etap 2 |
| **2** | Dodać `seatSetupStep` do `models.QueueInfo` | wysoki | ekran kolejki w TUI: sama pozycja to za mało (`docs/session-start-progress.md`) |
| **3** | `finalizedStreamingFeatures` nadpisuje `requestedStreamingFeatures` przed przekazaniem do C | wysoki | inaczej `ANNOUNCE` ogłosi parametry, których serwer nie przyznał → rozjazd obrazu |
| **4** | Wiązanie kursora z kontekstem + błąd `catalog_changed` (§5.1) | średni | cicha duplikacja/dziury w liście gier po zmianie regionu lub rewizji |
| **5** | Cache języków 14 dni + `allow_stale` (§5.3) | średni | oszczędność łącza i czasu startu na tetheringu |
| **6** | `RESUME` (`action:2, data:"RESUME"`) + `session_requires_resume` | średni (etap 5) | tethering będzie zrywał sesje; bez resume każde zerwanie = nowa kolejka |
| **7** | `pending-session-cleanup.json` (16 KiB) + dopisywanie do `remote_sessions` | średni (etap 5) | osierocone sesje zajmują miejsce na koncie |
| **8** | `check_catalog_revision` przed **i po** pobraniu strony (§5.2) | niski | odporność na zmianę w trakcie paginacji |
| **9** | `normalize_ice_servers` z trzema fallbackami STUN (§3.2) | niski | gdy serwer nie zwróci `iceServerConfiguration` |
| **10** | `adState` (reklamy przed startem strumienia) | niski | zależne od tieru konta |

**Czego NIE zmieniać:** dyscypliny `evidence`, rejestru `UNKNOWN`, zasady
`NOT_IMPLEMENTED` zamiast zgadywania, higieny sekretów, „błąd sieci nie kończy próby",
limitu `fetchCount`. To wszystko jest dobre i powinno zostać podniesione do standardu
również w kodzie C.

---

## 7. Jedna rozbieżność *pozorna* — żeby nie robić z niej buga

`adapter.py:1176-1194` traktuje **całkowity brak** `requestStatus` jako *ostrzeżenie*,
nie błąd, i parsuje dalej. OpenNOW (`cloudmatch.rs:963-968`) dla `v2/serverInfo` wymaga
`metaData` jako tablicy **oraz** `statusCode == 1` jeśli `requestStatus` istnieje.

To nie jest rozbieżność: Wasz komentarz uczciwie mówi, że kształt odpowiedzi 200 nie był
jeszcze zaobserwowany na żywej maszynie, więc wolicie ostrzec niż zablokować pierwsze
uruchomienie. **Zgadzam się z tym wyborem** i proponuję tylko, żeby po pierwszym udanym
`POST v2/session` na żywo dopisać w `evidence` adnotację `OBSERVED_LOG` z datą — tak, jak
zrobiliście to dla `userinfo` (HTTP 200, 2026-10-17). Wtedy ten fragment przestanie być
„tymczasowy".

---

## 8. Powiązanie z resztą projektu

- Kontrakt IPC Python ↔ C: `docs/core-protocol.md` (JSON-lines po stdin/stdout, limit linii
  **1 MiB**, `protocolVersion` + `capabilities`, 5 s na handshake, `unknown/malformed/oversized`
  → rozłączenie). Wasz `native_context` (§6.1) jest payloadem jednej z tych linii.
- Co klient C robi z tym kontekstem: `analiza/04-NVST-SPEC-BINARIA.md` (RTSP-over-WSS,
  Mjolnir, SRTP, FEC, NACK-v2, ramki NVB).
- Pełna lista pól `session_info()` z numerami linii: `cloudmatch.rs:2070-2102`.
- Stan bramek i następny krok: `HANDOFF.md` §9-§10.
