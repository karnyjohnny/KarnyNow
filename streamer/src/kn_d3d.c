/* kn_d3d.c — D3D9 capability + presentation probes (contract: kn_d3d.h).
 *
 * Every call pattern here is taken from tests/T4_d3d9_present.c (v4) — the
 * harness that ran on the target and produced G0.2 (docs/analysis/
 * G0.2-T4v4-RESULTS.md). Pitfall rule: never write D3D9 from memory.
 *
 * Crash guard: the working agreement says "isolate anything that can crash
 * (separate process / SEH marker before risky calls)". mingw GCC does NOT
 * provide MSVC's __try/__except keywords (verified: both cross compilers
 * reject them), so the marker here is a vectored exception handler armed
 * around the risky section + longjmp escape. The only operation that can
 * fault is OUR OWN store instruction writing into the locked surface (that
 * is the exact T4 SYSTEMMEM crash shape: G0.2 §3 isolated the AV to the
 * fill loop, driver code not on the stack), so unwinding past it with
 * longjmp leaves no driver lock held by us. After ANY caught exception the
 * D7 latch trips: this process never calls D3D again (not even Release —
 * that re-enters the driver); the OS reclaims at exit.
 */
#include "kn/kn_d3d.h"

#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#include "kn/knlog.h"

/* T4 spelling: mingw's d3d9types.h has no NV12 enum member (it is an
 * undocumented FourCC); YUY2/UYVY DO exist in the mingw enum, so no
 * redefinition for them. */
#ifndef D3DFMT_NV12
#define D3DFMT_NV12 ((D3DFORMAT)MAKEFOURCC('N', 'V', '1', '2'))
#endif

/* Production video mode for the milestone probe (D14 default 720p30) and
 * iteration count (~0.2 s of frames at the measured 3 ms NV12 present). */
#define KD_VIDEO_W        1280u
#define KD_VIDEO_H        720u
#define KD_PRESENT_ITERS  30

/* ------------------------------------------------------------- logging */

static void kd_logf(int level, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    if (!kn_log_is_initialized()) return;
    if (!kn_log_would_log(KN_CH_DIAG, (kn_level)level)) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg[strcspn(msg, "\r\n")] = '\0';
    if (msg[0])
        kn_log_simple(KN_CH_DIAG, (kn_level)level, NULL, msg);
}

/* ---------------------------------------------------------------- time */

static long long kd_qpc_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER pc;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&pc);
    if (!freq.QuadPart) return 0;
    return (long long)((double)pc.QuadPart * 1000000.0 / (double)freq.QuadPart);
}

/* --------------------------------------------------------------- window */

static const wchar_t k_kd_cls[] = L"KarnyNowKnD3D";

static LRESULT CALLBACK kd_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)w;
    /* No PostQuitMessage here: the probes never run a GetMessage loop and
     * destroy the window explicitly — posting WM_QUIT would leave a stale
     * quit in the thread queue for the NEXT probe's message drain. */
    if (m == WM_ERASEBKGND) return 1;               /* T4 pattern */
    return DefWindowProcW(h, m, w, l);
}

/* One window class per process; a second probe in the same process reuses
 * it (RegisterClassW then fails with ERROR_CLASS_ALREADY_EXISTS — benign). */
