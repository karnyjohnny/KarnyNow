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
#    the user's own FFmpeg dev tree instead): headers at the user's exact commit —
#    scripted: python3 tools/fetch_ffmpeg_headers_sandbox.py
#    (downloads bf0e831606938f530326c9679591109407fa8d0d, extracts
#    libavcodec/libavutil/libswscale *.h into third_party/ffmpeg-headers/,
#    writes the avconfig.h/ffversion.h stand-ins). Manual recipe:
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
# optional, for sandbox --decode E2E with a REAL decoder (system FFmpeg 5.1):
apt-get install -y libavcodec-dev libswscale-dev libavutil-dev libswresample-dev
# verify everything before trusting a run:
sh tools/build_native_tests.sh     # host: 10 suites + knvstd smoke + zip/jsonl validation
sh tools/cross_check.sh            # mingw x64+i686, -Wall -Wextra -Werror, F1 flags
```

**The authoritative compile+run gate is the TARGET machine** (w64devkit GCC
16.2.0 — stricter than sandbox GCC 12; it catches warnings GCC 12 misses):
`python tests\T10_build_and_test.py` under ConEmu → `T10_wynik.txt` +
`T10_skrot.txt`. The stage-2 socket gate is `python tests\T13_udp_e2e.py`
(loopback-only, no account; reuses T10's build). The user runs both quickly
and willingly — ask for a measurement instead of guessing (§10.7).

## Current status (updated 2026-10-25, post-round-XXIV: R7 RUN 5 — the round-XXIII fix confirmed LIVE (control-host switch → seat_step 1→3 → status:2 READY → OPTIONS 200 on a real rig); DESCRIBE died rc:-2 on the 8 KiB per-message scratch → FIXED (+ binary WS frames + Host:port + rtsp_detail instrumentation); awaiting the next target T10 + R7 run 6)

| Stage | State |
|---|---|
| **0 — measurements** | **COMPLETE.** G0.1 CPU ✅ · G0.2 D3D9 presentation ✅ **(T4 v4)** · G0.3 toolchain ✅ · G0.4 console ✅ |
| **1 — C skeleton, logging, loopback/audio** | **CLOSED.** Stage-1 scope complete; G0 closed and target T11 full G1 passed (see round XV evidence below). |
| **2 — NVST media/transport** | **IN PROGRESS — MEDIA CORE TARGET-VERIFIED (T10 r12: ALL PASS 67) + SOCKET LAYER DONE (round XVII, sandbox-verified incl. the full T13 E2E matrix under three builds + sanitizers; awaiting target T10 r13 + T13 r1).** `kn_rx` (reorder, FEC repair + re-wrap, RFI, NACK tracker + encoders, watchdog, SRTP incl. GCM/CM256) passed on Win7: test_knrx 209/209, rx.vectors 153/153 (scenarios 31/31, srtp256/gcm verified), registry 13. Round XVII adds the live path: `kn_sock` (real UDP, Win32+POSIX), `knvstd --capture` (streaming gs_capture.hex-format recorder — the R32 mechanism), `knvstd --decode` (socket → kn_rx on a real QPC clock → capacity-2 AU queue → the NEW persistent `kn_decode_stream_*` session), `tools/send_rx_capture.py` (paced/blast UDP sender), `tests/T13_udp_e2e.py` (the socket gate: capture → replay-verify → decode paced/blast → SRTP cm128/gcm256 → sock.loopback), and `sock.loopback` — the 14th self-test (portable, gated). **SOCKET LAYER CLOSED ON THE TARGET** (T10 r14 ALL PASS 69 + T13 r2 ALL PASS 61, 2026-10-25): the round-XVIII JSONL-escape fix confirmed; all T13 legs green on Win7 (capture 119/119 + parseable JSONL, live-recorded capture replays byte-identical, decode paced/blast/cm128/gcm256 each 60/60 with FFmpeg 63, zero overflows); **R34 CLOSED: Windows applies the full 8 MiB SO_RCVBUF** (requested 8 388 608 → applied 8 388 608 on every leg). Round XX added the signaling cores (kn_ws 84 + kn_rtsp 127 checks — target-verified in T10 r15, ALL PASS 71). **Round XXI built the REST of the production path: the persistent verified TLS-1.2 stream API (Win32+mbedTLS, honest stubs elsewhere), kn_json + kn_http + kn_cm (CloudMatch create/poll/extract — test_kncm 91 checks), kn_sock_udp_connect (kernel peer filter), the independent Python fake rig + a 36-check real-socket E2E of the WHOLE chain (create→poll→WS→RTSP→handoff→connected UDP), and `knvstd --session` (TLS-only, token length-only logging, queue events for the future TUI, the round-XVII media loop reused verbatim).** Remaining for stage 2 = MEASUREMENTS, not code: target T10 r16 (first compile of the Win32 TLS-stream body), then the R7 first-real-session round (token+appId → `--session`; D-s1/D-cm1/D-cm2 deviations get measured there), then G2. R7 RUNS 1-4 HAPPENED (round XXIII, on the T10 r17 ALL PASS 72 build): create works against the LIVE production cloud (4/4 real sessions, plausible live queue positions; D-cm1/D-cm2 accepted at create); the queue wait exposed ONE contract violation — polls stayed on the global create front (prod.cloudmatchbeta...) instead of the regional `sessionControlInfo.ip` node the create response nominates (np-waw-01... — exactly where the official client polls at ~4.1 s per the user's DevTools capture, and what cloudmatch.rs's learned control_base does :401-414/:652-656/:1149-1152) → frozen queuePosition across 40+ polls + `cm_poll_failed`. FIXED in round XXIII (trust-gated poll-host switch + the reference's transient-retry/404 semantics + no-clobber poll commit + requestStatus surfacing + 4 s cadence + best-effort DELETE cancel on abort + queue-timeout default 600 s); docs/RTSP.md §10 is the record. **R7 RUN 5 (round XXIV) CONFIRMED THE FIX LIVE: cm_control switched to np-waw-01 (source sessionControlInfo), the session progressed seat_step 1→3 to status:2 READY with a real rtsps endpoint, and OPTIONS returned 200 over RTSPS/WSS on the real rig; DESCRIBE then died rc:-2/status:0 — the 8 KiB per-message scratch in kn_rtsp_request (real SDPs are bigger; the reference accumulates up to 4 MiB, nvst_rtsp.rs:559-566/:614-623) and/or the refused BINARY frames (:621) — both FIXED, plus Host:port fidelity (:1070-1076) and rtsp_detail failure instrumentation; docs/RTSP.md §11 is the record. Next measurement = the next target T10 (expect ALL PASS 72) + R7 run 6.** |
| **3–5** | Not started; see BOOTSTRAP stage gates. |

Stage 1 closure: T11 full G1 passed on the target before this continuation; graphics, decode, audio, and logging gates are closed. No GPU retest is needed for the current Stage-2 socket work (round XVII touched no graphics code).

**Latest full target T10 (user-reported 2026-10-24 12:52, round 12):** `ALL PASS`, **67/67 checks** — the round-XVI media core verified on Win7 with mbedTLS default: build zero-warning (116 s), 9 unit suites (test_knrx **209/209** first target run incl. the GCM/CM256 legs), registry **13** self-tests all usable, `rx.vectors ok` (153 checks, scenarios 31/31, `srtp_backend:1`), decode 60/60 @ 3333 fps, d3d probes reproduced (pitch 1536, present mean 0.29 ms), net.tls/tls.chain OK (TLS 1.2, depth 3, Amazon Root CA 1), bundle 139 545 B validated, PE 6.01. `clock.resolution` reported **ok** this run (Sleep(1) 1.00→1.75 ms, wall tick 1000→1000 µs — the machine state swung back; R30 note updated). Raw evidence: `artifacts/T10-round12/`.

**Latest full target T10 (round 14, 2026-10-25 09:54):** `ALL PASS`, **69/69 checks** — the round-XVIII JSONL-escape rebuild gate: zero warnings, test_knselftest 114 (Win32 delta), test_knrx 215, test_knsock 71, registry 14 all usable. `clock.resolution` reported **degraded** (Sleep(1) 15.62→2.88 ms, wall tick 15600 µs) — the machine flipped back to the sleep regime hours after r13's healthy 1000 µs ticks (R30's documented swing; T10 accepts degraded by design). Raw: `artifacts/T13-round2/`. (Round 13, 08:36, was the round-XVII socket-delta verification, also ALL PASS 69:) test_knsock **71** checks (host 70 + Win32 truncation-branch delta), test_knrx **215**, registry **14**, `sock.loopback ok` (64/64 burst in 1 ms, timeout 50 ms), zero warnings, decode 60/60 @ 4000 fps, net.tls/tls.chain ok, clock.resolution ok (healthy regime: Sleep(1) 1.00→1.00 ms, wall tick 1000 µs). **First R34 datapoint: Windows applies SO_RCVBUF EXACTLY as requested (262 144 → 262 144).** Raw: `artifacts/T13-round1/T10_wynik_r13.txt`.

**Target T13 (round 1, 2026-10-25 08:39): FAIL (2 of 51) — ONE product defect, fixed round XVIII.** Every stage-2 mechanism worked on Win7 (capture recorded 119/119 — proved by the vector leg parsing live.hex; ALL decode legs 60/60 with real FFmpeg 63 incl. **gcm256 SRTP live**; sock.loopback ok; zero queue overflows; paced sender held the 2 s timeline in 1966 ms). The two failures were both the same JSONL-contract defect: **Windows absolute paths embedded RAW into stdout JSON** (`"out":"C:\Users\…"` → `\U` is not a legal escape) made the capture-summary and replay-summary lines unparseable ("VERIFY FAIL: 1 unparseable stdout lines" while all 60 frames actually matched). Fix: every argv/path/foreign string in knvstd's printf-JSON sites now routes through the loopback-proven `lb_json_path` sanitizer (T11 never hit this because loopback had it since round XII), hardened with a `>=0x7F → '?'` rule (Win32 argv is cp1250 — raw non-ASCII would be invalid UTF-8 in the JSONL); `st_gs_parse`'s KN_GS_CAPTURE path likewise (`st_json_str`). Reproduced 1:1 in the sandbox with literal-backslash filenames before/after; T13 hardened with per-leg "every stdout line parses" checks + a POSIX backslash-path leg + rcvbuf logging. Full story: docs/SOCK.md landmine 6, evidence `artifacts/T13-round1/`.

**Target T13 (round 2, 2026-10-25 09:58): ALL PASS, 61/61 checks — the exact predicted count.** The round-XVIII fix is target-verified: capture leg 119/119 records with every stdout line parseable; vector leg replays the live-recorded capture byte-identical to the reference; ALL FOUR decode legs 60/60 with libavcodec 63.2.100 (paced + blast + cm128 + **gcm256 live over a socket**), zero q_overflows, zero recv_errors, every stdout verified against the Python manifest; sock.loopback ok. **R34's headroom evidence: `rcvbuf_actual=8388608` on every leg — Windows honours the full 8 MiB request.** The stage-2 socket layer is CLOSED; raw: `artifacts/T13-round2/`.

**G2-offline demo on the target (idk_g2.txt, 2026-10-24): SUCCESS — the one failed command was a missing shell redirect, not a product bug.** Capture generation matched the sandbox byte-for-byte (seed-1 determinism across machines: 119 records / 360 333 B); `--replay-capture` delivered 60/60 AUs (fec_repaired=19, zero stale/gaps/withheld, state 0); ffprobe accepted the dumped AU. The `--verify` step failed with "Nie można odnaleźć określonego pliku" because knvstd's stdout was never redirected into `replay.jsonl` (cmd could not open the missing redirect INPUT — python never ran). The console JSONL was reconstructed in the sandbox and verified against the regenerated manifest: **VERIFY PASS, 60 frames byte-identical to the reference receiver**. tests/README.md's demo block now carries the redirect inline; T13 automates the whole sequence.

**Latest target T12 (2026-10-23 22:47):** `ALL PASS`, 27/27 checks using `KN_TLS_BACKEND=mbedtls`. Build is warning-free; DNS + TCP/443 pass; TLS verifies `login.nvidia.com`, CloudMatch, and Cloudflare; `tls.chain` is clean, depth 3, root Amazon Root CA 1, trust/policy both zero. Probe is pinned to TLS 1.2 (min=max); TLS 1.3's Finished/PSA issue remains isolated and is not used on Win7. User subsequently reported full T10 `ALL PASS (65 checks)`.

The embedded backend is now the default in CMake and both T10/T12 harnesses. It uses pinned mbedTLS 3.6.0 static CMake targets, bounded Win32 Winsock, hostname/SNI verification, `MBEDTLS_SSL_VERIFY_REQUIRED`, read-only Windows ROOT-store imports, and PSA initialization before TLS use. Schannel remains an explicit diagnostic fallback. Do not disable verification.

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
| **D21 (new, round XVII)** | **The stage-2 live loops (`--capture`, `--decode`) are SINGLE-THREADED**: one loop does `recv(10 ms timeout)` → `kn_rx_video_process_datagram` → inline AU-queue drain → `kn_rx_video_poll` at the reference's 10 ms cadence, all on the QPC-ms clock. The capacity-2 encoded queue keeps the reference's overflow semantics (drop-new + `kn_rx_inval_lose` + `kn_rx_video_request_keyframe`) so a later receive-thread split touches no pipeline logic. The 8 MiB SO_RCVBUF absorbs decode stalls (~16 ms worst 720p frame ≈ 500 datagrams at 30 fps). | Stage-1 loopback precedent (same shape, 30.000 fps for 600 s, G1); 2-core E5500 — threads cost context switches the budget cannot spare; kn_rx is single-threaded by contract (no locks = no lock bugs); sandbox T13 blast leg (119 datagrams in <1 ms) decoded 60/60 with zero overflows. | **Measured** socket-buffer backlog or `recv_errors`/`q_overflows` in a REAL session's decode summary → move the receive side to a thread (docs/SOCK.md §3 revisit condition) |

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
| **R30 (round X: stable pattern, informational; round XVII: swings BOTH ways)** | `clock.resolution` variance is NOT transient load: rounds 4+5 both measured Sleep(1) 15.6→**2.75/2.88** ms (verdict `degraded`, threshold 2.6 from round 1's 1.25→1.00) with wall tick pinned at 15600 us **even after timeBeginPeriod(1)** (round 1 saw 1250 us ticks — the machine state changed between rounds, probably a reboot cleared whatever had raised the timer). Rounds 8/9/12/13 then measured the OTHER state again (r12 2026-10-24 `ok`: Sleep(1) 1.00→1.75 ms, tick 1000; r13 2026-10-25 08:36 `ok`: 1.00→1.00, tick 1000) and **r14 (09:54, the SAME DAY ~80 min later) flipped back to `degraded` (15.62→2.88 ms, tick 15600)** — the regime can change within HOURS on one machine, not just across reboots; alternate between a ~1 ms and a ~15.6 ms timer regime. Consequences: (a) treat **~2.9 ms as the realistic WORST-CASE Sleep(1) floor** on this machine — enough for 33.3 ms frame pacing at 30 fps, NOT enough for precise 1 ms spins; (b) wall-clock granularity may be 15.6 ms — timestamps below that resolution are quantised in the degraded regime; (c) D9 stands stronger than ever: pacing measures real Present intervals, never assumes; (d) the T13 sender's hybrid sleep+spin pacing is regime-proof by design. Thresholds unchanged (honest `degraded` beats a fudged `ok`). |
| **R15 (impl round II)** | **Mostly closed.** T8 on the target resolved all 14 Python packages rc=0 (pip honours Requires-Python, so every pin declares 3.8 support): textual 6.2.1, rich 14.3.4, httpx 0.28.1, httpcore 1.0.9, h11 0.16.0, anyio 4.5.2, + 8 more — `python/requirements.txt` updated, wheels cached on target in `tests\t8_wheels\`. Remaining: offline install + runtime smoke at stage 4 (httpx 0.28 removed `proxies=`; textual 6.x API churn vs old docs). |
| **R31 (updated)** | Sandbox has host GCC 13.3 but no MinGW/Windows SDK headers (apt install denied). Target T12 now confirms the mbedTLS Win32 compile/link, zero warnings, TLS 1.2 verification on all three hosts, and a clean depth-3 chain. TLS 1.3 Finished/PSA still fails; the Win7 backend is capped at TLS 1.2. |
| **R32 (round XVII: REVISED — mechanism shipped, real traffic still pending)** | **kn_rx has still never seen a real GFN packet**, but the recording/rehearsal mechanism now exists and is proven: `knvstd --capture` writes real-socket datagrams in gs_capture.hex format (so ANY real session becomes a `--replay-capture` regression vector), `tools/send_rx_capture.py` + `tests/T13_udp_e2e.py` rehearse the whole graph offline (capture → replay-verify → decode paced/blast → SRTP cm128/gcm256), and the target's own G2-offline demo run (idk_g2.txt) verified byte-identical against the reference. The canonical parity construction, SPI-vs-seq relationship and GCM256-tag8 profile remain DERIVED from the reference and its tests — first real Mjolnir traffic (stage-2 WSS/RTSP + a live session) is still the true test. Until then every deviation is documented (RX.md D-a..D-e) and T13 exercises the loss/reorder/RFI/SRTP paths over real sockets. |
| **R33 (round XVI: CLOSED; round XVII: extended)** | Arena-memory class of bugs: two real defects (reorder-map `used` garbage from malloc; 8-byte arena misalignment) slipped past static-context unit tests and were caught by the malloc'd-arena scenario battery + UBSan BEFORE handover. Rule adopted: vector runners allocate through malloc (never statics), every arena carve rounds to 8, and ASan+UBSan+LSan is part of the pre-handover gate for any kn_rx-family change. **Round XVII proved the rule's worth again: LSan caught a real leak in the NEW `kn_decode_stream_close()` (kd_deliver's NV12 buffer, 115 264 B) on the first sanitized E2E run — fixed, and the whole T13 matrix now passes under ASan+UBSan+LSan.** Sanitizer policy note: the vendored `third_party/crypto-algorithms-sha1/sha1.c:26` (`data[j] << 24`, int promotion) is PRE-EXISTING formal UB in a hash-pinned file that must NOT be edited — sanitizer runs use recover mode and the gate is ZERO diagnostics from `streamer/` code (verified: the sha1.c line is the only runtime error in a sanitized T13). |
| **R34 (round XIX: CLOSED except the real-session firewall note)** | **The Winsock side of the live modes is target-verified (T13 r2: ALL PASS 61).** capture recorded 119/119 datagrams with parseable JSONL, every decode leg ran 60/60 (paced + blast + cm128 + gcm256), sock.loopback ok, select-cadence and QPC clock behaved, the round-XVIII JSONL-escape fix confirmed. **SO_RCVBUF: Windows applies requests EXACTLY — `rcvbuf_actual=8388608` on every decode leg (the full 8 MiB; ~6 700 datagrams of slack at shard 1296 — the burst-headroom math holds on the target).** The only remainder: a REAL session binding INADDR_ANY with inbound internet UDP — the firewall-allow question is deferred to the WSS/RTSP round and must be MEASURED, not assumed (loopback legs proved WFP does not filter 127.0.0.1). |

Everything else (R3 usrsctp/DTLS spike, R7's first READY session — the
first real `POST v2/session` itself is now MEASURED, 4/4 OK, round XXIII;
R22 31-field session context, R23 RESUME, R24 catalog revision binding)
unchanged — see BOOTSTRAP §9.

## Next steps (ordered)

1. **Target-verify round XXIV: `python tests\T10_build_and_test.py`** —
   expect ALL PASS **72** (suite count unchanged; test_knrtsp now runs
   132 internal checks, test_kncm 147 — internal counts are not T10
   checks). Round XXIV touched kn_rtsp.c (one function), knvstd_main.c's
   phase B glue, the E2E harness, the rig and test_knrtsp.c — all
   sandbox-gated (native 13 suites, cross x64+i686 -Werror, ctest
   15/15 ×2, E2E 36/36 with the NEW rig behaviour). T13 optional
   (unchanged, expect 61 — kn_rx untouched).
2. **R7 run 6 — the first READY session, take two** (run 5 reached
   `status:2 ready` + OPTIONS 200 on the real rig and died at DESCRIBE
   on the transport deviations — fixed; docs/RTSP.md §11). Same command:
   `knvstd --session --session-token=... --session-app=N
   [--session-title=...] --session-seconds=60 --session-au-dir=aus`
   → watch the JSONL: cm_created → cm_control → queue (on the NATIVE
   queue the position may stay static while seat_step advances — run 5
   measurement, not a defect) → ready → rtsp steps → handoff →
   aus/frames. EVERY outcome is evidence: DESCRIBE 200 → the SETUP
   sweep (peerless-200 poison rule + bundle fallback implemented, §2) →
   ANNOUNCE = D-s1's first real verdict → PLAY → the media loop
   (R32/R34-remainder). ANY failed step now emits an `rtsp_detail`
   line (kn_rtsp err + ws_err/ws_close/ws_text/ws_bin) — paste it
   verbatim; a server close shows as ws_err:-7 + the close code.
   `cloud_status 11` on cm_create = SESSION_LIMIT (an earlier session
   is still alive server-side — wait or end it in the GFN web UI); a
   not_ready exit ends with a `cm_cancel` event (200/204/404 =
   cleaned up). Capture always feeds the diagnostic ring; add
   --capture in a second run for the permanent regression vector.

3. Stage 3 (R3 DTLS/SCTP spike — replaces the D-s1 placeholder identity)
   + presentation wiring for --session frames (the G1-proven presenter;
   the natural follow-up once real frames flow).
4. Stage 3+ per BOOTSTRAP §12; R22's full 31-field context + R23 RESUME
   at stage 4/5.
5. **Presentation wiring for the live path**: `--decode` frames →
   `kn_d3d_vp_*` presenter (the G1-proven chain) = moving video from the
   socket path on screen; the D20 FPS overlay defaults OFF there. Small
   and independent of WSS/RTSP — can ship in the same round as step 2 or
   be rehearsed against `send_rx_capture.py` first.
6. Control senders wiring (stage 3 with SCTP): the NACK-v2/IDR/RFI
   encoders are DONE and KAT'd; `kn_rx_video_take_keyframe_request` now
   also carries the decode-queue-overflow requests (D21). SRTCP-GCM RR
   when !rtcp_on_sctp.


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
│   ├── RX.md                   ← kn_rx receive pipeline: provenance table,
│   │                              §Parity construction, deviations, landmines,
│   │                              replay-capture usage (ROUND XVI)
│   ├── SOCK.md                 ← kn_sock + --capture/--decode + persistent
│   │                              decoder session + T13 (ROUND XVII record)
│   ├── RTSP.md                 ← WSS+RTSP+CloudMatch wire spec (line refs
│   │                              @ a49b124) + §8/§9 implementation status
│   │                              + deviations (ROUND XIX research, XX/XXI impl)
│   ├── AUTH.md                 ← OAuth device-flow research (ROUND XXI;
│   │                              login implementation = stage 4)
│   ├── AUDIO.md                ← kn_audio (WASAPI) provenance + landmines
│   ├── NET.md                  ← kn_net phase-C probes (schannel) + verdict table
│   ├── LOOPBACK.md             ← stage-1 video path design + G1 gates
│   ├── analysis/               ← measurement analyses (G0.2 closed here)
│   └── research/               ← BOOTSTRAP.md + research HANDOFF (copies)
├── streamer/                   ← knvstd (C11)
│   ├── include/kn/             ← knlog knring knfec knsrtp kn_aes kn_sha1
│   │                              kn_diag kn_selftest kn_decode kn_d3d kn_gs
│   │                              kn_perf kn_audio kn_net kn_rx kn_sock
│   │                              kn_ws kn_rtsp kn_json kn_http kn_cm
│   ├── src/                    ← implementations + GENERATED: knfec_tables.h,
│   │   │                          knfec_kat_vectors.h, knsrtp_kat_vectors.h,
│   │   │                          kn_gs_kat_vectors.h, kn_rx_kat_vectors.h,
│   │   └                          kn_synthetic_clip.h + kn_rx_kat_runner.h
│   │                              (shared battery: test + rx.vectors)
│   ├── assets/synthetic.264    ← decode.synthetic KAT clip (embedded via tools/)
│   ├── assets/synthetic720.264 ← loopback/G1 source: 720p High@5.1, NOT embedded
│   └── tests/                  ← portable unit tests (run on Linux AND Win7):
│                                  knlog knring knfec knsrtp kndiag knselftest
│                                  kngs knaudio knrx knsock knws knrtsp kncm
├── python/                     ← TUI (stage 4); requirements.txt RESOLVED via T8
├── tests/                      ← target-machine harnesses T1-T13
│                                 (T9 = old build gate; T10 = build+selftests;
│                                  T11 = the G1-video loopback gate;
│                                  T12 = network-only gate;
│                                  T13 = stage-2 UDP E2E gate, loopback-only)
├── tools/                      ← build/verify scripts + KAT generators
│   (build_native_tests.sh, cross_check.sh, check_jsonl.py, check_zip.py,
│    fec_reference.py, srtp_reference.py, gs_reference.py, rx_reference.py,
│    make_synthetic_clip.py, make_rx_capture.py, send_rx_capture.py,
│    fake_rig.py, prepare_mbedtls_sandbox.sh,
│    fetch_ffmpeg_headers_sandbox.py,
│    sandbox_decode_{check,edge}.c, sandbox_loopback_source.c,
│    sandbox_stream_decode.c, sandbox_session_e2e.c)
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

User's next T12 after PSA init confirmed ECDH now succeeds, but TLS 1.3
fails later in `mbedtls_ssl_tls13_calculate_verify_data()` while processing
the server Finished message (`psa_mac`/HKDF path, `-0x6c00`). Set the probe's
maximum TLS version to TLS 1.2 while keeping `VERIFY_REQUIRED` and the
minimum at TLS 1.2. This is a protocol-isolation/Win7-compatible acceptance
test, not a verification bypass. Sandbox mbedTLS build + ctest 10/10 PASS.
NEXT: run network-only T12 again; if TLS 1.2 passes all three hosts and the
chain, accept that backend for Win7 and investigate TLS 1.3 separately.

User's target T12 (2026-10-23 22:47) now PASSES 27/27 with mbedTLS TLS 1.2:
zero-warning `knvstd` build, DNS/TCP pass, all three TLS handshakes verified,
and `login.nvidia.com` chain depth 3 to Amazon Root CA 1 with trust/policy 0.
The summary's `TLSv1` is truncated by the reason formatter (`%.5s`); the probe
sets min=max TLS 1.2 and the full result is `TLSv1.2`. TLS 1.3 remains a
separate issue and is not negotiated by this Win7 backend.

T10 is now confirmed `ALL PASS (65 checks)` with mbedTLS selected and default.
Stage 1 is closed; Stage 2 starts with the AU assembler and its target-only
test, then reorder/FEC/network integration. Do not repeat GPU tests during
ordinary Stage-2 iterations; run full T10 only after integration-wide changes.

### 2026-10-23 · mbedTLS default + Stage 2 AU assembler
User reported full T10 `ALL PASS (65 checks)` with mbedTLS, following the
target T12 TLS 1.2 pass. Promoted mbedTLS to the CMake, T10, and T12 default;
Schannel remains an explicit fallback. Fresh sandbox default build records
`tls=mbedtls`; ctest 10/10 passes. Stage 1 is formally closed from prior
target G1 evidence, and Stage 2 is now in progress.

Implemented `kn_gs_au_assembler` using caller-owned storage (no allocation),
2 MiB default / 16 MiB maximum cap, SOF/EOF assembly, H.264/H.265 Annex-B
payload location, AV1 short-header last-packet length and extended-header
expected-AU length, frame-type/keyframe metadata, trailing AUD removal,
24-bit stream-index continuity, and explicit incomplete-frame/gap/keyframe
signals. `test_kngs` now has 125 checks and passes; full default-mbedTLS ctest
passes 10/10. No real stream packets have gone through the assembler yet.

NEXT: target-only check: `cmake --build build --target test_kngs` followed by
`build\test_kngs.exe` (125 checks PASS). No T10/T11/GPU run needed. Then
implement the bounded RTP reorder buffer using extended SRTP packet indexes,
gap wait/NACK ranges, stale/duplicate rejection, and frame invalidation/PLI.
→ `CMakeLists.txt`, `tests/T10_build_and_test.py`, `tests/T12_network.py`,
  `streamer/include/kn/kn_gs.h`, `streamer/src/kn_gs.c`,
  `streamer/tests/test_kngs.c`, `docs/GS.md`, this file

### 2026-10-24 · impl round XVI (stage-2 media core: kn_rx + SRTP phase 3 + replay-capture — ALL sandbox gates green, awaiting target T10 r12)
**Context:** fresh agent session bootstrapped from THIS file + the repo
(clone) + OpenNOW @ a49b124 (re-cloned; nvst.rs 14 695 lines confirmed).
Baseline reproduced first: vendor files hash-verified, native 8 suites
703 checks PASS, cross-check PASS — the round-XV (Copilot) AU assembler
delta verified green in the sandbox (test_kngs 125/125) BEFORE building on
it. User instruction this round: *"continue stage 2, write+test as much as
possible before laptop testing; build tests so they run to completion even
when parts fail"* — the whole battery is section-isolated with per-check
accounting and never aborts (the fault-tolerant contract is now the suite
design rule).
**Review of the inherited AU assembler found two faithfulness gaps**
(fixed, vectors unaffected): `GsFrameHeader::parse` is codec-INDEPENDENT in
the reference (nvst.rs:3279-3289 — two ordered pattern arms), and
`Option<NvstFrameType>` distinguishes `Some(Unknown(x))` from `None` — the
RFI admit() logic keys off `is_some()`. Added `frame_type_parsed` /
`frame_type_known`; `kn_gs_rtp_parse` split into `parse_hdr` +
`kn_gs_rtp_payload` because SRTP padding bytes are CIPHERTEXT on the wire
(the reference validates padding only in payload() AFTER decryption,
nvst.rs:2695-2697).
**Round-XVI delta — `kn_rx` (docs/RX.md): the full pure NvstVideoReceiver
media path**, zero-allocation, injected-ms-clock, caller-owned pools:
RtpReorderBuffer (3537-3660 — 52 ms dequeue deadline, genuinely-absent
NACK ranges, empty-buffer jump resync, recovery ready.clear() quirk),
FecBlock+FecReorderBuffer (3925-4310 — knfec reconstruct, template
envelope re-wrap with pitfall #4 encoded, repair_failed single-retry,
completed_through silent-ignore, silent single-block wait), consume path
(4741-4862 — SOF-before-EOF, SPI continuity skip-push, RecoveredData
is_fec force, RFI admit/withhold), FrameInvalidation + queue
(nvst_invalidation.rs — RFI ranges, 100 ms retry, 250 ms one-shot IDR
backstop), NACK tracker (4 ms/52 ms/3-attempt/RTT policy, 64-batch cap,
overflow→keyframe), watchdog + timeouts + lifecycle + reset_media_state
(next_frame_contiguous=FALSE quirk), and the feedback ENCODERS: NACK-v2
0x0317 (base+bitmap records, wrap grouping), IDR 0x0302, RFI-range 0x0301,
frame-ack 0x0204 (f64 bits + 0xBF800000 constants), pacing 0x0203, QoS
0x0207 (command() field map ported as-is), RTCP generic NACK + PLI.
**SRTP phase 3 (docs/SRTP.md):** KDF256 + AES-256-CM + AES-GCM(128/256,
tags 4-16) under KN_TLS_BACKEND_MBEDTLS with honest NO_BACKEND stubs in
bare builds; `kn_rx_srtp_session` implements the four NvstSrtpMaterial
profiles and the reference unprotect order. GCM256-tag8 (the legacy
nvstVideo default = the production expectation) fully wired.
**Verification (the two-independent-implementations rule):**
`tools/rx_reference.py` — a from-scratch Python port of every table above —
SELF-PROVES (363 checks) against goldens transcribed from the reference's
own Rust tests (the 52 ms deadline scenario, NACK exclusion, parity-only
1+2 repair, 0xa5-poisoned fec_word 3+1 repair with byte-exactness at
[..28]/[32..], retain-following-blocks incl. allocation identity,
gaps_require_recovery contiguous=false, nack_v2/RTCP hex goldens, the
round-trip grid) + a pure-Python AES-256/GHASH/GCM oracle proved against
FIPS-197 C.3 and the GCM spec vectors → emits 31 scenarios + op scripts +
6-profile SRTP vectors. `test_knrx` (9th suite): battery + reorder units +
5 property families + error paths = **184 checks bare / 209 mbedTLS, 0
failures**; `rx.vectors` self-test runs the same battery from the shipped
binary (~1 ms, 126/153 checks). Gates: native 9 suites 889 checks PASS;
cross mingw x64+i686 -Werror (+truncation=2 on touched files) CLEAN;
ASan+UBSan+LSan clean; CMake+mbedTLS ctest 11/11; T10 POSIX dry-run ALL
PASS (57); kn_selftest paranoid-level warning count UNCHANGED (9
pre-existing, 0 new).
**Bugs the battery caught (all fixed, RX.md landmines):** (1) the reorder
map is caller memory — init MUST clear `used` flags (malloc'd-arena
scenarios caught what static-context unit tests could not); (2) arena
regions need 8-byte alignment (UBSan); (3) `--replay-capture` vps
derivation must subtract the SRTP tag from the largest WIRE record or
every FEC-repaired AU grows tag_len trailing bytes (E/F matrix caught it:
+10 CM128 / +8 GCM on exactly the repaired frames); (4) the capture
generator's parity base-seq off-by-one produced a duplicate wire seq — the
pipeline's replay window correctly rejected it (independent
implementations disagreeing loudly = the system working).
**NEW tool `--replay-capture` + `tools/make_rx_capture.py` (the offline G2
mechanism):** packetizes the real synthetic.264 clip into canonical
GS/RTP+FEC records (SPI = data-only counter — parity consumes RTP indices
but not SPIs), injects loss/reorder, optionally SRTP-protects, and records
the Python reference receiver's verdict in the manifest; knvstd replays on
the capture timeline (52 ms deadlines live) emitting per-AU JSONL
(fi/len/sha1/kf/contiguous) + optional .264 dumps for ffprobe. Matrix 6/6
VERIFY PASS: lossless; FEC25+loss6+reorder30%; FATAL loss (RFI
withhold-until-IDR: 38/60 frames — identical verdicts both sides);
FEC50+loss8+reorder20%; SRTP-CM128; SRTP-GCM256 (mbedTLS build).
**Key design insight recorded (RX.md §Parity):** canonical parity packets
carry the algebraic FOLD at ext[0..4] (spi) and ext[8..11] (flags) but
their OWN envelope/ext[4..8]/ext[11]/fec_word — which is exactly why a
recovered shard reconstructs spi+flags perfectly (continuity + the 4089
validation pass), why the re-wrap patches only envelope/frame_index/
ext11, and why the recovered fec_word is garbage by design (pitfall #4).
Recovered packets are shard_len-sized → repaired AUs carry trailing zero
stuffing (reference-sanctioned for AVC; the verifier models it exactly).
Awaiting: **target T10 round 12** (first target run of test_knrx +
rx.vectors incl. the GCM/CM256 legs), then stage-2 sockets (Next steps 2).
→ `streamer/{include/kn,src}/kn_rx.*`, `streamer/src/kn_rx_kat_runner.h`,
  `streamer/src/kn_rx_kat_vectors.h` (generated), `tools/rx_reference.py`,
  `tools/make_rx_capture.py`, `streamer/tests/test_knrx.c`,
  `streamer/{include/kn,src}/knsrtp.*` (phase 3),
  `streamer/{include/kn,src}/kn_gs.*` (split parse + assembler fixes),
  `streamer/src/kn_selftest.c` (rx.vectors + relevance),
  `streamer/include/kn/kn_selftest.h`, `streamer/tests/test_knselftest.c`,
  `streamer/src/knvstd_main.c` (--replay-capture), `CMakeLists.txt`,
  `tools/{build_native_tests,cross_check}.sh`,
  `tests/T10_build_and_test.py`, `docs/{RX,SRTP,GS}.md`,
  `INTEGRATION.md` rev 9, `tests/README.md`, this file

### 2026-10-25 · impl round XVII (T10 r12 ALL PASS 67 + G2-offline demo CLOSED → stage-2 SOCKET LAYER delivered: kn_sock + --capture/--decode + persistent decoder + T13)
**Context:** fresh agent session bootstrapped from THIS file + the repo clone
(HEAD `c3ff27c`) + OpenNOW @ a49b124 (nvst.rs sha256 `f52fc09f…`, 14 695
lines re-verified) + the user's three uploads (T10_wynik.txt = round 12,
idk_g2.txt = the G2-offline demo console, this file). Baseline reproduced
FIRST: vendor files hash-verified, native suites green, cross-check green —
exactly the round-XVI numbers (889 checks then; 972 now with the new suite).
**Target evidence analysed (both items round XVI awaited):**
(1) **T10 round 12 (2026-10-24 12:52): VERDICT ALL PASS, 67/67 checks.**
The round-XVI media core is target-verified: test_knrx **209/209** on Win7
(first execution — the GCM/CM256 legs active in the mbedTLS build),
`rx.vectors ok` 153 checks (scenarios 31/31, `srtp_backend:1`), registry
13/13 usable, zero warnings (GCC 16.2, 116 s build), decode 60/60 @
3333 fps, d3d probes reproduced (pitch 1536, present mean 0.29 ms, fill
max 28 ms = first touch), net.tls + tls.chain ok (TLS 1.2, depth 3, Amazon
Root CA 1), bundle 139 545 B validated, PE 6.01. **clock.resolution `ok`
this run** (Sleep(1) 1.00→1.75 ms, wall tick 1000→1000 µs) — the machine
alternates timer regimes across reboots; R30 reworded (worst-case floor
2.9 ms stands for pacing design; both regimes now have target samples).
(2) **idk_g2.txt (the user's G2-offline demo): SUCCESS — one missing shell
redirect, zero product defects.** Target capture generation matched the
sandbox regeneration byte-counts exactly (seed-1 determinism ACROSS
machines: 119 records / 360 333 B). Target replay: 60/60 AUs, all
contiguous, fec_repaired=19, zero stale/gaps/withheld, state 0; ffprobe
accepted au_00001_k.264 (h264 CB 320×240 — the synthetic.264 KAT clip's
profile, expected). The failing `--verify` line: `python … --verify
cap.json < replay.jsonl` → cmd's "Nie można odnaleźć określonego pliku"
= the redirect INPUT `replay.jsonl` never existed because step 2 ran
WITHOUT `> replay.jsonl` (python never even started). Reconstructed the
console JSONL in the sandbox → **VERIFY PASS: 60 frames byte-identical to
the reference receiver**, and the sandbox replay of the same capture is
byte-identical to the target's console output (all 60 AU lines + every
summary counter). tests/README.md's demo block now carries the redirect
INLINE (the parenthetical note was missed — make the command itself
correct); T13 automates the whole sequence so no future demo depends on
hand-copied redirects. Raw evidence archived: `artifacts/T10-round12/`.
**Round-XVII delta — the stage-2 socket layer (docs/SOCK.md is the design
record; provenance re-read from nvst.rs @ a49b124: 8 MiB SO_RCVBUF :235
(applied :5682), 10 ms poll :244 — the old Next-steps cite ":222" was
stale, RX.md corrected):**
- **`kn_sock`** (new module): bounded UDP, Win32 winsock + POSIX BOTH
  REAL (no stubs — the sandbox runs the full live E2E), zero allocation,
  refcounted WSAStartup, QPC/CLOCK_MONOTONIC µs+ms clock (the live
  `now_ms` source), SO_RCVBUF request + getsockopt READBACK (Linux caps at
  ~208 KiB→reports 425 984; Windows honours 8 MiB — every summary reports
  both), select-based recv with honest per-platform truncation semantics
  (Win32 WSAEMSGSIZE = datagram LOST, POSIX silent truncation — documented
  + tested both ways), sendto, lifetime counters. `test_knsock` (TENTH
  suite, 70 checks): clock/ipv4/lifecycle/100-datagram byte-exact
  round-trip/timeout/65 507 B max datagram/512-burst drain with seq
  markers/the PLATFORM-DELTA case/bad-args — section-isolated per the
  round-XVI suite rule.
- **`knvstd --capture`**: streaming recorder in the EXACT kn_ring_dump
  line format (→ `--replay-capture` + `--verify` read real sessions with
  ZERO tooling changes — the R32 mechanism). Documented deviation from the
  old Next-steps wording: the FILE is primary (a ring cannot hold a whole
  session on 2 GB); the 64 KiB kn_ring is the failure-diagnostic mirror
  (dumped to `<log_dir>/wire_capture.log` → rides the auto-bundle). Flush
  every 32 records/100 ms (bounded R2b loss, no fsync storm). Ctrl-C
  graceful (SetConsoleCtrlHandler/SIGINT) → clean summary. Empty capture =
  rc 1 + E_CAPTURE_EMPTY amplification (never a silent pass).
- **`knvstd --decode`**: live socket → kn_rx (real QPC-ms clock, real 5 s
  timeout — the production watchdog, unlike replay's MAX) → **capacity-2
  encoded queue** (analiza/04 §2.4) with the reference overflow policy
  (drop-new + `kn_rx_inval_lose` + `kn_rx_video_request_keyframe` — the
  ONE new kn_rx API, +6 test_knrx checks: 190 bare / 215 mbedTLS) →
  persistent decoder session. Per-AU JSONL with sha1 + capped rx-event
  lines + kf_request lines (UNSENT until stage 3 — recorded, never
  dropped) + 1 s transport stats log lines. Summary is FIELD-COMPATIBLE
  with `make_rx_capture.py --verify` ("frames" alias) → the cross-impl
  contract now covers LIVE traffic. **D21: single-threaded** (recv→process
  →inline drain→poll at 10 ms cadence; 8 MiB rcvbuf absorbs decode
  stalls; stage-1 loopback precedent; queue semantics already
  thread-ready; revisit on measured backlog).
- **`kn_decode_stream_*`** (new persistent session in kn_decode):
  `kn_decode_h264_mem` is one-shot — per-AU feeding through it kills
  inter-frame references. MEASURED (sandbox_stream_decode.c control):
  one-shot per AU delivers **2 of 60** frames; the persistent session
  delivers **60/60, 0 failures**, survives a poisoned mid-stream AU, and
  has a sticky callback abort. NO parser on this path (whole AUs in) → the
  round-IX parser-flush landmine cannot occur; `flush()` drains the
  decoder. Honest stubs in bare builds (`--decode` then runs
  pipeline-only, reported as such).
- **`sock.loopback`** — the 14th self-test, PORTABLE + GATED (loopback UDP
  failing blocks stage 2 anywhere): 64-datagram burst byte-exact, honest
  50 ms timeout, rcvbuf readback, closed-socket discipline; ~50 ms from
  the shipped binary. Relevance mapping learned socket/udp/recv/bind/
  capture/decode keywords (§7.7.4).
- **`tools/send_rx_capture.py`**: paced UDP sender on the capture's µs
  timeline (hybrid sleep+spin — regime-proof per R30; `--speed 0` blast);
  **`tests/T13_udp_e2e.py`**: the socket gate (capture leg → vector leg
  (replay of the LIVE-RECORDED file vs the reference manifest) → decode
  paced → decode blast → SRTP cm128 → SRTP gcm256 (auto-enabled from the
  binary's rx.vectors evidence) → sock.loopback → jsonl hygiene; wipes
  t13work/t13logs per the T11 lesson; reuses T10's build). T10 rev: 10
  suites + registry 14 + sock.loopback gated (POSIX dry-run 58 ALL PASS).
**Bugs caught by the gates this round (all fixed, all recorded in
docs/SOCK.md):** (1) **LSan: `kn_decode_stream_close()` leaked
kd_deliver's NV12 buffer (115 264 B)** — R33's rule earned its keep on
the FIRST sanitized E2E run; (2) `_POSIX_C_SOURCE` preamble missing in two
fresh files (the kn_diag landmine re-fires in every new POSIX-side file —
now documented in both); (3) mingw `socklen_t` needs `<ws2tcpip.h>` (the
kn_net landmine, re-confirmed); (4) WSA-refcount leak in open()'s error
paths (self-review, pre-gate); (5) T13 initially merged stderr into the
verify input — the PRE-EXISTING vendored `sha1.c:26` int-promotion UB
(pinned by hash, must NOT be edited) poisoned instrumented runs →
stdout-only capture + last-JSON-line parsing; sanitizer policy: recover
mode, zero streamer-code diagnostics (verified: sha1.c is the ONLY runtime
error in a sanitized T13).
**Verification matrix (sandbox, before handover):** native **10 suites 972
checks PASS**; cross mingw x64+i686 `-Werror` PASS (incl. kn_sock +
extended knvstd/kn_decode/kn_rx/kn_selftest); `-Wformat-truncation=2`
clean -O0/-O2/-O3 on every touched file (two host-only errsz==1
over-approximations documented, round-XI class); CMake ctest **12/12 in
BOTH backends** (schannel + mbedTLS, `tls=mbedtls` recorded);
ASan+UBSan+LSan: suites + the FULL T13 matrix clean; **T13 ALL PASS on
three builds** (44 FFmpeg-bare with real decode 60/60 paced+blast+cm128 ·
44 under sanitizers · **55 mbedTLS incl. the gcm256 leg**); T10 POSIX
dry-run ALL PASS 58; cross-machine parity: target idk_g2 JSONL == sandbox
output byte-identical, regenerated capture == target's 119 records/
360 333 B, cap.hex sha256 `31386ca8…` recorded.
**Awaiting (round XVIII):** target `T10 r13` (expect ALL PASS ~69:
test_knsock ~70, test_knrx 215, registry 14) + target `T13 r1` (expect
ALL PASS 55 — first Winsock execution of the live modes; record
`rcvbuf_actual` from the decode begin line: R34's headroom question).
Then WSS+RTSP (Next steps 2) and the presentation wiring (Next steps 3).
→ `streamer/{include/kn,src}/kn_sock.*` (new), `streamer/tests/test_knsock.c`
  (new), `streamer/{include/kn,src}/kn_decode.*` (stream session + leak fix),
  `streamer/{include/kn,src}/kn_rx.*` (request_keyframe),
  `streamer/tests/test_knrx.c` (+6), `streamer/src/kn_selftest.c`
  (sock.loopback + relevance), `streamer/include/kn/kn_selftest.h`,
  `streamer/tests/test_knselftest.c` (115 checks),
  `streamer/src/knvstd_main.c` (--capture/--decode/stop-handling),
  `tools/send_rx_capture.py` (new), `tools/sandbox_stream_decode.c` (new),
  `tools/fetch_ffmpeg_headers_sandbox.py` (new), `tests/T13_udp_e2e.py` (new),
  `tests/T10_build_and_test.py`, `CMakeLists.txt`,
  `tools/{build_native_tests,cross_check}.sh`, `docs/SOCK.md` (new),
  `docs/RX.md`, `tests/README.md`, `INTEGRATION.md` rev 10, `.gitignore`,
  `artifacts/T10-round12/` (raw evidence, gitignored), this file

### 2026-10-25 · impl round XVIII (T10 r13 ALL PASS 69 + T13 r1: 2 fails = ONE JSONL defect — reproduced, fixed, re-verified)
**Context:** same session as round XVII (continuation). User delivered
T10 r13 + T13 r1 results (T10_wynik attached; T13_wynik pasted in chat —
too big to attach; T13_skrot attached; all archived in
`artifacts/T13-round1/` with an evidence README).
**T10 round 13 (2026-10-25 08:36): VERDICT ALL PASS, 69/69** — exactly
the predicted count. Round-XVII socket delta target-verified: test_knsock
**71** (host 70 + the Win32 WSAEMSGSIZE-branch delta — the PLATFORM DELTA
case executing for real), test_knrx **215**, registry **14**,
sock.loopback ok in the shipped binary (64/64 burst 1 ms, timeout 50 ms),
zero warnings GCC 16.2, decode 60/60 @ 4000 fps, d3d probes reproduced,
net.tls/tls.chain ok, clock.resolution ok (healthy regime: wall tick 1000
µs, Sleep(1) 1.00→1.00). **R34 datapoint: Windows applies SO_RCVBUF
exactly (262 144 req → 262 144 applied).**
**T13 round 1 (2026-10-25 08:39): VERDICT FAIL (2 of 51).** Diagnosis —
both failures are ONE product defect, and everything else PASSED on Win7:
capture recorded 119/119 (the vector leg parsing live.hex proves the file
was perfect), all four decode legs 60/60 with real FFmpeg 63 (paced +
blast + cm128 + **gcm256 live over a socket**), zero q_overflows, zero
recv_errors, paced sender held the timeline (1966 ms for the 2 s
capture), sock.loopback ok, check_jsonl ok. The defect: **Windows
absolute paths embedded RAW into stdout JSONL** — the capture summary's
`"out":"C:\Users\KarnyJohnny\…"` contains `\U`, which is not a legal JSON
escape, so (a) the capture-summary line was unparseable ("summary
present" FAIL, its 4 dependent checks never ran — hence 51 not 55) and
(b) the replay summary line (same defect, `"capture":"C:\…"`, latent
since round XVI — relative paths hid it until T13 passed absolute ones)
made `--verify` report "1 unparseable stdout lines" **while all 60 frames
actually matched the reference**. The loopback mode never hit this because
round XII gave it `lb_json_path()` (T11-proven on the target); the
replay/capture/decode printfs didn't use it.
**Fix (this round):** (1) `lb_json_path` promoted to the
platform-neutral section of knvstd_main.c as THE stdout-JSON string
sanitizer (`\` and `/` → `/`, `"` escaped, C0 → `?`) and HARDENED: bytes
`>= 0x7F` → `?` (Win32 argv is ANSI cp1250 — a raw non-ASCII byte would
be invalid UTF-8 inside the JSONL contract; UTF-8-legal but non-ASCII
paths trade fidelity for parseability, documented); (2) every
argv/path/foreign string site routed through it — audit: unknown_selftest
name, diagnostics bundle path, replay error+summary paths, capture
error/detail/summary bind+out, decode error detail (loopback's three
sites already used it); (3) `st_gs_parse`'s KN_GS_CAPTURE path gets the
sibling `st_json_str` in kn_selftest.c (same rules). kn_selftest.c bar:
default-level `-Werror` clean both toolchains; paranoid `=2` count
UNCHANGED at the recorded 9 pre-existing over-approximations, 0 new.
**Reproduction discipline:** the target failure was reproduced 1:1 in the
sandbox BEFORE the fix — POSIX filenames legally contain literal
backslashes, so `--replay-capture='/tmp/cap\win.hex'` produced the exact
"VERIFY FAIL: 1 unparseable stdout lines" with 60/60 frames matching;
after the fix the same run gives VERIFY PASS with all 61 lines parseable
and the path normalized to `/tmp/cap/win.hex`. The capture leg was
reproved too (backslash `--capture-out`, summary parses, file written).
**T13 hardened (the gate learns):** per-leg "every stdout line parses as
JSON" checks (capture/replay/each decode leg — this defect class can
never again hide behind a downstream verdict), `rcvbuf_actual` logged per
decode leg from the begin line (R34 evidence in every future run), and a
POSIX-only backslash-path replay leg (on Windows the absolute paths in
every leg already exercise the contract).
**Sandbox gates after the fix:** native 10 suites 972 checks PASS; cross
mingw x64+i686 `-Werror` PASS; knvstd_main clean under
`-Wformat-truncation=2` -O0/-O2/-O3 both toolchains; CMake ctest 12/12
both backends; **T13 ALL PASS 51 (FFmpeg-bare) · 51 (ASan+UBSan+LSan
build) · 63 (mbedTLS incl. gcm256)** — the 63 includes the 2 POSIX
backslash-leg checks, so the Windows prediction is **61**; T10 POSIX
dry-run ALL PASS 58.
**Awaiting (round XIX):** target T10 r14 (MANDATORY before T13 — it
rebuilds knvstd with the fix; expect ALL PASS 69) + T13 r2 (expect ALL
PASS 61 + the 8 MiB rcvbuf_actual lines). Then WSS/RTSP (Next steps 2)
and/or the presentation wiring (Next steps 3).
→ `streamer/src/knvstd_main.c` (lb_json_path promoted+hardened, 9 sites
  routed), `streamer/src/kn_selftest.c` (st_json_str + gs.parse capture
  path), `tests/T13_udp_e2e.py` (contract checks + rcvbuf logging +
  backslash leg), `docs/SOCK.md` (landmine 6 + §7 target record),
  `artifacts/T13-round1/` (raw evidence, gitignored), this file

