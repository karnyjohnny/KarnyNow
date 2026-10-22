/* test_knfec.c — known-answer + property tests for the NVST Cauchy RS-FEC.
 *
 * Golden vectors come from tools/fec_reference.py (independent Python port of
 * nvst.rs @ a49b124), including a 1:1 port of the reference implementation's
 * own test nvst_cauchy_fec_reconstructs_multiple_missing_data_shards
 * (nvst.rs:13239). The property test cross-checks encode/reconstruct against
 * the zero-padding semantics of the reference. Runs on Linux host and Win7. */
#include "kn/knfec.h"
#include "knfec_kat_vectors.h"
#include "test_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------- algorithmic reference */

/* Rebuild the GF tables from the polynomial definition (nvst.rs:3726-3739)
 * so the embedded generated tables are verified, not trusted. */
static unsigned char ref_exp[512];
static unsigned char ref_log[256];

static void build_ref_tables(void)
{
    int v = 1, power;
    memset(ref_exp, 0, sizeof ref_exp);
    memset(ref_log, 0, sizeof ref_log);
    for (power = 0; power < 255; ++power) {
        ref_exp[power] = (unsigned char)v;
        ref_log[v] = (unsigned char)power;
        v <<= 1;
        if (v & 0x100) v ^= 0x11d;
    }
    for (power = 255; power < 512; ++power)
        ref_exp[power] = ref_exp[power - 255];
}

static unsigned char ref_mul(unsigned char a, unsigned char b)
{
    if (!a || !b) return 0;
    return ref_exp[(unsigned)ref_log[a] + (unsigned)ref_log[b]];
}

/* Deterministic PRNG (xorshift32) — identical sequence on every platform. */
static unsigned g_seed = 0xC0FFEEu;
static unsigned rnd(void)
{
    unsigned x = g_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_seed = x;
    return x;
}

/* ------------------------------------------------------------------ tests */

static void test_gf_tables(void)
{
    int a, b, i;
    build_ref_tables();

    /* embedded tables == algorithmic definition: full 256x256 mul sweep */
    for (a = 0; a < 256; ++a)
        for (b = 0; b < 256; ++b)
            if (kn_gf_mul((uint8_t)a, (uint8_t)b) != ref_mul((unsigned char)a,
                                                             (unsigned char)b)) {
                CHECK(0, "gf_mul disagrees with polynomial definition");
                return;
            }
    CHECK(1, "gf_mul table matches 0x11d definition (65536 products)");

    for (a = 1; a < 256; ++a) {
        if (kn_gf_mul((uint8_t)a, kn_gf_inv((uint8_t)a)) != 1) {
            CHECK(0, "a * inv(a) == 1");
            return;
        }
    }
    CHECK(1, "all 255 inverses correct");
    CHECK(kn_gf_inv(0) == 0, "inv(0) convention");

    for (i = 0; i < 16; ++i)
        CHECK(ref_exp[i] == k_kat_gf_exp[i], "generator powers match golden exp");
    for (i = 1; i < 16; ++i)
        CHECK(kn_gf_mul((uint8_t)ref_exp[i - 1], 2) == ref_exp[i],
              "mul by generator advances the power sequence");
    CHECK(ref_exp[8] == k_kat_gf_exp_wrap, "0x11d wrap golden value");

    for (i = 0; i < K_KAT_GF_MUL_COUNT; ++i)
        CHECK(kn_gf_mul(k_kat_gf_mul[i][0], k_kat_gf_mul[i][1]) == k_kat_gf_mul[i][2],
              "gf_mul golden pair (python reference)");

    for (i = 0; i < (int)(sizeof k_kat_gf_inv_inputs / sizeof k_kat_gf_inv_inputs[0]); ++i)
        CHECK(kn_gf_inv(k_kat_gf_inv_inputs[i]) == k_kat_gf_inv[i],
              "gf_inv golden value (python reference)");
}

