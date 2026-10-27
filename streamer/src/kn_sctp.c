/* kn_sctp.c — SCTP-over-DTLS on the bundle (see kn_sctp.h for the
 * contract + provenance). Three sections: the PURE DCEP wire helpers
 * (identical in every build, golden-tested), the real usrsctp body
 * (KN_SCTP_BACKEND_USRSCTP), and the honest stub. The kn_cert/kn_dtls
 * discipline: exactly one definition set per configuration. */
#include "kn/kn_sctp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================== PURE DCEP helpers (all builds) =================
 * Byte-exact ports of str0m's dcep.rs (vendor/str0m/src/sctp/dcep.rs
 * marshal_to / TryFrom<&[u8>), RFC 8832 §4.2/§4.3:
 *
 *   0        1        2        3
 *   |MsgType |ChanType|     Priority      |
 *   |        Reliability Parameter        |
 *   |   Label Length  |  Protocol Length  |
 *   | Label... | Protocol... |
 */

int kn_sctp_dcep_open_build(uint8_t *out, size_t outsz, size_t *outlen,
                            int unordered, uint8_t channel_type,
                            uint16_t priority, uint32_t rel_param,
                            const char *label, const char *protocol)
{
    size_t ll, pl, need;
    if (!out || !outlen || !label || !protocol) return KN_SCTP_ERR_ARGS;
    if (channel_type > 0x02u) return KN_SCTP_ERR_ARGS;  /* RFC 8832 set */
    ll = strlen(label);
    pl = strlen(protocol);
    if (ll > 0xFFFFu || pl > 0xFFFFu) return KN_SCTP_ERR_ARGS;
    need = 12u + ll + pl;
    if (outsz < need) return KN_SCTP_ERR_ARGS;
    out[0] = KN_SCTP_DCEP_MSG_OPEN;
    out[1] = (uint8_t)(channel_type | (unordered ? KN_SCTP_DCEP_UNORDERED : 0u));
    out[2] = (uint8_t)(priority >> 8);
    out[3] = (uint8_t)(priority & 0xFFu);
    out[4] = (uint8_t)(rel_param >> 24);
    out[5] = (uint8_t)(rel_param >> 16);
    out[6] = (uint8_t)(rel_param >> 8);
    out[7] = (uint8_t)(rel_param & 0xFFu);
    out[8]  = (uint8_t)(ll >> 8);
    out[9]  = (uint8_t)(ll & 0xFFu);
    out[10] = (uint8_t)(pl >> 8);
    out[11] = (uint8_t)(pl & 0xFFu);
    memcpy(out + 12, label, ll);
    memcpy(out + 12 + ll, protocol, pl);
    *outlen = need;
    return KN_SCTP_OK;
}

int kn_sctp_dcep_ack_build(uint8_t *out, size_t outsz, size_t *outlen)
{
    if (!out || !outlen || outsz < 1) return KN_SCTP_ERR_ARGS;
    out[0] = KN_SCTP_DCEP_MSG_ACK;
    *outlen = 1;
    return KN_SCTP_OK;
}

int kn_sctp_dcep_parse(const uint8_t *data, size_t len, int *unordered,
                       uint8_t *channel_type, uint16_t *priority,
                       uint32_t *rel_param,
                       char *label, size_t labelsz,
                       char *protocol, size_t protosz)
{
    size_t ll, pl;
    if (!data || len < 1) return 0;
    if (data[0] == KN_SCTP_DCEP_MSG_ACK)
        return len >= 1 ? KN_SCTP_DCEP_MSG_ACK : 0;
    if (data[0] != KN_SCTP_DCEP_MSG_OPEN) return 0;
    if (len < 12) return 0;
    if ((data[1] & 0x7Fu) > KN_SCTP_DCEP_TIMED) return 0; /* RFC 8832:
        only 0x00/0x01/0x02 are defined — refuse the rest (the golden
        battery pins this; str0m's ReliabilityType has no other arms) */
    ll = ((size_t)data[8] << 8) | data[9];
    pl = ((size_t)data[10] << 8) | data[11];
    if (len < 12u + ll + pl) return 0;
    if (unordered)    *unordered = (data[1] & KN_SCTP_DCEP_UNORDERED) ? 1 : 0;
    if (channel_type) *channel_type = (uint8_t)(data[1] & 0x7Fu);
    if (priority)     *priority = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
    if (rel_param) {
        *rel_param = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16)
                   | ((uint32_t)data[6] << 8) | data[7];
    }
    if (label && labelsz) {
        size_t n = ll < labelsz - 1 ? ll : labelsz - 1;
        memcpy(label, data + 12, n);
        label[n] = '\0';
    }
    if (protocol && protosz) {
        size_t n = pl < protosz - 1 ? pl : protosz - 1;
        memcpy(protocol, data + 12 + ll, n);
        protocol[n] = '\0';
    }
    return KN_SCTP_DCEP_MSG_OPEN;
}

