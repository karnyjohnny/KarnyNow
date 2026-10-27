# START HERE — KarnyNow entry point and living handoff (created at the close of round XXXVIII, 2026-10-27)

> **You are an agent continuing this project.** This file replaces
> `PROJECT-STATE.md` as the living handoff document. `PROJECT-STATE.md`
> (3173 lines, rounds I–XXXVIII) is now a **historical archive** — it is
> still the deepest record of *why* things are the way they are, but it
> is no longer updated and its "Current status"/"Next steps" sections are
> frozen at round XXXVIII.
>
> **The continuity rule (binding):** every round ends with THIS file
> (§Current state, §What to do next), `INTEGRATION.md` (a new rev delta)
> and the relevant `docs/*.md` updated **before** results are presented
> to the user. The repo is the source of truth. The agent cannot push —
> deliver the changed-file list and the user syncs. **English only in
> this repo (decision D12).**

---

## 1. The project in one paragraph

**KarnyNow** is a dependency-light **C11** reimplementation of the
**OpenNOW** Rust reference client for **GeForce NOW** (the "Mjolnir" /
NVST streaming protocol), targeting **Windows 7** (w64devkit GCC 16.2,
i686 + x86_64) with a TUI front end planned for later stages. It is
developed in a Linux sandbox under a **two-tier gate discipline**: every
round must pass the full sandbox gate set (`tools/build_native_tests.sh`,
`tools/cross_check.sh` ×2 arches, ctest ×2 TLS backends, sanitizers,
`tests/T10_build_and_test.py` POSIX dry, `tests/T13_udp_e2e.py`), and the
user then runs the **target gates on real Win7** (`T10` build+test,
`T13`, and the live **R7 `--session` runs** against the real GFN
production cloud). Work proceeds in numbered **rounds**; each round is
triggered by a measurement (a target gate result or a live run log) and
ships code + evidence + docs. The project is at **stage 2 of 5**
("NVST media/transport", see `BOOTSTRAP.md` §12): the full signaling →
ICE → DTLS → SCTP → SRTP video pipeline works live; the open question is
stream **sustainability** (see §3).

## 2. Reading order for a fresh agent

1. **This file** — state, next steps, agreements, traps.
2. **`BOOTSTRAP.md`** — the project map: mission, stage gates, logging
   spec (§7), decision register D1–D13, risk register, research
   provenance. Still authoritative for philosophy and stage definitions.
3. **`docs/RTSP.md` §25** — the most recent round's record (run 18
   diagnosis, what shipped, the run-19 decision matrix). §1–§24 are the
   run-by-run history of the live protocol work.
4. **`INTEGRATION.md` rev 28 (the top block)** — the current delta:
   new/changed files, integration traps, gate results. Older revs below
   it are superseded history.
5. **The layer doc for whatever you touch**: `docs/RX.md` (receive
   pipeline), `docs/SRTP.md`, `docs/FEC.md`, `docs/GS.md` (video
   framing), `docs/DTLS.md`, `docs/SCTP.md`, `docs/INPUT.md`
   (control/input plane), `docs/RTSP.md`, `docs/AUTH.md`, `docs/NET.md`,
   `docs/SOCK.md`, `docs/D3D.md`, `docs/AUDIO.md`, `docs/LOGGING.md`.
6. **`PROJECT-STATE.md`** (archive) — session log rounds I–XXXVIII with
   the full reasoning trail; the status header and Next-steps list are
   frozen but were accurate at XXXVIII close. **Its "Bootstrapping a NEW
   agent session" section is still CURRENT and useful**: the exact clone
   URLs, the vendor-staging recipe and the sandbox toolchain notes live
   there (not duplicated here).
7. **`HANDOFF.md`** (Polish, frozen) — the original research journal:
   *why* the early decisions were made. `AGENT-HANDOFF.md` (frozen at
   round IX) — the first recovery document; historical only.

## 3. Current state (frozen at round XXXVIII close, 2026-10-27)

**Live progress (R7 runs against the real production cloud):**

| run | round | outcome |
|---|---|---|
| 16 | XXXVI | **PLAY 200 — first real GFN video through the full stack** (307 AUs, 1280×720, zero errors); ~10.2 s stall |
| 17 | XXXVII | feedback set flew for 242 s, stall UNMOVED at 10.99 s → feedback hypothesis disproved; activation chain identified as the missing startup item |
| 18 | XXXVIII | **activation chain worked — stall moved to 30.5 s** (562 AUs, 13 keyframes, 6665/6665 datagrams authenticated, 0 decode errors); AU frame-size decay under zero loss named the NEXT gate: the server rate controller ramping down against our **zero-valued bandwidth-estimator fields** in QoS `0x0207` (deviation D-t10) |

