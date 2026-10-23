# INTEGRATION NOTES — Round XV delta, rev 8 (G1 CLOSED + phase-C network probes — registry 12/12)

> **Rev 8 supersedes rev 1-7.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/NET.md`** (endpoints, schannel flow, verdict
> table, T10 gating policy, landmines).
>
> **G1 PASSED ON THE TARGET (T11 full 25/25, T10 r10 40/40) — STAGE 1 IS
> CLOSED.** Round XV opens stage 2 with its diagnostic floor:
> 1. **NEW module `kn_net`** + the four phase-C self-tests (net.dns,
>    net.tcp443, net.tls, tls.chain) — REAL measurements on the target
>    against login.nvidia.com + prod.cloudmatchbeta.nvidiagrid.net with
>    non-NVIDIA baselines; schannel TLS with verification ALWAYS ON;
>    the §7.9 registry now has **ZERO stubs (12/12 implemented)**.
> 2. **T10**: 8 suites RUN (round-XIV omission fixed: test_knaudio was
>    built but never executed), net-probe gating policy (dns/tcp gated,
>    tls/chain reported), stub-count check replaced by "no stubs left".
>    Round 11 needs internet at the target and takes ~30 s extra.
> 3. New PE imports: ws2_32/secur32/crypt32 (OS-built-in).
>
> **Rev 7 (still current):** kn_audio WASAPI synthetic PCM + full-G1 T11.

# (older) Round XIV delta, rev 7 (WASAPI synthetic PCM — STAGE 1 CODE-COMPLETE)

> **Rev 7 supersedes rev 1-6.** Canonical state: `PROJECT-STATE.md`.
> Design record for this delta: **`docs/AUDIO.md`** (WASAPI engine
> pattern, COBJMACROS/INITGUID/Start-before-thread landmines, stage-2
> audio TODOs).
>
> **Rev 7 headline changes** (after target confirmations: T10 r9 40/40,
> T11-overlay quick G1-VIDEO 20/20 — "działa idealnie"):
> 1. **NEW module `kn_audio`** (`streamer/{include/kn,src}/kn_audio.*`):
>    WASAPI shared-mode event-driven render at the device mix format
>    (float32/s16/s32), dedicated refill thread, soft 440 Hz tone or
>    silence, honest counters (events/frames/starved/timeouts/
>    buffer_errors/queue_ms). Portable half (WAVEFORMATEX byte parser +
>    phase-continuous generator) host-tested by the **8th suite
>    `test_knaudio`** (22 checks). Only new PE import: ole32.dll.
> 2. **`--loopback` grew audio**: opens with the presenter; failure never
>    kills video (§7.7 amplification + summary `"audio"` block);
>    `queue_audio_ms` in perf.jsonl is now real; `--no-audio` for debug.
> 3. **`tests/T11_loopback_g1.py` is now the FULL G1 gate** (+5 audio
>    checks; verdict renamed "G1 PASS"). G1-VIDEO already passed twice on
>    the target — the next full T11 is the stage-1 verdict.
> 4. Build wiring: `kn_audio.c` in kncore (CMake INTERFACE: WIN32
>    `d3d9 winmm ole32`, POSIX `m`), 8 suites in both shell gates,
>    test_knaudio cross-links knlog (the engine logs).
>
> **Rev 6/6b (still current):** stage-1 video loopback + T11 + D20 FPS
> overlay (F5); 720p High@5.1 asset (ultrafast-downgrade landmine).

# (older) Round XII delta, rev 6 (stage-1 video loopback + T11 G1-video gate)

> **Rev 6 supersedes rev 1-5.** Canonical state: `PROJECT-STATE.md`.
> Design record for this delta: **`docs/LOOPBACK.md`** (architecture,
> measured-facts table, §7.6 conventions, failure policy, T11 criteria,
> the 720p asset recipe + the x264 ultrafast-downgrade landmine).
>
> **Rev 6 headline changes** (after T10 round 7 = ALL PASS 40/40, which
> closed the whole §7.9 phase-A+B verification chapter):
> 1. **`knvstd --loopback[=FILE.264] [--loopback-seconds=N]`** — the
>    stage-1 video path: decode → paced 30 fps → per-row copy into locked
>    DEFAULT NV12 → StretchRect(LINEAR) → letterboxed window → §7.6 perf
>    event per second into perf.jsonl → single-line summary JSON on stdout.
> 2. **NEW `kn_d3d_vp_*` presenter** (grown on the probe code; VEH guard +
>    D7 degrade latch + pre-fault) and **NEW `kn_perf`** (system counters;
>    K32/PSAPI_VERSION 2 — no new DLL imports).
> 3. **NEW asset** `streamer/assets/synthetic720.264` (High@5.1 120f —
>    NOT embedded; knvstd discovers it, embedded 320×240 fallback reports
>    budget_valid:false).
> 4. **NEW gate `tests/T11_loopback_g1.py`** — run AFTER T10:
>    `python T11_loopback_g1.py --quick` then `python T11_loopback_g1.py`
>    (600 s, AC power). Writes T11_wynik.txt + T11_skrot.txt.
> 5. WASAPI synthetic PCM is the NEXT delta (round XIII) — full G1 gates on
>    it; T11 currently verdicts "G1-VIDEO".
>
> **Rev 6b (round XIII, after T11 G1-VIDEO PASS 20/20):** the D20 FPS
> overlay — 5x7 bitmap font blitted into the locked NV12 surface during
> the per-frame copy (no D3DX/GDI, ~1.5 k writes/frame, inside the VEH
> section); **F5 toggles**, default ON in --loopback; text = the perf
> interval's frames-per-second ("FPS 30.0", +" LATE n"), refreshed once
> per second. Font+blit are platform-neutral (`kn_d3d_overlay_text_nv12`)
> with an ASCII-proof harness (`tools/sandbox_overlay_font.c`, PASS).
> `tests/T11_loopback_g1.py` now WIPES t11logs at start (perf.jsonl
> accumulated across the quick+full runs: 632 vs 601 lines). Changed:
> `streamer/{include/kn,src}/kn_d3d.*`, `streamer/src/knvstd_main.c`,
> `tests/T11_loopback_g1.py`, `docs/LOOPBACK.md`; new:
> `tools/sandbox_overlay_font.c`.
>
> **Rev 5/5b (still current):** kn_gs parse layer + gs.parse self-test
> (78 KAT vectors, portable) + test_kngs (7th suite); capnote[320] fix.

# (older) Round X delta, rev 5 (kn_gs parse layer + gs.parse self-test)

> **Rev 5 supersedes rev 1-4.** The canonical integration document remains
> `PROJECT-STATE.md` (repo root). Subsystem docs: `docs/LOGGING.md`,
> `docs/FEC.md`, `docs/SRTP.md`, `docs/D3D.md`, **`docs/GS.md` (new: kn_gs
> provenance, vector pipeline, stage-2 TODOs)**.
>
> **Rev 5 headline changes** (after T10 round 5 = ALL PASS 38/38):
> 1. **NEW module `kn_gs`** (`streamer/{include/kn,src}/kn_gs.*` +
>    `streamer/src/kn_gs_kat_vectors.h` generated): the NVST RTP/GS parse
>    layer — RTP header/CSRC/extension/padding, the 16-byte LE GS body
>    (pitfalls #1/#2 encoded), Annex-B payload location (≤68-byte search),
>    keyframe detection, trailing-AUD strip, FEC block layout, stream-index
>    continuity, kn_ring_dump hexdump line parsing. Pure, zero-allocation.
> 2. **`gs.parse` self-test IMPLEMENTED** (no longer a stub): 78-vector KAT
>    battery from the shipped binary, portable (runs on host AND target),
>    plus optional real capture via `KN_GS_CAPTURE=<path>` or
>    `./gs_capture.hex`. Stubs remaining: 4 (net.dns, net.tcp443, net.tls,
>    tls.chain).
> 3. **NEW generator `tools/gs_reference.py`** (independent Python port of
>    nvst.rs parse ranges; self-proves with hand goldens + pitfall
>    discriminators + 700 round-trips before emitting) and **NEW host suite
>    `streamer/tests/test_kngs.c`** (105 checks: vectors + C-side property
>    round-trips + error paths). Suite count is now SEVEN (CMake,
>    build_native_tests.sh, cross_check.sh, T10 all updated).
> 4. Relevance mapping (§7.7) extended for video failures: adds `gs.parse`
>    and the keywords nack/reorder/reassemb/pli. **Not** bare "rtp" —
>    it substring-matches inside "srtp" (caught by the host suite).
> 5. Round-5 evidence recorded: **pitch 1536 for 1280×720 NV12 DEFAULT**
>    (per-row copy mandatory in stage 1!), first-touch fill ~28 ms once
>    (pre-fault before pacing), present mean 0.28-0.43 ms, **SWVP actually
>    S_OK** (G0.2 §2.1 correction — details in docs/D3D.md "Target
>    evidence"), clock.resolution stable-degraded (~2.9 ms floor, R30).
>
> **Rev 5b (T10 round 6 fix, 39/40 -> expected 40/40):** GCC 16.2 caught
> one `-Wformat-truncation` in the new `st_gs_parse` capnote assembly that
> sandbox GCC 12 missed (R28 third incident): `capnote[256]` was too small
> for the PROVEN worst case (284 B with the modulo-bounded counters and the
> 128-char path precision) -> now `capnote[320]`, verified clean under the
> paranoid `-Wformat-truncation=2` on both toolchains. Also: the T10 line
> "no FFmpeg DLLs next to knvstd.exe" is an EXPECTED confirmation of the
> D18 static build, not a warning — wording clarified (a static dev tree
> correctly has no DLLs anywhere: bin/*.exe are CLI tools, lib/*.a are what
> knvstd links).
>
> **Rev 4 (still current):** decode parser-flush fix (60/60 on target),
> kn_d3d probes (R25 milestone 0 PASSED), knvstd immediate selftest
> logging, T10 standalone milestone-0 run, static FFmpeg closure.
>
> **Rev 4 headline changes** (after T10 round 4 on the target — 31/32 PASS,
> the only failure was `decode.synthetic` frames=59/60):
> 1. **`kn_decode.c` parser-flush fix** (the round-4 failure): `kd_run`
>    flushed the DECODER at EOF but never the PARSER — `av_parser_parse2`
>    withholds the last access unit until the NEXT start code, which never
>    comes at end-of-input, so exactly one frame was silently lost with
>    rc=0/failed=0. Reproduced 1:1 in the sandbox (FFmpeg 5.1 AND the
>    target's 63 behave identically), fixed by adding the NULL-input parser
>    flush before the decoder drain, refactored through a shared
>    `kd_send_drain` helper. Sandbox proof: 60/60 frames, abort path still
>    returns -4 at exactly N frames, truncated input survives (40 frames,
>    0 failed, no crash).
> 2. **NEW module `kn_d3d`** (`streamer/{include/kn,src}/kn_d3d.*`):
>    `d3d.formats` (D7 session-start capability matrix) and `d3d.present`
>    (R25 milestone 0: fill EVERY byte of a locked DEFAULT NV12 surface,
>    read-back verified, then timed StretchRect(LINEAR)+Present ×30 at
>    1280×720→fullscreen). Crash guard = VEH + longjmp + D7 permanent
>    degrade latch (mingw GCC has NO `__try` — verified). Both self-tests
>    are now REAL on Win32, honest "win32-only" skips on the host.
> 3. **`knvstd` crash-survivor logging**: `--selftest=all` now runs test by
>    test, logging a `selftest_begin` marker + each result IMMEDIATELY at
>    INFO (soft-flushed per §7.12) — an uncatchable fault mid-run still
>    names the exact test in `diag.log` (T4/R2b rule).
> 4. **T10 rev 4**: runs `--selftest=d3d.present` STANDALONE first (R25
>    milestone 0 in its own process), checks d3d verdicts (ok/degraded —
>    `failed` fails T10 on purpose: stage 1 must not start on a broken
>    video path), requires `decode.synthetic` status **ok** (60/60), and
>    the stub count is now 5 (gs.parse + net/tls).
> 5. Relevance mapping extended: video/d3d-flavoured failures re-run
>    `decode.synthetic + d3d.formats + d3d.present` into the bundle.
>
> **Rev 3b (T10 round 3 fix, still current):** static FFmpeg needs the FULL
> closure at link time — `KN_FFMPEG_LIBS` is `avcodec swscale swresample
> avutil` (+`bcrypt` on WIN32). CONFIRMED WORKING by round 4: configure
> found the new top-level prefix, link succeeded, no DLLs (static),
> `build.json extras` recorded the prefix.
>
> **Rev 3 headline changes** (after T10 round 2 on the target):
> 1. FFmpeg MUST be rebuilt `--enable-static --disable-shared` (D18): the F1
>    `-static` link ignores `.dll.a` — CMake now fails at configure time with
>    the exact recipe if `lib/libavcodec.a` is missing.
> 2. `decode.synthetic` uses a clip EMBEDDED at build time
>    (`streamer/assets/synthetic.264` → `streamer/src/kn_synthetic_clip.h`,
>    regenerate with `tools/make_synthetic_clip.py`) — the target FFmpeg has
>    no encoder and external files go missing. `kn_decode_h264_mem()` added.
> 3. GCC 16.2 fixes: modulo-bounded timestamp formatters (kn_diag,
>    kn_selftest), `0x0601` expectations, platform-aware test_knselftest,
>    ok_count read on Win32, T10 PE-check guard.
> 4. No zip deltas anymore: the user syncs from the workspace/repo directly.

Copy these files into `NewKarnyNow\` preserving paths (overwrite the rev-1
copies if you already applied them). All code compiles with zero warnings
under `-Wall -Wextra -Wpedantic` (host GCC 12) AND `-Wall -Wextra -Werror`
(mingw x64+i686), including `kn_decode.c` against the real FFmpeg headers at
your exact commit `bf0e831606` (libavcodec 63 / libavutil 61 / libswscale 10).

Rev 2 fixes the four T10 round-1 findings (all harness/environment issues, no
functional bugs found in the subsystems themselves):
1. GCC 16.2 flagged two `-Wformat-truncation` in UTC timestamp formatters
   that GCC 12 accepted — all format inputs are now modulo-bounded.
2. `test_kndiag`/T10 expected `"0x601"` in build.json; the stringified macro
   is `"0x0601"` — T10 now parses the value as an int.
3. `test_knselftest` hard-coded HOST expectations (ok==2/skipped==10); on the
   target `clock.resolution`+`console.host` really run — expectations are now
   platform-aware.
4. `decode.synthetic` is now IMPLEMENTED (FFmpeg), no longer a stub.

## New files

| File | Purpose |
|---|---|
| `streamer/include/kn/kn_diag.h` | diagnostic bundle contract (BOOTSTRAP §7.8) |
| `streamer/src/kn_diag.c` | store-only ZIP writer, cap enforcement, failure hook, selftest provider |
| `streamer/include/kn/kn_selftest.h` | self-test registry contract (BOOTSTRAP §7.9) |
| `streamer/src/kn_selftest.c` | 12 registered self-tests (fec/srtp vectors, clock, console, decode + 7 honest stubs) |
| `streamer/include/kn/kn_decode.h` | H.264 decode contract (D3/D4/D6 pipeline) |
| `streamer/src/kn_decode.c` | libavcodec h264 threads=1 -> yuv420p -> NV12 (swscale) -> callback; honest stub without FFmpeg |
| `streamer/src/knvstd_main.c` | knvstd skeleton: `--selftest=<name\|all>`, `--diagnostics`, `--version`, `--list-selftests` |
| `streamer/tests/test_kndiag.c` | 109 checks incl. an embedded store-only ZIP reader |
| `streamer/tests/test_knselftest.c` | ~92 checks (platform-aware), diag-provider integration |
| `tools/check_zip.py` | independent bundle validation (python `zipfile`) |
| `tests/T10_build_and_test.py` | target harness rev 2 (FFmpeg-aware; successor of T9) |

## Modified files

| File | Change |
|---|---|
| `CMakeLists.txt` | +kn_diag/kn_selftest/kn_decode in kncore; +test_kndiag/test_knselftest/knvstd targets; `KN_FFMPEG_PREFIX` cache option; FFmpeg DLLs copied next to knvstd.exe POST_BUILD; git rev/dirty + effective flags + ffmpeg extras injected as `KN_DIAG_BUILD_*` for build.json |
| `tools/build_native_tests.sh` | +new suites, +knvstd smoke, +check_zip.py, +compile check of kn_decode/kn_selftest against `third_party/ffmpeg-headers` when present |
| `tools/cross_check.sh` | +kn_decode.o, +new exes (`-lwinmm`), +FFmpeg-headers compile check per arch |
| `tools/fec_reference.py` | output path: KAT vectors moved to `streamer/src/` |
| `tools/srtp_reference.py` | output path: KAT vectors moved to `streamer/src/` |

## Moved files (IMPORTANT)

- `streamer/tests/knfec_kat_vectors.h`  ->  `streamer/src/knfec_kat_vectors.h`
- `streamer/tests/knsrtp_kat_vectors.h` -> `streamer/src/knsrtp_kat_vectors.h`

Reason: §7.9 requires `fec.vectors`/`srtp.vectors` to run FROM THE SHIPPED
BINARY, so the generated vectors are production assets now (same precedent as
`knfec_tables.h`). **Delete the old copies from `streamer/tests/`** — a
quoted `#include` resolves the includer's directory first, and stale
duplicates would silently shadow the src/ versions. Content is
byte-identical (regenerated and diffed against your originals).

