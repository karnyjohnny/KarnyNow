/* test_knaudio.c — host tests for the NEUTRAL half of kn_audio: the
 * WAVEFORMATEX(WSEXTENSIBLE) byte parser and the synthetic sample
 * generator (the WASAPI engine itself is Win32-only; its first real
 * execution is T11 on the target). Hand-built format blobs below mirror
 * what GetMixFormat returns on real systems (float32 extensible is the
 * near-universal mix format). */
#include "kn/kn_audio.h"
#include "test_port.h"

#include <math.h>
#include <string.h>

/* 48000 Hz stereo float32 WAVEFORMATEXTENSIBLE — the typical mix format */
static const unsigned char k_mix_f32[40] = {
    0xFE, 0xFF,             /* wFormatTag = WAVE_FORMAT_EXTENSIBLE */
    0x02, 0x00,             /* nChannels = 2 */
    0x80, 0xBB, 0x00, 0x00, /* nSamplesPerSec = 48000 */
    0x00, 0xEE, 0x02, 0x00, /* nAvgBytesPerSec = 192000 */
    0x08, 0x00,             /* nBlockAlign = 8 */
    0x20, 0x00,             /* wBitsPerSample = 32 */
    0x16, 0x00,             /* cbSize = 22 */
    0x20, 0x00,             /* wValidBitsPerSample = 32 */
    0x03, 0x00, 0x00, 0x00, /* dwChannelMask = FL|FR */
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

/* 44100 Hz stereo s16 PCM (plain WAVEFORMATEX) */
static const unsigned char k_pcm16[18] = {
    0x01, 0x00,             /* WAVE_FORMAT_PCM */
    0x02, 0x00,             /* 2 ch */
    0x44, 0xAC, 0x00, 0x00, /* 44100 */
    0x10, 0xB1, 0x02, 0x00, /* 176400 */
    0x04, 0x00,             /* block align 4 */
    0x10, 0x00,             /* 16 bits */
    0x00, 0x00              /* cbSize 0 */
};

/* extensible s32 PCM subtype (Data1 = 1) */
static const unsigned char k_ext_s32[40] = {
    0xFE, 0xFF, 0x02, 0x00, 0x80, 0xBB, 0x00, 0x00,
    0x00, 0xEE, 0x02, 0x00, 0x08, 0x00, 0x20, 0x00,
    0x16, 0x00, 0x20, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

static void test_fmt_parse(void)
{
    kn_audio_fmt f;
    unsigned char bad[40];

    CHECK(kn_audio_fmt_parse(k_mix_f32, sizeof k_mix_f32, &f) == 0,
          "mix f32 parses");
    CHECK(f.fmt == KN_AFMT_F32 && f.rate == 48000 && f.channels == 2 &&
          f.bits == 32 && f.valid_bits == 32 && f.block_align == 8,
          "mix f32 fields");

    CHECK(kn_audio_fmt_parse(k_pcm16, sizeof k_pcm16, &f) == 0,
          "pcm16 parses");
    CHECK(f.fmt == KN_AFMT_S16 && f.rate == 44100 && f.channels == 2 &&
          f.bits == 16 && f.valid_bits == 0 && f.block_align == 4,
          "pcm16 fields");

    CHECK(kn_audio_fmt_parse(k_ext_s32, sizeof k_ext_s32, &f) == 0 &&
          f.fmt == KN_AFMT_S32 && f.rate == 48000, "ext s32 parses");

    /* wrong subformat tail -> structurally OK but UNKNOWN */
    memcpy(bad, k_mix_f32, sizeof bad);
    bad[39] ^= 0xFF;
    CHECK(kn_audio_fmt_parse(bad, sizeof bad, &f) == 0 &&
          f.fmt == KN_AFMT_UNKNOWN, "bad subformat tail -> UNKNOWN");

    /* malformed inputs */
    CHECK(kn_audio_fmt_parse(k_mix_f32, 16, &f) == -1, "short base -> -1");
    CHECK(kn_audio_fmt_parse(k_mix_f32, 38, &f) == -1, "short extensible -> -1");
    CHECK(kn_audio_fmt_parse(NULL, 40, &f) == -1, "NULL -> -1");
    memcpy(bad, k_pcm16, sizeof k_pcm16);
    bad[4] = bad[5] = bad[6] = bad[7] = 0;          /* rate 0 */
    CHECK(kn_audio_fmt_parse(bad, sizeof bad, &f) == -1, "rate 0 -> -1");
    memcpy(bad, k_pcm16, sizeof k_pcm16);
    bad[2] = 9;                                      /* channels 9 */
    CHECK(kn_audio_fmt_parse(bad, sizeof bad, &f) == -1, "channels 9 -> -1");
    memcpy(bad, k_pcm16, sizeof k_pcm16);
    bad[14] = 12;                                    /* bits 12 (non-byte) */
    CHECK(kn_audio_fmt_parse(bad, sizeof bad, &f) == -1, "bits 12 -> -1");
    memcpy(bad, k_pcm16, sizeof k_pcm16);
    bad[14] = 24;                                    /* 24-bit PCM */
    CHECK(kn_audio_fmt_parse(bad, sizeof bad, &f) == 0 &&
          f.fmt == KN_AFMT_UNKNOWN, "pcm24 -> UNKNOWN (v1 does not render)");

    {
        char d[64];
        kn_audio_fmt_parse(k_mix_f32, sizeof k_mix_f32, &f);
        kn_audio_fmt_describe(&f, d, sizeof d);
        CHECK(strcmp(d, "48000Hz 2ch float32") == 0, "describe exact");
    }
}

static void test_gen(void)
{
    kn_audio_fmt f;
    unsigned char buf[8 * 48];
    double phase;
    unsigned i;
    int nonzero = 0, range_ok = 1;

    /* silence */
    kn_audio_fmt_parse(k_mix_f32, sizeof k_mix_f32, &f);
    memset(buf, 0xAA, sizeof buf);
    phase = 1.234;
    kn_audio_gen(&f, buf, 12, 0, &phase);
    for (i = 0; i < 12 * 8; ++i)
        if (buf[i] != 0) range_ok = 0;
    CHECK(range_ok, "silence is digital zeros (f32)");
    CHECK(fabs(phase - 1.234) < 1e-12, "silence leaves phase untouched");

    /* sine range + non-triviality (f32) */
    memset(buf, 0, sizeof buf);
    phase = 0.0;
    kn_audio_gen(&f, buf, 48, 440, &phase);
    for (i = 0; i + 3 < 48 * 8; i += 4) {
        float v;
        memcpy(&v, buf + i, 4);
        if (v != 0.0f) nonzero = 1;
        if (v < -0.126f || v > 0.126f) range_ok = 0;
    }
    CHECK(nonzero, "sine is not all zeros");
    CHECK(range_ok, "sine amplitude bounded (~-18 dBFS)");

    /* phase continuity: 3 frames + 2 frames == 5 frames in one call */
    {
        unsigned char a[8 * 3], b[8 * 2], c[8 * 5];
        double p1 = 0.7, p2 = 0.7, p3 = 0.7;
        kn_audio_gen(&f, a, 3, 440, &p1);
        kn_audio_gen(&f, b, 2, 440, &p1);
        kn_audio_gen(&f, c, 5, 440, &p3);
        CHECK(memcmp(a, c, sizeof a) == 0 && memcmp(b, c + sizeof a,
                                                    sizeof b) == 0 &&
              fabs(p1 - p3) < 1e-12, "split calls are byte-identical");
        (void)p2;
    }

    /* exact period wrap: 480 Hz @ 48000 = 100 frames/period */
    phase = 0.0;
    kn_audio_gen(&f, buf, 40, 480, &phase);   /* 40 frames -> 0.4 period */
    CHECK(fabs(phase - 0.8 * 3.14159265358979323846) < 1e-9,
          "phase advances exactly 2*pi*f/rate per frame");

    /* s16 range — 48 frames x 2 ch = 96 shorts written; the buffer is
     * sized EXACTLY and poison-filled so any unwritten read would show */
    kn_audio_fmt_parse(k_pcm16, sizeof k_pcm16, &f);
    {
        short sbuf[48 * 2];
        int srange = 1, snon = 0;
        memset(sbuf, 0xAA, sizeof sbuf);
        phase = 0.0;
        kn_audio_gen(&f, (unsigned char *)sbuf, 48, 440, &phase);
        for (i = 0; i < 48 * 2; ++i) {
            if (sbuf[i] != 0) snon = 1;
            if (sbuf[i] < -4100 || sbuf[i] > 4100) srange = 0;
        }
        CHECK(snon && srange, "s16 sine in range");
    }

    /* unknown format is a no-op (no crash, no writes) */
    memset(buf, 0x5A, sizeof buf);
    f.fmt = KN_AFMT_UNKNOWN;
    phase = 0.0;
    kn_audio_gen(&f, buf, 4, 440, &phase);
    CHECK(buf[0] == 0x5A, "unknown format writes nothing");
}

int main(void)
{
    test_fmt_parse();
    test_gen();
    return kn_test_summary("test_knaudio");
}
