/* kn_rx.h — NVST video receive pipeline (Mjolnir stage-2 transport core).
 *
 * A zero-allocation C port of the pure media path of OpenNOW's
 * NvstVideoReceiver (nvst.rs @ a49b124): SRTP session unprotect, the RTP
 * reorder buffer, the FEC block collector + reorder buffer (Cauchy repair via
 * knfec + envelope re-wrap), reference-frame invalidation (RFI), the NACK
 * tracker, the frame-progress watchdog, the video timeout state machine, and
 * the control/RTCP feedback ENCODERS (ServerControl 0x0317 NACK-v2, 0x0301
 * RFI range, 0x0302 IDR, RTCP generic NACK + PLI).
 *
 * Provenance (every line range verified against nvst.rs / nvst_control.rs /
 * nvst_invalidation.rs @ a49b124 THIS round; full table in docs/RX.md):
 *   RtpReorderBuffer            nvst.rs:3537-3660  (52 ms dequeue: 229)
 *   FecBlock / FecReorderBuffer nvst.rs:3925-4310  (allowance 16: 215)
 *   FEC envelope re-wrap        nvst.rs:4055-4106  (pitfall #4: 4097-4106)
 *   NvstVideoReceiver pipeline  nvst.rs:4414-4961
 *   consume_reorder_result      nvst.rs:4741-4862
 *   invalidate_picture          nvst.rs:4867-4874
 *   FrameInvalidation (RFI)     nvst_invalidation.rs:118-269 (retry 100: 13,
 *                               backstop 250 ms: nvst.rs:103)
 *   NACK tracker                nvst.rs:946-985 + 1272-1333 (4/30/52 ms,
 *                               3 attempts, 2048 pending: 99-105)
 *   nack_v2 / ServerControl     nvst_control.rs:3-181 (codes 3-8, cap 64: 9)
 *   RTCP NACK/PLI               nvst.rs:3001-3048 (FCI cap: 208)
 *   SrtpReceiver::unprotect     nvst.rs:2635-2705 (GCM: 2711-2740,
 *                               material: 2576-2633, KDF256: 2873-2886)
 *   FrameProgressWatchdog       nvst.rs:2422-2442 + 4563-4615 (policy:
 *                               stall=grace=config.timeout, 9124-9126)
 *
 * Design rules (project conventions):
 *   - ZERO allocation: every buffer (packet pool, reorder map, FEC block
 *     table, NACK table, AU storage, reconstruct scratch, event array) is
 *     caller-owned and passed at init. Bounded pools replace the reference's
 *     Vec/BTreeMap; exhaustion degrades honestly (counted drops), never UB.
 *   - DETERMINISTIC TIME: every call takes `now_ms` (uint64, any monotonic
 *     origin). No clock reads inside — scenarios replay identically in tests,
 *     on host and on target.
 *   - PURE CORE: the pipeline never touches sockets. process_datagram()
 *     consumes one received UDP payload; the platform receive loop (stage-2
 *     sockets work) feeds it. Frames are delivered through a synchronous
 *     callback (AU bytes alias the assembler buffer — consume or copy inside
 *     the callback, exactly like kn_decode's frame callback).
 *   - FAITHFUL OVER CLEVER: reference quirks are ported as-is and documented
 *     (e.g. a gap recovery DISCARDS packets popped before the gap in the
 *     same drain — nvst.rs:3645-3652 ready.clear()).
 *
 * Documented deviations (bounded C reality; see docs/RX.md §Deviations):
 *   D-a  Packet storage is a fixed pool; the reference allocates per packet.
 *        A packet that does not fit (pool full or len > pkt_cap) is dropped
 *        with KN_RX_DROP_POOL and counted — poll prunes the FEC map on every
 *        push exactly like the reference, so exhaustion needs sustained loss.
 *   D-b  FEC blocks declaring data+parity > KN_RX_FEC_MAX_SHARDS (compile
 *        time, <= 255 = the knfec Cauchy coordinate bound) are rejected as
 *        Unsupported, matching what the reference's reconstruct would do for
 *        any LOSS in such a block.
 *   D-c  The feedback STATISTICS plane (publish_stream_packet, frame stage
 *        timings, RTCP Receiver Reports) is stage-3 work (needs SCTP/GCM
 *        SRTCP); this port keeps the local counters the stats_line reports
 *        and tracks last_assembled_at on the receive side (the reference
 *        publishes it from the same receive loop, nvst.rs:9199).
 *   D-d  SRTP AES-GCM and the 256-bit profiles need mbedTLS
 *        (KN_TLS_BACKEND_MBEDTLS builds); bare builds accept CM-128-HMAC
 *        only and report KN_RX_DROP_NO_CIPHER honestly for the rest.
 */
#ifndef KN_RX_H
#define KN_RX_H

#include <stddef.h>
#include <stdint.h>

#include "kn/kn_bw.h"
#include "kn/kn_gs.h"
#include "kn/knsrtp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- wire constants */

#define KN_RX_REORDER_TIMEOUT_MS   52u   /* MJOLNIR_REORDER_DEQUEUE_TIMEOUT
                                            (nvst.rs:229) */
#define KN_RX_DEFAULT_REORDER_WINDOW 1024u /* nvst.rs:224 */
#define KN_RX_MAX_REORDER_WINDOW   2048u /* nvst.rs:225 */
#define KN_RX_FEC_HEADER_ALLOWANCE 16u   /* NVST_FEC_RTP_HEADER_ALLOWANCE
                                            (nvst.rs:215) */
#define KN_RX_FEC_MAX_BLOCKS       128u  /* MAX_PENDING_FEC_BLOCKS
                                            (nvst.rs:211) */
#define KN_RX_FEC_MAX_SHARDS       255u  /* knfec Cauchy bound (nvst.rs:3806)
                                            — data+parity per block */
#define KN_RX_NACK_RETRY_MS        4u    /* nvst.rs:99 */
#define KN_RX_NACK_DEFAULT_RTT_MS  30u   /* nvst.rs:100 */
#define KN_RX_NACK_TRACKING_MS     52u   /* nvst.rs:101 */
#define KN_RX_NACK_MAX_ATTEMPTS    3u    /* nvst.rs:102 */
#define KN_RX_NACK_MAX_PENDING     2048u /* nvst.rs:105 */
#define KN_RX_NACK_MAX_PACKET_COUNT 64u  /* MAX_NACK_PACKET_COUNT
                                            (nvst_control.rs:9) */
