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

## 18. Round XXXI — R7 run 12 (the REAL fingerprint flew; PLAY refused identically → the fingerprint alone is not the gate) + R3-B SHIPPED (kn_dtls — the DTLS 1.2 client on the bundle socket, inside the pump thread)

**Run 12 (2026-10-27, the round-XXX tree — R3-A's first live run):**
* **Target T10 reported ALL PASS but 72 checks, not the expected 73** —
  the round-XXX harness adds test_kncert as the 14th suite (+1 check).
  Most consistent reading: the target tree's `tests/T10_build_and_test.py`
  predates the round-XXX patch (suites list without test_kncert) while the
  C tree WAS updated (the live run proves it). Action for the next target
  round: confirm `test_kncert` is in the T10 suites list (repo copy is
  correct) — kn_cert's real body is live-proven regardless (below).
* Queue: the FIRST zero-wait run — `queue_position:0, seat_step:3` from
  the first polls straight to `status:2 ready` (`relearns:23`). A lucky
  seat; the re-learning machinery kept working (23 migrations).
* RTSP: OPTIONS/DESCRIBE/SETUP 200 on a FOURTH rig
  (`rtsps://80-84-170-153…:322`, SETUP peer `80.84.170.153:5004`, first
  round, bundle_fb 0 — run 6's rig was .155, the same subnet).
* **`identity {"ufrag_len":4,"pwd_len":22,"fp_len":95,"cert":"real"}` —
  R3-A LIVE ON THE TARGET: the ANNOUNCE (200, 4137 B) carried the
  SHA-256 fingerprint of a REAL ECDSA P-256 self-signed certificate**
  (fp_len 95 = 32 bytes as colon-hex; kn_cert_generate + fingerprint +
  the whole mbedTLS/PSA keygen path executed correctly on Win7/GCC 16.2).
* ice_creds: enabled 1, ping_ver 6, every length 13 — identical shape to
  runs 9/10/11 (the ladder is deterministic across four rigs).
* Pump: `thread:1, pump_ms 20921, stun_sent 2841, natt_sent 947,
  req_in 126, answered 126, ok_in 3788, invalid_in 0, send_errors 0` —
  the fourth consecutive saturated bidirectional ICE window.
* **PLAY: `rc:-3, status:0, attempt:1` — timeout in TOTAL WS silence,
  byte-for-byte the run-11 refusal shape** (`ws_err:-8` = the recv
  timeout, `ws_close:-1, ws_text:4, ws_bin:0, ws_pings_in:0,
  ws_pongs_in:0, ws_closes_in:0, ws_frames_out:5`).

**Verdict: announcing a real fingerprint changed NOTHING.** Four rigs,
four refusals (455, timeout, timeout, timeout); the rig never answered,
pinged, or closed — it waits for a state change that, per the reference,
is the DTLS handshake itself. The elimination table's last row is now
split in two: the fingerprint's *existence* is CLOSED (run 12); the
*handshake behind it* is the remaining gate. **R3-B (kn_dtls) is the
decisive round — shipped this round.**

### R3-B design record (every fact re-read from the reference this round)

**What the reference does at DTLS time:**
* `prepare_nvst_webrtc_bundle` (nvst.rs:6231-6317): pins the remote
  fingerprint parsed from the handoff (`parse_nvst_fingerprint` :5640 —
  the DESCRIBE `general.dtlsFingerprintV2`, optional `sha-256 ` prefix),
  sets both ICE credential pairs, `set_ice_controlling(true)`, host
  candidates only, and **`api.start_dtls(true)` (:6300) — the client is
  the DTLS-ACTIVE side** (str0m `init_dtls` :1482-1500 generates the
  ClientHello immediately once ICE unblocks it).
* The official client **gates the ClientHello on real inbound STUN**
  (:8196-8198 comment — "skips setupDtls until a real inbound STUN. Do
  not synthesize Binding Success — that only … sends ClientHello before
  GFN has a pair").
* **Socket pair** (`reserve_nvst_socket_pair_from` :5933-6000): video
  (Mjolnir) on N — PREFERRED 49005 — and the ICE/DTLS bundle on N+1
  (preferred 49006), because "the server still relies on this pairing"
  (:5484) and "some cloud seats do not route the dedicated Mjolnir video
  flow back to an arbitrary source port" (:5941-5944). Fallback: up to
  32 attempts at a dynamic adjacent pair. SETUP's `X-GS-ClientPort` =
  the BUNDLE port when unified, the VIDEO port otherwise
  (nvst_rtsp.rs:1176-1183); the ANNOUNCE's `clientBundlePort` +
  `a=candidate` = the BUNDLE socket (the reference test :2955 pins
  `clientBundlePort:49006`).
* Bundle worker cadence (:7837-7896): before `dtls_ready` — 3×STUN +
  1×NATT per 20 ms; **after `dtls_ready` — NATT-only per 100 ms**
  (PING_INTERVAL_AFTER_CONNECTION :243), still answering the rig's
  binding requests; a literal `PING` datagram is answered `PONG`
  (:8605-8614). Inbound demux: STUN by magic cookie, **DTLS by first
  byte 20..=63** (`looks_like_dtls` :6345), RTP only in unified mode
  (:8616-8640), everything else → str0m `handle_input`.
* `Event::Connected → dtls_ready = true` (:8214-8223) and only then
  `start_sctp(true)` + the data channels (= our R3-C). Video SRTP keys
  stay ANNOUNCE-derived (:1820-1840) — **no DTLS-SRTP for video**.
* str0m defaults: DTLS 1.2 (`DtlsVersion::Dtls12`, config.rs:775),
  ECDSA P-256 self-signed cert (R3-A-verified), fingerprint =
  SHA-256(DER).

**What R3-B ships:**
* NEW `streamer/{include/kn,src}/kn_dtls.{h,c}` — the portable-mbedTLS
  DTLS 1.2 ACTIVE client (the kn_cert build discipline: real body under
  KN_TLS_BACKEND_MBEDTLS on BOTH platforms, honest stub elsewhere):
  min=max=TLS 1.2 datagram preset; own cert = the kn_cert handles
  (`kn_cert_x509`/`kn_cert_pk` → `mbedtls_ssl_conf_own_cert` — the R3-A
  context is now consumed as designed); **fingerprint pinning as a
  DOUBLE GATE: a verify callback clears the chain flags EXCLUSIVELY on
  an exact SHA-256(peer DER) == pinned DESCRIBE fingerprint match, and
  `kn_dtls_poll` REFUSES the ready verdict unless verify_result == 0
  AND the pin matched** (mismatch latches KN_DTLS_ERR_PIN + a
  close_notify to the peer). The authmode is VERIFY_OPTIONAL — NOT a
  weakening: mbedTLS fails VERIFY_REQUIRED *without a CA chain* before
  any callback runs (ssl_tls.c:8071 CA_CHAIN_REQUIRED) and a WebRTC
  peer is self-signed by design, so REQUIRED is simply unusable here;
  no CA, no date, no bypass — the pin IS the policy, strictly stronger
  than a chain check (a forgery needs the rig's private key);
  **inject-queue BIO** (the pump thread owns the socket — kn_dtls never
  touches it for recv; `kn_dtls_inject()` feeds demuxed datagrams, the
  send callback goes through `kn_sock_udp_sendto`); retransmission
  timers on `kn_sock_now_ms` (set/get_timer contract -1/0/1/2);
  MTU 1200 (str0m fragments similarly; keeps handshake flights off IP
  fragmentation); HVR cookies handled by the mbedTLS client core;
  post-ready `mbedtls_ssl_read` drains app records into a counted
  discard buffer (R3-C hooks here — SCTP records are evidence, never
  silently dropped). Stats are shapes/counts only (§7).
* **The socket pair** (reference layout): knvstd phase C reserves
  video N + bundle N+1 (preferred 49005/49006, then ≤32 dynamic
  adjacent attempts, hard-fail honestly) BEFORE SETUP; SETUP announces
  unified?bundle:video; clientBundlePort/candidate/localAddress come
  from the bundle socket (connected to the SETUP peer first — the
  routed source address, like today's single socket). Non-unified:
  the pump thread works the bundle socket (connected to ice_dest), the
  media loop works the video socket (connected to the SETUP video
  peer) — **the old save/restore reconnect dance is GONE** (each
  socket keeps one peer for life). Unified: one socket serves both
  (today's handoff shape).
* **Pump-thread state machine**: demux STUN | DTLS(20..63) | PING→PONG
  | invalid; the DTLS handshake arms at the FIRST REAL INBOUND STUN
  evidence (req_in or ok_in — the :8196 rule, never synthesized);
  ClientHello → full handshake interleaved with the unchanged 20 ms
  bursts; on `dtls_ready` the cadence switches to the reference's
  post-connection shape (NATT-only, 100 ms) and the thread PERSISTS
  through the media loop in non-unified mode (the reference's bundle
  worker runs for the whole session; single-owner discipline holds —
  main touches only the video socket, D21's media loop untouched).
  New evidence: `sock_pair`, `dtls_arm`, `dtls_ready` / `dtls_fail`
  (emitted from the thread AT the transition — timing is evidence),
  `ice_preplay` grows the dtls fields; a snapshot handshake (req/done
  flags) lets main read the counters while the thread runs.
* Gates: test_kndtls (15th suite) — loopback handshake against an
  in-test mbedTLS DTLS server WITH HelloVerifyRequest cookies AND the
  client cert pinned server-side (the rig's shape: pinning in BOTH
  directions), loss/retransmit legs in both directions, wrong-pin
  refusal at the ready gate, mid-handshake free, app-data drain
  (31 checks, ASan+UBSan+LSan clean); PLUS an OpenSSL-3
  `s_server -dtls1_2` interop leg (the two-implementations rule — an
  independent DTLS stack must accept our ClientHello/cookie/cert/
  Finished) driven by `tools/dtls_openssl_check.py` (PASS: DTLSv1.2,
  TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256, ready in ~24 ms).
* **Three mbedTLS landmines found and documented this round** (they
  cost the first test drafts a segfault and two refactorings):
  (1) a non-blocking BIO recv must return `WANT_READ` on an empty
  queue, NEVER `MBEDTLS_ERR_SSL_TIMEOUT` — fetch_input reads a BIO
  TIMEOUT as an expired retransmission delay and calls
  `mbedtls_ssl_resend()`, which dereferences a NULL flight before the
  first flight is buffered (ssl_msg.c:2256-2272 → :2540);
  (2) `VERIFY_REQUIRED` without a CA chain is fatal BEFORE the verify
  callback (ssl_tls.c:8071) — fingerprint pinning requires the
  OPTIONAL + callback + post-handshake-gate pattern;
  (3) a cookie-enabled DTLS SERVER surfaces
  `MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED` from
  `mbedtls_ssl_handshake` and the application must
  `mbedtls_ssl_session_reset` + re-arm
  `mbedtls_ssl_set_client_transport_id` before continuing (the
  dtls_server.c example's contract) — client-side HVR is automatic.

**Deviations (numbered, per R32):**
* **D-t1** — no str0m ICE agent: the bursts are the reference's own
  hand-built USERNAME-only STUN (:5013-5028 — no USE-CANDIDATE); the
  reference's PARALLEL str0m agent may nominate, but four live runs
  showed the rig answering non-nominating traffic happily. Revisit only
  if run 13+ shows the rig waiting for nomination.
* **D-t2** — unified-socket mode: no bundle servicing during the media
  loop (one socket, one owner — the pump joins before the media loop
  exactly like today). Non-unified gets the full-lifetime bundle
  worker. Which mode real seats describe is run 13's measurement (the
  `ice_creds` line now carries `unified`).
* **D-t3** — no `use_srtp` DTLS extension: video SRTP is
  ANNOUNCE-derived (:1820-1840); bundle audio does not exist yet
  (stage 3+). If the rig's server REQUIRES use_srtp, run 13 names it
  (handshake alert evidence).
* **D-t4** — post-handshake app data (the rig's SCTP INIT) is counted
  and discarded until R3-C; the rig's tolerance for a silent SCTP peer
  is run 13+'s measurement.

**What run 13 tests:** the same command. Expected sequence: `sock_pair`
(preferred 49005/49006 or dynamic) → identity cert:real → ANNOUNCE 200
→ ice_creds (now with `unified`) → pump → **`dtls_ready` with ms-since-arm
+ record counters** (or `dtls_fail` with the mbedTLS rc — itself a
precise diagnosis: alert from the rig vs timeout vs pin mismatch) →
PLAY's verdict. If PLAY 200: the media loop on the video socket,
`natt_keepalive.sent`, and the first real Mjolnir SRTP → kn_rx → AUs
(R32's final stage-2 gate + R34's inbound answer at media scale). If
PLAY still refuses WITH dtls_ready:1 → the gate needs SCTP channels
(R3-C promoted); if dtls_fail → fix per the rc. Either outcome is a
clean measurement.

## 19. Round XXXII — R7 run 13 (THE DTLS HANDSHAKE COMPLETED ON A LIVE PRODUCTION RIG — the R3-B layer is live-proven; PLAY moved from silence to an explicit 400, whose body we did not capture → round XXXII instruments the refusal and closes the last pre-PLAY fidelity gap)

**Target T10 (the synced harness): ALL PASS 74, zero warnings** — the
round-XXXI prediction exactly; test_kndtls ran its REAL 31-check
battery on Win7 (the first DTLS handshake executed on the target in
the suite), test_kncert's 14 checks finally ran there too.

**Run 13, invocation 1 (legal queue wait):** 20 re-learns, the 156 cap
with real glimpses (44/48/47/33/7), clean step 1→3→ready on the FIFTH
rig (80-250-96-38). Then, in order:
* **`sock_pair {"video_port":49005,"bundle_port":49006,
  "selection":"preferred","unified":0}`** — the official reserved pair
  bound cleanly on the target (both ports free), 8 MiB rcvbuf each.
  **`unified:0` answers DTLS.md open question 4: real PL-region seats
  describe the NON-unified two-socket layout** — the pair + the
  persisting bundle worker are the production path, not an edge case.
* OPTIONS/DESCRIBE/SETUP 200 (peer :5004, first round), identity
  cert:real, ANNOUNCE 200 (4137 B).
* `dtls_arm {"armed":1}` → **`dtls_ready {"ms":388,"version":
  "DTLSv1.2","cipher":"TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256",
  "records_in":2,"records_out":2,"verify":0}`** — **THE FIRST REAL
  DTLS HANDSHAKE WITH A PRODUCTION GFN RIG.** Everything R3-B bet on
  is live-confirmed: the rig answered our client-active ClientHello,
  negotiated ECDHE-ECDSA over DTLS 1.2, REQUESTED AND VERIFIED our
  client certificate against the ANNOUNCE'd fingerprint (a completed
  handshake is the proof — a pin mismatch on its side aborts), and our
  pin verified its certificate (verify:0). The ECDSA/PSA sign path
  (CertificateVerify) works on Win7 — which also retroactively closes
  the run-12 T10-72 worry about test_kncert never running there.
* **PLAY: `rc:0, status:400`** — refusal mode #3 (after run 9's
  instant 455 and runs 10-12's hold-in-silence). The rig ANSWERED
  (ws_text 5, ws_err 0, ws_close -1): it now considers the request
  evaluable and rejects something specific. **The response body/
  headers were NOT captured — the round's main evidence gap, closed by
  the delta below.**
* `ice_preplay`: stun 54, natt 20, req_in 3/answered 3, ok_in 40,
  invalid 0, persist:1, dtls_ready:1 — and **`dtls_in:3` vs
  `records_in:2` at ready time: the rig sent EXACTLY ONE app-data
  record right after the handshake** (discarded per D-t4; its content
  — an SCTP INIT? something else? — is R3-C's design input, now
  captured by the delta). `pump_ms:0` = the persist-mode snapshot
  artifact (the total only landed at thread exit) — fixed.

**Run 13, invocation 2 (the user re-ran the command after the first
exited — a live RESUME-at-create measurement):** `cm_created`
returned the SAME session_id with `status:2, phase:ready` — CloudMatch
hands a live READY session back to a repeat create (no queue, no
SESSION_LIMIT; the reference's attach path, R23-adjacent, measured).
The rig accepted a FRESH certificate in the re-ANNOUNCE (fingerprints
are not cached rig-side), DTLS completed again (293 ms; records_in 7 /
out 3 — a different flight shape, likely the HVR cookie round), and
PLAY 400'd identically, again with exactly one post-ready inbound app
record (dtls_in 8 vs 7).

**The 400 in context — what the reference has that we still don't at
PLAY time (re-read this round):**
1. **SCTP INIT**: the reference's bundle worker calls
   `start_sctp(true)` the instant DTLS connects (nvst.rs:8214-8223 —
   the client initiates the association); by PLAY the rig has our
   INIT. We send no app data (R3-C = usrsctp is the planned fix; the
   captured app record will show whether the rig also initiates).
2. **The pre-PLAY Mjolnir keepalive**: the reference arms BOTH
   workers before `finish()` (lib.rs:937 bundle → :990 mjolnir →
   :1117 finish) — the video socket fires its NATT keepalives from
   port N at 20 ms BEFORE PLAY (:8849-8905, "the pings are what ROUTES
   video here"). Run 13 left the announced video port 49005 silent
   until the (never-reached) media loop.
3. (Ruled out this round: the PLAY request shape — `finish()`
   (nvst_rtsp.rs:787-800) sends PLAY to the base target with
   common_headers + Session, byte-identical to ours; and `wait_for_play`
   (:738-743) is a state FLAG, not a blocker — the reference workers
   really do run pre-PLAY.)

**Round XXXII delta (measurement-maximizing; no protocol guesses —
"measure first, then code"):**
* **`rtsp_response` capture**: every non-200 RTSP answer now dumps its
  reason phrase, ALL headers, and a sanitized 240-byte body excerpt
  (kn_rtsp_response already parsed everything; run 14 reads the rig's
  own words about the 400 instead of guessing).
* **`rig_info`**: the DESCRIBE response's `Server` header (§16 open
  question 5 — correlating refusal modes with rig software versions).
* **The first DTLS app record is captured verbatim** (≤64 B head +
  the true length) in kn_dtls stats and printed as hex in
  `ice_preplay` (`app_records`/`app_bytes`/`app_first_len`/
  `app_first`) — names the rig's post-handshake protocol (SCTP INIT
  chunks start with the SCTP common header; the first bytes will show
  it) = R3-C's design input.
* **The pre-PLAY video-socket keepalive** (fidelity item 2 above): the
  pump thread now ALSO sends the Mjolnir NATT keepalive from the video
  socket at 20 ms (send-only — it never recvs there; inbound media
  queues in the 8 MiB socket buffer for the media loop) until main
  raises `vid_handoff` at media-loop entry. Non-unified mode only
  (unified: the bundle pings already cover the shared socket).
  `vid_ka` counter in ice_preplay.
* `pump_ms` accumulates live (the persist snapshot reads the true
  total now); test_kndtls 31→**32** checks (the first-app capture is
  asserted byte-exact against the test server's record).
* Deliberately NOT shipped: a hand-rolled SCTP INIT probe. A malformed
  INIT could poison rig state (the SETUP-sweep poison precedent,
  nvst_rtsp.rs :455-470) and R3-C (usrsctp) is the faithful path —
  run 14's captured 400 body + app-record hex decide whether SCTP is
  even the gate.

**What run 14 tests (same command):** the decision matrix —
(a) `rtsp_response` names the 400's cause → the targeted fix ships
next round; (b) 400 persists with everything captured → R3-C
(usrsctp) is promoted with the app_first hex as its design input;
(c) PLAY 200 (the video-socket keepalive was the missing routing
piece) → the media loop on 49005: `natt_keepalive.sent`, the summary's
datagrams/aus/frames = R32's final stage-2 gate. In ALL cases the rig
now talks back — the silence era is over.

## 20. Round XXXIII — R7 run 14 (the 400 CAPTURED: empty body, echo headers — a generic handler refusal; `app_records:0` proves the rig waits for OUR SCTP INIT) → R3-C SHIPPED: kn_sctp (vendored usrsctp, AF_CONN over the DTLS app layer) + the 8-channel DCEP profile + bounded PLAY gating

**Target T10 (the round-XXXII tree): ALL PASS 74, zero warnings** — the
instrumentation round verified; test_kndtls's real battery ran on Win7.

**Run 14 (live production, rig #6 = 185-136-70-139, fast queue: 17
relearns, cap-156 glimpses 44/48/47/33/7 → ready):**
* `sock_pair 49005/49006 preferred, unified:0` again (second
  consecutive seat on the reserved pair — the ports are reliably free
  on the target and the two-socket layout is the production norm).
* OPTIONS/DESCRIBE/SETUP/ANNOUNCE 200 (4136 B), identity cert:real.
* **`dtls_ready ms:190`** — the fastest live handshake yet, same
  cipher (TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256), verify:0. The R3-B
  layer is now THREE-for-three on live rigs (388/293/190 ms).
* `vid_ka:23` — the round-XXXII pre-PLAY video-socket keepalive fired
  (23 NATT pings from 49005 before PLAY). **PLAY: still 400** — the
  video-port silence was NOT the missing piece.
* **`rtsp_response` caught the refusal**: `status 400, reason "OK"` (!),
  headers = ONLY the echo set (`request-id:5, cseq:5, x-gs-version:
  14.2`), `body_len 0`. A generic templated refusal from a handler that
  had nothing specific to say — NOT the state-machine's 455 (run 9) and
  NOT silence (runs 10-12). The rig evaluates PLAY once DTLS is up and
  rejects on a missing condition it doesn't bother to name.
* **`app_records:0, app_bytes:0`** — the rig sent NO DTLS app data
  after the handshake (the run-13 "one extra record" was NOT app data;
  with the round-XXXIII inject-level capture the next run names it —
  ContentType byte: 20 CCS / 21 alert / 22 handshake / 23 app). So the
  rig does NOT initiate SCTP: it waits for the CLIENT's INIT, exactly
  like the reference (`start_sctp(true)` = "this side initiates",
  nvst.rs:8221 + str0m direct.rs `start_sctp(client: bool)`).
* `pump_ms:491` (the live accumulation works), dtls_in 8 vs 7 at ready
  (the mystery non-app record again), req_in 3/answered 3, ok_in 20,
  invalid 0 — the fourth consecutive clean ICE window.

**Verdict: the missing layer is SCTP.** At PLAY time the reference has:
DTLS up + the SCTP association initiated + the 8 DCEP channels opening
(`Event::Connected → start_sctp(true) → NvstInputChannels::create`,
nvst.rs:8214-8233, all before `finish()` at lib.rs:1117). We had DTLS
up and nothing above it. **R3-C shipped this round** (the BOOTSTRAP's
usrsctp choice; full design + the SEVEN measured usrsctp landmines are
in the NEW `docs/SCTP.md`):

* NEW `streamer/{include/kn,src}/kn_sctp.{h,c}` — vendored usrsctp
  (pinned c625e219…, SHA-256-verified tarball via T10, the mbedTLS
  model) in AF_CONN mode over the DTLS app layer: the wire parameters
  mirror sctp-proto's defaults exactly (5000↔5000, OS/MIS 65535,
  a_rwnd 1 MiB, RTO 3000/1000/60000, unlimited INIT retries, MAXSEG
  1200); the tx thread-boundary queue (usrsctp's timer thread may emit
  any time; the mbedTLS context stays single-threaded); the PURE DCEP
  helpers (RFC 8832 marshal/parse, golden-tested against the
  independent Python reference) available in EVERY build; the honest
  stub elsewhere (`sctp_arm {"armed":0}` — the log never lies).
* NEW `tools/sctp_reference.py` + generated
  `streamer/src/kn_sctp_kat_vectors.h` — the independent Python
  implementation (CRC-32c proven against RFC 3720 B.4 before emitting;
  hand-transcribed DCEP goldens; the full NVST profile; an INIT packet
  builder) — the two-implementations rule at the SCTP wire level.
* NEW `streamer/tests/test_knsctp.c` (16th suite, **33 real checks**):
  goldens, the independent live-INIT parse (ports/CRC/OS/MIS), the full
  4-way association over a memory relay, the 8-channel burst received
  with exact sids/labels/types/params + ACKed, bidirectional user data,
  loss→SACK recovery, garbage tolerance — ASan+UBSan+LSan clean.
* knvstd: the pump thread arms kn_sctp the instant `dtls_ready` fires
  (the reference's `Event::Connected` moment), opens ALL 8 channels on
  COMM_UP, answers rig-side OPENs with ACKs, captures the first rig
  message head; PLAY now WAITS (bounded, `--session-play-wait=2500`
  default) for `sctp_up` + a settle (`--session-play-settle=250`)
  before flying — deviation D-t5 (runs 13/14 PLAYed ~12 ms after
  dtls_ready; the reference's PLAY always lands behind its SCTP
  initiation). New evidence: `sctp_arm`, `sctp_up {"ms"}`,
  `sctp_channels {"opened","acked","rig_opened"}`, `sctp_down`, and
  ice_preplay grew 10 sctp fields incl. the first-rig-message hex.
* kn_dtls: the app path became a bridge — `kn_dtls_write_app` /
  `kn_dtls_read_app` (a bounded decrypted-app ring; D-t4's counted
  discard is now the fallback only) + the post-ready RAW record
  capture (`first_post_ready`: run 15 names the mystery record).

**What run 15 tests (same command; the decision matrix):**
1. **PLAY 200** → the SCTP association (± channels) was the gate: the
   media loop on 49005 starts — `natt_keepalive.sent` + the summary's
   datagrams/aus/frames = R32's FINAL stage-2 gate (first real Mjolnir
   SRTP → kn_rx → AUs → decoder).
2. **PLAY still 400 WITH `sctp_up` + `sctp_channels acked:8`** → the
   gate is deeper (rig-side channel data? an RTCP RR on sid 14? the
   `sctp_first*` capture shows what the rig itself sent) — stage-3
   senders are already KAT'd, wiring is the next round.
3. **`sctp_fail` / no `sctp_up`** → the rig's DTLS-app behaviour
   differs from the loopback peer (the `first_post_ready` hex +
   dtls_fail/sctp counters localize it).
4. Any outcome: paste `sock_pair`, `dtls_ready`, `sctp_arm`, `sctp_up`,
   `sctp_channels`, PLAY, `rtsp_response` (if non-200), `ice_preplay`
   verbatim; on PLAY 200 also `handoff` + the summary.

## 21. Round XXXIV — R7 run 15 (usrsctp LIVE on Win7: the INIT flew, the rig stayed silent, PLAY 400 #4 → the nomination hypothesis: USE-CANDIDATE ships) + the target-warning fix (the seventh catch)

**Target T10 (the round-XXXIII tree): FAIL on exactly ONE check —
`zero compiler warnings (3 seen)`**; everything else passed (75/76),
incl. the usrsctp download/build and test_knsctp on Win7. GCC 16.2
flagged `-Wmaybe-uninitialized` on `test_knsctp.c`'s `t_sctp_pkt p`
(the independent INIT parser fills the INIT fields conditionally; the
`prc == 0 &&` short-circuit protects the reads, but GCC's value-range
pass cannot prove the interprocedural invariant — sandbox GCC 12 at
-O1/-O3 was silent). **The seventh target-catch incident** (R28's
pattern holds); fix: zero-init both declarations (the round-XXVII
warning-class discipline). This log also PROVES the round-XXXIII
hotfix worked: the usrsctp Win32 build/link is clean on the target.

**Run 15 (live production, rig #7 = 80-84-170-153 — run 12's rig
again):** queue clean (19 relearns, cap-53 glimpses 50/14/15/13/17/6,
1→3→ready). Then the R3-C layer flew for the first time:
* `sock_pair 49005/49006 preferred, unified:0` (third consecutive
  seat on the reserved pair).
* OPTIONS/DESCRIBE/SETUP/ANNOUNCE 200 (4137 B), identity cert:real,
  `dtls_ready ms:178` — the FOURTH consecutive live DTLS handshake
  (388/293/190/178 ms — the layer is boring now, exactly as it
  should be).
* **`sctp_arm {"armed":1}` — usrsctp initialized and the SCTP INIT
  left the building on Win7** (`dtls_out` 3→4 = exactly one app
  record post-ready; `sctp_out` was not yet in the evidence — added
  this round).
* **The rig NEVER answered the INIT**: `sctp_up:0` after the full
  bounded wait (`pump_ms:2596` = 100 ms head start + the 2500 ms
  play-wait — the D-t5 gate worked as designed), `app_records:0`
  again, `sctp_fail:0` (no COMM_LOST — just silence). usrsctp's first
  INIT retransmit (RTO 3000 ms) never got its chance inside the
  window — honest and reference-shaped (unlimited retries).
* **PLAY: 400 again** — the FOURTH identical refusal (`reason:"OK"`,
  echo headers, empty body). Two rigs now refuse identically
  post-DTLS (80-250-96-38 ×2, 185-136-70-139, 80-84-170-153).
* `vid_ka:127` (the video keepalive pumped the whole window),
  post-ready cadence switch visible: `stun_sent 24 / natt_sent 32`
  (NATT outlives STUN exactly like :7837-7896), `dtls_in 8` vs
  `records_in 7` at ready — **the mystery post-ready record arrived
  AGAIN** (third run in a row; captured since XXXIII but NOT printed
  — this round's evidence fix).

**The analysis (what run 15 eliminated, what remains):**
* SCTP INIT alone does not move the rig. Either it (a) never sees the
  INIT as legitimate (framing? checksum? — the sandbox suite verified
  both against independent implementations, but the live path adds the
  DTLS record layer — `dtls_out:4` says one record left; the rig's
  silence could also be a deliberate pre-PLAY posture), or (b) its
  data-plane arming waits on something else entirely.
* The strongest remaining ICE-layer difference vs the reference is
  **nomination**: the reference runs str0m's FULL ICE agent alongside
  the hand-built bursts (every STUN datagram feeds `rtc.handle_input`,
  nvst.rs:8652; poll_output ships whatever str0m emits, :8160-8180),
  and a CONTROLLING str0m agent nominates the valid pair with
  USE-CANDIDATE (RFC 8445 §5.2 — `set_ice_controlling(true)`,
  :6295). A libwebrtc-derived rig (Bifrost) plausibly gates its
  transport-ready state — and with it the SCTP endpoint and the PLAY
  handler — on the SELECTED pair, which only a nomination creates.
  DTLS completing without it fits: stacks gate DTLS on connectivity,
  not on selection.
* Alternative hypotheses kept alive by the missing print: the mystery
  post-ready record could be a DTLS alert/close (the rig unhappy with
  something at the record layer) — `post_first` hex now names it
  (ContentType byte: 20 CCS / 21 alert / 22 handshake / 23 app).
  use_srtp (D-t3) stays a candidate only if that record turns out to
  be an alert.

**Round XXXIV delta:**
* **USE-CANDIDATE nomination ships (D-t8 retires D-t1):**
  `kn_sock_stun_build2` (the two-attribute form; `build` is the
  attr2-less case — wire-identical, the existing goldens re-prove it)
  + `KN_SOCK_STUN_ATTR_USE_CANDIDATE 0x0025` (zero-length). The pump
  nominates from the SAME gate as the DTLS arm (the first REAL inbound
  STUN evidence — never synthesized, the :8196 rule): pre-ready the
  3× bursts carry the flag; post-ready ONE nominated request rides
  each 100 ms tick alongside the NATT ping (mirroring str0m's
  selected-pair traffic beside the hand-built layer). `nom_sent`
  counter in ice_preplay. KAT: the nominated packet is byte-exact vs
  an independently generated golden — the Python builder SELF-PROVED
  first by reproducing the round-XXV k_req/k_succ goldens byte-for-byte
  (incl. GFN's non-RFC `0x5354554E` fingerprint XOR, nvst.rs:258 —
  live-validated by four rigs answering our requests). test_knsock
  98→**102**.
* **The evidence gaps closed:** ice_preplay grew `post_first`/
  `post_first_len` (the mystery record's raw head — captured since
  XXXIII, printed at last), `peer_closed`, `nom_sent`, `sctp_out`/
  `sctp_in` (kn_sctp packet counters — proof the INIT left at the
  SCTP layer, not just the DTLS layer), and `sctp_fail` gained the
  send-error class (3).
* The test_knsctp zero-init warning fix (the seventh target catch,
  above).
* Gates re-run on the final tree: native 16 suites (test_knsock 102),
  cross ×2 incl. the usrsctp link leg, ctest **18/18 ×2**, ASan+UBSan
  test_knsctp 33/33 + test_knsock 102/102, E2E 43/43, T10 dry 66,
  T13 63.

**What run 16 tests (same command):**
1. `nom_sent > 0` + **the rig's reaction**: does `sctp_up:1` finally
   fire (nomination armed its data plane → it answers the INIT)? Does
   PLAY turn 200?
2. `post_first` names the mystery record (expect one of: 21+02 00 =
   close_notify, 21+02 xx = alert (xx names the complaint — e.g.
   unexpected_message could point at use_srtp expectations), 22 =
   renegotiation HelloRequest, 23 = app data that arrived between
   ready and the first drain).
3. If nomination changes NOTHING and post_first is benign: the
   remaining wire-level suspect list narrows to the DTLS record layer
   itself (use_srtp extension, D-t3) and the rig's expectation of
   specific channel data — both actionable next round.
4. As always: paste sock_pair → dtls_ready → sctp_arm → sctp_up (or
   its absence) → PLAY → rtsp_response → ice_preplay verbatim.

## 22. Round XXXV (sandbox, pre-run-16) — the SECOND fidelity gap found by re-reading the reference: str0m's ClientHello carries use_srtp (DTLS-SRTP, RFC 5764) and derives the bundle audio's keys from it → kn_dtls now offers AES128_CM_HMAC_SHA1_80 (D-t9), wire-pinned + OpenSSL-negotiated

**The finding (read, not guessed):** str0m's DTLS layer emits
`DtlsOutput::KeyingMaterial(km, profile)` right after Connected
(vendor str0m src/lib.rs:1648-1658 → `session.set_keying_material`,
session.rs:259-272) — i.e. the reference NEGOTIATES DTLS-SRTP (the
RFC 5764 `use_srtp` ClientHello extension) and takes the bundle
audio's SRTP keys from the DTLS session. Its offer list (crypto/
srtp.rs `SrtpProfile::ALL`, most-preferred first): **AEAD_AES_256_GCM
(0x0008), AEAD_AES_128_GCM (0x0007), AES128_CM_SHA1_80 (0x0001)**.
Our own ANNOUNCE mirrors `rtcAudioOnNativeBundle:1` — audio on the
bundle is part of the session shape we advertise, yet our ClientHello
(runs 13-15) carried no use_srtp at all (deviation D-t3, until now).
A Bifrost-class rig MAY treat a transport without negotiated SRTP as
non-media-capable — hypothesis #2 beside run 15's nomination gap, and
a fidelity fix regardless of the rig's opinion (the R32 rule: be the
reference on the wire).

**What shipped (round XXXV, sandbox-only round — no live run consumed):**
* **mbedTLS DTLS-SRTP enabled the sanctioned way**: NEW
  `streamer/src/kn_mbedtls_user_config.h` via the
  `MBEDTLS_USER_CONFIG_FILE` mechanism (the pinned vendor tree stays
  untouched — the sha1.c discipline). CMake sets the define PUBLIC on
  the mbedtls target: it CHANGES THE STRUCT LAYOUT of
  mbedtls_ssl_context/config, so every consumer (kncore → knvstd →
  tests) inherits it — an asymmetric build would be silent memory
  corruption. The four cross_check mbedtls legs carry the same define.
* **kn_dtls offers `[SRTP_AES128_CM_HMAC_SHA1_80 (0x0001)]`, no MKI** —
  the INTERSECTION of the reference's list and mbedTLS 3.6's ceiling
  (no AEAD-GCM SRTP profiles in 3.6) and the mandatory WebRTC profile
  (**deviation D-t9**, subsumes D-t3). New API
  `kn_dtls_srtp_profile()` → the negotiated IANA id (0 = not
  negotiated). RFC 5764 semantics make this SAFE by construction: a
  server without a common profile simply omits the extension and the
  handshake proceeds exactly as in runs 13-15 — pinned by a test leg.
* **The keying material itself is NOT extracted yet** — bundle audio
  is a later stage; §7 shapes-only evidence: the `dtls_ready` line
  grew `"srtp_prof"` (the negotiated id).
* **Tests (test_kndtls 32→39):** the ClientHello is captured on a
  second loopback socket and an INDEPENDENT hand parser (RFC 6347
  record + RFC 5246 body + the extension walk) asserts `use_srtp
  0x000e` with ONE profile 0x0001 and an empty MKI — the parser bug
  it caught on the way is itself evidence: RFC 5764's profile vector
  is a BYTE LENGTH (`<2..2^16-1>`), not a count. Plus: negotiation
  asserted on BOTH ends of the loopback pair (0x0001 chosen), and the
  NEGATIVE leg — a server without profiles still completes the
  handshake with profile 0 (the runs-13-15 wire can never regress).
* **OpenSSL interop grew a second leg**: the same client against
  `s_server -dtls1_2 -use_srtp SRTP_AES128_CM_SHA1_80` →
  **`srtp_profile=0x0001` negotiated through an INDEPENDENT stack**;
  the plain leg still passes with 0x0000 (the RFC omission semantics,
  cross-verified).
* Gates: native 16 suites, cross ×2 (the four mbedtls legs + usrsctp
  link leg), ctest **18/18 ×2** (fresh rebuilds — layout change),
  ASan+UBSan+LSan 39/39, E2E 43/43, T10 dry 66, T13 63.

**Run 16 now tests TWO reference-faithful additions at once** (both
individually justified by the reference read; the evidence lines
deconfound them): the USE-CANDIDATE nomination (round XXXIV, D-t8) and
use_srtp (this round, D-t9). Reading the tea leaves:
* `sctp_up:1` → the nomination armed the rig's data plane (SCTP
  answers the INIT on a SELECTED pair) — hypothesis #1.
* `dtls_ready … "srtp_prof":1` + PLAY 200 with sctp still down → the
  SRTP-capable transport was the gate — hypothesis #2.
* PLAY 200 with everything up → R32's final stage-2 gate (media).
* Nothing changes → `post_first` (the mystery record's ContentType,
  printed since XXXIV) becomes the primary clue: 0x15 (alert) points
  at the record layer, 0x17 (app) at a rig-initiated protocol, 0x16
  (handshake) at renegotiation.

## 23. Round XXXVI — R7 run 16: **PLAY 200 — THE FIRST REAL GFN VIDEO FLOWED THROUGH THE FULL STACK** (R32's final stage-2 gate PASSED); the stream stalled at ~10 s for lack of feedback → the reference's full feedback sender set ships this round

**Target T10 (rounds XXXIV+XXXV tree): ALL PASS 76, zero warnings** —
the use_srtp/nomination build verified; the mbedTLS layout change
clean on GCC 16.2.

**Run 16 (live production, rig #8 = 185-136-70-135; queue: 23
relearns, the 78 cap with real glimpses, clean 1→3→ready):**
* `sock_pair 49005/49006 preferred, unified:0`; OPTIONS/DESCRIBE/
  SETUP/ANNOUNCE 200 (4137 B); identity cert:real.
* **`dtls_ready ms:183 … "srtp_prof":1`** — the rig NEGOTIATED
  use_srtp 0x0001 (hypothesis #2's datum: the transport is now
  SRTP-capable in the rig's eyes).
* **`sctp_up {"ms":52}`** — the rig ANSWERED the SCTP INIT this time
  (run 15: eternal silence). The association came up in 52 ms.
* **`sctp_channels {"opened":8,"acked":8,"rig_opened":0}`** — all 8
  DCEP channels opened and ACKed by the rig.
* **`PLAY: rc:0, status:200`** — **THE FIRST SUCCESSFUL PLAY IN
  PROJECT HISTORY** (16 runs: 455 → timeouts → 400 ×4 → **200**).
  Both round-XXXIV/XXXV additions were live (nom_sent:18,
  srtp_prof:1); the evidence cannot fully deconfound which unlocked
  the INIT answer — both are reference-faithful and both stay.
* `handoff`: video_peer :5004, packet_size 1280, **srtp_profile
  AEAD_AES_256_GCM_8, key_source local** (the rig encrypts with the
  key OUR announce carried), rtcp_on_sctp 1, unified 0.
* **THE MEDIA LOOP RAN ON REAL MOLJNIR TRAFFIC: `datagrams 4900,
  authenticated 4900` (SRTP GCM-256-tag8 — 100 % authentication, zero
  recv_errors), `aus 307, frames_decoded 307, frames_failed 0,
  decode_errors 0, q_overflows 0, fec_repaired 0, stale 0, gaps 0,
  withheld 0` — libavcodec 63.2.100, 1280×720.** ffprobe accepts
  au_00001_k.264: h264 High, yuv420p, bt709, 60 tbr. **R32's final
  stage-2 gate — real GFN SRTP → kn_rx → AU → decode — PASSED.**
* `ice_preplay` rich evidence: `invalid_in:0` thanks to the new
  classification — **`bundle_rtp` traffic confirmed** (run 15's
  invalid_in:175 was the rig's AUDIO on the bundle, as theorized);
  `post_first "17fefd…"` — the mystery post-ready record is
  **ContentType 0x17 = APPLICATION DATA, epoch 1** (the rig's SCTP
  INIT-ACK — benign; runs 13-15's variant was the same slot, the
  capture now proves the class); `sctp_first: sid 0, ppi 53, len 6,
  "0e0202000300"` — **the rig talks to us: 0x020e = COMMAND_INPUT_
  VERSION (nvst_input.rs:31) on control_channel_reliable** (22
  messages total in the channels).
* **THE STALL: video flowed ~10.2 s (307 frames, RTP ts 695116→
  1612431 @90 kHz) then `progress_stall` → `recovery_timeout`;
  `kf_request {"sent":false,"note":"control channel is stage 3"}` —
  the exact documented cause: "Sends periodic SRTCP Receiver Reports
  so the peer keeps the video flowing … without any receiver feedback
  the server stops video after an initial burst" (nvst.rs:4320-4323).**
  The user Ctrl-C'd after 214 s (stop_reason signal);
  natt_keepalive 1945 kept the routing alive throughout.

**Round XXXVI delta — the reference's feedback sender set, all of it
through the existing kn_rx encoders (round XVI's KAT'd ports) over the
live SCTP channels:**
* **The feedback outbox** (main → pump thread): the media loop (the
  kn_rx single owner, D21 untouched) ENCODES; the pump thread (the
  SCTP single owner) SHIPS — a bounded 64×704 mutex queue, drop-new +
  counted (the kn_sctp tx-queue pattern one level up). Per-sid sender
  parameters mirror the channel profile (ordered flags + PR-SCTP:
  sid 4 RTX 2, sid 6/10 TTL 300 ms via the new `kn_sctp_send_pr` /
  SCTP_SENDV_SPA; the str0m configure_reliability sender-side half).
* **RTCP Receiver Report — sid 14 (rtcp_on_sctp_private), every 1 s**
  (SRTCP_RR_INTERVAL :97, the :8041-8060 block): NEW
  `kn_rx_rtcp_rr()` byte-exact with build_rtcp_receiver_report
  (:2983-2997 — 0x81/201/len7, sender ssrc **0x4F4E4F57 "ONOW"**
  (:96), 24-bit clamped cumulative loss, LSR=DLSR=0), fed by the NEW
  `kn_rx_video_rr_snapshot()` (the base_sequence anchor added to
  kn_rx_video, :1225 semantics; fraction = the reference's
  interval-delta arithmetic :1234-1247). 3 goldens from an independent
  Python transcription (negative loss, both clamps) — test_knrx +4 =
  **219**.
* **Frame ACK 0x0204 per assembled frame + pacing 0x0203 at the
  55 556-µs gate** (FRAME_PACING_INTERVAL, nvst_control.rs:18; the
  neutral-error rule of :7928-7933) — sid 6, hooked in
  dec_frame_in_cb; the rig's pending-ack window (MAX_PENDING_FRAME_
  ACKS 512, :106) can no longer starve.
* **QoS 0x0207 every 50 ms** (QOS_REPORT_INTERVAL :19) — sid 6, the
  kn_rx_qos snapshot fields filled from the RR snapshot; **D-t10: the
  estimator fields stay honest zeros and bw_estimate_kbps is the
  MEASURED downlink rate until the BWE stage.**
* **Control keepalive 0x0200 every 3 s** — sid 0, the byte-exact
  golden of the reference's own test (nvst_input.rs:1769:
  `control_keepalive(0) == 0002040000000000`).
* **The recovery senders now FLY**: `dec_take_kf` ships IDR 0x0302
  (sid 6) + RTCP PLI (sid 14, :8101) instead of the stage-2 "unsent"
  note (run 16's stall had exactly this gap); the NACK drain
  (kn_rx_nack_take's own 4 ms/52 ms/3-attempt policy) sends NACK-v2
  0x0317 (sid 6) + RTCP generic NACK (sid 14) per :3075-3078; RFI
  ranges (0x0301) ride sid 6 with restore-on-drop.
* **Bundle-socket media classification**: RTP/RTCP-looking datagrams
  on 49006 count as `bundle_rtp`/`bundle_rtcp` (the rig's audio —
  rtcAudioOnNativeBundle:1; the audio stage consumes them) instead of
  `invalid_in` (run 15's 175).
* Evidence: session_stats grows fb_rr/fb_ack/fb_qos; the summary grows
  the `feedback` block (rr/ack/pacing/qos/ka/nack/idr/pli/rfi/pushed/
  dropped); ice_preplay grows bundle_rtp/bundle_rtcp/fb_sent.

**What run 17 tests:** the SAME command — the stream must now SUSTAIN
(summary: wall_ms >> 10 s of frames, aus ≈ 30/s × seconds, fb_rr ≈
seconds, fb_ack ≈ aus, feedback.dropped 0). Watch: does the rig keep
sending past the 10 s mark (RR/ACK window satisfied)? Does it answer
our keepalive/ack flow with more control messages (sctp_msgs_in
grows)? If the stream still stalls: the kf_request now flies (IDR +
PLI) — a stall WITH sent keyframe requests points at the QoS/BWE
fields (D-t10) or the input activation chain (nvst_input.rs:1318-1328
— enable_input/device_descriptor/cursor/window/system/haptics — the
next fidelity candidate, deferred until measured). Presentation wiring
(frames → the G1-proven kn_d3d presenter = video ON SCREEN) is the
parallel track for G2.

---

## 24. Round XXXVII — R7 run 17: **the feedback hypothesis is DISPROVED** (the full sender set flew for 242 s and video still died at 10.99 s) → the input ACTIVATION CHAIN ships as the new prime suspect

Target first: `T10 VERDICT: ALL PASS (76 checks)` on the round-XXXVI tree,
zero warnings — the eighth consecutive clean target build. **After this
round the expectation is 78** (+1 for the `test_kninput` suite, +1 for the
`input.vectors` target-binary assertion).

Run 17 used the same command with `--session-seconds=300`; the user
Ctrl-C'd at 242 s because nothing was on screen ("Na web kliencie nie ma
sesji" — expected, the browser client was not in the loop).

### 24.1 Everything up to PLAY was at least as good as run 16

```
dtls_ready    ms:102  DTLSv1.2  TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256  verify:0  srtp_prof:1
sctp_up       ms:36                    (run 16: 52 ms)
sctp_channels opened:8 acked:8 rig_opened:0
PLAY          rc:0 status:200 attempt:1
handoff       AEAD_AES_256_GCM_8 tag 8 key 32 local, rtcp_on_sctp:1, unified:0, ping 6
```

New this run — the rig's **own SCTP INIT** was captured as the first DTLS
app record (`app_first_len:44`), which parses cleanly as a textbook
sctp-proto INIT and independently confirms our parameter choices:

| field | value | note |
|---|---|---|
| ports | 5000 → 5000 | RFC 8831 / our wire params |
| chunk | `01` INIT, length 30 | |
| initiate tag | `e2acdc88` | |
| a_rwnd | `0x00500000` = 5 242 880 | the rig advertises 5 MiB (we send 1 MiB — legal, it is a per-direction limit) |
| OS / MIS | 128 / 128 | we offer 65535/65535; the negotiated pair is the minimum, as always |
| initial TSN | `931b3eb2` | |
| param `0xC000` len 4 | **FORWARD-TSN-SUPPORTED** (RFC 3758) | the rig supports PR-SCTP — our per-sid TTL/RTX policies are meaningful |
| param `0x8008` len 6 → `82 c0` | **SUPPORTED-EXTENSIONS** (RFC 4805) listing chunk types `0x82` PAD and `0xC0` FORWARD-TSN | |
| trailer | `00 00` | even-length padding |

`app_records:25 / app_bytes:3984`, `sctp_out:17 / sctp_in:25`,
`sctp_first sid:0 ppi:53 len:6 "0e0202000300"`, `peer_closed:0`,
`post_first` again an epoch-1 application record (`17 fefd …`) — the
run-16 reading reconfirmed a third time.

### 24.2 The measurement that matters: feedback flew, video still died

```
summary: datagrams 3027, authenticated 3027, aus 331, frames 331, keyframes 10,
         frames_decoded 331, frames_failed 0, decode_errors 0, q_overflows 0,
         fec_repaired 4, stale 0, gaps 0, withheld 0, kf_requests 1,
         recv_timeouts 22711, recv_errors 0, wall_ms 242173,
         feedback { rr 242, ack 331, pacing 167, qos 4701, ka 81,
                    nack 0, idr 1, pli 1, rfi 0, pushed 5524, dropped 0 }
```

| quantity | value | reading |
|---|---|---|
| video span | ts 867936 → 1856990 = 989054 ticks = **10.99 s** | run 16: ~10.2 s |
| frame rate | 331 AUs / 10.99 s = **30.12 fps** | a clean 30 fps stream, then silence |
| RR coverage | 242 messages ≈ **242 s** at 1 s | flew the WHOLE session, including 231 s after the stall |
| QoS coverage | 4701 ≈ **235 s** at 50 ms | same |
| keepalive coverage | 81 ≈ **243 s** at 3 s | same |
| pacing coverage | 167 × 55.556 ms = **9.28 s** | frame-gated — stops with the video, exactly like the reference's (`take_completed_frame()` loop, nvst.rs:7913-7935) |
| ack | 331 = one per AU | correct |
| dropped | **0** | the outbox was never saturated |
| recovery | `kf_request sent:1`, `idr 1`, `pli 1` | **flew this time** (run 16: `sent:false`) — and the rig never answered |

Two conclusions, and they are the round:

1. **The round-XXXVI sender set is correct and was never starved.** RR,
   QoS and keepalive demonstrably covered all 242 s; the frame-gated
   pacing stopped *because* the video stopped, not before it.
2. **Receiver feedback is NOT what our rig is waiting for.** The stall
   moved 10.2 s → 10.99 s, i.e. not at all. `progress_stall` fired at
   `idle_ms:5002`, `recovery_timeout` at `idle_ms:5024`, and the IDR+PLI
   we then sent drew nothing — the rig had already torn the video down.

Side observation, deliberately **not** over-read: AU sizes fall monotonically
over the burst (18 356 → 13 912 → … → 4 378 → 2 946 → 2 862 B). That is
consistent both with a rate controller ramping down and with a static
desktop compressing better after its first keyframe; 10 keyframes in 331
AUs (~1 per 1.1 s) is a periodic-IDR pattern. No conclusion drawn — the
QoS estimator fields (D-t10) stay the secondary suspect.

### 24.3 The elimination, and what it left

The reference's complete control-plane inventory was swept for a
device-less session (docs/INPUT.md §2). Exactly one **startup** item was
missing from both runs 16 and 17:

**the eight-message input activation chain** — `enable_input(off)` →
`device_descriptor` → `cursor_capture(true)` → `cursor_tracking(true)` →
`window_state 19` → `system_state 0` → `enable_input(on)` →
`haptics(true)` — sent once, on sid 0, the instant the control channel is
open and `INPUT_VERSION` has arrived (nvst.rs:6554-6591 →
nvst_input.rs:1318-1329).

The reference documents the consequence of not sending the window state
itself (nvst_input.rs:1376-1381): an all-zero window state *"leaves the
session looking **inactive**"*. Run 17 gave us the version to negotiate —
`0e 02 02 00 03 00` parses as `0x020e` with a 2-byte payload, **input
protocol version 3** — and we never answered it, nor any of the rig's 20
control messages.

### 24.4 What round XXXVII ships

* **`kn_input.{h,c}`** — the control/input plane: TLV framing, all eight
  chain builders, the record-list walker, `input_protocol_version` with
  **both** reference fallbacks, cursor-notification detection, a bounded
  command histogram with a JSONL-safe serializer, and the full
  cursor-capture handoff machine (nvst_cursor.rs:3-126).
* **`tools/input_reference.py`** — the independent Python model. It
  self-proves against the reference's OWN byte-exact Rust goldens
  (nvst_input.rs:1768-1800, `timestamp_us = 20 102 193`) and its parser
  vectors (:1698-1712) **before** emitting
  `streamer/src/kn_input_kat_vectors.h`. The chain is therefore pinned by
  three implementations, not by our reading of the Rust. One real trap was
  caught this way: for `0e 02 ff 00 03` the reference returns **255**
  (fallback #1 reads `data[2..4]` verbatim), not the command code.
* **`test_kninput`** — 446 checks: 8/8 chain goldens + 11 singles,
  7 parser vectors (including run 17's live bytes), 7 cursor-notification
  vectors, all **five** reference cursor tests replayed as C command
  sequences, buffer/NULL contracts, and an every-truncation sweep of every
  golden (ASan-clean).
* **Pump-thread wiring** (`sess_ctrl_*` in knvstd_main.c) — the handshake
  latch, the chain, the cursor machine, and the evidence set that closes
  run 17's blind spot: `ctrl_msg` (the first 8 inbound messages verbatim
  with named codes), `input_version`, `input_activation`,
  `cursor_handoff`, `ctrl_in` (30 s histogram beat) and `ctrl_summary`.
* Deviations **D-t11** (unbatched control records — wire-compatible, the
  reference batches) and **D-t12** (the chain retry is bounded at 8
  attempts instead of unbounded) are recorded in docs/INPUT.md §7.
* **NEW `input.vectors` self-test** (48 checks, phase A, gated) — the §7.9
  discipline: the SHIPPED `knvstd` binary re-proves the same goldens, so
  the target run verifies the builders on Win7 and not only in the
  sandbox. The registry goes **14 → 15**; `test_knselftest` (117 checks)
  and the T10 harness counts follow, and T10 now asserts
  `input.vectors ok in the target binary`.

Gates: native **17** suites PASS (test_kninput **446**, test_knselftest
**117**) · cross ×2 PASS zero warnings · ctest **19/19 ×2** backends ·
ASan+UBSan+LSan clean on both (zero diagnostics from `streamer/` code) ·
OpenSSL interop ×2 legs PASS · E2E **43/43** · T10 POSIX dry **68** ·
T13 **63** · paranoid knvstd ×9 (−O0/−O2/−O3 × bare/mbedTLS/usrsctp) +
kn_input ×3 clean.

> Pre-existing, NOT this round's and NOT gated: `-Wformat-truncation=2` over
> `kn_selftest.c` flags `st_decode_synthetic` (:871) and `st_net_tls`
> (:1622). The paranoid gate covers the knvstd legs and T10 does not use
> `=2`, so the target build is unaffected; recorded in INTEGRATION.md rev 27
> so extending the gate later is not a surprise.

### 24.5 What run 18 decides

Same command, `--session-seconds=180`. The new evidence lines answer it
directly:

* **Video sustains** (aus ≈ 30/s × seconds, no `progress_stall`) →
  **stage 2 is done**; the activation chain was the gate, and the
  presentation wiring (frames → the G1-proven `kn_d3d_vp` presenter =
  G2 video ON SCREEN) becomes the main track.
* **`input_activation sent:8 failed:0 ready:1` but the stall persists at
  ~11 s** → the activation hypothesis dies too, and the suspects narrow to
  the QoS estimator fields (D-t10 / `nvst_bandwidth.rs` — the server PID
  reads them, nvst.rs:7924-7927) and whatever `ctrl_summary.codes` shows
  the rig asking for that we still ignore. **Paste `ctrl_summary` and the
  `ctrl_msg` lines — that is the whole point of this round's
  instrumentation.**
* **`cursor` reaching `local` via `cursor-notification`** → the rig's
  ServerControl path answered us, which is itself new information (run 17
  never proved the rig reacts to anything we send on sid 0).
* **`version:-1`** → the rig withheld `0x020e` this time; the chain cannot
  fire and the handshake timeout gap (docs/INPUT.md G1) becomes relevant.

Paste: `dtls_ready`, `sctp_up`, `sctp_channels`, `PLAY`, the `ctrl_msg`
lines, `input_version`, `input_activation`, one `ctrl_in`, `handoff`, the
FULL summary and `ctrl_summary`.

## 25. Round XXXVIII — R7 run 18: **the activation chain WORKED and the stall moved 11 s → 30.5 s** (562 AUs, 13 keyframes, 6665/6665 datagrams authenticated, zero decode errors) → the AU frame-size decay names the next gate: **the server's rate control is ramping us down because our QoS 0x0207 carries ZERO bandwidth-estimator fields** (D-t10), so the estimator ports and the whole evidence set ships

### 25.1 What run 18 proved

* **Activation closed.** `input_activation sent:8 failed:0 ready:1` on the
  first attempt; the rig's `0x020e` version 3 was negotiated exactly as
  round XXXVII designed. The stall moved from 10.99 s (run 17) to
  **30.5 s** — the first time ANY change we made moved the number. The
  session then ran the full 300 s budget (Ctrl-C at 5 min was the
  operator, not the pipeline).
* **The media plane is clean.** 562 AUs / 13 keyframes / 30.4 fps average
  over the live window, `6665/6665` datagrams authenticated,
  `decode_errors 0`, `fec_repaired` working. kn_rx has now survived three
  consecutive live rigs without a single pipeline fault.
* **SCTP died GRACEFULLY, not silently.** `sctp_fail:0` with the assoc
  reaching SHUTDOWN_COMPLETE — the round-XXXVIII cause-code work turns
  that from an inference into a printed fact on the next run.
* **Feedback still flew the whole session** — `rr:258, ack:562,
  pacing:298, qos:4932, ka:86, idr:1, pli:1` — but `pushed:6138,
  dropped:890`: the 64-slot outbox overflowed under one-message-per-record
  cadence. The reference does not send one message per record; it batches
  (below).

### 25.2 The root cause the log named: AU frame-size decay

The per-AU sizes in the truncated log decay steadily across the live
window while packet loss stays zero and RTT stays flat. That is not a
network symptom — it is the server's rate controller *ramping down*, and
the reference documents exactly what drives it: the server PID reads the
client's bandwidth-estimator fields out of QoS report `0x0207`
(nvst.rs:7924-7927 → nvst_control.rs:133-156). Ours were all zero — the
`kn_rx_qos` struct carried the seven BWE fields but nothing ever computed
them (deviation **D-t10**, open since round XXII). A GFN server seeing
`estimate_kbps:0, utilization:0` forever concludes the client measures
nothing and decays the encoder toward its floor. **The BWE port is the
primary fix of this round.**

### 25.3 What round XXXVIII ships (all target-verified pending)

1. **`kn_bw.{h,c}` — the BandwidthEstimator + server-control decoder,
   ported line-for-line from `nvst_bandwidth.rs`** (commit a49b124).
   Integer-ns clock (the float-clock trap is recorded in PROJECT-STATE),
   wrap-extended 90 kHz sender time, transit-min/jitter-EWMA/16-entry
   ring, the lossy-frame 60-frame 0.5 ms bump, the frame-ms EMA
   (.875/.125), `received_ms = max(dur − sender_gap, 0.5)`, utilization
   clamp, the report zero-gate (`first_arrival && min && count`) and the
   **100-observed-frame gate** before real estimates fly. NEW
   `tools/bw_reference.py` is the independent Python model; it self-proves
   against the reference's own Rust goldens before emitting
   `kn_bw_kat_vectors.h`, so the port is pinned by TWO implementations +
   the Rust test truths (minimum_server_time=270090, estimate_kbps=4800,
   utilization=5, queue_delay_us=1000, reorder lossy=2, parity
   receive_kbps=6400.0). NEW `test_knbw`: **387 checks**.
2. **The feed + the QoS rewrite.** `kn_rx_video_process_datagram` feeds
   the estimator on the authenticated-datagram path only
   (`feed_bw=1`; the plaintext process_packet path feeds 0 — FINAL
   decision, do not relitigate), at the reference's exact site: after
   resolve_nack, before FEC/reorder, tuple only when
   `!retransmitted && !reordered` AND RTP+GS parse; ssrc change resets;
   parity packets take `observe_parity`. `qos_report_at` now fills ALL
   SEVEN BWE fields with the reference's semantics (sender_frame = last
   COMPLETED frame index, bytes_received = CUMULATIVE completed bytes,
   loss = guarded DELTA snapshot, `client_time_90khz` = elapsed×90
   wrapping since transport origin, both constant-1000 u16s at @30/@32).
   The clock arrives via `cfg.now_ns` (µs socket clock ×1000) because
   sub-ms arrival granularity is load-bearing (ms-granular arrivals clamp
   to the 0.5 ms floor and double the receive-rate error).
3. **`ControlReportBatch` (nvst.rs:455-560 → `sess_cb_*`).** MAX 1071 =
   MAX_CONTROL_REPORT_BYTES; `queued_at` set on first record; flush_due =
   QoS present OR age ≥ 50 ms (QOS_REPORT_INTERVAL); one flush = ONE
   outbox push on sid 6; failure RETAINS the buffer. Frame-ack + pacing
   records append; QoS appends + flushes immediately; the media loop
   age-flushes each pass. Outbox maxlen raised 704 → 1071 to carry a
   full batch. This is the structural fix for run 18's `dropped:890`:
   ~1 message/50 ms instead of ~1 message/record.
4. **Per-sid outbox evidence.** `pushed_per_sid[8]` / `dropped_per_sid[8]`
   (sid/2 slots) + `refused`, surfaced in the summary's
   `"outbox_by_sid"` object — the next run NAMES the channel that drops.
5. **Server-control decoders live in the pump** (`kn_sctl_walk` from
   kn_bw.c, decoder table FINAL from nvst_server_control.rs:107-146):
   0x104/0x105 idle-timer, 0x109 termination (wire-code split
   0x80030023→(0x38,0x80030007), 0x80040010→(0x37,0x80040010)), 0x112
   frame-pacing latch (stream 0, mode 1 = enabled; default ENABLED per
   nvst.rs:664). The pump mirrors two VOLATILE latches to the media loop:
   `sctl_termination` ends the session with
   `stop_reason:"server_termination"` (the reference's
   `input_ready=false; break`), `sctl_pacing_enabled` gates the 0x0203
   pacing reports — a server that disabled pacing no longer gets PID
   feedback it explicitly switched off. Diagnostics counters + a 30 s
   `sctl_stats` line mirror the reference's 10 s nvst-control line.
6. **SCTP shutdown cause codes** (kn_sctp): assoc_state tracking +
   `kn_sctp_assoc_state_name`, so `sctp_down` prints SHUTDOWN_COMPLETE vs
   COMM_LOST vs the rest — run 18's graceful-death inference becomes a
   logged fact.
7. **AU wall-vs-RTP-ts telemetry** (dec_ctx): `drift_ms = wall_elapsed −
   rtp_ts_delta/90` per AU with min/max + `ts_backwards` counter, in the
   summary's `"au_clock"` block. If the rig's 90 kHz clock and our wall
   ever disagree (the truncated-log ts anomaly we were told not to chase
   without clean data), this measures it directly instead of guessing.
8. **R3-D: the WSS control keepalive.** `kn_ws` now captures pong
   payloads (`kn_ws_take_pong`, cursor-based consume-once; 5 new
   test_knws checks) and the media loop services the reference's
   control-ping worker semantics (nvst_rtsp.rs:844-935): 2 s interval,
   u64-BE sequence as ping payload, pong match records the RTT sample,
   5 s sample expiry, outstanding-ping expiry at the interval, bounded
   1 ms drain per 50 ms service beat, hard error latches `dead`. Evidence:
   `cping_*` fields on the 1 s session_stats beat + a `ctrl_ping_stop`
   line at teardown. Run 18's WSS went silent after PLAY with nothing
   noticing; the next run says whether the control channel lived.
9. **The summary grows four blocks** — `"batch"`, `"outbox_by_sid"`,
   `"bwe"` (estimator report + feed counters; `frames_observed < 100`
   would mean the zeros are the reference's own gate, not a broken port),
   `"au_clock"` — APPENDED after `"feedback"`, so
   `make_rx_capture.py --verify` (which consumes the legacy prefix) is
   untouched. JSON shape re-validated by extraction this round.

### 25.4 What run 19 decides

Same command, `--session-seconds=300`, log WITHOUT the ~76-char truncation
if the terminal allows (or `> run19.log`).

* **Video sustains** (aus ≈ 30/s × 300, no decay in the AU sizes,
  `bwe.estimate_kbps > 0` after frame 100) → **D-t10 was the gate;
  stage 2 is DONE** and the presentation wiring becomes the main track.
* **Stall persists but `bwe.frames_observed ≥ 100` and
  `estimate_kbps > 0`** → the estimator flew and the server still ramped
  down: paste the `bwe` block + one `session_stats` line; the suspect
  becomes the QoS wire values themselves (compare against the reference's
  field-by-field) or the `utilization` gate.
* **`bwe.frames_observed < 100`** → the feed is starved; the
  `skip_*` counters in the same block say where (reordered /
  retransmitted / no-video-parse).
* **`dropped` still > 0** → `outbox_by_sid` now names the channel; if it
  is sid 6 with the batch active, the batch flush cadence (50 ms) is
  wrong somewhere.
* **`stop_reason:"server_termination"`** → the rig sent 0x109; the
  `sctl_stats` + `term_*` fields say which wire code and whether it
  claims resumable.
* **`cping_dead:1` or `cping_expired` growing** → the WSS control channel
  died mid-session; correlate its timestamp with the video stall.

Paste: `handoff`, one `session_stats` line (mid-session), `sctl_stats`,
`ctrl_ping_stop`, `ctrl_summary`, the FULL summary (all four new blocks),
and the `sctp_down`/`ice_summary` lines.

---

## 26. Round XXXIX — R7 run 20: **the round-XXXVIII evidence set flew and PROVED the feedback plane healthy — and exposed a much harsher server behaviour: video was drip-fed from the FIRST SECOND** (40 AUs in 400 s = 1.275 s of media time, ~5 s burst cadence, then ONE gap → RECOVERY_REQUIRED until the end, 42 unanswered IDR/PLI) → STAGE 2 REMAINS OPEN; the decisive evidence (the 1 Hz estimator evolution) lives in the knlog `logs/` files, which run 20's stdout-only capture lacks. Target T10 this round: **ALL PASS 79** (test_knbw = 387 checks on Win7 = sandbox). The repo was reorganized to its final layout and the `outbox_by_sid` JSON-braces bug (caught in this very log) was fixed.

### 26.1 What run 20 proved (the instrumentation round XXXVIII shipped — every block arrived)

* **Target verification of round XXXVIII**: T10 ALL PASS 79 on Win7,
  zero warnings; `test_knbw` 387 checks on target = sandbox (the
  estimator port is byte-identical truth on both).
* **`batch`**: flushes 796, records 876, acks_sent 40, qos_sent 796,
  by_cause {qos:796}, **failed 0, retained 0, bytes_pending 0** — the
  ControlReportBatch cadence (one sid-6 message per ≤1071 B / ≤50 ms)
  works live.
* **`outbox_by_sid`**: sid0 [133,0] (keepalive) · sid6 [839,0] (control
  reports) · sid14 [441,0] (RTCP feedback) — **zero drops on every
  channel**. Run 18's `dropped:890` is fixed and proven fixed. (The
  block's JSON was missing its braces — the whole summary line was
  unparseable; fixed this round, §26.5.)
* **`sctl_stats`**: terminations 0, pacing_enabled 1 the whole session
  (no 0x0112 ever arrived), malformed 0, invalid_stream 0, timers 0,
  idle_warning 0, unknown 1 (0x020E), unhandled 405 ≈ the server's 1 Hz
  heartbeat family (0x0101). The reference decodes 0x0101 as
  `Unhandled` too (`nvst_server_control.rs:197`) — our no-op is
  reference-faithful, NOT a missing responder. (Run 18 saw the same code
  ×8; run 20 ×392 — the server's own state differs, cause unknown,
  benign per the reference.)
* **`ctrl_ping_stop`**: pings 198, **pongs 0**, expired 197, dead 0 —
  this GFN beta rig never answers WSS-level pings. R3-D's expiry path
  exercised for 400 s without a false kill (`dead:0` — the hard-error
  rule works); RTT sampling is simply impossible against this server.
* **`au_clock`**: wall 392.8 s vs rtp 1.275 s, drift 312 s,
  ts_backwards 0 — the drift number IS the starvation, quantified.
* Signalling/handshake chain: perfect again (queue 164→seat, 19 control
  relearns, RTSP 5×200, DTLS 414 ms, sctp_up 131 ms, activation 8/0/1,
  cursor handoff 1, NATT keepalives 796).

### 26.2 The failure shape — and how it differs from run 18

| | run 18 (round XXXVII code) | run 20 (round XXXVIII code) |
|---|---|---|
| session | 300 s, stop time | 400 s, stop_reason "time" |
| datagrams | 6665 / 6665 authenticated | 795 total, 658 authenticated, **137 dropped reason 3** (RECOVERY_REQUIRED) |
| AUs / keyframes | 562 / 13 | **40 / 1** |
| decode errors | 0 | 0 (all 40 decoded, libavcodec 63.2.100, 1280×720) |
| feedback pushed/dropped | 6138 / **890** | 1413 / **0** |
| QoS 0x0207 | broken layout, zero fields | reference layout, real estimator fields (warm-up zeros) |
| shape | steady flow decaying over ~30 s | **drip from second zero**: 41 stall/resume cycles (idle ≈5.0–5.1 s), 1–3 AUs per burst |

Timeline (log line numbers): first `progress_stall` (idle 5018 ms) at
line 81 — BEFORE the first AU (line 84, ts 1504245 = 16.7 s into the
server's media clock). Burst-per-5-s cadence through line 242 (last AU,
fi 40). Lines 244–245: `recovery_gap` + `progress_recovery` — ONE
unrepaired sequence gap flips kn_rx to RECOVERY_REQUIRED. Lines 246–388:
137 consecutive `dropped reason 3` — every later datagram discarded
pending a keyframe that never comes (kf_requests 42, idr 42, pli 42,
nack 2; keyframes total: 1 — the very first AU).

**`bwe` block**: frames_observed **40**, samples 536, parity 122,
**skip_reordered / skip_retransmitted / skip_no_video ALL 0**,
estimate_kbps 0, utilization 0, lossy_frames 1, min_server_time 37.5 s,
median_server_time 8.3 s, queue_delay_us **329 s**, jitter_us **65535**
(saturated). Reads: the feed path is FULLY healthy — every completed AU
reached the estimator; 40 < 100 so the zero estimate is the reference's
OWN warm-up gate (`nvst.rs:1068-1069`), not a port bug. The pathological
queue_delay/jitter are the mathematically faithful output of 5-s
inter-frame gaps — the reference never sees inputs like these because a
real client never gets drip-fed.

### 26.3 Verdict (matrix §25.4) and hypotheses

**Branch: `frames_observed < 100`** — but with all `skip_*` at zero the
starvation is UPSTREAM (the source), not in the feed. **STAGE 2 IS NOT
DONE.** The chicken-and-egg is now concrete: the estimate stays 0 until
100 frames arrive; the server never lets 100 frames arrive.

* **H1 (prime): our now-CORRECT warm-up feedback triggers immediate
  throttling.** Run 18's broken 0x0207 (garbage layout) bought 562 AUs;
  run 20's reference-layout 0x0207 (all-zero estimator fields at
  reference cadence, plus stall-driven RR/IDR/PLI) bought 40. A real
  reference client also starts at estimate 0 and gets fed — so plain
  zeroes cannot be the whole story. Suspects, in order: (a) the
  stall-driven IDR/PLI cadence (42 requests; the reference's recovery
  policy cadence is UNREAD — `nvst_recovery_policy.rs` exists and has
  not been ported/compared); (b) RR fraction_lost/jitter during the
  first stall seconds; (c) after the first burst: queue_delay 329 s /
  jitter 65535 going out in 0x0207 raw — any rate controller reading
  those would pin the encoder to the floor.
* **H2: rig/PoP variance** (run 20 peer 80.250.97.38, Warsaw; 19
  control relearns). One run cannot exclude it; run 21 on a different
  seat partially controls for it.
* **The missing evidence**: the 1 Hz `session_stats` lines (bwe
  evolution, cping_*, fb counters) go through knlog to `--log-dir`
  (default `logs/`) — NOT stdout. run20.log (stdout only) therefore
  cannot answer whether the pathological fields preceded or followed the
  throttle. **The `logs/` directory from the run-20 machine is the
  decisive artifact.**

### 26.4 Run 21 plan (decisive evidence)

1. Recover the run-20 `logs/` directory from the Win7 machine (knlog
   channel files for that session) — no new run needed for this.
2. Round XL (sandbox) before any new run: read the reference recovery
   policy (`nvst_recovery_policy.rs` + the nvst.rs stall/IDR path) and
   compare cadence; re-audit the hand-written 0x0207 SENDER mapping in
   `knvstd_main.c` against `nvst_control.rs:133-156` for the warm-up
   case (what exactly goes out while frames_observed < 100, and whether
   the reference clamps queue_delay/jitter anywhere we missed); audit
   the RR contents we sent during stall seconds.
3. New run: `--session-seconds=180` (the shape is decided in the first
   60 s), untruncated stdout (`> run21.log`) **plus the whole `logs/`
   directory afterwards**.

### 26.5 What else shipped this round (no behaviour changes beyond the JSON fix)

* **The repo reorganization** (root = README.md + START-HERE.md;
  `docs/archive/`; logs/ + build/ conventions; the tracked
  `build/ffmpeg/` tree; README acknowledgements) — full table in
  `docs/INTEGRATION.md` rev 29.
* **Fixed: summary `outbox_by_sid` JSON braces** — run 20's summary line
  was `"outbox_by_sid":"0":[133,0],…` (unparseable). The format now
  wraps the `%s` entries in `{}`; the empty-buffer case yields `{}`
  instead of `{{}}`. Trap recorded (START-HERE §8.5): eyeball +
  `json.loads` a REAL emitted summary once per round, not just the
  format skeleton.

## 27. Round XL — R7 run 21: **THE RUN-18→20 REGRESSION IS FOUND: Windows `SO_RCVTIMEO` has ~500 ms granularity, and the round-XXXVIII WSS control drain (R3-D) called it with "1 ms" from the media loop — every tick blocked ~500 ms, pinning the whole loop to the server's 2 Hz datagram cadence** (QoS "20/s" actually flew at 2/s; `recv_timeouts=1` against a 10 ms poll was the paradox that named it). Fixed with a `select()`-based bounded recv; the D1–D7 reference-fidelity divergences (reliable IDR + 100 ms retry, real RR jitter, frame-ack `first_packet_time_ms`, standalone pacing, NACK mode split, RFI channel) ship with it, plus `loop_ticks` telemetry and a build stamp so run 22's log states its own tick rate and binary identity.

### 27.1 Run 21 evidence (300 s, peer 185.136.70.137:5004; stdout + `logs/` — both provided this time)

* Handoff: `unified_socket=0`, `rtcp_on_sctp=1`, ping_version 6 ⇒ **non-mjolnir**
  (the RTCP video-feedback plane is open: RR/NACK/PLI on sid 14 apply).
* Transport: **596/596 datagrams authenticated**, 37 AUs (1 keyframe),
  `frames_decoded` 37, gaps 0, 1280×720. The session SURVIVED the full
  300 s (run 20 died at its single lost one-shot IDR; here the first
  keyframe completed at t≈7 s and no gap ever re-armed the latch).
* The drip: **2.000 datagrams/s from the first second** (t=0.5 s:
  datagrams=2), the first keyframe paced in at ~2 packets/s over ~7 s.
  A 500 ms-quantized metronome = the server's idle-heartbeat timer, not
  congestion. The user attached via the web client during the run; the
  media clock says the game screen was STATIC: `rtp_ms` **1182** over
  300 s wall (the encoder emitted 1.18 s of media), and
  `min_server_time` 27882167 ticks ≈ 309.8 s of media clock at handoff
  ⇒ the game had been idle ~11 s before our attach.
* Estimator: samples 486, parity 110, estimate 0 — `frames_observed`
  37 < the reference's 100-frame warm-up gate (nvst.rs — the zero is
  the reference's own honest warm-up zero, not a port bug);
  `queue_delay_us` 298015317 (≈ the whole session — arrivals spread
  over 300 s against a 1.18 s media clock), jitter 65535 (clamped),
  lossy 1.
* Feedback actually pushed: rr 298 / ack 37 / pacing 37 / **qos 597** /
  ka 100 / nack 0 / idr 37 / pli 37 / pushed 1069 / **dropped 0**;
  batch flushes 597, records 671; outbox sid0[100,0] sid6[634,0]
  sid14[335,0] — the sender machinery itself is healthy (round
  XXXIX's proof, reconfirmed).
* Control keepalive (R3-D WSS): cping **149 sent / 0 pongs / 148
  expired** — the server answered NO WebSocket ping all session. We do
  not enforce expiry (correctly: the endpoint evidently ignores WS
  pings), but run 22 should watch whether pongs appear once the loop
  can actually service the 2 s beat on time.
* **`recv_timeouts = 1`.** One. In 300 s of a 10 ms-polled loop. This
  single number is the loudest line in the whole log (see §27.2).

### 27.2 The loop-tick forensics (run 21 ≡ run 20, and run 18 is the control)

Every periodic counter in run 21 is phase-locked to the datagram rate:

| counter (gate) | observed /300 s | 100 Hz tick predicts | 2 Hz tick predicts |
|---|---|---|---|
| QoS 0x0207 (50 ms) | **597** | ~6000 | ~600 ✓ |
| NATT keepalive (100 ms) | **597** | ~3000 | ~600 ✓ |
| RR (1 s) | 298 | ~300 | ~300 ✓ |
| session_stats (1 s) | 298 | ~300 | ~300 ✓ |
| cping beat (2 s) | 149 | ~150 | ~150 ✓ |
| KA 3 s | 100 | ~100 | ~100 ✓ |
| `recv_timeouts` | **1** | **~29 400** | ~0–2 ✓ |
| loop iterations | **597 = datagrams+1** | ~30 000 | ~597 ✓ |

One loop iteration per received datagram, plus one. Run 20 (400 s) shows
the identical signature (qos 796 = datagrams 795 + 1, `recv_timeouts`
1). Run 18 (round XXXVII code) is the control: QoS **4932/300 s ≈
16.4/s** — the 10 ms tick demonstrably WORKED before round XXXVIII.
The regression therefore entered in XXXVIII, and exactly one XXXVIII
change touches the media loop: **R3-D** — `sess_ctrl_ping_service`
drains the WSS control socket via `kn_ws_recv_msg(ws, 1 ms)` →
`ws_fill` → `tls_recv` (knvstd_main.c) → `kn_net_tls_stream_recv` on
every pass of the loop's 50 ms service beat.

### 27.3 Root cause: `SO_RCVTIMEO` is NOT millisecond-accurate on Windows

The production TLS recv body (Win32 + mbedTLS — `kn_net_mbedtls.c`;
`kn_net.c` carries only the honest stub for `!KN_TLS_BACKEND_MBEDTLS`
builds, and the cmake default/target backend is mbedTLS, CMakeLists
:91) did:

```c
io_timeout = timeout_ms ? timeout_ms : KN_NET_IO_TIMEOUT_MS;
setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, ...);
ret = mbedtls_ssl_read(&s->ssl, buf, cap);   /* blocks in the BIO recv */
```

with the BIO registered as `mbedtls_ssl_set_bio(&s->ssl, &s->sock,
kn_ssl_send, kn_ssl_recv, NULL)` — **no `f_recv_timeout` callback**, so
`SO_RCVTIMEO` as seen through `kn_ssl_recv`'s error mapping was the
ONLY bound on the blocking `recv()`. And on Windows, winsock rounds
`SO_RCVTIMEO` to the AFD driver's poll granularity — **~500 ms on
every Windows since NT** (a documented, notorious pitfall; `select()`
is ms-accurate, `SO_RCVTIMEO` is not). The "1 ms" drain therefore
blocked **~500 ms per call**.

The closed loop then explains EVERY observed number:

1. The drain blocks ~500 ms (SO_RCVTIMEO granularity).
2. The server's next datagram (502 ms cadence) arrives during that
   block and is already pending in the socket buffer.
3. The loop's `kn_sock_udp_recv` `select(10 ms)` returns **readable
   immediately** — hence `recv_timeouts = 1` (a single phase slip in
   300 s; 502 vs 500 ms drifts slowly), NOT the ~29 400 a live 10 ms
   tick would produce.
4. The tick period = the datagram period ⇒ every sub-500 ms gate fires
   exactly once per datagram (QoS 597, NATT 597), every ≥1 s gate
   fires on its integer divisor (RR 298, cping 149).

`kn_sock.c` was audited line-by-line this round and is CORRECT (QPC
clock, proper `select` timeouts) — the UDP path was never the problem;
do not touch it.

**Consequence for the drip:** the server never saw a live client. The
reference answers QoS at 20/s (50 ms); we flew at 2/s. Frame acks,
pacing and RR cadences were 10× off. Whatever ramp policy GFN's rate
control runs, its input signal said "barely-alive client" from the
first second — and the floor-rate idle heartbeat is what it kept us
on. (The static screen is a SECOND, independent depressant: an idle
GFN encoder emits almost nothing regardless of feedback — `rtp_ms`
1182 proves the content itself was frozen. Run 22 must remove BOTH
confounds: fixed cadence AND an active game.)

### 27.4 The fix (kn_net_mbedtls.c — `kn_net_tls_stream_recv`)

* Wait with **`select()` on the raw TCP socket** (ms-accurate on
  winsock) BEFORE calling `mbedtls_ssl_read`; `SO_RCVTIMEO` stays set
  as the belt-and-suspenders bound for a partial TLS record already in
  flight.
* `mbedtls_ssl_check_pending()` guard: buffered plaintext (e.g. a
  second WebSocket frame inside one TLS record) is read WITHOUT
  touching the socket — no busy-wait, no spurious block.
* Return semantics UNCHANGED: `>0` bytes, `0` retryable timeout /
  WANT_READ, `-2` EOF / close_notify / conn-reset, `-1` fatal. Callers
  (`tls_recv`, `sess_ws_recv_text`, the kn_ws fill path) needed no
  changes.
* Verified where verifiable in the sandbox: cross-compiled zero-warning
  for BOTH arches under `-DKN_TLS_BACKEND_MBEDTLS=1` (the Win32 body
  only compiles there); runtime proof is target-only by construction —
  run 22's `loop_ticks` (~100/s expected) is the direct witness.

### 27.5 The reference-fidelity divergences D1–D7 (audited after run 21; ALL fixed this round)

* **D1 — IDR channel:** `0x0302` now rides **sid 0 (reliable ordered —
  the reference's `send_control`, nvst.rs:3095-3114)**. Run 20 died
  sending it ONE-SHOT on sid 6 (partial-reliable unordered, TTL
  300 ms): one lost datagram = a keyframe request that never arrives.
* **D2 — RFI channel:** frame-invalidation → sid 0 (same reference
  path).
* **D3 — the retry loop:** while kn_rx's keyframe latch is pending
  (`kn_rx_video_keyframe_request_pending` — armed by any loss/stall
  path, cleared ONLY by an assembled keyframe), re-fire every
  **`SESS_IDR_RETRY_INTERVAL_MS` = 100 ms** (nvst.rs:104, :8092-8110):
  IDR on sid 0 PLUS a PLI on sid 14 when `rtcp_fb_open &&
  have_bound_ssrc` (`uses_rtcp_video_feedback`, :3116 +
  `keyframe_request_routes` :1272-1286). Failed pushes consume the
  interval too ("Bound failed sends as well as successful ones",
  :8090). Episodes log: first attempt, then every 50th; `kf_cleared`
  on latch release. `rtcp_fb_open = !unified_mode && desc.rtcp_on_sctp`
  is now initialized at session start (it gated code paths that read
  it uninitialized-on-paper — memset saved us, but the intent is now
  explicit).
* **D4 — RR jitter:** was hard-zero. kn_rx now ports the reference's
  RFC 3550 interarrival EWMA exactly (arrival RELATIVE to first
  packet ×90, `rtp_ticks = (u32)(ts - first_ts)` ZERO-EXTENDING wrap
  — the `i64::from(wrapping_sub)` gotcha, first packet latches only,
  `ts == last` skips, `/16` EWMA, clamp 0..u32::MAX, truncate) and
  `rr_snapshot` grew a 6th out-param; the RR is gated on
  `rtcp_fb_open`. Golden: the 4-frame/10 ms-step stream yields jitter
  **369** (hand-derived: 131.25 → 254.296875 → 369.60546875 →
  trunc).
* **D5 — NACK mode split:** `uses_rtcp_video_feedback` ? RTCP NACK on
  sid 14 : RTPFB NACK via the control plane — we dual-sent before
  (nvst.rs:3050-3092).
* **D6 — frame ack `first_packet_time_ms`:** kn_rx records the wall
  clock of the AU's FIRST packet at the frame boundary
  (`cur_au_first_ms`, the reference's `CompletedFrameFeedback`
  `timing.first_at`, nvst.rs:447-462) and emits it in `kn_rx_frame`;
  `dec_frame_in_cb` passes it (clamped ≥ 0 against
  `transport_origin_ms`) into `kn_rx_ctrl_frame_ack` — was 0/NULL.
* **D7 — pacing order & channel:** `0x0203` is pushed **standalone on
  sid 6 BEFORE the frame ack** (reference order, nvst.rs:7922-7944),
  55.556 ms gate, neutral error 0 — it was batched after the ack.

### 27.6 New evidence channels for run 22

* **C18 build stamp:** every RUN mode prints
  `{"mode":"build","compiled":"<__DATE__ __TIME__>","round":"XL"}` as
  its first stdout line (utility modes `--version/--list-selftests/
  --selftest/--diagnostics` are EXCLUDED — their stdout is a
  single-JSON-document contract the native gate validates). The
  stale-exe hypothesis can never eat a round again.
* **C19 `loop_ticks`:** raw media-loop iterations, emitted in the 1 Hz
  `session_stats` jsonl AND the final summary. Healthy ≈ 100/s;
  pathological = datagram-rate. This is the direct regression witness
  for §27.3.
* **F1 `sctl_010a` dump:** 0x010a was the most frequent Unknown in the
  run-18/21 server-control inventory, but `kn_sctl_msg` deliberately
  carries no payload pointer — so a self-contained TLV walker in
  `sess_ctrl_ingest` now re-walks the raw record buffer (u16 LE code +
  u16 LE len) and dumps every 0x010a payload as hex: ≤ 16 lines per
  session, ≤ 800 hex chars (line < 900). Run 22 finally says what the
  server puts in it.
* knrx suite +9 checks (203): the jitter golden, `first_packet_ms`
  per-frame wall-clock assertion, `rr_snapshot` pre-bind 0, and the
  pending-latch non-clearing contract D3 polls.

### 27.7 Sandbox gates (round XL, all green)

native 18/18 **2261** checks (knrx 203, knbw 387, kninput 446) ·
paranoid knvstd ×9 (c11/c17/gnu11 × pedantic / format-truncation=2 /
unused-but-set+shadow) clean · kn_bw/kn_rx ×3 clean · cross ×2 arches
zero warnings (incl. the `-DKN_TLS_BACKEND_MBEDTLS=1` legs that
compile the fixed TLS body for Win7) · ctest **20/20 mbedTLS + 20/20
schannel** · ASan+UBSan knbw 387 + knrx 203 clean ·
`sandbox_session_e2e` **43/43** · T10 POSIX dry **ALL PASS 69** · T13
**ALL PASS 63** · printf validation: all 99 JSON printf blocks
specs=args (session summary **71 = 71**) + strict-JSON skeleton
`json.loads` PASS.

### 27.8 Run 22 plan (target)

1. **CLEAN rebuild on Win7** (delete the build tree; full T10 — expect
   ALL PASS; the suite count grows with the new knrx checks).
2. `knvstd --session … --session-seconds=300`, untruncated stdout
   **PLUS the whole `logs/` directory** (the 1 Hz `session_stats` with
   `loop_ticks` lives only there).
3. **The user keeps the game ACTIVE at and after handoff** (input,
   motion — the run-21 media clock proved a static screen freezes the
   encoder independently of any client fix).
4. Pass criteria: build stamp `"round":"XL"` present; `loop_ticks`
   ≈ 100/s (NOT datagram-rate); `recv_timeouts` ≈ ticks − datagrams
   (tens of thousands); QoS ≈ 20/s; during motion, datagram rate ≫
   2/s and `rtp_ms` tracks wall time; `kf_request` episodes end in
   `kf_cleared`; `sctl_010a` lines decode the mystery payload; cping
   beat on its 2 s cadence (watch for pongs now that the drain cannot
   eat the timer).

## 28. Round XLI — R7 run 22: **THE LOOP FIX IS PROVEN AND THE FIRST SUSTAINED REAL-RATE GFN VIDEO FLOWED** (42 882 loop ticks = 143/s, `recv_timeouts` 27 095 = ticks − datagrams − 1 — the run-20/21 paradox INVERTED into the proof; QoS 19.5/s; 15 786/15 786 datagrams authenticated; 1139 AUs, 1013 of them a continuous 29.98 fps @ ~3.7 Mbps stream with ZERO media gaps; BWE left warm-up for the first time: estimate 25 398 kbps) — **then the server froze EVERY plane at ~t+30–45 s** (video, the ~1 Hz 0x0101 heartbeat, WSS pongs; no termination, no idle warning; the WSS itself only errored at ~t+216 s = TCP retransmit exhaustion) and never answered 2535 reliable IDR+PLI retries. The 0x010a mystery is DECODED (JSON `NvExtendedCommandMessage` app notifications). Run 22 also exposed our own dead end: `RECOVERY_REQUIRED` has no way back in knvstd — the reference's bounded recovery contract (`attempt_nvst_recovery`, limit 1, terminal `nvst-recovery-exhausted`) ships this round as **D8**.

### 28.1 Run 22 evidence (300 s, peer 80.250.98.39:5004, stdout only — the `logs/` dir was NOT provided this time)

* Handoff: `unified_socket=0`, `rtcp_on_sctp=1`, ping_version 6 ⇒ non-mjolnir
  (RTCP plane open — PLI/RR on sid 14 apply). SRTP `AEAD_AES_256_GCM_8`,
  key_id 2280930115, local key. DTLS 85 ms (fastest yet), sctp_up 17 ms,
  8/8 channels, nomination 15 sent. RTSP 5×200 incl. PLAY attempt 1.
  Activation 8/0/1 ready:1, input version 3, cursor local (3 enables,
  1 handoff). Queue 73→33→16→seat over 26 control relearns.
* Transport: **15 786/15 786 datagrams authenticated**, 1139 AUs =
  1139 frames = 1139 decoded, 9 keyframes, **0 decode errors, 0 gaps,
  0 stale, 0 withheld, drops {}**, fec_repaired 3, 1280×720,
  libavcodec 63.2.100.
* The loop (C19): **`loop_ticks` 42 882 = 143/s**, `recv_timeouts`
  **27 095 = 42 882 − 15 786 − 1** — the §27.2 pathology table inverted
  exactly as predicted. 27 095 × 10 ms ≈ 271 s of idle poll + ~29 s of
  datagram work = 300 s: the loop free-runs at the 10 ms floor and
  wakes immediately per datagram (~52.6 datagrams/s session-average;
  ~350/s during the streaming window).
* Feedback (all reference cadences, all delivered): rr **300** (1/s —
  run 21: 298 phase-locked), ack **1139** (per frame), pacing **539**
  (55.556 ms gate ≈ the 30 s streaming window), **qos 5861 = 19.54/s**
  (run 21: 597 = 2/s), ka 100 (3 s), NATT 2951 (~9.8/s, 100 ms gate),
  idr **2535** + pli **2535** (the D3 retry loop), pushed 11 870,
  **dropped 0**, pacing_disabled_skips 0. Batch: flushes 5861, records
  7000 (= acks 1139 + qos 5861), failed 0, retained 0, by_cause
  {qos:5861}. Outbox: sid0 [2635,0] (= idr 2535 + ka 100), sid6
  [6400,0] (= qos 5861 + pacing 539), sid14 [2835,0] (= pli 2535 +
  rr 300) — every channel accounted to the unit, zero drops.
* **BWE left warm-up for the first time in project history**:
  frames_observed **1136** ≥ 100 ⇒ `estimate_kbps` **25 398** (run 20:
  40 frames ⇒ honest zero; run 21: 37), utilization_pct 6,
  queue_delay_us 5270, jitter_us 2470, lossy_frames 2, samples 12 617,
  parity 3162, skip_reordered 7, skip_retransmitted/no_video 0.
* au_clock: **drift_ms −2** (min −7, max 393), ts_backwards 0, wall
  299 970 ms vs **rtp 37 922 ms** — during the streaming window the
  media clock tracked wall exactly (median inter-AU Δts 33.3 ms =
  29.98 fps, min 13.6, max 47.1, ZERO gaps > 1 s of media time); the
  session-wide 12.6 % ratio is entirely the post-freeze silence.
* Server control: 43 msgs / 43 records — 020e×1 (input version, via
  kn_input), 0111×2, **010a×9**, 0106×2, 010f×1, 010e×1, **0101×27**;
  every code is in the reference's Unhandled set
  (nvst_server_control.rs:196-198: `0x101..=0x103 | 0x106 |
  0x10a..=0x111 | 0x113 | 0x206 | 0x325 | 0x408`) — the no-op is
  reference-faithful. sctl_stats: terminations 0, timers 0,
  idle_warning 0, pacing_msgs 0 (pacing_enabled 1 throughout),
  malformed 0, unknown 1 (0x020e — expected, it belongs to kn_input).
* ctrl_ping_stop: pings **108**, pongs 0, expired 107, drained 0,
  **dead 1** — the 2 s beat held (108 × 2 s = 216 s ⇒ the WSS died at
  ~t+216 s; run 21: 149 pings over the full 300 s, dead 0). The rig
  still answers no WS ping. `drained:0` = not one text/binary frame
  arrived post-PLAY.

### 28.2 The 0x010a mystery is DECODED (F1 dumper, 9 payloads)

Every 0x010a payload is ASCII **JSON**: an `NvExtendedCommandMessage`
(dataType application/json) addressed to `Serenity` — the VM-side
AppInfo notification feed (`rtsp://Serenity/AppInfo/v.1.0/Notification`):

| n | event | content |
|---|---|---|
| 1 | foregroundWindow | processType 4, pid 5428, **fullscreen:true** |
| 2 | process action:2 | **Launcher.exe** pid 4128 JustLaunched (parent nvcontainer.exe 8276) |
| 3 | process action:2 | **Games.exe** pid 3932 JustLaunched, processType 1, shortName `107057…` (parent Launcher.exe) |
| 4 | process action:3 | Launcher.exe **exited, code 0** |
| 5 | process action:1 | processType 6, info "" |
| 6 | process action:2 | **QtWebEngineProcess.exe** pid 6860 (child of Games.exe) |
| 7 | foregroundWindow | processType 0, pid 0, fullscreen:**false** |
| 8 | foregroundWindow | processType 1 (Games.exe) fullscreen:**false** |
| 9 | SESSION_LENGTH_TIMER | → GFN_CLIENT_UI, OTel trace headers (session telemetry) |

So 0x010a = the server's app-lifecycle/foreground telemetry (the game
launch sequence on the VM, arriving pre-PLAY — 8 of the 9 within the
first 24 control messages). It is Unhandled in the reference and stays
Unhandled here; F1's job was to prove it carries no gating command, and
it did. Nothing in the feed asks the client for a response.

### 28.3 The media timeline (from AU `ts` — stdout had no wall stamps, the knlog `logs/` would have them)

* AU 1 (keyframe, 18 445 B) at media ts 10.53 s — the encoder clock
  started ~10.5 s BEFORE our media loop (the VM was already running
  the app; `min_server_time` 27 945 604/90 000 = 310.5 s vs the 300 s
  budget shows the same ~10.5 s pre-PLAY offset — run 21: 309.8 s).
* fi 2..1013: **continuous 29.98 fps**, sizes flat at ~15.2–15.7 KB
  median (~3.7 Mbps of real motion content — the game was ACTIVE),
  zero media-time gaps > 1 s. This is the first sustained real-rate
  GFN stream in 22 runs (run 18: 562 AUs decaying from the start;
  runs 20/21: 40/37 AUs drip-fed).
* fi 901..1013: size decay 15.5→7.3 KB median (content settling or
  the rate controller easing — indistinguishable client-side).
* fi 1014..1139 (media 44.29→48.45 s): the tail burst — 126 frames,
  **8 keyframes at exactly 16-frame (0.533 s media) spacing**, sizes
  2.9–7.8 KB: near-static content with a periodic refresh pattern.
  No client request caused it (all 2535 IDRs postdate AU 1139 — the
  log order is same-thread and decisive). Then: nothing, ever.
* The stall cascade: `progress_stall` idle 5006 ms (latch armed,
  kf episode 1 attempt 1) → `recovery_timeout` idle 5009 ms →
  RECOVERY_REQUIRED (final rx_state 2) → 51 logged attempts
  (1, 50, …, 2500; 2535 total = 253.5 s of 100 ms retries to session
  end, all sid 0 reliable + sid 14 PLI, zero outbox drops). The server
  never sent another datagram on ANY plane.

### 28.4 The server-side freeze — what the log proves and what it cannot

Everything the server owned stopped inside ~t+30..45 s: video (last
datagram ≈ t+41.5, from 2535 × 100 ms ending at t+300), the ~1 Hz
0x0101 heartbeat family (all 27 within the first two 30 s ctrl_in
snapshots; msgs frozen at 43 for snapshots 3–10 = t+60..300), and
eventually the WSS (dead at ~t+216 — consistent with TCP retransmit
exhaustion ~175 s after the peer went silent; SCTP never formally
went down within budget — usrsctp was still retransmitting, outbox
drops 0). NO protocol-level notice: terminations 0, idle_warning 0,
timers 0, no sctp_down, no RTSP teardown. The client side was
provably healthy throughout (§28.1 cadences).

Candidates the client log CANNOT separate (run 22 was stdout-only):
1. **VM/rig freeze or reclaim** on the beta fleet (everything stopped
   at once, silently — the strongest fit).
2. **Takeover/preference flip** if an official client (web) attached
   to the same session mid-run (run 21 precedent) — the server may
   move the media plane to the attaching client.
3. A server policy against clients that never send real input — but a
   polite idle kick would send the warning/timer/termination family
   first; none came, and 45 s is far below GFN's idle-kick horizon.

The knlog `logs/` directory (1 Hz `session_stats`: per-second
datagram/tick rates, the estimator ramp, cping beats) would pin the
freeze instant to the second and show whether the QoS estimate ramp
correlates with the size decay — **run 23 must include it** (the
stdout-only capture is the second time this bites: run 20 §26).

### 28.5 The dead end run 22 exposed: RECOVERY_REQUIRED has no way back (→ D8)

`kn_rx_video_process_datagram`/`process_packet` drop EVERY packet
while state != RUNNING (kn_rx.c:2569-2575/:2602-2608 — the reference's
own guards, nvst.rs:4633-4634/:4927-4928), and `kn_rx_video_recover()`
— ported and sitting in kn_rx.c:2769-2780 — was **never called by
anyone**. Once run 22 entered RECOVERY_REQUIRED at t+46.5 s, even a
keyframe answering our 2535 IDRs would have been dropped (run 20's
terminal shape: 137 drops reason 3 until the budget ran out).

The reference contract (opennow-streamer-core `lib.rs`):
`forward_nvst_event` maps `RecoveryNeeded(Timeout)` (:2287-2294) and
`Lifecycle(RecoveryRequired)` (:2296-2305) — but NOT `PacketGap`
("The receiver owns gap recovery", :2269-2273) — onto
`attempt_nvst_recovery` (:2448-2477): if `recovery_attempts >=
NVST_RECOVERY_ATTEMPT_LIMIT` (**1**, :127) → terminal
`"nvst-recovery-exhausted"` (the engine breaks; the app restarts the
whole pipeline); else attempts++, `cut_recording(Interrupted)`,
**`request_keyframe()`**, **`recover()`** (nvst.rs:4503-4515:
"Returns a typed timeout only once. The caller must deliberately call
`recover` before more media is accepted, preventing a stale stream
from silently resuming" — reset media, timeout origin = now, state →
Running, Lifecycle event). The decode-side watchdog
(`decode_progress.rs`, stall/keyframe_grace/recovery_grace = 5 s each)
feeds the SAME attempt path (:2079-2097) and arms a terminal deadline.

Run 22 under the reference contract: stall t+46.5 → timeout t+46.5 →
attempt 1 (recover → RUNNING, latch re-armed) → still silent →
timeout t+51.5 → **terminal at ~t+52** with a clean verdict, instead
of 248 s of dead air. That is what D8 ships.

### 28.6 Round XLI changes (D8 + the evidence set)

* **D8 — the bounded recovery contract** (`dec_recovery_attempt` in
  knvstd_main.c, wired into `dec_drain_events` — session mode only,
  `c->fb` gate; --decode/--replay-capture behaviour is untouched, so
  T13 is unaffected): first `RECOVERY_TIMEOUT`/`PROGRESS_RECOVERY` →
  `rx_recovery` line (`action:"recover"`) + `kn_rx_video_request_keyframe`
  + `kn_rx_video_recover`; second → `rx_recovery` (`action:"terminal"`)
  + the media loop breaks with **`stop_reason:"recovery_exhausted"`**
  (exit-code policy unchanged: the run still produced video, like
  `server_termination`). The action runs AFTER the print loop —
  `recover()` does `ev_reset` + `lifecycle_push`, which would rewrite
  the event array mid-iteration otherwise. `cut_recording` has no
  counterpart (--session does not record). A full session RESTART
  (the reference app's reconnect after terminal) is NOT part of knvstd
  — deferred, see §28.8.
* **`ctrl_ping_dead`** JSONL at both dead transitions (send_failed |
  recv+rc): run 22's WSS death moment was only inferable from the
  ping count; run 23 dates it directly.
* **Summary `recovery` block** `{"attempts":%u,"exhausted":%d}`
  appended after `au_clock` (71→73 specs=args, skeleton `json.loads`
  PASS — trap #5 ritual run in full).
* **Build stamp → `"round":"XLI"`** (C18 discipline: run 23's log
  states its own binary identity).
* **test_knrx 203 → 218**: the recovery state machine — the 250 ms
  timeout transition (249 ms: silent), RECOVERY_REQUIRED's drop guard
  (reason 3, decoder never sees the packet), `recover()` (→1, RUNNING,
  Lifecycle event, timeout origin re-anchored: 249 ms after recover is
  silent, 250 ms re-fires), the no-op contract from RUNNING, **the
  keyframe latch SURVIVING the reset** (the D3 retry loop's lifeline),
  and kn_rx-level recover being unlimited (the LIMIT lives in the
  session layer). `prop_h_init` grew a `_to` variant (timeout param).
* `tools/sandbox_session_e2e.c` header recipe fixed (+`kn_bw.c` — the
  round-XXXVIII link dep was missing from the comment).
* **D-t13 (new deviation, deferred)**: the reference's DECODE-side
  progress watchdog (`decode_progress.rs` — decoder-output stall with
  packets flowing → RecoveryNeeded → the same attempt path + the
  recovery_grace terminal deadline, lib.rs:2079-2117) is not ported;
  knvstd ships the packet-side contract only. No live run has ever
  shown decode-output starvation with healthy packet flow (every
  failure so far was packet-side), so the deferral is evidence-based.

### 28.7 Sandbox gates (round XLI, all green)

native 18/18 **2276** checks (knrx **218**, knbw 387, kninput 446) ·
cross ×2 arches zero warnings (incl. the `-DKN_TLS_BACKEND_MBEDTLS=1`
legs) · ctest **20/20 mbedTLS + 20/20 schannel** · paranoid knvstd ×9
(c11/c17/gnu11 × pedantic / format-truncation=2 / unused-but-set+shadow)
clean + kn_bw/kn_rx ×3 clean · ASan+UBSan knrx 218 + knbw 387 clean ·
`sandbox_session_e2e` **43/43** · T10 POSIX dry **ALL PASS 69** · T13
**ALL PASS 63** · summary-printf validation: all 107 JSON printf blocks
specs=args (session summary **73 = 73**) + strict-JSON skeleton
`json.loads` PASS · the four new stdout line shapes `json.loads` 4/4.

### 28.8 Run 23 plan (target) + the open questions

1. **User syncs the round-XLI files** (§INTEGRATION rev 31) and does a
   **CLEAN rebuild on Win7**: delete the build tree, run T10 in full.
   Expected verdict: **ALL PASS 79** with `test_knrx` reporting **218**
   checks (203 + 15).
2. Run 23 — `knvstd --session … --session-seconds=300`, untruncated
   stdout (`> run23.log`) **PLUS the whole `logs/` directory** (non-
   negotiable this round: the 1 Hz `session_stats` is the only record
   of the per-second freeze shape and the estimator ramp).
3. **The run-22 procedure questions must be answered before run 23**
   (they decide which §28.4 hypothesis run 23 tests): was the official
   WEB CLIENT attached to the run-22 session (and at what moment)?
   Which app/game was launched (the 0x010a feed shows
   Launcher.exe → Games.exe, shortName `107057…`)? How was "keep the
   game active" performed — through what input path?
4. Pass criteria (run 23): build stamp `"round":"XLI"`; if the server
   holds: sustained video, `rtp_ms` ≈ wall, kf episodes end in
   `kf_cleared`, `recovery.attempts` 0. If the server freezes again:
   `rx_recovery` attempt 1 (`action:"recover"`) ~5 s after the stall,
   `action:"terminal"` ~+5 s later, `stop_reason:"recovery_exhausted"`
   ~10–15 s after the freeze — a fast, clean, self-labelling verdict;
   `ctrl_ping_dead` dates the WSS death. A freeze surviving a RESTART
   of knvstd (fresh session, fresh seat) would separate rig-flake from
   session-specific policy; if the web client was attached in run 22,
   run 23 WITHOUT it separates takeover from freeze.
5. Deferred, on evidence: the session restart loop after terminal
   (the reference app's reconnect); synthetic input (mouse jiggle) —
   only if run 23 + the answers implicate idle policy; the D-t13
   decode-side watchdog.

## 29. Round XLI.1 — the operator's answers KILL the takeover theory and revive the idle/no-input policy as the prime freeze suspect: the web client was NEVER attached during run 22 (ours was the sole receiver) and ZERO input ever reached the session → survival is monotone in client fidelity (16/17 ~10-11 s → 18 ~30.5 s → 22 ~45 s) with exactly one missing item, so the deferred synthetic-input slice ships NOW: `INPUT_MOUSE_RELATIVE` byte-exact against the reference's own Rust golden, routed to control_partial (sid 6), as a gated 10 Hz ±1 px zero-drift jiggle (docs/INPUT.md §9)

1. **The three answers (operator, after round XLI).** (a) The web
   client is opened only AFTER knvstd finishes — never during a run;
   run 22's sole receiver was ours, so §28's "takeover" branch is dead
   and run 23 does not need to test it. (b) The session app is
   `--session-app=107057914`, matching the 0x010a `shortName` decoded
   in §28 — the rig was running the game we asked for. (c) No input
   was ever sent into the session by anyone; the operator's local
   mouse/keyboard activity is invisible to the VM. The rig therefore
   saw a client that received video, fed QoS/RR/keepalives… and never
   touched the mouse.
2. **Why idle/no-input is now the prime suspect.** Every freeze theory
   that survives run 22's forensics must explain the monotone survival
   ladder: ~10-11 s without feedback (16/17), ~30.5 s with the
   activation chain (18), ~45 s with the full reference cadence +
   warm BWE (22). Each round bought exactly the lifetime its fidelity
   item was worth. The only remaining fidelity gap against the
   official client is real input — and START-HERE §4.4's condition for
   reconsidering the deferred jiggle ("cannot separate idle-policy
   from rig flake without shipped input") is met: a run-23 freeze with
   zero input shipped would be undecidable.
3. **What ships (the first G3 slice).** `kn_input_mouse_relative`
   (kn_input.{h,c}) builds the 44-byte `0x0206`/type-7 message —
   dx/dy BE i16 at wire [20..24], ONE LE u64 timestamp tail, inner
   `remote_input_packet(0x0e, …)` framing — byte-exact against the
   reference's OWN golden from
   `keyboard_and_mouse_translate_to_byte_exact_control_commands`
   (nvst_input.rs:1888-1911): `06022800000000240e0000000000000a
   07000000001800180000…3013860100000000`. Two implementations as
   always: `tools/input_reference.py` asserts the golden in
   `self_prove()` and emits two new KAT singles (`mouse_rel`,
   `mouse_rel_neg`); `test_kninput` pins the Rust bytes + BE-i16
   negatives/extremes + buffer contracts (446 → **548** checks); the
   shipped binary re-proves them via `input.vectors` (48 → **50**).
4. **Routing and cadence.** nvst_input.rs:969 sends MOUSE_RELATIVE to
   ControlPartial = **sid 6** (unordered, PR-SCTP TTL 300 ms —
   NVST_CHANNEL_PROFILE[3]; ordered PR-SCTP would let one lost report
   stall every newer one for 300 ms). The media loop pushes one
   message every 100 ms (10 Hz) through the EXISTING
   `sess_fb_push(fb, 6, …)` outbox path (the QoS batch's channel;
   sender params already pinned by `sess_fb_sid_params` case 6). The
   dx sign alternates every message (±1 px, zero net drift across the
   run); the timestamp rides `pump.origin_us`, the activation chain's
   epoch. Gate: a new `volatile unsigned input_active` mirror of the
   pump's activation latch (single writer `sess_ctrl_try_activate`,
   single-word reads — the `sctl_*` precedent), so nothing flies
   before `enable-input(true)`. Volume 440 B/s — run 22 carried the
   50 ms QoS cadence on the same channel with `dropped:0`.
5. **Evidence added.** One `input_jiggle`/`first_send` stdout line
   (`{sid:6, interval_ms:100, ts_us}`, json.loads-verified); summary
   `feedback.jiggle` (73 → **74** specs; printf validator 108 blocks,
   0 mismatches); `outbox_by_sid["6"]` now includes the jiggle pushes
   (~+10/s over run 22's 5861). Build stamp **XLI.1**.
6. **Sandbox gates (all green this round).** native 18/18, **2378**
   checks (2276 + 102); cross ×2 zero warnings `-Werror`; ctest
   20/20 mbedTLS + 20/20 schannel; paranoid knvstd ×9 +
   kn_rx/kn_bw/kn_input ×3 clean; ASan+UBSan knrx **218** / kninput
   **548** / knbw **387** clean (third_party objects unsanitized —
   the established recipe; crypto-algorithms' sha1.c trips
   `-fsanitize=undefined`'s shift check by design of THAT library);
   e2e **43/43**; T10 POSIX dry **ALL PASS 69**; T13 **ALL PASS 63**.
7. **Run 23 decision matrix (target gates unchanged: T10 ALL PASS
   79, test_knrx 218, test_kninput 548, `input.vectors` 50).**
   * Video survives PAST ~45 s with `feedback.jiggle` climbing →
     idle/no-input policy CONFIRMED as the freeze cause; the G3
     remainder (keyboard/wheel/text) becomes the fidelity roadmap.
   * Freeze at ~the same wall WITH the jiggle demonstrably flying
     (`first_send` line + counter) → input is eliminated too; the
     freeze is rig-side (encoder/publisher) and no client byte fixes
     it — the D8 recovery contract's `recovery_exhausted` verdict
     (shipped round XLI, still unexercised live) dates it precisely.
   * The round-XLI recovery block is expected to fire in BOTH freeze
     outcomes: attempt 1 `action:"recover"` ~5 s after the stall,
     terminal ~+5 s, `stop_reason:"recovery_exhausted"` ~10-15 s
     after the freeze (run 23 should therefore END EARLY on a freeze
     instead of burning the remaining 255 s — a free bonus verdict).
   * Still mandatory: stdout capture PLUS the whole `logs/` directory
     (run 22 lacked it), including `transport.jsonl` — and the
     run-22 files the operator listed (`transport.jsonl`,
     `T10_skrot/wynik.txt`, `diag.log`, `app.log`, `http.log`) are
     requested for cross-checking before/with run 23.

## 30. Round XLII — R7 run 23: **THE JIGGLE FLEW AND THE FREEZE CAME ANYWAY — INPUT IS ELIMINATED AS A SURVIVAL FACTOR, AND WITH IT THE FIDELITY LADDER** (198 relative-mouse reports on sid 6 — opened 8/8 acked, outbox 0 drops — from t+1.3 s; media STILL died instantly at 10.58 s: 319 AUs @ 30.15 fps continuous, zero gaps/errors, BWE warm at 34.8 Mbps peak, then 0 datagrams / 0 control / 0 WSS pongs in the same second). **D8's recovery contract made its live debut exactly to spec**: stall +5 s → `rx_recovery` recover + 50 IDR/PLI retries → +5 s → terminal → `stop_reason:"recovery_exhausted"`, clean exit at 20.6 s wall instead of 340 s of dead air. The 0x010a app-lifecycle sequence is IDENTICAL to run 22's (Arknights: Endfield never got past its own Qt launcher — `QtWebEngineProcess.exe` + foreground lost to pid 0) yet the freeze moved 41 s → 10.6 s ⇒ media survival across runs 16–23 (10.2, 11.0, 30.5, ~1.3, ~1.2, ~41, 10.6 s) is a **rig-side lottery uncorrelated with client fidelity**. Ships: **F2** the 0x010a hex cap 800→2400 (the 566-byte `SESSION_LENGTH_TIMER` was truncated by OUR OWN cap in BOTH runs — its body has never been read) and **F3** `rel_ms` session-relative timestamps on `ctrl_msg`/`sctl_010a`/`ctrl_in` (run 23 could not order the notifications against the freeze). Stamp **XLII**.

1. **Target deployment verified (T10 on Win7, 2026-10-28 17:13 build).**
   `ALL PASS 79`. Suite deltas vs the XL rebuild match the sandbox
   EXACTLY: `test_knrx` 228 → **243** (+15 = D8's recovery section),
   `test_kninput` 446 → **548** (+102), gated `input.vectors` **50**
   (was 48), `test_knselftest` 116 (stable). **Docs correction**: the
   XL/XLI-era "expect knrx 218 on target" quoted the RAW-GCC recipe
   count — the cmake/ninja build (T10, both platforms) links the full
   `kncore` and reports **243** (sandbox T10 dry agrees: 243). The +25
   is a BUILD-RECIPE delta, not Windows; the T10 verdict was never
   affected. Canonical expectations from now: target/cmake knrx 243,
   raw-script knrx 218, kninput 548 everywhere (pure wire code).
2. **Startup (all clean, all fast).** Process start 16:21:09Z; queue
   position **118** (evening load — the operator's 360 s budget was
   well spent: ~150 s queue+signalling), 33 poll lines to `ready`;
   RTSP OPTIONS/DESCRIBE/SETUP/ANNOUNCE/PLAY all 200 (PLAY attempt 1);
   peer 80.84.172.148:5004; DTLS ready in 174 ms (DTLSv1.2,
   ECDHE-ECDSA-AES128-GCM-SHA256, srtp_prof 1); SCTP up in 52 ms;
   **channels opened 8, acked 8, rig_opened 0**; `input_version` 3;
   activation `sent:8 failed:0` at ts_us 333 606 (t+0.33 s);
   `cursor_handoff reason:"cursor-notification"` (the rig sent 0x010f
   ×2 — the handoff machine ran its notification path for the first
   time live); **`input_jiggle first_send` at ts_us 1 279 732
   (t+1.28 s), sid 6, interval 100 ms** — the XLI.1 gate
   (`input_active`) opened exactly when designed.
3. **The media window: 10.58 s of pristine video.** Per-second
   `transport.jsonl` (first live capture of the knlog 1 Hz plane —
   run 22 never delivered it): 365→588→330→215→328→199→217→184→144→
   168→74 datagrams/s; AUs 23/s → continuous 30.15 fps (319 AUs,
   RTP span 10 584 ms = `rtp_ms` exactly); BWE left warm-up at t+3
   (9 358 kbps) and peaked **34 799 kbps**; utilization 2–28 %;
   jitter 583–3 855 µs; `gaps/withheld/stale/fec_repaired/drops` ALL
   ZERO; q_depth 0; decode_errors 0; frames_decoded 319/319
   (libavcodec 63.2.100, 1280×720); keyframes 9. `loop_ticks` 4 205
   ≈ 204/s with `recv_timeouts` 1 393 = ticks − datagrams − 1 ✓ (the
   round-XL loop fix holding at an even higher tick rate than run 22).
4. **The freeze: instantaneous and total, at 16:23:50.4Z (~t+10.4 of
   the media loop).** 74 datagrams in second 10 → **0 in second 11**
   and every second after (2 812 total, all authenticated — the count
   never moves again). The control plane fell silent in the same
   instant (16 inbound records total: 020e×1, 010a×9, 0111×2, 010f×2,
   0106×1, 010e×1 — and **NO 0x0101 heartbeats at all**, vs run 22
   where they ran ~1 Hz for the first 30–60 s; the rig's heartbeat
   apparently starts later than t+10 s). **WSS: `pongs:0` from the
   FIRST ping (t+1 s) — the WSS control channel has never ponged in
   ANY run** (run 22: 108 pings, 0 pongs; it only errored at t+216 s
   via TCP retransmit exhaustion). SCTP stayed associated (no
   `sctp_down`, no SHUTDOWN/ABORT from the rig) while we kept sending
   everything: QoS batches 384 (≈19/s), frame acks 319, pacing 146,
   keepalives 7, NAT-T 198, IDR×50 + PLI×50 (episode 1, attempts 1
   and 50 logged), **jiggle 198** — outbox `pushed:856 dropped:0`,
   per-sid 0:[57,0] 6:[728,0] 14:[71,0]. The server answered nothing,
   ever again.
5. **D8's live debut — the round-XLI contract, to the second.**
   `progress_stall` (idle 5 006 ms) at 16:23:54 → `rx_recovery
   attempts:1 action:"recover"` + keyframe re-request (the 100 ms
   retry loop fired 50 IDR/PLI over the next 5 s) →
   `recovery_timeout` (idle 5 009 ms) → `rx_recovery attempts:1
   action:"terminal"` (16:23:59) → media loop break →
   `stop_reason:"recovery_exhausted"`, `recovery:{attempts:1,
   exhausted:1}`, exit clean at wall 20.56 s. Run 22 burned 248 s of
   dead air after its freeze; run 23 spent 10 s diagnosing itself and
   terminated with a self-labelled verdict. The transport.jsonl
   `rx_event`/`rx_recovery` lines timestamp each step — the knlog
   plane and stdout agree to the second.
6. **The app state was IDENTICAL to run 22 — the game never launched.**
   All nine 0x010a notifications match run 22's sequence and nearly
   their lengths: foregroundWindow (fullscreen, pid 10 664) →
   `Launcher.exe` JustLaunched → `Games.exe` JustLaunched (shortName
   `107057…` = Arknights: Endfield, the operator confirmed launching
   it) → Launcher exits 0 → process action 1 → **`QtWebEngineProcess.exe`
   JustLaunched (child of Games.exe)** → **foregroundWindow pid 0
   (foreground LOST)** → foregroundWindow Games.exe NOT fullscreen →
   `SESSION_LENGTH_TIMER` (566 B). Reading: the session sat in the
   game's own launcher/login web portal — gameplay was never reached
   in run 22 OR run 23 (and plausibly not in runs 20/21 either — the
   drip-feed and the static screen smell like the same launcher
   state). Yet with the SAME app state the freeze landed at 41 s once
   and 10.6 s once → app state does not TIME the freeze; it may still
   CAUSE the class (an instance parked on a login portal may be
   encoder- or reaper-hostile), which is only testable by changing
   the app. The `SESSION_LENGTH_TIMER` body — potentially the rig
   telling the client something about session limits — was cut at
   400 payload bytes by our OWN F1 cap (`%.800s` hex) in both runs.
   **F2 raises the cap to 2400 hex chars** (1 200 B; every 0x010a so
   far ≤ 566 B) so run 24 finally reads it. **F3 adds `rel_ms`**
   (transport-origin-relative, the activation epoch) to `ctrl_msg`,
   `sctl_010a` and `ctrl_in` — run 23 could not say whether the
   launcher notifications landed before or after the media freeze;
   run 24 will, to the millisecond.
7. **Verdict per the §29.7 decision matrix: branch 2 — INPUT
   ELIMINATED.** The jiggle demonstrably flew (first_send line, 198
   pushes, sid 6 open+acked, 0 drops) for 9.2 s BEFORE the freeze and
   continued through it; the freeze came anyway — and EARLIER than
   run 22's zero-input 41 s. Neither direction of causality survives:
   input neither keeps the session alive nor kills it. The
   run-16→22 "monotone fidelity ladder" (10→11→30.5→41 s) was a
   pattern built on four points; runs 20 (~1.3 s of media!) and 21
   (~1.2 s) already contradicted it, and run 23 at maximum fidelity
   lands back at the runs-16/17 floor. **Media survival is a rig-side
   lottery (1.2–41 s across seven runs), uncorrelated with anything
   the client sends.** The idle/no-input hypothesis is closed; the
   server-policy family is closed with it (no termination, no idle
   warning, no pacing change, no SCTP teardown — just silence). What
   remains is rig/instance-side: the encoder pipeline or the instance
   supervisor killing a session parked on a game-launcher screen, or
   plain bad-instance luck (evening, queue 118).
8. **Round XLII ships** (evidence-only; no wire-format change): F2
   the 0x010a cap 800→2400 hex chars; F3 `rel_ms` on the three
   control-evidence lines (printf validator: 108 blocks, specs=args,
   summary unchanged 74=74, the 3 new line shapes `json.loads`-clean);
   build stamp **XLII**. **The jiggle stays ON** for run 24: input is
   now a controlled constant (and the idle policy is not formally
   dead — only 9.2 s of input preceded this freeze; a policy window
   longer than that stays untested), it costs 440 B/s, and it keeps
   our client the highest-fidelity non-official receiver on the wire.
   Deferred on evidence: the auto-restart loop after terminal (the
   reference app restarts its pipeline; our `--session-token` reuse
   semantics for a SECOND CM flow are unproven — that needs its own
   probe round), D-t13.
9. **Sandbox gates (all green):** native 18/18 **2378** checks
   (raw-gcc recipe: knrx 218, kninput 548, knbw 387) · `cross_check`
   ×2 zero warnings `-Werror` · ctest **20/20 mbedTLS + 20/20
   schannel** · paranoid knvstd ×9 + kn_rx/kn_bw/kn_input ×3 clean ·
   ASan+UBSan knrx 218 / kninput 548 / knbw 387 clean ·
   `sandbox_session_e2e` **43/43** · T10 POSIX dry **ALL PASS 69**
   (its cmake recipe reports knrx **243**, matching the target — the
   recipe delta of §30.1) · T13 **ALL PASS 63** · summary-printf
   validation **108** blocks specs=args, summary **74=74**, skeleton
   `json.loads` PASS.
10. **Run 24 plan — change the ONE variable left: the app.** Pick a
    GFN title that reaches GAMEPLAY with no login/launcher interaction
    (Arknights: Endfield is banned until its portal behaviour is
    understood; a first-party-friendly free title that auto-starts is
    ideal). Keep everything else constant (evening or not, the same
    command, `--session-seconds=360` for queue slack). Optionally
    sample the lottery: after a `recovery_exhausted` exit, immediately
    re-run (a fresh CM flow = a fresh rig draw) for 2–3 draws in one
    evening — the freeze instant's spread across draws IS the
    measurement. Per attempt: untruncated stdout + the whole `logs/`.
    Expected: stamp `"round":"XLII"`; `input_jiggle first_send` ~1 s
    after activation; `sctl_010a` lines with `rel_ms` and FULL
    `SESSION_LENGTH_TIMER` bodies; T10 target **ALL PASS 79** with
    knrx **243** (cmake), kninput **548**, `input.vectors` **50**.
    Read: gameplay-reaching app surviving past ~41 s ⇒ the
    launcher-parked-instance theory is confirmed and stage 2 can
    close against a real gameplay stream; same early freeze with
    gameplay on screen ⇒ bad-instance lottery, and the auto-restart
    loop becomes the practical stage-2 exit (ship it next round).

---

## 31. Round XLIII — R7 run 24: **THE LOTTERY DREW THE SAME SLOT TWICE — a near-exact clone of run 23 (freeze at 10.43 s of media vs 10.58 s, recovery_exhausted at wall 20.54 s vs 20.56 s)** — and the two round-XLII instruments paid for themselves on the first flight: **F3** orders ALL control traffic inside the first 10 s (`ctrl_in rel_ms:10000`, 15 msgs) proving the freeze arrives with the control plane already silent, and **F2** reads the notification burst in full — all eight 0x010a land at **t+257–264 ms** (the launcher-portal startup burst, NOT the freeze trigger) and expose the rig's own classification: `"drsProfileName":"EA Desktop"` on an Arknights session, final foreground `Games.exe fullscreen:false`. This draw carried **no SESSION_LENGTH_TIMER** (run 23 had one — the server varies draw to draw; the full timer body is still unread). The user's field probe closed the restart-loop question: cm_create fired seconds after a recovery_exhausted exit returns a bare **HTTP 403** (twice; cloud_status 0, empty desc) while the SAME token queues fine after a pause ⇒ **403 = seat cooldown, token reusable**. Ships **F4** (cm_create 403 backoff, 20 s × 3, TLS rebuilt per retry) and **F5** (`--lottery=N`/`--lottery-gap=SEC`: one invocation re-draws the whole bring-up and reports every draw). Stamp **XLIII**. Run 25 = the first unattended lottery.

### 31.1 Run 24 vs run 23 — the clone table

Both draws: same app (107057914, Arknights: Endfield), same binary
family (run 24 = XLII stamp, compiled Oct 28 2026 18:15:18), same
command shape, same rig-side app state (Qt launcher portal; gameplay
never reached).

| metric | run 23 | run 24 | delta |
|---|---|---|---|
| queue depth at entry | 118 | 33 | rig-side |
| activation | t+0.33 s | t+0.28 s (attempt 1, version 3) | same |
| jiggle `first_send` | t+1.28 s | t+1.10 s (`ts_us 1095109`, sid 6) | same |
| AUs / decoded | 319 / 319 | 314 / 314 | same |
| fps (continuous) | 30.15 | ~30.1 | same |
| media at freeze (`au_clock rtp_ms`) | 10 583 | **10 425** | **−158 ms** |
| keyframes / gaps / decode_errors | 11 / 0 / 0 | 10 / 0 / 0 | same |
| BWE peak | 34 799 kbps | 31 749 kbps | warm both |
| datagrams authenticated | 2586/2586 | 2578/2578 | same |
| stop_reason / wall | recovery_exhausted / 20.56 s | recovery_exhausted / **20.54 s** | **same** |
| feedback: jiggle / idr / pli / rr | 198 / 50 / 50 / 21 | 200 / 50 / 50 / 21 | same |
| outbox sid 6 pushed / dropped | 727 / 0 | 730 / 0 | same |
| WSS pongs | 0 | 0 | same |
| rx_recovery | stall 5006 → terminal 5007, 50 retries | idle 5008 → terminal 5015, 50 retries | same to the ms |

The `au_clock` arithmetic is identical in shape: rtp 10 425 ms +
~10.0 s of frozen-air wall = 20 462 ms measured (`drift_ms −56`).
Two independent draws landing within 158 ms of media time on the same
app state is the strongest evidence yet that **the ~10.5 s freeze is a
modal rig-side behaviour of the launcher-parked instance**, not a
random per-run coin.

### 31.2 F3's first flight — the control plane is silent AT the freeze

`ctrl_in` (the per-poll control digest) reported **`rel_ms:10000`**
with **15 messages** — every byte of server control traffic in the
whole session arrived inside the first 10 s, i.e. BEFORE the media
clock reached 10.43 s. Code breakdown (vs run 23's 16):
`020e`×1, `0111`×2, **`010a`×8** (run 23: 9), `010f`×2, `0106`×1,
`010e`×1 — and **no `0x0101` heartbeat at all** (run 22 had one at
4.37 s; runs 23/24 none). The freeze therefore arrives with:
no termination (0x109), no idle warning, no heartbeat decay — nothing.
Same signature as run 23, now with millisecond ordering proven.

### 31.3 F2's first flight — the full 0x010a burst, and what was hidden

All eight notifications are one startup burst at **rel_ms 257–264**:

| # | rel_ms | len | event (full body now readable) |
|---|---|---|---|
| 1 | 257 | 298 | `foregroundWindow` pid 6108 processType 4 **fullscreen:true** |
| 2 | 262 | 472 | `process` action 2 **Launcher.exe** pid 5384 parent nvcontainer.exe JustLaunched — `drsAppName:"launcher.exe"` `drsProfileName:"launcher"` |
| 3 | 262 | 450 | `process` action 2 **Games.exe** pid 2364 parent Launcher.exe JustLaunched `shortName:"107057914"` `cmsId:107057914` |
| 4 | 262 | 283 | `process` action 3 pid 5384 (Launcher.exe) `exitCode:"0"` |
| 5 | 263 | 277 | `process` action 1 processType 6 info:"" |
| 6 | 263 | 489 | `process` action 2 **QtWebEngineProcess.exe** pid 10920 parent Games.exe JustLaunched — `drsAppName:"qtwebengineprocess.exe"` **`drsProfileName:"EA Desktop"`** |
| 7 | 264 | 296 | `foregroundWindow` processType 0 **pid 0** fullscreen:false |
| 8 | 264 | 299 | `foregroundWindow` processType 1 pid 2364 (Games.exe) **fullscreen:false** |

Three facts this buys:

1. **F2 was load-bearing on day one**: rows 2 and 6 are 936 and 970
   hex chars — BOTH were silently truncated at the old 800-char cap in
   runs 22–23. The `drsAppName`/`drsProfileName` fields sit at the tail
   of the payload; without F2 they would still be invisible.
2. **The rig classified this Arknights session under the `EA Desktop`
   DRS profile** (row 6). Whatever the rig's dynamic-resolution logic
   does to this stream, it does it under a profile named for a
   completely different publisher's launcher — the classification
   follows the window/process the rig sees (QtWebEngineProcess), not
   the cmsId we requested. This is rig-side behaviour we can only
   observe, but it is the first direct glimpse of the rig's app-state
   confusion on these parked sessions.
3. **No SESSION_LENGTH_TIMER in this draw** (run 23 had one as its 9th
   notification). The server's notification set varies per draw; the
   full timer body — the one payload that might state the rig's own
   session limit — remains unread. The lottery (F5) is the way to
   catch a draw that carries it.

The foreground sequence ends at row 8: Games.exe, NOT fullscreen — the
portal window. Nothing in the burst correlates with the freeze at
t+10.4 s; the app lifecycle settled 10 s earlier.

### 31.4 The survival distribution, updated (runs 16–24)

| run | 16 | 17 | 18 | 20 | 21 | 22 | 23 | 24 |
|---|---|---|---|---|---|---|---|---|
| media s | 10.2 | 11.0 | 30.5 | ~1.3 | ~1.2 | ~41 | 10.6 | **10.4** |

The **~10.5 s mode is now the plurality (4 of 8 draws)**; two long
draws (30.5, 41) and two drips (~1.2). Runs 16/17 had NO activation
and NO jiggle; 23/24 had everything — the same modal lifetime across
both extremes re-confirms the XLII verdict (client fidelity is not the
dial) and sharpens it: the launcher-parked instance most often dies at
~10.5 s of media regardless of what the client does.

### 31.5 The 403 seat probe (run24v2) — restart-loop unblocked

Verbatim from the user's double attempt seconds after run 24's
recovery_exhausted exit:

```
{"mode":"session","error":"cm_create","rc":0,"http_status":403,"cloud_status":0,"desc":""}
```

— twice. After a longer pause, the SAME token entered the queue again
(that queued session is the tail of run24v2.log). Conclusions: the
session token is reusable across draws; a bare 403 from cm_create
means "the rig still holds your previous seat" (cf. the known
`cloud_status 11 = SESSION_LIMIT` sibling, which arrives with a JSON
body — the 403 arrives with none). An auto-restart loop therefore
needs a cooldown backoff, not a new token.

### 31.6 Round XLIII shipped — F4 + F5

**F4: cm_create 403 backoff.** The TLS-open → `kn_cm_create` →
TLS-close sequence is now a loop: on `http_status == 403` (and only
403 — every other failure aborts instantly as before) knvstd prints
`{"event":"cm_create_403_backoff","attempt":k,"wait_ms":20000}`,
sleeps 20 s, rebuilds the TLS stream and retries, up to
`SESS_CM_403_RETRIES 3` (= 4 attempts, ≤ 60 s of backoff). This makes
back-to-back draws — the lottery's — survive the seat cooldown
automatically.

**F5: the lottery driver.** `--lottery=N` (1–100, default **1** = the
legacy single-draw path, byte-identical) + `--lottery-gap=SEC`
(0–3600, default 20). For N > 1 main() loops `run_session()` up to N
times: between draws a `lottery_gap` line + sleep; after each draw a
`lottery_draw {draw,of,rc}` line; at the end
`lottery_summary {draws,of,last_rc}`; the process exit code is the
last draw's rc. **rc == 0 stops the lottery early** — a draw that
satisfied `--session-expect-frames` (or ran its full
`--session-seconds`) is a keeper worth keeping the rig for.

Re-callability audit (the reason F5 is in-process, not a respawn
driver): every stateful static inside `run_session()` is re-initialised
at entry (`memset &c`, `memset &pump` + `sess_fb_init(&fb)`) and
destroyed before return (`sess_pump_stop` joins the worker,
`sess_fb_destroy`), the kn_log init/shutdown cycle is the same one the
selftest battery already runs back-to-back in a single process, and
the TLS/DTLS/socket objects are all function-scoped and closed on
every exit path. The sandbox field test exercised the loop mechanics
over three failing draws (gap timing exact, envelope lines
`json.loads`-clean, exit code propagated); the full two-live-sessions-
in-one-process path is Win7-only and is the one thing run 25 teaches.

**Evidence lines added (4):** `cm_create_403_backoff`, `lottery_gap`,
`lottery_draw`, `lottery_summary` — validator now covers **112** JSON
printf blocks, session summary unchanged at **74 = 74** specs.

### 31.7 Run 25 — the first unattended lottery

One invocation, five draws, 30 s apart (the F4 backoff covers the
seat cooldown inside each bring-up):

```
knvstd.exe --session --session-token=<tok> --session-app=107057914 ^
  --session-seconds=360 --lottery=5 --lottery-gap=30 ^
  --session-au-dir=logs\aus_r25 > logs\run25.log
```

Read matrix:

| outcome | verdict |
|---|---|
| any draw survives > 60 s of media (esp. a full 360 s → rc 0, early stop) | the long-draw mode is reachable on demand ⇒ stage 2 can close against a real gameplay stream; keep drawing until it happens |
| all 5 draws freeze ~10.5 s | the modal behaviour is now measured, not suspected ⇒ the practical stage-2 exit is the lottery itself (draw until a keeper); investigate the EA-Desktop-profile/parked-portal angle rig-side |
| a draw carries SESSION_LENGTH_TIMER | F2 finally reads the full body — the rig's own session-limit statement |
| `cm_create_403_backoff` lines appear and the draw proceeds | F4 proven live; the seat cooldown bound is measured (attempt count × 20 s) |

The same app is deliberately allowed again: with the lottery
instrumented, every draw is data — the app switch is only needed for a
KEEPER draw (gameplay), and the operator has not yet supplied a
gameplay-reaching app id.

---

## 32. Round XLIV — R7 run 25: **THE LOTTERY'S FIRST LIVE FLIGHT EXPOSED ITS OWN STOP-RULE BUG (draw 1 of 5 declared a "keeper" while its summary said `recovery_exhausted`) — and the single draw it did deliver paid for the run twice over: F2 read the FULL `SESSION_LENGTH_TIMER` for the first time ever (`beforeEventMS: 3599657` — the rig granted the session a FULL ONE-HOUR budget, killing the session-limit theory of the early freezes) and the draw itself was the first LOSSY one on record** (gaps 2, `fec_repaired` 6, nack 5, withheld 90, lossy_frames 5 — yet 257/257 AUs decoded; BWE peaked at an all-time 45.9 Mbps). Media survived 11.59 s — the fifth straight draw in the ~10–12 s band. The bug: `run_session`'s exit code returned 0 for ANY session that received ≥1 datagram and wasn't `socket_error`, so `recovery_exhausted` exited 0 (runs 23–25 all did) — the documented contract says 1 ("failed verdict"). **F6** fixes the exit code (only the natural endings `"time"`/`"expect_frames"` can exit 0) and corrects the lottery stop rule (only rc 1 re-draws; rc 0 = keeper; rc 2/3 = abort; Ctrl-C = abort). Stamp **XLIV**. Run 26 = the lottery again — this time it will actually draw 5 times.

### 32.1 What run 25 delivered in ONE draw

| metric | run 23 | run 24 | run 25 |
|---|---|---|---|
| queue at entry | 118 | 33 | 32 |
| activation | t+0.33 s | t+0.28 s | t+0.75 s (attempt 1, v3) |
| jiggle `first_send` | t+1.28 s | t+1.10 s | t+1.32 s (196 reports, 0 drops) |
| AUs / decoded | 319/319 | 314/314 | **257/257** |
| media at freeze | 10.58 s | 10.43 s | **11.59 s** (`rtp_ms 11586`) |
| gaps / fec / nack | 0/0/0 | 0/0/0 | **2 / 6 / 5** (first lossy draw) |
| withheld / lossy_frames | – | – | **90 / 5** (`skip_reordered` 11) |
| idr / pli / kf_requests | 50/50/1 | 50/50/1 | **69/69/2** (episode 1 early + cleared after 25 attempts, episode 2 = the recovery) |
| BWE peak | 34.8 Mbps | 31.7 Mbps | **45.9 Mbps** (all-time) |
| datagrams authenticated | 2586/2586 | 2578/2578 | 3074/3074 |
| stop_reason / wall | recovery_exhausted / 20.56 s | recovery_exhausted / 20.54 s | recovery_exhausted / **21.76 s** |
| rig IP (handoff) | — | 80.84.174.148 | **80.250.97.36** (different rig) |
| 0x0101 heartbeat | none | none | **present** (`ctrl_in`: 16 msgs, codes 020e×1 0111×2 010a×9 010f×1 0106×1 010e×1 0101×1, all `rel_ms ≤ 9999`) |
| SESSION_LENGTH_TIMER | truncated (own cap) | absent | **FULL BODY READ** (F2) |

The loss is notable but not causal: the gaps/nacks rode in the middle of
the stream, every frame still decoded, and the freeze arrived the same
way (datagrams pinned at 3074 from one second to the next while
`loop_ticks` kept climbing — transport.log tail: 4065 → 4121 → 4198 →
4266, then `recovery_timeout` → `terminal` at 18:59:41.816Z). A
different rig IP froze the same way at the same media age.

`sctl_unknown code 526` = opcode **0x020e** — present in runs 23/24's
`ctrl_in` too (`020e`×1 every time); the reference's decode table does
not know it either (it lands in the reference's own Unknown bucket).
Counted, logged, ignored — exactly the designed behaviour.

### 32.2 The SESSION_LENGTH_TIMER, in full at last (F2's prize)

Notification #9, `rel_ms 1665` (run 23's copy arrived ~12.9 s — the
delivery time varies wildly per draw), 567 B payload = 1126 hex chars
— **the old 800-char cap would have cut it in half**:

```json
{"messageType":"SESSION_LENGTH_TIMER",
 "tracingData":{"requestId":"73eb5bc5-...","traceId":"8bfa0415c7b301a1",
   "traceContext":[{"key":"ot-tracer-traceid","value":"8bfa0415c7b301a1"},
                   {"key":"ot-tracer-spanid","value":"46fed972bd71745b"},
                   {"key":"ot-tracer-sampled","value":"1"}]},
 "timerData":{"timerType":"DEFAULT","presentDurationMS":0,
              "beforeEventMS":3599657}}
```

**`beforeEventMS: 3 599 657` ≈ 3600 s = one hour.** The rig granted
this session a full hour before its "event" (termination). The freeze
at 11.6 s was therefore NOT a session-budget expiry, NOT an idle
policy with a seconds-scale timer, and NOT a limit tied to our
account's remaining time — the server's own stated budget had 59+
minutes left. Combined with §31 (silent control plane at the freeze,
no 0x0109/0x0112/0x0101-decay), the early freeze is now conclusively
a rig-side encoder/app-state failure, invisible to every control
channel the protocol offers.

The notification set for this draw was otherwise the run-24 clone:
the same eight-burst at t+658–664 ms (Launcher.exe → Games.exe
cmsId 107057914 → Launcher exit → **QtWebEngineProcess.exe
`drsProfileName:"EA Desktop"`** → foreground pid 0 → Games.exe
fullscreen:false) + the timer at 1.67 s. The EA-Desktop profile on an
Arknights session reproduced exactly (§31.3) — it is the rig's stable
misclassification of the parked portal, not a one-off.

### 32.3 The F6 bug — anatomy of a false keeper

Run 25's stdout, verbatim:

```
{"mode":"session","summary":{"stop_reason":"recovery_exhausted",...}}
{"mode":"session","event":"lottery_draw","draw":1,"of":5,"rc":0}
{"mode":"session","event":"lottery_summary","draws":1,"of":5,"last_rc":0}
```

rc 0 from a frozen session. The pre-XLIV exit-code rule inside
`run_session` was:

```c
exit_code = (datagrams > 0 && produced > 0 &&
             c.au_write_errors == 0 &&
             strcmp(stop_reason, "socket_error") != 0) ? 0 : 1;
```

— i.e. ANY media at all + anything except `socket_error` = "ok". That
predates D8: before the recovery contract, a session that produced
frames and ended had, in practice, run its course, and the rule was
never exercised against a TERMINAL stop_reason (runs 23/24 exited 0 as
well — nobody consumed the code until the lottery did). F5's stop rule
(`rc == 0` = keeper) inherited the flaw. The documented contract in
usage() always said the opposite: "exit codes: 0 ok/skipped, **1 failed
verdict**" — a `recovery_exhausted` session IS a failed verdict.

**F6a (the fix):** only the natural endings can exit 0 —
`stop_reason == "time"` (the requested `--session-seconds` elapsed) or
`"expect_frames"` (the `--session-expect-frames` goal was met) — AND
datagrams > 0 AND produced > 0 AND no AU write errors. Every terminal
or interrupted reason (`recovery_exhausted`, `server_termination`,
`socket_error`, `queue_timeout`, `terminated`, `session_not_found`,
`interrupted`, `cm_poll_failed`, `cm_poll_rejected`, `signal`) exits
1. Historical note: this changes the process exit code of runs
23–25-shaped sessions from 0 to 1; nothing in the gate set consumes
the live-session exit code (T10/T13/e2e never touch it), and the
contract line in usage() now matches reality.

**F6b (the lottery stop rule):** re-draw ONLY on rc 1 (a dead draw).
rc 0 = a genuine keeper → stop and keep the rig. rc 2/3 = usage /
internal failure a re-draw cannot fix → stop. `g_knv_stop` (Ctrl-C)
→ stop. The sandbox proof: `--lottery=5` against the TLS-less POSIX
build now ends after draw 1 with `last_rc:3` (before F6b it spun all
N draws on an unfixable error).

### 32.4 The survival distribution after 9 draws (runs 16–25)

| run | 16 | 17 | 18 | 20 | 21 | 22 | 23 | 24 | 25 |
|---|---|---|---|---|---|---|---|---|---|
| media s | 10.2 | 11.0 | 30.5 | ~1.3 | ~1.2 | ~41 | 10.6 | 10.4 | **11.6** |

The ~10–12 s band now holds **5 of 9 draws** (16, 17, 23, 24, 25), on
two different rig IPs, with and without activation, with and without
jiggle, with and without 0x0101, with and without packet loss, with a
one-hour session budget granted. The band is the rig's modal failure
mode for launcher-parked sessions. The long draws (30.5, 41) remain
unpredicted by any client-side variable — which is exactly why the
lottery exists: run 26 draws five times unattended and the distribution
speaks for itself.

### 32.5 Run 26 — the lottery, actually lotterying

Same command as run 25 (the operator's copy-paste stays valid); with
F6 the driver will now re-draw after each `recovery_exhausted` and
stop early ONLY on a draw that survives its full 360 s:

```
knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=360 --lottery=5 --lottery-gap=30 --session-au-dir=logs\aus_r26 > logs\run26.log
```

Read matrix: 5 dead draws ⇒ the modal band is confirmed at n=5 in one
sitting and the practical stage-2 exit is a persistence lottery (or a
gameplay-reaching app); ANY draw surviving > 60 s ⇒ long draws are
reachable on demand, keep drawing until a full-length keeper closes
stage 2; `cm_create_403_backoff` lines ⇒ F4's live proof and the
seat-cooldown bound. Every draw prints its own summary + envelope
lines; stdout is the complete record (the knlog `logs/` channels hold
the LAST draw only).

---

## 33. Round XLV — R7 run 26: **THE LOTTERY RAN FOR REAL — 5 draws, 2 real sessions, 3 lost at the CloudMatch door, and the two refusals are now measured: the seat cooldown can exceed 90 s (draw 2: 30 s gap + 3×20 s F4 retries, still 403) and repeated creates trip an HTTP 429 rate limiter (draws 3 and 5 — F4 did not retry 429, so both died instantly).** F6a/F6b passed their live proof (every dead draw exited 1 honestly, the lottery completed all 5, and `wire_session.log` — the exit≠0 ring dump — reappeared for the first time since the pre-D8 runs). The two real draws both died in the band (**13.15 s** and **11.34 s** of media; 379/379 and 342/342 AUs; draw 1 lossy again — gaps 1, fec 1, withheld 16 — decoded perfectly), which completes a pattern worth a controlled test: **all FIVE jiggle-on draws ever (runs 23–26) died at 10.4–13.2 s, while the only TWO long draws in history (runs 18/22: 30.5/41 s) were activation-WITHOUT-jiggle.** Ships **F7** (cm_create retries 403 + the whole transient class incl. 429, escalating 20/40/80/160 s, event renamed `cm_create_backoff` + `status` field) and **F8** (`--session-jiggle=MS`, 0 = OFF — the jiggle stops being a controlled constant and becomes the variable under test). Stamp **XLV**. Run 27 = the jiggle-OFF lottery.

### 33.1 Run 26, draw by draw

| draw | fate | detail |
|---|---|---|
| 1 | **session, dead at 13.15 s media** | queue instant (`queue_position:0`); `recovery_exhausted`, wall 23.3 s; 379/379 AUs, 11 keyframes; gaps 1, fec_repaired 1, withheld 16, lossy_frames 3 — decoded perfectly; BWE 36.6 Mbps; rig handoff normal |
| 2 | **cm_create 403 ×4 → died at the door** | 30 s lottery gap + F4's 3×20 s backoff = ~90 s of patience, STILL 403 ⇒ the seat cooldown exceeded 90 s (run 24's manual probe suggested "a pause" — now bounded from below) |
| 3 | **cm_create 429 → died instantly** | the FIRST 429 on record: CloudMatch rate-limits session CREATES (≈7 creates in ~6 minutes triggered it); F4 only retried 403 → no patience applied |
| 4 | **session, dead at 11.34 s media** | queued after ONE 20 s backoff (seat freed); `recovery_exhausted`, wall 21.6 s; 342/342 AUs, gaps 0, clean; BWE 33.8 Mbps |
| 5 | **cm_create 403 ×3 → 429 → died** | the refusals compose: seat first, then the rate limiter on the retries |

`lottery_summary {"draws":5,"of":5,"last_rc":1}` — F6b re-drew on every
rc 1 and never mistook a dead draw for a keeper; the process exited 1
("no keeper this evening"), exactly the new contract. The 167 KB
`wire_session.log` (draw 4's media ring, dumped because exit ≠ 0) is
the first ring dump since the exit-code fix made it reachable again.

Control plane, both draws (F3): `ctrl_in rel_ms:10000` — draw 1: 18
msgs (`020e`×1 `0111`×2 `010a`×9 `010f`×1 `0106`×1 `010e`×1
**`0101`×3**), draw 4: 17 msgs (**`0101`×1**). ALL control traffic
inside the first 10 s again; the freeze lands on silence again.
**No SESSION_LENGTH_TIMER in either draw** (9 = the 8-notification
burst + one extra foreground/process variant) — the timer's delivery
is intermittent: present in runs 23 (~12.9 s) and 25 (1.67 s), absent
in 24, 26×2. The one-hour budget fact from §32.2 stands; its delivery
is simply per-draw luck.

### 33.2 F7 — the CloudMatch patience schedule

Run 26 measured both refusal classes and F4's insufficiency. F7:

- retry when `http_status == 403 || cm_status_transient(http_status)`
  — reusing the polling loop's existing class (408/425/**429**/500/
  502/503/504); every other status aborts instantly as before;
- escalating waits `SESS_CM_BACKOFF_BASE_MS << attempt` =
  **20 → 40 → 80 → 160 s** (`SESS_CM_RETRIES 4`; ≤ 5 min of patience
  per draw, 5 attempts total) — covers the >90 s seat cooldown with
  margin and gives a 429 limiter minutes to clear;
- event line renamed `cm_create_403_backoff` → **`cm_create_backoff`**
  with a `"status"` field (`{"status":429,"attempt":2,"wait_ms":40000}`).

Worst-case lottery wall time grows (5 draws × up to ~5 min of CM
patience) but every wait is bounded and every line is evidence.

### 33.3 The jiggle question reopens — as a controlled A/B (F8)

The survival record after 11 measured draws:

| run | 16 | 17 | 18 | 20 | 21 | 22 | 23 | 24 | 25 | 26a | 26d |
|---|---|---|---|---|---|---|---|---|---|---|---|
| media s | 10.2 | 11.0 | **30.5** | ~1.3 | ~1.2 | **~41** | 10.6 | 10.4 | 11.6 | **13.2** | 11.3 |
| activation | – | – | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| jiggle | – | – | – | – | – | – | ✓ | ✓ | ✓ | ✓ | ✓ |

The band (~10–13 s) holds 7 of 11 draws — including runs 16/17 (no
activation, no jiggle) and every jiggle-on draw. The TWO long draws
ever recorded (18: 30.5 s; 22: ~41 s) share exactly one
configuration: **activation without jiggle**. XLII eliminated input as
a SAVIOUR (run 23 froze WITH the jiggle flying, earlier than run 22
without it) — but it never tested the opposite direction: could the
jiggle be *suppressing* the long-draw mode (a server-side
synthetic-input policy, an encoder interaction, or pure chance at
n=5)? Confounders are real (different evenings, different rig pools —
run 26 drew a fresh IP), which is why this needs draws, not argument.

**F8** makes the jiggle a run parameter: `--session-jiggle=MS`
(0–10000, default 100 = unchanged behaviour; **0 = OFF** — the gate
`jiggle_interval &&` short-circuits the media-loop block; the
`input_jiggle` first_send line and the `feedback.jiggle` counter then
simply never fire). The activation chain is untouched — run 27 is
"run 22's configuration inside run 26's lottery".

### 33.4 Run 27 — the jiggle-OFF lottery

```
knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=360 --lottery=5 --lottery-gap=30 --session-jiggle=0 --session-au-dir=logs\aus_r27 > logs\run27.log
```

Read matrix:

| outcome | verdict |
|---|---|
| ANY draw > 30 s (esp. a 360 s keeper → rc 0, early stop) | **the jiggle suppresses long draws** — input flips from "eliminated as saviour" to "active depressant"; default goes OFF and the long-draw mode becomes farmable |
| all draws ~10–13 s again | the jiggle is exonerated a second time (7 jiggle-on + 5 jiggle-off draws, same band) ⇒ the long-draw mode is pure rig-side luck; keep the jiggle ON or OFF indifferently and farm draws until a keeper |
| `cm_create_backoff {"status":429/403,...}` lines followed by queued draws | F7 proven live; the CM patience schedule measured against the real limiter |

Every draw prints its own summary; `feedback.jiggle` MUST read 0 in
run 27 (the off-switch proof), and no `input_jiggle` line may appear.

---

## 34. Round XLVI — R7 run 27: **THE JIGGLE A/B IS DECIDED — THE SYNTHETIC MOUSE JIGGLE IS A SURVIVAL DEPRESSANT.** Five jiggle-OFF draws on the SAME evening, the SAME app, five FRESH rigs (every draw landed on a different IP): **10.32 / 21.81 / 15.39 / 52.19 / 22.87 s** of media — four of five beyond the jiggle-ON maximum of 13.2 s, and draw 4 set the new project record: **52.19 s of continuous 30 fps video, 1546/1546 AUs decoded, 13 071/13 071 datagrams authenticated, ONE gap, zero decode errors, BWE 37.8 Mbps** — beating run 22's ~41 s (also a jiggle-off run). The five jiggle-ON draws (runs 23–26, same evening): 10.6 / 10.4 / 11.6 / 13.2 / 11.3 s — five of five inside the band. Read against the OFF population, P(all five ON draws ≤ 13.2 s) is ≈(1/5)⁵ ≈ 0.03 %: the XLI.1 jiggle was TRUNCATING every draw to the ~10–13 s band. **F9 flips the default to OFF** (`--session-jiggle=100` restores the legacy behaviour); the XLII "jiggle stays ON as a controlled constant" decision is reversed on evidence, and the "fidelity ladder" of runs 16→22 gets its final reinterpretation: the survival differences across eras were the jiggle penalty (and rig variance), never client fidelity. Run 27 also field-proved **F7** (every draw 2–5 hit the seat cooldown and queued after EXACTLY three escalating backoffs — 20+40+80 s, fourth create succeeding; no 429 at this ~3 min/draw cadence) and **F8** (zero `input_jiggle` lines, `feedback.jiggle:0` in all five summaries, activation chain flying 5/5 — the handshake is NOT the culprit; the synthetic MOTION is). Stamp **XLVI**. Run 28 = the keeper hunt: ten draws, default OFF.

### 34.1 The A/B table (all same-app 107057914, all Oct 28 evening)

| | jiggle ON (runs 23–26) | jiggle OFF (run 27) |
|---|---|---|
| draws | 5 | 5 |
| media survival (s) | 10.6, 10.4, 11.6, 13.2, 11.3 | 10.3, 21.8, 15.4, **52.2**, 22.9 |
| max | 13.2 | **52.2 (project record)** |
| median | 11.6 | 21.8 |
| draws > 13.2 s | **0 / 5** | **4 / 5** |
| rigs | 2 known IPs (23/24 same, 25 new) | **5 distinct IPs, one per draw** |
| decode integrity | 100 % (lossy draws included) | 100 % (gaps up to 7, fec up to 20) |

Including the pre-jiggle activation era (runs 18: 30.5 s, 22: ~41 s —
both OFF by construction), the OFF population is 7 draws with median
22.9 s and max 52.2 s; the ON population is 5 draws, ALL ≤ 13.2 s.
The band (~10–13 s) still occurs OFF (draw 1, which was also the
lossiest: gaps 5, withheld 147, fec 10) — so the rig's early-death
mode is not purely jiggle-induced — but the LONG draws only ever
appear when no synthetic mouse motion flies.

Mechanism candidates (unresolved, and no longer blocking): a
server-side synthetic-input detector treating the 10 Hz ±1 px
zero-drift square wave as a bot signature and killing the encoder;
input-triggered work on the parked Qt portal (cursor render / focus
churn) destabilising the rig's capture; a DRS-profile interaction
(the "EA Desktop" misclassification, §31.3). The verdict is
statistical and does not need the mechanism — but it does relocate
the freeze question from "rig-side lottery we can only ride" to
"client-controllable, at least in part".

### 34.2 Run 27 per-draw detail

| draw | rig (handoff) | backoffs before queue | media | AUs dec | gaps/fec/withheld | BWE kbps | notes |
|---|---|---|---|---|---|---|---|
| 1 | 77.111.250.39 | 0 (instant seat) | 10.32 s | 159/159 | 5 / 10 / 147 | 38 390 | the lossiest draw, died in-band |
| 2 | 80.250.97.37 | 3 (20+40+80) | 21.81 s | 656/656 | 0 / 0 / 0 | 49 140 | pristine |
| 3 | 80.84.170.153 | 3 | 15.39 s | 446/446 | 1 / 10 / 16 | 41 168 | |
| 4 | 80.84.171.151 | 3 | **52.19 s** | **1546/1546** | 1 / 2 / 20 | 37 759 | **RECORD**; ctrl_in at rel_ms 40 005 with 53 msgs — the control plane ALIVE deep into a long draw (0x0101 heartbeats flowing); 1587 AU files on disk |
| 5 | 80.84.174.149 | 3 | 22.87 s | 626/626 | 7 / 20 / 55 | 34 164 | |

Every draw: `recovery_exhausted` (the rig still dies eventually — the
lottery remains the stage-2 vehicle, now with far better odds);
`jiggle:0`; NO `input_jiggle` line; activation chain intact
(`input_activation` ×5, `version:3`, `ready:1`); `sctl_unknown` 0x020e
×5 (one per draw — a stable, harmless unknown); **no
SESSION_LENGTH_TIMER in any of the 5** (it has now appeared in 2 of
the last 8 draws — genuinely intermittent; the one-hour-budget fact of
§32.2 stands untouched). Target T10 on the XLV build: **ALL PASS 79**.

### 34.3 F7's field proof — the seat cooldown is ~2–3 minutes

Draws 2–5 each ran the IDENTICAL pattern: 30 s lottery gap → create
403 → wait 20 s → 403 → wait 40 s → 403 → wait 80 s → **fourth
create succeeds** (~170 s after the previous session's exit). Run
26's draw 2 died under F4's flat 20 s × 3 (≈90 s — not enough); F7's
escalation covers the real cooldown with one attempt to spare, and
the ~3 min/draw cadence never tripped the 429 rate limiter (run 26
tripped it at ~7 creates in 6 minutes). The 160 s fourth wait was
never needed — the schedule's shape matches the field exactly.

### 34.4 The survival record, final form (16 measured draws)

| configuration | draws | media s |
|---|---|---|
| minimal client (no activation, no jiggle) — runs 16/17 | 2 | 10.2, 11.0 |
| drip era (activation, loop pinned — runs 20/21) | 2 | ~1.3, ~1.2 (media-starved, not freezes) |
| activation, NO jiggle — runs 18, 22, 27×5 | 7 | 30.5, ~41, 10.3, 21.8, 15.4, **52.2**, 22.9 |
| activation + jiggle — runs 23–26 | 5 | 10.6, 10.4, 11.6, 13.2, 11.3 |

### 34.5 F9 and run 28 — the keeper hunt

**F9**: `sess_jiggle_ms` default 100 → **0**; usage(), the F8 comment
and the `SESS_JIGGLE_INTERVAL_MS` define all record the run-27
verdict. `--session-jiggle=100` restores the legacy jiggle for any
future re-verification (one optional ON draw inside a run-28 evening
would re-control for rig-pool era at zero extra cost).

Run 28 hunts the first **keeper** (a draw surviving its full
`--session-seconds`, which stops the lottery with rc 0):

```
knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=360 --lottery=10 --lottery-gap=30 --session-au-dir=logs\aus_r28 > logs\run28.log
```

Ten draws at the run-27 odds (median ~22 s, max 52 s so far) may or
may not produce a 360 s keeper in one evening — every draw is
distribution data regardless, and a keeper ends the run early by
design. Read matrix: a keeper ⇒ stage 2 closes against a real
gameplay-length stream (and the next variable is a gameplay-reaching
app); no keeper but draws ≥ 52 s ⇒ the tail is real and longer
evenings farm it; a re-appearance of the ~10–13 s band in EVERY draw
⇒ check the jiggle flag actually flew OFF (`feedback.jiggle:0`) and
treat it as rig-pool drift, not a regression of F9.

---

## 35. Round XLVII — R7 runs 28 + 28b: **THE FIRST KEEPERS — STAGE 2 IS CLOSED.** Run 28 (jiggle OFF, `--lottery=10`) drew 33.2 s, then 201.6 s, then a **FULL-LENGTH KEEPER: `stop_reason:"time"`, wall 360.0 s, media 359.96 s — 10 800/10 800 access units decoded (= exactly 30 fps × 360 s), 169 036/169 036 datagrams authenticated, ZERO gaps, ZERO decode errors, ZERO recovery attempts, ONE keyframe, idr/pli NEVER needed, BWE 29.2 Mbps, au_clock drift −3 ms** — and the lottery stopped on it by design (rc 0). Run 28b (the optional era re-control, jiggle ON = 100) produced **a second full-length keeper on its FIRST draw** (359.95 s, 10 800/10 800, 171 427/171 427 datagrams, `jiggle:3533` reports flown, zero gaps/errors). Two perfect six-minute sessions, both settings — the client sustains a full-length GFN stream end to end. **The 28b keeper also forces the honest correction of the XLVI verdict**: a jiggle-ON draw CAN survive, so "the jiggle is a depressant" cannot stand as stated — the same-evening survival trend (band draws 18:00–20:30Z → OFF tails 20:41Z → OFF keeper 22:05Z → ON keeper 22:22Z) shows the rig pool drifted favourable across the evening for BOTH settings, and the run-27 A/B is downgraded from "conviction" to "suggestive, era-confounded". The default stays OFF (F9 — no evidence ON helps; the conservative choice stands). Ships **F10**: honest knlog failure labels — run 28's draws 1–2 logged `E_SESSION_NO_FRAMES` ("no access unit completed") right after decoding 998 and 6049 AUs perfectly, because the pre-D8 label pair predated the recovery contract that F6a made reachable; failure codes now follow the stop_reason. Stamp **XLVII**. Run 29 = sustained stability (30-minute draws) + optionally the first gameplay-reaching app.

### 35.1 Run 28 draw by draw (jiggle OFF, T10 on XLVI first: ALL PASS 79)

| draw | seat path | stop_reason | wall | media | AUs dec | datagrams | gaps/fec | BWE kbps |
|---|---|---|---|---|---|---|---|---|
| 1 | instant | recovery_exhausted | 43.3 s | 33.22 s | 998/998 | 13 340/13 340 | 0/0 | 31 045 |
| 2 | 3 backoffs (20+40+80) | recovery_exhausted | 211.7 s | **201.59 s** | 6049/6049 | 91 941/91 941 | 0/**90** | 40 563 |
| 3 | 3 backoffs (20+40+80) | **time — KEEPER** | **360.0 s** | **359.96 s** | **10 800/10 800** | **169 036/169 036** | 0/0 | 29 172 |

Draw 2 already rewrote the endurance record threefold (52 s → 201 s,
with 90 FEC repairs and zero gaps — the loss-resilience proof at
scale); draw 3 then ran the contract to completion. Note the monotone
improvement WITHIN the run (33 → 201 → 360 s across ~25 minutes) —
the same evening-drift signature the correction below needs.

### 35.2 Keeper anatomy (draws 28/3 and 28b/1 — nearly identical)

- **Media**: 10 800 AUs = exactly 30.00 fps × 360 s; every one
  decoded; `au_clock` drift −3/−9 ms over six minutes (the RTP clock
  and the wall clock agree to 25 ppm — the transport's timing story
  is sound at session scale).
- **Integrity**: zero gaps, zero decode errors, zero withheld, zero
  recovery attempts; ONE keyframe (the opening IDR — the stream never
  needed a refresh, so `idr`/`pli` fired ZERO times; the whole
  reliable-IDR machinery of round XL simply never armed).
- **Control plane**: `0x0101` heartbeats at ~1 Hz for the full
  session (333 by rel_ms 340 023; 348 total control msgs), the 8/9-
  notification 0x010a startup burst as usual, `0x020e` ×1 (the
  standing unknown), NO SESSION_LENGTH_TIMER (2 of the last 10 draws
  carried one). The §34 healthy-draw indicator — 0x0101 alive past
  10 s — holds at the 360 s scale: heartbeats never stopped in
  either keeper.
- **Estimator**: warm throughout (25–29 Mbps estimates, 1–3 %
  util); `skip_reordered` single digits; the feedback plane's first
  six-minute soak with zero pathologies.
- **28b specifics**: `jiggle:3533` reports (~9.8/s × 360 s ✓) flew on
  sid 6 with zero outbox drops across the whole session — the input
  path is now endurance-proven too, it just isn't NEEDED.

### 35.3 The corrected jiggle verdict (XLVI §34 → XLVII)

The run-27 A/B (5 OFF draws beyond/near the ON band vs 5/5 ON draws
in-band, P ≈ 0.03 %) assumed draws are i.i.d. from a stable nightly
population. Runs 28/28b break that assumption directly: the SAME
evening went band-draws (18:00–20:30Z, both settings) → OFF tails
(20:41Z) → OFF keeper (22:05Z) → **ON keeper (22:22Z)**. A monotone
evening drift explains every observation without any jiggle effect;
so does a real depressant effect that the late-hot pool overwhelmed —
the data no longer separates them. What survives:

1. **The jiggle is NOT necessary** for any survival class (band,
   tail, or keeper draws all happened OFF; 27/28 have no jiggle and
   hold every record).
2. **The jiggle is NOT invariably fatal** (28b's keeper flew 3533
   reports and survived 360 s pristine).
3. The run 23–26 vs 27 contrast remains *suggestive* of a depressant
   effect but is era-confounded; settling it would need interleaved
   ON/OFF draws inside one run — **deprioritised**: with keepers
   reachable under both settings the question lost its practical
   stakes.
4. **Default stays OFF** (F9). Rationale unchanged: no evidence ON
   helps, the 440 B/s bot-signature risk is real and asymmetric, and
   the activation handshake — the part run 18 proved load-bearing —
   is untouched either way.

Survival-record bookkeeping: the "rig-side lottery" of §30/§34 is now
a "rig-side lottery with a hot-pool mode": within ~4 hours the same
client + app went from 5/5 band draws to 2/2 keepers. Any future A/B
MUST be same-evening-interleaved or it is unreadable (trap 17, rev 38).

### 35.4 F10 — honest failure labels

Run 28's app.log carried, for draws 1–2:
`operation_failed op=session.media code=E_SESSION_NO_FRAMES
reason="media arrived but no access unit completed..."` — next to
summaries reporting 998/998 and 6049/6049 decoded. The label pair
predates D8: back then a non-zero exit with datagrams > 0 really did
mean "no frames". F6a made `recovery_exhausted` (frames decoded,
server froze, recovery exhausted) the dominant failure class, and the
old label actively misinforms exactly there. F10 selects the failure
code from the stop_reason: `E_SESSION_NO_MEDIA` (zero datagrams),
`E_SESSION_RECOVERY_EXHAUSTED` (the common dead-draw case, reason
text points at the recovery block + transport channel),
`E_SESSION_SERVER_TERMINATION` (0x109), `E_SESSION_SOCKET_ERROR`,
`E_SESSION_NO_FRAMES` (datagrams but zero AUs — the original
meaning), `E_SESSION_FAILED_VERDICT` (contract under-delivery, e.g.
`--session-expect-frames` unmet). Stdout is untouched; only the
knlog/diag layer changes.

### 35.5 Stage 2 — CLOSED

Against BOOTSTRAP §12's G2 and the working definition carried in
START-HERE §1 ("NVST media/transport"): RTSP OPTIONS→DESCRIBE→SETUP→
ANNOUNCE→PLAY all 200 (runs 16+); access units assemble and decode
correctly at scale (10 800/10 800 in one session; sha1-per-AU
telemetry); reorder/NACK-v2/FEC survive injected AND natural loss
(runs 25–27: gaps ≤ 7, fec ≤ 90, withheld ≤ 147, 100 % decoded);
SRTP/GS/GS-header pipelines authenticated every datagram of every
run (169 036/169 036 at the keeper); the estimator port flies the
full seven-field QoS; D8's bounded recovery contract works live to
the millisecond; the full-length session completes on contract
(`stop_reason:"time"`) — twice. The stage-2 evidence set is complete;
the remaining known-unknowns (audio bundle, keyboard/wheel/text,
cursor payloads, input handshake timeout, decode-side watchdog) are
stage-3/5 scope and none of them block the media/transport claim.

**Stage 3 opens** with, in order: (1) the audio bundle — extraction
first (DTLS-SRTP keying already negotiated, `srtp_prof:1`; Opus
decode second), (2) G3's remainder behind the INPUT.md §9 caveat (no
periodic patterns without a survival test), (3) G4 cursor payload
decoding, (4) G1 input-handshake timeout, (5) D-t13 decode watchdog.

### 35.6 Run 29 — sustained stability (and maybe gameplay)

The keeper proved 360 s. Run 29 asks for **1800 s** (30 min) — the
drift/leak/estimator soak at session scale, and the natural bridge to
stage-3 testing (audio extraction lands better against long captures):

```
knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=1800 --lottery=3 --lottery-gap=30 > logs\run29.log
```

NO `--session-au-dir` this time: a 30-minute keeper writes ~54 000 AU
files (~800 MB) to the laptop for evidence the stdout telemetry
already carries (per-AU sha1/len are sampled in the log, summary
counts are exact). If a specific AU range is ever needed, a targeted
short draw with `--session-au-dir` is cheaper.

Read matrix: a 1800 s keeper ⇒ sustained-stability claim + green
light for stage-3 audio work against long sessions; a death at N
minutes with `recovery_exhausted` ⇒ the hot pool has a ceiling —
the freeze forensics (transport channel + F10's honest label) date
it exactly; if the operator can supply a GAMEPLAY-reaching app id,
substituting `--session-app=` makes run 29 the first interactive-
content test (scene-cut bitrate storms vs the parked portal's static
screen — the estimator's real exam).

## 36. Round XLVIII — R7 runs 29 + 29g: **THE ~600-SECOND SERVER CEILING IS MEASURED FROM BOTH SIDES, AND THE FIRST GAMEPLAY-REACHING APP DELIVERED A FLAWLESS 599-SECOND DRAW.** Run 29 (Arknights: Endfield, `--session-seconds=1800 --lottery=3`, XLVII stamp) never got near 30 minutes — the rig pool enforces a **session ceiling at ~600 s**: draw 1 died `recovery_exhausted` in the classic band; draw 2 streamed **17 821/17 821 AUs = 594 s of perfect 30 fps video** (280 505/280 505 datagrams, zero gaps) and then the datagram plane DECAYED GRADUALLY (444 → 0 over ~9 s — the rig-freeze signature is abrupt silence; a decay is the ceiling's) until `recovery_exhausted` at wall 604.1 s (the ~10 s recovery window included); draw 3 ran to **599.2 s and received the POLITE termination** (control `0x0109`, transport TERM `0x80008BE3` code 56 / `0x80008BC7`) → `stop_reason:"server_termination"` with **17 974/17 974 AUs, 285 078/285 078 datagrams, zero gaps**. Run 29g repeated the protocol on **Watch Dogs: Legion (app 100361011) — the first gameplay-reaching title in project history** (animated loading screens, intros, then a static settings-confirmation window): draw 1 = **a flawless 599.1 s `server_termination` keeper — 17 970/17 970 AUs decoded, zero gaps, zero decode errors** (only 54 307 datagrams — ~3 per AU: the static settings window is a low-bitrate scene, and the pipeline holds on real interactive content: scene cuts, video intros, launcher transitions); draws 2–3 died `recovery_exhausted` at **110 s and 144 s of media** (3 297 and 4 323 AUs, near-zero gaps) — long by band standards (the 10–13 s mode did NOT reproduce), short of the ceiling: the rig-free lottery still lives BELOW the ~600 s cap. **F10 matched all six draws** (every dead draw labelled `E_SESSION_RECOVERY_EXHAUSTED`, both ceiling draws `E_SESSION_SERVER_TERMINATION` — the XLVII mislabel class is extinct in the field); **F7 matched every queue** (403 → 20/40/80 s escalating backoff → create 200). Verdict: **session length is server-bounded at ~594–599 s** (silent-decay and polite-term are the ceiling's two faces); a soak must therefore target **≈570 s** (`--session-seconds=570`) so `stop_reason:"time"` remains reachable BELOW the floor — a 570 s keeper is the new full-length proof. Note for evidence readers: `bundle_rtp_in`/`bundle_rtcp_in` were counted internally in XLVII but are NOT in the summary JSON, so runs 29/29g carry **no retro audio measurement** — run 30 is the audio plane's first instrumented flight (the XLVIII `"audio"` summary block + `audio_arm`/`audio_close` events; `docs/BUNDLE-AUDIO.md`). **F11 ships** (sandbox-proven, target-pending): `kn_ogg` (OggOpus muxer) + `kn_arx` (bundle-audio SRTP receive: RFC 3711 AES_CM_128_HMAC_SHA1_80 via the RFC 5764 §4.2 DTLS-SRTP exporter → RFC 2198 RED → RFC 6716 TOC → Opus payload emit) + `--session-audio-out=F.opus` + the 34-key summary `"audio"` block, all pinned by two independent Python references (`tools/oggopus_reference.py`, `tools/arx_reference.py`) through `audio.vectors` (188 checks) and the new `test_knogg` (163) / `test_knarx` (62) suites. Stamp **XLVIII**. Run 30 = `--session-seconds=570 --lottery=2 --session-audio-out=audio.opus` (NO `--session-au-dir`): a `"time"` keeper + a playable ~570 s stereo 48 kHz .opus file.
