/* sandbox_decode_check.c — off-target reproduction/verification harness for
 * the decode.synthetic self-test path. Links kn_decode.c against the sandbox
 * FFmpeg (bookworm 5.1, avcodec 59 — the target uses master avcodec 63; the
 * parser/decoder flush semantics are identical across both).
 *
 * Exit 0 = 60/60 frames decoded, rc==0, 0 failed, callback count matches,
 * dimensions 320x240, NV12 delivered. Anything else exits 1 and prints what
 * it saw. This is the sandbox stand-in for the T10 decode.synthetic check.
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
    int       dim_ok;
    int       nv12_ok;
    int       stride_ok;
} sdc_count;

static int sdc_cb(const uint8_t *y, int w, int h, int stride_y, int stride_uv,
                  int is_nv12, long long idx, void *ud)
{
    sdc_count *c = (sdc_count *)ud;
    (void)y;
    if (idx != c->frames) {
        printf("WARN: frame_index gap: got %lld expected %lld\n", idx,
               c->frames);
    }
    c->frames++;
    if (w == KN_SYNTH_CLIP_WIDTH && h == KN_SYNTH_CLIP_HEIGHT) c->dim_ok++;
    if (is_nv12) c->nv12_ok++;
    if (stride_y >= w && stride_uv >= w) c->stride_ok++;
    return 0;
}

int main(void)
{
    kn_decode_stats st;
    sdc_count cnt;
    int rc;

    memset(&cnt, 0, sizeof cnt);
    memset(&st, 0, sizeof st);
    printf("backend: %s (available=%d)\n", kn_decode_backend(),
           kn_decode_available());
    rc = kn_decode_h264_mem(k_synthetic_clip, KN_SYNTH_CLIP_LEN, 1,
                            sdc_cb, &cnt, &st);
    printf("rc=%d frames_decoded=%d (expect %d) frames_failed=%d "
           "cb_frames=%lld\n", rc, st.frames_decoded, KN_SYNTH_CLIP_FRAMES,
           st.frames_failed, cnt.frames);
    printf("dim_ok=%d nv12_ok=%d stride_ok=%d  %dx%d fps=%.1f "
           "cpu_pct_of_core=%.1f wall_ms=%lld decode_ms_max=%lld\n",
           cnt.dim_ok, cnt.nv12_ok, cnt.stride_ok, st.width, st.height,
           st.fps, st.cpu_pct_of_core, st.wall_ms, st.decode_ms_max);

    if (rc == 0 && st.frames_decoded == KN_SYNTH_CLIP_FRAMES &&
        st.frames_failed == 0 && cnt.frames == st.frames_decoded &&
        cnt.dim_ok == KN_SYNTH_CLIP_FRAMES &&
        cnt.nv12_ok == KN_SYNTH_CLIP_FRAMES &&
        cnt.stride_ok == KN_SYNTH_CLIP_FRAMES) {
        printf("SANDBOX DECODE CHECK: PASS (%d/%d frames)\n",
               (int)st.frames_decoded, KN_SYNTH_CLIP_FRAMES);
        return 0;
    }
    printf("SANDBOX DECODE CHECK: FAIL\n");
    return 1;
}
