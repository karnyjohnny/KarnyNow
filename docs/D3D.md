# D3D9 presentation probes (`kn_d3d`) — implementation notes

Status: **IMPLEMENTED, sandbox-verified (native 454 checks + cross `-Werror`
x64/i686 + ctest 8/8 + T10 POSIX dry-run ALL PASS 31). NOT YET RUN ON THE
TARGET** — the authoritative execution is T10 round 5 on the Win7 machine
(the sandbox has no D3D9 runtime; only compile + link evidence exists here).

Scope v0 (phase B of §7.9): the two capability probes behind the
`d3d.formats` and `d3d.present` self-tests. Stage 1's `--loopback` presenter
will grow on this module; v0 intentionally holds no long-lived state
(probe → report → release).

## Why this module exists

1. **R25 milestone 0** (G0.2 §4): the ONLY video-path operation the T4
   measurement never exercised is *writing decoded pixels into a locked
   DEFAULT-pool NV12 surface*. Lock itself: verified OK (pitch = width).
   The SYSTEMMEM sibling of this operation **crashes the driver**
   (0xC0000005 isolated to the CPU fill — G0.2 §3). P=low, S=high → it must
   be proven once, in an isolated process, BEFORE the 10-minute G1 run.
   `knvstd --selftest=d3d.present` is exactly that proof, and T10 now runs
   it standalone first (own process) and again inside `--selftest=all`.
2. **D7 session-start probe**: the presentation chain (NV12 → YUY2 →
   X8R8G8B8 → GDI) is probed once at startup; `d3d.formats` is that probe
   in self-test form. A driver update that changes the matrix must show up
   here, not as garbled video in stage 1.

## Provenance (everything measured, nothing remembered)

All facts encoded in `kn_d3d.c` come from the target-machine evidence:

| Fact / pattern | Source |
|---|---|
| NV12 DEFAULT → `StretchRect(LINEAR)` → backbuffer works, 3.0 ms incl. Present | `docs/analysis/G0.2-T4v4-RESULTS.md` §1, §2.5 |
| SYSTEMMEM NV12 fill = 0xC0000005; path permanently dead | G0.2 §3 (T4 v4 sub-step split: the FILL is the fatal call) |
| Conversion destinations other than the backbuffer → `D3DERR_INVALIDCALL` | G0.2 §2.5 (last row) |
| HAL + HARDWARE_VERTEXPROCESSING only; SWVP → `E_FAIL`; REF has no YUV | G0.2 §2.1, §2.2 |
| `CheckDeviceFormat` matrix (NV12/YUY2/UYVY/X8/A8/R5G6B5 OK, X1R5G5B5 not) | G0.2 §2.2 |
| `CheckDeviceFormatConversion` (NV12→X8/A8/R5G6B5 OK; RGB→NV12 dead) | G0.2 §2.3 |
| DEFAULT NV12 lock OK, pitch == width (960→960, 1280→1280) | G0.2 §2.4 |
| `StretchRectFilterCaps = 0x03000300`; CORRECT masks MINFPOINT 0x100 / MINFLINEAR 0x200 / MAGFPOINT 0x1000000 / MAGFLINEAR 0x2000000 (the T4 harness tested the wrong bits) | G0.2 §2.1 + §7.1 lesson 1 |
| 60 Hz present at 1280×800; 40 Hz entries are Intel DPST battery modes; measure, never assume (D9) | G0.2 §2.1 (R9) |
| Letterbox dest for 720p in 1280×800: (0,40)-(1280,760) | G0.2 §5 |
| Window class/WndProc/present-parameter spellings (WS_POPUP\|WS_VISIBLE, TOPMOST, DISCARD, 1 backbuffer, IMMEDIATE) | `tests/T4_d3d9_present.c` (`make_window`, `main` — zero-warning on target GCC 16.2) |
| C-style COM call spellings (`IDirect3DDevice9_StretchRect` etc.) | T4 v4, verified against mingw `d3d9.h` macros in the sandbox |

Landmines hit while writing this module (recorded so nobody repeats them):

- `IDirect3D9_CheckDeviceFormat` takes **6 parameters** after `This`
  (Adapter, DeviceType, **AdapterFormat**, Usage, RType, CheckFormat) —
  the AdapterFormat is easy to drop from memory; the mingw macro arity
  caught it at `-Werror`.
- mingw `D3DADAPTER_IDENTIFIER9.Description` is `char[512]`, **not** WCHAR
  (checked `d3d9types.h`, not remembered).
- `D3DFMT_X8R8G8B8 = 22`, `A8R8G8B8 = 21`, `R5G6B5 = 23` (the header name
  helper in `kn_selftest.c` uses the numeric values — verified against
  `d3d9types.h` because the POSIX side has no d3d9.h).
- `D3DFMT_NV12` does not exist in mingw's enum → T4's guarded
  `MAKEFOURCC('N','V','1','2')` define is reused verbatim.

## The crash guard: VEH + longjmp (not `__try`)

The working agreement says *"isolate anything that can crash (separate
process / SEH marker before risky calls)"*. Verified in the sandbox:
**mingw GCC (both arches) does not provide MSVC's `__try/__except`
keywords** — so the marker here is:

1. `AddVectoredExceptionHandler(1, ...)` armed immediately around the
   risky section (registered FIRST in the chain — relevant under ConEmu's
   hook DLL), disarmed right after.
