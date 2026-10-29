# KarnyNow

A dependency-light **C11** implementation of the **GeForce NOW** ("Mjolnir" /
NVST) streaming-client protocol, targeting **Windows 7 SP1** (w64devkit GCC,
i686 + x86_64). It reimplements the protocol behaviour of the MIT-licensed
**OpenNOW** Rust reference client: CloudMatch queue → WSS/RTSPS signalling →
ICE/STUN → DTLS 1.2 (`use_srtp`) → SCTP control/input channels (usrsctp, 8
DCEP channels) → SRTP-protected RTP/H.264 video (FEC repair, NACK-v2,
reordering) → in-process FFmpeg decode → one strict-JSONL evidence line per
protocol event.

> Unofficial and experimental, built for research and education. This project
> is **not affiliated with or endorsed by NVIDIA Corporation**; "GeForce NOW"
> is a trademark of NVIDIA Corporation.

**Current state:** stage 2 of 5 — live sessions against the production GFN
cloud work end-to-end (real server video decodes through the full stack);
stream *sustainability* under the server's rate control is the open
investigation. The entry point for agents and contributors is
**[`START-HERE.md`](START-HERE.md)** (the living handoff document).

---

## Repository layout

```
README.md               this file
START-HERE.md           living handoff: state, next steps, agreements, traps
CMakeLists.txt          full build (knvstd + 20 test suites, 2 TLS backends)
docs/                   per-layer design records (RTSP, RX, SRTP, FEC, GS,
                        DTLS, SCTP, INPUT, NET, SOCK, D3D, AUDIO,
                        BUNDLE-AUDIO, LOGGING, AUTH) + BOOTSTRAP.md (the project map: mission, stage
                        gates, logging spec, decision register) +
                        INTEGRATION.md (per-round delta notes)
docs/archive/           frozen history: PROJECT-STATE.md (rounds I–XXXVIII
                        session log), HANDOFF.md, AGENT-HANDOFF.md, analiza/
docs/reference/         vendored reference headers (OpenH264 API, mingw-w64
                        d3d9/dxva2) — reference material, not compiled
docs/research/          early research notes
streamer/include/kn/    public headers (one per module)
streamer/src/           29 modules + knvstd_main.c (the production binary)
streamer/tests/         20 native test suites + KAT vector headers
streamer/assets/        synthetic test clips
tests/                  T1…T13 harnesses (T10 = the target build gate);
                        requirements.txt for the Python harnesses
tools/                  independent reference models (*_reference.py — the
                        authoritative wire truths), build/check scripts,
                        sandbox harnesses, the fake rig
third_party/            vendored dependencies — NOT committed; staged locally
                        (see below)
build/                  ALL build outputs — NOT committed, with ONE tracked
                        exception: build/ffmpeg/ (the user-supplied FFmpeg
                        dev tree, committed on purpose)
logs/                   ALL logs and test evidence — NOT committed.
                        knvstd --log-dir defaults to "logs"; the T-harnesses
                        write logs/tNN/ dirs + logs/TNN_wynik.txt verdicts
```

## Building & testing

### Windows 7 target (primary)

```bat
python tests\T10_build_and_test.py    :: cmake+ninja build, 20 suites, self-tests
python tests\T13_udp_e2e.py           :: local end-to-end path (no GFN account)
```

T10 **stages `third_party/` automatically** (downloads the pinned sources and
verifies SHA-256 before building). Evidence lands in `logs\T10_wynik.txt`
(full log) and `logs\T10_skrot.txt` (the verdict line, e.g. `ALL PASS 79`).

### FFmpeg (required for the decode path)

Drop a prebuilt FFmpeg dev tree into **`build/ffmpeg/`**:

```
build/ffmpeg/
├── bin/        ffmpeg.exe, ffprobe.exe, DLLs (shipped next to knvstd.exe)
├── include/    libavcodec/ libavdevice/ libavfilter/ libavformat/
│               libavutil/ libswresample/ libswscale/
├── lib/        STATIC libs (libavcodec.a …) + pkgconfig/
└── share/
```

It must contain **static** libraries — CMake checks for `lib/libavcodec.a`
(the `-static` link ignores `.dll.a` import libs), so build FFmpeg with
`--enable-static`. Both CMake and T10 auto-detect `build/ffmpeg/`; override
with `-DKN_FFMPEG_PREFIX=<path>` (CMake) or `set KN_FFMPEG_PREFIX=<path>`
(T10). This directory is the one tracked path under `build/` so the binaries
travel with the repo — see `build/ffmpeg/README.md`.

