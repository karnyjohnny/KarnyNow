/* kn_sock.h — bounded UDP datagram layer for the stage-2 receive loops.
 *
 * Purpose: the platform glue between a real socket and the PURE kn_rx
 * pipeline (kn_rx never touches sockets — its header says the stage-2
 * socket work feeds process_datagram/poll; this is that work). Two knvstd
 * modes build on it:
 *   --capture   record raw datagrams to a kn_ring_dump-format file
 *               (gs_capture.hex-compatible => --replay-capture readable)
 *   --decode    live pipeline: recv -> kn_rx_video_process_datagram ->
 *               paced kn_rx_video_poll -> AU queue -> kn_decode
 *
 * Provenance (nvst.rs @ a49b124):
 *   NVST_UDP_RECEIVE_BUFFER_BYTES = 8 * 1024 * 1024   (nvst.rs:235,
 *                                                     set at :5682)
 *   UDP_RECEIVE_POLL_INTERVAL     = 10 ms             (nvst.rs:244)
 *   MAX_NVST_VIDEO_PACKET_SIZE    = 65 519            (nvst.rs:218)
 * The reference runs an async tokio receive task; this port is a plain
 * blocking-with-timeout recvfrom loop (single-threaded orchestrators in
 * knvstd; see docs/SOCK.md for the pacing/latency rationale).
 *
 * Platform: BOTH sides are real implementations (unlike kn_net's POSIX
 * stub): Win32 winsock2 (ws2_32.dll — OS-built-in, already in the F1
 * import set) and POSIX BSD sockets. The sandbox therefore runs the full
 * capture/decode E2E over loopback; the target re-proves the Win32 path
 * (sock.loopback self-test + T13).
 *
 * Design rules (project conventions):
 *   - BOUNDED: caller-owned buffers everywhere; a datagram larger than the
 *     receive buffer is reported honestly, never heap-grown.
 *   - HONEST ERRORS: every failure carries the raw platform error
 *     (WSAGetLastError / errno) in last_error; no silent fallbacks.
 *   - NO ALLOCATION: the struct is plain data; open() allocates nothing.
 *   - IPv4 ONLY for stage 2 (the cloud endpoints are A-record reachable —
 *     net.dns evidence: AAAA=0 for every GFN host; IPv6 is not a Win7
 *     story on this machine).
 *
 * Truncation semantics differ by platform (documented, tested both ways):
 *   Win32  recvfrom with an undersized buffer FAILS (WSAEMSGSIZE) and the
 *          datagram is LOST -> kn_sock_udp_recv returns KN_SOCK_ERR_MSGSIZE.
 *   POSIX  recvfrom truncates silently -> returns 1 with *out_truncated=1
 *          when the received length equals the buffer capacity (an exactly
 *          fitting datagram is indistinguishable without MSG_TRUNC; real
 *          callers size the buffer >= 65 535 so this cannot occur: the UDP
 *          payload maximum is 65 507).
 */
#ifndef KN_SOCK_H
#define KN_SOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- returns */
enum {
    KN_SOCK_OK          =  0,
    KN_SOCK_ERR_ARGS    = -1,   /* NULL/zero-size/bad string argument */
    KN_SOCK_ERR_SOCKET  = -2,   /* socket/select/recvfrom failed (last_error) */
    KN_SOCK_ERR_MSGSIZE = -3,   /* Win32: datagram > buffer, LOST (WSAEMSGSIZE) */
    KN_SOCK_ERR_SEND    = -4,   /* sendto failed (last_error) */
    KN_SOCK_ERR_STATE   = -5    /* call on a closed/never-opened socket */
};

/* Reference constants (nvst.rs lines above). */
#define KN_SOCK_RCVBUF_DEFAULT   (8u * 1024u * 1024u)  /* nvst.rs:235 */
#define KN_SOCK_POLL_INTERVAL_MS 10u                   /* nvst.rs:244 */
#define KN_SOCK_MAX_DATAGRAM     65507u                /* UDP payload max */
#define KN_SOCK_RECV_BUF         65600u                /* >= max + headroom */

