/* kn_h264rtp.h — RFC 6184 H.264 RTP depacketizer / access-unit assembler
 * (slice 5n, B5a: the WebRTC video half).
 *
 * WHY THIS MODULE EXISTS. The native path assembles AUs from NVST's own
 * RTP framing (kn_rx: SOF/start-code/AU-length fields — a proprietary
 * layout). The WebRTC path carries STANDARD RFC 6184 payloads, which the
 * offer names explicitly (a=rtpmap:96 H264/90000 + packetization-mode
 * semantics; runB5b/runB5c measured PT 96 flowing at ~450 pkt/s once the
 * 5l feedback plane opened). Nothing in the tree spoke 6184 — this module
 * is that translator: RTP packets in, Annex-B access units out, ready for
 * kn_decode_stream_push (the same decoder the native path feeds).
 *
 * WIRE CONTRACT (RFC 6184 §5):
 *   payload[0] = the NAL-unit header octet: F|NRI|Type. F MUST be 0
 *   (a set F bit marks the packet malformed — the AU is dropped).
 *   Type 1..23   Single NAL unit packet  (§5.2): the payload IS one NAL.
 *   Type 24      STAP-A (§5.7.1): [2-byte BE length][NAL] repeated —
 *                the keyframe opener (SPS+PPS+first IDR slice in one
 *                packet is the libwebrtc convention).
 *   Type 28      FU-A (§5.8): payload[1] = S|E|R|Type. S reconstructs the
 *                NAL header ((fu_indicator & 0xE0) | (fu_hdr & 0x1F));
 *                fragments concatenate; E closes the NAL.
 *   Types 25..27, 29..31 (FU-B/STAP-B/MTAP/RSVD) are NOT emitted by
 *   libwebrtc senders with packetization-mode 1 — counted and the AU
 *   marked broken (honest drop, no guessing).
 *
 * AU BOUNDARIES: the RTP marker bit marks the last packet of an AU
 * (§5.1); a TIMESTAMP CHANGE also closes the previous AU (defensive —
 * a lost marker must not glue two frames together; counted ts_split).
 *
 * SEQUENCE DISCIPLINE (the caller feeds packets in arrival order):
 *   - the first packet primes the expectation;
 *   - seq < expected (int16 distance) = duplicate/late → skipped, the AU
 *     in progress is untouched (RTX-retransmit semantics without RTX:
 *     a dup carries nothing new);
 *   - seq > expected = a gap → the partial AU is DROPPED (a NAL with a
 *     hole is decoder poison; the drop is counted, the stream resyncs on
 *     the next AU) and the packet is accepted as the new expectation;
 *   - wraparound is handled by the int16 distance (65535 → 0 is +1).
 *
 * OUTPUT: Annex-B with 4-byte start codes (00 00 00 01) before every
 * NAL — exactly what kn_decode's av_parser split expects (the native
 * path's AUs are the same shape). The AU buffer is CALLER-PROVIDED
 * (zero-allocation module rule, same as kn_twcc); an AU that would
 * overflow it is dropped whole (drop_over) — never emitted truncated.
 *
 * PURITY: no clock, no sockets, no stdio, no allocation. The callback
 * aliases the assembly buffer (valid only during the call — consume or
 * copy, the kn_rx frame-callback contract verbatim). A non-zero callback
 * return propagates out of push (sticky abort is the CALLER's policy —
 * the module just stops being fed).
 *
 * GROUND TRUTH: RFC 6184 §5.2/§5.7/§5.8; libwebrtc's H264 packetizer
 * (single/STAP-A/FU-A only, keyframes open with STAP-A[SPS,PPS]);
 * str0m's depacketizer (rtp/h264.rs) for the same three shapes. The KAT
 * vectors come from tools/h264rtp_reference.py — an independent Python
 * implementation with a structural self-check (every emitted AU is
 * re-parsed and its NAL list compared byte-for-byte against the
 * scenario's construction table).
 */
#ifndef KN_H264RTP_H
#define KN_H264RTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kn_h264rtp_stats {
    /* intake */
    unsigned long long pkts_in;      /* packets pushed */
    unsigned long long pkt_dups;     /* seq behind expectation: skipped */
    unsigned long long pkt_gaps;     /* seq jumps: partial AUs dropped */
    /* payload classes */
    unsigned long long single_nal;   /* type 1..23 packets */
    unsigned long long stap_a_pkts;  /* type 24 packets */
    unsigned long long stap_a_nals;  /* NALs extracted from STAP-As */
    unsigned long long fu_a_pkts;    /* type 28 packets (all fragments) */
    unsigned long long fu_incomplete;/* FUs open at emit/preempt: AU drop */
    unsigned long long malformed;    /* F-bit, truncations, orphan frags */
    unsigned long long other_type;   /* 25..27/29..31: not depacketized */
    /* output */
    unsigned long long aus;          /* AUs delivered to the callback */
    unsigned long long au_bytes;     /* total Annex-B bytes delivered */
    unsigned long long au_max;       /* largest AU delivered */
    unsigned long long ts_split;     /* AUs closed by a ts change, not a
                                      * marker (0 on a healthy stream) */
    /* drops */
    unsigned long long drop_gap;     /* partial AUs killed by a seq gap */
    unsigned long long drop_broken;  /* malformed/other-type tainted AUs */
    unsigned long long drop_over;    /* AUs that overflowed the buffer */
} kn_h264rtp_stats;

typedef struct kn_h264rtp {
    uint8_t *buf;                    /* caller-owned AU assembly buffer */
    size_t   cap;
    size_t   len;                    /* bytes currently assembled */
    uint32_t cur_ts;                 /* ts of the AU in progress */
    int      have_ts;
    uint16_t next_seq;               /* expected sequence number */
    int      have_seq;
    int      au_broken;              /* tainted: drop at emit */
    int      au_over;                /* overflowed: drop at emit */
    int      in_fu;                  /* a FU-A fragment run is open */
    /* first malformed packet's identity — the probe prints it once so a
     * format surprise wears a face on the FIRST flight (the 5k lesson:
     * observability before the next round-trip). */
    int      bad_logged;
    unsigned bad_type;               /* NAL type of that packet */
    uint16_t bad_seq;
    uint8_t  bad_head[16];
    unsigned bad_head_len;
    kn_h264rtp_stats st;
} kn_h264rtp;

/* AU callback: annexb aliases the assembly buffer (valid ONLY during the
 * call). rtp_ts = the AU's 90 kHz timestamp. Return 0 to continue; any
 * other value aborts (push propagates it; further pushes are the
 * caller's decision). */
typedef int (*kn_h264rtp_au_fn)(const uint8_t *annexb, size_t len,
                                uint32_t rtp_ts, void *ud);

void kn_h264rtp_init(kn_h264rtp *d, uint8_t *buf, size_t cap);

/* Feed ONE video RTP packet: seq/ts from the RTP header, marker = the
 * M bit, payload = the packet's payload region (padding already
 * excluded — kn_rx's payload validation runs before this). Returns 0 on
 * success, the callback's non-zero code on abort, or -1 on bad args. */
int kn_h264rtp_push(kn_h264rtp *d, uint16_t seq, uint32_t ts, int marker,
                    const uint8_t *payload, size_t plen,
                    kn_h264rtp_au_fn cb, void *ud);

#ifdef __cplusplus
}
#endif
#endif /* KN_H264RTP_H */
