/* kn_twcc.c — TWCC receive register + feedback builder. Contract and
 * str0m provenance in kn_twcc.h; wire format draft-holmer-rmcat-
 * transport-wide-cc-extensions-01. Golden vectors: tools/twcc_reference.py
 * -> kn_twcc_kat_vectors.h (the reference re-derives every report byte
 * from the same receipt streams; test_kntwcc pins C against it). */
#include "kn/kn_twcc.h"

#include <string.h>

#define KN_TWCC_REF_STEP_US   64000ull   /* reference_time granularity */
#define KN_TWCC_DELTA_UNIT_US 250        /* recv-delta granularity */
#define KN_TWCC_SMALL_MAX_US  63750      /* max small delta (255*250) */
#define KN_TWCC_LARGE_MIN_US  (-8192000) /* i16*250 floor */
#define KN_TWCC_LARGE_MAX_US  8191750    /* i16*250 ceiling */
#define KN_TWCC_MAX_RUN       8192u      /* 13-bit run length */

void kn_twcc_init(kn_twcc *t)
{
    if (!t) return;
    memset(t, 0, sizeof *t);
}

/* Logical entry i -> ring slot. */
static unsigned tw_slot(const kn_twcc *t, unsigned i)
{
    return (t->head + i) % KN_TWCC_CAPACITY;
}

