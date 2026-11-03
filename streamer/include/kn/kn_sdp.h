/* kn_sdp.h — WebRTC offer parse + answer/nvstSdp build (B-track slice 5b).
 *
 * Ground truth (docs/RTSP.md §65):
 *  - the runB3 capture (karnynow-logs, 2026-11-02): the server's
 *    {"type":"offer"} inner message carries THREE fields — "sdp" (the
 *    classic WebRTC SDP, 3122 B in runB3: ice-lite, setup:actpass,
 *    4 bundled m-sections 0=audio sendonly RED+opus / 1=video sendonly
 *    H264+RTX+AV1+H265+FlexFEC / 2=application webrtc-datachannel /
 *    3=audio recvonly mic), "nvstSdp" (the ~40 KB server configuration
 *    document (a=general.x, a=video.x, a=vqos.x lines) — observed, NOT
 *    consumed
 *    here) and "nvstServerOverrides" (25 "key: value" lines).
 *  - OpenNOW 1cb3479 (opennow_webrtc_ref/core_sdp.rs nvst_answer(),
 *    transport_mod.rs negotiate()): the client answers with the OFFER's
 *    payload types (a codec subset), recvonly for the server-sent media,
 *    a rejected (port-0, inactive) mic section, one bundled UDP port,
 *    setup:active (the offer is actpass and ice-lite => we are the DTLS
 *    client and the ICE controlling agent), plus a second "nvstSdp"
 *    string built from the local identity + the video-attribute table +
 *    the bitrate ladder + the ri.* input-reliability defaults (30 /
 *    0xFFFFFFFF / 15 / hid). OpenNOW reads ri.* from the CLASSIC offer
 *    only (which never carries them) => the defaults are what actually
 *    flies in production; we mirror that exactly.
 *
 * Pure string transformers: no allocation, no I/O, no globals — every
 * output goes into a caller buffer, every failure is a return code.
 * The classic answer uses CRLF line breaks (RFC 4566); the nvstSdp uses
 * LF (OpenNOW joins with "\n" — the server accepts both, the reference
 * client sends LF, so we send LF).
 */
#ifndef KN_SDP_H
#define KN_SDP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_SDP_MAX_MEDIA   8      /* the runB3 offer carries 4 */
#define KN_SDP_MAX_EXTMAP  6
#define KN_SDP_MAX_RTPMAP  12
#define KN_SDP_MAX_FMTP    12
#define KN_SDP_MAX_RTCPFB  24

/* Return codes (all functions). */
#define KN_SDP_OK          0
#define KN_SDP_ERR_ARGS   -1      /* NULL/0-size argument */
#define KN_SDP_ERR_PARSE  -2      /* not a usable SDP offer */
#define KN_SDP_ERR_BIG    -3      /* output buffer too small */
#define KN_SDP_ERR_NOCODEC -4     /* no negotiable video/audio codec */

typedef struct kn_sdp_media {
    char kind[16];                /* "audio" | "video" | "application" | ... */
    int  port;                    /* m-line port (0 = offerer rejected) */
    char proto[48];               /* "UDP/TLS/RTP/SAVPF", "UDP/DTLS/SCTP" */
    char fmt[256];                /* the m-line format list, verbatim */
    char mid[16];
    char dir[16];                 /* sendonly/recvonly/sendrecv/inactive/"" */
    char msid[96];
    int  ptime;                   /* 0 = absent */
    int  sctp_port;               /* 0 = absent */
    unsigned long long max_message_size;   /* 0 = absent */
    int  n_extmap, n_rtpmap, n_fmtp, n_rtcpfb;
    char extmap[KN_SDP_MAX_EXTMAP][128];   /* attribute lines verbatim, */
    char rtpmap[KN_SDP_MAX_RTPMAP][96];    /* without the leading "a=" */
    char fmtp[KN_SDP_MAX_FMTP][160];
    char rtcpfb[KN_SDP_MAX_RTCPFB][80];
    /* codec picks the answer builder uses (filled by the parser): */
    int  h264_pt, h264_rtx_pt;    /* first offered H264 + its RTX (0/-) */
    int  opus_pt;                 /* first offered opus payload */
} kn_sdp_media;

typedef struct kn_sdp_offer {
    char ice_ufrag[64];
    char ice_pwd[128];
    char fingerprint[128];        /* sha-256 value, colon-hex, no algo */
    char setup[16];               /* actpass/active/passive/"" */
    int  ice_lite;                /* a=ice-lite seen */
    int  trickle;                 /* a=ice-options:trickle seen */
    int  n_media;
    kn_sdp_media m[KN_SDP_MAX_MEDIA];
} kn_sdp_offer;

/* The local material for both builders. All strings NUL-terminated and
 * borrowed for the call only. bitrate_kbps feeds the nvstSdp ladder
 * (max = clamp(kbps,1000,200000), min = min(max,5000),
 *  initial = max(min, max/4) — core_sdp.rs nvst_answer()). */
