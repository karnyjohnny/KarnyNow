/* kn_rtsp.h — the RTSP-over-WebSocket client (stage-2 signaling).
 *
 * A C port of OpenNOW's RtspClient + prepare_on_endpoint media half
 * (nvst_rtsp.rs @ a49b124; the full provenance table and wire contract
 * live in docs/RTSP.md — this header is the API, that document is the
 * spec). PURE module: no sockets, no TLS, no clocks read internally —
 * a message-transport seam (kn_rtsp_msg_io, satisfied by kn_ws in
 * production and by scripted shims in tests) plus injected now/sleep
 * functions. Everything here is KAT-able offline; the first real rig is
 * the true test (R32's rule applies to signaling too — every deviation
 * from the reference is documented, none invented).
 *
 * Wire contract (docs/RTSP.md §1-3):
 *   request  = "{METHOD} {uri} RTSP/1.0\r\nCSeq: N\r\nRequest-Id: N\r\n"
 *              + headers + [Content-Length] + "\r\n" + body   (one WS Text
 *              message; CSeq == Request-Id == one shared counter)
 *   response = status line "RTSP/1.0 <code> <text>", lower-cased header
 *              map, Content-Length body; leading CR/LF runs are transport
 *              padding and dropped (Bifrost quirk); "\n\n" header ends
 *              accepted; CSeq match required, Request-Id fallback when
 *              CSeq absent; TEARDOWN tolerates (skips) mismatches.
 *   limits   = 20 s request timeout, 4 MiB response cap (nvst_rtsp.rs
 *              :30/:41), peer port-range span < 16 (:2273, nvst.rs:80).
 */
#ifndef KN_RTSP_H
#define KN_RTSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KN_RTSP_OK         =  0,
    KN_RTSP_MORE       =  1,   /* parser: need more bytes (not an error) */
    KN_RTSP_ERR_ARGS   = -1,
    KN_RTSP_ERR_IO     = -2,   /* transport failed */
    KN_RTSP_ERR_TIMEOUT= -3,   /* deadline expired mid-request */
    KN_RTSP_ERR_PROTO  = -4,   /* bad status line / content-length / limits */
    KN_RTSP_ERR_SEQ    = -5,   /* nvst-rtsp-sequence-mismatch */
    KN_RTSP_ERR_BIG    = -6,   /* response exceeds the buffer */
    KN_RTSP_ERR_STATE  = -7    /* SETUP sweep exhausted (no usable peer) */
};

#define KN_RTSP_MAX_RESPONSE   (4u * 1024u * 1024u)  /* nvst_rtsp.rs:41 */
#define KN_RTSP_TIMEOUT_MS     20000u                /* nvst_rtsp.rs:30 */
#define KN_RTSP_MAX_HEADERS    24
#define KN_RTSP_HDR_NAME       48
#define KN_RTSP_HDR_VALUE      512
#define KN_RTSP_URI_MAX        512
#define KN_RTSP_MAX_PEER_SPAN  16u                   /* nvst.rs:80 */
#define KN_RTSP_SETUP_ROUNDS   3u                    /* nvst_rtsp.rs:35 */
#define KN_RTSP_SETUP_DELAY_MS 3000u                 /* nvst_rtsp.rs:36 */

typedef struct kn_rtsp_header {
    const char *name;
    const char *value;
} kn_rtsp_header;

/* Parsed response. headers are COPIED (bounded); body ALIASES the client
 * accumulation buffer — valid until the next kn_rtsp_request call (same
 * consume-or-copy contract as kn_rx frames / kn_ws messages). */
typedef struct kn_rtsp_response {
    int  status;
    char status_text[64];
    char names[KN_RTSP_MAX_HEADERS][KN_RTSP_HDR_NAME];
    char values[KN_RTSP_MAX_HEADERS][KN_RTSP_HDR_VALUE];
    int  hdr_count;
    const char *body;      /* into the client buffer; may be "" */
    size_t      body_len;
} kn_rtsp_response;

/* Message transport seam — kn_ws in production:
 *   send_text: deliver one message, 0 ok / <0 error.
 *   recv_text: one complete message into buf (cap bytes); returns 0 with
 *              *out_len set, KN_RTSP_ERR_TIMEOUT when nothing arrived
 *              within timeout_ms (retryable), <0 fatal.
 * Binary messages never occur on this channel (the reference appends
 * them lossily; our kn_ws surfaces them as BINARY — the glue refuses). */
typedef struct kn_rtsp_msg_io {
    int (*send_text)(void *ctx, const char *text, size_t len);
    int (*recv_text)(void *ctx, char *buf, size_t cap, uint32_t timeout_ms,
                     size_t *out_len);
    void *ctx;
} kn_rtsp_msg_io;

