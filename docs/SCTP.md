# SCTP.md — kn_sctp: the SCTP-over-DTLS layer on the bundle (R3-C, round XXXIII)

> Status: **SHIPPED in the sandbox (round XXXIII, 2026-10-27)** — awaiting
> the target T10 (expect ALL PASS **76**) and R7 run 15. Provenance:
> nvst.rs + nvst_input.rs @ a49b124, str0m vendored in the reference tree,
> sctp-proto (crates.io, defaults read from the published source),
> RFC 4960 / RFC 8831 / RFC 8832, usrsctp @ c625e219 (pinned). Runs 13/14
> evidence: docs/RTSP.md §19/§20. Language rule D12: English.

## 1. Why this module exists (the measurement chain)

* Run 13: DTLS completed live (388 ms) → PLAY moved from silence to an
  explicit **400**.
* Run 14: the captured 400 (`reason:"OK"`, empty body, echo headers only)
  + **`app_records:0`** — the rig sends NO app data after the handshake.
  It waits for OURS: the reference is the SCTP initiator
  (`start_sctp(true)`, nvst.rs:8221) and fires the INIT the instant DTLS
  connects (`Event::Connected` :8214-8223), then opens the 8-channel
  NVST profile (`NvstInputChannels::create`) — all BEFORE `finish()`
  delivers PLAY (lib.rs:937→990→1117). We sent zero app data. The 400
  is the rig refusing PLAY from a peer whose data plane never showed up.

## 2. Wire parameters (every value re-read from the reference, not remembered)

| parameter | value | provenance |
|---|---|---|
| SCTP ports | 5000 ↔ 5000 | RFC 8831; str0m `fake_addr "1.1.1.1:5000"` (sctp/mod.rs:298); sctp-proto `DEFAULT_SCTP_PORT` both directions |
| OS / MIS in INIT | 65535 / 65535 | sctp-proto `TransportConfig::default()` (`u16::MAX`, config.rs) |
| a_rwnd (advertised) | 1 MiB | `INITIAL_RECV_BUF_SIZE = 1024*1024` (sctp-proto config.rs) → SO_RCVBUF 1 MiB |
| RTO initial/min/max | 3000 / 1000 / 60000 ms | sctp-proto `RTO_INITIAL/MIN/MAX`; set BOTH via sysctls and SCTP_RTOINFO (belt-and-braces against default drift) |
| INIT retransmits | unlimited | `webrtc_transport_config().with_max_init_retransmits(None)` (snap.rs:16-23) → `sinit_max_attempts = 65535` |
| outgoing fragment size | 1200 | sctp-proto `INITIAL_MTU 1228` minus DTLS record overhead → SCTP_MAXSEG 1200 (keeps every packet inside one IP datagram) |
| max send message | 64 KiB | sctp-proto `DEFAULT_MAX_MESSAGE_SIZE` — kn_sctp_send's ceiling |
| checksum | CRC-32c | RFC 4960 §6.8 (usrsctp internal; the test verifies it with an INDEPENDENT parser against RFC 3720 B.4 goldens) |

## 3. The channel profile (nvst_input.rs NVST_CHANNEL_PROFILE, all 8)

The bundle case (`create(rtc, !mjolnir)` → bundle_video=true) opens ALL
EIGHT: the 6 base channels + cursor + rtcp. Client-initiated association
→ the client owns the EVEN stream ids (RFC 8832 §5.1):

| sid | label | ordered | DCEP type byte | rel param |
|---|---|---|---|---|
| 0 | control_channel_reliable | yes | 0x00 | 0 |
| 2 | custom_message_on_sctp_private_reliable | yes | 0x00 | 0 |
| 4 | custom_message_on_sctp_private_partially_reliable | yes | 0x01 (Rexmit) | 2 |
| 6 | control_channel_partially_reliable | NO | 0x82 (Timed|U) | 300 |
| 8 | control_channel_unreliable | NO | 0x81 (Rexmit|U) | 0 |
| 10 | input_channel_partially_reliable | NO | 0x82 | 300 |
| 12 | cursor_channel | yes | 0x00 | 0 |
| 14 | rtcp_on_sctp_private | yes | 0x00 | 0 |

DCEP OPEN = RFC 8832 §4.2 (12-byte head + label + protocol), sent
ORDERED+RELIABLE with PPI 50 on the channel's own stream (str0m
`write_with_ppi(.., Dcep)`, sctp/mod.rs:855-863); **priority 0, protocol
""** (`From<&ChannelConfig> for DcepOpen`, :1311-1322). Incoming OPEN
(the rig may open its own channels, odd sids): answer with the 1-byte
ACK 0x02 on the same stream (:1046-1060). `rtcp_on_sctp_private` (sid
14) is the RTCP-feedback channel — "a must for One SDK video" (:411);
stage 3 sends the kn_rx NACK-v2/IDR/RFI encoders there.

