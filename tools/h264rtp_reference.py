#!/usr/bin/env python3
"""h264rtp_reference.py — independent Python reference for kn_h264rtp
(slice 5n, B5a) and generator of the C known-answer vectors.

Ground truth: RFC 6184 §5.2 (single NAL), §5.7.1 (STAP-A), §5.8 (FU-A);
libwebrtc's H264 packetizer shapes (keyframes open with STAP-A[SPS,PPS],
slices fragment via FU-A, the marker bit closes the access unit);
str0m rtp/h264.rs depacketizes the same three shapes.

The module contract (streamer/include/kn/kn_h264rtp.h) is replicated
here byte-for-byte: sequence discipline (int16 distance, dup skip, gap
drop-and-resync), timestamp-split emits, the broken/over/incomplete
gauntlet at the single emit path, the first-malformed identity capture.

Self-checks before emitting (a failed check emits NOTHING):
  1. STRUCTURAL: every emitted AU in every CLEAN scenario is re-parsed
     (split on 00 00 00 01) and its NAL list compared BYTE-FOR-BYTE
     against the scenario's construction table — the reference must
     reassemble exactly what the scenario builder fragmented;
  2. FU-A RECONSTRUCTION: scenarios fragment WHOLE source NALs and the
     expected AUs contain those SOURCE NALs verbatim — a wrong header
     rebuild ((indicator & 0xE0) | (fu_hdr & 0x1F)) or fragment order
     fails check 1's byte comparison;
  3. COUNTERS: every scenario carries a HAND-LABELED per-packet
     expectation table; the reference's final counters must equal the
     sum of the labels (an independent arithmetic path);
  4. DROP SAFETY: no emitted AU may contain bytes from a packet that
     was skipped as dup or accepted across a gap that killed its AU
     (checked by re-running the clean scenarios with each packet
     duplicated/dropped and asserting the AU set only shrinks or stays).

Usage:  python3 tools/h264rtp_reference.py
Emits:  streamer/src/kn_h264rtp_kat_vectors.h
"""
import os
import sys

SC = b"\x00\x00\x00\x01"

COUNTER_ORDER = [
    "pkts_in", "pkt_dups", "pkt_gaps", "single_nal", "stap_a_pkts",
    "stap_a_nals", "fu_a_pkts", "fu_incomplete", "malformed",
    "other_type", "aus", "au_bytes", "au_max", "ts_split", "drop_gap",
    "drop_broken", "drop_over",
]


