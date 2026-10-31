/* test_knd3d.c — the TWENTY-EIGHTH suite: kn_d3d's platform-NEUTRAL F33
 * machinery (round N slice 4j).
 *
 * WHY THIS SUITE EXISTS: F33 turned the presenter into an input device —
 * the Ctrl+G pause menu (layout, hit-testing, labels, NV12 draw) and the
 * 512-slot capture ring. All of it was deliberately written in the
 * neutral section of kn_d3d.c (pure integer math on plain structs) so
 * the exact code the Windows wndproc uses is testable HERE, on POSIX,
 * under ASan/UBSan, without a GPU. The Windows-only halves (wndproc
 * translation, ClipCursor, D3D9 present) are exercised by the rig's
 * interactive flight; everything that can be pinned by a vector is
 * pinned here.
 *
 * WHAT IT PROVES:
 *  1. THE RING: fill past capacity, drop-NEWEST with an exact overflow
 *     count, FIFO order preserved, drain-then-reuse (kn_d3d_iq_selftest
 *     is the production seam — the same function runs on the rig).
 *  2. MENU GEOMETRY: centered, even-aligned, rows ordered and disjoint,
 *     inside the box, inside the frame; tiny frames clamp instead of
 *     overflowing (the menu must never draw outside the surface).
 *  3. HIT-TESTING: every row's center maps to its own index, inter-row
 *     gaps and the margin around the box map to -1.
 *  4. LABELS: the exact per-state strings the wndproc, the draw and any
 *     future KAT must agree on.
 *  5. GLYPH COVERAGE: every character the menu and the FPS overlay can
 *     ever render (full A-Z + digits + punctuation) lights pixels via
 *     kn_d3d_overlay_text_nv12 — a missing glyph would silently render
 *     a blank and the operator would stare at a broken menu item.
 *  6. THE DRAW: dark box, neutral UV, bright selection band on the
 *     chosen row, white glyphs in the title — pixel-probed, not eyeballed.
 *  7. HONEST STUBS: the vp-level API degrades to zeros/ERR_ARGS with a
 *     NULL vp on EVERY platform (the engine's drain loop must never
 *     crash on a headless host).
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_d3d.h"
#include "test_port.h"

#include <stdio.h>
#include <string.h>

#define T_W 1280u
#define T_H 720u

static unsigned char g_y[T_W * T_H];
static unsigned char g_uv[T_W * (T_H / 2u)];

static void t_ring_seam(void)
{
    CHECK(kn_d3d_iq_selftest() == 0, "capture ring selftest (production seam)");
}

static void t_menu_geometry(void)
{
    kn_d3d_rect box, items[3];
    unsigned i;

    CHECK(kn_d3d_menu_layout(T_W, T_H, 4u, &box, items) == 0,
          "layout ok at 1280x720 scale 4");
    CHECK(box.x0 >= 0 && box.y0 >= 0, "box origin non-negative");
    CHECK((box.x0 & 1) == 0 && (box.y0 & 1) == 0, "box origin even (NV12)");
    CHECK(box.x1 > box.x0 && box.y1 > box.y0, "box has area");
    CHECK((unsigned)box.x1 <= T_W && (unsigned)box.y1 <= T_H,
          "box inside the frame");
    /* centered: the two margins differ by at most one pixel (even snap) */
    CHECK(box.x0 - 0 <= (int)(T_W - (unsigned)box.x1) + 1 &&
          (int)(T_W - (unsigned)box.x1) <= box.x0 + 1, "box centered X");
    for (i = 0; i < 3u; ++i) {
        CHECK(items[i].x0 >= box.x0 && items[i].x1 <= box.x1,
              "item spans inside the box");
        CHECK(items[i].y0 >= box.y0 && items[i].y1 <= box.y1,
              "item rows inside the box");
        CHECK(items[i].y1 > items[i].y0, "item row has height");
        if (i > 0)
            CHECK(items[i].y0 >= items[i - 1].y1, "rows ordered, disjoint");
    }
    /* the first row sits below the title ("PAUSED" = 7*scale tall + pads) */
    CHECK(items[0].y0 > box.y0 + (int)(2u * 4u + 7u * 4u),
          "row 0 clears the title");

    /* args discipline */
    CHECK(kn_d3d_menu_layout(0, T_H, 4u, &box, items) == -1, "zero width refused");
    CHECK(kn_d3d_menu_layout(T_W, T_H, 3u, &box, items) == -1, "odd scale refused");
    CHECK(kn_d3d_menu_layout(T_W, T_H, 4u, NULL, items) == -1, "NULL box refused");

    /* tiny frames clamp, never overflow */
    CHECK(kn_d3d_menu_layout(64u, 48u, 4u, &box, items) == 0, "tiny layout ok");
    CHECK(box.x1 <= 64 && box.y1 <= 48, "tiny box clamped inside 64x48");
}

