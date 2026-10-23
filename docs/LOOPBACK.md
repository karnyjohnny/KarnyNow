# Stage-1 video loopback (`knvstd --loopback`) — design notes

Status: **G1-VIDEO PASSED ON THE TARGET (T11 full 600 s run, 2026-10-23,
round XIII): fps_mean 30.000, frames_late 0/18 001, CPU 13.0 % of all
cores (≤45 % gate), RSS 20.9→18.8 MB (peak 24.2), perf lines 601, DPST 0,
logs clean, bundle contains perf.jsonl — VERDICT G1-VIDEO PASS 20/20.**
Caveat recorded: the synthetic clip decodes ~5× cheaper than T1's real
720p30 content (94 % skip MBs) — the production CPU reference stays T1
(28.9 % incl. NV12), real numbers arrive with stage 2 + G2. Sandbox
verification before that run: native 7 suites PASS, cross `-Werror`
x64/i686 clean incl. `-Wformat-truncation=2` on knvstd_main at
-O0/-O2/-O3, ctest 9/9, T10 dry-run ALL PASS, T11 POSIX-honest skip, and
the 720p source decoded 120/120 through `kn_decode_h264_file`.
Round XIII added the **D20 FPS overlay** (F5 toggle) — see below.

Scope: BOOTSTRAP §12 stage 1 offline path. Round XIV ADDED the WASAPI
synthetic-PCM half (`docs/AUDIO.md`): audio opens with the presenter,
`queue_audio_ms` is real (device padding), the summary carries an `audio`
block, `--no-audio` disables for debugging, and T11 now gates FULL G1
(video + audio, still no network). G1-VIDEO already PASSED on the target
(round XIII, 20/20); the full-G1 run is the next T11.

## What runs

```
knvstd --loopback[=FILE.264] [--loopback-seconds=N] [--log-dir=DIR]

source discovery   FILE.264  >  ./synthetic720.264  >  <exe dir>/synthetic720.264
                   >  EMBEDDED 320x240 clip (budget_valid:false + WARN)
pipeline (one thread, by design)
   kn_decode_h264_{file,mem}  (FFmpeg h264 threads=1 -> swscale NV12)
     -> per-frame callback:
          pace to absolute deadline  t0 + n*1e6/30 us   (D9)
          kn_d3d_vp_present_nv12:  LockRect(NOSYSLOCK)
                                   PER-ROW copy (surface pitch 1536 !=
                                     decoder stride 1280 — round-5 fact;
                                     a whole-buffer memcpy is WRONG)
                                   UnlockRect
                                   StretchRect(LINEAR) into the letterboxed
                                     backbuffer (0,40,1280,760 @ 1280x800)
                                   Present
          pump window messages (WM_CLOSE / ESC = graceful stop)
          every 1000 ms: one §7.6 perf event -> KN_CH_PERF -> perf.jsonl
   clip end -> decode again (loop) until end_us; abort inside the callback
```

Single-threaded is a stage-1 DELIBERATE choice: decode is 5-7× faster than
the pace (T1: 211 fps 720p decode vs 30 fps present), so pacing inside the
decode callback needs no queue — `queue_video_depth` reports an honest 0.
Stage 2's network path brings the reorder buffer and a real decode/present
split; the perf schema is already shaped for it.

## Measured facts this design stands on (target evidence, rounds 5-7)

| Fact | Value | Consequence encoded here |
|---|---|---|
| NV12 1280×720 DEFAULT lock pitch | **1536** (≠ width) | per-row copy, `min(pitch, vw, stride)` per row |
| first touch of a fresh surface | ~28 ms once (fill_max vs mean 2.13) | `kn_d3d_vp_open` PRE-FAULTS (gray fill + one present) before t0 |
| StretchRect+Present 1:1 letterbox | mean 0.28-0.43 ms, max ~5-6 ms | budget non-issue; present_ms tracked anyway |
| Sleep(1) real floor | ~2.9 ms (R30, rounds 4-6) | pacing = Sleep(deadline−3 ms) + QPC spin tail |
| 720p30 decode CPU | 24.8 % of both cores (T1), +NV12 28.9 % | G1-video CPU gate ≤ 45 % (all-cores convention) |
| wall-clock granularity | 15.6 ms even after timeBeginPeriod(1) | all pacing/timing uses QPC, never wall clock |

## Pacing and lateness

