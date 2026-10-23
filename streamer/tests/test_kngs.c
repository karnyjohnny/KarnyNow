/* test_kngs.c — known-answer + property tests for the NVST RTP/GS parse
 * layer (kn_gs). Golden vectors come from tools/gs_reference.py (independent
 * Python port of nvst.rs @ a49b124 — line references in docs/GS.md). The
 * property tests re-build random valid packets IN C (a second, independent
 * encoder) and require the parser to recover every field exactly — an
 * endianness or offset mistake cannot survive both the Python goldens and
 * the C round-trips. Runs on the Linux host and the Win7 target. */
#include "kn/kn_gs.h"
#include "kn_gs_kat_vectors.h"
#include "test_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- C-side builders */

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}
static void put_be32(unsigned char *p, uint32_t v)
{
    p[3] = (unsigned char)(v & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[0] = (unsigned char)((v >> 24) & 0xFF);
}

static void build_gs_ext(unsigned char out16[16], uint32_t spi, uint32_t fi,
                         unsigned flags, unsigned cur, unsigned last,
                         unsigned pct, unsigned fidx, unsigned fsrc,
                         unsigned low8, unsigned r9, unsigned r10)
{
    uint32_t pw = ((spi & KN_GS_INDEX_MASK) << 8) | (low8 & 0xFFu);
    uint32_t fw = (flags & 0x0Fu) | ((r9 & 0xFFu) << 8) | ((r10 & 0xFFu) << 16)
                | ((uint32_t)(((last & 3u) << 6) | ((cur & 3u) << 4)) << 24);
    uint32_t few = ((pct & 0xFFu) << 4) | ((fidx & 0x3FFu) << 12)
                 | ((fsrc & 0x3FFu) << 22);
    put_le32(out16 + 0, pw);
    put_le32(out16 + 4, fi);
    put_le32(out16 + 8, fw);
    put_le32(out16 + 12, few);
}

/* Full RTP packet with a GS extension; returns the packet length. buf must
 * have room for 12 + 4*ncsrc + 20 + paylen + pad. */
static size_t build_rtp(unsigned char *buf, unsigned pt, unsigned seq,
                        uint32_t ts, uint32_t ssrc, unsigned marker,
                        const uint32_t *csrc, unsigned ncsrc,
                        const unsigned char ext16[16],
                        const unsigned char *pay, size_t paylen,
                        unsigned pad_count)
{
    size_t o = 0;
    unsigned i;
    buf[o++] = (unsigned char)(0x80u | (pad_count ? 0x20u : 0u) | 0x10u
                               | (ncsrc & 0x0Fu));
    buf[o++] = (unsigned char)((marker ? 0x80u : 0u) | (pt & 0x7Fu));
    buf[o++] = (unsigned char)(seq >> 8);
    buf[o++] = (unsigned char)(seq & 0xFF);
    put_be32(buf + o, ts); o += 4;
    put_be32(buf + o, ssrc); o += 4;
    for (i = 0; i < ncsrc; ++i) { put_be32(buf + o, csrc[i]); o += 4; }
    buf[o++] = 0x47; buf[o++] = 0x53;          /* profile 0x4753 (BE) */
    buf[o++] = 0x00; buf[o++] = 0x04;          /* words = 4 -> exactly 16 B */
    memcpy(buf + o, ext16, 16); o += 16;
    if (paylen) { memcpy(buf + o, pay, paylen); o += paylen; }
    if (pad_count) {
        memset(buf + o, 0, pad_count - 1);
        buf[o + pad_count - 1] = (unsigned char)pad_count;
        o += pad_count;
    }
    return o;
}

/* Deterministic PRNG (xorshift32) — same sequence on every platform. */
static uint32_t t_rng;
static uint32_t trnd(void)
{
    t_rng ^= t_rng << 13;
    t_rng ^= t_rng >> 17;
    t_rng ^= t_rng << 5;
    return t_rng;
}

/* ------------------------------------------------------------ vector runs */

static void test_rtp_vectors(void)
{
    int i;
    for (i = 0; i < K_KAT_RTP_N; ++i) {
        const kn_gs_kat_rtp *v = &k_kat_rtp[i];
        kn_gs_rtp h;
        int rc = kn_gs_rtp_parse(v->pkt, v->len, &h);
        char nm[96];
        snprintf(nm, sizeof nm, "rtp[%s]", v->name);
        if (v->rc != KN_GS_OK) {
            CHECK(rc == v->rc, nm);
            continue;
        }
        CHECK(rc == KN_GS_OK &&
              h.pt == v->pt && h.seq == v->seq &&
              h.timestamp == (uint32_t)v->ts && h.ssrc == (uint32_t)v->ssrc &&
              h.payload_off == v->payload_off &&
              h.payload_len == v->payload_len &&
              h.has_padding == v->has_padding && h.has_gs == v->has_gs &&
              (v->has_gs == 0 || h.gs_off == v->gs_off), nm);
    }
}

static void test_video_vectors(void)
{
    int i;
    for (i = 0; i < K_KAT_VIDEO_N; ++i) {
        const kn_gs_kat_video *v = &k_kat_video[i];
        kn_gs_video g;
        char nm[96];
        snprintf(nm, sizeof nm, "video[%s]", v->name);
        CHECK(kn_gs_video_parse(v->ext, &g) == KN_GS_OK &&
              g.stream_packet_index == v->spi &&
              g.frame_index == v->frame_index &&
              g.flags == v->flags &&
              g.fec_current_block == v->cur &&
              g.fec_last_block == v->last &&
              g.fec_percentage == v->pct &&
              g.fec_index == v->fidx &&
              g.fec_source_packets == v->fsrc &&
              g.is_fec == v->is_fec &&
              g.is_start_of_frame == v->sof &&
              g.is_end_of_frame == v->eof &&
              g.flags_valid == v->flags_valid, nm);
    }
}

static void test_payload_vectors(void)
{
    int i;
    for (i = 0; i < K_KAT_PAYLOAD_N; ++i) {
        const kn_gs_kat_payload *v = &k_kat_payload[i];
        size_t off = 0;
        int rc = kn_gs_picture_payload(v->codec, v->pay, v->len, &off);
        char nm[96];
        snprintf(nm, sizeof nm, "payload[%s]", v->name);
        CHECK(rc == v->rc && (v->rc != KN_GS_OK || off == v->off), nm);
    }
}

static void test_keyframe_vectors(void)
{
    int i;
    for (i = 0; i < K_KAT_KEY_N; ++i) {
        const kn_gs_kat_key *v = &k_kat_key[i];
        char nm[96];
        snprintf(nm, sizeof nm, "key[%s]", v->name);
        CHECK(kn_gs_is_keyframe(v->codec, v->b, v->len, v->hint) == v->keyframe,
              nm);
    }
}

static void test_aud_vectors(void)
{
    int i;
    for (i = 0; i < K_KAT_AUD_N; ++i) {
        const kn_gs_kat_aud *v = &k_kat_aud[i];
        char nm[96];
        snprintf(nm, sizeof nm, "aud[%s]", v->name);
        CHECK(kn_gs_strip_trailing_aud(v->codec, v->b, v->len) == v->newlen,
              nm);
    }
}

static void test_fec_vectors(void)
{
    int i;
    for (i = 0; i < K_KAT_FEC_N; ++i) {
        const kn_gs_kat_fec *v = &k_kat_fec[i];
        kn_gs_fec_layout lay;
        int rc = kn_gs_fec_layout_parse(v->ext, &lay);
        char nm[96];
        snprintf(nm, sizeof nm, "fec[%s]", v->name);
        if (v->rc != KN_GS_OK) {
            CHECK(rc == v->rc, nm);
            continue;
        }
        CHECK(rc == KN_GS_OK &&
              lay.frame_index == v->frame_index &&
              lay.block_index == v->block &&
              lay.last_block_index == v->last_block &&
              lay.shard_index == v->shard &&
              lay.data_shards == v->data &&
              lay.parity_shards == v->parity, nm);
    }
}

static void test_cont_vectors(void)
{
    int i;
    for (i = 0; i < K_KAT_CONT_N; ++i) {
        char nm[64];
        snprintf(nm, sizeof nm, "cont[%06x->%06x]",
                 k_kat_cont[i][0], k_kat_cont[i][1]);
        CHECK(kn_gs_index_continuous(k_kat_cont[i][0], k_kat_cont[i][1])
              == (int)k_kat_cont[i][2], nm);
    }
}

static void test_hex_vectors(void)
{
    int i;
    for (i = 0; i < K_KAT_HEX_N; ++i) {
        const kn_gs_kat_hex *v = &k_kat_hex[i];
        kn_gs_dump_rec rec;
        unsigned char pay[2048];
        size_t got = 0;
        int rc = kn_gs_hexdump_parse_line(v->line, &rec, pay, sizeof pay,
                                          &got);
        char nm[96];
        snprintf(nm, sizeof nm, "hex[%s]", v->name);
        if (v->rc != KN_GS_OK) {
            CHECK(rc == v->rc, nm);
            continue;
        }
        CHECK(rc == KN_GS_OK &&
              rec.mono_us == v->mono_us && rec.meta == v->meta &&
              rec.flags == v->flags && rec.len == v->len &&
              got == v->len &&
              (v->len == 0 || memcmp(pay, v->payload, v->len) == 0), nm);
    }
}

/* ------------------------------------------------ pitfall discriminators */

static void test_pitfalls(void)
{
    /* #1 little-endian body vs big-endian header: the golden vector's
     * frame_index must NOT equal its big-endian reading. */
    const kn_gs_kat_video *golden = &k_kat_video[0];   /* "golden_body" */
    uint32_t be = ((uint32_t)golden->ext[4] << 24) |
                  ((uint32_t)golden->ext[5] << 16) |
                  ((uint32_t)golden->ext[6] << 8) |
                  (uint32_t)golden->ext[7];
    kn_gs_video g;
    CHECK(strcmp(golden->name, "golden_body") == 0, "vector[0] is the golden");
    CHECK(kn_gs_video_parse(golden->ext, &g) == KN_GS_OK, "golden parses");
    CHECK(g.frame_index != be, "#1: LE reading differs from BE reading");
    CHECK(g.frame_index == golden->frame_index, "#1: LE is correct");

    /* #2 ext[11] block indices are NOT ext[8] nibbles nor ext[12]. */
    CHECK(g.fec_current_block != ((golden->ext[8] >> 4) & 3u),
          "#2: cur != ext[8] high nibble");
    CHECK(g.fec_last_block != ((golden->ext[12] >> 4) & 3u),
          "#2: last != ext[12] high nibble");
    CHECK(g.fec_current_block == 2u && g.fec_last_block == 3u,
          "#2: ext[11] extraction");

    /* strict 16-byte GS extension (nvst.rs:2490) */
    {
        unsigned char pkt[64];
        unsigned char ext16[16];
        kn_gs_rtp h;
        build_gs_ext(ext16, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        /* words=3 variant: hand-build (builder only makes strict GS) */
        memcpy(pkt, "\x90\x60\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
                    "\x47\x53\x00\x03", 16);
        memcpy(pkt + 16, ext16, 12);
        CHECK(kn_gs_rtp_parse(pkt, 28, &h) == KN_GS_ERR_EXT,
              "GS with words=3 rejected");
    }
}

/* ------------------------------------------------------- property: video */

static void test_property_video(void)
{
    int i, bad = 0;
    t_rng = 0x5EED2026u;
    for (i = 0; i < 200; ++i) {
        unsigned char ext16[16];
        kn_gs_video g;
        uint32_t spi = trnd() & KN_GS_INDEX_MASK;
        uint32_t fi = trnd();
        unsigned flags = trnd() & 0x0Fu;
        unsigned cur = trnd() & 3u, last = trnd() & 3u;
        unsigned pct = trnd() & 0xFFu;
        unsigned fidx = trnd() & 0x3FFu, fsrc = trnd() & 0x3FFu;
        unsigned low8 = trnd() & 0xFFu;
        build_gs_ext(ext16, spi, fi, flags, cur, last, pct, fidx, fsrc,
                     low8, trnd() & 0xFFu, trnd() & 0xFFu);
        if (kn_gs_video_parse(ext16, &g) != KN_GS_OK ||
            g.stream_packet_index != spi || g.frame_index != fi ||
            g.flags != flags || g.fec_current_block != cur ||
            g.fec_last_block != last || g.fec_percentage != pct ||
            g.fec_index != fidx || g.fec_source_packets != fsrc ||
            g.is_fec != (pct != 0 && fidx >= fsrc ? 1 : 0) ||
            g.is_start_of_frame != ((flags & KN_GS_FLAG_SOF) && cur == 0) ||
            g.is_end_of_frame !=
                ((flags & KN_GS_FLAG_EOF) && cur == last ? 1 : 0))
            bad++;
    }
    CHECK(bad == 0, "property: 200 GS-body round-trips");
}

/* --------------------------------------------------------- property: rtp */

static void test_property_rtp(void)
{
    int i, bad = 0;
    unsigned char pkt[256];
    t_rng = 0xC0FFEE11u;
    for (i = 0; i < 200; ++i) {
        unsigned char ext16[16];
        unsigned char pay[64];
        uint32_t csrc[3];
        unsigned pt = trnd() & 0x7Fu, seq = trnd() & 0xFFFFu;
        uint32_t ts = trnd(), ssrc = trnd();
        unsigned marker = trnd() & 1u, ncsrc = trnd() % 4u;
        size_t paylen = trnd() % 40u;
        unsigned pad = (trnd() % 4u == 0) ? (1u + trnd() % 20u) : 0u;
        size_t len, j;
        kn_gs_rtp h;
        kn_gs_video g;
        uint32_t spi = trnd() & KN_GS_INDEX_MASK;
        uint32_t fi = trnd();
        unsigned flags = trnd() & 7u;
        for (j = 0; j < ncsrc; ++j) csrc[j] = trnd();
        for (j = 0; j < paylen; ++j) pay[j] = (unsigned char)trnd();
        build_gs_ext(ext16, spi, fi, flags, 0, 0, 0, 0, 0, 0, 0, 0);
        len = build_rtp(pkt, pt, seq, ts, ssrc, marker, csrc, ncsrc, ext16,
                        pay, paylen, pad);
        if (kn_gs_rtp_parse(pkt, len, &h) != KN_GS_OK ||
            h.pt != pt || h.seq != seq || h.timestamp != ts ||
            h.ssrc != ssrc || h.has_padding != (pad ? 1 : 0) ||
            h.has_gs != 1 ||
            h.payload_off != 32u + 4u * (size_t)ncsrc ||
            h.payload_len != paylen ||
            memcmp(pkt + h.payload_off, pay, paylen) != 0 ||
            kn_gs_video_parse(pkt + h.gs_off, &g) != KN_GS_OK ||
            g.stream_packet_index != spi || g.frame_index != fi)
            bad++;
    }
    CHECK(bad == 0, "property: 200 RTP+GS packet round-trips");
}

/* ----------------------------------------------------- property: hexdump */

static void test_property_hexdump(void)
{
    int i, bad = 0;
    t_rng = 0xD1CE0007u;
    for (i = 0; i < 100; ++i) {
        unsigned char pay[128];
        char line[512];
        size_t paylen = trnd() % 100u, j, o;
        uint64_t mono = ((uint64_t)trnd() << 20) ^ trnd();
        uint32_t meta = trnd(), flags = trnd() & 0xFFFFu;
        kn_gs_dump_rec rec;
        unsigned char got[128];
        size_t gotlen = 0;
        for (j = 0; j < paylen; ++j) pay[j] = (unsigned char)trnd();
        /* render EXACTLY like knring.c dump_visit */
        o = 0;
        o += (size_t)sprintf(line + o, "%llu %08x %u %u",
                             (unsigned long long)mono, meta, flags,
                             (unsigned)paylen);
        for (j = 0; j < paylen; ++j)
            o += (size_t)sprintf(line + o, " %02x", pay[j]);
        line[o++] = '\n';
        line[o] = '\0';
        if (kn_gs_hexdump_parse_line(line, &rec, got, sizeof got,
                                     &gotlen) != KN_GS_OK ||
            rec.mono_us != mono || rec.meta != meta || rec.flags != flags ||
            rec.len != (uint32_t)paylen || gotlen != paylen ||
            (paylen && memcmp(got, pay, paylen) != 0))
            bad++;
    }
    CHECK(bad == 0, "property: 100 hexdump render/parse round-trips");
}

/* --------------------------------------------------------- error paths */

static void test_error_paths(void)
{
    kn_gs_rtp h;
    kn_gs_video g;
    kn_gs_dump_rec rec;
    unsigned char pkt[16] = { 0x80, 0x60 };
    unsigned char ext16[16];
    size_t off, got;

    CHECK(kn_gs_rtp_parse(NULL, 12, &h) == KN_GS_ERR_ARGS, "rtp NULL pkt");
    CHECK(kn_gs_rtp_parse(pkt, 16, NULL) == KN_GS_ERR_ARGS, "rtp NULL out");
    CHECK(kn_gs_video_parse(NULL, &g) == KN_GS_ERR_ARGS, "video NULL ext");
    CHECK(kn_gs_find_start_code(NULL, 10, &off, &got) == KN_GS_ERR_ARGS,
          "find NULL buf");
    CHECK(kn_gs_picture_payload(KN_GS_CODEC_H264, NULL, 4, &off)
          == KN_GS_ERR_ARGS, "payload NULL");
    CHECK(kn_gs_fec_layout_parse(ext16, NULL) == KN_GS_ERR_ARGS, "fec NULL out");
    CHECK(kn_gs_hexdump_parse_line(NULL, &rec, NULL, 0, &got)
          == KN_GS_ERR_ARGS, "hex NULL line");
    CHECK(kn_gs_hexdump_parse_line("1 00000000 0 8 00", &rec, NULL, 0, &got)
          == KN_GS_ERR_HEX, "hex too few tokens");
    CHECK(kn_gs_hexdump_parse_line("1 00000000 0 8 00 01 02 03 04 05 06",
                                   &rec, NULL, 0, &got) == KN_GS_ERR_HEX,
          "hex still too few tokens (7 of 8)");
    CHECK(kn_gs_hexdump_parse_line("1 00000000 0 8 00 01 02 03 04 05 06 07",
                                   &rec, NULL, 0, &got) == KN_GS_OK,
          "hex exactly 8 of 8 tokens parses");
    /* capacity: well-formed line, buffer too small */
    {
        unsigned char small[2];
        CHECK(kn_gs_hexdump_parse_line("5 0000000a 1 4 aa bb cc dd",
                                       &rec, small, sizeof small, &got)
              == KN_GS_ERR_HEX_CAP, "hex cap enforced");
        /* fields-only mode: no buffer -> no cap verdict */
        CHECK(kn_gs_hexdump_parse_line("5 0000000a 1 4 aa bb cc dd",
                                       &rec, NULL, 0, &got) == KN_GS_OK &&
              got == 4, "hex fields-only mode");
    }
    /* trailing space is malformed (matches python split(" ")) */
    CHECK(kn_gs_hexdump_parse_line("5 0000000a 1 1 aa ", &rec, NULL, 0, &got)
          == KN_GS_ERR_HEX, "hex trailing space rejected");
    /* start-code boundary: 4-byte code needing the byte PAST the window */
    {
        unsigned char b[69];
        size_t o2, pl;
        memset(b, 0x11, sizeof b);
        b[65] = 0; b[66] = 0; b[67] = 0; b[68] = 1;
        CHECK(kn_gs_picture_payload(KN_GS_CODEC_H264, b, sizeof b, &o2)
              == KN_GS_ERR_NO_PICTURE, "4-byte code at 65 needs byte 68");
        b[67] = 1;    /* 00 00 01 at 65..67 -> inside the 68-byte window */
        CHECK(kn_gs_picture_payload(KN_GS_CODEC_H264, b, sizeof b, &o2)
              == KN_GS_OK && o2 == 65, "3-byte code at 65 found");
        CHECK(kn_gs_find_start_code(b, 2, &o2, &pl) == KN_GS_ERR_NO_PICTURE,
              "2-byte buffer has no code");
    }
}

int main(void)
{
    test_rtp_vectors();
    test_video_vectors();
    test_payload_vectors();
    test_keyframe_vectors();
    test_aud_vectors();
    test_fec_vectors();
    test_cont_vectors();
    test_hex_vectors();
    test_pitfalls();
    test_property_video();
    test_property_rtp();
    test_property_hexdump();
    test_error_paths();
    return kn_test_summary("test_kngs");
}