## FFmpeg wiring (your N-127939-gbf0e831606 STATIC build, D18)

T10 auto-detects the dev prefix at (probe order, first hit wins):

    C:\Users\KarnyJohnny\Desktop\ffmpeg\ffmpeg-win7-dev            (current)
    C:\Users\KarnyJohnny\Desktop\ffmpeg\ffmpeg-win7-dev\usr\local  (legacy)

(override with the `KN_FFMPEG_PREFIX` environment variable if it moves).
When detected, T10 passes `-DKN_FFMPEG_PREFIX=<path>` to cmake, which:
- compiles `kn_decode.c` for real (`KN_HAVE_FFMPEG=1`)
- FATAL_ERRORs at configure time if `lib/libavcodec.a` is missing (the F1
  `-static` link ignores `.dll.a`; the message carries the rebuild recipe)
- links `avcodec swscale swresample avutil` (+`bcrypt` on WIN32) — the full
  static closure proven by T10 round 3/4, in GNU-ld dependency order
- copies `bin/*.dll` next to knvstd.exe ONLY if present (a static build
  has none — round 4 confirmed "no FFmpeg DLLs … expected")
- records `extras: ffmpeg=<prefix>` in build.json (T10 verifies it)

The `decode.synthetic` clip is EMBEDDED at build time
(`streamer/assets/synthetic.264` → `streamer/src/kn_synthetic_clip.h`,
regenerate with `tools/make_synthetic_clip.py` — sandbox-side only; the
target FFmpeg has no encoder). External clip discovery
(`KN_SYNTHETIC_H264` env / `./synthetic.264` / exe-dir) still works and
takes precedence when present.

