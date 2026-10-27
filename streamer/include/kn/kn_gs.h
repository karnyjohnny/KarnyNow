/* kn_gs.h — NVST RTP/GS parse layer (Mjolnir video, stage-2 module v1).
 *
 * Scope v1 parses wire formats: RTP fixed header + CSRC + extension, GS
 * `0x4753` body, Annex-B payload location, keyframe detection, AUD stripping,
 * FEC layout, stream continuity, and kn_ring_dump lines. Stage 2 now adds a
 * caller-buffered AU assembler (GsFrameHeader/expected length, nvst.rs:3270+).
 * RTP reorder buffering and PLI/NACK feedback remain separate Stage 2 work.
 *
 * Provenance: line-by-line port of nvst.rs @ a49b124 — see docs/GS.md for
 * the full fact/line table. Known-answer vectors (tools/gs_reference.py →
 * streamer/src/kn_gs_kat_vectors.h) pin every behaviour; the gs.parse
 * self-test re-runs them from the shipped binary (§7.9).
 *
 * Pitfalls this API encodes (BOOTSTRAP §8):
 *   #1 the GS body is LITTLE-endian while RTP is big-endian;
 *   #2 fec_current_block/fec_last_block live in ext[11] (top byte of the
 *      flags word), not in a separate field;
 *   #4 reconstructed FEC shards carry transport metadata that legitimately
 *      differs — the fec_word is NOT re-validated on recovered packets
 *      (nvst.rs:4103-4106); consumers must not reject on it.
 *
 * Parsing and AU assembly use caller-owned memory, perform no heap allocation,
 * and are safe from any thread when each caller owns its assembler state.
 */
#ifndef KN_GS_H
#define KN_GS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wire constants (nvst.rs:79, 212, 236-237, 263-274) */
#define KN_GS_FIXED_HEADER_LEN   12
#define KN_GS_EXT_LEN            16
#define KN_GS_PROFILE            0x4753u
#define KN_GS_START_SEARCH       68   /* MAX_GS_FRAME_HEADER_BYTES(64) + 4 */
#define KN_GS_INDEX_MASK         0x00FFFFFFu
#define KN_GS_FLAG_PIC           0x01 /* FLAG_CONTAINS_PIC_DATA */
#define KN_GS_FLAG_EOF           0x02
#define KN_GS_FLAG_SOF           0x04
#define KN_GS_AV1_SHORT_HEADER   8    /* payload[0] == 0x01 */
#define KN_GS_AV1_GFN_HEADER     20   /* payload[0] == 0x81 */
#define KN_GS_DEFAULT_MAX_ACCESS_UNIT (2u * 1024u * 1024u)
#define KN_GS_MAX_ACCESS_UNIT    (16u * 1024u * 1024u)  /* nvst.rs:237 */

/* Return codes (mirror RtpParseError + local additions; precedence of the
 * RTP parser matches the reference exactly) */
enum {
    KN_GS_OK               =   0,
    KN_GS_ERR_TOO_SHORT    =  -1,
    KN_GS_ERR_VERSION      =  -2,
    KN_GS_ERR_CSRC         =  -3,
    KN_GS_ERR_EXT          =  -4,   /* truncated ext, or GS profile with
                                        words*4 != 16 (nvst.rs:2490) */
    KN_GS_ERR_PADDING      =  -5,   /* padding byte 0 or > payload region */
    KN_GS_ERR_MISSING_VIDEO=  -6,   /* no GS ext where video is required */
    KN_GS_ERR_NO_PICTURE   =  -7,   /* start code not found / AV1 marker */
    KN_GS_ERR_HEX          =  -8,   /* malformed kn_ring_dump line */
    KN_GS_ERR_HEX_CAP      =  -9,   /* record larger than the caller buffer */
    KN_GS_ERR_FEC_LAYOUT   = -10,   /* FecPacketLayout::from_packet -> None */
    KN_GS_ERR_ARGS         = -11,
    KN_GS_ERR_AU_STATE     = -12,
    KN_GS_ERR_AU_AWAITING_SOF = -13,
    KN_GS_ERR_AU_DISCONTINUITY = -14,
    KN_GS_ERR_AU_TOO_LARGE = -15,
    KN_GS_ERR_AU_LENGTH    = -16,
    KN_GS_ERR_AU_FEC       = -17
};

typedef enum kn_gs_codec {
    KN_GS_CODEC_H264 = 0,
    KN_GS_CODEC_H265 = 1,
    KN_GS_CODEC_AV1  = 2
} kn_gs_codec;

/* Parsed RTP header INCLUDING payload bounds (padding stripped) — the
 * combination of RtpHeader::parse (nvst.rs:2456-2510) and payload()
 * (2512-2525). A foreign (non-0x4753) extension is skipped and recorded as
 * has_gs=0; MissingNvVideoHeader is then the CONSUMER's verdict when it
 * needs video metadata (the RTP parse itself succeeds). */