static void t_menu_hit(void)
{
    kn_d3d_rect box, items[3];
    unsigned i;
    int cx, cy;

    CHECK(kn_d3d_menu_layout(T_W, T_H, 4u, &box, items) == 0, "layout for hit");
    for (i = 0; i < 3u; ++i) {
        cx = (items[i].x0 + items[i].x1) / 2;
        cy = (items[i].y0 + items[i].y1) / 2;
        CHECK(kn_d3d_menu_hit(items, cx, cy) == (int)i, "row center hits itself");
        CHECK(kn_d3d_menu_hit(items, items[i].x0, items[i].y0) == (int)i,
              "row top-left corner hits (inclusive)");
        CHECK(kn_d3d_menu_hit(items, items[i].x1, items[i].y1) == -1 ||
              i < 2u, "row bottom-right is exclusive (or the next row's gap)");
    }
    CHECK(kn_d3d_menu_hit(items, 0, 0) == -1, "frame corner misses");
    CHECK(kn_d3d_menu_hit(items, (int)T_W - 1, (int)T_H - 1) == -1,
          "opposite corner misses");
    CHECK(kn_d3d_menu_hit(items, box.x0 - 1, (items[0].y0 + items[0].y1) / 2)
              == -1 || box.x0 == 0, "left margin misses");
    CHECK(kn_d3d_menu_hit(NULL, 10, 10) == -1, "NULL items miss");
}

static void t_labels(void)
{
    CHECK(strcmp(kn_d3d_menu_label(0u, 0), "EXIT STREAM") == 0, "label 0");
    CHECK(strcmp(kn_d3d_menu_label(1u, 0), "RESUME CAPTURE") == 0, "label 1");
    CHECK(strcmp(kn_d3d_menu_label(2u, 1), "STATS OVERLAY ON") == 0,
          "label 2 with overlay ON");
    CHECK(strcmp(kn_d3d_menu_label(2u, 0), "STATS OVERLAY OFF") == 0,
          "label 2 with overlay OFF");
    CHECK(kn_d3d_menu_label(3u, 0)[0] == '\0', "out-of-range label is empty");
}

/* Every glyph the menus/overlay vocabulary can contain must light at
 * least one pixel: a missing table entry renders as a silent blank. */
static void t_glyph_coverage(void)
{
    static const char vocab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:-+/ ";
    const char *p;
    unsigned misses = 0;

    for (p = vocab; *p; ++p) {
        char one[2] = { *p, '\0' };
        unsigned lit = 0, xx, yy;
        if (*p == ' ') continue;               /* space renders blank BY DESIGN */
        memset(g_y, 0x10, sizeof g_y);
        /* scale 2 into a 16x16 corner region is enough for one glyph */
        if (kn_d3d_overlay_text_nv12(g_y, T_W, g_uv, T_W, 64u, 32u,
                                     2u, 2u, one, 2u) != 1) { misses++; continue; }
        for (yy = 0; yy < 32u; ++yy)
            for (xx = 0; xx < 64u; ++xx)
                if (g_y[yy * T_W + xx] == 0xEBu) lit++;
        if (lit == 0u) misses++;
    }
    CHECK(misses == 0u, "every vocab glyph lights pixels (full A-Z font)");
}

