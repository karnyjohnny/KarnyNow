/* test_knbw.c — known-answer tests for the bandwidth estimator and the
 * server-control decoder (round XXXVIII, deviation D-t10).
 *
 * The expectations in kn_bw_kat_vectors.h are NOT this suite's opinion:
 * tools/bw_reference.py transcribes them from the reference client's own
 * Rust tests (nvst_bandwidth.rs:250-416 and nvst_server_control_tests.rs)
 * and re-proves every one of them against an independent Python model of
 * the same arithmetic BEFORE it writes the header. A failure here means
 * the C port drifted from the reference, not that the reference is wrong.
 *
 * The packet scripts below are the literal C transcription of the
 * generator's scripts — same frame counts, same timestamps, same arrival
 * times, same byte counts. Keeping the two in sync is what makes the
 * third implementation (Rust -> Python -> C) worth having.
 */
#include "test_port.h"
#include "kn/kn_bw.h"
#include "../src/kn_bw_kat_vectors.h"

#define KAT_SCTL_OUT 16u           /* more than any KAT buffer holds */
#define NS_MS ((int64_t)1000000)   /* the generator's clock unit */
#define NS_US ((int64_t)1000)
#define U32MAX 0xFFFFFFFFu

/* ------------------------------------------------------------------ *
 * part 1: scenario scripts — the LITERAL transcription of
 * tools/bw_reference.py:scenario_*. Same frame counts, same sequences,
 * same 90 kHz timestamps, same arrival times, same payload sizes, same
 * report instant. If one side changes, the KAT table disagrees and the
 * suite fails; that disagreement is the point.
 * ------------------------------------------------------------------ */

static void obs(kn_bw *b, uint32_t frame, uint32_t seq, uint32_t ts,
                uint32_t bytes, int started, int ended, int64_t at_ns)
{
    kn_bw_sample s;
    memset(&s, 0, sizeof s);
    s.frame_number = frame;
    s.sequence = seq;
    s.timestamp = ts;
    s.bytes = bytes;
    s.started = started;
    s.ended = ended;
    s.received_ns = at_ns;
    kn_bw_observe(b, &s);
}

/* scenario_train_101: 101 packets, 3 per frame at 20 ms/frame, 1 ms of
 * receive spread inside each frame, report one millisecond after the
 * last arrival. This is the shape the reference's own test uses. */
static kn_bw_feedback scen_report;

static void scen_train(kn_bw *b, unsigned frames, int64_t report_ms)
{
    unsigned frame;
    for (frame = 0; frame < frames; frame++) {
        int64_t at = (int64_t)frame * 20 * NS_MS;
        uint32_t ts = (uint32_t)(90000u + frame * 1800u);
        unsigned part;
        for (part = 0; part < 3u; part++)
            obs(b, frame, frame * 3u + part, ts, 200u, part == 0u,
                part == 2u, at + (int64_t)part * 500 * NS_US);
    }
    kn_bw_report(b, report_ms * NS_MS, &scen_report);
}

/* scenario_wrap: the RTP timestamp crosses zero between two frames. */
static void scen_wrap(kn_bw *b)
{
    obs(b, 1u, 10u, U32MAX - 899u, 300u, 1, 1, 0);
    obs(b, 2u, 11u, 901u, 300u, 1, 1, 20 * NS_MS);
    kn_bw_report(b, 20 * NS_MS, &scen_report);
}

/* scenario_parity: a FEC parity shard between the first and last packet
 * of frame 1 must keep the frame contiguous without being measured. */
static void scen_parity(kn_bw *b)
{
    obs(b, 1u, 10u, 90000u, 400u, 1, 0, 0);
    kn_bw_observe_parity(b, 1u, 11u);
    obs(b, 1u, 12u, 90000u, 400u, 0, 1, 1 * NS_MS);
    obs(b, 2u, 13u, 91800u, 400u, 1, 1, 20 * NS_MS);
    kn_bw_report(b, 20 * NS_MS, &scen_report);
}

/* scenario_reorder: frame 2 lands AFTER frame 3, so the train it opens
 * is dropped (distance == 0 on the way back) and both count as lossy. */
static void scen_reorder(kn_bw *b)
{
    obs(b, 1u, 100u, 90000u, 1200u, 1, 0, 0);
    obs(b, 1u, 102u, 90000u, 1200u, 0, 1, 1 * NS_MS);
    obs(b, 3u, 103u, 93600u, 1200u, 1, 1, 40 * NS_MS);
    obs(b, 2u, 101u, 91800u, 1200u, 1, 1, 41 * NS_MS);
    kn_bw_report(b, 50 * NS_MS, &scen_report);
}

/* scenario_backward_clock: the run-18 shape. The sender's 90 kHz clock
 * jumps BACKWARDS by one second at frame 60 while the wall clock keeps
 * running, so every packet after the jump has delta < 0 and must change
 * no state at all — the transit minimum may not be poisoned into a
 * negative-delay lie. */
static void scen_backward_clock(kn_bw *b)
{
    uint32_t ts = 90000u;
    int i;
    for (i = 0; i < 120; i++) {
        if (i == 60) ts -= 90000u;
        else ts += 1800u;
        obs(b, (uint32_t)i, (uint32_t)i, ts, 1000u, 1, 1,
            (int64_t)i * 20 * NS_MS);
    }
    kn_bw_report(b, 2400 * NS_MS, &scen_report);
}

