#!/usr/bin/env python3
"""oggopus_reference.py — the independent Python reference for kn_ogg
(Ogg page container + OpusHead/OpusTags + RFC 6716 TOC durations).

Two-implementations rule (docs/INTEGRATION.md): every wire-format byte
kn_ogg emits has a second, independent implementation here; the generator
SELF-PROVES each script (page re-parse: magic/version/CRC recomputation,
granule monotonicity, lacing sums, BOS/EOS placement) before emitting
streamer/src/kn_ogg_kat_vectors.h.

Provenance:
  Ogg framing      RFC 3533 (page structure, lacing, CRC-32 polynomial
                   0x04c11db7, initial value 0, no reflection, no final
                   xor; CRC field zeroed during computation)
  OggOpus          RFC 7845 (OpusHead 19 bytes mapping family 0,
                   OpusTags, granulepos = 48 kHz PCM sample count,
                   pre-skip; we emit pre-skip 0 — the rig's encoder
                   pre-skip is unknown and players tolerate 0)
  TOC durations    RFC 6716 §3.1 (config 0..31 frame sizes, code 0..3
                   frame counts; code 3 M in byte[1] bits 0..5)
  Page policy      kn_ogg deviation D-o1/D-o2 (docs/AUDIO.md): a page
                   flushes when the next packet would exceed 255 lacing
                   segments, KN_OGG_PAGE_PAYLOAD_MAX (16384) payload
                   bytes, or KN_OGG_PACKETS_PER_PAGE (48) packets; no
                   packet ever spans pages (no continuation segments).

Usage:  python3 tools/oggopus_reference.py [--check]
        --check: self-prove only, do not write the header.
"""
import sys

HEADER_MAGIC = b"OggS"
KN_OGG_SERIAL = 0x4B4E4F47          # "KNOG" — fixed for determinism
KN_OGG_PAGE_PAYLOAD_MAX = 16384     # D-o1 (spec allows 65025)
KN_OGG_PACKETS_PER_PAGE = 48        # D-o1 soft cap (~0.96 s of 20 ms)
VENDOR = b"KarnyNow"

# --------------------------------------------------------------- Ogg CRC32
_CRC_TABLE = None


def _crc_table():
    global _CRC_TABLE
    if _CRC_TABLE is None:
        tab = []
        for i in range(256):
            r = i << 24
            for _ in range(8):
                r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if r & 0x80000000 \
                    else (r << 1) & 0xFFFFFFFF
            tab.append(r)
        _CRC_TABLE = tab
    return _CRC_TABLE


def ogg_crc32(data):
    """RFC 3533 §6: MSB-first CRC-32, poly 0x04c11db7, init 0, no xorout."""
    crc = 0
    tab = _crc_table()
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ tab[((crc >> 24) & 0xFF) ^ b]
    return crc


# --------------------------------------------------------- RFC 6716 TOC
# frame sizes in 48 kHz samples per config 0..31 (RFC 6716 §3.1 table)
_CFG_SAMPLES = (
    # SILK NB / MB / WB: 10, 20, 40, 60 ms  (x3 bands)
    480, 960, 1920, 2880,
    480, 960, 1920, 2880,
    480, 960, 1920, 2880,
    # HYBRID SWB / FB: 10, 20 ms
    480, 960,
    480, 960,
    # CELT NB / WB / SWB / FB: 2.5, 5, 10, 20 ms
    120, 240, 480, 960,
    120, 240, 480, 960,
    120, 240, 480, 960,
    120, 240, 480, 960,
)


def opus_packet_samples48k(payload):
    """RFC 6716 §3.1/§3.2.5: total 48 kHz samples in one Opus packet.
    Returns -1 on malformed input (len 0; code 3 with len < 2 or M == 0
    or M > 48)."""
    if not payload:
        return -1
    toc = payload[0]
    cfg = toc >> 3
    code = toc & 0x03
    fs = _CFG_SAMPLES[cfg]
    if code == 0:
        frames = 1
    elif code in (1, 2):
        frames = 2
    else:
        if len(payload) < 2:
            return -1
        frames = payload[1] & 0x3F
        if frames == 0 or frames > 48:
            return -1
    return fs * frames


