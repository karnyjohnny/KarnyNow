# START HERE — KarnyNow entry point and living handoff (created at the close of round XXXVIII, 2026-10-27; updated at round XL close)

> **You are an agent continuing this project.** This file replaces
> `docs/archive/PROJECT-STATE.md` as the living handoff document.
> `docs/archive/PROJECT-STATE.md` (3173 lines, rounds I–XXXVIII) is now a
> **historical archive** — it is still the deepest record of *why*
> things are the way they are, but it is no longer updated and its
> "Current status"/"Next steps" sections are frozen at round XXXVIII.
>
> **The continuity rule (binding):** every round ends with THIS file
> (§Current state, §What to do next), `docs/INTEGRATION.md` (a new rev
> delta) and the relevant `docs/*.md` updated **before** results are
> presented to the user. The repo is the source of truth. The agent
> cannot push — deliver the changed-file list and the user syncs.
> **English only in this repo (decision D12).**
>
> **Round XXXIX reorganized the repo** (root = `README.md` +
> `START-HERE.md` only; history in `docs/archive/`; all logs under
> `logs/`; all builds under `build/`; FFmpeg lives in the tracked
> `build/ffmpeg/`). Old path references inside frozen archives describe
> the pre-XXXIX layout on purpose — see `docs/INTEGRATION.md` rev 29.

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
("NVST media/transport", see `docs/BOOTSTRAP.md` §12): the full signaling →
ICE → DTLS → SCTP → SRTP video pipeline works live; the open question is
stream **sustainability** (see §3).

## 2. Reading order for a fresh agent

1. **This file** — state, next steps, agreements, traps.
2. **`README.md`** — repo layout, build/test quick start, the FFmpeg and
   `third_party/` provisioning rules, acknowledgements/licences.
3. **`docs/BOOTSTRAP.md`** — the project map: mission, stage gates,
   logging spec (§7), decision register D1–D13, risk register, research
   provenance. Still authoritative for philosophy and stage definitions.
4. **`docs/RTSP.md` §25–§26** — the two most recent rounds' records
   (run 18 diagnosis + run 20 verdict + the run-21 plan). §1–§24 are the
   run-by-run history of the live protocol work.
5. **`docs/INTEGRATION.md` rev 29 (the top block)** — the current delta:
   new/changed files, integration traps, gate results. Older revs below
   it are superseded history.
6. **The layer doc for whatever you touch**: `docs/RX.md` (receive
   pipeline), `docs/SRTP.md`, `docs/FEC.md`, `docs/GS.md` (video
   framing), `docs/DTLS.md`, `docs/SCTP.md`, `docs/INPUT.md`
   (control/input plane), `docs/RTSP.md`, `docs/AUTH.md`, `docs/NET.md`,
   `docs/SOCK.md`, `docs/D3D.md`, `docs/AUDIO.md`, `docs/LOGGING.md`.
7. **`docs/archive/PROJECT-STATE.md`** — session log rounds I–XXXVIII
   with the full reasoning trail; the status header and Next-steps list
   are frozen but were accurate at XXXVIII close. **Its "Bootstrapping a
   NEW agent session" section is still CURRENT and useful**: the exact
   clone URLs, the vendor-staging recipe and the sandbox toolchain notes
   live there (not duplicated here).
8. **`docs/archive/HANDOFF.md`** (Polish, frozen) — the original
   research journal: *why* the early decisions were made.
   `docs/archive/AGENT-HANDOFF.md` (frozen at round IX) — the first
   recovery document; historical only. `docs/archive/analiza/` — the
   round 0–3 analyses (Polish), incl. the byte-level NVST spec (04).

## 3. Current state (as of round XL close, 2026-10-27)

**Live progress (R7 runs against the real production cloud):**

