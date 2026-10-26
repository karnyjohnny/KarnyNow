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
#include "kn/kn_sha1.h"   /* round XXV: the STUN MESSAGE-INTEGRITY HMAC */

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

int kn_sock_udp_connect(kn_sock_udp *s, uint32_t ipv4, uint16_t port)
{
    struct sockaddr_in to;
    if (!s) return KN_SOCK_ERR_ARGS;
    if (!s->is_open || !s->fd_valid) return KN_SOCK_ERR_STATE;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    memcpy(&to.sin_addr, &ipv4, 4);
    if (connect(KN_SOCK_FD(s), (const struct sockaddr *)&to,
                (socklen_t)sizeof to) != 0) {
        s->last_error = KN_SOCK_ERRNO();
        return KN_SOCK_ERR_SOCKET;
    }
    s->peer_addr = ipv4;
    s->peer_port = port;
    s->connected = 1;
    {   /* the kernel has now chosen the route's SOURCE address — refresh
         * the local view so callers (the RTSP ANNOUNCE's
         * clientPorts.localAddress) report the real one */
        struct sockaddr_in sa;
        socklen_t sl = (socklen_t)sizeof sa;
        if (getsockname(KN_SOCK_FD(s), (struct sockaddr *)&sa, &sl) == 0) {
            s->local_port = ntohs(sa.sin_port);
            memcpy(&s->local_addr, &sa.sin_addr, 4);
        }
    }
    return KN_SOCK_OK;
}

int kn_sock_udp_sendto(kn_sock_udp *s, uint32_t ipv4, uint16_t port,
                       const uint8_t *data, size_t len)
{
    struct sockaddr_in to;
    int n;
    if (!s || (!data && len)) return KN_SOCK_ERR_ARGS;
    if (!s->is_open || !s->fd_valid) return KN_SOCK_ERR_STATE;
    if (len > KN_SOCK_MAX_DATAGRAM) return KN_SOCK_ERR_ARGS;

    if (s->connected) {
        n = send(KN_SOCK_FD(s), (const char *)data, (int)len, 0);
    } else {
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        memcpy(&to.sin_addr, &ipv4, 4);
        n = sendto(KN_SOCK_FD(s), (const char *)data, (int)len, 0,
                   (const struct sockaddr *)&to, (socklen_t)sizeof to);
    }
    if (n < 0 || (size_t)n != len) {
        s->last_error = KN_SOCK_ERRNO();
        s->send_errors++;
        return KN_SOCK_ERR_SEND;
    }
    s->send_datagrams++;
    s->send_bytes += (uint64_t)n;
    return KN_SOCK_OK;
}

/* --------------------------------------------------------- STUN toolkit
 * Round XXV — the byte-exact port of nvst.rs's authenticated-STUN subset
 * (provenance line refs in kn_sock.h). Builders produce MESSAGE-INTEGRITY
 * (HMAC-SHA1, RFC 5389 §10.5 length adjustment) + FINGERPRINT (CRC-32 ^
 * 0x5354554E) exactly like build_authenticated_stun_packet (:4977-5012);
 * inspect/check_integrity mirror handle_stun_datagram's validation gate
 * (:5131-5180) and valid_stun_message_integrity (:139-160). */

