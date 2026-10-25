/* kn_sock.c — bounded UDP datagram layer (see kn_sock.h for the contract).
 *
 * Win32: winsock2. Landmines inherited from kn_net (docs/NET.md) and
 * respected here: <winsock2.h> BEFORE <windows.h>; WIN32_LEAN_AND_MEAN;
 * ws2_32 linked explicitly; select()'s first argument is ignored on
 * Windows (pass 0 for portability of the call shape).
 * POSIX: BSD sockets + poll-free select (same code shape both sides).
 */
/* See kn_diag.c/kn_selftest.c: the feature macro must precede EVERY system
 * include or glibc hides clock_gettime/CLOCK_MONOTONIC (hit in round XVII
 * on the first host compile — the header itself pulls in <stddef.h>). */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_sock.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>   /* socklen_t (mingw: NOT in winsock2.h alone —
                             same landmine kn_net.c documents) */
#  include <windows.h>
#  define KN_SOCK_INVALID_FD ((uintptr_t)INVALID_SOCKET)
#  define KN_SOCK_ERRNO()    WSAGetLastError()
#  define KN_SOCK_CLOSE(fd)  closesocket((SOCKET)(fd))
typedef SOCKET kn_sock_fd_t;
#  define KN_SOCK_FD(s)      ((kn_sock_fd_t)(s)->fd)
#else
#  include <errno.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <time.h>
#  define KN_SOCK_INVALID_FD (-1)
#  define KN_SOCK_ERRNO()    errno
#  define KN_SOCK_CLOSE(fd)  close(fd)
typedef int kn_sock_fd_t;
#  define KN_SOCK_FD(s)      ((s)->fd)
#endif

/* ------------------------------------------------------- init/teardown */

#ifdef _WIN32
static int g_kn_sock_wsa_refs;

int kn_sock_startup(void)
{
    WSADATA w;
    if (g_kn_sock_wsa_refs > 0) { g_kn_sock_wsa_refs++; return KN_SOCK_OK; }
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return KN_SOCK_ERR_SOCKET;
    g_kn_sock_wsa_refs = 1;
    return KN_SOCK_OK;
}

void kn_sock_shutdown(void)
{
    if (g_kn_sock_wsa_refs > 0 && --g_kn_sock_wsa_refs == 0) WSACleanup();
}
#else
int  kn_sock_startup(void)  { return KN_SOCK_OK; }
void kn_sock_shutdown(void) { }
#endif

/* --------------------------------------------------------- monotonic */

uint64_t kn_sock_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER pc;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&pc);
    if (!freq.QuadPart) return 1;   /* never 0: kn_rx reads 0 as "no time" */
    return (uint64_t)((double)pc.QuadPart * 1000000.0 / (double)freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 1;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

uint64_t kn_sock_now_ms(void)
{
    uint64_t us = kn_sock_now_us();
    return us / 1000u ? us / 1000u : 1u;
}

/* ------------------------------------------------------------ ipv4 */

int kn_sock_ipv4_parse(const char *text, uint32_t *out)
{
    unsigned a, b, c, d;
    int n;
    char tail;
    if (!text || !out) return -1;
    tail = '\0';
    n = sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail);
    if (n != 4 || a > 255u || b > 255u || c > 255u || d > 255u) return -1;
#ifdef _WIN32
    *out = ((uint32_t)a) | ((uint32_t)b << 8) | ((uint32_t)c << 16) |
           ((uint32_t)d << 24);          /* network order = LE on the wire */
#else
    *out = htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                 ((uint32_t)c << 8) | (uint32_t)d);
#endif
    return 0;
}

const char *kn_sock_ipv4_text(uint32_t addr, char buf[16])
{
    if (!buf) return "";
    snprintf(buf, 16, "%u.%u.%u.%u",
             (unsigned)(addr & 0xFFu), (unsigned)((addr >> 8) & 0xFFu),
             (unsigned)((addr >> 16) & 0xFFu), (unsigned)((addr >> 24) & 0xFFu));
    return buf;
}

/* --------------------------------------------------------------- UDP */

static void kn_sock_err(char *err, size_t errsz, const char *fmt, int code)
{
    if (err && errsz) snprintf(err, errsz, "%s (errno %d)", fmt, code);
}

