/* kn_wire.c — F34 (round N slice 4m): the TLS plaintext wire dump.
 * Rationale + redaction contract: kn/kn_wire.h. Platform-neutral C; the
 * only platform bits are the dump mutex and the millisecond clock (both
 * already abstracted by kn_sock). Hooked from kn_net_tls_stream_send /
 * kn_net_tls_stream_recv — the single choke point every TLS byte of the
 * CM/RTSP/WS handshake crosses on the rig (Win32+mbedTLS) build. */

#include "kn/kn_wire.h"
#include "kn/kn_sock.h"          /* kn_sock_now_ms */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static CRITICAL_SECTION g_wd_cs;
static int              g_wd_cs_ready = 0;
#  define WD_LOCK()   EnterCriticalSection(&g_wd_cs)
#  define WD_UNLOCK() LeaveCriticalSection(&g_wd_cs)
#else
#  include <pthread.h>
static pthread_mutex_t g_wd_mtx = PTHREAD_MUTEX_INITIALIZER;
#  define WD_LOCK()   pthread_mutex_lock(&g_wd_mtx)
#  define WD_UNLOCK() pthread_mutex_unlock(&g_wd_mtx)
#endif

#define KN_WIRE_MAX_RECORD (256u * 1024u)   /* per-record hex cap */
#define KN_WIRE_B64_MIN    200u             /* long-run redaction floor */

static FILE              *g_wd_fp = NULL;
static unsigned long long g_wd_t0 = 0;
static unsigned long       g_wd_records = 0;
static unsigned long       g_wd_redactions = 0;
static unsigned long long  g_wd_bytes_out = 0;
static unsigned long long  g_wd_bytes_in = 0;

/* ------------------------------------------------------------- redaction */

static void wd_xfill(unsigned char *b, size_t from, size_t to)
{
    size_t i;
    for (i = from; i < to; i++) b[i] = 'X';
}

/* Case-insensitive raw-byte search for an ASCII-lowercase needle.
 * Returns the index of the first match at or after `start`, or
 * WD_NPOS. Buffers are NOT NUL-terminated; len governs. */
#define WD_NPOS ((size_t)-1)

static size_t wd_find_ci(const unsigned char *h, size_t hn, size_t start,
                         const char *needle_lower)
{
    size_t nl = strlen(needle_lower);
    size_t i;
    if (nl == 0 || nl > hn) return WD_NPOS;
    for (i = (start > hn - nl ? hn : start); i <= hn - nl; i++) {
        size_t k = 0;
        while (k < nl &&
               tolower((unsigned char)h[i + k]) ==
               (unsigned char)needle_lower[k])
            k++;
        if (k == nl) return i;
    }
    return WD_NPOS;
}

static size_t wd_first_of(const unsigned char *b, size_t len, size_t start,
                          const char *set)
{
    size_t i;
    for (i = start; i < len; i++)
        if (b[i] && strchr(set, (char)b[i])) return i;
    return len;
}

static int wd_b64ish(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=' ||
           c == '_' || c == '-' || c == '.';
}

long kn_wire_redact(unsigned char *buf, size_t len)
{
    /* JSON string-valued keys whose contents are always secret. The
     * patterns are matched WITH the surrounding quotes, lowercase. */
    static const char *const jkeys[] = {
        "\"token\"", "\"jwt\"", "\"authorization\"", "\"secret\"",
        "\"password\"", "\"sessionkey\"", "\"authtoken\"",
        "\"accesstoken\"", "\"refreshtoken\""
    };
    long spans = 0;
    size_t i, p;

    if (!buf || len == 0) return 0;

    /* 1. SDP ICE password: value up to CR/LF. */
    p = 0;
    while ((i = wd_find_ci(buf, len, p, "a=ice-pwd:")) != WD_NPOS) {
        size_t v0 = i + 10;
        size_t v1 = wd_first_of(buf, len, v0, "\r\n");
        if (v1 > v0) { wd_xfill(buf, v0, v1); spans++; }
        p = v1;
    }

    /* 2. Authorization header value: up to CR/LF or the JSON closing
     *    quote (a JSON "authorization":"GFNJWT ..." is rule 3's job; the
     *    quote stop keeps this rule from eating the rest of a JSON
     *    line). */
    p = 0;
    while ((i = wd_find_ci(buf, len, p, "authorization:")) != WD_NPOS) {
        size_t v0 = i + 14;
        size_t v1 = wd_first_of(buf, len, v0, "\r\n\"");
        if (v1 > v0) { wd_xfill(buf, v0, v1); spans++; }
        p = (v1 < len && buf[v1] == '"') ? v1 + 1 : v1;
    }

    /* 3. JSON secret keys: redact the quoted string value, honoring
     *    backslash escapes. */
    for (i = 0; i < sizeof jkeys / sizeof jkeys[0]; i++) {
        size_t kl = strlen(jkeys[i]);
        p = 0;
        while ((p = wd_find_ci(buf, len, p, jkeys[i])) != WD_NPOS) {
            size_t q = p + kl;
            while (q < len && (buf[q] == ' ' || buf[q] == '\t')) q++;
            if (q < len && buf[q] == ':') {
                q++;
                while (q < len && (buf[q] == ' ' || buf[q] == '\t')) q++;
                if (q < len && buf[q] == '"') {
                    size_t v0 = ++q;
                    while (q < len && buf[q] != '"') {
                        if (buf[q] == '\\') q++;   /* skip escaped char */
                        q++;
                    }
                    if (q > v0) { wd_xfill(buf, v0, q < len ? q : len);
                                  spans++; }
                    p = q;
                    continue;
                }
            }
            p += kl;
        }
    }

    /* 4. Last resort: any base64url-ish run >= KN_WIRE_B64_MIN chars
     *    (JWT bodies survive in URL queries and binary framings). Runs
     *    that are ALREADY all 'X' (output of rules 1-3) are skipped so
     *    a span is never double-counted. Note: this can also catch a
     *    long PUBLIC key blob — acceptable: redact too much, never too
     *    little; the dump file is operator-sensitive by contract. */
    i = 0;
    while (i < len) {
        if (wd_b64ish(buf[i])) {
            size_t j = i, xall = 1;
            while (j < len && wd_b64ish(buf[j])) {
                if (buf[j] != 'X') xall = 0;
                j++;
            }
            if (j - i >= KN_WIRE_B64_MIN && !xall) {
                wd_xfill(buf, i, j);
                spans++;
            }
            i = j;
        } else {
            i++;
        }
    }

    return spans;
}

