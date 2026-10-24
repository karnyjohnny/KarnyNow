/* knsrtp.c — SRTP crypto phases 1+2. Contract + provenance in knsrtp.h. */
#include "kn/knsrtp.h"
#include "kn/kn_aes.h"
#include "kn/kn_sha1.h"

#include <string.h>

/* Add n (host u64) to a big-endian 128-bit counter; carry propagates over
 * all 16 bytes (RFC 3711 §4.1.1 treats the IV as one 128-bit counter). */
static void be128_add_u64(uint8_t ctr[16], uint64_t n)
{
    int i;
    unsigned carry = 0;
    for (i = 15; i >= 0; --i) {
        unsigned add = (i >= 8) ? (unsigned)((n >> ((15 - i) * 8)) & 0xFFu) : 0u;
        unsigned s = (unsigned)ctr[i] + add + carry;
        ctr[i] = (uint8_t)(s & 0xFFu);
        carry = s >> 8;
        if (i < 8 && carry == 0) break;   /* high half only sees the carry */
    }
}

static void xor_be32(uint8_t *dst, uint32_t v)
{
    dst[0] ^= (uint8_t)(v >> 24);
    dst[1] ^= (uint8_t)(v >> 16);
    dst[2] ^= (uint8_t)(v >> 8);
    dst[3] ^= (uint8_t)v;
}

static void xor_be16(uint8_t *dst, uint16_t v)
{
    dst[0] ^= (uint8_t)(v >> 8);
    dst[1] ^= (uint8_t)v;
}

/* ------------------------------------------------------------------- KDF */

int kn_srtp_kdf128(const uint8_t master_key[16],
                   const uint8_t *master_salt, size_t salt_len,
                   uint8_t label, uint8_t *out, size_t out_len)
{
    kn_aes128_ctx ctx;
    uint8_t iv[16], block[16];
    size_t off = 0;

    if (!master_key || !master_salt || !out) return -1;
    if (salt_len > KN_SRTP_MAX_SALT_LEN || out_len == 0 ||
        out_len > KN_SRTP_MAX_KDF_OUT)
        return -1;

    /* x = master_salt * 2^16 XOR label * 2^64  (nvst.rs:2892-2895) */
    memset(iv, 0, sizeof iv);
    memcpy(iv, master_salt, salt_len);
    iv[7] ^= label;

    kn_aes128_init(&ctx, master_key);
    while (off < out_len) {
        size_t n = (out_len - off) < 16u ? (out_len - off) : 16u;
        kn_aes128_encrypt(&ctx, iv, block);
        memcpy(out + off, block, n);
        off += n;
        be128_add_u64(iv, 1);
    }
    return 0;
}

/* --------------------------------------------------------------- AES-CM */

void kn_srtp_aes_cm_keystream_block(const uint8_t key[16], const uint8_t iv[16],
                                    uint64_t block_index, uint8_t out[16])
{
    kn_aes128_ctx ctx;
    uint8_t ctr[16];
    if (!key || !iv || !out) return;
    memcpy(ctr, iv, 16);
    be128_add_u64(ctr, block_index);
    kn_aes128_init(&ctx, key);
    kn_aes128_encrypt(&ctx, ctr, out);
}

void kn_srtp_aes_cm_keystream_xor(const uint8_t key[16], const uint8_t iv[16],
                                  uint64_t start_block,
                                  uint8_t *data, size_t len)
{
    kn_aes128_ctx ctx;
    uint8_t ctr[16], ks[16];
    size_t off = 0;
    if (!key || !iv || (!data && len > 0)) return;
    kn_aes128_init(&ctx, key);
    memcpy(ctr, iv, 16);
    be128_add_u64(ctr, start_block);
    while (off < len) {
        size_t n = (len - off) < 16u ? (len - off) : 16u;
        size_t k;
        kn_aes128_encrypt(&ctx, ctr, ks);
        for (k = 0; k < n; ++k) data[off + k] ^= ks[k];
        off += n;
        be128_add_u64(ctr, 1);
    }
}

