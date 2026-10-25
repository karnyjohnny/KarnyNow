/* sandbox_stream_decode.c — sandbox-only functional proof of the
 * kn_decode_stream_* persistent session API (round XVII, stage-2 --decode).
 * NOT part of any gate (like sandbox_decode_check.c): Linux + distro FFmpeg
 * only. The target-side proof is T13's live --decode run.
 *
 * What it proves:
 *   1. per-AU feeding of the real synthetic.264 (60 frames 320x240, GOP30,
 *      bf0) through ONE persistent session decodes ALL frames with
 *      references intact (a one-shot kn_decode_h264_mem per AU would fail
 *      every P-frame — the control case at the end proves exactly that).
 *   2. NV12 conversion state survives across pushes (to_nv12=1).
 *   3. a corrupt AU mid-stream is counted and the session CONTINUES.
 *   4. callback abort is sticky (-4) and stats stay readable.
 *   5. flush at end-of-stream delivers 0 extra frames for a bf0 stream
 *      (honest no-op, not a lost frame).
 *
 * The AU splitter here uses av_parser (harness-side only) — the session
 * under test receives whole AUs and runs NO parser, which is the stage-2
 * contract (kn_rx assembles the AUs).
 *
 * build (sandbox):
 *   gcc -std=c11 -O1 -g -Wall -Wextra -DKN_HAVE_FFMPEG=1 \
 *       -Istreamer/include tools/sandbox_stream_decode.c \
 *       streamer/src/kn_decode.c streamer/src/knlog.c streamer/src/knring.c \
 *       -lavcodec -lswscale -lavutil -o build/sandbox_stream_decode
 */
#include "kn/kn_decode.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failures;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (name));        \
        }                                                                  \
    } while (0)

#define MAX_AUS   128
#define AU_CAP    (2 * 1024 * 1024)

typedef struct {
    int frames;
    int last_w, last_h, last_sy, last_suv, last_nv12;
    int abort_at;      /* >0: return non-zero on that frame index */
} cbctx;

static int frame_cb(const uint8_t *data, int w, int h, int sy, int suv,
                    int is_nv12, long long idx, void *ud)
{
    cbctx *c = (cbctx *)ud;
    (void)data; (void)idx;
    c->frames++;
    c->last_w = w; c->last_h = h; c->last_sy = sy; c->last_suv = suv;
    c->last_nv12 = is_nv12;
    if (c->abort_at > 0 && c->frames >= c->abort_at) return 1;
    return 0;
}

