/* kn_rx.c — NVST video receive pipeline (stage-2 transport core).
 *
 * Zero-allocation C port of the pure media path of OpenNOW's
 * NvstVideoReceiver (nvst.rs @ a49b124). Section map:
 *   1. helpers + packet pool
 *   2. RtpReorderBuffer               (nvst.rs:3537-3660)
 *   3. FecBlock + FecReorderBuffer    (nvst.rs:3925-4310)
 *   4. FrameInvalidation (RFI)        (nvst_invalidation.rs:118-269)
 *   5. NACK tracker                   (nvst.rs:946-985, 1272-1333)
 *   6. control + RTCP encoders        (nvst_control.rs:3-181, nvst.rs:3001-3048)
 *   7. SRTP session                   (nvst.rs:2546-2705)
 *   8. NvstVideoReceiver pipeline     (nvst.rs:4414-4961)
 *
 * See kn_rx.h for the deviation list (bounded pools, ms clock, event model).
 * Every quirk of the reference is ported as-is; the WHY lives in docs/RX.md.
 */
#include "kn/kn_rx.h"

#include "kn/knfec.h"

#include <string.h>

/* ------------------------------------------------------------- 1. helpers */

#define KN_RX_ORIGIN_AUTH     0u
#define KN_RX_ORIGIN_RECOVERED 1u

/* result.ready capacity — the reference Vec is unbounded; ours stops the
 * drain/complete loop instead (state stays consistent, the next poll
 * continues). Bounded by the reorder window: a single drain can never pop
 * more entries than the map holds (max_packets + 1). */
#define KN_RX_READY_MAX (KN_RX_MAX_REORDER_WINDOW + 1u)

static uint64_t rx_sat_sub_u64(uint64_t a, uint64_t b)
{
    return a > b ? a - b : 0u;
}

static void rx_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void rx_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void rx_put_le64(uint8_t *p, uint64_t v)
{
    rx_put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    rx_put_le32(p + 4, (uint32_t)(v >> 32));
}

static void rx_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void rx_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

/* ---------------------------------------------------------- packet pool */

int kn_rx_pool_init(kn_rx_pool *p, kn_rx_pkt *meta, uint32_t slots,
                    uint8_t *bytes, uint32_t pkt_cap)
{
    uint32_t i;
    if (!p || !meta || !bytes || slots == 0u || pkt_cap == 0u)
        return KN_RX_ERR_ARGS;
    p->meta = meta;
    p->bytes = bytes;
    p->slots = slots;
    p->pkt_cap = pkt_cap;
    p->in_use = 0;
    p->alloc_failures = 0;
    for (i = 0; i < slots; ++i) {
        memset(&meta[i], 0, sizeof meta[i]);
        meta[i].used = 0;
    }
    /* Free list: slot i links to i+1 through `index` (unused while free);
     * free_head is index+1 (0 = none) so slot 0 is addressable. */
    p->free_head = 1;
    return KN_RX_OK;
}

void kn_rx_pool_reset(kn_rx_pool *p)
{
    uint32_t i;
    if (!p || !p->meta) return;
    for (i = 0; i < p->slots; ++i) p->meta[i].used = 0;
    p->in_use = 0;
    p->free_head = 1;
}

static uint32_t pool_alloc(kn_rx_pool *p)
{
    uint32_t slot;
    if (p->free_head == 0u) {
        p->alloc_failures++;
        return KN_RX_NO_SLOT;
    }
    slot = p->free_head - 1u;
    /* Rebuild the next free slot by scanning: the pool is small (<= a few
     * thousand) and allocation happens once per received packet — a linear
     * scan keeps the free list trivially correct without extra per-slot
     * linkage storage. (Hot-path note for docs/RX.md: measure before
     * optimising; the E5500 budget is dominated by decode, not this.) */
    {
        uint32_t i, next = 0u;
        for (i = 0; i < p->slots; ++i) {
            if (!p->meta[i].used && i != slot) { next = i + 1u; break; }
        }
        p->free_head = next;
    }
    memset(&p->meta[slot], 0, sizeof p->meta[slot]);
    p->meta[slot].used = 1;
    p->in_use++;
    return slot;
}

static void pool_free(kn_rx_pool *p, uint32_t slot)
{
    if (!p || slot >= p->slots || !p->meta[slot].used) return;
    p->meta[slot].used = 0;
    if (p->in_use) p->in_use--;
    if (p->free_head == 0u || slot + 1u < p->free_head)
        p->free_head = slot + 1u;
}

static uint8_t *pool_bytes(kn_rx_pool *p, uint32_t slot)
{
    return p->bytes + (size_t)slot * (size_t)p->pkt_cap;
}

void kn_rx_pool_release(kn_rx_pool *p, uint32_t slot)
{
    pool_free(p, slot);
}

/* ------------------------------------------------------- 2. reorder buffer */

int kn_rx_reorder_init(kn_rx_reorder *r, kn_rx_reorder_entry *map,
                       uint32_t map_entries, uint32_t max_packets)
{
    uint32_t i;
    if (!r || !map || max_packets == 0u ||
        max_packets > KN_RX_MAX_REORDER_WINDOW ||
        map_entries < max_packets + 1u)
        return KN_RX_ERR_ARGS;
    memset(r, 0, sizeof *r);
    r->map = map;
    /* The map is caller-owned memory (possibly fresh from malloc): every
     * entry MUST be marked unused here — a garbage `used` flag would fake
     * buffered packets (spurious duplicates/phantom gaps). Caught by the
     * scenario battery running on a malloc'd arena. */
    for (i = 0; i < max_packets + 1u; ++i) {
        map[i].used = 0;
        map[i].index = 0;
        map[i].slot = 0;
    }
    r->max_packets = max_packets;
    return KN_RX_OK;
}

/* Free every held slot and clear state (reset, nvst.rs:3563-3567 — the
 * reference drops the BTreeMap which frees the packets). */
static void reorder_clear_all(kn_rx_reorder *r, kn_rx_pool *pool)
{
    uint32_t i, cap = r->max_packets + 1u;
    for (i = 0; i < cap; ++i) {
        if (r->map[i].used) {
            r->map[i].used = 0;
            pool_free(pool, r->map[i].slot);
        }
    }
    r->count = 0;
}

void kn_rx_reorder_reset(kn_rx_reorder *r)
{
    if (!r) return;
    /* The pool is owned by the pipeline; reset() without a pool can only
     * clear the metadata — the pipeline calls the pool-aware variant. */
    if (r->map) {
        uint32_t i, cap = r->max_packets + 1u;
        for (i = 0; i < cap; ++i) r->map[i].used = 0;
    }
    r->count = 0;
    r->have_next = 0;
    r->next_index = 0;
    r->have_gap_wait = 0;
}

static int reorder_find(const kn_rx_reorder *r, uint64_t index, uint32_t *pos)
{
    uint32_t i, cap = r->max_packets + 1u;
    for (i = 0; i < cap; ++i) {
        if (r->map[i].used && r->map[i].index == index) {
            if (pos) *pos = i;
            return 1;
        }
    }
    return 0;
}

static int reorder_remove(kn_rx_reorder *r, uint64_t index, uint32_t *slot_out)
{
    uint32_t pos;
    if (!reorder_find(r, index, &pos)) return 0;
    if (slot_out) *slot_out = r->map[pos].slot;
    r->map[pos].used = 0;
    if (r->count) r->count--;
    return 1;
}

static int reorder_min_used(const kn_rx_reorder *r, uint64_t *out_index)
{
    uint32_t i, cap = r->max_packets + 1u;
    uint64_t best = 0;
    int found = 0;
    for (i = 0; i < cap; ++i) {
        if (!r->map[i].used) continue;
        if (!found || r->map[i].index < best) {
            best = r->map[i].index;
            found = 1;
        }
    }
    if (found && out_index) *out_index = best;
    return found;
}

static void result_init(kn_rx_result *out)
{
    memset(out, 0, sizeof *out);
}

/* Free staged ready slots (ready.clear() in the reference DROPS the
 * packets — nvst.rs:3648). */
static void result_clear_ready(kn_rx_result *out, kn_rx_pool *pool)
{
    uint32_t i;
    for (i = 0; i < out->ready_len; ++i)
        pool_free(pool, out->ready[i]);
    out->ready_len = 0;
}

/* drain (nvst.rs:3577-3659). in_recovery: the PacketGap produced by push's
 * empty-buffer huge-jump resync (nvst.rs:3594-3601). */
static void reorder_drain(kn_rx_reorder *r, kn_rx_pool *pool, uint64_t now_ms,
                          int have_in_recovery, uint64_t in_rec_first,
                          uint64_t in_rec_last, kn_rx_result *out)
{
    int have_recovery = have_in_recovery;
    uint64_t rec_first = in_rec_first, rec_last = in_rec_last;

    out->ready_len = 0;
    out->have_nack = 0;

    for (;;) {
        uint32_t slot;
        /* pop the consecutive run starting at next_index */
        while (r->have_next &&
               out->ready_len < KN_RX_READY_MAX &&
               reorder_remove(r, r->next_index, &slot)) {
            out->ready[out->ready_len++] = slot;
            r->next_index++;
        }

        {
            uint64_t first_available;
            if (!reorder_min_used(r, &first_available)) {
                r->have_gap_wait = 0;
                break;                       /* map empty (nvst.rs:3620-3623) */
            }
            if (!r->have_next) {
                /* Unreachable in the reference (next_index is initialised on
                 * the first push and never cleared while packets exist);
                 * defensive resync instead of UB. */
                r->next_index = first_available;
                r->have_next = 1;
                continue;
            }
            {
                uint64_t expected = r->next_index;
                uint64_t gap_started;
                int exceeded, timed_out;
                /* Only NACK genuinely absent indices (nvst.rs:3628-3632 —
                 * the duplicate-retransmission-burst fix). */
                out->have_nack = 1;
                out->nack_first = expected;
                out->nack_last = rx_sat_sub_u64(first_available, 1u);

                if (r->have_gap_wait && r->gap_wait_index == expected) {
                    gap_started = r->gap_wait_start_ms;
                } else {
                    r->have_gap_wait = 1;
                    r->gap_wait_index = expected;
                    r->gap_wait_start_ms = now_ms;
                    gap_started = now_ms;
                }
                exceeded = (rx_sat_sub_u64(first_available, expected) >=
                            (uint64_t)r->max_packets) ||
                           ((uint64_t)r->count >= (uint64_t)r->max_packets);
                timed_out = rx_sat_sub_u64(now_ms, gap_started) >=
                            (uint64_t)KN_RX_REORDER_TIMEOUT_MS;
                if (!exceeded && !timed_out)
                    break;                   /* keep waiting (nvst.rs:3643) */

                /* Recovery: skip the gap. The reference clears the packets
                 * popped BEFORE the gap in this same drain (ready.clear(),
                 * nvst.rs:3648) — ported exactly (docs/RX.md Q1). */
                have_recovery = 1;
                rec_first = expected;
                rec_last = first_available - 1u;
                result_clear_ready(out, pool);
                r->next_index = first_available;
                r->have_gap_wait = 0;
                out->have_nack = 0;
                /* loop again: pop the run after the gap */
            }
        }
    }

    out->have_recovery = have_recovery;
    out->rec_first = rec_first;
    out->rec_last = rec_last;
}

int kn_rx_reorder_push(kn_rx_reorder *r, kn_rx_pool *pool, uint32_t slot,
                       uint64_t index, uint64_t now_ms, kn_rx_result *out)
{
    uint32_t buffered_before, i, free_pos = 0;
    int found_free = 0, have_recovery = 0;
    uint64_t rec_first = 0, rec_last = 0;

    if (!r || !pool || !out || slot >= pool->slots) return KN_RX_ERR_ARGS;
    result_init(out);

    if (!r->have_next) {                 /* get_or_insert(index), 3569 */
        r->next_index = index;
        r->have_next = 1;
    }
    if (index < r->next_index) {         /* StaleRtpPacket, 3571-3580 */
        pool_free(pool, slot);
        out->have_dropped = 1;
        out->drop_reason = KN_RX_DROP_STALE;
        out->drop_index = index;
        return KN_RX_OK;
    }
    if (reorder_find(r, index, NULL)) {  /* DuplicateRtpPacket, 3581-3590 */
        pool_free(pool, slot);
        out->have_dropped = 1;
        out->drop_reason = KN_RX_DROP_DUPLICATE;
        out->drop_index = index;
        return KN_RX_OK;
    }

    buffered_before = r->count;
    if (buffered_before == 0 &&
        rx_sat_sub_u64(index, r->next_index) >= (uint64_t)r->max_packets) {
        /* Empty-buffer huge jump: resync immediately (nvst.rs:3593-3601). */
        have_recovery = 1;
        rec_first = r->next_index;
        rec_last = index - 1u;
        r->next_index = index;
        r->have_gap_wait = 0;
    }

    /* insert into the map */
    {
        uint32_t cap = r->max_packets + 1u;
        for (i = 0; i < cap; ++i) {
            if (!r->map[i].used) { free_pos = i; found_free = 1; break; }
        }
    }
    if (!found_free) {
        /* Bounded-C deviation D-a: the reference BTreeMap always accepts
         * (then drain's packets.len() >= max_packets check recovers). Our
         * map is sized window+1 which drain keeps <= window after every
         * call, so this needs a caller under-sizing to trigger. */
        pool_free(pool, slot);
        pool->alloc_failures++;
        out->have_dropped = 1;
        out->drop_reason = KN_RX_DROP_POOL;
        out->drop_index = index;
        return KN_RX_OK;
    }
    r->map[free_pos].used = 1;
    r->map[free_pos].index = index;
    r->map[free_pos].slot = slot;
    r->count++;

    reorder_drain(r, pool, now_ms, have_recovery, rec_first, rec_last, out);
    return KN_RX_OK;
}

int kn_rx_reorder_poll(kn_rx_reorder *r, kn_rx_pool *pool, uint64_t now_ms,
                       kn_rx_result *out)
{
    if (!r || !pool || !out) return KN_RX_ERR_ARGS;
    result_init(out);
    reorder_drain(r, pool, now_ms, 0, 0, 0, out);
    return KN_RX_OK;
}

/* ------------------------------------------------------------ 3. FEC path */