Deadlines are ABSOLUTE (`t0 + n·10⁶/30 µs`, integer math) — per-frame
errors never accumulate. A frame counts as `late` when its present STARTS
more than 2 ms after its deadline (the grace absorbs frame 0's callback
entry, whose deadline is t0 itself). Loopback never SKIPS a frame on
purpose; `frames_dropped` in the perf event = the late count (documented
deviation: with a self-fed source a true "drop" cannot happen; on the
network path stage 2 will report real drops in the same field).

Loop restarts (the 4 s clip re-decodes every ~120 frames) re-open the
decoder: that gap (~10-30 ms) lands in `decode_ms_max` honestly — it is a
real cost of the looping source, not of the pipeline.

## §7.6 perf event (all 25 spec fields + `seq`)

Emitted once per 1000 ms to KN_CH_PERF (perf.jsonl by the default jsonl
mask; the bundle collects it as `logs/perf.jsonl`). Conventions:

- `cpu_process_pct` / `cpu_system_pct`: **percent of ALL cores** (the §6.1
  table convention: 24.8 % = 720p30 decode; 100 % = machine saturated).
  Process = GetProcessTimes deltas; system = GetSystemTimes busy
  (kernel−idle+user) deltas; both divided by wall×ncpu.
- `frames_decoded/presented/dropped`: **per-interval deltas** (T11 sums
  them; cumulative counts live in the stdout summary).
- `decode_ms_p95`: per-frame decode+convert wall time, measured as the
  gap between consecutive callbacks (pacing happens inside the callback,
  after the measurement — so the gap is pure decode).
- `present_ms_p95`: copy + StretchRect + Present (the full per-frame
  presentation cost); `copy_ms_mean` in the stdout summary splits it.
- `rss_kb`/`peak_rss_kb`: K32GetProcessMemoryInfo (PSAPI_VERSION 2 →
  **kernel32**, no psapi.dll import — the F1 import set stays
  kernel32/user32/winmm/d3d9/msvcrt).
- `handle_count`: GetProcessHandleCount; `thread_count`: toolhelp snapshot.
- `timer_resolution_us`: timeGetDevCaps supported minimum — NOT the real
  floor (R30: clock.resolution remains the authority; ~2.9 ms measured).
- `refresh_hz_measured`: GetDisplayMode each interval; first change from
  the initial value sets `dpst_change_detected` (R9/D9 evidence per run).
- net/fec/nack/idr fields: honest zeros until stage 2 (schema stable from
  day one — T11 and the bundle readers never re-learn field names).
- Field count: 26 ≤ KN_LOG_MAX_FIELDS (32) with headroom for stage 2.

## Failure policy

- Every CPU-write section (pre-fault fill, per-frame copy) runs under the
  kn_d3d VEH guard (mingw has no `__try` — docs/D3D.md). A caught
  exception latches the **D7 permanent degrade**: the presenter refuses
  every further call (`KN_D3D_ERR_FAULT`), leaks its D3D objects ON
  PURPOSE, loopback stops gracefully, `kn_log_on_failure` fires (§7.7:
  amplify + seal + auto-bundle via `kn_diag_install` + relevant
  self-tests — the relevance mapping now catches `loopback`), and the
  stdout summary carries `fault`/`fault_addr`/`bundle`.
- Device lost (TestCooperativeLevel) → `KN_D3D_ERR_LOST` → graceful stop
  (the Reset()/rung-fallback ladder is stage 2; the loopback's job is to
  prove the happy path under G1, not to recover from driver death).
- Dimensions changing mid-stream (impossible with the fixed clips, a real
  case on the network path) → `KN_D3D_ERR_DIMS` → stop, not improvise.
- Source unreadable / decode error → stop with `stopped_reason`, rc 1.
- POSIX / stub builds: honest `{"error":"loopback_requires_win32"}`, rc 3.

## The 720p source asset

`streamer/assets/synthetic720.264` — 136 853 B, sha256
`870bdf29309cd717b04806f1f8f157ad106cf76d56545eb1c8c9d6500a5242a1`,
**H.264 High@5.1** (profile_idc 100 — the §6.1 budget shape), 1280×720
yuv420p, 120 frames @ 30 fps (4 s), GOP 30, `-bf 0` (no B-frames, like
zerolatency streams). Regenerate (sandbox-side only; the target FFmpeg has
no encoder):

```sh
ffmpeg -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=4" \
  -c:v libx264 -preset superfast -profile:v high -level 5.1 -bf 0 \
  -g 30 -sc_threshold 0 -b:v 250k -maxrate 300k -bufsize 600k \
  -pix_fmt yuv420p -f h264 streamer/assets/synthetic720.264
```

