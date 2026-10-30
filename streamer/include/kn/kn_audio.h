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

/* --- F21 feed core (platform-NEUTRAL, host-testable) -------------------
 * The single-producer/single-consumer PCM ring between the session's
 * decode side (producer: the pump thread, inside the kn_arx emit
 * callback) and the WASAPI render thread (consumer). Frame-indexed,
 * power-of-two capacity, floats stay floats INSIDE the ring — the
 * device-format conversion happens exactly once, at the consumer, in
 * kn_audio_xform_f32(), so a 44.1 kHz S16 device and a 48 kHz float32
 * device ride the same ring code.
 *
 * Concurrency contract: `wr` is written ONLY by the producer after its
 * memcpys, `rd` ONLY by the consumer after its reads; both are volatile
 * unsigned words (the same single-word latch discipline as the pump's
 * sctl_* mirrors — x86 stores are release-ordered for this use, and the
 * worst case of a stale index read is one refill of silence or one
 * dropped tail chunk, both counted, never corruption).
 * push() never blocks and never fails: frames that do not fit are
 * DROPPED from the tail and counted (a full ring means the consumer is
 * a whole ring behind — audibly hopeless anyway; dropping keeps the
 * playback edge at the live position instead of sliding into latency).
 * pop() returns how many frames it took; the caller zero-fills the rest
 * (that zero-fill IS the underrun, and the engine counts it). */
typedef struct kn_audio_feed {
    float *buf;                /* cap * channels interleaved floats */
    unsigned cap;              /* power-of-two frame capacity */
    unsigned channels;
    volatile unsigned wr;      /* producer frame cursor (mod cap) */
    volatile unsigned rd;      /* consumer frame cursor (mod cap) */
    unsigned long long pushed; /* frames accepted, lifetime */
    unsigned long long popped; /* frames delivered, lifetime */
    unsigned long long dropped;/* frames refused (ring full), lifetime */
} kn_audio_feed;

/* Bind a feed to caller-owned `backing` (backing_frames * channels
 * floats). backing_frames is ROUNDED DOWN to a power of two (minimum 1);
 * returns the usable frame capacity, or 0 on bad args. The caller keeps
 * the allocation alive for the feed's whole life. */
unsigned kn_audio_feed_init(kn_audio_feed *f, float *backing,
                            unsigned backing_frames, unsigned channels);

/* Producer: copy up to `frames` interleaved float frames in; returns the
 * frames accepted (the rest counted in dropped). */
unsigned kn_audio_feed_push(kn_audio_feed *f, const float *pcm,
                            unsigned frames);

/* Consumer: take up to `frames` frames into `dst` (capacity frames *
 * channels floats); returns frames taken (0 = empty ring). */
unsigned kn_audio_feed_pop(kn_audio_feed *f, float *dst, unsigned frames);

/* F25 (round N slice 4): the pre-buffer waterline decision, pure and
 * host-testable. Feed-mode render tick: with *primed==0 the ring must
 * reach `target_frames` (or be full — a source faster than the device
 * must drain regardless) before the engine starts consuming; until then
 * the device gets silence WITHOUT it counting as starvation. Once
 * primed, plan returns `avail` and pop's own contract rules. The
 * caller (engine) flips *primed back to 0 on a post-prime underrun, so
 * `starved` counts UNDERRUN EPISODES, each re-armed behind the
 * waterline. target_frames==0 disables gating (legacy behaviour).
 * Returns the frames to pop now (0 = zero-fill this tick). NULL-safe. */
unsigned kn_audio_feed_plan(const kn_audio_feed *f, unsigned avail,
                            int *primed, unsigned target_frames);

/* Current fill in frames (wr - rd, unsigned wrap-safe). */
unsigned kn_audio_feed_level(const kn_audio_feed *f);

/* Convert `frames` interleaved float32 samples (already at the device
 * rate and channel count) into the device container format: F32 copies
 * with a +-1 clamp, S16/S32 scale+round+clamp, KN_AFMT_UNKNOWN zeroes.
 * Byte-level portable code, same family as kn_audio_gen. */
void kn_audio_xform_f32(const kn_audio_fmt *fmt, unsigned char *dst,
                        const float *src, unsigned frames);

typedef struct kn_audio_stats {
    unsigned long long events;           /* buffer events serviced */
    unsigned long long frames_written;
    unsigned long long starved;          /* padding hit 0 => audible gap */
    unsigned long long wait_timeouts;    /* event never fired (200 ms) */
    unsigned long long buffer_errors;    /* GetBuffer/ReleaseBuffer fails */
    unsigned queue_ms;                   /* last measured device padding */
    /* F21 feed mode (zero in synthetic mode): */
    unsigned long long push_dropped;     /* frames refused: ring was full */
    unsigned long long prerolls;         /* F25: ring primings (waterline) */
    unsigned ring_ms;                    /* current ring fill, ms */
    unsigned feed;                       /* 1 = feed mode engine */
} kn_audio_stats;

typedef struct kn_audio kn_audio;        /* opaque */

int kn_audio_available(void);            /* 1 on Win32 */

/* Open the default render endpoint and start the synthetic stream.
 * tone_hz: 0 = silence, else the sine frequency. On failure returns NULL
 * and writes a human reason into errbuf. */
kn_audio *kn_audio_open(unsigned tone_hz, char *errbuf, size_t errsz);

/* F21: open the same default endpoint in FEED mode — the render thread
 * consumes a kn_audio_feed instead of the synthetic sine. The ring is
 * sized from the NEGOTIATED device rate (2 s of frames), so the producer
 * pushes at device rate (the session resamples before pushing when the
 * device is not 48 kHz). Starved counts refills that found the ring
 * empty AFTER the first push (pre-roll silence is not an underrun).
 * Failure modes and the errbuf contract are kn_audio_open's. POSIX/stub
 * builds return NULL ("no WASAPI backend"). */
kn_audio *kn_audio_open_feed(char *errbuf, size_t errsz);

/* Producer side of the feed engine: interleaved float32 at the device
 * rate. Returns frames accepted. 0 when `a` is NULL, not in feed mode,
 * or the ring is full (every refusal counted in stats.push_dropped). */
unsigned kn_audio_push_f32(kn_audio *a, const float *pcm, unsigned frames);

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
