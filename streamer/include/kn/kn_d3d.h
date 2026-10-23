/* kn_d3d.h — D3D9 presentation subsystem, v0 = capability probes.
 *
 * Purpose (BOOTSTRAP §7.9 phase B, D7, R25 milestone 0):
 *   kn_d3d_probe_formats  the D7 session-start probe: CheckDeviceFormat /
 *                         CheckDeviceFormatConversion matrix over the rungs
 *                         (NV12 -> YUY2 -> X8R8G8B8), adapter identity,
 *                         current mode, 60 Hz availability (R9/D9), device
 *                         behaviour (HAL + HARDWARE_VERTEXPROCESSING).
 *   kn_d3d_probe_present  R25 milestone 0: the ONE video-path operation T4
 *                         never exercised — create DEFAULT-pool NV12, lock,
 *                         FILL EVERY BYTE (read-back verified), unlock,
 *                         StretchRect(LINEAR) into the backbuffer, Present,
 *                         timed, repeated; runs under a vectored-exception
 *                         guard so an access violation (the SYSTEMMEM crash
 *                         shape, G0.2 §3) is caught, recorded with its fault
 *                         address, and latches a PERMANENT degrade (D7:
 *                         never retry the failing operation in-process).
 *
 * Evidence base (everything here is measured, not remembered):
 *   docs/analysis/G0.2-T4v4-RESULTS.md  — settled chain, crash isolation
 *   tests/T4_d3d9_present.c             — the proven call patterns (v4)
 *   docs/D3D.md                         — design notes + provenance
 *
 * Hard rules encoded by this module (violating any of them crashed T4 v1-v3):
 *   - no SYSTEMMEM NV12 anywhere (G0.2 §3: filling a locked SYSTEMMEM NV12
 *     surface AVs the driver; the path is permanently dead — R2 CLOSED),
 *   - CheckDeviceFormatConversion before every StretchRect (D7),
 *   - format-conversion StretchRect destinations must be the BACKBUFFER
 *     (offscreen plain-surface targets return D3DERR_INVALIDCALL, G0.2 §2.5),
 *   - honour the pitch returned by LockRect, never assume pitch == width,
 *   - HAL + D3DCREATE_HARDWARE_VERTEXPROCESSING (SOFTWARE_VERTEXPROCESSING
 *     fails E_FAIL on the target driver; no REF fallback exists for YUV),
 *   - no CPU scaling anywhere (D6) — StretchRect does CSC + scale on the GPU.
 *
 * Platform contract: the header is platform-neutral plain C (no d3d9.h
 * types leak), so kn_selftest.c and host builds include it unchanged. On
 * non-Win32 builds kn_d3d_available() == 0 and the probes return -1 — the
 * self-tests then report an honest "skipped" (§10.6).
 *
 * Stage 1 will grow the --loopback presenter on top of this module; v0
 * intentionally holds no long-lived state (probe, report, release).
 */
#ifndef KN_D3D_H
#define KN_D3D_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of the D7 session-start capability probe. All HRESULTs are stored
 * as long (0x80004005-style values survive the round trip); format-matrix
 * fields are 1 = S_OK, 0 = D3DERR_NOTAVAILABLE, -1 = any other failure. */
