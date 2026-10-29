/* kn_arx.h — NVST bundle-audio receive/extract (stage-3 audio, F11).
 *
 * The rig's game audio rides the ICE/DTLS bundle socket as SRTP RTP
 * next to the SCTP control traffic (rtcAudioOnNativeBundle:1; the
 * reference admits it via NvstAudioStreams::admit, nvst.rs:6349-6400,
 * str0m decrypts with the DTLS-SRTP keying material). kn_rx owns the
 * VIDEO path on the NATT media socket; this module owns the AUDIO path
 * on the bundle socket:
 *
 *   SRTP unprotect   RFC 3711 AES_CM_128_HMAC_SHA1_80 — the profile the
 *                    DTLS use_srtp handshake negotiates (kn_dtls offers
 *                    0x0001, round XXXV; srtp_prof:1 in every run since).
 *                    Master key/salt come from the RFC 5764 §4.2
 *                    EXTRACTOR-dtls_srtp output (kn_dtls_srtp_export);
 *                    session keys via kn_srtp_kdf128 (labels 0/1/2).
 *                    Replay window + ROC estimation: kn_srtp_replay.
 *   admission        matches_audio_track (nvst.rs:6358-6361): PT 111
 *                    (GFN_OPUS_PAYLOAD_TYPE) or PT 63 (GFN_RED_PAYLOAD_
 *                    TYPE) — the reference hardcodes the track as
 *                    {payloadType:111, codec:opus, clockRateHz:48000,
 *                    channels:2, mid:"0", ssrc:None} (nvst_rtsp.rs:1309),
 *                    so ANY SSRC with those PTs is admitted; the first
 *                    SSRC locks an epoch, a change resets the replay
 *                    window and the seq tracking (per-stream contexts,
 *                    like str0m — deviation D-a2).
 *   RED de-framing   RFC 2198: 4-octet F=1 headers for the redundant
 *                    blocks, a 1-octet F=0 header for the primary (the
 *                    LAST block); block lengths must tile the payload
 *                    body exactly or the packet is red_invalid.
 *   emission         decrypted Opus payloads (RED primaries) go to the
 *                    caller's emit callback in ARRIVAL order (deviation
 *                    D-a1: str0m reorders through a jitter buffer; the
 *                    extraction target is a file, so reorder/loss/dup
 *                    are counted instead of repaired — the counters
 *                    make the difference visible).
 *
 * Processing order + counter semantics: THE SPEC lives in
 * tools/arx_reference.py (the independent Python implementation of this
 * contract — two-implementations rule; KAT vectors in
 * kn_arx_kat_vectors.h). Zero allocation, deterministic time (now_ms is
 * a parameter), honest stats.
 *
 * NOT in scope (later rounds): Opus decode to PCM (XLIX candidate),
 * SRTCP on the bundle (counted by the pump, never parsed), the
 * microphone direction (send), playout timing/jitter.
 */
#ifndef KN_ARX_H
#define KN_ARX_H

#include <stddef.h>
#include <stdint.h>

#include "kn/knsrtp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KN_ARX_PT_OPUS      111u   /* GFN_OPUS_PAYLOAD_TYPE, nvst.rs:276 */
#define KN_ARX_PT_RED       63u    /* GFN_RED_PAYLOAD_TYPE,  nvst.rs:275 */
#define KN_ARX_CHANNELS     2u     /* nvst_rtsp.rs:1309 (hardcoded track) */
#define KN_ARX_CLOCK_HZ     48000u
#define KN_ARX_TAG_LEN      10u    /* HMAC_SHA1_80 */
#define KN_ARX_MAX_PKT      1600u  /* bundle MTU ceiling; larger = malformed */

typedef struct kn_arx_stats {
    unsigned long long consumed;        /* datagrams handed to process */
    unsigned long long packets;         /* emitted Opus payloads */
    unsigned long long payload_bytes;   /* emitted bytes */
    unsigned long long red_packets, red_blocks, red_invalid;
    unsigned long long pt_filtered;     /* RTP with a non-audio PT */
    unsigned long long malformed;       /* steps 2/4/5/11 rejections */
    unsigned long long toc_invalid;     /* decrypted but TOC malformed */
    unsigned long long padded;          /* P-bit packets (pad stripped) */
    unsigned long long seq_loss, seq_reorder, seq_dup;
    unsigned long long ssrc_changes;
    unsigned long long srtp_fail;       /* authentication failures */
    unsigned long long replay_reject;   /* replay-window rejections */
    unsigned long long srtp_nokey;      /* seen before kn_arx_set_srtp */
    uint64_t first_ms, last_ms;         /* emit timestamps (now_ms arg) */
} kn_arx_stats;

/* Emitted payload — the callback must consume or copy (the buffer is
 * the module scratch, reused by the next process call). */
typedef void (*kn_arx_emit_fn)(void *ctx, const uint8_t *payload,
                               size_t len, uint32_t samples48);

typedef struct kn_arx {
    kn_arx_emit_fn emit;
    void          *ectx;

    /* SRTP session (armed by kn_arx_set_srtp) */
    int      armed;
    uint8_t  enc_key[16], auth_key[20], sess_salt[14];
    kn_srtp_replay replay;

    /* SSRC epoch + seq accounting */
    int      have_ssrc;
    uint32_t ssrc;
    int      have_seq;
    uint16_t highest_seq;

    /* wire scratch (unprotect is in-place) */
    uint8_t  pkt[KN_ARX_MAX_PKT];

    kn_arx_stats st;
} kn_arx;

void kn_arx_init(kn_arx *a, kn_arx_emit_fn emit, void *ectx);

/* Derive the session keys from the DTLS-SRTP master pair (RFC 3711
 * KDF labels 0/1/2 via kn_srtp_kdf128) and arm the replay window.
 * Returns 0, or -1 on invalid arguments / KDF failure. */
int kn_arx_set_srtp(kn_arx *a, const uint8_t master_key[16],
                    const uint8_t master_salt[14]);

/* One bundle-socket datagram. Returns:
 *   1  an Opus payload was emitted,
 *   0  not audio for us (pt_filtered — e.g. a stray video/RTCP PT),
 *  -1  rejected (malformed / no key / replay / auth / red_invalid /
 *      toc_invalid — the counters say which).
 * now_ms feeds stats.first_ms/last_ms (any monotonic origin). */
int kn_arx_process(kn_arx *a, const uint8_t *datagram, size_t len,
                   uint64_t now_ms);

/* The derived session keys (KAT cross-check; NULL-tolerant). */
void kn_arx_session_keys(const kn_arx *a, uint8_t enc[16],
                         uint8_t auth[20], uint8_t salt[14]);

#ifdef __cplusplus
}
#endif
#endif /* KN_ARX_H */
