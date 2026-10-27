/* kn_aes.h — minimal vetted AES-128 block-encrypt API.
 *
 * Backend: vendored tiny-AES-c (third_party/tiny-aes-c, Unlicense; see its
 * PROVENANCE.md). When mbedTLS arrives at stage 2, only kn_aes.c changes —
 * every KAT vector (test_knsrtp) must pass identically across backends.
 *
 * Encryption only: SRTP AES-CM keystream, the RFC 3711 KDF, and AES-GCM's
 * counter blocks all need E(k, block); nothing in the client needs decryption
 * of raw blocks (GCM decrypt = CTR encrypt + GHASH).
 *
 * Thread safety: a kn_aes128_ctx belongs to one caller/thread; the functions
 * themselves are pure over caller memory.
 */
#ifndef KN_AES_H
#define KN_AES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_AES_BLOCK_LEN 16
#define KN_AES128_KEY_LEN 16

/* Opaque context. Sized for tiny-AES-c's default struct AES_ctx
 * (RoundKey[176] + Iv[16] = 192); compile-time asserted in kn_aes.c. */
typedef struct kn_aes128_ctx {
    uint8_t opaque[192];
} kn_aes128_ctx;

void kn_aes128_init(kn_aes128_ctx *ctx, const uint8_t key[KN_AES128_KEY_LEN]);

/* out = AES-128-ECB-Encrypt(key, in). in and out may alias. */
void kn_aes128_encrypt(const kn_aes128_ctx *ctx,
                       const uint8_t in[KN_AES_BLOCK_LEN],
                       uint8_t out[KN_AES_BLOCK_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* KN_AES_H */