#define KN_RX_NACK_MAX_FCI_ENTRIES 4u    /* div_ceil(64, 17) (nvst.rs:208) */
#define KN_RX_RFI_RETRY_MS         100u  /* nvst_invalidation.rs:13 */
#define KN_RX_RFI_BACKSTOP_MS      250u  /* nvst.rs:103 */
#define KN_RX_IDR_RETRY_MS         100u  /* nvst.rs:104 */
#define KN_RX_DEFAULT_VIDEO_PACKET_SIZE 1280u /* nvst.rs:216 */
#define KN_RX_MIN_VIDEO_PACKET_SIZE 256u      /* nvst.rs:217 */
#define KN_RX_MAX_VIDEO_PACKET_SIZE 65519u    /* nvst.rs:218 */
#define KN_RX_DEFAULT_TIMEOUT_MS   5000u /* nvst.rs:238 */
#define KN_RX_MIN_TIMEOUT_MS       250u  /* nvst.rs:239 */
#define KN_RX_MAX_TIMEOUT_MS       90000u /* nvst.rs:240 */

/* ServerControl command codes (nvst_control.rs:3-8) — little-endian u16
 * code + little-endian u16 payload length + payload (encoded(): 28-38). */
#define KN_RX_CTRL_FRAME_PACING     0x0203u
#define KN_RX_CTRL_FRAME_ACK        0x0204u
#define KN_RX_CTRL_QOS_REPORT       0x0207u
#define KN_RX_CTRL_FRAME_INVALIDATION 0x0301u
#define KN_RX_CTRL_IDR_REQUEST      0x0302u
#define KN_RX_CTRL_NACK_V2          0x0317u
#define KN_RX_CTRL_FRAME_ACK_PAYLOAD_LEN 102u
#define KN_RX_CTRL_FRAME_PACING_PAYLOAD_LEN 28u
#define KN_RX_CTRL_QOS_PAYLOAD_LEN  52u
#define KN_RX_CTRL_FRAME_INVALIDATION_PAYLOAD_LEN 24u

/* ------------------------------------------------------------- returns */

enum {
    KN_RX_OK            =  0,
    KN_RX_ERR_ARGS      = -1,
    KN_RX_ERR_STATE     = -2,   /* init preconditions (sizes, caps) */
    KN_RX_ERR_NO_CIPHER = -3    /* profile needs a backend this build lacks */
};

/* Drop reasons — mirror the media-path subset of NvstDropReason
 * (nvst.rs:2283-2323). KN_RX_DROP_POOL / _NO_CIPHER / _PKT_TOO_LARGE are
 * the bounded-C additions (deviation D-a/D-d). */
typedef enum kn_rx_drop {
    KN_RX_DROP_NONE = 0,
    KN_RX_DROP_PAUSED,             /* datagram in Paused state */
    KN_RX_DROP_STOPPED,
    KN_RX_DROP_RECOVERY_REQUIRED,
    KN_RX_DROP_MALFORMED_RTP,      /* detail = kn_gs error code */
    KN_RX_DROP_AUTH_FAILED,
    KN_RX_DROP_REPLAY_REJECTED,
    KN_RX_DROP_UNEXPECTED_PT,
    KN_RX_DROP_UNEXPECTED_SSRC,
    KN_RX_DROP_STALE,              /* index carried in the event */
    KN_RX_DROP_DUPLICATE,
    KN_RX_DROP_UNSUPPORTED_FEC,    /* NvstUnsupportedFeature::FecRepair */
    KN_RX_DROP_AWAITING_SOF,
    KN_RX_DROP_FRAME_DISCONTINUITY,
    KN_RX_DROP_MISSING_START_CODE,
    KN_RX_DROP_INVALID_AU_LENGTH,
    KN_RX_DROP_AU_TOO_LARGE,
    KN_RX_DROP_FEC_LAYOUT,         /* layout/matches mismatch on FEC push */
    KN_RX_DROP_POOL,               /* packet pool exhausted (D-a) */
    KN_RX_DROP_PKT_TOO_LARGE,      /* datagram > pkt_cap (D-a) */
    KN_RX_DROP_NO_CIPHER,          /* profile unavailable in this build (D-d) */
    KN_RX_DROP_TIMEOUT_RESET       /* informational: media state was reset by
                                      the video timeout (not a packet drop) */
} kn_rx_drop;

/* Receiver lifecycle state (NvstReceiverState, nvst.rs — media subset). */
typedef enum kn_rx_state {
    KN_RX_STATE_RUNNING = 0,
    KN_RX_STATE_PAUSED,
    KN_RX_STATE_RECOVERY_REQUIRED,
    KN_RX_STATE_STOPPED
} kn_rx_state;

/* Non-frame event kinds (frames go through the callback — see below). */
typedef enum kn_rx_event_type {
    KN_RX_EV_NONE = 0,
    KN_RX_EV_DROPPED,          /* one dropped packet/record */
    KN_RX_EV_RECOVERY_GAP,     /* NvstRecovery::PacketGap */
    KN_RX_EV_RECOVERY_TIMEOUT, /* NvstRecovery::Timeout */
    KN_RX_EV_LIFECYCLE,        /* state change (pause/resume/recover/stop) */
    KN_RX_EV_PROGRESS_RESUMED, /* FrameProgressResumed */
    KN_RX_EV_PROGRESS_STALL,   /* FrameProgressStall (keyframe requested) */
    KN_RX_EV_PROGRESS_RECOVERY /* frame progress -> RecoveryRequired */
} kn_rx_event_type;

typedef struct kn_rx_event {
    int      type;             /* kn_rx_event_type */
    int      drop_reason;      /* KN_RX_DROP_* for DROPPED */
    int      detail;           /* kn_gs error code for MALFORMED_RTP */
    uint64_t index;            /* packet index for STALE/DUPLICATE/drops */
    uint64_t gap_first;        /* RECOVERY_GAP: first/last missing index */
    uint64_t gap_last;
    uint64_t idle_ms;          /* RECOVERY_TIMEOUT / PROGRESS_*: idle time */
    uint32_t last_frame;       /* PROGRESS_*: last assembled frame (if any) */
    int      have_last_frame;
    int      new_state;        /* LIFECYCLE: kn_rx_state */
} kn_rx_event;