int kn_rx_fec_init(kn_rx_fec *f, kn_rx_fec_block *blocks, uint32_t max_blocks,
                   uint32_t *shard_arena, uint32_t max_shards,
                   size_t video_packet_size,
                   uint8_t *scratch, size_t scratch_len)
{
    uint32_t i;
    if (!f || !blocks || !shard_arena || !scratch ||
        max_blocks == 0u || max_blocks > KN_RX_FEC_MAX_BLOCKS ||
        max_shards < 2u || max_shards > KN_RX_FEC_MAX_SHARDS ||
        video_packet_size == 0u ||
        video_packet_size > (size_t)KN_RX_MAX_VIDEO_PACKET_SIZE)
        return KN_RX_ERR_ARGS;
    /* NOTE: the protocol range (MIN..MAX_NVST_VIDEO_PACKET_SIZE) is
     * enforced by kn_rx_video_init's config validation (nvst.rs:1777-1786);
     * this building block only rejects nonsense. */
    /* The scratch must at least cover the smallest realistic repair
     * (4 parity rows); blocks needing more degrade to the NACK path
     * (kn_fec_reconstruct -> -5), exactly like a reference reconstruct
     * failure would. */
    if (scratch_len < kn_fec_scratch_bytes(4u, video_packet_size +
                                           KN_RX_FEC_HEADER_ALLOWANCE))
        return KN_RX_ERR_STATE;
    memset(f, 0, sizeof *f);
    f->blocks = blocks;
    f->shard_arena = shard_arena;
    f->max_blocks = max_blocks;
    f->max_shards = max_shards;
    f->shard_len = video_packet_size + KN_RX_FEC_HEADER_ALLOWANCE;
    f->scratch = scratch;
    f->scratch_len = scratch_len;
    for (i = 0; i < max_blocks; ++i) {
        blocks[i].used = 0;
        blocks[i].shard_slots = shard_arena + (size_t)i * (size_t)max_shards;
    }
    return KN_RX_OK;
}

static void fec_block_clear(kn_rx_fec_block *b, uint32_t total,
                            kn_rx_pool *pool)
{
    uint32_t i;
    for (i = 0; i < total; ++i) {
        if (b->shard_slots[i] != KN_RX_NO_SLOT) {
            pool_free(pool, b->shard_slots[i]);
            b->shard_slots[i] = KN_RX_NO_SLOT;
        }
    }
}

void kn_rx_fec_reset(kn_rx_fec *f)
{
    /* Metadata-only reset (see kn_rx_reorder_reset note): the pipeline uses
     * kn_rx_fec_reset_pool which returns the slots. */
    if (!f) return;
    f->have_completed_through = 0;
    f->have_gap_wait = 0;
}

static void fec_reset_pool(kn_rx_fec *f, kn_rx_pool *pool)
{
    uint32_t b;
    if (!f || !f->blocks) return;
    for (b = 0; b < f->max_blocks; ++b) {
        if (!f->blocks[b].used) continue;
        fec_block_clear(&f->blocks[b], f->max_shards, pool);
        f->blocks[b].used = 0;
    }
    f->have_completed_through = 0;
    f->have_gap_wait = 0;
}

static uint32_t fec_block_total(const kn_gs_fec_layout *l)
{
    return (uint32_t)l->data_shards + (uint32_t)l->parity_shards;
}

static int fec_layout_eq(const kn_gs_fec_layout *a, const kn_gs_fec_layout *b)
{
    return a->frame_index == b->frame_index &&
           a->block_index == b->block_index &&
           a->last_block_index == b->last_block_index &&
           a->data_shards == b->data_shards &&
           a->parity_shards == b->parity_shards;
}

/* matches (nvst.rs:3939-3947) */
static int fec_block_matches(const kn_rx_fec_block *b,
                             const kn_gs_fec_layout *l, uint64_t packet_index)
{
    return fec_layout_eq(&b->layout, l) &&
           b->base_index == rx_sat_sub_u64(packet_index,
                                           (uint64_t)l->shard_index);
}

/* missing_data_range (nvst.rs:3949-3960): first..last missing DATA shard. */
static int fec_missing_data_range(const kn_rx_fec_block *b, uint64_t *first,
                                  uint64_t *last)
{
    uint32_t i, d = b->layout.data_shards, fpos = 0, lpos = 0;
    int found = 0;
    for (i = 0; i < d; ++i) {
        if (b->shard_slots[i] == KN_RX_NO_SLOT) {
            if (!found) { fpos = i; found = 1; }
            lpos = i;
        }
    }
    if (!found) return 0;
    *first = b->base_index + (uint64_t)fpos;
    *last = b->base_index + (uint64_t)lpos;
    return 1;
}

/* first_missing_data_run (nvst.rs:3962-3974) */
static int fec_first_missing_run(const kn_rx_fec_block *b, uint64_t *first,
                                 uint64_t *last)
{
    uint32_t i, d = b->layout.data_shards, fpos = 0, len = 0;
    int found = 0;
    for (i = 0; i < d; ++i) {
        if (b->shard_slots[i] == KN_RX_NO_SLOT) {
            if (!found) { fpos = i; found = 1; len = 1; }
            else if (fpos + len == i) len++;
            else break;
        }
    }
    if (!found) return 0;
    *first = b->base_index + (uint64_t)fpos;
    *last = *first + (uint64_t)(len - 1u);
    return 1;
}

static uint32_t fec_count_present(const kn_rx_fec_block *b, uint32_t total)
{
    uint32_t i, n = 0;
    for (i = 0; i < total; ++i)
        if (b->shard_slots[i] != KN_RX_NO_SLOT) ++n;
    return n;
}

/* finish (nvst.rs:3998-4117): returns 0 ok with the data-shard slots staged
 * in out_ready (callers own them), *out_repaired = reconstructed count;
 * negative = NvstDropReason-ish failure with the block left INTACT (the
 * reference clones before finish and reinserts the clone — same net state).
 * Failure codes: -1 = KN_RX_DROP_UNSUPPORTED_FEC, -2 = malformed envelope. */
static int fec_block_finish(kn_rx_fec *f, kn_rx_pool *pool,
                            kn_rx_fec_block *b, uint32_t *out_ready,
                            uint32_t *out_ready_len, uint32_t *out_repaired)
{
    uint32_t total = fec_block_total(&b->layout);
    uint32_t d = b->layout.data_shards, p = b->layout.parity_shards;
    uint32_t missing_data = 0, i;
    uint32_t template_slot = KN_RX_NO_SLOT;
    size_t extension_start;
    uint8_t *template_env = NULL;
    kn_gs_rtp template_hdr;
    uint32_t recovered_slots[KN_RX_FEC_MAX_SHARDS];
    uint32_t n_recovered = 0;

    *out_ready_len = 0;
    *out_repaired = 0;

    for (i = 0; i < d; ++i)
        if (b->shard_slots[i] == KN_RX_NO_SLOT) ++missing_data;

    if (missing_data == 0) {
        /* Healthy block: data shards move straight out, parity is freed
         * (nvst.rs:4001-4009 — "without copying the video payload"). */
        for (i = 0; i < d; ++i)
            out_ready[(*out_ready_len)++] = b->shard_slots[i];
        for (i = d; i < total; ++i) {
            if (b->shard_slots[i] != KN_RX_NO_SLOT)
                pool_free(pool, b->shard_slots[i]);
            b->shard_slots[i] = KN_RX_NO_SLOT;
        }
        *out_repaired = 0;
        return 0;
    }

    /* Any present shard longer than shard_len poisons the algebra
     * (nvst.rs:4011-4018). */
    for (i = 0; i < total; ++i) {
        if (b->shard_slots[i] == KN_RX_NO_SLOT) continue;
        if (pool->meta[b->shard_slots[i]].len > f->shard_len)
            return -1;
    }

    /* Template = FIRST present shard in shard order — parity included
     * (nvst.rs:4019-4031: the 1-data/2-parity layout must stay
     * recoverable when the single data packet is the one lost). */
    for (i = 0; i < total; ++i) {
        if (b->shard_slots[i] != KN_RX_NO_SLOT) {
            template_slot = b->shard_slots[i];
            break;
        }
    }
    if (template_slot == KN_RX_NO_SLOT) return -1;   /* empty block: -1 */
    if (!pool->meta[template_slot].rtp_ok) return -2;
    template_hdr = pool->meta[template_slot].rtp;
    if ((size_t)template_hdr.payload_off < (size_t)KN_GS_EXT_LEN)
        return -2;                       /* checked_sub underflow, 4055-4060 */
    extension_start = (size_t)template_hdr.payload_off - (size_t)KN_GS_EXT_LEN;
    template_env = pool_bytes(pool, template_slot);
    if (f->shard_len < extension_start + (size_t)KN_GS_EXT_LEN)
        return -2;                       /* 4068-4073 */

    {
        /* Reconstruct: present shards point into their pool slots (knfec
         * zero-pads short shards), missing DATA shards receive fresh pool
         * slots. knfec mirrors reconstruct_nvst_cauchy_data (3798-3924). */
        uint8_t *shard_ptrs[KN_RX_FEC_MAX_SHARDS];
        size_t shard_lens[KN_RX_FEC_MAX_SHARDS];
        uint8_t *out_missing[KN_RX_FEC_MAX_SHARDS];
        int recon;

        if (total > KN_RX_FEC_MAX_SHARDS) return -1;   /* deviation D-b */

        for (i = 0; i < total; ++i) {
            if (b->shard_slots[i] != KN_RX_NO_SLOT) {
                shard_ptrs[i] = pool_bytes(pool, b->shard_slots[i]);
                shard_lens[i] = (size_t)pool->meta[b->shard_slots[i]].len;
            } else {
                shard_ptrs[i] = NULL;
                shard_lens[i] = 0;
            }
            out_missing[i] = NULL;
        }
        /* Allocate recovery slots for missing DATA shards up front so a
         * mid-repair failure can free them cleanly. */
        for (i = 0; i < d; ++i) {
            if (b->shard_slots[i] != KN_RX_NO_SLOT) continue;
            {
                uint32_t s = pool_alloc(pool);
                if (s == KN_RX_NO_SLOT) {
                    uint32_t k;
                    for (k = 0; k < n_recovered; ++k)
                        pool_free(pool, recovered_slots[k]);
                    return -1;           /* pool exhausted (D-a) */
                }
                recovered_slots[n_recovered++] = s;
                out_missing[i] = pool_bytes(pool, s);
            }
        }
        recon = kn_fec_reconstruct(shard_ptrs, shard_lens, d, p,
                                   f->shard_len, f->scratch, f->scratch_len,
                                   out_missing);
        if (recon < 0) {
            uint32_t k;
            for (k = 0; k < n_recovered; ++k)
                pool_free(pool, recovered_slots[k]);
            return -1;                   /* Unsupported(FecRepair), 4043-4048
                                            and reconstruct errors alike */
        }

        /* Re-wrap every recovered shard (nvst.rs:4061-4106): envelope from
         * the template, then sequence, frame_index and the ext[11] block
         * nibbles; strict re-parse; flags validation IS enforced here
         * (4087-4093) while the fec_word is NOT (4097-4106 — pitfall #4).
         * The shards are committed to the block only after EVERY re-wrap
         * validated: the reference reinserts a pre-finish CLONE on failure
         * (4246-4250), so a failed repair retries from the pristine state —
         * partially recovered shards must not leak into the retry. */
        {
            uint32_t k = 0;
            uint32_t assign_idx[KN_RX_FEC_MAX_SHARDS];
            for (i = 0; i < d; ++i) {
                uint8_t *plain;
                uint16_t sequence;
                kn_gs_rtp hdr;
                unsigned flags;
                int prc;
                if (b->shard_slots[i] != KN_RX_NO_SLOT) continue;
                if (k >= n_recovered) { /* defensive; cannot happen */
                    uint32_t j;
                    for (j = 0; j < n_recovered; ++j)
                        pool_free(pool, recovered_slots[j]);
                    return -1;
                }
                plain = pool_bytes(pool, recovered_slots[k]);
                sequence = (uint16_t)((b->base_index + (uint64_t)i) & 0xFFFFu);
                memcpy(plain, template_env, extension_start);
                rx_put_be16(plain + 2, sequence);
                rx_put_le32(plain + extension_start + 4, b->layout.frame_index);
                plain[extension_start + 11] =
                    (uint8_t)(((b->layout.last_block_index & 3u) << 6) |
                              ((b->layout.block_index & 3u) << 4));
                prc = kn_gs_rtp_parse_hdr(plain, f->shard_len, &hdr);
                if (prc != KN_GS_OK || !hdr.has_gs) {
                    uint32_t j;
                    for (j = 0; j < n_recovered; ++j)
                        pool_free(pool, recovered_slots[j]);
                    return -2;           /* MalformedRtp / MissingNvVideoHeader
                                            (4084-4087) */
                }
                flags = (unsigned)plain[hdr.gs_off + 8] & 0x0Fu;
                if ((flags & ~(KN_GS_FLAG_SOF | KN_GS_FLAG_EOF |
                               KN_GS_FLAG_PIC)) != 0u) {
                    uint32_t j;
                    for (j = 0; j < n_recovered; ++j)
                        pool_free(pool, recovered_slots[j]);
                    return -1;           /* Unsupported(FecRepair), 4089-4094 */
                }
                pool->meta[recovered_slots[k]].index =
                    b->base_index + (uint64_t)i;
                pool->meta[recovered_slots[k]].len = (uint32_t)f->shard_len;
                pool->meta[recovered_slots[k]].origin = KN_RX_ORIGIN_RECOVERED;
                pool->meta[recovered_slots[k]].rtp = hdr;
                pool->meta[recovered_slots[k]].rtp_ok = 1;
                pool->meta[recovered_slots[k]].rtp_err = 0;
                assign_idx[k] = i;
                ++k;
            }
            for (k = 0; k < n_recovered; ++k)
                b->shard_slots[assign_idx[k]] = recovered_slots[k];
        }
    }

    /* Collect data shards in order; free parity. */
    for (i = 0; i < d; ++i)
        out_ready[(*out_ready_len)++] = b->shard_slots[i];
    for (i = d; i < total; ++i) {
        if (b->shard_slots[i] != KN_RX_NO_SLOT)
            pool_free(pool, b->shard_slots[i]);
        b->shard_slots[i] = KN_RX_NO_SLOT;
    }
    *out_repaired = missing_data;
    return 0;
}

static int fec_find_block(kn_rx_fec *f, uint64_t base, uint32_t *pos)
{
    uint32_t i;
    for (i = 0; i < f->max_blocks; ++i) {
        if (f->blocks[i].used && f->blocks[i].base_index == base) {
            *pos = i;
            return 1;
        }
    }
    return 0;
}

