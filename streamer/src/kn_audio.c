/* kn_audio.c — WASAPI synthetic-PCM render (contract: kn_audio.h).
 *
 * Engine pattern = the documented WASAPI shared-mode event-driven render
 * client; every COM spelling verified against mingw-w64 headers this round
 * (audioclient.h / mmdeviceapi.h C-style macros — the d3d9 lesson: never
 * from memory). IIDs are instantiated locally via INITGUID in THIS single
 * translation unit (no -luuid needed); the only new import is ole32.dll
 * (OS-built-in on Win7).
 *
 * The parser/generator below are platform-neutral byte-level code, host-
 * tested by test_knaudio.
 */
#include "kn/kn_audio.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KA_PI 3.14159265358979323846
#define KA_AMP 0.125                      /* ~ -18 dBFS */

/* ------------------------------------------------------ neutral: format */

static unsigned ka_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned ka_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* KSDATAFORMAT_SUBTYPE_* share the tail {0000-0010-8000-00aa00389b71};
 * Data1: 1 = PCM, 3 = IEEE_FLOAT (ksmedia.h:620-628). */
static const unsigned char k_ka_kdf_tail[12] = {
    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
};

int kn_audio_fmt_parse(const unsigned char *wfx, size_t len,
                       kn_audio_fmt *out)
{
    unsigned tag, bits;
    if (!wfx || !out || len < 18) return -1;
    memset(out, 0, sizeof *out);
    tag = ka_u16(wfx);
    out->channels = ka_u16(wfx + 2);
    out->rate = ka_u32(wfx + 4);
    out->bits = ka_u16(wfx + 14);
    bits = out->bits;
    out->fmt = KN_AFMT_UNKNOWN;

    if (tag == 0xFFFEu) {                            /* WAVE_FORMAT_EXTENSIBLE */
        if (len < 40) return -1;
        out->valid_bits = ka_u16(wfx + 18);
        if (memcmp(wfx + 28, k_ka_kdf_tail, 12) == 0) {
            unsigned d1 = ka_u32(wfx + 24);
            if (d1 == 3u && bits == 32u) out->fmt = KN_AFMT_F32;
            else if (d1 == 1u) out->fmt = bits == 16u ? KN_AFMT_S16
                                       : bits == 32u ? KN_AFMT_S32
                                                     : KN_AFMT_UNKNOWN;
        }
        if (out->valid_bits && out->valid_bits < bits &&
            out->fmt == KN_AFMT_S32 && out->valid_bits <= 24u) {
            /* 32-bit containers with <=24 valid bits still parse as S32;
             * the generator writes full-range S32 which the device scales —
             * acceptable for a synthetic source (documented). */
        }
    } else if (tag == 0x0001u) {                     /* WAVE_FORMAT_PCM */
        out->fmt = bits == 16u ? KN_AFMT_S16
                 : bits == 32u ? KN_AFMT_S32 : KN_AFMT_UNKNOWN;
    }
    if (out->rate == 0 || out->channels == 0 || out->channels > 8u)
        return -1;
    if (bits == 0 || (bits & 7u)) return -1;
    out->block_align = out->channels * (bits / 8u);
    return 0;
}

void kn_audio_fmt_describe(const kn_audio_fmt *f, char *out, size_t outsz)
{
    const char *name;
    if (!out || outsz == 0) return;
    if (!f) { snprintf(out, outsz, "none"); return; }
    name = f->fmt == KN_AFMT_F32 ? "float32"
         : f->fmt == KN_AFMT_S16 ? "s16"
         : f->fmt == KN_AFMT_S32 ? "s32" : "unknown";
    snprintf(out, outsz, "%uHz %uch %s", f->rate % 1000000u,
             f->channels % 100u, name);
}

/* ---------------------------------------------------- neutral: generator */

