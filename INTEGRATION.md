# INTEGRATION NOTES — Round XXII delta, rev 12b (T10 r16 build-fix + gate hardening)

> **Rev 12b (round XXII):** target T10 r16 caught a round-XXI uncounted-
> replace edit in `kn_net_mbedtls.c` (port snippet landed in the DNS
> lookup; the Win32 body had never been sandbox-compiled). Fixed (DNS
> literal restored) + a second latent bug fixed (missing `<stdlib.h>` —
> i686-only) + **`cross_check.sh` now cross-compiles kn_net_mbedtls.c on
> both arches whenever third_party/mbedtls exists** (the permanent gap
> closure). All gates re-run green; target expects T10 r17 ALL PASS 72.
> Full story: PROJECT-STATE.md round-XXII log, artifacts/T10-round16/.

---

# Round XXI delta, rev 12 (STAGE 2 PRODUCTION PATH: TLS stream + kn_cm/kn_http/kn_json + fake-rig E2E + knvstd --session)

> **Rev 12 supersedes rev 1-11.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §9** (round-XXI status + deviations
> D-s1/D-cm1..3) and **`docs/AUTH.md`** (the OAuth device-flow research,
> login implementation = stage 4).
>
> **Round XXI headline: the stage-2 production signaling path is BUILT
> end-to-end — CloudMatch (POST v2/session + poll + session extraction)
> over a persistent verified TLS-1.2 stream, RTSPS-over-WSS with the
> round-XX cores, the SRTP/peer handoff into kn_rx, and `knvstd
> --session` glue with the connected-UDP media loop — proven against an
> independent Python rig over real loopback sockets (36 checks) before
> any target run.** The only remaining unknowns are cloud-side (R7) and
> documented as D-s1/D-cm1/D-cm2.
>
> ## New files (round XXI)
> 1. `streamer/{include/kn,src}/kn_json.*` — minimal zero-alloc JSON
>    reader (1024-node pool, depth 32, raw spans, unescape incl.
>    surrogate pairs, int64-exact).
> 2. `streamer/{include/kn,src}/kn_http.*` — HTTP/1.1 one-shot client on
>    a stream seam (CL/chunked/read-to-EOF, resumable chunk parser,
>    injected clock).
> 3. `streamer/{include/kn,src}/kn_cm.*` — CloudMatch: the create body
>    (field-for-field with build_create_body), GFNJWT + nv-client header
>    set (Content-Type text/plain quirk), create/poll, and the
>    session_info extraction subset (rtspsEndpoints DERIVATION from
>    connectionInfo usage 16/14+alp{1,6}, media usage 2|17, queue
>    4-source fallback, ready/resumable, phase table).
> 4. `streamer/tests/test_kncm.c` — the THIRTEENTH suite (91 checks:
>    json/http/cm sections, fixtures per cloudmatch.rs:4073-4085).
> 5. `tools/fake_rig.py` — the independent Python rig (HTTP + WS server,
>    own RFC 6455 framing; asserts the ANNOUNCE key echo + PLAY count).
> 6. `tools/sandbox_session_e2e.c` — sandbox-only E2E harness (36
>    checks) driving kn_cm→kn_ws→kn_rtsp→handoff→connected UDP against
>    the rig over real sockets.
> 7. `docs/AUTH.md` — OAuth device-flow research record (endpoints,
>    forms, headers, token semantics; implementation = stage 4).
>
> ## Changed files (round XXI)
> 1. `streamer/include/kn/kn_net.h` + `src/kn_net_mbedtls.c` +
>    `src/kn_net.c` — the persistent TLS-1.2 stream API
>    (`kn_net_tls_stream_*`, Win32+mbedTLS only; complementary honest
>    stubs elsewhere; kn_tcp_open gained a port parameter for 322).
> 2. `streamer/{include/kn,src}/kn_sock.*` — `kn_sock_udp_connect`
>    (kernel peer filter + getsockname local-address refresh);
>    test_knsock 70→82 checks.
> 3. `streamer/src/knvstd_main.c` — `--session` mode (phases A-E per
>    docs/RTSP.md §9; token length-only logging; D-s1 placeholder ICE
>    identity; TEARDOWN on exit) + usage/dispatch.
> 4. Build wiring: CMakeLists + both shell gates + T10 (13 suites;
>    target r16 expects 72 checks) + .gitignore unchanged.
>
> ## Verification (round XXI, sandbox)
> native 13 suites 1309 PASS · cross x64+i686 -Werror PASS · knvstd
> =2-paranoid clean (host -O1, mingw -O2) · CMake 0 warnings + ctest
> 15/15 x2 backends · T13 ALL PASS 51/51(sanitized)/63(mbedTLS) ·
> sandbox_session_e2e 36/36 · T10 dry 61. Self-review/KAT catches:
> the chunked-parser stranded cursor (1-byte KAT), the json root-index
> confusion (cleaned pre-ship), kn_net.c's missing top-level stdio
> (POSIX stubs), hex math in the E2E expectations (1194884=0x123B84).
> NOT target-compiled yet: the Win32 body of kn_net_tls_stream_* and
> --session's Win32 sleep path (T10 r16 is their first compile; both
> mirror verified patterns line-for-line).

