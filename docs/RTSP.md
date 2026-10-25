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