typedef struct kn_gs_rtp {
    unsigned pt;               /* payload_type (7 bits) */
    unsigned seq;              /* sequence_number (BE16) */
    uint32_t timestamp;        /* BE32 */
    uint32_t ssrc;             /* BE32 */
    unsigned payload_off;      /* first payload byte (after hdr+csrc+ext) */
    unsigned payload_len;      /* padding EXCLUDED */
    int      has_padding;
    int      has_gs;           /* GS 0x4753 extension present and strict-OK */
    unsigned gs_off;           /* offset of the 16-byte GS body in the packet */
} kn_gs_rtp;

int kn_gs_rtp_parse(const uint8_t *pkt, size_t len, kn_gs_rtp *out);

/* Split parsing for the SRTP receive path (kn_rx): RtpHeader::parse alone
 * (nvst.rs:2456-2510) — structure, CSRC, extension, flags, but NO padding
 * validation and payload_len left 0. For encrypted datagrams the padding
 * byte is ciphertext; the reference validates it only in payload() AFTER
 * decryption (nvst.rs:2695-2697). */
int kn_gs_rtp_parse_hdr(const uint8_t *pkt, size_t len, kn_gs_rtp *out);

/* payload() port (nvst.rs:2512-2525): bounds + padding validation against
 * the buffer pkt[0..total_len) (the DECRYPTED plaintext for SRTP). Returns
 * KN_GS_OK + payload bounds, KN_GS_ERR_TOO_SHORT or KN_GS_ERR_PADDING. */
int kn_gs_rtp_payload(const kn_gs_rtp *rtp, const uint8_t *pkt,
                      size_t total_len, size_t *off, size_t *len);

/* Parsed GS body — NvVideoPacket (nvst.rs:3212-3257). flags_valid reflects
 * the recovered-packet validation of nvst.rs:4089 (informational on the
 * normal path; the reference only enforces it on FEC-recovered packets). */
typedef struct kn_gs_video {
    uint32_t stream_packet_index;  /* (packet_word >> 8) & 0xFFFFFF */
    uint32_t frame_index;          /* LE32 ext[4..8] */
    unsigned flags;                /* flags_word & 0x0F */
    unsigned fec_current_block;    /* (ext[11] >> 4) & 3 — pitfall #2 */
    unsigned fec_last_block;       /* (ext[11] >> 6) & 3 */
    unsigned fec_percentage;       /* (fec_word >> 4) & 0xFF */
    unsigned fec_index;            /* (fec_word >> 12) & 0x3FF (shard) */
    unsigned fec_source_packets;   /* (fec_word >> 22) & 0x3FF */
    int is_fec;                    /* pct != 0 && index >= source (systematic) */
    int is_start_of_frame;         /* SOF && fec_current_block == 0 */
    int is_end_of_frame;           /* EOF && fec_current_block == last */
    int flags_valid;               /* flags & ~(SOF|EOF|PIC) == 0 */
} kn_gs_video;

int kn_gs_video_parse(const uint8_t ext16[KN_GS_EXT_LEN], kn_gs_video *out);

/* Annex-B start-code search (nvst.rs:3525-3534 — windows(3) semantics:
 * offsets 0..len-3; at each offset the 3-byte pattern is tested first, the
 * 4-byte variant needs b[off+3]==1 within the slice). Returns KN_GS_OK and
 * sets *off / *prefix_len (3 or 4), or KN_GS_ERR_NO_PICTURE. */
int kn_gs_find_start_code(const uint8_t *b, size_t len,
                          size_t *off, size_t *prefix_len);

/* Locate the encoded picture inside an RTP payload (nvst.rs:3481-3497):
 * H264/H265 — start code within the first min(len, 68) bytes; AV1 — fixed
 * 8/20-byte GFN cloud header by the first payload byte (0x01/0x81).
 * Returns KN_GS_OK + *off, or KN_GS_ERR_NO_PICTURE. */
int kn_gs_picture_payload(int codec, const uint8_t *pay, size_t len,
                          size_t *off);

/* Keyframe detection over a whole access unit (nvst.rs:3500-3523):
 * H264 nal_type 5 (IDR); H265 (hdr>>1)&0x3F in 16..=21; AV1 uses the
 * frame-header hint (no start codes exist). Returns 1/0. */
int kn_gs_is_keyframe(int codec, const uint8_t *b, size_t len,
                      int frame_header_hint);

/* Length after stripping a trailing access-unit delimiter (nvst.rs:3458-
 * 3479): H264 nal_type 9 / H265 type 35 as the LAST NAL -> truncate at its
 * start code. Returns the new length (== len when nothing to strip; AV1
 * never strips). Does not modify the buffer. */
size_t kn_gs_strip_trailing_aud(int codec, const uint8_t *b, size_t len);

