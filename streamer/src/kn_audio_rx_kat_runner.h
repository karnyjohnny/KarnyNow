/* kn_audio_rx_kat_runner.h — shared KNOWN-ANSWER battery runner for the
 * stage-3 audio pair: kn_ogg (OggOpus framing) + kn_arx (bundle-audio
 * SRTP receive/extract).
 *
 * Consumed by BOTH:
 *   - streamer/tests/test_knogg.c / test_knarx.c (host/target unit
 *     suites; verbose)
 *   - streamer/src/kn_selftest.c (the §7.9 "audio.vectors" self-test
 *     that re-runs the battery from the SHIPPED binary; silent —
 *     knvstd's stdout is a JSON-lines contract)
 *
 * Vectors: streamer/src/kn_ogg_kat_vectors.h (tools/oggopus_reference.py)
 * and streamer/src/kn_arx_kat_vectors.h (tools/arx_reference.py) — the
 * independent Python implementations self-prove before emitting, so the
 * C battery compares against a second implementation, not against
 * itself (two-implementations rule).
 *
 * All functions are static; every section accumulates (checks, failures)
 * into a kn_audio_tally and records the first failing vector name.
 * Nothing aborts on failure — the battery always runs to the end (the
 * "execute as much as possible" test contract, user instruction
 * 2026-10-24). */
#ifndef KN_AUDIO_RX_KAT_RUNNER_H
#define KN_AUDIO_RX_KAT_RUNNER_H

#include "kn/kn_ogg.h"
#include "kn/kn_arx.h"
#include "kn_ogg_kat_vectors.h"
#include "kn_arx_kat_vectors.h"

#include <stdio.h>
#include <string.h>

typedef struct kn_audio_tally {
    int  checks;
    int  failures;
    char first_fail[96];
} kn_audio_tally;

#define KAT(t, cond, name)                                                \
    do {                                                                  \
        (t).checks++;                                                     \
        if (!(cond)) {                                                    \
            (t).failures++;                                               \
            if (!(t).first_fail[0])                                       \
                snprintf((t).first_fail, sizeof (t).first_fail,           \
                         "%s", (name));                                   \
        }                                                                 \
    } while (0)

#if defined(__GNUC__)
#  define K_AUDIO_UNUSED __attribute__((unused))
#else
#  define K_AUDIO_UNUSED
#endif

