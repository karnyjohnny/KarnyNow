# KarnyNow — bootstrap document for the implementing agent

> **Read this first, in full.** You are the second agent on this project. A research agent
> spent several sessions establishing the facts below and writing them down so that you do
> not have to rediscover them. You are **not** being handed an implementation: you will
> design and write everything yourself. This document is a **map** — it tells you what is
> already known, what is already measured, where the remaining unknowns are, and which
> mistakes have already been made once so you don't repeat them.
>
> **Language rule (hard requirement):** the application language is **English**. All user-
> facing strings, all log messages, all error messages, all exception texts, all code
> comments, all commit messages, and all documentation you produce are in **English**.
> Internal design notes you exchange with the user may be in Polish, but anything that ends
> up in the repository, in a log file, or on the screen is English.
>
> **Working rule (hard requirement):** *measure first, then code.* Every stage has a gate.
> Do not advance past a failed gate by assuming it will work out.

---

## 1. Mission

Build a **GeForce NOW client** for hardware on which neither the official NVIDIA client nor
[OpenNOW](https://github.com/OpenCloudGaming/OpenNOW) can run:

- **Frontend:** a **TUI in Python** — login, game list, queue, launch. Runs in a console.
- **Streaming engine:** **plain C** — NVST transport, video decode, audio, input, presentation.
- **Reference material:** OpenNOW (`main`, commit `a49b124`, **MIT license**). You take
  *protocol knowledge* from it, not code.

The user's own Python code for login/catalog/session already exists and works (~3 000 lines
across 4 files). **You will not receive it.** Its verified findings are summarised in §7 and
in `RESEARCH-INDEX` entries; you re-implement from the same public sources, your own way.

### Non-negotiable rules (user's, from the project charter)

- Do not bypass login, entitlements, subscription checks, or queue limits.
- Do not steal cookies or tokens from other clients.
- **Never disable TLS verification.** If the certificate store is the problem, diagnose and
  report it — do not work around it.
- Do not implement your own cryptography. Use vetted libraries.
- In MOCK mode, do not touch the network at all (the mock adapter has no HTTP client, so
  there is no way to send anything — keep it that way).

---

## 2. Target machine (also the development machine)

Both build and run happen on this one laptop. There is no CI, no second machine, and no
emulator. Design for it.

| Item | Value | Consequence |
|---|---|---|
| Model | Dell Latitude E5500 | — |
| CPU | Intel Core 2 Duo **T7250**, 2 cores / 2 threads, Merom, 2.0 GHz, FSB 800 | **SSE3 + SSSE3, no SSE4, no AVX** → `-march=core2 -msse3` |
| RAM | **2 GB** DDR2 | ~1.25 GB free in practice. Every buffer bounded. |
| GPU | Intel **GMA 4500MHD**, PCI `8086:2A42` | D3D 10.0 / SM 4.0 / GL 2.1. **No D3D11. No DXVA2.** |
| Driver | `8.15.10.2869`, `igdumd64.dll` (x64) / `igdumdx32.dll` (x86) | The `Description` string says "Intel(R) HD Graphics Family" — **the name lies; the PCI ID is the truth** |
| Display | 1280 × 800, **60 Hz** | Was reported as 40 Hz earlier — Intel **DPST** changes it on battery. See §9 R9. |
| OS | Windows 7 SP1 **x64** (6.1.7601), JG 2021 modification | Python ≤ 3.8.x; stock conhost has **no ANSI parser** |
| Terminal | **ConEmu** build 230724, `C:\Program Files\ConEmu\ConEmu`, elevated (ADMIN) | **Required.** See D1. |
| Network | Wi-Fi 2.4 GHz or USB 2.0 phone tethering | Packet loss and jitter are normal. FEC and NACK must actually work. |
| Toolchain | **w64devkit**, GCC **16.2.0**, mingw-w64 **v14.0**; cmake 4.4.3, ninja 1.13.2, make 4.4.1, git 2.46.2 | See §9 F1 for the canonical flag set |
| Python | **3.8.20** x64, `C:\Python\Python38\python.exe`, ssl **OpenSSL 1.1.1w** | Only `pip` + `setuptools` installed. Pin everything; cp38 wheels only. |
| FFmpeg | `2026-02-18-git-52b676bb29-essentials_build-www.gyan.dev`, static, `--enable-gpl` | License is **GPL** → the whole client is GPL. Accepted (D3). |
| Not on PATH | `bash`, `powershell`, `py`, `nasm`, `dumpbin`, `cl`, `clang` | Test scripts must be pure Python runnable from `cmd`/ConEmu |

---

## 3. Architecture you must build

```
┌──────────────────────────────┐        ┌───────────────────────────────────┐
│  karnynow.exe  (Python 3.8)  │        │  knvstd.exe  (C11, mingw)         │
│  MUST be launched via ConEmu │        │                                   │
│                              │ JSON   │  • CloudMatch → session context   │
│  • TUI: Textual/Rich         │─lines─▶│  • WSS + RTSP (OPTIONS..PLAY)     │
│  • OAuth 2.0 device grant    │ stdout │  • Mjolnir UDP 49005: SRTP,       │
│  • GraphQL catalog/library   │◀───────│    GS 0x4753, reorder, RS-FEC,    │
│  • v2/serverInfo → vpcId     │ events │    NACK-v2 0x0317                 │
│  • v2/session (queue)        │        │  • bundle 49006: ICE + DTLS +     │
│  • DPAPI vault for tokens    │        │    SCTP → Opus, input, RTCP       │
│  • console host detection    │        │  • ffmpeg libavcodec h264 → NV12  │
│  • structured logging (§8)   │        │    (threads = 1)                  │
│                              │        │  • WASAPI (audio out)             │
└──────────────────────────────┘        │  • D3D9 → GDI presentation chain  │
         ▲                              │  • Raw Input → NVB frames         │
         │                              │  • structured logging (§8)        │
   ConEmu window                        └───────────────────────────────────┘
                                                     │
                                          separate WS_POPUP window 1280×800
```

**Two processes, not `ctypes`.** Reasons, in order of importance:

1. **A driver crash is a real scenario on this machine, not a theoretical one.** The D3D9
   probe crashed with `0xC0000005` inside `UpdateSurface` on NV12 (§9). Process separation
   means a presentation crash cannot lose the OAuth tokens or the session state.
2. OpenNOW uses exactly this pattern and it works: newline-delimited UTF-8 JSON over
   inherited stdin/stdout, **1 MiB line limit**, stderr for redacted diagnostics, 5 s
   handshake deadline, `unknown`/`malformed`/`oversized` → disconnect rather than sit in an
   ambiguous state. Read `docs/core-protocol.md` in the OpenNOW repo and copy the *contract*
   (versioning, capabilities, error handling).
3. GIL and `timeBeginPeriod`: the decode loop and presentation loop need a hard timer and
   thread priorities. Fighting the interpreter for that is pointless.
4. Python never touches pixels or packets, so its EOL status does not affect the media path.

**Streamer run modes** (so you can develop without an account):

- `--loopback` — synthetic H.264 from a file → the whole decode + present path
- `--capture`  — Mjolnir only → dump access units to `.264` (diagnose without a decoder)
- `--replay`   — read a recorded wire transcript (§8.7) instead of the network
- `--real`     — full session

---

## 4. Decisions already made — do not re-litigate

Each has a reason and a reversal condition. If you think one is wrong, say so explicitly
with evidence; do not silently work around it.

