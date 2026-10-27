#!/usr/bin/env python3
"""make_rx_capture.py — synthetic Mjolnir-style capture generator.

Turns the repo's real H.264 clip (streamer/assets/synthetic.264 — the
decode.synthetic KAT source, 60 frames 320x240) into a kn_ring_dump-format
hex capture that `knvstd --replay-capture` feeds through the FULL stage-2
receive pipeline (SRTP-none or protected, reorder, FEC repair, AU assembly,
RFI/NACK logic). This is the OFFLINE half of the G2 mechanism:

  python3 tools/make_rx_capture.py --out /tmp/cap.hex --manifest /tmp/cap.json
  knvstd --replay-capture=/tmp/cap.hex --replay-au-dir=/tmp/aus
  python3 tools/make_rx_capture.py --verify /tmp/cap.json < replay stdout

Packetization mirrors the canonical cloud layout derived from nvst.rs
(docs/RX.md §Parity construction):
  - every packet: RTP V2 + GS extension 0x4753 (16-byte LE body),
    pt 96, ssrc 0x11223344, ts = frame * 3000 (90 kHz),
  - SOF packet media = 4-byte GS frame header (47 53 00 wireType) + AU chunk,
    continuation packets carry raw AU bytes,
  - stream_packet_index counts DATA packets only (parity consumes RTP
    sequence numbers but not SPI values — same-frame SPI continuity must
    survive interleaved parity),
  - FEC: one block per frame, d = the frame's packet count, pct configurable;
    parity = Cauchy fold over the zero-padded FULL data packets, then the
    parity's own envelope + frame_index + ext[11] + fec_word are restored
    (ext[0..4] spi and ext[8..11] flags stay algebraic so recovery
    reconstructs them exactly — pitfall #4 territory),
  - records are emitted in kn_ring_dump line format (knring.c dump_visit:
    "%llu %08x %u %u" + " %02x"*len), meta = RTP sequence.

Loss/reorder injection:
  --loss N            drop N DATA packets, chosen FEC-recoverable when
                      possible (per block at most parity_count)
  --loss-mode fatal   ignore recoverability (exercises NACK -> gap timeout
                      -> recovery -> RFI/keyframe paths)
  --reorder P         probability of swapping adjacent emitted records
                      (position 0 is never swapped — a higher index first
                      makes earlier packets legitimately stale)
  --seed S            deterministic injection

The manifest carries per-AU SHA-1 + length + the exact pipeline config the
replay must use (vps => shard_len); --verify compares a replay's JSON-lines
stdout against it.

Uses fec_reference.py's proved GF(2^8) Cauchy primitives (self-checked
against nvst.rs:13239 in earlier rounds).
"""
import argparse
import hashlib
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import fec_reference as fecr          # noqa: E402

SSRC = 0x11223344
PT = 96
CLIP = os.path.join(ROOT, "streamer", "assets", "synthetic.264")


# ------------------------------------------------------------ H264 -> AUs

def split_nals(data):
    """NAL units with their on-disk start positions (start codes included).
    Returns [(start, end, nal_type)] — end excludes the next start code."""
    starts = []
    i = 0
    n = len(data)
    while i < n - 2:
        if data[i:i + 3] == b"\x00\x00\x01":
            starts.append(i)
            i += 3
        else:
            i += 1
    out = []
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else n
        hdr = s + 3
        if data[s:s + 4] == b"\x00\x00\x00\x01":
            hdr = s + 4
        out.append((s, e, data[hdr] & 0x1F))
    return out


