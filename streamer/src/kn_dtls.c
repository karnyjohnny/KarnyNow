/* kn_dtls.c — the DTLS 1.2 ACTIVE client (see kn_dtls.h for the
 * contract + provenance). One file, two complementary sections — the
 * kn_cert/kn_net discipline: exactly one definition set per
 * configuration, honest stubs elsewhere, no silent fallbacks.
 * kn_dtls_peek is the ONE shared function (pure; identical in both
 * builds) so the pump's demux behaves the same everywhere. */
#include "kn/kn_dtls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* looks_like_dtls (nvst.rs:6345): first byte 20..=63 (DTLS ContentType
 * range 20-63; STUN's first byte is 0x00/0x01 and RTP's 128-191, so the
 * classes never collide on a well-formed wire). */
int kn_dtls_peek(const uint8_t *data, size_t len)
{
    return (data && len > 0 && data[0] >= 20u && data[0] <= 63u) ? 1 : 0;
}

#ifdef KN_TLS_BACKEND_MBEDTLS
/* ============================ the real body (portable mbedTLS) ======== */

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>   /* MBEDTLS_ERR_NET_SEND_FAILED constant
                                      * only — the BIO sends through
                                      * kn_sock, never mbedtls_net_* */
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

/* Handshake flights stay off IP fragmentation (str0m fragments around
 * its DATAGRAM_MTU target the same way; a P-256 certificate flight is
 * far below this, the bound is belt-and-braces for bigger chains). */
#define KN_DTLS_MTU 1200

typedef struct {
    uint64_t t0;          /* arm time (kn_sock_now_ms) */
    uint32_t inter_ms;    /* intermediate delay */
    uint32_t fin_ms;      /* final delay (0 = cancelled) */
    int      running;
} kn_dtls_timer;

struct kn_dtls {
    mbedtls_ssl_context       ssl;
    mbedtls_ssl_config        conf;
    mbedtls_entropy_context   ent;
    mbedtls_ctr_drbg_context  drbg;
    kn_dtls_timer             timer;

    kn_sock_udp *sock;        /* borrowed — NEVER closed here */
    uint32_t     dest_ip;
    uint16_t     dest_port;

    uint8_t pin[32];          /* the DESCRIBE fingerprint, binary */

    /* inject queue (bounded ring; the pump thread is the only writer
     * AND the only poller — no locking needed by construction) */
    uint8_t  q[KN_DTLS_QUEUE_SLOTS][KN_DTLS_MAX_RECORD];
    uint16_t qlen[KN_DTLS_QUEUE_SLOTS];
    unsigned q_head, q_count;

    /* state */
    int      ready;
    int      pin_matched;     /* verify cb saw SHA-256(der) == pin */
    int      fatal;           /* latched KN_DTLS_ERR_* (refuses polls) */
    uint64_t t_create, t_ready;

    kn_dtls_stats st;
};

int kn_dtls_available(void) { return 1; }

static void kn_dtls_fail(char *err, size_t errsz, const char *what, int ret)
{
    if (err && errsz) {
        char why[128];
        mbedtls_strerror(ret, why, sizeof why);
        snprintf(err, errsz, "%s (-0x%04x: %.80s)", what,
                 (unsigned)(ret < 0 ? -ret : ret), why);
    }
}

/* "AA:BB:…" (case-insensitive) with an optional "sha-256 " algorithm
 * prefix — parse_nvst_fingerprint semantics (nvst.rs:5640-5647: a value
 * without a space gets "sha-256 " prepended, then the SDP form parses).
 * Trailing whitespace tolerated. Returns 0 / -1. */