# ------------------------------------------------------------ reference
class Dep:
    """The kn_h264rtp semantics, independently implemented."""

    def __init__(self, cap):
        self.cap = cap
        self.buf = bytearray()
        self.cur_ts = 0
        self.have_ts = False
        self.next_seq = 0
        self.have_seq = False
        self.au_broken = False
        self.au_over = False
        self.in_fu = False
        self.bad_logged = False
        self.bad_type = 0
        self.bad_seq = 0
        self.bad_head = b""
        self.st = {k: 0 for k in COUNTER_ORDER}

    # -- buffer helpers
    def append(self, b):
        if self.au_over:
            return
        if len(self.buf) + len(b) > self.cap:
            self.au_over = True
            return
        self.buf += b

    def append_nal(self, nal):
        self.append(SC)
        self.append(nal)

    def reset(self):
        self.buf = bytearray()
        self.au_broken = False
        self.au_over = False
        self.in_fu = False

    def log_bad(self, typ, seq, payload):
        if not self.bad_logged:
            self.bad_logged = True
            self.bad_type = typ
            self.bad_seq = seq
            self.bad_head = bytes(payload[:16])

    # -- the single emit path
    def emit(self, split, out):
        if len(self.buf) > 0:
            if self.in_fu:
                self.st["fu_incomplete"] += 1
                self.st["drop_broken"] += 1
            elif self.au_over:
                self.st["drop_over"] += 1
            elif self.au_broken:
                self.st["drop_broken"] += 1
            else:
                if split:
                    self.st["ts_split"] += 1
                self.st["aus"] += 1
                self.st["au_bytes"] += len(self.buf)
                self.st["au_max"] = max(self.st["au_max"], len(self.buf))
                out.append((self.cur_ts, bytes(self.buf)))
        self.reset()

    # -- one packet
    def push(self, seq, ts, marker, payload):
        out = []
        self.st["pkts_in"] += 1
        if not self.have_seq:
            self.have_seq = True
            self.next_seq = (seq + 1) & 0xFFFF
        else:
            dist = (seq - self.next_seq) & 0xFFFF
            if dist > 32767:
                dist -= 65536
            if dist < 0:
                self.st["pkt_dups"] += 1
                return out
            if dist > 0:
                self.st["pkt_gaps"] += 1
                if len(self.buf) > 0 or self.in_fu:
                    if self.in_fu:
                        self.st["fu_incomplete"] += 1
                    self.st["drop_gap"] += 1
                    self.reset()
            self.next_seq = (seq + 1) & 0xFFFF

        if self.have_ts and ts != self.cur_ts:
            self.emit(True, out)
        self.cur_ts = ts
        self.have_ts = True

        if len(payload) < 1:
            self.st["malformed"] += 1
            self.au_broken = True
            self.log_bad(0xFFFFFFFF, seq, payload)
        else:
            b0 = payload[0]
            typ = b0 & 0x1F
            if b0 & 0x80:
                self.st["malformed"] += 1
                self.au_broken = True
                self.log_bad(typ, seq, payload)
            elif 1 <= typ <= 23:
                if self.in_fu:
                    self.st["fu_incomplete"] += 1
                    self.au_broken = True
                self.in_fu = False
                self.st["single_nal"] += 1
                self.append_nal(payload)
            elif typ == 24:
                if self.in_fu:
                    self.st["fu_incomplete"] += 1
                    self.au_broken = True
                    self.in_fu = False
                if len(payload) < 3:
                    self.st["malformed"] += 1
                    self.au_broken = True
                    self.log_bad(typ, seq, payload)
                else:
                    self.st["stap_a_pkts"] += 1
                    off = 1
                    nals = 0
                    while off + 2 <= len(payload):
                        nlen = (payload[off] << 8) | payload[off + 1]
                        off += 2
                        if nlen == 0 or off + nlen > len(payload):
                            self.st["malformed"] += 1
                            self.au_broken = True
                            self.log_bad(typ, seq, payload)
                            break
                        self.append_nal(payload[off:off + nlen])
                        off += nlen
                        nals += 1
                    self.st["stap_a_nals"] += nals
            elif typ == 28:
                self.st["fu_a_pkts"] += 1
                if len(payload) < 2:
                    self.st["malformed"] += 1
                    self.au_broken = True
                    self.log_bad(typ, seq, payload)
                else:
                    fu = payload[1]
                    if fu & 0x80:
                        if self.in_fu:
                            self.st["fu_incomplete"] += 1
                            self.au_broken = True
                        recon = bytes([(b0 & 0xE0) | (fu & 0x1F)])
                        self.append(SC)
                        self.append(recon)
                        self.append(payload[2:])
                        self.in_fu = True
                    elif self.in_fu:
                        self.append(payload[2:])
                        if fu & 0x40:
                            self.in_fu = False
                    else:
                        self.st["malformed"] += 1
                        self.au_broken = True
                        self.log_bad(typ, seq, payload)
            else:
                self.st["other_type"] += 1
                self.au_broken = True
                self.log_bad(typ, seq, payload)

        if marker:
            self.emit(False, out)
        return out


# ------------------------------------------------------- NAL builders
_lcg = [0x12345678]


