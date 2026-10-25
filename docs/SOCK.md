# SOCK.md — kn_sock + the stage-2 live modes (`--capture`, `--decode`)

Round XVII (2026-10-25). Provenance baseline: OpenNOW `nvst.rs` @ `a49b124`
(sha256 `f52fc09f…`, 14 695 lines — re-verified this round).

This is the platform-glue layer that connects a REAL UDP socket to the pure
`kn_rx` pipeline (which by design never touches sockets — see RX.md). It
closes Next-steps item 2 of round XVI: *"Stage-2 sockets: the Mjolnir UDP
receive loop → `kn_rx_video_process_datagram` + paced `kn_rx_video_poll` on
a QPC clock → kn_ring capture of raw datagrams → `--decode` mode feeding
`kn_decode` from the frame callback."*

---

## 1. Provenance (every constant read from the reference, not remembered)

| Fact | Value | nvst.rs @ a49b124 |
|---|---|---|
| UDP receive buffer | `NVST_UDP_RECEIVE_BUFFER_BYTES = 8 MiB` (applied at :5682) | :235 |
| Receive poll interval | `UDP_RECEIVE_POLL_INTERVAL = 10 ms` | :244 |
| Control poll interval (stage 3) | `1 ms` | :247 |
| Max video packet | `65 519` | :218 |
| Reorder window / NACK queue | `1024 / 2048` | :224-225 |
| Encoded-frame queue capacity | **2** (2-3 on a 2 GB machine) | analiza/04 §2.4 + `docs/streamer-comparison/video.md` pkt 5 |
| Queue overflow policy | clear the reference chain + request keyframe | analiza/04 §5 ("Przepełnienie kolejki zakodowanej") |

The reference runs an async tokio task per socket; this port is a
blocking-with-timeout `select()`+`recvfrom()` loop. Same cadence constants,
different concurrency model — the deviation is deliberate (below, §4).

## 2. `kn_sock` — the module

`streamer/include/kn/kn_sock.h` + `streamer/src/kn_sock.c`.

* **Both platforms are REAL** (Win32 winsock2 / POSIX BSD sockets). Unlike
  `kn_net` (whose POSIX side is an honest stub), kn_sock is fully testable
  in the sandbox — the whole stage-2 E2E (sender → capture → decode) runs
  on Linux loopback before the target ever sees it.
* Zero allocation; caller-owned buffers; every failure carries the raw
  platform error (`WSAGetLastError`/`errno`) in `last_error`.
* `kn_sock_now_us()/now_ms()` — QPC (Win32) / `CLOCK_MONOTONIC` (POSIX).
  This is the live `now_ms` clock fed to every `kn_rx` call (the pipeline
  itself stays clock-free — its "DETERMINISTIC TIME" contract is why the
  replay matrix and the live path share one core).
* `SO_RCVBUF` is set AND read back (`rcvbuf_actual`): **Linux caps at
  `net.core.rmem_max` (~208 KiB unprivileged, reports 2× stored = 425 984);
  Windows honours the 8 MiB request.** Sandbox measurements confirmed both.
  Every summary/log reports requested AND applied so burst headroom is
  never a guess.
* Truncation PLATFORM DELTA (documented in the header, tested both ways in
  `test_knsock`): Win32 `recvfrom` with an undersized buffer fails
  `WSAEMSGSIZE` and LOSES the datagram (→ `KN_SOCK_ERR_MSGSIZE`, counted);
  POSIX truncates silently (→ `out_truncated=1`). Real callers size the
  buffer ≥ 65 535 (UDP payload max is 65 507), so this is unreachable in
  production — it exists so the difference can never surprise anyone.

### Landmines hit this round (all recorded at the point of the fix)

1. **`_POSIX_C_SOURCE` must precede EVERY system include** — the
   kn_diag/kn_selftest landmine re-fired twice in fresh files (kn_sock.c:
   `clock_gettime` hidden; test_knsock.c: `nanosleep` hidden). Any new
   POSIX-side file needs the guarded preamble as its first lines.
2. **mingw `socklen_t` lives in `<ws2tcpip.h>`**, not `<winsock2.h>` alone
   (same finding kn_net.c documents — it re-bites every new socket file).
3. **Portable headers cannot use `UINT_PTR`** (a windows.h type): the
   socket handle field is `uintptr_t` (== UINT_PTR on every mingw arch).
4. **WSAStartup refcount leak**: `open()`'s post-startup error paths
   (bad bind_ip, bind() failure) must pair the startup — fixed by making
   `is_open` the ownership marker from the moment startup succeeds (I/O is
   gated by `fd_valid`, so a half-open socket can never recv/send).
   Caught in self-review before any gate ran.
5. **`-pthread` is load-bearing on the host builds**: glibc hides POSIX
   declarations under strict `-std=c11` unless the compile links pthread —
   `build_native_tests.sh` always passed it; ad-hoc manual builds must too.

## 3. The knvstd live modes

### `--capture` — the R32 recording mechanism

```
knvstd --capture [--capture-port=N] [--capture-bind=IP]
                 [--capture-out=FILE] [--capture-seconds=N]
```

