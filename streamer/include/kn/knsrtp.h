/* knsrtp.h — SRTP crypto phase 1: RFC 3711 KDF, AES-CM keystream, IV builders.
 *
 * Provenance (verified byte-for-byte against sources, not memory):
 *   - KDF labels: SRTP cipher key 0x00, auth key 0x01, cipher salt 0x02;
 *     SRTCP key 0x03, SRTCP salt 0x05        (nvst.rs:88-95 @ a49b124)
 *     "Mjolnir video is RTP, so use 0x00/0x02" (nvst.rs:89)
 *   - KDF: iv = zeros16; iv[..salt_len] = master_salt; iv[7] ^= label;
 *     out = AES-CM keystream(master_key, iv) truncated to out_len
 *                                  (nvst.rs:2888-2900 derive_aes128_cm_key;
 *                                   RFC 3711 §4.3.1: label*2^64 lands in byte 7)
 *   - AES-CM: keystream block i = E(key, be128(iv) + i), full 128-bit counter
 *                                   (RFC 3711 §4.1.1; nvst.rs:2820-2861)
 *   - SRTP CM IV: salt(14) at [0..14]; ^= ssrc_be32 at [4..8];
 *     ^= packet_index_be48 at [8..14]        (nvst.rs:2902-2913)
 *   - SRTP GCM IV (RFC 7714 §9.1): salt(12); ^= ssrc at [2..6], roc at [6..10],
 *     seq at [10..12]                        (nvst.rs:2915-2926)
 *   - SRTCP GCM IV (RFC 7714 §9.2): salt(12); ^= ssrc at [2..6],
 *     srtcp_index at [6..10]                 (nvst.rs:2929-2939)
 *
 * Phase 2 (next): HMAC-SHA1 auth + be32(roc) MAC input (nvst.rs:2820-2872),
 * replay window, GCM via mbedTLS. Not in this header yet — do not fake it.
 *
 * Zero allocation; all buffers caller-provided; safe from any thread.
 */
#ifndef KN_SRTP_H
#define KN_SRTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* KDF labels (nvst.rs:88-95, RFC 3711 §4.3.1) */
#define KN_SRTP_LABEL_CIPHER_KEY  0x00u  /* GFN_SRTP_KEY_LABEL   — video (Mjolnir) */
#define KN_SRTP_LABEL_AUTH_KEY    0x01u  /* RFC 3711 authentication key           */
#define KN_SRTP_LABEL_SALT        0x02u  /* GFN_SRTP_SALT_LABEL  — video (Mjolnir) */
#define KN_SRTCP_LABEL_KEY        0x03u  /* GFN_SRTCP_KEY_LABEL                   */
#define KN_SRTCP_LABEL_SALT       0x05u  /* GFN_SRTCP_SALT_LABEL                  */

#define KN_SRTP_MAX_SALT_LEN 14   /* 14 for AES-CM profiles, 12 for GCM (nvst.rs:1586-1640) */
#define KN_SRTP_MAX_KDF_OUT  4096

/* --------------------------------------------------------------- KDF */

/* RFC 3711 §4.3.1 KDF with AES-128-CM PRF, initial derivation only
 * (key_derivation_rate = 0, index DIV kdr = 0 — GFN never rekeys mid-stream;
 * nvst.rs has no kdr parameter either).
 *   master_key:  16 bytes
 *   master_salt: salt_len bytes (12 or 14 in practice; <= 14 enforced)
 *   label:       one of KN_SRTP_LABEL_* / KN_SRTCP_LABEL_*
 *   out:         out_len bytes (16 cipher key, 14/12 salt, 20 or 94 auth key)
 * Returns 0, or -1 on invalid arguments. */
int kn_srtp_kdf128(const uint8_t master_key[16],
                   const uint8_t *master_salt, size_t salt_len,
                   uint8_t label, uint8_t *out, size_t out_len);

/* ---------------------------------------------------------- AES-CM */

