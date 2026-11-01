# tools/wiretap — capturing the OFFICIAL client's handshake (reference side)

Companion to F34 (`--wire-dump=FILE`, the client's own TLS plaintext dump).
Goal: a byte-level diff of OUR handshake identity vs the OFFICIAL GeForce NOW
browser client's handshake, captured on the operator's Windows machine, same
account, same evening. The kill model (RTSP §53-55.3) has exactly one
surviving suspect: the identity/attestation bytes. This kit produces the
reference half of that audit.

## What you need

- Wireshark installed (includes `tshark`; default path
  `C:\Program Files\Wireshark\tshark.exe`).
- Chrome or Edge (the GFN web client at https://play.geforcenow.com).
- ~10 minutes.

## Step-by-step (one capture session)

1. **Close ALL Chrome/Edge windows first.** An already-running browser ignores
   `SSLKEYLOGFILE` — the key log only works for a browser started fresh with
   the variable set. `capture_ref.bat` starts Chrome with a throwaway profile
   precisely for this; use `capture_ref_edge.bat` for Edge.
2. **Start Wireshark** on your main network interface, capture filter:
   `tcp port 443 or udp port 443 or udp portrange 49152-65535`
   (keeps the file small; the handshake is all TCP/443).
3. **Run `capture_ref.bat`** (or the Edge variant). It sets
   `SSLKEYLOGFILE=%USERPROFILE%\gfn_keys.log` and opens the GFN web client.
4. Log in, **start any game**, let it stream ~60 s (long enough for the full
   handshake + first minute), then **stop the Wireshark capture** and save as
   `%USERPROFILE%\gfn_ref.pcapng`.
5. **Extract the readable layers** — run `extract_ref.bat` (edit the two paths
   at the top if you saved elsewhere). It produces three TEXT files:
   - `gfn_ref_h2.json`   — all HTTP/2 API traffic, decrypted (the
     `v2/session` POST body with the identity JSON is in here),
   - `gfn_ref_ws.txt`    — every WebSocket frame payload (hex),
   - `gfn_ref_urls.txt`  — every SNI + URL seen (which servers, which
     client-version query params).
6. **Sanity-check decryption worked**: open `gfn_ref_h2.json` — if it contains
   readable JSON (`sessionRequestData` etc.), the key log worked. If it is
   empty, the browser was already running in step 1 — redo from a full close.

## What gets pushed / sent (SAFETY — read this)

- PUSH ONLY the three extracted TEXT files, and run them through
  `decode_jwt_claims.py`-style redaction FIRST if they contain an
  `Authorization: GFNJWT ...` value: `extract_ref.bat` already pipes the h2/ws
  output through `findstr /v` for the common secret headers, but eyeball the
  files before pushing — **never push `gfn_keys.log` (TLS master secrets) and
  never push the raw `.pcapng`** (contains your decrypted session + account
  data). Delete both after extraction.
- The F34 `--wire-dump` file from our client is redacted at the source
  (JWT/ice-pwd/Authorization/long-base64 → 'X'), but still treat it as
  operator-sensitive: push it to the logs repo only for a diagnostic flight,
  and prefer zipping it into the diag bundle path.

## Token claims comparison (no secrets in chat)

`decode_jwt_claims.py` decodes a GFNJWT's claim structure LOCALLY and prints
it with account-identifying values masked:

    python tools\wiretap\decode_jwt_claims.py < token.txt

Paste its output (it is already masked) — it lets the agent compare claim
KEYS and non-sensitive values (token type, client ids, versions) between the
token our client uses and the token the browser gets, without anyone's real
credentials ever leaving the machine.

## Also worth grabbing in the same session (30 s each)

- `chrome://netlog` → Start Logging → reproduce → Stop → save
  `gfn_netlog.json` (backup view of every request/socket event).
- `chrome://webrtc-internals` → "Create Dump" during the stream →
  `gfn_webrtc_dump.txt` + the data-channel log (same recipe as the
  logs-archive/webrtc round-2 dumps — but FRESH, for era comparison).
- The web client's version string: in `gfn_ref_urls.txt` look for
  `version=` / app-bundle paths, or check the page source of
  play.geforcenow.com for the bundle version.
