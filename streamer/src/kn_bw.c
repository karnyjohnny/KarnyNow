/* kn_bw.c — see kn_bw.h. A byte-faithful port of nvst_bandwidth.rs and
 * the nvst_server_control.rs decoder (OpenNOW @ a49b124).
 *
 * PORTING DISCIPLINE (the reason this file is longer than the Rust):
 * every float->integer conversion in the reference is a Rust `as` cast,
 * which SATURATES to the target range and then reinterprets the bit
 * pattern for unsigned targets. C's conversion is UB out of range, so
 * each cast site gets an explicit saturating helper below. The masks
 * (`& 0x7fff_ffff`, `& 0xffff`) and the truncation-toward-zero semantics
 * are reproduced literally, because the server reads these fields as
 * wire integers and the reference's own tests pin their exact values
 * (nvst_bandwidth.rs:277-282 asserts minimum_server_time == 270090).
 */
#include "kn/kn_bw.h"

#include <string.h>

/* ---------------------------------------------------------- f64 -> ints
 * Rust `as` semantics: NaN -> 0; below min -> min; above max -> max;
 * otherwise truncate toward zero. The unsigned cases then reinterpret
 * the (possibly negative) two's-complement bit pattern, so a saturating
 * i64 step followed by a mask is exact. */
#define KN_BW_I64_MIN (-9223372036854775807LL - 1LL)
#define KN_BW_I64_MAX 9223372036854775807LL

static int64_t bw_sat_i64(double d)
{
    if (!(d == d)) return 0;                       /* NaN */
    if (d <= -9223372036854775808.0) return KN_BW_I64_MIN;
    if (d >= 9223372036854775807.0) return KN_BW_I64_MAX;
    return (int64_t)d;                             /* |d| in range: OK */
}

static uint64_t bw_sat_u64(double d)
{
    int64_t s = bw_sat_i64(d);
    /* (x as u64) for a negative saturating i64 reinterprets the pattern,
     * which for KN_BW_I64_MIN is 0x8000000000000000 — the same value C
     * gives for the two's-complement bit pattern, so a plain cast of the
     * unsigned reinterpretation is exact. */
    return (uint64_t)s;
}

static uint32_t bw_as_u32(double d)
{
    return (uint32_t)(bw_sat_u64(d) & 0xFFFFFFFFull);
}

static uint16_t bw_as_u16(double d)
{
    return (uint16_t)(bw_sat_u64(d) & 0xFFFFull);
}

static uint8_t bw_as_u8(double d)
{
    return (uint8_t)(bw_sat_u64(d) & 0xFFull);
}

/* Duration::as_secs_f64() * 1000.0 on a saturating nanosecond delta. The
 * split keeps the conversion exact for every clock value a session can
 * produce (see kn_bw_sample.received_ns for why exactness is load-bearing:
 * the reference's utilization_percent == 5 test fails at 4 without it). */
static double bw_dur_ms(int64_t ns)
{
    if (ns < 0) ns = 0;                              /* saturating_duration */
    return (double)(ns / 1000000000LL) * 1000.0
         + (double)(ns % 1000000000LL) / 1000000.0;
}

static int64_t bw_sat_sub_ns(int64_t a, int64_t b)
{
    return a > b ? a - b : 0;                        /* saturating_duration_since */
}

static double bw_max_d(double a, double b) { return a > b ? a : b; }
static double bw_min_d(double a, double b) { return a < b ? a : b; }

static uint32_t bw_sat_add_u32(uint32_t a, uint32_t b)
{
    uint64_t s = (uint64_t)a + (uint64_t)b;
    return s > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)s;
}

static uint64_t bw_sat_add_u64(uint64_t a, uint64_t b)
{
    return a > 0xFFFFFFFFFFFFFFFFull - b ? 0xFFFFFFFFFFFFFFFFull : a + b;
}

static uint64_t bw_sat_sub_u64(uint64_t a, uint64_t b)
{
    return a > b ? a - b : 0u;
}

/* smooth(previous, value) (:245-247) — EWMA 0.875/0.125, seeded by the
 * first sample (Option::map_or(value, ...)). */