* Writes every datagram SEQUENTIALLY to `--capture-out` (default
  `gs_capture.hex`) in the **exact `kn_ring_dump` line format**
  (`"<mono_us> <meta_%08x> <flags> <len> <hex…>"`, knring.c `dump_visit`)
  — so a real cloud session is replayable by `--replay-capture` and
  verifiable by `make_rx_capture.py --verify` with ZERO tooling changes.
  `meta` = the RTP sequence read from the unencrypted header (valid even
  under SRTP); `mono_us` = `kn_sock_now_us()`.
* **Deviation from the round-XVI Next-steps wording** ("kn_ring capture"):
  the PRIMARY record is the streaming file, not the ring — a ring only
  ever holds the last N records, which cannot record a whole session on a
  2 GB machine. The 64 KiB kn_ring (max_record 1312 → ~48 records,
  §7.10 sizing) is kept as the failure-diagnostic MIRROR: on a failed run
  it is dumped to `<log_dir>/wire_capture.log` so the §7.7 auto-bundle
  carries recent wire bytes (the bundle's logs/ enumeration picks up
  `*.log`).
* Flush policy: every 32 records or 100 ms (`fflush`, never `fsync` —
  §7.12). Worst-case loss on a hard crash: < 32 datagrams (R2b-honest).
* Ctrl-C: `SetConsoleCtrlHandler` (Win32) / `SIGINT+SIGTERM` (POSIX) set a
  stop flag → clean summary + final flush instead of a torn write.
* 0 datagrams = FAILED run (rc 1) + `kn_log_on_failure("session.capture",
  E_CAPTURE_EMPTY…)` + auto-bundle: an empty capture is evidence of a
  misconfiguration, never a silent pass.

### `--decode` — the live stage-2 receive path

```
knvstd --decode [--decode-port=N] [--decode-bind=IP] [--decode-seconds=N]
                [--decode-srtp=PROFILE:TAG:KEY:SALT] [--decode-vps=N]
                [--decode-au-dir=DIR] [--decode-expect-frames=N]
```

Socket → `kn_rx_video_process_datagram` (real QPC-ms clock, SRTP session
when `--decode-srtp` is given) → `kn_rx_video_poll` at the reference's
10 ms cadence → frame callback → **capacity-2 encoded queue** →
`kn_decode_stream_push` (persistent session, §5) → per-frame stats.

* **Queue overflow policy is the reference's** (analiza/04 §5): the NEW AU
  is dropped, `kn_rx_inval_lose()` marks the reference chain broken (RFI
  owns recovery when frame types are known → subsequent frames are
  WITHHELD until an I-frame clears it), and when RFI does not own it,
  `kn_rx_video_request_keyframe()` sets the flag the stage-3 control
  channel will send. `q_overflows` is in every summary.
* Keyframe requests surface as stdout lines
  (`{"event":"kf_request","sent":false,…}`) + counter — the transport that
  would SEND them (SCTP/RTCP) is stage 3; until then a request is
  recorded, never silently dropped (§10.6: name your unknowns).
* The summary line is deliberately **field-compatible with the replay-mode
  summary and `make_rx_capture.py --verify`'s counter contract** (it
  carries `"frames"` as an alias of `"aus"`): the SAME manifest
  verification runs against a live socket session as against an offline
  replay. This is the cross-implementation contract extended to live
  traffic.
