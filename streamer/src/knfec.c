/* knfec.c — NVST Cauchy RS-FEC over GF(2^8)/0x11d. See knfec.h for the
 * contract and the provenance of every rule (nvst.rs @ a49b124 line refs).
 * Generated const tables: knfec_tables.h (tools/fec_reference.py). */
#include "kn/knfec.h"
#include "knfec_tables.h"

#include <string.h>

/* Stack bounds for the row/index pointer arrays (n+p <= 255). */
#define KN_FEC_ROWS_MAX KN_FEC_MAX_TOTAL_SHARDS

/* ------------------------------------------------------------- GF(2^8) */

uint8_t kn_gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    /* exp table is duplicated (512 entries) so the sum needs no modulo */
    return kn_gf_exp_table[(unsigned)kn_gf_log_table[a] + (unsigned)kn_gf_log_table[b]];
}

uint8_t kn_gf_inv(uint8_t v)
{
    if (v == 0) return 0;                    /* convention; callers guard */
    return kn_gf_exp_table[255u - (unsigned)kn_gf_log_table[v]];
}

void kn_gf_axpy(uint8_t *dst, const uint8_t *src, size_t len, uint8_t coeff)
{
    size_t i;
    if (!dst || !src || coeff == 0) return;
    if (coeff == 1) {                        /* hot path (nvst.rs:3764-3769) */
        for (i = 0; i < len; ++i) dst[i] = (uint8_t)(dst[i] ^ src[i]);
        return;
    }
    for (i = 0; i < len; ++i)
        dst[i] = (uint8_t)(dst[i] ^ kn_gf_mul(coeff, src[i]));
}

void kn_gf_scale(uint8_t *buf, size_t len, uint8_t coeff)
{
    size_t i;
    if (!buf || coeff == 1) return;          /* nvst.rs:3777-3778 */
    for (i = 0; i < len; ++i) buf[i] = kn_gf_mul(buf[i], coeff);
}

int kn_fec_cauchy_coeff(unsigned data_index, unsigned parity_index,
                        unsigned parity_shards, uint8_t *out)
{
    unsigned data_coord = parity_shards + data_index;   /* nvst.rs:3790 */
    uint8_t x;
    if (!out) return -1;
    if (data_coord > 0xFFu || parity_index > 0xFFu) return -1;
    x = (uint8_t)((uint8_t)data_coord ^ (uint8_t)parity_index);
    if (x == 0) return -1;                   /* disjoint ranges make this
                                                unreachable for valid params */
    *out = kn_gf_inv(x);
    return 0;
}

unsigned kn_fec_parity_count(unsigned data_shards, unsigned repair_percent)
{
    /* saturating ceil-multiply, mirrors nvst.rs:3688 */
    unsigned long long v = (unsigned long long)data_shards * repair_percent;
    if (v == 0) return 0;
    return (unsigned)((v + 99ULL) / 100ULL);
}

/* ---------------------------------------------------------------- encode */

int kn_fec_encode(const uint8_t *const *data, const size_t *data_lens,
                  unsigned data_shards,
                  uint8_t *const *parity, unsigned parity_shards,
                  size_t shard_len)
{
    unsigned d, p;
    if (!data || !data_lens || !parity) return -1;
    if (data_shards == 0 || parity_shards == 0 || shard_len == 0) return -1;
    if (data_shards + parity_shards > KN_FEC_MAX_TOTAL_SHARDS) return -4;
    for (d = 0; d < data_shards; ++d)
        if (!data[d] || data_lens[d] > shard_len) return -1;
    for (p = 0; p < parity_shards; ++p)
        if (!parity[p]) return -1;

    for (p = 0; p < parity_shards; ++p) memset(parity[p], 0, shard_len);
    for (d = 0; d < data_shards; ++d) {
        size_t n = data_lens[d];             /* zeros beyond (padding rule) */
        for (p = 0; p < parity_shards; ++p) {
            uint8_t c;
            if (kn_fec_cauchy_coeff(d, p, parity_shards, &c) != 0) return -4;
            kn_gf_axpy(parity[p], data[d], n, c);
        }
    }
    return 0;
}

/* --------------------------------------------------------- reconstruct */

size_t kn_fec_scratch_bytes(unsigned parity_shards, size_t shard_len)
{
    return (size_t)parity_shards * shard_len
         + (size_t)parity_shards * parity_shards;
}

