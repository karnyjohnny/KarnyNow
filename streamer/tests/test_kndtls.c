/* test_kndtls.c — the FIFTEENTH suite: kn_dtls (R3-B, docs/DTLS.md).
 * Self-adaptive by design (the test_kncert pattern): under the CMake
 * mbedtls backend the SAME binary runs the real battery — a full DTLS
 * 1.2 handshake over REAL loopback UDP against an in-test mbedTLS
 * server shaped like the GFN rig (HelloVerifyRequest cookies ON, client
 * certificate REQUIRED and fingerprint-PINNED — pinning in BOTH
 * directions, nvst.rs:6245-6300) — and in every other build it checks
 * the honest-stub contract. Either path is a full pass.
 *
 * The wire-shape independence leg (an OpenSSL s_server accepting our
 * ClientHello/cert/Finished) lives in tools/dtls_openssl_check.py —
 * sandbox-gated, like the POSIX-only legs of T13.
 *
 * Battery: guards · full handshake (mutual pin, version, ciphersuite,
 * counters) · app-data drain (the R3-C hook, D-t4) · loss + timer-
 * driven retransmission in BOTH directions · wrong-pin refusal (fatal
 * latch, no crash) · inject-queue overflow · mid-handshake free.
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_dtls.h"
#include "kn/kn_cert.h"
#include "test_port.h"

#include <string.h>

#ifdef KN_TLS_BACKEND_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>   /* error constants only */
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/x509_crt.h>
#endif

/* ------------------------------------------------- portable predicate */

static void test_peek(void)
{
    const uint8_t stun[20] = { 0x00, 0x01, 0,0, 0x21,0x12,0xA4,0x42 };
    const uint8_t stun_ok[4] = { 0x01, 0x01, 0, 0 };
    const uint8_t hs[4]   = { 22, 0xfe, 0xfd, 0 };   /* Handshake */
    const uint8_t ccs[4]  = { 20, 0xfe, 0xfd, 0 };   /* ChangeCipherSpec */
    const uint8_t app[4]  = { 23, 0xfe, 0xfd, 0 };   /* ApplicationData */
    const uint8_t hi63[2] = { 63, 1 };               /* the range edge */
    const uint8_t rtp[4]  = { 0x80, 0x60, 0, 0 };
    const uint8_t ping[4] = { 'P', 'I', 'N', 'G' };  /* 0x50 = 80: NOT dtls */
    printf("-- kn_dtls demux predicate (looks_like_dtls, nvst.rs:6345) --\n");
    CHECK(kn_dtls_peek(hs, sizeof hs) == 1 &&
          kn_dtls_peek(ccs, sizeof ccs) == 1 &&
          kn_dtls_peek(app, sizeof app) == 1 &&
          kn_dtls_peek(hi63, sizeof hi63) == 1 &&
          kn_dtls_peek(stun, sizeof stun) == 0 &&
          kn_dtls_peek(stun_ok, sizeof stun_ok) == 0 &&
          kn_dtls_peek(rtp, sizeof rtp) == 0 &&
          kn_dtls_peek(ping, sizeof ping) == 0 &&
          kn_dtls_peek(NULL, 0) == 0 &&
          kn_dtls_peek(hs, 0) == 0,
          "peek: DTLS content types 20..63 only; STUN/RTP/PING/edge safe");
}

/* ------------------------------------------------------- stub contract */

static void test_stub_mode(void)
{
    char err[128];
    kn_dtls *d;
    kn_dtls_stats st;
    char ver_cs[64];

    printf("-- kn_dtls stub contract --\n");
    err[0] = '\0';
    d = kn_dtls_client(NULL, 0, 0, NULL, NULL, err, sizeof err);
    CHECK(d == NULL && err[0] != '\0',
          "stub: client refuses loudly with a reason");
    CHECK(kn_dtls_inject(NULL, (const uint8_t *)"x", 1) == KN_DTLS_ERR_STATE &&
          kn_dtls_poll(NULL) == KN_DTLS_ERR_STATE &&
          kn_dtls_ready(NULL) == 0 &&
          kn_dtls_version(NULL)[0] == '\0' &&
          kn_dtls_ciphersuite(NULL, ver_cs, sizeof ver_cs) == -1 &&
          kn_dtls_ready_ms(NULL) == 0,
          "stub: every accessor is NULL-safe and negative");
    st.records_in = 123;
    kn_dtls_stats_get(NULL, &st);
    CHECK(st.records_in == 0, "stub: stats_get zeroes the output");
    kn_dtls_free(NULL);
    CHECK(1, "stub: free(NULL) is safe");
}

