/* kn_ogg.h — Ogg page container + OggOpus muxing (RFC 3533 / RFC 7845)
 * and the RFC 6716 Opus TOC duration parser.
 *
 * Round XLVIII (F11, stage-3 audio): the bundle-audio extractor (kn_arx)
 * emits decrypted Opus packets; this module frames them into a PLAYABLE
 * .opus file (OggOpus) so the operator can verify a capture with the
 * tools already on the target machine (mpv/ffprobe) — no decode code in
 * the client yet (Opus decode is the XLIX candidate; extraction first).
 *
 * Contract (docs/AUDIO.md; KAT'd byte-exact against the independent
 * Python implementation tools/oggopus_reference.py → kn_ogg_kat_vectors.h):
 *   - kn_ogg_crc32: RFC 3533 §6 — MSB-first CRC-32, poly 0x04c11db7,
 *     initial value 0, no reflection, no final xor; the CRC field is
 *     zeroed during computation.
 *   - kn_opus_packet_samples48k: RFC 6716 §3.1/§3.2.5 — total 48 kHz
 *     samples carried by one Opus packet (TOC config 0..31 × frame-count
 *     code 0..3; code 3 carries M in byte[1] bits 0..5, M in 1..48).
 *     Returns -1 for malformed packets (empty; code 3 with len < 2 or
 *     M outside 1..48).
 *   - kn_ogg_mux_*: the page muxer. ZERO ALLOCATION — the caller owns
 *     the page scratch (KN_OGG_PAGE_BUF bytes) and receives finished
 *     pages through a write callback. Deterministic: a fixed serial and
 *     the same packet script produce byte-identical streams.
 *
 * Documented deviations (docs/AUDIO.md §Deviations):
 *   D-o1  Pages flush when the next packet would exceed 255 lacing
 *         segments, KN_OGG_PAGE_PAYLOAD_MAX (16384) payload bytes, or
 *         KN_OGG_PACKETS_PER_PAGE (48) packets. The spec allows up to
 *         65025 bytes/page; smaller pages are fully compliant and keep
 *         the scratch bounded. A packet too large for an EMPTY page
 *         (segs > 255 or len > payload max) is dropped honestly
 *         (dropped_packets++) — real Opus packets never exceed 1275×5
 *         bytes, so this cannot fire on a live stream.
 *   D-o2  No packet ever spans pages (no continuation segments): the
 *         muxer flushes BEFORE appending an oversized fit. granulepos
 *         is therefore always the exact cumulative 48 kHz sample count
 *         at the page end.
 *   D-o3  OpusHead pre-skip is 0 — the rig's encoder pre-skip is not
 *         signalled anywhere on the wire; players tolerate 0 (the first
 *         frames may carry a few ms of encoder delay).
 */
#ifndef KN_OGG_H
#define KN_OGG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_OGG_PAGE_PAYLOAD_MAX   16384u   /* D-o1 */
#define KN_OGG_PACKETS_PER_PAGE   48u      /* D-o1 */
#define KN_OGG_MAX_LACING         255u
/* 27-byte header + 255 lacing + payload cap (D-o1) */
#define KN_OGG_PAGE_BUF           (27u + 255u + KN_OGG_PAGE_PAYLOAD_MAX)

/* RFC 3533 §6 Ogg CRC-32 (poly 0x04c11db7, init 0, no reflect/xor). */
uint32_t kn_ogg_crc32(const void *data, size_t len);

/* RFC 6716 §3.1: total 48 kHz samples in one Opus packet; -1 malformed. */
int kn_opus_packet_samples48k(const uint8_t *pkt, size_t len);

/* Page sink: 0 = ok, anything else = write error (latched into stats). */
typedef int (*kn_ogg_write_fn)(void *ctx, const uint8_t *data, size_t len);

typedef struct kn_ogg_mux {
    /* config (init) */
    uint8_t        *buf;              /* caller-owned page scratch */
    size_t          bufsz;            /* >= KN_OGG_PAGE_BUF */
    uint32_t        serial;
    unsigned        channels;         /* OpusHead channel count */
    kn_ogg_write_fn write;
    void           *wctx;

    /* stream state */
    uint32_t        page_seq;
    int64_t         granule;          /* cumulative 48 kHz samples */
    size_t          seg_off;          /* payload bytes in the open page */
    unsigned        nsegs;            /* lacing segments in the open page */
    unsigned        npkts;            /* packets in the open page */
    uint8_t         segs[KN_OGG_MAX_LACING];
    int             began;            /* begin() emitted head+tags */
    int             finished;         /* finish() ran (EOS emitted) */

    /* honest stats */
    unsigned long long pages_out, bytes_out, write_errors, dropped_packets;
} kn_ogg_mux;

/* Wire the muxer to a caller-owned scratch + sink. Returns 0, or -1 on
 * invalid arguments. Emits nothing (begin() writes the header pages). */
int kn_ogg_mux_init(kn_ogg_mux *m, uint8_t *buf, size_t bufsz,
                    uint32_t serial, unsigned channels,
                    kn_ogg_write_fn write, void *wctx);

/* Emit the OpusHead (BOS) and OpusTags pages, both at granulepos 0
 * (RFC 7845 §3/§5). Returns 0, or -1 (not init'd / write error). */
int kn_ogg_mux_begin(kn_ogg_mux *m);

/* Append one Opus packet (samples48 = its duration from
 * kn_opus_packet_samples48k). Flushes the open page first when the
 * packet would not fit (D-o1/D-o2). Returns 0, or -1 (state/write
 * error; an oversized packet is counted as dropped_packets and
 * returns 0 — honest skip, not a stream abort). */
int kn_ogg_mux_packet(kn_ogg_mux *m, const uint8_t *payload, size_t len,
                      uint32_t samples48);

/* Flush the final page with the EOS header bit (an empty EOS page is
 * emitted when the last packet exactly filled its page). Idempotent.
 * Returns 0, or -1 on write error. */
int kn_ogg_mux_finish(kn_ogg_mux *m);

#ifdef __cplusplus
}
#endif
#endif /* KN_OGG_H */