void kn_audio_gen(const kn_audio_fmt *f, unsigned char *dst, unsigned frames,
                  unsigned tone_hz, double *phase)
{
    unsigned fr;
    double step, ph;
    if (!f || !dst || !phase || f->fmt == KN_AFMT_UNKNOWN ||
        f->block_align == 0)
        return;
    step = tone_hz ? (2.0 * KA_PI * (double)tone_hz / (double)f->rate) : 0.0;
    ph = *phase;
    for (fr = 0; fr < frames; ++fr) {
        unsigned ch;
        double s = tone_hz ? sin(ph) * KA_AMP : 0.0;
        ph += step;
        if (ph >= 2.0 * KA_PI) ph -= 2.0 * KA_PI;
        for (ch = 0; ch < f->channels; ++ch) {
            switch (f->fmt) {
            case KN_AFMT_F32: {
                float v = (float)s;
                memcpy(dst, &v, 4);
                dst += 4;
                break;
            }
            case KN_AFMT_S16: {
                short v = (short)(s * 32767.0);
                memcpy(dst, &v, 2);
                dst += 2;
                break;
            }
            case KN_AFMT_S32: {
                int v = (int)(s * 2147483647.0);
                memcpy(dst, &v, 4);
                dst += 4;
                break;
            }
            default:
                memset(dst, 0, f->block_align);
                dst += f->block_align;
                ch = f->channels;              /* bail: whole frame zeroed */
                break;
            }
        }
    }
    *phase = ph;
}

/* ============================================================ F21 feed core
 * Platform-neutral SPSC PCM ring + the float32 -> device-format convert.
 * Host-testable by design (test_knaudio): the Win32 engine below is a
 * thin consumer of exactly these two functions. */

unsigned kn_audio_feed_init(kn_audio_feed *f, float *backing,
                            unsigned backing_frames, unsigned channels)
{
    unsigned cap;

    if (!f || !backing || channels == 0u || channels > 8u)
        return 0u;
    cap = 1u;
    while (cap * 2u <= backing_frames && cap * 2u >= cap)
        cap *= 2u;
    if (backing_frames < 1u || cap < 1u)
        return 0u;
    f->buf = backing;
    f->cap = cap;
    f->channels = channels;
    f->wr = 0u;
    f->rd = 0u;
    f->pushed = 0ull;
    f->popped = 0ull;
    f->dropped = 0ull;
    return cap;
}

unsigned kn_audio_feed_level(const kn_audio_feed *f)
{
    if (!f)
        return 0u;
    return (f->wr - f->rd) & (f->cap - 1u);
}

unsigned kn_audio_feed_push(kn_audio_feed *f, const float *pcm,
                            unsigned frames)
{
    unsigned free_slots, n, first, second, ch;

    if (!f || !pcm || frames == 0u)
        return 0u;
    /* cap-1 usable frames: wr==rd must stay unambiguously EMPTY */
    free_slots = (f->cap - 1u) - kn_audio_feed_level(f);
    n = frames < free_slots ? frames : free_slots;
    if (n < frames)
        f->dropped += (unsigned long long)(frames - n);
    if (n == 0u)
        return 0u;
    ch = f->channels;
    first = f->cap - (f->wr & (f->cap - 1u));      /* frames to the wrap */
    if (first > n)
        first = n;
    second = n - first;
    memcpy(f->buf + (size_t)(f->wr & (f->cap - 1u)) * ch, pcm,
           (size_t)first * ch * sizeof(float));
    if (second)
        memcpy(f->buf, pcm + (size_t)first * ch,
               (size_t)second * ch * sizeof(float));
    f->wr = (f->wr + n) & (f->cap - 1u);
    f->pushed += n;
    return n;
}

unsigned kn_audio_feed_pop(kn_audio_feed *f, float *dst, unsigned frames)
{
    unsigned level, n, first, second, ch;

    if (!f || !dst || frames == 0u)
        return 0u;
    level = kn_audio_feed_level(f);
    n = frames < level ? frames : level;
    if (n == 0u)
        return 0u;
    ch = f->channels;
    first = f->cap - (f->rd & (f->cap - 1u));
    if (first > n)
        first = n;
    second = n - first;
    memcpy(dst, f->buf + (size_t)(f->rd & (f->cap - 1u)) * ch,
           (size_t)first * ch * sizeof(float));
    if (second)
        memcpy(dst + (size_t)first * ch, f->buf,
               (size_t)second * ch * sizeof(float));
    f->rd = (f->rd + n) & (f->cap - 1u);
    f->popped += n;
    return n;
}

unsigned kn_audio_feed_plan(const kn_audio_feed *f, unsigned avail,
                            int *primed, unsigned target_frames)
{
    unsigned level;

    if (!f || !primed || avail == 0u)
        return 0u;
    if (target_frames == 0u || *primed)
        return avail;                        /* legacy / already primed */
    level = kn_audio_feed_level(f);
    if (level >= target_frames || level + 1u >= f->cap) {
        *primed = 1;                         /* waterline (or ring full) */
        return avail;
    }
    return 0u;                               /* silent pre-roll tick */
}