#ifdef KN_TLS_BACKEND_MBEDTLS
/* ================= the in-test server (the rig's shape) ==============
 * mbedTLS DTLS 1.2 server over the SAME inject-queue BIO discipline the
 * production module uses: the harness below is the single reader of both
 * sockets and relays datagrams (with optional drops) into the two
 * queues — one owner per socket, exactly like the pump thread. */

#define TQ_SLOTS 16u
#define TQ_REC   2048u

typedef struct {
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_ssl_cookie_ctx   cookie;
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    uint8_t  q[TQ_SLOTS][TQ_REC];
    uint16_t qlen[TQ_SLOTS];
    unsigned qh, qc;
    uint8_t  pin[32];           /* the CLIENT fingerprint to pin */
    uint8_t  seen_fp[32];       /* what the client actually presented */
    int      pin_matched, saw_client_cert;
    uint32_t vflags;            /* verify flags the server saw */
    uint64_t t0; uint32_t inter_ms, fin_ms; int running;
    int      hs_done, hs_rc;
    char     tid[32];           /* client transport id (the cookie key) */
    kn_sock_udp *sock;
    uint32_t   dest_ip;
    uint16_t   dest_port;
} tsrv;

static int tsrv_verify(void *p, mbedtls_x509_crt *crt, int depth,
                       uint32_t *flags)
{
    tsrv *s = (tsrv *)p;
    if (depth != 0 || !crt || !crt->raw.p) return 0;
    s->saw_client_cert = 1;
    if (mbedtls_sha256(crt->raw.p, crt->raw.len, s->seen_fp, 0) != 0)
        return 0;
    if (memcmp(s->seen_fp, s->pin, 32) == 0) {
        *flags = 0;
        s->pin_matched = 1;
    }
    s->vflags = *flags;
    return 0;
}

static int tsrv_send(void *ctx, const unsigned char *buf, size_t len)
{
    tsrv *s = (tsrv *)ctx;
    if (kn_sock_udp_sendto(s->sock, s->dest_ip, s->dest_port, buf, len)
        != KN_SOCK_OK)
        return MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)len;
}

