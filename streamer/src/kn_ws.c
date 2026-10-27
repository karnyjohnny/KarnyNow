/* kn_ws.c — RFC 6455 client framing (see kn_ws.h for the contract).
 * Sandbox-proven over loopback TCP (test_knws); production transport is
 * the kn_net_mbedtls TLS stream (round XXI wiring). */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#  define _CRT_RAND_S          /* rand_s BEFORE <stdlib.h> */
#endif

#include "kn/kn_ws.h"
#include "kn/kn_sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/random.h>      /* getrandom (glibc >= 2.25) */
#  endif
#endif

/* ------------------------------------------------------------ helpers */

int kn_ws_take_pong(kn_ws_client *c, unsigned long long *seen,
                    uint8_t *buf, size_t cap, size_t *out_len)
{
    size_t n;
    if (!c || !seen) return 0;
    if (c->last_pong_seq == *seen) return 0;      /* nothing fresh */
    *seen = c->last_pong_seq;
    n = c->last_pong_len < cap ? c->last_pong_len : cap;
    if (buf && n) memcpy(buf, c->last_pong, n);
    if (out_len) *out_len = n;
    return 1;
}

int kn_ws_rand(uint8_t *buf, size_t n)
{
    size_t i;
    if (!buf) return -1;
#ifdef _WIN32
    for (i = 0; i < n; ) {
        unsigned v;
        size_t take, k;
        if (rand_s(&v) != 0) return -1;
        take = n - i < sizeof v ? n - i : sizeof v;
        for (k = 0; k < take; ++k) buf[i + k] = (uint8_t)(v >> (8 * k));
        i += take;
    }
    return 0;
#else
    i = 0;
#  ifdef __linux__
    while (i < n) {
        long r = getrandom(buf + i, n - i, 0);
        if (r < 0) break;
        i += (size_t)r;
    }
    if (i == n) return 0;
#  endif
    {   /* portable fallback: /dev/urandom (POSIX hosts; Linux path above
           normally succeeds) */
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) return -1;
        i = 0;
        while (i < n) {
            ssize_t r = read(fd, buf + i, n - i);
            if (r <= 0) { close(fd); return -1; }
            i += (size_t)r;
        }
        close(fd);
        return 0;
    }
#endif
}

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int kn_ws_base64(const uint8_t *in, size_t n, char *out, size_t outsz)
{
    size_t i, o = 0;
    size_t need = 4u * ((n + 2u) / 3u) + 1u;
    if (!in && n) return -1;
    if (!out || outsz < need) return -1;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                     (uint32_t)in[i + 2];
        out[o++] = k_b64[(v >> 18) & 63u];
        out[o++] = k_b64[(v >> 12) & 63u];
        out[o++] = k_b64[(v >> 6) & 63u];
        out[o++] = k_b64[v & 63u];
    }
    if (i + 1 == n) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = k_b64[(v >> 18) & 63u];
        out[o++] = k_b64[(v >> 12) & 63u];
        out[o++] = '=';
        out[o++] = '=';
    } else if (i + 2 == n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = k_b64[(v >> 18) & 63u];
        out[o++] = k_b64[(v >> 12) & 63u];
        out[o++] = k_b64[(v >> 6) & 63u];
        out[o++] = '=';
    }
    out[o] = '\0';
    return (int)o;
}

int kn_ws_accept(const char *key_b64, char out[29])
{
    char cat[128];
    uint8_t dig[KN_SHA1_DIGEST_LEN];
    int n;
    if (!key_b64 || !out) return -1;
    n = snprintf(cat, sizeof cat, "%s%s", key_b64, KN_WS_GUID);
    if (n < 0 || (size_t)n >= sizeof cat) return -1;
    kn_sha1(cat, (size_t)n, dig);
    if (kn_ws_base64(dig, sizeof dig, out, 29) != 28) return -1;
    return 0;
}

/* ------------------------------------------------- buffered transport */

static int ws_fill(kn_ws_client *c, uint32_t timeout_ms)
{
    int r;
    if (c->rx_start == c->rx_end) { c->rx_start = c->rx_end = 0; }
    if (c->rx_end >= sizeof c->rxb) return KN_WS_ERR_PROTO;   /* need > 4 KiB
        contiguous header/handshake bytes — impossible by construction */
    r = c->io.recv(c->io.ctx, c->rxb + c->rx_end,
                   sizeof c->rxb - c->rx_end, timeout_ms);
    if (r < 0) { c->last_error = KN_WS_ERR_IO; return KN_WS_ERR_IO; }
    if (r == 0) { c->last_error = KN_WS_ERR_TIMEOUT; return KN_WS_ERR_TIMEOUT; }
    c->rx_end += (size_t)r;
    c->bytes_in += (unsigned long long)r;
    return KN_WS_OK;
}