static float ka_clamp1(float v)
{
    if (!(v == v))                       /* NaN: silence, never propagate */
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    if (v < -1.0f)
        return -1.0f;
    return v;
}

void kn_audio_xform_f32(const kn_audio_fmt *fmt, unsigned char *dst,
                        const float *src, unsigned frames)
{
    unsigned i;
    unsigned ch;

    if (!fmt || !dst || !src)
        return;
    ch = fmt->channels ? fmt->channels : 1u;
    for (i = 0u; i < frames * ch; ++i) {
        float v = ka_clamp1(src[i]);

        switch (fmt->fmt) {
        case KN_AFMT_F32: {
            memcpy(dst, &v, 4);
            dst += 4;
            break;
        }
        case KN_AFMT_S16: {
            short s = (short)lrintf(v * 32767.0f);
            memcpy(dst, &s, 2);
            dst += 2;
            break;
        }
        case KN_AFMT_S32: {
            /* double math: 2147483647.0f rounds UP to 2^31 in float, which
             * would overflow the int cast at v == 1.0 (lrintf(2^31) UB). */
            int s = (int)lrint((double)v * 2147483647.0);
            memcpy(dst, &s, 4);
            dst += 4;
            break;
        }
        default:
            memset(dst, 0, fmt->block_align ? fmt->block_align : 4u);
            dst += fmt->block_align ? fmt->block_align : 4u;
            i += ch - 1u;                /* one container frame of silence */
            break;
        }
    }
}

/* ============================================================ Win32 engine */

#ifdef _WIN32

#define INITGUID            /* instantiate CLSID/IIDs in THIS TU only */
#define COBJMACROS          /* C-style COM macros: audioclient.h/mmdeviceapi.h
                             * gate IAudioClient_* / IMMDevice_* behind this
                             * (d3d9.h does not — header inconsistency) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include "kn/knlog.h"

struct kn_audio {
    kn_audio_fmt fmt;
    WAVEFORMATEX *mix;               /* CoTaskMemAlloc'd by GetMixFormat */
    IMMDeviceEnumerator *enumerator;
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    HANDLE event;
    HANDLE thread;
    volatile int run;
    volatile kn_audio_stats stats;   /* single writer (audio thread) */
    unsigned buffer_frames;
    unsigned tone_hz;
    int coinit;
    /* F21 feed mode: 0 = synthetic sine (v1), 1 = consume fd */
    int feed;
    int primed;                        /* F25: waterline state (engine) */
    unsigned prime_frames;             /* F25: 100 ms at the device rate */
    kn_audio_feed fd;
    float *fd_backing;               /* the ring storage (malloc'd) */
    float *pop_buf;                  /* per-refill float scratch, frames*ch */
};

static void ka_logf(int level, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    if (!kn_log_is_initialized()) return;
    if (!kn_log_would_log(KN_CH_MEDIA, (kn_level)level)) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg[strcspn(msg, "\r\n")] = '\0';
    if (msg[0])
        kn_log_simple(KN_CH_MEDIA, (kn_level)level, NULL, msg);
}

