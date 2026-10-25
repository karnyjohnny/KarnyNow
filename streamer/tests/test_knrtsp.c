/* test_knrtsp.c — the RTSP-over-WS client battery (round XX).
 *
 * Every fixture below is TRANSCRIBED from nvst_rtsp.rs @ a49b124 (its own
 * #[test] bodies where they exist — the parser cases :3443-3470, the
 * official-control case :3470+, the announce params :2923-2960 — and the
 * production code paths elsewhere; docs/RTSP.md carries the line table).
 * The scripted message-io shim plays the server side deterministically;
 * the injected clock/sleep make the SETUP retry rounds instant and
 * countable. Section-isolated, never aborts (round-XVI suite rule).
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_rtsp.h"
#include "test_port.h"

#include <string.h>

/* -------------------------------------------------- scripted transport */

#define RIO_MSGS   24
#define RIO_SENT   24
#define RIO_MSGCAP 8192

static uint64_t g_now;
static unsigned g_sleeps;

typedef struct {
    const char *msgs[RIO_MSGS];
    size_t lens[RIO_MSGS];
    int nmsgs, pos;
    char sent[RIO_SENT][RIO_MSGCAP];
    size_t sent_lens[RIO_SENT];
    int nsent;
    int send_fails_from;      /* -1 = never; else fail at that index */
} rio;

static int rio_send(void *ctx, const char *text, size_t len)
{
    rio *r = (rio *)ctx;
    if (r->send_fails_from >= 0 && r->nsent >= r->send_fails_from) return -1;
    if (r->nsent >= RIO_SENT || len + 1 > RIO_MSGCAP) return -1;
    memcpy(r->sent[r->nsent], text, len);
    r->sent[r->nsent][len] = '\0';
    r->sent_lens[r->nsent] = len;
    r->nsent++;
    return 0;
}

static int rio_recv(void *ctx, char *buf, size_t cap, uint32_t timeout_ms,
                    size_t *out_len)
{
    rio *r = (rio *)ctx;
    *out_len = 0;
    if (r->pos >= r->nmsgs) {
        /* A real recv_text WAITS the timeout — the fake must advance the
         * injected clock the same way or deadline loops never exit. */
        g_now += timeout_ms;
        return KN_RTSP_ERR_TIMEOUT;
    }
    if (r->lens[r->pos] + 1 > cap) return -1;
    memcpy(buf, r->msgs[r->pos], r->lens[r->pos]);
    buf[r->lens[r->pos]] = '\0';
    *out_len = r->lens[r->pos];
    r->pos++;
    g_now += 1;                       /* a served message still took time */
    return KN_RTSP_OK;
}

/* injected clock + sleep (the sleep ADVANCES the clock — retry rounds are
 * instant but the deadline math sees real progress) */
static uint64_t fake_now(void *ctx) { (void)ctx; return g_now; }
static void fake_sleep(uint32_t ms, void *ctx)
{
    (void)ctx;
    g_sleeps++;
    g_now += ms;
}

static char g_rtsp_buf[KN_RTSP_MAX_RESPONSE + 4096];

static void client_reset(rio *r, kn_rtsp_client *c)
{
    memset(r, 0, sizeof *r);
    r->send_fails_from = -1;
    g_now = 1000000;
    g_sleeps = 0;
    {
        kn_rtsp_msg_io io;
        io.send_text = rio_send;
        io.recv_text = rio_recv;
        io.ctx = r;
        CHECK(kn_rtsp_client_init(c, io, g_rtsp_buf, sizeof g_rtsp_buf,
                                  KN_RTSP_TIMEOUT_MS, fake_now, NULL,
                                  fake_sleep, NULL) == KN_RTSP_OK,
              "client init");
    }
}

static void rio_add(rio *r, const char *msg)
{
    if (r->nmsgs < RIO_MSGS) {
        r->msgs[r->nmsgs] = msg;
        r->lens[r->nmsgs] = strlen(msg);
        r->nmsgs++;
    }
}

/* --------------------------------------------------------- serialize */

static void test_serialize(void)
{
    char out[2048];
    size_t n = 0;
    kn_rtsp_header h[2];
    int rc;

    printf("-- serialize --\n");
    h[0].name = "X-GS-Version"; h[0].value = "14.2";
    h[1].name = "Host"; h[1].value = "rig.example:322";
    rc = kn_rtsp_serialize(out, sizeof out, "OPTIONS", "rtsps://rig.example:322",
                           7, h, 2, NULL, 0, &n);
    CHECK(rc == KN_RTSP_OK, "serialize ok");
    CHECK(strcmp(out,
                 "OPTIONS rtsps://rig.example:322 RTSP/1.0\r\n"
                 "CSeq: 7\r\nRequest-Id: 7\r\n"
                 "X-GS-Version: 14.2\r\nHost: rig.example:322\r\n"
                 "\r\n") == 0,
          "request bytes EXACT (CSeq == Request-Id, no Content-Length)");
    rc = kn_rtsp_serialize(out, sizeof out, "ANNOUNCE", "rtsps://x:322", 9,
                           h, 1, "SDP", 3, &n);
    CHECK(rc == KN_RTSP_OK &&
          strstr(out, "Content-Length: 3\r\n\r\nSDP") != NULL,
          "body adds Content-Length then CRLF then body");
    CHECK(kn_rtsp_serialize(out, 20, "OPTIONS", "x", 1, NULL, 0, NULL, 0, &n)
          == KN_RTSP_ERR_BIG, "undersized out -> ERR_BIG");
    CHECK(kn_rtsp_serialize(NULL, 10, "OPTIONS", "x", 1, NULL, 0, NULL, 0, &n)
          == KN_RTSP_ERR_ARGS, "NULL out -> ERR_ARGS");
}