/* scenario_lossy: frames 1, 2, 4 and 7 arrive complete; 3, 5 and 6 never
 * do. lossy_frames counts both the frame-number gaps (distance - 1) and
 * the trains abandoned incomplete. */
static void scen_lossy(kn_bw *b)
{
    static const unsigned frames[4] = { 1u, 2u, 4u, 7u };
    static const int64_t at[4] = { 0, 20 * NS_MS, 60 * NS_MS, 120 * NS_MS };
    unsigned i;
    for (i = 0; i < 4u; i++)
        obs(b, frames[i], frames[i] * 10u,
            (uint32_t)(90000u + (frames[i] - 1u) * 1800u), 800u, 1, 1, at[i]);
    kn_bw_report(b, 140 * NS_MS, &scen_report);
}

static int run_scenario(const char *name, kn_bw *st, kn_bw_feedback *out)
{
    kn_bw_init(st);
    memset(&scen_report, 0xAB, sizeof scen_report);
    if (strcmp(name, "train_101") == 0)
        scen_train(st, 101u, 2001);
    else if (strcmp(name, "gated_below_100") == 0)
        scen_train(st, 100u, 2000);
    else if (strcmp(name, "wrap") == 0)
        scen_wrap(st);
    else if (strcmp(name, "parity") == 0)
        scen_parity(st);
    else if (strcmp(name, "reorder") == 0)
        scen_reorder(st);
    else if (strcmp(name, "backward_clock") == 0)
        scen_backward_clock(st);
    else if (strcmp(name, "lossy") == 0)
        scen_lossy(st);
    else
        return 0;
    *out = scen_report;
    return 1;
}

/* A plain 30 fps train used by the non-KAT checks below: `frames`
 * packets, one per frame, 20 ms apart, 6000 bytes each. */
static void feed_plain_train(kn_bw *b, unsigned frames)
{
    unsigned i;
    for (i = 0; i < frames; i++)
        obs(b, i, i, (uint32_t)(90000u + i * 1800u), 6000u, 1, 1,
            (int64_t)i * 20 * NS_MS);
}

static void check_kat_field(const char *name, const char *field,
                            unsigned long got, unsigned long want)
{
    char label[160];
    snprintf(label, sizeof label, "kat[%s].%s == %lu (got %lu)",
             name, field, want, got);
    CHECK(got == want, label);
}

static void test_kat_scenarios(void)
{
    kn_bw st;
    size_t i;

    CHECK(kn_bw_kat_scenarios_n == 7u, "seven estimator scenarios generated");

    for (i = 0; i < kn_bw_kat_scenarios_n; i++) {
        const kn_bw_kat *k = &kn_bw_kat_scenarios[i];
        kn_bw_feedback fb;
        char label[160];
        memset(&fb, 0xAB, sizeof fb);
        snprintf(label, sizeof label, "scenario %s is known", k->name);
        CHECK(run_scenario(k->name, &st, &fb), label);
        check_kat_field(k->name, "minimum_server_time",
                        fb.minimum_server_time, k->minimum_server_time);
        check_kat_field(k->name, "median_server_time",
                        fb.median_server_time, k->median_server_time);
        check_kat_field(k->name, "queue_delay_us",
                        fb.queue_delay_us, k->queue_delay_us);
        check_kat_field(k->name, "lossy_frames",
                        fb.lossy_frames, k->lossy_frames);
        check_kat_field(k->name, "estimate_kbps",
                        fb.estimate_kbps, k->estimate_kbps);
        check_kat_field(k->name, "jitter_us",
                        fb.jitter_us, k->jitter_us);
        check_kat_field(k->name, "utilization_percent",
                        fb.utilization_percent, k->utilization_percent);
    }
}

/* The values the reference's Rust tests assert verbatim
 * (nvst_bandwidth.rs:277-288), spelled out here so a golden-table edit
 * can never quietly move them. */
static void test_rust_goldens(void)
{
    kn_bw st;
    kn_bw_feedback fb;

    CHECK(run_scenario("train_101", &st, &fb), "train_101 runs");
    CHECK(fb.minimum_server_time == KN_BW_KAT_RUST_MINIMUM_SERVER_TIME,
          "rust golden minimum_server_time == 270090");
    CHECK(fb.estimate_kbps == KN_BW_KAT_RUST_ESTIMATE_KBPS,
          "rust golden estimate_kbps == 4800");
    CHECK(fb.utilization_percent == KN_BW_KAT_RUST_UTILIZATION_PERCENT,
          "rust golden utilization_percent == 5");
    CHECK(fb.queue_delay_us == KN_BW_KAT_RUST_QUEUE_DELAY_US,
          "rust golden queue_delay_us == 1000");
    CHECK(fb.lossy_frames == 0u, "rust golden lossy_frames == 0");
    CHECK(kn_bw_frames_observed(&st) == 100u,
          "101 three-packet frames -> 100 completed frames");

    CHECK(run_scenario("reorder", &st, &fb), "reorder runs");
    CHECK(fb.lossy_frames == KN_BW_KAT_RUST_REORDER_LOSSY,
          "rust golden reorder lossy_frames == 2");

    CHECK(run_scenario("parity", &st, &fb), "parity runs");
    CHECK(fb.minimum_server_time == 91800u,
          "rust golden parity minimum_server_time == 91800");
    /* the reference's parity test asserts receive_kbps == 6400.0 for the
     * 800-byte frame received over 1 ms; that number is what the 100-frame
     * gate turns into estimate_kbps, so the train above pins it end to
     * end (4800 == the EMA over 100 identical 6400/4800-shaped frames). */
}