#ifdef KN_SCTP_BACKEND_USRSCTP
/* ==================== the real body (vendored usrsctp) ============== */

#include <usrsctp.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>   /* htons/ntohl (usrsctp.h pulls these too) */
#  include <windows.h>
#  define KN_SCTP_MUTEX            CRITICAL_SECTION
#  define kn_sctp_mutex_init(m)    InitializeCriticalSection(m)
#  define kn_sctp_mutex_lock(m)    EnterCriticalSection(m)
#  define kn_sctp_mutex_unlock(m)  LeaveCriticalSection(m)
#  define kn_sctp_mutex_destroy(m) DeleteCriticalSection(m)
#else
#  include <arpa/inet.h>  /* htons/ntohl */
#  include <pthread.h>
#  define KN_SCTP_MUTEX            pthread_mutex_t
#  define kn_sctp_mutex_init(m)    pthread_mutex_init((m), NULL)
#  define kn_sctp_mutex_lock(m)    pthread_mutex_lock(m)
#  define kn_sctp_mutex_unlock(m)  pthread_mutex_unlock(m)
#  define kn_sctp_mutex_destroy(m) pthread_mutex_destroy(m)
#endif

#include "kn/kn_sock.h"   /* the ms clock (up_ms) */

struct kn_sctp {
    struct socket *so;
    kn_sctp_write_fn write_cb;
    void *write_ctx;
    /* The AF_CONN routing key: production = this instance (a private
     * token — the DTLS layer is the real wire); the in-process
     * loopback TEST shares the server's key between both instances
     * because usrsctp_conninput stamps src AND dst with the injected
     * key (user_socket.c:3285-3296) — a per-instance key would make
     * every packet's source address disagree with the association's
     * remote address and the stack would ABORT (measured). bind,
     * connect, conninput and the registry all use this one key; the
     * packet PORTS (5001/5000 in the test) do the demultiplexing. */
    void *conn_key;

    /* the tx thread boundary: usrsctp's conn_output may fire from its
     * internal timer thread; the mbedTLS ssl context must stay single-
     * threaded (the pump owns it) — packets queue here and leave only
     * from kn_sctp_poll */
    KN_SCTP_MUTEX tx_lock;
    uint8_t  txq[KN_SCTP_TXQ_SLOTS][KN_SCTP_MAX_PKT];
    uint16_t txlen[KN_SCTP_TXQ_SLOTS];
    unsigned tx_head, tx_count;

    int established, lost, closed;
    int server_mode, accepted;
    uint64_t t_open, t_up;
    kn_sctp_stats st;
};

/* the one global usrsctp init (process-wide, like the reference's
 * install_crypto call_once). udp_encapsulation_port 0 = no real sockets
 * are ever opened (AF_CONN only — DTLS is our wire). */
static int g_sctp_inited = 0;

/* The AF_CONN routing registry: conn_output may fire from the usrsctp
 * timer thread with ANY registered address, so the trampoline never
 * blind-casts — it looks the key up here. Bounded (4 instances: one
 * live session + the test pair + slack), mutex-guarded. An unknown key
 * is refused (counted), never dereferenced. */
#define KN_SCTP_REG_SLOTS 4
static kn_sctp *g_sctp_reg[KN_SCTP_REG_SLOTS];
static KN_SCTP_MUTEX g_sctp_reg_lock;
static int g_sctp_reg_inited = 0;
static unsigned long g_sctp_unknown_addr;

static void sctp_reg_lock_init(void)
{
    if (!g_sctp_reg_inited) {
        kn_sctp_mutex_init(&g_sctp_reg_lock);
        g_sctp_reg_inited = 1;
    }
}

static int sctp_reg_add(kn_sctp *s)
{
    int i, rc = -1;
    sctp_reg_lock_init();
    kn_sctp_mutex_lock(&g_sctp_reg_lock);
    for (i = 0; i < KN_SCTP_REG_SLOTS; ++i) {
        if (!g_sctp_reg[i]) { g_sctp_reg[i] = s; rc = 0; break; }
    }
    kn_sctp_mutex_unlock(&g_sctp_reg_lock);
    return rc;
}

static void sctp_reg_del(kn_sctp *s)
{
    int i;
    if (!g_sctp_reg_inited) return;
    kn_sctp_mutex_lock(&g_sctp_reg_lock);
    for (i = 0; i < KN_SCTP_REG_SLOTS; ++i)
        if (g_sctp_reg[i] == s) g_sctp_reg[i] = NULL;
    kn_sctp_mutex_unlock(&g_sctp_reg_lock);
}