static int fec_min_block(kn_rx_fec *f, uint64_t *base_out, uint32_t *pos_out)
{
    uint32_t i, best = 0;
    int found = 0;
    uint64_t best_base = 0;
    for (i = 0; i < f->max_blocks; ++i) {
        if (!f->blocks[i].used) continue;
        if (!found || f->blocks[i].base_index < best_base) {
            best_base = f->blocks[i].base_index;
            best = i;
            found = 1;
        }
    }
    if (found) { *base_out = best_base; *pos_out = best; }
    return found;
}

static int fec_max_block_base(kn_rx_fec *f, uint64_t *base_out)
{
    uint32_t i;
    int found = 0;
    uint64_t best = 0;
    for (i = 0; i < f->max_blocks; ++i) {
        if (!f->blocks[i].used) continue;
        if (!found || f->blocks[i].base_index > best) {
            best = f->blocks[i].base_index;
            found = 1;
        }
    }
    if (found) *base_out = best;
    return found;
}

static uint32_t fec_blocks_used(const kn_rx_fec *f)
{
    uint32_t i, n = 0;
    for (i = 0; i < f->max_blocks; ++i)
        if (f->blocks[i].used) ++n;
    return n;
}

/* poll_deadline (nvst.rs:4182-4310). */
static void fec_poll(kn_rx_fec *f, kn_rx_pool *pool, uint64_t now_ms,
                     kn_rx_result *out)
{
    for (;;) {
        uint64_t base;
        uint32_t pos;
        kn_rx_fec_block *head;
        uint32_t total;

        if (!fec_min_block(f, &base, &pos)) {
            f->have_gap_wait = 0;
            break;                                   /* 4192-4195 */
        }
        head = &f->blocks[pos];
        total = fec_block_total(&head->layout);

        /* Inter-block gap: completed_through < base (4197-4227). */
        if (f->have_completed_through && base > f->completed_through) {
            uint64_t expected = f->completed_through;
            uint64_t gap_started;
            out->have_nack = 1;
            out->nack_first = expected;
            out->nack_last = base - 1u;
            if (f->have_gap_wait && f->gap_wait_index == expected)
                gap_started = f->gap_wait_start_ms;
            else {
                f->have_gap_wait = 1;
                f->gap_wait_index = expected;
                f->gap_wait_start_ms = now_ms;
                gap_started = now_ms;
            }
            if (rx_sat_sub_u64(now_ms, gap_started) <
                    (uint64_t)KN_RX_REORDER_TIMEOUT_MS &&
                rx_sat_sub_u64(base, expected) <
                    (uint64_t)KN_RX_MAX_REORDER_WINDOW &&
                fec_blocks_used(f) < f->max_blocks)
                break;                               /* keep waiting */
            out->have_recovery = 1;
            out->rec_first = expected;
            out->rec_last = base - 1u;
            result_clear_ready(out, pool);
            out->have_nack = 0;
            f->completed_through = base;
            f->have_gap_wait = 0;
            continue;
        }

        /* Completable head block (4229-4272). */
        if (fec_count_present(head, total) >= head->layout.data_shards &&
            !head->repair_failed) {
            uint32_t ready_stage[KN_RX_FEC_MAX_SHARDS];
            uint32_t ready_len = 0, repaired = 0;
            uint64_t completed_through_new =
                base + (uint64_t)total;
            int frc;
            if (out->ready_len + head->layout.data_shards > KN_RX_READY_MAX)
                break;              /* ready full: retry on the next poll */
            frc = fec_block_finish(f, pool, head, ready_stage, &ready_len,
                                   &repaired);
            if (frc == 0) {
                uint32_t i;
                head->used = 0;      /* block consumed; slots moved out */
                for (i = 0; i < ready_len; ++i)
                    out->ready[out->ready_len++] = ready_stage[i];
                out->fec_repaired += repaired;
                f->packets_repaired += repaired;
                f->blocks_completed++;
                f->completed_through = completed_through_new;
                f->have_completed_through = 1;
                f->have_gap_wait = 0;
                continue;
            }
            /* Reconstruction failed: keep the block, burn ONE attempt, hand
             * the loss to NACK (4246-4272). repair_failed is reset by the
             * next insert (nvst.rs:3988-3991). */
            f->blocks_repair_failed++;
            head->repair_failed = 1;
            {
                uint64_t fm = 0, lm = 0;
                if (fec_first_missing_run(head, &fm, &lm)) {
                    out->have_nack = 1;
                    out->nack_first = fm;
                    out->nack_last = lm;
                    f->have_gap_wait = 1;
                    f->gap_wait_index = fm;
                    f->gap_wait_start_ms = now_ms;
                }
            }
            break;
        }

        /* Incomplete head (4274-4309): a later block proves the head
         * stopped arriving; ask for the first missing run only. */
        if (fec_blocks_used(f) == 1 && !f->have_gap_wait)
            break;
        {
            uint64_t fm = 0, lm = 0, gap_started, block_span = 0, last_base;
            if (!fec_first_missing_run(head, &fm, &lm))
                break;               /* defensive: complete block above */
            out->have_nack = 1;
            out->nack_first = fm;
            out->nack_last = lm;
            if (f->have_gap_wait && f->gap_wait_index == fm)
                gap_started = f->gap_wait_start_ms;
            else {
                f->have_gap_wait = 1;
                f->gap_wait_index = fm;
                f->gap_wait_start_ms = now_ms;
                gap_started = now_ms;
            }
            if (fec_max_block_base(f, &last_base))
                block_span = rx_sat_sub_u64(last_base, base);
            if (rx_sat_sub_u64(now_ms, gap_started) <
                    (uint64_t)KN_RX_REORDER_TIMEOUT_MS &&
                block_span < (uint64_t)KN_RX_MAX_REORDER_WINDOW &&
                fec_blocks_used(f) < f->max_blocks)
                break;

            /* Timed out: drop the head block, report its full missing DATA
             * range, resync completed_through past it (4296-4309). */
            {
                uint64_t rf = 0, rl = 0;
                if (!fec_missing_data_range(head, &rf, &rl)) {
                    rf = base;                 /* defensive; cannot happen */
                    rl = base;
                }
                fec_block_clear(head, total, pool);
                head->used = 0;
                f->blocks_dropped_timeout++;
                out->have_recovery = 1;
                out->rec_first = rf;
                out->rec_last = rl;
                result_clear_ready(out, pool);
                out->have_nack = 0;
                f->completed_through = base + (uint64_t)total;
                f->have_completed_through = 1;
                f->have_gap_wait = 0;
                continue;      /* "emit any complete successors" (4309) */
            }
        }
    }
}

int kn_rx_fec_push(kn_rx_fec *f, kn_rx_pool *pool, uint32_t slot,
                   uint64_t index, const kn_gs_fec_layout *layout,
                   uint64_t now_ms, kn_rx_result *out)
{
    uint64_t packet_base;
    uint32_t pos = 0, total;
    kn_rx_fec_block *b;

    if (!f || !pool || !layout || !out || slot >= pool->slots)
        return KN_RX_ERR_ARGS;
    result_init(out);

    packet_base = rx_sat_sub_u64(index, (uint64_t)layout->shard_index);

    /* Silently ignore packets behind completed_through (4145-4156 — no
     * drop event in the reference either). */
    if (f->have_completed_through && index < f->completed_through) {
        pool_free(pool, slot);
        return KN_RX_OK;
    }

    if (!fec_find_block(f, packet_base, &pos)) {
        /* New block (or_insert_with, 4157-4159). */
        uint32_t i;
        int placed = 0;
        total = fec_block_total(layout);
        if (total > f->max_shards) {
            /* Deviation D-b: the shard table cannot hold this block. The
             * reference stores it; any LOSS would fail its reconstruct
             * anyway (Cauchy u8 coordinates, nvst.rs:3806). */
            pool_free(pool, slot);
            out->have_dropped = 1;
            out->drop_reason = KN_RX_DROP_UNSUPPORTED_FEC;
            out->drop_index = index;
            return KN_RX_OK;
        }
        for (i = 0; i < f->max_blocks; ++i) {
            if (!f->blocks[i].used) { pos = i; placed = 1; break; }
        }
        if (!placed) {
            /* Bounded deviation D-a: the reference map is uncapped but
             * poll prunes it back under MAX_PENDING_FEC_BLOCKS on every
             * push, so this needs sustained pathological loss. */
            pool_free(pool, slot);
            pool->alloc_failures++;
            out->have_dropped = 1;
            out->drop_reason = KN_RX_DROP_POOL;
            out->drop_index = index;
            return KN_RX_OK;
        }
        b = &f->blocks[pos];
        memset(b, 0, sizeof *b);
        b->shard_slots = f->shard_arena + (size_t)pos * (size_t)f->max_shards;
        {
            uint32_t k;
            for (k = 0; k < f->max_shards; ++k)
                b->shard_slots[k] = KN_RX_NO_SLOT;
        }
        b->layout = *layout;
        b->base_index = packet_base;
        b->repair_failed = 0;
        b->used = 1;
    }
    b = &f->blocks[pos];

    if (!fec_block_matches(b, layout, index)) {
        pool_free(pool, slot);
        out->have_dropped = 1;
        /* The reference reports Unsupported(FecRepair) for the mismatch
         * (4160-4170); the pipeline maps a DATA-shard Unsupported to
         * invalidate_picture, so the reason must stay UNSUPPORTED. */
        out->drop_reason = KN_RX_DROP_UNSUPPORTED_FEC;
        out->drop_index = index;
        return KN_RX_OK;
    }

    /* insert (nvst.rs:3976-3991) */
    if (layout->shard_index >= f->max_shards ||
        b->shard_slots[layout->shard_index] != KN_RX_NO_SLOT) {
        pool_free(pool, slot);
        out->have_dropped = 1;
        out->drop_reason = KN_RX_DROP_DUPLICATE;    /* DuplicateRtpPacket */
        out->drop_index = index;
        return KN_RX_OK;
    }
    pool->meta[slot].index = index;
    b->shard_slots[layout->shard_index] = slot;
    b->repair_failed = 0;    /* a new shard re-arms one repair attempt */

    fec_poll(f, pool, now_ms, out);
    return KN_RX_OK;
}

int kn_rx_fec_poll(kn_rx_fec *f, kn_rx_pool *pool, uint64_t now_ms,
                   kn_rx_result *out)
{
    if (!f || !pool || !out) return KN_RX_ERR_ARGS;
    result_init(out);
    fec_poll(f, pool, now_ms, out);
    return KN_RX_OK;
}

/* ------------------------------------------------- 4. frame invalidation */

void kn_rx_inval_init(kn_rx_inval *v)
{
    if (v) memset(v, 0, sizeof *v);
}

void kn_rx_inval_reset(kn_rx_inval *v)
{
    if (v) memset(v, 0, sizeof *v);   /* discard() + Self::default() */
}

int kn_rx_inval_lose(kn_rx_inval *v, uint64_t now_ms)
{
    if (!v) return 0;
    if (!v->frame_types_known) return 0;   /* 141-148: keep the immediate
                                              keyframe request */
    if (!v->have_active_since) {
        v->active_since_ms = now_ms;
        v->have_active_since = 1;
    }
    return 1;
}

static void inval_request_idr_backstop(kn_rx_inval *v, int *out_kf)
{
    if (v->backstop_requested) return;     /* 251-261: one-shot */
    v->backstop_requested = 1;
    v->cnt_idr_backstops++;
    if (out_kf) *out_kf = 1;
}

static void inval_clear(kn_rx_inval *v)
{
    /* clear (263-269) + feedback.discard (95-97): the unsent range, the
     * in-flight marker AND last_sent_at are all reset. */
    v->have_unsent = 0;
    v->unsent_first = v->unsent_last = 0;
    v->rfi_in_flight = 0;
    v->have_rfi_last_sent = 0;
    v->have_pending = 0;
    v->pending_first = v->pending_last = 0;
    v->have_active_since = 0;
    v->have_last_retry = 0;
    v->backstop_requested = 0;
}

static void inval_invalidate(kn_rx_inval *v, int have_previous,
                             uint32_t previous, uint32_t frame_index,
                             uint64_t now_ms, int *out_kf)
{
    /* invalidate (233-249): a real sender-frame range becomes the RFI
     * report; anything else falls back to the one-shot IDR request. */
    if (!have_previous ||
        frame_index <= (previous == 0xFFFFFFFFu ? previous : previous + 1u)) {
        inval_request_idr_backstop(v, out_kf);
        return;
    }
    {
        uint64_t lost_first = (uint64_t)previous + 1u;
        uint64_t lost_last = (uint64_t)frame_index - 1u;
        uint64_t rf, rl;
        if (v->have_pending) {
            rf = v->pending_first < lost_first ? v->pending_first : lost_first;
            rl = v->pending_last > lost_last ? v->pending_last : lost_last;
        } else {
            rf = lost_first;
            rl = lost_last;
        }
        v->have_pending = 1;
        v->pending_first = rf;
        v->pending_last = rl;
        v->have_unsent = 1;             /* queue(range) — 74-77: overwrite */
        v->unsent_first = rf;
        v->unsent_last = rl;
        v->have_last_retry = 1;
        v->last_retry_ms = now_ms;
    }
}

void kn_rx_inval_poll_backstop(kn_rx_inval *v, uint64_t now_ms,
                               int *out_keyframe_request)
{
    int have_last, kf = 0;
    uint64_t last = 0;
    if (!v) return;
    /* 150-168 */
    if (v->have_pending) {
        have_last = 0;
        if (v->have_last_retry) { last = v->last_retry_ms; have_last = 1; }
        if (v->have_rfi_last_sent &&
            (!have_last || v->rfi_last_sent_ms > last)) {
            last = v->rfi_last_sent_ms;
            have_last = 1;
        }
        if (!have_last ||
            rx_sat_sub_u64(now_ms, last) >= (uint64_t)KN_RX_RFI_RETRY_MS) {
            v->have_unsent = 1;
            v->unsent_first = v->pending_first;
            v->unsent_last = v->pending_last;
            v->have_last_retry = 1;
            v->last_retry_ms = now_ms;
        }
    }
    if (v->have_active_since &&
        rx_sat_sub_u64(now_ms, v->active_since_ms) >=
            (uint64_t)KN_RX_RFI_BACKSTOP_MS)
        inval_request_idr_backstop(v, &kf);
    if (out_keyframe_request && kf) *out_keyframe_request = 1;
}