/* The reference zeroes everything before the first packet and keeps
 * utilization/estimate zero below 100 completed frames — the exact defect
 * run 18 showed (30 s of zeros in 0x0207). */
static void test_gating_and_reset(void)
{
    kn_bw st;
    kn_bw_feedback fb;

    kn_bw_init(&st);
    memset(&fb, 0xAB, sizeof fb);
    kn_bw_report(&st, 12345 * NS_MS, &fb);
    CHECK(fb.minimum_server_time == 0u, "empty: minimum_server_time 0");
    CHECK(fb.median_server_time == 0u, "empty: median_server_time 0");
    CHECK(fb.queue_delay_us == 0u, "empty: queue_delay_us 0");
    CHECK(fb.jitter_us == 0u, "empty: jitter_us 0");
    CHECK(fb.utilization_percent == 0u, "empty: utilization_percent 0");
    CHECK(fb.estimate_kbps == 0u, "empty: estimate_kbps 0");
    CHECK(fb.lossy_frames == 0u, "empty: lossy_frames 0");
    CHECK(kn_bw_frames_observed(&st) == 0u, "empty: frames_observed 0");

    /* one packet: a transit exists but no frame has completed */
    kn_bw_init(&st);
    obs(&st, 0u, 0u, 90000u, 1000u, 1, 0, 0);
    kn_bw_report(&st, 5 * NS_MS, &fb);
    CHECK(fb.estimate_kbps == 0u, "single packet: estimate gated to 0");
    CHECK(fb.utilization_percent == 0u, "single packet: utilization gated 0");
    CHECK(kn_bw_frames_observed(&st) == 0u, "single packet: 0 frames");

    /* 99 completed frames: the server-time fields are live, the two
     * gated ones are not */
    CHECK(run_scenario("gated_below_100", &st, &fb), "gated_below_100 runs");
    CHECK(kn_bw_frames_observed(&st) == 99u, "99 completed frames");
    CHECK(fb.minimum_server_time != 0u, "99 frames: minimum is live");
    CHECK(fb.median_server_time != 0u, "99 frames: median is live");
    CHECK(fb.estimate_kbps == 0u, "99 frames: estimate still gated");
    CHECK(fb.utilization_percent == 0u, "99 frames: utilization still gated");

    /* init() is a full reset (the reference's ssrc-change path) */
    feed_plain_train(&st, 101u);
    kn_bw_init(&st);
    kn_bw_report(&st, 999 * NS_MS, &fb);
    CHECK(fb.minimum_server_time == 0u, "re-init clears transit state");
    CHECK(kn_bw_frames_observed(&st) == 0u, "re-init clears frames_observed");
}

/* A report taken AT the first arrival must not underflow into a huge
 * server_time (the reference's `.max(0, ..)` clamp on now - transit). */
static void test_report_at_origin(void)
{
    kn_bw st;
    kn_bw_feedback fb;
    kn_bw_init(&st);
    feed_plain_train(&st, 5u);
    kn_bw_report(&st, 0, &fb);
    CHECK(fb.queue_delay_us <= 100000u, "queue_delay_us sane at t0");
    CHECK(fb.minimum_server_time <= 1000000u, "minimum_server_time sane at t0");
    CHECK(fb.median_server_time <= 1000000u, "median_server_time sane at t0");
}

/* The backward-clock scenario is the run-18 shape: whatever the arrival
 * and sender clocks do, every published field must stay inside its wire
 * type and the walk must stay finite. */
static void test_backward_clock_stays_in_range(void)
{
    kn_bw st;
    kn_bw_feedback fb;
    CHECK(run_scenario("backward_clock", &st, &fb), "backward_clock runs");
    CHECK(fb.jitter_us <= 65535u, "jitter_us clamped to u16");
    CHECK(fb.utilization_percent <= 100u, "utilization <= 100");
    CHECK((fb.minimum_server_time & 0x80000000u) == 0u,
          "minimum_server_time keeps the 31-bit mask");
    CHECK((fb.median_server_time & 0x80000000u) == 0u,
          "median_server_time keeps the 31-bit mask");
    /* the jump costs 51 frames: 59 trains completed before it, then the
     * packets whose timestamp went backwards are dropped until the
     * wrap-extended sender clock catches the pre-jump value again at
     * i == 110 (delta < 0 early return, nvst_bandwidth.rs:110-113), which
     * leaves 9 more completed frames. lossy_frames == 50 counts the
     * frame-number distance the estimator walked across the gap. */
    CHECK(kn_bw_frames_observed(&st) == 68u,
          "frames_observed after the backward jump == 68");
    CHECK(fb.lossy_frames == 50u, "lossy_frames across the jump == 50");
}

/* Parity notes must never create or extend a frame train on their own,
 * and a stale/non-advancing sequence must be a no-op. */
