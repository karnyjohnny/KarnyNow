/* kn_decode.c — H.264 decode subsystem. Contract in kn_decode.h (D3/D4/D6,
 * §6.1 measurements, §9 R2: the UpdateSurface upload path is DEAD — NV12
 * output here is for lockable DEFAULT-pool surfaces or GDI, never for
 * UpdateSurface).
 *
 * v1 scope (self-tests + stage-1 --loopback): decode a WHOLE bounded file
 * from memory (cap KN_DECODE_MAX_FILE). The live network path (stage 2)
 * feeds the decoder from the access-unit assembler instead and never
 * touches this file API.
 */
#ifndef _WIN32
/* POSIX clock/APIs under strict -std=c11 (see kn_diag.c for the rationale);
 * must precede every system include. */
#if !defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L)
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "kn/kn_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ real build */

#ifdef KN_HAVE_FFMPEG

/* ---------------------------------------------------------------- shims */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static long long kd_mono_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER pc;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&pc);
    if (!freq.QuadPart) return 0;
    return (long long)((double)pc.QuadPart * 1000.0 / (double)freq.QuadPart);
}
static long long kd_cpu_ms(void)
{
    FILETIME cr, ex, ke, us;
    ULARGE_INTEGER k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &ke, &us)) return 0;
    k.LowPart = ke.dwLowDateTime; k.HighPart = ke.dwHighDateTime;
    u.LowPart = us.dwLowDateTime; u.HighPart = us.dwHighDateTime;
    return (long long)((k.QuadPart + u.QuadPart) / 10000ULL);
}
#else
#  include <time.h>
static long long kd_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
static long long kd_cpu_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
#endif


#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#define KN_DECODE_MAX_FILE (64 * 1024 * 1024)   /* bounded input (R11) */

int kn_decode_available(void) { return 1; }

const char *kn_decode_backend(void)
{
    static char s[96];
    static int ready;
    if (!ready) {
        unsigned v = avcodec_version();
        snprintf(s, sizeof s, "libavcodec %u.%u.%u (threads=1)",
                 v >> 16, (v >> 8) & 0xFF, v & 0xFF);
        ready = 1;
    }
    return s;
}

/* FFmpeg logs -> knlog media channel, mapped DOWN to DEBUG (§7.7 synergy:
 * failure amplification raises media to DEBUG automatically, so FFmpeg's
 * own diagnostics become visible exactly when they matter, and a corrupt
 * frame on a live stream never fsync-storms the sinks at ERROR). */
#include "kn/knlog.h"
static void kd_av_log(void *avcl, int level, const char *fmt, va_list vl)
{
    char msg[512];
    kn_level kl;
    (void)avcl;
    if (!kn_log_is_initialized()) return;
    if (level <= AV_LOG_ERROR)        kl = KN_DEBUG;   /* counters carry WARN */
    else if (level <= AV_LOG_WARNING) kl = KN_DEBUG;
    else if (level <= AV_LOG_INFO)    kl = KN_DEBUG;
    else                              kl = KN_TRACE;
    if (!kn_log_would_log(KN_CH_MEDIA, kl)) return;
    vsnprintf(msg, sizeof msg, fmt, vl);
    msg[strcspn(msg, "\r\n")] = '\0';
    if (msg[0])
        kn_log_simple(KN_CH_MEDIA, kl, NULL, msg);
}

typedef struct {
    kn_decode_frame_fn cb;
    void              *ud;
    long long          idx;
    int                abort;
    /* nv12 conversion state */
    struct SwsContext *sws;
    int                sws_w, sws_h, sws_srcfmt;
    AVFrame           *nv;
    uint8_t           *nvbuf;
    int                nvbuf_size;
} kd_ctx;

/* Deliver one decoded frame (convert to NV12 when asked). Returns 0 to
 * continue, non-zero when the callback asked to abort. */