2. The handler accepts only `EXCEPTION_ACCESS_VIOLATION` /
   `EXCEPTION_IN_PAGE_ERROR` while armed, records the code and the fault
   **write address** (`ExceptionInformation[1]`), then `longjmp`s out.
3. Why longjmp is sound here: the faulting instruction is **our own store**
   into the mapped surface buffer — the exact T4 crash shape (G0.2 §3
   isolated the AV to the fill loop, driver code not on the stack). No
   driver lock is held by us at that instant (LockRect already returned).
4. On catch: the **D7 latch** trips (`kn_d3d_permanently_degraded()`),
   the process never calls D3D again — not even `Release` (that re-enters
   the driver). Objects are reclaimed by the OS at exit. Subsequent
   self-tests (decode etc.) are unaffected and still run.
5. Belt and braces: T10 ALSO runs `--selftest=d3d.present` standalone
   before `--selftest=all`, and knvstd now logs a `selftest_begin` marker +
   each result IMMEDIATELY at INFO (soft-flushed per §7.12), so even an
   uncatchable fault names the running test in `diag.log` — the T4/R2b
   "evidence survives the crash" rule.

State crossing the setjmp/longjmp boundary is static or struct-borne (no
clobbered locals — `-Wclobbered`-clean at `-O0..-O3` on both arches).

## Probe design

### `kn_d3d_probe_formats` (d3d.formats)

No window is needed for the matrix itself: `IDirect3D9_GetDeviceCaps`
gives `StretchRectFilterCaps`/`PresentationIntervals` at the IDirect3D9
level. A small (64×64) visible window is created only for the two
`CreateDevice` behaviour attempts (HWVP then SWVP, each released before
the next — sequential, never two live devices). Records: PCI
vendor:device (truth over `Description`), driver version, current mode +
mode list for the current format with a **60 Hz-at-current-resolution
count** (R9/D9 regression evidence in every bundle), the 7-format
`CheckDeviceFormat` row, the 4-pair conversion matrix (including the
informational RGB→NV12 refusal), and both CreateDevice HRESULTs.

Self-test verdicts: FAILED without d3d/device/mode or with no usable rung;
DEGRADED when rung 1 (NV12) or LINEAR caps or 60 Hz are missing but a
fallback exists; OK when the production chain is fully available.

### `kn_d3d_probe_present` (d3d.present, R25 milestone 0)

Production-shaped rehearsal at 1280×720 (D14 default) into a fullscreen
window sized to `GetAdapterDisplayMode` (1280×800 expected; fallback
constants match T4):

```
CreateDevice(HAL, HWVP|MULTITHREADED, DISCARD, IMMEDIATE)
  -> CheckDeviceFormatConversion(NV12 -> backbuffer)   [refused = NO stretch, D7]
  -> CreateOffscreenPlainSurface(NV12, DEFAULT)        [never SYSTEMMEM]
  -> 30 x { LockRect(NOSYSLOCK)
            fill EVERY byte of the locked allocation (pitch honoured:
              Y = pitch*h, UV = pitch*h/2 at offset pitch*h)
            iteration 0: full read-back verify (pattern recomputed)
            UnlockRect
            GetBackBuffer -> StretchRect(LINEAR, letterbox dest) -> Present
            Release backbuffer, drain messages, QPC timings }
  -> TestCooperativeLevel
```

Timings are reported separately for fill (CPU) and StretchRect+Present
(GPU) — mean and max, so the G1 budget analysis gets the real E5500/Intel
UMA numbers (G0.2 predicted ~3 ms present at these sizes; a mean > 16.7 ms
degrades the verdict). The fill pattern is deterministic per byte
(`kd_pat_y`/`kd_pat_uv`), so a short or aliased allocation shows up either
as a caught AV (with the fault address recorded next to the intended
extent — the G0.2 §3 diagnosis, automatically) or as a read-back mismatch
count.

## Wiring

- `kn_selftest.c`: `st_d3d_formats`/`st_d3d_present` render status/reason/
  evidence from the probe structs; on POSIX the probes report
  `kn_d3d_available() == 0` → honest "win32-only … target machine" skip.
  All d3d9.h types stay inside `kn_d3d.c` (the header is plain C).
- Relevance mapping (§7.7 step 4) extended: `video|decode|garbl|corrupt|
  d3d|present|stretch|gpu` → `fec.vectors + decode.synthetic +
  d3d.formats + d3d.present`. If the latch already tripped, the re-run
  returns immediately (-3) — no crash loop inside a failure bundle.
- Build: `kncore` gains `kn_d3d.c`; WIN32 links `d3d9` (kncore INTERFACE,
  plus explicit `-ld3d9` in `cross_check.sh`). Resulting PE imports:
  d3d9/kernel32/msvcrt/user32/winmm — all Win7-built-in, F1 `-static`
  unaffected (libd3d9.a is an import archive like winmm's).

## Stage-1 TODOs growing from here

- The presenter object (device + swapchain lifecycle, D7 rung fallback at
  runtime, device-lost handling via TestCooperativeLevel, the GDI rung 4).
- Reuse `kd_letterbox`, the fill path (sws_scale writes into the locked
  surface instead of `kd_fill_nv12`), and the QPC pacing (D9: measure the
  Present interval, never assume).
- `d3d.formats` results should feed the startup bundle (state.json) so
  every field report carries the video-path capability snapshot.