static double bw_smooth(int have_prev, double prev, double value)
{
    return have_prev ? prev * 0.875 + value * 0.125 : value;
}

/* ============================================================ estimator */

void kn_bw_init(kn_bw *b)
{
    if (!b) return;
    memset(b, 0, sizeof *b);
}

void kn_bw_observe_parity(kn_bw *b, uint32_t frame_number, uint32_t sequence)
{
    if (!b || !b->have_active_frame) return;        /* :81-82 */
    if (b->af_number != frame_number) return;
    if (!(sequence > b->af_last_sequence)) return;
    if (sequence != b->af_last_sequence + 1u)
        b->af_contiguous = 0;
    b->af_last_sequence = sequence;
}

void kn_bw_observe(kn_bw *b, const kn_bw_sample *s)
{
    double transit_ms, now_ms, extended_d;
    int64_t at;
    int32_t delta;
    uint64_t extended;

    if (!b || !s) return;
    at = s->received_ns;

    /* let first = *self.first_arrival.get_or_insert(at);  (:100) */
    if (!b->have_first_arrival) {
        b->have_first_arrival = 1;
        b->first_arrival_ns = at;
    }

    /* the wrap-extended sender clock (:101-113) */
    if (b->have_last_timestamp) {
        delta = (int32_t)(s->timestamp - b->last_timestamp);   /* wrapping_sub */
        if (delta < 0) return;                        /* a stale/re-run clock
                                                       * contributes nothing */
    }
    extended = b->have_last_timestamp
        ? b->extended_timestamp + (uint64_t)(int32_t)(s->timestamp -
                                                      b->last_timestamp)
        : (uint64_t)s->timestamp;
    b->have_last_timestamp = 1;
    b->last_timestamp = s->timestamp;
    b->extended_timestamp = extended;

    /* transit_ms = arrival_since_first - extended/90 (:114-115) */
    now_ms = bw_dur_ms(bw_sat_sub_ns(at, b->first_arrival_ns));
    extended_d = (double)extended;
    transit_ms = now_ms - extended_d / 90.0;

    if (!b->have_minimum_transit) {                  /* get_or_insert (:116) */
        b->have_minimum_transit = 1;
        b->minimum_transit_ms = transit_ms;
    }
    if (transit_ms < b->minimum_transit_ms) b->minimum_transit_ms = transit_ms;
    b->latest_transit_ms = transit_ms;

    if (b->have_previous_transit) {                  /* EWMA /16 (:118-120) */
        double diff = transit_ms - b->previous_transit_ms;
        if (diff < 0.0) diff = -diff;
        b->jitter_ms += (diff - b->jitter_ms) / 16.0;
    }
    b->have_previous_transit = 1;
    b->previous_transit_ms = transit_ms;

    b->transits[b->transit_next] = transit_ms;       /* ring of 16 (:121-123) */
    b->transit_next = (b->transit_next + 1u) % KN_BW_TRANSIT_SAMPLES;
    if (b->transit_count < KN_BW_TRANSIT_SAMPLES) b->transit_count++;

    /* ---- the same frame continues (:125-154) ---- */
    if (b->have_active_frame && b->af_number == s->frame_number) {
        double sender_gap_ms, receive_gap_ms;
        if (s->sequence <= b->af_last_sequence) {
            b->af_contiguous = 0;
            return;                                   /* :127-130 */
        }
        if (s->sequence != b->af_last_sequence + 1u)
            b->af_contiguous = 0;
        sender_gap_ms = (double)bw_sat_sub_u64(extended, b->af_last_ts) / 90.0;
        receive_gap_ms = bw_dur_ms(bw_sat_sub_ns(at, b->af_last_ns));
        if (sender_gap_ms >= 1.0)
            b->af_sender_gap_ms += bw_min_d(sender_gap_ms, receive_gap_ms);
        b->af_last_ns = at;
        b->af_last_ts = extended;
        b->af_last_sequence = s->sequence;
        b->af_bytes = bw_sat_add_u64(b->af_bytes, (uint64_t)s->bytes);
        if (s->ended) b->af_ended = 1;
        return;
    }

    /* ---- a different frame number: close the previous train (:156-195) ---- */
    if (b->have_active_frame) {
        uint32_t distance = s->frame_number - b->af_number;  /* wrapping_sub */
        if (distance == 0u || distance >= 0x80000000u)
            return;                    /* active_frame is left untouched — the
                                        * reference `take()`s it and puts it
                                        * back, so a reordered/older frame
                                        * number neither closes nor replaces
                                        * the train (:157-161) */
        b->lossy_frames = bw_sat_add_u32(b->lossy_frames,
                                         distance > 1u ? distance - 1u : 0u);
        if (b->af_started && b->af_ended && b->af_contiguous) {
            double frame_ms =
                (double)bw_sat_sub_u64(extended, b->af_first_ts) / 90.0;
            double received_ms =
                bw_max_d(bw_dur_ms(bw_sat_sub_ns(b->af_last_ns,
                                                 b->af_first_ns))
                         - b->af_sender_gap_ms, 0.5);
            if (frame_ms > 0.0 && frame_ms < 1000.0) {
                double receive_kbps;
                b->average_frame_ms =
                    bw_smooth(b->have_average_frame_ms,
                              b->average_frame_ms, frame_ms);
                b->have_average_frame_ms = 1;
                receive_kbps = (double)b->af_bytes * 8.0 / received_ms;
                /* A train shorter than the receive clock can resolve only
                 * bounds capacity from BELOW: it may raise the estimate but
                 * must not drag it down (:176-186). */
                if (received_ms >= 1.0 ||              /* MIN_MEASURABLE_TRAIN_MS */
                    !b->have_average_receive_kbps ||
                    receive_kbps > b->average_receive_kbps) {
                    b->average_receive_kbps =
                        bw_smooth(b->have_average_receive_kbps,
                                  b->average_receive_kbps, receive_kbps);
                    b->have_average_receive_kbps = 1;
                }
                b->average_utilization_percent =
                    bw_smooth(b->have_average_utilization,
                              b->average_utilization_percent,
                              bw_min_d(100.0 * received_ms /
                                       b->average_frame_ms, 100.0));
                b->have_average_utilization = 1;
                b->frames_observed = bw_sat_add_u32(b->frames_observed, 1u);
                if (b->frames_observed % 60u == 0u && b->have_minimum_transit)
                    b->minimum_transit_ms += 0.5;   /* :191-194 */
            }
        } else {
            b->lossy_frames = bw_sat_add_u32(b->lossy_frames, 1u);
        }
    }

    /* ---- open the new train (:196-207) ---- */
    b->have_active_frame = 1;
    b->af_number = s->frame_number;
    b->af_first_ns = at;
    b->af_last_ns = at;
    b->af_first_ts = extended;
    b->af_last_ts = extended;
    b->af_last_sequence = s->sequence;
    b->af_bytes = (uint64_t)s->bytes;
    b->af_sender_gap_ms = 0.0;
    b->af_started = s->started ? 1 : 0;
    b->af_ended = s->ended ? 1 : 0;
    b->af_contiguous = 1;
}

