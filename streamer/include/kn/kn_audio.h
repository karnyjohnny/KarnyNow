/* kn_audio.h — WASAPI synthetic-PCM render path (BOOTSTRAP §12 stage 1:
 * "Plus WASAPI with synthetic PCM"; the last piece before full G1).
 *
 * v1 scope: open the default render endpoint in SHARED mode, EVENTCALLBACK
 * driven, at the device MIX format (float32 on virtually every setup —
 * parsed portably, F32/S16/S32 supported), and render a synthetic source
 * (soft tone or silence) from a dedicated thread for the whole run. This
 * proves the audio pipeline (open → paced refill → stats → close) that
 * stage 2 will feed with decoded Opus 48 kHz (OpenNOW audio facts: PT 111,
 * 48 kHz, 45 ms jitter target, 100 ms concealment cap — swresample is
 * already linked for that day).
 *
 * Design notes:
 *  - The WAVEFORMATEX(WSEXTENSIBLE) parser and the sample generator are
 *    platform-NEUTRAL byte-level code (testable on the host: test_knaudio);
 *    only the engine is Win32. Same split as kn_d3d/kn_gs.
 *  - Stats (events/frames/starved/timeouts) have a single writer (the audio
 *    thread) and one reader (main); fields are volatile — benign races on
 *    32-bit reads are accepted for counters (the target is x64, where the
 *    reads are atomic anyway).
 *  - Failure to open audio never kills the video loop (the orchestrator
 *    fires kn_log_on_failure and reports it in the summary — T11 gates the
 *    G1 verdict on audio being open).
 */
#ifndef KN_AUDIO_H
#define KN_AUDIO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum kn_audio_samplefmt {
    KN_AFMT_UNKNOWN = 0,
    KN_AFMT_F32,
    KN_AFMT_S16,
    KN_AFMT_S32
} kn_audio_samplefmt;

typedef struct kn_audio_fmt {
    unsigned rate;            /* nSamplesPerSec */
    unsigned channels;
    unsigned bits;            /* wBitsPerSample (container) */
    unsigned valid_bits;      /* extensible wValidBitsPerSample (0 = n/a) */
    int      fmt;             /* kn_audio_samplefmt */
    unsigned block_align;     /* bytes per frame (channels * bits/8) */
} kn_audio_fmt;

/* Portable WAVEFORMATEX / WAVEFORMATEXTENSIBLE byte-layout parser.
 * len must be >= 18 (base) or >= 40 (extensible, wFormatTag 0xFFFE).
 * Returns 0 and fills *out (out->fmt may still be KN_AFMT_UNKNOWN for
 * subformats we do not render), or -1 on malformed input. */
int kn_audio_fmt_parse(const unsigned char *wfx, size_t len,
                       kn_audio_fmt *out);

/* Synthetic source: fill `frames` into `dst` (capacity frames *
 * block_align) per `fmt`, continuing the sine phase `*phase` (radians,
 * updated in place — seamless across calls). tone_hz == 0 = digital
 * silence. Amplitude ~ -18 dBFS (0.125) — audible proof of life without
 * hurting ears during a 10-minute gate. Neutral/testable. */
void kn_audio_gen(const kn_audio_fmt *fmt, unsigned char *dst,
                  unsigned frames, unsigned tone_hz, double *phase);

typedef struct kn_audio_stats {
    unsigned long long events;           /* buffer events serviced */
    unsigned long long frames_written;
    unsigned long long starved;          /* padding hit 0 => audible gap */
    unsigned long long wait_timeouts;    /* event never fired (200 ms) */
    unsigned long long buffer_errors;    /* GetBuffer/ReleaseBuffer fails */
    unsigned queue_ms;                   /* last measured device padding */
} kn_audio_stats;

typedef struct kn_audio kn_audio;        /* opaque */

int kn_audio_available(void);            /* 1 on Win32 */

/* Open the default render endpoint and start the synthetic stream.
 * tone_hz: 0 = silence, else the sine frequency. On failure returns NULL
 * and writes a human reason into errbuf. */
kn_audio *kn_audio_open(unsigned tone_hz, char *errbuf, size_t errsz);

/* Device padding in ms (queue_audio_ms for §7.6); -1 when unavailable. */
int kn_audio_queue_ms(kn_audio *a);

/* Snapshot the counters (0 ok, -1 args). */
int kn_audio_get_stats(kn_audio *a, kn_audio_stats *out);

/* The negotiated format (NULL when not open). Borrowed pointer. */
const kn_audio_fmt *kn_audio_format(kn_audio *a);

/* One-line format description for logs/summaries ("48000Hz 2ch float32"). */
void kn_audio_fmt_describe(const kn_audio_fmt *f, char *out, size_t outsz);

/* Stop, join the thread, release everything. NULL-safe. */
void kn_audio_close(kn_audio *a);

#ifdef __cplusplus
}
#endif

#endif /* KN_AUDIO_H */