/* Frame callback: invoked synchronously for every ADMITTED access unit.
 * f->bytes aliases the pipeline's AU buffer and is valid ONLY during the
 * call (consume or copy — same contract as kn_decode's frame callback). */
typedef struct kn_rx_frame {
    int       codec;             /* kn_gs_codec */
    uint32_t  timestamp;         /* RTP timestamp of the completing packet */
    uint32_t  frame_index;
    uint32_t  first_stream_packet_index;
    unsigned  frame_type_wire;   /* NvstFrameType wire value (0 = unknown) */
    int       frame_type_known;  /* GsFrameHeader parsed at all (Option::is_some) */
    int       frame_type_valid;  /* wire value in 1..=6 */
    int       keyframe;
    int       contiguous;        /* FINAL value after RFI admit() logic */
    const uint8_t *bytes;
    size_t    bytes_len;
    /* Round XL: the wall-clock ms (kn_rx's now_ms domain) at which the
     * FIRST packet of this AU was processed — the reference's
     * CompletedFrameFeedback timing.first_at, which frame_ack turns into
     * first_packet_time_ms = (first_at - transport_origin) as f64 ms
     * (nvst.rs:447-462). 0 before any packet was seen. */
    uint64_t  first_packet_ms;
} kn_rx_frame;

typedef void (*kn_rx_frame_fn)(const kn_rx_frame *frame, void *userdata);

/* --------------------------------------------------------- packet pool */

/* One buffered packet. `bytes` lives in the pool's byte arena at
 * slot * pkt_cap; len <= pkt_cap. rtp is the parse of the stored bytes —
 * it stays valid because pool slots never move. */
typedef struct kn_rx_pkt {
    uint64_t  index;           /* extended SRTP packet index (u48+) */
    uint32_t  len;             /* plaintext packet bytes stored */
    uint8_t   origin;          /* 0 = authenticated, 1 = recovered data */
    uint8_t   used;            /* slot occupied */
    int       rtp_ok;          /* kn_gs_rtp_parse succeeded at insert */
    int       rtp_err;         /* kn_gs error when !rtp_ok */
    kn_gs_rtp rtp;             /* parsed header + payload bounds */
} kn_rx_pkt;

typedef struct kn_rx_pool {
    kn_rx_pkt *meta;           /* slots entries (caller-owned) */
    uint8_t   *bytes;          /* slots * pkt_cap bytes (caller-owned) */
    uint32_t   slots;
    uint32_t   pkt_cap;
    uint32_t   free_head;      /* index+1 of the first free slot, 0 = none */
    uint32_t   in_use;
    /* lifetime counters */
    uint64_t   alloc_failures; /* requested a slot while full (D-a) */
} kn_rx_pool;

int  kn_rx_pool_init(kn_rx_pool *p, kn_rx_pkt *meta, uint32_t slots,
                     uint8_t *bytes, uint32_t pkt_cap);
void kn_rx_pool_reset(kn_rx_pool *p);           /* free every slot */
/* Return one slot (the pipeline consumes ready packets with it; direct
 * users of the reorder/FEC buffers must release drained slots themselves).
 * No-op for invalid/unused slots. */
void kn_rx_pool_release(kn_rx_pool *p, uint32_t slot);

/* ------------------------------------------------------- reorder buffer */

/* Port of RtpReorderBuffer (nvst.rs:3537-3660). The map is an unordered
 * array of (index -> slot) entries with linear min/max scans; the reference
 * BTreeMap is ordered, so first_key_value/last_key_value are min/max here.
 * Behaviour is identical because the drain loop only ever asks for the
 * minimum above next_index and the maximum base. */
typedef struct kn_rx_reorder_entry {
    uint64_t index;
    uint32_t slot;             /* pool slot id */
    uint8_t  used;
} kn_rx_reorder_entry;

typedef struct kn_rx_reorder {
    kn_rx_reorder_entry *map;  /* max_packets + 1 entries (see docs/RX.md) */
    uint32_t   max_packets;    /* 1..KN_RX_MAX_REORDER_WINDOW */
    uint64_t   next_index;
    int        have_next;
    int        have_gap_wait;
    uint64_t   gap_wait_index;
    uint64_t   gap_wait_start_ms;
    uint32_t   count;          /* entries in use */
} kn_rx_reorder;

/* Result of a reorder/FEC operation — ReorderResult (nvst.rs:3544-3550).
 * ready[] holds pool slot ids valid until the next push/poll of the SAME
 * buffer or a pool reset; the pipeline consumes them immediately. */
typedef struct kn_rx_result {
    uint32_t  ready[KN_RX_MAX_REORDER_WINDOW + 1]; /* slot ids */
    uint32_t  ready_len;
    int       have_nack;
    uint64_t  nack_first, nack_last;   /* extended indices, inclusive */
    int       have_recovery;
    uint64_t  rec_first, rec_last;     /* PacketGap missing range */
    int       have_dropped;
    int       drop_reason;             /* kn_rx_drop */
    uint64_t  drop_index;
    uint32_t  fec_repaired;
} kn_rx_result;

int  kn_rx_reorder_init(kn_rx_reorder *r, kn_rx_reorder_entry *map,
                        uint32_t map_entries, uint32_t max_packets);
void kn_rx_reorder_reset(kn_rx_reorder *r);

/* Insert an authenticated packet (pool slot already filled). Copies nothing;
 * takes ownership of the slot (frees it when drained/dropped). */
int  kn_rx_reorder_push(kn_rx_reorder *r, kn_rx_pool *pool, uint32_t slot,
                        uint64_t index, uint64_t now_ms, kn_rx_result *out);
int  kn_rx_reorder_poll(kn_rx_reorder *r, kn_rx_pool *pool, uint64_t now_ms,
                        kn_rx_result *out);

/* -------------------------------------------------------------- FEC */

