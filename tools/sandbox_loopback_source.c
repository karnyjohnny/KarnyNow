/* sandbox_loopback_source.c — verify the stage-1 loopback SOURCE path in the
 * sandbox: kn_decode_h264_file over streamer/assets/synthetic720.264 must
 * decode to exactly 120 NV12 frames at 1280x720 with sane strides. This is
 * the portable half of what the target's --loopback run will do (the D3D
 * half is Win32-only). Sandbox-only tool, not part of any gate.
 *
 * Build (repo root):
 *   gcc -std=c11 -O1 -Wall -Wextra -Istreamer/include -Istreamer/src \
 *       -DKN_HAVE_FFMPEG=1 tools/sandbox_loopback_source.c \
 *       streamer/src/kn_decode.c streamer/src/knlog.c streamer/src/knring.c \
 *       $(pkg-config --cflags --libs libavcodec libavutil libswscale) \
 *       -pthread -o /tmp/sls && /tmp/sls [path.264]
 */
#include "kn/kn_decode.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    long long frames;
    int dim_bad, nv12_bad, stride_bad;
    int w, h, sy, suv;
} src_count;

static int src_cb(const unsigned char *data, int w, int h, int sy, int suv,
                  int is_nv12, long long idx, void *ud)
{
    src_count *c = (src_count *)ud;
    (void)data; (void)idx;
    c->frames++;
    if (w != 1280 || h != 720) c->dim_bad++;
    if (!is_nv12) c->nv12_bad++;
    if (sy < w || suv < w) c->stride_bad++;
    c->w = w; c->h = h; c->sy = sy; c->suv = suv;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "streamer/assets/synthetic720.264";
    kn_decode_stats st;
    src_count cnt;
    int rc;

    memset(&cnt, 0, sizeof cnt);
    memset(&st, 0, sizeof st);
    rc = kn_decode_h264_file(path, 1, src_cb, &cnt, &st);
    printf("source: %s\nrc=%d frames=%lld (expect 120) failed=%d\n", path,
           rc, cnt.frames, st.frames_failed);
    printf("dims=%dx%d bad=%d nv12_bad=%d stride_bad=%d last sy=%d suv=%d\n",
           st.width, st.height, cnt.dim_bad, cnt.nv12_bad, cnt.stride_bad,
           cnt.sy, cnt.suv);
    printf("fps=%.1f cpu_pct_of_core=%.1f decode_ms_max=%lld wall_ms=%lld\n",
           st.fps, st.cpu_pct_of_core, st.decode_ms_max, st.wall_ms);
    if (rc == 0 && cnt.frames == 120 && st.frames_failed == 0 &&
        cnt.dim_bad == 0 && cnt.nv12_bad == 0 && cnt.stride_bad == 0) {
        printf("LOOPBACK SOURCE CHECK: PASS\n");
        return 0;
    }
    printf("LOOPBACK SOURCE CHECK: FAIL\n");
    return 1;
}
