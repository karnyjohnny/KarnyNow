# NVST receive pipeline (`kn_rx`) — implementation notes

Status: **stage-2 media/transport core IMPLEMENTED and sandbox-verified**
(native 9-suite gate, mingw x64+i686 `-Werror`, ASan+UBSan+LSan clean,
CMake+mbedTLS ctest, T10 POSIX dry-run, and the 6-case capture replay
matrix — all green this round). It has **never seen a real GFN packet**;
the first real traffic arrives with the stage-2 sockets/WSS+RTSP work.

Scope: the pure media path of OpenNOW's `NvstVideoReceiver` — SRTP session
unprotect (all four GFN profiles), the RTP reorder buffer, the FEC block
collector + reorder buffer (Cauchy repair via `knfec` + envelope re-wrap),
reference-frame invalidation (RFI), the NACK tracker + NACK-v2/RTCP/
ServerControl encoders, the frame-progress watchdog, the video timeout
state machine, and the AU-assembly consume path. NOT in scope (stage 3+):
the feedback STATISTICS plane (publish_stream_packet, frame-stage timings,
SRTCP Receiver Reports), the SCTP/WSS control transport that carries the
encoded commands, ICE/DTLS.

## Why this module exists

Stage 2's promise (BOOTSTRAP §12): *"Mjolnir, SRTP, GS header, access-unit
assembly; reorder and NACK-v2 survive injected loss"* — G2. `kn_gs` parses
single packets; `knfec`/`knsrtp` are crypto/algebra primitives. `kn_rx` is
the stateful glue between them and the decoder: the exact place where the
three costliest failure modes of a cloud stream live (out-of-order
delivery, packet loss, and the decoder-poisoning that follows an
unrecovered loss). It is its own module with its own KAT battery so
"video freezes/corrupts" narrows to *transport-state bug vs wire-format
bug vs decoder bug* before anyone debugs live (§7.9 philosophy).

## Provenance (every line verified against the live tree @ a49b124 THIS
## round — read, not remembered)

