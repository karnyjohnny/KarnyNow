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

#include <stdlib.h>
#include <string.h>

/* ============================================================ D20 overlay
 * Platform-NEUTRAL 5x7 bitmap font + NV12 blit (compiles and is testable
 * everywhere — tools/sandbox_overlay_font.c renders ASCII proofs). Bit
 * 0x10 of a row byte = leftmost pixel. Glyph set covers the overlay
 * vocabulary: digits, "FPS LATE", '.', ':', '-', '/', space. */

typedef struct { char c; unsigned char rows[7]; } kd_glyph;

static const kd_glyph k_kd_font[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};
#define KD_FONT_N ((int)(sizeof k_kd_font / sizeof k_kd_font[0]))

static const kd_glyph *kd_glyph_of(char c)
{
    int i;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (i = 0; i < KD_FONT_N; ++i)
        if (k_kd_font[i].c == c) return &k_kd_font[i];
    return NULL;                            /* unknown char -> blank */
}

int kn_d3d_overlay_text_nv12(unsigned char *y_plane, unsigned y_stride,
                             unsigned char *uv_plane, unsigned uv_stride,
                             unsigned buf_w, unsigned buf_h,
                             unsigned x0, unsigned y0,
                             const char *text, unsigned scale)
{
    unsigned nglyphs = 0, text_w, text_h, bx0, by0, bx1, by1, pad;
    unsigned gy, i, row;
    const char *p;

    if (!y_plane || !uv_plane || !text || !buf_w || !buf_h) return -1;
    if (scale < 2u || (scale & 1u)) return -1;   /* even scale for exact UV */
    for (p = text; *p; ++p) ++nglyphs;
    if (nglyphs == 0) return 0;
    if (nglyphs > 64u) nglyphs = 64u;            /* bounded overlay */

    x0 &= ~1u;                                   /* even origin */
    y0 &= ~1u;
    text_w = nglyphs * 5u * scale + (nglyphs - 1u) * scale;
    text_h = 7u * scale;
    pad = 2u * scale;
    bx0 = x0;
    by0 = y0;
    bx1 = x0 + text_w + 2u * pad;
    by1 = y0 + text_h + 2u * pad;
    if (bx1 > buf_w) bx1 = buf_w & ~1u;
    if (by1 > buf_h) by1 = buf_h & ~1u;

    /* dark background box (Y=0x10, neutral UV) so the overlay reads over
     * any content */
    for (row = by0; row < by1; ++row) {
        unsigned char *yl = y_plane + (size_t)row * y_stride;
        for (i = bx0; i < bx1; ++i) yl[i] = 0x10;
    }
    for (row = by0 / 2u; row < by1 / 2u; ++row) {
        unsigned char *uvl = uv_plane + (size_t)row * uv_stride;
        for (i = bx0; i < bx1; i += 2u) { uvl[i] = 0x80; uvl[i + 1] = 0x80; }
    }

    /* white glyphs */
    for (gy = 0; gy < nglyphs; ++gy) {
        const kd_glyph *g = kd_glyph_of(text[gy]);
        unsigned cx = x0 + pad + gy * 6u * scale;
        unsigned ry, rx, sy, sx;
        if (!g) continue;
        for (ry = 0; ry < 7u; ++ry) {
            unsigned char bits = g->rows[ry];
            for (rx = 0; rx < 5u; ++rx) {
                if (!(bits & (0x10u >> rx))) continue;
                for (sy = 0; sy < scale; ++sy) {
                    unsigned yy = y0 + pad + ry * scale + sy;
                    unsigned char *yl;
                    if (yy >= by1 || yy >= buf_h) continue;
                    yl = y_plane + (size_t)yy * y_stride;
                    for (sx = 0; sx < scale; ++sx) {
                        unsigned xx = cx + rx * scale + sx;
                        if (xx >= bx1 || xx >= buf_w) continue;
                        yl[xx] = 0xEB;
                    }
                }
                /* UV: scale is even and cx/pad are even, so each font pixel
                 * covers exactly (scale/2)^2 whole UV pairs */
                for (sy = 0; sy < scale / 2u; ++sy) {
                    unsigned vy = (y0 + pad + ry * scale) / 2u + sy;
                    unsigned char *uvl;
                    if (vy >= by1 / 2u || vy >= buf_h / 2u) continue;
                    uvl = uv_plane + (size_t)vy * uv_stride;
                    for (sx = 0; sx < scale / 2u; ++sx) {
                        unsigned vx = (cx + rx * scale) / 2u + sx;
                        if (vx >= bx1 / 2u || vx >= buf_w / 2u) continue;
                        uvl[vx * 2u] = 0x80;
                        uvl[vx * 2u + 1u] = 0x80;
                    }
                }
            }
        }
    }
    return (int)nglyphs;
}

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

