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
 * 0x10 of a row byte = leftmost pixel. Glyph set (F33): the FULL
 * uppercase A-Z vocabulary + digits + '.', ':', '-', '+', '/', space —
 * the overlay vocabulary AND the Ctrl+G pause-menu labels. */

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
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
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

/* =============================================== F33 neutral machinery
 * Pause-menu geometry + hit-testing + the capture input ring. All of it
 * platform-neutral (pure integer math on plain structs) so the native
 * POSIX test suite exercises the exact code the Windows wndproc uses.
 * Text metrics MATCH kn_d3d_overlay_text_nv12: a glyph is 5*scale wide,
 * glyphs are separated by `scale`, pad = 2*scale, even origins. */

#define KD_MENU_ITEMS 3
#define KD_MENU_SCALE 4u                       /* video-space px per font px */

typedef kn_d3d_rect kd_rect;   /* public name in kn_d3d.h */

/* The three fixed labels + the variable third-item suffix. Item 2 is the
 * widest possible label ("STATS OVERLAY OFF") and sizes the box. */
static const char *const k_kd_menu_label[KD_MENU_ITEMS] = {
    "EXIT STREAM", "RESUME CAPTURE", "STATS OVERLAY"
};

static unsigned kd_text_w(unsigned nglyphs, unsigned scale)
{
    return nglyphs ? nglyphs * 5u * scale + (nglyphs - 1u) * scale : 0u;
}

/* Box + per-item rows in VIDEO space (the same space the overlay blits
 * into). items[i] spans the full inner width (forgiving click target);
 * rows are 7*scale tall with a scale-sized gap. Returns 0 on success,
 * -1 on args. Everything is clamped inside the frame and forced even. */
int kn_d3d_menu_layout(unsigned win_w, unsigned win_h, unsigned scale,
                   kd_rect *box, kd_rect *items)
{
    unsigned inner, box_w, box_h, x0, y0, row_y, i;
    const unsigned title_g = 6u;               /* "PAUSED" */
    const unsigned wide_g  = 17u;              /* "STATS OVERLAY OFF" */
    if (!win_w || !win_h || !box || !items) return -1;
    if (scale < 2u || (scale & 1u)) return -1;
    inner = kd_text_w(wide_g, scale) + 4u * scale;      /* label + pad both */
    box_w = inner + 4u * scale;                          /* breathing room  */
    box_h = 2u * scale            /* top pad */
          + 7u * scale            /* title row */
          + 2u * scale            /* title gap */
          + KD_MENU_ITEMS * (7u * scale + scale)         /* rows + gaps */
          + scale;                /* bottom pad */
    if (box_w > win_w) box_w = win_w & ~1u;
    if (box_h > win_h) box_h = win_h & ~1u;
    x0 = ((win_w - box_w) / 2u) & ~1u;
    y0 = ((win_h - box_h) / 2u) & ~1u;
    box->x0 = (int)x0; box->y0 = (int)y0;
    box->x1 = (int)(x0 + box_w); box->y1 = (int)(y0 + box_h);
    (void)title_g;
    row_y = y0 + 2u * scale + 7u * scale + 2u * scale;
    for (i = 0; i < KD_MENU_ITEMS; ++i) {
        items[i].x0 = (int)(x0 + 2u * scale);
        items[i].y0 = (int)row_y;
        items[i].x1 = (int)(x0 + box_w - 2u * scale);
        items[i].y1 = (int)(row_y + 7u * scale);
        row_y += 7u * scale + scale;
    }
    return 0;
}

/* Point-in-menu hit test: returns 0..KD_MENU_ITEMS-1 or -1. */
int kn_d3d_menu_hit(const kd_rect *items, int x, int y)
{
    unsigned i;
    if (!items) return -1;
    for (i = 0; i < KD_MENU_ITEMS; ++i)
        if (x >= items[i].x0 && x < items[i].x1 &&
            y >= items[i].y0 && y < items[i].y1)
            return (int)i;
    return -1;
}

/* The third label depends on the overlay state; kept here so the draw
 * and any test agree on the exact string. */
const char *kn_d3d_menu_label(unsigned idx, int overlay_on)
{
    if (idx == 2u) return overlay_on ? "STATS OVERLAY ON"
                                     : "STATS OVERLAY OFF";
    return idx < KD_MENU_ITEMS ? k_kd_menu_label[idx] : "";
}

/* Draw the centered pause menu into an NV12 buffer: dark box, white
 * title "PAUSED", one row per item; the selected row gets a bright
 * background + dark glyphs. Same pixel discipline as the overlay blit
 * (even origins, per-2x2 UV pairs). Returns 0 ok / -1 args. */