## 4. Architecture (the thread map)

```
                 pump thread (single owner of everything)
  bundle socket ── recv ──> demux: STUN | DTLS(20..63) | PING
                                │
                                v  kn_dtls_inject ── kn_dtls_poll
                                │      (mbedTLS decrypts; app records
                                │       land in kn_dtls's app ring)
                                v  kn_dtls_read_app
                            kn_sctp_inject ──> usrsctp_conninput
                                │                    │
                                │              usrsctp stack
                                │                    │ conn_output
                                │             (ALSO from usrsctp's
                                │              internal timer thread!)
                                v                    v
                            kn_sctp_poll  <── bounded mutex tx queue
                                │           (THE thread boundary: the
                                v            mbedTLS ssl context must
                        kn_dtls_write_app    stay single-threaded)
                                │
                                v
                          bundle socket send
```

* The tx queue is the ONLY cross-thread surface (usrsctp's timer thread
  may emit retransmits at any moment); everything else runs on the pump
  thread. The queue is bounded (64×2048); overflow drops NEW packets,
  counted — SCTP's own retransmission recovers (RFC 4960 §6).
* AF_CONN routing: one registered key per process (the instance itself,
  the libdatachannel pattern). usrsctp NEVER opens a real socket
  (`usrsctp_init(0, …)`) — DTLS is the wire.
* Lifecycle: the pump thread creates kn_sctp the moment `dtls_ready`
  fires (mirroring `Event::Connected → start_sctp(true)`), opens the 8
  channels on COMM_UP, and services the association for the whole
  session (persist mode). Main closes it AFTER the join, BEFORE
  kn_dtls_free (its write path).

## 5. Landmines found and fixed (all measured, all pinned by tests)

1. **`usrsctp_recvv` needs `*infolen` PRE-SET to `sizeof(sctp_rcvinfo)`**
   (user_socket.c:1164: the fill branch requires
   `*infolen >= sizeof(struct sctp_rcvinfo)`). Passing 0 silently yields
   `SCTP_RECVV_NOINFO` and every message reports **sid 0 / ppi 0** — the
   loopback bring-up showed all 8 OPENs arriving stream-blind.
2. **`SCTP_RECVRCVINFO` is a PLAIN int sockopt** (Manual.md:363 and every
   bundled example: `setsockopt(so, IPPROTO_SCTP, SCTP_RECVRCVINFO,
   &on, sizeof(int))`), NOT an `SCTP_EVENT` subscription — the
   event-struct form is accepted (rc 0) and silently no-ops.
3. **`sctp_sndinfo.snd_ppid` / `rcvinfo.rcv_ppid` are NETWORK order**
   (RFC 6458; the libdatachannel convention): send `htonl(ppi)`, read
   `ntohl(rcv_ppid)`. usrsctp copies the value verbatim onto the wire.
4. **`usrsctp_conninput(addr, …)` stamps BOTH src and dst endpoint
   addresses with `addr`** (user_socket.c:3285-3296 — only the ports
   come from the packet). Consequence for the in-process loopback test:
   both instances must share ONE AF_CONN key and the test relay routes
   by the packet's DESTINATION PORT (client 5001, server 5000). With
   per-instance keys every packet's source address disagrees with the
   association's remote address and the stack ABORTs (measured:
   chunk 0x06 right after the INIT-ACK). Production is immune (one
   instance per process).
5. **Never call `usrsctp_conninput` from inside `conn_output`** — that
   is re-entry while the stack holds its internal locks: DEADLOCK
   (measured: the first raw harness hung inside `usrsctp_connect`).
   The tx-queue decoupling is what makes the pump design legal.
6. `usrsctp_accept` only yields ESTABLISHED associations — for a server
   side, accept IS the COMM_UP (the notification may have fired on the
   listener before the swap; the test server treats accept as up and
   re-subscribes on the accepted socket anyway).
7. The DCEP parser must REJECT channel types > 0x02 (RFC 8832 defines
   only 0x00/0x01/0x02) — the golden battery pins it.
8. **BUILD (the SIXTH target catch, round XXXIII hotfix):** never call
   `target_link_libraries(usrsctp …)` from OUR CMakeLists — usrsctp's
   own CMakeLists uses the PLAIN signature (it links `ws2_32 iphlpapi`
   itself on WIN32, usrsctplib/CMakeLists.txt:195-198) and CMake
   forbids mixing plain with keyword signatures on one target. The
   first target T10 died at configure on exactly this — a WIN32-only
   branch the POSIX sandbox never executes (the round-XXII class).
   **The permanent gate:** cross_check.sh now CROSS-BUILDS usrsctp for
   mingw x64 and LINKS the real test suite with the exact F1 flags —
   proving the static closure (imports: KERNEL32/msvcrt/WS2_32/
   IPHLPAPI only — all Win7-built-in, no winpthread DLL) before the
   target ever configures.

