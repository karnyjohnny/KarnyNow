/* test_knws.c — RFC 6455 client-framing battery for kn_ws.
 *
 * Strategy: an in-memory duplex transport (mio) drives the REAL client
 * code paths — handshake, masked sends, fragmented receives, control
 * frames, error classes — including 1-byte-per-recv chunking, which is
 * exactly what a kernel TLS socket does under load. Real-socket/TLS E2E
 * is the round-XXI fake-rig gate (docs/RTSP.md §6); the framing contract
 * itself is fully pinned here. Section-isolated, never aborts (round-XVI
 * suite rule).
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_ws.h"
#include "test_port.h"

#include <string.h>

/* ------------------------------------------------- in-memory transport */

#define MIO_CAP (512 * 1024)

typedef struct {
    uint8_t to_client[MIO_CAP];   size_t tc_len, tc_pos;  /* server -> client */
    uint8_t from_client[MIO_CAP]; size_t fc_len;          /* client -> server */
    size_t chunk;                 /* 0 = serve everything available */
    int recv_timeouts;            /* 1 = every recv returns 0 */
    int send_fails;               /* 1 = every send fails */
} mio;

static void mio_reset(mio *m) { memset(m, 0, sizeof *m); }

static int mio_send(void *ctx, const uint8_t *b, size_t n)
{
    mio *m = (mio *)ctx;
    if (m->send_fails) return -1;
    if (m->fc_len + n > MIO_CAP) return -1;
    memcpy(m->from_client + m->fc_len, b, n);
    m->fc_len += n;
    return 0;
}

static int mio_recv(void *ctx, uint8_t *b, size_t cap, uint32_t timeout_ms)
{
    mio *m = (mio *)ctx;
    size_t avail, take;
    (void)timeout_ms;
    if (m->recv_timeouts) return 0;
    avail = m->tc_len - m->tc_pos;
    if (avail == 0) return 0;
    take = avail < cap ? avail : cap;
    if (m->chunk && take > m->chunk) take = m->chunk;
    memcpy(b, m->to_client + m->tc_pos, take);
    m->tc_pos += take;
    return (int)take;
}

static void mio_queue(mio *m, const void *data, size_t n)
{
    if (m->tc_len + n <= MIO_CAP) {
        memcpy(m->to_client + m->tc_len, data, n);
        m->tc_len += n;
    }
}

static kn_ws_io mio_io(mio *m)
{
    kn_ws_io io;
    io.send = mio_send;
    io.recv = mio_recv;
    io.ctx = m;
    return io;
}

/* ------------------------------------------- server-side frame builder */

static size_t sframe(uint8_t *out, int fin, int op, const uint8_t *pl,
                     size_t n)
{
    size_t o = 0;
    out[o++] = (uint8_t)((fin ? 0x80 : 0) | (op & 0x0F));
    if (n < 126u) {
        out[o++] = (uint8_t)n;
    } else if (n <= 0xFFFFu) {
        out[o++] = 126u;
        out[o++] = (uint8_t)(n >> 8);
        out[o++] = (uint8_t)(n & 0xFFu);
    } else {
        int i;
        uint64_t l = (uint64_t)n;
        out[o++] = 127u;
        for (i = 56; i >= 0; i -= 8) out[o++] = (uint8_t)(l >> i);
    }
    if (pl && n) { memcpy(out + o, pl, n); o += n; }
    return o;
}

/* Queue a correct 101 handshake computed from the client's actual request
 * (parses Sec-WebSocket-Key out of the captured GET — the accept is then
 * verified by the client against its own key via the SHA-1 path). */