/* Arm the guard. Returns 0 when protected, -1 when registration failed —
 * callers MUST refuse the risky section in that case (isolation rule). */
static int kd_veh_begin(void)
{
    if (g_veh_handle) return 0;              /* already armed (single-thread) */
    g_veh_code = 0;
    g_veh_addr = 0;
    g_veh_handle = AddVectoredExceptionHandler(1, kd_veh);
    if (!g_veh_handle) return -1;
    g_veh_armed = 1;
    return 0;
}

static void kd_veh_end(void)
{
    g_veh_armed = 0;
    if (g_veh_handle) {
        RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = NULL;
    }
}

/* D7 permanent-degrade latch trip (call from the longjmp landing only). */
static void kd_latch_trip(void)
{
    g_degraded_latch = 1;
    g_latch_code = g_veh_code;
    g_latch_addr = g_veh_addr;
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

    if (kd_veh_begin() != 0) {
        /* No guard, no risky section — running the fill unprotected would
         * violate the isolation agreement (a driver AV would kill the whole
         * knvstd process and every later self-test's evidence). */
        kd_logf(KN_WARN, "d3d.present: AddVectoredExceptionHandler failed "
                "(err %lu) — probe REFUSED", (unsigned long)GetLastError());
        return -2;
    }
    if (setjmp(g_veh_jmp) == 0) {
        kd_present_risky(p);
    } else {
        /* longjmp landing from kd_veh: driver state is undefined. No D3D
         * calls from here on (not even Release); the window and objects
         * are reclaimed by the OS at process exit. */
        kd_latch_trip();
        p->seh_code = g_veh_code;
        p->seh_write_addr = g_veh_addr;
        p->permanently_degraded = 1;
        kd_logf(KN_WARN,
                "d3d.present: EXCEPTION 0x%08lx writing at 0x%08lx%08lx — "
                "R25 confirmed on DEFAULT pool; PERMANENT degrade (D7)",
                g_latch_code,
                (unsigned long)(g_latch_addr >> 32),
                (unsigned long)(g_latch_addr & 0xFFFFFFFFull));
    }
    kd_veh_end();

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

/* ============================================================ presenter */

struct kn_d3d_vp {
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev;
    IDirect3DSurface9 *srf;
    HWND hwnd;
    unsigned vw, vh, sw, sh;
    RECT srcrc, dstrc;
    int pitch;
    volatile int closed;
    volatile int overlay_on;          /* D20: F5 toggles; default ON here */
    char overlay_text[64];
    unsigned long fault_code;
    unsigned long long fault_addr;
    unsigned refresh_initial;
};

static const wchar_t k_kd_vp_cls[] = L"KarnyNowKnD3DVp";

static LRESULT CALLBACK kd_vp_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || (m == WM_KEYDOWN && w == VK_ESCAPE)) {
        kn_d3d_vp *vp = (kn_d3d_vp *)(uintptr_t)GetWindowLongPtrW(h,
                                                                 GWLP_USERDATA);
        if (vp) vp->closed = 1;
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_F5) {           /* D20 overlay toggle */
        kn_d3d_vp *vp = (kn_d3d_vp *)(uintptr_t)GetWindowLongPtrW(h,
                                                                 GWLP_USERDATA);
        if (vp) {
            vp->overlay_on = !vp->overlay_on;
            kd_logf(KN_INFO, "presenter: FPS overlay %s (F5)",
                    vp->overlay_on ? "ON" : "OFF");
        }
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

static HWND kd_vp_make_window(int w, int h, const char *title)
{
    WNDCLASSW wc;
    ATOM a;
    wchar_t wtitle[128];
    int n;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = kd_vp_wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = k_kd_vp_cls;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    a = RegisterClassW(&wc);
    if (!a && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1,
                            wtitle, 127);
    if (n <= 0 || !wtitle[0]) {
        static const wchar_t def[] = L"KarnyNow loopback";
        memcpy(wtitle, def, sizeof def);
    }
    wtitle[127] = L'\0';
    return CreateWindowExW(WS_EX_TOPMOST, k_kd_vp_cls, wtitle,
                           WS_POPUP | WS_VISIBLE, 0, 0, w, h,
                           NULL, NULL, wc.hInstance, NULL);
}

/* Pre-fault: fill EVERY byte of the locked surface once (dark gray) and
 * present once. Absorbs the ~28 ms first-touch cost of freshly allocated
 * stolen UMA memory (measured round 5: fill_max 28.4/28.7 ms vs mean
 * 2.13) and warms the StretchRect/Present path BEFORE the paced loop. */
static void kd_vp_prefault(kn_d3d_vp *vp)
{
    D3DLOCKED_RECT lr;
    IDirect3DSurface9 *bb = NULL;
    memset(&lr, 0, sizeof lr);
    if (SUCCEEDED(IDirect3DSurface9_LockRect(vp->srf, &lr, NULL,
                                             D3DLOCK_NOSYSLOCK)) &&
        lr.pBits && lr.Pitch > 0) {
        size_t plane = (size_t)lr.Pitch * (size_t)vp->vh;
        vp->pitch = (int)lr.Pitch;
        memset(lr.pBits, 0x10, plane);                      /* Y: dark */
        memset((unsigned char *)lr.pBits + plane, 0x80, plane / 2u); /* UV */
        IDirect3DSurface9_UnlockRect(vp->srf);
    }
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(vp->dev, 0, 0,
                    D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        (void)IDirect3DDevice9_StretchRect(vp->dev, vp->srf, &vp->srcrc,
                                           bb, &vp->dstrc, D3DTEXF_LINEAR);
        IDirect3DSurface9_Release(bb);
    }
    (void)IDirect3DDevice9_Present(vp->dev, NULL, NULL, NULL, NULL);
}

kn_d3d_vp *kn_d3d_vp_open(unsigned video_w, unsigned video_h,
                          const char *title)
{
    /* volatile: vp crosses the setjmp/longjmp boundary (the longjmp landing
     * must see the real pointer to free it) — -Wclobbered clean by design. */
    kn_d3d_vp *volatile vp = NULL;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *srf = NULL;
    HWND hwnd = NULL;
    D3DDISPLAYMODE dm;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;

    if (!video_w || !video_h || (video_w & 1u) || (video_h & 1u)) return NULL;
    if (g_degraded_latch) {
        kd_logf(KN_WARN, "presenter: REFUSED — D7 permanent degrade latched "
                "(exception 0x%08lx earlier this process)", g_latch_code);
        return NULL;
    }

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        kd_logf(KN_WARN, "presenter: Direct3DCreate9 returned NULL");
        return NULL;
    }
    memset(&dm, 0, sizeof dm);
    if (FAILED(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT,
                                                &dm)) || dm.Width == 0) {
        dm.Width = 1280; dm.Height = 800;
        dm.RefreshRate = 60; dm.Format = D3DFMT_X8R8G8B8;
    }
    hwnd = kd_vp_make_window((int)dm.Width, (int)dm.Height, title);
    if (!hwnd) {
        kd_logf(KN_WARN, "presenter: window creation failed (err %lu)",
                (unsigned long)GetLastError());
        IDirect3D9_Release(d3d);
        return NULL;
    }
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth      = (UINT)dm.Width;     /* T4-proven parameters */
    pp.BackBufferHeight     = (UINT)dm.Height;
    pp.BackBufferFormat     = dm.Format;
    pp.BackBufferCount      = 1;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow        = hwnd;
    pp.Windowed             = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
             hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
             &pp, &dev);
    if (FAILED(hr) || !dev) {
        kd_logf(KN_WARN, "presenter: CreateDevice(HWVP) failed 0x%08lx",
                (unsigned long)hr);
        DestroyWindow(hwnd);
        IDirect3D9_Release(d3d);
        return NULL;
    }
    /* D7 standing rule: conv check BEFORE any StretchRect */
    hr = IDirect3D9_CheckDeviceFormatConversion(d3d, D3DADAPTER_DEFAULT,
             D3DDEVTYPE_HAL, D3DFMT_NV12, dm.Format);
    if (FAILED(hr)) {
        kd_logf(KN_WARN, "presenter: NV12->backbuffer conversion REFUSED "
                "0x%08lx — D7 rung 1 unavailable (YUY2/GDI ladder is stage 2)",
                (unsigned long)hr);
        IDirect3DDevice9_Release(dev);
        DestroyWindow(hwnd);
        IDirect3D9_Release(d3d);
        return NULL;
    }
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, (UINT)video_w,
             (UINT)video_h, D3DFMT_NV12, D3DPOOL_DEFAULT, &srf, NULL);
    if (FAILED(hr) || !srf) {
        kd_logf(KN_WARN, "presenter: NV12 DEFAULT surface %ux%u failed "
                "0x%08lx", video_w, video_h, (unsigned long)hr);
        IDirect3DDevice9_Release(dev);
        DestroyWindow(hwnd);
        IDirect3D9_Release(d3d);
        return NULL;
    }

    vp = (kn_d3d_vp *)calloc(1, sizeof *vp);
    if (!vp) {
        IDirect3DSurface9_Release(srf);
        IDirect3DDevice9_Release(dev);
        DestroyWindow(hwnd);
        IDirect3D9_Release(d3d);
        return NULL;
    }
    vp->d3d = d3d;
    vp->dev = dev;
    vp->srf = srf;
    vp->hwnd = hwnd;
    vp->vw = video_w;
    vp->vh = video_h;
    vp->sw = (unsigned)dm.Width;
    vp->sh = (unsigned)dm.Height;
    vp->refresh_initial = (unsigned)dm.RefreshRate;
    vp->overlay_on = 1;                         /* D20: debug default ON */
    snprintf(vp->overlay_text, sizeof vp->overlay_text, "FPS --.-");
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)(uintptr_t)vp);

    {
        int x0, y0, x1, y1;
        kd_letterbox(vp->vw, vp->vh, vp->sw, vp->sh, &x0, &y0, &x1, &y1);
        vp->dstrc.left = (LONG)x0;
        vp->dstrc.top = (LONG)y0;
        vp->dstrc.right = (LONG)x1;
        vp->dstrc.bottom = (LONG)y1;
    }
    vp->srcrc.left = 0; vp->srcrc.top = 0;
    vp->srcrc.right = (LONG)video_w;
    vp->srcrc.bottom = (LONG)video_h;

    /* Pre-fault under the VEH guard (the fill is the R25-class operation). */
    if (kd_veh_begin() != 0) {
        kd_logf(KN_WARN, "presenter: VEH guard unavailable — open REFUSED");
        IDirect3DSurface9_Release(srf);
        IDirect3DDevice9_Release(dev);
        DestroyWindow(hwnd);
        IDirect3D9_Release(d3d);
        free(vp);
        return NULL;
    }
    if (setjmp(g_veh_jmp) == 0) {
        kd_vp_prefault(vp);
    } else {
        kd_latch_trip();
        kd_logf(KN_WARN, "presenter: EXCEPTION 0x%08lx at 0x%08lx%08lx during "
                "pre-fault — PERMANENT degrade (D7); D3D objects intentionally "
                "leaked", g_latch_code, (unsigned long)(g_latch_addr >> 32),
                (unsigned long)(g_latch_addr & 0xFFFFFFFFull));
        kd_veh_end();
        free(vp);
        return NULL;
    }
    kd_veh_end();

    kd_logf(KN_INFO, "presenter open: NV12 DEFAULT %ux%u pitch %d -> "
            "backbuffer %ux%u dest (%ld,%ld)-(%ld,%ld), refresh %u Hz, "
            "pre-faulted", vp->vw, vp->vh, vp->pitch, vp->sw, vp->sh,
            (long)vp->dstrc.left, (long)vp->dstrc.top,
            (long)vp->dstrc.right, (long)vp->dstrc.bottom,
            vp->refresh_initial);
    return vp;
}