static void test_cauchy_golden(void)
{
    unsigned d, p;
    uint8_t c;
    for (d = 0; d < 5; ++d)
        for (p = 0; p < 2; ++p) {
            CHECK(kn_fec_cauchy_coeff(d, p, 2, &c) == 0 && c == k_kat_cauchy_5_2[d][p],
                  "cauchy C[d][p] (n=5,P=2) golden");
        }
    for (d = 0; d < 3; ++d) {
        CHECK(kn_fec_cauchy_coeff(d, 0, 1, &c) == 0 && c == k_kat_cauchy_3_1[d][0],
              "cauchy C[d][0] (n=3,P=1) golden");
    }
    CHECK(kn_fec_cauchy_coeff(0, 0, 300, &c) == -1, "coordinate > 255 rejected");
    CHECK(kn_fec_cauchy_coeff(255, 0, 1, &c) == -1, "coordinate overflow rejected");
}

static void test_parity_count(void)
{
    CHECK(kn_fec_parity_count(3, 33) == 1, "ceil 99/100 = 1");
    CHECK(kn_fec_parity_count(3, 34) == 2, "ceil 102/100 = 2");
    CHECK(kn_fec_parity_count(10, 25) == 3, "ceil 250/100 = 3");
    CHECK(kn_fec_parity_count(7, 100) == 7, "100 percent");
    CHECK(kn_fec_parity_count(5, 20) == 1, "exact 100/100 = 1");
    CHECK(kn_fec_parity_count(5, 21) == 2, "ceil 105/100 = 2");
    CHECK(kn_fec_parity_count(0, 50) == 0, "zero data shards");
    CHECK(kn_fec_parity_count(4, 0) == 0, "zero repair percent");
}

/* 1:1 port of nvst.rs:13239 (reference implementation's own KAT). */
static void test_nvst_reference_vector(void)
{
    static uint8_t data[5][97];
    static uint8_t parity[2][97];
    uint8_t *dptr[5], *pptr[2], *shards[7], *out[5];
    size_t dlens[5], slens[7];
    uint8_t scratch[512];
    int s, b, rc;

    for (s = 0; s < 5; ++s) {
        for (b = 0; b < 97; ++b) data[s][b] = (uint8_t)(s * 41 + b * 17);
        dptr[s] = data[s];
        dlens[s] = 97;
    }
    pptr[0] = parity[0];
    pptr[1] = parity[1];

    rc = kn_fec_encode((const uint8_t *const *)dptr, dlens, 5, pptr, 2, 97);
    CHECK(rc == 0, "reference vector encode rc");
    CHECK(memcmp(parity[0], k_kat_nvst_parity_0, 97) == 0,
          "parity[0] byte-identical to python reference");
    CHECK(memcmp(parity[1], k_kat_nvst_parity_1, 97) == 0,
          "parity[1] byte-identical to python reference");

    /* erase data shards 1 and 3, reconstruct from shards {0,2,4} + parity */
    shards[0] = data[0]; slens[0] = 97;
    shards[1] = NULL;    slens[1] = 0;
    shards[2] = data[2]; slens[2] = 97;
    shards[3] = NULL;    slens[3] = 0;
    shards[4] = data[4]; slens[4] = 97;
    shards[5] = parity[0]; slens[5] = 97;
    shards[6] = parity[1]; slens[6] = 97;
    memset(out, 0, sizeof out);
    {
        static uint8_t rec1[97], rec3[97];
        out[1] = rec1;
        out[3] = rec3;
        rc = kn_fec_reconstruct(shards, slens, 5, 2, 97, scratch, sizeof scratch,
                                (uint8_t *const *)out);
        CHECK(rc == 2, "reference vector reconstructs 2 shards");
        CHECK(memcmp(rec1, data[1], 97) == 0, "shard 1 restored exactly");
        CHECK(memcmp(rec3, data[3], 97) == 0, "shard 3 restored exactly");
    }
}

/* Zero-padding semantics (nvst.rs:3852, 3867-3873): a known shard shorter
 * than shard_len contributes zeros beyond its length; the reconstructed
 * shard is the zero-padded original. */
