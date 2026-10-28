# START HERE — KarnyNow entry point and living handoff (created at the close of round XXXVIII, 2026-10-27; updated at round XLIV close, 2026-10-28)

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
4. **`docs/RTSP.md` §31–§32** — the two most recent rounds' records
   (run 24's clone draw, the F2/F3 yields, the 403 seat probe, the
   lottery driver; run 25's false-keeper catch, the full
   SESSION_LENGTH_TIMER, the F6 exit-code fix). §1–§30 are the
   run-by-run history of the live protocol work.
5. **`docs/INTEGRATION.md` rev 35 (the top block)** — the current delta:
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

## 3. Current state (as of round XLIV close, 2026-10-28)

**Live progress (R7 runs against the real production cloud):**

| run | round | outcome |
|---|---|---|
| 16 | XXXVI | **PLAY 200 — first real GFN video through the full stack** (307 AUs, 1280×720, zero errors); ~10.2 s stall |
| 17 | XXXVII | feedback set flew for 242 s, stall UNMOVED at 10.99 s → feedback hypothesis disproved; activation chain identified as the missing startup item |
| 18 | XXXVIII | **activation chain worked — stall moved to 30.5 s** (562 AUs, 13 keyframes, 6665/6665 datagrams authenticated, 0 decode errors); AU frame-size decay under zero loss named the NEXT gate: the server rate controller ramping down against our **zero-valued bandwidth-estimator fields** in QoS `0x0207` (deviation D-t10) |
| 20 | XXXIX | **the BWE evidence set flew and PROVED the feedback plane healthy** (skip_* all 0, batch flushes 796 / failed 0 / retained 0, outbox dropped 0 on every sid, terminations 0, pacing never disabled) — but the server **drip-fed video from second zero**: 40 AUs in 400 s (1.275 s of media time) in ~5 s bursts, then ONE gap → RECOVERY_REQUIRED until the end (137 drops, no keyframe despite 42 IDR/PLI). `frames_observed:40 < 100` ⇒ `estimate_kbps:0` is the reference's OWN warm-up zero. **STAGE 2 REMAINS OPEN** — full analysis `docs/RTSP.md` §26 |
| 21 | XL | **the regression is ROOT-CAUSED from the log alone**: identical loop-tick signature to run 20 (iterations = datagrams+1, `recv_timeouts=1` against a 10 ms poll, QoS 597 = 2/s not 20/s) ⇒ the media loop was pinned to the server's 500 ms datagram cadence by the R3-D WSS drain blocking in `SO_RCVTIMEO` (Windows rounds it to the ~500 ms AFD granularity — `select()` is the ms-accurate primitive). The session survived 300 s (user attached via web client); the media clock advanced only 1.18 s (`rtp_ms` 1182) = the game screen was STATIC — a second, independent depressant. Fixed + D1–D7 fidelity set shipped — full analysis `docs/RTSP.md` §27 |
| 22 | XLI | **THE LOOP FIX IS PROVEN AND THE FIRST SUSTAINED REAL-RATE VIDEO FLOWED**: `loop_ticks` 42 882 = 143/s, `recv_timeouts` 27 095 = ticks − datagrams − 1 (the paradox inverted into the proof), QoS 19.5/s, 15 786/15 786 datagrams authenticated, 1139 AUs — 1013 of them a continuous 29.98 fps @ ~3.7 Mbps stream, zero gaps, zero decode errors; BWE left warm-up for the first time (estimate 25 398 kbps); 0x010a DECODED (JSON app-lifecycle notifications). **Then the server froze EVERY plane at ~t+30–45 s** (video, 1 Hz heartbeat, WSS; no termination/warning; WSS errored only at ~t+216 s) and never answered 2535 reliable IDR+PLI. Run 22 exposed our own dead end: nothing ever called `kn_rx_video_recover` → D8 the reference's bounded recovery contract ships — full analysis `docs/RTSP.md` §28 |
| 23 | XLII | **THE DECISION-MATRIX RUN — INPUT ELIMINATED**: the XLI.1 jiggle flew exactly as designed (`first_send` t+1.28 s, **198** reports on sid 6 — opened 8/8 acked, `dropped:0`) and the server froze every plane ANYWAY at **10.58 s** of pristine video (319/319 AUs decoded @ 30.15 fps continuous, zero gaps, BWE warm at **34.8 Mbps** peak) — EARLIER than run 22's zero-input 41 s. **D8's live debut to the second**: stall+5 s → recover + 50 IDR/PLI → +5 s → terminal → `stop_reason:"recovery_exhausted"`, clean exit at 20.6 s (vs run 22's 248 s of dead air). The 0x010a sequence is IDENTICAL to run 22's: Arknights: Endfield never got past its own **Qt launcher portal** (`QtWebEngineProcess.exe`, foreground lost to pid 0) in either run. With the same app state freezing at 41 s once and 10.6 s once, the run-16→22 fidelity ladder is DEAD: media survival (10.2, 11.0, 30.5, ~1.3, ~1.2, ~41, 10.6 s over runs 16–23) is a **rig-side lottery**. WSS has NEVER ponged in any run. Ships F2 (0x010a cap 800→2400 hex — the 566 B `SESSION_LENGTH_TIMER` was truncated by our own cap in both runs) + F3 (`rel_ms` on the control-evidence lines) — full analysis `docs/RTSP.md` §30 |
| 24 | XLIII | **LOTTERY DRAW #4 — A NEAR-EXACT CLONE OF RUN 23**: same app state (Arknights' Qt launcher portal), freeze at **10.43 s** of media (vs 10.58 s), `recovery_exhausted` at wall **20.54 s** (vs 20.56 s), 314/314 AUs decoded, zero gaps, BWE warm (31.7 Mbps peak), jiggle 200 reports / 0 drops. **F3's first flight**: ALL control traffic arrived inside the first 10 s (`ctrl_in rel_ms:10000`, 15 msgs, no 0x0101) — the freeze lands on an already-silent control plane. **F2's first flight**: the eight-notification startup burst decoded IN FULL (t+257–264 ms; two payloads at 936/970 hex chars were truncated by the old 800 cap in runs 22–23) — the rig ran this Arknights session under **`drsProfileName:"EA Desktop"`**, final foreground `Games.exe fullscreen:false`; NO `SESSION_LENGTH_TIMER` this draw (run 23 had one — the notification set varies per draw). The ~10.5 s mode is now the plurality (4 of 8 draws). The operator's post-run probe settled the deferred restart question: instant cm_create → bare **HTTP 403** twice, same token queued after a pause ⇒ **403 = seat cooldown, token reusable** — full analysis `docs/RTSP.md` §31 |
| 25 | XLIV | **THE LOTTERY'S FIRST LIVE FLIGHT — AND ITS FIRST BUG CATCH**: F5 stopped at draw 1 of 5 because `run_session`'s exit code returned **0 for any session with ≥1 datagram** (pre-D8 legacy — runs 23/24 exited 0 too, nobody consumed the code until the lottery did) so the frozen draw looked like a keeper. **F6** ships the honest contract (only `"time"`/`"expect_frames"` exit 0; every terminal stop_reason exits 1) + the corrected lottery stop rule (only rc 1 re-draws; rc 0 = keeper; 2/3/Ctrl-C = abort). The single draw still paid twice: **F2 read the FULL `SESSION_LENGTH_TIMER` — `beforeEventMS:3599657` ≈ a ONE-HOUR budget — killing the session-limit theory of the early freezes**; and it was the first LOSSY draw on record (gaps 2, fec_repaired 6, nack 5, withheld 90, lossy_frames 5 — yet **257/257 AUs decoded**; BWE all-time peak **45.9 Mbps**) on a DIFFERENT rig (80.250.97.36). 0x0101 returned; `sctl_unknown` = opcode 0x020e (the reference doesn't know it either); the EA-Desktop drsProfile misclassification reproduced exactly; media died at **11.59 s** — the ~10–12 s band is now **5 of 9 draws**. Target T10 on the XLIII build: **ALL PASS 79** (knrx 243) — full analysis `docs/RTSP.md` §32 |

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

**Round XLI shipped (the recovery contract + run-22 evidence set):**

- **D8 — the reference's BOUNDED RECOVERY CONTRACT** (run 22's dead
  end): `dec_recovery_attempt` in `knvstd_main.c` ports
  `attempt_nvst_recovery` (opennow-streamer-core `lib.rs:2448-2477`,
  `NVST_RECOVERY_ATTEMPT_LIMIT` = 1): the first
  `RECOVERY_TIMEOUT`/`PROGRESS_RECOVERY` event → `rx_recovery`
  (`action:"recover"`) + `kn_rx_video_request_keyframe` +
  `kn_rx_video_recover` (media reset, timeout origin re-anchored, back
  to RUNNING — the kn_rx function existed since round XVII and NOBODY
  CALLED IT; every packet in RECOVERY_REQUIRED is dropped, so runs 20
  and 22 could never come back — even a delivered keyframe would have
  been dropped). The second trigger → `action:"terminal"` →
  **`stop_reason:"recovery_exhausted"`** and the media loop breaks
  (the reference's `"nvst-recovery-exhausted"`; its app then restarts
  the pipeline — a knvstd session restart is deferred). `PacketGap` is
  NOT a trigger (the receiver owns gap recovery, `lib.rs:2269-2273`).
  Session mode only (`c->fb` gate) — `--decode`/`--replay-capture`
  behaviour unchanged, T13 untouched. The recovery action runs AFTER
  the event-print loop (`recover()` does `ev_reset` + `lifecycle_push`
  — it rewrites the array being iterated).
- **`ctrl_ping_dead`** JSONL at both WSS-dead transitions
  (`send_failed` | `recv`+rc): run 22's WSS death (~t+216 s, TCP
  retransmit exhaustion against the frozen server) was only inferable
  from the ping count; run 23 dates it directly.
