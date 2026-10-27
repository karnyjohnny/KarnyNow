/* kn_http.c — minimal HTTP/1.1 client (see kn_http.h). */
#include "kn/kn_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* memchr-based needle search (memmem is a GNU extension — not portable) */
static const uint8_t *memmem_local(const uint8_t *hay, size_t haylen,
                                   const char *needle, size_t nlen)
{
    size_t i;
    if (nlen == 0 || nlen > haylen) return NULL;
    for (i = 0; i + nlen <= haylen; ++i)
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    return NULL;
}

static char ht_lower(char a)
{
    return (a >= 'A' && a <= 'Z') ? (char)(a + 32) : a;
}

const char *kn_http_header_get(const kn_http_response *r, const char *name)
{
    int i;
    size_t k, n;
    char nm[KN_HTTP_HDR_NAME];
    if (!r || !name) return NULL;
    n = strlen(name);
    if (n >= sizeof nm) return NULL;
    for (k = 0; k < n; ++k) nm[k] = ht_lower(name[k]);
    nm[n] = '\0';
    for (i = 0; i < r->hdr_count; ++i)
        if (strcmp(r->names[i], nm) == 0) return r->values[i];
    return NULL;
}

/* rx accumulation state over rxb */
typedef struct {
    uint8_t *b;
    size_t cap, len;
} rxbuf;

static int rx_fill(rxbuf *r, kn_http_io *io, uint32_t timeout_ms)
{
    int n;
    if (r->len >= r->cap) return KN_HTTP_ERR_BIG;
    n = io->recv(io->ctx, r->b + r->len, r->cap - r->len, timeout_ms);
    if (n > 0) { r->len += (size_t)n; return KN_HTTP_OK; }
    if (n == 0) return KN_HTTP_ERR_TIMEOUT;
    return (n == -2) ? KN_HTTP_ERR_EOF : KN_HTTP_ERR_IO;
}