* `--decode-expect-frames=N` ends the run early (T13's legs use 60);
  "produced" = decoded frames when the build has FFmpeg, assembled AUs in
  a pipeline-only build (honestly reported via `"decoder"`).
* Live `cfg.timeout_ms` = the real 5 s default (replay mode intentionally
  uses MAX so capture-timeline gaps don't fire it — the difference is the
  point: decode mode exercises the production watchdog).

### Threading decision (single-threaded, documented)

`--decode` runs one loop: `recv(10 ms timeout)` → process → drain the AU
queue inline → poll every 10 ms. No threads, no locks: `kn_rx` is
single-threaded by contract, and the 8 MiB SO_RCVBUF absorbs decode stalls
(worst 720p frame on the E5500 ≈ 16 ms ≈ 500 datagrams of headroom at
30 fps). Precedent: the stage-1 loopback ran the same shape at 30.000 fps
for 600 s (G1). The queue semantics are already the thread-ready ones —
if live G2 measurements ever show recv backlog during decode stalls, the
receive side moves to a thread without touching pipeline logic.
**Revisit condition:** measured socket-buffer growth or `recv_errors` in a
real session's decode summary.

## 4. `kn_decode_stream_*` — the persistent decoder session

`kn_decode_h264_mem` is one-shot (parser + codec context per call) — live
per-AU feeding through it destroys inter-frame references. **Measured, not
argued** (`tools/sandbox_stream_decode.c` control case): one-shot per AU
delivered **2 of 60** frames of the synthetic clip (58 failed); the
persistent session delivered **60/60, 0 failures** through the same split.

Contract (header docs are normative): one complete Annex-B AU per push
(what `kn_rx` delivers; NO parser on this path — the round-IX parser-flush
landmine does not exist here, the end-of-stream drain is
`kn_decode_stream_flush()`); corrupt AUs are counted and the session
CONTINUES (proved: a poisoned slice mid-stream still decoded 60/60 with
error concealment); callback abort is sticky (`-4`); stats cumulative.
`to_nv12=1` runs the production swscale path so measured costs equal the
presentation chain's.

**LSan finding (round XVII, fixed):** `kn_decode_stream_close()` leaked
`kd_deliver`'s NV12 buffer (`k.nvbuf`, 115 264 B at 320×240) — the
one-shot path frees it in `kd_run`'s `out:` block, the stream close must
too. Caught by the R33 sanitizer gate on the first E2E run; the whole T13
matrix now passes under ASan+UBSan+LSan.

**Vendored-code UBSan note (pre-existing, NOT a round-XVII defect):**
`third_party/crypto-algorithms-sha1/sha1.c:26` (`data[j] << 24`, int
promotion of a BYTE ≥ 128) is formal UB in the pinned, hash-verified Brad
Conte file — it must NOT be edited (T10 verifies its SHA-256). Sanitizer
runs therefore use recover mode (or `-fno-sanitize=shift-base`); the gate
is: **zero diagnostics from `streamer/` code**, suite completes, 0
failures. Verified this round: the only runtime error in the whole
sanitized T13 comes from that vendored line.

## 5. `sock.loopback` — the 14th self-test

Portable and GATED (a loopback-UDP failure blocks stage 2 on any
platform): binds two ephemeral 127.0.0.1 sockets, 64-datagram burst
byte-exact, honest 50 ms timeout, SO_RCVBUF readback ≥ 64 KiB,
closed-socket discipline. Runs from the shipped binary in ~50 ms. The
relevance mapping learned socket-flavoured keywords (`socket/udp/recv/
bind/capture/decode`) → `session.capture`/`session.decode` failures
re-prove the UDP layer automatically (§7.7.4).

## 6. The E2E mechanism (offline rehearsal of a real session)

```
make_rx_capture.py ──► cap.hex + cap.json (reference verdict)
        │
        ▼  tools/send_rx_capture.py (paced on the capture's µs timeline,
        │   hybrid sleep+spin pacing; --speed 0 = blast)
        ▼
knvstd --capture ──► live.hex ──► knvstd --replay-capture ──► verify
knvstd --decode  ──► stdout jsonl ──────────────────────────► verify
```

`--verify` accepts BOTH replay and decode stdout (field-compatible
contract, §3). T13 (`tests/T13_udp_e2e.py`) automates the whole graph:
capture leg → vector leg (replay of the LIVE-RECORDED file) → decode
paced → decode blast → SRTP cm128 → SRTP gcm256 (auto-enabled when the
binary's `rx.vectors` evidence reports `srtp_backend=1`) → the binary's
own `sock.loopback` → jsonl hygiene. It reuses T10's build by default and
wipes `t13work/`+`t13logs/` at start (the T11 accumulation lesson).

### Sandbox evidence (round XVII, before handover)

| Build | T13 verdict | Notes |
|---|---|---|
| `knvstd_ff` (system FFmpeg 5.1, bare SRTP) | **ALL PASS 44** | real decode 60/60 paced+blast+cm128; gcm256 honestly skipped |
| same + ASan/UBSan/LSan | **ALL PASS 44** | after the `k.nvbuf` leak fix |
| CMake mbedTLS (pipeline-only decode) | **ALL PASS 55** | gcm256 leg ENABLED + verified |

Plus: `test_knsock` 70 checks (host, and under sanitizers: zero
diagnostics from streamer code); `test_knrx` 190 bare / 215 mbedTLS;
native suites 10/10; cross mingw x64+i686 `-Werror` clean; knvstd/
kn_sock/kn_decode/kn_rx clean under `-Wformat-truncation=2` (R28
pre-check) at -O0/-O2/-O3 on every touched file.

Deterministic cross-machine parity: the target's round-XVI demo capture
(`idk_g2.txt` console output) was reconstructed in the sandbox — the
regenerated `cap.hex` (seed 1) matches the target's byte counts exactly
(119 records / 360 333 B) and the target's console JSONL verifies
`VERIFY PASS: 60 frames byte-identical` against the regenerated manifest.

## 7. Target-side expectations (round XVIII)

* `python tests\T10_build_and_test.py` — expect **ALL PASS ~69 checks**:
  10 suites (new `test_knsock` ~70 checks), registry **14**, new
  `sock.loopback ok in the target binary` check.
* `python tests\T13_udp_e2e.py` — the first Winsock execution of the live
  modes: expect **ALL PASS 55 checks** (the mbedTLS default build enables
  the gcm256 leg; FFmpeg static build enables real decode). Windows
  Firewall does NOT filter loopback, so no prompt/allow is expected for
  T13's 127.0.0.1 binds — a real cloud session (binding ANY, inbound from
  the internet) is a different story and will be measured when the
  WSS/RTSP client lands (landmine registered, not guessed).
* Landmine watch on target: `rcvbuf_actual` in the decode begin line —
  Windows should report the full 8 MiB request; if it reports less, the
  burst-headroom math changes (recorded either way; that is the
  measurement).
