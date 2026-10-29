#!/usr/bin/env python3
"""arx_reference.py — the independent Python reference for kn_arx
(NVST bundle-audio receive: SRTP unprotect via DTLS-SRTP keying, RTP
header walk, RFC 2198 RED de-framing, RFC 3550-style sequence
accounting, Opus emission).

Two-implementations rule: the kn_arx contract (streamer/include/kn/
kn_arx.h) has a second implementation here; this generator self-proves
every vector (Python-side SRTP unprotect round trip) before emitting
streamer/src/kn_arx_kat_vectors.h.

Provenance:
  DTLS-SRTP keying  RFC 5764 §4.2 (EXTRACTOR-dtls_srtp) is NOT exercised
                    here — that is the mbedTLS side (kn_dtls). This
                    reference starts from the master key/salt pair and
                    runs the RFC 3711 KDF (imported from
                    tools/srtp_reference.py, itself KAT'd against
                    nvst.rs @ a49b124).
  SRTP              RFC 3711 AES_CM_128_HMAC_SHA1_80: 10-byte tag over
                    encrypted packet || be32(ROC); IV per kn_srtp_aes_
                    cm_iv; ROC/seq estimation + replay window per
                    kn_srtp_replay (guess/check/commit, nvst.rs:3137-
                    3152).
  RED               RFC 2198 — 4-octet headers (F=1) for the redundant
                    blocks, a 1-octet header (F=0) for the primary;
                    the primary is the LAST block; matches_audio_track
                    (nvst.rs:6358-6361) admits PT 63 next to PT 111.
  Track constants   nvst_rtsp.rs:1309 — the reference hardcodes
                    {payloadType:111, codec:opus, clockRateHz:48000,
                    channels:2, mid:"0", ssrc:None} into the handoff;
                    GFN_OPUS_PAYLOAD_TYPE 111 / GFN_RED_PAYLOAD_TYPE 63
                    (nvst.rs:275-276).

PROCESSING ORDER + counter semantics (THE spec for both
implementations; every packet runs this exact sequence):
   1 consumed++
   2 len < 12 or V != 2                -> malformed++,   rc -1
   3 PT not in {111, 63}               -> pt_filtered++, rc  0
   4 len > KN_ARX_MAX_PKT (1600)       -> malformed++,   rc -1
   5 header walk (CC/X); overflow      -> malformed++,   rc -1
   6 no SRTP keys armed                -> srtp_nokey++,  rc -1
   7 SSRC epoch: first locks; a change -> ssrc_changes++,
     re-lock AND reset the replay window + seq tracking
     (per-stream contexts, like str0m — deviation D-a2)
   8 replay guess/check fails          -> replay_reject++, rc -1
   9 unprotect auth fails              -> srtp_fail++,   rc -1
     success -> replay commit, then:
  10 seq accounting: delta = (int16)(seq - highest_seq):
     delta > 0: seq_loss += delta-1, highest_seq = seq
     delta = 0: seq_dup++             (emitted anyway)
     delta < 0: seq_reorder++         (emitted anyway, arrival
                                       order — deviation D-a1)
     first packet of the epoch just sets highest_seq.
     P bit set -> padded++ (the pad is stripped at step 11)
  11 padding strip; invalid pad (0 or > payload len)
                                        -> malformed++,   rc -1
  12 PT 63: red_packets++; RFC 2198 walk; block lengths must tile
     the payload exactly, else red_invalid++, rc -1; redundant
     blocks counted in red_blocks; the LAST block is the primary.
  13 Opus TOC malformed (kn_opus_packet_samples48k < 0)
                                        -> toc_invalid++, rc -1
  14 emit: packets++, payload_bytes += len, rc 1

Usage:  python3 tools/arx_reference.py [--check]
"""
import hmac
import hashlib
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from srtp_reference import (kdf128, srtp_aes_cm_iv, packet_index,
                            cm_keystream_block)

PT_OPUS = 111
PT_RED = 63
TAG_LEN = 10
KN_ARX_MAX_PKT = 1600
SSRC_A = 0xDEADBEEF
SSRC_B = 0x0BADF00D
MASTER_KEY = bytes(range(16))                     # 00 01 02 ... 0f
MASTER_SALT = bytes(range(0x10, 0x1E))            # 14 bytes 10..1d


