#!/usr/bin/env python3
"""bisect_row.py — one row of the bisect report out of one knvstd flight log.

Why this exists (round N slice 4s): the 0x0101 rig heartbeat turned out to be
the cheap oracle for "will this session live". The run-46 keeper logged 1001 of
them (~1 Hz from the first second); today's HEAD draws log 0-2 and die at 12-15 s.
That is visible ten seconds into a flight, so a bisect can use two-minute probes
instead of twenty-minute flights. This script reads the fields that decide it and
prints them, so nobody has to grep a 90 kB JSON-lines file by hand on Win7.

It also catches the trap that invalidated run 200: a build without FFmpeg reports
"decoder":"pipeline-only" and "video":"0x0", receives media but consumes none, and
its flight must not be compared with a decoding build.

Usage (rig, ConEmu, Python 3.8):
    python tools\\bisect_row.py logs\\bisect_a339b52_1.log
Usage (sandbox):
    python3 tools/bisect_row.py /tmp/live_d1.log

Pure stdlib, Python 3.8-compatible. Tolerates both JSON styles the tree has
emitted over time (older builds print '"event": "x"' with a space after the
colon, newer ones '"event":"x"').
"""
import io
import re
import sys

# Heartbeats below this count over a 120 s probe mean the rig never really
# started beating; at ~1 Hz a healthy flight reaches ~110.
HEALTHY_HB = 30
PARTIAL_HB = 3


def main(argv):
    if len(argv) != 2:
        print("usage: bisect_row.py <flight log>")
        return 2
    path = argv[1]
    txt = io.open(path, encoding="utf-8", errors="replace").read()

    def one(pat, default="?"):
        m = re.search(pat, txt)
        return m.group(1) if m else default

    hb = [int(x) for x in re.findall(r'\["0101", ?(\d+)\]', txt)]
    hbn = max(hb) if hb else 0
    dec = one(r'"decoder": ?"([^"]+)"')
    vid = one(r'"video": ?"([^"]+)"')

    print("plik        : %s" % path)
    print("profil      : %s   seat: %s%s   rig: %s   relearns: %s" % (
        one(r'"profile": ?"(\w+)"', "-"),
        one(r'"event": ?"cm_created".{0,200}?"phase": ?"(\w+)"'),
        " (WZNOWIONY)" if one(
            r'"event": ?"cm_created".{0,260}?"resumed": ?(\d)', "0") == "1" else "",
        one(r'"event": ?"ready","?rtsps"?: ?"rtsps://([0-9.\-]+)\.'),
        one(r'"relearns": ?(\d+)')))
    print("wynik       : stop=%s  wall=%s ms" % (
        one(r'"stop_reason": ?"(\w+)"'), one(r'"wall_ms": ?(\d+)')))
    print("rig nadawal : wideo do %s ms, kontrolna do %s ms, frames_obs=%s, frames_dec=%s"
          % (one(r'"last_video_rel_ms": ?(\d+)'), one(r'"last_ctrl_rel_ms": ?(\d+)'),
             one(r'"frames_observed": ?(\d+)'), one(r'"frames_decoded": ?(\d+)')))
    print("heartbeaty  : 0x0101 = %d   pings=%s pongs=%s" % (
        hbn,
        one(r'"ctrl_ping_stop".{0,80}?"pings": ?(\d+)'),
        one(r'"ctrl_ping_stop".{0,120}?"pongs": ?(\d+)')))
    print("dekoder     : %s   video=%s" % (dec, vid))
    if dec == "pipeline-only" or vid == "0x0":
        print("BUILD       : *** BRAK FFMPEG - ten lot sie NIE LICZY, "
              "zbuduj ponownie ***")
    else:
        print("BUILD       : ok (dekoduje i prezentuje)")
    if hbn >= HEALTHY_HB:
        print("WERDYKT     : ZDROWY - heartbeaty ~1Hz przez caly lot")
    elif hbn >= PARTIAL_HB:
        print("WERDYKT     : CZESCIOWO - %d heartbeatow, potem cisza" % hbn)
    else:
        print("WERDYKT     : MARTWE SERCE - %d heartbeatow "
              "(rig nigdy nie zaczal bic)" % hbn)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
