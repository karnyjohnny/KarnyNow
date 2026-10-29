/* kn_ogg.c — Ogg page container + OggOpus muxing (contract: kn_ogg.h).
 *
 * Byte-exact against tools/oggopus_reference.py (the independent Python
 * implementation — two-implementations rule); the KAT battery in
 * kn_ogg_kat_vectors.h compares full page streams, CRCs and TOC
 * durations. Provenance: RFC 3533 (framing/CRC), RFC 7845 (OggOpus),
 * RFC 6716 §3.1 (TOC durations). Deviations D-o1..D-o3: kn_ogg.h.
 *
 * Design rules (project conventions): zero allocation (caller-owned
 * page scratch + write callback), deterministic (fixed serial → byte-
 * identical streams), honest stats (pages_out/bytes_out/write_errors/
 * dropped_packets). */
#include "kn/kn_ogg.h"

#include <string.h>

/* ------------------------------------------------------- RFC 3533 CRC */

static const uint32_t k_ogg_crc_tab[256] = {
    0x00000000u, 0x04c11db7u, 0x09823b6eu, 0x0d4326d9u,
    0x130476dcu, 0x17c56b6bu, 0x1a864db2u, 0x1e475005u,
    0x2608edb8u, 0x22c9f00fu, 0x2f8ad6d6u, 0x2b4bcb61u,
    0x350c9b64u, 0x31cd86d3u, 0x3c8ea00au, 0x384fbdbdu,
    0x4c11db70u, 0x48d0c6c7u, 0x4593e01eu, 0x4152fda9u,
    0x5f15adacu, 0x5bd4b01bu, 0x569796c2u, 0x52568b75u,
    0x6a1936c8u, 0x6ed82b7fu, 0x639b0da6u, 0x675a1011u,
    0x791d4014u, 0x7ddc5da3u, 0x709f7b7au, 0x745e66cdu,
    0x9823b6e0u, 0x9ce2ab57u, 0x91a18d8eu, 0x95609039u,
    0x8b27c03cu, 0x8fe6dd8bu, 0x82a5fb52u, 0x8664e6e5u,
    0xbe2b5b58u, 0xbaea46efu, 0xb7a96036u, 0xb3687d81u,
    0xad2f2d84u, 0xa9ee3033u, 0xa4ad16eau, 0xa06c0b5du,
    0xd4326d90u, 0xd0f37027u, 0xddb056feu, 0xd9714b49u,
    0xc7361b4cu, 0xc3f706fbu, 0xceb42022u, 0xca753d95u,
    0xf23a8028u, 0xf6fb9d9fu, 0xfbb8bb46u, 0xff79a6f1u,
    0xe13ef6f4u, 0xe5ffeb43u, 0xe8bccd9au, 0xec7dd02du,
    0x34867077u, 0x30476dc0u, 0x3d044b19u, 0x39c556aeu,
    0x278206abu, 0x23431b1cu, 0x2e003dc5u, 0x2ac12072u,
    0x128e9dcfu, 0x164f8078u, 0x1b0ca6a1u, 0x1fcdbb16u,
    0x018aeb13u, 0x054bf6a4u, 0x0808d07du, 0x0cc9cdcau,
    0x7897ab07u, 0x7c56b6b0u, 0x71159069u, 0x75d48ddeu,
    0x6b93dddbu, 0x6f52c06cu, 0x6211e6b5u, 0x66d0fb02u,
    0x5e9f46bfu, 0x5a5e5b08u, 0x571d7dd1u, 0x53dc6066u,
    0x4d9b3063u, 0x495a2dd4u, 0x44190b0du, 0x40d816bau,
    0xaca5c697u, 0xa864db20u, 0xa527fdf9u, 0xa1e6e04eu,
    0xbfa1b04bu, 0xbb60adfcu, 0xb6238b25u, 0xb2e29692u,
    0x8aad2b2fu, 0x8e6c3698u, 0x832f1041u, 0x87ee0df6u,
    0x99a95df3u, 0x9d684044u, 0x902b669du, 0x94ea7b2au,
    0xe0b41de7u, 0xe4750050u, 0xe9362689u, 0xedf73b3eu,
    0xf3b06b3bu, 0xf771768cu, 0xfa325055u, 0xfef34de2u,
    0xc6bcf05fu, 0xc27dede8u, 0xcf3ecb31u, 0xcbffd686u,
    0xd5b88683u, 0xd1799b34u, 0xdc3abdedu, 0xd8fba05au,
    0x690ce0eeu, 0x6dcdfd59u, 0x608edb80u, 0x644fc637u,
    0x7a089632u, 0x7ec98b85u, 0x738aad5cu, 0x774bb0ebu,
    0x4f040d56u, 0x4bc510e1u, 0x46863638u, 0x42472b8fu,
    0x5c007b8au, 0x58c1663du, 0x558240e4u, 0x51435d53u,
    0x251d3b9eu, 0x21dc2629u, 0x2c9f00f0u, 0x285e1d47u,
    0x36194d42u, 0x32d850f5u, 0x3f9b762cu, 0x3b5a6b9bu,
    0x0315d626u, 0x07d4cb91u, 0x0a97ed48u, 0x0e56f0ffu,
    0x1011a0fau, 0x14d0bd4du, 0x19939b94u, 0x1d528623u,
    0xf12f560eu, 0xf5ee4bb9u, 0xf8ad6d60u, 0xfc6c70d7u,
    0xe22b20d2u, 0xe6ea3d65u, 0xeba91bbcu, 0xef68060bu,
    0xd727bbb6u, 0xd3e6a601u, 0xdea580d8u, 0xda649d6fu,
    0xc423cd6au, 0xc0e2d0ddu, 0xcda1f604u, 0xc960ebb3u,
    0xbd3e8d7eu, 0xb9ff90c9u, 0xb4bcb610u, 0xb07daba7u,
    0xae3afba2u, 0xaafbe615u, 0xa7b8c0ccu, 0xa379dd7bu,
    0x9b3660c6u, 0x9ff77d71u, 0x92b45ba8u, 0x9675461fu,
    0x8832161au, 0x8cf30badu, 0x81b02d74u, 0x857130c3u,
    0x5d8a9099u, 0x594b8d2eu, 0x5408abf7u, 0x50c9b640u,
    0x4e8ee645u, 0x4a4ffbf2u, 0x470cdd2bu, 0x43cdc09cu,
    0x7b827d21u, 0x7f436096u, 0x7200464fu, 0x76c15bf8u,
    0x68860bfdu, 0x6c47164au, 0x61043093u, 0x65c52d24u,
    0x119b4be9u, 0x155a565eu, 0x18197087u, 0x1cd86d30u,
    0x029f3d35u, 0x065e2082u, 0x0b1d065bu, 0x0fdc1becu,
    0x3793a651u, 0x3352bbe6u, 0x3e119d3fu, 0x3ad08088u,
    0x2497d08du, 0x2056cd3au, 0x2d15ebe3u, 0x29d4f654u,
    0xc5a92679u, 0xc1683bceu, 0xcc2b1d17u, 0xc8ea00a0u,
    0xd6ad50a5u, 0xd26c4d12u, 0xdf2f6bcbu, 0xdbee767cu,
    0xe3a1cbc1u, 0xe760d676u, 0xea23f0afu, 0xeee2ed18u,
    0xf0a5bd1du, 0xf464a0aau, 0xf9278673u, 0xfde69bc4u,
    0x89b8fd09u, 0x8d79e0beu, 0x803ac667u, 0x84fbdbd0u,
    0x9abc8bd5u, 0x9e7d9662u, 0x933eb0bbu, 0x97ffad0cu,
    0xafb010b1u, 0xab710d06u, 0xa6322bdfu, 0xa2f33668u,
    0xbcb4666du, 0xb8757bdau, 0xb5365d03u, 0xb1f740b4u
};

