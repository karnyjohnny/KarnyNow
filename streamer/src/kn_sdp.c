/* kn_sdp.c — offer parse + answer/nvstSdp build (B-track slice 5b).
 * See kn_sdp.h for the ground-truth references (runB3 capture +
 * OpenNOW 1cb3479 core_sdp.rs / transport_mod.rs). Pure string
 * transformers: no allocation, no I/O, deterministic output so the
 * golden tests pin every byte. */
#include "kn/kn_sdp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- appender */

typedef struct {
    char  *b;
    size_t cap, len;
    char   eol[3];        /* "\r\n" for the classic answer, "\n" nvst */
    int    err;           /* sticky KN_SDP_ERR_BIG */
} ab;

static void ab_init(ab *a, char *buf, size_t cap, const char *eol)
{
    a->b = buf; a->cap = cap; a->len = 0; a->err = 0;
    if (buf && cap) buf[0] = '\0';
    snprintf(a->eol, sizeof a->eol, "%s", eol ? eol : "\r\n");
}

static void ab_add(ab *a, const char *s, size_t n)
{
    if (a->err) return;
    if (a->len + n + 1 > a->cap) { a->err = KN_SDP_ERR_BIG; return; }
    memcpy(a->b + a->len, s, n);
    a->len += n;
    a->b[a->len] = '\0';
}

static void ab_linef(ab *a, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    if (a->err) return;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { a->err = KN_SDP_ERR_BIG; return; }
    ab_add(a, tmp, (size_t)n);
    ab_add(a, a->eol, strlen(a->eol));
}

/* --------------------------------------------------------- utilities */

