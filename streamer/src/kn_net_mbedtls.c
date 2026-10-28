/* kn_net_mbedtls.c — Win32 network probes using the pinned mbedTLS backend. */
#include "kn/kn_net.h"

#include <stdio.h>    /* unconditional: the POSIX stub section (round XXI
                         TLS-stream stubs) formats error text too */
#include <stdlib.h>   /* calloc/free in the stream API — do NOT rely on
                         transitive includes (i686 mingw round-XXII catch:
                         x64 headers dragged stdlib in, i686 did not) */
#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <limits.h>
#include <stdio.h>

#include "kn/knlog.h"
#include "mbedtls/debug.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#define KN_NET_CONNECT_TIMEOUT_MS 5000
#define KN_NET_IO_TIMEOUT_MS      10000
#define KN_MBEDTLS_DEBUG_LEVEL    2

static int g_wsa_refs;

static void kn_mbed_debug(void *ctx, int level, const char *file, int line,
                          const char *message)
{
    char text[256];
    (void)ctx;
    if (level > KN_MBEDTLS_DEBUG_LEVEL || !kn_log_is_initialized()) return;
    snprintf(text, sizeof text, "mbedTLS[%d] %.80s:%d %.150s", level,
             file ? file : "?", line, message ? message : "");
    kn_log_simple(KN_CH_DIAG, KN_INFO, NULL, text);
}

static void kn_mbed_log_error(const char *phase, int error)
{
    char text[160], detail[96];
    if (!kn_log_is_initialized()) return;
    mbedtls_strerror(error, detail, sizeof detail);
    snprintf(text, sizeof text, "mbedTLS %s failed: -0x%04x (%s)",
             phase, (unsigned)(-error) & 0xffffu, detail);
    kn_log_simple(KN_CH_DIAG, KN_WARN, NULL, text);
}

static int kn_wsa_begin(void)
{
    WSADATA data;
    if (g_wsa_refs > 0) {
        ++g_wsa_refs;
        return 0;
    }
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
    g_wsa_refs = 1;
    return 0;
}

static void kn_wsa_end(void)
{
    if (g_wsa_refs > 0 && --g_wsa_refs == 0) WSACleanup();
}

static long long kn_mono_ms(void)
{
    LARGE_INTEGER frequency, counter;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return 0;
    QueryPerformanceCounter(&counter);
    return (long long)((double)counter.QuadPart * 1000.0 /
                       (double)frequency.QuadPart);
}

static void kn_json_copy(char *dst, size_t dstsz, const char *src)
{
    size_t i;
    if (!dst || dstsz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    for (i = 0; i + 1 < dstsz && src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20u || c == '"' || c == '\\') ? '?' : (char)c;
    }
    dst[i] = '\0';
}

