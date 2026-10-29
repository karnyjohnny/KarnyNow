/* kn_arx.c — NVST bundle-audio receive/extract (contract: kn_arx.h).
 *
 * Byte/counter-exact against tools/arx_reference.py (the independent
 * Python implementation — two-implementations rule); the KAT battery in
 * kn_arx_kat_vectors.h replays 12 groups / 21 SRTP vectors through BOTH
 * implementations. Provenance table in kn_arx.h; the 14-step processing
 * order + counter semantics spec is the arx_reference.py docstring.
 *
 * Design rules (project conventions): zero allocation (fixed wire
 * scratch), deterministic time (now_ms parameter), honest stats (every
 * rejection path increments exactly one counter), faithful over clever
 * (arrival-order emission D-a1, per-epoch replay reset D-a2). */
#include "kn/kn_arx.h"
#include "kn/kn_ogg.h"          /* kn_opus_packet_samples48k (TOC) */

#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

void kn_arx_init(kn_arx *a, kn_arx_emit_fn emit, void *ectx)
{
    if (!a)
        return;
    memset(a, 0, sizeof *a);
    a->emit = emit;
    a->ectx = ectx;
    kn_srtp_replay_init(&a->replay);
}

int kn_arx_set_srtp(kn_arx *a, const uint8_t master_key[16],
                    const uint8_t master_salt[14])
{
    if (!a || !master_key || !master_salt)
        return -1;
    if (kn_srtp_kdf128(master_key, master_salt, 14,
                       KN_SRTP_LABEL_CIPHER_KEY, a->enc_key, 16) != 0)
        return -1;
    if (kn_srtp_kdf128(master_key, master_salt, 14,
                       KN_SRTP_LABEL_AUTH_KEY, a->auth_key, 20) != 0)
        return -1;
    if (kn_srtp_kdf128(master_key, master_salt, 14,
                       KN_SRTP_LABEL_SALT, a->sess_salt, 14) != 0)
        return -1;
    a->armed = 1;
    return 0;
}

void kn_arx_session_keys(const kn_arx *a, uint8_t enc[16],
                         uint8_t auth[20], uint8_t salt[14])
{
    if (!a)
        return;
    if (enc)  memcpy(enc, a->enc_key, 16);
    if (auth) memcpy(auth, a->auth_key, 20);
    if (salt) memcpy(salt, a->sess_salt, 14);
}

/* RFC 2198 walk: returns the primary-block offset/length, or -1 when
 * the block lengths do not tile the body exactly. red_blocks counts the
 * redundant blocks ONLY for a valid packet (spec step 12). */
static int red_primary(const uint8_t *pay, size_t len, size_t *out_off,
                       size_t *out_len, unsigned *blocks)
{
    size_t off = 0, hdrs_end, total = 0;
    unsigned nblocks = 0;
    int have_primary = 0;
    while (off < len) {
        uint8_t b0 = pay[off];
        if (b0 & 0x80u) {                  /* F=1: 4-octet header */
            if (off + 4 > len)
                return -1;
            total += (size_t)(((pay[off + 2] & 0x03u) << 8) | pay[off + 3]);
            nblocks++;
            off += 4;
        } else {                           /* F=0: primary, 1 octet */
            have_primary = 1;
            off += 1;
            break;
        }
    }
    if (!have_primary)
        return -1;
    hdrs_end = off;
    if (total > len - hdrs_end)            /* redundant blocks overflow */
        return -1;
    *out_len = (len - hdrs_end) - total;   /* primary = what remains */
    *out_off = hdrs_end + total;
    *blocks = nblocks;
    return 0;
}