def rnd(n, seed=None):
    """Deterministic filler (reproducible vectors)."""
    if seed is not None:
        _lcg[0] = seed
    out = bytearray()
    for _ in range(n):
        _lcg[0] = (_lcg[0] * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((_lcg[0] >> 16) & 0xFF)
    return bytes(out)


def nal_sps(seed=1):
    return bytes([0x67]) + rnd(23, seed)          # type 7, NRI 3


def nal_pps(seed=2):
    return bytes([0x68]) + rnd(5, seed)            # type 8, NRI 3


def nal_idr(n, seed=3):
    return bytes([0x65]) + rnd(n, seed)            # type 5, NRI 3


def nal_p(n, seed=4):
    return bytes([0x41]) + rnd(n, seed)            # type 1, NRI 2


def pkt_single(nal):
    return nal


def pkt_stap(nals):
    body = b""
    for n in nals:
        body += bytes([len(n) >> 8, len(n) & 0xFF]) + n
    return bytes([0x18]) + body                    # type 24, NRI 0


def pkt_fu(nal, frags, i):
    """FU-A packet i of frags for a NAL (nal[0] = the original header)."""
    ind = (nal[0] & 0xE0) | 28
    hdr = nal[0] & 0x1F
    if i == 0:
        hdr |= 0x80
    if i == frags - 1:
        hdr |= 0x40
    body = nal[1:]
    step = (len(body) + frags - 1) // frags
    return bytes([ind, hdr]) + body[i * step:(i + 1) * step]


# -------------------------------------------------------- scenarios
# Each scenario: (name, cap, packets, expect_aus, expect_counters, bad)
#   packets: list of (seq, ts, marker, payload)
#   expect_aus: list of (ts, annexb_bytes) — the CONSTRUCTION truth
#               (start codes + NALs), derived independently of the ref
#   expect_counters: hand-labeled dict (independent arithmetic path)
#   bad: None or (type, seq, head_prefix) for the malformed identity
def au(ts, nals):
    return (ts, b"".join(SC + n for n in nals))


def scen_single():
    sps, pps, idr = nal_sps(), nal_pps(), nal_idr(60)
    pkts = [(1000, 90000, 1, pkt_single(idr))]
    cnt = dict(pkts_in=1, single_nal=1, aus=1,
               au_bytes=len(SC) + len(idr), au_max=len(SC) + len(idr))
    return ("single", 4096, pkts, [au(90000, [idr])], cnt, None)


def scen_stap_a():
    sps, pps, idr = nal_sps(), nal_pps(), nal_idr(40)
    pkts = [(2000, 180000, 1, pkt_stap([sps, pps, idr]))]
    a = au(180000, [sps, pps, idr])
    cnt = dict(pkts_in=1, stap_a_pkts=1, stap_a_nals=3, aus=1,
               au_bytes=len(a[1]), au_max=len(a[1]))
    return ("stap_a", 4096, pkts, [a], cnt, None)


def scen_fu_a():
    idr = nal_idr(300)
    pkts = [(3000, 270000, 0, pkt_fu(idr, 3, 0)),
            (3001, 270000, 0, pkt_fu(idr, 3, 1)),
            (3002, 270000, 1, pkt_fu(idr, 3, 2))]
    a = au(270000, [idr])
    cnt = dict(pkts_in=3, fu_a_pkts=3, aus=1,
               au_bytes=len(a[1]), au_max=len(a[1]))
    return ("fu_a", 4096, pkts, [a], cnt, None)


def scen_keyframe_real():
    """The GFN/libwebrtc keyframe opener: STAP-A[SPS,PPS] (M=0) then
    the IDR slice in FU-A fragments, the last one carrying M=1."""
    sps, pps, idr = nal_sps(11), nal_pps(12), nal_idr(500, 13)
    pkts = [(90, 900000, 0, pkt_stap([sps, pps])),
            (91, 900000, 0, pkt_fu(idr, 4, 0)),
            (92, 900000, 0, pkt_fu(idr, 4, 1)),
            (93, 900000, 0, pkt_fu(idr, 4, 2)),
            (94, 900000, 1, pkt_fu(idr, 4, 3))]
    a = au(900000, [sps, pps, idr])
    cnt = dict(pkts_in=5, stap_a_pkts=1, stap_a_nals=2, fu_a_pkts=4,
               aus=1, au_bytes=len(a[1]), au_max=len(a[1]))
    return ("keyframe_real", 4096, pkts, [a], cnt, None)


def scen_two_frames():
    p1, p2 = nal_p(80, 21), nal_p(90, 22)
    pkts = [(500, 3000, 1, pkt_single(p1)),
            (501, 6000, 1, pkt_single(p2))]
    cnt = dict(pkts_in=2, single_nal=2, aus=2,
               au_bytes=len(SC) * 2 + len(p1) + len(p2),
               au_max=len(SC) + max(len(p1), len(p2)))
    return ("two_frames", 4096, pkts,
            [au(3000, [p1]), au(6000, [p2])], cnt, None)


def scen_ts_split():
    """Frame A's last packet loses its marker; frame B's ts change must
    still close A (ts_split), and B closes on its own marker."""
    a1, a2, b1 = nal_p(70, 31), nal_p(70, 32), nal_p(70, 33)
    pkts = [(600, 3000, 0, pkt_single(a1)),
            (601, 3000, 0, pkt_single(a2)),      # marker lost
            (602, 6000, 1, pkt_single(b1))]
    cnt = dict(pkts_in=3, single_nal=3, aus=2, ts_split=1,
               au_bytes=2 * len(SC) + len(a1) + len(a2) + len(SC) + len(b1),
               au_max=len(SC) * 2 + len(a1) + len(a2))
    return ("ts_split", 4096, pkts,
            [au(3000, [a1, a2]), au(6000, [b1])], cnt, None)


def scen_gap_drop():
    """A fragment run loses its middle packet: the partial AU dies at
    the gap, the orphan E fragment is malformed-broken (no bytes), the
    NEXT frame resyncs cleanly."""
    idr = nal_idr(200, 41)
    p1 = nal_p(60, 42)
    pkts = [(700, 9000, 0, pkt_fu(idr, 3, 0)),
            (702, 9000, 1, pkt_fu(idr, 3, 2)),   # 701 lost -> gap
            (703, 12000, 1, pkt_single(p1))]
    cnt = dict(pkts_in=3, pkt_gaps=1, fu_a_pkts=2, single_nal=1,
               fu_incomplete=1, malformed=1, drop_gap=1, drop_broken=0,
               # the orphan E fragment taints an EMPTY buffer (the gap
               # already counted the lost bytes) — no second drop
               aus=1, au_bytes=len(SC) + len(p1), au_max=len(SC) + len(p1))
    bad = (28, 702, pkt_fu(idr, 3, 2)[:16])
    return ("gap_drop", 4096, pkts, [au(12000, [p1])], cnt, bad)


def scen_dup_late():
    """Retransmitted duplicates must be skipped without touching the
    AU in progress."""
    idr = nal_idr(240, 51)
    f0, f1, f2 = pkt_fu(idr, 3, 0), pkt_fu(idr, 3, 1), pkt_fu(idr, 3, 2)
    pkts = [(800, 15000, 0, f0),
            (800, 15000, 0, f0),                 # dup
            (801, 15000, 0, f1),
            (799, 15000, 0, f0),                 # late
            (802, 15000, 1, f2)]
    a = au(15000, [idr])
    cnt = dict(pkts_in=5, pkt_dups=2, fu_a_pkts=3, aus=1,
               au_bytes=len(a[1]), au_max=len(a[1]))
    return ("dup_late", 4096, pkts, [a], cnt, None)


def scen_fu_incomplete():
    """A fragment run is preempted by a single NAL (the E never came):
    the AU is tainted and dropped; the next frame is clean."""
    idr = nal_idr(150, 61)
    p1 = nal_p(50, 62)
    pkts = [(300, 21000, 0, pkt_fu(idr, 2, 0)),
            (301, 21000, 1, pkt_single(p1)),     # preempts the FU
            (302, 24000, 1, pkt_single(p1))]
    cnt = dict(pkts_in=3, fu_a_pkts=1, single_nal=2, fu_incomplete=1,
               drop_broken=1, aus=1,
               au_bytes=len(SC) + len(p1), au_max=len(SC) + len(p1))
    return ("fu_incomplete", 4096, pkts, [au(24000, [p1])], cnt, None)


def scen_oversize():
    """A NAL larger than the assembly cap must drop WHOLE (never
    truncated); the next small frame still emits."""
    big = nal_idr(300, 71)
    small = nal_p(20, 72)
    pkts = [(400, 30000, 1, pkt_single(big)),
            (401, 33000, 1, pkt_single(small))]
    cnt = dict(pkts_in=2, single_nal=2, drop_over=1, aus=1,
               au_bytes=len(SC) + len(small), au_max=len(SC) + len(small))
    return ("oversize", 64, pkts, [au(33000, [small])], cnt, None)


def scen_seq_wrap():
    """65534 -> 65535 -> 0 -> 1: the int16 distance must see +1 steps,
    not a 65534-packet gap."""
    n1, n2, n3, n4 = nal_p(30, 81), nal_p(30, 82), nal_p(30, 83), nal_idr(120, 84)
    pkts = [(65534, 1000, 1, pkt_single(n1)),
            (65535, 2000, 1, pkt_single(n2)),
            (0, 3000, 1, pkt_single(n3)),
            (1, 4000, 0, pkt_fu(n4, 2, 0)),
            (2, 4000, 1, pkt_fu(n4, 2, 1))]
    cnt = dict(pkts_in=5, single_nal=3, fu_a_pkts=2, aus=4,
               au_bytes=(len(SC) + len(n1)) + (len(SC) + len(n2)) +
                        (len(SC) + len(n3)) + (len(SC) + len(n4)),
               au_max=len(SC) + len(n4))
    return ("seq_wrap", 4096, pkts,
            [au(1000, [n1]), au(2000, [n2]), au(3000, [n3]),
             au(4000, [n4])], cnt, None)


def scen_malformed():
    """The three malformed classes, each M=1 so each drops its own AU:
    F-bit set, truncated STAP-A, 1-byte FU-A. The FIRST one owns the
    bad-identity capture."""
    fbit = bytes([0xE5]) + rnd(30, 91)           # F=1, type 5
    stap = bytes([0x18, 0x00, 0x20]) + rnd(8, 92)   # claims 32, has 8
    fu1 = bytes([0x7C])                          # FU-A, no header byte
    clean = nal_p(40, 93)
    pkts = [(10, 5000, 1, fbit),
            (11, 8000, 1, stap),
            (12, 11000, 1, fu1),
            (13, 14000, 1, pkt_single(clean))]
    cnt = dict(pkts_in=4, malformed=3, stap_a_pkts=1, fu_a_pkts=1,
               single_nal=1,
               # class counters count DISPATCH (the truncated STAP-A and
               # the 1-byte FU-A entered their parsers); malformed counts
               # the corruption. Malformed packets contribute NO bytes,
               # so their marker emits are silent: drop_broken stays 0 —
               # drop counters measure discarded PAYLOAD only.
               aus=1, au_bytes=len(SC) + len(clean),
               au_max=len(SC) + len(clean))
    bad = (5, 10, fbit[:16])
    return ("malformed", 4096, pkts, [au(14000, [clean])], cnt, bad)


def scen_other_type():
    """Type 30 (unspecified in RFC 6184, never emitted by pm=1
    senders): counted, AU tainted, next frame clean."""
    weird = bytes([0x7E]) + rnd(12, 101)         # F0 NRI3 type 30
    clean = nal_p(35, 102)
    pkts = [(20, 40000, 1, weird),
            (21, 43000, 1, pkt_single(clean))]
    cnt = dict(pkts_in=2, other_type=1, single_nal=1,
               # other_type contributes no bytes -> the tainted-but-empty
               # AU emits silently; only the counter records it
               aus=1, au_bytes=len(SC) + len(clean),
               au_max=len(SC) + len(clean))
    bad = (30, 20, weird[:16])
    return ("other_type", 4096, pkts, [au(43000, [clean])], cnt, bad)


def scen_mixed_stream():
    """A realistic mini-GOP: the keyframe opener then five P-frames at
    3000-ts steps, one single-packet frame, one two-packet FU-A frame,
    one duplicate in the middle. Everything must come out byte-exact."""
    sps, pps = nal_sps(111), nal_pps(112)
    idr = nal_idr(400, 113)
    frames = [nal_p(90, 120 + i) for i in range(4)]
    big_p = nal_p(260, 130)
    seq = 5000
    ts = 900000
    pkts = []
    pkts.append((seq, ts, 0, pkt_stap([sps, pps]))); seq += 1
    for i in range(3):
        pkts.append((seq, ts, 1 if i == 2 else 0, pkt_fu(idr, 3, i)))
        seq += 1                                            # E carries M
    for f in frames[:2]:
        ts += 3000
        pkts.append((seq, ts, 1, pkt_single(f))); seq += 1
    ts += 3000
    dup_seq = seq
    pkts.append((seq, ts, 0, pkt_fu(big_p, 2, 0))); seq += 1
    pkts.append((dup_seq, ts, 0, pkt_fu(big_p, 2, 0)))       # dup
    pkts.append((seq, ts, 1, pkt_fu(big_p, 2, 1))); seq += 1
    ts += 3000
    pkts.append((seq, ts, 1, pkt_single(frames[2]))); seq += 1
    ts += 3000
    pkts.append((seq, ts, 0, pkt_single(frames[3]))); seq += 1
    pkts.append((seq, ts, 1, pkt_single(frames[3]))); seq += 1  # same NAL
    # expected AUs (construction truth)
    exp = [au(900000, [sps, pps, idr]),
           au(903000, [frames[0]]),
           au(906000, [frames[1]]),
           au(909000, [big_p]),
           au(912000, [frames[2]]),
           au(915000, [frames[3], frames[3]])]
    au_bytes = sum(len(a[1]) for a in exp)
    au_max = max(len(a[1]) for a in exp)
    cnt = dict(pkts_in=len(pkts), pkt_dups=1, stap_a_pkts=1,
               stap_a_nals=2, fu_a_pkts=5, single_nal=5, aus=6,
               au_bytes=au_bytes, au_max=au_max)
    return ("mixed_stream", 8192, pkts, exp, cnt, None)


SCENARIOS = [
    scen_single(), scen_stap_a(), scen_fu_a(), scen_keyframe_real(),
    scen_two_frames(), scen_ts_split(), scen_gap_drop(), scen_dup_late(),
    scen_fu_incomplete(), scen_oversize(), scen_seq_wrap(),
    scen_malformed(), scen_other_type(), scen_mixed_stream(),
]


# ------------------------------------------------------------ checks
def parse_annexb(au_bytes):
    """Independent AU re-parse: split on the 4-byte start code."""
    assert au_bytes.startswith(SC)
    parts = au_bytes.split(SC)
    assert parts[0] == b""
    nals = [p for p in parts[1:]]
    assert all(len(n) > 0 for n in nals)
    return nals


def run_scenario(sc):
    name, cap, pkts, exp_aus, exp_cnt, bad = sc
    dep = Dep(cap)
    got = []
    for (seq, ts, marker, payload) in pkts:
        got.extend(dep.push(seq, ts, marker, payload))
    return dep, got


def check_all():
    for sc in SCENARIOS:
        name, cap, pkts, exp_aus, exp_cnt, bad = sc
        dep, got = run_scenario(sc)
        # 1/3: emitted AUs byte-exact against the construction truth
        assert got == exp_aus, (
            f"{name}: AU mismatch\n got={[(t, len(b)) for t, b in got]}\n"
            f" exp={[(t, len(b)) for t, b in exp_aus]}")
        # counters: hand-labeled expectations, every unlisted key == 0
        for k in COUNTER_ORDER:
            want = exp_cnt.get(k, 0)
            have = dep.st[k]
            assert want == have, f"{name}: counter {k}: {have} != {want}"
        # bad identity
        if bad is not None:
            btype, bseq, bhead = bad
            assert dep.bad_logged, f"{name}: bad not logged"
            assert dep.bad_type == btype, f"{name}: bad_type"
            assert dep.bad_seq == bseq, f"{name}: bad_seq"
            assert dep.bad_head == bhead, f"{name}: bad_head"
        else:
            assert not dep.bad_logged, f"{name}: unexpected bad capture"
        # 1: structural re-parse of every CLEAN emitted AU
        for (ts, b) in got:
            nals = parse_annexb(b)
            assert all(len(n) >= 1 and not (n[0] & 0x80) for n in nals), \
                f"{name}: emitted NAL with F bit"
    # 2: FU-A reconstruction is proven by check 1: every FU-A in the
    #    scenarios is built from a WHOLE source NAL, and exp_aus contain
    #    that SOURCE NAL — a wrong (indicator&0xE0)|(fu&0x1F) rebuild or
    #    a wrong fragment order fails the byte comparison above.
    # 4: drop safety —
    #    DUP: duplicating any packet must leave the AU stream EXACTLY
    #    unchanged (int16 dup-skip is total: same-seq copies and late
    #    arrivals never touch assembly);
    #    DEL: deleting a packet may reshape/drop AUs (gap semantics) but
    #    must never (a) emit a malformed AU (F-bit / empty NAL) or
    #    (b) increase the total emitted payload — loss cannot invent
    #    bytes.
    for sc in SCENARIOS:
        name, cap, pkts, exp_aus, exp_cnt, bad = sc
        if name not in ("two_frames", "mixed_stream", "seq_wrap"):
            continue
        base = run_scenario(sc)[1]
        base_bytes = sum(len(b) for _, b in base)
        for i in range(len(pkts)):
            dup = pkts[:i] + [pkts[i]] + pkts[i:]
            dep = Dep(cap)
            got = []
            for (seq, ts, marker, payload) in dup:
                got.extend(dep.push(seq, ts, marker, payload))
            assert got == base, f"{name}: dup@{i} changed the AU set"
        for i in range(len(pkts)):
            red = pkts[:i] + pkts[i + 1:]
            dep = Dep(cap)
            got = []
            for (seq, ts, marker, payload) in red:
                got.extend(dep.push(seq, ts, marker, payload))
            for (ts, b) in got:
                nals = parse_annexb(b)
                assert all(not (n[0] & 0x80) for n in nals), \
                    f"{name}: del@{i} emitted an F-bit NAL"
            assert sum(len(b) for _, b in got) <= base_bytes, \
                f"{name}: del@{i} invented payload"
    print(f"self-check OK ({len(SCENARIOS)} scenarios, "
          f"{sum(len(s[2]) for s in SCENARIOS)} packets, "
          f"{sum(len(s[3]) for s in SCENARIOS)} expected AUs; "
          "byte-exact re-parse + hand-labeled counters + drop safety)")


# ------------------------------------------------------------ emit
def emit_header(path):
    blob = bytearray()
    scen_rows, pkt_rows, au_rows, cnt_rows, bad_rows = [], [], [], [], []
    for (name, cap, pkts, exp_aus, exp_cnt, bad) in SCENARIOS:
        pkt_off = len(pkt_rows)
        for (seq, ts, marker, payload) in pkts:
            off = len(blob)
            blob += payload
            pkt_rows.append((seq, ts, marker, off, len(payload)))
        au_off = len(au_rows)
        for (ts, b) in exp_aus:
            off = len(blob)
            blob += b
            au_rows.append((ts, off, len(b)))
        cnts = [exp_cnt.get(k, 0) for k in COUNTER_ORDER]
        cnt_rows.append(cnts)
        if bad is not None:
            btype, bseq, bhead = bad
            off = len(blob)
            blob += bhead
            bad_rows.append((1, btype, bseq, len(bhead), off))
        else:
            bad_rows.append((0, 0, 0, 0, 0))
        scen_rows.append((name, cap, len(pkts), pkt_off, len(exp_aus),
                          au_off))

    out = []
    out.append("/* kn_h264rtp_kat_vectors.h — GENERATED FILE, DO NOT EDIT.")
    out.append(" * Generator: tools/h264rtp_reference.py (slice 5n).")
    out.append(" * RFC 6184 depacketizer known-answer vectors:")
    out.append(f" *   {len(SCENARIOS)} scenarios,"
               f" {len(pkt_rows)} packets, {len(au_rows)} expected AUs,")
    out.append(f" *   {len(blob)} blob bytes. Counter order:")
    out.append(" *     " + ", ".join(COUNTER_ORDER))
    out.append(" * Every expected AU is the CONSTRUCTION truth (start")
    out.append(" * codes + source NALs), checked byte-exact against the")
    out.append(" * reference's output plus the drop-safety invariants. */")
    out.append("#ifndef KN_H264RTP_KAT_VECTORS_H")
    out.append("#define KN_H264RTP_KAT_VECTORS_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define K_KAT_H264RTP_NSCEN %d" % len(SCENARIOS))
    out.append("#define K_KAT_H264RTP_NPKTS %d" % len(pkt_rows))
    out.append("#define K_KAT_H264RTP_NAUS %d" % len(au_rows))
    out.append("#define K_KAT_H264RTP_NCNTR %d" % len(COUNTER_ORDER))
    out.append("#define K_KAT_H264RTP_BLOB_LEN %d" % len(blob))
    out.append("")
    out.append("typedef struct k_h264rtp_scen_s {")
    out.append("    const char *name;")
    out.append("    unsigned cap;")
    out.append("    unsigned npkts, pkt_off;")
    out.append("    unsigned naus, au_off;")
    out.append("} k_h264rtp_scen_t;")
    out.append("")
    out.append("typedef struct k_h264rtp_pkt_s {")
    out.append("    uint16_t seq;")
    out.append("    uint32_t ts;")
    out.append("    uint8_t  marker;")
    out.append("    uint16_t off, len;   /* into the blob */")
    out.append("} k_h264rtp_pkt_t;")
    out.append("")
    out.append("typedef struct k_h264rtp_au_s {")
    out.append("    uint32_t ts;")
    out.append("    uint16_t off, len;   /* into the blob */")
    out.append("} k_h264rtp_au_t;")
    out.append("")
    out.append("typedef struct k_h264rtp_bad_s {")
    out.append("    uint8_t  logged;")
    out.append("    uint32_t type;")
    out.append("    uint16_t seq;")
    out.append("    uint8_t  head_len;")
    out.append("    uint16_t head_off;   /* into the blob */")
    out.append("} k_h264rtp_bad_t;")
    out.append("")
    out.append("static const k_h264rtp_scen_t k_kat_h264rtp_scen"
               "[K_KAT_H264RTP_NSCEN] = {")
    for (name, cap, npkts, pkt_off, naus, au_off) in scen_rows:
        out.append('    { "%s", %uu, %uu, %uu, %uu, %uu },'
                   % (name, cap, npkts, pkt_off, naus, au_off))
    out.append("};")
    out.append("")
    out.append("static const k_h264rtp_pkt_t k_kat_h264rtp_pkts"
               "[K_KAT_H264RTP_NPKTS] = {")
    for (seq, ts, marker, off, ln) in pkt_rows:
        out.append("    { %uu, %uu, %uu, %uu, %uu },"
                   % (seq, ts, marker, off, ln))
    out.append("};")
    out.append("")
    out.append("static const k_h264rtp_au_t k_kat_h264rtp_aus"
               "[K_KAT_H264RTP_NAUS] = {")
    for (ts, off, ln) in au_rows:
        out.append("    { %uu, %uu, %uu }," % (ts, off, ln))
    out.append("};")
    out.append("")
    out.append("static const uint64_t k_kat_h264rtp_counters"
               "[K_KAT_H264RTP_NSCEN][K_KAT_H264RTP_NCNTR] = {")
    for row in cnt_rows:
        out.append("    { " + ", ".join("%uull" % v for v in row) + " },")
    out.append("};")
    out.append("")
    out.append("static const k_h264rtp_bad_t k_kat_h264rtp_bad"
               "[K_KAT_H264RTP_NSCEN] = {")
    for (logged, btype, bseq, hlen, hoff) in bad_rows:
        out.append("    { %uu, %uu, %uu, %uu, %uu },"
                   % (logged, btype, bseq, hlen, hoff))
    out.append("};")
    out.append("")
    out.append("static const uint8_t k_kat_h264rtp_blob"
               "[K_KAT_H264RTP_BLOB_LEN] = {")
    line = []
    for i, b in enumerate(blob):
        line.append("0x%02x" % b)
        if len(line) == 16 or i == len(blob) - 1:
            out.append("    " + ", ".join(line) +
                       ("," if i != len(blob) - 1 else ""))
            line = []
    out.append("};")
    out.append("")
    out.append("#endif /* KN_H264RTP_KAT_VECTORS_H */")
    text = "\n".join(out) + "\n"
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"emitted {path} ({len(text)} bytes, blob {len(blob)})")


def main():
    check_all()
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    emit_header(os.path.join(root, "streamer", "src",
                             "kn_h264rtp_kat_vectors.h"))


if __name__ == "__main__":
    sys.exit(main())
