# DTLS.md — the R3 spike: real bundle identity, DTLS + SCTP (promoted to the stage-2 PLAY blocker by R7 run 10)

> Status: **R3-A SHIPPED (round XXX, 2026-10-26** — kn_cert live: real
> ECDSA P-256 self-signed identity, sandbox-proven 10/10 under
> ASan+UBSan via the CMake mbedtls backend, cross-compiled for Win32,
> wired into the knvstd ANNOUNCE with the `identity` evidence line;
> run 12 live-proved it: `"cert":"real"`, fp_len 95, ANNOUNCE 200).
> **R3-B SHIPPED (round XXXI, 2026-10-26/27** — run 12's verdict: the
> real fingerprint ALONE changed nothing (PLAY timeout in total WS
> silence on a 4th rig) → kn_dtls, the DTLS 1.2 ACTIVE client on the
> bundle socket inside the pump thread; the reference's N/N+1 socket
> pair (preferred 49005/49006); the full design record + the three
> mbedTLS landmines are in docs/RTSP.md §18; sandbox-gated: test_kndtls
> 31/31 real battery (mutual pinning, HVR cookies, loss legs) under
> ASan+UBSan, the OpenSSL-3 interop leg PASS, ctest 17/17 ×2; run 13 is
> its first live measurement). R3-C/D: PLAN as below.
> Original plan header (round XXIX, 2026-10-26): Provenance: nvst.rs @ a49b124
> (line refs below), str0m vendored in the reference tree
> (`native/opennow-streamer/vendor/str0m`), runs 6/9/10 evidence in
> docs/RTSP.md §12/§15/§16. Language rule D12: English.

## 0. Why this is now the blocker

The elimination table (RTSP.md §16): every wire-shape and transport
difference between knvstd and the reference is CLOSED and live-proven
(ANNOUNCE 200 ×3 rigs; bidirectional integrity-validated ICE with
3671 rig answers; the pump thread spanning the whole PLAY exchange).
Two different rigs refused PLAY in two different modes (instant 455;
hold-until-timeout). The one major remaining difference: the reference
announces the fingerprint of a REAL self-signed certificate and runs a
live ICE+DTLS stack on the bundle socket from receiver-spawn time — by
PLAY the rig has typically seen a DTLS ClientHello from a certificate
matching the ANNOUNCE. We announce a random placeholder (D-s1) and
never speak DTLS. R3 is therefore promoted from "stage 3" to the
prerequisite for finishing stage 2.

## 1. What the reference actually does (provenance)

* **Sockets** (`ReservedNvstBundle::reserve`, nvst.rs:5472-5541): a
  bundle UDP socket on port N and a dedicated Mjolnir video socket on
  N+1 (comment :5484: the server relies on this pairing; some seats
  keep video off otherwise). Unified-socket mode (SDP
  `general.enableUnifiedSocket`) collapses video onto the bundle.
  knvstd today: ONE socket (SETUP client port range N..N+1 announced;
  kernel-connected to the video peer). R3-B must decide: keep one
  socket while `enableUnifiedSocket=1` is described, add the N+1
  sibling when it is not (measure which mode the real seats describe —
  the handoff line already prints `unified_socket`).
* **Identity** (`generate_gfn_local_ice_credentials` :5545-5559 +
  `create_nvst_bundle_rtc` :5562+): 4-char ufrag / 22-char password
  over the base64-ish alphabet (ALREADY mirrored — round XXIX), and a
  str0m-generated self-signed certificate whose SHA-256 fingerprint is
  announced as colon-uppercase-hex (`nvst_fingerprint_hex`
  :5596-5603). str0m's cert: generated per-session (ECDSA P-256 by
  str0m default — VERIFY in vendor/str0m during R3-A).
* **Bundle setup** (`prepare_nvst_webrtc_bundle` :6106-6160): client
  is **ICE controlling** (`set_ice_controlling(true)`); host candidates
  only (local = routed source address, remote = `bundle_peer()`);
  local/remote ICE creds = the same ladder knvstd already implements;
  **remote fingerprint** parsed from DESCRIBE
  (`parse_nvst_fingerprint` :5640) and PINNED for the handshake.
