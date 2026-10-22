# INTEGRATION NOTES — Round VII delta, rev 2 (kn_diag + kn_selftest + kn_decode + knvstd)

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

## FFmpeg wiring (your N-127939-gbf0e831606 shared build)

T10 auto-detects the dev prefix at:

    C:\Users\KarnyJohnny\Desktop\ffmpeg\ffmpeg-win7-dev\usr\local

(override with the `KN_FFMPEG_PREFIX` environment variable if it moves).
When detected, T10 passes `-DKN_FFMPEG_PREFIX=<path>` to cmake, which:
- compiles `kn_decode.c` for real (`KN_HAVE_FFMPEG=1`)
- links `knvstd.exe`/`test_knselftest.exe` with `-lavcodec -lswscale -lavutil`
  (your `lib/*.dll.a` import libs)
- **copies the FFmpeg DLLs next to knvstd.exe** after the build
  (avcodec-63, avutil-61, swscale-10, swresample-7 — avcodec pulls in
  swresample transitively)
- records `extras: ffmpeg=<prefix>` in build.json (T10 verifies it)

T10 also GENERATES the `decode.synthetic` clip once, next to knvstd.exe:

    ffmpeg -f lavfi -i testsrc2=size=320x240:rate=30:duration=2 -c:v libx264
           -preset ultrafast -g 30 -sc_threshold 0 -pix_fmt yuv420p
           -f h264 synthetic.264

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
- `decode.synthetic` status ok with real fps/cpu numbers (320x240 clip should
  decode at hundreds of fps; anything < 60 reports `degraded`)
- build.json extras recording your FFmpeg prefix
- FFmpeg DLLs present next to knvstd.exe

## Notes / NOT VERIFIED list (§10.6)

1. `kn_decode.c` is compile-verified against your exact headers (both
   arches, -Werror) but NOT yet link/run-verified — that happens in this T10
   run. The API surface used is send/receive + swscale (stable since 2016).
2. `console.host` attribute read-back assumes ConEmu consumes ESC and lands
   the marker at the cursor cell (§6.2 behaviour). Under T10 stdout is a
   pipe, so the read-back reports "not probeable" and the ConEmuHk presence
   is the evidence — run `knvstd --selftest=console.host` directly in a
   ConEmu console for the full attribute probe.
3. `clock.resolution` thresholds (Sleep(1) <= 2.6 ms = ok) were calibrated by
   your round-1 run (1.25 -> 1.00 ms observed) — they hold.
4. Wall-tick before timeBeginPeriod measured 1250 us on your machine (not
   15.6 ms) — something in the system already raises the timer; harmless,
   recorded in evidence.
5. Stubs (`d3d.formats`, `d3d.present`, `gs.parse`, `net.*`, `tls.chain`)
   report `skipped` with explicit phase reasons — never `ok`.
6. `state.json` v1 carries the logging subsystem state only; the richer state
   machine registry lands with the subsystems that own state machines.
