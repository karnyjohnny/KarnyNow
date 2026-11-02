/* test_kncm.c — kn_json + kn_http + kn_cm battery (round XXI).
 * CloudMatch fixtures follow the shapes in cloudmatch.rs's OWN tests
 * (:4073-4085 — requestStatus/session/sessionControlInfo/connectionInfo)
 * plus the derivation rules of session_info (:1946-2103). Section-
 * isolated, never aborts (round-XVI suite rule).
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_cm.h"
#include "kn/kn_http.h"
#include "kn/kn_json.h"
#include "test_port.h"

#include <ctype.h>
#include <string.h>

/* ------------------------------------------------------- byte-stream io */

#define HIO_CAP (256 * 1024)

typedef struct {
    uint8_t to[HIO_CAP];   size_t tlen, tpos, chunk;
    uint8_t from[HIO_CAP]; size_t flen;
    int eof_after_all;     /* 1: serve EOF once exhausted (default) */
} hio;

static int hio_send(void *ctx, const uint8_t *b, size_t n)
{
    hio *h = (hio *)ctx;
    if (h->flen + n > HIO_CAP) return -1;
    memcpy(h->from + h->flen, b, n);
    h->flen += n;
    return 0;
}

static int hio_recv(void *ctx, uint8_t *b, size_t cap, uint32_t t)
{
    hio *h = (hio *)ctx;
    size_t avail, take;
    (void)t;
    avail = h->tlen - h->tpos;
    if (avail == 0) return -2;             /* EOF */
    take = avail < cap ? avail : cap;
    if (h->chunk && take > h->chunk) take = h->chunk;
    memcpy(b, h->to + h->tpos, take);
    h->tpos += take;
    return (int)take;
}

static void hio_reset(hio *h) { memset(h, 0, sizeof *h); h->eof_after_all = 1; }
static void hio_queue(hio *h, const char *s)
{
    size_t n = strlen(s);
    if (h->tlen + n <= HIO_CAP) { memcpy(h->to + h->tlen, s, n); h->tlen += n; }
}
static kn_http_io hio_io(hio *h)
{
    kn_http_io io;
    io.send = hio_send;
    io.recv = hio_recv;
    io.ctx = h;
    return io;
}

static uint64_t g_now;
static uint64_t fake_now(void *ctx) { (void)ctx; return g_now; }

static uint8_t g_rxb[64 * 1024];
static char g_body[HIO_CAP];

/* ------------------------------------------------------------- kn_json */

static void test_json(void)
{
    static kn_jdoc d;
    const kn_jnode *root, *n;
    char s[128];
    long long iv;
    double dv;
    int bv;
    size_t err_at = 0;

    printf("-- kn_json --\n");
    {
        static const char doc[] =
            "{\"requestStatus\":{\"statusCode\":1},\"arr\":[1,2.5,true,false,null,"
            "{\"deep\":\"x\"}],\"esc\":\"a\\\"b\\\\c\\/d\\n\\u00e9\\uD83D\\uDE00\","
            "\"empty\":\"\",\"neg\":-42}";
        CHECK(kn_json_parse(&d, doc, strlen(doc), &err_at) == KN_JSON_OK,
              "nested document parses");
        root = kn_json_root(&d);
        CHECK(root && root->type == KN_J_OBJ, "root is an object");
        n = kn_json_get(&d, kn_json_get(&d, root, "requestStatus"),
                        "statusCode");
        CHECK(kn_json_int(n, &iv) == KN_JSON_OK && iv == 1,
              "nested member int");
        n = kn_json_get(&d, root, "arr");
        CHECK(kn_json_len(&d, n) == 6, "array length");
        CHECK(kn_json_int(kn_json_at(&d, n, 0), &iv) == KN_JSON_OK && iv == 1,
              "arr[0] int");
        CHECK(kn_json_double(kn_json_at(&d, n, 1), &dv) == KN_JSON_OK &&
              dv > 2.49 && dv < 2.51, "arr[1] double");
        CHECK(kn_json_bool(kn_json_at(&d, n, 2), &bv) == KN_JSON_OK && bv == 1,
              "arr[2] true");
        CHECK(kn_json_bool(kn_json_at(&d, n, 3), &bv) == KN_JSON_OK && bv == 0,
              "arr[3] false");
        CHECK(kn_json_at(&d, n, 4)->type == KN_J_NULL, "arr[4] null");
        CHECK(kn_json_str_copy(kn_json_get(&d, kn_json_at(&d, n, 5),
                                           "deep"), s, sizeof s)
                  == KN_JSON_OK && strcmp(s, "x") == 0,
              "arr[5].deep string");
        CHECK(kn_json_at(&d, n, 99) == NULL, "out-of-range index -> NULL");
        n = kn_json_get(&d, root, "esc");
        CHECK(kn_json_str_copy(n, s, sizeof s) == KN_JSON_OK &&
              strcmp(s, "a\"b\\c/d\n\xc3\xa9\xf0\x9f\x98\x80") == 0,
              "escapes: quote/backslash/slash/newline/u00e9/surrogate pair");
        n = kn_json_get(&d, root, "empty");
        CHECK(kn_json_str_copy(n, s, sizeof s) == KN_JSON_OK && s[0] == '\0',
              "empty string");
        CHECK(kn_json_int(kn_json_get(&d, root, "neg"), &iv) == KN_JSON_OK &&
              iv == -42, "negative int");
        CHECK(kn_json_get(&d, root, "absent") == NULL, "absent key -> NULL");
    }
    {   /* int64 precision: kn_json_int re-parses the raw span */
        static const char doc2[] = "{\"big\":9007199254740993}";
        CHECK(kn_json_parse(&d, doc2, strlen(doc2), NULL) == KN_JSON_OK &&
              kn_json_int(kn_json_get(&d, kn_json_root(&d), "big"), &iv)
                  == KN_JSON_OK && iv == 9007199254740993LL,
              "int64 beyond double precision stays exact");
    }
    {   /* error classes */
        static const char bad1[] = "{\"a\":1} trailing";
        static const char bad2[] = "{\"a\":}";
        static const char bad3[] = "[1,2";
        static const char bad4[] = "{\"s\":\"bad \\q\"}";
        static const char bad5[] = "{\"s\":\"lone \\uD800 ok\"}";
        CHECK(kn_json_parse(&d, bad1, strlen(bad1), &err_at) ==
                  KN_JSON_ERR_SYNTAX, "trailing garbage -> SYNTAX");
        CHECK(kn_json_parse(&d, bad2, strlen(bad2), NULL) ==
                  KN_JSON_ERR_SYNTAX, "missing value -> SYNTAX");
        CHECK(kn_json_parse(&d, bad3, strlen(bad3), NULL) ==
                  KN_JSON_ERR_SYNTAX, "unterminated array -> SYNTAX");
        CHECK(kn_json_parse(&d, bad4, strlen(bad4), NULL) ==
                  KN_JSON_ERR_SYNTAX, "invalid escape -> SYNTAX");
        CHECK(kn_json_parse(&d, bad5, strlen(bad5), NULL) == KN_JSON_OK,
              "lone surrogate parses (RFC: allowed in text)");
        CHECK(kn_json_str_copy(kn_json_get(&d, kn_json_root(&d), "s"),
                               s, sizeof s) == KN_JSON_OK,
              "lone surrogate copies (U+FFFD)");
    }
    {   /* depth cap: 40 nested arrays */
        char deep[128];
        int i, rc;
        memset(deep, '[', 40);
        memset(deep + 40, ']', 40);
        deep[80] = '\0';
        rc = kn_json_parse(&d, deep, 80, NULL);
        CHECK(rc == KN_JSON_ERR_DEPTH, "40-deep nesting -> ERR_DEPTH");
        for (i = 0; i < 60; ++i) deep[i] = i < 30 ? '[' : ']';
        CHECK(kn_json_parse(&d, deep, 60, NULL) == KN_JSON_OK,
              "30-deep nesting parses");
    }
    CHECK(kn_json_parse(NULL, "{}", 2, NULL) == KN_JSON_ERR_ARGS,
          "NULL doc -> ERR_ARGS");
}

/* ------------------------------------------------------------- kn_http */