int kn_rx_inval_admit(kn_rx_inval *v, kn_rx_frame *f, int loss_before,
                      uint64_t now_ms, int *out_keyframe_request)
{
    int have_previous, frame_gap, invalidating, kf = 0;
    uint32_t previous = 0;
    unsigned wire;

    if (!v || !f) return 0;
    /* admit (170-231) */
    have_previous = v->have_last_frame;
    previous = v->last_frame;
    v->last_frame = f->frame_index;
    v->have_last_frame = 1;
    v->frame_types_known = f->frame_type_known;

    frame_gap = have_previous &&
                f->frame_index > (previous == 0xFFFFFFFFu ? previous
                                                          : previous + 1u);
    if (frame_gap)
        kn_rx_inval_lose(v, now_ms);
    invalidating = v->have_active_since;

    if (!f->frame_type_known) {
        if (invalidating) kf = 1;
        inval_clear(v);
        f->contiguous = !loss_before && !invalidating;
        if (out_keyframe_request) *out_keyframe_request = kf;
        return 1;
    }

    wire = f->frame_type_wire;
    if (f->keyframe || wire == KN_GS_FRAME_I ||
        wire == KN_GS_FRAME_INTRA_REFRESH || wire == KN_GS_FRAME_REF_PIC_I) {
        if (invalidating) {
            if (wire == KN_GS_FRAME_INTRA_REFRESH)
                v->cnt_recovered_intra_refresh++;
            else if (wire == KN_GS_FRAME_REF_PIC_I)
                v->cnt_recovered_ref_pic_i++;
            else
                v->cnt_recovered_i++;
            inval_clear(v);
        }
        f->contiguous = invalidating || !loss_before;
        if (out_keyframe_request) *out_keyframe_request = kf;
        return 1;
    }

    if (!invalidating) {
        f->contiguous = !loss_before;
        if (out_keyframe_request) *out_keyframe_request = kf;
        return 1;
    }

    if (loss_before || frame_gap)
        inval_invalidate(v, have_previous, previous, f->frame_index, now_ms,
                         &kf);

    if (wire == KN_GS_FRAME_HEADER) {
        f->contiguous = 1;
        if (out_keyframe_request) *out_keyframe_request = kf;
        return 1;
    }

    v->cnt_dropped_frames++;
    if (out_keyframe_request) *out_keyframe_request = kf;
    return 0;                            /* withheld */
}

int kn_rx_inval_take_rfi(kn_rx_inval *v, uint64_t *first, uint64_t *last)
{
    int had;
    if (!v || !first || !last) return 0;
    had = v->have_unsent;
    *first = v->unsent_first;
    *last = v->unsent_last;
    v->have_unsent = 0;
    v->rfi_in_flight = had;              /* take() — 79-84 */
    return had;
}

void kn_rx_inval_restore_rfi(kn_rx_inval *v, uint64_t first, uint64_t last)
{
    if (!v) return;
    if (v->rfi_in_flight) {              /* restore() — 86-92 */
        v->rfi_in_flight = 0;
        if (!v->have_unsent) {
            v->have_unsent = 1;
            v->unsent_first = first;
            v->unsent_last = last;
        }
    }
}

void kn_rx_inval_record_sent(kn_rx_inval *v, uint64_t now_ms)
{
    if (!v) return;
    if (!v->rfi_in_flight) return;       /* record_sent — 94-101 */
    v->rfi_in_flight = 0;
    v->rfi_last_sent_ms = now_ms;
    v->have_rfi_last_sent = 1;
    v->cnt_sent++;
}

/* --------------------------------------------------------- 5. NACK plane */

int kn_rx_nack_init(kn_rx_nack *n, kn_rx_nack_entry *table, uint32_t capacity)
{
    if (!n || !table || capacity == 0u || capacity > KN_RX_NACK_MAX_PENDING)
        return KN_RX_ERR_ARGS;
    memset(n, 0, sizeof *n);
    memset(table, 0, sizeof(table[0]) * (size_t)capacity);
    n->table = table;
    n->capacity = capacity;
    return KN_RX_OK;
}

void kn_rx_nack_reset(kn_rx_nack *n)
{
    if (!n || !n->table) return;
    memset(n->table, 0, sizeof(n->table[0]) * (size_t)n->capacity);
    n->count = 0;
}

static int nack_find(kn_rx_nack *n, uint64_t index, uint32_t *pos)
{
    uint32_t i;
    for (i = 0; i < n->capacity; ++i) {
        if (n->table[i].used && n->table[i].index == index) {
            if (pos) *pos = i;
            return 1;
        }
    }
    return 0;
}

static void nack_purge_expired(kn_rx_nack *n, uint64_t now_ms)
{
    uint32_t i;
    for (i = 0; i < n->capacity; ++i) {
        if (!n->table[i].used) continue;
        if (rx_sat_sub_u64(now_ms, n->table[i].requested_at_ms) >=
            (uint64_t)KN_RX_NACK_TRACKING_MS) {
            n->table[i].used = 0;
            if (n->count) n->count--;
            n->expired_total++;
        }
    }
}

void kn_rx_nack_request(kn_rx_nack *n, uint64_t first, uint64_t last,
                        uint64_t now_ms, int *out_keyframe_request)
{
    uint64_t index;
    if (!n || first > last) return;      /* 946-949 */
    nack_purge_expired(n, now_ms);
    for (index = first; ; ++index) {
        uint32_t i, free_pos = 0;
        int found_free = 0;
        if (nack_find(n, index, NULL)) {
            if (index == last) break;
            continue;
        }
        if (n->count >= n->capacity) {
            n->overflow_events++;
            if (out_keyframe_request) *out_keyframe_request = 1;
            break;                       /* 961-965 */
        }
        for (i = 0; i < n->capacity; ++i) {
            if (!n->table[i].used) { free_pos = i; found_free = 1; break; }
        }
        if (!found_free) break;          /* defensive; count says otherwise */
        n->table[free_pos].used = 1;
        n->table[free_pos].index = index;
        n->table[free_pos].requested_at_ms = now_ms;
        n->table[free_pos].have_last_sent = 0;
        n->table[free_pos].last_sent_at_ms = 0;
        n->table[free_pos].attempts = 0;
        n->count++;
        if (index == last) break;
        if (index == 0xFFFFFFFFFFFFFFFFull) break;   /* wrap guard */
    }
    n->requests_total++;
}

int kn_rx_nack_resolve(kn_rx_nack *n, uint64_t index)
{
    uint32_t pos;
    int sent;
    if (!n) return 0;
    if (!nack_find(n, index, &pos)) return 0;
    sent = n->table[pos].have_last_sent; /* 977-985 */
    n->table[pos].used = 0;
    if (n->count) n->count--;
    if (sent) n->resolved_total++;
    return sent;
}

int kn_rx_nack_take(kn_rx_nack *n, uint64_t now_ms, uint32_t retry_rtt_ms,
                    uint64_t *first_out, uint64_t *last_out)
{
    uint32_t retry_interval, max_attempts, i, taken = 0;
    uint64_t best = 0, cur, last;
    int found = 0;

    if (!n || !first_out || !last_out) return 0;
    /* take_nack (1272-1318): Mjolnir policy — interval rtt+4ms, attempts
     * clamp(52/rtt, 1, 3); no RTT sample == zero RTT == (4ms, 3). */
    if (retry_rtt_ms == 0u) {
        retry_interval = KN_RX_NACK_RETRY_MS;
        max_attempts = KN_RX_NACK_MAX_ATTEMPTS;
    } else {
        uint32_t attempts = (uint32_t)((uint64_t)KN_RX_NACK_TRACKING_MS /
                                       (uint64_t)retry_rtt_ms);
        if (attempts < 1u) attempts = 1u;
        if (attempts > KN_RX_NACK_MAX_ATTEMPTS)
            attempts = KN_RX_NACK_MAX_ATTEMPTS;
        retry_interval = retry_rtt_ms + KN_RX_NACK_RETRY_MS;
        max_attempts = attempts;
    }
    nack_purge_expired(n, now_ms);

    for (i = 0; i < n->capacity; ++i) {
        kn_rx_nack_entry *e = &n->table[i];
        if (!e->used) continue;
        if (e->attempts >= max_attempts) continue;
        if (e->have_last_sent &&
            rx_sat_sub_u64(now_ms, e->last_sent_at_ms) < retry_interval)
            continue;
        if (!found || e->index < best) { best = e->index; found = 1; }
    }
    if (!found) return 0;

    cur = best;
    last = best;
    while (taken < KN_RX_NACK_MAX_PACKET_COUNT) {
        uint32_t pos;
        kn_rx_nack_entry *e;
        if (!nack_find(n, cur, &pos)) break;
        e = &n->table[pos];
        if (cur > last + 1u) break;      /* contiguous run only (1313-1315) */
        if (e->attempts >= max_attempts) break;
        if (e->have_last_sent &&
            rx_sat_sub_u64(now_ms, e->last_sent_at_ms) < retry_interval)
            break;
        e->have_last_sent = 1;
        e->last_sent_at_ms = now_ms;
        if (e->attempts < 255u) e->attempts++;
        last = cur;
        ++taken;
        if (cur == 0xFFFFFFFFFFFFFFFFull) break;
        ++cur;
    }
    if (taken == 0u) return 0;
    n->sends_total++;
    *first_out = best;
    *last_out = last;
    return 1;
}

void kn_rx_nack_mark_send_failed(kn_rx_nack *n, uint64_t first, uint64_t last)
{
    uint64_t index;
    if (!n) return;
    for (index = first; index <= last; ++index) {
        uint32_t pos;
        if (nack_find(n, index, &pos)) {
            n->table[pos].have_last_sent = 0;    /* 1320-1327 */
            if (n->table[pos].attempts > 0u) n->table[pos].attempts--;
        }
        if (index == 0xFFFFFFFFFFFFFFFFull) break;
    }
}

void kn_rx_nack_clear(kn_rx_nack *n)
{
    if (!n || !n->table) return;
    {
        uint32_t i;
        for (i = 0; i < n->capacity; ++i) n->table[i].used = 0;
    }
    n->count = 0;                        /* clear_nacks — 1329-1333 */
}

/* ------------------------------------------------ feedback encoders (6) */

/* ServerControl framing (nvst_control.rs:28-38): u16LE code, u16LE payload
 * length, payload. */
static int ctrl_encode(uint16_t code, const uint8_t *payload,
                       size_t payload_len, uint8_t *out, size_t out_len)
{
    if (payload_len > 0xFFFFu) return 0;
    if (out_len < 4u + payload_len) return -1;
    rx_put_le16(out, code);
    rx_put_le16(out + 2, (uint16_t)payload_len);
    if (payload_len) memcpy(out + 4, payload, payload_len);
    return (int)(4u + payload_len);
}

int kn_rx_ctrl_nack_v2(uint8_t stream_index, const uint16_t *missing,
                       uint32_t missing_count, uint8_t *out, size_t out_len)
{
    uint8_t payload[3u + KN_RX_NACK_MAX_PACKET_COUNT * 10u];
    uint16_t base;
    uint64_t bitmap;
    uint32_t i;
    size_t plen;

    if (!out) return -1;
    if (!missing || missing_count == 0u ||
        missing_count > KN_RX_NACK_MAX_PACKET_COUNT)
        return 0;                        /* None (nvst_control.rs:41-43) */

    payload[0] = 2u;                     /* version */
    payload[1] = stream_index;
    payload[2] = 0u;                     /* record count, patched below */
    plen = 3u;
    base = missing[0];
    bitmap = 0u;
    for (i = 1u; i < missing_count; ++i) {
        uint16_t distance = (uint16_t)(missing[i] - base);   /* wrapping */
        if (distance == 0u) continue;
        if (distance <= 64u) {
            bitmap |= (uint64_t)1 << (unsigned)(distance - 1u);
        } else {
            rx_put_le16(payload + plen, base);
            rx_put_le64(payload + plen + 2u, bitmap);
            plen += 10u;
            payload[2]++;
            base = missing[i];
            bitmap = 0u;
        }
    }
    rx_put_le16(payload + plen, base);
    rx_put_le64(payload + plen + 2u, bitmap);
    plen += 10u;
    payload[2]++;
    {
        int rc = ctrl_encode(KN_RX_CTRL_NACK_V2, payload, plen, out, out_len);
        return rc;
    }
}

int kn_rx_ctrl_idr_request(uint8_t out[6])
{
    uint8_t payload[2];
    if (!out) return -1;
    rx_put_le16(payload, 0u);            /* nvst_control.rs:162-167 */
    return ctrl_encode(KN_RX_CTRL_IDR_REQUEST, payload, 2u, out, 6u);
}

int kn_rx_ctrl_frame_invalidation(uint64_t first_frame, uint64_t last_frame,
                                  uint16_t stream_index,
                                  uint8_t out[4 + KN_RX_CTRL_FRAME_INVALIDATION_PAYLOAD_LEN])
{
    uint8_t payload[KN_RX_CTRL_FRAME_INVALIDATION_PAYLOAD_LEN];
    if (!out) return -1;
    memset(payload, 0, sizeof payload);
    rx_put_le64(payload + 0, first_frame);   /* nvst_control.rs:169-181 */
    rx_put_le64(payload + 8, last_frame);
    rx_put_le64(payload + 16, (uint64_t)stream_index);
    return ctrl_encode(KN_RX_CTRL_FRAME_INVALIDATION, payload, sizeof payload,
                       out, 4u + sizeof payload);
}

int kn_rx_ctrl_frame_ack(uint32_t frame_number,
                         const double *first_packet_time_ms,
                         uint32_t frame_bytes,
                         uint8_t out[4 + KN_RX_CTRL_FRAME_ACK_PAYLOAD_LEN])
{
    uint8_t payload[KN_RX_CTRL_FRAME_ACK_PAYLOAD_LEN];
    unsigned off;
    if (!out) return -1;
    memset(payload, 0, sizeof payload);      /* nvst_control.rs:72-94 */
    rx_put_le16(payload + 0, 1u);
    rx_put_le16(payload + 2, 9u);
    rx_put_le32(payload + 4, frame_number);
    if (first_packet_time_ms) {
        uint64_t bits;
        double v = *first_packet_time_ms;
        memcpy(&bits, &v, sizeof bits);      /* f64::to_bits */
        rx_put_le64(payload + 12, bits);
    }
    for (off = 20u; off <= 48u; off += 4u)
        rx_put_le32(payload + off, 0xBF800000u);  /* (-1.0f32).to_bits() */
    rx_put_le32(payload + 72, frame_bytes);
    rx_put_le32(payload + 94, 0xBF800000u);
    rx_put_le32(payload + 98, 0xBF800000u);
    return ctrl_encode(KN_RX_CTRL_FRAME_ACK, payload, sizeof payload, out,
                       4u + sizeof payload);
}