static HWND kd_make_window(int w, int h, int visible, int topmost)
{
    WNDCLASSW wc;
    ATOM a;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = kd_wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = k_kd_cls;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    a = RegisterClassW(&wc);
    if (!a && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    return CreateWindowExW(topmost ? WS_EX_TOPMOST : 0, k_kd_cls,
                           L"KarnyNow d3d probe",
                           WS_POPUP | (visible ? WS_VISIBLE : 0),
                           0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
}

static void kd_drain_messages(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

/* ------------------------------------------------- exception guard (VEH) */

static PVOID g_veh_handle;
static volatile LONG g_veh_armed;
static volatile unsigned long g_veh_code;
static volatile unsigned long long g_veh_addr;
static jmp_buf g_veh_jmp;

/* D7 permanent-degrade latch: sticky for the process lifetime. */
static int g_degraded_latch;
static unsigned long g_latch_code;
static unsigned long long g_latch_addr;

static LONG CALLBACK kd_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code;
    if (!g_veh_armed || !ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    code = ep->ExceptionRecord->ExceptionCode;
    if (code != (DWORD)EXCEPTION_ACCESS_VIOLATION &&
        code != (DWORD)EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;
    g_veh_code = (unsigned long)code;
    g_veh_addr = (code == (DWORD)EXCEPTION_ACCESS_VIOLATION &&
                  ep->ExceptionRecord->NumberParameters >= 2)
               ? (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]
               : 0ULL;
    longjmp(g_veh_jmp, 1);
    /* not reached */
}

/* --------------------------------------------- NV12 fill/verify pattern */

/* Deterministic per-byte patterns; the verify pass recomputes them, so a
 * short/aliased allocation shows up either as an AV (caught) or as a
 * mismatch count. Rows are `pitch` bytes wide — the pitch RETURNED BY
 * LockRect is always honoured (T4: DEFAULT NV12 pitch == width; SYSTEMMEM
 * reported 2x width and lied about the allocation — G0.2 §3). */
static unsigned char kd_pat_y(unsigned x, unsigned y, unsigned it)
{
    return (unsigned char)(x * 3u + y * 5u + it * 7u);
}
static unsigned char kd_pat_uv(unsigned x, unsigned y, unsigned it)
{
    return (x & 1u) ? (unsigned char)(160u + (x & 31u) + it)
                    : (unsigned char)(90u + (y & 31u) + it);
}

static void kd_fill_nv12(unsigned char *bits, int pitch, unsigned h, int it)
{
    unsigned p = (unsigned)pitch, y, x;
    unsigned char *uv;
    for (y = 0; y < h; ++y) {
        unsigned char *row = bits + (size_t)y * p;
        for (x = 0; x < p; ++x) row[x] = kd_pat_y(x, y, (unsigned)it);
    }
    uv = bits + (size_t)p * h;                      /* NV12: UV after Y */
    for (y = 0; y < h / 2u; ++y) {
        unsigned char *row = uv + (size_t)y * p;
        for (x = 0; x < p; ++x) row[x] = kd_pat_uv(x, y, (unsigned)it);
    }
}

static int kd_verify_nv12(const unsigned char *bits, int pitch, unsigned h,
                          int it)
{
    unsigned p = (unsigned)pitch, y, x;
    const unsigned char *uv;
    int mism = 0;
    for (y = 0; y < h; ++y) {
        const unsigned char *row = bits + (size_t)y * p;
        for (x = 0; x < p; ++x)
            if (row[x] != kd_pat_y(x, y, (unsigned)it)) mism++;
    }
    uv = bits + (size_t)p * h;
    for (y = 0; y < h / 2u; ++y) {
        const unsigned char *row = uv + (size_t)y * p;
        for (x = 0; x < p; ++x)
            if (row[x] != kd_pat_uv(x, y, (unsigned)it)) mism++;
    }
    return mism;
}

/* --------------------------------------------------- letterbox (D6/D7) */

/* Aspect-preserving fit of video into screen; the GPU scales (D6: never
 * the CPU). 720p into 1280x800 reproduces the G0.2 §5 dest rect exactly:
 * (0,40)-(1280,760). */
static void kd_letterbox(unsigned vw, unsigned vh, unsigned sw, unsigned sh,
                         int *x0, int *y0, int *x1, int *y1)
{
    int dw, dh;
    if ((long long)vw * (long long)sh >= (long long)vh * (long long)sw) {
        dw = (int)sw;                               /* video wider: fit width */
        dh = (int)((long long)vh * (long long)sw / (long long)vw);
    } else {
        dh = (int)sh;                               /* fit height */
        dw = (int)((long long)vw * (long long)sh / (long long)vh);
    }
    *x0 = ((int)sw - dw) / 2;
    *y0 = ((int)sh - dh) / 2;
    *x1 = *x0 + dw;
    *y1 = *y0 + dh;
}

/* ------------------------------------------------------- formats probe */

static int kd_check_fmt(IDirect3D9 *d3d, D3DFORMAT adapter_fmt, D3DFORMAT fmt)
{
    /* T4 spelling: (Adapter, DeviceType, AdapterFormat, Usage, RType,
     * CheckFormat) — usage 0 + D3DRTYPE_SURFACE is the G0.2 §2.2 matrix. */
    HRESULT hr = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT,
                                              D3DDEVTYPE_HAL, adapter_fmt,
                                              0, D3DRTYPE_SURFACE, fmt);
    if (SUCCEEDED(hr)) return 1;
    return (hr == (HRESULT)D3DERR_NOTAVAILABLE) ? 0 : -1;
}

static int kd_check_conv(IDirect3D9 *d3d, D3DFORMAT src, D3DFORMAT dst)
{
    HRESULT hr = IDirect3D9_CheckDeviceFormatConversion(d3d,
                     D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, src, dst);
    if (SUCCEEDED(hr)) return 1;
    return (hr == (HRESULT)D3DERR_NOTAVAILABLE) ? 0 : -1;
}

static void kd_copy_desc(char *dst, size_t dstsz, const char *src)
{
    size_t i;
    if (!dst || dstsz == 0) return;
    for (i = 0; i + 1 < dstsz && i < MAX_DEVICE_IDENTIFIER_STRING && src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20u || c == '"' || c == '\\') ? '?' : (char)c;
    }
    dst[i] = '\0';
}

int kn_d3d_available(void) { return 1; }

int kn_d3d_permanently_degraded(void) { return g_degraded_latch; }

int kn_d3d_probe_formats(kn_d3d_formats *f)
{
    IDirect3D9 *d3d;
    D3DADAPTER_IDENTIFIER9 id;
    D3DCAPS9 caps;
    D3DDISPLAYMODE dm;
    D3DFORMAT mode_fmt;
    HWND hwnd;
    UINT n, i;

    if (!f) return -1;
    memset(f, 0, sizeof *f);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        kd_logf(KN_WARN, "d3d.formats: Direct3DCreate9 returned NULL");
        return 0;                       /* struct says d3d_created=0 */
    }
    f->d3d_created = 1;
    f->adapter_count = (int)IDirect3D9_GetAdapterCount(d3d);

    memset(&id, 0, sizeof id);
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT,
                                                  0, &id))) {
        f->vendor_id = (unsigned)id.VendorId;
        f->device_id = (unsigned)id.DeviceId;
        f->driver_version = (unsigned long long)id.DriverVersion.QuadPart;
        kd_copy_desc(f->description, sizeof f->description, id.Description);
    }

    memset(&dm, 0, sizeof dm);
    if (FAILED(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT,
                                                &dm)) || dm.Width == 0) {
        dm.Width = 1280; dm.Height = 800;          /* T4 fallback defaults */
        dm.RefreshRate = 60; dm.Format = D3DFMT_X8R8G8B8;
    }
    f->mode_w = (unsigned)dm.Width;
    f->mode_h = (unsigned)dm.Height;
    f->mode_hz = (unsigned)dm.RefreshRate;
    f->mode_fmt = (unsigned)dm.Format;
    mode_fmt = dm.Format;

    /* R9/D9 evidence: enumerate the modes of the CURRENT format and count
     * 60 Hz entries at the current resolution (T4: 60 Hz present, 40 Hz are
     * the Intel DPST battery modes). */
    n = IDirect3D9_GetAdapterModeCount(d3d, D3DADAPTER_DEFAULT, mode_fmt);
    f->mode_count = (int)n;
    for (i = 0; i < n; ++i) {
        D3DDISPLAYMODE m2;
        if (SUCCEEDED(IDirect3D9_EnumAdapterModes(d3d, D3DADAPTER_DEFAULT,
                                                  mode_fmt, i, &m2))) {
            if (m2.Width == dm.Width && m2.Height == dm.Height &&
                m2.RefreshRate == 60)
                f->modes_60hz_at_res++;
        }
    }

    /* CheckDeviceFormat matrix (G0.2 §2.2 expectations: all OK except
     * X1R5G5B5; a driver update changing this must SHOW UP, not silently
     * break stage 1). AdapterFormat = the current mode's format (T4). */
    f->fmt_nv12     = kd_check_fmt(d3d, mode_fmt, D3DFMT_NV12);
    f->fmt_yuy2     = kd_check_fmt(d3d, mode_fmt, D3DFMT_YUY2);
    f->fmt_uyvy     = kd_check_fmt(d3d, mode_fmt, D3DFMT_UYVY);
    f->fmt_x8r8g8b8 = kd_check_fmt(d3d, mode_fmt, D3DFMT_X8R8G8B8);
    f->fmt_a8r8g8b8 = kd_check_fmt(d3d, mode_fmt, D3DFMT_A8R8G8B8);
    f->fmt_r5g6b5   = kd_check_fmt(d3d, mode_fmt, D3DFMT_R5G6B5);
    f->fmt_x1r5g5b5 = kd_check_fmt(d3d, mode_fmt, D3DFMT_X1R5G5B5);

    /* Conversion matrix towards the CURRENT backbuffer format (D7 rungs). */
    f->conv_nv12_to_mode = kd_check_conv(d3d, D3DFMT_NV12, mode_fmt);
    f->conv_yuy2_to_mode = kd_check_conv(d3d, D3DFMT_YUY2, mode_fmt);
    f->conv_x8_to_mode   = kd_check_conv(d3d, D3DFMT_X8R8G8B8, mode_fmt);
    f->conv_mode_to_nv12 = kd_check_conv(d3d, mode_fmt, D3DFMT_NV12);

    /* Caps WITHOUT a device (GetDeviceCaps at the IDirect3D9 level). Mask
     * constants are the CORRECTED ones from G0.2 §2.1/§7.1 (the T4 harness
     * tested the wrong bits and printed srf_linear=false while LINEAR
     * worked everywhere — lesson: functional probe wins over caps bits). */
    memset(&caps, 0, sizeof caps);
    if (SUCCEEDED(IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT,
                                           D3DDEVTYPE_HAL, &caps))) {
        f->srf_caps = (unsigned long long)caps.StretchRectFilterCaps;
        f->srf_min_linear = (caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) != 0;
        f->srf_mag_linear = (caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR) != 0;
        f->srf_min_point  = (caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MINFPOINT)  != 0;
        f->srf_mag_point  = (caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MAGFPOINT)  != 0;
        f->interval_immediate =
            (caps.PresentationIntervals & D3DPRESENT_INTERVAL_IMMEDIATE) != 0;
        f->interval_one =
            (caps.PresentationIntervals & D3DPRESENT_INTERVAL_ONE) != 0;
    }

    /* Device-creation behaviour (G0.2 §2.1): HAL + HARDWARE_VERTEXPROCESSING
     * is the ONLY working combination on the target driver (SWVP -> E_FAIL,
     * recorded here for regression evidence). Small visible window; both
     * attempts sequentially, each device released before the next. */
    hwnd = kd_make_window(64, 64, 1, 0);
    if (hwnd) {
        D3DPRESENT_PARAMETERS pp;
        IDirect3DDevice9 *dev = NULL;
        memset(&pp, 0, sizeof pp);
        pp.BackBufferWidth      = 64;
        pp.BackBufferHeight     = 64;
        pp.BackBufferFormat     = mode_fmt;
        pp.BackBufferCount      = 1;
        pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow        = hwnd;
        pp.Windowed             = TRUE;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        f->hr_dev_hwvp = (long)IDirect3D9_CreateDevice(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &pp, &dev);
        if (dev) { IDirect3DDevice9_Release(dev); dev = NULL; }
        f->hr_dev_swvp = (long)IDirect3D9_CreateDevice(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &pp, &dev);
        if (dev) { IDirect3DDevice9_Release(dev); dev = NULL; }
        DestroyWindow(hwnd);
    } else {
        f->hr_dev_hwvp = (long)E_FAIL;
        f->hr_dev_swvp = (long)E_FAIL;
        kd_logf(KN_WARN, "d3d.formats: window creation failed (err %lu)",
                (unsigned long)GetLastError());
    }

    IDirect3D9_Release(d3d);
    kd_logf(KN_INFO,
            "d3d.formats: adapter %04x:%04x drv %08x mode %ux%u@%u "
            "nv12 fmt/conv %d/%d hwvp 0x%08lx",
            f->vendor_id % 0x10000u, f->device_id % 0x10000u,
            (unsigned)(f->driver_version & 0xFFFFFFFFull),
            f->mode_w % 100000u, f->mode_h % 100000u, f->mode_hz % 10000u,
            f->fmt_nv12, f->conv_nv12_to_mode, (unsigned long)f->hr_dev_hwvp);
    return 0;
}

