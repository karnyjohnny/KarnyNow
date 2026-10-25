/* test_knsock.c — the stage-2 UDP layer, host- AND target-runnable.
 *
 * Everything here is loopback-only (127.0.0.1): no firewall, no router, no
 * account — the same battery runs in the sandbox and on Win7, so a failure
 * on the target is a Winsock-specific finding, not a test-environment
 * artifact. Section-isolated with per-check accounting (round-XVI rule):
 * a broken section never aborts the run, the summary stays honest.
 *
 * Platform-conditional expectations are marked PLATFORM DELTA and exist
 * exactly once (recvfrom truncation semantics — see kn_sock.h).
 */
/* Feature macro before EVERY system include (glibc hides nanosleep
 * otherwise — the kn_sock.c landmine, same file family). */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_sock.h"
#include "test_port.h"

#include <string.h>

#ifdef _WIN32
#  define kn_sock_sleep_ms(ms) Sleep(ms)
#  include <windows.h>
#else
#  include <time.h>
static void kn_sock_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
}
#endif

#define LOOPBACK_IP 0x0100007Fu   /* 127.0.0.1 in network byte order */

static void fill_pattern(uint8_t *b, size_t n, unsigned seed)
{
    size_t i;
    for (i = 0; i < n; ++i) b[i] = (uint8_t)((i * 31u + seed * 7u) & 0xFFu);
}

static int pattern_ok(const uint8_t *b, size_t n, unsigned seed)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (b[i] != (uint8_t)((i * 31u + seed * 7u) & 0xFFu)) return 0;
    return 1;
}

/* ------------------------------------------------------------- clock */

static void test_clock(void)
{
    uint64_t a, b, ma, mb;

    printf("-- clock --\n");
    a = kn_sock_now_us();
    ma = kn_sock_now_ms();
    CHECK(a > 0, "now_us non-zero (kn_rx 'no time' sentinel is 0)");
    CHECK(ma > 0, "now_ms non-zero");
    kn_sock_sleep_ms(20);
    b = kn_sock_now_us();
    mb = kn_sock_now_ms();
    CHECK(b >= a, "now_us monotonic across a 20 ms sleep");
    CHECK(b - a >= 10000u, "now_us advanced >= 10 ms (got real delta)");
    CHECK(b - a <= 2000000u, "now_us sane upper bound (2 s)");
    CHECK(mb - ma >= 10u && mb - ma <= 2000u, "now_ms agrees with now_us");
}

/* -------------------------------------------------------------- ipv4 */

static void test_ipv4(void)
{
    uint32_t v = 0;
    char buf[16];

    printf("-- ipv4 helpers --\n");
    CHECK(kn_sock_ipv4_parse("127.0.0.1", &v) == 0 && v == LOOPBACK_IP,
          "127.0.0.1 parses to network-order bytes");
    CHECK(kn_sock_ipv4_parse("0.0.0.0", &v) == 0 && v == 0u, "0.0.0.0");
    CHECK(kn_sock_ipv4_parse("255.255.255.255", &v) == 0 &&
          v == 0xFFFFFFFFu, "broadcast");
    CHECK(kn_sock_ipv4_text(LOOPBACK_IP, buf) != NULL &&
          strcmp(buf, "127.0.0.1") == 0, "text round-trip");
    CHECK(kn_sock_ipv4_parse("256.0.0.1", &v) == -1, "256 rejected");
    CHECK(kn_sock_ipv4_parse("1.2.3", &v) == -1, "too short rejected");
    CHECK(kn_sock_ipv4_parse("1.2.3.4.5", &v) == -1, "too long rejected");
    CHECK(kn_sock_ipv4_parse("localhost", &v) == -1, "hostname rejected");
    CHECK(kn_sock_ipv4_parse(NULL, &v) == -1 && kn_sock_ipv4_parse("", &v) == -1,
          "NULL/empty rejected");
    CHECK(kn_sock_ipv4_parse("1.2.3.4", NULL) == -1, "NULL out rejected");
}

/* --------------------------------------------------------- lifecycle */