| # | Decision | Reason | Reversal condition |
|---|---|---|---|
| **D1** | **ConEmu is a required console host.** The TUI is Textual/Rich. At startup the client **detects the host** and, if VT is unavailable, exits with a clear message and code 2 instead of drawing garbage. | Measured both ways: under ConEmu, `ConEmuHk64.dll` is loaded in-process and ANSI works (attributes `0x4`/`0xC`/`0x9`/`0xF` applied, `ESC` consumed). Under stock `cmd.exe` there is no hook DLL and the `ESC` byte **lands in the console buffer as a character**, which the raster font draws as `←`. Third reason: the stock profile has `FaceName=null` = raster font with **no Unicode box-drawing glyphs**. | If ConEmu becomes unavailable → own Win32 backend (`WriteConsoleOutputW` measured at **0.1 ms** per full 80×25 screen, so it is cheap) + ASCII boxes. |
| **D2** | **Default 1280×720@30**, switchable via `ANNOUNCE` (`clientViewportWd/Ht`, `maxFPS`). Lower rungs: 540p60, 540p30, 360p30. **1088p rejected.** | Measured: 720p30 decode costs **24.8 %** of both cores; with a 1.8× factor for real gameplay content ≈ 45 %. 720p60 = 46 % (≈83 % with the factor) → too much as a default. 1088p is pointless because the panel is 1280×800. | If 720p30 stutters in practice → 540p60 (28.7 %). |
| **D3** | **Decoder: FFmpeg `libavcodec` h264** from the existing build. openh264 was considered and **dropped** (its meson build does `find_program('nasm')` without `required:false`, so no NASM = no build). Whole client is **GPL**. | User decision + measurement: FFmpeg decodes 720p30 at 211 fps = **7× real-time**. | If GPL ever becomes a problem → openh264 (BSD-2), which needs NASM ≥ 2.10.06. |
| **D4** | **Decoder `threads = 1`.** | Measured: 720p30 → 211 fps with 1 thread vs 191 fps with 2. On a 2-core Merom, decoder thread synchronisation costs more than it gains. The second core stays free for network, Opus, presentation, and the Python process. | If measurement on a live stream says otherwise. |
| **D5** | **Never attempt DXVA2** — not as a primary path, not as a fallback. | Measured: `-hwaccel dxva2` returns `rc=0` (looks like success!) while being **3-4× slower** than software (720p30: 50 fps vs 211 fps) because every driver call fails. Platform probe: `QI(IDirectXVideoDecoderService)=0x80004002`. | — (settled by measurement) |
| **D6** | **Scaling happens on the GPU.** If D3D9 cannot scale, present **1:1** (720p centred in a 1280×800 window with letterbox) — never stretch on the CPU. | Measured: `scale=1280:800` in software adds **11.8 ms/frame** (+18 percentage points of CPU: 24.8 % → 42.5 %). It is the single most expensive operation in the video path. | — |
| **D7** | **Presentation fallback chain, probed once at session start:** NV12 `StretchRect` → YUY2 → X8R8G8B8 → GDI `SetDIBitsToDevice`. **`CheckDeviceFormatConversion` before every `StretchRect`**; on refusal, do not call it. On `0xC0000005`, degrade permanently — never retry the failing operation. | The probe crashed the driver by calling into it against its own reported capabilities. **`UpdateSurface` from a SYSTEMMEM NV12 surface crashes** (see §9 R2). | T4 v4 results may shorten the chain. |
| **D8** | **Target: x86_64**, `-static -static-libgcc -static-libstdc++`. x86 is the fallback if RSS becomes critical. | Measured: both work, but x64 has better defaults (PE subsystem 5.2 vs 4.0) and 16 XMM registers. | If RSS exceeds budget at 2 GB RAM. |
| **D9** | **The client never assumes a fixed refresh rate.** It measures the interval between `Present` calls and adapts. Optionally (explicitly, reversibly) force 1280×800@60 via `ChangeDisplaySettingsW` at session start and restore after `TEARDOWN`. | Refresh reported 40 Hz once and 60 Hz later — Intel **DPST** lowers it on battery. OpenNOW's `PresentationClock` does the same thing: it ignores RTP timestamps and *learns* the arrival cadence. | — |
| **D10** | **The TUI is keyboard-only.** Arrows + Enter/Esc/letters. **Mouse support is not required** and the UI is not designed for it. `Textual(mouse=False)`. | User preference. Measurement confirms the keyboard path is complete: VK 13 (Enter), 37-40 (arrows), 65/90 (A/Z), 112 (F1). | — (user preference) |
| **D11** | **Resize via polling**, not events: each loop tick call `GetConsoleScreenBufferInfo`, compare `srWindow`, redraw on change. | Measured: no resize events arrive (`ESC[8;H;Wt` not delivered by ConEmu to the app; zero type-4 records in 20 s). Polling costs one API call per tick. | — |
| **D12** | **Application language is English** (§0). Logs, errors, UI strings, comments, commits — all English. | User requirement. | — |
| **D13** | **Logging is a first-class subsystem**, specified in §8, implemented in stage 1, not retrofitted. | Login, queue, and streaming **cannot be tested in a sandbox**. When they fail on the live machine, the log is the *only* evidence available. | — |

---

## 5. Research map — where to look for what

**Clone OpenNOW `main` first.** It is MIT-licensed; reading it and taking notes is exactly
what it is for. `git clone https://github.com/OpenCloudGaming/OpenNOW`.

The repository contains ~470 k lines of Rust, of which you need roughly **10-14 k**. Do not
read it linearly. Use this index.

### 5.1 Protocol documents (read these before any code)

| Topic | File | Why |
|---|---|---|
| Whole video path, plus official-client behaviour | `docs/streamer-comparison/video.md` | Mjolnir, GS extension, decode, present, recovery ladder, bitrate defaults |
| **Byte-exact NVB control frames** | `docs/streamer-comparison/features.md` | The activation sequence and exact payloads. Nothing else gives you this. |
| NACK-v2 `0x0317` wire format | `docs/mjolnir-nack-v2.md` | Field-by-field table, retry policy, RTT source priority |
| Audio: Opus/RED/jitter/WASAPI | `docs/streamer-comparison/audio.md` | PT 111/63, 48 kHz, 45 ms jitter target, 100 ms concealment cap |
| Mouse/controller input | `docs/streamer-comparison/mouse-input.md`, `controller-input.md` | Cursor modes, type-7 deltas, rumble |
| RTSP ordering and the PLAY gate | `docs/streamer-comparison/rtsp-negotiation.md` | What counts as "transport ready"; 455 = failed startup |
| Queue progress semantics | `docs/session-start-progress.md` | `seatSetupStep` meanings; **steps are not linear and yield no percentage** |
| IPC contract to copy | `docs/core-protocol.md` | JSON-lines, 1 MiB line, handshake, capabilities, error policy |
| Two-client comparison overview | `docs/streamer-comparison/README.md` | Ports 49005/49006/5004, what official GFN logs say |

### 5.2 Source files (with line numbers, verified against commit `a49b124`)

| What you need | Where |
|---|---|
| Transport constants (timeouts, windows, capacities) | `native/opennow-streamer/crates/opennow-streamer-transport/src/nvst.rs:79-296` |
| RTP header + GS `0x4753` extension parsing | `nvst.rs:2440-2515` |
| GS 16-byte body layout (little-endian!) | `nvst.rs:3217-3257` (`NvVideoPacket::parse`) |
| Locating the H.264 payload (Annex-B start code) | `nvst.rs:3481-3495`, `find_annex_b_start_code` at `3525` |
| Keyframe detection | `nvst.rs:3500-3523` |
| Trailing AUD stripping | `nvst.rs:3458-3479` (`strip_trailing_access_unit_delimiter`) |
| SRTP profiles and their SDP names | `nvst.rs:301-329` |
| SRTP master material from RTSP (`srtpAesKeyHex`, `srtpSaltHex`) | `nvst.rs:1586-1640` |
| **SRTP KDF** (RFC 3711 AES-CM, labels) | `nvst.rs:2873` (`derive_aes_cm_key`), `2888` (`derive_aes128_cm_key`) |
| **SRTP IVs** | `nvst.rs:2902` (`srtp_aes_cm_iv`), `2915` (`srtp_gcm_iv`) |
| `unprotect` + replay ordering | `nvst.rs:2635-2705` |
| AES-GCM with 8-byte tag | `nvst.rs:2711-2790` |
| AES-CM + HMAC-SHA1 (note `be32(roc)` in the MAC) | `nvst.rs:2820-2872` |
| SRTCP | `nvst.rs:4357-4372` |
| FEC block layout from the packet | `nvst.rs:3668` (struct), `3677-3701` (`from_packet`) |
| **GF(2^8) tables, polynomial `0x11d`** | `nvst.rs:3717-3742` |
| `gf_mul` / `gf_inv` / `axpy` / `scale` | `nvst.rs:3744-3783` |
| **Cauchy coefficient** | `nvst.rs:3785-3793` |
| RS reconstruction + the metadata caveat | `nvst.rs:3798-3880`, and the critical comment at `4103-4106` |
| SCTP data-channel labels and reliability | `nvst_input.rs:14-110` |
| RTSP over WebSocket, DESCRIBE/SETUP/ANNOUNCE/PLAY | `opennow-streamer-core/src/nvst_rtsp.rs:1052-1180` |
| **Full `ANNOUNCE` SDP** (all `x-nv-*` attributes) | `nvst_rtsp.rs:1520-1620` |
| CloudMatch `POST v2/session` body | `opennow-core/src/cloudmatch.rs:1643-1760` (`build_create_body`) |
| **The 31-field session context** the streamer consumes | `cloudmatch.rs:2070-2102` (`session_info`) |
| Media/bundle endpoint extraction | `cloudmatch.rs:1866-1881` (`media_connection_info`) |
| ICE server normalisation + STUN fallbacks | `cloudmatch.rs:2630-2654` |
| Queue position (4 possible locations) | `cloudmatch.rs:2655-2663` |
| Session status → phase | `cloudmatch.rs:2698-2708` |
| Resume body | `cloudmatch.rs:1540-1598`, gate at `1506` |
| Orphaned-session cleanup | `cloudmatch.rs:580-611`, `3527` |
| CloudMatch headers (`GFNJWT`, `text/plain`!) | `cloudmatch.rs:2263-2286` |
| LCARS headers (`GFNJWT`, `GENERIC`/`PC`) | `gfn.rs:3614-3672` |
| `session_token()` = id_token else access_token | `cloudmatch.rs:2918-2923` |
| Device profiles (`Windows`, `UNKNOWN`, UA Bifrost) | `cloudmatch.rs:2979-3033` |
| DLSS overrides normalisation | `dlss.rs:19-102` |
| **Catalog pagination guards** (`catalog_changed`, revision checks, 14-day language cache) | `gfn/catalog.rs:214`, `403-418` |
| Raw Input capture pattern (pure Win32, portable to C 1:1) | `opennow-streamer-platform/src/windows_raw_input.rs` |

