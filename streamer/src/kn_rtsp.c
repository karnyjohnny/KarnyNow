/* kn_rtsp.c — RTSP-over-WebSocket client (see kn_rtsp.h + docs/RTSP.md;
 * every behaviour ported from nvst_rtsp.rs @ a49b124, line refs in the
 * document). Pure: injected message transport, clock and sleep. */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_rtsp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ helpers */

static char rt_lower(char a)
{
    return (a >= 'A' && a <= 'Z') ? (char)(a + 32) : a;
}

static int rt_ci_eqn(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (rt_lower(a[i]) != rt_lower(b[i])) return 0;
    return 1;
}

/* ------------------------------------------------------------- client */

int kn_rtsp_client_init(kn_rtsp_client *c, kn_rtsp_msg_io io,
                        char *buf, size_t buf_cap, uint32_t timeout_ms,
                        uint64_t (*now_ms)(void *), void *now_ctx,
                        void (*sleep_ms)(uint32_t, void *), void *sleep_ctx)
{
    if (!c || !io.send_text || !io.recv_text || !buf ||
        buf_cap < KN_RTSP_MAX_RESPONSE || !now_ms)
        return KN_RTSP_ERR_ARGS;
    memset(c, 0, sizeof *c);
    c->io = io;
    c->buf = buf;
    c->buf_cap = buf_cap;
    c->timeout_ms = timeout_ms ? timeout_ms : KN_RTSP_TIMEOUT_MS;
    c->now_ms = now_ms;
    c->now_ctx = now_ctx;
    c->sleep_ms = sleep_ms;
    c->sleep_ctx = sleep_ctx;
    return KN_RTSP_OK;
}

int kn_rtsp_serialize(char *out, size_t outsz, const char *method,
                      const char *uri, uint64_t cseq,
                      const kn_rtsp_header *hdrs, int nhdrs,
                      const char *body, size_t body_len, size_t *out_len)
{
    size_t o = 0;
    int i, n;
    if (!out || !method || !uri || (nhdrs && !hdrs) || (body_len && !body) ||
        !out_len)
        return KN_RTSP_ERR_ARGS;
    n = snprintf(out, outsz, "%s %s RTSP/1.0\r\nCSeq: %llu\r\n"
                 "Request-Id: %llu\r\n", method, uri,
                 (unsigned long long)(cseq % 1000000000000ull),
                 (unsigned long long)(cseq % 1000000000000ull));
    if (n < 0 || (size_t)n >= outsz) return KN_RTSP_ERR_BIG;
    o = (size_t)n;
    for (i = 0; i < nhdrs; ++i) {
        n = snprintf(out + o, outsz - o, "%s: %s\r\n", hdrs[i].name,
                     hdrs[i].value);
        if (n < 0 || o + (size_t)n >= outsz) return KN_RTSP_ERR_BIG;
        o += (size_t)n;
    }
    if (body_len) {
        n = snprintf(out + o, outsz - o, "Content-Length: %lu\r\n",
                     (unsigned long)(body_len % 100000000ul));
        if (n < 0 || o + (size_t)n >= outsz) return KN_RTSP_ERR_BIG;
        o += (size_t)n;
    }
    if (o + 3 > outsz) return KN_RTSP_ERR_BIG;
    out[o++] = '\r';
    out[o++] = '\n';
    if (body_len) {
        if (o + body_len + 1 > outsz) return KN_RTSP_ERR_BIG;
        memcpy(out + o, body, body_len);
        o += body_len;
    }
    out[o] = '\0';
    *out_len = o;
    return KN_RTSP_OK;
}

/* End-of-line index (excluding the \n) for the line starting at s. */
static size_t rt_line_end(const char *d, size_t s, size_t e)
{
    while (s < e && d[s] != '\n') s++;
    return s;
}

/* Line-span iterator over the header block [start,hend): yields each
 * line's index span [li_s, li_e) without the terminating \n.
 * (Round-XX self-review: the first draft nested two for-loops, which ran
 * the body per CHARACTER — invisible on CRLF fixtures, fatal on LF-only
 * ones. Single-loop form, the LF-only KAT caught it.) */
#define RT_FOR_LINES(li_s, li_e, start, hend)                        \
    for ((li_s) = (start),                                           \
         (li_e) = rt_line_end(data, (li_s), (hend));                 \
         (li_s) < (hend);                                            \
         (li_s) = ((li_e) < (hend) ? (li_e) + 1 : (hend)),           \
         (li_e) = rt_line_end(data, (li_s), (hend)))

