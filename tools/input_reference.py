#!/usr/bin/env python3
"""input_reference.py — the INDEPENDENT reference generator for the NVST
control/input plane (round XXXVII).

WHY THIS FILE EXISTS (the two-implementations rule): the input activation
chain is the one thing runs 16 AND 17 both lacked. Run 17 proved the
receiver-feedback set is NOT the stall cause (RR/QoS/keepalive flew the
whole 242 s, dropped:0 — video still died at exactly 10.99 s / 331 AUs,
vs 10.2 s with no feedback at all). The reference sends eight control
messages on sid 0 the instant the control channel opens and INPUT_VERSION
arrives (nvst.rs:6554-6591 finish_nvst_input_handshake ->
nvst_input.rs:1318-1329 activation_chain), and its own comment says an
all-zero window state "leaves the session looking INACTIVE". This script
is the independent Python model of that chain: it self-proves against the
reference's byte-exact Rust goldens (nvst_input.rs:1768-1800) and the
parser vectors (nvst_input.rs:1698-1712, including run 17's LIVE bytes
0e0202000300 -> version 3) BEFORE emitting the C KAT header.

Regeneration is byte-identical. Run:  python3 tools/input_reference.py
"""

from __future__ import annotations

import os
import struct
import sys

# ---------------------------------------------------------------- constants
# nvst_input.rs:11-12, nvst_cursor.rs:3-5
CONTROL_KEEPALIVE_INTERVAL_MS = 3000
INPUT_HANDSHAKE_TIMEOUT_MS = 5000
CURSOR_RETRY_INTERVAL_MS = 250
CURSOR_MAX_ATTEMPTS = 8
CURSOR_SILENT_HOST_TIMEOUT_MS = 3000

# nvst_input.rs:25-42
COMMAND_SYSTEM_CURSOR = 0x010F
COMMAND_BITMAP_CURSOR = 0x0110
COMMAND_KEEPALIVE = 0x0200
COMMAND_REMOTE_INPUT = 0x0206
COMMAND_ENABLE_INPUT = 0x020B
COMMAND_GAMEPAD = 0x020D
COMMAND_INPUT_VERSION = 0x020E
COMMAND_MOUSE_CURSOR_CAPTURE = 0x0308
COMMAND_TRACK_REMOTE_CURSOR_IMAGE = 0x030D
COMMAND_WINDOW_STATE = 0x0320
COMMAND_SYSTEM_STATE = 0x0321

# nvst_input.rs:44-56 (native input types)
INPUT_KEY_DOWN = 3
INPUT_KEY_UP = 4
INPUT_MOUSE_ABSOLUTE = 5
INPUT_MOUSE_RELATIVE = 7
INPUT_HAPTICS_ENABLED = 13

# the descriptor body template, nvst_input.rs:1358-1371 (39 bytes; the
# bitmap overwrites body[9..11])
DEVICE_DESCRIPTOR_BODY = bytes(
    [
        0x22, 0x0C, 0x00, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x14,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x55, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ]
)

COMMAND_NAMES = {
    COMMAND_SYSTEM_CURSOR: "system-cursor",
    COMMAND_BITMAP_CURSOR: "bitmap-cursor",
    COMMAND_KEEPALIVE: "keepalive",
    COMMAND_REMOTE_INPUT: "remote-input",
    COMMAND_ENABLE_INPUT: "enable-input",
    COMMAND_GAMEPAD: "gamepad-descriptor",
    COMMAND_INPUT_VERSION: "input-version",
    COMMAND_MOUSE_CURSOR_CAPTURE: "mouse-cursor-capture",
    COMMAND_TRACK_REMOTE_CURSOR_IMAGE: "track-remote-cursor",
    COMMAND_WINDOW_STATE: "window-state",
    COMMAND_SYSTEM_STATE: "system-state",
    0x0301: "frame-invalidation",
    0x0302: "idr-request",
    0x0317: "nack-v2",
    0x0203: "frame-pacing",
    0x0204: "frame-ack",
    0x0207: "qos-report",
}