/* FecBlock (nvst.rs:3925-4117). shard_slots[i] = pool slot of shard i, or
 * KN_RX_NO_SLOT. The shard table is a slice of the caller's block arena. */
#define KN_RX_NO_SLOT 0xFFFFFFFFu

typedef struct kn_rx_fec_block {
    kn_gs_fec_layout layout;
    uint64_t  base_index;
    uint32_t *shard_slots;     /* total = data+parity entries */
    int       repair_failed;   /* one reconstruction attempt burned */
    uint8_t   used;
} kn_rx_fec_block;

typedef struct kn_rx_fec {
    kn_rx_fec_block *blocks;   /* max_blocks entries */
    uint32_t *shard_arena;     /* max_blocks * max_shards slot ids */
    uint32_t   max_blocks;
    uint32_t   max_shards;     /* per block; <= KN_RX_FEC_MAX_SHARDS */
    size_t     shard_len;      /* video_packet_size + 16 (nvst.rs:4133) */
    uint64_t   completed_through;
    int        have_completed_through;
    int        have_gap_wait;
    uint64_t   gap_wait_index;
    uint64_t   gap_wait_start_ms;
    /* reconstruct scratch (caller-owned; kn_fec_scratch_bytes sized) */
    uint8_t   *scratch;
    size_t     scratch_len;
    /* recovered-packet staging: one pkt_cap-sized buffer per reconstruct
     * call is NOT enough (up to parity shards recovered at once) — recovery
     * writes directly into freshly allocated pool slots. */
    uint64_t   blocks_completed;
    uint64_t   blocks_repair_failed;
    uint64_t   blocks_dropped_timeout;
    uint64_t   packets_repaired;
} kn_rx_fec;

int  kn_rx_fec_init(kn_rx_fec *f, kn_rx_fec_block *blocks, uint32_t max_blocks,
                    uint32_t *shard_arena, uint32_t max_shards,
                    size_t video_packet_size,
                    uint8_t *scratch, size_t scratch_len);
void kn_rx_fec_reset(kn_rx_fec *f);

/* Insert a packet carrying a valid FEC layout (slot ownership as above). */
int  kn_rx_fec_push(kn_rx_fec *f, kn_rx_pool *pool, uint32_t slot,
                    uint64_t index, const kn_gs_fec_layout *layout,
                    uint64_t now_ms, kn_rx_result *out);
int  kn_rx_fec_poll(kn_rx_fec *f, kn_rx_pool *pool, uint64_t now_ms,
                    kn_rx_result *out);

/* ------------------------------------------------- frame invalidation */

/* Port of FrameInvalidation + FrameInvalidationFeedback
 * (nvst_invalidation.rs:45-269). The RFI queue models the control-channel
 * sender's view: queue/take/restore/record_sent (stage 3 consumes it). */
typedef struct kn_rx_inval {
    /* FrameInvalidation state */
    uint32_t last_frame;   int have_last_frame;
    int      frame_types_known;
    int      have_pending; uint64_t pending_first, pending_last;
    int      have_active_since; uint64_t active_since_ms;
    int      have_last_retry;   uint64_t last_retry_ms;
    int      backstop_requested;
    /* FrameInvalidationFeedback state */
    int      have_unsent;  uint64_t unsent_first, unsent_last;
    int      rfi_in_flight;
    int      have_rfi_last_sent; uint64_t rfi_last_sent_ms;
    uint64_t cnt_sent, cnt_dropped_frames, cnt_recovered_i,
             cnt_recovered_intra_refresh, cnt_recovered_ref_pic_i,
             cnt_idr_backstops;
} kn_rx_inval;

void kn_rx_inval_init(kn_rx_inval *v);
void kn_rx_inval_reset(kn_rx_inval *v);

/* Returns 1 when invalidation owns the recovery (caller must NOT request a
 * keyframe), 0 when the caller should request one immediately (frame types
 * unknown on this stream). */
int  kn_rx_inval_lose(kn_rx_inval *v, uint64_t now_ms);

/* poll_backstop: re-queues the pending RFI range every 100 ms until sent and
 * fires the one-shot IDR backstop after 250 ms of continuous loss.
 * *out_keyframe_request is set to 1 when the backstop fired. */
void kn_rx_inval_poll_backstop(kn_rx_inval *v, uint64_t now_ms,
                               int *out_keyframe_request);

/* admit: decides whether an assembled frame may reach the decoder, sets
 * f->contiguous (the FINAL value) and *out_keyframe_request (RFI streams
 * without frame types keep the immediate keyframe request). Returns 1 =
 * admit (deliver), 0 = withhold (dropped_frames counter). */
int  kn_rx_inval_admit(kn_rx_inval *v, kn_rx_frame *f, int loss_before,
                       uint64_t now_ms, int *out_keyframe_request);

/* Control-channel sender helpers (stage 3): take the queued RFI range for
 * sending; restore it after a failed send; record a successful send. */
int  kn_rx_inval_take_rfi(kn_rx_inval *v, uint64_t *first, uint64_t *last);
void kn_rx_inval_restore_rfi(kn_rx_inval *v, uint64_t first, uint64_t last);
void kn_rx_inval_record_sent(kn_rx_inval *v, uint64_t now_ms);

/* --------------------------------------------------------- NACK plane */

/* Port of the feedback NACK tracker (nvst.rs:946-985, 1272-1333). */
typedef struct kn_rx_nack_entry {
    uint64_t index;
    uint64_t requested_at_ms;
    uint64_t last_sent_at_ms;  int have_last_sent;
    uint8_t  attempts;
    uint8_t  used;
} kn_rx_nack_entry;

typedef struct kn_rx_nack {
    kn_rx_nack_entry *table;   /* KN_RX_NACK_MAX_PENDING entries */
    uint32_t capacity;
    uint32_t count;
    uint64_t overflow_events;  /* request hit the cap (-> keyframe) */
    uint64_t requests_total;   /* ranges requested */
    uint64_t sends_total;      /* take_nack hits */
    uint64_t resolved_total;   /* retransmissions that had been sent */
    uint64_t expired_total;    /* tracking-timeout evictions */
} kn_rx_nack;

int  kn_rx_nack_init(kn_rx_nack *n, kn_rx_nack_entry *table, uint32_t capacity);
void kn_rx_nack_reset(kn_rx_nack *n);

