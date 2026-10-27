/* test_knsctp.c — the SIXTEENTH suite: kn_sctp (R3-C, docs/SCTP.md).
 * Self-adaptive (the kn_cert/kn_dtls pattern): with the CMake usrsctp
 * backend it runs the REAL battery — the wire goldens, an independent
 * INIT-packet parse with CRC-32c verification, and a full client<->
 * server association over an in-test relay with the 8-channel NVST
 * DCEP profile (the rig's shape: labels/sids/reliability byte-checked
 * against the Python reference's goldens). Every other build checks the
 * honest-stub contract. Either path is a full pass.
 *
 * The two-implementations rule: the DCEP/INIT goldens come from
 * tools/sctp_reference.py (independent Python; self-proved against the
 * RFC 3720 CRC-32c check value before emitting), and the SCTP-packet
 * parser below is written from RFC 4960 directly — it shares NO code
 * with usrsctp, so a wrong stack configuration cannot hide.
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_sctp.h"
#include "kn/kn_sock.h"
#include "test_port.h"
#include "kn_sctp_kat_vectors.h"

#include <string.h>

/* ------------------------------------------------- the independent wire
 * layer (RFC 4960 §3 common header + §3.3.2 INIT; CRC per §6.8/appendix
 * B — the reflected Castagnoli walk, proven against the RFC 3720 B.4
 * check value by the generator). */
