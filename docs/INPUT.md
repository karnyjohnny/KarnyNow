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
| `streamer/tests/test_kninput.c` | **794** checks (was 548): activation + single goldens, the round-XLI.1 relative-mouse Rust golden, **the 21 round-L discrete vectors** (key/absolute/button/wheel + the second Rust golden), parser vectors, the 5 cursor tests, truncation sweep |
| `streamer/tests/fixtures/input_login_example.txt` | **round L**: the input-script format as a runnable fixture (137 events, every command form) + the `knvstd_input_check` ctest |
| `streamer/tests/fixtures/input_bad.txt` | **round L**: the rejection fixture (`knvstd_input_check_rejects`, WILL_FAIL) |
| `streamer/include/kn/kn_drawpath.h`, `streamer/src/kn_drawpath.c` | **round L (F16)**: the per-draw capture-naming rule, alias-free (pinned by `test_kndrawpath`, 1051 checks) |
| `streamer/src/knvstd_main.c` | `sess_ctrl_*` (pump-thread service) + the evidence lines + the round-XLI.1 jiggle cadence (§9) + **the round-L input driver `si_*` (§10)** |
| `streamer/src/kn_selftest.c` | the **`input.vectors`** self-test (50 checks, phase A, gated) — the shipped binary re-proves the same goldens; registry 14 → 15 |
| `streamer/tests/test_knselftest.c` | 117 checks — registry/run-all counts and the portable-must-be-OK set updated for `input.vectors` |

Regenerate the vectors with `python3 tools/input_reference.py`; the output
must be byte-identical.

Two ways to prove the builders on any machine:

