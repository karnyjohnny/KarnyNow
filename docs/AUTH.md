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