static void cpv(char *dst, size_t dstsz, const char *src, size_t n)
{
    if (dstsz == 0) return;
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* length of the leading decimal run; 0 when it does not start a digit */
static int lead_int(const char *s, int *out)
{
    int v = 0, n = 0;
    while (s[n] >= '0' && s[n] <= '9') { v = v * 10 + (s[n] - '0'); ++n; }
    if (n == 0) return 0;
    *out = v;
    return n;
}

/* ------------------------------------------------------ offer parser */

/* One a=... attribute line (without the trailing CR/LF, without the
 * leading "a=") applied either to the session (sec == NULL) or to the
 * section. */
static void offer_attr(kn_sdp_offer *o, kn_sdp_media *sec,
                       const char *a, size_t n)
{
    if (!sec) {
        if (n >= 10 && memcmp(a, "ice-ufrag:", 10) == 0)
            cpv(o->ice_ufrag, sizeof o->ice_ufrag, a + 10, n - 10);
        else if (n >= 8 && memcmp(a, "ice-pwd:", 8) == 0)
            cpv(o->ice_pwd, sizeof o->ice_pwd, a + 8, n - 8);
        else if (n >= 21 && memcmp(a, "fingerprint:sha-256 ", 20) == 0)
            cpv(o->fingerprint, sizeof o->fingerprint, a + 20, n - 20);
        else if (n >= 6 && memcmp(a, "setup:", 6) == 0)
            cpv(o->setup, sizeof o->setup, a + 6, n - 6);
        else if (n == 8 && memcmp(a, "ice-lite", 8) == 0)
            o->ice_lite = 1;
        else if (n >= 12 && memcmp(a, "ice-options:", 12) == 0) {
            size_t k;   /* comma-separated list; plain token scan */
            for (k = 12; k + 7 <= n; ++k)
                if (memcmp(a + k, "trickle", 7) == 0) {
                    o->trickle = 1;
                    break;
                }
        }
        return;
    }
    if (n >= 4 && memcmp(a, "mid:", 4) == 0)
        cpv(sec->mid, sizeof sec->mid, a + 4, n - 4);
    else if ((n == 8 && memcmp(a, "sendonly", 8) == 0) ||
             (n == 8 && memcmp(a, "recvonly", 8) == 0) ||
             (n == 8 && memcmp(a, "sendrecv", 8) == 0) ||
             (n == 8 && memcmp(a, "inactive", 8) == 0))
        cpv(sec->dir, sizeof sec->dir, a, n);
    else if (n >= 5 && memcmp(a, "msid:", 5) == 0)
        cpv(sec->msid, sizeof sec->msid, a + 5, n - 5);
    else if (n >= 7 && memcmp(a, "extmap:", 7) == 0) {
        if (sec->n_extmap < KN_SDP_MAX_EXTMAP)
            cpv(sec->extmap[sec->n_extmap++],
                sizeof sec->extmap[0], a, n);
    } else if (n >= 8 && memcmp(a, "rtpmap:", 7) == 0) {
        if (sec->n_rtpmap < KN_SDP_MAX_RTPMAP)
            cpv(sec->rtpmap[sec->n_rtpmap++],
                sizeof sec->rtpmap[0], a, n);
    } else if (n >= 5 && memcmp(a, "fmtp:", 5) == 0) {
        if (sec->n_fmtp < KN_SDP_MAX_FMTP)
            cpv(sec->fmtp[sec->n_fmtp++], sizeof sec->fmtp[0], a, n);
    } else if (n >= 9 && memcmp(a, "rtcp-fb:", 8) == 0) {
        if (sec->n_rtcpfb < KN_SDP_MAX_RTCPFB)
            cpv(sec->rtcpfb[sec->n_rtcpfb++],
                sizeof sec->rtcpfb[0], a, n);
    } else if (n >= 6 && memcmp(a, "ptime:", 6) == 0)
        (void)lead_int(a + 6, &sec->ptime);
    else if (n >= 10 && memcmp(a, "sctp-port:", 10) == 0)
        (void)lead_int(a + 10, &sec->sctp_port);
    else if (n >= 17 && memcmp(a, "max-message-size:", 17) == 0)
        sec->max_message_size = strtoull(a + 17, NULL, 10);
}

/* m= line: kind port proto fmt... */
static int offer_mline(kn_sdp_offer *o, const char *l, size_t n)
{
    kn_sdp_media *sec;
    size_t i = 2;                    /* skip "m=" */
    size_t tok0;
    int port = 0;
    if (o->n_media >= KN_SDP_MAX_MEDIA) return KN_SDP_ERR_PARSE;
    sec = &o->m[o->n_media];
    tok0 = i;
    while (i < n && l[i] != ' ') ++i;
    cpv(sec->kind, sizeof sec->kind, l + tok0, i - tok0);
    if (i < n) ++i;
    tok0 = i;
    while (i < n && l[i] != ' ') ++i;
    {   /* the port can carry "port/nc" — the leading int is the port */
        const char *ps = l + tok0;
        while (ps < l + i && *ps >= '0' && *ps <= '9')
            port = port * 10 + (*ps++ - '0');
    }
    sec->port = port;
    if (i < n) ++i;
    tok0 = i;
    while (i < n && l[i] != ' ') ++i;
    cpv(sec->proto, sizeof sec->proto, l + tok0, i - tok0);
    if (i < n) ++i;
    cpv(sec->fmt, sizeof sec->fmt, l + i, n - i);
    ++o->n_media;
    return KN_SDP_OK;
}

/* After the raw lines are in: pick the codecs the answer will use. */
static void offer_pick_codecs(kn_sdp_media *sec)
{
    int i, pt;
    sec->h264_pt = 0; sec->h264_rtx_pt = 0; sec->opus_pt = 0;
    for (i = 0; i < sec->n_rtpmap; ++i) {
        const char *r = sec->rtpmap[i] + 7;      /* past "rtpmap:" */
        if (!lead_int(r, &pt)) continue;
        {
            const char *sp = strchr(r, ' ');
            if (!sp) continue;
            ++sp;
            if (!sec->h264_pt &&
                (strncmp(sp, "H264/", 5) == 0 || strncmp(sp, "h264/", 5) == 0))
                sec->h264_pt = pt;
            if (!sec->opus_pt && strncmp(sp, "opus/", 5) == 0)
                sec->opus_pt = pt;
        }
    }
    if (sec->h264_pt) {
        char apt[24];
        int n = snprintf(apt, sizeof apt, "apt=%d", sec->h264_pt);
        size_t al = (n > 0) ? (size_t)n : 0;
        if (al) {
            for (i = 0; i < sec->n_fmtp; ++i) {
                const char *f = sec->fmtp[i] + 5;    /* past "fmtp:" */
                const char *sp = strchr(f, ' ');
                const char *q;
                int fpt;
                if (!lead_int(f, &fpt) || !sp) continue;
                /* scan the ';'-separated parameters for an exact
                 * apt=<h264_pt> token (no prefix matches) */
                for (q = sp + 1; q && *q; ) {
                    if (strncmp(q, apt, al) == 0 &&
                        (q[al] == ';' || q[al] == '\0')) {
                        sec->h264_rtx_pt = fpt;
                        break;
                    }
                    q = strchr(q, ';');
                    if (q) ++q;
                }
                if (sec->h264_rtx_pt) break;
            }
        }
    }
}

int kn_sdp_offer_parse(const char *sdp, kn_sdp_offer *out)
{
    const char *p;
    int have_v = 0;
    if (!sdp || !out) return KN_SDP_ERR_ARGS;
    memset(out, 0, sizeof *out);
    p = sdp;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n;
        if (!e) n = strlen(p);
        else    n = (size_t)(e - p);
        if (n && p[n - 1] == '\r') --n;
        if (n >= 2 && p[0] == 'v' && p[1] == '=') have_v = 1;
        else if (n >= 2 && p[0] == 'm' && p[1] == '=') {
            if (offer_mline(out, p, n) != KN_SDP_OK)
                return KN_SDP_ERR_PARSE;
        } else if (n >= 2 && p[0] == 'a' && p[1] == '=') {
            kn_sdp_media *sec =
                out->n_media ? &out->m[out->n_media - 1] : NULL;
            offer_attr(out, sec, p + 2, n - 2);
        }
        if (!e) break;
        p = e + 1;
    }
    if (!have_v || out->n_media == 0) return KN_SDP_ERR_PARSE;
    {
        int i;
        for (i = 0; i < out->n_media; ++i) offer_pick_codecs(&out->m[i]);
    }
    return KN_SDP_OK;
}

