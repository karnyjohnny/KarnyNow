/* test_knwire.c — the F34 wire-dump battery: the length-preserving
 * secret redactor (the security contract of the dump) and the dump file
 * machinery (open/record/close/stats/append/truncation). The redactor
 * runs on raw byte buffers — no TLS, no sockets, identical on every
 * platform, so this suite is native-test material. */

#if !defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L)
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_wire.h"
#include "test_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_PATH "logs/test_knwire_dump.txt"

/* Build a 200+ char base64url-ish run deterministically. */
static void fill_b64(char *dst, size_t n)
{
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";
    size_t i;
    for (i = 0; i < n; i++) dst[i] = alpha[(i * 7 + 3) % (sizeof alpha - 1)];
    dst[n] = '\0';
}

static int has_secret(const char *buf, size_t len, const char *secret)
{
    size_t sl = strlen(secret), i;
    if (sl > len) return 0;
    for (i = 0; i + sl <= len; i++)
        if (memcmp(buf + i, secret, sl) == 0) return 1;
    return 0;
}

static void test_redact_sdp_and_headers(void)
{
    char sdp[512];
    long n;
    int len = snprintf(sdp, sizeof sdp,
        "v=0\r\na=ice-ufrag:AB12\r\na=ice-pwd:SUPERSECRETPWDVALUE\r\n"
        "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11\r\n");
    n = kn_wire_redact((unsigned char *)sdp, (size_t)len);
    CHECK(n == 1, "sdp: exactly one redacted span (ice-pwd)");
    CHECK((size_t)len == strlen(sdp), "sdp: length preserved");
    CHECK(!has_secret(sdp, strlen(sdp), "SUPERSECRETPWDVALUE"),
          "sdp: ice-pwd value gone");
    CHECK(strstr(sdp, "a=ice-ufrag:AB12") != NULL,
          "sdp: ufrag kept (identity)");
    CHECK(strstr(sdp, "a=fingerprint:sha-256 AA:BB:CC:DD") != NULL,
          "sdp: fingerprint kept (identity)");

    {
        char req[512];
        int rlen = snprintf(req, sizeof req,
            "POST /v2/session HTTP/1.1\r\n"
            "Authorization: GFNJWT eyJhbGciOiJSUzI1NiJ9.payload.sig\r\n"
            "Host: prod.cloudmatchbeta.nvidiagrid.net\r\n\r\n");
        n = kn_wire_redact((unsigned char *)req, (size_t)rlen);
        /* rule 2 (header) redacts the value; rule 4 skips the all-X run */
        CHECK(n == 1, "http: authorization header = one span");
        CHECK(!has_secret(req, strlen(req), "eyJhbGciOiJSUzI1NiJ9"),
              "http: JWT gone");
        CHECK(strstr(req, "Host: prod.cloudmatchbeta") != NULL,
          "http: following header intact");
        CHECK((size_t)rlen == strlen(req), "http: length preserved");
    }
}

static void test_redact_json(void)
{
    char js[1024];
    long n;
    int len = snprintf(js, sizeof js,
        "{\"sessionRequestData\":{\"token\":\"eyJLONG.eyJBODY.SIGS\","
        "\"clientVersion\":\"2.0.87.131\","
        "\"JWT\":\"lowercase-key-check\","
        "\"note\":\"escaped \\\" quote inside\"}}");
    n = kn_wire_redact((unsigned char *)js, (size_t)len);
    CHECK(n == 2, "json: token + JWT keys redacted (2 spans)");
    CHECK(!has_secret(js, strlen(js), "eyJLONG"), "json: token value gone");
    CHECK(!has_secret(js, strlen(js), "lowercase-key-check"),
          "json: JWT value gone (case-insensitive key)");
    CHECK(strstr(js, "\"clientVersion\":\"2.0.87.131\"") != NULL,
          "json: clientVersion kept (identity)");
    CHECK(strstr(js, "escaped \\\" quote inside") != NULL,
          "json: escaped-quote value survives");
    CHECK((size_t)len == strlen(js), "json: length preserved");
}

