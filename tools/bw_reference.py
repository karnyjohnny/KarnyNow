#!/usr/bin/env python3
"""bw_reference.py — the INDEPENDENT reference model for the NVST
bandwidth estimator (nvst_bandwidth.rs @ a49b124) and the server-control
decoder (nvst_server_control.rs), round XXXVIII.

WHY THIS FILE EXISTS (the three-implementations rule, same discipline as
tools/input_reference.py): run 18 showed the rig walking its bitrate down
to the floor and then closing the session, and the one QoS field set we
have never filled is the estimator block of the 0x0207 report (D-t10).
The port has to be byte-exact, and "I read the Rust" is not evidence — so
this script re-implements the estimator independently, PROVES ITSELF
against the reference's own five unit tests (nvst_bandwidth.rs:250-416,
whose expected values are hard-coded in the Rust source) and the
server-control tests (nvst_server_control_tests.rs, all five), and only
then emits streamer/src/kn_bw_kat_vectors.h for the C suite to replay.

The scenarios are DETERMINISTIC: `received_at` is a plain millisecond
offset, so Python floats and the C doubles take identical paths and the
emitted expectations are exact integers (the two places where the Rust
asserts a float — `average_receive_kbps == Some(6400.0)` — are checked
with an exact equality that holds because 6400.0 is representable).

Regeneration is byte-identical. Run:  python3 tools/bw_reference.py
"""

from __future__ import annotations

import os
import struct
import sys

TRANSIT_SAMPLES = 16
MIN_MEASURABLE_TRAIN_MS = 1.0

# The estimator's clock is INTEGER NANOSECONDS (the reference passes an
# Instant and converts with Duration::as_secs_f64, which is exact). These
# two constants keep the scenario scripts readable.
NS_MS = 1000000
NS_US = 1000

U32 = 0xFFFFFFFF
U16 = 0xFFFF
I64_MIN = -(2**63)
I64_MAX = 2**63 - 1


# ------------------------------------------------------- Rust `as` casting
def sat_i64(x: float) -> int:
    if x != x:  # NaN
        return 0
    if x <= float(I64_MIN):
        return I64_MIN
    if x >= float(I64_MAX):
        return I64_MAX
    return int(x)  # truncates toward zero


def as_u32(x: float) -> int:
    return sat_i64(x) & U32


def as_u16(x: float) -> int:
    return sat_i64(x) & U16


def as_u8(x: float) -> int:
    return sat_i64(x) & 0xFF