```sh
./build/test_kninput                          # 794 checks (the full battery)
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

### The live verdict, CORRECTED (run 27, round XLVI) — the jiggle is a DEPRESSANT; default OFF

The XLII verdict above was right about one direction only: input does
not SAVE a session (run 23 froze with the jiggle flying). Run 27
tested the other direction as a clean same-evening A/B — five draws
with `--session-jiggle=0` on five fresh rigs lived **10.3 / 21.8 /
15.4 / 52.2 / 22.9 s** of media (draw 4 = the project record: 52.19 s,
1546/1546 AUs, zero gaps) against the five jiggle-ON draws of runs
23–26 at **10.6 / 10.4 / 11.6 / 13.2 / 11.3 s** — every ON draw inside
the ~10–13 s band, four of five OFF draws beyond it (P ≈ 0.03 % under
the OFF population). **The synthetic mouse motion truncates draws.**
The "stays ON as a controlled constant" decision is REVERSED on
evidence: `knvstd` now defaults to `--session-jiggle=0` (F9), and
`100` restores the legacy behaviour for re-verification. Note what is
NOT indicted: the activation handshake flew in all five OFF draws
(`input_activation` ×5, `ready:1`) and the long draws came WITH it —
the three-message enable-input chain is exonerated; only the periodic
MOUSE_RELATIVE reports are implicated. Mechanism unknown (synthetic-
input detection, portal cursor/focus churn, or a DRS-profile
interaction — §34.1); the G3 remainder (keyboard/wheel/text) stays
deprioritized, and if it ever ships, run 27 says to measure its
survival impact BEFORE adopting any periodic pattern. Full record:
`docs/RTSP.md` §34.

### The verdict, DOWNGRADED (runs 28/28b, round XLVII) — suggestive, not proven

Run 28b flew the legacy jiggle (`--session-jiggle=100`, 3533 reports
over 360 s, zero outbox drops) and produced a **full-length KEEPER**:
`stop_reason:"time"`, 10 800/10 800 AUs, zero gaps/errors — the
stream pristine WITH the synthetic motion flying. A depressant that
permits a perfect six-minute draw is not a depressant in any usable
sense. The correction of the correction: the run-27 A/B remains
*suggestive* (the ON draws of 18:00–20:30Z were 5/5 in-band while OFF
draws grew tails and keepers from 20:41Z on), but the SAME evening
drifted favourable for both settings — band draws → OFF tails → OFF
keeper → ON keeper — and the data no longer separates "jiggle
penalty" from "hot rig pool". Settling it would require interleaved
ON/OFF draws inside one run; with keepers reachable under both
settings that experiment is **deprioritised**. What stands: the
jiggle is NOT necessary (every record was set without it) and NOT
invariably fatal (28b). **The default stays OFF** — no evidence it
helps, the 440 B/s bot-signature risk is asymmetric, and the
activation handshake (the load-bearing part, run 18) is untouched
either way. The G3 caveat stands unchanged: any future periodic
input pattern gets survival-tested before adoption, and any such
test must be same-evening-interleaved or it is unreadable. Full
record: `docs/RTSP.md` §35.3.

### Round XLIX — the interleaved A/B instrument EXISTS (F14), and the idle-policy question is load-bearing again

XLVII deprioritised the jiggle A/B because settling it "would require
interleaved ON/OFF draws inside one run" and keepers were reachable
either way. **Round XLIX ships exactly that instrument**: with
`--lottery=N>1` the jiggle now ALTERNATES per draw — odd draws fly
`--session-jiggle=MS`, even draws are OFF — and every draw states its arm
in a `lottery_arm {"draw":N,"jiggle_ms":M}` stdout line (the summary's
`feedback.jiggle` and the `input_jiggle` lines confirm it afterwards).
`--lottery=1` is byte-identical to XLVIII, and
`--lottery-stop-on-keeper=0` (F15) keeps the lottery running after a
survivor so an A/B actually completes both arms. Trap 17
(same-evening-interleaved or unreadable) is now satisfiable by
construction rather than by luck.

The reason it matters changed, though. Run 30's operator correction
retired the "~600 s server ceiling" reading of runs 29/29g: the Warsaw
pool on a Free tier grants a **one-hour** session in the browser and has
never been cut at ten minutes (`SESSION_LENGTH_TIMER beforeEventMS:3599657`
said the same in run 25). So the ≈594–599 s endings of runs 29/29g need a
cause, and the leading candidate is precisely the one this file has been
arguing about since round XLI.1: a **server-side idle/no-input policy**.
Every ≈600 s draw was parked on a launcher/menu screen with zero input
flying, and run 30 froze two more static, input-less draws (at 75 s and
197 s of media) while their SCTP control plane kept heartbeating — the
rig was alive, the media was cut.

Run 31 is the measurement: four 900 s draws on Watch Dogs: Legion with
the arms interleaved (ON, OFF, ON, OFF) and no early stop. The outcomes
and their meanings are in `START-HERE.md` §4's read matrix and
`docs/RTSP.md` §37.4/§37.7. Two notes for whoever reads it:

- if an ON draw outlives 600 s while OFF draws die at ≈600 s, the jiggle
  stops being "not necessary" and becomes **load-bearing for long
  sessions** — which reverses the XLVI/XLVII default-OFF reasoning for
  soaks (though not the bot-signature caution: a real G3 input path is
  still the better long-term answer).
- if BOTH arms die at ≈600 s, input is exonerated a third time and the
  cut is app- or policy-specific; the next probe is a different app at
  900 s, not a different input pattern.

The G3 caveat is unchanged and now sharper: a launcher-parked session
cannot produce audio or gameplay evidence, so the first *real* input
(a click / ENTER at a chosen t) is the next input-stage candidate —
run 30's silence is the field argument for it (`docs/BUNDLE-AUDIO.md`
§9, trap 24).

## 10. Round L — REAL INPUT (F19): the discrete wire layer, three sources, one gate

> §9 is the *synthetic* jiggle and its A/B history. This section is the
> thing §9 kept deferring: a client that can actually type and click.
> Everything here shipped in round L and is live-pending (run 32).

### 10.1 Why now: the login wall, named from the rig's own notifications

Run 31 (four interleaved Legion draws) decoded all 64 `sctl_010a`
notifications. Their `processName` tally: `UplayWebCore.exe` ×26,
`UplayService.exe` ×16, `upc.exe` ×4 — **46/46 Ubisoft Connect, zero game
processes**. The rig launched Ubisoft Connect for app 100361011 and went
no further, so every draw streamed a static launcher screen (~3
datagrams/AU) with silent audio until it froze.

The operator then retracted XLIX's "Legion launches itself" note after
seeing run 31's `au_00001_k.264`: the stream shows the **Ubisoft login
dialog**. Their explanation matches the process evidence exactly —
play.geforcenow.com's web client performs auto-login **by sending the
credentials into the stream as input**, so the browser is in the game in
seconds while our client, which sent **zero discrete input messages in
four draws**, is handed the login prompt and waits.

A grep of the reference client settles what we can copy: the OpenNOW
sparse clone has **no credential or login handling anywhere in the
streamer protocol** — auto-login is a web-tier feature. So there is no
shortcut. The only route past that wall is the one the browser takes:
**type**. (`docs/RTSP.md` §38.2–§38.3 carries the full analysis.)

### 10.2 The five discrete builders (byte-exact, two Rust goldens)

`kn_input.c` grew the discrete set alongside the round-XLI.1 relative
mouse. All five ride the same `0x0206` envelope as §9; only the inner
body and the timestamp count differ:

| type | code | inner body (offsets from `[20]`) | total | route |
|---|---|---|---|---|
| KEY_DOWN / KEY_UP | 3 / 4 | vk BE u16, modifiers BE u16, 10×`00` | **52 B** | sid 0 |
| MOUSE_ABSOLUTE | 5 | x BE u16, y BE u16, `08 00`, w BE u16, h BE u16 | **52 B** | sid 6 |
| MOUSE_RELATIVE | 7 | dx BE i16, dy BE i16, `00 00` | **44 B** | sid 6 |
| BUTTON_DOWN / BUTTON_UP | 8 / 9 | button u8, `00` | **44 B** | sid 0 |
| WHEEL | 10 | dx BE i16, dy BE i16, `00 00` | **44 B** | sid 0 |

Envelope fields are unchanged from §9 (`[0..2]` command, `[2..4]` payload
length, `[4..8]` packet length BE, `[8..12]` `0x0e` LE, `[12..16]` inner
length BE, `[16..20]` native type LE, body at `[20]`). **The timestamps
are the trap**: they sit at `[36..44]` for every type and additionally at
`[44..52]` for types 3/4/5 (KEY_DOWN, KEY_UP, MOUSE_ABSOLUTE) — *not* at
32/40. Padding to the 24-byte floor happens before the timestamps are
appended, which is what moves them there.

API (all in `kn/kn_input.h`, each documented with its reference line):

```c
int kn_input_key(uint16_t vk, uint16_t modifiers, int pressed,
                 uint64_t timestamp_us, uint8_t *out, size_t out_len);
