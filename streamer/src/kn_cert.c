/* kn_cert.c — the self-signed DTLS identity (see kn_cert.h for the
 * contract + provenance). One file, two complementary sections — the
 * kn_net/kn_net_mbedtls discipline: exactly one definition set per
 * configuration, honest stubs elsewhere, no silent fallbacks. */
#include "kn/kn_cert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef KN_TLS_BACKEND_MBEDTLS
/* ============================ the real body (portable mbedTLS) ======== */

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>   /* 3.6: parse AND write APIs live here */
#include <psa/crypto.h>

struct kn_cert {
    mbedtls_pk_context pk;        /* the ECDSA P-256 keypair (private) */
    mbedtls_x509_crt   crt;       /* our DER parsed back (R3-B own_cert) */
    uint8_t           *der;       /* exact DER the fingerprint covers */
    size_t             der_len;
    uint8_t            fp[32];    /* SHA-256(der) */
    int                pk_init, crt_init;
};

/* the fixed validity window (see kn_cert.h — no wall-clock dependency;
 * the rig pins the certificate by fingerprint, never by CA/date) */
#define KN_CERT_NOT_BEFORE "20200101000000"
#define KN_CERT_NOT_AFTER  "20351231235959"
#define KN_CERT_SUBJECT    "CN=KarnyNow"
#define KN_CERT_DER_CAP    4096u

int kn_cert_available(void) { return 1; }

static void kn_cert_fail(char *err, size_t errsz, const char *what, int ret)
{
    if (err && errsz) {
        char why[128];
        mbedtls_strerror(ret, why, sizeof why);
        snprintf(err, errsz, "%s (-0x%04x: %.80s)", what,
                 (unsigned)(ret < 0 ? -ret : ret), why);
    }
}

kn_cert *kn_cert_generate(char *err, size_t errsz)
{
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509write_cert wc;
    unsigned char scratch[KN_CERT_DER_CAP];
    unsigned char serial[16];
    kn_cert *c = NULL;
    int ret, seeded = 0;
    long der_len;

    if (err && errsz) err[0] = '\0';
    if (psa_crypto_init() != PSA_SUCCESS) {   /* ECDSA sign needs PSA (3.x) */
        if (err && errsz) snprintf(err, errsz, "psa_crypto_init failed");
        return NULL;
    }
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509write_crt_init(&wc);
    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                                (const unsigned char *)"KarnyNow R3-A", 13);
    if (ret != 0) { kn_cert_fail(err, errsz, "drbg seed", ret); goto done; }
    seeded = 1;

    c = (kn_cert *)calloc(1, sizeof *c);
    if (!c) {
        if (err && errsz) snprintf(err, errsz, "alloc failed");
        goto done;
    }
    mbedtls_pk_init(&c->pk);
    c->pk_init = 1;
    mbedtls_x509_crt_init(&c->crt);
    c->crt_init = 1;

    /* ECDSA P-256 — the str0m provider default the reference announces */
    ret = mbedtls_pk_setup(&c->pk,
                           mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) { kn_cert_fail(err, errsz, "pk setup", ret); goto fail; }
    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(c->pk),
                              mbedtls_ctr_drbg_random, &drbg);
    if (ret != 0) { kn_cert_fail(err, errsz, "ecp gen_key", ret); goto fail; }

    ret = mbedtls_ctr_drbg_random(&drbg, serial, sizeof serial);
    if (ret != 0) { kn_cert_fail(err, errsz, "serial rng", ret); goto fail; }
    serial[0] &= 0x7F;                        /* keep the serial positive */

    mbedtls_x509write_crt_set_version(&wc, MBEDTLS_X509_CRT_VERSION_3);
    ret = mbedtls_x509write_crt_set_serial_raw(&wc, serial, sizeof serial);
    if (ret != 0) { kn_cert_fail(err, errsz, "serial", ret); goto fail; }
    ret = mbedtls_x509write_crt_set_validity(&wc, KN_CERT_NOT_BEFORE,
                                             KN_CERT_NOT_AFTER);
    if (ret != 0) { kn_cert_fail(err, errsz, "validity", ret); goto fail; }
    ret = mbedtls_x509write_crt_set_issuer_name(&wc, KN_CERT_SUBJECT);
    if (ret != 0) { kn_cert_fail(err, errsz, "issuer", ret); goto fail; }
    ret = mbedtls_x509write_crt_set_subject_name(&wc, KN_CERT_SUBJECT);
    if (ret != 0) { kn_cert_fail(err, errsz, "subject", ret); goto fail; }
    mbedtls_x509write_crt_set_subject_key(&wc, &c->pk);
    mbedtls_x509write_crt_set_issuer_key(&wc, &c->pk);
    mbedtls_x509write_crt_set_md_alg(&wc, MBEDTLS_MD_SHA256);

    /* mbedTLS writes DER from the END of the buffer backwards */
    der_len = mbedtls_x509write_crt_der(&wc, scratch, sizeof scratch,
                                        mbedtls_ctr_drbg_random, &drbg);
    if (der_len <= 0) {
        kn_cert_fail(err, errsz, "crt der", (int)der_len);
        goto fail;
    }
    c->der_len = (size_t)der_len;
    c->der = (uint8_t *)malloc(c->der_len);
    if (!c->der) {
        if (err && errsz) snprintf(err, errsz, "der alloc failed");
        goto fail;
    }
    memcpy(c->der, scratch + sizeof scratch - c->der_len, c->der_len);
    memset(scratch, 0, sizeof scratch);

    ret = mbedtls_sha256(c->der, c->der_len, c->fp, 0);
    if (ret != 0) { kn_cert_fail(err, errsz, "sha256", ret); goto fail; }

    /* parse our own DER back — R3-B hands this crt to
     * mbedtls_ssl_conf_own_cert, and the selfcheck re-verifies it */
    ret = mbedtls_x509_crt_parse(&c->crt, c->der, c->der_len);
    if (ret != 0) { kn_cert_fail(err, errsz, "crt parse", ret); goto fail; }