/* ----------------------------------------------------- answer builder */

static void emit_verbatim_pts(ab *a, const char *base, size_t stride,
                              int n, int pt, const char *prefix_skip)
{
    /* copy every stored attribute line whose payload id is pt. The
     * stored form is "rtpmap:96 H264/90000" (no "a="); the stored rows
     * are fixed-width, hence base+stride instead of a char* array. */
    int i;
    size_t skip = strlen(prefix_skip);
    for (i = 0; i < n; ++i) {
        const char *l = base + (size_t)i * stride;
        int v;
        if (strncmp(l, prefix_skip, skip) != 0) continue;
        if (!lead_int(l + skip, &v) || v != pt) continue;
        ab_add(a, "a=", 2);
        ab_add(a, l, strlen(l));
        ab_add(a, a->eol, strlen(a->eol));
    }
}

int kn_sdp_answer_build(const kn_sdp_offer *off,
                        const kn_sdp_answer_cfg *cfg,
                        char *out, size_t outsz)
{
    ab a;
    int i;
    const char *setup = "active";
    if (!off || !cfg || !out || !outsz) return KN_SDP_ERR_ARGS;
    if (!cfg->ufrag || !cfg->ufrag[0] || !cfg->pwd || !cfg->pwd[0] ||
        !cfg->fingerprint || !cfg->fingerprint[0] ||
        !cfg->local_ip || !cfg->local_ip[0] || cfg->local_port == 0 ||
        cfg->local_port > 65535u)
        return KN_SDP_ERR_ARGS;
    if (off->setup[0] && strcmp(off->setup, "actpass") != 0 &&
        strcmp(off->setup, "active") == 0)
        setup = "passive";               /* offerer is the DTLS client */
    ab_init(&a, out, outsz, "\r\n");
    ab_linef(&a, "v=0");
    ab_linef(&a, "o=- %lu 2 IN IP4 127.0.0.1", cfg->sess_id);
    ab_linef(&a, "s=-");
    ab_linef(&a, "t=0 0");
    {   /* BUNDLE mirrors the offer's section order, rejected included */
        char grp[256];
        size_t gl = 0;
        grp[0] = '\0';
        for (i = 0; i < off->n_media; ++i) {
            const char *mid = off->m[i].mid[0] ? off->m[i].mid : "?";
            int n = snprintf(grp + gl, sizeof grp - gl, "%s%s",
                             gl ? " " : "", mid);
            if (n < 0 || gl + (size_t)n >= sizeof grp)
                return KN_SDP_ERR_BIG;
            gl += (size_t)n;
        }
        ab_linef(&a, "a=group:BUNDLE %s", grp);
    }
    ab_linef(&a, "a=ice-ufrag:%s", cfg->ufrag);
    ab_linef(&a, "a=ice-pwd:%s", cfg->pwd);
    ab_linef(&a, "a=fingerprint:sha-256 %s", cfg->fingerprint);
    ab_linef(&a, "a=setup:%s", setup);

    for (i = 0; i < off->n_media; ++i) {
        const kn_sdp_media *s = &off->m[i];
        int serving = 0;
        const char *want_dir = (s->dir[0] == '\0' ||
                                strcmp(s->dir, "sendonly") == 0 ||
                                strcmp(s->dir, "sendrecv") == 0)
                                   ? "recvonly" : NULL;
        /* what we can serve: video with H264, audio with opus, the
         * datachannel always; everything else is rejected (port 0 +
         * inactive + the offered formats kept — str0m/OpenNOW parity) */
        if (strcmp(s->kind, "video") == 0)
            serving = s->h264_pt > 0 && want_dir != NULL;
        else if (strcmp(s->kind, "audio") == 0)
            serving = s->opus_pt > 0 && want_dir != NULL;
        else if (strcmp(s->kind, "application") == 0)
            serving = 1;
        if (!serving) {
            ab_linef(&a, "m=%s 0 %s %s", s->kind, s->proto, s->fmt);
            ab_linef(&a, "c=IN IP4 0.0.0.0");
            ab_linef(&a, "a=mid:%s", s->mid);
            ab_linef(&a, "a=inactive");
            continue;
        }
        if (strcmp(s->kind, "application") == 0) {
            ab_linef(&a, "m=application %u %s %s",
                     cfg->local_port, s->proto, s->fmt);
            ab_linef(&a, "c=IN IP4 0.0.0.0");
            ab_linef(&a, "a=mid:%s", s->mid);
            if (s->sctp_port)
                ab_linef(&a, "a=sctp-port:%d", s->sctp_port);
            if (s->max_message_size)
                ab_linef(&a, "a=max-message-size:%llu",
                         s->max_message_size);
            continue;
        }
        {   /* audio or video: the chosen codec (+ RTX for video) */
            int pt  = (s->kind[0] == 'v') ? s->h264_pt : s->opus_pt;
            int rtx = (s->kind[0] == 'v') ? s->h264_rtx_pt : 0;
            if (rtx)
                ab_linef(&a, "m=%s %u %s %d %d", s->kind, cfg->local_port,
                         s->proto, pt, rtx);
            else
                ab_linef(&a, "m=%s %u %s %d", s->kind, cfg->local_port,
                         s->proto, pt);
            ab_linef(&a, "c=IN IP4 0.0.0.0");
            ab_linef(&a, "a=rtcp:%u IN IP4 0.0.0.0", cfg->local_port);
            ab_linef(&a, "a=mid:%s", s->mid);
            {
                int e;
                for (e = 0; e < s->n_extmap; ++e)
                    ab_linef(&a, "a=%s", s->extmap[e]);
            }
            ab_linef(&a, "a=%s", want_dir);
            ab_linef(&a, "a=rtcp-mux");
            ab_linef(&a, "a=rtcp-rsize");
            emit_verbatim_pts(&a, (const char *)s->rtpmap, sizeof s->rtpmap[0],
                              s->n_rtpmap, pt, "rtpmap:");
            emit_verbatim_pts(&a, (const char *)s->rtcpfb, sizeof s->rtcpfb[0],
                              s->n_rtcpfb, pt, "rtcp-fb:");
            emit_verbatim_pts(&a, (const char *)s->fmtp, sizeof s->fmtp[0],
                              s->n_fmtp, pt, "fmtp:");
            if (rtx) {
                emit_verbatim_pts(&a, (const char *)s->rtpmap,
                                  sizeof s->rtpmap[0], s->n_rtpmap, rtx,
                                  "rtpmap:");
                emit_verbatim_pts(&a, (const char *)s->fmtp,
                                  sizeof s->fmtp[0], s->n_fmtp, rtx,
                                  "fmtp:");
            }
            if (s->kind[0] == 'a' && s->ptime)
                ab_linef(&a, "a=ptime:%d", s->ptime);
        }
    }
    if (a.err) return a.err;
    return KN_SDP_OK;
}

