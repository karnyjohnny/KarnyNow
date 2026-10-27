#!/usr/bin/env python3
"""rx_reference.py — independent Python reference for the NVST video receive
pipeline (kn_rx) and generator of the C known-answer vectors.

Line-by-line port of nvst.rs / nvst_control.rs / nvst_invalidation.rs
@ a49b124 (line numbers verified THIS round against the live tree):

  RtpReorderBuffer::{push,poll_deadline,drain}   nvst.rs:3537-3660
  FecBlock::{new,matches,insert,finish,...}      nvst.rs:3925-4117
  FecReorderBuffer::{push,poll_deadline}         nvst.rs:4120-4310
  VideoAccessUnitAssembler::push                 nvst.rs:3292-3447
  NvstVideoReceiver::{process_datagram tail,
    consume_reorder_result, invalidate_picture,
    poll_timeout, poll_frame_progress,
    poll_recovery, lifecycle, reset_media_state} nvst.rs:4414-4961
  FrameInvalidation + feedback queue             nvst_invalidation.rs:45-269
  FrameProgressWatchdog                          nvst.rs:2422-2442
  NACK tracker (request/resolve/take/...)        nvst.rs:946-985, 1272-1333
  nack_v2 / ServerControl encoders               nvst_control.rs:3-181
  RTCP generic NACK + PLI                        nvst.rs:3001-3048
  SrtpReceiver::unprotect (GCM path)             nvst.rs:2635-2740
  derive_aes_cm_key (KDF256)                     nvst.rs:2873-2886

Reuses (both are independent ports themselves, self-proved in earlier
rounds): gs_reference.py (RTP/GS parse + builders + Annex-B helpers) and
fec_reference.py (GF(2^8) Cauchy encode/reconstruct).

Before emitting anything this script PROVES itself:
  1. encoder goldens transcribed from the reference's own Rust tests
     (nvst_control.rs:210-260, nvst.rs:13406-13419),
  2. the reorder 52-ms dequeue-deadline scenario and the NACK-exclusion
     scenario transcribed from nvst.rs:13329-13404,
  3. the FEC parity-only and retain-successor scenarios transcribed from
     nvst.rs:13040-13327 (byte-exact recovered-packet assertions),
  4. the pipeline gaps_require_recovery scenario (nvst.rs:13406-13450),
  5. AES-256 ECB against FIPS-197 C.3, AES-GCM against the GCM spec
     case-1 tag and the H = E(K0, 0) constant, then KDF256/GCM SRTP
     vectors derived from the proved primitives,
  6. randomized pipeline properties (loss/reorder/dup/stale/FEC): frames
     stay byte-identical to the lossless baseline whenever every lost
     packet is FEC-recovered; delivered indices never repeat or go
     backwards; NACK ranges never contain a buffered index.
If any self-check fails, NOTHING is emitted.

Usage:  python3 tools/rx_reference.py
Emits:  streamer/src/kn_rx_kat_vectors.h
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "streamer", "src", "kn_rx_kat_vectors.h")
sys.path.insert(0, HERE)

import gs_reference as gsr            # noqa: E402
import fec_reference as fecr          # noqa: E402

# ------------------------------------------------------------- constants
# (nvst.rs line numbers verified this round)

REORDER_TIMEOUT_MS = 52            # :229 MJOLNIR_REORDER_DEQUEUE_TIMEOUT
DEFAULT_REORDER_WINDOW = 1024      # :224
MAX_REORDER_WINDOW = 2048          # :225
FEC_HEADER_ALLOWANCE = 16          # :215 NVST_FEC_RTP_HEADER_ALLOWANCE
MAX_PENDING_FEC_BLOCKS = 128       # :211
NACK_RETRY_MS = 4                  # :99
NACK_TRACKING_MS = 52              # :101
NACK_MAX_ATTEMPTS = 3              # :102
NACK_MAX_PENDING = 2048            # :105
NACK_MAX_COUNT = 64                # nvst_control.rs:9
NACK_MAX_FCI = 4                   # :208 div_ceil(64, 17)
RFI_RETRY_MS = 100                 # nvst_invalidation.rs:13
RFI_BACKSTOP_MS = 250              # nvst.rs:103
DEFAULT_VIDEO_PACKET_SIZE = 1280   # :216
KN_FEC_MAX_SHARDS_PY = 255         # knfec Cauchy bound (nvst.rs:3806)
DEFAULT_AU_CAP = 2 * 1024 * 1024   # :236
DEFAULT_TIMEOUT_MS = 5000          # :238

FLAG_PIC = 0x01
FLAG_EOF = 0x02
FLAG_SOF = 0x04
SPI_MASK = 0x00FFFFFF

# wire frame types (nvst_invalidation.rs:17-38)
FT_P, FT_I, FT_HEADER, FT_INTRA, FT_REF_PIC_I, FT_NON_REF_P = 1, 2, 3, 4, 5, 6

# kn_rx.h drop reasons (must match the C enum exactly)
D_NONE = 0
D_PAUSED = 1
D_STOPPED = 2
D_RECOVERY_REQUIRED = 3
D_MALFORMED_RTP = 4
D_AUTH_FAILED = 5
D_REPLAY_REJECTED = 6
D_UNEXPECTED_PT = 7
D_UNEXPECTED_SSRC = 8
D_STALE = 9
D_DUPLICATE = 10
D_UNSUPPORTED_FEC = 11
D_AWAITING_SOF = 12
D_FRAME_DISCONTINUITY = 13
D_MISSING_START_CODE = 14
D_INVALID_AU_LENGTH = 15
D_AU_TOO_LARGE = 16
D_FEC_LAYOUT = 17
D_POOL = 18
D_PKT_TOO_LARGE = 19
D_NO_CIPHER = 20

# kn_rx.h event types
EV_NONE = 0
EV_DROPPED = 1
EV_RECOVERY_GAP = 2
EV_RECOVERY_TIMEOUT = 3
EV_LIFECYCLE = 4
EV_PROGRESS_RESUMED = 5
EV_PROGRESS_STALL = 6
EV_PROGRESS_RECOVERY = 7

ST_RUNNING, ST_PAUSED, ST_RECOVERY, ST_STOPPED = 0, 1, 2, 3

# kn_gs.h error codes mirrored (gs_reference constants)
GS_OK = gsr.OK


class Drop(Exception):
    """Assembler/pipeline drop with a kn_rx drop reason + detail."""

    def __init__(self, reason, detail=0):
        Exception.__init__(self)
        self.reason = reason
        self.detail = detail


# --------------------------------------------------------- packet builders

def xorshift32_stream(seed, n):
    """Deterministic byte stream (must match the C builder in test_knrx.c)."""
    x = (seed & 0xFFFFFFFF) | 1
    out = bytearray()
    for _ in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        out.append((x & 0xFF) | 1)     # odd bytes: no accidental start codes
    return bytes(out)


def build_media(gs_hdr_wire, nal_type, body_seed, body_len, aud_tail):
    """Picture payload: optional 4-byte GS frame header (47 53 00 wire),
    one Annex-B NAL, optional trailing AUD (nal 9). Mirrors the C builder."""
    media = b""
    if gs_hdr_wire is not None:
        media += bytes([0x47, 0x53, 0x00, gs_hdr_wire & 0xFF])
    media += b"\x00\x00\x01" + bytes([nal_type & 0xFF])
    media += xorshift32_stream(body_seed, body_len)
    if aud_tail:
        media += b"\x00\x00\x01\x09\xf0"
    return media


def build_pkt(seq, ts, ssrc, flags, spi, frame_index, cur, last,
              pct, fidx, fsrc, media, pt=96, marker=0):
    ext = gsr.build_gs_ext(spi, frame_index, flags, cur, last, pct, fidx,
                           fsrc)
    return gsr.build_rtp(pt=pt, seq=seq & 0xFFFF, ts=ts, ssrc=ssrc,
                         marker=marker, gs_ext=ext, payload=media)


def rtp_parse_hdr(pkt):
    """Structural parse WITHOUT padding validation (RtpHeader::parse alone,
    nvst.rs:2456-2510) — padding lives in the encrypted payload for SRTP and
    in the FEC algebra for recovered shards; payload() validates it later."""
    if len(pkt) < 12:
        return gsr.E_TOO_SHORT, None
    first = pkt[0]
    if first >> 6 != 2:
        return gsr.E_INVALID_VERSION, None
    cc = first & 0x0F
    payload_off = 12 + cc * 4
    if len(pkt) < payload_off:
        return gsr.E_INVALID_CSRC, None
    has_gs, gs_off = 0, 0
    if first & 0x10:
        if len(pkt) < payload_off + 4:
            return gsr.E_INVALID_EXT, None
        profile = gsr.be16(pkt[payload_off:payload_off + 2])
        words = gsr.be16(pkt[payload_off + 2:payload_off + 4])
        ext_end = payload_off + words * 4 + 4
        if len(pkt) < ext_end:
            return gsr.E_INVALID_EXT, None
        if profile == gsr.GS_VIDEO_EXTENSION_PROFILE:
            if words * 4 != 16:
                return gsr.E_INVALID_EXT, None
            gs_off = payload_off + 4
            has_gs = 1
        payload_off = ext_end
    return GS_OK, {
        "pt": pkt[1] & 0x7F, "seq": gsr.be16(pkt[2:4]),
        "ts": gsr.be32(pkt[4:8]), "ssrc": gsr.be32(pkt[8:12]),
        "payload_off": payload_off, "has_padding": 1 if first & 0x20 else 0,
        "has_gs": has_gs, "gs_off": gs_off,
    }


def rtp_payload(hdr, pkt):
    """payload() port (nvst.rs:2512-2525): bounds + padding validation."""
    end = len(pkt)
    if end < hdr["payload_off"]:
        return gsr.E_TOO_SHORT, 0, 0
    if hdr["has_padding"]:
        if end == 0:
            return gsr.E_INVALID_PADDING, 0, 0
        padding = pkt[-1]
        if padding == 0 or padding > end - hdr["payload_off"]:
            return gsr.E_INVALID_PADDING, 0, 0
        end -= padding
    return GS_OK, hdr["payload_off"], end - hdr["payload_off"]


def sat_sub(a, b):
    return a - b if a > b else 0


# ------------------------------------------------- 2. RtpReorderBuffer port

class ReorderResult(object):
    __slots__ = ("ready", "nack", "recovery", "dropped", "fec_repaired")

    def __init__(self):
        self.ready = []
        self.nack = None
        self.recovery = None
        self.dropped = None
        self.fec_repaired = 0


class Packet(object):
    """RtpPacket (nvst.rs:2530-2536): index, parsed header, plaintext,
    origin. `slot` is the C-side pool analogue (identity for the
    retains-original-buffer assertion)."""
    __slots__ = ("index", "hdr", "data", "origin", "slot")

    def __init__(self, index, data, hdr=None, origin=0, slot=None):
        self.index = index
        self.data = data
        self.origin = origin
        self.slot = slot if slot is not None else id(data)
        if hdr is None:
            rc, hdr = rtp_parse_hdr(data)
            if rc != GS_OK:
                hdr = None
        self.hdr = hdr


class RtpReorderBuffer(object):
    """nvst.rs:3537-3660 — BTreeMap semantics via a sorted-view dict."""

    def __init__(self, max_packets):
        self.next_index = None
        self.packets = {}
        self.max_packets = max_packets
        self.gap_wait = None

    def reset(self):
        self.next_index = None
        self.packets = {}
        self.gap_wait = None

    def push(self, packet, now):
        index = packet.index
        if self.next_index is None:
            self.next_index = index
        next_index = self.next_index
        if index < next_index:
            r = ReorderResult()
            r.dropped = (D_STALE, index)
            return r
        if index in self.packets:
            r = ReorderResult()
            r.dropped = (D_DUPLICATE, index)
            return r
        buffered_before = len(self.packets)
        recovery = None
        if buffered_before == 0 and sat_sub(index, next_index) >= self.max_packets:
            recovery = (next_index, index - 1)
            self.next_index = index
            self.gap_wait = None
        self.packets[index] = packet
        return self.drain(now, recovery)

    def poll_deadline(self, now):
        return self.drain(now, None)

    def drain(self, now, recovery):
        ready = []
        nack = None
        while True:
            while self.next_index is not None and self.next_index in self.packets:
                ready.append(self.packets.pop(self.next_index))
                self.next_index += 1
            if not self.packets:
                self.gap_wait = None
                break
            first_available = min(self.packets)
            expected = self.next_index
            assert first_available > expected or expected is None
            nack = (expected, sat_sub(first_available, 1))
            if self.gap_wait is not None and self.gap_wait[0] == expected:
                gap_started = self.gap_wait[1]
            else:
                self.gap_wait = (expected, now)
                gap_started = now
            gap_exceeded_window = (
                sat_sub(first_available, expected) >= self.max_packets
                or len(self.packets) >= self.max_packets)
            gap_timed_out = sat_sub(now, gap_started) >= REORDER_TIMEOUT_MS
            if not gap_exceeded_window and not gap_timed_out:
                break
            recovery = (expected, first_available - 1)
            ready = []                     # ready.clear() drops the packets
            self.next_index = first_available
            self.gap_wait = None
            nack = None
        r = ReorderResult()
        r.ready = ready
        r.nack = nack
        r.recovery = recovery
        return r


# -------------------------------------------------------- 3. FEC path port

class FecBlock(object):
    """nvst.rs:3925-4117."""

    def __init__(self, layout, packet_index):
        self.layout = dict(layout)
        self.base_index = sat_sub(packet_index, layout["shard"])
        total = layout["data"] + layout["parity"]
        self.shards = [None] * total
        self.repair_failed = False

    def matches(self, layout, packet_index):
        l, m = self.layout, layout
        return (l["frame_index"] == m["frame_index"]
                and l["block"] == m["block"]
                and l["last_block"] == m["last_block"]
                and l["data"] == m["data"]
                and l["parity"] == m["parity"]
                and self.base_index == sat_sub(packet_index, m["shard"]))

    def missing_data_range(self):
        d = self.layout["data"]
        miss = [i for i in range(d) if self.shards[i] is None]
        if not miss:
            return None
        return (self.base_index + miss[0], self.base_index + miss[-1])

    def first_missing_data_run(self):
        d = self.layout["data"]
        miss = [i for i in range(d) if self.shards[i] is None]
        if not miss:
            return None
        first = miss[0]
        length = 1
        while first + length < d and self.shards[first + length] is None:
            length += 1
        return (self.base_index + first, self.base_index + first + length - 1)

    def insert(self, layout, packet):
        slot = layout["shard"]
        if self.shards[slot] is not None:
            return (D_DUPLICATE, packet.index)
        self.shards[slot] = packet
        self.repair_failed = False         # nvst.rs:3988-3991
        return None

    def has_enough_shards(self):
        return sum(1 for s in self.shards if s is not None) \
            >= self.layout["data"]

    def finish(self, shard_len):
        """Returns (data_packets, repaired_count); raises Drop on failure.
        nvst.rs:3998-4117."""
        d = self.layout["data"]
        total = d + self.layout["parity"]
        missing = [i for i in range(d) if self.shards[i] is None]
        if not missing:
            return ([self.shards[i] for i in range(d)], 0)
        for s in self.shards:
            if s is not None and len(s.data) > shard_len:
                raise Drop(D_UNSUPPORTED_FEC)
        template = next((s for s in self.shards if s is not None), None)
        if template is None or template.hdr is None:
            raise Drop(D_UNSUPPORTED_FEC)
        ext_start = template.hdr["payload_off"] - 16
        if ext_start < 0:
            raise Drop(D_MALFORMED_RTP, gsr.E_INVALID_EXT)
        shard_bytes = [None if s is None
                       else bytes(s.data).ljust(shard_len, b"\0")[:shard_len]
                       for s in self.shards]
        try:
            recovered = fecr.fec_reconstruct(shard_bytes, d,
                                             self.layout["parity"], shard_len)
        except ValueError:
            raise Drop(D_UNSUPPORTED_FEC)
        out = []
        for i in range(d):
            if self.shards[i] is not None:
                out.append(self.shards[i])
                continue
            plain = bytearray(recovered[i])
            if len(plain) < ext_start + 16:
                raise Drop(D_MALFORMED_RTP, gsr.E_INVALID_EXT)
            sequence = (self.base_index + i) & 0xFFFF
            plain[0:ext_start] = template.data[0:ext_start]
            plain[2:4] = bytes([(sequence >> 8) & 0xFF, sequence & 0xFF])
            fi = self.layout["frame_index"]
            plain[ext_start + 4:ext_start + 8] = bytes(
                [fi & 0xFF, (fi >> 8) & 0xFF, (fi >> 16) & 0xFF,
                 (fi >> 24) & 0xFF])
            plain[ext_start + 11] = ((self.layout["last_block"] & 3) << 6) \
                | ((self.layout["block"] & 3) << 4)
            rc, hdr = rtp_parse_hdr(bytes(plain))
            if rc != GS_OK or not hdr["has_gs"]:
                raise Drop(D_MALFORMED_RTP, rc if rc != GS_OK
                           else gsr.E_MISSING_NV_VIDEO)
            flags = plain[hdr["gs_off"] + 8] & 0x0F
            if flags & ~(FLAG_SOF | FLAG_EOF | FLAG_PIC):
                raise Drop(D_UNSUPPORTED_FEC)     # nvst.rs:4089-4094
            # pitfall #4: the fec_word of a recovered shard is NOT validated
            out.append(Packet(self.base_index + i, bytes(plain), hdr,
                              origin=1))
        return (out, len(missing))


class FecReorderBuffer(object):
    """nvst.rs:4120-4310."""

    def __init__(self, video_packet_size, max_blocks=MAX_PENDING_FEC_BLOCKS,
                 max_shards=255):
        self.blocks = {}
        self.completed_through = None
        self.gap_wait = None
        self.shard_len = video_packet_size + FEC_HEADER_ALLOWANCE
        self.max_blocks = max_blocks
        self.max_shards = max_shards

    def reset(self):
        self.blocks = {}
        self.completed_through = None
        self.gap_wait = None

    def push(self, packet, layout, now):
        packet_base = sat_sub(packet.index, layout["shard"])
        if self.completed_through is not None \
                and packet.index < self.completed_through:
            return ReorderResult()             # silent ignore (4145-4156)
        if packet_base not in self.blocks:
            total = layout["data"] + layout["parity"]
            if total > self.max_shards:
                # deviation D-b: bounded shard table (the reference stores
                # up to 1023 but its reconstruct dies above 255 anyway)
                r = ReorderResult()
                r.dropped = (D_UNSUPPORTED_FEC, packet.index)
                return r
            if len(self.blocks) >= self.max_blocks:
                r = ReorderResult()
                r.dropped = (D_POOL, packet.index)
                return r
            self.blocks[packet_base] = FecBlock(layout, packet.index)
        block = self.blocks[packet_base]
        if not block.matches(layout, packet.index):
            r = ReorderResult()
            r.dropped = (D_UNSUPPORTED_FEC, packet.index)
            return r
        dropped = block.insert(layout, packet)
        if dropped is not None:
            r = ReorderResult()
            r.dropped = dropped
            return r
        return self.poll_deadline(now)

    def poll_deadline(self, now):
        res = ReorderResult()
        while True:
            if not self.blocks:
                self.gap_wait = None
                break
            base = min(self.blocks)
            head = self.blocks[base]
            if self.completed_through is not None and base > self.completed_through:
                expected = self.completed_through
                res.nack = (expected, base - 1)
                if self.gap_wait is not None and self.gap_wait[0] == expected:
                    started = self.gap_wait[1]
                else:
                    self.gap_wait = (expected, now)
                    started = now
                if sat_sub(now, started) < REORDER_TIMEOUT_MS \
                        and sat_sub(base, expected) < MAX_REORDER_WINDOW \
                        and len(self.blocks) < self.max_blocks:
                    break
                res.recovery = (expected, base - 1)
                res.ready = []
                res.nack = None
                self.completed_through = base
                self.gap_wait = None
                continue
            if head.has_enough_shards() and not head.repair_failed:
                del self.blocks[base]
                completed_through = base + head.layout["data"] \
                    + head.layout["parity"]
                backup_layout = dict(head.layout)
                backup_shards = list(head.shards)
                backup_base = head.base_index
                try:
                    ready, repaired = head.finish(self.shard_len)
                    res.ready.extend(ready)
                    res.fec_repaired += repaired
                    self.completed_through = completed_through
                    self.gap_wait = None
                    continue
                except Drop:
                    pending = FecBlock(backup_layout, backup_base)
                    pending.shards = backup_shards
                    pending.repair_failed = True
                    self.blocks[base] = pending
                    run = pending.first_missing_data_run()
                    assert run is not None
                    res.nack = run
                    self.gap_wait = (run[0], now)
                    break
            if len(self.blocks) == 1 and self.gap_wait is None:
                break
            run = head.first_missing_data_run()
            assert run is not None
            res.nack = run
            if self.gap_wait is not None and self.gap_wait[0] == run[0]:
                started = self.gap_wait[1]
            else:
                self.gap_wait = (run[0], now)
                started = now
            block_span = sat_sub(max(self.blocks), base)
            if sat_sub(now, started) < REORDER_TIMEOUT_MS \
                    and block_span < MAX_REORDER_WINDOW \
                    and len(self.blocks) < self.max_blocks:
                break
            failed = self.blocks.pop(base)
            rng = failed.missing_data_range()
            assert rng is not None
            res.recovery = rng
            res.ready = []
            res.nack = None
            self.completed_through = base + failed.layout["data"] \
                + failed.layout["parity"]
            self.gap_wait = None
        return res


# --------------------------------------------- AU assembler port (3292+)

class Assembler(object):
    """VideoAccessUnitAssembler (nvst.rs:3260-3447)."""

    def __init__(self, codec, max_au):
        self.codec = codec
        self.max_au = max_au
        self.reset()
        self.invalid_av1_length_logged = False

    def reset(self):
        self.current_frame = None
        self.first_spi = None
        self.frame_type = None          # None or wire value (int)
        self.frame_type_parsed = False
        self.last_packet_payload_length = None
        self.expected_au_length = None
        self.bytes = bytearray()

    @staticmethod
    def _parse_frame_header(payload, header_size):
        """GsFrameHeader::parse (nvst.rs:3279-3289) — codec-INDEPENDENT,
        two ordered arms."""
        if len(payload) >= 4 and payload[0] == 0x47 and payload[1] == 0x53 \
                and header_size >= 4:
            return payload[3] & 0x0F
        if len(payload) >= 4 and payload[0] in (0x01, 0x81) \
                and header_size > 0:
            return payload[3]
        return None

    def push(self, video, timestamp, payload):
        if video["is_fec"]:
            raise Drop(D_UNSUPPORTED_FEC)
        frame_header_size = 0
        picture = payload
        if video["sof"]:
            self.reset()
            rc, off = gsr.video_picture_payload(self.codec, payload)
            if rc != GS_OK:
                raise Drop(D_MISSING_START_CODE)
            picture = payload[off:]
            frame_header_size = len(payload) - len(picture)
            wire = self._parse_frame_header(payload, frame_header_size)
            self.frame_type_parsed = wire is not None
            self.frame_type = wire
            if self.codec == gsr.AV1 and payload and payload[0] == 0x01:
                if len(payload) >= 6:
                    self.last_packet_payload_length = \
                        payload[4] | (payload[5] << 8)
            elif self.codec == gsr.AV1 and payload and payload[0] == 0x81:
                if len(payload) >= 20:
                    self.expected_au_length = gsr.le32(payload[16:20])
            self.current_frame = video["frame_index"]
            self.first_spi = video["spi"]
        elif self.current_frame != video["frame_index"]:
            self.reset()
            raise Drop(D_AWAITING_SOF)

        if self.codec == gsr.AV1 and video["eof"] \
                and self.last_packet_payload_length is not None:
            reported = self.last_packet_payload_length
            exact = reported - frame_header_size
            if exact > 0 and exact <= len(picture):
                picture = picture[:exact]
            # else: hint ignored (log-once in the reference)

        remaining = sat_sub(self.max_au, len(self.bytes))
        if len(picture) > remaining:
            self.reset()
            raise Drop(D_AU_TOO_LARGE)
        self.bytes += picture
        if not video["eof"]:
            return None

        out = self.bytes
        self.bytes = bytearray()
        if self.codec == gsr.AV1 and self.expected_au_length is not None:
            reported = self.expected_au_length
            if 0 < reported <= len(out):
                out = out[:reported]
            else:
                self.reset()
                raise Drop(D_INVALID_AU_LENGTH)
        keep = gsr.strip_trailing_aud(self.codec, bytes(out))
        out = out[:keep]
        frame = {
            "timestamp": timestamp,
            "frame_index": video["frame_index"],
            "first_spi": self.first_spi,
            "frame_type": self.frame_type,
            "frame_type_parsed": self.frame_type_parsed,
            "keyframe": gsr.is_keyframe(
                self.codec, bytes(out),
                1 if self.frame_type == FT_I else 0),
            "contiguous": True,
            "bytes": bytes(out),
        }
        self.current_frame = None
        self.first_spi = None
        return frame


# ------------------------------------------------- 4. frame invalidation

class Invalidation(object):
    """FrameInvalidation + FrameInvalidationFeedback (nvst_invalidation.rs)."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.last_frame = None
        self.frame_types_known = False
        self.pending = None
        self.active_since = None
        self.last_retry = None
        self.backstop_requested = False
        # feedback queue
        self.unsent = None
        self.in_flight = False
        self.rfi_last_sent = None
        self.cnt_sent = 0
        self.cnt_dropped = 0
        self.cnt_rec_i = 0
        self.cnt_rec_intra = 0
        self.cnt_rec_ref = 0
        self.cnt_idr_backstops = 0

    def discard(self):
        self.unsent = None
        self.in_flight = False
        self.rfi_last_sent = None

    def queue(self, rng):
        self.unsent = rng

    def take(self):
        rng = self.unsent
        self.unsent = None
        self.in_flight = rng is not None
        return rng

    def restore(self, rng):
        if self.in_flight:
            self.in_flight = False
            if self.unsent is None:
                self.unsent = rng

    def record_sent(self, now):
        if not self.in_flight:
            return
        self.in_flight = False
        self.rfi_last_sent = now
        self.cnt_sent += 1

    def lose(self, now):
        if not self.frame_types_known:
            return False
        if self.active_since is None:
            self.active_since = now
        return True

    def request_idr_backstop(self, kf):
        if self.backstop_requested:
            return
        self.backstop_requested = True
        self.cnt_idr_backstops += 1
        kf.append(1)

    def poll_backstop(self, now, kf):
        if self.pending is not None:
            cands = [t for t in (self.last_retry, self.rfi_last_sent)
                     if t is not None]
            last = max(cands) if cands else None
            if last is None or sat_sub(now, last) >= RFI_RETRY_MS:
                self.queue(self.pending)
                self.last_retry = now
        if self.active_since is not None \
                and sat_sub(now, self.active_since) >= RFI_BACKSTOP_MS:
            self.request_idr_backstop(kf)

    def invalidate(self, previous, frame_index, now, kf):
        if previous is None or frame_index <= previous + 1:
            self.request_idr_backstop(kf)
            return
        lost = (previous + 1, frame_index - 1)
        if self.pending is None:
            rng = lost
        else:
            rng = (min(self.pending[0], lost[0]), max(self.pending[1], lost[1]))
        self.pending = rng
        self.queue(rng)
        self.last_retry = now

    def admit(self, frame, loss_before, now, kf):
        previous = self.last_frame
        self.last_frame = frame["frame_index"]
        self.frame_types_known = frame["frame_type_parsed"]
        frame_gap = previous is not None and frame["frame_index"] > previous + 1
        if frame_gap:
            self.lose(now)
        invalidating = self.active_since is not None
        ft = frame["frame_type"] if frame["frame_type_parsed"] else None
        if ft is None:
            if invalidating:
                kf.append(1)
            self._clear()
            frame["contiguous"] = (not loss_before) and (not invalidating)
            return True
        clears = frame["keyframe"] or ft in (FT_I, FT_INTRA, FT_REF_PIC_I)
        if clears:
            if invalidating:
                if ft == FT_INTRA:
                    self.cnt_rec_intra += 1
                elif ft == FT_REF_PIC_I:
                    self.cnt_rec_ref += 1
                else:
                    self.cnt_rec_i += 1
                self._clear()
            frame["contiguous"] = invalidating or (not loss_before)
            return True
        if not invalidating:
            frame["contiguous"] = not loss_before
            return True
        if loss_before or frame_gap:
            self.invalidate(previous, frame["frame_index"], now, kf)
        if ft == FT_HEADER:
            frame["contiguous"] = True
            return True
        self.cnt_dropped += 1
        return False

    def _clear(self):
        self.discard()
        self.pending = None
        self.active_since = None
        self.last_retry = None
        self.backstop_requested = False


