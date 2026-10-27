/* kn_sctp.h — SCTP-over-DTLS client for the bundle (R3-C, docs/DTLS.md
 * phase 3; provenance in docs/SCTP.md + docs/RTSP.md §20).
 *
 * WHY: run 13 completed the DTLS handshake on a live production rig;
 * run 14 proved the rig sends NO app data after it (app_records:0) and
 * answers PLAY with an explicit 400 — the reference client is the SCTP
 * initiator (start_sctp(true), nvst.rs:8221) and fires the INIT the
 * instant DTLS connects (:8214-8223), then opens 8 DCEP channels
 * (nvst_input.rs NVST_CHANNEL_PROFILE) BEFORE finish()/PLAY. We sent
 * nothing. This module is that missing layer.
 *
 * WHAT IT IS: a thin, honest wrapper over VENDORED usrsctp (pinned
 * commit, third_party model like mbedTLS) in AF_CONN mode — usrsctp
 * never touches a real socket; every SCTP packet travels as a DTLS
 * application record:
 *   tx: usrsctp conn_output -> bounded mutex-protected queue -> drained
 *       by kn_sctp_poll (the pump thread) -> caller's write_cb
 *       (= kn_dtls_write_app). The queue exists because usrsctp's
 *       internal timer thread may emit packets at any time, and the
 *       mbedTLS ssl context must stay single-threaded (the pump owns
 *       it) — the queue is the thread boundary.
 *   rx: the pump feeds DTLS app records to kn_sctp_inject ->
 *       usrsctp_conninput (pump-thread context) -> messages surface
 *       through kn_sctp_poll's callbacks (notifications + stream data).
 *
 * Wire parameters MIRROR the reference's sctp-proto defaults exactly
 * (docs/SCTP.md §2): ports 5000<->5000 (RFC 8831), INIT OS/MIS =
 * 65535/65535, a_rwnd = 1 MiB, RTO 3000/1000/60000 ms, unlimited INIT
 * retransmits, SCTP_MAXSEG 1200 (INITIAL_MTU 1228 minus DTLS overhead).
 *
 * SECURITY/§7: no key material flows here (DTLS already encrypted the
 * channel); logs carry counters/labels only.
 *
 * Build discipline (the kn_cert/kn_dtls pattern): the real body
 * compiles with KN_SCTP_BACKEND_USRSCTP (CMake sets it when
 * third_party/usrsctp exists, BOTH platforms); every other build gets
 * the honest stub — available() == 0, open() refuses loudly, and the
 * knvstd evidence line says armed:0. The DCEP builders/parsers below
 * are PURE and identical in every build (they are wire-format code,
 * golden-tested against tools/sctp_reference.py).
 */
#ifndef KN_SCTP_H
#define KN_SCTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- returns */
enum {
    KN_SCTP_OK          =  0,
    KN_SCTP_READY       =  1,   /* association established (poll event) */
    KN_SCTP_ERR_ARGS    = -1,
    KN_SCTP_ERR_STATE   = -2,   /* not established / closed */
    KN_SCTP_ERR_MEM     = -3,
    KN_SCTP_ERR_SOCKET  = -4,   /* usrsctp socket/connect failure */
    KN_SCTP_ERR_SEND    = -5,   /* sendv failed (last_errno) */
    KN_SCTP_ERR_LOST    = -6,   /* association lost (last_errno) */
    KN_SCTP_ERR_BACKEND = -7    /* built without usrsctp (the stub) */
};

/* The WebRTC SCTP port — BOTH sides use it as local and remote
 * (RFC 8831 §"port 5000"; sctp-proto DEFAULT_SCTP_PORT). */
#define KN_SCTP_PORT       5000u
/* Payload Protocol Identifiers (RFC 8831 §6.6 / the WebRTC registry) */
#define KN_SCTP_PPI_DCEP      50u
#define KN_SCTP_PPI_STRING    51u
#define KN_SCTP_PPI_BINARY    53u
#define KN_SCTP_PPI_STR_EMPTY 56u
#define KN_SCTP_PPI_BIN_EMPTY 57u