static int tsrv_recv(void *ctx, unsigned char *buf, size_t len,
                     uint32_t timeout)
{
    tsrv *s = (tsrv *)ctx;
    size_t n;
    (void)timeout;
    /* WANT_READ, not TIMEOUT — see the note in kn_dtls.c: a BIO TIMEOUT
     * is read by fetch_input as an expired retransmission delay */
    if (s->qc == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    n = s->qlen[s->qh];
    if (n > len) {
        s->qh = (s->qh + 1u) % TQ_SLOTS;
        s->qc--;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    memcpy(buf, s->q[s->qh], n);
    s->qh = (s->qh + 1u) % TQ_SLOTS;
    s->qc--;
    return (int)n;
}

static void tsrv_timer_set(void *ctx, uint32_t i_ms, uint32_t f_ms)
{
    tsrv *s = (tsrv *)ctx;
    if (f_ms == 0) { s->running = 0; return; }
    s->t0 = kn_sock_now_ms(); s->inter_ms = i_ms; s->fin_ms = f_ms;
    s->running = 1;
}

static int tsrv_timer_get(void *ctx)
{
    tsrv *s = (tsrv *)ctx;
    uint64_t e;
    if (!s->running) return -1;
    e = kn_sock_now_ms() - s->t0;
    if (e >= s->fin_ms) return 2;
    if (e >= s->inter_ms) return 1;
    return 0;
}

static int tsrv_inject(tsrv *s, const uint8_t *data, size_t len)
{
    unsigned slot;
    if (len == 0 || len > TQ_REC) return -1;
    if (s->qc >= TQ_SLOTS) return -2;
    slot = (s->qh + s->qc) % TQ_SLOTS;
    memcpy(s->q[slot], data, len);
    s->qlen[slot] = (uint16_t)len;
    s->qc++;
    return 0;
}

static void tsrv_free(tsrv *s)
{
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_ssl_cookie_free(&s->cookie);
    mbedtls_ctr_drbg_free(&s->drbg);
    mbedtls_entropy_free(&s->ent);
}

/* returns 0 ok / negative mbedtls rc */
static int tsrv_init(tsrv *s, kn_sock_udp *sock, uint32_t dest_ip,
                     uint16_t dest_port, kn_cert *id,
                     const uint8_t client_pin[32],
                     const char *transport_id)
{
    int ret;
    memset(s, 0, sizeof *s);
    s->sock = sock; s->dest_ip = dest_ip; s->dest_port = dest_port;
    memcpy(s->pin, client_pin, 32);
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_ssl_cookie_init(&s->cookie);
    mbedtls_entropy_init(&s->ent);
    mbedtls_ctr_drbg_init(&s->drbg);
    ret = mbedtls_ctr_drbg_seed(&s->drbg, mbedtls_entropy_func, &s->ent,
                                (const unsigned char *)"tsrv", 4);
    if (ret != 0) return ret;
    ret = mbedtls_ssl_cookie_setup(&s->cookie, mbedtls_ctr_drbg_random,
                                   &s->drbg);
    if (ret != 0) return ret;
    ret = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_SERVER,
                                      MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return ret;
    mbedtls_ssl_conf_min_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->drbg);
    /* the rig's shape, mbedTLS-legal: OPTIONAL + the pin callback (a
     * VERIFY_REQUIRED server without a CA chain dies with
     * CA_CHAIN_REQUIRED, ssl_tls.c:8071); the pin is ENFORCED below —
     * tsrv_step fails the "handshake" with a close_notify when the
     * client certificate did not match, exactly what the real rig does
     * when the presented cert is not the ANNOUNCE'd one. */
    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&s->conf, tsrv_verify, s);
    mbedtls_ssl_conf_dtls_cookies(&s->conf, mbedtls_ssl_cookie_write,
                                  mbedtls_ssl_cookie_check, &s->cookie);
    ret = mbedtls_ssl_conf_own_cert(&s->conf,
                                    (mbedtls_x509_crt *)kn_cert_x509(id),
                                    (mbedtls_pk_context *)kn_cert_pk(id));
    if (ret != 0) return ret;
    ret = mbedtls_ssl_setup(&s->ssl, &s->conf);
    if (ret != 0) return ret;
    mbedtls_ssl_set_mtu(&s->ssl, 1200);
    mbedtls_ssl_set_bio(&s->ssl, s, tsrv_send, NULL, tsrv_recv);
    mbedtls_ssl_set_timer_cb(&s->ssl, s, tsrv_timer_set, tsrv_timer_get);
    snprintf(s->tid, sizeof s->tid, "%s", transport_id);
    ret = mbedtls_ssl_set_client_transport_id(&s->ssl,
                                              (const unsigned char *)s->tid,
                                              strlen(s->tid));
    return ret;
}

static int tsrv_step(tsrv *s)
{
    int ret;
    if (s->hs_done) return s->hs_rc;
    ret = mbedtls_ssl_handshake(&s->ssl);
    if (ret == 0) {
        /* the server-side pin gate (mirrors kn_dtls_poll's) */
        if (!s->pin_matched ||
            mbedtls_ssl_get_verify_result(&s->ssl) != 0) {
            (void)mbedtls_ssl_close_notify(&s->ssl);
            s->hs_done = 1;
            s->hs_rc = MBEDTLS_ERR_SSL_BAD_CERTIFICATE;
            return s->hs_rc;
        }
        s->hs_done = 1; s->hs_rc = 0; return 0;
    }
    if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
        /* the documented DTLS-server contract (programs/ssl/dtls_server.c):
         * the HVR is already on the wire; reset for the cookied
         * ClientHello and re-arm the transport id the cookie binds to */
        (void)mbedtls_ssl_session_reset(&s->ssl);
        (void)mbedtls_ssl_set_client_transport_id(
            &s->ssl, (const unsigned char *)s->tid, strlen(s->tid));
        return 1;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
        ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
        ret == MBEDTLS_ERR_SSL_TIMEOUT)
        return 1;                   /* in progress */
    s->hs_done = 1; s->hs_rc = ret; /* fatal */
    return ret;
}

