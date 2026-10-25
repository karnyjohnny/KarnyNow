/* kn_http.h — minimal HTTP/1.1 client over an injected byte stream.
 *
 * Scope: exactly what CloudMatch needs (docs/RTSP.md §5, docs/AUTH.md §4):
 * one request per connection ("Connection: close"), response framing by
 * Content-Length, chunked Transfer-Encoding, or read-to-EOF; bounded
 * caller-owned buffers; injected clock for the deadline. NOT a general
 * HTTP stack: no keep-alive, no redirects, no gzip, no URL parsing (the
 * caller passes host header + path; the transport seam is already
 * connected — TLS in production via kn_net, plain TCP in the fake-rig
 * tests).
 *
 * recv contract (kn_http_io): >0 = bytes read, 0 = timeout (retryable),
 * -1 = transport error, -2 = peer closed (EOF). The kn_net TLS-stream
 * adapter and the sandbox plain-TCP adapter both map to this.
 */
#ifndef KN_HTTP_H
#define KN_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KN_HTTP_OK        =  0,
    KN_HTTP_ERR_ARGS  = -1,
    KN_HTTP_ERR_IO    = -2,
    KN_HTTP_ERR_TIMEOUT = -3,
    KN_HTTP_ERR_PROTO = -4,   /* bad status line / chunk framing */
    KN_HTTP_ERR_BIG   = -5,   /* headers overflow the rx buffer */
    KN_HTTP_ERR_EOF   = -6    /* peer closed mid-headers/mid-body */
};

#define KN_HTTP_MAX_HEADERS 32
#define KN_HTTP_HDR_NAME    48
#define KN_HTTP_HDR_VALUE   256

typedef struct kn_http_io {
    int (*send)(void *ctx, const uint8_t *buf, size_t len);
    int (*recv)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    void *ctx;
} kn_http_io;

typedef struct kn_http_header {
    const char *name;
    const char *value;
} kn_http_header;

typedef struct kn_http_response {
    int  status;
    char status_text[48];
    char names[KN_HTTP_MAX_HEADERS][KN_HTTP_HDR_NAME];
    char values[KN_HTTP_MAX_HEADERS][KN_HTTP_HDR_VALUE];
    int  hdr_count;
    char *body;             /* caller buffer; NUL-terminated when it fits */
    size_t body_len;
    int  truncated;         /* body exceeded bodycap (prefix kept) */
} kn_http_response;

/* One request/response round.
 *   path          request target, e.g. "/v2/session?x=1"
 *   host_header   the Host: value (e.g. "prod.cloudmatchbeta.nvidiagrid.net")
 *   hdrs          extra headers (Authorization, nv-client-*, ...)
 *   body/body_len request body (may be NULL/0)
 *   rxb/rxbsz     scratch for the request text + response accumulation
 *                 (>= 16 KiB recommended)
 *   bodybuf/bodycap  destination for the response body
 * Returns KN_HTTP_OK with out filled, or a negative code. */
int kn_http_request(kn_http_io io, const char *method, const char *path,
                    const char *host_header,
                    const kn_http_header *hdrs, int nhdrs,
                    const char *body, size_t body_len,
                    uint32_t timeout_ms,
                    uint64_t (*now_ms)(void *), void *now_ctx,
                    uint8_t *rxb, size_t rxbsz,
                    char *bodybuf, size_t bodycap,
                    kn_http_response *out);

/* Case-insensitive response header lookup (NULL when absent). */
const char *kn_http_header_get(const kn_http_response *r, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTTP_H */