/* DCEP (RFC 8832) channel types — byte 1 of the OPEN message carries
 * the type, OR'd with 0x80 when unordered (str0m dcep.rs marshal_to). */
#define KN_SCTP_DCEP_RELIABLE   0x00u
#define KN_SCTP_DCEP_REXMIT     0x01u
#define KN_SCTP_DCEP_TIMED      0x02u
#define KN_SCTP_DCEP_UNORDERED  0x80u
#define KN_SCTP_DCEP_MSG_ACK    0x02u
#define KN_SCTP_DCEP_MSG_OPEN   0x03u

/* One queued output packet (SCTP over DTLS: never above the DTLS
 * record ceiling our kn_dtls uses). */
#define KN_SCTP_MAX_PKT      2048u
#define KN_SCTP_TXQ_SLOTS      64u
#define KN_SCTP_MAX_LABEL     128u
#define KN_SCTP_MAX_PROTO      64u

typedef struct kn_sctp kn_sctp;   /* opaque; heap-owned like kn_dtls */

/* Evidence counters (shapes/counts only — §7). The tx-side counters
 * are bumped by the conn_output callback, which usrsctp may call from
 * its OWN timer thread — so they can race with a stats read. They are
 * diagnostic only (never gating); kn_sctp_stats_get takes the tx lock
 * for a consistent snapshot, and a torn read here would be harmless. */
typedef struct {
    unsigned pkts_out, pkts_in;        /* packets through the queues */
    unsigned bytes_out, bytes_in;
    unsigned txq_dropped;              /* bounded tx queue overflows */
    unsigned notif_assoc;              /* ASSOC_CHANGE notifications */
    unsigned msgs_in;                  /* stream-data messages up-called */
    unsigned sends, send_errors;
    unsigned opened, acked;            /* DCEP OPENs sent / ACKs received */
    int      established;              /* 1 after COMM_UP */
    int      lost;                     /* 1 after a lost/shutdown assoc */
    int      last_errno;               /* usrsctp errno at the failure */
    /* Round XXXVIII: the ASSOC_CHANGE cause. Run 18 logged a bare
     * `sctp_down` with sctp_fail:0, and the only way to read that was by
     * knowing kn_sctp's internal rule (SHUTDOWN_COMP -> lost = 0). The
     * distinction is the whole diagnosis: SHUTDOWN_COMP means the PEER
     * closed the association gracefully (it had decided the session was
     * over — consistent with a rate controller that walked the bitrate to
     * its floor and stopped), while COMM_LOST means the path died. Both
     * fields are the raw usrsctp values, so nothing is inferred. */
    int      have_assoc_state;         /* 1 once any ASSOC_CHANGE landed */
    unsigned assoc_state;              /* sac_state of the LAST change */
    unsigned assoc_error;              /* sac_error of the LAST change */
} kn_sctp_stats;

/* The ASSOC_CHANGE sac_state values this port names in evidence lines
 * (RFC 4960 §6.1 / usrsctp sctp_assoc_change). Named here so a log
 * reader never has to guess which integer meant "the peer shut down". */
#define KN_SCTP_ASSOC_COMM_UP        1u
#define KN_SCTP_ASSOC_COMM_LOST      2u
#define KN_SCTP_ASSOC_RESTART        3u
#define KN_SCTP_ASSOC_SHUTDOWN_COMP  4u
#define KN_SCTP_ASSOC_CANT_STR_ASSOC 5u

/* The human name of a sac_state value ("comm_up", "comm_lost",
 * "restart", "shutdown_comp", "cant_str_assoc", "unknown"). Never NULL. */
const char *kn_sctp_assoc_state_name(unsigned sac_state);

/* One NVST channel definition — the reference's NvstChannelDefinition
 * (nvst_input.rs:72-127): a fixed stream id, the label, the ordered
 * flag, the DCEP channel type and its reliability parameter. */