/* hex chunk-size parse (strict: hex digits only, bounded) */
static int parse_chunk_size(const uint8_t *s, size_t n, size_t *out)
{
    size_t i, v = 0;
    if (n == 0 || n > 16) return -1;
    for (i = 0; i < n; ++i) {
        char c = (char)s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (size_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (size_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (size_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

int kn_http_request(kn_http_io io, const char *method, const char *path,
                    const char *host_header,
                    const kn_http_header *hdrs, int nhdrs,
                    const char *body, size_t body_len,
                    uint32_t timeout_ms,
                    uint64_t (*now_ms)(void *), void *now_ctx,
                    uint8_t *rxb, size_t rxbsz,
                    char *bodybuf, size_t bodycap,
                    kn_http_response *out)
{
    rxbuf rx;
    size_t req_len = 0, o, hend = 0, i;
    uint64_t deadline;
    int rc, n, chunked = 0;
    long long content_length = -1;

    if (!io.send || !io.recv || !method || !path || !host_header || !now_ms ||
        !rxb || rxbsz < 4096 || !bodybuf || bodycap == 0 || !out ||
        (nhdrs && !hdrs) || (body_len && !body))
        return KN_HTTP_ERR_ARGS;
    memset(out, 0, sizeof *out);
    out->body = bodybuf;
    deadline = now_ms(now_ctx) + (timeout_ms ? timeout_ms : 20000u);

    /* ---- serialize the request straight into rxb, send it ---- */
    n = snprintf((char *)rxb, rxbsz,
                 "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                 method, path, host_header);
    if (n < 0 || (size_t)n >= rxbsz) return KN_HTTP_ERR_BIG;
    req_len = (size_t)n;
    for (i = 0; i < (size_t)(nhdrs > 0 ? nhdrs : 0); ++i) {
        n = snprintf((char *)rxb + req_len, rxbsz - req_len, "%s: %s\r\n",
                     hdrs[i].name, hdrs[i].value);
        if (n < 0 || req_len + (size_t)n >= rxbsz) return KN_HTTP_ERR_BIG;
        req_len += (size_t)n;
    }
    n = snprintf((char *)rxb + req_len, rxbsz - req_len,
                 body_len ? "Content-Length: %lu\r\n\r\n" : "\r\n",
                 body_len ? (unsigned long)(body_len % 10000000000ul) : 0ul);
    if (n < 0 || req_len + (size_t)n >= rxbsz) return KN_HTTP_ERR_BIG;
    req_len += (size_t)n;
    if (io.send(io.ctx, rxb, req_len) != 0) return KN_HTTP_ERR_IO;
    if (body_len && io.send(io.ctx, (const uint8_t *)body, body_len) != 0)
        return KN_HTTP_ERR_IO;

    /* ---- accumulate the response header block ---- */
    rx.b = rxb; rx.cap = rxbsz; rx.len = 0;
    for (;;) {
        uint8_t *p4;
        if (rx.len >= 4 &&
            (p4 = (uint8_t *)memmem_local(rx.b, rx.len, "\r\n\r\n", 4)) != NULL) {
            hend = (size_t)(p4 - rx.b) + 4;
            break;
        }
        rc = rx_fill(&rx, &io, 1000u);
        if (rc == KN_HTTP_ERR_TIMEOUT) {
            if (now_ms(now_ctx) >= deadline) return KN_HTTP_ERR_TIMEOUT;
            continue;
        }
        if (rc != KN_HTTP_OK) return rc;
    }

    /* ---- status line + headers ---- */
    {
        size_t ls = 0, le;
        const char *sp1, *sp2;
        le = 0;
        while (le < hend && rx.b[le] != '\r' && rx.b[le] != '\n') le++;
        if (le >= 5 && memcmp(rx.b, "HTTP/", 5) != 0) return KN_HTTP_ERR_PROTO;
        sp1 = (const char *)memchr(rx.b, ' ', le);
        if (!sp1) return KN_HTTP_ERR_PROTO;
        sp2 = (const char *)memchr(sp1 + 1, ' ',
                                   (size_t)(rx.b + le - (const uint8_t *)sp1 - 1));
        out->status = atoi(sp1 + 1);
        if (out->status <= 0 || out->status > 999) return KN_HTTP_ERR_PROTO;
        if (sp2) {
            size_t tl = (size_t)(rx.b + le - (const uint8_t *)sp2 - 1);
            if (tl >= sizeof out->status_text) tl = sizeof out->status_text - 1;
            memcpy(out->status_text, sp2 + 1, tl);
            out->status_text[tl] = '\0';
        }
        ls = le;
        while (ls < hend && (rx.b[ls] == '\r' || rx.b[ls] == '\n')) ls++;
        while (ls < hend) {
            const uint8_t *colon;
            size_t nl, vl;
            const uint8_t *v;
            le = ls;
            while (le < hend && rx.b[le] != '\r' && rx.b[le] != '\n') le++;
            colon = (const uint8_t *)memchr(rx.b + ls, ':', le - ls);
            if (colon && out->hdr_count < KN_HTTP_MAX_HEADERS) {
                nl = (size_t)(colon - (rx.b + ls));
                while (nl && rx.b[ls + nl - 1] == ' ') nl--;
                if (nl && nl < sizeof out->names[0]) {
                    size_t k;
                    int slot = out->hdr_count++;
                    for (k = 0; k < nl; ++k)
                        out->names[slot][k] = ht_lower((char)rx.b[ls + k]);
                    out->names[slot][nl] = '\0';
                    v = colon + 1;
                    vl = (size_t)(rx.b + le - v);
                    while (vl && *v == ' ') { v++; vl--; }
                    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r')) vl--;
                    if (vl >= sizeof out->values[0])
                        vl = sizeof out->values[0] - 1;   /* honest truncation */
                    memcpy(out->values[slot], v, vl);
                    out->values[slot][vl] = '\0';
                    if (strcmp(out->names[slot], "transfer-encoding") == 0 &&
                        strstr(out->values[slot], "chunked"))
                        chunked = 1;
                    if (strcmp(out->names[slot], "content-length") == 0) {
                        char *endp = NULL;
                        content_length = strtoll(out->values[slot], &endp, 10);
                        if (endp == out->values[slot] || *endp != '\0' ||
                            content_length < 0)
                            content_length = -1;
                    }
                }
            }
            ls = le;
            while (ls < hend && (rx.b[ls] == '\r' || rx.b[ls] == '\n')) ls++;
        }
    }

    /* ---- body ---- */
    o = 0;
    if (bodycap && bodybuf) bodybuf[0] = '\0';
    if (chunked) {
        size_t cur = hend;                 /* read cursor into rx */
        for (;;) {
            size_t csz = 0, line_end;
            /* need the chunk-size line */
            for (;;) {
                line_end = cur;
                while (line_end < rx.len && rx.b[line_end] != '\r' &&
                       rx.b[line_end] != '\n')
                    line_end++;
                if (line_end < rx.len) break;
                rc = rx_fill(&rx, &io, 1000u);
                if (rc == KN_HTTP_ERR_TIMEOUT) {
                    if (now_ms(now_ctx) >= deadline) return KN_HTTP_ERR_TIMEOUT;
                    continue;
                }
                if (rc != KN_HTTP_OK) return rc;
            }
            /* strip chunk extensions after ';' */
            {
                size_t semi = cur;
                while (semi < line_end && rx.b[semi] != ';') semi++;
                if (parse_chunk_size(rx.b + cur, semi - cur, &csz) != 0)
                    return KN_HTTP_ERR_PROTO;
            }
            /* skip the size line's CRLF terminator — ensure both bytes
             * are buffered first (1-byte delivery used to strand the
             * cursor ON the '\n': the skip is resumable now) */
            while (rx.len < line_end + 2) {
                rc = rx_fill(&rx, &io, 1000u);
                if (rc == KN_HTTP_ERR_TIMEOUT) {
                    if (now_ms(now_ctx) >= deadline)
                        return KN_HTTP_ERR_TIMEOUT;
                    continue;
                }
                if (rc == KN_HTTP_ERR_EOF) break;   /* let the payload
                                                       wait report it */
                if (rc != KN_HTTP_OK) return rc;
            }
            cur = line_end;
            if (cur < rx.len && rx.b[cur] == '\r') cur++;
            if (cur < rx.len && rx.b[cur] == '\n') cur++;
            if (csz == 0) break;           /* last chunk (trailers ignored) */
            /* read csz bytes + trailing CRLF */
            while (rx.len < cur + csz + 2) {
                rc = rx_fill(&rx, &io, 1000u);
                if (rc == KN_HTTP_ERR_TIMEOUT) {
                    if (now_ms(now_ctx) >= deadline) return KN_HTTP_ERR_TIMEOUT;
                    continue;
                }
                if (rc != KN_HTTP_OK) return rc;
            }
            if (o + csz + 1 > bodycap) {
                size_t fit = bodycap - 1 - o;
                memcpy(bodybuf + o, rx.b + cur, fit);
                o = bodycap - 1;
                out->truncated = 1;
            } else {
                memcpy(bodybuf + o, rx.b + cur, csz);
                o += csz;
            }
            cur += csz + 2;
        }
    } else if (content_length >= 0) {
        size_t want = (size_t)content_length;
        while (rx.len < hend + want) {
            rc = rx_fill(&rx, &io, 1000u);
            if (rc == KN_HTTP_ERR_TIMEOUT) {
                if (now_ms(now_ctx) >= deadline) return KN_HTTP_ERR_TIMEOUT;
                continue;
            }
            if (rc == KN_HTTP_ERR_EOF) break;   /* short body: honest truncation */
            if (rc != KN_HTTP_OK) return rc;
        }
        {
            size_t avail = rx.len - hend;
            if (avail < want) out->truncated = 1;
            if (avail > want) avail = want;
            if (avail + 1 > bodycap) { avail = bodycap - 1;
                                       out->truncated = 1; }
            memcpy(bodybuf, rx.b + hend, avail);
            o = avail;
        }
    } else {
        /* read-to-EOF framing (Connection: close, no CL) */
        for (;;) {
            rc = rx_fill(&rx, &io, 1000u);
            if (rc == KN_HTTP_ERR_EOF) break;
            if (rc == KN_HTTP_ERR_TIMEOUT) {
                if (now_ms(now_ctx) >= deadline) return KN_HTTP_ERR_TIMEOUT;
                continue;
            }
            if (rc == KN_HTTP_ERR_BIG) break;   /* rx full: take what we have */
            if (rc != KN_HTTP_OK) return rc;
        }
        {
            size_t avail = rx.len - hend;
            if (avail + 1 > bodycap) { avail = bodycap - 1;
                                       out->truncated = 1; }
            memcpy(bodybuf, rx.b + hend, avail);
            o = avail;
        }
    }
    out->body_len = o;
    bodybuf[o] = '\0';        /* o <= bodycap-1 by construction above */
    return KN_HTTP_OK;
}