#define KATV(verbose, cond, ...)                                          \
    do {                                                                  \
        if (verbose) {                                                    \
            printf("%s ", (cond) ? "PASS" : "FAIL");                      \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------ ogg sink */

#define K_AUDIO_SINK_CAP 65536u

typedef struct k_audio_sink {
    unsigned char buf[K_AUDIO_SINK_CAP];
    size_t len;
    int    err;
} k_audio_sink;

K_AUDIO_UNUSED static int k_ogg_sink_write(void *ctx, const uint8_t *data, size_t len)
{
    k_audio_sink *s = (k_audio_sink *)ctx;
    if (s->len + len > sizeof s->buf) {
        s->err = 1;
        return -1;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    return 0;
}

K_AUDIO_UNUSED static uint8_t k_ogg_scratch[KN_OGG_PAGE_BUF];
K_AUDIO_UNUSED static k_audio_sink k_ogg_sink;

K_AUDIO_UNUSED static int kn_ogg_kat_run_all(kn_audio_tally *t, int verbose)
{
    unsigned i;
    char nm[96];

    /* CRC vectors */
    for (i = 0; i < (unsigned)KN_OGG_CRC_VECS; i++) {
        const kn_ogg_crc_vec *v = &kn_ogg_crc_vecs[i];
        uint32_t crc = kn_ogg_crc32(v->in, v->in_len);
        snprintf(nm, sizeof nm, "ogg.crc.%s", v->name);
        KAT(*t, crc == v->crc, nm);
        KATV(verbose, crc == v->crc, "ogg.crc.%s = %08x", v->name,
             (unsigned)crc);
    }
    /* TOC duration vectors */
    for (i = 0; i < (unsigned)KN_OGG_TOC_VECS; i++) {
        const kn_ogg_toc_vec *v = &kn_ogg_toc_vecs[i];
        int s = kn_opus_packet_samples48k(v->pkt, v->pkt_len);
        snprintf(nm, sizeof nm, "ogg.toc[%u]", i);
        KAT(*t, s == v->samples48, nm);
    }
    KATV(verbose, t->failures == 0, "ogg.toc %d vectors", KN_OGG_TOC_VECS);
    /* muxer scripts — byte-exact full-stream comparison */
    for (i = 0; i < (unsigned)KN_OGG_SCRIPTS; i++) {
        const kn_ogg_script *sc = &kn_ogg_scripts[i];
        kn_ogg_mux m;
        unsigned p;
        int rc = 0;
        memset(&k_ogg_sink, 0, sizeof k_ogg_sink);
        rc |= kn_ogg_mux_init(&m, k_ogg_scratch, sizeof k_ogg_scratch,
                              KN_OGG_KAT_SERIAL, KN_OGG_KAT_CHANNELS,
                              k_ogg_sink_write, &k_ogg_sink);
        rc |= kn_ogg_mux_begin(&m);
        for (p = 0; p < sc->n_pkts && rc == 0; p++)
            rc |= kn_ogg_mux_packet(&m, sc->pkts[p].pkt, sc->pkts[p].len,
                                    sc->pkts[p].samples48);
        if (rc == 0)
            rc |= kn_ogg_mux_finish(&m);
        snprintf(nm, sizeof nm, "ogg.script.%s.rc", sc->name);
        KAT(*t, rc == 0 && k_ogg_sink.err == 0, nm);
        snprintf(nm, sizeof nm, "ogg.script.%s.bytes", sc->name);
        KAT(*t, k_ogg_sink.len == sc->stream_len &&
                memcmp(k_ogg_sink.buf, sc->stream, sc->stream_len) == 0,
            nm);
        snprintf(nm, sizeof nm, "ogg.script.%s.stats", sc->name);
        KAT(*t, m.pages_out == sc->pages && m.bytes_out == sc->bytes_out &&
                m.dropped_packets == 0 && m.write_errors == 0, nm);
        KATV(verbose, t->failures == 0, "ogg.script.%s: %u pages, %u bytes",
             sc->name, (unsigned)m.pages_out, (unsigned)k_ogg_sink.len);
    }
    /* contract edges: oversized packet is an honest drop, not an abort;
     * finish() is idempotent; a lone finish after a full page emits the
     * empty EOS page */
    {
        kn_ogg_mux m;
        static uint8_t big[KN_OGG_PAGE_PAYLOAD_MAX + 1];
        memset(&k_ogg_sink, 0, sizeof k_ogg_sink);
        big[0] = 0xF8;
        kn_ogg_mux_init(&m, k_ogg_scratch, sizeof k_ogg_scratch,
                        KN_OGG_KAT_SERIAL, 2, k_ogg_sink_write, &k_ogg_sink);
        kn_ogg_mux_begin(&m);
        KAT(*t, kn_ogg_mux_packet(&m, big, sizeof big, 960) == 0 &&
                m.dropped_packets == 1, "ogg.oversize.drop");
        KAT(*t, kn_ogg_mux_finish(&m) == 0, "ogg.finish.rc");
        {
            size_t len_after_first = k_ogg_sink.len;
            KAT(*t, kn_ogg_mux_finish(&m) == 0 &&
                    k_ogg_sink.len == len_after_first, "ogg.finish.idem");
        }
        KATV(verbose, t->failures == 0, "ogg.contract edges");
    }
    return t->failures;
}

/* ------------------------------------------------------- arx capture */

typedef struct k_arx_capture {
    unsigned char buf[2048];
    size_t  len;
    uint32_t samples;
    int     fired;
} k_arx_capture;

K_AUDIO_UNUSED static void k_arx_emit(void *ctx, const uint8_t *payload, size_t len,
                       uint32_t samples48)
{
    k_arx_capture *c = (k_arx_capture *)ctx;
    c->fired = 1;
    c->samples = samples48;
    c->len = len < sizeof c->buf ? len : sizeof c->buf;
    memcpy(c->buf, payload, c->len);
}

K_AUDIO_UNUSED static int k_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

K_AUDIO_UNUSED static int kn_arx_kat_run_all(kn_audio_tally *t, int verbose)
{
    kn_arx a;
    k_arx_capture cap;
    uint8_t enc[16], auth[20], salt[14];
    unsigned gi, pi;
    char nm[96];

    /* derived session keys (RFC 3711 KDF over the vector master pair) */
    kn_arx_init(&a, k_arx_emit, &cap);
    KAT(*t, kn_arx_set_srtp(&a, kn_arx_master_key, kn_arx_master_salt) == 0,
        "arx.set_srtp.rc");
    kn_arx_session_keys(&a, enc, auth, salt);
    KAT(*t, memcmp(enc, kn_arx_sess_enc, 16) == 0, "arx.kdf.enc");
    KAT(*t, memcmp(auth, kn_arx_sess_auth, 20) == 0, "arx.kdf.auth");
    KAT(*t, memcmp(salt, kn_arx_sess_salt, 14) == 0, "arx.kdf.salt");
    KATV(verbose, t->failures == 0, "arx.kdf session keys");

    for (gi = 0; gi < (unsigned)KN_ARX_GROUPS; gi++) {
        const kn_arx_group *g = &kn_arx_groups[gi];
        int group_ok = 1;
        kn_arx_init(&a, k_arx_emit, &cap);
        if (strcmp(g->name, "no_key") != 0)
            kn_arx_set_srtp(&a, kn_arx_master_key, kn_arx_master_salt);
        for (pi = 0; pi < g->n_pkts; pi++) {
            const kn_arx_pkt_vec *v = &g->pkts[pi];
            size_t hlen = strlen(v->emit_hex);
            int rc = kn_arx_process(&a, v->pkt, v->pkt_len,
                                    1000u + pi * 20u);
            int ok = (rc == v->expect_rc);
            /* re-run for the capture (process is deterministic; the
             * first call already updated the stats we compare below) */
            if (ok) {
                kn_arx b;
                unsigned k;
                /* capture pass on a twin state: replay the group prefix
                 * on a fresh twin, RESET the capture, then run p_i —
                 * cap holds exactly p_i's emission */
                kn_arx_init(&b, k_arx_emit, &cap);
                if (strcmp(g->name, "no_key") != 0)
                    kn_arx_set_srtp(&b, kn_arx_master_key,
                                    kn_arx_master_salt);
                for (k = 0; k < pi; k++)
                    kn_arx_process(&b, g->pkts[k].pkt,
                                   g->pkts[k].pkt_len, 1000u + k * 20u);
                memset(&cap, 0, sizeof cap);
                kn_arx_process(&b, v->pkt, v->pkt_len, 1000u + pi * 20u);
                ok = (cap.fired != 0) == (hlen != 0);
                if (ok && hlen) {
                    size_t j;
                    ok = cap.len == hlen / 2 && cap.samples ==
                         v->emit_samples48;
                    for (j = 0; ok && j < cap.len; j++) {
                        int hi = k_hexval(v->emit_hex[2 * j]);
                        int lo = k_hexval(v->emit_hex[2 * j + 1]);
                        ok = hi >= 0 && lo >= 0 &&
                             cap.buf[j] == (unsigned char)((hi << 4) | lo);
                    }
                }
                if (ok && hlen == 0)
                    ok = cap.samples == v->emit_samples48;
            }
            snprintf(nm, sizeof nm, "arx.%s.p%u", g->name, pi);
            KAT(*t, ok, nm);
            if (!ok)
                group_ok = 0;
        }
        /* final counters — the 17-field contract */
        {
            int cok = a.st.consumed == g->consumed &&
                      a.st.packets == g->packets &&
                      a.st.payload_bytes == g->payload_bytes &&
                      a.st.red_packets == g->red_packets &&
                      a.st.red_blocks == g->red_blocks &&
                      a.st.red_invalid == g->red_invalid &&
                      a.st.pt_filtered == g->pt_filtered &&
                      a.st.malformed == g->malformed &&
                      a.st.toc_invalid == g->toc_invalid &&
                      a.st.padded == g->padded &&
                      a.st.seq_loss == g->seq_loss &&
                      a.st.seq_reorder == g->seq_reorder &&
                      a.st.seq_dup == g->seq_dup &&
                      a.st.ssrc_changes == g->ssrc_changes &&
                      a.st.srtp_fail == g->srtp_fail &&
                      a.st.replay_reject == g->replay_reject &&
                      a.st.srtp_nokey == g->srtp_nokey;
            snprintf(nm, sizeof nm, "arx.%s.counters", g->name);
            KAT(*t, cok, nm);
            if (!cok)
                group_ok = 0;
            /* emission timestamps only on emitting groups */
            if (g->packets) {
                KAT(*t, a.st.first_ms == 1000u &&
                        a.st.last_ms >= a.st.first_ms, "arx.ms");
            }
        }
        KATV(verbose, group_ok, "arx.group.%s (%u pkts)", g->name,
             g->n_pkts);
    }
    return t->failures;
}

K_AUDIO_UNUSED static int kn_audio_kat_run_all(kn_audio_tally *t, int verbose)
{
    kn_ogg_kat_run_all(t, verbose);
    kn_arx_kat_run_all(t, verbose);
    return t->failures;
}

#endif /* KN_AUDIO_RX_KAT_RUNNER_H */