static int kd_vp_present_inner(kn_d3d_vp *vp, const unsigned char *y,
                               unsigned sy, const unsigned char *uv,
                               unsigned suv, double *copy_ms, double *sp_ms)
{
    D3DLOCKED_RECT lr;
    IDirect3DSurface9 *bb = NULL;
    HRESULT hr;
    long long t0, t1, t2;
    unsigned r;
    size_t wcopy;

    hr = IDirect3DDevice9_TestCooperativeLevel(vp->dev);
    if (FAILED(hr)) {
        kd_logf(KN_WARN, "presenter: device lost (TCL 0x%08lx) — stopping; "
                "the Reset()/rung ladder is stage 2", (unsigned long)hr);
        return KN_D3D_ERR_LOST;
    }
    memset(&lr, 0, sizeof lr);
    hr = IDirect3DSurface9_LockRect(vp->srf, &lr, NULL, D3DLOCK_NOSYSLOCK);
    if (FAILED(hr) || !lr.pBits || lr.Pitch <= 0)
        return KN_D3D_ERR_LOCK;
    if (vp->pitch == 0) vp->pitch = (int)lr.Pitch;

    t0 = kd_qpc_us();
    /* PER-ROW copy: the surface pitch (1536 for 1280x720 — round 5) differs
     * from the decoder stride (32-aligned); a whole-buffer memcpy is WRONG.
     * Valid pixels per row = vw; padding columns are never read (src RECT
     * is exactly vw x vh). */
    wcopy = (size_t)lr.Pitch < (size_t)vp->vw ? (size_t)lr.Pitch
                                              : (size_t)vp->vw;
    if ((size_t)sy < wcopy) wcopy = (size_t)sy;
    for (r = 0; r < vp->vh; ++r)
        memcpy((unsigned char *)lr.pBits + (size_t)r * (size_t)lr.Pitch,
               y + (size_t)r * (size_t)sy, wcopy);
    {
        unsigned char *duv = (unsigned char *)lr.pBits
                           + (size_t)lr.Pitch * (size_t)vp->vh;
        size_t wcopy_uv = wcopy;
        if ((size_t)suv < wcopy_uv) wcopy_uv = (size_t)suv;
        for (r = 0; r < vp->vh / 2u; ++r)
            memcpy(duv + (size_t)r * (size_t)lr.Pitch,
                   uv + (size_t)r * (size_t)suv, wcopy_uv);
        /* D20 FPS overlay: drawn into the SAME locked buffer right after
         * the copy (still inside the VEH-guarded section), so it costs no
         * extra lock/stretch/pass — ~1.5 k pixel writes for "FPS 30.0". */
        if (vp->overlay_on && vp->overlay_text[0])
            (void)kn_d3d_overlay_text_nv12((unsigned char *)lr.pBits,
                        (unsigned)lr.Pitch, duv, (unsigned)lr.Pitch,
                        vp->vw, vp->vh, 8u, 8u, vp->overlay_text, 2u);
    }
    t1 = kd_qpc_us();
    IDirect3DSurface9_UnlockRect(vp->srf);

    hr = IDirect3DDevice9_GetBackBuffer(vp->dev, 0, 0,
                                        D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        kd_logf(KN_WARN, "presenter: GetBackBuffer failed 0x%08lx",
                (unsigned long)hr);
        return KN_D3D_ERR_STRETCH;
    }
    hr = IDirect3DDevice9_StretchRect(vp->dev, vp->srf, &vp->srcrc,
                                      bb, &vp->dstrc, D3DTEXF_LINEAR);
    IDirect3DSurface9_Release(bb);
    if (FAILED(hr)) {
        kd_logf(KN_WARN, "presenter: StretchRect failed 0x%08lx",
                (unsigned long)hr);
        return KN_D3D_ERR_STRETCH;
    }
    hr = IDirect3DDevice9_Present(vp->dev, NULL, NULL, NULL, NULL);
    t2 = kd_qpc_us();
    if (FAILED(hr)) {
        kd_logf(KN_WARN, "presenter: Present failed 0x%08lx",
                (unsigned long)hr);
        return KN_D3D_ERR_PRESENT;
    }
    if (copy_ms) *copy_ms = (double)(t1 - t0) / 1000.0;
    if (sp_ms) *sp_ms = (double)(t2 - t1) / 1000.0;
    return KN_D3D_OK;
}

