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
granulepos.

## 6. knvstd wiring (`knvstd_main.c`, stamp XLVIII)

- **CLI**: `--session-audio-out=F.opus` (help text updated; no file →
  counters still run, no capture). Threaded through `run_session(…,
  audio_out, log_dir)`; both call sites (single + lottery).
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
- **Close** (teardown, after `sess_ctrl_ping_stop`, skipped while
  `pump_persist` — closing under a live writer would race):
  `kn_ogg_mux_finish` (EOS) + `fclose` + `audio_close` event
  (pages/bytes/packets/payload_bytes/red_packets/srtp_fail/seq_loss/
  write_errors) + knlog MEDIA line.
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

Command (NO `--session-au-dir` — the ~600 s ceiling makes AU dumps
pointless; §36):

```
knvstd.exe --session --session-token=<TOKEN> --session-app=<APP> \
  --session-seconds=570 --lottery=2 --lottery-gap=30 \
  --session-audio-out=audio.opus > logs\run30.log
```

Healthy draw: `audio_arm` with `srtp_derived:1 armed:1 file:1`;
summary `audio.packets` ≈ **9.5k–30k** (50 pkt/s × ~570 s minimum for
20 ms Opus; RED doubles the payload count while halving packet count —
anything in the band with `red_packets+packets` ≈ const is fine);
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