/* ------------------------------------------------------ nvstSdp build */

/* OpenNOW 1cb3479 webrtc/video_attributes.sdp — verbatim. */
static const char *const kn_sdp_video_attrs[] = {
    "m=video 0 RTP/AVP",
    "a=msid:fbc-video-0",
    "a=vqos.fec.rateDropWindow:10",
    "a=vqos.fec.minRequiredFecPackets:2",
    "a=vqos.fec.repairMinPercent:5",
    "a=vqos.fec.repairPercent:5",
    "a=vqos.fec.repairMaxPercent:35",
    "a=vqos.bllFec.enable:0",
    "a=vqos.dynamicStreamingMode:0",
    "a=vqos.drc.enable:0",
    "a=vqos.dfc.enable:0",
    "a=vqos.dfc.adjustResAndFps:0",
    "a=vqos.calculateAvgVideoStreamingBitrate:1",
    "a=video.dx9EnableNv12:1",
    "a=video.dx9EnableHdr:0",
    "a=vqos.qpg.enable:1",
    "a=vqos.resControl.qp.qpg.featureSetting:7",
    "a=video.adaptiveQuantization.spatialAQSetting:7",
    "a=video.adaptiveQuantization.temporalAQSetting:0",
    "a=video.adaptiveQuantization.spatialAQStrength:12",
    "a=video.adaptiveQuantization.qpThresholdAdjPercent:2",
    "a=video.adaptiveQuantization.saqAdaptMinQpThresholdPercent:40",
    "a=video.adaptiveQuantization.saqAdaptMaxQpThresholdPercent:100",
    "a=video.adaptiveQuantization.saqAdaptDecayStrengthX100:250",
    "a=video.adaptiveQuantization.perfAdjEnablement:1",
    "a=video.framePacing.mode:2",
    "a=bwe.useOwdCongestionControl:1",
    "a=video.enableRtpNack:1",
    "a=vqos.bw.txRxLag.minFeedbackTxDeltaMs:200",
    "a=vqos.drc.bitrateIirFilterFactor:18",
    "a=video.packetSize:1140",
    "a=packetPacing.version:3",
    "a=packetPacing.mode:1",
    "a=packetPacing.minNumPacketsPerGroup:15",
    "a=packetPacing.enableAccurateSleep:1",
    "a=packetPacing.enableSmoothTransition:1",
    "a=packetPacing.allowFpsBasedToggle:1",
    "a=vqos.relaxMaxBitrate.overrideAvgBitrateThresholdPercent:4",
    "a=vqos.relaxMaxBitrate.customAvgBitrateThresholdPercent:65",
    "a=vqos.relaxMaxBitrate.overrideAvgQpThresholdPercent:7",
    "a=vqos.relaxMaxBitrate.customAvgQpThresholdPercent:51",
    "a=vqos.relaxMaxBitrate.iirFilterFactor:120",
    "a=vqos.qpDelta.qpDeltaMaxPercent:10",
    "a=vqos.qpDelta.qpDeltaSurfaceAdjustmentStrengthPercent:70",
    "a=vqos.qpDelta.qpDeltaVbvUsageFactorPercentH264:100",
    "a=vqos.qpDelta.qpDeltaVbvUsageFactorPercentH265:100",
    "a=vqos.qpDelta.qpDeltaVbvUsageFactorPercentAv1:100",
    "a=vqos.qpDelta.qpDeltaMinPercent:60",
    "a=vqos.qpDelta.qpDeltaIirFactor:60",
    "a=vqos.qpDelta.qpDeltaThrottlePercent:100",
    "a=vqos.adjustStreamingFpsDuringOutOfFocus:0",
    "a=vqos.resControl.cpmRtc.ignoreOutOfFocusWindowState:1",
    "a=vqos.resControl.perfHistory.rtcIgnoreOutOfFocusWindowState:1",
    "a=vqos.resControl.cpmRtc.featureMask:0",
    "a=vqos.resControl.cpmRtc.enable:0",
    "a=vqos.resControl.cpmRtc.minResolutionPercent:100",
    "a=vqos.resControl.cpmRtc.resolutionChangeHoldonMs:999999",
    "a=packetPacing.maxDelayUs:1000",
    "a=packetPacing.minNumPacketsFrame:10",
    "a=video.rtpNackQueueLength:1024",
    "a=video.rtpNackQueueMaxPackets:512",
    "a=video.rtpNackMaxPacketCount:25",
    "a=vqos.drc.qpMaxResThresholdAdj:4",
    "a=vqos.grc.qpMaxResThresholdAdj:4",
    "a=vqos.drc.iirFilterFactor:100",
    "a=vqos.bw.enableBandwidthEstimation:1",
    "a=vqos.bw.disableBitrateLimit:0",
    "a=vqos.grc.enable:0",
    "a=video.maxNumReferenceFrames:4",
    "a=video.mapRtpTimestampsToFrames:1",
    "a=video.encoderCscMode:3",
    "a=video.dynamicRangeMode:0",
    "a=video.scalingFeature1:0",
    "a=video.prefilterParams.prefilterModel:0",
};

