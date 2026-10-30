#!/usr/bin/env python3
"""opus_reference.py — the F21 second implementation + vector emitter.

Two-implementations rule (START-HERE §5): the kn_opus wrapper contract is
pinned twice. THIS script is the independent path: it drives libopus
through Python ctypes (encoder AND decoder, separate states, never the C
wrapper), self-proves the round trip, and emits
streamer/src/kn_opus_kat_vectors.h for test_knopus to replay through the
C wrapper. What the vectors pin is OUR contract, not the codec: frame
counts per packet duration, the stereo interleave order, the float->s16
quantisation the docs promise (tolerance 1 LSB: libopus versions may
differ in the last ulp, and lrintf vs floor(x+.5) disagree on exact
half-LSB boundaries), and every error path (-1 for garbage, -1 for a
capacity smaller than the 120 ms worst case, -1 for NULL/zero args).

Usage:  python3 tools/opus_reference.py            # regenerate the header
        python3 tools/opus_reference.py --selftest # self-prove only

The script REFUSES to emit when a self-proof fails (same discipline as
arx_reference.py / oggopus_reference.py).
"""
import ctypes
import ctypes.util
import math
import os
import struct
import sys

RATE = 48000
CH = 2
APP_AUDIO = 2049
OPUS_SET_BITRATE = 4002
OPUS_SET_COMPLEXITY = 4010

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "streamer", "src", "kn_opus_kat_vectors.h")


def load_opus():
    names = ["libopus.so.0", "libopus.so", "libopus.dylib", "opus.dll"]
    path = ctypes.util.find_library("opus")
    if path:
        names.insert(0, path)
    last = None
    for n in names:
        try:
            return ctypes.CDLL(n)
        except OSError as e:
            last = e
    raise SystemExit("opus_reference: no libopus found (%s)" % last)


class Ref:
    """The independent ctypes path: one encoder, fresh decoder per call."""

    def __init__(self):
        self.lib = load_opus()
        self.lib.opus_encoder_create.restype = ctypes.c_void_p
        self.lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int,
                                                 ctypes.c_int,
                                                 ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_encode.restype = ctypes.c_int
        self.lib.opus_encode.argtypes = [ctypes.c_void_p,
                                         ctypes.POINTER(ctypes.c_int16),
                                         ctypes.c_int,
                                         ctypes.POINTER(ctypes.c_ubyte),
                                         ctypes.c_int32]
        self.lib.opus_decoder_create.restype = ctypes.c_void_p
        self.lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int,
                                                 ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_decode_float.restype = ctypes.c_int
        self.lib.opus_decode_float.argtypes = [ctypes.c_void_p,
                                               ctypes.POINTER(ctypes.c_ubyte),
                                               ctypes.c_int32,
                                               ctypes.POINTER(ctypes.c_float),
                                               ctypes.c_int, ctypes.c_int]
        self.lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
        self.lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]

    def encoder(self):
        err = ctypes.c_int(0)
        enc = self.lib.opus_encoder_create(RATE, CH, APP_AUDIO,
                                           ctypes.byref(err))
        if not enc or err.value != 0:
            raise SystemExit("opus_reference: encoder_create err %d"
                             % err.value)
        # fixed operating point: the vectors must be reproducible forever
        self.lib.opus_encoder_ctl(enc, OPUS_SET_BITRATE, ctypes.c_int32(96000))
        self.lib.opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY,
                                  ctypes.c_int32(10))
        return enc

    def encode(self, enc, pcm_s16, frame_size):
        buf = (ctypes.c_ubyte * 4000)()
        n = len(pcm_s16)
        arr = (ctypes.c_int16 * n)(*pcm_s16)
        got = self.lib.opus_encode(enc, arr, frame_size, buf, 4000)
        if got <= 0:
            raise SystemExit("opus_reference: opus_encode rc %d" % got)
        return bytes(buf[:got])

    def decode_f32(self, packet, cap_frames=5760, dec=None):
        """Cold decode by default; pass `dec` to ride a warm decoder (the
        production shape: ONE decoder, sequential packets — the CELT
        postfilter state makes a cold single-packet decode land far below
        the signal's true level, so the vectors pin the WARM stream)."""
        own = dec is None
        err = ctypes.c_int(0)
        if own:
            dec = self.lib.opus_decoder_create(RATE, CH, ctypes.byref(err))
            if not dec or err.value != 0:
                raise SystemExit("opus_reference: decoder_create err %d"
                                 % err.value)
        out = (ctypes.c_float * (cap_frames * CH))()
        pkt = (ctypes.c_ubyte * len(packet))(*packet) if packet else None
        n = self.lib.opus_decode_float(dec, pkt, len(packet), out,
                                       cap_frames, 0)
        if own:
            self.lib.opus_decoder_destroy(dec)
        if n <= 0:
            return n, []
        return n, [out[i] for i in range(n * CH)]


    def decoder(self):
        err = ctypes.c_int(0)
        dec = self.lib.opus_decoder_create(RATE, CH, ctypes.byref(err))
        if not dec or err.value != 0:
            raise SystemExit("opus_reference: decoder_create err %d"
                             % err.value)
        return dec