* **DTLS/SCTP**: str0m performs the DTLS handshake over the bundle
  socket once ICE pairs, then SCTP data channels — `rtcp1` (RTCP
  feedback: "RTCP over SCTP is a must for One SDK video", :411) and
  the input channels (text/HID). Video SRTP keys do NOT come from
  DTLS-SRTP: `NvstSrtpMaterial` is built from the ANNOUNCE runtime key
  (:1820-1840) — DTLS here is purely the control/SCTP transport.
* **After PLAY** (`ActiveNvstRtspSession::spawn`, nvst_rtsp.rs:844-930
  + `NvstControlPing` :260-290): the WS control channel is kept alive
  with WebSocket PING frames (sequence-numbered, RTT-tracked,
  CONTROL_PING_EXPIRY); kn_ws already auto-pongs inbound pings but
  never sends them.

## 2. Phased plan (each phase gated before the next)

### R3-A — the real certificate identity (replaces D-s1's fingerprint)
* NEW `streamer/{include/kn,src}/kn_cert.{h,c}` — **portable mbedTLS**
  (third_party/mbedtls 3.6.0 is already vendored and pinned): generate
  a self-signed cert at session start (ECDSA P-256 if str0m's default
  confirms, else RSA-2048; CN fixed, validity ~1 day), expose:
  the DER blob + SHA-256 fingerprint in the announced colon-hex form +
  an opaque keypair handle for R3-B. NO sockets, NO Win32 — pure
  crypto, so it KATs in the sandbox on BOTH platforms (unlike
  kn_net_mbedtls's Win32-only body; the round-XXII lesson applied from
  day one: the cross_check leg compiles it for Win32 too).
* knvstd: `sess_local_identity` fingerprint ← kn_cert (placeholder
  kept as the honest fallback when kn_cert is unavailable — schannel
  builds refuse loudly like the TLS stream does).
* Build wiring: kn_cert.c compiles ONLY when third_party/mbedtls
  exists (CMake `KN_HAVE_MBEDTLS` + both shell gates + T10 expectation
  bump for the new suite `test_kncert`).
* Gates: KATs (cert re-parses via mbedTLS x509, fingerprint matches an
  independent recomputation, key/cert pair verifies a test signature,
  deterministic-shape checks), cross-compile legs, T10.
* Live value on its own: the ANNOUNCE now carries a fingerprint whose
  cert EXISTS — if the rig validates fingerprint presence/shape before
  DTLS, run 12 may already behave differently (measurement).

### R3-B — DTLS 1.2 client on the bundle socket — **SHIPPED (round XXXI)**
* DELIVERED as planned, with the design record + provenance + the three
  mbedTLS landmines in **docs/RTSP.md §18** (kn_dtls.{h,c} — DTLS 1.2
  ACTIVE client, fingerprint pinning as a DOUBLE GATE (verify callback
  + the poll-side ready refusal; VERIFY_OPTIONAL because mbedTLS fails
  REQUIRED without a CA chain before any callback — ssl_tls.c:8071),
  inject-queue BIO with WANT_READ (never TIMEOUT — the resend-NULL-
  flight segfault class), ms-clock retransmission timers, MTU 1200,
  post-ready app-record drain counted for R3-C).
* Integration: the pump thread's state machine — the ClientHello arms
  at the first REAL inbound STUN evidence (req_in or ok_in; the :8196
  "never synthesize" rule), handshake steps interleave with the 20 ms
  bursts, and on `dtls_ready` the cadence switches to the reference's
  post-connection shape (NATT-only, 100 ms, :7837-7846) with the thread
  PERSISTING through the media loop in non-unified mode (the bundle
  worker lifetime; D21 untouched). Socket pairing: the video-N +
  bundle-N+1 pair (preferred 49005/49006, then <= 32 dynamic adjacent
  attempts, :5933-6000) reserved BEFORE SETUP; SETUP's X-GS-ClientPort
  = unified ? bundle : video (:1176-1183); clientBundlePort +
  a=candidate + localAddress from the bundle socket (:1626/:1648, the
  test :2955 pins 49006). The old single-socket save/restore reconnect
  dance is gone — one peer per socket for life. Literal `PING` gets a
  `PONG` (:8605-8614).
