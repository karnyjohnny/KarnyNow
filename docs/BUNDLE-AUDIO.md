# Bundle-audio extraction (`kn_arx` + `kn_ogg`) — F11, round XLVIII

Status: **IMPLEMENTED, sandbox-proven, target-PENDING.** Native battery
green (`test_knogg` 163 checks, `test_knarx` 62, `audio.vectors`
self-test 188, all byte-exact against the Python references); cross
`-Werror` ×2 arches clean; ctest **22/22** on BOTH TLS backends; T10
POSIX dry **72**. The path has **never touched the production cloud** —
run 30 is its first instrumented flight (§ Run 30 below).

> **Not to be confused with `docs/AUDIO.md`**: that file documents
> `kn_audio`, the WASAPI *playback* engine (stage 1, synthetic PCM).
> THIS file documents the *receive/extract* side of the rig's audio:
> SRTP datagrams in, an OggOpus file out. Playback of the captured
> stream (decode to PCM → `kn_audio`) is a deliberately deferred
> stage-3 follow-on.

## 1. Where the audio lives (topology, all from the reference)

- The rig sends audio as **SRTP RTP on the SAME ICE/DTLS bundle socket**
  as video (`rtcAudioOnNativeBundle:1`; the reference admits it via
  `audio_streams`, nvst.rs:8620-8627). There is no second media port.
- The audio track is **hardcoded** in the reference's SDP builder
  (`nvst_rtsp.rs:1309`): `m=audio 5006 RTP/SAVPF 111`, mid "0",
  **PT 111 = Opus, 48 kHz, 2 channels** (`GFN_OPUS_PAYLOAD_TYPE`,
  nvst.rs:276), **PT 63 = RED** (`GFN_RED_PAYLOAD_TYPE`, nvst.rs:275).
  The SSRC is ANY (not negotiated — the reference does not filter it).
- Demux rule (already live since round XXXVI, now consumed): a datagram
  on the bundle with `v==2` and second byte in 192..223 is RTCP
  (`bundle_rtcp_in`), otherwise RTP (`bundle_rtp_in`). XLVIII feeds
  every bundle RTP datagram to `kn_arx_process` when armed; the PT
  filter inside `kn_arx` (111/63 only) keeps stray video-mirror or
  RTX packets out (`pt_filtered`).

## 2. Keying — RFC 5764 §4.2 (DTLS-SRTP exporter)

`use_srtp` is offered in our ClientHello since round XXXV and the rig
answers with profile **0x0001 = SRTP_AES128_CM_HMAC_SHA1_80**
(`srtp_prof:1` in every `dtls_ready` event; runs 22–29g). str0m is not
vendored, so RFC 5764 §4.2 governs directly:

```
keying_material = TLS-PRF(SHA256, master_secret,
                          "EXTRACTOR-dtls_srtp", client_random||server_random, 60)
                = [client_key 16][server_key 16][client_salt 14][server_salt 14]
```

- `kn_dtls` registers mbedTLS's **export-keys callback**
  (`mbedtls_ssl_set_export_keys_cb`, `MBEDTLS_SSL_KEY_EXPORT_TLS12_
  MASTER_SECRET`) at setup; the callback captures master+randoms in
  private struct fields. `MBEDTLS_SSL_DTLS_SRTP` was already enabled via
  the pinned user config (round XXXV).
- **`int kn_dtls_srtp_export(const kn_dtls *d, uint8_t out[60])`** runs
  `mbedtls_ssl_tls_prf` over the captured material. Returns 0 only when
  ready AND profile 0x0001 AND material captured; **−1 in every
  stub/schannel build** (honest no-op — audio extraction is
  mbedTLS-only, mirroring the video SRTP backend split).
- The **receive direction = the SERVER half**: `out+16` (key) and
  `out+46` (salt) feed `kn_arx_set_srtp`, which runs the RFC 3711 KDF
  (`kn_srtp_kdf128`, labels 0/1/2) → enc/auth/salt session keys and
  arms the 2048-window replay list.
- Key hygiene: the 60 bytes live on the arm-site stack frame, are
  `memset`-scrubbed immediately, and are NEVER logged (§7 discipline:
  shapes and ids in the log, never keys). `kn_arx_session_keys()`
  exists for KAT cross-checks only.

## 3. `kn_arx` — the audio receive pipeline (streamer/src/kn_arx.c)