# ------------------------------------------------------ 5. NACK tracker

class NackTracker(object):
    """nvst.rs:946-985 + 1272-1333."""

    def __init__(self, capacity=NACK_MAX_PENDING):
        self.capacity = capacity
        self.pending = {}            # index -> {requested_at, last_sent, attempts}
        self.overflows = 0

    def purge(self, now):
        for idx in [i for i, p in self.pending.items()
                    if sat_sub(now, p["requested_at"]) >= NACK_TRACKING_MS]:
            del self.pending[idx]

    def request(self, first, last, now, kf):
        if first > last:
            return
        self.purge(now)
        for index in range(first, last + 1):
            if index in self.pending:
                continue
            if len(self.pending) == self.capacity:
                self.overflows += 1
                kf.append(1)
                break
            self.pending[index] = {"requested_at": now, "last_sent": None,
                                   "attempts": 0}

    def resolve(self, index):
        pkt = self.pending.pop(index, None)
        return bool(pkt is not None and pkt["last_sent"] is not None)

    def take(self, now, retry_rtt=None):
        if retry_rtt is None:
            retry_interval, max_attempts = NACK_RETRY_MS, NACK_MAX_ATTEMPTS
        else:
            attempts = NACK_MAX_ATTEMPTS if retry_rtt == 0 else \
                max(1, min(NACK_MAX_ATTEMPTS, NACK_TRACKING_MS // retry_rtt))
            retry_interval, max_attempts = retry_rtt + NACK_RETRY_MS, attempts
        self.purge(now)

        def eligible(p):
            return p["attempts"] < max_attempts and (
                p["last_sent"] is None
                or sat_sub(now, p["last_sent"]) >= retry_interval)

        elig = [i for i in sorted(self.pending) if eligible(self.pending[i])]
        if not elig:
            return None
        first = elig[0]
        last = first
        count = 0
        for index in sorted(self.pending):
            if index < first:
                continue
            if count >= NACK_MAX_COUNT:
                break
            p = self.pending[index]
            if index > last + 1 or not eligible(p):
                break
            p["last_sent"] = now
            p["attempts"] = min(255, p["attempts"] + 1)
            last = index
            count += 1
        return (first, last)

    def mark_send_failed(self, first, last):
        for index in range(first, last + 1):
            p = self.pending.get(index)
            if p:
                p["last_sent"] = None
                p["attempts"] = max(0, p["attempts"] - 1)

    def clear(self):
        self.pending = {}


# ------------------------------------------------------- 6. encoders

def ctrl_encode(code, payload):
    return bytes([code & 0xFF, (code >> 8) & 0xFF,
                  len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) \
        + bytes(payload)


def nack_v2(stream_index, missing):
    """nvst_control.rs:40-70."""
    if not missing or len(missing) > NACK_MAX_COUNT:
        return None
    payload = bytearray([2, stream_index & 0xFF, 0])
    base = missing[0] & 0xFFFF
    bitmap = 0
    for seq in missing[1:]:
        distance = (seq - base) & 0xFFFF
        if distance == 0:
            continue
        if distance <= 64:
            bitmap |= 1 << (distance - 1)
        else:
            payload += base.to_bytes(2, "little") + bitmap.to_bytes(8, "little")
            payload[2] += 1
            base = seq & 0xFFFF
            bitmap = 0
    payload += base.to_bytes(2, "little") + bitmap.to_bytes(8, "little")
    payload[2] += 1
    return ctrl_encode(0x0317, payload)


def idr_request():
    return ctrl_encode(0x0302, (0).to_bytes(2, "little"))


def frame_invalidation_range(first, last, stream_index):
    payload = (first & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little") \
        + (last & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little") \
        + (stream_index & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little")
    return ctrl_encode(0x0301, payload)


def frame_ack(frame_number, first_packet_time_ms, frame_bytes):
    import struct
    payload = bytearray(102)
    payload[0:2] = (1).to_bytes(2, "little")
    payload[2:4] = (9).to_bytes(2, "little")
    payload[4:8] = (frame_number & 0xFFFFFFFF).to_bytes(4, "little")
    if first_packet_time_ms is not None:
        payload[12:20] = struct.pack("<d", first_packet_time_ms)
    for off in range(20, 52, 4):
        payload[off:off + 4] = (0xBF800000).to_bytes(4, "little")
    payload[72:76] = (frame_bytes & 0xFFFFFFFF).to_bytes(4, "little")
    payload[94:98] = (0xBF800000).to_bytes(4, "little")
    payload[98:102] = (0xBF800000).to_bytes(4, "little")
    return ctrl_encode(0x0204, payload)


def frame_pacing(frame_number, target_us, error_us):
    payload = bytearray(28)
    payload[0:4] = (5).to_bytes(4, "little")
    payload[8:12] = (2).to_bytes(4, "little")
    payload[12:16] = (frame_number & 0xFFFFFFFF).to_bytes(4, "little")
    payload[16:20] = (target_us & 0xFFFFFFFF).to_bytes(4, "little")
    payload[20:24] = (min(error_us, target_us) & 0xFFFFFFFF).to_bytes(4, "little")
    payload[24:28] = (0x341A).to_bytes(4, "little")
    return ctrl_encode(0x0203, payload)


def qos_report(sequence, sender_frame, min_server_time, queue_delay_us,
               jitter_us, loss_per_ten_thousand, utilization, client_time,
               lossy_frames, estimate_kbps, median_server_time):
    payload = bytearray(52)
    def pu32(off, v):
        payload[off:off + 4] = (v & 0xFFFFFFFF).to_bytes(4, "little")
    def pu16(off, v):
        payload[off:off + 2] = (v & 0xFFFF).to_bytes(2, "little")
    pu32(0, 7)
    pu32(8, sequence)
    pu32(12, sender_frame)
    pu32(16, min_server_time)
    pu32(20, queue_delay_us)
    pu16(24, jitter_us)
    pu16(26, loss_per_ten_thousand)
    payload[28] = utilization & 0xFF
    pu16(30, 1000)
    pu16(32, 1000)
    pu32(36, client_time)
    pu32(40, lossy_frames)
    pu32(44, estimate_kbps)
    pu32(48, median_server_time)
    return ctrl_encode(0x0207, payload)


def rtcp_pli(sender_ssrc, media_ssrc):
    return bytes([0x81, 206]) + (2).to_bytes(2, "big") \
        + (sender_ssrc & 0xFFFFFFFF).to_bytes(4, "big") \
        + (media_ssrc & 0xFFFFFFFF).to_bytes(4, "big")


def rtcp_nack(sender_ssrc, media_ssrc, first, last):
    """nvst.rs:3015-3048."""
    entries = []
    effective_last = min(last, first + NACK_MAX_COUNT - 1)
    current = first
    while current <= effective_last and len(entries) < NACK_MAX_FCI:
        pid = current & 0xFFFF
        represented = min(effective_last - current, 16)
        blp = 0xFFFF if represented == 16 else (1 << represented) - 1
        entries.append((pid, blp))
        current += 17
    packet = bytes([0x81, 205]) + (2 + len(entries)).to_bytes(2, "big") \
        + (sender_ssrc & 0xFFFFFFFF).to_bytes(4, "big") \
        + (media_ssrc & 0xFFFFFFFF).to_bytes(4, "big")
    for pid, blp in entries:
        packet += pid.to_bytes(2, "big") + blp.to_bytes(2, "big")
    return packet


# ------------------------------------------------ AES-256 + GCM oracle

_SBOX = None
_RSBOX = None


def _sbox():
    global _SBOX
    if _SBOX is None:
        import srtp_reference as sr
        _SBOX = sr._make_sbox()
    return _SBOX


def _xtime(a):
    import srtp_reference as sr
    return sr._xtime(a)


def _expand_key_any(key):
    """AES key expansion for 128/192/256-bit keys (FIPS-197 §5.2)."""
    sbox = _sbox()
    nk = len(key) // 4
    nr = nk + 6
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (nr + 1)):
        temp = list(w[i - 1])
        if i % nk == 0:
            temp = temp[1:] + temp[:1]
            temp = [sbox[b] for b in temp]
            temp[0] ^= rcon
            rcon = ((rcon << 1) ^ 0x11B) & 0xFF if rcon & 0x80 else rcon << 1
        elif nk > 6 and i % nk == 4:
            temp = [sbox[b] for b in temp]
        w.append([w[i - nk][j] ^ temp[j] for j in range(4)])
    return w, nr


def _aes_encrypt_any(key, block):
    w, nr = _expand_key_any(key)
    sbox = _sbox()
    s = [list(block[i:i + 4]) for i in range(0, 16, 4)]
    # state as columns: s[col][row]
    def add_rk(rnd):
        for c in range(4):
            for r in range(4):
                s[c][r] ^= w[rnd * 4 + c][r]
    # load: FIPS state[row][col] = in[row + 4col]
    st = [[block[r + 4 * c] for c in range(4)] for r in range(4)]
    for c in range(4):
        for r in range(4):
            s[c][r] = st[r][c]
    add_rk(0)
    for rnd in range(1, nr + 1):
        for c in range(4):
            for r in range(4):
                s[c][r] = sbox[s[c][r]]
        # ShiftRows: state row r shifted left by r  => s[c][r] = s[(c+r)%4][r]
        ns = [[0] * 4 for _ in range(4)]
        for c in range(4):
            for r in range(4):
                ns[c][r] = s[(c + r) % 4][r]
        s = ns
        if rnd != nr:
            for c in range(4):
                col = [s[c][r] for r in range(4)]
                t = col[0] ^ col[1] ^ col[2] ^ col[3]
                for r in range(4):
                    s[c][r] = col[r] ^ t ^ _xtime(col[r] ^ col[(r + 1) % 4])
        add_rk(rnd)
    out = bytearray(16)
    for c in range(4):
        for r in range(4):
            out[r + 4 * c] = s[c][r]
    return bytes(out)


def _ghash_mul(x, y):
    """GF(2^128) multiply, bit-reflected per the GCM spec."""
    R = 0xE1000000000000000000000000000000
    z = 0
    v = y
    for i in range(127, -1, -1):
        if (x >> i) & 1:
            z ^= v
        if v & 1:
            v = (v >> 1) ^ R
        else:
            v >>= 1
    return z


def _int_be(b):
    return int.from_bytes(b, "big")


def _ghash(h, data):
    y = 0
    for i in range(0, len(data), 16):
        block = data[i:i + 16].ljust(16, b"\0")
        y = _ghash_mul(y ^ _int_be(block), h)
    return y


def gcm_crypt(key, iv12, aad, data, tag_len, encrypting=True):
    """CTR32BE-from-inc32(J0) core + GHASH tag. GHASH always runs over the
    CIPHERTEXT: `out` when encrypting, `data` when decrypting.
    Returns (output_bytes, tag)."""
    h = _aes_encrypt_any(key, b"\0" * 16)
    j0 = iv12 + b"\0\0\0\1"
    ctr = _int_be(j0)
    out = bytearray()
    for i in range(0, len(data), 16):
        ctr = ((ctr & ~0xFFFFFFFF) | ((ctr + 1) & 0xFFFFFFFF))
        ks = _aes_encrypt_any(key, ctr.to_bytes(16, "big"))
        chunk = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
    ct = bytes(out) if encrypting else bytes(data)
    lenblk = (len(aad) * 8).to_bytes(8, "big") + (len(ct) * 8).to_bytes(8, "big")
    g = _ghash(_int_be(h), aad.ljust((len(aad) + 15) // 16 * 16, b"\0")
               + ct.ljust((len(ct) + 15) // 16 * 16, b"\0") + lenblk)
    mask = _int_be(_aes_encrypt_any(key, j0))
    tag = (g ^ mask).to_bytes(16, "big")
    return bytes(out), tag[:tag_len]


def kdf256(master_key32, master_salt, label, out_len):
    """derive_aes_cm_key::<N> (nvst.rs:2873-2886) with the AES-256 PRF."""
    iv = bytearray(16)
    iv[0:len(master_salt)] = master_salt
    iv[7] ^= label
    out = b""
    ctr = _int_be(bytes(iv))
    while len(out) < out_len:
        out += _aes_encrypt_any(master_key32, ctr.to_bytes(16, "big"))
        ctr += 1
    return out[:out_len]


# ------------------------------------------------------- 8. the pipeline

class Receiver(object):
    """NvstVideoReceiver pure-media-path port (nvst.rs:4414-4961).
    Feeds on already-authenticated packets: process_packet(index, bytes,
    now). SRTP itself is exercised separately (kn_srtp / vectors)."""

    def __init__(self, codec=gsr.H264, window=DEFAULT_REORDER_WINDOW,
                 au_cap=DEFAULT_AU_CAP, vps=DEFAULT_VIDEO_PACKET_SIZE,
                 timeout_ms=DEFAULT_TIMEOUT_MS, startup_ms=None,
                 stall_ms=None, grace_ms=None,
                 expected_pt=None, expected_ssrc=None,
                 fec_max_blocks=MAX_PENDING_FEC_BLOCKS,
                 fec_max_shards=KN_FEC_MAX_SHARDS_PY,
                 nack_capacity=NACK_MAX_PENDING, origin_ms=0):
        self.cfg = {
            "codec": codec, "window": window, "au_cap": au_cap, "vps": vps,
            "timeout_ms": timeout_ms,
            "startup_ms": startup_ms if startup_ms is not None else timeout_ms,
            "stall_ms": stall_ms if stall_ms is not None else timeout_ms,
            "grace_ms": grace_ms if grace_ms is not None else timeout_ms,
            "expected_pt": expected_pt, "expected_ssrc": expected_ssrc,
        }
        self.reorder = RtpReorderBuffer(window)
        self.fec = FecReorderBuffer(vps, fec_max_blocks, fec_max_shards)
        self.assembler = Assembler(codec, au_cap)
        self.inval = Invalidation()
        self.nack = NackTracker(nack_capacity)
        self.state = ST_RUNNING
        self.bound_ssrc = None
        self.highest_seq = 0
        self.last_spi = None
        self.next_frame_contiguous = True
        self.authenticated = 0
        self.fec_packets = 0
        self.fec_repaired = 0
        self.frames_emitted = 0
        self.replay_rejections = 0
        self.stale_packets = 0
        self.retransmissions = 0
        self.gap_recoveries = 0
        self.withheld = 0
        self.keyframe_needed = False
        self.keyframe_requests = 0
        self.timeout_origin = origin_ms
        self.last_authenticated = None
        self.initial_timeout_pending = True
        # watchdog
        self.wp_first_auth = None
        self.wp_first_kf = False
        self.wp_last_frame = None
        self.wp_last_assembled = None
        self.wp_stage = 0
        self.wp_kf_requested_at = None

    # -- helpers
    def request_keyframe(self):
        if not self.keyframe_needed:
            self.keyframe_requests += 1
        self.keyframe_needed = True

    def take_keyframe_request(self):
        had = self.keyframe_needed
        self.keyframe_needed = False
        return had

    def invalidate_picture(self, now):
        # nvst.rs:4867-4874 — no direct event; the caller emits the trigger's
        # event (drop/recovery) around this.
        self.assembler.reset()
        self.last_spi = None
        self.next_frame_contiguous = False
        if not self.inval.lose(now):
            self.request_keyframe()

    def reset_media_state(self):
        self.reorder.reset()
        self.fec.reset()
        self.assembler.reset()
        self.last_spi = None
        self.next_frame_contiguous = False   # FALSE, not true (4946-4961)
        self.wp_first_auth = None
        self.wp_first_kf = False
        self.wp_last_frame = None
        self.wp_last_assembled = None
        self.wp_stage = 0
        self.wp_kf_requested_at = None
        self.inval.reset()

    # -- main entry
    def process_packet(self, index, plaintext, now, events):
        if self.state != ST_RUNNING:
            reason = {ST_PAUSED: D_PAUSED, ST_STOPPED: D_STOPPED,
                      ST_RECOVERY: D_RECOVERY_REQUIRED}[self.state]
            events.append({"t": EV_DROPPED, "r": reason, "i": index})
            return
        rc, hdr = rtp_parse_hdr(plaintext)
        if rc != GS_OK:
            events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP, "i": index,
                           "d": rc})
            return
        rc, poff, plen = rtp_payload(hdr, plaintext)
        if rc != GS_OK:
            events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP, "i": index,
                           "d": rc})
            return
        self._process_authenticated(index, hdr, poff, plen, plaintext, now,
                                    events)

    def _process_authenticated(self, index, hdr, poff, plen, plaintext, now,
                               events):
        cfg = self.cfg
        if cfg["expected_pt"] is not None and hdr["pt"] != cfg["expected_pt"]:
            events.append({"t": EV_DROPPED, "r": D_UNEXPECTED_PT, "i": index})
            return
        expected = cfg["expected_ssrc"] if cfg["expected_ssrc"] is not None \
            else self.bound_ssrc
        if expected is not None and hdr["ssrc"] != expected:
            events.append({"t": EV_DROPPED, "r": D_UNEXPECTED_SSRC,
                           "i": index})
            return
        if self.bound_ssrc is None:
            self.bound_ssrc = hdr["ssrc"]
        self.last_authenticated = now
        if self.initial_timeout_pending:
            self.initial_timeout_pending = False
            if self.wp_first_auth is None:
                self.wp_first_auth = now
        kf = []
        self.inval.poll_backstop(now, kf)
        if kf:
            self.request_keyframe()
        self.authenticated += 1
        seq32 = index & 0xFFFFFFFF
        if seq32 > self.highest_seq:
            self.highest_seq = seq32
        if self.nack.resolve(index):
            self.retransmissions += 1

        pkt = Packet(index, plaintext, hdr, origin=0)
        layout = None
        if hdr["has_gs"]:
            lrc, layout = gsr.fec_layout(plaintext[hdr["gs_off"]:
                                                   hdr["gs_off"] + 16])
            if lrc != GS_OK:
                layout = None
        if layout is not None:
            if layout["shard"] >= layout["data"]:
                self.fec_packets += 1
            result = self.fec.push(pkt, layout, now)
            if layout["shard"] < layout["data"] and result.dropped is not None \
                    and result.dropped[0] == D_UNSUPPORTED_FEC:
                self.invalidate_picture(now)
        else:
            result = self.reorder.push(pkt, now)
        self.consume(result, now, events)

    def consume(self, result, now, events):
        self.fec_repaired += result.fec_repaired
        if result.nack is not None:
            kf = []
            self.nack.request(result.nack[0], result.nack[1], now, kf)
            if kf:
                self.request_keyframe()
        if result.dropped is not None:
            reason, index = result.dropped
            if reason == D_STALE:
                self.stale_packets += 1
            events.append({"t": EV_DROPPED, "r": reason, "i": index})
        if result.recovery is not None:
            self.gap_recoveries += 1
            self.invalidate_picture(now)
            self.nack.clear()
            events.append({"t": EV_RECOVERY_GAP, "a": result.recovery[0],
                           "b": result.recovery[1]})
        for pkt in result.ready:
            self._consume_packet(pkt, now, events)

    def _consume_packet(self, pkt, now, events):
        data = pkt.data
        if pkt.hdr is None:
            self.invalidate_picture(now)
            events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP,
                           "i": pkt.index})
            return
        rc, poff, plen = rtp_payload(pkt.hdr, data)
        if rc != GS_OK:
            self.invalidate_picture(now)
            events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP,
                           "i": pkt.index, "d": rc})
            return
        if not pkt.hdr["has_gs"]:
            self.invalidate_picture(now)
            events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP,
                           "i": pkt.index, "d": gsr.E_MISSING_NV_VIDEO})
            return
        video = gsr.video_parse(data[pkt.hdr["gs_off"]:pkt.hdr["gs_off"] + 16])
        if pkt.origin == 1:
            video["is_fec"] = 0            # pitfall #4 (4786-4788)
        if video["is_fec"]:
            self.fec_packets += 1
            events.append({"t": EV_DROPPED, "r": D_UNSUPPORTED_FEC,
                           "i": pkt.index})
            return
        if video["sof"] and self.assembler.current_frame is not None \
                and self.assembler.current_frame != video["frame_index"]:
            self.invalidate_picture(now)
            self.nack.clear()
            events.append({"t": EV_DROPPED, "r": D_FRAME_DISCONTINUITY,
                           "i": pkt.index})
        same_frame = (not video["sof"]) \
            and self.assembler.current_frame == video["frame_index"]
        if same_frame and self.last_spi is not None \
                and not gsr.index_continuous(self.last_spi, video["spi"]):
            self.invalidate_picture(now)
            events.append({"t": EV_DROPPED, "r": D_FRAME_DISCONTINUITY,
                           "i": pkt.index})
            return
        self.last_spi = video["spi"]
        media = data[poff:poff + plen]
        try:
            frame = self.assembler.push(video, pkt.hdr["ts"], media)
        except Drop as drop:
            self.invalidate_picture(now)
            events.append({"t": EV_DROPPED, "r": drop.reason, "i": pkt.index,
                           "d": drop.detail})
            return
        if frame is None:
            return
        loss_before = not self.next_frame_contiguous
        self.next_frame_contiguous = True
        kf = []
        admitted = self.inval.admit(frame, loss_before, now, kf)
        if kf:
            self.request_keyframe()
        if not admitted:
            self.withheld += 1
            return
        self.frames_emitted += 1
        self.wp_last_frame = frame["frame_index"]
        self.wp_last_assembled = now
        if self.wp_first_kf or frame["keyframe"]:
            self.wp_first_kf = True
            closed = self.wp_stage != 0
            self.wp_stage = 0
            self.wp_kf_requested_at = None
            if closed:
                events.append({"t": EV_PROGRESS_RESUMED})
        if frame["keyframe"] and self.keyframe_needed:
            self.keyframe_needed = False
        digest = hashlib.sha1(frame["bytes"]).digest()
        events.append({
            "t": -1,                       # frame pseudo-event (ordered)
            "frame_index": frame["frame_index"],
            "len": len(frame["bytes"]),
            "hash": int.from_bytes(digest[:8], "big"),
            "keyframe": 1 if frame["keyframe"] else 0,
            "contiguous": 1 if frame["contiguous"] else 0,
            "wire": frame["frame_type"] if frame["frame_type"] is not None
            else 0,
            "known": 1 if frame["frame_type_parsed"] else 0,
            "first_spi": frame["first_spi"] if frame["first_spi"] is not None
            else 0,
            "ts": frame["timestamp"],
        })

    # -- polling
    def poll(self, now, events):
        if self.state != ST_RUNNING:
            return
        kf = []
        self.inval.poll_backstop(now, kf)
        if kf:
            self.request_keyframe()
        self.consume(self.reorder.poll_deadline(now), now, events)
        self.consume(self.fec.poll_deadline(now), now, events)
        last = self.last_authenticated if self.last_authenticated is not None \
            else self.timeout_origin
        if last is not None:
            idle = sat_sub(now, last)
            tmo = self.cfg["startup_ms"] if self.initial_timeout_pending \
                else self.cfg["timeout_ms"]
            if idle >= tmo:
                self.reset_media_state()
                self.state = ST_RECOVERY
                events.append({"t": EV_RECOVERY_TIMEOUT, "a": idle})
                return
        if not self.initial_timeout_pending:
            self._poll_progress(now, events)

    def _poll_progress(self, now, events):
        if not self.wp_first_kf:
            base = self.wp_first_auth
        else:
            cands = [t for t in (self.wp_last_assembled, self.wp_first_auth)
                     if t is not None]
            base = max(cands) if cands else None
        if base is None:
            return
        idle = sat_sub(now, base)
        if self.wp_stage == 0:
            if idle >= self.cfg["stall_ms"]:
                self.wp_stage = 1
                self.wp_kf_requested_at = now
                self.request_keyframe()
                events.append({"t": EV_PROGRESS_STALL, "a": idle})
        elif self.wp_stage == 1:
            grace_started = self.wp_kf_requested_at \
                if self.wp_kf_requested_at is not None else now
            if sat_sub(now, grace_started) >= self.cfg["grace_ms"]:
                self.wp_stage = 2
                self.state = ST_RECOVERY
                events.append({"t": EV_PROGRESS_RECOVERY, "a": idle})

    # -- lifecycle
    def pause(self, now, events):
        if self.state != ST_RUNNING:
            return False
        self.reset_media_state()
        self.state = ST_PAUSED
        events.append({"t": EV_LIFECYCLE, "s": self.state})
        return True

    def resume(self, now, events):
        if self.state != ST_PAUSED:
            return False
        self.reset_media_state()
        self.last_authenticated = None
        self.timeout_origin = now
        self.initial_timeout_pending = False
        self.state = ST_RUNNING
        events.append({"t": EV_LIFECYCLE, "s": self.state})
        return True

    def recover(self, now, events):
        if self.state != ST_RECOVERY:
            return False
        self.reset_media_state()
        self.last_authenticated = None
        self.timeout_origin = now
        self.initial_timeout_pending = False
        self.state = ST_RUNNING
        events.append({"t": EV_LIFECYCLE, "s": self.state})
        return True

    def stop(self, now, events):
        if self.state == ST_STOPPED:
            return False
        self.reset_media_state()
        self.state = ST_STOPPED
        events.append({"t": EV_LIFECYCLE, "s": self.state})
        return True


