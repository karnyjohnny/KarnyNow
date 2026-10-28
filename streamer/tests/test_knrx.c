/* test_knrx.c — unit + property tests for the NVST receive pipeline
 * (kn_rx). The known-answer battery (encoders, NACK/RFI op scripts, SRTP
 * vectors, 31 pipeline scenarios) lives in kn_rx_kat_runner.h — SHARED with
 * the rx.vectors self-test so the shipped binary re-runs the exact same
 * goldens (§7.9). Golden vectors: tools/rx_reference.py →
 * streamer/src/kn_rx_kat_vectors.h (provenance: docs/RX.md).
 *
 * FAULT-TOLERANT BY DESIGN (user instruction 2026-10-24): every section
 * runs independently; failures are counted per check; nothing aborts the
 * run — one execution yields the maximum diagnostic surface even when early
 * sections fail. Sections:
 *   1. the shared KAT battery (verbose failures)
 *   2. reorder buffer units (52 ms deadline, stale/dup, huge jump, pool
 *      accounting)
 *   3. C-side properties (second independent encoder, seeded xorshift):
 *      in-order, healed reordering, FEC-recoverable loss byte-identity,
 *      SRTP CM128 datagram round-trip, dup/stale accounting
 *   4. error paths / init validation
 *
 * Runs on the Linux host and the Win7 target (portable — no platform
 * code). GCM/CM256 vectors light up fully in KN_TLS_BACKEND_MBEDTLS
 * builds (CMake); the bare build checks the honest refusal instead.
 */
#include "kn_rx_kat_runner.h"
#include "test_port.h"

/* -------------------------------------------------- 1. the KAT battery */

static void test_kat_battery(void)
{
    kn_rx_tally t;
    int sp = 0, st = 0;
    kn_rx_kat_run_all(&t, 1, &sp, &st);
    kn_test_checks += t.checks;
    kn_test_failures += t.failures;
    if (t.failures) {
        printf("FAIL kat battery: %d failures (first: %s)\n", t.failures,
               t.first_fail);
    }
    printf("  kat battery: %d checks, %d failures; scenarios %d/%d\n",
           t.checks, t.failures, sp, st);
}

/* ------------------------------------------------- 2. reorder units */

#define RU_SLOTS 32
#define RU_CAP   1400
#define RU_MAP   (1024u + 1u)           /* the deadline scenario runs the
                                          reference DEFAULT_REORDER_WINDOW */

typedef struct {
    kn_rx_pool pool;
    kn_rx_pkt meta[RU_SLOTS];
    uint8_t bytes[RU_SLOTS * RU_CAP];
    kn_rx_reorder ro;
    kn_rx_reorder_entry map[RU_MAP];
} ru_ctx;

static int ru_init(ru_ctx *c, uint32_t window)
{
    memset(c, 0, sizeof *c);
    if (kn_rx_pool_init(&c->pool, c->meta, RU_SLOTS, c->bytes, RU_CAP)
        != KN_RX_OK)
        return 0;
    if (kn_rx_reorder_init(&c->ro, c->map, window + 1u, window) != KN_RX_OK)
        return 0;
    return 1;
}

static uint32_t ru_make(ru_ctx *c, uint64_t index)
{
    /* Minimal RTP packet (no ext) in a fresh slot. The test allocates
     * through the pool's public surface semantics: find an unused slot,
     * mark it used the way the pipeline's copy-in would. */
    uint32_t slot = KN_RX_NO_SLOT, i, next = 0;
    uint8_t *b;
    for (i = 0; i < RU_SLOTS; ++i)
        if (!c->meta[i].used) { slot = i; break; }
    if (slot == KN_RX_NO_SLOT) return KN_RX_NO_SLOT;
    c->meta[slot].used = 1;
    c->pool.in_use++;
    for (i = 0; i < RU_SLOTS; ++i)
        if (!c->meta[i].used) { next = i + 1u; break; }
    c->pool.free_head = next;
    b = c->bytes + (size_t)slot * RU_CAP;
    memset(b, 0, 12);
    b[0] = 0x80; b[1] = 96;
    b[2] = (uint8_t)(index >> 8); b[3] = (uint8_t)index;
    rxt_be32(b + 4, 90000u);
    rxt_be32(b + 8, 1u);
    memset(&c->meta[slot], 0, sizeof c->meta[slot]);
    c->meta[slot].used = 1;
    c->meta[slot].index = index;
    c->meta[slot].len = 12;
    c->meta[slot].origin = 0;
    c->meta[slot].rtp_ok = 1;
    kn_gs_rtp_parse_hdr(b, 12, &c->meta[slot].rtp);
    return slot;
}

/* Emulate the pipeline consuming a result: release every ready slot. */
static void ru_consume(kn_rx_pool *pool, const kn_rx_result *r)
{
    uint32_t i;
    for (i = 0; i < r->ready_len; ++i)
        kn_rx_pool_release(pool, r->ready[i]);
}

