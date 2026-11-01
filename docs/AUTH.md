# AUTH.md — GFN login/token research record (round XXI)

Read from OpenNOW @ `a49b124` THIS round (`native/opennow-core/src/gfn.rs`,
5252 lines; `account_connections.rs`; `cloudmatch.rs`). Implementation of
the login UI/flow is **stage-4 (TUI)** work per the BOOTSTRAP stage plan —
stage-2 bring-up uses a pasted `--session-token` (the GFNJWT), logged as
length+presence only (§7 redaction). This document is the provenance so
stage 4 does not re-research it.

## 1. The flow is the OAuth 2.0 DEVICE grant (RFC 8628)

Endpoints (gfn.rs:96-107, overridable via `pcs.geforcenow.com/v1/serviceUrls`):

| Endpoint | URL |
|---|---|
| device authorize | `https://login.nvidia.com/device/authorize` |
| token | `https://login.nvidia.com/token` |
| client token | `https://login.nvidia.com/client_token` |
| userinfo | `https://login.nvidia.com/userinfo` |
| revoke | `https://login.nvidia.com/assets/v2/Tokens?level=client` |
| service urls | `https://pcs.geforcenow.com/v1/serviceUrls` |
| CloudMatch base | `https://prod.cloudmatchbeta.nvidiagrid.net/` (:38) |
| graphql | `https://games.geforce.com/graphql` (:72), public: `https://public.games.geforce.com/graphql` (:106) |
| MES (subscription) | `https://mes.geforcenow.com/v4/subscriptions` (:73) |

Constants (:37, :63-71): `DEFAULT_IDP_ID = "PDiAhv2kJTFeQ7WOPqiQ2tRZ7lGhR2X11dXvM4TZSxg"`,
`STEAM_DECK_CLIENT_ID = "q61ddeJrVt7O90Nl-P-N7I36yctih4Ml6FyXLrb6j-U"`,
`SCOPES = "openid consent email tk_client age"`,
`LCARS_CLIENT_ID = "ec7e38d4-03af-4b58-b131-cfb0495903ab"`,
`GFN_CLIENT_VERSION = "2.0.87.131"`, token refresh window 10 min (client
token 5 min) (:68-69).

## 2. Device authorize (start_device_login, :658-730)

`POST {device_authorize}` as `application/x-www-form-urlencoded; charset=UTF-8`
with form fields: `client_id={STEAM_DECK_CLIENT_ID}`, `scope={SCOPES}`,
`device_id={device_id}`, `display_name=OpenNOW`, `idp_id={provider.idp_id}`.
Headers: `Accept: application/json, text/plain, */*`,
`Origin: https://play.geforcenow.com`, `Referer: https://play.geforcenow.com/`,
`User-Agent: {STEAM_DECK_USER_AGENT}` (:65 — Chrome/128 Steam Deck string),
`x-device-id`, `nv-client-id: {STEAM_DECK_CLIENT_ID}`,
`nv-client-streamer: WEBRTC`, `nv-client-type: BROWSER`,
`nv-client-platform-name: browser`, `nv-browser-type: CHROME`,
`nv-device-os: STEAMOS`, `nv-device-type: CONSOLE`,
`nv-device-model: STEAMDECK`, `nv-device-make: VALVE`.
Response: `device_code` (:732) + the user-facing code/URI + `interval`.

NOTE the identity choice: the reference presents itself as a Steam-Deck
browser client for the DEVICE flow (no embedded-browser/CEF path needed),
while CloudMatch traffic uses the GFN-PC/NATIVE identity (§4). Both
identities are constants above — no secrets.

## 3. Token poll + session tokens (:795-880)

`POST {token}`, same content type/Origin/Referer/UA (Steam Deck), form:
`grant_type=urn:ietf:params:oauth:grant-type:device_code`,
`device_code={...}`, `client_id={STEAM_DECK_CLIENT_ID}`.
Pending states (:826-851): `authorization_pending` → keep polling at
`interval`; `slow_down` → interval += 5 (cap 3600); `expired_token` /
`access_denied` → cancel. Success payload: `access_token` (required),
`refresh_token`, `id_token` (JWT), `client_token` (+ separate
client_token bootstrap, deferrable :869-873).
* **The CloudMatch/RTSPS credential is `id_token` when present, else
  `access_token`** — `service_token()` (:254-256); expiry tracked per
  purpose with `jwt_expiry` (:258-263).
* Refresh: `grant_type=refresh_token` (:2866), window 10 min before
  expiry; revocation errors `invalid_grant|invalid_token|token_revoked`
  force re-login (:358).
* `userinfo` fetch populates the account profile (:2748+, `user_from_jwt`
  decodes the id_token claims as a fallback).

## 4. Where the token is used (cloudmatch.rs:2036-2059)