/* ------------------------------------------------------- present probe */

/* The risky sequence. Runs under the VEH guard; on a caught exception this
 * frame is abandoned via longjmp (no cleanup — see file header). On normal
 * exit (success OR clean HRESULT failure) everything is released. */
static void kd_present_risky(kn_d3d_present *p)
{
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *srf = NULL;
    HWND hwnd = NULL;
    D3DDISPLAYMODE dm;
    D3DPRESENT_PARAMETERS pp;
    RECT srcrc, dstrc;
    long long fill_total = 0, fill_max = 0, pres_total = 0, pres_max = 0;
    int i;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        kd_logf(KN_WARN, "d3d.present: Direct3DCreate9 returned NULL");
        return;
    }
    memset(&dm, 0, sizeof dm);
    if (FAILED(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT,
                                                &dm)) || dm.Width == 0) {
        dm.Width = 1280; dm.Height = 800;
        dm.RefreshRate = 60; dm.Format = D3DFMT_X8R8G8B8;
    }
    p->screen_w = (unsigned)dm.Width;
    p->screen_h = (unsigned)dm.Height;
    p->backbuffer_fmt = (unsigned)dm.Format;

    hwnd = kd_make_window((int)dm.Width, (int)dm.Height, 1, 1);
    if (!hwnd) {
        kd_logf(KN_WARN, "d3d.present: window creation failed (err %lu)",
                (unsigned long)GetLastError());
        IDirect3D9_Release(d3d);
        return;
    }
    p->window_created = 1;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth      = (UINT)dm.Width;      /* T4-proven parameters */
    pp.BackBufferHeight     = (UINT)dm.Height;
    pp.BackBufferFormat     = dm.Format;
    pp.BackBufferCount      = 1;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow        = hwnd;
    pp.Windowed             = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    p->hr_device = (long)IDirect3D9_CreateDevice(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &pp, &dev);
    if (FAILED((HRESULT)p->hr_device) || !dev) {
        kd_logf(KN_WARN, "d3d.present: CreateDevice(HWVP) failed 0x%08lx",
                (unsigned long)p->hr_device);
        DestroyWindow(hwnd);
        IDirect3D9_Release(d3d);
        return;
    }

    /* D7 standing rule: CheckDeviceFormatConversion BEFORE any StretchRect
     * (the missing check is what killed T4 v1). */
    p->hr_conv = (long)IDirect3D9_CheckDeviceFormatConversion(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_NV12, dm.Format);
    if (FAILED((HRESULT)p->hr_conv)) {
        kd_logf(KN_WARN, "d3d.present: NV12->backbuffer conversion REFUSED "
                "0x%08lx — StretchRect not attempted (D7)",
                (unsigned long)p->hr_conv);
        IDirect3DDevice9_Release(dev);
        DestroyWindow(hwnd);
        IDirect3D9_Release(d3d);
        return;
    }

    /* DEFAULT pool ONLY — SYSTEMMEM NV12 is permanently dead (G0.2 §3). */
    p->hr_surface = (long)IDirect3DDevice9_CreateOffscreenPlainSurface(
        dev, (UINT)p->video_w, (UINT)p->video_h, D3DFMT_NV12,
        D3DPOOL_DEFAULT, &srf, NULL);
    if (FAILED((HRESULT)p->hr_surface) || !srf) {
        kd_logf(KN_WARN, "d3d.present: CreateOffscreenPlainSurface NV12 "
                "DEFAULT %ux%u failed 0x%08lx", p->video_w, p->video_h,
                (unsigned long)p->hr_surface);
        IDirect3DDevice9_Release(dev);
        DestroyWindow(hwnd);
        IDirect3D9_Release(d3d);
        return;
    }

    kd_letterbox(p->video_w, p->video_h, p->screen_w, p->screen_h,
                 &p->dest_x0, &p->dest_y0, &p->dest_x1, &p->dest_y1);
    srcrc.left = 0; srcrc.top = 0;
    srcrc.right = (LONG)p->video_w; srcrc.bottom = (LONG)p->video_h;
    dstrc.left = (LONG)p->dest_x0; dstrc.top = (LONG)p->dest_y0;
    dstrc.right = (LONG)p->dest_x1; dstrc.bottom = (LONG)p->dest_y1;

    kd_logf(KN_INFO,
            "d3d.present: begin — NV12 DEFAULT %ux%u -> backbuffer %ux%u "
            "dest (%d,%d)-(%d,%d), %d iterations, VEH guard armed",
            p->video_w, p->video_h, p->screen_w, p->screen_h,
            p->dest_x0, p->dest_y0, p->dest_x1, p->dest_y1, p->iterations);

    for (i = 0; i < p->iterations; ++i) {
        IDirect3DSurface9 *bb = NULL;
        D3DLOCKED_RECT lr;
        long long t0, t1, t2;
        HRESULT hr_bb;

        memset(&lr, 0, sizeof lr);
        p->hr_lock = (long)IDirect3DSurface9_LockRect(srf, &lr, NULL,
                                                      D3DLOCK_NOSYSLOCK);
        if (FAILED((HRESULT)p->hr_lock) || !lr.pBits || lr.Pitch <= 0) {
            kd_logf(KN_WARN, "d3d.present: LockRect failed 0x%08lx at iter %d",
                    (unsigned long)p->hr_lock, i);
            break;
        }
        if (i == 0) p->pitch = (int)lr.Pitch;
        p->fill_bytes_per_iter =
            (unsigned long long)lr.Pitch * (unsigned long long)p->video_h * 3u / 2u;

        t0 = kd_qpc_us();
        /* THE R25 OPERATION: CPU write of every byte of a locked DEFAULT
         * NV12 surface. If the driver under-allocated (the SYSTEMMEM shape)
         * this store faults -> VEH -> longjmp -> permanent degrade. */
        kd_fill_nv12((unsigned char *)lr.pBits, (int)lr.Pitch, p->video_h, i);
        if (i == 0)
            p->fill_mismatches =
                kd_verify_nv12((const unsigned char *)lr.pBits, (int)lr.Pitch,
                               p->video_h, i);
        IDirect3DSurface9_UnlockRect(srf);
        t1 = kd_qpc_us();
        fill_total += t1 - t0;
        if (t1 - t0 > fill_max) fill_max = t1 - t0;

        t1 = kd_qpc_us();
        hr_bb = IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                                               D3DBACKBUFFER_TYPE_MONO, &bb);
        if (SUCCEEDED(hr_bb) && bb) {
            /* Conversion destination is the BACKBUFFER (offscreen targets
             * return D3DERR_INVALIDCALL on this driver — G0.2 §2.5). */
            p->hr_stretch = (long)IDirect3DDevice9_StretchRect(
                dev, srf, &srcrc, bb, &dstrc, D3DTEXF_LINEAR);
            IDirect3DSurface9_Release(bb);
        } else {
            p->hr_stretch = (long)hr_bb;
        }
        if (SUCCEEDED((HRESULT)p->hr_stretch)) {
            p->hr_present = (long)IDirect3DDevice9_Present(dev, NULL, NULL,
                                                           NULL, NULL);
        }
        /* stretch failed -> hr_present stays 0 ("not reached"); a fabricated
         * E_FAIL here would misname the failing step in the evidence */
        t2 = kd_qpc_us();
        pres_total += t2 - t1;
        if (t2 - t1 > pres_max) pres_max = t2 - t1;

        if (FAILED((HRESULT)p->hr_stretch) || FAILED((HRESULT)p->hr_present)) {
            kd_logf(KN_WARN,
                    "d3d.present: stretch 0x%08lx / present 0x%08lx at iter %d",
                    (unsigned long)p->hr_stretch, (unsigned long)p->hr_present,
                    i);
            break;
        }
        p->iterations_done++;
        kd_drain_messages();
    }

    if (p->iterations_done == p->iterations)
        p->hr_tcl_after = (long)IDirect3DDevice9_TestCooperativeLevel(dev);

    if (p->iterations_done > 0) {
        p->fill_mean_ms = (double)fill_total / 1000.0 / (double)p->iterations_done;
        p->fill_max_ms = (double)fill_max / 1000.0;
        p->present_mean_ms = (double)pres_total / 1000.0 / (double)p->iterations_done;
        p->present_max_ms = (double)pres_max / 1000.0;
        p->filter_linear_used = SUCCEEDED((HRESULT)p->hr_stretch) ? 1 : 0;
    }
    p->ok = (p->iterations_done == p->iterations &&
             p->fill_mismatches == 0 &&
             SUCCEEDED((HRESULT)p->hr_lock) &&
             SUCCEEDED((HRESULT)p->hr_stretch) &&
             SUCCEEDED((HRESULT)p->hr_present) &&
             SUCCEEDED((HRESULT)p->hr_tcl_after)) ? 1 : 0;

    IDirect3DSurface9_Release(srf);
    IDirect3DDevice9_Release(dev);
    DestroyWindow(hwnd);
    IDirect3D9_Release(d3d);
}