static void test_lifecycle(void)
{
    kn_sock_udp s, z;
    char err[128];
    uint8_t b[16];
    size_t n;
    int rc;

    printf("-- lifecycle --\n");
    CHECK(kn_sock_startup() == KN_SOCK_OK, "startup succeeds");
    CHECK(kn_sock_startup() == KN_SOCK_OK, "startup refcounted (2nd call)");
    kn_sock_shutdown();
    kn_sock_shutdown();

    memset(&s, 0, sizeof s);
    rc = kn_sock_udp_open(&s, "127.0.0.1", 0, 256u * 1024u, err, sizeof err);
    CHECK(rc == KN_SOCK_OK, "open ephemeral loopback socket");
    CHECK(s.is_open == 1, "is_open set");
    CHECK(kn_sock_udp_port(&s) != 0, "ephemeral port assigned");
    CHECK(s.local_addr == LOOPBACK_IP, "local_addr read back via getsockname");
    CHECK(s.rcvbuf_requested == 256u * 1024u, "rcvbuf_requested recorded");
    CHECK(s.rcvbuf_actual > 0, "rcvbuf_actual read back (getsockopt)");
    /* Linux caps at rmem_max and reports 2x the stored value; Windows
     * honours the request. Both must give a usable buffer >= 64 KiB. */
    CHECK(s.rcvbuf_actual >= 64u * 1024u,
          "rcvbuf_actual usable (>= 64 KiB on any platform)");

    /* closed-socket discipline */
    memset(&z, 0, sizeof z);   /* never opened */
    CHECK(kn_sock_udp_recv(&z, b, sizeof b, 0, &n, NULL) == KN_SOCK_ERR_STATE,
          "recv on never-opened socket -> ERR_STATE");
    CHECK(kn_sock_udp_sendto(&z, LOOPBACK_IP, 9, b, 1) == KN_SOCK_ERR_STATE,
          "sendto on never-opened socket -> ERR_STATE");
    CHECK(kn_sock_udp_port(&z) == 0, "port of closed socket is 0");

    kn_sock_udp_close(&s);
    CHECK(s.is_open == 0 && s.fd_valid == 0, "close clears state");
    kn_sock_udp_close(&s);   /* idempotent — must not crash or double-free */
    CHECK(kn_sock_udp_recv(&s, b, sizeof b, 0, &n, NULL) == KN_SOCK_ERR_STATE,
          "recv after close -> ERR_STATE");
    kn_sock_udp_close(NULL);   /* NULL-safe */
    CHECK(1, "close(NULL) survives");
}

/* ------------------------------------------------------- round-trips */

static void test_roundtrip(void)
{
    kn_sock_udp rx, tx;
    char err[128];
    static uint8_t out[2048], in[2048];
    size_t n = 0;
    int tr = 1, rc, i, got = 0, mism = 0;
    uint16_t rxport;

    printf("-- loopback round-trip --\n");
    rc  = kn_sock_udp_open(&rx, "127.0.0.1", 0, 1u << 20, err, sizeof err);
    rc |= kn_sock_udp_open(&tx, "127.0.0.1", 0, 0, err, sizeof err);
    CHECK(rc == KN_SOCK_OK, "both sockets open");
    rxport = kn_sock_udp_port(&rx);

    for (i = 0; i < 100; ++i) {
        size_t len = (size_t)(1 + (i * 37) % 2048);
        fill_pattern(out, len, (unsigned)i);
        rc = kn_sock_udp_sendto(&tx, LOOPBACK_IP, rxport, out, len);
        if (rc != KN_SOCK_OK) { mism++; continue; }
        rc = kn_sock_udp_recv(&rx, in, sizeof in, 1000, &n, &tr);
        if (rc != 1 || n != len || !pattern_ok(in, n, (unsigned)i) || tr)
            mism++;
        else
            got++;
    }
    CHECK(got == 100 && mism == 0, "100 varying-size datagrams byte-exact");
    CHECK(tx.send_datagrams == 100, "send counter exact");
    CHECK(rx.recv_datagrams == 100, "recv counter exact");
    CHECK(rx.recv_timeouts == 0, "no timeouts on a hot queue");
    {
        uint64_t want = 0;
        for (i = 0; i < 100; ++i) want += (uint64_t)(1 + (i * 37) % 2048);
        CHECK(rx.recv_bytes == want && tx.send_bytes == want,
              "byte counters exact both directions");
    }
    kn_sock_udp_close(&rx);
    kn_sock_udp_close(&tx);
}

static void test_timeout(void)
{
    kn_sock_udp rx;
    char err[128];
    uint8_t b[64];
    size_t n = 12345;
    int tr = 1;
    uint64_t t0, t1;
    int rc;

    printf("-- timeout path --\n");
    CHECK(kn_sock_udp_open(&rx, "127.0.0.1", 0, 0, err, sizeof err) ==
          KN_SOCK_OK, "socket opens");
    t0 = kn_sock_now_us();
    rc = kn_sock_udp_recv(&rx, b, sizeof b, 50, &n, &tr);
    t1 = kn_sock_now_us();
    CHECK(rc == 0, "idle socket: recv returns 0 (timeout, not error)");
    CHECK(n == 0 && tr == 0, "timeout clears out params");
    CHECK(t1 - t0 >= 40000u, "timeout waited >= 40 ms");
    CHECK(t1 - t0 <= 1000000u, "timeout returned within 1 s");
    CHECK(rx.recv_timeouts == 1, "timeout counter");
    t0 = kn_sock_now_us();
    rc = kn_sock_udp_recv(&rx, b, sizeof b, 0, &n, NULL);
    t1 = kn_sock_now_us();
    CHECK(rc == 0 && t1 - t0 < 20000u, "timeout_ms=0 is a pure poll");
    kn_sock_udp_close(&rx);
}