typedef struct kn_rtsp_client {
    kn_rtsp_msg_io io;
    uint64_t cseq;                    /* shared CSeq/Request-Id counter */
    char    *buf;                     /* accumulation (caller-owned) */
    size_t   buf_cap, buf_len;
    uint32_t timeout_ms;
    uint64_t (*now_ms)(void *ctx);    /* injected clock (deadline math) */
    void     *now_ctx;
    void (*sleep_ms)(uint32_t ms, void *ctx);   /* injected (SETUP retries) */
    void *sleep_ctx;
    /* lifetime counters */
    unsigned long long requests, responses, seq_mismatch_skips, timeouts;
    char err[160];                    /* last human failure reason */
} kn_rtsp_client;

int kn_rtsp_client_init(kn_rtsp_client *c, kn_rtsp_msg_io io,
                        char *buf, size_t buf_cap, uint32_t timeout_ms,
                        uint64_t (*now_ms)(void *), void *now_ctx,
                        void (*sleep_ms)(uint32_t, void *), void *sleep_ctx);

/* Serialize one request (the exact reference shape). Returns 0 and sets
 * *out_len, or KN_RTSP_ERR_ARGS / KN_RTSP_ERR_BIG. Exposed for KATs. */
int kn_rtsp_serialize(char *out, size_t outsz, const char *method,
                      const char *uri, uint64_t cseq,
                      const kn_rtsp_header *hdrs, int nhdrs,
                      const char *body, size_t body_len, size_t *out_len);

/* Incremental response parser (take_rtsp_response port). Returns:
 *   KN_RTSP_OK       complete response parsed; *consumed = bytes eaten
 *   KN_RTSP_MORE     incomplete — feed more
 *   KN_RTSP_ERR_SEQ  sequence mismatch (CSeq / Request-Id fallback)
 *   KN_RTSP_ERR_PROTO bad status line or Content-Length
 *   KN_RTSP_ERR_BIG   total exceeds cap
 * out->body aliases `data` (zero-copy). */
int kn_rtsp_parse(const char *data, size_t len, uint64_t expect_cseq,
                  size_t cap, kn_rtsp_response *out, size_t *consumed);

/* One full request/response round: serialize -> send -> accumulate
 * (deadline = now + timeout_ms, every recv waits the remaining time) ->
 * parse. skip_seq_mismatch (TEARDOWN semantics): a mismatched response is
 * dropped and the wait continues. Response/body alias c->buf until the
 * next call. Returns KN_RTSP_OK or a negative code (c->err filled). */
int kn_rtsp_request(kn_rtsp_client *c, const char *method, const char *uri,
                    const kn_rtsp_header *hdrs, int nhdrs,
                    const char *body, size_t body_len, int skip_seq_mismatch,
                    kn_rtsp_response *out);

const char *kn_rtsp_header_get(const kn_rtsp_response *r, const char *name);

/* ------------------------------------------------------------- SDP side */

/* sdp_attribute port: matches "a=x-nv-{name}:" then "a={name}:"
 * (case-insensitive prefixes, value trimmed, empty = not found). Honours
 * the WS-carried-SDP quirks: only the first "||" segment; attribute
 * lookup flattens all ";;" segments (nvst_rtsp.rs sdp_attribute).
 * Returns 0 found / -1 not found. */
int kn_rtsp_sdp_attribute(const char *sdp, const char *name,
                          char *out, size_t outsz);

/* media_control port: the first non-"*" a=control: inside m={kind}
 * (first "||" AND first ";;" segment). 0 found / -1. */
int kn_rtsp_media_control(const char *sdp, const char *kind,
                          char *out, size_t outsz);

/* runtime_key port: a=x-nv-runtime.encryptionKey (exactly 64 hex) +
 * encryptionKeyId (u32). 0 with key65 uppercased / -1 absent-invalid. */
int kn_rtsp_runtime_key(const char *sdp, char key65[65], uint32_t *key_id);

/* The salt rule (nvst_rtsp.rs:1280): key_id as a 24-digit zero-padded
 * uppercase hex string. out needs 25 bytes. */
void kn_rtsp_salt_from_key_id(uint32_t key_id, char out25[25]);

/* advertised_srtp_profile port: first of the 8 known profile names found
 * in the SETUP Transport header (and any srtp/crypto-named headers),
 * then the DESCRIBE SDP lines; default "AEAD_AES_256_GCM_8". Returns a static string. */
const char *kn_rtsp_srtp_profile(const kn_rtsp_response *setup,
                                 const char *describe_sdp);

/* parse_video_peer port: Transport split on ';' and ',', "source=" ip +
 * "X-GS-ServerPort=a[-b]" (both required, non-zero, span < 16). 0/-1. */
int kn_rtsp_parse_video_peer(const char *transport, char ip[64],
                             uint16_t *port, uint16_t *port_end);

/* official_video_setup_control port: append "/0" to a bare
 * "streamid=video/..." control (exactly one '/'). */
void kn_rtsp_official_control(const char *control, char *out, size_t outsz);

/* The SETUP URI ladder (video_setup_candidates port): official form,
 * raw control (when different), then absolute target-joined forms of
 * both. Returns the candidate count (<= max). */
