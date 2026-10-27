# INTEGRATION NOTES — Round XXXVIII delta, rev 28 (R7 run 18 = THE ACTIVATION CHAIN WORKED: the stall moved 10.99 s -> 30.5 s for the first time — 562 AUs, 13 keyframes, 6665/6665 authenticated, 0 decode errors, feedback present the whole 300 s — and the AU frame-size decay under zero loss / flat RTT names the next gate: the server rate control ramping down against our ZERO bandwidth-estimator fields in QoS 0x0207 (D-t10). The nvst_bandwidth.rs port ships, plus the evidence set run 18 demanded: ControlReportBatch + per-sid outbox counters (the dropped:890 fix), server-control latches (0x109 termination / 0x112 pacing), SCTP cause codes, AU wall-vs-ts telemetry, R3-D the WSS control keepalive)

> **Rev 28 supersedes rev 1-27.** Canonical state: **`START-HERE.md`** (the living handoff since round XXXVIII; `PROJECT-STATE.md` is the frozen rounds I–XXXVIII archive).
> Design records: **`docs/RTSP.md` §25** (NEW — run 18, the diagnosis,
> the round-XXXVIII evidence set, the run-19 decision matrix),
> `docs/INPUT.md` (the control/input plane, unchanged),
> `docs/SCTP.md` (the association layer + the new cause codes).
>
> **Round XXXVIII headline: run 18 is the first POSITIVE movement in the
> stall series** (10.2 -> 10.99 -> **30.5 s**) and it arrived exactly
> when the activation chain started flying (`sent:8 failed:0 ready:1`).
> What remains is not a startup gap but a sustained-rate gap: the rig's
> encoder decays while our QoS reports `estimate_kbps:0,
> utilization:0` forever — the reference's server PID reads those fields
> (nvst.rs:7924-7927 -> nvst_control.rs:133-156). D-t10 was the last
> open wire-fidelity item on the feedback plane; this round closes it
> with a two-implementation-pinned port.

## New files

| file | what | provenance |
|---|---|---|
| `streamer/include/kn/kn_bw.h` | the estimator + server-control API: `kn_bw_*` (init/observe/observe_parity/report/reset/frames_observed), `kn_bw_feedback` (the seven QoS fields), `kn_sctl_walk` + `kn_sctl_diag`, `KN_SCTL_PACING_DEFAULT_ENABLED=1` | nvst_bandwidth.rs + nvst_server_control.rs @ a49b124 |
| `streamer/src/kn_bw.c` | the implementation — pure computation, NO sockets/threads/alloc; the clock arrives as int64 ns parameters; every Rust-`as`-cast site uses the saturating helpers (`bw_sat_i64/u64` + masks) because Rust saturates and C is UB | line-for-line port; parity truths in test_knbw |
| `streamer/src/kn_bw_kat_vectors.h` | **GENERATED** — do not edit; regeneration is byte-identical (`tools/bw_reference.py`) | |
| `streamer/tests/test_knbw.c` | **387 checks** — the 18th suite: the Rust goldens (minimum_server_time=270090, estimate_kbps=4800, utilization=5, queue_delay_us=1000, reorder lossy=2, parity receive_kbps=6400.0), backward_clock (68 frames/50 lossy), stale-frame parity (1 loss 0 frames, jitter 121 us), the batched-server-control walk ([0x104][0x10A][0x112] -> 3 records, pacing on LAST), every truncation, the sctl iterator semantics | replays the vectors AND the hand-checked truths |
| `tools/bw_reference.py` | the independent Python model (the two-implementations rule). It self-proves against the reference's OWN Rust goldens BEFORE emitting the header. **Its scenario scripts (:534-620) are the ONLY authoritative source — never hand-write C scenarios from memory** | |

## Changed files

| file | change |
|---|---|
| `streamer/include/kn/kn_rx.h` | `kn_rx_config` gains `now_ns` (+ud) — the estimator's monotonic ns clock (us socket clock x1000 in production; NULL -> now_ms x 1e6 deterministic fallback for replay/tests); `kn_rx_qos` gains the seven BWE fields; 5 new prototypes (`kn_rx_video_bw_report/bw_frames_observed/bw_counters/bw_reset`, the qos snapshot) |
| `streamer/src/kn_rx.c` | the feed: `process_authenticated` takes `feed_bw` (datagram path=1, plaintext process_packet path=0 — **FINAL, do not relitigate**), at the reference site (post-resolve_nack, pre-FEC/reorder; tuple only when `!retransmitted && !reordered` AND RTP+GS parse; bytes = RTP payload len; ssrc change -> estimator reset; parity -> `observe_parity` only). PITFALL pinned by tests: `reordered` must use the PREVIOUS `highest_sequence_received`. `kn_rx_video_rr_snapshot` + the QoS accessors |
| `streamer/src/knvstd_main.c` | (a) the QoS 0x0207 rewrite — all seven BWE fields with the reference semantics (sender_frame = last COMPLETED frame INDEX, bytes_received = CUMULATIVE, loss = guarded DELTA 10000x(expected-received)/expected with the same-ssrc/same-base/monotonic guard, client_time_90khz = (elapsed_ms x 90) wrapping since transport_origin, const-1000 u16 at @30 AND @32, RR cumulative_lost clamped [-0x800000, 0x7fffff]); (b) `sess_cb_*` ControlReportBatch (MAX 1071, queued_at on first record, flush_due = QoS present OR age >= 50 ms, one flush = ONE sid-6 push, retain-on-failure) wired into frame-ack/pacing/QoS; (c) outbox maxlen 704->1071 + `pushed_per_sid[8]`/`dropped_per_sid[8]`/`refused`; (d) `sess_sctl_ingest` (sid 0 AND sid 6) + the pump's volatile latches + the 30 s `sctl_stats` line; (e) the media loop reads the latches each pass — `sctl_termination` -> `stop_reason:"server_termination"` + break, `sctl_pacing_enabled` gates the 0x0203 sender (`fb_pacing_gated/disabled` counters); (f) AU wall-vs-ts telemetry in dec_ctx (`au_drift_ms/min/max`, `au_ts_backwards`); (g) R3-D `sess_ctrl_ping` (2 s u64-BE pings, pong-match RTT sample, 5 s expiry, 1 ms drain on a 50 ms beat, `cping_*` on the 1 s beat, `ctrl_ping_stop` at teardown); (h) the summary grows `batch`/`outbox_by_sid`/`bwe`/`au_clock` blocks APPENDED after `feedback` — `make_rx_capture.py --verify` untouched |
| `streamer/include/kn/kn_ws.h` + `streamer/src/kn_ws.c` | pong payload capture (`last_pong[125]`/`last_pong_len`/`last_pong_seq` + the caller-cursor `pong_seen`) and `kn_ws_take_pong` (consume-once) — the reference matches the pong payload against its ping sequence (nvst_rtsp.rs:895-901) |
| `streamer/tests/test_knws.c` | +5 R3-D checks (payload capture, u64-BE round-trip, consume-once cursor) — suite 83 -> **88** |
| `streamer/include/kn/kn_sctp.h` + `streamer/src/kn_sctp.c` | assoc_state/error tracking + `kn_sctp_assoc_state_name` — `sctp_down` now prints SHUTDOWN_COMPLETE vs COMM_LOST vs the rest (run 18's graceful death was an inference; run 19's is a fact) |
| `CMakeLists.txt` | `kn_bw.c` into kncore; `knbw` into the test foreach |
| `tools/build_native_tests.sh` | `kn_bw.c` into `$KNCORE` AND the direct-link `test_knrx` leg; the new `test_knbw` compile+run leg |
| `tools/cross_check.sh` | the `kn_bw.c` `-c` leg per arch; `test_knbw_$arch.exe` link leg; `kn_bw.c` into `$KCORE` and the `test_knrx` cross leg |
| `tests/T10_build_and_test.py` | `"test_knbw"` into the suites list -> **the target count goes 78 -> 79** |
| `docs/RTSP.md` | §25 (run 18 — the diagnosis, the shipped set, the run-19 matrix) |
| `PROJECT-STATE.md` | status header, R35 updated + R36 NEW, Next steps, session log |

## Integration notes / traps for the next agent

1. **`test_knbw` is backend-independent.** `kn_bw.c` is pure computation
   (no sockets, no clock of its own, no allocation) — sandbox and target
   run the SAME 387 checks. No CMake define needed.
2. **The CLOCK-EXACTNESS RULE is load-bearing.** The reference does all
   clock math on integer-ns Durations and only then `as_secs_f64`; a
   float clock broke utilization in early parity runs. `kn_bw.c` AND
   `bw_reference.py` are both int64-ns end to end. Production feeds the
   us socket clock x1000 via `cfg.now_ns`; do not "simplify" to ms —
   sub-ms arrival granularity is what keeps `received_ms` off the 0.5 ms
   clamp floor (ms-granular arrivals read 0 ms -> clamp -> 2x
   receive-rate error).
3. **Do NOT send real BWE before 100 observed frames.** The reference
   zero-gates the report until then (nvst.rs:1068-1069); the port
   replicates it. A run-19 summary showing `frames_observed < 100` with
   zero estimates is CORRECT behavior, not a broken port.
4. **The feed decision is FINAL** (it went through two reversals during
   the port): `process_authenticated(feed_bw)` — datagram path 1,
   plaintext `process_packet` path 0. Do not relitigate without new
   reference evidence.
5. **Rust `as` casts saturate/reinterpret; C is UB.** `kn_bw.c` uses
   `bw_sat_i64`/`bw_sat_u64` + explicit masks everywhere. Do not
   simplify them away.
6. **`completed_frame_bytes` is CUMULATIVE and `sender_frame_number` is
   the last COMPLETED frame index** (store-on-complete), not per-report
   deltas. The loss field IS a delta snapshot, guarded by
   same-ssrc + same-base + monotonic.
7. **Server-control iterator semantics are FINAL**
   (nvst_server_control.rs:107-146 + its test): Oversized yields once at
   construction (bytes emptied); BOUNDS errors empty bytes WITHOUT an
   error and yield once; DECODE errors consume one record and the walk
   continues. `0x0100` is NOT in the Unhandled set — it decodes as
   Unknown; bulk tests use the `0x0101` family.
8. **The batch changes the outbox's message shape.** One sid-6 push now
   carries up to 1071 bytes (multiple concatenated records) — exactly
   the reference's `send(&self.bytes)`. Anything that assumed
   one-record-per-message (nothing in-tree does; `--verify` reads the
   summary only) must be updated.
9. **The R3-D service runs on the MAIN thread** (the media loop), not
   the pump: the pump owns the bundle socket, main owns the WSS from
   handshake to TEARDOWN (single-owner discipline, D21 untouched). The
   50 ms service beat bounds the 1 ms drain; the reference's dedicated
   worker reads every 100 ms, so ping/pong timing is unaffected.
10. **Build-leg completeness trap:** `kn_rx.c` now references `kn_bw`,
    and FOUR places link `kn_rx.o` directly (native test_knrx, the cross
    test_knrx leg, `$KCORE`, CMake kncore). When adding a new core
    module, grep every script for direct `kn_rx.c` links — this round
    the native gate caught it, the cross gate caught the rest.
11. **Summary-printf patching traps (hit and fixed this round):** two
    identical `char drops[512]` anchors exist (replay + session) —
    search from the END or pin the function; a format-line edit that
    drops the closing quote silently turns the printf into a comma
    expression (the compiler only says "too many arguments"); after any
    format edit, re-validate the JSON by literal extraction (specs vs
    args count + `json.loads` with `%dx%d` pre-replaced).

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **NATIVE TESTS PASS** — 17 suites incl. **test_knbw 387**, test_knws **88** (+5 R3-D), test_kninput 446, test_knselftest 117; jsonl + zip validation; the paranoid knvstd leg |
| `tools/cross_check.sh` | **CROSS-CHECK PASS** ×2 (x86_64 + i686), zero warnings under `-Wall -Wextra -Werror`, incl. the NEW kn_bw `-c` legs, the test_knbw link legs, kn_bw in $KCORE + the test_knrx leg |
| ctest, mbedTLS backend | **20/20 PASS** (+test_knbw), zero build warnings |
| ctest, schannel backend | **20/20 PASS**, zero build warnings |
| ASan+UBSan on `test_knbw` | **387/387, zero diagnostics** |
| ASan+UBSan on `test_knrx` (the patched feed) | **194/194, zero diagnostics** |
| `tools/sandbox_session_e2e` | **43/43 PASS** (kn_bw added to its link line) |
| `tests/T10_build_and_test.py` (POSIX dry) | **ALL PASS (69 checks)** — was 68: +1 for the new suite |
| `tests/T13_udp_e2e.py` | **ALL PASS (63 checks)** — unchanged |
| paranoid matrix | knvstd ×9 (−O0/−O2/−O3 × bare/mbedTLS/usrsctp) + kn_bw/kn_rx ×3, all clean with `-Wunused-but-set-variable -Wformat-truncation=2` |

> **Pre-existing, NOT introduced this round:** `kn_ws.c` has two
> `-Wformat-truncation=2` diagnostics at HEAD (verified via stash) —
> outside the declared paranoid gate (knvstd + kn_input legs), same
> class as the recorded `kn_selftest.c` pair. Fixing them needs the R28
> value-range bounding treatment and belongs in its own round.

