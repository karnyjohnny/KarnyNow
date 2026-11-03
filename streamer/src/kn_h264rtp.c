/* kn_h264rtp.c — RFC 6184 depacketizer / AU assembler (see kn_h264rtp.h
 * for the full wire contract, ground truth and purity rules).
 *
 * Structure of push(): sequence discipline -> timestamp boundary ->
 * payload class -> marker boundary. The emit path is the ONLY place the
 * callback runs, so every AU — marker-closed or ts-split — passes the
 * same broken/over/incomplete gauntlet exactly once.
 */
#include "kn/kn_h264rtp.h"

#include <string.h>

#define KN_H264_SC_LEN 4u              /* 00 00 00 01 before every NAL */

static const uint8_t k_start_code[KN_H264_SC_LEN] = { 0x00, 0x00, 0x00, 0x01 };

void kn_h264rtp_init(kn_h264rtp *d, uint8_t *buf, size_t cap)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->buf = buf;
    d->cap = cap;
}

/* Append bytes to the assembly buffer; on overflow set au_over and stop
 * appending (the AU is dropped WHOLE at emit — never truncated). */
static void h264rtp_append(kn_h264rtp *d, const uint8_t *src, size_t n)
{
    if (d->au_over) return;
    if (d->len + n > d->cap) {
        d->au_over = 1;
        return;
    }
    memcpy(d->buf + d->len, src, n);
    d->len += n;
}

/* Start-code + NAL append. */
static void h264rtp_append_nal(kn_h264rtp *d, const uint8_t *nal, size_t n)
{
    h264rtp_append(d, k_start_code, KN_H264_SC_LEN);
    h264rtp_append(d, nal, n);
}

static void h264rtp_reset_au(kn_h264rtp *d)
{
    d->len = 0;
    d->au_broken = 0;
    d->au_over = 0;
    d->in_fu = 0;
}

/* The single emit path. reason_split: this emit was caused by a ts
 * change without a marker. ts_split counts only AUs ACTUALLY CLOSED
 * by the ts change (a clean emit) — the honest signal: 0 on a healthy
 * stream, >0 means the sender lost markers. Returns the callback's rc
 * (0 = keep going). */
static int h264rtp_emit(kn_h264rtp *d, int reason_split,
                        kn_h264rtp_au_fn cb, void *ud)
{
    int rc = 0;
    if (d->len > 0) {
        if (d->in_fu) {
            /* the marker/ts closed an AU while a fragment run was still
             * open: the trailing NAL never finished — decoder poison. */
            ++d->st.fu_incomplete;
            ++d->st.drop_broken;
        } else if (d->au_over) {
            ++d->st.drop_over;
        } else if (d->au_broken) {
            ++d->st.drop_broken;
        } else {
            if (reason_split) ++d->st.ts_split;
            ++d->st.aus;
            d->st.au_bytes += (unsigned long long)d->len;
            if ((unsigned long long)d->len > d->st.au_max)
                d->st.au_max = (unsigned long long)d->len;
            if (cb) rc = cb(d->buf, d->len, d->cur_ts, ud);
        }
    }
    h264rtp_reset_au(d);
    return rc;
}

static void h264rtp_log_bad(kn_h264rtp *d, unsigned type, uint16_t seq,
                            const uint8_t *payload, size_t plen)
{
    size_t n = plen < sizeof d->bad_head ? plen : sizeof d->bad_head;
    if (!d->bad_logged) {
        d->bad_logged = 1;
        d->bad_type = type;
        d->bad_seq = seq;
        d->bad_head_len = (unsigned)n;
        if (n) memcpy(d->bad_head, payload, n);   /* plen==0: NULL-safe */
    }
}