static void test_reorder_units(void)
{
    static ru_ctx c;                  /* large: keep off the stack */
    kn_rx_result r;
    uint32_t s;

    if (!ru_init(&c, 1024u)) { CHECK(0, "reorder unit init"); return; }

    /* the reference 52-ms dequeue-deadline scenario (nvst.rs:13341+) */
    s = ru_make(&c, 1);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 1, 0, &r);
    CHECK(r.ready_len == 1 && !r.have_nack && !r.have_recovery,
          "deadline: first ready");
    ru_consume(&c.pool, &r);
    s = ru_make(&c, 3);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 3, 1, &r);
    CHECK(r.ready_len == 0 && r.have_nack && r.nack_first == 2 &&
          r.nack_last == 2 && !r.have_recovery, "deadline: gap nack (2,2)");
    s = ru_make(&c, 4);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 4, 52, &r);
    CHECK(r.ready_len == 0 && r.have_nack && !r.have_recovery,
          "deadline: still waiting at 51ms");
    s = ru_make(&c, 5);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 5, 53, &r);
    CHECK(r.have_recovery && r.rec_first == 2 && r.rec_last == 2,
          "deadline: recovery at 52ms");
    CHECK(r.ready_len == 3 && !r.have_nack, "deadline: ready 3,4,5");
    {
        uint64_t idx[3];
        uint32_t i;
        for (i = 0; i < r.ready_len; ++i)
            idx[i] = c.pool.meta[r.ready[i]].index;
        CHECK(idx[0] == 3 && idx[1] == 4 && idx[2] == 5,
              "deadline: ready order");
    }
    CHECK(c.pool.in_use == 3, "deadline: 3 ready packets held");
    ru_consume(&c.pool, &r);
    CHECK(c.pool.in_use == 0, "deadline: pool empty after consume");

    /* stale */
    s = ru_make(&c, 3);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 3, 60, &r);
    CHECK(r.have_dropped && r.drop_reason == KN_RX_DROP_STALE &&
          r.drop_index == 3, "stale drop");
    CHECK(c.pool.in_use == 0, "stale drop frees its slot");

    /* metadata-only reset keeps held slots */
    s = ru_make(&c, 20);
    kn_rx_reorder_reset(&c.ro);
    CHECK(c.pool.in_use == 1, "held slots survive metadata reset");

    if (!ru_init(&c, 8u)) { CHECK(0, "reorder unit reinit"); return; }
    s = ru_make(&c, 10);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 10, 0, &r);
    CHECK(r.ready_len == 1, "dup: first delivered");
    ru_consume(&c.pool, &r);
    s = ru_make(&c, 12);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 12, 1, &r);
    CHECK(r.ready_len == 0 && r.have_nack && r.nack_first == 11,
          "dup: 12 buffered behind gap");
    s = ru_make(&c, 12);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 12, 2, &r);
    CHECK(r.have_dropped && r.drop_reason == KN_RX_DROP_DUPLICATE,
          "duplicate drop (still buffered)");
    CHECK(c.pool.in_use == 1, "duplicate drop frees its slot");
    s = ru_make(&c, 11);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 11, 3, &r);
    CHECK(r.ready_len == 2 && !r.have_nack, "gap fill delivers the chain");
    ru_consume(&c.pool, &r);

    /* huge jump on an empty buffer (window 4) */
    if (!ru_init(&c, 4u)) { CHECK(0, "reorder unit reinit2"); return; }
    s = ru_make(&c, 1);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 1, 0, &r);
    CHECK(r.ready_len == 1, "jump: first ready");
    ru_consume(&c.pool, &r);
    s = ru_make(&c, 9);
    kn_rx_reorder_push(&c.ro, &c.pool, s, 9, 1, &r);
    CHECK(r.have_recovery && r.rec_first == 2 && r.rec_last == 8 &&
          r.ready_len == 1 && c.pool.meta[r.ready[0]].index == 9,
          "jump: immediate recovery + delivery");
    CHECK(c.pool.in_use == 1, "jump: only the new packet held");
    ru_consume(&c.pool, &r);
    CHECK(c.pool.in_use == 0, "jump: pool drained");

    /* poll on an empty buffer is a quiet no-op */
    kn_rx_reorder_poll(&c.ro, &c.pool, 100, &r);
    CHECK(r.ready_len == 0 && !r.have_nack && !r.have_recovery &&
          !r.have_dropped, "empty poll quiet");
}

/* -------------------------------------------------- 3. C-side properties
 * (the second independent encoder: packets are built HERE from seeds, not
 * taken from the Python generator — an encoder bug cannot fake a pass
 * because the pipeline must reconstruct the same AU bytes the builder
 * produced, and the FEC path must rebuild lost packets from folds computed
 * with knfec, not with the builder.) */

typedef struct {
    uint8_t *pkts[64];
    size_t lens[64];
    uint64_t idx[64];
    int n;
} prop_stream;

static void prop_free(prop_stream *ps)
{
    int i;
    for (i = 0; i < ps->n; ++i) free(ps->pkts[i]);
    ps->n = 0;
}

static void prop_free_parity(uint8_t **par, size_t *parl, uint64_t *pari,
                             int npar)
{
    int q;
    if (par)
        for (q = 0; q < npar; ++q) free(par[q]);
    free(par); free(parl); free(pari);
}

/* Build a plaintext H264 stream: n_frames frames of n_pkts packets; with
 * fec_pct > 0 packets are EXACT shard_len bytes and parity packets are
 * appended to the parity arrays (frame-major). */
