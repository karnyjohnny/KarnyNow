# KarnyNow — implementation project state (living handoff)

> This file is the **implementation agent's** running state. It supersedes the research
> `HANDOFF.md` (archived verbatim under `uploads/`, along with `BOOTSTRAP.md`, the map of
> everything already known). Read `BOOTSTRAP.md` first for mission, hardware, architecture,
> decisions D1–D13, verified facts, the logging spec (§7), pitfalls and the stage plan.
> Language rule D12: everything in this repo is **English**.

## Bootstrapping a NEW agent session (do this first, ~5 minutes)

Everything needed is public or in this repo. No file transfers from the user required.

```sh
# 1. This repo (source of truth; the user keeps it current):
git clone https://github.com/karnyjohnny/KarnyNow.git
# 2. Protocol reference (MIT; commit a49b124 is the verified baseline —
#    all nvst.rs line numbers in the docs point at it):
git clone https://github.com/OpenCloudGaming/OpenNOW.git
#    Key file: native/opennow-streamer/crates/opennow-streamer-transport/src/nvst.rs
#    Key docs: docs/core-protocol.md, docs/mjolnir-nack-v2.md,
#              docs/session-start-progress.md, docs/streamer-comparison/*
# 3. third_party/ is GITIGNORED — fetch it:
#    a) tiny-AES-c (Unlicense), UNMODIFIED upstream defaults (AES128, ECB+CBC+CTR):
#       https://raw.githubusercontent.com/kokke/tiny-AES-c/master/aes.{c,h}
#       -> third_party/tiny-aes-c/
#    b) Brad Conte SHA-1 (public domain):
#       https://raw.githubusercontent.com/B-Con/crypto-algorithms/master/sha1.{c,h}
#       -> third_party/crypto-algorithms-sha1/
# 4. OPTIONAL (sandbox compile-checks of kn_decode.c only; the TARGET build uses
#    the user's own FFmpeg dev tree instead): headers at the user's exact commit:
#       https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/bf0e831606938f530326c9679591109407fa8d0d
#    extract libavcodec/*.h libavutil/*.h libswscale/*.h -> third_party/ffmpeg-headers/
#    then create two stand-ins for configure-generated headers:
#      libavutil/avconfig.h:  #define AV_HAVE_BIGENDIAN 0   (+ guard)
#      libavutil/ffversion.h: #define FFMPEG_VERSION "N-127939-gbf0e831606" (+ guard)
```

Sandbox toolchain (agent side; **does NOT persist between chat messages** —
reinstall every session, ~2-4 min):

```sh
apt-get update && apt-get install -y gcc cmake ninja-build libc6-dev \
    gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
# verify everything before trusting a run:
sh tools/build_native_tests.sh     # host: 6 suites + knvstd smoke + zip/jsonl validation
sh tools/cross_check.sh            # mingw x64+i686, -Wall -Wextra -Werror, F1 flags
```

**The authoritative compile+run gate is the TARGET machine** (w64devkit GCC
16.2.0 — stricter than sandbox GCC 12; it catches warnings GCC 12 misses):
`python tests\T10_build_and_test.py` under ConEmu → `T10_wynik.txt` +
`T10_skrot.txt`. The user runs it quickly and willingly — ask for a
measurement instead of guessing (§10.7).

## Current status (updated 2026-10-23, impl round IX)

| Stage | State |
|---|---|
| **0 — measurements** | **COMPLETE.** G0.1 CPU ✅ · G0.2 D3D9 presentation ✅ **(T4 v4)** · G0.3 toolchain ✅ · G0.4 console ✅ |
| **1 — C skeleton offline + logging subsystem** | **Core subsystems + diagnostics + self-tests IMPLEMENTED; phase-B video probes delivered.** knlog/knring/knfec/knsrtp(ph1+2) verified on target (T9 ALL PASS + T10 rounds 1/4). `kn_diag` bundle + `kn_selftest` registry + `knvstd` skeleton target-verified. **T10 round 4: 31/32 PASS** — static FFmpeg link (D18 closure) CONFIRMED working, all 6 unit suites green on GCC 16.2 (incl. srtp ph2 77 checks), bundle 5060 B validated, PE 6.01. The single failure was `decode.synthetic` frames=59/60: **root-caused (missing PARSER flush at EOF — the last access unit never leaves `av_parser_parse2`), reproduced 1:1 in the sandbox on two FFmpeg majors, fixed + measured 60/60** (abort/truncation contracts re-proven). NEW: `kn_d3d` module — `d3d.formats` (D7 session-start matrix incl. 60 Hz-at-res R9 evidence) + `d3d.present` (**R25 milestone 0**: fill every byte of a locked DEFAULT NV12, read-back verified, timed StretchRect(LINEAR)+Present ×30, VEH crash guard + D7 permanent-degrade latch) — sandbox compile+link verified (mingw x64/i686 -Werror, d3d9 import in PE), **never yet executed anywhere: round 5 is the first run**. knvstd now logs `selftest_begin` + every result immediately (INFO soft-flush) so an uncatchable fault still names the running test in diag.log. Remaining for G1: `--loopback` video path (decode → locked DEFAULT NV12 → StretchRect → letterboxed window, perf snapshots §7.6), WASAPI. |
| 2–5 | not started |