/* ----------------------------------------------------------------- dump */

int kn_wire_dump_open(const char *path)
{
    if (!path || !path[0]) return -1;
#ifdef _WIN32
    if (!g_wd_cs_ready) {
        InitializeCriticalSection(&g_wd_cs);
        g_wd_cs_ready = 1;
    }
#endif
    WD_LOCK();
    if (g_wd_fp) { WD_UNLOCK(); return -2; }
    /* Append: a lottery re-opens per draw and every open writes its own
     * header line, so one file can carry several draws. Operators should
     * still use a fresh path per flight. */
    g_wd_fp = fopen(path, "ab");
    if (!g_wd_fp) { WD_UNLOCK(); return -3; }
    g_wd_t0 = (unsigned long long)kn_sock_now_ms();
    g_wd_records = 0;
    g_wd_redactions = 0;
    g_wd_bytes_out = 0;
    g_wd_bytes_in = 0;
    fprintf(g_wd_fp, "# kn_wire_dump v1 (F34) — TLS plaintext, "
                     "redacted (X = redacted byte, lengths are true)\n");
    fflush(g_wd_fp);
    WD_UNLOCK();
    return 0;
}

void kn_wire_dump_close(void)
{
    WD_LOCK();
    if (g_wd_fp) {
        fflush(g_wd_fp);
        fclose(g_wd_fp);
        g_wd_fp = NULL;
    }
    WD_UNLOCK();
}

int kn_wire_dump_is_open(void)
{
    return g_wd_fp ? 1 : 0;
}

void kn_wire_dump_record(int dir_out, const unsigned char *buf, size_t len)
{
    unsigned char *copy = NULL;
    size_t clen, i;
    long red = 0;
    unsigned long long t;

    if (!g_wd_fp) return;                    /* hot path: cheap bail */
    if (!buf && len) return;

    WD_LOCK();
    if (!g_wd_fp) { WD_UNLOCK(); return; }   /* raced with close */
    clen = len > KN_WIRE_MAX_RECORD ? (size_t)KN_WIRE_MAX_RECORD : len;
    if (clen) {
        copy = (unsigned char *)malloc(clen);
        if (!copy) { WD_UNLOCK(); return; }  /* honest skip, no partials */
        memcpy(copy, buf, clen);
        red = kn_wire_redact(copy, clen);
    }
    t = (unsigned long long)kn_sock_now_ms() - g_wd_t0;
    fprintf(g_wd_fp, "%c t_ms=%llu len=%llu red=%ld%s\n",
            dir_out ? 'W' : 'R', t, (unsigned long long)len, red,
            len > clen ? " truncated" : "");
    for (i = 0; i < clen; i++) fprintf(g_wd_fp, "%02x", copy[i]);
    fputc('\n', g_wd_fp);
    fflush(g_wd_fp);                         /* crash-resistant by design */
    g_wd_records++;
    g_wd_redactions += (unsigned long)red;
    if (dir_out) g_wd_bytes_out += (unsigned long long)len;
    else         g_wd_bytes_in  += (unsigned long long)len;
    free(copy);
    WD_UNLOCK();
}

void kn_wire_dump_stats(unsigned long *records, unsigned long *redactions,
                        unsigned long long *bytes_out,
                        unsigned long long *bytes_in)
{
    if (records)    *records    = g_wd_records;
    if (redactions) *redactions = g_wd_redactions;
    if (bytes_out)  *bytes_out  = g_wd_bytes_out;
    if (bytes_in)   *bytes_in   = g_wd_bytes_in;
}