def dur_ms(ns: int) -> float:
    """`Duration::as_secs_f64() * 1000.0` on a saturating nanosecond delta.
    as_secs_f64 is `secs as f64 + nanos as f64 / 1e9`, so this is exact for
    every clock value a session can produce."""
    if ns < 0:
        ns = 0
    return float(ns // 1000000000) * 1000.0 + float(ns % 1000000000) / 1e6


def smooth(previous, value: float) -> float:
    return value if previous is None else previous * 0.875 + value * 0.125


# ======================================================== the estimator
class BandwidthEstimator:
    """nvst_bandwidth.rs:41-243, line for line."""

    def __init__(self) -> None:
        self.first_arrival = None
        self.last_timestamp = None          # (raw, extended)
        self.minimum_transit_ms = None
        self.previous_transit_ms = None
        self.jitter_ms = 0.0
        self.transits = [0.0] * TRANSIT_SAMPLES
        self.transit_count = 0
        self.transit_next = 0
        self.latest_transit_ms = 0.0
        self.active_frame = None            # dict or None
        self.frames_observed = 0
        self.lossy_frames = 0
        self.average_frame_ms = None
        self.average_receive_kbps = None
        self.average_utilization_percent = None

    def observe_parity(self, frame_number: int, sequence: int) -> None:
        frame = self.active_frame
        if frame is not None and frame["number"] == frame_number and sequence > frame["last_sequence"]:
            if sequence != frame["last_sequence"] + 1:
                frame["contiguous"] = False
            frame["last_sequence"] = sequence

    def observe(self, frame_number: int, sequence: int, timestamp: int,
                nbytes: int, started: bool, ended: bool, at: int) -> None:
        """`at` is a monotonic clock in INTEGER NANOSECONDS.

        This is not a stylistic choice: the reference does all of its clock
        arithmetic on `Duration` (exact integer nanoseconds) and converts to
        f64 only at `.as_secs_f64()`. Modelling the clock as a float makes
        `received_ms` 0.999999999999455 instead of exactly 1.0, and the
        reference's own test expects `utilization_percent == 5` — a float
        clock yields 4 after the `as u8` truncation. Exactness matters."""
        if self.first_arrival is None:
            self.first_arrival = at
        if self.last_timestamp is not None:
            last, extended = self.last_timestamp
            delta = (timestamp - last) & U32
            delta = delta - (1 << 32) if delta >= (1 << 31) else delta
            if delta < 0:
                return
            extended = extended + delta
        else:
            extended = timestamp
        self.last_timestamp = (timestamp, extended)

        transit_ms = dur_ms(at - self.first_arrival) - extended / 90.0
        if self.minimum_transit_ms is None:
            self.minimum_transit_ms = transit_ms
        self.minimum_transit_ms = min(self.minimum_transit_ms, transit_ms)
        self.latest_transit_ms = transit_ms
        if self.previous_transit_ms is not None:
            self.jitter_ms += (abs(transit_ms - self.previous_transit_ms) - self.jitter_ms) / 16.0
        self.previous_transit_ms = transit_ms
        self.transits[self.transit_next] = transit_ms
        self.transit_next = (self.transit_next + 1) % TRANSIT_SAMPLES
        self.transit_count = min(self.transit_count + 1, TRANSIT_SAMPLES)

        frame = self.active_frame
        if frame is not None and frame["number"] == frame_number:
            if sequence <= frame["last_sequence"]:
                frame["contiguous"] = False
                return
            if sequence != frame["last_sequence"] + 1:
                frame["contiguous"] = False
            sender_gap_ms = max(0, extended - frame["last_timestamp"]) / 90.0
            receive_gap_ms = dur_ms(at - frame["last_at"])
            if sender_gap_ms >= 1.0:
                frame["sender_gap_ms"] += min(sender_gap_ms, receive_gap_ms)
            frame["last_at"] = at
            frame["last_timestamp"] = extended
            frame["last_sequence"] = sequence
            frame["bytes"] += nbytes
            frame["ended"] = frame["ended"] or ended
            return

        if frame is not None:
            distance = (frame_number - frame["number"]) & U32
            if distance == 0 or distance >= 0x80000000:
                return
            self.lossy_frames = min(U32, self.lossy_frames + max(0, distance - 1))
            if frame["started"] and frame["ended"] and frame["contiguous"]:
                frame_ms = max(0, extended - frame["first_timestamp"]) / 90.0
                received_ms = max(dur_ms(frame["last_at"] - frame["first_at"])
                                  - frame["sender_gap_ms"], 0.5)
                if 0.0 < frame_ms < 1000.0:
                    self.average_frame_ms = smooth(self.average_frame_ms, frame_ms)
                    receive_kbps = frame["bytes"] * 8.0 / received_ms
                    if (received_ms >= MIN_MEASURABLE_TRAIN_MS
                            or self.average_receive_kbps is None
                            or receive_kbps > self.average_receive_kbps):
                        self.average_receive_kbps = smooth(self.average_receive_kbps, receive_kbps)
                    self.average_utilization_percent = smooth(
                        self.average_utilization_percent,
                        min(100.0 * received_ms / self.average_frame_ms, 100.0))
                    self.frames_observed = min(U32, self.frames_observed + 1)
                    if self.frames_observed % 60 == 0 and self.minimum_transit_ms is not None:
                        self.minimum_transit_ms += 0.5
            else:
                self.lossy_frames = min(U32, self.lossy_frames + 1)

        self.active_frame = {
            "number": frame_number, "first_at": at, "last_at": at,
            "first_timestamp": extended, "last_timestamp": extended,
            "last_sequence": sequence, "bytes": nbytes, "sender_gap_ms": 0.0,
            "started": started, "ended": ended, "contiguous": True,
        }

    def report(self, at: int) -> dict:
        if self.first_arrival is None or self.minimum_transit_ms is None:
            return {"minimum_server_time": 0, "median_server_time": 0,
                    "queue_delay_us": 0, "jitter_us": 0,
                    "utilization_percent": 0, "estimate_kbps": 0,
                    "lossy_frames": 0}
        samples = sorted(self.transits[:self.transit_count])
        median = samples[(self.transit_count - 1) // 2]
        now_ms = dur_ms(at - self.first_arrival)

        def server_time(transit: float) -> int:
            return int(max(0.0, now_ms - transit) * 90.0) & 0x7FFFFFFF

        return {
            "minimum_server_time": server_time(self.minimum_transit_ms),
            "median_server_time": server_time(median),
            "queue_delay_us": as_u32(max(0.0, self.latest_transit_ms - self.minimum_transit_ms) * 1000.0),
            "jitter_us": as_u16(min(self.jitter_ms * 1000.0, 65535.0)),
            "utilization_percent": (as_u8(self.average_utilization_percent or 0.0)
                                    if self.frames_observed >= 100 else 0),
            "estimate_kbps": (as_u32(self.average_receive_kbps or 0.0)
                              if self.frames_observed >= 100 else 0),
            "lossy_frames": self.lossy_frames,
        }


# ============================================== server control (0x1xx plane)
MAX_SERVER_CONTROL_BYTES = 4 + 0xFFFF
MAX_SERVER_CONTROL_COMMANDS = 256

UNHANDLED = set([0x101, 0x102, 0x103, 0x106, 0x10A, 0x10B, 0x10C, 0x10D,
                 0x10E, 0x10F, 0x110, 0x111, 0x113, 0x206, 0x325, 0x408])


def session_resumable(reason, extended_code):
    if reason is None:
        return None
    if reason == 0x37:
        if extended_code is None:
            return None
        return not (extended_code == 0x80040003
                    or 0x8004000B <= extended_code <= 0x8004000F
                    or 0x80040011 <= extended_code <= 0x80040016)
    if reason == 0x3B:
        if extended_code is None:
            return None
        return extended_code in (0x80040004, 0x80040007, 0x80040010)
    return False


def termination_from_wire(wire_code: int):
    if wire_code == 0x80030023:
        return 0x38, 0x80030007
    if wire_code == 0x80040010:
        return 0x37, 0x80040010
    return None, None


def server_control_messages(data: bytes):
    """The reference iterator (nvst_server_control.rs:107-146), exactly.

    Three distinct error paths, and they do NOT behave alike:
      * Oversized is decided at CONSTRUCTION (`error: (len > MAX).then_some(..)`)
        and is the only user of the latched `self.error` field — it is yielded
        first, `bytes` is emptied, and the next call returns None.
      * A BOUNDS error (remaining == 0 -> TooManyCommands, len < 4 ->
        Truncated, payload past the end -> Truncated) empties `bytes` and
        returns Some(Err) WITHOUT setting `self.error`, so it is yielded once
        and the walk ends there.
      * A DECODE error (a 0x104/0x105/0x109 payload < 4 bytes, or a 0x112
        payload != 8 bytes) is just `Some(decode_control_message(..))` — the
        record is consumed and THE WALK CONTINUES with the remaining bytes.
        Nothing is discarded. (This is why the reference's test buffer
        [0x999 ok][0x104 truncated][0x112 ok] counts malformed == 1 and
        never decodes the third record: the buffer simply ran out.)
    """
    out = []
    if len(data) > MAX_SERVER_CONTROL_BYTES:
        return [{"kind": "error", "error": "oversized"}]
    remaining = MAX_SERVER_CONTROL_COMMANDS
    off = 0
    while off < len(data):
        if remaining == 0:
            out.append({"kind": "error", "error": "too_many"})
            break
        if len(data) - off < 4:
            out.append({"kind": "error", "error": "truncated"})
            break
        code = struct.unpack_from("<H", data, off)[0]
        length = struct.unpack_from("<H", data, off + 2)[0]
        if length > len(data) - off - 4:
            out.append({"kind": "error", "error": "truncated"})
            break
        payload = data[off + 4: off + 4 + length]
        off += 4 + length
        remaining -= 1
        out.append(decode_control_message(code, payload))
    return out


def decode_control_message(code: int, payload: bytes) -> dict:
    def word(offset: int):
        if len(payload) < offset + 4:
            return None
        return struct.unpack_from("<I", payload, offset)[0]

    if code in (0x104, 0x105):
        seconds = word(0)
        if seconds is None:
            return {"kind": "error", "error": "truncated", "code": code}
        return {"kind": "timer", "code": code,
                "timer_type": 4 if code == 0x104 else 5,
                "seconds_left": seconds, "active": code == 0x104}
    if code == 0x109:
        if len(payload) < 4:
            return {"kind": "error", "error": "truncated", "code": code}
        wire = struct.unpack_from(">I", payload, 0)[0]
        reason, extended = termination_from_wire(wire)
        return {"kind": "termination", "code": code, "wire_code": wire,
                "reason": reason, "extended_code": extended,
                "resumable": session_resumable(reason, extended)}
    if code == 0x112:
        if len(payload) != 8:
            return {"kind": "error", "error": "invalid_length", "code": code}
        return {"kind": "pacing_mode", "code": code,
                "stream_index": word(0), "interval": word(4) == 1}
    if code in UNHANDLED:
        return {"kind": "unhandled", "code": code}
    return {"kind": "unknown", "code": code}


def server_command(code: int, payload: bytes = b"") -> bytes:
    return struct.pack("<HH", code, len(payload)) + payload


# ================================================================ self-proof
FAILURES = []


def expect(cond: bool, label: str) -> None:
    if not cond:
        FAILURES.append(label)
        print(f"  SELF-PROOF FAIL: {label}")


def test_packet_train_reports_measured_capacity_after_one_hundred_frames():
    """nvst_bandwidth.rs:269-289 — the exact asserted values."""
    est = BandwidthEstimator()
    for frame in range(0, 101):
        at = frame * 20 * NS_MS                      # 20 ms per frame
        timestamp = 90000 + frame * 1800
        for part in range(3):
            est.observe(frame, frame * 3 + part, timestamp, 200,
                        part == 0, part == 2, at + part * 500 * NS_US)
    r = est.report(2001 * NS_MS)
    expect(r["minimum_server_time"] == 270090,
           f"minimum_server_time == 270090 (got {r['minimum_server_time']})")
    expect(r["estimate_kbps"] == 4800, f"estimate_kbps == 4800 (got {r['estimate_kbps']})")
    expect(r["utilization_percent"] == 5,
           f"utilization_percent == 5 (got {r['utilization_percent']})")
    expect(r["queue_delay_us"] == 1000,
           f"queue_delay_us == 1000 (got {r['queue_delay_us']})")
    expect(r["jitter_us"] > 0, f"jitter_us > 0 (got {r['jitter_us']})")
    expect(r["lossy_frames"] == 0, f"lossy_frames == 0 (got {r['lossy_frames']})")
    expect(r["minimum_server_time"] != r["median_server_time"],
           "minimum_server_time != median_server_time")
    return r


def test_unresolvable_single_packet_frames_do_not_lower_the_estimate():
    """nvst_bandwidth.rs:291-315."""
    est = BandwidthEstimator()
    for frame in range(0, 101):
        at = frame * 20 * NS_MS
        timestamp = 90000 + frame * 1800
        for part in range(3):
            est.observe(frame, frame * 3 + part, timestamp, 200,
                        part == 0, part == 2, at + part * 500 * NS_US)
    measured = est.average_receive_kbps
    for frame in range(101, 201):
        est.observe(frame, 202 + frame, 90000 + frame * 1800, 200,
                    True, True, frame * 20 * NS_MS)
    expect(est.average_receive_kbps == measured,
           "a sub-millisecond train never lowers the estimate")
    expect(est.frames_observed == 200,
           f"frames_observed == 200 (got {est.frames_observed})")


def test_incomplete_and_reordered_frames_do_not_inflate_the_estimate():
    """nvst_bandwidth.rs:317-346."""
    est = BandwidthEstimator()
    est.observe(1, 100, 90000, 1200, True, False, 0)
    est.observe(1, 102, 90000, 1200, False, True, 1 * NS_MS)
    est.observe(3, 103, 93600, 1200, True, True, 40 * NS_MS)
    est.observe(2, 101, 91800, 1200, True, True, 41 * NS_MS)
    expect(est.report(50 * NS_MS)["lossy_frames"] == 2,
           f"lossy_frames == 2 (got {est.report(50 * NS_MS)['lossy_frames']})")
    expect(est.frames_observed == 0, "frames_observed == 0")
    expect(est.average_receive_kbps is None, "no estimate from a broken train")


def test_sender_timestamp_wrap_preserves_estimated_server_clock():
    """nvst_bandwidth.rs:348-360."""
    est = BandwidthEstimator()
    est.observe(1, 10, U32 - 899, 300, True, True, 0)
    est.observe(2, 11, 901, 300, True, True, 20 * NS_MS)
    r = est.report(20 * NS_MS)
    expect(abs(r["minimum_server_time"] - 900) <= 1,
           f"minimum_server_time ~= 900 (got {r['minimum_server_time']})")
    expect(est.frames_observed == 1, "frames_observed == 1")


def test_parity_between_video_shards_does_not_invalidate_a_frame():
    """nvst_bandwidth.rs:362-379 — asserts average_receive_kbps == 6400.0."""
    est = BandwidthEstimator()
    est.observe(1, 10, 90000, 400, True, False, 0)
    est.observe_parity(1, 11)
    est.observe(1, 12, 90000, 400, False, True, 1 * NS_MS)
    est.observe(2, 13, 91800, 400, True, True, 20 * NS_MS)
    expect(est.frames_observed == 1, "frames_observed == 1")
    expect(est.lossy_frames == 0, "lossy_frames == 0")
    expect(est.average_receive_kbps == 6400.0,
           f"average_receive_kbps == 6400.0 (got {est.average_receive_kbps})")


def test_server_control_idle_warning_and_expiry():
    """nvst_server_control_tests.rs:10-46."""
    msgs = server_control_messages(server_command(0x104, struct.pack("<I", 60)))
    expect(msgs == [{"kind": "timer", "code": 0x104, "timer_type": 4,
                     "seconds_left": 60, "active": True}],
           f"0x104 -> active timer 60 s (got {msgs})")
    msgs = server_control_messages(server_command(0x105, struct.pack("<I", 0)))
    expect(msgs[0]["active"] is False and msgs[0]["timer_type"] == 5,
           f"0x105 -> expired timer (got {msgs})")


def test_server_control_zero_second_warning_stays_active():
    """nvst_server_control_tests.rs:48-60."""
    msgs = server_control_messages(server_command(0x104, struct.pack("<I", 0)))
    expect(msgs[0]["active"] is True and msgs[0]["seconds_left"] == 0,
           "a 0-second warning is still ACTIVE")


def test_server_control_termination():
    """nvst_server_control_tests.rs:62-85."""
    for wire, resumable in ((0x80030023, False), (0x80040010, True),
                            (0xDEADBEEF, None)):
        msgs = server_control_messages(server_command(0x109, struct.pack(">I", wire)))
        expect(msgs[0]["kind"] == "termination", f"0x109 {wire:#x} is a termination")
        expect(msgs[0]["wire_code"] == wire, f"wire code preserved for {wire:#x}")
        expect(msgs[0]["resumable"] == resumable,
               f"resumable({wire:#x}) == {resumable} (got {msgs[0]['resumable']})")


def test_server_control_pacing_mode_only_for_stream_zero():
    """nvst_server_control_tests.rs:87-108."""
    for stream, mode, enabled in ((0, 0, False), (0, 1, True), (1, 0, True),
                                  (0, 99, False)):
        msgs = server_control_messages(
            server_command(0x112, struct.pack("<II", stream, mode)))
        expect(msgs[0]["kind"] == "pacing_mode", "0x112 parses")
        expect(msgs[0]["stream_index"] == stream, "stream index preserved")
        expect(msgs[0]["interval"] == (mode == 1),
               f"mode {mode} -> interval {mode == 1}")


def test_server_control_unknown_and_malformed_are_counted_not_terminal():
    """nvst_server_control_tests.rs:110-132 — the exact concatenated buffer."""
    buf = (server_command(0x999, bytes([1, 2, 3]))
           + server_command(0x104, bytes([0]))
           + server_command(0x112, bytes(8)))
    msgs = server_control_messages(buf)
    # The reference's own assertion is on the DIAGNOSTICS (unknown 1,
    # malformed 1, pacing disabled), not on the item list — because a DECODE
    # error consumes only its own record and the walk continues, so the
    # trailing all-zero 0x112 is still decoded (mode 0 -> pacing off, which
    # is what the test asserts with `!frame_pacing_enabled`).
    expect([m["kind"] for m in msgs] == ["unknown", "error", "pacing_mode"],
           f"unknown, ONE decode error, then the walk continues "
           f"(got {[m['kind'] for m in msgs]})")
    expect(msgs[0]["code"] == 0x999, "unknown code 0x999")
    expect(msgs[1]["error"] == "truncated", "the 1-byte 0x104 is Truncated")
    expect(msgs[2]["kind"] == "pacing_mode" and msgs[2]["interval"] is False,
           "the trailing 0x112 still decodes to pacing OFF")
    expect(sum(1 for m in msgs if m["kind"] == "error") == 1,
           "exactly one malformed record")
    msgs = server_control_messages(server_command(0x112, bytes(7)))
    expect(msgs[0]["error"] == "invalid_length", "a 7-byte 0x112 is InvalidLength")
    expect(len(server_control_messages(b"\x01\x02")) == 1, "a short header is Truncated")
    expect(server_control_messages(b"") == [], "an empty buffer yields nothing")


def test_unhandled_codes_match_the_reference_list():
    for code in (0x101, 0x106, 0x10A, 0x10E, 0x10F, 0x111, 0x113, 0x206,
                 0x325, 0x408):
        msgs = server_control_messages(server_command(code))
        expect(msgs[0]["kind"] == "unhandled", f"{code:#06x} is Unhandled")


SELF_TESTS = [
    test_packet_train_reports_measured_capacity_after_one_hundred_frames,
    test_unresolvable_single_packet_frames_do_not_lower_the_estimate,
    test_incomplete_and_reordered_frames_do_not_inflate_the_estimate,
    test_sender_timestamp_wrap_preserves_estimated_server_clock,
    test_parity_between_video_shards_does_not_invalidate_a_frame,
    test_server_control_idle_warning_and_expiry,
    test_server_control_zero_second_warning_stays_active,
    test_server_control_termination,
    test_server_control_pacing_mode_only_for_stream_zero,
    test_server_control_unknown_and_malformed_are_counted_not_terminal,
    test_unhandled_codes_match_the_reference_list,
]


def self_prove() -> dict:
    print("== self-proof: the reference's OWN Rust test expectations ==")
    golden = None
    for t in SELF_TESTS:
        before = len(FAILURES)
        r = t()
        if r is not None:
            golden = r
        print(f"  {'ok  ' if len(FAILURES) == before else 'FAIL'} {t.__name__}")
    if FAILURES:
        print(f"\nSELF-PROOF FAILED ({len(FAILURES)} expectation(s)) — "
              "NOT emitting the KAT header.")
        for f in FAILURES:
            print(f"  - {f}")
        raise SystemExit(1)
    print("SELF-PROOF PASS: every nvst_bandwidth.rs / "
          "nvst_server_control_tests.rs expectation reproduced exactly.")
    return golden


# ================================================================== scenarios
# Each scenario is a deterministic packet script the C suite replays through
# kn_bw_observe() and then reports at the given clock. Kept SMALL on purpose:
# the five Rust tests above are the proof, these are the shared vectors.
def scenario_train_101() -> dict:
    est = BandwidthEstimator()
    for frame in range(0, 101):
        at = frame * 20 * NS_MS
        ts = 90000 + frame * 1800
        for part in range(3):
            est.observe(frame, frame * 3 + part, ts, 200, part == 0,
                        part == 2, at + part * 500 * NS_US)
    return est.report(2001 * NS_MS)


def scenario_gated_below_100() -> dict:
    """The gate itself: 99 frames -> estimate/utilization MUST be 0 while
    the server-time fields are already live. This is the exact shape of the
    D-t10 defect (run 18 sent zeros for 30 s)."""
    est = BandwidthEstimator()
    for frame in range(0, 100):
        at = frame * 20 * NS_MS
        ts = 90000 + frame * 1800
        for part in range(3):
            est.observe(frame, frame * 3 + part, ts, 200, part == 0,
                        part == 2, at + part * 500 * NS_US)
    return est.report(2000 * NS_MS)


def scenario_wrap() -> dict:
    est = BandwidthEstimator()
    est.observe(1, 10, U32 - 899, 300, True, True, 0)
    est.observe(2, 11, 901, 300, True, True, 20 * NS_MS)
    return est.report(20 * NS_MS)


def scenario_parity() -> dict:
    est = BandwidthEstimator()
    est.observe(1, 10, 90000, 400, True, False, 0)
    est.observe_parity(1, 11)
    est.observe(1, 12, 90000, 400, False, True, 1 * NS_MS)
    est.observe(2, 13, 91800, 400, True, True, 20 * NS_MS)
    return est.report(20 * NS_MS)


def scenario_reorder() -> dict:
    est = BandwidthEstimator()
    est.observe(1, 100, 90000, 1200, True, False, 0)
    est.observe(1, 102, 90000, 1200, False, True, 1 * NS_MS)
    est.observe(3, 103, 93600, 1200, True, True, 40 * NS_MS)
    est.observe(2, 101, 91800, 1200, True, True, 41 * NS_MS)
    return est.report(50 * NS_MS)


def scenario_backward_clock() -> dict:
    """The run-18 shape: the sender's 90 kHz clock advances 18.6 s while the
    wall clock advances 30.5 s (a paused/reset encoder clock). Packets whose
    timestamp goes backwards contribute NOTHING (delta < 0 early return), so
    the transit minimum cannot be poisoned into a negative-delay lie."""
    est = BandwidthEstimator()
    ts = 90000
    for i in range(120):
        if i == 60:
            ts -= 90000            # the clock jumps BACKWARDS by 1 s
        else:
            ts += 1800
        # one packet per frame; the sequence is per-PACKET and monotonic, so
        # frame i carries sequence i (each frame closes with contiguous=true)
        est.observe(i, i, ts & U32, 1000, True, True, i * 20 * NS_MS)
    return est.report(2400 * NS_MS)


def scenario_lossy() -> dict:
    """Frames 1,2,4,7 arrive complete; 3,5,6 never do -> lossy_frames counts
    the gaps (distance-1) and the incomplete trains."""
    est = BandwidthEstimator()
    for frame, at in ((1, 0), (2, 20 * NS_MS), (4, 60 * NS_MS), (7, 120 * NS_MS)):
        est.observe(frame, frame * 10, 90000 + (frame - 1) * 1800, 800,
                    True, True, at)
    return est.report(140 * NS_MS)


BW_SCENARIOS = [
    ("train_101", scenario_train_101),
    ("gated_below_100", scenario_gated_below_100),
    ("wrap", scenario_wrap),
    ("parity", scenario_parity),
    ("reorder", scenario_reorder),
    ("backward_clock", scenario_backward_clock),
    ("lossy", scenario_lossy),
]

# server-control buffers: (name, hex, expected kinds)
SCTL_VECTORS = [
    ("timer_active", server_command(0x104, struct.pack("<I", 45)), "timer"),
    ("timer_expired", server_command(0x105, struct.pack("<I", 0)), "timer"),
    ("termination_resume", server_command(0x109, struct.pack(">I", 0x80040010)),
     "termination"),
    ("termination_final", server_command(0x109, struct.pack(">I", 0x80030023)),
     "termination"),
    ("termination_unknown", server_command(0x109, struct.pack(">I", 0xDEADBEEF)),
     "termination"),
    ("pacing_off", server_command(0x112, struct.pack("<II", 0, 0)), "pacing_mode"),
    ("pacing_on", server_command(0x112, struct.pack("<II", 0, 1)), "pacing_mode"),
    ("pacing_other_stream", server_command(0x112, struct.pack("<II", 1, 0)),
     "pacing_mode"),
    ("pacing_bad_length", server_command(0x112, bytes(7)), "error"),
    ("batched", (server_command(0x104, struct.pack("<I", 30))
                 + server_command(0x10A, b"{}")
                 + server_command(0x112, struct.pack("<II", 0, 1))), "batch"),
    ("unknown_then_malformed", (server_command(0x999, bytes([1, 2, 3]))
                                + server_command(0x104, bytes([0]))
                                + server_command(0x112, bytes(8))), "error"),
    ("truncated_header", b"\x01\x02", "error"),
    ("empty", b"", "none"),
    ("run18_inventory", (server_command(0x020E, struct.pack("<H", 3))
                         + server_command(0x0111, bytes(16))
                         + server_command(0x010A, bytes(40))
                         + server_command(0x010F, bytes(8))
                         + server_command(0x0106, bytes(4))
                         + server_command(0x010E, bytes(12))
                         + server_command(0x0101, bytes(20))), "unhandled_batch"),
]


# ==================================================================== emitter
def emit(path: str) -> None:
    out = []
    out.append("/* kn_bw_kat_vectors.h — GENERATED by tools/bw_reference.py.\n"
               " * Do not edit. The generator self-proves against the reference\n"
               " * client's own Rust unit tests (nvst_bandwidth.rs:250-416 and\n"
               " * nvst_server_control_tests.rs) before writing this file, so the\n"
               " * expectations below are the REFERENCE's, transcribed once.\n"
               " * Regenerate:  python3 tools/bw_reference.py\n */\n")
    out.append("#ifndef KN_BW_KAT_VECTORS_H\n#define KN_BW_KAT_VECTORS_H\n\n"
               "#include \"kn/kn_bw.h\"\n\n")

    out.append("/* One replayed scenario: the C suite runs the packet script\n"
               " * named here (see scenario scripts in test_knbw.c, which are\n"
               " * the literal transcription of the Python generator's) and\n"
               " * compares the report field by field. */\n")
    out.append("typedef struct {\n"
               "    const char *name;\n"
               "    uint32_t minimum_server_time, median_server_time;\n"
               "    uint32_t queue_delay_us, lossy_frames, estimate_kbps;\n"
               "    uint16_t jitter_us;\n"
               "    uint8_t  utilization_percent;\n"
               "} kn_bw_kat;\n\n")
    out.append("static const kn_bw_kat kn_bw_kat_scenarios[] = {\n")
    for name, fn in BW_SCENARIOS:
        r = fn()
        out.append(
            f"    {{ \"{name}\", {r['minimum_server_time']}u, "
            f"{r['median_server_time']}u, {r['queue_delay_us']}u, "
            f"{r['lossy_frames']}u, {r['estimate_kbps']}u, "
            f"{r['jitter_us']}u, {r['utilization_percent']}u }},\n")
    out.append("};\n")
    out.append("static const size_t kn_bw_kat_scenarios_n =\n"
               "    sizeof kn_bw_kat_scenarios / sizeof kn_bw_kat_scenarios[0];\n\n")

    out.append("/* server-control buffers, verbatim bytes. */\n")
    for i, (name, buf, _) in enumerate(SCTL_VECTORS):
        raw = bytes(buf)
        body = ", ".join(f"0x{b:02x}" for b in raw) if raw else "0x00"
        out.append(f"static const uint8_t kat_sctl_{i}[] = {{ {body} }};\n")
    out.append("\ntypedef struct {\n"
               "    const char *name;\n"
               "    const uint8_t *bytes;\n"
               "    size_t len;\n"
               "    unsigned n_msgs;          /* records kn_sctl_walk must yield */\n"
               "    uint16_t codes[8];        /* the expected kind per record,\n"
               "                                 * KN_SCTL_* */\n"
               "    unsigned timer_active;    /* 0/1 for the first TIMER record */\n"
               "    uint32_t seconds_left;\n"
               "    uint32_t wire_code;\n"
               "    int resumable;            /* -1 = undecidable */\n"
               "    uint32_t stream_index;\n"
               "    int pacing_interval;\n"
               "    unsigned unknown, malformed, invalid_stream;\n"
               "} kn_sctl_kat;\n\n")

    kind_enum = {"timer": "KN_SCTL_TIMER", "termination": "KN_SCTL_TERMINATION",
                 "pacing_mode": "KN_SCTL_PACING_MODE",
                 "unhandled": "KN_SCTL_UNHANDLED", "unknown": "KN_SCTL_UNKNOWN",
                 "error": "KN_SCTL_MALFORMED"}
    out.append("static const kn_sctl_kat kn_sctl_kat_vectors[] = {\n")
    for i, (name, buf, tag) in enumerate(SCTL_VECTORS):
        raw = bytes(buf)
        msgs = server_control_messages(raw)
        codes = [kind_enum[m["kind"]] for m in msgs]
        if not codes:
            codes = []
        codes_c = ", ".join(codes) if codes else "0"
        timer_active = 0
        seconds = 0
        wire = 0
        resumable = -1
        stream = 0
        interval = 0
        unknown = sum(1 for m in msgs if m["kind"] == "unknown")
        malformed = sum(1 for m in msgs if m["kind"] == "error")
        invalid_stream = sum(1 for m in msgs
                             if m["kind"] == "pacing_mode" and m["stream_index"] != 0)
        for m in msgs:
            if m["kind"] == "timer" and not timer_active and not seconds:
                timer_active = 1 if m["active"] else 0
                seconds = m["seconds_left"]
            if m["kind"] == "termination":
                wire = m["wire_code"]
                resumable = -1 if m["resumable"] is None else (1 if m["resumable"] else 0)
            if m["kind"] == "pacing_mode":
                stream = m["stream_index"]
                interval = 1 if m["interval"] else 0
        out.append(
            f"    {{ \"{name}\", kat_sctl_{i}, {len(raw)}, {len(msgs)}u, "
            f"{{ {codes_c} }}, {timer_active}u, {seconds}u, {wire}u, "
            f"{resumable}, {stream}u, {interval}, {unknown}u, {malformed}u, "
            f"{invalid_stream}u }},\n")
    out.append("};\n")
    out.append("static const size_t kn_sctl_kat_vectors_n =\n"
               "    sizeof kn_sctl_kat_vectors / sizeof kn_sctl_kat_vectors[0];\n\n")

    out.append("/* The reference's own asserted values (nvst_bandwidth.rs:277-288),\n"
               " * pinned separately so a generator regression cannot move them. */\n")
    g = scenario_train_101()
    out.append("#define KN_BW_KAT_RUST_MINIMUM_SERVER_TIME %du\n" % g["minimum_server_time"])
    out.append("#define KN_BW_KAT_RUST_ESTIMATE_KBPS %du\n" % g["estimate_kbps"])
    out.append("#define KN_BW_KAT_RUST_UTILIZATION_PERCENT %du\n" % g["utilization_percent"])
    out.append("#define KN_BW_KAT_RUST_QUEUE_DELAY_US %du\n" % g["queue_delay_us"])
    out.append("#define KN_BW_KAT_RUST_PARITY_KBPS 6400.0\n")
    out.append("#define KN_BW_KAT_RUST_REORDER_LOSSY 2u\n")
    out.append("\n#endif /* KN_BW_KAT_VECTORS_H */\n")

    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(out))
    print(f"wrote {path} ({os.path.getsize(path)} bytes)")


def main() -> int:
    self_prove()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    emit(os.path.join(root, "streamer", "src", "kn_bw_kat_vectors.h"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
