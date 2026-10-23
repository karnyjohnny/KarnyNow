#!/usr/bin/env python3
"""gs_reference.py — independent Python reference for the NVST RTP/GS parse
layer and generator of the C known-answer vectors (BOOTSTRAP §7.9 "gs.parse").

Line-by-line port of nvst.rs @ a49b124 (local copy: artifacts/nvst-extract/):
  RtpHeader::parse                  2456-2510   (fixed header, CSRC, extension,
                                                 GS profile 0x4753 strictness)
  RtpHeader::payload                2512-2525   (padding strip)
  NvVideoPacket::parse              3228-3249   (16-byte little-endian GS body)
  is_start_of_frame/is_end_of_frame 3251-3257
  strip_trailing_access_unit_delim  3458-3479
  video_picture_payload             3481-3497   (≤68-byte start-code search;
                                                 AV1 fixed 8/20 offsets)
  video_access_unit_is_keyframe     3500-3523
  find_annex_b_start_code           3525-3534   (windows(3) semantics!)
  FecPacketLayout::from_packet      3677-3701
  flags validation                  4089        (recovered-packet path)
  stream-index continuity           4813-4824   ((last+1) & 0xFFFFFF)
Plus the kn_ring_dump line format (streamer/src/knring.c dump_visit:
"%llu %08x %u %u" + " %02x"*len + "\\n").

Before emitting anything this script PROVES itself:
  1. hand-written byte goldens (ext16 + RTP header) derived independently of
     the builder — the LE/BE and ext[11] pitfall traps (BOOTSTRAP §8 #1/#2),
  2. build->parse round-trips over the full field space (asymmetric values
     catch any endianness or field-offset mistake),
  3. discriminating checks: the same bytes parsed BIG-endian or with a
     "separate block-index field" reading MUST disagree with the reference
     (a vector that cannot distinguish the pitfall is worthless),
  4. find_annex_b_start_code boundary semantics (windows(3): last 3-byte
     offset is len-3; 4-byte variant needs offset+3 < slice len),
  5. hexdump render->parse round-trip with the exact fprintf format.
If any self-check fails, NOTHING is emitted — the vectors are then wrong,
not the code under test.

Usage:  python3 tools/gs_reference.py
Emits:  streamer/src/kn_gs_kat_vectors.h
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "streamer", "src", "kn_gs_kat_vectors.h")

# ------------------------------------------------------------- constants
# (nvst.rs:79, 212, 236-237, 263-274)

RTP_FIXED_HEADER_LEN = 12
NV_VIDEO_PACKET_LEN = 16
GS_VIDEO_EXTENSION_PROFILE = 0x4753
FLAG_CONTAINS_PIC_DATA = 0x01
FLAG_EOF = 0x02
FLAG_SOF = 0x04
STREAM_PACKET_INDEX_MASK = 0x00FFFFFF
MAX_GS_FRAME_HEADER_BYTES = 64
GS_SHORT_FRAME_HEADER_BYTES = 8       # AV1: payload[0] == 0x01
GFN_EXTENDED_FRAME_HEADER_BYTES = 20  # AV1: payload[0] == 0x81
START_CODE_SEARCH = MAX_GS_FRAME_HEADER_BYTES + 4   # = 68 (nvst.rs:3484)

H264, H265, AV1 = 0, 1, 2

# C error codes (kn_gs.h) — order mirrors the reference's error precedence
OK = 0
E_TOO_SHORT = -1
E_INVALID_VERSION = -2
E_INVALID_CSRC = -3
E_INVALID_EXT = -4
E_INVALID_PADDING = -5
E_MISSING_NV_VIDEO = -6
E_NO_PICTURE = -7
E_BAD_HEX = -8
E_HEX_CAP = -9
E_FEC_LAYOUT = -10


# ------------------------------------------------------------- primitives

def be16(b):
    return (b[0] << 8) | b[1]


def be32(b):
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]


def le32(b):
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)


def w32be(v):
    return bytes([(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])


def w32le(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])


# --------------------------------------------------------------- builders

def build_gs_ext(spi, frame_index, flags, cur=0, last=0, pct=0, fidx=0,
                 fsrc=0, low8=0, r9=0, r10=0):
    """The 16-byte GS body exactly as the wire carries it (LE words;
    ext[11] = block indices in the flags word's top byte — pitfall #2)."""
    packet_word = ((spi & STREAM_PACKET_INDEX_MASK) << 8) | (low8 & 0xFF)
    ext11 = ((last & 3) << 6) | ((cur & 3) << 4)
    flags_word = (flags & 0x0F) | ((r9 & 0xFF) << 8) | ((r10 & 0xFF) << 16) \
        | (ext11 << 24)
    fec_word = ((pct & 0xFF) << 4) | ((fidx & 0x3FF) << 12) | ((fsrc & 0x3FF) << 22)
    return (w32le(packet_word) + w32le(frame_index & 0xFFFFFFFF)
            + w32le(flags_word) + w32le(fec_word))


def build_rtp(pt=96, seq=0, ts=0, ssrc=0, csrc=(), marker=0, gs_ext=None,
              ext_profile=GS_VIDEO_EXTENSION_PROFILE, ext_words=None,
              ext_body=b"", payload=b"", pad_count=0, version=2):
    """Full RTP packet builder. gs_ext (16 B) -> GS extension; ext_profile/
    ext_words/ext_body allow malformed and foreign-extension vectors."""
    cc = len(csrc)
    first = ((version & 3) << 6) | ((1 if pad_count else 0) << 5) \
        | ((1 if (gs_ext is not None or ext_body or ext_words) else 0) << 4) | cc
    out = bytes([first, (marker << 7) | (pt & 0x7F)])
    out += bytes([seq >> 8, seq & 0xFF]) + w32be(ts) + w32be(ssrc)
    for c in csrc:
        out += w32be(c)
    if gs_ext is not None:
        assert len(gs_ext) == 16
        words = 4 if ext_words is None else ext_words
        out += bytes([(ext_profile >> 8) & 0xFF, ext_profile & 0xFF])
        out += bytes([words >> 8, words & 0xFF]) + gs_ext
    elif ext_body or ext_words:
        words = (len(ext_body) + 3) // 4 if ext_words is None else ext_words
        out += bytes([ext_profile >> 8, ext_profile & 0xFF])
        out += bytes([words >> 8, words & 0xFF])
        out += ext_body + b"\x00" * ((4 - len(ext_body) % 4) % 4)
    body = payload
    if pad_count:
        body += bytes(pad_count - 1) + bytes([pad_count])
    return out + body


# ------------------------------------------------- reference parse (ports)

def rtp_parse(pkt):
    """Port of RtpHeader::parse + payload() (nvst.rs:2456-2525).
    Returns (rc, dict). Field names match kn_gs_rtp."""
    if len(pkt) < RTP_FIXED_HEADER_LEN:
        return E_TOO_SHORT, None
    first = pkt[0]
    if first >> 6 != 2:
        return E_INVALID_VERSION, None
    cc = first & 0x0F
    payload_off = RTP_FIXED_HEADER_LEN + cc * 4
    if len(pkt) < payload_off:
        return E_INVALID_CSRC, None
    has_gs = 0
    gs_off = 0
    if first & 0x10:
        if len(pkt) < payload_off + 4:
            return E_INVALID_EXT, None
        profile = be16(pkt[payload_off:payload_off + 2])
        words = be16(pkt[payload_off + 2:payload_off + 4])
        ext_len = words * 4 + 4
        ext_end = payload_off + ext_len
        if len(pkt) < ext_end:
            return E_INVALID_EXT, None
        if profile == GS_VIDEO_EXTENSION_PROFILE:
            if words * 4 != NV_VIDEO_PACKET_LEN:      # nvst.rs:2490 (strict!)
                return E_INVALID_EXT, None
            gs_off = payload_off + 4
            has_gs = 1
        payload_off = ext_end                          # foreign ext: skipped
    end = len(pkt)
    has_padding = 1 if (first & 0x20) else 0
    if has_padding:
        padding = pkt[-1]
        if padding == 0 or padding > end - payload_off:
            return E_INVALID_PADDING, None
        end -= padding
    return OK, {
        "pt": pkt[1] & 0x7F,
        "seq": be16(pkt[2:4]),
        "ts": be32(pkt[4:8]),
        "ssrc": be32(pkt[8:12]),
        "payload_off": payload_off,
        "payload_len": end - payload_off,
        "has_padding": has_padding,
        "has_gs": has_gs,
        "gs_off": gs_off,
    }


def video_parse(ext):
    """Port of NvVideoPacket::parse (nvst.rs:3228-3257)."""
    packet_word = le32(ext[0:4])
    flags_word = le32(ext[8:12])
    fec_word = le32(ext[12:16])
    ext11 = ext[11]
    pct = (fec_word >> 4) & 0xFF
    fidx = (fec_word >> 12) & 0x3FF
    fsrc = (fec_word >> 22) & 0x3FF
    flags = flags_word & 0x0F
    cur = (ext11 >> 4) & 0x03
    last = (ext11 >> 6) & 0x03
    return {
        "spi": (packet_word >> 8) & STREAM_PACKET_INDEX_MASK,
        "frame_index": le32(ext[4:8]),
        "flags": flags,
        "cur": cur,
        "last": last,
        "pct": pct,
        "fidx": fidx,
        "fsrc": fsrc,
        "is_fec": 1 if (pct != 0 and fidx >= fsrc) else 0,
        "sof": 1 if (flags & FLAG_SOF and cur == 0) else 0,
        "eof": 1 if (flags & FLAG_EOF and cur == last) else 0,
        "flags_valid": 1 if (flags & ~(FLAG_SOF | FLAG_EOF
                                       | FLAG_CONTAINS_PIC_DATA)) == 0 else 0,
    }


def find_annex_b_start_code(b):
    """Port of nvst.rs:3525-3534 — bytes.windows(3) semantics: offsets
    0..len-3; at each offset the 3-byte pattern wins over the 4-byte one."""
    for off in range(0, len(b) - 2):
        if b[off:off + 3] == b"\x00\x00\x01":
            return (off, 3)
        if b[off:off + 3] == b"\x00\x00\x00" and off + 3 < len(b) \
                and b[off + 3] == 1:
            return (off, 4)
    return None


def video_picture_payload(codec, payload):
    """Port of nvst.rs:3481-3497. Returns (rc, off)."""
    if codec in (H264, H265):
        search_len = min(len(payload), START_CODE_SEARCH)
        r = find_annex_b_start_code(payload[:search_len])
        if r is None:
            return E_NO_PICTURE, 0
        return OK, r[0]
    if not payload:
        return E_NO_PICTURE, 0
    if payload[0] == 0x01:
        return (OK, GS_SHORT_FRAME_HEADER_BYTES) \
            if len(payload) >= GS_SHORT_FRAME_HEADER_BYTES else (E_NO_PICTURE, 0)
    if payload[0] == 0x81:
        return (OK, GFN_EXTENDED_FRAME_HEADER_BYTES) \
            if len(payload) >= GFN_EXTENDED_FRAME_HEADER_BYTES else (E_NO_PICTURE, 0)
    return E_NO_PICTURE, 0


def is_keyframe(codec, b, hint=0):
    """Port of nvst.rs:3500-3523."""
    if codec == AV1:
        return 1 if hint else 0
    off = 0
    while True:
        r = find_annex_b_start_code(b[off:])
        if r is None:
            return 0
        nal_start = off + r[0] + r[1]
        if nal_start < len(b):
            h = b[nal_start]
            if codec == H264 and h & 0x1F == 5:
                return 1
            if codec == H265 and 16 <= ((h >> 1) & 0x3F) <= 21:
                return 1
        off = nal_start


def strip_trailing_aud(codec, b):
    """Port of nvst.rs:3458-3479. Returns the new length."""
    if codec == AV1:
        return len(b)
    off = 0
    last_nal = None
    while True:
        r = find_annex_b_start_code(b[off:])
        if r is None:
            break
        start = off + r[0]
        nal_start = start + r[1]
        last_nal = (start, nal_start)
        off = nal_start
    if last_nal is not None:
        start, nal_start = last_nal
        if nal_start < len(b):
            h = b[nal_start]
            if codec == H264 and h & 0x1F == 9:
                return start
            if codec == H265 and (h >> 1) & 0x3F == 35:
                return start
    return len(b)


def fec_layout(ext):
    """Port of FecPacketLayout::from_packet (nvst.rs:3677-3701)."""
    frame_index = le32(ext[4:8])
    fec_word = le32(ext[12:16])
    pct = (fec_word >> 4) & 0xFF
    shard = (fec_word >> 12) & 0x3FF
    data = (fec_word >> 22) & 0x3FF
    if data == 0 or pct == 0:
        return E_FEC_LAYOUT, None
    parity = -(-(data * pct) // 100)          # div_ceil
    total = data + parity
    if parity == 0 or shard >= total or total > 1023:
        return E_FEC_LAYOUT, None
    return OK, {
        "frame_index": frame_index,
        "block": (ext[11] >> 4) & 0x03,
        "last_block": (ext[11] >> 6) & 0x03,
        "shard": shard, "data": data, "parity": parity,
    }


def index_continuous(last, idx):
    """nvst.rs:4813-4824 continuation rule."""
    return 1 if ((last + 1) & STREAM_PACKET_INDEX_MASK) == idx else 0


def hexdump_parse_line(line, cap=65536):
    """Port of the kn_ring_dump line format (knring.c dump_visit)."""
    s = line.rstrip("\r\n")
    toks = s.split(" ")
    if len(toks) < 4:
        return E_BAD_HEX, None
    try:
        mono = int(toks[0], 10)
        if len(toks[1]) != 8:
            return E_BAD_HEX, None
        meta = int(toks[1], 16)
        flags = int(toks[2], 10)
        ln = int(toks[3], 10)
    except ValueError:
        return E_BAD_HEX, None
    if mono < 0 or meta < 0 or flags < 0 or ln < 0:
        return E_BAD_HEX, None
    if len(toks) != 4 + ln:
        return E_BAD_HEX, None
    payload = bytearray()
    for t in toks[4:]:
        if len(t) != 2:
            return E_BAD_HEX, None
        try:
            payload.append(int(t, 16))
        except ValueError:
            return E_BAD_HEX, None
    if ln > cap:
        return E_HEX_CAP, None
    return OK, {"mono_us": mono, "meta": meta, "flags": flags, "len": ln,
                "payload": bytes(payload)}


def hexdump_render(mono_us, meta, flags, payload):
    """Exactly knring.c dump_visit's fprintf format."""
    s = "%d %08x %d %d" % (mono_us, meta, flags, len(payload))
    for b in payload:
        s += " %02x" % b
    return s + "\n"


# ------------------------------------------------------------ self-checks

def self_check():
    fails = []

    def want(cond, what):
        if not cond:
            fails.append(what)

    # 1. HAND-WRITTEN byte goldens (derived by hand from the spec tables,
    #    independently of build_gs_ext — the pitfall #1/#2 trap).
    #    spi=0x123456 low8=0xAB frame_index=0x01020304 flags=0x07
    #    cur=2 last=3 r9=0x5A r10=0xA5 pct=20 fidx=0x155 fsrc=0x2AA
    golden_ext = bytes.fromhex("ab563412" "04030201" "075aa5e0" "405195aa")
    ext = build_gs_ext(0x123456, 0x01020304, 0x07, cur=2, last=3, pct=20,
                       fidx=0x155, fsrc=0x2AA, low8=0xAB, r9=0x5A, r10=0xA5)
    want(ext == golden_ext, "gs ext builder == hand golden")
    v = video_parse(golden_ext)
    want(v["spi"] == 0x123456, "golden spi")
    want(v["frame_index"] == 0x01020304, "golden frame_index (LE)")
    want(v["flags"] == 7 and v["cur"] == 2 and v["last"] == 3,
         "golden flags/cur/last (ext[11])")
    want(v["pct"] == 20 and v["fidx"] == 0x155 and v["fsrc"] == 0x2AA,
         "golden fec word")
    want(v["is_fec"] == 0, "golden is_fec=0 (fidx < fsrc)")
    want(v["sof"] == 0, "golden sof=0 (cur != 0)")
    want(v["eof"] == 0, "golden eof=0 (cur != last)")
    want(v["flags_valid"] == 1, "golden flags valid")
    #    RTP header golden: V2 P0 X1 CC0 / M1 PT96 / seq 0x1234 /
    #    ts 0xDEADBEEF / ssrc 0x0BADC0DE / profile 4753 words 4
    golden_hdr = bytes.fromhex("90e01234deadbeef0badc0de47530004")
    pkt = build_rtp(pt=96, seq=0x1234, ts=0xDEADBEEF, ssrc=0x0BADC0DE,
                    marker=1, gs_ext=golden_ext, payload=b"\xaa\xbb")
    want(pkt[:16] == golden_hdr, "rtp header == hand golden")
    rc, h = rtp_parse(pkt)
    want(rc == OK and h["payload_off"] == 32 and h["payload_len"] == 2
         and h["has_gs"] == 1 and h["gs_off"] == 16 and h["pt"] == 96
         and h["seq"] == 0x1234 and h["ts"] == 0xDEADBEEF
         and h["ssrc"] == 0x0BADC0DE, "golden rtp parse")

    # 2. Discriminators: wrong readings MUST disagree.
    be_frame = be32(golden_ext[4:8])
    want(be_frame != v["frame_index"], "vector discriminates LE vs BE")
    wrong_cur = (golden_ext[8] >> 4) & 3     # "separate nibble" misreading
    want(wrong_cur != v["cur"], "vector discriminates ext[11] vs ext[8]")
    wrong_last = (golden_ext[12] >> 4) & 3
    want(wrong_last != v["last"], "vector discriminates ext[11] vs ext[12]")

    # 3. Round-trips over the field space (asymmetric values everywhere).
    rnd = random.Random(20261023)
    for _ in range(300):
        spi = rnd.getrandbits(24)
        fi = rnd.getrandbits(32)
        flags = rnd.getrandbits(4)
        cur, last = rnd.randrange(4), rnd.randrange(4)
        pct = rnd.getrandbits(8)
        fidx, fsrc = rnd.getrandbits(10), rnd.getrandbits(10)
        low8 = rnd.getrandbits(8)
        e = build_gs_ext(spi, fi, flags, cur, last, pct, fidx, fsrc, low8,
                         rnd.getrandbits(8), rnd.getrandbits(8))
        w = video_parse(e)
        want(w["spi"] == spi and w["frame_index"] == fi
             and w["flags"] == (flags & 0x0F) and w["cur"] == cur
             and w["last"] == last and w["pct"] == pct and w["fidx"] == fidx
             and w["fsrc"] == fsrc
             and w["is_fec"] == (1 if pct and fidx >= fsrc else 0)
             and w["sof"] == (1 if flags & FLAG_SOF and cur == 0 else 0)
             and w["eof"] == (1 if flags & FLAG_EOF and cur == last else 0),
             "video round-trip")
    for _ in range(200):
        kw = dict(pt=rnd.randrange(128), seq=rnd.getrandbits(16),
                  ts=rnd.getrandbits(32), ssrc=rnd.getrandbits(32),
                  marker=rnd.randrange(2),
                  csrc=tuple(rnd.getrandbits(32)
                             for _ in range(rnd.randrange(0, 3))))
        e = build_gs_ext(rnd.getrandbits(24), rnd.getrandbits(32),
                         rnd.getrandbits(3))
        pay = bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(0, 40)))
        pad = rnd.choice([0, 0, 0, 1, 4, 20])
        pkt = build_rtp(gs_ext=e, payload=pay, pad_count=pad, **kw)
        rc, h = rtp_parse(pkt)
        want(rc == OK, "rtp round-trip rc")
        if rc == OK:
            want(h["pt"] == kw["pt"] and h["seq"] == kw["seq"]
                 and h["ts"] == kw["ts"] and h["ssrc"] == kw["ssrc"]
                 and h["payload_off"] == 32 + 4 * len(kw["csrc"])
                 and h["payload_len"] == len(pay)
                 and h["has_padding"] == (1 if pad else 0)
                 and h["has_gs"] == 1
                 and pkt[h["gs_off"]:h["gs_off"] + 16] == e,
                 "rtp round-trip fields")

    # 4. Start-code search boundary semantics (windows(3)).
    want(find_annex_b_start_code(b"") is None, "sc empty")
    want(find_annex_b_start_code(b"\x00\x00") is None, "sc 2 bytes")
    want(find_annex_b_start_code(b"\x00\x00\x01") == (0, 3), "sc 3 bytes")
    want(find_annex_b_start_code(b"\x00\x00\x00\x01") == (0, 4), "sc 4 bytes")
    want(find_annex_b_start_code(b"\x00\x00\x00\x00\x01") == (1, 4),
         "sc 000001 -> offset 1")
    want(find_annex_b_start_code(b"\x01\x00\x00\x01") == (1, 3), "sc mid")
    # in a 68-byte search window: 3-byte code at 65 found (65+3 == 68)
    p68 = b"\xff" * 65 + b"\x00\x00\x01" + b"\x65"
    want(video_picture_payload(H264, p68) == (OK, 65), "sc at 65 found")
    # 4-byte code needing byte 68 -> NOT found within the window
    p68b = b"\xff" * 65 + b"\x00\x00\x00\x01" + b"\x65"
    want(video_picture_payload(H264, p68b) == (E_NO_PICTURE, 0),
         "sc 4-byte past window rejected")
    # ... but found when the search window is the whole (short) payload
    want(video_picture_payload(H264, b"\x00\x00\x00\x01\x65") == (OK, 0),
         "sc 4-byte at 0")

    # 5. Hexdump render/parse round-trip + golden line.
    golden_line = "1729657003123456 0000002a 1 4 " + "80 60 ab cd" + "\n"
    rc, rec = hexdump_parse_line(golden_line)
    want(rc == OK and rec["mono_us"] == 1729657003123456
         and rec["meta"] == 0x2A and rec["flags"] == 1 and rec["len"] == 4
         and rec["payload"] == bytes.fromhex("8060abcd"), "hex golden line")
    for _ in range(200):
        pay = bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(0, 60)))
        line = hexdump_render(rnd.getrandbits(48), rnd.getrandbits(32),
                              rnd.randrange(0, 65536), pay)
        rc, rec = hexdump_parse_line(line)
        want(rc == OK and rec["payload"] == pay, "hex round-trip")
    want(hexdump_parse_line("x y z")[0] == E_BAD_HEX, "hex malformed")
    want(hexdump_parse_line("1 00000000 0 2 aa")[0] == E_BAD_HEX,
         "hex short payload")

    # 6. FEC layout arithmetic goldens (div_ceil edges).
    e = build_gs_ext(0, 77, 0, cur=1, last=2, pct=25, fidx=12, fsrc=10)
    rc, lay = fec_layout(e)
    want(rc == OK and lay["parity"] == 3 and lay["data"] == 10
         and lay["shard"] == 12 and lay["block"] == 1 and lay["last_block"] == 2
         and lay["frame_index"] == 77, "fec layout 25%/10 -> parity 3")
    e = build_gs_ext(0, 1, 0, pct=100, fidx=23, fsrc=12)
    rc, lay = fec_layout(e)
    want(rc == OK and lay["parity"] == 12 and lay["shard"] == 23,
         "fec layout 100%/12 -> parity 12, shard 23 == total-1 ok")
    want(fec_layout(build_gs_ext(0, 1, 0, pct=100, fidx=24, fsrc=12))[0]
         == E_FEC_LAYOUT, "fec shard >= total rejected")
    want(fec_layout(build_gs_ext(0, 1, 0, pct=0, fidx=0, fsrc=10))[0]
         == E_FEC_LAYOUT, "fec pct=0 rejected")
    want(fec_layout(build_gs_ext(0, 1, 0, pct=20, fidx=0, fsrc=0))[0]
         == E_FEC_LAYOUT, "fec data=0 rejected")
    want(fec_layout(build_gs_ext(0, 1, 0, pct=100, fidx=511, fsrc=512))[0]
         == E_FEC_LAYOUT, "fec total 1024 > 1023 rejected")

    # 7. Continuity incl. the 24-bit wrap.
    want(index_continuous(0xFFFFFE, 0xFFFFFF) == 1, "cont pre-wrap")
    want(index_continuous(0xFFFFFF, 0x000000) == 1, "cont wrap")
    want(index_continuous(0xFFFFFF, 0x000001) == 0, "cont wrap+1 gap")
    want(index_continuous(5, 7) == 0, "cont gap")

    if fails:
        print("SELF-CHECK FAILED (%d):" % len(fails))
        for f in fails[:20]:
            print("  -", f)
        sys.exit(1)
    print("gs_reference.py self-check: %d round-trips + goldens OK"
          % (300 + 200 + 200))


