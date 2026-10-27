/* kn_bw.h — the NVST bandwidth estimator + the server-control decoder
 * (round XXXVIII; provenance in docs/BW.md and docs/RTSP.md §25).
 *
 * WHY THIS FILE EXISTS — R7 run 18 (2026-10-27). The input activation
 * chain moved the stall from 10.99 s to 30.5 s (331 -> 562 AUs), so the
 * chain matters — but the stream still died, and the FULL AU-length log
 * shows exactly how: a smooth exponential bitrate ramp-DOWN at a constant
 * 30 fps (15.8 kB/frame at fi 100 -> 14.1 kB at fi 280 -> 9.2 kB at 350
 * -> 6.2 kB at 400 -> 4.6 kB at 450 -> 3.5 kB at 500 -> 3.05 kB at 562),
 * the periodic IDR spikes shrinking with it (13.3 kB -> 5.3 kB), then
 * silence, one unanswered IDR+PLI, and ~5 s later a GRACEFUL SCTP
 * shutdown (sctp_down with sctp_fail:0 = SHUTDOWN_COMP, not COMM_LOST).
 * Run 17 has the same shape compressed into 11 s (18 356 -> 2 862 B).
 * A rate controller walking the bitrate to its floor and stopping is what
 * that looks like — not "a static desktop compresses better" (R35's
 * not-over-read caveat is hereby retired by the full log).
 *
 * The one QoS field set we have never filled is the estimator block of
 * the 0x0207 report (deviation D-t10): minimum_server_time,
 * median_server_time, queue_delay_us, jitter_us, utilization_percent and
 * lossy_frames all went out as ZERO for 4701/4932 reports, while
 * estimate_kbps carried a raw measured-downlink number instead of the
 * reference's smoothed, gated estimate. minimum/median_server_time are
 * the server's OWN 90 kHz clock reflected back through our measured
 * minimum/median packet transit — with zeros there, the server's
 * one-way-delay view is (now_ms - 0), i.e. a delay that grows linearly
 * with the session and never stops growing. That is a congestion signal
 * that never relaxes, which is precisely a monotonic ramp-down.
 *
 * WHAT IT IS: a byte-faithful port of nvst_bandwidth.rs (270 lines at
 * a49b124) plus nvst_server_control.rs (the inbound 0x104/0x105/0x109/
 * 0x112 decoder the reference uses to see idle warnings, terminations
 * and frame-pacing mode changes). Pure computation: no sockets, no
 * threads, no allocation, no clock of its own (the caller passes
 * milliseconds), so the sandbox and the Win7 target run the SAME code
 * and the SAME vectors.
 *
 * PINNED BY THREE IMPLEMENTATIONS (the kn_input discipline): the Rust
 * reference's own unit tests (nvst_bandwidth.rs:250-416 — five tests
 * whose exact expected values this port must reproduce, including
 * minimum_server_time == 270090, estimate_kbps == 4800,
 * utilization_percent == 5, queue_delay_us == 1000 and the 6400.0 kbps
 * parity case) are replayed by tools/bw_reference.py BEFORE it emits
 * kn_bw_kat_vectors.h, and test_knbw replays them again against the C.
 *
 * SECURITY/§7: no key material, no user data. Everything here is
 * counters and timings; the evidence lines print numbers only.
 */
#ifndef KN_BW_H
#define KN_BW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================ part 1:
 * BandwidthEstimator (nvst_bandwidth.rs)
 * ============================================================ */

#define KN_BW_TRANSIT_SAMPLES 16u   /* TRANSIT_SAMPLES, :3 */

/* One observed packet. `received_ns` is a MONOTONIC clock in INTEGER
 * NANOSECONDS owned by the caller (the reference passes an `Instant`).
 *
 * Integer nanoseconds are NOT a stylistic choice here. The reference does
 * every clock subtraction on `Duration` and converts with
 * `as_secs_f64() * 1000.0` only at the point of use, so its
 * `received_ms` for a 1 ms packet train is EXACTLY 1.0. The same
 * arithmetic on a double millisecond clock yields 0.999999999999455, and
 * `utilization_percent` — which the reference's own test pins at 5 —
 * truncates to 4. tools/bw_reference.py caught this before it shipped.
 *
 * `bytes` is the RTP PAYLOAD length (nvst.rs:4697 payload.len()), not the
 * datagram length. */
typedef struct {
    uint32_t frame_number;
    uint32_t sequence;         /* u32 of the extended SRTP index, :4681 */
    uint32_t timestamp;        /* the RTP 90 kHz timestamp */
    uint32_t bytes;
    int      started;          /* is_start_of_frame() */
    int      ended;            /* is_end_of_frame() */
    int64_t  received_ns;
} kn_bw_sample;