static void test_http(void)
{
    static hio h;
    kn_http_response r;
    int rc, status = 0;
    g_now = 0;

    printf("-- kn_http --\n");
    /* Content-Length framing + header capture */
    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                  "Content-Length: 5\r\n\r\nhello");
    rc = kn_http_request(hio_io(&h), "GET", "/x", "example.com", NULL, 0,
                         NULL, 0, 1000, fake_now, NULL, g_rxb,
                         sizeof g_rxb, g_body, sizeof g_body, &r);
    CHECK(rc == KN_HTTP_OK && r.status == 200 && r.body_len == 5 &&
          memcmp(r.body, "hello", 5) == 0 && !r.truncated,
          "CL-framed 200 with body");
    CHECK(strcmp(kn_http_header_get(&r, "content-TYPE"),
                 "application/json") == 0,
          "header lookup case-insensitive");
    CHECK(h.flen > 0 && memcmp(h.from, "GET /x HTTP/1.1\r\n", 17) == 0,
          "request line exact");
    CHECK(strstr((char *)h.from, "Host: example.com\r\n") != NULL &&
          strstr((char *)h.from, "Connection: close\r\n") != NULL,
          "Host + Connection: close sent");

    /* chunked framing (multiple chunks + terminators) */
    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                  "5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n");
    rc = kn_http_request(hio_io(&h), "GET", "/c", "example.com", NULL, 0,
                         NULL, 0, 1000, fake_now, NULL, g_rxb,
                         sizeof g_rxb, g_body, sizeof g_body, &r);
    CHECK(rc == KN_HTTP_OK && r.body_len == 12 &&
          memcmp(r.body, "hello, world", 12) == 0, "chunked body joined");

    /* chunked delivered 1 byte per recv (kernel fragmentation) */
    hio_reset(&h);
    h.chunk = 1;
    hio_queue(&h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                  "5\r\nhello\r\n0\r\n\r\n");
    rc = kn_http_request(hio_io(&h), "GET", "/c", "example.com", NULL, 0,
                         NULL, 0, 1000, fake_now, NULL, g_rxb,
                         sizeof g_rxb, g_body, sizeof g_body, &r);
    CHECK(rc == KN_HTTP_OK && r.body_len == 5, "chunked 1-byte chunks");

    /* read-to-EOF framing */
    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 204 No Content\r\nX-a: b\r\n\r\n");
    rc = kn_http_request(hio_io(&h), "GET", "/e", "example.com", NULL, 0,
                         NULL, 0, 1000, fake_now, NULL, g_rxb,
                         sizeof g_rxb, g_body, sizeof g_body, &r);
    CHECK(rc == KN_HTTP_OK && r.status == 204 && r.body_len == 0,
          "no-CL/no-chunk body reads to EOF");

    /* non-200 passes through with the body */
    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno");
    rc = kn_http_request(hio_io(&h), "GET", "/m", "example.com", NULL, 0,
                         NULL, 0, 1000, fake_now, NULL, g_rxb,
                         sizeof g_rxb, g_body, sizeof g_body, &r);
    CHECK(rc == KN_HTTP_OK && r.status == 404 &&
          strcmp(r.status_text, "Not Found") == 0, "404 surfaces as data");

    /* request headers + body */
    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    {
        kn_http_header hh[2];
        hh[0].name = "Authorization"; hh[0].value = "GFNJWT T0K";
        hh[1].name = "Content-Type"; hh[1].value = "text/plain";
        h.from[0] = '\0';
        rc = kn_http_request(hio_io(&h), "POST", "/v2/session", "cm.example",
                             hh, 2, "{}", 2, 1000, fake_now, NULL, g_rxb,
                             sizeof g_rxb, g_body, sizeof g_body, &r);
        h.from[h.flen] = '\0';
        CHECK(rc == KN_HTTP_OK &&
              strstr((char *)h.from,
                     "POST /v2/session HTTP/1.1\r\n") == (char *)h.from,
              "POST request line");
        CHECK(strstr((char *)h.from, "Authorization: GFNJWT T0K\r\n") &&
              strstr((char *)h.from, "Content-Length: 2\r\n\r\n{}"),
              "extra headers + CL + body bytes");
    }

    /* protocol errors */
    hio_reset(&h);
    hio_queue(&h, "GARBAGE\r\n\r\n");
    rc = kn_http_request(hio_io(&h), "GET", "/g", "example.com", NULL, 0,
                         NULL, 0, 1000, fake_now, NULL, g_rxb,
                         sizeof g_rxb, g_body, sizeof g_body, &r);
    CHECK(rc == KN_HTTP_ERR_PROTO, "garbage status line -> ERR_PROTO");
    hio_reset(&h);                    /* immediate EOF */
    rc = kn_http_request(hio_io(&h), "GET", "/z", "example.com", NULL, 0,
                         NULL, 0, 1000, fake_now, NULL, g_rxb,
                         sizeof g_rxb, g_body, sizeof g_body, &r);
    CHECK(rc == KN_HTTP_ERR_EOF, "silent peer -> ERR_EOF");
    /* body truncation flag */
    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789");
    {
        char small[8];
        rc = kn_http_request(hio_io(&h), "GET", "/t", "example.com", NULL, 0,
                             NULL, 0, 1000, fake_now, NULL, g_rxb,
                             sizeof g_rxb, small, sizeof small, &r);
        CHECK(rc == KN_HTTP_OK && r.truncated && r.body_len == 7,
              "undersized body buffer -> honest truncated prefix");
    }
    (void)status;
}

/* ---------------------------------------------------------------- kn_cm */

static const char k_session_ready[] =
    "{\"requestStatus\":{\"statusCode\":1},\"session\":{"
    "\"sessionId\":\"SESS-1\",\"status\":2,"
    "\"sessionControlInfo\":{\"ip\":\"10.0.0.1\",\"port\":443},"
    "\"connectionInfo\":["
      "{\"usage\":16,\"ip\":\"rig.example.nvidiagrid.net\",\"port\":322},"
      "{\"usage\":2,\"ip\":\"10.0.0.5\",\"port\":47995},"
      "{\"usage\":14,\"appLevelProtocol\":4,\"ip\":\"webrtc.example\","
       "\"port\":8443,\"resourcePath\":\"/nvst/\"}],"
    "\"seatSetupInfo\":{\"seatSetupStep\":3,\"queuePosition\":7},"
    "\"subSessionId\":\"sub-1\"}}";

static void test_cm_extract(void)
{
    kn_cm_session s;

    printf("-- kn_cm extract --\n");
    CHECK(kn_cm_extract_session(k_session_ready, &s) == 0, "ready fixture");
    CHECK(strcmp(s.session_id, "SESS-1") == 0 && s.status == 2,
          "sessionId + status");
    CHECK(strcmp(s.phase, "ready") == 0 && s.ready == 1 && s.resumable == 1,
          "phase/ready/resumable from status 2");
    CHECK(s.have_rtsps &&
          strcmp(s.rtsps_endpoint, "rtsps://rig.example.nvidiagrid.net:322")
              == 0,
          "usage-16 connection derives the rtsps endpoint (:1989)");
    CHECK(strcmp(s.signaling_url,
                 "wss://rig.example.nvidiagrid.net:322") == 0,
          "signalingUrl = rtsps->wss (:2046)");
    CHECK(strcmp(s.server_ip, "rig.example.nvidiagrid.net") == 0,
          "serverIp from the signaling connection");
    CHECK(s.have_media && strcmp(s.media_ip, "10.0.0.5") == 0 &&
          s.media_port == 47995, "usage-2 bundle/media peer (:2050)");
    CHECK(s.have_seat && s.seat_setup_step == 3, "seatSetupStep");
    CHECK(s.have_queue && s.queue_position == 7, "queuePosition");
    CHECK(s.conn_count == 3 && s.conn0_usage == 16 && s.conn0_alp == -1 &&
          s.conn0_port == 322 && s.conn0_path[0] == '\0',
          "F37: conn0 witness on a healthy native seat (absent alp/path = "
          "-1/\"\", present port = 322)");

    {   /* usage 14 + appLevelProtocol 1 also qualifies; status 1 not ready */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S2\",\"status\":1,"
            "\"connectionInfo\":[{\"usage\":14,\"appLevelProtocol\":1,"
            "\"ip\":\"seat2.example\",\"port\":443}]}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0 && s.have_rtsps &&
              strcmp(s.rtsps_endpoint, "rtsps://seat2.example:443") == 0 &&
              s.ready == 0 && s.resumable == 0 &&
              strcmp(s.phase, "preparing") == 0,
              "usage14+alp1 qualifies; status 1 = preparing, not ready");
    }
    {   /* alp 4 with usage 14 is the WebRTC signaling — must NOT derive */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S3\",\"status\":3,"
            "\"connectionInfo\":[{\"usage\":14,\"appLevelProtocol\":4,"
            "\"ip\":\"w.example\",\"port\":8443}]}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0 && !s.have_rtsps &&
              s.ready == 0,
              "usage14+alp4 (WebRTC) does not produce an rtsps endpoint");
    }
    {   /* F37 (run-101 verbatim): the BROWSER-identity seat — status 2,
         * usage 14 + alp 5 on :443 /nvst/. ready MUST stay 0 and the
         * conn0 witness fields must name the shape so the session loop
         * can fail fast with conn_unusable instead of polling silently
         * for 217 s (RTSP §59.2). */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"E5FEC7C9\",\"status\":2,"
            "\"connectionInfo\":[{\"protocol\":1,\"appLevelProtocol\":5,"
            "\"ip\":\"80-250-97-36.cloudmatchbeta.nvidiagrid.net\","
            "\"resourcePath\":\"/nvst/\",\"usage\":14,\"port\":443}],"
            "\"seatSetupInfo\":{\"queuePosition\":0,\"seatSetupStep\":3}}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0, "run-101 seat fixture");
        CHECK(s.status == 2 && s.ready == 0 && !s.have_rtsps,
              "run-101: status 2 but usage14+alp5 is NOT rtsps -> ready 0");
        CHECK(s.conn_count == 1 && s.conn0_usage == 14 && s.conn0_alp == 5 &&
              s.conn0_port == 443 && strcmp(s.conn0_path, "/nvst/") == 0,
              "run-101: conn0 witness fields (usage/alp/port/path)");
        CHECK(s.have_seat && s.seat_setup_step == 3 && !s.have_queue,
              "run-101: seatSetupStep 3, queuePosition 0 filtered");
    }
    {   /* resourcePath rtsps URL kept as-is; port required (:2005) */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S4\",\"status\":2,"
            "\"connectionInfo\":[{\"usage\":16,"
            "\"resourcePath\":\"rtsps://seat4.example:322\"}]}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0 && s.have_rtsps &&
              strcmp(s.rtsps_endpoint, "rtsps://seat4.example:322") == 0 &&
              s.ready == 1, "resourcePath rtsps URL passes through");
        static const char fx2[] =
            "{\"session\":{\"sessionId\":\"S5\",\"status\":2,"
            "\"connectionInfo\":[{\"usage\":16,"
            "\"resourcePath\":\"rtsps://seat5.example\"}]}}";
        CHECK(kn_cm_extract_session(fx2, &s) == 0 && !s.have_rtsps,
              "rtsps URL without explicit port rejected");
    }
    {   /* IPv6 bracketing (:2015) */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S6\",\"status\":3,"
            "\"connectionInfo\":[{\"usage\":16,\"ip\":\"2001:db8::9\","
            "\"port\":322}]}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0 &&
              strcmp(s.rtsps_endpoint, "rtsps://[2001:db8::9]:322") == 0,
              "IPv6 endpoint bracketed");
    }
    {   /* queue fallback chain (:1971): sessionProgress source */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S7\",\"status\":1,"
            "\"sessionProgress\":{\"queuePosition\":4}}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0 && s.have_queue &&
              s.queue_position == 4, "sessionProgress.queuePosition fallback");
    }
    {   /* queuePosition <= 0 filtered (:1981) */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S8\",\"status\":1,"
            "\"queuePosition\":0}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0 && !s.have_queue,
              "queuePosition 0 filtered out");
    }
    {   /* hard requirement: sessionId */
        CHECK(kn_cm_extract_session("{\"session\":{\"status\":2}}", &s) == -1,
              "missing sessionId -> -1");
        CHECK(kn_cm_extract_session("{\"nope\":1}", &s) == -1,
              "missing session -> -1");
        CHECK(kn_cm_extract_session("not json", &s) == -1,
              "garbage -> -1");
    }
    CHECK(strcmp(kn_cm_phase(3), "streaming") == 0 &&
          strcmp(kn_cm_phase(4), "paused") == 0 &&
          strcmp(kn_cm_phase(6), "resuming") == 0 &&
          strcmp(kn_cm_phase(7), "finished") == 0 &&
          strcmp(kn_cm_phase(9), "failed") == 0,
          "phase table (:2698)");
}