# ================================================================ self-proof

_CHECKS = 0
_FAILS = 0


def _check(cond, label):
    global _CHECKS, _FAILS
    _CHECKS += 1
    if not cond:
        _FAILS += 1
        print("SELF-CHECK FAIL: %s" % label)


def _hx(b):
    return bytes(b).hex()


def _rust_test_packet(seq, flags, frame_index, cur, last, media,
                      fec_word_bytes=None):
    """Byte-exact build_plaintext_rtp_with_fec_blocks (nvst.rs:10506-10531)."""
    pkt = bytearray([0x90, 0xE0])
    pkt += (seq & 0xFFFF).to_bytes(2, "big")
    pkt += (0x01020304).to_bytes(4, "big")
    pkt += (0x11223344).to_bytes(4, "big")
    pkt += (0x4753).to_bytes(2, "big") + (4).to_bytes(2, "big")
    pkt += ((seq << 8) & 0xFFFFFFFF).to_bytes(4, "little")
    pkt += (frame_index & 0xFFFFFFFF).to_bytes(4, "little")
    pkt += bytes([flags & 0xFF, 0, 0,
                  ((cur & 3) << 4) | ((last & 3) << 6)])
    pkt += fec_word_bytes if fec_word_bytes else bytes(4)
    pkt += media
    return bytes(pkt)


def sc_builder_identity():
    """My generic builder must reproduce the Rust test builder byte-exactly."""
    expect = bytes.fromhex(
        "90e0" "0001" "01020304" "11223344" "4753" "0004"
        "00010000" "2a000000" "07000000" "00000000" "00000165aa")
    rust = _rust_test_packet(1, 7, 42, 0, 0, b"\x00\x00\x01\x65\xaa")
    _check(rust == expect, "rust-builder transcription golden")
    mine = build_pkt(1, 0x01020304, 0x11223344, 7, 1, 42, 0, 0, 0, 0, 0,
                     b"\x00\x00\x01\x65\xaa", marker=1)
    _check(mine == rust, "build_pkt == build_plaintext_rtp_with_fec_blocks")
    # a multi-byte-field discriminator: BE frame_index reading must differ
    ext = gsr.build_gs_ext(0x0102, 0xAABBCCDD, 5, 1, 2, 3, 4, 5)
    _check(gsr.le32(ext[4:8]) == 0xAABBCCDD and
           gsr.be32(ext[4:8]) != 0xAABBCCDD, "GS ext stays little-endian")


def sc_encoders():
    """Goldens transcribed from nvst_control.rs tests (216-260) and
    nvst.rs RTCP tests (13420-13431)."""
    _check(_hx(nack_v2(0, [0x1234])) ==
           "17030d0002000134120000000000000000", "nack_v2 single")
    _check(_hx(nack_v2(7, [0x1234, 0x1235, 0x1274, 0x1275])) ==
           "170317000207023412010000000000008075120000000000000000",
           "nack_v2 two records")
    _check(_hx(nack_v2(0, [65534, 65535, 0, 62, 63])) ==
           "17031700020002feff03000000000000803f000000000000000000",
           "nack_v2 wrap grouping")
    _check(_hx(nack_v2(0, list(range(64)))) ==
           "17030d000200010000ffffffffffffff7f", "nack_v2 contiguous 64")
    sparse = [(i * 65) & 0xFFFF for i in range(64)]
    cmd = nack_v2(0, sparse)
    payload = cmd[4:]
    _check(len(payload) == 643, "nack_v2 sparse payload len")
    _check(list(cmd[:7]) == [0x17, 3, 0x83, 2, 2, 0, 64], "nack_v2 sparse hdr")
    ok = True
    for rec, seq in zip([payload[3:][i:i + 10] for i in range(0, 640, 10)],
                        sparse):
        if rec[:2] != (seq & 0xFFFF).to_bytes(2, "little") or rec[2:] != b"\0" * 8:
            ok = False
    _check(ok, "nack_v2 sparse records")
    _check(nack_v2(0, []) is None, "nack_v2 empty -> None")
    _check(nack_v2(0, [0] * 65) is None, "nack_v2 65 -> None")
    # round-trip property (nvst_control.rs:262-280 grid)
    rt_ok = True
    for base in (0, 1, 32767, 65534, 65535):
        for step in (0, 1, 2, 64, 65, 127, 4095, 65535):
            for count in range(1, 65):
                missing = [(base + i * step) & 0xFFFF for i in range(count)]
                cmd = nack_v2(3, missing)
                payload = cmd[4:]
                if len(payload) != 3 + payload[2] * 10:
                    rt_ok = False
                    break
                decoded = set()
                for rec in [payload[3:][i:i + 10]
                            for i in range(0, len(payload) - 3, 10)]:
                    b = int.from_bytes(rec[:2], "little")
                    bm = int.from_bytes(rec[2:], "little")
                    decoded.add(b)
                    for j in range(64):
                        if bm >> j & 1:
                            decoded.add((b + j + 1) & 0xFFFF)
                if decoded != set(missing):
                    rt_ok = False
                    break
    _check(rt_ok, "nack_v2 round-trip grid")

    _check(list(rtcp_nack(0x01020304, 0x05060708, 65534, 65536)) ==
           [0x81, 205, 0, 3, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0xFE, 0, 3],
           "rtcp nack wrapping golden")
    _check(list(rtcp_pli(0x01020304, 0x05060708)) ==
           [0x81, 206, 0, 2, 1, 2, 3, 4, 5, 6, 7, 8], "rtcp pli golden")
    _check(list(ctrl_encode(0x207, bytes([1, 2, 3]))) ==
           [0x07, 0x02, 0x03, 0x00, 1, 2, 3], "ctrl header golden")
    _check(_hx(idr_request()) == "0203020000 00".replace(" ", ""),
           "idr request golden")
    rfi = frame_invalidation_range(5, 9, 2)
    _check(_hx(rfi) == "0103180005000000000000000900000000000000"
           "0200000000000000", "rfi range golden")
    ack = frame_ack(7, 1234.5, 99)
    import struct
    exp = bytearray(102)
    exp[0:2] = (1).to_bytes(2, "little")
    exp[2:4] = (9).to_bytes(2, "little")
    exp[4:8] = (7).to_bytes(4, "little")
    exp[12:20] = struct.pack("<d", 1234.5)
    for off in range(20, 52, 4):
        exp[off:off + 4] = (0xBF800000).to_bytes(4, "little")
    exp[72:76] = (99).to_bytes(4, "little")
    exp[94:98] = (0xBF800000).to_bytes(4, "little")
    exp[98:102] = (0xBF800000).to_bytes(4, "little")
    _check(ack == ctrl_encode(0x0204, bytes(exp)), "frame_ack golden")
    _check(len(ack) == 106, "frame_ack length")
    pac = frame_pacing(12, 16666, 20000)
    p = bytearray(28)
    p[0:4] = (5).to_bytes(4, "little")
    p[8:12] = (2).to_bytes(4, "little")
    p[12:16] = (12).to_bytes(4, "little")
    p[16:20] = (16666).to_bytes(4, "little")
    p[20:24] = (16666).to_bytes(4, "little")   # min(error, target)
    p[24:28] = (0x341A).to_bytes(4, "little")
    _check(pac == ctrl_encode(0x0203, bytes(p)), "frame_pacing golden")
    qos = qos_report(3, 77, 0, 0, 250, 15, 42, 0x11223344, 2, 4500, 900)
    q = bytearray(52)
    q[0:4] = (7).to_bytes(4, "little")
    q[8:12] = (3).to_bytes(4, "little")
    q[12:16] = (77).to_bytes(4, "little")
    q[24:26] = (250).to_bytes(2, "little")
    q[26:28] = (15).to_bytes(2, "little")
    q[28] = 42
    q[30:32] = (1000).to_bytes(2, "little")
    q[32:34] = (1000).to_bytes(2, "little")
    q[36:40] = (0x11223344).to_bytes(4, "little")
    q[40:44] = (2).to_bytes(4, "little")
    q[44:48] = (4500).to_bytes(4, "little")
    q[48:52] = (900).to_bytes(4, "little")
    _check(qos == ctrl_encode(0x0207, bytes(q)), "qos golden")