static int mio_queue_handshake(mio *m, int status, const char *accept_override)
{
    const char *k = NULL;
    char key[32], accept[29], resp[256];
    int n;
    {   /* memmem is a GNU extension — hand-rolled scan keeps the suite
           strict-POSIX/Win32 portable */
        size_t i;
        for (i = 0; i + 19 <= m->fc_len; ++i)
            if (memcmp(m->from_client + i, "Sec-WebSocket-Key: ", 19) == 0) {
                k = (const char *)(m->from_client + i);
                break;
            }
    }
    if (!k) return -1;
    k += 19;
    {
        size_t i = 0;
        while (i < 24 && k[i] && k[i] != '\r') { key[i] = k[i]; i++; }
        key[i] = '\0';
        if (i != 24) return -1;
    }
    if (accept_override)
        snprintf(accept, sizeof accept, "%s", accept_override);
    else if (kn_ws_accept(key, accept) != 0)
        return -1;
    n = snprintf(resp, sizeof resp,
                 "HTTP/1.1 %d %s\r\nUpgrade: websocket\r\nConnection: Upgrade"
                 "\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                 status, status == 101 ? "Switching Protocols" : "Not Found",
                 accept);
    mio_queue(m, resp, (size_t)n);
    return 0;
}

static uint8_t g_msg[70000];

/* A recv shim that synthesizes the handshake response on the first read
 * (it can see the request already captured by send). This keeps the E2E
 * single-threaded and exercises the client's real accept verification. */
typedef struct {
    mio m;
    int status;
    const char *accept_override;
    int answered;
} mio_hs;

static int mio_hs_recv(void *ctx, uint8_t *b, size_t cap, uint32_t t)
{
    mio_hs *h = (mio_hs *)ctx;
    if (!h->answered && h->m.fc_len > 0) {
        h->answered = 1;
        if (mio_queue_handshake(&h->m, h->status, h->accept_override) != 0)
            return -1;
    }
    return mio_recv(&h->m, b, cap, t);
}

static int mio_hs_send(void *ctx, const uint8_t *b, size_t n)
{
    return mio_send(&((mio_hs *)ctx)->m, b, n);
}

static kn_ws_io mio_hs_io(mio_hs *h)
{
    kn_ws_io io;
    io.send = mio_hs_send;
    io.recv = mio_hs_recv;
    io.ctx = h;
    return io;
}

static int hs_open(mio_hs *h, kn_ws_client *c, int status,
                   const char *accept_override, const char *extra,
                   char *err, size_t errsz)
{
    memset(h, 0, sizeof *h);
    h->status = status;
    h->accept_override = accept_override;
    return kn_ws_client_open(c, mio_hs_io(h), "rig.example", 322, "/rtsp",
                             extra, 1000, g_msg, sizeof g_msg, err, errsz);
}

/* ------------------------------------------------------------- tests */

static void test_base64(void)
{
    char out[64];
    printf("-- base64 (RFC 4648 goldens) --\n");
    CHECK(kn_ws_base64((const uint8_t *)"", 0, out, sizeof out) == 0 &&
          strcmp(out, "") == 0, "empty");
    CHECK(kn_ws_base64((const uint8_t *)"f", 1, out, sizeof out) == 4 &&
          strcmp(out, "Zg==") == 0, "f");
    CHECK(kn_ws_base64((const uint8_t *)"fo", 2, out, sizeof out) == 4 &&
          strcmp(out, "Zm8=") == 0, "fo");
    CHECK(kn_ws_base64((const uint8_t *)"foo", 3, out, sizeof out) == 4 &&
          strcmp(out, "Zm9v") == 0, "foo");
    CHECK(kn_ws_base64((const uint8_t *)"foob", 4, out, sizeof out) == 8 &&
          strcmp(out, "Zm9vYg==") == 0, "foob");
    CHECK(kn_ws_base64((const uint8_t *)"fooba", 5, out, sizeof out) == 8 &&
          strcmp(out, "Zm9vYmE=") == 0, "fooba");
    CHECK(kn_ws_base64((const uint8_t *)"foobar", 6, out, sizeof out) == 8 &&
          strcmp(out, "Zm9vYmFy") == 0, "foobar");
    CHECK(kn_ws_base64((const uint8_t *)"foobar", 6, out, 8) == -1,
          "undersized output refused");
}