/* ------------------------------------------------------------ IV builders */

uint64_t kn_srtp_packet_index(uint32_t roc, uint16_t seq)
{
    return ((uint64_t)roc << 16) | (uint64_t)seq;
}

void kn_srtp_aes_cm_iv(const uint8_t session_salt[14], uint32_t ssrc,
                       uint64_t packet_index, uint8_t out[16])
{
    int i;
    if (!session_salt || !out) return;
    memset(out, 0, 16);
    memcpy(out, session_salt, 14);          /* salt * 2^16 */
    xor_be32(out + 4, ssrc);                /* nvst.rs:2906-2908 */
    /* low 48 bits of packet_index, big-endian, into out[8..14]
     * (index_bytes[2..] of the u64 BE form, nvst.rs:2909-2912) */
    for (i = 0; i < 6; ++i)
        out[8 + i] ^= (uint8_t)((packet_index >> ((5 - i) * 8)) & 0xFFu);
}

void kn_srtp_gcm_iv(const uint8_t session_salt[12], uint32_t ssrc,
                    uint32_t roc, uint16_t seq, uint8_t out[12])
{
    if (!session_salt || !out) return;
    memcpy(out, session_salt, 12);
    xor_be32(out + 2, ssrc);
    xor_be32(out + 6, roc);
    xor_be16(out + 10, seq);
}

void kn_srtcp_gcm_iv(const uint8_t session_salt[12], uint32_t ssrc,
                     uint32_t srtcp_index, uint8_t out[12])
{
    if (!session_salt || !out) return;
    memcpy(out, session_salt, 12);
    xor_be32(out + 2, ssrc);
    xor_be32(out + 6, srtcp_index);
}

/* ============================ phase 2 ==================================== */

uint32_t kn_srtp_roc_of(uint64_t packet_index)
{
    return (uint32_t)(packet_index >> 16);
}

static int tag_len_valid(size_t tag_len)
{
    return tag_len == KN_SRTP_TAG_LEN_32 || tag_len == KN_SRTP_TAG_LEN_80;
}

int kn_srtp_auth_tag(const uint8_t auth_key[20],
                     const uint8_t *packet, size_t auth_len,
                     uint32_t roc, size_t tag_len, uint8_t mac_out[20])
{
    kn_hmac_sha1_ctx ctx;
    uint8_t roc_be[4];
    if (!auth_key || !mac_out || !tag_len_valid(tag_len)) return -1;
    if (!packet && auth_len) return -1;
    roc_be[0] = (uint8_t)(roc >> 24);
    roc_be[1] = (uint8_t)(roc >> 16);
    roc_be[2] = (uint8_t)(roc >> 8);
    roc_be[3] = (uint8_t)roc;
    /* MAC input = packet bytes || be32(roc)   (nvst.rs:2842-2845) */
    kn_hmac_sha1_init(&ctx, auth_key, 20);
    kn_hmac_sha1_update(&ctx, packet, auth_len);
    kn_hmac_sha1_update(&ctx, roc_be, 4);
    kn_hmac_sha1_final(&ctx, mac_out);
    return 0;
}

int kn_srtp_verify_tag(const uint8_t auth_key[20],
                       const uint8_t *packet, size_t auth_len,
                       uint32_t roc, const uint8_t *tag, size_t tag_len)
{
    uint8_t mac[20];
    if (!tag) return -1;
    if (kn_srtp_auth_tag(auth_key, packet, auth_len, roc, tag_len, mac) != 0)
        return -1;
    return kn_ct_memeq(mac, tag, tag_len) ? 0 : 1;
}

