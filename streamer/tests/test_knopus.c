/* test_knopus.c — wrapper-contract + KAT tests for kn_opus (F21).
 *
 * The vectors come from tools/opus_reference.py, which encodes AND
 * decodes them through an INDEPENDENT path (libopus via Python ctypes,
 * separate encoder/decoder states, self-proved before emitting). They
 * form ONE sequential Opus stream — 40 warmup packets, then per content
 * vector 10 transition packets + the pinned packet, then the garbage
 * reject — because that is the production shape: F21 feeds ONE decoder
 * per session and the CELT postfilter makes packet N's output depend on
 * the decoder's history. The C side therefore walks the same sequence
 * with ONE kn_opus decoder and compares the content vectors sample by
 * sample (s16 quantisation, tolerance 1 LSB: libopus versions may differ
 * in the last ulp and lrintf vs floor(x+.5) disagree on exact half-LSB
 * boundaries; a 2-LSB gap would mean a REAL contract break).
 *
 * In builds without libopus (KN_HAVE_OPUS=0: the cross-compile legs,
 * opus-less POSIX) the suite still runs and PASSES on the stub
 * contract: available()==0, create NULL with a reason, decode -1,
 * version "none". The vector battery self-skips with a printed note —
 * the same honesty pattern as the degraded selftests.
 * Runs on the Linux host and the Win7 target. */
#include "kn/kn_opus.h"
#include "kn_opus_kat_vectors.h"
#include "test_port.h"

#include <math.h>
#include <stdio.h>

#define KN_OPUS_TEST_CAP 5760u             /* 120 ms at 48 kHz, per channel */

static float g_pcm[KN_OPUS_TEST_CAP * 2u];

/* The exact quantisation kn_audio_xform_f32 promises for S16 devices —
 * the vectors are compared through it so the playback path and the test
 * share ONE rounding rule. */
static short q16f(float v)
{
    if (!(v == v))
        return 0;
    if (v > 1.0f)
        return 32767;
    if (v < -1.0f)
        return -32768;
    return (short)lrintf(v * 32767.0f);
}

static void test_stub_or_real_contract(void)
{
    char err[128];
    kn_opus_dec *d;

    CHECK(kn_opus_version() != NULL, "opus.version.nonnull");
    if (!kn_opus_available()) {
        CHECK(strcmp(kn_opus_version(), "none") == 0, "opus.stub.version");
        err[0] = '\0';
        d = kn_opus_decoder_create(48000, 2, err, sizeof err);
        CHECK(d == NULL, "opus.stub.create_null");
        CHECK(err[0] != '\0', "opus.stub.create_reason");
        CHECK(kn_opus_decode_f32(NULL, (const unsigned char *)"x", 1, g_pcm,
                                 KN_OPUS_TEST_CAP) == -1, "opus.stub.decode");
        kn_opus_decoder_destroy(NULL);           /* must not crash */
        CHECK(1, "opus.stub.destroy_null_safe");
        return;
    }
    /* real body: argument discipline */
    err[0] = '\0';
    CHECK(kn_opus_decoder_create(0, 2, err, sizeof err) == NULL,
          "opus.create.rate0");
    CHECK(err[0] != '\0', "opus.create.rate0_reason");
    err[0] = '\0';
    CHECK(kn_opus_decoder_create(48000, 3, err, sizeof err) == NULL,
          "opus.create.ch3");
    CHECK(err[0] != '\0', "opus.create.ch3_reason");
    d = kn_opus_decoder_create(KN_OPUS_KAT_RATE, KN_OPUS_KAT_CH, err,
                               sizeof err);
    CHECK(d != NULL, "opus.create.ok");
    if (!d)
        return;
    CHECK(kn_opus_decode_f32(d, NULL, 4, g_pcm, KN_OPUS_TEST_CAP) == -1,
          "opus.decode.null_pkt");
    CHECK(kn_opus_decode_f32(d, kn_opus_vec0_pkt, 0, g_pcm,
                             KN_OPUS_TEST_CAP) == -1, "opus.decode.zero_len");
    CHECK(kn_opus_decode_f32(d, kn_opus_vec0_pkt, kn_opus_kat[0].pkt_len,
                             g_pcm, 100) == -1, "opus.decode.small_cap");
    CHECK(kn_opus_decode_f32(NULL, kn_opus_vec0_pkt,
                             kn_opus_kat[0].pkt_len, g_pcm,
                             KN_OPUS_TEST_CAP) == -1, "opus.decode.null_dec");
    kn_opus_decoder_destroy(d);
    kn_opus_decoder_destroy(NULL);
    CHECK(1, "opus.destroy_null_safe");
}