/* total_cmp ordering is irrelevant here (no NaN can enter: every transit
 * is a difference of finite clock values), so a plain insertion sort on
 * <= 16 doubles is exact and dependency-free. */
static void bw_sort_transits(double *v, unsigned n)
{
    unsigned i, j;
    for (i = 1; i < n; ++i) {
        double key = v[i];
        j = i;
        while (j > 0u && v[j - 1u] > key) { v[j] = v[j - 1u]; --j; }
        v[j] = key;
    }
}

void kn_bw_report(const kn_bw *b, int64_t at_ns, kn_bw_feedback *out)
{
    double samples[KN_BW_TRANSIT_SAMPLES];
    double median, now_ms;
    unsigned i;

    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!b) return;
    /* let (Some(first), Some(minimum)) = ... else return default  (:211-214) */
    if (!b->have_first_arrival || !b->have_minimum_transit) return;
    if (b->transit_count == 0u) return;   /* (count - 1) would underflow. The
                                           * reference cannot reach this state
                                           * (a minimum exists only after one
                                           * observe(), which also fills slot
                                           * 0) — guarded, not guessed. */

    for (i = 0; i < b->transit_count; ++i) samples[i] = b->transits[i];
    bw_sort_transits(samples, b->transit_count);
    median = samples[(b->transit_count - 1u) / 2u];

    now_ms = bw_dur_ms(bw_sat_sub_ns(at_ns, b->first_arrival_ns));

    /* server_time(transit) = ((now - transit).max(0) * 90) as u64 & 0x7fffffff
     * (:218-219) */
    {
        uint64_t mn = bw_sat_u64(bw_max_d(now_ms - b->minimum_transit_ms, 0.0) * 90.0);
        uint64_t md = bw_sat_u64(bw_max_d(now_ms - median, 0.0) * 90.0);
        out->minimum_server_time = (uint32_t)(mn & 0x7FFFFFFFull);
        out->median_server_time  = (uint32_t)(md & 0x7FFFFFFFull);
    }
    out->queue_delay_us = bw_as_u32(
        bw_max_d(b->latest_transit_ms - b->minimum_transit_ms, 0.0) * 1000.0);
    out->jitter_us = bw_as_u16(bw_min_d(b->jitter_ms * 1000.0, 65535.0));
    out->utilization_percent =
        (b->frames_observed >= 100u && b->have_average_utilization)
            ? bw_as_u8(b->average_utilization_percent) : 0u;
    out->estimate_kbps =
        (b->frames_observed >= 100u && b->have_average_receive_kbps)
            ? bw_as_u32(b->average_receive_kbps) : 0u;
    out->lossy_frames = b->lossy_frames;
}