| run | round | outcome |
|---|---|---|
| 16 | XXXVI | **PLAY 200 — first real GFN video through the full stack** (307 AUs, 1280×720, zero errors); ~10.2 s stall |
| 17 | XXXVII | feedback set flew for 242 s, stall UNMOVED at 10.99 s → feedback hypothesis disproved; activation chain identified as the missing startup item |
| 18 | XXXVIII | **activation chain worked — stall moved to 30.5 s** (562 AUs, 13 keyframes, 6665/6665 datagrams authenticated, 0 decode errors); AU frame-size decay under zero loss named the NEXT gate: the server rate controller ramping down against our **zero-valued bandwidth-estimator fields** in QoS `0x0207` (deviation D-t10) |
| 20 | XXXIX | **the BWE evidence set flew and PROVED the feedback plane healthy** (skip_* all 0, batch flushes 796 / failed 0 / retained 0, outbox dropped 0 on every sid, terminations 0, pacing never disabled) — but the server **drip-fed video from second zero**: 40 AUs in 400 s (1.275 s of media time) in ~5 s bursts, then ONE gap → RECOVERY_REQUIRED until the end (137 drops, no keyframe despite 42 IDR/PLI). `frames_observed:40 < 100` ⇒ `estimate_kbps:0` is the reference's OWN warm-up zero. **STAGE 2 REMAINS OPEN** — full analysis `docs/RTSP.md` §26 |
| 21 | XL | **the regression is ROOT-CAUSED from the log alone**: identical loop-tick signature to run 20 (iterations = datagrams+1, `recv_timeouts=1` against a 10 ms poll, QoS 597 = 2/s not 20/s) ⇒ the media loop was pinned to the server's 500 ms datagram cadence by the R3-D WSS drain blocking in `SO_RCVTIMEO` (Windows rounds it to the ~500 ms AFD granularity — `select()` is the ms-accurate primitive). The session survived 300 s (user attached via web client); the media clock advanced only 1.18 s (`rtp_ms` 1182) = the game screen was STATIC — a second, independent depressant. Fixed + D1–D7 fidelity set shipped — full analysis `docs/RTSP.md` §27 |

**Round XXXVIII shipped** (target-verified in round XXXIX: **T10 ALL PASS
79 on Win7**, `test_knbw` reported 387 checks on target = sandbox — the
estimator port is proven against the reference goldens on Win7):

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

**Round XXXIX shipped** (repo hygiene + the run-20 catch):

- **Repo reorganization** — root is now `README.md` + `START-HERE.md`
  only; frozen MDs moved to `docs/archive/` (PROJECT-STATE, HANDOFF,
  AGENT-HANDOFF, analiza/); `INTEGRATION.md` → `docs/INTEGRATION.md`;
  `BOOTSTRAP.md` → `docs/BOOTSTRAP.md`; `src/` reference headers →
  `docs/reference/`; stray root `tools_*.{sh,py}` → `tools/` (renamed);
  `python/requirements.txt` → `tests/`; `aus_real_test/` untracked (kept
  on disk, ignored); NEW `README.md` (layout, builds, conventions,
  acknowledgements/licences of every dependency).
- **Logs convention** — everything log-shaped lands in `logs/` (ignored):
  T9–T13 verdicts (`logs/TNN_wynik.txt`) and run dirs (`logs/tNN/`),
  `build_native_tests.sh` evidence, and the C test scratch dirs
  (`test_knlog`/`test_kndiag`/`test_knselftest`/`test_knring` now write
  under `logs/`). knvstd's `--log-dir` already defaulted to `logs`.
- **Builds convention** — everything under `build/` (T12 moved from
  `build-network/` to `build/network/`).
- **FFmpeg provisioning** — the user's prebuilt tree lives in the
  TRACKED `build/ffmpeg/` (the one exception to the build/ ignore);
  CMake auto-detects it when `KN_FFMPEG_PREFIX` is empty and T10 probes
  it first (the old Desktop paths are gone).
