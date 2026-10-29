/* test_knarx.c — KAT + property tests for kn_arx (bundle-audio SRTP
 * receive/extract).
 *
 * Golden vectors come from tools/arx_reference.py (the independent
 * Python implementation of the kn_arx contract — RFC 3711
 * AES_CM_128_HMAC_SHA1_80, RFC 2198 RED, RFC 3550 seq accounting); the
 * generator self-proves every vector before emitting
 * kn_arx_kat_vectors.h. The property leg re-derives the same round trip
 * through knsrtp's OWN protect() so kn_arx and kn_srtp cross-check each
 * other on the host without any Python in the loop. The shared battery
 * lives in kn_audio_rx_kat_runner.h so the SHIPPED binary re-runs the
 * identical vectors via knvstd --selftest=audio.vectors (§7.9).
 * Runs on the Linux host and the Win7 target. */
#include "kn/kn_arx.h"
#include "kn/kn_ogg.h"
#include "kn/knsrtp.h"
#include "kn_audio_rx_kat_runner.h"
#include "test_port.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------- property round trip */

typedef struct prop_ctx {
    unsigned char last[2048];
    size_t  last_len;
    uint32_t last_samples;
    int     fired;
} prop_ctx;

static void prop_emit(void *ctx, const uint8_t *payload, size_t len,
                      uint32_t samples48)
{
    prop_ctx *p = (prop_ctx *)ctx;
    p->fired = 1;
    p->last_samples = samples48;
    p->last_len = len < sizeof p->last ? len : sizeof p->last;
    memcpy(p->last, payload, p->last_len);
}

/* Build one SRTP(AES_CM_128_HMAC_SHA1_80) packet with knsrtp's own
 * protect(), mirroring tools/arx_reference.py's builder. */
static size_t build_srtp_pkt(unsigned char *out, size_t outcap,
                             unsigned seq, unsigned ts, unsigned ssrc,
                             unsigned pt, const unsigned char *payload,
                             size_t paylen,
                             const uint8_t enc[16], const uint8_t auth[20],
                             const uint8_t salt[14])
{
    size_t hlen = 12;
    if (outcap < hlen + paylen + 10)
        return 0;
    out[0] = 0x80;                       /* V=2, no P/X/CC */
    out[1] = (unsigned char)(pt & 0x7F);
    out[2] = (unsigned char)(seq >> 8);
    out[3] = (unsigned char)(seq & 0xFF);
    out[4] = (unsigned char)(ts >> 24);
    out[5] = (unsigned char)(ts >> 16);
    out[6] = (unsigned char)(ts >> 8);
    out[7] = (unsigned char)(ts & 0xFF);
    out[8]  = (unsigned char)(ssrc >> 24);
    out[9]  = (unsigned char)(ssrc >> 16);
    out[10] = (unsigned char)(ssrc >> 8);
    out[11] = (unsigned char)(ssrc & 0xFF);
    memcpy(out + hlen, payload, paylen);
    if (kn_srtp_protect_cm_hmac(out, hlen + paylen, hlen, ssrc,
                                kn_srtp_packet_index(0, (uint16_t)seq),
                                enc, auth, salt, 10) != 0)
        return 0;
    return hlen + paylen + 10;
}