int kn_sdp_nvst_build(const kn_sdp_answer_cfg *cfg, char *out, size_t outsz)
{
    ab a;
    unsigned max_k, min_k, init_k, frame_time;
    size_t i;
    if (!cfg || !out || !outsz) return KN_SDP_ERR_ARGS;
    if (!cfg->ufrag || !cfg->ufrag[0] || !cfg->pwd || !cfg->pwd[0] ||
        !cfg->fingerprint || !cfg->fingerprint[0])
        return KN_SDP_ERR_ARGS;
    /* core_sdp.rs: maximum = (bps/1000).clamp(1000,200000);
     * minimum = maximum.min(5000); initial = minimum.max(maximum/4) */
    max_k = cfg->bitrate_kbps;
    if (max_k < 1000u)   max_k = 1000u;
    if (max_k > 200000u) max_k = 200000u;
    min_k = max_k < 5000u ? max_k : 5000u;
    init_k = max_k / 4u;
    if (init_k < min_k) init_k = min_k;
    frame_time = cfg->fps ? 950000u / cfg->fps : 950000u;
    if (frame_time < 1000u) frame_time = 1000u;

    ab_init(&a, out, outsz, "\n");
    ab_linef(&a, "v=0");
    ab_linef(&a, "o=SdpTest test_id_13 14 IN IPv4 127.0.0.1");
    ab_linef(&a, "s=-");
    ab_linef(&a, "t=0 0");
    ab_linef(&a, "a=general.icePassword:%s", cfg->pwd);
    ab_linef(&a, "a=general.iceUserNameFragment:%s", cfg->ufrag);
    ab_linef(&a, "a=general.dtlsFingerprint:%s", cfg->fingerprint);
    for (i = 0; i < sizeof kn_sdp_video_attrs / sizeof kn_sdp_video_attrs[0];
         ++i)
        ab_linef(&a, "%s", kn_sdp_video_attrs[i]);
    ab_linef(&a, "a=video.framePacing.pid.minTargetFrameTimeUs:%u",
             frame_time);
    if (cfg->fps > 60u) {
        ab_linef(&a, "a=vqos.resControl.dfc.useClientFpsPerf:0");
        ab_linef(&a, "a=bwe.iirFilterFactor:8");
        ab_linef(&a, "a=video.encoderFeatureSetting:47");
        ab_linef(&a, "a=video.encoderPreset:6");
        ab_linef(&a, "a=vqos.maxStreamFpsEstimate:%u", cfg->fps);
        if (cfg->fps == 90u || cfg->fps == 120u || cfg->fps >= 240u) {
            unsigned grab = 0, decode = 0;
            if (cfg->fps == 90u)       { grab = 9;  decode = 11; }
            else if (cfg->fps == 120u) { grab = 6;  decode = 9;  }
            else                       { grab = 18; decode = 9;  }
            ab_linef(&a, "a=video.fbcDynamicFpsGrabTimeoutMs:%u", grab);
            ab_linef(&a,
                     "a=vqos.resControl.cpmRtc.decodeTimeThresholdMs:%u",
                     decode);
        }
    }
    if (cfg->fps >= 240u) {
        ab_linef(&a, "a=video.enableNextCaptureMode:1");
        ab_linef(&a, "a=video.videoSplitEncodeStripsPerFrame:3");
        ab_linef(&a, "a=video.updateSplitEncodeStateDynamically:1");
        ab_linef(&a, "a=vqos.rtcPreemptiveIdrSettings.minBurstNackSize:65535");
        ab_linef(&a,
                 "a=vqos.rtcPreemptiveIdrSettings.minNackPacketCaptureAgeMs:"
                 "65535");
    }
    ab_linef(&a, "a=packetPacing.numGroups:%d", cfg->fps == 120u ? 3 : 5);
    ab_linef(&a, "a=video.clientViewportWd:%u", cfg->width);
    ab_linef(&a, "a=video.clientViewportHt:%u", cfg->height);
    ab_linef(&a, "a=video.maxFPS:%u", cfg->fps);
    ab_linef(&a, "a=video.bitDepth:8");
    ab_linef(&a, "a=video.initialBitrateKbps:%u", init_k);
    ab_linef(&a, "a=video.initialPeakBitrateKbps:%u", init_k);
    ab_linef(&a, "a=vqos.bw.maximumBitrateKbps:%u", max_k);
    ab_linef(&a, "a=vqos.bw.minimumBitrateKbps:%u", min_k);
    ab_linef(&a, "a=vqos.bw.peakBitrateKbps:%u", max_k);
    ab_linef(&a, "a=vqos.bw.serverPeakBitrateKbps:%u", max_k);
    ab_linef(&a, "a=vqos.grc.maximumBitrateKbps:%u", max_k);
    ab_linef(&a, "m=audio 0 RTP/AVP");
    ab_linef(&a, "a=msid:audio");
    ab_linef(&a, "m=application 0 RTP/AVP");
    ab_linef(&a, "a=msid:input_1");
    /* OpenNOW defaults — the classic offer carries no ri.* lines, so
     * these ARE the production values (core_sdp.rs fallbacks). */
    ab_linef(&a, "a=ri.partialReliableThresholdMs:30");
    ab_linef(&a, "a=ri.hidDeviceMask:4294967295");
    ab_linef(&a, "a=ri.enablePartiallyReliableTransferGamepad:15");
    ab_linef(&a, "a=ri.enablePartiallyReliableTransferHid:4294967295");
    ab_add(&a, "", 0);                   /* the joined "" trailing line */
    if (a.err) return a.err;
    return KN_SDP_OK;
}