/* ------------------------------------------------------------ parse */

static void test_parse(void)
{
    kn_rtsp_response r;
    size_t consumed = 0;
    int rc;

    printf("-- parse (reference fixtures :3443-3470) --\n");
    /* fixture 1: waits for body, checks cseq */
    {
        const char *b = "RTSP/1.0 200 OK\r\nCSeq: 3\r\nContent-Length: 4\r\n"
                        "\r\ntest";
        rc = kn_rtsp_parse(b, strlen(b), 3, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_OK && r.status == 200 && r.body_len == 4 &&
              memcmp(r.body, "test", 4) == 0 && consumed == strlen(b),
              "fixture 1: body complete + cseq match consumes everything");
        /* same buffer, one byte short -> MORE */
        rc = kn_rtsp_parse(b, strlen(b) - 1, 3, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_MORE, "fixture 1 truncated -> MORE");
    }
    /* fixture 2: Request-Id fallback when CSeq is absent */
    {
        const char *b = "RTSP/1.0 200 OK\r\nRequest-Id: 3\r\n"
                        "Content-Length: 0\r\n\r\n";
        rc = kn_rtsp_parse(b, strlen(b), 3, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_OK && r.status == 200,
              "fixture 2: Request-Id fallback accepted");
        /* fixture 3: uncorrelated -> SEQ error */
        b = "RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
        rc = kn_rtsp_parse(b, strlen(b), 3, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_ERR_SEQ, "fixture 3: uncorrelated -> ERR_SEQ");
        /* wrong CSeq value -> SEQ error, consumed = total (drain-then-err) */
        b = "RTSP/1.0 200 OK\r\nCSeq: 9\r\nContent-Length: 0\r\n\r\n";
        rc = kn_rtsp_parse(b, strlen(b), 3, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_ERR_SEQ && consumed == strlen(b),
              "mismatch reports consumed (the reference drains first)");
    }
    /* fixture 4: Bifrost leading-CRLF padding */
    {
        const char *b = "\r\n\r\nRTSP/1.0 200 OK\r\nCSeq: 4\r\nRequest-Id: 4"
                        "\r\nContent-Length: 0\r\n\r\n";
        rc = kn_rtsp_parse(b, strlen(b), 4, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_OK && r.status == 200 && consumed == strlen(b),
              "fixture 4: leading transport padding dropped");
        /* padding-only prefix with an incomplete tail: padding IS consumed
         * on MORE (the reference drains it eagerly) */
        rc = kn_rtsp_parse(b, 6, 4, KN_RTSP_MAX_RESPONSE, &r, &consumed);
        CHECK(rc == KN_RTSP_MORE && consumed == 4,
              "padding consumed even while incomplete");
    }
    /* LF-only header end */
    {
        const char *b = "RTSP/1.0 404 Not Found\nCSeq: 5\nContent-Length: 0\n\n";
        rc = kn_rtsp_parse(b, strlen(b), 5, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_OK && r.status == 404 &&
              strcmp(r.status_text, "Not Found") == 0,
              "LF-only header end + status text");
    }
    /* header semantics: lower-cased names, trimmed values, last wins */
    {
        const char *b = "RTSP/1.0 200 OK\r\nCSeq: 6\r\nTransport: a=1\r\n"
                        "TRANSPORT: b=2\r\nX-Nv-Ping: 6\r\n"
                        "Content-Length: 0\r\n\r\n";
        rc = kn_rtsp_parse(b, strlen(b), 6, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_OK, "duplicate-header response parses");
        {
            const char *t = kn_rtsp_header_get(&r, "transport");
            const char *p = kn_rtsp_header_get(&r, "X-NV-PING");
            CHECK(t && strcmp(t, "b=2") == 0, "last duplicate wins (HashMap)");
            CHECK(p && strcmp(p, "6") == 0, "header_get is case-insensitive");
            CHECK(kn_rtsp_header_get(&r, "missing") == NULL,
                  "absent header -> NULL");
        }
    }
    /* bad status line / cap */
    {
        const char *b = "GARBAGE\r\nCSeq: 7\r\nContent-Length: 0\r\n\r\n";
        rc = kn_rtsp_parse(b, strlen(b), 7, KN_RTSP_MAX_RESPONSE, &r,
                           &consumed);
        CHECK(rc == KN_RTSP_ERR_PROTO, "non-numeric status -> ERR_PROTO");
        b = "RTSP/1.0 200 OK\r\nCSeq: 8\r\nContent-Length: 100\r\n\r\nx";
        rc = kn_rtsp_parse(b, strlen(b), 8, 50, &r, &consumed);
        CHECK(rc == KN_RTSP_ERR_BIG, "total over cap -> ERR_BIG");
    }
}

/* -------------------------------------------------------- SDP side */