def sc_crypto():
    """AES-256 / GCM / KDF oracles against published constants."""
    key256 = bytes(range(32))
    pt = bytes.fromhex("00112233445566778899aabbccddeeff")
    ct = _aes_encrypt_any(key256, pt)
    _check(ct.hex() == "8ea2b7ca516745bfeafc49904b496089",
           "AES-256 ECB FIPS-197 C.3")
    zero16 = b"\0" * 16
    h = _aes_encrypt_any(zero16, zero16)
    _check(h.hex() == "66e94bd4ef8a2c3b884cfa59ca342b2e",
           "GCM H = E(K0, 0^128)")
    out, tag = gcm_crypt(zero16, b"\0" * 12, b"", b"", 16)
    _check(out == b"" and tag.hex() == "58e2fccefa7e3061367f1d57a4e7455a",
           "GCM spec case 1 (empty)")
    out, tag = gcm_crypt(zero16, b"\0" * 12, b"", zero16, 16)
    _check(out.hex() == "0388dace60b6a392f328c2b971b2fe78" and
           tag.hex() == "ab6e47d42cec13bdf53a67b21257bddf",
           "GCM spec case 2 (one zero block)")
    out2, tag2 = gcm_crypt(zero16, b"\0" * 12, b"", bytes(out), 16,
                           encrypting=False)
    _check(out2 == zero16 and tag2 == tag, "GCM decrypt round-trip")
    # 8-byte truncated tag path (the active Mjolnir policy)
    _, tag8 = gcm_crypt(bytes(range(32)), bytes(range(12)), b"aad",
                        b"payload-bytes", 8)
    dec, vtag = gcm_crypt(bytes(range(32)), bytes(range(12)), b"aad",
                          bytes(bytearray(b"payload-bytes")), 8)
    _check(vtag == tag8, "GCM-256 tag8 deterministic")
    # AES-128 path cross-check against srtp_reference's proved implementation
    import srtp_reference as sr
    same = True
    x = 12345
    for _ in range(20):
        x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF; x &= 0xFFFFFFFF
        k = x.to_bytes(16, "little")
        b = (x * 7919).to_bytes(16, "little")
        if _aes_encrypt_any(k, b) != sr.aes128_encrypt(k, b):
            same = False
    _check(same, "AES-128 any-expansion == srtp_reference")
    # KDF256 structure: label placement + counter carry
    mk = bytes(range(32))
    ms = bytes(range(12))
    k0 = kdf256(mk, ms, 0x00, 32)
    k1 = kdf256(mk, ms, 0x02, 12)
    _check(len(k0) == 32 and len(k1) == 12 and k0[:12] != k1,
           "kdf256 labels differ")
    long_out = kdf256(mk, ms, 0x00, 40)   # > 2 blocks: counter carry
    _check(long_out[:32] == k0, "kdf256 counter chaining")


def sc_reorder_deadline():
    """nvst.rs:13341-13404 reorder_gap_uses_mjolnir_dequeue_deadline..."""
    def pkt(index):
        data = bytearray(12)
        data[0] = 0x80
        data[1] = 96
        data[2:4] = (index & 0xFFFF).to_bytes(2, "big")
        data[4:8] = (90000).to_bytes(4, "big")
        data[8:12] = (1).to_bytes(4, "big")
        return Packet(index, bytes(data))

    started = 0
    ro = RtpReorderBuffer(DEFAULT_REORDER_WINDOW)
    r = ro.push(pkt(1), started)
    _check([p.index for p in r.ready] == [1] and r.nack is None
           and r.recovery is None, "deadline: first ready")
    gap_started = started + 1
    r = ro.push(pkt(3), gap_started)
    _check(r.nack == (2, 2) and not r.ready and r.recovery is None,
           "deadline: gap nack")
    r = ro.push(pkt(4), gap_started + REORDER_TIMEOUT_MS - 1)
    _check(r.nack == (2, 2) and not r.ready and r.recovery is None,
           "deadline: still waiting at 51ms")
    r = ro.push(pkt(5), gap_started + REORDER_TIMEOUT_MS)
    _check(r.recovery == (2, 2), "deadline: recovery at 52ms")
    _check([p.index for p in r.ready] == [3, 4, 5], "deadline: ready 3,4,5")
    _check(r.nack is None, "deadline: nack cleared on recovery")
    # stale + duplicate
    r = ro.push(pkt(3), gap_started + 60)
    _check(r.dropped == (D_STALE, 3) and not r.ready, "stale drop")
    ro2 = RtpReorderBuffer(8)
    ro2.push(pkt(10), 0)
    r = ro2.push(pkt(10), 1)
    # packet 10 was DELIVERED on the first push (drain pops it) — the twin
    # is therefore STALE, not DUPLICATE (duplicate needs it still buffered)
    _check(r.dropped == (D_STALE, 10), "delivered twin is stale")
    ro2b = RtpReorderBuffer(8)
    ro2b.push(pkt(10), 0)
    ro2b.push(pkt(12), 1)          # 12 stays buffered behind the gap at 11
    r = ro2b.push(pkt(12), 2)
    _check(r.dropped == (D_DUPLICATE, 12), "duplicate drop (still buffered)")
    # empty-buffer huge jump resync (nvst.rs:3593-3601)
    ro3 = RtpReorderBuffer(4)
    ro3.push(pkt(1), 0)
    r = ro3.push(pkt(9), 1)          # 9 - 2 = 7 >= 4 -> immediate recovery
    _check(r.recovery == (2, 8), "huge jump recovery range")
    _check([p.index for p in r.ready] == [9], "huge jump delivers the packet")


def sc_fec_unit():
    """nvst.rs:13040-13327 FEC block/reorder tests (byte-exact)."""
    # --- parity-only recovery (13188-13240)
    base = 17392
    media = bytes([0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21])
    data = bytearray(_rust_test_packet(base, 7, 1466, 0, 0, media))
    ext_start = 32 - 16
    fec_word = (len(data) | (1 << 22) | (2 << 26)) & 0xFFFFFFFF
    data[ext_start + 12:ext_start + 16] = fec_word.to_bytes(4, "little")
    data = bytes(data)
    shard_len = len(data)
    parities = []
    for p in range(2):
        c = fecr.cauchy(0, p, 2)
        shard = bytearray(shard_len)
        fecr.axpy(shard, data, c)
        shard[0:ext_start] = data[0:ext_start]
        shard[2:4] = ((base + 1 + p) & 0xFFFF).to_bytes(2, "big")
        parities.append(bytes(shard))
    layout = {"frame_index": 1466, "block": 0, "last_block": 0,
              "shard": 1, "data": 1, "parity": 2}
    blk = FecBlock(dict(layout, shard=0), base)
    _check(blk.insert(layout, Packet(base + 1, parities[0])) is None,
           "parity-only insert")
    _check(blk.has_enough_shards(), "parity-only has_enough")
    ready, repaired = blk.finish(shard_len)
    _check(repaired == 1 and len(ready) == 1, "parity-only repaired=1")
    _check(ready[0].index == base, "parity-only index")
    rc, hdr = rtp_parse_hdr(ready[0].data)
    _check(rc == GS_OK and hdr["seq"] == base & 0xFFFF, "parity-only seq")
    _check(ready[0].data[ext_start:] == data[ext_start:],
           "parity-only ext+media exact")

    # --- 3+1 with corrupted fec_info on parity (13040-13186)
    base2 = 900
    datas = []
    for i in range(3):
        pkt = bytearray(_rust_test_packet(
            base2 + i, 7, 77, 1, 2,
            bytes([0, 0, 1, 0x65, (0x10 + i)] * 3)))
        w = (len(pkt) | (3 << 22)).to_bytes(4, "little")  # pct bits: see below
        # replicate the Rust test's fec_word: length | data<<22 (pct lands in
        # bits 4..12 of the length field — the test never parses it)
        pkt[ext_start + 12:ext_start + 16] = w
        datas.append(bytes(pkt))
    middle = datas[1]
    slen = len(middle)
    par = bytearray(slen)
    for i in range(3):
        fecr.axpy(par, datas[i].ljust(slen, b"\0"), fecr.cauchy(i, 0, 1))
    par[0:16] = datas[0][0:16]
    par[2:4] = ((base2 + 3) & 0xFFFF).to_bytes(2, "big")
    par[28:32] = b"\xa5" * 4                 # corrupted fecInfo (pitfall #4)
    lay = lambda s: {"frame_index": 77, "block": 1, "last_block": 2,
                     "shard": s, "data": 3, "parity": 1}
    blk = FecBlock(lay(0), base2)
    _check(blk.insert(lay(0), Packet(base2, datas[0])) is None, "3+1 ins0")
    _check(blk.insert(lay(2), Packet(base2 + 2, datas[2])) is None, "3+1 ins2")
    _check(blk.insert(lay(3), Packet(base2 + 3, bytes(par))) is None,
           "3+1 ins-parity")
    _check(blk.has_enough_shards(), "3+1 has_enough")
    ready, repaired = blk.finish(slen)
    _check(repaired == 1 and len(ready) == 3, "3+1 repaired=1, ready=3")
    rec = ready[1]
    _check(rec.index == base2 + 1, "3+1 recovered index")
    _check(rec.data[:28] == middle[:28], "3+1 envelope+ext[0..12] restored")
    _check(rec.data[28:32] != middle[28:32], "3+1 fec_word NOT restored")
    _check(rec.data[32:] == middle[32:], "3+1 media exact")

    # --- retain-following-blocks (13275-13327)
    def plain(index):
        d = bytearray(12)
        d[0] = 0x80
        d[1] = 96
        d[2:4] = (index & 0xFFFF).to_bytes(2, "big")
        d[4:8] = (90000).to_bytes(4, "big")
        d[8:12] = (1).to_bytes(4, "big")
        return Packet(index, bytes(d))

    def lay2(bi, shard):
        return {"frame_index": 7, "block": bi, "last_block": 1,
                "shard": shard, "data": 2, "parity": 1}

    started = 0
    fo = FecReorderBuffer(48)
    first = plain(100)
    first_slot = first.slot
    r = fo.push(first, lay2(0, 0), started)
    _check(not r.ready and r.nack is None, "retain: first push quiet")
    r = fo.push(plain(103), lay2(1, 0), started + 1)
    _check(r.nack == (101, 101) and not r.ready and r.recovery is None,
           "retain: successor nack 101")
    r = fo.push(plain(101), lay2(0, 1), started + 20)
    _check([p.index for p in r.ready] == [100, 101], "retain: ready 100,101")
    _check(r.recovery is None, "retain: no recovery")
    _check(r.ready[0].slot == first_slot, "retain: original allocation kept")
    _check(len(fo.blocks) == 1, "retain: successor stays buffered")
    r = fo.push(plain(102), lay2(0, 2), started + 30)   # late parity
    _check(not r.ready and r.recovery is None, "retain: late parity ignored")


def sc_pipeline_gaps():
    """nvst.rs:13406-13450 gaps_require_recovery_and_never_emit_an_incomplete_frame."""
    cfg = dict(codec=gsr.H264, window=4, au_cap=4096,
               vps=DEFAULT_VIDEO_PACKET_SIZE, timeout_ms=500,
               expected_pt=96, expected_ssrc=0x11223344, origin_ms=0)
    rx = Receiver(**cfg)
    first = _rust_test_packet(1, FLAG_SOF | FLAG_PIC, 1, 0, 0,
                              bytes([0, 0, 1, 0x65]))
    far = _rust_test_packet(6, FLAG_EOF | FLAG_PIC, 1, 0, 0, bytes([0xAA]))
    ev = []
    rx.process_packet(1, first, 0, ev)
    rx.process_packet(6, far, 0, ev)
    kinds = [e["t"] for e in ev]
    _check(EV_RECOVERY_GAP in kinds, "gaps: recovery emitted")
    _check(all(e["t"] != -1 for e in ev), "gaps: no frame emitted")
    fresh = _rust_test_packet(7, FLAG_SOF | FLAG_EOF | FLAG_PIC, 2, 0, 0,
                              bytes([0, 0, 1, 0x65, 0xBB]))
    ev2 = []
    rx.process_packet(7, fresh, 0, ev2)
    frames = [e for e in ev2 if e["t"] == -1]
    _check(len(frames) == 1, "gaps: fresh frame after recovery")
    _check(frames and frames[0]["contiguous"] == 0, "gaps: contiguous=false")
    _check(frames and frames[0]["keyframe"] == 1, "gaps: keyframe detected")


def sc_nack_exclusion():
    """nvst.rs:13329-13340 reorder_nack_excludes_packets_already_buffered."""
    cfg = dict(codec=gsr.H264, window=4, au_cap=4096,
               vps=DEFAULT_VIDEO_PACKET_SIZE, timeout_ms=500,
               expected_pt=96, expected_ssrc=0x11223344, origin_ms=0)
    rx = Receiver(**cfg)
    ev = []
    for seq in (1, 3, 4):
        pkt = _rust_test_packet(seq, FLAG_SOF | FLAG_EOF | FLAG_PIC, seq,
                                0, 0, bytes([0, 0, 1, 0x65]))
        rx.process_packet(seq, pkt, 0, ev)
    got = rx.nack.take(0, None)
    _check(got == (2, 2), "nack exclusion: take (2,2)")
    got2 = rx.nack.take(0, None)
    _check(got2 is None, "nack exclusion: second take None")


def sc_inval_units():
    """Hand-computed FrameInvalidation traces (nvst_invalidation.rs)."""
    v = Invalidation()
    kf = []
    _check(v.lose(10) is False, "inval: unknown types -> no ownership")
    fr = {"frame_index": 1, "frame_type": FT_P, "frame_type_parsed": True,
          "keyframe": 0}
    _check(v.admit(fr, False, 10, kf) is True and fr["contiguous"],
           "inval: first P admitted contiguous")
    _check(v.lose(20) is True and v.active_since == 20,
           "inval: lose takes ownership once types known")
    fr3 = {"frame_index": 3, "frame_type": FT_P, "frame_type_parsed": True,
           "keyframe": 0}
    _check(v.admit(fr3, True, 21, kf) is False, "inval: P withheld while active")
    _check(v.pending == (2, 2) and v.unsent == (2, 2),
           "inval: RFI range queued (2,2)")
    _check(v.cnt_dropped == 1, "inval: dropped counter")
    # retry re-queue after 100 ms (last_retry = 21 from the invalidate)
    v.unsent = None
    v.poll_backstop(120, kf)
    _check(v.unsent is None, "inval: no retry before 100ms elapsed")
    v.poll_backstop(121, kf)
    _check(v.unsent == (2, 2), "inval: retry re-queues at 100ms")
    # backstop after 250 ms of continuous loss
    kf2 = []
    v.poll_backstop(270, kf2)
    _check(kf2 == [1] and v.backstop_requested and v.cnt_idr_backstops == 1,
           "inval: IDR backstop one-shot at 250ms")
    kf3 = []
    v.poll_backstop(600, kf3)
    _check(kf3 == [], "inval: backstop never re-fires")
    # I frame recovers
    fri = {"frame_index": 4, "frame_type": FT_I, "frame_type_parsed": True,
           "keyframe": 1}
    _check(v.admit(fri, True, 300, kf) is True, "inval: I admitted")
    _check(fri["contiguous"] == True and v.active_since is None       # noqa: E712
           and v.cnt_rec_i == 1, "inval: recovery cleared state")
    # Header frames pass through during invalidation, contiguous forced True
    v.lose(400)
    frh = {"frame_index": 5, "frame_type": FT_HEADER,
           "frame_type_parsed": True, "keyframe": 0}
    _check(v.admit(frh, True, 401, kf) is True and frh["contiguous"] is True,
           "inval: Header frame admitted contiguous")
    # unknown-type frame during invalidation -> keyframe + clear
    v.lose(500)
    frn = {"frame_index": 6, "frame_type": None, "frame_type_parsed": False,
           "keyframe": 0}
    kf4 = []
    _check(v.admit(frn, False, 501, kf4) is True and kf4 == [1]
           and frn["contiguous"] is False and v.active_since is None,
           "inval: typeless frame requests keyframe and clears")
    # take/restore/record_sent queue semantics
    v2 = Invalidation()
    v2.queue((7, 9))
    _check(v2.take() == (7, 9) and v2.in_flight, "queue: take")
    v2.restore((7, 9))
    _check(v2.unsent == (7, 9) and not v2.in_flight, "queue: restore")
    rng = v2.take()
    v2.record_sent(1000)
    _check(rng == (7, 9) and v2.rfi_last_sent == 1000 and v2.cnt_sent == 1,
           "queue: record_sent")


def sc_nack_tracker():
    t = NackTracker(capacity=4)
    kf = []
    t.request(10, 12, 0, kf)
    _check(sorted(t.pending) == [10, 11, 12], "tracker: request range")
    got = t.take(0, None)
    _check(got == (10, 12), "tracker: contiguous run taken")
    _check(t.take(0, None) is None, "tracker: retry interval gates")
    _check(t.take(4, None) == (10, 12), "tracker: retry after 4ms")
    _check(t.take(8, None) == (10, 12), "tracker: attempt 3")
    _check(t.take(12, None) is None, "tracker: attempts exhausted")
    _check(t.resolve(11) is True, "tracker: resolve sent -> True")
    _check(t.resolve(11) is False, "tracker: resolve twice -> False")
    t.clear()
    _check(not t.pending, "tracker: clear")
    # RTT-scaled attempts: rtt=30 -> clamp(52/30)=1 attempt, interval 34ms
    t.request(5, 5, 100, kf)
    _check(t.take(100, 30) == (5, 5), "tracker: rtt first send")
    _check(t.take(120, 30) is None, "tracker: rtt interval gates")
    _check(t.take(134, 30) is None, "tracker: rtt single attempt cap")
    # expiry at 52 ms
    t.purge(153)
    _check(not t.pending, "tracker: 52ms expiry")
    # overflow -> keyframe
    kf2 = []
    t.request(100, 109, 200, kf2)       # capacity 4
    _check(t.overflows == 1 and kf2 == [1] and len(t.pending) == 4,
           "tracker: overflow -> keyframe")
    # run truncation at 64 packets
    t2 = NackTracker(capacity=2048)
    t2.request(0, 99, 0, kf)
    got = t2.take(0, None)
    _check(got == (0, 63), "tracker: 64-packet batch cap")


def sc_timeout_watchdog():
    rx = Receiver(window=8, timeout_ms=500, startup_ms=500, stall_ms=100,
                  grace_ms=100, origin_ms=0)
    ev = []
    rx.poll(499, ev)
    _check(ev == [], "timeout: nothing before deadline")
    rx.poll(500, ev)
    _check([e["t"] for e in ev] == [EV_RECOVERY_TIMEOUT]
           and rx.state == ST_RECOVERY, "timeout: fires at deadline")
    ev2 = []
    pkt = _rust_test_packet(1, 7, 1, 0, 0, bytes([0, 0, 1, 0x65]))
    rx.process_packet(1, pkt, 501, ev2)
    _check([e["t"] for e in ev2] == [EV_DROPPED]
           and ev2[0]["r"] == D_RECOVERY_REQUIRED,
           "timeout: packets dropped in recovery state")
    ev3 = []
    _check(rx.recover(502, ev3) and ev3[0]["t"] == EV_LIFECYCLE
           and rx.state == ST_RUNNING, "timeout: recover lifecycle")
    # frame-progress watchdog: stall -> keyframe request -> recovery
    rx2 = Receiver(window=8, timeout_ms=5000, stall_ms=100, grace_ms=100,
                   origin_ms=0)
    ev4 = []
    kpkt = _rust_test_packet(1, 7, 1, 0, 0, bytes([0, 0, 1, 0x65]))
    rx2.process_packet(1, kpkt, 10, ev4)
    _check(any(e["t"] == -1 for e in ev4), "watchdog: keyframe frame in")
    rx2.poll(109, ev4)
    _check(all(e["t"] != EV_PROGRESS_STALL for e in ev4), "watchdog: no stall yet")
    ev5 = []
    rx2.poll(110, ev5)
    _check([e["t"] for e in ev5] == [EV_PROGRESS_STALL]
           and rx2.keyframe_needed, "watchdog: stall -> keyframe request")
    ev6 = []
    rx2.poll(209, ev6)
    _check(all(e["t"] != EV_PROGRESS_RECOVERY for e in ev6),
           "watchdog: grace not elapsed")
    ev7 = []
    rx2.poll(210, ev7)
    _check([e["t"] for e in ev7] == [EV_PROGRESS_RECOVERY]
           and rx2.state == ST_RECOVERY, "watchdog: grace -> recovery")
    # resumed event: new receiver, stall then a keyframe closes the episode
    rx3 = Receiver(window=8, timeout_ms=5000, stall_ms=50, grace_ms=50,
                   origin_ms=0)
    ev8 = []
    rx3.process_packet(1, kpkt, 0, ev8)
    rx3.poll(50, ev8)
    _check(any(e["t"] == EV_PROGRESS_STALL for e in ev8), "watchdog: stall 2")
    ev9 = []
    rx3.process_packet(2, _rust_test_packet(2, 7, 2, 0, 0,
                                            bytes([0, 0, 1, 0x65])), 60, ev9)
    _check(any(e["t"] == EV_PROGRESS_RESUMED for e in ev9),
           "watchdog: resumed on keyframe")