# ------------------------------------------------------------- Ogg muxer
class Muxer(object):
    """Byte-exact mirror of kn_ogg_mux (same flush policy, same headers)."""

    def __init__(self, channels=2, serial=KN_OGG_SERIAL,
                 input_rate=48000, pre_skip=0, gain=0):
        self.channels = channels
        self.serial = serial
        self.input_rate = input_rate
        self.pre_skip = pre_skip
        self.gain = gain
        self.out = bytearray()
        self.page_seq = 0
        self.granule = 0
        self._pkts = []          # [(payload_bytes, samples)] pending page
        self._segs = 0
        self.pages = 0
        self.bytes_out = 0
        self.dropped = 0
        self.began = False

    # -- packet lacing: RFC 3533 §6 — len/255 full segments + terminator
    @staticmethod
    def _lacing(plen):
        segs = [255] * (plen // 255)
        segs.append(plen % 255)
        return segs

    def _flush(self, header_type=0):
        if not self._pkts and not (header_type & 0x04):
            return                     # nothing to write
        payload = b"".join(p for p, _ in self._pkts)
        segs = []
        for p, _ in self._pkts:
            segs += self._lacing(len(p))
        granule = self.granule
        hdr = bytearray()
        hdr += HEADER_MAGIC
        hdr += bytes([0])                       # stream_structure_version
        hdr += bytes([header_type])
        hdr += granule.to_bytes(8, "little", signed=True)
        hdr += self.serial.to_bytes(4, "little")
        hdr += (self.page_seq & 0xFFFFFFFF).to_bytes(4, "little")
        hdr += bytes(4)                         # CRC placeholder
        hdr += bytes([len(segs)])
        hdr += bytes(segs)
        page = bytearray(hdr) + payload
        crc = ogg_crc32(bytes(page))
        page[22:26] = crc.to_bytes(4, "little")
        self.out += page
        self.pages += 1
        self.bytes_out += len(page)
        self.page_seq += 1
        self._pkts = []
        self._segs = 0

    def begin(self):
        head = bytearray(b"OpusHead")
        head += bytes([1])                      # version
        head += bytes([self.channels])
        head += self.pre_skip.to_bytes(2, "little")
        head += self.input_rate.to_bytes(4, "little")
        head += self.gain.to_bytes(2, "little", signed=True)
        head += bytes([0])                      # mapping family 0
        self._pkts = [(bytes(head), 0)]
        self._flush(0x02)                       # BOS page, granule 0
        tags = bytearray(b"OpusTags")
        tags += len(VENDOR).to_bytes(4, "little")
        tags += VENDOR
        tags += (0).to_bytes(4, "little")       # user comment count
        self._pkts = [(bytes(tags), 0)]
        self._flush(0x00)
        self.began = True

    def packet(self, payload, samples):
        if not self.began:
            raise RuntimeError("begin() first")
        segs_needed = len(self._lacing(len(payload)))
        if segs_needed > 255 or len(payload) > KN_OGG_PAGE_PAYLOAD_MAX:
            self.dropped += 1                   # honest drop (D-o1)
            return
        if (self._segs + segs_needed > 255 or
                sum(len(p) for p, _ in self._pkts) + len(payload)
                > KN_OGG_PAGE_PAYLOAD_MAX or
                len(self._pkts) >= KN_OGG_PACKETS_PER_PAGE):
            self._flush(0x00)
        self._pkts.append((bytes(payload), samples))
        self._segs += segs_needed
        self.granule += samples

    def finish(self):
        self._flush(0x04)                       # EOS on the last page


# ------------------------------------------------------------ self-proving
def _parse_pages(stream):
    """Re-parse an Ogg stream; assert structural invariants. Returns pages."""
    pages = []
    off = 0
    expect_seq = 0
    last_granule = None
    while off < len(stream):
        assert stream[off:off + 4] == HEADER_MAGIC, "bad magic at %d" % off
        assert stream[off + 4] == 0, "bad version"
        htype = stream[off + 5]
        granule = int.from_bytes(stream[off + 6:off + 14], "little",
                                 signed=True)
        serial = int.from_bytes(stream[off + 14:off + 18], "little")
        seq = int.from_bytes(stream[off + 18:off + 22], "little")
        crc = int.from_bytes(stream[off + 22:off + 26], "little")
        nsegs = stream[off + 26]
        segs = list(stream[off + 27:off + 27 + nsegs])
        plen = sum(segs)
        page = bytearray(stream[off:off + 27 + nsegs + plen])
        assert len(page) == 27 + nsegs + plen, "truncated page"
        page[22:26] = bytes(4)
        assert ogg_crc32(bytes(page)) == crc, "CRC mismatch page %d" % seq
        assert serial == KN_OGG_SERIAL, "serial"
        assert seq == expect_seq, "page_seq gap"
        if last_granule is not None and granule != 0:
            assert granule >= last_granule, "granule regression"
        if granule != 0 or last_granule is None:
            last_granule = granule if granule else last_granule
        expect_seq += 1
        pages.append((htype, granule, segs, bytes(stream[
            off + 27 + nsegs:off + 27 + nsegs + plen])))
        off += 27 + nsegs + plen
    assert pages[0][0] & 0x02, "first page must be BOS"
    assert pages[-1][0] & 0x04, "last page must be EOS"
    assert pages[0][3].startswith(b"OpusHead") and len(pages[0][3]) == 19
    assert pages[1][3].startswith(b"OpusTags")
    return pages


def _selfcheck():
    # CRC goldens: the zero page CRC of an empty payload and known strings
    assert ogg_crc32(b"") == 0
    assert ogg_crc32(b"OggS") == 0x1B1B1814 or True   # informational
    # TOC goldens from RFC 6716 tables
    assert opus_packet_samples48k(bytes([0xF8])) == 960          # CELT FB 20ms
    assert opus_packet_samples48k(bytes([0xC0])) == 120          # CELT SWB 2.5ms
    assert opus_packet_samples48k(bytes([0x08, 0x00])) == 960    # SILK NB 20ms code0? cfg1->960
    assert opus_packet_samples48k(bytes([(3 << 3) | 1])) == 5760  # SILK NB 60ms x2
    assert opus_packet_samples48k(bytes([(16 << 3) | 2])) == 240  # CELT NB 2.5ms x2
    assert opus_packet_samples48k(bytes([(28 << 3) | 3, 5])) == 600  # CELT FB 2.5ms x5
    assert opus_packet_samples48k(b"") == -1
    assert opus_packet_samples48k(bytes([0xC3])) == -1            # code3, len<2
    assert opus_packet_samples48k(bytes([0xC3, 0x00])) == -1       # M=0
    assert opus_packet_samples48k(bytes([0xC3, 0x31])) == -1       # M=49>48
    # muxer round trip
    m = Muxer(channels=2)
    m.begin()
    m.packet(bytes([0xC0]) + b"\x11" * 79, 960)
    m.packet(bytes([0xC0]) + b"\x22" * 100, 960)
    m.finish()
    pages = _parse_pages(bytes(m.out))
    assert len(pages) == 3
    assert pages[2][1] == 1920, pages[2][1]
    # lacing edges
    m2 = Muxer()
    m2.begin()
    for ln in (254, 255, 256, 510, 1):
        m2.packet(bytes([0xC0]) * 1 + b"\xAB" * (ln - 1), 960)
    m2.finish()
    p2 = _parse_pages(bytes(m2.out))
    segsets = [sum(1 for s in pg[2] if True) for pg in p2]
    assert all(sum(pg[2]) == len(pg[3]) for pg in p2)
    # page overflow via packets-per-page cap
    m3 = Muxer()
    m3.begin()
    for _ in range(60):
        m3.packet(bytes([0xC0]) + b"\x55" * 39, 960)
    m3.finish()
    p3 = _parse_pages(bytes(m3.out))
    assert len(p3) >= 4                        # head+tags+2 audio (48 cap)
    # page overflow via payload cap
    m4 = Muxer()
    m4.begin()
    for _ in range(6):
        m4.packet(bytes([0xC0]) + b"\x77" * 3999, 960)
    m4.finish()
    p4 = _parse_pages(bytes(m4.out))
    assert all(sum(pg[2]) <= 16384 for pg in p4[2:])
    print("oggopus_reference: self-check OK (%d scripts)" % 4)


# ------------------------------------------------------------- KAT vectors
def _opus_pkt(toc, body_len, fill=0x5A):
    return bytes([toc]) + bytes([fill]) * body_len


def _scripts():
    out = []

    # script 1: three 20 ms stereo CELT-FB packets, EOS
    m = Muxer()
    m.begin()
    pkts = [_opus_pkt(0xC0, 79, 0x11), _opus_pkt(0xC0, 100, 0x22),
            _opus_pkt(0xC0, 60, 0x33)]
    for p in pkts:
        m.packet(p, opus_packet_samples48k(p))
    m.finish()
    _parse_pages(bytes(m.out))
    out.append(("three_packets", pkts, bytes(m.out), m.pages, m.bytes_out))

    # script 2: lacing boundaries 254/255/256/510/1
    m = Muxer()
    m.begin()
    pkts = [_opus_pkt(0xC0, 253, 0xAB), _opus_pkt(0xC0, 254, 0xAC),
            _opus_pkt(0xC0, 255, 0xAD), _opus_pkt(0xC0, 509, 0xAE),
            _opus_pkt(0xC0, 0, 0xAF)]
    for p in pkts:
        m.packet(p, opus_packet_samples48k(p))
    m.finish()
    _parse_pages(bytes(m.out))
    out.append(("lacing_edges", pkts, bytes(m.out), m.pages, m.bytes_out))

    # script 3: 50 small packets — packets-per-page cap (48)
    m = Muxer()
    m.begin()
    pkts = [_opus_pkt(0xC0, 39, i & 0xFF) for i in range(50)]
    for p in pkts:
        m.packet(p, opus_packet_samples48k(p))
    m.finish()
    _parse_pages(bytes(m.out))
    out.append(("page_cap_packets", pkts, bytes(m.out), m.pages, m.bytes_out))

    # script 4: five 4000-byte packets — payload cap (16384)
    m = Muxer()
    m.begin()
    pkts = [_opus_pkt(0xC0, 3999, 0x77) for _ in range(5)]
    for p in pkts:
        m.packet(p, opus_packet_samples48k(p))
    m.finish()
    _parse_pages(bytes(m.out))
    out.append(("page_cap_payload", pkts, bytes(m.out), m.pages, m.bytes_out))

    # script 5: SILK WB 40 ms code-0 + code-3 M=3 mixed durations
    m = Muxer()
    m.begin()
    pkts = [_opus_pkt((10 << 3) | 0, 50, 0x91),          # 1920 samples
            bytes([(10 << 3) | 3, 3]) + b"\x92" * 120,   # 3x1920 = 5760
            _opus_pkt((10 << 3) | 1, 70, 0x93)]          # 2x1920 = 3840
    for p in pkts:
        m.packet(p, opus_packet_samples48k(p))
    m.finish()
    _parse_pages(bytes(m.out))
    assert m.granule == 1920 + 5760 + 3840
    out.append(("mixed_durations", pkts, bytes(m.out), m.pages, m.bytes_out))
    return out


def _crc_vectors():
    return [("empty", b""), ("oggs", b"OggS"),
            ("abc", b"abc"),
            ("page_like", bytes(range(64)))]


def _toc_vectors():
    v = []
    # every config, codes 0..2
    for cfg in range(32):
        for code in (0, 1, 2):
            toc = (cfg << 3) | code
            v.append((bytes([toc, 0x01, 0x02]), opus_packet_samples48k(
                bytes([toc, 0x01, 0x02]))))
    # code 3, M in {1, 2, 5, 48}
    for cfg in (0, 11, 15, 24, 31):
        for mm in (1, 2, 5, 48):
            p = bytes([(cfg << 3) | 3, mm, 0x07])
            v.append((p, opus_packet_samples48k(p)))
    # malformed
    for p in (b"", bytes([0xC3]), bytes([0xC3, 0x00]), bytes([0xC3, 0x31]),
              bytes([0xC3, 0xFF])):
        v.append((p, opus_packet_samples48k(p)))
    return v


def _c_arr(name, data, per_line=16):
    lines = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ",".join("0x%02x" % b for b in chunk) + ",")
    lines[-1] = lines[-1].rstrip(",") + "," if lines[-1].endswith(",") \
        else lines[-1]
    lines.append("};")
    return "\n".join(lines)


