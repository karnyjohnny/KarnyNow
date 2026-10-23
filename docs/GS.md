# NVST RTP/GS parse layer (`kn_gs`) — implementation notes

Status: parser **target-verified**; Stage 2 AU assembler implemented and
sandbox-verified (`test_kngs` 125 checks PASS; full ctest 10/10). The AU
assembler has not yet received real network packets; a later capture/replay
test will validate it against target traffic.

Scope v1 = analiza/04 §1-2 (wire formats). Stage 2 delta now adds
`kn_gs_au_assembler`: caller-owned AU storage, frame-header wire type,
AV1 short/extended length handling, 24-bit stream-index continuity, SOF/EOF
assembly, trailing-AUD stripping, keyframe metadata, 2 MiB default / 16 MiB
maximum cap, and discontinuity/keyframe-request signals. RTP reorder buffering
(nvst.rs:3537+), queueing and PLI/NACK feedback remain open.

## Why this module exists

Stage 2 consumes Mjolnir video: RTP packets on a dedicated UDP socket,
each video packet carrying a 16-byte GameStream ("GS") header in RTP
extension profile `0x4753`, **little-endian inside a big-endian packet**.
Three of the five costliest BOOTSTRAP pitfalls live in these 32 bytes:

1. GS body little-endian vs RTP big-endian (symptom: every frame index is
   nonsense),
2. `fec_current_block`/`fec_last_block` hide in **ext[11]** (top byte of
   the flags word), not a separate field (symptom: frame boundaries never
   trigger),
4. reconstructed FEC shards carry transport metadata that legitimately
   differs — the fec_word must NOT be re-validated on recovered packets
   (nvst.rs:4103-4106; symptom: every real cloud repair rejected).

The parse layer is therefore its own module with its own KAT battery, so
"video is corrupt" can be narrowed to *wire-format bug vs transport bug vs
decoder bug* before anyone debugs live (§7.9 philosophy).

## Provenance (all line numbers verified against nvst.rs @ a49b124 THIS
## round — read, not remembered; local copy `artifacts/nvst-extract/`)

| Fact / rule | Source |
|---|---|
| RTP fixed header: version check `first>>6==2`, CC→payload_off `12+4·cc`, pt=`p[1]&0x7f`, seq/ts/ssrc BE | `nvst.rs:2456-2510` (`RtpHeader::parse`) |
| Extension: profile+words BE16, `ext_len=words·4+4`; **foreign profiles are SKIPPED, not rejected**; GS profile requires `words·4 == 16` STRICTLY | `nvst.rs:2471-2499`; test `gs_extension_requires_exactly_four_words` @ `12099` |
| Padding: last byte = pad count; `pad==0 \|\| pad > payload_region` → InvalidPadding | `nvst.rs:2517-2525` |
| GS body: `spi=(LE32 ext[0..4] >> 8) & 0xFFFFFF`, `frame_index=LE32 ext[4..8]`, `flags=LE32 ext[8..12] & 0x0F`, `cur=(ext[11]>>4)&3`, `last=(ext[11]>>6)&3`, `pct=(LE32 ext[12..16]>>4)&0xFF`, `fec_index=(>>12)&0x3FF`, `fec_source=(>>22)&0x3FF` | `nvst.rs:3228-3249` (`NvVideoPacket::parse`) |
| `is_fec = pct != 0 && fec_index >= fec_source` (systematic code: data shards first) | `nvst.rs:3245` |
| `is_start_of_frame = SOF && cur==0`; `is_end_of_frame = EOF && cur==last` | `nvst.rs:3251-3257` |
| flags validation `flags & ~(SOF\|EOF\|PIC) != 0` — enforced on the FEC-RECOVERY path only (informational field elsewhere) | `nvst.rs:4089` |
| stream-index continuity for same-frame continuations: `idx == (last+1) & 0xFFFFFF` | `nvst.rs:4813-4824` |
| Start-code search: `windows(3)` — offsets `0..len-3`, 3-byte pattern tested first, 4-byte variant needs `b[off+3]==1` INSIDE the searched slice; search window `min(len, 68)` (64+4) | `nvst.rs:3525-3534`, `3481-3491` |
| H.264 payload location = first Annex-B start code in the window (NOT a fixed offset — the GS frame header length varies); fixed 8/20-byte headers are AV1-ONLY (`payload[0]` 0x01/0x81 — "similarly named but different consumer GameStream layout" warning) | `nvst.rs:3481-3497` |
| Keyframe: H264 `nal&0x1f==5`; H265 `(nal>>1)&0x3f ∈ 16..=21`; AV1 = frame-header hint | `nvst.rs:3500-3523` |
| Trailing AUD strip: LAST NAL is H264 type 9 / H265 type 35 → truncate at its start-code offset | `nvst.rs:3458-3479` |
| FEC layout: `parity = div_ceil(data·pct, 100)`; reject `data==0 \|\| pct==0 \|\| parity==0 \|\| shard>=total \|\| total>1023` | `nvst.rs:3677-3701` |
| Constants: RTP_FIXED_HEADER_LEN=12, NV_VIDEO_PACKET_LEN=16, profile 0x4753, flags PIC=1/EOF=2/SOF=4, INDEX_MASK=0xFFFFFF, MAX_GS_FRAME_HEADER=64, AU caps 2 MiB default /16 MiB max | `nvst.rs:79, 212, 236-237, 263-274` |
| kn_ring_dump line: `"%llu %08x %u %u"` + `" %02x"`×len + `"\n"` | `streamer/src/knring.c` (`dump_visit`) |

