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
# 3. Run tests/T10_build_and_test.py. It downloads the four small required
#    AES/SHA-1 source files into the ignored third_party/ tree and verifies
#    pinned SHA-256 hashes before configuring/building. No manual vendor setup.
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
sh tools/build_native_tests.sh     # host: 8 suites + knvstd smoke + zip/jsonl validation
sh tools/cross_check.sh            # mingw x64+i686, -Wall -Wextra -Werror, F1 flags
```

**The authoritative compile+run gate is the TARGET machine** (w64devkit GCC
16.2.0 — stricter than sandbox GCC 12; it catches warnings GCC 12 misses):
`python tests\T10_build_and_test.py` under ConEmu → `T10_wynik.txt` +
`T10_skrot.txt`. The user runs it quickly and willingly — ask for a
measurement instead of guessing (§10.7).

## Current status (updated 2026-10-23, post-T10 TLS backend work)

| Stage | State |
|---|---|
| **0 — measurements** | **COMPLETE.** G0.1 CPU ✅ · G0.2 D3D9 presentation ✅ **(T4 v4)** · G0.3 toolchain ✅ · G0.4 console ✅ |
| **1 — C skeleton offline + logging subsystem** | **Self-test registry phase A+B COMPLETE and target-verified (T10 round 7: ALL PASS 40/40 — the verification chapter is CLOSED: 7 unit suites green, 8 real self-tests incl. gs.parse 78 checks in the shipped binary, 4 honest phase-C stubs, d3d.present reproduced milestone 0 a second time: pitch 1536, present mean 0.30 ms). STAGE-1 VIDEO LOOP DELIVERED (round XII, sandbox-verified, awaiting target): `knvstd --loopback[=FILE.264] [--loopback-seconds=N]` — decode → paced 30 fps (absolute QPC deadlines, Sleep+spin per R30) → per-row copy into locked DEFAULT NV12 (pitch 1536 ≠ stride 1280) → StretchRect(LINEAR) → letterboxed window → §7.6 perf event every 1000 ms into perf.jsonl (CPU % of ALL cores per §6.1, per-interval deltas, net fields honest zeros). Presenter `kn_d3d_vp_*`: pre-fault at open (first-touch ~28 ms), VEH guard on every CPU write → D7 permanent-degrade latch, device-lost/ESC → graceful stop. New `kn_perf` (K32 counters via PSAPI_VERSION 2 — no psapi.dll import). New asset `streamer/assets/synthetic720.264` (High@5.1 VERIFIED at the SPS byte; landmine: `-preset ultrafast` silently downgrades to Constrained Baseline). New gate `tests/T11_loopback_g1.py` = G1-VIDEO: 10 evidence-computed criteria (30 fps steady, CPU ≤ 45 % all-cores, RSS drift ≤ +4 MiB, late ≤ 0.2 %, perf lines ≥ 95 %, DPST 0, logs WARN/ERROR-free, bundle contains perf.jsonl). Failure → `kn_log_on_failure` → §7.7 amplification + auto-bundle; relevance mapping catches `loopback`. Design: `docs/LOOPBACK.md`. **ROUND XIV COMPLETED THE STAGE-1 DELIVERABLES: WASAPI synthetic PCM (`kn_audio`, docs/AUDIO.md)** — audio opens with the presenter (open failure never kills video; §7.7 amplification + honest summary), `queue_audio_ms` is real device padding, 8th suite `test_knaudio` (22 checks, portable neutral half), T11 now gates FULL G1 (video+audio). G1-VIDEO already PASSED on target (round XIII: T11 600 s 20/20 — fps 30.000, late 0, CPU 13.0 %; overlay confirmed live in the round-XIII re-run: T10 40/40 + T11 quick 20/20 with F5 overlay working). **G1 CLOSED ON TARGET (round XV evidence): T10 round 10 ALL PASS 40/40 + T11 FULL = **G1 PASS 25/25** — fps_mean 30.000, frames_late 2/18 001, CPU 13.3 % all-cores (max-second 51.6 = clip-restart spikes), RSS 22.5→19.8 MB (peak 26.2, drift +2 607 kB), 601 perf lines (600/601 in [27,33]), DPST 0, logs WARN/ERROR-free, bundle 686 kB with perf.jsonl; AUDIO: opened **44100 Hz 2ch float32** (the machine's real mix format — the parser negotiated it, not us), 59 060 events, frames_written 26 462 842 = 100.006 % of realtime, **starved 0**, timeouts 0, buffer_errors 0, queue_audio_ms alive 601/601. **STAGE 1 IS DONE. STAGE 2 OPENED.** Round-XIV omission found+fixed: T10 built test_knaudio.exe but never RAN it (missing from the suites list) — added (round 11 expects 41 checks incl. test_knaudio -> PASS). **Round XV DELIVERED phase C: `kn_net` (schannel/winsock) + the four net self-tests — the §7.9 registry now has ZERO stubs (12/12 implemented).** net.dns/net.tcp443/net.tls/tls.chain probe login.nvidia.com + prod.cloudmatchbeta.nvidiagrid.net with a non-NVIDIA baseline per layer (DNS: stun.l.google.com; TCP/TLS: www.cloudflare.com) so "NVIDIA unreachable" is separable from "no network". TLS = schannel (the stage-2 client's real future stack), verification ALWAYS ON (dwFlags=0 + SNI; a failed handshake is the measurement, never bypassed), stock protocol defaults (grbitEnabledProtocols=0 — the Win7 TLS-1.2 reality is the question). T10 gating policy: dns+tcp443 gated ok/degraded (FAILED = stage-2 blocker → red on purpose), net.tls+tls.chain REPORTED (a Win7 TLS failure is a finding to analyse, not a client bug). Chain details (depth ≤6, per-element subject/issuer/dates/trust + SSL policy verdict) go to diag.log INFO + evidence summary. NEVER YET EXECUTED — round 11 on the target is the first run and a stage-2 planning input. Design/landmines: docs/NET.md. AU assembler stays queued for round XVI. |
| 2–5 | not started (stage-2 seeds shipped and target-verified: `kn_gs`, `knsrtp` ph1+2, `knfec`) |

**Latest target T10 (2026-10-23 21:18):** `ALL PASS`, 62/62 harness checks, zero compiler warnings, all 8 unit suites pass, D3D/decode self-tests pass, DNS and TCP/443 pass. TLS findings remain: all three Schannel handshakes fail with `0x80090302` (`SEC_E_UNSUPPORTED_FUNCTION`); `tls.chain` cannot handshake. This run used the default Schannel backend; it does NOT test mbedTLS.

**TLS backend work:** added `KN_TLS_BACKEND=mbedtls` as an optional static CMake subproject (mbedTLS 3.6.0), with bounded Win32 Winsock, TLS 1.2 minimum, SNI/hostname verification, `MBEDTLS_SSL_VERIFY_REQUIRED`, and read-only Windows ROOT-store imports. GCC 16 warning and source integration errors are fixed. The latest target trace localized TLS failure to TLS 1.3 ECDH: `psa_generate_key()` returned `-0x6c00` because PSA Crypto was not initialized before SSL use. Added `psa_crypto_init()` before CTR-DRBG/handshake and explicit logging if init fails. Sandbox mbedTLS build + ctest pass 10/10. This PSA fix is not yet target-tested; do not mark mbedTLS usable until fresh T12 verifies TLS/chain.

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
| **D20 (new, round XIII)** | **On-screen FPS counter overlay, low-overhead, toggleable by key (F5).** 5x7 bitmap font blitted straight into the locked NV12 surface during the per-frame copy (no D3DX, no GDI, no lockable backbuffer — zero new imports, ~1.5 k pixel writes/frame, inside the VEH-guarded section); shows the SAME number perf.jsonl reports (frames presented per completed 1-s interval). Default ON in `--loopback` (debug surface); production streamer defaults OFF with the same toggle (stage-2 wiring). | User requirement 2026-10-23 ("niskonarzutowy fps counter ze switchem pod klawiszem — dobrze wiedziec, ile fps generuje streamer"); implementation choice follows the measured D3D9 constraints (G0.2: offscreen conversion targets refused; d3dx9 not OS-built-in on Win7) | user preference |
| **D19** | **Toolchain stays w64devkit** (GCC 16.2.0 + cmake 4.4.3 + ninja, `C:\Programy\w64devkit`). NOT WinLibs, NOT Nuwen, NOT Cygwin. | It is measured end-to-end on the target: T2 (15 variants ran on Win7), T9/T10 (zero-warning builds, correct PE 6.01, imports only KERNEL32/msvcrt/WINMM), FFmpeg built with it works. Cygwin is disqualified by design (POSIX runtime layer, not native Win32). WinLibs-MSVCRT/Nuwen are equivalent-ish but unproven here — switching re-opens R6 for zero gain. | Only if w64devkit develops a blocking defect; WinLibs **MSVCRT** variant (never UCRT — no UCRT on stock Win7) is the designated fallback, re-run T2 before trusting it |

## Risk register — changes this session

| # | Change |
|---|---|
| R2 | **CLOSED** (T4 v4: NV12 path works; crashing variant isolated and excluded) |
| R2b | **CLOSED** (v4 writer preserved all data through a crashing run — pattern proven; the same rules ship in the client per §7.12) |
| R9 | **Mitigated** (60 Hz available and active; user keeps 60 Hz on battery; D9 stands) |
| R17 | **CLOSED** (GPU does CSC + scale; CPU never touches pixels) |
| **R25 (impl round X: CLOSED)** | **CLOSED by T10 round 5 on the target:** the locked-DEFAULT-NV12 fill works — 1280×720 surface, **pitch 1536 (≠ width!)**, 1 658 880 B filled + read-back verified per iteration, 30/30 iterations, 0 mismatches, no AV, VEH never fired, standalone AND in-process runs ok. Stage-1 video path unblocked. Two new hard facts (docs/D3D.md "Target evidence"): per-row copy mandatory (pitch 1536 vs kn_decode's 32-aligned stride) and first-touch costs ~28 ms (pre-fault the surface before the paced loop). |
| **R29 (round X: happy path verified, catch path UNVERIFIED)** | mingw GCC has NO `__try/__except` (verified both arches) — the crash marker is `AddVectoredExceptionHandler` + `longjmp`. Round 5: the guard armed/disarmed cleanly 4× on the target (2 runs × 2 probes) and never fired — happy path proven on real D3D9. The catch path itself (longjmp out of an AV) **cannot be provoked without risking the driver — stays UNVERIFIED by design**; if it ever fires in the field, the latch + standalone-process order (T10 milestone run first) contain the damage. |
| **R26 (REVISED, impl round VI/VII)** | The agent sandbox **DOES have a working toolchain** (the round-II "apt fails" era is over): `apt-get update && apt-get install -y gcc cmake ninja-build libc6-dev gcc-mingw-w64-x86-64 gcc-mingw-w64-i686` (~2-4 min). **Packages do NOT persist between chat messages** — reinstall at the start of every session (see Bootstrapping). If apt lists hang, fall back to direct .deb downloads from `deb.debian.org/debian/pool/` + `dpkg -i` (proven: libc6-dev must match installed libc6 `2.36-9+deb12u14`; ninja must be bookworm `1.11.1`, NOT trixie `1.13.2` which needs GLIBC_2.38). Sandbox GCC is 12.2 — the TARGET's GCC 16.2 remains the authority (it catches `-Wformat-truncation` classes GCC 12 misses; see R28). Every C delta still ships through `build_native_tests.sh` + `cross_check.sh` + a target T10 run. |
| **R27 (new, CLOSED by D18)** | `-static` (F1/D8) vs shared FFmpeg: ld ignores `.dll.a`, error text is misleading ("have you installed the static version?"). Reproduced 1:1 in the sandbox. Mitigation shipped: CMake configure-time FATAL_ERROR with the rebuild recipe when `lib/libavcodec.a` is missing; FFmpeg rebuilt static per D18. |
| **R28 (new)** | Sandbox GCC 12.2 vs target GCC 16.2 warning drift: T10 round 1 failed on two `-Wformat-truncation` GCC 16 flags that GCC 12 accepted (timestamp formatters). Mitigation: all `snprintf` with time/foreign-string inputs use **explicitly bounded arguments** (modulo/precision), verified at `-O0..-O3`; T10 fails the build on ANY warning so drift can never ship silently. Round IX re-confirmed the discipline (cross `-Werror` caught a 64-bit format promotion in the new d3d code on the first pass). **Round XI third incident:** GCC 16 flagged the new `st_gs_parse` capnote (worst case 284 B > 256 B buffer, sandbox GCC 12 default level silent) — fix verified under `-Wformat-truncation=2` (paranoid level) on BOTH toolchains; that flag level is now the pre-check for any new snprintf aggregation before handover. |
| **R30 (round X: stable pattern, informational)** | `clock.resolution` variance is NOT transient load: rounds 4+5 both measured Sleep(1) 15.6→**2.75/2.88** ms (verdict `degraded`, threshold 2.6 from round 1's 1.25→1.00) with wall tick pinned at 15600 us **even after timeBeginPeriod(1)** (round 1 saw 1250 us ticks — the machine state changed between rounds, probably a reboot cleared whatever had raised the timer). Consequences: (a) treat **~2.9 ms as the realistic Sleep(1) floor** on this machine — enough for 33.3 ms frame pacing at 30 fps, NOT enough for precise 1 ms spins; (b) wall-clock granularity stays 15.6 ms — timestamps below that resolution are quantised; (c) D9 stands stronger than ever: pacing measures real Present intervals, never assumes. Thresholds unchanged (honest `degraded` beats a fudged `ok`). |
| **R15 (impl round II)** | **Mostly closed.** T8 on the target resolved all 14 Python packages rc=0 (pip honours Requires-Python, so every pin declares 3.8 support): textual 6.2.1, rich 14.3.4, httpx 0.28.1, httpcore 1.0.9, h11 0.16.0, anyio 4.5.2, + 8 more — `python/requirements.txt` updated, wheels cached on target in `tests\t8_wheels\`. Remaining: offline install + runtime smoke at stage 4 (httpx 0.28 removed `proxies=`; textual 6.x API churn vs old docs). |
| **R31 (updated)** | Sandbox has host GCC 13.3 but no MinGW/Windows SDK headers (apt install denied). Target T12 confirms the mbedTLS Win32 source compiles/links with zero warnings, DNS/TCP work, and localized TLS 1.3 ECDH failure to missing PSA initialization. `psa_crypto_init()` is now called before TLS; target T12 must verify handshake and certificate validation before acceptance. |

Everything else (R3 usrsctp/DTLS spike, R7 first real `POST v2/session`, R22 31-field
session context, R23 RESUME, R24 catalog revision binding) unchanged — see BOOTSTRAP §9.

## Next steps (ordered)

1. **Verify PSA init and mbedTLS handshake.** Sync the latest `kn_net_mbedtls.c` patch to the laptop and rerun `set KN_TLS_BACKEND=mbedtls` + `python tests\T12_network.py`. The previous trace showed PSA uninitialized during TLS 1.3 ECDH; `psa_crypto_init()` is now called before use. DNS/TCP already pass and the Win32 source builds cleanly. Require all three `net.tls` hosts to verify and `tls.chain` to be clean before stage 2.
2. If the mbedTLS target build or handshake fails, use `T12_wynik.txt` and `t12logs/diag-*.zip`; diagnose this backend only. Do not modify Windows settings or disable certificate verification. Run full T10 after the TLS backend passes or when changes touch non-network build/runtime behavior.
3. **AU assembler + reorder buffer on `kn_gs`** (round XVI, docs/GS.md
   "Stage-2 TODOs"): VideoAccessUnitAssembler port (GsFrameHeader,
   expected_access_unit_length, AU caps), reorder + continuity +
   PLI/NACK hooks, FEC recovery (pitfall #4) — offline, KAT-verified via
   gs_reference.py simulation.
4. Stage 2 sockets: Mjolnir UDP receive loop → kn_ring capture →
  `--capture`/`--decode` modes; WSS+RTSP client using the validated TLS
  backend; G2 gate.
5. Stage 3+ per BOOTSTRAP §12 (R3 spike → TUI → hardening). R22/R23
   mandatory at their stages.

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
│   │                              + TARGET EVIDENCE (round 5: pitch 1536!)
│   ├── GS.md                   ← kn_gs provenance + vector pipeline + stage-2 TODOs
│   ├── AUDIO.md                ← kn_audio (WASAPI) provenance + landmines
│   ├── NET.md                  ← kn_net phase-C probes (schannel) + verdict table
│   ├── LOOPBACK.md             ← stage-1 video path design + G1 gates
│   ├── analysis/               ← measurement analyses (G0.2 closed here)
│   └── research/               ← BOOTSTRAP.md + research HANDOFF (copies)
├── streamer/                   ← knvstd (C11)
│   ├── include/kn/             ← knlog knring knfec knsrtp kn_aes kn_sha1
│   │                              kn_diag kn_selftest kn_decode kn_d3d kn_gs
│   │                              kn_perf kn_audio kn_net
│   ├── src/                    ← implementations + GENERATED: knfec_tables.h,
│   │   │                          knfec_kat_vectors.h, knsrtp_kat_vectors.h,
│   │   └                          kn_gs_kat_vectors.h, kn_synthetic_clip.h
│   ├── assets/synthetic.264    ← decode.synthetic KAT clip (embedded via tools/)
│   ├── assets/synthetic720.264 ← loopback/G1 source: 720p High@5.1, NOT embedded
│   └── tests/                  ← portable unit tests (run on Linux AND Win7):
│                                  knlog knring knfec knsrtp kndiag knselftest
│                                  kngs knaudio
├── python/                     ← TUI (stage 4); requirements.txt RESOLVED via T8
├── tests/                      ← target-machine harnesses T1-T11
│                                 (T9 = old build gate; T10 = build+selftests;
│                                  T11 = the G1-video loopback gate)
├── tools/                      ← build/verify scripts + KAT generators
│   (build_native_tests.sh, cross_check.sh, check_jsonl.py, check_zip.py,
│    fec_reference.py, srtp_reference.py, gs_reference.py,
│    make_synthetic_clip.py, sandbox_decode_{check,edge}.c,
│    sandbox_loopback_source.c)
├── third_party/                ← GITIGNORED: tiny-aes-c, crypto-algorithms-sha1
│                                  (+ ffmpeg-headers in the agent sandbox only)
└── CMakeLists.txt              ← Windows build (w64devkit: cmake + ninja);
                                   KN_FFMPEG_PREFIX option, git/flags → build.json
```