static uint32_t t_crc32c(const uint8_t *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < n; ++i) {
        crc ^= d[i];
        for (b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static int t_pkt_crc_ok(const uint8_t *pkt, size_t len)
{
    uint8_t tmp[KN_SCTP_MAX_PKT];
    uint32_t want, got;
    if (len < 12 || len > sizeof tmp) return 0;
    memcpy(tmp, pkt, len);
    got = (uint32_t)tmp[8] | ((uint32_t)tmp[9] << 8) |
          ((uint32_t)tmp[10] << 16) | ((uint32_t)tmp[11] << 24);
    tmp[8] = tmp[9] = tmp[10] = tmp[11] = 0;
    want = t_crc32c(tmp, len);
    return got == want;
}

typedef struct {
    uint16_t sport, dport;
    uint32_t vtag;
    uint8_t  chunk_type, chunk_flags;
    uint16_t chunk_len;
    uint32_t init_tag, a_rwnd, initial_tsn;
    uint16_t os, mis;
} t_sctp_pkt;

static int t_parse(const uint8_t *pkt, size_t len, t_sctp_pkt *out)
{
    const uint8_t *c;
    if (len < 12 + 20) return -1;
    out->sport = (uint16_t)((pkt[0] << 8) | pkt[1]);
    out->dport = (uint16_t)((pkt[2] << 8) | pkt[3]);
    out->vtag  = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                 ((uint32_t)pkt[6] << 8) | pkt[7];
    c = pkt + 12;
    out->chunk_type  = c[0];
    out->chunk_flags = c[1];
    out->chunk_len   = (uint16_t)((c[2] << 8) | c[3]);
    if (out->chunk_len > len - 12) return -2;
    if (out->chunk_type == 0x01) {          /* INIT */
        out->init_tag = ((uint32_t)c[4] << 24) | ((uint32_t)c[5] << 16) |
                        ((uint32_t)c[6] << 8) | c[7];
        out->a_rwnd   = ((uint32_t)c[8] << 24) | ((uint32_t)c[9] << 16) |
                        ((uint32_t)c[10] << 8) | c[11];
        out->os  = (uint16_t)((c[12] << 8) | c[13]);
        out->mis = (uint16_t)((c[14] << 8) | c[15]);
        out->initial_tsn = ((uint32_t)c[16] << 24) | ((uint32_t)c[17] << 16) |
                           ((uint32_t)c[18] << 8) | c[19];
    }
    return 0;
}

/* ------------------------------------------------------- pure battery
 * (every build: the DCEP helpers are identical in all configurations) */

static void test_dcep_goldens(void)
{
    int i;
    int all_ok = 1, parse_ok = 1;
    printf("-- DCEP goldens vs the independent Python reference --\n");
    for (i = 0; i < KN_SCTP_KAT_N_CHANNELS; ++i) {
        const kn_sctp_channel_kat *k = &kn_sctp_kat_channels[i];
        uint8_t buf[256];
        size_t len = 0;
        int u = 0, prt;
        uint8_t ct = 0xFF;
        uint16_t prio = 0xFFFF;
        uint32_t rp = 0xFFFFFFFFu;
        char label[KN_SCTP_MAX_LABEL], proto[KN_SCTP_MAX_PROTO];
        if (kn_sctp_dcep_open_build(buf, sizeof buf, &len, k->unordered,
                                    k->channel_type, 0, k->rel_param,
                                    k->label, "") != KN_SCTP_OK ||
            len != k->len || memcmp(buf, k->bytes, len) != 0)
            all_ok = 0;
        /* round-trip through the parser */
        label[0] = proto[0] = '\0';
        prt = kn_sctp_dcep_parse(k->bytes, k->len, &u, &ct, &prio, &rp,
                                 label, sizeof label, proto, sizeof proto);
        if (prt != KN_SCTP_DCEP_MSG_OPEN || u != k->unordered ||
            ct != k->channel_type || prio != 0 || rp != k->rel_param ||
            strcmp(label, k->label) != 0 || proto[0] != '\0')
            parse_ok = 0;
    }
    CHECK(all_ok, "all 8 NVST channel OPENs are byte-identical to the "
                  "Python golden (labels/sids/reliability of "
                  "nvst_input.rs NVST_CHANNEL_PROFILE)");
    CHECK(parse_ok, "every golden round-trips through kn_sctp_dcep_parse "
                    "(unordered bit, type, priority 0, rel param, label)");
    {   /* the profile itself matches the reference table exactly */
        int i2, ok = 1;
        for (i2 = 0; i2 < 8; ++i2) {
            const kn_sctp_channel_def *d = &kn_sctp_nvst_channels[i2];
            const kn_sctp_channel_kat *k = &kn_sctp_kat_channels[i2];
            if (d->sid != k->sid || d->unordered != k->unordered ||
                d->channel_type != k->channel_type ||
                d->rel_param != k->rel_param ||
                strcmp(d->label, k->label) != 0)
                ok = 0;
        }
        CHECK(ok, "kn_sctp_nvst_channels == the Python-transcribed "
                  "reference profile (8 channels, sids 0..14)");
    }
    {   /* the one-byte DATA_CHANNEL_ACK (RFC 8832 §4.3) */
        uint8_t a[4];
        size_t alen = 0;
        CHECK(kn_sctp_dcep_ack_build(a, sizeof a, &alen) == KN_SCTP_OK &&
              alen == sizeof kn_sctp_kat_ack &&
              memcmp(a, kn_sctp_kat_ack, alen) == 0 &&
              kn_sctp_dcep_ack_build(a, 0, &alen) == KN_SCTP_ERR_ARGS,
              "ACK builds to the golden 0x02 byte; a zero buffer refuses");
    }
    {   /* the parse battery: valid OPENs, the ACK, a protocol-carrying
         * OPEN, and six malformed inputs that must be REJECTED */
        int i4, pok = 1;
        for (i4 = 0; i4 < KN_SCTP_KAT_N_PARSE; ++i4) {
            const kn_sctp_dcep_parse_kat *k = &kn_sctp_kat_parse[i4];
            int u = 0, prt;
            uint8_t ct = 0;
            uint16_t prio = 0;
            uint32_t rp = 0;
            char label[KN_SCTP_MAX_LABEL], proto[KN_SCTP_MAX_PROTO];
            prt = kn_sctp_dcep_parse(k->data, k->len, &u, &ct, &prio, &rp,
                                     label, sizeof label,
                                     proto, sizeof proto);
            if (prt != k->expect) { pok = 0; continue; }
            if (k->expect == KN_SCTP_DCEP_MSG_OPEN) {
                if (u != k->unordered || ct != k->channel_type ||
                    rp != k->rel_param || strcmp(label, k->label) != 0 ||
                    strcmp(proto, k->protocol) != 0)
                    pok = 0;
            }
        }
        CHECK(pok, "the parse battery: every valid vector decodes exactly, "
                   "every malformed vector is refused (0)");
    }
    {   /* CRC-32c goldens + the golden INIT packet */
        size_t i5;
        int cok = 1;
        t_sctp_pkt p;
        memset(&p, 0, sizeof p);   /* target GCC 16.2 -Wmaybe-
                                     uninitialized: t_parse fills the INIT
                                     fields conditionally (the round-XXVII
                                     warning-class discipline) */
        for (i5 = 0; i5 < KN_SCTP_KAT_N_CRC32C; ++i5) {
            if (t_crc32c((const uint8_t *)kn_sctp_kat_crc32c[i5].in,
                         strlen(kn_sctp_kat_crc32c[i5].in)) !=
                kn_sctp_kat_crc32c[i5].crc)
                cok = 0;
        }
        CHECK(cok, "CRC-32c matches the Python goldens (incl. the RFC "
                   "3720 B.4 check value 0xE3069283)");
        CHECK(t_pkt_crc_ok(kn_sctp_kat_init.pkt, kn_sctp_kat_init.len) == 1 &&
              t_parse(kn_sctp_kat_init.pkt, kn_sctp_kat_init.len, &p) == 0 &&
              p.sport == 5000 && p.dport == 5000 && p.vtag == 0 &&
              p.chunk_type == 0x01 && p.init_tag == 0xDEADBEEFu &&
              p.a_rwnd == 1024u * 1024u && p.os == 65535 &&
              p.mis == 65535 && p.initial_tsn == 0x12345678u,
              "the golden INIT packet parses + its CRC verifies through "
              "the INDEPENDENT parser (the parser the live-capture leg "
              "below uses on real usrsctp output)");
    }
}

/* ------------------------------------------------------- stub contract */

static void test_stub_mode(void)
{
    char err[128];
    kn_sctp *s;
    kn_sctp_stats st;

    printf("-- kn_sctp stub contract --\n");
    err[0] = '\0';
    s = kn_sctp_open(NULL, NULL, err, sizeof err);
    CHECK(s == NULL && err[0] != '\0',
          "stub: open refuses loudly with a reason");
    CHECK(kn_sctp_inject(NULL, (const uint8_t *)"x", 1) == KN_SCTP_ERR_STATE &&
          kn_sctp_poll(NULL, NULL, NULL, NULL, NULL) == KN_SCTP_ERR_STATE &&
          kn_sctp_established(NULL) == 0 &&
          kn_sctp_send(NULL, 0, 0, 1, NULL, 0) == KN_SCTP_ERR_ARGS &&
          kn_sctp_up_ms(NULL) == 0 && kn_sctp_key(NULL) == NULL,
          "stub: every accessor is NULL-safe");
    st.pkts_in = 9;
    kn_sctp_stats_get(NULL, &st);
    CHECK(st.pkts_in == 0, "stub: stats_get zeroes the output");
    CHECK(kn_sctp_open_channels(NULL, kn_sctp_nvst_channels, 8) == 0,
          "stub: open_channels is a counted no-op");
    kn_sctp_close(NULL);
    CHECK(1, "stub: close(NULL) is safe");
}

#ifdef KN_SCTP_BACKEND_USRSCTP
/* ================= the real battery (vendored usrsctp) =============== */

/* the capture write_cb: record what the stack emits */
typedef struct {
    uint8_t pkts[16][KN_SCTP_MAX_PKT];
    size_t  lens[16];
    unsigned n;
} cap_t;

static int cap_write(void *ctx, const uint8_t *data, size_t len)
{
    cap_t *c = (cap_t *)ctx;
    if (c->n < 16 && len <= KN_SCTP_MAX_PKT) {
        memcpy(c->pkts[c->n], data, len);
        c->lens[c->n] = len;
        c->n++;
    }
    return 0;
}

/* the relay pair: client <-> server over memory. DIRECTION IS ROUTED
 * BY THE PACKET'S DESTINATION PORT, never by the AF_CONN key:
 * usrsctp_conninput stamps BOTH endpoints of a received packet with
 * the injected key (user_socket.c:3285-3296), so inside ONE stack the
 * accepted association's remote addr is also the server key and every
 * conn_output lands in one tx queue. The ports keep the two directions
 * distinguishable (client 5001, server 5000) — production never needs
 * this (one instance per process, the DTLS layer is the wire). */
#define T_CLI_PORT 5001u
#define T_SRV_PORT 5000u

typedef struct {
    kn_sctp *cli, *srv;
    int drop_c2s, drop_s2c;             /* one-shot loss legs */
    unsigned relayed_c2s, relayed_s2c, dropped, unrouted;
} relay_t;

static int relay_write(void *ctx, const uint8_t *data, size_t len)
{
    relay_t *r = (relay_t *)ctx;
    uint16_t dport;
    kn_sctp *target;
    int to_srv;
    if (len < 12) { r->unrouted++; return -1; }
    dport = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
    if (dport == T_SRV_PORT)      { target = r->srv; to_srv = 1; }
    else if (dport == T_CLI_PORT) { target = r->cli; to_srv = 0; }
    else                          { r->unrouted++; return -1; }
    if (to_srv ? r->drop_c2s : r->drop_s2c) {
        if (to_srv) r->drop_c2s = 0; else r->drop_s2c = 0;
        r->dropped++;
        return 0;
    }
    if (to_srv) r->relayed_c2s++; else r->relayed_s2c++;
    return kn_sctp_inject(target, data, len);
}

/* server side: answer DCEP OPENs with ACKs exactly like str0m does
 * (sctp/mod.rs:1014-1060), count what arrived */
typedef struct {
    unsigned opens, acks, data_msgs;
    char     labels[8][KN_SCTP_MAX_LABEL];
    uint16_t sids[8];
    uint8_t  ctypes[8];
    uint32_t relparams[8];
    unsigned label_n;
    uint16_t last_data_sid;
    uint32_t last_data_ppi;
    uint8_t  last_data[64];
    size_t   last_data_len;
    kn_sctp *self;          /* for sending the ACK back */
} srv_cb_t;

static void srv_msg(void *ctx, uint16_t sid, uint16_t ssn, uint32_t ppi,
                    int unordered, const uint8_t *data, size_t len)
{
    srv_cb_t *c = (srv_cb_t *)ctx;
    (void)ssn; (void)unordered;
    if (ppi == KN_SCTP_PPI_DCEP && len >= 1 &&
        data[0] == KN_SCTP_DCEP_MSG_OPEN) {
        int u = 0;
        uint8_t ct = 0;
        uint16_t prio = 0;
        uint32_t rp = 0;
        char label[KN_SCTP_MAX_LABEL], proto[KN_SCTP_MAX_PROTO];
        if (kn_sctp_dcep_parse(data, len, &u, &ct, &prio, &rp,
                               label, sizeof label,
                               proto, sizeof proto) == KN_SCTP_DCEP_MSG_OPEN) {
            uint8_t ack[4];
            size_t alen = 0;
            if (c->opens < 8) {
                snprintf(c->labels[c->opens], KN_SCTP_MAX_LABEL, "%s", label);
                c->sids[c->opens] = sid;
                c->ctypes[c->opens] = (uint8_t)(ct | (u ? 0x80 : 0));
                c->relparams[c->opens] = rp;
                c->opens++;
            }
            if (kn_sctp_dcep_ack_build(ack, sizeof ack, &alen) == 0)
                (void)kn_sctp_send(c->self, sid, KN_SCTP_PPI_DCEP, 1,
                                   ack, alen);
        }
        return;
    }
    if (ppi == KN_SCTP_PPI_DCEP && len >= 1 &&
        data[0] == KN_SCTP_DCEP_MSG_ACK) {
        c->acks++;
        return;
    }
    c->data_msgs++;
    c->last_data_sid = sid;
    c->last_data_ppi = ppi;
    c->last_data_len = len < sizeof c->last_data ? len : sizeof c->last_data;
    memcpy(c->last_data, data, c->last_data_len);
}

static int g_cli_acks;
static uint16_t g_cli_data_sid;
static uint32_t g_cli_data_ppi;
static uint8_t g_cli_data[64];
static size_t g_cli_data_len;

static void cli_msg(void *ctx, uint16_t sid, uint16_t ssn, uint32_t ppi,
                    int unordered, const uint8_t *data, size_t len)
{
    (void)ctx; (void)ssn; (void)unordered;
    if (ppi == KN_SCTP_PPI_DCEP && len == 1 &&
        data[0] == KN_SCTP_DCEP_MSG_ACK) {
        g_cli_acks++;
        return;
    }
    g_cli_data_sid = sid;
    g_cli_data_ppi = ppi;
    g_cli_data_len = len < sizeof g_cli_data ? len : sizeof g_cli_data;
    memcpy(g_cli_data, data, g_cli_data_len);
}

static int g_event_up_cli, g_event_up_srv, g_event_down;

static void ev_cb(void *ctx, int established)
{
    int *flag = (int *)ctx;
    if (established) *flag = 1;
    else g_event_down = 1;
}

static void test_real_mode(void)
{
    char err[256];
    cap_t cap;
    kn_sctp *c1;

    printf("-- kn_sctp real battery (vendored usrsctp, AF_CONN) --\n");

    /* ---- 1. the INIT on the wire: independent parse + CRC + the
     *         reference's exact association parameters ---- */
    memset(&cap, 0, sizeof cap);
    c1 = kn_sctp_open(cap_write, &cap, err, sizeof err);
    CHECK(c1 != NULL, "open() creates the client association");
    if (c1) {
        (void)kn_sctp_poll(c1, NULL, NULL, NULL, NULL);  /* flush INIT */
        CHECK(cap.n >= 1, "the INIT left through the write callback "
                          "(fire-and-forget on open, like start_sctp)");
        if (cap.n >= 1) {
            t_sctp_pkt p;
        memset(&p, 0, sizeof p);   /* target GCC 16.2 -Wmaybe-
                                     uninitialized: t_parse fills the INIT
                                     fields conditionally (the round-XXVII
                                     warning-class discipline) */
            int prc = t_parse(cap.pkts[0], cap.lens[0], &p);
            CHECK(prc == 0 && t_pkt_crc_ok(cap.pkts[0], cap.lens[0]),
                  "the live INIT packet passes the INDEPENDENT parser "
                  "AND its CRC-32c verifies");
            CHECK(prc == 0 && p.sport == KN_SCTP_PORT &&
                  p.dport == KN_SCTP_PORT && p.vtag == 0 &&
                  p.chunk_type == 0x01,
                  "INIT: ports 5000->5000 (RFC 8831), vtag 0, chunk 0x01");
            CHECK(prc == 0 && p.os == 65535 && p.mis == 65535,
                  "INIT advertises OS/MIS 65535/65535 (the sctp-proto "
                  "TransportConfig defaults the reference sends)");
            CHECK(prc == 0 && p.a_rwnd >= 65536u && p.init_tag != 0 &&
                  p.initial_tsn != 0,
                  "INIT: a_rwnd >= 64 KiB, nonzero init tag + TSN");
        }
        CHECK(kn_sctp_established(c1) == 0,
              "not established with nobody on the other end (honest)");
        kn_sctp_close(c1);
    }

    /* ---- 2. the full loopback association + the 8-channel DCEP
     *         profile (client AND server side are kn_sctp) ---- */
    {
        relay_t r;
        srv_cb_t scb;
        kn_sctp_stats cst, sst;
        uint64_t t_end;
        int established = 0, i;

        memset(&r, 0, sizeof r);
        memset(&scb, 0, sizeof scb);
        g_cli_acks = 0;
        g_event_up_cli = g_event_up_srv = g_event_down = 0;
        g_cli_data_len = 0;
        scb.last_data_len = 0;

        err[0] = '\0';
        r.srv = kn_sctp_open_server(relay_write, &r, err, sizeof err);
        CHECK(r.srv != NULL, "the loopback server side opens (listen)");
        if (!r.srv) return;
        scb.self = r.srv;
        err[0] = '\0';
        r.cli = kn_sctp_open_peer(relay_write, &r, kn_sctp_key(r.srv),
                                  T_CLI_PORT, T_SRV_PORT, err, sizeof err);
        CHECK(r.cli != NULL, "the client side opens toward the server key");
        if (!r.cli) { kn_sctp_close(r.srv); return; }

        /* drive the 4-way handshake */
        t_end = kn_sock_now_ms() + 10000u;
        while (kn_sock_now_ms() < t_end &&
               !(g_event_up_cli && g_event_up_srv)) {
            (void)kn_sctp_poll(r.cli, cli_msg, NULL, ev_cb,
                               &g_event_up_cli);
            (void)kn_sctp_poll(r.srv, srv_msg, &scb, ev_cb,
                               &g_event_up_srv);
            if (g_event_down) break;
        }
        established = (g_event_up_cli && g_event_up_srv);
        CHECK(established, "the 4-way association handshake completes "
                           "(INIT -> INIT-ACK -> COOKIE-ECHO -> "
                           "COOKIE-ACK) over the memory relay");
        if (!established) {
            kn_sctp_close(r.cli);
            kn_sctp_close(r.srv);
            return;
        }
        CHECK(kn_sctp_established(r.cli) && kn_sctp_established(r.srv) &&
              kn_sctp_up_ms(r.cli) < 10000,
              "both sides report established with a sane up_ms");

        /* the reference's channel open burst */
        CHECK(kn_sctp_open_channels(r.cli, kn_sctp_nvst_channels,
                                    KN_SCTP_NVST_N_CHANNELS) == 8,
              "all 8 DATA_CHANNEL_OPENs send on the association");
        t_end = kn_sock_now_ms() + 5000u;
        while (kn_sock_now_ms() < t_end && (scb.opens < 8 || g_cli_acks < 8)) {
            (void)kn_sctp_poll(r.cli, cli_msg, NULL, NULL, NULL);
            (void)kn_sctp_poll(r.srv, srv_msg, &scb, NULL, NULL);
        }
        CHECK(scb.opens == 8, "the server side received all 8 OPENs");
        CHECK(g_cli_acks == 8, "the client got 8 DATA_CHANNEL_ACKs back "
                               "(the str0m AwaitDcepAck -> open path)");
        {   /* the received OPENs carry the EXACT profile: sid order,
             * labels, channel-type bytes (incl. the unordered bit) and
             * reliability parameters */
            int ok = 1;
            for (i = 0; i < 8 && ok; ++i) {
                const kn_sctp_channel_kat *k = &kn_sctp_kat_channels[i];
                if (scb.sids[i] != k->sid ||
                    strcmp(scb.labels[i], k->label) != 0 ||
                    scb.ctypes[i] != (uint8_t)(k->channel_type |
                                     (k->unordered ? 0x80 : 0)) ||
                    scb.relparams[i] != k->rel_param)
                    ok = 0;
            }
            CHECK(ok, "every received OPEN matches the reference profile "
                      "byte-for-byte (sid 0/2/4/6/8/10/12/14, labels, "
                      "0x00/0x01/0x82/0x81 types, params 0/2/300)");
        }

        /* ---- 3. user data both ways (stage-3 hook smoke) ---- */
        {
            const uint8_t msg[] = "kn-r3c-hello";
            CHECK(kn_sctp_send(r.cli, 0, KN_SCTP_PPI_BINARY, 1, msg,
                               sizeof msg - 1) == KN_SCTP_OK,
                  "client sends a BINARY message on the control stream");
            t_end = kn_sock_now_ms() + 3000u;
            while (kn_sock_now_ms() < t_end && scb.data_msgs == 0) {
                (void)kn_sctp_poll(r.cli, cli_msg, NULL, NULL, NULL);
                (void)kn_sctp_poll(r.srv, srv_msg, &scb, NULL, NULL);
            }
            CHECK(scb.data_msgs == 1 && scb.last_data_sid == 0 &&
                  scb.last_data_ppi == KN_SCTP_PPI_BINARY &&
                  scb.last_data_len == sizeof msg - 1 &&
                  memcmp(scb.last_data, msg, sizeof msg - 1) == 0,
                  "the server receives it intact (sid/ppi/payload)");
            (void)kn_sctp_send(r.srv, 14, KN_SCTP_PPI_BINARY, 1,
                               msg, sizeof msg - 1);
            t_end = kn_sock_now_ms() + 3000u;
            while (kn_sock_now_ms() < t_end && g_cli_data_len == 0) {
                (void)kn_sctp_poll(r.cli, cli_msg, NULL, NULL, NULL);
                (void)kn_sctp_poll(r.srv, srv_msg, &scb, NULL, NULL);
            }
            CHECK(g_cli_data_len == sizeof msg - 1 &&
                  g_cli_data_sid == 14 &&
                  memcmp(g_cli_data, msg, sizeof msg - 1) == 0,
                  "and the return path works on the rtcp stream (sid 14)");
            /* round XXXVI: the PR-SCTP send path (SCTP_SENDV_SPA) —
             * an unordered TTL-300 message on sid 6 (the partial-
             * reliable control channel's sender-side policy) */
            {
                const uint8_t prm[] = "pr-msg";
                unsigned before = scb.data_msgs;
                CHECK(kn_sctp_send_pr(r.cli, 6, KN_SCTP_PPI_BINARY, 0,
                                      KN_SCTP_PR_TTL, 300, prm,
                                      sizeof prm - 1) == KN_SCTP_OK,
                      "send_pr (SPA: sndinfo+prinfo) accepts the TTL-300 "
                      "unordered send");
                t_end = kn_sock_now_ms() + 3000u;
                while (kn_sock_now_ms() < t_end &&
                       scb.data_msgs == before) {
                    (void)kn_sctp_poll(r.cli, cli_msg, NULL, NULL, NULL);
                    (void)kn_sctp_poll(r.srv, srv_msg, &scb, NULL, NULL);
                }
                CHECK(scb.data_msgs > before && scb.last_data_sid == 6 &&
                      scb.last_data_len == sizeof prm - 1 &&
                      memcmp(scb.last_data, prm, sizeof prm - 1) == 0,
                      "the PR message arrives on sid 6 (the feedback "
                      "senders' path: RR/ACK/QoS all ride it)");
            }
        }

        /* ---- 4. retransmission: drop the client's next packet (a
         *         DATA carrying nothing critical) and prove the SACK
         *         machinery recovers a following message ---- */
        {
            const uint8_t msg2[] = "after-loss";
            r.drop_c2s = 1;
            (void)kn_sctp_send(r.cli, 2, KN_SCTP_PPI_BINARY, 1,
                               msg2, sizeof msg2 - 1);
            /* poll until the drop actually fires (the DATA chunk may
             * leave on a later flush — usrsctp bundles/delays output
             * under traffic; the one-shot flag persists until then) */
            t_end = kn_sock_now_ms() + 2000u;
            while (kn_sock_now_ms() < t_end && r.dropped == 0) {
                (void)kn_sctp_poll(r.cli, cli_msg, NULL, NULL, NULL);
                (void)kn_sctp_poll(r.srv, srv_msg, &scb, NULL, NULL);
            }
            CHECK(r.dropped == 1, "the one-shot drop fired");
            t_end = kn_sock_now_ms() + 8000u;
            while (kn_sock_now_ms() < t_end &&
                   !(scb.data_msgs >= 2 &&
                     scb.last_data_len == sizeof msg2 - 1 &&
                     memcmp(scb.last_data, msg2, sizeof msg2 - 1) == 0)) {
                (void)kn_sctp_poll(r.cli, cli_msg, NULL, NULL, NULL);
                (void)kn_sctp_poll(r.srv, srv_msg, &scb, NULL, NULL);
            }
            CHECK(scb.data_msgs >= 2 &&
                  memcmp(scb.last_data, msg2, sizeof msg2 - 1) == 0,
                  "the lost packet's message still arrives (SACK + "
                  "fast-retransmit recovery inside usrsctp)");
        }

        /* ---- 5. stats + garbage tolerance + teardown ---- */
        kn_sctp_stats_get(r.cli, &cst);
        kn_sctp_stats_get(r.srv, &sst);
        /* NOTE: in the shared-key loopback topology EVERY conn_output
         * packet lands in the server instance's tx queue (the key
         * routes there), so the client's pkts_out is legitimately 0
         * and the server's pkts_out carries both directions. In
         * production (one instance per process) each side counts its
         * own output — the wire-level packet flow is asserted by the
         * relay counters and the INIT-parse leg above. */
        CHECK(cst.pkts_in > 0 && cst.msgs_in >= 1 &&
              cst.sends >= 10 && cst.send_errors == 0 &&
              cst.notif_assoc >= 1 && cst.lost == 0 &&
              r.relayed_c2s > 0 && r.relayed_s2c > 0 &&
              r.unrouted == 0,
              "client stats are coherent (packets in, the 8 DCEP ACKs, "
              "the sends, COMM_UP, no losses) and the relay moved "
              "traffic both ways with nothing unrouted");
        CHECK(sst.msgs_in >= 10 && sst.send_errors == 0 &&
              sst.pkts_out >= r.relayed_c2s + r.relayed_s2c,
              "server stats saw the 8 OPENs + the data messages and its "
              "queue carried the whole relayed stream");
        CHECK(kn_sctp_inject(r.cli, (const uint8_t *)"\xff\xff\xff\xff\xff",
                             5) == KN_SCTP_OK,
              "garbage into inject is tolerated (the stack discards it)");
        (void)kn_sctp_poll(r.cli, cli_msg, NULL, NULL, NULL);
        CHECK(kn_sctp_established(r.cli) == 1,
              "still established after garbage");
        kn_sctp_close(r.cli);
        kn_sctp_close(r.srv);
        CHECK(1, "both sides close cleanly (ASan's verdict is the gate)");
    }
}
#endif /* KN_SCTP_BACKEND_USRSCTP */

int main(void)
{
    int avail = kn_sctp_available();
    CHECK(avail == 0 || avail == 1, "available() is boolean");
    test_dcep_goldens();
#ifdef KN_SCTP_BACKEND_USRSCTP
    if (avail) test_real_mode();
    else       test_stub_mode();
#else
    test_stub_mode();
#endif
    return kn_test_summary("test_knsctp");
}
