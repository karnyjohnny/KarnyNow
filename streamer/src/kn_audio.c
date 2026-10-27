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
        if (pad == 0 && a->stats.frames_written > 0)
            a->stats.starved++;                      /* device ran dry */
        {
            BYTE *data = NULL;
            HRESULT hr = IAudioRenderClient_GetBuffer(a->render, avail, &data);
            if (SUCCEEDED(hr) && data) {
                kn_audio_gen(&a->fmt, data, avail, a->tone_hz, &phase);
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

kn_audio *kn_audio_open(unsigned tone_hz, char *errbuf, size_t errsz)
{
    kn_audio *a;
    HRESULT hr;

    a = (kn_audio *)calloc(1, sizeof *a);
    if (!a) {
        ka_err(errbuf, errsz, "out of memory");
        return NULL;
    }
    a->tone_hz = tone_hz;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) a->coinit = 1;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&a->enumerator);
    if (FAILED(hr) || !a->enumerator) {
        ka_err(errbuf, errsz, "CoCreateInstance(MMDeviceEnumerator) 0x%08lx",
               (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(a->enumerator, eRender,
                                                     eMultimedia, &a->device);
    if (FAILED(hr) || !a->device) {
        ka_err(errbuf, errsz, "GetDefaultAudioEndpoint 0x%08lx (no default "
               "render device?)", (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
    }
    hr = IMMDevice_Activate(a->device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&a->client);
    if (FAILED(hr) || !a->client) {
        ka_err(errbuf, errsz, "IAudioClient activate 0x%08lx",
               (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
    }
    hr = IAudioClient_GetMixFormat(a->client, &a->mix);
    if (FAILED(hr) || !a->mix) {
        ka_err(errbuf, errsz, "GetMixFormat 0x%08lx", (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
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
            kn_audio_close(a);
            return NULL;
        }
    }

    a->event = CreateEventExW(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!a->event) {
        ka_err(errbuf, errsz, "CreateEventEx failed (err %lu)",
               (unsigned long)GetLastError());
        kn_audio_close(a);
        return NULL;
    }
    /* 100 ms shared-mode buffer, event-driven (the documented WASAPI
     * pattern; ~4800 frames at 48 kHz) */
    hr = IAudioClient_Initialize(a->client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 1000000LL, 0LL, a->mix, NULL);
    if (FAILED(hr)) {
        ka_err(errbuf, errsz, "IAudioClient_Initialize 0x%08lx",
               (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
    }
    hr = IAudioClient_GetBufferSize(a->client, &a->buffer_frames);
    if (FAILED(hr) || a->buffer_frames == 0) {
        ka_err(errbuf, errsz, "GetBufferSize 0x%08lx", (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
    }
    hr = IAudioClient_SetEventHandle(a->client, a->event);
    if (FAILED(hr)) {
        ka_err(errbuf, errsz, "SetEventHandle 0x%08lx", (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
    }
    hr = IAudioClient_GetService(a->client, &IID_IAudioRenderClient,
                                 (void **)&a->render);
    if (FAILED(hr) || !a->render) {
        ka_err(errbuf, errsz, "GetService(RenderClient) 0x%08lx",
               (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
    }

    /* Start BEFORE spawning the refill thread: a thread waiting on the
     * event before Start would burn its first 200 ms timeout and pollute
     * wait_timeouts (WASAPI only signals buffer events on a running
     * stream). */
    hr = IAudioClient_Start(a->client);
    if (FAILED(hr)) {
        ka_err(errbuf, errsz, "IAudioClient_Start 0x%08lx",
               (unsigned long)hr);
        kn_audio_close(a);
        return NULL;
    }
    a->run = 1;
    a->thread = CreateThread(NULL, 0, kn_audio_thread, a, 0, NULL);
    if (!a->thread) {
        ka_err(errbuf, errsz, "CreateThread failed (err %lu)",
               (unsigned long)GetLastError());
        a->run = 0;
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