int kn_rx_ctrl_frame_pacing(uint32_t frame_number,
                            uint32_t target_frame_time_us,
                            uint32_t pacing_error_us,
                            uint8_t out[4 + KN_RX_CTRL_FRAME_PACING_PAYLOAD_LEN])
{
    uint8_t payload[KN_RX_CTRL_FRAME_PACING_PAYLOAD_LEN];
    if (!out) return -1;
    memset(payload, 0, sizeof payload);      /* nvst_control.rs:96-112 */
    rx_put_le32(payload + 0, 5u);
    rx_put_le32(payload + 8, 2u);
    rx_put_le32(payload + 12, frame_number);
    rx_put_le32(payload + 16, target_frame_time_us);
    rx_put_le32(payload + 20, pacing_error_us < target_frame_time_us
                                  ? pacing_error_us : target_frame_time_us);
    rx_put_le32(payload + 24, 0x341au);
    return ctrl_encode(KN_RX_CTRL_FRAME_PACING, payload, sizeof payload, out,
                       4u + sizeof payload);
}

int kn_rx_ctrl_qos_report(const kn_rx_qos *q,
                          uint8_t out[4 + KN_RX_CTRL_QOS_PAYLOAD_LEN])
{
    uint8_t payload[KN_RX_CTRL_QOS_PAYLOAD_LEN];
    if (!q || !out) return -1;
    memset(payload, 0, sizeof payload);      /* nvst_control.rs:146-160 —
                                                bytes_received and the packet
                                                snapshot are NOT encoded by
                                                the reference command();
                                                ported as-is. */
    rx_put_le32(payload + 0, 7u);
    rx_put_le32(payload + 8, q->sequence);
    rx_put_le32(payload + 12, q->sender_frame_number);
    rx_put_le32(payload + 16, q->bw_minimum_server_time);
    rx_put_le32(payload + 20, q->bw_queue_delay_us);
    rx_put_le16(payload + 24, q->bw_jitter_us);
    rx_put_le16(payload + 26, q->loss_per_ten_thousand);
    payload[28] = q->bw_utilization_percent;
    rx_put_le16(payload + 30, 1000u);
    rx_put_le16(payload + 32, 1000u);
    rx_put_le32(payload + 36, q->client_time_90khz);
    rx_put_le32(payload + 40, q->bw_lossy_frames);
    rx_put_le32(payload + 44, q->bw_estimate_kbps);
    rx_put_le32(payload + 48, q->bw_median_server_time);
    return ctrl_encode(KN_RX_CTRL_QOS_REPORT, payload, sizeof payload, out,
                       4u + sizeof payload);
}

void kn_rx_rtcp_pli(uint32_t sender_ssrc, uint32_t media_ssrc,
                    uint8_t out[12])
{
    if (!out) return;
    out[0] = 0x81u;                      /* V=2 P=0 FMT=1 (nvst.rs:3001) */
    out[1] = 206u;                       /* PSFB */
    rx_put_be16(out + 2, 2u);
    rx_put_be32(out + 4, sender_ssrc);
    rx_put_be32(out + 8, media_ssrc);
}

/* The reference's build_rtcp_receiver_report (nvst.rs:2983-2997), byte
 * for byte: 0x81 | 201 | len 7 | sender | media | fraction | cumulative
 * (24-bit, clamped like report_snapshot :1233) | highest | jitter |
 * LSR 0 | DLSR 0. Sent on the rtcp1 channel every 1 s (:97/:8041). */
void kn_rx_rtcp_rr(uint32_t sender_ssrc, uint32_t media_ssrc,
                   uint8_t fraction_lost, int32_t cumulative_lost,
                   uint32_t highest_sequence, uint32_t jitter,
                   uint8_t out[32])
{
    uint32_t cum;
    if (!out) return;
    if (cumulative_lost > 0x7FFFFF) cumulative_lost = 0x7FFFFF;
    if (cumulative_lost < -0x800000) cumulative_lost = -0x800000;
    cum = (uint32_t)cumulative_lost & 0xFFFFFFu;
    out[0] = 0x81u;                      /* V=2 P=0 RC=1 */
    out[1] = 201u;                       /* PT = Receiver Report */
    rx_put_be16(out + 2, 7u);
    rx_put_be32(out + 4, sender_ssrc);
    rx_put_be32(out + 8, media_ssrc);
    out[12] = fraction_lost;
    out[13] = (uint8_t)(cum >> 16);
    out[14] = (uint8_t)(cum >> 8);
    out[15] = (uint8_t)cum;
    rx_put_be32(out + 16, highest_sequence);
    rx_put_be32(out + 20, jitter);
    rx_put_be32(out + 24, 0u);           /* LSR  — the reference never */
    rx_put_be32(out + 28, 0u);           /* DLSR — fills these (:2994) */
}

/* ------------------------------------------------------------ round XXXVIII
 * D-t10: the bandwidth-estimator feed (publish_stream_packet,
 * nvst.rs:852-899).
 *
 * Two reference details decide what may be fed:
 *   - the feed happens on the AUTHENTICATED DATAGRAM path only, after
 *     resolve_nack and before the FEC/reorder routing (:4703-4725);
 *   - the video tuple exists only when `!retransmitted && !reordered`
 *     (:4694-4702) AND the packet parses as an RTP payload plus a GS
 *     video header. Anything else publishes with video = None, which
 *     still counts the packet for the QoS snapshot but measures nothing.
 * `bytes` is the RTP PAYLOAD length (:4697 payload.len()), not the
 * datagram length — the difference is the whole SRTP/RTP header, which
 * would inflate estimate_kbps by ~5%.
 */
static int64_t bw_now_ns(const kn_rx_video *v, uint64_t now_ms)
{
    if (v->cfg.now_ns) return v->cfg.now_ns(v->cfg.now_ns_ud);
    return (int64_t)(now_ms * 1000000u);   /* deterministic replay fallback */
}

static void bw_feed(kn_rx_video *v, uint32_t ssrc, const kn_gs_rtp *rtp,
                    const uint8_t *plain, size_t plain_len, uint32_t seq32,
                    int reordered, int retransmitted, uint64_t now_ms)
{
    /* publish_stream_packet's ssrc-change reset (:875-884): a new media
     * ssrc discards the whole estimator, so a stream switch cannot report
     * a transit minimum measured on the previous stream. */
    if (v->have_bw_ssrc && v->bw_ssrc != ssrc) kn_bw_init(&v->bw);
    v->bw_ssrc = ssrc;
    v->have_bw_ssrc = 1;

    if (retransmitted) { v->bw_skipped_retransmitted++; return; }
    if (reordered)     { v->bw_skipped_reordered++;     return; }
    if (!rtp->has_gs)  { v->bw_skipped_no_video++;      return; }

    {
        kn_gs_video video;
        size_t poff = 0, plen = 0;
        /* header.payload(&plaintext) — padding validation on the DECRYPTED
         * buffer (:2512-2525) — then NvVideoPacket::parse (:3229-3248).
         * A failure on either is the reference's `.ok()`/None: counted,
         * not fed, never an error. */
        if (kn_gs_rtp_payload(rtp, plain, plain_len, &poff, &plen) != KN_GS_OK ||
            kn_gs_video_parse(plain + rtp->gs_off, &video) != KN_GS_OK) {
            v->bw_skipped_no_video++;
            return;
        }
        if (video.is_fec) {
            /* a parity shard is not measured; it only keeps the frame's
             * sequence contiguity honest (:886-887) */
            kn_bw_observe_parity(&v->bw, video.frame_index, seq32);
            v->bw_parity_samples++;
            return;
        }
        {
            kn_bw_sample s;
            memset(&s, 0, sizeof s);
            s.frame_number = video.frame_index;
            s.sequence = seq32;
            s.timestamp = rtp->timestamp;
            s.bytes = (uint32_t)plen;
            s.started = video.is_start_of_frame;
            s.ended = video.is_end_of_frame;
            s.received_ns = bw_now_ns(v, now_ms);
            kn_bw_observe(&v->bw, &s);
            v->bw_samples++;
        }
    }
}

int kn_rx_video_rr_snapshot(const kn_rx_video *v, uint32_t *ssrc,
                            uint32_t *base, uint32_t *highest,
                            uint64_t *received, uint32_t *jitter)
{
    if (!v || !v->have_bound_ssrc || !v->have_base_sequence) return 0;
    if (ssrc)     *ssrc = v->bound_ssrc;
    if (base)     *base = v->base_sequence;
    if (highest)  *highest = v->highest_sequence_received;
    if (received) *received = v->authenticated_packets;
    if (jitter) {
        /* report_snapshot (:1251-1261): timing.jitter clamped to u32. */
        double j = v->timing_jitter;
        *jitter = j < 0.0 ? 0u
                : (j > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)j);
    }
    return 1;
}

int kn_rx_video_qos_snapshot(const kn_rx_video *v, uint32_t *ssrc,
                             uint32_t *base, uint32_t *highest,
                             uint32_t *received)
{
    if (!v || !v->have_qos_snapshot) return 0;
    if (ssrc)     *ssrc = v->bw_ssrc;
    if (base)     *base = v->qos_base_sequence;
    if (highest)  *highest = v->qos_highest_sequence;
    if (received) *received = v->qos_received_packets;
    return 1;
}

int kn_rx_video_bw_report(const kn_rx_video *v, int64_t now_ns,
                          kn_bw_feedback *out)
{
    if (!v || !out) return 0;
    memset(out, 0, sizeof *out);
    /* now_ns == 0 is the production convenience path: read the configured
     * monotonic clock here instead of making every caller convert. */
    kn_bw_report(&v->bw, now_ns ? now_ns : bw_now_ns(v, 0u), out);
    return 1;
}

void kn_rx_video_bw_counters(const kn_rx_video *v, uint64_t *samples,
                             uint64_t *parity, uint64_t *skipped_reordered,
                             uint64_t *skipped_retransmitted,
                             uint64_t *skipped_no_video)
{
    if (!v) return;
    if (samples)                *samples = v->bw_samples;
    if (parity)                 *parity = v->bw_parity_samples;
    if (skipped_reordered)      *skipped_reordered = v->bw_skipped_reordered;
    if (skipped_retransmitted)  *skipped_retransmitted =
                                    v->bw_skipped_retransmitted;
    if (skipped_no_video)       *skipped_no_video = v->bw_skipped_no_video;
}

uint32_t kn_rx_video_bw_frames_observed(const kn_rx_video *v)
{
    return v ? kn_bw_frames_observed(&v->bw) : 0u;
}

void kn_rx_video_bw_reset(kn_rx_video *v)
{
    if (!v) return;
    kn_bw_init(&v->bw);
    v->have_bw_ssrc = 0;
    v->bw_ssrc = 0;
}

size_t kn_rx_rtcp_nack(uint32_t sender_ssrc, uint32_t media_ssrc,
                       uint64_t first_missing, uint64_t last_missing,
                       uint8_t *out, size_t out_len)
{
    uint16_t pid[KN_RX_NACK_MAX_FCI_ENTRIES], blp[KN_RX_NACK_MAX_FCI_ENTRIES];
    uint32_t entries = 0, i;
    uint64_t current = first_missing, effective_last;
    size_t need;

    if (!out) return 0;
    /* build_rtcp_nack (nvst.rs:3015-3048) */
    effective_last = last_missing;
    {
        uint64_t cap = first_missing +
                       (uint64_t)(KN_RX_NACK_MAX_PACKET_COUNT - 1u);
        if (first_missing > (uint64_t)0xFFFFFFFFFFFFFFFFull -
                            (uint64_t)(KN_RX_NACK_MAX_PACKET_COUNT - 1u))
            cap = 0xFFFFFFFFFFFFFFFFull;      /* saturating_add */
        if (effective_last > cap) effective_last = cap;
    }
    while (current <= effective_last && entries < KN_RX_NACK_MAX_FCI_ENTRIES) {
        uint64_t represented = effective_last - current;
        if (represented > 16u) represented = 16u;
        pid[entries] = (uint16_t)(current & 0xFFFFu);
        blp[entries] = represented == 16u
                           ? (uint16_t)0xFFFFu
                           : (uint16_t)(((uint32_t)1 << (unsigned)represented) - 1u);
        ++entries;
        if (current > 0xFFFFFFFFFFFFFFFFull - 17u) break;  /* saturating */
        current += 17u;
    }
    need = 12u + (size_t)entries * 4u;
    if (out_len < need) return 0;
    out[0] = 0x81u;                      /* V=2 P=0 FMT=1 generic NACK */
    out[1] = 205u;                       /* RTPFB */
    rx_put_be16(out + 2, (uint16_t)(2u + entries));
    rx_put_be32(out + 4, sender_ssrc);
    rx_put_be32(out + 8, media_ssrc);
    for (i = 0; i < entries; ++i) {
        rx_put_be16(out + 12 + (size_t)i * 4u, pid[i]);
        rx_put_be16(out + 14 + (size_t)i * 4u, blp[i]);
    }
    return need;
}

/* -------------------------------------------------------- 7. SRTP session */