static const char k_describe_sdp[] =
    "v=0\r\n"
    "o=- 0 0 IN IP4 0.0.0.0\r\n"
    "s=NVIDIA Streaming Server\r\n"
    "t=0 0\r\n"
    "a=x-nv-general.pingVersion:5\r\n"
    "a=x-nv-general.iceUsernameFragment:OLDUFRAG\r\n"
    "a=x-nv-general.iceUserNameFragmentV2:RUFRAG\r\n"
    "a=x-nv-general.icePasswordV2:RPWD\r\n"
    "a=x-nv-general.dtlsFingerprintV2:AA:BB:CC\r\n"
    "a=x-nv-general.nativeRtcOnBundlePort:1\r\n"
    "a=x-nv-general.rtcpOnSctp:1\r\n"
    "a=x-nv-general.enableUnifiedSocket:1\r\n"
    "a=x-nv-runtime.encryptionKey:"
    "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef\r\n"
    "a=x-nv-runtime.encryptionKeyId:7\r\n"
    "m=video 5004 RTP/AVP 96\r\n"
    "a=control:*\r\n"
    "a=control:streamid=video/0\r\n"
    "m=audio 0 RTP/AVP 111\r\n"
    "a=control:streamid=audio/0\r\n";

static void test_sdp(void)
{
    char v[256], key[65];
    uint32_t id = 0;
    char salt[25];

    printf("-- SDP extraction --\n");
    CHECK(kn_rtsp_sdp_attribute(k_describe_sdp, "general.pingVersion",
                                v, sizeof v) == KN_RTSP_OK &&
          strcmp(v, "5") == 0, "x-nv attribute found");
    CHECK(kn_rtsp_sdp_attribute(k_describe_sdp, "general.missing",
                                v, sizeof v) == -1, "absent attribute -> -1");
    CHECK(kn_rtsp_media_control(k_describe_sdp, "video", v, sizeof v)
              == KN_RTSP_OK && strcmp(v, "streamid=video/0") == 0,
          "media_control skips a=control:* and finds the video control");
    CHECK(kn_rtsp_media_control(k_describe_sdp, "audio", v, sizeof v)
              == KN_RTSP_OK && strcmp(v, "streamid=audio/0") == 0,
          "media_control audio section");
    CHECK(kn_rtsp_media_control(k_describe_sdp, "mic", v, sizeof v) == -1,
          "absent media section -> -1");
    /* the || and ;; quirks */
    {
        const char *quirk =
            "a=x-nv-general.pingVersion:6\r\n;;\r\na=x-nv-other.flag:1\r\n"
            "||garbage after bars";
        CHECK(kn_rtsp_sdp_attribute(quirk, "other.flag", v, sizeof v)
                  == KN_RTSP_OK && strcmp(v, "1") == 0,
              "attribute lookup flattens ;; segments, stops at ||");
        CHECK(kn_rtsp_sdp_attribute(quirk, "general.pingVersion", v,
                                    sizeof v) == KN_RTSP_OK,
              "attribute before ;; still found");
    }
    /* runtime key + salt */
    CHECK(kn_rtsp_runtime_key(k_describe_sdp, key, &id) == KN_RTSP_OK &&
          id == 7, "runtime key + id extracted");
    CHECK(strcmp(key,
                 "0123456789ABCDEF0123456789ABCDEF"
                 "0123456789ABCDEF0123456789ABCDEF") == 0,
          "key uppercased (to_ascii_upper)");
    kn_rtsp_salt_from_key_id(7, salt);
    CHECK(strcmp(salt, "000000000000000000000007") == 0,
          "salt = key_id as 24-digit uppercase hex (:1280)");
    kn_rtsp_salt_from_key_id(0xDEADBEEFu, salt);
    CHECK(strcmp(salt, "0000000000000000DEADBEEF") == 0,
          "salt of a large key id");
    {   /* invalid key material */
        const char *bad = "a=x-nv-runtime.encryptionKey:tooshort\r\n"
                          "a=x-nv-runtime.encryptionKeyId:7\r\n";
        CHECK(kn_rtsp_runtime_key(bad, key, &id) == -1,
              "short key rejected");
    }
}