static void test_property_roundtrip(void)
{
    kn_arx a;
    prop_ctx pc;
    uint8_t enc[16], auth[20], salt[14];
    unsigned char wire[2048];
    unsigned i;
    int ok = 1;

    kn_arx_init(&a, prop_emit, &pc);
    CHECK(kn_arx_set_srtp(&a, kn_arx_master_key, kn_arx_master_salt) == 0,
          "prop.set_srtp");
    kn_arx_session_keys(&a, enc, auth, salt);

    /* 24 sequential packets carrying distinct payloads */
    for (i = 0; i < 24; i++) {
        unsigned char opus[160];
        size_t n;
        int rc;
        memset(&pc, 0, sizeof pc);
        opus[0] = 0xF8;                       /* CELT FB 20 ms, code 0 */
        memset(opus + 1, (int)(0x10 + i), 40 + i);
        n = build_srtp_pkt(wire, sizeof wire, 1000 + i, 48000 + i * 960,
                           0x5EED0001u, KN_ARX_PT_OPUS, opus, 41 + i,
                           enc, auth, salt);
        CHECK(n > 0, "prop.build");
        rc = kn_arx_process(&a, wire, n, 5000u + i * 20u);
        ok = rc == 1 && pc.fired && pc.last_len == 41 + i &&
             memcmp(pc.last, opus, pc.last_len) == 0 &&
             pc.last_samples == 960;
        CHECK(ok, "prop.roundtrip");
        if (!ok)
            break;
    }
    CHECK(a.st.packets == 24 && a.st.seq_loss == 0 &&
          a.st.seq_reorder == 0 && a.st.srtp_fail == 0, "prop.counters");
    CHECK(a.st.first_ms == 5000u && a.st.last_ms == 5000u + 23u * 20u,
          "prop.timestamps");

    /* a RED packet built in-test: one redundant block + the primary */
    {
        unsigned char red[512], prev[80], cur[80];
        size_t n;
        int rc;
        memset(&pc, 0, sizeof pc);
        prev[0] = 0xF8; memset(prev + 1, 0xA1, 49);   /* 50 bytes */
        cur[0]  = 0xF8; memset(cur + 1, 0xB2, 59);    /* 60 bytes */
        red[0] = 0x80 | KN_ARX_PT_OPUS;               /* F=1 header */
        red[1] = (960 >> 6) & 0xFF;
        red[2] = (unsigned char)(((960 & 0x3F) << 2) | (50 >> 8));
        red[3] = 50 & 0xFF;
        red[4] = KN_ARX_PT_OPUS;                      /* F=0 primary */
        memcpy(red + 5, prev, 50);
        memcpy(red + 55, cur, 60);
        n = build_srtp_pkt(wire, sizeof wire, 2000, 96000, 0x5EED0001u,
                           KN_ARX_PT_RED, red, 115, enc, auth, salt);
        CHECK(n > 0, "prop.red.build");
        rc = kn_arx_process(&a, wire, n, 9000u);
        CHECK(rc == 1 && pc.fired && pc.last_len == 60 &&
              memcmp(pc.last, cur, 60) == 0, "prop.red.primary");
        CHECK(a.st.red_packets == 1 && a.st.red_blocks == 1 &&
              a.st.red_invalid == 0, "prop.red.counters");
    }

    /* NULL emit is legal: packets still counted, nothing delivered */
    {
        kn_arx b;
        size_t n;
        kn_arx_init(&b, NULL, NULL);
        kn_arx_set_srtp(&b, kn_arx_master_key, kn_arx_master_salt);
        kn_arx_session_keys(&b, enc, auth, salt);
        n = build_srtp_pkt(wire, sizeof wire, 7, 0, 0x5EED0002u,
                           KN_ARX_PT_OPUS, (const unsigned char *)"\xF8"
                           "zzzzzzzzz", 10, enc, auth, salt);
        CHECK(kn_arx_process(&b, wire, n, 1u) == 1 && b.st.packets == 1,
              "prop.null_emit");
    }
}

static void test_contract_edges(void)
{
    kn_arx a;
    prop_ctx pc;
    unsigned char tiny[4] = { 0x80, 0x6F, 0x00, 0x01 };
    kn_arx_init(NULL, prop_emit, &pc);            /* must not crash */
    kn_arx_init(&a, prop_emit, &pc);
    CHECK(kn_arx_process(NULL, tiny, sizeof tiny, 0) == -1, "edge.null");
    CHECK(kn_arx_process(&a, NULL, 0, 0) == -1, "edge.null_pkt");
    CHECK(kn_arx_set_srtp(NULL, kn_arx_master_key,
                          kn_arx_master_salt) == -1, "edge.set_null");
    CHECK(kn_arx_process(&a, tiny, sizeof tiny, 0) == -1 &&
          a.st.malformed == 1, "edge.short");
    CHECK(kn_arx_process(&a, tiny, 0, 0) == -1 &&
          a.st.malformed == 2, "edge.empty");
}

static void test_kat_battery(void)
{
    kn_audio_tally t;
    memset(&t, 0, sizeof t);
    kn_arx_kat_run_all(&t, 1);
    printf("  kn_arx KAT battery: %d checks, %d failures\n",
           t.checks, t.failures);
    if (t.failures)
        printf("  first failing vector: %s\n", t.first_fail);
    CHECK(t.failures == 0, "arx.kat.battery");
    CHECK(t.checks >= 40, "arx.kat.coverage");
}

int main(void)
{
    test_contract_edges();
    test_property_roundtrip();
    test_kat_battery();
    return kn_test_summary("test_knarx");
}
