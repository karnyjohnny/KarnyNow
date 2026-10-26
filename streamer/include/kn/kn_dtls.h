/* kn_dtls.h — the DTLS 1.2 ACTIVE client for the bundle socket (R3-B,
 * docs/DTLS.md phase 2; provenance in docs/RTSP.md §18).
 *
 * The reference (nvst.rs:6300) starts str0m with start_dtls(true): the
 * CLIENT is the DTLS-active side and sends the first ClientHello — gated
 * on real inbound STUN (:8196-8198), never synthesized. This module is
 * that handshake, portable mbedTLS (the kn_cert build discipline: the
 * real body compiles under KN_TLS_BACKEND_MBEDTLS on BOTH platforms, so
 * the sandbox ctest exercises real DTLS over real loopback UDP; every
 * other configuration gets the honest stub — available() == 0 and a
 * loud refusal, no plaintext, no silence).
 *
 * Model: INJECT-QUEUE BIO. The pump thread owns the socket (single-
 * owner discipline, round XXVIII): it demuxes inbound datagrams
 * (STUN | DTLS first-byte 20..63, looks_like_dtls nvst.rs:6345 | PING)
 * and feeds the DTLS-looking ones to kn_dtls_inject(); kn_dtls_poll()
 * steps the handshake, pulling from that bounded queue through the BIO
 * recv callback and pushing records straight out through
 * kn_sock_udp_sendto. The module NEVER calls recv — two readers on one
 * socket is exactly the bug class this design forbids. Retransmission
 * timers run on kn_sock_now_ms (set/get_timer contract: -1/0/1/2).
 *
 * SECURITY (§7 + the pin IS the policy):
 *   - DTLS 1.2 only (min == max; the str0m default, config.rs:775, and
 *     the same ceiling the Win7 TLS stream proved in T12).
 *   - The peer is authenticated ONLY by its SHA-256 certificate
 *     fingerprint, PINNED from the DESCRIBE SDP (parse_nvst_fingerprint
 *     :5640 semantics: an optional "sha-256 " prefix is tolerated).
 *     A verify callback clears the chain flags EXCLUSIVELY on an exact
 *     fingerprint match, and kn_dtls_poll REFUSES the ready verdict
 *     unless verify_result == 0 AND the pin matched (latching
 *     KN_DTLS_ERR_PIN + close_notify otherwise). The authmode is
 *     VERIFY_OPTIONAL because mbedTLS fails VERIFY_REQUIRED without a
 *     CA chain BEFORE any callback (ssl_tls.c:8071) and a WebRTC peer
 *     is self-signed by design — there is no CA path and no
 *     verification-disabled path; the pin IS the policy, strictly
 *     stronger than a chain check (a forgery needs the rig's private
 *     key). The rig pins us the same way, by the ANNOUNCE fingerprint.
 *   - Our identity is the kn_cert whose fingerprint flew in the
 *     ANNOUNCE (kn_cert_x509/kn_cert_pk handles — R3-A kept them for
 *     exactly this). The private key never leaves the mbedTLS contexts
 *     and nothing key-shaped is ever logged (shapes/counts only).
 *   - No DTLS-SRTP (use_srtp) extension: video SRTP material is
 *     ANNOUNCE-derived (nvst.rs:1820-1840) — deviation D-t3.
 *
 * Post-handshake: app-data records (the rig's SCTP until R3-C) are
 * drained and COUNTED, never silently dropped — deviation D-t4.
 */
#ifndef KN_DTLS_H
#define KN_DTLS_H

#include <stddef.h>
#include <stdint.h>

#include "kn/kn_cert.h"
#include "kn/kn_sock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- returns */
enum {
    KN_DTLS_OK            =  0,  /* in progress (or op accepted) */
    KN_DTLS_READY         =  1,  /* handshake complete + peer pinned */
    KN_DTLS_ERR_ARGS      = -1,  /* NULL/bad argument */
    KN_DTLS_ERR_STATE     = -2,  /* call in the wrong state */
    KN_DTLS_ERR_MEM       = -3,  /* allocation failure */
    KN_DTLS_ERR_HANDSHAKE = -4,  /* fatal handshake error (last_rc tells) */
    KN_DTLS_ERR_PIN       = -5,  /* peer fingerprint != the pinned one */
    KN_DTLS_ERR_FULL      = -6,  /* inject queue full (datagram dropped) */
    KN_DTLS_ERR_IO        = -7   /* socket send failed (last_rc tells) */
};