- **Summary `recovery` block** `{"attempts":N,"exhausted":0|1}`
  appended after `au_clock` (specs 71→73 = args; skeleton `json.loads`
  PASS — the trap-#5 ritual ran in full). **Build stamp → `"XLI"`.**
- **Tests**: knrx 203 → **218** — the recovery state machine: the
  250 ms timeout transition (249 ms silent), the RECOVERY_REQUIRED
  drop guard (reason 3), the full `recover()` contract (→1, RUNNING,
  Lifecycle event, origin re-anchored, no-op from RUNNING), **the
  keyframe latch SURVIVES the media reset** (the D3 retry loop's
  lifeline), and kn_rx `recover()` is deliberately NOT attempt-limited
  (the budget lives in the session layer). `prop_h_init_to` (timeout
  param) added; existing call sites unchanged.
- **D-t13 (deferred, on evidence)**: the reference's decode-side
  progress watchdog (`decode_progress.rs` → the same attempt path +
  the recovery-grace terminal deadline) is not ported; no live run has
  ever shown decode-output starvation with healthy packet flow.

**Round XLI.1 shipped (the operator's answers + the first G3 slice —
the synthetic mouse jiggle):**

- **The three operator answers reshaped the hypothesis set**: (a) the
  web client is opened only AFTER knvstd finishes — run 22's sole
  receiver was ours, so the takeover branch is DEAD; (b) the session
  app was `--session-app=107057914`, matching the 0x010a `shortName`;
  (c) ZERO input ever reached the session. With input absent, the
  server-side **idle/no-input policy** becomes the prime freeze
  suspect: survival is monotone in client fidelity (16/17 ~10-11 s →
  18 ~30.5 s → 22 ~45 s) and input was the one missing item —
  START-HERE §4.4's reconsideration condition for the deferred jiggle
  is met (without shipped input, run 23 could not separate idle-policy
  from rig flake).
- **`kn_input_mouse_relative(dx, dy, ts_us)`** — the 44-byte
  `0x0206`/`INPUT_MOUSE_RELATIVE(7)` message (dx/dy **BE i16** at wire
  [20..24], ONE **LE u64** timestamp tail), byte-exact against the
  reference's OWN Rust golden (nvst_input.rs:1888-1911). Two
  implementations as always: `tools/input_reference.py` (`mouse_relative`
  + the golden in `self_prove()` + 2 new KAT singles → 13) and the C
  builder; `test_kninput` 446 → **548**; the shipped binary re-proves
  via `input.vectors` 48 → **50**.
- **Routing = control_partial = sid 6** (unordered, PR-SCTP TTL
  300 ms — nvst_input.rs:969 + NVST_CHANNEL_PROFILE[3]; profile INDEX
  3 ↔ SID 6, sids are 2×index). It rides the EXISTING
  `sess_fb_push(fb, 6, …)` outbox path — the QoS batch's channel, no
  new send path.
- **The jiggle cadence** (media loop, after the keepalive block):
  every `SESS_JIGGLE_INTERVAL_MS` (100 ms = 10 Hz) one ±1 px relative
  move, sign alternating (ZERO net cursor drift), timestamp on
  `pump.origin_us` (the activation chain's epoch). Gate: the pump's
  new `volatile unsigned input_active` mirror of the activation latch
  (single writer `sess_ctrl_try_activate`; the `sctl_*` single-word
  read precedent) — nothing flies before `enable-input(true)`. Volume
  440 B/s — run 22 carried the 50 ms QoS cadence on the same channel
  with `dropped:0`.
- **Evidence**: one `input_jiggle`/`first_send` stdout line
  (`{sid:6, interval_ms:100, ts_us}`); summary `feedback.jiggle`
  (specs 73 → **74** = args; skeleton `json.loads` PASS; validator now
  covers **108** JSON printf blocks). **Build stamp → `"XLI.1"`.**
- **Run-22 files requested from the operator** (listed on disk):
  `transport.jsonl` (the 1 Hz `session_stats` — pins the freeze
  instant + the estimator ramp), `T10_skrot.txt`/`T10_wynik.txt` (the
  XL-rebuild target verdict), `diag.log`, `app.log`, `http.log`.

**Round XLII shipped (run-23 verdict + the two instrumentation gaps it
exposed — evidence-only, no wire-format change):**

- **Run 23's verdict** (RTSP §30): the jiggle flew, the freeze came
  anyway and EARLIER than run 22's → **input eliminated** as a
  survival factor; the fidelity ladder is dead; media survival is a
  **rig-side lottery** (1.2–41 s across runs 16–23). The one
  client-side variable never tested: runs 20–23 ALL parked on
  Arknights: Endfield's **Qt launcher/login portal** — gameplay was
  never reached (0x010a sequences identical in 22+23:
  `QtWebEngineProcess.exe` + foreground lost to pid 0).
- **D8's live debut, to the second**: `progress_stall` (+5 s idle) →
  `rx_recovery recover` + 50 IDR/PLI retries → (+5 s) →
  `terminal` → `stop_reason:"recovery_exhausted"` → clean exit at
  wall 20.6 s. The knlog `transport.jsonl` (1 Hz `session_stats` +
  `rx_event`/`rx_recovery` lines — captured live for the first time)
  timestamps every step; stdout and knlog agree to the second.
- **F2**: `sess_ctrl_dump_010a` hex cap 800 → **2400** chars (1200 B
  payload; every 0x010a so far ≤ 566 B). The 566-byte
  `SESSION_LENGTH_TIMER` was truncated by our OWN cap in runs 22 AND
  23 — its body has never been read; run 24 reads it.
- **F3**: `rel_ms` (session-relative, the activation epoch) on
  `ctrl_msg`, `sctl_010a`, `ctrl_in` — run 23 could not order the
  app-lifecycle notifications against the media freeze; run 24 can,
  to the millisecond.
- **The jiggle stays ON** (controlled constant; 440 B/s; the idle
  policy is not formally dead — only 9.2 s of input preceded this
  freeze). Deferred on evidence: the auto-restart loop after terminal
  (needs a `--session-token` reuse probe first), D-t13.
- **Docs correction**: `test_knrx`'s count is RECIPE-dependent —
  raw-gcc script **218**, cmake/ninja (T10, sandbox AND Win7) **243**
  (+25; the XL/XLI "expect 218 on target" was wrong, verdicts
  unaffected). Target run-23 T10: **ALL PASS 79**, knrx 228→243 (+15
  = D8, exactly the sandbox delta), kninput 446→**548** (+102),
  `input.vectors` **50**, knselftest 116 stable.

**Round XLIII shipped (run-24 analysis + the restart-loop unblock —
driver-side only, no wire-format change):**

- **Run 24's verdict** (RTSP §31): the lottery drew the same slot
  twice — a near-exact clone of run 23 (10.43 s vs 10.58 s of media,
  recovery_exhausted at 20.54 s vs 20.56 s, every counter matching).
  F3 ordered the control plane: everything arrived inside the first
  10 s; the freeze lands on silence. F2 read the full 0x010a burst:
  eight notifications at t+257–264 ms (the launcher-portal startup,
  NOT the freeze trigger), and the rig's own classification —
  `"drsProfileName":"EA Desktop"` on an Arknights session (it follows
  the window it sees, `QtWebEngineProcess.exe`, not our cmsId). No
  SESSION_LENGTH_TIMER this draw; the full timer body is still
  unread — only a draw that carries it can deliver it.
- **F4**: cm_create **403 backoff** — a bare 403 means "the rig still
  holds your previous seat" (run24v2 field probe: two instant 403s,
  same token queued after a pause). knvstd now waits 20 s, rebuilds
  the TLS stream and retries, ≤ 3 times, printing
  `cm_create_403_backoff` per wait. Every other failure aborts
  instantly, exactly as before.
- **F5**: the **lottery driver** — `--lottery=N` (default 1 =
  byte-identical legacy path) + `--lottery-gap=SEC` (default 20)
  re-draw the WHOLE bring-up N times in one invocation:
  `lottery_gap` / `lottery_draw {draw,of,rc}` / `lottery_summary`
  envelope lines, exit code = last rc, **rc == 0 (a surviving draw)
  stops the lottery early**. In-process by design — `run_session()`
  passed a full re-callability audit (every stateful static
  re-initialised at entry, destroyed before return; the kn_log cycle
  is the selftest battery's own pattern).
- **Evidence**: 4 new JSONL line shapes; the validator now covers
  **112** JSON printf blocks (summary unchanged **74 = 74**).
  **Build stamp → `"XLIII"`.**

**Round XLIV shipped (run-25 analysis + the F6 exit-code fix —
driver-side only, no wire-format change):**

- **Run 25's verdict** (RTSP §32): the lottery stopped at draw 1 of 5
  — our own bug, caught live on its first flight. The draw itself
  delivered the project's biggest single control-plane fact yet: the
  FULL `SESSION_LENGTH_TIMER` body (`timerType:"DEFAULT",
  presentDurationMS:0, beforeEventMS:3599657`) — the rig granted a
  ONE-HOUR session budget, so the ~10–12 s freezes are conclusively
  NOT a session limit. Plus the first lossy draw (gaps 2 / fec 6 /
  nack 5 / withheld 90 — 257/257 decoded anyway: the receive pipeline
  is now loss-proven live), a different rig IP freezing identically,
  and the fifth consecutive ~10–12 s draw.
- **F6a**: the honest exit code — only the natural endings (`"time"`,
  `"expect_frames"`) can exit 0; `recovery_exhausted`,
  `server_termination`, `socket_error`, `queue_timeout`, `terminated`,
  `session_not_found`, `interrupted`, `cm_poll_*`, `signal` all exit
  1, matching usage()'s documented contract ("1 failed verdict") for
  the first time. Historical note: runs 23–25 exited 0 despite
  freezing; nothing in the gate set consumes the live-session exit
  code, so no verdict ever depended on the old behaviour.
- **F6b**: the lottery stop rule — re-draw ONLY on rc 1; rc 0 = keeper
  (stop and keep the rig); rc 2/3 = usage/internal (a re-draw cannot
  fix them — stop); `g_knv_stop` (Ctrl-C) = stop. Sandbox-proven: on
  the TLS-less build `--lottery=5` now ends after draw 1 with
  `last_rc:3`.
- **Evidence**: no new line shapes (the F5 envelope carried the
  diagnosis — `lottery_draw rc:0` next to `stop_reason:
  "recovery_exhausted"` IS the bug report); validator unchanged at
  **112** blocks, summary **74 = 74**. **Build stamp → `"XLIV"`.**

**Sandbox gates at XLIV close (all green):** `build_native_tests.sh`
**NATIVE TESTS PASS** (per-suite counts unchanged: knrx 218, kninput
548, knbw 387, knselftest 117; paranoid legs incl.) · `cross_check.sh`
PASS ×2 arches, zero warnings · ctest **20/20 ×3** (native + mbedTLS +
schannel) · T10 POSIX dry **69 checks, 0 failed** · T13 **ALL PASS
63** · summary-printf validation **112** blocks, 0 mismatches,
summary skeleton `json.loads` PASS · F6b field test (`--lottery=5` on
the TLS-less build: stops after draw 1, `last_rc:3`, exit 3, envelope
`json.loads` 4/4, stamp XLIV) · `sandbox_session_e2e` 43/43 carried
from XLIII (e2e links no file XLIV touched) · ASan/UBSan carried from
XLI.1 (no wire/decoder code touched in XLIII/XLIV).

**Codebase shape:** 28 `streamer/src/*.c` + 26 `streamer/include/kn/*.h`;
the production binary is `knvstd` (`knvstd_main.c`, 6744 lines: modes
`--capture`, `--decode`, `--replay-capture`, `--session`, `--selftest`,
`--list-selftests`); 15 registered production self-tests; 18 native test
suites. Third-party (vendored, gitignored, re-stage if cleaned):
tiny-AES-c, crypto-algorithms-sha1, usrsctp, mbedTLS 3.6.0 (+framework).

## 4. What to do next (in order)

> The operator asked for **very simple, step-by-step** instructions —
> this section is written to be followed literally, one step at a time.

**Step 1 — sync these 4 files** (round XLIV delta, on top of the
already-synced XLIII set):

- `streamer/src/knvstd_main.c`
- `docs/RTSP.md`
- `docs/INTEGRATION.md`
- `START-HERE.md`

**Step 2 — clean rebuild + T10 on Win7** (the usual procedure):
delete the build tree, build, run T10 in full.
Expected verdict: **ALL PASS 79**, `test_knrx` **243**,
`test_kninput` **548**, `input.vectors` **50** — identical to run
25's fresh T10 (XLIV changed no test file).

**Step 3 — run 26, the lottery — the SAME command as run 25** (only
the file names change; replace `<TOKEN>`):

```
knvstd.exe --session --session-token=<TOKEN> --session-app=107057914 --session-seconds=360 --lottery=5 --lottery-gap=30 --session-au-dir=logs\aus_r26 > logs\run26.log
```

What happens NOW (after the F6 fix): each dead draw (`recovery_
exhausted`) exits 1 → the lottery waits 30 s and **draws again**, up
to 5 times. It stops early ONLY if a draw survives the full 360 s
(that is a keeper — leave the machine alone, it is streaming). HTTP
403 seat cooldowns are handled automatically (`cm_create_403_
backoff` lines are NORMAL between draws). Ctrl+C ends everything.
Expect ~20–60 minutes total (5 × queue + ~20 s per dead draw +
pauses).

**Step 4 — check the first line** of `logs\run26.log`:
`{"mode":"build","compiled":"...","round":"XLIV"}` — if it says
`XLIII`, the rebuild did not take; redo Step 2.

**Step 5 — send back:**

1. `logs\run26.log` — the WHOLE file (every draw's summary is in it);
2. the entire `logs\` directory (the LAST draw's knlog channels);
3. the fresh `T10_skrot.txt` + `T10_wynik.txt` pair.

**Read matrix for run 26 (agent side, RTSP §32.5):** five dead draws
⇒ the ~10–12 s modal band is confirmed at n=5 in one sitting and the
practical stage-2 exit is a persistence lottery (or a gameplay-
reaching app); ANY draw surviving > 60 s ⇒ long draws are reachable
on demand — keep drawing until a full-length keeper closes stage 2;
`lottery_summary draws:K last_rc:0` with K < 5 ⇒ a keeper happened —
the run that changes everything. If the operator can supply a
gameplay-reaching app id, swapping `--session-app=` remains the
single most valuable variable (the Arknights portal draws are now
fully characterised: 1-hour budget, EA-Desktop profile, ~10–12 s
modal death).

**Still deferred (unchanged):** D-t13 (decode-side watchdog), G3
keyboard/wheel/text (deprioritised — input is eliminated as a
survival factor), bundle audio extraction, G1 (input handshake
timeout), G4 (server cursor payloads).

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

## 8. Traps that will bite you (curated; full list in docs/INTEGRATION.md rev 35 §traps)

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
   BE, the haptics `remote_input_message` timestamp is LE, the
   relative-mouse dx/dy are **BE i16** (signed; negatives and the i16
   extremes are test-pinned) while its single timestamp tail is **LE
   u64** (key/absolute carry TWO tails). All test-pinned; do not
   "unify". And **control_partial is sid 6, not sid 4** —
   NVST_CHANNEL_PROFILE's creation INDEX (3) ≠ its SID (6); every sid
   is 2×index. Mouse motion on the reliable/ordered sid 0 would be a
   fidelity break the reference's own comment warns about.
5. **Patch discipline on `knvstd_main.c`** (6.6k lines, heavily
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
13. **A bare HTTP 403 from cm_create is a SEAT COOLDOWN, not a dead
    token** (run24v2: two instant 403s after a recovery_exhausted
    exit, then the SAME token queued fine after a pause). F4 backs
    off automatically (20 s × 3, `cm_create_403_backoff` lines). The
    JSON-body sibling `cloud_status 11 = SESSION_LIMIT` is a
    different signal and is deliberately NOT retried.
14. **The live-session exit code only became honest in XLIV (F6a)**:
    pre-XLIV builds exited **0 even for `recovery_exhausted`** (any
    media at all ⇒ 0) — which is why run 25's lottery stopped at
    draw 1. Since XLIV, exit 0 MEANS "the draw survived its full
    `--session-seconds` (or met `--session-expect-frames`)"; every
    terminal stop_reason exits 1. Never build automation on pre-XLIV
    exit-code folklore.

## 9. Repository map

```
README.md               layout, builds, provisioning, acknowledgements
START-HERE.md           THIS FILE — living handoff (update every round)
CMakeLists.txt          full build (knvstd + 18 suites; TLS backend choice;
                        KN_FFMPEG_PREFIX auto-detects build/ffmpeg/)
docs/BOOTSTRAP.md       the project map (mission, stages, logging, D1-D13)
docs/INTEGRATION.md     per-round delta notes (rev 31 = the top block)
docs/*.md               per-layer design records + deviations + run history
  RTSP.md §1-§27        the live-protocol run-by-run record (run 22 → §28)
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
| XLI | **run 22 PROVED the loop fix live** (`loop_ticks` 143/s, `recv_timeouts` = ticks − datagrams − 1, QoS 19.5/s) **and delivered the first sustained real-rate GFN video** (1013 continuous frames @ 29.98 fps ≈ 3.7 Mbps, zero gaps/errors; BWE estimate 25 398 kbps — warm-up left for the first time; 0x010a decoded = JSON app notifications) — **then the server froze every plane at ~t+30–45 s** (no termination; WSS died ~t+216 s; 2535 reliable IDRs unanswered) and exposed our dead end: `kn_rx_video_recover` was never called → **D8 the reference's bounded recovery contract ships** (attempt 1 = keyframe + recover, attempt 2 = terminal `recovery_exhausted`) + `ctrl_ping_dead` + the summary `recovery` block + knrx 218; D-t13 defers the decode-side watchdog. Run 23 = T10 (expect 79, knrx 218) + the run-22 procedure answers + stdout AND `logs/` |
| XLI.1 | **the operator's answers killed the takeover theory and revived idle/no-input as the prime freeze suspect** (the web client was never attached during run 22; the app matched the 0x010a shortName; ZERO input ever reached the session — survival is monotone in fidelity: ~10-11 s → ~30.5 s → ~45 s, and input was the one missing item) → **G3's first slice ships**: `kn_input_mouse_relative` byte-exact against the reference's OWN Rust golden (nvst_input.rs:1888-1911), routed to control_partial (**sid 6**, unordered PR-TTL 300 ms), as a gated **10 Hz ±1 px zero-net-drift jiggle** on the existing outbox path (gate: the pump's volatile activation latch). kninput 446→**548**, `input.vectors` 48→**50**, KAT singles 11→13, summary 73→**74** specs, stamp **XLI.1**. Run 23 = the decision matrix (RTSP §29.7): survival past ~45 s ⇒ idle policy CONFIRMED; freeze with the jiggle flying ⇒ input eliminated → rig-side, and D8 dates it (`recovery_exhausted`, early end) |
| XLII | **run 23 = the decision-matrix run: THE JIGGLE FLEW (198 reports, sid 6, 0 drops) AND THE FREEZE CAME ANYWAY at 10.58 s** — earlier than run 22's zero-input 41 s ⇒ **input eliminated**, the fidelity ladder is dead, media survival (1.2–41 s over runs 16–23) is a **rig-side lottery**. **D8's live debut to the second** (stall+5 s → recover + 50 IDR/PLI → +5 s → terminal → `recovery_exhausted`, exit 20.6 s vs run 22's 248 s dead air). 0x010a IDENTICAL to run 22: Arknights never left its **Qt launcher portal** (foreground lost pid 0) — the untested variable is the APP. WSS never ponged in any run. Target T10 **79** (knrx +15=243 cmake-recipe, kninput 548, vectors 50). Ships **F2** (010a cap 800→2400 — `SESSION_LENGTH_TIMER` was self-truncated in both runs) + **F3** (`rel_ms` on ctrl evidence) + stamp XLII; jiggle stays ON. Run 24 = a gameplay-reaching app + optional 2–3 lottery draws |
| XLIII | **run 24 = lottery draw #4, a near-exact CLONE of run 23** (freeze 10.43 s vs 10.58 s of media, `recovery_exhausted` 20.54 s vs 20.56 s; 314/314 AUs, zero gaps, jiggle 200/0) — **F3** proved all control traffic arrived inside the first 10 s (the freeze lands on a silent control plane); **F2** decoded the eight-notification burst in full (t+257–264 ms; two payloads had been truncated by the old 800 hex cap) and exposed the rig's classification: the Arknights session runs under **`drsProfileName:"EA Desktop"`** (it follows the Qt window, not our cmsId); NO SESSION_LENGTH_TIMER this draw — the set varies per draw. The ~10.5 s mode is now the plurality (4 of 8 draws). Operator probe: instant cm_create after exit → bare **HTTP 403** ×2, same token queued after a pause ⇒ seat cooldown, token reusable. Ships **F4** (403 backoff 20 s × 3, TLS rebuilt per retry) + **F5** (`--lottery=N`/`--lottery-gap=SEC` in-process re-draw driver with `lottery_gap/draw/summary` envelope, early stop on rc==0, full re-callability audit of `run_session()`) + stamp **XLIII**; validator 112 blocks. Run 25 = the first unattended lottery: five draws, one command |
| XLIV | **run 25 = the lottery's first live flight caught our own bug**: it stopped at draw 1 of 5 because `run_session` exited **0 for any session with ≥1 datagram** (pre-D8 legacy contract drift — runs 23/24 exited 0 too) and F5 read rc 0 as "keeper". **F6a** restores the documented contract (only `"time"`/`"expect_frames"` exit 0; every terminal stop_reason exits 1) and **F6b** fixes the stop rule (only rc 1 re-draws; 0 = keeper; 2/3/Ctrl-C = abort). The lone draw still made the round: **F2 read the FULL `SESSION_LENGTH_TIMER` — `beforeEventMS:3599657` = a ONE-HOUR budget ⇒ the session-limit theory of the ~10–12 s freezes is DEAD**; first LOSSY draw (gaps 2, fec 6, nack 5, withheld 90 — 257/257 decoded: the pipeline is loss-proven live; BWE record 45.9 Mbps) on a different rig IP; 0x0101 back; 0x020e logged as `sctl_unknown` (the reference doesn't know it either); EA-Desktop drsProfile reproduced. Media 11.59 s — the band is **5 of 9 draws**. Target T10 (XLIII build): ALL PASS 79, knrx 243. Stamp **XLIV**. Run 26 = the same command — five real draws |

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
