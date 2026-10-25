/* kn_ws.h — RFC 6455 WebSocket CLIENT framing (stage-2 signaling).
 *
 * Purpose: the transport under kn_rtsp — the reference carries RTSP text
 * as WebSocket Text messages to wss://host:port/rtsp (nvst_rtsp.rs:103,
 * 513-527; docs/RTSP.md §1). This module is the framing ONLY: it sits on
 * an injected byte-stream seam (kn_ws_io), so the sandbox proves it over
 * loopback TCP and the production wiring (round XXI) plugs in the
 * kn_net_mbedtls TLS-1.2 stream without touching a line of framing code.
 *
 * Scope (client side, per RFC 6455):
 *   - opening handshake: GET + Sec-WebSocket-Key (16 CSPRNG bytes,
 *     base64), 101 status check, Sec-WebSocket-Accept verification
 *     (base64(SHA-1(key + GUID)) — kn_sha1, the RFC golden vector is a
 *     test); extra header lines pass through verbatim (the reference
 *     sends "x-nv-sessionid: ..." and "content-length: 0", nvst_rtsp.rs
 *     :517-524).
 *   - frames: FIN/opcode, 7/16/64-bit lengths, CLIENT-SIDE MASKING
 *     mandatory on every sent frame (a fresh 4-byte CSPRNG mask key per
 *     frame, RFC 5.3), server frames with the mask bit set are a
 *     protocol error (RFC 5.1).
 *   - messages: fragmentation (continuation join), Text/Binary surfaced
 *     to the caller, Ping answered with an echoed Pong automatically
 *     (the reference does exactly this, nvst_rtsp.rs:624-626), Pong
 *     counted, Close answered with a Close and surfaced as
 *     KN_WS_ERR_CLOSED (the reference turns it into nvst-network-failed,
 *     :627-632).
 *   - bounded: the reassembly buffer is caller-owned and sized by the
 *     caller (kn_rtsp passes 4 MiB + slack = MAX_REQUEST_RESPONSE_BYTES,
 *     nvst_rtsp.rs:41); anything larger is KN_WS_ERR_TOO_BIG, never a
 *     realloc. Control-frame payload > 125 bytes or a fragmented control
 *     frame = protocol error (RFC 5.5).
 *
 * Randomness: Sec-WebSocket-Key and mask keys MUST be unpredictable
 * (RFC 4.2.1/5.3). kn_ws uses rand_s() on Win32 (msvcrt-backed CSPRNG —
 * NO new PE import; bcrypt/advapi32 deliberately avoided) and
 * getrandom()/urandom on POSIX. A handshake or frame sent with a failed
 * RNG is refused (KN_WS_ERR_ARGS), never sent with weak bytes.
 *
 * NOT in scope (round XXI+): TLS itself (transport seam), server role
 * (tests hand-craft server frames), extensions (none negotiated — the
 * reference negotiates none), auto-reconnect (kn_rtsp owns retries).
 */
#ifndef KN_WS_H
#define KN_WS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KN_WS_OK        =  0,
    KN_WS_ERR_ARGS  = -1,   /* NULL/bad sizes/RNG failure */
    KN_WS_ERR_IO    = -2,   /* transport send/recv failed */
    KN_WS_ERR_PROTO = -3,   /* peer violated RFC 6455 (masked server frame,
                               fragmented/oversize control, bad length bits) */
    KN_WS_ERR_HTTP  = -4,   /* handshake answer is not HTTP/1.x 101
                               (http_status carries the code when parsed) */
    KN_WS_ERR_ACCEPT= -5,   /* Sec-WebSocket-Accept mismatch */
    KN_WS_ERR_BIG   = -6,   /* message exceeds the caller's buffer */
    KN_WS_ERR_CLOSED= -7,   /* close frame seen (close answered already) */
    KN_WS_ERR_TIMEOUT = -8  /* recv deadline expired mid-message (state is
                               kept — call again to continue) */
};

/* RFC 6455 opcodes surfaced by kn_ws_recv_msg (data frames only —
 * control frames are handled inside). */
enum {
    KN_WS_OP_TEXT   = 0x1,
    KN_WS_OP_BINARY = 0x2
};

#define KN_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define KN_WS_MAX_CONTROL_PAYLOAD 125u
#define KN_WS_HANDSHAKE_MAX 4096u   /* HTTP response accumulation cap */

/* Transport seam: a reliable ordered byte stream (TLS in production,
 * plain TCP in tests). send: deliver ALL len bytes, 0 ok / <0 error.
 * recv: store up to cap bytes, >0 count / 0 timeout after timeout_ms /
 * <0 error. Both may be called repeatedly; recv timeouts must be
 * resumable (no state loss). */