/* Queue a missing range. Sets *out_keyframe_request on overflow (the
 * reference requests a keyframe when the pending table is full). */
void kn_rx_nack_request(kn_rx_nack *n, uint64_t first, uint64_t last,
                        uint64_t now_ms, int *out_keyframe_request);
/* A packet arrived: drop it from pending. Returns 1 when it had been SENT
 * (a real retransmission recovery), 0 otherwise (resolve_nack semantics). */
int  kn_rx_nack_resolve(kn_rx_nack *n, uint64_t index);
/* Next sendable contiguous run (retry interval / attempt caps per the
 * Mjolnir NACK-v2 policy). retry_rtt_ms: 0 = no RTT sample (fixed 4 ms /
 * 3 attempts), else clamp(52/rtt, 1, 3) attempts at rtt+4 ms interval. */
int  kn_rx_nack_take(kn_rx_nack *n, uint64_t now_ms, uint32_t retry_rtt_ms,
                     uint64_t *first, uint64_t *last);
void kn_rx_nack_mark_send_failed(kn_rx_nack *n, uint64_t first, uint64_t last);
void kn_rx_nack_clear(kn_rx_nack *n);

/* ------------------------------------------------ feedback encoders */

/* ServerControl NACK-v2 (nvst_control.rs:40-70 + docs/mjolnir-nack-v2.md):
 * missing = ascending list of 16-bit RTP sequence numbers (1..64 entries;
 * duplicates skipped). Writes the FULL encoded command (code+len+payload)
 * to out. Returns the encoded length, 0 on empty/oversize input (the
 * reference returns None), or -1 when out_len is too small (need <= 647). */
int kn_rx_ctrl_nack_v2(uint8_t stream_index, const uint16_t *missing,
                       uint32_t missing_count, uint8_t *out, size_t out_len);

/* IDR request (0x0302, payload = u16LE 0). Returns encoded length (6). */
int kn_rx_ctrl_idr_request(uint8_t out[6]);

/* Frame invalidation range (0x0301, 24-byte payload: u64LE first, u64LE
 * last, u64LE stream_index). Returns encoded length (28). */
int kn_rx_ctrl_frame_invalidation(uint64_t first_frame, uint64_t last_frame,
                                  uint16_t stream_index,
                                  uint8_t out[4 + KN_RX_CTRL_FRAME_INVALIDATION_PAYLOAD_LEN]);

/* Frame ack (0x0204, 102-byte payload; nvst_control.rs:72-94).
 * first_packet_time_ms: NULL -> zeros (the reference's None). */
int kn_rx_ctrl_frame_ack(uint32_t frame_number,
                         const double *first_packet_time_ms,
                         uint32_t frame_bytes,
                         uint8_t out[4 + KN_RX_CTRL_FRAME_ACK_PAYLOAD_LEN]);

/* Frame pacing report (0x0203, 28-byte payload; nvst_control.rs:96-112). */
int kn_rx_ctrl_frame_pacing(uint32_t frame_number, uint32_t target_frame_time_us,
                            uint32_t pacing_error_us,
                            uint8_t out[4 + KN_RX_CTRL_FRAME_PACING_PAYLOAD_LEN]);

/* QoS report (0x0207, 52-byte payload; nvst_control.rs:118-160). */
typedef struct kn_rx_qos {
    uint32_t sequence;
    uint32_t sender_frame_number;
    uint32_t bytes_received;
    uint16_t loss_per_ten_thousand;
    uint32_t client_time_90khz;
    int      have_packet_snapshot;
    uint32_t snap_ssrc, snap_base, snap_highest, snap_received;
    uint32_t bw_minimum_server_time, bw_queue_delay_us, bw_median_server_time,
             bw_lossy_frames, bw_estimate_kbps;
    uint16_t bw_jitter_us;
    uint8_t  bw_utilization_percent;
} kn_rx_qos;
int kn_rx_ctrl_qos_report(const kn_rx_qos *q,
                          uint8_t out[4 + KN_RX_CTRL_QOS_PAYLOAD_LEN]);

/* RTCP PLI (RFC 4585 PSFB; nvst.rs:3001-3009). Writes 12 bytes. */
void kn_rx_rtcp_pli(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t out[12]);

/* RTCP Receiver Report, ONE report block — byte-exact with the
 * reference's build_rtcp_receiver_report (nvst.rs:2983-2997): 0x81 /
 * PT 201 / length 7, sender ssrc, media ssrc, fraction_lost (u8, the
 * 256-scaled interval ratio), cumulative_lost (24-bit two's-complement,
 * clamped to [-0x800000, 0x7fffff] like report_snapshot :1233),
 * highest extended sequence, jitter, and LSR=DLSR=0 (the reference
 * never fills them — RTCP-over-SCTP has no NTP exchange). Writes 32
 * bytes. Sent on the rtcp1 channel every SRTCP_RR_INTERVAL (1 s,
 * :97/:8041-8060) — "so the peer keeps the video flowing" (:4320). */
void kn_rx_rtcp_rr(uint32_t sender_ssrc, uint32_t media_ssrc,
                   uint8_t fraction_lost, int32_t cumulative_lost,
                   uint32_t highest_sequence, uint32_t jitter,
                   uint8_t out[32]);


/* RTCP generic NACK (RFC 4585 §6.2.1; nvst.rs:3015-3048): FCI entries
 * PID+BLP over the extended-index range truncated to 64 packets / 4 entries.
 * Returns the packet length (12 + 4*entries). */
size_t kn_rx_rtcp_nack(uint32_t sender_ssrc, uint32_t media_ssrc,
                       uint64_t first_missing, uint64_t last_missing,
                       uint8_t *out, size_t out_len);

/* -------------------------------------------------------- SRTP session */

/* SrtpReceiver (nvst.rs:2546-2705): session material derivation + replay
 * window + per-profile unprotect. Profiles mirror NvstSrtpMaterial. */
