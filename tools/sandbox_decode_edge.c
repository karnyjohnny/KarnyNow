/* sandbox_decode_edge.c — contract checks for the kd_send_drain refactor:
 *  1. abort at frame N via callback  -> rc == -4, callback saw exactly N
 *  2. truncated Annex-B tail         -> no crash, rc == 0, frames < 60
 *  3. full clip again                -> rc == 0, frames == 60
 * Exit 0 when all three hold.
 *
 * Sandbox-only (agent side, Linux + distro FFmpeg dev libs; NOT part of
 * any build gate). Build+run from the repo root, e.g.:
 *   gcc -std=c11 -O1 -Wall -Wextra -Istreamer/include -Istreamer/src \
 *       -DKN_HAVE_FFMPEG=1 tools/sandbox_decode_check.c \
 *       streamer/src/kn_decode.c streamer/src/knlog.c streamer/src/knring.c \
 *       $(pkg-config --cflags --libs libavcodec libavutil libswscale) \
 *       -pthread -o /tmp/sdc && /tmp/sdc
 */
#include "kn/kn_decode.h"
#include "kn_synthetic_clip.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    long long frames;
    long long abort_at;   /* <=0: never abort */
} edg_count;

static int edg_cb(const uint8_t *y, int w, int h, int sy, int suv,
                  int is_nv12, long long idx, void *ud)
{
    edg_count *c = (edg_count *)ud;
    (void)y; (void)w; (void)h; (void)sy; (void)suv; (void)is_nv12; (void)idx;
    c->frames++;
    if (c->abort_at > 0 && c->frames >= c->abort_at) return 1;
    return 0;
}

int main(void)
{
    kn_decode_stats st;
    edg_count cnt;
    int rc, fail = 0;

    /* 1. abort at frame 10 */
    memset(&cnt, 0, sizeof cnt); memset(&st, 0, sizeof st);
    cnt.abort_at = 10;
    rc = kn_decode_h264_mem(k_synthetic_clip, KN_SYNTH_CLIP_LEN, 1,
                            edg_cb, &cnt, &st);
    printf("abort:  rc=%d (expect -4) cb_frames=%lld (expect 10) decoded=%d\n",
           rc, cnt.frames, st.frames_decoded);
    if (rc != -4 || cnt.frames != 10) fail = 1;

    /* 2. truncated tail (cut mid-stream, no start code at the end) */
    memset(&cnt, 0, sizeof cnt); memset(&st, 0, sizeof st);
    cnt.abort_at = 0;
    rc = kn_decode_h264_mem(k_synthetic_clip, 30000, 1, edg_cb, &cnt, &st);
    printf("trunc:  rc=%d (expect 0) cb_frames=%lld (expect >0 <60) failed=%d\n",
           rc, cnt.frames, st.frames_failed);
    if (rc != 0 || cnt.frames <= 0 || cnt.frames >= 60) fail = 1;

    /* 3. full clip, plain count */
    memset(&cnt, 0, sizeof cnt); memset(&st, 0, sizeof st);
    rc = kn_decode_h264_mem(k_synthetic_clip, KN_SYNTH_CLIP_LEN, 1,
                            edg_cb, &cnt, &st);
    printf("full:   rc=%d (expect 0) cb_frames=%lld (expect 60) failed=%d\n",
           rc, cnt.frames, st.frames_failed);
    if (rc != 0 || cnt.frames != 60 || st.frames_failed != 0) fail = 1;

    printf("EDGE CHECKS: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