static void test_peer_and_profile(void)
{
    char ip[64];
    uint16_t p1 = 0, p2 = 0;
    kn_rtsp_response setup;
    const char *prof;

    printf("-- video peer + SRTP profile --\n");
    CHECK(kn_rtsp_parse_video_peer(
              "RTP/AVP/UDP;unicast;source=192.0.2.5;"
              "X-GS-ServerPort=5000-5005", ip, &p1, &p2) == KN_RTSP_OK &&
          strcmp(ip, "192.0.2.5") == 0 && p1 == 5000 && p2 == 5005,
          "source + port range parsed");
    CHECK(kn_rtsp_parse_video_peer("source=192.0.2.5;X-GS-ServerPort=5000",
                                   ip, &p1, &p2) == KN_RTSP_OK &&
          p1 == 5000 && p2 == 5000, "single port -> end = first");
    CHECK(kn_rtsp_parse_video_peer("source=192.0.2.5;X-GS-ServerPort=5000-5020",
                                   ip, &p1, &p2) == KN_RTSP_OK && p2 == 5000,
          "span >= 16 -> port_end collapses to first (:2273, nvst.rs:80)");
    CHECK(kn_rtsp_parse_video_peer("source=192.0.2.5;X-GS-ServerPort=0",
                                   ip, &p1, &p2) == -1, "port 0 rejected");
    CHECK(kn_rtsp_parse_video_peer("X-GS-ServerPort=5000", ip, &p1, &p2) == -1,
          "missing source rejected");
    CHECK(kn_rtsp_parse_video_peer("source=192.0.2.5", ip, &p1, &p2) == -1,
          "missing server port rejected");
    CHECK(kn_rtsp_parse_video_peer("SOURCE=192.0.2.5,x-gs-serverport=5000",
                                   ip, &p1, &p2) == KN_RTSP_OK,
          "comma separator + case-insensitive names");

    memset(&setup, 0, sizeof setup);
    setup.hdr_count = 1;
    snprintf(setup.names[0], sizeof setup.names[0], "transport");
    snprintf(setup.values[0], sizeof setup.values[0],
             "x-srtp-crypto=AEAD_AES_128_GCM;source=1.2.3.4");
    prof = kn_rtsp_srtp_profile(&setup, k_describe_sdp);
    CHECK(prof && strcmp(prof, "AEAD_AES_128_GCM") == 0,
          "SETUP transport header wins (exact name, not _8)");
    snprintf(setup.values[0], sizeof setup.values[0],
             "crypto=SRTP_AEAD_AES_256_GCM_8");
    prof = kn_rtsp_srtp_profile(&setup, NULL);
    CHECK(prof && strcmp(prof, "AEAD_AES_256_GCM_8") == 0,
          "GCM_8 name matched inside a longer value");
    prof = kn_rtsp_srtp_profile(NULL, NULL);
    CHECK(prof && strcmp(prof, "AEAD_AES_256_GCM_8") == 0,
          "default is AEAD_AES_256_GCM_8 (:1283)");
    {   /* SDP-line fallback */
        const char *sdp = "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:xx\r\n";
        prof = kn_rtsp_srtp_profile(NULL, sdp);
        CHECK(prof && strcmp(prof, "AES_CM_128_HMAC_SHA1_80") == 0,
              "SDP line fallback");
    }
    {   /* priority order inside ONE value: 128_GCM_8 before 128_GCM */
        const char *sdp = "a=x:AEAD_AES_128_GCM_8 and AEAD_AES_128_GCM\r\n";
        prof = kn_rtsp_srtp_profile(NULL, sdp);
        CHECK(prof && strcmp(prof, "AEAD_AES_128_GCM_8") == 0,
              "PROFILES order wins within a single value");
    }
}

static void test_candidates(void)
{
    static char cand[6][KN_RTSP_URI_MAX];
    char off[KN_RTSP_URI_MAX];
    int n;

    printf("-- SETUP URI ladder --\n");
    kn_rtsp_official_control("streamid=video/0", off, sizeof off);
    CHECK(strcmp(off, "streamid=video/0/0") == 0,
          "official form appends /0 (reference test :3470)");
    kn_rtsp_official_control("streamid=video/0/0", off, sizeof off);
    CHECK(strcmp(off, "streamid=video/0/0") == 0,
          "already-official control unchanged");
    n = kn_rtsp_setup_candidates("streamid=video/0", "rtsps://rig:322",
                                 cand, 6);
    CHECK(n == 4, "ladder has 4 forms");
    CHECK(strcmp(cand[0], "streamid=video/0/0") == 0, "cand0 = official");
    CHECK(strcmp(cand[1], "streamid=video/0") == 0, "cand1 = raw control");
    CHECK(strcmp(cand[2], "rtsps://rig:322/streamid=video/0/0") == 0,
          "cand2 = absolute official");
    CHECK(strcmp(cand[3], "rtsps://rig:322/streamid=video/0") == 0,
          "cand3 = absolute raw");
    n = kn_rtsp_setup_candidates("rtsps://rig:322/streamid=video/0",
                                 "rtsps://rig:322", cand, 6);
    CHECK(n == 1 && strcmp(cand[0], "rtsps://rig:322/streamid=video/0") == 0,
          "absolute control: no extra forms");
    n = kn_rtsp_setup_candidates("streamid=video/0", "rtsps://rig:322/",
                                 cand, 6);
    CHECK(n == 4 && strcmp(cand[2], "rtsps://rig:322/streamid=video/0/0") == 0,
          "target trailing slash trimmed");
}