static void test_redact_b64_and_binary(void)
{
    char buf[1024];
    long n, n2;
    char run200[256], run199[256];

    fill_b64(run200, 200);
    fill_b64(run199, 199);

    /* space-separated: '=' and '&' are in the b64url charset and would
     * merge runs — the delimiter must not be. */
    snprintf(buf, sizeof buf, "u %s a %s", run199, run200);
    n = kn_wire_redact((unsigned char *)buf, strlen(buf));
    CHECK(n == 1, "b64: only the >=200 run redacted");
    CHECK(strstr(buf, run199) != NULL, "b64: 199-char run kept");
    CHECK(!has_secret(buf, strlen(buf), run200), "b64: 200-char run gone");

    /* idempotence: an all-X run must not double-count */
    n2 = kn_wire_redact((unsigned char *)buf, strlen(buf));
    CHECK(n2 == 0, "b64: second pass redacts nothing (no double count)");

    /* binary safety: NULs are ordinary bytes; a pattern after a NUL is
     * still found, and a b64 run stops AT the NUL. */
    {
        unsigned char bin[128];
        size_t blen = 0;
        char tail[64];
        int tlen;
        memset(bin, 0, sizeof bin);
        bin[blen++] = 0x00; bin[blen++] = 0xDE; bin[blen++] = 0x00;
        tlen = snprintf(tail, sizeof tail, "a=ice-pwd:BINARYPWD\r\n");
        memcpy(bin + blen, tail, (size_t)tlen);
        blen += (size_t)tlen;
        bin[blen++] = 0x00;
        n = kn_wire_redact(bin, blen);
        CHECK(n == 1, "binary: ice-pwd after NULs found");
        CHECK(!has_secret((char *)bin, blen, "BINARYPWD"),
              "binary: value gone");
        CHECK(bin[1] == 0xDE && bin[3] == (unsigned char)'a',
              "binary: surrounding bytes untouched");
    }

    CHECK(kn_wire_redact(NULL, 16) == 0, "redact(NULL) = 0, no crash");
    {
        unsigned char one = 'x';
        CHECK(kn_wire_redact(&one, 0) == 0, "redact(len=0) = 0");
    }
}