/* keystream block at counter offset `block_index` from iv:
 * out = E(key, be128(iv) + block_index). Full 128-bit counter (carry
 * propagates across all 16 bytes — RFC 3711 B.2 exercises the FEFF->FF00
 * carry). */
void kn_srtp_aes_cm_keystream_block(const uint8_t key[16], const uint8_t iv[16],
                                    uint64_t block_index, uint8_t out[16]);

/* XOR data with the keystream starting at counter offset `start_block`
 * (for SRTP: start_block = 0 — the packet index is already folded into the
 * IV; see kn_srtp_aes_cm_iv). Encrypt == decrypt. Partial trailing block
 * supported (matches apply_keystream semantics, nvst.rs:2858-2861). */
void kn_srtp_aes_cm_keystream_xor(const uint8_t key[16], const uint8_t iv[16],
                                  uint64_t start_block,
                                  uint8_t *data, size_t len);

/* ------------------------------------------------------------ IV builders */

/* 48-bit SRTP packet index: (ROC << 16) | SEQ. */
uint64_t kn_srtp_packet_index(uint32_t roc, uint16_t seq);

/* SRTP AES-CM IV (nvst.rs:2902): out[16]. */
void kn_srtp_aes_cm_iv(const uint8_t session_salt[14], uint32_t ssrc,
                       uint64_t packet_index, uint8_t out[16]);

/* SRTP AES-GCM IV, RFC 7714 §9.1 (nvst.rs:2915): out[12]. */
void kn_srtp_gcm_iv(const uint8_t session_salt[12], uint32_t ssrc,
                    uint32_t roc, uint16_t seq, uint8_t out[12]);

/* SRTCP AES-GCM IV, RFC 7714 §9.2 (nvst.rs:2929): out[12]. */
void kn_srtcp_gcm_iv(const uint8_t session_salt[12], uint32_t ssrc,
                     uint32_t srtcp_index, uint8_t out[12]);

/* ============================ phase 2 ====================================
 * Authentication (HMAC-SHA1 with be32(roc)), in-place protect/unprotect for
 * the AES_CM_128_HMAC_SHA1_{32,80} profiles, and the 2048-packet replay
 * window. Provenance:
 *   - tag = first tag_len bytes of
 *         HMAC-SHA1(auth_key20, packet[..authenticated_len] || be32(roc))
 *     verified BEFORE decryption; constant-time compare
 *                                             (nvst.rs:2820-2871)
 *   - replay: guess_packet_index / check / commit, window 2048 packets
 *     (32 x u64 bitmask, bit N = highest_index - N)
 *                                             (nvst.rs:233-234, 3121-3211)
 *   - order of operations in unprotect(): auth -> replay check -> decrypt
 *     -> commit                            (nvst.rs:2635-2705)
 * AES_CM_256 needs a 32-byte-key AES (mbedTLS, stage 2) — the enc_key below
 * is 16 bytes; 256-bit profiles are rejected until then.
 * SRTCP note: nvst uses SRTCP only for GCM profiles (SrtcpSender::from_material
 * returns None for CM); RR travels on the DTLS-encrypted rtcp1 SCTP channel —
 * no SRTCP-CM code is needed here (nvst.rs:4346-4410).
 * ======================================================================== */

#define KN_SRTP_TAG_LEN_32  4    /* HMAC_SHA1_32 profile */
#define KN_SRTP_TAG_LEN_80  10   /* HMAC_SHA1_80 profile (GFN default) */

#define KN_SRTP_REPLAY_WINDOW_PACKETS 2048u          /* nvst.rs:225/233 */
#define KN_SRTP_REPLAY_WINDOW_WORDS   (KN_SRTP_REPLAY_WINDOW_PACKETS / 64u)

/* roc component of a 48-bit packet index. */
uint32_t kn_srtp_roc_of(uint64_t packet_index);