int kn_input_mouse_button(unsigned button, int pressed,
                          uint64_t timestamp_us, uint8_t *out, size_t out_len);
int kn_input_mouse_absolute(uint16_t x, uint16_t y, uint16_t width,
                            uint16_t height, uint64_t timestamp_us,
                            uint8_t *out, size_t out_len);
int kn_input_wheel(int16_t delta_x, int16_t delta_y,
                   uint64_t timestamp_us, uint8_t *out, size_t out_len);
int kn_input_route_sid(uint32_t input_type);     /* 6 for motion, 0 else */
int kn_input_vk_from_name(const char *name);     /* "enter", "f13", "a" … */
int kn_input_vk_from_ascii(int ch, uint16_t *vk, uint16_t *mods);
```

Constants worth knowing: `KN_INPUT_MOD_SHIFT/CTRL/ALT/META` = 0x01/0x02/
0x04/0x08 (AltGr = right Alt + the ctrl+alt bits, per the reference's
`docs/keyboard-mapping.md`); `KN_INPUT_BTN_LEFT/MIDDLE/RIGHT/X1/X2` =
1/2/3/4/5 (`windows_raw_input.rs:326-332`); `KN_INPUT_VK_ANTI_AFK` =
0x7C. A wheel notch is ±120. **Absolute coordinates are PIXELS inside the
w×h viewport, not normalized 0..65535**, and a zero viewport is rejected
by the reference FFI (`opennow-streamer-ffi:1088-1108`) — so is ours.

Proven three ways: `tools/input_reference.py` (independent Python model,
self-proves 8/8 activation messages + **both** Rust goldens before
emitting the header) → `kn_input_kat_vectors.h` (21 discrete vectors) →
`test_kninput` **794 checks** (byte-exact rebuild of every vector, field
offsets asserted, route/length/type checked, bounds sweep). The two
goldens transcribed verbatim from `nvst_input.rs:1868-1911`:

```
KEY_DOWN vk=0x41 mods=1 ts=0x015b15b1
060230000000002c0e000000000000120300000000410001000000000000000000000000b1155b0100000000b1155b0100000000
MOUSE_RELATIVE dx=dy=24 ts=0x01861330
06022800000000240e0000000000000a07000000001800180000000000000000000000003013860100000000
```

### 10.3 Source 1 — `--input-script=FILE`: a timeline

One event per line, `#` or `;` comments, blank lines ignored. **`t_ms` is
relative to the draw's FIRST MEDIA TICK**, not to process start — so one
file replays identically on every lottery draw however long the queue
took (run 31's queues burned 140 s each on the 403 ladder; a
process-relative script would have typed into a dead channel).