typedef struct kn_sdp_answer_cfg {
    const char *local_ip;         /* dotted quad for c=/rtcp lines: the
                                     routed source address (0.0.0.0 is
                                     what the browser sends with trickle
                                     ICE — OpenNOW/str0m do the same) */
    unsigned    local_port;       /* the single bundled UDP port */
    const char *ufrag;            /* our ICE username fragment */
    const char *pwd;              /* our ICE password */
    const char *fingerprint;      /* "AA:BB:.." sha-256 of our DTLS cert */
    unsigned long sess_id;        /* o=- <sess_id> 2 IN IP4 127.0.0.1 */
    unsigned    width, height, fps;
    unsigned    bitrate_kbps;
} kn_sdp_answer_cfg;

/* Parse a classic WebRTC SDP offer (CRLF or LF). Lenient on unknown
 * attributes, strict on the shape: needs v=0 and at least one m-line.
 * Returns KN_SDP_OK / ERR_ARGS / ERR_PARSE. */
int kn_sdp_offer_parse(const char *sdp, kn_sdp_offer *out);

/* Build the answer SDP (CRLF). Rules (RTSP §65.4): BUNDLE mirrors the
 * offer's section order; session-level ice-ufrag/ice-pwd/fingerprint;
 * setup:active when the offer says actpass (ice-lite server => we are
 * the DTLS client); video answers H264+RTX from the OFFER's payload
 * types (fmtp/rtpmap/rtcp-fb copied verbatim, RTX apt kept), audio
 * answers opus only (RED dropped — str0m parity), datachannel mirrors
 * sctp-port/max-message-size, every section we cannot serve (the mic,
 * an offer with no H264) is rejected with port 0 + a=inactive + the
 * offered format list kept (OpenNOW clones the offered pts onto
 * disabled sections). Needs local_ip/local_port/ufrag/pwd/fingerprint.
 * Returns KN_SDP_OK / ERR_ARGS / ERR_BIG. Never ERR_NOCODEC: a video
 * section without H264 is rejected, not fatal (the audio-only answer
 * is still a valid answer). */
int kn_sdp_answer_build(const kn_sdp_offer *off,
                        const kn_sdp_answer_cfg *cfg,
                        char *out, size_t outsz);

/* Build the client nvstSdp (LF). The OpenNOW recipe: identity triplet
 * from OUR answer material, the 74-line video-attribute table, the
 * frame-pacing target from fps, the fps>60 / >=240 blocks, the
 * packet-pacing group count, the viewport/bitDepth/bitrate ladder,
 * then m=audio 0 + m=application 0 with the ri.* defaults (30 /
 * 0xFFFFFFFF / 15 / 0xFFFFFFFF — the offer-classic carries no ri.*
 * lines, so the OpenNOW fallbacks are the production values; the
 * server's own nvstSdp ri.* block is NOT echoed, matching the
 * reference client). Returns KN_SDP_OK / ERR_ARGS / ERR_BIG. */
int kn_sdp_nvst_build(const kn_sdp_answer_cfg *cfg,
                      char *out, size_t outsz);

/* "candidate:1 1 udp 2122260223 <ip> <port> typ host" — the exact form
 * the F37 ANNOUNCE flew (kn_rtsp.c) and Chrome's host priority. */
int kn_sdp_candidate_host(const char *ip, unsigned port,
                          char *out, size_t outsz);

/* Slice 5d (B2.5): parse an ICE candidate line — the server's trickle
 * candidate is the AUTHORITATIVE media destination for the outbound
 * connectivity checks (the runB2a capture: the offer's m-line ports are
 * placeholders and the real address arrives 67 ms after our answer as
 * "candidate:1467310487 1 udp 1677721855 80.250.98.36 5004 typ srflx
 * raddr invalid rport 0"). Tolerant of the trailing extension tokens
 * (raddr/rport/tcptype/generation/ufrag — and of the seat's literal
 * "raddr invalid rport 0"), strict on the seven mandatory fields.
 * Accepts the bare "candidate:..." form and the SDP "a=candidate:..."
 * form. IPv6 hosts parse into ip[] but leave ip_be == 0 (kn_sock is
 * IPv4-only, stage 2). */
typedef struct kn_sdp_candidate {
    char     foundation[32];
    unsigned component;             /* 1-based; RFC 8445 §5.1 */
    char     transport[8];          /* "udp" | "tcp" */
    unsigned long long priority;
    char     ip[64];                /* verbatim token (v4 or v6) */
    uint32_t ip_be;                 /* network-order IPv4; 0 = not v4 */
    unsigned port;
    char     typ[16];               /* host | srflx | prflx | relay */
    char     raddr[64];             /* "" when absent/garbage-tolerated */
    unsigned rport;
} kn_sdp_candidate;

int kn_sdp_candidate_parse(const char *line, kn_sdp_candidate *out);

#ifdef __cplusplus
}
#endif

#endif /* KN_SDP_H */