int kn_rx_srtp_session_init(kn_rx_srtp_session *s, int profile,
                            const uint8_t *master_key, size_t master_key_len,
                            const uint8_t *master_salt, size_t master_salt_len,
                            size_t tag_len)
{
    if (!s) return KN_RX_ERR_ARGS;
    memset(s, 0, sizeof *s);
    kn_srtp_replay_init(&s->replay);
    s->profile = profile;

    if (profile == KN_RX_SRTP_NONE) {
        s->ready = 1;
        s->tag_len = 0;
        s->key_len = 0;
        return KN_RX_OK;
    }
    if (!master_key || !master_salt) return KN_RX_ERR_ARGS;

    switch (profile) {
    case KN_RX_SRTP_CM128_HMAC_SHA1:
        if (master_key_len != 16u || master_salt_len > 14u ||
            (tag_len != 4u && tag_len != 10u))
            return KN_RX_ERR_ARGS;
        if (kn_srtp_kdf128(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_CIPHER_KEY, s->enc_key, 16) != 0 ||
            kn_srtp_kdf128(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_AUTH_KEY, s->auth_key, 20) != 0 ||
            kn_srtp_kdf128(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_SALT, s->salt, 14) != 0)
            return KN_RX_ERR_STATE;
        s->key_len = 16;
        break;
    case KN_RX_SRTP_CM256_HMAC_SHA1:
        if (master_key_len != 32u || master_salt_len > 14u ||
            (tag_len != 4u && tag_len != 10u))
            return KN_RX_ERR_ARGS;
        if (!kn_srtp_backend_256()) return KN_RX_ERR_NO_CIPHER;
        if (kn_srtp_kdf256(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_CIPHER_KEY, s->enc_key, 32) != 0 ||
            kn_srtp_kdf256(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_AUTH_KEY, s->auth_key, 20) != 0 ||
            kn_srtp_kdf256(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_SALT, s->salt, 14) != 0)
            return KN_RX_ERR_STATE;
        s->key_len = 32;
        break;
    case KN_RX_SRTP_GCM128:
        if (master_key_len != 16u || master_salt_len > 14u ||
            (tag_len != 8u && tag_len != 16u))
            return KN_RX_ERR_ARGS;
        if (!kn_srtp_backend_gcm()) return KN_RX_ERR_NO_CIPHER;
        if (kn_srtp_kdf128(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_CIPHER_KEY, s->enc_key, 16) != 0 ||
            kn_srtp_kdf128(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_SALT, s->salt, 12) != 0)
            return KN_RX_ERR_STATE;
        s->key_len = 16;
        break;
    case KN_RX_SRTP_GCM256:
        if (master_key_len != 32u || master_salt_len > 14u ||
            (tag_len != 8u && tag_len != 16u))
            return KN_RX_ERR_ARGS;
        if (!kn_srtp_backend_gcm() || !kn_srtp_backend_256())
            return KN_RX_ERR_NO_CIPHER;
        if (kn_srtp_kdf256(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_CIPHER_KEY, s->enc_key, 32) != 0 ||
            kn_srtp_kdf256(master_key, master_salt, master_salt_len,
                           KN_SRTP_LABEL_SALT, s->salt, 12) != 0)
            return KN_RX_ERR_STATE;
        s->key_len = 32;
        break;
    default:
        return KN_RX_ERR_ARGS;
    }
    s->tag_len = tag_len;
    s->ready = 1;
    return KN_RX_OK;
}

/* SrtpReceiver::unprotect (nvst.rs:2635-2705), order of operations exact:
 * parse -> guess -> cipher -> replay check -> payload(padding) -> commit. */
int kn_rx_srtp_unprotect(kn_rx_srtp_session *s, uint8_t *datagram, size_t len,
                         uint64_t *out_index, kn_gs_rtp *out_rtp,
                         size_t *out_plain_len, int *out_detail)
{
    kn_gs_rtp rtp;
    uint64_t index = 0;
    uint32_t roc;
    int rc, detail = 0;
    size_t plain_len;
    size_t poff, plen;

    if (out_detail) *out_detail = 0;
    if (!s || !datagram || !out_index || !out_rtp || !out_plain_len)
        return KN_RX_ERR_ARGS;
    if (!s->ready) return KN_RX_DROP_NO_CIPHER;

    rc = kn_gs_rtp_parse_hdr(datagram, len, &rtp);
    if (rc != KN_GS_OK) {
        if (out_detail) *out_detail = rc;
        return KN_RX_DROP_MALFORMED_RTP;
    }
    rc = kn_srtp_replay_guess(&s->replay, (uint16_t)rtp.seq, &index);
    if (rc != 0)
        return KN_RX_DROP_REPLAY_REJECTED;   /* ROC estimate underflow */
    roc = (uint32_t)(index >> 16);

    switch (s->profile) {
    case KN_RX_SRTP_NONE:
        plain_len = len;
        break;
    case KN_RX_SRTP_CM128_HMAC_SHA1: {
        size_t ol = 0;
        rc = kn_srtp_unprotect_cm_hmac(datagram, len, rtp.payload_off,
                                       rtp.ssrc, index, s->enc_key,
                                       s->auth_key, s->salt, s->tag_len, &ol);
        if (rc == 1) return KN_RX_DROP_AUTH_FAILED;
        if (rc < 0) {
            /* knsrtp reports malformed (short/tag underrun) as -1; the
             * reference verdict is MissingAuthenticationTag. */
            detail = KN_GS_ERR_TOO_SHORT;
            if (out_detail) *out_detail = detail;
            return KN_RX_DROP_MALFORMED_RTP;
        }
        plain_len = ol;
        break;
    }
    case KN_RX_SRTP_CM256_HMAC_SHA1: {
        size_t auth_len;
        if (len < s->tag_len || len - s->tag_len < (size_t)rtp.payload_off) {
            detail = KN_GS_ERR_TOO_SHORT;
            if (out_detail) *out_detail = detail;
            return KN_RX_DROP_MALFORMED_RTP;
        }
        auth_len = len - s->tag_len;
        rc = kn_srtp_verify_tag(s->auth_key, datagram, auth_len, roc,
                                datagram + auth_len, s->tag_len);
        if (rc != 0) return KN_RX_DROP_AUTH_FAILED;
        {
            uint8_t iv[16];
            kn_srtp_aes_cm_iv(s->salt, rtp.ssrc, index, iv);
            kn_srtp_aes256_cm_keystream_xor(s->enc_key, iv, 0,
                                            datagram + rtp.payload_off,
                                            auth_len - (size_t)rtp.payload_off);
        }
        plain_len = auth_len;
        break;
    }
    case KN_RX_SRTP_GCM128:
    case KN_RX_SRTP_GCM256: {
        uint8_t iv[12];
        size_t ct_end;
        if (len < s->tag_len) {
            detail = KN_GS_ERR_TOO_SHORT;      /* MissingAuthenticationTag */
            if (out_detail) *out_detail = detail;
            return KN_RX_DROP_MALFORMED_RTP;
        }
        ct_end = len - s->tag_len;
        if (ct_end < (size_t)rtp.payload_off) {
            detail = KN_GS_ERR_TOO_SHORT;
            if (out_detail) *out_detail = detail;
            return KN_RX_DROP_MALFORMED_RTP;
        }
        kn_srtp_gcm_iv(s->salt, rtp.ssrc, roc, (uint16_t)rtp.seq, iv);
        rc = kn_srtp_gcm_auth_decrypt(s->enc_key,
                                      s->profile == KN_RX_SRTP_GCM128 ? 128u
                                                                      : 256u,
                                      iv, datagram, rtp.payload_off,
                                      datagram + ct_end, s->tag_len,
                                      datagram + rtp.payload_off,
                                      ct_end - (size_t)rtp.payload_off);
        if (rc == 1) {
            /* Scrub the tentatively decrypted region: mbedTLS decrypts
             * before verifying (documented deviation in knsrtp.h). */
            memset(datagram + rtp.payload_off, 0,
                   ct_end - (size_t)rtp.payload_off);
            return KN_RX_DROP_AUTH_FAILED;
        }
        if (rc != 0) return KN_RX_DROP_NO_CIPHER;
        plain_len = ct_end;
        break;
    }
    default:
        return KN_RX_DROP_NO_CIPHER;
    }

    rc = kn_srtp_replay_check(&s->replay, index);
    if (rc != 0) return KN_RX_DROP_REPLAY_REJECTED;

    /* payload()/padding validation on the PLAINTEXT (2695-2697) */
    rc = kn_gs_rtp_payload(&rtp, datagram, plain_len, &poff, &plen);
    if (rc != KN_GS_OK) {
        if (out_detail) *out_detail = rc;
        return KN_RX_DROP_MALFORMED_RTP;
    }
    kn_srtp_replay_commit(&s->replay, index);

    rtp.payload_off = (unsigned)poff;
    rtp.payload_len = (unsigned)plen;
    *out_index = index;
    *out_rtp = rtp;
    *out_plain_len = plain_len;
    return KN_RX_OK;
}

/* ----------------------------------------------------------- 8. pipeline */

static void ev_reset(kn_rx_video *v)
{
    v->ev_count = 0;
}

static void ev_push(kn_rx_video *v, const kn_rx_event *e)
{
    if (v->ev_count < v->ev_capacity)
        v->mem.events[v->ev_count++] = *e;
    else
        v->ev_overflow++;
}

static void ev_dropped(kn_rx_video *v, int reason, uint64_t index, int detail)
{
    kn_rx_event e;
    memset(&e, 0, sizeof e);
    e.type = KN_RX_EV_DROPPED;
    e.drop_reason = reason;
    e.index = index;
    e.detail = detail;
    if (reason >= 0 && reason < (int)(sizeof v->drop_counters /
                                      sizeof v->drop_counters[0]))
        v->drop_counters[reason]++;
    ev_push(v, &e);
}

static void ev_recovery_gap(kn_rx_video *v, uint64_t first, uint64_t last)
{
    kn_rx_event e;
    memset(&e, 0, sizeof e);
    e.type = KN_RX_EV_RECOVERY_GAP;
    e.gap_first = first;
    e.gap_last = last;
    ev_push(v, &e);
}

static void request_keyframe(kn_rx_video *v)
{
    if (!v->keyframe_needed) v->keyframe_requests++;
    v->keyframe_needed = 1;
}

int kn_rx_video_init(kn_rx_video *v, const kn_rx_video_config *cfg,
                     const kn_rx_mem *mem, uint64_t now_ms)
{
    kn_rx_video_config c;
    size_t shard_len;
    int rc;

    if (!v || !cfg || !mem) return KN_RX_ERR_ARGS;
    if (cfg->codec != KN_GS_CODEC_H264 && cfg->codec != KN_GS_CODEC_H265 &&
        cfg->codec != KN_GS_CODEC_AV1)
        return KN_RX_ERR_ARGS;

    c = *cfg;
    if (c.reorder_window_packets == 0u)
        c.reorder_window_packets = KN_RX_DEFAULT_REORDER_WINDOW;
    if (c.max_access_unit_bytes == 0u)
        c.max_access_unit_bytes = KN_GS_DEFAULT_MAX_ACCESS_UNIT;
    if (c.video_packet_size == 0u)
        c.video_packet_size = KN_RX_DEFAULT_VIDEO_PACKET_SIZE;
    if (c.timeout_ms == 0u) c.timeout_ms = KN_RX_DEFAULT_TIMEOUT_MS;
    if (c.startup_timeout_ms == 0u) c.startup_timeout_ms = c.timeout_ms;
    if (c.frame_time_us == 0u) c.frame_time_us = 16666u;
    if (c.progress_stall_ms == 0u) c.progress_stall_ms = c.timeout_ms;
    if (c.progress_keyframe_grace_ms == 0u)
        c.progress_keyframe_grace_ms = c.timeout_ms;

    /* Config ranges (nvst.rs:1748-1786) */
    if (c.reorder_window_packets > KN_RX_MAX_REORDER_WINDOW)
        return KN_RX_ERR_STATE;
    if (c.max_access_unit_bytes == 0u ||
        c.max_access_unit_bytes > KN_GS_MAX_ACCESS_UNIT)
        return KN_RX_ERR_STATE;
    if (c.timeout_ms < KN_RX_MIN_TIMEOUT_MS ||
        c.timeout_ms > KN_RX_MAX_TIMEOUT_MS)
        return KN_RX_ERR_STATE;
    if (c.startup_timeout_ms < c.timeout_ms ||
        c.startup_timeout_ms > KN_RX_MAX_TIMEOUT_MS)
        return KN_RX_ERR_STATE;
    if (c.video_packet_size < KN_RX_MIN_VIDEO_PACKET_SIZE ||
        c.video_packet_size > KN_RX_MAX_VIDEO_PACKET_SIZE)
        return KN_RX_ERR_STATE;

    /* Memory bounds */
    if (!mem->pool_meta || !mem->pool_bytes || mem->pool_slots == 0u ||
        mem->pool_pkt_cap == 0u)
        return KN_RX_ERR_ARGS;
    if (!mem->reorder_map) return KN_RX_ERR_ARGS;
    if (!mem->fec_blocks || mem->fec_max_blocks == 0u ||
        mem->fec_max_blocks > KN_RX_FEC_MAX_BLOCKS ||
        !mem->fec_shard_arena || mem->fec_max_shards < 2u ||
        mem->fec_max_shards > KN_RX_FEC_MAX_SHARDS)
        return KN_RX_ERR_ARGS;
    if (!mem->fec_scratch || mem->fec_scratch_len == 0u)
        return KN_RX_ERR_ARGS;
    if (!mem->nack_table || mem->nack_capacity == 0u)
        return KN_RX_ERR_ARGS;
    if (!mem->au_buffer ||
        mem->au_buffer_capacity < c.max_access_unit_bytes)
        return KN_RX_ERR_ARGS;
    if (!mem->events || mem->events_capacity < 4u)
        return KN_RX_ERR_ARGS;
    shard_len = (size_t)c.video_packet_size + KN_RX_FEC_HEADER_ALLOWANCE;
    if ((size_t)mem->pool_pkt_cap < shard_len)
        return KN_RX_ERR_STATE;
    if (mem->pool_slots < c.reorder_window_packets + 1u)
        return KN_RX_ERR_STATE;

    memset(v, 0, sizeof *v);
    v->cfg = c;
    v->mem = *mem;
    kn_bw_init(&v->bw);          /* D-t10: the estimator starts empty */

    rc = kn_rx_pool_init(&v->pool, mem->pool_meta, mem->pool_slots,
                         mem->pool_bytes, mem->pool_pkt_cap);
    if (rc != KN_RX_OK) return rc;
    rc = kn_rx_reorder_init(&v->reorder, mem->reorder_map,
                            c.reorder_window_packets + 1u,
                            c.reorder_window_packets);
    if (rc != KN_RX_OK) return rc;
    rc = kn_rx_fec_init(&v->fec, mem->fec_blocks, mem->fec_max_blocks,
                        mem->fec_shard_arena, mem->fec_max_shards,
                        c.video_packet_size, mem->fec_scratch,
                        mem->fec_scratch_len);
    if (rc != KN_RX_OK) return rc;
    rc = kn_rx_nack_init(&v->nack, mem->nack_table, mem->nack_capacity);
    if (rc != KN_RX_OK) return rc;
    rc = kn_gs_au_assembler_init(&v->assembler, c.codec, mem->au_buffer,
                                 mem->au_buffer_capacity,
                                 c.max_access_unit_bytes);
    if (rc != KN_GS_OK) return KN_RX_ERR_STATE;
    kn_rx_inval_init(&v->inval);
    v->ev_capacity = mem->events_capacity;

    v->state = KN_RX_STATE_RUNNING;
    v->next_frame_contiguous = 1;        /* new(): true (nvst.rs:4455) */
    v->timeout_origin_ms = now_ms;
    v->initial_timeout_pending = 1;
    v->srtp.profile = KN_RX_SRTP_NONE;
    kn_srtp_replay_init(&v->srtp.replay);
    v->srtp.ready = 1;
    return KN_RX_OK;
}