static kn_sctp *sctp_reg_find(void *addr)
{
    kn_sctp *found = NULL;
    int i;
    if (!g_sctp_reg_inited) return NULL;
    kn_sctp_mutex_lock(&g_sctp_reg_lock);
    for (i = 0; i < KN_SCTP_REG_SLOTS; ++i) {
        if ((void *)g_sctp_reg[i] == addr) { found = g_sctp_reg[i]; break; }
    }
    kn_sctp_mutex_unlock(&g_sctp_reg_lock);
    return found;
}

static int kn_sctp_conn_output(void *addr, void *buffer, size_t length,
                               uint8_t tos, uint8_t set_df)
{
    kn_sctp *s = sctp_reg_find(addr);
    unsigned slot;
    (void)tos; (void)set_df;
    if (!s || !buffer || length == 0 || length > KN_SCTP_MAX_PKT) {
        if (!s) g_sctp_unknown_addr++;
        return -1;
    }
    kn_sctp_mutex_lock(&s->tx_lock);
    if (s->closed) {
        kn_sctp_mutex_unlock(&s->tx_lock);
        return -1;
    }
    if (s->tx_count >= KN_SCTP_TXQ_SLOTS) {
        s->st.txq_dropped++;
        kn_sctp_mutex_unlock(&s->tx_lock);
        return -1;              /* bounded: SCTP retransmits (RFC 4960) */
    }
    slot = (s->tx_head + s->tx_count) % KN_SCTP_TXQ_SLOTS;
    memcpy(s->txq[slot], buffer, length);
    s->txlen[slot] = (uint16_t)length;
    s->tx_count++;
    kn_sctp_mutex_unlock(&s->tx_lock);
    return 0;
}

int kn_sctp_available(void) { return 1; }

/* The reference profile — nvst_input.rs NVST_CHANNEL_PROFILE, all 8
 * entries (the bundle case opens cursor + rtcp too). Ordered channels
 * map to DCEP type RELIABLE, MaxRetransmits(n) to REXMIT with
 * parameter n, MaxPacketLifetime(ms) to TIMED with parameter ms;
 * unordered sets the 0x80 bit (str0m sctp/mod.rs:1325-1337). */
const kn_sctp_channel_def kn_sctp_nvst_channels[KN_SCTP_NVST_N_CHANNELS] = {
    {  0, 0, KN_SCTP_DCEP_RELIABLE,   0, "control_channel_reliable" },
    {  2, 0, KN_SCTP_DCEP_RELIABLE,   0,
      "custom_message_on_sctp_private_reliable" },
    {  4, 0, KN_SCTP_DCEP_REXMIT,     2,
      "custom_message_on_sctp_private_partially_reliable" },
    {  6, 1, KN_SCTP_DCEP_TIMED,    300,
      "control_channel_partially_reliable" },
    {  8, 1, KN_SCTP_DCEP_REXMIT,     0, "control_channel_unreliable" },
    { 10, 1, KN_SCTP_DCEP_TIMED,    300,
      "input_channel_partially_reliable" },
    { 12, 0, KN_SCTP_DCEP_RELIABLE,   0, "cursor_channel" },
    { 14, 0, KN_SCTP_DCEP_RELIABLE,   0, "rtcp_on_sctp_private" },
};

int kn_sctp_open_channels(kn_sctp *s, const kn_sctp_channel_def *defs, int n)
{
    int i, sent = 0;
    if (!s || !defs || n <= 0) return 0;
    for (i = 0; i < n; ++i) {
        uint8_t msg[256];
        size_t len = 0;
        if (kn_sctp_dcep_open_build(msg, sizeof msg, &len,
                                    defs[i].unordered,
                                    defs[i].channel_type, 0u,
                                    defs[i].rel_param, defs[i].label,
                                    "") != KN_SCTP_OK)
            break;
        if (kn_sctp_send(s, defs[i].sid, KN_SCTP_PPI_DCEP, 1, msg, len)
            != KN_SCTP_OK)
            break;
        sent++;
    }
    return sent;
}

static void kn_sctp_global_init_once(void)
{
    if (!g_sctp_inited) {
        /* no debug printf (stdout is the JSONL contract); sysctls:
         * mirror the sctp-proto WebRTC defaults where usrsctp differs
         * (RTO initial/min/max 3000/1000/60000 = the same values, but
         * set explicitly so a usrsctp default drift can never change
         * our wire — docs/SCTP.md §2) */
        usrsctp_init(0, kn_sctp_conn_output, NULL);
        usrsctp_sysctl_set_sctp_rto_initial_default(3000);
        usrsctp_sysctl_set_sctp_rto_min_default(1000);
        usrsctp_sysctl_set_sctp_rto_max_default(60000);
        usrsctp_sysctl_set_sctp_valid_cookie_life_default(60000);
        /* no ECN on the DTLS wire (str0m's association doesn't use it) */
        g_sctp_inited = 1;
    }
}