/* Full 20-byte MAC; tag = mac[0..tag_len). auth_len = packet bytes covered
 * (whole packet minus the tag). roc is appended big-endian AFTER the packet
 * bytes (nvst.rs:2843-2845). Returns 0, -1 on invalid args (tag_len must be
 * 4 or 10). */
int kn_srtp_auth_tag(const uint8_t auth_key[20],
                     const uint8_t *packet, size_t auth_len,
                     uint32_t roc, size_t tag_len, uint8_t mac_out[20]);

/* Constant-time tag check. Returns 0 on match, 1 on mismatch
 * (AuthenticationFailed), -1 on invalid args. */
int kn_srtp_verify_tag(const uint8_t auth_key[20],
                       const uint8_t *packet, size_t auth_len,
                       uint32_t roc, const uint8_t *tag, size_t tag_len);

/* In-place protect: encrypts packet[payload_offset .. plain_len) with the
 * AES-CM keystream for IV(salt, ssrc, packet_index), then writes the tag at
 * packet[plain_len .. plain_len + tag_len). The buffer must have room for
 * plain_len + tag_len bytes. Returns 0, or -1 on invalid args. */
int kn_srtp_protect_cm_hmac(uint8_t *packet, size_t plain_len,
                            size_t payload_offset,
                            uint32_t ssrc, uint64_t packet_index,
                            const uint8_t enc_key[16],
                            const uint8_t auth_key[20],
                            const uint8_t session_salt[14], size_t tag_len);

/* In-place unprotect for a received packet of packet_len = auth_len +
 * tag_len bytes: verifies the tag over the RECEIVED (encrypted) bytes plus
 * be32(roc) first — on failure nothing is decrypted (nvst.rs order) — then
 * decrypts packet[payload_offset .. packet_len - tag_len).
 * The ROC used in the MAC is DERIVED from packet_index (>> 16), exactly as
 * the reference does (nvst.rs:2638-2640) — callers cannot pass an
 * inconsistent pair.
 * Returns: 0 ok (*out_plain_len = auth_len), 1 = AuthenticationFailed,
 * -1 = invalid args / malformed (packet_len < tag_len, auth_len <
 * payload_offset). */
int kn_srtp_unprotect_cm_hmac(uint8_t *packet, size_t packet_len,
                              size_t payload_offset,
                              uint32_t ssrc, uint64_t packet_index,
                              const uint8_t enc_key[16],
                              const uint8_t auth_key[20],
                              const uint8_t session_salt[14], size_t tag_len,
                              size_t *out_plain_len);

/* ------------------------------------------------------------ replay window */

typedef struct kn_srtp_replay {
    int      have_highest;
    uint64_t highest_index;
    uint64_t seen[KN_SRTP_REPLAY_WINDOW_WORDS]; /* bit N = highest_index - N */
} kn_srtp_replay;

void kn_srtp_replay_init(kn_srtp_replay *w);

/* ROC/seq estimation (nvst.rs:3137-3152): with no history, index = seq;
 * else delta = seq - highest_seq (signed 16-bit semantics via i32 compare
 * against +/-32768) adjusts the ROC by +/-1.
 * Returns 0 and writes *out_index, or -1 when the ROC estimate underflows
 * (ReplayRejected). */
int kn_srtp_replay_guess(const kn_srtp_replay *w, uint16_t seq,
                         uint64_t *out_index);

/* Returns 0 = acceptable, 1 = ReplayRejected (duplicate or older than the
 * 2048-packet window). Always 0 before the first commit. */
int kn_srtp_replay_check(const kn_srtp_replay *w, uint64_t index);

/* Record an accepted index. Call ONLY after check() returned 0 (the
 * in-window branch assumes it; out-of-window late commits are ignored
 * defensively rather than crashing — the reference .expect()s here). */
void kn_srtp_replay_commit(kn_srtp_replay *w, uint64_t index);

#ifdef __cplusplus
}
#endif

#endif /* KN_SRTP_H */