typedef enum kn_rx_srtp_profile {
    KN_RX_SRTP_NONE = 0,          /* plaintext mode (replay/capture, tests) */
    KN_RX_SRTP_CM128_HMAC_SHA1,   /* tag 4 or 10 */
    KN_RX_SRTP_CM256_HMAC_SHA1,   /* tag 4 or 10 — needs mbedTLS build */
    KN_RX_SRTP_GCM128,            /* tag 8 or 16 — needs mbedTLS build */
    KN_RX_SRTP_GCM256             /* tag 8 or 16 — needs mbedTLS build;
                                     the legacy nvstVideo default */
} kn_rx_srtp_profile;

typedef struct kn_rx_srtp_session {
    int      profile;
    size_t   tag_len;
    int      key_len;             /* 16 or 32 */
    uint8_t  enc_key[32];
    uint8_t  auth_key[20];
    uint8_t  salt[14];
    kn_srtp_replay replay;
    int      ready;               /* material derived successfully */
} kn_rx_srtp_session;

/* Derive session material from master key/salt (nvst.rs:2576-2633).
 * master_key: 16 bytes for the *128 profiles, 32 for the *256 ones.
 * Returns KN_RX_OK, KN_RX_ERR_ARGS, or KN_RX_ERR_NO_CIPHER when the build
 * lacks the backend (bare builds: CM128 only). */
int kn_rx_srtp_session_init(kn_rx_srtp_session *s, int profile,
                            const uint8_t *master_key, size_t master_key_len,
                            const uint8_t *master_salt, size_t master_salt_len,
                            size_t tag_len);

/* Unprotect one datagram IN PLACE (the payload region is decrypted; the tag
 * is stripped from the reported plaintext length). Order of operations is
 * the reference's exactly: parse -> replay guess -> verify+decrypt ->
 * replay check -> payload(padding) validation -> replay commit.
 * Returns KN_RX_OK (0) and fills out_index, out_rtp, out_plain_len; or a
 * POSITIVE kn_rx_drop verdict (KN_RX_DROP_MALFORMED_RTP with *out_detail =
 * kn_gs code, KN_RX_DROP_AUTH_FAILED, KN_RX_DROP_REPLAY_REJECTED,
 * KN_RX_DROP_NO_CIPHER), or KN_RX_ERR_ARGS. On auth failure the payload
 * region must be treated as garbage (the GCM path scrubs it). */
int kn_rx_srtp_unprotect(kn_rx_srtp_session *s, uint8_t *datagram, size_t len,
                         uint64_t *out_index, kn_gs_rtp *out_rtp,
                         size_t *out_plain_len, int *out_detail);

/* ------------------------------------------------------------- pipeline */

typedef struct kn_rx_video_config {
    int      codec;                  /* kn_gs_codec; H264 for Mjolnir */
    uint32_t reorder_window_packets; /* 1..2048 (0 -> default 1024) */
    size_t   max_access_unit_bytes;  /* <= 16 MiB (0 -> default 2 MiB) */
    uint32_t video_packet_size;      /* 256..65519 (0 -> default 1280) */
    int      have_expected_pt;  unsigned expected_payload_type;
    int      have_expected_ssrc; uint32_t expected_ssrc;
    uint32_t timeout_ms;             /* 250..90000 (0 -> 5000) */
    uint32_t startup_timeout_ms;     /* >= timeout_ms (0 -> timeout_ms) */
    uint32_t frame_time_us;          /* 0 -> 16666 (pacing feedback, stage 3) */
    int      rtcp_on_sctp;           /* recorded; SRTCP RR is stage 3 */
    /* Progress-watchdog policy: the production values are stall = grace =
     * config.timeout (nvst.rs:9124-9126). 0 -> same default. */
    uint32_t progress_stall_ms;
    uint32_t progress_keyframe_grace_ms;
    /* Round XXXVIII (D-t10) — the bandwidth estimator's clock. The
     * reference hands BandwidthEstimator an `Instant` and does every
     * subtraction on integer-nanosecond Durations (nvst_bandwidth.rs:60,
     * :96-108, :212), so sub-millisecond arrival spacing is part of the
     * measured quantity: a frame's packets usually land inside ONE
     * millisecond, and a ms-granular clock would read that train as 0 ms
     * and clamp it to the reference's 0.5 ms floor (a 2x receive-rate
     * error, straight into estimate_kbps). `now_ns(ud)` returns any
     * monotonic integer-nanosecond reading; only DIFFERENCES are used, so
     * the origin is irrelevant. NULL (tests, capture/KAT replay, which
     * must stay deterministic) falls back to now_ms * 1000000. */
    int64_t (*now_ns)(void *ud);
    void    *now_ns_ud;
} kn_rx_video_config;

/* Storage bundle for kn_rx_video_init — one struct so the caller sees every
 * bound at a glance. All pointers caller-owned; sizes are validated. */
typedef struct kn_rx_mem {
    /* packet pool */
    kn_rx_pkt *pool_meta;   uint32_t pool_slots;
    uint8_t   *pool_bytes;  uint32_t pool_pkt_cap;
    /* reorder map: reorder_window_packets + 1 entries */
    kn_rx_reorder_entry *reorder_map;
    /* FEC: blocks + shard arena (blocks * shards_per_block) + scratch */
    kn_rx_fec_block *fec_blocks;  uint32_t fec_max_blocks;
    uint32_t *fec_shard_arena;    uint32_t fec_max_shards;
    uint8_t  *fec_scratch;        size_t   fec_scratch_len;
    /* NACK table (KN_RX_NACK_MAX_PENDING entries recommended) */
    kn_rx_nack_entry *nack_table; uint32_t nack_capacity;
    /* AU assembly buffer (>= max_access_unit_bytes) */
    uint8_t  *au_buffer;          size_t   au_buffer_capacity;
    /* non-frame event staging per call */
    kn_rx_event *events;          uint32_t events_capacity;
} kn_rx_mem;