int kn_srtp_protect_cm_hmac(uint8_t *packet, size_t plain_len,
                            size_t payload_offset,
                            uint32_t ssrc, uint64_t packet_index,
                            const uint8_t enc_key[16],
                            const uint8_t auth_key[20],
                            const uint8_t session_salt[14], size_t tag_len)
{
    uint8_t iv[16], mac[20];
    if (!packet || !enc_key || !auth_key || !session_salt) return -1;
    if (!tag_len_valid(tag_len)) return -1;
    if (payload_offset > plain_len) return -1;

    kn_srtp_aes_cm_iv(session_salt, ssrc, packet_index, iv);
    kn_srtp_aes_cm_keystream_xor(enc_key, iv, 0, packet + payload_offset,
                                 plain_len - payload_offset);
    if (kn_srtp_auth_tag(auth_key, packet, plain_len,
                         kn_srtp_roc_of(packet_index), tag_len, mac) != 0)
        return -1;
    memcpy(packet + plain_len, mac, tag_len);
    return 0;
}

int kn_srtp_unprotect_cm_hmac(uint8_t *packet, size_t packet_len,
                              size_t payload_offset,
                              uint32_t ssrc, uint64_t packet_index,
                              const uint8_t enc_key[16],
                              const uint8_t auth_key[20],
                              const uint8_t session_salt[14], size_t tag_len,
                              size_t *out_plain_len)
{
    uint8_t iv[16];
    size_t auth_len;
    int vrc;
    const uint32_t roc = kn_srtp_roc_of(packet_index);  /* derived, nvst.rs:2638 */
    if (!packet || !enc_key || !auth_key || !session_salt || !out_plain_len)
        return -1;
    if (!tag_len_valid(tag_len)) return -1;
    if (packet_len < tag_len) return -1;                 /* MissingAuthenticationTag */
    auth_len = packet_len - tag_len;
    if (auth_len < payload_offset) return -1;            /* (nvst.rs:2832-2840) */

    /* Authenticate over the RECEIVED bytes BEFORE decrypting (nvst.rs order:
     * tag check -> replay check happens in the caller -> decrypt). */
    vrc = kn_srtp_verify_tag(auth_key, packet, auth_len, roc,
                             packet + auth_len, tag_len);
    if (vrc != 0) return vrc == 1 ? 1 : -1;

    kn_srtp_aes_cm_iv(session_salt, ssrc, packet_index, iv);
    kn_srtp_aes_cm_keystream_xor(enc_key, iv, 0, packet + payload_offset,
                                 auth_len - payload_offset);
    *out_plain_len = auth_len;
    return 0;
}

/* ------------------------------------------------------------ replay window */

void kn_srtp_replay_init(kn_srtp_replay *w)
{
    if (!w) return;
    w->have_highest = 0;
    w->highest_index = 0;
    memset(w->seen, 0, sizeof w->seen);
}

int kn_srtp_replay_guess(const kn_srtp_replay *w, uint16_t seq,
                         uint64_t *out_index)
{
    uint32_t roc, highest_seq;
    long delta;                 /* i32 semantics of nvst.rs:3145 */
    uint64_t guessed_roc;
    if (!w || !out_index) return -1;
    if (!w->have_highest) { *out_index = seq; return 0; }
    roc = (uint32_t)(w->highest_index >> 16);
    highest_seq = (uint32_t)(w->highest_index & 0xFFFFu);
    delta = (long)(int)seq - (long)(int)highest_seq;
    if (delta < -32768L) {
        /* seq just wrapped forward -> next ROC */
        if (roc == 0xFFFFFFFFu) return -1;              /* checked_add overflow */
        guessed_roc = (uint64_t)roc + 1u;
    } else if (delta > 32768L) {
        /* seq far "ahead" = actually a pre-wrap packet -> previous ROC */
        if (roc == 0) return -1;                        /* checked_sub underflow */
        guessed_roc = (uint64_t)roc - 1u;
    } else {
        guessed_roc = roc;
    }
    *out_index = (guessed_roc << 16) | (uint64_t)seq;
    return 0;
}

int kn_srtp_replay_check(const kn_srtp_replay *w, uint64_t index)
{
    uint64_t age;
    if (!w) return 1;
    if (!w->have_highest) return 0;
    if (index > w->highest_index) return 0;
    age = w->highest_index - index;
    if (age >= KN_SRTP_REPLAY_WINDOW_PACKETS) return 1;
    if (w->seen[age / 64u] & ((uint64_t)1 << (age % 64u))) return 1;
    return 0;
}