# ------------------------------------------------------------ vector sets

def make_vectors():
    """All vectors below are SYNTHETIC (constructed from the spec). Real
    cloud captures arrive with stage 2 (kn_ring_dump files) — the self-test
    also parses those when present."""
    rtp = []      # (name, bytes, rc, fields-or-None)
    video = []    # (name, ext16, expected dict)
    payload = []  # (name, codec, bytes, rc, off)
    key = []      # (name, codec, bytes, hint, keyframe)
    aud = []      # (name, codec, bytes, newlen)
    fec = []      # (name, ext16, rc, dict)
    cont = []     # (last, idx, expected)
    hexl = []     # (name, line, rc, dict)

    gs = build_gs_ext(0x00ABCDE, 0x00000007, FLAG_SOF | FLAG_CONTAINS_PIC_DATA)
    idr = b"\x00\x00\x00\x01\x65\x88\x84\x00"          # IDR slice NAL
    hdr8 = b"\x47\x53\x46\x48\x00\x00\x00\x00"          # placeholder GS frame hdr

    # --- RTP packet vectors
    rtp.append(("minimal_gs_idr",
                build_rtp(seq=1, ts=3600, ssrc=0xDEADBEEF, gs_ext=gs,
                          marker=1, payload=hdr8 + idr),
                OK, dict(pt=96, seq=1, ts=3600, ssrc=0xDEADBEEF,
                         payload_off=32, payload_len=16, has_padding=0,
                         has_gs=1, gs_off=16)))
    rtp.append(("csrc2",
                build_rtp(csrc=(0x11111111, 0x22222222), gs_ext=gs,
                          payload=b"\x00\x00\x01\x41\x9a"),
                OK, dict(pt=96, seq=0, ts=0, ssrc=0, payload_off=40,
                         payload_len=5, has_padding=0, has_gs=1, gs_off=24)))
    rtp.append(("padding4",
                build_rtp(gs_ext=gs, payload=b"\x00\x00\x01\x41\x9a\x2f",
                          pad_count=4),
                OK, dict(pt=96, seq=0, ts=0, ssrc=0, payload_off=32,
                         payload_len=6, has_padding=1, has_gs=1, gs_off=16)))
    rtp.append(("foreign_ext_skipped",
                build_rtp(ext_profile=0xBEDE, ext_words=1,
                          ext_body=b"\x10\x20\x30\x40", gs_ext=None,
                          payload=b"\x00\x00\x01\x61"),
                OK, dict(pt=96, seq=0, ts=0, ssrc=0, payload_off=20,
                         payload_len=4, has_padding=0, has_gs=0, gs_off=0)))
    rtp.append(("no_extension",
                build_rtp(payload=b"\x00\x00\x01\x61"),
                OK, dict(pt=96, seq=0, ts=0, ssrc=0, payload_off=12,
                         payload_len=4, has_padding=0, has_gs=0, gs_off=0)))
    rtp.append(("too_short_11", bytes(range(11)), E_TOO_SHORT, None))
    rtp.append(("bad_version_0",
                b"\x00" + bytes(31), E_INVALID_VERSION, None))
    rtp.append(("bad_version_3",
                b"\xc0" + bytes(31), E_INVALID_VERSION, None))
    csrc_pkt = build_rtp(csrc=(1, 2, 3), gs_ext=gs, payload=b"xx")
    rtp.append(("csrc_truncated", csrc_pkt[:20], E_INVALID_CSRC, None))
    rtp.append(("ext_header_truncated",
                build_rtp(gs_ext=gs, payload=b"xx")[:14], E_INVALID_EXT, None))
    rtp.append(("ext_body_truncated",
                build_rtp(gs_ext=gs, payload=b"xx")[:24], E_INVALID_EXT, None))
    bad_words = build_rtp(ext_profile=GS_VIDEO_EXTENSION_PROFILE, ext_words=3,
                          ext_body=gs[:12], payload=b"xx")
    rtp.append(("gs_words_3_strict", bad_words, E_INVALID_EXT, None))
    rtp.append(("padding_zero",
                build_rtp(gs_ext=gs, payload=b"\x01\x02", pad_count=1)[:-1]
                + b"\x00", E_INVALID_PADDING, None))
    _ptb = bytearray(build_rtp(gs_ext=gs, payload=b"\x01\x02"))
    _ptb[0] |= 0x20                    # set the padding bit by hand
    _ptb += b"\x09"                     # padding byte > payload region (3)
    rtp.append(("padding_too_big", bytes(_ptb), E_INVALID_PADDING, None))
    rtp.append(("precedence_short_beats_version",
                b"\xc0\x00", E_TOO_SHORT, None))
    rtp.append(("padding_all_payload",
                build_rtp(gs_ext=gs, payload=b"", pad_count=20),
                OK, dict(pt=96, seq=0, ts=0, ssrc=0, payload_off=32,
                         payload_len=0, has_padding=1, has_gs=1, gs_off=16)))

    # --- GS body vectors
    video.append(("golden_body", golden_ext_bytes(), dict(
        spi=0x123456, frame_index=0x01020304, flags=7, cur=2, last=3,
        pct=20, fidx=0x155, fsrc=0x2AA, is_fec=0, sof=0, eof=0,
        flags_valid=1)))
    video.append(("sof_first_block", build_gs_ext(1, 9, FLAG_SOF, 0, 0), dict(
        spi=1, frame_index=9, flags=FLAG_SOF, cur=0, last=0, pct=0, fidx=0,
        fsrc=0, is_fec=0, sof=1, eof=0, flags_valid=1)))
    video.append(("sof_second_block_not_sof",
                  build_gs_ext(2, 9, FLAG_SOF, 1, 3), dict(
                      spi=2, frame_index=9, flags=FLAG_SOF, cur=1, last=3,
                      pct=0, fidx=0, fsrc=0, is_fec=0, sof=0, eof=0,
                      flags_valid=1)))
    video.append(("eof_last_block", build_gs_ext(3, 9, FLAG_EOF, 2, 2), dict(
        spi=3, frame_index=9, flags=FLAG_EOF, cur=2, last=2, pct=0, fidx=0,
        fsrc=0, is_fec=0, sof=0, eof=1, flags_valid=1)))
    video.append(("eof_not_last_block", build_gs_ext(4, 9, FLAG_EOF, 1, 2),
                  dict(spi=4, frame_index=9, flags=FLAG_EOF, cur=1, last=2,
                       pct=0, fidx=0, fsrc=0, is_fec=0, sof=0, eof=0,
                       flags_valid=1)))
    video.append(("fec_parity_shard",
                  build_gs_ext(5, 10, 0, 0, 0, pct=20, fidx=12, fsrc=10),
                  dict(spi=5, frame_index=10, flags=0, cur=0, last=0, pct=20,
                       fidx=12, fsrc=10, is_fec=1, sof=0, eof=0,
                       flags_valid=1)))
    video.append(("fec_data_shard",
                  build_gs_ext(6, 10, 0, 0, 0, pct=20, fidx=9, fsrc=10),
                  dict(spi=6, frame_index=10, flags=0, cur=0, last=0, pct=20,
                       fidx=9, fsrc=10, is_fec=0, sof=0, eof=0,
                       flags_valid=1)))
    video.append(("pct_zero_never_fec",
                  build_gs_ext(7, 11, 0, 0, 0, pct=0, fidx=500, fsrc=10),
                  dict(spi=7, frame_index=11, flags=0, cur=0, last=0, pct=0,
                       fidx=500, fsrc=10, is_fec=0, sof=0, eof=0,
                       flags_valid=1)))
    video.append(("invalid_flag_bit3", build_gs_ext(8, 12, 0x08), dict(
        spi=8, frame_index=12, flags=0x08, cur=0, last=0, pct=0, fidx=0,
        fsrc=0, is_fec=0, sof=0, eof=0, flags_valid=0)))
    video.append(("all_flags", build_gs_ext(9, 13, 0x0F, 0, 0), dict(
        spi=9, frame_index=13, flags=0x0F, cur=0, last=0, pct=0, fidx=0,
        fsrc=0, is_fec=0, sof=1, eof=1, flags_valid=0)))
    video.append(("spi_mask_24bit",
                  build_gs_ext(0xFFFFFF, 0xFFFFFFFF, 0, 0, 0, low8=0xFF),
                  dict(spi=0xFFFFFF, frame_index=0xFFFFFFFF, flags=0, cur=0,
                       last=0, pct=0, fidx=0, fsrc=0, is_fec=0, sof=0, eof=0,
                       flags_valid=1)))
    video.append(("fec_fields_max",
                  build_gs_ext(0, 0, 0, 3, 3, pct=0xFF, fidx=0x3FF,
                               fsrc=0x3FF),
                  dict(spi=0, frame_index=0, flags=0, cur=3, last=3,
                       pct=0xFF, fidx=0x3FF, fsrc=0x3FF, is_fec=1, sof=0,
                       eof=0, flags_valid=1)))
    video.append(("reserved_bytes_ignored",
                  build_gs_ext(0x55, 0x66, 0x01, 0, 0, r9=0xFF, r10=0xFF),
                  dict(spi=0x55, frame_index=0x66, flags=1, cur=0, last=0,
                       pct=0, fidx=0, fsrc=0, is_fec=0, sof=0, eof=0,
                       flags_valid=1)))

    # --- picture payload vectors
    payload.append(("start_at_0_4byte", H264, b"\x00\x00\x00\x01\x65\x01",
                    OK, 0))
    payload.append(("start_at_0_3byte", H264, b"\x00\x00\x01\x41\x01", OK, 0))
    payload.append(("gs_header_8_then_start", H264,
                    hdr8 + b"\x00\x00\x00\x01\x65", OK, 8))
    payload.append(("start_at_64_edge", H264,
                    b"\x11" * 64 + b"\x00\x00\x01\x65", OK, 64))
    payload.append(("start_3byte_at_65_edge", H264,
                    b"\x11" * 65 + b"\x00\x00\x01\x65", OK, 65))
    payload.append(("start_past_68_rejected", H264,
                    b"\x11" * 66 + b"\x00\x00\x01\x65", E_NO_PICTURE, 0))
    payload.append(("no_start_code", H264, b"\x11" * 80, E_NO_PICTURE, 0))
    payload.append(("empty_payload", H264, b"", E_NO_PICTURE, 0))
    payload.append(("av1_short_marker", AV1, b"\x01" + b"\x00" * 20, OK, 8))
    payload.append(("av1_extended_marker", AV1, b"\x81" + b"\x00" * 30,
                    OK, 20))
    payload.append(("av1_short_truncated", AV1, b"\x01" + b"\x00" * 6,
                    E_NO_PICTURE, 0))
    payload.append(("av1_unknown_marker", AV1, b"\x02" + b"\x00" * 30,
                    E_NO_PICTURE, 0))
    payload.append(("h265_same_search", H265,
                    b"\x22" * 12 + b"\x00\x00\x00\x01\x26\x01", OK, 12))

    # --- keyframe vectors
    key.append(("idr_present", H264,
                b"\x00\x00\x00\x01\x67\x64\x00\x1e"      # SPS
                + b"\x00\x00\x00\x01\x68\xee\x3c\x80"    # PPS
                + b"\x00\x00\x00\x01\x65\x88\x84\x00",   # IDR
                0, 1))
    key.append(("no_idr", H264,
                b"\x00\x00\x00\x01\x67\x64\x00\x1e"
                + b"\x00\x00\x01\x41\x9a\x2f", 0, 0))
    key.append(("aud_then_idr", H264,
                b"\x00\x00\x00\x01\x09\xf0"
                + b"\x00\x00\x00\x01\x65\x01", 0, 1))
    key.append(("h265_idr_w_radl", H265,
                b"\x00\x00\x00\x01\x26\x01", 0, 1))      # (0x26>>1)&0x3f = 19
    key.append(("h265_single_nal", H265,
                b"\x00\x00\x00\x01\x26", 0, 1))          # (0x26>>1)&0x3f = 19
    key.append(("av1_hint_true", AV1, b"\x01\x00", 1, 1))
    key.append(("av1_hint_false", AV1, b"\x01\x00", 0, 0))
    key.append(("trailing_start_no_header", H264,
                b"\x00\x00\x01\x41\x01" + b"\x00\x00\x01", 0, 0))

    # --- AUD strip vectors
    aud_body = b"\x00\x00\x00\x01\x65\x88\x84\x00"
    # NOTE: aud_body's trailing 0x00 MERGES with the AUD's 00 00 01 into a
    # 4-byte start code at index 7 — the reference truncates at the start
    # code offset (7), not after the body (8). Zero-stuffing is common in
    # real streams, so this vector pins the merge behaviour.
    aud.append(("strip_trailing_aud", H264,
                aud_body + b"\x00\x00\x01\x09\xf0", 7))
    aud.append(("keep_non_aud_tail", H264,
                aud_body + b"\x00\x00\x01\x41\x9a", len(aud_body) + 5))
    aud.append(("aud_not_last_untouched", H264,
                b"\x00\x00\x01\x09\xf0" + aud_body,
                5 + len(aud_body)))
    aud.append(("no_nals", H264, b"\x11\x22\x33", 3))
    aud.append(("av1_never_strips", AV1,
                b"\x00\x00\x01\x09\xf0", 5))
    aud.append(("h265_eos_nal_35", H265,
                b"\x00\x00\x00\x01\x02\x01" + b"\x00\x00\x00\x01\x46\x01",
                6))                                        # (0x46>>1)&0x3f=35
    aud.append(("trailing_start_no_header_kept", H264,
                aud_body + b"\x00\x00\x01", len(aud_body) + 3))

    # --- FEC layout vectors
    fec.append(("parity_20pct_10", build_gs_ext(0, 77, 0, 1, 2, 20, 11, 10),
                OK, dict(frame_index=77, block=1, last_block=2, shard=11,
                         data=10, parity=2)))
    fec.append(("shard_is_data", build_gs_ext(0, 77, 0, 0, 0, 20, 3, 10),
                OK, dict(frame_index=77, block=0, last_block=0, shard=3,
                         data=10, parity=2)))
    fec.append(("shard_out_of_range",
                build_gs_ext(0, 1, 0, 0, 0, 20, 12, 10), E_FEC_LAYOUT, None))
    fec.append(("pct_zero", build_gs_ext(0, 1, 0, 0, 0, 0, 0, 10),
                E_FEC_LAYOUT, None))
    fec.append(("data_zero", build_gs_ext(0, 1, 0, 0, 0, 20, 0, 0),
                E_FEC_LAYOUT, None))
    fec.append(("total_over_1023",
                build_gs_ext(0, 1, 0, 0, 0, 100, 0, 512), E_FEC_LAYOUT, None))
    fec.append(("div_ceil_round_up",
                build_gs_ext(0, 5, 0, 0, 0, 25, 12, 10), OK,
                dict(frame_index=5, block=0, last_block=0, shard=12, data=10,
                     parity=3)))
    fec.append(("minimal_1pct",
                build_gs_ext(0, 6, 0, 0, 0, 1, 100, 100), OK,
                dict(frame_index=6, block=0, last_block=0, shard=100,
                     data=100, parity=1)))

    # --- continuity vectors
    cont.append((0, 1, 1))
    cont.append((0xFFFFFE, 0xFFFFFF, 1))
    cont.append((0xFFFFFF, 0, 1))
    cont.append((0xFFFFFF, 1, 0))
    cont.append((1000, 1002, 0))
    cont.append((0xABCDEF, 0xABCDF0 % 0x1000000, 1))

    # --- hexdump line vectors
    sample = build_rtp(seq=77, ts=90000, ssrc=0x5150, gs_ext=gs,
                       payload=hdr8 + idr)
    hexl.append(("full_packet_line",
                 hexdump_render(1729657003999888, 0x2A, 1, sample),
                 OK, dict(mono_us=1729657003999888, meta=0x2A, flags=1,
                          payload=sample)))
    hexl.append(("empty_payload", hexdump_render(42, 0, 0, b""), OK,
                 dict(mono_us=42, meta=0, flags=0, payload=b"")))
    hexl.append(("golden", golden_line(), OK,
                 dict(mono_us=1729657003123456, meta=0x2A, flags=1,
                      payload=bytes.fromhex("8060abcd"))))
    hexl.append(("malformed_meta", "1 0000gg00 0 1 aa\n", E_BAD_HEX, None))
    hexl.append(("len_mismatch", "1 00000000 0 2 aa\n", E_BAD_HEX, None))
    hexl.append(("missing_fields", "1 00000000\n", E_BAD_HEX, None))
    hexl.append(("odd_hex_token", "1 00000000 0 1 a\n", E_BAD_HEX, None))

    return rtp, video, payload, key, aud, fec, cont, hexl