### 5.3 What you do **not** need

`opennow-qt/` (96 767 lines), `opennow-streamer-platform-windows/` (11 452 lines of
MF/D3D11/DXGI — unusable here), `opennow-streamer-hid/` (gamepads), `opennow-streamer-ffi/`
(the Qt C ABI), HEVC/AV1/HDR/microphone/recording/replay branches, updater, analytics,
Discord RPC. Skipping these is what turns a 470 k-line problem into a ~12 k-line one.

---

## 6. Verified facts you can rely on

These were established by measurement on the target machine or by reading the reference
source with line numbers. Treat them as ground truth; re-measure only if something
contradicts them.

### 6.1 CPU budget (measured, `T1`)

H.264 High@5.1, yuv420p, `threads` as noted. `ms CPU/frame` is process CPU time per frame;
`%` is of **both cores** (200 % = fully saturated).

| Source | Config | fps | ms CPU/frame | @30 fps | @60 fps |
|---|---|---|---|---|---|
| 360p30 | decode, 1 thread | 720.7 | 5.20 | **7.8 %** | 15.6 % |
| 540p30 | decode, 1 thread | 344.3 | 10.53 | **15.8 %** | 31.6 % |
| 540p60 | decode, 1 thread | 385.2 | 9.55 | 14.3 % | **28.7 %** |
| **720p30** | **decode, 1 thread** | **211.1** | **16.52** | **24.8 %** | 49.5 % |
| 720p30 | decode, 2 threads | 191.1 | 17.16 | 25.7 % | — |
| 720p30 | + NV12 output | 191.5 | 19.23 | 28.9 % | — |
| 720p30 | + RGB24 (CPU CSC) | 169.5 | 22.10 | 33.2 % | — |
| 720p30 | + YUY2 (CPU CSC) | 132.8 | 28.60 | 42.9 % | — |
| 720p30 | + scale to 1280×800 | 133.4 | 28.33 | **42.5 %** | — |
| 720p60 | decode, 1 thread | 252.0 | 15.34 | — | **46.0 %** |
| 1088p30 | decode, 1 thread | 103.7 | 36.41 | 54.6 % | — |

**Reading these numbers:**
- Decoding is *not* the bottleneck. 720p30 leaves ~75 % of the machine for everything else.
- **CPU scaling is the most expensive single operation** (+11.8 ms/frame). Hence D6.
- CPU colour conversion is cheaper than feared: NV12 ≈ free, RGB +5.6 ms, YUY2 +12.1 ms.
  Prefer NV12 all the way to the GPU.
- **1 thread beats 2.** Hence D4.
- `-hwaccel dxva2` returns success while being 3-4× slower. Hence D5.

**Caveat:** the test clips were `testsrc2` + noise, `scenecut=0` — easier than real gameplay.
Apply a **1.3-1.8× factor**. Even at 1.8×, 720p30 is ~45 %.

Memory: 1 314 940 kB free before, 1 472 232 kB after, of 2 051 244 kB total.

### 6.2 Console / TUI (measured, `T3` + `T5` + `T7`)

| Measurement | ConEmu | stock `cmd.exe` |
|---|---|---|
| ANSI 16-colour | ✅ attributes applied | ❌ `ESC` becomes the glyph `←` |
| ANSI 256 / truecolor | ✅ parsed but **quantised to 16 colours** (`0xC`, `0x9`) | ❌ |
| alt-screen + cursor addressing | ✅ | ❌ |
| `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` | ❌ `ERROR_INVALID_PARAMETER (87)` | ❌ same |
| `ConEmuHk64.dll` in process | **yes** | no |
| `CreateConsoleScreenBuffer` + `SetConsoleActiveScreenBuffer` | ✅ | ✅ (works even in stock conhost) |
| `WriteConsoleOutputW`, full 80×25 | **0.1 ms** | — |
| `ReadConsoleInputW` keyboard | 25 events, VK 13/37-40/65/90/112 | — |
| Unicode round-trip (`─│┌┐└┘ █░▒▓ ▶✓`) | **identical**, zero `?` | — |
| Mouse via ANSI/SGR | ❌ (0 events) | ❌ |
| Resize via ANSI | ❌ | ❌ |
| Code pages | input 852, output 852; `stdout_encoding=utf-8` | same |
| Window | 105 × 28 cells, buffer 105 × 32766 | — |

Consequences: D1 (ConEmu required), D10 (keyboard only), D11 (resize by polling), and set
`color_system="standard"` explicitly in Rich/Textual so it does not emit 24-bit sequences
that get quantised anyway. Do not design a UI that depends on subtle hue differences.

### 6.3 Toolchain (measured, `T2`)

15 build variants (3 compilers × 5 flag sets), **all compiled and all ran** on Windows 7
(`runtime_os=6.1.7601`), with `hard_blockers_present=[]` and `unknown_dlls=[]`. Imports were
only `d3d9.dll, KERNEL32.dll, msvcrt.dll, USER32.dll, WS2_32.dll`.

Two traps the measurement exposed:

| Trap | Measurement | Mitigation |
|---|---|---|
| mingw-w64 v14 defaults to a **newer** Windows | x64 `default`: `_WIN32_WINNT=0x0A00` (Win10), PE subsystem 5.2. x86 `default`: `0x0501` (**WinXP**), PE subsystem **4.0** | Define `0x0601` globally — for **your code and every vendored library** |
| **SSE3/SSSE3 off by default** | `default`: `sse3=0 ssse3=0`. Only with `-march=core2 -msse3`: `sse3=1 ssse3=1` | Same flag set for every library, or their SIMD loops stay at SSE2/C (20-40 % loss) |

### 6.4 D3D9 / presentation (measured, `T4` v3 — partial; v4 built and verified, awaiting a run)

What v3 established before crashing:

- **Every stage passed except `updatesurf`.** In particular `surfaces` (creation) and all five
  `stretch` cases ran **without crashing** (`run_rc=0` on x64 and x86).
- **`updatesurf` crashed with `0xC0000005` on both x64 and x86.** The operation is
  `UpdateSurface(SYSTEMMEM NV12 → DEFAULT NV12)`.
- Because the JSON writer was broken in v3 (it produced 0-byte files), the *content* of the
  passing stages was lost. v4 fixes the writer and splits `updatesurf` into six sub-steps so
  the exact failing call is named.

**Interim conclusion you can already act on:** the "decoder writes to system memory, then
`UpdateSurface` uploads it" path is **dead on this driver**. Plan for the decoder writing
directly into a lockable video-memory NV12 surface, or for a CPU-side path to GDI.

**v4 status:** rewritten so the JSON writer accumulates in RAM and rewrites the whole file
(no `fseek`/`ftell`), the writer is unit-tested on Linux (`tools/test_json_layer.sh`, PASS),
`updatesurf` is split into six sub-steps so the exact failing call gets named, and the
harness keeps per-stage files. It compiles with `-Wall -Wextra` and **zero warnings**
(`tools/verify_c.sh`), and all **73** `IDirect3D*` macro calls match the real `d3d9.h`
argument counts (`tools/check_d3d_calls.py`). It has **not been run on the target yet** —
that run closes gate G0.2.

**Rule that must ship in the client** (D7): `CheckDeviceFormatConversion` before every
`StretchRect`; on refusal, skip; on AV, degrade permanently.

### 6.5 Protocol facts (verified against source, with line numbers)

The full byte-level specification lives in the research notes (§11). The headlines:

- **RTSP is tunnelled over WebSocket.** `rtsps://…` becomes `wss://…`; RTSP messages travel
  as WebSocket text frames. Confirmed by `metaData: wssignaling=1` in the CloudMatch body.
- Video rides a **dedicated UDP socket ("Mjolnir", client port 49005)**, not the ICE bundle.
  Bundle (audio/input/RTCP) is 49006, server 5004.
- Each video RTP packet carries a **16-byte GameStream header** in RTP extension profile
  **`0x4753`**, and it is **little-endian** while the RTP header is big-endian.
- SRTP is implemented directly on AES-CTR/HMAC-SHA1/AES-GCM primitives (no libsrtp), with
  RFC 3711 KDF labels `0x00/0x01/0x02` (SRTP) and `0x03/0x05` (SRTCP).
- FEC is **systematic Cauchy Reed-Solomon over GF(2^8) with primitive polynomial `0x11d`**.
  Generic RS libraries use a Vandermonde matrix and will produce *"mathematically valid but
  corrupt H.264 packets"* (verbatim warning in the reference source). **Write your own.**