void kn_srtp_replay_commit(kn_srtp_replay *w, uint64_t index)
{
    if (!w) return;
    if (!w->have_highest) {
        w->have_highest = 1;
        w->highest_index = index;
        memset(w->seen, 0, sizeof w->seen);
        w->seen[0] = 1;
        return;
    }
    if (index > w->highest_index) {
        uint64_t advance = index - w->highest_index;
        if (advance >= KN_SRTP_REPLAY_WINDOW_PACKETS) {
            memset(w->seen, 0, sizeof w->seen);
        } else {
            unsigned word_shift = (unsigned)(advance / 64u);
            unsigned bit_shift  = (unsigned)(advance % 64u);
            unsigned dst;
            /* shift the age-bitmask "up" by advance bits (nvst.rs:3186-3199:
             * iterate destinations from the top so sources are not clobbered) */
            for (dst = KN_SRTP_REPLAY_WINDOW_WORDS; dst-- > 0u; ) {
                uint64_t shifted;
                unsigned src;
                if (dst < word_shift) { w->seen[dst] = 0; continue; }
                src = dst - word_shift;
                shifted = w->seen[src] << bit_shift;
                if (bit_shift != 0 && src > 0)
                    shifted |= w->seen[src - 1] >> (64u - bit_shift);
                w->seen[dst] = shifted;
            }
        }
        w->seen[0] |= 1;
        w->highest_index = index;
        return;
    }
    {
        uint64_t age = w->highest_index - index;
        if (age >= KN_SRTP_REPLAY_WINDOW_PACKETS) return; /* defensive no-op:
            reference .expect()s that check() passed; we never crash instead */
        w->seen[age / 64u] |= (uint64_t)1 << (age % 64u);
    }
}

/* ============================ phase 3 ====================================
 * AES-256 CM/KDF + AES-GCM via mbedTLS (see knsrtp.h phase-3 block for
 * provenance). Bare builds: honest NO_BACKEND stubs, zero fake crypto.
 * ======================================================================== */

#ifdef KN_TLS_BACKEND_MBEDTLS
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#endif

int kn_srtp_backend_256(void)
{
#ifdef KN_TLS_BACKEND_MBEDTLS
    return 1;
#else
    return 0;
#endif
}

int kn_srtp_backend_gcm(void)
{
#ifdef KN_TLS_BACKEND_MBEDTLS
    return 1;
#else
    return 0;
#endif
}

int kn_srtp_kdf256(const uint8_t master_key[32],
                   const uint8_t *master_salt, size_t salt_len,
                   uint8_t label, uint8_t *out, size_t out_len)
{
#ifdef KN_TLS_BACKEND_MBEDTLS
    mbedtls_aes_context ctx;
    uint8_t iv[16], block[16];
    size_t off = 0;
    int rc;

    if (!master_key || !master_salt || !out) return -1;
    if (salt_len > KN_SRTP_MAX_SALT_LEN || out_len == 0 ||
        out_len > KN_SRTP_MAX_KDF_OUT)
        return -1;

    memset(iv, 0, sizeof iv);
    memcpy(iv, master_salt, salt_len);
    iv[7] ^= label;                       /* key_id = label * 2^64 (byte 7) */

    mbedtls_aes_init(&ctx);
    rc = mbedtls_aes_setkey_enc(&ctx, master_key, 256);
    if (rc != 0) { mbedtls_aes_free(&ctx); return -1; }
    while (off < out_len) {
        size_t n = (out_len - off) < 16u ? (out_len - off) : 16u;
        if (mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, iv, block) != 0) {
            mbedtls_aes_free(&ctx);
            return -1;
        }
        memcpy(out + off, block, n);
        off += n;
        be128_add_u64(iv, 1);
    }
    mbedtls_aes_free(&ctx);
    return 0;
