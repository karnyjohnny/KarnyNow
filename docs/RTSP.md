# RTSP.md — WSS + RTSP-over-WebSocket + CloudMatch session (stage-2 research record)

Round XIX (2026-10-25) RESEARCH PHASE. Everything below was read from the
OpenNOW tree @ `a49b124` THIS round (line numbers point at that commit);
nothing is recalled from memory. Implementation is round XX — this document
is its specification and the provenance anchor.

Key files:
* `native/opennow-streamer/crates/opennow-streamer-core/src/nvst_rtsp.rs` (3921 lines) — the RTSP-over-WSS client
* `native/opennow-core/src/cloudmatch.rs` (7398 lines) — session lifecycle (`POST v2/session`, polling, resume)
* `docs/core-protocol.md` — shell↔native IPC (not the NVIDIA wire; useful for the TUI contract)

---

## 0. The full session flow (where stage 2 plugs in)

```
OAuth login (login.nvidia.com — TLS 1.2 verified on target, T12/T10 r12+)
   → GFNJWT token
CloudMatch POST {base}/v2/session        (cloudmatch.rs:361-371, body :1643-1762)
   → session status polling              (ready = status 2|3 AND rtspsEndpoints non-empty, :1524-1533)
   → session_info (the R22 31-field context, :2065-2103)
        signalingUrl = rtspsEndpoints[0] with "rtsps://" → "wss://"   (:2046-2048)
WebSocket connect wss://host:port/rtsp   (nvst_rtsp.rs:1922-1988, default port 322)
   header: x-nv-sessionid: <sessionId>, content-length: 0            (:513-527)
RTSP text messages inside WS frames:
   OPTIONS → DESCRIBE → SETUP(video) → ANNOUNCE(our SDP) → PLAY      (:1083-1089, :777-800)
   → handoff JSON (SRTP key/salt/profile, video peer, packet size…)  (:1287-1327)
UDP media from videoPeerIp:videoPeerPort → kn_rx (ALREADY BUILT + target-verified)
```

## 1. Transport: RTSP over WebSocket (nvst_rtsp.rs)

* **URL transform** `rtsp_endpoint_urls()` (:1922-1988): `rtsps://host[:port]`
  → WS `wss://host:port/rtsp` + RTSP target `rtsps://host:port`.
  **Default port 322** (:1969-1971). Raw `rtsp://` is REJECTED
  (`nvst-raw-rtsp-unsupported`). Validation: no '@' in authority, no '#',
  no control chars, IPv6 must parse (:1947-1966).
* **WS handshake headers**: `x-nv-sessionid: <session_id>` (the CloudMatch
  sessionId — this is the "native /rtsp websocket session-ID authentication"
  core-protocol.md:854 refers to) and `content-length: 0` (:517-524).
* **Connect retry**: 3 attempts; only `nvst-connect-failed` /
  `nvst-service-unavailable` are transient (:655-680). HTTP errors on the
  WS handshake: 401/403 → `nvst-signaling-forbidden`, 503 →
  `nvst-service-unavailable` (with the "not a video decoder error" note),
  else `nvst-signaling-rejected` (:701-732).
* **Message framing**: one RTSP request = ONE WS **Text** frame (:669-670).
  Responses arrive as Text (Binary → lossy-UTF8 append, :620-624), Ping →
  Pong (:624-626), Close → `nvst-network-failed` (:627-632).
* **Request serialization** `send_request()` (:641-671):
  ```
  {METHOD} {uri} RTSP/1.0\r\n
  CSeq: {n}\r\n
  Request-Id: {n}\r\n          ← SAME incrementing counter as CSeq
  {headers...}\r\n
  [Content-Length: {len}\r\n]  ← only when body non-empty
  \r\n{body}
  ```
* **Response parser** `take_rtsp_response()` (:1806-1920): leading CR/LF
  runs are DROPPED first (Bifrost seats insert blank CRLF blocks between
  WS-carried responses — transport padding, not empty responses, :1809-1817);
  header end `\r\n\r\n` or `\n\n`; case-insensitive `Content-Length`
  (default 0); body waited for until complete; status line
  `RTSP/1.0 <code> <text>` (splitn 3); headers lower-cased, values trimmed
  (HashMap — last wins); **sequence check**: if a `CSeq` header is present
  it must equal the expected counter, else fall back to `Request-Id`
  (:1893-1901); mismatch → `nvst-rtsp-sequence-mismatch` (TEARDOWN
  tolerates and SKIPS mismatched responses, :593-595).
* **Limits**: REQUEST_TIMEOUT **20 s** (:30), MAX_REQUEST_RESPONSE_BYTES
  **4 MiB** (:41), MAX_CONTROL_RESPONSE_BYTES 64 KiB (:42),
  MAX_STREAM_BITRATE_MBPS 200 (:43).

## 2. The method sequence (prepare_on_endpoint, :1052-1400)

Common headers on EVERY request (:1077-1081):
`X-GS-Version: 14.2`, `Host: {host[:port]}`, `x-nv-sessionid: {session_id}`.

1. **OPTIONS** target — must be 200 (`ensure_rtsp_ok` :1083-1084).
2. **DESCRIBE** target + `Accept: application/sdp` + `x-nv-abtesting: 2`
   (:1085-1089). From the response:
   * `Session:` header (value before ';') — REQUIRED (:1110-1118).
   * SDP `a=control:` of the **video** m-section (:1119-1124).
   * SDP attributes (`a=x-nv-...`): `general.pingVersion` (default 6),
     ICE creds `general.iceUserNameFragmentV2` (fallback
     `iceUsernameFragment`), `general.icePasswordV2` (fallback
     `iceUsernamePwd`), `general.dtlsFingerprintV2` (fallback
     `dtlsFingerprint`), `general.disablePlay` ("1" → SKIP PLAY),
     **`general.nativeRtcOnBundlePort` MUST be "1"** (else
     `nvst-legacy-transport-unsupported` — the retired multi-socket
     transport is refused, :1141-1147), `general.rtcpOnSctp`,
     `ri.hidDeviceMask`, `general.enableUnifiedSocket`,
     `general.serverBundlePort`, QoS offers + `qos_messages_size`
     (:1125-1140, :1152-1166).
   * **SRTP key material** (:1276-1281): `runtime.encryptionKey` (64 hex
     chars = **32 bytes / AES-256**) + `runtime.encryptionKeyId` (u32) from
     the DESCRIBE SDP; if ABSENT the CLIENT generates a random 32-byte key
     + random u32 id (`random_runtime_key` :1868-1885).
     **Salt = `format!("{key_id:024X}")`** — the key id as a 24-digit
     zero-padded uppercase hex string (12 bytes) (:1280).
   * **SRTP profile** `advertised_srtp_profile()` (:1282-1283, fn :1904+):
     first match of [AEAD_AES_128_GCM_8, AEAD_AES_256_GCM_8,
     AEAD_AES_128_GCM, AEAD_AES_256_GCM, AES_CM_128_HMAC_SHA1_32,
     AES_CM_128_HMAC_SHA1_80, AES_CM_256_HMAC_SHA1_32,
     AES_CM_256_HMAC_SHA1_80] scanned over SETUP Transport/*srtp*/*crypto*
     headers THEN the DESCRIBE SDP lines; **default AEAD_AES_256_GCM_8**
     (= kn_rx's `KN_RX_SRTP_GCM256` tag-8 — the round-XVI production
     expectation, now proven from the reference).
3. **SETUP** (video) `setup_video_with_retry()` (:312-505):
   * URI candidates (:508+): `official_video_setup_control(control)` =
     `control + "/0"` when control is exactly `streamid=video/...` with one
     '/'; then the raw control; then absolute `{target}/{control}` forms.
   * Transport forms (outer loop, :395-401): FIRST an **empty** Transport
     header, then `unicast;X-GS-ClientPort={p}-{p+1}` (p = local bundle
     port).
   * Extra headers: `Session:` + `x-nv-ping: {described pingVersion}`
     (:1152-1154).
   * Status handling (:449-497): 200 + valid peer → DONE; **200 WITHOUT a
     usable peer → STOP the sweep immediately** (live alliance rigs
     "poison" the session if further forms are tried — later rounds
     degrade to 400s and ANNOUNCE is then rejected; every observed working
     session peers on its first 200) → CloudMatch-bundle-peer fallback if
     available (WARN `video-peer-fallback source=cloudmatch-bundle`), else
     retry rounds **3 × 3 s** (:35-36) within the 20 s deadline;
     400/404/459/460/461 → next candidate; ANY OTHER status → fail.
   * Peer extraction `parse_video_peer()` (:1990+): from the response
     `Transport:` header, split on ';' and ',', `name=value`:
     **`source=` the peer IP**, **`X-GS-ServerPort=a[-b]`** the peer port
     (range span < MAX_NVST_VIDEO_PEER_PORTS; both must be present and
     non-zero).
   * SETUP response also yields `x-nv-ping` (overrides DESCRIBE's
     pingVersion) and `x-nv-ping-payload` (REQUIRED when ping version 6;
     default payload "PING") (:1245-1254, :1270-1272).
4. **ANNOUNCE** target (:768-784): headers + `Session:` +
   `Content-Type: application/sdp`; body = the client SDP (§3). Must be
   200. (`negotiated_announce()` post-processes the built SDP against the
   DESCRIBE body — e.g. the Data-Saver/dynamicStreamingMode decision,
   :1354-1360, :1720-1735.)
5. **PLAY** target (:790-800): headers + `Session:`; SKIPPED when
   `disablePlay=1` was described. Must be 200.