# ---------------------------------------------------------------- builders
def control_command(code: int, payload: bytes) -> bytes:
    """nvst_input.rs:1385-1391 — code LE u16, payload length LE u16, body."""
    return struct.pack("<HH", code, len(payload)) + payload


def control_keepalive(stream_value: int) -> bytes:
    """nvst_input.rs:1346-1348."""
    return control_command(COMMAND_KEEPALIVE, struct.pack("<I", stream_value))


def enable_input(counter: int, enabled: bool) -> bytes:
    """nvst_input.rs:1350-1357 — stream index, counter, enabled (3x LE u32)."""
    payload = struct.pack("<III", 0, counter, 1 if enabled else 0)
    return control_command(COMMAND_ENABLE_INPUT, payload)


def device_descriptor(timestamp_us: int, bitmap: int) -> bytes:
    """nvst_input.rs:1358-1371 — 0x23, timestamp BE u64, 39-byte body."""
    body = bytearray(DEVICE_DESCRIPTOR_BODY)
    body[9:11] = struct.pack("<H", bitmap)
    payload = bytes([0x23]) + struct.pack(">Q", timestamp_us) + bytes(body)
    return control_command(COMMAND_GAMEPAD, payload)


def mouse_cursor_capture(enabled: bool) -> bytes:
    """nvst_input.rs:1331-1333."""
    return control_command(COMMAND_MOUSE_CURSOR_CAPTURE,
                           bytes([1 if enabled else 0]))


def remote_cursor_tracking(enabled: bool) -> bytes:
    """nvst_input.rs:1335-1337."""
    return control_command(COMMAND_TRACK_REMOTE_CURSOR_IMAGE,
                           bytes([1 if enabled else 0]))


def state_change(code: int, state: int, frame_number: int) -> bytes:
    """nvst_input.rs:1372-1383 — stream index, state, frame (3x LE u32)."""
    payload = struct.pack("<III", 0, state, frame_number)
    return control_command(code, payload)


def remote_input_packet(input_type: int, body: bytes) -> bytes:
    """nvst_input.rs:989-995 — BE u32 (4 + body length), type LE u32, body."""
    return struct.pack(">I", 4 + len(body)) + struct.pack("<I", input_type) + body


