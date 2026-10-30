/* kn_opus.c — decode-only Opus wrapper (contract: kn/kn_opus.h, F21).
 *
 * Two bodies, one contract:
 *   KN_HAVE_OPUS  — the real libopus calls. opus_decode_float() returns
 *                   frames per channel or a negative OPUS_E* code; every
 *                   negative becomes -1 here so callers have ONE error
 *                   value. The capacity check runs BEFORE the call with
 *                   opus_packet_get_nb_frames()+duration math replaced by
 *                   the cheaper, conservative rule: the caller's cap must
 *                   hold the packet's worst case (120 ms), else -1 — a
 *                   GFN 5 ms packet can never trip it, and an oversized
 *                   hostile one dies without touching the decoder.
 *   stub          — honest "not linked" behaviour (available 0, create
 *                   NULL+reason, decode -1, version "none"). The
 *                   cross-compile legs and opus-less POSIX builds take
 *                   this body; T10 on the rig and any sandbox with the
 *                   dev package take the real one.
 */
#include "kn/kn_opus.h"

#ifdef KN_HAVE_OPUS

/* Two include layouts exist in the wild: pkg-config points straight at
 * .../include/opus and the vendored tree exports .../include (so <opus.h>),
 * while a system /usr/include resolves <opus/opus.h>. Accept both, never
 * guess per-platform. */
#if defined(__has_include)
#  if __has_include(<opus/opus.h>)
#    include <opus/opus.h>
#  elif __has_include(<opus.h>)
#    include <opus.h>
#  else
#    error "KN_HAVE_OPUS set but no opus header found"
#  endif
#else
#  include <opus/opus.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kn_opus_dec {
    OpusDecoder *dec;
    unsigned rate, channels;
};

/* 120 ms = the RFC 6716 maximum packet duration, in frames at `rate`. */
#define KN_OPUS_MAX_FRAMES(rate) ((unsigned)((rate) / 1000u * 120u))

int kn_opus_available(void) { return 1; }

const char *kn_opus_version(void) { return opus_get_version_string(); }

kn_opus_dec *kn_opus_decoder_create(unsigned rate, unsigned channels,
                                    char *errbuf, size_t errsz)
{
    kn_opus_dec *d;
    int err = OPUS_OK;

    if (rate == 0u || (channels != 1u && channels != 2u)) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "kn_opus: bad rate/channels %u/%u",
                     rate, channels);
        return NULL;
    }
    d = (kn_opus_dec *)calloc(1, sizeof *d);
    if (!d) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "kn_opus: decoder alloc failed");
        return NULL;
    }
    d->dec = opus_decoder_create((opus_int32)rate, (int)channels, &err);
    if (!d->dec || err != OPUS_OK) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "opus_decoder_create: %s",
                     opus_strerror(err));
        free(d);
        return NULL;
    }
    d->rate = rate;
    d->channels = channels;
    return d;
}

int kn_opus_decode_f32(kn_opus_dec *d, const unsigned char *pkt, size_t len,
                       float *out, unsigned cap_frames)
{
    int n;

    if (!d || !d->dec || !pkt || len == 0u || !out)
        return -1;
    if (cap_frames < KN_OPUS_MAX_FRAMES(d->rate))
        return -1;                       /* refuse before the decoder sees it */
    n = opus_decode_float(d->dec, (const unsigned char *)pkt, (opus_int32)len,
                          out, (int)cap_frames, 0);
    return n > 0 ? n : -1;
}

void kn_opus_decoder_destroy(kn_opus_dec *d)
{
    if (!d)
        return;
    if (d->dec)
        opus_decoder_destroy(d->dec);
    free(d);
}

#else  /* ---------------- honest stub: no libopus in this build --------- */

#include <stdio.h>

struct kn_opus_dec { int unused; };

int kn_opus_available(void) { return 0; }

const char *kn_opus_version(void) { return "none"; }

kn_opus_dec *kn_opus_decoder_create(unsigned rate, unsigned channels,
                                    char *errbuf, size_t errsz)
{
    (void)rate;
    (void)channels;
    if (errbuf && errsz)
        snprintf(errbuf, errsz,
                 "libopus not linked in this build (KN_HAVE_OPUS=0)");
    return NULL;
}

int kn_opus_decode_f32(kn_opus_dec *d, const unsigned char *pkt, size_t len,
                       float *out, unsigned cap_frames)
{
    (void)d;
    (void)pkt;
    (void)len;
    (void)out;
    (void)cap_frames;
    return -1;
}

void kn_opus_decoder_destroy(kn_opus_dec *d) { (void)d; }

#endif /* KN_HAVE_OPUS */