static void test_describe(void)
{
    kn_rtsp_response resp;
    kn_rtsp_describe d;
    size_t consumed;
    static char raw[8192];
    int n;

    printf("-- DESCRIBE extraction --\n");
    n = snprintf(raw, sizeof raw,
                 "RTSP/1.0 200 OK\r\nCSeq: 2\r\nSession: SESS123;timeout=60"
                 "\r\nContent-Type: application/sdp\r\nContent-Length: %lu"
                 "\r\n\r\n%s", (unsigned long)strlen(k_describe_sdp),
                 k_describe_sdp);
    CHECK(n > 0 && (size_t)n < sizeof raw, "fixture built");
    CHECK(kn_rtsp_parse(raw, (size_t)n, 2, KN_RTSP_MAX_RESPONSE, &resp,
                        &consumed) == KN_RTSP_OK, "fixture parses");
    CHECK(kn_rtsp_extract_describe(&resp, &d) == KN_RTSP_OK,
          "extraction succeeds");
    CHECK(strcmp(d.session, "SESS123") == 0, "session taken before ';'");
    CHECK(strcmp(d.video_control, "streamid=video/0") == 0, "video control");
    CHECK(d.ping_version == 5, "pingVersion 5 read");
    CHECK(strcmp(d.remote_ufrag, "RUFRAG") == 0, "V2 ufrag preferred");
    CHECK(strcmp(d.remote_password, "RPWD") == 0, "V2 password");
    CHECK(strcmp(d.remote_fp, "AA:BB:CC") == 0, "V2 fingerprint");
    CHECK(d.native_bundle == 1 && d.rtcp_on_sctp == 1 &&
          d.unified_socket == 1 && d.disable_play == 0,
          "boolean attributes");
    CHECK(d.have_runtime_key && d.key_id == 7, "runtime key carried");
    {   /* missing Session header -> -1 */
        kn_rtsp_response bad;
        kn_rtsp_describe d2;
        static char raw2[8192];
        int n2 = snprintf(raw2, sizeof raw2,
                          "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: %lu"
                          "\r\n\r\n%s",
                          (unsigned long)strlen(k_describe_sdp),
                          k_describe_sdp);
        CHECK(kn_rtsp_parse(raw2, (size_t)n2, 2, KN_RTSP_MAX_RESPONSE, &bad,
                            &consumed) == KN_RTSP_OK &&
              kn_rtsp_extract_describe(&bad, &d2) == -1,
              "missing Session -> -1 (fields still filled)");
        CHECK(strcmp(d2.video_control, "streamid=video/0") == 0,
              "best-effort fields survive the failure");
    }
    {   /* pingVersion absent -> 6 */
        kn_rtsp_response r3;
        kn_rtsp_describe d3;
        static char raw3[4096];
        const char *sdp3 = "m=video 1 RTP/AVP 96\r\na=control:streamid=video/0\r\n";
        int n3 = snprintf(raw3, sizeof raw3,
                          "RTSP/1.0 200 OK\r\nCSeq: 3\r\nSession: S\r\n"
                          "Content-Length: %lu\r\n\r\n%s",
                          (unsigned long)strlen(sdp3), sdp3);
        CHECK(kn_rtsp_parse(raw3, (size_t)n3, 3, KN_RTSP_MAX_RESPONSE, &r3,
                            &consumed) == KN_RTSP_OK &&
              kn_rtsp_extract_describe(&r3, &d3) == KN_RTSP_OK &&
              d3.ping_version == 6, "pingVersion defaults to 6 (:1129)");
    }
}

/* ------------------------------------------------------ SETUP sweep */

static const char *const k_common_names[3] = {
    "X-GS-Version", "Host", "x-nv-sessionid"
};
static const char *const k_common_values[3] = {
    "14.2", "rig.example:322", "SESS123"
};

static void fill_common(kn_rtsp_header *h)
{
    int i;
    for (i = 0; i < 3; ++i) { h[i].name = k_common_names[i];
                              h[i].value = k_common_values[i]; }
}

