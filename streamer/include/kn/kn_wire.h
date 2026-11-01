#ifndef KN_WIRE_H
#define KN_WIRE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F34 (round N slice 4m): the TLS plaintext wire dump.
 *
 * WHY: the kill model (RTSP §53-55) has one surviving suspect left — the
 * handshake identity/attestation bytes. To audit them byte-for-byte
 * against a fresh browser reference (SSLKEYLOGFILE capture), the client
 * must expose its OWN side of every TLS conversation (CM HTTPS POST,
 * RTSP OPTIONS/DESCRIBE/SETUP/ANNOUNCE/PLAY, WS signaling) in plaintext.
 * Wireshark cannot: mbedTLS does not export session keys. So the dump
 * hooks the single choke point every TLS byte crosses:
 * kn_net_tls_stream_send / kn_net_tls_stream_recv.
 *
 * SECRETS: the dump would contain the session JWT, the ICE password and
 * any Authorization header. kn_wire_redact() overwrites those spans with
 * 'X' (LENGTH-PRESERVING — offsets in the dump still match the wire) and
 * counts the redactions; the wire_dump_end event line carries the count
 * so a dump with red=0 where secrets were expected is caught. The dump
 * file is still operator-sensitive: it must NEVER be pushed to a public
 * repo — see tools/wiretap/README.md.
 *
 * The dump covers TLS only. DTLS/SCTP (media + ctrl) is NOT dumped (it
 * would be gigabytes); ctrl plaintext is already evidenced by the
 * ctrl_msg / ctrl_in / ctrl_summary event lines.
 */

/* Open the dump file (truncating). Returns 0 on success, <0 on failure
 * (-1 bad args, -2 already open, -3 fopen failed). */
int  kn_wire_dump_open(const char *path);

/* Flush + close. Safe to call when not open. Counters survive close (so
 * the caller can report them) until the next open resets them. */
void kn_wire_dump_close(void);

int  kn_wire_dump_is_open(void);

/* Record one plaintext transfer. dir_out: 1 = outgoing (client->server,
 * 'W'), 0 = incoming ('R'). No-op when the dump is not open. The buffer
 * is COPIED and redacted — the caller's bytes are never modified.
 * Records longer than the internal cap (256 KiB) are truncated and the
 * line is marked " truncated". */
void kn_wire_dump_record(int dir_out, const unsigned char *buf, size_t len);

/* Counters since the last open (valid after close too). Any pointer may
 * be NULL. */
void kn_wire_dump_stats(unsigned long *records, unsigned long *redactions,
                        unsigned long long *bytes_out,
                        unsigned long long *bytes_in);

/* Length-preserving secret redaction over a raw byte buffer (NOT
 * NUL-terminated; len bytes). Overwrites sensitive spans with 'X'.
 * Returns the number of redacted spans. Exported for unit tests and
 * reusable by any tool that must publish captured payloads. Patterns:
 *   - "a=ice-pwd:" value up to CR/LF
 *   - "Authorization:" header value up to CR/LF or '"'
 *   - JSON string values of keys: token, jwt, authorization, secret,
 *     password, sessionkey, authtoken, accesstoken, refreshtoken
 *   - any base64url-ish run of >= 200 chars (JWT bodies, key blobs)
 * Deliberately NOT redacted (they are the audit subject):
 *   a=fingerprint, a=ice-ufrag, client ids, version strings, SDP fmtp. */
long kn_wire_redact(unsigned char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KN_WIRE_H */