def golden_ext_bytes():
    return bytes.fromhex("ab563412" "04030201" "075aa5e0" "405195aa")


def golden_line():
    return "1729657003123456 0000002a 1 4 80 60 ab cd\n"


def check_vectors():
    """Prove every emitted vector against the reference BEFORE emission
    (the vectors self-prove first — the srtp_reference rule)."""
    rtp, video, payload, key, aud, fec, cont, hexl = make_vectors()
    for name, pkt, rc, exp in rtp:
        got_rc, got = rtp_parse(pkt)
        assert got_rc == rc, "rtp %s: rc %d != %d" % (name, got_rc, rc)
        if exp is not None:
            for k, v in exp.items():
                assert got[k] == v, "rtp %s: %s %r != %r" % (name, k, got[k], v)
    for name, ext, exp in video:
        got = video_parse(ext)
        for k, v in exp.items():
            assert got[k] == v, "video %s: %s %r != %r" % (name, k, got[k], v)
    for name, codec, pay, rc, off in payload:
        got_rc, got_off = video_picture_payload(codec, pay)
        assert got_rc == rc and (rc != OK or got_off == off), \
            "payload %s: (%d,%d) != (%d,%d)" % (name, got_rc, got_off, rc, off)
    for name, codec, b, hint, kf in key:
        assert is_keyframe(codec, b, hint) == kf, "key " + name
    for name, codec, b, newlen in aud:
        assert strip_trailing_aud(codec, b) == newlen, "aud " + name
    for name, ext, rc, exp in fec:
        got_rc, got = fec_layout(ext)
        assert got_rc == rc, "fec %s: rc" % name
        if exp is not None:
            for k, v in exp.items():
                assert got[k] == v, "fec %s: %s" % (name, k)
    for last, idx, exp in cont:
        assert index_continuous(last, idx) == exp, "cont %d->%d" % (last, idx)
    for name, line, rc, exp in hexl:
        got_rc, got = hexdump_parse_line(line)
        assert got_rc == rc, "hex %s: rc %d != %d" % (name, got_rc, rc)
        if exp is not None:
            for k, v in exp.items():
                assert got[k] == v, "hex %s: %s" % (name, k)
    print("vector self-proof OK: rtp=%d video=%d payload=%d key=%d aud=%d "
          "fec=%d cont=%d hex=%d" % (len(rtp), len(video), len(payload),
                                      len(key), len(aud), len(fec),
                                      len(cont), len(hexl)))
    return rtp, video, payload, key, aud, fec, cont, hexl