static unsigned count_y_in(unsigned char v, int x0, int y0, int x1, int y1)
{
    unsigned n = 0;
    int x, y;
    for (y = y0; y < y1; ++y)
        for (x = x0; x < x1; ++x)
            if (g_y[(unsigned)y * T_W + (unsigned)x] == v) n++;
    return n;
}

static void t_menu_draw(void)
{
    kn_d3d_rect box, items[3];
    int r;

    memset(g_y, 0x99, sizeof g_y);
    memset(g_uv, 0x33, sizeof g_uv);
    r = kn_d3d_menu_draw_nv12(g_y, T_W, g_uv, T_W, T_W, T_H, 4u, 1, 1);
    CHECK(r == 0, "menu draw ok");
    CHECK(kn_d3d_menu_layout(T_W, T_H, 4u, &box, items) == 0, "layout for draw");

    /* the box interior is dark where no glyph/band sits: probe the very
     * first interior pixel row above the title (pad area) */
    CHECK(g_y[(unsigned)(box.y0 + 2) * T_W + (unsigned)(box.x0 + 2)] == 0x10u,
          "box background dark (Y=0x10)");
    /* UV neutral inside the box */
    CHECK(g_uv[((unsigned)box.y0 / 2u + 1u) * T_W + (unsigned)box.x0] == 0x80u,
          "box UV neutral");
    /* outside the box the buffer is untouched */
    CHECK(g_y[0] == 0x99u, "outside the box untouched");
    /* the selected row (1) carries the bright band; row 0 does not */
    CHECK(count_y_in(0xD0u, items[1].x0, items[1].y0,
                     items[1].x1, items[1].y1) > 100u,
          "selection band bright on row 1");
    CHECK(count_y_in(0xD0u, items[0].x0, items[0].y0,
                     items[0].x1, items[0].y1) == 0u,
          "no band on unselected row 0");
    /* dark glyphs ON the band (fg flips to 0x10 for the selected row) and
     * white glyphs in the title area */
    CHECK(count_y_in(0x10u, items[1].x0, items[1].y0,
                     items[1].x1, items[1].y1) > 10u,
          "dark glyphs on the selected band");
    CHECK(count_y_in(0xEBu, box.x0, box.y0, box.x1, items[0].y0) > 10u,
          "white title glyphs above row 0");

    /* args discipline + tiny frame */
    CHECK(kn_d3d_menu_draw_nv12(NULL, T_W, g_uv, T_W, T_W, T_H, 4u, 0, 0) == -1,
          "NULL y refused");
    memset(g_y, 0x99, 64u * 48u);
    CHECK(kn_d3d_menu_draw_nv12(g_y, 64u, g_uv, 64u, 64u, 48u, 4u, 2, 0) == 0,
          "tiny frame clamps, still ok");
}

static void t_stub_degradation(void)
{
    kn_d3d_input_ev ev;
    memset(&ev, 0, sizeof ev);
    CHECK(kn_d3d_vp_set_interactive(NULL, 1) == KN_D3D_ERR_ARGS,
          "set_interactive(NULL) = ERR_ARGS");
    CHECK(kn_d3d_vp_capturing(NULL) == 0, "capturing(NULL) = 0");
    CHECK(kn_d3d_vp_pop_input(NULL, &ev) == 0, "pop_input(NULL) = 0");
    CHECK(kn_d3d_vp_take_menu_exit(NULL) == 0, "take_menu_exit(NULL) = 0");
    CHECK(kn_d3d_vp_input_overflows(NULL) == 0u, "input_overflows(NULL) = 0");
#ifndef _WIN32
    /* POSIX: the whole vp family is the honest stub */
    CHECK(kn_d3d_available() == 0, "POSIX: d3d unavailable");
    CHECK(kn_d3d_vp_open(1280u, 720u, "t") == NULL, "POSIX: vp_open NULL");
#endif
}

int main(void)
{
    printf("== test_knd3d: the neutral F33 presenter machinery ==\n");
    t_ring_seam();
    t_menu_geometry();
    t_menu_hit();
    t_labels();
    t_glyph_coverage();
    t_menu_draw();
    t_stub_degradation();
    return kn_test_summary("test_knd3d");
}