static int kd_deliver(kd_ctx *k, AVFrame *frame, int to_nv12,
                      kn_decode_stats *st, long long *worst_ms, long long t_in)
{
    const uint8_t *out_data;
    int w = frame->width, h = frame->height, sy, suv = 0, is_nv12 = 0;
    int rc = 0;
    long long dt;

    st->width = w;
    st->height = h;

    if (to_nv12) {
        int srcfmt = frame->format;
        int need;
        if (!k->sws || k->sws_w != w || k->sws_h != h || k->sws_srcfmt != srcfmt) {
            k->sws = sws_getCachedContext(k->sws, w, h, srcfmt,
                                          w, h, AV_PIX_FMT_NV12,
                                          SWS_POINT, NULL, NULL, NULL);
            k->sws_w = w; k->sws_h = h; k->sws_srcfmt = srcfmt;
        }
        if (!k->sws) { st->frames_failed++; goto done; }
        need = av_image_get_buffer_size(AV_PIX_FMT_NV12, w, h, 32);
        if (need <= 0) { st->frames_failed++; goto done; }
        if (need > k->nvbuf_size) {
            uint8_t *nb = (uint8_t *)av_realloc(k->nvbuf, (size_t)need + 64);
            if (!nb) { st->frames_failed++; goto done; }
            k->nvbuf = nb;
            k->nvbuf_size = need + 64;
            if (av_image_fill_arrays(k->nv->data, k->nv->linesize, k->nvbuf,
                                     AV_PIX_FMT_NV12, w, h, 32) < 0) {
                st->frames_failed++;
                goto done;
            }
        }
        if (sws_scale(k->sws, (const uint8_t *const *)frame->data,
                      frame->linesize, 0, h,
                      k->nv->data, k->nv->linesize) != h) {
            st->frames_failed++;
            goto done;
        }
        out_data = k->nv->data[0];
        sy = k->nv->linesize[0];
        suv = k->nv->linesize[1];
        is_nv12 = 1;
    } else {
        out_data = frame->data[0];
        sy = frame->linesize[0];
    }

    if (k->cb) {
        if (k->cb(out_data, w, h, sy, suv, is_nv12, k->idx, k->ud) != 0) {
            k->abort = 1;
            rc = 1;
        }
    }
    k->idx++;

done:
    dt = kd_mono_ms() - t_in;
    if (dt > *worst_ms) *worst_ms = dt;
    return rc;
}

/* Core decode loop over a padded Annex-B buffer. `data` must have
 * AV_INPUT_BUFFER_PADDING_SIZE zeroed bytes after `fsz` (bitstream readers
 * may over-read within the padding — documented FFmpeg requirement);
 * ownership stays with the caller. */
static int kd_run(const uint8_t *data, size_t fsz, int to_nv12,
                  kn_decode_frame_fn cb, void *userdata,
                  kn_decode_stats *stats)
{
    const AVCodec *codec;
    AVCodecContext *ctx = NULL;
    AVCodecParserContext *parser = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    kd_ctx k;
    long long t0, cpu0, worst = 0;
    size_t off = 0;
    int rc = 0, ret;

    if (!data || fsz == 0 || !stats) return -1;
    memset(stats, 0, sizeof *stats);
    memset(&k, 0, sizeof k);
    k.cb = cb;
    k.ud = userdata;

    av_log_set_callback(kd_av_log);

    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { rc = -3; goto out; }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) { rc = -3; goto out; }
    ctx->thread_count = 1;               /* D4: measured faster on 2C Merom */
    ctx->thread_type = 0;                /* no frame/slice threading */
    if (avcodec_open2(ctx, codec, NULL) < 0) { rc = -3; goto out; }
    parser = av_parser_init(AV_CODEC_ID_H264);
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    k.nv = av_frame_alloc();
    if (!parser || !frame || !pkt || !k.nv) { rc = -3; goto out; }

    t0 = kd_mono_ms();
    cpu0 = kd_cpu_ms();

    while (off < (size_t)fsz && !k.abort) {
        uint8_t *pkt_data = NULL;
        int pkt_size = 0;
        int used = av_parser_parse2(parser, ctx, &pkt_data, &pkt_size,
                                    data + off, (int)((size_t)fsz - off),
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        off += (size_t)(used > 0 ? used : 0);
        if (used <= 0 && pkt_size <= 0) break;         /* no progress: corrupt tail */
        if (pkt_size > 0) {
            long long t_in;
            pkt->data = pkt_data;
            pkt->size = pkt_size;
            ret = avcodec_send_packet(ctx, pkt);
            if (ret == AVERROR(EAGAIN)) {
                /* receiver full: drain one frame, retry the same packet */
                if (avcodec_receive_frame(ctx, frame) == 0) {
                    stats->frames_decoded++;
                    t_in = kd_mono_ms();
                    if (kd_deliver(&k, frame, to_nv12, stats, &worst, t_in)) {
                        av_frame_unref(frame);
                        break;
                    }
                    av_frame_unref(frame);
                }
                ret = avcodec_send_packet(ctx, pkt);
            }
            if (ret < 0 && ret != AVERROR_EOF) stats->frames_failed++;
            while (!k.abort && (ret = avcodec_receive_frame(ctx, frame)) == 0) {
                stats->frames_decoded++;
                t_in = kd_mono_ms();
                if (kd_deliver(&k, frame, to_nv12, stats, &worst, t_in)) {
                    av_frame_unref(frame);
                    break;
                }
                av_frame_unref(frame);
            }
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                stats->frames_failed++;
        }
    }

    /* flush the decoder (trailing frames without a following start code) */
    if (!k.abort) {
        avcodec_send_packet(ctx, NULL);
        while ((ret = avcodec_receive_frame(ctx, frame)) == 0) {
            long long t_in;
            stats->frames_decoded++;
            t_in = kd_mono_ms();
            if (kd_deliver(&k, frame, to_nv12, stats, &worst, t_in)) {
                av_frame_unref(frame);
                break;
            }
            av_frame_unref(frame);
        }
    }

    stats->wall_ms = kd_mono_ms() - t0;
    stats->cpu_ms = kd_cpu_ms() - cpu0;
    stats->decode_ms_max = worst;
    if (stats->wall_ms > 0) {
        stats->fps = (double)stats->frames_decoded * 1000.0 / (double)stats->wall_ms;
        stats->cpu_pct_of_core = (double)stats->cpu_ms * 100.0 / (double)stats->wall_ms;
    }
    rc = k.abort ? -4 : 0;

out:
    if (k.sws) sws_freeContext(k.sws);
    if (k.nv) av_frame_free(&k.nv);
    if (k.nvbuf) av_free(k.nvbuf);
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    if (parser) av_parser_close(parser);
    if (ctx) avcodec_free_context(&ctx);
    return rc;
}