static void test_setup(void)
{
    rio r;
    kn_rtsp_client c;
    kn_rtsp_header common[3];
    kn_rtsp_setup_result res;
    int rc;

    printf("-- SETUP sweep --\n");
    fill_common(common);

    /* happy path: first candidate, empty Transport, 200 + peer */
    client_reset(&r, &c);
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 1\r\nTransport: unicast;source="
                "192.0.2.9;X-GS-ServerPort=5000-5004\r\nx-nv-ping: 6\r\n"
                "x-nv-ping-payload: PLD\r\nContent-Length: 0\r\n\r\n");
    rc = kn_rtsp_setup_video(&c, "streamid=video/0", "rtsps://rig:322",
                             "SESS123", 5, 49006, common, 3, NULL, 0, &res);
    CHECK(rc == KN_RTSP_OK, "happy SETUP ok");
    CHECK(res.status == 200 && strcmp(res.peer_ip, "192.0.2.9") == 0 &&
          res.peer_port == 5000 && res.peer_port_end == 5004,
          "peer extracted from Transport");
    CHECK(res.bundle_fallback == 0, "not a bundle fallback");
    CHECK(res.ping_version == 6 && res.have_ping_payload &&
          strcmp(res.ping_payload, "PLD") == 0,
          "SETUP x-nv-ping override + payload");
    CHECK(r.nsent == 1, "exactly one SETUP sent");
    CHECK(strstr(r.sent[0], "SETUP streamid=video/0/0 RTSP/1.0\r\n") ==
          r.sent[0], "first candidate is the official form");
    CHECK(strstr(r.sent[0], "CSeq: 1\r\nRequest-Id: 1\r\n") != NULL,
          "CSeq==Request-Id==1");
    CHECK(strstr(r.sent[0], "Session: SESS123\r\n") != NULL &&
          strstr(r.sent[0], "x-nv-ping: 5\r\n") != NULL &&
          strstr(r.sent[0], "Transport: \r\n") != NULL,
          "Session + described ping + EMPTY first Transport form");

    /* 400 on the official form, 200 on the raw control form */
    client_reset(&r, &c);
    rio_add(&r, "RTSP/1.0 400 Bad Request\r\nCSeq: 1\r\nContent-Length: 0"
                "\r\n\r\n");
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 2\r\nTransport: source=192.0.2.9;"
                "X-GS-ServerPort=5000\r\nContent-Length: 0\r\n\r\n");
    rc = kn_rtsp_setup_video(&c, "streamid=video/0", "rtsps://rig:322",
                             "SESS123", 6, 49006, common, 3, NULL, 0, &res);
    CHECK(rc == KN_RTSP_OK && res.peer_port == 5000 &&
          res.peer_port_end == 5000, "400 -> next candidate succeeds");
    CHECK(r.nsent == 2 &&
          strstr(r.sent[1], "SETUP streamid=video/0 RTSP") != NULL,
          "second request uses the raw control form");

    /* peerless 200 WITH a bundle fallback peer */
    client_reset(&r, &c);
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 1\r\nTransport: unicast\r\n"
                "Content-Length: 0\r\n\r\n");
    rc = kn_rtsp_setup_video(&c, "streamid=video/0", "rtsps://rig:322",
                             "SESS123", 6, 49006, common, 3,
                             "198.51.100.7", 47998, &res);
    CHECK(rc == KN_RTSP_OK && res.bundle_fallback == 1 &&
          strcmp(res.peer_ip, "198.51.100.7") == 0 &&
          res.peer_port == 47998,
          "peerless 200 -> CloudMatch bundle fallback (:345-357)");
    CHECK(r.nsent == 1, "sweep STOPPED at the peerless 200 (poison rule)");

    /* peerless 200 WITHOUT fallback -> 3 retry rounds then ERR_STATE */
    client_reset(&r, &c);
    {
        static char m0[128], m1[128], m2[128], m3[128];
        int i;
        char *mm[4];
        mm[0] = m0; mm[1] = m1; mm[2] = m2; mm[3] = m3;
        for (i = 0; i < 4; ++i) {
            snprintf(mm[i], 128,
                     "RTSP/1.0 200 OK\r\nCSeq: %d\r\nTransport: unicast"
                     "\r\nContent-Length: 0\r\n\r\n", i + 1);
            rio_add(&r, mm[i]);
        }
    }
    rc = kn_rtsp_setup_video(&c, "streamid=video/0", "rtsps://rig:322",
                             "SESS123", 6, 49006, common, 3, NULL, 0, &res);
    CHECK(rc == KN_RTSP_ERR_STATE, "peerless without fallback exhausts");
    CHECK(g_sleeps == 3 && res.rounds_slept == 3,
          "exactly SETUP_PEER_RETRY_ROUNDS=3 sleeps (:35)");

    /* a fatal status (500) fails immediately, no retry rounds */
    client_reset(&r, &c);
    rio_add(&r, "RTSP/1.0 500 Server Error\r\nCSeq: 1\r\nContent-Length: 0"
                "\r\n\r\n");
    rc = kn_rtsp_setup_video(&c, "streamid=video/0", "rtsps://rig:322",
                             "SESS123", 6, 49006, common, 3, NULL, 0, &res);
    CHECK(rc == KN_RTSP_ERR_PROTO && g_sleeps == 0,
          "status 500 is fatal without retries (:488-497)");

    /* second Transport form is used when the empty form 400s everything */
    client_reset(&r, &c);
    {
        static char q[4][96];   /* worst case 67 B (11-digit %d) — bounded (R28) */
        int i;
        for (i = 0; i < 4; ++i) {
            snprintf(q[i], sizeof q[i],
                     "RTSP/1.0 400 Bad Request\r\nCSeq: %d\r\n"
                     "Content-Length: 0\r\n\r\n", i + 1);
            rio_add(&r, q[i]);
        }
        rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 5\r\nTransport: source="
                    "192.0.2.9;X-GS-ServerPort=5000\r\nContent-Length: 0"
                    "\r\n\r\n");
    }
    rc = kn_rtsp_setup_video(&c, "streamid=video/0", "rtsps://rig:322",
                             "SESS123", 6, 49006, common, 3, NULL, 0, &res);
    CHECK(rc == KN_RTSP_OK && r.nsent == 5,
          "second pass uses the client-udp Transport form");
    CHECK(strstr(r.sent[4], "Transport: unicast;X-GS-ClientPort=49006-49007"
                            "\r\n") != NULL,
          "X-GS-ClientPort={p}-{p+1} form exact (:398-401)");
}

/* ------------------------------------------------------- full sequence */

static const char k_opts_resp[] =
    "RTSP/1.0 200 OK\r\nCSeq: 1\r\nPublic: OPTIONS, DESCRIBE, SETUP, "
    "ANNOUNCE, PLAY, TEARDOWN\r\nContent-Length: 0\r\n\r\n";