typedef struct kn_d3d_formats {
    int  d3d_created;                 /* Direct3DCreate9 returned non-NULL */
    int  adapter_count;
    unsigned vendor_id;               /* PCI vendor (truth over Description) */
    unsigned device_id;               /* PCI device */
    unsigned long long driver_version;
    char description[64];             /* UTF-8, JSON-sanitized, truncated */
    unsigned mode_w, mode_h, mode_hz; /* current display mode */
    unsigned mode_fmt;                /* D3DFORMAT of the current mode */
    int  mode_count;                  /* modes enumerated for mode_fmt */
    int  modes_60hz_at_res;           /* 60 Hz entries at mode_w x mode_h (R9) */
    long hr_dev_hwvp;                 /* CreateDevice HAL+HWVP result */
    long hr_dev_swvp;                 /* CreateDevice HAL+SWVP result (info) */
    /* CheckDeviceFormat(HAL, usage 0, D3DRTYPE_SURFACE) */
    int  fmt_nv12, fmt_yuy2, fmt_uyvy;
    int  fmt_x8r8g8b8, fmt_a8r8g8b8, fmt_r5g6b5, fmt_x1r5g5b5;
    /* CheckDeviceFormatConversion(HAL, src -> mode_fmt) */
    int  conv_nv12_to_mode;           /* D7 rung 1 — the production path */
    int  conv_yuy2_to_mode;           /* D7 rung 2 */
    int  conv_x8_to_mode;             /* D7 rung 3 */
    int  conv_mode_to_nv12;           /* informational; expected 0 (G0.2 §2.3) */
    unsigned long long srf_caps;      /* StretchRectFilterCaps, raw */
    int  srf_min_linear, srf_mag_linear, srf_min_point, srf_mag_point;
    int  interval_immediate;          /* PresentationIntervals & IMMEDIATE */
    int  interval_one;                /* PresentationIntervals & ONE */
} kn_d3d_formats;

/* Result of the R25 milestone-0 presentation probe. `ok` is 1 only when the
 * full sequence (device -> NV12 DEFAULT surface -> lock -> fill every byte
 * -> verified read-back -> StretchRect(LINEAR) -> Present, N iterations)
 * completed with every HRESULT S_OK and no exception. */
typedef struct kn_d3d_present {
    int  ok;
    int  permanently_degraded;        /* D7 latch: exception caught; this
                                         process must never call D3D again */
    unsigned long seh_code;           /* 0 = none; e.g. 0xC0000005 */
    unsigned long long seh_write_addr;/* AV ExceptionInformation[1] */
    int  window_created;
    long hr_device, hr_conv, hr_surface, hr_lock, hr_stretch, hr_present;
    long hr_tcl_after;                /* TestCooperativeLevel post-loop */
    unsigned video_w, video_h;        /* NV12 source (production 1280x720) */
    unsigned screen_w, screen_h;
    int  dest_x0, dest_y0, dest_x1, dest_y1;  /* letterboxed dest RECT */
    int  pitch;                       /* LockRect pitch (honoured, recorded) */
    int  iterations;                  /* requested */
    int  iterations_done;             /* completed */
    int  fill_mismatches;             /* read-back verify, iteration 0 */
    unsigned long long fill_bytes_per_iter;
    int  filter_linear_used;
    double fill_mean_ms, fill_max_ms;             /* lock+fill+verify+unlock */
    double present_mean_ms, present_max_ms;       /* StretchRect+Present */
    unsigned backbuffer_fmt;          /* D3DFORMAT actually used */
} kn_d3d_present;

/* 1 when this build really talks to d3d9.dll (Win32), 0 on POSIX/stub. */
int kn_d3d_available(void);

/* Run the capability probe. Returns 0 when the probe executed (inspect the
 * struct — it carries the verdicts), -1 on unavailable/bad args. The struct
 * is always fully initialized. Never throws; no window is left behind. */
int kn_d3d_probe_formats(kn_d3d_formats *f);

/* Run the milestone-0 presentation probe (creates a real fullscreen window
 * briefly, presents `iterations` frames). Returns 0 when ok, -1 unavailable/
 * bad args, -2 executed but not ok, -3 an exception was caught (struct
 * carries seh_code/seh_write_addr; the permanent-degrade latch is set). */
int kn_d3d_probe_present(kn_d3d_present *p);

/* 1 once an exception was caught in this process (D7: degrade permanently;
 * stage 1 must fall through to the GDI rung or refuse to start the video
 * path). Sticky for the process lifetime. */
int kn_d3d_permanently_degraded(void);

#ifdef __cplusplus
}
#endif

#endif /* KN_D3D_H */