/* ------------------------------------------------- the pair harness */

typedef struct {
    kn_sock_udp csock, ssock;
    kn_dtls    *cli;
    tsrv        srv;
    kn_cert    *cid, *sid, *wrong;
    unsigned    drop_to_cli, drop_to_srv, dropped_c, dropped_s;
    int         cli_rc, cli_done, cli_ready;
    uint32_t    cport, sport;
} pair_t;

static void pair_free(pair_t *p)
{
    if (p->cli) { kn_dtls_free(p->cli); p->cli = NULL; }
    tsrv_free(&p->srv);
    kn_cert_free(p->cid);
    kn_cert_free(p->sid);
    kn_cert_free(p->wrong);
    kn_sock_udp_close(&p->csock);
    kn_sock_udp_close(&p->ssock);
}

/* wrong_pin: pin a THIRD identity's fingerprint (never presented).
 * drop_*: the harness discards the first N datagrams in each direction
 * (loss legs — the retransmission timers must recover). */
static int pair_setup(pair_t *p, int wrong_pin, unsigned drop_to_cli,
                      unsigned drop_to_srv, char *err, size_t errsz)
{
    char cfpc[KN_CERT_FP_LEN + 1];
    char tid[32];
    uint32_t lo;
    memset(p, 0, sizeof *p);
    p->drop_to_cli = drop_to_cli;
    p->drop_to_srv = drop_to_srv;
    if (kn_sock_ipv4_parse("127.0.0.1", &lo) != 0) return -1;
    if (kn_sock_udp_open(&p->csock, "127.0.0.1", 0, 262144, err, errsz)
        != KN_SOCK_OK) return -2;
    if (kn_sock_udp_open(&p->ssock, "127.0.0.1", 0, 262144, err, errsz)
        != KN_SOCK_OK) { pair_free(p); return -3; }
    p->cport = kn_sock_udp_port(&p->csock);
    p->sport = kn_sock_udp_port(&p->ssock);
    if (kn_sock_udp_connect(&p->csock, lo, p->sport) != KN_SOCK_OK ||
        kn_sock_udp_connect(&p->ssock, lo, p->cport) != KN_SOCK_OK) {
        pair_free(p); return -4;
    }
    p->cid = kn_cert_generate(err, errsz);
    p->sid = kn_cert_generate(err, errsz);
    if (!p->cid || !p->sid) { pair_free(p); return -5; }
    if (wrong_pin) {
        p->wrong = kn_cert_generate(err, errsz);
        if (!p->wrong) { pair_free(p); return -6; }
    }
    /* the server pins the client's TRUE fingerprint (the rig pins what
     * the ANNOUNCE carried); the client pins the server's — or the
     * wrong one for the refusal leg */
    if (kn_cert_fingerprint(p->cid, cfpc, sizeof cfpc) != 0) {
        pair_free(p); return -7;
    }
    {   /* colon-hex -> binary for the server pin */
        size_t i;
        uint8_t bin[32];
        for (i = 0; i < 32; ++i) {
            unsigned v;
            if (sscanf(cfpc + i * 3, "%2x", &v) != 1) { pair_free(p); return -8; }
            bin[i] = (uint8_t)v;
        }
        snprintf(tid, sizeof tid, "127.0.0.1:%u", p->cport);
        if (tsrv_init(&p->srv, &p->ssock, lo, p->cport, p->sid, bin, tid)
            != 0) { pair_free(p); return -9; }
    }
    {   /* the client's pin: sid's fingerprint, or the wrong cert's */
        char fp[KN_CERT_FP_LEN + 1];
        if (kn_cert_fingerprint(wrong_pin ? p->wrong : p->sid, fp,
                                sizeof fp) != 0) {
            pair_free(p); return -10;
        }
        p->cli = kn_dtls_client(&p->csock, lo, p->sport, p->cid, fp,
                                err, errsz);
        if (!p->cli) { pair_free(p); return -11; }
    }
    return 0;
}