/* ------------------------------------------------------- init/teardown */

/* Win32: refcounted WSAStartup(MAKEWORD(2,2)) / WSACleanup pair (kn_net
 * keeps its own independent refcount — nested WSAStartup is legal and each
 * startup must pair with exactly one cleanup). POSIX: no-ops, always 0.
 * kn_sock_udp_open calls startup internally; these exist for symmetry and
 * for callers that want the DLL loaded before anything else. */
int  kn_sock_startup(void);
void kn_sock_shutdown(void);

/* Monotonic clock — the kn_rx `now_ms` source and the capture timeline.
 * Win32: QueryPerformanceCounter (same pattern as kn_diag/kn_net).
 * POSIX: CLOCK_MONOTONIC. Origin unspecified; only deltas are meaningful.
 * now_ms never returns 0 after the first us tick (kn_rx treats 0 as "no
 * time yet"); both wrap-safe for any realistic session length. */
uint64_t kn_sock_now_us(void);
uint64_t kn_sock_now_ms(void);

/* IPv4 helpers. kn_sock_ipv4_parse: dotted quad -> network-order u32
 * (the raw in_addr byte layout). Returns 0 ok, -1 malformed.
 * kn_sock_ipv4_text: network-order u32 -> dotted quad, NUL-terminated.
 * Returns dst. buf must hold >= 16 bytes. */
int         kn_sock_ipv4_parse(const char *text, uint32_t *out);
const char *kn_sock_ipv4_text(uint32_t addr, char buf[16]);

/* ------------------------------------------------------------ UDP socket */

typedef struct kn_sock_udp {
    int      is_open;
    int      fd_valid;          /* internal: fd holds a live socket */
    uint32_t local_addr;        /* network-order bytes (post-bind) */
    uint16_t local_port;        /* host order (post-bind; 0 requested ->
                                   the OS-assigned ephemeral port) */
    uint32_t rcvbuf_requested;  /* what open() asked for */
    uint32_t rcvbuf_actual;     /* getsockopt readback — Linux caps at
                                   net.core.rmem_max (~208 KiB unprivileged);
                                   Windows honours multi-MiB requests. The
                                   capture/decode logs report BOTH so the
                                   burst headroom is never a guess. */
    int      last_error;        /* WSAGetLastError() / errno at the last
                                   failing call (0 when none) */
    /* lifetime counters (the --capture/--decode summaries report these) */
    uint64_t recv_datagrams, recv_bytes, recv_timeouts, recv_errors,
             recv_msgsize, recv_truncated;
    uint64_t send_datagrams, send_bytes, send_errors;
    int      connected;         /* kn_sock_udp_connect() applied: the
                                   kernel filters recv to this peer and
                                   sendto() uses send() */
    uint32_t peer_addr;         /* network order (when connected) */
    uint16_t peer_port;
#ifdef _WIN32
    uintptr_t fd;               /* SOCKET without a winsock type here
                                   (uintptr_t == UINT_PTR on every Win32
                                   arch mingw targets) */
#else
    int      fd;
#endif
} kn_sock_udp;

/* Create + bind a UDP/IPv4 socket.
 *   bind_ip:  dotted quad ("127.0.0.1") or NULL/"" = INADDR_ANY.
 *   port:     host order; 0 = ephemeral (read back via kn_sock_udp_port).
 *   rcvbuf:   SO_RCVBUF bytes; 0 = KN_SOCK_RCVBUF_DEFAULT (8 MiB).
 * Returns KN_SOCK_OK or a negative error (last_error filled; err/errsz get
 * a one-line human reason when non-NULL). The socket is non-blocking-ready
 * but used through recv's timeout parameter (select), never O_NONBLOCK —
 * one wait primitive, identical shape on both platforms. */
