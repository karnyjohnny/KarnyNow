# KarnyNow — implementation project state (living handoff)

> This file is the **implementation agent's** running state. It supersedes the research
> `HANDOFF.md` (archived verbatim under `uploads/`, along with `BOOTSTRAP.md`, the map of
> everything already known). Read `BOOTSTRAP.md` first for mission, hardware, architecture,
> decisions D1–D13, verified facts, the logging spec (§7), pitfalls and the stage plan.
> Language rule D12: everything in this repo is **English**.

## Bootstrapping a NEW agent session (do this first, ~5 minutes)

Everything needed is public or in this repo. No file transfers from the user required.

```sh
# 1. This repo (source of truth; the user keeps it current):
git clone https://github.com/karnyjohnny/KarnyNow.git
# 2. Protocol reference (MIT; commit a49b124 is the verified baseline —
#    all nvst.rs line numbers in the docs point at it):
git clone https://github.com/OpenCloudGaming/OpenNOW.git
#    Key file: native/opennow-streamer/crates/opennow-streamer-transport/src/nvst.rs
#    Key docs: docs/core-protocol.md, docs/mjolnir-nack-v2.md,
#              docs/session-start-progress.md, docs/streamer-comparison/*
# 3. Run tests/T10_build_and_test.py. It downloads the four small required
#    AES/SHA-1 source files into the ignored third_party/ tree and verifies
#    pinned SHA-256 hashes before configuring/building. No manual vendor setup.
# 4. OPTIONAL (sandbox compile-checks of kn_decode.c only; the TARGET build uses
#    the user's own FFmpeg dev tree instead): headers at the user's exact commit —
#    scripted: python3 tools/fetch_ffmpeg_headers_sandbox.py
#    (downloads bf0e831606938f530326c9679591109407fa8d0d, extracts
#    libavcodec/libavutil/libswscale *.h into third_party/ffmpeg-headers/,
#    writes the avconfig.h/ffversion.h stand-ins). Manual recipe:
#       https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/bf0e831606938f530326c9679591109407fa8d0d
#    extract libavcodec/*.h libavutil/*.h libswscale/*.h -> third_party/ffmpeg-headers/
#    then create two stand-ins for configure-generated headers:
#      libavutil/avconfig.h:  #define AV_HAVE_BIGENDIAN 0   (+ guard)
#      libavutil/ffversion.h: #define FFMPEG_VERSION "N-127939-gbf0e831606" (+ guard)
```

Sandbox toolchain (agent side; **does NOT persist between chat messages** —
reinstall every session, ~2-4 min):

```sh
apt-get update && apt-get install -y gcc cmake ninja-build libc6-dev \
    gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
# optional, for sandbox --decode E2E with a REAL decoder (system FFmpeg 5.1):
apt-get install -y libavcodec-dev libswscale-dev libavutil-dev libswresample-dev
# verify everything before trusting a run:
sh tools/build_native_tests.sh     # host: 10 suites + knvstd smoke + zip/jsonl validation
sh tools/cross_check.sh            # mingw x64+i686, -Wall -Wextra -Werror, F1 flags
```