The decode pipeline (measured facts it encodes): threads=1 (D4), NO scaling
(D6), NV12 output for the lockable DEFAULT-pool surface path (T4 v4 result;
UpdateSurface is dead, §9 R2). FFmpeg's own logs are mapped INTO the knlog
media channel at DEBUG/TRACE — failure amplification (§7.7) raises media to
DEBUG automatically, so FFmpeg diagnostics appear exactly when needed and
never fsync-storm the sinks on corrupt-frame spam.

## Running on the target

```bat
cd /d "%USERPROFILE%\Desktop\NewKarnyNow\tests"
python T10_build_and_test.py
```

Under ConEmu, elevated, as usual. Send back `T10_wynik.txt` + `T10_skrot.txt`.
Expected new evidence in this run:
- `decode.synthetic` status **ok**: 60/60 embedded frames (the parser-flush
  fix; round 4 saw 59/60) with real fps/cpu numbers on the E5500
- **two brief fullscreen window flashes** are NORMAL: `d3d.present` runs
  standalone (milestone 0) and again inside `--selftest=all`; it also runs
  inside `test_knselftest.exe` (run_all + the video relevance case)
- `d3d.formats`/`d3d.present` statuses ok (or degraded with a named reason);
  `failed` fails T10 by design — that is an R25/D7 finding, send it back