def self_check():
    sc_builder_identity()
    sc_encoders()
    sc_crypto()
    sc_reorder_deadline()
    sc_fec_unit()
    sc_pipeline_gaps()
    sc_nack_exclusion()
    sc_inval_units()
    sc_nack_tracker()
    sc_timeout_watchdog()
    print("self-check: %d checks, %d failures" % (_CHECKS, _FAILS))
    return _FAILS == 0


# ============================================================== scenarios

SSRC = 0x11223344
TS0 = 0x01020304


def parity_count_for(d, pct):
    return -(-(d * pct) // 100)          # div_ceil


class ScenarioBuilder(object):
    """Builds step lists; the runner expands them to packets and records
    per-step expectations. Steps are addressable by id (parity refs)."""

    def __init__(self, name, window=8, au_cap=4096,
                 vps=DEFAULT_VIDEO_PACKET_SIZE, timeout_ms=5000,
                 startup_ms=None, stall_ms=None, grace_ms=None,
                 expected_pt=None, expected_ssrc=None, codec=gsr.H264,
                 fec_max_blocks=8, fec_max_shards=16, nack_capacity=64,
                 srtp=None):
        self.name = name
        self.cfg = dict(codec=codec, window=window, au_cap=au_cap, vps=vps,
                        timeout_ms=timeout_ms,
                        startup_ms=startup_ms if startup_ms is not None
                        else timeout_ms,
                        stall_ms=stall_ms if stall_ms is not None
                        else timeout_ms,
                        grace_ms=grace_ms if grace_ms is not None
                        else timeout_ms,
                        expected_pt=expected_pt,
                        expected_ssrc=expected_ssrc,
                        fec_max_blocks=fec_max_blocks,
                        fec_max_shards=fec_max_shards,
                        nack_capacity=nack_capacity,
                        origin_ms=0)
        self.steps = []
        self.srtp = srtp          # None or dict(profile, tag_len, master, salt)
        self.clock = 1000

    # -- step emitters
    def data(self, index, flags, spi, fi, nal, seed, blen, cur=0, last=0,
             pct=0, fidx=0, fsrc=0, gs_wire=None, aud=0, now=None,
             deliver=1, seq=None, ts=None, ssrc=SSRC, pt=96, marker=0):
        if now is None:
            now = self.clock
            self.clock += 1
        step = {"kind": "data", "index": index,
                "seq": index & 0xFFFF if seq is None else seq,
                "ts": (TS0 + fi * 3000) & 0xFFFFFFFF if ts is None else ts,
                "ssrc": ssrc, "pt": pt, "marker": marker,
                "flags": flags, "spi": spi, "fi": fi, "cur": cur,
                "last": last, "pct": pct, "fidx": fidx, "fsrc": fsrc,
                "gs_wire": gs_wire if gs_wire is not None else -1,
                "nal": nal, "seed": seed, "blen": blen, "aud": aud,
                "now": now, "deliver": deliver}
        self.steps.append(step)
        return len(self.steps) - 1

    def parity(self, index, fi, cur, last, pct, d, p, refs, shard_len,
               now=None, deliver=1, seq=None, ts=None, ssrc=SSRC, pt=96):
        if now is None:
            now = self.clock
            self.clock += 1
        step = {"kind": "parity", "index": index,
                "seq": index & 0xFFFF if seq is None else seq,
                "ts": (TS0 + fi * 3000) & 0xFFFFFFFF if ts is None else ts,
                "ssrc": ssrc, "pt": pt,
                "fi": fi, "cur": cur, "last": last, "pct": pct,
                "d": d, "p": p, "refs": list(refs), "shard_len": shard_len,
                "now": now, "deliver": deliver}
        self.steps.append(step)
        return len(self.steps) - 1

    def raw(self, index, blob, now=None, deliver=1, via_datagram=0):
        if now is None:
            now = self.clock
            self.clock += 1
        self.steps.append({"kind": "raw", "index": index, "blob": bytes(blob),
                           "now": now, "deliver": deliver,
                           "via_datagram": via_datagram})
        return len(self.steps) - 1

    def poll(self, now=None):
        if now is None:
            now = self.clock
            self.clock += 1
        self.steps.append({"kind": "poll", "now": now})
        return len(self.steps) - 1

    def lifecycle(self, op, now=None):
        if now is None:
            now = self.clock
            self.clock += 1
        self.steps.append({"kind": "lifecycle", "op": op, "now": now})
        return len(self.steps) - 1

    def nack_take(self, rtt=0, now=None):
        if now is None:
            now = self.clock
            self.clock += 1
        self.steps.append({"kind": "nack_take", "rtt": rtt, "now": now})
        return len(self.steps) - 1

    def kf_take(self, now=None):
        if now is None:
            now = self.clock
        self.steps.append({"kind": "kf_take", "now": now})
        return len(self.steps) - 1

    def rfi_take(self, now=None):
        if now is None:
            now = self.clock
        self.steps.append({"kind": "rfi_take", "now": now})
        return len(self.steps) - 1

    # -- frame helpers
    def frame(self, base_index, fi, n_pkts, seed0, blens, keyframe=False,
              aud=0, gs=True, cur=0, last=0, pct=0, deliver_mask=None):
        """Emit DATA steps for one frame: first pkt SOF(+EOF if single),
        last EOF, all PIC. spi = index. Returns list of step ids."""
        ids = []
        wire = (FT_I if keyframe else FT_P) if gs else None
        nal = 5 if keyframe else 1
        for i in range(n_pkts):
            flags = FLAG_PIC
            if i == 0:
                flags |= FLAG_SOF
            if i == n_pkts - 1:
                flags |= FLAG_EOF
            fidx = i
            deliver = 1
            if deliver_mask is not None:
                deliver = deliver_mask[i]
            ids.append(self.data(base_index + i, flags, base_index + i, fi,
                                 nal, seed0 + i, blens[i], cur=cur, last=last,
                                 pct=pct, fidx=fidx if pct else 0,
                                 fsrc=n_pkts if pct else 0,
                                 gs_wire=wire if i == 0 else None,
                                 aud=aud if i == n_pkts - 1 else 0,
                                 deliver=deliver))
        return ids


def expand_step(step, cache):
    """Build the packet bytes for a step (deterministic, both sides)."""
    if step["kind"] == "data":
        media = build_media(
            None if step["gs_wire"] < 0 else step["gs_wire"],
            step["nal"], step["seed"], step["blen"], step["aud"])
        return build_pkt(step["seq"], step["ts"], step["ssrc"], step["flags"],
                         step["spi"], step["fi"], step["cur"], step["last"],
                         step["pct"], step["fidx"], step["fsrc"], media,
                         pt=step["pt"], marker=step["marker"])
    if step["kind"] == "parity":
        d = step["d"]
        p = step["p"]
        pct = step["pct"]
        npar = parity_count_for(d, pct)
        shard_len = step["shard_len"]
        fold = bytearray(shard_len)
        for i, ref in enumerate(step["refs"]):
            src = cache[ref].ljust(shard_len, b"\0")[:shard_len]
            fecr.axpy(fold, src, fecr.cauchy(i, p, npar))
        # own envelope + selected ext fields (see docs/RX.md — canonical
        # GFN-compatible parity construction; ext[0..4] and ext[8..11]
        # stay algebraic so recovery reconstructs the data shard's spi and
        # flags exactly)
        fold[0] = 0x90
        fold[1] = step["pt"] & 0x7F
        fold[2:4] = (step["seq"] & 0xFFFF).to_bytes(2, "big")
        fold[4:8] = (step["ts"] & 0xFFFFFFFF).to_bytes(4, "big")
        fold[8:12] = (step["ssrc"] & 0xFFFFFFFF).to_bytes(4, "big")
        fold[12:16] = bytes([0x47, 0x53, 0x00, 0x04])
        fi = step["fi"] & 0xFFFFFFFF
        fold[20:24] = fi.to_bytes(4, "little")
        fold[27] = ((step["last"] & 3) << 6) | ((step["cur"] & 3) << 4)
        fidx = d + p
        fec_word = ((pct & 0xFF) << 4) | ((fidx & 0x3FF) << 12) \
            | ((d & 0x3FF) << 22)
        fold[28:32] = fec_word.to_bytes(4, "little")
        return bytes(fold)
    if step["kind"] == "raw":
        return step["blob"]
    return None


def run_scenario(sb):
    """Execute a scenario through the Python reference; returns the trace
    structure the emitter needs."""
    rx = Receiver(**sb.cfg)
    if sb.srtp:
        setup_srtp(rx, sb.srtp)
    cache = {}
    trace = []
    for sid, step in enumerate(sb.steps):
        events = []
        pkt = expand_step(step, cache)
        if pkt is not None:
            cache[sid] = pkt
        state_before = rx.state
        if step["kind"] in ("data", "parity", "raw") and step.get("deliver", 1):
            if step.get("via_datagram"):
                # the blob IS the protected datagram (pre-protected by the
                # scenario builder with the Python session)
                rx_process_datagram(rx, pkt, step["now"], events)
            else:
                rx.process_packet(step["index"], pkt, step["now"], events)
        elif step["kind"] == "poll":
            rx.poll(step["now"], events)
        elif step["kind"] == "lifecycle":
            op = step["op"]
            if op == 0:
                rx.pause(step["now"], events)
            elif op == 1:
                rx.resume(step["now"], events)
            elif op == 2:
                rx.recover(step["now"], events)
            else:
                rx.stop(step["now"], events)
        elif step["kind"] == "nack_take":
            pass       # handled below (needs its own record)
        elif step["kind"] == "kf_take":
            pass
        elif step["kind"] == "rfi_take":
            pass
        exp = {"events": [], "frames": [], "nack": None, "kf": -1,
               "rfi": None, "state": -1}
        for e in events:
            if e["t"] == -1:
                exp["frames"].append(e)
            else:
                exp["events"].append(e)
        if step["kind"] == "nack_take":
            got = rx.nack.take(step["now"],
                               step["rtt"] if step["rtt"] else None)
            exp["nack"] = got
        elif step["kind"] == "kf_take":
            exp["kf"] = 1 if rx.take_keyframe_request() else 0
        elif step["kind"] == "rfi_take":
            exp["rfi"] = rx.inval.take()
        if step["kind"] not in ("data", "parity", "raw", "poll", "nack_take",
                                "kf_take", "rfi_take") or state_before != rx.state:
            exp["state"] = rx.state
        trace.append(exp)
    counters = {
        "auth": rx.authenticated, "fec_pkts": rx.fec_packets,
        "fec_rep": rx.fec_repaired, "frames": rx.frames_emitted,
        "stale": rx.stale_packets, "gaps": rx.gap_recoveries,
        "withheld": rx.withheld, "kf_req": rx.keyframe_requests,
        "retrans": rx.retransmissions, "replay": rx.replay_rejections,
    }
    return trace, counters


# --------------------------------------------------------- SRTP scenarios

def setup_srtp(rx, spec):
    """Attach crypto material to the Receiver (mirrors kn_rx_video_set_srtp).
    The Python Receiver consumes plaintext via process_packet; for datagram
    scenarios we keep the session in rx._srtp and do the unprotect inline."""
    profile = spec["profile"]
    master = spec["master"]
    salt = spec["salt"]
    tag_len = spec["tag_len"]
    if profile == "cm128":
        enc = sr_kdf128(master, salt, 0x00, 16)
        auth = sr_kdf128(master, salt, 0x01, 20)
        ssalt = sr_kdf128(master, salt, 0x02, 14)
    elif profile == "cm256":
        enc = kdf256(master, salt, 0x00, 32)
        auth = kdf256(master, salt, 0x01, 20)
        ssalt = kdf256(master, salt, 0x02, 14)
    elif profile == "gcm256":
        enc = kdf256(master, salt, 0x00, 32)
        ssalt = kdf256(master, salt, 0x02, 12)
        auth = b""
    elif profile == "gcm128":
        enc = sr_kdf128(master, salt, 0x00, 16)
        ssalt = sr_kdf128(master, salt, 0x02, 12)
        auth = b""
    else:
        raise ValueError(profile)
    import srtp_reference as sr
    rx._srtp = {"profile": profile, "enc": enc, "auth": auth, "salt": ssalt,
                "tag_len": tag_len, "replay": sr.ReplayWindow()}
    rx._srtp_derived = {"enc": enc, "auth": auth, "salt": ssalt}


def sr_kdf128(master, salt, label, n):
    import srtp_reference as sr
    return sr.kdf128(master, salt, label, n)


def _hmac_sha1(key, msg):
    import hmac as pyhmac
    import hashlib
    return pyhmac.new(bytes(key), bytes(msg), hashlib.sha1).digest()


def _cm_keystream_xor(key, iv, data):
    """AES-CM (128 or 256) keystream XOR via the proved primitives."""
    import srtp_reference as sr
    out = bytearray(data)
    nblocks = (len(out) + 15) // 16
    for b in range(nblocks):
        if len(key) == 16:
            ks = sr.cm_keystream_block(key, iv, b)
        else:
            ctr = (int.from_bytes(iv, "big") + b)
            ks = _aes_encrypt_any(key, ctr.to_bytes(16, "big"))
        chunk = out[b * 16:(b + 1) * 16]
        out[b * 16:(b + 1) * 16] = bytes(x ^ y for x, y in zip(chunk, ks))
    return bytes(out)


def protect_for_scenario(plain, step, rx, spec):
    """Protect one plaintext packet exactly like the C-side protect in the
    test harness (kn_srtp_protect_cm_hmac / gcm_encrypt / manual CM256)."""
    s = rx._srtp
    rc, hdr = rtp_parse_hdr(plain)
    assert rc == GS_OK
    ssrc = hdr["ssrc"]
    seq = hdr["seq"]
    index = step["index"]
    roc = index >> 16
    tag_len = s["tag_len"]
    if s["profile"] in ("cm128", "cm256"):
        import srtp_reference as sr
        iv = sr.srtp_aes_cm_iv(s["salt"], ssrc, index)
        body = plain[:hdr["payload_off"]] + \
            _cm_keystream_xor(s["enc"], iv, plain[hdr["payload_off"]:])
        mac_in = body + (roc & 0xFFFFFFFF).to_bytes(4, "big")
        tag = _hmac_sha1(s["auth"], mac_in)[:tag_len]
        return body + tag
    # GCM
    import srtp_reference as sr
    iv = sr.srtp_gcm_iv(s["salt"], ssrc, roc & 0xFFFFFFFF, seq)
    aad = plain[:hdr["payload_off"]]
    ct, tag = gcm_crypt(s["enc"], iv, aad, plain[hdr["payload_off"]:],
                        tag_len, encrypting=True)
    return aad + ct + tag


def rx_process_datagram(rx, datagram, now, events):
    """process_datagram for the Python reference (SRTP path)."""
    if rx.state != ST_RUNNING:
        reason = {ST_PAUSED: D_PAUSED, ST_STOPPED: D_STOPPED,
                  ST_RECOVERY: D_RECOVERY_REQUIRED}[rx.state]
        events.append({"t": EV_DROPPED, "r": reason, "i": 0})
        return
    s = rx._srtp
    rc, hdr = rtp_parse_hdr(datagram)
    if rc != GS_OK:
        events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP, "i": 0,
                       "d": rc})
        return
    import srtp_reference as sr
    grc, idx = s["replay"].guess(hdr["seq"])
    if grc != 0:
        rx.replay_rejections += 1
        events.append({"t": EV_DROPPED, "r": D_REPLAY_REJECTED, "i": 0})
        return
    roc = idx >> 16
    tag_len = s["tag_len"]
    if len(datagram) < tag_len:
        events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP, "i": 0,
                       "d": gsr.E_TOO_SHORT})
        return
    ct_end = len(datagram) - tag_len
    ssrc = hdr["ssrc"]
    if s["profile"] in ("cm128", "cm256"):
        auth_len = ct_end
        tag = datagram[auth_len:auth_len + tag_len]
        mac_in = datagram[:auth_len] + (roc & 0xFFFFFFFF).to_bytes(4, "big")
        if not _const_eq(_hmac_sha1(s["auth"], mac_in)[:tag_len], tag):
            events.append({"t": EV_DROPPED, "r": D_AUTH_FAILED, "i": 0})
            return
        iv = sr.srtp_aes_cm_iv(s["salt"], ssrc, idx)
        plain = datagram[:hdr["payload_off"]] + _cm_keystream_xor(
            s["enc"], iv, datagram[hdr["payload_off"]:auth_len])
    else:
        if ct_end < hdr["payload_off"]:
            events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP, "i": 0,
                           "d": gsr.E_TOO_SHORT})
            return
        iv = sr.srtp_gcm_iv(s["salt"], ssrc, roc & 0xFFFFFFFF, hdr["seq"])
        pt, tag = gcm_crypt(s["enc"], iv, datagram[:hdr["payload_off"]],
                            datagram[hdr["payload_off"]:ct_end], tag_len,
                            encrypting=False)
        if not _const_eq(tag, datagram[ct_end:ct_end + tag_len]):
            events.append({"t": EV_DROPPED, "r": D_AUTH_FAILED, "i": 0})
            return
        plain = datagram[:hdr["payload_off"]] + pt
    if s["replay"].check(idx) != 0:
        rx.replay_rejections += 1
        events.append({"t": EV_DROPPED, "r": D_REPLAY_REJECTED, "i": 0})
        return
    rc2, poff, plen = rtp_payload(hdr, plain)
    if rc2 != GS_OK:
        events.append({"t": EV_DROPPED, "r": D_MALFORMED_RTP, "i": 0,
                       "d": rc2})
        return
    s["replay"].commit(idx)
    rx._process_authenticated(idx, hdr, poff, plen, plain, now, events)


def _const_eq(a, b):
    if len(a) != len(b):
        return False
    r = 0
    for x, y in zip(a, b):
        r |= x ^ y
    return r == 0


# ------------------------------------------------------- scenario catalog