* ICE role: unchanged (controlling-consistent bursts; no USE-CANDIDATE
  — the reference's OWN hand-built bursts are USERNAME-only,
  :5013-5028; str0m's parallel agent nomination stays deviation D-t1,
  measured per run).
* Gates (all green in the sandbox): test_kndtls 31/31 (in-test mbedTLS
  server shaped like the rig: HVR cookies + client-cert pinned
  server-side = pinning BOTH directions; loss/retransmit legs both
  directions; wrong-pin refusal at the ready gate; mid-handshake free;
  app-data drain) under ASan+UBSan+LSan; `tools/dtls_openssl_check.py`
  — the independent-stack interop leg (OpenSSL 3 s_server -dtls1_2
  accepts our ClientHello/cookie/cert/Finished: PASS, ~24 ms); ctest
  17/17 ×2 backends; cross x64+i686 -Werror incl. the real-body legs;
  E2E 43/43; T10 dry 64; T13 63.
* Live evidence lines (run 13): `sock_pair`, `dtls_arm`,
  `dtls_ready`/`dtls_fail` (emitted from the thread AT the transition),
  the grown `ice_preplay` (persist + the dtls counters), and
  `ice_creds` now carries `unified` — answering open question 4 (which
  mode real seats describe) even when PLAY fails.

### R3-C — SCTP over DTLS (usrsctp) + the rtcp1 channel
* Vendor usrsctp (BOOTSTRAP's R3 choice) behind the third_party model;
  open the `rtcp1` data channel (RTCP feedback for the video stream —
  without it the rig may degrade/stop the stream; :411) and the input
  channels (text/HID — feeds the stage-4 TUI).
* This phase also unlocks sending the NACK-v2/IDR/RFI encoders that
  kn_rx already produces (Next steps 6).

### R3-D — WS control pings (small; ships with the media phase)
* kn_ws gains an outbound PING (sequence-numbered) at the reference's
  cadence + RTT tracking (`NvstControlPing` semantics), emitted in the
  periodic session_stats log lines.

## 3. Open questions (answered BY the spike, not before)

1. str0m's exact cert algorithm/key size — **ANSWERED (R3-A):** ECDSA
   P-256 (the str0m provider default), fingerprint = SHA-256(DER)
   (vendor str0m src/dtls.rs:69-71); mirrored by kn_cert.
2. Does the rig initiate DTLS toward the client if the client is
   silent — **ANSWERED (runs 9-12):** NO within ~20 s (invalid_in 0 —
   the client is the DTLS-ACTIVE side, `start_dtls(true)`, nvst.rs:6300).
3. Is PLAY gated on DTLS *completion* or merely *started*? (Run 13 is
   the measurement: `dtls_ready` timing vs the PLAY verdict. Run 12
   proved the announced fingerprint ALONE is not the gate.)
4. unified vs paired sockets on real PL-region seats — run 13 answers
   it even on failure (the `ice_creds` + `sock_pair` lines carry
   `unified`; the handoff line does too).
5. Whether the rig's 455-vs-timeout refusal modes correlate with rig
   software versions (collect `serverId`/peer evidence per run; so far
   four rigs: 455, timeout ×3).
6. NEW (run 13+): does the rig's DTLS server REQUEST a client
   certificate (WebRTC stacks pin the announced fingerprint, so almost
   certainly yes — kn_dtls sends it whenever asked), and does the rig
   tolerate a client that completes DTLS but never opens SCTP channels
   (D-t4 — R3-C's urgency depends on this answer)?

## 4. Rule bindings

* R32 (no trust without measurement): every phase ships with sandbox
  KATs FIRST, then one live run as the verdict; every deviation
  documented (D-s1 dies at R3-A; new D-t1.. deviations get numbers as
  they appear).
* Round-XXII gate rule: any Win32-only code path gets a cross_check
  compile leg the same round it is written; kn_cert/kn_dtls are
  deliberately PORTABLE so the sandbox can run them for real.
* §7 logging: key material and fingerprints — shapes/lengths only in
  stdout JSONL (the fingerprint is public by design — it travels in
  the ANNOUNCE — but the private key never touches any log).
