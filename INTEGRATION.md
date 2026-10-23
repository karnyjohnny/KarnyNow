# INTEGRATION NOTES — Round IX delta, rev 4 (decode flush fix + kn_d3d phase-B probes)

> **Rev 4 supersedes rev 1-3.** The canonical integration document remains
> `PROJECT-STATE.md` (repo root). Subsystem docs: `docs/LOGGING.md`,
> `docs/FEC.md`, `docs/SRTP.md`, **`docs/D3D.md` (new: kn_d3d provenance,
> VEH crash-guard rationale, probe design)**.
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
