/* kn_twcc.h — transport-wide congestion control: the RECEIVE register and
 * the feedback-packet builder (B-track slice 5l).
 *
 * Wire format: draft-holmer-rmcat-transport-wide-cc-extensions-01 (the
 * `a=extmap:3`/`a=rtcp-fb:* transport-cc` pair the GFN seat offers on
 * EVERY m-line and our answer mirrors). Algorithmic ground truth is str0m
 * (src/rtp/rtcp/twcc.rs — TwccRecvRegister/Twcc), the stack OpenNOW
 * streams through against real seats:
 *   - one Receipt {extended seq, arrival time} per inbound RTP packet that
 *     carries the TWCC extension, inserted SORTED, exact duplicates
 *     ignored (the sender reusing a seq is an error — ignore, not crash);
 *   - a report covers every receipt from `report_from` on; gaps between
 *     receipts become NotReceived runs; reference_time = the FIRST
 *     unreported receipt's time, quantized to 64 ms steps from the first
 *     ever report's `time_start`; the first recv delta is relative to that
 *     quantized base_time, every later delta relative to the time the
 *     PREVIOUS delta REPORTED (quantized accumulation — str0m's comment:
 *     carrying the true arrival would let each 250 us truncation compound);
 *   - status symbols: 0 NotReceived, 1 small delta (0..63.75 ms, 1 byte,
 *     250 us units), 2 large-or-negative delta (i16, 250 us units); a diff
 *     outside [-8 192 000, +8 191 750] us is unrepresentable — the report
 *     ends there and the rest waits for the next one (str0m's `break`);
 *   - packet: PT 205 (RTPFB) FMT 15, sender ssrc, media ssrc (str0m fills
 *     the first remote media ssrc — session-level feedback carries a
 *     media-level field), base_seq (low 16 of the extended), status_count,
 *     24-bit reference_time, 5-bit-ish feedback_count (u8, increments per
 *     generated report), 2-byte chunks, deltas, then RFC 3550 padding to a
 *     4-byte boundary with the P bit and the pad count in the last byte;
 *   - reported receipts are KEPT (str0m keep_reported=100) so a late
 *     packet can pull `report_from` back and be re-reported.
 *
 * Deliberate deviations from str0m (both wire-legal, documented in RTSP
 * §74.4): (1) CHUNK ENCODING is run-length ONLY — a status vector chunk
 * is never emitted. The draft requires RECEIVERS to support all chunk
 * types and lets senders choose freely; runs are near-optimal for GFN's
 * dense audio/video cadence (lossless reports are one run per status
 * segment). (2) Storage is a fixed 1024-receipt ring (zero allocation,
 * the project rule) instead of an unbounded VecDeque; eviction only bites
 * if builds stop for ~7 s at 150 pkt/s, and evictions are counted.
 *
 * Zero allocation; single-threaded (the probe's reader loop owns it).
 */
#ifndef KN_TWCC_H
#define KN_TWCC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_TWCC_CAPACITY       1024u  /* receipt ring */
#define KN_TWCC_KEEP_REPORTED  100u   /* str0m TwccRecvRegister::new(100) */
/* Per-report scratch bounds (the byte budget inside kn_twcc_build also
 * enforces the real limit; these cap the worst case for fixed arrays). */
#define KN_TWCC_MAX_CHUNKS     512u
#define KN_TWCC_MAX_DELTAS     512u

/* Packet status symbols (draft §3.1.1). */
#define KN_TWCC_ST_NOT_RECEIVED 0u
#define KN_TWCC_ST_SMALL_DELTA  1u
#define KN_TWCC_ST_LARGE_DELTA  2u

typedef struct kn_twcc_chunk {
    uint8_t  status;      /* KN_TWCC_ST_* */
    uint16_t run;         /* 1..8192 */
} kn_twcc_chunk;

typedef struct kn_twcc {
    /* sorted receipt ring; logical entry i lives at (head + i) % CAP */
    uint64_t seq[KN_TWCC_CAPACITY];         /* extended (unwrapped) seq */
    uint64_t arrival_us[KN_TWCC_CAPACITY];
    unsigned head;
    unsigned count;
    unsigned report_from;                   /* first unreported entry */
    int      have_last;
    uint64_t last_ext;                      /* for 16->64 extension */
    int      have_time_start;
    uint64_t time_start_us;                 /* first ever reported time */
    int      have_floor;                    /* trims/evictions removed past */
    uint64_t floor_ext;                     /* oldest still-placeable ext */
    unsigned long generated_reports;        /* feedback_count source */
    /* evidence counters (the flight reads these, not the wire) */
    unsigned long long recorded, dup_ignored, late_dropped,
                       old_time_dropped, evicted, reports_truncated;
} kn_twcc;

void kn_twcc_init(kn_twcc *t);

/* Record one inbound RTP packet's TWCC sequence (the 16-bit wire value
 * from the header extension) and its arrival time in microseconds
 * (kn_sock_now_us). Order-tolerant: out-of-order arrivals insert sorted
 * (below report_from they pull it back for a re-report, str0m semantics);
 * exact duplicates are ignored; arrivals below the trimmed/evicted floor
 * (reported history that is gone — the floor only exists where reports
 * were consumed) and arrivals more than 8192 ms before the report
 * epoch are counted and dropped. */
void kn_twcc_record(kn_twcc *t, uint16_t wire_seq, uint64_t arrival_us);

/* 1 when at least one receipt has not been reported yet. */
int kn_twcc_has_unreported(const kn_twcc *t);

/* Build the next feedback packet into out[0..out_len). Mirrors str0m's
 * build_report: consumes what fits (>= 1 receipt; chunks+deltas+padding
 * must stay within out_len), advances report_from, bumps the feedback
 * counter, trims reported receipts down to KN_TWCC_KEEP_REPORTED.
 * Returns the packet length (>= 24, multiple of 4), or 0 when there is
 * nothing reportable / out_len is too small for even one receipt. */
size_t kn_twcc_build(kn_twcc *t, uint32_t sender_ssrc, uint32_t media_ssrc,
                     uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* KN_TWCC_H */
