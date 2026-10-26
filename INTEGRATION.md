# INTEGRATION NOTES — Round XXV delta, rev 15 (R7 run 6: DESCRIBE/SETUP/ANNOUNCE 200 LIVE — PLAY 455 root-caused: the reference's pre-PLAY hole-punch burst was missing → the STUN toolkit + the credential ladder + the pump)

> **Rev 15 supersedes rev 1-14.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §12** (the run-6 measurement + the
> fix), §11 (run 5), §10 (runs 1-4).
>
> **Round XXV headline: run 6 walked the whole live signaling chain —
> queue → ready → OPTIONS/DESCRIBE/SETUP/ANNOUNCE all 200 on the real
> rig (the round-XXIV DESCRIBE fix confirmed live; SETUP peered on the
> first sweep round; ANNOUNCE carried the 4164-byte negotiated SDP with
> the D-s1 placeholder identity and was ACCEPTED) — and PLAY was
> rejected 455 "Method Not Valid in This State" with a healthy WS layer
> (rtsp_detail: ws_err 0, no close, 5 text frames). Root cause: the
> reference spawns its media receivers BETWEEN announce() and PLAY
> (lib.rs:894→937→1117) and the bundle thread immediately fires the
> hole-punch burst (3× STUN binding + 1× NATT ping per 20 ms,
> nvst.rs:7438/:7842-7900, dest = bundle_peer() :1880) — we sent PLAY
> with zero UDP egress. Fixed with the sequential pre-PLAY pump (D-s2)
> + the reference's exact STUN wire shapes + its remote-ufrag ladder.
> Bonus catch: the ladder fixtures exposed a pre-existing
> kn_rtsp_sdp_attribute bug (first-match instead of the reference's
> LAST-match across ";;" segments). The user also proved R23 RESUME is
> real: run 6's session was left alive and played in the browser.**
>
> ## Changed files (round XXV)
> 1. `streamer/{include/kn,src}/kn_sock.*` — the STUN toolkit: crc32
>    (IEEE reflected), stun_build (byte-exact port of
>    build_authenticated_stun_packet :4977-5012 — MI HMAC-SHA1 with the
>    RFC 5389 §10.5 length adjustment + FINGERPRINT CRC^0x5354554E),
>    xor_addr_v4, inspect (the :5131-5146 gate — magic/exact-length/
>    MANDATORY fingerprint), check_integrity (:139-160, const-time).
>    test_knsock now links kn_sha1 (+ the vendored sha1) — build wiring
>    updated in both gate scripts.
> 2. `streamer/{include/kn,src}/kn_rtsp.*` — the ladder: increment_hex
>    (:2305), bundle_natt_username (:1327), resolve_remote_ufrag
>    (:2329); describe extracts ufrag_v1 + server_bundle_port; **the
>    sdp_attribute LAST-match fix** (".last()" semantics — pinned by a
>    dedicated check; media_control legitimately stays first-segment).
> 3. `streamer/src/knvstd_main.c` — phase C/D: `ice_creds` (lengths
>    only) + the credential gate (:1729, D-s3 honest skip on the
>    ping6-no-payload error case :1248) + bundle-peer re-connect/
>    restore + `sess_ice_pump` (20 ms bursts, answers the rig's
>    requests, counts successes) + PLAY 455-retry (3 attempts, 600 ms
>    re-pump) + the `ice_preplay` counters line + `attempt` in the PLAY
>    event.
> 4. `streamer/tests/test_knsock.c` — test_stun: goldens from an
>    independent Python implementation (hmac/hashlib/zlib), RFC 2202
>    HMAC, tamper/short/no-fingerprint rejects, xor-addr round-trip:
>    **82 → 98 checks**.
> 5. `streamer/tests/test_knrtsp.c` — test_ice_ladder (fixtures
>    transcribed from the reference's own tests :3860-3920) + the
>    last-wins sdp pin: **132 → 150 checks**.
> 6. `tools/fake_rig.py` + `tools/sandbox_session_e2e.c` — the rig
>    runs an independent Python STUN validator/responder on the media
>    socket and GATES PLAY (first PLAY always 455; later need ≥1
>    validated request); the harness pumps, asserts the ladder
>    (a1b24800→a1b24801) and the 455→200 sequence: **36 → 43 checks**
>    (rig evidence: ice_req 12, ice_resp 12, play_455 1, plays 1).
> 7. `docs/RTSP.md` §12, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXV, sandbox)
> native **13 suites PASS** (knsock 98, knrtsp 150, kncm 147) · cross
> x64+i686 `-Werror` PASS · ASan+UBSan clean on both touched suites
> (the single known vendored sha1.c:26 note aside) · kn_rtsp.c +
> knvstd_main.c paranoid `-Wformat-truncation=2` clean (host -O1 +
> mingw -O2 ×2); kn_sock.c's two =2 hits are PRE-EXISTING round-XVII
> honest-truncation error strings (standard gates clean — left alone,
> churn discipline) · CMake fresh both backends 0 warnings + ctest
> **15/15 ×2** · sandbox_session_e2e **43/43** · T10 POSIX dry ALL PASS
> **62** (baseline).
> **Target expectations:** next T10 → ALL PASS **72** (suite count
> unchanged). Then **R7 run 7**: expect `ice_creds enabled:1`,
> `ice_preplay` counters > 0, PLAY attempt 1 possibly 455 → attempt 2
> 200, then `handoff` + the media loop (the first real Mjolnir SRTP →
> kn_rx → AUs = R32's final stage-2 gate). If PLAY still 455s:
> `ice_preplay.ok_in` (did the rig answer?) + `ice_creds` (which ladder
> leg fired) localize it.

---

# Round XXIV delta, rev 14 (R7 run 5: FIRST READY SESSION — the queue fix confirmed live; DESCRIBE died on the 8 KiB per-message scratch → fixed + binary frames + Host:port + rtsp_detail instrumentation)

> **Rev 14 supersedes rev 1-13.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §11** (the run-5 measurement + the
> fix), §10 (run 1-4 + the control-host fix), §9 (round XXI deviations).
>
> **Round XXIV headline: R7 run 5 took the round-XXIII fix to the live
> cloud — `cm_control` switched to np-waw-01 (source sessionControlInfo),
> the session progressed (seat_step 1→3) and reached `status:2 ready`
> with a real rtsps endpoint; the RTSPS/WSS transport then answered
> OPTIONS 200 on a REAL GFN rig and died at `DESCRIBE rc:-2 status:0`.
> Root-cause candidates (both produce exactly that signature at the
> first big response; both fixed): kn_rtsp_request read each WS message
> into an 8 KiB scratch — real DESCRIBE SDPs are bigger, while the
> reference accumulates whole messages up to the 4 MiB total
> (nvst_rtsp.rs:559-566, :614-623) — and the adapters refused BINARY
> frames the reference accepts (:621). Also fixed: the Host header now
> carries :port like the reference (:1070-1076). Failed rtsp steps now
> emit an `rtsp_detail` evidence line (kn_rtsp err + WS close code +
> frame counters), so run 6 measures even if it fails elsewhere.**
>
> ## Changed files (round XXIV)
> 1. `streamer/src/kn_rtsp.c` — `kn_rtsp_request` reads DIRECTLY into
>    the accumulation buffer's tail (no per-message cap below the 4 MiB
>    total; runaway-shim guard kept; the seam contract is unchanged).
> 2. `streamer/src/knvstd_main.c` — `sess_ws_recv_text` accepts
>    TEXT|BINARY; Host = the rtsps authority WITH the port; NEW
>    `sess_rtsp_detail` (err[160] + ws_err/ws_close/ws_text/ws_bin)
>    emitted at all five step-failure sites (OPTIONS/DESCRIBE/SETUP/
>    ANNOUNCE/PLAY).
> 3. `tools/sandbox_session_e2e.c` — the same binary acceptance + Host
>    authority (the two implementations stay mirrored).
> 4. `tools/fake_rig.py` — the DESCRIBE SDP is padded to ~11 KiB and
>    delivered as TWO binary messages (the run-5 shape, exercised over
>    real sockets by the E2E).
> 5. `streamer/tests/test_knrtsp.c` — `test_big_messages`: a >19 KiB
>    single-message DESCRIBE parses (the old scratch died here) + the
>    same response split across two messages accumulates: **127 → 132
>    checks**.
> 6. `docs/RTSP.md` §11 (the run-5 record incl. the queue-position
>    observation: the NATIVE queue published step transitions, not
>    per-poll ticks — NOT a defect, keep observing), this file,
>    `PROJECT-STATE.md`.
>
> ## Verification (round XXIV, sandbox)
> native 13 suites PASS (test_knrtsp 132/132, test_kncm 147/147) ·
> cross x64+i686 `-Werror` PASS · paranoid `-Wformat-truncation=2` clean
> (kn_rtsp.c/knvstd_main.c, host -O1 + mingw -O2 both arches) · CMake
> 0 warnings + ctest **15/15 ×2** · sandbox_session_e2e **36/36** with
> the new rig behaviour (binary + split + padded SDP over real loopback
> sockets; announce key echo + plays=1 asserted by the rig) · T10 POSIX
> dry-run ALL PASS **62** (baseline).
> **Target expectations:** T10 r19 → ALL PASS **72** (suite count
> unchanged). Then **R7 run 6**: same command; expect DESCRIBE 200 →
> the SETUP sweep → ANNOUNCE (D-s1's first real verdict) → PLAY → the
> media loop; any failure now names its stage in `rtsp_detail`.

---

# Round XXIII delta, rev 13 (R7 runs 1-4: the first REAL CloudMatch traffic — the control-host bug FOUND + FIXED)

> **Rev 13 supersedes rev 1-12b.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §10** (the R7 measurement + the fix),
> §9 (round XXI status + deviations D-s1/D-cm1..3), `docs/AUTH.md`.
>
> **Round XXIII headline: the user ran `knvstd --session` against the
> LIVE production backend for the first time (T10 r17 build, ALL PASS 72;
> a real pasted GFNJWT + appId 107057914). Create works — 4/4 real
> sessions with plausible live queue positions. The queue wait FAILED:
> polls froze at a stale queuePosition (40+ identical lines) and ended in
> `cm_poll_failed`. Root cause: the poll loop stayed on the global create
> front (`prod.cloudmatchbeta.nvidiagrid.net`) instead of switching to
> the REGIONAL control node the create response nominates
> (`sessionControlInfo.ip` — `np-waw-01.cloudmatchbeta.nvidiagrid.net`
> in the user's DevTools capture; exactly where the official client polls
> at ~4.1 s and exactly what cloudmatch.rs does: control_base learned at
> :401-414/:1149-1152/:1966-1984, every poll goes there :652-656).
> Fixed + the poll loop hardened to the reference's retry/404 semantics
> + a best-effort DELETE cancel so an aborted wait no longer orphans a
> session (runs 1-4 each left one behind).**
>
> ## Changed files (round XXIII)
> 1. `streamer/{include/kn,src}/kn_cm.*` — `kn_cm_session` learns
>    `control_host/control_port/have_control` (sessionControlInfo) and
>    `req_status_code/req_status_desc` (requestStatus — filled even when
>    the session extraction fails, so a rejected payload still says why);
>    NEW `kn_cm_control_base` (the trust mirror of
>    `trusted_learned_server_base` + `public_session_host/_ipv4`,
>    cloudmatch.rs:2400-2456: https-only, port 443, private/loopback/
>    link-local/CGNAT/multicast/reserved/.local/userinfo/path all
>    rejected — a response can never redirect polling into a private
>    address; 0 = fall back to the create host, which is also what keeps
>    the loopback fake-rig E2E unchanged); NEW `kn_cm_cancel` (DELETE
>    `v2/session/{id}` = the reference's stop_at :789-800; `cm_round`
>    now tolerates `out == NULL`).
> 2. `streamer/src/knvstd_main.c` — phase A: the poll-target switch (new
>    `cm_control` JSONL event = the R7 evidence line), cadence 2 s → 4 s
>    (`SESS_POLL_MS`, the capture's Δ≈4.1 s), per-round 3-attempt
>    transient retry (408/425/429/5xx, 250/750 ms — get_session
>    :1369-1394), 404 → `session_not_found`, requestStatus.statusCode≠1
>    → failed round with the cloud's description in a `cm_poll_error`
>    event, failed rounds never clobber the live session state
>    (poll-into-temp + commit), 4 consecutive bad rounds tolerated before
>    `cm_poll_failed`, the `not_ready` line carries `rc`+`http_status`,
>    the `cm_create` error line carries the cloud's requestStatus
>    (cloud_status 11 = SESSION_LIMIT — runs 1-4 orphaned four queued
>    sessions), best-effort `cm_cancel` on every not_ready exit except
>    404, `cm_created` now includes the initial queue_position,
>    `--session-queue-timeout` default 180 → 600 s.
> 3. `streamer/tests/test_kncm.c` — NEW control-base battery (the exact
>    R7 capture fixture + the full accept/reject matrix + requestStatus-
>    on-failed-extract) and the cancel wire checks (DELETE line, Host,
>    GFNJWT header, 204/404/200-with-body, empty-id guard): **91 → 147
>    checks**.
> 4. `tools/fake_rig.py` — answers `DELETE /v2/session/{id}` with 204 No
>    Content (the validate_delete_response shape); E2E legs unchanged.
> 5. `docs/RTSP.md` §10 (R7 runs 1-4 + the fix + the honest not-covered
>    list), this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXIII, sandbox)
> native **13 suites PASS** (test_kncm 147/147, incl. ASan+UBSan+LSan
> run) · cross x64+i686 `-Werror` PASS · kn_cm.c + knvstd_main.c clean
> under `-Wformat-truncation=2` at -O1 host / -O2 mingw (x64+i686) ·
> CMake fresh configure both backends: 0 warnings, ctest **15/15 ×2** ·
> sandbox_session_e2e **36/36** (exercises the control-host FALLBACK
> path: the rig's 127.0.0.1 is rejected by the trust gate → create-host;
> the ACCEPT path is unit-pinned by the capture fixture — a public-host
> loopback rig is impossible without DNS games) · T10 POSIX dry-run ALL
> PASS **62** (62 is also the pre-delta baseline on this tree — the
> round-XXI "61" predates a check added since; verified by a stashed
> rerun).
> **Target expectations:** T10 r18 → ALL PASS **72** (suite count
> unchanged; test_kncm's internal count is not a T10 check). Then **R7
> run 5** = the same `--session` command: expect `cm_control` with
> `source:"sessionControlInfo"`, a MOVING queue_position, and — if a
> seat arrives — phase B onward (D-s1 gets its first real answer there).
>
> ## Known gaps carried forward (honest)
> * Phases B-E aborts still don't cancel the session (churn discipline;
>   next round).
> * IPv6 trust checks are prefix-class approximations, not a full
>   IpAddr decode (documented in kn_cm.c; reject-direction only affects
>   exotic control hosts, which fall back to the create host).
> * D-cm1/D-cm2 measured OK AT CREATE (4/4 real sessions); seat/streaming
>   stages still unmeasured.

---

# Round XXII delta, rev 12b (T10 r16 build-fix + gate hardening)

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