uint32_t kn_ogg_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0;
    size_t i;
    for (i = 0; i < len; i++)
        crc = (crc << 8) ^ k_ogg_crc_tab[((crc >> 24) ^ p[i]) & 0xFFu];
    return crc;
}

/* --------------------------------------------------- RFC 6716 §3.1 TOC */

/* frame size in 48 kHz samples per config 0..31 (RFC 6716 §3.1):
 * SILK NB/MB/WB 10/20/40/60 ms, HYBRID SWB/FB 10/20 ms,
 * CELT NB/WB/SWB/FB 2.5/5/10/20 ms. */
static const uint16_t k_opus_cfg_samples[32] = {
    480, 960, 1920, 2880,  480, 960, 1920, 2880,
    480, 960, 1920, 2880,  480, 960,  480,  960,
    120, 240,  480,  960,  120, 240,  480,  960,
    120, 240,  480,  960,  120, 240,  480,  960
};

int kn_opus_packet_samples48k(const uint8_t *pkt, size_t len)
{
    unsigned toc, code, frames;
    if (!pkt || len == 0)
        return -1;
    toc = pkt[0];
    code = toc & 0x03u;
    if (code == 0)
        frames = 1;
    else if (code == 1 || code == 2)
        frames = 2;
    else {
        if (len < 2)
            return -1;
        frames = pkt[1] & 0x3Fu;      /* RFC 6716 §3.2.5: M in 1..48 */
        if (frames == 0 || frames > 48)
            return -1;
    }
    return (int)k_opus_cfg_samples[toc >> 3] * (int)frames;
}

