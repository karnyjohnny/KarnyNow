# WASAPI synthetic-PCM render (`kn_audio`) — implementation notes

> **Round XLVIII naming note:** this module is PLAYBACK (render). The
> rig's audio RECEIVE path — SRTP extraction from the bundle socket
> into an OggOpus file (`kn_arx` + `kn_ogg`, F11) — is documented in
> **`docs/BUNDLE-AUDIO.md`**. The stage-2 TODO below ("Opus decode →
> the refill loop") now starts from a captured `.opus` file.

Status: **IMPLEMENTED, sandbox-verified** (neutral half host-tested:
`test_knaudio` 22 checks; cross `-Werror` x64/i686 clean; ctest 10/10;
knvstd clean under `-Wformat-truncation=2` at -O0/-O2/-O3; PE imports
gain only ole32.dll — OS-built-in). **The WASAPI engine has NEVER
EXECUTED anywhere** (no audio stack in the sandbox) — its first run is
T11 on the target. This completes the stage-1 deliverable list
(BOOTSTRAP §12: "Plus WASAPI with synthetic PCM"); full **G1** (video +
audio, no network) gates on the next T11 full run.

## Why this module exists

Stage 1 must prove the audio pipeline offline before stage 2 feeds it
with real Opus: open the default render endpoint, negotiate the device
mix format, refill an event-driven shared-mode buffer from a dedicated
thread for 10 minutes without starvation or leaks, and report honest
counters. The synthetic source (soft 440 Hz tone at ~-18 dBFS, or
silence with `tone_hz=0`) is audible proof of life during the gate run.

## Engine pattern (every spelling verified against mingw-w64 headers this
## round — the d3d9 lesson applied up front)

Documented WASAPI shared-mode event-driven render client:

```
CoInitializeEx(MULTITHREADED)
CoCreateInstance(CLSID_MMDeviceEnumerator) -> GetDefaultAudioEndpoint(eRender,
    eMultimedia) -> IMMDevice_Activate(IID_IAudioClient)
GetMixFormat -> kn_audio_fmt_parse (portable byte parser, see below)
CreateEventExW(EVENT_MODIFY_STATE|SYNCHRONIZE)
IAudioClient_Initialize(SHARED, EVENTCALLBACK, 100 ms, 0, mix, NULL)
GetBufferSize -> SetEventHandle -> GetService(IID_IAudioRenderClient)
IAudioClient_Start()            <-- BEFORE CreateThread (a thread waiting on
                                    the event before Start burns its first
                                    200 ms timeout and pollutes the counter)
CreateThread(refill loop):
    WaitForSingleObject(event, 200 ms)
      timeout  -> wait_timeouts++
      padding   = GetCurrentPadding; queue_ms = padding*1000/rate  (§7.6)
      avail     = buffer_frames - padding   (skip when padding == buffer)
      padding==0 && frames_written>0 -> starved++   (device ran dry = glitch)
      GetBuffer(avail) -> kn_audio_gen(...) -> ReleaseBuffer(avail, 0)
close: run=0 -> SetEvent -> join(3 s) -> Stop -> Release chain ->
       CloseHandle -> CoTaskMemFree(mix) -> CoUninitialize -> free
```

Landmines recorded (all hit and defused in the sandbox this round):

1. **`COBJMACROS`**: mingw's `audioclient.h`/`mmdeviceapi.h` gate the
   C-style `IAudioClient_*`/`IMMDevice_*` macros behind it — `d3d9.h`
   does NOT (header inconsistency; T4 never needed the define).
2. **`INITGUID` in exactly one TU** (kn_audio.c) instantiates
   CLSID_MMDeviceEnumerator + the IIDs locally → **no `-luuid`**, the only
   new import is ole32.dll.
3. Stats have a single writer (audio thread) and one reader (main):
   `volatile` counters, per-field snapshot copy; benign 32-bit tear on
   x86 accepted for counters (target is x64 where the reads are atomic).
4. The mix-format subformat GUIDs are matched as the hand-written
   KSDATAFORMAT tail `{0000-0010-8000-00aa00389b71}` + Data1 (1 = PCM,
   3 = IEEE_FLOAT — ksmedia.h:620-628), not via ksmedia symbols.

## The neutral half (host-tested)

- `kn_audio_fmt_parse`: portable byte-layout parser for WAVEFORMATEX /
  WAVEFORMATEXTENSIBLE (tag/channels/rate/bits/cbSize/valid-bits/SubFormat;
  structural rejection of rate 0, channels >8, non-byte-aligned bits).
  v1 renders **float32 / s16 / s32** — anything else (24-bit PCM, ADPCM…)
  reports UNKNOWN and `kn_audio_open` fails with the tag in the reason
  (honest refusal, not a crash). Real mix formats are float32 on virtually
  every Windows setup.
- `kn_audio_gen`: sine/silence generator, phase passed by reference so
  successive calls are byte-identical to one big call (test-proven),
  amplitude ~-18 dBFS.
- `test_knaudio` (22 checks): hand-built format blobs (the typical
  48 kHz/2ch/float32 extensible, plain PCM16, extensible s32, corrupted
  subformat tail, every malformed-input path), generator contracts
  (silence = digital zeros, range bounds, phase continuity, exact
  2πf/rate advance, unknown-format no-op), exact `describe` string.
- Gate story worth keeping: the s16 range check initially iterated past
  the written region into uninitialized stack — passed at -O1, failed at
  -O3+`NDEBUG` under ctest. Fixed with an exact-sized, poison-filled
  buffer. **Different -O levels catch different lies** (R28 sibling
  lesson, on the TEST side this time).

## Wiring into `--loopback`

- Audio opens right after the presenter (first decoded frame). **Open
  failure never kills the video loop** — it fires
  `kn_log_on_failure("session.loopback.audio", …)` (§7.7: amplify + seal +
  auto-bundle) and the run continues; the summary reports
  `"audio":{"opened":false,"reason":…}` and T11 gates full G1 on it.
- `--no-audio` disables (debug only; summary says `"reason":"disabled"`).
- The §7.6 `queue_audio_ms` field is now REAL (device padding in ms at the
  last refill); T11 requires it alive (>0) in ≥80 % of perf lines.
- Summary audio block: format string, events, frames_written vs
  expected_frames (rate × seconds_run), starved, wait_timeouts,
  buffer_errors, queue_ms_last.

## Stage-2 TODOs growing from here

- Opus decode (PT 111, 48 kHz — OpenNOW `docs/streamer-comparison/audio.md`
  facts already indexed in BOOTSTRAP §5.1) → `swresample` (already linked)
  into the mix format → the same refill loop with a jitter buffer
  (45 ms target, 100 ms concealment cap — the audio.md numbers).
- `queue_audio_ms` then reports the jitter-buffer depth, not device
  padding; `starved` becomes the concealment trigger.
- Volume/mute via ISimpleAudioVolume when the TUI grows transport keys.

## F21 (round N slice 3): the module grows its real source — live playback of the arx Opus pipe

Status: **IMPLEMENTED, sandbox-gated; the WASAPI engine still has NEVER
EXECUTED anywhere** — the first live playback is run 36 on the target
(`--session-audio-play`). Everything host-testable is host-tested:
`test_knaudio` **54 checks** (feed ring + xform + the pre-F21 battery)
under ASan+UBSan, `test_knopus` **105 checks** (85 KAT vectors, bit-exact
vs libopus 1.5.2: max |diff| **0 LSB**), cross `-Werror` ×2 arches,
ctest **27/27** on both TLS backends, T10 dry **ALL PASS 79** with the
vendored opus staged (the real-vector leg).

### What ships

1. **`kn_opus.{h,c}` — the project's ONLY Opus knowledge** (decode-only,
   four functions: `kn_opus_available` / `kn_opus_version` /
   `kn_opus_decoder_create(rate, channels, err, errsz)` /
   `kn_opus_decode_f32(dec, pkt, len, out, cap_frames)` → frames | −1,
   plus `kn_opus_decoder_destroy`). Three build modes, in order:
   system libopus (pkg-config), the pinned vendored tree
   `third_party/opus` (T10 stages the 1.5.2 release tarball, SHA-256
   `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`),
   or the honest stub (`KN_HAVE_OPUS=0`: available 0, version `"none"`,
   create NULL + `"libopus not linked in this build"`). The wrapper
   accepts BOTH include layouts (`<opus/opus.h>` and `<opus.h>`) via
   `__has_include`. Capacity is checked BEFORE decode
   (`rate/1000*120` frames); `len==0`/NULL is −1; **no PLC by design** —
   a lost packet is the arx `seq_loss` counter's business, and the ring
   below already turns absence into counted, honest silence.
