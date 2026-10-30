/* kn_opus.h — decode-only Opus wrapper (round N slice 3, F21).
 *
 * WHY THIS MODULE EXISTS: F21 plays the bundle-audio pipeline LIVE
 * (kn_arx emit -> Opus decode -> WASAPI). The ONLY Opus knowledge in the
 * project must live behind this four-function surface; everything else
 * (knvstd_main, kn_audio) sees interleaved float32 PCM and never an
 * opus_types.h symbol. When the build has no libopus (POSIX dry builds
 * without the dev package, cross-compile legs, rigs before the one-time
 * provisioning) the same API compiles as an honest stub: available()==0,
 * create()==NULL with a reason string, decode()==-1. The session then
 * stays file-only and says so on its audio_play_begin line — playback
 * absence is NEVER silent and NEVER fatal (the kn_audio_open failure
 * policy, docs/AUDIO.md).
 *
 * The decoder targets the GFN rig's measured format (BUNDLE-AUDIO §10.1):
 * CELT fullband, 5 ms packets (TOC 0xEC), 48 kHz, stereo — but nothing
 * here hardcodes it: the wrapper decodes whatever RFC 6716 packet it is
 * given at the rate/channels the caller created it with, and returns the
 * decoded frame count per channel so the caller sizes its buffers from
 * the return value, not from an assumption.
 *
 * Two-implementations rule: the wrapper contract (frame counts, the
 * stereo interleave, the s16 quantisation used by the vectors, the error
 * paths) is pinned by tools/opus_reference.py, which encodes AND decodes
 * the vectors through an INDEPENDENT path (libopus via Python ctypes)
 * before emitting streamer/src/kn_opus_kat_vectors.h for test_knopus. The
 * codec itself is libopus on both sides by design — what the vectors pin
 * is OUR call sequence, buffer discipline and conversions.
 */
#ifndef KN_OPUS_H
#define KN_OPUS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kn_opus_dec kn_opus_dec;      /* opaque */

/* 1 when this binary links a real libopus decoder, else 0. Callers gate
 * their whole playback path on this (and still handle create failure). */
int kn_opus_available(void);

/* libopus version string ("libopus 1.5.2"-class, borrowed pointer, never
 * NULL — the stub returns "none"). Logged on audio_play_begin. */
const char *kn_opus_version(void);

/* Create a decoder for `rate` Hz / `channels` (1 or 2). On failure
 * returns NULL and writes a human-readable reason into errbuf (which may
 * be NULL/0 when the caller does not care). */
kn_opus_dec *kn_opus_decoder_create(unsigned rate, unsigned channels,
                                    char *errbuf, size_t errsz);

/* Decode ONE RFC 6716 payload into interleaved float32 (`out`, capacity
 * `cap_frames` frames of `channels` channels each). Returns the decoded
 * frame count per channel (>0), or -1 on bad args / decoder error /
 * capacity too small for the packet's duration. len==0/NULL payload is
 * rejected with -1 (no PLC in this contract: a lost 5 ms GFN packet is
 * the arx seq_loss counter's business, and silence-padding there would
 * hide loss from the playback stats). */
int kn_opus_decode_f32(kn_opus_dec *d, const unsigned char *pkt, size_t len,
                       float *out, unsigned cap_frames);

/* NULL-safe. */
void kn_opus_decoder_destroy(kn_opus_dec *d);

#ifdef __cplusplus
}
#endif

#endif /* KN_OPUS_H */