Contract (`kn/kn_arx.h`): `kn_arx_init(a, emit, ectx)` →
`kn_arx_set_srtp(a, master_key16, master_salt14)` →
`kn_arx_process(a, datagram, len, now_ms)` per bundle datagram.
Return: **1** = an Opus payload was emitted, **0** = not audio for us
(`pt_filtered`), **−1** = rejected (the counters say which).

The **14-step order** is pinned by `tools/arx_reference.py` (its
docstring is the normative spec) and the C follows it EXACTLY — the
order is semantics, not style:

1. length sanity (`len < 12` or `len > KN_ARX_MAX_PKT (1600)` →
   `malformed`);
2. `v==2` header check + header walk (CC/X extension; **`hlen+10 > len`
   = `malformed`, NOT srtp_fail**);
3. PT filter (111 or 63, else `pt_filtered`, rc 0);
4. SSRC-change detection → **`ssrc_changes`, replay window + seq state
   reset (D-a2, per-epoch)** BEFORE the replay guess;
5. ROC guess via `kn_srtp_replay_guess`;
6. **replay check BEFORE seq accounting** — a true duplicate hits
   `replay_reject` and `seq_dup` stays 0 (the reference behaviour the
   KAT pins);
7. no key yet → `srtp_nokey`;
8. SRTP unprotect (`kn_srtp_unprotect_cm_hmac`, tag 10) → auth failure =
   `srtp_fail`; replay commit;
9. **seq accounting immediately after the SRTP commit** (loss/reorder/
   dup from the extended sequence; wrap pinned: the post-0xFFFF packet
   is tagged at ROC 1 — `seq_wrap` KAT group);
10. padding: `padded++` **on the P bit even if the pad length later
    proves invalid** (pinned), then pad removal (invalid → `malformed`);
11. RED (PT 63): RFC 2198 block walk; blocks counted (`red_blocks`)
    **only for VALID tiling** — a partial/overflowing block layout is
    `red_invalid`, rc −1; valid RED emits every block (`red_packets`);
12. TOC parse (RFC 6716 §3.1): config → frame size via the 32-entry
    table (`toc_invalid` on reserved configs 12-15 / self-delimited
    framing we never see);
13. payload emit **in arrival order (D-a1)** — no reordering buffer
    (the video side withholds; audio extraction stays honest-simple:
    late packets that pass replay are emitted when they arrive);
14. stats: `packets`, `payload_bytes`, `first_ms`/`last_ms` from
    `now_ms` (any monotonic origin), `consumed` = every datagram fed.

Counters (17 + 2 timestamps, all in `kn_arx_stats`): consumed, packets,
payload_bytes, red_packets, red_blocks, red_invalid, pt_filtered,
malformed, toc_invalid, padded, seq_loss, seq_reorder, seq_dup,
ssrc_changes, srtp_fail, replay_reject, srtp_nokey, first_ms, last_ms.

## 4. `kn_ogg` — the OggOpus muxer (streamer/src/kn_ogg.c)

Capture-file framing per RFC 3533 (Ogg) + RFC 7845 (OggOpus):

- Caller-owned page scratch (`KN_OGG_PAGE_BUF` = 27 + 255 + 16384);
  zero-allocation by contract; sink = `kn_ogg_write_fn` callback.
- `kn_ogg_mux_begin` emits **OpusHead** (19 B, family 0, **preskip 0 —
  D-o3**: we do not own the encoder's pre-roll; extraction is
  bit-transparent) + **OpusTags** (vendor "KarnyNow"), both at
  granulepos 0. Serial **0x4B4E4F47** ("KNOG").
- **granulepos = Σ TOC samples** of completed packets (48 kHz clock —
  the TOC duration table is shared with `kn_arx`).
- Flush policy **D-o1**: a page is flushed at 255 segments, 16384 B
  payload, or 48 packets. **D-o2: no continuation pages** — every Opus
  packet starts on a fresh packet boundary (lacing lengths ≤ 255 are
  guaranteed by the 1275×5 B Opus cap; an oversized packet is
  `dropped_packets`, honest skip, not a stream abort).
- `kn_ogg_mux_finish` emits the final page with the EOS bit (an empty
  EOS page when the last packet exactly filled its page). Idempotent.