Maps onto the user's repo `C:\Users\KarnyJohnny\Desktop\NewKarnyNow\`.

## Working agreements (carried over, still binding)

- **CONTINUITY RULE (user instruction, 2026-10-23):** at the END OF EVERY
  TASK/ROUND — before presenting results — update THIS file (Current status,
  Risk register, Next steps, Session log) so that a FRESH agent session can
  pick up the work immediately from the repo alone, with no chat history.
  This is the recovery path against chat overflow, network failures and
  session loss. The repo is the source of truth; the user keeps GitHub
  current (the agent cannot push — deliver files in the workspace + a
  change list).
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

### 2026-10-23 · impl round XV (T10 network run)
The target T10 run completed with a clean build and a green self-test set: configure/build rc=0, zero warnings, 8 unit suites passed, `knvstd --selftest=all` exit 0, `d3d.present` milestone 0 passed, and the diagnostic bundle was generated. The sole environment-level finding was `net.tls` and `tls.chain` failing on Win7/Schannel across all tested configs (`0x80090302` / `0x80090326`, alert `2/70`), while `net.dns` and `net.tcp443` remained OK. Interpretation: stage-2 network/input issue (Win7 TLS stack, service pack / cipher policy / cert chain), not a build regression.

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

### 2026-10-23 · impl round X (T10 round 5: ALL PASS 38/38 — R25 CLOSED; then gs.parse)
**T10 round 5 on the target: VERDICT ALL PASS (38 checks, 0 failed).**
Everything round IX shipped is now target-verified: the parser-flush fix
(`decode.synthetic` **ok 60/60** at 3157.9 fps, cpu 84% of one core, worst
frame 1 ms — round 4's 59/60 is gone), both d3d probes (**d3d.formats ok**
10 checks; **d3d.present ok** 11 checks — standalone milestone-0 run AND
inside `--selftest=all`, 30/30 iterations, fill read-back mismatches=0,
every HRESULT S_OK, VEH armed but never fired, latch false), the knvstd
crash-survivor logging (begin markers visible in the committed diag.log),
T10 rev-4 checks (38 total), test_knselftest 97 on Win32 (94 on host —
platform deltas expected). **R25 CLOSED. R29 happy path verified (catch
path stays UNVERIFIED by design). Phase B complete except gs.parse.**
New hard facts (full table in docs/D3D.md "Target evidence"):
(1) **pitch 1536 for 1280×720 NV12 DEFAULT** — NOT width (T4 saw pitch==width
at 960×540/1280×800; alignment depends on dimensions unpredictably) →
stage-1 loopback copies NV12 **per row** (kn_decode's 32-aligned stride vs
surface pitch), never one memcpy; the milestone passed BECAUSE it honoured
the returned pitch. (2) **first-touch fill ≈ 28 ms once** (mean 2.13) —
stolen UMA memory page-fault cost → pre-fault the surface during setup.
(3) present mean **0.28–0.43 ms** for the 1:1 720p letterbox (G0.2's 3.0 ms
was a scaled stretch) — GPU side has ~10× headroom vs the 33.3 ms budget.
(4) **G0.2 §2.1 CORRECTION: SWVP CreateDevice actually returns S_OK** when
attempted sequentially after releasing the HWVP device — T4 never really
attempted it after HWVP success (its E_FAIL was the init value). Production
rule unchanged (HWVP, D7). (5) clock.resolution degraded twice in a row
(15.6→2.75/2.88 ms, wall tick pinned 15600 even after timeBeginPeriod) —
R30 reclassified as the machine's stable state: ~2.9 ms is the realistic
Sleep(1) floor; timestamps quantise at 15.6 ms; D9 pacing unaffected.
**New binding working agreement (user instruction): the CONTINUITY RULE** —
every round must leave this file updated as a complete handoff BEFORE
presenting results (recovery path against chat overflow/network loss).
Workspace synced to GitHub `4c47794` (user committed round IX verbatim +
round-5 t10logs evidence); raw round-5 files archived in
`artifacts/T10-round5/`, nvst.rs @ a49b124 re-fetched to
`artifacts/nvst-extract/` (sha256 f52fc09f…, 14 695 lines — matches the
round-VI verification).
Then: **gs.parse + kn_gs DELIVERED (the stage-2 seed).** New module
`kn_gs` (pure, zero-allocation): RTP header/CSRC/extension/padding parse
with the reference's exact error precedence, the 16-byte LE GS body
(pitfalls #1/#2 encoded as first-class fields), Annex-B payload location
(68-byte search window, windows(3) semantics preserved), keyframe
detection, trailing-AUD strip (incl. the zero-MERGE case: a body ending in
00 fuses with a following 00 00 01 into a 4-byte code — vector pinned),
FEC block layout (div_ceil parity, all five rejection rules), stream-index
continuity (2^24 wrap), and kn_ring_dump hexdump line parsing (strict
single-space format, cap-after-validation ordering). Every fact re-verified
against nvst.rs @ a49b124 THIS round (RtpHeader::parse 2456-2510,
NvVideoPacket 3228-3257, payload 3481-3497, start-code 3525-3534, keyframe
3500-3523, AUD 3458-3479, FecPacketLayout 3677-3701, flags 4089,
continuity 4813-4824) — docs/GS.md carries the full table. Generator
`tools/gs_reference.py` self-proved before emitting (hand goldens,
LE-vs-BE and ext[11] discriminators, 700 round-trips) and caught 10 of its
own bugs first (w32be profile bug, six wrong eof expectations, the AUD
merge case, an out-of-range FEC shard, two no-op truncations). Emitted
battery: **78 vectors**; host suite `test_kngs` (SEVENTH suite): **105
checks** incl. C-side property round-trips (a second independent encoder).
`gs.parse` self-test: battery from the shipped binary (~1 ms, portable —
host AND target) + optional real capture (`KN_GS_CAPTURE` env or
./gs_capture.hex; parsed-lines/RTP/GS/SOF/EOF/FEC counters in evidence;
absence reported honestly). Relevance mapping: video failures now also
re-run gs.parse; landmine recorded — bare "rtp" keyword substring-matches
"srtp" (caught by the host suite before shipping). Stubs left: 4. Sandbox
gates after the delta: native PASS (7 suites: 105+34+125+77+109+97+105 =
652 checks), cross -Werror PASS x64+i686 (-O0..-O3 clean for kn_gs),
ctest 9/9, T10 POSIX dry-run ALL PASS (33). Awaiting: T10 round 6 (target
run of the gs battery + the new suite).
→ `streamer/{include/kn,src}/kn_gs.*` (new), `streamer/src/kn_gs_kat_vectors.h`
  (generated, new), `tools/gs_reference.py` (new), `streamer/tests/test_kngs.c`
  (new), `docs/GS.md` (new), `streamer/src/kn_selftest.c` (gs.parse +
  relevance), `streamer/include/kn/kn_selftest.h`, `streamer/tests/test_knselftest.c`,
  `CMakeLists.txt` + `tools/{build_native_tests,cross_check}.sh` (7th suite),
  `tests/T10_build_and_test.py` (kngs + gs.parse checks, min-stub 4),
  INTEGRATION.md rev 5, docs/D3D.md (target-evidence section)

### 2026-10-23 · impl round XI (T10 round 6: 39/40 — quick fix: R28 third incident)
**T10 round 6 on the target: 39/40 PASS.** Everything round X shipped is
target-verified: `test_kngs` PASS on Win7 (first run), **`gs.parse` ok with
78 checks in the shipped binary** (0 ms), d3d probes ok again (present
mean 0.34 ms, pitch 1536 reproduced), decode 60/60 at 3529 fps, 4 stubs
skipped, bundle 12 393 B validated, clock.resolution stable-degraded
(2.88 ms — third consecutive run, R30 pattern holds).
**The single FAIL was the zero-warning gate**: GCC 16.2 flagged
`-Wformat-truncation` in the NEW `st_gs_parse` capnote snprintf — the
`%u` directives are modulo-bounded (GCC even printed the [0,999999]
range) but `capnote[256]` was too small for the proven worst case
(107 literal + 128 path + 8×6 digits + NUL = 284). Sandbox GCC 12 at the
DEFAULT level did not flag it — **R28 third incident, this time in my own
round-X code; the two-tier gate worked exactly as designed** (target
caught what the sandbox missed; nothing functional was ever at risk).
Fix: `capnote[320]` + a comment with the worst-case arithmetic. Verified
BEYOND the target's bar: clean under `-Wformat-truncation=2` (paranoid
level) on host GCC 12 at -O0..-O3 AND mingw x64 -Werror — the two
remaining =2-only float directives (`%.1f fps` in decode/d3d reasons) are
level-2 over-approximations of lines that ran zero-warning under GCC 16 in
rounds 5-6; target gate is GCC 16 default level, so they stay untouched
(don't churn verified code on a stricter-than-production analysis).
Also clarified the T10 informational line that read like a warning:
"no FFmpeg DLLs next to knvstd.exe — static build (D18): knvstd links
lib/*.a directly, there is nothing to deploy (EXPECTED, not a warning)"
(the user's dev tree having no DLLs anywhere IS the correct static layout).
Sandbox gates after the fix: native 7 suites PASS (652 checks), cross
-Werror PASS x64+i686, T10 dry-run ALL PASS (33). Awaiting: T10 round 7 —
expected ALL PASS 40/40, then stage 1 `--loopback` is the next real work.
→ `streamer/src/kn_selftest.c` (capnote[320]), `tests/T10_build_and_test.py`
  (message wording), this file, INTEGRATION.md rev 5b

### 2026-10-23 · impl round XII (T10 round 7: ALL PASS 40/40 — then stage-1 video loopback delivered)
**T10 round 7 on the target: VERDICT ALL PASS (40 checks).** The capnote fix
closed the last warning (zero-warning gate green on GCC 16.2 again);
`test_kngs` passed on Win7 and `gs.parse` ran its 78-check battery from the
shipped binary; d3d.present reproduced milestone 0 a second time (30/30,
pitch 1536, present mean 0.30 ms max 5.08, fill mean 2.08); decode 60/60
at 3157.9 fps; clock.resolution stable-degraded (2.88 ms — fourth
consistent sample, R30 pattern). **The entire §7.9 phase-A+B verification
chapter is closed; 4 phase-C stubs remain (net/tls, gated on stage 2).**
Then implemented the **stage-1 video half** (docs/LOOPBACK.md is the design
record):
- **`kn_d3d_vp_*` presenter** grown on the probe code: open (T4-proven
  window/device parameters + D7 conv check + NV12 DEFAULT surface +
  PRE-FAULT fill&present absorbing the ~28 ms first touch), present_nv12
  (TCL check → lock → **per-row copy honouring pitch 1536 vs stride 1280**
  → StretchRect(LINEAR) → Present, timed copy vs stretch+present
  separately), pump (WM_CLOSE/ESC → graceful), refresh_hz (DPST detection),
  close. Every CPU-write section under the VEH guard; a caught exception
  latches the D7 permanent degrade and leaks D3D objects ON PURPOSE.
- **`kn_perf`** (§7.6 system counters): process/system CPU % OF ALL CORES
  (§6.1 convention) from GetProcessTimes/GetSystemTimes deltas, RSS+peak
  via K32GetProcessMemoryInfo (PSAPI_VERSION 2 — kernel32 only, the F1
  import set stays kernel32/user32/winmm/d3d9/msvcrt), handles, toolhelp
  thread count, timeGetDevCaps.
- **`knvstd --loopback`**: single-threaded by design (decode 211 fps vs
  30 fps pace — no queue needed; queue_video_depth honestly 0), pacing
  INSIDE the decode callback on absolute deadlines (t0+n·1e6/30, integer —
  errors never accumulate; Sleep(deadline−3ms)+QPC spin per R30), late
  grace 2 ms, source discovery explicit→cwd→exe-dir→embedded (embedded
  fallback reports budget_valid:false + WARN), clip loops until
  --loopback-seconds (default 600 = G1). One §7.6 perf event per 1000 ms
  (26 fields ≤ KN_LOG_MAX_FIELDS 32; stable schema with honest zeros for
  stage-2 groups). stdout: single-line summary JSON (fps_mean, late, cpu
  mean/max, rss first/last/peak, refresh initial/last, dpst, perf_lines,
  stopped_reason, fault/fault_addr, auto-bundle path). Failure →
  kn_log_on_failure("session.loopback", …) → §7.7 amplification +
  auto-bundle (kn_diag_install); the relevance mapping learned "loopback".
- **Asset** `streamer/assets/synthetic720.264`: 136 853 B, sha256
  870bdf29…, High@5.1 1280×720 yuv420p 120f GOP30 bf0 (verified at the
  SPS byte: profile_idc=100). **Landmine: x264 `-preset ultrafast`
  SILENTLY downgrades High→Constrained Baseline even with explicit
  `-profile:v high`** (CABAC/8x8dct off) — a Baseline clip would have made
  the G1 CPU gate lenient vs the High@5.1 budget it verifies; superfast
  keeps High. Not embedded (T11 passes the path; discovery + honest
  fallback in knvstd).
- **`tests/T11_loopback_g1.py`**: the G1-VIDEO gate — 10 criteria computed
  from the evidence (summary + perf.jsonl + logs + post-run bundle
  incl. "bundle CONTAINS logs/perf.jsonl"); --quick 30 s smoke; POSIX
  honest skip. WASAPI is round XIII; T11 then extends to full G1.
Sandbox gates: native 7 suites PASS (652 checks), cross -Werror PASS
x64+i686 (knvstd_main additionally clean under -Wformat-truncation=2 at
-O0/-O2/-O3 — R28 pre-check now standard), ctest 9/9, T10 dry-run ALL PASS
(33), T11 posix-skip verified, POSIX --loopback honest refusal rc=3, and
`tools/sandbox_loopback_source.c` proved the 720p source decodes 120/120
through kn_decode_h264_file (NV12, strides sane). Self-review caught two
bugs before the gates: frame-0 always-late (deadline==t0; fixed with the
2 ms grace) and the missing "loopback" relevance keyword. Awaiting: T10
round 8 + T11 quick + T11 full (G1-VIDEO).
→ `streamer/src/knvstd_main.c` (--loopback), `streamer/{include/kn,src}/kn_perf.*`
  (new), `streamer/{include/kn,src}/kn_d3d.*` (presenter),
  `streamer/assets/synthetic720.264` (new), `tests/T11_loopback_g1.py` (new),
  `docs/LOOPBACK.md` (new), `tools/sandbox_loopback_source.c` (new),
  `CMakeLists.txt` + `tools/{build_native_tests,cross_check}.sh` (kn_perf),
  `.gitignore` (T11 outputs), INTEGRATION.md rev 6

### 2026-10-23 · impl round XIII (T10 round 8 ALL PASS + **T11 G1-VIDEO PASS** → FPS overlay D20)
**T10 round 8 on the target: ALL PASS 40/40** (loopback delta builds
warning-free; all suites + self-tests green; clock.resolution reported
**ok** this run — Sleep(1) ≤ 2.6 ms; R30 variance swings both ways with
machine state, thresholds unchanged).
**T11 FULL G1-VIDEO run on the target: VERDICT G1-VIDEO PASS (20/20
checks).** The stage-1 video path held for the whole 600 s: fps_mean
**30.000**, frames_late **0** of 18 001 presented, 151 clip loops,
cpu_process_pct mean **13.0 %** of all cores (max single second 51.8 —
the clip-restart+IDR spikes), RSS first→last **20.9→18.8 MB** (peak 24.2,
drift check +2 855 kB ≤ +4 096), decode_ms mean 3.41/max 14.97,
present_ms mean 1.16 (copy 0.90 + stretch/present ~0.26), refresh 60 Hz
start=end, DPST 0, perf lines 601, logs zero WARN zero ERROR, post-run
bundle 719 kB containing logs/perf.jsonl. **Honest caveats recorded:**
(1) the synthetic testsrc2 clip is skip-heavy (x264: 94 % skip MBs) so
loopback CPU UNDERSTATES a real stream — the production budget reference
stays T1's 720p30 High@5.1 = 24.8 % (decode) / 28.9 % (+NV12), still far
inside the ≤45 % gate; real numbers arrive with stage 2 + G2. (2) The run
analysed 632 perf lines = 601 (full) + 31 (the earlier --quick smoke) —
t11logs accumulates across runs; **T11 now wipes t11logs at start** so the
record run is deterministic. G1 remains gated on WASAPI (round XIV);
G1-VIDEO is CLOSED.
**NEW user requirement → D20: low-overhead on-screen FPS counter with a
toggle key (F5)** — implemented this round in the loopback presenter:
5×7 bitmap font blitted directly into the locked NV12 surface during the
per-frame copy (no D3DX/GDI/lockable-backbuffer — zero new imports;
~1.5 k pixel writes per frame inside the VEH-guarded section; background
box + white glyphs, even coordinates only so the 2×2 UV subsampling maps
exactly at scale 2). The overlay shows the frames-presented-per-second of
the completed perf interval — the same number perf.jsonl carries. F5
toggles (wndproc), the orchestrator refreshes the text once per second
("FPS 30.0", plus " LATE n" when the interval dropped frames). Font+blit
live in a platform-NEUTRAL section of kn_d3d.c (kn_d3d_overlay_text_nv12)
so the sandbox can render ASCII proofs of every glyph (verified this round
before handover). Loopback default: overlay ON (debug surface); the
production streamer will default OFF with the same toggle (stage 2).
Awaiting: T10 round 9 + T11 (quick suffices — the G1-VIDEO verdict stands
until stage-2 changes land; the overlay is additive).
→ `streamer/{include/kn,src}/kn_d3d.*` (overlay font/blit + F5 + text API),
  `streamer/src/knvstd_main.c` (per-second overlay text),
  `tests/T11_loopback_g1.py` (t11logs wipe), `tools/sandbox_overlay_font.c`
  (ASCII glyph proof, sandbox-only), docs/LOOPBACK.md (overlay + keys),
  INTEGRATION.md rev 6b, this file (D20 + round XIII)

### 2026-10-23 · impl round XIV (T10 r9 40/40 + T11-overlay 20/20 on target → WASAPI synthetic PCM delivered: STAGE 1 CODE-COMPLETE)
**User-confirmed on the target:** T10 round 9 ALL PASS 40/40 (clock
resolution back to `ok` — Sleep(1) ≤ 2.6 ms; R30 swings with machine
state, thresholds stand) and T11 quick with the D20 overlay: G1-VIDEO
PASS 20/20 (fps_mean 30.001, late 0, CPU 13.9 %, RSS drift −1 952 kB,
31 perf lines from the wiped dir — the T11 cleanup works). User verdict:
"działa idealnie". GitHub @ `50f5291` (partial commits — the workspace
remains the delivery medium).
**Round XIV delta — `kn_audio` (WASAPI synthetic PCM), the last stage-1
deliverable** (BOOTSTRAP §12: "Plus WASAPI with synthetic PCM"; design +
provenance + landmines in docs/AUDIO.md): shared-mode event-driven render
client on the default endpoint at the device MIX format; dedicated refill
thread (WaitForSingleObject → GetCurrentPadding → GetBuffer → generate →
ReleaseBuffer); soft 440 Hz tone (~−18 dBFS) or silence; counters
events/frames_written/starved/wait_timeouts/buffer_errors/queue_ms.
Portable half (host-tested, `test_knaudio` 22 checks): the
WAVEFORMATEX(WSEXTENSIBLE) byte parser (float32/s16/s32; honest UNKNOWN
otherwise) + the phase-continuous generator (split calls byte-identical to
one big call — test-proven). Landmines hit and recorded: mingw
`audioclient.h` gates the C-style COM macros behind **`COBJMACROS`**
(d3d9.h does not); `INITGUID` in exactly one TU → **no -luuid**, the only
new PE import is ole32.dll (OS-built-in); **Start() BEFORE CreateThread**
(a thread waiting before Start burns a 200 ms timeout into the counter).
Integration: audio opens with the presenter in `--loopback` (open failure
never kills video — §7.7 amplification + `"audio":{"opened":false,…}` in
the summary; T11 gates full G1 on it), `queue_audio_ms` in the §7.6 perf
line is now REAL device padding, `--no-audio` for debugging, summary
carries the full audio block incl. expected_frames = rate × seconds_run.
**T11 upgraded to the FULL G1 gate**: +5 audio checks (opened, starved ≤5,
timeouts+buffer_errors ≤5, frames_written ≥95 % of realtime,
queue_audio_ms alive ≥80 % of perf lines); verdict renamed **G1 PASS**.
Gate story worth keeping: the s16 test initially read past the written
region into uninitialized stack — passed at -O1, FAILED at -O3+NDEBUG
under ctest; fixed with an exact-sized poison-filled buffer (different -O
levels catch different lies — R28's sibling, on the test side).
Sandbox gates after the delta: native **8 suites PASS (674 checks)**,
cross `-Werror` PASS x64+i686 (test_knaudio cross-links knlog for the
engine's logging), ctest **10/10**, knvstd+kn_audio clean under
`-Wformat-truncation=2` at -O0/-O2/-O3, T10 dry-run ALL PASS (33), T11
posix honest-skip, PE imports: d3d9/kernel32/msvcrt/**ole32**/user32/winmm
— all Win7-built-in. The WASAPI engine itself has NEVER EXECUTED (no audio
stack in the sandbox) — labelled as such everywhere; T11 on the target is
its first run. **STAGE 1 IS CODE-COMPLETE; the G1 verdict needs one full
T11 run.**
→ `streamer/{include/kn,src}/kn_audio.*` (new), `streamer/tests/test_knaudio.c`
  (new), `docs/AUDIO.md` (new), `streamer/src/knvstd_main.c` (audio wiring +
  --no-audio), `tests/T11_loopback_g1.py` (full-G1 checks), `CMakeLists.txt`
  + `tools/{build_native_tests,cross_check}.sh` (8th suite, ole32/m),
  docs/LOOPBACK.md (audio status), INTEGRATION.md rev 7, this file

### 2026-10-23 · impl round XV (G1 CLOSED on target → phase-C network probes delivered: registry 12/12, zero stubs)
**G1 CLOSED — the stage-1 gate passed on the target (T11 FULL, 20/20 +
T10 r10 40/40; user: "działa idealnie").** Numbers of record: video
fps_mean **30.000**, frames_late **2**/18 001 (0.011 %), CPU **13.3 %** of
all cores (max second 51.6 % = clip-restart+IDR spikes), RSS 22.5→19.8 MB
(peak 26.2, drift +2 607 kB ≤ +4 096), 601 perf lines (600/601 within
[27,33]), present_ms mean 1.17 (copy 0.91), decode_ms mean 3.52/max 16.34,
DPST 0, refresh 60→60, logs zero WARN/ERROR, bundle 686 kB with
perf.jsonl. AUDIO: opened at the machine's real mix format **44100 Hz 2ch
float32** (the portable parser negotiated it — not hard-coded 48k),
59 060 refill events, frames_written **26 462 842 = 100.006 %** of
realtime, **starved 0**, timeouts 0, buffer_errors 0, queue_audio_ms alive
in **601/601** perf lines (last 89 ms of a 100 ms buffer). Caveat kept:
the synthetic clip decodes ~5× cheaper than T1's real 720p30 content —
production budget reference stays §6.1 (28.9 %), real numbers at G2.
**Round-XIV omission found+fixed:** T10 BUILT test_knaudio.exe but never
RAN it (suites list) — added; round 11 expects 41 checks.
**Round-XV delta — phase C (`kn_net` + four self-tests; docs/NET.md):**
schannel TLS probes (handshake loop with EXTRA_DATA carry, alert
send-on-failure, 64-iter cap), winsock DNS/TCP443 with per-layer
non-NVIDIA baselines, cert-chain walk + CERT_CHAIN_POLICY_SSL verdict for
login.nvidia.com (the §7.9 stale-store diagnostic). Verification never
disabled; stock protocol defaults measured as-is. T10 gates dns/tcp443
(blocker findings turn it red on purpose) and REPORTS net.tls/tls.chain
(a Win7-TLS failure is a research finding to analyse, not a client bug).
Landmines hit in the sandbox (all recorded in docs/NET.md): sspi.h
requires **SECURITY_WIN32**; `????-??-??` in a string literal is a
**TRIGRAPH** under -std=c11 (-Werror=trigraphs); winsock2-before-windows;
mingw's SSPI entry points need explicit `A` suffixes; SP_PROT_TLS1_1/2/3
client bits missing from mingw's schannel.h (stable ABI values defined
locally, decode-only). Self-review caught the `k_net_nvidia[0].rc`
type-slip before the gates. Sandbox: native 8 suites PASS (674 checks),
cross -Werror PASS x64+i686, kn_selftest+knvstd clean under
-Wformat-truncation=2 (-O0/-O2/-O3), ctest 10/10, T10 dry-run ALL PASS
(38 — all four probes honestly skipped on POSIX), PE imports verified:
+crypt32/secur32/ws2_32 (all Win7-built-in). Awaiting: **T10 round 11 —
the first real execution of the phase-C probes; its net.tls/tls.chain
output is a stage-2 planning input** (Win7 TLS ceiling + cert-store gaps
feed the WSS/RTSP client design and R7).
→ `streamer/{include/kn,src}/kn_net.*` (new), `docs/NET.md` (new),
  `streamer/src/kn_selftest.c` (4 real probes, stubs extinct),
  `streamer/include/kn/kn_selftest.h`, `streamer/tests/test_knselftest.c`,
  `tests/T10_build_and_test.py` (8 suites + net gating policy),
  `CMakeLists.txt` + `tools/{build_native_tests,cross_check}.sh`
  (kn_net + ws2_32/secur32/crypt32), INTEGRATION.md rev 8