static void test_full_sequence(void)
{
    rio r;
    kn_rtsp_client c;
    kn_rtsp_header h[6];
    kn_rtsp_response resp;
    kn_rtsp_describe d;
    int rc;
    static char desc_raw[8192], announce[8192], negotiated[8192];
    size_t alen = 0, nlen = 0;

    printf("-- full OPTIONS->DESCRIBE->SETUP->ANNOUNCE->PLAY --\n");
    client_reset(&r, &c);
    snprintf(desc_raw, sizeof desc_raw,
             "RTSP/1.0 200 OK\r\nCSeq: 2\r\nSession: SESS123\r\n"
             "Content-Type: application/sdp\r\nContent-Length: %lu\r\n\r\n%s",
             (unsigned long)strlen(k_describe_sdp), k_describe_sdp);
    rio_add(&r, k_opts_resp);
    rio_add(&r, desc_raw);
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 3\r\nTransport: source=192.0.2.9;"
                "X-GS-ServerPort=5000\r\nx-nv-ping: 5\r\nContent-Length: 0"
                "\r\n\r\n");
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 4\r\nContent-Length: 0\r\n\r\n");
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 5\r\nContent-Length: 0\r\n\r\n");

    h[0].name = "X-GS-Version"; h[0].value = "14.2";
    h[1].name = "Host"; h[1].value = "rig.example:322";
    h[2].name = "x-nv-sessionid"; h[2].value = "SESS123";
    rc = kn_rtsp_request(&c, "OPTIONS", "rtsps://rig.example:322", h, 3,
                         NULL, 0, 0, &resp);
    CHECK(rc == KN_RTSP_OK && resp.status == 200, "OPTIONS 200");

    h[3].name = "Accept"; h[3].value = "application/sdp";
    h[4].name = "x-nv-abtesting"; h[4].value = "2";
    rc = kn_rtsp_request(&c, "DESCRIBE", "rtsps://rig.example:322", h, 5,
                         NULL, 0, 0, &resp);
    CHECK(rc == KN_RTSP_OK && resp.status == 200, "DESCRIBE 200");
    CHECK(kn_rtsp_extract_describe(&resp, &d) == KN_RTSP_OK,
          "DESCRIBE extracts");
    CHECK(resp.body_len == strlen(k_describe_sdp) &&
          memcmp(resp.body, k_describe_sdp, resp.body_len) == 0,
          "SDP body byte-exact (aliases the accumulation buffer)");
    CHECK(strstr(r.sent[1], "Accept: application/sdp\r\n") != NULL &&
          strstr(r.sent[1], "x-nv-abtesting: 2\r\n") != NULL &&
          strstr(r.sent[1], "CSeq: 2\r\n") != NULL,
          "DESCRIBE headers + counter increment");

    {   /* SETUP through the sweep */
        kn_rtsp_setup_result sr;
        kn_rtsp_header common[3];
        fill_common(common);
        rc = kn_rtsp_setup_video(&c, d.video_control,
                                 "rtsps://rig.example:322", d.session,
                                 d.ping_version, 49006, common, 3,
                                 NULL, 0, &sr);
        CHECK(rc == KN_RTSP_OK && sr.peer_port == 5000, "SETUP peer 5000");
        CHECK(strstr(r.sent[2], "CSeq: 3\r\n") != NULL,
              "SETUP continues the counter");
    }

    /* ANNOUNCE: build -> negotiate against the DESCRIBE SDP -> send */
    {
        kn_rtsp_announce_params p;
        uint32_t negv[19];
        memset(&p, 0, sizeof p);
        p.width = 1280; p.height = 720; p.fps = 30;      /* D14 */
        p.bitrate_kbps = 5000;
        p.bitstream_format = 0;                           /* H264 */
        p.bit_depth = 0; p.chroma_format = 1;             /* 8-bit 4:2:0 */
        p.hdr = 0;
        snprintf(p.key_hex, sizeof p.key_hex, "%s", d.runtime_key);
        p.key_id = d.key_id;
        p.bundle_port = 49006;
        snprintf(p.local_address, sizeof p.local_address, "192.0.2.10");
        snprintf(p.ufrag, sizeof p.ufrag, "LUFRAG");
        snprintf(p.password, sizeof p.password, "LPASSWORD");
        snprintf(p.fingerprint, sizeof p.fingerprint, "DD:EE");
        p.video_port = 5000;                              /* SERVER peer port */
        p.video_packet_size = 1280;
        p.rtcp_on_sctp = d.rtcp_on_sctp;
        p.unified_socket = d.unified_socket;
        p.qos_timings_v5 = 0;
        p.dynamic_streaming_mode = 0;
        p.adjust_res_and_fps = 0;
        rc = kn_rtsp_build_announce(&p, announce, sizeof announce, &alen);
        CHECK(rc == KN_RTSP_OK && alen > 2000, "announce built");
        CHECK(strstr(announce, "a=x-nv-video[0].clientViewportWd:1280\r\n")
                  != NULL &&
              strstr(announce, "a=x-nv-video[0].clientViewportHt:720\r\n")
                  != NULL &&
              strstr(announce, "a=x-nv-video[0].maxFPS:30\r\n") != NULL,
              "D14 720p30 viewport/fps");
        CHECK(strstr(announce, "a=x-nv-video[0].packetSize:1280\r\n") != NULL,
              "packetSize = kn_rx video_packet_size bridge");
        CHECK(strstr(announce, "a=x-nv-runtime.encryptionKey:0123456789ABCDEF"
                               "0123456789ABCDEF0123456789ABCDEF"
                               "0123456789ABCDEF\r\n") != NULL,
              "the DESCRIBE runtime key is echoed");
        CHECK(strstr(announce, "a=x-nv-runtime.encryptionKeyId:7\r\n") != NULL,
              "key id echoed");
        CHECK(strstr(announce, "a=x-nv-general.clientBundlePort:49006\r\n")
                  != NULL, "clientBundlePort");
        CHECK(strstr(announce, "m=video 5000\r\nc=IN IP4 0.0.0.0\r\n"
                               "i=DeviceString, DeviceName\r\n") != NULL,
              "media tail exact");
        CHECK(alen >= 2 && announce[alen - 2] == '\r' &&
              announce[alen - 1] == '\n', "trailing CRLF (empty last line)");
        CHECK(strstr(announce, "\n\n") == NULL, "CRLF-joined (no bare LF)");
        CHECK(strstr(announce, "a=x-nv-video[0].bitDepth:0\r\n") != NULL &&
              strstr(announce, "a=x-nv-video[0].chromaFormat:1\r\n") != NULL,
              "color lines (8bit 420)");
        CHECK(strstr(announce, "dynamicRangeMode") == NULL,
              "no HDR line when hdr=0");

        rc = kn_rtsp_negotiate_announce(announce, k_describe_sdp, negotiated,
                                        sizeof negotiated, &nlen, negv);
        CHECK(rc == KN_RTSP_OK && nlen > 2000, "negotiate ok");
        CHECK(strstr(negotiated, "a=x-nv-vqos[0].fec.enable:1\r\n") != NULL,
              "fec.enable survives (announce value, no offer)");
        CHECK(negv[18] == 5000 && negv[16] == 5000 && negv[17] == 5000,
              "bitrate ceiling/initial/peak without offers");

        h[3].name = "Session"; h[3].value = d.session;
        h[4].name = "Content-Type"; h[4].value = "application/sdp";
        rc = kn_rtsp_request(&c, "ANNOUNCE", "rtsps://rig.example:322",
                             h, 5, negotiated, nlen, 0, &resp);
        CHECK(rc == KN_RTSP_OK && resp.status == 200, "ANNOUNCE 200");
        CHECK(strstr(r.sent[3], "Content-Length: ") != NULL &&
              strstr(r.sent[3], "Content-Type: application/sdp\r\n") != NULL,
              "ANNOUNCE carries Content-Type + Length");
    }

    /* PLAY (disable_play was 0) */
    h[3].name = "Session"; h[3].value = d.session;
    rc = kn_rtsp_request(&c, "PLAY", "rtsps://rig.example:322", h, 4,
                         NULL, 0, 0, &resp);
    CHECK(rc == KN_RTSP_OK && resp.status == 200, "PLAY 200");
    CHECK(c.cseq == 5 && c.requests == 5 && c.responses == 5,
          "five rounds, one shared counter");
}