### 2026-10-25 · impl round XIX (T10 r14 + T13 r2 BOTH ALL PASS on target → SOCKET LAYER CLOSED; WSS/RTSP research phase → docs/RTSP.md)
**Context:** same session as rounds XVII-XVIII. User delivered T10 r14 +
T13 r2 results (attached; archived in `artifacts/T13-round2/`).
**T10 round 14 (09:54): ALL PASS 69/69** — the round-XVIII JSONL-escape
rebuild gate: zero warnings (GCC 16.2), test_knselftest 114 (Win32 delta),
test_knrx 215, test_knsock 71, registry 14 all usable. `clock.resolution`
**degraded** this run (Sleep(1) 15.62→2.88 ms, wall tick 15600 µs) — the
machine flipped back to the sleep regime ~80 minutes after r13's healthy
1000 µs ticks (R30 now documents same-day flips; T10 accepts degraded by
design, honest verdict beats a fudged ok).
**T13 round 2 (09:58): ALL PASS 61/61 — the exact predicted count.** The
round-XVIII fix is target-verified: capture leg 119/119 records with every
stdout line parseable; vector leg replays the LIVE-RECORDED capture
byte-identical to the reference; all four decode legs 60/60 with
libavcodec 63.2.100 (paced + blast + cm128 + gcm256 live over a socket),
zero q_overflows, zero recv_errors, every stdout verified against the
Python reference manifest; sock.loopback ok. **R34 CLOSED (except the
real-session firewall note): `rcvbuf requested 8388608 → applied 8388608`
— Windows honours the full 8 MiB SO_RCVBUF on every leg (~6 700 datagrams
of slack at shard 1296; the burst-headroom math holds on the target).**
**The stage-2 SOCKET LAYER is closed end-to-end on the target machine.**
**Round-XIX delta — the WSS/RTSP research phase (read, not remembered):**
new `docs/RTSP.md` — the full stage-2 signaling specification with line
provenance into OpenNOW @ a49b124: RTSP-over-WebSocket transport
(`wss://host:port/rtsp`, default port 322, `x-nv-sessionid` handshake
header, one RTSP request per WS Text frame, the CSeq==Request-Id
serializer, the Bifrost leading-CRLF-padding-tolerant parser with the
CSeq→Request-Id fallback and the TEARDOWN mismatch-skip rule, 20 s /
4 MiB limits); the method sequence with exact headers (X-GS-Version 14.2,
x-nv-abtesting 2, Accept application/sdp…) and every quirk (SETUP
stop-on-peerless-200 alliance-poison rule, 3×3 s peer retries, the two
Transport forms, the URI candidate ladder, Transport `source=`/
`X-GS-ServerPort=` peer extraction, disablePlay, the
nativeRtcOnBundlePort==1 requirement); DESCRIBE SDP extraction incl. the
SRTP material rules (runtime.encryptionKey 64-hex = AES-256 +
encryptionKeyId → **salt = key_id as 24-digit uppercase hex**, client
random-key fallback, the 8-profile scan order defaulting to
**AEAD_AES_256_GCM_8** — confirming round XVI's GCM256-tag8 production
expectation from the reference itself); the COMPLETE build_announce
attribute set (incl. refPicInvalidation:1 and the NACK queue constants
that match kn_rx exactly); the handoff JSON = the literal bridge into
kn_rx_video_config + kn_rx_srtp_session + the connected UDP peer; and
CloudMatch (GFNJWT auth header, Content-Type text/plain quirk, the full
POST v2/session sessionRequestData field list, readiness status∈{2,3}+
rtspsEndpoints, resume statuses 2..=5, the 31-field session_info R22
contract, signalingUrl = rtspsEndpoints[0] rtsps→wss). Plus the round-XX
staged implementation plan (kn_ws → kn_rtsp → kn_cm → OAuth research →
--session integration) and the fake-rig sandbox test strategy
(transport-injection seam; production crypto never bypassed — the fake
rig sits behind plain ws:// in tests only). NOT VERIFIED list included
(OAuth/gfn.rs unread; networkTestSessionId optionality; real-cloud shapes
— first real session is the true test, per R32's rule).
No product code changed this round (analysis + research + docs only); all
round-XVIII gates stand. **Awaiting: round XX implementation (no target
run needed until kn_ws/kn_rtsp land with their sandbox fake-rig E2E).**
→ `docs/RTSP.md` (new), `artifacts/T13-round2/` (raw evidence, gitignored),
  this file (status, R30/R34, Next steps, round XIX log)

### 2026-10-25 · impl round XX (stage-2 signaling cores: kn_ws + kn_rtsp — ALL sandbox gates green; target T10 r15 expects 71)
**Context:** same session; user green-lit round XX ("zaczynaj") after the
round-XIX research (docs/RTSP.md §1-7). This round implements §6 stages
1-2: the WebSocket framing and the RTSP-over-WS client. Stages 3-5
(kn_cm, OAuth, TLS-stream adapter, fake-rig E2E, --session integration)
are round XXI. NO existing module changed behaviour — kn_rx/kn_sock/knvstd
untouched, so the target-verified socket layer stands as-is.
**`kn_ws`** (new): RFC 6455 CLIENT framing over an injected byte-stream
seam (`kn_ws_io` send/recv) — production will plug the kn_net_mbedtls
TLS-1.2 stream (round XXI), the sandbox proves framing on an in-memory
duplex shim + 1-byte-chunk delivery. Handshake: CSPRNG 16-byte key →
base64 → GET /path with Host/Upgrade/Connection/Key/Version + verbatim
extra headers (the reference's `x-nv-sessionid` + `content-length: 0`);
101 + `Sec-WebSocket-Accept` == base64(SHA-1(key+GUID)) enforced (RFC
golden vector in the suite; kn_sha1 reused). Frames: client masking with
a FRESH CSPRNG mask key per frame (RFC 5.3), 7/16/64-bit lengths,
fragmentation join into a caller-owned buffer (never realloc →
ERR_BIG), ping→echoed pong inline, pong skipped+counted, close answered
(1000) + surfaced with the peer code, RSV/masked-server-frame/
fragmented-control/oversize-control/orphan-continuation = ERR_PROTO.
Mid-frame timeouts RESUMABLE (frame state in the struct). RNG: Win32
`rand_s()` (msvcrt — **no new PE import**; bcrypt/advapi32 deliberately
avoided), POSIX getrandom→urandom; RNG failure REFUSES the operation
(never send weak keys/masks). `test_knws` (11th suite): **84 checks** —
RFC 4648 base64 goldens, the RFC 6455 §1.3 accept golden, request-shape
inspection, every failure class, wire-exact frame bytes at the
125/126/65535/65536 boundaries, fragmentation, control semantics,
protocol-error matrix, oversize, chunked recv, timeout resume.
**`kn_rtsp`** (new): the pure client per docs/RTSP.md — serializer
(CSeq==Request-Id, exact reference shape), incremental parser (Bifrost
leading-CRLF padding dropped even while incomplete; `\r\n\r\n` AND `\n\n`
ends; LF-only lines; case-insensitive last-wins headers; junk
Content-Length→0; status line = FIRST line only; drain-then-error on
sequence mismatch with the Request-Id fallback; 4 MiB cap), request loop
(deadline over the injected clock, remaining-time recv waits, TEARDOWN
mismatch-skip flag, body aliases the accumulation buffer — the memmove
only shifts the tail, documented), and the toolset: `sdp_attribute` (the
`||`/`;;` WS-SDP quirks, x-nv-prefix-then-plain candidates, empty=absent),
`media_control` (skips `a=control:*`), `runtime_key` (64-hex validation,
uppercasing) + the `{key_id:024X}` salt rule, the 8-profile SRTP scan
(per-value priority, SETUP headers before DESCRIBE lines, default
AEAD_AES_256_GCM_8), `parse_video_peer` (source=/X-GS-ServerPort=,
span≥16 collapse, comma+case tolerance), the SETUP URI ladder (the
`streamid=video/0`→`/0` rule + absolute forms), `extract_describe`
(Session + video control required; V2 credential preference; pingVersion
default 6; disablePlay/nativeRtcOnBundlePort/rtcpOnSctp/unifiedSocket;
runtime key passthrough), `setup_video` (the FULL sweep: stop-on-
peerless-200 poison rule, CloudMatch bundle-peer fallback, 3×3 s retry
rounds on the injected sleep, per-candidate remaining-deadline timeout
narrowing, the two Transport forms with the exact
`unicast;X-GS-ClientPort={p}-{p+1}` bytes, 400/404/459/460/461 continue
class, fatal-status rule, x-nv-ping/-payload extraction),
`build_announce` (the complete §3 attribute list incl. the color lines
and trailing empty line, CRLF-joined) and `negotiate_announce` (the
16-rule offered_in range table, min>max fallback, repair clamp,
initial/peak/ceiling bitrate clamps, line-substitution rewrite,
negotiated-values output for the evidence log). `test_knrtsp` (12th
suite): **127 checks** — every fixture TRANSCRIBED from the reference's
own tests (parser :3443-3470, official-control :3470+, announce params
:2923+) + scripted-server E2E of the full
OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY with byte-exact request inspection
and the D14 720p30 announce + TEARDOWN skip + every SETUP branch +
error paths. Injected clock/sleep make retry rounds instant+countable
(the rio shim ADVANCES the clock on timeout — the first draft hung
because a frozen fake clock never met a deadline; documented in-suite).
**Self-review + KAT catches this round (all fixed pre-gate):** (1) the
kn_ws mid-payload timeout was NOT resumable in the first draft (ws_take
lost its partial count → the retry restarted the header parse inside the
payload; frame-state fields added); (2) RT_FOR_LINES nested two for-loops
→ body ran per CHARACTER (invisible on CRLF fixtures — the trailing-\r
trim masked it; the LF-only KAT caught it); (3) the parser searched the
status line across the WHOLE header block (a header's colon-space could
fake a status; the GARBAGE KAT caught it); (4) blank SDP lines aborted
the attribute scan instead of being skipped (the `;;` quirk KAT caught
it); (5) the test-side scripted responses needed sequential CSeq values
(a fixed CSeq exposed the mismatch paths as hangs/fails — the suite now
doubles as the counter-increment proof); (6) a test buffer sized 64 for a
67-byte worst case (-Wformat-truncation at -O1; R28 discipline applies
to tests too → 96).
**Gates (sandbox):** native **12 suites, 1183 checks PASS** (972 + 84 +
127); cross mingw x64+i686 `-Werror` PASS; ASan+UBSan+LSan clean on both
new suites (the single runtime error remains the vendored sha1.c:26
padding note — zero streamer-code diagnostics); `-Wformat-truncation=2`
clean -O0..-O3 on kn_ws.c/kn_rtsp.c/test_knrtsp.c; CMake ctest **14/14**
both TLS backends; T10 POSIX dry-run ALL PASS **60**.
**Awaiting:** target T10 r15 (rebuild + the two new suites; expect ALL
PASS **71**). No T13/GPU rerun needed (no socket/graphics code touched).
Round XXI then wires the production path (Next steps 1a-1e).
→ `streamer/{include/kn,src}/kn_ws.*` (new), `streamer/tests/test_knws.c`
  (new), `streamer/{include/kn,src}/kn_rtsp.*` (new),
  `streamer/tests/test_knrtsp.c` (new), `CMakeLists.txt`,
  `tools/{build_native_tests,cross_check}.sh`,
  `tests/T10_build_and_test.py` (12 suites), `docs/RTSP.md` (§8 +
  deviations D-w1..D-r4), `INTEGRATION.md` rev 11, this file

### 2026-10-25 · impl round XXI (T10 r15 ALL PASS 71 → the stage-2 PRODUCTION SIGNALING PATH: TLS stream + kn_json/kn_http/kn_cm + fake-rig E2E + knvstd --session)
**Context:** same session (user: "zajmij się wszystkim co podałeś do rundy
XXI"). User delivered T10 r15 first: **ALL PASS 71/71** — the round-XX
cores (test_knws, test_knrtsp) passed on Win7/GCC 16.2 first try;
archived `artifacts/T10-round15/`. Then this round implemented the whole
remaining plan from docs/RTSP.md §6 (stages 3-5) + the OAuth research.
**OAuth research (docs/AUTH.md, new):** the login is the OAuth 2.0 DEVICE
grant (RFC 8628) against login.nvidia.com — `device/authorize` (form:
client_id/scope/device_id/display_name/idp_id; Steam-Deck browser
identity headers) → `token` poll (grant_type device_code; pending/slow_
down+5s/expired/access_denied) → access_token + **id_token (the GFNJWT
service token** — `service_token()` = id_token ?? access_token, gfn.rs
:254) + refresh (10-min window). CloudMatch auth: `Authorization: GFNJWT
{token}` + the NATIVE identity headers (docs/AUTH.md §4). Implementation
stays stage 4 (TUI); stage-2 bring-up = pasted `--session-token`
(length-only logging). NOT VERIFIED: the device-authorize RESPONSE field
names beyond device_code/interval (measure at stage 4).
**Round-XXI delta (all sandbox-proven; docs/RTSP.md §9 is the record):**
- **`kn_net_tls_stream_*`** — the persistent verified TLS-1.2 stream
  (Win32+mbedTLS): the T12-proven policy object verbatim (VERIFY_
  REQUIRED, SNI, ROOT stores, min=max 1.2, PSA), port-parameterized TCP
  (RTSPS = 322!), per-call SO_RCVTIMEO, recv contract >0/0/-1/-2. Other
  builds: honest stubs (exactly one definition set per configuration via
  complementary guards); `--session` refuses loudly without it — NO
  plaintext fallback anywhere. First target compile = T10 r16.
- **`kn_json`** (zero-alloc reader: 1024-node pool, depth 32, raw spans,
  unescape incl. surrogate pairs, int64-exact) + **`kn_http`** (HTTP/1.1
  one-shot: CL/chunked/read-to-EOF, resumable chunked parser, injected
  clock) + **`kn_cm`** (create body field-for-field per build_create_body
  incl. the Content-Type:text/plain quirk + GFNJWT header; poll; the
  session_info extraction SUBSET with the rtspsEndpoints derivation from
  connectionInfo (usage 16 / 14+alp{1,6}), media usage 2|17, queue
  4-source fallback, phase/ready/resumable rules). `test_kncm` (13th
  suite): **91 checks**.
- **`kn_sock_udp_connect`** — kernel peer filter for the media flow +
  getsockname refresh so the ANNOUNCE carries the REAL route source
  address (like the reference's reserved bundle socket). test_knsock
  70→**82**.
- **`tools/fake_rig.py` + `tools/sandbox_session_e2e.c`** — the two-
  implementations rule at the signaling layer: an independent Python rig
  (own WS framing + CloudMatch fixtures) vs the C client over REAL
  loopback sockets: **36/36** through create→poll→WS handshake→OPTIONS→
  DESCRIBE→SETUP→ANNOUNCE(negotiated)→PLAY→handoff(salt 0x123B84 rule,
  GCM256-8 default, SRTP session or honest NO_CIPHER)→connected UDP.
- **`knvstd --session`** (TLS-only): phase A CloudMatch (queue JSONL
  events = the future TUI feed) → B RTSPS/WSS full sequence (nativeRtcOn
  BundlePort enforced, legacy transport refused) → C SETUP sweep +
  D-s1 placeholder ICE identity + negotiated ANNOUNCE + PLAY → D SRTP
  material (DESCRIBE runtime key or local CSPRNG; salt={key_id:024X};
  profile string→enum map for all 8) + connected UDP + kn_rx arena → E
  the round-XVII media loop VERBATIM (dec_* machinery: capacity-2 queue,
  overflow policy, verify-compatible summary — a real session's stdout
  feeds make_rx_capture --verify unchanged). TEARDOWN on exit (1 s,
  mismatch-skip, reference Drop semantics). Token: length-only in logs;
  key material: never (handoff line carries shapes/lengths only).
  New deviations: **D-s1** (placeholder DTLS fingerprint until the
  stage-3 R3 spike — video/SRTP is bundle-independent), **D-cm1**
  (networkTestSessionId null), **D-cm2** (SubSessionId = device id),
  **D-cm3** (R22 subset until stage 4).
**Bugs caught by the gates this round (all fixed pre-handover):** the
kn_http chunked parser stranded its cursor on '\n' under 1-byte delivery
(KAT caught); kn_net.c's TLS-stream stubs needed a top-level <stdio.h>
(POSIX -Werror caught); the E2E's hex expectations mis-computed 1194884
(=0x123B84, not 0x123BC4 — the reference-derived salt rule itself was
right, proven by the unit KATs); a replace-all patch briefly injected
session memsets into run_decode (compile caught, restored — run_decode
is T13-verified and stayed byte-identical).
**Gates (sandbox, final tree):** native **13 suites 1309 checks PASS**;
cross x64+i686 -Werror PASS; knvstd =2-paranoid clean; CMake 0 warnings
+ ctest **15/15** both backends; **T13 ALL PASS 51 / 51 (ASan+UBSan+
LSan) / 63 (mbedTLS+gcm256)**; sandbox_session_e2e **36/36**; T10 dry
**61**.
**Awaiting (round XXII = the R7 measurement round):** target **T10 r16**
(expect ALL PASS **72** = r15's 71 + test_kncm; first compile of the
Win32 TLS-stream body + --session), then the FIRST REAL SESSION attempt
per Next steps 2 (token + appId; every outcome is evidence). T13 r3
optional (unchanged, 61). No GPU rerun needed.
→ `streamer/{include/kn,src}/kn_json.*`, `kn_http.*`, `kn_cm.*` (new),
  `streamer/tests/test_kncm.c` (new), `streamer/include/kn/kn_net.h` +
  `src/kn_net_mbedtls.c` + `src/kn_net.c` (TLS stream),
  `streamer/{include/kn,src}/kn_sock.*` (connect),
  `streamer/src/knvstd_main.c` (--session), `tools/fake_rig.py` (new),
  `tools/sandbox_session_e2e.c` (new), `docs/AUTH.md` (new),
  `docs/RTSP.md` §9, `CMakeLists.txt`, `tools/{build_native_tests,
  cross_check}.sh`, `tests/T10_build_and_test.py`, `INTEGRATION.md`
  rev 12, `artifacts/T10-round15/` (gitignored), this file

### 2026-10-25 · impl round XXII (T10 r16 build FAIL → root-caused, fixed, and the gate gap closed PERMANENTLY; project staged for a fresh-agent handoff)
**Context:** user ran T10 r16 on the target: **FAIL (2/72)** — `ninja
build` failed compiling `kn_net_mbedtls.c` (`'port' undeclared` at :453
inside `kn_net_dns_lookup`) and `test_kncm` cascaded (never linked).
Evidence excerpt archived: `artifacts/T10-round16/`.
**Root cause (ONE bad round-XXI edit):** the `kn_tcp_open` →
`kn_tcp_open_port` refactor was applied with a Python `str.replace`
WITHOUT a count — the `getaddrinfo(host, "443", …)` pattern occurred
TWICE in the file, so the port-parameterized snippet also landed in
`kn_net_dns_lookup`, which has no `port`. **Why every sandbox gate missed
it:** kn_net_mbedtls.c's Win32 body is never compiled in the sandbox —
POSIX cmake builds take the file's `#else` stub section and cross_check.sh
did not compile the file at all (no mbedTLS existed as a cross
dependency). The target's GCC 16.2 was the first compiler to see that code
this round. The two-tier gate (R26/R28) worked exactly as designed — this
is the fourth target-catch incident and each one has tightened a gate.
**Fixes:** (1) `kn_net_dns_lookup` restored to the round-XV-verified
`getaddrinfo(host, "443", …)`; (2) **a SECOND latent bug surfaced by the
new cross-compile**: the round-XXI TLS-stream body used calloc/free
without `<stdlib.h>` — x64 mingw headers dragged stdlib in transitively,
**i686 did not** (target GCC happened to as well; still a landmine) →
explicit include added with a comment; (3) **the gap is closed
permanently**: `cross_check.sh` now cross-compiles `kn_net_mbedtls.c`
(Win32+mbedTLS body) on BOTH arches whenever `third_party/mbedtls` is
present (gitignored tree; skipped honestly when absent). Verified with
-std=c11 AND the exact target flag set (gnu11 -O3 -DNDEBUG -march=core2
-msse3 -Wall -Wextra -Werror): clean.
**Process lesson (recorded where it will be seen): NEVER use uncounted
str.replace on a file with repeated patterns, and treat "code the sandbox
cannot compile" as a standing risk that needs its own gate** — the
cross_check addition is that gate for the last such file.
**Full sandbox re-verification after the fixes (final tree):** native 13
suites PASS (1309 checks); cross PASS incl. the new kn_net_mbedtls legs;
sandbox_session_e2e **36/36**; T13 ALL PASS **51 (FFmpeg) / 51
(ASan+UBSan+LSan) / 63 (mbedTLS+gcm256)**; CMake FRESH reconfigure both
backends: 0 warnings, ctest **15/15 ×2**; T10 POSIX dry-run ALL PASS 61.
**HANDOFF STATE (user is moving to a fresh chat window — this file + the
repo are the complete context; no chat history needed):** everything
through round XXII is in the workspace tree; the user commits/pushes
(the agent cannot). A fresh agent: read THIS file's Bootstrapping section,
clone the repo + OpenNOW @ a49b124, reinstall the sandbox toolchain
(does NOT persist between messages — R26), run the two gate scripts, and
continue at Next steps 1 (target T10 r17, expect ALL PASS 72) then 2 (the
R7 first-real-session round: needs a GFNJWT token + numeric appId from
the user; docs/AUTH.md §3 documents the device flow; every outcome of
that run is a recorded measurement — D-s1/D-cm1/D-cm2/R34-firewall all
get their answers there). Round-XXI deliverables awaiting that first
compile-on-target were partially de-risked this round: the kn_net_
mbedtls.c Win32 body (incl. the TLS-stream implementation) now compiles
clean under the exact target flags in the sandbox.
→ `streamer/src/kn_net_mbedtls.c` (both fixes), `tools/cross_check.sh`
  (the permanent kn_net_mbedtls cross-compile), `artifacts/T10-round16/`
  (evidence, gitignored), this file

### 2026-10-25 · impl round XXIII (R7 runs 1-4: the FIRST real CloudMatch traffic → the control-host bug FOUND+FIXED; the poll loop hardened to the reference's semantics)
**Context:** same handoff chain (this file + the repo). The user ran T10
r17 on the target (**ALL PASS 72** — the round-XXII fixes confirmed, the
Win32 TLS-stream body + --session compiled clean on GCC 16.2 first try),
then executed the R7 first-real-session plan: a GFNJWT id_token pulled
from the browser + appId 107057914 → four consecutive live `knvstd
--session` runs against the PRODUCTION backend, plus a parallel DevTools
capture of the official WebRTC client and a full captured poll-response
body. This round is the analysis + fix; the record lives in
**docs/RTSP.md §10**.
**The measurement (runs 1-4):** create works — every run produced a real
sessionId, status 1, and a plausible live queuePosition (167/166/181/178
while the browser saw 161 in the same window; D-cm1 networkTestSessionId
null + D-cm2 SubSessionId=device-id ACCEPTED at create, 4/4). The queue
wait failed: the position FROZE after 0-1 updates (run 2: `163` ×40
polls; run 3: `176` ×30) and every run ended in
`not_ready/cm_poll_failed` after 2-43 polls. The browser capture: the
official client polls the REGIONAL node
`https://np-waw-01.cloudmatchbeta.nvidiagrid.net/v2/session/{id}` every
~4.1 s and its position MOVES (161→151→147→135 in 10 s); the captured
body nominates that node in `sessionControlInfo` (ip/resourcePath/
usage 10/port 443).
**Root cause (ONE contract violation):** phase A polled the CREATE host
(`prod.cloudmatchbeta.nvidiagrid.net` — the reference's
DEFAULT_STREAMING_URL, gfn.rs:38) for the whole wait. The reference
learns `control_base = https://{sessionControlInfo.ip}` (cloudmatch.rs
:1966-1984, trust-gated :2400-2456) at create (:401-414) / attach
(:1149-1152) and EVERY poll goes ONLY there (:652-656 → get_session
:1356). The global front answers with stale snapshots + intermittent
non-200s — exactly the freeze-then-fail signature.
**The fix + hardening (all reference-mirrored):** `kn_cm_session` learns
control_host/port + requestStatus code/desc (filled even on failed
extract — a rejected payload now says WHY, e.g. statusCode 11
SESSION_LIMIT :2354+); NEW `kn_cm_control_base` (the trust mirror of
trusted_learned_server_base + public_session_host: https-only, 443,
private/loopback/link-local/CGNAT/multicast/reserved/.local/userinfo/
path rejected — a response can never redirect polling into a private
address; 0 → create-host fallback = the :1984 unwrap_or semantics, which
also keeps the loopback fake-rig E2E unchanged); NEW `kn_cm_cancel`
(DELETE v2/session/{id} = stop_at :789-800, validate_delete_response
:2728 — 2xx/404 = gone); phase A: the poll-host switch (+ the
`cm_control` evidence event), cadence 2 s → 4 s (the capture's Δ≈4.1 s),
3-attempt transient retry (408/425/429/5xx, 250/750 ms — get_session
:1369-1394), 404 → session_not_found, statusCode≠1 → failed round with
the cloud's description (`cm_poll_error` events), failed rounds NEVER
clobber the live session state (poll-into-temp + commit — the old loop
let one bad extract wipe session_id), 4 consecutive bad rounds tolerated
before cm_poll_failed, `not_ready` carries rc+http_status, best-effort
`cm_cancel` on every not_ready exit except 404 (runs 1-4 each ORPHANED a
queued session — no cancel existed then), `cm_created` includes the
initial queue_position, `--session-queue-timeout` default 180 → 600 s.
**Gates (sandbox, final tree):** native 13 suites PASS — test_kncm
**91 → 147 checks** (control-base battery incl. the exact R7 capture
fixture + cancel wire checks; also clean under ASan+UBSan+LSan); cross
x64+i686 -Werror PASS; kn_cm.c + knvstd_main.c clean under
-Wformat-truncation=2 (-O1 host, -O2 mingw both arches); CMake fresh
both backends 0 warnings + ctest 15/15 ×2; sandbox_session_e2e
**36/36** (exercises the FALLBACK branch — the rig's 127.0.0.1 control
host is rejected by design; the ACCEPT path is unit-pinned); T10 POSIX
dry-run ALL PASS **62** — 62 is also the pre-delta baseline on this tree
(verified by a stashed rerun; the round-XXI "61" predates a since-added
check).
**Awaiting (round XXIV):** target T10 r18 (expect ALL PASS 72), then
**R7 run 5** — the same command; expect `cm_control` with
`source:"sessionControlInfo"`, a MOVING queue_position, and if a seat
arrives the first real phase B-E (D-s1's first answer). Known gaps
carried: phases B-E aborts don't cancel yet; IPv6 trust checks are
prefix-class approximations (documented).
→ `streamer/{include/kn,src}/kn_cm.*`, `streamer/src/knvstd_main.c`
  (phase A), `streamer/tests/test_kncm.c`, `tools/fake_rig.py` (DELETE
  branch), `docs/RTSP.md` §10, `INTEGRATION.md` rev 13, this file

### 2026-10-25 · impl round XXIV (R7 run 5: the control-host fix CONFIRMED LIVE — first READY session + first real RTSPS traffic; DESCRIBE rc:-2 root-caused to the 8 KiB per-message scratch → fixed + binary frames + Host:port + rtsp_detail)
**Context:** same handoff chain. The user applied the round-XXIII patch
on the target and reran the SAME live command (real GFNJWT + appId
107057914). The run is the round-XXIII fix's confirmation AND the next
measurement: docs/RTSP.md §11 is the full record.
**Run 5 evidence (live production cloud):** `cm_control:
poll_host=np-waw-01.cloudmatchbeta.nvidiagrid.net, port=443,
source=sessionControlInfo` — the learned-control-node switch fired. The
session PROGRESSED through the real queue: `queue_position:81,
seat_step:1` ×29 polls (~2 min), then `seat_step:3` with queuePosition
gone, then `status:2 ready` with `rtsps://80-84-174-149.cloudmatchbeta.
nvidiagrid.net:322` (media_peer ":0" — the ready response carried no
usage-2/17 connection; honest, the SETUP sweep provides the video peer).
Phase B then worked at the transport level against a REAL GFN rig: the
WS handshake (x-nv-sessionid) + `OPTIONS rc:0 status:200`. The run died
at `DESCRIBE rc:-2 status:0` (KN_RTSP_ERR_IO, no response parsed).
**Queue-position observation (recorded, NOT a defect):** the NATIVE
session's position did not tick per poll — the server published step
transitions (1→3→ready) instead; the user's moving browser numbers were
a WebRTC-transport session. Client-side correctness is proven by the
transitions + ready. Keep observing.
**Diagnosis (three real deviations from nvst_rtsp.rs; without wire logs
#1 and #2 are indistinguishable as THE run-5 cause — both yield exactly
rc=-2/status=0 at the first big response — so all three are fixed):**
(1) `kn_rtsp_request` read each WS message into a `char scratch[8192]`
— a real DESCRIBE SDP routinely exceeds 8 KiB while OPTIONS responses
are a few hundred bytes (the observed signature); the reference has no
per-message cap below the 4 MiB total (tungstenite reassembles whole
messages, :559-566, and appends text AND binary to one buffer,
:614-623). (2) The adapters refused BINARY frames the reference accepts
(:621). (3) The Host header lacked the port (the reference strips only
the scheme, :1070-1076 — the rig tolerated it for OPTIONS, fidelity is
the R32 rule).
**Fixes:** kn_rtsp_request reads DIRECTLY into the accumulation buffer's
tail (seam contract unchanged, runaway-shim guard kept); both C adapters
accept TEXT|BINARY; knvstd + E2E build the Host authority from the rtsps
endpoint (host:port); NEW `sess_rtsp_detail` evidence line at all five
step-failure sites (kn_rtsp err[160] + ws_err/ws_close/ws_text/ws_bin —
a server close now shows as ws_err:-7 + the close code instead of a bare
-2); the fake rig delivers its DESCRIBE SDP padded to ~11 KiB as TWO
binary messages (the run-5 shape over real sockets).
**Gates (sandbox, final tree):** native 13 suites PASS — test_knrtsp
**127 → 132** (a >19 KiB single-message DESCRIBE parses — the old
scratch died here; the same response split across two messages
accumulates), test_kncm 147; cross x64+i686 -Werror PASS; paranoid
-Wformat-truncation=2 clean (host -O1 + mingw -O2 both arches) on every
touched .c; CMake fresh both backends 0 warnings + ctest 15/15 ×2;
sandbox_session_e2e **36/36** with the NEW rig behaviour (binary +
split + padded SDP; the rig still asserts the announce key echo +
plays=1); T10 POSIX dry-run ALL PASS 62 (baseline).
**Awaiting (round XXV):** the next target T10 (expect ALL PASS 72),
then **R7 run 6** — expect DESCRIBE 200 → the SETUP sweep → ANNOUNCE
(D-s1's first real verdict) → PLAY → the media loop; any failure now
names its stage in rtsp_detail. Known gaps carried: phases B-E aborts
still don't DELETE the session (round XXV candidate); D-cm1/D-cm2
remain measured-at-create-only.
→ `streamer/src/kn_rtsp.c` (direct accumulation),
  `streamer/src/knvstd_main.c` (binary acceptance + Host authority +
  rtsp_detail), `tools/sandbox_session_e2e.c` (mirrored),
  `tools/fake_rig.py` (padded binary split DESCRIBE),
  `streamer/tests/test_knrtsp.c` (test_big_messages), `docs/RTSP.md`
  §11, `INTEGRATION.md` rev 14, this file