### 2026-10-23 · optional mbedTLS backend after target T10
Target T10 from the user: 62/62 harness checks ALL PASS; Schannel TLS
findings persist (`net.tls` failed on NVIDIA hosts and Cloudflare with
0x80090302; `tls.chain` failed because the handshake did not start). T10's
gated policy now matches the code: DNS/TCP gate, TLS/chain findings report.

Added optional `KN_TLS_BACKEND=mbedtls`: CMake builds pinned mbedTLS 3.6.0
and its framework as static subprojects (not the Linux `.a` artifacts from
the sandbox build); `kn_net_mbedtls.c` implements Win32 DNS/TCP/TLS using
Winsock, TLS 1.2 minimum, SNI + hostname verification, required certificate
validation, read-only Windows ROOT trust stores, and peer-chain reporting.
T10 downloads the fixed source archives, validates SHA-256, selects the
requested backend, and verifies backend identity in build.json. Updated
`docs/NET.md`; corrected the sandbox prep script to use tag `v3.6.0` and
initialize the framework submodule.

Validation: default Schannel native CMake build + ctest 10/10; mbedTLS
selected native CMake build + ctest 10/10; clean temporary checkout fetched
and verified both pinned mbedTLS archives; T10 Python syntax and diff checks
pass. Limitation: host GCC builds only POSIX stubs; `apt-get install
gcc-mingw-w64-x86-64` failed due permission denied. No Windows compilation or
mbedTLS handshake has occurred yet.