/* One queued datagram slot. Sized to the pump's rx buffer (2048 in
 * sess_pump) so NO record the pump can receive is rejected for size;
 * typical handshake flights fragment around the 1200 MTU, and the
 * protocol's own ceiling is 16 KiB per record — anything above 2048
 * would be dropped-and-counted honestly (retransmission recovers). */
#define KN_DTLS_MAX_RECORD   2048u
#define KN_DTLS_QUEUE_SLOTS    32u

typedef struct kn_dtls kn_dtls;   /* opaque; heap-owned like kn_cert */

/* Evidence counters (shapes/counts only — §7). */
typedef struct {
    unsigned records_in;      /* datagrams accepted by inject() */
    unsigned records_dropped; /* inject() rejections (full/oversize) */
    unsigned records_out;     /* datagrams sent through the BIO */
    unsigned send_errors;     /* BIO send failures */
    unsigned polls;           /* kn_dtls_poll calls */
    unsigned app_records;     /* post-ready app-data reads (R3-C hook) */
    unsigned app_bytes;       /* app-data bytes drained (discarded) */
    unsigned peer_closed;     /* a close_notify arrived (rig teardown) */
    int      pin_matched;     /* verify callback saw the pinned fp */
    int      verify_result;   /* mbedtls_ssl_get_verify_result (0 = clean) */
    int      last_rc;         /* last fatal mbedTLS return (negative) */
} kn_dtls_stats;

/* 1 when this build carries the real mbedTLS body, 0 for the stub. */
int kn_dtls_available(void);

/* Portable predicate for the pump's demux (looks_like_dtls, nvst.rs:6345
 * — first byte 20..=63). Always the real logic, in EVERY build (it is
 * pure; the stub section defines it identically). */
int kn_dtls_peek(const uint8_t *data, size_t len);

/* Create the ACTIVE client. sock: an OPEN kn_sock_udp (caller-owned for
 * life; the module only sends through it). dest: the bundle peer (the
 * pump's ice_dest — STUN-proven before arming, per the :8196 rule the
 * CALLER enforces). identity: a live kn_cert (must OUTLIVE this object —
 * mbedTLS borrows the contexts). remote_fp: the pinned fingerprint in
 * colon-hex, with or without a "sha-256 " prefix (case-insensitive hex;
 * parse_nvst_fingerprint :5640). err/errsz get a one-line reason on
 * failure (with the mbedTLS code). Returns NULL on any failure. */
kn_dtls *kn_dtls_client(kn_sock_udp *sock, uint32_t dest_ip,
                        uint16_t dest_port, kn_cert *identity,
                        const char *remote_fp, char *err, size_t errsz);

/* Feed ONE inbound datagram (already demuxed by the pump). Copies into
 * the bounded queue; returns KN_DTLS_OK, KN_DTLS_ERR_FULL (dropped,
 * counted) or KN_DTLS_ERR_ARGS. Never blocks. */
int kn_dtls_inject(kn_dtls *d, const uint8_t *data, size_t len);

/* Step the handshake (or, once ready, drain app data). Non-blocking:
 * one mbedtls_ssl_handshake step per call; WANT_READ/WANT_WRITE/TIMEOUT
 * all map to KN_DTLS_OK (in progress — retransmissions are timer-driven
 * inside later calls). Returns KN_DTLS_READY exactly once when the
 * handshake completes with the pin matched, then KN_DTLS_READY stays
 * (idempotent); KN_DTLS_ERR_PIN / _HANDSHAKE / _IO are FINAL (the
 * object refuses further polling; free it). */
int kn_dtls_poll(kn_dtls *d);

int kn_dtls_ready(const kn_dtls *d);

/* Negotiated facts for the evidence line (bounded copies; "" when not
 * ready). version: e.g. "DTLSv1.2". ciphersuite: the mbedTLS name
 * (truncated to outsz-1 — the log line caps it further). */
const char *kn_dtls_version(const kn_dtls *d);
int kn_dtls_ciphersuite(const kn_dtls *d, char *out, size_t outsz);

/* Milliseconds between create and ready (0 before ready). */
uint64_t kn_dtls_ready_ms(const kn_dtls *d);

void kn_dtls_stats_get(const kn_dtls *d, kn_dtls_stats *out);

void kn_dtls_free(kn_dtls *d);   /* free(NULL)-safe (NOT double-free —
                                    the kn_cert/kn_net convention); does
                                    NOT close the socket or free the
                                    kn_cert (both caller-owned) */

#ifdef __cplusplus
}
#endif

#endif /* KN_DTLS_H */