static void test_dump_file(void)
{
    unsigned long recs, reds;
    unsigned long long bo, bi;
    static const char secret[] = "a=ice-pwd:FILESECRETPWD\r\n";
    char payload[256];
    FILE *f;
    char *filebuf;
    long fsz;

    kn_test_unlink(DUMP_PATH);

    CHECK(kn_wire_dump_open(NULL) == -1, "open(NULL) = -1");
    CHECK(kn_wire_dump_open("") == -1, "open(\"\") = -1");
    CHECK(kn_wire_dump_is_open() == 0, "not open yet");

    CHECK(kn_wire_dump_open(DUMP_PATH) == 0, "open ok");
    CHECK(kn_wire_dump_is_open() == 1, "is_open = 1");
    CHECK(kn_wire_dump_open(DUMP_PATH) == -2, "double open = -2");

    /* two records: one plain, one carrying a secret */
    kn_wire_dump_record(1, (const unsigned char *)"ANNOUNCE rtsp", 13);
    kn_wire_dump_record(0, (const unsigned char *)secret,
                        (unsigned long)strlen(secret));
    kn_wire_dump_stats(&recs, &reds, &bo, &bi);
    CHECK(recs == 2, "stats: 2 records");
    CHECK(reds == 1, "stats: 1 redaction");
    CHECK(bo == 13, "stats: bytes_out = 13");
    CHECK(bi == strlen(secret), "stats: bytes_in exact");

    kn_wire_dump_close();
    kn_wire_dump_close();               /* idempotent */
    CHECK(kn_wire_dump_is_open() == 0, "closed");
    kn_wire_dump_record(1, (const unsigned char *)"x", 1);  /* no-op */
    kn_wire_dump_stats(&recs, NULL, NULL, NULL);
    CHECK(recs == 2, "record after close is a no-op");

    /* the file: header, both direction lines, hex of the plain payload,
     * and the SECRET NOWHERE in raw form */
    f = fopen(DUMP_PATH, "rb");
    CHECK(f != NULL, "dump file exists");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    filebuf = (char *)malloc((size_t)fsz + 1);
    CHECK(filebuf != NULL, "slurp alloc");
    if (!filebuf) { fclose(f); return; }
    fread(filebuf, 1, (size_t)fsz, f);
    filebuf[fsz] = '\0';
    fclose(f);

    CHECK_STR_CONTAINS(filebuf, "# kn_wire_dump v1", "file: header");
    CHECK_STR_CONTAINS(filebuf, "W t_ms=", "file: W line");
    CHECK_STR_CONTAINS(filebuf, "R t_ms=", "file: R line");
    CHECK_STR_CONTAINS(filebuf, "len=13", "file: true length");
    CHECK_STR_CONTAINS(filebuf, "red=1", "file: per-line redaction count");
    CHECK_STR_CONTAINS(filebuf, "414e4e4f554e434520" /* "ANNOUNCE " */,
                       "file: hex payload present");
    CHECK(!has_secret(filebuf, (size_t)fsz, "FILESECRETPWD"),
          "file: secret NEVER hits disk in raw form");
    CHECK_STR_NOT_CONTAINS(filebuf, "46494c45534543", /* "FILESEC" hex */
                           "file: secret hex absent");
    free(filebuf);

    /* re-open appends (lottery draws) and resets counters */
    CHECK(kn_wire_dump_open(DUMP_PATH) == 0, "re-open ok (append)");
    kn_wire_dump_stats(&recs, NULL, NULL, NULL);
    CHECK(recs == 0, "counters reset on open");
    snprintf(payload, sizeof payload, "draw2");
    kn_wire_dump_record(1, (const unsigned char *)payload, 5);
    kn_wire_dump_close();
    f = fopen(DUMP_PATH, "rb");
    if (f) {
        int headers = 0;
        char line[512];
        while (fgets(line, sizeof line, f))
            if (strstr(line, "# kn_wire_dump v1")) headers++;
        fclose(f);
        CHECK(headers == 2, "append: one header per open");
    } else {
        CHECK(0, "append: file readable");
    }
    kn_test_unlink(DUMP_PATH);
}

static void test_dump_truncation(void)
{
    size_t big = 256u * 1024u + 4096u;
    unsigned char *b = (unsigned char *)malloc(big);
    unsigned long long bo;
    FILE *f;
    char line[512];
    int found = 0;

    if (!b) { CHECK(0, "trunc: alloc"); return; }
    memset(b, 'A', big);
    kn_test_unlink(DUMP_PATH);
    CHECK(kn_wire_dump_open(DUMP_PATH) == 0, "trunc: open");
    kn_wire_dump_record(1, b, big);
    kn_wire_dump_stats(NULL, NULL, &bo, NULL);
    CHECK(bo == (unsigned long long)big, "trunc: true byte count kept");
    kn_wire_dump_close();
    f = fopen(DUMP_PATH, "rb");
    while (f && fgets(line, sizeof line, f))
        if (strstr(line, "truncated") && strstr(line, "len=266240"))
            found = 1;
    if (f) fclose(f);
    CHECK(found == 1, "trunc: line marked truncated with true len");
    free(b);
    kn_test_unlink(DUMP_PATH);
}

int main(void)
{
    test_redact_sdp_and_headers();
    test_redact_json();
    test_redact_b64_and_binary();
    test_dump_file();
    test_dump_truncation();
    return kn_test_summary("test_knwire");
}