| Fact / rule | Source |
|---|---|
| `RtpReorderBuffer::{push,poll_deadline,drain}`: stale `< next_index`, duplicate, empty-buffer huge-jump resync (`index - next >= max` **only when the map was empty**), NACK = genuinely-absent range `(expected, first_available-1)`, gap_wait keyed by expected index, `gap_exceeded_window = first_available-expected >= max \|\| packets.len() >= max`, timeout 52 ms, recovery **clears the packets popped before the gap in the same drain** (`ready.clear()`) and jumps `next_index = first_available` | `nvst.rs:3537-3660` |
| `MJOLNIR_REORDER_DEQUEUE_TIMEOUT = 52 ms`; `DEFAULT_REORDER_WINDOW = 1024`; `MAX_REORDER_WINDOW = 2048` | `nvst.rs:229, 224-225` |
| `FecBlock`: `base_index = packet_index - shard_index` (saturating); `matches` = frame_index + block nibbles + data/parity counts + same base; duplicate shard slot → `DuplicateRtpPacket`; **insert resets `repair_failed`** (a new shard re-arms ONE reconstruction attempt); `has_enough = present >= data_shards`; `finish`: missing==0 → data shards move out untouched (no copy), any present shard longer than shard_len → Unsupported, **template = FIRST present shard (parity allowed — the 1-data/2-parity layout must survive losing its only data packet)** | `nvst.rs:3925-4117` |
| FEC re-wrap (pitfall #4): copy the template envelope `plaintext[..extension_start]` (`extension_start = template.payload_offset - 16`), patch **sequence** BE16 at `[2..4]`, **frame_index** LE32 at `ext+4..8`, **ext[11]** = `(last<<6)\|(block<<4)`; then STRICT `RtpHeader::parse`; flags `ext[8]&0x0f` validated (`& ~(SOF\|EOF\|PIC) != 0` → Unsupported); the **fec_word is NEVER re-validated** — recovered shards carry transport metadata that legitimately differs; `origin = RecoveredData` forces `is_fec = false` downstream | `nvst.rs:4055-4106`, force at `4786-4788` |
| `FecReorderBuffer`: shard_len = `video_packet_size + 16`; packets with `index < completed_through` are SILENTLY ignored (no drop event); inter-block gap → NACK + 52 ms/2048/128-block wait → recovery + `ready.clear()`; completable head → finish; **reconstruction failure → reinsert with `repair_failed = true`, NACK the first missing DATA run, arm gap_wait on it** (deferred to NACK, retried on any new shard); incomplete head: **a single buffered block with no prior gap_wait waits SILENTLY** (no NACK until a later block proves the head stalled), then NACK-first-missing-run, block_span/timeout/128-cap → drop head, recovery = full `missing_data_range`, `completed_through = base + data + parity`, continue (successors emit after the assembler was marked discontinuous) | `nvst.rs:4120-4310` |
| `MAX_PENDING_FEC_BLOCKS = 128`; `NVST_FEC_RTP_HEADER_ALLOWANCE = 16`; packetSize 256..65519 default 1280 | `nvst.rs:211, 215, 216-218` |
| Receiver pipeline order: state gate → `srtp.unprotect` → payload-type gate → SSRC gate (expected **or first-bound**) → bind ssrc → `last_authenticated` → initial-timeout clear + watchdog `authenticated(now)` → RFI `poll_backstop(250 ms)` → `authenticated_packets++` → highest-seq bookkeeping → `resolve_nack(index)` (retransmission recovery counter) → routing: **valid `FecPacketLayout` → fec_reorder (parity arrival counted), else → reorder**; DATA-shard Unsupported from the FEC push → `invalidate_picture` | `nvst.rs:4617-4726` |
| `consume_reorder_result` order: `fec_repaired +=` → NACK request (overflow → keyframe) → dropped event (stale counter) → recovery (`packet_gap_recoveries++`, `invalidate_picture`, **`clear_nacks`**, RecoveryNeeded) → per ready packet: `payload()` padding re-validation → `NvVideoPacket::parse` → RecoveredData forces `is_fec=false` → leaked parity → Unsupported → **SOF-before-EOF (assembler busy with a DIFFERENT frame) → invalidate + clear_nacks + FrameDiscontinuity event but the packet STILL starts the next AU (no `continue`)** → same-frame SPI continuity (`(last+1) & 0xFFFFFF`) violation → invalidate + FrameDiscontinuity + **skip the push** → assembler push → COMPLETE: `loss_before = !next_frame_contiguous`, RFI `admit` (withheld frames counted, never emitted) → watchdog `assembled` (closed episode → FrameProgressResumed) → keyframe clears the pending keyframe request → emit | `nvst.rs:4741-4862` |
| `invalidate_picture`: assembler reset + `last_stream_packet_index = None` + `next_frame_contiguous = false` + **`if !frame_invalidation.lose(now) { request_keyframe() }`** (RFI owns recovery once frame types are known; otherwise the immediate IDR request stays) | `nvst.rs:4867-4874` |
| RFI (`FrameInvalidation`): `lose` = ownership only when `frame_types_known`; `admit`: previous/gap tracking (`frame_index > previous+1` → gap → lose), typeless frame → keyframe-if-invalidating + clear + `contiguous = !loss_before && !invalidating`; I/IntraRefresh/RefPicI or keyframe → recovery counters + clear + `contiguous = invalidating \|\| !loss_before`; non-invalidating P → `contiguous = !loss_before`; invalidating P → RFI range `(previous+1 .. frame_index-1)` merged with pending, queued, **Header frames pass with contiguous=true**, everything else WITHHELD (dropped_frames++); `poll_backstop`: re-queue pending every 100 ms until sent, one-shot IDR backstop after 250 ms continuous loss; queue `take/restore/record_sent` in-flight semantics | `nvst_invalidation.rs:13, 45-269`; backstop timeout `nvst.rs:103` |
| NACK tracker: `request_nack` (dedup, 52 ms expiry purge, 2048 cap → overflow counts + **requests a keyframe**), `resolve_nack` = removed AND had been sent, `take_nack` = first eligible + contiguous run ≤ 64, eligibility `attempts < max && (never sent \|\| now-last_sent >= interval)`, Mjolnir policy: `interval = rtt + 4 ms`, `attempts = clamp(52/rtt, 1, 3)`, no-RTT == zero-RTT == (4 ms, 3); `mark_send_failed` rolls attempts back | `nvst.rs:946-985, 1272-1333`; constants `99-105` |
| NACK-v2 (0x0317): payload `[2, stream_ordinal, record_count]` + per record `u16LE base + u64LE bitmap(base+1..base+64)`, wrapping distance, duplicates skipped, 1..64 sequences else None, max framed 647 B | `nvst_control.rs:40-70`, `docs/mjolnir-nack-v2.md` |
| ServerControl codes: pacing 0x0203, frame-ack 0x0204, QoS 0x0207, **RFI range 0x0301** (`u64LE first, u64LE last, u64LE stream_index`), **IDR request 0x0302** (`u16LE 0`); framing `u16LE code + u16LE len`; frame_ack field constants (1, 9, `-1.0f32` = `0xBF800000` at 20..=48 step 4, 94, 98; f64 bits at 12); pacing (5, 2, error clamped to target, 0x341a); QoS `command()` does NOT encode `bytes_received`/the packet snapshot — ported as-is | `nvst_control.rs:3-181` |
| RTCP: PLI `[0x81,206,len=2,sender,media]`; generic NACK `[0x81,205,len=2+entries,...]`, FCI entries PID+BLP, range truncated to 64 packets / 4 entries, `current += 17` | `nvst.rs:3001-3048, 208` |
| `poll_timeout`: `last_authenticated.unwrap_or(timeout_origin)`, startup timeout until the first authenticated packet, on fire → `reset_media_state` + `RecoveryRequired` + one typed event (deliberate `recover()` required) | `nvst.rs:4526-4548` |
| Frame-progress watchdog: stall → KeyframePending + `request_keyframe` + event; `keyframe_grace` after the request → `RecoveryRequired`; base = first-keyframe-seen ? max(last_assembled_at, first_authenticated) : first_authenticated; **`assembled()` closes the episode only after the FIRST KEYFRAME** (a P-frame never closes it); production policy stall = grace = config.timeout | `nvst.rs:2422-2442, 4563-4615, 9124-9126, 9199` |
| `reset_media_state`: reorder+fec+assembler reset, `last_spi = None`, **`next_frame_contiguous = FALSE`** (the constructor's `true` is not restored — the first frame after any reset reports contiguous=false unless it clears invalidation), watchdog + invalidation reset | `nvst.rs:4946-4961` vs `4455` |
| SRTP `unprotect` order: parse → replay `guess` → cipher → replay `check` → **`payload()` padding validation on the PLAINTEXT** → `commit`. GCM: AAD = `packet[..payload_off)`, tag = trailing 8/16 B, IV = RFC 7714 builder; CM: tag over `packet[..auth_len] \|\| be32(roc)`, verified BEFORE decryption | `nvst.rs:2635-2705, 2711-2740` |
| Session material: CM128 = KDF128(enc16@0x00, auth20@0x01, salt14@0x02); CM256 = KDF256(32/20/14); GCM128 = KDF128(enc16@0x00, salt12@0x02); **GCM256 = KDF256(enc32, salt12) — the legacy `nvstVideo` default = AEAD_AES_256_GCM with the 8-byte tag policy**; KDF256 = the same construction with the AES-256-CM PRF (`Aes256Ctr = Ctr128BE` → the full-128-bit counter carry is right) | `nvst.rs:2576-2633, 2873-2900, 1348-1350, 2709-2710, 292-293` |
| Frame-type wire values: 1=P, 2=I, 3=Header, 4=IntraRefresh, 5=RefPicI, 6=NonRefP, else Unknown(x); `GsFrameHeader::parse` is **codec-INDEPENDENT** (two ordered arms: `47 53` needs header_size>=4 and masks 0x0f; `01/81` needs header_size>0 and keeps the raw byte); `clears_invalidation` = I/IntraRefresh/RefPicI | `nvst_invalidation.rs:17-43`, `nvst.rs:3279-3289` |
| Test-config shapes used by the KAT scenarios: window 4 (Rust test handoff), pt 96, ssrc 0x11223344, ts 0x01020304, `build_plaintext_rtp_with_fec_blocks` byte layout | `nvst.rs:10330-10348, 10506-10535` |

## Design

- **Zero allocation, caller-owned pools.** One bounded packet pool
  (`kn_rx_pool`: slot meta + byte arena, free-list) serves BOTH the reorder
  map and the FEC block shard tables; delivered/dropped/recovered slots
  return to the pool at exact, audited points (the unit tests assert
  `in_use` accounting). Reorder map = flat array sized window+1 with linear
  min/max scans (the reference BTreeMap only ever asks for the minimum
  above `next_index` and the maximum base — behaviour is identical; n ≤
  2049, entries live milliseconds).
- **Deterministic injected time.** Every call takes `now_ms` (any monotonic
  origin). No clock reads inside — the 52/100/250/4-ms policies replay
  identically in vectors, on host and on target. The production receive
  loop passes QPC-derived ms.
- **Frames by callback, events by bounded staging.** A completed+admitted
  AU invokes `kn_rx_frame_fn` synchronously (bytes alias the assembler
  buffer — consume or copy, the `kn_decode` contract). Non-frame events
  stage into a per-call array (overflow counted, aggregate drop counters
  stay exact). The reference's `Vec<Event>` per call maps 1:1 onto this.
- **Two entry points.** `process_datagram` = the production path (SRTP
  unprotect inside, datagram buffer modified in place — it is the receive
  scratch). `process_packet(index, plaintext)` = the post-authentication
  entry the replay/capture modes and all vectors use (explicit extended
  index, no crypto).

## §Parity construction (the FEC fold contract)

The algebra only reconstructs what the parity actually folded. The
canonical packet (matching the reference's own tests at `nvst.rs:13040-
13240` byte-for-byte) is:

```
parity_full = XOR_d cauchy(d, p, P) * zeropad(DATA_d, shard_len)   # FULL packets
parity_full[0..ext_start]              := parity's own RTP hdr + profile/words
parity_full[ext_start+4..+8]           := frame_index (real, LE32)
parity_full[ext_start+11]              := (last<<6)|(cur<<4)
parity_full[ext_start+12..+16]         := parity's own fec_word (pct, d+p, d)
ext_start+0..4 (spi word) and +8..11 (flags word low)  STAY ALGEBRAIC
```

Consequences (all test-pinned): a recovered data shard reconstructs its
**spi and flags exactly** (so same-frame SPI continuity and the flags
validation at 4089 pass), its **frame_index/ext[11] are patched from the
authenticated layout**, and its **fec_word is garbage by design** — hence
pitfall #4 (never validate it; `origin=RecoveredData` forces `is_fec=0`).
A recovered packet's length is **shard_len** (the reference `resize`), so
an AU containing a repaired packet carries trailing zero stuffing after
that packet's media — sanctioned by the reference's own comment ("AVC/HEVC
Annex B decoders tolerate those trailing zeroes"); AV1 needs the exact-
length hints instead. The capture generator + verifier encode exactly this
expectation (recovered-frame AUs are compared against the stuffed form).

## Deviations from the reference (bounded-C reality; ALL test-pinned)

- **D-a** Fixed pool instead of per-packet `Vec`s: pool/map/block-table
  exhaustion drops the packet with `KN_RX_DROP_POOL` and counts it
  (`alloc_failures`). The reference self-prunes via the same poll paths, so
  exhaustion needs sustained pathological loss.
- **D-b** FEC blocks declaring `data+parity > fec_max_shards` (≤ 255 = the
  knfec Cauchy coordinate bound, `nvst.rs:3806`) are rejected Unsupported;
  the reference stores up to 1023 but its reconstruct dies above 255
  anyway.
- **D-c** Feedback statistics plane deferred to stage 3 (needs SCTP/GCM
  SRTCP); the local counters mirror `stats_line`, and `last_assembled_at`
  is tracked receive-side (the reference publishes it from the same
  receive loop, `nvst.rs:9199`).
- **D-d** GCM + 256-bit profiles need the mbedTLS build
  (`KN_TLS_BACKEND_MBEDTLS` — the target default since the stage-2
  kickoff); bare builds refuse honestly (`KN_RX_ERR_NO_CIPHER` /
  `KN_RX_DROP_NO_CIPHER`), never fake crypto. The mbedTLS GCM API decrypts
  before verifying; the payload region is scrubbed on auth failure.
- **D-e** `kn_rx_result.ready` is bounded (`window+1`); a full ready list
  pauses the drain/complete loop instead of growing (state stays
  consistent, the next poll continues). Unreachable for reorder (map ≤
  window+1); for FEC it needs > 2049 completing data shards in one poll.
- The GsFrameHeader codec-gating bug and the `Some(Unknown(x)) != None`
  conflation in the round-XV assembler were FIXED this round in `kn_gs`
  (`frame_type_parsed`/`frame_type_known` added; parse made
  codec-independent) — `test_kngs` 125 checks unaffected.

## Landmines hit THIS round (all now pinned by tests)

1. **The reorder map is caller memory**: `kn_rx_reorder_init` MUST mark
   every entry unused — a garbage `used` byte from fresh `malloc` fakes
   buffered packets (spurious duplicates/phantom gaps). Caught by the
   scenario battery running on a malloc'd arena while the static-context
   unit tests passed. (Static zero-init hides allocation bugs — the
   vector runner must malloc.)
2. **Recovered packets are shard_len-sized** → vps derivation in
   `--replay-capture` must subtract the SRTP tag from the largest WIRE
   record; deriving it from the protected size inflates shard_len by
   tag_len and every repaired frame grows trailing garbage (caught by the
   E/F capture matrix: CM128 +10, GCM8 +8 on exactly the repaired AUs).
3. **Padding validation is split from the header parse** for SRTP: the pad
   byte is ciphertext on the wire. `kn_gs_rtp_parse_hdr` (structure only)
   + `kn_gs_rtp_payload` (bounds+padding on the plaintext) mirror the
   reference's parse/payload split; the old combined `kn_gs_rtp_parse`
   stays for plaintext callers (vectors unchanged).
4. **`ready.clear()` on gap recovery DROPS packets popped before the gap
   in the same drain** (nvst.rs:3648) — ported exactly (slots are
   released to the pool); it looks wrong and is deliberate: the recovery
   event replaces the drain's output.
5. **Reordering can complete an FEC block BEFORE a slow data shard
   arrives** — the block repairs from parity (algebraically fine) and the
   late shard is then silently ignored (`index < completed_through`).
   `fec_repaired` can exceed the true loss count; expectations must model
   emission order, not loss sets (the capture verifier runs the whole
   capture through the Python reference for exactly this reason).
6. **A single incomplete FEC block never NACKs** until a later block
   proves it stalled (`blocks.len()==1 && gap_wait.is_none()` → silent
   wait) — a NACK on the first missing shard would race the normal
   in-flight delivery.
7. `reset_media_state` sets `next_frame_contiguous = FALSE` (not TRUE like
   the constructor) — the first frame after pause/resume/recover/timeout
   reports `contiguous=false` even when it is a keyframe. Faithful.
8. **The generator caught its own bug through the pipeline**: a parity
   base-seq computed from `records[-d]` AFTER the first parity was
   appended produced a duplicate wire seq — the replay's SRTP replay
   window correctly rejected it and the frame cascade showed up as
   withheld frames. Two independent implementations disagreeing loudly =
   the system working as designed.

## The vector pipeline (`tools/rx_reference.py` → `kn_rx_kat_vectors.h`)

The generator is an independent line-by-line Python port of the same
reference ranges (reusing the already-proved `gs_reference`/
`fec_reference`/`srtp_reference` primitives). It **self-proves before
emitting** (363 checks):

- encoder goldens transcribed from the reference's own Rust tests
  (nack_v2 ×5 incl. the 64-record sparse layout + the round-trip grid,
  RTCP NACK wrap bytes, PLI, ctrl framing, IDR, RFI, frame_ack f64 bits,
  pacing clamp, QoS field map);
- the reorder 52-ms dequeue-deadline scenario, the NACK-exclusion
  scenario, both FEC byte-exactness tests (parity-only 1+2 with corrupted
  fecInfo; 3+1 with `0xa5`-poisoned fec_word — envelope/ext[0..12]/media
  restored, fec bytes NOT), the retain-following-blocks test incl. the
  original-allocation identity, and the `gaps_require_recovery` pipeline
  test — ALL transcribed with their exact Rust expectations;
- AES-256 ECB vs FIPS-197 C.3, GCM vs the spec case-1/case-2 vectors and
  the H = E(K0,0) constant, AES-128 cross-checked against
  `srtp_reference`'s proved implementation, KDF256 label/counter
  structure; then KDF256/GCM/CM256 SRTP session + datagram vectors
  derived from the proved primitives (the C side cross-verifies mbedTLS
  against them — true cross-implementation crypto proof);
- randomized properties: lossless permutations reproduce the baseline
  frame-for-frame; FEC-recoverable loss reproduces the baseline
  byte-for-byte with `fec_repaired == loss`; NACK ranges never contain a
  delivered or buffered index; frame indices strictly increase.

Emitted battery: **31 scenarios** (step scripts with per-step event/frame/
flag/state expectations + final counters), the NACK-tracker and RFI op
scripts, 6 SRTP profile vectors, and every encoder golden. The SAME runner
(`kn_rx_kat_runner.h`) executes it in `test_knrx` (verbose) and in the
shipped binary's `rx.vectors` self-test (silent, §7.9) — 126 checks bare /
153 with mbedTLS, ~1 ms.

## `knvstd --replay-capture` (the offline G2 mechanism)

```
python3 tools/make_rx_capture.py --out cap.hex --manifest cap.json \
        [--fec-pct 25] [--loss 6] [--loss-mode recoverable|fatal] \
        [--reorder 0.3] [--seed N] [--srtp cm128:10:KEYHEX:SALTHEX]
knvstd --replay-capture=cap.hex [--replay-au-dir=DIR] \
       [--replay-srtp=PROFILE:TAG:KEYHEX:SALTHEX] [--replay-vps=N]
python3 tools/make_rx_capture.py --verify cap.json < replay-stdout
```

The generator packetizes the repo's real H.264 clip
(`streamer/assets/synthetic.264`, 60 frames) into the canonical wire shape
(GS ext, SPI = data-only counter, per-frame FEC blocks, kn_ring_dump hex
records on a 33.3 ms frame timeline), optionally SRTP-protects every
record, and records the **Python reference receiver's verdict** (frames,
SHA-1s, contiguity, counters) in the manifest. The replay feeds the
records through the production C pipeline on the capture's own timeline
(52-ms deadlines included) and must reproduce that verdict byte-for-byte —
`--replay-au-dir` additionally writes the AUs as `.264` files for the
target-side `ffprobe` half of G2. Round-XVI matrix (all PASS, bare AND
ASan/UBSan builds): lossless; FEC25+loss6+reorder30%; fatal loss (RFI
withhold-until-IDR: 38/60 frames, identical on both sides); FEC50+loss8+
reorder20%; **SRTP-CM128** protected; **SRTP-GCM256** protected (mbedTLS
build). The replay's `vps` defaults to `max_plaintext_record - 16`
(tag-aware) and the manifest carries the generator's exact value
(`--replay-vps` overrides).

Exit codes: 0 = frames assembled and AU writes clean; 1 = no frames or AU
write errors (→ `kn_log_on_failure("session.replay", …)` → §7.7
amplification + auto-bundle; relevance mapping learned
`replay|capture|mjolnir`); 2 = usage; 3 = internal (init/alloc).

## Stage-2 remainders growing from here

- **Sockets**: the Mjolnir UDP receive loop (winsock, 8 MiB SO_RCVBUF per
  `nvst.rs:222`) → `process_datagram` + `poll` on a real clock → kn_ring
  capture of the raw datagrams (`--capture`); the loop must feed
  `gs_capture.hex`-format dumps so `gs.parse` and `--replay-capture` turn
  every real session into a regression test.
- **WSS + RTSP client** on the validated mbedTLS TLS-1.2 backend
  (`OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY`), the 31-field session context
  (R22) feeding `kn_rx_video_config` + SRTP material.
- **Control senders**: the encoders are done and KAT'd; the stage-3 SCTP
  channel will consume `kn_rx_nack_take` → `kn_rx_ctrl_nack_v2`,
  `kn_rx_inval_take_rfi` → `kn_rx_ctrl_frame_invalidation`,
  `kn_rx_video_take_keyframe_request` → `kn_rx_ctrl_idr_request` /
  `kn_rx_rtcp_pli` (RTT source: the ping tracker, `DEFAULT_NACK_RTT` 30 ms
  until then).
- **RTCP Receiver Reports** (SRTCP-GCM) when not `rtcp_on_sctp` —
  stage 3 (needs the GCM SRTCP index/E-bit path, `nvst.rs:4346-4410`).
- Audio (RED/Opus) + input share the same datagram plumbing later.
