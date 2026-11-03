#!/usr/bin/env python3
"""twcc_reference.py — independent Python reference for kn_twcc (slice 5l)
and generator of the C known-answer vectors.

Semantics ground truth (line refs into the str0m clone):
  src/rtp/rtcp/twcc.rs
    update_seq    :285-310   (sorted insert, exact-duplicate ignore,
                              time_start-8192ms drop, report_from pull-back)
    build_report  :312-476   (reference_time = (first_time-time_start)/64ms,
                              base_time quantization, budget loop,
                              feedback_count = generated_reports PRE-increment,
                              trim to keep_reported)
    build_interims:508-570   (delta vs PREVIOUS REPORTED time, 250us units,
                              small 0..63750us / large-or-negative i16,
                              representability break [-8192000, +8191750]us,
                              missing runs <= 8192 per chunk)
Wire layout: draft-holmer-rmcat-transport-wide-cc-extensions-01 §3.1
  (PT 205 FMT 15; 2-byte chunks; 1B small / 2B large deltas; RFC 3550
  padding with the P bit and the pad count in the last byte).

The kn_twcc DEVIATIONS are replicated here deliberately (they are the
contract under test, documented in streamer/include/kn/kn_twcc.h):
  * RUN-LENGTH chunks only — a status-vector chunk is never emitted
    (the draft lets senders choose; receivers must accept all types);
  * fixed 1024-receipt ring instead of an unbounded deque (evict-oldest,
    counters expose it);
  * conservative per-receipt byte budget (truncating one receipt early is
    invisible — it rides the next report);
  * arrivals below the dropped-history FLOOR are dropped + counted; the
    floor exists only where reported receipts were trimmed or evicted
    (str0m's unbounded deque never forgets, so it has no such rule).

Self-checks before emitting (a failed check emits NOTHING):
  1. wire re-parse of every generated report by an INDEPENDENT decoder
     (draft §3.1 walk: chunk types, status_count consumption, delta
     widths, P-bit/pad-count/length-field consistency);
  2. time reconstruction: summing the decoded deltas from base_time must
     land within 250us of every reported receipt's true arrival (the
     quantization must not compound — str0m's reported-time chaining);
  3. seq reconstruction: the decoded status sequence must mark exactly
     the consumed receipts' extended seqs (base_seq + status positions);
  4. register invariants: entries stay sorted + unique, report_from
     monotone except pull-back, trim keeps exactly KEEP_REPORTED.

Usage:  python3 tools/twcc_reference.py
Emits:  streamer/src/kn_twcc_kat_vectors.h
"""
import os
import sys
from bisect import bisect_left

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "streamer", "src", "kn_twcc_kat_vectors.h")

CAPACITY      = 1024        # kn_twcc.h KN_TWCC_CAPACITY
KEEP_REPORTED = 100         # str0m TwccRecvRegister::new(100)
MAX_CHUNKS    = 512
MAX_DELTAS    = 512
REF_STEP_US   = 64_000
DELTA_UNIT_US = 250
SMALL_MAX_US  = 63_750
LARGE_MIN_US  = -8_192_000
LARGE_MAX_US  = 8_191_750
MAX_RUN       = 8192

ST_NOT_RECEIVED = 0
ST_SMALL        = 1
ST_LARGE        = 2

SENDER_SSRC = 0x4F4E4F57    # the probe's SESS_RTCP_SENDER_SSRC ("ONOW")
MEDIA_SSRC  = 0x00000002    # GFN video ssrc (the first remote media ssrc)