def remote_input_message(inner: bytes, timestamp_us: int) -> bytes:
    """nvst_input.rs:956-987 — pad to 24/8k, append N LE-u64 timestamps,
    wrap in remote_input_packet(0x0e, ...) then control_command(0x0206)."""
    input_type = struct.unpack_from("<I", inner, 4)[0]
    body = bytearray(inner)
    padded = max(24, -(-len(body) // 8) * 8)
    body.extend(b"\x00" * (padded - len(body)))
    timestamp_count = (
        1 if input_type in (INPUT_KEY_DOWN, INPUT_KEY_UP, INPUT_MOUSE_ABSOLUTE)
        else 0
    ) + 1
    for _ in range(timestamp_count):
        body.extend(struct.pack("<Q", timestamp_us))
    return control_command(COMMAND_REMOTE_INPUT,
                           remote_input_packet(0x0E, bytes(body)))


def mouse_relative(dx: int, dy: int, timestamp_us: int) -> bytes:
    """nvst_input.rs:727-741 — INPUT_MOUSE_RELATIVE(7): the body is
    native[4..8] (dx BE i16, dy BE i16) plus two zero bytes; the
    timestamp is read BE u64 from native[14..22] and appended LE u64 by
    remote_input_message (7 is not KEY_DOWN/KEY_UP/MOUSE_ABSOLUTE, so
    timestamp_count = 1). Total 44 bytes. Route: ControlPartial
    (nvst_input.rs:969) = sid 6, unordered, PR-SCTP TTL 300 ms
    (NVST_CHANNEL_PROFILE[3]). Rust golden: nvst_input.rs:1888-1911
    (keyboard_and_mouse_translate_to_byte_exact_control_commands)."""
    inner = remote_input_packet(INPUT_MOUSE_RELATIVE,
                                struct.pack(">hh", dx, dy) + b"\x00\x00")
    return remote_input_message(inner, timestamp_us)


def haptics_state(enabled: bool, timestamp_us: int) -> bytes:
    """nvst_input.rs:1339-1344."""
    inner = remote_input_packet(INPUT_HAPTICS_ENABLED,
                                struct.pack("<H", 1 if enabled else 0))
    return remote_input_message(inner, timestamp_us)


def activation_chain(timestamp_us: int) -> list[bytes]:
    """nvst_input.rs:1318-1329 — the eight messages, in order."""
    return [
        enable_input(1, False),
        device_descriptor(timestamp_us, 0),
        mouse_cursor_capture(True),
        remote_cursor_tracking(True),
        state_change(COMMAND_WINDOW_STATE, 19, 0),
        state_change(COMMAND_SYSTEM_STATE, 0, 0),
        enable_input(1, True),
        haptics_state(True, timestamp_us),
    ]


# ---------------------------------------------------------------- parsers
def scan_commands(data: bytes) -> list[tuple[int, bytes]]:
    """The TLV list walk of nvst_input.rs:404-424 (first loop only)."""
    out: list[tuple[int, bytes]] = []
    cursor = 0
    while len(data) - cursor >= 4:
        code = struct.unpack_from("<H", data, cursor)[0]
        plen = struct.unpack_from("<H", data, cursor + 2)[0]
        start = cursor + 4
        end = start + plen
        if end > len(data):
            break
        out.append((code, data[start:end]))
        cursor = end
    return out


def input_protocol_version(data: bytes):
    """nvst_input.rs:404-441 — EXACT semantics, both fallbacks included."""
    if len(data) < 2:
        return None
    for code, payload in scan_commands(data):
        if code == COMMAND_INPUT_VERSION and len(payload) >= 2:
            return struct.unpack_from("<H", payload, 0)[0]
    first = struct.unpack_from("<H", data, 0)[0]
    if first == COMMAND_INPUT_VERSION:
        return struct.unpack_from("<H", data, 2)[0] if len(data) >= 4 else 2
    if data[0] == 0x0E:
        return first
    return None


def has_cursor_notification(data: bytes) -> bool:
    """nvst.rs:8322-8326 — a system/bitmap cursor command in the TLV list
    drives cursor_capture.notify() (the enable->disable handoff)."""
    return any(code in (COMMAND_SYSTEM_CURSOR, COMMAND_BITMAP_CURSOR)
               for code, _ in scan_commands(data))


# ---------------------------------------------------------------- self-proof
GOLDEN_TS_US = 20_102_193          # the Rust test's timestamp
GOLDEN_HEX = [                     # nvst_input.rs:1780-1799, verbatim
    "0b020c00000000000100000000000000",
    "0d02300023000000000132bc31220c0000001a000000000014000000000000000000"
    "000000000000550000000000000000000000",
    "0803010001",
    "0d03010001",
    "20030c00000000001300000000000000",
    "21030c00000000000000000000000000",
    "0b020c00000000000100000001000000",
    "06022800000000240e000000000000060d00000001000000000000000000000000"
    "00000031bc320100000000",
]

VERSION_VECTORS = [                # nvst_input.rs:1699-1712, verbatim
    ("0e0202000300", 3),           # RUN 17's LIVE rig bytes (sid 0, ppi 53)
    ("0e020300", 3),
    ("0e03", 0x030E),
    ("0e02", 2),
    ("01", None),
    ("01010000" "0e0202000300", 3),
    # the truncated-tail fallback (declared length overruns the buffer ->
    # the walk yields nothing -> fallback #2 reads data[2..4] verbatim)
    ("0e02ff0003", 0x00FF),
]

CURSOR_VISIBLE = "0f010900010000000c80168001"   # nvst_input.rs:1743

# has_cursor_notification() vectors: (hex, expected). The bitmap entry is
# the EXACT byte list of the reference test at nvst_cursor.rs:186-189.
CURSOR_VECTORS = [
    (CURSOR_VISIBLE, 1),                       # 0x010f system cursor
    ("10010800deadbeef00000000", 1),           # 0x0110 bitmap cursor
    ("0e0202000300", 0),                       # run 17's INPUT_VERSION
    ("0b020c00000000000100000000000000", 0),   # our own enable-input
    ("0803010001", 0),                         # our own capture(true)
    ("0f01ff0001", 0),                         # truncated tail
    ("", 0),                                   # empty
]


def self_prove() -> None:
    """Fail loudly before emitting anything (the vectors-are-trusted rule)."""
    assert control_keepalive(0).hex() == "0002040000000000", "keepalive golden"
    assert mouse_cursor_capture(False).hex() == "0803010000"
    assert mouse_cursor_capture(True).hex() == "0803010001"
    assert remote_cursor_tracking(False).hex() == "0d03010000"
    assert remote_cursor_tracking(True).hex() == "0d03010001"

    chain = activation_chain(GOLDEN_TS_US)
    assert len(chain) == 8, "activation chain length"
    for i, (got, want) in enumerate(zip(chain, GOLDEN_HEX)):
        assert got.hex() == want, f"chain[{i}] {got.hex()} != {want}"

    # structural invariants the C port must also hold
    assert len(chain[1]) == 52, "device_descriptor total length"   # 4 + 48
    assert len(chain[7]) == 44, "haptics total length"             # 4 + 40
    assert all(len(m) <= 64 for m in chain), "KN_INPUT_CMD_MAX"
    # the bitmap reaches body[9..11] of the descriptor (payload offset 10)
    d1 = device_descriptor(GOLDEN_TS_US, 0x1234)
    assert d1[4 + 1 + 8 + 9: 4 + 1 + 8 + 11] == bytes([0x34, 0x12]), "bitmap"

    # round XLI.1 — the reference's OWN byte-exact relative-mouse golden,
    # from keyboard_and_mouse_translate_to_byte_exact_control_commands
    # (nvst_input.rs:1888-1911): dx=24, dy=24, ts=0x01861330 us.
    rust_mouse = mouse_relative(24, 24, 0x01861330)
    assert rust_mouse.hex() == (
        "06022800000000240e0000000000000a0700000000180018"
        "0000000000000000000000003013860100000000"
    ), "rust relative-mouse golden"
    assert len(rust_mouse) == 44, "relative mouse total length"  # 4 + 40
    # wire offsets the C port must match: dx BE i16 at [20..22], dy at
    # [22..24], timestamp LE u64 at [36..44]
    neg = mouse_relative(-1, -2, 0x0102030405060708)
    assert neg[20:24] == b"\xff\xff\xff\xfe", "negative deltas BE i16"
    assert neg[36:44] == struct.pack("<Q", 0x0102030405060708), "ts LE u64"
    ext = mouse_relative(-32768, 32767, 1)
    assert ext[20:24] == b"\x80\x00\x7f\xff", "i16 extremes"

    for hexed, want in VERSION_VECTORS:
        got = input_protocol_version(bytes.fromhex(hexed))
        assert got == want, f"version({hexed}) = {got}, want {want}"

    for hexed, want in CURSOR_VECTORS:
        raw = bytes.fromhex(hexed) if hexed else b""
        got = int(has_cursor_notification(raw))
        assert got == want, f"cursor({hexed}) = {got}, want {want}"
    assert not has_cursor_notification(chain[0]), "no false cursor notify"

    # The TLV walk must tolerate a truncated tail (nvst_input.rs:415-417):
    # the declared length overruns the buffer, so the walk yields nothing
    # and the version falls through to the SECOND fallback, which reads
    # data[2..4] verbatim (0x00ff here) — NOT the command code. A port
    # that "helpfully" returned the code instead would be wrong.
    assert scan_commands(bytes.fromhex("0e02ff00" "03")) == []
    assert input_protocol_version(bytes.fromhex("0e02ff0003")) == 0x00FF

    names = sorted(set(COMMAND_NAMES.values()))
    assert len(names) == len(COMMAND_NAMES), "duplicate command name"
    print(f"self-proof OK: 8/8 activation goldens + 1/1 relative-mouse "
          f"golden byte-exact at ts={GOLDEN_TS_US}, "
          f"{len(VERSION_VECTORS)} parser vectors, "
          f"{len(CURSOR_VECTORS)} cursor vectors, "
          f"{len(COMMAND_NAMES)} named commands")


# ---------------------------------------------------------------- emission
HEADER = """/* kn_input_kat_vectors.h — GENERATED by tools/input_reference.py.
 * Do not edit. The Python reference self-proves against the reference
 * client's OWN byte-exact Rust goldens (nvst_input.rs:1768-1800) and its
 * parser vectors (nvst_input.rs:1698-1712, including run 17's live rig
 * bytes) BEFORE emitting this file. Regeneration is byte-identical.
 */
#ifndef KN_INPUT_KAT_VECTORS_H
#define KN_INPUT_KAT_VECTORS_H

#include <stddef.h>
#include <stdint.h>

/* The activation-chain timestamp the reference test uses. */
#define KN_INPUT_KAT_TS_US {ts}ULL

/* One byte-exact control message (code + payload, TLV-framed). */
typedef struct {{
    uint16_t        code;
    const uint8_t  *bytes;
    size_t          len;
    const char     *what;
}} kn_input_cmd_kat;

/* The eight activation-chain messages, in send order. */
extern const kn_input_cmd_kat kn_input_kat_activation[8];

/* Standalone single-message goldens. */
extern const kn_input_cmd_kat kn_input_kat_singles[];
extern const size_t kn_input_kat_singles_n;

/* input_protocol_version() vectors: input bytes -> expected version
 * (-1 == the reference's None). */
typedef struct {{
    const uint8_t  *bytes;
    size_t          len;
    int             want;
}} kn_input_ver_kat;
extern const kn_input_ver_kat kn_input_kat_versions[];
extern const size_t kn_input_kat_versions_n;

/* Server-cursor notification detection vectors (the enable->disable
 * handoff trigger). */
typedef struct {{
    const uint8_t  *bytes;
    size_t          len;
    int             want;
}} kn_input_cursor_kat;
extern const kn_input_cursor_kat kn_input_kat_cursor[];
extern const size_t kn_input_kat_cursor_n;

#endif /* KN_INPUT_KAT_VECTORS_H */
"""


def _c_bytes(name: str, data: bytes) -> str:
    body = ", ".join(f"0x{b:02x}" for b in data)
    return f"static const uint8_t {name}[] = {{ {body} }};\n"


def emit(path: str) -> None:
    out: list[str] = [HEADER.format(ts=GOLDEN_TS_US), "\n"]

    chain = activation_chain(GOLDEN_TS_US)
    for i, msg in enumerate(chain):
        out.append(_c_bytes(f"kat_act_{i}", msg))
    singles = {
        "keepalive0": control_keepalive(0),
        "capture_on": mouse_cursor_capture(True),
        "capture_off": mouse_cursor_capture(False),
        "tracking_on": remote_cursor_tracking(True),
        "tracking_off": remote_cursor_tracking(False),
        "window19": state_change(COMMAND_WINDOW_STATE, 19, 0),
        "system0": state_change(COMMAND_SYSTEM_STATE, 0, 0),
        "enable_off": enable_input(1, False),
        "enable_on": enable_input(1, True),
        "haptics": haptics_state(True, GOLDEN_TS_US),
        "descriptor": device_descriptor(GOLDEN_TS_US, 0),
        "mouse_rel": mouse_relative(24, 24, GOLDEN_TS_US),
        "mouse_rel_neg": mouse_relative(-5, 3, GOLDEN_TS_US),
    }
    for key, msg in singles.items():
        out.append(_c_bytes(f"kat_{key}", msg))
    out.append("\n")

    out.append("const kn_input_cmd_kat kn_input_kat_activation[8] = {\n")
    labels = [
        "enable-input(counter=1, off)",
        "gamepad device descriptor (bitmap 0)",
        "mouse-cursor-capture(true)",
        "track-remote-cursor(true)",
        "window-state 19 frame 0",
        "system-state 0 frame 0",
        "enable-input(counter=1, on)",
        "remote-input haptics-enabled(true)",
    ]
    codes = [COMMAND_ENABLE_INPUT, COMMAND_GAMEPAD,
             COMMAND_MOUSE_CURSOR_CAPTURE, COMMAND_TRACK_REMOTE_CURSOR_IMAGE,
             COMMAND_WINDOW_STATE, COMMAND_SYSTEM_STATE, COMMAND_ENABLE_INPUT,
             COMMAND_REMOTE_INPUT]
    for i in range(8):
        out.append(f'    {{ 0x{codes[i]:04x}, kat_act_{i}, sizeof kat_act_{i},'
                   f' "{labels[i]}" }},\n')
    out.append("};\n\n")

    out.append("const kn_input_cmd_kat kn_input_kat_singles[] = {\n")
    for key, msg in singles.items():
        code = struct.unpack_from("<H", msg, 0)[0]
        out.append(f'    {{ 0x{code:04x}, kat_{key}, sizeof kat_{key},'
                   f' "{key}" }},\n')
    out.append("};\n")
    out.append("const size_t kn_input_kat_singles_n =\n"
               f"    sizeof kn_input_kat_singles / sizeof kn_input_kat_singles[0];\n\n")

    for i, (hexed, _) in enumerate(VERSION_VECTORS):
        out.append(_c_bytes(f"kat_ver_{i}", bytes.fromhex(hexed)))
    out.append("\nconst kn_input_ver_kat kn_input_kat_versions[] = {\n")
    for i, (_, want) in enumerate(VERSION_VECTORS):
        w = "-1" if want is None else str(want)
        out.append(f"    {{ kat_ver_{i}, sizeof kat_ver_{i}, {w} }},\n")
    out.append("};\n")
    out.append("const size_t kn_input_kat_versions_n =\n"
               "    sizeof kn_input_kat_versions / sizeof kn_input_kat_versions[0];\n\n")

    cursor_vecs = CURSOR_VECTORS
    for i, (hexed, _) in enumerate(cursor_vecs):
        raw = bytes.fromhex(hexed) if hexed else b"\x00"
        out.append(_c_bytes(f"kat_cur_{i}", raw))
    out.append("\nconst kn_input_cursor_kat kn_input_kat_cursor[] = {\n")
    for i, (hexed, want) in enumerate(cursor_vecs):
        n = len(bytes.fromhex(hexed)) if hexed else 0
        out.append(f"    {{ kat_cur_{i}, {n}, {want} }},\n")
    out.append("};\n")
    out.append("const size_t kn_input_kat_cursor_n =\n"
               "    sizeof kn_input_kat_cursor / sizeof kn_input_kat_cursor[0];\n")

    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(out))
    print(f"wrote {path} ({os.path.getsize(path)} bytes)")


def main() -> int:
    self_prove()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    emit(os.path.join(root, "streamer", "src", "kn_input_kat_vectors.h"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