def session_keys(master_key=MASTER_KEY, master_salt=MASTER_SALT):
    enc = kdf128(master_key, master_salt, 0, 16)
    auth = kdf128(master_key, master_salt, 1, 20)
    salt = kdf128(master_key, master_salt, 2, 14)
    return enc, auth, salt


def _keystream_xor(key, iv, data):
    out = bytearray(data)
    nblocks = (len(out) + 15) // 16
    for b in range(nblocks):
        ks = cm_keystream_block(key, iv, b)
        for i in range(16):
            j = b * 16 + i
            if j < len(out):
                out[j] ^= ks[i]
    return bytes(out)


def rtp_header(seq, ts, ssrc, pt=PT_OPUS, marker=False, cc=0,
               csrcs=(), ext=None, pad=0):
    b0 = 0x80 | (cc & 0x0F)
    if ext is not None:
        b0 |= 0x10
    if pad:
        b0 |= 0x20
    h = bytearray([b0, (0x80 if marker else 0) | (pt & 0x7F)])
    h += struct.pack(">HII", seq & 0xFFFF, ts & 0xFFFFFFFF,
                     ssrc & 0xFFFFFFFF)
    for c in csrcs:
        h += struct.pack(">I", c)
    if ext is not None:
        words = (len(ext) + 3) // 4
        h += struct.pack(">HH", 0xBEDE, words)
        h += ext + b"\x00" * (words * 4 - len(ext))
    return bytes(h)


def opus_pkt(toc=0xF8, body_len=79, fill=0x5A):
    return bytes([toc]) + bytes([fill]) * body_len


def build_srtp(seq, ts, ssrc, payload, pt=PT_OPUS, marker=False, cc=0,
               csrcs=(), ext=None, pad=0, corrupt=False, roc=0,
               keys=None):
    """A complete SRTP(AES_CM_128_HMAC_SHA1_80) packet at the given ROC."""
    enc, auth, salt = keys or session_keys()
    hdr = rtp_header(seq, ts, ssrc, pt=pt, marker=marker, cc=cc,
                     csrcs=csrcs, ext=ext)
    plain_pad = (b"\x00" * (pad - 1) + bytes([pad])) if pad else b""
    body = payload + plain_pad
    pidx = packet_index(roc, seq & 0xFFFF)
    iv = srtp_aes_cm_iv(salt, ssrc, pidx)
    cipher = _keystream_xor(enc, iv, body)
    pkt = hdr + cipher
    if corrupt:
        i = len(pkt) - TAG_LEN - 1
        pkt = pkt[:i] + bytes([pkt[i] ^ 0xFF]) + pkt[i + 1:]
    tag = hmac.new(auth, pkt + struct.pack(">I", roc),
                   hashlib.sha1).digest()[:TAG_LEN]
    return pkt + tag


def red_payload(blocks):
    """RFC 2198: blocks = [(pt, ts_offset_or_None, data)]; the LAST one
    is the primary (F=0 header)."""
    hdrs = bytearray()
    for pt, off, data in blocks[:-1]:
        hdrs += bytes([0x80 | (pt & 0x7F),
                       (off >> 6) & 0xFF,
                       ((off & 0x3F) << 2) | (len(data) >> 8),
                       len(data) & 0xFF])
    pt = blocks[-1][0]
    hdrs += bytes([pt & 0x7F])
    body = b"".join(d for _, _, d in blocks)
    return bytes(hdrs) + body


def walk_header(pkt):
    """RTP header walk: returns hlen, or None on overflow (step 5)."""
    if len(pkt) < 12 or (pkt[0] >> 6) != 2:
        return None
    cc = pkt[0] & 0x0F
    hlen = 12 + 4 * cc
    if pkt[0] & 0x10:
        if len(pkt) < hlen + 4:
            return None
        words = struct.unpack(">H", pkt[hlen + 2:hlen + 4])[0]
        hlen += 4 + 4 * words
    if hlen + TAG_LEN > len(pkt):
        return None
    return hlen


_CFG = (480, 960, 1920, 2880, 480, 960, 1920, 2880,
        480, 960, 1920, 2880, 480, 960, 480, 960,
        120, 240, 480, 960, 120, 240, 480, 960,
        120, 240, 480, 960, 120, 240, 480, 960)