static void test_parity_guards(void)
{
    kn_bw st;
    kn_bw_feedback fb;

    kn_bw_init(&st);
    kn_bw_observe_parity(&st, 1u, 5u);          /* no active frame at all */
    kn_bw_report(&st, 10 * NS_MS, &fb);
    CHECK(fb.minimum_server_time == 0u, "parity alone publishes nothing");
    CHECK(kn_bw_frames_observed(&st) == 0u, "parity alone completes nothing");

    kn_bw_init(&st);
    obs(&st, 1u, 10u, 90000u, 400u, 1, 0, 0);
    kn_bw_observe_parity(&st, 2u, 11u);         /* a different frame */
    kn_bw_observe_parity(&st, 1u, 9u);          /* not ahead of the last */
    kn_bw_observe_parity(&st, 1u, 10u);         /* equal, not ahead */
    obs(&st, 1u, 12u, 90000u, 400u, 0, 1, 1 * NS_MS);
    obs(&st, 2u, 13u, 91800u, 400u, 1, 1, 20 * NS_MS);
    kn_bw_report(&st, 20 * NS_MS, &fb);
    /* the note for a DIFFERENT frame number is not a no-op: the reference
     * takes() the active frame, sees distance != 0, counts it as lossy and
     * abandons the train. Only the two non-advancing sequences do nothing. */
    CHECK(fb.lossy_frames == 1u, "a stale-frame parity note counts one loss");
    CHECK(kn_bw_frames_observed(&st) == 0u,
          "the abandoned train completes no frame");
    CHECK(fb.minimum_server_time == 91800u,
          "guarded parity notes leave the transit minimum alone");
    CHECK(fb.jitter_us == 121u, "parity scenario jitter still 121 us");
}

/* ------------------------------------------------------------------ *
 * part 2: the server-control decoder
 * ------------------------------------------------------------------ */

static void check_kat_sctl_field(const char *name, const char *field,
                                 long got, long want)
{
    char label[160];
    snprintf(label, sizeof label, "sctl[%s].%s == %ld (got %ld)",
             name, field, want, got);
    CHECK(got == want, label);
}

static void test_kat_server_control(void)
{
    size_t v;

    CHECK(kn_sctl_kat_vectors_n == 14u, "fourteen server-control buffers");

    for (v = 0; v < kn_sctl_kat_vectors_n; v++) {
        const kn_sctl_kat *k = &kn_sctl_kat_vectors[v];
        kn_sctl_msg msgs[KAT_SCTL_OUT];
        kn_sctl_diag d;
        size_t n, i;
        char label[160];

        n = kn_sctl_walk(k->bytes, k->len, msgs, KAT_SCTL_OUT);
        snprintf(label, sizeof label, "sctl[%s]: %u records", k->name,
                 k->n_msgs);
        CHECK(n == (size_t)k->n_msgs, label);
        if (n != (size_t)k->n_msgs) continue;

        kn_sctl_diag_init(&d);
        for (i = 0; i < n; i++) {
            if (k->codes[i] != 0xFFFFu) {      /* 0xFFFF = don't care */
                snprintf(label, sizeof label, "sctl[%s] record %u kind == %u",
                         k->name, (unsigned)i, (unsigned)k->codes[i]);
                CHECK((unsigned)msgs[i].kind == (unsigned)k->codes[i], label);
            }
            kn_sctl_diag_add(&d, &msgs[i]);
        }

        /* the first record's decoded payload */
        if (n > 0u) {
            check_kat_sctl_field(k->name, "timer_active",
                                 msgs[0].timer_active, (long)k->timer_active);
            check_kat_sctl_field(k->name, "seconds_left",
                                 (long)msgs[0].seconds_left,
                                 (long)k->seconds_left);
            check_kat_sctl_field(k->name, "wire_code",
                                 (long)msgs[0].wire_code, (long)k->wire_code);
            check_kat_sctl_field(k->name, "resumable",
                                 msgs[0].have_resumable ? msgs[0].resumable : -1,
                                 (long)k->resumable);
            check_kat_sctl_field(k->name, "stream_index",
                                 (long)msgs[0].stream_index,
                                 (long)k->stream_index);
            /* the pacing fields belong to the LAST record when the buffer
             * batches several (the reference latches them in walk order) */
            {
                size_t last = n - 1u;
                if (msgs[last].kind != KN_SCTL_PACING_MODE) last = 0u;
                check_kat_sctl_field(k->name, "pacing_interval",
                                     (long)msgs[last].pacing_interval,
                                     (long)k->pacing_interval);
            }
        }

        /* the diagnostics the reference's 10-second line prints */
        check_kat_sctl_field(k->name, "unknown", (long)d.unknown,
                             (long)k->unknown);
        check_kat_sctl_field(k->name, "malformed", (long)d.malformed,
                             (long)k->malformed);
        check_kat_sctl_field(k->name, "invalid_stream", (long)d.invalid_stream,
                             (long)k->invalid_stream);
    }
}

/* The reference's own decoder tests, asserted directly
 * (nvst_server_control.rs:204-330 and nvst_server_control_tests.rs:109). */