- **The codec is never sent to CloudMatch.** It is chosen locally and announced only in
  `ANNOUNCE` (`x-nv-vqos[0].bitStreamFormat`). The backend does not ask whether you have a
  hardware decoder. This is why a software-decode-only client is viable at all.
- CloudMatch uses `Authorization: GFNJWT <id_token>` (**not** `Bearer`) and
  `Content-Type: text/plain` for a JSON body. `login.nvidia.com/userinfo` uses `Bearer`.
- Login is **OAuth 2.0 Device Authorization Grant (RFC 8628)** — no `redirect_uri`, no
  loopback listener, no PKCE.

### 6.6 Auth/catalog facts established by the user's live testing

The user's own implementation reached a working login on the live machine (2026-10-17).
These findings were independently re-verified against the reference source and are correct:

- Device grant against `login.nvidia.com/device/authorize` and `/token`;
  `scope = openid consent email tk_client age`.
- `userinfo` requires `Authorization: Bearer <access_token>`, `Origin: https://nvfile`, and
  a Steam Deck Chrome user-agent. **Observed live: HTTP 200.**
- GFN services require **`GFNJWT`**, with the token being `id_token` and falling back to
  `access_token`. Sending `Bearer` there yields 401 on the first real request.
- **Two different device profiles for the same header names:** LCARS sends
  `nv-device-make: GENERIC` / `nv-device-model: PC`; CloudMatch sends `UNKNOWN` / `UNKNOWN`.
  Sending the LCARS profile to CloudMatch was one of the causes of HTTP 400.
- **`x-nv-client-identity` means two different things:** `"GFN-PC"` (static) for LCARS, the
  Bifrost user-agent for CloudMatch.
- The CloudMatch `POST v2/session` body needed, beyond the obvious: `appLaunchMode: 1` (not
  `null`), `clientPlatformName: "Windows"` (not `"WINDOWS"`), `userAge: 25`, a default
  `dlssOverrides` object with `srScale: 100` and **`fgMultiplier` removed** unless
  `fgMode == 1`, a full-UUID `SubSessionId`, and the query parameters
  **`keyboardLayout` + `languageCode`** (an empty query caused HTTP 400).
- `vpcId` comes from `GET {base}v2/serverInfo` → `requestStatus.serverId`.
- Library query defaults: `locale: "en_US"` (underscore, not hyphen), a specific three-field
  `sortString`, and `filters: {variants:{gfn:{library:{status:{notEquals:"NOT_OWNED"}}}}}`.

You will re-derive all of this from the reference source yourself — the pointers are in §5.2.
The value of the list above is that it tells you **which details are load-bearing**, i.e.
where a naive implementation gets a 400 and the reason is not obvious.

---

## 7. Logging specification (D13) — the most important section for you

**Why this matters more here than in a normal project:** you cannot test login, the queue, or
streaming in a sandbox. There is no account, no network path to NVIDIA, and no way to iterate.
You will get a small number of live attempts, each costing the user real time. **The log is
the only instrument.** Design it so that one failed live run tells you, without a second run:

- which stage failed and which state machine was in which state,
- what went over the wire (redacted),
- what the machine was doing (CPU, RSS, timers, dropped frames),
- and what the client already tried in response.

Everything below is a requirement, not a suggestion.

### 7.1 Two outputs, one event stream

Emit **one** structured event per thing that happens, and render it two ways:

1. **Human log** (`logs/<channel>.log`): one line per event.
   `2026-10-21T14:03:22.481Z INFO  [rtsp] c=a1b2c3 stage=SETUP status=200 transport=udp;unicast;client_port=49005`
2. **Structured log** (`logs/<channel>.jsonl`): the same event as one JSON object per line,
   machine-parsable, so a diagnostic bundle can be analysed without reading prose.

Both come from the same `log_event(level, channel, message, **fields)` call. Never `printf`
directly into a log file.

### 7.2 Fields present on every event

```
ts            RFC 3339 UTC, millisecond resolution
mono_ms       monotonic clock since process start (for latency arithmetic)
level         ERROR | WARN | INFO | DEBUG | TRACE
channel       see §7.3
correlation   id tying together everything belonging to one logical operation
session_id    when a session exists (never in the auth channel)
pid, thread   integers
build         git rev + build config, once per process start
```

`correlation` is the single most useful field you will add. Generate one per user-visible
operation (`login`, `catalog.fetch`, `session.create`, `session.stream`) and propagate it
into every nested call and every log line. When something fails, `grep` on one correlation
id yields the entire story in order.

### 7.3 Channels and rotation

Per the project constraints: **2 MiB per file, 3 generations per channel.**

| Channel | Contents |
|---|---|
| `app` | lifecycle, state transitions, user actions, config resolution |
| `http` | every request/response for OAuth, GraphQL, CloudMatch |
| `rtsp` | the WebSocket + RTSP transcript and both SDP bodies |
| `transport` | SRTP, ICE, DTLS, SCTP, reorder, NACK, FEC |
| `media` | decode, presentation, audio playout |
| `input` | Raw Input capture and NVB control frames |
| `perf` | periodic counters (see §7.6) |
| `diag` | capability probes, self-tests, diagnostic bundles |

Rotation: on reaching 2 MiB, `channel.log` → `channel.1.log` → `channel.2.log` → dropped.
Same for `.jsonl`. Total worst case: 8 channels × 2 MiB × 3 × 2 formats = **96 MiB**, which
is too much for a 2 GB machine — so make `TRACE` and `DEBUG` **off by default** and only the
`perf`/`transport` channels keep `.jsonl` at `DEBUG` and above. Document the actual budget.

### 7.4 Levels

| Level | Default | Use for |
|---|---|---|
| `ERROR` | on | an operation failed; always with `code`, `reason`, and `next_action` |
| `WARN` | on | degraded but continuing (fallback taken, retry scheduled, capability missing) |
| `INFO` | on | state transitions, one line per request/response, session milestones |
| `DEBUG` | off | headers (redacted), SDP, per-frame timings, channel state |
| `TRACE` | off | per-packet counters, hexdumps (bounded), keystream sanity |

`--log-level`, `--log-channels`, and per-channel overrides from a config file. Level changes
must apply **without restart** where feasible (a session in progress is exactly when you want
to raise verbosity, and you will not get a second chance).

### 7.5 What to log at each stage — and what it proves

This is the checklist that makes a single live run diagnostic. For each item, the "proves"
column is why you need it.

**OAuth / device grant**

| Log at | Fields | Proves |
|---|---|---|
| `device_authorize` request | url, header **names** only, form field names, `device_id` length + first 8 chars | that identity headers were sent at all |
| response | http status, presence of each of `device_code`, `user_code`, `verification_uri`, `verification_uri_complete`, `expires_in`, `interval` (values for the non-secret ones, **lengths** for secrets) | whether the provider accepted our client identity |
| browser open | method tried (`webbrowser` / `startfile` / `rundll32`), success, error | whether the user ever saw the code |
| each poll | attempt #, interval in force, elapsed, `remaining_s`, provider `error` string, http status | distinguishes "user has not approved yet" from "we poll wrong" |
| `slow_down` | old interval → new interval | that we back off correctly |
| network error during poll | exception class (not message, which may carry data), attempt kept alive | that a tethering blip does not kill login |
| token response | presence of `access_token`, `refresh_token`, `id_token`, `client_token`; `expires_in`; **JWT claims of `id_token` with values redacted except `exp`/`iat`/`iss`/`aud`** | which tokens we actually got; whether `id_token` is usable for `GFNJWT` |

**Never log:** any token value, `srtpAesKeyHex`, `srtpSaltHex`, ICE `credential`,
`icePassword*`, `device_code`, `user_code` (log its length and show it in the UI instead).

**HTTP layer (every request, one event pair)**

```
→ method url query_keys body_keys body_bytes attempt correlation
← status elapsed_ms response_bytes error_code? retry_scheduled?
```

Plus, at `DEBUG`: request header **names** and whitelisted values
(`nv-client-type`, `nv-client-id`, `Content-Type`, `User-Agent`), and the response's
`requestStatus` object in full (it carries `statusCode` / `statusDescription` /
`unifiedErrorCode` — the actual reason for a rejection).

**Catalog**

| Log | Proves |
|---|---|
| `vpcId` discovered: length + first 8 chars, source (`serverInfo.requestStatus.serverId`) | that discovery worked and returned something plausible |
| GraphQL request: operation name, **sorted variable names**, which are present, `fetchCount`, `cursor` length | that no mandatory `!` variable was omitted (a missing one is a query error that looks like an empty library) |
| response: `numberReturned`, `numberSupported`, `hasNextPage`, `endCursor` length, `totalCount` | pagination health |
| catalog context hash + `catalogRevision` per page | detects the "cursor from a different route" failure before it corrupts the list |
| per-item parse failures: item id + which field | a schema change shows up as N parse failures, not as a silently shorter list |

**CloudMatch session**