int kn_d3d_menu_draw_nv12(unsigned char *y_plane, unsigned y_stride,
                      unsigned char *uv_plane, unsigned uv_stride,
                      unsigned buf_w, unsigned buf_h,
                      unsigned scale, int sel, int overlay_on)
{
    kd_rect box, items[KD_MENU_ITEMS];
    unsigned i, row, col, gy, rx, ry, sx, sy;
    int r;
    const char *text;
    if (!y_plane || !uv_plane || !buf_w || !buf_h) return -1;
    r = kn_d3d_menu_layout(buf_w, buf_h, scale, &box, items);
    if (r != 0) return r;

    /* box background: Y=0x10, neutral UV */
    for (row = (unsigned)box.y0; row < (unsigned)box.y1 && row < buf_h; ++row) {
        unsigned char *yl = y_plane + (size_t)row * y_stride;
        for (col = (unsigned)box.x0; col < (unsigned)box.x1 && col < buf_w; ++col)
            yl[col] = 0x10;
    }
    for (row = (unsigned)box.y0 / 2u; row < (unsigned)box.y1 / 2u &&
                                            row < buf_h / 2u; ++row) {
        unsigned char *uvl = uv_plane + (size_t)row * uv_stride;
        for (col = (unsigned)box.x0; col < (unsigned)box.x1 && col < buf_w;
             col += 2u) { uvl[col] = 0x80; uvl[col + 1u] = 0x80; }
    }

    /* rows: title + items */
    for (i = 0; i < KD_MENU_ITEMS + 1u; ++i) {
        int tx, ty;
        unsigned char fg = 0xEB, bg = 0x10;
        unsigned ng = 0, tw;
        const char *p;
        if (i == 0) {
            text = "PAUSED";
            ty = box.y0 + (int)(2u * scale);
        } else {
            text = kn_d3d_menu_label(i - 1u, overlay_on);
            ty = items[i - 1u].y0;
            if ((int)(i - 1u) == sel) { fg = 0x10; bg = 0xD0;
                /* bright selection band across the full row */
                for (row = (unsigned)items[i - 1u].y0;
                     row < (unsigned)items[i - 1u].y1 && row < buf_h; ++row) {
                    unsigned char *yl = y_plane + (size_t)row * y_stride;
                    for (col = (unsigned)items[i - 1u].x0;
                         col < (unsigned)items[i - 1u].x1 && col < buf_w; ++col)
                        yl[col] = bg;
                }
            }
        }
        for (p = text; *p; ++p) ++ng;
        tw = kd_text_w(ng, scale);
        tx = box.x0 + ((box.x1 - box.x0) - (int)tw) / 2;
        tx &= ~1;
        for (gy = 0; gy < ng; ++gy) {
            const kd_glyph *g = kd_glyph_of(text[gy]);
            unsigned cx = (unsigned)tx + gy * 6u * scale;
            if (!g) continue;
            for (ry = 0; ry < 7u; ++ry) {
                unsigned char bits = g->rows[ry];
                for (rx = 0; rx < 5u; ++rx) {
                    if (!(bits & (0x10u >> rx))) continue;
                    for (sy = 0; sy < scale; ++sy) {
                        unsigned yy = (unsigned)ty + ry * scale + sy;
                        unsigned char *yl;
                        if (yy >= buf_h) continue;
                        yl = y_plane + (size_t)yy * y_stride;
                        for (sx = 0; sx < scale; ++sx) {
                            unsigned xx = cx + rx * scale + sx;
                            if (xx >= buf_w) continue;
                            yl[xx] = fg;
                        }
                    }
                    for (sy = 0; sy < scale / 2u; ++sy) {
                        unsigned vy = ((unsigned)ty + ry * scale) / 2u + sy;
                        unsigned char *uvl;
                        if (vy >= buf_h / 2u) continue;
                        uvl = uv_plane + (size_t)vy * uv_stride;
                        for (sx = 0; sx < scale / 2u; ++sx) {
                            unsigned vx = (cx + rx * scale) / 2u + sx;
                            if (vx >= buf_w / 2u) continue;
                            uvl[vx * 2u] = 0x80; uvl[vx * 2u + 1u] = 0x80;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

/* The capture ring: 512 slots, drop-NEWEST on overflow (an overflow
 * means the owner thread stopped draining — honest counter, one WARN
 * from the Windows side). Neutral so POSIX tests drive it directly. */
#define KD_IQ_CAP 512u
typedef struct {
    kn_d3d_input_ev ev[KD_IQ_CAP];
    unsigned head, count;
    unsigned long long overflow;
} kd_iq;

static void kd_iq_init(kd_iq *q)
{
    q->head = 0; q->count = 0; q->overflow = 0;
}

static int kd_iq_push(kd_iq *q, const kn_d3d_input_ev *e)
{
    unsigned tail;
    if (q->count == KD_IQ_CAP) { q->overflow++; return -1; }
    tail = (q->head + q->count) % KD_IQ_CAP;
    q->ev[tail] = *e;
    q->count++;
    return 0;
}

static int kd_iq_pop(kd_iq *q, kn_d3d_input_ev *out)
{
    if (!q->count) return 0;
    *out = q->ev[q->head];
    q->head = (q->head + 1u) % KD_IQ_CAP;
    q->count--;
    return 1;
}

/* Single-entry seam so the static ring is exercised on EVERY platform
 * (native ctest) and never trips -Wunused-function on POSIX: fills the
 * ring, checks drop-newest + overflow counting + FIFO order. 0 = pass. */
int kn_d3d_iq_selftest(void)
{
    static kd_iq q;
    kn_d3d_input_ev e, out;
    unsigned i;
    kd_iq_init(&q);
    memset(&e, 0, sizeof e);
    for (i = 0; i < KD_IQ_CAP + 5u; ++i) {
        e.kind = KN_D3D_IEV_KEY;
        e.vk = i;
        if (i < KD_IQ_CAP) {
            if (kd_iq_push(&q, &e) != 0) return -1;
        } else if (kd_iq_push(&q, &e) == 0) return -2;   /* must refuse */
    }
    if (q.overflow != 5u) return -3;
    if (q.count != KD_IQ_CAP) return -4;
    for (i = 0; i < KD_IQ_CAP; ++i) {
        if (kd_iq_pop(&q, &out) != 1) return -5;
        if (out.vk != i) return -6;                      /* FIFO order */
    }
    if (kd_iq_pop(&q, &out) != 0) return -7;
    /* after draining, the ring accepts again */
    e.vk = 999;
    if (kd_iq_push(&q, &e) != 0) return -8;
    if (kd_iq_pop(&q, &out) != 1 || out.vk != 999u) return -9;
    return 0;
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
    /* ---- F33 (round N slice 4j): interactive capture ---- */
    volatile int interactive;         /* kn_d3d_vp_set_interactive       */
    volatile int focused;             /* WM_SETFOCUS / WM_KILLFOCUS      */
    volatile int paused;              /* Ctrl+G menu state               */
    volatile int menu_sel;            /* highlighted menu row 0..2       */
    volatile int exit_req;            /* menu EXIT STREAM latch          */
    int iq_warned;                    /* one WARN per ring overflow era  */
    unsigned cliw, clih;              /* client rect (event norms space) */
    kd_iq iq;                         /* neutral capture ring            */
    kn_d3d_capture_stats cs;          /* F33a: queued/dropped/flaps      */
    /* ---- F39 (run-102 read): the rig's cursor state decides the mouse
     * mode. Hidden (notification 0,0 = the game captured the cursor):
     * moves become warp-to-center REL deltas (wire type 7 — the only
     * motion the rig consumes in that state); Visible: the letterboxed
     * ABS path (type 5). Written by the engine thread only. ---- */
    volatile int cursor_hidden;
    int warp_x, warp_y;               /* client-rect center (warp point) */
};

/* F33a: focus GROUND TRUTH. Run 47 proved the flag-only model wrong in
 * both directions: WM_SETFOCUS can arrive before GWLP_USERDATA exists
 * (the flag misses its own birth) and foreground changes can skip
 * WM_KILLFOCUS (stale 1) — the operator's keys went to the true focus
 * window while our mouse path happily queued under a stale flag. Truth
 * = GetForegroundWindow(); on LOSS we auto-pause and arm the menu (the
 * operator's run-47 request: alt-tab back = control handed back). */
static void kd_vp_capture_apply(kn_d3d_vp *vp);   /* fwd: used below */

static void kd_vp_focus_refresh(kn_d3d_vp *vp)
{
    int fg = (GetForegroundWindow() == vp->hwnd) ? 1 : 0;
    if (fg == vp->focused) return;
    vp->focused = fg;
    vp->cs.focus_flaps++;
    if (fg) {
        kd_logf(KN_INFO, "presenter: focus GAINED (foreground truth)");
    } else {
        kd_logf(KN_INFO, "presenter: focus LOST - capture AUTO-PAUSED, "
                "menu armed (resume is explicit)");
        if (vp->interactive && !vp->paused) {
            vp->paused = 1;
            vp->menu_sel = 0;
        }
    }
    kd_vp_capture_apply(vp);
}

/* ---------------- F33 capture helpers (wndproc runs on the owner thread
 * inside kn_d3d_vp_pump, so the ring and these flags need no lock) ---- */

static unsigned kd_vp_mods(void)
{
    unsigned mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= KN_D3D_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= KN_D3D_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000)    mods |= KN_D3D_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
        mods |= KN_D3D_MOD_META;
    return mods;
}

static void kd_vp_capture_apply(kn_d3d_vp *vp)
{
    int cap = vp->interactive && vp->focused && !vp->paused;
    if (cap) {
        RECT wr;
        GetWindowRect(vp->hwnd, &wr);
        ClipCursor(&wr);              /* the mouse belongs to the game */
        if (vp->cursor_hidden) {
            /* F39: re-arm the warp point (the window may have changed)
             * and pull the OS cursor onto it so the first delta is
             * measured from the center, like every later one. */
            RECT rc;
            if (GetClientRect(vp->hwnd, &rc)) {
                vp->warp_x = rc.right / 2;
                vp->warp_y = rc.bottom / 2;
            }
            {   POINT p;
                p.x = vp->warp_x; p.y = vp->warp_y;
                if (ClientToScreen(vp->hwnd, &p)) SetCursorPos(p.x, p.y);
            }
        }
    } else {
        ClipCursor(NULL);             /* free the cursor for the OS/menu */
    }
    SetCursor(cap ? NULL : LoadCursor(NULL, IDC_ARROW));
}

/* F39: queue one REL sample per i16-sized chunk (output.rs splits
 * out-of-range deltas the same way). Relative samples are NEVER merged
 * — only consecutive ABS ones are (mouse-input.md: "Consecutive
 * absolute samples replace the previous one. Relative samples are not
 * merged."). */
static void kd_vp_queue_fail(kn_d3d_vp *vp);   /* fwd: defined below */
static void kd_vp_queue_mouse_rel(kn_d3d_vp *vp, int dx, int dy)
{
    while (dx || dy) {
        kn_d3d_input_ev e;
        int sx = dx > 32767 ? 32767 : (dx < -32768 ? -32768 : dx);
        int sy = dy > 32767 ? 32767 : (dy < -32768 ? -32768 : dy);
        memset(&e, 0, sizeof e);
        e.kind = KN_D3D_IEV_MOVE_REL;
        e.x = sx; e.y = sy;
        e.win_w = vp->vw; e.win_h = vp->vh;
        if (kd_iq_push(&vp->iq, &e) != 0) { kd_vp_queue_fail(vp); return; }
        vp->cs.queued_move_rel++;
        dx -= sx; dy -= sy;
    }
}

static void kd_vp_queue_fail(kn_d3d_vp *vp)
{
    if (!vp->iq_warned) {
        vp->iq_warned = 1;
        kd_logf(KN_WARN, "presenter: input ring FULL (%u slots) - events "
                "dropped newest-first; is the media loop draining?",
                (unsigned)KD_IQ_CAP);
    }
}

static void kd_vp_queue_key(kn_d3d_vp *vp, unsigned vk, unsigned mods, int down)
{
    kn_d3d_input_ev e;
    memset(&e, 0, sizeof e);
    e.kind = KN_D3D_IEV_KEY;
    e.vk = vk; e.mods = mods; e.down = down;
    e.win_w = vp->cliw; e.win_h = vp->clih;
    if (kd_iq_push(&vp->iq, &e) == 0) vp->cs.queued_key++;
    else kd_vp_queue_fail(vp);
}

/* Client px -> VIDEO space through the letterbox destination rect, so
 * the engine normalizes without any geometry knowledge and black bars
 * never shift the aim. Clamped to [0, dim-1] (the wire fields are
 * unsigned). Before the first letterbox computation (dstrc empty) the
 * mapping degenerates to (0,0) — the window is a black pre-fault rect
 * at that point anyway. */
static void kd_vp_client_to_video(const kn_d3d_vp *vp, int cx, int cy,
                                  int *vx, int *vy)
{
    int dw = vp->dstrc.right - vp->dstrc.left;
    int dh = vp->dstrc.bottom - vp->dstrc.top;
    if (dw <= 0 || dh <= 0) { *vx = 0; *vy = 0; return; }
    *vx = (int)(((long long)(cx - vp->dstrc.left) * (long long)vp->vw) / dw);
    *vy = (int)(((long long)(cy - vp->dstrc.top) * (long long)vp->vh) / dh);
    if (*vx < 0) *vx = 0;
    if (*vx > (int)vp->vw - 1) *vx = (int)vp->vw - 1;
    if (*vy < 0) *vy = 0;
    if (*vy > (int)vp->vh - 1) *vy = (int)vp->vh - 1;
}

static void kd_vp_queue_mouse(kn_d3d_vp *vp, int kind, unsigned id, int down,
                              LPARAM l, int wheel)
{
    kn_d3d_input_ev e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.vk = id; e.down = down;
    kd_vp_client_to_video(vp, (int)(short)LOWORD(l), (int)(short)HIWORD(l),
                          &e.x, &e.y);
    e.wheel = wheel;
    e.win_w = vp->vw; e.win_h = vp->vh;   /* normalization basis = video */
    /* F39 coalescing: a fresh ABS move REPLACES a pending unsent ABS
     * move (the reference's send queue keeps only the latest absolute
     * sample — run 102 draw 4 flew 1251 abs reports in 14 s). */
    if (kind == KN_D3D_IEV_MOVE && vp->iq.count > 0u) {
        unsigned tail = (vp->iq.head + vp->iq.count - 1u) % KD_IQ_CAP;
        if (vp->iq.ev[tail].kind == KN_D3D_IEV_MOVE) {
            vp->iq.ev[tail] = e;
            vp->cs.coalesced_move++;
            vp->cs.queued_move++;
            return;
        }
    }
    if (kd_iq_push(&vp->iq, &e) == 0) {
        if (kind == KN_D3D_IEV_MOVE)  vp->cs.queued_move++;
        else if (kind == KN_D3D_IEV_BTN) vp->cs.queued_btn++;
        else                          vp->cs.queued_wheel++;
    } else kd_vp_queue_fail(vp);
}

static void kd_vp_menu_activate(kn_d3d_vp *vp, int item)
{
    switch (item) {
    case 0:
        vp->exit_req = 1;             /* engine consumes -> "menu_exit" */
        kd_logf(KN_INFO, "presenter: menu EXIT STREAM requested");
        break;
    case 1:
        vp->paused = 0;
        kd_vp_capture_apply(vp);
        kd_logf(KN_INFO, "presenter: capture RESUMED (menu)");
        break;
    case 2:
        vp->overlay_on = !vp->overlay_on;
        kd_logf(KN_INFO, "presenter: FPS overlay %s (menu)",
                vp->overlay_on ? "ON" : "OFF");
        break;
    default: break;
    }
}

/* Click while paused: client px -> VIDEO space (the menu is drawn into
 * the video frame, then stretched), then the neutral hit test. */
static void kd_vp_menu_click(kn_d3d_vp *vp, LPARAM l)
{
    kn_d3d_rect box, items[KD_MENU_ITEMS];
    int vx, vy, hit;
    if (kn_d3d_menu_layout(vp->vw, vp->vh, KD_MENU_SCALE, &box, items) != 0)
        return;
    kd_vp_client_to_video(vp, (int)(short)LOWORD(l), (int)(short)HIWORD(l),
                          &vx, &vy);
    hit = kn_d3d_menu_hit(items, vx, vy);
    if (hit >= 0) {
        vp->menu_sel = hit;
        kd_vp_menu_activate(vp, hit);
    }
}

static const wchar_t k_kd_vp_cls[] = L"KarnyNowKnD3DVp";

static LRESULT CALLBACK kd_vp_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    kn_d3d_vp *vp = (kn_d3d_vp *)(uintptr_t)GetWindowLongPtrW(h,
                                                              GWLP_USERDATA);
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_CLOSE) {                 /* alt+F4 / taskbar: both modes */
        if (vp) vp->closed = 1;
        return 0;
    }
    if (!vp) return DefWindowProcW(h, m, w, l);

    if (!vp->interactive) {
        /* LEGACY presenter (loopback, --session-present): ESC closes,
         * F5 toggles the D20 overlay — untouched by F33. */
        if (m == WM_KEYDOWN && w == VK_ESCAPE) { vp->closed = 1; return 0; }
        if (m == WM_KEYDOWN && w == VK_F5) {
            vp->overlay_on = !vp->overlay_on;
            kd_logf(KN_INFO, "presenter: FPS overlay %s (F5)",
                    vp->overlay_on ? "ON" : "OFF");
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    /* ---- F33 interactive: focused window = ALL input belongs to GFN;
     * Ctrl+G is the only local key (pause + menu). While paused nothing
     * is forwarded; arrows/Enter/Esc drive the menu, clicks hit-test. */
    switch (m) {
    case WM_SETFOCUS:
        if (!vp->focused) {
            vp->focused = 1;
            vp->cs.focus_flaps++;
            kd_logf(KN_INFO, "presenter: focus GAINED (WM_SETFOCUS)");
        }
        kd_vp_capture_apply(vp);
        return 0;
    case WM_KILLFOCUS:
        if (vp->focused) {
            vp->focused = 0;
            vp->cs.focus_flaps++;
            kd_logf(KN_INFO, "presenter: focus LOST (WM_KILLFOCUS) - "
                    "capture AUTO-PAUSED, menu armed");
        }
        if (!vp->paused) { vp->paused = 1; vp->menu_sel = 0; }
        kd_vp_capture_apply(vp);
        return 0;
    case WM_SETCURSOR:
        /* F33b: interactive too — after capture_released (loop exit) the
         * window may still hold focus for the teardown moment; without
         * this the cursor hides itself again right after we freed it. */
        if (vp->interactive && vp->focused && !vp->paused) {
            SetCursor(NULL); return TRUE;
        }
        break;                            /* released/paused: class arrow */
    case WM_SIZE: {
        RECT cr;
        if (GetClientRect(h, &cr)) {
            vp->cliw = (unsigned)(cr.right - cr.left);
            vp->clih = (unsigned)(cr.bottom - cr.top);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        unsigned mods = kd_vp_mods();
        /* F33a self-heal: a key message ARRIVING is OS-guaranteed proof
         * this window holds keyboard focus — heal a flag that missed its
         * WM_SETFOCUS (run 47's mute-keyboard half). */
        if (!vp->focused) {
            vp->focused = 1;
            vp->cs.focus_flaps++;
            kd_logf(KN_INFO, "presenter: focus HEALED by key arrival "
                    "(WM_SETFOCUS was missed)");
            kd_vp_capture_apply(vp);
        }
        if (w == 'G' && (mods & KN_D3D_MOD_CTRL)) {   /* the escape hatch */
            if (!(l & 0x40000000)) {   /* F33a: ignore auto-repeat (run 47
                                        * held Ctrl+G -> 3 toggles/0.75 s) */
                vp->paused = !vp->paused;
                vp->menu_sel = 0;
                kd_vp_capture_apply(vp);
                kd_logf(KN_INFO, "presenter: capture %s (Ctrl+G)",
                        vp->paused ? "PAUSED, menu shown" : "RESUMED");
            }
            return 0;
        }
        if (vp->paused) {
            if (w == VK_UP)
                vp->menu_sel = (vp->menu_sel + KD_MENU_ITEMS - 1)
                               % KD_MENU_ITEMS;
            else if (w == VK_DOWN)
                vp->menu_sel = (vp->menu_sel + 1) % KD_MENU_ITEMS;
            else if (w == VK_RETURN || w == VK_SPACE)
                kd_vp_menu_activate(vp, vp->menu_sel);
            else if (w == VK_ESCAPE) {
                vp->paused = 0;
                kd_vp_capture_apply(vp);
            } else if (w == VK_F5)
                vp->overlay_on = !vp->overlay_on;
            return 0;                     /* nothing forwarded while paused */
        }
        if (vp->focused) kd_vp_queue_key(vp, (unsigned)w, mods, 1);
        return 0;                         /* ESC too: it belongs to the game */
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (!vp->focused) {              /* F33a self-heal (as above) */
            vp->focused = 1;
            vp->cs.focus_flaps++;
            kd_vp_capture_apply(vp);
        }
        if (!vp->paused && vp->focused)
            kd_vp_queue_key(vp, (unsigned)w, kd_vp_mods(), 0);
        return 0;
    case WM_MOUSEMOVE:
        kd_vp_focus_refresh(vp);         /* F33a: ground truth per message */
        if (!vp->focused)      { vp->cs.dropped_nofocus++; return 0; }
        if (vp->paused)        { vp->cs.dropped_paused++;  return 0; }
        if (vp->cursor_hidden) {
            /* F39: Hidden-cursor mode (the rig's notification (0,0) —
             * the game owns the cursor). Warp-to-center relative
             * capture: the delta since the last warp is the sample;
             * the SetCursorPos echo lands exactly ON the warp point
             * (delta 0, inherently silent) and re-warping is a no-op.
             * The reference uses a Raw Input thread; the warp method is
             * its Win32-classic equivalent (residual: one warp echo can
             * merge with real motion — the delta stays correct because
             * it is measured from the warp point either way). */
            int x = (int)(short)LOWORD(l), y = (int)(short)HIWORD(l);
            int dx = x - vp->warp_x, dy = y - vp->warp_y;
            if (dx || dy) {
                POINT p;
                kd_vp_queue_mouse_rel(vp, dx, dy);
                p.x = vp->warp_x; p.y = vp->warp_y;
                if (ClientToScreen(h, &p)) SetCursorPos(p.x, p.y);
            }
            return 0;
        }
        kd_vp_queue_mouse(vp, KN_D3D_IEV_MOVE, 0, 0, l, 0);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
        /* ids EQUAL KN_INPUT_BTN_*: left 1, middle 2, right 3, x1 4, x2 5 */
        unsigned id = (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP) ? 1u
                    : (m == WM_MBUTTONDOWN || m == WM_MBUTTONUP) ? 2u
                    : (m == WM_RBUTTONDOWN || m == WM_RBUTTONUP) ? 3u
                    : (HIWORD(w) == XBUTTON2 ? 5u : 4u);
        int down = (m == WM_LBUTTONDOWN || m == WM_MBUTTONDOWN ||
                    m == WM_RBUTTONDOWN || m == WM_XBUTTONDOWN) ? 1 : 0;
        kd_vp_focus_refresh(vp);         /* F33a: ground truth per message */
        if (vp->paused) {
            if (down) kd_vp_menu_click(vp, l);
            else vp->cs.dropped_paused++;
            return 0;
        }
        if (!vp->focused) { vp->cs.dropped_nofocus++; return 0; }
        kd_vp_queue_mouse(vp, KN_D3D_IEV_BTN, id, down, l, 0);
        return 0;
    }
    case WM_MOUSEWHEEL:
        kd_vp_focus_refresh(vp);         /* F33a: ground truth per message */
        if (!vp->focused) { vp->cs.dropped_nofocus++; return 0; }
        if (vp->paused)   { vp->cs.dropped_paused++;  return 0; }
        kd_vp_queue_mouse(vp, KN_D3D_IEV_WHEEL, 0, 0, l,
                          (int)(short)HIWORD(w));
        return 0;
    default:
        break;
    }
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
    /* F33: exstyle 0 — WS_EX_TOPMOST REMOVED on purpose (operator
     * requirement: alt+tab must work; the window takes focus on click
     * like any other). Frameless fullscreen stays: WS_POPUP, adapter
     * display-mode size. */
    return CreateWindowExW(0, k_kd_vp_cls, wtitle,
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
    /* F33: capture state starts clean; the ring + client rect are live
     * from creation (WM_SIZE may not have fired yet for WS_POPUP). */
    kd_iq_init(&vp->iq);
    {
        RECT cr;
        if (GetClientRect(hwnd, &cr)) {
            vp->cliw = (unsigned)(cr.right - cr.left);
            vp->clih = (unsigned)(cr.bottom - cr.top);
        }
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)(uintptr_t)vp);
    /* F33a: WM_SETFOCUS fired DURING CreateWindowExW — before USERDATA
     * existed — so seed the flag from the truth instead of missing the
     * window's own birth focus. */
    vp->focused = (GetFocus() == hwnd) ? 1 : 0;

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
        /* F33: the Ctrl+G pause menu rides the SAME locked buffer (no
         * extra lock/stretch pass), drawn over the live frame — the
         * stream keeps playing while capture is paused. */
        if (vp->interactive && vp->paused)
            (void)kn_d3d_menu_draw_nv12((unsigned char *)lr.pBits,
                        (unsigned)lr.Pitch, duv, (unsigned)lr.Pitch,
                        vp->vw, vp->vh, KD_MENU_SCALE, vp->menu_sel,
                        vp->overlay_on);
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

int kn_d3d_vp_set_interactive(kn_d3d_vp *vp, int on)
{
    if (!vp) return KN_D3D_ERR_ARGS;
    vp->interactive = on ? 1 : 0;
    if (on) {
        /* F33a: grab the keyboard at arm time — the process is
         * foreground-eligible right after the console launch; if
         * Windows still refuses, the per-message focus_refresh and the
         * key-arrival self-heal cover the rest. */
        (void)SetForegroundWindow(vp->hwnd);
        (void)SetFocus(vp->hwnd);
        vp->focused = (GetFocus() == vp->hwnd) ? 1 : 0;
        kd_vp_capture_apply(vp);
        kd_logf(KN_INFO, "presenter: interactive capture ON (Ctrl+G = "
                "pause menu; ESC belongs to the game; focus grabbed: %d)",
                vp->focused);
    } else {
        vp->paused = 0;
        ClipCursor(NULL);
        SetCursor(LoadCursor(NULL, IDC_ARROW)); /* F33b: visible again now */
    }
    return KN_D3D_OK;
}

int kn_d3d_vp_capturing(kn_d3d_vp *vp)
{
    return (vp && vp->interactive && vp->focused && !vp->paused) ? 1 : 0;
}

int kn_d3d_vp_paused(kn_d3d_vp *vp)
{
    return (vp && vp->paused) ? 1 : 0;
}

int kn_d3d_vp_capture_stats(kn_d3d_vp *vp, kn_d3d_capture_stats *out)
{
    if (!out) return KN_D3D_ERR_ARGS;
    if (!vp) { memset(out, 0, sizeof *out); return KN_D3D_ERR_ARGS; }
    *out = vp->cs;
    return KN_D3D_OK;
}

int kn_d3d_vp_pop_input(kn_d3d_vp *vp, kn_d3d_input_ev *out)
{
    if (!vp || !out) return 0;
    return kd_iq_pop(&vp->iq, out);
}

int kn_d3d_vp_take_menu_exit(kn_d3d_vp *vp)
{
    if (!vp) return 0;
    if (vp->exit_req) { vp->exit_req = 0; return 1; }
    return 0;
}

unsigned long long kn_d3d_vp_input_overflows(kn_d3d_vp *vp)
{
    return vp ? vp->iq.overflow : 0ull;
}

/* F39: the engine applies the pump's latched cursor state every tick;
 * idempotent. On the Hidden transition the warp point is (re)computed
 * from the client rect and the OS cursor is pulled onto it. */
int kn_d3d_vp_set_cursor_hidden(kn_d3d_vp *vp, int hidden)
{
    int h = hidden ? 1 : 0;
    if (!vp) return -1;
    if (vp->cursor_hidden == h) return 0;
    vp->cursor_hidden = h;
    if (h) {
        RECT rc;
        if (GetClientRect(vp->hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            vp->warp_x = rc.right / 2;
            vp->warp_y = rc.bottom / 2;
        } else {
            vp->warp_x = (int)vp->cliw / 2;   /* pre-window fallback */
            vp->warp_y = (int)vp->clih / 2;
        }
        if (vp->interactive && vp->focused && !vp->paused) {
            POINT p;
            p.x = vp->warp_x; p.y = vp->warp_y;
            if (ClientToScreen(vp->hwnd, &p)) SetCursorPos(p.x, p.y);
        }
        kd_logf(KN_INFO, "presenter: cursor HIDDEN by the rig - relative "
                "mode (type 7), warp center %d,%d", vp->warp_x, vp->warp_y);
    } else {
        kd_logf(KN_INFO, "presenter: cursor VISIBLE - absolute mode "
                "(type 5, letterbox-mapped)");
    }
    return 1;
}

int kn_d3d_vp_cursor_hidden(kn_d3d_vp *vp)
{
    return vp ? vp->cursor_hidden : 0;
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
    /* F33b: the clip rect is per-thread and SURVIVES DestroyWindow —
     * close must free the mouse explicitly or the operator is left with
     * a locked cursor after the window is gone (run 48 complaint). */
    ClipCursor(NULL);
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
int kn_d3d_vp_set_interactive(kn_d3d_vp *vp, int on)
{ (void)vp; (void)on; return KN_D3D_ERR_ARGS; }
int kn_d3d_vp_capturing(kn_d3d_vp *vp) { (void)vp; return 0; }
int kn_d3d_vp_paused(kn_d3d_vp *vp) { (void)vp; return 0; }
int kn_d3d_vp_capture_stats(kn_d3d_vp *vp, kn_d3d_capture_stats *out)
{
    if (!out) return KN_D3D_ERR_ARGS;
    memset(out, 0, sizeof *out);
    (void)vp;
    return KN_D3D_ERR_ARGS;
}
int kn_d3d_vp_pop_input(kn_d3d_vp *vp, kn_d3d_input_ev *out)
{ (void)vp; (void)out; return 0; }
int kn_d3d_vp_take_menu_exit(kn_d3d_vp *vp) { (void)vp; return 0; }
unsigned long long kn_d3d_vp_input_overflows(kn_d3d_vp *vp)
{ (void)vp; return 0; }
int kn_d3d_vp_set_cursor_hidden(kn_d3d_vp *vp, int hidden)
{ (void)vp; (void)hidden; return -1; }
int kn_d3d_vp_cursor_hidden(kn_d3d_vp *vp) { (void)vp; return 0; }
void kn_d3d_vp_close(kn_d3d_vp *vp) { (void)vp; }

#endif /* _WIN32 */