/* relay one datagram from `from` into the destination queue, honouring
 * the drop budget. Returns 1 when a datagram moved (or was dropped). */
static int pair_pump_sock(pair_t *p, int to_client)
{
    uint8_t buf[TQ_REC];
    size_t len = 0;
    int n = kn_sock_udp_recv(to_client ? &p->csock : &p->ssock,
                             buf, sizeof buf, to_client ? 2u : 0u,
                             &len, NULL);
    if (n != 1) return 0;
    if (to_client) {
        if (p->dropped_c < p->drop_to_cli) { p->dropped_c++; return 1; }
        (void)kn_dtls_inject(p->cli, buf, len);
    } else {
        if (p->dropped_s < p->drop_to_srv) { p->dropped_s++; return 1; }
        (void)tsrv_inject(&p->srv, buf, len);
    }
    return 1;
}

/* run both handshakes to completion/failure within budget_ms.
 * Returns: 1 both ready, 0 budget exhausted, -1 client fatal (cli_rc
 * set), -2 server fatal (hs_rc set). After ready, extra_iters more
 * relay rounds run (the app-data drain window). */
static int pair_run(pair_t *p, uint32_t budget_ms, unsigned extra_iters)
{
    uint64_t t_end = kn_sock_now_ms() + budget_ms;
    int srv_rc_seen = 0;
    unsigned extra = 0;
    while (kn_sock_now_ms() < t_end) {
        (void)pair_pump_sock(p, 1);
        (void)pair_pump_sock(p, 0);
        if (!p->cli_done) {
            int rc = kn_dtls_poll(p->cli);
            if (rc == KN_DTLS_READY && !p->cli_ready) {
                p->cli_ready = 1;
            }
            if (rc < 0) { p->cli_done = 1; p->cli_rc = rc; return -1; }
        }
        if (!p->srv.hs_done) {
            int rc = tsrv_step(&p->srv);
            if (rc < 0 && rc != 1) { srv_rc_seen = rc; return -2; }
        } else if (p->srv.hs_rc != 0) {
            return -2;
        }
        if (p->cli_ready && p->srv.hs_done && p->srv.hs_rc == 0) {
            if (extra == 0 && extra_iters == 0) return 1;
            if (extra >= extra_iters) return 1;
            extra++;
        }
    }
    (void)srv_rc_seen;
    return 0;
}

/* --------------------------------------------------- the real battery */