static void test_accept(void)
{
    char acc[29];
    printf("-- accept (RFC 6455 golden) --\n");
    CHECK(kn_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", acc) == 0 &&
          strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0,
          "RFC 6455 §1.3 example key -> documented accept");
    CHECK(kn_ws_accept(NULL, acc) == -1, "NULL key refused");
}

static void test_rand(void)
{
    uint8_t a[64], b[64];
    printf("-- CSPRNG --\n");
    CHECK(kn_ws_rand(a, sizeof a) == 0, "rand fill ok");
    CHECK(kn_ws_rand(b, sizeof b) == 0, "second fill ok");
    CHECK(memcmp(a, b, sizeof a) != 0, "consecutive draws differ");
    CHECK(kn_ws_rand(NULL, 4) == -1, "NULL refused");
}

static void test_handshake(void)
{
    static mio_hs h;              /* ~1 MiB shim: static, memset per use */
    static kn_ws_client c;
    char err[128];
    const char *req;
    int rc;

    printf("-- opening handshake --\n");
    rc = hs_open(&h, &c, 101, NULL,
                 "x-nv-sessionid: SESS-42\r\ncontent-length: 0\r\n",
                 err, sizeof err);
    CHECK(rc == KN_WS_OK, "handshake completes on a correct 101");
    CHECK(c.handshake_done == 1, "handshake_done set");
    /* request shape (the client side of nvst_rtsp.rs:513-527) */
    h.m.from_client[h.m.fc_len < MIO_CAP - 1 ? h.m.fc_len : MIO_CAP - 1] = 0;
    req = (const char *)h.m.from_client;
    CHECK(strstr(req, "GET /rtsp HTTP/1.1\r\n") == req, "request line + path");
    CHECK(strstr(req, "Host: rig.example:322\r\n") != NULL, "Host header");
    CHECK(strstr(req, "Upgrade: websocket\r\n") != NULL, "Upgrade header");
    CHECK(strstr(req, "Connection: Upgrade\r\n") != NULL, "Connection header");
    CHECK(strstr(req, "Sec-WebSocket-Key: ") != NULL, "Key header present");
    CHECK(strstr(req, "Sec-WebSocket-Version: 13\r\n") != NULL, "version 13");
    CHECK(strstr(req, "x-nv-sessionid: SESS-42\r\n") != NULL,
          "extra headers pass through (x-nv-sessionid)");
    CHECK(strstr(req, "content-length: 0\r\n") != NULL,
          "extra headers pass through (content-length)");
    CHECK(strstr(req, "\r\n\r\n") != NULL, "header block terminated");

    /* failure classes */
    rc = hs_open(&h, &c, 404, NULL, NULL, err, sizeof err);
    CHECK(rc == KN_WS_ERR_HTTP && c.http_status == 404,
          "non-101 -> ERR_HTTP with the status recorded");
    rc = hs_open(&h, &c, 101, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", NULL,
                 err, sizeof err);
    CHECK(rc == KN_WS_ERR_ACCEPT, "wrong accept -> ERR_ACCEPT");
    {   /* timeout: no response bytes at all */
        static mio m;
        mio_reset(&m);
        m.recv_timeouts = 1;
        rc = kn_ws_client_open(&c, mio_io(&m), "h", 1, "/rtsp", NULL, 10,
                               g_msg, sizeof g_msg, err, sizeof err);
        CHECK(rc == KN_WS_ERR_TIMEOUT, "silent peer -> ERR_TIMEOUT");
        CHECK(m.fc_len > 0, "request was still sent before the timeout");
    }
    {   /* malformed answer */
        static mio m;
        static const char garbage[] = "GARBAGE NOT HTTP\r\n\r\n";
        mio_reset(&m);
        mio_queue(&m, garbage, sizeof garbage - 1);
        rc = kn_ws_client_open(&c, mio_io(&m), "h", 1, "/rtsp", NULL, 10,
                               g_msg, sizeof g_msg, err, sizeof err);
        CHECK(rc == KN_WS_ERR_HTTP, "non-HTTP answer -> ERR_HTTP");
    }
    {   /* missing accept header */
        static mio m;
        static const char noacc[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n\r\n";
        mio_reset(&m);
        mio_queue(&m, noacc, sizeof noacc - 1);
        rc = kn_ws_client_open(&c, mio_io(&m), "h", 1, "/rtsp", NULL, 10,
                               g_msg, sizeof g_msg, err, sizeof err);
        CHECK(rc == KN_WS_ERR_ACCEPT, "missing Accept header -> ERR_ACCEPT");
    }
    CHECK(kn_ws_client_open(NULL, mio_io(&h.m), "h", 1, "/", NULL, 10,
                            g_msg, sizeof g_msg, err, sizeof err)
          == KN_WS_ERR_ARGS, "NULL client refused");
}

/* open a client over a plain mio with a pre-queued handshake. The key is
 * random per open, so queue the response lazily through mio_hs, then keep
 * using the same client for framing tests. */
static int open_client(mio_hs *h, kn_ws_client *c)
{
    char err[128];
    return hs_open(h, c, 101, NULL, NULL, err, sizeof err);
}

static void test_send_frames(void)
{
    static mio_hs h;
    static kn_ws_client c;
    static uint8_t big[70000];
    size_t hs_len;
    int rc;

    printf("-- send framing (masked client frames) --\n");
    rc = open_client(&h, &c);
    CHECK(rc == KN_WS_OK, "client opened");
    if (rc != KN_WS_OK) return;
    hs_len = h.m.fc_len;                 /* handshake bytes already sent */

    memset(big, 'A', sizeof big);
    CHECK(kn_ws_send_text(&c, "OPTIONS x RTSP/1.0", 18) == KN_WS_OK,
          "send_text ok");
    {
        const uint8_t *f = h.m.from_client + hs_len;
        size_t n = h.m.fc_len - hs_len;
        uint8_t mask[4];
        CHECK(n == 2 + 4 + 18, "small frame: 2-byte header + mask + payload");
        CHECK(f[0] == 0x81, "FIN + text opcode");
        CHECK((f[1] & 0x80) != 0, "MASK bit set (client frames)");
        CHECK((f[1] & 0x7F) == 18, "7-bit length");
        memcpy(mask, f + 2, 4);
        {
            int i, ok = 1;
            for (i = 0; i < 18; ++i)
                if ((uint8_t)(f[6 + i] ^ mask[i & 3]) !=
                    (uint8_t)"OPTIONS x RTSP/1.0"[i]) ok = 0;
            CHECK(ok, "masked payload unmasks to the exact text");
        }
    }
    hs_len = h.m.fc_len;
    memset(big, 'B', 126);
    CHECK(kn_ws_send_binary(&c, big, 126) == KN_WS_OK, "126-byte send ok");
    CHECK(h.m.from_client[hs_len + 1] == (0x80 | 126),
          "126 uses the 16-bit length form");
    hs_len = h.m.fc_len;
    CHECK(kn_ws_send_binary(&c, big, 65535) == KN_WS_OK, "65535-byte send ok");
    CHECK(h.m.from_client[hs_len + 1] == (0x80 | 126) &&
          h.m.fc_len - hs_len == 8 + 65535,
          "65535: 16-bit form (2+2+4 mask), exact wire size");
    hs_len = h.m.fc_len;
    CHECK(kn_ws_send_binary(&c, big, 65536) == KN_WS_OK, "65536-byte send ok");
    CHECK(h.m.from_client[hs_len + 1] == (0x80 | 127) &&
          h.m.fc_len - hs_len == 14 + 65536,
          "65536: 64-bit form (2+8+4 mask), exact wire size");
    hs_len = h.m.fc_len;
    CHECK(kn_ws_send_ping(&c, (const uint8_t *)"hb", 2) == KN_WS_OK,
          "ping send ok");
    CHECK((h.m.from_client[hs_len] & 0x0F) == 0x9, "ping opcode");
    CHECK(kn_ws_send_ping(&c, big, 126) == KN_WS_ERR_ARGS,
          "ping > 125 refused");
    CHECK(kn_ws_send_text(&c, NULL, 4) == KN_WS_ERR_ARGS, "NULL payload refused");
    /* close */
    hs_len = h.m.fc_len;
    CHECK(kn_ws_send_close(&c, 1000) == KN_WS_OK, "close send ok");
    {
        const uint8_t *cf = h.m.from_client + hs_len;
        CHECK((cf[0] & 0x0F) == 0x8 &&
              (uint8_t)(cf[6] ^ cf[2]) == 0x03 &&
              (uint8_t)(cf[7] ^ cf[3]) == 0xE8,
              "close frame carries masked code 1000");
    }
    CHECK(kn_ws_send_close(&c, 1000) == KN_WS_OK, "close idempotent");
    CHECK(kn_ws_send_text(&c, "x", 1) == KN_WS_ERR_CLOSED,
          "send after close refused");
    /* send failure path */
    {
        static mio_hs h2;
        static kn_ws_client c2;
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "second client opened");
        h2.m.send_fails = 1;
        CHECK(kn_ws_send_text(&c2, "x", 1) == KN_WS_ERR_IO,
              "transport failure -> ERR_IO");
    }
}