/* The seven fields the 0x0207 report carries (nvst_bandwidth.rs:17-25),
 * already clamped/masked to their wire types by kn_bw_report(). */
typedef struct {
    uint32_t minimum_server_time;
    uint32_t median_server_time;
    uint32_t queue_delay_us;
    uint32_t lossy_frames;
    uint32_t estimate_kbps;
    uint16_t jitter_us;
    uint8_t  utilization_percent;
} kn_bw_feedback;

typedef struct {
    /* ---- arrival/transit state (:43-58) ---- */
    int     have_first_arrival;
    int64_t first_arrival_ns;
    int    have_last_timestamp;
    uint32_t last_timestamp;
    uint64_t extended_timestamp;   /* the wrap-extended 90 kHz clock */
    int    have_minimum_transit;
    double minimum_transit_ms;
    int    have_previous_transit;
    double previous_transit_ms;
    double jitter_ms;
    double latest_transit_ms;
    double transits[KN_BW_TRANSIT_SAMPLES];
    unsigned transit_count;
    unsigned transit_next;
    /* ---- the frame train in progress (:28-39) ---- */
    int      have_active_frame;
    uint32_t af_number;
    int64_t  af_first_ns, af_last_ns;
    uint64_t af_first_ts, af_last_ts;
    uint32_t af_last_sequence;
    uint64_t af_bytes;
    double   af_sender_gap_ms;
    int      af_started, af_ended, af_contiguous;
    /* ---- smoothed outputs (:52-57) ---- */
    uint32_t frames_observed;
    uint32_t lossy_frames;
    int      have_average_frame_ms;
    double   average_frame_ms;
    int      have_average_receive_kbps;
    double   average_receive_kbps;
    int      have_average_utilization;
    double   average_utilization_percent;
} kn_bw;

void kn_bw_init(kn_bw *b);

/* A FEC PARITY shard of the current frame (nvst_bandwidth.rs:80-92):
 * parity packets are not measured, they only keep the frame's sequence
 * contiguity honest. `sequence` must be > the frame's last sequence for
 * the note to apply (the reference's guard). */
void kn_bw_observe_parity(kn_bw *b, uint32_t frame_number, uint32_t sequence);

/* A DATA packet (:94-208). Feed in arrival order; a packet whose RTP
 * timestamp goes BACKWARDS is ignored (the reference's `delta < 0`
 * early-return — this is why a stalled or reset sender clock does not
 * poison the transit minimum). */
void kn_bw_observe(kn_bw *b, const kn_bw_sample *s);

/* The report (:210-243). `at_ns` is the caller's monotonic clock at
 * report time, in the same integer-nanosecond domain as
 * kn_bw_sample.received_ns (the reference passes Instant::now()). Before the first
 * packet, and before 100 completed frames for the two gated fields, the
 * corresponding outputs are ZERO — that gating is the reference's, not
 * an omission: utilization_percent and estimate_kbps stay 0 until
 * frames_observed >= 100. */
void kn_bw_report(const kn_bw *b, int64_t at_ns, kn_bw_feedback *out);

/* frames_observed (evidence: lets a log say whether the estimator has
 * converged yet, i.e. whether estimate/utilization are live or gated). */
uint32_t kn_bw_frames_observed(const kn_bw *b);

/* ============================================================ part 2:
 * ServerControl (nvst_server_control.rs) — the inbound 0x1xx plane
 * ============================================================
 * Run 18's rig sent 23 messages and we named their codes but decoded
 * none of them. Four codes are NOT ignorable:
 *   0x104 / 0x105  NvstSessionTimer  (idle warning active / expired)
 *   0x109          NvstServerTermination (wire code + resumability)
 *   0x112          FramePacingMode (the server switches our 0x0203 on/off)
 * Everything else in 0x101..=0x103, 0x106, 0x10a..=0x111, 0x113, 0x206,
 * 0x325, 0x408 is explicitly Unhandled by the reference, and any other
 * code is Unknown (counted, never terminal). */

#define KN_SCTL_MAX_BYTES    (4u + 65535u)  /* MAX_SERVER_CONTROL_BYTES */
#define KN_SCTL_MAX_COMMANDS 256u           /* MAX_SERVER_CONTROL_COMMANDS */

