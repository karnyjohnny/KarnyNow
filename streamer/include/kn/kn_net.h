/* kn_net.h — phase-C network capability probes (BOOTSTRAP §7.9: net.dns,
 * net.tcp443, net.tls, tls.chain).
 *
 * Purpose: BEFORE any stage-2 protocol code is debugged, answer from the
 * target machine itself:
 *   - do the real GFN endpoints resolve?            (net.dns)
 *   - is outbound TCP/443 open to them?              (net.tcp443)
 *   - what TLS does the selected backend negotiate with them, and does the
 *     certificate VERIFY? (net.tls — never bypass verification; a failure
 *     here is the measurement, not something to work around)
 *   - what peer chain is presented for login.nvidia.com? (tls.chain)
 *
 * The default backend is Schannel. The optional mbedTLS backend verifies
 * hostnames and certificates against the Windows ROOT stores opened read-only.
 * Neither backend may skip certificate verification.
 *
 * All probes are bounded (DNS/connect ~5 s, socket I/O 10 s, whole TLS
 * probe well under §7.9's "~1 s each" ideal — network reality allowed up
 * to ~30 s worst case per host; the harness timeout covers it) and
 * side-effect free beyond the connection itself.
 *
 * Platform: Win32 real (ws2_32/secur32/crypt32 — all OS-built-in); POSIX
 * stub reports unavailable so the self-tests skip honestly on the host.
 */
#ifndef KN_NET_H
#define KN_NET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int kn_net_available(void);              /* 1 on Win32 */

/* ------------------------------------------------------------- net.dns */
typedef struct kn_net_dns {
    int rc;                              /* 0 ok; -1 getaddrinfo failed */
    int gai_error;                       /* raw getaddrinfo return */
    int wsa_error;                       /* WSAGetLastError when rc != 0 */
    unsigned a_count, aaaa_count;
    char first_v4[24];                   /* dotted-quad or "" */
    char first_v6[48];                   /* text or "" */
    long ms;
} kn_net_dns;

int kn_net_dns_lookup(const char *host, kn_net_dns *out);

/* ---------------------------------------------------------- net.tcp443 */
typedef struct kn_net_tcp {
    int rc;          /* 0 connected; -1 dns, -2 connect refused/failed,
                      * -3 select timeout, -4 socket error */
    int wsa_error;   /* WSAGetLastError / SO_ERROR at the failure */
    char peer_ip[24];
    long ms;
} kn_net_tcp;

int kn_net_tcp_connect443(const char *host, kn_net_tcp *out);

/* ------------------------------------------------------------ net.tls */
typedef struct kn_net_tls {
    int rc;          /* 0 handshake OK AND cert verified (schannel auto
                      * validation); -1 dns, -2 tcp, -3 tls handshake
                      * (sec_status carries the SECURITY_STATUS), -4 query */
    long sec_status; /* final InitializeSecurityContext status (0 when ok) */
    unsigned long protocol;      /* dwProtocol raw (SP_PROT_* bits) */
    char protocol_name[16];      /* "TLS1.2" / "TLS1.0" / "0x..." */
    unsigned cipher_alg, cipher_bits, hash_alg, exch_alg, exch_bits;
    char subject[64];            /* leaf cert, JSON-sanitized, truncated */
    char issuer[64];
    long ms;
} kn_net_tls;

int kn_net_tls_probe(const char *host, kn_net_tls *out);

/* ----------------------------------------------------------- tls.chain */
typedef struct kn_net_chain_elem {
    char subject[64];
    char issuer[64];
    char not_before[12];         /* "YYYY-MM-DD" */
    char not_after[12];
    unsigned long trust_error;   /* per-element TrustStatus.dwErrorStatus */
} kn_net_chain_elem;

#define KN_NET_CHAIN_MAX 6

typedef struct kn_net_chain {
    int rc;          /* 0 = handshake ok AND chain built (trust/policy
                      * verdicts are in the fields — a built chain with
                      * trust errors is still rc 0: the dump succeeded) */
    long sec_status;
    int depth;                       /* elements captured (<= MAX) */
    unsigned long chain_trust;       /* overall TrustStatus.dwErrorStatus */
    unsigned long policy_error;      /* CertVerifyCertificateChainPolicy */
    long policy_chain_index, policy_element_index;
    kn_net_chain_elem elem[KN_NET_CHAIN_MAX];
    long ms;
} kn_net_chain;

int kn_net_chain_dump(const char *host, kn_net_chain *out);

#ifdef __cplusplus
}
#endif

#endif /* KN_NET_H */