static int dtls_fp_parse(const char *s, uint8_t out[32])
{
    size_t i;
    const char *p;
    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    p = strrchr(s, ' ');            /* algorithm prefix, when present */
    if (p) s = p + 1;
    for (i = 0; i < 32; ++i) {
        int hi, lo;
        char c1 = s[0], c2 = s[1];
        if (!c1 || !c2) return -1;
        hi = (c1 >= '0' && c1 <= '9') ? c1 - '0'
           : (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10
           : (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10 : -1;
        lo = (c2 >= '0' && c2 <= '9') ? c2 - '0'
           : (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10
           : (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i + 1 < 32) {
            if (*s != ':') return -1;
            s++;
        }
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return *s == '\0' ? 0 : -1;
}

/* ---- the pin IS the policy: accept the peer iff SHA-256(DER) == pin.
 * Chain flags (BADCERT_NOT_TRUSTED etc. — always set for a self-signed
 * peer with no CA) are cleared EXCLUSIVELY on an exact match at depth 0;
 * a mismatch leaves them set, VERIFY_REQUIRED then fails the handshake
 * and poll() latches KN_DTLS_ERR_PIN. */
static int dtls_verify(void *p, mbedtls_x509_crt *crt, int depth,
                       uint32_t *flags)
{
    kn_dtls *d = (kn_dtls *)p;
    uint8_t hash[32];
    if (depth != 0 || !crt || !crt->raw.p || crt->raw.len == 0)
        return 0;                       /* let the chain flags stand */
    if (mbedtls_sha256(crt->raw.p, crt->raw.len, hash, 0) != 0)
        return 0;
    if (memcmp(hash, d->pin, 32) == 0) {
        *flags = 0;                     /* pinned: trusted by definition */
        d->pin_matched = 1;
    }
    /* mismatch: flags stay non-zero -> VERIFY_REQUIRED aborts below */
    return 0;
}

/* ---- BIO: send straight to the (connected) socket; recv pops the
 * inject queue and NEVER blocks — an empty queue is an honest TIMEOUT
 * and the pump's next poll drives retransmission through the timer. */
static int dtls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    kn_dtls *d = (kn_dtls *)ctx;
    if (kn_sock_udp_sendto(d->sock, d->dest_ip, d->dest_port, buf, len)
        != KN_SOCK_OK) {
        d->st.send_errors++;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    d->st.records_out++;
    return (int)len;
}

/* An empty queue is WANT_READ, never TIMEOUT: fetch_input treats a
 * TIMEOUT from the BIO as an expired retransmission delay and calls
 * mbedtls_ssl_resend() (ssl_msg.c:2256-2272), which dereferences a NULL
 * flight before the first flight is buffered. WANT_READ just yields back
 * to the pump, and retransmission stays driven by the timer callbacks. */
static int dtls_bio_recv(void *ctx, unsigned char *buf, size_t len,
                         uint32_t timeout)
{
    kn_dtls *d = (kn_dtls *)ctx;
    size_t n;
    (void)timeout;              /* the queue is memory: never wait */
    if (d->q_count == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    n = d->qlen[d->q_head];
    if (n > len) {
        /* cannot happen with a default-sized mbedTLS input buffer
         * (>= 16 KiB) and our 1600-byte slots; a truncated DTLS record
         * is worse than a lost one (the protocol retransmits), so drop
         * it honestly. */
        d->q_head = (d->q_head + 1u) % KN_DTLS_QUEUE_SLOTS;
        d->q_count--;
        d->st.records_dropped++;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    memcpy(buf, d->q[d->q_head], n);
    d->q_head = (d->q_head + 1u) % KN_DTLS_QUEUE_SLOTS;
    d->q_count--;
    return (int)n;
}

/* ---- retransmission timers on the kn_sock monotonic ms clock
 * (contract: -1 cancelled, 0 nothing passed, 1 intermediate passed,
 * 2 final passed — mbedtls/ssl.h mbedtls_ssl_get_timer_t). */
static void dtls_timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    kn_dtls *d = (kn_dtls *)ctx;
    if (fin_ms == 0) { d->timer.running = 0; return; }
    d->timer.t0 = kn_sock_now_ms();
    d->timer.inter_ms = int_ms;
    d->timer.fin_ms = fin_ms;
    d->timer.running = 1;
}

static int dtls_timer_get(void *ctx)
{
    kn_dtls *d = (kn_dtls *)ctx;
    uint64_t elapsed;
    if (!d->timer.running) return -1;
    elapsed = kn_sock_now_ms() - d->timer.t0;
    if (elapsed >= d->timer.fin_ms) return 2;
    if (elapsed >= d->timer.inter_ms) return 1;
    return 0;
}

/* post-ready drain: the rig's SCTP records are R3-C's business — until
 * then they are counted and discarded, NEVER silently dropped (D-t4). */
static void dtls_drain_app(kn_dtls *d)
{
    unsigned char buf[KN_DTLS_MAX_RECORD];
    for (;;) {
        int n = mbedtls_ssl_read(&d->ssl, buf, sizeof buf);
        if (n > 0) {
            d->st.app_records++;
            d->st.app_bytes += (unsigned)n;
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            d->st.peer_closed = 1;
        break;      /* WANT_READ / TIMEOUT / anything else: not fatal
                     * in R3-B; the next poll retries */
    }
}

kn_dtls *kn_dtls_client(kn_sock_udp *sock, uint32_t dest_ip,
                        uint16_t dest_port, kn_cert *identity,
                        const char *remote_fp, char *err, size_t errsz)
{
    kn_dtls *d = NULL;
    int ret;
    mbedtls_x509_crt *crt;
    mbedtls_pk_context *pk;

    if (err && errsz) err[0] = '\0';
    if (!sock || !sock->is_open || !identity || !remote_fp) {
        if (err && errsz) snprintf(err, errsz, "bad arguments");
        return NULL;
    }
    crt = (mbedtls_x509_crt *)kn_cert_x509(identity);
    pk  = (mbedtls_pk_context *)kn_cert_pk(identity);
    if (!crt || !pk) {
        if (err && errsz) snprintf(err, errsz, "identity has no contexts");
        return NULL;
    }

    d = (kn_dtls *)calloc(1, sizeof *d);
    if (!d) {
        if (err && errsz) snprintf(err, errsz, "alloc failed");
        return NULL;
    }
    if (dtls_fp_parse(remote_fp, d->pin) != 0) {
        if (err && errsz)
            snprintf(err, errsz, "remote fingerprint is not colon-hex");
        kn_dtls_free(d);
        return NULL;
    }
    d->sock = sock;
    d->dest_ip = dest_ip;
    d->dest_port = dest_port;
    d->t_create = kn_sock_now_ms();

    if (psa_crypto_init() != PSA_SUCCESS) {   /* idempotent (kn_cert may
                                               * have run first); ECDSA
                                               * ops need PSA in 3.x */
        if (err && errsz) snprintf(err, errsz, "psa_crypto_init failed");
        kn_dtls_free(d);
        return NULL;
    }
    mbedtls_entropy_init(&d->ent);
    mbedtls_ctr_drbg_init(&d->drbg);
    ret = mbedtls_ctr_drbg_seed(&d->drbg, mbedtls_entropy_func, &d->ent,
                                (const unsigned char *)"KarnyNow R3-B", 13);
    if (ret != 0) {
        kn_dtls_fail(err, errsz, "drbg seed", ret);
        kn_dtls_free(d);
        return NULL;
    }

    mbedtls_ssl_init(&d->ssl);
    mbedtls_ssl_config_init(&d->conf);
    ret = mbedtls_ssl_config_defaults(&d->conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        kn_dtls_fail(err, errsz, "ssl config defaults", ret);
        kn_dtls_free(d);
        return NULL;
    }
    /* DTLS 1.2 exactly — the str0m default (config.rs:775) and the same
     * ceiling the Win7 TLS stream proved (T12: TLS 1.3's PSA path fails
     * on this machine; DTLS 1.2 never touches it). */
    mbedtls_ssl_conf_min_tls_version(&d->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&d->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&d->conf, mbedtls_ctr_drbg_random, &d->drbg);
    /* VERIFY_OPTIONAL, deliberately: mbedTLS treats VERIFY_REQUIRED
     * WITHOUT a CA chain as fatal before any callback runs
     * (ssl_tls.c:8071 "got no CA chain" -> CA_CHAIN_REQUIRED) — and a
     * WebRTC peer is self-signed BY DESIGN, there is no CA to configure.
     * The pinning is enforced in TWO places instead, both mandatory:
     *   1. dtls_verify clears the chain flags EXCLUSIVELY on an exact
     *      SHA-256(DER) match with the pinned fingerprint;
     *   2. kn_dtls_poll refuses the READY verdict unless
     *      verify_result == 0 AND pin_matched == 1 (a completed
     *      handshake with dirty flags latches KN_DTLS_ERR_PIN and a
     *      close_notify goes back to the peer).
     * A forged certificate cannot produce the pinned SHA-256 without
     * the rig's private key, so this is strictly stronger than any CA
     * chain check — the pin IS the policy. */
    mbedtls_ssl_conf_authmode(&d->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&d->conf, dtls_verify, d);
    /* the ANNOUNCE'd identity: the rig pins this certificate by the
     * fingerprint we already sent (start_dtls(true) = client-active,
     * nvst.rs:6300) */
    ret = mbedtls_ssl_conf_own_cert(&d->conf, crt, pk);
    if (ret != 0) {
        kn_dtls_fail(err, errsz, "own cert", ret);
        kn_dtls_free(d);
        return NULL;
    }
    ret = mbedtls_ssl_setup(&d->ssl, &d->conf);
    if (ret != 0) {
        kn_dtls_fail(err, errsz, "ssl setup", ret);
        kn_dtls_free(d);
        return NULL;
    }
    mbedtls_ssl_set_mtu(&d->ssl, KN_DTLS_MTU);
    mbedtls_ssl_set_bio(&d->ssl, d, dtls_bio_send, NULL, dtls_bio_recv);
    mbedtls_ssl_set_timer_cb(&d->ssl, d, dtls_timer_set, dtls_timer_get);
    return d;
}

int kn_dtls_inject(kn_dtls *d, const uint8_t *data, size_t len)
{
    unsigned slot;
    if (!d || !data || len == 0) return KN_DTLS_ERR_ARGS;
    if (len > KN_DTLS_MAX_RECORD) {
        d->st.records_dropped++;
        return KN_DTLS_ERR_ARGS;        /* oversize: never a real record */
    }
    if (d->q_count >= KN_DTLS_QUEUE_SLOTS) {
        d->st.records_dropped++;        /* bounded: drop-new, counted */
        return KN_DTLS_ERR_FULL;
    }
    slot = (d->q_head + d->q_count) % KN_DTLS_QUEUE_SLOTS;
    memcpy(d->q[slot], data, len);
    d->qlen[slot] = (uint16_t)len;
    d->q_count++;
    d->st.records_in++;
    return KN_DTLS_OK;
}

int kn_dtls_poll(kn_dtls *d)
{
    int ret;
    if (!d) return KN_DTLS_ERR_ARGS;
    if (d->fatal) return d->fatal;      /* final errors latch */
    d->st.polls++;
    if (d->ready) {
        dtls_drain_app(d);
        return KN_DTLS_READY;
    }
    ret = mbedtls_ssl_handshake(&d->ssl);
    if (ret == 0) {
        /* The READY gate (see the authmode note in kn_dtls_client):
         * a completed handshake is REFUSED unless the pin matched and
         * the verify flags are clean — with VERIFY_OPTIONAL mbedTLS
         * lets a dirty-flag handshake finish, so the gate lives here.
         * The peer gets an honest close_notify (what a real WebRTC
         * stack does on a fingerprint mismatch). */
        d->st.verify_result = (int)mbedtls_ssl_get_verify_result(&d->ssl);
        if (!d->pin_matched || d->st.verify_result != 0) {
            (void)mbedtls_ssl_close_notify(&d->ssl);   /* best effort */
            d->fatal = KN_DTLS_ERR_PIN;
            return d->fatal;
        }
        d->ready = 1;
        d->t_ready = kn_sock_now_ms();
        dtls_drain_app(d);
        return KN_DTLS_READY;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
        ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
        ret == MBEDTLS_ERR_SSL_TIMEOUT ||
        ret == MBEDTLS_ERR_SSL_RECEIVED_EARLY_DATA)
        return KN_DTLS_OK;              /* in progress — keep pumping */
    d->st.last_rc = ret;
    d->fatal = !d->pin_matched ? KN_DTLS_ERR_PIN
             : ret == MBEDTLS_ERR_NET_SEND_FAILED ? KN_DTLS_ERR_IO
             : KN_DTLS_ERR_HANDSHAKE;
    /* NOTE on the pin latch: any fatal handshake error before the peer
     * certificate was accepted is REPORTED as the pin class when the pin
     * never matched — the honest summary ("we never got past identity")
     * with the raw mbedTLS rc alongside in last_rc for the exact cause.
     * A verified-then-failed handshake keeps HANDSHAKE/IO. */
    return d->fatal;
}

int kn_dtls_ready(const kn_dtls *d) { return (d && d->ready) ? 1 : 0; }

const char *kn_dtls_version(const kn_dtls *d)
{
    if (!d || !d->ready) return "";
    return mbedtls_ssl_get_version(&d->ssl);
}

int kn_dtls_ciphersuite(const kn_dtls *d, char *out, size_t outsz)
{
    const char *cs;
    if (!d || !out || !outsz) return -1;
    out[0] = '\0';
    if (!d->ready) return -1;
    cs = mbedtls_ssl_get_ciphersuite(&d->ssl);
    if (!cs) return -1;
    snprintf(out, outsz, "%s", cs);
    return 0;
}

uint64_t kn_dtls_ready_ms(const kn_dtls *d)
{
    if (!d || !d->ready) return 0;
    return d->t_ready - d->t_create;
}

void kn_dtls_stats_get(const kn_dtls *d, kn_dtls_stats *out)
{
    if (!d || !out) return;
    *out = d->st;
    ((kn_dtls_stats *)out)->pin_matched = d->pin_matched;
}

void kn_dtls_free(kn_dtls *d)
{
    if (!d) return;
    /* calloc-zeroed contexts are safe for every mbedtls_*_free, so the
     * failure paths in kn_dtls_client need no init bookkeeping */
    mbedtls_ssl_free(&d->ssl);
    mbedtls_ssl_config_free(&d->conf);
    mbedtls_ctr_drbg_free(&d->drbg);
    mbedtls_entropy_free(&d->ent);
    /* the socket and the kn_cert are CALLER-owned (header contract) */
    memset(d, 0, sizeof *d);
    free(d);
}

#else
/* ============================ the honest stub ========================
 * Complementary guard: any build WITHOUT the mbedTLS backend gets this
 * section — available() == 0, client() refuses loudly, and knvstd keeps
 * the pump STUN/NATT-only with the evidence line saying dtls armed:0
 * unavailable:1. No plaintext DTLS look-alike, no silence. */

struct kn_dtls { int unused; };

int kn_dtls_available(void) { return 0; }

kn_dtls *kn_dtls_client(kn_sock_udp *sock, uint32_t dest_ip,
                        uint16_t dest_port, kn_cert *identity,
                        const char *remote_fp, char *err, size_t errsz)
{
    (void)sock; (void)dest_ip; (void)dest_port; (void)identity;
    (void)remote_fp;
    if (err && errsz)
        snprintf(err, errsz,
                 "kn_dtls unavailable (built without the mbedTLS backend)");
    return NULL;
}

int kn_dtls_inject(kn_dtls *d, const uint8_t *data, size_t len)
{
    (void)d; (void)data; (void)len;
    return KN_DTLS_ERR_STATE;
}

int kn_dtls_poll(kn_dtls *d) { (void)d; return KN_DTLS_ERR_STATE; }
int kn_dtls_ready(const kn_dtls *d) { (void)d; return 0; }
const char *kn_dtls_version(const kn_dtls *d) { (void)d; return ""; }

int kn_dtls_ciphersuite(const kn_dtls *d, char *out, size_t outsz)
{
    (void)d;
    if (out && outsz) out[0] = '\0';
    return -1;
}

uint64_t kn_dtls_ready_ms(const kn_dtls *d) { (void)d; return 0; }

void kn_dtls_stats_get(const kn_dtls *d, kn_dtls_stats *out)
{
    (void)d;
    if (out) memset(out, 0, sizeof *out);
}

void kn_dtls_free(kn_dtls *d) { (void)d; }

#endif /* KN_TLS_BACKEND_MBEDTLS */