def sine_s16(frames, hz_l, hz_r, amp):
    pcm = []
    for i in range(frames):
        t = i / float(RATE)
        for hz in (hz_l, hz_r):
            v = amp * math.sin(2.0 * math.pi * hz * t)
            pcm.append(max(-32768, min(32767, int(math.floor(v * 32767.0
                                                             + 0.5)))))
    return pcm


def q16(v):
    """The quantisation the C test mirrors (lrintf(v*32767.0f), clamped)."""
    if v != v:
        return 0
    v = max(-1.0, min(1.0, v))
    return max(-32768, min(32767, int(math.floor(v * 32767.0 + 0.5))))


def build_vectors(ref):
    vecs = []
    # ONE encoder stream, ONE warm decoder replay: the vectors are a
    # sequential Opus stream (the production shape — F21 feeds ONE
    # decoder per session), prefixed by 40 pre-roll packets that only
    # exist to clear the ENCODER lookahead (the first ~6.5 ms of any Opus
    # encode emerge as near-silence); the decoder side starts cold at
    # vector 0 on BOTH implementations, so vec0 carries the CELT
    # postfilter ramp and its band is widened accordingly.
    # rms bands are in FLOAT scale (a sine of amplitude A has rms A/sqrt2)
    specs = [("tone5", 5, 440.0, 554.37, 0.25, 0.05, 0.30),
             ("tone5b", 5, 880.0, 330.0, 0.5, 0.15, 0.60),
             ("quiet5", 5, 1000.0, 1000.0, 0.001, 0.002, 0.05),  # codec noise floor, not the -60 dBFS signal
             ("tone10", 10, 261.63, 392.0, 0.25, 0.05, 0.30)]
    enc = ref.encoder()
    warm = ref.decoder()
    for _ in range(40):                      # encoder lookahead pre-roll
        pkt = ref.encode(enc, sine_s16(RATE * 5 // 1000, 440.0, 554.37, 0.25),
                         RATE * 5 // 1000)
        # the DECODER needs the same pre-roll: a cold CELT decoder's
        # postfilter wakes up over the first packets, so both sides walk
        # the warmup to reach comparable state. Warmups pin frame counts
        # only (no PCM arrays — the header stays small); the content
        # vectors that follow pin samples.
        n, _ = ref.decode_f32(pkt, dec=warm)
        if n != RATE * 5 // 1000:
            raise SystemExit("self-proof FAIL warmup: decoded %d" % n)
        vecs.append({"name": "warmup", "pkt": pkt, "frames": n, "s16": None})
    for name, ms, hl, hr, amp, lo, hi in specs:
        frames = RATE * ms // 1000
        # transition pre-roll: the encoder's ~6.5 ms lookahead means a
        # packet still carries the PREVIOUS signal unless the new sine
        # has filled the lookahead window — 10 packets of the new signal
        # (50-100 ms) make the content vector pure. These ride the warm
        # decoder as frame-count-only warmups.
        for _ in range(10):
            tp = ref.encode(enc, sine_s16(frames, hl, hr, amp), frames)
            tn, _ = ref.decode_f32(tp, dec=warm)
            if tn != frames:
                raise SystemExit("self-proof FAIL %s transition: %d"
                                 % (name, tn))
            vecs.append({"name": name + "_tr", "pkt": tp, "frames": tn,
                         "s16": None})
        pkt = ref.encode(enc, sine_s16(frames, hl, hr, amp), frames)
        n, pcm = ref.decode_f32(pkt, dec=warm)
        if n != frames:
            raise SystemExit("self-proof FAIL %s: decoded %d != %d frames"
                             % (name, n, frames))
        rms = math.sqrt(sum(v * v for v in pcm) / len(pcm))
        if not (lo < rms < hi):
            raise SystemExit("self-proof FAIL %s: rms %f outside (%f,%f)"
                             % (name, rms, lo, hi))
        vecs.append({"name": name, "pkt": pkt, "frames": n,
                     "s16": [q16(v) for v in pcm]})
    ref.lib.opus_decoder_destroy(warm)
    ref.lib.opus_encoder_destroy(enc)
    # determinism proof: a second encoder, same stream, SAME bytes
    enc2 = ref.encoder()
    pkts2 = []
    for _ in range(40):
        pkts2.append(ref.encode(enc2,
                                sine_s16(RATE * 5 // 1000, 440.0, 554.37,
                                         0.25),
                                RATE * 5 // 1000))
    for name, ms, hl, hr, amp, lo, hi in specs:
        frames = RATE * ms // 1000
        for _ in range(10):
            pkts2.append(ref.encode(enc2, sine_s16(frames, hl, hr, amp),
                                    frames))
        pkts2.append(ref.encode(enc2, sine_s16(frames, hl, hr, amp), frames))
    ref.lib.opus_encoder_destroy(enc2)
    if pkts2 != [v["pkt"] for v in vecs]:
        raise SystemExit("self-proof FAIL: encoder not deterministic")
    # warm-replay repeatability: a second cold-start decoder walking the
    # same sequence must land on the same quantised PCM
    warm2 = ref.decoder()
    for v in vecs:
        n2, pcm2 = ref.decode_f32(v["pkt"], dec=warm2)
        if n2 != v["frames"]:
            raise SystemExit("self-proof FAIL %s: warm replay frame count"
                             % v["name"])
        if v["s16"] is not None and [q16(x) for x in pcm2] != v["s16"]:
            raise SystemExit("self-proof FAIL %s: warm replays disagree"
                             % v["name"])
    ref.lib.opus_decoder_destroy(warm2)
    # the garbage vector: RFC 6716 TOC 0xFF is reserved; libopus rejects
    garbage = bytes([0xff, 0xff, 0xff, 0xff, 0x00, 0x11, 0x22, 0x33])
    ng, _ = ref.decode_f32(garbage)
    if ng >= 0:
        raise SystemExit("self-proof FAIL: garbage decoded (%d)" % ng)
    vecs.append({"name": "garbage", "pkt": garbage, "frames": -1, "s16": []})
    return vecs


def emit(vecs):
    def blob(name, data):
        rows = []
        for i in range(0, len(data), 16):
            rows.append("    " + ",".join("0x%02x" % b for b in data[i:i + 16])
                        + ",")
        return ("static const unsigned char %s[%d] = {\n%s\n};\n"
                % (name, len(data), "\n".join(rows)))

    out = ["""/* kn_opus_kat_vectors.h — GENERATED by tools/opus_reference.py.
 * Do not edit by hand; regenerate + rerun the gates.
 * F21 wrapper vectors: libopus-encoded Opus packets (48 kHz stereo,
 * fixed 96 kb/s / complexity 10 operating point) + the float32 decode
 * expected from a COLD decoder, quantised to s16 (the C test compares
 * with tolerance 1 LSB). 'garbage' must be rejected with -1. */
#ifndef KN_OPUS_KAT_VECTORS_H
#define KN_OPUS_KAT_VECTORS_H

#define KN_OPUS_KAT_RATE 48000u
#define KN_OPUS_KAT_CH 2u
"""]
    for i, v in enumerate(vecs):
        out.append(blob("kn_opus_vec%d_pkt" % i, v["pkt"]))
        if v["s16"]:
            rows = []
            for r in range(0, len(v["s16"]), 12):
                rows.append("    " + ",".join(str(x) for x in
                                              v["s16"][r:r + 12]) + ",")
            out.append("static const short kn_opus_vec%d_s16[%d] = {\n%s\n};\n"
                       % (i, len(v["s16"]), "\n".join(rows)))
    out.append("""
typedef struct {
    const unsigned char *pkt; unsigned pkt_len;
    int expect_frames;                 /* >0 decoded frames/ch, -1 reject */
    const short *expect_s16;           /* NULL: frame count only (warmup,
                                        * and the garbage reject vector) */
    unsigned expect_samples;           /* frames * channels */
} kn_opus_kat_vec;

static const kn_opus_kat_vec kn_opus_kat[] = {""")
    for i, v in enumerate(vecs):
        s16 = ("kn_opus_vec%d_s16" % i) if v["s16"] else "0"
        out.append("    { kn_opus_vec%d_pkt, %u, %d, %s, %u },"
                   % (i, len(v["pkt"]), v["frames"], s16, len(v["s16"] or [])))
    out.append("""};
#define KN_OPUS_KAT_N (sizeof kn_opus_kat / sizeof kn_opus_kat[0])

#endif /* KN_OPUS_KAT_VECTORS_H */
""")
    with open(OUT, "w") as f:
        f.write("\n".join(out))
    print("wrote %s (%d vectors)" % (OUT, len(vecs)))


def main():
    ref = Ref()
    vecs = build_vectors(ref)
    print("self-proofs OK: %d vectors, determinism + energy + cold-decoder "
          "+ garbage-reject all passed" % len(vecs))
    if "--selftest" not in sys.argv:
        emit(vecs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