/* Binary search: lowest logical index with seq >= target (count = end). */
static unsigned tw_lower_bound(const kn_twcc *t, uint64_t target)
{
    unsigned lo = 0, hi = t->count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        if (t->seq[tw_slot(t, mid)] < target) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

void kn_twcc_record(kn_twcc *t, uint16_t wire_seq, uint64_t arrival_us)
{
    uint64_t ext;
    unsigned idx, i;
    if (!t) return;

    /* 16 -> 64 extension: signed distance from the last seen wire seq
     * (the same wrap semantics kn_srtp_replay_guess uses for RTP). */
    if (!t->have_last) {
        ext = wire_seq;
        t->have_last = 1;
        t->last_ext = ext;
    } else {
        int16_t d = (int16_t)(wire_seq - (uint16_t)(t->last_ext & 0xFFFFull));
        ext = (uint64_t)((int64_t)t->last_ext + (int64_t)d);
        if (ext > t->last_ext) t->last_ext = ext;
    }

    /* str0m's representability guard: a receipt whose time went back more
     * than 8192 ms below the report epoch cannot be delta-encoded — drop
     * it (counted) instead of poisoning the next report. Only applies once
     * time_start exists (the first report seeds it from whatever came). */
    if (t->have_time_start &&
        t->time_start_us > arrival_us + 8192000ull) {
        ++t->old_time_dropped;
        return;
    }

    /* the dropped-history floor: arrivals below the ext seq of the oldest
     * KEPT receipt are unplaceable once trims/evictions removed the past
     * (str0m's unbounded deque never needs this; before the first trim
     * there is no floor and ANY out-of-order arrival inserts freely). */
    if (t->have_floor && ext < t->floor_ext) {
        ++t->late_dropped;
        return;
    }

    idx = 0;
    if (t->count) {
        idx = tw_lower_bound(t, ext);
        if (idx < t->count && t->seq[tw_slot(t, idx)] == ext) {
            ++t->dup_ignored;        /* sender reused a seq: ignore */
            return;
        }
    }

    if (t->count == KN_TWCC_CAPACITY) {
        /* ring full: evict the oldest (only reachable when builds stall
         * for seconds; counted so the flight can see it). The floor only
         * moves when the popped receipt was REPORTED — that is the exact
         * invariant: floor = reported history we deliberately forgot.
         * Evicting an unreported receipt forgets nothing, so out-of-window
         * late arrivals below it stay placeable (str0m-like). */
        int popped_reported = (t->report_from > 0);
        t->head = (t->head + 1u) % KN_TWCC_CAPACITY;
        --t->count;
        if (t->report_from) --t->report_from;
        ++t->evicted;
        if (idx) --idx;
        if (popped_reported) {
            t->have_floor = 1;
            t->floor_ext = t->seq[tw_slot(t, 0)];   /* new oldest */
        }
    }

    /* insert at logical idx: shift the tail up by one */
    for (i = t->count; i > idx; --i) {
        unsigned dst = tw_slot(t, i), src = tw_slot(t, i - 1u);
        t->seq[dst] = t->seq[src];
        t->arrival_us[dst] = t->arrival_us[src];
    }
    t->seq[tw_slot(t, idx)] = ext;
    t->arrival_us[tw_slot(t, idx)] = arrival_us;
    ++t->count;
    if (idx < t->report_from) t->report_from = idx;   /* str0m: re-report */
    ++t->recorded;
}

int kn_twcc_has_unreported(const kn_twcc *t)
{
    return (t && t->count > t->report_from) ? 1 : 0;
}

/* Append one received status to the run list, coalescing same-status runs
 * up to 8192. Returns 0 ok, -1 when the chunk table is full. */
static int tw_push_run(kn_twcc_chunk *chunks, unsigned *n_chunks,
                       uint8_t status, unsigned run)
{
    while (run) {
        unsigned part = run > KN_TWCC_MAX_RUN ? KN_TWCC_MAX_RUN : run;
        if (*n_chunks &&
            chunks[*n_chunks - 1u].status == status &&
            chunks[*n_chunks - 1u].run + part <= KN_TWCC_MAX_RUN) {
            chunks[*n_chunks - 1u].run = (uint16_t)(
                chunks[*n_chunks - 1u].run + part);
        } else {
            if (*n_chunks == KN_TWCC_MAX_CHUNKS) return -1;
            chunks[*n_chunks].status = status;
            chunks[*n_chunks].run = (uint16_t)part;
            ++*n_chunks;
        }
        run -= part;
    }
    return 0;
}

size_t kn_twcc_build(kn_twcc *t, uint32_t sender_ssrc, uint32_t media_ssrc,
                     uint8_t *out, size_t out_len)
{
    kn_twcc_chunk chunks[KN_TWCC_MAX_CHUNKS];
    int16_t dval[KN_TWCC_MAX_DELTAS];
    uint8_t  dbig[KN_TWCC_MAX_DELTAS];
    unsigned n_chunks = 0, n_deltas = 0, consumed = 0;
    unsigned long status_count = 0;
    unsigned long delta_bytes = 0;
    uint64_t base_seq, first_time, time_start, base_time;
    uint64_t prev_seq, prev_time;
    uint32_t reference_time;
    size_t total;
    unsigned ri;
    int pad;

    if (!t || !out) return 0;
    if (t->count <= t->report_from) return 0;
    /* 20 fixed + one chunk (2) + one large delta (2) + pad slack (3):
     * the conservative per-receipt budget below needs 28 to admit even a
     * single receipt; production callers pass ~1200 (str0m: mtu-100). */
    if (out_len < 28u) return 0;

    base_seq   = t->seq[tw_slot(t, t->report_from)];
    first_time = t->arrival_us[tw_slot(t, t->report_from)];
    if (!t->have_time_start) {
        t->have_time_start = 1;
        t->time_start_us = first_time;
    }
    time_start = t->time_start_us;
    /* first_time < time_start is only reachable when a late pull-back
     * receipt carries a time before the epoch (arrival clocks are
     * monotone per process — str0m's Instant relies on the same
     * invariant). Clamp to 0: the record-time guard (drop arrivals more
     * than 8192 ms before time_start) keeps the first delta inside the
     * large-delta range, so the report stays representable. */
    reference_time = (first_time >= time_start)
        ? (uint32_t)((first_time - time_start) / KN_TWCC_REF_STEP_US)
          & 0xFFFFFFu
        : 0u;
    /* the quantized epoch this report's first delta is relative to */
    base_time = time_start + (uint64_t)reference_time * KN_TWCC_REF_STEP_US;

    prev_seq = base_seq;
    prev_time = base_time;
    for (ri = t->report_from; ri < t->count; ++ri) {
        uint64_t rseq = t->seq[tw_slot(t, ri)];
        uint64_t rtime = t->arrival_us[tw_slot(t, ri)];
        uint64_t gap = rseq - prev_seq;
        int64_t diff_time;
        int32_t d250;
        uint8_t status;
        size_t add;

        /* budget: what this receipt would cost — the missing run before
         * it (2 bytes per 8192-run chunk, ceil) + a new chunk slot worst
         * case (2 — coalescing may refund it) + its delta (1 or 2) + up
         * to 3 pad bytes. Deliberately conservative: truncation one
         * receipt early is invisible (it just rides the next report). */
        add = (gap > 1u)
            ? 2u * (unsigned)((gap - 1u + 8191u) / 8192u)
            : 0u;
        add += 2u;                          /* worst-case new run chunk */

        diff_time = (int64_t)(rtime - prev_time);
        if (diff_time < KN_TWCC_LARGE_MIN_US || diff_time > KN_TWCC_LARGE_MAX_US) {
            /* unrepresentable — end the report here, start anew next
             * time (str0m's break; report_from stays put for the receipt
             * itself, so the NEXT report re-bases on it). */
            t->reports_truncated++;
            break;
        }
        if (diff_time < 0 || diff_time > (int64_t)KN_TWCC_SMALL_MAX_US) {
            status = KN_TWCC_ST_LARGE_DELTA;
            d250 = (int32_t)(diff_time / KN_TWCC_DELTA_UNIT_US);
            add += 2u;
        } else {
            status = KN_TWCC_ST_SMALL_DELTA;
            d250 = (int32_t)(diff_time / KN_TWCC_DELTA_UNIT_US);
            add += 1u;
        }
        if (20u + 2u * (size_t)n_chunks + delta_bytes + add + 3u > out_len) {
            t->reports_truncated++;
            break;                          /* budget: leave for next fb */
        }
        if (n_deltas == KN_TWCC_MAX_DELTAS) {
            t->reports_truncated++;
            break;
        }
        if (status_count + (gap > 1u ? gap - 1u : 0u) + 1u > 65535ul) {
            t->reports_truncated++;
            break;
        }

        /* the missing run(s) before this receipt */
        if (gap > 1u) {
            if (tw_push_run(chunks, &n_chunks, KN_TWCC_ST_NOT_RECEIVED,
                            (unsigned)(gap - 1u)) != 0) {
                t->reports_truncated++;
                break;
            }
            status_count += (unsigned long)(gap - 1u);
        }
        /* the receipt itself */
        if (tw_push_run(chunks, &n_chunks, status, 1u) != 0) {
            t->reports_truncated++;
            break;
        }
        status_count += 1u;
        dval[n_deltas] = (int16_t)d250;
        dbig[n_deltas] = (uint8_t)(status == KN_TWCC_ST_LARGE_DELTA);
        ++n_deltas;
        delta_bytes += (status == KN_TWCC_ST_LARGE_DELTA) ? 2u : 1u;

        /* str0m: the next delta is relative to the time THIS delta
         * REPORTS (quantized), not the true arrival — truncations must
         * not compound across the report. Negative step clamps at
         * prev_time (str0m checked_sub().unwrap_or(prev.1)). */
        {
            uint64_t step = (uint64_t)(d250 < 0 ? -(int64_t)d250
                                                : (int64_t)d250)
                            * KN_TWCC_DELTA_UNIT_US;
            prev_time = (diff_time < 0)
                ? ((prev_time >= step) ? prev_time - step : prev_time)
                : prev_time + step;
        }
        prev_seq = rseq;
        ++consumed;
    }

    if (!consumed) return 0;                /* nothing encodable yet */

    /* ---- emit ---- */
    total = 20u + 2u * (size_t)n_chunks + delta_bytes;
    pad = (int)((4u - (total % 4u)) % 4u);
    total += (size_t)pad;
    if (total > out_len) {
        /* cannot happen with the budget above; honest refusal anyway */
        t->reports_truncated++;
        return 0;
    }
    memset(out, 0, total);
    out[0] = 0x8Fu;                         /* V=2 P=0 FMT=15 */
    out[1] = 205u;                          /* RTPFB */
    out[2] = (uint8_t)(((total / 4u) - 1u) >> 8);
    out[3] = (uint8_t)(((total / 4u) - 1u) & 0xFFu);
    out[4] = (uint8_t)(sender_ssrc >> 24);
    out[5] = (uint8_t)(sender_ssrc >> 16);
    out[6] = (uint8_t)(sender_ssrc >> 8);
    out[7] = (uint8_t)sender_ssrc;
    out[8] = (uint8_t)(media_ssrc >> 24);
    out[9] = (uint8_t)(media_ssrc >> 16);
    out[10] = (uint8_t)(media_ssrc >> 8);
    out[11] = (uint8_t)media_ssrc;
    out[12] = (uint8_t)(base_seq >> 8);     /* low 16 of the ext seq */
    out[13] = (uint8_t)base_seq;
    out[14] = (uint8_t)(status_count >> 8);
    out[15] = (uint8_t)status_count;
    out[16] = (uint8_t)(reference_time >> 16);
    out[17] = (uint8_t)(reference_time >> 8);
    out[18] = (uint8_t)reference_time;
    out[19] = (uint8_t)(t->generated_reports & 0xFFu);
    {
        size_t o = 20u, k;
        for (k = 0; k < n_chunks; ++k) {
            uint16_t x = (uint16_t)(((uint16_t)chunks[k].status << 13) |
                                    chunks[k].run);
            out[o++] = (uint8_t)(x >> 8);
            out[o++] = (uint8_t)x;
        }
        for (k = 0; k < n_deltas; ++k) {
            if (dbig[k]) {
                out[o++] = (uint8_t)((uint16_t)dval[k] >> 8);
                out[o++] = (uint8_t)((uint16_t)dval[k] & 0xFFu);
            } else {
                out[o++] = (uint8_t)dval[k];
            }
        }
        if (pad) {
            out[0] |= 0x20u;                /* P bit */
            out[total - 1u] = (uint8_t)pad;
        }
    }

    /* ---- bookkeeping (str0m build_report tail) ---- */
    t->report_from += consumed;
    t->generated_reports++;
    if (t->report_from > KN_TWCC_KEEP_REPORTED) {
        unsigned remove = t->report_from - KN_TWCC_KEEP_REPORTED;
        if (remove > t->count) remove = t->count;
        t->head = (t->head + remove) % KN_TWCC_CAPACITY;
        t->count -= remove;
        t->report_from -= remove;
        if (t->count) {
            t->have_floor = 1;
            t->floor_ext = t->seq[tw_slot(t, 0)];  /* history below is gone */
        }
    }
    return total;
}
