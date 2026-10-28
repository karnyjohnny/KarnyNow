# INPUT.md — the NVST control/input plane (sid 0)

Status: **round XXXVII shipped the activation half** (the eight-message
input handshake + the cursor-capture handoff machine + full inbound-record
evidence). The discrete-input half (keyboard/mouse/gamepad/HID/text) is
stage 3 and is **not** built yet — nothing in this round sends user input.

Provenance: every byte and every interval below is a port of a named line
in the reference client (OpenNOW @ `a49b124`,
`native/opennow-streamer/crates/opennow-streamer-transport/src/`). Line
numbers point at that commit.

---

## 1. Why this layer exists — the run-17 elimination

Round XXXVI shipped the complete receiver-feedback set on the hypothesis
documented verbatim in the reference: *"without any receiver feedback the
server stops video after an initial burst"* (nvst.rs:4320-4323).

**R7 run 17 disproved that hypothesis for our stall.** The feedback set
flew for the whole session and the video died at the same mark anyway:

| | run 16 (no feedback) | run 17 (full feedback) |
|---|---|---|
| wall time | ~10.2 s of video, then session end | 242.2 s session (`wall_ms` 242173) |
| video span | ~10.2 s | **10.99 s** (ts 867936 → 1856990, 989054 ticks @ 90 kHz) |
| AUs / frames | 307 / 307 | 331 / 331 @ 30.12 fps |
| feedback actually sent | none | `rr 242`, `qos 4701`, `ka 81`, `ack 331`, `pacing 167`, `dropped 0` |
| coverage | — | RR/QoS/keepalive flew the **whole 242 s**; pacing covered 9.28 s (frame-gated, exactly like the reference's) |
| post-stall recovery | `kf_request sent:false` ("stage 3") | `kf_request sent:1`, `idr 1`, `pli 1` — **the rig never answered** |

Two independent readings of the same table:

1. RR at 242 = one per second for 242 s, QoS at 4701 ≈ 242 s / 51.5 ms,
   keepalive at 81 ≈ 242 s / 3 s, ack at 331 = one per AU, `dropped 0`.
   The senders are correct and were never starved.
2. The stall moved from 10.2 s to 10.99 s — **within noise**. Adding the
   entire feedback plane bought ~0.8 s.

So the cause is something **both** runs lacked. Re-reading the reference
for what it sends on the control plane at session start, exactly one thing
was missing: the **input activation chain**.

The reference's own comment on its window-state member
(nvst_input.rs:1376-1381) states the consequence:

> *"The desktop client announces window state 19 at frame zero once input
> is activated. Advertising the previous all-zero window state **leaves
> the session looking inactive** and prevents the server from sending its
> system-cursor mode updates."*

A session that looks inactive is precisely what a streaming server stops
feeding after a short burst. This is now the prime hypothesis for the
~11 s stall, and run 18 tests it.

## 2. The complete control-plane inventory (what the reference sends)

Built by sweeping every `send_control` / `send_partial_control` /
`send_rtcp` / `send_keepalive` / `send_activation` call site in nvst.rs
for a **device-less** session (no HID runtime, no gamepad, no microphone):

| sid | message | trigger / cadence | reference | ours before XXXVII | ours now |
|---|---|---|---|---|---|
| 0 | keepalive `0x0200`, stream_value 0 | every 3 s while control open | :7750-7755, nvst_input.rs:1346 | ✅ round XXXVI | ✅ |
| 0 | **activation chain (8 messages)** | once, control open + INPUT_VERSION | :6554-6591, nvst_input.rs:1318 | ❌ **missing** | ✅ **shipped** |
| 0 | cursor capture `0x0308` retries | 250 ms × ≤ 8 while waiting | nvst_cursor.rs:88-99 | ❌ missing | ✅ shipped |
| 0 | cursor tracking `0x030d` | with each capture retry | nvst_cursor.rs:96 | ❌ missing | ✅ shipped |
| 0 | cursor capture `0x0308` **false** | on the rig's cursor notification, or after 3 s of silence | nvst_cursor.rs:64-72, :100-124 | ❌ missing | ✅ shipped |
| 0 | IDR request `0x0302` | on keyframe request | :8107 | ✅ round XXXVI | ✅ |
| 0 | frame invalidation `0x0301` | on unrecoverable loss | :3104 | ✅ round XXXVI | ✅ |
| 6 | frame ack `0x0204` (batched) | per completed frame | :7936-7946 | ✅ round XXXVI (unbatched) | ✅ |
| 6 | frame pacing `0x0203` | 55 556 µs, frame-gated | :7920-7935 | ✅ round XXXVI | ✅ |
| 6 | QoS report `0x0207` | every 50 ms | :7948-7970 | ✅ round XXXVI; **all seven BWE fields live round XXXVIII (D-t10 CLOSED)** | ✅ |
| 6 | NACK-v2 `0x0317` | from the tracker | :3075-3078 | ✅ round XXXVI | ✅ |
| 14 | RTCP RR / NACK / PLI | 1 s / on loss | :8041-8103 | ✅ round XXXVI | ✅ |
| 0 | HID: sony/generic reports, gamepad descriptors, text | only with devices attached | :7767-7800, :8290-8310 | ❌ n/a | ❌ stage 3 |

The activation chain is the only **startup** item that was missing. Note
also that the reference *batches* frame-ack/pacing/QoS records into one
SCTP message (`ControlReportBatch`, nvst.rs:462-548); the wire format is a
concatenated TLV list either way, so our one-record-per-message form
parses identically. Batching is a throughput nicety, not a fidelity gap —
recorded as **D-t11** (see §7) — **CLOSED round XXXVIII**: the
`ControlReportBatch` now ships (MAX 1071, one flush = one sid-6 message,
due on QoS-or-50-ms-age, retain-on-failure).

## 3. The activation chain, byte for byte

`activation_chain(timestamp_us)` — nvst_input.rs:1318-1329. Order matters:
input is disabled before the descriptor lands and re-enabled after the
client state is announced.

| # | code | bytes | payload semantics |
|---|---|---|---|
| 1 | `0x020b` enable-input | 16 | `{stream 0, counter 1, enabled 0}` 3× LE u32 |
| 2 | `0x020d` gamepad descriptor | 52 | `0x23`, `timestamp_us` **BE u64**, 39-byte body template with `body[9..11]` = bitmap LE |
| 3 | `0x0308` cursor capture | 5 | `{1}` |
| 4 | `0x030d` cursor tracking | 5 | `{1}` |
| 5 | `0x0320` window state | 16 | `{stream 0, state 19, frame 0}` |
| 6 | `0x0321` system state | 16 | `{stream 0, state 0, frame 0}` |
| 7 | `0x020b` enable-input | 16 | `{stream 0, counter 1, enabled 1}` |
| 8 | `0x0206` remote-input | 44 | nested: `remote_input_packet(0x0e, pad24(remote_input_packet(13, u16 LE 1)) ‖ timestamp_us LE u64)` |

Total 170 bytes over 8 messages on sid 0, ordered + reliable
(nvst_input.rs:72-78).

Two endianness traps, both pinned by test:
* the descriptor timestamp is **BE** (`to_be_bytes`, :1360);
* the haptics timestamp is **LE** (`to_le_bytes`, :973).

`timestamp_us` is `transport_origin.elapsed().as_micros()`
(nvst.rs:6564-6567) — microseconds since the transport started, **not**
wall clock. We keep a `pump.origin_us` for exactly this.

### The reference publishes its own goldens

nvst_input.rs:1768-1800 (`control_keepalive_and_activation_are_byte_exact`)
asserts the exact hex of all eight messages at `timestamp_us = 20 102 193`
plus the keepalive and both cursor commands. `tools/input_reference.py`
transcribes those goldens verbatim and **self-proves against them before
emitting** `streamer/src/kn_input_kat_vectors.h` (the vectors-are-trusted
rule). `test_kninput` then checks the C port against that header — so the
chain is pinned by three independent implementations (Rust reference →
Python model → C), not by our reading of the Rust.

## 4. The handshake that gates it

`NvstInputChannelState` (nvst_input.rs:319-397):

```
is_ready() = control_open && negotiated_version.is_some()
```

* `control_open` — the sid-0 channel exists. Ours: association up **and**
  every DCEP OPEN we sent was ACKed (run 17: `opened 8 acked 8`).
* `negotiated_version` — parsed from an inbound sid-0 message
  (`channel_data`, :339-351). Run 17's rig sent it as its very first
  message: `0e 02 02 00 03 00` → **version 3**.
* `handshake_timed_out` — 5 s (`INPUT_HANDSHAKE_TIMEOUT`, :12) without
  ready. Not enforced yet; recorded as a gap (§7).
* On ready: `finish_nvst_input_handshake` sends the chain (retrying the
  WHOLE chain if any message fails to queue, :6569-6574), latches
  `activation_sent`, arms the cursor machine (:7519-7522) and stores
  `input_ready = true` (:6589).

`input_protocol_version` (nvst_input.rs:404-441) has **two fallbacks** that
a naive port gets wrong, both pinned by test:
1. the first record is `0x020e` but its declared length overruns the
   buffer → read `data[2..4]` verbatim (or `2` when the buffer is 3 bytes);
2. otherwise, if `data[0] == 0x0e` → return the first LE u16 as-is
   (so `0e 03` yields `0x030e`, not a version).

Consequence worth recording: `0e 02 ff 00 03` returns **255**, not the
command code. A port that "helpfully" returned `0x020e` would silently
negotiate a bogus version.

## 5. The cursor-capture handoff machine

nvst_cursor.rs in full — a small bounded state machine, ported as
`kn_input_cursor`:

```
Inactive --activate(now)--> Waiting{deadline=now+3000, retry_at=now+250, attempts=1}
Waiting  --notify(now)----> Disabling{retry_at=now, attempts=0, "cursor-notification"}
Waiting  --now>=deadline--> Disabling{retry_at=now, attempts=0, "silent-host-timeout"}
Waiting  --now>=retry_at && attempts<8--> send capture(true)+tracking, attempts++, retry_at=now+250
Disabling--now>=retry_at--> attempts++, send capture(false);
                            sent  -> Local (handoff, update() returns 1)
                            !sent -> attempts==8 ? Exhausted : retry_at=now+250
```

Constants: `RETRY_INTERVAL` 250 ms, `MAX_ATTEMPTS` 8, `SILENT_HOST_TIMEOUT`
3 s (nvst_cursor.rs:3-5).

The reference's five tests (:165-297) pin the exact command sequences;
`test_kninput` replays all five as C:

* **notification handoff** — exactly one `capture(false)`, then silence;
* **silent host** — `(MAX_ATTEMPTS-1) × [capture(true), tracking]` = 14
  commands, then the 3 s deadline hands off **in the same `update` call**
  (the deadline check runs before the match, :82-89);
* **paced disable** — a failed disable does not retry before `retry_at`,
  and handoff is reported **only** on success;
* **exhaustion** — 8 failed disables → `Exhausted`, never reports local
  composition; `activate()` straight out of `Exhausted` re-arms;
* **failed enables never move the deadline** — a failing `send` must not
  extend the silent-host fallback.

Why it matters beyond cosmetics: the comment at nvst_input.rs:32-36 says
the server composites its cursor into the video while capture is on, and
the client turns capture **off** only after the rig's first cursor
notification — i.e. the notification is proof the rig's ServerControl path
is alive. If the rig never notifies, we fall back after 3 s. Run 18's
`cursor` field therefore doubles as a liveness probe for the rig's control
responses.

## 6. Evidence added this round

Run 17 left the rig's **20 control messages uninterpreted** — we could
name only the first. That gap is closed:

* `ctrl_msg` — the first **8** inbound non-DCEP messages verbatim:
  `{n, sid, ppi, len, records, codes, hex}` where `codes` is
  `"020e:input-version"`-style (named via `kn_input_command_name`, hex for
  unknown codes) and `hex` is up to 48 bytes.
* `input_version` — `{version, sid, len, records}` the moment `0x020e`
  parses.
* `input_activation` — `{sent, failed, bytes, ts_us, version, attempt,
  ready}` for the chain.
* `cursor_handoff` — `{reason, enables, disables}` when the machine
  reaches `Local`.
* `ctrl_in` — every 30 s: `{t_ms, msgs, records, version, ready,
  activation, cursor, codes}` where `codes` is the full histogram
  (`[["020e",1],["0200",5]]`, first-sighting order, overflow counted).
* `ctrl_summary` — the same state at session end, from a **fresh** pump
  snapshot (the pump thread still owns it at summary time).

The histogram is bounded (32 slots) and its JSON serializer **refuses** to
emit a truncated buffer (`kn_input_hist_json` returns −1) rather than write
malformed JSONL — the round-XVIII discipline.

## 7. Deviations and known gaps

| id | deviation / gap | status |
|---|---|---|
| **D-t11** | ~~frame-ack/pacing/QoS are sent one TLV record per SCTP message~~ — **CLOSED round XXXVIII**: `sess_cb_*` implements the reference `ControlReportBatch` (nvst.rs:455-560): MAX 1071 = MAX_CONTROL_REPORT_BYTES, `queued_at` on first record, flush_due = QoS present OR age ≥ 50 ms, one flush = ONE sid-6 push, failure retains the buffer. Run 18's `dropped:890` (per-record cadence saturating the 64-slot outbox) was the trigger. | closed |
| **D-t12** | the activation chain is retried at most **8 times** on partial send failure; the reference retries the whole chain indefinitely (nvst.rs:6569-6574). A dead association is already reported via `sctp_fail`, so an unbounded spin adds nothing. | accepted, bounded on purpose |
| G1 | `INPUT_HANDSHAKE_TIMEOUT` (5 s) is defined but not enforced — we never give up waiting for `0x020e`. | open; only matters if a rig withholds the version |
| G2 | `device_descriptor` is sent with bitmap 0 (no gamepad attached) — faithful for a device-less session, but a real pad needs `set_rich_slot_mask` + the sony/generic HID paths (nvst.rs:7767-7800). | stage 3 |
| G3 | discrete input (keyboard/mouse/wheel/text), the `0x0206` remote-input envelope for real events, and the `input_partial`/`control_partial` routing split (nvst_input.rs:241-247). **Round XLI.1 first slice**: `INPUT_MOUSE_RELATIVE(7)` is implemented (`kn_input_mouse_relative`, byte-exact against the reference's OWN Rust golden, nvst_input.rs:1888-1911) and routed to `control_partial` (sid 6, unordered PR-SCTP TTL 300 ms) per nvst_input.rs:969; the media loop sends a synthetic 10 Hz ±1 px jiggle (§9). Keyboard/wheel/text and the `input_partial` (gamepad) arm remain open. | partial (stage 3) |
| G4 | server cursor payloads are detected (for `notify`) but not decoded into shapes/positions (`server_cursor_messages`, nvst_input.rs:461-529). No local cursor rendering yet. | stage 3+ (presentation) |
| G5 | ~~the QoS estimator fields remain honest zeros (**D-t10**)~~ — **CLOSED round XXXVIII**: run 18 promoted it back to prime suspect (the stall moved 11→30.5 s once activation flew, and the AU frame-size decay under zero loss named the server rate control reading our zero BWE fields). `kn_bw.{h,c}` is the line-for-line `nvst_bandwidth.rs` port (two-implementation pinned, test_knbw 387); the feed rides `process_authenticated` (datagram=1/plaintext=0); `qos_report_at` fills all seven fields with the reference semantics. | closed; run 19 verifies live |

## 8. Files

| file | role |
|---|---|
| `streamer/include/kn/kn_input.h` | the API + every constant, each with its reference line |
| `streamer/src/kn_input.c` | builders, TLV walker, version parser, histogram, cursor machine |
| `streamer/src/kn_input_kat_vectors.h` | **generated** — do not edit |
| `tools/input_reference.py` | the independent Python model; self-proves, then emits the header |
| `streamer/tests/test_kninput.c` | 548 checks: goldens (incl. the round-XLI.1 relative-mouse Rust golden), parser vectors, the 5 cursor tests, truncation sweep |
| `streamer/src/knvstd_main.c` | `sess_ctrl_*` (pump-thread service) + the evidence lines + the round-XLI.1 jiggle cadence (§9) |
| `streamer/src/kn_selftest.c` | the **`input.vectors`** self-test (50 checks, phase A, gated) — the shipped binary re-proves the same goldens; registry 14 → 15 |
| `streamer/tests/test_knselftest.c` | 117 checks — registry/run-all counts and the portable-must-be-OK set updated for `input.vectors` |

Regenerate the vectors with `python3 tools/input_reference.py`; the output
must be byte-identical.

Two ways to prove the builders on any machine:

```sh
./build/test_kninput                          # 548 checks (the full battery)
./build/knvstd --selftest=input.vectors       #  50 checks from the SHIPPED binary
```

The second is the one that matters on the target: `kn_input.c` has no
backend split, so `input.vectors` executes identically on Win7 and in the
sandbox — a failure there means the binary the user will actually run
disagrees with the reference's goldens.

## 9. Round XLI.1 — the relative mouse and the synthetic jiggle

### Why this exists (the run-22 turning point)

Run 22 (round XLI's live run) froze on EVERY plane ~30-45 s in with no
termination and no idle warning, and the operator's answers afterwards
removed the last alternative: the web client was never attached during the
run (ours was the SOLE receiver) and **zero input messages ever reached
the session** — the VM saw a client that watched but never touched. What
remains, and what now fits ALL the data, is a server-side idle /
no-input policy: survival has grown monotonically with client fidelity —
runs 16/17 (no/incomplete feedback, no input) died at ~10-11 s, run 18
(+activation chain) at ~30.5 s, run 22 (+BWE + the reference's control
cadence) at ~45 s. The one fidelity item still missing against the
official client is real input. START-HERE §4.4's reconsideration
condition ("ship input when the evidence cannot separate idle-policy from
rig flake") is met: without shipped input, run 23 could not decide
between the two.

### The wire format (two implementations, one Rust golden)

`INPUT_MOUSE_RELATIVE` (native type 7) through the `0x0206` envelope —
nvst_input.rs:727-741 (the encode arm), :956-987 (`remote_input_message`),
:989-995 (`remote_input_packet`):

| bytes | field |
|---|---|
| `[0..2]` | command `0x0206` LE u16 |
| `[2..4]` | payload length LE u16 (40) |
| `[4..8]` | `remote_input_packet` length BE u32 (4 + 36) |
| `[8..12]` | packet type LE u32 (`0x0e`) |
| `[12..16]` | inner length BE u32 (4 + 10) |
| `[16..20]` | native type LE u32 (7 = MOUSE_RELATIVE) |
| `[20..22]` | **dx BE i16** |
| `[22..24]` | **dy BE i16** |
| `[24..26]` | two zero bytes (native[8..10]) |
| `[26..36]` | zero padding to the 24-byte floor |
| `[36..44]` | timestamp LE u64 (ONE copy — 7 is not KEY_DOWN/KEY_UP/MOUSE_ABSOLUTE) |

The reference publishes its own byte-exact golden for exactly this
message in `keyboard_and_mouse_translate_to_byte_exact_control_commands`
(nvst_input.rs:1888-1911; dx=24, dy=24, ts=0x01861330 µs):

```
06022800000000240e0000000000000a0700000000180018
0000000000000000000000003013860100000000
```

`tools/input_reference.py` asserts this golden in `self_prove()` before
emitting the KAT header; `test_kninput.c` pins the same 44 bytes against
the C builder (`t_mouse_relative_golden`) plus BE-i16 negative/extreme
encodings; `kn_selftest.c`'s `input.vectors` re-proves both new singles
(`mouse_rel`, `mouse_rel_neg`) from the SHIPPED binary (48 → 50 checks;
test_kninput 446 → 548).

### Routing: control_partial = sid 6, NOT sid 0

nvst_input.rs:969 routes `MOUSE_RELATIVE` (and `MOUSE_ABSOLUTE`) to
`NvstInputRoute::ControlPartial`, and `NVST_CHANNEL_PROFILE[3]` gives
that channel **sid 6: unordered, PR-SCTP lifetime 300 ms** — with the
reference's own comment on why unordered: with ordered PR-SCTP one lost
report blocks every newer one for the full lifetime ("delayed motion
followed by catch-up"). The jiggle therefore rides the SAME channel and
outbox path (`sess_fb_push(fb, 6, …)` → `sess_fb_sid_params` case 6) as
the QoS batch and the unified-mode NACK-v2 — no new send path, no new
channel, sender-side reliability already pinned by the round-XXXVIII
outbox.

### The jiggle cadence (knvstd_main.c, media loop)

* **Gate**: `pump_persist && pump.input_active` — a new volatile mirror
  of the pump's `activation_sent` latch (single writer:
  `sess_ctrl_try_activate`, single-word reads: the `sctl_*` precedent).
  No input flies before the activation chain's `enable-input(true)`.
* **Pattern**: every `SESS_JIGGLE_INTERVAL_MS` (100 ms → 10 Hz) one
  `kn_input_mouse_relative(±1, 0, now_us − pump.origin_us)`; the sign
  alternates every message, so the cursor nets ZERO drift across the
  run while the server sees continuous motion. Timestamp epoch is the
  activation chain's own (`pump.origin_us`).
* **Volume**: 44 B × 10/s = 440 B/s on sid 6 — an order of magnitude
  under the QoS batch's 50 ms cadence that run 22 already carried with
  `dropped:0`.
* **Evidence**: one `input_jiggle` `first_send` line
  (`{sid:6, interval_ms:100, ts_us}`), the summary's new
  `feedback.jiggle` counter (pushes accepted by the outbox; 73 → 74
  summary specs), and `outbox_by_sid["6"]` — which now mixes QoS/NACK
  and jiggle pushes (~10/s more than run 22's 5861).

### What run 23 decides

* Video survives PAST ~45 s with `feedback.jiggle` climbing → the
  idle/no-input policy was the freeze cause; G3's remaining slices
  (keyboard, wheel, text) become the fidelity roadmap.
* Video still freezes at ~the same wall while the jiggle demonstrably
  flew → input is eliminated too, and the freeze is rig-side
  (encoder/publisher), which no client byte can fix.

### The live verdict (run 23, round XLII) — input eliminated, jiggle stays

Run 23 flew the jiggle exactly as designed: `first_send` at t+1.28 s
(0.95 s after the activation latch opened), 198 reports on sid 6
(opened 8/8, acked, outbox `dropped:0`) through the freeze and beyond.
The server froze every plane anyway — at 10.58 s of pristine video,
EARLIER than run 22's zero-input 41 s. Per the §9 decision matrix that
is branch 2: **real input neither extends nor shortens session
survival; the idle/no-input hypothesis is closed**, and the run-16→22
"fidelity ladder" was a four-point coincidence (runs 20/21 at ~1.2 s of
media already contradicted it). Media survival is a rig-side lottery
(RTSP §30.7). The jiggle nevertheless **stays ON**: input is now a
controlled constant across runs, the cost is 440 B/s, the idle policy
is not formally dead (only 9.2 s of input preceded this freeze — a
longer policy window stays untested), and it keeps this client the
highest-fidelity non-official receiver on the wire. The G3 remainder
(keyboard/wheel/text) is deprioritized behind the rig-side
investigation — run 24 changes the app (one that reaches gameplay
without a login portal; runs 20–23 all parked on Arknights: Endfield's
Qt launcher) rather than adding input types.