static void test_short_shard_padding(void)
{
    uint8_t d0[8], d1[5], d2[8], par[8], rec1[8];
    uint8_t *dptr[3], *shards[4], *out[3];
    size_t dlens[3], slens[4];
    uint8_t scratch[64];
    int b, rc;

    for (b = 0; b < 8; ++b) { d0[b] = (uint8_t)(b * 29); d2[b] = (uint8_t)(106 + b * 29); }
    for (b = 0; b < 5; ++b) d1[b] = (uint8_t)(53 + b * 29);

    dptr[0] = d0; dptr[1] = d1; dptr[2] = d2;
    dlens[0] = 8; dlens[1] = 5; dlens[2] = 8;
    {
        uint8_t *pptr[1];
        pptr[0] = par;
        rc = kn_fec_encode((const uint8_t *const *)dptr, dlens, 3, pptr, 1, 8);
        CHECK(rc == 0, "short-shard encode rc");
    }
    CHECK(memcmp(par, k_kat_short_parity, 8) == 0, "short-shard parity golden");
    CHECK(memcmp(d0, k_kat_short_data0, 8) == 0, "test data matches vector data0");
    CHECK(memcmp(d2, k_kat_short_data2, 8) == 0, "test data matches vector data2");

    shards[0] = d0;  slens[0] = 8;
    shards[1] = NULL; slens[1] = 0;
    shards[2] = d2;  slens[2] = 8;
    shards[3] = par; slens[3] = 8;
    out[0] = NULL; out[1] = rec1; out[2] = NULL;
    rc = kn_fec_reconstruct(shards, slens, 3, 1, 8, scratch, sizeof scratch, out);
    CHECK(rc == 1, "short-shard reconstruct rc");
    CHECK(memcmp(rec1, k_kat_short_data1_padded, 8) == 0,
          "reconstructed shard = original + zero padding");
}

static void test_wide_block_encode(void)
{
    static uint8_t data[K_KAT_WIDE_N][K_KAT_WIDE_LEN];
    static uint8_t parity[K_KAT_WIDE_P][K_KAT_WIDE_LEN];
    uint8_t *dptr[K_KAT_WIDE_N], *pptr[K_KAT_WIDE_P];
    const uint8_t *goldens[K_KAT_WIDE_P];
    size_t dlens[K_KAT_WIDE_N];
    int d, b, rc;

    goldens[0] = k_kat_wide_parity_0;
    goldens[1] = k_kat_wide_parity_1;
    goldens[2] = k_kat_wide_parity_2;
    goldens[3] = k_kat_wide_parity_3;
    for (d = 0; d < K_KAT_WIDE_N; ++d) {
        for (b = 0; b < K_KAT_WIDE_LEN; ++b)
            data[d][b] = (uint8_t)(d * 53 + b * 29 + d * b * 7);
        dptr[d] = data[d];
        dlens[d] = K_KAT_WIDE_LEN;
        if (d < K_KAT_WIDE_P) pptr[d] = parity[d];
    }
    rc = kn_fec_encode((const uint8_t *const *)dptr, dlens, K_KAT_WIDE_N,
                       pptr, K_KAT_WIDE_P, K_KAT_WIDE_LEN);
    CHECK(rc == 0, "wide encode rc");
    for (d = 0; d < K_KAT_WIDE_P; ++d)
        CHECK(memcmp(parity[d], goldens[d], K_KAT_WIDE_LEN) == 0,
              "wide parity golden (n=12 p=4)");
}

/* Deterministic randomized round-trips with mixed short shards and every
 * erasure count up to p. */
#define PT_MAX_N    16
#define PT_MAX_P     8
#define PT_MAX_LEN 1280

