# START HERE — KarnyNow entry point and living handoff (created at the close of round XXXVIII, 2026-10-27; updated at round L close, 2026-10-29)

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
ships code + evidence + docs. The project is at **stage 3 of 5**
(see `docs/BOOTSTRAP.md` §12 + its XLIX status marker): **STAGE 2 IS
CLOSED** (runs 28/28b: two perfect full-length keepers) **AND STAGE 3
IS UNDERWAY** — runs 29/29g measured the **~600 s server ceiling**
(polite TERM at 599.1–599.2 s; silent decay at 594 s) and delivered the
**first gameplay-reaching keeper** (Watch Dogs: Legion, flawless
599.1 s / 17 970 AUs); round XLVIII shipped **F11, the bundle-audio
extraction stage** (`kn_arx` + `kn_ogg` + the RFC 5764 §4.2 DTLS-SRTP
exporter + `--session-audio-out`), sandbox-proven and awaiting run 30
(`docs/BUNDLE-AUDIO.md`). Round XLIX then read run 30: **F11's
extraction is PROVEN LIVE and zero-error on both draws (40 000/40 000
SRTP-authenticated Opus packets, every one RFC 2198 RED, `seq_loss` 0,
a valid 199.92 s OggOpus file) — and the audio is DIGITAL SILENCE,
because Watch Dogs: Legion never got past Ubisoft Connect in either
draw** (the operator's browser control: Legion needs ≈2 min 19 s from
stream start to gameplay audio; our draws died at 75 s and 197 s of
media). The same round **WITHDREW §36's "~600 s server ceiling"** on the
operator's first-hand correction (Warsaw pool, Free tier, one-hour
browser sessions, never cut at 10 minutes) and re-opened the ≈600 s cut
as a suspected server-side **idle/no-input policy**, and shipped F12–F15
(the audio-file close was dead code; one file per draw; the interleaved
jiggle A/B; `--lottery-stop-on-keeper`). Stage 3's remaining scope: a
CONTENT-bearing audio capture (needs a draw that reaches gameplay),
Opus decode + playback, G3 keyboard/wheel/text, G4 cursor payloads, G1
input-handshake timeout, D-t13 (see §4). Round L read **run 31**
(four Legion draws, all `recovery_exhausted` at 14–122 s of media;
**F12/F14/F15 proven live** — `audio_close` per draw, four `lottery_arm`
lines, every draw flown — and **F13 caught broken by its own evidence
line**: the per-draw audio files were delivered as `audio_d1_d1us` …
`audio_d4_d4us` — doubled suffix AND no `.opus` extension — because the
derivation `memcpy`'d OVERLAPPING memory, ASan-flagged on the rig)
and shipped **F16–F19**: the naming rule as one alias-free, separately
tested function (`kn_drawpath`, 1051 checks); **the `.264` answer** —
single-AU dumps are not corrupt, they simply carry no SPS/PPS, proven
from the shipping binary in `logs/f17_video_proof_L.txt` and fixed by a
CONTINUOUS per-draw `video_d<N>.264` plus draw-numbered AU names;
`--session-release` (the RTSP TEARDOWN is now always LOGGED and can be
followed by the CloudMatch DELETE that actually frees the seat — run 31's
orphaned seats cost a 403 SESSION_LIMIT + 20/40/80 s backoff); and
**REAL INPUT** — five byte-exact discrete wire builders (`test_kninput`
794 checks) under `--input-script=FILE` timeline playback, `--input-live`
console forwarding and `--input-anti-afk` (the reference's own F13 pulse),
all behind the activation-chain gate. That is the login-wall answer: the
rig sits at Ubisoft Connect because the GFN *browser* types the
credentials into the stream, and until now our client could not type at
all.

## 2. Reading order for a fresh agent

1. **This file** — state, next steps, agreements, traps.
2. **`README.md`** — repo layout, build/test quick start, the FFmpeg and
   `third_party/` provisioning rules, acknowledgements/licences.
3. **`docs/BOOTSTRAP.md`** — the project map: mission, stage gates,
   logging spec (§7), decision register D1–D13, risk register, research
   provenance. Still authoritative for philosophy and stage definitions.
4. **`docs/RTSP.md` §35–§38** — the four most recent rounds' records
   (runs 28/28b's keepers + the stage-2 closure; runs 29/29g's ≈600 s
   cut — **read §37.4 before believing §36's "ceiling" wording, XLIX
   withdrew it**; run 30's audio flight + the silence finding + F12–F15;
   **§38 = run 31 + round L: the orphaned-seat anatomy, the `.264`
   verdict with its ffprobe evidence, and the F16–F19 contracts**).
   §1–§34 are the run-by-run history of the live protocol work.
5. **`docs/INTEGRATION.md` rev 41 (the top block)** — the current delta:
   new/changed files, integration traps, gate results. Older revs below
   it are superseded history. For the audio stage specifically,
   **`docs/BUNDLE-AUDIO.md`** is the complete contract + run-30
   reaction matrix.
6. **The layer doc for whatever you touch**: `docs/RX.md` (receive
   pipeline), `docs/SRTP.md`, `docs/FEC.md`, `docs/GS.md` (video
   framing), `docs/DTLS.md`, `docs/SCTP.md`, `docs/INPUT.md`
   (control/input plane), `docs/RTSP.md`, `docs/AUTH.md`, `docs/NET.md`,
   `docs/SOCK.md`, `docs/D3D.md`, `docs/AUDIO.md` (playback/WASAPI),
   **`docs/INPUT.md` §9 = the real-input layer (F19): the script format,
   the route table, the wire offsets and the gate**,
   `docs/BUNDLE-AUDIO.md` (audio receive/extraction — F11),
   `docs/LOGGING.md`.
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

## 3. Current state (as of round L close, 2026-10-29)

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
| 26 | XLV | **THE LOTTERY'S FIRST REAL SITTING — 5 draws, 2 sessions, 3 lost at the CM door, BOTH refusals measured**: draw 1 dead at **13.15 s** media (379/379 AUs; lossy: gaps 1 / fec 1 / withheld 16 — decoded perfectly; BWE 36.6 Mbps); draw 2 **403 ×4 over ~90 s** (the seat cooldown is now bounded from below at >90 s); draw 3 **HTTP 429** — CloudMatch rate-limits session creates (first on record; F4 didn't retry it); draw 4 queued after ONE 20 s backoff, dead at **11.34 s** (342/342, clean); draw 5 403×3 → 429. **F6a/F6b live-proven**: honest rc 1 ×5, `lottery_summary draws:5`, exit 1, and `wire_session.log` (the exit≠0 ring dump) reappeared. Both draws: ALL control ≤10 s (`rel_ms:10000`, 0x0101 ×3/×1), NO SESSION_LENGTH_TIMER (intermittent: 23/25 yes, 24/26 no — the one-hour-budget fact stands). **The pattern reopening the jiggle question**: all FIVE jiggle-on draws ever died at 10.4–13.2 s; the only TWO long draws (18/22: 30.5/41 s) were activation-WITHOUT-jiggle — input was eliminated as a SAVIOUR (run 23) but never tested as a DEPRESSANT. Band now **7 of 11 draws**. Target T10 on XLIV: **ALL PASS 79** — full analysis `docs/RTSP.md` §33 |
| 27 | XLVI | **THE JIGGLE A/B — DECIDED: THE SYNTHETIC MOUSE JIGGLE IS A SURVIVAL DEPRESSANT.** Five `--session-jiggle=0` draws (same evening, same app, FIVE fresh rigs — a different IP per draw) lived **10.32 / 21.81 / 15.39 / 52.19 / 22.87 s** of media: four of five beyond the jiggle-ON maximum (13.2 s); draw 4 = **NEW PROJECT RECORD: 52.19 s continuous 30 fps, 1546/1546 AUs, 13 071/13 071 datagrams authenticated, 1 gap, 0 decode errors, BWE 37.8 Mbps** (beats run 22's ~41 s — also jiggle-off). ON draws (23–26): 10.6/10.4/11.6/13.2/11.3 — 5/5 in-band; P ≈ 0.03 % under the OFF population. The activation handshake is EXONERATED (flew 5/5; only the periodic MOTION is implicated). **F7 field-proved**: draws 2–5 each queued on the 4th create after exactly 20+40+80 s of backoff — the seat cooldown is ~2–3 min; no 429 at this cadence. F8 proof: zero `input_jiggle` lines, `"jiggle":0` ×5. Draw 4's control plane stayed ALIVE to t+40 s (53 msgs, heartbeats) — a healthy-draw indicator. No SESSION_LENGTH_TIMER ×5 (2 of last 8 draws carried one). Target T10 on XLV: **ALL PASS 79**. First evidence delivery via the new `karnynow-logs` repo (complete: AU dumps, diag rotations, wire ring) — full analysis `docs/RTSP.md` §34 |
| 28 | XLVII | **THE FIRST KEEPER — STAGE 2 CLOSED.** The 10-draw hunt (jiggle OFF) produced 33.2 s → **201.6 s** (6049/6049 AUs, 91 941/91 941 datagrams, 90 FEC repairs, zero gaps — the endurance record tripled) → **a FULL-LENGTH KEEPER: `stop_reason:"time"`, wall 360.0 s, media 359.96 s, 10 800/10 800 AUs (exactly 30 fps × 360 s), 169 036/169 036 datagrams authenticated, ZERO gaps/decode-errors/recoveries, ONE keyframe, idr/pli never needed, drift −3 ms, BWE 29.2 Mbps** — the lottery stopped on it by design (rc 0, exit 0). Keeper control plane: 0x0101 at ~1 Hz for the full 360 s (333 heartbeats by rel 340 s). Draws 1–2 died honestly (recovery_exhausted) but app.log mislabelled them `E_SESSION_NO_FRAMES` despite 998/6049 perfect decodes → **F10** (stop_reason-driven failure labels). Target T10 on XLVI: ALL PASS 79 — full analysis `docs/RTSP.md` §35 |
| 28b | XLVII | **THE ERA RE-CONTROL BECAME THE SECOND KEEPER — and corrected the XLVI verdict**: with the jiggle back ON (`--session-jiggle=100`), draw 1 ran the FULL 360 s (10 800/10 800 AUs, 171 427/171 427 datagrams, `jiggle:3533` flown, zero gaps/errors, drift −9 ms). An ON keeper means "the jiggle is a depressant" cannot stand as stated: the same evening drifted favourable for BOTH settings (band draws 18:00–20:30Z → OFF tails 20:41Z → OFF keeper 22:05Z → ON keeper 22:22Z). Verdict DOWNGRADED to *suggestive, era-confounded*; **default stays OFF** (not necessary for any survival class, not invariably fatal, asymmetric bot-signature risk); any future input A/B must be same-evening-interleaved (trap 17). The activation handshake stays exonerated and load-bearing — full analysis `docs/RTSP.md` §35.3, `docs/INPUT.md` §9 |
| 29 | XLVII→XLVIII | **THE CEILING'S SILENT FACE + A 594-SECOND FLOOR** (Arknights: Endfield, `--session-seconds=1800 --lottery=3`): draw 1 died in the classic band (`recovery_exhausted`); draw 2 streamed **17 821/17 821 perfect AUs = 594 s** (280 505 datagrams, zero gaps) and then the datagram plane DECAYED GRADUALLY (444 → 0 over ~9 s — decay = ceiling; abrupt silence = rig freeze) into `recovery_exhausted` (wall 604.1 s); draw 3 ran to **599.2 s and got the POLITE termination** (control `0x0109`; TERM `0x80008BE3` code 56 / `0x80008BC7`) → `server_termination`, **17 974/17 974 AUs, 285 078 datagrams, zero gaps**. **The ~600 s session ceiling is server-side**; soaks must target ≈570 s. F10 matched all draws; F7 matched every queue (403 → 20/40/80 s) — full analysis `docs/RTSP.md` §36 |
| 29g | XLVII→XLVIII | **THE FIRST GAMEPLAY-REACHING TEST — AND A FLAWLESS 599-SECOND KEEPER** (Watch Dogs: Legion, app 100361011 — animated loading screens, intros, then a static settings window): draw 1 = **`server_termination` at 599.1 s, 17 970/17 970 AUs decoded, zero gaps, zero decode errors** (only 54 307 datagrams ≈ 3/AU — the static settings window is a low-bitrate scene) — the pipeline holds on real interactive content (scene cuts, video intros, launcher transitions); draws 2–3 died `recovery_exhausted` at **110 s / 144 s of media** (3 297 / 4 323 AUs — the 10–13 s band did NOT reproduce; the freeze lottery lives below the ceiling too). Two ~599 s perfect draws across two apps = the client sustains the full ceiling; only the SERVER ends these sessions — full analysis `docs/RTSP.md` §36 |
| 30 | XLIX | **F11'S FIRST FLIGHT — THE AUDIO PIPELINE IS PROVEN LIVE AND ZERO-ERROR, AND THE .opus IS DIGITAL SILENCE BECAUSE THE GAME NEVER LAUNCHED.** Both draws `recovery_exhausted` (75.0 s / 197.0 s of media — no keeper, `--session-seconds=570` never reached), but the audio stage flew perfectly on both: `audio_arm srtp_derived:1 armed:1 profile:1 pt:111 red_pt:63 channels:2 clock_hz:48000 file:1` ×2, `packets` 15 610 / **40 000** = `bundle_rtp_in` (nothing dropped), **`srtp_fail`/`malformed`/`toc_invalid`/`pt_filtered`/`padded`/`replay_reject`/`seq_loss`/`seq_reorder`/`seq_dup` ALL ZERO**, `red_packets` = `packets` with `red_blocks` = 2× and `red_invalid:0` (every audio packet is RFC 2198 RED + one redundancy block), `last_ms−first_ms` = `packets × 20 ms` exactly, `ogg.dropped_packets/write_errors` 0. The delivered 2 089 866-B file is a valid OggOpus stream (ffprobe: Opus 48 kHz stereo **199.92 s**, 83 kb/s; 834 pages, contiguous seq, monotone granulepos = Σ TOC samples; **39 929/39 936 readable audio packets are TOC `0xEC` = CELT-FB 20 ms**) ⇒ the RFC 5764 §4.2 exporter's SERVER half is proven against live traffic (40 000 HMAC-SHA1-80 authentications cannot pass with a wrong key half). **Decoded content: mean −91.0 dB, max −80.8 dB, RMS 0.51 LSB in every 10 s bucket, all 19 146 240 samples |x|<8 = silence**, and the cause is content-side: 0x010a carries only `upc.exe`/`UplayService.exe`/`UplayWebCore.exe` (Ubisoft Connect — **no game process in either draw**), AU sizes 0.8–1.2 KB/frame = a static launcher window, and the operator's browser control measured Legion's boot at **≈2 min 19 s from stream start to gameplay audio**. Freeze anatomy (first clean separation): video+audio abrupt to zero in one second while **SCTP `0x0101` heartbeats kept arriving ~5 s** and the WSS stayed writable (`pings 41/101, pongs 0/0, dead:0`); draw 2 degraded first (`qos_owd_us` 229 752, `jitter_us` 41 230 at t−4 s). F10 2/2, F7 matched the queue (403 → 20/40/80 s). Target T10 on XLVIII: **ALL PASS 82** (`audio.vectors` 188/0, knogg 163, knarx 62) — full analysis `docs/RTSP.md` §37, `docs/BUNDLE-AUDIO.md` §9 |

| 31 | XLIX→L | **FOUR DRAWS, FOUR `recovery_exhausted` — AND TWO OF OUR OWN FIXES CAUGHT BY THEIR OWN EVIDENCE.** The interleaved A/B (`--lottery=4 --lottery-gap=60 --session-jiggle=100 --lottery-stop-on-keeper=0`, Legion) flew all four draws as designed: `lottery_arm` ×4 with the jiggle alternating 100/0/100/0, `audio_close` ×4 (**F12 proven live**), every draw flown despite survivors (**F15 proven**) — and media survival was 14–122 s, so the ≈600 s question got **NO RESULT** again (no draw lived long enough to reach it). Audio was digital silence once more (−91 dB) for the run-30 reason: 0x010a again shows only `upc.exe`/`UplayService` — **the Ubisoft login wall**, which the operator then explained (retracting XLIX's "Legion launches" note): play.geforcenow.com's web client *types the credentials into the stream*, so the browser is in the game in seconds while our client, which sent **zero discrete input messages**, is handed the launcher's login dialog (`au_00001_k.264` shows it). Video itself was flawless: **5655 AUs, 0 failures**. Two self-inflicted wounds surfaced: **F13's per-draw audio path was mangled** (delivered as `audio_d1_d1us` … `audio_d4_d4us` — doubled suffix AND no `.opus` extension, from an overlapping `memcpy` ASan flagged on the rig) ⇒ F16; and **no TEARDOWN ever reached the wire**, so each draw orphaned its seat and the next create ate a **403 SESSION_LIMIT** + 20/40/80 s backoff ⇒ F18. The input CHANNEL was live-proven (activation 8/8 messages, 0 build failures) — only the *content* was missing ⇒ F19 — full analysis `docs/RTSP.md` §38 |
| 32 | L→M | **THE FIRST TYPING RUN: EVERYTHING FLEW EXCEPT THE TYPING.** Endfield (107057914), the operator's own 80-event/416 s macro, one draw with video+audio+release all on. **T10 on target = ALL PASS 84** (the rev-41 suites-list catch is the only reason `test_kndrawpath` ran on Win7). **F17 proven where it counts**: `video.264` 3 679 210 B, 668/668 frames decode, and the operator WATCHED it — Endfield launcher + animations from t≈0. **F16 proven**: `audio.opus` correctly named, size == `audio_close.bytes`. **F18 PROVEN**: `rtsp_teardown rc:0 status:200` + `cm_release status:200 gone:1` — the first seat this project ever gave back, zero 403 ladders. **F19 armed but silent**: `input_script_loaded` 80 events `unmapped:0`, gate latched at 0.83 s, and `input_summary fired:0 gated:0 sent:0` — the draw died at **29.0 s of media** (39.2 s wall, `recovery_exhausted`, the first LOSSY freeze: 17 gaps / 191 withheld / 31 FEC / 51 NACK, BWE 24–42 Mbps) **before the macro's first timestamp**. Audio silent again (the launcher makes no sound; peak 1 LSB). 0x010a: `Launcher.exe → Games.exe (cmsId 107057914) → QtWebEngineProcess.exe` mislabeled "EA Desktop" — the Qt Endfield launcher UI; canon confirmed by pixels + cmsId. Lesson + fix: the login UI is on screen from t≈1 s ⇒ **run 33's macro starts at t≈6–12 s** with retries and `--input-anti-afk=60000` (INPUT §10.9, RTSP §39) |

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

**Round XLV shipped (run-26 analysis + CM patience + the jiggle A/B —
driver-side only, no wire-format change):**

- **Run 26's verdict** (RTSP §33): the lottery completed 5 honest
  draws (F6 live-proven: rc 1 per dead draw, no false keeper, exit 1,
  the `wire_session.log` ring dump reachable again). Two real
  sessions, both dead in the band (13.15 s / 11.34 s; 379/379 +
  342/342 AUs; draw 1 lossy and decoded perfectly). Three draws lost
  at CloudMatch: the seat cooldown exceeded 90 s (403 ×4), and the
  first-ever **429 rate limiter** killed two draws instantly (F4 only
  retried 403).
- **F7**: cm_create patience widened — retry on 403 + the whole
  `cm_status_transient()` class (incl. 429), escalating
  **20/40/80/160 s** (≤ ~5 min per draw, 5 attempts); event renamed
  **`cm_create_backoff`** with a `"status"` field (403 vs 429 are now
  distinguishable in evidence).
- **F8**: `--session-jiggle=MS` (0–10000, default 100 = unchanged;
  **0 = OFF**) — run_session takes it as a parameter; the media-loop
  gate short-circuits on 0 (no `input_jiggle` line,
  `feedback.jiggle` stays 0). The jiggle stops being a controlled
  constant and becomes the variable under test: ALL five jiggle-on
  draws died at 10.4–13.2 s; the only two long draws ever (runs 18/22)
  were activation-WITHOUT-jiggle. Direction caveat for the XLII
  verdict: input is eliminated as a SAVIOUR (run 23), untested as a
  DEPRESSANT — run 27 answers it.
- **Field correction (rev 36 trap 2)**: the knlog channel files
  **APPEND across lottery draws** within one process (run 26's
  `transport.log` held both sessions; diag rotates at ~2 MB) — the
  rev-34 "overwritten per draw" note was wrong; a lottery run's
  `logs/` directory holds the WHOLE run.
- **Evidence**: one renamed + reshaped line (`cm_create_backoff`),
  validator unchanged at **112** blocks, summary **74 = 74**.
  **Build stamp → `"XLV"`.**

**Round XLVI shipped (run-27 verdict + the default flip — one-line
behaviour change, driver-side only):**

- **Run 27's verdict** (RTSP §34, INPUT.md §9 corrected): the jiggle
  A/B is decided — the synthetic mouse motion TRUNCATES draws to the
  ~10–13 s band (ON: 5/5 in-band; OFF: 4/5 beyond the ON maximum,
  record 52.19 s). The XLII "jiggle stays ON as a controlled
  constant" decision is REVERSED on evidence; the "fidelity ladder"
  reinterpretation is final: survival tracked the jiggle penalty and
  rig variance, never client fidelity. The activation handshake is
  exonerated (flew in all five OFF draws; the long draws came WITH
  it). Mechanism unresolved (synthetic-input detection / portal
  cursor churn / DRS interaction) and no longer blocking.
- **F9**: `--session-jiggle` default **0 (OFF)**; `100` restores the
  XLI.1 legacy behaviour for re-verification. usage(), the F8 comment
  and the define carry the evidence.
- **F7's field proof**: the seat cooldown measured at ~2–3 min (four
  of four draws queued on the 4th create after 20+40+80 s); the
  escalating schedule matches the field exactly; the 429 limiter
  stays quiet at a ≥ ~3 min/draw cadence.
- **New evidence workflow**: the operator pushes each run's whole
  `logs/` + stdout to **`github.com/karnyjohnny/karnynow-logs`**
  (cleaned per round); the agent clones, archives to `runNN_logs/`
  in the workspace IMMEDIATELY (the repo is wiped each round — the
  workspace is the history), then analyzes. Run 27 arrived complete,
  including `aus_r27/` (1587 AU files) and all diag rotations.
- **Evidence**: no new line shapes; validator unchanged at **112**
  blocks, summary **74 = 74**. **Build stamp → `"XLVI"`.**

**Round XLVII shipped (runs 28/28b analysis + F10 + the stage flip —
knlog-layer fix only, no wire-format change):**

- **STAGE 2 CLOSED** (RTSP §35.5, BOOTSTRAP §12 marker): two perfect
  full-length keepers under both jiggle settings; G2's criteria met
  and exceeded at session scale (169 036/169 036 datagrams
  authenticated in one draw; 10 800/10 800 AUs decoded; reorder/NACK/
  FEC proven against natural loss at up to 90 repairs/draw; the
  estimator soaked six minutes warm; D8 never needed in a keeper).
- **The corrected jiggle verdict** (RTSP §35.3, INPUT.md §9): the
  XLVI "depressant" conviction is unsound — 28b's ON keeper permits a
  perfect six-minute draw WITH 3533 synthetic reports flying. The
  run-27 A/B stays *suggestive* but era-confounded (the same evening
  went band → tails → keepers for BOTH settings). Default stays OFF;
  the handshake stays untouched; the interleaved-A/B rule becomes
  trap 17.
- **F10**: `kn_log_on_failure("session.media", ...)` now selects the
  code from the stop_reason — `E_SESSION_NO_MEDIA` /
  `E_SESSION_RECOVERY_EXHAUSTED` (the dominant dead-draw class) /
  `E_SESSION_SERVER_TERMINATION` / `E_SESSION_SOCKET_ERROR` /
  `E_SESSION_NO_FRAMES` (its original meaning: datagrams but zero
  AUs) / `E_SESSION_FAILED_VERDICT`. Stdout untouched; knlog/diag
  only. Run 28's mislabel (NO_FRAMES next to 6049 perfect decodes)
  was the field evidence.
- **Evidence**: no new stdout shapes; validator unchanged at **112**
  blocks, summary **74 = 74**. **Build stamp → `"XLVII"`.**
- **Stage 3 opens** with, in order: (1) the audio bundle (extraction
  first — DTLS-SRTP keying already negotiated, `srtp_prof:1`; Opus
  decode second), (2) G3 remainder behind the INPUT.md §9 caveat,
  (3) G4 cursor payloads, (4) G1 handshake timeout, (5) D-t13 decode
  watchdog.

**Round XLVIII shipped (F11 — the bundle-audio extraction stage;
runs 29/29g analysis + two new modules + the DTLS-SRTP exporter;
full contract `docs/BUNDLE-AUDIO.md`, delta `docs/INTEGRATION.md`
rev 39):**

- **`kn_arx`** (`streamer/src/kn_arx.{c,h}`): the bundle-audio receive
  pipeline — SRTP unprotect (AES_CM_128_HMAC_SHA1_80, RFC 3711 KDF via
  `kn_srtp_kdf128`) → replay → seq accounting → padding → RFC 2198 RED
  → RFC 6716 TOC → Opus payload emit. The **14-step order is pinned by
  `tools/arx_reference.py`** (ordering IS semantics — rev 39 trap 3).
  17 counters + first/last_ms; PT 111 Opus / PT 63 RED (nvst.rs:275-276;
  audioTrack hardcoded nvst_rtsp.rs:1309, 48 kHz stereo, SSRC any).
- **`kn_ogg`** (`streamer/src/kn_ogg.{c,h}`): zero-alloc OggOpus muxer
  (RFC 3533/7845) — OpusHead family-0 preskip 0 (D-o3) + OpusTags
  "KarnyNow", granulepos = Σ TOC samples, flush 255 segs/16384 B/48
  pkts (D-o1), no continuation (D-o2), serial 0x4B4E4F47, CRC table
  GENERATED + Python-verified (never hand-type tables — rev 39 trap 1).
- **`kn_dtls`**: mbedTLS export-keys callback +
  **`kn_dtls_srtp_export(out60)`** = RFC 5764 §4.2
  `EXTRACTOR-dtls_srtp` (client_random‖server_random seed, SHA-256
  PRF); RX = the SERVER half (out+16 key, out+46 salt); stub/schannel
  honest −1; keys never logged, stack-scrubbed after arming.
- **knvstd wiring**: `--session-audio-out=F.opus`; `audio_arm` event at
  `dtls_ready` (`srtp_derived/armed/profile/pt/red_pt/channels/
  clock_hz/file`); demux feeds every bundle RTP datagram to
  `kn_arx_process` (pump mirror → snapshot-safe stats); `audio_close`
  at teardown (EOS flush; skipped under `pump_persist`); the **34-key
  summary `"audio"` block** after `"recovery"`. Video is NEVER touched
  by audio-path failures.
- **Two-implementations rule**: `tools/oggopus_reference.py` +
  `tools/arx_reference.py` (both self-prove before emitting) →
  `kn_ogg_kat_vectors.h` (5 byte-exact scripts) +
  `kn_arx_kat_vectors.h` (12 groups/21 packets incl. seq_wrap ROC 1,
  RED tiling, replay-before-accounting) → **`audio.vectors`** (188
  checks, gated, registry **16**) + `test_knogg` (**163**) +
  `test_knarx` (**62**).
- **New validator**: `tools/check_summary_skeleton.py` — diffs the
  summary printf skeleton's recursive KEY SET against a live summary
  (drops/outbox_by_sid emptied). Canon: **113 keys** (79 through XLVII
  + 34 audio). Retires the old block-scan validator and its
  108-vs-112 mystery.
- **App-id canon (operator-corrected)**: **107057914 = Arknights:
  Endfield** (it ALWAYS was — every "Arknights launcher-parked" note
  in runs 22–29 means Endfield); **100361011 = Watch Dogs: Legion**
  (the most dynamic title in the operator's mostly-F2P GFN library).

**Round L shipped (run-31 analysis + F16–F19; three new files, and
`knvstd_main.c` for the wiring):**

- **F16 — `kn_drawpath.{h,c}` + `test_kndrawpath` (the 21st suite, 1051
  checks).** F13's per-draw naming rule, extracted from `knvstd_main.c`
  and made alias-free: the XLIX version took `dot = strrchr(dst, '.')` —
  a pointer INTO the destination — and then `memcpy`'d the extension
  forward, an overlapping copy (UB; ASan `memcpy-param-overlap` on the
  rig) that produced run 31's `audio_d1_d1us` (extension destroyed). The
  rule now copies
  stem/suffix/extension through LOCAL buffers and uses `memmove`, so the
  in-place idiom the caller actually uses is defined behaviour. Pinned
  three ways: the run-31 strings verbatim, a **second independent
  builder** in the test compared across a 14-path × 9-draw matrix
  (Windows and POSIX separators, dot-in-directory, no-extension,
  multi-dot), and honest degradation (no room ⇒ keep the ORIGINAL path +
  set `truncated`, never a half-written name; the caller logs
  `audio_path_suffix`/`video_path_suffix` if it ever fires). ASan/UBSan
  clean at -O1 and -O2.
- **F17 — the `.264` question ANSWERED, with evidence, then fixed.** The
  operator's repeated "only the first `.264` works" is a real observation
  and NOT corruption: a single-AU dump has no SPS/PPS of its own, so no
  decoder can start on it. Proof from the SHIPPING binary on the host
  (`logs/f17_video_proof_L.txt`, reproducible in three commands):
  `au_00001_k.264` → ffprobe **1 frame**, ffmpeg clean (it is the
  keyframe and carries the parameter sets); `au_00002_p.264` and
  `au_00030_p.264` → **"Invalid data found when processing input"**;
  the new continuous file → **60/60 frames, clean decode**. Ships
  (a) `--session-video-out=F.264` — ONE file per draw, every decoded AU
  appended in order ⇒ `video_d<N>.264`, a normal Annex-B elementary
  stream, with `video_stream_begin`/`video_stream_end` event lines
  (aus/bytes/file_bytes/write_errors, the size read back with `ftell` so
  the line states what is ON DISK); (b) the same writer in replay mode
  (`--replay-video-out=F.264`) so the mechanism is provable with no rig,
  no account and no queue; (c) **draw-numbered single-AU names**
  (`au_d<N>_%05u_%c.264`) — run 31 flew four draws into ONE
  `--session-au-dir`, so draw 2's `au_00001_k.264` had silently replaced
  draw 1's. One writer (`knv_dump_au`) serves both dump legs and both
  modes, so they cannot drift.
- **F18 — `--session-release` (default OFF; `--no-session-release` says
  so explicitly).** Run 31's orphan anatomy: the teardown's TEARDOWN was
  fire-and-forget, unlogged, and never reached the wire on a dead
  channel — so the cloud kept the seat and the next create hit **403
  SESSION_LIMIT** and burned the 20/40/80 s backoff ladder. Now (a) the
  TEARDOWN is ALWAYS logged with its real rc/status (`rtsp_teardown`,
  plus a WARN note when it did not complete) and (b) with the flag it is
  followed by the CloudMatch **DELETE v2/session/{id}** — the reference's
  `stop_at` (:789-800) — logged as `cm_release` with `gone` = 2xx-or-404
  (`validate_delete_response`, :2728). Also fixed a latent crash on the
  way: a new `rtsp_inited` latch guards the call, because an early
  failure path used to reach `kn_rtsp_request` with a client phase B had
  never initialised. The e2e grew a release leg (the fake rig now COUNTS
  deletes; the harness asserts `kn_cm_cancel` → 204 and `deletes >= 1`).
- **F19 — REAL INPUT, both modes the operator chose.** Wire layer first:
  five discrete builders in `kn_input.c` (KEY_DOWN/UP 3/4,
  MOUSE_ABSOLUTE 5, MOUSE_RELATIVE 7, BUTTON_DOWN/UP 8/9, WHEEL 10)
  **byte-exact against the reference's own Rust goldens**
  (`nvst_input.rs:1868-1911`) — `test_kninput` grew 548 → **794 checks**
  over 21 regenerated discrete vectors, and `tools/input_reference.py`
  self-proves (8/8 activation + 2/2 Rust goldens) before emitting
  `kn_input_kat_vectors.h`. On top of it, in `knvstd_main.c`:
  - `--input-script=FILE` — a timeline replayed against the media-loop
    clock. t=0 is the draw's FIRST MEDIA TICK, not process start, so one
    file replays identically on every draw however long the queue took.
    Commands: `key <vk|name|char> <down|up> [mods]`, `type <text>`
    (expanded to down/up pairs through the US VK table, spacing
    `--input-text-gap=MS`, default 30), `mouse <x> <y> <w> <h>`
    (absolute **PIXELS** in a viewport — NOT normalized 0..65535),
    `mrel <dx> <dy>`, `btn <1..5|left|middle|right|x1|x2> <down|up>`,
    `wheel <dx> <dy>` (a notch is ±120). A single character carries its
    IMPLIED modifiers (`key A down` = shift+0x41, not a lowercase "a");
    an explicit argument replaces them.
  - `--input-live` — keystrokes typed into the knvstd console are
    forwarded as they arrive (Win32 `_kbhit`/`_getch`; POSIX termios raw
    mode, restored on EVERY exit path including Ctrl-C). Arrows, Tab,
    Enter, Backspace, ESC and the extended-key prefix are mapped;
    unmappable characters are counted, never silently retyped. Reports
    `armed:0 reason:no_console` honestly when stdin is redirected (the
    operator flies with `> logs\run.log`, so this matters).
  - `--input-anti-afk[=MS]` — the reference's own keep-awake pulse:
    VK 0x7C (F13) down+up, no modifiers (`anti_afk_pulse`,
    opennow-streamer-core:1604-1648); bare flag = 60 000 ms. F13 is
    bound to nothing in any layout, which is why the reference chose it.
  - All three share ONE sender and ONE gate: nothing flies before the
    activation chain latches `enable-input(true)` (`pump.input_active`),
    because input ahead of the chain is the one thing the reference never
    does. Routing is `kn_input_route_sid` (keys/buttons/wheel → sid 0
    ControlReliable ordered; mouse motion → sid 6 ControlPartial,
    unordered, 300 ms PR-SCTP lifetime) and timestamps ride
    `pump.origin_us`, the chain's own epoch. Gated events are COUNTED
    (`gated`) and retried, never dropped; events more than 5 s late are
    skipped and counted (`late_skips`) so a recovery cannot dump a whole
    timeline at once. Evidence: `input_script_loaded`, `input_live`,
    `input_first_send`, `input_anti_afk`, and a per-draw `input_summary`
    that goes WARN when input was armed but nothing flew — the run-31
    shape, now impossible to miss.
  - `--input-check=FILE` — parse and dump a script WITHOUT a session
    (exit 2 + the line number + the reason on any error). Two permanent
    ctest legs pin the parser: `knvstd_input_check` on the 137-event
    login-shaped fixture (`streamer/tests/fixtures/input_login_example.txt`,
    every command form + the implied-modifier rule) and
    `knvstd_input_check_rejects` (WILL_FAIL) on a bad one.
  - **Why this is the login-wall answer:** the reference client contains
    no credential handling at all (sparse-clone grep: nothing), the GFN
    browser auto-logs-in by typing into the stream, so the ONLY way past
    Ubisoft Connect is to type. The script lives on the operator's rig —
    no credential ever enters the repo, a log line or a delivery.

**Sandbox gates at L close (all green, re-run after the last edit):**
`build_native_tests.sh` **NATIVE TESTS PASS**, zero warnings (new suite
`test_kndrawpath` **1051 checks**; `test_kninput` **794**, was 548; every
XLIX baseline unchanged: knogg 163, knarx 62, knselftest 118) ·
`cross_check.sh` PASS ×2 arches, `-Werror` clean (`logs/cross_L.log`;
added the `kn_drawpath.c` per-file leg, the `test_kndrawpath` link and the
KCORE entry, so `test_kndrawpath_{x86_64,i686}.exe` are built) ·
cmake+ninja+ctest **25/25 on BOTH backends** (was 22/22: +`test_kndrawpath`,
+`knvstd_input_check`, +`knvstd_input_check_rejects`; `logs/ctest_mb_L.log`,
`logs/ctest_sch_L.log`) · T10 POSIX dry **ALL PASS 74** (was 72; target
expectation **84**) — round L adds no production self-test, but it DOES
add a suite, and cmake building a suite is not the same as T10 running
it: `test_kndrawpath` is now in T10's own `suites` list AND in
`required_bins`, so the target executes the F16 rule (+2 checks:
`test_kndrawpath exists in build`, `test_kndrawpath -> PASS`,
`logs/t10_L2.log`) ·
T13 **ALL PASS 63** against `build/net-mb/knvstd` and **ALL PASS 51**
against the bare binary (the one by-design gcm256 SKIP) ·
`sandbox_session_e2e` **48/48** from the repo root (was 43; +5 for the F18
release leg, `deletes:1` in the rig's done line) · summary skeleton
**113 == 113** against run 31's real stdout — F16–F19 add NO summary keys
(deliberate: per-draw evidence lives on the `input_summary`,
`video_stream_end`, `rtsp_teardown` and `cm_release` event lines, so the
canon and `tools/check_summary_skeleton.py` stay untouched) · ASan/UBSan
green on `test_kninput` (794), `test_kndrawpath` (1051) AND a fully
sanitized `knvstd` running `--input-check` on both fixtures plus the F17
replay dumps (60 AUs, 0 write errors, ffprobe 60 frames from the
sanitized binary's output). The only UBSan note in that run is vendored —
`third_party/crypto-algorithms-sha1/sha1.c:26` left shift — pre-existing,
not our code, not gated. · knvstd smoke: stamp **L**, and the usage text
documents all six new flags (`--session-video-out`, `--session-release`,
`--input-script`, `--input-live`, `--input-anti-afk[=MS]`,
`--input-text-gap`, `--input-check`, `--replay-video-out`).

**Round XLIX shipped (run-30 analysis + F12–F15; `knvstd_main.c` is the
ONLY code file touched — no wire-format, interface or KAT change, so
every test count is unchanged):**

- **THE OPERATOR'S CORRECTION (binding): there is no "~600 s server
  ceiling".** The pool is **Warsaw**
  (`prod.cloudmatchbeta.nvidiagrid.net` → `np-waw-01…`; rigs
  `80-250-98-39` / `77-111-250-40` in run 30 — "beta" is the pool's own
  naming, not a beta tier), the account is **GeForce NOW Free**, the
  browser client at play.geforcenow.com gets a **one-hour** maximum
  session and **has never been cut at 10 minutes** — sessions always run
  the hour. That matches run 25's full `SESSION_LENGTH_TIMER`
  (`beforeEventMS:3599657` ≈ 3 599.7 s). §36's conclusion is therefore
  withdrawn: the ≈594–599 s endings of runs 29/29g are real but they are
  **not** a pool/tier/session-length limit — they are something our
  sessions do. **Prime suspect: a server-side idle/no-input policy** —
  every ≈600 s draw was parked on a launcher/menu with **zero input
  flying** (jiggle OFF is the default since XLVI/F9), and run 30 adds
  the mechanism's plausibility (both draws frozen while static and
  input-less, control plane alive). The reference has the vocabulary for
  it (`0x104`/`0x105` → `NvstSessionTimer`, a `session_resumable()`
  table keyed on the termination reason) but our runs show
  `timers:0 idle_warning:0` — the rig never warns, it just cuts.
- **F12 — the audio-file close was DEAD CODE on every live session.**
  The XLVIII teardown block was gated `if (pump.audio_file_on &&
  !pump_persist)` and `pump_persist` is 1 for every live session, so run
  30 shows `audio_arm` ×2 and **`audio_close` ×0**: no EOS page, no
  `fclose`, and draw 1's `FILE*` **leaked open across the lottery gap**
  (draw 2's `memset(&pump,0,…)` dropped it) — the delivered file is
  99.88 % valid but carries a **2 505-byte scrambled region at
  815 462…817 967 = exactly one page's span at the draw1→draw2
  boundary**, with draw1's last-page bytes (CRC `9b da 06 b2`, serial
  `GONK`, seq 327) legible inside it. Fix: the idempotent
  `sess_audio_file_close(&pump, &ice_st)` (EOS + `fclose` + totals
  mirror + the `audio_close` lines) called **immediately after
  `sess_pump_stop()`** on both live teardown paths (D21: the pump is the
  single writer — once joined, main owns the FILE*) plus one defensive
  call at the `teardown:` label. `audio_close` now fires once per draw.
- **F13 — one audio file per draw**: `run_session(…, draw_index, …)`
  derives `<stem>_d<N><ext>` when `draw_index > 0`, so `--lottery=4`
  delivers `audio_d1.opus … audio_d4.opus` (each self-contained:
  OpusHead/OpusTags + BOS + EOS). The XLVIII rule "audio.opus always
  holds the LAST draw's audio" is gone.
- **F14 — the interleaved jiggle A/B** (trap 17 made runnable): with
  `--lottery=N>1` the jiggle **alternates per draw** — odd draws fly
  `--session-jiggle=MS`, even draws are OFF — and each draw prints
  `lottery_arm {"draw":N,"jiggle_ms":M}`. `--lottery=1` stays
  byte-identical to XLVIII.
- **F15 — `--lottery-stop-on-keeper=0|1`** (default 1 = the XLIII rule):
  an interleaved A/B is worthless if its first surviving draw ends it, so
  run 31 passes 0.
- **Evidence**: one new stdout JSONL shape (`lottery_arm`), one revived
  (`audio_close`, now per draw); the summary key set is **unchanged
  (113 canon)** — F12 only moves *when* the ogg counters are mirrored.
  **Build stamp → `XLIX`.**

**Sandbox gates at XLIX close (all green, re-run after the last edit):**
`build_native_tests.sh` **NATIVE TESTS PASS**, zero warnings (knogg 163,
knarx 62, knselftest 118, kninput 548 — no new suite/check, so the
XLVIII baselines are the canon) · `cross_check.sh` PASS ×2 arches,
`-Werror` clean, **0 SKIPs** with all four vendor trees staged
(`logs/cross_xlix.log`) · cmake+ninja+ctest **22/22 on BOTH backends**
(`build/net-mb` mbedtls, `build/net-sch` schannel; `logs/cmake_mb.log`,
`logs/cmake_sch.log`). **cmake builds `knvstd` in both trees** — the
round-XLIX stamp is verified in all three sandbox binaries — but
*without FFmpeg* (the owner's `build/ffmpeg/` dev tree is gitignored, so
`kn_decode` is the honest stub and `decode.synthetic` reports skipped;
that is `CMakeLists.txt` :146-193's designed no-FFmpeg mode, not a
failure) · T10 POSIX dry **ALL PASS 72** (target expectation **82**;
zero compiler warnings, bundle 6 767 B, `build.json` extras
`ffmpeg=disabled,tls=mbedtls`) · T13 **ALL PASS 63** against the
mbedTLS cmake binary (`--knvstd build/net-mb/knvstd`, no wire change);
against the bare `build/knvstd` it is ALL PASS **51** with ONE by-design
SKIP (the gcm256 leg — bare builds are cm128-only) ·
`sandbox_session_e2e` rebuilt from XLIX sources with `-Wall -Wextra`
(zero warnings) → **43 checks, 0 failures** from the repo ROOT ·
summary skeleton **113 keys** and `check_summary_skeleton.py` run
against run 30's real stdout → **PASS, 113 == 113, no key added or
removed by F12–F15** · knvstd smoke: `{"mode":"build",…,"round":"XLIX"}`
and the usage text documents all four new behaviours (per-draw audio
file, the alternating arm, `lottery_arm`, `--lottery-stop-on-keeper`).

**Sandbox gates at XLVIII close (all green):** `build_native_tests.sh`
**NATIVE TESTS PASS**, zero warnings (new baselines: knogg 163, knarx
62, knselftest 118; knrx 218 / kninput 548 unchanged) ·
`cross_check.sh` PASS ×2 arches, zero warnings under `-Werror` (kn_ogg/
kn_arx per-file legs + KCORE links added) · ctest **22/22 ×2 backends**
(mbedtls + schannel) · T10 POSIX dry **ALL PASS 72** · T13 **ALL PASS
63** (no wire change) · summary skeleton **113 keys**, live-diff = only
the audio block (validator proven against run 29's log) ·
`sandbox_session_e2e` **43/43** (repo ROOT) · ASan/UBSan legs green on
the new suites (ASan caught the KAT pointer-array emission bug before
first link) · knvstd smoke: stamp XLVIII, `--help` shows the flag,
early-abort rc contract unchanged.

**Codebase shape:** 31 `streamer/src/*.c` + 29 `streamer/include/kn/*.h`
(round L added `kn_drawpath.{h,c}`); the production binary is `knvstd`
(`knvstd_main.c`, 8495 lines: modes `--capture`, `--decode`,
`--replay-capture`, `--session`, `--input-check`, `--selftest`,
`--list-selftests`); 16 registered production self-tests; 21 native test
suites. Third-party (vendored, gitignored, re-stage if cleaned):
tiny-AES-c, crypto-algorithms-sha1, usrsctp, mbedTLS 3.6.0 (+framework).

## 4. What to do next (in order)

> The operator asked for **very simple, step-by-step** instructions —
> this section is written to be followed literally, one step at a time.
> (Agent side: run 31's analysis + the F16–F19 contracts live in
> `docs/RTSP.md` §38, `docs/INPUT.md` §9 and `docs/INTEGRATION.md`
> rev 41; round state is §3 above.)

**The one-sentence plan for run 32:** the rig is parked at the Ubisoft
login prompt because our client never typed anything — so this run
**types**. You write a small text file with your login on the rig (it
never leaves the rig), the client replays it into the stream, and the
video file tells us whether the game started.

**Step 1 — sync these files** (the round-L delta):

- `streamer/src/kn_drawpath.c`    (**NEW** — F16, the per-draw naming rule)
- `streamer/include/kn/kn_drawpath.h` (**NEW**)
- `streamer/tests/test_kndrawpath.c`  (**NEW** — the 21st suite, 1051 checks)
- `streamer/tests/fixtures/input_login_example.txt` (**NEW** — the script
                                  format, and a ctest fixture)
- `streamer/tests/fixtures/input_bad.txt` (**NEW** — the rejection fixture)
- `streamer/src/knvstd_main.c`    (F16 use + F17 video stream + F18
                                  release + F19 input driver + stamp L)
- `streamer/src/kn_input.c`       (F19 wire: the five discrete builders)
- `streamer/include/kn/kn_input.h`
- `streamer/src/kn_input_kat_vectors.h` (regenerated: +21 discrete vectors)
- `streamer/tests/test_kninput.c` (548 → **794** checks)
- `tools/input_reference.py`      (the self-proving reference emitter)
- `tools/fake_rig.py`             (counts DELETEs — the F18 e2e leg)
- `tools/sandbox_session_e2e.c`   (43 → **48** checks)
- `tools/build_native_tests.sh`, `tools/cross_check.sh`, `CMakeLists.txt`
                                  (the new suite + the two new ctest legs)
- `tests/T10_build_and_test.py`   (`test_kndrawpath` added to the suites
                                  list AND `required_bins` — otherwise the
                                  target would build it and never run it)
- `START-HERE.md`, `docs/RTSP.md` (§38), `docs/INTEGRATION.md` (rev 41),
  `docs/INPUT.md` (§10), `docs/BUNDLE-AUDIO.md`, `docs/BOOTSTRAP.md` (§12)

**Step 2 — clean rebuild + T10 on Win7** (the usual: delete the build
tree, build, run T10 in full). Expected verdict: **ALL PASS 84** (was 82:
+1 `test_kndrawpath exists in build`, +1 `test_kndrawpath -> PASS`; round L
adds no production self-test). The suite counts DO
move: `test_kninput` **794** (was 548) and the new `test_kndrawpath`
**1051**; everything else stays at its XLIX baseline (`test_knogg` 163,
`test_knarx` 62, `test_knselftest` 118, `test_knrx` 243, `audio.vectors`
188/0). ctest goes **22/22 → 25/25** on both TLS backends. If T10
reports anything else, re-sync and redo.

**Step 3 — check the build stamp**: the first line of the run log must
be `{"mode":"build","compiled":"...","round":"L"}`. If it says `XLIX`,
the rebuild did not take — redo Step 2.

**Step 4 — write the login script ON THE RIG (2 minutes).** Create
`login.txt` next to `knvstd.exe` with Notepad, exactly this (then replace
ONLY the two `type` lines with your real Ubisoft e-mail and password):

```
# run 32 - the Ubisoft Connect login. t_ms counts from the draw's first
# media tick, so these times are "seconds after the stream is up".
# attempt 1
25000 key tab down
25050 key tab up
26000 type YOUR_UBISOFT_EMAIL@example.com
28000 key enter down
28050 key enter up
31000 type YOUR_UBISOFT_PASSWORD
33000 key enter down
33050 key enter up
# attempt 2 (in case the launcher was still booting at 25 s)
95000 key tab down
95050 key tab up
96000 type YOUR_UBISOFT_EMAIL@example.com
98000 key enter down
98050 key enter up
101000 type YOUR_UBISOFT_PASSWORD
103000 key enter down
103050 key enter up
```

Rules that matter: one event per line; `#` is a comment; times are
milliseconds; **keep this file on the rig** — never paste it into the
logs repo, a chat or an issue (it is your password).

**Step 5 — validate it (10 seconds, no session, no queue):**

```
knvstd.exe --input-check=login.txt
```

Expected: a stream of `{"mode":"input_check","event":"ev",...}` lines and
then a summary ending with `"unmapped_chars":0`. **If `unmapped_chars` is
NOT 0, STOP and tell me** — it means your password contains a character
the US key table cannot send (Polish diacritics `ą ć ę ł ń ó ś ź ż` are
the usual suspects). The login would silently mistype, and the fix (a
layout/scancode path) is a round of its own. Exit code 2 with a line
number means a syntax error — fix that line and re-run.

**Step 6 — run 32.** ONE draw, 7 minutes, video + audio + release all on
(replace `<TOKEN>`):

```
knvstd.exe --session --session-token=<TOKEN> --session-app=100361011 --session-seconds=420 --input-script=login.txt --session-video-out=video.264 --session-audio-out=audio.opus --session-release > logs\run32.log
```

What happens, in order: the draw comes up, the activation chain flies,
`input_script_loaded` reports the event count, and at t+25 s the client
starts typing into the stream. At the end you get THREE new lines —
`input_summary` (how many messages actually flew), `video_stream_end`
(the size of `video_d0.264` on disk) and `rtsp_teardown` + `cm_release`
(F18: the seat is given back, so the NEXT run does not eat a 403).
Because `--lottery` is not given, this is draw 0 and the files keep their
plain names (`video.264`, `audio.opus`).

**Step 7 — watch it (this is the whole point).** `video.264` is ONE
continuous file that plays (that is F17; the old per-AU files could not):

```
mpv video.264        (or VLC: drag the file in)
```

Look for: the Ubisoft login prompt → your e-mail appearing → the password
field → **the launcher closing and the game booting**. Note the timestamp
where the game starts (or write down what blocked it — a CAPTCHA, a
two-factor prompt, a "wrong password" message, a different window). If
you hear nothing, `mpv audio.opus` should now have content if the game
launched.

**Step 8 — send back (via the karnynow-logs repo, as usual):** clean the
repo, push as `run32_logs/`:

- the whole `logs\` directory INCLUDING `run32.log`;
- the fresh `T10_skrot.txt` + `T10_wynik.txt`;
- **`video.264`** — a launcher/login scene is low-bitrate, so ~10–20 MB
  is normal. `.264` is gitignored, so force it:
  `git add -f run32_logs/video.264`;
- `audio.opus` (only if it is not silent — Step 7 tells you);
- **NOT `login.txt`** — never. If I need to see the timing, send the
  `--input-check` output from Step 5 instead (it has no credentials, only
  VK codes).

Plus, in your message: what you SAW in Step 7 and at what second.

**Read matrix for run 32 (agent side):**

| observation | reading |
|---|---|
| video shows the game booting after the typed login | **THE LOGIN WALL IS DOWN** — F19 is proven live; every later round can start a game on demand (and the audio-silence question closes with it) |
| `input_summary` shows `sent` > 0 but the video still shows the login prompt | the messages flew but the rig ignored them ⇒ next: the window-state member / cursor-capture path, or the coordinates (add a `mouse` click on the field — F19 already supports absolute pixels) |
| `input_summary` shows `sent:0` and `gated` > 0 | the activation chain never latched `enable-input(true)` on that draw ⇒ the gate held; correlate with the `ctrl_in` lines |
| `input_summary` shows `sent:0` and `gated:0` | TWO shapes, split them by the video: if `video_stream_end.aus` > 0 the media loop DID tick and the draw simply died before the first event's `t_ms` (**run 32 — a scheduling miss: move the timeline left**); if `aus` is 0 too, then nothing ever ran and it IS a wiring bug, my side |
| a CAPTCHA / 2FA prompt in the video | typing works but Ubisoft gates it ⇒ the browser-comparison route (the operator's DevTools capture) becomes necessary after all |

**RUN 32 RESULT (read 2026-10-29, `karnynow-logs/run32_logs/`):** the
fourth row above is what happened — `aus:668`, `fired:0`, `gated:0`: the
draw lived 29 s of media and the macro's first event sat past that. Full
numbers, the freeze anatomy (steady to t≈28 s → total media silence at
t≈29 s → `progress_stall` 32.9 s → two 5 s `rx_recovery` timeouts →
terminal), the F17/F18 proofs and the Endfield process chain:
`docs/RTSP.md` §39. **Run 33 = the same run with the timeline moved
left**: first click/type at **t≈6–12 s** (the Endfield `Log In` button is
on screen from frame 1, measured ≈(984, 44) in 1280×720 pixels; `Play`
≈(1104, 632)), retries at ~30/60/90 s, plus `--input-anti-afk=60000`
(the reference's own pulse — it also probes the idle-policy hypothesis
while the login attempt runs), same app, same `--session-release`.
Everything else in Steps 1–8 stands; the only line that changes is the
first timestamp in your macro.
| `cm_release` shows `gone:true` and the next create is instant (no 403 ladder) | **F18 proven on target** — the orphan-seat era is over |
| `video_stream_end` `file_bytes` > 0 and the file plays in mpv | **F17 proven on target** |
| the audio path in `audio_arm` reads `audio.opus` (draw 0) or `audio_d1.opus` (a lottery) — never `_d1_d1us` | **F16 proven on target** |

**Then (agent side, next rounds):** in order — (1) whatever run 32's
video says: a mouse-click leg (absolute pixels on the e-mail field) if
Tab/Enter focus is wrong, or the layout/scancode path if
`unmapped_chars` blocked it, or the DevTools browser comparison if a
CAPTCHA gates it; (2) the ≈600 s question, still unanswered — now
answerable with `--input-anti-afk=60000` on a long draw (the reference's
own pulse, and the honest test of the idle-policy theory: real input,
not synthetic motion); (3) Opus **decode** (PCM/WAV) + playback via
`kn_audio` — worth a round the moment a capture contains sound;
(4) G4 cursor payloads, G1 input-handshake timeout, D-t13 decode
watchdog; (5) fidelity hygiene: parse the handoff's `audioTrack`
(pt/clock/channels/ssrc/mid) instead of hardcoding PT 111 / 48 kHz /
stereo (`nvst.rs:1654 parse_audio_track`).


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

## 8. Traps that will bite you (curated; full list in docs/INTEGRATION.md rev 40 §traps)

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
13. **CloudMatch's door has TWO waitables, not one** (run 26): a bare
    HTTP 403 = seat cooldown (can exceed 90 s; run24v2 proved the
    token stays valid) and HTTP **429** = create-rate limiter (draws
    3/5 died to it under the old 403-only F4). F7 (XLV) retries both —
    plus the rest of the transient class — on an escalating
    20/40/80/160 s schedule, logging `cm_create_backoff` with a
    `"status"` field (the XLIII/XLIV `cm_create_403_backoff` name is
    GONE — update any parsers). The JSON-body sibling
    `cloud_status 11 = SESSION_LIMIT` remains deliberately NOT
    retried.
14. **The live-session exit code only became honest in XLIV (F6a)**:
    pre-XLIV builds exited **0 even for `recovery_exhausted`** (any
    media at all ⇒ 0) — which is why run 25's lottery stopped at
    draw 1. Since XLIV, exit 0 MEANS "the draw survived its full
    `--session-seconds` (or met `--session-expect-frames`)"; every
    terminal stop_reason exits 1. Never build automation on pre-XLIV
    exit-code folklore.
15. **Lottery knlog files APPEND across draws** (run 26 field
    correction: `transport.log` held both sessions' stats; the
    rev-34 "overwritten per draw" claim was wrong) and diag rotates
    at ~2 MB (`diag.1.log`). A lottery run's `logs/` directory is
    whole-run evidence — always request it in full.
16. **THE JIGGLE DEFAULT IS OFF (XLVI/F9, verdict DOWNGRADED in
    XLVII)** — every procedure, doc or memory from XLII–XLV that says
    "the jiggle stays ON as a controlled constant" or expects an
    `input_jiggle` line is STALE. The XLVI "depressant" CONVICTION
    did not survive run 28b's ON keeper (a perfect 360 s WITH 3533
    jiggle reports): the run-27 A/B is *suggestive* but
    era-confounded — the whole evening drifted favourable for both
    settings. Default stays OFF (not necessary, not invariably fatal,
    asymmetric bot-signature risk). OFF proof = no `input_jiggle`
    line + `"jiggle":0`; `--session-jiggle=100` is the legacy ON. The
    activation handshake is NOT implicated — never disable it
    together with the jiggle.
17. **Cross-evening A/Bs are UNREADABLE (era drift)**: the same
    client + app went from 5/5 band draws (≤13.2 s) to 2/2 full
    keepers within ~4 hours on Oct 28. Never compare a run against a
    previous evening's distribution; any input/policy A/B must
    interleave its arms inside one run (or at minimum one evening).
18. **F10 failure-label taxonomy (knlog/diag only)**:
    `E_SESSION_NO_FRAMES` now ONLY means "datagrams but zero AUs";
    the common dead-draw case is `E_SESSION_RECOVERY_EXHAUSTED`.
    Pre-XLVII logs carry the misleading old labels — read run ≤ 28
    app.log failures with that in mind.
19. **THERE IS NO "~600 s SERVER CEILING" (XLIX CORRECTION — the old
    trap 19 is WITHDRAWN).** The operator's first-hand control: the pool
    is **Warsaw** (`prod.cloudmatchbeta.nvidiagrid.net` →
    `np-waw-01.cloudmatchbeta…`; the "beta" in those host names is the
    pool's own naming, NOT a beta tier), the account is **GeForce NOW
    Free**, the browser client gets a **one-hour** maximum session and
    **has never been cut at 10 minutes**. That agrees with run 25's full
    `SESSION_LENGTH_TIMER` (`beforeEventMS:3599657` ≈ 1 h). What stands
    from runs 29/29g is the *observation*: three draws ended at 594 s
    (silent datagram decay 444 → 0 over ~9 s) and 599.1/599.2 s (polite
    `0x0109` + TERM `0x80008BE3` code 56 / `0x80008BC7`). What falls is
    the *explanation*: it is not a pool/tier/session-length limit — the
    prime suspect is a **server-side idle/no-input policy** (every ≈600 s
    draw was launcher-parked with zero input flying; run 30 froze two
    more static, input-less draws). Run 31 is the interleaved A/B that
    measures it (F14). Two discriminators that DO still hold:
    **gradual decay = a policy/ceiling-style cut; ABRUPT silence with
    the SCTP control plane still heartbeating = rig freeze**; and a soak
    that wants `stop_reason:"time"` should still use ≈570 s (safely
    below every observed cut) — 900 s+ draws are experiments, not soaks.
20. **F11 audio-stage traps** (full set: INTEGRATION rev 40 §traps):
    never hand-type const tables (generate + verify — the first kn_ogg
    CRC table was 160/256 entries, wrong from index 24); RFC 6716 TOC
    0xF8 = CELT-FB 20 ms = **960** samples (0xE0 is FB **2.5 ms** = 120;
    the GFN rig itself sends 0xEC = cfg 29 = CELT-FB **5 ms** = 240 samples,
    stereo, ~200 pkt/s — measured runs 30–32, BUNDLE-AUDIO §10.1;
    0xC0 = SWB 2.5 ms = 120); the kn_arx 14-step ORDER is semantics
    (replay BEFORE seq accounting; SSRC reset BEFORE the guess;
    padded++ on the P bit even if the pad is invalid; seq accounting
    right after the SRTP commit; header-walk overflow = malformed, not
    srtp_fail); OggOpus body starts at page offset **28** (27 + 1
    lacing byte); native test link lines must carry
    `third_party/tiny-aes-c/aes.c` +
    `third_party/crypto-algorithms-sha1/sha1.c` (kn_aes/kn_sha1 are
    wrappers).
21. **The sandbox toolchain is NOT persistent across agent sessions**:
    a fresh session starts with no gcc/cmake/make/mingw — reinstall
    first (`apt-get update && apt-get install -y gcc cmake make
    ninja-build libc6-dev` — **`libc6-dev` is mandatory**, without it
    every compile dies on `stdint.h: No such file or directory`; then
    `gcc-mingw-w64-x86-64 gcc-mingw-w64-i686` for the cross legs, ~10
    min). Vendor staging: tiny-AES-c + crypto-algorithms come from the
    raw URLs pinned in `tests/T10_build_and_test.py:VENDOR_FILES`
    (SHA-256-verified); mbedTLS = `git clone --branch v3.6.0` +
    `git submodule update --init --recursive`; usrsctp =
    **`github.com/sctplab/usrsctp`** (the `sctp/usrsctp` form no longer
    resolves) at `c625e219`. A full OpenNOW reference clone (314 MB)
    VANISHES between tool calls — the snapshot caps at ~128 MB; clone it
    to `/tmp` with `--depth 1 --filter=blob:none --sparse` (13 MB).
22. **`pump_persist` is 1 for EVERY live session** (`pump_th &&
    !unified_mode`): teardown work gated on `!pump_persist` is DEAD CODE
    on the target — it only runs in `--decode`/`--replay-capture`/
    disablePlay paths. That is exactly how the XLVIII audio close
    vanished (F12). Post-pump cleanup belongs **immediately after
    `sess_pump_stop()`** (the join satisfies D21 single-ownership); and
    every pump-owned resource must be released before the draw returns —
    `memset(&pump, 0, sizeof pump)` at `run_session` entry drops handles
    WITHOUT closing them, which is how run 30's `audio.opus` got a
    2 505-byte scrambled page at the draw1→draw2 boundary.
23. **`--lottery=N>1` now changes TWO behaviours** (F13/F14): the audio
    file name (`<stem>_d<N><ext>`) and the jiggle arm (odd = ON, even =
    OFF, stated in the `lottery_arm` line). `--lottery=1` is
    byte-identical to XLVIII. A run that needs a constant arm must use
    `--lottery=1` — never assume, read `lottery_arm` /
    `feedback.jiggle`. And `--lottery-stop-on-keeper=0` (F15) means N ×
    `--session-seconds` of wall time: size the operator's window first
    (run 31 = 4 × up to 900 s ≈ 75 min worst case with queues).
24. **Healthy audio counters + a silent decode ≠ an audio bug** (run 30):
    read the VIDEO first (AU sizes ~1 KB/frame + ~10 keyframes/draw = a
    static screen; animated gameplay costs 5–30 KB/frame) and remember
    the `0x010a` dumper caps at **16 lines per session** (F1) and they
    all arrive in the first ~400 ms — an app that launches at t+139 s is
    INVISIBLE to that channel by construction. The AU-size timeline is
    the app-state evidence for late launches.
25. **F13 splits the audio path at the LAST DOT IN THE WHOLE STRING**
    (`strrchr(path, '.')`), not at the last dot of the file name:
    `--session-audio-out=C:\my.dir\audio` (a *directory* containing a
    dot, no extension) would become `C:\my_d1.dir\audio`. Pass a plain
    file name with an extension — run 31's `audio.opus` →
    `audio_d1.opus … audio_d4.opus` — and the case cannot arise. The
    builder is the bounded `pump_set_str` + `memcpy` idiom with an
    explicit length proof, so a too-long path keeps the un-suffixed name
    instead of truncating mid-string (`-Wformat-truncation` has nothing
    to prove; the paranoid knvstd leg re-checks it every round).

## 9. Repository map

```
README.md               layout, builds, provisioning, acknowledgements
START-HERE.md           THIS FILE — living handoff (update every round)
CMakeLists.txt          full build (knvstd + 21 suites; TLS backend choice;
                        KN_FFMPEG_PREFIX auto-detects build/ffmpeg/)
docs/BOOTSTRAP.md       the project map (mission, stages, logging, D1-D13)
docs/INTEGRATION.md     per-round delta notes (rev 41 = the top block)
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
| XLV | **run 26 = the lottery's first real sitting: 5 draws, 2 sessions (dead at 13.15 s + 11.34 s, both in-band; 379/379 + 342/342 AUs), 3 lost at the CM door — BOTH refusals measured**: seat cooldown >90 s (403 ×4) and the first-ever **429 create-rate-limiter** (killed draws 3/5 instantly under 403-only F4). F6 live-proven (honest rc 1 ×5, exit 1, `wire_session.log` returned). No SESSION_LENGTH_TIMER in either draw (intermittent: 23/25 yes, 24/26 no); all control ≤10 s again; band now **7 of 11 draws**. The pattern reopening the jiggle question: 5/5 jiggle-on draws in-band vs the only 2 long draws ever (18/22) jiggle-OFF — input eliminated as SAVIOUR (run 23), never tested as DEPRESSANT. Ships **F7** (retry 403+transient incl. 429, escalating 20/40/80/160 s, `cm_create_backoff {"status":...}`) + **F8** (`--session-jiggle=MS`, 0 = OFF — the A/B knob) + the knlog-APPEND field correction. Stamp **XLV**. Run 27 = the jiggle-OFF lottery |
| XLVI | **run 27 = THE JIGGLE A/B, DECIDED: THE SYNTHETIC MOUSE JIGGLE IS A SURVIVAL DEPRESSANT** — five OFF draws (same evening, same app, five fresh rigs) lived 10.3/21.8/15.4/**52.2**/22.9 s: 4/5 beyond the ON maximum (13.2 s), draw 4 = **PROJECT RECORD 52.19 s** (1546/1546 AUs, 13 071/13 071 datagrams, 1 gap, 0 decode errors — beats run 22's ~41 s, also jiggle-off); the five ON draws were 5/5 in-band (P ≈ 0.03 %). The activation handshake is EXONERATED (flew 5/5; only the periodic MOTION is implicated); the "fidelity ladder" reinterpretation is final (survival tracked the jiggle penalty + rig variance, never fidelity). **F7 field-proved** (draws 2–5 queued on the 4th create after exactly 20+40+80 s — the seat cooldown is ~2–3 min; no 429 at this cadence); F8 proof: `input_jiggle` ×0, `"jiggle":0` ×5. Draw 4's control plane alive at t+40 s (heartbeats = healthy-draw indicator). No timer ×5 (intermittent 2-of-8). Target T10 on XLV: ALL PASS 79. First delivery via the **karnynow-logs** git repo (complete: AUs, diag rotations, wire ring). Ships **F9**: jiggle default **OFF** (`--session-jiggle=100` = legacy); INPUT.md §9 verdict CORRECTED. Stamp **XLVI**. Run 28 = the keeper hunt (10 draws) + optional 2-draw ON re-control |
| XLVII | **runs 28/28b = THE FIRST KEEPERS — STAGE 2 CLOSED.** Run 28 (OFF, lottery 10): 33.2 s → 201.6 s (endurance record tripled: 6049/6049 AUs, 91 941 datagrams, 90 FEC repairs, zero gaps) → **FULL KEEPER: `stop_reason:"time"`, 360.0 s, 10 800/10 800 AUs (30 fps exact), 169 036/169 036 datagrams, zero gaps/errors/recoveries, ONE keyframe, idr/pli ×0, drift −3 ms**; lottery stopped on rc 0 by design; heartbeats ~1 Hz for the full 360 s. Run 28b (ON re-control): **a second keeper on draw 1** (360 s, 10 800/10 800, 171 427 datagrams, `jiggle:3533`, zero gaps) — which **DOWNGRADES the XLVI depressant verdict** to *suggestive, era-confounded* (the evening drifted favourable for BOTH settings: band → tails → OFF keeper → ON keeper within ~4 h); default stays OFF, handshake untouched, new trap 17 = same-evening-interleaved A/B rule. **F10**: knlog failure labels follow the stop_reason (`E_SESSION_RECOVERY_EXHAUSTED` for the dominant dead-draw class — run 28's draws logged `E_SESSION_NO_FRAMES` next to 998/6049 perfect decodes). Target T10 on XLVI: ALL PASS 79. Stage 3 opens: audio bundle (Opus) → G3 → G4 → G1 → D-t13. Stamp **XLVII**. Run 29 = the 30-minute soak (no au-dir!), optionally the first gameplay app |
| XLVIII | **runs 29/29g = THE ~600 s SERVER CEILING (⚠ WITHDRAWN IN XLIX — the operator's browser control gets a FULL HOUR on the same Warsaw pool / Free tier, so the ≈600 s cut is not a pool limit; see the XLIX row + trap 19) + THE FIRST GAMEPLAY KEEPER; F11 SHIPS THE BUNDLE-AUDIO EXTRACTION STAGE.** Run 29 (Endfield, 1800 s soak attempt): the pool never lets a draw near 30 min — draw 2 = 17 821 perfect AUs / **594 s** then a GRADUAL datagram decay (444→0 over ~9 s; decay = ceiling, abrupt = rig freeze) → recovery_exhausted; draw 3 = the POLITE face: `0x0109` + TERM `0x80008BE3`/56 + `0x80008BC7` at **599.2 s** → server_termination. Run 29g (**Watch Dogs: Legion, 100361011 — first gameplay-reaching app**): draw 1 = **flawless 599.1 s server_termination keeper, 17 970/17 970 AUs, zero gaps/errors** — the pipeline holds on interactive content (only 54 307 datagrams ≈ 3/AU — a low-bitrate scene); draws 2–3 died recovery_exhausted at **110 s / 144 s of media** (the 10–13 s band did NOT reproduce — the freeze lottery lives below the ceiling too). F10 matched 6/6 draws; F7 matched every queue. **F11** (sandbox-proven, target-pending): `kn_arx` (bundle SRTP audio rx — 14-step order pinned by `tools/arx_reference.py`) + `kn_ogg` (zero-alloc OggOpus muxer, byte-exact vs `tools/oggopus_reference.py`) + `kn_dtls_srtp_export` (RFC 5764 §4.2 export-keys; server half → RFC 3711 KDF; stub −1) + `--session-audio-out=F.opus` + `audio_arm`/`audio_close` + the 34-key summary `"audio"` block (skeleton canon **113**, new `tools/check_summary_skeleton.py`) + `audio.vectors` (188, registry 16) + `test_knogg` 163 / `test_knarx` 62 → ctest **22/22 ×2**, T10 dry **72** (target **82**), cross ×2 green, e2e 43. App-id canon corrected: 107057914 = **Arknights: Endfield** (always was). Stamp **XLVIII**. Run 30 = `--session-seconds=570 --lottery=2 --session-audio-out=audio.opus` |
| XLIX | **run 30 = F11'S FIRST FLIGHT: THE BUNDLE-AUDIO EXTRACTION IS PROVEN LIVE AND ZERO-ERROR — AND THE CAPTURE IS DIGITAL SILENCE BECAUSE THE GAME NEVER LAUNCHED; THE "~600 s SERVER CEILING" IS WITHDRAWN ON THE OPERATOR'S CORRECTION.** Both draws died `recovery_exhausted` (75.0 s / 197.0 s of media) but the audio stage was flawless: `armed:1 srtp_derived:1 file:1` ×2, `packets` 15 610 / **40 000** = `bundle_rtp_in`, `srtp_fail`/`malformed`/`toc_invalid`/`pt_filtered`/`padded`/`replay_reject`/`seq_loss`/`seq_reorder`/`seq_dup` ALL 0, `red_packets` = `packets` with `red_blocks` 2× and `red_invalid:0`, `last_ms−first_ms` = `packets × 20 ms` exactly, and the 2 089 866-B file is a valid OggOpus stream (ffprobe: 48 kHz stereo **199.92 s** 83 kb/s; 834 pages, contiguous seq, monotone granulepos; **39 929/39 936 audio packets TOC `0xEC` = CELT-FB 20 ms**) ⇒ the RFC 5764 §4.2 SERVER half is proven (40 000 HMAC authentications cannot pass with a wrong key). Decoded content: **mean −91.0 dB, every one of 19 146 240 samples |x|<8 = silence** — and content-side: 0x010a shows only `upc.exe`/`UplayService`/`UplayWebCore` (Ubisoft Connect, **no game process**), AU sizes 0.8–1.2 KB/frame = a static launcher screen, while the operator's browser control measured Legion's boot at **≈2 min 19 s from stream start to gameplay audio**. Freeze anatomy (cleanest yet): video+audio abrupt to 0 in one second while **SCTP `0x0101` heartbeats continued ~5 s** and the WSS stayed writable (`pongs:0 dead:0`). **THE CORRECTION (binding)**: the pool is Warsaw (`np-waw-01.cloudmatchbeta` — "beta" is the pool's name, not a tier), the account is Free, browser sessions run the **full hour** and were never cut at 10 min (matching run 25's `beforeEventMS:3599657`) ⇒ §36's ceiling reading is withdrawn; the ≈600 s cut is now a suspected **server-side idle/no-input policy**. Ships **F12** (`sess_audio_file_close` after `sess_pump_stop` — the XLVIII close was gated on `!pump_persist`, which is 1 in every live session, so `audio_close` NEVER fired: no EOS page, no fclose, draw-1's FILE* leaked across the lottery gap → the 2 505-byte scrambled page at the draw boundary), **F13** (one `audio_d<N>.opus` per draw), **F14** (the interleaved jiggle A/B: with `--lottery>1` odd draws ON / even OFF + a `lottery_arm` line — trap 17 satisfied inside one run), **F15** (`--lottery-stop-on-keeper=0|1`). Sandbox: native battery PASS zero warnings at the XLVIII baselines, cross `-Werror` ×2 arches PASS with all four vendor trees (no SKIPs), ctest **22/22 ×2 backends**; no wire/interface/KAT change ⇒ summary canon still **113**. Target T10 on XLVIII: **ALL PASS 82**. Stamp **XLIX**. Run 31 = `--session-seconds=900 --lottery=4 --lottery-gap=60 --session-jiggle=100 --lottery-stop-on-keeper=0 --session-audio-out=audio.opus` on Legion |
| L | **run 31 read + F16–F19: THE CLIENT CAN TYPE.** Run 31 (four interleaved Legion draws) proved F12/F14/F15 live, got **NO RESULT** on the ≈600 s cut (every draw died at 14–122 s of media), and exposed two of our own bugs by their own evidence lines: F13's audio path came out `audio_d1_d1us` (doubled suffix, extension destroyed — an overlapping `memcpy`, ASan-flagged on the rig) ⇒ **F16** (`kn_drawpath.{h,c}`, alias-free, 1051-check suite pinned against an independent second builder); and **no TEARDOWN ever reached the wire**, so every draw orphaned its seat and the next create ate 403 SESSION_LIMIT + 20/40/80 s ⇒ **F18** (`--session-release`: the TEARDOWN is now always logged, and the flag adds the CloudMatch DELETE; e2e 43 → **48** with the rig counting deletes). The operator's repeated "only the first `.264` works" is **answered with evidence, not assurance** ⇒ **F17**: a single-AU dump has no SPS/PPS (ffprobe: `au_00001_k` 1 frame OK, `au_00002_p` "Invalid data"), the new CONTINUOUS per-draw `video_d<N>.264` decodes **60/60 frames** from the shipping binary on the host (`logs/f17_video_proof_L.txt`), plus `--replay-video-out` so it is provable without a rig and draw-numbered AU names so draws stop overwriting each other. And the login wall (0x010a: only `upc.exe`; the operator's retraction: the GFN *browser* types the credentials into the stream) ⇒ **F19**: five byte-exact discrete builders (KEY 3/4, MOUSE_ABSOLUTE 5, MOUSE_RELATIVE 7, BUTTON 8/9, WHEEL 10; `test_kninput` 548 → **794**) under `--input-script=FILE` (timeline playback, t=0 = the draw's first media tick), `--input-live` (console forwarding, Win32 `_kbhit`/POSIX termios) and `--input-anti-afk[=MS]` (the reference's VK 0x7C F13 pulse) — ONE sender, ONE gate (nothing before `enable-input(true)`), route per `kn_input_route_sid`, plus `--input-check=FILE` (two ctest legs) so a login script is validated before it costs a queue slot. Gates: native PASS (21 suites, new one 1051), cross ×2 clean, ctest **25/25 ×2**, T10 dry **74** (was 72 — round L also caught that cmake *builds* every suite while T10 *runs* only its own list, so `test_kndrawpath` joined `suites` + `required_bins`; target expectation **82 → 84**), T13 63/51, e2e **48/48**, skeleton **113 == 113**, ASan/UBSan green including a sanitized `knvstd`. Stamp **L**. Run 32 = the login run (`--input-script=login.txt --session-video-out=video.264 --session-release`, 420 s, one draw) |

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
- **Since run 27 the evidence arrives by GIT, not chat uploads**: the
  operator cleans and pushes the whole run (`logs/` contents + the
  stdout capture + the T10 pair) to
  **`github.com/karnyjohnny/karnynow-logs`**. The agent clones it,
  copies everything into `runNN_logs/` inside the workspace
  IMMEDIATELY (the repo is wiped each round — the workspace archive is
  the only history), and analyzes from there. No more `.txt` renames,
  no 10-file limit; lottery runs APPEND across draws, so the pushed
  `logs/` directory is whole-run evidence by construction.