/* ------------------------------------------------------------ helpers */

static void ogg_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void ogg_put64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

#define OGG_HDR      27u
#define OGG_LACING   255u
#define OGG_PAY_OFF  (OGG_HDR + OGG_LACING)   /* fixed payload region */

/* ------------------------------------------------------------- muxer */

int kn_ogg_mux_init(kn_ogg_mux *m, uint8_t *buf, size_t bufsz,
                    uint32_t serial, unsigned channels,
                    kn_ogg_write_fn write, void *wctx)
{
    if (!m || !buf || bufsz < KN_OGG_PAGE_BUF || !write ||
        channels == 0 || channels > 255)
        return -1;
    memset(m, 0, sizeof *m);
    m->buf = buf;
    m->bufsz = bufsz;
    m->serial = serial;
    m->channels = channels;
    m->write = write;
    m->wctx = wctx;
    return 0;
}

/* Write the open page (npkts may be 0 ONLY for the EOS case). */
static int ogg_flush(kn_ogg_mux *m, uint8_t htype)
{
    uint8_t hdr[OGG_HDR];
    size_t page_len;
    uint32_t crc;

    if (m->write_errors)                    /* latched after a sink error */
        return -1;
    if (m->npkts == 0 && !(htype & 0x04u))
        return 0;                           /* nothing open, not EOS */
    if (OGG_PAY_OFF + m->seg_off > m->bufsz)
        return -1;                          /* unreachable with D-o1 caps */

    memcpy(hdr, "OggS", 4);
    hdr[4] = 0;                             /* stream_structure_version */
    hdr[5] = htype;
    ogg_put64(hdr + 6, (uint64_t)m->granule);
    ogg_put32(hdr + 14, m->serial);
    ogg_put32(hdr + 18, m->page_seq);
    hdr[22] = hdr[23] = hdr[24] = hdr[25] = 0;   /* CRC placeholder */
    hdr[26] = (uint8_t)m->nsegs;

    memcpy(m->buf, hdr, OGG_HDR);
    memcpy(m->buf + OGG_HDR, m->segs, m->nsegs);
    if (m->nsegs < OGG_LACING && m->seg_off)
        memmove(m->buf + OGG_HDR + m->nsegs, m->buf + OGG_PAY_OFF,
                m->seg_off);
    page_len = OGG_HDR + m->nsegs + m->seg_off;
    crc = kn_ogg_crc32(m->buf, page_len);
    ogg_put32(m->buf + 22, crc);

    if (m->write(m->wctx, m->buf, page_len) != 0) {
        m->write_errors++;
        return -1;
    }
    m->pages_out++;
    m->bytes_out += (unsigned long long)page_len;
    m->page_seq++;
    m->seg_off = 0;
    m->nsegs = 0;
    m->npkts = 0;
    return 0;
}