```
<t_ms> key   <vk|name|char> <down|up> [mods]
<t_ms> type  <text to end of line>
<t_ms> mouse <x> <y> <w> <h>
<t_ms> mrel  <dx> <dy>
<t_ms> btn   <1..5|left|middle|right|x1|x2> <down|up>
<t_ms> wheel <dx> <dy>
```

- `<vk>` accepts `0x41`, `65`, a single character (`A`), or a name from
  the reference's `shortcut_virtual_key` table (`enter`, `esc`, `tab`,
  `space`, `up`/`down`/`left`/`right`, `f1`..`f24`, …).
- `[mods]` accepts `shift`/`ctrl`/`control`/`alt`/`meta`/`win` joined with
  `+`, a number (`3`, `0x03`), or `none`. **A single character carries its
  IMPLIED modifiers** (`key A down` = shift+0x41, not a lowercase "a";
  `key ! down` = shift+1); an explicit argument REPLACES them. Without
  that rule a login script silently mistypes every capital.
- `type` expands to one down/up pair per character through the US table,
  spaced `--input-text-gap=MS` (default 30; the up lands halfway).
  Characters the table cannot express are **counted, not guessed**
  (`unmapped_chars`) — see the caveat in §10.7.
- `mouse` is absolute PIXELS in a `w×h` viewport (the CM create body asks
  for 1280×720, so `mouse 640 360 1280 720` is the centre of the screen).
- Capacity 4096 events; the loader sorts by `t_ms` (insertion), so an
  out-of-order file still fires in order.

`streamer/tests/fixtures/input_login_example.txt` is the format's
runnable documentation: 137 events, every command form, the implied-mods
rule, arrows, and a hand-placed anti-AFK pulse. It is also a permanent
ctest (`knvstd_input_check`).

### 10.4 Source 2 — `--input-live`: the console becomes the keyboard

Keystrokes typed into the knvstd console are forwarded as they arrive
(down+up per key). Win32 uses `_kbhit`/`_getch` (including the `0xE0`
extended-key prefix); POSIX uses termios raw mode with `VMIN=VTIME=0` and a
zero-timeout `select`, **restored on every exit path including Ctrl-C** so
the operator never loses their shell. Mapped: arrows, Tab, Enter (`\r` and
`\n` → VK 0x0D), Backspace, ESC, Home/End/PgUp/PgDn/Del; Ctrl-C is sent as
ctrl+0x43 rather than swallowed. At most 16 keys are drained per media
tick so a paste cannot starve the loop. Unmappable input is counted
(`unmapped_chars`), never retyped as something else.