2. **Feed mode in `kn_audio`** — `kn_audio_open_feed()` (default render
   endpoint, shared mode, event-driven, plus a **2 s ring at the device
   rate**: absorbs decode/pump jitter without audible lip-drift) and
   `kn_audio_push_f32()` (the producer side). The engine thread's refill
   branch becomes: pop from the ring → zero-fill the untaken tail (the
   zero-fill IS the underrun) → `kn_audio_xform_f32` into the device
   buffer. **`starved` counts only `pop==0 && pushed>0`** — pre-roll
   silence while the stream arms is not a gap. POSIX: `open_feed`
   refuses honestly (`"WASAPI is Win32-only"`), `push_f32` returns 0.
3. **The feed ring itself** (`kn_audio_feed_*`, neutral, host-tested):
   SPSC, power-of-two capacity rounded DOWN from the backing size,
   **cap−1 usable frames** so `wr==rd` stays unambiguously EMPTY;
   monotonic `wr`/`rd` masked on access; overflow **drops the tail and
   counts it** (`dropped`) — for live audio the newest samples win and
   back-pressure is forbidden; lifetime `pushed`/`popped` counters.
4. **`kn_audio_xform_f32`** — float32 → the negotiated device format:
   F32 copy+clamp; S16/S32 `lrintf`/`lrint` scale+clamp (S32 computes in
   **double**: the float constant `2147483647.0f` rounds UP to 2^31 and
   overflowed the int cast at v==1.0 — caught by the new checks on the
   first ASan pass); NaN → silence, never propagated; UNKNOWN format
   writes one block-aligned container of silence per frame.