---

## Round XX (rev 11, superseded — detail below)

> **Rev 11 supersedes rev 1-10b.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md`** — §1-5 are the wire specification
> (read from nvst_rtsp.rs/cloudmatch.rs @ a49b124 with line refs), §8 is
> this round's implementation status + deviations D-w1..D-r4.
>
> **Round XX headline: the stage-2 signaling cores are implemented and
> sandbox-proven — RFC 6455 client framing (`kn_ws`, 84 checks incl. the
> RFC goldens) and the full RTSP-over-WS state machine (`kn_rtsp`, 127
> checks with every fixture transcribed from the reference's own tests,
> incl. a scripted OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY E2E).** No
> production wiring yet: the TLS stream transport, CloudMatch, OAuth and
> `--session` are round XXI. No existing module changed behaviour (kn_rx /
> kn_sock / knvstd untouched this round).
## New files (round XX)
1. `streamer/include/kn/kn_ws.h` + `streamer/src/kn_ws.c` — RFC 6455
   client framing over an injected byte-stream seam (`kn_ws_io`);
   handshake with CSPRNG key + accept verification; masked sends;
   resumable mid-frame timeouts; control-frame handling (ping→pong echo,
   close answered); bounded caller-owned reassembly; exposed test hooks
   `kn_ws_base64` / `kn_ws_accept` / `kn_ws_rand`. Win32 RNG = `rand_s()`
   (msvcrt — NO new PE import); POSIX = getrandom/urandom fallback.
2. `streamer/tests/test_knws.c` — the ELEVENTH suite (84 checks):
   RFC 4648 + RFC 6455 §1.3 goldens, handshake request/response classes,
   wire-exact frames at every length-boundary, fragmentation, control
   frames, all protocol-error classes, oversize, 1-byte-chunk recv,
   timeout resume. In-memory duplex shim (the framing contract is
   transport-independent by design; real-socket/TLS E2E is the round-XXI
   fake-rig gate).
3. `streamer/include/kn/kn_rtsp.h` + `streamer/src/kn_rtsp.c` — the pure
   RTSP-over-WS client: serializer/parser/request loop (CSeq==Request-Id,
   Bifrost CRLF padding, Request-Id fallback, TEARDOWN mismatch-skip,
   20 s / 4 MiB limits), SDP toolset (`sdp_attribute` with the `||`/`;;`
   quirks, `media_control`, `runtime_key` + `{key_id:024X}` salt, the
   8-profile scan defaulting AEAD_AES_256_GCM_8, `parse_video_peer` with
   the span<16 collapse, the URI ladder), `extract_describe`, the full
   SETUP sweep (poison-rule stop, bundle fallback, 3×3 s injected-sleep
   retries, remaining-deadline narrowing), `build_announce` (complete
   §3 attribute list) and `negotiate_announce` (16-rule range-validated
   offer resolution + clamps + line rewrite).
4. `streamer/tests/test_knrtsp.c` — the TWELFTH suite (127 checks):
   reference-test fixtures transcribed (nvst_rtsp.rs:3443-3470 parser
   cases, :3470+ official control, :2923+ announce params), scripted-
   server full-sequence E2E with byte-exact request inspection, every
   SETUP branch (happy/400-ladder/peerless+bundle/peerless-exhaust/
   fatal-500/client-udp-form), TEARDOWN skip semantics, error paths.
   Injected clock+sleep keep retry rounds instant and countable.

## Changed files (round XX)
1. `CMakeLists.txt` — kn_ws.c + kn_rtsp.c into kncore; knws + knrtsp in
   the test foreach (ctest 12→14).
2. `tools/build_native_tests.sh` + `tools/cross_check.sh` — the two new
   suites + module compiles (12 native suites, 1183 checks).
3. `tests/T10_build_and_test.py` — suites += test_knws, test_knrtsp
   (target r15 expects 71 checks = r14's 69 + 2).
4. `docs/RTSP.md` — §8 implementation status + deviations D-w1..D-r4.

## Verification status (round XX, sandbox)
native 12 suites 1183 checks PASS · cross mingw x64+i686 -Werror PASS ·
ASan+UBSan+LSan clean on both new suites (the single runtime error is the
known vendored sha1.c:26 note, triggered by SHA-1 padding — streamer code
has zero diagnostics) · -Wformat-truncation=2 clean -O0..-O3 on kn_ws.c,
kn_rtsp.c AND test_knrtsp.c · CMake ctest 14/14 both TLS backends ·
T10 POSIX dry-run ALL PASS 60. Self-review catches this round (all fixed
pre-gate): the kn_ws mid-payload timeout was NOT resumable in the first
draft (frame state added), the RT_FOR_LINES macro ran its body per
character (LF-only KAT caught it), the parser status-line scan leaked
into the header block (GARBAGE KAT caught it), blank SDP lines aborted
the attribute scan (the `;;` quirk KAT caught it).



> **Rev 10b (round XVIII)** — the target T13 r1 finding, fixed:
> 1. `streamer/src/knvstd_main.c` — `lb_json_path` (the loopback sanitizer
>    T11 proved) promoted to the platform-neutral section as THE
>    stdout-JSON string escaper, hardened with a `>=0x7F -> '?'` rule
>    (cp1250 argv); every argv/path/foreign-string printf-JSON site routed
>    through it (unknown_selftest name, diagnostics bundle, replay
>    error+summary, capture error/detail/summary, decode error detail).
>    Root cause of T13 r1's only 2 failures: `"out":"C:\Users\..."` raw —
>    `\U` is not a legal JSON escape, summary lines became unparseable.
> 2. `streamer/src/kn_selftest.c` — sibling `st_json_str`; gs.parse's
>    KN_GS_CAPTURE path sanitized (=2 paranoid count unchanged at 9).
> 3. `tests/T13_udp_e2e.py` — per-leg "every stdout line parses as JSON"
>    checks, `rcvbuf_actual` logged per decode leg (R34), POSIX
>    backslash-path contract leg. Target expectation: T10 r14 ALL PASS
>    69 (rebuild is MANDATORY before T13), T13 r2 ALL PASS 61.
> 4. `docs/SOCK.md` — landmine 6 + §7 target record (T10 r13 ALL PASS 69,
>    T13 r1 analysis). Evidence: `artifacts/T13-round1/`.
> Sandbox re-verification: 10 suites 972 PASS, cross PASS, ctest 12/12
> x2, T13 ALL PASS 51/51(sanitized)/63(mbedTLS+gcm256), T10 dry 58.


> **Rev 10 supersedes rev 1-9.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/SOCK.md`** (provenance table with corrected
> nvst.rs line refs, landmines 1-5, the single-thread decision, the E2E
> graph and all sandbox evidence), plus `docs/RX.md` §Stage-2 remainders
> (sockets bullet now DONE).
>
> **Round XVII headline: the stage-2 socket layer is implemented and proven
> end-to-end on loopback — sender → live capture → replay-verify → live
> decode (paced + blast + SRTP cm128/gcm256) — under ASan+UBSan+LSan, on
> three build configurations, before the target ever saw it.** The pure
> kn_rx core is UNCHANGED (one additive API); the round-XVI KAT battery
> still pins every behaviour.
>
> Context this round also CLOSED two target items: T10 round 12
> (**ALL PASS 67 checks** — test_knrx 209/209 on Win7, rx.vectors 153/153,
> registry 13, zero warnings) and the G2-offline demo from `idk_g2.txt`
> (replay verified byte-identical; the demo's `--verify` failure was a
> missing `> replay.jsonl` redirect on the knvstd command — reconstructed
> and PASSED in the sandbox: `VERIFY PASS: 60 frames byte-identical`).