int kn_h264rtp_push(kn_h264rtp *d, uint16_t seq, uint32_t ts, int marker,
                    const uint8_t *payload, size_t plen,
                    kn_h264rtp_au_fn cb, void *ud)
{
    int16_t dist;
    int rc = 0;
    unsigned type;
    uint8_t b0;

    if (!d || !d->buf || (!payload && plen)) return -1;
    ++d->st.pkts_in;

    /* ---- sequence discipline (int16 distance: wrap-safe) ---- */
    if (!d->have_seq) {
        d->have_seq = 1;
        d->next_seq = (uint16_t)(seq + 1u);
    } else {
        dist = (int16_t)(uint16_t)(seq - d->next_seq);
        if (dist < 0) {
            ++d->st.pkt_dups;            /* duplicate/late: nothing new */
            return 0;
        }
        if (dist > 0) {
            ++d->st.pkt_gaps;
            if (d->len > 0 || d->in_fu) {
                if (d->in_fu) ++d->st.fu_incomplete;
                ++d->st.drop_gap;        /* a NAL with a hole is poison */
                h264rtp_reset_au(d);
            }
        }
        /* dist == 0 (expected) or dist > 0 (resync after the gap):
         * the expectation always advances past the accepted packet. */
        d->next_seq = (uint16_t)(seq + 1u);
    }

    /* ---- timestamp boundary: close the previous AU first ---- */
    if (d->have_ts && ts != d->cur_ts) {
        rc = h264rtp_emit(d, 1, cb, ud);
        if (rc != 0) return rc;
    }
    d->cur_ts = ts;
    d->have_ts = 1;

    if (plen < 1) {
        ++d->st.malformed;
        d->au_broken = 1;
        h264rtp_log_bad(d, 0xFFFFFFFFu, seq, payload, plen);
        goto marker;
    }
    b0 = payload[0];
    type = (unsigned)(b0 & 0x1Fu);
    if (b0 & 0x80u) {                    /* F bit MUST be 0 (RFC 6184 §5.3) */
        ++d->st.malformed;
        d->au_broken = 1;
        h264rtp_log_bad(d, type, seq, payload, plen);
        goto marker;
    }

    if (type >= 1u && type <= 23u) {
        /* ---- single NAL unit packet (§5.2) ---- */
        if (d->in_fu) {                  /* a fragment run died mid-way */
            ++d->st.fu_incomplete;
            d->au_broken = 1;
        }
        d->in_fu = 0;
        ++d->st.single_nal;
        h264rtp_append_nal(d, payload, plen);
    } else if (type == 24u) {
        /* ---- STAP-A (§5.7.1): [BE16 len][NAL]... ---- */
        size_t off = 1;
        unsigned nals = 0;
        if (d->in_fu) {
            ++d->st.fu_incomplete;
            d->au_broken = 1;
            d->in_fu = 0;
        }
        if (plen < 3) {
            ++d->st.malformed;
            d->au_broken = 1;
            h264rtp_log_bad(d, type, seq, payload, plen);
            goto marker;
        }
        ++d->st.stap_a_pkts;
        while (off + 2 <= plen) {
            size_t nlen = ((size_t)payload[off] << 8) | payload[off + 1];
            off += 2;
            if (nlen == 0 || off + nlen > plen) {
                ++d->st.malformed;       /* truncated aggregation */
                d->au_broken = 1;
                h264rtp_log_bad(d, type, seq, payload, plen);
                break;
            }
            h264rtp_append_nal(d, payload + off, nlen);
            off += nlen;
            ++nals;
        }
        d->st.stap_a_nals += nals;
    } else if (type == 28u) {
        /* ---- FU-A (§5.8): S|E|R|type + fragment body ---- */
        uint8_t fu;
        ++d->st.fu_a_pkts;
        if (plen < 2) {
            ++d->st.malformed;
            d->au_broken = 1;
            h264rtp_log_bad(d, type, seq, payload, plen);
            goto marker;
        }
        fu = payload[1];
        if (fu & 0x80u) {                /* S: start of a fragmented NAL */
            if (d->in_fu) {              /* previous FU never saw its E */
                ++d->st.fu_incomplete;
                d->au_broken = 1;
            }
            {
                uint8_t recon = (uint8_t)((b0 & 0xE0u) | (fu & 0x1Fu));
                h264rtp_append(d, k_start_code, KN_H264_SC_LEN);
                h264rtp_append(d, &recon, 1);
            }
            h264rtp_append(d, payload + 2, plen - 2);
            d->in_fu = 1;
        } else if (d->in_fu) {           /* middle / end fragment */
            h264rtp_append(d, payload + 2, plen - 2);
            if (fu & 0x40u)              /* E: the NAL is complete */
                d->in_fu = 0;
        } else {                         /* orphan fragment: no S seen */
            ++d->st.malformed;
            d->au_broken = 1;
            h264rtp_log_bad(d, type, seq, payload, plen);
        }
    } else {
        /* ---- 0, 25..27, 29..31: not emitted by pm=1 senders ---- */
        ++d->st.other_type;
        d->au_broken = 1;
        h264rtp_log_bad(d, type, seq, payload, plen);
    }

marker:
    if (marker) {
        int erc = h264rtp_emit(d, 0, cb, ud);
        if (erc != 0) return erc;
    }
    return rc;
}