static void test_recv_frames(void)
{
    static mio_hs h;
    static kn_ws_client c;
    uint8_t f[256];
    size_t fl, n;
    int op, rc;

    printf("-- receive framing --\n");
    rc = open_client(&h, &c);
    CHECK(rc == KN_WS_OK, "client opened");
    if (rc != KN_WS_OK) return;

    /* single text message */
    fl = sframe(f, 1, 0x1, (const uint8_t *)"RTSP/1.0 200 OK", 15);
    mio_queue(&h.m, f, fl);
    rc = kn_ws_recv_msg(&c, 100, &op, &n);
    CHECK(rc == KN_WS_OK && op == KN_WS_OP_TEXT && n == 15 &&
          memcmp(g_msg, "RTSP/1.0 200 OK", 15) == 0, "single text message");

    /* fragmented text (3 pieces) */
    fl = sframe(f, 0, 0x1, (const uint8_t *)"frag-", 5);   mio_queue(&h.m, f, fl);
    fl = sframe(f, 0, 0x0, (const uint8_t *)"mented-", 7); mio_queue(&h.m, f, fl);
    fl = sframe(f, 1, 0x0, (const uint8_t *)"message", 7); mio_queue(&h.m, f, fl);
    rc = kn_ws_recv_msg(&c, 100, &op, &n);
    CHECK(rc == KN_WS_OK && op == KN_WS_OP_TEXT && n == 19 &&
          memcmp(g_msg, "frag-mented-message", 19) == 0,
          "fragmented message joined in order");

    /* ping answered with echoed pong, not surfaced */
    fl = sframe(f, 1, 0x9, (const uint8_t *)"hb", 2);
    mio_queue(&h.m, f, fl);
    fl = sframe(f, 1, 0x1, (const uint8_t *)"after", 5);
    mio_queue(&h.m, f, fl);
    {
        size_t hs_len = h.m.fc_len;
        rc = kn_ws_recv_msg(&c, 100, &op, &n);
        CHECK(rc == KN_WS_OK && n == 5 && memcmp(g_msg, "after", 5) == 0,
              "ping skipped, next message surfaced");
        CHECK(c.pings_in == 1 && c.pongs_out == 1, "ping/pong counters");
        /* the pong went out: find it after the previously captured bytes */
        {
            const uint8_t *p = h.m.from_client + hs_len;
            size_t avail = h.m.fc_len - hs_len;
            CHECK(avail >= 8 && (p[0] & 0x0F) == 0xA && (p[1] & 0x7F) == 2,
                  "auto-pong frame sent (opcode A, payload 2)");
        }
    }

    /* pong skipped */
    fl = sframe(f, 1, 0xA, NULL, 0);
    mio_queue(&h.m, f, fl);
    fl = sframe(f, 1, 0x2, (const uint8_t *)"\x01\x02\x03", 3);
    mio_queue(&h.m, f, fl);
    rc = kn_ws_recv_msg(&c, 100, &op, &n);
    CHECK(rc == KN_WS_OK && op == KN_WS_OP_BINARY && n == 3,
          "pong skipped, binary message surfaced");
    CHECK(c.pongs_in == 1, "pong counter");

    /* R3-D: pong payload capture (nvst_rtsp.rs:895-901 — the control
     * worker matches the pong against its u64-BE ping sequence) */
    {
        const uint8_t seq[8] = { 0,0,0,0, 0,0,0,7 };
        unsigned long long seen = 0;
        uint8_t got[16];
        size_t gotn = 0;
        int took;
        fl = sframe(f, 1, 0xA, seq, 8);
        mio_queue(&h.m, f, fl);
        fl = sframe(f, 1, 0x2, (const uint8_t *)"z", 1);
        mio_queue(&h.m, f, fl);
        rc = kn_ws_recv_msg(&c, 100, &op, &n);
        CHECK(rc == KN_WS_OK && op == KN_WS_OP_BINARY && n == 1,
              "R3-D: pong with payload skipped, message surfaced");
        CHECK(c.pongs_in == 2, "R3-D: pong counter");
        took = kn_ws_take_pong(&c, &seen, got, sizeof got, &gotn);
        CHECK(took == 1 && gotn == 8 && memcmp(got, seq, 8) == 0,
              "R3-D: take_pong returns the u64-BE payload once");
        took = kn_ws_take_pong(&c, &seen, got, sizeof got, &gotn);
        CHECK(took == 0, "R3-D: take_pong is consume-once (cursor)");
    }


    /* protocol errors */
    fl = sframe(f, 1, 0x1, (const uint8_t *)"x", 1);
    f[1] |= 0x80;                       /* server frame with MASK bit */
    fl += 4;                            /* (mask key bytes ignored by parser) */
    mio_queue(&h.m, f, fl);
    CHECK(kn_ws_recv_msg(&c, 100, &op, &n) == KN_WS_ERR_PROTO,
          "masked server frame -> ERR_PROTO");

    {   /* reopen: the client stays usable after a proto error? NO —
           error is sticky by design; open a fresh one for each class. */
        static mio_hs h2; static kn_ws_client c2;
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "fresh client (ctrl frag)");
        fl = sframe(f, 0, 0x9, (const uint8_t *)"x", 1);   /* fragmented ping */
        mio_queue(&h2.m, f, fl);
        CHECK(kn_ws_recv_msg(&c2, 100, &op, &n) == KN_WS_ERR_PROTO,
              "fragmented control frame -> ERR_PROTO");
    }
    {
        static mio_hs h2; static kn_ws_client c2;
        static uint8_t big[200];
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "fresh client (ctrl size)");
        memset(big, 0, sizeof big);
        fl = sframe(f, 1, 0x9, NULL, 0);
        f[1] = 126;                     /* lie: 16-bit length for control */
        f[2] = 0; f[3] = 126;
        fl = 4;
        mio_queue(&h2.m, f, fl);
        mio_queue(&h2.m, big, 126);
        CHECK(kn_ws_recv_msg(&c2, 100, &op, &n) == KN_WS_ERR_PROTO,
              "control payload > 125 -> ERR_PROTO");
    }
    {   /* RSV bits set */
        static mio_hs h2; static kn_ws_client c2;
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "fresh client (rsv)");
        fl = sframe(f, 1, 0x1, (const uint8_t *)"x", 1);
        f[0] |= 0x40;
        mio_queue(&h2.m, f, fl);
        CHECK(kn_ws_recv_msg(&c2, 100, &op, &n) == KN_WS_ERR_PROTO,
              "RSV bits -> ERR_PROTO");
    }
    {   /* continuation without a start */
        static mio_hs h2; static kn_ws_client c2;
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "fresh client (orphan cont)");
        fl = sframe(f, 1, 0x0, (const uint8_t *)"x", 1);
        mio_queue(&h2.m, f, fl);
        CHECK(kn_ws_recv_msg(&c2, 100, &op, &n) == KN_WS_ERR_PROTO,
              "orphan continuation -> ERR_PROTO");
    }
    {   /* oversized message */
        static mio_hs h2; static kn_ws_client c2;
        uint8_t hdr[10];
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "fresh client (oversize)");
        /* claim a 64-bit length just over the 70000-byte buffer */
        hdr[0] = 0x82; hdr[1] = 127;
        hdr[2] = hdr[3] = hdr[4] = hdr[5] = hdr[6] = 0;
        hdr[7] = 0x01; hdr[8] = 0x11; hdr[9] = 0xCC;   /* 70092 > 70000 */
        mio_queue(&h2.m, hdr, 10);
        CHECK(kn_ws_recv_msg(&c2, 100, &op, &n) == KN_WS_ERR_BIG,
              "message over the caller buffer -> ERR_BIG (no realloc)");
    }

    /* close handshake */
    {
        static mio_hs h2; static kn_ws_client c2;
        uint8_t pl[2];
        size_t hs_len;
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "fresh client (close)");
        pl[0] = 0x03; pl[1] = 0xE9;     /* 1001 going away */
        hs_len = h2.m.fc_len;
        fl = sframe(f, 1, 0x8, pl, 2);
        mio_queue(&h2.m, f, fl);
        CHECK(kn_ws_recv_msg(&c2, 100, &op, &n) == KN_WS_ERR_CLOSED,
              "server close -> ERR_CLOSED");
        CHECK(c2.last_close_code == 1001, "close code recorded");
        CHECK(h2.m.fc_len > hs_len &&
              (h2.m.from_client[hs_len] & 0x0F) == 0x8,
              "close answered with a close frame");
        CHECK(kn_ws_recv_msg(&c2, 100, &op, &n) == KN_WS_ERR_CLOSED,
              "recv after close stays CLOSED");
    }

    /* timeout mid-message is resumable */
    {
        static mio_hs h2; static kn_ws_client c2;
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "fresh client (resume)");
        fl = sframe(f, 1, 0x1, (const uint8_t *)"HELLO", 5);
        mio_queue(&h2.m, f, 4);          /* partial frame */
        CHECK(kn_ws_recv_msg(&c2, 50, &op, &n) == KN_WS_ERR_TIMEOUT,
              "partial frame -> ERR_TIMEOUT");
        mio_queue(&h2.m, f + 4, fl - 4); /* the rest arrives later */
        rc = kn_ws_recv_msg(&c2, 50, &op, &n);
        CHECK(rc == KN_WS_OK && n == 5 && memcmp(g_msg, "HELLO", 5) == 0,
              "resumed recv completes the message");
    }

    /* 1-byte-per-recv chunking (kernel partial reads) */
    {
        static mio_hs h2; static kn_ws_client c2;
        static uint8_t payload[1000];
        uint8_t hdr[16];
        size_t hdrlen;
        CHECK(open_client(&h2, &c2) == KN_WS_OK, "fresh client (chunked)");
        memset(payload, 'z', sizeof payload);
        hdrlen = sframe(hdr, 1, 0x1, NULL, sizeof payload);  /* header only */
        mio_queue(&h2.m, hdr, hdrlen);
        mio_queue(&h2.m, payload, sizeof payload);
        h2.m.chunk = 1;
        rc = kn_ws_recv_msg(&c2, 5000, &op, &n);
        CHECK(rc == KN_WS_OK && n == sizeof payload &&
              memcmp(g_msg, payload, n) == 0,
              "1000-byte message assembled from 1-byte recv chunks");
    }
}

int main(void)
{
    test_base64();
    test_accept();
    test_rand();
    test_handshake();
    test_send_frames();
    test_recv_frames();
    return kn_test_summary("test_knws");
}