static void test_kat_stream(void)
{
    kn_opus_dec *d;
    size_t i;
    int maxdiff = 0;
    unsigned content = 0, warmups = 0;

    if (!kn_opus_available()) {
        printf("  kn_opus KAT battery: SKIPPED (no libopus in this build)\n");
        CHECK(1, "opus.kat.skipped_stub");
        return;
    }
    /* the garbage vector rides its own cold decoder (last entry) */
    d = kn_opus_decoder_create(KN_OPUS_KAT_RATE, KN_OPUS_KAT_CH, NULL, 0);
    CHECK(d != NULL, "opus.kat.create");
    if (!d)
        return;
    for (i = 0; i < KN_OPUS_KAT_N; ++i) {
        const kn_opus_kat_vec *v = &kn_opus_kat[i];
        int n;

        if (v->expect_frames < 0) {
            /* reject vector: fresh decoder, wrapper must say -1 */
            kn_opus_dec *g = kn_opus_decoder_create(KN_OPUS_KAT_RATE,
                                                    KN_OPUS_KAT_CH, NULL, 0);
            CHECK(g != NULL, "opus.kat.garbage_create");
            n = g ? kn_opus_decode_f32(g, v->pkt, v->pkt_len, g_pcm,
                                       KN_OPUS_TEST_CAP) : 0;
            CHECK(n == -1, "opus.kat.garbage_reject");
            kn_opus_decoder_destroy(g);
            continue;
        }
        n = kn_opus_decode_f32(d, v->pkt, v->pkt_len, g_pcm,
                               KN_OPUS_TEST_CAP);
        if (n != v->expect_frames) {
            printf("  vector %u (%s): frames %d != %d\n", (unsigned)i,
                   v->expect_s16 ? "content" : "warmup", n,
                   v->expect_frames);
        }
        CHECK(n == v->expect_frames, "opus.kat.frames");
        if (v->expect_s16 && n > 0) {
            unsigned s;
            int bad = 0;
            for (s = 0; s < v->expect_samples; ++s) {
                int got = q16f(g_pcm[s]);
                int diff = got - v->expect_s16[s];
                if (diff < 0)
                    diff = -diff;
                if (diff > maxdiff)
                    maxdiff = diff;
                if (diff > 1)
                    bad = 1;
            }
            if (bad)
                printf("  vector %u: s16 mismatch beyond 1 LSB\n",
                       (unsigned)i);
            CHECK(!bad, "opus.kat.samples");
            content++;
        } else {
            warmups++;
        }
    }
    kn_opus_decoder_destroy(d);
    printf("  kn_opus KAT battery: %u vectors (%u content, %u warmup), "
           "max |diff| %d LSB\n", (unsigned)KN_OPUS_KAT_N, content, warmups,
           maxdiff);
    CHECK(content == 4, "opus.kat.coverage_content");
    CHECK(warmups >= 40, "opus.kat.coverage_warmup");
    CHECK(maxdiff <= 1, "opus.kat.tolerance");
}

int main(void)
{
    test_stub_or_real_contract();
    test_kat_stream();
    return kn_test_summary("test_knopus");
}