static DWORD WINAPI kn_audio_thread(LPVOID p)
{
    kn_audio *a = (kn_audio *)p;
    double phase = 0.0;
    (void)CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (a->run) {
        UINT32 pad = 0, avail;
        DWORD w = WaitForSingleObject(a->event, 200);
        if (w == WAIT_TIMEOUT) { a->stats.wait_timeouts++; continue; }
        if (w != WAIT_OBJECT_0) break;
        if (FAILED(IAudioClient_GetCurrentPadding(a->client, &pad))) {
            a->stats.buffer_errors++;
            continue;
        }
        a->stats.queue_ms = a->fmt.rate
            ? (unsigned)(((unsigned long long)pad * 1000ull) / a->fmt.rate)
            : 0u;
        if (pad >= a->buffer_frames) continue;       /* nothing to fill */
        avail = a->buffer_frames - pad;
        if (pad == 0 && a->stats.frames_written > 0 && !a->feed)
            a->stats.starved++;                      /* device ran dry */
        {
            BYTE *data = NULL;
            HRESULT hr = IAudioRenderClient_GetBuffer(a->render, avail, &data);
            if (SUCCEEDED(hr) && data) {
                if (a->feed) {
                    /* F21+F25: consume the session ring behind a 100 ms
                     * pre-buffer waterline. plan() holds the device on
                     * silence (NOT counted as starvation) until the ring
                     * primes; after that a pop that comes back empty is
                     * an underrun EPISODE: starved++, re-arm the
                     * waterline. The zero-fill of the untaken tail is
                     * what keeps the device clock honest. */
                    int was = a->primed;
                    unsigned want = kn_audio_feed_plan(&a->fd, avail,
                                                       &a->primed,
                                                       a->prime_frames);
                    unsigned got = want
                        ? kn_audio_feed_pop(&a->fd, a->pop_buf, want) : 0u;
                    if (a->primed && !was)
                        a->stats.prerolls++;
                    if (want && got == 0u && a->fd.pushed > 0ull) {
                        a->stats.starved++;
                        a->primed = 0;       /* re-prime behind waterline */
                    }
                    if (got < avail)
                        memset(a->pop_buf + (size_t)got * a->fd.channels, 0,
                               (size_t)(avail - got) * a->fd.channels *
                               sizeof(float));
                    kn_audio_xform_f32(&a->fmt, data, a->pop_buf, avail);
                } else {
                    kn_audio_gen(&a->fmt, data, avail, a->tone_hz, &phase);
                }
                hr = IAudioRenderClient_ReleaseBuffer(a->render, avail, 0);
                if (SUCCEEDED(hr)) {
                    a->stats.frames_written += avail;
                    a->stats.events++;
                } else {
                    a->stats.buffer_errors++;
                }
            } else {
                a->stats.buffer_errors++;
            }
        }
    }
    CoUninitialize();
    return 0;
}

int kn_audio_available(void) { return 1; }

static void ka_err(char *errbuf, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    if (!errbuf || errsz == 0) return;
    va_start(ap, fmt);
    vsnprintf(errbuf, errsz, fmt, ap);
    va_end(ap);
}

/* F21: the device-setup half of kn_audio_open, shared verbatim by the
 * feed opener so both modes ride ONE COM/WASAPI bring-up (the round-XV
 * G1 pattern; only the refill source differs). Returns 0 on success. */
static int ka_setup_device(kn_audio *a, char *errbuf, size_t errsz)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) a->coinit = 1;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&a->enumerator);
    if (FAILED(hr) || !a->enumerator) {
        ka_err(errbuf, errsz, "CoCreateInstance(MMDeviceEnumerator) 0x%08lx",
               (unsigned long)hr);
        return -1;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(a->enumerator, eRender,
                                                     eMultimedia, &a->device);
    if (FAILED(hr) || !a->device) {
        ka_err(errbuf, errsz, "GetDefaultAudioEndpoint 0x%08lx (no default "
               "render device?)", (unsigned long)hr);
        return -1;
    }
    hr = IMMDevice_Activate(a->device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&a->client);
    if (FAILED(hr) || !a->client) {
        ka_err(errbuf, errsz, "IAudioClient activate 0x%08lx",
               (unsigned long)hr);
        return -1;
    }
    hr = IAudioClient_GetMixFormat(a->client, &a->mix);
    if (FAILED(hr) || !a->mix) {
        ka_err(errbuf, errsz, "GetMixFormat 0x%08lx", (unsigned long)hr);
        return -1;
    }
    {
        size_t wlen = sizeof(WAVEFORMATEX) + (size_t)a->mix->cbSize;
        if (wlen > sizeof(WAVEFORMATEXTENSIBLE))
            wlen = sizeof(WAVEFORMATEXTENSIBLE);
        if (kn_audio_fmt_parse((const unsigned char *)a->mix, wlen,
                               &a->fmt) != 0 ||
            a->fmt.fmt == KN_AFMT_UNKNOWN) {
            char d[64];
            kn_audio_fmt_describe(&a->fmt, d, sizeof d);
            ka_err(errbuf, errsz, "unsupported mix format (tag 0x%04x, %s) — "
                   "v1 renders float32/s16/s32 only",
                   (unsigned)a->mix->wFormatTag, d);
            return -1;
        }
    }

    a->event = CreateEventExW(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!a->event) {
        ka_err(errbuf, errsz, "CreateEventEx failed (err %lu)",
               (unsigned long)GetLastError());
        return -1;
    }
    /* 100 ms shared-mode buffer, event-driven (the documented WASAPI
     * pattern; ~4800 frames at 48 kHz) */
    hr = IAudioClient_Initialize(a->client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 1000000LL, 0LL, a->mix, NULL);
    if (FAILED(hr)) {
        ka_err(errbuf, errsz, "IAudioClient_Initialize 0x%08lx",
               (unsigned long)hr);
        return -1;
    }
    hr = IAudioClient_GetBufferSize(a->client, &a->buffer_frames);
    if (FAILED(hr) || a->buffer_frames == 0) {
        ka_err(errbuf, errsz, "GetBufferSize 0x%08lx", (unsigned long)hr);
        return -1;
    }
    hr = IAudioClient_SetEventHandle(a->client, a->event);
    if (FAILED(hr)) {
        ka_err(errbuf, errsz, "SetEventHandle 0x%08lx", (unsigned long)hr);
        return -1;
    }
    hr = IAudioClient_GetService(a->client, &IID_IAudioRenderClient,
                                 (void **)&a->render);
    if (FAILED(hr) || !a->render) {
        ka_err(errbuf, errsz, "GetService(RenderClient) 0x%08lx",
               (unsigned long)hr);
        return -1;
    }
    return 0;
}