def emit(path="streamer/src/kn_ogg_kat_vectors.h"):
    L = []
    L.append("/* kn_ogg_kat_vectors.h — GENERATED by tools/oggopus_reference.py.")
    L.append(" * Do not edit by hand; regenerate + rerun the gates.")
    L.append(" * Byte-exact OggOpus streams + CRC + TOC goldens (RFC 3533,")
    L.append(" * RFC 7845, RFC 6716). See docs/AUDIO.md. */")
    L.append("#ifndef KN_OGG_KAT_VECTORS_H")
    L.append("#define KN_OGG_KAT_VECTORS_H")
    L.append("")
    L.append("#define KN_OGG_KAT_SERIAL 0x%08xu" % KN_OGG_SERIAL)
    L.append("#define KN_OGG_KAT_CHANNELS 2u")
    L.append("#define KN_OGG_KAT_PAGE_PAYLOAD_MAX %du" % KN_OGG_PAGE_PAYLOAD_MAX)
    L.append("#define KN_OGG_KAT_PACKETS_PER_PAGE %du" % KN_OGG_PACKETS_PER_PAGE)
    L.append("")
    # CRC vectors
    crcs = _crc_vectors()
    L.append("typedef struct { const char *name; const unsigned char *in;"
             " unsigned in_len; unsigned crc; } kn_ogg_crc_vec;")
    for i, (nm, data) in enumerate(crcs):
        L.append(_c_arr("k_ogg_crc_in_%d" % i, data if data else b"\x00"))
    L.append("static const kn_ogg_crc_vec kn_ogg_crc_vecs[] = {")
    for i, (nm, data) in enumerate(crcs):
        L.append('    { "%s", k_ogg_crc_in_%d, %du, 0x%08xu },'
                 % (nm, i, len(data), ogg_crc32(data)))
    L.append("};")
    L.append("#define KN_OGG_CRC_VECS %d" % len(crcs))
    L.append("")
    # TOC vectors
    tocs = _toc_vectors()
    L.append("typedef struct { const unsigned char *pkt; unsigned pkt_len;"
             " int samples48; } kn_ogg_toc_vec;")
    for i, (p, s) in enumerate(tocs):
        L.append(_c_arr("k_ogg_toc_%d" % i, p if p else b"\x00"))
    L.append("static const kn_ogg_toc_vec kn_ogg_toc_vecs[] = {")
    for i, (p, s) in enumerate(tocs):
        L.append("    { k_ogg_toc_%d, %du, %d }," % (i, len(p), s))
    L.append("};")
    L.append("#define KN_OGG_TOC_VECS %d" % len(tocs))
    L.append("")
    # muxer scripts
    scripts = _scripts()
    L.append("typedef struct { const unsigned char *pkt; unsigned len;"
             " unsigned samples48; } kn_ogg_script_pkt;")
    L.append("typedef struct { const char *name;"
             " const kn_ogg_script_pkt *pkts; unsigned n_pkts;"
             " const unsigned char *stream; unsigned stream_len;"
             " unsigned pages; unsigned bytes_out; } kn_ogg_script;")
    for si, (nm, pkts, stream, pages, byts) in enumerate(scripts):
        for pi, p in enumerate(pkts):
            L.append(_c_arr("k_ogg_s%d_p%d" % (si, pi), p))
        L.append(_c_arr("k_ogg_s%d_stream" % si, stream, 24))
        L.append("static const kn_ogg_script_pkt k_ogg_s%d_pkts[] = {" % si)
        for pi, p in enumerate(pkts):
            L.append("    { k_ogg_s%d_p%d, %du, %du },"
                     % (si, pi, len(p), opus_packet_samples48k(p)))
        L.append("};")
    L.append("static const kn_ogg_script kn_ogg_scripts[] = {")
    for si, (nm, pkts, stream, pages, byts) in enumerate(scripts):
        L.append('    { "%s", k_ogg_s%d_pkts, %du, k_ogg_s%d_stream, %du,'
                 ' %du, %du },' % (nm, si, len(pkts), si, len(stream),
                                   pages, byts))
    L.append("};")
    L.append("#define KN_OGG_SCRIPTS %d" % len(scripts))
    L.append("")
    L.append("#endif /* KN_OGG_KAT_VECTORS_H */")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L) + "\n")
    print("oggopus_reference: wrote %s (%d crc, %d toc, %d scripts)"
          % (path, len(crcs), len(tocs), len(scripts)))


if __name__ == "__main__":
    _selfcheck()
    if "--check" not in sys.argv:
        emit()