static SOCKET kn_tcp_open_port(const char *host, uint16_t port, int *rc,
                               int *wsa_error, char *peer_ip, size_t peer_ipsz,
                               long *ms_out)
{
    struct addrinfo hints, *results = NULL, *entry;
    SOCKET connected = INVALID_SOCKET;
    long long start = kn_mono_ms();
    int gai_rc;

    *rc = -1;
    *wsa_error = 0;
    if (peer_ip && peer_ipsz) peer_ip[0] = '\0';
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    {
        char portstr[8];
        snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
        gai_rc = getaddrinfo(host, portstr, &hints, &results);
    }
    if (gai_rc != 0 || !results) {
        *wsa_error = WSAGetLastError();
        if (results) freeaddrinfo(results);
        if (ms_out) *ms_out = (long)(kn_mono_ms() - start);
        return INVALID_SOCKET;
    }

    for (entry = results; entry; entry = entry->ai_next) {
        SOCKET sock = socket(entry->ai_family, entry->ai_socktype,
                             entry->ai_protocol);
        u_long nonblocking = 1;
        int connect_rc;

        if (sock == INVALID_SOCKET) {
            *wsa_error = WSAGetLastError();
            continue;
        }
        if (ioctlsocket(sock, FIONBIO, &nonblocking) != 0) {
            *rc = -4;
            *wsa_error = WSAGetLastError();
            closesocket(sock);
            continue;
        }

        connect_rc = connect(sock, entry->ai_addr, (int)entry->ai_addrlen);
        if (connect_rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set write_set, error_set;
            struct timeval timeout;
            int selected, socket_error = 0;
            int socket_error_len = sizeof socket_error;

            FD_ZERO(&write_set);
            FD_SET(sock, &write_set);
            FD_ZERO(&error_set);
            FD_SET(sock, &error_set);
            timeout.tv_sec = KN_NET_CONNECT_TIMEOUT_MS / 1000;
            timeout.tv_usec = (KN_NET_CONNECT_TIMEOUT_MS % 1000) * 1000;
            selected = select(0, NULL, &write_set, &error_set, &timeout);
            if (selected > 0 && FD_ISSET(sock, &write_set)) {
                getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&socket_error,
                           &socket_error_len);
                connect_rc = socket_error == 0 ? 0 : SOCKET_ERROR;
                if (socket_error != 0) *wsa_error = socket_error;
            } else {
                *rc = selected == 0 ? -3 : -4;
                if (selected < 0) *wsa_error = WSAGetLastError();
                connect_rc = SOCKET_ERROR;
            }
        } else if (connect_rc != 0) {
            *rc = -2;
            *wsa_error = WSAGetLastError();
        }

        if (connect_rc == 0) {
            DWORD io_timeout = KN_NET_IO_TIMEOUT_MS;
            nonblocking = 0;
            if (ioctlsocket(sock, FIONBIO, &nonblocking) != 0) {
                *rc = -4;
                *wsa_error = WSAGetLastError();
                closesocket(sock);
                continue;
            }
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                       (const char *)&io_timeout, sizeof io_timeout);
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                       (const char *)&io_timeout, sizeof io_timeout);
            if (peer_ip && peer_ipsz) {
                const struct sockaddr_in *address =
                    (const struct sockaddr_in *)entry->ai_addr;
                inet_ntop(AF_INET, &address->sin_addr, peer_ip,
                          (socklen_t)peer_ipsz);
            }
            connected = sock;
            *rc = 0;
            break;
        }
        closesocket(sock);
    }

    freeaddrinfo(results);
    if (ms_out) *ms_out = (long)(kn_mono_ms() - start);
    return connected;
}

static int kn_load_root_store(mbedtls_x509_crt *roots)
{
    static const DWORD locations[] = {
        CERT_SYSTEM_STORE_CURRENT_USER,
        CERT_SYSTEM_STORE_LOCAL_MACHINE
    };
    size_t i;
    unsigned parsed = 0;

    for (i = 0; i < sizeof locations / sizeof locations[0]; ++i) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_A,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                locations[i] | CERT_STORE_OPEN_EXISTING_FLAG |
                CERT_STORE_READONLY_FLAG, "ROOT");
        PCCERT_CONTEXT cert = NULL;
        if (!store) continue;
        while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL) {
            if (mbedtls_x509_crt_parse_der(roots, cert->pbCertEncoded,
                                           cert->cbCertEncoded) == 0)
                ++parsed;
        }
        CertCloseStore(store, 0);
    }
    return parsed ? 0 : MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
}