static void test_property_roundtrips(void)
{
    static uint8_t data[PT_MAX_N][PT_MAX_LEN];
    static uint8_t parity[PT_MAX_P][PT_MAX_LEN];
    static uint8_t rec[PT_MAX_N][PT_MAX_LEN];
    static uint8_t scratch[PT_MAX_P * PT_MAX_LEN + PT_MAX_P * PT_MAX_P];
    static const size_t lens_choices[4] = { 1, 7, 64, 1280 };
    int iter, ok_iters = 0;

    g_seed = 0xC0FFEEu;
    for (iter = 0; iter < 200; ++iter) {
        unsigned n = 1u + rnd() % PT_MAX_N;
        unsigned pct = 5u + rnd() % 46u;
        unsigned p = kn_fec_parity_count(n, pct);
        size_t shard_len = lens_choices[rnd() % 4];
        size_t dlens[PT_MAX_N], slens[PT_MAX_N + PT_MAX_P];
        uint8_t *dptr[PT_MAX_N], *pptr[PT_MAX_P], *shards[PT_MAX_N + PT_MAX_P];
        uint8_t *out[PT_MAX_N];
        unsigned erased[PT_MAX_N];
        unsigned m = 0, i, d, max_erase;
        int rc, ok = 1;

        if (p == 0) p = 1;
        if (p > PT_MAX_P) p = PT_MAX_P;
        if (n + p > KN_FEC_MAX_TOTAL_SHARDS) continue;

        for (d = 0; d < n; ++d) {
            size_t b;
            dlens[d] = (d % 3 == 0 && shard_len > 1) ? 1 + rnd() % shard_len : shard_len;
            for (b = 0; b < dlens[d]; ++b) data[d][b] = (uint8_t)(rnd() & 0xFF);
            dptr[d] = data[d];
        }
        for (i = 0; i < p; ++i) pptr[i] = parity[i];
        rc = kn_fec_encode((const uint8_t *const *)dptr, dlens, n, pptr, p, shard_len);
        if (rc != 0) { CHECK(0, "property encode rc"); return; }

        /* choose m distinct data shards to erase, m in [0, min(p,n)] */
        max_erase = (p < n) ? p : n;
        m = rnd() % (max_erase + 1);
        for (i = 0; i < m; ++i) {
            unsigned cand;
            int dup;
            do {
                cand = rnd() % n;
                dup = 0;
                for (d = 0; d < i; ++d) if (erased[d] == cand) { dup = 1; break; }
            } while (dup);
            erased[i] = cand;
        }
        for (d = 0; d < n; ++d) {
            unsigned e;
            int is_erased = 0;
            for (e = 0; e < m; ++e) if (erased[e] == d) { is_erased = 1; break; }
            shards[d] = is_erased ? NULL : data[d];
            slens[d] = is_erased ? 0 : dlens[d];
            memset(out, 0, sizeof out);
        }
        for (i = 0; i < p; ++i) {
            shards[n + i] = parity[i];
            slens[n + i] = shard_len;
        }
        for (i = 0; i < m; ++i) out[erased[i]] = rec[erased[i]];

        rc = kn_fec_reconstruct(shards, slens, n, p, shard_len, scratch,
                                sizeof scratch, out);
        if (rc != (int)m) {
            printf("FAIL property iter %d: rc=%d expected m=%u (n=%u p=%u len=%zu)\n",
                   iter, rc, m, n, p, shard_len);
            ok = 0;
        }
        for (i = 0; i < m && ok; ++i) {
            d = erased[i];
            if (memcmp(rec[d], data[d], dlens[d]) != 0) {
                printf("FAIL property iter %d: shard %u payload mismatch\n", iter, d);
                ok = 0;
            }
            /* zero padding beyond the original length */
            {
                size_t b;
                for (b = dlens[d]; b < shard_len; ++b)
                    if (rec[d][b] != 0) {
                        printf("FAIL property iter %d: shard %u padding byte %zu\n",
                               iter, d, b);
                        ok = 0;
                        break;
                    }
            }
        }
        if (!ok) { CHECK(0, "property round-trip"); return; }
        ok_iters++;
    }
    printf("  property round-trips executed: %d/200\n", ok_iters);
    /* a few iterations may be skipped by the range guard; require most */
    CHECK(ok_iters >= 190, "property iterations ran (>=190/200)");
}