static void test_real_mode(void)
{
    char err[256], cs[80], cfpc[KN_CERT_FP_LEN + 1];
    pair_t p;
    kn_dtls_stats st;
    int rc;

    printf("-- kn_dtls real battery (mbedTLS, loopback UDP) --\n");

    /* guards */
    err[0] = '\0';
    CHECK(kn_dtls_client(NULL, 0, 0, NULL, NULL, err, sizeof err) == NULL &&
          err[0] != '\0',
          "guards: NULL arguments refuse loudly");
    {   /* a malformed fingerprint never creates a client */
        kn_sock_udp s;
        kn_cert *id = kn_cert_generate(err, sizeof err);
        uint32_t lo = 0;
        (void)kn_sock_ipv4_parse("127.0.0.1", &lo);
        rc = kn_sock_udp_open(&s, "127.0.0.1", 0, 262144, err, sizeof err);
        CHECK(rc == KN_SOCK_OK && id != NULL, "guards: fixtures open");
        if (rc == KN_SOCK_OK && id) {
            err[0] = '\0';
            CHECK(kn_dtls_client(&s, lo, 1, id, "not-a-fingerprint",
                                 err, sizeof err) == NULL && err[0] != '\0',
                  "guards: a malformed pin refuses (parse_nvst_fingerprint "
                  "semantics)");
            err[0] = '\0';
            CHECK(kn_dtls_client(&s, lo, 1, id, "sha-256 ZZ",
                                 err, sizeof err) == NULL,
                  "guards: non-hex bytes refuse even with the algorithm "
                  "prefix");
            kn_cert_free(id);
        }
        kn_sock_udp_close(&s);
    }

    /* ---- the full handshake, rig-shaped server ---- */
    err[0] = '\0';
    rc = pair_setup(&p, 0, 0, 0, err, sizeof err);
    if (rc != 0) printf("pair_setup rc=%d err=%s\n", rc, err);
    CHECK(rc == 0, "pair: loopback fixture arms (client + pinned server)");
    if (rc == 0) {
        int run = pair_run(&p, 15000u, 0);
        if (run != 1) {
            kn_dtls_stats dbg;
            memset(&dbg, 0, sizeof dbg);
            kn_dtls_stats_get(p.cli, &dbg);
            printf("   diag: run=%d cli_rc=%d cli_last_rc=-0x%04x cli_pin=%d "
                   "cli_verify=%d srv_done=%d srv_hs_rc=-0x%04x srv_vflags=0x%x "
                   "cli[in=%u out=%u drop=%u snderr=%u polls=%u] "
                   "srv[cert=%d pin=%d] dropped c=%u s=%u\n",
                   run, p.cli_rc,
                   (unsigned)(dbg.last_rc < 0 ? -dbg.last_rc : dbg.last_rc),
                   dbg.pin_matched, dbg.verify_result, p.srv.hs_done,
                   (unsigned)(p.srv.hs_rc < 0 ? -p.srv.hs_rc : p.srv.hs_rc),
                   p.srv.vflags,
                   dbg.records_in, dbg.records_out, dbg.records_dropped,
                   dbg.send_errors, dbg.polls, p.srv.saw_client_cert,
                   p.srv.pin_matched, p.dropped_c, p.dropped_s);
        }
        CHECK(run == 1, "handshake completes over real loopback UDP "
                        "(HVR cookie round included)");
        CHECK(kn_dtls_ready(p.cli) == 1, "ready flag set");
        memset(&st, 0, sizeof st);
        kn_dtls_stats_get(p.cli, &st);
        CHECK(st.pin_matched == 1 && st.verify_result == 0 &&
              st.records_in > 0 && st.records_out > 0 &&
              st.send_errors == 0 && st.records_dropped == 0,
              "client evidence: pin matched, verify clean, records flew, "
              "zero send errors");
        CHECK(strcmp(kn_dtls_version(p.cli), "DTLSv1.2") == 0,
              "negotiated exactly DTLS 1.2 (the str0m default, the Win7 "
              "ceiling)");
        CHECK(kn_dtls_ciphersuite(p.cli, cs, sizeof cs) == 0 &&
              strstr(cs, "ECDSA") != NULL,
              "ciphersuite is an ECDHE-ECDSA suite (the P-256 identity)");
        CHECK(kn_dtls_ready_ms(p.cli) > 0 && kn_dtls_ready_ms(p.cli) < 15000,
              "ready_ms is a plausible wall duration");
        CHECK(p.srv.pin_matched == 1 && p.srv.saw_client_cert == 1,
              "SERVER side pinned us too: the client certificate flew and "
              "matched the announced fingerprint (the rig's shape — "
              "pinning in BOTH directions)");
        CHECK(kn_cert_fingerprint(p.cid, cfpc, sizeof cfpc) == 0 &&
              p.srv.hs_done && p.srv.hs_rc == 0,
              "server handshake completed cleanly");
        /* app-data drain: the R3-C hook (D-t4 — counted, not dropped
         * silently) */
        {
            const unsigned char msg[] = "kn-r3b";
            int w = mbedtls_ssl_write(&p.srv.ssl, msg, sizeof msg - 1);
            CHECK(w == (int)(sizeof msg - 1), "server writes an app record");
            (void)pair_run(&p, 3000u, 8);
            memset(&st, 0, sizeof st);
            kn_dtls_stats_get(p.cli, &st);
            CHECK(st.app_records >= 1 && st.app_bytes >= sizeof msg - 1,
                  "client drains post-ready app data into the counted "
                  "discard path (D-t4)");
            CHECK(st.first_app_len == sizeof msg - 1 &&
                  memcmp(st.first_app, msg, sizeof msg - 1) == 0,
                  "the FIRST app record's head is captured verbatim "
                  "(run-13 evidence for R3-C: what does the rig send "
                  "after the handshake?)");
            CHECK(kn_dtls_poll(p.cli) == KN_DTLS_READY,
                  "poll stays READY (idempotent) after the handshake");
        }
        pair_free(&p);
    }

    /* ---- loss legs: the retransmission timers must carry it ---- */
    err[0] = '\0';
    rc = pair_setup(&p, 0, 1, 1, err, sizeof err);
    CHECK(rc == 0, "loss pair arms");
    if (rc == 0) {
        /* first datagram dropped in EACH direction: without working
         * set/get_timer callbacks a DTLS handshake can never recover */
        int run = pair_run(&p, 25000u, 0);
        CHECK(run == 1, "handshake survives first-flight loss in both "
                        "directions (timer-driven retransmission)");
        CHECK(p.dropped_c == 1 && p.dropped_s == 1,
              "the loss budget was actually spent (the leg is honest)");
        pair_free(&p);
    }

    /* ---- wrong pin: fatal, latched, honest ---- */
    err[0] = '\0';
    rc = pair_setup(&p, 1, 0, 0, err, sizeof err);
    CHECK(rc == 0, "wrong-pin pair arms");
    if (rc == 0) {
        int run = pair_run(&p, 15000u, 0);
        CHECK(run == -1 && p.cli_rc == KN_DTLS_ERR_PIN,
              "a fingerprint mismatch fails the handshake with the PIN "
              "class (never ready, never silent)");
        CHECK(kn_dtls_ready(p.cli) == 0 &&
              kn_dtls_poll(p.cli) == KN_DTLS_ERR_PIN,
              "the fatal error LATCHES (later polls repeat it, no rerun)");
        memset(&st, 0, sizeof st);
        kn_dtls_stats_get(p.cli, &st);
        /* the wrong pin dies at the READY gate (the TLS-level handshake
         * completes under VERIFY_OPTIONAL, poll() refuses it): the
         * evidence is the dirty verify flags + the never-set pin. A
         * fatal-error-path rc lives in last_rc; the gate path keeps the
         * verify_result as the artifact. */
        CHECK(st.pin_matched == 0 && st.verify_result != 0,
              "evidence: pin never matched, verify flags preserved dirty");
        pair_free(&p);
    }

    /* ---- inject queue: bounded, drop-new, counted ---- */
    err[0] = '\0';
    rc = pair_setup(&p, 0, 0, 0, err, sizeof err);
    CHECK(rc == 0, "overflow pair arms");
    if (rc == 0) {
        uint8_t rec[8] = { 22, 0xfe, 0xfd, 0, 0, 0, 0, 1 };
        unsigned i;
        int full_seen = 0, ok_count = 0;
        for (i = 0; i < KN_DTLS_QUEUE_SLOTS + 5u; ++i) {
            int irc = kn_dtls_inject(p.cli, rec, sizeof rec);
            if (irc == KN_DTLS_OK) ok_count++;
            else if (irc == KN_DTLS_ERR_FULL) full_seen = 1;
        }
        CHECK(ok_count == KN_DTLS_QUEUE_SLOTS && full_seen == 1,
              "inject accepts exactly QUEUE_SLOTS then reports FULL "
              "(drop-new, bounded)");
        CHECK(kn_dtls_inject(p.cli, rec, KN_DTLS_MAX_RECORD + 1) ==
                  KN_DTLS_ERR_ARGS,
              "oversize datagrams refuse (never a real DTLS record)");
        memset(&st, 0, sizeof st);
        kn_dtls_stats_get(p.cli, &st);
        CHECK(st.records_dropped >= 5, "drops are counted honestly");
        /* mid-handshake teardown while records are queued: ASan bait */
        pair_free(&p);
        CHECK(1, "mid-handshake free with a full queue is clean");
    }

    /* ---- NULL-safety of every accessor ---- */
    kn_dtls_stats_get(NULL, &st);
    CHECK(kn_dtls_ready(NULL) == 0 && kn_dtls_version(NULL)[0] == '\0' &&
          kn_dtls_ready_ms(NULL) == 0,
          "accessors are NULL-safe in the real build too");
}
#endif /* KN_TLS_BACKEND_MBEDTLS */