/* Add a packet to the open page — caller guarantees it fits. */
static void ogg_append(kn_ogg_mux *m, const uint8_t *payload, size_t len,
                       uint32_t samples48)
{
    size_t n255 = len / 255u;
    size_t i;
    for (i = 0; i < n255; i++)
        m->segs[m->nsegs++] = 255u;
    m->segs[m->nsegs++] = (uint8_t)(len % 255u);   /* terminator/boundary */
    memcpy(m->buf + OGG_PAY_OFF + m->seg_off, payload, len);
    m->seg_off += len;
    m->npkts++;
    m->granule += (int64_t)samples48;
}

int kn_ogg_mux_begin(kn_ogg_mux *m)
{
    uint8_t head[19];
    uint8_t tags[8 + 4 + 8 + 4];
    static const char vendor[] = "KarnyNow";

    if (!m || !m->buf || m->began)
        return -1;
    /* OpusHead — RFC 7845 §5.1, mapping family 0, pre-skip 0 (D-o3) */
    memcpy(head, "OpusHead", 8);
    head[8] = 1;                            /* version */
    head[9] = (uint8_t)m->channels;
    head[10] = 0; head[11] = 0;             /* pre-skip LE16 = 0 */
    ogg_put32(head + 12, 48000u);           /* input sample rate */
    head[16] = 0; head[17] = 0;             /* output gain = 0 */
    head[18] = 0;                           /* mapping family 0 */
    ogg_append(m, head, sizeof head, 0);
    if (ogg_flush(m, 0x02u) != 0)           /* BOS */
        return -1;
    /* OpusTags — RFC 7845 §5.2 */
    memcpy(tags, "OpusTags", 8);
    ogg_put32(tags + 8, (uint32_t)(sizeof vendor - 1));
    memcpy(tags + 12, vendor, sizeof vendor - 1);
    ogg_put32(tags + 20, 0u);               /* user comment count */
    ogg_append(m, tags, sizeof tags, 0);
    if (ogg_flush(m, 0x00u) != 0)
        return -1;
    m->began = 1;
    return 0;
}

int kn_ogg_mux_packet(kn_ogg_mux *m, const uint8_t *payload, size_t len,
                      uint32_t samples48)
{
    size_t segs_needed;
    if (!m || !m->began || m->finished || !payload)
        return -1;
    if (m->write_errors)
        return -1;
    segs_needed = len / 255u + 1u;
    if (segs_needed > OGG_LACING || len > KN_OGG_PAGE_PAYLOAD_MAX) {
        m->dropped_packets++;               /* D-o1: honest skip */
        return 0;
    }
    if (m->nsegs + segs_needed > OGG_LACING ||
        m->seg_off + len > KN_OGG_PAGE_PAYLOAD_MAX ||
        m->npkts >= KN_OGG_PACKETS_PER_PAGE) {
        if (ogg_flush(m, 0x00u) != 0)
            return -1;
    }
    ogg_append(m, payload, len, samples48);
    return 0;
}

int kn_ogg_mux_finish(kn_ogg_mux *m)
{
    if (!m || !m->began)
        return -1;
    if (m->finished)
        return 0;                           /* idempotent */
    m->finished = 1;
    return ogg_flush(m, 0x04u);             /* EOS (empty page allowed) */
}