- no FFmpeg DLLs next to knvstd.exe (static build); knvstd.exe now also
  imports d3d9.dll (OS-built-in) — the PE check still requires 6.01

## Notes / NOT VERIFIED list (§10.6)

1. `kn_decode.c`: link+run VERIFIED on target (round 4: rc=0, 59 frames,
   fps≈3700, cpu 93.8% of one core during the 16 ms run). The parser-flush
   fix is sandbox-verified (60/60 on FFmpeg 5.1 + edge contracts) but
   **NOT yet target-verified** — that is this T10 run.
2. `kn_d3d.c` has NEVER EXECUTED anywhere (sandbox has no D3D9 runtime):
   compile+link evidence only (mingw x64/i686 -Werror, F1 flags, d3d9
   import in the PE). Every call pattern is copied from T4 v4, which ran on
   the target. First real execution = this T10 run.
3. `console.host` attribute read-back assumes ConEmu consumes ESC and lands
   the marker at the cursor cell (§6.2 behaviour). Under T10 stdout is a
   pipe, so the read-back reports "not probeable" and the ConEmuHk presence
   is the evidence — run `knvstd --selftest=console.host` directly in a
   ConEmu console for the full attribute probe.
4. `clock.resolution` thresholds (Sleep(1) <= 2.6 ms = ok) were calibrated
   by the round-1 run (1.25 -> 1.00 ms). Round 4 measured 15.50 -> **2.75**
   (verdict degraded, tolerated): wall tick stayed 15.6 ms and the post-
   timeBeginPeriod sleep mean was above the ok threshold — consistent with
   background load at 02:16 (round 1 ran with something already raising the
   timer to 1.25 ms). One loaded sample is not a recalibration basis; the
   verdict system reported the variance honestly, which is the design. D9
   stands: pacing code measures intervals, never assumes 1 ms sleeps.
5. Stubs (`gs.parse`, `net.*`, `tls.chain` — 5 names) report `skipped`
   with explicit phase reasons — never `ok`.
6. `state.json` v1 carries the logging subsystem state only; the richer
   state-machine registry lands with the subsystems that own them.