done:
    mbedtls_x509write_crt_free(&wc);
    if (seeded) mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return c;

fail:
    mbedtls_x509write_crt_free(&wc);
    if (seeded) mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    kn_cert_free(c);
    return NULL;
}

void kn_cert_free(kn_cert *c)
{
    if (!c) return;
    if (c->crt_init) mbedtls_x509_crt_free(&c->crt);
    if (c->pk_init) mbedtls_pk_free(&c->pk);   /* zeroizes the private key */
    if (c->der) {
        memset(c->der, 0, c->der_len);
        free(c->der);
    }
    memset(c, 0, sizeof *c);
    free(c);
}

int kn_cert_fingerprint(const kn_cert *c, char *out, size_t outsz)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t i, o = 0;
    if (!c || !out || outsz < KN_CERT_FP_LEN + 1) return -1;
    for (i = 0; i < 32; ++i) {
        if (i) out[o++] = ':';
        out[o++] = hx[c->fp[i] >> 4];
        out[o++] = hx[c->fp[i] & 0x0Fu];
    }
    out[o] = '\0';
    return 0;
}

const uint8_t *kn_cert_der(const kn_cert *c, size_t *out_len)
{
    if (!c) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = c->der_len;
    return c->der;
}

void *kn_cert_pk(kn_cert *c)   { return c ? (void *)&c->pk : NULL; }
void *kn_cert_x509(kn_cert *c) { return c ? (void *)&c->crt : NULL; }

int kn_cert_selfcheck(const kn_cert *c)
{
    mbedtls_x509_crt fresh;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    uint8_t hash[32], sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0;
    int ret;

    if (!c || !c->der || c->der_len == 0) return -1;
    /* 1. the fingerprint IS the SHA-256 of the stored DER */
    ret = mbedtls_sha256(c->der, c->der_len, hash, 0);
    if (ret != 0 || memcmp(hash, c->fp, 32) != 0) return -2;
    /* 2. the DER re-parses standalone and carries the same raw bytes */
    mbedtls_x509_crt_init(&fresh);
    ret = mbedtls_x509_crt_parse(&fresh, c->der, c->der_len);
    if (ret != 0 || fresh.raw.len != c->der_len ||
        memcmp(fresh.raw.p, c->der, c->der_len) != 0) {
        mbedtls_x509_crt_free(&fresh);
        return -3;
    }
    /* 3. the keypair signs and the CERTIFICATE's public key verifies
     *    (proves the announced fingerprint binds OUR private key) */
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                                (const unsigned char *)"KarnyNow R3-A", 13);
    if (ret == 0)
        ret = mbedtls_pk_sign((mbedtls_pk_context *)&c->pk,
                              MBEDTLS_MD_SHA256, hash, sizeof hash,
                              sig, sizeof sig, &sig_len,
                              mbedtls_ctr_drbg_random, &drbg);
    if (ret == 0)
        ret = mbedtls_pk_verify(&fresh.pk, MBEDTLS_MD_SHA256, hash,
                                sizeof hash, sig, sig_len);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    mbedtls_x509_crt_free(&fresh);
    return ret == 0 ? 0 : -4;
}

#else
/* ============================ the honest stub ========================
 * Complementary guard: any build WITHOUT the mbedTLS backend gets this
 * section — available() == 0, generate() refuses loudly, and knvstd
 * keeps the D-s1 placeholder identity (its evidence line says
 * "placeholder"). No plaintext, no fake certificate, no silence. */

struct kn_cert { int unused; };

int kn_cert_available(void) { return 0; }

kn_cert *kn_cert_generate(char *err, size_t errsz)
{
    if (err && errsz)
        snprintf(err, errsz,
                 "kn_cert unavailable (built without the mbedTLS backend)");
    return NULL;
}

void kn_cert_free(kn_cert *c) { (void)c; }

int kn_cert_fingerprint(const kn_cert *c, char *out, size_t outsz)
{
    (void)c; (void)out; (void)outsz;
    return -1;
}

const uint8_t *kn_cert_der(const kn_cert *c, size_t *out_len)
{
    (void)c;
    if (out_len) *out_len = 0;
    return NULL;
}

void *kn_cert_pk(kn_cert *c)   { (void)c; return NULL; }
void *kn_cert_x509(kn_cert *c) { (void)c; return NULL; }
int kn_cert_selfcheck(const kn_cert *c) { (void)c; return -1; }

#endif /* KN_TLS_BACKEND_MBEDTLS */