static kn_sctp *sctp_open_common(kn_sctp_write_fn write_cb, void *write_ctx,
                                 int server_mode, void *peer_key,
                                 uint16_t local_port, uint16_t remote_port,
                                 char *err, size_t errsz);

kn_sctp *kn_sctp_open(kn_sctp_write_fn write_cb, void *write_ctx,
                      char *err, size_t errsz)
{
    /* the production client: both AF_CONN keys are the instance itself
     * (the libdatachannel pattern — the key is a private routing token,
     * the wire peer lives behind the DTLS layer), ports 5000<->5000 */
    return sctp_open_common(write_cb, write_ctx, 0, NULL,
                            KN_SCTP_PORT, KN_SCTP_PORT, err, errsz);
}

kn_sctp *kn_sctp_open_server(kn_sctp_write_fn write_cb, void *write_ctx,
                             char *err, size_t errsz)
{
    return sctp_open_common(write_cb, write_ctx, 1, NULL,
                            KN_SCTP_PORT, KN_SCTP_PORT, err, errsz);
}

kn_sctp *kn_sctp_open_peer(kn_sctp_write_fn write_cb, void *write_ctx,
                           void *peer_key, uint16_t local_port,
                           uint16_t remote_port, char *err, size_t errsz)
{
    return sctp_open_common(write_cb, write_ctx, 0, peer_key,
                            local_port, remote_port, err, errsz);
}