int kn_rtsp_setup_candidates(const char *control, const char *target,
                             char cand[][KN_RTSP_URI_MAX], int max);

/* ------------------------------------------------- DESCRIBE extraction */

typedef struct kn_rtsp_describe {
    char     session[128];        /* Session header (before ';') */
    char     video_control[KN_RTSP_URI_MAX];
    uint8_t  ping_version;        /* default 6 when absent */
    char     remote_ufrag[128], remote_password[128], remote_fp[192];
    int      disable_play;        /* general.disablePlay == "1" */
    int      native_bundle;       /* general.nativeRtcOnBundlePort == "1"
                                     (REQUIRED — legacy transport refused) */
    int      rtcp_on_sctp;
    int      unified_socket;      /* offered_bool default false */
    int      have_runtime_key;
    char     runtime_key[65];
    uint32_t key_id;
} kn_rtsp_describe;

/* Pull everything stage-2 needs out of a DESCRIBE response (header +
 * SDP). Returns 0 when session AND video_control are present (the
 * reference's two hard requirements); -1 otherwise (fields best-effort
 * filled for diagnostics). */
int kn_rtsp_extract_describe(const kn_rtsp_response *resp,
                             kn_rtsp_describe *out);

/* --------------------------------------------------------- SETUP sweep */

typedef struct kn_rtsp_setup_result {
    int      status;                 /* last SETUP status seen */
    char     transport[KN_RTSP_HDR_VALUE];   /* response Transport value */
    char     peer_ip[64];
    uint16_t peer_port, peer_port_end;
    int      bundle_fallback;        /* peer came from the CloudMatch bundle */
    uint8_t  ping_version;           /* SETUP x-nv-ping override */
    char     ping_payload[64];       /* SETUP x-nv-ping-payload */
    int      have_ping_payload;
    unsigned rounds_slept;           /* evidence: retry rounds actually taken */
} kn_rtsp_setup_result;

/* The full setup_video_with_retry port: candidate x transport-form sweep,
 * stop-on-peerless-200 (the alliance-poison rule), CloudMatch bundle-peer
 * fallback, bounded retry rounds via the injected sleep. Headers: the
 * caller passes the common set; Session + x-nv-ping are added here.
 * Returns KN_RTSP_OK (result filled) or a negative code. */
int kn_rtsp_setup_video(kn_rtsp_client *c, const char *control,
                        const char *target, const char *session,
                        uint8_t described_ping_version, uint16_t client_port,
                        const kn_rtsp_header *common, int ncommon,
                        const char *bundle_peer_ip, uint16_t bundle_peer_port,
                        kn_rtsp_setup_result *out);

/* ------------------------------------------------------ ANNOUNCE side */

typedef struct kn_rtsp_announce_params {
    uint32_t width, height, fps;         /* D14: 1280x720 @ 30 */
    uint32_t bitrate_kbps;               /* initial = peak = maximum */
    int      bitstream_format;           /* 0 H264 / 1 H265 / 2 AV1 */
    int      bit_depth, chroma_format;   /* announce_color_lines inputs
                                            (8bit 420 -> 0? NO: bit_depth
                                            value per color_quality; our
                                            8bit_420 -> bitDepth 0,
                                            chromaFormat 1) */
    int      hdr;                        /* dynamicRangeMode:1 when set */
    char     key_hex[65];                /* 64 uppercase hex */
    uint32_t key_id;
    uint32_t bundle_port;                /* clientBundlePort */
    char     local_address[64];
    char     ufrag[128], password[128], fingerprint[192];
    uint32_t video_port;                 /* m=video (the SERVER peer port) */
    uint32_t video_packet_size;
    int      rtcp_on_sctp, unified_socket, microphone_available;
    int      qos_timings_v5;             /* framePacing.mode/feedbackMode */
    uint8_t  dynamic_streaming_mode;
    int      adjust_res_and_fps;         /* dfc.adjustResAndFps */
} kn_rtsp_announce_params;

/* build_announce port (docs/RTSP.md §3 has the attribute table and the
 * exact ordering). CRLF-joined, trailing empty line. Returns 0 with
 * *out_len, or ERR_ARGS/BIG. */
int kn_rtsp_build_announce(const kn_rtsp_announce_params *p,
                           char *out, size_t outsz, size_t *out_len);

/* negotiated_announce port: for the fec/pacing attribute set, the
 * DESCRIBE offer wins when inside its validated range, else our own
 * value; fec.repairMin>Max falls back to ours; repairPercent clamps to
 * [min,max]; initial/peak bitrate clamp to the ceiling. Rewrites the
 * matching "a=x-nv-{name}:{value}" lines in place. Returns 0 / negative.
 * negotiated_out (optional, >= 20 u32s) receives the resolved values in
 * the documented order for the evidence log line. */
int kn_rtsp_negotiate_announce(const char *announce, const char *describe,
                               char *out, size_t outsz, size_t *out_len,
                               uint32_t *negotiated_out);

#ifdef __cplusplus
}
#endif

#endif /* KN_RTSP_H */