| Log | Proves |
|---|---|
| create request: `appId`, resolution, fps, header names, **body key list** (not values) | that the body has the right shape |
| create response: http status, `requestStatus.statusCode`, `statusDescription`, `unifiedErrorCode`, `session.status`, `queuePosition`, `seatSetupStep` | the actual reason for a 4xx/5xx or a rejection with HTTP 200 |
| every status poll: `status`, `phase`, `queuePosition`, `seatSetupStep`, elapsed | the queue timeline; lets you see whether it is stuck at a step |
| extracted context: which of the 31 fields are present, `usage` values seen in `connectionInfo`, `mediaConnectionInfo` present?, `signalingUrl` scheme, `iceServers` count | **this is the #1 thing that will be wrong first** — see §9 R22 |
| resume / cleanup decisions | orphaned sessions |

**RTSP over WebSocket**

| Log | Proves |
|---|---|
| DNS resolution result (address count, family) and connect elapsed | whether the seat is reachable at all |
| TLS: negotiated version and cipher, certificate chain **subject/issuer names only**, verification result | TLS problems on an EOL OS without disabling verification |
| WebSocket upgrade: request header names, response status, `Sec-WebSocket-Accept` present | that the signalling transport works |
| every RTSP request: method, URI, header names, body bytes | ordering |
| every RTSP response: status, header names + `Session`, `x-nv-ping`, body bytes | the `PLAY` gate (200 vs 455) |
| **both SDP bodies verbatim** (they contain no secrets — the fingerprint is public by design) | the single most valuable artefact when negotiation fails |
| extracted SDP attributes: `nativeRtcOnBundlePort`, `rtcpOnSctp`, `enableUnifiedSocket`, `pingVersion`, ICE ufrag **length**, fingerprint **length**, `a=control` for video | that we required `nativeRtcOnBundlePort == 1` and got it |
| ANNOUNCE attribute diff vs the DESCRIBE baseline | which attributes we suppressed and why |

**Transport (SRTP / ICE / DTLS / SCTP / FEC / NACK)**

| Log | Proves |
|---|---|
| SRTP: profile **name**, master key/salt **lengths only**, derived session key length, tag length | that profile negotiation matched |
| first authenticated packet: seq, ssrc, timestamp, payload bytes | that decryption works — the moment the stream becomes real |
| unprotect failures by reason: `MalformedRtp`, `ReplayRejected`, `AuthFailed`, with counters | distinguishes a wrong key (all `AuthFailed`) from clock/seq issues |
| replay window: highest index, ROC, window fill | ROC rollover bugs |
| reorder: window size, gaps seen, packets waited, dequeue timeouts | network quality |
| NACK-v2: records sent, packets requested, retries, RTT used and its source, repairs received | whether loss recovery works at all |
| FEC: blocks seen, `data_shards`, `parity_shards`, `repair_percent`, repaired shards, failed repairs | and, crucially, whether repaired shards produce **valid H.264** |
| ICE: candidates gathered (type/count), binding requests sent/received, selected pair + RTT | connectivity |
| DTLS: handshake state transitions, negotiated cipher, **peer fingerprint hash** (a hash, not the value), keying-material export success | the most likely place for a silent stall |
| SCTP: association state, channels opened by label, ordered/reliable params per channel, bytes in/out | whether the input channel ever opens |

**Media**

| Log | Proves |
|---|---|
| decoder init: codec, profile/level from SPS, chosen `threads`, pixel format, buffer sizes | that we asked for 1 thread (D4) |
| first frame: width, height, format, ms since PLAY | time-to-first-frame |
| format change events (`MF_E_TRANSFORM_STREAM_CHANGE` equivalent) | mid-session resolution change |
| per-second: frames in, frames decoded, frames dropped, frames presented, decode ms (mean/p95), queue depth, keyframes, IDR requests sent | the health of the whole path |
| presentation: chosen fallback rung and **why** (`CheckDeviceFormatConversion` result, AV caught), `StretchRect` HRESULTs, `Present` ms (mean/p95/max), measured refresh interval | D7/D9 evidence |
| audio: Opus init, sample rate, channels, jitter target (current), concealment ms, playout buffer ms, device name, underruns | A/V sync problems |

**Input**

| Log | Proves |
|---|---|
| Raw Input registration: flags, success, device handle count | capture works |
| the 7-frame activation sequence: each command code and payload **length** (payloads are not secret, but log lengths at INFO and full hex at DEBUG) | that we sent window state **19** and in the right order |
| channel state per label: open/closed, bytes, messages | whether input ever reaches the game |
| frames sent per second by type, plus round-trip for the frames that get acknowledged | responsiveness |
| cursor mode changes received from the server (IDs 0 = hidden+locked, others = warp) | whether the server accepted us as an active session |

### 7.6 Periodic performance snapshot

Every 1000 ms, one `perf` event:

```
cpu_process_pct, cpu_system_pct, rss_kb, peak_rss_kb, handle_count, thread_count,
timer_resolution_us, frames_decoded, frames_presented, frames_dropped,
decode_ms_p95, present_ms_p95, queue_video_depth, queue_audio_ms,
net_rx_bytes, net_rx_packets, net_rx_dropped, rtt_ms, loss_pct,
fec_repaired, nack_sent, nack_repaired, idr_requested,
refresh_hz_measured, dpst_change_detected
```

This one line answers "is it the CPU, the network, the GPU, or the clock?" without a profiler.

### 7.7 Failure amplification — the mechanism that replaces a debugger

When an operation fails, the client must **immediately and automatically** do more than log
the error:

1. **Raise verbosity** for the affected channels to `DEBUG` (and `TRACE` for `transport` if
   the failure is protocol-level), for the remainder of the session.
2. **Flush and seal** all log files (`fflush` + `fsync` equivalent) — a crash must not lose
   the tail. This is exactly how the research agent lost three rounds of D3D9 probe data.
3. **Write a diagnostic bundle** (§7.8).
4. **Run the relevant self-test** from §7.9 and include its result.
5. **Emit a one-screen human summary** to the TUI: what failed, the error `code`, what was
   already tried, and the exact path of the bundle to send.

Implement this as a single `on_operation_failed(op, code, reason)` hook that every subsystem
calls, so amplification cannot be forgotten in one module.

### 7.8 Diagnostic bundle

`logs/diag-<UTC>-<shortid>.zip`, produced automatically on any `ERROR` and on demand
(`--diagnostics`). Contents:

| File | Content |
|---|---|
| `summary.json` | one machine-readable verdict per subsystem: `ok` / `degraded` / `failed`, with the reason and the evidence pointer. **This is what gets read first.** |
| `capabilities.json` | the platform probe: adapters (PCI ID, driver version, `Description`), D3D9 caps, `CheckDeviceFormat`/`FormatConversion` matrix, display modes and refresh rates, `EnumProcessModules`-equivalent for the console host, Python/OpenSSL versions, code pages, free RAM |
| `logs/*.log`, `logs/*.jsonl` | all channels, all generations |
| `wire/*.txt` | redacted transcripts: HTTP, WebSocket/RTSP (SDP verbatim), and the **bounded raw ring buffers** (§7.10) hexdumped |
| `state.json` | every state machine's current state and its transition history (last 64 transitions each) |
| `perf.jsonl` | the periodic snapshots |
| `selftest.json` | results of the self-tests that ran |
| `build.json` | git rev, dirty flag, compiler version and **exact flags**, `_WIN32_WINNT`/`WINVER`/`NTDDI_VERSION` actually in effect, library versions |

`build.json` matters more than it looks: the toolchain traps in §6.3 are invisible unless you
record the effective flags.

Size cap the bundle (e.g. 8 MiB) and truncate the largest artefacts first, newest-kept.

### 7.9 Self-tests that run without an account

So that "it does not work" can be narrowed down **before** blaming the protocol:

| Self-test | Trigger | What it separates |
|---|---|---|
| `net.dns`, `net.tcp443`, `net.tls` against the real hostnames | any network failure | connectivity vs TLS vs HTTP semantics |
| `tls.chain` — dump the chain the OS presents for `login.nvidia.com` | TLS failure | stale Windows 7 certificate store (diagnose, never bypass) |
| `console.host` — ConEmu detection + ANSI write/read-back probe | TUI startup | D1 verification, and it explains garbage output instantly |
| `d3d.formats` — the full `CheckDeviceFormat` / `FormatConversion` matrix | presentation failure | which rung of the D7 chain is available |
| `d3d.present` — timed `StretchRect`+`Present` on the chosen rung | frame pacing problems | GPU vs CPU vs clock |
| `decode.synthetic` — decode a bundled `.264` and report fps/CPU | media failure | decoder vs transport |
| `srtp.vectors` — run the KDF and unprotect against **known-answer test vectors** | `AuthFailed` | wrong key material vs wrong IV vs wrong ROC |
| `fec.vectors` — encode/decode a shard set with known erasures | corrupt video after loss | your Cauchy matrix vs a Vandermonde mistake |
| `gs.parse` — parse captured packets and print the 16-byte GS fields | garbled video | endianness/offset mistakes in the GS header |
| `clock.resolution` — measure `timeBeginPeriod` effect | jitter/pacing | timer granularity |