static kn_sctp *sctp_open_common(kn_sctp_write_fn write_cb, void *write_ctx,
                                 int server_mode, void *peer_key,
                                 uint16_t local_port, uint16_t remote_port,
                                 char *err, size_t errsz)
{
    kn_sctp *s = NULL;
    struct sockaddr_conn sac;
    struct sctp_initmsg initmsg;
    struct sctp_rtoinfo rto;
    struct sctp_assoc_value maxseg;
    struct sctp_event ev;
    int on = 1, rc;
    int bufsz = 1024 * 1024;    /* a_rwnd 1 MiB = sctp-proto's
                                   INITIAL_RECV_BUF_SIZE */

    if (err && errsz) err[0] = '\0';
    if (!write_cb) {
        if (err && errsz) snprintf(err, errsz, "write_cb is required");
        return NULL;
    }
    kn_sctp_global_init_once();
    s = (kn_sctp *)calloc(1, sizeof *s);
    if (!s) {
        if (err && errsz) snprintf(err, errsz, "alloc failed");
        return NULL;
    }
    s->write_cb = write_cb;
    s->write_ctx = write_ctx;
    s->server_mode = server_mode;
    s->conn_key = peer_key ? peer_key : (void *)s;
    kn_sctp_mutex_init(&s->tx_lock);
    s->t_open = kn_sock_now_ms();

    if (sctp_reg_add(s) != 0) {
        if (err && errsz) snprintf(err, errsz, "registry full");
        kn_sctp_close(s);
        return NULL;
    }
    usrsctp_register_address(s->conn_key);   /* conn_output routing */
    s->so = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP,
                           NULL, NULL, 0, NULL);
    if (!s->so) {
        if (err && errsz) snprintf(err, errsz, "usrsctp_socket errno %d",
                                   errno);
        kn_sctp_close(s);
        return NULL;
    }
    usrsctp_set_non_blocking(s->so, on);

    /* the reference's association shape (docs/SCTP.md §2):
     * a_rwnd 1 MiB (INITIAL_RECV_BUF_SIZE), OS/MIS 65535/65535
     * (sctp-proto TransportConfig defaults), unlimited INIT retries
     * (webrtc_transport_config: with_max_init_retransmits(None)),
     * RTO 3000/1000/60000 (sctp-proto RTO_INITIAL/MIN/MAX). */
    rc  = usrsctp_setsockopt(s->so, SOL_SOCKET, SO_RCVBUF,
                             (const void *)&bufsz, sizeof bufsz);
    rc |= usrsctp_setsockopt(s->so, SOL_SOCKET, SO_SNDBUF,
                             (const void *)&bufsz, sizeof bufsz);
    memset(&initmsg, 0, sizeof initmsg);
    initmsg.sinit_num_ostreams = 65535u;
    initmsg.sinit_max_instreams = 65535u;
    initmsg.sinit_max_attempts = 65535u;   /* effectively unlimited */
    initmsg.sinit_max_init_timeo = 60;     /* seconds (RFC 4960 unit) */
    rc |= usrsctp_setsockopt(s->so, IPPROTO_SCTP, SCTP_INITMSG,
                             (const void *)&initmsg, sizeof initmsg);
    memset(&rto, 0, sizeof rto);
    rto.srto_assoc_id = SCTP_ALL_ASSOC;
    rto.srto_initial = 3000;
    rto.srto_min = 1000;
    rto.srto_max = 60000;
    rc |= usrsctp_setsockopt(s->so, IPPROTO_SCTP, SCTP_RTOINFO,
                             (const void *)&rto, sizeof rto);
    memset(&maxseg, 0, sizeof maxseg);
    maxseg.assoc_id = SCTP_FUTURE_ASSOC;
    maxseg.assoc_value = 1200;   /* INITIAL_MTU 1228 minus DTLS/record
                                  * overhead — keeps every SCTP packet
                                  * inside one unfragmented IP datagram
                                  * (sctp-proto INITIAL_MTU) */
    rc |= usrsctp_setsockopt(s->so, IPPROTO_SCTP, SCTP_MAXSEG,
                             (const void *)&maxseg, sizeof maxseg);
    if (rc != 0) {
        if (err && errsz) snprintf(err, errsz, "setsockopt errno %d",
                                   errno);
        kn_sctp_close(s);
        return NULL;
    }

    /* AF_CONN: the address is an opaque routing key — ours. Both ports
     * are the WebRTC convention 5000 (RFC 8831). */
    /* Subscribe ASSOC_CHANGE (the COMM_UP/COMM_LOST evidence) AND
     * RECVRCVINFO BEFORE bind/listen so accepted sockets inherit both.
     * RECVRCVINFO is what makes recvv fill sctp_rcvinfo (sid/ssn/ppid/
     * flags) — without it every message reports sid 0 / ppi 0 (measured
     * in the loopback bring-up: the OPENs arrived stream-blind). */
    memset(&ev, 0, sizeof ev);
    ev.se_assoc_id = SCTP_ALL_ASSOC;
    ev.se_type = SCTP_ASSOC_CHANGE;
    ev.se_on = 1;
    rc |= usrsctp_setsockopt(s->so, IPPROTO_SCTP, SCTP_EVENT,
                             (const void *)&ev, sizeof ev);
    {   /* RECVRCVINFO is a PLAIN int option (usrsctp Manual.md:363 +
         * every bundled example) — NOT an SCTP_EVENT subscription; the
         * event-struct form silently no-ops (measured: sid/ppi came
         * back zero without this) */
        int rcvinfo_on = 1;
        rc |= usrsctp_setsockopt(s->so, IPPROTO_SCTP, SCTP_RECVRCVINFO,
                                 (const void *)&rcvinfo_on,
                                 sizeof rcvinfo_on);
    }
    memset(&sac, 0, sizeof sac);
#ifdef HAVE_SA_LEN
    sac.sconn_len = sizeof sac;
#endif
    sac.sconn_family = AF_CONN;
    sac.sconn_port = htons(local_port);
    sac.sconn_addr = s->conn_key;
    if (usrsctp_bind(s->so, (struct sockaddr *)&sac,
                     (socklen_t)sizeof sac) != 0) {
        if (err && errsz) snprintf(err, errsz, "bind errno %d", errno);
        kn_sctp_close(s);
        return NULL;
    }
    if (server_mode) {
        /* the test/loopback peer side: listen; kn_sctp_poll accepts the
         * first association (one session per instance, like production) */
        if (usrsctp_listen(s->so, 1) != 0) {
            if (err && errsz)
                snprintf(err, errsz, "listen errno %d", errno);
            kn_sctp_close(s);
            return NULL;
        }
        return s;
    }
    {
        struct sockaddr_conn pac = sac;   /* sconn_addr = conn_key:
         * production = our own token (the libdatachannel pattern — the
         * real peer lives behind DTLS); the loopback test shares the
         * server's key so both endpoints agree on the address while
         * the ports demux (see the conn_key comment). */
        pac.sconn_port = htons(remote_port);
        if (usrsctp_connect(s->so, (struct sockaddr *)&pac,
                            (socklen_t)sizeof pac) != 0
            && errno != EINPROGRESS && errno != EWOULDBLOCK
            && errno != EINTR && errno != 0) {
            if (err && errsz)
                snprintf(err, errsz, "connect errno %d", errno);
            kn_sctp_close(s);
            return NULL;
        }
    }
    /* the INIT is queued for conn_output already — the first poll
     * flushes it into the DTLS record stream */
    return s;
}