/* Start the client + the refill thread (both openers, after setup). */
static int ka_start_engine(kn_audio *a, char *errbuf, size_t errsz)
{
    HRESULT hr;

    /* Start BEFORE spawning the refill thread: a thread waiting on the
     * event before Start would burn its first 200 ms timeout and pollute
     * wait_timeouts (WASAPI only signals buffer events on a running
     * stream). */
    hr = IAudioClient_Start(a->client);
    if (FAILED(hr)) {
        ka_err(errbuf, errsz, "IAudioClient_Start 0x%08lx",
               (unsigned long)hr);
        return -1;
    }
    a->run = 1;
    a->thread = CreateThread(NULL, 0, kn_audio_thread, a, 0, NULL);
    if (!a->thread) {
        ka_err(errbuf, errsz, "CreateThread failed (err %lu)",
               (unsigned long)GetLastError());
        a->run = 0;
        return -1;
    }
    return 0;
}

kn_audio *kn_audio_open(unsigned tone_hz, char *errbuf, size_t errsz)
{
    kn_audio *a;

    a = (kn_audio *)calloc(1, sizeof *a);
    if (!a) {
        ka_err(errbuf, errsz, "out of memory");
        return NULL;
    }
    a->tone_hz = tone_hz;
    if (ka_setup_device(a, errbuf, errsz) != 0) {
        kn_audio_close(a);
        return NULL;
    }
    if (ka_start_engine(a, errbuf, errsz) != 0) {
        kn_audio_close(a);
        return NULL;
    }
    {
        char d[64];
        kn_audio_fmt_describe(&a->fmt, d, sizeof d);
        ka_logf(KN_INFO, "audio open: %s, buffer %u frames (~%u ms), tone %u Hz",
                d, a->buffer_frames % 1000000u,
                a->fmt.rate ? (a->buffer_frames * 1000u / a->fmt.rate) % 100000u
                            : 0u,
                tone_hz % 100000u);
    }
    return a;
}

kn_audio *kn_audio_open_feed(char *errbuf, size_t errsz)
{
    kn_audio *a;
    unsigned cap, backing_frames;

    a = (kn_audio *)calloc(1, sizeof *a);
    if (!a) {
        ka_err(errbuf, errsz, "out of memory");
        return NULL;
    }
    a->feed = 1;
    if (ka_setup_device(a, errbuf, errsz) != 0) {
        kn_audio_close(a);
        return NULL;
    }
    /* 2 s of device-rate ring: absorbs decode/pump jitter and a brief
     * scheduler hiccup without an audible gap, while staying far below
     * the latency a listener would notice as lip-drift (docs/AUDIO.md). */
    backing_frames = a->fmt.rate ? a->fmt.rate * 2u : 96000u;
    a->fd_backing = (float *)malloc((size_t)backing_frames *
                                    a->fmt.channels * sizeof(float));
    a->pop_buf = (float *)malloc((size_t)a->buffer_frames *
                                 a->fmt.channels * sizeof(float));
    if (!a->fd_backing || !a->pop_buf) {
        ka_err(errbuf, errsz, "feed ring alloc failed (%u frames x %u ch)",
               backing_frames % 10000000u, a->fmt.channels % 1000u);
        kn_audio_close(a);
        return NULL;
    }
    cap = kn_audio_feed_init(&a->fd, a->fd_backing, backing_frames,
                             a->fmt.channels);
    if (cap == 0u) {
        ka_err(errbuf, errsz, "feed init rejected the ring");
        kn_audio_close(a);
        return NULL;
    }
    /* F25: 100 ms pre-buffer waterline, clamped to the usable ring. The
     * latency is inaudible against GFN's own pipeline and buys arrival
     * jitter immunity; runs 36-38 showed the ungated ring starving on
     * every hiccup (starved 759/801/802). */
    a->primed = 0;
    a->prime_frames = a->fmt.rate / 10u;
    if (a->prime_frames > cap - 1u)
        a->prime_frames = cap - 1u;
    if (ka_start_engine(a, errbuf, errsz) != 0) {
        kn_audio_close(a);
        return NULL;
    }
    {
        char d[64];
        kn_audio_fmt_describe(&a->fmt, d, sizeof d);
        ka_logf(KN_INFO, "audio open (feed): %s, buffer %u frames, ring %u frames",
                d, a->buffer_frames % 1000000u, cap % 10000000u);
    }
    return a;
}