static int kn_ssl_send(void *ctx, const unsigned char *buf, size_t len)
{
    SOCKET sock = *(SOCKET *)ctx;
    int error, sent = send(sock, (const char *)buf,
                           (int)(len > INT_MAX ? INT_MAX : len), 0);
    if (sent > 0) return sent;
    error = WSAGetLastError();
    if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int kn_ssl_recv(void *ctx, unsigned char *buf, size_t len)
{
    SOCKET sock = *(SOCKET *)ctx;
    int error, received = recv(sock, (char *)buf,
                               (int)(len > INT_MAX ? INT_MAX : len), 0);
    if (received > 0) return received;
    if (received == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    error = WSAGetLastError();
    if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static void kn_cert_names(const mbedtls_x509_crt *cert,
                          char *subject, size_t subject_sz,
                          char *issuer, size_t issuer_sz)
{
    char temp[256];
    if (!cert) {
        if (subject && subject_sz) subject[0] = '\0';
        if (issuer && issuer_sz) issuer[0] = '\0';
        return;
    }
    if (mbedtls_x509_dn_gets(temp, sizeof temp, &cert->subject) < 0)
        temp[0] = '\0';
    kn_json_copy(subject, subject_sz, temp);
    if (mbedtls_x509_dn_gets(temp, sizeof temp, &cert->issuer) < 0)
        temp[0] = '\0';
    kn_json_copy(issuer, issuer_sz, temp);
}

static void kn_cert_date(const mbedtls_x509_time *date,
                         char *out, size_t outsz)
{
    if (out && outsz)
        snprintf(out, outsz, "%04d-%02d-%02d", date->year, date->mon,
                 date->day);
}

static void kn_copy_chain(const mbedtls_ssl_context *ssl,
                          kn_net_chain *chain)
{
    const mbedtls_x509_crt *cert;
    uint32_t verify_flags = mbedtls_ssl_get_verify_result(ssl);
    chain->chain_trust = (unsigned long)verify_flags;
    chain->policy_error = (unsigned long)verify_flags;
    for (cert = mbedtls_ssl_get_peer_cert(ssl); cert &&
         chain->depth < KN_NET_CHAIN_MAX; cert = cert->next) {
        kn_net_chain_elem *elem = &chain->elem[chain->depth];
        kn_cert_names(cert, elem->subject, sizeof elem->subject,
                      elem->issuer, sizeof elem->issuer);
        kn_cert_date(&cert->valid_from, elem->not_before,
                     sizeof elem->not_before);
        kn_cert_date(&cert->valid_to, elem->not_after,
                     sizeof elem->not_after);
        elem->trust_error = (unsigned long)verify_flags;
        ++chain->depth;
    }
}

static int kn_tls_session(const char *host, kn_net_tls *tls,
                          kn_net_chain *chain)
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt roots;
    SOCKET sock = INVALID_SOCKET;
    int tcp_rc = 0, wsa_error = 0, ret, wsa_started = 0;
    long tcp_ms = 0;
    long long start;
    const mbedtls_x509_crt *peer;

    if (tls) memset(tls, 0, sizeof *tls);
    if (chain) memset(chain, 0, sizeof *chain);
    if (!host || (!tls && !chain)) return -1;
    if (kn_wsa_begin() != 0) {
        int error = WSAGetLastError();
        if (tls) { tls->rc = -1; tls->sec_status = error; }
        if (chain) { chain->rc = -1; chain->sec_status = error; }
        return -1;
    }
    wsa_started = 1;
    sock = kn_tcp_open_port(host, 443, &tcp_rc, &wsa_error, NULL, 0, &tcp_ms);
    if (sock == INVALID_SOCKET) {
        if (tls) { tls->rc = -2; tls->sec_status = wsa_error; tls->ms = tcp_ms; }
        if (chain) { chain->rc = -2; chain->sec_status = wsa_error; chain->ms = tcp_ms; }
        ret = -2;
        goto cleanup_socket;
    }

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ssl_config_init(&config);
    mbedtls_ssl_init(&ssl);
    mbedtls_x509_crt_init(&roots);
    start = kn_mono_ms();

    {
        psa_status_t psa_status = psa_crypto_init();
        if (psa_status != PSA_SUCCESS) {
            char text[128];
            snprintf(text, sizeof text, "PSA crypto init failed: %d",
                     (int)psa_status);
            if (kn_log_is_initialized())
                kn_log_simple(KN_CH_DIAG, KN_WARN, NULL, text);
            ret = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        } else {
            ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                        (const unsigned char *)"KarnyNow TLS", 12);
        }
    }
    if (ret == 0) ret = kn_load_root_store(&roots);
    if (ret == 0)
        ret = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0) {
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config, &roots, NULL);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_dbg(&config, kn_mbed_debug, NULL);
        mbedtls_debug_set_threshold(KN_MBEDTLS_DEBUG_LEVEL);
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        ret = mbedtls_ssl_setup(&ssl, &config);
    }
    if (ret == 0) ret = mbedtls_ssl_set_hostname(&ssl, host);
    if (ret == 0) {
        mbedtls_ssl_set_bio(&ssl, &sock, kn_ssl_send, kn_ssl_recv, NULL);
        ret = mbedtls_ssl_handshake(&ssl);
    }

    if (chain) {
        chain->ms = (long)(kn_mono_ms() - start);
        chain->sec_status = ret;
        kn_copy_chain(&ssl, chain);
    }
    if (tls) {
        tls->ms = (long)(kn_mono_ms() - start);
        tls->sec_status = ret;
    }
    if (ret != 0) {
        kn_mbed_log_error("TLS handshake", ret);
        if (tls) tls->rc = -3;
        if (chain) chain->rc = -3;
        ret = -3;
        goto cleanup_tls;
    }

    peer = mbedtls_ssl_get_peer_cert(&ssl);
    if (!peer || mbedtls_ssl_get_verify_result(&ssl) != 0) {
        ret = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
        kn_mbed_log_error("certificate verification", ret);
        if (tls) { tls->rc = -3; tls->sec_status = ret; }
        if (chain) { chain->rc = -3; chain->sec_status = ret; }
        ret = -3;
        goto cleanup_tls;
    }
    if (tls) {
        const char *version = mbedtls_ssl_get_version(&ssl);
        const mbedtls_ssl_ciphersuite_t *suite =
            mbedtls_ssl_ciphersuite_from_id(
                mbedtls_ssl_get_ciphersuite_id_from_ssl(&ssl));
        tls->protocol = version && strcmp(version, "TLSv1.3") == 0 ? 0x2000ul :
                        version && strcmp(version, "TLSv1.2") == 0 ? 0x0800ul : 0;
        kn_json_copy(tls->protocol_name, sizeof tls->protocol_name, version);
        tls->cipher_alg = suite ?
            (unsigned)suite->MBEDTLS_PRIVATE(cipher) : 0;
        tls->cipher_bits = suite ? (unsigned)
            mbedtls_ssl_ciphersuite_get_cipher_key_bitlen(suite) : 0;
        tls->hash_alg = suite ? (unsigned)suite->MBEDTLS_PRIVATE(mac) : 0;
        tls->exch_alg = suite ?
            (unsigned)suite->MBEDTLS_PRIVATE(key_exchange) : 0;
        tls->exch_bits = 0;
        kn_cert_names(peer, tls->subject, sizeof tls->subject,
                      tls->issuer, sizeof tls->issuer);
        tls->rc = 0;
    }
    if (chain) chain->rc = chain->depth > 0 ? 0 : -4;
    ret = 0;