int kn_rtsp_parse(const char *data, size_t len, uint64_t expect_cseq,
                  size_t cap, kn_rtsp_response *out, size_t *consumed)
{
    size_t start = 0, hend, sep, total, cl = 0;
    const char *hdr_end4 = NULL, *hdr_end2 = NULL;
    size_t li_s, li_e;
    int code, have_cseq = 0, have_rid = 0;
    uint64_t cseq_v = 0, rid_v = 0;

    if (consumed) *consumed = 0;
    if (!data || !out) return KN_RTSP_ERR_ARGS;

    /* leading CR/LF runs are transport padding (Bifrost quirk, :1809) —
     * reported as consumed even when the rest is still incomplete */
    while (start < len && (data[start] == '\r' || data[start] == '\n'))
        start++;
    if (consumed) *consumed = start;

    {
        size_t i;
        for (i = start; i + 1 < len; ++i) {
            if (!hdr_end4 && i + 3 < len &&
                memcmp(data + i, "\r\n\r\n", 4) == 0)
                hdr_end4 = data + i;
            if (!hdr_end2 && data[i] == '\n' && data[i + 1] == '\n')
                hdr_end2 = data + i;
            if (hdr_end4 || hdr_end2) break;
        }
    }
    if (!hdr_end4 && !hdr_end2) return KN_RTSP_MORE;
    if (hdr_end4 && (!hdr_end2 || hdr_end4 <= hdr_end2)) {
        hend = (size_t)(hdr_end4 - data);
        sep = 4;
    } else {
        hend = (size_t)(hdr_end2 - data);
        sep = 2;
    }

    /* Content-Length (case-insensitive; unparsable -> 0, the reference's
     * parse().ok().unwrap_or(0)) */
    RT_FOR_LINES(li_s, li_e, start, hend) {
        const char *colon = (const char *)memchr(data + li_s, ':',
                                                 li_e - li_s);
        char nm[32];
        size_t nl;
        if (!colon) continue;
        nl = (size_t)(colon - (data + li_s));
        while (nl && data[li_s + nl - 1] == ' ') nl--;
        if (nl != 14 || nl >= sizeof nm) continue;
        {
            size_t k;
            for (k = 0; k < nl; ++k) nm[k] = rt_lower(data[li_s + k]);
            nm[nl] = '\0';
        }
        if (strcmp(nm, "content-length") != 0) continue;
        {
            const char *v = colon + 1;
            while (v < data + li_e && *v == ' ') v++;
            cl = (size_t)strtoul(v, NULL, 10);
        }
        break;                              /* first match wins (find_map) */
    }

    total = hend + sep + cl;
    if (total < cl || total < hend) return KN_RTSP_ERR_PROTO;   /* wrap */
    if (total > cap) return KN_RTSP_ERR_BIG;
    if (len < total) return KN_RTSP_MORE;

    /* status line: "RTSP/1.0 <code> <text>" — the FIRST line only
     * (the reference's lines().next() + splitn 3 on ' '; searching the
     * whole header block would let a header's colon-space leak in —
     * caught by the GARBAGE KAT in test_knrtsp) */
    {
        size_t line_end = start;
        const char *sp1, *sp2;
        while (line_end < hend && data[line_end] != '\r' &&
               data[line_end] != '\n')
            line_end++;
        sp1 = (const char *)memchr(data + start, ' ', line_end - start);
        if (!sp1) return KN_RTSP_ERR_PROTO;
        sp2 = (const char *)memchr(sp1 + 1, ' ',
                                   (size_t)(data + line_end - (sp1 + 1)));
        code = atoi(sp1 + 1);
        if (code <= 0 || code > 999) return KN_RTSP_ERR_PROTO;
        memset(out, 0, sizeof *out);
        out->status = code;
        if (sp2) {
            size_t tl = (size_t)(data + line_end - (sp2 + 1));
            if (tl >= sizeof out->status_text)
                tl = sizeof out->status_text - 1;
            memcpy(out->status_text, sp2 + 1, tl);
            while (tl && (out->status_text[tl - 1] == '\r' ||
                          out->status_text[tl - 1] == ' ')) tl--;
            out->status_text[tl] = '\0';
        }
    }

    /* header lines: lower-cased names, trimmed values, LAST wins
     * (the reference's HashMap insert) */
    /* Header lines. The status line is included in the iteration and
     * skipped by the no-colon rule ("RTSP/1.0 200 OK" has none). */
    RT_FOR_LINES(li_s, li_e, start, hend) {
        const char *colon;
        char nm[KN_RTSP_HDR_NAME];
        size_t nl, vl, k, lstart = li_s;
        const char *v;
        int slot = -1;
        while (lstart < li_e && data[lstart] == ' ') lstart++;
        colon = (const char *)memchr(data + lstart, ':', li_e - lstart);
        if (!colon) continue;
        nl = (size_t)(colon - (data + lstart));
        while (nl && data[lstart + nl - 1] == ' ') nl--;
        if (nl == 0 || nl >= sizeof nm) continue;
        for (k = 0; k < nl; ++k) nm[k] = rt_lower(data[lstart + k]);
        nm[nl] = '\0';
        v = colon + 1;
        vl = (size_t)((data + li_e) - v);
        while (vl && *v == ' ') { v++; vl--; }
        while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
        for (k = 0; (int)k < out->hdr_count; ++k)
            if (strcmp(out->names[k], nm) == 0) { slot = (int)k; break; }
        if (slot < 0) {
            if (out->hdr_count >= KN_RTSP_MAX_HEADERS) continue;
            slot = out->hdr_count++;
            memcpy(out->names[slot], nm, nl + 1);
        }
        if (vl + 1 <= sizeof out->values[slot]) {
            memcpy(out->values[slot], v, vl);
            out->values[slot][vl] = '\0';
        } else {
            out->values[slot][0] = '\0';      /* overlong: honest empty */
        }
        if (strcmp(nm, "cseq") == 0) {
            char *endp = NULL;
            unsigned long long pv = strtoull(out->values[slot], &endp, 10);
            have_cseq = (endp && endp != out->values[slot] && *endp == '\0')
                            ? 1 : 2;
            cseq_v = (uint64_t)pv;
        } else if (strcmp(nm, "request-id") == 0) {
            char *endp = NULL;
            unsigned long long pv = strtoull(out->values[slot], &endp, 10);
            have_rid = (endp && endp != out->values[slot] && *endp == '\0')
                           ? 1 : 2;
            rid_v = (uint64_t)pv;
        }
    }

    /* sequence match (:1895-1913): CSeq when present (an unparsable CSeq
     * can never match), else the Request-Id fallback */
    {
        int matches;
        if (have_cseq) matches = (have_cseq == 1 && cseq_v == expect_cseq);
        else           matches = (have_rid == 1 && rid_v == expect_cseq);
        if (!matches) {
            if (consumed) *consumed = total;  /* the reference DRAINS, then
                                                 errors — TEARDOWN skips */
            return KN_RTSP_ERR_SEQ;
        }
    }

    out->body = data + hend + sep;
    out->body_len = cl;
    if (consumed) *consumed = total;
    return KN_RTSP_OK;
}

const char *kn_rtsp_header_get(const kn_rtsp_response *r, const char *name)
{
    int i;
    char nm[KN_RTSP_HDR_NAME];
    size_t k, n;
    if (!r || !name) return NULL;
    n = strlen(name);
    if (n >= sizeof nm) return NULL;
    for (k = 0; k < n; ++k) nm[k] = rt_lower(name[k]);
    nm[n] = '\0';
    for (i = 0; i < r->hdr_count; ++i)
        if (strcmp(r->names[i], nm) == 0)
            return r->values[i];
    return NULL;
}

int kn_rtsp_request(kn_rtsp_client *c, const char *method, const char *uri,
                    const kn_rtsp_header *hdrs, int nhdrs,
                    const char *body, size_t body_len, int skip_seq_mismatch,
                    kn_rtsp_response *out)
{
    uint64_t deadline, now;
    size_t req_len = 0;
    int rc;

    if (!c || !method || !uri || !out) return KN_RTSP_ERR_ARGS;
    c->err[0] = '\0';
    deadline = c->now_ms(c->now_ctx) + c->timeout_ms;

    c->cseq++;
    c->buf_len = 0;
    rc = kn_rtsp_serialize(c->buf, c->buf_cap, method, uri, c->cseq,
                           hdrs, nhdrs, body, body_len, &req_len);
    if (rc != KN_RTSP_OK) {
        snprintf(c->err, sizeof c->err, "%.16s: serialize failed (%d)",
                 method, rc);
        return rc;
    }
    if (c->io.send_text(c->io.ctx, c->buf, req_len) != 0) {
        snprintf(c->err, sizeof c->err, "%.16s: transport send failed",
                 method);
        return KN_RTSP_ERR_IO;
    }
    c->requests++;
    c->buf_len = 0;

    for (;;) {
        size_t consumed = 0;
        now = c->now_ms(c->now_ctx);
        if (c->buf_len > KN_RTSP_MAX_RESPONSE) {
            snprintf(c->err, sizeof c->err,
                     "%.16s: response exceeds 4 MiB", method);
            return KN_RTSP_ERR_BIG;
        }
        if (now >= deadline) {
            c->timeouts++;
            snprintf(c->err, sizeof c->err, "%.16s: timed out", method);
            return KN_RTSP_ERR_TIMEOUT;
        }
        rc = kn_rtsp_parse(c->buf, c->buf_len, c->cseq,
                           KN_RTSP_MAX_RESPONSE, out, &consumed);
        if (rc == KN_RTSP_OK) {
            /* Drain the consumed prefix. The body ALIASES c->buf and stays
             * valid: the memmove only shifts the TAIL forward, the
             * consumed region (where the body lives) is untouched. */
            if (consumed < c->buf_len)
                memmove(c->buf, c->buf + consumed, c->buf_len - consumed);
            c->buf_len -= consumed;
            c->responses++;
            return KN_RTSP_OK;
        }
        if (rc == KN_RTSP_ERR_SEQ) {
            if (consumed && consumed <= c->buf_len) {
                memmove(c->buf, c->buf + consumed, c->buf_len - consumed);
                c->buf_len -= consumed;
            }
            if (!skip_seq_mismatch) {
                snprintf(c->err, sizeof c->err,
                         "%.16s: sequence mismatch (cseq %llu)", method,
                         (unsigned long long)(c->cseq % 1000000ull));
                return KN_RTSP_ERR_SEQ;
            }
            c->seq_mismatch_skips++;
            continue;                       /* TEARDOWN semantics (:593) */
        }
        if (rc == KN_RTSP_MORE) {
            if (consumed && consumed <= c->buf_len) {
                memmove(c->buf, c->buf + consumed, c->buf_len - consumed);
                c->buf_len -= consumed;      /* padding dropped early */
            }
        } else if (rc < 0) {
            snprintf(c->err, sizeof c->err, "%.16s: parse error %d",
                     method, rc);
            return rc;
        }
        {   /* wait for the next message with the remaining time */
            char scratch[8192];
            size_t got = 0;
            uint64_t remain = deadline - now;
            uint32_t wait_ms = remain > 60000ull ? 60000u
                                                 : (uint32_t)remain;
            int rrc = c->io.recv_text(c->io.ctx, scratch,
                                      sizeof scratch - 1, wait_ms, &got);
            if (rrc == KN_RTSP_ERR_TIMEOUT || (rrc == 0 && got == 0))
                continue;                   /* the deadline check exits */
            if (rrc < 0) {
                snprintf(c->err, sizeof c->err,
                         "%.16s: transport recv failed", method);
                return KN_RTSP_ERR_IO;
            }
            if (c->buf_len + got + 1 > c->buf_cap) {
                snprintf(c->err, sizeof c->err,
                         "%.16s: accumulation overflow", method);
                return KN_RTSP_ERR_BIG;
            }
            memcpy(c->buf + c->buf_len, scratch, got);
            c->buf_len += got;
            c->buf[c->buf_len] = '\0';
        }
    }
}