Gate G0.2 evidence and the settled presentation chain: `docs/analysis/G0.2-T4v4-RESULTS.md`.
One-line summary: **NV12 `StretchRect` from a DEFAULT-pool surface into the backbuffer
works (S_OK, 3.0 ms incl. Present); SYSTEMMEM NV12 fill crashes the driver (0xC0000005,
both arches, exact call isolated) and is permanently excluded.**
`kn_d3d` design + provenance + crash-guard rationale: `docs/D3D.md`.

## Decision register — additions this session

D1–D13 are in `BOOTSTRAP.md` §4 (unchanged, except D7 settled below).

| # | Decision | Reason / source | Reversal condition |
|---|---|---|---|
| **D7 (settled)** | Presentation chain: NV12 DEFAULT → `StretchRect(LINEAR)` → backbuffer; fallbacks YUY2 → X8R8G8B8 → GDI `SetDIBitsToDevice`. No SYSTEMMEM NV12, no offscreen conversion targets, `CheckDeviceFormatConversion` before every `StretchRect`, permanent degrade on AV. | T4 v4 measurement; `docs/analysis/G0.2-T4v4-RESULTS.md` §5 | none expected |
| **D14** | Default video mode **720p30**, switchable. Both mechanisms: a **settings screen** choice (applied at launch) **and an F7 in-session hotkey** cycling 720p30 → 540p60 → 540p30 → 360p30. In-session application needs renegotiation (`dynamicStreamingMode` / re-ANNOUNCE) = stage 5; until then F7 queues the choice for the next launch and says so on screen. | User answer to open question Q2 ("720p default, switchability nice; F7 vs settings — don't know") → provide both, staged honestly | user preference |
| **D15** | Game list = **account library only** (`filters: {variants:{gfn:{library:{status:{notEquals:"NOT_OWNED"}}}}}`), not the full GFN catalog. R24 pagination guards (context hash + revision re-check before/after each page) still mandatory. | User answer Q3 (uses GFN for owned/free-to-play games) | user preference |
| **D16** | Stream renders in a **separate window**; all keyboard/mouse capture lives in the streamer process; the Python TUI stays in the console. (Confirms the BOOTSTRAP §3 architecture — was a recommendation, now a user requirement.) | User answer Q4 | — |
| **D17** | Artwork: **text-only game list for stage 4**. A bounded local artwork cache may be added behind a flag after G4 (disk + HTTP cache on a 2 GB machine — deferred, non-blocking). | User answer Q5 ("don't know") → decide by not blocking on it | revisit after G4 |
| **D18** | **FFmpeg is consumed STATIC** (`--enable-static --disable-shared`), linked into `knvstd.exe` under the canonical F1 `-static` (D8). The decode.synthetic clip is **embedded in the binary** (`streamer/src/kn_synthetic_clip.h`, generated by `tools/make_synthetic_clip.py` from `streamer/assets/synthetic.264`; 60 frames 320×240 yuv420p, 42 KB). | T10 round 2 proof: GNU ld with `-static` **ignores `.dll.a` import libraries** ("cannot find -lavcodec — have you installed the static version?") — a shared-only FFmpeg cannot link into an F1 binary. Embedded clip: the user's FFmpeg build has no encoder (`--disable-autodetect`, no libx264 — `-preset` unrecognized), and §7.9 says "a BUNDLED .264" — hermetic beats external file. Static also kills DLL deployment failure modes on Win7. | If a future dependency is shared-only (e.g. an NVENC-style DLL), relax `-static` per-target and ship DLLs next to the exe (the CMake POST_BUILD copy path still exists) |
| **D19** | **Toolchain stays w64devkit** (GCC 16.2.0 + cmake 4.4.3 + ninja, `C:\Programy\w64devkit`). NOT WinLibs, NOT Nuwen, NOT Cygwin. | It is measured end-to-end on the target: T2 (15 variants ran on Win7), T9/T10 (zero-warning builds, correct PE 6.01, imports only KERNEL32/msvcrt/WINMM), FFmpeg built with it works. Cygwin is disqualified by design (POSIX runtime layer, not native Win32). WinLibs-MSVCRT/Nuwen are equivalent-ish but unproven here — switching re-opens R6 for zero gain. | Only if w64devkit develops a blocking defect; WinLibs **MSVCRT** variant (never UCRT — no UCRT on stock Win7) is the designated fallback, re-run T2 before trusting it |

## Risk register — changes this session