- CRC: the Ogg polynomial 0x04c11db7 table is **generated, never
  hand-typed** (the first hand-written table shipped 160/256 entries
  with errors from index 24 — caught by the Python bitwise differential
  in `test_knogg`; the in-file table is now byte-identical to the
  generator's).
- `write_errors` latches sink failures; the muxer then refuses further
  output (the file is untrustworthy from the first short write).

## 5. Two-implementations rule (wire formats, standing agreement)

| C (shipped) | independent Python reference | vectors |
|---|---|---|
| `kn_ogg.c` | `tools/oggopus_reference.py` (muxer + CRC + TOC table; `--check` self-proves against RFC examples before emitting) | `streamer/src/kn_ogg_kat_vectors.h` (4 CRC, 121 TOC, 5 byte-exact scripts — 3296 lines) |
| `kn_arx.c` | `tools/arx_reference.py` (ArxModel: the same 14 steps + real RFC 3711 SRTP via `srtp_reference.py` primitives; 12 groups / 21 packet vectors) | `streamer/src/kn_arx_kat_vectors.h` (KDF session-key KAT + per-packet rc/emit-hex/samples + per-group counter finals) |

The batteries run in three places: `test_knogg` / `test_knarx` (unit,
with differential + property legs), the `audio.vectors` self-test in
the PRODUCTION binary (`kn_audio_rx_kat_runner.h`; 188 checks, gated),
and ctest on both TLS backends. Property legs: `test_knogg` compares
the C CRC against a bitwise-computed one for random buffers and walks
all 32 TOC configs × 4 durations; `test_knarx` round-trips real
`kn_srtp_protect_cm_hmac` packets (including an in-test-built RED
packet) through `kn_arx_process`.

### RFC 6716 TOC trap (locked in both implementations)

Configs 16-31 are CELT NB/WB/SWB/FB × 2.5/5/10/20 ms
(120/240/480/960 samples @48 kHz): **`0xC0` = cfg 24 = SWB 2.5 ms =
120 samples**, and CELT-FB 20 ms is **cfg 31 → TOC `0xF8` = 960**
(NOT `0xE0` — that is FB 2.5 ms = 120). The reference model uses
`0xF8` as its default 20 ms packet; a mis-derived table shifts every
granulepos. **What the GFN rig actually sends** (measured runs 30–32):
TOC **`0xEC`** = config **29** = CELT-FB **5 ms**, `s=1` stereo, `c=0`
one frame = **240 samples/packet at ~200 packets/s**.

## 6. knvstd wiring (`knvstd_main.c`, stamp L)

- **CLI**: `--session-audio-out=F.opus` (help text updated; no file →
  counters still run, no capture). Threaded through `run_session(…,
  audio_out, draw_index, log_dir)`; both call sites (single =
  `draw_index` 0, lottery = the draw number). **XLIX/F13**: a lottery
  draw writes its OWN file `<stem>_d<draw><ext>` (`audio.opus` +
  `--lottery=4` → `audio_d1.opus … audio_d4.opus`).
  **L/F16 — the rule now lives in `kn_drawpath.c`, not inline.** The XLIX
  derivation took `dot = strrchr(audio_path, '.')` — a pointer INTO the
  destination buffer — and then `memcpy`'d the extension forward: an
  overlapping copy (undefined behaviour; ASan `memcpy-param-overlap` on
  the rig). Run 31 delivered the proof on disk: **`audio_d1_d1us`,
  `audio_d2_d2us`, `audio_d3_d3us`, `audio_d4_d4us`** — suffix doubled
  AND the `.opus` tail overwritten by the copy's NUL, so the files had
  **no extension at all** (their byte sizes match the four `audio_close`
  counters exactly, so only the NAME was wrong; rename one to `.opus` and
  it is a normal OggOpus stream). `kn_drawpath_suffix_inplace()` copies
  stem/suffix/extension through LOCAL buffers and uses `memmove` for
  every write, so the in-place idiom the caller actually uses is defined
  behaviour. Pinned by `test_kndrawpath` (**1051 checks**, the 21st
  suite) three ways: the run-31 strings verbatim, a SECOND independent
  `snprintf` builder across a path × draw matrix, and honest degradation
  (a too-small buffer keeps the ORIGINAL path and sets `truncated`). The
  SAME call now names the F17 video stream (`video_d<N>.264`) and the
  draw-numbered AU dumps, so all three capture files follow one tested
  rule. Path building still never uses `snprintf("%s")` —
  `-Wformat-truncation=2` cannot prove it, and the paranoid knvstd leg in
  `build_native_tests.sh` re-checks that class every round.
- **Arm** (inside the `dtls_ready` event, right after `srtp_prof`):
  `kn_dtls_srtp_export` → `kn_arx_init/set_srtp(server half)` →
  `fopen` + `kn_ogg_mux_init/begin` → one stdout event + one knlog
  MEDIA line:
  `{"mode":"session","event":"audio_arm","srtp_derived":0|1,"armed":0|1,
    "profile":1,"pt":111,"red_pt":63,"channels":2,"clock_hz":48000,
    "file":0|1}`
  Any failure degrades honestly (`armed:0`); **video is never touched
  by the audio path's failures**.
- **Demux**: every bundle RTP datagram → `kn_arx_process` (armed only);
  the pump mirrors `arx.st` + Ogg totals into `pump.st.audio_*` after
  each packet so the snapshot handshake stays race-free (the pump.st
  discipline).
- **Close** — **XLIX/F12 (was dead code in XLVIII)**:
  `sess_audio_file_close(&pump, &ice_st)` runs `kn_ogg_mux_finish`
  (EOS) + `fclose` + the ogg-totals mirror + the `audio_close` event
  (pages/bytes/packets/payload_bytes/red_packets/srtp_fail/seq_loss/
  write_errors) + the knlog MEDIA line. It is called **immediately
  after `sess_pump_stop()`** on both live teardown paths (D21: the pump
  thread is the single writer — once joined, main owns the FILE*; that
  is the race the XLVIII `!pump_persist` gate was avoiding, and joining
  first removes it), plus once defensively at the `teardown:` label.
  Idempotent (`audio_file_on` is cleared), so `audio_close` fires
  **exactly once per draw**. The XLVIII gate was wrong: `pump_persist`
  is 1 for every live session, so the block never ran — run 30 has
  `audio_arm` ×2 and `audio_close` ×0, no EOS page, and a leaked draw-1
  FILE* across the lottery gap (the 2 505-byte scrambled page at the
  draw boundary in the delivered `audio.opus`).
- **Summary**: the 34-key `"audio"` block sits after `"recovery"`:
  armed, srtp_derived, profile, pt, red_pt, channels, clock_hz,
  bundle_rtp_in, bundle_rtcp_in, the 17 kn_arx counters + first_ms/
  last_ms, `ogg{pages,bytes,dropped_packets,write_errors}`, file
  (1 = the file was opened and written). New skeleton canon:
  **113 recursive keys** (79 through XLVII + 34), validated by
  `tools/check_summary_skeleton.py` (replaces the old block-scan
  validator and its 108-vs-112 mystery: the new tool diffs the printf
  skeleton's key SET against a live summary with drops/outbox_by_sid
  emptied — run it on every stdout-shape change).

## 7. Run 30 — first flight (expectations + reaction matrix)

Command (NO `--session-au-dir` — at the time, the ~600 s ceiling was
believed to make AU dumps pointless, §36. **XLIX withdrew the ceiling
reading** (§37.4) but the choice stands for a better reason: the audio
file *is* the content probe, and run 30's draws only lasted 75 s /
197 s of media):

```
knvstd.exe --session --session-token=<TOKEN> --session-app=<APP> \
  --session-seconds=570 --lottery=2 --lottery-gap=30 \
  --session-audio-out=audio.opus > logs\run30.log
```

Healthy draw: `audio_arm` with `srtp_derived:1 armed:1 file:1`;
summary `audio.packets` ≈ **38k–120k** (the rig sends **200 pkt/s of
5 ms Opus** — measured in runs 30, 31 AND 32; see §10.1 — so ~570 s of
media ≈ 114k packets; RED doubles the payload count while the packet
count stays at one primary per RTP datagram — anything in the band with
`red_packets == packets` is fine);
`srtp_fail:0`, `malformed:0`, `toc_invalid:0`, `seq_loss` small;
`ogg.pages` ≈ packets/48 + 3; `audio.opus` plays ≈570 s stereo 48 kHz
(mpv/VLC); `stop_reason:"time"` BELOW the 594 s floor.

| observation | reading | action |
|---|---|---|
| `armed:0, srtp_derived:0` | exporter failed (profile ≠ 1 or export-keys cb never fired on the Win7 mbedTLS build) | check `dtls_ready.srtp_prof`; if 1, the callback registration is the suspect — ship a diag counter next round |
| `armed:1`, `packets:0`, `bundle_rtp_in:0` | the rig sent no bundle RTP (audio off for this app/draw) | retry with a different app; not a client bug |
| `bundle_rtp_in` > 0 but `pt_filtered` ≈ all | PT is not 111/63 for this rig | log a PT histogram next round (cheap addition) |
| `srtp_fail` ≈ all packets | key half/label mismatch (client vs server half) | flip to `km+0`/`km+32` in a diagnostic build — the KAT pins the derivation, so this would be a layout surprise |
| `packets` healthy, file unplayable | muxer/framing issue | `audio_close.write_errors` + `ogg.dropped_packets` localize it; send the .opus back |
| `seq_loss` huge, `packets` healthy | audio SSRC epoch churn (rig restarts the stream) | benign if `ssrc_changes` tracks it (D-a2 resets per epoch) |

### 7.1 Reaction-matrix rows ADDED by run 30

| observation | reading | action |
|---|---|---|
| all counters healthy (as above) but the decoded PCM is a constant ±3 LSB noise floor | the rig encoded **silence** — the app never produced audio (launcher-parked, muted, or the game never started) | **read the video and the 0x010a burst first**: AU sizes ~1 KB/frame + no game process = nothing to hear. Not an audio-path bug; do not "fix" kn_arx. Re-run with a draw that reaches gameplay |
| `audio_close` missing from stdout while `audio_arm file:1` is present | the teardown close never ran (the XLVIII `!pump_persist` gate — **fixed by F12/XLIX**) | pre-XLIX builds: the file has no EOS page and, under `--lottery`, the next draw reopens the same path with the previous `FILE*` leaked open (run 30's 2 505-byte scrambled page at the draw boundary). XLIX+: one `audio_close` per draw, one `audio_d<N>.opus` per draw |
| the delivered file is named `audio_d1_d1us` (suffix doubled, NO extension) while its size matches `audio_close.bytes` | the pre-L F13 derivation `memcpy`'d OVERLAPPING memory (`strrchr` into the destination buffer) — **fixed by F16/`kn_drawpath`** | the audio itself is intact: rename to `audio_d1.opus` and it decodes normally. Never re-inline the derivation — call `kn_drawpath_suffix_inplace()` |
| first 1–2 audio packets of a stream carry TOC `0x7F`/`0x70` (CELT-NB configs) instead of `0xEC` | the rig encoder's warm-up frames | benign; `kn_opus_packet_samples48k` handles every config — but a future decoder must not assume a constant config across the stream |

## 8. Decision ledger (F11)

- **D-o1** page flush at 255 segs / 16384 B / 48 pkts; **D-o2** no
  continuation pages; **D-o3** preskip 0 (bit-transparent extraction).
- **D-a1** emit in arrival order (no reorder buffer); **D-a2** SSRC
  change resets replay + seq accounting (per-epoch).
- Extraction is **mbedTLS-only**; schannel/stub builds export −1 and
  the session runs exactly as before (audio disarmed, honest zeros).
- Opus **decode** (PCM/WAV) and playback routing (`kn_audio`) are
  deferred follow-ons — the .opus file IS the deliverable for now.
- Audio RTCP (`bundle_rtcp_in`) is counted, not parsed — no RR/SR
  consumption for audio yet.

## 9. Run 30 — RESULT (round XLIX): extraction PROVEN, content SILENT

Full analysis: `docs/RTSP.md` §37. The short version for anyone touching
this stage:

**Everything the §7 "healthy" column asked for arrived, on both draws.**
`audio_arm srtp_derived:1 armed:1 profile:1 pt:111 red_pt:63 channels:2
clock_hz:48000 file:1` ×2 · `packets` 15 610 / **40 000** = `bundle_rtp_in`
(no RTP datagram was ever dropped by the audio path) · `srtp_fail:0`,
`srtp_nokey:0`, `replay_reject:0`, `malformed:0`, `toc_invalid:0`,
`pt_filtered:0`, `padded:0` · `red_packets` = `packets`, `red_blocks` =
2 × `red_packets`, `red_invalid:0` (the rig sends **every** audio packet
as RFC 2198 RED with one redundancy block — D-a1's arrival-order emit is
therefore loss-protected by the server itself) · `seq_loss/reorder/dup`
0/0/0, `ssrc_changes:0` · `last_ms − first_ms` = 78.029 s / **199.952 s**
= `packets × 5 ms` exactly (200 pkt/s — corrected in §10.1; the XLIX
"× 20 ms" sentence was wrong by the factor 4) · `ogg.dropped_packets:0`,
`ogg.write_errors:0` · the delivered file is a valid OggOpus stream
(ffprobe: Opus 48 kHz stereo 199.92 s, 83 kb/s; 834 pages, `seq`
contiguous, `granulepos` monotone, `last_granule` = Σ TOC samples).
**The DTLS-SRTP exporter's SERVER half (`km+16` key / `km+46` salt) is
proven against live traffic** — 40 000/40 000 HMAC-SHA1-80
authentications cannot pass with a wrong key half.

**But the audio is digital silence**: decoded 199.44 s / 19 146 240
samples → mean −91.0 dB, max −80.8 dB, per-10 s RMS 0.51 LSB everywhere,
every sample |x| < 8. The packets are structurally perfect CELT-FB **5 ms**
stereo single-frame packets (TOC `0xEC` = config 29, `s=1`, `c=0`, on
39 929 of 39 936 readable audio packets — NOT 20 ms; see §10.1), i.e. the rig
**encoded silence**: no game ever ran in either draw (the 0x010a burst
carries only `upc.exe` / `UplayService.exe` / `UplayWebCore.exe`; the AU
sizes are 0.8–1.2 KB/frame = a static launcher window for 75 s / 200 s).
The operator's browser control measured the boot: **≈2 min 19 s from
stream start to gameplay audio** (queue→stream 2:26, launcher→game 1:08,
game→first notification 1:11, sound from game start). Our draws died at
75 s and 197 s of media.

**Consequence for the stage plan:** F11's extraction contract is CLOSED
(sandbox KAT + live zero-error proof). What is *not* proven yet is a
**content-bearing** capture, and that is not an audio-stage problem — it
needs a draw that (a) outlives ≈150 s and (b) reaches the game. Opus
**decode** (PCM/WAV) + `kn_audio` playback stay the follow-on; they are
now unblocked (the .opus files are valid input), but there is no point
spending a round on them until one capture actually contains sound.

### 9.1 What XLIX shipped on top of run 30 (F12–F15) and what run 31
measures

- **F12 — the close was dead code.** `sess_audio_file_close` was gated on
  `!pump_persist`, and `pump_persist` is 1 in every live session, so
  `audio_close` NEVER fired: no EOS page, no `fclose`, and under
  `--lottery` the next draw reopened the same path with the previous
  `FILE*` leaked open (run 30's 2 505-byte scrambled page at the draw
  boundary, offset [815 462, 817 967)). XLIX closes the file **after
  `sess_pump_stop` on both exit paths** plus a defensive close at
  `teardown:`. Expect exactly one `audio_close` per armed draw.
- **F13 — one file per draw.** `--session-audio-out=audio.opus` with
  `--lottery=N>1` now writes `audio_d1.opus`, `audio_d2.opus`, … (single
  draw, or `N=1`: the literal path, unchanged). This removes the
  cross-draw interleaving hazard entirely instead of relying on the
  close ordering.
- **F14 — the interleaved A/B instrument.** With `--lottery>1` and
  `--session-jiggle` set, ODD draws run with the jiggle ARMED and EVEN
  draws with it DISARMED, and each draw emits a `lottery_arm` line
  (`draw:N jiggle:1|0`). One run therefore answers two questions at
  once: does input keep the session alive past ≈600 s (the idle-policy
  hypothesis that replaced the withdrawn ceiling), and does a surviving
  draw reach gameplay audio.
- **F15 — `--lottery-stop-on-keeper=0|1`** (default 1 = the historical
  behaviour: stop the lottery on the first keeper). Run 31 needs **0**,
  otherwise an early keeper ends the A/B before the OFF arm is measured.
  The pre-XLIX stop rule also had a logically dead first draft (an
  `lrc==0` return swallowed by the surrounding `lrc != 1` break) — do not
  re-introduce the flat form.

Run-31 expectation for this stage: `audio_d<N>.opus` × up to 4, each with
its own `audio_arm` + `audio_close`, `srtp_fail/malformed/toc_invalid/
seq_loss` all 0, and — the actual prize — a **non-silent** decode
(per-10 s RMS well above ~1 LSB) in whichever draw reaches gameplay.
Read the arms against each other: if the jiggle-ON draws outlive ≈600 s
and the OFF draws do not, the idle-policy hypothesis is confirmed.

## 10. Run 31 — RESULT (round L): F12 PROVEN, F13 CAUGHT BROKEN BY ITS OWN FILES, CONTENT STILL SILENT

Full analysis: `docs/RTSP.md` §38; the round delta: `docs/INTEGRATION.md`
rev 41. The short version for this stage:

**The four draws (`--lottery=4 --session-audio-out=audio.opus`, Watch Dogs:
Legion, all `recovery_exhausted`):**

| draw | media (`au_clock.rtp_ms`) | `audio_close` pages | bytes (== the delivered file size) | packets | `red_packets` | `srtp_fail` | `seq_loss` | `write_errors` |
|---|---|---|---|---|---|---|---|---|
| 1 | 16.19 s | 82 | 199 510 | 3 814 | 3 814 | 0 | 0 | 0 |
| 2 | 13.96 s | 73 | 176 601 | 3 376 | 3 376 | 0 | 0 | 0 |
| 3 | 35.96 s | 164 | 406 292 | 7 771 | 7 771 | 0 | **1** | 0 |
| 4 | 122.23 s | 525 | 1 309 966 | 25 062 | 25 062 | 0 | 0 | 0 |

- **F12 is PROVEN LIVE**: exactly one `audio_close` per armed draw (4 of
  4), each with its EOS page, each `bytes=` matching the file actually on
  disk. Run 30's failure mode did not repeat — no leaked `FILE*` across a
  lottery gap, no scrambled page at a draw boundary.
- **F13 delivered per-draw files — with a mangled NAME**: `audio_d1_d1us`,
  `audio_d2_d2us`, `audio_d3_d3us`, `audio_d4_d4us` (no `.opus` at all).
  Cause and fix in §6 above (F16/`kn_drawpath`). The payloads were never
  affected: size == `audio_close.bytes` on all four, `write_errors` 0.
- **`seq_loss` 1 on draw 3 is the first non-zero loss this stage has ever
  seen** — and it is benign by construction: every packet arrives as RFC
  2198 RED with one redundancy block (`red_packets == packets` ×4), so the
  previous packet's payload covered the gap. No page was dropped
  (`ogg.dropped_packets` 0, `write_errors` 0). D-a1's arrival-order emit
  is doing exactly what it was designed to do.
- **Content: STILL DIGITAL SILENCE, and round L finally explains why.**
  All 64 `sctl_010a` notifications name only Ubisoft Connect
  (`UplayWebCore.exe` ×26, `UplayService.exe` ×16, `upc.exe` ×4) — **zero
  game processes on any draw**. The rig sat at the Ubisoft login prompt:
  the GFN browser client auto-logs-in by **typing the credentials into the
  stream**, and our client sent no keystrokes (the activation chain flew
  perfectly — `sent:8 failed:0 version:3 ready:1` ×4 — and not one
  discrete input message followed it). So the silence is not an audio bug
  and not a launcher-park coincidence: **the audio stage cannot capture
  content until the client can type.** That is F19 (`--input-script`,
  `--input-live`, `--input-anti-afk`), shipped this round.
- **The ≈600 s idle-policy A/B: NO RESULT.** Media ran 14–122 s; nothing
  reached the region the A/B was designed to probe. The interleaved
  instrument (F14/F15) is proven and stays available for a run that draws
  survivors.

**What run 32 must show for this stage** (the login run —
`--input-script=login.txt --session-audio-out=audio.opus
--session-video-out=video.264 --session-release`):

1. the file is named **`audio_d1.opus`** (F16 proven on target — a
   `audio_d1_d1us`-style name means the target built a pre-L tree);
2. `audio_close` once, `srtp_fail`/`write_errors` 0;
3. **a non-silent decode** — per-10 s RMS well above the ~1 LSB floor —
   in whichever part of the draw reaches gameplay, which is the first
   content-bearing capture this project has ever had;
4. corroboration from the other stages: a GAME process in the `0x010a`
   burst (anything that is not `upc.exe`/`UplayService.exe`/
   `UplayWebCore.exe`), AU sizes well above the ~1 KB/frame static-screen
   signature, and `input_summary` with `sent > 0`.

If (3) is still silence while (4) shows a game process, THEN it is an
audio-stage question again (mixer/mute/PT) and the reaction matrix in
§7/§7.1 is the place to start. Until then, do not touch `kn_arx`.

### 10.1 Run 32 (round M read): silence again, the seat came back, and the 5 ms correction

`run32_logs/audio.opus` (Endfield launcher, 29 s draw): 333 274 B =
`audio_close.bytes` exactly (F16's name + F12's close, both proven on
target), 135 pages, 6374 packets, `red_packets == packets`,
`srtp_fail:0`, `write_errors:0`, and — the first real audio loss ever
delivered — `seq_loss:68` (1.07 %), invisible in the output because every
packet rides RFC 2198 RED with one redundancy block (D-a1's arrival-order
emit absorbing loss exactly as designed). Decoded: 31.87 s, **peak 1 LSB
in every 5 s bucket** — digital silence, because the Endfield LAUNCHER
makes no sound and nothing else ran. The first content-bearing capture
still waits behind a typed login (docs/RTSP.md §39).

**The frame-size correction, measured on all three audio runs:** the
delivered timeline is **240 samples = 5 ms per packet at ~200 pkt/s**
(run 30: 40 000 × 5 ms = 199.95 s = ffprobe's 199.92 s; × 20 ms would be
800 s). TOC `0xEC` = config 29 = CELT-FB 5 ms, `s=1`, `c=0` under RFC 6716
§3.1's `config(5)|s(1)|c(2)` layout — which is exactly what
`kn_opus_packet_samples48k` returns (its `code = toc & 3` reads the
2-bit frame-count field in the LOW bits, the stereo flag is bit 2), so
the muxer's granulepos was right all along and the XLIX "20 ms / CELT-FB
20 ms" label was the only thing that was wrong. Any future decoder or
playback work (§8's deferred follow-on) must budget **200 packets/s**,
not 50.

### 10.2 Run 33: the first content-bearing capture (13:38 of real game sound)

Run 33 turned §10.1's "proven as extraction" into proof of content:

- `audio.opus` — **163 728 packets × 5 ms = 818.68 s**, ffmpeg duration
  `00:13:38.64` (the 5 ms correction holds on the longest capture yet;
  at §10.1's 20 ms guess this would have "played" 4× too slow),
  23 575 615 B, `srtp_fail 0`, `seq_loss 0`, `write_errors 0`.
- The operator opened it in Audacity: **stereo, audible content** — the
  Endfield loading screen and in-world walking. This is the first time
  in the project that a capture is not just structurally valid but
  verifiably real sound, and it is also the first independent proof
  that the arx pipeline carries content at all (run 32's launcher
  capture was structurally clean but never content-checked).
- The freeze at 815.2 s cut BOTH media planes at once (video 24 424 AUs
  ≈ 29.94 fps and audio ≈ 163.7 k packets over 815.689 s of rtp time)
  with `seq_loss 0` to the very end — no loss precursor in either
  direction. Audio-plane evidence for RTSP.md §40.2: whatever kills
  these sessions, it is not the network.
- Still owed to close the audio bar with a second implementation: an
  independent RMS/spectrum decode of a pushable excerpt in the sandbox
  (run-33 addendum in START-HERE §4 lists the exact ffmpeg cut).

**Round M addendum — the independent decode CLOSED the second-implementation
bar.** `media/run33_tail.opus` (the pushed last-10 s excerpt) decoded in
the sandbox with ffmpeg + numpy, no operator ears involved:
483 840 samples (10.08 s), **mean RMS −60.5 dB, peak −27.3 dB** (run 30's
silence floor was mean −91.0 / peak −80.8 dB — this capture sits ~30 dB
above it), 82.6 % of 10 ms windows below −60 dB but p95 −58.5 dB =
quiet-but-real ambience, **L/R correlation 0.294** with side channel
−63.1 dB = genuinely decorrelated stereo (not a duplicated mono), and a
stationary dominant tone at **391 Hz** (G4; 578–582 Hz overtone late)
present in all ten per-second spectra = an in-world ambient drone, plus
the operator's own room tone ("dźwięki otoczenia"). Band split: 50.3 %
low-mid / 28.4 % mid / 17.3 % sub-bass / 3.9 % above 2 kHz — a muffled
environmental bed, exactly what a standing character hears. The audio
plane is hereby certified **content-bearing by two independent
implementations** (operator's Audacity + sandbox numpy decode). Its
freeze-side behaviour (cut ~11 s after video, no decay of its own) is
in RTSP.md §40.4.

### 10.3 Run 34: content to the last packet of a 900-second keeper

The pushed `media/audio_trimmed.opus` (true last 10 s — the Opus leg of
the operator's bat script carries real timestamps, unlike the Annex-B
video leg) decodes at **mean −58.1 dB / peak −28.8 dB, L/R correlation
0.380**, flat −57…−59 dB in every 2 s bucket: ambient game bed, healthy,
to the final packet of the first 900 s keeper. The video leg of the same
script used `-r 25` on a 30 fps stream, stretching the timeline 1.2×
(the "last 10 s" cut is really the last 250 frames ≈ 8.3 s); harmless
for the tail check (sharpness flat 8.7–8.8, zero dips — RTSP.md §41.1)
but the corrected script ships with the run-34 addendum in
START-HERE §13: **`-r 30`**, or `-f h264 -r 30 -i` form.
