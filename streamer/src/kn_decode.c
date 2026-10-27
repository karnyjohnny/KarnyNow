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

/* Send one packet (pkt == NULL enters decoder draining mode) and deliver
 * every frame the decoder hands back. Returns non-zero when the callback
 * asked to abort (k->abort is then already set), 0 otherwise. Shared by
 * the parse loop and BOTH flushes (parser tail + decoder drain) so all
 * three paths keep identical accounting. */
static int kd_send_drain(AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame,
                         kd_ctx *k, int to_nv12, kn_decode_stats *stats,
                         long long *worst)
{
    int ret = avcodec_send_packet(ctx, pkt);
    if (ret == AVERROR(EAGAIN)) {
        /* receiver full: drain one frame, retry the same packet */
        if (avcodec_receive_frame(ctx, frame) == 0) {
            long long t_in = kd_mono_ms();
            stats->frames_decoded++;
            if (kd_deliver(k, frame, to_nv12, stats, worst, t_in)) {
                av_frame_unref(frame);
                return 1;
            }
            av_frame_unref(frame);
        }
        ret = avcodec_send_packet(ctx, pkt);
    }
    if (ret < 0 && ret != AVERROR_EOF) stats->frames_failed++;
    while (!k->abort && (ret = avcodec_receive_frame(ctx, frame)) == 0) {
        long long t_in = kd_mono_ms();
        stats->frames_decoded++;
        if (kd_deliver(k, frame, to_nv12, stats, worst, t_in)) {
            av_frame_unref(frame);
            return 1;
        }
        av_frame_unref(frame);
    }
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        stats->frames_failed++;
    return k->abort;
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
    int rc = 0;

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
            pkt->data = pkt_data;
            pkt->size = pkt_size;
            if (kd_send_drain(ctx, pkt, frame, &k, to_nv12, stats, &worst))
                break;
        }
    }

    /* Flush the PARSER: av_parser_parse2 withholds an access unit until it
     * sees the NEXT start code — which never arrives at end-of-input, so
     * the final AU stays buffered unless we parse once more with NULL/0.
     * Skipping this silently loses exactly one frame at EOF with rc=0 and
     * frames_failed=0 (T10 round 4 on target AND sandbox repro on FFmpeg
     * 5.1/63 alike: 59 of 60 decoded). */
    if (!k.abort) {
        uint8_t *pkt_data = NULL;
        int pkt_size = 0;
        av_parser_parse2(parser, ctx, &pkt_data, &pkt_size, NULL, 0,
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (pkt_size > 0) {
            pkt->data = pkt_data;
            pkt->size = pkt_size;
            (void)kd_send_drain(ctx, pkt, frame, &k, to_nv12, stats, &worst);
        }
    }

    /* Flush the DECODER (draining mode): releases frames still held for
     * reorder/delay reasons after the last packet. Must come AFTER the
     * parser flush or the tail AU never reaches the decoder. */
    if (!k.abort)
        (void)kd_send_drain(ctx, NULL, frame, &k, to_nv12, stats, &worst);

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

/* ------------------------------------------- persistent stream session */

struct kn_decode_stream {
    AVCodecContext *ctx;
    AVFrame        *frame;
    AVPacket       *pkt;
    kd_ctx          k;          /* cb/sws/nv12 state (shared with kd_run) */
    uint8_t        *inbuf;      /* padded AU staging, grow-only */
    size_t          inbuf_cap;  /* usable bytes (excl. padding) */
    int             to_nv12;
    int             aborted;    /* callback asked to stop (sticky -4) */
    int             fatal;      /* allocation failure (sticky -3) */
    long long       t0, cpu0, worst;
    kn_decode_stats stats;      /* cumulative; wall/cpu filled on query */
};

static void kd_stream_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz) snprintf(err, errsz, "%s", msg);
}

kn_decode_stream *kn_decode_stream_open(int to_nv12, char *err, size_t errsz)
{
    kn_decode_stream *s;
    const AVCodec *codec;

    s = (kn_decode_stream *)calloc(1, sizeof *s);
    if (!s) { kd_stream_err(err, errsz, "calloc failed"); return NULL; }
    s->to_nv12 = to_nv12;
    av_log_set_callback(kd_av_log);
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { kd_stream_err(err, errsz, "no h264 decoder"); goto fail; }
    s->ctx = avcodec_alloc_context3(codec);
    if (!s->ctx) { kd_stream_err(err, errsz, "ctx alloc failed"); goto fail; }
    s->ctx->thread_count = 1;            /* D4: measured faster on 2C Merom */
    s->ctx->thread_type = 0;             /* no frame/slice threading */
    if (avcodec_open2(s->ctx, codec, NULL) < 0) {
        kd_stream_err(err, errsz, "avcodec_open2 failed");
        goto fail;
    }
    s->frame = av_frame_alloc();
    s->pkt   = av_packet_alloc();
    s->k.nv  = av_frame_alloc();
    if (!s->frame || !s->pkt || !s->k.nv) {
        kd_stream_err(err, errsz, "frame/pkt alloc failed");
        goto fail;
    }
    s->t0   = kd_mono_ms();
    s->cpu0 = kd_cpu_ms();
    return s;

fail:
    kn_decode_stream_close(s);
    return NULL;
}