cleanup_tls:
    mbedtls_debug_set_threshold(0);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_x509_crt_free(&roots);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
cleanup_socket:
    if (sock != INVALID_SOCKET) closesocket(sock);
    if (wsa_started) kn_wsa_end();
    return ret;
}

int kn_net_available(void) { return 1; }

int kn_net_dns_lookup(const char *host, kn_net_dns *out)
{
    struct addrinfo hints, *results = NULL, *entry;
    long long start;
    int gai_rc;
    if (!host || !out) return -1;
    memset(out, 0, sizeof *out);
    if (kn_wsa_begin() != 0) {
        out->rc = -1;
        out->wsa_error = WSAGetLastError();
        return -1;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    start = kn_mono_ms();
    gai_rc = getaddrinfo(host, "443", &hints, &results);
    out->ms = (long)(kn_mono_ms() - start);
    if (gai_rc != 0) {
        out->rc = -1;
        out->gai_error = gai_rc;
        out->wsa_error = WSAGetLastError();
        if (results) freeaddrinfo(results);
        kn_wsa_end();
        return -1;
    }
    for (entry = results; entry; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET) {
            ++out->a_count;
            if (!out->first_v4[0]) {
                const struct sockaddr_in *addr =
                    (const struct sockaddr_in *)entry->ai_addr;
                inet_ntop(AF_INET, &addr->sin_addr, out->first_v4,
                          sizeof out->first_v4);
            }
        } else if (entry->ai_family == AF_INET6) {
            ++out->aaaa_count;
            if (!out->first_v6[0]) {
                const struct sockaddr_in6 *addr =
                    (const struct sockaddr_in6 *)entry->ai_addr;
                inet_ntop(AF_INET6, &addr->sin6_addr, out->first_v6,
                          sizeof out->first_v6);
            }
        }
    }
    freeaddrinfo(results);
    out->rc = 0;
    kn_wsa_end();
    return 0;
}