- **Fixed: the summary `outbox_by_sid` JSON bug** — run 20's log caught
  missing braces (`"outbox_by_sid":"0":[…]` — the whole summary line was
  unparseable). The format now wraps the entries in `{}`; the
  empty-buffer case yields `{}` instead of `{{}}`.

**Round XL shipped (the regression fix + the fidelity set):**

- **THE FIX — `kn_net_tls_stream_recv` (Win32/mbedTLS body,
  `kn_net_mbedtls.c`)**: wait with **`select()`** (ms-accurate on
  winsock) before `mbedtls_ssl_read`; `SO_RCVTIMEO` — which Windows
  rounds to the ~500 ms AFD granularity — remains only as the
  partial-record safety net; `mbedtls_ssl_check_pending()` keeps
  buffered plaintext from waiting on the socket at all. Return
  semantics unchanged. This un-pins the media loop from the server's
  datagram cadence (runs 20+21 ran at ONE tick per datagram; run 18 on
  XXXVII code proved the 10 ms tick worked pre-R3-D). `kn_sock.c` was
  audited line-by-line and is CORRECT — untouched.
- **D1–D7 reference-fidelity set**: IDR `0x0302` + RFI → **sid 0
  reliable** (run 20 died on a one-shot IDR on lossy sid 6, TTL
  300 ms); the **100 ms armed retry loop** (`dec_kf_service`: IDR on
  sid 0 + PLI on sid 14 while the kn_rx keyframe latch is pending,
  episode/attempt logging, failed pushes consume the interval);
  **real RFC 3550 jitter in the RR** (was hard-zero; kn_rx EWMA port +
  6-arg `rr_snapshot` + `rtcp_fb_open` gate, now initialized at
  session start as `!unified_mode && desc.rtcp_on_sctp`); **NACK mode
  split** (RTCP-vs-control, was dual-sent); **frame-ack
  `first_packet_time_ms`** (AU first-packet wall clock, was 0);
  **standalone pacing on sid 6 BEFORE the ack** (55.556 ms gate).
- **New evidence channels**: C18 build stamp (first stdout line of
  every RUN mode: `{"mode":"build","compiled":…,"round":"XL"}`;
  utility modes excluded — single-document stdout contract); C19
  `loop_ticks` (raw media-loop iterations → 1 Hz `session_stats` +
  summary; healthy ≈ 100/s, pathological = datagram-rate); F1 the
  **0x010a payload dumper** (self-contained TLV walker in
  `sess_ctrl_ingest`, ≤ 16 lines/session, hex ≤ 800 chars).
- **Tests**: knrx 194 → **203** — the jitter golden (369, hand-derived
  131.25 → 254.296875 → 369.60546875 → trunc), per-frame
  `first_packet_ms`, pre-bind `rr_snapshot` 0, the pending-latch
  non-clearing contract.

**Sandbox gates at XL close (all green):** native suite **18/18, 2261
checks** (knrx 203, knbw 387, kninput 446) · `cross_check.sh` PASS ×2
arches, zero warnings (incl. the `-DKN_TLS_BACKEND_MBEDTLS=1` legs —
the only place the fixed Win32 TLS body compiles in the sandbox) ·
ctest **20/20 mbedTLS + 20/20 schannel** · paranoid knvstd ×9
(c11/c17/gnu11 × pedantic / format-truncation=2 / unused-but-set+shadow)
clean · kn_bw/kn_rx ×3 clean · ASan+UBSan knbw 387 + knrx 203 clean ·
`sandbox_session_e2e` **43/43** · T10 POSIX dry **ALL PASS 69** · T13
**ALL PASS 63** · summary-printf validation: all 99 JSON printf blocks
specs=args (session summary **71 = 71**) + strict-JSON skeleton
`json.loads` PASS.

**Codebase shape:** 28 `streamer/src/*.c` + 26 `streamer/include/kn/*.h`;
the production binary is `knvstd` (`knvstd_main.c`, 6437 lines: modes
`--capture`, `--decode`, `--replay-capture`, `--session`, `--selftest`,
`--list-selftests`); 15 registered production self-tests; 18 native test
suites. Third-party (vendored, gitignored, re-stage if cleaned):
tiny-AES-c, crypto-algorithms-sha1, usrsctp, mbedTLS 3.6.0 (+framework).