uint32_t kn_sock_crc32(const uint8_t *data, size_t len)
{
    /* reflected CRC-32 (IEEE 802.3 — the crc32fast the reference uses);
     * the table is built on first use (single-threaded orchestrators,
     * the project-wide rule). */
    static uint32_t tab[256];
    static int ready = 0;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (!ready) {
        uint32_t n;
        int k;
        for (n = 0; n < 256u; ++n) {
            uint32_t c = n;
            for (k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[n] = c;
        }
        ready = 1;
    }
    if (!data) return 0;
    for (i = 0; i < len; ++i)
        crc = tab[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* attribute walk (find_stun_attribute :5074-5092). Returns the attribute
 * HEADER pointer; *val / *vlen get the value span. NULL when absent or when
 * an attribute overruns the packet. */
static const uint8_t *stun_find_attr(const uint8_t *pkt, size_t len,
                                     uint16_t wanted, const uint8_t **val,
                                     size_t *vlen)
{
    size_t off = KN_SOCK_STUN_HEADER_LEN;
    while (off + 4u <= len) {
        uint16_t type = (uint16_t)(((uint16_t)pkt[off] << 8) | pkt[off + 1]);
        size_t alen = (size_t)(((size_t)pkt[off + 2] << 8) | pkt[off + 3]);
        size_t vs = off + 4u, ve = vs + alen;
        if (ve > len) return NULL;
        if (type == wanted) {
            *val = pkt + vs;
            *vlen = alen;
            return pkt + off;
        }
        off = (ve + 3u) & ~(size_t)3u;      /* next_multiple_of(4) */
    }
    return NULL;
}

int kn_sock_stun_build(uint8_t *out, size_t outsz, size_t *outlen,
                       uint16_t msg_type, const uint8_t tid[12],
                       const uint8_t *key, size_t keylen,
                       uint16_t attr_type, const uint8_t *attr,
                       size_t attrlen)
{
    size_t len, pad, need;
    uint16_t l;
    uint32_t crc;
    uint8_t mi[KN_SHA1_DIGEST_LEN];
    if (!out || !outlen || !tid || !key || (attrlen && !attr))
        return KN_SOCK_ERR_ARGS;
    if (attrlen > 0xFFFFu) return KN_SOCK_ERR_ARGS;
    pad = (4u - (attrlen & 3u)) & 3u;
    need = KN_SOCK_STUN_HEADER_LEN + 4u + attrlen + pad + 24u + 8u;
    if (need > outsz || need > KN_SOCK_STUN_MAX_PKT) return KN_SOCK_ERR_ARGS;
    out[0] = (uint8_t)(msg_type >> 8);
    out[1] = (uint8_t)(msg_type & 0xFF);
    out[2] = 0; out[3] = 0;               /* length: filled below */
    out[4] = 0x21; out[5] = 0x12; out[6] = 0xA4; out[7] = 0x42;
    memcpy(out + 8, tid, 12);
    len = KN_SOCK_STUN_HEADER_LEN;
    out[len++] = (uint8_t)(attr_type >> 8);
    out[len++] = (uint8_t)(attr_type & 0xFF);
    l = (uint16_t)attrlen;
    out[len++] = (uint8_t)(l >> 8);
    out[len++] = (uint8_t)(l & 0xFF);
    if (attrlen) { memcpy(out + len, attr, attrlen); len += attrlen; }
    if (pad) { memset(out + len, 0, pad); len += pad; }
    /* HMAC covers the packet with the length field adjusted to reach the
     * END of the MESSAGE-INTEGRITY attribute (:4992) */
    l = (uint16_t)(len - KN_SOCK_STUN_HEADER_LEN + 24u);
    out[2] = (uint8_t)(l >> 8);
    out[3] = (uint8_t)(l & 0xFF);
    kn_hmac_sha1(key, keylen, out, len, mi);
    out[len++] = 0x00; out[len++] = 0x08; out[len++] = 0x00; out[len++] = 20;
    memcpy(out + len, mi, sizeof mi);
    len += sizeof mi;
    /* the final length also covers the FINGERPRINT attribute (:5002) */
    l = (uint16_t)(len - KN_SOCK_STUN_HEADER_LEN + 8u);
    out[2] = (uint8_t)(l >> 8);
    out[3] = (uint8_t)(l & 0xFF);
    crc = kn_sock_crc32(out, len) ^ KN_SOCK_STUN_FINGERPRINT_XOR;
    out[len++] = 0x80; out[len++] = 0x28; out[len++] = 0x00; out[len++] = 0x04;
    out[len++] = (uint8_t)(crc >> 24);
    out[len++] = (uint8_t)((crc >> 16) & 0xFF);
    out[len++] = (uint8_t)((crc >> 8) & 0xFF);
    out[len++] = (uint8_t)(crc & 0xFF);
    *outlen = len;
    return KN_SOCK_OK;
}

int kn_sock_stun_xor_addr_v4(uint32_t ipv4_be, uint16_t port, uint8_t out[8])
{
    uint16_t xp;
    uint8_t ipb[4];
    if (!out) return KN_SOCK_ERR_ARGS;
    xp = (uint16_t)(port ^ (uint16_t)(KN_SOCK_STUN_MAGIC_COOKIE >> 16));
    out[0] = 0; out[1] = 1;               /* reserved + family IPv4 */
    out[2] = (uint8_t)(xp >> 8);
    out[3] = (uint8_t)(xp & 0xFF);
    memcpy(ipb, &ipv4_be, 4);             /* network-order bytes */
    ipb[0] = (uint8_t)(ipb[0] ^ 0x21);
    ipb[1] = (uint8_t)(ipb[1] ^ 0x12);
    ipb[2] = (uint8_t)(ipb[2] ^ 0xA4);
    ipb[3] = (uint8_t)(ipb[3] ^ 0x42);
    memcpy(out + 4, ipb, 4);
    return KN_SOCK_OK;
}

int kn_sock_stun_inspect(const uint8_t *pkt, size_t len,
                         uint16_t *msg_type, uint8_t tid[12],
                         char *user, size_t usersz, size_t *userlen,
                         uint32_t *xip, uint16_t *xport)
{
    const uint8_t *val, *fp_hdr, *mi_hdr;
    size_t vlen, flen, mlen, fp_off;
    uint32_t crc, expect;
    if (!pkt || !msg_type || !tid) return 0;
    if (userlen) *userlen = 0;
    if (user && usersz) user[0] = '\0';
    if (xip) *xip = 0;
    if (xport) *xport = 0;
    *msg_type = 0;
    memset(tid, 0, 12);
    if (len < KN_SOCK_STUN_HEADER_LEN) return 0;
    if (pkt[4] != 0x21 || pkt[5] != 0x12 || pkt[6] != 0xA4 || pkt[7] != 0x42)
        return 0;                          /* looks_like_stun (:245-248) */
    mlen = (size_t)(((size_t)pkt[2] << 8) | pkt[3]);
    if (KN_SOCK_STUN_HEADER_LEN + mlen != len) return 0;   /* exact (:5138) */
    fp_hdr = stun_find_attr(pkt, len, KN_SOCK_STUN_ATTR_FINGERPRINT,
                            &val, &flen);
    if (!fp_hdr || flen != 4u) return 0;   /* mandatory + valid (:5094) */
    fp_off = (size_t)(fp_hdr - pkt);
    if (fp_off + 8u != len) return 0;      /* FINGERPRINT is last (:5096) */
    crc = kn_sock_crc32(pkt, fp_off) ^ KN_SOCK_STUN_FINGERPRINT_XOR;
    expect = ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) |
             ((uint32_t)val[2] << 8) | (uint32_t)val[3];
    if (crc != expect) return 0;
    *msg_type = (uint16_t)(((uint16_t)pkt[0] << 8) | pkt[1]);
    memcpy(tid, pkt + 8, 12);
    if (stun_find_attr(pkt, len, KN_SOCK_STUN_ATTR_USERNAME, &val, &vlen)) {
        if (userlen) *userlen = vlen;
        if (user && usersz) {
            size_t c = vlen < usersz - 1u ? vlen : usersz - 1u;
            memcpy(user, val, c);
            user[c] = '\0';               /* honest truncation for print */
        }
    }
    mi_hdr = stun_find_attr(pkt, len, KN_SOCK_STUN_ATTR_XOR_MAPPED_ADDRESS,
                            &val, &vlen);
    if (mi_hdr && vlen == 8u && val[0] == 0 && val[1] == 1) {
        if (xport)
            *xport = (uint16_t)((((uint16_t)val[2] << 8) | val[3]) ^
                                (uint16_t)(KN_SOCK_STUN_MAGIC_COOKIE >> 16));
        if (xip) {
            uint8_t ipb[4];
            static const uint8_t cookie[4] = { 0x21, 0x12, 0xA4, 0x42 };
            int i;
            for (i = 0; i < 4; ++i)
                ipb[i] = (uint8_t)(val[4 + i] ^ cookie[i]);
            memcpy(xip, ipb, 4);           /* network-order u32 */
        }
    }
    return 1;
}

int kn_sock_stun_check_integrity(const uint8_t *pkt, size_t len,
                                 const uint8_t *key, size_t keylen)
{
    const uint8_t *mi_hdr, *fp_hdr, *mi_val, *fp_val;
    size_t mi_len, fp_len, mi_off, adjusted;
    uint8_t auth[KN_SOCK_STUN_MAX_PKT];
    uint8_t mac[KN_SHA1_DIGEST_LEN];
    uint8_t diff = 0;
    int i;
    if (!pkt || !key || len < KN_SOCK_STUN_HEADER_LEN) return 0;
    mi_hdr = stun_find_attr(pkt, len, KN_SOCK_STUN_ATTR_MESSAGE_INTEGRITY,
                            &mi_val, &mi_len);
    if (!mi_hdr || mi_len != KN_SHA1_DIGEST_LEN) return 0;
    mi_off = (size_t)(mi_hdr - pkt);
    fp_hdr = stun_find_attr(pkt, len, KN_SOCK_STUN_ATTR_FINGERPRINT,
                            &fp_val, &fp_len);
    (void)fp_val;
    adjusted = len - KN_SOCK_STUN_HEADER_LEN - (fp_hdr ? 8u : 0u);
    if (mi_off > sizeof auth) return 0;
    memcpy(auth, pkt, mi_off);
    auth[2] = (uint8_t)(adjusted >> 8);
    auth[3] = (uint8_t)(adjusted & 0xFF);
    kn_hmac_sha1(key, keylen, auth, mi_off, mac);
    for (i = 0; i < KN_SHA1_DIGEST_LEN; ++i)
        diff = (uint8_t)(diff | (mac[i] ^ mi_val[i]));   /* const-time */
    return diff == 0;
}