**The authoritative compile+run gate is the TARGET machine** (w64devkit GCC
16.2.0 — stricter than sandbox GCC 12; it catches warnings GCC 12 misses):
`python tests\T10_build_and_test.py` under ConEmu → `T10_wynik.txt` +
`T10_skrot.txt`. The stage-2 socket gate is `python tests\T13_udp_e2e.py`
(loopback-only, no account; reuses T10's build). The user runs both quickly
and willingly — ask for a measurement instead of guessing (§10.7).

## Current status (updated 2026-10-27, post-round-XXXIII: R7 RUN 14 captured the 400 — `reason:"OK"`, EMPTY body, echo headers only (a generic handler refusal, not the 455 state machine) — and proved **`app_records:0`: the rig sends no app data after the handshake; it waits for OUR SCTP INIT** (`start_sctp(true)` = the client initiates, nvst.rs:8221; the reference opens the association + the 8 DCEP channels BEFORE finish()/PLAY, :8214-8233 → lib.rs:1117). DTLS itself is live-solid: the THIRD consecutive rig handshook (190 ms — the fastest yet), and the round-XXXII video-socket keepalive (vid_ka:23) did NOT change the verdict → **R3-C SHIPPED: kn_sctp — vendored usrsctp (pinned c625e219…, SHA-256 tarball via T10, the mbedTLS model) in AF_CONN mode bridged onto the DTLS app layer (new kn_dtls_write_app/read_app ring; the tx thread-boundary queue keeps the mbedTLS context single-threaded), sctp-proto-matching wire parameters (5000↔5000, OS/MIS 65535, a_rwnd 1 MiB, RTO 3000/1000/60000, unlimited INIT retries, MAXSEG 1200), the FULL 8-channel NVST DCEP profile (sids 0-14, labels/reliability byte-exact per nvst_input.rs, priority 0, protocol ""), rig-OPEN answering, the first-rig-message capture, and PLAY gated on sctp_up (bounded --session-play-wait 2500 + --session-play-settle 250 — deviation D-t5)**. SEVEN usrsctp landmines measured+fixed+pinned (recvv infolen pre-set, RECVRCVINFO plain-int sockopt, ppi network order, conninput stamps both endpoints → the shared-key/port-routed test relay, conn_output re-entry deadlock → the tx queue, accept==COMM_UP, DCEP type strictness) — docs/SCTP.md is the record. Gates: test_knsctp **33/33** real (independent-Python goldens + live INIT parse with CRC-32c + the full 4-way association + the 8-channel burst + loss recovery; ASan+UBSan+LSan clean), tools/sctp_reference.py self-proved (RFC 3720 B.4), ctest **18/18 ×2**, cross real-body legs ×2, native 16 suites, E2E 43/43, T10 dry **66**, T13 63. Target expectation: T10 ALL PASS **76** + R7 run 15 (the decision matrix in Next steps 2; docs/RTSP.md §20)

| Stage | State |
|---|---|
| **0 — measurements** | **COMPLETE.** G0.1 CPU ✅ · G0.2 D3D9 presentation ✅ **(T4 v4)** · G0.3 toolchain ✅ · G0.4 console ✅ |
| **1 — C skeleton, logging, loopback/audio** | **CLOSED.** Stage-1 scope complete; G0 closed and target T11 full G1 passed (see round XV evidence below). |
| **2 — NVST media/transport** | **IN PROGRESS — MEDIA CORE TARGET-VERIFIED (T10 r12: ALL PASS 67) + SOCKET LAYER DONE (round XVII, sandbox-verified incl. the full T13 E2E matrix under three builds + sanitizers; awaiting target T10 r13 + T13 r1).** `kn_rx` (reorder, FEC repair + re-wrap, RFI, NACK tracker + encoders, watchdog, SRTP incl. GCM/CM256) passed on Win7: test_knrx 209/209, rx.vectors 153/153 (scenarios 31/31, srtp256/gcm verified), registry 13. Round XVII adds the live path: `kn_sock` (real UDP, Win32+POSIX), `knvstd --capture` (streaming gs_capture.hex-format recorder — the R32 mechanism), `knvstd --decode` (socket → kn_rx on a real QPC clock → capacity-2 AU queue → the NEW persistent `kn_decode_stream_*` session), `tools/send_rx_capture.py` (paced/blast UDP sender), `tests/T13_udp_e2e.py` (the socket gate: capture → replay-verify → decode paced/blast → SRTP cm128/gcm256 → sock.loopback), and `sock.loopback` — the 14th self-test (portable, gated). **SOCKET LAYER CLOSED ON THE TARGET** (T10 r14 ALL PASS 69 + T13 r2 ALL PASS 61, 2026-10-25): the round-XVIII JSONL-escape fix confirmed; all T13 legs green on Win7 (capture 119/119 + parseable JSONL, live-recorded capture replays byte-identical, decode paced/blast/cm128/gcm256 each 60/60 with FFmpeg 63, zero overflows); **R34 CLOSED: Windows applies the full 8 MiB SO_RCVBUF** (requested 8 388 608 → applied 8 388 608 on every leg). Round XX added the signaling cores (kn_ws 84 + kn_rtsp 127 checks — target-verified in T10 r15, ALL PASS 71). **Round XXI built the REST of the production path: the persistent verified TLS-1.2 stream API (Win32+mbedTLS, honest stubs elsewhere), kn_json + kn_http + kn_cm (CloudMatch create/poll/extract — test_kncm 91 checks), kn_sock_udp_connect (kernel peer filter), the independent Python fake rig + a 36-check real-socket E2E of the WHOLE chain (create→poll→WS→RTSP→handoff→connected UDP), and `knvstd --session` (TLS-only, token length-only logging, queue events for the future TUI, the round-XVII media loop reused verbatim).** Remaining for stage 2 = MEASUREMENTS, not code: target T10 r16 (first compile of the Win32 TLS-stream body), then the R7 first-real-session round (token+appId → `--session`; D-s1/D-cm1/D-cm2 deviations get measured there), then G2. R7 RUNS 1-4 HAPPENED (round XXIII, on the T10 r17 ALL PASS 72 build): create works against the LIVE production cloud (4/4 real sessions, plausible live queue positions; D-cm1/D-cm2 accepted at create); the queue wait exposed ONE contract violation — polls stayed on the global create front (prod.cloudmatchbeta...) instead of the regional `sessionControlInfo.ip` node the create response nominates (np-waw-01... — exactly where the official client polls at ~4.1 s per the user's DevTools capture, and what cloudmatch.rs's learned control_base does :401-414/:652-656/:1149-1152) → frozen queuePosition across 40+ polls + `cm_poll_failed`. FIXED in round XXIII (trust-gated poll-host switch + the reference's transient-retry/404 semantics + no-clobber poll commit + requestStatus surfacing + 4 s cadence + best-effort DELETE cancel on abort + queue-timeout default 600 s); docs/RTSP.md §10 is the record. **R7 RUN 5 (round XXIV) CONFIRMED THE FIX LIVE: cm_control switched to np-waw-01 (source sessionControlInfo), the session progressed seat_step 1→3 to status:2 READY with a real rtsps endpoint, and OPTIONS returned 200 over RTSPS/WSS on the real rig; DESCRIBE then died rc:-2/status:0 — the 8 KiB per-message scratch in kn_rtsp_request (real SDPs are bigger; the reference accumulates up to 4 MiB, nvst_rtsp.rs:559-566/:614-623) and/or the refused BINARY frames (:621) — both FIXED, plus Host:port fidelity (:1070-1076) and rtsp_detail failure instrumentation; docs/RTSP.md §11 is the record. **R7 RUN 6 (round XXV): DESCRIBE/SETUP/ANNOUNCE 200 LIVE (the §11 fix confirmed; SETUP peered first round; the D-s1 placeholder ANNOUNCE was ACCEPTED); PLAY 455 — the reference spawns its media receivers between announce() and PLAY (lib.rs:894→937→1117) and the bundle thread immediately fires the hole-punch burst (3×STUN+1×NATT per 20 ms, nvst.rs:7438/:7842-7900, dest bundle_peer() :1880); we sent PLAY with zero UDP egress. Round XXV ships the kn_sock STUN toolkit (byte-exact builders, Python-independent goldens), the kn_rtsp credential ladder (+ a caught pre-existing sdp_attribute LAST-match bug), the knvstd pre-PLAY pump + 455-retry (D-s2/D-s3), and the rig now gates PLAY exactly like run 6 (E2E 43/43). The user also proved R23 RESUME real: run 6's session resumed+played in the browser. docs/RTSP.md §12 is the record. **R7 RUN 7 (round XXVI): the pump never got its live try — BOTH attempts died earlier, in the queue: after ~29 static step-1 polls (~2 min — the exact mark where runs 5/6 transitioned to seat_step 3) every GET poll answered 503 (15 attempts/attempt-run, rc:0 = parsed responses) while a SAME-MOMENT DELETE to the same host answered 200 (cm_cancel target-proven: the user watched the session leave the web UI; the immediate re-create hit no SESSION_LIMIT). The round-XXIII 5-round give-up killed the wait inside the seat-transition window. Round XXVI mirrors the reference (which never gives up on transient poll errors): transient 4xx/5xx are OUTLIVED until queue_timeout, transport deaths bounded at 10 rounds, hard rejections at 5 (new stop_reason cm_poll_rejected), the poll gap backs off 4→6→9→13.5→20 s while failing (D-s4), and cm_poll_error now carries the response BODY excerpt + class + gap — run 8 names the 503's sender. docs/RTSP.md §13 is the record. **R7 RUN 8 (round XXVII): the policy PROVED itself — the client rode ten transient rounds with the visible backoff (gap_ms 6000→9000→13500→20000) until the user's Ctrl-C (interrupted → cm_cancel 200; the queue-stage session then correctly left the web UI — DELETE worked; run 6's resume survived only because READY sessions aren't cancelled). The captured 503 body is CloudMatch's OWN JSON: a session SKELETON with sessionControlInfo.ip:null + the id echoed in resourcePath — the front knows the record but can't name its owning state mid-transition; the record is alive (same-window DELETE 200). Round XXVII: the deep fields (status/errorCode/requestStatus+unifiedErrorCode — now filled even on rejected payloads) ride in cm_poll_error; the T10-r20 warning (write-only consec_transient; target GCC 16.2's -Wunused-but-set-variable is not in sandbox GCC 12's -Wall — the FIFTH target catch) is fixed AND gated permanently (paranoid knvstd legs in both scripts); the control node is RE-LEARNED per successful poll (store_active :1333-1336 fidelity — a mid-queue migration is followed; ip:null keeps the host); --session-keep skips the abort cancel for browser-RESUME experiments. docs/RTSP.md §14 is the record. **R7 RUN 9 (round XXVIII): T10 r21 ALL PASS 72 (zero warnings — the paranoid legs held). The re-learning fired ~25× in one wait — the nominated control front migrates nearly every poll (np-waw→ams→sth→lon→par→frk…) and EVERY nominated node answered: the 503 class of runs 7/8 VANISHED (zero 503s, clean step 1→3→ready; runs 7/8 were us pinned to a node the record had drifted from — the ip:null skeleton's real meaning). The XXV STUN toolkit is live-proven byte-correct: ice_creds enabled:1 via the pingVersion-6 bundle-username ladder leg (ufrag/bundle/natt/payload lengths all 13 — the real DESCRIBE carries iceUsernameFragment + serverBundlePort), ice_preplay = stun 147 / natt 49 / rig-requests 99 ALL answered / rig-successes 327 / invalid 0 — a fully bidirectional validated ICE exchange. PLAY still 455 (burst existence necessary but not sufficient), and the 455-retry hit a DEAD WS (rc -2, ws_close -1 — the rig drops the RTSP context after refusing). Remaining deltas vs the reference at PLAY time: the sequential pump goes quiet DURING the exchange (the reference thread never stops) and D-s1 (no real DTLS identity; unproven — the rig sent no DTLS: invalid 0). Round XXVIII: the pump moves to a bounded worker thread spanning exactly the PLAY exchange (joined before the media loop — D21 untouched; sequential fallback kept), PLAY is ONE-SHOT, and the media loop gains the reference's Mjolnir NATT keepalive (nvst.rs:8849-8905 — 'the pings are what ROUTES video here': {setup payload}:{local ufrag} to the video peer, 20 ms pre-first-inbound then 100 ms, + the natt_keepalive evidence line); cm_control/ready now quantify the migrations (n/relearns). docs/RTSP.md §15 is the record. **R7 RUN 10 (round XXIX): the pump thread spanned the ENTIRE 20 s PLAY window (thread:1, pump_ms 20525, 2760 STUN + 920 NATT, the rig's 123 probes ALL answered, 3671 rig successes validated, invalid_in 0 — difference (a) CLOSED) and PLAY received NO response at all (rc -3, ws_text stuck at 4, no close frame). A second rig (80.250.98.40) refused DIFFERENTLY than run 9's (instant 455 vs hold-until-timeout) — same root state. The elimination table (RTSP.md §16) now has ONE row left: the D-s1 placeholder DTLS identity — the reference announces a real certificate's fingerprint (nvst.rs:5562-5603) and runs ICE+DTLS on the bundle from receiver-spawn (:6106-6160: client ICE-CONTROLLING, remote fingerprint pinned from DESCRIBE, video SRTP keys stay ANNOUNCE-derived, DTLS carries SCTP rtcp1+input). **R3 PROMOTED to the stage-2 PLAY blocker — docs/DTLS.md (NEW) phases it: R3-A kn_cert (PORTABLE mbedTLS: self-signed cert + colon-hex SHA-256 fingerprint, sandbox-KAT'd both platforms, cross-compiled for Win32 per the round-XXII rule) → R3-B kn_dtls (DTLS 1.2 client on the bundle socket, fingerprint-pinned, inside the pump thread's state machine, + the N+1 Mjolnir sibling socket when !unifiedSocket) → R3-C usrsctp (rtcp1 + input channels) → R3-D WS control pings (NvstControlPing :260-290/:844-930).** Round XXIX also mirrored the OFFICIAL ICE identity shapes (generate_gfn_local_ice_credentials :5545-5559 — 4-char ufrag / 22-char password over the base64-ish alphabet; str0m's 16-char default is Bifrost-rejected :5564 — our 16/24-hex had been tolerated but fidelity rules), added the 400 ms pump warm-up before PLAY (run 10 raced the thread start) and the full WS counters in rtsp_detail. **R7 RUN 11 (round XXX) delivered the predicted final datum: a THIRD rig (77-111-249-146; ANNOUNCE 200, 4137 B — the 4/22 shapes live) held PLAY in TOTAL silence — timeout with ws_pings_in 0 / ws_closes_in 0 / frames_out 5 vs text 4 — while the pump thread saturated UDP beside it (pump_ms 20920, 2820 STUN + 940 NATT, the rig's 126 probes ALL answered, 3753 rig successes validated, invalid 0). The elimination is FINAL: the only remaining difference is the missing DTLS layer behind the announced fingerprint. R3-A SHIPPED the same round: kn_cert — portable-mbedTLS ECDSA P-256 self-signed identity, SHA-256 fingerprint over the exact DER in the announced colon-hex form (nvst.rs:5596 ≡ str0m lib.rs:1662), fixed 2020-2035 validity, selfcheck KAT (fp==SHA-256(der) + byte-identical re-parse + sign/verify through the cert's public key); the CMake mbedtls backend compiles the REAL body on both platforms (sandbox ctest: real battery 10/10 under ASan+UBSan — which caught a double-free in the first test draft pre-ship), every other build gets the honest stub (4/4); cross_check compiles the real body under the exact target flags ×2 arches (the round-XXII rule); knvstd announces the real fingerprint when available with the new identity evidence line ({"cert":"real"|"placeholder"} — the log never lies). NEW suite test_kncert (14th) → ctest 16/16 ×2, T10 suites +1. docs/RTSP.md §17 + docs/DTLS.md (R3-A marked SHIPPED) are the records. **R7 RUN 12 (round XXXI): the real fingerprint flew (identity cert:real fp_len 95, ANNOUNCE 200 4137 B, 4th rig 80-84-170-153) and PLAY STILL timed out in total WS silence with the saturated pump (20921 ms, 2841+947, 126/126 answered, 3788 validated, invalid 0) — the fingerprint ALONE is not the gate; run 12's T10 said 72 not 73 (the target harness is stale — sync it). R3-B SHIPPED the same round: kn_dtls — the DTLS 1.2 ACTIVE client (nvst.rs:6300 start_dtls(true)) with the double-gate fingerprint pinning (VERIFY_OPTIONAL + the callback + poll's ready refusal — ssl_tls.c:8071 makes REQUIRED-without-CA impossible; landmine documented), the inject-queue BIO (WANT_READ never TIMEOUT — the resend-NULL-flight segfault class), ms-clock timers, MTU 1200, the counted app-record drain (D-t4 → R3-C); the video-N/bundle-N+1 socket pair (preferred 49005/49006; SETUP port unified?bundle:video; clientBundlePort/candidate from the bundle socket; the reconnect dance GONE — one peer per socket for life); the pump-thread DTLS state machine (ClientHello gated on the first REAL inbound STUN :8196, post-ready NATT-only 100 ms :7837, PING→PONG :8605, dtls_ready/dtls_fail from the thread, the snapshot handshake, the worker persists through the media loop in non-unified mode — D21 untouched); the evidence lines (sock_pair/dtls_arm/grown ice_preplay/unified in ice_creds). Gates: test_kndtls 31/31 real (rig-shaped in-test server: HVR + mutual pinning; loss legs both directions; wrong-pin latch; ASan+UBSan clean) + the OpenSSL-3 interop PASS + ctest 17/17 ×2 + cross ×2 + E2E 43/43 + T10 dry 64 + T13 63. docs/RTSP.md §18 + docs/DTLS.md (R3-B marked SHIPPED) are the records. Next = the target T10 with the SYNCED harness (expect ALL PASS **74**, zero warnings) + R7 run 13 (dtls_ready → PLAY's verdict; if PLAY 200 → the first real Mjolnir media; if refused WITH dtls_ready:1 → R3-C (SCTP) is the gate; dtls_fail → the mbedtls rc names the fix). **R7 RUN 13 (round XXXII): T10 ALL PASS 74 confirmed, then THE DTLS HANDSHAKE COMPLETED ON A LIVE PRODUCTION RIG (dtls_ready ms:388, DTLSv1.2, TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256, verify:0 — the rig requested+verified our cert against the ANNOUNCE'd fingerprint; twice, incl. a RESUME-at-create re-attach with a fresh cert in 293 ms) — R3-B LIVE-CLOSED. sock_pair bound the preferred 49005/49006; the seat is `unified:0` (Q4 answered: the two-socket layout is production). PLAY moved silence→explicit **400** (the rig now evaluates the request; body NOT captured — the evidence gap) and the rig sent exactly ONE post-handshake app record (discarded per D-t4, content unknown). At PLAY the reference additionally has: the SCTP INIT (start_sctp(true) the instant DTLS connects, :8214-8223) and the PRE-PLAY Mjolnir NATT keepalives from the video socket (lib.rs:990 arms the mjolnir worker before finish(); :8849-8905) — run 13 left 49005 silent. Round XXXII shipped the instrumentation + that fidelity piece: `rtsp_response` (the full non-200 capture: reason/headers/body), `rig_info` (the Server header), the first-app-record hex capture in ice_preplay, the pump-thread video-socket keepalive (send-only + vid_handoff + vid_ka), live pump_ms, test_kndtls 32. NO hand-rolled SCTP (poison precedent — run 14's captures decide R3-C). docs/RTSP.md §19 is the record. Next = R7 run 14: the 400's own words → targeted fix or R3-C promotion; if PLAY 200 → the first real Mjolnir media on 49005 (R32's final gate). **R7 RUN 14 (round XXXIII): the capture worked — PLAY 400 `reason:"OK"` with an EMPTY body and echo-only headers (a generic refusal), `app_records:0` (the rig initiates nothing — the CLIENT must, nvst.rs:8221), dtls_ready 190 ms on a third rig, vid_ka:23 proved the video keepalive wasn't the gate. R3-C SHIPPED the same round: kn_sctp (vendored usrsctp c625e219 AF_CONN over the new kn_dtls app bridge, sctp-proto wire parameters, the 8-channel DCEP profile opened on COMM_UP inside the pump thread, rig-OPEN answering, bounded PLAY gating on sctp_up = D-t5) + tools/sctp_reference.py (the independent Python: CRC-32c self-proved against RFC 3720 B.4, hand-transcribed DCEP goldens, the INIT builder) + test_knsctp (16th suite, 33 real checks: the live INIT parsed by the independent reader — ports 5000, CRC valid, OS/MIS 65535; the full 4-way association over a memory relay; the 8 OPENs received byte-exact + ACKed; data both ways; SACK loss recovery; ASan clean). SEVEN usrsctp landmines measured and documented (docs/SCTP.md §5 — incl. the recvv-infolen NOINFO trap and the conn_output re-entry deadlock). Gates: ctest 18/18 ×2, cross ×2 incl. real legs, native 16 suites, E2E 43/43, T10 dry 66, T13 63. Next = target T10 (expect ALL PASS **76**) + R7 run 15: sctp_arm/sctp_up/sctp_channels → PLAY's verdict (200 → the first real Mjolnir media = R32's final stage-2 gate; still-400 → the sctp_first* capture names what the rig wants next).** |
| **3–5** | Not started; see BOOTSTRAP stage gates. |

Stage 1 closure: T11 full G1 passed on the target before this continuation; graphics, decode, audio, and logging gates are closed. No GPU retest is needed for the current Stage-2 socket work (round XVII touched no graphics code).

**Latest full target T10 (user-reported 2026-10-24 12:52, round 12):** `ALL PASS`, **67/67 checks** — the round-XVI media core verified on Win7 with mbedTLS default: build zero-warning (116 s), 9 unit suites (test_knrx **209/209** first target run incl. the GCM/CM256 legs), registry **13** self-tests all usable, `rx.vectors ok` (153 checks, scenarios 31/31, `srtp_backend:1`), decode 60/60 @ 3333 fps, d3d probes reproduced (pitch 1536, present mean 0.29 ms), net.tls/tls.chain OK (TLS 1.2, depth 3, Amazon Root CA 1), bundle 139 545 B validated, PE 6.01. `clock.resolution` reported **ok** this run (Sleep(1) 1.00→1.75 ms, wall tick 1000→1000 µs — the machine state swung back; R30 note updated). Raw evidence: `artifacts/T10-round12/`.

**Latest full target T10 (round 14, 2026-10-25 09:54):** `ALL PASS`, **69/69 checks** — the round-XVIII JSONL-escape rebuild gate: zero warnings, test_knselftest 114 (Win32 delta), test_knrx 215, test_knsock 71, registry 14 all usable. `clock.resolution` reported **degraded** (Sleep(1) 15.62→2.88 ms, wall tick 15600 µs) — the machine flipped back to the sleep regime hours after r13's healthy 1000 µs ticks (R30's documented swing; T10 accepts degraded by design). Raw: `artifacts/T13-round2/`. (Round 13, 08:36, was the round-XVII socket-delta verification, also ALL PASS 69:) test_knsock **71** checks (host 70 + Win32 truncation-branch delta), test_knrx **215**, registry **14**, `sock.loopback ok` (64/64 burst in 1 ms, timeout 50 ms), zero warnings, decode 60/60 @ 4000 fps, net.tls/tls.chain ok, clock.resolution ok (healthy regime: Sleep(1) 1.00→1.00 ms, wall tick 1000 µs). **First R34 datapoint: Windows applies SO_RCVBUF EXACTLY as requested (262 144 → 262 144).** Raw: `artifacts/T13-round1/T10_wynik_r13.txt`.

**Target T13 (round 1, 2026-10-25 08:39): FAIL (2 of 51) — ONE product defect, fixed round XVIII.** Every stage-2 mechanism worked on Win7 (capture recorded 119/119 — proved by the vector leg parsing live.hex; ALL decode legs 60/60 with real FFmpeg 63 incl. **gcm256 SRTP live**; sock.loopback ok; zero queue overflows; paced sender held the 2 s timeline in 1966 ms). The two failures were both the same JSONL-contract defect: **Windows absolute paths embedded RAW into stdout JSON** (`"out":"C:\Users\…"` → `\U` is not a legal escape) made the capture-summary and replay-summary lines unparseable ("VERIFY FAIL: 1 unparseable stdout lines" while all 60 frames actually matched). Fix: every argv/path/foreign string in knvstd's printf-JSON sites now routes through the loopback-proven `lb_json_path` sanitizer (T11 never hit this because loopback had it since round XII), hardened with a `>=0x7F → '?'` rule (Win32 argv is cp1250 — raw non-ASCII would be invalid UTF-8 in the JSONL); `st_gs_parse`'s KN_GS_CAPTURE path likewise (`st_json_str`). Reproduced 1:1 in the sandbox with literal-backslash filenames before/after; T13 hardened with per-leg "every stdout line parses" checks + a POSIX backslash-path leg + rcvbuf logging. Full story: docs/SOCK.md landmine 6, evidence `artifacts/T13-round1/`.

**Target T13 (round 2, 2026-10-25 09:58): ALL PASS, 61/61 checks — the exact predicted count.** The round-XVIII fix is target-verified: capture leg 119/119 records with every stdout line parseable; vector leg replays the live-recorded capture byte-identical to the reference; ALL FOUR decode legs 60/60 with libavcodec 63.2.100 (paced + blast + cm128 + **gcm256 live over a socket**), zero q_overflows, zero recv_errors, every stdout verified against the Python manifest; sock.loopback ok. **R34's headroom evidence: `rcvbuf_actual=8388608` on every leg — Windows honours the full 8 MiB request.** The stage-2 socket layer is CLOSED; raw: `artifacts/T13-round2/`.

**G2-offline demo on the target (idk_g2.txt, 2026-10-24): SUCCESS — the one failed command was a missing shell redirect, not a product bug.** Capture generation matched the sandbox byte-for-byte (seed-1 determinism across machines: 119 records / 360 333 B); `--replay-capture` delivered 60/60 AUs (fec_repaired=19, zero stale/gaps/withheld, state 0); ffprobe accepted the dumped AU. The `--verify` step failed with "Nie można odnaleźć określonego pliku" because knvstd's stdout was never redirected into `replay.jsonl` (cmd could not open the missing redirect INPUT — python never ran). The console JSONL was reconstructed in the sandbox and verified against the regenerated manifest: **VERIFY PASS, 60 frames byte-identical to the reference receiver**. tests/README.md's demo block now carries the redirect inline; T13 automates the whole sequence.

**Latest target T12 (2026-10-23 22:47):** `ALL PASS`, 27/27 checks using `KN_TLS_BACKEND=mbedtls`. Build is warning-free; DNS + TCP/443 pass; TLS verifies `login.nvidia.com`, CloudMatch, and Cloudflare; `tls.chain` is clean, depth 3, root Amazon Root CA 1, trust/policy both zero. Probe is pinned to TLS 1.2 (min=max); TLS 1.3's Finished/PSA issue remains isolated and is not used on Win7. User subsequently reported full T10 `ALL PASS (65 checks)`.

The embedded backend is now the default in CMake and both T10/T12 harnesses. It uses pinned mbedTLS 3.6.0 static CMake targets, bounded Win32 Winsock, hostname/SNI verification, `MBEDTLS_SSL_VERIFY_REQUIRED`, read-only Windows ROOT-store imports, and PSA initialization before TLS use. Schannel remains an explicit diagnostic fallback. Do not disable verification.

Gate G0.2 evidence and the settled presentation chain: `docs/analysis/G0.2-T4v4-RESULTS.md`.
One-line summary: **NV12 `StretchRect` from a DEFAULT-pool surface into the backbuffer
works (S_OK, 3.0 ms incl. Present); SYSTEMMEM NV12 fill crashes the driver (0xC0000005,
both arches, exact call isolated) and is permanently excluded.**
`kn_d3d` design + provenance + crash-guard rationale: `docs/D3D.md`.

## Decision register — additions this session

D1–D13 are in `BOOTSTRAP.md` §4 (unchanged, except D7 settled below).

| # | Decision | Reason / source | Reversal condition |
|---|---|---|---|
| **D7 (settled)** | Presentation chain: NV12 DEFAULT → `StretchRect(LINEAR)` → backbuffer; fallbacks YUY2 → X8R8G8B8 → GDI `SetDIBitsToDevice`. No SYSTEMMEM NV12, no offscreen conversion targets, `CheckDeviceFormatConversion` before every `StretchRect`, permanent degrade on AV. | T4 v4 measurement; `docs/analysis/G0.2-T4v4-RESULTS.md` §5 | none expected |
| **D14** | Default video mode **720p30**, switchable. Both mechanisms: a **settings screen** choice (applied at launch) **and an F7 in-session hotkey** cycling 720p30 → 540p60 → 540p30 → 360p30. In-session application needs renegotiation (`dynamicStreamingMode` / re-ANNOUNCE) = stage 5; until then F7 queues the choice for the next launch and says so on screen. | User answer to open question Q2 ("720p default, switchability nice; F7 vs settings — don't know") → provide both, staged honestly | user preference |
| **D15** | Game list = **account library only** (`filters: {variants:{gfn:{library:{status:{notEquals:"NOT_OWNED"}}}}}`), not the full GFN catalog. R24 pagination guards (context hash + revision re-check before/after each page) still mandatory. | User answer Q3 (uses GFN for owned/free-to-play games) | user preference |
| **D16** | Stream renders in a **separate window**; all keyboard/mouse capture lives in the streamer process; the Python TUI stays in the console. (Confirms the BOOTSTRAP §3 architecture — was a recommendation, now a user requirement.) | User answer Q4 | — |
| **D17** | Artwork: **text-only game list for stage 4**. A bounded local artwork cache may be added behind a flag after G4 (disk + HTTP cache on a 2 GB machine — deferred, non-blocking). | User answer Q5 ("don't know") → decide by not blocking on it | revisit after G4 |
| **D18** | **FFmpeg is consumed STATIC** (`--enable-static --disable-shared`), linked into `knvstd.exe` under the canonical F1 `-static` (D8). The decode.synthetic clip is **embedded in the binary** (`streamer/src/kn_synthetic_clip.h`, generated by `tools/make_synthetic_clip.py` from `streamer/assets/synthetic.264`; 60 frames 320×240 yuv420p, 42 KB). | T10 round 2 proof: GNU ld with `-static` **ignores `.dll.a` import libraries** ("cannot find -lavcodec — have you installed the static version?") — a shared-only FFmpeg cannot link into an F1 binary. Embedded clip: the user's FFmpeg build has no encoder (`--disable-autodetect`, no libx264 — `-preset` unrecognized), and §7.9 says "a BUNDLED .264" — hermetic beats external file. Static also kills DLL deployment failure modes on Win7. | If a future dependency is shared-only (e.g. an NVENC-style DLL), relax `-static` per-target and ship DLLs next to the exe (the CMake POST_BUILD copy path still exists) |
| **D20 (new, round XIII)** | **On-screen FPS counter overlay, low-overhead, toggleable by key (F5).** 5x7 bitmap font blitted straight into the locked NV12 surface during the per-frame copy (no D3DX, no GDI, no lockable backbuffer — zero new imports, ~1.5 k pixel writes/frame, inside the VEH-guarded section); shows the SAME number perf.jsonl reports (frames presented per completed 1-s interval). Default ON in `--loopback` (debug surface); production streamer defaults OFF with the same toggle (stage-2 wiring). | User requirement 2026-10-23 ("niskonarzutowy fps counter ze switchem pod klawiszem — dobrze wiedziec, ile fps generuje streamer"); implementation choice follows the measured D3D9 constraints (G0.2: offscreen conversion targets refused; d3dx9 not OS-built-in on Win7) | user preference |
| **D19** | **Toolchain stays w64devkit** (GCC 16.2.0 + cmake 4.4.3 + ninja, `C:\Programy\w64devkit`). NOT WinLibs, NOT Nuwen, NOT Cygwin. | It is measured end-to-end on the target: T2 (15 variants ran on Win7), T9/T10 (zero-warning builds, correct PE 6.01, imports only KERNEL32/msvcrt/WINMM), FFmpeg built with it works. Cygwin is disqualified by design (POSIX runtime layer, not native Win32). WinLibs-MSVCRT/Nuwen are equivalent-ish but unproven here — switching re-opens R6 for zero gain. | Only if w64devkit develops a blocking defect; WinLibs **MSVCRT** variant (never UCRT — no UCRT on stock Win7) is the designated fallback, re-run T2 before trusting it |
| **D21 (new, round XVII)** | **The stage-2 live loops (`--capture`, `--decode`) are SINGLE-THREADED**: one loop does `recv(10 ms timeout)` → `kn_rx_video_process_datagram` → inline AU-queue drain → `kn_rx_video_poll` at the reference's 10 ms cadence, all on the QPC-ms clock. The capacity-2 encoded queue keeps the reference's overflow semantics (drop-new + `kn_rx_inval_lose` + `kn_rx_video_request_keyframe`) so a later receive-thread split touches no pipeline logic. The 8 MiB SO_RCVBUF absorbs decode stalls (~16 ms worst 720p frame ≈ 500 datagrams at 30 fps). | Stage-1 loopback precedent (same shape, 30.000 fps for 600 s, G1); 2-core E5500 — threads cost context switches the budget cannot spare; kn_rx is single-threaded by contract (no locks = no lock bugs); sandbox T13 blast leg (119 datagrams in <1 ms) decoded 60/60 with zero overflows. | **Measured** socket-buffer backlog or `recv_errors`/`q_overflows` in a REAL session's decode summary → move the receive side to a thread (docs/SOCK.md §3 revisit condition) |

## Risk register — changes this session

| # | Change |
|---|---|
| R2 | **CLOSED** (T4 v4: NV12 path works; crashing variant isolated and excluded) |
| R2b | **CLOSED** (v4 writer preserved all data through a crashing run — pattern proven; the same rules ship in the client per §7.12) |
| R9 | **Mitigated** (60 Hz available and active; user keeps 60 Hz on battery; D9 stands) |
| R17 | **CLOSED** (GPU does CSC + scale; CPU never touches pixels) |
| **R25 (impl round X: CLOSED)** | **CLOSED by T10 round 5 on the target:** the locked-DEFAULT-NV12 fill works — 1280×720 surface, **pitch 1536 (≠ width!)**, 1 658 880 B filled + read-back verified per iteration, 30/30 iterations, 0 mismatches, no AV, VEH never fired, standalone AND in-process runs ok. Stage-1 video path unblocked. Two new hard facts (docs/D3D.md "Target evidence"): per-row copy mandatory (pitch 1536 vs kn_decode's 32-aligned stride) and first-touch costs ~28 ms (pre-fault the surface before the paced loop). |
| **R29 (round X: happy path verified, catch path UNVERIFIED)** | mingw GCC has NO `__try/__except` (verified both arches) — the crash marker is `AddVectoredExceptionHandler` + `longjmp`. Round 5: the guard armed/disarmed cleanly 4× on the target (2 runs × 2 probes) and never fired — happy path proven on real D3D9. The catch path itself (longjmp out of an AV) **cannot be provoked without risking the driver — stays UNVERIFIED by design**; if it ever fires in the field, the latch + standalone-process order (T10 milestone run first) contain the damage. |
| **R26 (REVISED, impl round VI/VII)** | The agent sandbox **DOES have a working toolchain** (the round-II "apt fails" era is over): `apt-get update && apt-get install -y gcc cmake ninja-build libc6-dev gcc-mingw-w64-x86-64 gcc-mingw-w64-i686` (~2-4 min). **Packages do NOT persist between chat messages** — reinstall at the start of every session (see Bootstrapping). If apt lists hang, fall back to direct .deb downloads from `deb.debian.org/debian/pool/` + `dpkg -i` (proven: libc6-dev must match installed libc6 `2.36-9+deb12u14`; ninja must be bookworm `1.11.1`, NOT trixie `1.13.2` which needs GLIBC_2.38). Sandbox GCC is 12.2 — the TARGET's GCC 16.2 remains the authority (it catches `-Wformat-truncation` classes GCC 12 misses; see R28). Every C delta still ships through `build_native_tests.sh` + `cross_check.sh` + a target T10 run. |
| **R27 (new, CLOSED by D18)** | `-static` (F1/D8) vs shared FFmpeg: ld ignores `.dll.a`, error text is misleading ("have you installed the static version?"). Reproduced 1:1 in the sandbox. Mitigation shipped: CMake configure-time FATAL_ERROR with the rebuild recipe when `lib/libavcodec.a` is missing; FFmpeg rebuilt static per D18. |
| **R28 (new)** | Sandbox GCC 12.2 vs target GCC 16.2 warning drift: T10 round 1 failed on two `-Wformat-truncation` GCC 16 flags that GCC 12 accepted (timestamp formatters). Mitigation: all `snprintf` with time/foreign-string inputs use **explicitly bounded arguments** (modulo/precision), verified at `-O0..-O3`; T10 fails the build on ANY warning so drift can never ship silently. Round IX re-confirmed the discipline (cross `-Werror` caught a 64-bit format promotion in the new d3d code on the first pass). **Round XI third incident:** GCC 16 flagged the new `st_gs_parse` capnote (worst case 284 B > 256 B buffer, sandbox GCC 12 default level silent) — fix verified under `-Wformat-truncation=2` (paranoid level) on BOTH toolchains; that flag level is now the pre-check for any new snprintf aggregation before handover. |
| **R30 (round X: stable pattern, informational; round XVII: swings BOTH ways)** | `clock.resolution` variance is NOT transient load: rounds 4+5 both measured Sleep(1) 15.6→**2.75/2.88** ms (verdict `degraded`, threshold 2.6 from round 1's 1.25→1.00) with wall tick pinned at 15600 us **even after timeBeginPeriod(1)** (round 1 saw 1250 us ticks — the machine state changed between rounds, probably a reboot cleared whatever had raised the timer). Rounds 8/9/12/13 then measured the OTHER state again (r12 2026-10-24 `ok`: Sleep(1) 1.00→1.75 ms, tick 1000; r13 2026-10-25 08:36 `ok`: 1.00→1.00, tick 1000) and **r14 (09:54, the SAME DAY ~80 min later) flipped back to `degraded` (15.62→2.88 ms, tick 15600)** — the regime can change within HOURS on one machine, not just across reboots; alternate between a ~1 ms and a ~15.6 ms timer regime. Consequences: (a) treat **~2.9 ms as the realistic WORST-CASE Sleep(1) floor** on this machine — enough for 33.3 ms frame pacing at 30 fps, NOT enough for precise 1 ms spins; (b) wall-clock granularity may be 15.6 ms — timestamps below that resolution are quantised in the degraded regime; (c) D9 stands stronger than ever: pacing measures real Present intervals, never assumes; (d) the T13 sender's hybrid sleep+spin pacing is regime-proof by design. Thresholds unchanged (honest `degraded` beats a fudged `ok`). |
| **R15 (impl round II)** | **Mostly closed.** T8 on the target resolved all 14 Python packages rc=0 (pip honours Requires-Python, so every pin declares 3.8 support): textual 6.2.1, rich 14.3.4, httpx 0.28.1, httpcore 1.0.9, h11 0.16.0, anyio 4.5.2, + 8 more — `python/requirements.txt` updated, wheels cached on target in `tests\t8_wheels\`. Remaining: offline install + runtime smoke at stage 4 (httpx 0.28 removed `proxies=`; textual 6.x API churn vs old docs). |
| **R31 (updated)** | Sandbox has host GCC 13.3 but no MinGW/Windows SDK headers (apt install denied). Target T12 now confirms the mbedTLS Win32 compile/link, zero warnings, TLS 1.2 verification on all three hosts, and a clean depth-3 chain. TLS 1.3 Finished/PSA still fails; the Win7 backend is capped at TLS 1.2. |
| **R32 (round XVII: REVISED — mechanism shipped, real traffic still pending)** | **kn_rx has still never seen a real GFN packet**, but the recording/rehearsal mechanism now exists and is proven: `knvstd --capture` writes real-socket datagrams in gs_capture.hex format (so ANY real session becomes a `--replay-capture` regression vector), `tools/send_rx_capture.py` + `tests/T13_udp_e2e.py` rehearse the whole graph offline (capture → replay-verify → decode paced/blast → SRTP cm128/gcm256), and the target's own G2-offline demo run (idk_g2.txt) verified byte-identical against the reference. The canonical parity construction, SPI-vs-seq relationship and GCM256-tag8 profile remain DERIVED from the reference and its tests — first real Mjolnir traffic (stage-2 WSS/RTSP + a live session) is still the true test. Until then every deviation is documented (RX.md D-a..D-e) and T13 exercises the loss/reorder/RFI/SRTP paths over real sockets. |
| **R33 (round XVI: CLOSED; round XVII: extended)** | Arena-memory class of bugs: two real defects (reorder-map `used` garbage from malloc; 8-byte arena misalignment) slipped past static-context unit tests and were caught by the malloc'd-arena scenario battery + UBSan BEFORE handover. Rule adopted: vector runners allocate through malloc (never statics), every arena carve rounds to 8, and ASan+UBSan+LSan is part of the pre-handover gate for any kn_rx-family change. **Round XVII proved the rule's worth again: LSan caught a real leak in the NEW `kn_decode_stream_close()` (kd_deliver's NV12 buffer, 115 264 B) on the first sanitized E2E run — fixed, and the whole T13 matrix now passes under ASan+UBSan+LSan.** Sanitizer policy note: the vendored `third_party/crypto-algorithms-sha1/sha1.c:26` (`data[j] << 24`, int promotion) is PRE-EXISTING formal UB in a hash-pinned file that must NOT be edited — sanitizer runs use recover mode and the gate is ZERO diagnostics from `streamer/` code (verified: the sha1.c line is the only runtime error in a sanitized T13). |
| **R34 (round XIX: CLOSED except the real-session firewall note)** | **The Winsock side of the live modes is target-verified (T13 r2: ALL PASS 61).** capture recorded 119/119 datagrams with parseable JSONL, every decode leg ran 60/60 (paced + blast + cm128 + gcm256), sock.loopback ok, select-cadence and QPC clock behaved, the round-XVIII JSONL-escape fix confirmed. **SO_RCVBUF: Windows applies requests EXACTLY — `rcvbuf_actual=8388608` on every decode leg (the full 8 MiB; ~6 700 datagrams of slack at shard 1296 — the burst-headroom math holds on the target).** The only remainder: a REAL session binding INADDR_ANY with inbound internet UDP — the firewall-allow question is deferred to the WSS/RTSP round and must be MEASURED, not assumed (loopback legs proved WFP does not filter 127.0.0.1). |

Everything else (R3 usrsctp/DTLS spike, R7's first READY session — the
first real `POST v2/session` itself is now MEASURED, 4/4 OK, round XXIII;
R22 31-field session context, R23 RESUME, R24 catalog revision binding)
unchanged — see BOOTSTRAP §9.

## Next steps (ordered)

1. **Target-verify round XXXIII: `python tests\T10_build_and_test.py`** —
   expect ALL PASS **76** (16 suites: test_knsctp is new; the harness
   now also DOWNLOADS the pinned usrsctp tarball — c625e219…, SHA-256
   verified — into third_party/usrsctp on first run, needs internet
   like the mbedTLS/vendor fetches) and **zero warnings** (the usrsctp
   target compiles with scoped -w like the mbedtls suppression — OUR
   files stay strict). test_knsctp runs the REAL 33-check battery on
   Win7 (the loopback association + the independent INIT parse).
   Round XXXIII sandbox gates: native 16 suites, cross + the kn_sctp
   real-body/test real-section legs ×2, ctest 18/18 ×2, ASan+UBSan+LSan
   33/33, E2E 43/43, T10 dry 66, T13 63, paranoid knvstd legs clean
   with BOTH backend defines ×3 opt levels. T13 optional (unchanged,
   61 on Win7).
2. **R7 run 15 — the SCTP round (docs/RTSP.md §20 + docs/SCTP.md).**
   Same command: `knvstd --session --session-token=... --session-app=N
   [--session-title=...] --session-seconds=60 --session-au-dir=aus`
   → the expected sequence: sock_pair → identity cert:real → ANNOUNCE
   200 → dtls_arm → **dtls_ready** → **sctp_arm {"armed":1}** →
   **sctp_up {"ms":…}** → **sctp_channels {"opened":8,"acked":8,…}** →
   PLAY (now AFTER the association, D-t5) → the verdict. The decision
   matrix:
   * **PLAY 200** → the media loop on 49005: watch `natt_keepalive`,
     the summary's datagrams/aus/frames = **the first real Mjolnir
     SRTP → kn_rx → AUs (R32's FINAL stage-2 gate)**; paste handoff +
     summary too.
   * **PLAY still 400 with sctp_up + acked:8** → the gate is deeper:
     paste `ice_preplay` — `sctp_rig_opened`/`sctp_msgs_in`/
     `sctp_first_*` name what the rig sent us (its own channels? a
     control message? silence?) and stage-3 senders (the KAT'd
     NACK-v2/IDR/RFI + an RTCP RR on sid 14) become the next round.
   * **No sctp_up** (sctp_fail / stuck connecting) → the rig's DTLS-app
     behaviour differs from the loopback peer: paste everything incl.
     `app_first` (the round-XXXIII inject-level capture names the
     post-ready record: ContentType 20/21/22/23).
   * Optional knobs: `--session-play-wait=N` / `--session-play-settle=N`
     (defaults 2500/250). A second invocation after a failed PLAY
     re-attaches to the live session (run 13-proven) — a cheap second
     datum. `cloud_status 11` = SESSION_LIMIT (end it in the web UI).
3. **R3 — A/B/C shipped (rounds XXX/XXXI/XXXIII; docs/DTLS.md +
   docs/SCTP.md):** remaining R3-C scope = SENDING on the channels
   (the kn_rx NACK-v2/IDR/RFI encoders over sid 14, RTCP RRs, the
   input channels — stage 3 proper, unlocked by run 15's evidence) →
   R3-D WS control pings (NvstControlPing :260-290/:844-930).
   Presentation wiring for --session frames (the G1-proven presenter)
   follows once real frames flow.
4. Stage 3+ per BOOTSTRAP §12; R22's full 31-field context + R23 RESUME
   at stage 4/5.
5. **Presentation wiring for the live path**: `--decode` frames →
   `kn_d3d_vp_*` presenter (the G1-proven chain) = moving video from the
   socket path on screen; the D20 FPS overlay defaults OFF there. Small
   and independent of WSS/RTSP — can ship in the same round as step 2 or
   be rehearsed against `send_rx_capture.py` first.
6. Control senders wiring (stage 3 with SCTP): the NACK-v2/IDR/RFI
   encoders are DONE and KAT'd; `kn_rx_video_take_keyframe_request` now
   also carries the decode-queue-overflow requests (D21). SRTCP-GCM RR
   when !rtcp_on_sctp.


## Repo layout (this workspace → user machine)

```
karnynow/
├── AGENT-HANDOFF.md            ← snapshot-style handoff (round VI research view)
├── PROJECT-STATE.md            ← THIS file (living state; read Bootstrapping first)
├── INTEGRATION.md              ← file-by-file integration notes for the deltas
├── docs/
│   ├── LOGGING.md              ← knlog/knring conformance + API guide
│   ├── FEC.md                  ← knfec provenance (nvst.rs line refs) + semantics
│   ├── SRTP.md                 ← knsrtp provenance + phase-2 checklist
│   ├── D3D.md                  ← kn_d3d provenance (G0.2/T4 refs) + VEH guard
│   │                              + TARGET EVIDENCE (round 5: pitch 1536!)
│   ├── GS.md                   ← kn_gs provenance + vector pipeline + stage-2 TODOs
│   ├── RX.md                   ← kn_rx receive pipeline: provenance table,
│   │                              §Parity construction, deviations, landmines,
│   │                              replay-capture usage (ROUND XVI)
│   ├── SOCK.md                 ← kn_sock + --capture/--decode + persistent
│   │                              decoder session + T13 (ROUND XVII record)
│   ├── RTSP.md                 ← WSS+RTSP+CloudMatch wire spec (line refs
│   │                              @ a49b124) + §8/§9 implementation status
│   │                              + deviations (ROUND XIX research, XX/XXI impl)
│   ├── AUTH.md                 ← OAuth device-flow research (ROUND XXI;
│   │                              login implementation = stage 4)
│   ├── AUDIO.md                ← kn_audio (WASAPI) provenance + landmines
│   ├── NET.md                  ← kn_net phase-C probes (schannel) + verdict table
│   ├── LOOPBACK.md             ← stage-1 video path design + G1 gates
│   ├── analysis/               ← measurement analyses (G0.2 closed here)
│   └── research/               ← BOOTSTRAP.md + research HANDOFF (copies)
├── streamer/                   ← knvstd (C11)
│   ├── include/kn/             ← knlog knring knfec knsrtp kn_aes kn_sha1
│   │                              kn_diag kn_selftest kn_decode kn_d3d kn_gs
│   │                              kn_perf kn_audio kn_net kn_rx kn_sock
│   │                              kn_ws kn_rtsp kn_json kn_http kn_cm
│   ├── src/                    ← implementations + GENERATED: knfec_tables.h,
│   │   │                          knfec_kat_vectors.h, knsrtp_kat_vectors.h,
│   │   │                          kn_gs_kat_vectors.h, kn_rx_kat_vectors.h,
│   │   └                          kn_synthetic_clip.h + kn_rx_kat_runner.h
│   │                              (shared battery: test + rx.vectors)
│   ├── assets/synthetic.264    ← decode.synthetic KAT clip (embedded via tools/)
│   ├── assets/synthetic720.264 ← loopback/G1 source: 720p High@5.1, NOT embedded
│   └── tests/                  ← portable unit tests (run on Linux AND Win7):
│                                  knlog knring knfec knsrtp kndiag knselftest
│                                  kngs knaudio knrx knsock knws knrtsp kncm
├── python/                     ← TUI (stage 4); requirements.txt RESOLVED via T8
├── tests/                      ← target-machine harnesses T1-T13
│                                 (T9 = old build gate; T10 = build+selftests;
│                                  T11 = the G1-video loopback gate;
│                                  T12 = network-only gate;
│                                  T13 = stage-2 UDP E2E gate, loopback-only)
├── tools/                      ← build/verify scripts + KAT generators
│   (build_native_tests.sh, cross_check.sh, check_jsonl.py, check_zip.py,
│    fec_reference.py, srtp_reference.py, gs_reference.py, rx_reference.py,
│    make_synthetic_clip.py, make_rx_capture.py, send_rx_capture.py,
│    fake_rig.py, prepare_mbedtls_sandbox.sh,
│    fetch_ffmpeg_headers_sandbox.py,
│    sandbox_decode_{check,edge}.c, sandbox_loopback_source.c,
│    sandbox_stream_decode.c, sandbox_session_e2e.c)
├── third_party/                ← GITIGNORED: tiny-aes-c, crypto-algorithms-sha1
│                                  (+ ffmpeg-headers in the agent sandbox only)
└── CMakeLists.txt              ← Windows build (w64devkit: cmake + ninja);
                                   KN_FFMPEG_PREFIX option, git/flags → build.json
```

Maps onto the user's repo `C:\Users\KarnyJohnny\Desktop\NewKarnyNow\`.

## Working agreements (carried over, still binding)

- **CONTINUITY RULE (user instruction, 2026-10-23):** at the END OF EVERY
  TASK/ROUND — before presenting results — update THIS file (Current status,
  Risk register, Next steps, Session log) so that a FRESH agent session can
  pick up the work immediately from the repo alone, with no chat history.
  This is the recovery path against chat overflow, network failures and
  session loss. The repo is the source of truth; the user keeps GitHub
  current (the agent cannot push — deliver files in the workspace + a
  change list).
- Measure first, then code; every stage has a gate; a failed gate is information.
- **Compile gate is two-tier** (R26 revised): sandbox `build_native_tests.sh` +
  `cross_check.sh` (fast, catches most things) and the TARGET `tests/T10_build_and_test.py`
  (w64devkit GCC 16.2 — authoritative; it caught `-Wstring-compare` and two
  `-Wformat-truncation` classes the sandbox GCC 12 missed). Zero warnings on both.
- Compile before handing over (when a compiler exists: `tools/build_native_tests.sh`,
  `tools/cross_check.sh`); zero warnings with `-Wall -Wextra`.
- Keep artefacts small: every big JSON gets a `*_skrot`/`*_summary` sibling.
- Isolate anything that can crash (separate process / SEH marker before risky calls).
- Name unknowns explicitly (`NOT VERIFIED` / `UNVERIFIED` labels in code).
- Never disable TLS verification; never bypass login/entitlements/queue; no network in
  MOCK mode; secrets logged as lengths/presence only (whitelist redaction at the logging
  boundary).

## Session log (implementation)

### 2026-10-23 · impl round XV (T10 network run)
The target T10 run completed with a clean build and a green self-test set: configure/build rc=0, zero warnings, 8 unit suites passed, `knvstd --selftest=all` exit 0, `d3d.present` milestone 0 passed, and the diagnostic bundle was generated. The sole environment-level finding was `net.tls` and `tls.chain` failing on Win7/Schannel across all tested configs (`0x80090302` / `0x80090326`, alert `2/70`), while `net.dns` and `net.tcp443` remained OK. Interpretation: stage-2 network/input issue (Win7 TLS stack, service pack / cipher policy / cert chain), not a build regression.

### 2026-10-21 · impl round I
Received T4 v4 results (`T4_wynik.json` complete this time — writer fix proven) and the
user's answers to the five open questions. Analysed the run: G0.2 **closed**, D7
**settled** (NV12 StretchRect primary; SYSTEMMEM NV12 fill isolated as the crashing call
and excluded; offscreen conversion targets refused with `D3DERR_INVALIDCALL`; harness
`srf_*` mask bug documented), R2/R2b/R17 closed, R9 mitigated, R25 opened (DEFAULT NV12
fill → stage-1 milestone 0). Recorded D14–D17 from the user's answers. Built the
implementation workspace; wrote the C logging subsystem core + ring buffers + unit tests
(BOOTSTRAP §7, D13); drafted cp38 `requirements.txt` and the T8 dependency probe for the
user. → `docs/analysis/G0.2-T4v4-RESULTS.md`

### 2026-10-21 · impl round II
User ran the Win7 build (cmake 4.4.3 + ninja, w64devkit GCC 16.2.0):
**test_knlog 105 checks PASS, test_knring 34 checks PASS on the target** —
real Win7 file/rename/flush semantics verified. GCC 16 caught one warning the
sandbox GCC 12 had missed (`-Wstring-compare` in `kn_redact_key_is_secret`
when inlined into `kn_log_on_failure` with short literal keys) → fixed with a
`strstr(norm,"srtp")==norm` prefix test (fix awaits T9 verification).
**T8 resolved R15**: all 14 cp38 packages downloadable rc=0; pins recorded in
`python/requirements.txt`; wheels cached in `tests\t8_wheels\` on the target.
**Sandbox lost its toolchain** (apt installs now fail with infrastructure
errors) → new binding agreement **R26**: no toolchain installs in the
sandbox; the target machine is the compile authority via
`tests/T9_build_and_test.py` (new: cmake/ninja build with direct-gcc fallback,
runs all three test binaries + JSONL validation, emits `T9_skrot.txt`).
**FEC half of BOOTSTRAP §13.3 delivered (pending compile)**: fetched full
`nvst.rs` @ a49b124 into `artifacts/nvst-extract/`, extracted the exact GF/
Cauchy/reconstruction rules with line provenance (`docs/FEC.md`), wrote
`tools/fec_reference.py` (independent Python port; self-check reproduces the
reference's own test `nvst.rs:13239`), generated `knfec_tables.h` +
`knfec_kat_vectors.h`, implemented `streamer/src/knfec.c` (zero-allocation
encoder + Gauss-Jordan reconstruction) and `streamer/tests/test_knfec.c`
(tables-vs-definition sweep of all 65 536 products, golden Cauchy matrices,
nvst.rs:13239 port, zero-padding semantics, wide-block encode KAT, 200
deterministic property round-trips, error paths). Created `AGENT-HANDOFF.md`
(recovery document for a future agent, with the file list to attach).
→ `docs/FEC.md`, `tests/T9_build_and_test.py`, `AGENT-HANDOFF.md`

### 2026-10-21 · impl round III
Resend of the "no sandbox toolchain" instruction → scanned the workspace (all
round-II artefacts intact) and continued the queue with the SRTP KDF/IV half
of BOOTSTRAP §13.3. Fetched RFC 3711 text and read the vector appendices from
source (B.2 = AES-CM, B.3 = KDF — the common "B.1 is the KDF" assumption from
memory was WRONG; fetching caught it). Extracted nvst.rs crypto facts:
labels 0x00/0x01/0x02 SRTP + 0x03/0x05 SRTCP (88-95), profile set incl. GCM_8
and AES-256 variants (301-330), KDF/IV layouts (2873-2939). Vendored
tiny-AES-c (Unlicense, sha256 recorded, PROVENANCE.md) behind a `kn_aes`
wrapper with a compile-time size assertion; wrote `knsrtp` (KDF + AES-CM
keystream + three IV builders, zero allocation) and `test_knsrtp` (~40 checks
against FIPS-197/SP800-38A/RFC-3711-B.2/B.3 goldens + cross-impl vectors).
`tools/srtp_reference.py` self-proves its pure-Python AES against all RFC
constants before emitting vectors — and immediately earned its keep: the first
S-box draft degenerated because **AES's 0x11B is not primitive (ord(0x02)=51;
the S-box needs generator 0x03, unlike FEC's 0x11D where 0x02 works)** —
recorded as a landmine. Wired test_knsrtp into CMakeLists/T9/shell gates;
docs/SRTP.md written (incl. the phase-2 checklist: HMAC-SHA1 + be32(roc),
replay window, SRTCP, mbedTLS decision). knfec + knsrtp now share one gate:
T9 on the target.
→ `docs/SRTP.md`, `streamer/src/{kn_aes.c,knsrtp.c}`, `streamer/tests/{test_knsrtp.c,knsrtp_kat_vectors.h}`, `third_party/tiny-aes-c/`, `tools/srtp_reference.py`

### 2026-10-21 · impl round IV
First T9 run on the target. **cmake build rc=0, warnings=0 under GCC 16.2**
(the `-Wstring-compare` fix is confirmed). Manual runs (T9's exe launch hit a
Windows trap, below): **test_knlog 105/105 PASS, test_knring 34/34 PASS,
test_knsrtp 37/37 PASS** — SRTP phase 1 reproduces FIPS-197/SP800-38A/RFC
3711 B.2 (incl. the FEFF→FF00 counter carry) and B.3 (incl. the 94-octet
auth key) byte-for-byte with the vendored tiny-AES-c on the production
toolchain. **test_knfec 122/125**: property round-trips 200/200 and every
golden vector PASS; the 3 failures were a test-side state leak in
`test_error_paths` (NULL `shards[2]` carried between subcases → `-2` fired
before the tested behaviour). Fixed: each subcase now sets full explicit
state. Two harness bugs fixed: (1) T9 launched test exes with relative paths
while passing `cwd` to Popen — on Windows the executable path resolves
against the CALLER's cwd, not the `cwd` argument (documented Python
behaviour) → absolute paths now, comment added; (2) same trap noted in
`run_tests` for future agents. Stage-1 crypto+FEC cores are now target-
verified; remaining gate: clean T9 re-run.
→ `streamer/tests/test_knfec.c` (fix), `tests/T9_build_and_test.py` (fix)

### 2026-10-21 · impl round V
**First T9 run: VERDICT ALL PASS.** cmake build rc=0 **warnings=0** (GCC 16.2),
test_knlog 105/105, test_knring 34/34, test_knfec **125/125** (error-path fix
confirmed; property 200/200), test_knsrtp 37/37, JSONL validation PASS. The
crypto/FEC/logging verification chapter is closed for phase 1.
Then implemented **SRTP phase 2** (from `nvst.rs`, read not remembered):
vendored Brad Conte SHA-1 (public domain, sha256 recorded) behind `kn_sha1`
with RFC 3174 KAT; HMAC-SHA1 RFC 2104 glue with RFC 2202 cases 1-7 KAT;
`kn_srtp_auth_tag`/`verify_tag` (be32(roc) suffix, constant-time compare,
authenticate-before-decrypt); `protect/unprotect_cm_hmac` in-place (ROC
derived from packet_index internally — no inconsistent-pair footgun);
`kn_srtp_replay` window 1:1 from nvst.rs:3121-3211 (guess/check/commit,
2048 packets, 32xu64 bitmask, defensive no-op instead of the reference's
panic). Evidence that **SRTCP-CM is not needed** (nvst SRTCP is GCM-only;
RR rides DTLS-encrypted `rtcp1` SCTP). `srtp_reference.py` extended: p2
self-check (RFC3174 x4 via hashlib oracle, RFC2202 x7, protect goldens with
B.3-derived session material, 96-op replay script vs a line-by-line python
port of ReplayWindow) — the generator caught my TEST4 transcription error
(TEST4a+TEST4b = 64-byte base, not 32) before emission, twice proving the
"vectors self-prove first" rule. test_knsrtp extended (~35 new checks;
~72 total). CMake/T9/shell gates updated for the new sources. Everything
awaits the next T9 run.
-> `streamer/src/kn_sha1.c`, `streamer/src/knsrtp.c` (phase 2),
   `streamer/tests/test_knsrtp.c`, `third_party/crypto-algorithms-sha1/`,
   `tools/srtp_reference.py`, `docs/SRTP.md`

### 2026-10-21/22 · impl round VI (second agent chat — workspace handoff via files)
A second agent chat received the file handoff (BOOTSTRAP.md, research HANDOFF.md,
analiza/04+05, all streamer sources/tests, tools, T9_wynik.txt) and rebuilt the
full workspace from it: cloned OpenNOW @ a49b124, **verified every cited nvst.rs
line reference against the live tree** (derive_aes_cm_key@2873, srtp_aes_cm_iv@2902,
ReplayWindow@3121, gf256_tables@3717, nvst_cauchy_coefficient@3785 — all exact),
fetched third_party from upstream, reproduced **T9-equivalent ALL PASS on the
Linux host** (341 checks) and cross-check zero-warning x64+i686, and confirmed
the KAT generators reproduce the checked-in vectors **byte-identically**.
Then implemented the AGENT-HANDOFF priority — **§7.8 diagnostic bundle**:
`kn_diag` (store-only ZIP, sequential write / never fseek over own output per
§7.12, `summary.json` as the FIRST entry with per-subsystem verdicts, 8 MiB cap
enforced continuously — SECONDARY generations dropped whole first, PRIMARY
tail-kept newest-with-marker, CORE untouchable; auto-trigger through the
existing `kn_log_failure_cb_register` hook with recursion guard; auto
build.json/state.json; selftest-provider hook for §7.7 step 4) and the
**§7.9 self-test registry** `kn_selftest` (12 names; fec.vectors 88 KAT checks
+ srtp.vectors 52+96-op replay FULLY implemented; clock.resolution +
console.host implemented for Win32; 8 honest phase-B/C stubs that can never
read as passing; relevance mapping op/code → tests) plus the **knvstd
skeleton** (`--selftest=<name|all>`, `--diagnostics`, `--version`,
`--list-selftests`; JSON-lines stdout; exit codes 0/1/2/3). Test tooling:
`test_kndiag` (109 checks, embeds an independent store-only ZIP reader with
CRC verification), `test_knselftest`, `tools/check_zip.py` (python zipfile as
second opinion), `tests/T10_build_and_test.py` (target harness: T9 scope +
live Win32 self-tests + bundle validation + PE 6.01 check). KAT vectors moved
`streamer/tests/` → `streamer/src/` (they are production assets now — the
shipped binary runs them; generators updated; regeneration byte-identical).
Host suites: 452 checks, 0 failures. → this section, INTEGRATION.md rev 1.

### 2026-10-22 · impl round VII (T10 rounds 1-2 on target + FFmpeg integration)
**T10 round 1 on the target: 30 checks, 4 failed — zero functional bugs.**
All four were environment/expectation issues, fixed the same day: (1) GCC 16.2
flagged two `-Wformat-truncation` in UTC formatters GCC 12 accepted → all
format inputs modulo-bounded, clean at -O0..-O3 (R28); (2)+(4) `"0x601"` vs
the stringified `"0x0601"` in test+harness → int-parsed; (3) test_knselftest
hard-coded host expectations while clock.resolution/console.host really ran
on Win7 → platform-aware. **First real target evidence for §7.9**: Sleep(1)
1.25→1.00 ms after timeBeginPeriod(1); ConEmuHk present (build 230724 — D1
confirmed from inside a shipped binary); fec/srtp vectors pass in the GCC 16.2
binary; bundle written+validated on Win7 (4863 B, PE 6.01).
**FFmpeg answered** (open question from round VI): the user built it himself —
git master N-127939-gbf0e831606, w64devkit GCC 16.2, `--enable-shared
--disable-static --enable-gpl --disable-autodetect`; ffmpeg.exe runs on Win7.
Sandbox pulled the headers at the EXACT commit (avcodec 63/avutil 61/swscale
10.2.100 match) and implemented **`kn_decode`**: Annex-B → av_parser →
libavcodec h264 threads=1 (D4) → swscale yuv420p→NV12 (no scaling — D6;
NV12 for the locked DEFAULT surface, never UpdateSurface — R2) → per-frame
callback; stats frames/fps/cpu%-of-core/worst-frame; 64 MiB input cap;
av_log mapped INTO knlog media at DEBUG/TRACE (amplification surfaces it
exactly when needed; no fsync-storm on corrupt-frame spam). Compile-verified
against the real headers with -Werror on both arches.
**T10 round 2 on the target: link FAILED** — `cannot find -lavcodec … have you
installed the static version?` Root cause reproduced 1:1 in the sandbox: the
canonical F1 `-static` makes ld ignore `.dll.a` import libraries, and the
FFmpeg build was shared-only. Also: clip generation failed (no libx264 in a
`--disable-autodetect` build → `-preset` unrecognized), and GCC 16 caught an
unused-but-set `ok_count` in the Win32 test branch. **Fixes (D18):** FFmpeg
goes static (user rebuilds — exact recipe in Next steps step 1 and enforced by
a CMake configure-time FATAL_ERROR with the recipe when `libavcodec.a` is
missing); decode.synthetic gets a **build-time embedded clip**
(`streamer/assets/synthetic.264`, 60×320×240 yuv420p 42 KB, →
`kn_synthetic_clip.h`, hermetic — no encoder needed on target); `kn_decode`
grew `kn_decode_h264_mem` (file API refactored over a common padded-buffer
core); T10 hardened (PE-check guard, informational DLL report, embedded-clip
expectations). Toolchain decision recorded (D19: stay on w64devkit; Cygwin
disqualified; WinLibs-MSVCRT = designated fallback only). Sandbox suites after
the fixes: native 452 checks PASS, cross -Werror PASS (incl. kn_decode+
kn_selftest vs real FFmpeg headers), ctest 8/8, T10 dry-run ALL PASS (29).
Awaiting: user FFmpeg static rebuild + T10 round 3.
→ `streamer/{include/kn,src}/kn_decode.*`, `streamer/src/kn_synthetic_clip.h`,
   `streamer/assets/synthetic.264`, `tools/make_synthetic_clip.py`,
   `CMakeLists.txt` (KN_FFMPEG_PREFIX + static guard + DLL copy),
   `tests/T10_build_and_test.py` rev 3, fixes in kn_diag/kn_selftest/tests

### 2026-10-23 · impl round VIII (T10 round 3 diagnosis — static FFmpeg closure)
User rebuilt FFmpeg static per D18 (confirmed: `--enable-static
--disable-shared` in `ffmpeg.exe -version`, same commit N-127939-gbf0e831606)
and moved the dev prefix to `C:\Users\KarnyJohnny\Desktop\ffmpeg\ffmpeg-win7-dev`
(bin/include/lib at top level — no `usr\local` anymore). **T10 round 3: cmake
configure PASS (prefix + `libavcodec.a` guard both worked), ninja build FAILED
at the knvstd/test_knselftest link**: `undefined reference to swr_*` (DSD/DST/
Opus/WavPack decoders inside `libavcodec.a` call libswresample) and
`BCryptGenRandom/BCryptOpen/BCryptClose` (avutil's `av_random_seed` on
Windows). A shared build hides this (each DLL carries its own deps); a static
link needs the full closure, single-pass, in dependency order. Fixed in
CMakeLists: `KN_FFMPEG_LIBS = avcodec swscale swresample avutil` + `bcrypt`
on WIN32 (bcrypt.dll exists since Vista — Win7-safe). T10 now probes both
prefix layouts (current top-level, legacy `usr\local`) with `KN_FFMPEG_PREFIX`
override. Configure-time guards re-verified in the sandbox with a fake prefix
(bad include path → readable FATAL; missing `.a` → full rebuild recipe).
Sandbox regression after the fix: native 452 checks PASS, cross `-Werror`
PASS x64+i686 (incl. kn_decode vs the real FFmpeg 63 headers), ctest 8/8,
T10 dry-run ALL PASS (29). The 6 unit suites + kncore were never affected
(only the two FFmpeg-linking executables failed to build, everything else ran).
Awaiting: T10 round 4 on the target — the first run of `decode.synthetic`
with real fps/CPU numbers on the E5500.
→ `CMakeLists.txt` (KN_FFMPEG_LIBS closure), `tests/T10_build_and_test.py`
  (prefix probing)

### 2026-10-23 · impl round IX (T10 round 4: 31/32 — decode flush fix + kn_d3d phase-B probes)
**T10 round 4 on the target: 31/32 PASS.** The D18 static closure worked
end-to-end: configure found the top-level prefix, `libavcodec.a` guard
silent, link OK, zero warnings under GCC 16.2, no DLLs, all six unit suites
green on the target for the first time with SRTP phase 2 included (knsrtp
77 checks), bundle 5060 B fully validated, PE 6.01, build.json extras
record the FFmpeg prefix. `decode.synthetic` EXECUTED (first ever real run
of kn_decode): rc=0, failed=0, fps≈3687, cpu 93.8% of one core over the
16 ms run, decode_ms_max=1 — but **frames=59 of 60** → the one FAIL
(cascading into `--selftest=all` rc=1).
**Root cause (measured, not guessed):** the clip itself is clean (ffprobe
`nb_read_frames=60`); the sandbox reproduced 59/60 EXACTLY against bookworm
FFmpeg 5.1 (same as the target's 63 — version-independent): `kd_run`
flushed the DECODER at EOF but never the PARSER, and `av_parser_parse2`
withholds the last access unit until the NEXT start code — which never
comes at end-of-input. Fix: NULL-input parser flush before the decoder
drain, both routed through a new shared `kd_send_drain` helper (identical
accounting on all three paths). Post-fix sandbox evidence: **60/60**, and
the contracts still hold — abort at frame 10 → rc=-4 with exactly 10
delivered, truncated 30 000 B input → rc=0, 40 frames, 0 failed, no crash.
**Phase B remainder delivered — `kn_d3d` module** (docs/D3D.md): the D7
session-start capability probe (`d3d.formats`: PCI id, driver version,
current mode + 60 Hz-at-res count (R9 evidence in every bundle), the 7-
format CheckDeviceFormat row, 4-pair conversion matrix, corrected
filter-caps masks, HWVP/SWVP CreateDevice behaviour) and **R25
milestone 0** (`d3d.present`: production-shaped 1280×720 NV12 DEFAULT →
lock → fill EVERY byte (pitch honoured) → full read-back verify →
StretchRect(LINEAR, letterbox (0,40,1280,760) computed) → Present ×30,
QPC-timed fill vs stretch+present separately, TCL after). Every call
pattern copied from T4 v4 — and the pitfall rule immediately earned its
keep: from-memory `CheckDeviceFormat` missed the AdapterFormat parameter
(macro-arity error under -Werror) and the D3DFMT enum values were checked
against d3d9types.h instead of recalled. **Crash guard: VEH + longjmp +
D7 permanent-degrade latch** — mingw GCC has NO `__try/__except` (verified
on both arches); the handler catches AV/IN_PAGE_ERROR only while armed,
records code + fault write-address, and the latch makes every later
in-process D3D call impossible (Release included). Registered landmines:
R29 (guard never executed on real D3D9 — round 5 is the first run) and
R30 (clock.resolution round-4 variance 15.50→2.75 ms degraded vs round 1's
1.25→1.00 — one loaded sample, thresholds stand, D9 pacing unaffected).
**knvstd hardening:** `--selftest=all` now runs test-by-test, logging
`selftest_begin` + each result IMMEDIATELY (INFO soft-flush §7.12) — an
uncatchable fault mid-run leaves the exact test name in diag.log (T4/R2b
rule applied to the runner). Relevance mapping extended:
video/d3d-flavoured failures re-run decode + both d3d probes into the
bundle (latch-safe: a tripped probe returns -3 instantly, no crash loop).
**T10 rev 4:** standalone `--selftest=d3d.present` FIRST (milestone-0
process isolation; a hard crash there cannot eat the rest of the
evidence), d3d verdict checks (`failed` fails T10 BY DESIGN — stage 1 must
not start on a broken video path), `decode.synthetic` must be **ok**
(60/60), stub count 5. Sandbox gates after all edits: native suites PASS
(test_knselftest 94 checks), cross `-Werror` PASS x64+i686 (kn_d3d +
kn_decode vs real FFmpeg 63 headers; PE imports d3d9/kernel32/msvcrt/
user32/winmm — all Win7-built-in), ctest 8/8, T10 POSIX dry-run ALL PASS
(31). Awaiting: T10 round 5 — first-ever execution of the d3d probes.
→ `streamer/{include/kn,src}/kn_d3d.*` (new), `docs/D3D.md` (new),
   `streamer/src/kn_decode.c` (parser flush), `streamer/src/kn_selftest.c`
   (d3d tests + relevance), `streamer/src/knvstd_main.c` (immediate
   logging), `streamer/tests/test_knselftest.c`, `CMakeLists.txt` +
   `tools/*.sh` (kn_d3d wiring), `tests/T10_build_and_test.py` rev 4,
   INTEGRATION.md rev 4, `tools/sandbox_decode_{check,edge}.c` (the
   sandbox repro/contract harnesses — Linux + distro FFmpeg only, not part
   of any gate), `artifacts/T10-round4/` (raw round-4 evidence, gitignored)

### 2026-10-23 · impl round X (T10 round 5: ALL PASS 38/38 — R25 CLOSED; then gs.parse)
**T10 round 5 on the target: VERDICT ALL PASS (38 checks, 0 failed).**
Everything round IX shipped is now target-verified: the parser-flush fix
(`decode.synthetic` **ok 60/60** at 3157.9 fps, cpu 84% of one core, worst
frame 1 ms — round 4's 59/60 is gone), both d3d probes (**d3d.formats ok**
10 checks; **d3d.present ok** 11 checks — standalone milestone-0 run AND
inside `--selftest=all`, 30/30 iterations, fill read-back mismatches=0,
every HRESULT S_OK, VEH armed but never fired, latch false), the knvstd
crash-survivor logging (begin markers visible in the committed diag.log),
T10 rev-4 checks (38 total), test_knselftest 97 on Win32 (94 on host —
platform deltas expected). **R25 CLOSED. R29 happy path verified (catch
path stays UNVERIFIED by design). Phase B complete except gs.parse.**
New hard facts (full table in docs/D3D.md "Target evidence"):
(1) **pitch 1536 for 1280×720 NV12 DEFAULT** — NOT width (T4 saw pitch==width
at 960×540/1280×800; alignment depends on dimensions unpredictably) →
stage-1 loopback copies NV12 **per row** (kn_decode's 32-aligned stride vs
surface pitch), never one memcpy; the milestone passed BECAUSE it honoured
the returned pitch. (2) **first-touch fill ≈ 28 ms once** (mean 2.13) —
stolen UMA memory page-fault cost → pre-fault the surface during setup.
(3) present mean **0.28–0.43 ms** for the 1:1 720p letterbox (G0.2's 3.0 ms
was a scaled stretch) — GPU side has ~10× headroom vs the 33.3 ms budget.
(4) **G0.2 §2.1 CORRECTION: SWVP CreateDevice actually returns S_OK** when
attempted sequentially after releasing the HWVP device — T4 never really
attempted it after HWVP success (its E_FAIL was the init value). Production
rule unchanged (HWVP, D7). (5) clock.resolution degraded twice in a row
(15.6→2.75/2.88 ms, wall tick pinned 15600 even after timeBeginPeriod) —
R30 reclassified as the machine's stable state: ~2.9 ms is the realistic
Sleep(1) floor; timestamps quantise at 15.6 ms; D9 pacing unaffected.
**New binding working agreement (user instruction): the CONTINUITY RULE** —
every round must leave this file updated as a complete handoff BEFORE
presenting results (recovery path against chat overflow/network loss).
Workspace synced to GitHub `4c47794` (user committed round IX verbatim +
round-5 t10logs evidence); raw round-5 files archived in
`artifacts/T10-round5/`, nvst.rs @ a49b124 re-fetched to
`artifacts/nvst-extract/` (sha256 f52fc09f…, 14 695 lines — matches the
round-VI verification).
Then: **gs.parse + kn_gs DELIVERED (the stage-2 seed).** New module
`kn_gs` (pure, zero-allocation): RTP header/CSRC/extension/padding parse
with the reference's exact error precedence, the 16-byte LE GS body
(pitfalls #1/#2 encoded as first-class fields), Annex-B payload location
(68-byte search window, windows(3) semantics preserved), keyframe
detection, trailing-AUD strip (incl. the zero-MERGE case: a body ending in
00 fuses with a following 00 00 01 into a 4-byte code — vector pinned),
FEC block layout (div_ceil parity, all five rejection rules), stream-index
continuity (2^24 wrap), and kn_ring_dump hexdump line parsing (strict
single-space format, cap-after-validation ordering). Every fact re-verified
against nvst.rs @ a49b124 THIS round (RtpHeader::parse 2456-2510,
NvVideoPacket 3228-3257, payload 3481-3497, start-code 3525-3534, keyframe
3500-3523, AUD 3458-3479, FecPacketLayout 3677-3701, flags 4089,
continuity 4813-4824) — docs/GS.md carries the full table. Generator
`tools/gs_reference.py` self-proved before emitting (hand goldens,
LE-vs-BE and ext[11] discriminators, 700 round-trips) and caught 10 of its
own bugs first (w32be profile bug, six wrong eof expectations, the AUD
merge case, an out-of-range FEC shard, two no-op truncations). Emitted
battery: **78 vectors**; host suite `test_kngs` (SEVENTH suite): **105
checks** incl. C-side property round-trips (a second independent encoder).
`gs.parse` self-test: battery from the shipped binary (~1 ms, portable —
host AND target) + optional real capture (`KN_GS_CAPTURE` env or
./gs_capture.hex; parsed-lines/RTP/GS/SOF/EOF/FEC counters in evidence;
absence reported honestly). Relevance mapping: video failures now also
re-run gs.parse; landmine recorded — bare "rtp" keyword substring-matches
"srtp" (caught by the host suite before shipping). Stubs left: 4. Sandbox
gates after the delta: native PASS (7 suites: 105+34+125+77+109+97+105 =
652 checks), cross -Werror PASS x64+i686 (-O0..-O3 clean for kn_gs),
ctest 9/9, T10 POSIX dry-run ALL PASS (33). Awaiting: T10 round 6 (target
run of the gs battery + the new suite).
→ `streamer/{include/kn,src}/kn_gs.*` (new), `streamer/src/kn_gs_kat_vectors.h`
  (generated, new), `tools/gs_reference.py` (new), `streamer/tests/test_kngs.c`
  (new), `docs/GS.md` (new), `streamer/src/kn_selftest.c` (gs.parse +
  relevance), `streamer/include/kn/kn_selftest.h`, `streamer/tests/test_knselftest.c`,
  `CMakeLists.txt` + `tools/{build_native_tests,cross_check}.sh` (7th suite),
  `tests/T10_build_and_test.py` (kngs + gs.parse checks, min-stub 4),
  INTEGRATION.md rev 5, docs/D3D.md (target-evidence section)

### 2026-10-23 · impl round XI (T10 round 6: 39/40 — quick fix: R28 third incident)
**T10 round 6 on the target: 39/40 PASS.** Everything round X shipped is
target-verified: `test_kngs` PASS on Win7 (first run), **`gs.parse` ok with
78 checks in the shipped binary** (0 ms), d3d probes ok again (present
mean 0.34 ms, pitch 1536 reproduced), decode 60/60 at 3529 fps, 4 stubs
skipped, bundle 12 393 B validated, clock.resolution stable-degraded
(2.88 ms — third consecutive run, R30 pattern holds).
**The single FAIL was the zero-warning gate**: GCC 16.2 flagged
`-Wformat-truncation` in the NEW `st_gs_parse` capnote snprintf — the
`%u` directives are modulo-bounded (GCC even printed the [0,999999]
range) but `capnote[256]` was too small for the proven worst case
(107 literal + 128 path + 8×6 digits + NUL = 284). Sandbox GCC 12 at the
DEFAULT level did not flag it — **R28 third incident, this time in my own
round-X code; the two-tier gate worked exactly as designed** (target
caught what the sandbox missed; nothing functional was ever at risk).
Fix: `capnote[320]` + a comment with the worst-case arithmetic. Verified
BEYOND the target's bar: clean under `-Wformat-truncation=2` (paranoid
level) on host GCC 12 at -O0..-O3 AND mingw x64 -Werror — the two
remaining =2-only float directives (`%.1f fps` in decode/d3d reasons) are
level-2 over-approximations of lines that ran zero-warning under GCC 16 in
rounds 5-6; target gate is GCC 16 default level, so they stay untouched
(don't churn verified code on a stricter-than-production analysis).
Also clarified the T10 informational line that read like a warning:
"no FFmpeg DLLs next to knvstd.exe — static build (D18): knvstd links
lib/*.a directly, there is nothing to deploy (EXPECTED, not a warning)"
(the user's dev tree having no DLLs anywhere IS the correct static layout).
Sandbox gates after the fix: native 7 suites PASS (652 checks), cross
-Werror PASS x64+i686, T10 dry-run ALL PASS (33). Awaiting: T10 round 7 —
expected ALL PASS 40/40, then stage 1 `--loopback` is the next real work.
→ `streamer/src/kn_selftest.c` (capnote[320]), `tests/T10_build_and_test.py`
  (message wording), this file, INTEGRATION.md rev 5b

### 2026-10-23 · impl round XII (T10 round 7: ALL PASS 40/40 — then stage-1 video loopback delivered)
**T10 round 7 on the target: VERDICT ALL PASS (40 checks).** The capnote fix
closed the last warning (zero-warning gate green on GCC 16.2 again);
`test_kngs` passed on Win7 and `gs.parse` ran its 78-check battery from the
shipped binary; d3d.present reproduced milestone 0 a second time (30/30,
pitch 1536, present mean 0.30 ms max 5.08, fill mean 2.08); decode 60/60
at 3157.9 fps; clock.resolution stable-degraded (2.88 ms — fourth
consistent sample, R30 pattern). **The entire §7.9 phase-A+B verification
chapter is closed; 4 phase-C stubs remain (net/tls, gated on stage 2).**
Then implemented the **stage-1 video half** (docs/LOOPBACK.md is the design
record):
- **`kn_d3d_vp_*` presenter** grown on the probe code: open (T4-proven
  window/device parameters + D7 conv check + NV12 DEFAULT surface +
  PRE-FAULT fill&present absorbing the ~28 ms first touch), present_nv12
  (TCL check → lock → **per-row copy honouring pitch 1536 vs stride 1280**
  → StretchRect(LINEAR) → Present, timed copy vs stretch+present
  separately), pump (WM_CLOSE/ESC → graceful), refresh_hz (DPST detection),
  close. Every CPU-write section under the VEH guard; a caught exception
  latches the D7 permanent degrade and leaks D3D objects ON PURPOSE.
- **`kn_perf`** (§7.6 system counters): process/system CPU % OF ALL CORES
  (§6.1 convention) from GetProcessTimes/GetSystemTimes deltas, RSS+peak
  via K32GetProcessMemoryInfo (PSAPI_VERSION 2 — kernel32 only, the F1
  import set stays kernel32/user32/winmm/d3d9/msvcrt), handles, toolhelp
  thread count, timeGetDevCaps.
- **`knvstd --loopback`**: single-threaded by design (decode 211 fps vs
  30 fps pace — no queue needed; queue_video_depth honestly 0), pacing
  INSIDE the decode callback on absolute deadlines (t0+n·1e6/30, integer —
  errors never accumulate; Sleep(deadline−3ms)+QPC spin per R30), late
  grace 2 ms, source discovery explicit→cwd→exe-dir→embedded (embedded
  fallback reports budget_valid:false + WARN), clip loops until
  --loopback-seconds (default 600 = G1). One §7.6 perf event per 1000 ms
  (26 fields ≤ KN_LOG_MAX_FIELDS 32; stable schema with honest zeros for
  stage-2 groups). stdout: single-line summary JSON (fps_mean, late, cpu
  mean/max, rss first/last/peak, refresh initial/last, dpst, perf_lines,
  stopped_reason, fault/fault_addr, auto-bundle path). Failure →
  kn_log_on_failure("session.loopback", …) → §7.7 amplification +
  auto-bundle (kn_diag_install); the relevance mapping learned "loopback".
- **Asset** `streamer/assets/synthetic720.264`: 136 853 B, sha256
  870bdf29…, High@5.1 1280×720 yuv420p 120f GOP30 bf0 (verified at the
  SPS byte: profile_idc=100). **Landmine: x264 `-preset ultrafast`
  SILENTLY downgrades High→Constrained Baseline even with explicit
  `-profile:v high`** (CABAC/8x8dct off) — a Baseline clip would have made
  the G1 CPU gate lenient vs the High@5.1 budget it verifies; superfast
  keeps High. Not embedded (T11 passes the path; discovery + honest
  fallback in knvstd).
- **`tests/T11_loopback_g1.py`**: the G1-VIDEO gate — 10 criteria computed
  from the evidence (summary + perf.jsonl + logs + post-run bundle
  incl. "bundle CONTAINS logs/perf.jsonl"); --quick 30 s smoke; POSIX
  honest skip. WASAPI is round XIII; T11 then extends to full G1.
Sandbox gates: native 7 suites PASS (652 checks), cross -Werror PASS
x64+i686 (knvstd_main additionally clean under -Wformat-truncation=2 at
-O0/-O2/-O3 — R28 pre-check now standard), ctest 9/9, T10 dry-run ALL PASS
(33), T11 posix-skip verified, POSIX --loopback honest refusal rc=3, and
`tools/sandbox_loopback_source.c` proved the 720p source decodes 120/120
through kn_decode_h264_file (NV12, strides sane). Self-review caught two
bugs before the gates: frame-0 always-late (deadline==t0; fixed with the
2 ms grace) and the missing "loopback" relevance keyword. Awaiting: T10
round 8 + T11 quick + T11 full (G1-VIDEO).
→ `streamer/src/knvstd_main.c` (--loopback), `streamer/{include/kn,src}/kn_perf.*`
  (new), `streamer/{include/kn,src}/kn_d3d.*` (presenter),
  `streamer/assets/synthetic720.264` (new), `tests/T11_loopback_g1.py` (new),
  `docs/LOOPBACK.md` (new), `tools/sandbox_loopback_source.c` (new),
  `CMakeLists.txt` + `tools/{build_native_tests,cross_check}.sh` (kn_perf),
  `.gitignore` (T11 outputs), INTEGRATION.md rev 6

### 2026-10-23 · impl round XIII (T10 round 8 ALL PASS + **T11 G1-VIDEO PASS** → FPS overlay D20)
**T10 round 8 on the target: ALL PASS 40/40** (loopback delta builds
warning-free; all suites + self-tests green; clock.resolution reported
**ok** this run — Sleep(1) ≤ 2.6 ms; R30 variance swings both ways with
machine state, thresholds unchanged).
**T11 FULL G1-VIDEO run on the target: VERDICT G1-VIDEO PASS (20/20
checks).** The stage-1 video path held for the whole 600 s: fps_mean
**30.000**, frames_late **0** of 18 001 presented, 151 clip loops,
cpu_process_pct mean **13.0 %** of all cores (max single second 51.8 —
the clip-restart+IDR spikes), RSS first→last **20.9→18.8 MB** (peak 24.2,
drift check +2 855 kB ≤ +4 096), decode_ms mean 3.41/max 14.97,
present_ms mean 1.16 (copy 0.90 + stretch/present ~0.26), refresh 60 Hz
start=end, DPST 0, perf lines 601, logs zero WARN zero ERROR, post-run
bundle 719 kB containing logs/perf.jsonl. **Honest caveats recorded:**
(1) the synthetic testsrc2 clip is skip-heavy (x264: 94 % skip MBs) so
loopback CPU UNDERSTATES a real stream — the production budget reference
stays T1's 720p30 High@5.1 = 24.8 % (decode) / 28.9 % (+NV12), still far
inside the ≤45 % gate; real numbers arrive with stage 2 + G2. (2) The run
analysed 632 perf lines = 601 (full) + 31 (the earlier --quick smoke) —
t11logs accumulates across runs; **T11 now wipes t11logs at start** so the
record run is deterministic. G1 remains gated on WASAPI (round XIV);
G1-VIDEO is CLOSED.
**NEW user requirement → D20: low-overhead on-screen FPS counter with a
toggle key (F5)** — implemented this round in the loopback presenter:
5×7 bitmap font blitted directly into the locked NV12 surface during the
per-frame copy (no D3DX/GDI/lockable-backbuffer — zero new imports;
~1.5 k pixel writes per frame inside the VEH-guarded section; background
box + white glyphs, even coordinates only so the 2×2 UV subsampling maps
exactly at scale 2). The overlay shows the frames-presented-per-second of
the completed perf interval — the same number perf.jsonl carries. F5
toggles (wndproc), the orchestrator refreshes the text once per second
("FPS 30.0", plus " LATE n" when the interval dropped frames). Font+blit
live in a platform-NEUTRAL section of kn_d3d.c (kn_d3d_overlay_text_nv12)
so the sandbox can render ASCII proofs of every glyph (verified this round
before handover). Loopback default: overlay ON (debug surface); the
production streamer will default OFF with the same toggle (stage 2).
Awaiting: T10 round 9 + T11 (quick suffices — the G1-VIDEO verdict stands
until stage-2 changes land; the overlay is additive).
→ `streamer/{include/kn,src}/kn_d3d.*` (overlay font/blit + F5 + text API),
  `streamer/src/knvstd_main.c` (per-second overlay text),
  `tests/T11_loopback_g1.py` (t11logs wipe), `tools/sandbox_overlay_font.c`
  (ASCII glyph proof, sandbox-only), docs/LOOPBACK.md (overlay + keys),
  INTEGRATION.md rev 6b, this file (D20 + round XIII)

### 2026-10-23 · impl round XIV (T10 r9 40/40 + T11-overlay 20/20 on target → WASAPI synthetic PCM delivered: STAGE 1 CODE-COMPLETE)
**User-confirmed on the target:** T10 round 9 ALL PASS 40/40 (clock
resolution back to `ok` — Sleep(1) ≤ 2.6 ms; R30 swings with machine
state, thresholds stand) and T11 quick with the D20 overlay: G1-VIDEO
PASS 20/20 (fps_mean 30.001, late 0, CPU 13.9 %, RSS drift −1 952 kB,
31 perf lines from the wiped dir — the T11 cleanup works). User verdict:
"działa idealnie". GitHub @ `50f5291` (partial commits — the workspace
remains the delivery medium).
**Round XIV delta — `kn_audio` (WASAPI synthetic PCM), the last stage-1
deliverable** (BOOTSTRAP §12: "Plus WASAPI with synthetic PCM"; design +
provenance + landmines in docs/AUDIO.md): shared-mode event-driven render
client on the default endpoint at the device MIX format; dedicated refill
thread (WaitForSingleObject → GetCurrentPadding → GetBuffer → generate →
ReleaseBuffer); soft 440 Hz tone (~−18 dBFS) or silence; counters
events/frames_written/starved/wait_timeouts/buffer_errors/queue_ms.
Portable half (host-tested, `test_knaudio` 22 checks): the
WAVEFORMATEX(WSEXTENSIBLE) byte parser (float32/s16/s32; honest UNKNOWN
otherwise) + the phase-continuous generator (split calls byte-identical to
one big call — test-proven). Landmines hit and recorded: mingw
`audioclient.h` gates the C-style COM macros behind **`COBJMACROS`**
(d3d9.h does not); `INITGUID` in exactly one TU → **no -luuid**, the only
new PE import is ole32.dll (OS-built-in); **Start() BEFORE CreateThread**
(a thread waiting before Start burns a 200 ms timeout into the counter).
Integration: audio opens with the presenter in `--loopback` (open failure
never kills video — §7.7 amplification + `"audio":{"opened":false,…}` in
the summary; T11 gates full G1 on it), `queue_audio_ms` in the §7.6 perf
line is now REAL device padding, `--no-audio` for debugging, summary
carries the full audio block incl. expected_frames = rate × seconds_run.
**T11 upgraded to the FULL G1 gate**: +5 audio checks (opened, starved ≤5,
timeouts+buffer_errors ≤5, frames_written ≥95 % of realtime,
queue_audio_ms alive ≥80 % of perf lines); verdict renamed **G1 PASS**.
Gate story worth keeping: the s16 test initially read past the written
region into uninitialized stack — passed at -O1, FAILED at -O3+NDEBUG
under ctest; fixed with an exact-sized poison-filled buffer (different -O
levels catch different lies — R28's sibling, on the test side).
Sandbox gates after the delta: native **8 suites PASS (674 checks)**,
cross `-Werror` PASS x64+i686 (test_knaudio cross-links knlog for the
engine's logging), ctest **10/10**, knvstd+kn_audio clean under
`-Wformat-truncation=2` at -O0/-O2/-O3, T10 dry-run ALL PASS (33), T11
posix honest-skip, PE imports: d3d9/kernel32/msvcrt/**ole32**/user32/winmm
— all Win7-built-in. The WASAPI engine itself has NEVER EXECUTED (no audio
stack in the sandbox) — labelled as such everywhere; T11 on the target is
its first run. **STAGE 1 IS CODE-COMPLETE; the G1 verdict needs one full
T11 run.**
→ `streamer/{include/kn,src}/kn_audio.*` (new), `streamer/tests/test_knaudio.c`
  (new), `docs/AUDIO.md` (new), `streamer/src/knvstd_main.c` (audio wiring +
  --no-audio), `tests/T11_loopback_g1.py` (full-G1 checks), `CMakeLists.txt`
  + `tools/{build_native_tests,cross_check}.sh` (8th suite, ole32/m),
  docs/LOOPBACK.md (audio status), INTEGRATION.md rev 7, this file

### 2026-10-23 · impl round XV (G1 CLOSED on target → phase-C network probes delivered: registry 12/12, zero stubs)
**G1 CLOSED — the stage-1 gate passed on the target (T11 FULL, 20/20 +
T10 r10 40/40; user: "działa idealnie").** Numbers of record: video
fps_mean **30.000**, frames_late **2**/18 001 (0.011 %), CPU **13.3 %** of
all cores (max second 51.6 % = clip-restart+IDR spikes), RSS 22.5→19.8 MB
(peak 26.2, drift +2 607 kB ≤ +4 096), 601 perf lines (600/601 within
[27,33]), present_ms mean 1.17 (copy 0.91), decode_ms mean 3.52/max 16.34,
DPST 0, refresh 60→60, logs zero WARN/ERROR, bundle 686 kB with
perf.jsonl. AUDIO: opened at the machine's real mix format **44100 Hz 2ch
float32** (the portable parser negotiated it — not hard-coded 48k),
59 060 refill events, frames_written **26 462 842 = 100.006 %** of
realtime, **starved 0**, timeouts 0, buffer_errors 0, queue_audio_ms alive
in **601/601** perf lines (last 89 ms of a 100 ms buffer). Caveat kept:
the synthetic clip decodes ~5× cheaper than T1's real 720p30 content —
production budget reference stays §6.1 (28.9 %), real numbers at G2.
**Round-XIV omission found+fixed:** T10 BUILT test_knaudio.exe but never
RAN it (suites list) — added; round 11 expects 41 checks.
**Round-XV delta — phase C (`kn_net` + four self-tests; docs/NET.md):**
schannel TLS probes (handshake loop with EXTRA_DATA carry, alert
send-on-failure, 64-iter cap), winsock DNS/TCP443 with per-layer
non-NVIDIA baselines, cert-chain walk + CERT_CHAIN_POLICY_SSL verdict for
login.nvidia.com (the §7.9 stale-store diagnostic). Verification never
disabled; stock protocol defaults measured as-is. T10 gates dns/tcp443
(blocker findings turn it red on purpose) and REPORTS net.tls/tls.chain
(a Win7-TLS failure is a research finding to analyse, not a client bug).
Landmines hit in the sandbox (all recorded in docs/NET.md): sspi.h
requires **SECURITY_WIN32**; `????-??-??` in a string literal is a
**TRIGRAPH** under -std=c11 (-Werror=trigraphs); winsock2-before-windows;
mingw's SSPI entry points need explicit `A` suffixes; SP_PROT_TLS1_1/2/3
client bits missing from mingw's schannel.h (stable ABI values defined
locally, decode-only). Self-review caught the `k_net_nvidia[0].rc`
type-slip before the gates. Sandbox: native 8 suites PASS (674 checks),
cross -Werror PASS x64+i686, kn_selftest+knvstd clean under
-Wformat-truncation=2 (-O0/-O2/-O3), ctest 10/10, T10 dry-run ALL PASS
(38 — all four probes honestly skipped on POSIX), PE imports verified:
+crypt32/secur32/ws2_32 (all Win7-built-in). Awaiting: **T10 round 11 —
the first real execution of the phase-C probes; its net.tls/tls.chain
output is a stage-2 planning input** (Win7 TLS ceiling + cert-store gaps
feed the WSS/RTSP client design and R7).
→ `streamer/{include/kn,src}/kn_net.*` (new), `docs/NET.md` (new),
  `streamer/src/kn_selftest.c` (4 real probes, stubs extinct),
  `streamer/include/kn/kn_selftest.h`, `streamer/tests/test_knselftest.c`,
  `tests/T10_build_and_test.py` (8 suites + net gating policy),
  `CMakeLists.txt` + `tools/{build_native_tests,cross_check}.sh`
  (kn_net + ws2_32/secur32/crypt32), INTEGRATION.md rev 8

### 2026-10-23 · optional mbedTLS backend after target T10
Target T10 from the user: 62/62 harness checks ALL PASS; Schannel TLS
findings persist (`net.tls` failed on NVIDIA hosts and Cloudflare with
0x80090302; `tls.chain` failed because the handshake did not start). T10's
gated policy now matches the code: DNS/TCP gate, TLS/chain findings report.

Added optional `KN_TLS_BACKEND=mbedtls`: CMake builds pinned mbedTLS 3.6.0
and its framework as static subprojects (not the Linux `.a` artifacts from
the sandbox build); `kn_net_mbedtls.c` implements Win32 DNS/TCP/TLS using
Winsock, TLS 1.2 minimum, SNI + hostname verification, required certificate
validation, read-only Windows ROOT trust stores, and peer-chain reporting.
T10 downloads the fixed source archives, validates SHA-256, selects the
requested backend, and verifies backend identity in build.json. Updated
`docs/NET.md`; corrected the sandbox prep script to use tag `v3.6.0` and
initialize the framework submodule.

Validation: default Schannel native CMake build + ctest 10/10; mbedTLS
selected native CMake build + ctest 10/10; clean temporary checkout fetched
and verified both pinned mbedTLS archives; T10 Python syntax and diff checks
pass. Limitation: host GCC builds only POSIX stubs; `apt-get install
gcc-mingw-w64-x86-64` failed due permission denied. No Windows compilation or
mbedTLS handshake has occurred yet.

NEXT ACTION: run `set KN_TLS_BACKEND=mbedtls` and
`python tests\T12_network.py` on Win7. T12 builds only knvstd and runs just
DNS/TCP/TLS/chain. Treat mbedTLS as proven only if the Windows build passes,
`net.tls` verifies all three hosts, and `tls.chain` returns a clean chain.
The POSIX T12 run is a dry-run only. If target T12 fails, analyze
`T12_wynik.txt` and `t12logs/diag-*.zip`; do not advance to WSS/RTSP and
never disable verification. Run full T10 after this gate passes.
→ `CMakeLists.txt`, `streamer/src/kn_net_mbedtls.c` (new),
  `streamer/src/kn_net.c`, `streamer/include/kn/kn_net.h`,
  `streamer/src/kn_selftest.c`, `tests/T10_build_and_test.py`,
  `tools/prepare_mbedtls_sandbox.sh`, `docs/NET.md`, this file

### 2026-10-23 · network-only T12 harness
Per user request, added `tests/T12_network.py` so network iterations do not
repeat the graphics, decoder, clock, console, and unit-test portions of T10.
T12 configures the selected backend, builds only `knvstd`, runs the four
phase-C probes, validates their JSON and diagnostic bundle, and records
`T12_wynik.txt` / `T12_skrot.txt` under ignored paths. DNS/TCP are gates;
Schannel TLS remains a reported finding; mbedTLS TLS/chain are acceptance
gates. Updated `tests/README.md` and this handoff with the Windows command.

Validation: T12 POSIX dry-run built knvstd only, all four network probes
reported skipped, bundle/JSONL checks passed, and verdict was explicitly
`DRY-RUN ONLY`. It is not a network result. Existing Linux GCC warnings in
knlog are informational for POSIX T12; the Windows target still gates zero
warnings. The user's first mbedTLS target run then exposed the GCC 16
upstream-label warning; scoped suppression has been added but is not target-
verified yet. Next action is the same target T12 retry.
→ `tests/T12_network.py` (new), `.gitignore`, `tests/README.md`, this file

### 2026-10-23 · first target T12 mbedTLS compile
User ran T12 with `KN_TLS_BACKEND=mbedtls`. Archive download/hash checks and
CMake configure passed. GCC 16.2 failed while compiling upstream
`third_party/mbedtls/library/ssl_tls13_keys.c` because its intentionally
non-NUL-terminated label arrays trigger `-Werror=unterminated-string-
initialization`. Build stopped before `kn_net_mbedtls.c`, so no Win32 app
backend compile or TLS handshake was tested. Added a CMake suppression only
to the `mbedtls` target for GCC MinGW >= 16; all other targets/warnings stay
strict. Sandbox mbedTLS CMake build + ctest 10/10 pass. User should sync this
patch and rerun only `set KN_TLS_BACKEND=mbedtls` + `python
tests\T12_network.py`; no need for full T10 on this retry.

### 2026-10-23 · target T12 mbedTLS compile and handshake
After scoped suppression of GCC 16's upstream TLS-label warning, target
T12 reached `kn_net_mbedtls.c` and reported four errors: `valid_to` was
mistakenly used instead of the public result field `not_after`, and
mbedTLS 3.6 ciphersuite fields require `MBEDTLS_PRIVATE(...)`. Fixed those
references and reran sandbox mbedTLS build + ctest (10/10 PASS).

User reran target T12: CMake configure and `kn_net_mbedtls.c` compile/link
passed with zero compiler warnings; DNS and TCP/443 passed. TLS/chain then
failed on all hosts with `0xffff9400` (`-0x6c00`,
`MBEDTLS_ERR_SSL_INTERNAL_ERROR`), before any peer certificate was exposed.
Added mbedTLS debug callback capped at level 2, mapped to INFO diagnostics,
plus `mbedtls_strerror` WARN output. Level 2 emits no raw TLS buffers or key
material. Sandbox build + ctest 10/10 PASS. Next: sync this diagnostic patch,
run only T12, inspect `t12logs/diag.log`, then fix the specific handshake
failure. T10/GPU tests are not needed for this network-only iteration.

User supplied that T12 `diag.log`: the failure occurs in TLS 1.3 ECDH,
where `psa_generate_key()` returns `-0x6c00`. The pinned mbedTLS SSL client
example calls `psa_crypto_init()` before RNG/SSL setup. Added PSA init before
CTR-DRBG seeding and explicit WARN logging if init itself fails. Sandbox
mbedTLS build + ctest remain 10/10 PASS. NEXT: sync this change and rerun
only T12 on Win7; verify the handshake now advances and all cert chains
validate. No full T10/GPU run is needed.

User's next T12 after PSA init confirmed ECDH now succeeds, but TLS 1.3
fails later in `mbedtls_ssl_tls13_calculate_verify_data()` while processing
the server Finished message (`psa_mac`/HKDF path, `-0x6c00`). Set the probe's
maximum TLS version to TLS 1.2 while keeping `VERIFY_REQUIRED` and the
minimum at TLS 1.2. This is a protocol-isolation/Win7-compatible acceptance
test, not a verification bypass. Sandbox mbedTLS build + ctest 10/10 PASS.
NEXT: run network-only T12 again; if TLS 1.2 passes all three hosts and the
chain, accept that backend for Win7 and investigate TLS 1.3 separately.

User's target T12 (2026-10-23 22:47) now PASSES 27/27 with mbedTLS TLS 1.2:
zero-warning `knvstd` build, DNS/TCP pass, all three TLS handshakes verified,
and `login.nvidia.com` chain depth 3 to Amazon Root CA 1 with trust/policy 0.
The summary's `TLSv1` is truncated by the reason formatter (`%.5s`); the probe
sets min=max TLS 1.2 and the full result is `TLSv1.2`. TLS 1.3 remains a
separate issue and is not negotiated by this Win7 backend.

T10 is now confirmed `ALL PASS (65 checks)` with mbedTLS selected and default.
Stage 1 is closed; Stage 2 starts with the AU assembler and its target-only
test, then reorder/FEC/network integration. Do not repeat GPU tests during
ordinary Stage-2 iterations; run full T10 only after integration-wide changes.

### 2026-10-23 · mbedTLS default + Stage 2 AU assembler
User reported full T10 `ALL PASS (65 checks)` with mbedTLS, following the
target T12 TLS 1.2 pass. Promoted mbedTLS to the CMake, T10, and T12 default;
Schannel remains an explicit fallback. Fresh sandbox default build records
`tls=mbedtls`; ctest 10/10 passes. Stage 1 is formally closed from prior
target G1 evidence, and Stage 2 is now in progress.

Implemented `kn_gs_au_assembler` using caller-owned storage (no allocation),
2 MiB default / 16 MiB maximum cap, SOF/EOF assembly, H.264/H.265 Annex-B
payload location, AV1 short-header last-packet length and extended-header
expected-AU length, frame-type/keyframe metadata, trailing AUD removal,
24-bit stream-index continuity, and explicit incomplete-frame/gap/keyframe
signals. `test_kngs` now has 125 checks and passes; full default-mbedTLS ctest
passes 10/10. No real stream packets have gone through the assembler yet.

NEXT: target-only check: `cmake --build build --target test_kngs` followed by
`build\test_kngs.exe` (125 checks PASS). No T10/T11/GPU run needed. Then
implement the bounded RTP reorder buffer using extended SRTP packet indexes,
gap wait/NACK ranges, stale/duplicate rejection, and frame invalidation/PLI.
→ `CMakeLists.txt`, `tests/T10_build_and_test.py`, `tests/T12_network.py`,
  `streamer/include/kn/kn_gs.h`, `streamer/src/kn_gs.c`,
  `streamer/tests/test_kngs.c`, `docs/GS.md`, this file

### 2026-10-24 · impl round XVI (stage-2 media core: kn_rx + SRTP phase 3 + replay-capture — ALL sandbox gates green, awaiting target T10 r12)
**Context:** fresh agent session bootstrapped from THIS file + the repo
(clone) + OpenNOW @ a49b124 (re-cloned; nvst.rs 14 695 lines confirmed).
Baseline reproduced first: vendor files hash-verified, native 8 suites
703 checks PASS, cross-check PASS — the round-XV (Copilot) AU assembler
delta verified green in the sandbox (test_kngs 125/125) BEFORE building on
it. User instruction this round: *"continue stage 2, write+test as much as
possible before laptop testing; build tests so they run to completion even
when parts fail"* — the whole battery is section-isolated with per-check
accounting and never aborts (the fault-tolerant contract is now the suite
design rule).
**Review of the inherited AU assembler found two faithfulness gaps**
(fixed, vectors unaffected): `GsFrameHeader::parse` is codec-INDEPENDENT in
the reference (nvst.rs:3279-3289 — two ordered pattern arms), and
`Option<NvstFrameType>` distinguishes `Some(Unknown(x))` from `None` — the
RFI admit() logic keys off `is_some()`. Added `frame_type_parsed` /
`frame_type_known`; `kn_gs_rtp_parse` split into `parse_hdr` +
`kn_gs_rtp_payload` because SRTP padding bytes are CIPHERTEXT on the wire
(the reference validates padding only in payload() AFTER decryption,
nvst.rs:2695-2697).
**Round-XVI delta — `kn_rx` (docs/RX.md): the full pure NvstVideoReceiver
media path**, zero-allocation, injected-ms-clock, caller-owned pools:
RtpReorderBuffer (3537-3660 — 52 ms dequeue deadline, genuinely-absent
NACK ranges, empty-buffer jump resync, recovery ready.clear() quirk),
FecBlock+FecReorderBuffer (3925-4310 — knfec reconstruct, template
envelope re-wrap with pitfall #4 encoded, repair_failed single-retry,
completed_through silent-ignore, silent single-block wait), consume path
(4741-4862 — SOF-before-EOF, SPI continuity skip-push, RecoveredData
is_fec force, RFI admit/withhold), FrameInvalidation + queue
(nvst_invalidation.rs — RFI ranges, 100 ms retry, 250 ms one-shot IDR
backstop), NACK tracker (4 ms/52 ms/3-attempt/RTT policy, 64-batch cap,
overflow→keyframe), watchdog + timeouts + lifecycle + reset_media_state
(next_frame_contiguous=FALSE quirk), and the feedback ENCODERS: NACK-v2
0x0317 (base+bitmap records, wrap grouping), IDR 0x0302, RFI-range 0x0301,
frame-ack 0x0204 (f64 bits + 0xBF800000 constants), pacing 0x0203, QoS
0x0207 (command() field map ported as-is), RTCP generic NACK + PLI.
**SRTP phase 3 (docs/SRTP.md):** KDF256 + AES-256-CM + AES-GCM(128/256,
tags 4-16) under KN_TLS_BACKEND_MBEDTLS with honest NO_BACKEND stubs in
bare builds; `kn_rx_srtp_session` implements the four NvstSrtpMaterial
profiles and the reference unprotect order. GCM256-tag8 (the legacy
nvstVideo default = the production expectation) fully wired.
**Verification (the two-independent-implementations rule):**
`tools/rx_reference.py` — a from-scratch Python port of every table above —
SELF-PROVES (363 checks) against goldens transcribed from the reference's
own Rust tests (the 52 ms deadline scenario, NACK exclusion, parity-only
1+2 repair, 0xa5-poisoned fec_word 3+1 repair with byte-exactness at
[..28]/[32..], retain-following-blocks incl. allocation identity,
gaps_require_recovery contiguous=false, nack_v2/RTCP hex goldens, the
round-trip grid) + a pure-Python AES-256/GHASH/GCM oracle proved against
FIPS-197 C.3 and the GCM spec vectors → emits 31 scenarios + op scripts +
6-profile SRTP vectors. `test_knrx` (9th suite): battery + reorder units +
5 property families + error paths = **184 checks bare / 209 mbedTLS, 0
failures**; `rx.vectors` self-test runs the same battery from the shipped
binary (~1 ms, 126/153 checks). Gates: native 9 suites 889 checks PASS;
cross mingw x64+i686 -Werror (+truncation=2 on touched files) CLEAN;
ASan+UBSan+LSan clean; CMake+mbedTLS ctest 11/11; T10 POSIX dry-run ALL
PASS (57); kn_selftest paranoid-level warning count UNCHANGED (9
pre-existing, 0 new).
**Bugs the battery caught (all fixed, RX.md landmines):** (1) the reorder
map is caller memory — init MUST clear `used` flags (malloc'd-arena
scenarios caught what static-context unit tests could not); (2) arena
regions need 8-byte alignment (UBSan); (3) `--replay-capture` vps
derivation must subtract the SRTP tag from the largest WIRE record or
every FEC-repaired AU grows tag_len trailing bytes (E/F matrix caught it:
+10 CM128 / +8 GCM on exactly the repaired frames); (4) the capture
generator's parity base-seq off-by-one produced a duplicate wire seq — the
pipeline's replay window correctly rejected it (independent
implementations disagreeing loudly = the system working).
**NEW tool `--replay-capture` + `tools/make_rx_capture.py` (the offline G2
mechanism):** packetizes the real synthetic.264 clip into canonical
GS/RTP+FEC records (SPI = data-only counter — parity consumes RTP indices
but not SPIs), injects loss/reorder, optionally SRTP-protects, and records
the Python reference receiver's verdict in the manifest; knvstd replays on
the capture timeline (52 ms deadlines live) emitting per-AU JSONL
(fi/len/sha1/kf/contiguous) + optional .264 dumps for ffprobe. Matrix 6/6
VERIFY PASS: lossless; FEC25+loss6+reorder30%; FATAL loss (RFI
withhold-until-IDR: 38/60 frames — identical verdicts both sides);
FEC50+loss8+reorder20%; SRTP-CM128; SRTP-GCM256 (mbedTLS build).
**Key design insight recorded (RX.md §Parity):** canonical parity packets
carry the algebraic FOLD at ext[0..4] (spi) and ext[8..11] (flags) but
their OWN envelope/ext[4..8]/ext[11]/fec_word — which is exactly why a
recovered shard reconstructs spi+flags perfectly (continuity + the 4089
validation pass), why the re-wrap patches only envelope/frame_index/
ext11, and why the recovered fec_word is garbage by design (pitfall #4).
Recovered packets are shard_len-sized → repaired AUs carry trailing zero
stuffing (reference-sanctioned for AVC; the verifier models it exactly).
Awaiting: **target T10 round 12** (first target run of test_knrx +
rx.vectors incl. the GCM/CM256 legs), then stage-2 sockets (Next steps 2).
→ `streamer/{include/kn,src}/kn_rx.*`, `streamer/src/kn_rx_kat_runner.h`,
  `streamer/src/kn_rx_kat_vectors.h` (generated), `tools/rx_reference.py`,
  `tools/make_rx_capture.py`, `streamer/tests/test_knrx.c`,
  `streamer/{include/kn,src}/knsrtp.*` (phase 3),
  `streamer/{include/kn,src}/kn_gs.*` (split parse + assembler fixes),
  `streamer/src/kn_selftest.c` (rx.vectors + relevance),
  `streamer/include/kn/kn_selftest.h`, `streamer/tests/test_knselftest.c`,
  `streamer/src/knvstd_main.c` (--replay-capture), `CMakeLists.txt`,
  `tools/{build_native_tests,cross_check}.sh`,
  `tests/T10_build_and_test.py`, `docs/{RX,SRTP,GS}.md`,
  `INTEGRATION.md` rev 9, `tests/README.md`, this file

### 2026-10-25 · impl round XVII (T10 r12 ALL PASS 67 + G2-offline demo CLOSED → stage-2 SOCKET LAYER delivered: kn_sock + --capture/--decode + persistent decoder + T13)
**Context:** fresh agent session bootstrapped from THIS file + the repo clone
(HEAD `c3ff27c`) + OpenNOW @ a49b124 (nvst.rs sha256 `f52fc09f…`, 14 695
lines re-verified) + the user's three uploads (T10_wynik.txt = round 12,
idk_g2.txt = the G2-offline demo console, this file). Baseline reproduced
FIRST: vendor files hash-verified, native suites green, cross-check green —
exactly the round-XVI numbers (889 checks then; 972 now with the new suite).
**Target evidence analysed (both items round XVI awaited):**
(1) **T10 round 12 (2026-10-24 12:52): VERDICT ALL PASS, 67/67 checks.**
The round-XVI media core is target-verified: test_knrx **209/209** on Win7
(first execution — the GCM/CM256 legs active in the mbedTLS build),
`rx.vectors ok` 153 checks (scenarios 31/31, `srtp_backend:1`), registry
13/13 usable, zero warnings (GCC 16.2, 116 s build), decode 60/60 @
3333 fps, d3d probes reproduced (pitch 1536, present mean 0.29 ms, fill
max 28 ms = first touch), net.tls + tls.chain ok (TLS 1.2, depth 3, Amazon
Root CA 1), bundle 139 545 B validated, PE 6.01. **clock.resolution `ok`
this run** (Sleep(1) 1.00→1.75 ms, wall tick 1000→1000 µs) — the machine
alternates timer regimes across reboots; R30 reworded (worst-case floor
2.9 ms stands for pacing design; both regimes now have target samples).
(2) **idk_g2.txt (the user's G2-offline demo): SUCCESS — one missing shell
redirect, zero product defects.** Target capture generation matched the
sandbox regeneration byte-counts exactly (seed-1 determinism ACROSS
machines: 119 records / 360 333 B). Target replay: 60/60 AUs, all
contiguous, fec_repaired=19, zero stale/gaps/withheld, state 0; ffprobe
accepted au_00001_k.264 (h264 CB 320×240 — the synthetic.264 KAT clip's
profile, expected). The failing `--verify` line: `python … --verify
cap.json < replay.jsonl` → cmd's "Nie można odnaleźć określonego pliku"
= the redirect INPUT `replay.jsonl` never existed because step 2 ran
WITHOUT `> replay.jsonl` (python never even started). Reconstructed the
console JSONL in the sandbox → **VERIFY PASS: 60 frames byte-identical to
the reference receiver**, and the sandbox replay of the same capture is
byte-identical to the target's console output (all 60 AU lines + every
summary counter). tests/README.md's demo block now carries the redirect
INLINE (the parenthetical note was missed — make the command itself
correct); T13 automates the whole sequence so no future demo depends on
hand-copied redirects. Raw evidence archived: `artifacts/T10-round12/`.
**Round-XVII delta — the stage-2 socket layer (docs/SOCK.md is the design
record; provenance re-read from nvst.rs @ a49b124: 8 MiB SO_RCVBUF :235
(applied :5682), 10 ms poll :244 — the old Next-steps cite ":222" was
stale, RX.md corrected):**
- **`kn_sock`** (new module): bounded UDP, Win32 winsock + POSIX BOTH
  REAL (no stubs — the sandbox runs the full live E2E), zero allocation,
  refcounted WSAStartup, QPC/CLOCK_MONOTONIC µs+ms clock (the live
  `now_ms` source), SO_RCVBUF request + getsockopt READBACK (Linux caps at
  ~208 KiB→reports 425 984; Windows honours 8 MiB — every summary reports
  both), select-based recv with honest per-platform truncation semantics
  (Win32 WSAEMSGSIZE = datagram LOST, POSIX silent truncation — documented
  + tested both ways), sendto, lifetime counters. `test_knsock` (TENTH
  suite, 70 checks): clock/ipv4/lifecycle/100-datagram byte-exact
  round-trip/timeout/65 507 B max datagram/512-burst drain with seq
  markers/the PLATFORM-DELTA case/bad-args — section-isolated per the
  round-XVI suite rule.
- **`knvstd --capture`**: streaming recorder in the EXACT kn_ring_dump
  line format (→ `--replay-capture` + `--verify` read real sessions with
  ZERO tooling changes — the R32 mechanism). Documented deviation from the
  old Next-steps wording: the FILE is primary (a ring cannot hold a whole
  session on 2 GB); the 64 KiB kn_ring is the failure-diagnostic mirror
  (dumped to `<log_dir>/wire_capture.log` → rides the auto-bundle). Flush
  every 32 records/100 ms (bounded R2b loss, no fsync storm). Ctrl-C
  graceful (SetConsoleCtrlHandler/SIGINT) → clean summary. Empty capture =
  rc 1 + E_CAPTURE_EMPTY amplification (never a silent pass).
- **`knvstd --decode`**: live socket → kn_rx (real QPC-ms clock, real 5 s
  timeout — the production watchdog, unlike replay's MAX) → **capacity-2
  encoded queue** (analiza/04 §2.4) with the reference overflow policy
  (drop-new + `kn_rx_inval_lose` + `kn_rx_video_request_keyframe` — the
  ONE new kn_rx API, +6 test_knrx checks: 190 bare / 215 mbedTLS) →
  persistent decoder session. Per-AU JSONL with sha1 + capped rx-event
  lines + kf_request lines (UNSENT until stage 3 — recorded, never
  dropped) + 1 s transport stats log lines. Summary is FIELD-COMPATIBLE
  with `make_rx_capture.py --verify` ("frames" alias) → the cross-impl
  contract now covers LIVE traffic. **D21: single-threaded** (recv→process
  →inline drain→poll at 10 ms cadence; 8 MiB rcvbuf absorbs decode
  stalls; stage-1 loopback precedent; queue semantics already
  thread-ready; revisit on measured backlog).
- **`kn_decode_stream_*`** (new persistent session in kn_decode):
  `kn_decode_h264_mem` is one-shot — per-AU feeding through it kills
  inter-frame references. MEASURED (sandbox_stream_decode.c control):
  one-shot per AU delivers **2 of 60** frames; the persistent session
  delivers **60/60, 0 failures**, survives a poisoned mid-stream AU, and
  has a sticky callback abort. NO parser on this path (whole AUs in) → the
  round-IX parser-flush landmine cannot occur; `flush()` drains the
  decoder. Honest stubs in bare builds (`--decode` then runs
  pipeline-only, reported as such).
- **`sock.loopback`** — the 14th self-test, PORTABLE + GATED (loopback UDP
  failing blocks stage 2 anywhere): 64-datagram burst byte-exact, honest
  50 ms timeout, rcvbuf readback, closed-socket discipline; ~50 ms from
  the shipped binary. Relevance mapping learned socket/udp/recv/bind/
  capture/decode keywords (§7.7.4).
- **`tools/send_rx_capture.py`**: paced UDP sender on the capture's µs
  timeline (hybrid sleep+spin — regime-proof per R30; `--speed 0` blast);
  **`tests/T13_udp_e2e.py`**: the socket gate (capture leg → vector leg
  (replay of the LIVE-RECORDED file vs the reference manifest) → decode
  paced → decode blast → SRTP cm128 → SRTP gcm256 (auto-enabled from the
  binary's rx.vectors evidence) → sock.loopback → jsonl hygiene; wipes
  t13work/t13logs per the T11 lesson; reuses T10's build). T10 rev: 10
  suites + registry 14 + sock.loopback gated (POSIX dry-run 58 ALL PASS).
**Bugs caught by the gates this round (all fixed, all recorded in
docs/SOCK.md):** (1) **LSan: `kn_decode_stream_close()` leaked
kd_deliver's NV12 buffer (115 264 B)** — R33's rule earned its keep on
the FIRST sanitized E2E run; (2) `_POSIX_C_SOURCE` preamble missing in two
fresh files (the kn_diag landmine re-fires in every new POSIX-side file —
now documented in both); (3) mingw `socklen_t` needs `<ws2tcpip.h>` (the
kn_net landmine, re-confirmed); (4) WSA-refcount leak in open()'s error
paths (self-review, pre-gate); (5) T13 initially merged stderr into the
verify input — the PRE-EXISTING vendored `sha1.c:26` int-promotion UB
(pinned by hash, must NOT be edited) poisoned instrumented runs →
stdout-only capture + last-JSON-line parsing; sanitizer policy: recover
mode, zero streamer-code diagnostics (verified: sha1.c is the ONLY runtime
error in a sanitized T13).
**Verification matrix (sandbox, before handover):** native **10 suites 972
checks PASS**; cross mingw x64+i686 `-Werror` PASS (incl. kn_sock +
extended knvstd/kn_decode/kn_rx/kn_selftest); `-Wformat-truncation=2`
clean -O0/-O2/-O3 on every touched file (two host-only errsz==1
over-approximations documented, round-XI class); CMake ctest **12/12 in
BOTH backends** (schannel + mbedTLS, `tls=mbedtls` recorded);
ASan+UBSan+LSan: suites + the FULL T13 matrix clean; **T13 ALL PASS on
three builds** (44 FFmpeg-bare with real decode 60/60 paced+blast+cm128 ·
44 under sanitizers · **55 mbedTLS incl. the gcm256 leg**); T10 POSIX
dry-run ALL PASS 58; cross-machine parity: target idk_g2 JSONL == sandbox
output byte-identical, regenerated capture == target's 119 records/
360 333 B, cap.hex sha256 `31386ca8…` recorded.
**Awaiting (round XVIII):** target `T10 r13` (expect ALL PASS ~69:
test_knsock ~70, test_knrx 215, registry 14) + target `T13 r1` (expect
ALL PASS 55 — first Winsock execution of the live modes; record
`rcvbuf_actual` from the decode begin line: R34's headroom question).
Then WSS+RTSP (Next steps 2) and the presentation wiring (Next steps 3).
→ `streamer/{include/kn,src}/kn_sock.*` (new), `streamer/tests/test_knsock.c`
  (new), `streamer/{include/kn,src}/kn_decode.*` (stream session + leak fix),
  `streamer/{include/kn,src}/kn_rx.*` (request_keyframe),
  `streamer/tests/test_knrx.c` (+6), `streamer/src/kn_selftest.c`
  (sock.loopback + relevance), `streamer/include/kn/kn_selftest.h`,
  `streamer/tests/test_knselftest.c` (115 checks),
  `streamer/src/knvstd_main.c` (--capture/--decode/stop-handling),
  `tools/send_rx_capture.py` (new), `tools/sandbox_stream_decode.c` (new),
  `tools/fetch_ffmpeg_headers_sandbox.py` (new), `tests/T13_udp_e2e.py` (new),
  `tests/T10_build_and_test.py`, `CMakeLists.txt`,
  `tools/{build_native_tests,cross_check}.sh`, `docs/SOCK.md` (new),
  `docs/RX.md`, `tests/README.md`, `INTEGRATION.md` rev 10, `.gitignore`,
  `artifacts/T10-round12/` (raw evidence, gitignored), this file

### 2026-10-25 · impl round XVIII (T10 r13 ALL PASS 69 + T13 r1: 2 fails = ONE JSONL defect — reproduced, fixed, re-verified)
**Context:** same session as round XVII (continuation). User delivered
T10 r13 + T13 r1 results (T10_wynik attached; T13_wynik pasted in chat —
too big to attach; T13_skrot attached; all archived in
`artifacts/T13-round1/` with an evidence README).
**T10 round 13 (2026-10-25 08:36): VERDICT ALL PASS, 69/69** — exactly
the predicted count. Round-XVII socket delta target-verified: test_knsock
**71** (host 70 + the Win32 WSAEMSGSIZE-branch delta — the PLATFORM DELTA
case executing for real), test_knrx **215**, registry **14**,
sock.loopback ok in the shipped binary (64/64 burst 1 ms, timeout 50 ms),
zero warnings GCC 16.2, decode 60/60 @ 4000 fps, d3d probes reproduced,
net.tls/tls.chain ok, clock.resolution ok (healthy regime: wall tick 1000
µs, Sleep(1) 1.00→1.00). **R34 datapoint: Windows applies SO_RCVBUF
exactly (262 144 req → 262 144 applied).**
**T13 round 1 (2026-10-25 08:39): VERDICT FAIL (2 of 51).** Diagnosis —
both failures are ONE product defect, and everything else PASSED on Win7:
capture recorded 119/119 (the vector leg parsing live.hex proves the file
was perfect), all four decode legs 60/60 with real FFmpeg 63 (paced +
blast + cm128 + **gcm256 live over a socket**), zero q_overflows, zero
recv_errors, paced sender held the timeline (1966 ms for the 2 s
capture), sock.loopback ok, check_jsonl ok. The defect: **Windows
absolute paths embedded RAW into stdout JSONL** — the capture summary's
`"out":"C:\Users\KarnyJohnny\…"` contains `\U`, which is not a legal JSON
escape, so (a) the capture-summary line was unparseable ("summary
present" FAIL, its 4 dependent checks never ran — hence 51 not 55) and
(b) the replay summary line (same defect, `"capture":"C:\…"`, latent
since round XVI — relative paths hid it until T13 passed absolute ones)
made `--verify` report "1 unparseable stdout lines" **while all 60 frames
actually matched the reference**. The loopback mode never hit this because
round XII gave it `lb_json_path()` (T11-proven on the target); the
replay/capture/decode printfs didn't use it.
**Fix (this round):** (1) `lb_json_path` promoted to the
platform-neutral section of knvstd_main.c as THE stdout-JSON string
sanitizer (`\` and `/` → `/`, `"` escaped, C0 → `?`) and HARDENED: bytes
`>= 0x7F` → `?` (Win32 argv is ANSI cp1250 — a raw non-ASCII byte would
be invalid UTF-8 inside the JSONL contract; UTF-8-legal but non-ASCII
paths trade fidelity for parseability, documented); (2) every
argv/path/foreign string site routed through it — audit: unknown_selftest
name, diagnostics bundle path, replay error+summary paths, capture
error/detail/summary bind+out, decode error detail (loopback's three
sites already used it); (3) `st_gs_parse`'s KN_GS_CAPTURE path gets the
sibling `st_json_str` in kn_selftest.c (same rules). kn_selftest.c bar:
default-level `-Werror` clean both toolchains; paranoid `=2` count
UNCHANGED at the recorded 9 pre-existing over-approximations, 0 new.
**Reproduction discipline:** the target failure was reproduced 1:1 in the
sandbox BEFORE the fix — POSIX filenames legally contain literal
backslashes, so `--replay-capture='/tmp/cap\win.hex'` produced the exact
"VERIFY FAIL: 1 unparseable stdout lines" with 60/60 frames matching;
after the fix the same run gives VERIFY PASS with all 61 lines parseable
and the path normalized to `/tmp/cap/win.hex`. The capture leg was
reproved too (backslash `--capture-out`, summary parses, file written).
**T13 hardened (the gate learns):** per-leg "every stdout line parses as
JSON" checks (capture/replay/each decode leg — this defect class can
never again hide behind a downstream verdict), `rcvbuf_actual` logged per
decode leg from the begin line (R34 evidence in every future run), and a
POSIX-only backslash-path replay leg (on Windows the absolute paths in
every leg already exercise the contract).
**Sandbox gates after the fix:** native 10 suites 972 checks PASS; cross
mingw x64+i686 `-Werror` PASS; knvstd_main clean under
`-Wformat-truncation=2` -O0/-O2/-O3 both toolchains; CMake ctest 12/12
both backends; **T13 ALL PASS 51 (FFmpeg-bare) · 51 (ASan+UBSan+LSan
build) · 63 (mbedTLS incl. gcm256)** — the 63 includes the 2 POSIX
backslash-leg checks, so the Windows prediction is **61**; T10 POSIX
dry-run ALL PASS 58.
**Awaiting (round XIX):** target T10 r14 (MANDATORY before T13 — it
rebuilds knvstd with the fix; expect ALL PASS 69) + T13 r2 (expect ALL
PASS 61 + the 8 MiB rcvbuf_actual lines). Then WSS/RTSP (Next steps 2)
and/or the presentation wiring (Next steps 3).
→ `streamer/src/knvstd_main.c` (lb_json_path promoted+hardened, 9 sites
  routed), `streamer/src/kn_selftest.c` (st_json_str + gs.parse capture
  path), `tests/T13_udp_e2e.py` (contract checks + rcvbuf logging +
  backslash leg), `docs/SOCK.md` (landmine 6 + §7 target record),
  `artifacts/T13-round1/` (raw evidence, gitignored), this file

### 2026-10-25 · impl round XIX (T10 r14 + T13 r2 BOTH ALL PASS on target → SOCKET LAYER CLOSED; WSS/RTSP research phase → docs/RTSP.md)
**Context:** same session as rounds XVII-XVIII. User delivered T10 r14 +
T13 r2 results (attached; archived in `artifacts/T13-round2/`).
**T10 round 14 (09:54): ALL PASS 69/69** — the round-XVIII JSONL-escape
rebuild gate: zero warnings (GCC 16.2), test_knselftest 114 (Win32 delta),
test_knrx 215, test_knsock 71, registry 14 all usable. `clock.resolution`
**degraded** this run (Sleep(1) 15.62→2.88 ms, wall tick 15600 µs) — the
machine flipped back to the sleep regime ~80 minutes after r13's healthy
1000 µs ticks (R30 now documents same-day flips; T10 accepts degraded by
design, honest verdict beats a fudged ok).
**T13 round 2 (09:58): ALL PASS 61/61 — the exact predicted count.** The
round-XVIII fix is target-verified: capture leg 119/119 records with every
stdout line parseable; vector leg replays the LIVE-RECORDED capture
byte-identical to the reference; all four decode legs 60/60 with
libavcodec 63.2.100 (paced + blast + cm128 + gcm256 live over a socket),
zero q_overflows, zero recv_errors, every stdout verified against the
Python reference manifest; sock.loopback ok. **R34 CLOSED (except the
real-session firewall note): `rcvbuf requested 8388608 → applied 8388608`
— Windows honours the full 8 MiB SO_RCVBUF on every leg (~6 700 datagrams
of slack at shard 1296; the burst-headroom math holds on the target).**
**The stage-2 SOCKET LAYER is closed end-to-end on the target machine.**
**Round-XIX delta — the WSS/RTSP research phase (read, not remembered):**
new `docs/RTSP.md` — the full stage-2 signaling specification with line
provenance into OpenNOW @ a49b124: RTSP-over-WebSocket transport
(`wss://host:port/rtsp`, default port 322, `x-nv-sessionid` handshake
header, one RTSP request per WS Text frame, the CSeq==Request-Id
serializer, the Bifrost leading-CRLF-padding-tolerant parser with the
CSeq→Request-Id fallback and the TEARDOWN mismatch-skip rule, 20 s /
4 MiB limits); the method sequence with exact headers (X-GS-Version 14.2,
x-nv-abtesting 2, Accept application/sdp…) and every quirk (SETUP
stop-on-peerless-200 alliance-poison rule, 3×3 s peer retries, the two
Transport forms, the URI candidate ladder, Transport `source=`/
`X-GS-ServerPort=` peer extraction, disablePlay, the
nativeRtcOnBundlePort==1 requirement); DESCRIBE SDP extraction incl. the
SRTP material rules (runtime.encryptionKey 64-hex = AES-256 +
encryptionKeyId → **salt = key_id as 24-digit uppercase hex**, client
random-key fallback, the 8-profile scan order defaulting to
**AEAD_AES_256_GCM_8** — confirming round XVI's GCM256-tag8 production
expectation from the reference itself); the COMPLETE build_announce
attribute set (incl. refPicInvalidation:1 and the NACK queue constants
that match kn_rx exactly); the handoff JSON = the literal bridge into
kn_rx_video_config + kn_rx_srtp_session + the connected UDP peer; and
CloudMatch (GFNJWT auth header, Content-Type text/plain quirk, the full
POST v2/session sessionRequestData field list, readiness status∈{2,3}+
rtspsEndpoints, resume statuses 2..=5, the 31-field session_info R22
contract, signalingUrl = rtspsEndpoints[0] rtsps→wss). Plus the round-XX
staged implementation plan (kn_ws → kn_rtsp → kn_cm → OAuth research →
--session integration) and the fake-rig sandbox test strategy
(transport-injection seam; production crypto never bypassed — the fake
rig sits behind plain ws:// in tests only). NOT VERIFIED list included
(OAuth/gfn.rs unread; networkTestSessionId optionality; real-cloud shapes
— first real session is the true test, per R32's rule).
No product code changed this round (analysis + research + docs only); all
round-XVIII gates stand. **Awaiting: round XX implementation (no target
run needed until kn_ws/kn_rtsp land with their sandbox fake-rig E2E).**
→ `docs/RTSP.md` (new), `artifacts/T13-round2/` (raw evidence, gitignored),
  this file (status, R30/R34, Next steps, round XIX log)

### 2026-10-25 · impl round XX (stage-2 signaling cores: kn_ws + kn_rtsp — ALL sandbox gates green; target T10 r15 expects 71)
**Context:** same session; user green-lit round XX ("zaczynaj") after the
round-XIX research (docs/RTSP.md §1-7). This round implements §6 stages
1-2: the WebSocket framing and the RTSP-over-WS client. Stages 3-5
(kn_cm, OAuth, TLS-stream adapter, fake-rig E2E, --session integration)
are round XXI. NO existing module changed behaviour — kn_rx/kn_sock/knvstd
untouched, so the target-verified socket layer stands as-is.
**`kn_ws`** (new): RFC 6455 CLIENT framing over an injected byte-stream
seam (`kn_ws_io` send/recv) — production will plug the kn_net_mbedtls
TLS-1.2 stream (round XXI), the sandbox proves framing on an in-memory
duplex shim + 1-byte-chunk delivery. Handshake: CSPRNG 16-byte key →
base64 → GET /path with Host/Upgrade/Connection/Key/Version + verbatim
extra headers (the reference's `x-nv-sessionid` + `content-length: 0`);
101 + `Sec-WebSocket-Accept` == base64(SHA-1(key+GUID)) enforced (RFC
golden vector in the suite; kn_sha1 reused). Frames: client masking with
a FRESH CSPRNG mask key per frame (RFC 5.3), 7/16/64-bit lengths,
fragmentation join into a caller-owned buffer (never realloc →
ERR_BIG), ping→echoed pong inline, pong skipped+counted, close answered
(1000) + surfaced with the peer code, RSV/masked-server-frame/
fragmented-control/oversize-control/orphan-continuation = ERR_PROTO.
Mid-frame timeouts RESUMABLE (frame state in the struct). RNG: Win32
`rand_s()` (msvcrt — **no new PE import**; bcrypt/advapi32 deliberately
avoided), POSIX getrandom→urandom; RNG failure REFUSES the operation
(never send weak keys/masks). `test_knws` (11th suite): **84 checks** —
RFC 4648 base64 goldens, the RFC 6455 §1.3 accept golden, request-shape
inspection, every failure class, wire-exact frame bytes at the
125/126/65535/65536 boundaries, fragmentation, control semantics,
protocol-error matrix, oversize, chunked recv, timeout resume.
**`kn_rtsp`** (new): the pure client per docs/RTSP.md — serializer
(CSeq==Request-Id, exact reference shape), incremental parser (Bifrost
leading-CRLF padding dropped even while incomplete; `\r\n\r\n` AND `\n\n`
ends; LF-only lines; case-insensitive last-wins headers; junk
Content-Length→0; status line = FIRST line only; drain-then-error on
sequence mismatch with the Request-Id fallback; 4 MiB cap), request loop
(deadline over the injected clock, remaining-time recv waits, TEARDOWN
mismatch-skip flag, body aliases the accumulation buffer — the memmove
only shifts the tail, documented), and the toolset: `sdp_attribute` (the
`||`/`;;` WS-SDP quirks, x-nv-prefix-then-plain candidates, empty=absent),
`media_control` (skips `a=control:*`), `runtime_key` (64-hex validation,
uppercasing) + the `{key_id:024X}` salt rule, the 8-profile SRTP scan
(per-value priority, SETUP headers before DESCRIBE lines, default
AEAD_AES_256_GCM_8), `parse_video_peer` (source=/X-GS-ServerPort=,
span≥16 collapse, comma+case tolerance), the SETUP URI ladder (the
`streamid=video/0`→`/0` rule + absolute forms), `extract_describe`
(Session + video control required; V2 credential preference; pingVersion
default 6; disablePlay/nativeRtcOnBundlePort/rtcpOnSctp/unifiedSocket;
runtime key passthrough), `setup_video` (the FULL sweep: stop-on-
peerless-200 poison rule, CloudMatch bundle-peer fallback, 3×3 s retry
rounds on the injected sleep, per-candidate remaining-deadline timeout
narrowing, the two Transport forms with the exact
`unicast;X-GS-ClientPort={p}-{p+1}` bytes, 400/404/459/460/461 continue
class, fatal-status rule, x-nv-ping/-payload extraction),
`build_announce` (the complete §3 attribute list incl. the color lines
and trailing empty line, CRLF-joined) and `negotiate_announce` (the
16-rule offered_in range table, min>max fallback, repair clamp,
initial/peak/ceiling bitrate clamps, line-substitution rewrite,
negotiated-values output for the evidence log). `test_knrtsp` (12th
suite): **127 checks** — every fixture TRANSCRIBED from the reference's
own tests (parser :3443-3470, official-control :3470+, announce params
:2923+) + scripted-server E2E of the full
OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY with byte-exact request inspection
and the D14 720p30 announce + TEARDOWN skip + every SETUP branch +
error paths. Injected clock/sleep make retry rounds instant+countable
(the rio shim ADVANCES the clock on timeout — the first draft hung
because a frozen fake clock never met a deadline; documented in-suite).
**Self-review + KAT catches this round (all fixed pre-gate):** (1) the
kn_ws mid-payload timeout was NOT resumable in the first draft (ws_take
lost its partial count → the retry restarted the header parse inside the
payload; frame-state fields added); (2) RT_FOR_LINES nested two for-loops
→ body ran per CHARACTER (invisible on CRLF fixtures — the trailing-\r
trim masked it; the LF-only KAT caught it); (3) the parser searched the
status line across the WHOLE header block (a header's colon-space could
fake a status; the GARBAGE KAT caught it); (4) blank SDP lines aborted
the attribute scan instead of being skipped (the `;;` quirk KAT caught
it); (5) the test-side scripted responses needed sequential CSeq values
(a fixed CSeq exposed the mismatch paths as hangs/fails — the suite now
doubles as the counter-increment proof); (6) a test buffer sized 64 for a
67-byte worst case (-Wformat-truncation at -O1; R28 discipline applies
to tests too → 96).
**Gates (sandbox):** native **12 suites, 1183 checks PASS** (972 + 84 +
127); cross mingw x64+i686 `-Werror` PASS; ASan+UBSan+LSan clean on both
new suites (the single runtime error remains the vendored sha1.c:26
padding note — zero streamer-code diagnostics); `-Wformat-truncation=2`
clean -O0..-O3 on kn_ws.c/kn_rtsp.c/test_knrtsp.c; CMake ctest **14/14**
both TLS backends; T10 POSIX dry-run ALL PASS **60**.
**Awaiting:** target T10 r15 (rebuild + the two new suites; expect ALL
PASS **71**). No T13/GPU rerun needed (no socket/graphics code touched).
Round XXI then wires the production path (Next steps 1a-1e).
→ `streamer/{include/kn,src}/kn_ws.*` (new), `streamer/tests/test_knws.c`
  (new), `streamer/{include/kn,src}/kn_rtsp.*` (new),
  `streamer/tests/test_knrtsp.c` (new), `CMakeLists.txt`,
  `tools/{build_native_tests,cross_check}.sh`,
  `tests/T10_build_and_test.py` (12 suites), `docs/RTSP.md` (§8 +
  deviations D-w1..D-r4), `INTEGRATION.md` rev 11, this file

### 2026-10-25 · impl round XXI (T10 r15 ALL PASS 71 → the stage-2 PRODUCTION SIGNALING PATH: TLS stream + kn_json/kn_http/kn_cm + fake-rig E2E + knvstd --session)
**Context:** same session (user: "zajmij się wszystkim co podałeś do rundy
XXI"). User delivered T10 r15 first: **ALL PASS 71/71** — the round-XX
cores (test_knws, test_knrtsp) passed on Win7/GCC 16.2 first try;
archived `artifacts/T10-round15/`. Then this round implemented the whole
remaining plan from docs/RTSP.md §6 (stages 3-5) + the OAuth research.
**OAuth research (docs/AUTH.md, new):** the login is the OAuth 2.0 DEVICE
grant (RFC 8628) against login.nvidia.com — `device/authorize` (form:
client_id/scope/device_id/display_name/idp_id; Steam-Deck browser
identity headers) → `token` poll (grant_type device_code; pending/slow_
down+5s/expired/access_denied) → access_token + **id_token (the GFNJWT
service token** — `service_token()` = id_token ?? access_token, gfn.rs
:254) + refresh (10-min window). CloudMatch auth: `Authorization: GFNJWT
{token}` + the NATIVE identity headers (docs/AUTH.md §4). Implementation
stays stage 4 (TUI); stage-2 bring-up = pasted `--session-token`
(length-only logging). NOT VERIFIED: the device-authorize RESPONSE field
names beyond device_code/interval (measure at stage 4).
**Round-XXI delta (all sandbox-proven; docs/RTSP.md §9 is the record):**
- **`kn_net_tls_stream_*`** — the persistent verified TLS-1.2 stream
  (Win32+mbedTLS): the T12-proven policy object verbatim (VERIFY_
  REQUIRED, SNI, ROOT stores, min=max 1.2, PSA), port-parameterized TCP
  (RTSPS = 322!), per-call SO_RCVTIMEO, recv contract >0/0/-1/-2. Other
  builds: honest stubs (exactly one definition set per configuration via
  complementary guards); `--session` refuses loudly without it — NO
  plaintext fallback anywhere. First target compile = T10 r16.
- **`kn_json`** (zero-alloc reader: 1024-node pool, depth 32, raw spans,
  unescape incl. surrogate pairs, int64-exact) + **`kn_http`** (HTTP/1.1
  one-shot: CL/chunked/read-to-EOF, resumable chunked parser, injected
  clock) + **`kn_cm`** (create body field-for-field per build_create_body
  incl. the Content-Type:text/plain quirk + GFNJWT header; poll; the
  session_info extraction SUBSET with the rtspsEndpoints derivation from
  connectionInfo (usage 16 / 14+alp{1,6}), media usage 2|17, queue
  4-source fallback, phase/ready/resumable rules). `test_kncm` (13th
  suite): **91 checks**.
- **`kn_sock_udp_connect`** — kernel peer filter for the media flow +
  getsockname refresh so the ANNOUNCE carries the REAL route source
  address (like the reference's reserved bundle socket). test_knsock
  70→**82**.
- **`tools/fake_rig.py` + `tools/sandbox_session_e2e.c`** — the two-
  implementations rule at the signaling layer: an independent Python rig
  (own WS framing + CloudMatch fixtures) vs the C client over REAL
  loopback sockets: **36/36** through create→poll→WS handshake→OPTIONS→
  DESCRIBE→SETUP→ANNOUNCE(negotiated)→PLAY→handoff(salt 0x123B84 rule,
  GCM256-8 default, SRTP session or honest NO_CIPHER)→connected UDP.
- **`knvstd --session`** (TLS-only): phase A CloudMatch (queue JSONL
  events = the future TUI feed) → B RTSPS/WSS full sequence (nativeRtcOn
  BundlePort enforced, legacy transport refused) → C SETUP sweep +
  D-s1 placeholder ICE identity + negotiated ANNOUNCE + PLAY → D SRTP
  material (DESCRIBE runtime key or local CSPRNG; salt={key_id:024X};
  profile string→enum map for all 8) + connected UDP + kn_rx arena → E
  the round-XVII media loop VERBATIM (dec_* machinery: capacity-2 queue,
  overflow policy, verify-compatible summary — a real session's stdout
  feeds make_rx_capture --verify unchanged). TEARDOWN on exit (1 s,
  mismatch-skip, reference Drop semantics). Token: length-only in logs;
  key material: never (handoff line carries shapes/lengths only).
  New deviations: **D-s1** (placeholder DTLS fingerprint until the
  stage-3 R3 spike — video/SRTP is bundle-independent), **D-cm1**
  (networkTestSessionId null), **D-cm2** (SubSessionId = device id),
  **D-cm3** (R22 subset until stage 4).
**Bugs caught by the gates this round (all fixed pre-handover):** the
kn_http chunked parser stranded its cursor on '\n' under 1-byte delivery
(KAT caught); kn_net.c's TLS-stream stubs needed a top-level <stdio.h>
(POSIX -Werror caught); the E2E's hex expectations mis-computed 1194884
(=0x123B84, not 0x123BC4 — the reference-derived salt rule itself was
right, proven by the unit KATs); a replace-all patch briefly injected
session memsets into run_decode (compile caught, restored — run_decode
is T13-verified and stayed byte-identical).
**Gates (sandbox, final tree):** native **13 suites 1309 checks PASS**;
cross x64+i686 -Werror PASS; knvstd =2-paranoid clean; CMake 0 warnings
+ ctest **15/15** both backends; **T13 ALL PASS 51 / 51 (ASan+UBSan+
LSan) / 63 (mbedTLS+gcm256)**; sandbox_session_e2e **36/36**; T10 dry
**61**.
**Awaiting (round XXII = the R7 measurement round):** target **T10 r16**
(expect ALL PASS **72** = r15's 71 + test_kncm; first compile of the
Win32 TLS-stream body + --session), then the FIRST REAL SESSION attempt
per Next steps 2 (token + appId; every outcome is evidence). T13 r3
optional (unchanged, 61). No GPU rerun needed.
→ `streamer/{include/kn,src}/kn_json.*`, `kn_http.*`, `kn_cm.*` (new),
  `streamer/tests/test_kncm.c` (new), `streamer/include/kn/kn_net.h` +
  `src/kn_net_mbedtls.c` + `src/kn_net.c` (TLS stream),
  `streamer/{include/kn,src}/kn_sock.*` (connect),
  `streamer/src/knvstd_main.c` (--session), `tools/fake_rig.py` (new),
  `tools/sandbox_session_e2e.c` (new), `docs/AUTH.md` (new),
  `docs/RTSP.md` §9, `CMakeLists.txt`, `tools/{build_native_tests,
  cross_check}.sh`, `tests/T10_build_and_test.py`, `INTEGRATION.md`
  rev 12, `artifacts/T10-round15/` (gitignored), this file

### 2026-10-25 · impl round XXII (T10 r16 build FAIL → root-caused, fixed, and the gate gap closed PERMANENTLY; project staged for a fresh-agent handoff)
**Context:** user ran T10 r16 on the target: **FAIL (2/72)** — `ninja
build` failed compiling `kn_net_mbedtls.c` (`'port' undeclared` at :453
inside `kn_net_dns_lookup`) and `test_kncm` cascaded (never linked).
Evidence excerpt archived: `artifacts/T10-round16/`.
**Root cause (ONE bad round-XXI edit):** the `kn_tcp_open` →
`kn_tcp_open_port` refactor was applied with a Python `str.replace`
WITHOUT a count — the `getaddrinfo(host, "443", …)` pattern occurred
TWICE in the file, so the port-parameterized snippet also landed in
`kn_net_dns_lookup`, which has no `port`. **Why every sandbox gate missed
it:** kn_net_mbedtls.c's Win32 body is never compiled in the sandbox —
POSIX cmake builds take the file's `#else` stub section and cross_check.sh
did not compile the file at all (no mbedTLS existed as a cross
dependency). The target's GCC 16.2 was the first compiler to see that code
this round. The two-tier gate (R26/R28) worked exactly as designed — this
is the fourth target-catch incident and each one has tightened a gate.
**Fixes:** (1) `kn_net_dns_lookup` restored to the round-XV-verified
`getaddrinfo(host, "443", …)`; (2) **a SECOND latent bug surfaced by the
new cross-compile**: the round-XXI TLS-stream body used calloc/free
without `<stdlib.h>` — x64 mingw headers dragged stdlib in transitively,
**i686 did not** (target GCC happened to as well; still a landmine) →
explicit include added with a comment; (3) **the gap is closed
permanently**: `cross_check.sh` now cross-compiles `kn_net_mbedtls.c`
(Win32+mbedTLS body) on BOTH arches whenever `third_party/mbedtls` is
present (gitignored tree; skipped honestly when absent). Verified with
-std=c11 AND the exact target flag set (gnu11 -O3 -DNDEBUG -march=core2
-msse3 -Wall -Wextra -Werror): clean.
**Process lesson (recorded where it will be seen): NEVER use uncounted
str.replace on a file with repeated patterns, and treat "code the sandbox
cannot compile" as a standing risk that needs its own gate** — the
cross_check addition is that gate for the last such file.
**Full sandbox re-verification after the fixes (final tree):** native 13
suites PASS (1309 checks); cross PASS incl. the new kn_net_mbedtls legs;
sandbox_session_e2e **36/36**; T13 ALL PASS **51 (FFmpeg) / 51
(ASan+UBSan+LSan) / 63 (mbedTLS+gcm256)**; CMake FRESH reconfigure both
backends: 0 warnings, ctest **15/15 ×2**; T10 POSIX dry-run ALL PASS 61.
**HANDOFF STATE (user is moving to a fresh chat window — this file + the
repo are the complete context; no chat history needed):** everything
through round XXII is in the workspace tree; the user commits/pushes
(the agent cannot). A fresh agent: read THIS file's Bootstrapping section,
clone the repo + OpenNOW @ a49b124, reinstall the sandbox toolchain
(does NOT persist between messages — R26), run the two gate scripts, and
continue at Next steps 1 (target T10 r17, expect ALL PASS 72) then 2 (the
R7 first-real-session round: needs a GFNJWT token + numeric appId from
the user; docs/AUTH.md §3 documents the device flow; every outcome of
that run is a recorded measurement — D-s1/D-cm1/D-cm2/R34-firewall all
get their answers there). Round-XXI deliverables awaiting that first
compile-on-target were partially de-risked this round: the kn_net_
mbedtls.c Win32 body (incl. the TLS-stream implementation) now compiles
clean under the exact target flags in the sandbox.
→ `streamer/src/kn_net_mbedtls.c` (both fixes), `tools/cross_check.sh`
  (the permanent kn_net_mbedtls cross-compile), `artifacts/T10-round16/`
  (evidence, gitignored), this file

### 2026-10-25 · impl round XXIII (R7 runs 1-4: the FIRST real CloudMatch traffic → the control-host bug FOUND+FIXED; the poll loop hardened to the reference's semantics)
**Context:** same handoff chain (this file + the repo). The user ran T10
r17 on the target (**ALL PASS 72** — the round-XXII fixes confirmed, the
Win32 TLS-stream body + --session compiled clean on GCC 16.2 first try),
then executed the R7 first-real-session plan: a GFNJWT id_token pulled
from the browser + appId 107057914 → four consecutive live `knvstd
--session` runs against the PRODUCTION backend, plus a parallel DevTools
capture of the official WebRTC client and a full captured poll-response
body. This round is the analysis + fix; the record lives in
**docs/RTSP.md §10**.
**The measurement (runs 1-4):** create works — every run produced a real
sessionId, status 1, and a plausible live queuePosition (167/166/181/178
while the browser saw 161 in the same window; D-cm1 networkTestSessionId
null + D-cm2 SubSessionId=device-id ACCEPTED at create, 4/4). The queue
wait failed: the position FROZE after 0-1 updates (run 2: `163` ×40
polls; run 3: `176` ×30) and every run ended in
`not_ready/cm_poll_failed` after 2-43 polls. The browser capture: the
official client polls the REGIONAL node
`https://np-waw-01.cloudmatchbeta.nvidiagrid.net/v2/session/{id}` every
~4.1 s and its position MOVES (161→151→147→135 in 10 s); the captured
body nominates that node in `sessionControlInfo` (ip/resourcePath/
usage 10/port 443).
**Root cause (ONE contract violation):** phase A polled the CREATE host
(`prod.cloudmatchbeta.nvidiagrid.net` — the reference's
DEFAULT_STREAMING_URL, gfn.rs:38) for the whole wait. The reference
learns `control_base = https://{sessionControlInfo.ip}` (cloudmatch.rs
:1966-1984, trust-gated :2400-2456) at create (:401-414) / attach
(:1149-1152) and EVERY poll goes ONLY there (:652-656 → get_session
:1356). The global front answers with stale snapshots + intermittent
non-200s — exactly the freeze-then-fail signature.
**The fix + hardening (all reference-mirrored):** `kn_cm_session` learns
control_host/port + requestStatus code/desc (filled even on failed
extract — a rejected payload now says WHY, e.g. statusCode 11
SESSION_LIMIT :2354+); NEW `kn_cm_control_base` (the trust mirror of
trusted_learned_server_base + public_session_host: https-only, 443,
private/loopback/link-local/CGNAT/multicast/reserved/.local/userinfo/
path rejected — a response can never redirect polling into a private
address; 0 → create-host fallback = the :1984 unwrap_or semantics, which
also keeps the loopback fake-rig E2E unchanged); NEW `kn_cm_cancel`
(DELETE v2/session/{id} = stop_at :789-800, validate_delete_response
:2728 — 2xx/404 = gone); phase A: the poll-host switch (+ the
`cm_control` evidence event), cadence 2 s → 4 s (the capture's Δ≈4.1 s),
3-attempt transient retry (408/425/429/5xx, 250/750 ms — get_session
:1369-1394), 404 → session_not_found, statusCode≠1 → failed round with
the cloud's description (`cm_poll_error` events), failed rounds NEVER
clobber the live session state (poll-into-temp + commit — the old loop
let one bad extract wipe session_id), 4 consecutive bad rounds tolerated
before cm_poll_failed, `not_ready` carries rc+http_status, best-effort
`cm_cancel` on every not_ready exit except 404 (runs 1-4 each ORPHANED a
queued session — no cancel existed then), `cm_created` includes the
initial queue_position, `--session-queue-timeout` default 180 → 600 s.
**Gates (sandbox, final tree):** native 13 suites PASS — test_kncm
**91 → 147 checks** (control-base battery incl. the exact R7 capture
fixture + cancel wire checks; also clean under ASan+UBSan+LSan); cross
x64+i686 -Werror PASS; kn_cm.c + knvstd_main.c clean under
-Wformat-truncation=2 (-O1 host, -O2 mingw both arches); CMake fresh
both backends 0 warnings + ctest 15/15 ×2; sandbox_session_e2e
**36/36** (exercises the FALLBACK branch — the rig's 127.0.0.1 control
host is rejected by design; the ACCEPT path is unit-pinned); T10 POSIX
dry-run ALL PASS **62** — 62 is also the pre-delta baseline on this tree
(verified by a stashed rerun; the round-XXI "61" predates a since-added
check).
**Awaiting (round XXIV):** target T10 r18 (expect ALL PASS 72), then
**R7 run 5** — the same command; expect `cm_control` with
`source:"sessionControlInfo"`, a MOVING queue_position, and if a seat
arrives the first real phase B-E (D-s1's first answer). Known gaps
carried: phases B-E aborts don't cancel yet; IPv6 trust checks are
prefix-class approximations (documented).
→ `streamer/{include/kn,src}/kn_cm.*`, `streamer/src/knvstd_main.c`
  (phase A), `streamer/tests/test_kncm.c`, `tools/fake_rig.py` (DELETE
  branch), `docs/RTSP.md` §10, `INTEGRATION.md` rev 13, this file

### 2026-10-25 · impl round XXIV (R7 run 5: the control-host fix CONFIRMED LIVE — first READY session + first real RTSPS traffic; DESCRIBE rc:-2 root-caused to the 8 KiB per-message scratch → fixed + binary frames + Host:port + rtsp_detail)
**Context:** same handoff chain. The user applied the round-XXIII patch
on the target and reran the SAME live command (real GFNJWT + appId
107057914). The run is the round-XXIII fix's confirmation AND the next
measurement: docs/RTSP.md §11 is the full record.
**Run 5 evidence (live production cloud):** `cm_control:
poll_host=np-waw-01.cloudmatchbeta.nvidiagrid.net, port=443,
source=sessionControlInfo` — the learned-control-node switch fired. The
session PROGRESSED through the real queue: `queue_position:81,
seat_step:1` ×29 polls (~2 min), then `seat_step:3` with queuePosition
gone, then `status:2 ready` with `rtsps://80-84-174-149.cloudmatchbeta.
nvidiagrid.net:322` (media_peer ":0" — the ready response carried no
usage-2/17 connection; honest, the SETUP sweep provides the video peer).
Phase B then worked at the transport level against a REAL GFN rig: the
WS handshake (x-nv-sessionid) + `OPTIONS rc:0 status:200`. The run died
at `DESCRIBE rc:-2 status:0` (KN_RTSP_ERR_IO, no response parsed).
**Queue-position observation (recorded, NOT a defect):** the NATIVE
session's position did not tick per poll — the server published step
transitions (1→3→ready) instead; the user's moving browser numbers were
a WebRTC-transport session. Client-side correctness is proven by the
transitions + ready. Keep observing.
**Diagnosis (three real deviations from nvst_rtsp.rs; without wire logs
#1 and #2 are indistinguishable as THE run-5 cause — both yield exactly
rc=-2/status=0 at the first big response — so all three are fixed):**
(1) `kn_rtsp_request` read each WS message into a `char scratch[8192]`
— a real DESCRIBE SDP routinely exceeds 8 KiB while OPTIONS responses
are a few hundred bytes (the observed signature); the reference has no
per-message cap below the 4 MiB total (tungstenite reassembles whole
messages, :559-566, and appends text AND binary to one buffer,
:614-623). (2) The adapters refused BINARY frames the reference accepts
(:621). (3) The Host header lacked the port (the reference strips only
the scheme, :1070-1076 — the rig tolerated it for OPTIONS, fidelity is
the R32 rule).
**Fixes:** kn_rtsp_request reads DIRECTLY into the accumulation buffer's
tail (seam contract unchanged, runaway-shim guard kept); both C adapters
accept TEXT|BINARY; knvstd + E2E build the Host authority from the rtsps
endpoint (host:port); NEW `sess_rtsp_detail` evidence line at all five
step-failure sites (kn_rtsp err[160] + ws_err/ws_close/ws_text/ws_bin —
a server close now shows as ws_err:-7 + the close code instead of a bare
-2); the fake rig delivers its DESCRIBE SDP padded to ~11 KiB as TWO
binary messages (the run-5 shape over real sockets).
**Gates (sandbox, final tree):** native 13 suites PASS — test_knrtsp
**127 → 132** (a >19 KiB single-message DESCRIBE parses — the old
scratch died here; the same response split across two messages
accumulates), test_kncm 147; cross x64+i686 -Werror PASS; paranoid
-Wformat-truncation=2 clean (host -O1 + mingw -O2 both arches) on every
touched .c; CMake fresh both backends 0 warnings + ctest 15/15 ×2;
sandbox_session_e2e **36/36** with the NEW rig behaviour (binary +
split + padded SDP; the rig still asserts the announce key echo +
plays=1); T10 POSIX dry-run ALL PASS 62 (baseline).
**Awaiting (round XXV):** the next target T10 (expect ALL PASS 72),
then **R7 run 6** — expect DESCRIBE 200 → the SETUP sweep → ANNOUNCE
(D-s1's first real verdict) → PLAY → the media loop; any failure now
names its stage in rtsp_detail. Known gaps carried: phases B-E aborts
still don't DELETE the session (round XXV candidate); D-cm1/D-cm2
remain measured-at-create-only.
→ `streamer/src/kn_rtsp.c` (direct accumulation),
  `streamer/src/knvstd_main.c` (binary acceptance + Host authority +
  rtsp_detail), `tools/sandbox_session_e2e.c` (mirrored),
  `tools/fake_rig.py` (padded binary split DESCRIBE),
  `streamer/tests/test_knrtsp.c` (test_big_messages), `docs/RTSP.md`
  §11, `INTEGRATION.md` rev 14, this file

### 2026-10-25 · impl round XXV (R7 run 6: DESCRIBE/SETUP/ANNOUNCE 200 LIVE → PLAY 455 root-caused: the missing pre-PLAY hole-punch burst → the STUN toolkit + credential ladder + pump (D-s2) + a caught sdp_attribute LAST-match bug)
**Context:** same handoff chain. The user applied the round-XXIV patch,
rebuilt on the target and reran the live command (run 6). The result is
the project's deepest real-cloud evidence yet — and one new contract
violation, found and fixed. docs/RTSP.md §12 is the record.
**Run 6 evidence (live production):** cm_control → np-waw-01
(source sessionControlInfo); queue 63→65 then static, seat_step 1→3,
status:2 ready (the native-queue position pattern of run 5 repeated —
server behaviour, not a defect); rtsps://80-84-170-155...:322; WS
handshake OK; **OPTIONS 200 → DESCRIBE 200 (the round-XXIV 8 KiB-scratch
fix confirmed live) → SETUP 200 (peer 80.84.170.155:5004, first sweep
round, bundle_fb 0) → ANNOUNCE 200 (4164-byte negotiated SDP — the D-s1
placeholder ICE/DTLS identity was ACCEPTED at signaling level) → PLAY
455** "Method Not Valid in This State" with a healthy WS layer
(rtsp_detail: ws_err 0, ws_close -1, ws_text 5, ws_bin 0 — the round-XXIV
instrumentation doing exactly its job). Afterwards the user saw the
session as RESUMABLE in the GFN web UI and played it in the browser:
**R23 RESUME is proven real**, and leaving the session alive after a
phase-B-E abort (reference parity) is the right default.
**Root cause (PLAY 455):** the reference never sends PLAY straight after
ANNOUNCE — the engine spawns the media receivers in between (lib.rs:894
announce → :937/:990 receiver spawn → :1117 finish/PLAY) and the bundle
thread fires its hole-punch burst IMMEDIATELY (last_hole_punch pre-armed,
nvst.rs:7438): 3× STUN binding request + 1× NATT hole-punch ping per
20 ms (PING_INTERVAL_BEFORE_CONNECTION :242, burst :7842-7900) at
bundle_peer() = CloudMatch bundle peer else the SETUP video peer (:1880),
answering the rig's binding requests on the way (:5151-5172). knvstd sent
PLAY with zero UDP egress → the rig's state machine refused.
**Round XXV delta:** kn_sock grew the STUN toolkit (crc32; stun_build =
byte-exact port of build_authenticated_stun_packet :4977-5012 incl. the
RFC 5389 §10.5 MI length adjustment + FINGERPRINT; xor_addr_v4; inspect
= the :5131-5146 gate with the MANDATORY fingerprint; check_integrity
:139-160 const-time — all KAT'd against goldens from an independent
Python implementation + RFC 2202). kn_rtsp grew the credential ladder
(increment_hex :2305, bundle_natt_username :1327, resolve_remote_ufrag
:2329; describe extracts ufrag_v1 + server_bundle_port; fixtures
transcribed from the reference's own tests :3860-3920). **The ladder
fixtures caught a pre-existing bug: kn_rtsp_sdp_attribute returned the
FIRST ";;" -segment match; the reference keeps the LAST (.last() — its
test :3861 pins 47998-then-;;47999 → 47999). Fixed + pinned**
(media_control legitimately stays first-segment-only, :1494-1501).
knvstd phase C/D: the ice_creds evidence line (lengths only, §7), the
credential gate (:1729; the ping6-without-payload hard error :1248
becomes deviation D-s3: skip the pump honestly, still try PLAY), the
bundle-peer re-connect/restore, sess_ice_pump (the sequential form of
the reference's first receiver iteration — 20 ms bursts, answers
requests, counts successes), PLAY 455-retry (3 attempts, 600 ms re-pump
— deviation D-s2, the reference is concurrent), the ice_preplay counters
line, `attempt` in the PLAY event. The rig now runs an independent
Python STUN validator on the media socket and GATES PLAY exactly like
run 6 (first PLAY always 455; later ones need ≥1 validated request);
the E2E pumps + asserts the ladder and the 455→200 sequence.
**Gates (sandbox, final tree):** native 13 suites PASS — test_knsock
**82 → 98**, test_knrtsp **132 → 150** (the ladder + last-wins pins);
cross x64+i686 -Werror PASS (test_knsock now links kn_sha1+sha1 — both
gate scripts updated); ASan+UBSan clean on both touched suites (the
single known vendored sha1.c:26 note aside); kn_rtsp.c + knvstd_main.c
paranoid -Wformat-truncation=2 clean (the two kn_sock.c =2 hits are
PRE-EXISTING round-XVII error strings — standard gates clean, left
alone); CMake fresh both backends 0 warnings + ctest 15/15 ×2;
sandbox_session_e2e **43/43** (rig evidence: ice_req 12, ice_resp 12,
play_455 1, plays 1 — one rig-side FP-offset bug caught by the first
E2E run and fixed pre-gate); T10 POSIX dry ALL PASS 62 (baseline).
**Awaiting (round XXVI):** the next target T10 (expect ALL PASS 72),
then **R7 run 7** — PLAY 200 (attempt 1 or after one 455), the handoff
line, and the first real Mjolnir SRTP → kn_rx → AU frames (R32's final
stage-2 gate + the R34 inbound-UDP firewall answer). If PLAY still 455s:
ice_preplay.ok_in (did the rig answer?) + ice_creds (which ladder leg
fired) localize it. Known gaps carried: the post-PLAY STUN/RTCP/SCTP
plane + the real DTLS identity = stage 3 (R3); whether the real rig's
455 gate wants a VALIDATED binding vs any packet is run-7's measurement.
→ `streamer/{include/kn,src}/kn_sock.*` (the STUN toolkit),
  `streamer/{include/kn,src}/kn_rtsp.*` (the ladder + the LAST-match
  fix), `streamer/src/knvstd_main.c` (pump + retry + evidence lines),
  `streamer/tests/{test_knsock,test_knrtsp}.c`, `tools/fake_rig.py`
  (Python STUN + the PLAY gate), `tools/sandbox_session_e2e.c`,
  `tools/{build_native_tests,cross_check}.sh` (test_knsock sha1 link),
  `docs/RTSP.md` §12, `INTEGRATION.md` rev 15, this file

### 2026-10-26 · impl round XXVI (R7 run 7: the ~2-minute seat-transition 503 storm — the poll policy hardened to the reference's patience; cm_cancel TARGET-PROVEN)
**Context:** same handoff chain. The user ran the round-XXV tree live
twice (run 7, attempts 1-2). Neither reached PLAY — both died in the
QUEUE, in a new, cleanly-measured cloud-side window. docs/RTSP.md §13
is the record. NOTE: GitHub now carries the work as commits `6475fee`
(patch 24) and `f1b4399` (patch 25) — round XXVI sits on f1b4399.
**Run 7 evidence (both attempts, real cloud):** cm_control → np-waw-01;
~29 polls (~2 min) of STATIC `queue_position:53/57, seat_step:1` — the
same ~29-30-poll step-1 duration runs 5/6 showed BEFORE transitioning
to seat_step 3 — then five consecutive failed rounds with every HTTP
attempt answering **503** (rc:0 — parsed responses, not transport; 15
attempts per run incl. the in-round 250/750 ms retries) → cm_poll_failed
→ **cm_cancel http_status:200 — a DELETE to the SAME host in the SAME
window while every GET 503'd.** The user watched the session disappear
from the GFN web UI (round-XXIII cancel proven on the target) and the
immediate re-create hit no SESSION_LIMIT.
**Analysis:** three runs agree on the clock — the ~2-minute mark is
when the seat-assignment machinery acts on the record. Runs 5/6: a seat
was found → step 3 → ready. Run 7 (twice, two sessions): the GET path
503'd at exactly that mark while the session-store path (DELETE) stayed
healthy — the consistent reading is an overloaded/timing-out seat
upstream (503 = the classic proxy answer for it; Sunday-evening WAW
pool), with the record very likely alive behind it. The round-XXIII
"5 bad rounds → dead" bound (~30 s) killed the wait inside the very
window the transition needs. Contract check: the reference NEVER gives
up on transient poll errors — get_session retries per call (:1369-1394)
and the UI cadence simply continues; only 404 clears (:664-671).
**Fix (round XXVI — the failure-class policy, deviation D-s4; knvstd
phase A only, no library/API changes):** TRANSIENT statuses
(408/425/429/500/502/503/504) are OUTLIVED until queue_timeout (600 s
default, flag-extendable; Ctrl-C/timeout still cancel — proven);
transport deaths bounded at 10 consecutive rounds (cm_poll_failed);
hard rejections (other non-200, or 200 with statusCode!=1) bounded at 5
(NEW stop_reason cm_poll_rejected — an auth death must not burn 10
minutes); the poll gap backs off 4→6→9→13.5→20 s cap while failing
(sess_poll_gap), reset on success; cm_poll_error now carries `"body"`
(a bounded escaped excerpt of THIS round's response — cm_json is
cleared per round so it can never be stale; 503 bodies usually name
their sender), `"class"` and `"gap_ms"`.
**Gates (sandbox, final tree):** native 13 suites PASS (knsock 98,
knrtsp 150, kncm 147 — libraries unchanged); cross x64+i686 -Werror
PASS incl. the kn_net_mbedtls legs; paranoid -Wformat-truncation=2
clean on knvstd_main.c (host -O1 + mingw -O2 ×2); sandbox_session_e2e
43/43; CMake fresh both backends 0 warnings + ctest 15/15 ×2; T10
POSIX dry ALL PASS 62. Sandbox toolchain note: apt hung twice this
round (the aliyun mirror) — recovered via --print-uris + deb.debian.org
downloads + dpkg (R26's fallback, extended: rewrite the mirror host).
**Awaiting (round XXVII):** the next target T10 (expect ALL PASS 72),
then **R7 run 8** — the round-XXV PLAY pump has STILL never seen a live
run; expect either the step-3 transition → ready → ice_creds/
ice_preplay → PLAY(455?)→200 → the first real Mjolnir media, or a
ridden-out 503 window whose body excerpts finally name the sender.
Known gaps carried: phases B-E aborts leave the session resumable
(reference parity, run-6-proven useful); the post-PLAY STUN/RTCP/SCTP
plane + the real DTLS identity = stage 3 (R3).
→ `streamer/src/knvstd_main.c` (the phase-A poll policy only),
  `docs/RTSP.md` §13, `INTEGRATION.md` rev 16, this file

### 2026-10-26 · impl round XXVII (R7 run 8: the policy RODE the 503 window; the body revealed an ip:null session skeleton — now parsed; the T10-r20 warning fixed + gated permanently; control-node re-learning; --session-keep)
**Context:** same handoff chain. The user ran the round-XXVI tree live
(run 8) after T10 r20 FAILED on exactly one check: `zero compiler
warnings (1 seen)` — target GCC 16.2 flagged `consec_transient`
(write-only: the transient class has no bound by design) via
`-Wunused-but-set-variable`, which sandbox GCC 12 does NOT include in
`-Wall`. The fifth target-catch incident (R28's pattern held: every
catch tightens a gate). docs/RTSP.md §14 is the record.
**Run 8 evidence (live cloud):** 12 polls of static
`queue_position:80/seat_step:1`, then the 503 window — and the
round-XXVI policy performed EXACTLY as designed: ten
`class:"transient"` rounds ridden out with the D-s4 backoff visible in
the evidence (`gap_ms` 6000→9000→13500→20000 cap), until the user's
Ctrl-C (~5 min) → `stop_reason:"interrupted"` → `cm_cancel` 200. The
session then correctly did NOT appear in the web UI (a DELETEd
queue-stage session is gone; run 6's browser-resume worked because
READY sessions survive phase-B-E aborts — reference parity).
**The 503 body TALKED (the excerpt instrumentation earned its keep):**
`{"session":{"sessionAds":null,"sessionControlInfo":{"protocol":0,
"appLevelProtocol":5,"ip":null,"resourcePath":"/v2/session/479ecbcd…`
— CloudMatch's OWN JSON, a session SKELETON with
`sessionControlInfo.ip:null` while the resourcePath still echoes the id.
Reading: the front knows the record but cannot presently name the
node/state owning it — the §13 seat-transition window seen from inside;
the record is ALIVE (the same-window DELETE answered 200). The deep
fields (status/errorCode/requestStatus) sit past a 160-char excerpt —
so this round parses them.
**Round XXVII delta:** (1) `consec_transient` removed + **both gate
scripts now run a PERMANENT paranoid knvstd leg** (`-Werror
-Wunused-but-set-variable -Wformat-truncation=2`, host + x64 + i686) —
the class can never reach the target again. (2) `kn_cm_session` grows
`error_code` + `unified_error`; status/phase/errorCode are filled
BEFORE the hard sessionId gate (rejected payloads stay diagnosable —
the reference's cloudmatch_http_error likewise reads non-200 payloads,
:2377); `cm_poll_error` gains `unified_error`/`sess_status`/`err_code`
+ desc fallback and the excerpt grows to 240 chars. (3) **Control-node
RE-LEARNING (a real fidelity gap closed):** the reference re-derives
streamingBaseUrl from EVERY successful poll and store_active saves it
as the next control_base (:1333-1336 + :1966-1984) — knvstd now re-runs
kn_cm_control_base per successful poll (the trust gate; ip:null keeps
the current host) and emits `cm_control source:"relearned"` on a
change, so a mid-queue node migration is followed. (4) `--session-keep`
skips the phase-A cancel (evidence line `cm_cancel {"skipped":
"session-keep"}`) for the user's browser-RESUME workflow / R23
experiments; default stays cancel (run-7-proven hygiene).
**Gates (sandbox, final tree):** native 13 suites PASS — test_kncm
**147 → 148** (the run-8 skeleton fixture: rejected but status/
errorCode/unified/desc all surface and ip:null yields no control host;
unifiedErrorCode added to the SESSION_LIMIT fixture; errorCode pinned
on the run-5 capture fixture); build_native_tests + cross_check PASS
WITH the new paranoid legs (3 .o); E2E 43/43; CMake fresh both backends
0 warnings + ctest 15/15 ×2; T10 POSIX dry ALL PASS 62.
**Awaiting (round XXVIII):** the next target T10 (expect ALL PASS 72,
zero warnings), then **R7 run 9** — either the seat transition →
ready → the round-XXV pump's FIRST live try (ice_creds/ice_preplay/
PLAY 455→200) → first real Mjolnir media, or a 503 window speaking in
parsed fields (sess_status/err_code/unified_error/cloud_status/desc —
the basis for the next decision: extend the budget, treat long 503s as
a dead record, or re-create), possibly with a `relearned` control node.
Known gaps carried: phases B-E aborts leave sessions resumable (now
also selectable at phase A via --session-keep); the post-PLAY
STUN/RTCP/SCTP plane + the real DTLS identity = stage 3 (R3).
→ `streamer/src/knvstd_main.c` (diagnostics + re-learning + keep),
  `streamer/{include/kn,src}/kn_cm.*` (error_code/unified_error +
  pre-gate fills), `streamer/tests/test_kncm.c` (148),
  `tools/{build_native_tests,cross_check}.sh` (the paranoid legs),
  `docs/RTSP.md` §14, `INTEGRATION.md` rev 17, this file

### 2026-10-26 · impl round XXVIII (R7 run 9: re-learning LIVE — the control front migrates constantly and the 503 class vanished; ICE fully bidirectional yet PLAY still 455 → the pump goes CONCURRENT, PLAY ONE-SHOT, the Mjolnir NATT keepalive lands)
**Context:** same handoff chain; GitHub now carries the work through
`8f97bb8` (patch 27 = the round-XXVI+XXVII trees applied verbatim —
verified byte-identical to the sandbox-gated state before this round).
The user ran T10 r21 (**ALL PASS 72, zero warnings** — the XXVII
paranoid-leg fix confirmed) and then run 9 live. docs/RTSP.md §15 is
the record.
**Run 9 evidence (live production):** (1) the XXVII control-node
re-learning fired ~25 times in one queue wait — the nominated front
migrates across np-ams-06/07, np-sth-04, np-lon-06/07, np-par-05,
np-frk-07 nearly every poll, every node answered, and **the 503 class
of runs 7/8 disappeared entirely** (zero 503s across ~28 polls; clean
step 1→3→ready). Retro-reading: runs 7/8 were us PINNED to np-waw-01
after the record drifted — the ip:null skeleton was that node saying
"not mine to serve". (2) Queue position sat at a `100` cap on every
node with one `23` glimpse mid-migration — recorded as display
semantics, not a defect. (3) ice_creds enabled:1 through the
pingVersion-6 bundle-username ladder leg (all lengths 13 — the real
DESCRIBE carries iceUsernameFragment + serverBundlePort; the ladder
fixtures from the reference's own tests matched reality). (4)
**ice_preplay: stun 147, natt 49, req_in 99, answered 99, ok_in 327,
invalid 0, send_errors 0** — the round-XXV STUN toolkit is live-proven
byte-correct in BOTH directions with the real rig. (5) **PLAY 455
anywhere**; the retry hit a dead WS (rc -2, ws_err -2, ws_close -1,
ws_text stuck at 5 — the rig drops the RTSP context after refusing).
**Conclusions:** burst existence is necessary (run 6) but not
sufficient (run 9). Remaining deltas vs the reference at PLAY time:
(a) the sequential pump goes QUIET during the PLAY exchange — the
reference's receiver thread keeps bursting every 20 ms through it
(lib.rs:894→937→1117; nvst.rs:7438/:7842); (b) D-s1's placeholder DTLS
identity — unproven either way (invalid_in:0 — the rig sent no DTLS
during the window). And re-PLAY after a 455 is pointless (dead
channel) — the reference has no in-place retry at all.
**Round XXVIII delta (knvstd_main.c only + docs):** the pump moves to a
bounded worker thread (sess_pump ctx + sess_ice_pump_run core;
kernel32 CreateThread — F1 import set, no new DLLs; POSIX pthread for
the sandbox build) spanning exactly the PLAY request/response window,
joined before the media loop (single-owner socket discipline: main
touches only WS/TLS while it runs; stats read after join; **D21's
single-threaded MEDIA loop untouched** — the thread is gone before the
first media tick); spawn failure falls back to the sequential 400 ms
pump honestly. PLAY is ONE-SHOT (the 455-retry loop is deleted;
failure → TEARDOWN + the resumable-session policy). ice_preplay gains
"thread". The media loop gains the reference's **Mjolnir NATT
keepalive** (nvst.rs:8849-8905 + the test :14549's shape: USERNAME
"{setup x-nv-ping-payload}:{local ufrag}", key = the DESCRIBE remote
password, to the video peer, 20 ms until the first inbound datagram
then 100 ms, inline in the loop's 10 ms tick) — the reference's
comment is explicit that these pings are what ROUTES video to the
socket, so a PLAY-200 run without them risked media silence; evidence
line natt_keepalive{"sent":N} after the loop (the verify-compatible
summary schema is untouched). cm_control relearn events carry "n", the
ready line the "relearns" total.
**E2E honesty note:** sandbox_session_e2e keeps its OWN sequential pump
+ two-PLAY sequence — it is the independent second implementation of
the WIRE shapes (its rig gate accepts once a request validates); the
thread scheduling is knvstd glue on Win32-target territory (POSIX
session mode refuses at the TLS gate). 43/43 unchanged.
**Gates (sandbox, final tree):** native 13 suites PASS (knsock 98,
knrtsp 150, kncm 148 — libraries unchanged); cross x64+i686 -Werror
PASS incl. the paranoid knvstd legs on three compilers; E2E 43/43;
CMake fresh both backends 0 warnings + ctest 15/15 ×2; T10 POSIX dry
ALL PASS 62. (Workspace note: the agent's .git does not survive the
between-message snapshot; this round re-based onto the user's pushed
8f97bb8 after verifying it byte-identical to the XXVII state.)
**Awaiting (round XXIX):** the next target T10 (expect ALL PASS 72,
zero warnings), then **R7 run 10** — PLAY 200 → the keepalive carries
the routing → first real Mjolnir SRTP → kn_rx → aus/frames (R32's
final stage-2 gate + the R34 inbound-firewall answer); OR PLAY still
455 with "thread":1 + rich counters → the DTLS identity (D-s1) is the
prime suspect and the stage-3 R3 spike becomes the PLAY prerequisite
(the honest next milestone either way).
→ `streamer/src/knvstd_main.c` (the pump thread + one-shot PLAY + the
  media-loop keepalive + the relearn counters), `docs/RTSP.md` §15,
  `INTEGRATION.md` rev 18, this file

### 2026-10-26 · impl round XXIX (R7 run 10: the pump thread live-proven through the whole PLAY window; PLAY went 455→SILENCE on a second rig → the elimination table is empty except D-s1 — R3 PROMOTED to the stage-2 PLAY blocker with docs/DTLS.md; the official ICE identity shapes mirrored)
**Context:** same handoff chain. The user ran the round-XXVIII tree
live (run 10) after T10 confirmed ALL PASS 72 / zero warnings.
docs/RTSP.md §16 (the measurement + the elimination table) and the NEW
**docs/DTLS.md** (the R3 spike plan) are the records.
**Run 10 evidence (live production):** queue — re-learning ×24 (the
`ready` line's new `relearns:24`), the 97 cap with real glimpses
(17/16/5 — the queue moved under the cap), zero 503s again, clean
step 1→3→ready. RTSP — OPTIONS/DESCRIBE/SETUP/ANNOUNCE 200 (peer
80.250.98.40:5004; ANNOUNCE 4165 B). ICE — `ice_creds enabled:1`
(identical ladder shape to run 9: all lengths 13) and the pump thread
PERFORMED: `"thread":1`, `pump_ms 20525` (it spanned the ENTIRE 20 s
PLAY timeout), 2760 STUN + 920 NATT sent at the 20 ms cadence, the
rig's 123 probes ALL validated+answered, 3671 rig successes validated
here, invalid 0, send_errors 0. **PLAY: rc -3 timeout, status 0 — NO
response at all** (ws_text stuck at 4 — the fifth RTSP answer never
came; ws_close -1 — the WS stayed open and silent). The run-9 rig
refused instantly (455); the run-10 rig held until timeout. Two rigs,
two refusal modes, one common missing layer.
**Verdict (RTSP.md §16 elimination table):** wire shape — proven
(ANNOUNCE 200 ×3 rigs); bursts before PLAY — closed (run 6→9);
burst continuity THROUGH the exchange — closed (run 10, thread);
credential ladder — proven (invalid 0 both directions, 3671 answers);
**the announced DTLS identity — the ONLY major remaining difference**
(D-s1: a random placeholder fingerprint, no DTLS stack; the reference
announces a real certificate's fingerprint — nvst.rs:5562-5603 — and
runs ICE+DTLS on the bundle from receiver-spawn time, :6106-6160:
client ICE-CONTROLLING, host candidates only, the remote fingerprint
PINNED from DESCRIBE; video SRTP keys stay ANNOUNCE-derived, DTLS
carries SCTP — rtcp1 "a must for One SDK video" :411 — and input).
**R3 is PROMOTED from stage 3 to the stage-2 PLAY blocker.** The plan
(docs/DTLS.md): R3-A kn_cert — a PORTABLE mbedTLS self-signed-cert
module (sandbox-KAT'd on both platforms; Win32 cross-compiled per the
round-XXII rule) whose colon-hex SHA-256 fingerprint replaces the
placeholder in the ANNOUNCE; R3-B kn_dtls — the DTLS 1.2 client on the
bundle socket (fingerprint-pinned, inside the pump thread's state
machine; the N+1 Mjolnir sibling socket when !unifiedSocket — the
:5484 pairing comment); R3-C usrsctp — rtcp1 + input channels (also
unlocks the kn_rx NACK-v2/IDR/RFI senders); R3-D WS control pings
(NvstControlPing :260-290/:844-930 — kn_ws auto-pongs but never pings).
**Round XXIX deltas (while the verdict stands):** (1) the OFFICIAL ICE
identity shapes — generate_gfn_local_ice_credentials (:5545-5559):
4-char ufrag / 22-char password over the base64-ish alphabet; :5564
notes str0m's 16-char default is REJECTED by Bifrost length checks —
our 16/24-hex had been tolerated at ANNOUNCE+ICE (runs 9/10) but
fidelity is the R32 rule and R3 reuses this identity. (2) The 400 ms
pump warm-up before PLAY (run 10 raced the thread start — PLAY could
land before the first burst; run 9's sequential shape had 400 ms of
history; the warm-up removes the last timing confound). (3)
rtsp_detail now carries the full WS counter set (pings_in/pongs_in/
pongs_out/closes_in/frames_out) — run 11 shows what the rig does on
the control channel during a hold.
**Gates (sandbox, final tree):** native 13 suites PASS (libraries
unchanged — the delta is knvstd glue; E2E keeps its own static
identity by design); cross x64+i686 -Werror PASS; paranoid
-Wunused-but-set-variable -Wformat-truncation=2 clean on
knvstd_main.c ×3 compilers; E2E 43/43; CMake both backends 0 warnings
+ ctest 15/15 ×2; T10 POSIX dry ALL PASS 62.
**Awaiting (round XXX = R3-A):** the next target T10 (expect ALL PASS
72, zero warnings) + **R7 run 11** — the EXPECTED outcome is PLAY
still refused, which FINALIZES the elimination (and the enriched WS
counters document the rig's control-channel behaviour during the
hold); either way R3-A (kn_cert) starts next: the first sandbox-
provable piece of the DTLS layer, retiring D-s1's fingerprint half.
→ `streamer/src/knvstd_main.c` (identity shapes + warm-up + counters),
  `docs/DTLS.md` (NEW), `docs/RTSP.md` §16, `INTEGRATION.md` rev 19,
  this file

### 2026-10-26 · impl round XXX (R7 run 11: PLAY refused in TOTAL WS silence with a saturated pump = the FINAL elimination datum → R3-A SHIPPED: kn_cert, the real ECDSA P-256 identity; the ANNOUNCE now carries a fingerprint of a certificate that EXISTS)
**Context:** same handoff chain. The user ran the round-XXIX tree live
(run 11) after T10 confirmed ALL PASS 72 / zero warnings. docs/RTSP.md
§17 is the measurement record; docs/DTLS.md now carries R3-A as
SHIPPED.
**Run 11 evidence (live production, the THIRD rig):** queue — 25
re-learns, the 146 cap with real glimpses (28/48/53 — the queue moved
under the cap), zero 503s (third consecutive clean run — the
re-learning architecture is stable), clean step 1→3→ready. RTSP —
OPTIONS/DESCRIBE/SETUP/ANNOUNCE 200 on 77-111-249-146 (SETUP peer
:5004 first round; ANNOUNCE 4137 B — the round-XXIX 4/22 identity
shapes on the wire). ICE — ice_creds identical (all lengths 13); the
pump thread saturated the whole 20 s window: pump_ms 20920, 2820 STUN
+ 940 NATT, the rig's 126 probes ALL validated+answered, 3753 rig
successes validated, invalid 0, send_errors 0. **PLAY: timeout in
TOTAL control-channel silence — ws_pings_in 0, ws_pongs_in 0,
ws_closes_in 0, frames_out 5 vs ws_text 4: the rig neither answered,
nor pinged, nor closed.** Three rigs, three refusals (455, timeout,
timeout), one constant: no real DTLS behind the announced fingerprint.
The elimination table (§16) is FINAL — the DTLS layer is the only
remaining difference.
**R3-A shipped (docs/DTLS.md phase 1):** NEW `streamer/{include/kn,
src}/kn_cert.{h,c}` — the portable-mbedTLS self-signed identity:
ECDSA P-256 (the str0m provider default), X.509 v3, SHA-256 signature,
FIXED 2020-2035 validity (no wall-clock dependency; the rig pins by
fingerprint, not by CA/date), random 16-byte serial, DER stored
verbatim. The fingerprint = SHA-256(exact DER) in the announced
colon-uppercase-hex form (nvst.rs:5596-5603 ≡ str0m src/lib.rs:1662).
The parsed x509 + pk contexts are kept for R3-B behind opaque handles
(kn_cert_pk/kn_cert_x509 — the header stays mbedTLS-free).
`kn_cert_selfcheck` is the built-in KAT: fp==SHA-256(der), the DER
re-parses byte-identical, and an ECDSA sign/verify roundtrip through
the CERTIFICATE's public key proves the fingerprint binds our private
key. Build discipline (the kn_net pattern, extended): the real body
compiles under KN_TLS_BACKEND_MBEDTLS — which the CMake mbedtls
backend defines on BOTH platforms — so the SANDBOX ctest runs the
REAL battery (test_kncert: 10 checks, ASan+UBSan clean); every other
build gets the honest stub (available()==0, loud refusal — 4 checks);
cross_check compiles the real body with the exact target flags on x64
AND i686 whenever third_party/mbedtls exists (the round-XXII rule).
knvstd: after the 4/22 identity, a real cert is generated when
available and its fingerprint REPLACES the D-s1 placeholder in the
ANNOUNCE; the new `identity` evidence line reports
ufrag_len/pwd_len/fp_len + `"cert":"real"|"placeholder"` (the log
never lies about which identity flew); R3-A frees the cert right after
fingerprinting (R3-B will keep it for the handshake). **ASan caught a
real bug pre-ship: the first test draft double-freed the cert — the
free contract is free(NULL)-safe, NOT double-free-safe (the
kn_net_tls_stream_close convention); test + header doc fixed.**
**Wiring:** CMakeLists (kn_cert.c into kncore; kncert into the test
foreach → ctest 15→16 both backends), build_native_tests.sh (KNCORE +
the stub-mode run, rc18), cross_check.sh (stub -c + the real-body legs
+ the suite link), T10 suites += test_kncert.
**Gates (sandbox, final tree):** test_kncert REAL 10/10 under
ASan+UBSan (mbedtls backend) + stub 4/4 (shell gates); native **14
suites** PASS (knsock 98, knrtsp 150, kncm 148); cross x64+i686
-Werror PASS incl. the kn_cert real-body legs; paranoid knvstd legs ×3
clean; CMake fresh both backends 0 warnings + ctest **16/16 ×2**
(mbedtls: real; schannel: stub — the honest split);
sandbox_session_e2e 43/43 (unchanged — the rig doesn't validate
fingerprints yet; an R3-B-era upgrade candidate); T10 POSIX dry ALL
PASS **63** (62 + the new suite).
**Awaiting (round XXXI):** the next target T10 (expect ALL PASS **73**,
zero warnings), then **R7 run 12** — the first run announcing a REAL
certificate (`identity {"cert":"real"}`). If PLAY 200: watch
natt_keepalive + the summary for the first real media (R32's final
gate). If PLAY still refuses (the leading theory — the gate wants the
actual handshake): **R3-B kn_dtls** is the decisive round (DTLS 1.2
client on the bundle socket, fingerprint-pinning the DESCRIBE remote
fp, inside the pump thread's state machine; the N+1 Mjolnir sibling
socket when !unifiedSocket — docs/DTLS.md).
→ `streamer/{include/kn,src}/kn_cert.*` (NEW),
  `streamer/tests/test_kncert.c` (NEW), `streamer/src/knvstd_main.c`
  (the identity block), `CMakeLists.txt`,
  `tools/{build_native_tests,cross_check}.sh`,
  `tests/T10_build_and_test.py`, `docs/RTSP.md` §17, `docs/DTLS.md`
  (R3-A SHIPPED), `INTEGRATION.md` rev 20, this file

### 2026-10-27 · impl round XXXI (R7 run 12: the REAL fingerprint flew and PLAY STILL refused in total silence — the fingerprint alone is not the gate → R3-B SHIPPED: kn_dtls, the DTLS 1.2 ACTIVE client + the reference's socket pair + the pump-thread handshake state machine)
**Context:** fresh agent session bootstrapped from THIS file + the repo
clone (HEAD `972f85d` = round XXX) + OpenNOW @ a49b124 + the user's
run-12 log paste. The user's previous chat died mid-round; the repo +
this file were the complete recovery path (the continuity rule earned
its keep again). Baseline reproduced FIRST: vendor files hash-verified,
mbedTLS v3.6.0+framework cloned, native 15-suite-equivalent tree green
(14 suites at baseline), cross green, ctest 16/16 — exactly the
round-XXX state before building on it.
**Run 12 evidence (live production, the round-XXX tree):** the FIRST
zero-queue run (queue_position 0 → seat_step 3 → ready immediately,
relearns 23) on a FOURTH rig (80-84-170-153 — run 6's rig was .155,
same subnet). **`identity {"ufrag_len":4,"pwd_len":22,"fp_len":95,
"cert":"real"}` — R3-A live-proved on the target: the ANNOUNCE (200,
4137 B) carried the SHA-256 fingerprint of a REAL ECDSA P-256
certificate** (the whole mbedTLS/PSA keygen path works on Win7/GCC
16.2). ice_creds identical (all lengths 13); the pump saturated the
window a fourth time (thread:1, pump_ms 20921, 2841 STUN + 940 NATT,
126/126 rig probes answered, 3788 rig successes validated, invalid 0).
**PLAY: rc -3 timeout in TOTAL WS silence — byte-for-byte the run-11
refusal shape** (ws_err -8 = recv timeout, ws_close -1, ws_text 4,
ws_frames_out 5, zero pings/closes). **Verdict: announcing a real
fingerprint changed NOTHING — four rigs, four refusals (455, timeout
×3). The gate is the DTLS handshake itself. R3-B green-lit and
SHIPPED this round** (docs/RTSP.md §18 is the full record).
**Discrepancy noted:** the user's T10 said ALL PASS **72**, not the
round-XXX-expected 73 — the target's T10_build_and_test.py predates
the test_kncert suite addition (the repo copy is correct; sync before
the next target run → expect **74** with test_kndtls).
**Reference facts re-read for R3-B (not remembered):**
`start_dtls(true)` = the client is DTLS-ACTIVE (nvst.rs:6300 + str0m
direct.rs:146, init_dtls triggers the ClientHello immediately once ICE
unblocks, str0m lib.rs:1482-1500); the official client GATES the
ClientHello on a real inbound STUN (:8196-8198 — "Do not synthesize
Binding Success"); the socket pair video-N/bundle-N+1 with preferred
49005/49006 and a ≤32-attempt dynamic-adjacent fallback (:5933-6000 —
"the server still relies on this pairing", :5484; some seats don't
route Mjolnir to an arbitrary source port); SETUP's X-GS-ClientPort =
unified?bundle:video (nvst_rtsp.rs:1176-1183); clientBundlePort +
a=candidate = the bundle port (the reference test pins 49006, :2955);
unified = described || bundle_fallback (:1205) and then video_peer =
bundle_peer (:1207-1214); bundle-worker cadence 20 ms (3×STUN+1×NATT)
pre-dtls_ready → 100 ms NATT-only after (:7837-7896); literal PING →
PONG (:8605-8614); demux looks_like_dtls = first byte 20..63 (:6345);
DTLS 1.2 (str0m config default :775); remote fp from DESCRIBE with an
optional "sha-256 " prefix (parse_nvst_fingerprint :5640); Event::
Connected → dtls_ready → start_sctp(true) (:8214-8223 = R3-C's hook);
video SRTP stays ANNOUNCE-derived (:1820-1840 — no DTLS-SRTP).
**Round XXXI delta:**
* NEW `streamer/{include/kn,src}/kn_dtls.{h,c}` — the portable-mbedTLS
  DTLS 1.2 ACTIVE client (real body under KN_TLS_BACKEND_MBEDTLS on
  BOTH platforms, honest stub elsewhere — the kn_cert discipline):
  min=max DTLS 1.2; own cert = the R3-A kn_cert handles; MTU 1200;
  HVR automatic client-side; **the pin as a DOUBLE GATE** (the verify
  callback clears chain flags ONLY on SHA-256(peer DER)==pin;
  kn_dtls_poll REFUSES ready unless verify_result==0 && pin_matched,
  latching KN_DTLS_ERR_PIN + close_notify); **inject-queue BIO** (the
  pump thread stays the socket's single owner: inject() feeds demuxed
  datagrams, the BIO send goes through kn_sock_udp_sendto, an empty
  queue returns WANT_READ — NEVER TIMEOUT); retransmission timers on
  kn_sock_now_ms; post-ready app records drained + counted (D-t4).
* **THREE mbedTLS landmines found, fixed, documented (RTSP.md §18):**
  (1) a BIO recv returning TIMEOUT on an empty queue is read by
  fetch_input as an expired retransmission delay → mbedtls_ssl_resend
  → flight_transmit dereferences a NULL flight (the first test draft
  SEGFAULTED here; ssl_msg.c:2256-2272→:2540); (2) VERIFY_REQUIRED
  without a CA chain is fatal BEFORE any verify callback
  (ssl_tls.c:8071) — fingerprint pinning REQUIRES the
  OPTIONAL+callback+post-gate pattern; (3) a cookie-enabled DTLS
  SERVER surfaces HELLO_VERIFY_REQUIRED from mbedtls_ssl_handshake and
  the app must session_reset + re-set the client transport id before
  continuing (the dtls_server.c example contract — the in-test server
  implements it).
* `knvstd --session` phase C/D/E rework: sess_reserve_pair (preferred
  49005/49006 → dynamic adjacent ×32 → honest failure; the `sock_pair`
  evidence line); the SETUP/ANNOUNCE port routing per the reference
  (above); the cert stays ALIVE (R3-A freed it) and arms
  `kn_dtls_client` when ice+remote_fp+real-cert+backend all exist
  (`dtls_arm` line with the refusal reason + a sanitized detail); the
  pump demux PING→PONG | STUN | DTLS | invalid; the DTLS arm gate =
  the first real inbound STUN (req_in||ok_in); post-ready cadence
  100 ms NATT-only; `dtls_ready`/`dtls_fail` emitted FROM THE THREAD
  at the transition (timing is evidence); the snapshot handshake
  (snap_req/snap_done) for reading counters mid-flight;
  **pump_persist**: in non-unified mode the worker outlives PLAY and
  services the bundle socket through the whole media loop (the
  reference's bundle-worker lifetime; D21 untouched — the media
  pipeline stays single-threaded on its OWN socket); unified mode
  keeps the join-before-media handoff (D-t2); the reconnect dance is
  GONE (one peer per socket for life); teardown order join →
  kn_dtls_free → sockets → kn_cert_free; ice_preplay grew
  pongs/persist + six dtls counters; ice_creds grew `unified`.
* NEW `streamer/tests/test_kndtls.c` (15th suite, self-adaptive):
  REAL battery **31 checks** — the in-test mbedTLS server is shaped
  like the rig (HVR cookies ON, the CLIENT cert REQUIRED+pinned
  server-side = pinning BOTH directions, the same inject-queue BIO
  discipline); legs: full handshake over real loopback UDP,
  DTLSv1.2+ECDHE-ECDSA assertions, first-flight loss in BOTH
  directions (the timer callbacks earn their keep), wrong-pin refusal
  at the ready gate (latched, dirty verify flags preserved),
  inject-queue overflow (drop-new counted), mid-handshake free with a
  full queue (ASan bait), NULL guards. Stub contract 6 checks
  elsewhere. ASan+UBSan+LSan: clean.
* NEW `tools/dtls_openssl_check.py` — the two-implementations rule at
  the DTLS layer (sandbox-only; the target has no openssl): OpenSSL-
  generated ECDSA P-256 cert + `openssl s_server -dtls1_2` + the new
  `test_kndtls --openssl <port> <fp>` client mode → **PASS: an
  independent DTLS stack accepts our ClientHello/HVR-cookie/cert/
  Finished (DTLSv1.2, TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256,
  ready in ~24 ms, 7 records in / 3 out)**.
* Wiring: CMakeLists (kncore += kn_dtls.c; foreach += kndtls; the
  per-test backend define → ctest **17/17 ×2**: mbedtls real /
  schannel stub), build_native_tests.sh (15th suite, rc19),
  cross_check.sh (stub -c + the REAL-body legs for kn_dtls.c AND the
  test's real section ×2 arches + the suite link + KCORE), T10 suites
  += test_kndtls → target expectation **74**.
**Gates (sandbox, final tree):** native 15 suites PASS; cross x64+i686
-Werror PASS incl. all new legs; paranoid knvstd legs
(-Wunused-but-set-variable -Wformat-truncation=2) clean host+cross;
ctest 17/17 ×2 backends; ASan+UBSan+LSan clean; the OpenSSL interop
PASS; sandbox_session_e2e 43/43 (unchanged by design — the rig speaks
no DTLS; an R3-C-era upgrade candidate); T10 POSIX dry ALL PASS 64;
T13 ALL PASS 63.
**Deviations (new, numbered):** D-t1 (no str0m ICE agent — the
reference's OWN hand-built bursts are USERNAME-only :5013-5028; no
USE-CANDIDATE nomination; four live runs showed the rig answering
non-nominating traffic), D-t2 (unified mode: no bundle servicing
during the media loop — one socket one owner; non-unified gets the
full-lifetime worker), D-t3 (no use_srtp DTLS extension — video SRTP
is ANNOUNCE-derived; if the rig's server REQUIRES it, run 13's
handshake alert names it), D-t4 (post-handshake app data = the rig's
SCTP INIT is counted and discarded until R3-C; the rig's tolerance is
run 13's measurement).
**Awaiting (round XXXII):** the target T10 with the SYNCED harness
(expect ALL PASS **74**, zero warnings) + **R7 run 13** — the decision
matrix lives in Next steps 2 (PLAY 200 → the first real Mjolnir media;
refused WITH dtls_ready → R3-C promoted; dtls_fail → the rc names the
fix). Known gaps carried: R3-C SCTP channels, R3-D WS control pings,
the presentation wiring for live frames.
→ `streamer/{include/kn,src}/kn_dtls.*` (NEW),
  `streamer/tests/test_kndtls.c` (NEW),
  `tools/dtls_openssl_check.py` (NEW),
  `streamer/src/knvstd_main.c` (the phase-C/D/E rework),
  `CMakeLists.txt`, `tools/{build_native_tests,cross_check}.sh`,
  `tests/T10_build_and_test.py`, `docs/RTSP.md` §18,
  `docs/DTLS.md` (R3-B SHIPPED + the open-question answers),
  `INTEGRATION.md` rev 21, this file

### 2026-10-27 · impl round XXXII (R7 run 13: THE DTLS HANDSHAKE COMPLETED ON A LIVE PRODUCTION RIG — R3-B live-closed; PLAY went silence→explicit 400 with the body uncaught → the round instruments the refusal, captures the rig's post-DTLS app record, and closes the last pre-PLAY fidelity gap: the video-socket keepalive)
**Context:** same handoff chain. The user delivered target T10 **ALL
PASS 74** (the synced harness — test_kndtls's REAL battery and
test_kncert both ran on Win7 for the first time) and TWO run-13
invocations: a legal queue wait, then (after the first exited on the
PLAY failure) a deliberate re-run that RE-ATTACHED to the live
session. docs/RTSP.md §19 is the record.
**Run 13 evidence (live production, rig #5 = 80-250-96-38):**
* `sock_pair {"video_port":49005,"bundle_port":49006,
  "selection":"preferred","unified":0}` — the official reserved pair
  bound cleanly; **`unified:0` answers DTLS.md Q4: real seats describe
  the two-socket layout** (the persisting bundle worker is the
  production path). Queue: 20 re-learns, the 156 cap with real
  glimpses (44/48/47/33/7), clean 1→3→ready.
* **`dtls_arm armed:1` → `dtls_ready {"ms":388,"version":"DTLSv1.2",
  "cipher":"TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256","records_in":2,
  "records_out":2,"verify":0}` — THE FIRST REAL DTLS HANDSHAKE WITH A
  PRODUCTION GFN RIG.** Every R3-B bet confirmed live: the rig
  answered the client-active ClientHello, negotiated ECDHE-ECDSA over
  DTLS 1.2, REQUESTED+VERIFIED our client cert against the ANNOUNCE'd
  fingerprint (a completed handshake proves it), our pin verified its
  cert (verify:0). The ECDSA/PSA sign path works on Win7 (closing the
  run-12 stale-harness worry for good).
* **PLAY: `rc:0, status:400`** — refusal mode #3 (455 → timeout ×3 →
  400). The rig ANSWERED (ws_text 5, ws_close -1): with DTLS up it now
  EVALUATES the request and rejects something specific. The response
  body/headers were NOT captured — the evidence gap this round closes.
* `ice_preplay`: stun 54, natt 20, req_in 3/answered 3, ok_in 40,
  invalid 0, persist:1 — and **dtls_in 3 vs records_in 2 at ready: the
  rig sent EXACTLY ONE app-data record right after the handshake**
  (discarded per D-t4; content unknown → now captured). pump_ms:0 =
  the persist snapshot artifact (fixed: live accumulation).
* **Invocation 2 = a live RESUME-at-create measurement:** `cm_created`
  returned the SAME session_id with status:2/ready (no queue, no
  SESSION_LIMIT — CloudMatch hands back the live session; R23-adjacent
  attach path measured). The rig accepted a FRESH cert on re-ANNOUNCE
  (fingerprints not cached rig-side), DTLS redid in 293 ms (records_in
  7/out 3 — an HVR-round shape), PLAY 400'd identically with again
  exactly one post-ready app record (dtls_in 8 vs 7). The 400
  correlates with OUR missing state, not the rig's identity.
**Reference re-read (what the reference has at PLAY time that run 13
lacked):** (1) `start_sctp(true)` fires the instant DTLS connects
(:8214-8223) — by PLAY the rig has the client's SCTP INIT; we send no
app data (R3-C). (2) The reference arms BOTH workers before
finish()/PLAY (lib.rs:937 bundle → :990 mjolnir → :1117 finish) — the
Mjolnir video socket fires its NATT keepalives from port N at 20 ms
BEFORE PLAY (:8849-8905); run 13 left 49005 silent until the
never-reached media loop. (3) RULED OUT: the PLAY request shape
(finish() nvst_rtsp.rs:787-800 = base target + common headers +
Session, byte-identical to ours) and wait_for_play (:738-743 — a state
FLAG, not a blocker).
**Round XXXII delta (measurement-maximizing; "measure first, then
code" — NO protocol guesses):**
* **`rtsp_response`**: every non-200 RTSP answer dumps its reason
  phrase, ALL headers and a sanitized 240-byte body excerpt
  (kn_rtsp_response already held everything; sess_rtsp_detail grew the
  nullable-resp parameter; run 14 reads the rig's OWN WORDS).
* **`rig_info`**: the DESCRIBE `Server` header per run (§16 Q5 —
  refusal-mode/version correlation now accumulates automatically).
* **kn_dtls captures the FIRST app record verbatim** (≤64-byte head +
  the true length; stats fields + a byte-exact test assertion 31→32)
  and `ice_preplay` prints it as hex (`app_records`/`app_bytes`/
  `app_first_len`/`app_first`) — names the rig's post-handshake
  protocol (SCTP common header? something else?) = R3-C's design
  input.
* **The pre-PLAY video-socket keepalive** (fidelity item 2): the pump
  thread sends the Mjolnir NATT keepalive from the video socket at
  20 ms — SEND-ONLY (it never recvs there; inbound media queues in the
  8 MiB socket buffer for the media loop) — until main raises
  `vid_handoff` at media-loop entry (a duplicate ping or two during
  the overlap is harmless). Non-unified only; `vid_ka` counter in
  ice_preplay.
* `pump_ms` accumulates live (persist snapshots read the true total);
  one paranoid-gate catch fixed pre-ship (the errsz==1
  -Wformat-truncation=2 over-approximation in sess_reserve_pair's
  fallback message — bounded memcpy instead of snprintf; the file's
  permanent =2 gate stays green ×4 opt levels).
* Deliberately NOT shipped: a hand-rolled SCTP INIT probe — a
  malformed INIT could poison rig state (the SETUP-sweep poison
  precedent) and usrsctp (R3-C) is the faithful path; run 14's
  captured 400 body + app_first hex decide.
**Gates (sandbox, final tree):** native 15 suites PASS; cross x64+i686
-Werror PASS incl. the kn_dtls/test real legs; paranoid knvstd legs
clean ×4; ctest **17/17 ×2**; test_kndtls **32/32** ASan+UBSan+LSan
clean; the OpenSSL-3 interop PASS; sandbox_session_e2e **43/43**; T10
POSIX dry ALL PASS **64**; T13 ALL PASS **63**.
**Awaiting (round XXXIII):** the next target T10 (expect ALL PASS
**74**, zero warnings) + **R7 run 14** — the decision matrix in Next
steps 2: the 400's captured words → a targeted fix; or SCTP-shaped
evidence → R3-C (usrsctp) promotion; or PLAY 200 (the video-socket
keepalive was the missing routing piece) → the first real Mjolnir
media on 49005 = R32's final stage-2 gate. Known gaps carried: R3-C
SCTP channels, R3-D WS control pings, the presentation wiring for
live frames, unified-mode bundle servicing during media (D-t2 —
moot while seats describe unified:0).
→ `streamer/src/knvstd_main.c` (rtsp_response/rig_info capture, the
  pump video-keepalive leg + vid_handoff, live pump_ms, the grown
  ice_preplay), `streamer/{include/kn,src}/kn_dtls.*` (the first-app
  capture), `streamer/tests/test_kndtls.c` (32 checks),
  `docs/RTSP.md` §19, `docs/DTLS.md` (R3-B live-proven, Q3-Q6
  updates), `INTEGRATION.md` rev 22, this file

### 2026-10-27 · impl round XXXIII (R7 run 14: the 400 CAPTURED — empty body, echo headers, app_records:0 → the rig waits for OUR SCTP INIT → R3-C SHIPPED: kn_sctp on vendored usrsctp, the 8-channel DCEP profile, bounded PLAY gating; SEVEN usrsctp landmines measured+fixed)
**Context:** same handoff chain (repo @ round XXXII tree + the user's
run-14 paste + T10 ALL PASS 74). Run 14 is the round-XXXII
instrumentation's payoff: the rig's 400 was captured verbatim and it
says NOTHING (`reason:"OK"`, body_len 0, headers = request-id/cseq/
x-gs-version echo only) — a generic handler refusal. Combined with
**`app_records:0`** (the rig sent no app data after the third
consecutive successful DTLS handshake, 190 ms, fastest yet) and
`vid_ka:23` (the pre-PLAY video keepalive flew and changed nothing),
the reading is forced: **the missing layer is the client-initiated
SCTP association** — the reference fires `start_sctp(true)` the instant
DTLS connects (nvst.rs:8221) and opens the 8-channel NVST profile
BEFORE finish()/PLAY (lib.rs:937→1117). docs/RTSP.md §20 is the record.
**Round XXXIII delta — R3-C (docs/SCTP.md is the design record):**
* NEW vendored **usrsctp** (pinned master c625e21950600460cb5c607561
  6eb1622df2b06a, 2026-10-07, tarball SHA-256 090b9d03…; T10
  downloads+verifies like mbedTLS; CMake builds it static with scoped
  -w; sctp_debug/werror/programs off, inet6 off).
* NEW `streamer/{include/kn,src}/kn_sctp.{h,c}` — AF_CONN over the
  DTLS app layer: the wire parameters mirror sctp-proto EXACTLY
  (5000↔5000 RFC 8831, OS/MIS 65535/65535, a_rwnd 1 MiB via SO_RCVBUF,
  RTO 3000/1000/60000 via sysctls AND SCTP_RTOINFO, unlimited INIT
  attempts, SCTP_MAXSEG 1200 ≈ INITIAL_MTU 1228 minus DTLS overhead);
  the bounded mutex tx queue is the THREAD BOUNDARY (usrsctp's timer
  thread may emit anytime; the mbedTLS ssl context stays pump-thread-
  only); PURE DCEP helpers (RFC 8832 marshal/parse, strict type set)
  identical in every build; the honest stub elsewhere. The NVST
  profile table lives in the module (`kn_sctp_nvst_channels`, all 8:
  sids 0/2/4/6/8/10/12/14, types 0x00/0x01/0x82/0x81, params 0/2/300,
  labels verbatim, priority 0, protocol "" — nvst_input.rs:72-127 +
  str0m sctp/mod.rs:1311-1322).
* kn_dtls became the bridge: NEW `kn_dtls_write_app` /
  `kn_dtls_read_app` (a bounded decrypted-app ring; the D-t4 counted
  discard is now the fallback) + `first_post_ready` — the inject-level
  RAW capture of the first record after ready (run 14 again showed one
  mystery non-app record: dtls_in 8 vs 7; its ContentType byte now
  lands in the evidence) + `peer_closed` surfaced.
* knvstd: the pump thread arms kn_sctp AT dtls_ready (the reference's
  Event::Connected moment), opens all 8 channels on COMM_UP, answers
  rig-side OPENs with ACKs (str0m :1046-1060), captures the first rig
  message head; PLAY waits (bounded) for sctp_up + settle — NEW flags
  `--session-play-wait` (2500) / `--session-play-settle` (250),
  deviation **D-t5**; NEW evidence lines `sctp_arm`, `sctp_up`,
  `sctp_channels`, `sctp_down`; ice_preplay grew 10 sctp fields;
  teardown order = join → kn_sctp_close → kn_dtls_free → sockets →
  cert.
* NEW `tools/sctp_reference.py` (the independent Python: CRC-32c
  self-proved against RFC 3720 B.4's 0xE3069283 BEFORE emitting,
  hand-transcribed DCEP goldens, the 8-channel table, an INIT packet
  builder) → generated `streamer/src/kn_sctp_kat_vectors.h`
  (byte-stable regeneration).
* NEW `streamer/tests/test_knsctp.c` (16th suite): **33 real checks** —
  goldens ↔ builder byte-equality ×8 + parse round-trips + the 6-vector
  malformed battery; the LIVE usrsctp INIT captured and parsed by the
  INDEPENDENT reader (ports 5000↔5000, vtag 0, CRC-32c verifies,
  OS/MIS 65535/65535, a_rwnd ≥ 64 KiB); the full 4-way association
  over a memory relay (INIT→INIT-ACK→COOKIE-ECHO→COOKIE-ACK); the
  8-channel burst received with EXACT sids/labels/types/params and
  ACKed back; user data both ways (sid 0 + sid 14, PPI BINARY);
  one-shot loss recovered by SACK/fast-retransmit; garbage tolerated;
  stats coherent; clean close. Stub contract 13 checks elsewhere.
* **SEVEN usrsctp landmines measured this round (docs/SCTP.md §5):**
  (1) `recvv` fills rcvinfo ONLY when `*infolen` enters ≥ sizeof —
  a zeroed infolen silently yields NOINFO (all 8 OPENs arrived
  sid 0/ppi 0 in the first loopback run); (2) `SCTP_RECVRCVINFO` is a
  PLAIN int sockopt, not an SCTP_EVENT subscription (the event form
  no-ops silently, rc 0); (3) snd_ppid/rcv_ppid are NETWORK order
  (htonl/ntohl — the libdatachannel convention); (4) `conninput`
  stamps BOTH endpoints with the injected key (user_socket.c:3285-
  3296) → per-instance keys make every source address disagree with
  the assoc's remote and the stack ABORTs (chunk 0x06 measured) — the
  in-process loopback needs ONE shared key + DESTINATION-PORT routing
  (client 5001 / server 5000); (5) calling conninput from inside
  conn_output DEADLOCKS (re-entry under the stack's locks — the first
  raw harness hung inside connect); the tx queue is what makes the
  pump design legal; (6) accept() == COMM_UP for a server (the
  notification may pre-fire on the listener); (7) the DCEP parser must
  reject channel types > 0x02 (the golden battery caught my own
  generator putting a valid ACK into the malformed list first).
* Wiring: CMake (usrsctp subdirectory + KN_SCTP_BACKEND_USRSCTP on
  kncore/test + ws2_32 on Win32; ctest 17→**18** ×2), both shell gates
  (16th suite + the real-body/test-real-section cross legs + KCORE),
  T10 (`ensure_usrsctp_source` + suites → target expectation **76**).
**Gates (sandbox, final tree):** native 16 suites PASS; cross x64+i686
-Werror PASS incl. all new legs; paranoid knvstd legs clean with BOTH
backend defines ×3; ctest **18/18 ×2**; test_knsctp **33/33** +
test_kndtls **32/32** under ASan+UBSan+LSan; the OpenSSL DTLS interop
PASS (re-run post-bridge); sandbox_session_e2e **43/43**; T10 POSIX dry
ALL PASS **66**; T13 ALL PASS **63**.
**Awaiting (round XXXIV):** target T10 (expect ALL PASS **76**, zero
warnings) + **R7 run 15** — the decision matrix in Next steps 2 (PLAY
200 → R32's final stage-2 gate; still-400 → the sctp_first* capture
names the next gate; no sctp_up → the app_first hex names the rig's
post-DTLS record). Known gaps carried: sending on the channels (stage
3), R3-D WS pings, the presentation wiring, unified-mode bundle
servicing during media (D-t2 — moot while seats describe unified:0),
teardown ABORTs instead of SHUTDOWN (D-t7).
→ `streamer/{include/kn,src}/kn_sctp.{h,c}` (NEW),
  `streamer/src/kn_sctp_kat_vectors.h` (generated, NEW),
  `streamer/tests/test_knsctp.c` (NEW), `tools/sctp_reference.py` (NEW),
  `streamer/{include/kn,src}/kn_dtls.{h,c}` (the app bridge + the
  post-ready raw capture), `streamer/src/knvstd_main.c` (the SCTP pump
  integration + PLAY gating + evidence), `CMakeLists.txt`,
  `tools/{build_native_tests,cross_check}.sh`,
  `tests/T10_build_and_test.py`, `docs/SCTP.md` (NEW),
  `docs/RTSP.md` §20, `docs/DTLS.md` (R3-C SHIPPED),
  `INTEGRATION.md` rev 23, this file

### 2026-10-27 · impl round XXXIII-hotfix (target T10 configure FAIL → the SIXTH target catch: a WIN32-only CMake branch; fixed + gated permanently with the usrsctp cross-link leg; expectation corrected 75→76)
**The user's first target T10 of the round-XXXIII tree FAILED at
configure (3/76 checks):** `target_link_libraries(usrsctp PUBLIC
ws2_32)` in OUR CMakeLists collided with usrsctp's OWN plain-signature
`target_link_libraries(usrsctp ws2_32 iphlpapi)` (usrsctplib/
CMakeLists.txt:195-198) — CMake forbids mixing plain and keyword
signatures on one target. **Why every sandbox gate missed it:** the
colliding line sat inside `if(WIN32)` — POSIX configure never executes
it (the exact round-XXII class, sixth target-catch incident; the
sandbox ctest/cross legs all stayed green because usrsctp links its own
Win32 deps without us).
**Fix:** the redundant block is GONE (usrsctp already links ws2_32 +
iphlpapi itself on WIN32) with a comment pinning the rule. **The
permanent gate (round-XXII rule extended to LINK closure):**
cross_check.sh now cross-BUILDS usrsctp for mingw x64 (toolchain file,
sctp_debug/werror/programs/shared off) and LINKS `test_knsctp_real_
x86_64.exe` with the exact F1 flags (-static -static-libgcc, the 6.01
subsystem) — the exe's imports are KERNEL32/msvcrt/WS2_32/IPHLPAPI
ONLY (all Win7-built-in; no winpthread DLL — usrsctp uses native Win32
threads). A CMake-configure break of this class can still only be
caught by the target itself, but the LINK closure is now sandbox-
proven every round.
**Expectation corrected:** the failed run's own count (3/76) proved
the target total is **76**, not the 75 predicted (the usrsctp-ensure
check was not counted) — all docs updated. Sandbox re-verification
after the fix: cmake configure+build+ctest **18/18** (mbedtls),
CROSS-CHECK PASS incl. the new link leg, native 16 suites, T10 dry
**66**. Awaiting: the target T10 rerun (expect ALL PASS **76**, zero
warnings) + R7 run 15 (unchanged matrix).
→ `CMakeLists.txt` (the removed WIN32 block + the pinned comment),
  `tools/cross_check.sh` (the usrsctp x64 cross-build + F1 link leg),
  `docs/SCTP.md` §5 landmine 8, `INTEGRATION.md` rev 23b, this file
