/* kn_sha1.c — SHA-1/HMAC-SHA1 wrapper over vendored Brad Conte SHA-1.
 * Contract in kn_sha1.h; provenance in third_party/crypto-algorithms-sha1/. */
#include "kn/kn_sha1.h"

#include <assert.h>
#include <string.h>

#include "sha1.h"   /* third_party/crypto-algorithms-sha1 (build include path) */

static_assert(sizeof(SHA1_CTX) <= sizeof(kn_sha1_ctx),
              "kn_sha1_ctx.opaque too small for SHA1_CTX");

void kn_sha1_init(kn_sha1_ctx *ctx)
{
    if (!ctx) return;
    sha1_init((SHA1_CTX *)(void *)ctx->opaque);
}

void kn_sha1_update(kn_sha1_ctx *ctx, const void *data, size_t len)
{
    if (!ctx || (!data && len)) return;
    sha1_update((SHA1_CTX *)(void *)ctx->opaque, (const BYTE *)data, len);
}

void kn_sha1_final(kn_sha1_ctx *ctx, uint8_t out[KN_SHA1_DIGEST_LEN])
{
    if (!ctx || !out) return;
    sha1_final((SHA1_CTX *)(void *)ctx->opaque, out);
}

void kn_sha1(const void *data, size_t len, uint8_t out[KN_SHA1_DIGEST_LEN])
{
    kn_sha1_ctx ctx;
    kn_sha1_init(&ctx);
    kn_sha1_update(&ctx, data, len);
    kn_sha1_final(&ctx, out);
}

/* ------------------------------------------------------------ HMAC-SHA1 */

static void hmac_prepare_key(const uint8_t *key, size_t key_len,
                             uint8_t block_key[KN_SHA1_BLOCK_LEN])
{
    memset(block_key, 0, KN_SHA1_BLOCK_LEN);
    if (key_len > KN_SHA1_BLOCK_LEN) {
        /* RFC 2104: hash keys longer than the block size first */
        kn_sha1(key, key_len, block_key);          /* 20 bytes + zero pad */
    } else if (key && key_len) {
        memcpy(block_key, key, key_len);
    }
}

void kn_hmac_sha1_init(kn_hmac_sha1_ctx *ctx, const uint8_t *key, size_t key_len)
{
    uint8_t bk[KN_SHA1_BLOCK_LEN];
    uint8_t ipad[KN_SHA1_BLOCK_LEN];
    size_t i;
    if (!ctx) return;
    hmac_prepare_key(key, key_len, bk);
    for (i = 0; i < KN_SHA1_BLOCK_LEN; ++i) {
        ipad[i] = (uint8_t)(bk[i] ^ 0x36u);
        ctx->opad_key[i] = (uint8_t)(bk[i] ^ 0x5cu);
    }
    kn_sha1_init(&ctx->inner);
    kn_sha1_update(&ctx->inner, ipad, KN_SHA1_BLOCK_LEN);
}

void kn_hmac_sha1_update(kn_hmac_sha1_ctx *ctx, const void *data, size_t data_len)
{
    if (!ctx) return;
    kn_sha1_update(&ctx->inner, data, data_len);
}

void kn_hmac_sha1_final(kn_hmac_sha1_ctx *ctx, uint8_t out[KN_SHA1_DIGEST_LEN])
{
    uint8_t inner[KN_SHA1_DIGEST_LEN];
    kn_sha1_ctx outer;
    if (!ctx || !out) return;
    kn_sha1_final(&ctx->inner, inner);
    kn_sha1_init(&outer);
    kn_sha1_update(&outer, ctx->opad_key, KN_SHA1_BLOCK_LEN);
    kn_sha1_update(&outer, inner, KN_SHA1_DIGEST_LEN);
    kn_sha1_final(&outer, out);
}

void kn_hmac_sha1(const uint8_t *key, size_t key_len,
                  const void *data, size_t data_len,
                  uint8_t out[KN_SHA1_DIGEST_LEN])
{
    kn_hmac_sha1_ctx ctx;
    kn_hmac_sha1_init(&ctx, key, key_len);
    kn_hmac_sha1_update(&ctx, data, data_len);
    kn_hmac_sha1_final(&ctx, out);
}

/* ------------------------------------------------------------- utilities */

int kn_ct_memeq(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    volatile uint8_t diff = 0;
    size_t i;
    if (!a || !b) return a == b;
    for (i = 0; i < len; ++i) diff = (uint8_t)(diff | (uint8_t)(pa[i] ^ pb[i]));
    return diff == 0;
}