static void test_server_control_reference_cases(void)
{
    kn_sctl_msg m[4];
    size_t n;

    /* 0x104 = idle warning ACTIVE, 0x105 = expired; seconds are LE */
    {
        static const uint8_t t104[] = { 0x04, 0x01, 0x04, 0x00, 60, 0, 0, 0 };
        static const uint8_t t105[] = { 0x05, 0x01, 0x04, 0x00, 60, 0, 0, 0 };
        n = kn_sctl_walk(t104, sizeof t104, m, 4u);
        CHECK(n == 1u, "0x104: one record");
        CHECK(m[0].kind == KN_SCTL_TIMER, "0x104: TIMER");
        CHECK(m[0].timer_type == 4u, "0x104: timer_type 4");
        CHECK(m[0].seconds_left == 60u, "0x104: seconds_left 60");
        CHECK(m[0].timer_active == 1, "0x104: active");
        n = kn_sctl_walk(t105, sizeof t105, m, 4u);
        CHECK(n == 1u && m[0].timer_type == 5u, "0x105: timer_type 5");
        CHECK(m[0].timer_active == 0, "0x105: not active");
    }

    /* a 0x104/0x105 payload shorter than 4 bytes is Truncated, and the
     * record is still consumed so the walk CONTINUES */
    {
        unsigned len;
        for (len = 0u; len < 4u; len++) {
            uint8_t buf[8];
            char label[64];
            buf[0] = 0x04; buf[1] = 0x01;
            buf[2] = (uint8_t)len; buf[3] = 0x00;
            buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0;
            n = kn_sctl_walk(buf, 4u + len, m, 4u);
            snprintf(label, sizeof label, "0x104 payload %u: one record", len);
            CHECK(n == 1u, label);
            snprintf(label, sizeof label, "0x104 payload %u: Truncated", len);
            CHECK(m[0].kind == KN_SCTL_MALFORMED &&
                  m[0].error == KN_SCTL_ERR_TRUNCATED, label);
            snprintf(label, sizeof label, "0x104 payload %u keeps its code", len);
            CHECK(m[0].code == 0x0104u, label);
        }
    }

    /* 0x109: BE wire code + opaque tail, from_wire mapping, resumability */
    {
        static const uint8_t term[] = {
            0x09, 0x01, 0x07, 0x00, 0x80, 0x03, 0x00, 0x23, 1, 2, 3
        };
        n = kn_sctl_walk(term, sizeof term, m, 4u);
        CHECK(n == 1u, "0x109 with tail: one record");
        CHECK(m[0].kind == KN_SCTL_TERMINATION, "0x109: TERMINATION");
        CHECK(m[0].wire_code == 0x80030023u, "0x109: wire_code BE");
        CHECK(m[0].have_reason && m[0].reason == 0x38u, "0x80030023 -> 0x38");
        CHECK(m[0].have_extended && m[0].extended_code == 0x80030007u,
              "0x80030023 -> extended 0x80030007");
        CHECK(m[0].have_resumable && m[0].resumable == 0,
              "0x38 is not resumable");
    }
    {
        static const uint8_t resume[] = {
            0x09, 0x01, 0x04, 0x00, 0x80, 0x04, 0x00, 0x10
        };
        n = kn_sctl_walk(resume, sizeof resume, m, 4u);
        CHECK(n == 1u && m[0].wire_code == 0x80040010u, "0x80040010 wire");
        CHECK(m[0].reason == 0x37u, "0x80040010 -> 0x37");
        CHECK(m[0].resumable == 1, "0x37/0x80040010 is resumable");
    }
    {
        static const uint8_t short_term[] = { 0x09, 0x01, 0x03, 0x00, 0, 0, 0 };
        n = kn_sctl_walk(short_term, sizeof short_term, m, 4u);
        CHECK(n == 1u && m[0].kind == KN_SCTL_MALFORMED &&
              m[0].error == KN_SCTL_ERR_TRUNCATED, "0x109 len 3: Truncated");
    }
    {
        static const uint8_t raw[] = {
            0x09, 0x01, 0x04, 0x00, 0xde, 0xad, 0xbe, 0xef
        };
        n = kn_sctl_walk(raw, sizeof raw, m, 4u);
        CHECK(n == 1u && m[0].wire_code == 0xDEADBEEFu,
              "unmapped wire code passes through");
        CHECK(m[0].have_reason == 0, "unmapped wire code has no reason");
        CHECK(m[0].have_resumable == 0, "unmapped wire code: undecidable");
    }

    /* session_resumable(reason, extended) — the full truth table */
    CHECK(kn_sctl_resumable(0x37u, 0x80040010u) == 1, "0x37/0x80040010 resumable");
    CHECK(kn_sctl_resumable(0x37u, 0x80040003u) == 0, "0x37/0x80040003 not");
    CHECK(kn_sctl_resumable(0x37u, 0x8004000bu) == 0, "0x37/0x8004000b not");
    CHECK(kn_sctl_resumable(0x37u, 0x8004000fu) == 0, "0x37/0x8004000f not");
    CHECK(kn_sctl_resumable(0x37u, 0x80040010u - 1u) == 0, "0x37/0x8004000f- not");
    CHECK(kn_sctl_resumable(0x37u, 0x80040011u) == 0, "0x37/0x80040011 not");
    CHECK(kn_sctl_resumable(0x37u, 0x80040016u) == 0, "0x37/0x80040016 not");
    CHECK(kn_sctl_resumable(0x37u, 0x80040017u) == 1, "0x37/0x80040017 resumable");
    CHECK(kn_sctl_resumable(0x37u, 0x8004000au) == 1, "0x37/0x8004000a resumable");
    CHECK(kn_sctl_resumable(0x3bu, 0x80040004u) == 1, "0x3b/0x80040004 resumable");
    CHECK(kn_sctl_resumable(0x3bu, 0x80040007u) == 1, "0x3b/0x80040007 resumable");
    CHECK(kn_sctl_resumable(0x3bu, 0x80040010u) == 1, "0x3b/0x80040010 resumable");
    CHECK(kn_sctl_resumable(0x3bu, 0x80040005u) == 0, "0x3b/0x80040005 not");
    CHECK(kn_sctl_resumable(0x38u, 0x80030007u) == 0, "other reason: not");
    CHECK(kn_sctl_resumable(0u, 0u) == 0, "reason 0: not resumable");

    /* 0x112: exact 8-byte payload, mode normalised, stream != 0 counted */
    {
        static const uint32_t modes[4] = { 0u, 1u, 2u, 0xFFFFFFFFu };
        static const int want[4] = { 0, 1, 0, 0 };
        unsigned i;
        for (i = 0; i < 4u; i++) {
            uint8_t buf[12];
            char label[64];
            buf[0] = 0x12; buf[1] = 0x01; buf[2] = 0x08; buf[3] = 0x00;
            buf[4] = 0x02; buf[5] = 0x00; buf[6] = 0x00; buf[7] = 0x00;
            buf[8] = (uint8_t)(modes[i] & 0xFFu);
            buf[9] = (uint8_t)((modes[i] >> 8) & 0xFFu);
            buf[10] = (uint8_t)((modes[i] >> 16) & 0xFFu);
            buf[11] = (uint8_t)((modes[i] >> 24) & 0xFFu);
            n = kn_sctl_walk(buf, sizeof buf, m, 4u);
            snprintf(label, sizeof label, "0x112 mode %u: one record", modes[i]);
            CHECK(n == 1u, label);
            snprintf(label, sizeof label, "0x112 mode %u -> interval %d",
                     modes[i], want[i]);
            CHECK(m[0].kind == KN_SCTL_PACING_MODE &&
                  m[0].pacing_interval == want[i], label);
            snprintf(label, sizeof label, "0x112 mode %u keeps stream 2",
                     modes[i]);
            CHECK(m[0].stream_index == 2u, label);
        }
    }
    {
        static const unsigned bad[5] = { 0u, 4u, 7u, 9u, 12u };
        unsigned i;
        for (i = 0; i < 5u; i++) {
            uint8_t buf[4 + 12];
            char label[64];
            buf[0] = 0x12; buf[1] = 0x01;
            buf[2] = (uint8_t)bad[i]; buf[3] = 0x00;
            memset(buf + 4, 0, 12u);
            n = kn_sctl_walk(buf, 4u + bad[i], m, 4u);
            snprintf(label, sizeof label, "0x112 len %u: InvalidLength", bad[i]);
            CHECK(n == 1u && m[0].kind == KN_SCTL_MALFORMED &&
                  m[0].error == KN_SCTL_ERR_INVALID_LENGTH, label);
        }
    }

    /* the reference's envelope test: a payload's bytes are never scanned
     * for record headers ([0x999 pl=4 "09 01 00 00"][0x105 pl=4 zeros]) */
    {
        static const uint8_t env[] = {
            0x99, 0x01, 4, 0, 0x09, 0x01, 0, 0,
            0x05, 0x01, 4, 0, 0, 0, 0, 0
        };
        n = kn_sctl_walk(env, sizeof env, m, 4u);
        CHECK(n == 2u, "envelope test: exactly two records");
        CHECK(m[0].kind == KN_SCTL_UNKNOWN && m[0].code == 0x0199u,
              "envelope test: first is Unknown(0x199)");
        CHECK(m[1].kind == KN_SCTL_TIMER && m[1].timer_type == 5u &&
              m[1].seconds_left == 0u && m[1].timer_active == 0,
              "envelope test: second is Timer{5, 0, inactive}");
    }

    /* the reference's counting test: unknown, one malformed, then a
     * pacing record — the walk is NOT terminal on a decode error */
    {
        static const uint8_t counted[] = {
            0x99, 0x09, 0x03, 0x00, 0x01, 0x02, 0x03,
            0x04, 0x01, 0x01, 0x00, 0x00,
            0x12, 0x01, 0x08, 0x00, 0, 0, 0, 0, 0, 0, 0, 0
        };
        kn_sctl_diag d;
        size_t i;
        n = kn_sctl_walk(counted, sizeof counted, m, 4u);
        CHECK(n == 3u, "counted test: three records");
        CHECK(m[0].kind == KN_SCTL_UNKNOWN && m[0].code == 0x0999u,
              "counted test: Unknown(0x999)");
        CHECK(m[1].kind == KN_SCTL_MALFORMED &&
              m[1].error == KN_SCTL_ERR_TRUNCATED,
              "counted test: 0x104 payload 1 -> Truncated");
        CHECK(m[2].kind == KN_SCTL_PACING_MODE && m[2].pacing_interval == 0 &&
              m[2].stream_index == 0u,
              "counted test: pacing mode 0 on stream 0 (disables pacing)");
        kn_sctl_diag_init(&d);
        for (i = 0; i < n; i++) kn_sctl_diag_add(&d, &m[i]);
        CHECK(d.unknown == 1u, "counted test: unknown == 1");
        CHECK(d.have_last_unknown && d.last_unknown == 0x0999u,
              "counted test: last_unknown == 0x999");
        CHECK(d.malformed == 1u, "counted test: malformed == 1");
        CHECK(d.invalid_stream == 0u, "counted test: invalid_stream == 0");
    }
}