**Round XXXVIII shipped** (all sandbox-gated, awaiting target):

- `kn_bw.{h,c}` — the line-for-line port of `nvst_bandwidth.rs`
  (BandwidthEstimator + `kn_sctl_walk` server-control decoder), pinned by
  the independent `tools/bw_reference.py` and `test_knbw` (387 checks).
- The estimator feed inside `kn_rx` (`process_authenticated`, datagram
  path only) and the full seven-field QoS `0x0207` rewrite in `knvstd`.
- `ControlReportBatch` (one sid-6 message per ≤1071 B / ≤50 ms instead of
  one message per record — the fix for run 18's `dropped:890`) + per-sid
  outbox counters.
- Server-control latches in the media loop: `0x109` termination →
  `stop_reason:"server_termination"`; `0x112` gates the pacing sender.
- SCTP association cause codes on the `sctp_down` line.
- AU wall-clock vs RTP-timestamp drift telemetry (`au_clock`).
- **R3-D closed**: the WSS control-channel keepalive (2 s u64-BE pings,
  pong-match RTT sample, 5 s expiry; `kn_ws_take_pong` + `cping_*`
  evidence). The R3 series (A cert / B DTLS / C SCTP / D WS-pings) is
  now fully shipped.
- The session summary grew four evidence blocks: `batch`,
  `outbox_by_sid`, `bwe`, `au_clock` (appended after `feedback`, so the
  `--verify` contract is untouched).

**Sandbox gates at XXXVIII close (all green):** native suite **18/18,
2252 checks** (incl. knbw 387, kninput 446, knws 88) · `cross_check.sh`
PASS ×2 arches, zero warnings under `-Wall -Wextra -Werror` · ctest
**20/20** with mbedTLS and **20/20** with schannel, zero warnings ·
ASan+UBSan clean on test_knbw (387) and test_knrx (194) ·
`sandbox_session_e2e` 43/43 · T10 POSIX dry **ALL PASS 69** · T13 **ALL
PASS 63** · paranoid matrix (knvstd ×9 + kn_bw/kn_rx ×3 with
`-Wunused-but-set-variable -Wformat-truncation=2 -Werror`) clean.

**Codebase shape:** 28 `streamer/src/*.c` + 26 `streamer/include/kn/*.h`;
the production binary is `knvstd` (`knvstd_main.c`, 6276 lines: modes
`--capture`, `--decode`, `--replay-capture`, `--session`, `--selftest`,
`--list-selftests`); 15 registered production self-tests; 18 native test
suites. Third-party (vendored, gitignored, re-stage if cleaned):
tiny-AES-c, crypto-algorithms-sha1, usrsctp, mbedTLS 3.6.0 (+framework).

## 4. What to do next (in order)

1. **The user runs target T10**: `python tests\T10_build_and_test.py` →
   expect **ALL PASS 79** (was 78; +1 for the new `test_knbw` suite) and
   **zero warnings**; `test_knbw` must report **387 checks** on the
   target (pure computation — identical to sandbox; that equality is the
   Win7-side proof the estimator matches the reference goldens).
   - If it FAILS: it is almost certainly a **target catch** (GCC 16.2
     sees warnings sandbox GCC 12 does not; there have been seven such
     catches, each fixed AND permanently gated — see the paranoid-leg
     pattern in `tools/build_native_tests.sh`). Fix, add the gate leg,
     bump the expectation honestly.
2. **The user runs R7 run 19** (the BWE run): `--session-seconds=300`,
   untruncated log if possible (`> run19.log`). **Decision matrix** (full
   version in `docs/RTSP.md` §25.4):
   - **Video sustains** (aus ≈ 30/s × 300, no AU-size decay,
     `bwe.estimate_kbps > 0` after frame 100) → **D-t10 was the gate;
     STAGE 2 IS DONE.** The presentation wiring becomes the main track:
     `--session` frames → the G1-proven `kn_d3d_vp` presenter = G2 video
     ON SCREEN (see `docs/D3D.md`; the NV12 StretchRect chain is
     settled — decision D7).
   - **Stall persists with `frames_observed ≥ 100` and nonzero
     estimate** → estimator flew, server still ramped down: compare the
     QoS wire values field-by-field against `nvst_control.rs:133-156`;
     suspect the utilization gate.
   - **`frames_observed < 100`** → feed starved; the `bwe.skip_*`
     counters name where (reordered / retransmitted / no-video-parse).
   - **`dropped > 0` with the batch active** → `outbox_by_sid` names the
     channel; a sid-6 drop means the batch cadence constants are wrong.
   - **`stop_reason:"server_termination"`** → the rig sent `0x109`;
     `sctl_stats` + `term_*` fields say which wire code and whether it
     claims resumable.
   - **`cping_dead:1` / growing `cping_expired`** → the WSS control
     channel died mid-session; correlate with the stall timestamp.
   - Ask the user to paste: `handoff`, one mid-session `session_stats`
     line, `sctl_stats`, `ctrl_ping_stop`, `ctrl_summary`, the FULL
     summary (all four new blocks), `sctp_down`/`ice_summary`.
3. **Then stage 3 per `BOOTSTRAP.md` §12** — the discrete-input half
   (`docs/INPUT.md` gaps G2/G3: keyboard/mouse/text), bundle audio
   (DTLS-SRTP keying already negotiated, `srtp_prof:1`; extraction is
   the first step), and open fidelity items: the input handshake timeout
   (G1 — the 5 s `INPUT_HANDSHAKE_TIMEOUT` is defined but not enforced),
   server cursor payload decoding (G4).

## 5. Binding working agreements (carried through every round)

- **Measure first.** Never guess when a measurement is possible; if the
  evidence is ambiguous, ship instrumentation and ask the user for a run.
  Every hypothesis that was skipped produced a wasted live run; every
  hypothesis that was measured produced a fix.
- **Two-tier compile gate**: sandbox (GCC 12) + target (GCC 16.2 via
  T10). **Zero warnings** on both. The paranoid knvstd legs
  (`-Wunused-but-set-variable -Wformat-truncation=2 -Werror`) exist
  because target-only warning classes repeatedly escaped sandbox `-Wall`.
- **Two-implementations rule**: every wire format is pinned twice — an
  independent Python model under `tools/` (which self-proves against the
  reference's own Rust test goldens BEFORE emitting C vectors) and a C
  test suite replaying those vectors. Never hand-write C scenario data
  from memory; only the `tools/*_reference.py` scripts are authoritative.
- **No TLS bypasses** anywhere (verification REQUIRED; pins are
  measured, never disabled). **Secrets are logged as lengths only.**
- **Single-owner discipline (D21)**: live loops own their sockets — the
  pump thread owns the bundle socket (ICE/DTLS/SCTP/control-in), the
  media loop (main thread) owns the video socket, the kn_rx context and
  the WSS control channel. Cross-thread communication only through the
  bounded feedback outbox, the stats snapshot channel, and volatile
  single-word latches.
- **Evidence-lines philosophy**: every protocol event prints one JSONL
  line on stdout (strict JSON, bounded fields, `check_jsonl.py`-clean);
  a failure that cannot be diagnosed from the log alone is an
  instrumentation bug — fix it in the same round.
- **The agent cannot push.** A round ends with: files changed in the
  workspace + an explicit change list + docs updated (the continuity
  rule at the top of this file). The user syncs, runs the target gates,
  and pastes results.
- **Reference fidelity over invention**: the OpenNOW reference (below) is
  the specification. Deviations are allowed only when bounded-C reality
  demands them, and each one is numbered (**D-tN**), recorded in the
  relevant `docs/*.md` §Deviations, and revisited when evidence arrives.

## 6. The reference baseline

- **Repo**: OpenCloudGaming/OpenNOW (GitHub), **commit `a49b124ce31e`
  — verified** as the baseline every port cites. A clone lives beside
  this repo in the agent workspace as `ref/` (not part of this repo).
  Line numbers in docs/ comments cite this commit; re-verify against
  @HEAD before citing exact lines in NEW docs (they drift).
- **Key files**: `native/opennow-streamer/crates/opennow-streamer-core/src/`
  → `nvst.rs` (the session core: media loop :7860-7965, QoS report
  :1140-1177, feedback senders :8041-8060, server-control ingest
  :694-730/:8240-8300, bundle worker :7438-7531), `nvst_control.rs`
  (0x0207 wire layout :133-156), `nvst_input.rs` (channel profiles
  :74-127, activation chain :1318-1329, keepalive golden :1769),
  `nvst_bandwidth.rs` (the estimator — ported to `kn_bw.c`),
  `nvst_server_control.rs` (decoder table :107-146), `nvst_rtsp.rs`
  (control-ping worker :844-935, NvstControlPing :260-290),
  `nvst_cursor.rs` (capture-handoff machine :52-126).
- **SCTP channel map** (nvst_input.rs:74-127): sid 0 ctrl_reliable
  (ordered) · 2 custom_reliable · 4 custom_partial (ordered, maxretx 2)
  · **6 ctrl_partial (UNORDERED, 300 ms TTL — the feedback channel)** ·
  8 ctrl_unreliable · 10 input_partial · 12 cursor · 14 rtcp (ordered).
  Server control is decoded from sid 0 AND sid 6.
- **Vendor pins** (enforced by `tests/T10_build_and_test.py:134-152`):
  tiny-AES-c `23856752f` · crypto-algorithms `cfbde484` · usrsctp
  `c625e219` · mbedTLS 3.6.0 `2ca6c285` + framework `750634d3`.

## 7. Architecture in ten lines

```
knvstd --session:  CloudMatch create/poll (kn_cm)  →  session context
   → WSS/RTSPS signaling (kn_ws + kn_rtsp: OPTIONS/DESCRIBE/SETUP/
     ANNOUNCE/PLAY, credential ladder, 455-retry)
   → pump thread (pre-PLAY → persists): STUN/ICE + nomination (kn_sock),
     DTLS 1.2 + use_srtp (kn_dtls/kn_cert, mbedTLS), usrsctp AF_CONN
     (kn_sctp, 8 DCEP channels), control-in (kn_input + kn_bw sctl),
     activation chain, cursor machine, volatile latches → media loop
   → media loop (main): video UDP → kn_rx (SRTP decrypt, FEC repair,
     reorder, NACK, AU assembly; the kn_bw estimator feed rides here)
     → kn_decode (FFmpeg) → AU file/presenter; feedback senders (RR,
     frame-ack, pacing, QoS 0x0207 w/ BWE, keepalive, IDR/PLI/NACK/RFI)
     → ControlReportBatch → outbox (64×1071, per-sid counters) → pump
   → evidence: one JSONL line per event; summary JSON at exit.
```

## 8. Traps that will bite you (curated; full list in INTEGRATION.md rev 28 §traps)

1. **Clock exactness is load-bearing**: all estimator math runs on
   **integer nanoseconds** (int64) exactly like the reference's
   `Duration`s; a float or ms-granular clock silently doubles the
   receive-rate error (the 0.5 ms clamp floor). Production feeds the µs
   socket clock ×1000 via `cfg.now_ns`; tests/replay use the
   deterministic fallback.
2. **Decided-and-final items — do not relitigate without new reference
   evidence**: the feed path (datagram=1, plaintext=0); the BWE
   100-frame zero-gate (a sub-100 run showing zero estimates is CORRECT);
   the server-control iterator semantics (Oversized yields once; BOUNDS
   empties without error; DECODE consumes one record and continues);
   `completed_frame_bytes` is CUMULATIVE, `sender_frame_number` is the
   last COMPLETED frame index, the QoS loss field is a guarded DELTA.
3. **Rust `as` casts saturate/reinterpret; C is UB.** `kn_bw.c` uses
   `bw_sat_i64/u64` + explicit masks everywhere — never simplify them.
4. **Endianness split in `kn_input`**: `device_descriptor` timestamp is
   BE, the haptics `remote_input_message` timestamp is LE. Both are
   test-pinned; do not "unify".
5. **Patch discipline on `knvstd_main.c`** (6.3k lines, heavily
   commented): anchors go stale after earlier patches rewrite the same
   block — re-read the current text first; two identical `char drops[512]`
   anchors exist (replay vs session summary) — pin the function; a
   format-line edit that drops a closing `"` turns the printf into a
   comma expression and the compiler only says "too many arguments" —
   after ANY summary-format edit, re-validate by literal extraction
   (spec count == arg count, `json.loads` the skeleton with `%dx%d`
   pre-replaced).
6. **Build-leg completeness**: `kn_rx.o` is linked directly in four
   places (native `test_knrx` leg, cross `test_knrx` leg, `$KCORE`,
   CMake kncore). Any new module referenced from `kn_rx.c` must be added
   to ALL of them + the T10 suites list + the cross `-c` legs, or a gate
   fails at link time.
7. **`0x0100` is NOT in the server-control Unhandled set** — it decodes
   as Unknown. Bulk decoder tests use the `0x0101` family.
8. **Sandbox environment**: `ps` unavailable; `apt` only in foreground
   with a long timeout; `third_party/` trees are gitignored — re-stage
   them (git clone at the pinned commits) if the workspace was cleaned;
   `build_native_tests.sh` needs tiny-AES-c + sha1 present.

## 9. Repository map

```
BOOTSTRAP.md            the project map (mission, stages, logging, D1-D13)
START-HERE.md           THIS FILE — living handoff (update every round)
INTEGRATION.md          per-round delta notes (rev 28 = current)
PROJECT-STATE.md        ARCHIVE — session log rounds I-XXXVIII (frozen)
HANDOFF.md              ARCHIVE — Polish research journal (frozen)
AGENT-HANDOFF.md        ARCHIVE — round-IX recovery doc (frozen)
docs/*.md               per-layer design records + deviations + run history
  RTSP.md §1-§25        the live-protocol run-by-run record (run 19 → §26)
streamer/include/kn/    public headers (one per module)
streamer/src/           modules: knlog knring knfec kn_aes kn_sha1 knsrtp
                        kn_diag kn_selftest kn_decode kn_d3d kn_gs kn_perf
                        kn_audio kn_net(+_mbedtls) kn_rx kn_sock kn_ws
                        kn_rtsp kn_json kn_http kn_cm kn_cert kn_dtls
                        kn_sctp kn_input kn_bw + knvstd_main.c
streamer/tests/         test_kn*.c (18 suites) + KAT vector headers
tools/                  *_reference.py (independent models — authoritative)
                        build_native_tests.sh · cross_check.sh ·
                        fake_rig.py · sandbox_* harnesses
tests/                  T1..T13 harnesses (T10 = the target build gate)
third_party/            vendored pins (gitignored; re-stage per §6)
```

## 10. Round history at a glance (detail: PROJECT-STATE.md session log)

| rounds | milestone |
|---|---|
| I–XIV | skeleton, logging, loopback, audio, D3D9 presentation settled (D7), decode pipeline, T1–T9 gates |
| XV–XIX | target networking fixed (Win7 TLS), kn_rx pipeline + vector machinery, the socket layer (T13), Winsock buffer proof |
| XX–XXII | signaling cores (kn_ws/kn_rtsp), the TLS stream, CloudMatch (kn_cm), `--session` born, E2E fake rig |
| XXIII–XXVIII | R7 runs 1–10: live create/poll against production, the control-node re-learning fix, the 503 class eliminated, PLAY 455 → the pump thread |
| XXIX–XXXV | R3 series: kn_cert → kn_dtls (DTLS 1.2 live on run 13) → kn_sctp/usrsctp (run 14–15: INIT sent, silence) → nomination (D-t8) + use_srtp (D-t9) |
| XXXVI | **run 16: PLAY 200 — first real GFN video end-to-end**; the full feedback sender set ships |
| XXXVII | run 17: feedback disproved as the stall cause; `kn_input` + the activation chain (three-implementation pinned) |
| XXXVIII | **run 18: stall moved 11 → 30.5 s** (activation confirmed); the `nvst_bandwidth.rs` port (D-t10 closed), ControlReportBatch, server-control latches, SCTP cause codes, AU telemetry, R3-D — **awaiting target T10 (expect 79) + run 19** |

## 11. The user protocol

- The user runs target commands (`T10`, `T13`, R7 `--session`) on the
  Win7 machine and **pastes logs/results back**. Live-rig sessions are
  precious — never ask for a run without shipping the instrumentation
  that makes it decisive, and always state the exact expected counts
  (e.g. "ALL PASS 79") so a mismatch is instantly visible.
- The user syncs changed files themselves (the agent cannot push);
  always end a round with the explicit file list.
- Logs sometimes arrive truncated (~76 chars/line in run 18) — ask for
  an untruncated capture (`> runNN.log`) when field values matter; never
  chase an anomaly that may be a truncation artifact.
- When a question is answerable by measurement, propose the measurement
  instead of a guess (§5, "measure first" — the user explicitly endorses
  this and will willingly run it).