Every self-test must be runnable standalone (`knvstd --selftest=fec.vectors`) and must print
a machine-readable result. Build the known-answer vectors **before** the live attempt; they
are the only way to prove crypto correctness offline.

### 7.10 Bounded raw ring buffers

For Mjolnir and the bundle socket, keep the last **64 KiB** of raw datagrams (pre-decryption)
in a ring buffer per socket, plus the last 64 KiB of WebSocket frames. They are dumped into
the bundle **only** on failure. This is what turns "the video is garbage" from a guessing
game into a parseable artefact. Bound them hard: at 2 GB RAM, 128 KiB total is a rounding
error, but an unbounded capture is not.

### 7.11 Redaction

Implement as a **whitelist**, not a blacklist. A `redact_headers()` helper keeps only
known-safe header names and masks everything else; a `redact_json()` helper walks a payload
and replaces any key matching a secret pattern with `"<redacted:N>"` (N = byte length, so
you can still tell present from absent). Secret patterns include: `token`, `secret`,
`password`, `credential`, `srtp*Key*`, `srtp*Salt*`, `icePassword`, `fingerprint` (hash it
instead), `device_code`, `user_code`, `Authorization` (log the scheme only).

Apply redaction at the **logging boundary**, not at each call site — one choke point, no
forgotten call sites. Add a unit test that asserts a deliberately secret-laden object comes
out clean.

### 7.12 Rules that come from having lost data already

- **Flush after every `ERROR`** and on every state transition. Buffered logs are worthless
  in a crash — this exact mistake cost three rounds of D3D9 probe results.
- **Never build a diagnostic file with `fseek` over your own output.** Either accumulate in
  RAM and rewrite the whole file, or append-only with explicit flush. (The v3 probe used
  `fseek`/`ftell` mixes and produced 0-byte files.)
- **Isolate anything that can crash** — a separate process, or at minimum structured
  exception handling with a marker written *before* the risky call.
- Log **lengths and presence** for secrets, never values — but do log presence, because
  "field missing" and "field empty" are different failures.
- Every `ERROR` carries `code` (stable, machine-readable), `reason` (human, English), and
  `next_action` (what the client will do, or what the user should do).

---

## 8. Known pitfalls

Twenty of them are catalogued with symptoms in the research notes (§11, `analiza/04`). The
five that will cost you the most time if you miss them:

1. **The GS extension body is little-endian** while the RTP header is big-endian. Symptom:
   every frame index is nonsense.
2. **`fec_current_block` / `fec_last_block` live in `ext[11]`** (the top byte of the flags
   word), not in a separate field. Symptom: frame boundaries never trigger.
3. **Cauchy, not Vandermonde** for the FEC matrix. Symptom: repairs succeed mathematically
   and produce corrupt H.264 — the decoder just keeps asking for IDR frames.
4. **Reconstructed shards contain transport metadata** that legitimately differs from the
   original data shard. Symptom: every real cloud repair is rejected even though the video
   payload is fine.
5. **`window state 19`** in control command `0x0320`. Symptom: the session looks inactive and
   the server never sends system-cursor mode updates — which presents as a "cursor bug".

And two process-level ones:

6. **Do not write Win32/D3D9 code from memory.** The research agent did it three times and
   produced three non-compiling files (wrong `D3DCAPS9` field names, wrong macro arity,
   a type `D3DUSAGE` that does not exist in mingw-w64, a deleted function still called from
   seven places). Verify signatures against the real `d3d9.h`, and **compile before handing
   anything over** (§10).
7. **Do not trust a zero exit code.** `-hwaccel dxva2` returned `rc=0` while being 4× slower
   than software. Always measure the thing you care about, not the thing that is easy to check.

---

## 9. Risk register (current)

| # | Risk | P | S | Status / mitigation |
|---|---|---|---|---|
| R1 | CPU cannot keep up | — | — | ✅ **closed** by measurement (§6.1) |
| R2 | D3D9 NV12 path rejected or crashes the driver | high | critical | ⏳ partially measured: `UpdateSurface` on NV12 **crashes** (`0xC0000005`, both arches); `StretchRect` stages did **not** crash but their results were lost to a writer bug. T4 v4 re-measures. Mitigation D7. |
| R3 | usrsctp/DTLS will not build or run on mingw + Win7 | medium | high | ⏳ **Do a standalone spike first**: DTLS handshake + one SCTP channel + echo, before any NVST code. Plan B: libdatachannel (MPL-2.0, C++17 with a C API). |
| R4 | no VT in conhost | — | — | ✅ **closed** (§6.2, D1) |
| R5 | Python 3.8 is EOL | certain | medium | Store tokens via **DPAPI** (`CryptProtectData` through `ctypes`), never plaintext |
| R6 | GCC 16 binaries will not start on Win7 | — | — | ✅ **closed** (§6.3) |
| R7 | CloudMatch rejects a client without a hardware decoder | low | critical | The codec is never sent to CloudMatch (§6.5). Gate: the first real `POST v2/session` |
| R8 | a seat requires `nativeRtcOnBundlePort != 1` | low | medium | Fail with the reference implementation's error name instead of hanging |
| R9 | Intel **DPST** drops the refresh rate mid-session | high | medium | D9. Also ask the user to disable DPST for DC in the Intel panel. T4 v4 lists available 1280×800 modes |
| R10 | NVB type 10 exact bytes unknown | certain | low | Nobody has them, including the reference implementation (it reconstructs `0x0323`). Mark `UNVERIFIED` in code |
| R11 | OOM at 2 GB | medium | high | Socket buffer **1-2 MiB**, not 8 MiB. No `ffmpeg.exe` subprocess — link `libavcodec` in-process |
| R12 | GPL covers the whole client | certain | low | Accepted (D3), private project |
| R13 | tethering jitter/loss | high | medium | RS-FEC + NACK-v2 from the start; `packetSize=1280`, not 1500 |
| R14 | vendored libraries built without `-march=core2 -msse3` | medium | high | One canonical flag set for **everything**, verified via `-dM` in the build |
| R15 | Python deps: no rich/textual installed, cp38, EOL | medium | medium | Pinned `requirements.txt` + a local wheel directory + an offline install test |
| R16 | console raster font lacks Unicode glyphs | high | low | `--ascii-boxes` switch, identical layout |
| R17 | CPU scaling costs ~18 points | high | high | D6 |
| R20 | TUI resize does not arrive as an event | certain | low | D11 (polling) |
| R21 | C code does not compile because signatures were guessed | low now | medium | ✅ mitigated: the research sandbox has `apt` → `gcc-mingw-w64-x86-64`; **compile before handing over** (§10) |
| **R22** | **Session context passed to the C client is too thin** | certain | **critical for stage 2** | The reference `session_info()` produces **31 fields**; a minimal parse yields ~8. Missing: `mediaConnectionInfo` (the bundle peer — no audio, no input without it), `signalingUrl` (the `rtsps://`→`wss://` transform), `iceServers` (with three STUN fallbacks), `rtspsEndpoints[]` (plural, for fallback), `connectionInfo`, `streamingBaseUrl`, `finalizedStreamingFeatures` (**the server overrides what you requested**), `seatSetupStep`, `subSessionId`, `termination`. **Copy the 31-field contract 1:1** as the IPC payload, with redaction in logs. |
| **R23** | no session resume, no orphan cleanup | certain | medium | `{"action":2,"data":"RESUME",…}` and a pending-cleanup record. On tethering, every drop becomes a new queue otherwise. Stage 5, mandatory. |
| **R24** | catalog pagination not bound to the catalog revision | medium | medium | The reference hashes `[user_id, provider, generation, graphql, vpcId, cache_scope, revision, locale]` and fails with `catalog_changed` on mismatch; it also re-checks the revision **before and after** each page fetch. Without this you get silent duplicates or holes after a region/account change. |

Also worth knowing: the reference caches the supported-languages list for **14 days** with
`allow_stale: true`. Do not refetch it on every start — on tethering that is seconds and data.

---

## 10. How to work (process rules that came from experience)

1. **Measure before you build.** Each stage has a gate (§12). A gate that fails is
   information, not a setback.
2. **Compile before you hand anything over.** The research sandbox has `apt`; install
   `gcc-mingw-w64-x86-64` and compile with `-Wall -Wextra`. Three rounds were wasted on
   files that did not build. Note that installed packages do **not** persist between
   sessions — reinstall each time.
3. **Test the boring parts on the host.** The JSON writer in the D3D9 probe was broken twice
   and both times it was caught only by running it on Linux without any D3D9 involved.
   Anything that is not hardware-specific can be unit-tested anywhere.
4. **Keep artefacts small.** A 150 kB JSON attachment got truncated and lost a whole
   measurement round. Always emit a small `*_summary.txt` next to the full report.
5. **Isolate anything that can crash** — separate process per risky operation. This is what
   turned "the probe crashed" into "`UpdateSurface` on NV12 crashes".
6. **Name your unknowns.** Keep a `NOT VERIFIED` list the way the reference implementation
   marks `UNVERIFIED`. Never let a plausible guess into the code without a label.