void kn_rx_video_set_srtp(kn_rx_video *v, const kn_rx_srtp_session *session)
{
    if (!v || !session) return;
    v->srtp = *session;
}

void kn_rx_video_reset_media(kn_rx_video *v, uint64_t now_ms)
{
    if (!v) return;
    /* reset_media_state (nvst.rs:4946-4961). Note: next_frame_contiguous
     * becomes FALSE here (the constructor's `true` is not restored) — the
     * first frame after any reset reports contiguous=false unless it is an
     * invalidation-clearing type; ported exactly. */
    (void)now_ms;
    reorder_clear_all(&v->reorder, &v->pool);
    kn_rx_reorder_reset(&v->reorder);
    fec_reset_pool(&v->fec, &v->pool);
    kn_gs_au_assembler_reset(&v->assembler);
    v->have_last_spi = 0;
    v->next_frame_contiguous = 0;
    /* watchdog reset (FrameProgressWatchdog::default) */
    v->have_wp_first_authenticated = 0;
    v->wp_first_authenticated_ms = 0;
    v->wp_first_keyframe_assembled = 0;
    v->have_wp_last_frame = 0;
    v->wp_last_frame = 0;
    v->have_wp_last_assembled = 0;
    v->wp_last_assembled_ms = 0;
    v->wp_stage = 0;
    v->have_wp_keyframe_requested = 0;
    kn_rx_inval_reset(&v->inval);
}

static void invalidate_picture(kn_rx_video *v, uint64_t now_ms)
{
    /* invalidate_picture (nvst.rs:4867-4874) */
    kn_gs_au_assembler_reset(&v->assembler);
    v->have_last_spi = 0;
    v->next_frame_contiguous = 0;
    if (!kn_rx_inval_lose(&v->inval, now_ms))
        request_keyframe(v);
}

static int au_err_to_drop(int rc)
{
    switch (rc) {
    case KN_GS_ERR_NO_PICTURE:        return KN_RX_DROP_MISSING_START_CODE;
    case KN_GS_ERR_AU_AWAITING_SOF:   return KN_RX_DROP_AWAITING_SOF;
    case KN_GS_ERR_AU_DISCONTINUITY:  return KN_RX_DROP_FRAME_DISCONTINUITY;
    case KN_GS_ERR_AU_TOO_LARGE:      return KN_RX_DROP_AU_TOO_LARGE;
    case KN_GS_ERR_AU_LENGTH:         return KN_RX_DROP_INVALID_AU_LENGTH;
    case KN_GS_ERR_AU_FEC:            return KN_RX_DROP_UNSUPPORTED_FEC;
    default:                          return KN_RX_DROP_MALFORMED_RTP;
    }
}

/* consume_reorder_result (nvst.rs:4741-4862). */
static void consume_result(kn_rx_video *v, const kn_rx_result *result,
                           uint64_t now_ms, kn_rx_frame_fn cb, void *ud)
{
    uint32_t i;
    int kf = 0;

    v->fec_repaired_packets += result->fec_repaired;

    if (result->have_nack)
        kn_rx_nack_request(&v->nack, result->nack_first, result->nack_last,
                           now_ms, &kf);
    if (kf) { request_keyframe(v); kf = 0; }

    if (result->have_dropped) {
        if (result->drop_reason == KN_RX_DROP_STALE)
            v->stale_packets++;
        ev_dropped(v, result->drop_reason, result->drop_index, 0);
    }

    if (result->have_recovery) {
        v->packet_gap_recoveries++;
        invalidate_picture(v, now_ms);
        kn_rx_nack_clear(&v->nack);
        ev_recovery_gap(v, result->rec_first, result->rec_last);
    }

    for (i = 0; i < result->ready_len; ++i) {
        uint32_t slot = result->ready[i];
        kn_rx_pkt *pkt = &v->pool.meta[slot];
        uint8_t *bytes = pool_bytes(&v->pool, slot);
        size_t poff, plen;
        kn_gs_video video;
        kn_gs_access_unit au;
        int rc;

        if (!pkt->rtp_ok) {
            invalidate_picture(v, now_ms);
            ev_dropped(v, KN_RX_DROP_MALFORMED_RTP, pkt->index, pkt->rtp_err);
            pool_free(&v->pool, slot);
            continue;
        }
        /* header.payload(&plaintext) — padding validation on the stored
         * plaintext (4763-4771); for FEC-recovered packets this is the
         * FIRST padding check they ever see. */
        rc = kn_gs_rtp_payload(&pkt->rtp, bytes, pkt->len, &poff, &plen);
        if (rc != KN_GS_OK) {
            invalidate_picture(v, now_ms);
            ev_dropped(v, KN_RX_DROP_MALFORMED_RTP, pkt->index, rc);
            pool_free(&v->pool, slot);
            continue;
        }
        if (!pkt->rtp.has_gs) {
            invalidate_picture(v, now_ms);
            ev_dropped(v, KN_RX_DROP_MALFORMED_RTP, pkt->index,
                       KN_GS_ERR_MISSING_VIDEO);
            pool_free(&v->pool, slot);
            continue;
        }
        rc = kn_gs_video_parse(bytes + pkt->rtp.gs_off, &video);
        if (rc != KN_GS_OK) {
            invalidate_picture(v, now_ms);
            ev_dropped(v, KN_RX_DROP_MALFORMED_RTP, pkt->index, rc);
            pool_free(&v->pool, slot);
            continue;
        }
        if (pkt->origin == KN_RX_ORIGIN_RECOVERED)
            video.is_fec = 0;            /* pitfall #4 (4786-4788) */
        if (video.is_fec) {
            v->fec_packets++;
            ev_dropped(v, KN_RX_DROP_UNSUPPORTED_FEC, pkt->index, 0);
            pool_free(&v->pool, slot);
            continue;
        }

        if (video.is_start_of_frame && v->assembler.active &&
            v->assembler.current_frame != video.frame_index) {
            /* New SOF before EOF: the previous frame was incomplete even
             * with continuous RTP indices (4790-4802). The packet itself
             * still starts the next AU — no `continue` in the reference. */
            invalidate_picture(v, now_ms);
            kn_rx_nack_clear(&v->nack);
            ev_dropped(v, KN_RX_DROP_FRAME_DISCONTINUITY, pkt->index, 0);
        }
        {
            int same_frame_continuation =
                !video.is_start_of_frame && v->assembler.active &&
                v->assembler.current_frame == video.frame_index;
            if (same_frame_continuation && v->have_last_spi &&
                !kn_gs_index_continuous(v->last_stream_packet_index,
                                        video.stream_packet_index)) {
                invalidate_picture(v, now_ms);
                ev_dropped(v, KN_RX_DROP_FRAME_DISCONTINUITY, pkt->index, 0);
                pool_free(&v->pool, slot);
                continue;                /* 4803-4818: packet NOT pushed */
            }
        }
        v->last_stream_packet_index = video.stream_packet_index;
        v->have_last_spi = 1;

        /* Round XL: the wall clock of the FIRST packet of the AU being
         * accumulated — the reference's CompletedFrameFeedback
         * timing.first_at, which frame_ack serializes as
         * first_packet_time_ms (nvst.rs:447-462). Recorded at the frame
         * boundary: either the assembler is idle, or this packet belongs
         * to a different frame than the one in progress. */
        if (!v->assembler.active ||
            v->assembler.current_frame != video.frame_index)
            v->cur_au_first_ms = now_ms;

        rc = kn_gs_au_assembler_push(&v->assembler, &video, pkt->rtp.timestamp,
                                     bytes + poff, plen, &au);
        if (rc == KN_GS_AU_COMPLETE) {
            kn_rx_frame f;
            int loss_before, admit, admitted_kf = 0;
            memset(&f, 0, sizeof f);
            f.codec = au.codec;
            f.timestamp = au.timestamp;
            f.frame_index = au.frame_index;
            f.first_stream_packet_index = au.first_stream_packet_index;
            f.frame_type_wire = au.frame_type_wire;
            f.frame_type_known = au.frame_type_known;
            f.frame_type_valid = au.frame_type_valid;
            f.keyframe = au.keyframe;
            f.contiguous = au.contiguous;
            f.bytes = au.bytes;
            f.bytes_len = au.bytes_len;
            f.first_packet_ms = v->cur_au_first_ms;

            loss_before = !v->next_frame_contiguous;
            v->next_frame_contiguous = 1;
            admit = kn_rx_inval_admit(&v->inval, &f, loss_before, now_ms,
                                      &admitted_kf);
            if (admitted_kf) request_keyframe(v);
            if (!admit) {
                v->withheld_frames++;
                pool_free(&v->pool, slot);
                continue;                /* 4836-4843: withheld by RFI */
            }
            v->frames_emitted++;
            /* FrameProgressWatchdog::assembled (2427-2437) + the receive
             * loop's publish_assembled_frame (9199). */
            v->wp_last_frame = f.frame_index;
            v->have_wp_last_frame = 1;
            v->wp_last_assembled_ms = now_ms;
            v->have_wp_last_assembled = 1;
            if (v->wp_first_keyframe_assembled || f.keyframe) {
                int closed_episode;
                v->wp_first_keyframe_assembled = 1;
                closed_episode = v->wp_stage != 0;
                v->wp_stage = 0;
                v->have_wp_keyframe_requested = 0;
                if (closed_episode) {
                    kn_rx_event e;
                    memset(&e, 0, sizeof e);
                    e.type = KN_RX_EV_PROGRESS_RESUMED;
                    e.last_frame = f.frame_index;
                    e.have_last_frame = 1;
                    ev_push(v, &e);
                }
            }
            /* publish_completed_frame (987-1005): a delivered keyframe
             * clears the pending request. */
            if (f.keyframe && v->keyframe_needed)
                v->keyframe_needed = 0;
            if (cb) cb(&f, ud);
        } else if (rc != KN_GS_AU_NEED_MORE) {
            invalidate_picture(v, now_ms);
            ev_dropped(v, au_err_to_drop(rc), pkt->index, rc);
        }
        pool_free(&v->pool, slot);
    }
}

/* Shared tail after authentication (process_datagram 4655-4726). The
 * caller validated the state and produced (index, rtp, plaintext[0..len)). */