/* ================================================================ SDP */

/* Line-span iteration over an SDP region [base, end) with \n breaks;
 * each line is trimmed of ' ', '\t', '\r'. */
#define SDP_FOR_LINES(base, end, ls, le)                          \
    for ((ls) = (base), (le) = next_line((ls), (end));            \
         (ls) < (end);                                            \
         (ls) = (le), (le) = next_line((ls), (end)))

static const char *next_line(const char *p, const char *end)
{
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    return nl ? nl + 1 : end;
}

static void trim_span(const char **s, const char **e)
{
    while (*s < *e && (**s == ' ' || **s == '\t' || **s == '\r' ||
                       **s == '\n'))
        (*s)++;
    while (*e > *s && ((*e)[-1] == ' ' || (*e)[-1] == '\t' ||
                       (*e)[-1] == '\r' || (*e)[-1] == '\n'))
        (*e)--;
}

/* Prefix match, case-insensitive, on a trimmed line span. */
static int span_has_prefix_ci(const char *s, const char *e, const char *pref,
                              size_t *out_rest)
{
    size_t n = strlen(pref), i;
    if ((size_t)(e - s) <= n) return 0;
    for (i = 0; i < n; ++i)
        if (rt_lower(s[i]) != rt_lower(pref[i])) return 0;
    *out_rest = n;
    return 1;
}

int kn_rtsp_sdp_attribute(const char *sdp, const char *name,
                          char *out, size_t outsz)
{
    char p1[192], p2[192];
    const char *seg_end, *semi, *seg_lines_end, *ls, *le;
    int n1, n2, pi;

    if (!sdp || !name || !out || outsz == 0) return KN_RTSP_ERR_ARGS;
    out[0] = '\0';
    n1 = snprintf(p1, sizeof p1, "a=x-nv-%s:", name);
    n2 = snprintf(p2, sizeof p2, "a=%s:", name);
    if (n1 < 0 || n2 < 0 || (size_t)n1 >= sizeof p1 || (size_t)n2 >= sizeof p2)
        return KN_RTSP_ERR_ARGS;

    {   const char *bars = strstr(sdp, "||");
        seg_end = bars ? bars : sdp + strlen(sdp);
    }
    /* the reference flattens ALL ";;" segments of the first "||" part */
    semi = sdp;
    for (;;) {
        const char *ss = (const char *)memchr(semi, ';',
                                              (size_t)(seg_end - semi));
        if (ss && ss + 1 < seg_end && ss[1] == ';') seg_lines_end = ss;
        else seg_lines_end = seg_end;

        SDP_FOR_LINES(semi, seg_lines_end, ls, le) {
            const char *s0 = ls, *e0 = le;
            size_t rest;
            trim_span(&s0, &e0);
            if (s0 >= e0) continue;   /* blank line: the reference's
                                         lines() just yields it; no prefix
                                         matches, iteration continues */
            for (pi = 0; pi < 2; ++pi) {
                const char *pref = pi ? p2 : p1;
                if (!span_has_prefix_ci(s0, e0, pref, &rest)) continue;
                {
                    const char *v = s0 + rest;
                    size_t vl;
                    const char *ve = e0;
                    while (v < ve && (*v == ' ' || *v == '\t')) v++;
                    vl = (size_t)(ve - v);
                    if (vl == 0) break;               /* empty = not found */
                    if (vl + 1 > outsz) break;        /* honest refusal */
                    memcpy(out, v, vl);
                    out[vl] = '\0';
                    return KN_RTSP_OK;
                }
            }
        }
        if (seg_lines_end == seg_end) break;
        semi = seg_lines_end + 2;                     /* skip the ";;" */
        if (semi >= seg_end) break;
    }
    return -1;
}