Every CloudMatch call: `Authorization: GFNJWT {service_token}` plus the
NATIVE client identity headers — `User-Agent: {bifrost_user_agent()}`
(:3025), `Content-Type: text/plain` (quirk — body is JSON),
`nv-client-id: {LCARS_CLIENT_ID}`, `nv-client-streamer: NVIDIA-CLASSIC`,
`nv-client-type: NATIVE`, `nv-client-version: {GFN_CLIENT_VERSION}`,
`nv-device-os/type/make/model`, `x-device-id`, `x-nv-client-identity:
{user_agent}`. (Full v2/session mechanics: docs/RTSP.md §5.)

## 5. Stage-4 implementation notes (deferred, decided here)

* Device flow needs NO embedded browser: print the verification URI +
  user code in the TUI (ConEmu), poll at the interval — the whole flow is
  form-POSTs over the kn_http TLS stack built this round.
* Token storage: DPAPI (`CryptProtectData`) vault in the TUI (BOOTSTRAP
  stage-4 scope); the C side only ever sees the token in memory, logs
  length+presence (`kn_redact_key_is_secret` already treats "token"/
  "jwt"/"auth" families as secret — verify at stage 4).
* `device_id`: a generated UUID persisted per install (the reference
  derives one and reports `device_identity_unavailable` when it cannot,
  :660-663); `deviceHashId` in the v2/session body is the same value.
* NOT VERIFIED: the exact `user_code`/`verification_uri` field names in
  the device-authorize RESPONSE (the reference reads `device_code`,
  `interval` and forwards the rest of the payload to the UI layer) —
  measure at stage 4 against the live endpoint.

## 6. Stage-4 status (2026-11-01, slice 4o): the PyQt5 launcher, its two real bugs, and the Python-3.8 rule

`tools/kn_launcher/` is the stage-4 login UI — implemented NOT as the TUI
sketched in §5 but as a PyQt5 + httpx dark-mode GUI (operator decision),
covering the §1-§3 flow end to end: saved-token check + API verify ->
device grant (copyable verification URI + user code, interval poll) ->
the unlocked GFN game-list tab (GraphQL, owned games marked, smart
search) -> param box + launch of `build\knvstd.exe`. Token handling
follows §5's contract: DPAPI vault (store.py), the C side receives the
token ONLY as `--session-token=` on argv (redacted from console/log),
and an operator-typed `--session` in the param box is REJECTED with a
dialog — the launcher owns the session contract.

Two rules this slice proved the hard way:

* **The target is Python 3.8.20 (Win7) — every launcher module MUST
  start with `from __future__ import annotations`.** PEP 604/585
  annotations (`X | None`, `list[str]`) are EVALUATED at import on 3.8
  and raise TypeError before any Qt code runs — the "failed gui" the
  operator saw. A sandbox on a modern Python can NEVER reproduce it.
  The guard is permanent: `test_launcher.py` greps every module for the
  future import AND runs `ast.parse(..., feature_version=(3,8))` over
  every .py in the package. Any new module inherits both checks
  automatically.
* **The param box is split with CommandLineToArgvW rules, not shlex.**
  `shlex.split` (posix) ate Windows backslashes (`logs\wire_r1.txt` ->
  `logswire_r1.txt`); `posix=False` keeps mid-token quotes
  (`--session-title="My Game"`). `tools/kn_launcher/params.py`
  implements the MSVCRT rules (backslash literal except before a quote;
  2n backslashes + quote -> n + toggle grouping; 2n+1 -> n + literal
  quote) and single-sources `DEFAULT_PARAMS`.

Test discipline: run `test_launcher.py` AS A SCRIPT with
`QT_QPA_PLATFORM=offscreen` (never via pytest — the checks are
script-mode). Current: **71 checks, 0 failures**. Flow details live in
RTSP §59.4 and START-HERE §32.

### 6.1 Addendum (slice 4p): ONE device identity per install — login and CloudMatch finally agree
`store.device_id()` (a persisted UUID in `%APPDATA%/KarnyNow/device_id.txt`)
was already sent as the login `x-device-id` header (auth.py) — but the
launcher never passed it to knvstd, and knvstd defaults to persisting its
GUID **per log directory**: every draw of run 102 flew a fresh
`device_hash_id`, so the account looked like FIVE different devices in one
evening while the login said one. The reference keeps a single per-install
identity for both paths. Fixed: `gui.py:_launch` appends
`--session-device=<store.device_id()>` (operator override wins), and knvstd
gained `--device-id-file=PATH` for manual runs. Consequence for the resume
flow (§RTSP 60.2): a resumed session's `deviceHashId` in the resume body
now matches the identity that created it, instead of silently rotating.
`test_launcher.py`: 71/0 (the flag is injected only when the operator did
not type `--session-device` — same guard style as the existing `--has-app`
logic).