int kn_arx_process(kn_arx *a, const uint8_t *datagram, size_t len,
                   uint64_t now_ms)
{
    unsigned pt, cc, pad_bit, x_bit;
    size_t hlen;
    uint16_t seq;
    uint32_t ssrc;
    uint64_t index;
    size_t plain_len = 0, pay_off, pay_len;
    const uint8_t *payload;
    int samples;
    int16_t delta;

    if (!a || !datagram)
        return -1;
    a->st.consumed++;

    /* step 2 — minimal RTP shape */
    if (len < 12 || (datagram[0] >> 6) != 2) {
        a->st.malformed++;
        return -1;
    }
    /* step 3 — PT admission (matches_audio_track, nvst.rs:6358-6361) */
    pt = datagram[1] & 0x7Fu;
    if (pt != KN_ARX_PT_OPUS && pt != KN_ARX_PT_RED) {
        a->st.pt_filtered++;
        return 0;
    }
    /* step 4 — wire scratch ceiling */
    if (len > KN_ARX_MAX_PKT) {
        a->st.malformed++;
        return -1;
    }
    /* step 5 — header walk (CC + X extension) */
    cc = datagram[0] & 0x0Fu;
    x_bit = (datagram[0] & 0x10u) != 0;
    pad_bit = (datagram[0] & 0x20u) != 0;
    hlen = 12 + 4 * (size_t)cc;
    if (x_bit) {
        uint16_t words;
        if (len < hlen + 4) {
            a->st.malformed++;
            return -1;
        }
        words = be16(datagram + hlen + 2);
        hlen += 4 + 4 * (size_t)words;
    }
    if (hlen + KN_ARX_TAG_LEN > len) {
        a->st.malformed++;
        return -1;
    }
    /* step 6 — keying */
    if (!a->armed) {
        a->st.srtp_nokey++;
        return -1;
    }
    ssrc = be32(datagram + 8);
    seq = be16(datagram + 2);
    /* step 7 — SSRC epoch (first locks; change resets replay + seq,
     * per-stream contexts like str0m — D-a2) */
    if (!a->have_ssrc) {
        a->have_ssrc = 1;
        a->ssrc = ssrc;
    } else if (ssrc != a->ssrc) {
        a->st.ssrc_changes++;
        a->ssrc = ssrc;
        kn_srtp_replay_init(&a->replay);
        a->have_seq = 0;
    }
    /* step 8 — replay guess/check (ROC estimation inside guess) */
    if (kn_srtp_replay_guess(&a->replay, seq, &index) != 0) {
        a->st.replay_reject++;
        return -1;
    }
    if (kn_srtp_replay_check(&a->replay, index) != 0) {
        a->st.replay_reject++;
        return -1;
    }
    /* step 9 — SRTP unprotect in the scratch (in-place), then commit */
    memcpy(a->pkt, datagram, len);
    if (kn_srtp_unprotect_cm_hmac(a->pkt, len, hlen, a->ssrc, index,
                                  a->enc_key, a->auth_key, a->sess_salt,
                                  KN_ARX_TAG_LEN, &plain_len) != 0) {
        a->st.srtp_fail++;
        return -1;
    }
    kn_srtp_replay_commit(&a->replay, index);
    /* step 10 — seq accounting (wire truth, before codec checks) */
    if (!a->have_seq) {
        a->have_seq = 1;
        a->highest_seq = seq;
    } else {
        delta = (int16_t)(uint16_t)(seq - a->highest_seq);
        if (delta > 0) {
            a->st.seq_loss += (unsigned long long)(delta - 1);
            a->highest_seq = seq;
        } else if (delta == 0) {
            a->st.seq_dup++;
        } else {
            a->st.seq_reorder++;
        }
    }
    if (pad_bit)
        a->st.padded++;
    /* step 11 — padding strip */
    pay_off = hlen;
    if (plain_len < pay_off) {
        a->st.malformed++;
        return -1;
    }
    pay_len = plain_len - pay_off;
    if (pad_bit) {
        uint8_t pad;
        if (pay_len == 0) {
            a->st.malformed++;
            return -1;
        }
        pad = a->pkt[plain_len - 1];
        if (pad == 0 || (size_t)pad > pay_len) {
            a->st.malformed++;
            return -1;
        }
        pay_len -= pad;
    }
    payload = a->pkt + pay_off;
    /* step 12 — RED de-framing (PT 63 → the primary block) */
    if (pt == KN_ARX_PT_RED) {
        size_t roff, rlen;
        unsigned blocks = 0;
        a->st.red_packets++;
        if (red_primary(payload, pay_len, &roff, &rlen, &blocks) != 0) {
            a->st.red_invalid++;
            return -1;
        }
        a->st.red_blocks += blocks;
        payload = payload + roff;
        pay_len = rlen;
    }
    /* step 13 — Opus TOC duration */
    samples = kn_opus_packet_samples48k(payload, pay_len);
    if (samples < 0) {
        a->st.toc_invalid++;
        return -1;
    }
    /* step 14 — emit (arrival order, D-a1) */
    a->st.packets++;
    a->st.payload_bytes += (unsigned long long)pay_len;
    if (!a->st.first_ms)
        a->st.first_ms = now_ms;
    a->st.last_ms = now_ms;
    if (a->emit)
        a->emit(a->ectx, payload, pay_len, (uint32_t)samples);
    return 1;
}