/* -------------------------------------------------- host candidate */

int kn_sdp_candidate_host(const char *ip, unsigned port,
                          char *out, size_t outsz)
{
    int n;
    if (!ip || !ip[0] || !out || !outsz || port == 0 || port > 65535u)
        return KN_SDP_ERR_ARGS;
    n = snprintf(out, outsz, "candidate:1 1 udp 2122260223 %s %u typ host",
                 ip, port);
    if (n < 0 || (size_t)n >= outsz) return KN_SDP_ERR_BIG;
    return KN_SDP_OK;
}

/* ------------------------------------------------- candidate parse (5d) */

/* Dotted-quad to a network-order u32 without pulling in kn_sock (this
 * module stays dependency-free): 1 on success. */
static int sdp_ipv4_be(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int i, oct = 0, dig = 0;
    if (!s || !out) return 0;
    for (i = 0; i < 4; ++i) {
        oct = 0; dig = 0;
        while (*s >= '0' && *s <= '9' && dig < 3) {
            oct = oct * 10 + (*s - '0');
            ++s; ++dig;
        }
        if (!dig || oct > 255) return 0;
        v = (v << 8) | (uint32_t)oct;
        if (i < 3) {
            if (*s != '.') return 0;
            ++s;
        }
    }
    if (*s) return 0;                      /* trailing garbage */
    *out = v;
    return 1;
}