/* Ensure at least n bytes are buffered (n <= 4096 - margin). Returns
 * KN_WS_OK / TIMEOUT / IO. Buffered bytes live at rxb[rx_start..]. */
static int ws_need(kn_ws_client *c, size_t n, uint32_t timeout_ms)
{
    while (c->rx_end - c->rx_start < n) {
        int rc = ws_fill(c, timeout_ms);
        if (rc != KN_WS_OK) return rc;
    }
    return KN_WS_OK;
}

/* Consume n payload bytes into dst (dst may be NULL = discard). On a
 * timeout/IO error *done reports the bytes already consumed — the caller
 * persists them so the frame resumes exactly where it stopped. */
static int ws_take(kn_ws_client *c, size_t n, uint8_t *dst, uint32_t timeout_ms,
                   size_t *done)
{
    size_t off = 0;
    while (off < n) {
        size_t avail = c->rx_end - c->rx_start;
        size_t take = n - off < avail ? n - off : avail;
        if (take == 0) {
            int rc = ws_fill(c, timeout_ms);
            if (rc != KN_WS_OK) { *done = off; return rc; }
            continue;
        }
        if (dst) memcpy(dst + off, c->rxb + c->rx_start, take);
        c->rx_start += take;
        off += take;
    }
    *done = off;
    return KN_WS_OK;
}

/* ---------------------------------------------------------- handshake */

/* Find a header value in the accumulated HTTP response (case-insensitive
 * name). Returns a pointer into c->http (NUL not guaranteed — copy with
 * the length) or NULL. */