## 4. What to do next (in order)

1. **User syncs the round-XL files** (list in `docs/INTEGRATION.md`
   rev 30) and does a **CLEAN rebuild on Win7**: delete the build tree,
   run T10 in full. Expected verdict: **ALL PASS** with the suite
   counts grown (knrx 203 in the sandbox build; the T10 total rises
   accordingly — state the exact number once the target run is back).
   The clean rebuild matters: the SO_RCVTIMEO fix lives in a file the
   incremental build must actually recompile.
2. **Run 22** — `knvstd --session … --session-seconds=300`, untruncated
   stdout (`> run22.log`) **PLUS the whole `logs/` directory**
   afterwards. **The user keeps the game ACTIVE at and after handoff**
   (input, motion): run 21's `rtp_ms` 1182 vs 300 s wall proved a
   static screen freezes the GFN encoder independently of any client
   fix — without activity the run is uninterpretable.
3. **Read run 22 against the pass criteria** (RTSP.md §27.8): build
   stamp `"round":"XL"` present; `loop_ticks` ≈ 100/s (NOT
   datagram-rate); `recv_timeouts` ≈ ticks − datagrams (tens of
   thousands — the paradox INVERTS into the proof); QoS ≈ 20/s; during
   motion: datagram rate ≫ 2/s and `rtp_ms` tracks wall time;
   `kf_request` episodes end in `kf_cleared`; `sctl_010a` lines finally
   decode the mystery payload; cping on its 2 s beat (watch for pongs
   now that the drain cannot eat the timer).
4. **If the drip persists with a healthy 100 Hz tick and active
   content**, the remaining suspects are server-side policy items we
   can only probe: the 0x010a payload (now dumped), the QoS field
   values under real motion (the estimator leaves warm-up after 100
   frames), and synthetic input cadence (a mouse-jiggle sender was
   deferred from round XL — reconsider only if run 22 shows the idle
   detector still gating us despite user activity).
5. **Then stage 3 per `docs/BOOTSTRAP.md` §12** — unchanged: the
   discrete-input half (`docs/INPUT.md` gaps G2/G3: keyboard/mouse/text),
   bundle audio (DTLS-SRTP keying already negotiated, `srtp_prof:1`;
   extraction is the first step), and the open fidelity items: the input
   handshake timeout (G1 — the 5 s `INPUT_HANDSHAKE_TIMEOUT` is defined
   but not enforced), server cursor payload decoding (G4).

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

## 8. Traps that will bite you (curated; full list in docs/INTEGRATION.md rev 30 §traps)

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
5. **Patch discipline on `knvstd_main.c`** (6.4k lines, heavily
   commented): anchors go stale after earlier patches rewrite the same
   block — re-read the current text first; two identical `char drops[512]`
   anchors exist (replay vs session summary) — pin the function; a
   format-line edit that drops a closing `"` turns the printf into a
   comma expression and the compiler only says "too many arguments" —
   after ANY summary-format edit, re-validate by literal extraction
   (spec count == arg count, `json.loads` the skeleton with `%dx%d`
   pre-replaced). Run 20 proved the rule a new way: the missing `{}`
   around the `outbox_by_sid` `%s` passed the compiler AND the arg count
   — only reading the actual emitted summary line caught it. Once per
   round, eyeball (and `json.loads`) a REAL emitted summary, not just
   the format skeleton.
6. **Build-leg completeness**: `kn_rx.o` is linked directly in four
   places (native `test_knrx` leg, cross `test_knrx` leg, `$KCORE`,
   CMake kncore). Any new module referenced from `kn_rx.c` must be added
   to ALL of them + the T10 suites list + the cross `-c` legs, or a gate
   fails at link time.