static void test_max_datagram(void)
{
    kn_sock_udp rx, tx;
    char err[128];
    static uint8_t big[KN_SOCK_MAX_DATAGRAM], in[KN_SOCK_RECV_BUF];
    size_t n = 0;
    int rc;

    printf("-- max datagram (65507 B) --\n");
    rc  = kn_sock_udp_open(&rx, "127.0.0.1", 0, 1u << 20, err, sizeof err);
    rc |= kn_sock_udp_open(&tx, "127.0.0.1", 0, 0, err, sizeof err);
    CHECK(rc == KN_SOCK_OK, "sockets open");
    fill_pattern(big, sizeof big, 99);
    CHECK(kn_sock_udp_sendto(&tx, LOOPBACK_IP, kn_sock_udp_port(&rx),
                             big, sizeof big) == KN_SOCK_OK,
          "65507 B send accepted");
    rc = kn_sock_udp_recv(&rx, in, sizeof in, 2000, &n, NULL);
    CHECK(rc == 1 && n == KN_SOCK_MAX_DATAGRAM, "65507 B received whole");
    CHECK(n == KN_SOCK_MAX_DATAGRAM && memcmp(big, in, n) == 0,
          "65507 B byte-exact");
    CHECK(kn_sock_udp_sendto(&tx, LOOPBACK_IP, kn_sock_udp_port(&rx),
                             big, KN_SOCK_MAX_DATAGRAM + 1) == KN_SOCK_ERR_ARGS,
          "oversize send rejected by the API (UDP payload max)");
    kn_sock_udp_close(&rx);
    kn_sock_udp_close(&tx);
}

static void test_burst(void)
{
    kn_sock_udp rx, tx;
    char err[128];
    static uint8_t pkt[64], in[KN_SOCK_RECV_BUF];
    int i, rc, got = 0, send_fail = 0, order_fail = 0, content_fail = 0;
    size_t n;
    uint64_t t0, t1;

    printf("-- burst (backlog drain) --\n");
    /* 512 x 64 B = 32 KiB — comfortably inside the rcvbuf floor every
     * platform provides (sandbox measurement: Linux gives >= 416 KiB even
     * capped; Windows honours the request). The point is the drain loop,
     * not kernel limits. Loopback delivery of a single flow is ordered on
     * both kernels; the seq marker asserts that empirically, per burst. */
    rc  = kn_sock_udp_open(&rx, "127.0.0.1", 0, 256u * 1024u, err, sizeof err);
    rc |= kn_sock_udp_open(&tx, "127.0.0.1", 0, 0, err, sizeof err);
    CHECK(rc == KN_SOCK_OK, "sockets open");
    t0 = kn_sock_now_us();
    for (i = 0; i < 512; ++i) {
        fill_pattern(pkt + 2, sizeof pkt - 2, (unsigned)i);
        pkt[0] = (uint8_t)i; pkt[1] = (uint8_t)(i >> 8);   /* u16LE seq */
        if (kn_sock_udp_sendto(&tx, LOOPBACK_IP, kn_sock_udp_port(&rx),
                               pkt, sizeof pkt) != KN_SOCK_OK) send_fail++;
    }
    while (got < 512) {
        unsigned seq;
        rc = kn_sock_udp_recv(&rx, in, sizeof in, 500, &n, NULL);
        if (rc != 1) break;                 /* timeout/error: drain over */
        if (n != sizeof pkt) { content_fail++; got++; continue; }
        seq = (unsigned)in[0] | ((unsigned)in[1] << 8);
        if (seq != (unsigned)got) order_fail++;
        if (!pattern_ok(in + 2, n - 2, seq)) content_fail++;
        got++;
    }
    t1 = kn_sock_now_us();
    CHECK(send_fail == 0, "all 512 burst sends accepted");
    CHECK(got == 512, "all 512 burst datagrams drained from the backlog");
    CHECK(order_fail == 0, "loopback order preserved (seq markers exact)");
    CHECK(content_fail == 0, "burst payloads byte-exact");
    CHECK(rx.recv_datagrams == 512, "burst recv counter exact");
    CHECK(t1 - t0 < 10000000u, "burst round-trip < 10 s");
    kn_sock_udp_close(&rx);
    kn_sock_udp_close(&tx);
}

