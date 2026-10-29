/* test_knogg.c — KAT + contract tests for kn_ogg (OggOpus framing).
 *
 * Golden vectors come from tools/oggopus_reference.py (the independent
 * Python implementation of the same contract — RFC 3533 framing/CRC,
 * RFC 7845 headers, RFC 6716 TOC durations); the generator self-proves
 * every script before emitting kn_ogg_kat_vectors.h. The shared battery
 * lives in kn_audio_rx_kat_runner.h so the SHIPPED binary re-runs the
 * identical vectors via knvstd --selftest=audio.vectors (§7.9).
 * Runs on the Linux host and the Win7 target. */
#include "kn/kn_ogg.h"
#include "kn_audio_rx_kat_runner.h"
#include "test_port.h"

#include <stdio.h>
#include <string.h>

/* Bitwise CRC reference (the definition itself — no table) so the
 * production table is verified, not trusted. */
static uint32_t crc32_bitwise(const unsigned char *p, size_t len)
{
    uint32_t crc = 0;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i] << 24;
        for (b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u)
                                      : (crc << 1);
    }
    return crc;
}

static void test_crc_contract(void)
{
    unsigned char data[300];
    size_t i;
    for (i = 0; i < sizeof data; i++)
        data[i] = (unsigned char)(i * 31u + 7u);
    CHECK(kn_ogg_crc32(data, 0) == 0, "crc.empty");
    for (i = 1; i <= sizeof data; i += 17)
        CHECK(kn_ogg_crc32(data, i) == crc32_bitwise(data, i),
              "crc.matches.bitwise");
}

static void test_toc_contract(void)
{
    static const int expect[32] = {
        480, 960, 1920, 2880, 480, 960, 1920, 2880,
        480, 960, 1920, 2880, 480, 960, 480, 960,
        120, 240, 480, 960, 120, 240, 480, 960,
        120, 240, 480, 960, 120, 240, 480, 960
    };
    unsigned cfg;
    for (cfg = 0; cfg < 32; cfg++) {
        unsigned char p[2];
        p[0] = (unsigned char)(cfg << 3);
        p[1] = 0x55;
        CHECK(kn_opus_packet_samples48k(p, 2) == expect[cfg],
              "toc.config");
        p[0] = (unsigned char)((cfg << 3) | 1);
        CHECK(kn_opus_packet_samples48k(p, 2) == 2 * expect[cfg],
              "toc.code1");
        p[0] = (unsigned char)((cfg << 3) | 2);
        CHECK(kn_opus_packet_samples48k(p, 2) == 2 * expect[cfg],
              "toc.code2");
        p[0] = (unsigned char)((cfg << 3) | 3);
        p[1] = 7;
        CHECK(kn_opus_packet_samples48k(p, 2) == 7 * expect[cfg],
              "toc.code3");
    }
    CHECK(kn_opus_packet_samples48k(NULL, 0) == -1, "toc.null");
}

static void test_muxer_contract(void)
{
    kn_ogg_mux m;
    static uint8_t scratch[KN_OGG_PAGE_BUF];
    memset(&m, 0, sizeof m);
    CHECK(kn_ogg_mux_init(NULL, scratch, sizeof scratch, 1, 2,
                          k_ogg_sink_write, &k_ogg_sink) == -1,
          "mux.init.null");
    CHECK(kn_ogg_mux_init(&m, scratch, 100, 1, 2, k_ogg_sink_write,
                          &k_ogg_sink) == -1, "mux.init.smallbuf");
    CHECK(kn_ogg_mux_init(&m, scratch, sizeof scratch, 1, 0,
                          k_ogg_sink_write, &k_ogg_sink) == -1,
          "mux.init.channels0");
    CHECK(kn_ogg_mux_packet(&m, (const unsigned char *)"x", 1, 960) == -1,
          "mux.packet.before_begin");
    CHECK(kn_ogg_mux_finish(&m) == -1, "mux.finish.before_begin");
    CHECK(kn_ogg_mux_init(&m, scratch, sizeof scratch, KN_OGG_KAT_SERIAL,
                          2, k_ogg_sink_write, &k_ogg_sink) == 0,
          "mux.init.ok");
    memset(&k_ogg_sink, 0, sizeof k_ogg_sink);
    CHECK(kn_ogg_mux_begin(&m) == 0, "mux.begin.ok");
    CHECK(kn_ogg_mux_begin(&m) == -1, "mux.begin.twice");
    CHECK(k_ogg_sink.len > 27 && memcmp(k_ogg_sink.buf, "OggS", 4) == 0,
          "mux.bos.magic");
    CHECK((k_ogg_sink.buf[5] & 0x02) != 0, "mux.bos.flag");
    CHECK(memcmp(k_ogg_sink.buf + 28, "OpusHead", 8) == 0,
          "mux.opushead");   /* 27-byte page header + 1 lacing byte */
    CHECK(kn_ogg_mux_finish(&m) == 0, "mux.finish.ok");
    CHECK(kn_ogg_mux_finish(&m) == 0, "mux.finish.idempotent");
}

static void test_kat_battery(void)
{
    kn_audio_tally t;
    memset(&t, 0, sizeof t);
    kn_ogg_kat_run_all(&t, 1);
    printf("  kn_ogg KAT battery: %d checks, %d failures\n",
           t.checks, t.failures);
    if (t.failures)
        printf("  first failing vector: %s\n", t.first_fail);
    CHECK(t.failures == 0, "ogg.kat.battery");
    CHECK(t.checks >= 100, "ogg.kat.coverage");
}

int main(void)
{
    test_crc_contract();
    test_toc_contract();
    test_muxer_contract();
    test_kat_battery();
    return kn_test_summary("test_knogg");
}