## Design

- **Pure, zero-allocation, no globals** — every function is `(buffers in →
  struct out)`. Safe from any thread; nothing to leak; usable from the
  receive loop, the FEC recovery path and the self-test alike.
- Error codes mirror `RtpParseError` variants with the reference's
  precedence (TooShort → Version → Csrc → Extension → Padding), so a
  disagreement shows up as a DIFFERENT CODE, not a silent pass.
- `kn_gs_hexdump_parse_line` parses kn_ring_dump text back into records —
  strict single-space format, exactly `len` hex-byte tokens, trailing
  space/token rejected (matches python `split(" ")` verdicts, because the
  vectors are the contract); capacity check comes AFTER format validation
  so a malformed line never reports `HEX_CAP`.
- The header exposes the wire constants (flags, profile, 68-byte window,
  AV1 header sizes, AU caps) so stage-2 code cannot re-invent them.

## The vector pipeline (`tools/gs_reference.py` → `kn_gs_kat_vectors.h`)

The generator is an independent line-by-line Python port of the same
nvst.rs ranges. It **self-proves before emitting** (the srtp_reference
rule — it earned its keep again this round):

- hand-written byte goldens for the GS body and the RTP+ext header
  (derived from the spec tables independently of the builder);
- discriminators: the golden's BE reading of frame_index MUST differ from
  the LE one, and ext[8]/ext[12] nibble misreadings MUST differ from the
  ext[11] extraction — a vector that cannot tell the pitfalls apart is
  worthless;
- 700 randomized build→parse round-trips (video body, full packets with
  CSRC/padding, hexdump lines);
- start-code window boundary cases (3-byte at 65 found; 4-byte needing
  byte 68 rejected) and div_ceil FEC arithmetic edges.

The generator caught, before anything shipped: a `w32be(profile)[:2]`
builder bug (emitted `00 00` instead of `47 53`), six wrong `eof`
expectations, the AUD-strip zero-MERGE case (a body ending in `00` fuses
with a following `00 00 01` into a 4-byte start code → truncation lands at
7, not 8 — kept as a vector because zero-stuffing is common in real
streams), an out-of-range FEC shard, and two truncation slices that were
no-ops.

Emitted battery: **78 vectors** — 16 RTP packets (incl. every error
precedence case), 13 GS bodies, 13 payload-location cases, 8 keyframe,
7 AUD-strip, 8 FEC-layout, 6 continuity (incl. the 2^24 wrap), 7 hexdump
lines. `test_kngs.c` adds the C-side property round-trips (a SECOND
independent encoder in C: 200 GS bodies + 200 packets + 100 hexdump
render/parse, deterministic xorshift seeds) plus pitfall discriminators,
AU assembly/AV1-length/continuity/cap tests, and arg/error paths — 125 checks
total.

## `gs.parse` self-test (§7.9)

- Runs the 78-vector battery from the SHIPPED binary (portable — executes
  on host and target alike; the registry entry is no longer a stub).
- Optional REAL capture: `KN_GS_CAPTURE=<path>` or `./gs_capture.hex`
  (kn_ring_dump text). Parses every line, then RTP+GS on each record ≥12 B;
  evidence carries `cap_lines/cap_bad/cap_rtp_ok/cap_rtp_err/cap_gs/
  cap_sof/cap_eof/cap_fec`. No capture → the reason says so explicitly
  ("none (stage 2 records real dumps)") — never dressed up as a pass.
- Relevance mapping (§7.7): video-flavoured failures
  (`video|decode|garbl|corrupt|d3d|present|stretch|gpu|nack|reorder|
  reassemb|pli`) re-run `gs.parse` + decode + d3d probes into the bundle.
  **Landmine recorded:** a bare `rtp` keyword substring-matches inside
  `srtp` and dragged the video battery into every SRTP failure — caught by
  the host suite before shipping.

## Stage-2 TODOs growing from here

- Reorder buffer + `invalidate_picture` semantics: new SOF before EOF ⇒
  previous frame incomplete ⇒ drop + PLI (nvst.rs:4800-4810); same-frame
  continuity is already enforced by the AU assembler and
  `kn_gs_index_continuous`. Use bounded caller-owned packet storage and test
  wrap, duplicate, stale, gap timeout, and capacity overflow.
- Capacity-2 encoded AU queue per analiza/04 §2.4; overflow must invalidate
  reference continuity and request a keyframe.
- FEC recovery path: `kn_gs_fec_layout_parse` → knfec reconstruct →
  re-wrap with the RTP envelope template (nvst.rs:4075-4106) — remember
  pitfall #4: do NOT re-validate the fec_word of recovered shards; the
  flags check at 4089 IS enforced there (`flags_valid` field is shipped
  for exactly this).
- SRTP ordering: `knsrtp unprotect` → `kn_gs_rtp_parse` → `kn_gs_video_parse`
  → `kn_gs_picture_payload` → assembler (parse AFTER authentication —
  the GS extension is inside the encrypted payload's RTP packet).
- Real captures: stage-2 receive loop writes `kn_ring_dump` files; drop
  them next to knvstd as `gs_capture.hex` and `gs.parse` turns into a
  real-traffic regression test.