static void process_authenticated(kn_rx_video *v, uint64_t index,
                                  const kn_gs_rtp *rtp, const uint8_t *plain,
                                  size_t plain_len, uint64_t now_ms,
                                  int feed_bw,
                                  kn_rx_frame_fn cb, void *ud)
{
    uint32_t slot, seq32;
    kn_rx_result result;
    kn_gs_fec_layout layout;
    int have_layout = 0, is_fec_path;
    int reordered = 0, retransmitted = 0;
    uint8_t *dst;
    kn_rx_pkt *pkt;

    /* expected_payload_type / expected_ssrc gates (4655-4676) */
    if (v->cfg.have_expected_pt &&
        (unsigned)rtp->pt != (unsigned)v->cfg.expected_payload_type) {
        ev_dropped(v, KN_RX_DROP_UNEXPECTED_PT, index, 0);
        return;
    }
    {
        int have_expected = v->cfg.have_expected_ssrc || v->have_bound_ssrc;
        uint32_t expected = v->cfg.have_expected_ssrc
                                ? v->cfg.expected_ssrc : v->bound_ssrc;
        if (have_expected && rtp->ssrc != expected) {
            ev_dropped(v, KN_RX_DROP_UNEXPECTED_SSRC, index, 0);
            return;
        }
    }
    if (!v->have_bound_ssrc) {
        v->bound_ssrc = rtp->ssrc;
        v->have_bound_ssrc = 1;
    }
    v->last_authenticated_ms = now_ms;
    v->have_last_authenticated = 1;
    if (v->initial_timeout_pending) {
        v->initial_timeout_pending = 0;
        if (!v->have_wp_first_authenticated) {
            v->wp_first_authenticated_ms = now_ms;
            v->have_wp_first_authenticated = 1;
        }
    }
    {
        int kf = 0;
        kn_rx_inval_poll_backstop(&v->inval, now_ms, &kf);
        if (kf) request_keyframe(v);
    }
    v->authenticated_packets++;
    seq32 = (uint32_t)(index & 0xFFFFFFFFull);
    /* `reordered` (nvst.rs:4681-4683) is evaluated against the PREVIOUS
     * highest sequence, so it must be computed before the update below.
     * Round XXXVIII: it is no longer statistics-only — it gates the
     * bandwidth-estimator feed (:4694-4702). */
    reordered = (v->authenticated_packets > 1u &&
                 seq32 <= v->highest_sequence_received);
    if (!v->have_base_sequence) {   /* round XXXVI: the RR expected-count
                                     * anchor (the reference's base_sequence,
                                     * nvst.rs:1225 — first extended seq) */
        v->base_sequence = seq32;
        v->have_base_sequence = 1;
    }
    if (seq32 > v->highest_sequence_received)
        v->highest_sequence_received = seq32;
    retransmitted = kn_rx_nack_resolve(&v->nack, index);
    if (retransmitted)
        v->recovered_retransmissions++;

    /* publish_stream_packet (nvst.rs:852-914): the QoS packet snapshot and
     * the bandwidth-estimator feed. Placed exactly where the reference has
     * it — after resolve_nack, before the FEC/reorder routing (:4703). */
    if (!v->have_qos_base) {         /* compare_exchange(u32::MAX, ..) :861 */
        v->qos_base_sequence = seq32;
        v->have_qos_base = 1;
    }
    if (seq32 > v->qos_highest_sequence)
        v->qos_highest_sequence = seq32;      /* fetch_max :867 */
    if (v->qos_received_packets < 0xFFFFFFFFu)
        v->qos_received_packets++;            /* saturating :901-906 */
    v->have_qos_snapshot = 1;
    if (feed_bw)
        bw_feed(v, rtp->ssrc, rtp, plain, plain_len, seq32, reordered,
                retransmitted, now_ms);

    /* Round XL — publish_stream_packet's reception-timing block
     * (nvst.rs:917-940), the RFC 3550 interarrival jitter in 90 kHz
     * ticks that the RTCP RR carries (:1251-1261). It runs for EVERY
     * authenticated packet (the reference's timing lock is outside the
     * `video` Option), anchors on the first one, and skips packets whose
     * RTP timestamp repeats the previous one. Note the deliberate u32
     * wrap (NOT sign extension) in rtp_ticks — `i64::from(u32
     * wrapping_sub)` — and the u64 abs_diff into the EWMA. Until round XL
     * we sent jitter=0 in every RR; the reference sends this. */
    if (!v->have_timing_first) {
        v->have_timing_first = 1;
        v->timing_first_ms = now_ms;
        v->timing_first_rtp = rtp->timestamp;
        v->timing_last_rtp = rtp->timestamp;
        v->timing_last_transit = 0;
    } else if (v->timing_last_rtp != rtp->timestamp) {
        int64_t arrival_ticks =
            (int64_t)((now_ms - v->timing_first_ms) * 90ull);
        int64_t rtp_ticks =
            (int64_t)(uint32_t)(rtp->timestamp - v->timing_first_rtp);
        int64_t transit = arrival_ticks - rtp_ticks;
        int64_t diff = transit - v->timing_last_transit;
        double delta = (double)(diff < 0 ? -diff : diff);
        v->timing_last_rtp = rtp->timestamp;
        v->timing_last_transit = transit;
        v->timing_jitter += (delta - v->timing_jitter) / 16.0;
    }

    /* Routing (4712-4725): a valid FEC layout goes to the FEC reorder
     * buffer, everything else to the plain RTP reorder buffer. */
    if (rtp->has_gs &&
        kn_gs_fec_layout_parse(plain + rtp->gs_off, &layout) == KN_GS_OK)
        have_layout = 1;
    is_fec_path = have_layout;

    slot = pool_alloc(&v->pool);
    if (slot == KN_RX_NO_SLOT) {
        ev_dropped(v, KN_RX_DROP_POOL, index, 0);
        return;
    }
    if (plain_len > (size_t)v->pool.pkt_cap) {
        pool_free(&v->pool, slot);
        ev_dropped(v, KN_RX_DROP_PKT_TOO_LARGE, index, 0);
        return;
    }
    dst = pool_bytes(&v->pool, slot);
    memcpy(dst, plain, plain_len);
    pkt = &v->pool.meta[slot];
    pkt->index = index;
    pkt->len = (uint32_t)plain_len;
    pkt->origin = KN_RX_ORIGIN_AUTH;
    pkt->rtp = *rtp;
    pkt->rtp_ok = 1;

    if (is_fec_path) {
        if (layout.shard_index >= layout.data_shards)
            v->fec_packets++;            /* parity arrival (4714-4716) */
        kn_rx_fec_push(&v->fec, &v->pool, slot, index, &layout, now_ms,
                       &result);
        if (layout.shard_index < layout.data_shards && result.have_dropped &&
            result.drop_reason == KN_RX_DROP_UNSUPPORTED_FEC)
            invalidate_picture(v, now_ms);   /* 4717-4721 */
    } else {
        kn_rx_reorder_push(&v->reorder, &v->pool, slot, index, now_ms,
                           &result);
    }
    consume_result(v, &result, now_ms, cb, ud);
}

int kn_rx_video_process_datagram(kn_rx_video *v, uint8_t *datagram, size_t len,
                                 uint64_t now_ms, kn_rx_frame_fn cb, void *ud)
{
    uint64_t index = 0;
    kn_gs_rtp rtp;
    size_t plain_len = 0;
    int detail = 0, rc;

    if (!v || !datagram) return KN_RX_ERR_ARGS;
    ev_reset(v);

    if (v->state != KN_RX_STATE_RUNNING) {
        int reason = v->state == KN_RX_STATE_PAUSED ? KN_RX_DROP_PAUSED :
                     v->state == KN_RX_STATE_STOPPED ? KN_RX_DROP_STOPPED :
                     KN_RX_DROP_RECOVERY_REQUIRED;
        ev_dropped(v, reason, 0, 0);
        return KN_RX_OK;
    }

    rc = kn_rx_srtp_unprotect(&v->srtp, datagram, len, &index, &rtp,
                              &plain_len, &detail);
    if (rc != KN_RX_OK) {
        if (rc == KN_RX_DROP_REPLAY_REJECTED) v->replay_rejections++;
        ev_dropped(v, rc > 0 ? rc : KN_RX_DROP_MALFORMED_RTP, 0, detail);
        return KN_RX_OK;
    }
    /* feed_bw = 1: this is the reference's process_datagram, the only path
     * that drives the bandwidth estimator. */
    process_authenticated(v, index, &rtp, datagram, plain_len, now_ms, 1,
                          cb, ud);
    return KN_RX_OK;
}

int kn_rx_video_process_packet(kn_rx_video *v, uint64_t index,
                               const uint8_t *plaintext, size_t len,
                               uint64_t now_ms, kn_rx_frame_fn cb, void *ud)
{
    kn_gs_rtp rtp;
    size_t poff, plen;
    int rc;

    if (!v || (!plaintext && len)) return KN_RX_ERR_ARGS;
    ev_reset(v);

    if (v->state != KN_RX_STATE_RUNNING) {
        int reason = v->state == KN_RX_STATE_PAUSED ? KN_RX_DROP_PAUSED :
                     v->state == KN_RX_STATE_STOPPED ? KN_RX_DROP_STOPPED :
                     KN_RX_DROP_RECOVERY_REQUIRED;
        ev_dropped(v, reason, index, 0);
        return KN_RX_OK;
    }
    rc = kn_gs_rtp_parse_hdr(plaintext, len, &rtp);
    if (rc != KN_GS_OK) {
        ev_dropped(v, KN_RX_DROP_MALFORMED_RTP, index, rc);
        return KN_RX_OK;
    }
    rc = kn_gs_rtp_payload(&rtp, plaintext, len, &poff, &plen);
    if (rc != KN_GS_OK) {
        ev_dropped(v, KN_RX_DROP_MALFORMED_RTP, index, rc);
        return KN_RX_OK;
    }
    rtp.payload_off = (unsigned)poff;
    rtp.payload_len = (unsigned)plen;
    /* feed_bw = 0: the plaintext entry point has no counterpart in the
     * reference's receive path (its only video=None publisher is the
     * #[cfg(test)] helper at nvst.rs:846-850), so replay/KAT runs must not
     * move estimator state — a capture replay would otherwise publish a
     * bandwidth estimate built from file timestamps. */
    process_authenticated(v, index, &rtp, plaintext, len, now_ms, 0, cb, ud);
    return KN_RX_OK;
}

int kn_rx_video_poll(kn_rx_video *v, uint64_t now_ms, kn_rx_frame_fn cb,
                     void *ud)
{
    kn_rx_result result;
    int kf = 0;
    uint64_t last, idle, tmo;

    if (!v) return KN_RX_ERR_ARGS;
    ev_reset(v);
    if (v->state != KN_RX_STATE_RUNNING)
        return KN_RX_OK;

    /* RFI backstop (poll_recovery, 4731-4733) */
    kn_rx_inval_poll_backstop(&v->inval, now_ms, &kf);
    if (kf) request_keyframe(v);

    kn_rx_reorder_poll(&v->reorder, &v->pool, now_ms, &result);
    consume_result(v, &result, now_ms, cb, ud);
    kn_rx_fec_poll(&v->fec, &v->pool, now_ms, &result);
    consume_result(v, &result, now_ms, cb, ud);

    /* poll_timeout (4526-4548) */
    last = v->have_last_authenticated ? v->last_authenticated_ms
                                      : v->timeout_origin_ms;
    idle = rx_sat_sub_u64(now_ms, last);
    tmo = v->initial_timeout_pending ? (uint64_t)v->cfg.startup_timeout_ms
                                     : (uint64_t)v->cfg.timeout_ms;
    if (idle >= tmo) {
        kn_rx_event e;
        kn_rx_video_reset_media(v, now_ms);
        v->state = KN_RX_STATE_RECOVERY_REQUIRED;
        memset(&e, 0, sizeof e);
        e.type = KN_RX_EV_RECOVERY_TIMEOUT;
        e.idle_ms = idle;
        ev_push(v, &e);
        return KN_RX_OK;    /* the spawn loop skips frame progress when the
                               packet timeout fired (9109-9114) */
    }

    /* poll_frame_progress (4563-4615), policy stall=grace=timeout
     * (9124-9126; overridable through the config). */
    if (!v->initial_timeout_pending) {
        uint64_t base = 0;
        int have_base = 0;
        if (!v->wp_first_keyframe_assembled) {
            if (v->have_wp_first_authenticated) {
                base = v->wp_first_authenticated_ms;
                have_base = 1;
            }
        } else {
            int have_a = v->have_wp_last_assembled;
            int have_e = v->have_wp_first_authenticated;
            if (have_a && have_e) {
                base = v->wp_last_assembled_ms > v->wp_first_authenticated_ms
                           ? v->wp_last_assembled_ms
                           : v->wp_first_authenticated_ms;
                have_base = 1;
            } else if (have_a) {
                base = v->wp_last_assembled_ms;
                have_base = 1;
            } else if (have_e) {
                base = v->wp_first_authenticated_ms;
                have_base = 1;
            }
        }
        if (have_base) {
            uint64_t idle_for = rx_sat_sub_u64(now_ms, base);
            if (v->wp_stage == 0) {
                if (idle_for >= (uint64_t)v->cfg.progress_stall_ms) {
                    kn_rx_event e;
                    v->wp_stage = 1;
                    v->wp_keyframe_requested_ms = now_ms;
                    v->have_wp_keyframe_requested = 1;
                    request_keyframe(v);
                    memset(&e, 0, sizeof e);
                    e.type = KN_RX_EV_PROGRESS_STALL;
                    e.idle_ms = idle_for;
                    if (v->have_wp_last_frame) {
                        e.last_frame = v->wp_last_frame;
                        e.have_last_frame = 1;
                    }
                    ev_push(v, &e);
                }
            } else if (v->wp_stage == 1) {
                uint64_t grace_started = v->have_wp_keyframe_requested
                                             ? v->wp_keyframe_requested_ms
                                             : now_ms;
                if (rx_sat_sub_u64(now_ms, grace_started) >=
                    (uint64_t)v->cfg.progress_keyframe_grace_ms) {
                    kn_rx_event e;
                    v->wp_stage = 2;
                    v->state = KN_RX_STATE_RECOVERY_REQUIRED;
                    memset(&e, 0, sizeof e);
                    e.type = KN_RX_EV_PROGRESS_RECOVERY;
                    e.idle_ms = idle_for;
                    if (v->have_wp_last_frame) {
                        e.last_frame = v->wp_last_frame;
                        e.have_last_frame = 1;
                    }
                    ev_push(v, &e);
                }
            }
            /* stage 2 (RecoveryRequired): no events (4611) */
        }
    }
    return KN_RX_OK;
}

static int lifecycle_push(kn_rx_video *v)
{
    kn_rx_event e;
    memset(&e, 0, sizeof e);
    e.type = KN_RX_EV_LIFECYCLE;
    e.new_state = v->state;
    ev_push(v, &e);
    return 1;
}

int kn_rx_video_pause(kn_rx_video *v, uint64_t now_ms)
{
    if (!v || v->state != KN_RX_STATE_RUNNING) return 0;
    ev_reset(v);
    kn_rx_video_reset_media(v, now_ms);
    v->state = KN_RX_STATE_PAUSED;
    return lifecycle_push(v);
}

int kn_rx_video_resume(kn_rx_video *v, uint64_t now_ms)
{
    if (!v || v->state != KN_RX_STATE_PAUSED) return 0;
    ev_reset(v);
    kn_rx_video_reset_media(v, now_ms);
    v->have_last_authenticated = 0;
    v->timeout_origin_ms = now_ms;
    v->initial_timeout_pending = 0;
    v->state = KN_RX_STATE_RUNNING;
    return lifecycle_push(v);
}

int kn_rx_video_recover(kn_rx_video *v, uint64_t now_ms)
{
    if (!v || v->state != KN_RX_STATE_RECOVERY_REQUIRED) return 0;
    ev_reset(v);
    kn_rx_video_reset_media(v, now_ms);
    v->have_last_authenticated = 0;
    v->timeout_origin_ms = now_ms;
    v->initial_timeout_pending = 0;
    v->state = KN_RX_STATE_RUNNING;
    return lifecycle_push(v);
}

int kn_rx_video_stop(kn_rx_video *v, uint64_t now_ms)
{
    if (!v || v->state == KN_RX_STATE_STOPPED) return 0;
    ev_reset(v);
    kn_rx_video_reset_media(v, now_ms);
    v->state = KN_RX_STATE_STOPPED;
    return lifecycle_push(v);
}

const kn_rx_event *kn_rx_video_events(const kn_rx_video *v, uint32_t *n)
{
    if (!v || !n) { if (n) *n = 0; return NULL; }
    *n = v->ev_count;
    return v->mem.events;
}

int kn_rx_video_take_keyframe_request(kn_rx_video *v)
{
    int had;
    if (!v) return 0;
    had = v->keyframe_needed;
    v->keyframe_needed = 0;
    return had;
}

int kn_rx_video_keyframe_request_pending(const kn_rx_video *v)
{
    /* keyframe_request_pending (nvst.rs:1266-1268): the NON-clearing read
     * the 100 ms IDR/PLI retry loop gates on. Only an assembled keyframe
     * clears the latch (:994 — mirrored at the emit site below). */
    return v ? v->keyframe_needed : 0;
}

void kn_rx_video_request_keyframe(kn_rx_video *v)
{
    /* Public wrapper over the internal loss-path helper (same flag, same
     * counter discipline): the stage-2 decode glue calls it when the
     * encoded-frame queue overflows — analiza/04 §5 "encoded queue
     * overflow -> clear the reference chain, request a keyframe". */
    if (!v) return;
    request_keyframe(v);
}