static const char *ws_http_header(const kn_ws_client *c, const char *name,
                                  size_t *out_len)
{
    size_t i = 0, nlen = strlen(name);
    while (i < c->http_len) {
        const char *ls = c->http + i;
        const char *le = memchr(ls, '\n', c->http_len - i);
        size_t llen = le ? (size_t)(le - ls) : c->http_len - i;
        while (llen && (ls[llen - 1] == '\r' || ls[llen - 1] == ' ')) llen--;
        if (llen > nlen + 1 && ls[nlen] == ':') {
            size_t k;
            int eq = 1;
            for (k = 0; k < nlen; ++k) {
                char a = ls[k], b = name[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { eq = 0; break; }
            }
            if (eq) {
                const char *v = ls + nlen + 1;
                size_t vl = llen - nlen - 1;
                while (vl && *v == ' ') { v++; vl--; }
                *out_len = vl;
                return v;
            }
        }
        if (!le) break;
        i += (size_t)(le - ls) + 1;
    }
    return NULL;
}

static int ws_send_all(kn_ws_client *c, const uint8_t *buf, size_t len)
{
    if (len == 0) return KN_WS_OK;
    if (c->io.send(c->io.ctx, buf, len) != 0) {
        c->last_error = KN_WS_ERR_IO;
        return KN_WS_ERR_IO;
    }
    c->bytes_out += (unsigned long long)len;
    return KN_WS_OK;
}

int kn_ws_client_open(kn_ws_client *c, kn_ws_io io, const char *host,
                      uint16_t port, const char *path,
                      const char *extra_headers, uint32_t timeout_ms,
                      uint8_t *msgbuf, size_t msgcap,
                      char *err, size_t errsz)
{
    uint8_t keyraw[16];
    char keyb64[32], expect[29], req[2048];
    int n, rc;

    if (err && errsz) err[0] = '\0';
    if (!c || !io.send || !io.recv || !host || !path || !msgbuf ||
        msgcap < 128)
        return KN_WS_ERR_ARGS;
    memset(c, 0, sizeof *c);
    c->io = io;
    c->frag_op = -1;
    c->last_close_code = -1;
    c->msg = msgbuf;
    c->msg_cap = msgcap;

    if (kn_ws_rand(keyraw, sizeof keyraw) != 0) {
        if (err && errsz) snprintf(err, errsz, "CSPRNG unavailable");
        c->last_error = KN_WS_ERR_ARGS;
        return KN_WS_ERR_ARGS;      /* never handshake with weak bytes */
    }
    if (kn_ws_base64(keyraw, sizeof keyraw, keyb64, sizeof keyb64) != 24) {
        c->last_error = KN_WS_ERR_ARGS;
        return KN_WS_ERR_ARGS;
    }
    n = snprintf(req, sizeof req,
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "%s\r\n",
                 path, host, (unsigned)port, keyb64,
                 extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof req) {
        if (err && errsz) snprintf(err, errsz, "handshake request too long");
        c->last_error = KN_WS_ERR_ARGS;
        return KN_WS_ERR_ARGS;
    }
    rc = ws_send_all(c, (const uint8_t *)req, (size_t)n);
    if (rc != KN_WS_OK) {
        if (err && errsz) snprintf(err, errsz, "transport send failed");
        return rc;
    }

    /* read the HTTP response up to \r\n\r\n (or \n\n) */
    for (;;) {
        const char *end4 = NULL, *end2 = NULL;
        size_t i;
        for (i = 0; i + 1 < c->http_len; ++i) {
            if (!end2 && c->http[i] == '\n' && c->http[i + 1] == '\n')
                end2 = c->http + i + 2;
            if (!end4 && i + 3 < c->http_len &&
                memcmp(c->http + i, "\r\n\r\n", 4) == 0)
                end4 = c->http + i + 4;
        }
        if (end4 || end2) {
            const char *hend = end4 ? end4 : end2;
            size_t consumed = (size_t)(hend - c->http);
            /* anything past the header block is frame data */
            c->rx_end = c->http_len - consumed;
            if (c->rx_end) memcpy(c->rxb, c->http + consumed, c->rx_end);
            c->http_len = consumed;
            break;
        }
        if (c->http_len >= sizeof c->http - 1) {
            if (err && errsz) snprintf(err, errsz, "handshake response too long");
            c->last_error = KN_WS_ERR_HTTP;
            return KN_WS_ERR_HTTP;
        }
        {   /* pull more bytes */
            int r = c->io.recv(c->io.ctx,
                               (uint8_t *)c->http + c->http_len,
                               sizeof c->http - 1 - c->http_len, timeout_ms);
            if (r < 0) {
                if (err && errsz) snprintf(err, errsz, "transport recv failed");
                c->last_error = KN_WS_ERR_IO;
                return KN_WS_ERR_IO;
            }
            if (r == 0) {
                if (err && errsz) snprintf(err, errsz, "handshake timed out");
                c->last_error = KN_WS_ERR_TIMEOUT;
                return KN_WS_ERR_TIMEOUT;
            }
            c->http_len += (size_t)r;
            c->http[c->http_len] = '\0';
        }
    }

    /* status line: HTTP/1.x 101 <reason> */
    {
        int code = 0;
        if (c->http_len < 12 || memcmp(c->http, "HTTP/1.", 7) != 0 ||
            sscanf(c->http + 9, "%3d", &code) != 1) {
            if (err && errsz) snprintf(err, errsz, "malformed handshake answer");
            c->last_error = KN_WS_ERR_HTTP;
            return KN_WS_ERR_HTTP;
        }
        c->http_status = code;
        if (code != 101) {
            if (err && errsz)
                snprintf(err, errsz, "handshake HTTP %d (want 101)", code);
            c->last_error = KN_WS_ERR_HTTP;
            return KN_WS_ERR_HTTP;
        }
    }
    /* Sec-WebSocket-Accept verification */
    {
        size_t vl = 0;
        const char *v = ws_http_header(c, "sec-websocket-accept", &vl);
        if (!v || vl != 28) {
            if (err && errsz) snprintf(err, errsz, "missing/short Accept header");
            c->last_error = KN_WS_ERR_ACCEPT;
            return KN_WS_ERR_ACCEPT;
        }
        if (kn_ws_accept(keyb64, expect) != 0 ||
            memcmp(expect, v, 28) != 0) {
            if (err && errsz) snprintf(err, errsz, "Sec-WebSocket-Accept mismatch");
            c->last_error = KN_WS_ERR_ACCEPT;
            return KN_WS_ERR_ACCEPT;
        }
    }
    c->handshake_done = 1;
    return KN_WS_OK;
}

/* -------------------------------------------------------------- send */

static int ws_send_frame(kn_ws_client *c, int fin, int opcode,
                         const uint8_t *payload, size_t len)
{
    uint8_t hdr[14];
    uint8_t mask[4];
    size_t hn = 0, off = 0;
    uint8_t scratch[4096];

    if (!c->handshake_done || c->closed) {
        c->last_error = KN_WS_ERR_CLOSED;
        return KN_WS_ERR_CLOSED;
    }
    if (kn_ws_rand(mask, sizeof mask) != 0) {
        c->last_error = KN_WS_ERR_ARGS;
        return KN_WS_ERR_ARGS;      /* never send unmasked/weak-mask frames */
    }
    hdr[hn++] = (uint8_t)((fin ? 0x80 : 0x00) | (opcode & 0x0F));
    if (len < 126u) {
        hdr[hn++] = (uint8_t)(0x80u | len);
    } else if (len <= 0xFFFFu) {
        hdr[hn++] = 0x80u | 126u;
        hdr[hn++] = (uint8_t)(len >> 8);
        hdr[hn++] = (uint8_t)(len & 0xFFu);
    } else {
        uint64_t l64 = (uint64_t)len;
        int i;
        hdr[hn++] = 0x80u | 127u;
        for (i = 56; i >= 0; i -= 8) hdr[hn++] = (uint8_t)(l64 >> i);
    }
    memcpy(hdr + hn, mask, 4);
    hn += 4;
    if (ws_send_all(c, hdr, hn) != KN_WS_OK) return KN_WS_ERR_IO;

    while (off < len) {
        size_t chunk = len - off < sizeof scratch ? len - off : sizeof scratch;
        size_t i;
        for (i = 0; i < chunk; ++i)
            scratch[i] = (uint8_t)(payload[off + i] ^ mask[(off + i) & 3u]);
        if (ws_send_all(c, scratch, chunk) != KN_WS_OK) return KN_WS_ERR_IO;
        off += chunk;
    }
    c->frames_out++;
    return KN_WS_OK;
}

int kn_ws_send_text(kn_ws_client *c, const char *text, size_t len)
{
    if (!c || (!text && len)) return KN_WS_ERR_ARGS;
    return ws_send_frame(c, 1, KN_WS_OP_TEXT, (const uint8_t *)text, len);
}

int kn_ws_send_binary(kn_ws_client *c, const uint8_t *data, size_t len)
{
    if (!c || (!data && len)) return KN_WS_ERR_ARGS;
    return ws_send_frame(c, 1, KN_WS_OP_BINARY, data, len);
}

int kn_ws_send_ping(kn_ws_client *c, const uint8_t *data, size_t len)
{
    if (!c || len > KN_WS_MAX_CONTROL_PAYLOAD) return KN_WS_ERR_ARGS;
    return ws_send_frame(c, 1, 0x9, data, len);
}

int kn_ws_send_close(kn_ws_client *c, uint16_t code)
{
    uint8_t pl[2];
    int rc;
    if (!c) return KN_WS_ERR_ARGS;
    if (c->closed) return KN_WS_OK;      /* idempotent */
    if (code) {
        pl[0] = (uint8_t)(code >> 8);
        pl[1] = (uint8_t)(code & 0xFFu);
        rc = ws_send_frame(c, 1, 0x8, pl, 2);
    } else {
        rc = ws_send_frame(c, 1, 0x8, NULL, 0);
    }
    c->closed = 1;                       /* sent OR failed: no more sends */
    return rc;
}

/* -------------------------------------------------------------- recv */

int kn_ws_recv_msg(kn_ws_client *c, uint32_t timeout_ms, int *out_opcode,
                   size_t *out_len)
{
    if (!c) return KN_WS_ERR_ARGS;
    if (out_opcode) *out_opcode = 0;
    if (out_len) *out_len = 0;
    if (c->closed) return KN_WS_ERR_CLOSED;

    for (;;) {
        int rc;
        size_t done;

        /* ---- frame header (resumes through the rxb cursors) ---- */
        if (!c->have_hdr) {
            uint8_t h2[2];
            int masked;
            uint64_t plen;
            rc = ws_need(c, 2, timeout_ms);
            if (rc != KN_WS_OK) return rc;
            memcpy(h2, c->rxb + c->rx_start, 2);
            c->rx_start += 2;
            masked = (h2[1] & 0x80) != 0;
            plen   = h2[1] & 0x7Fu;
            if (h2[0] & 0x70) {                 /* RSV bits — no extensions */
                c->last_error = KN_WS_ERR_PROTO;
                return KN_WS_ERR_PROTO;
            }
            if (masked) {                       /* RFC 5.1: servers MUST NOT mask */
                c->last_error = KN_WS_ERR_PROTO;
                return KN_WS_ERR_PROTO;
            }
            if (plen == 126u) {
                uint8_t e[2];
                rc = ws_take(c, 2, e, timeout_ms, &done);
                if (rc != KN_WS_OK) { c->last_error = rc; return rc; }
                plen = ((uint64_t)e[0] << 8) | e[1];
            } else if (plen == 127u) {
                uint8_t e[8];
                int i;
                rc = ws_take(c, 8, e, timeout_ms, &done);
                if (rc != KN_WS_OK) { c->last_error = rc; return rc; }
                plen = 0;
                for (i = 0; i < 8; ++i) plen = (plen << 8) | e[i];
                if (plen >> 63) {               /* RFC: high bit MUST be 0 */
                    c->last_error = KN_WS_ERR_PROTO;
                    return KN_WS_ERR_PROTO;
                }
            }
            c->hdr_fin  = (h2[0] & 0x80) != 0;
            c->hdr_op   = h2[0] & 0x0F;
            c->hdr_plen = plen;
            c->hdr_done = 0;
            c->ctrl_done = 0;
            c->have_hdr = 1;
            if (c->hdr_op >= 0x8 &&
                (!c->hdr_fin || plen > KN_WS_MAX_CONTROL_PAYLOAD)) {
                c->last_error = KN_WS_ERR_PROTO;    /* RFC 5.5 */
                return KN_WS_ERR_PROTO;
            }
            if (c->hdr_op < 0x8) {
                /* classify + capacity-check BEFORE consuming payload */
                if (c->hdr_op == 0x0) {
                    if (c->frag_op < 0) {
                        c->last_error = KN_WS_ERR_PROTO;   /* orphan cont */
                        return KN_WS_ERR_PROTO;
                    }
                } else if (c->hdr_op == KN_WS_OP_TEXT ||
                           c->hdr_op == KN_WS_OP_BINARY) {
                    if (c->frag_op >= 0) {
                        c->last_error = KN_WS_ERR_PROTO;   /* nested start */
                        return KN_WS_ERR_PROTO;
                    }
                    c->frag_op = c->hdr_op;
                    c->msg_len = 0;
                } else {
                    c->last_error = KN_WS_ERR_PROTO;       /* reserved op */
                    return KN_WS_ERR_PROTO;
                }
                if (c->msg_len + plen > c->msg_cap) {
                    c->have_hdr = 0;
                    c->frag_op = -1;
                    c->msg_len = 0;
                    c->last_error = KN_WS_ERR_BIG;         /* never realloc */
                    return KN_WS_ERR_BIG;
                }
            }
        }

        /* ---- frame payload (resumable via hdr_done / ctrl_done) ---- */
        if (c->hdr_op >= 0x8) {
            size_t want = (size_t)c->hdr_plen - c->ctrl_done;
            if (want) {
                rc = ws_take(c, want, c->ctrl + c->ctrl_done, timeout_ms,
                             &done);
                c->ctrl_done += done;
                if (rc != KN_WS_OK) { c->last_error = rc; return rc; }
            }
            c->have_hdr = 0;
            if (c->hdr_op == 0x9) {                        /* ping -> pong */
                c->pings_in++;
                if (!c->closed &&
                    ws_send_frame(c, 1, 0xA, c->ctrl, c->ctrl_done) == KN_WS_OK)
                    c->pongs_out++;
                continue;                                  /* not surfaced */
            }
            if (c->hdr_op == 0xA) {
                c->pongs_in++;
                /* R3-D: keep the payload — the control worker matches it
                 * against its ping sequence (nvst_rtsp.rs:895-901) */
                if (c->ctrl_done <= KN_WS_MAX_CONTROL_PAYLOAD) {
                    memcpy(c->last_pong, c->ctrl, c->ctrl_done);
                    c->last_pong_len = c->ctrl_done;
                    c->last_pong_seq++;
                }
                continue;
            }
            if (c->hdr_op == 0x8) {                        /* close */
                c->closes_in++;
                c->last_close_code = c->ctrl_done >= 2
                    ? (int)(((uint16_t)c->ctrl[0] << 8) | c->ctrl[1]) : -1;
                if (!c->closed) (void)kn_ws_send_close(c, 1000);
                c->closed = 1;
                c->last_error = KN_WS_ERR_CLOSED;
                return KN_WS_ERR_CLOSED;
            }
            c->last_error = KN_WS_ERR_PROTO;               /* unknown ctrl */
            return KN_WS_ERR_PROTO;
        }

        {   /* data frame payload → straight into the reassembly buffer */
            size_t want = (size_t)c->hdr_plen - c->hdr_done;
            if (want) {
                rc = ws_take(c, want, c->msg + c->msg_len + c->hdr_done,
                             timeout_ms, &done);
                c->hdr_done += done;
                if (rc != KN_WS_OK) { c->last_error = rc; return rc; }
            }
            c->msg_len += (size_t)c->hdr_plen;
            if (!c->hdr_fin) { c->have_hdr = 0; continue; }   /* next fragment */
            {
                int fop = c->frag_op;
                c->have_hdr = 0;
                c->frag_op = -1;
                if (fop == KN_WS_OP_TEXT) c->frames_in_text++;
                else c->frames_in_binary++;
                if (out_opcode) *out_opcode = fop;
                if (out_len) *out_len = c->msg_len;
                return KN_WS_OK;
            }
        }
    }
}