static void test_truncation(void)
{
    kn_sock_udp rx, tx;
    char err[128];
    static uint8_t pkt[1024], in[64];
    size_t n = 0;
    int tr = 0, rc;

    printf("-- undersized recv buffer (PLATFORM DELTA) --\n");
    rc  = kn_sock_udp_open(&rx, "127.0.0.1", 0, 1u << 20, err, sizeof err);
    rc |= kn_sock_udp_open(&tx, "127.0.0.1", 0, 0, err, sizeof err);
    CHECK(rc == KN_SOCK_OK, "sockets open");
    fill_pattern(pkt, sizeof pkt, 5);
    CHECK(kn_sock_udp_sendto(&tx, LOOPBACK_IP, kn_sock_udp_port(&rx),
                             pkt, sizeof pkt) == KN_SOCK_OK, "1024 B sent");
    rc = kn_sock_udp_recv(&rx, in, sizeof in, 1000, &n, &tr);
#ifdef _WIN32
    /* Win32: WSAEMSGSIZE — the datagram is LOST (documented, counted) */
    CHECK(rc == KN_SOCK_ERR_MSGSIZE, "win32: undersized recv -> ERR_MSGSIZE");
    CHECK(rx.recv_msgsize == 1, "win32: msgsize counter");
    rc = kn_sock_udp_recv(&rx, in, sizeof in, 50, &n, &tr);
    CHECK(rc == 0, "win32: the datagram is gone (next recv times out)");
#else
    /* POSIX: silent truncation, honest flag */
    CHECK(rc == 1 && n == sizeof in && tr == 1,
          "posix: truncated to the buffer, flag set");
    CHECK(memcmp(in, pkt, sizeof in) == 0, "posix: truncated prefix exact");
#endif
    kn_sock_udp_close(&rx);
    kn_sock_udp_close(&tx);
}

static void test_bad_args(void)
{
    kn_sock_udp s, tx;
    char err[128];
    uint8_t b[64];
    size_t n;

    memset(b, 0xA5, sizeof b);
    printf("-- bad arguments --\n");
    CHECK(kn_sock_udp_open(NULL, "127.0.0.1", 0, 0, err, sizeof err) ==
          KN_SOCK_ERR_ARGS, "open(NULL) -> ERR_ARGS");
    memset(&s, 0, sizeof s);
    CHECK(kn_sock_udp_open(&s, "not-an-ip", 0, 0, err, sizeof err) ==
          KN_SOCK_ERR_ARGS, "open(bad ip) -> ERR_ARGS");
    CHECK(err[0] != '\0', "open failure fills the error buffer");
    CHECK(kn_sock_udp_open(&s, "127.0.0.1", 0, 0, NULL, 0) == KN_SOCK_OK,
          "open with NULL err buffer works");
    CHECK(kn_sock_udp_recv(&s, NULL, 64, 0, &n, NULL) == KN_SOCK_ERR_ARGS,
          "recv(NULL buf) -> ERR_ARGS");
    CHECK(kn_sock_udp_recv(&s, b, 0, 0, &n, NULL) == KN_SOCK_ERR_ARGS,
          "recv(cap 0) -> ERR_ARGS");
    CHECK(kn_sock_udp_recv(NULL, b, sizeof b, 0, &n, NULL) == KN_SOCK_ERR_ARGS,
          "recv(NULL sock) -> ERR_ARGS");
    CHECK(kn_sock_udp_sendto(&s, LOOPBACK_IP, 9, NULL, 4) == KN_SOCK_ERR_ARGS,
          "sendto(NULL data, len>0) -> ERR_ARGS");
    /* send to a port nobody owns: succeeds at the API level (UDP is
     * connectionless; an ICMP port-unreachable may or may not surface —
     * we do NOT assert on it, only that the call is honest and stable) */
    CHECK(kn_sock_udp_open(&tx, "127.0.0.1", 0, 0, err, sizeof err) ==
          KN_SOCK_OK, "second socket opens");
    (void)kn_sock_udp_sendto(&tx, LOOPBACK_IP, 1, b, 1);
    CHECK(1, "sendto into the void survives (UDP semantics)");
    kn_sock_udp_close(&s);
    kn_sock_udp_close(&tx);
}

int main(void)
{
    test_clock();
    test_ipv4();
    test_lifecycle();
    test_roundtrip();
    test_timeout();
    test_max_datagram();
    test_burst();
    test_truncation();
    test_bad_args();
    return kn_test_summary("test_knsock");
}