/* strtoull with an exact-shape check: the token must be all digits and
 * non-empty (no sign, no whitespace, no locale surprises). */
static int sdp_num(const char *tok, unsigned long long *out)
{
    unsigned long long v = 0;
    const char *p = tok;
    if (!p || !*p) return 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10ull + (unsigned long long)(*p - '0');
    }
    *out = v;
    return 1;
}

int kn_sdp_candidate_parse(const char *line, kn_sdp_candidate *out)
{
    char buf[512];
    char *tok, *save = NULL;
    unsigned long long v;
    size_t n;
    if (!line || !out) return KN_SDP_ERR_ARGS;
    memset(out, 0, sizeof *out);
    {   /* skip leading blanks + an optional "a=" SDP prefix */
        const char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (p[0] == 'a' && p[1] == '=') p += 2;
        if (strncmp(p, "candidate:", 10) != 0) return KN_SDP_ERR_PARSE;
        p += 10;
        n = strlen(p);
        if (n == 0 || n >= sizeof buf) return KN_SDP_ERR_PARSE;
        memcpy(buf, p, n + 1);
        /* the foundation rides on the same token as the prefix */
        tok = buf;
        while (*tok && *tok != ' ' && *tok != '\t' && *tok != '\r' &&
               *tok != '\n')
            ++tok;
        if (*tok) { *tok++ = '\0'; save = tok; }
        else save = NULL;
        if (!buf[0]) return KN_SDP_ERR_PARSE;
        n = strlen(buf);
        if (n >= sizeof out->foundation) return KN_SDP_ERR_PARSE;
        memcpy(out->foundation, buf, n + 1);   /* RFC 8445: <= 32 chars */
    }
    {   /* whitespace-split the remainder; strtok-style, in place */
        char *rest = save;
        char *t[16];
        int nt = 0, i;
        const char *ws = " \t\r\n";
        while (rest && nt < 16) {
            while (*rest && strchr(ws, *rest)) ++rest;
            if (!*rest) break;
            t[nt++] = rest;
            while (*rest && !strchr(ws, *rest)) ++rest;
            if (*rest) *rest++ = '\0';
        }
        /* mandatory: component transport priority ip port typ type */
        if (nt < 7) return KN_SDP_ERR_PARSE;
        if (!sdp_num(t[0], &v) || v == 0 || v > 256ull)
            return KN_SDP_ERR_PARSE;   /* RFC 8445: 1..256 (Chrome's
                                        * tcp candidates ride 9) */
        out->component = (unsigned)v;
        if (strlen(t[1]) >= sizeof out->transport) return KN_SDP_ERR_PARSE;
        memcpy(out->transport, t[1], strlen(t[1]) + 1);
        if (!sdp_num(t[2], &v)) return KN_SDP_ERR_PARSE;
        out->priority = v;
        snprintf(out->ip, sizeof out->ip, "%.63s", t[3]);
        if (!sdp_num(t[4], &v) || v == 0 || v > 65535ull)
            return KN_SDP_ERR_PARSE;
        out->port = (unsigned)v;
        if (strcmp(t[5], "typ") != 0) return KN_SDP_ERR_PARSE;
        if (strlen(t[6]) >= sizeof out->typ || !t[6][0])
            return KN_SDP_ERR_PARSE;
        memcpy(out->typ, t[6], strlen(t[6]) + 1);
        (void)sdp_ipv4_be(out->ip, &out->ip_be);   /* 0 on non-v4: ok */
        /* tolerated extensions: raddr/rport keep their verbatim token
         * (the runB2a seat literally sends "raddr invalid rport 0");
         * everything else (tcptype/generation/network-id/ufrag...) is
         * ignored. */
        for (i = 7; i + 1 < nt; ++i) {
            if (strcmp(t[i], "raddr") == 0) {
                snprintf(out->raddr, sizeof out->raddr, "%.63s",
                         t[i + 1]);
                ++i;
            } else if (strcmp(t[i], "rport") == 0) {
                unsigned long long rp = 0;
                if (sdp_num(t[i + 1], &rp) && rp <= 65535ull)
                    out->rport = (unsigned)rp;
                ++i;
            }
        }
    }
    return KN_SDP_OK;
}