int kn_net_tcp_connect443(const char *host, kn_net_tcp *out)
{
    SOCKET sock;
    int rc, wsa_error;
    if (!host || !out) return -1;
    memset(out, 0, sizeof *out);
    if (kn_wsa_begin() != 0) {
        out->rc = -4;
        out->wsa_error = WSAGetLastError();
        return -1;
    }
    sock = kn_tcp_open_port(host, 443, &rc, &wsa_error, out->peer_ip,
                       sizeof out->peer_ip, &out->ms);
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        out->rc = 0;
    } else {
        out->rc = rc;
        out->wsa_error = wsa_error;
    }
    kn_wsa_end();
    return out->rc;
}

int kn_net_tls_probe(const char *host, kn_net_tls *out)
{
    if (!host || !out) return -1;
    return kn_tls_session(host, out, NULL);
}

int kn_net_chain_dump(const char *host, kn_net_chain *out)
{
    if (!host || !out) return -1;
    return kn_tls_session(host, NULL, out);
}

/* ------------------------------------------------- persistent TLS stream
 * Round XXI: the production transport under kn_ws (RTSPS/WSS, port 322)
 * and kn_http/kn_cm (CloudMatch, 443). The TLS policy is IDENTICAL to the
 * T12-verified probe path: VERIFY_REQUIRED, hostname/SNI, read-only
 * Windows ROOT stores, min=max=TLS 1.2, PSA initialized before use.
 * Verification is never disabled; there is no plaintext fallback. */

struct kn_net_tls_stream {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt roots;
    SOCKET sock;
    int wsa_started;
    char peer_ip[24];
    long handshake_ms;
};

int kn_net_tls_stream_available(void) { return 1; }

