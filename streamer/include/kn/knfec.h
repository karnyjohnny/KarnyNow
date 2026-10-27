/* knfec.h — NVST GameStream Cauchy Reed-Solomon FEC over GF(2^8).
 *
 * Wire-compatible with NVIDIA's GameStream FEC as implemented by the
 * reference (OpenNOW nvst.rs @ a49b124):
 *   - GF(2^8), primitive polynomial 0x11d              (nvst.rs:3717-3742)
 *   - Cauchy coefficient C[d][p] = inv((P + d) XOR p)   (nvst.rs:3785-3793)
 *   - systematic encoding: parity[p] = XOR_d C[d][p] * data[d]
 *                                                    (nvst.rs tests 13095+)
 *   - Gauss-Jordan reconstruction                     (nvst.rs:3798-3924)
 *
 * PITFALL #3 (BOOTSTRAP §8): this is a CAUCHY matrix, NOT Vandermonde.
 * Generic RS libraries produce "mathematically valid but corrupt H.264"
 * against NVIDIA parity. Never substitute a generic RS implementation here.
 *
 * PITFALL #4: reconstructed shards may contain transport metadata (RTP/GS
 * header bytes, fecInfo) that legitimately differs from the original data
 * shard — the algebra only guarantees the shard payload the encoder folded
 * in. The transport layer MUST restore the packet envelope from the block
 * template and MUST NOT validate fecInfo on recovered shards
 * (nvst.rs:4101-4107 and tests at 13100-13106, 13199-13204).
 *
 * Memory policy: zero allocation. All buffers (shards, parity outputs,
 * reconstruction scratch, per-missing-shard outputs) are caller-provided —
 * the 2 GB budget (R11) and crash-resistance rules require it.
 *
 * Thread safety: all functions are pure over caller memory and safe from
 * any thread (tables are const, generated — see knfec_tables.h).
 */
#ifndef KN_FEC_H
#define KN_FEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* data_shards + parity_shards must stay <= 255 so the Cauchy coordinates
 * (P + d) and (p) are distinct u8 values (nvst.rs:3806). */
#define KN_FEC_MAX_TOTAL_SHARDS 255

/* ------------------------------------------------------------- GF(2^8) */

uint8_t kn_gf_mul(uint8_t a, uint8_t b);
uint8_t kn_gf_inv(uint8_t v);            /* total: kn_gf_inv(0) == 0 by convention */
/* dst[0..len) ^= coeff * src[0..len) */
void    kn_gf_axpy(uint8_t *dst, const uint8_t *src, size_t len, uint8_t coeff);
/* buf[i] *= coeff */
void    kn_gf_scale(uint8_t *buf, size_t len, uint8_t coeff);

/* C[d][p] = inv((parity_shards + data_index) XOR parity_index).
 * Returns 0, or -1 if a coordinate leaves u8 range or is zero. */
int kn_fec_cauchy_coeff(unsigned data_index, unsigned parity_index,
                        unsigned parity_shards, uint8_t *out);

/* parity_shards = ceil(data_shards * repair_percent / 100)  (nvst.rs:3688) */
unsigned kn_fec_parity_count(unsigned data_shards, unsigned repair_percent);

/* ---------------------------------------------------------------- encode */

/* parity[p][0..shard_len) = XOR_d C[d][p] * data[d], where data[d]
 * contributes min(data_lens[d], shard_len) bytes and zeros beyond (the
 * reference zero-pads shorter shards, nvst.rs:3852/3867). Parity buffers
 * are fully zeroed by this call before folding.
 * Returns 0, or -1 invalid arguments, -4 sizes out of range. */
int kn_fec_encode(const uint8_t *const *data, const size_t *data_lens,
                  unsigned data_shards,
                  uint8_t *const *parity, unsigned parity_shards,
                  size_t shard_len);

/* --------------------------------------------------------- reconstruct */

/* Bytes of scratch kn_fec_reconstruct needs for the given worst case
 * (m = parity_shards rows): m * shard_len + m * m. */
size_t kn_fec_scratch_bytes(unsigned parity_shards, size_t shard_len);

/* Mirrors reconstruct_nvst_cauchy_data (nvst.rs:3798-3924):
 *   - shards[0..n+p): known shards have a pointer and shard_lens[i] <=
 *     shard_len (treated zero-padded); missing shards have NULL pointers.
 *   - the first m PRESENT parity shards in index order are used, where m
 *     is the number of missing data shards.
 *   - out_missing[d] for every MISSING data index d must point to
 *     shard_len writable bytes; entries for present shards are ignored.
 *   - scratch must provide kn_fec_scratch_bytes(p, shard_len) bytes.
 * Returns the number of reconstructed data shards (>= 0), or:
 *   -1 invalid arguments, -2 not enough present parity shards,
 *   -3 singular matrix (defensive; unreachable for valid Cauchy params),
 *   -4 sizes out of range (n+p > 255), -5 scratch too small. */
int kn_fec_reconstruct(uint8_t *const *shards, const size_t *shard_lens,
                       unsigned data_shards, unsigned parity_shards,
                       size_t shard_len,
                       uint8_t *scratch, size_t scratch_len,
                       uint8_t *const *out_missing);

#ifdef __cplusplus
}
#endif

#endif /* KN_FEC_H */