def toc_samples(payload):
    if not payload:
        return -1
    toc = payload[0]
    code = toc & 3
    fs = _CFG[toc >> 3]
    if code == 0:
        return fs
    if code in (1, 2):
        return 2 * fs
    if len(payload) < 2:
        return -1
    m = payload[1] & 0x3F
    if m == 0 or m > 48:
        return -1
    return m * fs


class ArxModel(object):
    """Mirror of the kn_arx contract (module docstring = the spec)."""

    def __init__(self):
        self.c = dict(consumed=0, packets=0, payload_bytes=0, red_packets=0,
                      red_blocks=0, red_invalid=0, pt_filtered=0,
                      malformed=0, toc_invalid=0, padded=0, seq_loss=0,
                      seq_reorder=0, seq_dup=0, ssrc_changes=0,
                      srtp_fail=0, replay_reject=0, srtp_nokey=0)
        self.keys = None
        self.highest_index = None      # SRTP replay state (per epoch)
        self.seen = set()
        self.ssrc = None
        self.highest_seq = None        # seq accounting (per epoch)
        self.emitted = []
        self.samples = []

    def set_srtp(self, master_key=MASTER_KEY, master_salt=MASTER_SALT):
        self.keys = session_keys(master_key, master_salt)

    def _reset_epoch(self):
        self.highest_index = None
        self.seen = set()
        self.highest_seq = None

    def _guess_index(self, seq):
        if self.highest_index is None:
            return seq
        highest_seq = self.highest_index & 0xFFFF
        roc = self.highest_index >> 16
        delta = seq - highest_seq
        if delta > 32767:
            delta -= 65536
        elif delta < -32768:
            delta += 65536
        if delta > 0 and seq < highest_seq:
            roc += 1                    # forward wrap
        elif delta < 0 and seq > highest_seq:
            if roc == 0:
                return None             # ReplayRejected (underflow)
            roc -= 1
        return packet_index(roc, seq)

    def _replay_ok(self, index):
        if self.highest_index is None:
            return True
        if index > self.highest_index:
            return True
        if self.highest_index - index >= 2048:
            return False
        return index not in self.seen

    def process(self, pkt):
        c = self.c
        c["consumed"] += 1
        if len(pkt) < 12 or (pkt[0] >> 6) != 2:       # step 2
            c["malformed"] += 1
            return -1
        pt = pkt[1] & 0x7F
        if pt not in (PT_OPUS, PT_RED):               # step 3
            c["pt_filtered"] += 1
            return 0
        if len(pkt) > KN_ARX_MAX_PKT:                 # step 4
            c["malformed"] += 1
            return -1
        hlen = walk_header(pkt)
        if hlen is None:                              # step 5
            c["malformed"] += 1
            return -1
        if self.keys is None:                         # step 6
            c["srtp_nokey"] += 1
            return -1
        ssrc = struct.unpack(">I", pkt[8:12])[0]
        seq = struct.unpack(">H", pkt[2:4])[0]
        if self.ssrc is None:                         # step 7
            self.ssrc = ssrc
            self._reset_epoch()
        elif ssrc != self.ssrc:
            c["ssrc_changes"] += 1
            self.ssrc = ssrc
            self._reset_epoch()
        index = self._guess_index(seq)                # step 8
        if index is None or not self._replay_ok(index):
            c["replay_reject"] += 1
            return -1
        enc, auth, salt = self.keys                   # step 9
        roc = index >> 16
        body, tag = pkt[hlen:-TAG_LEN], pkt[-TAG_LEN:]
        expect = hmac.new(auth, pkt[:-TAG_LEN] + struct.pack(">I", roc),
                          hashlib.sha1).digest()[:TAG_LEN]
        if not hmac.compare_digest(expect, tag):
            c["srtp_fail"] += 1
            return -1
        plain = _keystream_xor(enc, srtp_aes_cm_iv(salt, ssrc, index),
                               body)
        if self.highest_index is None or index > self.highest_index:
            self.highest_index = index
        self.seen.add(index)
        # step 10 — seq accounting (wire truth, before codec checks)
        if self.highest_seq is None:
            self.highest_seq = seq
        else:
            delta = seq - self.highest_seq
            if delta > 32767:
                delta -= 65536
            elif delta < -32768:
                delta += 65536
            if delta > 0:
                c["seq_loss"] += delta - 1
                self.highest_seq = seq
            elif delta == 0:
                c["seq_dup"] += 1
            else:
                c["seq_reorder"] += 1
        if pkt[0] & 0x20:
            c["padded"] += 1
        # step 11 — padding strip
        if pkt[0] & 0x20:
            pad = plain[-1] if plain else 0
            if pad == 0 or pad > len(plain):
                c["malformed"] += 1
                return -1
            plain = plain[:-pad]
        payload = plain
        if pt == PT_RED:                              # step 12
            c["red_packets"] += 1
            payload = self._red_primary(payload)
            if payload is None:
                c["red_invalid"] += 1
                return -1
        samples = toc_samples(payload)                # step 13
        if samples < 0:
            c["toc_invalid"] += 1
            return -1
        self.emitted.append(payload)                  # step 14
        self.samples.append(samples)
        c["packets"] += 1
        c["payload_bytes"] += len(payload)
        return 1

    def _red_primary(self, payload):
        blocks = []
        off = 0
        while True:
            if off >= len(payload):
                return None
            b0 = payload[off]
            if b0 & 0x80:
                if off + 4 > len(payload):
                    return None
                blen = ((payload[off + 2] & 0x03) << 8) | payload[off + 3]
                blocks.append((b0 & 0x7F, blen))
                off += 4
            else:
                blocks.append((b0 & 0x7F, None))
                off += 1
                break
        body_len = len(payload) - off
        total = sum(bl for _, bl in blocks[:-1])
        if blocks[-1][1] is None:
            primary_len = body_len - total
        else:
            return None
        if primary_len < 0 or total > body_len:
            return None
        for _ in blocks[:-1]:
            self.c["red_blocks"] += 1
        start = off + total
        return payload[start:start + primary_len]