static void test_cm_body(void)
{
    kn_cm_settings st;
    static char body[8192], auth[2048];
    kn_http_header hh[24];
    size_t n = 0;
    static kn_jdoc d;

    printf("-- kn_cm body/headers --\n");
    memset(&st, 0, sizeof st);
    st.token = "TOKEN123";
    st.device_id = "device-hash-abc";
    st.app_id = 1234567;
    st.title = "My Game";
    st.width = 1280; st.height = 720; st.fps = 30;
    st.timezone_offset_ms = 7200000;
    CHECK(kn_cm_build_create_body(&st, body, sizeof body, &n) == KN_HTTP_OK &&
          n > 1000, "create body builds");
    /* the body must itself be valid JSON with the reference's fields */
    CHECK(kn_json_parse(&d, body, n, NULL) == KN_JSON_OK,
          "create body is valid JSON");
    {
        const kn_jnode *srd = kn_json_get(&d, kn_json_root(&d),
                                          "sessionRequestData");
        long long iv;
        char sv[128];
        CHECK(srd != NULL, "sessionRequestData present");
        CHECK(kn_json_int(kn_json_get(&d, srd, "appId"), &iv) == KN_JSON_OK &&
              iv == 1234567, "appId numeric");
        CHECK(kn_json_str_copy(kn_json_get(&d, srd, "internalTitle"),
                               sv, sizeof sv) == KN_JSON_OK &&
              strcmp(sv, "My Game") == 0, "internalTitle");
        CHECK(kn_json_str_copy(kn_json_get(&d, srd, "clientIdentification"),
                               sv, sizeof sv) == KN_JSON_OK &&
              strcmp(sv, "GFN-PC") == 0, "clientIdentification GFN-PC");
        CHECK(kn_json_str_copy(kn_json_get(&d, srd, "clientVersion"),
                               sv, sizeof sv) == KN_JSON_OK &&
              strcmp(sv, "30.0") == 0, "clientVersion 30.0");
        CHECK(kn_json_str_copy(kn_json_get(&d, srd, "sdkVersion"),
                               sv, sizeof sv) == KN_JSON_OK &&
              strcmp(sv, "2.0") == 0 &&
              kn_json_str_copy(kn_json_get(&d, srd, "streamerVersion"),
                               sv, sizeof sv) == KN_JSON_OK &&
              strcmp(sv, "14") == 0, "sdkVersion 2.0 + streamerVersion 14");
        {
            const kn_jnode *mon = kn_json_at(&d,
                kn_json_get(&d, srd, "clientRequestMonitorSettings"), 0);
            CHECK(kn_json_int(kn_json_get(&d, mon, "widthInPixels"), &iv)
                      == KN_JSON_OK && iv == 1280, "monitor width 1280 (D14)");
            CHECK(kn_json_int(kn_json_get(&d, mon, "framesPerSecond"), &iv)
                      == KN_JSON_OK && iv == 30, "monitor fps 30 (D14)");
        }
        {
            const kn_jnode *md = kn_json_get(&d, srd, "metaData");
            CHECK(kn_json_len(&d, md) == 3, "metaData trio");
            CHECK(kn_json_str_copy(kn_json_get(&d, kn_json_at(&d, md, 0),
                                               "key"), sv, sizeof sv)
                      == KN_JSON_OK && strcmp(sv, "wssignaling") == 0,
                  "metaData[0] wssignaling");
        }
        {
            const kn_jnode *f = kn_json_get(&d, srd,
                                            "requestedStreamingFeatures");
            int bv;
            CHECK(kn_json_bool(kn_json_get(&d, f, "reflex"), &bv) ==
                      KN_JSON_OK && bv == 0, "reflex false at 30 fps");
            CHECK(kn_json_int(kn_json_get(&d, f, "bitDepth"), &iv) ==
                      KN_JSON_OK && iv == 0, "bitDepth 0 (8-bit H264)");
        }
        {
            int bv;
            CHECK(kn_json_bool(kn_json_get(&d, srd, "secureRTSPSupported"),
                               &bv) == KN_JSON_OK && bv == 1,
                  "secureRTSPSupported true");
            CHECK(kn_json_get(&d, srd, "transport")->type == KN_J_NULL,
                  "transport null (native NVST)");
        }
        {   /* F37: the OpenNOW-strict deltas (RTSP §59.3) */
            const kn_jnode *md, *f, *dlss;
            char sv2[128];
            CHECK(kn_json_int(kn_json_get(&d, srd, "appLaunchMode"), &iv)
                      == KN_JSON_OK && iv == 1,
                  "appLaunchMode 1 (\"default\", :2979-2985)");
            CHECK(kn_json_int(kn_json_get(&d, srd, "userAge"), &iv)
                      == KN_JSON_OK && iv == 25,
                  "userAge 25 fallback (:1740)");
            f = kn_json_get(&d, srd, "requestedStreamingFeatures");
            dlss = kn_json_get(&d, f, "dlssOverrides");
            CHECK(dlss != NULL && dlss->type == KN_J_OBJ &&
                  kn_json_int(kn_json_get(&d, dlss, "srScale"), &iv)
                      == KN_JSON_OK && iv == 100,
                  "dlssOverrides = default_overrides object (dlss.rs :22)");
            md = kn_json_get(&d, srd, "metaData");
            CHECK(kn_json_str_copy(kn_json_get(&d, kn_json_at(&d, md, 1),
                                               "value"),
                                   sv2, sizeof sv2) == KN_JSON_OK &&
                  strcmp(sv2, "device-hash-abc") == 0,
                  "SubSessionId falls back to the device id when unset");
        }
    }
    {   /* F37: a supplied fresh SubSessionId lands verbatim */
        st.subsession_id = "6406b345-f343-4fd5-8489-729bcdd01f4d";
        CHECK(kn_cm_build_create_body(&st, body, sizeof body, &n) ==
                  KN_HTTP_OK &&
              strstr(body, "{\"key\":\"SubSessionId\",\"value\":"
                           "\"6406b345-f343-4fd5-8489-729bcdd01f4d\"}")
                  != NULL,
              "SubSessionId = the fresh per-draw UUID (random_uuid :1675)");
        st.subsession_id = NULL;
    }
    st.fps = 120;
    CHECK(kn_cm_build_create_body(&st, body, sizeof body, &n) == KN_HTTP_OK &&
          strstr(body, "\"reflex\":true") != NULL,
          "reflex true at fps >= 120 (:1674)");
    st.fps = 30;

    CHECK(kn_cm_auth_header(&st, auth, sizeof auth) == KN_HTTP_OK &&
          strcmp(auth, "GFNJWT TOKEN123") == 0, "auth header format (:2040)");
    {
        int i, nh = kn_cm_fixed_headers(&st, hh, 22);
        int have_ct = 0, have_id = 0, have_ua = 0, have_ident = 0,
            have_os = 0, have_type = 0, have_make = 0, have_model = 0,
            have_xdev = 0;
        CHECK(nh == 12, "twelve fixed native headers (F37: +UA +identity)");
        for (i = 0; i < nh; ++i) {
            if (strcmp(hh[i].name, "Content-Type") == 0 &&
                strcmp(hh[i].value, "text/plain") == 0) have_ct = 1;
            if (strcmp(hh[i].name, "nv-client-id") == 0 &&
                strcmp(hh[i].value, KN_CM_LCARS_CLIENT_ID) == 0) have_id = 1;
            if (strcmp(hh[i].name, "user-agent") == 0 &&
                strcmp(hh[i].value, KN_CM_BIFROST_UA) == 0) have_ua = 1;
            if (strcmp(hh[i].name, "x-nv-client-identity") == 0 &&
                strcmp(hh[i].value, KN_CM_BIFROST_UA) == 0) have_ident = 1;
            if (strcmp(hh[i].name, "nv-device-os") == 0 &&
                strcmp(hh[i].value, "WINDOWS") == 0) have_os = 1;
            if (strcmp(hh[i].name, "nv-device-type") == 0 &&
                strcmp(hh[i].value, "DESKTOP") == 0) have_type = 1;
            if (strcmp(hh[i].name, "nv-device-make") == 0 &&
                strcmp(hh[i].value, "UNKNOWN") == 0) have_make = 1;
            if (strcmp(hh[i].name, "nv-device-model") == 0 &&
                strcmp(hh[i].value, "UNKNOWN") == 0) have_model = 1;
            if (strcmp(hh[i].name, "x-device-id") == 0 &&
                strcmp(hh[i].value, "device-hash-abc") == 0) have_xdev = 1;
        }
        CHECK(have_ct, "Content-Type text/plain quirk (:2270)");
        CHECK(have_id, "nv-client-id = LCARS (:2271)");
        CHECK(have_ua, "Bifrost user-agent (:2266, :3025)");
        CHECK(have_ident, "x-nv-client-identity = the UA (:2285)");
        CHECK(have_os && have_type && have_make && have_model,
              "device quartet WINDOWS/DESKTOP/UNKNOWN/UNKNOWN (:2999-3023)");
        CHECK(have_xdev, "x-device-id = settings device_id (:2284)");
        CHECK(strcmp(hh[0].name, "user-agent") == 0,
              "UA is the first header (capture order, :2266)");
        for (i = 0; i < nh; ++i)
            if (strstr(hh[i].name, "uthorization")) { CHECK(0, "no auth here"); }
        CHECK(1, "Authorization deliberately NOT in the fixed table");
        CHECK(kn_cm_fixed_headers(&st, hh, 10) == 0,
              "native table refuses a too-small max (<12)");
    }

    /* F41 (round N slice 4r): the RUN-46 identity profile — the header
     * table that flew every long session in project history (run 46:
     * 1020.0 s keeper + 1174.8 s + 1084.9 s; runs 60/61: two 1200 s
     * keepers) and that F37 replaced, after which the ceiling collapsed
     * to 317.6 s (run 102), 21.0 s (run 103) and 15.9 s (run 104) —
     * 0/8 draws over 21 s on the F37 identity. */
    {
        int i, nh;
        int have_ua = 0, have_ident = 0, have_ct = 0, have_os = 0,
            have_type = 0, have_make = 0, have_model = 0, have_ver = 0,
            have_streamer = 0, have_ctype = 0, have_xdev = 0;
        st.profile = 2;
        nh = kn_cm_fixed_headers(&st, hh, 22);
        CHECK(nh == 10, "r46: exactly the ten run-46 headers");
        for (i = 0; i < nh; ++i) {
            if (strcmp(hh[i].name, "user-agent") == 0) have_ua = 1;
            if (strcmp(hh[i].name, "x-nv-client-identity") == 0)
                have_ident = 1;
            if (strcmp(hh[i].name, "Content-Type") == 0 &&
                strcmp(hh[i].value, "text/plain") == 0) have_ct = 1;
            if (strcmp(hh[i].name, "nv-device-os") == 0 &&
                strcmp(hh[i].value, "Windows 7") == 0) have_os = 1;
            if (strcmp(hh[i].name, "nv-device-type") == 0 &&
                strcmp(hh[i].value, "PC") == 0) have_type = 1;
            if (strcmp(hh[i].name, "nv-device-make") == 0 &&
                strcmp(hh[i].value, "Dell") == 0) have_make = 1;
            if (strcmp(hh[i].name, "nv-device-model") == 0 &&
                strcmp(hh[i].value, "Latitude E5500") == 0) have_model = 1;
            if (strcmp(hh[i].name, "nv-client-version") == 0 &&
                strcmp(hh[i].value, KN_CM_CLIENT_VERSION) == 0) have_ver = 1;
            if (strcmp(hh[i].name, "nv-client-streamer") == 0 &&
                strcmp(hh[i].value, "NVIDIA-CLASSIC") == 0) have_streamer = 1;
            if (strcmp(hh[i].name, "nv-client-type") == 0 &&
                strcmp(hh[i].value, "NATIVE") == 0) have_ctype = 1;
            if (strcmp(hh[i].name, "x-device-id") == 0 &&
                strcmp(hh[i].value, "device-hash-abc") == 0) have_xdev = 1;
        }
        CHECK(!have_ua, "r46: NO user-agent header at all (F37 added it)");
        CHECK(!have_ident, "r46: NO x-nv-client-identity (F37 added it)");
        CHECK(have_ct, "r46: the text/plain quirk survives");
        CHECK(have_os && have_type && have_make && have_model,
              "r46: the device quartet is the run-46 chimera "
              "\"Windows 7\"/PC/Dell/\"Latitude E5500\"");
        CHECK(have_ver && have_streamer && have_ctype && have_xdev,
              "r46: version/streamer/client-type/x-device-id identical to "
              "native — the profile isolates the UA + device delta");
        CHECK(strcmp(hh[0].name, "Content-Type") == 0,
              "r46: Content-Type is FIRST (the run-46 order, no UA ahead)");
        CHECK(kn_cm_fixed_headers(&st, hh, 9) == 0,
              "r46: refuses a too-small max (<10)");
        st.profile = 0;
        nh = kn_cm_fixed_headers(&st, hh, 22);
        CHECK(nh == 12 && strcmp(hh[0].name, "user-agent") == 0,
              "profile 0 still yields the F37 native table (no regression)");
    }
}