Honest degradation: when stdin is not a TTY (the operator flies with
`> logs\run32.log` — stdout redirection is fine, but a *piped* stdin is
not) the flag reports
`{"event":"input_live","armed":0,"reason":"no_console"}` and the run
continues rather than pretending to read keys.

Use the script for anything timed (a login), `--input-live` for poking at
a menu interactively. Both may be armed at once (`source:"script+live"`).

### 10.5 Source 3 — `--input-anti-afk[=MS]`: the reference's own pulse

`opennow-streamer-core:1604-1648` keeps a session looking alive with
**VK 0x7C (F13) down+up, no modifiers** — F13 is bound to nothing in any
layout, which is exactly why the reference chose it. Our flag sends the
same two messages every MS (bare flag = 60 000; the first pulse logs
`input_anti_afk {"action":"first_pulse"}`). This is the *honest* instrument
for the idle-policy question §9 raised: real key events from the
reference's own keep-awake path, not synthetic ±1 px mouse motion whose
survival effect is confounded (§9's XLVII downgrade).

### 10.6 One sender, one gate, one route table

All three sources go through the same `si_send` → `sess_fb_push(sid, …)`
path, so there is exactly one place where a message can be built wrong:

- **the gate**: nothing flies before the activation chain latches
  `enable-input(true)` (`pump.input_active`) — the same latch the jiggle
  uses, because input ahead of the chain is the one thing the reference
  never does. Due-but-gated events are **counted (`gated`) and retried on
  a later tick**, never dropped.
- **the route**: `kn_input_route_sid(type)` → sid 6 (ControlPartial,
  unordered, 300 ms PR-SCTP lifetime) for MOUSE_RELATIVE and
  MOUSE_ABSOLUTE, sid 0 (ControlReliable, ordered) for keys, buttons and
  wheel — the reference's `NVST_CHANNEL_PROFILE`
  (`nvst_input.rs:72-158`), and the same split §9 proved live in run 23.
- **the clock**: timestamps ride `pump.origin_us`, the activation chain's
  own epoch (`transport_origin.elapsed()` in the reference).
- **staleness**: an event more than 5 s past its due time is skipped and
  counted (`late_skips`), so a recovery tick cannot dump a whole timeline
  into a session that just came back.

Evidence lines (stdout JSONL + knlog, channel `input`):
`input_script_loaded` (path, events, span_ms, unmapped_chars),
`input_live` (armed + reason), `input_first_send` (kind, sid, bytes,
ts_us, source), `input_anti_afk` (first pulse), and a per-draw
**`input_summary`** carrying `events/fired/sent/key/btn/abs/rel/wheel/
gated/build_fail/push_fail/late_skips/unmapped/afk_pulses/live_keys`.
`input_summary` is logged at **WARN when input was armed but `sent` is 0**
— run 31's exact shape, now impossible to miss.

### 10.7 Caveats (read before writing a login script)

1. **The US table only.** `kn_input_vk_from_ascii` implements a US layout.
   A password containing Polish diacritics (`ą ć ę ł ń ó ś ź ż`) cannot be
   expressed: those characters are counted in `unmapped_chars` and
   SKIPPED, so the login would silently mistype. `--input-check=FILE`
   reports the count before you spend a queue slot — **if it is non-zero,
   stop and say so**; the fix (a scancode/layout path, or the reference's
   `INPUT_TEXT` type 23) is a round of its own.
2. **Focus is blind.** Our client shows no video, so a script cannot
   verify where the caret is. The run-32 template therefore uses
   Tab/Enter only and repeats the whole attempt once ~70 s later. If the
   launcher's focus is not on the e-mail field, the fix is a `mouse`
   click at absolute pixels on that field (supported today).
3. **Credentials never leave the rig.** The script file is not a log
   artifact: do not commit it, paste it, or attach it. `--input-check`
   output is safe to share (VK codes only, no text) — send that instead.
4. **A CAPTCHA or 2FA prompt ends this route.** If run 32's video shows
   one, typing is proven but insufficient, and the browser comparison
   (the operator's DevTools capture of play.geforcenow.com's create body
   + input) becomes necessary after all.

### 10.8 Validating a script without a session: `--input-check=FILE`

```sh
knvstd --input-check=login.txt [--input-text-gap=30]
```

Prints one JSON line per event (`i`, `t_ms`, `kind`, and the kind's
fields + the `sid` it will ride), then a summary (`events`, `span_ms`,
`lines`, `comments`, `blanks`, `unmapped_chars`). **Exit 0** = the file
will replay; **exit 2** = a parse error with the line number and the
reason (`unknown command 'teleport'`). Per-event dump is capped at 200
lines. Two permanent ctest legs pin the parser:
`knvstd_input_check` (the 137-event fixture) and
`knvstd_input_check_rejects` (WILL_FAIL on `input_bad.txt`).

This exists because a rejected script must cost nothing: run 31 showed a
draw is 2–20 minutes of queue plus a seat cooldown, and a mistyped
password wastes all of it silently.

### 10.9 Run 32 taught the timing rule: the login UI is on screen at t≈1 s, so the macro must START inside the window draws actually give

Run 32 (docs/RTSP.md §39) was the first run with a real macro: 80 events
over **416 s**, parsed clean on the rig (`unmapped_chars:0`), activation
chain latched at rel 0.83 s (`input_activation ready:1`) — and
`input_summary {fired:0, sent:0, gated:0, late_skips:0}`, because the draw
died at **29 s of media**, long before the macro's first timestamp. The
video proves the waste: the Endfield launcher's `Log In` button is on
screen **from the first decoded frame**. A login timeline that begins at
t≈25–60 s can only ever fire in a keeper draw, and keeper draws are the
rare case.

The rules that follow, for every future script:

1. **First event at t≈6–12 s.** ICE/DTLS settle inside ~1 s, the gate
   latches inside ~1 s, and launchers render their login UI within a few
   seconds. Nothing useful happens before t≈5 s and nothing survives
   waiting past t≈15 s in the draws measured so far (media survival:
   14–122 s in run 31, 29 s in run 32).
2. **Retries are cheap, misses are not.** Repeat the click/type sequence
   at ~30 s / 60 s / 90 s. A `key enter` into a screen that has no dialog
   is harmless; a draw that dies with `fired:0` is a wasted seat.
3. **Add `--input-anti-afk=60000` to every login run.** Two 44-byte
   messages a minute, the reference's own pulse (§10.5), and the only
   instrument that probes the idle-policy hypothesis while the login
   attempt runs.
4. **Absolute coordinates are PIXELS in the 1280×720 viewport** (§10.7 /
   the wire table): the measured targets in run 32's frame 1 are `Log In`
   ≈ (984, 44) and the launcher's `Play` pill ≈ (1104, 632). Re-measure
   per app from `video.264` frames — ffmpeg -i video.264 -vf fps=1 gives
   one PNG per second, which is also how you time the dialog's arrival.
5. **Read `input_summary` as a three-way diagnostic**, not a pass/fail:
   `sent>0` = input flew; `fired:0, gated>0` = the activation chain never
   armed (gate/wire problem); `fired:0, gated:0, late_skips:0` = the draw
   died before the first event (run 32 — move the timeline left);
   `late_skips>0` = a recovery stall ate part of the timeline. (The knlog
   `note` string on the WARN line predates run 32 and overstates the
   `gated:0` case as "the media loop never ticked" — the triple above is
   the correct reading; the note text is queued for correction.)

### 10.10 Run 33: the macro logged in, booted the game and entered the world — F19 proven at full scale

Run 33 (2026-10-29, Endfield, app 107057914) is F19's live proof:

```
input_summary events:80 fired:80 sent:108 key:86 btn:16 abs:6 rel:0 wheel:0
              gated:0 late_skips:0 build_fail:0 push_fail:0 unmapped:0
afk_pulses:14 (every 60 s from t≈1 s)
```

- Every scheduled event fired on time (zero `late_skips`), the gate
  admitted all of them (zero `gated`), and the byte builders produced
  zero failures — 108 messages = 58 typing keys (29 chars ×2) + 28
  anti-AFK keys (14 pulses ×2) + 16 button + 6 absolute-mouse.
- The operator watched the results land: launcher **Play** at t=40 s →
  `Endfield.exe` JustLaunched at t=44.2 s (the first game process in
  project history, §40.1 in RTSP.md); the in-game login fields clicked
  and typed at t≈161–170 s; world-entry clicks at t≈411–416 s; sword
  swung. **Pixel-accurate absolute mouse (1080p viewport) and keyboard
  text entry both work against the real GFN rig.**
- Anti-AFK first-pulse quirk (by design, now documented): `next_afk_ms`
  starts at 0, so the FIRST pulse flies on the first armed tick (here
  ~1.2 s), then every 60 s. Harmless — an extra VK 0x7C pair early —
  but it means `afk_pulses ≈ ceil(elapsed/interval)+1`, and it is what
  `input_first_send` reported at 1.17 s in run 33.
- Two label bugs queued for round N (F23, cosmetic only):
  1. `input_first_send ... source:"script"` printed the *configured*
     source when the first send was actually an anti-AFK pulse — the
     per-event source should be reported.
  2. The `input_summary` WARN note claims `gated:0` with `aus>0` means
     "script never armed" — after F19's gate latch it can also mean
     "everything was admitted" (run 33: `aus 24424`, `gated 0`,
     `fired 80`). The note must key off `fired:0`, not `gated:0`.
- Credential hygiene held: only `--input-check` summaries and counters
  entered the repo; the pasted live credentials were rotated before the
  run per §10.7. (The typed character count in run 33 — 29 — matches
  e-mail+password; no characters are logged anywhere.)

### 10.11 Run 34: the attack macro flew 96/96 — and a credential incident with its procedure

Run 34's macro (`run34_logs/login.txt`, operator's v2) compressed the
login to t≈72–77 s, world entry to t≈330 s and added **ten left-click
attacks at 20 s spacing (t=420–600 s)** as an idle-eviction control.
Result: `events:96 fired:96 sent:126 (key:88 btn:32 abs:6) gated:0
late_skips:0 build_fail:0 push_fail:0`, `afk_pulses:15` — the 32 btn
messages are exactly the 6 login/world clicks + 10 attacks (16 clicks
× down/up), and F23's relabelled first-send line printed
`source:"afk", configured:"script"` live, as designed. The draw kept
the full 900 s (RTSP.md §41).