/* The 0x1xx codes the reference lists as Unhandled must be counted as
 * unhandled, not unknown — run 18's rig sent exactly these. */
static void test_unhandled_table(void)
{
    static const uint16_t codes[] = {
        0x101, 0x102, 0x103, 0x106, 0x10a, 0x10b, 0x10c, 0x10d,
        0x10e, 0x10f, 0x110, 0x111, 0x113, 0x206, 0x325, 0x408
    };
    /* and the neighbours that must NOT be unhandled */
    static const uint16_t not_codes[] = { 0x100, 0x104, 0x105, 0x107,
                                          0x108, 0x109, 0x112, 0x114 };
    size_t i;

    for (i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        uint8_t buf[8];
        kn_sctl_msg m[2];
        char label[64];
        size_t n;
        buf[0] = (uint8_t)(codes[i] & 0xFFu);
        buf[1] = (uint8_t)(codes[i] >> 8);
        buf[2] = 0x04; buf[3] = 0x00;
        buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0;
        n = kn_sctl_walk(buf, sizeof buf, m, 2u);
        snprintf(label, sizeof label, "0x%04x is UNHANDLED", codes[i]);
        CHECK(n == 1u && m[0].kind == KN_SCTL_UNHANDLED, label);
    }
    for (i = 0; i < sizeof not_codes / sizeof not_codes[0]; i++) {
        uint8_t buf[8];
        kn_sctl_msg m[2];
        char label[64];
        size_t n;
        buf[0] = (uint8_t)(not_codes[i] & 0xFFu);
        buf[1] = (uint8_t)(not_codes[i] >> 8);
        buf[2] = 0x04; buf[3] = 0x00;
        buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0;
        n = kn_sctl_walk(buf, sizeof buf, m, 2u);
        snprintf(label, sizeof label, "0x%04x is NOT unhandled", not_codes[i]);
        CHECK(n == 1u && m[0].kind != KN_SCTL_UNHANDLED, label);
    }
}