6. The WS connection STAYS OPEN after PLAY — `ActiveNvstRtspSession::spawn`
   runs the control ping loop on it (:810-813; stage-3 territory).
7. **TEARDOWN** on drop (:816-830): 1 s timeout, errors ignored, then WS
   close. `nvst-rtsp-sequence-mismatch` responses are skipped, not fatal.

`ensure_rtsp_ok` (:1878-1901): 200 ok; 503 → `nvst-service-unavailable`;
401/403 → `nvst-signaling-forbidden`; else `nvst-rtsp-failed`.

## 3. The ANNOUNCE client SDP (build_announce, :1520-1665)

Attribute-line SDP (no media codecs negotiated here beyond the video
m-line); CRLF-joined; `o=unknown 0 14 IN IPv4 127.0.0.1`,
`s=NVIDIA Streaming Client`, `t=0 0`, `m=video {videoPeerPort}`,
`c=IN IP4 0.0.0.0`, `i=DeviceString, DeviceName`, trailing empty line.
The full x-nv attribute set (values as coded, ours will differ only where
D14/D15 decide): viewport W/H (settings `resolution`, default 1920x1080 —
**ours: 1280x720 per D14**), maxCodecProfile/Level 3/51 (+ same for
H264), updateSplitEncodeStateDynamically:1, **packetSize:{video_packet_size}**
(measured-path or default — feeds kn_rx's `video_packet_size`!),
enableRtpNack:1, **rtpNackQueueLength:2048 / QueueMaxPackets:1024 /
MaxPacketCount:64** (matches kn_rx's NACK constants exactly),
framePacing.mode 2|1 + feedbackMode 0|1 (qos_timings_v5 switch),
maxNumReferenceFrames:0, **refPicInvalidation:1** (the RFI capability our
kn_rx_inval assumes!), clientSupportsIntraRefresh:1, prefilter params 0/4/0/0,
encoderCscMode:2, encoderHdrCscMode:4, mapRtpTimestampsToFrames:0,
maxFPS:{fps} (30..MAX, default 60 — **ours: 30 per D14**),
initial+initialPeakBitrateKbps (settings; **stage-5 cap 3-6 Mbps**),
vqos: bitStreamFormat 0=H264/1=H265/2=AV1, fec.enable:1,
fec.rateDropWindow:10, fec.minRequiredFecPackets:2, fec.repairPercent:20,
repairMin/Max 20/35, bllFec.enable:0, drc.enable:0,
dfc.adjustResAndFps:{dynamicStreamingMode-derived},
calculateAvgVideoStreamingBitrate:1, bw.maximum/minimumBitrateKbps
(min = min(bitrate,1000)), drc/resControl.bitrateIirFilterFactor:128,
dynamicStreamingMode:{negotiated} (RTSP-only — never sent to CloudMatch,
:1720-1725), packetPacing: version 3, mode 1, numGroups 5,
maxDelayUs 4000 (fps>=100) else 2000, minNumPacketsFrame 10,
minNumPacketsPerGroup 15, enableAccurateSleep 1, enableSmoothTransition 1,
allowFpsBasedToggle 1, ri: partialReliableThresholdMs:300,
timestampsEnabled:1, useMultipleGamepads:1,
usePartiallyReliableUdpChannel:0, enablePartiallyReliableTransferGamepad:255,
enablePartiallyReliableTransferHid:-1, bwe.useOwdCongestionControl:1,
**general.rtspWebSocketPerConnection:1**,
pingIntervalBeforeConnectionMs:20 / AfterConnectionMs:100 (nvst.rs:247
constants surface here), **runtime.audioSrtp:0, micSrtp:0, videoSrtp:1**,
**runtime.encryptionKey:{KEY_HEX} + encryptionKeyId:{id}** (the SRTP
material travels IN the ANNOUNCE SDP when we generated it; when DESCRIBE
provided it we echo it back), mouseCursorCapture:3, mimicRemoteCursor:0,
clientPorts.{video,audio,mic,control,bundle,session}:0 (all reserved via
useReserved:1 + fallbackDynamic:1), clientPorts.localAddress:{local ip},
clientBundlePort:{p}, nativeRtcOnBundlePort:1, rtcVideoOnNativeBundle:0,
rtcAudioOnNativeBundle:1, rtcDataChannelOnNativeBundle:1,
enableUnifiedSocket:{0|1}, rtcpOnSctp:{0|1},
iceUserNameFragmentV2/icePasswordV2/dtlsFingerprintV2 + the standard
ice-ufrag/ice-pwd/fingerprint:sha-256/setup:actpass/ice-options:trickle +
one host candidate `a=candidate:1 1 udp 2122260223 {addr} {port} typ host`
(stage-3 bundle; ANNOUNCE already carries them), optional mic lines, the
color lines (announce_color_lines), then the t/m/c/i tail.

## 4. The handoff (RTSP → media pipeline), :1287-1332

The `handoff` JSON is EXACTLY the bridge into what we already built:
`clientUdpPort`, `packetSize` → `kn_rx_video_config.video_packet_size`;
`mjolnirUdpPort` (only when !unifiedSocket), `unifiedSocket`,
`videoPeerIp/Port/PortEnd` → the UDP socket's peer (our `--decode`
listener becomes a CONNECTED socket to this peer);
`srtpAesKeyHex` (64 hex = AES-256), `srtpKeyId`, `srtpSaltHex` (24 hex),
`srtpProfile` (string → our `kn_rx_srtp_profile`), → `kn_rx_srtp_session`;
`pingPayload`/`pingVersion`, local/remote ICE + DTLS fields,
`rtcpOnSctp`, `hidDeviceMask`, `codec`, `audioTrack` (opus 48k 2ch
pt 111 mid 0), `timeoutMs`/`startupTimeoutMs` → `kn_rx_video_config`
timeouts. **Logging rule (:1327-1339): transport SHAPE only — never SDP,
runtime keys, or ICE credentials** (matches our §7 redaction discipline;
knlog's whitelist already treats "srtp" keys as secret).

## 5. CloudMatch (cloudmatch.rs)

* **Headers** `cloudmatch_headers()` (:2036-2059): `User-Agent:
  {bifrost_user_agent()}` (:3025), **`Authorization: GFNJWT {token}`**,
  **`Content-Type: text/plain`** (yes — with a JSON body; that is what the
  official client sends), `nv-client-id:
  ec7e38d4-03af-4b58-b131-cfb0495903ab` (LCARS_CLIENT_ID :16),
  `nv-client-streamer: NVIDIA-CLASSIC`, `nv-client-type: NATIVE`,
  `nv-client-version: 2.0.87.131` (GFN_CLIENT_VERSION :17),
  `nv-device-os/type/make/model`, `x-device-id: {device_id}`,
  `x-nv-client-identity: {user_agent}`.
* **Create**: `POST {base}v2/session` (+ language-preference query args,
  :361-371) with body `{"sessionRequestData": {...}}` — the full
  field-for-field builder is `build_create_body()` (:1643-1762). Notables:
  appId (i64), internalTitle, availableSupportedControllers [2],
  preferredController 2, networkTestSessionId (a prior probe session;
  `{"status":"not_requested"}` tolerated), clientIdentification
  **"GFN-PC"**, deviceHashId, clientVersion **"30.0"**, sdkVersion "2.0",
  streamerVersion "14", clientPlatformName, clientRequestMonitorSettings
  [{monitorId 0, positionX/Y 0, width/height, framesPerSecond, sdrHdrMode,
  displayData, hdr10PlusGamingData null, dpi 96}], useOps true,
  audioMode 2, **metaData [{wssignaling:"1"}, {SubSessionId: random uuid},
  {surroundAudioInfo:"2"}]**, sdrHdrMode, surroundAudioInfo 0,
  clientTimezoneOffset (ms), enhancedStreamMode 0, appLaunchMode,
  **secureRTSPSupported true**, accountLinked,
  enablePersistingInGameSettings, requestedAudioFormat 0, userAge 25,
  requestedStreamingFeatures {reflex, bitDepth, cloudGsync, enabledL4S,
  supportedHidDevices 0, profile 0, fallbackToLogicalResolution false,
  chromaFormat, prefilter* 0, hudStreamingMode 0, dlssOverrides,
  mouseMovementFlags 0, trueHdr false, hidDevices null} — codec/bitrate/
  vsync/QoS deliberately ABSENT (resolved locally + carried in ANNOUNCE),
  transport null. (allianceWebrtcCompatibility flips to the WebRTC
  variant — NOT our path.)
* **Readiness / polling**: session ready when `status ∈ {2,3}` AND
  `rtspsEndpoints` non-empty (:1524-1533); status 7 = terminated;
  resume semantics `status 2..=5 → resumable`, 1|6 → not, else fatal
  (:1505-1512) — that is R23's contract; queue position/seatSetupStep
  ride in session_info (`queuePosition`, `seatSetupStep`,
  docs/session-start-progress.md).
* **session_info (R22)** (:2065-2103): sessionId, transportMode
  (nvst|webrtc), reportedNativeTransport, subSessionId, appId, status,
  phase, termination, queuePosition, seatSetupStep, adState, zone,
  streamingBaseUrl, serverIp, signalingServer (`{ip}:443` when no port),
  **signalingUrl** (= rtspsEndpoints[0] `rtsps://`→`wss://`),
  serverLocation, gpuType, appLaunchMode,
  enablePersistingInGameSettings, connectionInfo, **rtspsEndpoints[]**,
  iceServers[], **mediaConnectionInfo** (the connection with usage 2|17),
  negotiatedStreamProfile (+hdr/codec sources), hdrDowngrade,
  catalogAppId, requestedStreamingFeatures, **finalizedStreamingFeatures**
  (server overrides), clientId, deviceId — the 31-field IPC contract,
  copied 1:1 per R22 with redaction at the logging boundary.
* **rtspsEndpoints extraction** (:956-1040): array from the payload,
  each entry validated (`invalid-rtsps-endpoint` on bad shapes),
  `missing-rtsps-endpoint` when absent while status says ready.

## 6. C-port plan (round XX) — staged, each stage gated

1. **`kn_ws`** — RFC 6455 CLIENT framing over the proven kn_net_mbedtls
   TLS-1.2 transport (handshake GET + Sec-WebSocket-Key/base64/SHA-1 —
   kn_sha1 exists; frame parse/serialize: FIN/opcode/mask, 7/16/64-bit
   lengths, client-side masking mandatory, text/binary/ping/pong/close,
   continuation). Bounded buffers (4 MiB per the RTSP limit), zero-alloc
   where possible. Sandbox-testable against a Python WS echo/server.
2. **`kn_rtsp`** — the request/response state machine EXACTLY per §1-2:
   serializer, the CRLF-padding-tolerant incremental parser, CSeq/
   Request-Id discipline, the sequence-mismatch TEARDOWN rule, status
   classes, the SETUP sweep with its stop-on-peerless-200 quirk, the
   DESCRIBE SDP attribute extractor (`a=x-nv-*` + m-section control),
   `parse_video_peer`, the SRTP key/salt/profile rules, and
   `build_announce` with OUR settings (D14: 720p30; bitrate cap stage 5).
   Pure module (injected transport fn) → KAT-able against transcribed
   reference shapes + a Python fake rig in the sandbox.
3. **`kn_cm`** — CloudMatch HTTP/1.1-over-TLS client (POST v2/session +
   GET polling + DELETE finish; headers per §5; the session_info parse
   into our own 31-field struct with redaction). Depends only on kn_net.
4. **Login/token** — the OAuth piece (gfn.rs / account_connections.rs —
   NOT yet read this round; that research opens round XX) + DPAPI token
   storage is stage-4 TUI territory; for stage-2 bring-up the token can
   be pasted once into a `--session-token` flag (never logged; length +
   presence only).
5. **Integration**: `knvstd --session ...` — POST v2/session → poll →
   WSS/RTSP → handoff → connect the UDP socket to videoPeer →
   `kn_rx_video_config` + SRTP session from §4 → the EXISTING --decode
   path (capture always on: R32's first real regression vector) → G2.

**Fake-rig test strategy (sandbox-first, the T13 pattern):** a Python
"rig" that speaks the §1-5 server side on loopback TLS (self-signed +
our verifier OFF is NOT allowed — instead the fake rig runs PLAIN ws://
+ http:// behind a kn_ws/kn_rtsp transport-injection seam, so the crypto
stays production and only the test skips it; the seam is test-only, never
a production code path). The rig replays DESCRIBE/SETUP/ANNOUNCE shapes
transcribed from the reference's own test fixtures (nvst_rtsp tests
include peerless-200, alliance-poison, CRLF-padding and endpoint-form
cases — our KAT battery mirrors them, the two-independent-implementations
rule).

## 7. NOT VERIFIED / open items (honest list)

* The OAuth login flow itself (gfn.rs) — NOT read yet; round XX opens
  with that research.
* `networkTestSessionId` acquisition (`acquire_network_test_session`) —
  the reference tolerates `not_requested`; whether CloudMatch requires a
  real network test for our account/zone is UNVERIFIED (measure at R7).
* `negotiated_announce()` post-processing details (:1735+) and
  `announce_color_lines` — read at implementation time.
* The control-ping loop on the kept-open WS (stage 3; `x-nv-ping`
  version 6 payload semantics partially read: :1245-1272).
* Whether the seat ever answers on a non-322 port from `rtspsEndpoints`
  (the code handles explicit ports; our E2E should too).
* Real-cloud shapes: EVERY SDP/header shape above comes from the
  reference and its tests, not from captured cloud traffic — the first
  real session is the true test (R32's rule applies to signaling too);
  `--capture` + the RTSP transcript log (KN_CH_RTSP exists in knlog
  already) make that session a permanent vector.

---

## 8. Implementation status (round XX — this document's §6 plan, stages 1-2 DONE)

**Delivered (sandbox-verified, awaiting target):**

* **`kn_ws`** (`streamer/{include/kn,src}/kn_ws.{h,c}`) — RFC 6455 client
  framing on an injected byte-stream seam (`kn_ws_io`): opening handshake
  (CSPRNG 16-byte key, base64, `Sec-WebSocket-Accept` = base64(SHA-1(key +
  GUID)) verified with the RFC golden vector), masked client frames with a
  fresh CSPRNG mask key per frame, 7/16/64-bit lengths, fragmentation
  join, ping→echoed-pong inline, pong skipped, close answered+surfaced,
  RSV/masked-server/oversize-control/orphan-continuation protocol errors,
  caller-owned reassembly buffer (never realloc → `KN_WS_ERR_BIG`).
  Mid-frame timeouts are RESUMABLE (frame-state kept in the struct — a
  self-review catch: the first draft restarted the header parse inside
  the payload). RNG: `rand_s()` on Win32 (**no new PE import** — msvcrt
  already in F1; bcrypt/advapi32 deliberately avoided), getrandom/urandom
  on POSIX; an RNG failure REFUSES the handshake/frame (never weak bytes).
  **`test_knws`: 84 checks** (RFC 4648 base64 goldens, the RFC 6455 §1.3
  accept golden, handshake request shape incl. the `x-nv-sessionid` +
  `content-length: 0` passthrough, 101/404/timeout/garbage/missing-accept
  classes, wire-exact frame bytes at the 125/126/65535/65536 boundaries,
  fragmentation, ping/pong/close, every protocol-error class, oversize,
  1-byte-per-recv chunking, resumable timeout). ASan+UBSan+LSan clean.
* **`kn_rtsp`** (`streamer/{include/kn,src}/kn_rtsp.{h,c}`) — the pure
  client: serializer (CSeq==Request-Id, exact reference shape), the
  incremental parser (leading-CRLF padding dropped even while incomplete,
  `\n\n` ends, LF-only lines, case-insensitive last-wins headers,
  Content-Length junk→0, drain-then-error on sequence mismatch, the
  Request-Id fallback, 4 MiB cap), `kn_rtsp_request` (deadline loop over
  the message seam, TEARDOWN skip flag), and the whole §2-3 toolset:
  `sdp_attribute` (with the `||`/`;;` quirks), `media_control`,
  `runtime_key` (+ the `{key_id:024X}` salt rule), the 8-profile scan
  (per-value priority order, SETUP headers before DESCRIBE SDP, default
  AEAD_AES_256_GCM_8), `parse_video_peer` (source=/X-GS-ServerPort=, span
  < 16 collapse), the URI ladder (`streamid=video/0` → `/0` suffix rule),
  `extract_describe` (Session/control requirements, V2-credential
  preference, pingVersion default 6, the boolean attributes, runtime key),
  the full SETUP sweep (stop-on-peerless-200 poison rule, bundle-peer
  fallback, 3×3 s retry rounds on the injected sleep, per-candidate
  remaining-deadline timeouts, the two Transport forms, 400/404/459/460/461
  continue class, fatal-status rule), `build_announce` (the complete §3
  attribute list, CRLF-joined, trailing empty line), and
  `negotiate_announce` (the 16-rule offered_in table with ranges,
  min>max fallback, repair clamp, bitrate ceiling/initial/peak clamps,
  line-substitution rewrite). **`test_knrtsp`: 127 checks** — every
  fixture transcribed from the reference's own tests (parser cases
  nvst_rtsp.rs:3443-3470, official-control :3470+, announce params
  :2923+) plus scripted-server E2E of the full
  OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY sequence with byte-exact request
  inspection, the TEARDOWN mismatch-skip, and every SETUP branch.
  ASan+UBSan+LSan clean; `-Wformat-truncation=2` clean at -O0..-O3 on
  both modules (R28 pre-check).
* Gates: native **12 suites, 1183 checks PASS**; cross mingw x64+i686
  `-Werror` PASS; CMake ctest **14/14** in both TLS backends; T10 POSIX
  dry-run ALL PASS **60** (target r15 expects **71** = r14's 69 + the two
  new suites).

**Documented deviations (bounded-C reality; all test-pinned):**

* D-w1 `kn_ws` messages are capped by the caller's buffer (4 MiB + slack
  for RTSP); the reference grows a String. Oversize is an honest error.
* D-w2 No auto-reconnect / no extension negotiation (the reference
  negotiates none either); retries belong to kn_rtsp / the session layer.
* D-r1 Response headers are bounded (24 × 48/512 bytes); extras are
  dropped, overlong values become honestly empty. Real NVST responses
  carry ≤ 12 short headers (DESCRIBE/SETUP fixtures prove the shape).
* D-r2 `ip_literal_ok` is a minimal IPv4/IPv6 shape check (the reference
  uses Rust's `IpAddr::parse`); the connect() in the socket layer is the
  real validator.
* D-r3 QoS-offer validation (`video_qos_offers.validate()`),
  `maxQosMessagesSize`, the control-ping loop and
  `framePacingFeedbackMode` are stage-3 work (they need the kept-open WS
  + SCTP context); `negotiate_announce` implements the value-resolution
  and rewrite, not the pacing-feedback derivation.
* D-r4 Per-request timeout inside the SETUP sweep temporarily narrows
  `timeout_ms` to the remaining deadline (same observable behaviour as
  the reference's `request_with_timeout(remaining)`).

**Remaining for the signaling path (round XXI+):** the production
transport (`kn_net_mbedtls` TLS-1.2 stream API → `kn_ws_io` adapter),
`kn_cm` (CloudMatch POST v2/session + polling + the 31-field
session_info), the OAuth research (gfn.rs — still unread), the Python
fake-rig E2E (real sockets: WS handshake + the full RTSP sequence against
`knvstd`), and the `knvstd --session` integration that feeds the handoff
(§4) into `kn_rx_video_config` + `kn_rx_srtp_session` + the connected UDP
socket → the existing target-verified `--decode` path.

## 9. Round XXI — the production path is BUILT (status + deviations)

Everything in §6's plan is now implemented and sandbox-proven EXCEPT the
OAuth login itself (stage 4 per docs/AUTH.md; bring-up uses a pasted
token) and the real-cloud measurement (R7):

* **`kn_net_tls_stream_*`** (kn_net.h / kn_net_mbedtls.c): the persistent
  verified TLS-1.2 stream — same policy object as the T12-proven probes
  (VERIFY_REQUIRED, SNI/hostname, Windows ROOT stores read-only,
  min=max=TLS 1.2, PSA init), port-parameterized TCP open (RTSPS lives on
  322, not 443), per-call SO_RCVTIMEO, recv contract >0/0-timeout/-1-err/
  -2-EOF. Win32+mbedTLS ONLY; every other build gets honest stubs
  (`available()==0`) and `--session` refuses loudly — NO plaintext
  fallback exists. The Win32 body compiles first on the target (T10 r16);
  it mirrors kn_tls_session line-for-line, so the risk is bounded.
* **`kn_json`** (1024-node pool, depth 32, zero-alloc, raw-span strings
  with on-demand unescape incl. surrogate pairs, int64-exact re-parse) +
  **`kn_http`** (HTTP/1.1 one-shot: CL / chunked / read-to-EOF framing,
  resumable chunk parser — the 1-byte-delivery KAT caught a stranded
  cursor in the first draft) + **`kn_cm`** (create/poll/extract per §5;
  the rtspsEndpoints DERIVATION from connectionInfo, media/bundle usage
  2|17, queue-position 4-source fallback, ready/resumable rules; the
  POST body field-for-field with build_create_body). **`test_kncm`: 91
  checks** — fixtures follow the reference's own test shapes (:4073-4085).
* **`kn_sock_udp_connect`**: the media socket CONNECTS to the SETUP peer
  (kernel source filter — stray traffic never reaches kn_rx) and refreshes
  local_addr via getsockname, so the ANNOUNCE carries the REAL route
  source address like the reference's reserved bundle socket does.
  `test_knsock` +12 checks (stray-datagram rejection, re-connect).
* **`tools/fake_rig.py` + `tools/sandbox_session_e2e.c`**: the two-
  implementations rule at the signaling layer — an independent Python rig
  (HTTP + WS server with its own RFC 6455 framing) against the C client
  over REAL loopback sockets: create → poll → WS handshake (x-nv-
  sessionid) → OPTIONS → DESCRIBE → SETUP → ANNOUNCE(negotiated) → PLAY →
  handoff (salt/profile/SRTP-session or honest NO_CIPHER) → connected UDP
  peer. **36 checks PASS.** The rig also asserts the server side (the
  ANNOUNCE must echo the runtime key; exactly 1 PLAY).
* **`knvstd --session`** (the §6 stage-5 glue, TLS-only): CloudMatch
  create + 2 s poll loop with queue JSONL events (status/phase/queue_
  position/seat — the TUI's stage-4 feed) → signaling TLS + WS + the full
  RTSP sequence → handoff → kn_rx SRTP session + connected UDP → the
  round-XVII media loop (SAME dec_* machinery: capacity-2 queue, overflow
  policy, JSONL contract — so --verify/T13-style checks apply to a real
  session's stdout unchanged). Token logged as LENGTH ONLY; key material
  never logged (lengths/handoff shapes only, §4's rule). TEARDOWN on exit
  (1 s, mismatch-skip). The media loop deliberately DUPLICATES run_decode
  instead of refactoring the T13-verified function (churn discipline);
  unification after --session has target evidence.

**New deviations (round XXI):**
* **D-s1 (UNVERIFIED, measured at R7)**: the ANNOUNCE carries locally
  generated ICE ufrag/pwd and a RANDOM 32-byte placeholder DTLS
  fingerprint — the real bundle identity needs the stage-3 DTLS
  certificate (R3). Mjolnir video (SRTP/UDP) is independent of bundle
  DTLS, so video should flow; the rig's bundle side failing DTLS is
  EXPECTED until stage 3.
* **D-cm1**: `networkTestSessionId: null` (the reference tolerates
  "not_requested"; whether the cloud wants a real network test for this
  account/zone is an R7 measurement).
* **D-cm2**: `SubSessionId` = the device id (the reference sends a fresh
  random UUID per launch; UUID generation lands with the login work).
* **D-cm3**: the R22 31-field session context is a SUBSET here (the
  stage-2 media fields); the full contract is stage-4 TUI IPC.

**Sandbox gates (round XXI):** native **13 suites, 1309 checks PASS**;
cross x64+i686 `-Werror` PASS; knvstd (incl. --session) clean under
`-Wformat-truncation=2` at -O1 host / -O2 mingw; CMake 0 warnings +
ctest **15/15** both backends; T13 ALL PASS **51 (FFmpeg) / 51 (ASan+
UBSan+LSan) / 63 (mbedTLS+gcm256)**; sandbox_session_e2e **36/36**;
T10 POSIX dry-run ALL PASS **61**.
**Target expectations:** T10 r16 → ALL PASS **72** (r15's 71 +
test_kncm; registry stays 14 — no new self-test this round; the Win32
TLS-stream implementation compiles for the first time). T13 r3 → ALL
PASS **61** unchanged. `--session` needs a real token + appId: that is
the R7 measurement round (docs/AUTH.md §3 explains how to get the token
manually until stage 4).

## 10. Round XXIII — R7 runs 1-4 (the FIRST real CloudMatch traffic) + the control-host fix

**What was measured (2026-10-25, target build = T10 r17 ALL PASS 72; real
GFNJWT pasted from the browser + appId 107057914, Win7 target):** four
consecutive live `--session` runs. EVERY run: `cm_created` OK — a real
sessionId, status 1, and a PLAUSIBLE LIVE queuePosition (167/166/181/178
while the browser saw 161 in the same minutes). Then the position FROZE:
run 2 repeated `queue_position:163` for ~40 polls, run 3 `176` for ~30,
run 4 `178` for 4; every run ended in
`{"error":"not_ready","stop_reason":"cm_poll_failed"}` after a variable
number of polls (2-43). The user's parallel DevTools capture of the
official WebRTC client: it polls
`https://np-waw-01.cloudmatchbeta.nvidiagrid.net/v2/session/{id}` — the
REGIONAL node — every ~4.1 s (Δ 4.15/4.12/4.42/3.21 s), and its
queuePosition MOVES (161→151 in 1 s, →147 in 4 s, →135 in 10 s).

**Root cause (ONE contract violation):** `run_session` polled the CREATE
host for the whole queue wait — the default `prod.cloudmatchbeta.
nvidiagrid.net` (the reference's own DEFAULT_STREAMING_URL, gfn.rs:38 —
correct for the create POST). The reference NEVER polls the create front:
`session_info` learns `control_base` = `https://{sessionControlInfo.ip}`
(cloudmatch.rs:1966-1984 — gated on port==443-or-absent + the
trusted-base rules :2400-2456), `create` stores it as the allocation's
base (:401-414), attach re-learns it (:1149-1152), and EVERY `poll` goes
ONLY there (:652-656 → `get_session` :1356 joins `v2/session/{id}`). The
global front evidently answers with stale snapshots of the session record
(frozen `seatSetupInfo`) and intermittent non-200s — exactly the observed
freeze-then-`cm_poll_failed`. The user's capture response body confirms
the nomination: `sessionControlInfo:{ip:"np-waw-01.cloudmatchbeta.
nvidiagrid.net", resourcePath:"/v2/session/{id}", usage:10, port:443}`.

**Fix (round XXIII, all reference-mirrored):**
* `kn_cm_session` learns `control_host/control_port/have_control`
  (sessionControlInfo) + `req_status_code/req_status_desc` (requestStatus,
  filled even when the session extraction fails, so a rejected payload
  still says WHY — e.g. statusCode 11 SESSION_LIMIT, :2354+).
* NEW `kn_cm_control_base` — the trust gate: the C mirror of
  `trusted_learned_server_base` + `public_session_host/_ipv4`
  (:2400-2456). https-only, port 443, no userinfo/query/fragment,
  private/loopback/link-local/CGNAT/multicast/reserved/.local all
  REJECTED (a response can never redirect polling into a private
  address); documented IPv6 simplifications (prefix-class checks instead
  of a full IpAddr decode). 0 → the caller falls back to the create host
  (:1984 `unwrap_or(base)` semantics — also what keeps the loopback
  fake-rig E2E unchanged).
* Phase A polls the learned node: new `cm_control` JSONL event
  (`poll_host`/`port`/`source`) is the R7 evidence line.
* Cadence 2 s → **4 s** (`SESS_POLL_MS`, the capture's ~4.1 s).
* Per-round transient retry: 3 attempts, 250/750 ms, on transport errors
  and 408/425/429/500/502/503/504 (`get_session` :1369-1394); 404 →
  `session_not_found` (:664-671); up to 4 consecutive failed rounds
  tolerated (each one a `cm_poll_error` event carrying rc + http_status +
  the cloud's requestStatus) before `cm_poll_failed`; the `not_ready`
  line now carries `rc`/`http_status`.
* Failed rounds never clobber the live session state (poll-into-temp +
  commit — `kn_cm_extract_session` memsets its output BEFORE parsing, so
  one bad payload used to be able to wipe `session_id` mid-wait).
* NEW `kn_cm_cancel` = DELETE `v2/session/{id}` (the reference's `stop_at`
  :789-800 / `validate_delete_response` :2728: 2xx and 404 = gone) +
  best-effort cancel on EVERY not_ready exit except 404 (`cm_cancel`
  JSONL event). Runs 1-4 each ORPHANED a queued session — with luck they
  aged out server-side, but a live one can block the next create with
  SESSION_LIMIT; from now on an aborted wait cleans up after itself.
* `--session-queue-timeout` default 180 → **600** s (a real 160+-deep
  queue deserves more than 3 minutes; cancel makes the longer wait safe).

**D-cm1/D-cm2 — measured OK at the create stage:** all four runs created
real queued sessions with `networkTestSessionId:null` (D-cm1) and
`SubSessionId = device id` (D-cm2). The cloud accepted both for THIS
account/zone/create; whether they matter at seat assignment/streaming
stays open until a run reaches ready.

**Not covered yet (honest list):** post-ready aborts (phases B-E) still
don't cancel the session — churn discipline, next round; the poll loop's
new branches are sandbox-proven only up to the fallback path (the rig's
`sessionControlInfo.ip` is 127.0.0.1 → trust gate → create-host
fallback; the ACCEPT path is pinned by the test_kncm capture-fixture
battery — a public-host loopback rig is impossible without DNS games).
**Next measurement = R7 run 5:** same command; expect `cm_control` with
`source:"sessionControlInfo"` and a MOVING queue_position.

## 11. Round XXIV — R7 run 5 (FIRST READY SESSION + first RTSPS traffic) + the DESCRIBE fix

**What was measured (2026-10-25, the round-XXIII tree on the target, same
command as runs 1-4):** the control-host fix WORKED end-to-end through
the queue:
* `cm_control: poll_host=np-waw-01.cloudmatchbeta.nvidiagrid.net,
  source=sessionControlInfo` — the poll switch fired on the real cloud.
* The session PROGRESSED: `queue_position:81/seat_step:1` for ~29 polls
  (~2 min), then `seat_step:3` with queuePosition GONE (have_queue:0),
  then `status:2 ready` — `rtsps://80-84-174-149.cloudmatchbeta.
  nvidiagrid.net:322`. (`media_peer:":0"` is honest: the ready response
  carried no usage-2/17 connection — the video peer comes from the SETUP
  sweep, as in the reference.)
* **Queue-position observation (NOT a defect, recorded):** on the NATIVE
  session the position did not tick down per poll — the server published
  step transitions instead (1 → 3 → ready). The user's moving browser
  numbers (161→135) were a WebRTC-transport session; the two queue
  feeds evidently differ. Client-side correctness is proven by the
  state transitions + ready arriving. Keep observing at run 6.
* Phase B: the RTSPS/WSS transport WORKS against a real GFN rig —
  `OPTIONS rc:0 status:200` over TLS:322 + the WS handshake with
  x-nv-sessionid. Then **`DESCRIBE rc:-2 status:0`** — a transport-level
  failure with no response parsed (KN_RTSP_ERR_IO), the run's new
  frontier.

**Diagnosis (three real deviations from nvst_rtsp.rs, all fixed; without
wire logs the first two are indistinguishable as THE cause of run 5 —
both produce exactly rc=-2/status=0 at the first big response):**
1. **The 8 KiB per-message scratch (the prime suspect).**
   `kn_rtsp_request` read each WS message into `char scratch[8192]`;
   the shim contract makes a larger message an IO error. A real DESCRIBE
   SDP (QoS offers, codec lines, ICE/DTLS attributes — the user's own
   capture shows the rig's style) routinely exceeds 8 KiB, while OPTIONS
   responses are a few hundred bytes: precisely the observed
   OPTIONS-200-then-DESCRIBE-IO signature. The reference has NO
   per-message cap below the 4 MiB total: tungstenite reassembles whole
   messages (`max_message_size = MAX_REQUEST_RESPONSE_BYTES`,
   :559-566) and appends text AND binary to one buffer (:614-623).
   **Fix:** kn_rtsp_request reads directly into the accumulation
   buffer's tail (cap = the full remaining 4 MiB); the shim contract is
   unchanged.
2. **BINARY frames were refused.** The reference feeds `Message::Binary`
   into the RTSP buffer (:621); both C adapters (knvstd +
   sandbox_session_e2e) rejected `op != TEXT` with ERR_IO. **Fix:** both
   accept TEXT|BINARY (kn_ws already reassembled and counted binary
   messages — only the adapters gated).
3. **Host header without the port.** The reference's Host = the target
   minus the scheme — `80-84-...:322` WITH the port (:1070-1076); run 5
   sent the bare host (the rig tolerated it for OPTIONS, but fidelity is
   the R32 rule). **Fix:** knvstd + E2E build the authority from the
   rtsps endpoint (scheme stripped, `/?#` cut).

**Instrumentation (so run 6 measures even if it fails elsewhere):** a
failed rtsp step now emits an `rtsp_detail` companion line: kn_rtsp's
`err[160]` (which stage failed — send/recv/parse/timeout/limits) plus
the WS-layer verdict (`ws_err` = KN_WS_ERR_*, `ws_close` = the peer's
close code, `ws_text`/`ws_bin` frame counters). A server-side close now
shows up as `ws_err:-7, ws_close:<code>` instead of a bare -2.

**Sandbox proof of the fix shapes:** test_knrtsp +5 checks (a >19 KiB
single-message DESCRIBE parses — the old scratch died here; the same
response split across two messages accumulates); the fake rig now
delivers its DESCRIBE SDP padded to ~11 KiB as TWO binary messages, and
sandbox_session_e2e stays **36/36** over real loopback sockets (binary
acceptance + across-message accumulation + negotiation on a padded SDP
all exercised end-to-end).

**Next measurement = R7 run 6:** same command. Expect DESCRIBE 200 (+
`describe-shape`-class evidence via our extract), then the SETUP sweep
(the reference's peerless-200 poison rule and the bundle fallback are
already implemented — docs/RTSP.md §2), ANNOUNCE with the D-s1
placeholder ICE/DTLS identity (its first real verdict), PLAY, and the
media loop. If ANY step fails, the rtsp_detail line names the stage.

## 12. Round XXV — R7 run 6 (FIRST READY→RTSP CHAIN: DESCRIBE/SETUP/ANNOUNCE 200 on the real rig; PLAY 455 root-caused to the missing pre-PLAY hole-punch burst)

**What was measured (2026-10-25, the round-XXIV tree on the target):**
run 6 walked the whole signaling chain against the live cloud:
`cm_control` switched to np-waw-01 (source sessionControlInfo), the
queue ran 63→65 (static again — see below) → `seat_step:3` → `status:2
ready` → `rtsps://80-84-170-155.cloudmatchbeta.nvidiagrid.net:322` →
WS handshake → **OPTIONS 200 → DESCRIBE 200 (the round-XXIV scratch fix
confirmed live) → SETUP 200 (video peer 80.84.170.155:5004, first
sweep round, no bundle fallback) → ANNOUNCE 200 (4164-byte negotiated
SDP)** → **PLAY 455** ("Method Not Valid in This State", rc:0 — an
honest RTSP rejection, `rtsp_detail` showed a healthy WS layer:
ws_err:0, ws_close:-1, 5 text frames, 0 binary). The user then saw the
session as RESUMABLE in the GFN web UI ("Endfield [WZNÓW]") and played
it in the browser — the session was real and alive; only OUR PLAY was
refused. Side evidence: R23 RESUME is a real, working cloud path.
**Queue-position note (run 6 = run 5 pattern):** the native queue again
published step transitions rather than per-poll position ticks (63→65
then static, then seat_step 3 → ready). Two runs now agree: this is the
server's native-queue behaviour, not a client defect.

**Root cause (PLAY 455):** the reference does NOT send PLAY straight
after ANNOUNCE. Its engine: `prepare_on_endpoint` (OPTIONS/DESCRIBE/
SETUP) → `prepared.announce()` (lib.rs:894) → **spawn the media
receivers (:937 bundle / :990 mjolnir)** → only then `prepared.finish()`
= PLAY (:1117). The spawned bundle receiver fires its hole-punch burst
IMMEDIATELY (`last_hole_punch` pre-armed at spawn, nvst.rs:7438): every
20 ms (PING_INTERVAL_BEFORE_CONNECTION :242) it sends **3× STUN binding
request + 1× NATT hole-punch ping** (:7842-7900) to `bundle_peer()` =
the CloudMatch bundle peer else the SETUP video peer (:1880), and
answers the rig's binding requests (:5151-5172). Our client sent PLAY
with zero UDP egress — the rig's state machine had not seen the
client's transport come alive → 455.

**Fix (round XXV) — the sequential pre-PLAY pump (deviation D-s2):**
* `kn_sock` grew the STUN toolkit: `kn_sock_crc32` (IEEE reflected),
  `kn_sock_stun_build` (byte-exact port of build_authenticated_stun_
  packet :4977-5012 — USERNAME/XOR-MAPPED + MESSAGE-INTEGRITY HMAC-SHA1
  with the RFC 5389 §10.5 length adjustment + FINGERPRINT CRC^0x5354
  554E), `kn_sock_stun_xor_addr_v4`, `kn_sock_stun_inspect` (the
  handle_stun_datagram gate: magic, exact length, MANDATORY valid
  fingerprint :5094-5104/:5138) and `kn_sock_stun_check_integrity`
  (:139-160, constant-time compare). KAT'd against goldens computed by
  an independent Python implementation (hmac/hashlib/zlib) + RFC 2202.
* `kn_rtsp` grew the credential ladder (fixtures transcribed from the
  reference's own tests :3860-3920): `kn_rtsp_increment_hex` (:2305),
  `kn_rtsp_bundle_natt_username` ("{V1-or-V2 ufrag}{serverBundlePort}"
  :1327-1345), `kn_rtsp_resolve_remote_ufrag` (the three-step ladder
  :2329-2352); `kn_rtsp_describe` extracts `ufrag_v1` +
  `server_bundle_port`.
* **A pre-existing kn_rtsp bug the ladder fixtures caught:**
  `kn_rtsp_sdp_attribute` returned the FIRST match across ";;" segments;
  the reference flattens ALL segments and keeps the LAST (.last()) — a
  later segment OVERRIDES an earlier one (its test :3861-62 pins
  plain-47998-then-;;47999 → 47999). Fixed + pinned by a dedicated
  check. (media_control legitimately stays first-segment-only —
  `.split(";;").next()`, :1494-1501.)
* `knvstd` phase C/D: after ANNOUNCE — `ice_creds` evidence line
  (LENGTHS only, §7), the credential gate (pingVersion==6 or a remote
  fingerprint, nvst.rs:1729; ping-6-without-SETUP-payload = the
  reference's missing-ice-credentials error :1248-1253 → deviation
  **D-s3**: we skip the pump honestly and still try PLAY), the
  destination switch (re-connect when the bundle peer differs from the
  video peer, restore after), the 400 ms pump (20 ms bursts, answers
  the rig's requests, counts its successes), then PLAY with **455
  retry** (up to 3 attempts, a 600 ms pump before each retry) and the
  `ice_preplay` counters line. The PLAY event carries `attempt`.
* The rig + E2E now mirror the run-6 state machine: the rig's media
  socket runs an INDEPENDENT Python STUN validator/responder (username
  "a1b24801:{announced ufrag}" / "PING:{...}", MI keyed RIGPASSWORD),
  and the rig GATES PLAY: the first PLAY is always 455, later ones need
  ≥1 validated hole-punch request. sandbox_session_e2e pumps, asserts
  the ladder ("a1b24800"→"a1b24801"), the 455-then-200 sequence, and
  the rig's counters (`ice_req:12, ice_resp:12, play_455:1, plays:1`):
  **36 → 43 checks** over real loopback sockets.

**Not covered yet (honest):** whether the REAL rig's 455 gate is
exactly "saw a hole-punch packet" vs "saw a VALIDATED ICE binding" vs
something DTLS-adjacent is run-7's measurement — the pump sends both
the ICE burst (USERNAME remote:local, keyed with the DESCRIBE password)
and the NATT ping (USERNAME payload:local), the two shapes the
reference sends pre-PLAY. If run 7 still 455s, `ice_preplay` (did the
rig answer? `ok_in`) + `ice_creds` (which ladder leg fired) localize
it. The post-PLAY STUN/RTCP/SCTP plane and the real DTLS identity stay
stage 3 (R3); phases B-E aborts still leave the session resumable
(reference parity: it retains allocations for RESUME — the user's
browser-resume of run 6 is the proof that this is the right default).

**Next measurement = R7 run 7:** expect `ice_creds enabled:1`,
`ice_preplay` with stun_sent/natt_sent > 0 (ok_in > 0 if the rig
answers), PLAY attempt 1 possibly 455 → attempt 2 200, then the handoff
line + the media loop (first real Mjolnir SRTP → kn_rx → AUs — R32's
final stage-2 gate).

## 13. Round XXVI — R7 run 7 (the ~2-minute seat-transition window: a 503 storm, and why the wait must OUTLIVE it)

**What was measured (2026-10-25, the round-XXV tree; two attempts, real
token + appId 107057914):**
* Attempt 1: cm_control → np-waw-01; **29 polls (~2 min) of static
  `queue_position:53, seat_step:1`** — the SAME ~29-30-poll step-1
  duration runs 5 and 6 showed before transitioning to `seat_step:3` —
  then, instead of the transition: **five consecutive failed rounds,
  every HTTP attempt `503`** (rc:0 — real parsed responses, 15 HTTP
  attempts over ~30 s incl. the in-round 250/750 ms retries) →
  `cm_poll_failed` → **`cm_cancel` http_status:200 — a DELETE to the
  SAME host, in the SAME window, answered 200 while every GET 503'd.**
  The user confirmed the session vanished from the GFN web UI (the
  round-XXIII cancel is target-proven), and the immediate re-create did
  NOT hit SESSION_LIMIT.
* Attempt 2 (right after): the identical shape — static step-1 at
  position 57, then the same 5×503 → cancel 200.

**Analysis (three runs now agree on the clock):** runs 5, 6 and 7 ALL
spent ~29-30 polls (~2 min) in `seat_step:1`. Runs 5/6 transitioned at
that mark (`seat_step:3` → ready within ~20 s). Run 7 hit 503s at that
exact mark, twice, on two different sessions. The consistent reading:
**the ~2-minute mark is when the seat-assignment machinery acts on the
record**; when a seat is available the record transitions (runs 5/6);
when the assignment backend is saturated/timing out (Sunday-evening WAW
pool), the GET path answers 503 — a classic proxy timeout for an
overloaded upstream — while the session STORE path (DELETE) stays
healthy. The session is very likely still alive and resumable through
the window; we simply stopped asking after ~30 s. Alternative readings
(a node-wide GET-path outage; an edge/WAF shed) cannot be fully
excluded from here — which is exactly why the fix also captures the
503 body excerpt: run 8 will NAME the source (nginx/ALB/JSON error).

**The contract check:** the reference NEVER gives up on a poll error —
`get_session` retries 408/425/429/5xx three times per call (:1369-1394)
and returns Err upward, but the UI's poll cadence just continues (only
404 clears the session, :664-671); the official browser client likewise
keeps polling through spinner states. Our round-XXIII "5 consecutive
bad rounds → dead" was a bring-up bound that run 7 proved too eager: it
killed the wait inside the very window the transition needs.

**Fix (round XXVI — the failure-class policy, deviation D-s4):**
* **Transient HTTP statuses (408/425/429/500/502/503/504) are OUTLIVED**
  — they no longer count toward any give-up bound; only `queue_timeout`
  (default 600 s = 10 min, flag-extendable) or Ctrl-C ends such a wait
  (both still cancel the session — proven at run 7).
* **Transport failures** (no HTTP response at all: TLS/DNS/socket) stay
  bounded: 10 consecutive rounds → `cm_poll_failed`.
* **Hard HTTP rejections** (any other non-200: 401/403/409… or a 200
  with requestStatus.statusCode != 1) stay bounded: 5 consecutive →
  `cm_poll_rejected` (a NEW stop_reason — an auth/entitlement death is
  not a transient storm and must not burn 10 minutes).
* **Gentle backoff while failing (D-s4):** the poll gap grows 4 s → 6 s
  → 9 s → 13.5 s → capped 20 s (`sess_poll_gap`), reset on any success —
  outlive the window with progressively lighter traffic instead of a
  fixed-rate hammer.
* **Evidence upgrade:** `cm_poll_error` now carries `"body"` (a bounded,
  escaped excerpt of THIS round's response body — 503 bodies usually
  name their sender), `"class"` (transport/transient/hard) and
  `"gap_ms"` (the next poll gap). `cm_json` is cleared per round so the
  excerpt can never be a stale body.

**Sandbox proof:** the policy is knvstd glue (Win32-TLS-gated), so the
sandbox evidence is: native 13 suites PASS, cross PASS, paranoid
-Wformat-truncation=2 clean, E2E 43/43 (phase A unchanged there), ctest
15/15 ×2, T10 dry 62. The policy itself gets its target proof at run 8.

**Next measurement = R7 run 8:** expect the ~2-min static step-1, then
EITHER the step-3 transition → ready → PLAY (with the round-XXV pump:
`ice_creds`/`ice_preplay`, attempt 1 maybe 455 → 200) → the first real
media, OR a 503 window with `class:"transient"`, growing `gap_ms` and a
`body` excerpt that names the source — ridden out up to 10 minutes.
If the storm outlives queue_timeout: `stop_reason:"queue_timeout"` +
`cm_cancel`, and the `body` lines tell us whether to extend the budget
or treat long 503s as a dead record.

## 14. Round XXVII — R7 run 8 (the 503 body SPEAKS: an app-level session skeleton with sessionControlInfo.ip:null) + the T10-r20 warning fix + control-node re-learning

**What was measured (2026-10-26, the round-XXVI tree):** the new policy
performed exactly as designed — 12 polls of static `queue_position:80,
seat_step:1`, then the 503 window opened and the client RODE IT: ten
`class:"transient"` rounds with the D-s4 backoff visible in the evidence
(`gap_ms` 6000→9000→13500→20000 cap), until the user pressed Ctrl-C at
~5 minutes (`stop_reason:"interrupted"`) → `cm_cancel` 200. (The
session then correctly did NOT appear in the web UI — a queue-stage
session that was DELETEd is gone; run 6's browser-resume worked because
that session had reached READY and phase-B-E aborts deliberately do not
cancel. For keep-it-alive experiments there is now `--session-keep`.)

**The 503 body (the round-XXVI excerpt instrumentation paid off):**
```
{"session":{"sessionAds":null,"sessionControlInfo":{"protocol":0,
 "appLevelProtocol":5,"ip":null,"resourcePath":"/v2/session/479ecbcd-…
```
This is **CloudMatch's own JSON** (not an nginx/ALB page): a session
SKELETON whose `sessionControlInfo.ip` is **null** while `resourcePath`
still echoes the session id. Reading: the front knows the record but
cannot presently name the node/state that owns it — the seat-transition
window of §13 from the inside. The record is alive (DELETE answers 200
in the same window). The deep fields (`status`, `errorCode`,
`requestStatus.*`) sit PAST the 160-char excerpt — so this round parses
them.

**Changes (round XXVII):**
* **The T10-r20 catch fixed + gated permanently:** `consec_transient`
  was write-only (the transient class has no bound by design) — target
  GCC 16.2 flags `-Wunused-but-set-variable`, which sandbox GCC 12 does
  NOT include in `-Wall` (the fifth target-catch incident, R28). The
  counter is gone and **both gate scripts now run a paranoid knvstd leg**
  (`-Werror -Wunused-but-set-variable -Wformat-truncation=2`, host +
  both cross arches) — the class can never reach the target again.
* **`kn_cm_session` grows `error_code` (session.errorCode) and
  `unified_error` (requestStatus.unifiedErrorCode, :2343); status/phase/
  errorCode are now filled BEFORE the hard sessionId gate**, so a
  rejected payload (the run-8 skeleton) is fully diagnosable. The
  `cm_poll_error` line gained `unified_error`, `sess_status`,
  `err_code`, and the body excerpt grew 160→240 chars. Run 9's 503s
  will carry the cloud's own numbers.
* **Control-node re-learning (reference fidelity, was a real gap):**
  the reference re-derives `streamingBaseUrl` from EVERY successful
  poll's `sessionControlInfo` and `store_active` saves it as the next
  poll's `control_base` (:1333-1336 + :1966-1984) — a session that
  MIGRATES nodes mid-queue is followed. knvstd pinned the poll host at
  create. Now every successful poll re-runs `kn_cm_control_base` (the
  round-XXIII trust gate; `ip:null` skeletons simply keep the current
  host) and a change emits a fresh `cm_control` event with
  `source:"relearned"`.
* **`--session-keep`**: skips the phase-A cancel on abort (emits
  `cm_cancel {"skipped":"session-keep"}`), leaving the queued session
  alive for the GFN web UI's RESUME / R23 experiments. Default unchanged
  (cancel — the SESSION_LIMIT hygiene the run-7 pair proved).

**Sandbox gates:** native 13 suites PASS (test_kncm 147→**148**: the
run-8 skeleton fixture — rejected but status/errorCode/unified/desc all
surface, ip:null → no control host; unifiedErrorCode added to the
SESSION_LIMIT fixture; errorCode pinned on the run-5 capture fixture) ·
cross PASS incl. the new paranoid legs (host+x64+i686 .o) · E2E 43/43 ·
ctest 15/15 ×2, 0 warnings · T10 dry 62.

**Next measurement = R7 run 9:** same command (+ optionally
`--session-queue-timeout=1200` in peak hours, + `--session-keep` if the
session should stay resumable). Watch for: the ~2-min mark → EITHER
`seat_step:3`→ready→the round-XXV pump's first live try
(`ice_creds`/`ice_preplay`/PLAY attempts) → first media, OR the 503
window now speaking in parsed fields (`sess_status`, `err_code`,
`unified_error`, `cloud_status`, `desc`) — and possibly a
`cm_control source:"relearned"` line if the session migrates. If the
window outlives the budget, those five numbers are the basis for the
next decision (extend the budget, treat long 503s as a dead record, or
re-create).

## 15. Round XXVIII — R7 run 9 (T10 r21 ALL PASS 72; re-learning LIVE: the control node migrates constantly; ICE fully bidirectional; PLAY still 455 → the pump goes CONCURRENT + one-shot PLAY + the Mjolnir keepalive)

**What was measured (2026-10-26, the round-XXVII tree; T10 r21 confirmed
ALL PASS 72, zero warnings — the paranoid legs work):**
* **The control-node re-learning fired ~25 times in one queue wait:**
  np-waw-01 → np-ams-06 → np-sth-04 → np-lon-06 → np-ams-07 → np-lon-07
  → np-par-05 → np-frk-07 … nearly every poll response nominated a
  DIFFERENT regional front, and every nominated node answered. This is
  the reference's own behaviour (store_active re-derives control_base
  from EVERY response, :1333-1336) — and it retro-explains runs 7/8:
  pinned to np-waw-01, we polled a node the session had already drifted
  from; its 503+`ip:null` skeleton was "this record is not mine to
  serve right now". **Run 9 saw ZERO 503s across ~28 polls and walked
  step 1 → 3 → ready cleanly.**
* Queue position semantics: `100` repeated on EVERY node (a saturation/
  display cap — the real depth was ≥100), with one poll catching `23`
  (a fresher per-node view mid-migration). Recorded, not a defect; the
  transitions are what matter.
* Ready → rtsps://77-111-249-145…:322 → OPTIONS/DESCRIBE/SETUP/
  ANNOUNCE 200 (SETUP peer 77.111.249.145:5004, first round).
* **ice_creds enabled:1** with all ladder legs real: ping_ver 6,
  remote_ufrag_len 13, bundle_user_len 13 (so the REAL DESCRIBE carries
  general.iceUsernameFragment + serverBundlePort — the ladder took the
  pingVersion-6 bundle-username leg, exactly the reference's own test
  shape "a1b248001"), setup_payload_len 13, remote_pw 1, remote_fp 1.
* **ice_preplay: stun_sent 147, natt_sent 49, req_in 99, answered 99,
  ok_in 327, invalid_in 0, send_errors 0, pump_ms 1005** — a fully
  bidirectional, integrity-validated ICE exchange with the real rig
  (it probed us 99 times; every answer it sent us validated). The
  round-XXV STUN toolkit is live-proven byte-correct.
* **PLAY attempt 1 → 455** anyway. **Attempt 2 → rc:-2, ws_err:-2,
  ws_close:-1, ws_text still 5** — the WS died without a close frame:
  the rig drops the RTSP context after refusing PLAY.

**Conclusions (evidence-driven):**
1. Burst EXISTENCE before PLAY is necessary (run 6: no UDP → 455) but
   not sufficient (run 9: full bidirectional ICE → still 455). The
   remaining differences vs the reference at PLAY time: (a) our
   sequential pump goes QUIET during the PLAY exchange itself — the
   reference's receiver thread keeps bursting every 20 ms through it;
   (b) D-s1: no real DTLS identity (notably `invalid_in:0` — the rig
   sent NO DTLS records during the pump, so (b) is unproven either
   way).
2. Re-PLAY on a 455'd WS is pointless (dead channel) — PLAY must be
   one-shot, like the reference (which has no in-place retry at all).
3. `invalid_in:0` + `ok_in:327` also means the rig's answers used the
   remote password exactly as the reference expects — the credential
   ladder resolution is confirmed correct end-to-end.

**Fix (round XXVIII):**
* **The pump goes CONCURRENT:** a small worker thread (kernel32
  CreateThread / POSIX pthread — no new imports, D21's single-threaded
  MEDIA loop untouched: the thread is joined before the first media
  tick) spans exactly the PLAY request/response window, bursting every
  20 ms like the reference's receiver; single-owner socket discipline
  (main touches only WS/TLS while it runs; stats read after join).
  Spawn failure falls back to the sequential 400 ms pump (honest).
* **PLAY is one-shot** (the 455-retry loop is gone; the failure path is
  TEARDOWN + the resumable-session policy, with the full ice_preplay
  evidence line incl. `"thread":0/1`).
* **The Mjolnir NATT keepalive in the media loop** (nvst.rs:8849-8905):
  USERNAME `{setup x-nv-ping-payload}:{local ufrag}`, key = the
  DESCRIBE remote password, to the video peer — 20 ms until the first
  inbound datagram, then 100 ms — sent inline from the loop's 10 ms
  tick. The reference's comment is explicit: these pings are WHAT
  ROUTES THE VIDEO to the socket; a PLAY-200 run would otherwise risk
  media silence. Evidence: the `natt_keepalive {"sent":N}` line after
  the loop (the summary schema stays verify-compatible).
* `cm_control` relearn events carry a running `"n"`, and the `ready`
  line the total `"relearns"` — the migration pattern is now
  quantified evidence.

**E2E honesty note:** sandbox_session_e2e keeps its OWN sequential pump
+ two-PLAY sequence (it is the independent second implementation of the
WIRE shapes, and its rig gate accepts once a request validates — the
thread scheduling is knvstd glue, Win32-target territory; POSIX session
mode refuses at the TLS gate anyway). The rig/E2E stay at 43/43.

**Next measurement = R7 run 10:** if PLAY 200 → the keepalive carries
the routing and the media loop should see real Mjolnir SRTP (watch
`natt_keepalive.sent` + the summary's datagrams/aus/frames_out — R32's
final gate + the R34 inbound-UDP firewall answer). If PLAY STILL 455
with `"thread":1` and rich ice counters → difference (b) (the real DTLS
identity / handshake on the bundle) becomes the prime suspect and
stage-3 R3 moves up as the prerequisite for PLAY.

## 16. Round XXIX — R7 run 10 (the pump thread LIVE-PROVEN; PLAY went from instant-455 to DEAD-SILENCE → difference (b), the real DTLS identity, is now the prime and near-only suspect; R3 is promoted to the stage-2 PLAY blocker)

**What was measured (2026-10-26, the round-XXVIII tree; T10 confirmed
ALL PASS 72 zero warnings):**
* Queue: re-learning fired 24× (`relearns:24` in the ready line — the
  migration pattern quantified), the position sat at the `97` cap with
  real glimpses (17, 16, 5 — the queue WAS moving under the cap), then
  seat_step 3 → ready → rtsps://80-250-98-40…:322. ZERO 503s again —
  the run-9 finding held.
* OPTIONS/DESCRIBE/SETUP/ANNOUNCE 200 (SETUP peer 80.250.98.40:5004,
  first round; ANNOUNCE 4165 B — byte-identical size class to run 9).
* ice_creds identical shape to run 9 (all lengths 13, ping_ver 6,
  remote_pw 1, remote_fp 1 — the ladder is deterministic across rigs).
* **The pump thread performed exactly as built (`"thread":1`):**
  `pump_ms 20525` — it ran through the ENTIRE 20 s PLAY window (run
  9's difference (a) — silence during the exchange — is CLOSED);
  `stun_sent 2760, natt_sent 920` (920 bursts at the 20 ms cadence),
  `req_in 123, answered 123` (every rig probe validated + answered),
  `ok_in 3671` (the rig answered ~every one of ours), `invalid_in 0`,
  `send_errors 0`. A fully saturated, bidirectional, integrity-clean
  ICE/NATT channel for 20 straight seconds.
* **PLAY: rc -3 (timeout), status 0 — NO response at all** (ws_text
  stuck at 4: the fifth RTSP answer never came; ws_close -1: the WS
  stayed open, silent). A DIFFERENT rig than run 9 (80.250.98.40 vs
  77.111.249.145) and a different refusal mode: run 9 = instant 455,
  run 10 = hold-until-timeout. Both are "PLAY is not valid in this
  state" — one rig says it immediately, one waits for a state change
  that never comes.

**The elimination table is now nearly empty:**
| difference vs the reference | status |
|---|---|
| request wire shape (target/headers/CSeq) | proven identical (run 6-era; ANNOUNCE 200 shows the SDP is consumed) |
| hole-punch burst before PLAY | CLOSED (run 6 → run 9/10: bursts flow) |
| burst continuity DURING the PLAY exchange | CLOSED (run 10: thread:1, pump_ms 20525) |
| ICE credential ladder | PROVEN correct (invalid_in 0 both directions, ok_in 3671) |
| **the announced DTLS identity (D-s1 placeholder)** | **OPEN — the only major remaining difference** |
| (post-PLAY items: WS control pings, SCTP channels) | not yet reachable — after PLAY |

The reference announces a fingerprint of a REAL certificate
(`create_nvst_bundle_rtc` → str0m → `local_dtls_fingerprint`,
nvst.rs:5562-5592) and its bundle socket runs a live ICE+DTLS stack
from receiver-spawn time — i.e., by PLAY time the rig has typically
seen a DTLS ClientHello from a certificate matching the ANNOUNCE. We
announce a random 32-byte placeholder and never speak DTLS. Two rigs,
two refusal modes, one common missing layer. **Verdict: the stage-3 R3
spike (real DTLS identity + handshake on the bundle) is promoted to the
PLAY blocker — stage 2 cannot complete without it.** The plan lives in
**docs/DTLS.md** (new this round).

**Round XXIX delta (small, evidence-maximizing — the verdict above is
the real content):**
* **Pump warm-up:** when the thread spawns, PLAY waits 400 ms so it can
  never land before the first bursts (run 10 raced the thread start by
  design; run 9's sequential shape had 400 ms of history — this removes
  the last timing confound before run 11).
* **rtsp_detail grew the full WS counter set** (`ws_pings_in`,
  `ws_pongs_in/out`, `ws_closes_in`, `ws_frames_out`) — run 11's 20 s
  of silence will show whether the rig WS-pings us while it holds PLAY
  (the reference answers pings via tungstenite; kn_ws already
  auto-pongs — the counters make it visible).
* NOTE (recorded, not yet built): the reference keeps the control
  channel alive AFTER PLAY with WS PING frames carrying a sequence +
  RTT tracking (NvstControlPing, nvst_rtsp.rs:260-290, :844-930) — our
  kn_ws answers pings but never sends them; that is DTLS.md phase R3-D
  (small, ships with the media phase).

**Next measurement = R7 run 11** (same command): expected outcome with
high confidence = PLAY still refused (455 or timeout) — that is the
final elimination datum; the new counters tell us what the rig does on
WS during the hold. The queue/ICE evidence keeps accumulating for free
(every run now also stress-tests re-learning + the pump). Then R3-A
(the real certificate) starts — see docs/DTLS.md.

## 17. Round XXX — R7 run 11 (the FINAL elimination datum: PLAY refused with a saturated pump + total WS silence) + R3-A SHIPPED (kn_cert — the real certificate identity)

**Run 11 (2026-10-26, the round-XXIX tree; T10 ALL PASS 72 confirmed):**
the queue side is now boring in the best way — 25 re-learns, cap 146
with real glimpses (28/48/53), zero 503s, clean step 1→3→ready (third
consecutive run). OPTIONS/DESCRIBE/SETUP/ANNOUNCE 200 on a THIRD rig
(77-111-249-146; ANNOUNCE 4137 B — the new 4/22 identity shapes on the
wire). ice_creds identical (all lengths 13). The pump thread saturated
the window again: `pump_ms 20920`, 2820 STUN + 940 NATT, the rig's 126
probes ALL answered, 3753 rig successes validated, invalid 0 — and
**PLAY: timeout, with the new WS counters showing TOTAL control-channel
silence from the rig** (`ws_pings_in:0, ws_pongs_in:0, ws_closes_in:0,
ws_frames_out:5` vs `ws_text:4`): it neither answered, nor pinged, nor
closed. Three rigs, three refusals (455, timeout, timeout), one
constant: no real DTLS behind the announced fingerprint.

**Verdict: the elimination is FINAL.** Every transport behavior the
reference exhibits before PLAY is now reproduced live except the DTLS
layer itself. R3-A shipped this round (docs/DTLS.md phase 1):

* **NEW `kn_cert.{h,c}`** — the portable-mbedTLS self-signed identity:
  ECDSA P-256 keypair (the str0m provider default), X.509 v3
  self-signed with SHA-256, a FIXED wide validity window (2020-2035 —
  no wall-clock dependency; the rig pins by fingerprint, not by CA),
  random 16-byte serial. The fingerprint is the SHA-256 of the exact
  stored DER, rendered in the announced colon-uppercase-hex form
  (nvst.rs:5596-5603 ≡ str0m src/lib.rs:1662). The struct keeps the
  parsed x509 + pk contexts for R3-B (`kn_cert_pk`/`kn_cert_x509`
  opaque handles — the header stays mbedTLS-free). `kn_cert_selfcheck`
  is the built-in KAT: SHA-256(der)==fp, DER re-parses byte-identical,
  and an ECDSA sign/verify roundtrip through the CERTIFICATE's public
  key (proving the announced fingerprint binds our private key).
* **Build discipline (the kn_net TLS-stream pattern, extended):** the
  real body compiles under `KN_TLS_BACKEND_MBEDTLS` — which the CMake
  mbedtls backend defines on BOTH platforms, so the sandbox ctest runs
  the REAL battery (10 checks under ASan+UBSan: generation, fingerprint
  format/length, DER shape, selfcheck, distinct keypairs, guards),
  while every non-mbedTLS build gets the honest stub (available()==0,
  loud generate() refusal — 4 checks). cross_check compiles the real
  body with the exact target flags on x64+i686 whenever the mbedTLS
  tree exists (the round-XXII rule). T10 suite list += test_kncert →
  **target expectation 72 → 73**.
* **knvstd:** after `sess_local_identity` (the 4/22 shapes), a real
  certificate is generated when available and its fingerprint REPLACES
  the D-s1 placeholder in the ANNOUNCE; the new `identity` evidence
  line reports `{"ufrag_len":4,"pwd_len":22,"fp_len":95,"cert":"real"}`
  (or `"placeholder"` — the log never lies about which identity flew).
  R3-A frees the cert right after fingerprinting; R3-B will keep it for
  the handshake.
* ASan caught a real bug pre-ship: the first test draft double-freed
  (the free contract is free(NULL)-safe, NOT double-free-safe — the
  kn_net_tls_stream_close convention; test + header doc fixed).

**What run 12 tests:** the SAME command. The rig now receives an
ANNOUNCE whose fingerprint belongs to a real, existing P-256
certificate. If the rig's PLAY gate only checks that the announced
fingerprint is a well-formed/known-shape value or probes it in any way,
behavior may change; if the gate requires an actual DTLS handshake
(the leading theory), PLAY stays refused — and R3-B (the DTLS client
on the bundle socket, inside the pump thread's state machine) is the
next and likely decisive round. Either outcome is a clean measurement.