static void test_cm_network(void)
{
    static hio h;
    kn_cm_settings st;
    kn_cm_session s;
    int status = 0, rc;

    printf("-- kn_cm networked create/poll (scripted server) --\n");
    memset(&st, 0, sizeof st);
    st.token = "T0KEN"; st.device_id = "dev-1"; st.app_id = 42;
    st.title = "T"; st.width = 1280; st.height = 720; st.fps = 30;

    hio_reset(&h);
    {   char framed[HIO_CAP / 2];
        int n = snprintf(framed, sizeof framed,
                         "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n%s",
                         (unsigned long)strlen(k_session_ready),
                         k_session_ready);
        hio_queue(&h, framed);
        (void)n;
    }
    g_now = 0;
    rc = kn_cm_create(hio_io(&h), KN_CM_DEFAULT_BASE_HOST, "/", &st, 1000,
                      fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                      sizeof g_body, &status, &s);
    CHECK(rc == KN_HTTP_OK && status == 200, "create round ok");
    CHECK(strcmp(s.session_id, "SESS-1") == 0 && s.ready,
          "create extracted the ready session");
    h.from[h.flen] = '\0';
    CHECK(strstr((char *)h.from, "POST /v2/session HTTP/1.1\r\n") ==
          (char *)h.from, "create used POST v2/session (:363)");
    CHECK(strstr((char *)h.from, "Host: prod.cloudmatchbeta.nvidiagrid.net")
          != NULL, "default CloudMatch host (:38)");
    CHECK(strstr((char *)h.from, "Authorization: GFNJWT T0KEN\r\n") != NULL,
          "GFNJWT authorization (:2040)");
    CHECK(strstr((char *)h.from, "nv-client-type: NATIVE\r\n") != NULL,
          "native client identity");
    CHECK(strstr((char *)h.from, "\"sessionRequestData\"") != NULL,
          "body carried");
    /* the response buffer holds SERVER bytes only; the token lived in a
     * stack buffer that cm_round scrubs (memset) before returning */
    CHECK(strstr(g_body, "SESS-1") != NULL &&
          strstr(g_body, "T0KEN") == NULL,
          "response body carries the session, never the token");

    hio_reset(&h);
    {   char framed[HIO_CAP / 2];
        snprintf(framed, sizeof framed,
                 "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n%s",
                 (unsigned long)strlen(k_session_ready), k_session_ready);
        hio_queue(&h, framed);
    }
    rc = kn_cm_poll(hio_io(&h), KN_CM_DEFAULT_BASE_HOST, "/", &st, "SESS-1",
                    1000, fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                    sizeof g_body, &status, &s);
    CHECK(rc == KN_HTTP_OK && status == 200 && s.ready, "poll round ok");
    h.from[h.flen] = '\0';
    CHECK(strstr((char *)h.from, "GET /v2/session/SESS-1 HTTP/1.1\r\n") ==
          (char *)h.from, "poll used GET v2/session/{id} (:534)");

    /* non-200 surfaces via http_status without extraction */
    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0"
                  "\r\n\r\n");
    rc = kn_cm_poll(hio_io(&h), KN_CM_DEFAULT_BASE_HOST, "/", &st, "SESS-1",
                    1000, fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                    sizeof g_body, &status, &s);
    CHECK(rc == KN_HTTP_OK && status == 503, "503 passes through as status");
}

/* ------------------------------------------- learned control-host gate */

static int cb_ok(const char *ip, unsigned port, const char *want)
{
    kn_cm_session s;
    char host[128];
    uint16_t p = 0;
    memset(&s, 0, sizeof s);
    snprintf(s.control_host, sizeof s.control_host, "%.127s", ip);
    s.control_port = (uint16_t)port;
    s.have_control = 1;
    if (!kn_cm_control_base(&s, host, sizeof host, &p)) return 0;
    return want && strcmp(host, want) == 0 && p == 443;
}