#ifdef KN_TLS_BACKEND_MBEDTLS
/* The cross-implementation leg (tools/dtls_openssl_check.py drives it):
 * the SAME kn_dtls client, pinned to an OpenSSL-generated server
 * certificate, against `openssl s_server -dtls1_2` — an INDEPENDENT
 * DTLS stack must accept our ClientHello (incl. its HelloVerifyRequest
 * cookie round), our ECDSA P-256 identity and our Finished. Usage:
 *   test_kndtls --openssl <port> <colon-hex-sha256-fingerprint>
 * Exit 0 + "openssl-interop: PASS" on success. Sandbox-only (the Win7
 * target has no openssl; its authority is test_kndtls's own battery
 * under T10). */
static int openssl_client_leg(const char *port_s, const char *fp)
{
    char err[256];
    kn_sock_udp sock;
    kn_cert *id;
    kn_dtls *d;
    uint32_t lo = 0;
    uint16_t port;
    uint64_t t_end;
    int rc_out = 1;
    long p = strtol(port_s, NULL, 10);
    if (p < 1 || p > 65535) { printf("bad port\n"); return 2; }
    port = (uint16_t)p;
    if (kn_sock_ipv4_parse("127.0.0.1", &lo) != 0) return 2;
    if (kn_sock_udp_open(&sock, NULL, 0, 262144, err, sizeof err)
        != KN_SOCK_OK) { printf("socket: %s\n", err); return 2; }
    if (kn_sock_udp_connect(&sock, lo, port) != KN_SOCK_OK) {
        printf("connect failed\n"); kn_sock_udp_close(&sock); return 2;
    }
    id = kn_cert_generate(err, sizeof err);
    if (!id) { printf("cert: %s\n", err); kn_sock_udp_close(&sock); return 2; }
    d = kn_dtls_client(&sock, lo, port, id, fp, err, sizeof err);
    if (!d) {
        printf("client: %s\n", err);
        kn_cert_free(id); kn_sock_udp_close(&sock); return 2;
    }
    t_end = kn_sock_now_ms() + 20000u;
    while (kn_sock_now_ms() < t_end) {
        uint8_t buf[KN_DTLS_MAX_RECORD];
        size_t len = 0;
        int n = kn_sock_udp_recv(&sock, buf, sizeof buf, 20u, &len, NULL);
        if (n == 1 && kn_dtls_peek(buf, len))
            (void)kn_dtls_inject(d, buf, len);
        {
        int drc = kn_dtls_poll(d);
        if (drc == KN_DTLS_READY) {
            kn_dtls_stats st;
            char cs[80];
            kn_dtls_stats_get(d, &st);
            (void)kn_dtls_ciphersuite(d, cs, sizeof cs);
            printf("openssl-interop: PASS (version=%s cipher=%.60s "
                   "ready_ms=%llu records_in=%u records_out=%u)\n",
                   kn_dtls_version(d), cs,
                   (unsigned long long)kn_dtls_ready_ms(d),
                   st.records_in, st.records_out);
            rc_out = 0;
            break;
        }
        if (drc < 0) {
            kn_dtls_stats st;
            kn_dtls_stats_get(d, &st);
            printf("openssl-interop: FAIL (handshake rc=%d, "
                   "mbedtls=-0x%04x verify=%d)\n", drc,
                   (unsigned)(st.last_rc < 0 ? -st.last_rc : st.last_rc),
                   st.verify_result);
            break;
        }
        }
    }
    if (rc_out != 0) printf("openssl-interop: FAIL (budget exhausted)\n");
    kn_dtls_free(d);
    kn_cert_free(id);
    kn_sock_udp_close(&sock);
    return rc_out;
}
#endif

int main(int argc, char **argv)
{
#ifdef KN_TLS_BACKEND_MBEDTLS
    if (argc == 4 && strcmp(argv[1], "--openssl") == 0)
        return openssl_client_leg(argv[2], argv[3]);
#else
    (void)argc; (void)argv;
#endif
    {
        int avail = kn_dtls_available();
        CHECK(avail == 0 || avail == 1, "available() is boolean");
        test_peek();
#ifdef KN_TLS_BACKEND_MBEDTLS
        if (avail) test_real_mode();
        else       test_stub_mode();   /* define without backend: impossible,
                                          but the branch keeps the contract
                                          honest if it ever happens */
#else
        test_stub_mode();
#endif
        return kn_test_summary("test_kndtls");
    }
}