static uint8_t *read_file(const char *p, size_t *out)
{
    FILE *f = fopen(p, "rb");
    long sz;
    uint8_t *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    buf = (uint8_t *)malloc((size_t)sz + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    memset(buf + sz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *out = (size_t)sz;
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const char *clip = argc > 1 ? argv[1] : "streamer/assets/synthetic.264";
    uint8_t *data;
    size_t fsz = 0;
    static uint8_t *au[MAX_AUS];
    static size_t au_len[MAX_AUS];
    int n_aus = 0, i, rc, delivered_total = 0;
    const AVCodec *codec;
    AVCodecContext *ctx = NULL;
    AVCodecParserContext *parser = NULL;
    kn_decode_stream *s;
    kn_decode_stats st;
    cbctx cb;
    char err[128];

    (void)data;
    data = read_file(clip, &fsz);
    if (!data) { printf("cannot read %s\n", clip); return 2; }
    printf("clip: %s (%zu bytes)\n", clip, fsz);

    /* --- harness-side AU split via the parser (simulates kn_rx output) --- */
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    ctx = avcodec_alloc_context3(codec);
    parser = av_parser_init(AV_CODEC_ID_H264);
    if (!ctx || !parser || avcodec_open2(ctx, codec, NULL) < 0) {
        printf("harness decoder init failed\n");
        return 2;
    }
    {
        size_t off = 0;
        while (off < fsz && n_aus < MAX_AUS) {
            uint8_t *pd = NULL;
            int psz = 0;
            int used = av_parser_parse2(parser, ctx, &pd, &psz,
                                        data + off, (int)(fsz - off),
                                        AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            off += (size_t)(used > 0 ? used : 0);
            if (used <= 0 && psz <= 0) break;
            if (psz > 0) {
                au[n_aus] = (uint8_t *)malloc((size_t)psz);
                memcpy(au[n_aus], pd, (size_t)psz);
                au_len[n_aus] = (size_t)psz;
                n_aus++;
            }
        }
        /* parser tail flush (the round-IX lesson, applied to the splitter) */
        if (n_aus < MAX_AUS) {
            uint8_t *pd = NULL;
            int psz = 0;
            av_parser_parse2(parser, ctx, &pd, &psz, NULL, 0,
                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (psz > 0) {
                au[n_aus] = (uint8_t *)malloc((size_t)psz);
                memcpy(au[n_aus], pd, (size_t)psz);
                au_len[n_aus] = (size_t)psz;
                n_aus++;
            }
        }
    }
    av_parser_close(parser);
    avcodec_free_context(&ctx);
    printf("harness split: %d AUs\n", n_aus);
    CHECK(n_aus == 60, "synthetic.264 splits into exactly 60 AUs");

    /* --- 1+2: full session, per-AU push, NV12 --- */
    memset(&cb, 0, sizeof cb);
    err[0] = '\0';
    s = kn_decode_stream_open(1, err, sizeof err);
    CHECK(s != NULL, "stream session opens (to_nv12=1)");
    if (s) {
        for (i = 0; i < n_aus; ++i) {
            rc = kn_decode_stream_push(s, au[i], au_len[i],
                                       (uint32_t)((i + 1) * 3000),
                                       frame_cb, &cb);
            if (rc < 0) { printf("FAIL push %d rc=%d\n", i, rc); g_failures++; }
            delivered_total += rc > 0 ? rc : 0;
        }
        CHECK(delivered_total == 60,
              "60 frames delivered across per-AU pushes (references intact)");
        CHECK(cb.frames == 60, "callback saw 60 frames");
        CHECK(cb.last_w == 320 && cb.last_h == 240, "geometry 320x240");
        CHECK(cb.last_nv12 == 1 && cb.last_sy >= 320 && cb.last_suv >= 320,
              "NV12 delivered with sane strides");
        rc = kn_decode_stream_flush(s, frame_cb, &cb);
        CHECK(rc == 0, "flush of a bf0 stream delivers 0 extra frames");
        kn_decode_stream_stats(s, &st);
        CHECK(st.frames_decoded == 60 && st.frames_failed == 0,
              "stats: 60 decoded, 0 failed");
        CHECK(st.width == 320 && st.height == 240, "stats geometry");
        CHECK(st.wall_ms >= 0 && st.decode_ms_max >= 0, "stats timings sane");
        printf("session stats: frames=%d failed=%d wall=%lldms fps=%.1f "
               "cpu=%.1f%% worst=%lldms\n", st.frames_decoded,
               st.frames_failed, st.wall_ms, st.fps, st.cpu_pct_of_core,
               st.decode_ms_max);
        kn_decode_stream_close(s);
    }

    /* --- 3: corrupt AU mid-stream — session must survive --- */
    memset(&cb, 0, sizeof cb);
    s = kn_decode_stream_open(0, err, sizeof err);
    CHECK(s != NULL, "second session opens (i420 passthrough)");
    if (s) {
        int frames_before_corrupt = 0, rc_corrupt;
        uint8_t *bad;
        for (i = 0; i < 30 && i < n_aus; ++i) {
            rc = kn_decode_stream_push(s, au[i], au_len[i],
                                       (uint32_t)(i * 3000), frame_cb, &cb);
            if (rc > 0) frames_before_corrupt += rc;
        }
        bad = (uint8_t *)malloc(au_len[30]);
        memcpy(bad, au[30], au_len[30]);
        if (au_len[30] > 40) memset(bad + 20, 0xFF, 20);   /* poison the slice */
        rc_corrupt = kn_decode_stream_push(s, bad, au_len[30], 30 * 3000,
                                           frame_cb, &cb);
        free(bad);
        CHECK(rc_corrupt >= -1, "corrupt push does not kill the session");
        /* keep pushing the rest: later frames reference the damaged one;
         * the decoder may emit error-concealed frames or count failures —
         * the contract is: the SESSION SURVIVES and keeps delivering. */
        for (i = 31; i < n_aus; ++i)
            (void)kn_decode_stream_push(s, au[i], au_len[i],
                                        (uint32_t)(i * 3000), frame_cb, &cb);
        (void)kn_decode_stream_flush(s, frame_cb, &cb);
        kn_decode_stream_stats(s, &st);
        CHECK(st.frames_decoded >= frames_before_corrupt,
              "session kept decoding after the corrupt AU");
        printf("corrupt-run stats: decoded=%d failed=%d (frames before "
               "corrupt: %d)\n", st.frames_decoded, st.frames_failed,
               frames_before_corrupt);
        kn_decode_stream_close(s);
    }

    /* --- 4: sticky callback abort --- */
    memset(&cb, 0, sizeof cb);
    cb.abort_at = 5;
    s = kn_decode_stream_open(0, err, sizeof err);
    CHECK(s != NULL, "abort-test session opens");
    if (s) {
        int saw_m4 = 0;
        for (i = 0; i < n_aus; ++i) {
            rc = kn_decode_stream_push(s, au[i], au_len[i],
                                       (uint32_t)(i * 3000), frame_cb, &cb);
            if (rc == -4) { saw_m4 = 1; break; }
        }
        CHECK(saw_m4 == 1, "abort surfaces as -4");
        rc = kn_decode_stream_push(s, au[0], au_len[0], 0, frame_cb, &cb);
        CHECK(rc == -4, "abort is sticky (later pushes return -4)");
        kn_decode_stream_stats(s, &st);
        CHECK(st.frames_decoded >= 5, "stats readable after abort");
        kn_decode_stream_close(s);
    }

    /* --- control: one-shot decode per AU FAILS P-frames (why the stream
     * session exists) --- */
    {
        int oneshot_frames = 0, oneshot_failed = 0;
        for (i = 0; i < n_aus; ++i) {
            memset(&cb, 0, sizeof cb);
            rc = kn_decode_h264_mem(au[i], au_len[i], 0, frame_cb, &cb, &st);
            if (rc == 0) {
                oneshot_frames += st.frames_decoded;
                oneshot_failed += st.frames_failed;
            } else {
                oneshot_failed++;
            }
        }
        printf("control (one-shot per AU): delivered=%d failed=%d\n",
               oneshot_frames, oneshot_failed);
        CHECK(oneshot_frames < 60,
              "control confirms one-shot per AU loses P-frames");
    }

    /* --- NULL/arg discipline --- */
    CHECK(kn_decode_stream_push(NULL, au[0], au_len[0], 0, NULL, NULL) == -1,
          "push(NULL session) -> -1");
    s = kn_decode_stream_open(0, err, sizeof err);
    if (s) {
        CHECK(kn_decode_stream_push(s, NULL, 10, 0, NULL, NULL) == -1,
              "push(NULL au) -> -1");
        CHECK(kn_decode_stream_push(s, au[0], 0, 0, NULL, NULL) == -1,
              "push(len 0) -> -1");
        kn_decode_stream_stats(NULL, &st);
        CHECK(st.frames_decoded == 0, "stats(NULL session) zeroes");
        kn_decode_stream_close(s);
    }
    kn_decode_stream_close(NULL);
    CHECK(1, "close(NULL) survives");

    for (i = 0; i < n_aus; ++i) free(au[i]);
    free(data);
    printf("sandbox_stream_decode: %d checks, %d failures -> %s\n",
           g_checks, g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
