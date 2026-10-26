/* kn_cert.h — the self-signed DTLS identity (R3-A, docs/DTLS.md).
 *
 * Portable mbedTLS: an ECDSA P-256 keypair + a self-signed X.509
 * certificate whose SHA-256 fingerprint is rendered in the announced
 * colon-uppercase-hex form (nvst_fingerprint_hex, nvst.rs:5596-5603;
 * str0m computes the fingerprint as the SHA-256 of the DER —
 * vendor/str0m src/lib.rs:1662; its provider default is ECDSA P-256,
 * matching this module). This replaces deviation D-s1's random
 * placeholder fingerprint with a REAL certificate — the one whose
 * private key R3-B's DTLS handshake will use on the bundle socket.
 *
 * SECURITY (§7): the private key never leaves this module and is never
 * logged; the fingerprint is public by construction (it travels in the
 * ANNOUNCE SDP).
 *
 * Build discipline (the kn_net TLS-stream pattern): the real body
 * compiles ONLY with KN_TLS_BACKEND_MBEDTLS (the CMake mbedtls backend
 * defines it and links mbedcrypto/mbedx509 on BOTH platforms, so the
 * sandbox ctest exercises the real code, not a stub); every other
 * configuration gets the honest stub below — available() == 0 and a
 * loud generate() refusal, and knvstd falls back to the D-s1
 * placeholder identity with the evidence line saying so. Exactly one
 * definition set per configuration via complementary guards.
 */
#ifndef KN_CERT_H
#define KN_CERT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "AA:BB:…" — 32 bytes as 2-digit uppercase hex joined by 31 colons. */
#define KN_CERT_FP_LEN 95

typedef struct kn_cert kn_cert;   /* opaque; heap-owned like kn_net_tls_stream */

/* 1 when this build carries the real mbedTLS body, 0 for the stub. */
int kn_cert_available(void);

/* Generate a fresh identity (CSPRNG keygen). Returns NULL on any
 * failure (err/errsz get a one-line reason with the mbedTLS code).
 * The validity window is a FIXED wide span (2020-2035) — the module
 * has no wall-clock dependency and the certificate is per-session
 * ephemeral anyway (the rig pins it by fingerprint, not by CA). */
kn_cert *kn_cert_generate(char *err, size_t errsz);

void kn_cert_free(kn_cert *c);      /* free(NULL)-safe (NOT double-free —
                                       the kn_net_tls_stream_close
                                       convention); scrubs the key */

/* The announced fingerprint form. Returns 0 / -1; out needs
 * KN_CERT_FP_LEN + 1 bytes. */
int kn_cert_fingerprint(const kn_cert *c, char *out, size_t outsz);

/* The exact DER the fingerprint was computed over (borrowed; valid
 * until kn_cert_free). */
const uint8_t *kn_cert_der(const kn_cert *c, size_t *out_len);

/* Opaque handles for R3-B (they ARE mbedtls_pk_context* and
 * mbedtls_x509_crt* — the header stays mbedTLS-free on purpose; the
 * DTLS module casts them back with its own mbedTLS includes). */
void *kn_cert_pk(kn_cert *c);
void *kn_cert_x509(kn_cert *c);

/* Internal-consistency KAT used by test_kncert (keeps the test
 * mbedTLS-free): re-parse the stored DER, recompute the SHA-256
 * fingerprint, and do an ECDSA sign/verify roundtrip with the key.
 * Returns 0 when everything agrees, negative otherwise. */
int kn_cert_selfcheck(const kn_cert *c);

#ifdef __cplusplus
}
#endif

#endif /* KN_CERT_H */