5. **`knvstd` wiring** (`--session-audio-play`, default OFF, Win32
   only): open on the MAIN thread before the pump starts (COM/WASAPI on
   a known thread, one device per draw); `sess_audio_emit` keeps the
   **file first** (the F13 `.opus` contract is untouched — playback is
   best-effort and never back-pressures the SRTP commit), then
   decode → `swr_convert` ONLY when the device rate ≠ 48 kHz (FFmpeg
   gated) → `push_f32`. Open failure is NON-FATAL: the session stays
   file-only and says so (`audio_play_begin ok:0 reason:"…"`). Close
   runs after `sess_pump_stop()` joined the worker (F12's
   join-before-read rule) at all three session-exit sites.

### Evidence lines (stdout + the media channel log)

```
{"mode":"session","event":"audio_play_begin","ok":1,"fmt":"48000 Hz 2ch f32",
 "rate":48000,"channels":2,"swr":0,"opus":"1.5.2"}          (or ok:0 + reason)
{"mode":"session","event":"audio_play_end","pkts":…,"frames":…,
 "decode_fail":…,"pushed":…,"dropped":…,"starved":…,
 "events":…,"written":…,"queue_ms":…}
```

No summary keys are added (the skeleton stays **113**): playback lives
on the event lines, the `"audio"` block stays the file's evidence.

### KAT vectors (`streamer/src/kn_opus_kat_vectors.h`, GENERATED)

85 vectors from `tools/opus_reference.py` (which encodes AND decodes
through an INDEPENDENT path — libopus via Python ctypes — before
emitting, and refuses to emit unless its self-proofs pass:
determinism, warm-replay repeatability, RMS bands, garbage rejection).
One sequential 48 kHz stereo stream at the fixed 96 kb/s / complexity-10
operating point: 40 warmup packets, then per content vector 10
transition packets + the pinned packet, then one garbage TOC (0xFF)
vector that MUST be rejected with −1 on a cold decoder. Four content
vectors carry full s16 expectations (two-tone, alternate two-tone, a
quiet-noise-floor band); the rest pin frame counts only. Tolerance for
content: |Δ| ≤ 1 LSB; measured across the battery: **0**.

### Relation to the old Stage-2 TODO list

The TODO above ("Opus decode → swresample → refill loop with a jitter
buffer, 45 ms target / 100 ms concealment cap") is SUPERSEDED by F21
with two deliberate deviations, both recorded here: the absorber is a
**2 s ring** (not a 45 ms jitter buffer — the GFN audio plane arrives
over SRTP with its own sequencing; our ring smooths the decode/pump
handoff, not the network), and concealment is **counted silence** (no
PLC — `starved`/`dropped`/`decode_fail` tell the truth instead).
`queue_audio_ms` still reports device padding at the last refill.
Volume/mute transport keys remain a future TUI item.