### Linux sandbox (development loop)

```sh
bash tools/build_native_tests.sh   # compile + RUN all native suites
bash tools/cross_check.sh          # Win7 cross-compile matrix, -Werror, both arches
cmake -B build && cmake --build build && ctest --test-dir build   # mbedTLS default
```

Sandbox vendor staging: `tools/prepare_mbedtls_sandbox.sh` (mbedTLS v3.6.0 +
framework submodule), `tools/fetch_ffmpeg_headers_sandbox.py` (headers for the
decode path). The full bootstrap recipe (clone URLs, pins, toolchain notes):
`docs/archive/PROJECT-STATE.md` §"Bootstrapping a NEW agent session".

## Conventions

- **Logs** — every C and Python log/test-evidence file goes under `logs/`
  (gitignored). `knvstd --log-dir` defaults to `logs`; T-harnesses use
  `logs/tNN/` subdirs and write `logs/TNN_wynik.txt` / `logs/TNN_skrot.txt`.
- **Builds** — everything compiled goes under `build/` (gitignored except
  `build/ffmpeg/`).
- **Zero warnings** on both tiers (sandbox GCC + target GCC 16.2):
  `-Wall -Wextra -Werror` plus dedicated paranoid legs.
- **Two-implementations rule** — every wire format is pinned twice: an
  independent Python model in `tools/` (self-proved against the reference's
  own test goldens before emitting vectors) and a C suite replaying the
  generated vectors. Never hand-write C scenario data.
- **Evidence-lines philosophy** — every protocol event prints one strict-JSON
  line on stdout (`tools/check_jsonl.py`-clean); a session prints a summary
  JSON at exit. A failure that cannot be diagnosed from the log alone is an
  instrumentation bug.
- **No TLS bypasses** anywhere; secrets are logged as lengths only.

## Third-party dependencies & acknowledgements

KarnyNow stands on excellent open-source work — with thanks to all of it:

| Component | Role here | License | How it is provided |
|---|---|---|---|
| [OpenNOW](https://github.com/OpenCloudGaming/OpenNOW) (© 2025 Zortos) | the protocol reference: semantics, wire layouts and test goldens this project reimplements (docs cite commit `a49b124ce31e`) | MIT | **not vendored** — studied as an external clone; no OpenNOW code is copied into this repo |
| [FFmpeg](https://ffmpeg.org/) | H.264 decode in-process (libavcodec / swscale / swresample / avutil) | LGPL-2.1+ (or GPL, depending on build configuration) | prebuilt win7 dev tree committed under `build/ffmpeg/` (built by the repo owner) |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) 3.6.0 | TLS/DTLS 1.2, X.509 verification (the `mbedtls` backend) | Apache-2.0 **OR** GPL-2.0-or-later | `third_party/mbedtls` — staged locally, not committed |
| [usrsctp](https://github.com/sctp/usrsctp) | userspace SCTP (AF_CONN) for the control/input plane | BSD-3-Clause | `third_party/usrsctp` — staged locally, not committed |
| [tiny-AES-c](https://github.com/kokke/tiny-aes-c) (kokke) | AES block primitives for SRTP/DTLS keying | public domain / Unlicense (per upstream) | `third_party/tiny-aes-c` — staged locally (T10 verifies SHA-256 of the pinned files) |
| [crypto-algorithms](https://github.com/B-Con/crypto-algorithms) SHA-1 (Brad Conte) | SHA-1 for DTLS fingerprints / SRTP KDF | provided "as is" (see file headers) | `third_party/crypto-algorithms-sha1` — staged locally |
| Cisco OpenH264 API headers; mingw-w64 `d3d9`/`dxva2` headers | signature-verification reference only — **not compiled into any binary** | BSD-2-Clause (Cisco); public domain / ZLIB (mingw-w64) | `docs/reference/` |

`third_party/` pins (enforced by `tests/T10_build_and_test.py`): tiny-AES-c
`23856752f` · crypto-algorithms `cfbde484` · usrsctp `c625e219` · mbedTLS
3.6.0 `2ca6c285` + framework `750634d3`.

## Documentation map

`START-HERE.md` → `docs/BOOTSTRAP.md` → `docs/RTSP.md` §25–26 (the live-run
record) → `docs/INTEGRATION.md` (latest rev) → the per-layer doc for whatever
you touch. Frozen history: `docs/archive/` (see its README for a guide).