def build_scenarios():
    S = []

    # S1: three in-order frames (I,P,P), GS frame headers, no FEC
    sb = ScenarioBuilder("inorder_frames", window=8)
    sb.frame(1, 1, 3, 100, [12, 10, 14], keyframe=True)
    sb.frame(4, 2, 3, 200, [12, 10, 14])
    sb.frame(7, 3, 2, 300, [20, 8])
    S.append(sb)

    # S2: same with trailing AUDs on every frame (must be stripped)
    sb = ScenarioBuilder("inorder_frames_aud", window=8)
    sb.frame(1, 1, 3, 100, [12, 10, 14], keyframe=True, aud=1)
    sb.frame(4, 2, 3, 200, [12, 10, 14], aud=1)
    S.append(sb)

    # S3: network reordering inside and across frames (reorder buffer heals;
    # the lowest index still arrives first — a higher index first would make
    # the earlier ones legitimately STALE, which S4/S22 cover)
    sb = ScenarioBuilder("reorder_swap", window=8)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 100, 12, gs_wire=FT_I)
    sb.data(3, FLAG_EOF | FLAG_PIC, 3, 1, 1, 102, 14)   # EOF early
    sb.data(2, FLAG_PIC, 2, 1, 1, 101, 10)              # mid late -> chain
    sb.data(4, FLAG_SOF | FLAG_PIC, 4, 2, 5, 200, 11, gs_wire=FT_P)
    sb.data(6, FLAG_EOF | FLAG_PIC, 6, 2, 1, 202, 9)
    sb.data(5, FLAG_PIC, 5, 2, 1, 201, 13)
    S.append(sb)

    # S4: duplicate + stale
    sb = ScenarioBuilder("dup_stale", window=8)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 100, 12, gs_wire=FT_I)
    sb.data(2, FLAG_EOF | FLAG_PIC, 2, 1, 1, 101, 10)
    sb.data(2, FLAG_EOF | FLAG_PIC, 2, 1, 1, 101, 10)   # duplicate (stale:
    # it was delivered already -> STALE; a still-buffered twin would be DUP)
    sb.data(3, FLAG_SOF | FLAG_PIC, 3, 2, 5, 200, 12, gs_wire=FT_P)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 100, 12, gs_wire=FT_I)  # stale
    sb.data(4, FLAG_EOF | FLAG_PIC, 4, 2, 1, 201, 10)
    S.append(sb)

    # S4b: a真 duplicate — the twin is still buffered behind a gap
    sb = ScenarioBuilder("dup_buffered", window=8)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 100, 12, gs_wire=FT_I)
    sb.data(3, FLAG_EOF | FLAG_PIC, 3, 1, 1, 102, 10)   # gap at 2: 3 buffers
    sb.data(3, FLAG_EOF | FLAG_PIC, 3, 1, 1, 102, 10)   # duplicate (buffered)
    sb.data(2, FLAG_PIC, 2, 1, 1, 101, 8)               # fill -> chain out
    S.append(sb)

    # S5: gaps_require_recovery pipeline port (window=4 like the Rust test)
    sb = ScenarioBuilder("gap_recovery_window", window=4)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 100, 12, gs_wire=FT_I)
    sb.data(6, FLAG_EOF | FLAG_PIC, 6, 1, 1, 101, 10)   # jump >= window
    sb.data(7, FLAG_SOF | FLAG_EOF | FLAG_PIC, 7, 2, 5, 200, 16,
            gs_wire=FT_I)
    S.append(sb)

    # S6: gap filled before the deadline; NACK taken and resolved
    sb = ScenarioBuilder("gap_fill_in_time", window=8)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 100, 12, gs_wire=FT_I)
    sb.data(3, FLAG_EOF | FLAG_PIC, 3, 1, 1, 102, 10, now=1002)
    sb.nack_take(rtt=0, now=1003)
    sb.data(2, FLAG_PIC, 2, 1, 1, 101, 8, now=1020)    # retransmission
    S.append(sb)

    # S7: 52-ms dequeue deadline at pipeline level (plain packets, no GS)
    sb = ScenarioBuilder("deadline_52ms", window=DEFAULT_REORDER_WINDOW)
    plain = bytes([0x80, 96]) + (1).to_bytes(2, "big") \
        + (90000).to_bytes(4, "big") + (1).to_bytes(4, "big")
    for idx, t in ((1, 0), (3, 1), (4, 52), (5, 53)):
        p = bytearray(plain)
        p[2:4] = (idx & 0xFFFF).to_bytes(2, "big")
        sb.raw(idx, bytes(p), now=1000 + t)
    S.append(sb)

    # S8/S8b: FEC repair end-to-end (4+1), byte-identical to the baseline
    def fec_frame(sb, base, fi, seed, deliver_mask, with_parity, d=4,
                  pct=25, blens=None):
        shard_len = sb.cfg["vps"] + FEC_HEADER_ALLOWANCE
        if blens is None:
            # exact-length packets (pkt len == shard_len) so a repaired
            # shard is byte-identical to the original (no zero stuffing)
            blens = [shard_len - 32 - 8] + [shard_len - 32 - 4] * (d - 1)
        ids = []
        for i in range(d):
            flags = FLAG_PIC | (FLAG_SOF if i == 0 else 0) \
                | (FLAG_EOF if i == d - 1 else 0)
            ids.append(sb.data(base + i, flags, base + i, fi,
                               5 if i == 0 else 1, seed + i, blens[i],
                               cur=0, last=0, pct=pct, fidx=i, fsrc=d,
                               gs_wire=(FT_I if i == 0 else None),
                               deliver=deliver_mask[i]))
        if with_parity:
            npar = parity_count_for(d, pct)
            for p in range(npar):
                sb.parity(base + d + p, fi, 0, 0, pct, d, p, ids, shard_len)
        return ids

    sb = ScenarioBuilder("fec_repair_e2e", window=16, vps=256)
    fec_frame(sb, 1, 1, 500, [1, 1, 0, 1], True)
    S.append(sb)
    sb = ScenarioBuilder("fec_repair_baseline", window=16, vps=256)
    fec_frame(sb, 1, 1, 500, [1, 1, 1, 1], True)
    S.append(sb)

    # S9: parity-only recovery (1 data + 2 parity, pct=200)
    sb = ScenarioBuilder("fec_parity_only", window=16, vps=256)
    shard_len = 256 + FEC_HEADER_ALLOWANCE
    id0 = sb.data(100, FLAG_SOF | FLAG_EOF | FLAG_PIC, 100, 7, 5, 700, 40,
                  pct=200, fidx=0, fsrc=1, gs_wire=FT_I, deliver=0)
    sb.parity(101, 7, 0, 0, 200, 1, 0, [id0], shard_len)
    sb.parity(102, 7, 0, 0, 200, 1, 1, [id0], shard_len)  # after completion
    S.append(sb)

    # S10: unrecoverable loss -> NACK, timeout, recovery, next frame
    sb = ScenarioBuilder("fec_unrecoverable", window=16, vps=256)
    fec_frame(sb, 1, 1, 800, [1, 0, 0, 1], True)     # data 1-4, parity 5
    fec_frame(sb, 6, 2, 850, [1, 1, 1, 1], True)     # data 6-9, parity 10
    sb.poll(now=1100)          # > 52 ms after the gap was armed
    fec_frame(sb, 11, 3, 900, [1, 1, 1, 1], False)   # data 11-14, no parity
    S.append(sb)

    # S11: duplicate FEC shard while the block is still incomplete
    sb = ScenarioBuilder("fec_dup_shard", window=16, vps=256)
    d, pct = 4, 25
    i0 = sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 950, 40, pct=pct, fidx=0,
                 fsrc=d, gs_wire=FT_I)
    i1 = sb.data(2, FLAG_PIC, 2, 1, 1, 951, 44, pct=pct, fidx=1, fsrc=d)
    sb.steps.append(dict(sb.steps[i1]))         # duplicate, block incomplete
    sb.data(3, FLAG_PIC, 3, 1, 1, 952, 44, pct=pct, fidx=2, fsrc=d)
    sb.data(4, FLAG_EOF | FLAG_PIC, 4, 1, 1, 953, 44, pct=pct, fidx=3,
            fsrc=d)
    S.append(sb)

    # S12: FEC layout mismatch (wrong frame_index on a data shard)
    sb = ScenarioBuilder("fec_mismatch", window=16, vps=256)
    shard_len = 256 + FEC_HEADER_ALLOWANCE
    a = sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 970, 40, pct=25, fidx=0,
                fsrc=4, gs_wire=FT_I)
    b = sb.data(2, FLAG_PIC, 2, 99, 1, 971, 44, pct=25, fidx=1, fsrc=4)
    c = sb.data(3, FLAG_PIC, 3, 1, 1, 972, 44, pct=25, fidx=2, fsrc=4)
    d = sb.data(4, FLAG_EOF | FLAG_PIC, 4, 1, 1, 973, 44, pct=25, fidx=3,
                fsrc=4)
    sb.parity(5, 1, 0, 0, 25, 4, 0, [a, b, c, d], shard_len)
    S.append(sb)

    # S13: multi-block frame (2 blocks of 4+1), one loss repaired.
    # Index space: block0 data 1-4 + parity 5; block1 data 6-9 + parity 10.
    # SPI counts DATA packets only (1-8) — parity consumes RTP indices but
    # not stream-packet indices, so same-frame SPI continuity holds across
    # the block boundary (recovered shards restore their SPI algebraically).
    sb = ScenarioBuilder("fec_multiblock", window=24, vps=256)
    shard_len = 256 + FEC_HEADER_ALLOWANCE
    b0 = []
    for i in range(4):
        flags = FLAG_PIC | (FLAG_SOF if i == 0 else 0)
        b0.append(sb.data(1 + i, flags, 1 + i, 21, 5 if i == 0 else 1,
                          1000 + i, shard_len - 40 if i == 0
                          else shard_len - 36, cur=0, last=1,
                          pct=25, fidx=i, fsrc=4,
                          gs_wire=(FT_I if i == 0 else None)))
    b1 = []
    for i in range(4):
        flags = FLAG_PIC | (FLAG_EOF if i == 3 else 0)
        b1.append(sb.data(6 + i, flags, 5 + i, 21, 1, 1010 + i,
                          shard_len - 36,
                          cur=1, last=1, pct=25, fidx=i, fsrc=4,
                          deliver=0 if i == 2 else 1))
    sb.parity(5, 21, 0, 1, 25, 4, 0, b0, shard_len)
    sb.parity(10, 21, 1, 1, 25, 4, 0, b1, shard_len)
    S.append(sb)

    # S14: SOF-before-EOF (frame discontinuity) with unknown frame types
    sb = ScenarioBuilder("sof_before_eof", window=8)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 1100, 12)     # no GS hdr
    sb.data(2, FLAG_PIC, 2, 1, 1, 1101, 10)
    sb.data(3, FLAG_SOF | FLAG_PIC, 3, 2, 5, 1200, 12)     # new SOF: drop
    sb.data(4, FLAG_EOF | FLAG_PIC, 4, 2, 1, 1201, 10)
    sb.kf_take()
    S.append(sb)

    # S15: stream-packet-index discontinuity inside a frame
    sb = ScenarioBuilder("spi_discontinuity", window=8)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 1300, 12, gs_wire=FT_I)
    sb.data(2, FLAG_PIC, 12, 1, 1, 1301, 10)               # spi jumps 1->12
    sb.data(3, FLAG_EOF | FLAG_PIC, 3, 1, 1, 1302, 10)     # after invalidate
    sb.data(4, FLAG_SOF | FLAG_EOF | FLAG_PIC, 4, 2, 5, 1400, 20,
            gs_wire=FT_I)
    S.append(sb)

    # S16: RFI withhold flow (types known -> invalidation owns recovery)
    sb = ScenarioBuilder("rfi_withhold_flow", window=4)
    sb.frame(1, 1, 1, 1500, [20], keyframe=True)           # I frame admitted
    sb.data(5, FLAG_SOF | FLAG_PIC, 5, 3, 5, 1600, 12, gs_wire=FT_P)  # jump
    # -> empty-buffer jump >= window: recovery -> invalidate (types known
    #    -> RFI owns it, NO keyframe request)
    sb.data(6, FLAG_EOF | FLAG_PIC, 6, 3, 1, 1601, 10)     # completes P f3
    # admit: gap 1->3 -> RFI range (2,2), P withheld
    sb.rfi_take()
    sb.frame(7, 4, 1, 1700, [20], keyframe=True)           # I -> recovers
    sb.kf_take()                                           # expect 0
    S.append(sb)

    # S17: video timeout + recovery lifecycle
    sb = ScenarioBuilder("timeout_lifecycle", window=8, timeout_ms=500)
    sb.frame(1, 1, 1, 1800, [20], keyframe=True, )
    sb.poll(now=1499)
    sb.poll(now=1500 + 500)      # >= origin+... last auth at ~1500
    sb.data(2, FLAG_SOF | FLAG_EOF | FLAG_PIC, 2, 2, 5, 1900, 16,
            gs_wire=FT_I)        # dropped: recovery required
    sb.lifecycle(2)              # recover
    sb.data(3, FLAG_SOF | FLAG_EOF | FLAG_PIC, 3, 3, 5, 1950, 16,
            gs_wire=FT_I)        # flows again; contiguous=0 after reset
    S.append(sb)

    # S18: pause/resume
    sb = ScenarioBuilder("pause_resume", window=8)
    sb.frame(1, 1, 1, 2000, [20], keyframe=True)
    sb.lifecycle(0)              # pause
    sb.data(2, FLAG_SOF | FLAG_EOF | FLAG_PIC, 2, 2, 5, 2050, 16,
            gs_wire=FT_I)        # dropped: paused
    sb.lifecycle(1)              # resume
    sb.data(3, FLAG_SOF | FLAG_EOF | FLAG_PIC, 3, 3, 5, 2100, 16,
            gs_wire=FT_I)
    S.append(sb)

    # S19: NACK-v2 flow — gap range taken from the tracker
    sb = ScenarioBuilder("nack_flow", window=8)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 2200, 12, gs_wire=FT_I)
    sb.data(4, FLAG_EOF | FLAG_PIC, 4, 1, 1, 2203, 10)     # gap 2..3
    sb.nack_take(rtt=0)
    sb.nack_take(rtt=0)          # gated by the retry interval
    sb.nack_take(rtt=0, now=sb.clock + 4)
    S.append(sb)

    # S20: keyframe-request flag lifecycle
    sb = ScenarioBuilder("keyframe_flag", window=4)
    sb.data(1, FLAG_SOF | FLAG_PIC, 1, 1, 5, 2300, 12)     # no GS hdr
    sb.data(9, FLAG_EOF | FLAG_PIC, 9, 1, 1, 2301, 10)     # jump -> recover
    sb.kf_take()                                           # 1 (types unknown)
    sb.kf_take()                                           # 0 (taken)
    sb.data(10, FLAG_SOF | FLAG_EOF | FLAG_PIC, 10, 2, 5, 2400, 16,
            gs_wire=FT_I)
    sb.kf_take()                                           # 0 (keyframe cleared)
    S.append(sb)

    # S21: 2^24 stream-index wrap
    sb = ScenarioBuilder("spi_wrap", window=8)
    base = 0xFFFFFE
    sb.data(base, FLAG_SOF | FLAG_PIC, base & SPI_MASK, 31, 5, 2500, 12,
            gs_wire=FT_I)
    sb.data(base + 1, FLAG_PIC, (base + 1) & SPI_MASK, 31, 1, 2501, 10)
    sb.data(base + 2, FLAG_EOF | FLAG_PIC, (base + 2) & SPI_MASK, 31, 1,
            2502, 10)
    sb.data(base + 3, FLAG_SOF | FLAG_EOF | FLAG_PIC, (base + 3) & SPI_MASK,
            32, 5, 2600, 16, gs_wire=FT_P)
    S.append(sb)

    # S22: huge jump on an empty buffer (window=4)
    sb = ScenarioBuilder("huge_jump_empty", window=4)
    plain = bytearray(bytes([0x80, 96]) + (0).to_bytes(2, "big")
                      + (90000).to_bytes(4, "big") + (1).to_bytes(4, "big"))
    plain[2:4] = (1).to_bytes(2, "big")
    sb.raw(1, bytes(plain), now=1000)
    p9 = bytearray(plain)
    p9[2:4] = (9).to_bytes(2, "big")
    sb.raw(9, bytes(p9), now=1001)
    S.append(sb)

    # S23: window capacity recovery (packets.len() >= max_packets)
    sb = ScenarioBuilder("capacity_recovery", window=4)
    for idx in (1, 3, 5, 6, 7):
        p = bytearray(plain)
        p[2:4] = (idx & 0xFFFF).to_bytes(2, "big")
        sb.raw(idx, bytes(p), now=1000 + idx)
    S.append(sb)

    # S24: malformed packets (ssrc = SSRC everywhere so the first AUTHENTICATED
    # packet binds the same ssrc the trailing good frame uses)
    sb = ScenarioBuilder("malformed", window=8)
    sb.raw(1, bytes([0x80, 96]) + (1).to_bytes(2, "big")
           + (90000).to_bytes(4, "big") + SSRC.to_bytes(4, "big"), now=1000)
    bad_ver = bytearray(bytes([0xC0, 96]) + (2).to_bytes(2, "big")
                        + (90000).to_bytes(4, "big") + SSRC.to_bytes(4, "big"))
    sb.raw(2, bytes(bad_ver), now=1001)                    # version 3
    sb.raw(3, bytes([0x80, 96, 0, 3]), now=1002)          # too short
    pad = bytearray(bytes([0xA0, 96]) + (4).to_bytes(2, "big")
                    + (90000).to_bytes(4, "big") + SSRC.to_bytes(4, "big")
                    + b"\x01\x02\x00")      # P bit, pad byte 0 -> invalid
    sb.raw(4, bytes(pad), now=1003)
    # window=8: index 50 on an empty buffer jumps >= window -> immediate
    # recovery past the malformed graveyard, frame flows again
    sb.frame(50, 9, 1, 2700, [16], keyframe=True)
    S.append(sb)

    # S25: payload-type / SSRC filters
    sb = ScenarioBuilder("filters", window=8, expected_pt=96,
                         expected_ssrc=0xDEADBEEF)
    sb.data(1, FLAG_SOF | FLAG_EOF | FLAG_PIC, 1, 1, 5, 2800, 16,
            gs_wire=FT_I, ssrc=0xDEADBEEF)
    sb.data(2, FLAG_SOF | FLAG_EOF | FLAG_PIC, 2, 2, 5, 2810, 16,
            gs_wire=FT_I, ssrc=0xDEADBEEF, pt=97)          # wrong PT
    sb.data(3, FLAG_SOF | FLAG_EOF | FLAG_PIC, 3, 3, 5, 2820, 16,
            gs_wire=FT_I, ssrc=0xBEEF)                     # wrong SSRC
    S.append(sb)

    # S26: progress watchdog stall -> keyframe -> recovery
    sb = ScenarioBuilder("watchdog_flow", window=8, timeout_ms=5000,
                         stall_ms=100, grace_ms=100)
    sb.frame(1, 1, 1, 2900, [20], keyframe=True)           # auth at ~1000
    sb.poll(now=1099)
    sb.poll(now=1101)          # stall (last assembled 1000 + 100)
    sb.kf_take()               # 1
    sb.poll(now=1202)          # grace elapsed -> recovery
    sb.lifecycle(2)            # recover
    sb.frame(2, 2, 1, 2950, [20], keyframe=True)
    S.append(sb)

    return S