static int prop_build(prop_stream *ps, uint32_t seed, int n_frames,
                      int n_pkts, unsigned fec_pct, uint32_t shard_len,
                      uint8_t ***parity_out, size_t **parity_lens_out,
                      uint64_t **parity_idx_out, int *n_parity_out)
{
    uint32_t x = seed | 1u;
    uint64_t index = 1;
    int f, p;
    ps->n = 0;
    if (n_parity_out) *n_parity_out = 0;
    if (parity_out) {
        *parity_out = NULL;
        *parity_lens_out = NULL;
        *parity_idx_out = NULL;
    }
    for (f = 0; f < n_frames; ++f) {
        uint32_t fi = (uint32_t)(f + 1);
        for (p = 0; p < n_pkts; ++p) {
            kn_rx_kat_step st;
            uint8_t media[512];
            size_t mlen, blen, plen;
            uint8_t *buf;
            int flags = KN_GS_FLAG_PIC;
            if (p == 0) flags |= KN_GS_FLAG_SOF;
            if (p == n_pkts - 1) flags |= KN_GS_FLAG_EOF;
            if (fec_pct) {
                blen = shard_len - 32u - ((p == 0) ? 8u : 4u);
            } else {
                x = rxt_xorshift(x);
                blen = 8u + (x % 40u);
            }
            memset(&st, 0, sizeof st);
            st.seq = (uint32_t)(index & 0xFFFFu);
            st.ts = 1000u + fi * 3000u;
            st.ssrc = 0x11223344u;
            st.pt = 96;
            st.flags = (unsigned)flags;
            st.spi = (unsigned)(index & KN_GS_INDEX_MASK);
            st.fi = fi;
            st.pct = fec_pct;
            st.fidx = (unsigned)p;
            st.fsrc = (unsigned)n_pkts;
            x = rxt_xorshift(x);
            mlen = rxt_media(media, p == 0 ? (f == 0 ? 2 : 1) : -1,
                             (p == 0 && f == 0) ? 5u : 1u, x,
                             (uint32_t)blen, 0);
            buf = (uint8_t *)malloc(2048);
            if (!buf) { prop_free(ps); return 0; }
            plen = rxt_pkt(buf, &st, media, mlen);
            ps->pkts[ps->n] = buf;
            ps->lens[ps->n] = plen;
            ps->idx[ps->n] = index;
            ps->n++;
            index++;
        }
        if (fec_pct && parity_out) {
            unsigned npar = rxt_parity_count((unsigned)n_pkts, fec_pct);
            unsigned q;
            if (!*parity_out) {
                *parity_out = (uint8_t **)calloc(64, sizeof(uint8_t *));
                *parity_lens_out = (size_t *)calloc(64, sizeof(size_t));
                *parity_idx_out = (uint64_t *)calloc(64, sizeof(uint64_t));
                if (!*parity_out) { prop_free(ps); return 0; }
            }
            for (q = 0; q < npar; ++q) {
                kn_rx_kat_step pst;
                uint8_t *refs[12];
                size_t reflens[12];
                uint8_t *buf = (uint8_t *)malloc((size_t)shard_len + 64u);
                int k;
                if (!buf || *n_parity_out >= 64) { free(buf); continue; }
                for (k = 0; k < n_pkts; ++k) {
                    int gid = (f * n_pkts + k);
                    refs[k] = ps->pkts[gid];
                    reflens[k] = ps->lens[gid];
                }
                memset(&pst, 0, sizeof pst);
                pst.seq = (uint32_t)(index & 0xFFFFu);
                pst.ts = 1000u + fi * 3000u;
                pst.ssrc = 0x11223344u;
                pst.pt = 96;
                pst.fi = fi;
                pst.pct = fec_pct;
                pst.d_shards = (unsigned)n_pkts;
                pst.p_idx = q;
                pst.n_refs = n_pkts;
                pst.shard_len = shard_len;
                (*parity_lens_out)[*n_parity_out] =
                    rxt_parity(buf, &pst, refs, reflens, shard_len);
                (*parity_out)[*n_parity_out] = buf;
                (*parity_idx_out)[*n_parity_out] = index;
                (*n_parity_out)++;
                index++;
            }
        }
    }
    return 1;
}

typedef struct {
    kn_rx_video v;
    kn_rx_mem mem;
    uint8_t *arena;
    rxt_col col;
} prop_h;