int  kn_sock_udp_open(kn_sock_udp *s, const char *bind_ip, uint16_t port,
                      uint32_t rcvbuf, char *err, size_t errsz);
void kn_sock_udp_close(kn_sock_udp *s);          /* idempotent */

uint16_t kn_sock_udp_port(const kn_sock_udp *s); /* 0 when not open */

/* Receive one datagram, waiting up to timeout_ms.
 * Returns:
 *    1  datagram stored (*out_len bytes; *out_truncated per header docs)
 *    0  timeout, no datagram (counts recv_timeouts — the poll cadence)
 *   <0  KN_SOCK_ERR_* (recv_errors/recv_msgsize counted)
 * out_len/out_truncated may be NULL. Buffers >= KN_SOCK_RECV_BUF never
 * truncate a real UDP datagram. timeout_ms 0 = pure poll (no wait). */
int  kn_sock_udp_recv(kn_sock_udp *s, uint8_t *buf, size_t cap,
                      uint32_t timeout_ms, size_t *out_len,
                      int *out_truncated);

/* Send one datagram to ipv4:port (addr in the network-order u32 form the
 * parse helper produces). Returns KN_SOCK_OK / KN_SOCK_ERR_*. Used by the
 * loopback tests today and by the stage-2 RTCP/ping senders later. */
int  kn_sock_udp_sendto(kn_sock_udp *s, uint32_t ipv4, uint16_t port,
                        const uint8_t *data, size_t len);

/* Connect the UDP socket to a fixed peer (stage-2 --session: the media
 * flow's source is the RTSP SETUP videoPeer — the kernel then DROPS any
 * datagram from another source, so the live pipeline never sees stray
 * traffic on the ephemeral port). Also makes sendto()'s destination
 * implicit (send() is used when connected). A connected socket can be
 * re-connected to a new peer (stream switch). KN_SOCK_OK / KN_SOCK_ERR_*. */
int  kn_sock_udp_connect(kn_sock_udp *s, uint32_t ipv4, uint16_t port);

/* ---------------------------------------------------- STUN toolkit (R7)
 * Round XXV: the minimal authenticated-STUN subset the NVST pre-PLAY
 * ICE/NATT burst needs — a byte-exact port of nvst.rs's builders and
 * validators (build_authenticated_stun_packet :4977-5012,
 * build_stun_binding_request :5013-5028, build_natt_hole_punch_request
 * :5033-5050, xor_mapped_address :5052-5072, find_stun_attribute
 * :5074-5092, valid_stun_fingerprint :5094-5104, looks_like_stun :245,
 * valid_stun_message_integrity :139-160). IPv4-only like the rest of
 * kn_sock (stage 2). NOT an ICE agent: no candidate gathering, no
 * role/conflict handling — the R7 run-6 measurement (PLAY 455 with zero
 * UDP sent) showed the rig needs the client's hole-punch burst BEFORE
 * PLAY; full ICE/DTLS is the stage-3 R3 spike. */

#define KN_SOCK_STUN_MAGIC_COOKIE  0x2112A442u
#define KN_SOCK_STUN_FINGERPRINT_XOR 0x5354554Eu
#define KN_SOCK_STUN_HEADER_LEN    20u
#define KN_SOCK_STUN_MAX_PKT       512u   /* builders refuse more */

enum {
    KN_SOCK_STUN_BINDING_REQUEST = 0x0001,
    KN_SOCK_STUN_BINDING_SUCCESS = 0x0101
};
enum {
    KN_SOCK_STUN_ATTR_USERNAME        = 0x0006,
    KN_SOCK_STUN_ATTR_MESSAGE_INTEGRITY = 0x0008,
    KN_SOCK_STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020,
    /* RFC 8445 §5.2: the controlling agent's nomination flag — zero
     * length. Round XXXIV (run 15's verdict): the reference runs
     * str0m's ICE agent alongside the hand-built bursts (nvst.rs:8652
     * feeds every STUN datagram to rtc.handle_input; poll_output sends
     * whatever str0m emits), and a controlling str0m nominates the
     * valid pair — the rig may gate its data plane on the nomination
     * (deviation D-t8 retires D-t1). */
    KN_SOCK_STUN_ATTR_USE_CANDIDATE   = 0x0025,
    KN_SOCK_STUN_ATTR_FINGERPRINT     = 0x8028
};