| # | Change |
|---|---|
| R2 | **CLOSED** (T4 v4: NV12 path works; crashing variant isolated and excluded) |
| R2b | **CLOSED** (v4 writer preserved all data through a crashing run — pattern proven; the same rules ship in the client per §7.12) |
| R9 | **Mitigated** (60 Hz available and active; user keeps 60 Hz on battery; D9 stands) |
| R17 | **CLOSED** (GPU does CSC + scale; CPU never touches pixels) |
| **R25 (impl round IX: MITIGATION BUILT, awaiting execution)** | Writing into a **locked DEFAULT NV12** surface is the only video-path operation not yet exercised (lock itself verified OK, pitch = width). P=low, S=high. Mitigation now shipped as code: `knvstd --selftest=d3d.present` (isolated process — T10 runs it STANDALONE first) fills + read-back-verifies every byte + presents ×30 under a VEH crash guard; on AV the D7 latch degrades permanently and the fault address is recorded next to the intended extent. **Closes when round 5 reports d3d.present ok/degraded on the target.** |
| **R29 (new, round IX)** | mingw GCC provides NO `__try/__except` (verified both arches) — the crash marker is `AddVectoredExceptionHandler` + `longjmp`. Sound for the expected fault shape (our own store instruction into the locked buffer — the exact G0.2 §3 shape, driver not on the stack, no locks held by us), but it has **never executed on the target** and cannot be exercised in the sandbox (no D3D9 runtime). If the AV instead happens INSIDE driver code on later calls, longjmp could leave driver state dirty — mitigated by the permanent-degrade latch (no D3D call ever again in-process) and by the standalone-process run order in T10. Watch round 5. |
| **R26 (REVISED, impl round VI/VII)** | The agent sandbox **DOES have a working toolchain** (the round-II "apt fails" era is over): `apt-get update && apt-get install -y gcc cmake ninja-build libc6-dev gcc-mingw-w64-x86-64 gcc-mingw-w64-i686` (~2-4 min). **Packages do NOT persist between chat messages** — reinstall at the start of every session (see Bootstrapping). If apt lists hang, fall back to direct .deb downloads from `deb.debian.org/debian/pool/` + `dpkg -i` (proven: libc6-dev must match installed libc6 `2.36-9+deb12u14`; ninja must be bookworm `1.11.1`, NOT trixie `1.13.2` which needs GLIBC_2.38). Sandbox GCC is 12.2 — the TARGET's GCC 16.2 remains the authority (it catches `-Wformat-truncation` classes GCC 12 misses; see R28). Every C delta still ships through `build_native_tests.sh` + `cross_check.sh` + a target T10 run. |
| **R27 (new, CLOSED by D18)** | `-static` (F1/D8) vs shared FFmpeg: ld ignores `.dll.a`, error text is misleading ("have you installed the static version?"). Reproduced 1:1 in the sandbox. Mitigation shipped: CMake configure-time FATAL_ERROR with the rebuild recipe when `lib/libavcodec.a` is missing; FFmpeg rebuilt static per D18. |
| **R28 (new)** | Sandbox GCC 12.2 vs target GCC 16.2 warning drift: T10 round 1 failed on two `-Wformat-truncation` GCC 16 flags that GCC 12 accepted (timestamp formatters). Mitigation: all `snprintf` with time/foreign-string inputs use **explicitly bounded arguments** (modulo/precision), verified at `-O0..-O3`; T10 fails the build on ANY warning so drift can never ship silently. Round IX re-confirmed the discipline (cross `-Werror` caught a 64-bit format promotion in the new d3d code on the first pass). |
| **R30 (new, round IX, informational)** | `clock.resolution` variance between runs: round 1 measured Sleep(1) 1.25→1.00 ms (wall tick 1250 us — something had already raised the system timer); round 4 measured 15.50→**2.75** ms (wall tick 15600 us before AND after timeBeginPeriod) — verdict `degraded`, harness tolerates it. One loaded sample (02:16 run) is not a recalibration basis. Consequence for stage 1: pacing must measure real intervals (D9 already mandates it); do NOT design on "Sleep(1) == 1 ms" on this machine. |
| **R15 (impl round II)** | **Mostly closed.** T8 on the target resolved all 14 Python packages rc=0 (pip honours Requires-Python, so every pin declares 3.8 support): textual 6.2.1, rich 14.3.4, httpx 0.28.1, httpcore 1.0.9, h11 0.16.0, anyio 4.5.2, + 8 more — `python/requirements.txt` updated, wheels cached on target in `tests\t8_wheels\`. Remaining: offline install + runtime smoke at stage 4 (httpx 0.28 removed `proxies=`; textual 6.x API churn vs old docs). |
| **R26 (new)** | **Sandbox has no compiler and must not install one** (apt installs fail with infrastructure errors; packages do not persist across turns anyway). The compile gate is the TARGET machine: `tests/T9_build_and_test.py` (w64devkit GCC 16.2 + cmake + ninja — the authoritative toolchain; it caught a `-Wstring-compare` that sandbox GCC 12 missed). Consequence: C leaving this workspace is compiled by the user, so review it line-by-line before handing over and keep changes per round small. |

Everything else (R3 usrsctp/DTLS spike, R7 first real `POST v2/session`, R22 31-field
session context, R23 RESUME, R24 catalog revision binding) unchanged — see BOOTSTRAP §9.

## Next steps (ordered)

1. **USER ACTION — re-run T10 (round 5)**. Everything from round 4 is fixed
   or newly delivered (see impl round IX below):
   ```bat
   cd /d "%USERPROFILE%\Desktop\NewKarnyNow\tests"
   python T10_build_and_test.py
   ```
   Expected: **`decode.synthetic` ok with 60/60 frames** (parser-flush fix);
   **`d3d.formats` + `d3d.present` execute for the first time** (brief
   fullscreen window flashes are normal — d3d.present runs standalone as the
   R25 milestone-0 process AND inside --selftest=all AND inside
   test_knselftest.exe); verdict ALL PASS with ~38 checks. If `d3d.present`
   reports `failed` with an exception code, that is the R25 verdict — send
   the files back unchanged, stage 1 then goes to the D7 GDI rung. Send back
   `T10_wynik.txt` + `T10_skrot.txt`.
2. **gs.parse self-test** + the RTP/GS parser itself (`kn_gs.c`, analiza/04
   §1-2): parse kn_ring_dump hexdumps and synthetic KAT packets (little-endian
   body, `ext[11]` block indices, ≤68 B start-code search — pitfalls 1/2/4).
   This is also the first stage-2 module. Reference: OpenNOW `nvst.rs`
   (clone per Bootstrapping; verified baseline a49b124).
3. **Stage 1 `--loopback`**: knvstd grows the video path on `kn_d3d` —
   decode file → NV12 → locked DEFAULT surface → StretchRect → 1280×800
   letterboxed window (D6/D7/D9), perf snapshots (§7.6), WASAPI synthetic
   PCM. Gate G1: 30 fps × 10 min, CPU ≤ ~45%, RSS flat, bundle complete, no
   network. (R25 milestone 0 = green `d3d.present` from step 1 gates this.)
4. Phase C self-tests (net.dns/tcp443/tls, tls.chain) via **schannel**
   (Win7-native, zero deps, diagnoses exactly what the C client will use);
   they gate stage 2 debugging, not stage 1.
5. Stage 2+ per BOOTSTRAP §12 (protocol without DTLS/SCTP → R3 spike →
   TUI → hardening). R22 (31-field session_info 1:1) and R23 (RESUME +
   orphan cleanup) remain mandatory at their stages.

## Repo layout (this workspace → user machine)

```
karnynow/
├── AGENT-HANDOFF.md            ← snapshot-style handoff (round VI research view)
├── PROJECT-STATE.md            ← THIS file (living state; read Bootstrapping first)
├── INTEGRATION.md              ← file-by-file integration notes for the deltas
├── docs/
│   ├── LOGGING.md              ← knlog/knring conformance + API guide
│   ├── FEC.md                  ← knfec provenance (nvst.rs line refs) + semantics
│   ├── SRTP.md                 ← knsrtp provenance + phase-2 checklist
│   ├── D3D.md                  ← kn_d3d provenance (G0.2/T4 refs) + VEH guard
│   ├── analysis/               ← measurement analyses (G0.2 closed here)
│   └── research/               ← BOOTSTRAP.md + research HANDOFF (copies)
├── streamer/                   ← knvstd (C11)
│   ├── include/kn/             ← knlog knring knfec knsrtp kn_aes kn_sha1
│   │                              kn_diag kn_selftest kn_decode kn_d3d
│   ├── src/                    ← implementations + GENERATED: knfec_tables.h,
│   │   │                          knfec_kat_vectors.h, knsrtp_kat_vectors.h,
│   │   └                          kn_synthetic_clip.h (embedded decode clip)
│   ├── assets/synthetic.264    ← clip source (regenerate header via tools/)
│   └── tests/                  ← portable unit tests (run on Linux AND Win7):
│                                  knlog knring knfec knsrtp kndiag knselftest
├── python/                     ← TUI (stage 4); requirements.txt RESOLVED via T8
├── tests/                      ← target-machine harnesses T1-T10
│                                 (T9 = old build gate; T10 = current, supersedes)
├── tools/                      ← build/verify scripts + KAT generators
│   (build_native_tests.sh, cross_check.sh, check_jsonl.py, check_zip.py,
│    fec_reference.py, srtp_reference.py, make_synthetic_clip.py)
├── third_party/                ← GITIGNORED: tiny-aes-c, crypto-algorithms-sha1
│                                  (+ ffmpeg-headers in the agent sandbox only)
└── CMakeLists.txt              ← Windows build (w64devkit: cmake + ninja);
                                   KN_FFMPEG_PREFIX option, git/flags → build.json