NEXT ACTION: run `set KN_TLS_BACKEND=mbedtls` and
`python tests\T12_network.py` on Win7. T12 builds only knvstd and runs just
DNS/TCP/TLS/chain. Treat mbedTLS as proven only if the Windows build passes,
`net.tls` verifies all three hosts, and `tls.chain` returns a clean chain.
The POSIX T12 run is a dry-run only. If target T12 fails, analyze
`T12_wynik.txt` and `t12logs/diag-*.zip`; do not advance to WSS/RTSP and
never disable verification. Run full T10 after this gate passes.
→ `CMakeLists.txt`, `streamer/src/kn_net_mbedtls.c` (new),
  `streamer/src/kn_net.c`, `streamer/include/kn/kn_net.h`,
  `streamer/src/kn_selftest.c`, `tests/T10_build_and_test.py`,
  `tools/prepare_mbedtls_sandbox.sh`, `docs/NET.md`, this file

### 2026-10-23 · network-only T12 harness
Per user request, added `tests/T12_network.py` so network iterations do not
repeat the graphics, decoder, clock, console, and unit-test portions of T10.
T12 configures the selected backend, builds only `knvstd`, runs the four
phase-C probes, validates their JSON and diagnostic bundle, and records
`T12_wynik.txt` / `T12_skrot.txt` under ignored paths. DNS/TCP are gates;
Schannel TLS remains a reported finding; mbedTLS TLS/chain are acceptance
gates. Updated `tests/README.md` and this handoff with the Windows command.