static int prop_h_init(prop_h *h, uint32_t window, uint32_t vps,
                       uint32_t fec_blocks, uint32_t fec_shards)
{
    kn_rx_video_config cfg;
    uint32_t shard_len = vps + KN_RX_FEC_HEADER_ALLOWANCE;
    uint32_t pkt_cap = shard_len + 64u;
    uint32_t slots = window + 1u + fec_blocks * fec_shards + 16u;
    uint32_t scratch = ((uint32_t)kn_fec_scratch_bytes(fec_shards, shard_len)
                        + 4096u + 7u) & ~7u;   /* keep the arena 8-aligned */
    size_t need = (size_t)slots * sizeof(kn_rx_pkt) + (size_t)slots * pkt_cap
                + (size_t)(window + 1u) * sizeof(kn_rx_reorder_entry)
                + (size_t)fec_blocks * sizeof(kn_rx_fec_block)
                + (size_t)fec_blocks * fec_shards * sizeof(uint32_t)
                + scratch + KN_RX_NACK_MAX_PENDING * sizeof(kn_rx_nack_entry)
                + 65536u + 16u * sizeof(kn_rx_event);
    memset(h, 0, sizeof *h);
    h->arena = (uint8_t *)malloc(need);
    if (!h->arena) return 0;
    {
        size_t off = 0;
        h->mem.pool_meta = (kn_rx_pkt *)(void *)(h->arena + off);
        off += (size_t)slots * sizeof(kn_rx_pkt);
        h->mem.pool_bytes = h->arena + off;
        off += (size_t)slots * pkt_cap;
        h->mem.pool_slots = slots;
        h->mem.pool_pkt_cap = pkt_cap;
        h->mem.reorder_map = (kn_rx_reorder_entry *)(void *)(h->arena + off);
        off += (size_t)(window + 1u) * sizeof(kn_rx_reorder_entry);
        h->mem.fec_blocks = (kn_rx_fec_block *)(void *)(h->arena + off);
        off += (size_t)fec_blocks * sizeof(kn_rx_fec_block);
        h->mem.fec_shard_arena = (uint32_t *)(void *)(h->arena + off);
        off += (size_t)fec_blocks * fec_shards * sizeof(uint32_t);
        h->mem.fec_max_blocks = fec_blocks;
        h->mem.fec_max_shards = fec_shards;
        h->mem.fec_scratch = h->arena + off;
        off += scratch;
        h->mem.fec_scratch_len = scratch;
        h->mem.nack_table = (kn_rx_nack_entry *)(void *)(h->arena + off);
        off += KN_RX_NACK_MAX_PENDING * sizeof(kn_rx_nack_entry);
        h->mem.nack_capacity = KN_RX_NACK_MAX_PENDING;
        h->mem.au_buffer = h->arena + off;
        off += 65536u;
        h->mem.au_buffer_capacity = 65536u;
        h->mem.events = (kn_rx_event *)(void *)(h->arena + off);
        h->mem.events_capacity = 16;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.codec = KN_GS_CODEC_H264;
    cfg.reorder_window_packets = window;
    cfg.max_access_unit_bytes = 65536u;
    cfg.video_packet_size = vps;
    cfg.timeout_ms = KN_RX_MAX_TIMEOUT_MS;
    return kn_rx_video_init(&h->v, &cfg, &h->mem, 0) == KN_RX_OK;
}

static void prop_h_free(prop_h *h)
{
    free(h->arena);
    memset(h, 0, sizeof *h);
}

static void test_properties(void)
{
    uint32_t trial;
    uint32_t seed = 0xBEEF0001u;
    int fails_inorder = 0, fails_reorder = 0, fails_fec = 0, fails_srtp = 0,
        fails_dupstale = 0;

    /* P1: in-order streams — every frame emitted, contiguous, keyframe
     * flags correct */
    for (trial = 0; trial < 30; ++trial) {
        prop_stream ps;
        prop_h h;
        int i;
        int n_frames = 1 + (int)(trial % 4);
        int n_pkts = 1 + (int)((trial / 2) % 4);
        seed = rxt_xorshift(seed);
        memset(&ps, 0, sizeof ps);
        memset(&h, 0, sizeof h);
        if (!prop_build(&ps, seed, n_frames, n_pkts, 0, 0,
                        NULL, NULL, NULL, NULL)) {
            CHECK(0, "P1 build");
            break;
        }
        if (!prop_h_init(&h, 64, KN_RX_DEFAULT_VIDEO_PACKET_SIZE, 4, 16)) {
            CHECK(0, "P1 init");
            prop_free(&ps);
            break;
        }
        for (i = 0; i < ps.n; ++i)
            kn_rx_video_process_packet(&h.v, ps.idx[i], ps.pkts[i],
                                       ps.lens[i], (uint64_t)(1000 + i),
                                       rxt_frame_cb, &h.col);
        {
            int good = (h.col.n == n_frames);
            for (i = 0; good && i < h.col.n; ++i) {
                if (h.col.frames[i].fi != (uint32_t)(i + 1)) good = 0;
                if (!h.col.frames[i].contig) good = 0;
                if (i == 0 && !h.col.frames[i].kf) good = 0;
                if (i > 0 && h.col.frames[i].kf) good = 0;
            }
            if (!good) fails_inorder++;
        }
        prop_h_free(&h);
        prop_free(&ps);
    }
    CHECK(fails_inorder == 0, "P1 in-order streams (30 trials)");

    /* P2: healed reordering — adjacent swaps keep every frame identical */
    for (trial = 0; trial < 20; ++trial) {
        prop_stream ps;
        prop_h h1, h2;
        int i, order[64], n;
        seed = rxt_xorshift(seed);
        memset(&ps, 0, sizeof ps);
        memset(&h1, 0, sizeof h1);
        memset(&h2, 0, sizeof h2);
        if (!prop_build(&ps, seed, 3, 3, 0, 0, NULL, NULL, NULL, NULL)) {
            CHECK(0, "P2 build");
            break;
        }
        n = ps.n;
        for (i = 0; i < n; ++i) order[i] = i;
        /* swap adjacent pairs except at position 0 (the first arrival must
         * stay the minimum or later packets go legitimately stale) */
        for (i = 1; i + 1 < n; i += 2) {
            int t = order[i];
            order[i] = order[i + 1];
            order[i + 1] = t;
        }
        if (!prop_h_init(&h1, 64, KN_RX_DEFAULT_VIDEO_PACKET_SIZE, 4, 16) ||
            !prop_h_init(&h2, 64, KN_RX_DEFAULT_VIDEO_PACKET_SIZE, 4, 16)) {
            CHECK(0, "P2 init");
            prop_free(&ps);
            prop_h_free(&h1);
            prop_h_free(&h2);
            break;
        }
        for (i = 0; i < n; ++i)
            kn_rx_video_process_packet(&h1.v, ps.idx[i], ps.pkts[i],
                                       ps.lens[i], (uint64_t)(1000 + i),
                                       rxt_frame_cb, &h1.col);
        for (i = 0; i < n; ++i) {
            int k = order[i];
            kn_rx_video_process_packet(&h2.v, ps.idx[k], ps.pkts[k],
                                       ps.lens[k], (uint64_t)(1000 + i),
                                       rxt_frame_cb, &h2.col);
        }
        {
            int good = (h1.col.n == h2.col.n && h1.col.n == 3 &&
                        h2.v.stale_packets == 0 &&
                        h2.v.packet_gap_recoveries == 0);
            for (i = 0; good && i < h1.col.n; ++i)
                if (memcmp(&h1.col.frames[i], &h2.col.frames[i],
                           sizeof(rxt_frame)) != 0)
                    good = 0;
            if (!good) fails_reorder++;
        }
        prop_h_free(&h1);
        prop_h_free(&h2);
        prop_free(&ps);
    }
    CHECK(fails_reorder == 0, "P2 healed reordering (20 trials)");

    /* P3: FEC-recoverable loss — frame bytes identical to the baseline and
     * the repaired counter exact. Data packets are exact shard_len bytes so
     * a repaired shard is byte-identical to the original. */
    for (trial = 0; trial < 24; ++trial) {
        prop_stream ps;
        prop_h hb, hl;
        int npar = 0, i;
        uint8_t **par = NULL;
        size_t *parl = NULL;
        uint64_t *pari = NULL;
        unsigned pct;
        static const unsigned pcts[4] = { 25u, 50u, 100u, 200u };
        int n_pkts, lose, n_frames = 2;
        uint32_t shard_len;
        seed = rxt_xorshift(seed);
        n_pkts = 1 + (int)(trial % 4);
        pct = pcts[trial % 4];
        shard_len = 256u + KN_RX_FEC_HEADER_ALLOWANCE;   /* protocol min */
        {
            unsigned npar_frame = rxt_parity_count((unsigned)n_pkts, pct);
            lose = 1 + (int)(trial % npar_frame);
            if (lose > n_pkts) lose = n_pkts;
        }
        memset(&ps, 0, sizeof ps);
        memset(&hb, 0, sizeof hb);
        memset(&hl, 0, sizeof hl);
        if (!prop_build(&ps, seed, n_frames, n_pkts, pct, shard_len,
                        &par, &parl, &pari, &npar)) {
            CHECK(0, "P3 build");
            break;
        }
        if (!prop_h_init(&hb, 64, 256, 8, 16) ||
            !prop_h_init(&hl, 64, 256, 8, 16)) {
            CHECK(0, "P3 init");
            prop_free(&ps);
            prop_free_parity(par, parl, pari, npar);
            prop_h_free(&hb);
            prop_h_free(&hl);
            break;
        }
        {   /* baseline: everything delivered in order */
            int pi = 0;
            for (i = 0; i < ps.n; ++i) {
                kn_rx_video_process_packet(&hb.v, ps.idx[i], ps.pkts[i],
                                           ps.lens[i], (uint64_t)(1000 + i),
                                           rxt_frame_cb, &hb.col);
                if ((i + 1) % n_pkts == 0) {
                    unsigned q, np = rxt_parity_count((unsigned)n_pkts, pct);
                    for (q = 0; q < np && pi < npar; ++q, ++pi)
                        kn_rx_video_process_packet(&hb.v, pari[pi], par[pi],
                                                   parl[pi],
                                                   (uint64_t)(1000 + i),
                                                   rxt_frame_cb, &hb.col);
                }
            }
        }
        {   /* lossy: drop `lose` data packets of frame 1 (FEC must repair) */
            int pi = 0;
            for (i = 0; i < ps.n; ++i) {
                int drop = (i < n_pkts) && (i % n_pkts) < lose;
                if (!drop)
                    kn_rx_video_process_packet(&hl.v, ps.idx[i], ps.pkts[i],
                                               ps.lens[i],
                                               (uint64_t)(1000 + i),
                                               rxt_frame_cb, &hl.col);
                if ((i + 1) % n_pkts == 0) {
                    unsigned q, np = rxt_parity_count((unsigned)n_pkts, pct);
                    for (q = 0; q < np && pi < npar; ++q, ++pi)
                        kn_rx_video_process_packet(&hl.v, pari[pi], par[pi],
                                                   parl[pi],
                                                   (uint64_t)(1000 + i),
                                                   rxt_frame_cb, &hl.col);
                }
            }
        }
        {
            int good = (hb.col.n == n_frames && hl.col.n == n_frames);
            if (!good && kn_test_failures == 0)
                printf("  P3 trial %u: frame counts %d/%d (want %d)\n",
                       trial, hb.col.n, hl.col.n, n_frames);
            for (i = 0; good && i < hb.col.n; ++i)
                if (memcmp(&hb.col.frames[i], &hl.col.frames[i],
                           sizeof(rxt_frame)) != 0) {
                    if (fails_fec == 0)
                        printf("  P3 trial %u frame %d mismatch: len %u/%u "
                               "hash %016llx/%016llx contig %d/%d\n",
                               trial, i,
                               hb.col.frames[i].len, hl.col.frames[i].len,
                               (unsigned long long)hb.col.frames[i].hash,
                               (unsigned long long)hl.col.frames[i].hash,
                               hb.col.frames[i].contig,
                               hl.col.frames[i].contig);
                    good = 0;
                }
            if (good && hl.v.fec_repaired_packets != (uint64_t)lose) {
                if (fails_fec == 0)
                    printf("  P3 trial %u: repaired %llu != lose %d\n",
                           trial,
                           (unsigned long long)hl.v.fec_repaired_packets,
                           lose);
                good = 0;
            }
            if (good && hl.col.frames[0].contig != 1)
                good = 0;      /* fully repaired -> contiguous */
            if (!good) fails_fec++;
        }
        prop_h_free(&hb);
        prop_h_free(&hl);
        prop_free(&ps);
        prop_free_parity(par, parl, pari, npar);
    }
    CHECK(fails_fec == 0, "P3 FEC-recoverable loss byte-identity (24 trials)");

    /* P4: SRTP CM128 datagrams — protect in C, feed process_datagram,
     * frames identical to the plaintext run */
    for (trial = 0; trial < 12; ++trial) {
        prop_stream ps;
        prop_h hp, hd;
        kn_rx_srtp_session s;
        uint8_t master[16], salt[14];
        int i;
        seed = rxt_xorshift(seed);
        memset(&ps, 0, sizeof ps);
        memset(&hp, 0, sizeof hp);
        memset(&hd, 0, sizeof hd);
        if (!prop_build(&ps, seed, 2, 3, 0, 0, NULL, NULL, NULL, NULL)) {
            CHECK(0, "P4 build");
            break;
        }
        for (i = 0; i < 16; ++i)
            master[i] = (uint8_t)(seed >> ((unsigned)i % 4u * 8u));
        for (i = 0; i < 14; ++i) salt[i] = (uint8_t)(0xA0 + i);
        if (kn_rx_srtp_session_init(&s, KN_RX_SRTP_CM128_HMAC_SHA1, master,
                                    16, salt, 14, 10) != KN_RX_OK) {
            CHECK(0, "P4 session");
            prop_free(&ps);
            break;
        }
        if (!prop_h_init(&hp, 64, KN_RX_DEFAULT_VIDEO_PACKET_SIZE, 4, 16) ||
            !prop_h_init(&hd, 64, KN_RX_DEFAULT_VIDEO_PACKET_SIZE, 4, 16)) {
            CHECK(0, "P4 init");
            prop_free(&ps);
            prop_h_free(&hp);
            prop_h_free(&hd);
            break;
        }
        kn_rx_video_set_srtp(&hd.v, &s);
        for (i = 0; i < ps.n; ++i) {
            static uint8_t dgram[2112];
            size_t dlen = ps.lens[i];
            kn_gs_rtp h;
            int rc;
            kn_rx_video_process_packet(&hp.v, ps.idx[i], ps.pkts[i],
                                       ps.lens[i], (uint64_t)(1000 + i),
                                       rxt_frame_cb, &hp.col);
            memcpy(dgram, ps.pkts[i], dlen);
            if (kn_gs_rtp_parse_hdr(dgram, dlen, &h) != KN_GS_OK) {
                fails_srtp++;
                continue;
            }
            rc = kn_srtp_protect_cm_hmac(dgram, dlen, h.payload_off, h.ssrc,
                                         ps.idx[i], s.enc_key, s.auth_key,
                                         s.salt, s.tag_len);
            if (rc != 0) { fails_srtp++; continue; }
            kn_rx_video_process_datagram(&hd.v, dgram, dlen + s.tag_len,
                                         (uint64_t)(1000 + i),
                                         rxt_frame_cb, &hd.col);
        }
        {
            int good = (hp.col.n == hd.col.n && hp.col.n == 2 &&
                        hd.v.authenticated_packets == (uint64_t)ps.n);
            for (i = 0; good && i < hp.col.n; ++i)
                if (memcmp(&hp.col.frames[i], &hd.col.frames[i],
                           sizeof(rxt_frame)) != 0)
                    good = 0;
            if (!good) fails_srtp++;
        }
        prop_h_free(&hp);
        prop_h_free(&hd);
        prop_free(&ps);
    }
    CHECK(fails_srtp == 0, "P4 SRTP CM128 datagram round-trip (12 trials)");

    /* P5: duplicate + stale accounting */
    for (trial = 0; trial < 10; ++trial) {
        prop_stream ps;
        prop_h h;
        int i;
        seed = rxt_xorshift(seed);
        memset(&ps, 0, sizeof ps);
        memset(&h, 0, sizeof h);
        if (!prop_build(&ps, seed, 2, 2, 0, 0, NULL, NULL, NULL, NULL)) {
            CHECK(0, "P5 build");
            break;
        }
        if (!prop_h_init(&h, 64, KN_RX_DEFAULT_VIDEO_PACKET_SIZE, 4, 16)) {
            CHECK(0, "P5 init");
            prop_free(&ps);
            break;
        }
        for (i = 0; i < ps.n; ++i) {
            kn_rx_video_process_packet(&h.v, ps.idx[i], ps.pkts[i],
                                       ps.lens[i], (uint64_t)(1000 + i),
                                       rxt_frame_cb, &h.col);
            if (i == 1) {     /* replay a delivered packet -> stale */
                kn_rx_video_process_packet(&h.v, ps.idx[0], ps.pkts[0],
                                           ps.lens[0],
                                           (uint64_t)(1000 + i),
                                           rxt_frame_cb, &h.col);
            }
        }
        {
            int good = (h.col.n == 2 && h.v.stale_packets == 1 &&
                        h.v.frames_emitted == 2);
            if (!good) fails_dupstale++;
        }
        prop_h_free(&h);
        prop_free(&ps);
    }
    CHECK(fails_dupstale == 0, "P5 stale accounting (10 trials)");
}

/* ------------------------------------------------------- 4. error paths */

static void test_error_paths(void)
{
    kn_rx_video v;
    kn_rx_video_config cfg;
    kn_rx_mem mem;
    kn_rx_pkt pm[8];
    static uint8_t pbytes[8 * 1400];
    kn_rx_reorder_entry rmap[5];
    kn_rx_fec_block fb[2];
    uint32_t fsh[2 * 4];
    static uint8_t scratch[70000];
    kn_rx_nack_entry nt[4];
    static uint8_t au[4096];
    kn_rx_event evs[4];
    kn_rx_srtp_session s;
    uint8_t out[8];

    memset(&cfg, 0, sizeof cfg);
    cfg.codec = KN_GS_CODEC_H264;
    cfg.reorder_window_packets = 4;
    cfg.max_access_unit_bytes = 4096;
    cfg.video_packet_size = 1280;
    cfg.timeout_ms = 5000;
    memset(&mem, 0, sizeof mem);
    mem.pool_meta = pm; mem.pool_slots = 8;
    mem.pool_bytes = pbytes; mem.pool_pkt_cap = 1400;
    mem.reorder_map = rmap;
    mem.fec_blocks = fb; mem.fec_max_blocks = 2;
    mem.fec_shard_arena = fsh; mem.fec_max_shards = 4;
    mem.fec_scratch = scratch;
    mem.fec_scratch_len = kn_fec_scratch_bytes(4, 1296) + 64;
    mem.nack_table = nt; mem.nack_capacity = 4;
    mem.au_buffer = au; mem.au_buffer_capacity = 4096;
    mem.events = evs; mem.events_capacity = 4;

    CHECK(kn_rx_video_init(NULL, &cfg, &mem, 0) == KN_RX_ERR_ARGS,
          "init NULL video");
    CHECK(kn_rx_video_init(&v, NULL, &mem, 0) == KN_RX_ERR_ARGS,
          "init NULL cfg");
    CHECK(kn_rx_video_init(&v, &cfg, &mem, 0) == KN_RX_OK, "init ok");

    {
        kn_rx_video_config bad = cfg;
        bad.reorder_window_packets = KN_RX_MAX_REORDER_WINDOW + 1u;
        CHECK(kn_rx_video_init(&v, &bad, &mem, 0) == KN_RX_ERR_STATE,
              "init window too big");
        bad = cfg;
        bad.video_packet_size = 100;
        CHECK(kn_rx_video_init(&v, &bad, &mem, 0) == KN_RX_ERR_STATE,
              "init vps too small");
        bad = cfg;
        bad.timeout_ms = 10;
        CHECK(kn_rx_video_init(&v, &bad, &mem, 0) == KN_RX_ERR_STATE,
              "init timeout below min");
        bad = cfg;
        bad.max_access_unit_bytes = 8192;      /* > buffer capacity */
        CHECK(kn_rx_video_init(&v, &bad, &mem, 0) == KN_RX_ERR_ARGS,
              "init au > buffer");
        bad = cfg;
        bad.codec = 9;
        CHECK(kn_rx_video_init(&v, &bad, &mem, 0) == KN_RX_ERR_ARGS,
              "init bad codec");
    }
    {
        kn_rx_mem bad = mem;
        bad.pool_pkt_cap = 100;                /* < shard_len 1296 */
        CHECK(kn_rx_video_init(&v, &cfg, &bad, 0) == KN_RX_ERR_STATE,
              "init pkt_cap < shard_len");
        bad = mem;
        bad.events_capacity = 2;
        CHECK(kn_rx_video_init(&v, &cfg, &bad, 0) == KN_RX_ERR_ARGS,
              "init events too few");
    }

    CHECK(kn_rx_video_init(&v, &cfg, &mem, 0) == KN_RX_OK, "re-init ok");
    CHECK(kn_rx_video_process_packet(&v, 1, NULL, 8, 0, NULL, NULL)
          == KN_RX_ERR_ARGS, "process_packet NULL");
    CHECK(kn_rx_video_process_datagram(&v, NULL, 8, 0, NULL, NULL)
          == KN_RX_ERR_ARGS, "process_datagram NULL");
    CHECK(kn_rx_reorder_init(NULL, rmap, 5, 4) == KN_RX_ERR_ARGS,
          "reorder init NULL");
    CHECK(kn_rx_reorder_init(&v.reorder, rmap, 4, 4) == KN_RX_ERR_ARGS,
          "reorder map too small");
    CHECK(kn_rx_nack_init(&v.nack, nt, 0) == KN_RX_ERR_ARGS,
          "nack init cap 0");
    CHECK(kn_rx_nack_init(&v.nack, nt, KN_RX_NACK_MAX_PENDING + 1u)
          == KN_RX_ERR_ARGS, "nack init cap too big");
    CHECK(kn_rx_fec_init(&v.fec, fb, 2, fsh, 1, 1280, scratch, 70000)
          == KN_RX_ERR_ARGS, "fec init shards < 2");
    CHECK(kn_rx_fec_init(&v.fec, fb, 2, fsh, KN_RX_FEC_MAX_SHARDS + 1u,
                         1280, scratch, 70000) == KN_RX_ERR_ARGS,
          "fec init shards > 255");

    {
        uint8_t mk[32] = {0}, ms[14] = {0};
        CHECK(kn_rx_srtp_session_init(NULL, KN_RX_SRTP_CM128_HMAC_SHA1,
                                      mk, 16, ms, 14, 10) == KN_RX_ERR_ARGS,
              "session NULL");
        CHECK(kn_rx_srtp_session_init(&s, KN_RX_SRTP_CM128_HMAC_SHA1,
                                      mk, 16, ms, 14, 7) == KN_RX_ERR_ARGS,
              "session bad tag len");
        CHECK(kn_rx_srtp_session_init(&s, KN_RX_SRTP_CM128_HMAC_SHA1,
                                      mk, 32, ms, 14, 10) == KN_RX_ERR_ARGS,
              "session bad key len");
        CHECK(kn_rx_srtp_session_init(&s, 42, mk, 16, ms, 14, 10)
              == KN_RX_ERR_ARGS, "session bad profile");
        CHECK(kn_rx_srtp_session_init(&s, KN_RX_SRTP_NONE, NULL, 0, NULL, 0,
                                      0) == KN_RX_OK, "session NONE ok");
        if (!kn_srtp_backend_gcm()) {
            CHECK(kn_rx_srtp_session_init(&s, KN_RX_SRTP_GCM128, mk, 16, ms,
                                          12, 8) == KN_RX_ERR_NO_CIPHER,
                  "session GCM honest refusal (bare build)");
            CHECK(kn_rx_srtp_session_init(&s, KN_RX_SRTP_CM256_HMAC_SHA1, mk,
                                          32, ms, 14, 10)
                  == KN_RX_ERR_NO_CIPHER,
                  "session CM256 honest refusal (bare build)");
        }
    }
    CHECK(kn_rx_ctrl_nack_v2(0, NULL, 0, out, sizeof out) == 0,
          "nack_v2 NULL missing -> 0");
    CHECK(kn_rx_ctrl_idr_request(NULL) == -1, "idr NULL");
    CHECK(kn_rx_rtcp_nack(1, 2, 5, 5, out, 4) == 0, "rtcp nack small buf");
    {
        kn_rx_qos q;
        uint8_t qos_out[4 + KN_RX_CTRL_QOS_PAYLOAD_LEN];
        memset(&q, 0, sizeof q);
        CHECK(kn_rx_ctrl_qos_report(NULL, qos_out) == -1, "qos NULL q");
        CHECK(kn_rx_ctrl_qos_report(&q, NULL) == -1, "qos NULL out");
    }
    {
        uint32_t n = 123;
        CHECK(kn_rx_video_events(NULL, &n) == NULL && n == 0,
              "events NULL video");
    }
    {   /* round XXXVI: the RTCP RR builder — goldens generated by an
         * independent Python transcription of the reference's
         * build_rtcp_receiver_report (nvst.rs:2983-2997): 0x81/201/
         * length-7 head, sender "ONOW" (RTCP_SENDER_SSRC :96), the
         * 24-bit two's-complement cumulative loss WITH the reference's
         * clamp (:1233), LSR/DLSR zeros (:2994-2996). */
        uint8_t rr[32];
        static const uint8_t g1[32] = {
            0x81, 0xc9, 0x00, 0x07, 0x4f, 0x4e, 0x4f, 0x57,
            0xde, 0xad, 0xbe, 0xef, 0x2a, 0xff, 0xff, 0xfb,
            0x00, 0x01, 0x23, 0x45, 0x00, 0x00, 0x03, 0x09,
            0, 0, 0, 0, 0, 0, 0, 0 };
        static const uint8_t g2[32] = {
            0x81, 0xc9, 0x00, 0x07, 0x4f, 0x4e, 0x4f, 0x57,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x7f, 0xff, 0xff,
            0x00, 0x00, 0x00, 0x01, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0 };
        static const uint8_t g3[32] = {
            0x81, 0xc9, 0x00, 0x07, 0x4f, 0x4e, 0x4f, 0x57,
            0x00, 0x00, 0x00, 0x01, 0xff, 0x80, 0x00, 0x00,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0, 0, 0, 0, 0, 0, 0, 0 };
        kn_rx_rtcp_rr(0x4F4E4F57u, 0xDEADBEEFu, 42, -5, 0x00012345u,
                      777u, rr);
        CHECK(memcmp(rr, g1, 32) == 0,
              "RR golden: negative cumulative -> 24-bit two's complement, "
              "jitter/highest exact, LSR=DLSR=0");
        kn_rx_rtcp_rr(0x4F4E4F57u, 1u, 0, 0x7FFFFF + 100, 1u, 0u, rr);
        CHECK(memcmp(rr, g2, 32) == 0,
              "RR clamps cumulative to +0x7FFFFF (report_snapshot :1233)");
        kn_rx_rtcp_rr(0x4F4E4F57u, 1u, 255, -0x800000 - 1, 0xFFFFFFFFu,
                      0xFFFFFFFFu, rr);
        CHECK(memcmp(rr, g3, 32) == 0,
              "RR clamps to -0x800000 and keeps max highest/jitter");
        CHECK(kn_rx_video_rr_snapshot(NULL, NULL, NULL, NULL, NULL, NULL) == 0,
              "rr_snapshot is NULL-safe (and 0 before the stream binds)");
    }

    /* round XVII: the external keyframe-request API (--decode's encoded
     * queue overflow calls it; same flag/counter discipline as the
     * internal loss paths — analiza/04 §5 queue-overflow row). */
    {
        uint64_t reqs0 = v.keyframe_requests;
        CHECK(kn_rx_video_take_keyframe_request(&v) == 0,
              "fresh pipeline has no pending keyframe request");
        kn_rx_video_request_keyframe(&v);
        CHECK(kn_rx_video_take_keyframe_request(&v) == 1,
              "request_keyframe sets the flag (take returns 1)");
        CHECK(kn_rx_video_take_keyframe_request(&v) == 0,
              "take-and-clear contract (second take returns 0)");
        kn_rx_video_request_keyframe(&v);
        kn_rx_video_request_keyframe(&v);
        CHECK(kn_rx_video_take_keyframe_request(&v) == 1,
              "repeated requests collapse into one flag");
        CHECK(v.keyframe_requests == reqs0 + 2,
              "counter counts SETS, not repeats (internal discipline)");
        kn_rx_video_request_keyframe(NULL);
        CHECK(1, "request_keyframe(NULL) survives");
    }
}

/* --------------------------- 5. round XL: feedback-plane semantics */

/* D4/D6 goldens. The stream: prop_build with 4 frames x 1 packet, so the
 * per-frame RTP timestamp advances 3000 ticks (prop_build: ts = 1000 +
 * fi*3000 → 4000, 7000, 10000, 13000) and every packet completes its
 * frame. Fed with now_ms = 1000 + 10*i (arrival advances 10 ms/packet =
 * 900 ticks). The jitter machine (kn_rx.c, RFC 3550 port of the
 * reference's timing EWMA) computes, with transit RELATIVE to the first
 * packet (first packet latches only; last_transit starts at 0):
 *   pkt1: arrival=(1010-1000)*90=900   rtp=7000-4000=3000
 *         transit=-2100  diff=-2100  delta=2100  J=0+(2100-0)/16=131.25
 *   pkt2: arrival=1800   rtp=6000  transit=-4200  diff=-2100  delta=2100
 *         J=131.25+(2100-131.25)/16=254.296875
 *   pkt3: arrival=2700   rtp=9000  transit=-6300  diff=-2100  delta=2100
 *         J=254.296875+(2100-254.296875)/16=369.60546875
 * rr_snapshot truncates to u32 → 369. A steady-rate stream would keep
 * delta=0 after the first step, so this golden pins the EWMA divisor,
 * the relative-transit baseline AND the truncation in one number.
 * first_packet_ms must equal the wall clock of each frame's first (here:
 * only) packet — the reference's CompletedFrameFeedback timing.first_at
 * that frame_ack serializes as first_packet_time_ms (nvst.rs:447-462). */
static void test_round_xl_feedback(void)
{
    prop_stream ps;
    prop_h h;
    int i, fpm_ok = 1;
    uint32_t ssrc = 0, base = 0, high = 0, jit = 0;
    uint64_t recv = 0;
    memset(&ps, 0, sizeof ps);
    memset(&h, 0, sizeof h);
    if (!prop_build(&ps, 0xD0D0D0D1u, 4, 1, 0, 0, NULL, NULL, NULL, NULL)) {
        CHECK(0, "XL build");
        return;
    }
    if (!prop_h_init(&h, 64, KN_RX_DEFAULT_VIDEO_PACKET_SIZE, 4, 16)) {
        CHECK(0, "XL init");
        prop_free(&ps);
        return;
    }
    CHECK(kn_rx_video_rr_snapshot(&h.v, &ssrc, &base, &high, &recv,
                                  &jit) == 0,
          "rr_snapshot is 0 before the stream binds");
    for (i = 0; i < ps.n; ++i)
        kn_rx_video_process_packet(&h.v, ps.idx[i], ps.pkts[i], ps.lens[i],
                                   (uint64_t)(1000 + 10 * i),
                                   rxt_frame_cb, &h.col);
    CHECK(h.col.n == 4, "XL: all four frames emitted");
    CHECK(kn_rx_video_rr_snapshot(&h.v, &ssrc, &base, &high, &recv,
                                  &jit) == 1,
          "rr_snapshot reports once the ssrc binds");
    CHECK(ssrc == 0x11223344u && recv == 4, "XL: ssrc/received snapshot");
    CHECK(jit == 369u, "XL jitter golden: 369 (EWMA/16 of delta 2100)");
    for (i = 0; i < h.col.n; ++i)
        if (h.col.first_ms[i] != (uint64_t)(1000 + 10 * i))
            fpm_ok = 0;
    CHECK(fpm_ok, "XL: first_packet_ms = wall clock of the frame's first "
                  "packet (frame_ack first_packet_time_ms source)");
    /* D3's latch read: pending must be a NON-clearing view, and the
     * take clears — dec_kf_service polls pending every 100 ms and must
     * see the request until an assembled keyframe clears it. */
    CHECK(kn_rx_video_keyframe_request_pending(&h.v) == 0,
          "XL: no keyframe request pending on a clean stream");
    kn_rx_video_request_keyframe(&h.v);
    CHECK(kn_rx_video_keyframe_request_pending(&h.v) == 1 &&
          kn_rx_video_keyframe_request_pending(&h.v) == 1,
          "XL: pending is non-clearing (the retry loop re-reads it)");
    CHECK(kn_rx_video_take_keyframe_request(&h.v) == 1 &&
          kn_rx_video_keyframe_request_pending(&h.v) == 0,
          "XL: take clears the latch");
    prop_h_free(&h);
    prop_free(&ps);
}

/* --------------------------------------------------------------- main */

int main(void)
{
    printf("== kn_rx KAT + property suite ==\n");
    printf("-- kat battery (shared with the rx.vectors self-test) --\n");
    test_kat_battery();
    printf("-- reorder units --\n");
    test_reorder_units();
    printf("-- properties --\n");
    test_properties();
    printf("-- error paths --\n");
    test_error_paths();
    printf("-- round XL feedback plane --\n");
    test_round_xl_feedback();
    return kn_test_summary("test_knrx");
}