uint32_t kn_bw_frames_observed(const kn_bw *b)
{
    return b ? b->frames_observed : 0u;
}

/* ======================================================= server control */

void kn_sctl_diag_init(kn_sctl_diag *d)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
}

/* session_resumable (:23-34) */
int kn_sctl_resumable(uint32_t reason, uint32_t extended_code)
{
    if (reason == 0x37u) {
        int excluded =
            extended_code == 0x80040003u ||
            (extended_code >= 0x8004000bu && extended_code <= 0x8004000fu) ||
            (extended_code >= 0x80040011u && extended_code <= 0x80040016u);
        return excluded ? 0 : 1;
    }
    if (reason == 0x3bu) {
        int included = extended_code == 0x80040004u ||
                       extended_code == 0x80040007u ||
                       extended_code == 0x80040010u;
        return included ? 1 : 0;
    }
    return 0;                       /* _ => Some(false) */
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* One record decode. `rec` receives the record's code even when the
 * payload turns out to be malformed (the reference's `code` is read
 * before decode_control_message runs). Returns 0 when the payload is
 * malformed (and sets *err), 1 otherwise. `*consumed` is the record's
 * full on-wire length, which the CALLER applies only on success — the
 * iterator discards the rest of the buffer on error. */
static int sctl_decode(uint16_t code, const uint8_t *p, uint16_t plen,
                       kn_sctl_msg *out, kn_sctl_err *err)
{
    out->code = code;
    switch (code) {
    case 0x104: case 0x105:
        if (plen < 4u) { *err = KN_SCTL_ERR_TRUNCATED; return 0; }  /* word(0) */
        out->kind = KN_SCTL_TIMER;
        out->timer_type = (code == 0x104u) ? 4u : 5u;
        out->seconds_left = rd_u32le(p);
        out->timer_active = (code == 0x104u) ? 1 : 0;
        return 1;
    case 0x109:
        if (plen < 4u) { *err = KN_SCTL_ERR_TRUNCATED; return 0; }
        out->kind = KN_SCTL_TERMINATION;
        out->wire_code = rd_u32be(p);         /* from_be_bytes (:167-171) */
        if (out->wire_code == 0x80030023u) {          /* from_wire (:63-73) */
            out->have_reason = 1; out->reason = 0x38u;
            out->have_extended = 1; out->extended_code = 0x80030007u;
        } else if (out->wire_code == 0x80040010u) {
            out->have_reason = 1; out->reason = 0x37u;
            out->have_extended = 1; out->extended_code = 0x80040010u;
        }
        if (out->have_reason && out->have_extended) {
            out->have_resumable = 1;
            out->resumable = kn_sctl_resumable(out->reason,
                                               out->extended_code);
        }
        return 1;
    case 0x112:
        if (plen != 8u) { *err = KN_SCTL_ERR_INVALID_LENGTH; return 0; }
        out->kind = KN_SCTL_PACING_MODE;
        out->stream_index = rd_u32le(p);
        out->pacing_interval = (rd_u32le(p + 4u) == 1u) ? 1 : 0;
        return 1;
    case 0x101: case 0x102: case 0x103: case 0x106:
    case 0x10a: case 0x10b: case 0x10c: case 0x10d: case 0x10e:
    case 0x10f: case 0x110: case 0x111: case 0x113:
    case 0x206: case 0x325: case 0x408:
        out->kind = KN_SCTL_UNHANDLED;        /* :186-188 */
        return 1;
    default:
        out->kind = KN_SCTL_UNKNOWN;          /* :189 */
        return 1;
    }
}

size_t kn_sctl_walk(const uint8_t *data, size_t len, kn_sctl_msg *out,
                    size_t max_out)
{
    size_t off = 0, n = 0, remaining = KN_SCTL_MAX_COMMANDS;

    if (!out || max_out == 0u) return 0;
    memset(out, 0, max_out * sizeof out[0]);

    /* Oversized is decided at CONSTRUCTION in the reference (`error:
     * (bytes.len() > MAX).then_some(..)`, :110-112) and is the only user of
     * its latched error field: it is yielded first and the walk ends. */
    if (len > (size_t)KN_SCTL_MAX_BYTES) {
        out[0].kind = KN_SCTL_MALFORMED;
        out[0].error = KN_SCTL_ERR_OVERSIZED;
        return 1;
    }
    if (data == NULL && len > 0u) return 0;

    while (off < len) {
        uint16_t code, plen;
        kn_sctl_err err = KN_SCTL_ERR_NONE;

        /* BOUNDS errors empty the buffer (`self.bytes = &[]`) without
         * latching, so exactly one is ever produced and the walk ends. */
        if (remaining == 0u) {                 /* :132-135 TooManyCommands */
            if (n >= max_out) break;
            out[n].kind = KN_SCTL_MALFORMED;
            out[n].error = KN_SCTL_ERR_TOO_MANY;
            n++;
            break;
        }
        if (len - off < 4u) {                  /* :136-137 Truncated */
            if (n >= max_out) break;
            out[n].kind = KN_SCTL_MALFORMED;
            out[n].error = KN_SCTL_ERR_TRUNCATED;
            n++;
            break;
        }
        code = rd_u16le(data + off);
        plen = rd_u16le(data + off + 2u);
        if ((size_t)plen > len - off - 4u) {   /* get(4..4+len) -> None (:138) */
            if (n >= max_out) break;
            out[n].kind = KN_SCTL_MALFORMED;
            out[n].error = KN_SCTL_ERR_TRUNCATED;
            n++;
            break;
        }
        if (n >= max_out) break;               /* the caller's array is the
                                                * bound; the record is left
                                                * unconsumed so a bigger array
                                                * sees strictly more */
        remaining--;
        off += 4u + (size_t)plen;
        if (!sctl_decode(code, data + off - (size_t)plen, plen, &out[n], &err)) {
            /* A DECODE error is just `Some(decode_control_message(..))`
             * (:145): the record is consumed and the walk CONTINUES. */
            out[n].kind = KN_SCTL_MALFORMED;
            out[n].error = err;
            out[n].code = code;
        }
        n++;
    }
    return n;
}

void kn_sctl_diag_add(kn_sctl_diag *d, const kn_sctl_msg *m)
{
    if (!d || !m) return;
    switch (m->kind) {
    case KN_SCTL_UNKNOWN:
        if (d->unknown != (unsigned long long)-1) d->unknown++;
        d->have_last_unknown = 1;
        d->last_unknown = m->code;
        break;
    case KN_SCTL_MALFORMED:
        if (d->malformed != (unsigned long long)-1) d->malformed++;
        break;
    case KN_SCTL_PACING_MODE:
        if (m->stream_index != 0u &&
            d->invalid_stream != (unsigned long long)-1)
            d->invalid_stream++;
        break;
    default:
        break;
    }
}