int kn_fec_reconstruct(uint8_t *const *shards, const size_t *shard_lens,
                       unsigned data_shards, unsigned parity_shards,
                       size_t shard_len,
                       uint8_t *scratch, size_t scratch_len,
                       uint8_t *const *out_missing)
{
    unsigned miss[KN_FEC_ROWS_MAX];
    unsigned prow[KN_FEC_ROWS_MAX];          /* parity slot indices (0-based over parity) */
    uint8_t *rec[KN_FEC_ROWS_MAX];           /* Gauss-Jordan rows (pointer swaps) */
    uint8_t *mrow[KN_FEC_ROWS_MAX];
    unsigned m = 0, rows = 0, r, col, d, p;
    uint8_t *mat_base;

    if (!shards || !shard_lens || !out_missing) return -1;
    if (data_shards == 0 || parity_shards == 0 || shard_len == 0) return -1;
    if (data_shards + parity_shards > KN_FEC_MAX_TOTAL_SHARDS) return -4;
    for (d = 0; d < data_shards + parity_shards; ++d)
        if (shards[d] && shard_lens[d] > shard_len) return -1;

    for (d = 0; d < data_shards; ++d)
        if (!shards[d]) miss[m++] = d;
    if (m == 0) return 0;                    /* nothing to do (ref: Ok(())) */
    if (m > parity_shards) return -2;        /* not enough redundancy */
    for (r = 0; r < m; ++r)
        if (!out_missing[miss[r]]) return -1;

    /* first m present parity rows in index order (nvst.rs:3818-3824) */
    for (p = 0; p < parity_shards && rows < m; ++p)
        if (shards[data_shards + p]) prow[rows++] = p;
    if (rows != m) return -2;

    if (!scratch || scratch_len < (size_t)m * shard_len + (size_t)m * m) return -5;
    mat_base = scratch + (size_t)m * shard_len;
    for (r = 0; r < m; ++r) {
        const uint8_t *src = shards[data_shards + prow[r]];
        size_t srclen = shard_lens[data_shards + prow[r]];
        rec[r] = scratch + (size_t)r * shard_len;
        memcpy(rec[r], src, srclen);                    /* resize(shard_len,0) */
        if (srclen < shard_len) memset(rec[r] + srclen, 0, shard_len - srclen);
        mrow[r] = mat_base + (size_t)r * m;
        for (col = 0; col < m; ++col)
            if (kn_fec_cauchy_coeff(miss[col], prow[r], parity_shards,
                                    &mrow[r][col]) != 0)
                return -4;
    }

    /* subtract the known data shards' contributions (nvst.rs:3856-3874) */
    for (r = 0; r < m; ++r) {
        for (d = 0; d < data_shards; ++d) {
            uint8_t c;
            if (!shards[d]) continue;
            if (kn_fec_cauchy_coeff(d, prow[r], parity_shards, &c) != 0) return -4;
            kn_gf_axpy(rec[r], shards[d], shard_lens[d], c); /* known_len rule */
        }
    }

    /* Gauss-Jordan to identity (nvst.rs:3876-3920) */
    for (col = 0; col < m; ++col) {
        unsigned pivot = col;
        uint8_t inv;
        while (pivot < m && mrow[pivot][col] == 0) ++pivot;
        if (pivot == m) return -3;           /* singular — defensive only */
        if (pivot != col) {
            uint8_t *tm = mrow[col]; mrow[col] = mrow[pivot]; mrow[pivot] = tm;
            { uint8_t *tr = rec[col]; rec[col] = rec[pivot]; rec[pivot] = tr; }
        }
        inv = kn_gf_inv(mrow[col][col]);
        kn_gf_scale(&mrow[col][col], m - col, inv);
        kn_gf_scale(rec[col], shard_len, inv);
        for (r = 0; r < m; ++r) {
            uint8_t c;
            if (r == col) continue;
            c = mrow[r][col];
            if (c == 0) continue;
            kn_gf_axpy(&mrow[r][col], &mrow[col][col], m - col, c);
            kn_gf_axpy(rec[r], rec[col], shard_len, c);
        }
    }

    /* row r now holds the shard for miss[r] (nvst.rs:3922-3924) */
    for (r = 0; r < m; ++r)
        memcpy(out_missing[miss[r]], rec[r], shard_len);
    return (int)m;
}
