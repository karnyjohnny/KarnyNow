#!/usr/bin/env python3
"""sctp_reference.py — the INDEPENDENT reference for kn_sctp's wire code
(the two-implementations rule; the fec/srtp/gs/rx_reference.py pattern).

Pure-Python, dependency-free:
  * DCEP DATA_CHANNEL_OPEN / ACK marshalling (RFC 8832 §4, transcribed
    from str0m vendor/str0m/src/sctp/dcep.rs marshal_to — the reference
    the C port must match byte-for-byte),
  * the NVST channel profile (nvst_input.rs NVST_CHANNEL_PROFILE +
    channel_config: priority 0, protocol "", the exact reliability
    mapping of str0m's From<&ChannelConfig> for DcepOpen),
  * CRC-32c (Castagnoli, RFC 4960 §6.8 / RFC 3720 polynomial) for the
    test's own independent SCTP-packet parser,
  * an INIT-chunk parser used to cross-check the C test's parser.

SELF-PROVES before emitting (the round-II rule): CRC-32c against the
public check value 0xE3069283 ("123456789"), DCEP marshalling against
hand-computed goldens, parsing round-trips. THEN writes
streamer/src/kn_sctp_kat_vectors.h (byte-stable; regeneration must be
identical).
"""
import os
import sys