def sc_scenario_invariants():
    """Run the catalog + fuzz through the Python reference and assert the
    cross-cutting invariants (self-proof #6)."""
    import random

    def run(sb):
        trace, counters = run_scenario(sb)
        frames = []
        for step_exp in trace:
            frames.extend(step_exp["frames"])
        return sb, trace, counters, frames

    # --- catalog sanity: baseline vs FEC-repaired byte identity
    S = {s.name: s for s in build_scenarios()}
    _, _, c_rep, f_rep = run(S["fec_repair_e2e"])
    _, _, c_base, f_base = run(S["fec_repair_baseline"])
    _check(len(f_rep) == 1 and len(f_base) == 1, "S8: one frame each")
    if f_rep and f_base:
        _check(f_rep[0]["hash"] == f_base[0]["hash"]
               and f_rep[0]["len"] == f_base[0]["len"],
               "S8: repaired frame byte-identical to baseline")
    _check(c_rep["fec_rep"] == 1 and c_base["fec_rep"] == 0,
           "S8: repaired counter")

    # --- fuzz: lossless reordering/dup/stale must reproduce the baseline
    rng = random.Random(20261024)
    for trial in range(60):
        n_frames = rng.randint(1, 4)
        sb = ScenarioBuilder("fuzz_base_%d" % trial, window=16)
        plan = []
        idx = 1
        for f in range(n_frames):
            n_pkts = rng.randint(1, 4)
            blens = [rng.randint(4, 40) for _ in range(n_pkts)]
            ids = sb.frame(idx, f + 1, n_pkts, 100 * (f + 1), blens,
                           keyframe=(f == 0))
            plan.extend((i, idx + k) for k, i in enumerate(ids))
            idx += n_pkts
        base_trace, base_cnt = run_scenario(sb)
        base_frames = [fr for st in base_trace for fr in st["frames"]]

        # reordered twin: permute step order but keep every packet
        sb2 = ScenarioBuilder("fuzz_re_%d" % trial, window=16)
        steps = list(sb.steps)
        order = list(range(len(steps)))
        for i in range(len(order) - 1):
            if rng.random() < 0.4:
                j = min(i + rng.randint(1, 3), len(order) - 1)
                order[i], order[j] = order[j], order[i]
        sb2.steps = [steps[k] for k in order]
        # clock must stay monotonic for the re-run
        t = 1000
        for st in sb2.steps:
            st["now"] = t
            t += 1
        re_trace, re_cnt = run_scenario(sb2)
        re_frames = [fr for st in re_trace for fr in st["frames"]]
        # The reorder buffer delivers in index order, but a permutation that
        # delivers a HIGHER index first makes every earlier index STALE
        # (reference behaviour, no recovery event) — real loss. Compare with
        # the baseline only when nothing was lost at all.
        if re_cnt["gaps"] == 0 and re_cnt["stale"] == 0:
            _check([ (f["frame_index"], f["hash"]) for f in re_frames ] ==
                   [ (f["frame_index"], f["hash"]) for f in base_frames ],
                   "fuzz %d: reordered == baseline when lossless" % trial)
        idxs = [f["frame_index"] for f in re_frames]
        _check(idxs == sorted(set(idxs)), "fuzz %d: frames ordered+unique" % trial)

    # --- fuzz: FEC-recoverable loss must reproduce the baseline exactly
    for trial in range(40):
        d = rng.randint(1, 4)
        pct = rng.choice([25, 50, 100, 200])
        npar = parity_count_for(d, pct)
        lose = min(rng.randint(1, npar), d)      # recoverable by construction
        lost = set(rng.sample(range(d), lose))
        sb = ScenarioBuilder("fuzz_fec_%d" % trial, window=16, vps=256)
        shard_len = 256 + FEC_HEADER_ALLOWANCE
        # exact-length packets: media = shard_len - 32
        blens = [shard_len - 32 - 8] + [shard_len - 32 - 4] * (d - 1)
        mask = [0 if i in lost else 1 for i in range(d)]
        ids = sb.frame(1, 1, d, 3000 + trial, blens[:d], keyframe=True,
                       pct=pct, deliver_mask=mask)
        # frame() sets fidx/fsrc through the pct path already
        for p in range(npar):
            sb.parity(1 + d + p, 1, 0, 0, pct, d, p, ids, shard_len)
        tr, cnt = run_scenario(sb)
        frames = [fr for st in tr for fr in st["frames"]]
        # baseline (no loss)
        sb_b = ScenarioBuilder("fuzz_fec_b_%d" % trial, window=16, vps=256)
        sb_b.frame(1, 1, d, 3000 + trial, blens[:d], keyframe=True)
        tr_b, _ = run_scenario(sb_b)
        frames_b = [fr for st in tr_b for fr in st["frames"]]
        _check(len(frames) == 1 and len(frames_b) == 1,
               "fuzz fec %d: frame emitted" % trial)
        if frames and frames_b:
            _check(frames[0]["hash"] == frames_b[0]["hash"]
                   and frames[0]["len"] == frames_b[0]["len"]
                   and frames[0]["contiguous"] == 1,
                   "fuzz fec %d: recovered frame byte-identical" % trial)
        _check(cnt["fec_rep"] == lose, "fuzz fec %d: repaired count" % trial)

    # --- fuzz: NACK ranges never contain delivered or buffered indices
    for trial in range(30):
        sb = ScenarioBuilder("fuzz_nack_%d" % trial, window=8)
        n = rng.randint(4, 14)
        deliver = [1] * n
        for i in range(n):
            if rng.random() < 0.3:
                deliver[i] = 0
        for i in range(n):
            flags = FLAG_PIC | (FLAG_SOF if i == 0 else 0) \
                | (FLAG_EOF if i == n - 1 else 0)
            sb.data(1 + i, flags, 1 + i, 1, 5 if i == 0 else 1,
                    4000 + i, 10, deliver=deliver[i])
        sb.poll(now=sb.clock + 60)
        rx = Receiver(**sb.cfg)
        cache = {}
        nack_ranges = []
        delivered = set()
        orig_request = rx.nack.request

        def spy(first, last, now, kf):
            nack_ranges.append((first, last))
            return orig_request(first, last, now, kf)
        rx.nack.request = spy
        for sid, step in enumerate(sb.steps):
            pkt = expand_step(step, cache)
            if pkt is not None:
                cache[sid] = pkt
            if step["kind"] == "data" and step["deliver"]:
                rx.process_packet(step["index"], pkt, step["now"], [])
                delivered.add(step["index"])
            elif step["kind"] == "poll":
                rx.poll(step["now"], [])
        buffered = set(rx.reorder.packets.keys())
        ok = True
        for (a, b) in nack_ranges:
            for idx in range(a, b + 1):
                if idx in delivered or idx in buffered:
                    ok = False
        _check(ok, "fuzz nack %d: ranges exclude held indices" % trial)


# ------------------------------------------------------------ SRTP vectors

SRTP_MASTER128 = bytes(range(16))
SRTP_MASTER256 = bytes(range(32))
SRTP_SALT14 = bytes.fromhex("00000000000000009ECA935E1234")
SRTP_SALT12 = bytes.fromhex("00000000000000009ECA935E")


def srtp_vectors():
    """Session-derivation + protect/unprotect goldens for every profile the
    C side implements (CM128 always; CM256/GCM128/GCM256 under mbedTLS)."""
    vecs = []
    specs = [
        ("cm128_tag10", "cm128", SRTP_MASTER128, SRTP_SALT14, 10),
        ("cm128_tag4", "cm128", SRTP_MASTER128, SRTP_SALT14, 4),
        ("cm256_tag10", "cm256", SRTP_MASTER256, SRTP_SALT14, 10),
        ("gcm128_tag8", "gcm128", SRTP_MASTER128, SRTP_SALT12, 8),
        ("gcm256_tag8", "gcm256", SRTP_MASTER256, SRTP_SALT12, 8),
        ("gcm256_tag16", "gcm256", SRTP_MASTER256, SRTP_SALT12, 16),
    ]
    for name, profile, master, salt, tag_len in specs:
        dummy = Receiver(window=8)
        setup_srtp(dummy, {"profile": profile, "master": master,
                           "salt": salt, "tag_len": tag_len})
        derived = dummy._srtp_derived
        plain = build_pkt(7, TS0, SSRC, FLAG_SOF | FLAG_EOF | FLAG_PIC, 7,
                          77, 0, 0, 0, 0, 0,
                          build_media(FT_I, 5, 0xC0FFEE, 24, 0))
        step = {"index": 7}
        datagram = protect_for_scenario(plain, step, dummy,
                                        {"profile": profile})
        vecs.append({"name": name, "profile": profile, "tag_len": tag_len,
                     "master": master, "salt": salt,
                     "enc": derived["enc"], "auth": derived["auth"],
                     "dsalt": derived["salt"],
                     "index": 7, "plain": plain, "datagram": datagram})
    return vecs


def srtp_scenarios():
    """End-to-end pipeline scenarios fed with Python-protected datagrams."""
    out = []
    for tag, profile, master, salt, tag_len in (
            ("srtp_cm128_stream", "cm128", SRTP_MASTER128, SRTP_SALT14, 10),
            ("srtp_cm256_stream", "cm256", SRTP_MASTER256, SRTP_SALT14, 10),
            ("srtp_gcm256_stream", "gcm256", SRTP_MASTER256, SRTP_SALT12, 8)):
        sb = ScenarioBuilder(tag, window=8,
                             srtp={"profile": profile, "master": master,
                                   "salt": salt, "tag_len": tag_len})
        # build two frames as plaintext steps, then convert to raw+datagram
        tmp = ScenarioBuilder("tmp", window=8)
        tmp.frame(1, 1, 2, 5000, [20, 16], keyframe=True)
        tmp.frame(3, 2, 2, 5100, [20, 16])
        rx = Receiver(**sb.cfg)
        setup_srtp(rx, sb.srtp)
        cache = {}
        for sid, step in enumerate(tmp.steps):
            pkt = expand_step(step, cache)
            cache[sid] = pkt
            dat = protect_for_scenario(pkt, step, rx, sb.srtp)
            sb.raw(step["index"], dat, now=step["now"], via_datagram=1)
        out.append(sb)
    return out


# ------------------------------------------------------------- op scripts

def nack_ops():
    """Op-script for the C unit test of kn_rx_nack_* (mirrors
    sc_nack_tracker + the Mjolnir RTT policy)."""
    ops = []
    t = NackTracker(capacity=4)

    def rec(op, a=0, b=0, now=0, rtt=0, kf=None):
        kfl = []
        have, first, last = 0, 0, 0
        if op == 0:
            t.request(a, b, now, kfl)
        elif op == 1:
            got = t.take(now, rtt if rtt else None)
            if got:
                have, (first, last) = 1, got
        elif op == 2:
            have = 1 if t.resolve(a) else 0
        elif op == 3:
            t.clear()
        elif op == 4:
            t.mark_send_failed(a, b)
        kf_out = 1 if kfl else (kf if kf is not None else -1)
        ops.append({"op": op, "a": a, "b": b, "now": now, "rtt": rtt,
                    "exp_have": have, "exp_first": first, "exp_last": last,
                    "exp_kf": kf_out, "exp_count": len(t.pending)})

    rec(0, 10, 12, 0)
    rec(1, now=0)
    rec(1, now=0)
    rec(1, now=4)
    rec(1, now=8)
    rec(1, now=12)
    rec(2, 11, now=12)
    rec(2, 11, now=12)
    rec(3)
    rec(0, 5, 5, 100)
    rec(1, now=100, rtt=30)
    rec(1, now=120, rtt=30)
    rec(1, now=134, rtt=30)
    rec(0, 200, 200, 140)
    rec(1, now=200, rtt=10)          # 52/10 -> 3 attempts, interval 14ms
    rec(1, now=214, rtt=10)
    rec(1, now=228, rtt=10)
    rec(1, now=242, rtt=10)
    # expiry
    rec(0, 300, 300, 1000)
    rec(1, now=1000)
    rec(4, 300, 300, now=1000)       # send failed -> attempts rolled back
    rec(1, now=1004)
    # overflow at capacity 4
    rec(3)
    rec(0, 400, 409, 2000, kf=1)
    # 64-packet batch cap on a big tracker
    ops.append({"op": 5, "a": 0, "b": 0, "now": 0, "rtt": 0,
                "exp_have": 0, "exp_first": 0, "exp_last": 0,
                "exp_kf": -1, "exp_count": 0})   # marker: fresh tracker test
    return ops


def nack_ops_part2():
    """The 64-batch-cap check on a full-size tracker."""
    t = NackTracker(capacity=2048)
    kf = []
    t.request(0, 99, 0, kf)
    got = t.take(0, None)
    return {"first": got[0], "last": got[1]}


def inval_ops():
    """Op-script for the C unit test of kn_rx_inval_* (mirrors
    sc_inval_units exactly)."""
    ops = []
    v = Invalidation()

    def rec(op, now=0, fi=0, wire=0, known=0, keyframe=0, loss=0,
            a=0, b=0):
        kfl = []
        rc = 0
        contig = -1
        if op == 0:                     # lose
            rc = 1 if v.lose(now) else 0
        elif op == 1:                   # admit
            frame = {"frame_index": fi, "frame_type": wire if known else None,
                     "frame_type_parsed": bool(known), "keyframe": keyframe}
            ok = v.admit(frame, bool(loss), now, kfl)
            rc = 1 if ok else 0
            contig = frame.get("contiguous", -1)
            if contig is True:
                contig = 1
            elif contig is False:
                contig = 0
        elif op == 2:                   # poll_backstop
            v.poll_backstop(now, kfl)
        elif op == 3:                   # take
            got = v.take()
            rc = 1 if got else 0
            if got:
                a, b = got
            else:
                a, b = -1, -1
        elif op == 4:                   # restore
            v.restore((a, b))
        elif op == 5:                   # record_sent
            v.record_sent(now)
        elif op == 6:                   # queue
            v.queue((a, b))
        exp_unsent = v.unsent if v.unsent else (-1, -1)
        if op == 3:
            # for take ops the C side compares the TAKEN range through
            # exp_ufirst/exp_ulast (queue state after take is always None)
            exp_unsent = (a, b)
        ops.append({"op": op, "now": now, "fi": fi, "wire": wire,
                    "known": known, "keyframe": keyframe, "loss": loss,
                    "a": a, "b": b,
                    "exp_rc": rc, "exp_contig": contig,
                    "exp_kf": 1 if kfl else 0,
                    "exp_pending": v.pending if v.pending else (-1, -1),
                    "exp_unsent": exp_unsent,
                    "exp_active": 1 if v.active_since is not None else 0,
                    "exp_dropped": v.cnt_dropped,
                    "exp_rec_i": v.cnt_rec_i,
                    "exp_backstops": v.cnt_idr_backstops})

    rec(0, now=10)                                   # unknown types
    rec(1, now=10, fi=1, wire=FT_P, known=1)         # first P admitted
    rec(0, now=20)                                   # lose -> ownership
    rec(1, now=21, fi=3, wire=FT_P, known=1, loss=1) # gap -> withheld
    rec(3, now=21)                                   # take RFI (2,2)
    rec(4, now=21, a=2, b=2)                         # restore after fail
    rec(5, now=30)                                   # record sent
    rec(2, now=120)                                  # retry too early
    rec(2, now=131)                                  # retry >= 100ms
    rec(2, now=269)                                  # backstop not yet
    rec(2, now=270)                                  # backstop fires (250ms)
    rec(2, now=600)                                  # one-shot
    rec(1, now=300, fi=4, wire=FT_I, known=1, keyframe=1, loss=1)
    rec(0, now=400)
    rec(1, now=401, fi=5, wire=FT_HEADER, known=1, loss=1)
    rec(0, now=500)
    rec(1, now=501, fi=6, wire=0, known=0)           # typeless -> kf + clear
    rec(1, now=600, fi=7, wire=FT_INTRA, known=1)    # intra-refresh counter
    return ops


# ----------------------------------------------------------------- emitter

PROFILE_IDS = {"cm128": 1, "cm256": 2, "gcm128": 3, "gcm256": 4}


class Blob(object):
    def __init__(self):
        self.buf = bytearray()

    def add(self, data):
        off = len(self.buf)
        self.buf += bytes(data)
        return off, len(data)


def _map_ev(e):
    if e["t"] == EV_DROPPED:
        return (EV_DROPPED, e.get("r", 0), e.get("d", 0), e.get("i", 0), 0)
    if e["t"] == EV_RECOVERY_GAP:
        return (EV_RECOVERY_GAP, 0, 0, e["a"], e["b"])
    if e["t"] == EV_RECOVERY_TIMEOUT:
        return (EV_RECOVERY_TIMEOUT, 0, 0, e["a"], 0)
    if e["t"] == EV_LIFECYCLE:
        return (EV_LIFECYCLE, 0, 0, e["s"], 0)
    if e["t"] == EV_PROGRESS_RESUMED:
        return (EV_PROGRESS_RESUMED, 0, 0, 0, 0)
    if e["t"] == EV_PROGRESS_STALL:
        return (EV_PROGRESS_STALL, 0, 0, e["a"], 0)
    if e["t"] == EV_PROGRESS_RECOVERY:
        return (EV_PROGRESS_RECOVERY, 0, 0, e["a"], 0)
    raise ValueError(e)


def emit():
    blob = Blob()
    miss = Blob()          # u16 arrays for nack_v2 vectors (byte blob /2)
    steps, exps, evs, frames = [], [], [], []
    scenarios = []

    all_sbs = build_scenarios() + srtp_scenarios()
    for sb in all_sbs:
        trace, counters = run_scenario(sb)
        assert len(trace) == len(sb.steps)
        steps_off = len(steps)
        for sid, (step, exp) in enumerate(zip(sb.steps, trace)):
            ev_off = len(evs)
            for e in exp["events"]:
                evs.append(_map_ev(e))
            fr_off = len(frames)
            for f in exp["frames"]:
                frames.append((f["frame_index"], f["len"], f["hash"],
                               f["keyframe"], f["contiguous"], f["wire"],
                               f["known"], f["first_spi"], f["ts"]))
            st = {k: 0 for k in ()}
            row = {
                "kind": {"data": 0, "parity": 1, "poll": 2, "lifecycle": 3,
                         "nack_take": 4, "kf_take": 5, "rfi_take": 6,
                         "raw": 7}[step["kind"]],
                "now_ms": step["now"],
                "deliver": step.get("deliver", 1),
                "index": step.get("index", 0),
                "seq": step.get("seq", 0), "ts": step.get("ts", 0),
                "ssrc": step.get("ssrc", 0),
                "pt": step.get("pt", 96) if step["kind"] == "data" else 96,
                "marker": step.get("marker", 0),
                "flags": step.get("flags", 0), "spi": step.get("spi", 0),
                "fi": step.get("fi", 0), "cur": step.get("cur", 0),
                "last": step.get("last", 0), "pct": step.get("pct", 0),
                "fidx": step.get("fidx", 0), "fsrc": step.get("fsrc", 0),
                "gs_wire": step.get("gs_wire", -1),
                "nal_type": step.get("nal", 0),
                "body_seed": step.get("seed", 0),
                "body_len": step.get("blen", 0),
                "aud_tail": step.get("aud", 0),
                "d_shards": step.get("d", 0), "p_idx": step.get("p", 0),
                "n_refs": len(step.get("refs", [])),
                "refs": list(step.get("refs", [])) + [0] * 12,
                "shard_len": step.get("shard_len", 0),
                "op": step.get("op", step.get("rtt", 0)),
                "blob_off": -1, "blob_len": 0,
                "via_datagram": step.get("via_datagram", 0),
            }
            if step["kind"] == "raw":
                off, ln = blob.add(step["blob"])
                row["blob_off"], row["blob_len"] = off, ln
            steps.append(row)
            nack_have = 0
            nack_first = nack_last = 0
            if exp["nack"] is not None:
                nack_have = 1
                nack_first, nack_last = exp["nack"]
            rfi_have = 0
            rfi_first = rfi_last = 0
            if exp["rfi"] is not None:
                rfi_have = 1
                rfi_first, rfi_last = exp["rfi"]
            exps.append({
                "n_ev": len(exp["events"]), "ev_off": ev_off,
                "n_frames": len(exp["frames"]), "frames_off": fr_off,
                "nack_have": nack_have, "nack_first": nack_first,
                "nack_last": nack_last,
                "kf_expect": exp["kf"],
                "rfi_have": rfi_have, "rfi_first": rfi_first,
                "rfi_last": rfi_last,
                "state_expect": exp["state"],
            })
        cfg = sb.cfg
        srtp = sb.srtp
        srow = {
            "name": sb.name,
            "codec": cfg["codec"],
            "window": cfg["window"], "au_cap": cfg["au_cap"],
            "vps": cfg["vps"], "timeout_ms": cfg["timeout_ms"],
            "startup_ms": cfg["startup_ms"], "stall_ms": cfg["stall_ms"],
            "grace_ms": cfg["grace_ms"],
            "have_pt": 1 if cfg["expected_pt"] is not None else 0,
            "pt": cfg["expected_pt"] or 0,
            "have_ssrc": 1 if cfg["expected_ssrc"] is not None else 0,
            "ssrc": cfg["expected_ssrc"] or 0,
            "fec_max_blocks": cfg["fec_max_blocks"],
            "fec_max_shards": cfg["fec_max_shards"],
            "nack_capacity": cfg["nack_capacity"],
            "srtp_profile": 0, "srtp_tag_len": 0,
            "master_off": -1, "master_len": 0,
            "salt_off": -1, "salt_len": 0,
            "enc_off": -1, "enc_len": 0, "auth_off": -1, "auth_len": 0,
            "dsalt_off": -1, "dsalt_len": 0,
            "steps_off": steps_off, "n_steps": len(sb.steps),
            "c_auth": counters["auth"], "c_fec_pkts": counters["fec_pkts"],
            "c_fec_rep": counters["fec_rep"], "c_frames": counters["frames"],
            "c_stale": counters["stale"], "c_gaps": counters["gaps"],
            "c_withheld": counters["withheld"],
            "c_kf": counters["kf_req"], "c_retrans": counters["retrans"],
            "c_replay": counters["replay"],
        }
        if srtp:
            dummy = Receiver(window=8)
            setup_srtp(dummy, srtp)
            der = dummy._srtp_derived
            srow["srtp_profile"] = PROFILE_IDS[srtp["profile"]]
            srow["srtp_tag_len"] = srtp["tag_len"]
            srow["master_off"], srow["master_len"] = blob.add(srtp["master"])
            srow["salt_off"], srow["salt_len"] = blob.add(srtp["salt"])
            srow["enc_off"], srow["enc_len"] = blob.add(der["enc"])
            if der["auth"]:
                srow["auth_off"], srow["auth_len"] = blob.add(der["auth"])
            srow["dsalt_off"], srow["dsalt_len"] = blob.add(der["salt"])
        scenarios.append(srow)

    # ---- encoder goldens
    nackv2 = []
    v2_cases = [
        ("single", 0, [0x1234], "17030d0002000134120000000000000000"),
        ("two_records", 7, [0x1234, 0x1235, 0x1274, 0x1275],
         "170317000207023412010000000000008075120000000000000000"),
        ("wrap_grouping", 0, [65534, 65535, 0, 62, 63],
         "17031700020002feff03000000000000803f000000000000000000"),
        ("contiguous_64", 0, list(range(64)),
         "17030d000200010000ffffffffffffff7f"),
    ]
    for name, stream, missing, expect_hex in v2_cases:
        got = nack_v2(stream, missing)
        assert got.hex() == expect_hex, (name, got.hex())
        m_off = len(miss.buf) // 2
        for m in missing:
            miss.buf += m.to_bytes(2, "little")
        e_off, e_len = blob.add(bytes.fromhex(expect_hex))
        nackv2.append((name, stream, len(missing), m_off, e_off, e_len))
    # sparse-64 + rejection cases
    sparse = [(i * 65) & 0xFFFF for i in range(64)]
    got = nack_v2(0, sparse)
    assert len(got) == 647 and got[0:7] == bytes([0x17, 3, 0x83, 2, 2, 0, 64])
    m_off = len(miss.buf) // 2
    for m in sparse:
        miss.buf += m.to_bytes(2, "little")
    e_off, e_len = blob.add(got)
    nackv2.append(("sparse_64", 0, 64, m_off, e_off, e_len))
    nackv2.append(("empty_rejected", 0, 0, 0, -1, 0))
    m65 = len(miss.buf) // 2
    for _ in range(65):
        miss.buf += (0).to_bytes(2, "little")
    nackv2.append(("oversize_rejected", 0, 65, m65, -1, 0))

    rtcp_nacks = []
    for name, first, last, expect_hex in (
            ("wrap_65534_65536", 65534, 65536,
             "81cd00030102030405060708fffe0003"),
            ("single", 100, 100, "81cd00030102030405060708 0064 0000"
             .replace(" ", "")),
            ("sixteen", 1000, 1016,
             "81cd00030102030405060708" "03e8" "ffff"),
            ("two_entries", 1000, 1020,
             "81cd00040102030405060708" "03e8" "ffff" "03f9" "0007"),
            ("cap_64", 0, 200,
             "81cd00060102030405060708" "0000" "ffff" "0011" "ffff"
             "0022" "ffff" "0033" "0fff")):
        got = rtcp_nack(0x01020304, 0x05060708, first, last)
        assert got.hex() == expect_hex.replace(" ", ""), (name, got.hex())
        e_off, e_len = blob.add(got)
        rtcp_nacks.append((name, first, last, e_off, e_len))
    plis = []
    got = rtcp_pli(0x01020304, 0x05060708)
    assert got.hex() == "81ce00020102030405060708"
    e_off, _ = blob.add(got)
    plis.append(("rfc4585", e_off))

    ctrls = []
    got = idr_request()
    assert got.hex() == "020302000000"
    o, l = blob.add(got)
    ctrls.append(("idr", 0, [], 0, 0, o, l))
    got = frame_invalidation_range(5, 9, 2)
    o, l = blob.add(got)
    ctrls.append(("rfi_range", 1, [5, 9, 2], 0, 0, o, l))
    got = frame_ack(7, 1234.5, 99)
    import struct
    fbits = struct.unpack("<Q", struct.pack("<d", 1234.5))[0]
    o, l = blob.add(got)
    ctrls.append(("frame_ack_f", 2, [7, 99], fbits, 1, o, l))
    got = frame_ack(8, None, 0)
    o, l = blob.add(got)
    ctrls.append(("frame_ack_none", 2, [8, 0], 0, 0, o, l))
    got = frame_pacing(12, 16666, 20000)
    o, l = blob.add(got)
    ctrls.append(("pacing_clamped", 3, [12, 16666, 20000], 0, 0, o, l))
    got = frame_pacing(13, 16666, 100)
    o, l = blob.add(got)
    ctrls.append(("pacing_small", 3, [13, 16666, 100], 0, 0, o, l))
    qos_args = [3, 77, 12345, 678, 250, 15, 42, 0x11223344, 2, 4500, 900]
    got = qos_report(3, 77, 12345, 678, 250, 15, 42, 0x11223344, 2, 4500,
                     900)
    o, l = blob.add(got)
    ctrls.append(("qos", 4, qos_args, 0, 0, o, l))
    got = ctrl_encode(0x207, bytes([1, 2, 3]))
    assert got.hex() == "07020300 010203".replace(" ", "")

    # ---- srtp vectors
    srtp_rows = []
    for v in srtp_vectors():
        r = {"name": v["name"], "profile": PROFILE_IDS[v["profile"]],
             "tag_len": v["tag_len"], "index": v["index"]}
        r["master_off"], r["master_len"] = blob.add(v["master"])
        r["salt_off"], r["salt_len"] = blob.add(v["salt"])
        r["enc_off"], r["enc_len"] = blob.add(v["enc"])
        if v["auth"]:
            r["auth_off"], r["auth_len"] = blob.add(v["auth"])
        else:
            r["auth_off"], r["auth_len"] = -1, 0
        r["dsalt_off"], r["dsalt_len"] = blob.add(v["dsalt"])
        r["plain_off"], r["plain_len"] = blob.add(v["plain"])
        r["dgram_off"], r["dgram_len"] = blob.add(v["datagram"])
        srtp_rows.append(r)

    nops = nack_ops()
    part2 = nack_ops_part2()
    iops = inval_ops()

    return render(blob, miss, steps, exps, evs, frames, scenarios, nackv2,
                  rtcp_nacks, plis, ctrls, srtp_rows, nops, part2, iops)