int kn_d3d_vp_present_nv12(kn_d3d_vp *vp, const unsigned char *y,
                           unsigned stride_y, const unsigned char *uv,
                           unsigned stride_uv, double *copy_ms,
                           double *stretch_present_ms)
{
    int rc;
    if (!vp || !y || !uv) return KN_D3D_ERR_ARGS;
    if (vp->fault_code || g_degraded_latch) return KN_D3D_ERR_FAULT;
    if (vp->closed) return KN_D3D_ERR_CLOSED;
    if (kd_veh_begin() != 0) {
        kd_logf(KN_WARN, "presenter: VEH guard unavailable — frame REFUSED");
        return KN_D3D_ERR_INIT;
    }
    if (setjmp(g_veh_jmp) == 0) {
        rc = kd_vp_present_inner(vp, y, stride_y, uv, stride_uv,
                                 copy_ms, stretch_present_ms);
    } else {
        vp->fault_code = g_veh_code;
        vp->fault_addr = g_veh_addr;
        kd_latch_trip();
        kd_logf(KN_WARN, "presenter: EXCEPTION 0x%08lx at 0x%08lx%08lx — "
                "PERMANENT degrade (D7); D3D objects intentionally leaked",
                vp->fault_code, (unsigned long)(vp->fault_addr >> 32),
                (unsigned long)(vp->fault_addr & 0xFFFFFFFFull));
        rc = KN_D3D_ERR_FAULT;
    }
    kd_veh_end();
    return rc;
}