int kn_sctp_inject(kn_sctp *s, const uint8_t *data, size_t len)
{
    if (!s || !data || len == 0) return KN_SCTP_ERR_ARGS;
    if (s->closed) return KN_SCTP_ERR_STATE;
    s->st.pkts_in++;
    s->st.bytes_in += (unsigned)len;
    /* the registered key routes this through the stack */
    usrsctp_conninput(s->conn_key, data, len, 0);
    return KN_SCTP_OK;
}

int kn_sctp_poll(kn_sctp *s, kn_sctp_msg_fn on_msg, void *msg_ctx,
                 kn_sctp_event_fn on_event, void *ev_ctx)
{
    uint8_t rxbuf[16 * 1024];
    if (!s) return KN_SCTP_ERR_ARGS;
    if (s->closed) return KN_SCTP_ERR_STATE;
    /* 1. flush the tx queue through write_cb (pump-thread context —
     *    the mbedTLS single-thread rule) */
    for (;;) {
        uint8_t pkt[KN_SCTP_MAX_PKT];
        size_t plen;
        kn_sctp_mutex_lock(&s->tx_lock);
        if (s->tx_count == 0) {
            kn_sctp_mutex_unlock(&s->tx_lock);
            break;
        }
        plen = s->txlen[s->tx_head];
        memcpy(pkt, s->txq[s->tx_head], plen);
        s->tx_head = (s->tx_head + 1u) % KN_SCTP_TXQ_SLOTS;
        s->tx_count--;
        kn_sctp_mutex_unlock(&s->tx_lock);
        if (s->write_cb(s->write_ctx, pkt, plen) != 0) {
            s->st.send_errors++;
            /* a DTLS send failure is surfaced by kn_dtls itself; the
             * SCTP layer keeps its state (retransmission recovers) */
        } else {
            s->st.pkts_out++;
            s->st.bytes_out += (unsigned)plen;
        }
    }
    if (s->lost) return KN_SCTP_ERR_LOST;
    /* server side (test/loopback): accept the first association once */
    if (s->server_mode && !s->accepted) {
        struct socket *acc = usrsctp_accept(s->so, NULL, NULL);
        if (acc) {
            struct sctp_event ev2;
            usrsctp_set_non_blocking(acc, 1);
            memset(&ev2, 0, sizeof ev2);
            ev2.se_assoc_id = SCTP_ALL_ASSOC;
            ev2.se_type = SCTP_ASSOC_CHANGE;
            ev2.se_on = 1;
            (void)usrsctp_setsockopt(acc, IPPROTO_SCTP, SCTP_EVENT,
                                     &ev2, sizeof ev2);
            {
                int rcvinfo_on = 1;
                (void)usrsctp_setsockopt(acc, IPPROTO_SCTP,
                                         SCTP_RECVRCVINFO,
                                         (const void *)&rcvinfo_on,
                                         sizeof rcvinfo_on);
            }
            usrsctp_close(s->so);   /* the listener is single-use */
            s->so = acc;
            s->accepted = 1;
            /* usrsctp_accept only yields ESTABLISHED associations —
             * that IS the server's COMM_UP (the notification itself may
             * have fired before the re-subscription above) */
            s->established = 1;
            s->st.established = 1;
            if (!s->t_up) s->t_up = kn_sock_now_ms();
        }
    }
    /* 2. drain notifications + stream data (non-blocking) */
    for (;;) {
        union sctp_notification *sn;
        struct sctp_rcvinfo rinfo;
        /* recvv only fills rcvinfo when *infolen ENTERS the call at
         * least sizeof(rcvinfo) (user_socket.c:1164) — a zeroed
         * infolen silently yields SCTP_RECVV_NOINFO (measured) */
        socklen_t rinfo_len = (socklen_t)sizeof rinfo;
        unsigned int infotype = 0;
        int nflags = 0;
        ssize_t n;
        memset(&rinfo, 0, sizeof rinfo);
        n = usrsctp_recvv(s->so, rxbuf, sizeof rxbuf, NULL, NULL,
                          &rinfo, &rinfo_len, &infotype, &nflags);
        if (n <= 0) break;
        if (nflags & MSG_NOTIFICATION) {
            sn = (union sctp_notification *)rxbuf;
            if (sn->sn_header.sn_type == SCTP_ASSOC_CHANGE) {
                uint16_t state = sn->sn_assoc_change.sac_state;
                s->st.notif_assoc++;
                if (state == SCTP_COMM_UP || state == SCTP_RESTART) {
                    if (!s->established) {
                        s->established = 1;
                        s->t_up = kn_sock_now_ms();
                    }
                    s->st.established = 1;
                    if (on_event) on_event(ev_ctx, 1);
                } else if (state == SCTP_COMM_LOST ||
                           state == SCTP_CANT_STR_ASSOC ||
                           state == SCTP_SHUTDOWN_COMP) {
                    s->established = 0;
                    s->lost = (state == SCTP_SHUTDOWN_COMP) ? 0 : 1;
                    s->st.established = 0;
                    s->st.lost = s->lost;
                    if (on_event) on_event(ev_ctx, 0);
                }
            }
            continue;
        }
        s->st.msgs_in++;
        if (on_msg) {
            /* infotype says what recvv filled: SCTP_RECVV_RCVINFO for
             * data with rcvinfo (our subscription), NOOPT otherwise —
             * then sid/ppi are unknown and reported as 0, honestly */
            int have_info = (infotype == SCTP_RECVV_RCVINFO);
            on_msg(msg_ctx,
                   have_info ? rinfo.rcv_sid : 0,
                   have_info ? rinfo.rcv_ssn : 0,
                   have_info ? ntohl(rinfo.rcv_ppid) : 0,
                   (have_info && (rinfo.rcv_flags & SCTP_UNORDERED)) ? 1 : 0,
                   rxbuf, (size_t)n);
        }
    }
    return s->lost ? KN_SCTP_ERR_LOST : KN_SCTP_OK;
}