7. **Ask for a measurement instead of guessing.** The user runs tests willingly and quickly.
   A 2-minute probe beats an hour of speculation.

---

## 11. Where the research notes live

The research agent's working files (in the workspace, carried across sessions):

| File | Contents |
|---|---|
| `docs/archive/HANDOFF.md` | the running handoff: decisions, risks, gates, session log |
| `docs/archive/analiza/00-WERDYKT.md` | feasibility, OpenNOW scale, protocol overview, first CPU estimate (**superseded** by measurement) |
| `docs/archive/analiza/01-WYNIKI-TESTOW.md` | toolchain + console measurement round |
| `docs/archive/analiza/02-DECYZJE.md` | decision register D1-D3 and facts F1-F6 |
| `docs/archive/analiza/03-WYNIKI-RUNDA3.md` | CPU measurement, console-host proof, the D3D9 crash |
| `docs/archive/analiza/04-NVST-SPEC-BINARIA.md` | ★ **byte-level NVST specification**: RTP + GS `0x4753` layout, SRTP KDF/IV/tag, FEC Cauchy over GF(2^8), NACK-v2, NVB frames, **20 pitfalls with symptoms**, source-line index |
| `docs/archive/analiza/05-PRZEGLAD-KODU-LOGOWANIA.md` | ★ review of the user's auth/catalog/session code: 32 verified claims, the two gaps that became R22/R23, and four catalog-pagination facts that became R24 |
| `tests/` | the measurement harnesses (`T1`…`T7`) — reusable, and good templates for your own probes |
| (external) | the OpenNOW reference tree at commit `a49b124` is cloned BESIDE the repo (agent workspace: `ref/`), not vendored |
| `docs/reference/hdr_d3d9*.h`, `docs/reference/hdr_dxva2api.h` | real mingw-w64 headers, for signature verification |
| `tools/verify_c.sh`, `tools/check_d3d_calls.py`, `tools/test_json_layer.sh` | compile check, D3D9 macro-arity check, JSON-writer unit test |

`docs/archive/analiza/04` and `05` are the two documents that will save you the most time.
Note that they are written in Polish; the *facts*, constants, and line references are
language-independent. Everything **you** write is in English (D12).

---

## 12. Stage plan and gates

| Stage | Scope | Gate |
|---|---|---|
| **0** | Measurements | G0.1 CPU ✅ · G0.2 D3D9 presentation ⏳ **(T4 v4)** · G0.3 toolchain ✅ · G0.4 console ✅ |
| **1** | C skeleton offline: `knvstd --loopback` reads a `.264`, decodes (FFmpeg, `threads=1`), presents in a 1280×800 D3D9 window with letterbox, reports fps/latency/CPU/RSS. Plus WASAPI with synthetic PCM. **Plus the entire logging subsystem (§7) and the diagnostic bundle.** | **G1:** steady 30 fps for 10 minutes, CPU within the measured budget, RSS flat, and a diagnostic bundle that is complete and self-explanatory with **no network involved** |
| **2** | Protocol without DTLS/SCTP: WSS + RTSP, Mjolnir, SRTP, GS header, access-unit assembly. `--capture` dumps AUs; `--decode` decodes them live. | **G2:** `OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY` returns 200; assembled access units verify with `ffprobe`; reorder and NACK-v2 survive injected loss |
| **3** | Bundle: ICE + DTLS + SCTP + Opus + input. **Start with the R3 spike** (DTLS handshake + one SCTP channel + echo) outside any NVST code. | **G3:** audible audio; the 7-frame activation sequence accepted; the cursor moves in the cloud |
| **4** | Python TUI: Textual/Rich, ConEmu host detection, account screen, game list (2000 entries in memory), queue screen (`queuePosition` + `seatSetupStep`), session launcher, DPAPI vault, logging. | **G4:** login → catalog → launch → queue → *streaming* → the game responds to the keyboard → `TEARDOWN` → back to the list, **three times in a row without a restart** |
| **5** | Hardening: RS-FEC, recovery (IDR `0x0301`, RFI `0x0302`, invalidation), reconnect + **RESUME** (R23), orphan cleanup, `dynamicStreamingMode`, bitrate cap 3-6 Mbps, real statistics in the TUI (the reference implementation admits its own LATENCY/PING/LOSS% are placeholders — do not repeat that) | — |

**Do not start stage 1 until G0.2 is closed**, because the presentation path determines the
shape of the video loop.