def trunc_div(a, b):
    """C/Rust integer division: truncation toward zero (NOT python floor)."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


class Twcc:
    """Line-by-line port of kn_twcc.c (which ports str0m twcc.rs)."""

    def __init__(self):
        self.entries = []            # [(ext_seq, arrival_us)] sorted, unique
        self.report_from = 0
        self.have_last = False
        self.last_ext = 0
        self.have_time_start = False
        self.time_start_us = 0
        self.have_floor = False
        self.floor_ext = 0
        self.generated_reports = 0
        self.recorded = 0
        self.dup_ignored = 0
        self.late_dropped = 0
        self.old_time_dropped = 0
        self.evicted = 0
        self.reports_truncated = 0

    # ---------------------------------------------------------- record
    def record(self, wire_seq, arrival_us):
        if not self.have_last:
            ext = wire_seq
            self.have_last = True
            self.last_ext = ext
        else:
            d = (wire_seq - (self.last_ext & 0xFFFF)) & 0xFFFF
            d = d - 0x10000 if d >= 0x8000 else d          # int16 distance
            ext = self.last_ext + d
            if ext > self.last_ext:
                self.last_ext = ext

        if self.have_time_start and \
                self.time_start_us > arrival_us + 8_192_000:
            self.old_time_dropped += 1
            return

        # dropped-history floor (see kn_twcc.c): before the first trim or
        # eviction there is NO floor and any out-of-order arrival inserts.
        if self.have_floor and ext < self.floor_ext:
            self.late_dropped += 1
            return

        idx = 0
        if self.entries:
            idx = bisect_left(self.entries, (ext, 0))
            if idx < len(self.entries) and self.entries[idx][0] == ext:
                self.dup_ignored += 1
                return

        if len(self.entries) == CAPACITY:
            popped_reported = self.report_from > 0
            self.entries.pop(0)
            self.evicted += 1
            if self.report_from:
                self.report_from -= 1
            if idx:
                idx -= 1
            if popped_reported:
                self.have_floor = True
                self.floor_ext = self.entries[0][0]

        self.entries.insert(idx, (ext, arrival_us))
        if idx < self.report_from:
            self.report_from = idx                         # str0m pull-back
        self.recorded += 1

    def has_unreported(self):
        return len(self.entries) > self.report_from

    # ----------------------------------------------------------- build
    def build(self, sender_ssrc, media_ssrc, out_len):
        """Returns (packet_bytes, consumed_ext_list); b"" when no report."""
        if len(self.entries) <= self.report_from:
            return b"", []
        if out_len < 28:
            return b"", []

        base_seq, first_time = self.entries[self.report_from]
        if not self.have_time_start:
            self.have_time_start = True
            self.time_start_us = first_time
        time_start = self.time_start_us
        # clamp: a pulled-back receipt may predate the epoch (the -8192ms
        # record guard keeps its first delta representable); see kn_twcc.c.
        reference_time = trunc_div(first_time - time_start, REF_STEP_US) \
            & 0xFFFFFF if first_time >= time_start else 0
        base_time = time_start + reference_time * REF_STEP_US

        chunks = []                    # [[status, run]]
        deltas = []                    # [(value, is_large)]
        status_count = 0
        delta_bytes = 0
        consumed = []                  # [(ext_seq, arrival_us)]
        prev_seq, prev_time = base_seq, base_time

        for ri in range(self.report_from, len(self.entries)):
            rseq, rtime = self.entries[ri]
            gap = rseq - prev_seq

            add = 2 * ((gap - 1 + 8191) // 8192) if gap > 1 else 0
            add += 2                                       # worst-case chunk

            diff_time = rtime - prev_time
            if diff_time < LARGE_MIN_US or diff_time > LARGE_MAX_US:
                self.reports_truncated += 1                # str0m break
                break
            if diff_time < 0 or diff_time > SMALL_MAX_US:
                status, big = ST_LARGE, True
                add += 2
            else:
                status, big = ST_SMALL, False
                add += 1
            d250 = trunc_div(diff_time, DELTA_UNIT_US)

            if 20 + 2 * len(chunks) + delta_bytes + add + 3 > out_len:
                self.reports_truncated += 1
                break
            if len(deltas) == MAX_DELTAS:
                self.reports_truncated += 1
                break
            if status_count + (gap - 1 if gap > 1 else 0) + 1 > 65535:
                self.reports_truncated += 1
                break

            if gap > 1:
                if not self._push_run(chunks, ST_NOT_RECEIVED, gap - 1):
                    self.reports_truncated += 1
                    break
                status_count += gap - 1
            if not self._push_run(chunks, status, 1):
                self.reports_truncated += 1
                break
            status_count += 1
            deltas.append((d250, big))
            delta_bytes += 2 if big else 1

            step = abs(d250) * DELTA_UNIT_US
            if diff_time < 0:
                prev_time = prev_time - step if prev_time >= step \
                    else prev_time                         # checked_sub
            else:
                prev_time += step
            prev_seq = rseq
            consumed.append((rseq, rtime))

        if not consumed:
            return b"", []

        total = 20 + 2 * len(chunks) + delta_bytes
        pad = (-total) % 4
        total += pad
        assert total <= out_len, "budget must guarantee the fit"

        out = bytearray(total)
        out[0] = 0x8F | (0x20 if pad else 0)               # V2 P FMT15
        out[1] = 205                                       # RTPFB
        out[2:4] = ((total // 4) - 1).to_bytes(2, "big")
        out[4:8] = sender_ssrc.to_bytes(4, "big")
        out[8:12] = media_ssrc.to_bytes(4, "big")
        out[12:14] = (base_seq & 0xFFFF).to_bytes(2, "big")
        out[14:16] = status_count.to_bytes(2, "big")
        out[16:19] = reference_time.to_bytes(3, "big")
        out[19] = self.generated_reports & 0xFF            # PRE-increment
        o = 20
        for st, run in chunks:
            out[o:o + 2] = (((st << 13) | run) & 0xFFFF).to_bytes(2, "big")
            o += 2
        for val, big in deltas:
            if big:
                out[o:o + 2] = (val & 0xFFFF).to_bytes(2, "big")
                o += 2
            else:
                out[o] = val & 0xFF
                o += 1
        if pad:
            out[total - 1] = pad
        assert o + pad == total

        self.report_from += len(consumed)
        self.generated_reports += 1
        if self.report_from > KEEP_REPORTED:
            remove = min(self.report_from - KEEP_REPORTED, len(self.entries))
            del self.entries[:remove]
            self.report_from -= remove
            if self.entries:
                self.have_floor = True
                self.floor_ext = self.entries[0][0]
        return bytes(out), consumed

    @staticmethod
    def _push_run(chunks, status, run):
        while run:
            part = min(run, MAX_RUN)
            if chunks and chunks[-1][0] == status and \
                    chunks[-1][1] + part <= MAX_RUN:
                chunks[-1][1] += part
            else:
                if len(chunks) == MAX_CHUNKS:
                    return False
                chunks.append([status, part])
            run -= part
        return True

    def counters(self):
        return [self.recorded, self.dup_ignored, self.late_dropped,
                self.old_time_dropped, self.evicted, self.reports_truncated]


# ------------------------------------------------- independent re-parser

def parse_report(pkt, expect_fb=None):
    """Draft §3.1 decoder — walks the wire independently of the encoder.
    Returns dict(statuses, deltas, base_seq, status_count, reference_time,
    fb_count, sender, media, pad)."""
    assert len(pkt) >= 20 and len(pkt) % 4 == 0, "length"
    assert pkt[0] & 0xC0 == 0x80, "version"
    assert pkt[0] & 0x1F == 15, "FMT 15"
    p_bit = (pkt[0] >> 5) & 1
    assert pkt[1] == 205, "PT 205"
    length = int.from_bytes(pkt[2:4], "big")
    assert (length + 1) * 4 == len(pkt), "length field"
    sender = int.from_bytes(pkt[4:8], "big")
    media = int.from_bytes(pkt[8:12], "big")
    base_seq = int.from_bytes(pkt[12:14], "big")
    status_count = int.from_bytes(pkt[14:16], "big")
    ref_time = int.from_bytes(pkt[16:19], "big")
    fb_count = pkt[19]
    if expect_fb is not None:
        assert fb_count == expect_fb, "fb_count"

    o = 20
    statuses = []
    while len(statuses) < status_count:
        assert o + 2 <= len(pkt), "chunk overrun"
        x = int.from_bytes(pkt[o:o + 2], "big")
        o += 2
        t_bit = (x >> 15) & 1
        s = (x >> 13) & 3
        if t_bit == 0:                                     # Run Length
            run = x & 0x1FFF
            assert run > 0, "zero run"
            statuses += [s] * run
        elif s == 0:                                       # VectorSingle
            for i in range(14):
                if len(statuses) >= status_count:
                    break
                statuses.append((x >> (13 - 1 - i)) & 1)
        elif s == 1:                                       # VectorDouble
            for i in range(7):
                if len(statuses) >= status_count:
                    break
                statuses.append((x >> (12 - 2 * i)) & 3)
        else:
            raise AssertionError("reserved chunk S=3")
    assert len(statuses) == status_count, "status_count consumed"

    deltas = []
    for st in statuses:
        if st == ST_SMALL:
            assert o + 1 <= len(pkt), "small delta overrun"
            deltas.append(pkt[o]); o += 1
        elif st == ST_LARGE:
            assert o + 2 <= len(pkt), "large delta overrun"
            v = int.from_bytes(pkt[o:o + 2], "big")
            deltas.append(v - 0x10000 if v >= 0x8000 else v)
            o += 2

    pad = 0
    if p_bit:
        pad = pkt[-1]
        assert 1 <= pad <= 3, "pad count"
    assert o + pad == len(pkt), "trailing bytes: %d + %d != %d" % (o, pad,
                                                                   len(pkt))
    if pad:
        assert all(b == 0 for b in pkt[o:len(pkt) - 1]), "pad bytes nonzero"
    return dict(statuses=statuses, deltas=deltas, base_seq=base_seq,
                status_count=status_count, reference_time=ref_time,
                fb_count=fb_count, sender=sender, media=media, pad=pad)


def verify_report(pkt, reg_time_start, consumed, expect_fb):
    """Semantic cross-check of one emitted report against the register:
    seq coverage, delta decode, time reconstruction within 250us."""
    r = parse_report(pkt, expect_fb)
    assert r["sender"] == SENDER_SSRC and r["media"] == MEDIA_SSRC

    # base_seq = low 16 of the first consumed extended seq
    assert consumed, "empty report"
    assert r["base_seq"] == (consumed[0][0] & 0xFFFF), "base_seq"

    # statuses must mark exactly the consumed seqs
    ext_base = consumed[0][0] - (consumed[0][0] & 0xFFFF) + r["base_seq"]
    # (ext_base == consumed[0][0] by construction of the 16-bit field)
    got_received = []
    for i, st in enumerate(r["statuses"]):
        if st != ST_NOT_RECEIVED:
            got_received.append(ext_base + i)
    assert got_received == [s for s, _ in consumed], "seq coverage"
    assert len(r["deltas"]) == len(consumed), "delta count"

    # time reconstruction: quantization must not compound (< 250us each)
    base_time = reg_time_start + r["reference_time"] * REF_STEP_US
    cur = base_time
    for (_, true_t), d in zip(consumed, r["deltas"]):
        cur += d * DELTA_UNIT_US
        assert abs(cur - true_t) <= DELTA_UNIT_US, \
            "reconstruction off: %d vs %d" % (cur, true_t)
    return r


# -------------------------------------------------------------- scenarios

def scen_steady():
    """S0: 40 receipts, 10ms cadence, build every 10 -> 4 clean reports."""
    ev = []
    for i in range(1, 41):
        ev.append((0, i, 1_000_000 + i * 10_000))
        if i % 10 == 0:
            ev.append((1, 0, 0))
    return ev


def scen_loss():
    """S1: a 4-packet loss burst -> NotReceived run between Received runs."""
    ev = []
    t = 2_000_000
    for s in list(range(1, 11)) + list(range(15, 25)):
        ev.append((0, s, t)); t += 10_000
    ev.append((1, 0, 0))
    for s in range(25, 30):
        ev.append((0, s, t)); t += 10_000
    ev.append((1, 0, 0))
    return ev


def scen_reorder():
    """S2: out-of-order arrivals (all insert — no floor yet), an exact
    duplicate, a pre-epoch pull-back receipt, then a trim phase that
    establishes the floor and a below-floor late arrival."""
    ev = [
        (0, 5, 3_000_000),
        (0, 3, 2_990_000),
        (0, 4, 2_995_000),
        (0, 1, 2_980_000),
        (0, 2, 2_985_000),
        (0, 3, 2_991_000),            # dup -> ignored
        (1, 0, 0),                    # report 1: seq 1-5 (time_start 2980000)
        (0, 0, 2_979_000),            # pre-epoch pull-back -> inserts at 0,
                                      # report_from back to 0, first delta
                                      # large-negative (-1000us), ref clamps 0
        (0, 6, 3_010_000),
        (1, 0, 0),                    # report 2: re-reports 0..6
    ]
    # trim phase: 200 dense receipts, one build -> consumes 200,
    # report_from 207 -> trim to 100 -> floor = ext 107
    t = 3_010_000
    for s in range(7, 207):
        t += 10_000
        ev.append((0, s, t))
    ev.append((1, 0, 0))              # report 3
    ev.append((0, 50, t + 10_000))    # below floor -> late_dropped
    ev.append((0, 207, t + 20_000))   # normal append above the kept range
    ev.append((1, 0, 0))              # report 4: base 207
    return ev


def scen_large():
    """S3: large + negative deltas, reference_time stepping, old-time drop."""
    return [
        (0, 1, 20_000_000),
        (0, 2, 20_500_000),           # +500ms -> large delta 2000
        (0, 3, 20_499_400),           # -600us -> large delta -2 (trunc)
        (0, 4, 20_499_500),           # +100us -> small delta 0
        (1, 0, 0),
        (0, 5, 20_700_000),
        (0, 9, 11_000_000),           # > 8192ms before time_start -> drop
        (1, 0, 0),                    # ref_time = 700000/64000 = 10
        (0, 10, 28_300_000),          # +7.6s -> large delta 30400
        (1, 0, 0),                    # ref_time = 8300000/64000 = 129
    ]


def scen_wrap():
    """S4: 16-bit seq wrap 65534..1 -> extended 65534..65537."""
    return [
        (0, 65534, 5_000_000),
        (0, 65535, 5_010_000),
        (0, 0,     5_020_000),
        (0, 1,     5_030_000),
        (1, 0, 0),
    ]


def scen_evict():
    """S5: 1100 receipts with no builds -> ring evicts 76, then two
    MAX_DELTAS-limited reports (512 + 412) and an empty third build."""
    ev = [(0, i & 0xFFFF, 1_000_000 + i * 1_000) for i in range(1100)]
    ev += [(1, 0, 0), (1, 0, 0), (1, 0, 0)]
    return ev


def scen_budget():
    """S6: 200 dense receipts, build with out_len 64 -> budget truncation
    at 37 receipts (reports_truncated=1), then a full 1200 build."""
    ev = [(0, i, 6_000_000 + (i - 1) * 1_000) for i in range(1, 201)]
    ev += [(2, 0, 0), (1, 0, 0)]
    return ev


def scen_rereport():
    """S7: build with a hole, then a late fill pulls report_from back and
    the receipt rides a re-report (str0m keep_reported semantics)."""
    ev = []
    t = 7_000_000
    for s in list(range(1, 6)) + list(range(7, 11)):
        t += 10_000
        ev.append((0, s, t))
    ev.append((1, 0, 0))
    ev.append((0, 6, 7_055_000))      # late fill inside the kept window
    ev.append((1, 0, 0))
    return ev


SCENARIOS = [
    ("steady",   scen_steady()),
    ("loss",     scen_loss()),
    ("reorder",  scen_reorder()),
    ("large",    scen_large()),
    ("wrap",     scen_wrap()),
    ("evict",    scen_evict()),
    ("budget",   scen_budget()),
    ("rereport", scen_rereport()),
]

OUT_LENS = {1: 1200, 2: 64}           # cmd -> out_len


def run_scenario(ev):
    reg = Twcc()
    reports = []                       # (bytes, consumed)
    fb_expected = 0
    for cmd, seq, t in ev:
        if cmd == 0:
            reg.record(seq, t)
        else:
            pkt, consumed = reg.build(SENDER_SSRC, MEDIA_SSRC, OUT_LENS[cmd])
            reports.append(pkt)
            if pkt:
                verify_report(pkt, reg.time_start_us, consumed, fb_expected)
                fb_expected += 1
            else:
                assert not consumed
    return reg, reports


def self_check():
    total_reports = 0
    for name, ev in SCENARIOS:
        reg, reports = run_scenario(ev)
        # register invariants after the full script
        seqs = [s for s, _ in reg.entries]
        assert seqs == sorted(set(seqs)), "%s: sorted+unique" % name
        assert 0 <= reg.report_from <= len(reg.entries), name
        assert len(reg.entries) <= CAPACITY, name
        if reg.generated_reports and len(reg.entries) > KEEP_REPORTED:
            assert reg.report_from == KEEP_REPORTED or \
                len(reg.entries) == reg.report_from, "%s: trim" % name
        n_rep = sum(1 for r in reports if r)
        print("  scenario %-9s events=%4d reports=%d (non-empty %d) "
              "counters=%s" % (name, len(ev), len(reports), n_rep,
                               reg.counters()))
        total_reports += len(reports)
    print("self-check OK (%d scenarios, %d build calls, every report "
          "re-parsed + time/seq reconstructed)" % (len(SCENARIOS),
                                                   total_reports))


# ------------------------------------------------------------------ emit

def c_arr(name, data, per_line=16, ctype="unsigned char"):
    rows = []
    for i in range(0, len(data), per_line):
        rows.append("    " + ", ".join("0x%02x" % b for b in data[i:i + per_line]) + ",")
    return "static const %s %s[%d] = {\n%s\n};" % (ctype, name, len(data),
                                                   "\n".join(rows))


def main():
    self_check()

    ev_seq, ev_time, ev_cmd = [], [], []
    scen_ev_off, scen_ev_len = [], []
    rep_blob = bytearray()
    rep_off, rep_len = [], []
    scen_rep_off = []
    counters = []

    for name, ev in SCENARIOS:
        scen_ev_off.append(len(ev_seq))
        for cmd, seq, t in ev:
            ev_cmd.append(cmd); ev_seq.append(seq); ev_time.append(t)
        scen_ev_len.append(len(ev))

        reg = Twcc()
        scen_rep_off.append(len(rep_off))
        for cmd, seq, t in ev:
            if cmd == 0:
                reg.record(seq, t)
            else:
                pkt, _ = reg.build(SENDER_SSRC, MEDIA_SSRC, OUT_LENS[cmd])
                rep_off.append(len(rep_blob))
                rep_len.append(len(pkt))
                rep_blob += pkt
        counters.append(reg.counters())
    scen_rep_off.append(len(rep_off))

    P = []
    P.append("""/* kn_twcc_kat_vectors.h -- GENERATED FILE, do not edit by hand.
 * Generator: python3 tools/twcc_reference.py   (run from the repo root)
 *
 * Known-answer vectors for kn_twcc (slice 5l). The generator is an
 * independent Python port of the str0m twcc.rs semantics (with the
 * documented kn_twcc deviations: RUN-only chunks, 1024 ring, conservative
 * budget); before emitting, it re-parses every report with a draft-3.1
 * decoder and reconstructs seq coverage + arrival times from the wire.
 * If the C code disagrees with a vector here, ONE of the two ports is
 * wrong -- check against str0m src/rtp/rtcp/twcc.rs, never "fix" blind.
 *
 * Script model, per scenario s:
 *   events  = ev_cmd[scen_ev_off[s] .. +scen_ev_len[s]] with seq/time
 *             cmd 0 = kn_twcc_record(seq, time)
 *             cmd 1 = kn_twcc_build(..., out_len 1200)
 *             cmd 2 = kn_twcc_build(..., out_len 64)
 *   reports = one per build call, in order: expected length
 *             k_kat_twcc_rep_len[i] (0 = build returned 0) and bytes at
 *             k_kat_twcc_rep_blob + k_kat_twcc_rep_off[i]
 *   scenario s owns reports [scen_rep_off[s], scen_rep_off[s+1])
 *   counters[s] = { recorded, dup_ignored, late_dropped, old_time_dropped,
 *                   evicted, reports_truncated } after the whole script.
 * Builds always use sender ssrc 0x4f4e4f57 and media ssrc 0x00000002.
 */