unsigned kn_audio_push_f32(kn_audio *a, const float *pcm, unsigned frames)
{
    if (!a || !a->feed)
        return 0u;
    return kn_audio_feed_push(&a->fd, pcm, frames);
}

int kn_audio_queue_ms(kn_audio *a)
{
    return a ? (int)a->stats.queue_ms : -1;
}

int kn_audio_get_stats(kn_audio *a, kn_audio_stats *out)
{
    if (!a || !out) return -1;
    out->events = a->stats.events;
    out->frames_written = a->stats.frames_written;
    out->starved = a->stats.starved;
    out->wait_timeouts = a->stats.wait_timeouts;
    out->buffer_errors = a->stats.buffer_errors;
    out->queue_ms = a->stats.queue_ms;
    out->push_dropped = a->fd.dropped;      /* producer-written, benign read */
    out->ring_ms = a->fmt.rate
        ? kn_audio_feed_level(&a->fd) * 1000u / a->fmt.rate
        : 0u;
    out->feed = a->feed ? 1u : 0u;
    return 0;
}

const kn_audio_fmt *kn_audio_format(kn_audio *a)
{
    return a ? &a->fmt : NULL;
}

void kn_audio_close(kn_audio *a)
{
    if (!a) return;
    a->run = 0;
    if (a->event) SetEvent(a->event);        /* wake the thread for exit */
    if (a->thread) {
        WaitForSingleObject(a->thread, 3000);
        CloseHandle(a->thread);
        a->thread = NULL;
    }
    if (a->client) (void)IAudioClient_Stop(a->client);  /* after the join */
    if (a->render) IAudioRenderClient_Release(a->render);
    if (a->client) IAudioClient_Release(a->client);
    if (a->device) IMMDevice_Release(a->device);
    if (a->enumerator) IMMDeviceEnumerator_Release(a->enumerator);
    if (a->event) CloseHandle(a->event);
    if (a->mix) CoTaskMemFree(a->mix);
    if (a->fd_backing) free(a->fd_backing);
    if (a->pop_buf) free(a->pop_buf);
    if (a->coinit) CoUninitialize();
    free(a);
}

#else  /* !_WIN32 — honest stub; the neutral parser/generator stay real */

int kn_audio_available(void) { return 0; }

kn_audio *kn_audio_open(unsigned tone_hz, char *errbuf, size_t errsz)
{
    (void)tone_hz;
    if (errbuf && errsz)
        snprintf(errbuf, errsz, "WASAPI is Win32-only (posix host build)");
    return NULL;
}

kn_audio *kn_audio_open_feed(char *errbuf, size_t errsz)
{
    if (errbuf && errsz)
        snprintf(errbuf, errsz, "WASAPI is Win32-only (posix host build)");
    return NULL;
}

unsigned kn_audio_push_f32(kn_audio *a, const float *pcm, unsigned frames)
{
    (void)a;
    (void)pcm;
    (void)frames;
    return 0u;
}

int kn_audio_queue_ms(kn_audio *a) { (void)a; return -1; }

int kn_audio_get_stats(kn_audio *a, kn_audio_stats *out)
{
    (void)a;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    return -1;
}

const kn_audio_fmt *kn_audio_format(kn_audio *a) { (void)a; return NULL; }

void kn_audio_close(kn_audio *a) { (void)a; }

#endif /* _WIN32 */
