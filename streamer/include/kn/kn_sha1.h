/* kn_sha1.h — SHA-1 + HMAC-SHA1 (RFC 2104) for SRTP authentication.
 *
 * Backend: vendored Brad Conte SHA-1 (third_party/crypto-algorithms-sha1,
 * public domain; provenance + sha256 in its PROVENANCE.md), verified at
 * runtime against RFC 3174 §7.3 vectors. The HMAC construction is standard
 * RFC 2104 glue over that vetted primitive, verified against RFC 2202 cases
 * 1-7. Used ONLY as the SRTP/SRTCP AES_CM_128_HMAC_SHA1_{32,80} MAC — never
 * for signatures or password hashing (SHA-1 is collision-broken; HMAC-SHA1
 * remains the RFC-3711-mandated PRF for these profiles).
 *
 * Replacement path: mbedTLS backend at stage 2 behind this same API.
 * Zero allocation; thread-safe (contexts are caller-owned).
 */
#ifndef KN_SHA1_H
#define KN_SHA1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_SHA1_DIGEST_LEN 20
#define KN_SHA1_BLOCK_LEN  64   /* HMAC block size for SHA-1 (RFC 2104) */

/* Opaque streaming context; sized for the backend SHA1_CTX (112-120 bytes),
 * compile-time asserted in kn_sha1.c. */
typedef struct kn_sha1_ctx {
    uint8_t opaque[128];
} kn_sha1_ctx;

void kn_sha1_init(kn_sha1_ctx *ctx);
void kn_sha1_update(kn_sha1_ctx *ctx, const void *data, size_t len);
void kn_sha1_final(kn_sha1_ctx *ctx, uint8_t out[KN_SHA1_DIGEST_LEN]);

/* One-shot. */
void kn_sha1(const void *data, size_t len, uint8_t out[KN_SHA1_DIGEST_LEN]);

/* RFC 2104 HMAC-SHA1. Keys longer than 64 bytes are hashed first (per RFC). */
void kn_hmac_sha1(const uint8_t *key, size_t key_len,
                  const void *data, size_t data_len,
                  uint8_t out[KN_SHA1_DIGEST_LEN]);

/* Incremental HMAC (avoids copying for the SRTP MAC, which is
 * packet-bytes || be32(roc) — two updates). */
typedef struct kn_hmac_sha1_ctx {
    kn_sha1_ctx inner;
    uint8_t     opad_key[KN_SHA1_BLOCK_LEN]; /* key XOR 0x5c, block-padded */
} kn_hmac_sha1_ctx;

void kn_hmac_sha1_init(kn_hmac_sha1_ctx *ctx, const uint8_t *key, size_t key_len);
void kn_hmac_sha1_update(kn_hmac_sha1_ctx *ctx, const void *data, size_t data_len);
void kn_hmac_sha1_final(kn_hmac_sha1_ctx *ctx, uint8_t out[KN_SHA1_DIGEST_LEN]);

/* Constant-time equality: returns 1 if equal, 0 otherwise. No early exit —
 * tag comparison must not leak position of the first difference. */
int kn_ct_memeq(const void *a, const void *b, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KN_SHA1_H */