**Landmine recorded:** `-preset ultrafast` SILENTLY downgrades High →
Constrained Baseline (x264 drops CABAC/8x8dct there) even with an explicit
`-profile:v high` — a Baseline clip would have made the G1 CPU gate
lenient versus the High@5.1 budget it is supposed to verify. `superfast`
keeps High. Always check the SPS `profile_idc` byte, not ffprobe alone.

The clip is NOT embedded (unlike decode.synthetic's 60-frame KAT clip):
T11 passes its path explicitly, `--loopback` discovers it, and the
embedded 320×240 fallback reports `budget_valid:false` so a missing asset
can never masquerade as a real G1 run.

## D20 — FPS overlay (round XIII, user requirement)

Low-overhead on-screen FPS counter, **F5 toggles** (ESC / window close
still stop the run):

- **Where it draws:** straight into the locked NV12 surface, right after
  the per-frame row copy and before UnlockRect — inside the same
  VEH-guarded section, no extra lock/stretch/pass, no D3DX (not
  OS-built-in on Win7), no GDI, no lockable backbuffer. Cost: ~1.5 k pixel
  writes per frame for "FPS 30.0" (vs 1.38 M for the frame itself).
- **Font:** 5×7 bitmap, scale ×2, dark box (Y=0x10, neutral UV) + white
  glyphs (Y=0xEB) — readable over any content. Even coordinates only, so
  the 2×2 UV subsampling maps exactly. Font+blit live in a
  platform-NEUTRAL section of `kn_d3d.c` (`kn_d3d_overlay_text_nv12`) —
  `tools/sandbox_overlay_font.c` renders ASCII proofs of every glyph and
  exercises clipping/args (PASS in the sandbox before handover).
- **What it shows:** frames presented per second of the completed perf
  interval, computed from the MEASURED interval wall time — literally the
  same number the `perf.jsonl` line carries (`frames_presented` delta),
  formatted "FPS 30.0" (+ " LATE n" when the interval had late frames;
  "FPS --.-" during the first second).
- **Defaults:** ON in `--loopback` (it is a debug surface); the production
  streamer will open with it OFF and keep the same F5 toggle (stage 2 —
  recorded in the D20 decision row).

## T11 (tests/T11_loopback_g1.py) — the G1-video gate

Prereq: T10 has built `build\knvstd.exe` (T11 does not build). Modes:
`--quick` (30 s smoke), default 600 s (the G1 duration), `--seconds=N`,
`--source=PATH`. **Every run wipes `t11logs\` first** (round-XIII finding:
perf.jsonl accumulates across runs — a --quick smoke added 31 lines to the
600-s analysis). Gates, all computed FROM THE EVIDENCE (summary JSON +
perf.jsonl + logs + post-run bundle):

1. rc 0 + `stopped_reason == "elapsed"`
2. `budget_valid` + 1280×720 source
3. `fps_mean ≥ 29.4` (98 % of 30)
4. `frames_late ≤ max(5, 0.2 % of presented)`
5. `cpu_process_pct_mean ≤ 45` (% of all cores; expected ~30-35)
6. RSS drift (last-10 % mean − first-10 % mean) ≤ +4 MiB
7. perf lines ≥ 95 % of seconds; ≥ 90 % of per-second deltas in [27,33]
8. `dpst_change_detected == 0` (run on AC power — R9)
9. zero WARN and zero ERROR in `t11logs/*.log`
10. post-run `--diagnostics` bundle valid and CONTAINS `logs/perf.jsonl`
    (G1 requires "a diagnostic bundle that is complete and
    self-explanatory")

Outputs `T11_wynik.txt` + `T11_skrot.txt` (gitignored, like T10's).
Expected on a healthy run: G1-VIDEO PASS, fps_mean ≈ 30.0, late ≈ 0,
cpu ≈ 30-35 %, RSS drift ≈ 0, ~601 perf lines.

## Stage-1 remainder / stage-2 hooks

- Reset()/rung-fallback ladder on device-lost (D7 rungs 2-4) — stage 2.
- Real decode/present thread split + frame queue (capacity 2-3 per
  analiza/04 §2.4) — stage 2, when the network feeds the decoder.
- F7/settings video-mode switch (D14) needs renegotiation — stage 5; the
  presenter already derives the letterbox rect from actual frame dims.