typedef struct {
    uint16_t    sid;
    int         unordered;
    uint8_t     channel_type;
    uint32_t    rel_param;
    const char *label;
} kn_sctp_channel_def;

/* The GFN channel profile: 8 channels (nvst_input.rs
 * NVST_CHANNEL_PROFILE[..8]). The reference opens ALL of them in the
 * bundle case (`create(rtc, !mjolnir)`: 6 always + cursor + rtcp when
 * the bundle carries video, which is the nativeRtcOnBundlePort=1 shape
 * knvstd enforces). Priority 0 and an empty protocol string, exactly
 * as str0m's From<&ChannelConfig> for DcepOpen emits
 * (sctp/mod.rs:1311-1322). */
extern const kn_sctp_channel_def kn_sctp_nvst_channels[8];
#define KN_SCTP_NVST_N_CHANNELS 8

/* Open every channel of a profile: builds each DATA_CHANNEL_OPEN and
 * sends it ordered+reliable with PPI 50 on its stream (str0m
 * write_with_ppi(.., Dcep), sctp/mod.rs:855-863). Returns the number
 * of channels whose OPEN was accepted by the stack (== n on success;
 * a partial count means the association died mid-way). */
int kn_sctp_open_channels(kn_sctp *s, const kn_sctp_channel_def *defs,
                          int n);

/* 1 when this build carries the real usrsctp body, 0 for the stub. */
int kn_sctp_available(void);

/* ---------- DCEP wire helpers (PURE — identical in every build;
 * byte-exact ports of str0m's dcep.rs marshal/parse, golden-tested
 * against the independent tools/sctp_reference.py) ---------- */

/* Build a DATA_CHANNEL_OPEN (RFC 8832 §4.2): 12-byte fixed head +
 * label + protocol. priority is what the reference sends: 0
 * (From<&ChannelConfig> for DcepOpen, str0m sctp/mod.rs:1311-1322).
 * Returns KN_SCTP_OK / ERR_ARGS (bad inputs or buffer too small). */
int kn_sctp_dcep_open_build(uint8_t *out, size_t outsz, size_t *outlen,
                            int unordered, uint8_t channel_type,
                            uint16_t priority, uint32_t rel_param,
                            const char *label, const char *protocol);

/* Build the one-byte DATA_CHANNEL_ACK (§4.3). */
int kn_sctp_dcep_ack_build(uint8_t *out, size_t outsz, size_t *outlen);

/* Parse a DCEP message. Returns KN_SCTP_DCEP_MSG_OPEN / _ACK / 0
 * (unknown or malformed). OPEN fills the out fields (label/protocol
 * NUL-terminated, truncated honestly when the buffers are small;
 * non-UTF8-safe: raw bytes copied). */
int kn_sctp_dcep_parse(const uint8_t *data, size_t len, int *unordered,
                       uint8_t *channel_type, uint16_t *priority,
                       uint32_t *rel_param,
                       char *label, size_t labelsz,
                       char *protocol, size_t protosz);

/* ---------- the association ---------- */

/* write_cb: invoked from kn_sctp_poll (the pump thread — never from
 * the usrsctp timer thread; see the file header) with one complete
 * SCTP packet to wrap into a DTLS app record. Returns 0 on success. */
typedef int (*kn_sctp_write_fn)(void *ctx, const uint8_t *data, size_t len);

/* Create the client association and START the 4-way handshake (the
 * INIT leaves through write_cb on the first poll). Single instance per
 * session; the caller owns the DTLS layer underneath. err/errsz get a
 * one-line reason on failure. */
kn_sctp *kn_sctp_open(kn_sctp_write_fn write_cb, void *write_ctx,
                      char *err, size_t errsz);

/* Loopback-test companions (the real body only — the stub forwards both
 * to open()'s loud refusal): open_server binds+listens on the AF_CONN
 * key; open_peer connects toward an explicit peer key with explicit
 * ports, so two instances inside ONE usrsctp stack can demux their
 * packets — usrsctp_conninput stamps BOTH endpoints with the injected
 * key (user_socket.c:3275-3296), so the test relay routes by the
 * packet's DESTINATION PORT (client 5001, server 5000), never by the
 * AF_CONN key. Production stays 5000<->5000 (RFC 8831). */