## 6. Verification (the two-implementations rule)

* `tools/sctp_reference.py` — independent pure-Python: CRC-32c (proven
  against the RFC 3720 B.4 check value 0xE3069283 BEFORE emitting),
  DCEP marshal (hand-transcribed goldens), the NVST profile table, a
  complete INIT packet builder. Emits `streamer/src/kn_sctp_kat_vectors.h`
  (byte-stable regeneration).
* `test_knsctp` (16th suite, 33 checks real / 13 stub):
  goldens ↔ `kn_sctp_dcep_open_build` byte-equality for all 8 channels;
  parse battery (valid + 6 malformed); CRC goldens; the golden INIT
  through the independent parser; **live usrsctp INIT capture** (ports
  5000↔5000, vtag 0, CRC verifies, OS/MIS 65535/65535, a_rwnd ≥ 64 KiB);
  full 4-way association over the memory relay (INIT → INIT-ACK →
  COOKIE-ECHO → COOKIE-ACK); the 8-channel burst received with the EXACT
  sids/labels/types/params and ACKed back; user data both ways (sid 0 /
  sid 14, PPI BINARY); a one-shot packet loss recovered by SACK/
  fast-retransmit; garbage inject tolerated; stats coherence; clean
  close (ASan+UBSan+LSan clean).
* Build gates: CMake ctest 18/18 ×2 TLS backends; cross_check compiles
  the kn_sctp real body AND the test's real section under the exact
  target flags ×2 arches (the round-XXII rule); T10 downloads the
  pinned usrsctp tarball (c625e219…, SHA-256 verified) and runs the
  real battery on Win7.

## 7. Deviations (numbered, per R32)

* **D-t4 (updated):** post-handshake DTLS app records are no longer
  discarded — they feed usrsctp. Non-DCEP messages from the rig (its
  control/RTCP payload) are counted and their first record's head
  captured in `ice_preplay` (`sctp_first*`) — interpretation is stage 3.
* **D-t5:** PLAY is gated on `sctp_up` with a BOUNDED wait
  (`--session-play-wait`, default 2500 ms) + a settle window
  (`--session-play-settle`, default 250 ms). The reference does not
  explicitly wait (its engine plumbing gives the receivers an implicit
  head start); runs 13/14 sent PLAY ~12 ms after dtls_ready with zero
  SCTP and drew the rig's 400. The wait never eats the PLAY budget:
  on DTLS/SCTP failure PLAY is attempted anyway (the evidence lines
  say exactly what state the stack was in).
* **D-t6:** no stream-reset / negotiated channels / SNAP
  (sctp-proto features the reference does not use against GFN rigs:
  `negotiated: None` on every channel config, nvst_input.rs:314).
* **D-t7:** teardown ABORTs the association (linger 0) instead of a
  graceful SHUTDOWN — the session is ending anyway; matches the
  WebRTC close style. Revisit only if a rig-side complaint is measured.
* The in-test loopback server (`kn_sctp_open_server` /
  `kn_sctp_open_peer`) is TEST infrastructure — production never
  listens (the rig is always the SCTP server, `start_sctp(true)`).

## 8. Open questions — run 15 ANSWERED the first one negatively

1. ~~Does the rig's 400 disappear once the association is up?~~ —
   **MEASURED (run 15, RTSP.md §21): the association never came up.**
   The INIT left (dtls_out 3→4, sctp_arm armed:1 — usrsctp live on
   Win7) and the rig stayed SILENT (sctp_up:0 after the full bounded
   wait, app_records:0, no COMM_LOST) → PLAY 400 #4. A well-formed
   client INIT alone does not move it. The round-XXXIV response: ICE
   NOMINATION (USE-CANDIDATE, D-t8) — the hypothesis is that the rig's
   SCTP endpoint arms only on a SELECTED pair (run 16 decides), with
   the mystery post-ready DTLS record (now printed as `post_first`) as
   the fallback clue: an alert would point at the record layer /
   use_srtp instead.
2. Does the rig open channels of its own (odd sids) or send control
   messages right after COMM_UP? (`sctp_rig_opened` / `sctp_msgs_in` /
   `sctp_first*` in ice_preplay answer it.)
3. Does PLAY 200 + media flow follow immediately, or does the rig also
   want an RTCP RR / keepalive on `rtcp_on_sctp_private` before video
   starts? (stage-3 senders are built and KAT'd — wiring is a small
   step once the channel is proven live.)
