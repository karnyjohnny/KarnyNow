#!/usr/bin/env python3
"""make_synthetic_clip.py — regenerate streamer/src/kn_synthetic_clip.h.

The decode.synthetic self-test (BOOTSTRAP §7.9) must decode a BUNDLED .264 —
embedded in the binary so the test is hermetic (the target's FFmpeg build has
no libx264 encoder, and external files can go missing).

Source clip: streamer/assets/synthetic.264 (checked into the repo). Regenerate it
with ANY ffmpeg that has libx264 (e.g. the gyan.dev essentials build already
on the target machine — NOT needed at runtime, only to remake the asset):

  ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=30:duration=2" \
      -c:v libx264 -preset ultrafast -tune zerolatency \
      -b:v 150k -maxrate 200k -bufsize 400k -g 30 -sc_threshold 0 \
      -pix_fmt yuv420p -f h264 streamer/assets/synthetic.264

Verify (must be exactly 60 frames, yuv420p, 320x240):
  ffmpeg -i streamer/assets/synthetic.264 -f null -

Then:  python3 tools/make_synthetic_clip.py [path/to/clip.264]
Emits: streamer/src/kn_synthetic_clip.h  (GENERATED — do not edit by hand)
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CLIP = os.path.join(ROOT, "streamer", "assets", "synthetic.264")
OUT = os.path.join(ROOT, "streamer", "src", "kn_synthetic_clip.h")

EXPECTED_FRAMES = 60          # 2 s @ 30 fps — asserted by the self-test
EXPECTED_W, EXPECTED_H = 320, 240
MAX_CLIP_BYTES = 96 * 1024    # keep the C array (~6 chars/byte) repo-friendly


def main(argv):
    clip = argv[1] if len(argv) > 1 else DEFAULT_CLIP
    with open(clip, "rb") as f:
        data = f.read()
    if not data:
        print("ERROR: empty clip %s" % clip)
        return 1
    if len(data) > MAX_CLIP_BYTES:
        print("ERROR: clip too big (%d B > %d B) — lower the bitrate" %
              (len(data), MAX_CLIP_BYTES))
        return 1
    # Annex-B sanity: must start with a start code
    if not (data[:4] == b"\x00\x00\x00\x01" or data[:3] == b"\x00\x00\x01"):
        print("ERROR: %s does not start with an Annex-B start code" % clip)
        return 1

    lines = []
    lines.append("/* kn_synthetic_clip.h -- GENERATED FILE, do not edit by hand.")
    lines.append(" * Generator: python3 tools/make_synthetic_clip.py")
    lines.append(" * Source:    %s (%d bytes)" %
                 (os.path.relpath(clip, ROOT).replace(os.sep, "/"), len(data)))
    lines.append(" *")
    lines.append(" * The bundled clip for the decode.synthetic self-test")
    lines.append(" * (BOOTSTRAP sec. 7.9: \"decode a bundled .264\"). Embedded so the")
    lines.append(" * test is hermetic: no encoder needed on the target, no external")
    lines.append(" * file to lose. Expected decode result: %d frames %dx%d yuv420p." %
                 (EXPECTED_FRAMES, EXPECTED_W, EXPECTED_H))
    lines.append(" */")
    lines.append("#ifndef KN_SYNTHETIC_CLIP_H")
    lines.append("#define KN_SYNTHETIC_CLIP_H")
    lines.append("")
    lines.append("#define KN_SYNTH_CLIP_LEN     %du" % len(data))
    lines.append("#define KN_SYNTH_CLIP_FRAMES  %d" % EXPECTED_FRAMES)
    lines.append("#define KN_SYNTH_CLIP_WIDTH   %d" % EXPECTED_W)
    lines.append("#define KN_SYNTH_CLIP_HEIGHT  %d" % EXPECTED_H)
    lines.append("")
    lines.append("static const unsigned char k_synthetic_clip[KN_SYNTH_CLIP_LEN] = {")
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk) +
                     ("," if off + 16 < len(data) else ""))
    lines.append("};")
    lines.append("")
    lines.append("#endif /* KN_SYNTHETIC_CLIP_H */")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("emitted %s (%d clip bytes -> %d header bytes)" %
          (OUT, len(data), os.path.getsize(OUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