kn_net_tls_stream *kn_net_tls_stream_open(const char *host, uint16_t port,
                                          uint32_t timeout_ms,
                                          char *err, size_t errsz)
{
    kn_net_tls_stream *s;
    int tcp_rc = 0, wsa_error = 0, ret;
    long tcp_ms = 0;
    long long start;
    DWORD io_timeout;

    if (err && errsz) err[0] = '\0';
    if (!host || !host[0]) {
        if (err && errsz) snprintf(err, errsz, "no host");
        return NULL;
    }
    s = (kn_net_tls_stream *)calloc(1, sizeof *s);
    if (!s) {
        if (err && errsz) snprintf(err, errsz, "alloc failed");
        return NULL;
    }
    s->sock = INVALID_SOCKET;
    if (kn_wsa_begin() != 0) {
        if (err && errsz) snprintf(err, errsz, "WSAStartup failed (%d)",
                                   WSAGetLastError());
        free(s);
        return NULL;
    }
    s->wsa_started = 1;
    s->sock = kn_tcp_open_port(host, port, &tcp_rc, &wsa_error,
                               s->peer_ip, sizeof s->peer_ip, &tcp_ms);
    if (s->sock == INVALID_SOCKET) {
        if (err && errsz) snprintf(err, errsz,
                                   "TCP connect to %.120s:%u failed "
                                   "(rc %d, wsa %d)", host,
                                   (unsigned)port, tcp_rc, wsa_error);
        kn_net_tls_stream_close(s);
        return NULL;
    }
    io_timeout = timeout_ms ? (DWORD)timeout_ms : (DWORD)KN_NET_IO_TIMEOUT_MS;
    setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&io_timeout,
               sizeof io_timeout);
    setsockopt(s->sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&io_timeout,
               sizeof io_timeout);

    mbedtls_entropy_init(&s->entropy);
    mbedtls_ctr_drbg_init(&s->drbg);
    mbedtls_ssl_config_init(&s->config);
    mbedtls_ssl_init(&s->ssl);
    mbedtls_x509_crt_init(&s->roots);
    start = kn_mono_ms();

    {
        psa_status_t psa_status = psa_crypto_init();
        if (psa_status != PSA_SUCCESS)
            ret = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        else
            ret = mbedtls_ctr_drbg_seed(&s->drbg, mbedtls_entropy_func,
                                        &s->entropy,
                                        (const unsigned char *)"KarnyNow TLS",
                                        12);
    }
    if (ret == 0) ret = kn_load_root_store(&s->roots);
    if (ret == 0)
        ret = mbedtls_ssl_config_defaults(&s->config, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0) {
        mbedtls_ssl_conf_authmode(&s->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&s->config, &s->roots, NULL);
        mbedtls_ssl_conf_rng(&s->config, mbedtls_ctr_drbg_random, &s->drbg);
        mbedtls_ssl_conf_dbg(&s->config, kn_mbed_debug, NULL);
        mbedtls_debug_set_threshold(KN_MBEDTLS_DEBUG_LEVEL);
        mbedtls_ssl_conf_min_tls_version(&s->config,
                                         MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&s->config,
                                         MBEDTLS_SSL_VERSION_TLS1_2);
        ret = mbedtls_ssl_setup(&s->ssl, &s->config);
    }
    if (ret == 0) ret = mbedtls_ssl_set_hostname(&s->ssl, host);
    if (ret == 0) {
        mbedtls_ssl_set_bio(&s->ssl, &s->sock, kn_ssl_send, kn_ssl_recv,
                            NULL);
        ret = mbedtls_ssl_handshake(&s->ssl);
    }
    s->handshake_ms = (long)(kn_mono_ms() - start);
    if (ret != 0) {
        char why[192];
        mbedtls_strerror(ret, why, sizeof why);
        if (err && errsz)
            snprintf(err, errsz,
                     "TLS handshake with %.120s:%u failed (-0x%04x: %.100s)",
                     host, (unsigned)port, (unsigned)(-ret), why);
        if (kn_log_is_initialized()) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "tls.stream handshake failed -0x%04x host=%.120s",
                     (unsigned)(-ret), host);
            kn_log_simple(KN_CH_DIAG, KN_WARN, NULL, msg);
        }
        kn_net_tls_stream_close(s);
        return NULL;
    }
    return s;
}

int kn_net_tls_stream_send(kn_net_tls_stream *s, const uint8_t *buf,
                           size_t len)
{
    size_t off = 0;
    if (!s || (!buf && len)) return -1;
    while (off < len) {
        int ret = mbedtls_ssl_write(&s->ssl, buf + off, len - off);
        if (ret > 0) { off += (size_t)ret; continue; }
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return -1;                       /* timeout/send failure: fatal */
    }
    return 0;
}