int kn_d3d_vp_pump(kn_d3d_vp *vp)
{
    MSG m;
    if (!vp) return KN_D3D_ERR_ARGS;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return vp->closed ? KN_D3D_ERR_CLOSED : KN_D3D_OK;
}

unsigned kn_d3d_vp_refresh_hz(kn_d3d_vp *vp)
{
    D3DDISPLAYMODE dm;
    if (!vp || !vp->dev || vp->fault_code) return 0;
    memset(&dm, 0, sizeof dm);
    /* 3 args: (This, iSwapChain, pMode) — verified against mingw d3d9.h */
    if (FAILED(IDirect3DDevice9_GetDisplayMode(vp->dev, 0, &dm))) return 0;
    return (unsigned)dm.RefreshRate;
}

unsigned kn_d3d_vp_pitch(kn_d3d_vp *vp)
{
    return vp ? (unsigned)(vp->pitch > 0 ? vp->pitch : 0) : 0u;
}

unsigned long kn_d3d_vp_fault_code(kn_d3d_vp *vp)
{
    return vp ? vp->fault_code : 0ul;
}

unsigned long long kn_d3d_vp_fault_addr(kn_d3d_vp *vp)
{
    return vp ? vp->fault_addr : 0ull;
}

int kn_d3d_vp_set_overlay(kn_d3d_vp *vp, int on)
{
    if (!vp) return KN_D3D_ERR_ARGS;
    vp->overlay_on = on ? 1 : 0;
    return KN_D3D_OK;
}