**The incident:** the same push delivered the macro file WITH the
plaintext credentials (`type <e-mail>`, `type <password>`) into the
public logs repository — the one thing §10.7 forbids. Immediate agent
actions: workspace archive copy redacted to `type <EMAIL-REDACTED>` /
`type <PASSWORD-REDACTED>`, scratch clone copy deleted, no credential
quoted in any doc. Operator procedure issued (chat, step-by-step):
(1) rotate the Endfield/account password NOW — rotation is what makes
the leaked history harmless; (2) delete `run34_logs/login.txt` from the
logs repo (new commit; history rewrite optional AFTER rotation);
(3) from now on the macro lives ONLY on the rig as `build\login.txt`;
if a reference macro must sit in a repo, it carries placeholders;
(4) never paste or push script bodies — `--input-check` summaries only
(§10.7 stands). The typed-character COUNTS (29 in run 33/34) are the
only macro fingerprint that may ever appear in evidence lines.

**Closure (same day):** the operator rotated the password AND, not
trusting git history surgery, **deleted the entire logs repository and
recreated it empty** — no commit with the old credential exists
anywhere upstream. Consequence for future rounds: the upstream
`karnynow-logs` history restarts at 2026-10-30; whatever runs are
missing upstream are safe in this workspace's `karnynow-logs/` archive
(§11's rule that the workspace archive is the only history now bites
in the operator's favour). Next evidence pull = a FRESH clone, not a
pull. The operator's standing commitment: macros stay on the rig;
repos get placeholder bodies only.