int kn_decode_h264_mem(const uint8_t *src, size_t len, int to_nv12,
                       kn_decode_frame_fn cb, void *userdata,
                       kn_decode_stats *stats)
{
    uint8_t *data;
    int rc;
    if (!src || len == 0 || !stats) return -1;
    if (len > KN_DECODE_MAX_FILE) return -2;
    /* copy into a padded buffer: callers may pass const/readonly memory
     * (the embedded clip lives in .rodata) */
    data = (uint8_t *)av_malloc(len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!data) return -2;
    memcpy(data, src, len);
    memset(data + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    rc = kd_run(data, len, to_nv12, cb, userdata, stats);
    av_free(data);
    return rc;
}

int kn_decode_h264_file(const char *path, int to_nv12,
                        kn_decode_frame_fn cb, void *userdata,
                        kn_decode_stats *stats)
{
    FILE *f;
    long fsz;
    uint8_t *data;
    size_t rd;
    int rc;

    if (!path || !stats) return -1;
    f = fopen(path, "rb");
    if (!f) return -2;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -2; }
    fsz = ftell(f);
    rewind(f);
    if (fsz <= 0 || fsz > KN_DECODE_MAX_FILE) { fclose(f); return -2; }
    data = (uint8_t *)av_malloc((size_t)fsz + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!data) { fclose(f); return -2; }
    rd = fread(data, 1, (size_t)fsz, f);
    fclose(f);
    if (rd != (size_t)fsz) { av_free(data); return -5; }
    memset(data + fsz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    rc = kd_run(data, (size_t)fsz, to_nv12, cb, userdata, stats);
    av_free(data);
    return rc;
}

/* =========================================================== stub build */

#else /* !KN_HAVE_FFMPEG */

int kn_decode_available(void) { return 0; }

const char *kn_decode_backend(void)
{
    return "stub (built without FFmpeg; set -DKN_FFMPEG_PREFIX=<dev tree>)";
}

int kn_decode_h264_file(const char *path, int to_nv12,
                        kn_decode_frame_fn cb, void *userdata,
                        kn_decode_stats *stats)
{
    (void)path; (void)to_nv12; (void)cb; (void)userdata; (void)stats;
    return -1;
}

int kn_decode_h264_mem(const uint8_t *data, size_t len, int to_nv12,
                       kn_decode_frame_fn cb, void *userdata,
                       kn_decode_stats *stats)
{
    (void)data; (void)len; (void)to_nv12; (void)cb; (void)userdata; (void)stats;
    return -1;
}

#endif /* KN_HAVE_FFMPEG */
