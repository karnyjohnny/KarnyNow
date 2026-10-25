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

#ifdef __cplusplus
}
#endif

#endif /* KN_SOCK_H */