Validation: T12 POSIX dry-run built knvstd only, all four network probes
reported skipped, bundle/JSONL checks passed, and verdict was explicitly
`DRY-RUN ONLY`. It is not a network result. Existing Linux GCC warnings in
knlog are informational for POSIX T12; the Windows target still gates zero
warnings. The user's first mbedTLS target run then exposed the GCC 16
upstream-label warning; scoped suppression has been added but is not target-
verified yet. Next action is the same target T12 retry.
→ `tests/T12_network.py` (new), `.gitignore`, `tests/README.md`, this file

### 2026-10-23 · first target T12 mbedTLS compile
User ran T12 with `KN_TLS_BACKEND=mbedtls`. Archive download/hash checks and
CMake configure passed. GCC 16.2 failed while compiling upstream
`third_party/mbedtls/library/ssl_tls13_keys.c` because its intentionally
non-NUL-terminated label arrays trigger `-Werror=unterminated-string-
initialization`. Build stopped before `kn_net_mbedtls.c`, so no Win32 app
backend compile or TLS handshake was tested. Added a CMake suppression only
to the `mbedtls` target for GCC MinGW >= 16; all other targets/warnings stay
strict. Sandbox mbedTLS CMake build + ctest 10/10 pass. User should sync this
patch and rerun only `set KN_TLS_BACKEND=mbedtls` + `python
tests\T12_network.py`; no need for full T10 on this retry.