## New files
1. `streamer/include/kn/kn_sock.h` + `streamer/src/kn_sock.c` — the
   bounded UDP layer: refcounted WSAStartup (Win32) / no-op (POSIX),
   monotonic µs/ms clock (QPC / CLOCK_MONOTONIC), IPv4 parse/text,
   open/bind with SO_RCVBUF request + getsockopt READBACK (Linux caps,
   Windows honours — both reported), select-based recv with timeout and
   honest per-platform truncation semantics, sendto, lifetime counters.
   BOTH platforms real (no stubs). Zero allocation.
2. `streamer/tests/test_knsock.c` — the TENTH suite (70 checks):
   clock, ipv4 helpers, lifecycle/refcounts/closed-socket discipline,
   100-datagram varying-size round-trip byte-exact, timeout path,
   65 507 B max datagram, 512-datagram burst drain with seq markers,
   the PLATFORM-DELTA truncation case (Win32 WSAEMSGSIZE-loss vs POSIX
   silent truncation), bad-argument matrix. Section-isolated, never
   aborts (round-XVI suite rule).
3. `tools/send_rx_capture.py` — paced UDP sender for kn_ring_dump-format
   captures (the offline stand-in for a real Mjolnir session): hybrid
   sleep+spin pacing on the capture's µs timeline (sub-ms jitter even
   where Sleep() floors at ~2.9 ms — R30), `--speed 0` blast mode,
   json-lines summary, py3.8-compatible.