int kn_net_tls_stream_recv(kn_net_tls_stream *s, uint8_t *buf, size_t cap,
                           uint32_t timeout_ms)
{
    int ret;
    DWORD io_timeout;
    if (!s || !buf || cap == 0) return -1;
    io_timeout = timeout_ms ? (DWORD)timeout_ms : (DWORD)KN_NET_IO_TIMEOUT_MS;

    /* ROUND XL — THE RUN-20/21 ROOT CAUSE. SO_RCVTIMEO on Windows is NOT
     * millisecond-accurate: winsock rounds it to the AFD poll granularity
     * (~500 ms on every Windows since NT). The R3-D control-ping drain
     * calls this with timeout_ms=1 from the MEDIA LOOP's 50 ms service
     * beat — and each call blocked ~500 ms instead, pinning the whole
     * loop to a 2 Hz tick. Every sub-second cadence collapsed onto it
     * (run 21: QoS 597 = datagrams+1, NATT 597, recv_timeouts 1; the
     * reference sends QoS at 20/s). select() IS ms-accurate on winsock,
     * so the wait happens here; SO_RCVTIMEO stays as the belt-and-
     * suspenders bound for a partial TLS record already in flight.
     * mbedtls_ssl_check_pending() keeps buffered plaintext (a second WS
     * frame in one record) from waiting on the socket at all. */
    if (!mbedtls_ssl_check_pending(&s->ssl)) {
        fd_set rfds;
        struct timeval tv;
        int sel;
        FD_ZERO(&rfds);
        FD_SET(s->sock, &rfds);
        tv.tv_sec  = (long)(io_timeout / 1000u);
        tv.tv_usec = (long)((io_timeout % 1000u) * 1000u);
        sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel == 0) return 0;              /* retryable timeout */
        if (sel < 0) return -1;
    }
    setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&io_timeout,
               sizeof io_timeout);
    ret = mbedtls_ssl_read(&s->ssl, buf, (int)(cap > INT_MAX ? INT_MAX : cap));
    if (ret > 0) return ret;
    if (ret == 0) return -2;                              /* orderly EOF */
    if (ret == MBEDTLS_ERR_SSL_TIMEOUT || ret == MBEDTLS_ERR_SSL_WANT_READ)
        return 0;                                         /* retryable */
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        ret == MBEDTLS_ERR_NET_CONN_RESET)
        return -2;                                        /* peer closed */
    return -1;
}

void kn_net_tls_stream_close(kn_net_tls_stream *s)
{
    if (!s) return;
    if (s->ssl.MBEDTLS_PRIVATE(state) != 0)
        (void)mbedtls_ssl_close_notify(&s->ssl);
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->config);
    mbedtls_x509_crt_free(&s->roots);
    mbedtls_ctr_drbg_free(&s->drbg);
    mbedtls_entropy_free(&s->entropy);
    if (s->sock != INVALID_SOCKET) {
        closesocket(s->sock);
        s->sock = INVALID_SOCKET;
    }
    if (s->wsa_started) kn_wsa_end();
    free(s);
}


#else

int kn_net_available(void) { return 0; }

int kn_net_dns_lookup(const char *host, kn_net_dns *out)
{
    (void)host;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->rc = -1;
    return -1;
}

int kn_net_tcp_connect443(const char *host, kn_net_tcp *out)
{
    (void)host;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->rc = -4;
    return -1;
}

int kn_net_tls_probe(const char *host, kn_net_tls *out)
{
    (void)host;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->rc = -3;
    return -1;
}

int kn_net_chain_dump(const char *host, kn_net_chain *out)
{
    (void)host;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->rc = -3;
    return -1;
}

/* TLS stream stubs for POSIX/mbedTLS builds (the Win32 section above has
 * the real ones; schannel builds get theirs from kn_net.c — exactly one
 * definition set per build configuration). */
int kn_net_tls_stream_available(void) { return 0; }

kn_net_tls_stream *kn_net_tls_stream_open(const char *host, uint16_t port,
                                          uint32_t timeout_ms,
                                          char *err, size_t errsz)
{
    (void)host; (void)port; (void)timeout_ms;
    if (err && errsz)
        snprintf(err, errsz,
                 "TLS stream is Win32+mbedTLS only in this build");
    return NULL;
}

int kn_net_tls_stream_send(kn_net_tls_stream *s, const uint8_t *buf,
                           size_t len)
{
    (void)s; (void)buf; (void)len;
    return -1;
}

int kn_net_tls_stream_recv(kn_net_tls_stream *s, uint8_t *buf, size_t cap,
                           uint32_t timeout_ms)
{
    (void)s; (void)buf; (void)cap; (void)timeout_ms;
    return -1;
}

void kn_net_tls_stream_close(kn_net_tls_stream *s) { (void)s; }

#endif