/* FEC block coordinates from the GS body — FecPacketLayout::from_packet
 * (nvst.rs:3677-3701). parity = div_ceil(data*pct, 100); rejected when
 * data==0, pct==0, parity==0, shard >= data+parity, or total > 1023.
 * Returns KN_GS_OK or KN_GS_ERR_FEC_LAYOUT. Feeds knfec (docs/FEC.md). */
typedef struct kn_gs_fec_layout {
    uint32_t frame_index;
    unsigned block_index;       /* ext[11] >> 4 & 3 */
    unsigned last_block_index;  /* ext[11] >> 6 & 3 */
    unsigned shard_index;
    unsigned data_shards;
    unsigned parity_shards;
} kn_gs_fec_layout;

int kn_gs_fec_layout_parse(const uint8_t ext16[KN_GS_EXT_LEN], kn_gs_fec_layout *out);

/* stream_packet_index continuity for same-frame continuations
 * (nvst.rs:4813-4824): idx must equal (last + 1) mod 2^24. Returns 1/0. */
int kn_gs_index_continuous(uint32_t last, uint32_t idx);

/* ----------------------------------------------------- AU assembly (stage 2)
 *
 * Caller owns the storage buffer. A completed result aliases that buffer and
 * must be consumed before the next push. The default cap is 2 MiB; configured
 * caps may be smaller or larger, up to the 16 MiB protocol maximum.
 */
typedef enum kn_gs_frame_type {
    KN_GS_FRAME_UNKNOWN = 0,
    KN_GS_FRAME_P = 1,
    KN_GS_FRAME_I = 2,
    KN_GS_FRAME_HEADER = 3,
    KN_GS_FRAME_INTRA_REFRESH = 4,
    KN_GS_FRAME_REF_PIC_I = 5,
    KN_GS_FRAME_NON_REF_P = 6
} kn_gs_frame_type;

typedef struct kn_gs_au_assembler {
    int codec;
    uint8_t *buffer;
    size_t buffer_capacity;
    size_t max_access_unit;
    size_t bytes_len;
    uint32_t current_frame;
    uint32_t first_stream_packet_index;
    uint32_t last_stream_packet_index;
    uint32_t expected_access_unit_length;
    unsigned frame_type_wire;
    uint16_t last_packet_payload_length;
    int active;
    int frame_type_valid;      /* wire value in 1..=6 (a known NvstFrameType) */
    int frame_type_parsed;     /* GsFrameHeader matched at all — the
                                  Option::is_some() of the reference
                                  (Some(Unknown(x)) != None; RFI admit()
                                  needs exactly this distinction) */
    int has_expected_access_unit_length;
    int has_last_packet_payload_length;
} kn_gs_au_assembler;

typedef struct kn_gs_access_unit {
    int codec;
    uint32_t timestamp;          /* timestamp of the completing packet */
    uint32_t frame_index;
    uint32_t first_stream_packet_index;
    unsigned frame_type_wire;    /* NvstFrameType wire value; unknown preserved */
    int frame_type_valid;        /* wire value in 1..=6 */
    int frame_type_known;        /* frame header parsed (Some vs None) */
    int keyframe;
    int contiguous;
    int discarded_incomplete_frame;
    int request_keyframe;
    int length_hint_ignored;
    const uint8_t *bytes;        /* aliases assembler.buffer */
    size_t bytes_len;
} kn_gs_access_unit;

enum {
    KN_GS_AU_NEED_MORE = 0,
    KN_GS_AU_COMPLETE = 1
};

int kn_gs_au_assembler_init(kn_gs_au_assembler *assembler, int codec,
                            uint8_t *buffer, size_t buffer_capacity,
                            size_t max_access_unit);
void kn_gs_au_assembler_reset(kn_gs_au_assembler *assembler);
/* Feed authenticated, already-reordered packets. A negative return uses
 * KN_GS_ERR_AU_*; discontinuities set request_keyframe. A new SOF may report
 * discarded_incomplete_frame while beginning the next access unit. */
int kn_gs_au_assembler_push(kn_gs_au_assembler *assembler,
                            const kn_gs_video *video, uint32_t timestamp,
                            const uint8_t *payload, size_t payload_len,
                            kn_gs_access_unit *out);

/* One kn_ring_dump text line: "<mono_us> <meta_hex> <flags> <len> <hex...>"
 * (knring.c dump_visit). Strict format: single spaces, meta exactly 8 hex
 * digits, exactly `len` 2-digit hex byte tokens, trailing \r\n tolerated.
 * payload may be NULL when only the header fields are wanted; records
 * longer than cap return KN_GS_ERR_HEX_CAP (after format validation, so a
 * malformed line never reports CAP). *out_len receives the byte count. */
typedef struct kn_gs_dump_rec {
    uint64_t mono_us;
    uint32_t meta;
    uint32_t flags;
    uint32_t len;
} kn_gs_dump_rec;

int kn_gs_hexdump_parse_line(const char *line, kn_gs_dump_rec *rec,
                             uint8_t *payload, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* KN_GS_H */