#ifndef KN_TWCC_KAT_VECTORS_H
#define KN_TWCC_KAT_VECTORS_H

#define K_KAT_TWCC_SENDER_SSRC 0x4f4e4f57u
#define K_KAT_TWCC_MEDIA_SSRC  0x00000002u
#define K_KAT_TWCC_NSCEN       %d
#define K_KAT_TWCC_NEV         %d
#define K_KAT_TWCC_NREP        %d""" % (len(SCENARIOS), len(ev_cmd),
                                        len(rep_off)))
    P.append("/* scenario order: %s */" % ", ".join(n for n, _ in SCENARIOS))
    P.append("static const unsigned char k_kat_twcc_ev_cmd[%d] = { %s };"
             % (len(ev_cmd), ", ".join(str(c) for c in ev_cmd)))
    P.append("static const unsigned short k_kat_twcc_ev_seq[%d] = { %s };"
             % (len(ev_seq), ", ".join(str(s) for s in ev_seq)))
    rows = []
    for i in range(0, len(ev_time), 6):
        rows.append("    " + ", ".join("%dull" % t for t in ev_time[i:i + 6]) + ",")
    P.append("static const unsigned long long k_kat_twcc_ev_time[%d] = {\n%s\n};"
             % (len(ev_time), "\n".join(rows)))
    P.append("static const unsigned short k_kat_twcc_scen_ev_off[%d] = { %s };"
             % (len(SCENARIOS), ", ".join(str(x) for x in scen_ev_off)))
    P.append("static const unsigned short k_kat_twcc_scen_ev_len[%d] = { %s };"
             % (len(SCENARIOS), ", ".join(str(x) for x in scen_ev_len)))
    P.append("static const unsigned short k_kat_twcc_scen_rep_off[%d] = { %s };"
             % (len(SCENARIOS) + 1, ", ".join(str(x) for x in scen_rep_off)))
    P.append("static const unsigned short k_kat_twcc_rep_off[%d] = { %s };"
             % (len(rep_off), ", ".join(str(x) for x in rep_off)))
    P.append("static const unsigned short k_kat_twcc_rep_len[%d] = { %s };"
             % (len(rep_len), ", ".join(str(x) for x in rep_len)))
    P.append(c_arr("k_kat_twcc_rep_blob", bytes(rep_blob), 24))
    rows = ",\n".join("    { %s }" % ", ".join("%dull" % v for v in c)
                      for c in counters)
    P.append("static const unsigned long long k_kat_twcc_counters[%d][6] = {\n%s\n};"
             % (len(counters), rows))
    P.append("\n#endif /* KN_TWCC_KAT_VECTORS_H */")

    text = "\n\n".join(P) + "\n"
    text = text.encode("ascii", "replace").decode("ascii")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="ascii") as f:
        f.write(text)
    print("emitted %s (%d bytes, blob %d)" % (OUT, len(text), len(rep_blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