/* Bounds: the three structural error paths, none of which may read
 * outside the buffer, plus the caller-array bound. */
static void test_bounds_errors(void)
{
    kn_sctl_msg m[4];
    size_t i, n;

    /* an empty buffer yields nothing; 1-3 bytes are a Truncated record
     * (the reference checks `bytes.is_empty()` BEFORE the length check) */
    {
        static const uint8_t pad[4] = { 0x04, 0x01, 0x04, 0x00 };
        char label[64];
        n = kn_sctl_walk(pad, 0u, m, 4u);
        CHECK(n == 0u, "empty buffer yields nothing");
        for (i = 1u; i < 4u; i++) {
            n = kn_sctl_walk(pad, i, m, 4u);
            snprintf(label, sizeof label, "%u-byte buffer: one Truncated",
                     (unsigned)i);
            CHECK(n == 1u && m[0].kind == KN_SCTL_MALFORMED &&
                  m[0].error == KN_SCTL_ERR_TRUNCATED, label);
        }
    }

    /* the reference's truncation test: [4,1] and [4,1,4,0,60] */
    {
        static const uint8_t a[] = { 4, 1 };
        static const uint8_t b[] = { 4, 1, 4, 0, 60 };
        n = kn_sctl_walk(a, sizeof a, m, 4u);
        CHECK(n == 1u && m[0].kind == KN_SCTL_MALFORMED &&
              m[0].error == KN_SCTL_ERR_TRUNCATED, "2-byte buffer: Truncated");
        n = kn_sctl_walk(b, sizeof b, m, 4u);
        CHECK(n == 1u && m[0].kind == KN_SCTL_MALFORMED &&
              m[0].error == KN_SCTL_ERR_TRUNCATED, "short payload: Truncated");
    }

    /* a length that runs past the end of the buffer */
    {
        static const uint8_t trunc[] = { 0x04, 0x01, 0xFF, 0x00, 0x01, 0x02 };
        n = kn_sctl_walk(trunc, sizeof trunc, m, 4u);
        CHECK(n == 1u && m[0].error == KN_SCTL_ERR_TRUNCATED,
              "overrun length: Truncated");
    }

    /* the largest legal record: 4 + 65535 bytes == KN_SCTL_MAX_BYTES */
    {
        static uint8_t max_rec[KN_SCTL_MAX_BYTES];
        max_rec[0] = 0x99; max_rec[1] = 0x01;
        max_rec[2] = 0xFF; max_rec[3] = 0xFF;
        memset(max_rec + 4, 0, sizeof max_rec - 4u);
        n = kn_sctl_walk(max_rec, sizeof max_rec, m, 4u);
        CHECK(n == 1u && m[0].kind == KN_SCTL_UNKNOWN &&
              m[0].code == 0x0199u, "MAX_BYTES record is legal");
        n = kn_sctl_walk(max_rec, sizeof max_rec + 0u, m, 0u);
        CHECK(n == 0u, "max_out 0 yields nothing");
        n = kn_sctl_walk(max_rec, sizeof max_rec, NULL, 4u);
        CHECK(n == 0u, "NULL out yields nothing");
    }

    /* one byte over the cap: Oversized, decided before any walking */
    {
        static uint8_t huge[KN_SCTL_MAX_BYTES + 1u];
        memset(huge, 0, sizeof huge);
        n = kn_sctl_walk(huge, sizeof huge, m, 4u);
        CHECK(n == 1u && m[0].kind == KN_SCTL_MALFORMED &&
              m[0].error == KN_SCTL_ERR_OVERSIZED, "oversized: one error");
    }

    /* 256 records fit; the 257th is TooManyCommands (the reference's
     * bounded-work test asserts exactly MAX + 1 items) */
    {
        static uint8_t big[(KN_SCTL_MAX_COMMANDS + 1u) * 4u];
        static kn_sctl_msg many[KN_SCTL_MAX_COMMANDS + 1u];
        size_t k;
        for (i = 0; i < KN_SCTL_MAX_COMMANDS + 1u; i++) {
            big[i * 4u + 0] = 0x01; big[i * 4u + 1] = 0x01;  /* 0x0101 */
            big[i * 4u + 2] = 0x00; big[i * 4u + 3] = 0x00;
        }
        n = kn_sctl_walk(big, sizeof big, many, KN_SCTL_MAX_COMMANDS + 1u);
        CHECK(n == KN_SCTL_MAX_COMMANDS + 1u, "257 records -> 257 items");
        for (k = 0; k < KN_SCTL_MAX_COMMANDS; k++) {
            if (many[k].kind != KN_SCTL_UNHANDLED) break;
        }
        CHECK(k == KN_SCTL_MAX_COMMANDS, "the first 256 are all decoded");
        CHECK(many[KN_SCTL_MAX_COMMANDS].kind == KN_SCTL_MALFORMED &&
              many[KN_SCTL_MAX_COMMANDS].error == KN_SCTL_ERR_TOO_MANY,
              "the 257th is TooManyCommands");
    }

    /* the caller's array is the bound: the record is left unconsumed, so
     * a bigger array sees strictly more (no silent truncation) */
    {
        static const uint8_t two[] = {
            0x12, 0x01, 0x08, 0x00, 0, 0, 0, 0, 1, 0, 0, 0,
            0x12, 0x01, 0x08, 0x00, 0, 0, 0, 0, 1, 0, 0, 0
        };
        kn_sctl_msg one[1];
        n = kn_sctl_walk(two, sizeof two, one, 1u);
        CHECK(n == 1u, "max_out 1 yields 1 of 2 records");
        n = kn_sctl_walk(two, sizeof two, m, 4u);
        CHECK(n == 2u, "max_out 4 yields both records");
        CHECK(m[0].pacing_interval == 1 && m[1].pacing_interval == 1,
              "both records decode identically");
    }

    /* a NULL buffer with a non-zero length must not be dereferenced */
    n = kn_sctl_walk(NULL, 8u, m, 4u);
    CHECK(n == 0u, "NULL buffer yields nothing");
}