/* Ensure inbuf holds len usable bytes + the FFmpeg padding. Grow-only. */
static int kd_stream_reserve(kn_decode_stream *s, size_t len)
{
    uint8_t *nb;
    if (len <= s->inbuf_cap) return 0;
    nb = (uint8_t *)av_realloc(s->inbuf, len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!nb) return -1;
    s->inbuf = nb;
    s->inbuf_cap = len;
    return 0;
}

int kn_decode_stream_push(kn_decode_stream *s, const uint8_t *au, size_t len,
                          uint32_t rtp_ts, kn_decode_frame_fn cb, void *ud)
{
    int before;

    if (!s || !au || len == 0) return -1;
    if (s->aborted) return -4;
    if (s->fatal) return -3;
    if (len > KN_DECODE_MAX_FILE) return -2;
    if (kd_stream_reserve(s, len) != 0) { s->fatal = 1; return -3; }

    memcpy(s->inbuf, au, len);
    memset(s->inbuf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    s->pkt->data = s->inbuf;
    s->pkt->size = (int)len;
    s->pkt->pts  = (int64_t)rtp_ts;      /* 90 kHz; nobody consumes pts yet */
    s->pkt->dts  = (int64_t)rtp_ts;
    s->k.cb = cb;
    s->k.ud = ud;

    before = s->stats.frames_decoded;
    if (kd_send_drain(s->ctx, s->pkt, s->frame, &s->k, s->to_nv12,
                      &s->stats, &s->worst)) {
        s->aborted = 1;
        return -4;
    }
    return s->stats.frames_decoded - before;
}

int kn_decode_stream_flush(kn_decode_stream *s, kn_decode_frame_fn cb, void *ud)
{
    int before;
    if (!s) return -1;
    if (s->aborted) return -4;
    if (s->fatal) return -3;
    s->k.cb = cb;
    s->k.ud = ud;
    before = s->stats.frames_decoded;
    if (kd_send_drain(s->ctx, NULL, s->frame, &s->k, s->to_nv12,
                      &s->stats, &s->worst)) {
        s->aborted = 1;
        return -4;
    }
    return s->stats.frames_decoded - before;
}

void kn_decode_stream_stats(const kn_decode_stream *s, kn_decode_stats *out)
{
    if (!out) return;
    if (!s) { memset(out, 0, sizeof *out); return; }
    *out = s->stats;
    out->wall_ms = kd_mono_ms() - s->t0;
    out->cpu_ms  = kd_cpu_ms() - s->cpu0;
    out->decode_ms_max = s->worst;
    if (out->wall_ms > 0) {
        out->fps = (double)out->frames_decoded * 1000.0 /
                   (double)out->wall_ms;
        out->cpu_pct_of_core = (double)out->cpu_ms * 100.0 /
                               (double)out->wall_ms;
    }
}

void kn_decode_stream_close(kn_decode_stream *s)
{
    if (!s) return;
    if (s->k.sws) sws_freeContext(s->k.sws);
    if (s->k.nv) av_frame_free(&s->k.nv);
    if (s->k.nvbuf) av_free(s->k.nvbuf);   /* LSan round XVII: the NV12
                                              conversion buffer belongs to
                                              kd_deliver's cache — kd_run
                                              frees it, the stream close
                                              must too */
    if (s->inbuf) av_free(s->inbuf);
    if (s->pkt) av_packet_free(&s->pkt);
    if (s->frame) av_frame_free(&s->frame);
    if (s->ctx) avcodec_free_context(&s->ctx);
    free(s);
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

/* stream session stubs — honest failures, same as the one-shot path */
struct kn_decode_stream { int unused; };

kn_decode_stream *kn_decode_stream_open(int to_nv12, char *err, size_t errsz)
{
    (void)to_nv12;
    if (err && errsz)
        snprintf(err, errsz, "built without FFmpeg (stub decode)");
    return NULL;
}

int kn_decode_stream_push(kn_decode_stream *s, const uint8_t *au, size_t len,
                          uint32_t rtp_ts, kn_decode_frame_fn cb, void *ud)
{
    (void)s; (void)au; (void)len; (void)rtp_ts; (void)cb; (void)ud;
    return -1;
}

int kn_decode_stream_flush(kn_decode_stream *s, kn_decode_frame_fn cb, void *ud)
{
    (void)s; (void)cb; (void)ud;
    return -1;
}

void kn_decode_stream_stats(const kn_decode_stream *s, kn_decode_stats *out)
{
    (void)s;
    if (out) memset(out, 0, sizeof *out);
}

void kn_decode_stream_close(kn_decode_stream *s) { (void)s; }

#endif /* KN_HAVE_FFMPEG */