int kn_sock_udp_open(kn_sock_udp *s, const char *bind_ip, uint16_t port,
                     uint32_t rcvbuf, char *err, size_t errsz)
{
    struct sockaddr_in sa;
    uint32_t want;
    int optval;
    socklen_t optlen = (socklen_t)sizeof optval;
    socklen_t salen  = (socklen_t)sizeof sa;
    kn_sock_fd_t fd;

    if (!s) return KN_SOCK_ERR_ARGS;
    memset(s, 0, sizeof *s);
    s->fd = KN_SOCK_INVALID_FD;
    if (err && errsz) err[0] = '\0';

    if (kn_sock_startup() != KN_SOCK_OK) {
        kn_sock_err(err, errsz, "WSAStartup failed", KN_SOCK_ERRNO());
        return KN_SOCK_ERR_SOCKET;
    }
    /* is_open=1 from here on ONLY as the WSA-refcount ownership marker:
     * every error path below closes through kn_sock_udp_close(), which
     * pairs the startup. I/O calls are gated by fd_valid, not is_open, so
     * a half-open socket can never recv/send. (Leak found in round-XVII
     * self-review: bad bind_ip/bind() failure used to skip WSACleanup.) */
    s->is_open = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) {
#else
    if (fd < 0) {
#endif
        s->last_error = KN_SOCK_ERRNO();
        kn_sock_err(err, errsz, "socket() failed", s->last_error);
        kn_sock_shutdown();
        return KN_SOCK_ERR_SOCKET;
    }
#ifdef _WIN32
    s->fd = (uintptr_t)fd;
#else
    s->fd = fd;
#endif
    s->fd_valid = 1;

    want = rcvbuf ? rcvbuf : KN_SOCK_RCVBUF_DEFAULT;
    s->rcvbuf_requested = want;
    optval = (int)want;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&optval,
                   (socklen_t)sizeof optval) != 0) {
        s->last_error = KN_SOCK_ERRNO();
        kn_sock_err(err, errsz, "SO_RCVBUF setsockopt failed", s->last_error);
        kn_sock_udp_close(s);
        return KN_SOCK_ERR_SOCKET;
    }
    optval = 0;
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&optval, &optlen) == 0)
        s->rcvbuf_actual = (uint32_t)optval;   /* Linux: 2x the stored cap */

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (bind_ip && bind_ip[0]) {
        uint32_t a;
        if (kn_sock_ipv4_parse(bind_ip, &a) != 0) {
            if (err && errsz) snprintf(err, errsz, "bad bind_ip '%.64s'",
                                       bind_ip);
            kn_sock_udp_close(s);
            return KN_SOCK_ERR_ARGS;
        }
        memcpy(&sa.sin_addr, &a, 4);
    } else {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        s->last_error = KN_SOCK_ERRNO();
        kn_sock_err(err, errsz, "bind() failed", s->last_error);
        kn_sock_udp_close(s);
        return KN_SOCK_ERR_SOCKET;
    }
    if (getsockname(fd, (struct sockaddr *)&sa, &salen) == 0) {
        s->local_port = ntohs(sa.sin_port);
        memcpy(&s->local_addr, &sa.sin_addr, 4);
    } else {
        s->local_port = port;
    }
    return KN_SOCK_OK;
}

void kn_sock_udp_close(kn_sock_udp *s)
{
    if (!s) return;
    if (s->fd_valid) {
        KN_SOCK_CLOSE(s->fd);
        s->fd = KN_SOCK_INVALID_FD;
        s->fd_valid = 0;
    }
    if (s->is_open) {
        s->is_open = 0;
        kn_sock_shutdown();     /* pairs the startup inside open() */
    }
}

uint16_t kn_sock_udp_port(const kn_sock_udp *s)
{
    return (s && s->is_open) ? s->local_port : 0u;
}

int kn_sock_udp_recv(kn_sock_udp *s, uint8_t *buf, size_t cap,
                     uint32_t timeout_ms, size_t *out_len, int *out_truncated)
{
    fd_set rfds;
    struct timeval tv;
    struct sockaddr_in from;
    socklen_t fromlen = (socklen_t)sizeof from;
    int sel, n;
    kn_sock_fd_t fd;

    if (out_len) *out_len = 0;
    if (out_truncated) *out_truncated = 0;
    if (!s || !buf || cap == 0) return KN_SOCK_ERR_ARGS;
    if (!s->is_open || !s->fd_valid) return KN_SOCK_ERR_STATE;
    fd = KN_SOCK_FD(s);

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec  = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
#ifdef _WIN32
    sel = select(0, &rfds, NULL, NULL, &tv);   /* nfds ignored on Windows */
#else
    sel = select(fd + 1, &rfds, NULL, NULL, &tv);
#endif
    if (sel < 0) {
        s->last_error = KN_SOCK_ERRNO();
        s->recv_errors++;
        return KN_SOCK_ERR_SOCKET;
    }
    if (sel == 0) { s->recv_timeouts++; return 0; }

    n = recvfrom(fd, (char *)buf, (int)cap, 0,
                 (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        s->last_error = KN_SOCK_ERRNO();
#ifdef _WIN32
        if (s->last_error == WSAEMSGSIZE) {
            /* the datagram did NOT fit and winsock DISCARDED it — honest
             * loss, counted separately (see the header's truncation docs) */
            s->recv_msgsize++;
            return KN_SOCK_ERR_MSGSIZE;
        }
#endif
        s->recv_errors++;
        return KN_SOCK_ERR_SOCKET;
    }
    s->recv_datagrams++;
    s->recv_bytes += (uint64_t)n;
    if (out_len) *out_len = (size_t)n;
#ifndef _WIN32
    if ((size_t)n == cap && out_truncated) *out_truncated = 1;
#endif
    return 1;
}

int kn_sock_udp_sendto(kn_sock_udp *s, uint32_t ipv4, uint16_t port,
                       const uint8_t *data, size_t len)
{
    struct sockaddr_in to;
    int n;
    if (!s || (!data && len)) return KN_SOCK_ERR_ARGS;
    if (!s->is_open || !s->fd_valid) return KN_SOCK_ERR_STATE;
    if (len > KN_SOCK_MAX_DATAGRAM) return KN_SOCK_ERR_ARGS;

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    memcpy(&to.sin_addr, &ipv4, 4);
    n = sendto(KN_SOCK_FD(s), (const char *)data, (int)len, 0,
               (const struct sockaddr *)&to, (socklen_t)sizeof to);
    if (n < 0 || (size_t)n != len) {
        s->last_error = KN_SOCK_ERRNO();
        s->send_errors++;
        return KN_SOCK_ERR_SEND;
    }
    s->send_datagrams++;
    s->send_bytes += (uint64_t)n;
    return KN_SOCK_OK;
}