### 2026-10-23 · target T12 mbedTLS compile and handshake
After scoped suppression of GCC 16's upstream TLS-label warning, target
T12 reached `kn_net_mbedtls.c` and reported four errors: `valid_to` was
mistakenly used instead of the public result field `not_after`, and
mbedTLS 3.6 ciphersuite fields require `MBEDTLS_PRIVATE(...)`. Fixed those
references and reran sandbox mbedTLS build + ctest (10/10 PASS).

User reran target T12: CMake configure and `kn_net_mbedtls.c` compile/link
passed with zero compiler warnings; DNS and TCP/443 passed. TLS/chain then
failed on all hosts with `0xffff9400` (`-0x6c00`,
`MBEDTLS_ERR_SSL_INTERNAL_ERROR`), before any peer certificate was exposed.
Added mbedTLS debug callback capped at level 2, mapped to INFO diagnostics,
plus `mbedtls_strerror` WARN output. Level 2 emits no raw TLS buffers or key
material. Sandbox build + ctest 10/10 PASS. Next: sync this diagnostic patch,
run only T12, inspect `t12logs/diag.log`, then fix the specific handshake
failure. T10/GPU tests are not needed for this network-only iteration.

User supplied that T12 `diag.log`: the failure occurs in TLS 1.3 ECDH,
where `psa_generate_key()` returns `-0x6c00`. The pinned mbedTLS SSL client
example calls `psa_crypto_init()` before RNG/SSL setup. Added PSA init before
CTR-DRBG seeding and explicit WARN logging if init itself fails. Sandbox
mbedTLS build + ctest remain 10/10 PASS. NEXT: sync this change and rerun
only T12 on Win7; verify the handshake now advances and all cert chains
validate. No full T10/GPU run is needed.
