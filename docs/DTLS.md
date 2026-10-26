# DTLS.md — the R3 spike: real bundle identity, DTLS + SCTP (promoted to the stage-2 PLAY blocker by R7 run 10)

> Status: PLAN (round XXIX, 2026-10-26). Provenance: nvst.rs @ a49b124
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

### R3-B — DTLS 1.2 client on the bundle socket
* NEW `kn_dtls.{h,c}` — mbedTLS DTLS 1.2 (min=max 1.2 like the TLS
  stream policy; VERIFY_REQUIRED against the PINNED remote
  fingerprint — a self-signed peer cert is matched by fingerprint, not
  chain; no-CA mode with an explicit verify callback), custom BIO over
  `kn_sock_udp` (non-blocking + our select tick), handshake
  retransmission timers driven by the same loop.
* Integration: the pump thread grows a state machine — STUN bursts
  until the first validated success pair, then DTLS handshake steps
  interleaved with continued 20 ms keepalives (the reference's str0m
  poll does exactly this on one socket). Single-owner discipline kept
  (thread owns the socket until joined).
* ICE role: knvstd becomes controlling-consistent (it already sends
  the USERNAME pair the controlling side sends; formal nomination
  (USE-CANDIDATE) only if the rig demands it — MEASURE first).
* Socket pairing: implement the N+1 Mjolnir sibling socket when
  `enableUnifiedSocket != 1` (reserve both BEFORE SETUP so the
  announced client-port range stays truthful).
* Gates: sandbox loopback KAT (kn_dtls vs a Python/mbedTLS mini-server
  with fingerprint pinning both ways — the two-implementations rule),
  then the live run: `dtls_ready:1` evidence line, and PLAY's verdict.

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

1. str0m's exact cert algorithm/key size (read vendor/str0m during
   R3-A; the rig only pins the fingerprint, so any strong algorithm
   SHOULD pass — but mirror the official one).
2. Does the rig initiate DTLS toward the client if the client is
   silent (run 9/10 `invalid_in:0` says NO within ~20 s — client
   initiates, matching the ICE-controlling role).
3. Is PLAY gated on DTLS *completion* or merely *started*? (Run 11/12
   measurements with R3-A/B in place.)
4. unified vs paired sockets on real PL-region seats (the handoff
   line's `unified_socket` field already reports it).
5. Whether the rig's 455-vs-timeout refusal modes correlate with rig
   software versions (collect `serverId`/peer evidence per run).

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