typedef enum {
    KN_SCTL_NONE = 0,        /* end of the buffer (walk finished) */
    KN_SCTL_TIMER,           /* 0x104 / 0x105 */
    KN_SCTL_TERMINATION,     /* 0x109 */
    KN_SCTL_PACING_MODE,     /* 0x112 */
    KN_SCTL_UNHANDLED,       /* a known-but-ignored code */
    KN_SCTL_UNKNOWN,         /* an unrecognised code (counted) */
    KN_SCTL_MALFORMED        /* Oversized / TooManyCommands / Truncated /
                              * InvalidLength — the walk STOPS here, exactly
                              * like the reference's iterator (it empties the
                              * buffer after yielding the error) */
} kn_sctl_kind;

typedef enum {
    KN_SCTL_ERR_NONE = 0,
    KN_SCTL_ERR_OVERSIZED,
    KN_SCTL_ERR_TOO_MANY,
    KN_SCTL_ERR_TRUNCATED,
    KN_SCTL_ERR_INVALID_LENGTH
} kn_sctl_err;

typedef struct {
    /* outputs of one step */
    kn_sctl_kind kind;
    kn_sctl_err  error;
    uint16_t     code;            /* the record's code (0 after MALFORMED
                                   * only when the header itself was short) */
    /* KN_SCTL_TIMER */
    uint8_t      timer_type;      /* 4 or 5 */
    uint32_t     seconds_left;
    int          timer_active;    /* code == 0x104 */
    /* KN_SCTL_TERMINATION */
    uint32_t     wire_code;       /* the payload's BE u32 */
    uint32_t     reason;          /* 0 when unknown (have_reason == 0) */
    uint32_t     extended_code;
    int          have_reason, have_extended, resumable, have_resumable;
    /* KN_SCTL_PACING_MODE */
    uint32_t     stream_index;
    int          pacing_interval; /* mode == 1 */
} kn_sctl_msg;

/* Diagnostics (nvst_server_control.rs:148-153). `unknown` and
 * `malformed` SATURATE — the reference's test pins that a counter at
 * u64::MAX stays at u64::MAX instead of wrapping. */
typedef struct {
    unsigned long long unknown;
    unsigned long long malformed;
    unsigned long long invalid_stream;
    int      have_last_unknown;
    uint16_t last_unknown;
} kn_sctl_diag;

/* Decode one concatenated TLV buffer into a caller array, reproducing the
 * reference iterator (nvst_server_control.rs:107-146) exactly. Its three
 * error paths do NOT behave alike, and the difference is observable:
 *   - Oversized is decided at construction, so `len > KN_SCTL_MAX_BYTES`
 *     yields ONE error and nothing else;
 *   - a BOUNDS error (a 257th record -> TooManyCommands, fewer than 4
 *     bytes left -> Truncated, a payload length past the end ->
 *     Truncated) empties the buffer: ONE error, then the walk ends;
 *   - a DECODE error (a 0x104/0x105/0x109 payload shorter than 4 bytes,
 *     or a 0x112 payload that is not exactly 8) consumes ONLY its own
 *     record and THE WALK CONTINUES. Nothing is discarded.
 * The reference's own test buffer [0x999 ok][0x104 payload=1 byte]
 * [0x112 all-zero] therefore yields unknown, ONE malformed, then a
 * pacing-mode record with mode 0 — and its assertion is that
 * malformed == 1 while frame pacing ends up DISABLED.
 * Returns the number of messages written (0 when the buffer is empty).
 * `max_out` bounds the array; if it runs out the walk stops WITHOUT
 * consuming the record, so a bigger array sees more. */
size_t kn_sctl_walk(const uint8_t *data, size_t len, kn_sctl_msg *out,
                    size_t max_out);

void kn_sctl_diag_init(kn_sctl_diag *d);

/* Fold one decoded message into the diagnostics (Unknown -> unknown++ and
 * last_unknown; MALFORMED -> malformed++; a pacing-mode record whose
 * stream_index != 0 -> invalid_stream++). The caller drives the state
 * (idle-warning latch, pacing-enabled latch, termination) — this only
 * keeps the counters the reference's 10-second stats line prints. */
void kn_sctl_diag_add(kn_sctl_diag *d, const kn_sctl_msg *m);

/* session_resumable(reason, extended) (:23-34) exposed for tests: 1 =
 * resumable, 0 = not, -1 = undecidable (no reason/extended pair). */
int kn_sctl_resumable(uint32_t reason, uint32_t extended_code);

/* The reference's default for the frame-pacing latch is ENABLED
 * (nvst.rs:664 AtomicBool::new(true)) and 0x112 mode 1 keeps it enabled,
 * anything else disables it. Named here so the caller's initial value is
 * pinned by the header, not by a magic literal. */
#define KN_SCTL_PACING_DEFAULT_ENABLED 1

#ifdef __cplusplus
}
#endif
#endif /* KN_BW_H */