static void test_cm_control(void)
{
    kn_cm_session s;
    char host[128];
    uint16_t port = 0;

    printf("-- kn_cm control base (R7 run-1 fix) --\n");
    /* the exact R7 capture: the create response nominates the regional
     * node that owns the session (np-waw-01...) */
    CHECK(cb_ok("np-waw-01.cloudmatchbeta.nvidiagrid.net", 443,
                "np-waw-01.cloudmatchbeta.nvidiagrid.net"),
          "capture host accepted (:1149)");
    CHECK(cb_ok("https://np-waw-01.cloudmatchbeta.nvidiagrid.net", 443,
                "np-waw-01.cloudmatchbeta.nvidiagrid.net"),
          "https:// prefix stripped (:2415)");
    CHECK(!cb_ok("np-waw-01.cloudmatchbeta.nvidiagrid.net", 8443, NULL),
          "port != 443 rejected (:1976-1980)");
    CHECK(!cb_ok("http://np-waw-01.example", 443, NULL),
          "http:// scheme rejected (:2405)");
    /* private/loopback/link-local/CGNAT/multicast/reserved
     * (public_session_ipv4, :2443-2456) */
    CHECK(!cb_ok("10.0.0.1", 443, NULL), "private 10/8 rejected");
    CHECK(!cb_ok("172.16.5.5", 443, NULL), "private 172.16/12 rejected");
    CHECK(cb_ok("172.32.5.5", 443, "172.32.5.5"), "172.32/16 stays public");
    CHECK(!cb_ok("192.168.1.1", 443, NULL), "private 192.168/16 rejected");
    CHECK(!cb_ok("127.0.0.1", 443, NULL), "loopback rejected");
    CHECK(!cb_ok("169.254.1.1", 443, NULL), "link-local rejected");
    CHECK(!cb_ok("100.64.0.1", 443, NULL), "CGNAT 100.64/10 rejected");
    CHECK(!cb_ok("100.127.9.9", 443, NULL), "CGNAT upper edge rejected");
    CHECK(cb_ok("100.128.0.1", 443, "100.128.0.1"), "100.128 is public");
    CHECK(!cb_ok("224.0.0.1", 443, NULL), "multicast rejected");
    CHECK(!cb_ok("0.1.2.3", 443, NULL), "0/8 rejected (:2450)");
    CHECK(!cb_ok("240.1.2.3", 443, NULL), "reserved 240+ rejected (:2451)");
    CHECK(!cb_ok("255.255.255.255", 443, NULL), "broadcast rejected");
    CHECK(!cb_ok("010.1.2.3", 443, NULL), "leading-zero octet rejected");
    CHECK(!cb_ok("1.2.3.4.5", 443, NULL), "malformed IPv4 rejected");
    CHECK(cb_ok("8.238.0.113", 443, "8.238.0.113"), "public IPv4 accepted");
    /* hostname shape rules (:2427-2429) */
    CHECK(!cb_ok("localhost", 443, NULL), "dotless hostname rejected");
    CHECK(!cb_ok("rig.local", 443, NULL), ".local rejected");
    CHECK(!cb_ok("RIG.LOCAL", 443, NULL), ".LOCAL rejected (case-insens.)");
    CHECK(!cb_ok("bad host.example", 443, NULL), "space rejected");
    CHECK(!cb_ok("user:pw@seat.example", 443, NULL), "userinfo rejected");
    CHECK(!cb_ok("seat.example/path", 443, NULL), "path rejected");
    CHECK(!cb_ok("seat.example?x=1", 443, NULL), "query rejected");
    CHECK(!cb_ok("-seat.example", 443, NULL), "leading edge hyphen rejected");
    CHECK(!cb_ok("seat-.example", 443, NULL), "trailing edge hyphen rejected");
    CHECK(!cb_ok("seat..example", 443, NULL), "empty label rejected");
    CHECK(cb_ok("np-waw-01.example", 443, "np-waw-01.example"),
          "in-label hyphens/digits accepted");
    /* IPv6 (the documented simplified mirror — see kn_cm.c) */
    CHECK(cb_ok("2600:1f18::9", 443, "2600:1f18::9"), "public IPv6 accepted");
    CHECK(!cb_ok("::1", 443, NULL), "IPv6 loopback rejected");
    CHECK(!cb_ok("::", 443, NULL), "IPv6 unspecified rejected");
    CHECK(!cb_ok("fe80::1", 443, NULL), "IPv6 link-local rejected");
    CHECK(!cb_ok("FE80::1", 443, NULL), "IPv6 link-local rejected (case)");
    CHECK(!cb_ok("fd00::1", 443, NULL), "IPv6 ULA rejected");
    CHECK(!cb_ok("ff02::1", 443, NULL), "IPv6 multicast rejected");
    CHECK(!cb_ok("::ffff:127.0.0.1", 443, NULL),
          "IPv4-mapped loopback rejected");
    CHECK(!cb_ok("[::1]", 443, NULL), "bracketed loopback rejected");
    CHECK(cb_ok("[2600:1f18::9]", 443, "[2600:1f18::9]"),
          "bracketed public IPv6 kept verbatim");
    /* absent control info -> 0 (the caller falls back to the create host) */
    memset(&s, 0, sizeof s);
    CHECK(!kn_cm_control_base(&s, host, sizeof host, &port),
          "absent sessionControlInfo -> fallback");
    CHECK(!kn_cm_control_base(NULL, host, sizeof host, &port) &&
          !kn_cm_control_base(&s, NULL, 0, &port),
          "argument guards");

    /* extract fills the control fields — the ready fixture's 10.0.0.1 is
     * filled honestly but then rejected by the trust gate */
    CHECK(kn_cm_extract_session(k_session_ready, &s) == 0 && s.have_control &&
          strcmp(s.control_host, "10.0.0.1") == 0 && s.control_port == 443 &&
          s.req_status_code == 1,
          "extract fills control+requestStatus (ready fixture)");
    CHECK(!kn_cm_control_base(&s, host, sizeof host, &port),
          "private control IP rejected -> create-host fallback");

    {   /* the EXACT R7 capture shape (2026-10-25, np-waw-01) */
        static const char fx[] =
            "{\"requestStatus\":{\"statusCode\":1,"
            "\"statusDescription\":\"SUCCESS_STATUS\",\"serverId\":"
            "\"NP-WAW-01\"},\"session\":{\"sessionId\":"
            "\"480e517f-a5bc-4bb2-80b8-dcf3f77ed2fa\",\"status\":1,"
            "\"sessionControlInfo\":{\"protocol\":0,\"appLevelProtocol\":5,"
            "\"ip\":\"np-waw-01.cloudmatchbeta.nvidiagrid.net\","
            "\"resourcePath\":\"/v2/session/"
            "480e517f-a5bc-4bb2-80b8-dcf3f77ed2fa\",\"usage\":10,"
            "\"port\":443},\"connectionInfo\":null,"
            "\"seatSetupInfo\":{\"queuePosition\":159,\"seatSetupStep\":1,"
            "\"seatSetupEta\":30000},\"errorCode\":1}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0 && s.have_control &&
              strcmp(s.control_host,
                     "np-waw-01.cloudmatchbeta.nvidiagrid.net") == 0 &&
              s.control_port == 443 && s.status == 1 &&
              strcmp(s.phase, "preparing") == 0 &&
              s.have_queue && s.queue_position == 159 &&
              s.have_seat && s.seat_setup_step == 1 && !s.ready &&
              s.req_status_code == 1 && s.error_code == 1 &&
              strcmp(s.req_status_desc, "SUCCESS_STATUS") == 0,
              "R7 capture fixture extracts (queue 159, control np-waw-01, "
              "errorCode 1 = the queued-state marker)");
        CHECK(kn_cm_control_base(&s, host, sizeof host, &port) == 1 &&
              strcmp(host, "np-waw-01.cloudmatchbeta.nvidiagrid.net") == 0 &&
              port == 443,
              "captured control base is pollable");
    }
    {   /* absent port defaults to 443 (:1978 is_none_or(443)) */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S10\",\"status\":1,"
            "\"sessionControlInfo\":{\"ip\":\"np-waw-01.example\"}}}";
        CHECK(kn_cm_extract_session(fx, &s) == 0 && s.have_control &&
              s.control_port == 443, "absent control port defaults to 443");
    }
    {   /* a rejected payload still reports the cloud-side reason
         * (:2337-2352 — e.g. statusCode 11 SESSION_LIMIT, :2354+) */
        static const char fx[] =
            "{\"requestStatus\":{\"statusCode\":11,"
            "\"statusDescription\":\"SESSION_LIMIT_REACHED\","
            "\"unifiedErrorCode\":4073}}";
        CHECK(kn_cm_extract_session(fx, &s) == -1 && s.req_status_code == 11 &&
              s.unified_error == 4073 &&
              strcmp(s.req_status_desc, "SESSION_LIMIT_REACHED") == 0,
              "failed extract keeps requestStatus+unifiedErrorCode");
    }
    {   /* R7 run 8's 503 body: a session SKELETON — no sessionId and
         * sessionControlInfo.ip NULL — rejected by the hard gate, yet
         * every diagnostic field surfaces (round XXVII: the deep fields
         * a bounded body excerpt cannot show) */
        static const char fx[] =
            "{\"requestStatus\":{\"statusCode\":53,"
            "\"statusDescription\":\"SERVER_UNAVAILABLE\","
            "\"unifiedErrorCode\":77},\"session\":{\"sessionAds\":null,"
            "\"sessionControlInfo\":{\"protocol\":0,"
            "\"appLevelProtocol\":5,\"ip\":null,\"resourcePath\":"
            "\"/v2/session/479ecbcd\",\"port\":443},\"status\":1,"
            "\"errorCode\":1}}";
        CHECK(kn_cm_extract_session(fx, &s) == -1 && s.status == 1 &&
              s.error_code == 1 && s.req_status_code == 53 &&
              s.unified_error == 77 &&
              strcmp(s.req_status_desc, "SERVER_UNAVAILABLE") == 0 &&
              !s.have_control,
              "run-8 503 skeleton: rejected but fully diagnosed "
              "(ip:null -> no control host)");
    }
}