7. **`0x0100` is NOT in the server-control Unhandled set** — it decodes
   as Unknown. Bulk decoder tests use the `0x0101` family.
8. **Sandbox environment**: `ps` unavailable; `apt` only in foreground
   with a long timeout; the toolchain is NOT persistent — a fresh
   sandbox needs `apt-get install gcc mingw-w64 cmake ninja-build`
   before any gate runs; `third_party/` trees are gitignored — re-stage
   them (git clone at the pinned commits) if the workspace was cleaned;
   `build_native_tests.sh` needs tiny-AES-c + sha1 present.
9. **`SO_RCVTIMEO` on Windows is ~500 ms-granular** (AFD driver timer);
   `select()` IS millisecond-accurate. Any short bounded blocking recv
   (the WSS drain's 1 ms!) must go through select/poll — never
   SO_RCVTIMEO. Runs 20+21 were lost to this: the symptom set is
   iterations = datagrams+1, `recv_timeouts ≈ 0` against a 10 ms poll,
   and every periodic counter at datagram-rate or an integer divisor.
   `loop_ticks` (C19) now states the tick rate outright.
10. **Shared test structs that property tests `memcmp` must not gain
    feed-order-dependent fields**: `rxt_frame` is compared whole-struct
    across in-order vs reordered/repaired feeds (P2/P3/P5) — the
    arrival-clock `first_packet_ms` lives in a PARALLEL array in
    `rxt_col` for exactly this reason.
11. **The stdout build stamp must skip the utility modes**
    (`--version/--list-selftests/--selftest/--diagnostics`): the native
    gate `json.load`s their captures as SINGLE documents — one extra
    leading line = "Extra data" AND the diag-bundle path check fails.
    Run modes (json-lines) print it first.
12. **Three bodies of `kn_net_tls_stream_recv`**: the real Win32+mbedTLS
    one (`kn_net_mbedtls.c` — production; cmake default + target backend
    are mbedTLS), the honest `!MBEDTLS` stub (`kn_net.c`, returns −1),
    and the POSIX stub (tail of `kn_net_mbedtls.c`). Fixes go in the
    Win32 body; in the sandbox only the cross `-DKN_TLS_BACKEND_MBEDTLS=1`
    legs even COMPILE it — there is no runtime test off-target.

## 9. Repository map

```
README.md               layout, builds, provisioning, acknowledgements
START-HERE.md           THIS FILE — living handoff (update every round)
CMakeLists.txt          full build (knvstd + 18 suites; TLS backend choice;
                        KN_FFMPEG_PREFIX auto-detects build/ffmpeg/)
docs/BOOTSTRAP.md       the project map (mission, stages, logging, D1-D13)
docs/INTEGRATION.md     per-round delta notes (rev 29 = current)
docs/*.md               per-layer design records + deviations + run history
  RTSP.md §1-§26        the live-protocol run-by-run record (run 21 → §27)
docs/archive/           FROZEN: PROJECT-STATE.md (rounds I-XXXVIII session
                        log), HANDOFF.md, AGENT-HANDOFF.md, analiza/ (+README)
docs/reference/         vendored reference headers (openh264 API, d3d9/dxva2)
docs/research/          early research notes
streamer/include/kn/    public headers (one per module)
streamer/src/           modules: knlog knring knfec kn_aes kn_sha1 knsrtp
                        kn_diag kn_selftest kn_decode kn_d3d kn_gs kn_perf
                        kn_audio kn_net(+_mbedtls) kn_rx kn_sock kn_ws
                        kn_rtsp kn_json kn_http kn_cm kn_cert kn_dtls
                        kn_sctp kn_input kn_bw + knvstd_main.c
streamer/tests/         test_kn*.c (18 suites) + KAT vector headers +
                        test_port.h (scratch dirs live under logs/)
streamer/assets/        synthetic test clips (synthetic720.264)
tools/                  *_reference.py (independent models — authoritative)
                        build_native_tests.sh · cross_check.sh ·
                        fake_rig.py · sandbox_* harnesses · check_*.py ·
                        verify_c.sh · test_json_layer.sh · prepare_*.sh
tests/                  T1..T13 harnesses (T10 = the target build gate) +
                        requirements.txt; verdicts → logs/TNN_{wynik,skrot}.txt
third_party/            vendored pins (gitignored; T10 auto-stages on the
                        target; sandbox recipe: docs/archive/PROJECT-STATE.md
                        §Bootstrapping + tools/prepare_mbedtls_sandbox.sh)
build/                  ALL build outputs (gitignored) — EXCEPT the tracked
                        build/ffmpeg/ (the user's prebuilt FFmpeg dev tree)
logs/                   ALL logs/evidence (gitignored): knvstd --log-dir
                        default; T-harness dirs logs/tNN/; native-gate
                        evidence; verdict files
```

## 10. Round history at a glance (detail: docs/archive/PROJECT-STATE.md)

| rounds | milestone |
|---|---|
| I–XIV | skeleton, logging, loopback, audio, D3D9 presentation settled (D7), decode pipeline, T1–T9 gates |
| XV–XIX | target networking fixed (Win7 TLS), kn_rx pipeline + vector machinery, the socket layer (T13), Winsock buffer proof |
| XX–XXII | signaling cores (kn_ws/kn_rtsp), the TLS stream, CloudMatch (kn_cm), `--session` born, E2E fake rig |
| XXIII–XXVIII | R7 runs 1–10: live create/poll against production, the control-node re-learning fix, the 503 class eliminated, PLAY 455 → the pump thread |
| XXIX–XXXV | R3 series: kn_cert → kn_dtls (DTLS 1.2 live on run 13) → kn_sctp/usrsctp (run 14–15: INIT sent, silence) → nomination (D-t8) + use_srtp (D-t9) |
| XXXVI | **run 16: PLAY 200 — first real GFN video end-to-end**; the full feedback sender set ships |
| XXXVII | run 17: feedback disproved as the stall cause; `kn_input` + the activation chain (three-implementation pinned) |
| XXXVIII | **run 18: stall moved 11 → 30.5 s** (activation confirmed); the `nvst_bandwidth.rs` port (D-t10 closed), ControlReportBatch, server-control latches, SCTP cause codes, AU telemetry, R3-D |
| XXXIX | **target T10 ALL PASS 79 — round XXXVIII proven on Win7** (knbw 387 on target). **Run 20: the feedback plane is proven healthy but the server drip-fed video from second zero** (40 AUs/400 s → RECOVERY_REQUIRED; `frames_observed:40<100` ⇒ warm-up zero estimates) → STAGE 2 open; repo reorganization (logs/, build/, docs/archive/, README with attributions, tracked `build/ffmpeg/`) + the `outbox_by_sid` JSON-braces fix |
| XL | **run 21 root-caused the run-18→20 regression from the log alone: Windows `SO_RCVTIMEO` ~500 ms granularity made the R3-D WSS drain pin the media loop to the server's 2 Hz datagram cadence** (QoS flew at 2/s not 20/s; `recv_timeouts=1` was the paradox) → the `select()`-based TLS recv fix + D1–D7 fidelity set (reliable IDR + 100 ms retry, real RR jitter, frame-ack `first_packet_time_ms`, standalone pacing, NACK split, RFI sid 0) + C18 build stamp + C19 `loop_ticks` + F1 the 0x010a dumper; knrx 203 (jitter golden 369). Run 22 = clean rebuild + ACTIVE game at handoff |

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
- **Live-run evidence = stdout capture PLUS the `logs/` directory.**
  `knvstd --session` writes its knlog channel files to `--log-dir`
  (default `logs/`) — the 1 Hz `session_stats` lines with the estimator
  evolution exist ONLY there (run 20's stdout alone could not answer
  cause-vs-effect on the QoS fields). Always ask for both.