static void test_error_paths(void)
{
    static uint8_t buf[4][16];
    uint8_t *shards[300], *out[300];
    size_t slens[300];
    uint8_t scratch[4096];
    int i, rc;

    memset(buf, 0xA5, sizeof buf);
    for (i = 0; i < 300; ++i) { shards[i] = NULL; slens[i] = 0; out[i] = NULL; }

    /* n+p > 255 */
    rc = kn_fec_reconstruct(shards, slens, 200, 100, 16, scratch, sizeof scratch, out);
    CHECK(rc == -4, "n+p > 255 rejected");

    /* not enough parity: n=3 p=1, erase 2 data shards */
    shards[0] = buf[0]; slens[0] = 16;
    shards[1] = NULL;   slens[1] = 0;
    shards[2] = NULL;   slens[2] = 0;
    shards[3] = buf[1]; slens[3] = 16;
    out[1] = buf[2]; out[2] = buf[3];
    rc = kn_fec_reconstruct(shards, slens, 3, 1, 16, scratch, sizeof scratch, out);
    CHECK(rc == -2, "insufficient parity rejected");

    /* nothing missing -> 0, no writes.
     * NOTE: every subcase below sets its FULL state explicitly — an earlier
     * draft leaked shards[2]==NULL from the previous case and the -2 path
     * fired before the behaviour under test (caught by the first T9 run). */
    shards[0] = buf[0]; slens[0] = 16;
    shards[1] = buf[1]; slens[1] = 16;
    shards[2] = buf[2]; slens[2] = 16;
    shards[3] = buf[3]; slens[3] = 16;      /* parity */
    rc = kn_fec_reconstruct(shards, slens, 3, 1, 16, scratch, sizeof scratch, out);
    CHECK(rc == 0, "no missing shards returns 0");

    /* scratch too small: exactly one missing data shard (m=1 <= p=1),
     * out buffer present, parity present -> must reach the scratch check */
    shards[1] = NULL; slens[1] = 0;
    out[1] = buf[1];
    rc = kn_fec_reconstruct(shards, slens, 3, 1, 16, scratch, 15, out);
    CHECK(rc == -5, "small scratch rejected");

    /* out buffer missing for an erased index */
    out[1] = NULL;
    rc = kn_fec_reconstruct(shards, slens, 3, 1, 16, scratch, sizeof scratch, out);
    CHECK(rc == -1, "missing out buffer rejected");

    /* shard longer than shard_len */
    shards[0] = buf[0]; slens[0] = 17;
    rc = kn_fec_reconstruct(shards, slens, 3, 1, 16, scratch, sizeof scratch, out);
    CHECK(rc == -1, "oversized shard length rejected");
    slens[0] = 16;

    /* encode argument validation */
    {
        uint8_t *dptr[1], *pptr[1];
        size_t dlens[1];
        dptr[0] = buf[0]; dlens[0] = 16; pptr[0] = buf[1];
        CHECK(kn_fec_encode(NULL, dlens, 1, pptr, 1, 16) == -1, "encode NULL data");
        CHECK(kn_fec_encode((const uint8_t *const *)dptr, dlens, 0, pptr, 1, 16) == -1,
              "encode zero data shards");
        CHECK(kn_fec_encode((const uint8_t *const *)dptr, dlens, 1, pptr, 1, 0) == -1,
              "encode zero shard_len");
        dlens[0] = 17;
        CHECK(kn_fec_encode((const uint8_t *const *)dptr, dlens, 1, pptr, 1, 16) == -1,
              "encode len > shard_len");
    }
}

int main(void)
{
    test_gf_tables();
    test_cauchy_golden();
    test_parity_count();
    test_nvst_reference_vector();
    test_short_shard_padding();
    test_wide_block_encode();
    test_property_roundtrips();
    test_error_paths();
    return kn_test_summary("test_knfec");
}