class BitReader(object):
    def __init__(self, buf, bitpos=0):
        self.b = buf
        self.p = bitpos

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.b[self.p >> 3]
            v = (v << 1) | ((byte >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def ue(self):
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            if zeros > 32:
                raise ValueError("ue(v) runaway")
        return (1 << zeros) - 1 + (self.u(zeros) if zeros else 0)


def first_mb_in_slice(nal_bytes, nal_type):
    """first_mb_in_slice is the first ue(v) after the 1-byte H.264 NAL
    header (avc). 0 marks the first slice of a picture."""
    hdr_len = 1                # forbidden_zero_bit + ref_idc + type
    br = BitReader(nal_bytes, hdr_len * 8)
    return br.ue()


def group_aus(data):
    """Access units: non-slice NALs attach to the FOLLOWING slice group; a
    slice with first_mb_in_slice == 0 starts a new picture. Returns a list
    of (start, end, is_key) byte ranges of the original file."""
    nals = split_nals(data)
    aus = []
    cur_start = None
    cur_key = False
    pending_prefix = None
    for (s, e, t) in nals:
        if t in (1, 5):                       # coded slice (non-IDR / IDR)
            nal_body = data[s:e]
            hdr = s + 4 if data[s:s + 4] == b"\x00\x00\x00\x01" else s + 3
            mb0 = first_mb_in_slice(data[hdr:e], t) == 0
            if mb0:
                if cur_start is not None:
                    aus.append((cur_start, s, cur_key))
                cur_start = pending_prefix if pending_prefix is not None else s
                cur_key = (t == 5)
                pending_prefix = None
            # continuation slice of the same picture: keeps the range
        else:                                 # SPS/PPS/SEI/AUD prefix
            if cur_start is None and pending_prefix is None:
                pending_prefix = s
            elif pending_prefix is None:
                pending_prefix = s
            # else: already prefixing — extend implicitly (ranges are
            # contiguous in the file)
    if cur_start is not None:
        aus.append((cur_start, len(data), cur_key))
    return aus


# --------------------------------------------------------- packet builders

def gs_ext(spi, frame_index, flags, cur, last, pct, fidx, fsrc):
    packet_word = ((spi & 0xFFFFFF) << 8)
    ext11 = ((last & 3) << 6) | ((cur & 3) << 4)
    flags_word = (flags & 0x0F) | (ext11 << 24)
    fec_word = ((pct & 0xFF) << 4) | ((fidx & 0x3FF) << 12) \
        | ((fsrc & 0x3FF) << 22)
    return (packet_word.to_bytes(4, "little")
            + (frame_index & 0xFFFFFFFF).to_bytes(4, "little")
            + flags_word.to_bytes(4, "little")
            + fec_word.to_bytes(4, "little"))


def rtp_pkt(seq, ts, flags, spi, fi, cur, last, pct, fidx, fsrc, media,
            marker=0):
    out = bytearray()
    out.append(0x90)                                # V2, X=1
    out.append((marker << 7) | (PT & 0x7F))
    out += (seq & 0xFFFF).to_bytes(2, "big")
    out += (ts & 0xFFFFFFFF).to_bytes(4, "big")
    out += (SSRC & 0xFFFFFFFF).to_bytes(4, "big")
    out += (0x4753).to_bytes(2, "big") + (4).to_bytes(2, "big")
    out += gs_ext(spi, fi, flags, cur, last, pct, fidx, fsrc)
    out += media
    return bytes(out)


def parity_pkt(base_seq, ts, d, p, pct, cur, last, fi, data_pkts, shard_len):
    npar = -(-(d * pct) // 100)
    fold = bytearray(shard_len)
    for i, pkt in enumerate(data_pkts):
        src = pkt.ljust(shard_len, b"\0")[:shard_len]
        fecr.axpy(fold, src, fecr.cauchy(i, p, npar))
    seq = base_seq + d + p
    fold[0] = 0x90
    fold[1] = PT & 0x7F
    fold[2:4] = (seq & 0xFFFF).to_bytes(2, "big")
    fold[4:8] = (ts & 0xFFFFFFFF).to_bytes(4, "big")
    fold[8:12] = SSRC.to_bytes(4, "big")
    fold[12:16] = bytes([0x47, 0x53, 0x00, 0x04])
    fold[20:24] = (fi & 0xFFFFFFFF).to_bytes(4, "little")
    fold[27] = ((last & 3) << 6) | ((cur & 3) << 4)
    fec_word = ((pct & 0xFF) << 4) | (((d + p) & 0x3FF) << 12) \
        | ((d & 0x3FF) << 22)
    fold[28:32] = fec_word.to_bytes(4, "little")
    return bytes(fold)


# ----------------------------------------------------------------- main

def build_capture(clip_path, vps, fec_pct, frames_limit):
    data = open(clip_path, "rb").read()
    aus = group_aus(data)
    if frames_limit:
        aus = aus[:frames_limit]
    shard_len = vps + 16
    records = []          # (mono_us, seq, pkt_bytes, kind, fi)
    aus_meta = []
    seq = 1
    spi = 1
    for fi0, (a_start, a_end, is_key) in enumerate(aus):
        fi = fi0 + 1
        au = data[a_start:a_end]
        ts = fi * 3000
        wire = 2 if is_key else 1               # NvstFrameType I / P
        # chunk capacities: SOF packet carries the 4-byte GS frame header
        cap_first = shard_len - 32 - 4
        cap_rest = shard_len - 32
        assert cap_first > 0
        chunks = []
        off = 0
        first = True
        while off < len(au):
            c = cap_first if first else cap_rest
            chunks.append(au[off:off + c])
            off += c
            first = False
        if not chunks:
            chunks = [b""]
        d = len(chunks)
        data_pkts = []
        frame_chunks = []          # (seq, chunk_len, capacity) per packet
        for i, chunk in enumerate(chunks):
            flags = 0x01                        # PIC
            if i == 0:
                flags |= 0x04                   # SOF (cur==0 -> is_start)
            if i == d - 1:
                flags |= 0x02                   # EOF (cur==last -> is_end)
            media = (bytes([0x47, 0x53, 0x00, wire]) + chunk) if i == 0 \
                else chunk
            pkt = rtp_pkt(seq, ts, flags, spi, fi, 0, 0,
                          fec_pct, i, d if fec_pct else 0, media,
                          marker=1 if i == d - 1 else 0)
            assert len(pkt) <= shard_len, (len(pkt), shard_len)
            mono = (fi - 1) * 33333 + i * 100
            records.append({"mono_us": mono, "seq": seq, "pkt": pkt,
                            "kind": "data", "fi": fi, "shard": i})
            data_pkts.append(pkt)
            frame_chunks.append((seq, len(chunk),
                                 cap_first if i == 0 else cap_rest))
            seq += 1
            spi += 1
        if fec_pct:
            npar = -(-(d * fec_pct) // 100)
            # the frame's base sequence, captured BEFORE parity records are
            # appended (records[-d] shifts once the first parity lands —
            # that off-by-one produced a duplicate pkt seq that the replay
            # replay-window correctly rejected)
            frame_base_seq = records[-d]["seq"]
            for p in range(npar):
                pkt = parity_pkt(frame_base_seq, ts, d, p,
                                 fec_pct, 0, 0, fi, data_pkts, shard_len)
                mono = (fi - 1) * 33333 + d * 100 + p * 10
                records.append({"mono_us": mono, "seq": seq, "pkt": pkt,
                                "kind": "parity", "fi": fi, "shard": d + p})
                seq += 1
        aus_meta.append({"fi": fi, "len": len(au),
                         "sha1": hashlib.sha1(au).hexdigest(),
                         "keyframe": 1 if is_key else 0, "pkts": d,
                         "chunks": frame_chunks,
                         "au_ranges": [(0, len(au))]})
    # stash chunk offsets so inject() can compute recovered expectations:
    # each AU = concat(chunks); chunk boundaries are cumulative
    for m in aus_meta:
        offs = []
        off = 0
        for (_, clen, _) in m["chunks"]:
            offs.append((off, clen))
            off += clen
        m["chunk_offsets"] = offs
        del m["au_ranges"]
    return records, aus_meta, shard_len


def simulate_replay(emitted, vps):
    """Run the emitted records through the rx_reference Python receiver —
    the manifest's replay_frames ARE the reference verdict the C replay
    must reproduce byte-for-byte (frames, hashes, contiguity). Config
    mirrors run_replay(): window 1024, timeout 90 s (the capture timeline
    drives the 52 ms reorder/FEC deadlines; the stream timeout must not
    fire), H264, 2 MiB AU cap."""
    import gs_reference as gsr
    import rx_reference as rxr
    r = rxr.Receiver(codec=gsr.H264, window=1024,
                     au_cap=2 * 1024 * 1024, vps=vps,
                     timeout_ms=90000, startup_ms=90000, origin_ms=0)
    frames = []
    t_last = 0
    for rec in emitted:
        now = rec["mono_us"] // 1000
        if now < t_last:
            now = t_last
        t_last = now
        for call in (0, 1):
            ev = []
            if call == 0:
                r.process_packet(rec["seq"], rec["pkt"], now, ev)
            else:
                r.poll(now, ev)
            for e in ev:
                if e["t"] == -1:
                    frames.append({
                        "fi": e["frame_index"], "len": e["len"],
                        "sha1": "%016x" % e["hash"],   # first 8 digest bytes
                        "kf": e["keyframe"], "contig": e["contiguous"]})
    return frames, r


def inject(records, loss, loss_mode, reorder_p, seed, fec_pct):
    rng = random.Random(seed)
    lost = set()
    if loss > 0:
        datas = [r for r in records if r["kind"] == "data"]
        if loss_mode == "recoverable":
            # per frame at most parity_count losses (frame blocks are the
            # frame's data records)
            by_frame = {}
            for r in datas:
                by_frame.setdefault(r["fi"], []).append(r)
            budget = loss
            fis = sorted(by_frame)
            rng.shuffle(fis)
            for fi in fis:
                if budget <= 0:
                    break
                npar = -(-(len(by_frame[fi]) * fec_pct) // 100) if fec_pct \
                    else 0
                take = min(budget, max(npar, 0))
                if take == 0:
                    continue
                victims = rng.sample(by_frame[fi], take)
                for v in victims:
                    lost.add(v["seq"])
                budget -= take
            if budget > 0 and loss_mode == "recoverable":
                pass                      # stay recoverable: drop the rest
        else:
            victims = rng.sample(datas, min(loss, len(datas)))
            lost = {v["seq"] for v in victims}
    emitted = [r for r in records if r["seq"] not in lost]
    if reorder_p > 0.0:
        i = 1
        while i < len(emitted) - 1:
            if rng.random() < reorder_p:
                emitted[i], emitted[i + 1] = emitted[i + 1], emitted[i]
                i += 2
            else:
                i += 1
    return emitted, sorted(lost)


def protect_records(emitted, spec):
    """SRTP-protect every record in place of its plaintext (the production
    capture shape). Uses the rx_reference/srtp_reference proved primitives —
    the same ones the KAT vectors cross-check against the C session."""
    import srtp_reference as sr
    import rx_reference as rxr
    profile, tag, keyhex, salthex = spec.split(":")
    tag_len = int(tag)
    master = bytes.fromhex(keyhex)
    salt = bytes.fromhex(salthex)
    # mirror the C session validation (kn_rx_srtp_session_init): the KDF
    # copies the salt into a 16-byte IV prefix — longer salts are rejected
    need_key = 32 if profile in ("cm256", "gcm256") else 16
    if len(master) != need_key:
        raise ValueError("%s needs a %d-byte master key (got %d)"
                         % (profile, need_key, len(master)))
    if len(salt) > 14:
        raise ValueError("master salt must be <= 14 bytes (got %d)"
                         % len(salt))
    if profile in ("cm128", "cm256") and tag_len not in (4, 10):
        raise ValueError("CM tag must be 4 or 10")
    if profile in ("gcm128", "gcm256") and tag_len not in (8, 16):
        raise ValueError("GCM tag must be 8 or 16")
    if profile == "cm128":
        enc = sr.kdf128(master, salt, 0x00, 16)
        auth = sr.kdf128(master, salt, 0x01, 20)
        ssalt = sr.kdf128(master, salt, 0x02, 14)
    elif profile == "cm256":
        enc = rxr.kdf256(master, salt, 0x00, 32)
        auth = rxr.kdf256(master, salt, 0x01, 20)
        ssalt = rxr.kdf256(master, salt, 0x02, 14)
    elif profile == "gcm128":
        enc = sr.kdf128(master, salt, 0x00, 16)
        ssalt = sr.kdf128(master, salt, 0x02, 12)
        auth = b""
    elif profile == "gcm256":
        enc = rxr.kdf256(master, salt, 0x00, 32)
        ssalt = rxr.kdf256(master, salt, 0x02, 12)
        auth = b""
    else:
        raise ValueError("unknown profile " + profile)
    out = []
    for r in emitted:
        pkt = r["pkt"]
        rc, hdr = rxr.rtp_parse_hdr(pkt)
        assert rc == 0
        index = r["seq"]              # roc 0 (captures stay under 2^16)
        if profile in ("cm128", "cm256"):
            iv = sr.srtp_aes_cm_iv(ssalt, hdr["ssrc"], index)
            body = pkt[:hdr["payload_off"]] + rxr._cm_keystream_xor(
                enc, iv, pkt[hdr["payload_off"]:])
            import hmac as pyhmac
            import hashlib
            roc_be = (index >> 16).to_bytes(4, "big")
            tagb = pyhmac.new(auth, body + roc_be, hashlib.sha1).digest()
            protected = body + tagb[:tag_len]
        else:
            iv = sr.srtp_gcm_iv(ssalt, hdr["ssrc"], 0, hdr["seq"])
            ct, tagb = rxr.gcm_crypt(enc, iv, pkt[:hdr["payload_off"]],
                                     pkt[hdr["payload_off"]:], tag_len,
                                     encrypting=True)
            protected = pkt[:hdr["payload_off"]] + ct + tagb
        nr = dict(r)
        nr["pkt"] = protected
        out.append(nr)
    return out


def render_hexdump(emitted, out_path):
    with open(out_path, "w", encoding="ascii", newline="\n") as f:
        for r in emitted:
            f.write("%d %08x %u %u" % (r["mono_us"], r["seq"] & 0xFFFFFFFF,
                                       0, len(r["pkt"])))
            for b in r["pkt"]:
                f.write(" %02x" % b)
            f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clip", default=CLIP)
    ap.add_argument("--out", default=None, help="hexdump capture output")
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--vps", type=int, default=1280,
                    help="negotiated packetSize (shard_len = vps+16)")
    ap.add_argument("--fec-pct", type=int, default=0,
                    help="FEC repair percentage (0 = no FEC)")
    ap.add_argument("--frames", type=int, default=0,
                    help="limit the number of AUs (0 = all)")
    ap.add_argument("--loss", type=int, default=0,
                    help="number of DATA packets to drop")
    ap.add_argument("--loss-mode", choices=["recoverable", "fatal"],
                    default="recoverable")
    ap.add_argument("--reorder", type=float, default=0.0,
                    help="adjacent-swap probability")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--srtp", default=None,
                    help="PROFILE:TAG:KEYHEX:SALTHEX — protect every record "
                         "(cm128|cm256|gcm128|gcm256); replay with the same "
                         "spec via --replay-srtp")
    ap.add_argument("--verify", default=None,
                    help="manifest to verify a replay's JSONL stdout "
                         "(reads stdin)")
    args = ap.parse_args()

    if args.verify:
        manifest = json.load(open(args.verify, "r", encoding="utf-8"))
        expect = manifest.get("replay_frames")
        if expect is None:
            print("VERIFY FAIL: manifest has no replay_frames (old "
                  "generator?) — regenerate the capture")
            sys.exit(1)
        got = []
        bad_json = 0
        summary = None
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                bad_json += 1
                continue
            if obj.get("event") == "au":
                got.append({"fi": obj["fi"], "len": obj["len"],
                            "sha1": obj["sha1"][:16],
                            "kf": obj["keyframe"],
                            "contig": obj["contiguous"]})
            elif "summary" in obj:
                summary = obj["summary"]
        ok = True
        if bad_json:
            print("VERIFY FAIL: %d unparseable stdout lines" % bad_json)
            ok = False
        if len(got) != len(expect):
            print("VERIFY FAIL: frame count %d != expected %d"
                  % (len(got), len(expect)))
            ok = False
        for i in range(min(len(got), len(expect))):
            g, e = got[i], expect[i]
            if (g["fi"], g["len"], g["sha1"], g["kf"], g["contig"]) != \
                    (e["fi"], e["len"], e["sha1"], e["kf"], e["contig"]):
                print("VERIFY FAIL: frame %d: got %s want %s" % (i, g, e))
                ok = False
                if i > 8:
                    print("  (further mismatches suppressed)")
                    break
        rc = manifest.get("replay_counters")
        if ok and summary and rc:
            pairs = [("authenticated", "auth"), ("fec_repaired", "fec_rep"),
                     ("stale", "stale"), ("gaps", "gaps"),
                     ("withheld", "withheld"), ("frames", "frames")]
            for sk, mk in pairs:
                if summary.get(sk) != rc.get(mk):
                    print("VERIFY FAIL: counter %s: replay %s != "
                          "reference %s" % (sk, summary.get(sk), rc.get(mk)))
                    ok = False
        n_lost = len(manifest.get("lost", []))
        print("VERIFY %s: %d frames byte-identical to the reference "
              "receiver (capture: %d records, %d lost injected, fec %d%%, "
              "reorder %.2f)"
              % ("PASS" if ok else "FAIL", len(expect),
                 manifest.get("packets_emitted", 0), n_lost,
                 manifest.get("fec_pct", 0), manifest.get("reorder_p", 0.0)))
        sys.exit(0 if ok else 1)

    if not args.out:
        ap.error("--out is required (or use --verify)")
    records, aus_meta, shard_len = build_capture(
        args.clip, args.vps, args.fec_pct, args.frames)
    emitted, lost = inject(records, args.loss, args.loss_mode,
                           args.reorder, args.seed, args.fec_pct)
    # the replay expectation IS the Python reference receiver's verdict on
    # the emitted record order (cross-implementation contract: the C replay
    # must reproduce it frame-for-frame, hash-for-hash)
    ref_frames, ref_rx = simulate_replay(emitted, args.vps)
    for m in aus_meta:      # keep the manifest small: original AU info
        del m["chunks"]
        del m["chunk_offsets"]
    wire = emitted
    if args.srtp:
        # the reference simulation runs on PLAINTEXT (post-auth entry); the
        # capture file carries the protected datagrams
        wire = protect_records(emitted, args.srtp)
    render_hexdump(wire, args.out)
    manifest = {
        "clip": os.path.relpath(args.clip, ROOT),
        "clip_sha1": hashlib.sha1(open(args.clip, "rb").read()).hexdigest(),
        "vps": args.vps, "shard_len": shard_len,
        "fec_pct": args.fec_pct,
        "frames": len(aus_meta),
        "packets_total": len(records),
        "packets_emitted": len(emitted),
        "lost": lost,
        "reorder_p": args.reorder,
        "seed": args.seed,
        "srtp": args.srtp,
        "replay_frames": ref_frames,
        "replay_counters": {
            "auth": ref_rx.authenticated,
            "fec_rep": ref_rx.fec_repaired,
            "stale": ref_rx.stale_packets,
            "gaps": ref_rx.gap_recoveries,
            "withheld": ref_rx.withheld,
            "frames": ref_rx.frames_emitted,
        },
        "aus": aus_meta,
    }
    if args.manifest:
        with open(args.manifest, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=1)
    size = os.path.getsize(args.out)
    print("capture: %s (%d records, %d bytes; frames=%d fec=%d%% lost=%d "
          "reorder=%.2f shard_len=%d)"
          % (args.out, len(emitted), size, len(aus_meta), args.fec_pct,
             len(lost), args.reorder, shard_len))


if __name__ == "__main__":
    main()