4. `tests/T13_udp_e2e.py` — the stage-2 socket gate (loopback only, no
   account): capture leg → vector leg (replay of the LIVE-RECORDED file
   verified against the reference manifest) → decode paced → decode blast
   → SRTP cm128 → SRTP gcm256 (auto-enabled from the binary's
   rx.vectors `srtp_backend` evidence) → `sock.loopback` from the shipped
   binary → check_jsonl. Reuses T10's build; wipes t13work/t13logs at
   start; T13_wynik.txt + T13_skrot.txt. Runs on POSIX for real (not a
   dry-run — real sockets; only the gcm256 leg may skip in bare builds).
5. `tools/sandbox_stream_decode.c` — sandbox-only functional proof of the
   persistent decoder session (NOT a gate, like sandbox_decode_check.c):
   60/60 through per-AU pushes, corrupt-AU survival, sticky abort, and
   the CONTROL case that justifies the API (one-shot per AU delivers 2 of
   60 — P-frames die without references).
6. `tools/fetch_ffmpeg_headers_sandbox.py` — scripts BOOTSTRAP step 4
   (pinned FFmpeg headers at bf0e8316 + avconfig/ffversion stand-ins);
   sandbox convenience, writes into the gitignored third_party/.
7. `docs/SOCK.md` — the round-XVII design record (this delta's canon).

## Changed files
1. `streamer/include/kn/kn_decode.h` + `streamer/src/kn_decode.c` —
   NEW persistent stream session: `kn_decode_stream_open/push/flush/
   stats/close`. One complete Annex-B AU per push (NO parser on this
   path — the round-IX parser-flush landmine cannot occur; end-of-stream
   is `flush()`), codec context persists so inter-frame references
   survive, corrupt AUs counted and the session continues, sticky
   callback abort (-4), cumulative stats, honest stubs in bare builds.
   Reuses kd_ctx/kd_deliver/kd_send_drain unchanged. LSan caught (and
   this round fixed) a `k.nvbuf` leak in close().
2. `streamer/include/kn/kn_rx.h` + `streamer/src/kn_rx.c` — ONE additive
   API: `kn_rx_video_request_keyframe()` (public wrapper over the
   internal `request_keyframe()` helper — same flag, same counter
   discipline). Used by --decode's queue-overflow path. No behaviour
   change to anything the KAT battery pins.
3. `streamer/tests/test_knrx.c` — +6 checks for the new API
   (take-and-clear, collapse-on-repeat, counter counts SETS, NULL-safe):
   184→190 bare, 209→215 mbedTLS.
4. `streamer/src/kn_selftest.c` + `include/kn/kn_selftest.h` — the 14th
   self-test `sock.loopback` (PORTABLE + GATED: 64-datagram burst
   byte-exact, honest 50 ms timeout, SO_RCVBUF readback, closed-socket
   ERR_STATE; ~50 ms). Relevance mapping learned
   `socket/udp/recv/bind/capture/decode` → re-runs sock.loopback
   (session.capture/session.decode failures re-prove the UDP layer).
5. `streamer/tests/test_knselftest.c` — registry 13→14 (two sites),
   host ok_count 4→5, sock.loopback not-skipped+ok on BOTH platforms,
   two new relevance cases (E_CAPTURE_SOCKET, E_DECODE_NO_DATA):
   108→115 checks host.
6. `streamer/src/knvstd_main.c` — graceful-stop plumbing
   (SetConsoleCtrlHandler / SIGINT+SIGTERM → flag polled every loop
   iteration → clean summary + flush); `--capture` (streaming
   kn_ring_dump-format file writer with 32-records/100 ms flush policy,
   64 KiB ring mirror dumped to `<log_dir>/wire_capture.log` on failure,
   RTP-seq meta from the unencrypted header, empty capture = FAILED +
   amplification); `--decode` (single-threaded recv→process→poll loop at
   the reference's 10 ms cadence on the QPC-ms clock, capacity-2 AU queue
   with the reference overflow policy — drop-new + `kn_rx_inval_lose` +
   request_keyframe, per-AU JSONL with sha1, capped rx-event JSONL,
   kf_request lines (unsent until stage 3 — recorded, never dropped),
   1 s transport stats log lines, verify-compatible summary incl.
   `"frames"` alias, ring mirror, `--decode-expect-frames`, AU dump dir,
   pipeline-only honesty in stub builds). usage() + main() dispatch for
   both modes with full argument range validation.
7. `CMakeLists.txt` — kn_sock.c into kncore; `knsock` in the test
   foreach (ctest 11→12).
8. `tools/build_native_tests.sh` + `tools/cross_check.sh` — kn_sock.c +
   test_knsock wiring (10th suite; cross links -lws2_32).
9. `tests/T10_build_and_test.py` — suites += test_knsock; registry check
   13→14; the portable-battery loop (fec/srtp/gs/rx) += sock.loopback
   ("ok in the target binary" — a skip would be a stub, it is real on
   every platform). POSIX dry-run: 57→58 checks.
10. `tests/README.md` — round-XVII header (T13 usage) + the G2-offline
    demo block FIXED: the `> replay.jsonl` redirect is now IN the command
    (the idk_g2.txt failure mode), with an explicit warning line.
11. `.gitignore` — /t13logs/ + /t13work/.
12. `docs/RX.md` — the "Sockets" remainder bullet → DONE (round XVII)
    with the corrected `nvst.rs:235` line ref (the old text cited :222 —
    stale even before this round; :235 is the constant, :5682 the
    setsockopt call site, verified against a49b124).

## Verification status (what ran WHERE)
* Sandbox (this round): native 10 suites PASS (test_knrx 190 bare;
  test_knsock 70); cross mingw x64+i686 -Werror PASS incl. kn_sock and
  the extended knvstd; -Wformat-truncation=2 clean at -O0/-O2/-O3 on
  every touched file (R28 pre-check; the two host-only =2 notes in
  kn_sock/kn_decode are errsz==1 over-approximations — bounded-safe,
  same class as round XI's decision); ctest 12/12 in BOTH CMake
  configurations (schannel + mbedTLS, tls=mbedtls recorded);
  ASan+UBSan+LSan: test_knrx 190/190, test_knsock 70/70 with ZERO
  streamer-code diagnostics (the single runtime error is the PRE-EXISTING
  vendored sha1.c:26 int-promotion UB — pinned by hash, must not be
  edited; sanitizer runs use recover mode, see docs/SOCK.md §4);
  T13 ALL PASS on three builds (44 checks FFmpeg-bare, 44 under
  sanitizers, 55 with mbedTLS incl. the gcm256 leg); T10 POSIX dry-run
  ALL PASS 58; sandbox_stream_decode 23 checks PASS; cross-machine
  parity: target's idk_g2 console JSONL == sandbox replay output
  byte-identical (60 AU lines + counters), regenerated capture matches
  the target's 119 records / 360 333 B.
* Target (PENDING — round XVIII asks for): `python
  tests\T10_build_and_test.py` (expect ALL PASS ~69: 10 suites,
  registry 14, test_knrx 215 in the mbedTLS build) and `python
  tests\T13_udp_e2e.py` (expect ALL PASS 55 — first Winsock execution
  of the live modes; watch the `rcvbuf_actual` line: Windows should
  apply the full 8 MiB). No GPU/T11 rerun needed (no graphics touched).