/* CRC-32 (IEEE 802.3 reflected — the crc32fast the reference uses) for
 * the STUN FINGERPRINT. Exposed for KATs. */
uint32_t kn_sock_crc32(const uint8_t *data, size_t len);

/* Build ONE authenticated single-attribute STUN packet (USERNAME or
 * XOR-MAPPED-ADDRESS) with MESSAGE-INTEGRITY (HMAC-SHA1 over the packet
 * truncated to the attribute block, length pre-adjusted per RFC 5389)
 * and FINGERPRINT. Returns KN_SOCK_OK (*outlen set; the packet never
 * exceeds KN_SOCK_STUN_MAX_PKT) / KN_SOCK_ERR_ARGS. */
int kn_sock_stun_build(uint8_t *out, size_t outsz, size_t *outlen,
                       uint16_t msg_type, const uint8_t tid[12],
                       const uint8_t *key, size_t keylen,
                       uint16_t attr_type, const uint8_t *attr,
                       size_t attrlen);

/* The two-attribute form (round XXXIV): attr1 + an OPTIONAL attr2
 * (attr2_type == 0 omits it; a zero-length attr2 like USE-CANDIDATE
 * passes attr2=NULL, attr2len=0). Attribute ORDER matters for the
 * wire bytes: attr1 first (USERNAME), attr2 second (USE-CANDIDATE) —
 * the golden in test_knsock pins the exact layout. Same MI/FP rules. */
int kn_sock_stun_build2(uint8_t *out, size_t outsz, size_t *outlen,
                        uint16_t msg_type, const uint8_t tid[12],
                        const uint8_t *key, size_t keylen,
                        uint16_t attr1_type, const uint8_t *attr1,
                        size_t attr1len,
                        uint16_t attr2_type, const uint8_t *attr2,
                        size_t attr2len);

/* The XOR-MAPPED-ADDRESS value for an IPv4 source (family 1): 8 bytes,
 * port XOR (cookie>>16), octets XOR the cookie. */
int kn_sock_stun_xor_addr_v4(uint32_t ipv4_be, uint16_t port,
                             uint8_t out[8]);

/* Structural validation + field extraction (handle_stun_datagram's
 * shared gate :5136-5146): magic cookie, the EXACT length the header
 * promises, and a valid FINGERPRINT (mandatory — the reference rejects
 * without it). Returns 1 when well-formed (*msg_type, tid always set;
 * user/xip/xport set when their attributes are present — *userlen / *xip
 * are 0-initialized first), 0 when not STUN we can use. The username is
 * copied raw (may be non-ASCII), NUL-terminated for printing; *userlen
 * carries the true length. */
int kn_sock_stun_inspect(const uint8_t *pkt, size_t len,
                         uint16_t *msg_type, uint8_t tid[12],
                         char *user, size_t usersz, size_t *userlen,
                         uint32_t *xip, uint16_t *xport);

/* MESSAGE-INTEGRITY check with the given key (valid_stun_message_
 * integrity :139-160: HMAC-SHA1 over pkt[0..MI) with the length field
 * adjusted to len-20-8 when a FINGERPRINT follows). 1 = valid. */
int kn_sock_stun_check_integrity(const uint8_t *pkt, size_t len,
                                 const uint8_t *key, size_t keylen);

#ifdef __cplusplus
}
#endif

#endif /* KN_SOCK_H */
