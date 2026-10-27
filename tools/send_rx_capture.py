#!/usr/bin/env python3
"""send_rx_capture.py — paced UDP sender for kn_ring_dump-format captures.

The offline stand-in for a real Mjolnir session: reads a .hex capture
(tools/make_rx_capture.py, or a real `knvstd --capture` recording) and
sends every datagram to host:port on the capture's own microsecond
timeline, so a `knvstd --capture`/`--decode` listener sees the same
arrival pattern the reference receiver was simulated against.

  python3 tools/send_rx_capture.py --in cap.hex --port 4798
  python3 tools/send_rx_capture.py --in cap.hex --port 4798 --speed 0   # blast

Timeline handling:
  --speed 1.0 (default)  wall-clock pacing: record i leaves at
                         t0 + (mono_us[i] - mono_us[0]) / speed.
                         Hybrid wait (coarse sleep until ~2 ms before the
                         deadline, then a perf_counter spin) keeps jitter
                         sub-millisecond even where Sleep() has a ~2.9 ms
                         floor (target R30).
  --speed 0              no waiting at all (blast): stress-tests the
                         socket backlog + pipeline under a compressed
                         timeline. All packets still arrive; the pipeline's
                         52 ms deadlines simply never fire mid-burst.

Line format (knring.c dump_visit / kn_gs_hexdump_parse_line, strict):
  "<mono_us> <meta_%08x> <flags> <len> <hex bytes...>"
Records whose stored len disagrees with the hex byte count are reported
and skipped (the C parser rejects them too — sender and receiver agree).

Stdout: ONE JSON summary line (json-lines contract):
  {"mode":"sender","records":N,"sent":N,"skipped":N,"bytes":B,
   "send_errors":E,"wall_ms":W,"speed":S,"host":"H","port":P}
Exit: 0 when everything was sent, 1 on any send error / empty capture,
2 on usage errors. Python 3.8-compatible (the target's interpreter).
"""
import argparse
import json
import socket
import sys
import time


def parse_line(line):
    """One kn_ring_dump line -> (mono_us, payload bytes) or None (bad)."""
    parts = line.split(" ")
    if len(parts) < 4:
        return None
    try:
        mono_us = int(parts[0], 10)
        int(parts[1], 16)            # meta (validated, not used)
        flags = int(parts[2], 10)
        length = int(parts[3], 10)
    except ValueError:
        return None
    if flags & 1:                    # truncated record: hex is partial
        return None
    hexbytes = parts[4:]
    if len(hexbytes) != length:
        return None
    try:
        payload = bytes(int(h, 16) for h in hexbytes)
    except ValueError:
        return None
    return mono_us, payload


def wait_until(deadline):
    """Hybrid precise wait: coarse sleep, then spin (sub-ms jitter)."""
    while True:
        now = time.perf_counter()
        remain = deadline - now
        if remain <= 0:
            return
        if remain > 0.002:
            time.sleep(remain - 0.002)
        # spin the last <= 2 ms


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--in", dest="inp", required=True,
                    help="capture file (kn_ring_dump hex lines)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--speed", type=float, default=1.0,
                    help="timeline scale (0 = blast, no pacing)")
    ap.add_argument("--start-record", type=int, default=0,
                    help="skip the first N records (resume experiments)")
    ap.add_argument("--max-records", type=int, default=0,
                    help="stop after N records (0 = all)")
    args = ap.parse_args()

    if args.port < 1 or args.port > 65535 or args.speed < 0:
        print(json.dumps({"mode": "sender", "error": "bad_args"}))
        return 2

    records = []
    skipped = 0
    with open(args.inp, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line:
                continue
            parsed = parse_line(line)
            if parsed is None:
                skipped += 1
                continue
            records.append(parsed)
    if args.start_record:
        records = records[args.start_record:]
    if args.max_records > 0:
        records = records[:args.max_records]
    if not records:
        print(json.dumps({"mode": "sender", "error": "empty_capture",
                          "skipped": skipped}))
        return 1

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.host, args.port)
    sent = 0
    send_errors = 0
    total_bytes = 0
    t0_wall = time.perf_counter()
    t0_cap = records[0][0]
    for mono_us, payload in records:
        if args.speed > 0:
            offset_s = (mono_us - t0_cap) / 1e6 / args.speed
            if offset_s > 0:
                wait_until(t0_wall + offset_s)
        try:
            sock.sendto(payload, dest)
            sent += 1
            total_bytes += len(payload)
        except OSError:
            send_errors += 1
    wall_ms = int((time.perf_counter() - t0_wall) * 1000)
    sock.close()

    print(json.dumps({"mode": "sender", "records": len(records),
                      "sent": sent, "skipped": skipped,
                      "bytes": total_bytes, "send_errors": send_errors,
                      "wall_ms": wall_ms, "speed": args.speed,
                      "host": args.host, "port": args.port}))
    return 0 if send_errors == 0 and sent > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