typedef struct kn_rx_video {
    kn_rx_video_config cfg;
    kn_rx_mem   mem;               /* copy of the validated bounds */
    kn_rx_pool  pool;
    kn_rx_reorder reorder;
    kn_rx_fec   fec;
    kn_rx_inval inval;
    kn_rx_nack  nack;
    kn_gs_au_assembler assembler;
    kn_rx_srtp_session srtp;

    int      state;                /* kn_rx_state */
    uint32_t bound_ssrc;    int have_bound_ssrc;
    uint32_t base_sequence; int have_base_sequence;  /* round XXXVI: the
        FIRST extended sequence of the bound stream — the RR report's
        expected-count anchor (the reference's base_sequence, :1225) */
    uint32_t highest_sequence_received;
    uint64_t authenticated_packets;
    uint32_t last_stream_packet_index; int have_last_spi;
    int      next_frame_contiguous;

    /* timeouts / watchdog */
    uint64_t timeout_origin_ms;    /* init/last reset reference */
    uint64_t last_authenticated_ms; int have_last_authenticated;
    int      initial_timeout_pending;
    /* FrameProgressWatchdog (nvst.rs:2422-2442) */
    uint64_t wp_first_authenticated_ms; int have_wp_first_authenticated;
    int      wp_first_keyframe_assembled;
    uint32_t wp_last_frame;       int have_wp_last_frame;
    uint64_t wp_last_assembled_ms; int have_wp_last_assembled;
    int      wp_stage;            /* 0 tracking, 1 keyframe pending, 2 recovery */
    uint64_t wp_keyframe_requested_ms; int have_wp_keyframe_requested;

    /* keyframe-request flag (feedback.request_keyframe): set by any loss
     * path without RFI ownership; cleared when a keyframe is emitted. */
    int      keyframe_needed;
    uint64_t keyframe_requests;

    /* stats (stats_line equivalents, nvst.rs:4884-4906) */
    uint64_t fec_packets, fec_repaired_packets, frames_emitted,
             replay_rejections, stale_packets, recovered_retransmissions,
             packet_gap_recoveries, withheld_frames;
    uint64_t drop_counters[24];    /* per kn_rx_drop reason */

    /* per-call event staging */
    uint32_t ev_count, ev_capacity, ev_overflow;

    /* ---- round XXXVIII (D-t10): the BandwidthEstimator ----
     * publish_stream_packet (nvst.rs:852-899) feeds it from the
     * authenticated datagram path only, and resets it when the media
     * ssrc changes. `bw_ssrc` is our copy of the reference's
     * qos_packet_snapshot.ssrc — the field whose change triggers
     * `timing.bandwidth = BandwidthEstimator::default()` (:875-884).
     * The samples fed are counted so a log can prove the estimator was
     * actually driven (run 18's QoS block was zeros for 4701/4932
     * reports and nothing in the log said so). */
    kn_bw    bw;
    uint32_t bw_ssrc;        int have_bw_ssrc;
    uint64_t bw_samples, bw_parity_samples, bw_skipped_reordered,
             bw_skipped_retransmitted, bw_skipped_no_video;

    /* The QoS packet snapshot (nvst.rs:860-914). These are SEPARATE from
     * the RR counters above on purpose: base_sequence/highest_sequence/
     * received_packets back the RTCP report block and are u64, while the
     * 0x0207 snapshot is u32 with `received` SATURATING at u32::MAX (the
     * reference's `previous.received < u32::MAX` guard, :901) and `base`
     * latched once by compare_exchange (:861-866). */
    uint32_t qos_base_sequence;  int have_qos_base;
    uint32_t qos_highest_sequence;
    uint32_t qos_received_packets; int have_qos_snapshot;

    /* ---- round XL: the reception-timing jitter block ----
     * publish_stream_packet's timing fields (nvst.rs:917-940), SEPARATE
     * from the bandwidth estimator's ms-domain jitter on purpose: this
     * is the RFC 3550 interarrival jitter in 90 kHz TICKS that the RTCP
     * Receiver Report carries (report_snapshot, :1251-1261). The first
     * authenticated packet anchors first_arrival/first_rtp and returns
     * without a jitter step; a packet whose RTP timestamp equals the
     * previous one is skipped; otherwise
     *   transit  = (now - first_arrival)*90000 - (u32)(ts - first_ts)
     *   delta    = |transit - last_transit|
     *   jitter  += (delta - jitter) / 16
     * exactly as the reference (u32 wrap, NOT sign extension, on the
     * timestamp difference). */
    uint64_t timing_first_ms;    int have_timing_first;
    uint32_t timing_first_rtp;
    uint32_t timing_last_rtp;
    int64_t  timing_last_transit;
    double   timing_jitter;

    /* Round XL: the wall-clock ms of the first packet of the AU the
     * assembler is currently accumulating (frame_ack's first_packet_time
     * input; recorded at the assembler's frame-start boundary). */
    uint64_t cur_au_first_ms;
} kn_rx_video;

/* Initialise the pipeline. Validates every bound (window 1..2048, AU cap
 * <= 16 MiB, packet size 256..65519, shard table >= 2 and <= 255, scratch
 * >= kn_fec_scratch_bytes(fec_max_shards/2, video_packet_size+16), pool cap
 * >= shard_len, events >= 4). KN_RX_OK / KN_RX_ERR_ARGS / KN_RX_ERR_STATE.
 * The SRTP session is set separately (kn_rx_video_set_srtp) — a plaintext
 * pipeline (profile NONE) is fully functional for replay/capture testing. */
int kn_rx_video_init(kn_rx_video *v, const kn_rx_video_config *cfg,
                     const kn_rx_mem *mem, uint64_t now_ms);
void kn_rx_video_set_srtp(kn_rx_video *v, const kn_rx_srtp_session *session);

/* Process ONE received UDP datagram (process_datagram, nvst.rs:4617-4726).
 * The datagram buffer is MODIFIED in place when SRTP decrypts (it is the
 * receive-loop scratch). Frames arrive via cb (may be NULL to discard).
 * Events stage into v's event array — drain with kn_rx_video_events()
 * before the next call. Returns KN_RX_OK, or a negative error for argument
 * problems (per-datagram verdicts are events, never errors). */
int kn_rx_video_process_datagram(kn_rx_video *v, uint8_t *datagram, size_t len,
                                 uint64_t now_ms, kn_rx_frame_fn cb, void *ud);

/* Process one ALREADY-AUTHENTICATED plaintext RTP packet with a known
 * extended index (the entry point below SRTP — replay/capture modes and
 * tests). Same consume path as process_datagram. */
int kn_rx_video_process_packet(kn_rx_video *v, uint64_t index,
                               const uint8_t *plaintext, size_t len,
                               uint64_t now_ms, kn_rx_frame_fn cb, void *ud);