int kn_d3d_vp_set_overlay_text(kn_d3d_vp *vp, const char *text)
{
    if (!vp || !text) return KN_D3D_ERR_ARGS;
    snprintf(vp->overlay_text, sizeof vp->overlay_text, "%s", text);
    return KN_D3D_OK;
}

int kn_d3d_vp_overlay(kn_d3d_vp *vp)
{
    return vp ? vp->overlay_on : 0;
}

void kn_d3d_vp_close(kn_d3d_vp *vp)
{
    if (!vp) return;
    if (!vp->fault_code && !g_degraded_latch) {
        if (vp->srf) IDirect3DSurface9_Release(vp->srf);
        if (vp->dev) IDirect3DDevice9_Release(vp->dev);
        if (vp->d3d) IDirect3D9_Release(vp->d3d);
    }
    /* else: D3D objects leaked ON PURPOSE (faulted driver — D7). */
    if (vp->hwnd) DestroyWindow(vp->hwnd);
    free(vp);
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

kn_d3d_vp *kn_d3d_vp_open(unsigned video_w, unsigned video_h,
                          const char *title)
{
    (void)video_w; (void)video_h; (void)title;
    return NULL;
}

int kn_d3d_vp_present_nv12(kn_d3d_vp *vp, const unsigned char *y,
                           unsigned stride_y, const unsigned char *uv,
                           unsigned stride_uv, double *copy_ms,
                           double *stretch_present_ms)
{
    (void)vp; (void)y; (void)stride_y; (void)uv; (void)stride_uv;
    (void)copy_ms; (void)stretch_present_ms;
    return KN_D3D_ERR_INIT;
}

int kn_d3d_vp_pump(kn_d3d_vp *vp) { (void)vp; return KN_D3D_ERR_INIT; }
unsigned kn_d3d_vp_refresh_hz(kn_d3d_vp *vp) { (void)vp; return 0; }
unsigned kn_d3d_vp_pitch(kn_d3d_vp *vp) { (void)vp; return 0; }
unsigned long kn_d3d_vp_fault_code(kn_d3d_vp *vp) { (void)vp; return 0; }
unsigned long long kn_d3d_vp_fault_addr(kn_d3d_vp *vp) { (void)vp; return 0; }
int kn_d3d_vp_set_overlay(kn_d3d_vp *vp, int on)
{ (void)vp; (void)on; return KN_D3D_ERR_ARGS; }
int kn_d3d_vp_set_overlay_text(kn_d3d_vp *vp, const char *text)
{ (void)vp; (void)text; return KN_D3D_ERR_ARGS; }
int kn_d3d_vp_overlay(kn_d3d_vp *vp) { (void)vp; return 0; }
void kn_d3d_vp_close(kn_d3d_vp *vp) { (void)vp; }

#endif /* _WIN32 */