static void test_teardown_skip(void)
{
    rio r;
    kn_rtsp_client c;
    kn_rtsp_response resp;
    int rc;

    printf("-- TEARDOWN mismatch skip --\n");
    client_reset(&r, &c);
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 99\r\nContent-Length: 0\r\n\r\n");
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 98\r\nContent-Length: 0\r\n\r\n");
    rio_add(&r, "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n");
    rc = kn_rtsp_request(&c, "TEARDOWN", "rtsps://rig:322", NULL, 0,
                         NULL, 0, 0, &resp);   /* cseq 1 vs CSeq: 99 */
    CHECK(rc == KN_RTSP_ERR_SEQ && c.seq_mismatch_skips == 0,
          "default: mismatch is an error (:593 is TEARDOWN-only)");
    rc = kn_rtsp_request(&c, "TEARDOWN", "rtsps://rig:322", NULL, 0,
                         NULL, 0, 1, &resp);   /* cseq 2: skip 98, take 2 */
    CHECK(rc == KN_RTSP_OK && resp.status == 200 &&
          c.seq_mismatch_skips == 1,
          "skip flag: mismatched response dropped, next one accepted");
}

static void test_errors(void)
{
    rio r;
    kn_rtsp_client c;
    kn_rtsp_response resp;

    printf("-- error paths --\n");
    client_reset(&r, &c);
    /* transport timeout -> deadline exit */
    g_now = 0;
    CHECK(kn_rtsp_request(&c, "OPTIONS", "rtsps://x", NULL, 0, NULL, 0, 0,
                          &resp) == KN_RTSP_ERR_TIMEOUT && c.timeouts == 1,
          "silent server -> ERR_TIMEOUT after the deadline");
    /* send failure */
    client_reset(&r, &c);
    r.send_fails_from = 0;
    CHECK(kn_rtsp_request(&c, "OPTIONS", "rtsps://x", NULL, 0, NULL, 0, 0,
                          &resp) == KN_RTSP_ERR_IO, "send failure -> ERR_IO");
    /* init validation */
    CHECK(kn_rtsp_client_init(NULL, c.io, g_rtsp_buf, sizeof g_rtsp_buf, 0,
                              fake_now, NULL, NULL, NULL) == KN_RTSP_ERR_ARGS,
          "NULL client refused");
    CHECK(kn_rtsp_client_init(&c, c.io, g_rtsp_buf, 1024, 0, fake_now, NULL,
                              NULL, NULL) == KN_RTSP_ERR_ARGS,
          "buffer < 4 MiB refused (the response cap is non-negotiable)");
    /* timeout_ms 0 -> the 20 s default */
    client_reset(&r, &c);
    CHECK(c.timeout_ms == KN_RTSP_TIMEOUT_MS, "timeout defaults to 20 s");
    /* announce arg discipline */
    {
        kn_rtsp_announce_params p;
        char out[8192];
        size_t n;
        memset(&p, 0, sizeof p);
        CHECK(kn_rtsp_build_announce(&p, out, sizeof out, &n)
              == KN_RTSP_ERR_ARGS, "empty key/address refused");
    }
}

int main(void)
{
    test_serialize();
    test_parse();
    test_sdp();
    test_peer_and_profile();
    test_candidates();
    test_describe();
    test_setup();
    test_full_sequence();
    test_teardown_skip();
    test_errors();
    return kn_test_summary("test_knrtsp");
}