# --------------------------------------------------------------- vectors
def _groups():
    g = []

    # g1 plain — one packet (+ derived session keys ride as KATs)
    m = ArxModel()
    m.set_srtp()
    p1 = build_srtp(100, 48000, SSRC_A, opus_pkt(0xF8, 79, 0x11))
    g.append(("plain", [p1], [m.process(p1)], m))

    # g2 CSRC x2 + one-word extension + 4-byte padding
    m = ArxModel()
    m.set_srtp()
    op = opus_pkt(0xF8, 60, 0x22)
    p1 = build_srtp(200, 96000, SSRC_A, op, cc=2, csrcs=(1, 2),
                    ext=b"\xAA\xBB\xCC", pad=4)
    g.append(("cc_ext_pad", [p1], [m.process(p1)], m))

    # g3 RED with one redundant block + primary
    m = ArxModel()
    m.set_srtp()
    prev = opus_pkt(0xF8, 70, 0x31)
    cur = opus_pkt(0xF8, 100, 0x32)
    red = red_payload([(PT_OPUS, 960, prev), (PT_OPUS, None, cur)])
    p1 = build_srtp(300, 144000, SSRC_A, red, pt=PT_RED)
    g.append(("red_two_blocks", [p1], [m.process(p1)], m))

    # g4 RED invalid — declared blocks exceed the payload body
    m = ArxModel()
    m.set_srtp()
    bad = bytes([0x80 | PT_OPUS, 0x00, 0x02, 0x00, PT_OPUS]) + b"\x01" * 10
    #  ^ one redundant header claiming 512 bytes; body is 10 → invalid
    p1 = build_srtp(400, 192000, SSRC_A, bad, pt=PT_RED)
    g.append(("red_invalid", [p1], [m.process(p1)], m))

    # g5 sequence maths: 1,2,4,3,4(replay-rejected),5
    m = ArxModel()
    m.set_srtp()
    pkts = [build_srtp(s, s * 960, SSRC_A, opus_pkt(0xF8, 20, 0x40 + i))
            for i, s in enumerate([1, 2, 4, 3, 4, 5])]
    g.append(("seq_math", pkts, [m.process(p) for p in pkts], m))

    # g6 wrong PT + too-short datagram
    m = ArxModel()
    m.set_srtp()
    p1 = build_srtp(500, 0, SSRC_A, opus_pkt(0xF8, 30), pt=96)
    p2 = b"\x80\x6f\x00\x01"          # V=2 PT111 but 4 bytes
    g.append(("pt_and_short", [p1, p2],
              [m.process(p1), m.process(p2)], m))

    # g7 corrupted auth tag
    m = ArxModel()
    m.set_srtp()
    p1 = build_srtp(600, 0, SSRC_A, opus_pkt(0xF8, 40), corrupt=True)
    g.append(("bad_tag", [p1], [m.process(p1)], m))

    # g8 SSRC change — new epoch resets replay + seq tracking
    m = ArxModel()
    m.set_srtp()
    p1 = build_srtp(700, 0, SSRC_A, opus_pkt(0xF8, 30, 0x51))
    p2 = build_srtp(10, 960, SSRC_B, opus_pkt(0xF8, 30, 0x52))
    g.append(("ssrc_change", [p1, p2], [m.process(p1), m.process(p2)], m))

    # g9 no keys armed
    m = ArxModel()
    p1 = build_srtp(800, 0, SSRC_A, opus_pkt(0xF8, 30))
    g.append(("no_key", [p1], [m.process(p1)], m))

    # g10 zero-length Opus payload → toc_invalid
    m = ArxModel()
    m.set_srtp()
    p1 = build_srtp(900, 0, SSRC_A, b"")
    g.append(("empty_payload", [p1], [m.process(p1)], m))

    # g11 seq wrap 65535 → 0 (ROC estimation; p2 is tagged at ROC 1)
    m = ArxModel()
    m.set_srtp()
    p1 = build_srtp(65535, 0, SSRC_A, opus_pkt(0xF8, 25, 0x61))
    p2 = build_srtp(0, 960, SSRC_A, opus_pkt(0xF8, 25, 0x62), roc=1)
    g.append(("seq_wrap", [p1, p2], [m.process(p1), m.process(p2)], m))

    # g12 SILK WB 40 ms code-0 + code-3 M=3 (mixed durations)
    m = ArxModel()
    m.set_srtp()
    p1 = build_srtp(1000, 0, SSRC_A, opus_pkt((10 << 3) | 0, 50, 0x71))
    p2 = build_srtp(1001, 1920, SSRC_A,
                    bytes([(10 << 3) | 3, 3]) + b"\x72" * 120)
    g.append(("mixed_durations", [p1, p2], [m.process(p1), m.process(p2)],
              m))
    return g