typedef struct kn_ws_io {
    int (*send)(void *ctx, const uint8_t *buf, size_t len);
    int (*recv)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    void *ctx;
} kn_ws_io;

typedef struct kn_ws_client {
    kn_ws_io io;
    /* transport byte buffer (partial reads) */
    uint8_t  rxb[4096];
    size_t   rx_start, rx_end;
    /* message reassembly (caller-owned at open) */
    uint8_t *msg;
    size_t   msg_cap, msg_len;
    int      frag_op;            /* -1 = no fragmentation in progress */
    /* current-frame parse state — makes a mid-frame TIMEOUT resumable
     * (ws_take reports partial progress; the header itself resumes via
     * the rxb cursors). Round-XX self-review catch: without this, a
     * mid-payload timeout restarted the header parse inside the payload. */
    int      have_hdr, hdr_fin, hdr_op;
    uint64_t hdr_plen;
    size_t   hdr_done;           /* data-payload bytes already stored */
    uint8_t  ctrl[KN_WS_MAX_CONTROL_PAYLOAD + 1];
    size_t   ctrl_done;
    int      closed;             /* close sent or seen */
    int      handshake_done;
    int      http_status;        /* parsed handshake status (0 = none) */
    char     http[KN_WS_HANDSHAKE_MAX];
    size_t   http_len;
    /* lifetime counters (evidence for logs/summaries) */
    unsigned long long frames_out, frames_in_text, frames_in_binary,
                       pings_in, pongs_in, pongs_out, closes_in,
                       bytes_out, bytes_in;
    int      last_error;         /* KN_WS_ERR_* of the last failure */
    int      last_close_code;    /* peer close payload code (-1 = none) */
} kn_ws_client;

/* Run the opening handshake over io. host/port are used ONLY for the
 * Host: header text (the transport is already connected — the seam owns
 * DNS/TCP/TLS). path e.g. "/rtsp". extra_headers: pre-formatted lines
 * EACH ending in \r\n (may be NULL). msgbuf/msgcap: the reassembly
 * buffer (>= the largest expected message; kn_rtsp uses 4 MiB + slack).
 * timeout_ms bounds every transport wait during the handshake.
 * Returns KN_WS_OK or a negative code (last_error set; err/errsz get a
 * one-line human reason). */
int kn_ws_client_open(kn_ws_client *c, kn_ws_io io, const char *host,
                      uint16_t port, const char *path,
                      const char *extra_headers, uint32_t timeout_ms,
                      uint8_t *msgbuf, size_t msgcap,
                      char *err, size_t errsz);

/* Send one single-frame message (masked). Text requires valid UTF-8 by
 * contract (the caller's JSON/RTSP is ASCII); no validation here — the
 * reference sends RTSP text verbatim. len <= msg_cap is NOT required
 * (sending streams in chunks), but receivers enforce their own caps. */
int kn_ws_send_text(kn_ws_client *c, const char *text, size_t len);
int kn_ws_send_binary(kn_ws_client *c, const uint8_t *data, size_t len);
int kn_ws_send_ping(kn_ws_client *c, const uint8_t *data, size_t len); /* <=125 */
int kn_ws_send_close(kn_ws_client *c, uint16_t code);  /* 0 = no payload */

/* Receive one COMPLETE message (fragments joined) into c->msg; valid
 * until the next recv call. *out_opcode = KN_WS_OP_TEXT/BINARY,
 * *out_len = payload bytes. Pings are answered inline and skipped; a
 * Close is answered and returns KN_WS_ERR_CLOSED; a mid-message timeout
 * returns KN_WS_ERR_TIMEOUT with reassembly state KEPT (call again). */
int kn_ws_recv_msg(kn_ws_client *c, uint32_t timeout_ms, int *out_opcode,
                   size_t *out_len);

/* ---- exposed for tests (stable, small, no hidden state) ---- */

/* Standard-alphabet base64 encoder with padding. Returns bytes written
 * (excl. NUL) or -1 when out is too small (need 4*ceil(n/3)+1). */
int kn_ws_base64(const uint8_t *in, size_t n, char *out, size_t outsz);

/* The RFC 6455 accept value for a base64 key: base64(SHA1(key + GUID)).
 * out must hold >= 29 bytes. Returns 0 / -1 bad args. */
int kn_ws_accept(const char *key_b64, char out[29]);

/* CSPRNG fill: 0 ok, -1 unavailable (callers MUST refuse to proceed). */
int kn_ws_rand(uint8_t *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* KN_WS_H */