> **Status marker (round L, 2026-10-29 — the definitions above are
> unchanged; this note only records progress):** **STAGE 2 CLOSED
> (XLVII); STAGE 3 UNDERWAY — audio extraction is LIVE-PROVEN (XLIX)
> AND THE INPUT GAP IS NOW CLOSED IN CODE: THE CLIENT CAN TYPE.**
> Run 31 (Watch Dogs: Legion, the interleaved jiggle A/B,
> `--lottery=4 --lottery-gap=60 --session-jiggle=100
> --lottery-stop-on-keeper=0 --session-audio-out=audio.opus`) came
> back with **four draws, four `recovery_exhausted`** (media 16.19 /
> 13.96 / 35.96 / 122.23 s; 487 / 420 / 1080 / 3668 AUs; 18 138
> datagrams, all authenticated, 0 gaps, 0 decode errors) — so
> **F14/F15 are proven live** (four `lottery_arm` lines alternating
> 100/0/100/0, all four draws flown) and **F12 is proven live** (one
> `audio_close` per draw, EOS pages present, no leaked FILE*, no
> scrambled page) — but the **≈600 s A/B has NO RESULT**: nothing
> survived past 123 s of media, so the idle-policy hypothesis is
> still open. **Two of our own fixes were caught by their own
> evidence.** (a) F13's per-draw naming was broken: the delivered
> files are `audio_d1_d1us` … `audio_d4_d4us` — suffix doubled AND
> no `.opus` extension — because the derivation `memcpy`'d
> OVERLAPPING memory (`strrchr` into the destination; ASan
> `memcpy-param-overlap` on the rig). Sizes match the `audio_close`
> counters exactly, so the audio was intact and only the name was
> wrong. (b) The `.264` complaint ("only the first file works") is
> answered with the shipping binary: the single-AU dumps are NOT
> corrupt — `au_00001_k.264` carries SPS/PPS and decodes, every
> P-frame file cannot be started on (`Invalid data found when
> processing input`) — and run 31 also flew four draws into ONE
> `--session-au-dir`, so draw 2's `au_00001_k.264` replaced draw 1's.
> **THE FINDING THAT REDIRECTS THE PROJECT — THE LOGIN WALL:** all
> **64** `sctl_010a` process notifications were decoded and their
> **46** `processName` entries are Ubisoft Connect ONLY
> (`UplayWebCore.exe` x26, `UplayService.exe` x16, `upc.exe` x4) with
> **ZERO game binaries on any draw**. The operator's retraction of
> the XLIX "Legion launches with our client" claim is therefore
> confirmed from our own capture, and the mechanism is now known:
> play.geforcenow.com's web client **types the credentials INTO THE
> STREAM** (auto-login), so the browser reaches the game in seconds
> while our client — which sent no keystroke — is handed the
> launcher's login dialog (`au_00001_k.264` IS that dialog). The
> audio is silent for the same reason: nothing that makes sound ever
> ran. **G3's keyboard scope is the critical path, not a follow-on.**
> Also measured: the string `TEARDOWN` appears **ZERO times in any
> run-31 log** (XLIX's teardown was fire-and-forget with no event
> line), and draws 2, 3 and 4 each walked the CloudMatch 403
> SESSION_LIMIT ladder (`cm_create_backoff` 20 s / 40 s / 80 s) —
> **140 s of dead queue time on three of four draws** because the
> previous draw never released its seat. Round L therefore ships four
> fixes: **F16** `kn_drawpath.{c,h}` — the per-draw naming rule as ONE
> pure, alias-free, in-place-safe module (`test_kndrawpath` **1051
> checks**, the 21st suite; used by the audio path, the video path and
> the AU names); **F17** `--session-video-out=F.264` /
> `--replay-video-out=F.264` — ONE **continuous** per-draw Annex-B
> stream (`video_d<N>.264`) through the shared writer
> `knv_dump_au()`, plus draw-numbered AU names
> `au_d<N>_%05u_%c.264`; **F18** the TEARDOWN is now ALWAYS logged
> with its real rc/status (`rtsp_teardown`) and `--session-release`
> (default **OFF**) adds the CloudMatch `DELETE v2/session/{id}` on a
> fresh TLS stream (`cm_release`, 2xx|404 = gone); **F19** REAL INPUT
> — `--input-script=FILE`, `--input-live`, `--input-anti-afk[=MS]`
> (the reference's VK 0x7C pulse), `--input-text-gap=MS` and
> `--input-check=FILE` (offline parse, rc 2 on error), with five
> discrete builders in `kn_input.c` byte-exact against the Rust
> goldens (`test_kninput` 548 -> **794**), gated on the activation
> chain's `enable-input(true)` and routed sid 0 (keys/buttons/wheel) /
> sid 6 (mouse motion).
> **Run 32 FLEW (2026-10-29, Endfield 107057914, the operator's own
> 80-event macro) and PROVED F16/F17/F18 ON TARGET**: T10 on Win7 =
> **ALL PASS 84** exactly as predicted (the +2 = `test_kndrawpath`,
> which only ran there because round L added it to T10's own suites
> list), stamp L, `video.264` 3 679 210 B with 668/668 frames decodable
> and WATCHED by the operator (Endfield launcher + animations from
> t≈0), `audio.opus` correctly named and byte-equal to `audio_close`,
> and — first in project history — `rtsp_teardown` **status 200** +
> `cm_release` **200 gone:1**: the seat came back, zero 403 ladders.
> The typing itself did not happen: `input_summary fired:0 gated:0
> sent:0` against `aus:668` means the draw (29.0 s of media, the first
> LOSSY freeze: 17 gaps / 191 withheld / FEC 31 / NACK 51, BWE
> 24–42 Mbps) died before the macro's first timestamp, and the login
> UI was on screen from frame 1. Audio silent again — a launcher
> makes no sound. Full read: `docs/RTSP.md` §39.
> **Target-pending: run 33 = THE LOGIN RUN, TIMELINE MOVED LEFT**:
> first event at t≈6–12 s (Endfield `Log In` ≈ (984, 44) px in the
> 1280×720 viewport, `Play` ≈ (1104, 632)), retries at ~30/60/90 s,
> `--input-anti-afk=60000` riding along, same app, same
> `--session-release`. Questions it answers: does a typed login get
> past the launcher, does a GAME process appear in `0x010a`, and is
> the capture finally non-silent. G3's
> "audible audio" bar stays met as EXTRACTION; decode-to-speaker
> remains the deferred follow-on. Remaining stage-3 scope after run
> 32: G3 mouse/wheel live proof (built, sandbox-proven, never flown),
> G4 cursor payloads, G1 handshake timeout, D-t13 — see
> `START-HERE.md` §3/§4/§8, `docs/RTSP.md` §38, `docs/INPUT.md` §10,
> `docs/BUNDLE-AUDIO.md` §6/§10, `docs/INTEGRATION.md` rev 41.
>
> **Sandbox gate canon at L close** (every gate re-run after the last
> edit): native battery **PASS**, zero warnings, **21 suites** —
> baselines **test_kndrawpath 1051 (NEW)**, **test_kninput 794** (was
> 548), test_knogg 163, test_knarx 62, test_knselftest 118, test_knrx
> 218, test_knbw 387, test_kncm 148, test_knrtsp 150 · cross
> `-Werror` x2 arches **PASS, 0 SKIPs** (all four vendor trees staged:
> tiny-aes-c `23856752f`, crypto-algorithms-sha1 `cfbde484`, mbedtls
> v3.6.0 `2ca6c285` + framework `750634d3`, usrsctp `c625e219` — the
> correct URL is `github.com/sctplab/usrsctp`) · ctest **25/25** x2
> backends (`build/net-mb`, `build/net-sch`; was 22/22 — the three new
> entries are `test_kndrawpath`, `knvstd_input_check` and
> `knvstd_input_check_rejects` (WILL_FAIL). cmake DOES build `knvstd`
> in both trees — round **L** stamp verified — but **without FFmpeg**,
> because the owner's `build/ffmpeg/` dev tree is gitignored:
> `kn_decode` compiles as the honest stub and `decode.synthetic`
> reports `skipped`. That is the designed no-FFmpeg mode
> (`CMakeLists.txt` :146-193), not a failure) · T10 POSIX dry **ALL
> PASS 74** (was 72) and the **TARGET expectation moves 82 -> 84**:
> round L caught that cmake *builds* every suite while T10 *runs* only
> the ones its own `suites` list names, so `test_kndrawpath` was added
> there AND to `required_bins` — without that edit the F16 suite would
> have been built, shipped and never executed on the target, silently ·
> T13 **ALL PASS 63** against the mbedTLS cmake binary (`--knvstd
> build/net-mb/knvstd`) and 63 against the cmake `build/knvstd` (cmake
> auto-detects the staged mbedTLS) — **ALL PASS 51** with ONE by-design
> SKIP (the gcm256 leg) against the raw-recipe `build/knvstd-bare`,
> which is cm128-only · e2e **48/48** (was 43; the F18 release leg
> asserts `deletes: 1` in the fake rig's `done` line over a real
> loopback socket) · summary skeleton canon **113 recursive keys**,
> re-validated against run 31's REAL stdout (113 == 113, "only in
> skeleton/live: (none)" — F16–F19 add and remove NO key, deliberately:
> the per-draw evidence lives on the `input_summary`,
> `video_stream_end`, `rtsp_teardown` and `cm_release` event lines) ·
> selftest registry **16** (audio.vectors 188 checks, gated) ·
> ASan/UBSan clean on `test_kndrawpath`, `test_kninput` and a fully
> sanitized `knvstd` (`--input-check` x2 + the F17 replay with both
> dump legs); the only runtime note in the tree remains the vendored
> `crypto-algorithms-sha1/sha1.c:26` · **F17 host proof**
> (`logs/f17_video_proof_L.txt`): the repo's 60-frame clip replayed
> through the full rx pipeline -> `video.264` 42 944 B, ffprobe/ffmpeg
> **60 frames OK**, while `au_00002_p.264` / `au_00030_p.264` report
> `Invalid data` and `au_00001_k.264` decodes (1 frame) · knvstd stamp
> **L** in all three sandbox binaries (`build/knvstd`,
> `build/net-mb/knvstd`, `build/net-sch/knvstd`).

---

## 13. Immediate next steps

1. **Run T4 v4** and read the results — this closes G0.2 and settles D7:
   ```bat
   cd /d "%USERPROFILE%\Desktop\NewKarnyNow\tests"
   python T4_d3d9_present.py
   ```
   It now runs 19 isolated processes per architecture (info, formats, device, surfaces,
   six `updatesurf` sub-steps, five `stretch` cases, four benchmarks). The JSON writer is
   fixed and unit-tested, so this time the data will actually be there. Per-stage files are
   kept in `T4_tmp\stages\`. Send `T4_wynik.json` and `T4_skrot.txt`.
2. **Write the logging subsystem first** (§7), before any protocol code. It is the only
   instrument available during the live attempts, and retrofitting it is how projects lose
   their one chance at a diagnosis.
3. **Build the known-answer vectors** for SRTP KDF/IV and for the Cauchy FEC (§7.9). They
   are the only way to prove crypto and erasure coding correct without a live session.
4. Then stage 1.

---

## 14. Open questions for the user

Ask these early; they change design decisions and are cheap to answer:

1. Is the display currently at 60 Hz, and is Intel **DPST** disabled for battery mode?
   (Affects D9 and the measured refresh rate.)
2. Which resolution/fps do you actually want as the default in the TUI — 720p30, or should
   the client offer a picker with 540p60 as an option? (Affects D2.)
3. Is the game list supposed to show the whole GFN catalog or only the account library
   (`notEquals: NOT_OWNED`)? The two come from different GraphQL operations, and only the
   library one is fully specified in the reference source.
4. Do you want the stream in a separate fullscreen window (recommended — the console stays
   free for the TUI and statistics), or embedded?
5. Should the client keep a local cache of catalog artwork, or is text-only acceptable?
   (Artwork means an HTTP cache and disk usage on a 2 GB machine.)

1. 60Hz to bazowa rozdzielczość, będę się starał ją utrzymać nawet jak przez przypadek wyciągnę zasilanie.
2. 720p domyślnie, ale możliwość zmiany będzie miła. nie wiem co lepsze, czy coś w rodzaju F7 do zmiany rozdzielczości czy ustawienia. 
3. tylko posiadane gry i tak używam gn pod gry free2play.
4. oczywiśie osobne okno streamu, tam będzie całe przechwytywanie klawiszy, myszy i tak dalej. po za tym tui python a screamer C, wiec lepiej odzielić je. 
5. nie wiem. 

**Run 33 marker (2026-10-29): the first game process, the first login,
the first world, the first real sound.** `Endfield.exe` JustLaunched at
rel 44.2 s after the macro's Play click; 80/80 events fired (108 wire
messages, zero gated/late/failed); 13:38 of content-bearing stereo audio
(Audacity-verified by the operator) and 29.94 fps of zero-loss video for
815.7 s; clean teardown + release. G3's input bar is MET LIVE; the audio
bar is met as extraction+content (independent decode pending an excerpt).
Stage 3's remaining bar is the presentation half: F20 live D3D9 video and
F21 live WASAPI audio inside `--session` — the skeletons of §3/§5 already
pass their selftests on the target (`present mean 0.31 ms`, synthetic
PCM), run 34 is the first run the operator watches and hears live. The
freeze family arrived at 815.2 s with the control plane still alive
(RTSP.md §40.2) — F22 instruments the witness.