def _selfcheck():
    enc, auth, salt = session_keys()
    assert len(enc) == 16 and len(auth) == 20 and len(salt) == 14
    pkt = build_srtp(4242, 12345, SSRC_A, opus_pkt(0xF8, 50))
    m = ArxModel()
    m.set_srtp()
    assert m.process(pkt) == 1
    assert m.emitted[0] == opus_pkt(0xF8, 50)
    bad = pkt[:-1] + bytes([pkt[-1] ^ 0x01])
    m2 = ArxModel()
    m2.set_srtp()
    assert m2.process(bad) == -1 and m2.c["srtp_fail"] == 1
    # every group's recorded rcs must reproduce on a fresh model
    for name, pkts, rcs, mm in _groups():
        m3 = ArxModel()
        if name != "no_key":
            m3.set_srtp()
        got = [m3.process(p) for p in pkts]
        assert got == list(rcs), (name, got, rcs)
        assert m3.c == mm.c, (name, m3.c, mm.c)
        assert m3.emitted == mm.emitted, name
    print("arx_reference: self-check OK (%d groups)" % len(_groups()))


def _hx(data):
    return "".join("%02x" % b for b in data)


def emit(path="streamer/src/kn_arx_kat_vectors.h"):
    groups = _groups()
    enc, auth, salt = session_keys()
    L = []
    L.append("/* kn_arx_kat_vectors.h — GENERATED by tools/arx_reference.py.")
    L.append(" * Do not edit by hand; regenerate + rerun the gates.")
    L.append(" * Bundle-audio SRTP vectors: RFC 3711 AES_CM_128_HMAC_SHA1_80,")
    L.append(" * RFC 2198 RED, RFC 3550 seq accounting (ROC 0 except the")
    L.append(" * seq_wrap group's second packet, ROC 1). See docs/AUDIO.md.")
    L.append(" */")
    L.append("#ifndef KN_ARX_KAT_VECTORS_H")
    L.append("#define KN_ARX_KAT_VATORS_H".replace("VATORS", "VECTORS"))
    L.append("")
    L.append("#define KN_ARX_KAT_MAX_PKT %du" % KN_ARX_MAX_PKT)
    L.append("")
    for nm, val in (("master_key", MASTER_KEY), ("master_salt", MASTER_SALT),
                    ("sess_enc", enc), ("sess_auth", auth),
                    ("sess_salt", salt)):
        L.append("static const unsigned char kn_arx_%s[%d] = {"
                 % (nm, len(val)))
        L.append("    " + ",".join("0x%02x" % b for b in val) + "};")
    L.append("")
    L.append("typedef struct {")
    L.append("    const unsigned char *pkt; unsigned pkt_len;")
    L.append("    int expect_rc;             /* 1 emitted / 0 filtered / -1 */")
    L.append("    const char *emit_hex;      /* emitted payload, \"\" = none */")
    L.append("    unsigned emit_samples48;   /* 0 when nothing emitted */")
    L.append("} kn_arx_pkt_vec;")
    L.append("")
    L.append("typedef struct {")
    L.append("    const char *name;")
    L.append("    const kn_arx_pkt_vec *pkts; unsigned n_pkts;")
    L.append("    unsigned consumed, packets, payload_bytes, red_packets;")
    L.append("    unsigned red_blocks, red_invalid, pt_filtered, malformed;")
    L.append("    unsigned toc_invalid, padded, seq_loss, seq_reorder;")
    L.append("    unsigned seq_dup, ssrc_changes, srtp_fail, replay_reject;")
    L.append("    unsigned srtp_nokey;")
    L.append("} kn_arx_group;")
    L.append("")
    vi = 0
    for gi, (name, pkts, rcs, m) in enumerate(groups):
        mm = ArxModel()
        if name != "no_key":
            mm.set_srtp()
        vecs = []
        for i, (p, rc) in enumerate(zip(pkts, rcs)):
            before = len(mm.emitted)
            mm.process(p)
            new = mm.emitted[before:]
            sam = mm.samples[before:]
            ehex = _hx(new[0]) if new else ""
            esam = sam[0] if sam else 0
            L.append("static const unsigned char k_arx_p%d[%d] = {"
                     % (vi, max(1, len(p))))
            row = p if p else b"\x00"
            for j in range(0, len(row), 16):
                L.append("    " + ",".join("0x%02x" % b
                                           for b in row[j:j + 16]) + ",")
            L.append("};")
            vecs.append('    { k_arx_p%d, %du, %d, "%s", %du },'
                        % (vi, len(p), rc, ehex, esam))
            vi += 1
        L.append("static const kn_arx_pkt_vec k_arx_g%d_pkts[] = {" % gi)
        L.extend(vecs)
        L.append("};")
    L.append("")
    L.append("static const kn_arx_group kn_arx_groups[] = {")
    for gi, (name, pkts, rcs, m) in enumerate(groups):
        c = m.c
        L.append('    { "%s", k_arx_g%d_pkts, %du,'
                 % (name, gi, len(pkts)))
        L.append("      %u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u,"
                 " %u },"
                 % (c["consumed"], c["packets"], c["payload_bytes"],
                    c["red_packets"], c["red_blocks"], c["red_invalid"],
                    c["pt_filtered"], c["malformed"], c["toc_invalid"],
                    c["padded"], c["seq_loss"], c["seq_reorder"],
                    c["seq_dup"], c["ssrc_changes"], c["srtp_fail"],
                    c["replay_reject"], c["srtp_nokey"]))
    L.append("};")
    L.append("#define KN_ARX_GROUPS %d" % len(groups))
    L.append("#define KN_ARX_TOTAL_PKTS %d" % vi)
    L.append("")
    L.append("#endif /* KN_ARX_KAT_VECTORS_H */")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L) + "\n")
    print("arx_reference: wrote %s (%d groups, %d packet vectors)"
          % (path, len(groups), vi))


if __name__ == "__main__":
    _selfcheck()
    if "--check" not in sys.argv:
        emit()
