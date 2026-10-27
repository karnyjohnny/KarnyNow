/* test_kncert.c — the FOURTEENTH suite: kn_cert (R3-A, docs/DTLS.md).
 * Self-adaptive by design: the SAME binary runs the real-KAT battery
 * under the CMake mbedtls backend (both platforms link the real
 * mbedcrypto/mbedx509) and the honest-stub contract in every other
 * build (the shell gates compile kn_cert.c without the backend define).
 * Either path is a full pass — the mode itself is a reported check.
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_cert.h"
#include "test_port.h"

#include <string.h>

static int fp_format_ok(const char *fp)
{
    size_t i;
    if (strlen(fp) != KN_CERT_FP_LEN) return 0;
    for (i = 0; i < KN_CERT_FP_LEN; ++i) {
        char c = fp[i];
        if (i % 3 == 2) {
            if (c != ':') return 0;
        } else {
            int hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
            if (!hex) return 0;
        }
    }
    return 1;
}

static void test_stub_mode(void)
{
    char err[128];
    kn_cert *c;
    char fp[KN_CERT_FP_LEN + 1];
    size_t dl = 12345;

    printf("-- kn_cert stub contract --\n");
    err[0] = '\0';
    c = kn_cert_generate(err, sizeof err);
    CHECK(c == NULL && err[0] != '\0',
          "stub: generate refuses loudly with a reason");
    CHECK(kn_cert_fingerprint(NULL, fp, sizeof fp) == -1 &&
          kn_cert_der(NULL, &dl) == NULL && dl == 0 &&
          kn_cert_pk(NULL) == NULL && kn_cert_x509(NULL) == NULL &&
          kn_cert_selfcheck(NULL) == -1,
          "stub: every accessor is NULL-safe and negative");
    kn_cert_free(NULL);             /* idempotent no-crash */
    CHECK(1, "stub: free(NULL) is safe");
}

static void test_real_mode(void)
{
    char err[128], fp[KN_CERT_FP_LEN + 1], fp2[KN_CERT_FP_LEN + 1];
    kn_cert *c, *c2;
    const uint8_t *der;
    size_t der_len = 0;

    printf("-- kn_cert real battery (mbedTLS) --\n");
    err[0] = '\0';
    c = kn_cert_generate(err, sizeof err);
    if (!c) printf("generate error: %s\n", err);
    CHECK(c != NULL, "generate succeeds");
    if (!c) return;                  /* the suite never aborts loudly */

    CHECK(kn_cert_fingerprint(c, fp, sizeof fp) == 0 && fp_format_ok(fp),
          "fingerprint is 95 chars of colon-joined uppercase hex "
          "(the announced form, nvst.rs:5596-5603)");
    CHECK(kn_cert_fingerprint(c, fp2, 10) == -1,
          "fingerprint refuses an undersized buffer");

    der = kn_cert_der(c, &der_len);
    CHECK(der != NULL && der_len > 100 && der_len < 2048 &&
          der[0] == 0x30,
          "DER present, plausibly sized, starts with a SEQUENCE tag");

    CHECK(kn_cert_selfcheck(c) == 0,
          "selfcheck: SHA-256(der) == fingerprint, DER re-parses "
          "byte-identical, and the keypair signs/verifies through the "
          "certificate's public key");

    CHECK(kn_cert_pk(c) != NULL && kn_cert_x509(c) != NULL,
          "R3-B handles exposed (pk + x509)");

    c2 = kn_cert_generate(err, sizeof err);
    CHECK(c2 != NULL, "a second identity generates");
    if (c2) {
        CHECK(kn_cert_fingerprint(c2, fp2, sizeof fp2) == 0 &&
              strcmp(fp, fp2) != 0,
              "fresh CSPRNG keypair per generate (fingerprints differ)");
        kn_cert_free(c2);
    }

    /* the free path is exercised under ASan/LSan in the gate runs;
     * the contract is free(NULL)-safe, NOT double-free-safe (the
     * kn_net_tls_stream_close convention: the caller NULLs its copy) */
    kn_cert_free(c);
    kn_cert_free(NULL);
    CHECK(1, "free(NULL) is safe");
}

int main(void)
{
    int avail = kn_cert_available();
    CHECK(avail == 0 || avail == 1, "available() is boolean");
    if (avail)
        test_real_mode();
    else
        test_stub_mode();
    return kn_test_summary("test_kncert");
}