int kn_d3d_probe_present(kn_d3d_present *p)
{
    int rc;
    if (!p) return -1;
    memset(p, 0, sizeof *p);
    p->video_w = KD_VIDEO_W;
    p->video_h = KD_VIDEO_H;
    p->iterations = KD_PRESENT_ITERS;

    if (g_degraded_latch) {
        /* D7: never retry the failing operation in this process. */
        p->permanently_degraded = 1;
        p->seh_code = g_latch_code;
        p->seh_write_addr = g_latch_addr;
        return -3;
    }

    g_veh_code = 0;
    g_veh_addr = 0;
    g_veh_handle = AddVectoredExceptionHandler(1, kd_veh);
    if (!g_veh_handle) {
        /* No guard, no risky section — running the fill unprotected would
         * violate the isolation agreement (a driver AV would kill the whole
         * knvstd process and every later self-test's evidence). */
        kd_logf(KN_WARN, "d3d.present: AddVectoredExceptionHandler failed "
                "(err %lu) — probe REFUSED", (unsigned long)GetLastError());
        return -2;
    }
    g_veh_armed = 1;
    if (setjmp(g_veh_jmp) == 0) {
        kd_present_risky(p);
    } else {
        /* longjmp landing from kd_veh: driver state is undefined. No D3D
         * calls from here on (not even Release); the window and objects
         * are reclaimed by the OS at process exit. */
        p->seh_code = g_veh_code;
        p->seh_write_addr = g_veh_addr;
        p->permanently_degraded = 1;
        g_degraded_latch = 1;
        g_latch_code = g_veh_code;
        g_latch_addr = g_veh_addr;
        kd_logf(KN_WARN,
                "d3d.present: EXCEPTION 0x%08lx writing at 0x%08lx%08lx — "
                "R25 confirmed on DEFAULT pool; PERMANENT degrade (D7)",
                g_latch_code,
                (unsigned long)(g_latch_addr >> 32),
                (unsigned long)(g_latch_addr & 0xFFFFFFFFull));
    }
    g_veh_armed = 0;
    if (g_veh_handle) {
        RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = NULL;
    }

    if (p->permanently_degraded) return -3;
    rc = p->ok ? 0 : -2;
    kd_logf(p->ok ? KN_INFO : KN_WARN,
            "d3d.present: %s — %d/%d iters, fill mean %.2f ms, "
            "stretch+present mean %.2f ms max %.2f ms, pitch %d",
            p->ok ? "MILESTONE 0 OK" : "NOT OK",
            p->iterations_done, p->iterations,
            p->fill_mean_ms, p->present_mean_ms, p->present_max_ms,
            p->pitch);
    return rc;
}

#else  /* !_WIN32 — honest stub (host builds; self-tests report "skipped") */

int kn_d3d_available(void) { return 0; }

int kn_d3d_permanently_degraded(void) { return 0; }

int kn_d3d_probe_formats(kn_d3d_formats *f)
{
    if (!f) return -1;
    memset(f, 0, sizeof *f);
    return -1;
}

int kn_d3d_probe_present(kn_d3d_present *p)
{
    if (!p) return -1;
    memset(p, 0, sizeof *p);
    return -1;
}

#endif /* _WIN32 */