int kn_rtsp_media_control(const char *sdp, const char *kind,
                          char *out, size_t outsz)
{
    const char *seg_end, *semi, *lines_end, *ls, *le;
    int in_kind = 0;

    if (!sdp || !kind || !out || outsz == 0) return KN_RTSP_ERR_ARGS;
    out[0] = '\0';
    {   const char *bars = strstr(sdp, "||");
        seg_end = bars ? bars : sdp + strlen(sdp);
    }
    {   semi = (const char *)memchr(sdp, ';', (size_t)(seg_end - sdp));
        if (semi && semi + 1 < seg_end && semi[1] == ';') lines_end = semi;
        else lines_end = seg_end;
    }
    SDP_FOR_LINES(sdp, lines_end, ls, le) {
        const char *s0 = ls, *e0 = le;
        size_t rest;
        trim_span(&s0, &e0);
        if (s0 >= e0) continue;   /* blank line: skipped, not fatal */
        if (span_has_prefix_ci(s0, e0, "m=", &rest)) {
            const char *w = s0 + rest;
            const char *we = (const char *)memchr(w, ' ', (size_t)(e0 - w));
            size_t wl = we ? (size_t)(we - w) : (size_t)(e0 - w);
            in_kind = (wl == strlen(kind)) && rt_ci_eqn(w, kind, wl);
        } else if (in_kind &&
                   span_has_prefix_ci(s0, e0, "a=control:", &rest)) {
            const char *v = s0 + rest;
            size_t vl = (size_t)(e0 - v);
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (vl && !(vl == 1 && v[0] == '*') && vl + 1 <= outsz) {
                memcpy(out, v, vl);
                out[vl] = '\0';
                return KN_RTSP_OK;
            }
        }
    }
    return -1;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int kn_rtsp_runtime_key(const char *sdp, char key65[65], uint32_t *key_id)
{
    char val[128];
    size_t i;
    long long id;
    char *endp;

    if (!sdp || !key65 || !key_id) return KN_RTSP_ERR_ARGS;
    key65[0] = '\0';
    *key_id = 0;
    if (kn_rtsp_sdp_attribute(sdp, "runtime.encryptionKey", val,
                              sizeof val) != KN_RTSP_OK)
        return -1;
    if (strlen(val) != 64) return -1;
    for (i = 0; i < 64; ++i)
        if (hex_value(val[i]) < 0) return -1;
    if (kn_rtsp_sdp_attribute(sdp, "runtime.encryptionKeyId", val,
                              sizeof val) != KN_RTSP_OK)
        return -1;
    id = strtoll(val, &endp, 10);
    if (endp == val || *endp != '\0') return -1;    /* parse::<i64>().ok() */
    /* re-read the key (val was overwritten by the id) */
    if (kn_rtsp_sdp_attribute(sdp, "runtime.encryptionKey", key65, 65)
        != KN_RTSP_OK)
        return -1;
    for (i = 0; i < 64; ++i) {
        char c = key65[i];
        if (c >= 'a' && c <= 'f') key65[i] = (char)(c - 32);   /* uppercased */
    }
    *key_id = (uint32_t)id;
    return KN_RTSP_OK;
}

void kn_rtsp_salt_from_key_id(uint32_t key_id, char out25[25])
{
    if (!out25) return;
    snprintf(out25, 25, "%024X", (unsigned)key_id);
}

static const char *const k_srtp_profiles[8] = {
    "AEAD_AES_128_GCM_8",
    "AEAD_AES_256_GCM_8",
    "AEAD_AES_128_GCM",
    "AEAD_AES_256_GCM",
    "AES_CM_128_HMAC_SHA1_32",
    "AES_CM_128_HMAC_SHA1_80",
    "AES_CM_256_HMAC_SHA1_32",
    "AES_CM_256_HMAC_SHA1_80"
};

/* Scan one value: the FIRST profile (in the reference's priority order)
 * contained in the uppercased value. */
static const char *profile_in(const char *value)
{
    char up[1024];
    size_t i, n = strlen(value);
    int p;
    if (n >= sizeof up) n = sizeof up - 1;     /* SDP lines are short; a
                                                  longer value cannot hide
                                                  a profile past 1 KiB */
    for (i = 0; i < n; ++i)
        up[i] = (value[i] >= 'a' && value[i] <= 'z') ? (char)(value[i] - 32)
                                                     : value[i];
    up[n] = '\0';
    for (p = 0; p < 8; ++p)
        if (strstr(up, k_srtp_profiles[p]))
            return k_srtp_profiles[p];
    return NULL;
}

const char *kn_rtsp_srtp_profile(const kn_rtsp_response *setup,
                                 const char *describe_sdp)
{
    int i;
    if (setup) {
        for (i = 0; i < setup->hdr_count; ++i) {
            const char *nm = setup->names[i];
            const char *hit;
            if (strcmp(nm, "transport") != 0 &&
                !strstr(nm, "srtp") && !strstr(nm, "crypto"))
                continue;
            hit = profile_in(setup->values[i]);
            if (hit) return hit;
        }
    }
    if (describe_sdp) {
        const char *l = describe_sdp;
        while (*l) {
            const char *nl = strchr(l, '\n');
            char line[1024];
            size_t n = nl ? (size_t)(nl - l) : strlen(l);
            const char *hit;
            if (n >= sizeof line) n = sizeof line - 1;
            memcpy(line, l, n);
            line[n] = '\0';
            hit = profile_in(line);
            if (hit) return hit;
            if (!nl) break;
            l = nl + 1;
        }
    }
    return "AEAD_AES_256_GCM_8";               /* the reference default */
}

int kn_rtsp_parse_video_peer(const char *transport, char ip[64],
                             uint16_t *port, uint16_t *port_end)
{
    char buf[KN_RTSP_HDR_VALUE];
    char *tok;
    int have_ip = 0, have_port = 0;
    unsigned long pfirst = 0, plast = 0;

    if (!transport || !ip || !port || !port_end) return KN_RTSP_ERR_ARGS;
    ip[0] = '\0';
    *port = 0;
    *port_end = 0;
    snprintf(buf, sizeof buf, "%s", transport);
    /* strtok over ';' then ',' — the reference splits on BOTH at once;
     * normalize ',' to ';' first, then one strtok pass */
    for (tok = buf; *tok; ++tok)
        if (*tok == ',') *tok = ';';
    tok = strtok(buf, ";");
    while (tok) {
        char *eq;
        while (*tok == ' ') tok++;
        eq = strchr(tok, '=');
        if (eq) {
            size_t nl = (size_t)(eq - tok);
            const char *v = eq + 1;
            size_t vl = strlen(v);
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r')) vl--;
            if (nl == 6 && rt_ci_eqn(tok, "source", 6)) {
                if (vl && vl < 64) {
                    memcpy(ip, v, vl);
                    ip[vl] = '\0';
                    have_ip = 1;
                }
            } else if (nl == 15 && rt_ci_eqn(tok, "x-gs-serverport", 15)) {
                const char *dash = (const char *)memchr(v, '-', vl);
                char *endp = NULL;
                pfirst = strtoul(v, &endp, 10);
                if (endp != (const char *)(dash ? dash : v + vl)) {
                    have_port = 0;
                }
                else {
                    have_port = pfirst != 0;
                    if (dash) {
                        unsigned long l = strtoul(dash + 1, &endp, 10);
                        if (endp == (const char *)(v + vl) && l >= pfirst &&
                            l - pfirst < KN_RTSP_MAX_PEER_SPAN)
                            plast = l;
                        else
                            plast = pfirst;
                    } else {
                        plast = pfirst;
                    }
                }
            }
        }
        tok = strtok(NULL, ";");
    }
    if (!have_ip || !have_port || !ip[0]) return -1;
    *port = (uint16_t)pfirst;
    *port_end = (uint16_t)(plast ? plast : pfirst);
    return KN_RTSP_OK;
}

void kn_rtsp_official_control(const char *control, char *out, size_t outsz)
{
    size_t i, slashes = 0, n;
    int prefixed;
    if (!control || !out || outsz == 0) return;
    out[0] = '\0';
    n = strlen(control);
    prefixed = (n >= 15) && rt_ci_eqn(control, "streamid=video/", 15);
    for (i = 0; i < n; ++i)
        if (control[i] == '/') slashes++;
    if (prefixed && slashes == 1 && n + 3 <= outsz) {
        memcpy(out, control, n);          /* bounds proven by hand — no
                                             snprintf here keeps even the
                                             paranoid =2 level silent */
        out[n] = '/';
        out[n + 1] = '0';
        out[n + 2] = '\0';
    } else if (n + 1 <= outsz) {
        memcpy(out, control, n + 1);
    }
}

int kn_rtsp_setup_candidates(const char *control, const char *target,
                             char cand[][KN_RTSP_URI_MAX], int max)
{
    int n = 0, i, j;
    char off[KN_RTSP_URI_MAX];
    if (!control || !target || !cand || max < 1) return 0;
    kn_rtsp_official_control(control, off, sizeof off);
    snprintf(cand[n++], KN_RTSP_URI_MAX, "%s", off);
    if (strcmp(off, control) != 0 && n < max)
        snprintf(cand[n++], KN_RTSP_URI_MAX, "%s", control);
    {
        int base = n;                       /* the reference iterates the
                                               entries that existed before
                                               the absolute-form loop */
        for (i = 0; i < base && n < max; ++i) {
            char abs_[KN_RTSP_URI_MAX];
            size_t tl = strlen(target), cl2;
            const char *cstart = cand[i];
            int is_abs = (rt_ci_eqn(cand[i], "rtsps://", 8) ||
                          rt_ci_eqn(cand[i], "rtsp://", 7));
            if (is_abs) continue;
            while (tl && target[tl - 1] == '/') tl--;
            while (*cstart == '/') cstart++;
            cl2 = strlen(cstart);
            if (tl + 1 + cl2 + 1 > sizeof abs_) continue;
            memcpy(abs_, target, tl);
            abs_[tl] = '/';
            memcpy(abs_ + tl + 1, cstart, cl2 + 1);
            for (j = 0; j < n; ++j)
                if (strcmp(cand[j], abs_) == 0) break;
            if (j == n) snprintf(cand[n++], KN_RTSP_URI_MAX, "%s", abs_);
        }
    }
    return n;
}

/* ------------------------------------------------ DESCRIBE extraction */

static int attr_flag(const char *sdp, const char *name)
{
    char v[64];
    if (kn_rtsp_sdp_attribute(sdp, name, v, sizeof v) != KN_RTSP_OK)
        return 0;
    return strcmp(v, "1") == 0;              /* offered_bool: only "1" */
}

int kn_rtsp_extract_describe(const kn_rtsp_response *resp,
                             kn_rtsp_describe *out)
{
    const char *session;
    char v[256];
    int ok = 1;

    if (!resp || !out) return KN_RTSP_ERR_ARGS;
    memset(out, 0, sizeof *out);
    out->ping_version = 6;                   /* :1129 default */

    session = kn_rtsp_header_get(resp, "session");
    if (session && session[0]) {
        const char *semi = strchr(session, ';');
        size_t n = semi ? (size_t)(semi - session) : strlen(session);
        while (n && session[n - 1] == ' ') n--;
        if (n && n < sizeof out->session) {
            memcpy(out->session, session, n);
            out->session[n] = '\0';
        } else {
            ok = 0;
        }
    } else {
        ok = 0;
    }
    if (kn_rtsp_media_control(resp->body, "video", out->video_control,
                              sizeof out->video_control) != KN_RTSP_OK)
        ok = 0;

    if (kn_rtsp_sdp_attribute(resp->body, "general.pingVersion", v,
                              sizeof v) == KN_RTSP_OK) {
        unsigned long pv = strtoul(v, NULL, 10);
        if (pv > 0 && pv <= 255) out->ping_version = (uint8_t)pv;
    }
    if (kn_rtsp_sdp_attribute(resp->body, "general.iceUserNameFragmentV2",
                              out->remote_ufrag,
                              sizeof out->remote_ufrag) != KN_RTSP_OK)
        (void)kn_rtsp_sdp_attribute(resp->body, "general.iceUsernameFragment",
                                    out->remote_ufrag,
                                    sizeof out->remote_ufrag);
    if (kn_rtsp_sdp_attribute(resp->body, "general.icePasswordV2",
                              out->remote_password,
                              sizeof out->remote_password) != KN_RTSP_OK)
        (void)kn_rtsp_sdp_attribute(resp->body, "general.iceUsernamePwd",
                                    out->remote_password,
                                    sizeof out->remote_password);
    if (kn_rtsp_sdp_attribute(resp->body, "general.dtlsFingerprintV2",
                              out->remote_fp,
                              sizeof out->remote_fp) != KN_RTSP_OK)
        (void)kn_rtsp_sdp_attribute(resp->body, "general.dtlsFingerprint",
                                    out->remote_fp, sizeof out->remote_fp);
    out->disable_play = attr_flag(resp->body, "general.disablePlay");
    out->native_bundle = attr_flag(resp->body, "general.nativeRtcOnBundlePort");
    out->rtcp_on_sctp = attr_flag(resp->body, "general.rtcpOnSctp");
    out->unified_socket = attr_flag(resp->body, "general.enableUnifiedSocket");
    if (kn_rtsp_runtime_key(resp->body, out->runtime_key, &out->key_id)
        == KN_RTSP_OK)
        out->have_runtime_key = 1;
    return ok ? KN_RTSP_OK : -1;
}

/* ------------------------------------------------------- SETUP sweep */

/* Minimal IP literal validation (the reference filters through
 * IpAddr::parse): dotted-quad with every octet <= 255, or a colon-bearing
 * string of hex/digit/colon/dot characters (IPv6 shape — full validation
 * is the socket layer's job when it connects). */
static int ip_literal_ok(const char *ip)
{
    size_t i, dots = 0, digits = 0;
    if (!ip || !ip[0]) return 0;
    if (strchr(ip, ':')) {
        for (i = 0; ip[i]; ++i)
            if (!strchr("0123456789abcdefABCDEF:.", ip[i])) return 0;
        return 1;
    }
    {
        unsigned long cur = 0;
        for (i = 0;; ++i) {
            char c = ip[i];
            if (c == '.' ) {
                if (cur > 255 || digits == 0) return 0;
                dots++; digits = 0; cur = 0;
                if (!ip[i + 1]) return 0;
            } else if (c >= '0' && c <= '9') {
                cur = cur * 10 + (unsigned long)(c - '0');
                digits++;
                if (digits > 3) return 0;
            } else if (c == '\0') {
                if (cur > 255 || digits == 0) return 0;
                break;
            } else {
                return 0;
            }
        }
    }
    return dots == 3;
}

static int setup_sweep(kn_rtsp_client *c, const char cand[][KN_RTSP_URI_MAX],
                       int ncand, kn_rtsp_header *hdrs, int nhdrs,
                       int tidx, uint16_t client_port, uint64_t deadline,
                       kn_rtsp_setup_result *out, int *peerless)
{
    int form, i, rc;
    char tval[64];

    *peerless = 0;
    for (form = 0; form < 2; ++form) {
        if (form == 0) {
            hdrs[tidx].value = "";
        } else {
            snprintf(tval, sizeof tval,
                     "unicast;X-GS-ClientPort=%u-%u",
                     (unsigned)client_port,
                     (unsigned)(client_port == 65535 ? 65535
                                                     : client_port + 1));
            hdrs[tidx].value = tval;
        }
        for (i = 0; i < ncand; ++i) {
            kn_rtsp_response resp;
            const char *tp;
            char pip[64];
            uint16_t p1 = 0, p2 = 0;
            {
                uint64_t now = c->now_ms(c->now_ctx);
                uint32_t save_to;
                uint64_t rem;
                if (now >= deadline) {
                    snprintf(c->err, sizeof c->err, "SETUP: timed out");
                    return KN_RTSP_ERR_TIMEOUT;
                }
                rem = deadline - now;
                save_to = c->timeout_ms;
                if (rem < (uint64_t)save_to) c->timeout_ms = (uint32_t)rem;
                rc = kn_rtsp_request(c, "SETUP", cand[i], hdrs, nhdrs,
                                     NULL, 0, 0, &resp);
                c->timeout_ms = save_to;
            }
            if (rc != KN_RTSP_OK) return rc;
            out->status = resp.status;
            tp = kn_rtsp_header_get(&resp, "transport");
            if (tp) snprintf(out->transport, sizeof out->transport, "%s", tp);
            else out->transport[0] = '\0';
            if (resp.status == 200) {
                int peer_ok = tp &&
                              kn_rtsp_parse_video_peer(tp, pip, &p1, &p2)
                                  == KN_RTSP_OK &&
                              ip_literal_ok(pip);
                if (peer_ok) {
                    const char *pv = kn_rtsp_header_get(&resp, "x-nv-ping");
                    const char *pp =
                        kn_rtsp_header_get(&resp, "x-nv-ping-payload");
                    snprintf(out->peer_ip, sizeof out->peer_ip, "%s", pip);
                    out->peer_port = p1;
                    out->peer_port_end = p2;
                    out->bundle_fallback = 0;
                    if (pv) {
                        unsigned long v = strtoul(pv, NULL, 10);
                        if (v > 0 && v <= 255) out->ping_version = (uint8_t)v;
                    }
                    if (pp) {
                        snprintf(out->ping_payload, sizeof out->ping_payload,
                                 "%s", pp);
                        out->have_ping_payload = 1;
                    }
                    return KN_RTSP_OK;
                }
                /* peerless 200: STOP the sweep immediately (the alliance
                 * poison rule, :458-486) */
                *peerless = 1;
                return KN_RTSP_ERR_STATE;
            }
            if (resp.status == 400 || resp.status == 404 ||
                resp.status == 459 || resp.status == 460 ||
                resp.status == 461)
                continue;                   /* next candidate */
            snprintf(c->err, sizeof c->err,
                     "SETUP failed with status %d", resp.status % 1000);
            return KN_RTSP_ERR_PROTO;       /* any other status is fatal */
        }
    }
    snprintf(c->err, sizeof c->err,
             "SETUP: no usable peer after %d URI forms x 2 Transport forms",
             ncand % 1000);
    return KN_RTSP_ERR_STATE;
}

int kn_rtsp_setup_video(kn_rtsp_client *c, const char *control,
                        const char *target, const char *session,
                        uint8_t described_ping_version, uint16_t client_port,
                        const kn_rtsp_header *common, int ncommon,
                        const char *bundle_peer_ip, uint16_t bundle_peer_port,
                        kn_rtsp_setup_result *out)
{
    static char cand[6][KN_RTSP_URI_MAX];
    kn_rtsp_header hdrs[KN_RTSP_MAX_HEADERS];
    char sess_v[128], ping_v[8], tport_v[64];
    int ncand, nhdrs, tidx, rc, peerless = 0;
    unsigned round = 0;
    uint64_t deadline;

    if (!c || !control || !target || !session || !out)
        return KN_RTSP_ERR_ARGS;
    memset(out, 0, sizeof *out);
    out->ping_version = described_ping_version;
    ncand = kn_rtsp_setup_candidates(control, target, cand, 6);
    if (ncand == 0) return KN_RTSP_ERR_ARGS;
    if (ncommon < 0 || ncommon + 3 > KN_RTSP_MAX_HEADERS)
        return KN_RTSP_ERR_ARGS;

    nhdrs = 0;
    for (; nhdrs < ncommon; ++nhdrs) hdrs[nhdrs] = common[nhdrs];
    snprintf(sess_v, sizeof sess_v, "%s", session);
    hdrs[nhdrs].name = "Session"; hdrs[nhdrs].value = sess_v; nhdrs++;
    snprintf(ping_v, sizeof ping_v, "%u", (unsigned)described_ping_version);
    hdrs[nhdrs].name = "x-nv-ping"; hdrs[nhdrs].value = ping_v; nhdrs++;
    tidx = nhdrs;
    hdrs[nhdrs].name = "Transport"; hdrs[nhdrs].value = tport_v; nhdrs++;
    tport_v[0] = '\0';

    deadline = c->now_ms(c->now_ctx) + c->timeout_ms;
    for (;;) {
        rc = setup_sweep(c, (const char (*)[KN_RTSP_URI_MAX])cand, ncand,
                         hdrs, nhdrs, tidx, client_port, deadline, out,
                         &peerless);
        if (rc == KN_RTSP_OK) return KN_RTSP_OK;
        if (peerless && bundle_peer_ip && ip_literal_ok(bundle_peer_ip) &&
            bundle_peer_port) {
            /* the CloudMatch bundle peer doubles as the video peer
             * (:1166-1174 + :345-357) */
            snprintf(out->peer_ip, sizeof out->peer_ip, "%s", bundle_peer_ip);
            out->peer_port = bundle_peer_port;
            out->peer_port_end = bundle_peer_port;
            out->bundle_fallback = 1;
            return KN_RTSP_OK;
        }
        if (rc != KN_RTSP_ERR_STATE) return rc;   /* IO/timeout/proto fatal */
        if (round >= KN_RTSP_SETUP_ROUNDS) return rc;
        {
            uint64_t now = c->now_ms(c->now_ctx);
            uint64_t remaining;
            uint32_t nap;
            if (now >= deadline) {
                snprintf(c->err, sizeof c->err,
                         "SETUP: peer retry window exhausted");
                return KN_RTSP_ERR_TIMEOUT;
            }
            remaining = deadline - now;
            nap = remaining < KN_RTSP_SETUP_DELAY_MS
                      ? (uint32_t)remaining : KN_RTSP_SETUP_DELAY_MS;
            if (c->sleep_ms) c->sleep_ms(nap, c->sleep_ctx);
            round++;
            out->rounds_slept = round;
        }
    }
}

/* --------------------------------------------------------- ANNOUNCE */

typedef struct {
    char *out;
    size_t cap, len;
    int overflow;
} ann_ctx;

static void ann_line(ann_ctx *a, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (a->overflow) return;
    if (a->len && a->len + 2 <= a->cap) {
        a->out[a->len++] = '\r';
        a->out[a->len++] = '\n';
    }
    va_start(ap, fmt);
    n = vsnprintf(a->out + a->len, a->cap - a->len, fmt, ap);
    va_end(ap);
    if (n < 0 || a->len + (size_t)n + 1 > a->cap) { a->overflow = 1; return; }
    a->len += (size_t)n;
}

int kn_rtsp_build_announce(const kn_rtsp_announce_params *p,
                           char *out, size_t outsz, size_t *out_len)
{
    ann_ctx a;
    uint32_t bitrate, min_bitrate, max_delay;

    if (!p || !out || outsz < 4096 || !out_len) return KN_RTSP_ERR_ARGS;
    if (!p->key_hex[0] || !p->local_address[0]) return KN_RTSP_ERR_ARGS;
    a.out = out; a.cap = outsz; a.len = 0; a.overflow = 0;
    out[0] = '\0';

    bitrate = p->bitrate_kbps;
    min_bitrate = bitrate < 1000u ? bitrate : 1000u;
    max_delay = p->fps >= 100u ? 4000u : 2000u;

    ann_line(&a, "v=0");
    ann_line(&a, "o=unknown 0 14 IN IPv4 127.0.0.1");
    ann_line(&a, "s=NVIDIA Streaming Client");
    ann_line(&a, "a=x-nv-video[0].clientViewportWd:%u", (unsigned)p->width);
    ann_line(&a, "a=x-nv-video[0].clientViewportHt:%u", (unsigned)p->height);
    ann_line(&a, "a=x-nv-video[0].maxCodecProfile:3");
    ann_line(&a, "a=x-nv-video[0].maxCodecLevel:51");
    ann_line(&a, "a=x-nv-video[0].maxH264Profile:3");
    ann_line(&a, "a=x-nv-video[0].maxH264Level:51");
    ann_line(&a, "a=x-nv-video[0].updateSplitEncodeStateDynamically:1");
    ann_line(&a, "a=x-nv-video[0].packetSize:%u",
             (unsigned)p->video_packet_size);
    ann_line(&a, "a=x-nv-video[0].enableRtpNack:1");
    ann_line(&a, "a=x-nv-video[0].rtpNackQueueLength:2048");
    ann_line(&a, "a=x-nv-video[0].rtpNackQueueMaxPackets:1024");
    ann_line(&a, "a=x-nv-video[0].rtpNackMaxPacketCount:64");
    ann_line(&a, "a=x-nv-video[0].framePacing.mode:%d",
             p->qos_timings_v5 ? 2 : 1);
    ann_line(&a, "a=x-nv-video[0].framePacing.feedbackMode:%d",
             p->qos_timings_v5 ? 0 : 1);
    ann_line(&a, "a=x-nv-video[0].maxNumReferenceFrames:0");
    ann_line(&a, "a=x-nv-video[0].refPicInvalidation:1");
    ann_line(&a, "a=x-nv-general.clientSupportsIntraRefresh:1");
    ann_line(&a, "a=x-nv-video[0].prefilterParams.prefilterMode:0");
    ann_line(&a, "a=x-nv-video[0].prefilterParams.prefilterModel:4");
    ann_line(&a, "a=x-nv-video[0].prefilterParams.denoiseLevel:0");
    ann_line(&a, "a=x-nv-video[0].prefilterParams.sharpnessLevel:0");
    ann_line(&a, "a=x-nv-video[0].encoderCscMode:2");
    ann_line(&a, "a=x-nv-video[0].encoderHdrCscMode:4");
    ann_line(&a, "a=x-nv-video[0].mapRtpTimestampsToFrames:0");
    ann_line(&a, "a=x-nv-video[0].maxFPS:%u", (unsigned)p->fps);
    ann_line(&a, "a=x-nv-video[0].initialBitrateKbps:%u", (unsigned)bitrate);
    ann_line(&a, "a=x-nv-video[0].initialPeakBitrateKbps:%u",
             (unsigned)bitrate);
    ann_line(&a, "a=x-nv-vqos[0].bitStreamFormat:%d", p->bitstream_format);
    ann_line(&a, "a=x-nv-vqos[0].fec.enable:1");
    ann_line(&a, "a=x-nv-vqos[0].fec.rateDropWindow:10");
    ann_line(&a, "a=x-nv-vqos[0].fec.minRequiredFecPackets:2");
    ann_line(&a, "a=x-nv-vqos[0].fec.repairPercent:20");
    ann_line(&a, "a=x-nv-vqos[0].fec.repairMinPercent:20");
    ann_line(&a, "a=x-nv-vqos[0].fec.repairMaxPercent:35");
    ann_line(&a, "a=x-nv-vqos[0].bllFec.enable:0");
    ann_line(&a, "a=x-nv-vqos[0].drc.enable:0");
    ann_line(&a, "a=x-nv-vqos[0].dfc.adjustResAndFps:%d",
             p->adjust_res_and_fps ? 1 : 0);
    ann_line(&a, "a=x-nv-vqos[0].calculateAvgVideoStreamingBitrate:1");
    ann_line(&a, "a=x-nv-vqos[0].bw.maximumBitrateKbps:%u", (unsigned)bitrate);
    ann_line(&a, "a=x-nv-vqos[0].bw.minimumBitrateKbps:%u",
             (unsigned)min_bitrate);
    ann_line(&a, "a=x-nv-vqos[0].drc.bitrateIirFilterFactor:128");
    ann_line(&a, "a=x-nv-vqos[0].resControl.bitrateIirFilterFactor:128");
    ann_line(&a, "a=x-nv-vqos[0].dynamicStreamingMode:%u",
             (unsigned)p->dynamic_streaming_mode);
    ann_line(&a, "a=x-nv-packetPacing.version:3");
    ann_line(&a, "a=x-nv-packetPacing.mode:1");
    ann_line(&a, "a=x-nv-packetPacing.numGroups:5");
    ann_line(&a, "a=x-nv-packetPacing.maxDelayUs:%u", (unsigned)max_delay);
    ann_line(&a, "a=x-nv-packetPacing.minNumPacketsFrame:10");
    ann_line(&a, "a=x-nv-packetPacing.minNumPacketsPerGroup:15");
    ann_line(&a, "a=x-nv-packetPacing.enableAccurateSleep:1");
    ann_line(&a, "a=x-nv-packetPacing.enableSmoothTransition:1");
    ann_line(&a, "a=x-nv-packetPacing.allowFpsBasedToggle:1");
    ann_line(&a, "a=x-nv-ri.partialReliableThresholdMs:300");
    ann_line(&a, "a=x-nv-ri.timestampsEnabled:1");
    ann_line(&a, "a=x-nv-ri.useMultipleGamepads:1");
    ann_line(&a, "a=x-nv-ri.usePartiallyReliableUdpChannel:0");
    ann_line(&a, "a=x-nv-ri.enablePartiallyReliableTransferGamepad:255");
    ann_line(&a, "a=x-nv-ri.enablePartiallyReliableTransferHid:-1");
    ann_line(&a, "a=x-nv-bwe.useOwdCongestionControl:1");
    ann_line(&a, "a=x-nv-general.rtspWebSocketPerConnection:1");
    ann_line(&a, "a=x-nv-general.pingIntervalBeforeConnectionMs:20");
    ann_line(&a, "a=x-nv-general.pingIntervalAfterConnectionMs:100");
    ann_line(&a, "a=x-nv-runtime.audioSrtp:0");
    ann_line(&a, "a=x-nv-runtime.micSrtp:0");
    ann_line(&a, "a=x-nv-runtime.mouseCursorCapture:3");
    ann_line(&a, "a=x-nv-runtime.mimicRemoteCursor:0");
    ann_line(&a, "a=x-nv-runtime.videoSrtp:1");
    ann_line(&a, "a=x-nv-runtime.encryptionKey:%s", p->key_hex);
    ann_line(&a, "a=x-nv-runtime.encryptionKeyId:%u", (unsigned)p->key_id);
    ann_line(&a, "a=x-nv-general.clientPorts.video:0");
    ann_line(&a, "a=x-nv-general.clientPorts.audio:0");
    ann_line(&a, "a=x-nv-general.clientPorts.mic:0");
    ann_line(&a, "a=x-nv-general.clientPorts.control:0");
    ann_line(&a, "a=x-nv-general.clientPorts.bundle:0");
    ann_line(&a, "a=x-nv-general.clientPorts.session:0");
    ann_line(&a, "a=x-nv-general.clientPorts.localAddress:%s",
             p->local_address);
    ann_line(&a, "a=x-nv-general.clientPorts.useReserved:1");
    ann_line(&a, "a=x-nv-general.clientPorts.fallbackDynamic:1");
    ann_line(&a, "a=x-nv-general.clientBundlePort:%u",
             (unsigned)p->bundle_port);
    ann_line(&a, "a=x-nv-general.nativeRtcOnBundlePort:1");
    ann_line(&a, "a=x-nv-general.rtcVideoOnNativeBundle:0");
    ann_line(&a, "a=x-nv-general.rtcAudioOnNativeBundle:1");
    ann_line(&a, "a=x-nv-general.rtcDataChannelOnNativeBundle:1");
    ann_line(&a, "a=x-nv-general.enableUnifiedSocket:%d",
             p->unified_socket ? 1 : 0);
    ann_line(&a, "a=x-nv-general.rtcpOnSctp:%d", p->rtcp_on_sctp ? 1 : 0);
    ann_line(&a, "a=x-nv-general.iceUserNameFragmentV2:%s", p->ufrag);
    ann_line(&a, "a=x-nv-general.icePasswordV2:%s", p->password);
    ann_line(&a, "a=x-nv-general.dtlsFingerprintV2:%s", p->fingerprint);
    ann_line(&a, "a=ice-options:trickle");
    ann_line(&a, "a=ice-ufrag:%s", p->ufrag);
    ann_line(&a, "a=ice-pwd:%s", p->password);
    ann_line(&a, "a=fingerprint:sha-256 %s", p->fingerprint);
    ann_line(&a, "a=setup:actpass");
    ann_line(&a, "a=candidate:1 1 udp 2122260223 %s %u typ host",
             p->local_address, (unsigned)p->bundle_port);
    if (p->microphone_available) {
        ann_line(&a, "a=x-nv-general.rtcMicOnNativeBundle:1");
        ann_line(&a, "a=x-nv-mic.micSsrcConfig.senderSsrc:1");
    }
    /* announce_color_lines (nvst_rtsp_color.rs:15-28) */
    ann_line(&a, "a=x-nv-video[0].bitDepth:%d", p->bit_depth);
    ann_line(&a, "a=x-nv-video[0].chromaFormat:%d", p->chroma_format);
    if (p->hdr)
        ann_line(&a, "a=x-nv-video[0].dynamicRangeMode:1");
    ann_line(&a, "t=0 0");
    ann_line(&a, "m=video %u", (unsigned)p->video_port);
    ann_line(&a, "c=IN IP4 0.0.0.0");
    ann_line(&a, "i=DeviceString, DeviceName");
    if (a.overflow) return KN_RTSP_ERR_BIG;
    /* the reference's trailing empty element: the joined string ends with
     * CRLF after the i= line */
    if (a.len + 3 > a.cap) return KN_RTSP_ERR_BIG;
    a.out[a.len++] = '\r';
    a.out[a.len++] = '\n';
    a.out[a.len] = '\0';
    *out_len = a.len;
    return KN_RTSP_OK;
}

/* ---------------------------------------------------- negotiated values */

typedef struct {
    const char *name;
    uint32_t lo, hi;
} neg_rule;

static const neg_rule k_neg_rules[16] = {
    { "vqos[0].fec.enable",                 0u, 1u },
    { "vqos[0].fec.rateDropWindow",         0u, 0xFFFFFFFFu },
    { "vqos[0].fec.minRequiredFecPackets",  0u, 0xFFFFFFFFu },
    { "vqos[0].fec.repairPercent",          0u, 100u },
    { "vqos[0].fec.repairMinPercent",       0u, 100u },
    { "vqos[0].fec.repairMaxPercent",       0u, 100u },
    { "vqos[0].bllFec.enable",              0u, 1u },
    { "packetPacing.version",               3u, 3u },
    { "packetPacing.mode",                  1u, 1u },
    { "packetPacing.numGroups",             1u, 0xFFFFFFFFu },
    { "packetPacing.maxDelayUs",            0u, 0xFFFFFFFFu },
    { "packetPacing.minNumPacketsFrame",    0u, 0xFFFFFFFFu },
    { "packetPacing.minNumPacketsPerGroup", 0u, 0xFFFFFFFFu },
    { "packetPacing.enableAccurateSleep",   0u, 1u },
    { "packetPacing.enableSmoothTransition",0u, 1u },
    { "packetPacing.allowFpsBasedToggle",   0u, 1u }
};

/* offered_u32: sdp_attribute parsed as a WHOLE u32 (strict). */
static int offered_u32(const char *sdp, const char *name, uint32_t *out)
{
    char v[64];
    char *endp = NULL;
    unsigned long pv;
    if (kn_rtsp_sdp_attribute(sdp, name, v, sizeof v) != KN_RTSP_OK)
        return -1;
    pv = strtoul(v, &endp, 10);
    if (endp == v || *endp != '\0' || pv > 0xFFFFFFFFul) return -1;
    *out = (uint32_t)pv;
    return 0;
}

static int offered_in(const char *sdp, const char *name, uint32_t lo,
                      uint32_t hi, uint32_t *out)
{
    uint32_t v;
    if (offered_u32(sdp, name, &v) != 0) return -1;
    if (v < lo || v > hi) return -1;      /* ignore_offer: out of range */
    *out = v;
    return 0;
}

int kn_rtsp_negotiate_announce(const char *announce, const char *describe,
                               char *out, size_t outsz, size_t *out_len,
                               uint32_t *negotiated_out)
{
    uint32_t values[19];
    size_t i, o = 0;
    const char *l;
    uint32_t vmin, vmax, repair, ceiling, initial, peak;

    if (!announce || !describe || !out || !out_len) return KN_RTSP_ERR_ARGS;

    for (i = 0; i < 16; ++i) {
        uint32_t v;
        if (offered_in(describe, k_neg_rules[i].name, k_neg_rules[i].lo,
                       k_neg_rules[i].hi, &v) != 0) {
            if (offered_u32(announce, k_neg_rules[i].name, &v) != 0)
                return KN_RTSP_ERR_ARGS;   /* our own announce always has it */
        }
        values[i] = v;
    }
    vmin = values[4];
    vmax = values[5];
    if (vmin > vmax) {                     /* ignore the bogus pair (:99) */
        if (offered_u32(announce, "vqos[0].fec.repairMinPercent", &vmin) != 0)
            return KN_RTSP_ERR_ARGS;
        if (offered_u32(announce, "vqos[0].fec.repairMaxPercent", &vmax) != 0)
            return KN_RTSP_ERR_ARGS;
        values[4] = vmin;
        values[5] = vmax;
    }
    repair = values[3];
    if (repair < values[4]) repair = values[4];
    if (repair > values[5]) repair = values[5];
    values[3] = repair;

    if (offered_u32(announce, "vqos[0].bw.maximumBitrateKbps", &ceiling) != 0)
        return KN_RTSP_ERR_ARGS;
    if (offered_in(describe, "video[0].initialBitrateKbps", 1u, 0xFFFFFFFFu,
                   &initial) != 0)
        initial = ceiling;
    if (initial > ceiling) initial = ceiling;
    if (offered_u32(describe, "video[0].initialPeakBitrateKbps", &peak) != 0)
        peak = ceiling;
    if (peak < initial) peak = initial;
    if (peak > ceiling) peak = ceiling;
    values[16] = initial;
    values[17] = peak;
    values[18] = ceiling;

    /* rewrite: substitute the negotiated a=x-nv-{name}:{value} lines,
     * copy everything else verbatim (split_inclusive "\r\n" semantics) */
    l = announce;
    while (*l) {
        const char *nl = strstr(l, "\r\n");
        size_t llen = nl ? (size_t)(nl - l) + 2 : strlen(l);
        int substituted = 0;
        if (llen > 7 && memcmp(l, "a=x-nv-", 7) == 0) {
            const char *colon = (const char *)memchr(l, ':', llen);
            if (colon) {
                size_t nlen = (size_t)(colon - (l + 7));
                for (i = 0; i < 19; ++i) {
                    const char *nm = i < 16 ? k_neg_rules[i].name
                             : (i == 16 ? "video[0].initialBitrateKbps"
                                : (i == 17 ? "video[0].initialPeakBitrateKbps"
                                           : "vqos[0].bw.maximumBitrateKbps"));
                    if (strlen(nm) == nlen && memcmp(l + 7, nm, nlen) == 0) {
                        int n = snprintf(out + o, outsz - o,
                                         "a=x-nv-%s:%u\r\n", nm,
                                         (unsigned)values[i]);
                        if (n < 0 || o + (size_t)n >= outsz)
                            return KN_RTSP_ERR_BIG;
                        o += (size_t)n;
                        substituted = 1;
                        break;
                    }
                }
            }
        }
        if (!substituted) {
            if (o + llen + 1 > outsz) return KN_RTSP_ERR_BIG;
            memcpy(out + o, l, llen);
            o += llen;
        }
        if (!nl) break;
        l = nl + 2;
    }
    out[o] = '\0';
    *out_len = o;
    if (negotiated_out)
        for (i = 0; i < 19; ++i) negotiated_out[i] = values[i];
    return KN_RTSP_OK;
}
