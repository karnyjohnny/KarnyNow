/* kn_aes.c — AES-128 block encrypt via vendored tiny-AES-c (ECB core).
 * See kn_aes.h for the contract and third_party/tiny-aes-c/PROVENANCE.md
 * for licensing/provenance. This is the ONLY file that includes aes.h. */
#include "kn/kn_aes.h"

#include <assert.h>
#include <string.h>

#include "aes.h"   /* third_party/tiny-aes-c (include path set by the build) */

/* The opaque storage must fit the backend context in the compiled
 * configuration (default: ECB+CBC+CTR on -> struct has RoundKey[176]+Iv[16]). */
static_assert(sizeof(struct AES_ctx) <= sizeof(kn_aes128_ctx),
              "kn_aes128_ctx.opaque too small for struct AES_ctx");

void kn_aes128_init(kn_aes128_ctx *ctx, const uint8_t key[KN_AES128_KEY_LEN])
{
    if (!ctx || !key) return;
    AES_init_ctx((struct AES_ctx *)(void *)ctx->opaque, key);
}

void kn_aes128_encrypt(const kn_aes128_ctx *ctx,
                       const uint8_t in[KN_AES_BLOCK_LEN],
                       uint8_t out[KN_AES_BLOCK_LEN])
{
    if (!ctx || !in || !out) return;
    if (out != in) memcpy(out, in, KN_AES_BLOCK_LEN);
    /* tiny-AES ECB encrypts in place; its ctx pointer is const in this API */
    AES_ECB_encrypt((const struct AES_ctx *)(const void *)ctx->opaque, out);
}
