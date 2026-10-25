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
    kn_http_header hh[10];
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
    }
    st.fps = 120;
    CHECK(kn_cm_build_create_body(&st, body, sizeof body, &n) == KN_HTTP_OK &&
          strstr(body, "\"reflex\":true") != NULL,
          "reflex true at fps >= 120 (:1674)");
    st.fps = 30;

    CHECK(kn_cm_auth_header(&st, auth, sizeof auth) == KN_HTTP_OK &&
          strcmp(auth, "GFNJWT TOKEN123") == 0, "auth header format (:2040)");
    {
        int i, nh = kn_cm_fixed_headers(&st, hh, 10);
        int have_ct = 0, have_id = 0;
        CHECK(nh == 10, "ten fixed headers");
        for (i = 0; i < nh; ++i) {
            if (strcmp(hh[i].name, "Content-Type") == 0 &&
                strcmp(hh[i].value, "text/plain") == 0) have_ct = 1;
            if (strcmp(hh[i].name, "nv-client-id") == 0 &&
                strcmp(hh[i].value, KN_CM_LCARS_CLIENT_ID) == 0) have_id = 1;
        }
        CHECK(have_ct, "Content-Type text/plain quirk (:2041)");
        CHECK(have_id, "nv-client-id = LCARS (:2042)");
        for (i = 0; i < nh; ++i)
            if (strstr(hh[i].name, "uthorization")) { CHECK(0, "no auth here"); }
        CHECK(1, "Authorization deliberately NOT in the fixed table");
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

int main(void)
{
    test_json();
    test_http();
    test_cm_extract();
    test_cm_body();
    test_cm_network();
    return kn_test_summary("test_kncm");
}