kn_sctp *kn_sctp_open_server(kn_sctp_write_fn write_cb, void *write_ctx,
                             char *err, size_t errsz);
kn_sctp *kn_sctp_open_peer(kn_sctp_write_fn write_cb, void *write_ctx,
                           void *peer_key, uint16_t local_port,
                           uint16_t remote_port, char *err, size_t errsz);
/* the instance's own AF_CONN routing key (for open_peer) */
void *kn_sctp_key(const kn_sctp *s);

/* Feed one DTLS application record (an SCTP packet) — the pump's
 * kn_dtls_read_app output. Never blocks. */
int kn_sctp_inject(kn_sctp *s, const uint8_t *data, size_t len);

/* Stream-data message callback (one complete SCTP message). */
typedef void (*kn_sctp_msg_fn)(void *ctx, uint16_t sid, uint16_t ssn,
                               uint32_t ppi, int unordered,
                               const uint8_t *data, size_t len);
/* Association-state callback: established 1 = COMM_UP, 0 = lost/down. */
typedef void (*kn_sctp_event_fn)(void *ctx, int established);

/* Drive the stack: flush the tx queue through write_cb, then pull
 * every pending notification/message (recvv, non-blocking) and up-call
 * the callbacks. Call once per pump iteration. Returns KN_SCTP_OK, or
 * KN_SCTP_ERR_LOST once (the assoc died; further polls are no-ops). */
int kn_sctp_poll(kn_sctp *s, kn_sctp_msg_fn on_msg, void *msg_ctx,
                 kn_sctp_event_fn on_event, void *ev_ctx);

int kn_sctp_established(const kn_sctp *s);

/* Send one complete message on a stream (ordered flag per RFC 4960
 * sinfo_flags; ppi per the registry). DCEP goes ordered+reliable with
 * PPI 50 (str0m write_with_ppi). Returns KN_SCTP_OK / ERR_*. */
int kn_sctp_send(kn_sctp *s, uint16_t sid, uint32_t ppi, int ordered,
                 const uint8_t *data, size_t len);

/* The PR-SCTP form (round XXXVI): per-send partial-reliability policy,
 * mirroring the channel profile's reliability on the SENDER side (the
 * reference's configure_reliability per stream, str0m sctp/mod.rs
 * :844-851): KN_SCTP_PR_TTL with pr_value = lifetime ms (the profile's
 * 300 ms channels), KN_SCTP_PR_RTX with pr_value = max retransmits
 * (2 / 0). policy 0 = reliable (kn_sctp_send equivalent). usrsctp
 * carries both via SCTP_SENDV_SPA. */
#define KN_SCTP_PR_TTL 0x0001u   /* usrsctp SCTP_PR_SCTP_TTL */
#define KN_SCTP_PR_RTX 0x0003u   /* usrsctp SCTP_PR_SCTP_RTX */
int kn_sctp_send_pr(kn_sctp *s, uint16_t sid, uint32_t ppi, int ordered,
                    uint16_t pr_policy, uint32_t pr_value,
                    const uint8_t *data, size_t len);

void kn_sctp_stats_get(const kn_sctp *s, kn_sctp_stats *out);

/* DCEP bookkeeping for the evidence lines (no-op counters in the stub). */
void kn_sctp_note_opened(kn_sctp *s);
void kn_sctp_note_acked(kn_sctp *s);

/* Milliseconds between open() and COMM_UP (0 before). */
uint64_t kn_sctp_up_ms(const kn_sctp *s);

void kn_sctp_close(kn_sctp *s);   /* free(NULL)-safe (NOT double-free —
                                     the kn_cert convention); aborts the
                                     association (linger 0) and
                                     unregisters the AF_CONN address */

#ifdef __cplusplus
}
#endif

#endif /* KN_SCTP_H */
