# D3D9 presentation probes (`kn_d3d`) — implementation notes

Status: **VERIFIED ON THE TARGET (T10 round 5, 2026-10-23, Win7 x64, GCC
16.2.0): `d3d.formats` ok (10 checks) + `d3d.present` ok (11 checks,
30/30 iterations) — R25 MILESTONE 0 PASSED, R25 CLOSED.** Sandbox gates
before the run: native suites PASS, cross `-Werror` x64+i686 clean, ctest
8/8, T10 POSIX dry-run ALL PASS. Target evidence (round 5) is summarized in
the section "Target evidence" below; raw files in `artifacts/T10-round5/`
(workspace) and the committed `t10logs/` bundle
`diag-20261023T063643Z-1e10db8e.zip`.

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

## Target evidence (T10 round 5, 2026-10-23, E5500 + Intel 8086:2a42)

`d3d.formats` (ok, 115 ms, 10 checks) — every value matches G0.2 exactly:

| Field | Round-5 value | G0.2 expectation |
|---|---|---|
| adapter / driver | `8086:2a42`, `0x0008000f000a0b35` (8.15.10.2869) | match |
| mode / backbuffer | `1280x800@60`, X8R8G8B8, 16 modes, 60 Hz-at-res: 1 | match |
| CheckDeviceFormat | nv12/yuy2/uyvy/x8/a8/r5g6 all OK, x1r5g5b5 refused | match §2.2 |
| conversions | nv12→bb OK, yuy2→bb OK, x8→bb OK, bb→nv12 refused | match §2.3 |
| `srf_caps` | `0x03000300` (LINEAR+POINT, min+mag) | match §2.1 |
| CreateDevice | **HWVP `0x0` AND SWVP `0x0`** | **CORRECTION — see below** |

> **Correction to G0.2 §2.1** ("SOFTWARE_VERTEXPROCESSING fails with
> E_FAIL"): T4's `main()` only attempted SWVP as a fallback when HWVP
> FAILED — on this machine HWVP succeeded, so the SWVP attempt never ran
> and the reported `E_FAIL` was the variable's init value. The round-5
> probe attempts both sequentially (each device released before the next):
> **both return S_OK**. Production rule is unchanged (HAL + HWVP — D7;
> SWVP would move vertex work to the 2-core CPU), but the "SWVP is
> broken on this driver" belief is dead.

`d3d.present` (ok, 176-189 ms, 11 checks, standalone AND in-`--all`):

| Field | Standalone | In `--selftest=all` |
|---|---|---|
| iterations | 30/30 | 30/30 |
| **pitch (1280×720 NV12 DEFAULT)** | **1536** | **1536** |
| fill mean / **max** | 2.13 ms / **28.36 ms** | 2.13 ms / **28.65 ms** |
| StretchRect+Present mean / max | 0.43 / 5.86 ms | 0.28 / 4.98 ms |
| read-back mismatches | 0 | 0 |
| bytes per fill | 1 658 880 (= 1536×720×1.5) | same |
| every HRESULT / VEH / latch | 0x0 / 0x0 / false | same |

**New hard facts for stage 1 (both were invisible until now):**

1. **Pitch ≠ width.** 1280×720 NV12 DEFAULT locks with **pitch 1536**
   (1280+256; T4 measured pitch==width at 960×540 and 1280×800, so the
   driver's alignment depends on dimensions in a way we cannot predict —
   never assume, always read `D3DLOCKED_RECT.Pitch`). Consequence: the
   loopback path copies decoded NV12 into the locked surface **per row**
   (`min(kn_decode stride, surface pitch)` bytes × height, Y plane and UV
   plane separately) — a whole-buffer memcpy is WRONG. The milestone fill
   passed 30/30 precisely because it honoured 1536.
2. **First-touch cost.** `fill_max ≈ 28 ms` on the FIRST iteration (mean
   2.13) — first CPU touch of freshly created stolen UMA memory pays
   page-fault/zeroing cost. Stage 1 must **pre-fault the surface** (one
   throwaway fill+present during setup, before the paced loop) or eat a
   dropped frame at stream start.
3. Presentation is even cheaper than G0.2 measured: 0.28-0.43 ms mean for
   the 1:1 720p→(0,40,1280,760) letterbox (G0.2's 3.0 ms was a SCALED
   960×540→fullscreen stretch; max ~5-6 ms occasional). The 30 fps budget
   (33.3 ms) has ~10× headroom for the GPU side.
4. The VEH guard armed and disarmed cleanly twice per T10 run and never
   fired (no AV) — the happy path is proven; the catch path itself remains
   UNVERIFIED (cannot be provoked without risking the driver; it stays
   labelled as such — R29 note).

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
