/* sandbox_overlay_font.c — ASCII proof for the D20 overlay font/blit.
 * Renders every glyph and a full "FPS 30.0 LATE 2" overlay into a fake
 * NV12 buffer, then prints the Y plane as ASCII ('#'=glyph 0xEB,
 * '+'=box 0x10, '.'=untouched) plus UV-neutrality checks. Also exercises
 * clipping (overlay at the buffer corner and half outside). Sandbox-only
 * tool, not part of any gate.
 *
 * Build (repo root):
 *   gcc -std=c11 -O1 -Wall -Wextra -Istreamer/include \
 *       tools/sandbox_overlay_font.c streamer/src/kn_d3d.c \
 *       -o /tmp/sof && /tmp/sof
 */
#include "kn/kn_d3d.h"

#include <stdio.h>
#include <string.h>

#define W 208
#define H 48

static void dump_y(const unsigned char *y, const unsigned char *ref,
                   unsigned w, unsigned h)
{
    unsigned r, c;
    for (r = 0; r < h; ++r) {
        for (c = 0; c < w; ++c) {
            unsigned char v = y[(size_t)r * w + c];
            char out = '.';
            if (v == 0xEB) out = '#';
            else if (v == 0x10 && ref[(size_t)r * w + c] != 0x10) out = '+';
            else if (v != ref[(size_t)r * w + c]) out = '?';
            putchar(out);
        }
        putchar('\n');
    }
}

int main(void)
{
    static unsigned char y[W * H], uv[W * H / 2], ref[W * H];
    unsigned i;
    int n, fail = 0;

    /* --- 1. every glyph, scale 1 view via scale 2 into the big buffer --- */
    memset(y, 0x30, sizeof y);          /* mid-gray backdrop */
    memset(uv, 0x55, sizeof uv);
    memcpy(ref, y, sizeof ref);
    n = kn_d3d_overlay_text_nv12(y, W, uv, W, W, H, 8, 8,
                                 "FPS 30.0 LATE 2", 2);
    printf("glyphs drawn: %d (expect 15: \"FPS 30.0 LATE 2\")\n", n);
    if (n != 15) fail = 1;
    dump_y(y, ref, W, H);

    /* --- 2. UV neutrality: every touched UV pair must be 0x80/0x80 --- */
    for (i = 0; i < W * H / 2; i += 2) {
        if (uv[i] != 0x55 && (uv[i] != 0x80 || uv[i + 1] != 0x80)) {
            printf("UV anomaly at %u: %02x %02x\n", i, uv[i], uv[i + 1]);
            fail = 1;
        }
    }

    /* --- 3. clipping: origin near the corner, text longer than buffer --- */
    memset(y, 0x30, sizeof y);
    memset(uv, 0x55, sizeof uv);
    memcpy(ref, y, sizeof ref);
    n = kn_d3d_overlay_text_nv12(y, W, uv, W, W, H, W - 12, H - 8,
                                 "FPS 30.0 LATE 2", 2);
    printf("\nclip corner: glyphs=%d (no crash, clipped)\n", n);
    n = kn_d3d_overlay_text_nv12(y, W, uv, W, W, H, 0, 0,
                                 "FPS 30.0 LATE 200000", 2);
    printf("clip overflow: glyphs=%d (no crash, clipped)\n", n);

    /* --- 4. argument discipline --- */
    if (kn_d3d_overlay_text_nv12(y, W, uv, W, W, H, 8, 8, "X", 3) != -1) {
        printf("odd scale accepted — WRONG\n"); fail = 1;
    }
    if (kn_d3d_overlay_text_nv12(NULL, W, uv, W, W, H, 8, 8, "X", 2) != -1) {
        printf("NULL y accepted — WRONG\n"); fail = 1;
    }
    if (kn_d3d_overlay_text_nv12(y, W, uv, W, W, H, 8, 8, "", 2) != 0) {
        printf("empty text != 0 glyphs\n"); fail = 1;
    }

    printf("\nOVERLAY FONT CHECK: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