/* Periodic poll (poll_recovery + poll_timeout + poll_frame_progress +
 * the RFI backstop, one call for the receive loop): drains reorder/FEC
 * deadlines, checks the video timeout and the produced-frame stall.
 * Returns KN_RX_OK; inspect staged events. When the timeout fires the
 * state becomes RECOVERY_REQUIRED (call kn_rx_video_recover to resume). */
int kn_rx_video_poll(kn_rx_video *v, uint64_t now_ms, kn_rx_frame_fn cb,
                     void *ud);

/* Lifecycle (nvst.rs:4486-4522): each returns 1 when a transition happened
 * (a LIFECYCLE event is staged), 0 when the current state made it a no-op. */
int kn_rx_video_pause(kn_rx_video *v, uint64_t now_ms);
int kn_rx_video_resume(kn_rx_video *v, uint64_t now_ms);
int kn_rx_video_recover(kn_rx_video *v, uint64_t now_ms);
int kn_rx_video_stop(kn_rx_video *v, uint64_t now_ms);

/* Staged events of the last call: returns the array and sets *n. Valid
 * until the next process/poll/lifecycle call. ev_overflow counts events
 * that did not fit (aggregate drop_counters remain exact). */
const kn_rx_event *kn_rx_video_events(const kn_rx_video *v, uint32_t *n);

/* The keyframe-request flag: take-and-clear (the control sender calls this
 * when it emits IDR 0x0302 / RTCP PLI). */
int kn_rx_video_take_keyframe_request(kn_rx_video *v);

/* Round XL: the NON-clearing peek the reference's retry loop actually has
 * — keyframe_request_pending (nvst.rs:1266-1268). The latch stays armed
 * until an assembled KEYFRAME clears it (:994) and the bundle loop retries
 * IDR+PLI every IDR_RETRY_INTERVAL (100 ms, :104/:1279-1281) while it is
 * set. A one-shot take-and-clear on a lossy channel is how run 20 ended up
 * "pending a keyframe that never comes". NULL-safe. */
int kn_rx_video_keyframe_request_pending(const kn_rx_video *v);

/* The reference's report_snapshot inputs (nvst.rs:1223-1261): returns
 * 1 when the stream is bound (all fields filled: the media SSRC, the
 * FIRST extended sequence seen, the highest extended sequence, the
 * authenticated packet count, and the RFC 3550 interarrival jitter in
 * 90 kHz ticks, clamped to 0..UINT32_MAX exactly like the reference's
 * `jitter.clamp(0.0, f64::from(u32::MAX)) as u32`), 0 before the first
 * packet. The caller derives expected = highest - base + 1, cumulative =
 * expected - received, and the 256-scaled fraction over its own prior
 * snapshot (the reference's update_interval arithmetic). NULL-safe. */
int kn_rx_video_rr_snapshot(const kn_rx_video *v, uint32_t *ssrc,
                            uint32_t *base, uint32_t *highest,
                            uint64_t *received, uint32_t *jitter);

/* ---- round XXXVIII (D-t10): the bandwidth-estimator plane ---- */

/* The QoS packet snapshot the 0x0207 report carries
 * (QosPacketSnapshot, nvst.rs:900-914): the media ssrc, the base
 * sequence latched on the FIRST packet of that ssrc, the highest
 * sequence seen, and the received-packet count (saturating at u32::MAX
 * exactly like the reference's `previous.received < u32::MAX` guard).
 * Returns 1 once a packet has been published, 0 before — the reference's
 * Option. The 0x0207 loss field is a DELTA between two of these, so the
 * caller keeps the previous one (nvst.rs:1147-1166). NULL-safe. */
int kn_rx_video_qos_snapshot(const kn_rx_video *v, uint32_t *ssrc,
                             uint32_t *base, uint32_t *highest,
                             uint32_t *received);

/* BandwidthEstimator::report(now) (nvst_bandwidth.rs:210-243) for the
 * seven 0x0207 fields. `now_ns` must come from the SAME clock domain as
 * cfg.now_ns; passing 0 asks the pipeline to read cfg.now_ns itself (the
 * production convenience path). Every field is already clamped/masked to
 * its wire type, and all of them are ZERO before the first packet — plus
 * utilization_percent/estimate_kbps stay zero until 100 completed frames,
 * which is the reference's gate, not an omission. Returns 1 on success. */
int kn_rx_video_bw_report(const kn_rx_video *v, int64_t now_ns,
                          kn_bw_feedback *out);

/* Evidence for the log: how many samples reached the estimator, how many
 * parity notes, and how many authenticated packets were withheld from it
 * (reordered / retransmitted / no parseable GS video tuple). The reference
 * gates the feed on `!retransmitted && !reordered` (nvst.rs:4694-4702), so
 * a session that never feeds anything is a defect these counters name. */
void kn_rx_video_bw_counters(const kn_rx_video *v, uint64_t *samples,
                             uint64_t *parity, uint64_t *skipped_reordered,
                             uint64_t *skipped_retransmitted,
                             uint64_t *skipped_no_video);

/* Number of completed frame trains the estimator has consumed — the gate
 * counter behind utilization_percent/estimate_kbps
 * (nvst_bandwidth.rs:238-241). 0 before the first completed frame. */
uint32_t kn_rx_video_bw_frames_observed(const kn_rx_video *v);

/* Drop the estimator and its ssrc latch (the reference resets it on an
 * ssrc change; this is the same reset for a deliberate stream switch). */
void kn_rx_video_bw_reset(kn_rx_video *v);

/* SET the keyframe-request flag from outside the pipeline (stage-2 decode
 * glue: encoded-queue overflow => the decoder never saw a frame later
 * packets reference => request an IDR; analiza/04 §5 queue-overflow row).
 * Same flag/counter discipline as the internal loss paths; consumed by
 * kn_rx_video_take_keyframe_request. NULL-safe no-op. */
void kn_rx_video_request_keyframe(kn_rx_video *v);

/* Reset media state only (reset_media_state, nvst.rs:4946-4961) — the
 * lifecycle calls use it; exposed for capture-mode stream switches. */
void kn_rx_video_reset_media(kn_rx_video *v, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* KN_RX_H */