/* Diagnostics saturate instead of wrapping (the reference pins this with
 * diagnostics.unknown = u64::MAX and asserts it stays there). */
static void test_diagnostics_saturation(void)
{
    kn_sctl_diag d;
    kn_sctl_msg m;
    const unsigned long long max64 = (unsigned long long)-1;

    kn_sctl_diag_init(&d);
    CHECK(d.unknown == 0u && d.malformed == 0u && d.invalid_stream == 0u,
          "diag init zeroes the counters");
    CHECK(d.have_last_unknown == 0, "diag init has no last_unknown");

    memset(&m, 0, sizeof m);
    m.kind = KN_SCTL_UNKNOWN; m.code = 0x0999u;
    d.unknown = max64;
    kn_sctl_diag_add(&d, &m);
    CHECK(d.unknown == max64, "unknown saturates at u64 max");
    CHECK(d.have_last_unknown && d.last_unknown == 0x0999u,
          "last_unknown still updates at saturation");

    m.kind = KN_SCTL_MALFORMED;
    d.malformed = max64;
    kn_sctl_diag_add(&d, &m);
    CHECK(d.malformed == max64, "malformed saturates at u64 max");

    m.kind = KN_SCTL_PACING_MODE; m.stream_index = 3u;
    d.invalid_stream = max64;
    kn_sctl_diag_add(&d, &m);
    CHECK(d.invalid_stream == max64, "invalid_stream saturates at u64 max");

    /* stream 0 never counts as invalid */
    kn_sctl_diag_init(&d);
    m.stream_index = 0u;
    kn_sctl_diag_add(&d, &m);
    CHECK(d.invalid_stream == 0u, "stream 0 pacing is not invalid_stream");

    /* TIMER / TERMINATION / UNHANDLED touch no counter */
    m.kind = KN_SCTL_TIMER;    kn_sctl_diag_add(&d, &m);
    m.kind = KN_SCTL_TERMINATION; kn_sctl_diag_add(&d, &m);
    m.kind = KN_SCTL_UNHANDLED;   kn_sctl_diag_add(&d, &m);
    CHECK(d.unknown == 0u && d.malformed == 0u && d.invalid_stream == 0u,
          "timer/termination/unhandled are not counted as diagnostics");

    /* the frame-pacing latch default is ENABLED (nvst.rs:664) */
    CHECK(KN_SCTL_PACING_DEFAULT_ENABLED == 1, "pacing default is enabled");
}

int main(void)
{
    printf("== test_knbw: bandwidth estimator + server-control decoder ==\n");
    test_kat_scenarios();
    test_rust_goldens();
    test_gating_and_reset();
    test_report_at_origin();
    test_backward_clock_stays_in_range();
    test_parity_guards();
    test_kat_server_control();
    test_server_control_reference_cases();
    test_unhandled_table();
    test_bounds_errors();
    test_diagnostics_saturation();
    return kn_test_summary("knbw");
}