static void test_cm_cancel(void)
{
    static hio h;
    kn_cm_settings st;
    int status = 0, rc;

    printf("-- kn_cm cancel (DELETE v2/session/{id}) --\n");
    memset(&st, 0, sizeof st);
    st.token = "T0KEN"; st.device_id = "dev-1"; st.app_id = 42;
    st.title = "T"; st.width = 1280; st.height = 720; st.fps = 30;

    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 204 No Content\r\n\r\n");
    g_now = 0;
    rc = kn_cm_cancel(hio_io(&h), "np-waw-01.example", "/", &st, "SESS-1",
                      1000, fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                      sizeof g_body, &status);
    CHECK(rc == KN_HTTP_OK && status == 204,
          "cancel: 204 accepted (validate_delete_response :2728)");
    h.from[h.flen] = '\0';
    CHECK(strstr((char *)h.from, "DELETE /v2/session/SESS-1 HTTP/1.1\r\n") ==
          (char *)h.from, "cancel used DELETE v2/session/{id} (:789-800)");
    CHECK(strstr((char *)h.from, "Host: np-waw-01.example\r\n") != NULL,
          "cancel targets the given (control) host");
    CHECK(strstr((char *)h.from, "Authorization: GFNJWT T0KEN\r\n") != NULL,
          "cancel carries the GFNJWT auth header");

    hio_reset(&h);
    hio_queue(&h, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    rc = kn_cm_cancel(hio_io(&h), "np-waw-01.example", "/", &st, "SESS-1",
                      1000, fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                      sizeof g_body, &status);
    CHECK(rc == KN_HTTP_OK && status == 404,
          "cancel: 404 passes through (already gone)");

    hio_reset(&h);
    {   static const char k_delete_body[] =
            "{\"requestStatus\":{\"statusCode\":1,"
            "\"statusDescription\":\"SUCCESS_STATUS\"}}";
        char framed[512];
        snprintf(framed, sizeof framed,
                 "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n%s",
                 (unsigned long)strlen(k_delete_body), k_delete_body);
        hio_queue(&h, framed);
    }
    rc = kn_cm_cancel(hio_io(&h), "np-waw-01.example", "/", &st, "SESS-1",
                      1000, fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                      sizeof g_body, &status);
    CHECK(rc == KN_HTTP_OK && status == 200,
          "cancel: 200 + JSON body accepted without session extraction");

    hio_reset(&h);
    CHECK(kn_cm_cancel(hio_io(&h), "np-waw-01.example", "/", &st, "",
                       1000, fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                       sizeof g_body, &status) == KN_HTTP_ERR_ARGS,
          "cancel: empty session_id rejected");
}

/* F35 (round N slice 4n): the browser-parity profile — body and header
 * table must mirror the 2026-11-01 play.geforcenow.com wiretap
 * field-for-field (RTSP §57.2). */
static void test_cm_browser_profile(void)
{
    kn_cm_settings st;
    static char body[8192];
    kn_http_header hh[24];
    size_t n = 0;
    int i, nh;

    printf("-- kn_cm browser profile (F35) --\n");
    memset(&st, 0, sizeof st);
    st.token = "TOKEN123";
    st.device_id = "bab41204-5dd9-414f-b522-2f0ad181adc0";
    st.app_id = 107057914;
    st.title = "Ignored In Browser Profile";
    st.width = 1280; st.height = 720; st.fps = 30;
    st.timezone_offset_ms = 3600000;
    st.profile = 1;
    st.user_age = 34;

    CHECK(kn_cm_build_create_body(&st, body, sizeof body, &n) ==
              KN_HTTP_ERR_ARGS,
          "browser profile requires subsession_id + tz_name");
    st.subsession_id = "3fe004f6-e388-410d-9d22-170a968afdce";
    st.tz_name = "Europe/Warsaw";
    CHECK(kn_cm_build_create_body(&st, body, sizeof body, &n) ==
              KN_HTTP_OK && n > 1000,
          "browser body builds");
    CHECK(strncmp(body,
              "{\"sessionRequestData\":{\"appId\":107057914,\"audioMode\":2,"
              "\"remoteControllersBitmap\":0,\"sdrHdrMode\":0,"
              "\"networkTestSessionId\":null,"
              "\"availableSupportedControllers\":[],"
              "\"clientVersion\":\"30.0\",\"deviceHashId\":\"bab41204-",
              158) == 0,
          "key order mirrors the capture (prefix)");

    CHECK(strstr(body, "\"clientPlatformName\":\"browser\"") != NULL,
          "platform browser");
    CHECK(strstr(body, "\"sdkVersion\":\"1.0\"") != NULL, "sdkVersion 1.0");
    CHECK(strstr(body, "\"streamerVersion\":1,") != NULL,
          "streamerVersion the NUMBER 1");
    CHECK(strstr(body, "\"secureRTSPSupported\":false") != NULL,
          "secureRTSP false");
    CHECK(strstr(body, "\"enhancedStreamMode\":1") != NULL,
          "enhancedStreamMode 1");
    CHECK(strstr(body, "\"appLaunchMode\":1") != NULL, "appLaunchMode 1");
    CHECK(strstr(body, "\"internalTitle\":null") != NULL,
          "internalTitle null (title ignored)");
    CHECK(strstr(body, "\"partnerCustomData\":\"\"") != NULL,
          "partnerCustomData empty string");
    CHECK(strstr(body, "\"userAge\":34") != NULL, "userAge from settings");
    CHECK(strstr(body, "\"clientTimezoneOffset\":3600000") != NULL,
          "tz offset passthrough");
    CHECK(strstr(body, "\"clientIdentification\":\"GFN-PC\"") != NULL,
          "GFN-PC kept");

    CHECK(strstr(body, "{\"key\":\"SubSessionId\","
                       "\"value\":\"3fe004f6-e388-410d-9d22-170a968afdce\"}")
              != NULL, "metaData SubSessionId fresh UUID");
    CHECK(strstr(body, "{\"key\":\"clientTimezone\","
                       "\"value\":\"Europe/Warsaw\"}") != NULL,
          "metaData clientTimezone");
    CHECK(strstr(body, "{\"key\":\"GSStreamerType\",\"value\":\"WebRTC\"}")
              != NULL, "metaData GSStreamerType WebRTC");
    CHECK(strstr(body, "{\"key\":\"networkType\",\"value\":\"Unknown\"}")
              != NULL, "metaData networkType");
    CHECK(strstr(body, "{\"key\":\"ClientImeSupport\",\"value\":\"0\"}")
              != NULL, "metaData ClientImeSupport");
    CHECK(strstr(body, "\\\"horizontalPixels\\\":1280,"
                       "\\\"verticalPixels\\\":720") != NULL,
          "metaData clientPhysicalResolution");

    CHECK(strstr(body, "\"displayData\":{\"desiredContentMaxLuminance\":0,")
              != NULL, "displayData luminance zeros");
    CHECK(strstr(body, "\"dpi\":0}") != NULL, "dpi 0");
    CHECK(strstr(body, "monitorId") == NULL, "no monitorId");
    CHECK(strstr(body, "hdr10PlusGamingData") == NULL, "no hdr10Plus");

    CHECK(strstr(body, "\"profile\":1") != NULL, "features profile 1");
    CHECK(strstr(body, "\"dlssOverrides\":{\"srMode\":0,\"srModel\":0,"
                       "\"srScale\":100,\"rrModel\":0,\"fgMode\":0,"
                       "\"fgModel\":0}") != NULL, "dlssOverrides object");
    CHECK(strstr(body, "preferredController") == NULL,
          "no preferredController");
    CHECK(strstr(body, "requestedAudioFormat") == NULL,
          "no requestedAudioFormat");
    CHECK(strstr(body, "\"transport\"") == NULL, "no transport");
    CHECK(strstr(body, "mouseMovementFlags") == NULL,
          "no mouseMovementFlags");
    CHECK(strstr(body, "trueHdr") == NULL, "no trueHdr");
    CHECK(strstr(body, "supportedHidDevices") == NULL,
          "no supportedHidDevices");
    CHECK(strstr(body, "Ignored") == NULL, "no title leak");

    CHECK(strstr(body, "\"reflex\":false") != NULL, "reflex false at 30fps");
    st.fps = 120;
    CHECK(kn_cm_build_create_body(&st, body, sizeof body, &n) ==
              KN_HTTP_OK && strstr(body, "\"reflex\":true") != NULL,
          "reflex true at 120fps");
    st.fps = 30;
    st.user_age = 0;
    CHECK(kn_cm_build_create_body(&st, body, sizeof body, &n) ==
              KN_HTTP_OK && strstr(body, "\"userAge\":25") != NULL,
          "userAge falls back to 25");
    st.user_age = 34;

    nh = kn_cm_fixed_headers(&st, hh, 22);
    CHECK(nh == 22, "22 browser fixed headers");
    {
        int got_type = 0, got_streamer = 0, got_ver = 0, got_ct = 0,
            got_ua = 0, got_origin = 0, got_xdev = 0, got_tp = 0,
            got_make = 0, got_model = 0, got_os = 0, got_dtype = 0,
            got_btype = 0, got_lang = 0, got_chua = 0, j;
        char tp1[64];
        for (i = 0; i < nh; i++) {
            const char *N = hh[i].name, *V = hh[i].value;
            if (!strcmp(N, "nv-client-type"))
                got_type = !strcmp(V, "BROWSER");
            else if (!strcmp(N, "nv-client-streamer"))
                got_streamer = !strcmp(V, "WEBRTC");
            else if (!strcmp(N, "nv-browser-type"))
                got_btype = !strcmp(V, "CHROME");
            else if (!strcmp(N, "nv-client-version"))
                got_ver = !strcmp(V, KN_CM_CLIENT_VERSION_BROWSER);
            else if (!strcmp(N, "content-type"))
                got_ct = !strcmp(V, "application/json");
            else if (!strcmp(N, "user-agent"))
                got_ua = strstr(V, "Chrome/109.0.0.0") != NULL;
            else if (!strcmp(N, "origin"))
                got_origin = !strcmp(V, "https://play.geforcenow.com");
            else if (!strcmp(N, "x-device-id"))
                got_xdev = !strcmp(V, st.device_id);
            else if (!strcmp(N, "nv-device-make"))
                got_make = !strcmp(V, "UNKNOWN");
            else if (!strcmp(N, "nv-device-model"))
                got_model = !strcmp(V, "UNKNOWN");
            else if (!strcmp(N, "nv-device-os"))
                got_os = !strcmp(V, "WINDOWS");
            else if (!strcmp(N, "nv-device-type"))
                got_dtype = !strcmp(V, "DESKTOP");
            else if (!strcmp(N, "accept-language"))
                got_lang = strstr(V, "pl-PL") != NULL;
            else if (!strcmp(N, "sec-ch-ua"))
                got_chua = strstr(V, "Google Chrome") != NULL;
            else if (!strcmp(N, "traceparent")) {
                /* 00-<32 hex>-<16 hex>-01 */
                got_tp = strlen(V) == 55 && V[2] == '-' && V[35] == '-' &&
                         V[52] == '-' && V[53] == '0' && V[54] == '1' &&
                         V[0] == '0' && V[1] == '0';
                for (j = 3; j < 52 && got_tp; j++)
                    if (j != 35 && !isxdigit((unsigned char)V[j]))
                        got_tp = 0;
                snprintf(tp1, sizeof tp1, "%s", V);
            }
            if (!strcmp(N, "Content-Type"))
                CHECK(0, "legacy text/plain absent from browser profile");
            if (strstr(N, "uthorization"))
                CHECK(0, "Authorization never in the fixed table");
        }
        CHECK(got_type && got_streamer && got_btype,
              "BROWSER/WEBRTC/CHROME identity headers");
        CHECK(got_ver, "nv-client-version 2.0.89.154");
        CHECK(got_ct, "content-type application/json (no text/plain quirk)");
        CHECK(got_ua, "Chrome-109 user-agent");
        CHECK(got_origin, "origin play.geforcenow.com");
        CHECK(got_xdev, "x-device-id = settings device_id");
        CHECK(got_make && got_model && got_os && got_dtype,
              "UNKNOWN make/model + WINDOWS/DESKTOP");
        CHECK(got_lang && got_chua, "accept-language + sec-ch-ua");
        CHECK(got_tp, "traceparent 00-<32hex>-<16hex>-01 shape");
        (void)tp1;
    }
    CHECK(kn_cm_fixed_headers(&st, hh, 10) == 0,
          "browser table refuses a too-small max");
}

/* ---- F36 (run-100 read): the queue-ad gate — sessionAds extraction +
 * the {"action":6,"adUpdates":[...]} report body (key order transcribed
 * from the browser IMA PUT in the R2 capture) + the PUT round. ---- */
static void test_cm_ads(void)
{
    static hio h;
    kn_cm_settings st;
    kn_cm_session s;
    char body[512];
    size_t blen = 0;
    int status = 0, rc;

    printf("-- kn_cm queue ads (F36) --\n");

    /* extraction: sessionAds[0] + sessionAdsRequired (run-100 shape —
     * sessionAds is the FIRST key of the session object) */
    {
        static const char k_poll[] =
            "{\"session\":{"
            "\"sessionAds\":[{\"adLengthInSeconds\":120,\"adMediaFiles\":[],"
            "\"adId\":\"641ab9a0-33d1-47fc-b230-54077ac9e5fa\","
            "\"adState\":1,\"adUrl\":\"https://pubads.g.doubleclick.net/"
            "gampad/ads?env=vp\"}],"
            "\"sessionAdsRequired\":true,"
            "\"sessionId\":\"SESS-1\",\"status\":1,\"queuePosition\":3}}";
        memset(&s, 0, sizeof s);
        rc = kn_cm_extract_session(k_poll, &s);
        CHECK(rc == 0 && s.have_ad == 1, "ads: offer extracted (have_ad)");
        CHECK(!strcmp(s.ad_id, "641ab9a0-33d1-47fc-b230-54077ac9e5fa"),
              "ads: adId transcribed");
        CHECK(s.ad_state == 1 && s.ad_len_s == 120,
              "ads: adState=1, adLengthInSeconds=120");
        CHECK(!strncmp(s.ad_url, "https://pubads.g.doubleclick.net/", 33),
              "ads: adUrl kept");
        CHECK(s.ads_required == 1, "ads: sessionAdsRequired=true");
        CHECK(s.queue_position == 3 && s.status == 1,
              "ads: queue fields coexist with the ad block");
    }
    {   /* no offer: sessionAds null / empty */
        static const char k_none[] =
            "{\"session\":{\"sessionAds\":null,\"sessionAdsRequired\":false,"
            "\"sessionId\":\"SESS-1\",\"status\":1}}";
        static const char k_empty[] =
            "{\"session\":{\"sessionAds\":[],"
            "\"sessionId\":\"SESS-1\",\"status\":1}}";
        memset(&s, 0, sizeof s);
        CHECK(kn_cm_extract_session(k_none, &s) == 0 && s.have_ad == 0 &&
              s.ads_required == 0, "ads: sessionAds null -> no offer");
        memset(&s, 0, sizeof s);
        CHECK(kn_cm_extract_session(k_empty, &s) == 0 && s.have_ad == 0,
              "ads: sessionAds [] -> no offer");
    }
    {   /* a malformed ad entry (no adId) must not set have_ad */
        static const char k_bad[] =
            "{\"session\":{\"sessionAds\":[{\"adState\":1}],"
            "\"sessionId\":\"SESS-1\",\"status\":1}}";
        memset(&s, 0, sizeof s);
        CHECK(kn_cm_extract_session(k_bad, &s) == 0 && s.have_ad == 0,
              "ads: entry without adId is not an offer");
    }

    /* body builder: the cancel shape is byte-identical to the browser's
     * own IMA PUT (gfn_ref_h2.json stream 15): adId, adAction,
     * clientTimestamp, cancelReason, watchedTimeInMs, pausedTimeInMs,
     * adDurationInMs */
    rc = kn_cm_build_ad_body("0f731652-dfcb-4bfc-b89a-c976a15055a9",
                             KN_CM_AD_CANCEL, 1793523113, 0, 0, 0,
                             "VAST_NO_ADS_AFTER_WRAPPER",
                             body, sizeof body, &blen);
    CHECK(rc == KN_HTTP_OK &&
          !strcmp(body,
                  "{\"action\":6,\"adUpdates\":[{"
                  "\"adId\":\"0f731652-dfcb-4bfc-b89a-c976a15055a9\","
                  "\"adAction\":5,\"clientTimestamp\":1793523113,"
                  "\"cancelReason\":\"VAST_NO_ADS_AFTER_WRAPPER\","
                  "\"watchedTimeInMs\":0,\"pausedTimeInMs\":0,"
                  "\"adDurationInMs\":0}]}"),
          "ads: cancel body byte-mirrors the captured IMA PUT");
    rc = kn_cm_build_ad_body("AD-1", KN_CM_AD_START, 123, 0, 0, 0, NULL,
                             body, sizeof body, &blen);
    CHECK(rc == KN_HTTP_OK && blen == strlen(body) &&
          !strcmp(body,
                  "{\"action\":6,\"adUpdates\":[{\"adId\":\"AD-1\","
                  "\"adAction\":1,\"clientTimestamp\":123,"
                  "\"watchedTimeInMs\":0,\"pausedTimeInMs\":0,"
                  "\"adDurationInMs\":0}]}"),
          "ads: start body omits cancelReason (OpenNOW conditional key)");
    rc = kn_cm_build_ad_body("AD-1", KN_CM_AD_FINISH, 999, 120000, 0,
                             120000, "", body, sizeof body, &blen);
    CHECK(rc == KN_HTTP_OK && strstr(body, "\"adAction\":4") &&
          strstr(body, "\"watchedTimeInMs\":120000") &&
          strstr(body, "\"adDurationInMs\":120000") &&
          !strstr(body, "cancelReason"),
          "ads: finish body (empty reason string omits the key)");
    CHECK(kn_cm_build_ad_body("", KN_CM_AD_START, 0, 0, 0, 0, NULL,
                              body, sizeof body, &blen) == KN_HTTP_ERR_ARGS,
          "ads: empty adId refused");
    CHECK(kn_cm_build_ad_body("AD-1", 0, 0, 0, 0, 0, NULL,
                              body, sizeof body, &blen) == KN_HTTP_ERR_ARGS &&
          kn_cm_build_ad_body("AD-1", 6, 0, 0, 0, 0, NULL,
                              body, sizeof body, &blen) == KN_HTTP_ERR_ARGS,
          "ads: adAction outside 1..5 refused");
    CHECK(kn_cm_build_ad_body("AD-1", KN_CM_AD_START, 0, -5, -1, -9, NULL,
                              body, sizeof body, &blen) == KN_HTTP_OK &&
          strstr(body, "\"watchedTimeInMs\":0") &&
          strstr(body, "\"pausedTimeInMs\":0") &&
          strstr(body, "\"adDurationInMs\":0"),
          "ads: negative times clamp to 0");
    CHECK(kn_cm_build_ad_body("AD-1", KN_CM_AD_START, 0, 0, 0, 0, NULL,
                              body, 40, &blen) == KN_HTTP_ERR_BIG,
          "ads: undersized buffer -> ERR_BIG, no overflow");

    /* networked PUT on the control host */
    memset(&st, 0, sizeof st);
    st.token = "T0KEN"; st.device_id = "dev-1"; st.app_id = 42;
    st.title = "T"; st.width = 1280; st.height = 720; st.fps = 30;
    hio_reset(&h);
    {   static const char k_upd[] =
            "{\"requestStatus\":{\"statusCode\":1},"
            "\"session\":{\"sessionId\":\"SESS-1\",\"status\":1,"
            "\"sessionAds\":null,\"queuePosition\":2}}";
        char framed[512];
        snprintf(framed, sizeof framed,
                 "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n%s",
                 (unsigned long)strlen(k_upd), k_upd);
        hio_queue(&h, framed);
    }
    g_now = 0;
    memset(&s, 0, sizeof s);
    rc = kn_cm_report_ad(hio_io(&h), "np-waw-01.example", "/", &st,
                         "SESS-1", "AD-1", KN_CM_AD_START, 0, 0, 0, NULL,
                         1000, fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                         sizeof g_body, &status, &s);
    CHECK(rc == KN_HTTP_OK && status == 200, "ads: PUT 200 accepted");
    h.from[h.flen] = '\0';
    CHECK(strstr((char *)h.from, "PUT /v2/session/SESS-1 HTTP/1.1\r\n") ==
          (char *)h.from, "ads: PUT targets the control host's session URL");
    CHECK(strstr((char *)h.from, "{\"action\":6,\"adUpdates\":[{\"adId\":"
                 "\"AD-1\",\"adAction\":1,") != NULL,
          "ads: the report body rides the PUT");
    CHECK(!strcmp(s.session_id, "SESS-1") && s.queue_position == 2 &&
          s.have_ad == 0,
          "ads: the PUT response re-fills the session (queue resumes)");
    CHECK(kn_cm_report_ad(hio_io(&h), "h", "/", &st, "SESS-1", "",
                          KN_CM_AD_START, 0, 0, 0, NULL, 1000, fake_now,
                          NULL, g_rxb, sizeof g_rxb, g_body, sizeof g_body,
                          &status, NULL) == KN_HTTP_ERR_ARGS,
          "ads: missing adId refused before any I/O");
}

/* ---- F38 (run-102 read): session resume — the 403 conflict payload
 * names the account's live session; attach instead of re-queueing.
 * The fixture is run-102 draw 3's REAL 403 body (draw 2's orphaned
 * seat), reduced to the load-bearing keys. ---- */
static const char k_conflict_403[] =
    "{\"session\":{\"sessionControlInfo\":{\"protocol\":0,"
      "\"appLevelProtocol\":5,"
      "\"ip\":\"np-waw-01.cloudmatchbeta.nvidiagrid.net\","
      "\"usage\":10,\"port\":443},"
    "\"sessionRequestData\":{\"appId\":107057914,\"appLaunchMode\":1,"
      "\"clientPlatformName\":\"Windows\"},"
    "\"sessionId\":\"1ce6d5f8-b77d-4516-a2d7-e450db59627b\",\"status\":1},"
    "\"requestStatus\":{\"statusDescription\":"
      "\"SESSION_LIMIT_EXCEEDED_STATUS 4AF1201B\",\"statusCode\":11},"
    "\"otherUserSessions\":[{"
      "\"sessionControlInfo\":{\"ip\":"
        "\"np-ams-05.cloudmatchbeta.nvidiagrid.net\",\"port\":443},"
      "\"connectionInfo\":[{\"protocol\":1,\"appLevelProtocol\":6,"
        "\"resourcePath\":\"rtsps://80-84-170-154.cloudmatchbeta."
        "nvidiagrid.net:322\",\"usage\":14,\"port\":322}],"
      "\"sessionRequestData\":{\"appId\":107057914,\"appLaunchMode\":1,"
        "\"enablePersistingInGameSettings\":false,"
        "\"clientPlatformName\":\"Windows\"},"
      "\"sessionId\":\"c3731ac8-d0ea-4577-a9ab-78dc539ad390\","
      "\"status\":3}]}";

static void test_cm_resume(void)
{
    static hio h;
    kn_cm_settings st;
    kn_cm_conflict cf;
    kn_cm_session s;
    char body[6144];
    size_t blen = 0;
    int status = 0, rc;

    printf("-- kn_cm session resume (F38) --\n");

    /* the run-102 payload: otherUserSessions[0] is the candidate; the
     * fresh `session` (status 1) is NOT resumable (:1506-1515). */
    memset(&cf, 0, sizeof cf);
    rc = kn_cm_parse_conflict(k_conflict_403, &cf);
    CHECK(rc == 0, "resume: run-102 403 parses");
    CHECK(!strcmp(cf.session_id, "c3731ac8-d0ea-4577-a9ab-78dc539ad390"),
          "resume: the ORPHAN's sessionId (otherUserSessions first)");
    CHECK(!strcmp(cf.control_host, "np-ams-05.cloudmatchbeta.nvidiagrid.net")
          && cf.control_port == 443,
          "resume: the orphan's own control node + 443");
    CHECK(cf.status == 3 && cf.app_id == 107057914,
          "resume: status 3 (resumable) + appId echoed");
    CHECK(cf.echo_app_launch_mode == 1 && cf.echo_persist == 0 &&
          !strcmp(cf.echo_platform, "Windows"),
          "resume: echoed appLaunchMode/persist/platform captured");
    {   /* status 1 only -> nothing resumable */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S1\",\"status\":1,"
            "\"sessionControlInfo\":{\"ip\":\"np-waw-01.cloudmatchbeta."
            "nvidiagrid.net\",\"port\":443},"
            "\"sessionRequestData\":{\"appId\":107057914}}}";
        CHECK(kn_cm_parse_conflict(fx, &cf) != 0,
              "resume: status 1 is not resumable (:1506-1515)");
    }
    {   /* status 6 (terminal-ish) is not resumable either */
        static const char fx[] =
            "{\"session\":{\"sessionId\":\"S6\",\"status\":6,"
            "\"sessionControlInfo\":{\"ip\":\"np-waw-01.cloudmatchbeta."
            "nvidiagrid.net\",\"port\":443},"
            "\"sessionRequestData\":{\"appId\":1}}}";
        CHECK(kn_cm_parse_conflict(fx, &cf) != 0,
              "resume: status 6 is not resumable");
    }
    {   /* an untrusted (private) control host is refused */
        static const char fx[] =
            "{\"otherUserSessions\":[{"
            "\"sessionControlInfo\":{\"ip\":\"10.0.0.1\",\"port\":443},"
            "\"sessionRequestData\":{\"appId\":99},"
            "\"sessionId\":\"SX\",\"status\":2}]}";
        CHECK(kn_cm_parse_conflict(fx, &cf) != 0,
              "resume: a private control host never redirects the PUT");
    }
    {   /* a non-443 control port is refused (:1976-1980) */
        static const char fx[] =
            "{\"otherUserSessions\":[{"
            "\"sessionControlInfo\":{\"ip\":\"np-ams-05.cloudmatchbeta."
            "nvidiagrid.net\",\"port\":8443},"
            "\"sessionRequestData\":{\"appId\":99},"
            "\"sessionId\":\"SX\",\"status\":2}]}";
        CHECK(kn_cm_parse_conflict(fx, &cf) != 0,
              "resume: port != 443 refused");
    }
    {   /* appId <= 0 is refused (:893-894) */
        static const char fx[] =
            "{\"otherUserSessions\":[{"
            "\"sessionControlInfo\":{\"ip\":\"np-ams-05.cloudmatchbeta."
            "nvidiagrid.net\",\"port\":443},"
            "\"sessionRequestData\":{\"appId\":0},"
            "\"sessionId\":\"SX\",\"status\":2}]}";
        CHECK(kn_cm_parse_conflict(fx, &cf) != 0,
              "resume: appId 0 refused");
    }

    /* the RESUME body: the reference's subset, in its key order, with
     * the echoed trio appended (:1546-1597). NO renegotiation keys. */
    memset(&st, 0, sizeof st);
    st.token = "T0KEN"; st.device_id = "dev-1"; st.app_id = 42;
    st.title = "T"; st.width = 1280; st.height = 720; st.fps = 30;
    st.subsession_id = "SUB-9"; st.timezone_offset_ms = 3600000;
    rc = kn_cm_parse_conflict(k_conflict_403, &cf);
    CHECK(rc == 0, "resume: reparse for the body leg");
    rc = kn_cm_build_resume_body(&st, &cf, body, sizeof body, &blen);
    CHECK(rc == KN_HTTP_OK && blen == strlen(body) && blen > 0,
          "resume: body built");
    {
        const char *pfx =
            "{\"action\":2,\"data\":\"RESUME\",\"sessionRequestData\":{"
            "\"appId\":42,\"audioMode\":2,\"remoteControllersBitmap\":0,"
            "\"sdrHdrMode\":0,\"networkTestSessionId\":null,"
            "\"availableSupportedControllers\":[2],"
            "\"preferredController\":2,\"clientVersion\":\"30.0\","
            "\"deviceHashId\":\"dev-1\",";
        CHECK(!strncmp(body, pfx, strlen(pfx)),
              "resume: exact subset prefix in the reference's key order");
    }
    CHECK(strstr(body, "\"internalTitle\":\"T\"") != NULL &&
          strstr(body, "\"clientPlatformName\":\"Windows\"") != NULL &&
          strstr(body, "\"clientTimezoneOffset\":3600000") != NULL &&
          strstr(body, "\"streamerVersion\":\"14\"") != NULL &&
          strstr(body, "\"secureRTSPSupported\":true") != NULL,
          "resume: subset values match the create body's sources");
    CHECK(strstr(body, "{\"key\":\"SubSessionId\",\"value\":\"SUB-9\"}") !=
          NULL, "resume: metaData carries the flight's SubSessionId");
    CHECK(strstr(body, "\"appLaunchMode\":1") != NULL &&
          strstr(body, "\"enablePersistingInGameSettings\":false") != NULL,
          "resume: the echoed keys are re-sent (:1589-1597)");
    CHECK(strstr(body, "\"metaData\":null,\"adUpdates\":null}") != NULL &&
          body[blen - 1] == '}',
          "resume: the wrapper closes with null metaData/adUpdates");
    CHECK(strstr(body, "clientRequestMonitorSettings") == NULL &&
          strstr(body, "requestedStreamingFeatures") == NULL &&
          strstr(body, "sdkVersion") == NULL,
          "resume: NO codec/geometry/fps/bitrate renegotiation (:1547)");
    {   /* cf NULL: the echoed trio is omitted, platform defaults */
        size_t l2 = 0;
        char b2[6144];
        CHECK(kn_cm_build_resume_body(&st, NULL, b2, sizeof b2, &l2) ==
                  KN_HTTP_OK &&
              strstr(b2, "appLaunchMode") == NULL &&
              strstr(b2, "enablePersistingInGameSettings") == NULL &&
              strstr(b2, "\"clientPlatformName\":\"Windows\"") != NULL,
              "resume: NULL conflict omits the echoed keys");
    }
    {   /* missing title refused (same gate as the create body) */
        kn_cm_settings bad = st;
        bad.title = NULL;
        CHECK(kn_cm_build_resume_body(&bad, &cf, body, sizeof body,
                                      &blen) == KN_HTTP_ERR_ARGS,
              "resume: missing title refused");
    }

    /* the PUT round-trip: URL carries the session preferences
     * (language.rs :75-83), the body rides it, and the response
     * re-fills the session like every other CM round. */
    memset(&st, 0, sizeof st);
    st.token = "T0KEN"; st.device_id = "dev-1"; st.app_id = 42;
    st.title = "T"; st.width = 1280; st.height = 720; st.fps = 30;
    hio_reset(&h);
    {   char framed[HIO_CAP / 2];
        snprintf(framed, sizeof framed,
                 "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n%s",
                 (unsigned long)strlen(k_session_ready), k_session_ready);
        hio_queue(&h, framed);
    }
    memset(&cf, 0, sizeof cf);
    snprintf(cf.session_id, sizeof cf.session_id, "SESS-1");
    g_now = 0;
    rc = kn_cm_resume(hio_io(&h), "np-ams-05.example", "/", &st, &cf,
                      1000, fake_now, NULL, g_rxb, sizeof g_rxb, g_body,
                      sizeof g_body, &status, &s);
    CHECK(rc == KN_HTTP_OK && status == 200, "resume: PUT 200 accepted");
    h.from[h.flen] = '\0';
    CHECK(strstr((char *)h.from,
          "PUT /v2/session/SESS-1?keyboardLayout=en-US&languageCode="
          "en_US HTTP/1.1\r\n") == (char *)h.from,
          "resume: PUT + the session-preference query (language.rs)");
    CHECK(strstr((char *)h.from, "Authorization: GFNJWT T0KEN\r\n") != NULL,
          "resume: GFNJWT authorization rides the PUT");
    CHECK(strstr((char *)h.from,
                 "{\"action\":2,\"data\":\"RESUME\"") != NULL,
          "resume: the RESUME body rides the PUT");
    CHECK(!strcmp(s.session_id, "SESS-1") && s.ready,
          "resume: the response re-fills the session (poll continues)");
    CHECK(kn_cm_resume(hio_io(&h), "h", "/", &st, NULL, 1000, fake_now,
                       NULL, g_rxb, sizeof g_rxb, g_body, sizeof g_body,
                       &status, NULL) == KN_HTTP_ERR_ARGS,
          "resume: missing conflict refused before any I/O");
}

int main(void)
{
    test_json();
    test_http();
    test_cm_extract();
    test_cm_control();
    test_cm_body();
    test_cm_browser_profile();
    test_cm_network();
    test_cm_cancel();
    test_cm_ads();
    test_cm_resume();
    return kn_test_summary("test_kncm");
}