# ---------------------------------------------------------------- emission

def carr(name, data, per_line=16):
    out = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), per_line):
        chunk = ", ".join("0x%02x" % b for b in data[i:i + per_line])
        out.append("    " + chunk + ",")
    out.append("};")
    return "\n".join(out)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit(vecs):
    rtp, video, payload, key, aud, fec, cont, hexl = vecs
    L = []
    A = L.append
    A("/* kn_gs_kat_vectors.h -- GENERATED FILE, do not edit by hand.")
    A(" * Generator: python3 tools/gs_reference.py   (run from the repo root)")
    A(" *")
    A(" * Known-answer vectors for the NVST RTP/GS parse layer (BOOTSTRAP")
    A(" * sec. 7.9 \"gs.parse\"). Every constant was produced by the")
    A(" * independent Python reference in tools/gs_reference.py, a line-by-")
    A(" * line port of nvst.rs @ a49b124 (RtpHeader::parse 2456-2510,")
    A(" * NvVideoPacket::parse 3228-3249, find_annex_b_start_code 3525-3534,")
    A(" * video_picture_payload 3481-3497, keyframe 3500-3523, AUD strip")
    A(" * 3458-3479, FecPacketLayout 3677-3701, continuity 4813-4824) plus")
    A(" * the kn_ring_dump line format (knring.c dump_visit).")
    A(" *")
    A(" * All packets are SYNTHETIC (built from the spec). Real cloud captures")
    A(" * arrive with stage 2; the gs.parse self-test also parses a capture")
    A(" * file when one is present (KN_GS_CAPTURE / ./gs_capture.hex).")
    A(" *")
    A(" * If the C implementation disagrees with these vectors, the C code is")
    A(" * wrong -- never \"fix\" a vector to match code.")
    A(" */")
    A("#ifndef KN_GS_KAT_VECTORS_H")
    A("#define KN_GS_KAT_VECTORS_H")
    A("")

    # --- RTP packet vectors
    A("/* ---- RTP/GS packet vectors ---- */")
    A("typedef struct {")
    A("    const char *name;")
    A("    const unsigned char *pkt;")
    A("    unsigned len;")
    A("    int rc;")
    A("    unsigned pt, seq;")
    A("    unsigned long ts, ssrc;")
    A("    unsigned payload_off, payload_len;")
    A("    int has_padding, has_gs;")
    A("    unsigned gs_off;")
    A("} kn_gs_kat_rtp;")
    A("")
    rows = []
    for i, (name, pkt, rc, exp) in enumerate(rtp):
        an = "k_kat_rtp_pkt%d" % i
        A(carr(an, pkt))
        A("")
        if exp is None:
            rows.append('    { "%s", %s, %du, %d, 0,0, 0,0, 0,0, 0,0, 0 },'
                        % (name, an, len(pkt), rc))
        else:
            rows.append('    { "%s", %s, %du, %d, %du, %du, %lu, %lu, '
                        '%du, %du, %d, %d, %du },'
                        % (name, an, len(pkt), rc, exp["pt"], exp["seq"],
                           exp["ts"], exp["ssrc"], exp["payload_off"],
                           exp["payload_len"], exp["has_padding"],
                           exp["has_gs"], exp["gs_off"]))
    A("static const kn_gs_kat_rtp k_kat_rtp[] = {")
    L.extend(rows)
    A("};")
    A("#define K_KAT_RTP_N ((int)(sizeof k_kat_rtp / sizeof k_kat_rtp[0]))")
    A("")

    # --- GS body vectors
    A("/* ---- GS 16-byte body vectors (all fields little-endian) ---- */")
    A("typedef struct {")
    A("    const char *name;")
    A("    unsigned char ext[16];")
    A("    unsigned spi, frame_index;")
    A("    unsigned flags, cur, last;")
    A("    unsigned pct, fidx, fsrc;")
    A("    int is_fec, sof, eof, flags_valid;")
    A("} kn_gs_kat_video;")
    A("")
    A("static const kn_gs_kat_video k_kat_video[] = {")
    for name, ext, e in video:
        hexs = ", ".join("0x%02x" % b for b in ext)
        A('    { "%s", { %s }, %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, '
          '%d, %d, %d, %d },'
          % (name, hexs, e["spi"], e["frame_index"], e["flags"], e["cur"],
             e["last"], e["pct"], e["fidx"], e["fsrc"], e["is_fec"],
             e["sof"], e["eof"], e["flags_valid"]))
    A("};")
    A("#define K_KAT_VIDEO_N ((int)(sizeof k_kat_video / sizeof k_kat_video[0]))")
    A("")

    # --- picture payload vectors
    A("/* ---- video_picture_payload vectors (codec: 0=H264 1=H265 2=AV1) ---- */")
    A("typedef struct {")
    A("    const char *name;")
    A("    int codec;")
    A("    const unsigned char *pay;")
    A("    unsigned len;")
    A("    int rc;")
    A("    unsigned off;")
    A("} kn_gs_kat_payload;")
    A("")
    rows = []
    for i, (name, codec, pay, rc, off) in enumerate(payload):
        an = "k_kat_pay%d" % i
        if len(pay) == 0:
            A("static const unsigned char %s[1] = { 0x00 }; /* unused (len 0) */"
              % an)
        else:
            A(carr(an, pay, 24))
        A("")
        rows.append('    { "%s", %d, %s, %du, %d, %du },'
                    % (name, codec, an, len(pay), rc, off))
    A("static const kn_gs_kat_payload k_kat_payload[] = {")
    L.extend(rows)
    A("};")
    A("#define K_KAT_PAYLOAD_N ((int)(sizeof k_kat_payload / sizeof k_kat_payload[0]))")
    A("")

    # --- keyframe vectors
    A("/* ---- keyframe detection vectors ---- */")
    A("typedef struct {")
    A("    const char *name;")
    A("    int codec;")
    A("    const unsigned char *b;")
    A("    unsigned len;")
    A("    int hint;")
    A("    int keyframe;")
    A("} kn_gs_kat_key;")
    A("")
    rows = []
    for i, (name, codec, b, hint, kf) in enumerate(key):
        an = "k_kat_key%d" % i
        A(carr(an, b, 24))
        A("")
        rows.append('    { "%s", %d, %s, %du, %d, %d },'
                    % (name, codec, an, len(b), hint, kf))
    A("static const kn_gs_kat_key k_kat_key[] = {")
    L.extend(rows)
    A("};")
    A("#define K_KAT_KEY_N ((int)(sizeof k_kat_key / sizeof k_kat_key[0]))")
    A("")

    # --- AUD strip vectors
    A("/* ---- trailing-AUD strip vectors (newlen = length after strip) ---- */")
    A("typedef struct {")
    A("    const char *name;")
    A("    int codec;")
    A("    const unsigned char *b;")
    A("    unsigned len;")
    A("    unsigned newlen;")
    A("} kn_gs_kat_aud;")
    A("")
    rows = []
    for i, (name, codec, b, newlen) in enumerate(aud):
        an = "k_kat_aud%d" % i
        A(carr(an, b, 24))
        A("")
        rows.append('    { "%s", %d, %s, %du, %du },'
                    % (name, codec, an, len(b), newlen))
    A("static const kn_gs_kat_aud k_kat_aud[] = {")
    L.extend(rows)
    A("};")
    A("#define K_KAT_AUD_N ((int)(sizeof k_kat_aud / sizeof k_kat_aud[0]))")
    A("")

    # --- FEC layout vectors
    A("/* ---- FecPacketLayout::from_packet vectors ---- */")
    A("typedef struct {")
    A("    const char *name;")
    A("    unsigned char ext[16];")
    A("    int rc;")
    A("    unsigned frame_index, block, last_block, shard, data, parity;")
    A("} kn_gs_kat_fec;")
    A("")
    A("static const kn_gs_kat_fec k_kat_fec[] = {")
    for name, ext, rc, e in fec:
        hexs = ", ".join("0x%02x" % b for b in ext)
        if e is None:
            A('    { "%s", { %s }, %d, 0,0,0,0,0,0 },' % (name, hexs, rc))
        else:
            A('    { "%s", { %s }, %d, %uu, %uu, %uu, %uu, %uu, %uu },'
              % (name, hexs, rc, e["frame_index"], e["block"],
                 e["last_block"], e["shard"], e["data"], e["parity"]))
    A("};")
    A("#define K_KAT_FEC_N ((int)(sizeof k_kat_fec / sizeof k_kat_fec[0]))")
    A("")

    # --- continuity vectors
    A("/* ---- stream_packet_index continuity: {last, idx, continuous} ---- */")
    A("static const unsigned k_kat_cont[][3] = {")
    for last, idx, exp in cont:
        A("    { 0x%06xu, 0x%06xu, %du }," % (last, idx, exp))
    A("};")
    A("#define K_KAT_CONT_N ((int)(sizeof k_kat_cont / sizeof k_kat_cont[0]))")
    A("")

    # --- hexdump vectors
    A("/* ---- kn_ring_dump line vectors ---- */")
    A("typedef struct {")
    A("    const char *name;")
    A("    const char *line;")
    A("    int rc;")
    A("    unsigned long long mono_us;")
    A("    unsigned meta, flags, len;")
    A("    const unsigned char *payload;")
    A("} kn_gs_kat_hex;")
    A("")
    rows = []
    for i, (name, line, rc, exp) in enumerate(hexl):
        if exp is not None and len(exp["payload"]) > 0:
            an = "k_kat_hex_pay%d" % i
            A(carr(an, exp["payload"], 24))
            A("")
            payref = an
            paylen = len(exp["payload"])
        else:
            payref = "(const unsigned char *)\"\""
            paylen = 0
        rows.append('    { "%s", "%s", %d, %s, %s, %s, %du, %s },'
                    % (name, line.replace("\\", "\\\\").rstrip("\n"),
                       rc,
                       ("%dULL" % exp["mono_us"]) if exp else "0ULL",
                       ("0x%xu" % exp["meta"]) if exp else "0",
                       ("%uu" % exp["flags"]) if exp else "0",
                       paylen, payref))
    A("static const kn_gs_kat_hex k_kat_hex[] = {")
    L.extend(rows)
    A("};")
    A("#define K_KAT_HEX_N ((int)(sizeof k_kat_hex / sizeof k_kat_hex[0]))")
    A("")
    A("#endif /* KN_GS_KAT_VECTORS_H */")
    return "\n".join(L) + "\n"


def main():
    self_check()
    vecs = check_vectors()
    text = emit(vecs)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("written: %s (%d bytes)" % (OUT, len(text)))


if __name__ == "__main__":
    main()