```

Maps onto the user's repo `C:\Users\KarnyJohnny\Desktop\NewKarnyNow\`.

## Working agreements (carried over, still binding)

- Measure first, then code; every stage has a gate; a failed gate is information.
- **Compile gate is two-tier** (R26 revised): sandbox `build_native_tests.sh` +
  `cross_check.sh` (fast, catches most things) and the TARGET `tests/T10_build_and_test.py`
  (w64devkit GCC 16.2 — authoritative; it caught `-Wstring-compare` and two
  `-Wformat-truncation` classes the sandbox GCC 12 missed). Zero warnings on both.
- Compile before handing over (when a compiler exists: `tools/build_native_tests.sh`,
  `tools/cross_check.sh`); zero warnings with `-Wall -Wextra`.
- Keep artefacts small: every big JSON gets a `*_skrot`/`*_summary` sibling.
- Isolate anything that can crash (separate process / SEH marker before risky calls).
- Name unknowns explicitly (`NOT VERIFIED` / `UNVERIFIED` labels in code).
- Never disable TLS verification; never bypass login/entitlements/queue; no network in
  MOCK mode; secrets logged as lengths/presence only (whitelist redaction at the logging
  boundary).

## Session log (implementation)

### 2026-10-21 · impl round I
Received T4 v4 results (`T4_wynik.json` complete this time — writer fix proven) and the
user's answers to the five open questions. Analysed the run: G0.2 **closed**, D7
**settled** (NV12 StretchRect primary; SYSTEMMEM NV12 fill isolated as the crashing call
and excluded; offscreen conversion targets refused with `D3DERR_INVALIDCALL`; harness
`srf_*` mask bug documented), R2/R2b/R17 closed, R9 mitigated, R25 opened (DEFAULT NV12
fill → stage-1 milestone 0). Recorded D14–D17 from the user's answers. Built the
implementation workspace; wrote the C logging subsystem core + ring buffers + unit tests
(BOOTSTRAP §7, D13); drafted cp38 `requirements.txt` and the T8 dependency probe for the
user. → `docs/analysis/G0.2-T4v4-RESULTS.md`

### 2026-10-21 · impl round II
User ran the Win7 build (cmake 4.4.3 + ninja, w64devkit GCC 16.2.0):
**test_knlog 105 checks PASS, test_knring 34 checks PASS on the target** —
real Win7 file/rename/flush semantics verified. GCC 16 caught one warning the
sandbox GCC 12 had missed (`-Wstring-compare` in `kn_redact_key_is_secret`
when inlined into `kn_log_on_failure` with short literal keys) → fixed with a
`strstr(norm,"srtp")==norm` prefix test (fix awaits T9 verification).
**T8 resolved R15**: all 14 cp38 packages downloadable rc=0; pins recorded in
`python/requirements.txt`; wheels cached in `tests\t8_wheels\` on the target.
**Sandbox lost its toolchain** (apt installs now fail with infrastructure
errors) → new binding agreement **R26**: no toolchain installs in the
sandbox; the target machine is the compile authority via
`tests/T9_build_and_test.py` (new: cmake/ninja build with direct-gcc fallback,
runs all three test binaries + JSONL validation, emits `T9_skrot.txt`).
**FEC half of BOOTSTRAP §13.3 delivered (pending compile)**: fetched full
`nvst.rs` @ a49b124 into `artifacts/nvst-extract/`, extracted the exact GF/
Cauchy/reconstruction rules with line provenance (`docs/FEC.md`), wrote
`tools/fec_reference.py` (independent Python port; self-check reproduces the
reference's own test `nvst.rs:13239`), generated `knfec_tables.h` +
`knfec_kat_vectors.h`, implemented `streamer/src/knfec.c` (zero-allocation
encoder + Gauss-Jordan reconstruction) and `streamer/tests/test_knfec.c`
(tables-vs-definition sweep of all 65 536 products, golden Cauchy matrices,
nvst.rs:13239 port, zero-padding semantics, wide-block encode KAT, 200
deterministic property round-trips, error paths). Created `AGENT-HANDOFF.md`
(recovery document for a future agent, with the file list to attach).
→ `docs/FEC.md`, `tests/T9_build_and_test.py`, `AGENT-HANDOFF.md`

### 2026-10-21 · impl round III
Resend of the "no sandbox toolchain" instruction → scanned the workspace (all
round-II artefacts intact) and continued the queue with the SRTP KDF/IV half
of BOOTSTRAP §13.3. Fetched RFC 3711 text and read the vector appendices from
source (B.2 = AES-CM, B.3 = KDF — the common "B.1 is the KDF" assumption from
memory was WRONG; fetching caught it). Extracted nvst.rs crypto facts:
labels 0x00/0x01/0x02 SRTP + 0x03/0x05 SRTCP (88-95), profile set incl. GCM_8
and AES-256 variants (301-330), KDF/IV layouts (2873-2939). Vendored
tiny-AES-c (Unlicense, sha256 recorded, PROVENANCE.md) behind a `kn_aes`
wrapper with a compile-time size assertion; wrote `knsrtp` (KDF + AES-CM
keystream + three IV builders, zero allocation) and `test_knsrtp` (~40 checks
against FIPS-197/SP800-38A/RFC-3711-B.2/B.3 goldens + cross-impl vectors).
`tools/srtp_reference.py` self-proves its pure-Python AES against all RFC
constants before emitting vectors — and immediately earned its keep: the first
S-box draft degenerated because **AES's 0x11B is not primitive (ord(0x02)=51;
the S-box needs generator 0x03, unlike FEC's 0x11D where 0x02 works)** —
recorded as a landmine. Wired test_knsrtp into CMakeLists/T9/shell gates;
docs/SRTP.md written (incl. the phase-2 checklist: HMAC-SHA1 + be32(roc),
replay window, SRTCP, mbedTLS decision). knfec + knsrtp now share one gate:
T9 on the target.
→ `docs/SRTP.md`, `streamer/src/{kn_aes.c,knsrtp.c}`, `streamer/tests/{test_knsrtp.c,knsrtp_kat_vectors.h}`, `third_party/tiny-aes-c/`, `tools/srtp_reference.py`

### 2026-10-21 · impl round IV
First T9 run on the target. **cmake build rc=0, warnings=0 under GCC 16.2**
(the `-Wstring-compare` fix is confirmed). Manual runs (T9's exe launch hit a
Windows trap, below): **test_knlog 105/105 PASS, test_knring 34/34 PASS,
test_knsrtp 37/37 PASS** — SRTP phase 1 reproduces FIPS-197/SP800-38A/RFC
3711 B.2 (incl. the FEFF→FF00 counter carry) and B.3 (incl. the 94-octet
auth key) byte-for-byte with the vendored tiny-AES-c on the production
toolchain. **test_knfec 122/125**: property round-trips 200/200 and every
golden vector PASS; the 3 failures were a test-side state leak in
`test_error_paths` (NULL `shards[2]` carried between subcases → `-2` fired
before the tested behaviour). Fixed: each subcase now sets full explicit
state. Two harness bugs fixed: (1) T9 launched test exes with relative paths
while passing `cwd` to Popen — on Windows the executable path resolves
against the CALLER's cwd, not the `cwd` argument (documented Python
behaviour) → absolute paths now, comment added; (2) same trap noted in
`run_tests` for future agents. Stage-1 crypto+FEC cores are now target-
verified; remaining gate: clean T9 re-run.
→ `streamer/tests/test_knfec.c` (fix), `tests/T9_build_and_test.py` (fix)

### 2026-10-21 · impl round V
**First T9 run: VERDICT ALL PASS.** cmake build rc=0 **warnings=0** (GCC 16.2),
test_knlog 105/105, test_knring 34/34, test_knfec **125/125** (error-path fix
confirmed; property 200/200), test_knsrtp 37/37, JSONL validation PASS. The
crypto/FEC/logging verification chapter is closed for phase 1.
Then implemented **SRTP phase 2** (from `nvst.rs`, read not remembered):
vendored Brad Conte SHA-1 (public domain, sha256 recorded) behind `kn_sha1`
with RFC 3174 KAT; HMAC-SHA1 RFC 2104 glue with RFC 2202 cases 1-7 KAT;
`kn_srtp_auth_tag`/`verify_tag` (be32(roc) suffix, constant-time compare,
authenticate-before-decrypt); `protect/unprotect_cm_hmac` in-place (ROC
derived from packet_index internally — no inconsistent-pair footgun);
`kn_srtp_replay` window 1:1 from nvst.rs:3121-3211 (guess/check/commit,
2048 packets, 32xu64 bitmask, defensive no-op instead of the reference's
panic). Evidence that **SRTCP-CM is not needed** (nvst SRTCP is GCM-only;
RR rides DTLS-encrypted `rtcp1` SCTP). `srtp_reference.py` extended: p2
self-check (RFC3174 x4 via hashlib oracle, RFC2202 x7, protect goldens with
B.3-derived session material, 96-op replay script vs a line-by-line python
port of ReplayWindow) — the generator caught my TEST4 transcription error
(TEST4a+TEST4b = 64-byte base, not 32) before emission, twice proving the
"vectors self-prove first" rule. test_knsrtp extended (~35 new checks;
~72 total). CMake/T9/shell gates updated for the new sources. Everything
awaits the next T9 run.
-> `streamer/src/kn_sha1.c`, `streamer/src/knsrtp.c` (phase 2),
   `streamer/tests/test_knsrtp.c`, `third_party/crypto-algorithms-sha1/`,
   `tools/srtp_reference.py`, `docs/SRTP.md`

### 2026-10-21/22 · impl round VI (second agent chat — workspace handoff via files)
A second agent chat received the file handoff (BOOTSTRAP.md, research HANDOFF.md,
analiza/04+05, all streamer sources/tests, tools, T9_wynik.txt) and rebuilt the
full workspace from it: cloned OpenNOW @ a49b124, **verified every cited nvst.rs
line reference against the live tree** (derive_aes_cm_key@2873, srtp_aes_cm_iv@2902,
ReplayWindow@3121, gf256_tables@3717, nvst_cauchy_coefficient@3785 — all exact),
fetched third_party from upstream, reproduced **T9-equivalent ALL PASS on the
Linux host** (341 checks) and cross-check zero-warning x64+i686, and confirmed
the KAT generators reproduce the checked-in vectors **byte-identically**.
Then implemented the AGENT-HANDOFF priority — **§7.8 diagnostic bundle**:
`kn_diag` (store-only ZIP, sequential write / never fseek over own output per
§7.12, `summary.json` as the FIRST entry with per-subsystem verdicts, 8 MiB cap
enforced continuously — SECONDARY generations dropped whole first, PRIMARY
tail-kept newest-with-marker, CORE untouchable; auto-trigger through the
existing `kn_log_failure_cb_register` hook with recursion guard; auto
build.json/state.json; selftest-provider hook for §7.7 step 4) and the
**§7.9 self-test registry** `kn_selftest` (12 names; fec.vectors 88 KAT checks
+ srtp.vectors 52+96-op replay FULLY implemented; clock.resolution +
console.host implemented for Win32; 8 honest phase-B/C stubs that can never
read as passing; relevance mapping op/code → tests) plus the **knvstd
skeleton** (`--selftest=<name|all>`, `--diagnostics`, `--version`,
`--list-selftests`; JSON-lines stdout; exit codes 0/1/2/3). Test tooling:
`test_kndiag` (109 checks, embeds an independent store-only ZIP reader with
CRC verification), `test_knselftest`, `tools/check_zip.py` (python zipfile as
second opinion), `tests/T10_build_and_test.py` (target harness: T9 scope +
live Win32 self-tests + bundle validation + PE 6.01 check). KAT vectors moved
`streamer/tests/` → `streamer/src/` (they are production assets now — the
shipped binary runs them; generators updated; regeneration byte-identical).
Host suites: 452 checks, 0 failures. → this section, INTEGRATION.md rev 1.

### 2026-10-22 · impl round VII (T10 rounds 1-2 on target + FFmpeg integration)
**T10 round 1 on the target: 30 checks, 4 failed — zero functional bugs.**
All four were environment/expectation issues, fixed the same day: (1) GCC 16.2
flagged two `-Wformat-truncation` in UTC formatters GCC 12 accepted → all
format inputs modulo-bounded, clean at -O0..-O3 (R28); (2)+(4) `"0x601"` vs
the stringified `"0x0601"` in test+harness → int-parsed; (3) test_knselftest
hard-coded host expectations while clock.resolution/console.host really ran
on Win7 → platform-aware. **First real target evidence for §7.9**: Sleep(1)
1.25→1.00 ms after timeBeginPeriod(1); ConEmuHk present (build 230724 — D1
confirmed from inside a shipped binary); fec/srtp vectors pass in the GCC 16.2
binary; bundle written+validated on Win7 (4863 B, PE 6.01).
**FFmpeg answered** (open question from round VI): the user built it himself —
git master N-127939-gbf0e831606, w64devkit GCC 16.2, `--enable-shared
--disable-static --enable-gpl --disable-autodetect`; ffmpeg.exe runs on Win7.
Sandbox pulled the headers at the EXACT commit (avcodec 63/avutil 61/swscale
10.2.100 match) and implemented **`kn_decode`**: Annex-B → av_parser →
libavcodec h264 threads=1 (D4) → swscale yuv420p→NV12 (no scaling — D6;
NV12 for the locked DEFAULT surface, never UpdateSurface — R2) → per-frame
callback; stats frames/fps/cpu%-of-core/worst-frame; 64 MiB input cap;
av_log mapped INTO knlog media at DEBUG/TRACE (amplification surfaces it
exactly when needed; no fsync-storm on corrupt-frame spam). Compile-verified
against the real headers with -Werror on both arches.
**T10 round 2 on the target: link FAILED** — `cannot find -lavcodec … have you
installed the static version?` Root cause reproduced 1:1 in the sandbox: the
canonical F1 `-static` makes ld ignore `.dll.a` import libraries, and the
FFmpeg build was shared-only. Also: clip generation failed (no libx264 in a
`--disable-autodetect` build → `-preset` unrecognized), and GCC 16 caught an
unused-but-set `ok_count` in the Win32 test branch. **Fixes (D18):** FFmpeg
goes static (user rebuilds — exact recipe in Next steps step 1 and enforced by
a CMake configure-time FATAL_ERROR with the recipe when `libavcodec.a` is
missing); decode.synthetic gets a **build-time embedded clip**
(`streamer/assets/synthetic.264`, 60×320×240 yuv420p 42 KB, →
`kn_synthetic_clip.h`, hermetic — no encoder needed on target); `kn_decode`
grew `kn_decode_h264_mem` (file API refactored over a common padded-buffer
core); T10 hardened (PE-check guard, informational DLL report, embedded-clip
expectations). Toolchain decision recorded (D19: stay on w64devkit; Cygwin
disqualified; WinLibs-MSVCRT = designated fallback only). Sandbox suites after
the fixes: native 452 checks PASS, cross -Werror PASS (incl. kn_decode+
kn_selftest vs real FFmpeg headers), ctest 8/8, T10 dry-run ALL PASS (29).
Awaiting: user FFmpeg static rebuild + T10 round 3.
→ `streamer/{include/kn,src}/kn_decode.*`, `streamer/src/kn_synthetic_clip.h`,
   `streamer/assets/synthetic.264`, `tools/make_synthetic_clip.py`,
   `CMakeLists.txt` (KN_FFMPEG_PREFIX + static guard + DLL copy),
   `tests/T10_build_and_test.py` rev 3, fixes in kn_diag/kn_selftest/tests

### 2026-10-23 · impl round VIII (T10 round 3 diagnosis — static FFmpeg closure)
User rebuilt FFmpeg static per D18 (confirmed: `--enable-static
--disable-shared` in `ffmpeg.exe -version`, same commit N-127939-gbf0e831606)
and moved the dev prefix to `C:\Users\KarnyJohnny\Desktop\ffmpeg\ffmpeg-win7-dev`
(bin/include/lib at top level — no `usr\local` anymore). **T10 round 3: cmake
configure PASS (prefix + `libavcodec.a` guard both worked), ninja build FAILED
at the knvstd/test_knselftest link**: `undefined reference to swr_*` (DSD/DST/
Opus/WavPack decoders inside `libavcodec.a` call libswresample) and
`BCryptGenRandom/BCryptOpen/BCryptClose` (avutil's `av_random_seed` on
Windows). A shared build hides this (each DLL carries its own deps); a static
link needs the full closure, single-pass, in dependency order. Fixed in
CMakeLists: `KN_FFMPEG_LIBS = avcodec swscale swresample avutil` + `bcrypt`
on WIN32 (bcrypt.dll exists since Vista — Win7-safe). T10 now probes both
prefix layouts (current top-level, legacy `usr\local`) with `KN_FFMPEG_PREFIX`
override. Configure-time guards re-verified in the sandbox with a fake prefix
(bad include path → readable FATAL; missing `.a` → full rebuild recipe).
Sandbox regression after the fix: native 452 checks PASS, cross `-Werror`
PASS x64+i686 (incl. kn_decode vs the real FFmpeg 63 headers), ctest 8/8,
T10 dry-run ALL PASS (29). The 6 unit suites + kncore were never affected
(only the two FFmpeg-linking executables failed to build, everything else ran).
Awaiting: T10 round 4 on the target — the first run of `decode.synthetic`
with real fps/CPU numbers on the E5500.
→ `CMakeLists.txt` (KN_FFMPEG_LIBS closure), `tests/T10_build_and_test.py`
  (prefix probing)

### 2026-10-23 · impl round IX (T10 round 4: 31/32 — decode flush fix + kn_d3d phase-B probes)
**T10 round 4 on the target: 31/32 PASS.** The D18 static closure worked
end-to-end: configure found the top-level prefix, `libavcodec.a` guard
silent, link OK, zero warnings under GCC 16.2, no DLLs, all six unit suites
green on the target for the first time with SRTP phase 2 included (knsrtp
77 checks), bundle 5060 B fully validated, PE 6.01, build.json extras
record the FFmpeg prefix. `decode.synthetic` EXECUTED (first ever real run
of kn_decode): rc=0, failed=0, fps≈3687, cpu 93.8% of one core over the
16 ms run, decode_ms_max=1 — but **frames=59 of 60** → the one FAIL
(cascading into `--selftest=all` rc=1).
**Root cause (measured, not guessed):** the clip itself is clean (ffprobe
`nb_read_frames=60`); the sandbox reproduced 59/60 EXACTLY against bookworm
FFmpeg 5.1 (same as the target's 63 — version-independent): `kd_run`
flushed the DECODER at EOF but never the PARSER, and `av_parser_parse2`
withholds the last access unit until the NEXT start code — which never
comes at end-of-input. Fix: NULL-input parser flush before the decoder
drain, both routed through a new shared `kd_send_drain` helper (identical
accounting on all three paths). Post-fix sandbox evidence: **60/60**, and
the contracts still hold — abort at frame 10 → rc=-4 with exactly 10
delivered, truncated 30 000 B input → rc=0, 40 frames, 0 failed, no crash.
**Phase B remainder delivered — `kn_d3d` module** (docs/D3D.md): the D7
session-start capability probe (`d3d.formats`: PCI id, driver version,
current mode + 60 Hz-at-res count (R9 evidence in every bundle), the 7-
format CheckDeviceFormat row, 4-pair conversion matrix, corrected
filter-caps masks, HWVP/SWVP CreateDevice behaviour) and **R25
milestone 0** (`d3d.present`: production-shaped 1280×720 NV12 DEFAULT →
lock → fill EVERY byte (pitch honoured) → full read-back verify →
StretchRect(LINEAR, letterbox (0,40,1280,760) computed) → Present ×30,
QPC-timed fill vs stretch+present separately, TCL after). Every call
pattern copied from T4 v4 — and the pitfall rule immediately earned its
keep: from-memory `CheckDeviceFormat` missed the AdapterFormat parameter
(macro-arity error under -Werror) and the D3DFMT enum values were checked
against d3d9types.h instead of recalled. **Crash guard: VEH + longjmp +
D7 permanent-degrade latch** — mingw GCC has NO `__try/__except` (verified
on both arches); the handler catches AV/IN_PAGE_ERROR only while armed,
records code + fault write-address, and the latch makes every later
in-process D3D call impossible (Release included). Registered landmines:
R29 (guard never executed on real D3D9 — round 5 is the first run) and
R30 (clock.resolution round-4 variance 15.50→2.75 ms degraded vs round 1's
1.25→1.00 — one loaded sample, thresholds stand, D9 pacing unaffected).
**knvstd hardening:** `--selftest=all` now runs test-by-test, logging
`selftest_begin` + each result IMMEDIATELY (INFO soft-flush §7.12) — an
uncatchable fault mid-run leaves the exact test name in diag.log (T4/R2b
rule applied to the runner). Relevance mapping extended:
video/d3d-flavoured failures re-run decode + both d3d probes into the
bundle (latch-safe: a tripped probe returns -3 instantly, no crash loop).
**T10 rev 4:** standalone `--selftest=d3d.present` FIRST (milestone-0
process isolation; a hard crash there cannot eat the rest of the
evidence), d3d verdict checks (`failed` fails T10 BY DESIGN — stage 1 must
not start on a broken video path), `decode.synthetic` must be **ok**
(60/60), stub count 5. Sandbox gates after all edits: native suites PASS
(test_knselftest 94 checks), cross `-Werror` PASS x64+i686 (kn_d3d +
kn_decode vs real FFmpeg 63 headers; PE imports d3d9/kernel32/msvcrt/
user32/winmm — all Win7-built-in), ctest 8/8, T10 POSIX dry-run ALL PASS
(31). Awaiting: T10 round 5 — first-ever execution of the d3d probes.
→ `streamer/{include/kn,src}/kn_d3d.*` (new), `docs/D3D.md` (new),
   `streamer/src/kn_decode.c` (parser flush), `streamer/src/kn_selftest.c`
   (d3d tests + relevance), `streamer/src/knvstd_main.c` (immediate
   logging), `streamer/tests/test_knselftest.c`, `CMakeLists.txt` +
   `tools/*.sh` (kn_d3d wiring), `tests/T10_build_and_test.py` rev 4,
   INTEGRATION.md rev 4, `tools/sandbox_decode_{check,edge}.c` (the
   sandbox repro/contract harnesses — Linux + distro FFmpeg only, not part
   of any gate), `artifacts/T10-round4/` (raw round-4 evidence, gitignored)