int kn_sctp_established(const kn_sctp *s)
{
    return (s && s->established) ? 1 : 0;
}

void *kn_sctp_key(const kn_sctp *s) { return s ? s->conn_key : NULL; }

int kn_sctp_send(kn_sctp *s, uint16_t sid, uint32_t ppi, int ordered,
                 const uint8_t *data, size_t len)
{
    struct sctp_sndinfo si;
    ssize_t n;
    if (!s || (!data && len)) return KN_SCTP_ERR_ARGS;
    if (!s->established || s->lost || s->closed) return KN_SCTP_ERR_STATE;
    /* messages up to the reference's send ceiling are legal — usrsctp
     * fragments them into MTU-sized DATA chunks itself (str0m's
     * DEFAULT/LOCAL max message sizes: 64 KiB send / 256 KiB recv) */
    if (len > 65536u) return KN_SCTP_ERR_ARGS;
    memset(&si, 0, sizeof si);
    si.snd_sid = sid;
    si.snd_flags = ordered ? 0 : SCTP_UNORDERED;
    si.snd_ppid = htonl(ppi);   /* RFC 6458: the ppid travels in
                                 * NETWORK order (usrsctp copies it
                                 * verbatim onto the wire — the
                                 * libdatachannel convention) */
    n = usrsctp_sendv(s->so, data, len, NULL, 0, &si,
                      (socklen_t)sizeof si, SCTP_SENDV_SNDINFO, 0);
    if (n < 0) {
        s->st.send_errors++;
        s->st.last_errno = errno;
        return KN_SCTP_ERR_SEND;
    }
    s->st.sends++;
    return KN_SCTP_OK;
}

/* Bump the DCEP counters from the caller's message callback path (the
 * pump thread owns them; keeping them in the module means every
 * consumer's evidence line reports the same numbers). */
void kn_sctp_note_opened(kn_sctp *s) { if (s) s->st.opened++; }
void kn_sctp_note_acked(kn_sctp *s)  { if (s) s->st.acked++; }

void kn_sctp_stats_get(const kn_sctp *s, kn_sctp_stats *out)
{
    if (!s || !out) return;
    kn_sctp_mutex_lock((KN_SCTP_MUTEX *)&s->tx_lock);
    *out = s->st;                    /* txq_dropped lives in st (the
                                      * conn_output side bumps it under
                                      * this same lock) */
    out->established = s->established;
    out->lost = s->lost;
    kn_sctp_mutex_unlock((KN_SCTP_MUTEX *)&s->tx_lock);
}

uint64_t kn_sctp_up_ms(const kn_sctp *s)
{
    if (!s || !s->established) return 0;
    return s->t_up - s->t_open;
}

void kn_sctp_close(kn_sctp *s)
{
    if (!s) return;
    /* order matters: mark closed (a late conn_output from the usrsctp
     * timer thread then drops its packet), THEN close the socket (its
     * final ABORT may still traverse conn_output — allowed, s is alive
     * and the mutex is not held), THEN deregister and destroy. */
    kn_sctp_mutex_lock(&s->tx_lock);
    s->closed = 1;
    s->tx_count = 0;
    kn_sctp_mutex_unlock(&s->tx_lock);
    if (s->so) {
        struct linger lg;
        lg.l_onoff = 1;
        lg.l_linger = 0;        /* ABORT on close — the WebRTC style */
        (void)usrsctp_setsockopt(s->so, SOL_SOCKET, SO_LINGER, &lg,
                                 sizeof lg);
        usrsctp_close(s->so);
        s->so = NULL;
    }
    usrsctp_deregister_address(s->conn_key);
    sctp_reg_del(s);
    kn_sctp_mutex_destroy(&s->tx_lock);
    memset(s, 0, sizeof *s);
    free(s);
}

