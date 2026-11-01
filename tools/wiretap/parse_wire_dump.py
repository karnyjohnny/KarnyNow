#!/usr/bin/env python3
"""parse_wire_dump.py — turn an F34 --wire-dump file into a readable
transcript. Each 'W'/'R' record (direction line + one hex line) becomes
one entry; printable payloads are decoded as text (HTTP/RTSP/WS frames
live here), binary payloads are summarized. Redacted bytes arrive as
'X' (0x58) runs — left as-is, they are honest placeholders.

Usage:
    python3 tools/wiretap/parse_wire_dump.py DUMP.txt [out_transcript.txt]
    python3 tools/wiretap/parse_wire_dump.py DUMP.txt --find sessionRequestData
"""
import re
import sys

HDR = re.compile(r"^([WR]) t_ms=(\d+) len=(\d+) red=(\d+)( truncated)?$")


def parse(path):
    recs = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        pending = None
        for line in f:
            line = line.rstrip("\n")
            m = HDR.match(line)
            if m:
                pending = (m.group(1), int(m.group(2)), int(m.group(3)),
                           int(m.group(4)), bool(m.group(5)))
            elif pending is not None:
                try:
                    data = bytes.fromhex(line.strip())
                except ValueError:
                    data = b""
                recs.append((pending, data))
                pending = None
            # '#' header lines and anything else: ignored
    return recs


def as_text(data):
    """Return text if the payload is >=85% printable ASCII, else None."""
    if not data:
        return ""
    printable = sum(1 for b in data
                    if 32 <= b < 127 or b in (9, 10, 13))
    if printable * 100 >= len(data) * 85:
        return data.decode("ascii", errors="replace")
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    find = None
    out = None
    args = sys.argv[2:]
    if args and args[0] == "--find":
        find = args[1] if len(args) > 1 else ""
        args = args[2:]
    if args:
        out = args[0]

    recs = parse(path)
    lines = []
    for (d, t, ln, red, trunc), data in recs:
        tag = "%s t=%8dms len=%-7d red=%d%s" % (
            d, t, ln, red, " TRUNC" if trunc else "")
        txt = as_text(data)
        if txt is not None:
            if find and find.lower() not in txt.lower():
                continue
            lines.append("=== %s (text) ===" % tag)
            lines.append(txt)
        else:
            if find:
                continue
            lines.append("=== %s (binary %d B, first 64: %s) ===" % (
                tag, len(data), data[:64].hex()))
    report = "\n".join(lines)
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write(report + "\n")
        print("wrote %s (%d records, %d shown)" % (out, len(recs),
              report.count("=== ")))
    else:
        print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