def render(blob, miss, steps, exps, evs, frames, scenarios, nackv2,
           rtcp_nacks, plis, ctrls, srtp_rows, nops, part2, iops):
    L = []
    A = L.append
    A("/* kn_rx_kat_vectors.h -- GENERATED FILE, do not edit by hand.")
    A(" * Source: tools/rx_reference.py (independent Python port of")
    A(" * nvst.rs / nvst_control.rs / nvst_invalidation.rs @ a49b124).")
    A(" * Regenerate: python3 tools/rx_reference.py   (self-proves first).")
    A(" */")
    A("#ifndef KN_RX_KAT_VECTORS_H")
    A("#define KN_RX_KAT_VECTORS_H")
    A("")
    A("#include <stdint.h>")
    A("#include <stddef.h>")
    A("")
    A("/* step kinds */")
    A("enum { KN_RX_KAT_DATA = 0, KN_RX_KAT_PARITY, KN_RX_KAT_POLL,")
    A("       KN_RX_KAT_LIFECYCLE, KN_RX_KAT_NACK_TAKE, KN_RX_KAT_KF_TAKE,")
    A("       KN_RX_KAT_RFI_TAKE, KN_RX_KAT_RAW };")
    A("/* lifecycle ops */")
    A("enum { KN_RX_KAT_PAUSE = 0, KN_RX_KAT_RESUME, KN_RX_KAT_RECOVER,")
    A("       KN_RX_KAT_STOP };")
    A("")
    A("typedef struct {")
    A("    int      kind;")
    A("    uint64_t now_ms;")
    A("    int      deliver;")
    A("    uint64_t index;")
    A("    uint32_t seq, ts, ssrc;")
    A("    unsigned pt, marker;")
    A("    unsigned flags, spi, fi, cur, last, pct, fidx, fsrc;")
    A("    int      gs_wire;           /* -1 = no GS frame header */")
    A("    unsigned nal_type;")
    A("    uint32_t body_seed, body_len;")
    A("    int      aud_tail;")
    A("    unsigned d_shards, p_idx;")
    A("    int      n_refs;")
    A("    const int *refs;            /* data-step ids feeding the fold */")
    A("    uint32_t shard_len;")
    A("    int      op;                /* lifecycle op / nack rtt ms */")
    A("    int      blob_off;          /* RAW: byte range in k_rx_blob */")
    A("    uint32_t blob_len;")
    A("    int      via_datagram;      /* RAW: feed process_datagram */")
    A("} kn_rx_kat_step;")
    A("")
    A("typedef struct { int type, reason, detail; uint64_t a, b; } kn_rx_kat_ev;")
    A("typedef struct {")
    A("    uint32_t frame_index, len;")
    A("    uint64_t hash;              /* first 8 bytes of SHA-1, BE */")
    A("    int keyframe, contiguous, wire, known;")
    A("    uint32_t first_spi, ts;")
    A("} kn_rx_kat_frame;")
    A("typedef struct {")
    A("    int n_ev, ev_off, n_frames, frames_off;")
    A("    int nack_have; uint64_t nack_first, nack_last;")
    A("    int kf_expect;              /* -1 = not a kf_take step */")
    A("    int rfi_have; uint64_t rfi_first, rfi_last;")
    A("    int state_expect;           /* -1 = don't check */")
    A("} kn_rx_kat_step_exp;")
    A("typedef struct {")
    A("    const char *name;")
    A("    int codec;")
    A("    uint32_t window, au_cap, vps, timeout_ms, startup_ms, stall_ms,")
    A("             grace_ms;")
    A("    int have_pt; unsigned pt;")
    A("    int have_ssrc; uint32_t ssrc;")
    A("    uint32_t fec_max_blocks, fec_max_shards, nack_capacity;")
    A("    int srtp_profile;           /* 0 none,1 cm128,2 cm256,3 gcm128,4 gcm256 */")
    A("    int srtp_tag_len;")
    A("    int master_off, master_len, salt_off, salt_len;")
    A("    int enc_off, enc_len, auth_off, auth_len, dsalt_off, dsalt_len;")
    A("    int steps_off, n_steps;")
    A("    uint64_t c_auth, c_fec_pkts, c_fec_rep, c_frames, c_stale, c_gaps,")
    A("             c_withheld, c_kf, c_retrans, c_replay;")
    A("} kn_rx_kat_scenario;")
    A("")
    for i, r in enumerate(steps):
        A("static const int k_step_refs%d[12] = { %s };"
          % (i, ", ".join(str(x) for x in r["refs"][:12])))
    A("")
    A("static const kn_rx_kat_step k_rx_steps[] = {")
    for i, r in enumerate(steps):
        A("    { %d, %dULL, %d, %dULL, %uu, %uu, %uu, %uu, %uu, %uu, %uu, "
          "%uu, %uu, %uu, %uu, %uu, %uu, %d, %uu, %uu, %uu, %d, %uu, %uu, "
          "%d, k_step_refs%d, %uu, %d, %d, %uu, %d },"
          % (r["kind"], r["now_ms"], r["deliver"], r["index"],
             r["seq"], r["ts"], r["ssrc"], r["pt"], r["marker"],
             r["flags"], r["spi"], r["fi"], r["cur"], r["last"],
             r["pct"], r["fidx"], r["fsrc"], r["gs_wire"], r["nal_type"],
             r["body_seed"], r["body_len"], r["aud_tail"],
             r["d_shards"], r["p_idx"], r["n_refs"], i, r["shard_len"],
             r["op"], r["blob_off"], r["blob_len"], r["via_datagram"]))
    A("};")
    A("#define K_RX_STEPS_N %d" % len(steps))
    A("")
    A("static const kn_rx_kat_ev k_rx_evs[] = {")
    for t, r, d, a, b in evs:
        A("    { %d, %d, %d, %dULL, %dULL }," % (t, r, d, a, b))
    if not evs:
        A("    { 0, 0, 0, 0ULL, 0ULL }")
    A("};")
    A("")
    A("static const kn_rx_kat_frame k_rx_frames[] = {")
    for f in frames:
        A("    { %uu, %uu, 0x%016xULL, %d, %d, %d, %d, %uu, %uu },"
          % (f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]))
    if not frames:
        A("    { 0u, 0u, 0ULL, 0, 0, 0, 0, 0u, 0u }")
    A("};")
    A("")
    A("static const kn_rx_kat_step_exp k_rx_exps[] = {")
    for e in exps:
        A("    { %d, %d, %d, %d, %d, %dULL, %dULL, %d, %d, %dULL, %dULL, "
          "%d },"
          % (e["n_ev"], e["ev_off"], e["n_frames"], e["frames_off"],
             e["nack_have"], e["nack_first"], e["nack_last"],
             e["kf_expect"], e["rfi_have"], e["rfi_first"], e["rfi_last"],
             e["state_expect"]))
    A("};")
    A("")
    A("static const kn_rx_kat_scenario k_rx_scenarios[] = {")
    for s in scenarios:
        A('    { "%s", %d, %uu, %uu, %uu, %uu, %uu, %uu, %uu, %d, %uu, '
          '%d, %uu, %uu, %uu, %uu, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, '
          '%d, %d, %d, %d, %dULL, %dULL, %dULL, %dULL, %dULL, %dULL, '
          '%dULL, %dULL, %dULL, %dULL },'
          % (s["name"], s["codec"], s["window"], s["au_cap"], s["vps"],
             s["timeout_ms"], s["startup_ms"], s["stall_ms"], s["grace_ms"],
             s["have_pt"], s["pt"], s["have_ssrc"], s["ssrc"],
             s["fec_max_blocks"], s["fec_max_shards"], s["nack_capacity"],
             s["srtp_profile"], s["srtp_tag_len"],
             s["master_off"], s["master_len"], s["salt_off"], s["salt_len"],
             s["enc_off"], s["enc_len"], s["auth_off"], s["auth_len"],
             s["dsalt_off"], s["dsalt_len"],
             s["steps_off"], s["n_steps"],
             s["c_auth"], s["c_fec_pkts"], s["c_fec_rep"], s["c_frames"],
             s["c_stale"], s["c_gaps"], s["c_withheld"], s["c_kf"],
             s["c_retrans"], s["c_replay"]))
    A("};")
    A("#define K_RX_SCENARIOS_N %d" % len(scenarios))
    A("")
    # nack ops
    A("typedef struct {")
    A("    int op;   /* 0 request,1 take,2 resolve,3 clear,4 mark_failed,")
    A("               5 marker: fresh 2048-tracker, request 0..99, take */")
    A("    uint64_t a, b, now;")
    A("    unsigned rtt;")
    A("    int exp_have; uint64_t exp_first, exp_last;")
    A("    int exp_kf; uint32_t exp_count;")
    A("} kn_rx_kat_nack_op;")
    A("static const kn_rx_kat_nack_op k_rx_nack_ops[] = {")
    for o in nops:
        A("    { %d, %dULL, %dULL, %dULL, %uu, %d, %dULL, %dULL, %d, %uu },"
          % (o["op"], o["a"], o["b"], o["now"], o["rtt"], o["exp_have"],
             o["exp_first"], o["exp_last"], o["exp_kf"], o["exp_count"]))
    A("};")
    A("#define K_RX_NACK_OPS_N %d" % len(nops))
    A("#define K_RX_NACK_OP5_FIRST %dULL" % part2["first"])
    A("#define K_RX_NACK_OP5_LAST  %dULL" % part2["last"])
    A("")
    # inval ops
    A("typedef struct {")
    A("    int op;   /* 0 lose,1 admit,2 backstop,3 take,4 restore,")
    A("               5 record_sent,6 queue */")
    A("    uint64_t now; uint32_t fi;")
    A("    int wire, known, keyframe, loss;")
    A("    uint64_t a, b;")
    A("    int exp_rc, exp_contig, exp_kf;")
    A("    int64_t exp_pfirst, exp_plast, exp_ufirst, exp_ulast;")
    A("    int exp_active;")
    A("    uint64_t exp_dropped, exp_rec_i, exp_backstops;")
    A("} kn_rx_kat_inval_op;")
    A("static const kn_rx_kat_inval_op k_rx_inval_ops[] = {")
    for o in iops:
        pf, pl = o["exp_pending"]
        uf, ul = o["exp_unsent"]
        A("    { %d, %dULL, %uu, %d, %d, %d, %d, %dULL, %dULL, %d, %d, %d, "
          "%dLL, %dLL, %dLL, %dLL, %d, %dULL, %dULL, %dULL },"
          % (o["op"], o["now"], o["fi"], o["wire"], o["known"], o["keyframe"],
             o["loss"], o.get("a", 0), o.get("b", 0), o["exp_rc"],
             o["exp_contig"], o["exp_kf"], pf, pl, uf, ul, o["exp_active"],
             o["exp_dropped"], o["exp_rec_i"], o["exp_backstops"]))
    A("};")
    A("#define K_RX_INVAL_OPS_N %d" % len(iops))
    A("")
    # srtp vectors
    A("typedef struct {")
    A("    const char *name; int profile, tag_len;")
    A("    int master_off, master_len, salt_off, salt_len;")
    A("    int enc_off, enc_len, auth_off, auth_len, dsalt_off, dsalt_len;")
    A("    uint64_t index;")
    A("    int plain_off, plain_len, dgram_off, dgram_len;")
    A("} kn_rx_kat_srtp;")
    A("static const kn_rx_kat_srtp k_rx_srtp_vecs[] = {")
    for r in srtp_rows:
        A('    { "%s", %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, '
          '%dULL, %d, %d, %d, %d },'
          % (r["name"], r["profile"], r["tag_len"], r["master_off"],
             r["master_len"], r["salt_off"], r["salt_len"], r["enc_off"],
             r["enc_len"], r["auth_off"], r["auth_len"], r["dsalt_off"],
             r["dsalt_len"], r["index"], r["plain_off"], r["plain_len"],
             r["dgram_off"], r["dgram_len"]))
    A("};")
    A("#define K_RX_SRTP_VECS_N %d" % len(srtp_rows))
    A("")
    # encoder goldens
    A("typedef struct { const char *name; unsigned stream; int n;")
    A("    int miss_off, exp_off, exp_len; } kn_rx_kat_nackv2;")
    A("static const kn_rx_kat_nackv2 k_rx_nackv2[] = {")
    for name, stream, n, m_off, e_off, e_len in nackv2:
        A('    { "%s", %uu, %d, %d, %d, %d },'
          % (name, stream, n, m_off, e_off, e_len))
    A("};")
    A("#define K_RX_NACKV2_N %d" % len(nackv2))
    A("")
    A("typedef struct { const char *name; uint64_t first, last;")
    A("    int exp_off, exp_len; } kn_rx_kat_rtcp_nack;")
    A("static const kn_rx_kat_rtcp_nack k_rx_rtcp_nacks[] = {")
    for name, first, last, e_off, e_len in rtcp_nacks:
        A('    { "%s", %dULL, %dULL, %d, %d },' % (name, first, last,
                                                   e_off, e_len))
    A("};")
    A("#define K_RX_RTCP_NACKS_N %d" % len(rtcp_nacks))
    A("#define K_RX_RTCP_PLI_OFF %d" % plis[0][1])
    A("")
    A("typedef struct { const char *name; int kind;")
    A("    uint64_t args[12]; int nargs; uint64_t fbits; int have_f;")
    A("    int exp_off, exp_len; } kn_rx_kat_ctrl;")
    A("static const kn_rx_kat_ctrl k_rx_ctrls[] = {")
    for name, kind, args, fbits, have_f, e_off, e_len in ctrls:
        argl = list(args) + [0] * (12 - len(args))
        A('    { "%s", %d, { %s }, %d, 0x%016xULL, %d, %d, %d },'
          % (name, kind, ", ".join("%dULL" % a for a in argl), len(args),
             fbits, have_f, e_off, e_len))
    A("};")
    A("#define K_RX_CTRLS_N %d" % len(ctrls))
    A("")
    # blobs
    A("static const uint16_t k_rx_miss[] = {")
    mb = bytes(miss.buf)
    for i in range(0, len(mb), 2):
        pass
    vals = [int.from_bytes(mb[i:i + 2], "little")
            for i in range(0, len(mb), 2)]
    row = []
    for v in vals:
        row.append(str(v))
        if len(row) == 16:
            A("    " + ", ".join(row) + ",")
            row = []
    if row:
        A("    " + ", ".join(row) + ",")
    if not vals:
        A("    0")
    A("};")
    A("")
    A("static const unsigned char k_rx_blob[] = {")
    for i in range(0, len(blob.buf), 16):
        chunk = blob.buf[i:i + 16]
        A("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    A("};")
    A("#define K_RX_BLOB_LEN %d" % len(blob.buf))
    A("")
    A("#endif /* KN_RX_KAT_VECTORS_H */")
    return "\n".join(L) + "\n"


def main():
    sc_builder_identity()
    sc_encoders()
    sc_crypto()
    sc_reorder_deadline()
    sc_fec_unit()
    sc_pipeline_gaps()
    sc_nack_exclusion()
    sc_inval_units()
    sc_nack_tracker()
    sc_timeout_watchdog()
    sc_scenario_invariants()
    if _FAILS:
        print("SELF-CHECK FAILED (%d) — nothing emitted" % _FAILS)
        sys.exit(1)
    text = emit()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("written: %s (%d bytes, self-check %d/%d)"
          % (OUT, len(text), _CHECKS - _FAILS, _CHECKS))


if __name__ == "__main__":
    main()