#else
/* ============================ the honest stub ========================
 * No usrsctp in this build: available() == 0, open() refuses loudly,
 * and knvstd logs sctp_arm {"armed":0,"reason":"backend_unavailable"}
 * — the DTLS session then simply carries no SCTP (D-t4 behaviour). */

struct kn_sctp { int unused; };

int kn_sctp_available(void) { return 0; }

kn_sctp *kn_sctp_open(kn_sctp_write_fn write_cb, void *write_ctx,
                      char *err, size_t errsz)
{
    (void)write_cb; (void)write_ctx;
    if (err && errsz)
        snprintf(err, errsz,
                 "kn_sctp unavailable (built without usrsctp)");
    return NULL;
}

kn_sctp *kn_sctp_open_server(kn_sctp_write_fn write_cb, void *write_ctx,
                             char *err, size_t errsz)
{
    return kn_sctp_open(write_cb, write_ctx, err, errsz);
}

kn_sctp *kn_sctp_open_peer(kn_sctp_write_fn write_cb, void *write_ctx,
                           void *peer_key, uint16_t local_port,
                           uint16_t remote_port, char *err, size_t errsz)
{
    (void)peer_key; (void)local_port; (void)remote_port;
    return kn_sctp_open(write_cb, write_ctx, err, errsz);
}

int kn_sctp_inject(kn_sctp *s, const uint8_t *data, size_t len)
{
    (void)s; (void)data; (void)len;
    return KN_SCTP_ERR_STATE;
}

int kn_sctp_poll(kn_sctp *s, kn_sctp_msg_fn on_msg, void *msg_ctx,
                 kn_sctp_event_fn on_event, void *ev_ctx)
{
    (void)s; (void)on_msg; (void)msg_ctx; (void)on_event; (void)ev_ctx;
    return KN_SCTP_ERR_STATE;
}

int kn_sctp_established(const kn_sctp *s) { (void)s; return 0; }

void *kn_sctp_key(const kn_sctp *s) { (void)s; return NULL; }

int kn_sctp_send(kn_sctp *s, uint16_t sid, uint32_t ppi, int ordered,
                 const uint8_t *data, size_t len)
{
    (void)sid; (void)ppi; (void)ordered;
    if (!s || (!data && len)) return KN_SCTP_ERR_ARGS;
    return KN_SCTP_ERR_BACKEND;
}

const kn_sctp_channel_def kn_sctp_nvst_channels[KN_SCTP_NVST_N_CHANNELS] = {
    {  0, 0, KN_SCTP_DCEP_RELIABLE,   0, "control_channel_reliable" },
    {  2, 0, KN_SCTP_DCEP_RELIABLE,   0,
      "custom_message_on_sctp_private_reliable" },
    {  4, 0, KN_SCTP_DCEP_REXMIT,     2,
      "custom_message_on_sctp_private_partially_reliable" },
    {  6, 1, KN_SCTP_DCEP_TIMED,    300,
      "control_channel_partially_reliable" },
    {  8, 1, KN_SCTP_DCEP_REXMIT,     0, "control_channel_unreliable" },
    { 10, 1, KN_SCTP_DCEP_TIMED,    300,
      "input_channel_partially_reliable" },
    { 12, 0, KN_SCTP_DCEP_RELIABLE,   0, "cursor_channel" },
    { 14, 0, KN_SCTP_DCEP_RELIABLE,   0, "rtcp_on_sctp_private" },
};

int kn_sctp_open_channels(kn_sctp *s, const kn_sctp_channel_def *defs, int n)
{
    (void)s; (void)defs; (void)n;
    return 0;
}

void kn_sctp_note_opened(kn_sctp *s) { (void)s; }
void kn_sctp_note_acked(kn_sctp *s)  { (void)s; }

void kn_sctp_stats_get(const kn_sctp *s, kn_sctp_stats *out)
{
    (void)s;
    if (out) memset(out, 0, sizeof *out);
}

uint64_t kn_sctp_up_ms(const kn_sctp *s) { (void)s; return 0; }

void kn_sctp_close(kn_sctp *s) { (void)s; }

#endif /* KN_SCTP_BACKEND_USRSCTP */