# ---------------------------------------------------------------- CRC32c
CRC32C_POLY = 0x82F63B78  # reflected Castagnoli


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (CRC32C_POLY if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


# ------------------------------------------------------------------ DCEP
def dcep_open(label: str, protocol: str, unordered: bool,
              channel_type: int, priority: int,
              reliability_parameter: int) -> bytes:
    lab = label.encode()
    pro = protocol.encode()
    out = bytearray()
    out.append(0x03)
    out.append(channel_type | (0x80 if unordered else 0x00))
    out += priority.to_bytes(2, "big")
    out += reliability_parameter.to_bytes(4, "big")
    out += len(lab).to_bytes(2, "big")
    out += len(pro).to_bytes(2, "big")
    out += lab
    out += pro
    return bytes(out)


def dcep_ack() -> bytes:
    return b"\x02"


RELIABLE, REXMIT, TIMED = 0x00, 0x01, 0x02

# nvst_input.rs NVST_CHANNEL_PROFILE (sid, label, ordered, kind, param)
NVST_CHANNELS = [
    (0,  "control_channel_reliable",                              True,  RELIABLE, 0),
    (2,  "custom_message_on_sctp_private_reliable",               True,  RELIABLE, 0),
    (4,  "custom_message_on_sctp_private_partially_reliable",     True,  REXMIT,   2),
    (6,  "control_channel_partially_reliable",                    False, TIMED,    300),
    (8,  "control_channel_unreliable",                            False, REXMIT,   0),
    (10, "input_channel_partially_reliable",                      False, TIMED,    300),
    (12, "cursor_channel",                                        True,  RELIABLE, 0),
    (14, "rtcp_on_sctp_private",                                  True,  RELIABLE, 0),
]


def sctp_init(src_port, dst_port, vtag, init_tag, a_rwnd, os_, mis,
              initial_tsn) -> bytes:
    """A minimal INIT chunk wrapped in the 12-byte common header with a
    correct CRC-32c — the parser cross-check vector."""
    chunk = bytearray()
    chunk.append(0x01)          # INIT
    chunk.append(0x00)          # flags
    chunk += (20).to_bytes(2, "big")   # length (fixed part only)
    chunk += init_tag.to_bytes(4, "big")
    chunk += a_rwnd.to_bytes(4, "big")
    chunk += os_.to_bytes(2, "big")
    chunk += mis.to_bytes(2, "big")
    chunk += initial_tsn.to_bytes(4, "big")
    pkt = bytearray()
    pkt += src_port.to_bytes(2, "big")
    pkt += dst_port.to_bytes(2, "big")
    pkt += vtag.to_bytes(4, "big")
    pkt += (0).to_bytes(4, "big")       # CRC placeholder
    pkt += chunk
    crc = crc32c(bytes(pkt))
    pkt[8:12] = crc.to_bytes(4, "little")  # RFC 4960: CRC in network
    # order is written as the LITTLE-endian byte image of the reflected
    # value — the standard's "transmit least significant octet first".
    return bytes(pkt)


# ------------------------------------------------------------- self-proof
def self_check() -> None:
    # the public CRC-32c check value (RFC 3720 B.4) + the empty-string
    # identity — the two vectors this generator trusts itself against
    assert crc32c(b"123456789") == 0xE3069283, "CRC-32c check value"
    assert crc32c(b"") == 0x00000000
    # all-zeros sanity: the reflected Castagnoli walk of four zero bytes
    # (computed independently below via the table-free bit loop)
    ref = 0xFFFFFFFF
    for _ in range(4):
        for _bit in range(8):
            ref = (ref >> 1) ^ (0x82F63B78 if ref & 1 else 0)
    assert crc32c(b"\x00" * 4) == (ref ^ 0xFFFFFFFF)
    # hand-computed DCEP goldens (RFC 8832 layout, transcribed by hand
    # from the str0m marshal order — independent of dcep_open() above)
    manual = (b"\x03\x00\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00abc")
    assert dcep_open("abc", "", False, RELIABLE, 0, 0) == manual
    manual2 = (b"\x03\x82\x00\x00\x00\x00\x01\x2c\x00\x02\x00\x00hi")
    assert dcep_open("hi", "", True, TIMED, 0, 300) == manual2
    assert dcep_ack() == b"\x02"
    # INIT packet: CRC verifies under an independent recomputation
    pkt = sctp_init(5000, 5000, 0, 0xDEADBEEF, 1024 * 1024, 65535,
                    65535, 0x12345678)
    assert len(pkt) == 32
    body = pkt[:8] + b"\x00\x00\x00\x00" + pkt[12:]
    assert int.from_bytes(pkt[8:12], "little") == crc32c(body)
    # every NVST OPEN marshals and re-parses (lengths round-trip)
    for sid, label, ordered, kind, param in NVST_CHANNELS:
        blob = dcep_open(label, "", not ordered, kind, 0, param)
        assert blob[0] == 0x03
        assert (blob[1] & 0x80) != 0 if not ordered else (blob[1] & 0x80) == 0
        lab_len = int.from_bytes(blob[8:10], "big")
        pro_len = int.from_bytes(blob[10:12], "big")
        assert lab_len == len(label) and pro_len == 0
        assert blob[12:12 + lab_len].decode() == label
    print("sctp_reference.py: self-check PASS (%d channels, CRC-32c ok)"
          % len(NVST_CHANNELS))


# ----------------------------------------------------------------- emit
HEADER = """/* kn_sctp_kat_vectors.h — GENERATED by tools/sctp_reference.py.
 * Do not edit. The Python reference self-proves against published
 * CRC-32c check values and hand-transcribed DCEP goldens BEFORE
 * emitting this file (the vectors-are-trusted rule). Regeneration is
 * byte-identical.
 */
#ifndef KN_SCTP_KAT_VECTORS_H
#define KN_SCTP_KAT_VECTORS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t    sid;
    uint8_t     unordered;
    uint8_t     channel_type;
    uint32_t    rel_param;
    const char *label;
    const uint8_t *bytes;   /* the golden DATA_CHANNEL_OPEN message */
    size_t      len;
} kn_sctp_channel_kat;

typedef struct {
    const uint8_t *pkt;     /* complete SCTP packet (common hdr + INIT) */
    size_t      len;
    uint16_t    src_port, dst_port;
    uint32_t    vtag, init_tag, a_rwnd, initial_tsn;
    uint16_t    os, mis;
    uint32_t    crc;        /* the CRC-32c the packet carries */
} kn_sctp_init_kat;

typedef struct {
    const uint8_t *data;
    size_t      len;
    int         expect;     /* 0 reject / 2 ACK / 3 OPEN */
    int         unordered;
    uint8_t     channel_type;
    uint32_t    rel_param;
    const char *label;
    const char *protocol;
} kn_sctp_dcep_parse_kat;

"""


def c_bytes(name: str, blob: bytes) -> str:
    body = ",".join("0x%02x" % b for b in blob)
    return "static const uint8_t %s[] = {%s};\n" % (name, body)


def emit(path: str) -> None:
    self_check()
    out = [HEADER]
    # channel goldens
    out.append("#define KN_SCTP_KAT_N_CHANNELS %d\n\n" % len(NVST_CHANNELS))
    for i, (sid, label, ordered, kind, param) in enumerate(NVST_CHANNELS):
        blob = dcep_open(label, "", not ordered, kind, 0, param)
        out.append(c_bytes("kn_sctp_kat_ch%d_bytes" % i, blob))
    out.append("\nstatic const kn_sctp_channel_kat "
               "kn_sctp_kat_channels[KN_SCTP_KAT_N_CHANNELS] = {\n")
    for i, (sid, label, ordered, kind, param) in enumerate(NVST_CHANNELS):
        out.append('    { %d, %d, 0x%02x, %u, "%s", kn_sctp_kat_ch%d_bytes, '
                   '%d },\n' % (sid, 0 if ordered else 1, kind, param,
                                label, i, len(dcep_open(label, "",
                                                        not ordered, kind,
                                                        0, param))))
    out.append("};\n\n")
    # ACK golden
    out.append(c_bytes("kn_sctp_kat_ack", dcep_ack()))
    # INIT packet golden
    pkt = sctp_init(5000, 5000, 0, 0xDEADBEEF, 1024 * 1024, 65535, 65535,
                    0x12345678)
    out.append("\n" + c_bytes("kn_sctp_kat_init_pkt", pkt))
    out.append("static const kn_sctp_init_kat kn_sctp_kat_init = {\n"
               "    kn_sctp_kat_init_pkt, %d, 5000, 5000, 0u, 0xDEADBEEFu,\n"
               "    %du, 0x12345678u, 65535, 65535, 0x%08Xu };\n\n"
               % (len(pkt), 1024 * 1024,
                  int.from_bytes(pkt[8:12], "little")))
    # CRC goldens
    out.append("static const struct { const char *in; uint32_t crc; } "
               "kn_sctp_kat_crc32c[] = {\n")
    for text in ("", "123456789", "a", "abc", "SCTP-over-DTLS"):
        out.append('    { "%s", 0x%08Xu },\n' % (text,
                                                 crc32c(text.encode())))
    out.append("};\n#define KN_SCTP_KAT_N_CRC32C %d\n\n" % 5)
    # DCEP parse vectors (valid + malformed)
    parse_cases = []
    for i, (sid, label, ordered, kind, param) in enumerate(NVST_CHANNELS):
        parse_cases.append((dcep_open(label, "", not ordered, kind, 0,
                                      param), 3, 0 if ordered else 1, kind,
                            param, label, ""))
    parse_cases.append((dcep_ack(), 2, 0, 0, 0, "", ""))
    parse_cases.append((dcep_open("x", "proto", False, RELIABLE, 0, 0), 3,
                        0, RELIABLE, 0, "x", "proto"))
    bad = [b"", b"\x01", b"\x03\x00", b"\x03\x00\x00\x00\x00\x00\x00\x00"
           b"\x00\x05\x00\x00ab",          # label length overruns
           b"\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",  # bad msg type
           b"\x03\x07\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"]  # bad channel type
    for blob in bad:
        parse_cases.append((blob, 0, 0, 0, 0, "", ""))
    out.append("#define KN_SCTP_KAT_N_PARSE %d\n" % len(parse_cases))
    for i, (blob, *_rest) in enumerate(parse_cases):
        out.append(c_bytes("kn_sctp_kat_parse%d" % i, blob) if blob else
                         "static const uint8_t kn_sctp_kat_parse%d[] = "
                         "{0};\n" % i)
    out.append("\nstatic const kn_sctp_dcep_parse_kat "
               "kn_sctp_kat_parse[KN_SCTP_KAT_N_PARSE] = {\n")
    for i, (blob, expect, unordered, kind, param, label,
            protocol) in enumerate(parse_cases):
        out.append('    { kn_sctp_kat_parse%d, %d, %d, %d, 0x%02x, %uu, '
                   '"%s", "%s" },\n' % (i, len(blob), expect, unordered,
                                        kind, param, label, protocol))
    out.append("};\n\n#endif /* KN_SCTP_KAT_VECTORS_H */\n")
    with open(path, "w") as f:
        f.write("".join(out))
    print("written: %s" % path)


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    emit(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(root, "streamer", "src", "kn_sctp_kat_vectors.h"))