**Next (user side):** sync the changed files, run
`python tests\T10_build_and_test.py` -> expect **ALL PASS 79**, zero
warnings; `test_knbw` must report **387 checks** on the target (same
pure-computation code as the sandbox — that is the Win7-side proof the
estimator matches the reference's goldens). Then **R7 run 19**
(`--session-seconds=300`, untruncated log if possible): paste `handoff`,
one mid-session `session_stats`, `sctl_stats`, `ctrl_ping_stop`,
`ctrl_summary`, the FULL summary (all four new blocks),
`sctp_down`/`ice_summary` — see docs/RTSP.md §25.4 for the decision
matrix. Sustained video + nonzero `bwe.estimate_kbps` past frame 100 =
**STAGE 2 DONE** and the presentation wiring becomes the main track.

---

---

# Round XXXVI delta, rev 26 (R7 run 16 = PLAY 200 — THE FIRST REAL GFN VIDEO THROUGH THE FULL STACK: 4900/4900 authenticated, 307 frames decoded, R32's final stage-2 gate PASSED; the ~10 s feedback stall → the reference's FULL sender set ships: RR/frame-ack/pacing/QoS/keepalive/IDR+PLI/NACK/RFI over the live channels)

> **Rev 26 supersedes rev 1-25.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §23** (run 16 — the breakthrough —
> + the feedback round + the run-17 matrix), `docs/SCTP.md` §8 (all
> three questions answered), `docs/RX.md`/`docs/SRTP.md` (R32 CLOSED
> in the risk register).
>
> **Round XXXVI headline: run 16 broke the wall — `srtp_prof:1` (the
> rig negotiated use_srtp) + `nom_sent:18` → `sctp_up ms:52` (the rig
> answered the INIT run 15 ignored) → `sctp_channels 8/8 acked` →
> **PLAY 200, the first in 16 runs** → handoff (AEAD_AES_256_GCM_8,
> local key) → **4900/4900 datagrams authenticated, 307 AUs, 307
> frames decoded 1280×720, ffprobe-verified h264 High bt709 — R32's
> FINAL STAGE-2 GATE PASSED.** The stream stalled at ~10.2 s exactly
> as the reference documents ("without any receiver feedback the
> server stops video", nvst.rs:4320-4323) with our kf_request still
> unsent — so this round ships the COMPLETE feedback sender set. New
> facts: the mystery post-ready record = app data epoch 1 (benign
> SCTP); run 15's invalid_in:175 = the rig's AUDIO RTP on the bundle
> (now classified); the rig sent COMMAND_INPUT_VERSION 0x020e + 21
> more control messages on sid 0 (the stage-3 input layer awaits).**
>
> ## Changed files (round XXXVI)
> 1. `streamer/{include/kn,src}/kn_rx.*` — NEW `kn_rx_rtcp_rr()` (the
>    reference's build_rtcp_receiver_report :2983-2997 byte-for-byte:
>    0x81/201/len-7, sender "ONOW" 0x4F4E4F57, 24-bit clamped
>    cumulative, LSR=DLSR=0) + NEW `kn_rx_video_rr_snapshot()` and the
>    `base_sequence`/`have_base_sequence` anchor in kn_rx_video (the
>    reference's :1225 expected-count base; set at the first
>    authenticated packet beside the existing highest-seq bookkeeping).
> 2. `streamer/{include/kn,src}/kn_sctp.{h,c}` — NEW
>    `kn_sctp_send_pr()` (SCTP_SENDV_SPA: sndinfo+prinfo in one
>    sendv) for the sender-side PR-SCTP policies; `kn_sctp_send` stays
>    the reliable wrapper (unchanged semantics).
> 3. `streamer/src/knvstd_main.c` —
>    * **the feedback outbox** (`sess_fb_outbox`: 64×704 bounded mutex
>      ring, drop-new counted; push = the media loop/main, pop = the
>      pump thread's `sess_sctp_service`) + `sess_fb_sid_params` (the
>      profile's ordered/PR mapping: sid 4 RTX 2, sid 6/10 TTL 300,
>      the rest ordered-reliable). The media loop ENCODES (kn_rx's
>      single owner — D21 untouched), the pump SHIPS (SCTP's single
>      owner).
>    * **cadences** (all the reference's): RR 1 s → sid 14 (fraction =
>      the :1234-1247 interval-delta arithmetic over the snapshot);
>      frame ACK 0x0204 per assembled AU + pacing 0x0203 @55 556 µs
>      neutral-error (:7928-7933) → sid 6, hooked in dec_frame_in_cb;
>      QoS 0x0207 @50 ms → sid 6 (D-t10: estimator zeros +
>      measured-downlink estimate); keepalive 0x0200 @3 s → sid 0 (the
>      byte-exact golden of nvst_input.rs:1769).
>    * **the recovery senders fly**: dec_take_kf → IDR 0x0302 (sid 6)
>      + PLI (sid 14); the NACK drain (kn_rx_nack_take's own policy) →
>      NACK-v2 (sid 6) + RTCP NACK (sid 14) per :3075-3078; RFI 0x0301
>      (sid 6) with restore-on-drop. The "unsent (stage-3)" notes are
>      GONE; kf_request lines now report sent:true/false honestly.
>    * **bundle demux**: RTP/RTCP-looking (v==2, pt ranges) →
>      `bundle_rtp_in`/`bundle_rtcp_in` instead of `invalid_in` (the
>      rig's audio on 49006 — the audio stage's input).
>    * Evidence: session_stats += fb_rr/fb_ack/fb_qos; the summary +=
>      the `feedback` block (rr/ack/pacing/qos/ka/nack/idr/pli/rfi/
>      pushed/dropped); ice_preplay += bundle_rtp/bundle_rtcp/fb_sent;
>      sess_ice_stats carries the snapshot-safe copies. Teardown
>      destroys the outbox after the join (both paths).
> 4. `streamer/tests/test_knrx.c` — 215→**219**: the RR goldens
>    (independent Python transcription of the reference builder:
>    negative-loss two's-complement, BOTH clamps, LSR/DLSR zeros) +
>    the NULL-safe snapshot guard.
> 5. `streamer/tests/test_knsctp.c` — 33→**35**: the send_pr legs
>    (SPA path: unordered TTL-300 on sid 6 arrives intact); the loss
>    leg hardened (polls until the one-shot drop actually fires —
>    usrsctp may delay/bundle output under traffic; timing-not-
>    semantics fix caught by the first ctest run).
> 6. Docs: `docs/RTSP.md` §23, `docs/SCTP.md` §8 (answered),
>    `PROJECT-STATE.md` (round XXXVI + **R32 CLOSED** in the risk
>    register), this file.
>
> ## Verification status (what ran WHERE, round XXXVI)
> * Sandbox: native 16 suites PASS (test_knrx 219); cross x64+i686
>   -Werror PASS; paranoid knvstd legs clean ×3 with both defines;
>   ctest **18/18 ×2** (fresh); ASan+UBSan+LSan test_knsctp **35/35**;
>   test_kndtls 39/39; the OpenSSL interop ×2 PASS; E2E **43/43**; T10
>   POSIX dry **66**; T13 **63**.
> * Target (round XXXVI's evidence, user-reported): T10 ALL PASS
>   **76** zero warnings; **R7 run 16 — PLAY 200 + real video** (docs/
>   RTSP.md §23) + the ffprobe verification of the dumped keyframe.
> * Target (PENDING — round XXXVII asks for): T10 (expect **76**) +
>   **R7 run 17** — the SUSTAINED-stream run (matrix in PROJECT-STATE
>   Next steps 2; paste the session_stats progression + the FULL
>   summary with the feedback block).
>
> ## Notes for the target build
> * Sync list (round XXXVI): kn_rx.{h,c}, kn_sctp.{h,c},
>   knvstd_main.c, test_knrx.c, test_knsctp.c + docs. The T10 harness
>   is UNCHANGED (expectation **76**).
> * No new libraries/imports: the outbox uses the same kernel32/
>   pthread primitives as the pump thread; send_pr is one more usrsctp
>   entry point (statically linked already).

# Round XXXV delta, rev 25 (sandbox-only round between runs: the reference re-read found fidelity gap #2 — str0m negotiates DTLS-SRTP (use_srtp) and the bundle audio's keys come from it → MBEDTLS_SSL_DTLS_SRTP enabled via user-config, kn_dtls offers 0x0001 (D-t9), wire-pinned by an independent parser + negotiated through OpenSSL)

> **Rev 25 supersedes rev 1-24.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §22** (the finding + the shipped
> delta + the run-16 two-hypothesis matrix), §21 (run 15 + the
> nomination), `docs/SCTP.md`, `docs/DTLS.md` (Q3 updated).
>
> **Round XXXV headline: str0m's DTLS emits `KeyingMaterial(km,
> profile)` right after Connected (vendor lib.rs:1648-1658 →
> session.rs:259-272) — the reference's ClientHello carries the RFC
> 5764 `use_srtp` extension, offering [AEAD_AES_256_GCM 0x0008,
> AEAD_AES_128_GCM 0x0007, AES128_CM_SHA1_80 0x0001]
> (crypto/srtp.rs ALL), and the bundle audio (rtcAudioOnNativeBundle:1
> — which OUR announce mirrors) takes its SRTP keys from that
> negotiation. Runs 13-15 sent NO use_srtp (D-t3). Shipped: mbedTLS's
> MBEDTLS_SSL_DTLS_SRTP enabled through the sanctioned
> MBEDTLS_USER_CONFIG_FILE (vendor tree untouched), kn_dtls offers
> [0x0001] no-MKI (D-t9 = the intersection with mbedTLS 3.6's
> ceiling; RFC 5764 omission semantics make it regression-proof,
> test-pinned), `kn_dtls_srtp_profile()` + `srtp_prof` in the
> dtls_ready evidence. Run 16 now tests nomination (D-t8) + use_srtp
> (D-t9) TOGETHER — deconfounded by the evidence lines.**
>
> ## Changed files (round XXXV)
> 1. `streamer/src/kn_mbedtls_user_config.h` (NEW) — the additive user
>    config: `#define MBEDTLS_SSL_DTLS_SRTP` (requires only
>    MBEDTLS_SSL_PROTO_DTLS, on by default). **ABI WARNING documented
>    in the header: the define changes the layout of
>    mbedtls_ssl_context/config — EVERY TU including mbedtls/ssl.h
>    must see it** (CMake: PUBLIC on the mbedtls target → all
>    consumers inherit; cross_check: the flag on all four mbedtls
>    legs; ad-hoc sanitizer/manual builds linking the CMake libraries:
>    add `-DMBEDTLS_USER_CONFIG_FILE='"kn_mbedtls_user_config.h"'`
>    with -Istreamer/src — the round-XXXV ASan legs did).
> 2. `CMakeLists.txt` — `target_compile_definitions(mbedtls PUBLIC
>    "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/streamer/
>    src/kn_mbedtls_user_config.h\"")` in the mbedtls block (absolute
>    path — streamer/src is not on the mbedtls target's include path).
> 3. `streamer/{include/kn,src}/kn_dtls.{h,c}` — the use_srtp offer
>    (`#ifdef MBEDTLS_SSL_DTLS_SRTP`, graceful no-op without: a static
>    [0x0001, UNSET] list → conf_dtls_srtp_protection_profiles) +
>    `kn_dtls_srtp_profile()` (the negotiated id via
>    `MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)` — the 3.x field
>    convention, kn_net_mbedtls precedent) + the stub twin. The
>    keying material is NOT extracted (bundle audio = stage 3; §7
>    shapes-only — the id rides the evidence line).
> 4. `streamer/src/knvstd_main.c` — `dtls_ready` grew `"srtp_prof"`
>    (1 = the rig negotiated 0x0001; 0 = RFC omission, the runs-13-15
>    wire).
> 5. `streamer/tests/test_kndtls.c` — 32→**39 checks**: (a) the
>    ClientHello capture leg — a SECOND loopback socket receives the
>    first flight (kn_dtls's BIO owns its send path — no write_cb
>    exists; the first draft wrongly borrowed the kn_sctp capture
>    pattern and saw zero packets) and an INDEPENDENT hand parser
>    (record 13 B + hs header 12 B + CH body walk + extension scan)
>    asserts use_srtp 0x000e with ONE profile 0x0001 and an empty MKI
>    — the parser caught its own bug against the live wire: RFC 5764's
>    SrtpProfiles is a BYTE-LENGTH vector (`<2..2^16-1>`), not a
>    count; (b) negotiation asserted on BOTH ends of the loopback pair
>    (0x0001 chosen); (c) the NEGATIVE leg: a profile-less server
>    completes the handshake with profile 0 — pinning that the offer
>    can never regress the runs-13-15 wire. tsrv_init grew with_srtp.
> 6. `tools/dtls_openssl_check.py` — second leg: `s_server -dtls1_2
>    -use_srtp SRTP_AES128_CM_SHA1_80` (OpenSSL's flag name measured:
>    `-use_srtp`, NOT `-srtp`) → the client asserts
>    `srtp_profile=0x0001`; **an INDEPENDENT stack negotiates our
>    offer**. The plain leg now also asserts 0x0000 (omission
>    semantics cross-verified). `test_kndtls --openssl PORT FP [srtp]`.
> 7. `tools/cross_check.sh` — the user-config define on all FOUR
>    mbedtls legs (kn_net_mbedtls, kn_cert real, kn_dtls real,
>    test_kndtls real section; bare-name form — CFLAGS carries
>    -Istreamer/src). One -Werror=comment catch fixed pre-ship (a `/*`
>    inside the new parser's doc comment).
> 8. Docs: `docs/RTSP.md` §22, `docs/DTLS.md` (Q3 updated: the live
>    hypotheses are nomination + use_srtp), `PROJECT-STATE.md` (round
>    XXXV), this file.
>
> ## Verification status (what ran WHERE, round XXXV)
> * Sandbox: native 16 suites PASS; cross x64+i686 -Werror PASS incl.
>   the four re-defined mbedtls legs + the usrsctp link leg; paranoid
>   knvstd legs clean ×3; ctest **18/18 ×2 with FRESH rebuilds** (the
>   layout change demands it); ASan+UBSan+LSan test_kndtls **39/39**;
>   the OpenSSL interop BOTH legs PASS (0x0000 plain / 0x0001
>   -use_srtp); sandbox_session_e2e **43/43**; T10 POSIX dry ALL PASS
>   **66**; T13 ALL PASS **63**.
> * Target (PENDING — round XXXVI asks for): T10 (expect ALL PASS
>   **76**, zero warnings; the first build recompiles mbedTLS with the
>   new layout, ~1 min extra) + **R7 run 16** — the two-hypothesis run
>   (matrix in PROJECT-STATE Next steps 2 + RTSP.md §22). Paste
>   dtls_ready (srtp_prof!), sctp_arm/sctp_up, PLAY, rtsp_response,
>   and the FULL ice_preplay (nom_sent/post_first/sctp_out).
>
> ## Notes for the target build
> * Sync list (round XXXIV+XXXV together): kn_sock.{h,c}, kn_dtls.{h,c},
>   knvstd_main.c, test_knsock.c, test_kndtls.c, test_knsctp.c,
>   kn_mbedtls_user_config.h (NEW), CMakeLists.txt,
>   tools/{cross_check.sh,dtls_openssl_check.py}, docs. T10 harness
>   UNCHANGED since rev 23b (expectation **76**).
> * The mbedTLS layout change is ABI-internal to the build (all TUs
>   recompile together via the PUBLIC define; no shipped interface
>   changes). If a stale build dir lingers on the target, T10's
>   `cmake --fresh` already handles it.

# Round XXXIV delta, rev 24 (R7 run 15 = usrsctp LIVE on Win7: the SCTP INIT flew inside DTLS and the rig stayed silent; PLAY 400 #4 → the ICE nomination hypothesis: USE-CANDIDATE ships with independent goldens; the mystery post-ready record finally prints; the seventh target catch fixed)

> **Rev 24 supersedes rev 1-23.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §21** (run 15 + the nomination
> analysis + the run-16 matrix), `docs/SCTP.md` §8 (the INIT-silence
> measurement), `docs/DTLS.md` (D-t8 retires D-t1).
>
> **Round XXXIV headline: run 15 proved the R3-C stack live (sctp_arm
> armed:1 — usrsctp up on Win7, the INIT left inside a DTLS app
> record, dtls_out 3→4) and the rig IGNORED it (sctp_up:0 after the
> full bounded wait; app_records:0; no COMM_LOST — silence) with PLAY
> 400 #4 across two rigs. The remaining wire difference vs the
> reference: its str0m CONTROLLING agent nominates the valid pair
> (USE-CANDIDATE) while ours never did — a Bifrost-class rig plausibly
> gates the data plane on the SELECTED pair (DTLS gates only on
> connectivity — four successful handshakes fit). Shipped:
> kn_sock_stun_build2 + the nomination (same real-STUN gate as the
> DTLS arm; post-ready one nominated request per 100 ms tick; D-t8),
> byte-exact vs an independently generated golden whose Python builder
> self-proved against the round-XXV goldens first (incl. GFN's
> non-RFC 0x5354554E fingerprint XOR); the run-15 evidence gaps
> closed (post_first/peer_closed/nom_sent/sctp_out/sctp_in in
> ice_preplay — the thrice-seen mystery post-ready record finally
> gets its ContentType printed); the target's 3 -Wmaybe-uninitialized
> warnings fixed (the SEVENTH target catch). Gates: test_knsock
> 98→102 (ASan clean), ctest 18/18 ×2, cross ×2 + the usrsctp link
> leg, native 16, E2E 43/43, T10 dry 66, T13 63.**
>
> ## Changed files (round XXXIV)
> 1. `streamer/{include/kn,src}/kn_sock.*` — `KN_SOCK_STUN_ATTR_USE_
>    CANDIDATE 0x0025` + `kn_sock_stun_build2` (the two-attribute
>    form; attr2_type 0 omits it; `kn_sock_stun_build` is now the
>    attr2-less wrapper — wire-identical, the existing goldens
>    re-prove it). Attribute order attr1→attr2 is wire-pinned by the
>    new golden.
> 2. `streamer/tests/test_knsock.c` — 98→**102 checks**: the
>    nominated-request golden (USERNAME+USE-CANDIDATE, byte-exact,
>    generated by the independent Python builder AFTER it reproduced
>    k_req/k_succ byte-for-byte — the vectors-self-prove rule),
>    integrity+inspect round-trips past the extra attribute, the
>    undersized-buffer refusal.
> 3. `streamer/src/knvstd_main.c` — the pump nominates: the gate is
>    `(req_in || ok_in)` — the SAME first-real-inbound-STUN rule as
>    the DTLS arm (:8196, never synthesized); pre-ready the 3× bursts
>    carry the flag (`build2`), post-ready ONE nominated request per
>    100 ms tick beside the NATT ping (the str0m selected-pair
>    traffic mirror — deviation **D-t8 retires D-t1**). ice_preplay
>    grew `nom_sent`, `post_first`/`post_first_len` (the kn_dtls
>    inject-level RAW capture of the first post-ready record — the
>    mystery record's ContentType: 0x14 CCS / 0x15 alert / 0x16
>    handshake / 0x17 app), `peer_closed`, `sctp_out`/`sctp_in` (the
>    kn_sctp packet counters — the INIT-left proof at the SCTP layer);
>    `sctp_fail` class 3 = send errors. sess_ice_stats carries the
>    snapshot-safe copies.
> 4. `streamer/tests/test_knsctp.c` — the seventh-target-catch fix:
>    both `t_sctp_pkt p` declarations zero-init (GCC 16.2's
>    -Wmaybe-uninitialized cannot prove the conditional-fill invariant
>    through the `prc == 0 &&` short-circuit; sandbox GCC 12 silent —
>    the two-tier gate working as designed).
> 5. Docs: `docs/RTSP.md` §21, `docs/SCTP.md` §8 (the answer),
>    `docs/DTLS.md` (D-t8), `PROJECT-STATE.md` (round XXXIV).
>
> ## Verification status (what ran WHERE, round XXXIV)
> * Sandbox: native 16 suites PASS (test_knsock 102); cross x64+i686
>   -Werror PASS incl. the usrsctp cross-build + F1 link leg; paranoid
>   knvstd legs clean ×3 with BOTH backend defines; ctest **18/18 ×2**;
>   ASan+UBSan+LSan: test_knsctp 33/33 + test_knsock 102/102;
>   sandbox_session_e2e **43/43**; T10 POSIX dry ALL PASS **66**; T13
>   ALL PASS **63**. Toolchain note: the sandbox reset between messages
>   (R26) and the apt mirror hung — recovered via round XXVI's extended
>   fallback (aliyun → deb.debian.org rewrite).
> * Target (round XXXIV's evidence, user-reported): T10 **FAIL 3/76 =
>   ONLY the 3 warnings** (everything else passed — incl. the usrsctp
>   Win32 download/build/link, proving the rev-23b hotfix); R7 run 15
>   (docs/RTSP.md §21).
> * Target (PENDING — round XXXV asks for): T10 (expect ALL PASS
>   **76**, zero warnings) + **R7 run 16** — the nomination run;
>   matrix in PROJECT-STATE Next steps 2. Paste the FULL ice_preplay
>   (nom_sent/post_first/sctp_out are the new deciders).
>
> ## Notes for the target build
> * No new files to wire: kn_sock.{h,c}, test_knsock.c, test_knsctp.c,
>   knvstd_main.c changed; the gates/T10 harness are unchanged since
>   rev 23b (expectation stays **76**).
> * The nomination changes ONLY the STUN attribute list on our own
>   requests — a rig that ignores USE-CANDIDATE behaves exactly as in
>   runs 9-15 (the attribute is integrity-covered like everything
>   else; four rigs have been validating our MI all along).

# Round XXXIII delta, rev 23 (R7 run 14 = the 400 captured: empty body, echo headers, app_records:0 → the rig waits for OUR SCTP INIT → R3-C SHIPPED: kn_sctp on vendored usrsctp + the 8-channel DCEP profile + bounded PLAY gating)

> **Rev 23 supersedes rev 1-22.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/SCTP.md`** (NEW — kn_sctp's full record:
> wire parameters, thread map, the SEVEN usrsctp landmines, deviations
> D-t4..D-t7), `docs/RTSP.md` §20 (run 14 + the delta + the run-15
> matrix), §19 (run 13), `docs/DTLS.md` (R3-C marked SHIPPED).
>
> **Round XXXIII headline: run 14's captures forced the verdict —
> PLAY's 400 says nothing (`reason:"OK"`, empty body, echo headers) and
> `app_records:0` proves the rig initiates no SCTP; the reference
> client does (`start_sctp(true)` at dtls_ready, then the 8-channel
> DCEP profile, all before finish()/PLAY). R3-C shipped: `kn_sctp`
> (vendored usrsctp c625e219, AF_CONN over the new kn_dtls app bridge,
> sctp-proto-exact wire parameters, the full NVST channel profile) +
> the independent Python reference + the 33-check suite + PLAY gated on
> sctp_up (D-t5). Target expectation: T10 ALL PASS **76** (the harness
> now fetches the pinned usrsctp tarball), then R7 run 15.**
>
> ## Changed files (round XXXIII)
> 1. `streamer/{include/kn,src}/kn_sctp.{h,c}` (NEW) — the R3-C module:
>    real body under KN_SCTP_BACKEND_USRSCTP (CMake auto-detects
>    third_party/usrsctp, BOTH platforms), honest stub elsewhere.
>    AF_CONN routing (one registered key per process — the
>    libdatachannel pattern; usrsctp opens no real sockets, DTLS is
>    the wire); the bounded mutex tx queue is the thread boundary
>    (usrsctp's timer thread emits into it; only kn_sctp_poll — the
>    pump thread — touches the DTLS context); wire parameters mirror
>    sctp-proto exactly (5000↔5000, OS/MIS 65535, a_rwnd 1 MiB, RTO
>    3000/1000/60000 via sysctls+RTOINFO, unlimited INIT attempts,
>    MAXSEG 1200); PURE DCEP marshal/parse (RFC 8832, strict type set)
>    identical in every build; `kn_sctp_nvst_channels[8]` = the
>    reference profile verbatim (sids 0-14, labels, 0x00/0x01/0x82/0x81,
>    params 0/2/300, priority 0, protocol ""); `kn_sctp_open_channels`
>    fires the burst ordered+reliable PPI 50 per stream; test-only
>    server/peer opens (shared-key + port-routed loopback — landmine
>    #4 in SCTP.md §5).
> 2. `streamer/{include/kn,src}/kn_dtls.{h,c}` — the app layer became a
>    BRIDGE: `kn_dtls_write_app` (mbedtls_ssl_write when ready; the
>    SCTP tx path) + `kn_dtls_read_app` (a bounded 8×2048 decrypted-app
>    ring; the SCTP rx path) + `first_post_ready` (the inject-level RAW
>    capture of the first post-ready record — run 15 names the mystery
>    record run 13/14 saw: ContentType 20/21/22/23) + `app_q_dropped`;
>    the D-t4 counted discard stays as the no-consumer fallback.
> 3. `streamer/src/knvstd_main.c` — the pump thread arms kn_sctp the
>    moment dtls_ready fires (write_cb = kn_dtls_write_app; the DTLS
>    app ring feeds kn_sctp_inject each pass); on COMM_UP it opens all
>    8 channels; rig-side OPENs get ACKs (str0m :1046-1060); the first
>    rig message head is captured. PLAY gating (D-t5): after the 100 ms
>    burst head start, main waits (snapshot-polled, bounded by
>    `--session-play-wait`, default 2500 ms) for sctp_up — or for
>    dtls/sctp failure or Ctrl-C — then settles `--session-play-settle`
>    (default 250 ms); PLAY ALWAYS flies afterwards (a dead data plane
>    never eats the measurement). New evidence: `sctp_arm` (with the
>    refusal detail), `sctp_up {"ms"}`, `sctp_channels {"opened",
>    "acked","rig_opened"}`, `sctp_down`; ice_preplay grew sctp_armed/
>    up/ms/opened/acked/rig_opened/msgs_in/fail/first_sid/first_ppi/
>    first_len/first(hex). Teardown order: join → kn_sctp_close →
>    kn_dtls_free → sockets → cert. run_session grew the two play
>    parameters; usage + argv parsing updated.
> 4. `streamer/tests/test_knsctp.c` (NEW, 16th suite) — self-adaptive:
>    REAL battery 33 checks / stub contract 13. The real legs: the
>    Python goldens ↔ builder byte-equality (all 8 channels) + parse
>    round-trips + the 6-vector malformed battery + CRC-32c goldens +
>    the golden INIT through the independent parser; the LIVE usrsctp
>    INIT captured at the write_cb and verified independently (ports
>    5000↔5000, vtag 0, CRC-32c, OS/MIS 65535/65535, a_rwnd ≥ 64 KiB);
>    the full 4-way association over the port-routed memory relay; the
>    channel burst received with exact sids/labels/types/params + ACKed
>    both ways; user data sid 0 + sid 14; one-shot loss → SACK/fast-
>    retransmit recovery; garbage inject tolerated; stats coherence
>    (honest about the shared-key topology: the server queue carries
>    both directions). ASan+UBSan+LSan clean.
> 5. `tools/sctp_reference.py` (NEW) + generated
>    `streamer/src/kn_sctp_kat_vectors.h` — the independent Python
>    (self-proves CRC-32c against RFC 3720 B.4 = 0xE3069283 and
>    hand-transcribed DCEP goldens BEFORE emitting; byte-stable
>    regeneration). It caught its own bug pre-ship (a valid ACK had
>    been filed under the malformed vectors).
> 6. `third_party/usrsctp` (GITIGNORED, vendored like mbedtls) —
>    pinned master c625e21950600460cb5c6075616eb1622df2b06a
>    (2026-10-07), tarball SHA-256
>    090b9d03bf068ce5989576c8e0ff6979d3031ea85e405acd263206f19166b41e;
>    `tests/T10_build_and_test.py` downloads+verifies+extracts it
>    (ensure_usrsctp_source, the mbedtls pattern) and gates the build
>    on it; suites += test_knsctp → target expectation **76**.
> 7. `CMakeLists.txt` — kn_sctp.c into kncore; knsctp into the foreach
>    (ctest 17→18 ×2); the usrsctp block: add_subdirectory with
>    sctp_debug/werror/invariants/programs/shared/fuzzer OFF + inet6
>    OFF, scoped `-w` on the vendored target ONLY (our files stay
>    strict — the mbedtls-suppression precedent), KN_SCTP_BACKEND_
>    USRSCTP on kncore + test_knsctp, ws2_32 on Win32, an explicit
>    ENABLED/stub status line.
> 8. `tools/build_native_tests.sh` (KNCORE += kn_sctp.c; the 16th suite
>    builds stub + runs as rc20) and `tools/cross_check.sh` (kn_sctp
>    stub -c + the REAL-body leg + the test's REAL-section leg under
>    the exact target flags ×2 arches whenever third_party/usrsctp
>    exists — the round-XXII rule; the stub suite link; KCORE +=).
> 9. Docs: `docs/SCTP.md` (NEW — §2 the wire-parameter provenance
>    table, §3 the channel profile, §4 the thread map, §5 the SEVEN
>    landmines, §6 verification, §7 deviations D-t4..D-t7, §8 run-15
>    questions), `docs/RTSP.md` §20, `docs/DTLS.md` (R3-C SHIPPED +
>    the remaining stage-3 send scope), `PROJECT-STATE.md` (round
>    XXXIII), `tests/README.md` untouched (no new target harness).
>
> ## Verification status (what ran WHERE, round XXXIII)
> * Sandbox: native 16 suites PASS; cross mingw x64+i686 -Werror PASS
>   incl. the kn_sctp real-body + test-real-section legs; paranoid
>   knvstd legs clean with BOTH backend defines ×3 opt levels; ctest
>   **18/18 ×2** (mbedtls: test_kndtls 32 + test_knsctp 33 real;
>   schannel: knsctp still REAL — usrsctp is TLS-independent —
>   kndtls stub); ASan+UBSan+LSan clean on both new suites; the
>   OpenSSL-3 DTLS interop PASS (re-run after the bridge changes);
>   sandbox_session_e2e **43/43** (unchanged by design — the rig
>   speaks no SCTP yet; upgrading it is a stage-3 candidate); T10
>   POSIX dry ALL PASS **66**; T13 ALL PASS **63**.
> * Target (round XXXIII's evidence, user-reported): T10 ALL PASS
>   **74** (the round-XXXII tree); R7 run 14 (docs/RTSP.md §20).
> * Target (PENDING — round XXXIV asks for): T10 (expect **76**, the
>   usrsctp download on first run) + **R7 run 15** (the decision
>   matrix in PROJECT-STATE Next steps 2; paste sock_pair/dtls_ready/
>   sctp_arm/sctp_up/sctp_channels/PLAY/rtsp_response/ice_preplay
>   verbatim, + handoff/summary on PLAY 200).
>
> ## Notes for the target build
> * **rev 23b HOTFIX (the sixth target catch):** the first target T10
>   died at configure — our `target_link_libraries(usrsctp PUBLIC
>   ws2_32)` mixed keyword signature with usrsctp's own PLAIN
>   `target_link_libraries(usrsctp ws2_32 iphlpapi)` (its CMakeLists
>   :195-198; WIN32-only branch — invisible to the POSIX sandbox).
>   Fixed by DELETING our line (usrsctp links its own Win32 deps) and
>   gated permanently: cross_check.sh now cross-builds usrsctp for
>   mingw x64 and LINKS the real suite under the exact F1 flags —
>   imports verified: KERNEL32/msvcrt/WS2_32/IPHLPAPI only, no
>   winpthread DLL. Target expectation corrected 75 → **76** (the
>   failed run's own 3/76 count proved it).
> * First T10 run downloads the ~0.8 MB pinned usrsctp tarball
>   (SHA-256 verified before extraction); the build grows by ~25 s
>   (usrsctp static). The vendored target compiles with scoped `-w`
>   (the mbedtls-suppression precedent); T10's zero-warning gate greps
>   the WHOLE ninja log for "warning:", so if GCC 16.2 ever surfaces a
>   class `-w` does not cover, the gate flags it — the two-tier rule
>   working as designed. The sandbox GCC 12 build is warning-free.
> * No new PE imports: usrsctp is static; AF_CONN opens no sockets
>   (ws2_32 already linked for kn_sock).
> * kn_sctp's real body needs NO code change on Win32 — the sandbox
>   cross legs already compiled it under the exact target flags ×2.


# Round XXXII delta, rev 22 (R7 run 13 = THE DTLS HANDSHAKE COMPLETED ON A LIVE PRODUCTION RIG — R3-B live-closed; PLAY went silence→explicit 400 with the body uncaught → the instrumentation round: the full non-200 response capture, the rig's post-DTLS app-record capture, the pre-PLAY video-socket keepalive)

> **Rev 22 supersedes rev 1-21.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §19** (run 13 + the round-XXXII
> delta + the run-14 decision matrix), §18 (R3-B design + the mbedTLS
> landmines), `docs/DTLS.md` (R3-B LIVE-PROVED; open questions 3-6
> updated).
>
> **Round XXXII headline: run 13 landed the project's deepest live
> datum yet — `dtls_ready {"ms":388,"version":"DTLSv1.2","cipher":
> "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256","verify":0}` on a
> production rig (and again in 293 ms on a RESUME-at-create re-attach
> with a fresh cert): the rig answered our client-active ClientHello,
> requested+verified our certificate against the ANNOUNCE'd
> fingerprint, our pin verified its cert. `sock_pair` bound the
> preferred 49005/49006 and the seat described `unified:0` — the
> two-socket layout IS the production path. PLAY moved from silence to
> an explicit **400** (the rig now evaluates the request) but the body
> was NOT captured, and the rig sent exactly ONE post-handshake app
> record that D-t4 discarded. At PLAY time the reference additionally
> has: the SCTP INIT (start_sctp(true) the instant DTLS connects,
> :8214-8223 → R3-C) and the PRE-PLAY Mjolnir NATT keepalives from the
> video socket (lib.rs:990 arms that worker before finish();
> :8849-8905) — run 13 left 49005 silent. This round ships the
> instrumentation that ends the guessing and that last fidelity piece;
> NO hand-rolled SCTP (poison precedent — run 14's captures decide
> R3-C).**
>
> ## Changed files (round XXXII)
> 1. `streamer/src/knvstd_main.c` —
>    * `sess_rtsp_detail` grew a nullable `resp` parameter and emits
>      the NEW **`rtsp_response`** line on any non-200: reason phrase,
>      EVERY header (`{"n":..,"v":..}` pairs, each through the
>      lb_json_path sanitizer) and a 240-byte body excerpt — the rig's
>      own words about run 13's 400 land in run 14's log.
>    * NEW **`rig_info`** line: the DESCRIBE response's `Server`
>      header (RTSP.md §16 open question 5 — refusal-mode/version
>      correlation accumulates per run now).
>    * The pump thread grew the **pre-PLAY video-socket keepalive**:
>      `vid_sock/vid_ip/vid_port/vid_payload` + the 20 ms NATT ping
>      (the media loop's exact shape, :8849-8905) — SEND-ONLY (it
>      never recvs the video socket; inbound media queues in the 8 MiB
>      buffer for the media loop), stopped by main's `vid_handoff`
>      flag at media-loop entry (duplicate-ping overlap harmless);
>      wired in non-unified mode only; `vid_ka` counter.
>    * `pump_ms` accumulates LIVE in the loop (the persist-mode
>      snapshot used to read 0 — run 13's artifact).
>    * `ice_preplay` grew `vid_ka`, `app_records`, `app_bytes`,
>      `app_first_len`, `app_first` (hex); `sess_ice_stats` carries
>      the snapshot-safe copies (main never touches kn_dtls while the
>      thread runs).
>    * One paranoid-gate fix pre-ship: sess_reserve_pair's fallback
>      message uses a bounded memcpy, not snprintf (the errsz==1
>      -Wformat-truncation=2 over-approximation class; the file's
>      permanent =2 gate stays clean ×4 optimization levels).
> 2. `streamer/{include/kn,src}/kn_dtls.{h,c}` — the FIRST app-data
>    record is captured verbatim (≤ KN_DTLS_FIRST_APP_CAP=64 bytes +
>    the true length) inside the drain path; stats grew
>    `first_app`/`first_app_len`. This is R3-C's design input: run 13
>    proved the rig sends exactly one app record after the handshake —
>    run 14 names its protocol (an SCTP common header is
>    unmistakable in hex).
> 3. `streamer/tests/test_kndtls.c` — 31→**32 checks**: the app-data
>    leg now asserts the first-record capture byte-exact against the
>    test server's "kn-r3b" record.
> 4. `docs/RTSP.md` §19 (run 13 ×2 invocations + the reference
>    re-read + the delta + the run-14 matrix), `docs/DTLS.md`
>    (R3-B LIVE-PROVED; Q3 partially / Q4 fully / Q5 instrumented /
>    Q6 answered+measured), `PROJECT-STATE.md` (round XXXII),
>    `tests/T10_build_and_test.py` UNCHANGED this round (no new
>    suites — the expectation stays **74**).
>
> ## Verification status (what ran WHERE, round XXXII)
> * Sandbox: native 15 suites PASS; cross mingw x64+i686 -Werror PASS
>   incl. the kn_dtls real-body + test real-section legs; paranoid
>   knvstd legs clean -O0..-O3; ctest **17/17 ×2** (test_kndtls real
>   32/32 / stub 6/6); ASan+UBSan+LSan clean; the OpenSSL-3 interop
>   PASS; sandbox_session_e2e **43/43**; T10 POSIX dry ALL PASS 64;
>   T13 ALL PASS 63.
> * Target (round XXXII's evidence, user-reported): T10 **ALL PASS
>   74** zero warnings; R7 run 13 ×2 invocations (docs/RTSP.md §19).
> * Target (PENDING — round XXXIII asks for): T10 (expect 74) + **R7
>   run 14** — paste rtsp_response/rig_info/sock_pair/dtls_arm/
>   dtls_ready/PLAY/ice_preplay verbatim (+ handoff/summary if PLAY
>   200).
>
> ## Notes for the target build
> * No new files to wire this round beyond syncing the three changed
>   sources + two headers' consumers: knvstd_main.c, kn_dtls.{h,c},
>   test_kndtls.c (the gates and T10 harness are unchanged since
>   round XXXI's sync).
> * Behaviour notes: the video socket now emits NATT pings from the
>   moment the pump thread starts (pre-PLAY) — a firewall prompt, if
>   the machine ever shows one, would appear there (run 13 already
>   proved inbound UDP flows on 49006; 49005 send-side is new).



# Round XXXI delta, rev 21 (R7 run 12 = the real fingerprint flew and PLAY STILL refused → R3-B SHIPPED: kn_dtls, the DTLS 1.2 ACTIVE client on the bundle socket + the reference's N/N+1 socket pair + the pump-thread DTLS state machine)

> **Rev 21 supersedes rev 1-20.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §18** (run 12 + the R3-B design +
> the three mbedTLS landmines), `docs/DTLS.md` (R3-A/R3-B SHIPPED).
>
> **Round XXXI headline: run 12 finalized the fingerprint question — a
> 4th rig (80-84-170-153) accepted the ANNOUNCE carrying the REAL
> P-256 fingerprint (`identity cert:real fp_len 95`, ANNOUNCE 200,
> 4137 B) and then held PLAY in the exact run-11 silence (timeout,
> ws_pings_in 0, ws_closes_in 0, frames_out 5 vs text 4) while the
> pump saturated UDP beside it (20921 ms, 2841+947, 126/126 answered,
> 3788 validated, invalid 0). The announced fingerprint ALONE is not
> the gate — the handshake behind it is. R3-B SHIPPED: `kn_dtls` (the
> portable-mbedTLS DTLS 1.2 ACTIVE client, fingerprint-pinned via the
> double gate, inject-queue BIO, ms-clock retransmission timers), the
> reference's socket pair (video N / bundle N+1, preferred
> 49005/49006), the pump-thread DTLS state machine (ClientHello gated
> on the first REAL inbound STUN, post-ready 100 ms NATT-only cadence,
> the worker persists through the media loop in non-unified mode), and
> the evidence lines (`sock_pair`, `dtls_arm`, `dtls_ready`/`dtls_fail`,
> the grown `ice_preplay`, `unified` in `ice_creds`).**
>
> ## Changed files (round XXXI)
> 1. `streamer/{include/kn,src}/kn_dtls.{h,c}` (NEW) — the R3-B module:
>    real body under KN_TLS_BACKEND_MBEDTLS on BOTH platforms (the
>    kn_cert discipline), honest stub elsewhere (`available()==0`,
>    loud refusal). DTLS 1.2 exactly (min=max), the kn_cert handles
>    as own_cert (R3-A kept them for this), MTU 1200, HVR automatic
>    (client side). PINNING = the double gate: the verify callback
>    clears chain flags ONLY on SHA-256(peer DER) == the pinned
>    DESCRIBE fingerprint, and poll() refuses READY unless
>    verify_result==0 AND pin_matched (latching KN_DTLS_ERR_PIN +
>    close_notify). VERIFY_OPTIONAL is REQUIRED for this pattern —
>    mbedTLS fails VERIFY_REQUIRED without a CA chain before any
>    callback (ssl_tls.c:8071); landmine documented in RTSP.md §18.
>    BIO recv returns WANT_READ on an empty queue, NEVER TIMEOUT
>    (a BIO TIMEOUT is read as an expired retransmission delay →
>    resend() → a NULL-flight segfault; landmine #2). Post-ready app
>    records are drained and counted (D-t4 — R3-C hooks here).
> 2. `streamer/tests/test_kndtls.c` (NEW, 15th suite) — self-adaptive:
>    REAL battery (31 checks) under the CMake mbedtls backend, the
>    stub contract (6) elsewhere. The in-test server is shaped like
>    the rig: HVR cookies ON, the CLIENT cert pinned server-side
>    (pinning BOTH directions), the same inject-queue BIO discipline;
>    legs: full handshake over real loopback UDP, DTLSv1.2 +
>    ECDHE-ECDSA assertions, first-flight loss in BOTH directions
>    (the timer callbacks earn their keep), wrong-pin refusal at the
>    ready gate (fatal latch + dirty verify flags preserved),
>    inject-queue overflow (drop-new, counted), mid-handshake free
>    (ASan bait), NULL guards. The server implements the documented
>    HVR contract: HELLO_VERIFY_REQUIRED → session_reset + re-armed
>    transport id (landmine #3). PLUS the `--openssl <port> <fp>` mode
>    for the cross-implementation leg.
> 3. `tools/dtls_openssl_check.py` (NEW) — the independent-stack gate
>    (sandbox-only; the target has no openssl): generates an OpenSSL
>    ECDSA P-256 self-signed cert, runs `openssl s_server -dtls1_2`,
>    drives `test_kndtls --openssl` pinned to that cert's fingerprint.
>    PASS proves our ClientHello/cookie-round/cert/Finished interop
>    with a NON-mbedTLS DTLS stack (measured: DTLSv1.2,
>    TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256, ~24 ms).
> 4. `streamer/src/knvstd_main.c` — the phase-C/D/E rework:
>    `sess_reserve_pair` (49005/49006 preferred → <= 32 dynamic
>    adjacent attempts → honest failure; the `sock_pair` evidence
>    line); SETUP's client port = unified ? bundle : video
>    (nvst_rtsp.rs:1176-1183); the BUNDLE socket is connected to the
>    SETUP peer pre-ANNOUNCE (localAddress + clientBundlePort +
>    a=candidate come from it — the reference test pins 49006) and to
>    ice_dest after; the video socket connects to the video peer at
>    media start; the save/restore reconnect dance is GONE (one peer
>    per socket for life). The cert stays ALIVE (R3-A freed it after
>    fingerprinting) and arms `kn_dtls_client` when ice + remote_fp +
>    real cert + backend all exist (`dtls_arm` line, lengths only).
>    The pump: demux PING→PONG | STUN | DTLS(20..63, kn_dtls_peek) |
>    invalid; the DTLS arm gate = the first real inbound STUN
>    (req_in||ok_in — the :8196 rule); cadence 20 ms (3×STUN+1×NATT)
>    → post-ready 100 ms NATT-only (:7837-7846); `dtls_ready` /
>    `dtls_fail` emitted FROM THE THREAD at the transition (timing is
>    evidence; stdio locks keep lines atomic); the snapshot handshake
>    (snap_req/snap_done) lets main read the counters while the
>    worker runs; `pump_persist` (non-unified + thread) keeps the
>    bundle worker alive through the media loop (D21 untouched — the
>    media pipeline stays single-threaded on its own socket);
>    teardown order = join → kn_dtls_free → close sockets → (teardown
>    label) kn_cert_free. `ice_preplay` grew pongs/persist + the six
>    dtls counters; `ice_creds` grew `unified` (answers DTLS.md open
>    question 4 even when PLAY fails). The sequential fallback and the
>    disablePlay window keep the STUN/NATT-only shape (documented).
> 5. `CMakeLists.txt` — kn_dtls.c into kncore; kndtls into the test
>    foreach (ctest 16→17 ×2); `target_compile_definitions(test_kndtls
>    PRIVATE KN_TLS_BACKEND_MBEDTLS=1)` under the mbedtls backend (the
>    suite's real section compiles only there — schannel builds run the
>    stub battery, the honest split).
> 6. `tools/build_native_tests.sh` — KNCORE += kn_dtls.c; the 15th
>    suite builds (stub) + runs as rc19.
> 7. `tools/cross_check.sh` — kn_dtls stub -c + the REAL-body legs
>    (kn_dtls.c AND test_kndtls.c's real section) under the exact
>    target flags ×2 arches whenever third_party/mbedtls exists (the
>    round-XXII rule); the stub suite link (needs -lws2_32 via
>    kn_sock.c); KCORE += kn_dtls.c.
> 8. `tests/T10_build_and_test.py` — suites += test_kndtls (15) →
>    target expectation **74** (run 12's T10 reported 72, not the
>    round-XXX-expected 73: the target harness predates the kncert
>    addition — SYNC THIS FILE from the repo before the next run).
> 9. `docs/RTSP.md` §18 — run 12 + the R3-B design record + the three
>    mbedTLS landmines. `docs/DTLS.md` — R3-B SHIPPED; open questions
>    1/2 answered, 6 added (does the rig request the client cert; does
>    it tolerate a DTLS-complete but SCTP-silent client — D-t4).
> 10. `PROJECT-STATE.md` — status/Next steps/session log round XXXI.
>
> ## Verification status (what ran WHERE, round XXXI)
> * Sandbox: native 15 suites PASS (stub legs); cross mingw x64+i686
>   -Werror PASS incl. the kn_dtls real-body + test real-section legs;
>   ctest 17/17 ×2 backends (mbedtls: test_kndtls REAL 31/31;
>   schannel: stub 6/6); ASan+UBSan+LSan clean on the real battery;
>   the OpenSSL interop leg PASS; paranoid knvstd legs
>   (-Wunused-but-set-variable -Wformat-truncation=2) clean host+cross;
>   sandbox_session_e2e 43/43 (unchanged by design — the rig doesn't
>   speak DTLS; upgrading it is an R3-C-era candidate); T10 POSIX dry
>   ALL PASS 64; T13 ALL PASS 63.
> * Target (PENDING — round XXXII asks for): `python
>   tests\T10_build_and_test.py` with the SYNCED harness (expect ALL
>   PASS **74**, zero warnings — test_kndtls runs the REAL battery in
>   the target's mbedtls build) then **R7 run 13** (the same --session
>   command; watch sock_pair → dtls_arm → dtls_ready/dtls_fail → PLAY).
>
> ## Notes for the target build
> * No new link libraries: kn_dtls uses the mbedtls/mbedx509/
>   mbedcrypto statics kncore already links (MBEDTLS_SSL_PROTO_DTLS +
>   MBEDTLS_SSL_COOKIE_C are ON in the pinned 3.6.0 default config).
> * No new PE imports (F1 holds): mbedTLS is static; the pump thread
>   reuses the round-XXVIII kernel32 CreateThread.
> * The kn_dtls real body compiles ONLY with the backend define — the
>   target's T10 (mbedtls default) exercises it for real; a schannel
>   fallback build gets the honest stub and knvstd logs
>   `dtls_arm {"armed":0,"reason":"backend_unavailable"}`.



# Round XXX delta, rev 20 (R7 run 11 = the FINAL elimination datum: PLAY refused with a saturated pump + TOTAL WS silence → R3-A SHIPPED: kn_cert, the real ECDSA P-256 self-signed identity replacing D-s1's placeholder fingerprint)

> **Rev 20 supersedes rev 1-19.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/DTLS.md`** (R3-A now SHIPPED, R3-B/C/D
> planned), `docs/RTSP.md` §17 (run 11 + R3-A), §16 (the elimination
> table), §15/§12.
>
> **Round XXX headline: run 11 completed the elimination — a third rig
> (77-111-249-146) took the full chain to ANNOUNCE 200 (4137 B — the
> new 4/22 identity shapes live on the wire) and then held PLAY in
> TOTAL silence (timeout; ws_pings_in 0, ws_closes_in 0, frames_out 5
> vs text 4) while our pump thread saturated the UDP path beside it
> (pump_ms 20920, 2820+940 sent, 126/126 rig probes answered, 3753 rig
> successes validated, invalid 0). Three rigs, three refusals, one
> constant: no real DTLS behind the announced fingerprint. So R3-A
> SHIPPED: `kn_cert` — the portable-mbedTLS self-signed identity
> (ECDSA P-256, SHA-256 fingerprint over the exact DER in the
> announced colon-hex form, fixed 2020-2035 validity, random serial;
> parsed x509+pk kept for R3-B behind opaque handles; a selfcheck KAT
> proves SHA-256(der)==fp + byte-identical re-parse + sign/verify
> through the certificate's public key). The CMake mbedtls backend
> compiles the REAL body on both platforms → the sandbox ctest runs
> the real battery (10 checks, ASan+UBSan clean after it caught a
> double-free in the first test draft); every other build gets the
> honest stub (4 checks). knvstd announces the real fingerprint when
> available and logs it: the new `identity` event
> ({"ufrag_len":4,"pwd_len":22,"fp_len":95,"cert":"real"}).**
>
> ## Changed files (round XXX)
> 1. `streamer/{include/kn,src}/kn_cert.{h,c}` (NEW) — the R3-A module
>    (real body + honest stub, the kn_net discipline; the header is
>    mbedTLS-free — R3-B casts the opaque handles back).
> 2. `streamer/tests/test_kncert.c` (NEW) — the FOURTEENTH suite,
>    self-adaptive: real battery under the mbedtls backend (10 checks),
>    stub contract elsewhere (4).
> 3. `streamer/src/knvstd_main.c` — the identity block: kn_cert
>    generate → fingerprint → free (R3-A only announces; R3-B keeps it
>    alive for the handshake), the `identity` evidence line, the
>    placeholder fallback stays honest.
> 4. Build wiring: `CMakeLists.txt` (kn_cert.c into kncore; kncert into
>    the test foreach → ctest 15→**16** both backends),
>    `tools/build_native_tests.sh` (KNCORE + the stub-mode suite run,
>    rc18), `tools/cross_check.sh` (the stub -c leg + the REAL-body
>    cross-compile under the exact target flags whenever
>    third_party/mbedtls exists — the round-XXII rule — + the suite
>    link), `tests/T10_build_and_test.py` (suites += test_kncert →
>    **target expectation 72 → 73**).
> 5. `docs/RTSP.md` §17, `docs/DTLS.md` (R3-A marked SHIPPED), this
>    file, `PROJECT-STATE.md`.
>
> ## Verification (round XXX, sandbox)
> **test_kncert real battery 10/10 under ASan+UBSan** (CMake mbedtls
> backend; it caught the double-free pre-ship) · stub contract 4/4 in
> the shell gates · native **14 suites** PASS · cross x64+i686 -Werror
> PASS incl. the kn_cert real-body legs ×2 · paranoid knvstd legs ×3
> clean · CMake fresh both backends 0 warnings + ctest **16/16 ×2**
> (mbedtls: real 10/10; schannel: stub 4/4 — the honest split) ·
> sandbox_session_e2e **43/43** (unchanged; the rig doesn't validate
> fingerprints yet — an R3-B-era upgrade candidate) · T10 POSIX dry
> ALL PASS **63** (62 + the new suite).
> **Target expectations:** next T10 → ALL PASS **73** (suite +1; zero
> warnings). Then **R7 run 12**: same command — watch the `identity`
> line (`"cert":"real"`, fp_len 95). If PLAY still refuses (the
> leading theory: the gate needs the actual handshake), R3-B (kn_dtls
> on the bundle socket inside the pump thread) is the decisive round.

---

# Round XXIX delta, rev 19 (R7 run 10: the pump thread live-proven, PLAY went 455→SILENCE → the elimination table is empty except D-s1; R3 (real DTLS) promoted to the stage-2 PLAY blocker — docs/DTLS.md; the official ICE identity shapes mirrored)

> **Rev 19 supersedes rev 1-18.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/DTLS.md`** (NEW — the R3 spike plan),
> `docs/RTSP.md` §16 (run 10 + the elimination table), §15, §12.
>
> **Round XXIX headline: run 10 closed the last transport difference —
> the pump thread ran through the ENTIRE 20 s PLAY window
> (`"thread":1`, pump_ms 20525, 2760 STUN + 920 NATT, the rig's 123
> probes ALL answered, 3671 rig successes validated, invalid 0) — and
> PLAY got NO response at all (rc -3 timeout, ws_text stuck at 4, no
> close frame). A different rig than run 9 refused differently
> (instant 455 vs hold-until-timeout), same root state: the transport
> never reaches "PLAY-valid". With the wire shape, the bursts, their
> continuity through the exchange and the credential ladder ALL proven
> live, the elimination table has one row left: the announced DTLS
> identity (D-s1 placeholder, no DTLS stack). The reference announces
> a REAL certificate's fingerprint and runs ICE+DTLS on the bundle from
> receiver-spawn time (nvst.rs:5562-5603, :6106-6160 — client is ICE
> CONTROLLING, remote fingerprint PINNED from DESCRIBE, video SRTP
> keys stay ANNOUNCE-derived, DTLS carries SCTP/rtcp1+input). R3 is
> promoted to the stage-2 PLAY blocker; docs/DTLS.md phases it
> (R3-A portable-mbedTLS certificate identity → R3-B DTLS client on
> the bundle → R3-C usrsctp channels → R3-D WS control pings), each
> phase sandbox-KAT'd before its live verdict, with the round-XXII
> gate rule baked in. Small round-XXIX deltas while the verdict
> stands: the official ICE identity shapes (4-char ufrag / 22-char
> password over the base64-ish alphabet — nvst.rs:5545-5565; our
> 16/24-hex was tolerated but Bifrost length-checks 16-char ufrags),
> a 400 ms pump warm-up so PLAY can never outrun the first bursts
> (run 10 raced the thread start), and the full WS counter set in
> rtsp_detail (pings/pongs/closes/frames_out — run 11 will show what
> the rig does on WS during the hold).**
>
> ## Changed files (round XXIX)
> 1. `streamer/src/knvstd_main.c` — `sess_local_identity` mirrors
>    generate_gfn_local_ice_credentials exactly (alphabet, 4/22);
>    the PLAY path gains the 400 ms warm-up when the pump thread
>    spawns; `sess_rtsp_detail` reports ws_pings_in/ws_pongs_in/
>    ws_pongs_out/ws_closes_in/ws_frames_out.
> 2. `docs/DTLS.md` (NEW — the R3 plan with provenance + open
>    questions), `docs/RTSP.md` §16, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXIX, sandbox)
> native 13 suites PASS (libraries unchanged — the identity/warm-up/
> counters are knvstd glue; E2E keeps its own static identity by
> design) · cross x64+i686 `-Werror` PASS · paranoid
> `-Wunused-but-set-variable -Wformat-truncation=2` clean on
> knvstd_main.c ×3 compilers · sandbox_session_e2e **43/43** · CMake
> both backends 0 warnings + ctest 15/15 ×2 · T10 dry ALL PASS 62.
> **Target expectations:** next T10 → ALL PASS **72**, zero warnings.
> Then **R7 run 11** (same command): the expected verdict is PLAY
> still refused (455 or timeout) — the FINAL elimination datum — with
> the enriched counters showing the rig's WS behaviour during the
> hold; queue/ICE evidence keeps accumulating. After that R3-A starts
> (docs/DTLS.md).

---

# Round XXVIII delta, rev 18 (R7 run 9: the control node MIGRATES constantly — re-learning live-proven, ZERO 503s; ICE fully bidirectional yet PLAY still 455 → the pump goes concurrent, PLAY goes one-shot, the Mjolnir NATT keepalive lands in the media loop)

> **Rev 18 supersedes rev 1-17.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §15** (run 9 + this round), §14 (run
> 8), §13 (the 503-window policy), §12 (the pump's birth).
>
> **Round XXVIII headline: run 9 validated TWO rounds of fixes live and
> exposed the next layer. (1) The XXVII control-node re-learning fired
> ~25 times in one wait — the session's nominated front migrates across
> np-ams/sth/lon/par/frk almost every poll — and with it the 503 class
> VANISHED (zero 503s across ~28 polls; runs 7/8 were us pinned to a
> node the record had drifted from, which is what its ip:null skeleton
> was saying). (2) The XXV STUN toolkit is live-proven byte-correct:
> ice_creds enabled with the pingVersion-6 bundle-username ladder leg
> (lengths 13/13/13 — the real DESCRIBE carries iceUsernameFragment +
> serverBundlePort), and ice_preplay shows a fully bidirectional,
> integrity-validated exchange (stun 147, natt 49, rig requests 99 —
> ALL answered, rig successes 327, invalid 0). (3) PLAY STILL 455 — so
> burst existence is necessary but not sufficient; the remaining deltas
> vs the reference: our sequential pump goes QUIET during the PLAY
> exchange (the reference's receiver thread never stops bursting) and
> D-s1 (no real DTLS identity — unproven either way, the rig sent no
> DTLS: invalid_in 0). (4) The 455 retry hit a DEAD WS (rc -2,
> ws_close -1, no close frame — the rig drops the RTSP context after
> refusing). Fixes: the pump moves to a bounded worker thread spanning
> exactly the PLAY exchange (kernel32/pthread; joined before the media
> loop — D21 untouched; honest sequential fallback), PLAY becomes
> ONE-SHOT, and the media loop gains the reference's Mjolnir NATT
> keepalive (nvst.rs:8849-8905 — "the pings are what ROUTES video
> here": USERNAME {setup payload}:{local ufrag} to the video peer, 20
> ms pre-first-inbound then 100 ms, inline in the loop's tick).**
>
> ## Changed files (round XXVIII)
> 1. `streamer/src/knvstd_main.c` — the pump refactor (sess_pump ctx +
>    sess_ice_pump_run core + thread entry/start/stop + the sequential
>    wrapper kept as fallback), the one-shot PLAY block (retry loop
>    removed; `ice_preplay` gains `"thread"`), the phase-E NATT
>    keepalive (+ the `natt_keepalive {"sent":N}` evidence line; the
>    summary schema untouched for make_rx_capture --verify), the
>    relearn counter (`cm_control` `"n"`, `ready` `"relearns"`), and the
>    POSIX `<pthread.h>` include (Win32 uses the F1-set kernel32
>    thread APIs — no new imports).
> 2. `docs/RTSP.md` §15, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXVIII, sandbox)
> native 13 suites PASS (counts unchanged: knsock 98, knrtsp 150, kncm
> 148 — the delta is knvstd glue) · cross x64+i686 `-Werror` PASS incl.
> the paranoid knvstd legs (host+x64+i686, `-Wunused-but-set-variable`
> `-Wformat-truncation=2`) · sandbox_session_e2e **43/43** (keeps its
> own sequential pump = the independent wire-shape implementation; the
> thread scheduling is Win32-target territory — POSIX session mode
> refuses at the TLS gate) · CMake fresh both backends 0 warnings +
> ctest **15/15 ×2** · T10 POSIX dry ALL PASS **62**.
> **Target expectations:** next T10 → ALL PASS **72**, zero warnings.
> Then **R7 run 10**: PLAY 200 → `natt_keepalive.sent` + real Mjolnir
> SRTP in the summary (R32's final stage-2 gate + the R34 firewall
> answer); PLAY still 455 with `"thread":1` + rich ice counters → the
> DTLS identity (D-s1) becomes the prime suspect and stage-3 R3 becomes
> the prerequisite for PLAY.

---

# Round XXVII delta, rev 17 (R7 run 8: the 503 body speaks — an app-level session skeleton with sessionControlInfo.ip:null; the T10-r20 warning fixed + gated permanently; control-node re-learning; --session-keep)

> **Rev 17 supersedes rev 1-16.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §14** (run 8 + this round), §13 (the
> 503-window policy), §12 (the pre-PLAY pump).
>
> **Round XXVII headline: run 8 proved the round-XXVI policy end-to-end
> — the client RODE the 503 window (ten transient rounds, the D-s4
> backoff visible: gap_ms 6000→9000→13500→20000) until the user's
> Ctrl-C, then cm_cancel 200. And the body excerpt made the 503 TALK:
> it is CloudMatch's OWN JSON — a session skeleton with
> `sessionControlInfo.ip:null` and the resourcePath still echoing the
> id (the front knows the record but cannot name its owning state — the
> §13 seat-transition window from the inside; the record is alive, the
> same-window DELETE answered 200). This round parses the deep fields
> (status/errorCode/requestStatus incl. unifiedErrorCode — now filled
> even on rejected payloads) into the cm_poll_error line, fixes the
> T10-r20 warning (write-only consec_transient — target GCC 16.2's
> -Wunused-but-set-variable is NOT in sandbox GCC 12's -Wall; the fifth
> target catch, and both gate scripts now carry a PERMANENT paranoid
> knvstd leg), closes a real reference-fidelity gap (the control node is
> RE-LEARNED from every successful poll like store_active :1333-1336 —
> a mid-queue node migration is followed, ip:null keeps the current
> host), and adds --session-keep (skip the abort cancel; the user's
> browser-RESUME workflow).**
>
> ## Changed files (round XXVII)
> 1. `streamer/src/knvstd_main.c` — consec_transient removed (the T10
>    catch); the cm_poll_error diag-view (parses non-200 JSON bodies:
>    `unified_error`/`sess_status`/`err_code` + desc fallback; excerpt
>    160→240); per-poll control-node re-learning (+ `cm_control`
>    `source:"relearned"` events); `--session-keep` (usage text incl.).
> 2. `streamer/{include/kn,src}/kn_cm.*` — `kn_cm_session.error_code` +
>    `.unified_error`; status/phase/errorCode filled BEFORE the
>    sessionId gate so rejected payloads stay diagnosable.
> 3. `streamer/tests/test_kncm.c` — the run-8 503-skeleton fixture
>    (rejected but fully diagnosed; ip:null → no control host),
>    unifiedErrorCode in the SESSION_LIMIT fixture, errorCode pinned on
>    the run-5 capture fixture: **147 → 148 checks**.
> 4. `tools/build_native_tests.sh` + `tools/cross_check.sh` — the
>    PERMANENT paranoid knvstd leg (`-Werror
>    -Wunused-but-set-variable -Wformat-truncation=2`; host + both
>    cross arches) — the round-XXII gate-hardening pattern applied to
>    the fifth target-catch class.
> 5. `docs/RTSP.md` §14, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXVII, sandbox)
> native 13 suites PASS (kncm 148, knrtsp 150, knsock 98) · cross
> x64+i686 `-Werror` PASS incl. the new paranoid legs (3 .o produced) ·
> sandbox_session_e2e **43/43** · CMake fresh both backends 0 warnings +
> ctest **15/15 ×2** · T10 POSIX dry ALL PASS **62**.
> **Target expectations:** next T10 → ALL PASS **72** with ZERO
> warnings (the r20 failure was exactly the removed counter). Then
> **R7 run 9** — the round-XXV PLAY pump is STILL awaiting its first
> live try; expect either the seat transition → ice_creds/ice_preplay →
> PLAY(455?)→200 → first media, or a 503 window whose parsed fields
> (sess_status/err_code/unified_error/cloud_status/desc) name the
> cloud-side state — and a `relearned` cm_control line if the session
> migrates nodes.

---

# Round XXVI delta, rev 16 (R7 run 7: the ~2-minute seat-transition 503 storm — transient statuses are now OUTLIVED (D-s4), hard rejections and transport deaths stay bounded, the 503 body is captured)

> **Rev 16 supersedes rev 1-15.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §13** (the run-7 measurement + the
> failure-class policy), §12 (run 6 + the pre-PLAY pump), §11/§10.
>
> **Round XXVI headline: run 7 (two attempts) measured a new cloud-side
> window — after ~2 minutes of static `seat_step:1` (the SAME duration
> runs 5/6 spent there before transitioning to step 3), every GET poll
> started answering 503 while a same-moment DELETE to the same host
> answered 200 (and the user watched the session disappear from the web
> UI — the round-XXIII cancel is target-proven; the immediate re-create
> hit no SESSION_LIMIT). The old policy killed the wait after 5 such
> rounds (~30 s) — right inside the window the seat transition needs.
> The reference never gives up on transient poll errors (the UI cadence
> just continues; only 404 clears). Round XXVI mirrors that with a
> failure-CLASS policy: transient 4xx/5xx are OUTLIVED until
> queue_timeout, transport deaths bounded at 10 rounds, hard HTTP/cloud
> rejections bounded at 5 (new stop_reason `cm_poll_rejected`), a gentle
> 4→6→9→13.5→20 s backoff while failing (D-s4), and the `cm_poll_error`
> line now carries the response BODY excerpt + class + next gap — run 8
> will name the 503's sender.**
>
> ## Changed files (round XXVI)
> 1. `streamer/src/knvstd_main.c` — phase A poll loop only: the
>    failure-class counters (consec_transient/transport/hard) + the new
>    give-up bounds, `sess_poll_gap` (the D-s4 backoff), the upgraded
>    `cm_poll_error` evidence line (body/class/gap_ms; `cm_json` is
>    cleared per round so the excerpt is always THIS round's), and the
>    `cm_poll_rejected` stop_reason. No library/API changes; phases B-E
>    and the round-XXV pump untouched.
> 2. `docs/RTSP.md` §13, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXVI, sandbox)
> native 13 suites PASS (knsock 98, knrtsp 150, kncm 147 — unchanged,
> the delta is knvstd glue) · cross x64+i686 `-Werror` PASS (with the
> kn_net_mbedtls legs — third_party/mbedtls present) · paranoid
> `-Wformat-truncation=2` clean on knvstd_main.c (host -O1 + mingw -O2
> ×2) · sandbox_session_e2e **43/43** · CMake 0 warnings + ctest
> **15/15 ×2** · T10 POSIX dry ALL PASS **62**. The policy itself is
> Win32-TLS-gated — its proof is run 8.
> **Target expectations:** next T10 → ALL PASS **72** (suite count
> unchanged). Then **R7 run 8** — either the step-3 transition → PLAY →
> first media, or a ridden-out 503 window whose `body` excerpts name the
> source; `queue_timeout` (600 s) + `cm_cancel` bound the worst case.

---

# Round XXV delta, rev 15 (R7 run 6: DESCRIBE/SETUP/ANNOUNCE 200 LIVE — PLAY 455 root-caused: the reference's pre-PLAY hole-punch burst was missing → the STUN toolkit + the credential ladder + the pump)

> **Rev 15 supersedes rev 1-14.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §12** (the run-6 measurement + the
> fix), §11 (run 5), §10 (runs 1-4).
>
> **Round XXV headline: run 6 walked the whole live signaling chain —
> queue → ready → OPTIONS/DESCRIBE/SETUP/ANNOUNCE all 200 on the real
> rig (the round-XXIV DESCRIBE fix confirmed live; SETUP peered on the
> first sweep round; ANNOUNCE carried the 4164-byte negotiated SDP with
> the D-s1 placeholder identity and was ACCEPTED) — and PLAY was
> rejected 455 "Method Not Valid in This State" with a healthy WS layer
> (rtsp_detail: ws_err 0, no close, 5 text frames). Root cause: the
> reference spawns its media receivers BETWEEN announce() and PLAY
> (lib.rs:894→937→1117) and the bundle thread immediately fires the
> hole-punch burst (3× STUN binding + 1× NATT ping per 20 ms,
> nvst.rs:7438/:7842-7900, dest = bundle_peer() :1880) — we sent PLAY
> with zero UDP egress. Fixed with the sequential pre-PLAY pump (D-s2)
> + the reference's exact STUN wire shapes + its remote-ufrag ladder.
> Bonus catch: the ladder fixtures exposed a pre-existing
> kn_rtsp_sdp_attribute bug (first-match instead of the reference's
> LAST-match across ";;" segments). The user also proved R23 RESUME is
> real: run 6's session was left alive and played in the browser.**
>
> ## Changed files (round XXV)
> 1. `streamer/{include/kn,src}/kn_sock.*` — the STUN toolkit: crc32
>    (IEEE reflected), stun_build (byte-exact port of
>    build_authenticated_stun_packet :4977-5012 — MI HMAC-SHA1 with the
>    RFC 5389 §10.5 length adjustment + FINGERPRINT CRC^0x5354554E),
>    xor_addr_v4, inspect (the :5131-5146 gate — magic/exact-length/
>    MANDATORY fingerprint), check_integrity (:139-160, const-time).
>    test_knsock now links kn_sha1 (+ the vendored sha1) — build wiring
>    updated in both gate scripts.
> 2. `streamer/{include/kn,src}/kn_rtsp.*` — the ladder: increment_hex
>    (:2305), bundle_natt_username (:1327), resolve_remote_ufrag
>    (:2329); describe extracts ufrag_v1 + server_bundle_port; **the
>    sdp_attribute LAST-match fix** (".last()" semantics — pinned by a
>    dedicated check; media_control legitimately stays first-segment).
> 3. `streamer/src/knvstd_main.c` — phase C/D: `ice_creds` (lengths
>    only) + the credential gate (:1729, D-s3 honest skip on the
>    ping6-no-payload error case :1248) + bundle-peer re-connect/
>    restore + `sess_ice_pump` (20 ms bursts, answers the rig's
>    requests, counts successes) + PLAY 455-retry (3 attempts, 600 ms
>    re-pump) + the `ice_preplay` counters line + `attempt` in the PLAY
>    event.
> 4. `streamer/tests/test_knsock.c` — test_stun: goldens from an
>    independent Python implementation (hmac/hashlib/zlib), RFC 2202
>    HMAC, tamper/short/no-fingerprint rejects, xor-addr round-trip:
>    **82 → 98 checks**.
> 5. `streamer/tests/test_knrtsp.c` — test_ice_ladder (fixtures
>    transcribed from the reference's own tests :3860-3920) + the
>    last-wins sdp pin: **132 → 150 checks**.
> 6. `tools/fake_rig.py` + `tools/sandbox_session_e2e.c` — the rig
>    runs an independent Python STUN validator/responder on the media
>    socket and GATES PLAY (first PLAY always 455; later need ≥1
>    validated request); the harness pumps, asserts the ladder
>    (a1b24800→a1b24801) and the 455→200 sequence: **36 → 43 checks**
>    (rig evidence: ice_req 12, ice_resp 12, play_455 1, plays 1).
> 7. `docs/RTSP.md` §12, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXV, sandbox)
> native **13 suites PASS** (knsock 98, knrtsp 150, kncm 147) · cross
> x64+i686 `-Werror` PASS · ASan+UBSan clean on both touched suites
> (the single known vendored sha1.c:26 note aside) · kn_rtsp.c +
> knvstd_main.c paranoid `-Wformat-truncation=2` clean (host -O1 +
> mingw -O2 ×2); kn_sock.c's two =2 hits are PRE-EXISTING round-XVII
> honest-truncation error strings (standard gates clean — left alone,
> churn discipline) · CMake fresh both backends 0 warnings + ctest
> **15/15 ×2** · sandbox_session_e2e **43/43** · T10 POSIX dry ALL PASS
> **62** (baseline).
> **Target expectations:** next T10 → ALL PASS **72** (suite count
> unchanged). Then **R7 run 7**: expect `ice_creds enabled:1`,
> `ice_preplay` counters > 0, PLAY attempt 1 possibly 455 → attempt 2
> 200, then `handoff` + the media loop (the first real Mjolnir SRTP →
> kn_rx → AUs = R32's final stage-2 gate). If PLAY still 455s:
> `ice_preplay.ok_in` (did the rig answer?) + `ice_creds` (which ladder
> leg fired) localize it.

---

# Round XXIV delta, rev 14 (R7 run 5: FIRST READY SESSION — the queue fix confirmed live; DESCRIBE died on the 8 KiB per-message scratch → fixed + binary frames + Host:port + rtsp_detail instrumentation)

> **Rev 14 supersedes rev 1-13.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §11** (the run-5 measurement + the
> fix), §10 (run 1-4 + the control-host fix), §9 (round XXI deviations).
>
> **Round XXIV headline: R7 run 5 took the round-XXIII fix to the live
> cloud — `cm_control` switched to np-waw-01 (source sessionControlInfo),
> the session progressed (seat_step 1→3) and reached `status:2 ready`
> with a real rtsps endpoint; the RTSPS/WSS transport then answered
> OPTIONS 200 on a REAL GFN rig and died at `DESCRIBE rc:-2 status:0`.
> Root-cause candidates (both produce exactly that signature at the
> first big response; both fixed): kn_rtsp_request read each WS message
> into an 8 KiB scratch — real DESCRIBE SDPs are bigger, while the
> reference accumulates whole messages up to the 4 MiB total
> (nvst_rtsp.rs:559-566, :614-623) — and the adapters refused BINARY
> frames the reference accepts (:621). Also fixed: the Host header now
> carries :port like the reference (:1070-1076). Failed rtsp steps now
> emit an `rtsp_detail` evidence line (kn_rtsp err + WS close code +
> frame counters), so run 6 measures even if it fails elsewhere.**
>
> ## Changed files (round XXIV)
> 1. `streamer/src/kn_rtsp.c` — `kn_rtsp_request` reads DIRECTLY into
>    the accumulation buffer's tail (no per-message cap below the 4 MiB
>    total; runaway-shim guard kept; the seam contract is unchanged).
> 2. `streamer/src/knvstd_main.c` — `sess_ws_recv_text` accepts
>    TEXT|BINARY; Host = the rtsps authority WITH the port; NEW
>    `sess_rtsp_detail` (err[160] + ws_err/ws_close/ws_text/ws_bin)
>    emitted at all five step-failure sites (OPTIONS/DESCRIBE/SETUP/
>    ANNOUNCE/PLAY).
> 3. `tools/sandbox_session_e2e.c` — the same binary acceptance + Host
>    authority (the two implementations stay mirrored).
> 4. `tools/fake_rig.py` — the DESCRIBE SDP is padded to ~11 KiB and
>    delivered as TWO binary messages (the run-5 shape, exercised over
>    real sockets by the E2E).
> 5. `streamer/tests/test_knrtsp.c` — `test_big_messages`: a >19 KiB
>    single-message DESCRIBE parses (the old scratch died here) + the
>    same response split across two messages accumulates: **127 → 132
>    checks**.
> 6. `docs/RTSP.md` §11 (the run-5 record incl. the queue-position
>    observation: the NATIVE queue published step transitions, not
>    per-poll ticks — NOT a defect, keep observing), this file,
>    `PROJECT-STATE.md`.
>
> ## Verification (round XXIV, sandbox)
> native 13 suites PASS (test_knrtsp 132/132, test_kncm 147/147) ·
> cross x64+i686 `-Werror` PASS · paranoid `-Wformat-truncation=2` clean
> (kn_rtsp.c/knvstd_main.c, host -O1 + mingw -O2 both arches) · CMake
> 0 warnings + ctest **15/15 ×2** · sandbox_session_e2e **36/36** with
> the new rig behaviour (binary + split + padded SDP over real loopback
> sockets; announce key echo + plays=1 asserted by the rig) · T10 POSIX
> dry-run ALL PASS **62** (baseline).
> **Target expectations:** T10 r19 → ALL PASS **72** (suite count
> unchanged). Then **R7 run 6**: same command; expect DESCRIBE 200 →
> the SETUP sweep → ANNOUNCE (D-s1's first real verdict) → PLAY → the
> media loop; any failure now names its stage in `rtsp_detail`.

---

# Round XXIII delta, rev 13 (R7 runs 1-4: the first REAL CloudMatch traffic — the control-host bug FOUND + FIXED)

> **Rev 13 supersedes rev 1-12b.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §10** (the R7 measurement + the fix),
> §9 (round XXI status + deviations D-s1/D-cm1..3), `docs/AUTH.md`.
>
> **Round XXIII headline: the user ran `knvstd --session` against the
> LIVE production backend for the first time (T10 r17 build, ALL PASS 72;
> a real pasted GFNJWT + appId 107057914). Create works — 4/4 real
> sessions with plausible live queue positions. The queue wait FAILED:
> polls froze at a stale queuePosition (40+ identical lines) and ended in
> `cm_poll_failed`. Root cause: the poll loop stayed on the global create
> front (`prod.cloudmatchbeta.nvidiagrid.net`) instead of switching to
> the REGIONAL control node the create response nominates
> (`sessionControlInfo.ip` — `np-waw-01.cloudmatchbeta.nvidiagrid.net`
> in the user's DevTools capture; exactly where the official client polls
> at ~4.1 s and exactly what cloudmatch.rs does: control_base learned at
> :401-414/:1149-1152/:1966-1984, every poll goes there :652-656).
> Fixed + the poll loop hardened to the reference's retry/404 semantics
> + a best-effort DELETE cancel so an aborted wait no longer orphans a
> session (runs 1-4 each left one behind).**
>
> ## Changed files (round XXIII)
> 1. `streamer/{include/kn,src}/kn_cm.*` — `kn_cm_session` learns
>    `control_host/control_port/have_control` (sessionControlInfo) and
>    `req_status_code/req_status_desc` (requestStatus — filled even when
>    the session extraction fails, so a rejected payload still says why);
>    NEW `kn_cm_control_base` (the trust mirror of
>    `trusted_learned_server_base` + `public_session_host/_ipv4`,
>    cloudmatch.rs:2400-2456: https-only, port 443, private/loopback/
>    link-local/CGNAT/multicast/reserved/.local/userinfo/path all
>    rejected — a response can never redirect polling into a private
>    address; 0 = fall back to the create host, which is also what keeps
>    the loopback fake-rig E2E unchanged); NEW `kn_cm_cancel` (DELETE
>    `v2/session/{id}` = the reference's stop_at :789-800; `cm_round`
>    now tolerates `out == NULL`).
> 2. `streamer/src/knvstd_main.c` — phase A: the poll-target switch (new
>    `cm_control` JSONL event = the R7 evidence line), cadence 2 s → 4 s
>    (`SESS_POLL_MS`, the capture's Δ≈4.1 s), per-round 3-attempt
>    transient retry (408/425/429/5xx, 250/750 ms — get_session
>    :1369-1394), 404 → `session_not_found`, requestStatus.statusCode≠1
>    → failed round with the cloud's description in a `cm_poll_error`
>    event, failed rounds never clobber the live session state
>    (poll-into-temp + commit), 4 consecutive bad rounds tolerated before
>    `cm_poll_failed`, the `not_ready` line carries `rc`+`http_status`,
>    the `cm_create` error line carries the cloud's requestStatus
>    (cloud_status 11 = SESSION_LIMIT — runs 1-4 orphaned four queued
>    sessions), best-effort `cm_cancel` on every not_ready exit except
>    404, `cm_created` now includes the initial queue_position,
>    `--session-queue-timeout` default 180 → 600 s.
> 3. `streamer/tests/test_kncm.c` — NEW control-base battery (the exact
>    R7 capture fixture + the full accept/reject matrix + requestStatus-
>    on-failed-extract) and the cancel wire checks (DELETE line, Host,
>    GFNJWT header, 204/404/200-with-body, empty-id guard): **91 → 147
>    checks**.
> 4. `tools/fake_rig.py` — answers `DELETE /v2/session/{id}` with 204 No
>    Content (the validate_delete_response shape); E2E legs unchanged.
> 5. `docs/RTSP.md` §10 (R7 runs 1-4 + the fix + the honest not-covered
>    list), this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXIII, sandbox)
> native **13 suites PASS** (test_kncm 147/147, incl. ASan+UBSan+LSan
> run) · cross x64+i686 `-Werror` PASS · kn_cm.c + knvstd_main.c clean
> under `-Wformat-truncation=2` at -O1 host / -O2 mingw (x64+i686) ·
> CMake fresh configure both backends: 0 warnings, ctest **15/15 ×2** ·
> sandbox_session_e2e **36/36** (exercises the control-host FALLBACK
> path: the rig's 127.0.0.1 is rejected by the trust gate → create-host;
> the ACCEPT path is unit-pinned by the capture fixture — a public-host
> loopback rig is impossible without DNS games) · T10 POSIX dry-run ALL
> PASS **62** (62 is also the pre-delta baseline on this tree — the
> round-XXI "61" predates a check added since; verified by a stashed
> rerun).
> **Target expectations:** T10 r18 → ALL PASS **72** (suite count
> unchanged; test_kncm's internal count is not a T10 check). Then **R7
> run 5** = the same `--session` command: expect `cm_control` with
> `source:"sessionControlInfo"`, a MOVING queue_position, and — if a
> seat arrives — phase B onward (D-s1 gets its first real answer there).
>
> ## Known gaps carried forward (honest)
> * Phases B-E aborts still don't cancel the session (churn discipline;
>   next round).
> * IPv6 trust checks are prefix-class approximations, not a full
>   IpAddr decode (documented in kn_cm.c; reject-direction only affects
>   exotic control hosts, which fall back to the create host).
> * D-cm1/D-cm2 measured OK AT CREATE (4/4 real sessions); seat/streaming
>   stages still unmeasured.

---

# Round XXII delta, rev 12b (T10 r16 build-fix + gate hardening)

> **Rev 12b (round XXII):** target T10 r16 caught a round-XXI uncounted-
> replace edit in `kn_net_mbedtls.c` (port snippet landed in the DNS
> lookup; the Win32 body had never been sandbox-compiled). Fixed (DNS
> literal restored) + a second latent bug fixed (missing `<stdlib.h>` —
> i686-only) + **`cross_check.sh` now cross-compiles kn_net_mbedtls.c on
> both arches whenever third_party/mbedtls exists** (the permanent gap
> closure). All gates re-run green; target expects T10 r17 ALL PASS 72.
> Full story: PROJECT-STATE.md round-XXII log, artifacts/T10-round16/.

---

# Round XXI delta, rev 12 (STAGE 2 PRODUCTION PATH: TLS stream + kn_cm/kn_http/kn_json + fake-rig E2E + knvstd --session)

> **Rev 12 supersedes rev 1-11.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §9** (round-XXI status + deviations
> D-s1/D-cm1..3) and **`docs/AUTH.md`** (the OAuth device-flow research,
> login implementation = stage 4).
>
> **Round XXI headline: the stage-2 production signaling path is BUILT
> end-to-end — CloudMatch (POST v2/session + poll + session extraction)
> over a persistent verified TLS-1.2 stream, RTSPS-over-WSS with the
> round-XX cores, the SRTP/peer handoff into kn_rx, and `knvstd
> --session` glue with the connected-UDP media loop — proven against an
> independent Python rig over real loopback sockets (36 checks) before
> any target run.** The only remaining unknowns are cloud-side (R7) and
> documented as D-s1/D-cm1/D-cm2.
>
> ## New files (round XXI)
> 1. `streamer/{include/kn,src}/kn_json.*` — minimal zero-alloc JSON
>    reader (1024-node pool, depth 32, raw spans, unescape incl.
>    surrogate pairs, int64-exact).
> 2. `streamer/{include/kn,src}/kn_http.*` — HTTP/1.1 one-shot client on
>    a stream seam (CL/chunked/read-to-EOF, resumable chunk parser,
>    injected clock).
> 3. `streamer/{include/kn,src}/kn_cm.*` — CloudMatch: the create body
>    (field-for-field with build_create_body), GFNJWT + nv-client header
>    set (Content-Type text/plain quirk), create/poll, and the
>    session_info extraction subset (rtspsEndpoints DERIVATION from
>    connectionInfo usage 16/14+alp{1,6}, media usage 2|17, queue
>    4-source fallback, ready/resumable, phase table).
> 4. `streamer/tests/test_kncm.c` — the THIRTEENTH suite (91 checks:
>    json/http/cm sections, fixtures per cloudmatch.rs:4073-4085).
> 5. `tools/fake_rig.py` — the independent Python rig (HTTP + WS server,
>    own RFC 6455 framing; asserts the ANNOUNCE key echo + PLAY count).
> 6. `tools/sandbox_session_e2e.c` — sandbox-only E2E harness (36
>    checks) driving kn_cm→kn_ws→kn_rtsp→handoff→connected UDP against
>    the rig over real sockets.
> 7. `docs/AUTH.md` — OAuth device-flow research record (endpoints,
>    forms, headers, token semantics; implementation = stage 4).
>
> ## Changed files (round XXI)
> 1. `streamer/include/kn/kn_net.h` + `src/kn_net_mbedtls.c` +
>    `src/kn_net.c` — the persistent TLS-1.2 stream API
>    (`kn_net_tls_stream_*`, Win32+mbedTLS only; complementary honest
>    stubs elsewhere; kn_tcp_open gained a port parameter for 322).
> 2. `streamer/{include/kn,src}/kn_sock.*` — `kn_sock_udp_connect`
>    (kernel peer filter + getsockname local-address refresh);
>    test_knsock 70→82 checks.
> 3. `streamer/src/knvstd_main.c` — `--session` mode (phases A-E per
>    docs/RTSP.md §9; token length-only logging; D-s1 placeholder ICE
>    identity; TEARDOWN on exit) + usage/dispatch.
> 4. Build wiring: CMakeLists + both shell gates + T10 (13 suites;
>    target r16 expects 72 checks) + .gitignore unchanged.
>
> ## Verification (round XXI, sandbox)
> native 13 suites 1309 PASS · cross x64+i686 -Werror PASS · knvstd
> =2-paranoid clean (host -O1, mingw -O2) · CMake 0 warnings + ctest
> 15/15 x2 backends · T13 ALL PASS 51/51(sanitized)/63(mbedTLS) ·
> sandbox_session_e2e 36/36 · T10 dry 61. Self-review/KAT catches:
> the chunked-parser stranded cursor (1-byte KAT), the json root-index
> confusion (cleaned pre-ship), kn_net.c's missing top-level stdio
> (POSIX stubs), hex math in the E2E expectations (1194884=0x123B84).
> NOT target-compiled yet: the Win32 body of kn_net_tls_stream_* and
> --session's Win32 sleep path (T10 r16 is their first compile; both
> mirror verified patterns line-for-line).

---

## Round XX (rev 11, superseded — detail below)

> **Rev 11 supersedes rev 1-10b.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md`** — §1-5 are the wire specification
> (read from nvst_rtsp.rs/cloudmatch.rs @ a49b124 with line refs), §8 is
> this round's implementation status + deviations D-w1..D-r4.
>
> **Round XX headline: the stage-2 signaling cores are implemented and
> sandbox-proven — RFC 6455 client framing (`kn_ws`, 84 checks incl. the
> RFC goldens) and the full RTSP-over-WS state machine (`kn_rtsp`, 127
> checks with every fixture transcribed from the reference's own tests,
> incl. a scripted OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY E2E).** No
> production wiring yet: the TLS stream transport, CloudMatch, OAuth and
> `--session` are round XXI. No existing module changed behaviour (kn_rx /
> kn_sock / knvstd untouched this round).
## New files (round XX)
1. `streamer/include/kn/kn_ws.h` + `streamer/src/kn_ws.c` — RFC 6455
   client framing over an injected byte-stream seam (`kn_ws_io`);
   handshake with CSPRNG key + accept verification; masked sends;
   resumable mid-frame timeouts; control-frame handling (ping→pong echo,
   close answered); bounded caller-owned reassembly; exposed test hooks
   `kn_ws_base64` / `kn_ws_accept` / `kn_ws_rand`. Win32 RNG = `rand_s()`
   (msvcrt — NO new PE import); POSIX = getrandom/urandom fallback.
2. `streamer/tests/test_knws.c` — the ELEVENTH suite (84 checks):
   RFC 4648 + RFC 6455 §1.3 goldens, handshake request/response classes,
   wire-exact frames at every length-boundary, fragmentation, control
   frames, all protocol-error classes, oversize, 1-byte-chunk recv,
   timeout resume. In-memory duplex shim (the framing contract is
   transport-independent by design; real-socket/TLS E2E is the round-XXI
   fake-rig gate).
3. `streamer/include/kn/kn_rtsp.h` + `streamer/src/kn_rtsp.c` — the pure
   RTSP-over-WS client: serializer/parser/request loop (CSeq==Request-Id,
   Bifrost CRLF padding, Request-Id fallback, TEARDOWN mismatch-skip,
   20 s / 4 MiB limits), SDP toolset (`sdp_attribute` with the `||`/`;;`
   quirks, `media_control`, `runtime_key` + `{key_id:024X}` salt, the
   8-profile scan defaulting AEAD_AES_256_GCM_8, `parse_video_peer` with
   the span<16 collapse, the URI ladder), `extract_describe`, the full
   SETUP sweep (poison-rule stop, bundle fallback, 3×3 s injected-sleep
   retries, remaining-deadline narrowing), `build_announce` (complete
   §3 attribute list) and `negotiate_announce` (16-rule range-validated
   offer resolution + clamps + line rewrite).
4. `streamer/tests/test_knrtsp.c` — the TWELFTH suite (127 checks):
   reference-test fixtures transcribed (nvst_rtsp.rs:3443-3470 parser
   cases, :3470+ official control, :2923+ announce params), scripted-
   server full-sequence E2E with byte-exact request inspection, every
   SETUP branch (happy/400-ladder/peerless+bundle/peerless-exhaust/
   fatal-500/client-udp-form), TEARDOWN skip semantics, error paths.
   Injected clock+sleep keep retry rounds instant and countable.

## Changed files (round XX)
1. `CMakeLists.txt` — kn_ws.c + kn_rtsp.c into kncore; knws + knrtsp in
   the test foreach (ctest 12→14).
2. `tools/build_native_tests.sh` + `tools/cross_check.sh` — the two new
   suites + module compiles (12 native suites, 1183 checks).
3. `tests/T10_build_and_test.py` — suites += test_knws, test_knrtsp
   (target r15 expects 71 checks = r14's 69 + 2).
4. `docs/RTSP.md` — §8 implementation status + deviations D-w1..D-r4.

## Verification status (round XX, sandbox)
native 12 suites 1183 checks PASS · cross mingw x64+i686 -Werror PASS ·
ASan+UBSan+LSan clean on both new suites (the single runtime error is the
known vendored sha1.c:26 note, triggered by SHA-1 padding — streamer code
has zero diagnostics) · -Wformat-truncation=2 clean -O0..-O3 on kn_ws.c,
kn_rtsp.c AND test_knrtsp.c · CMake ctest 14/14 both TLS backends ·
T10 POSIX dry-run ALL PASS 60. Self-review catches this round (all fixed
pre-gate): the kn_ws mid-payload timeout was NOT resumable in the first
draft (frame state added), the RT_FOR_LINES macro ran its body per
character (LF-only KAT caught it), the parser status-line scan leaked
into the header block (GARBAGE KAT caught it), blank SDP lines aborted
the attribute scan (the `;;` quirk KAT caught it).



> **Rev 10b (round XVIII)** — the target T13 r1 finding, fixed:
> 1. `streamer/src/knvstd_main.c` — `lb_json_path` (the loopback sanitizer
>    T11 proved) promoted to the platform-neutral section as THE
>    stdout-JSON string escaper, hardened with a `>=0x7F -> '?'` rule
>    (cp1250 argv); every argv/path/foreign-string printf-JSON site routed
>    through it (unknown_selftest name, diagnostics bundle, replay
>    error+summary, capture error/detail/summary, decode error detail).
>    Root cause of T13 r1's only 2 failures: `"out":"C:\Users\..."` raw —
>    `\U` is not a legal JSON escape, summary lines became unparseable.
> 2. `streamer/src/kn_selftest.c` — sibling `st_json_str`; gs.parse's
>    KN_GS_CAPTURE path sanitized (=2 paranoid count unchanged at 9).
> 3. `tests/T13_udp_e2e.py` — per-leg "every stdout line parses as JSON"
>    checks, `rcvbuf_actual` logged per decode leg (R34), POSIX
>    backslash-path contract leg. Target expectation: T10 r14 ALL PASS
>    69 (rebuild is MANDATORY before T13), T13 r2 ALL PASS 61.
> 4. `docs/SOCK.md` — landmine 6 + §7 target record (T10 r13 ALL PASS 69,
>    T13 r1 analysis). Evidence: `artifacts/T13-round1/`.
> Sandbox re-verification: 10 suites 972 PASS, cross PASS, ctest 12/12
> x2, T13 ALL PASS 51/51(sanitized)/63(mbedTLS+gcm256), T10 dry 58.


> **Rev 10 supersedes rev 1-9.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/SOCK.md`** (provenance table with corrected
> nvst.rs line refs, landmines 1-5, the single-thread decision, the E2E
> graph and all sandbox evidence), plus `docs/RX.md` §Stage-2 remainders
> (sockets bullet now DONE).
>
> **Round XVII headline: the stage-2 socket layer is implemented and proven
> end-to-end on loopback — sender → live capture → replay-verify → live
> decode (paced + blast + SRTP cm128/gcm256) — under ASan+UBSan+LSan, on
> three build configurations, before the target ever saw it.** The pure
> kn_rx core is UNCHANGED (one additive API); the round-XVI KAT battery
> still pins every behaviour.
>
> Context this round also CLOSED two target items: T10 round 12
> (**ALL PASS 67 checks** — test_knrx 209/209 on Win7, rx.vectors 153/153,
> registry 13, zero warnings) and the G2-offline demo from `idk_g2.txt`
> (replay verified byte-identical; the demo's `--verify` failure was a
> missing `> replay.jsonl` redirect on the knvstd command — reconstructed
> and PASSED in the sandbox: `VERIFY PASS: 60 frames byte-identical`).

## New files
1. `streamer/include/kn/kn_sock.h` + `streamer/src/kn_sock.c` — the
   bounded UDP layer: refcounted WSAStartup (Win32) / no-op (POSIX),
   monotonic µs/ms clock (QPC / CLOCK_MONOTONIC), IPv4 parse/text,
   open/bind with SO_RCVBUF request + getsockopt READBACK (Linux caps,
   Windows honours — both reported), select-based recv with timeout and
   honest per-platform truncation semantics, sendto, lifetime counters.
   BOTH platforms real (no stubs). Zero allocation.
2. `streamer/tests/test_knsock.c` — the TENTH suite (70 checks):
   clock, ipv4 helpers, lifecycle/refcounts/closed-socket discipline,
   100-datagram varying-size round-trip byte-exact, timeout path,
   65 507 B max datagram, 512-datagram burst drain with seq markers,
   the PLATFORM-DELTA truncation case (Win32 WSAEMSGSIZE-loss vs POSIX
   silent truncation), bad-argument matrix. Section-isolated, never
   aborts (round-XVI suite rule).
3. `tools/send_rx_capture.py` — paced UDP sender for kn_ring_dump-format
   captures (the offline stand-in for a real Mjolnir session): hybrid
   sleep+spin pacing on the capture's µs timeline (sub-ms jitter even
   where Sleep() floors at ~2.9 ms — R30), `--speed 0` blast mode,
   json-lines summary, py3.8-compatible.
4. `tests/T13_udp_e2e.py` — the stage-2 socket gate (loopback only, no
   account): capture leg → vector leg (replay of the LIVE-RECORDED file
   verified against the reference manifest) → decode paced → decode blast
   → SRTP cm128 → SRTP gcm256 (auto-enabled from the binary's
   rx.vectors `srtp_backend` evidence) → `sock.loopback` from the shipped
   binary → check_jsonl. Reuses T10's build; wipes t13work/t13logs at
   start; T13_wynik.txt + T13_skrot.txt. Runs on POSIX for real (not a
   dry-run — real sockets; only the gcm256 leg may skip in bare builds).
5. `tools/sandbox_stream_decode.c` — sandbox-only functional proof of the
   persistent decoder session (NOT a gate, like sandbox_decode_check.c):
   60/60 through per-AU pushes, corrupt-AU survival, sticky abort, and
   the CONTROL case that justifies the API (one-shot per AU delivers 2 of
   60 — P-frames die without references).
6. `tools/fetch_ffmpeg_headers_sandbox.py` — scripts BOOTSTRAP step 4
   (pinned FFmpeg headers at bf0e8316 + avconfig/ffversion stand-ins);
   sandbox convenience, writes into the gitignored third_party/.
7. `docs/SOCK.md` — the round-XVII design record (this delta's canon).

## Changed files
1. `streamer/include/kn/kn_decode.h` + `streamer/src/kn_decode.c` —
   NEW persistent stream session: `kn_decode_stream_open/push/flush/
   stats/close`. One complete Annex-B AU per push (NO parser on this
   path — the round-IX parser-flush landmine cannot occur; end-of-stream
   is `flush()`), codec context persists so inter-frame references
   survive, corrupt AUs counted and the session continues, sticky
   callback abort (-4), cumulative stats, honest stubs in bare builds.
   Reuses kd_ctx/kd_deliver/kd_send_drain unchanged. LSan caught (and
   this round fixed) a `k.nvbuf` leak in close().
2. `streamer/include/kn/kn_rx.h` + `streamer/src/kn_rx.c` — ONE additive
   API: `kn_rx_video_request_keyframe()` (public wrapper over the
   internal `request_keyframe()` helper — same flag, same counter
   discipline). Used by --decode's queue-overflow path. No behaviour
   change to anything the KAT battery pins.
3. `streamer/tests/test_knrx.c` — +6 checks for the new API
   (take-and-clear, collapse-on-repeat, counter counts SETS, NULL-safe):
   184→190 bare, 209→215 mbedTLS.
4. `streamer/src/kn_selftest.c` + `include/kn/kn_selftest.h` — the 14th
   self-test `sock.loopback` (PORTABLE + GATED: 64-datagram burst
   byte-exact, honest 50 ms timeout, SO_RCVBUF readback, closed-socket
   ERR_STATE; ~50 ms). Relevance mapping learned
   `socket/udp/recv/bind/capture/decode` → re-runs sock.loopback
   (session.capture/session.decode failures re-prove the UDP layer).
5. `streamer/tests/test_knselftest.c` — registry 13→14 (two sites),
   host ok_count 4→5, sock.loopback not-skipped+ok on BOTH platforms,
   two new relevance cases (E_CAPTURE_SOCKET, E_DECODE_NO_DATA):
   108→115 checks host.
6. `streamer/src/knvstd_main.c` — graceful-stop plumbing
   (SetConsoleCtrlHandler / SIGINT+SIGTERM → flag polled every loop
   iteration → clean summary + flush); `--capture` (streaming
   kn_ring_dump-format file writer with 32-records/100 ms flush policy,
   64 KiB ring mirror dumped to `<log_dir>/wire_capture.log` on failure,
   RTP-seq meta from the unencrypted header, empty capture = FAILED +
   amplification); `--decode` (single-threaded recv→process→poll loop at
   the reference's 10 ms cadence on the QPC-ms clock, capacity-2 AU queue
   with the reference overflow policy — drop-new + `kn_rx_inval_lose` +
   request_keyframe, per-AU JSONL with sha1, capped rx-event JSONL,
   kf_request lines (unsent until stage 3 — recorded, never dropped),
   1 s transport stats log lines, verify-compatible summary incl.
   `"frames"` alias, ring mirror, `--decode-expect-frames`, AU dump dir,
   pipeline-only honesty in stub builds). usage() + main() dispatch for
   both modes with full argument range validation.
7. `CMakeLists.txt` — kn_sock.c into kncore; `knsock` in the test
   foreach (ctest 11→12).
8. `tools/build_native_tests.sh` + `tools/cross_check.sh` — kn_sock.c +
   test_knsock wiring (10th suite; cross links -lws2_32).
9. `tests/T10_build_and_test.py` — suites += test_knsock; registry check
   13→14; the portable-battery loop (fec/srtp/gs/rx) += sock.loopback
   ("ok in the target binary" — a skip would be a stub, it is real on
   every platform). POSIX dry-run: 57→58 checks.
10. `tests/README.md` — round-XVII header (T13 usage) + the G2-offline
    demo block FIXED: the `> replay.jsonl` redirect is now IN the command
    (the idk_g2.txt failure mode), with an explicit warning line.
11. `.gitignore` — /t13logs/ + /t13work/.
12. `docs/RX.md` — the "Sockets" remainder bullet → DONE (round XVII)
    with the corrected `nvst.rs:235` line ref (the old text cited :222 —
    stale even before this round; :235 is the constant, :5682 the
    setsockopt call site, verified against a49b124).

## Verification status (what ran WHERE)
* Sandbox (this round): native 10 suites PASS (test_knrx 190 bare;
  test_knsock 70); cross mingw x64+i686 -Werror PASS incl. kn_sock and
  the extended knvstd; -Wformat-truncation=2 clean at -O0/-O2/-O3 on
  every touched file (R28 pre-check; the two host-only =2 notes in
  kn_sock/kn_decode are errsz==1 over-approximations — bounded-safe,
  same class as round XI's decision); ctest 12/12 in BOTH CMake
  configurations (schannel + mbedTLS, tls=mbedtls recorded);
  ASan+UBSan+LSan: test_knrx 190/190, test_knsock 70/70 with ZERO
  streamer-code diagnostics (the single runtime error is the PRE-EXISTING
  vendored sha1.c:26 int-promotion UB — pinned by hash, must not be
  edited; sanitizer runs use recover mode, see docs/SOCK.md §4);
  T13 ALL PASS on three builds (44 checks FFmpeg-bare, 44 under
  sanitizers, 55 with mbedTLS incl. the gcm256 leg); T10 POSIX dry-run
  ALL PASS 58; sandbox_stream_decode 23 checks PASS; cross-machine
  parity: target's idk_g2 console JSONL == sandbox replay output
  byte-identical (60 AU lines + counters), regenerated capture matches
  the target's 119 records / 360 333 B.
* Target (PENDING — round XVIII asks for): `python
  tests\T10_build_and_test.py` (expect ALL PASS ~69: 10 suites,
  registry 14, test_knrx 215 in the mbedTLS build) and `python
  tests\T13_udp_e2e.py` (expect ALL PASS 55 — first Winsock execution
  of the live modes; watch the `rcvbuf_actual` line: Windows should
  apply the full 8 MiB). No GPU/T11 rerun needed (no graphics touched).