#else
    (void)master_key; (void)master_salt; (void)salt_len; (void)label;
    (void)out; (void)out_len;
    return KN_SRTP_ERR_NO_BACKEND;
#endif
}

void kn_srtp_aes256_cm_keystream_xor(const uint8_t key[32],
                                     const uint8_t iv[16],
                                     uint64_t start_block,
                                     uint8_t *data, size_t len)
{
#ifdef KN_TLS_BACKEND_MBEDTLS
    mbedtls_aes_context ctx;
    uint8_t ctr[16], ks[16];
    size_t off = 0;
    if (!key || !iv || (!data && len > 0)) return;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 256) != 0) {
        mbedtls_aes_free(&ctx);
        return;
    }
    memcpy(ctr, iv, 16);
    be128_add_u64(ctr, start_block);
    while (off < len) {
        size_t n = (len - off) < 16u ? (len - off) : 16u;
        size_t k;
        if (mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, ctr, ks) != 0)
            break;
        for (k = 0; k < n; ++k) data[off + k] ^= ks[k];
        off += n;
        be128_add_u64(ctr, 1);
    }
    mbedtls_aes_free(&ctx);
#else
    (void)key; (void)iv; (void)start_block; (void)data; (void)len;
#endif
}

#ifdef KN_TLS_BACKEND_MBEDTLS
static int gcm_key_bits_valid(unsigned key_bits)
{
    return key_bits == 128u || key_bits == 256u;
}
#endif

int kn_srtp_gcm_auth_decrypt(const uint8_t *key, unsigned key_bits,
                             const uint8_t iv[12],
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *tag, size_t tag_len,
                             uint8_t *ct_pt, size_t ct_len)
{
#ifdef KN_TLS_BACKEND_MBEDTLS
    mbedtls_gcm_context ctx;
    int rc;
    if (!key || !iv || !tag || (!ct_pt && ct_len) ||
        !gcm_key_bits_valid(key_bits) || tag_len < 4u || tag_len > 16u ||
        (aad_len && !aad))
        return -1;
    mbedtls_gcm_init(&ctx);
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, key_bits) != 0) {
        mbedtls_gcm_free(&ctx);
        return -1;
    }
    rc = mbedtls_gcm_auth_decrypt(&ctx, ct_len, iv, 12, aad, aad_len,
                                  tag, tag_len, ct_pt, ct_pt);
    mbedtls_gcm_free(&ctx);
    if (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) return 1;
    return rc == 0 ? 0 : -1;
#else
    (void)key; (void)key_bits; (void)iv; (void)aad; (void)aad_len;
    (void)tag; (void)tag_len; (void)ct_pt; (void)ct_len;
    return KN_SRTP_ERR_NO_BACKEND;
#endif
}

int kn_srtp_gcm_auth_encrypt(const uint8_t *key, unsigned key_bits,
                             const uint8_t iv[12],
                             const uint8_t *aad, size_t aad_len,
                             uint8_t tag_out[16], size_t tag_len,
                             uint8_t *pt_ct, size_t pt_len)
{
#ifdef KN_TLS_BACKEND_MBEDTLS
    mbedtls_gcm_context ctx;
    int rc;
    if (!key || !iv || !tag_out || (!pt_ct && pt_len) ||
        !gcm_key_bits_valid(key_bits) || tag_len < 4u || tag_len > 16u ||
        (aad_len && !aad))
        return -1;
    mbedtls_gcm_init(&ctx);
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, key_bits) != 0) {
        mbedtls_gcm_free(&ctx);
        return -1;
    }
    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, pt_len,
                                   iv, 12, aad, aad_len, pt_ct, pt_ct,
                                   tag_len, tag_out);
    mbedtls_gcm_free(&ctx);
    return rc == 0 ? 0 : -1;
#else
    (void)key; (void)key_bits; (void)iv; (void)aad; (void)aad_len;
    (void)tag_out; (void)tag_len; (void)pt_ct; (void)pt_len;
    return KN_SRTP_ERR_NO_BACKEND;
#endif
}
