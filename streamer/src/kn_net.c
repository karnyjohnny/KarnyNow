/* kn_net.c — phase-C Schannel probes (contract: kn_net.h).
 *
 * Winsock + Schannel + crypt32, every pattern per MSDN's documented
 * "Creating an Schannel Security Context" flow; all spellings verified
 * against the mingw-w64 headers this round (sspi.h/schannel.h/wincrypt.h —
 * the d3d9/wasapi lesson: never from memory). Verification is NEVER
 * disabled (working agreement): SNI target name is passed, dwFlags = 0
 * (Schannel auto credential validation), and a failed verification is
 * REPORTED as the measurement (sec_status), not worked around.
 *
 * mingw's schannel.h predates TLS 1.1+; the SP_PROT_* client bits below
 * are the stable ABI values from the Windows SDK (decode-only use: we
 * never ENABLE specific protocols — grbitEnabledProtocols stays 0 so the
 * probe measures the machine's stock Schannel defaults, which is exactly
 * the stage-2 question on Win7).
 */
#include "kn/kn_net.h"

#include <stdio.h>    /* unconditional: the round-XXI TLS-stream
                         stubs format error text on every platform */
#include <string.h>

#if defined(KN_TLS_BACKEND_MBEDTLS)
/* Mbed TLS is added as an opt-in backend for the JG Win7 target. The backend is
 * deliberately isolated behind a compile-time switch so the project can be
 * validated in the sandbox before the laptop run. The Schannel path stays the
 * default until the backend is explicitly selected.
 */
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#else

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define SECURITY_WIN32        /* required by sspi.h before inclusion */
#include <sspi.h>
#include <schannel.h>
#include <wincrypt.h>
#include <stdarg.h>
#include <stdio.h>

#include "kn/knlog.h"

/* Windows SDK values missing from mingw's older schannel.h (decode-only). */
#ifndef SP_PROT_TLS1_1_CLIENT
#define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif
#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#define SP_PROT_TLS1_3_CLIENT 0x00002000
#endif
#ifndef SP_PROT_SSL3_CLIENT
#define SP_PROT_SSL3_CLIENT   0x00000020
#endif

#define KN_NET_CONNECT_TIMEOUT_MS 5000
#define KN_NET_IO_TIMEOUT_MS      10000
#define KN_NET_RECV_BUF           (17 * 1024)   /* >= max TLS record + hdr */

/* ------------------------------------------------------------- plumbing */

static int g_wsa_refs;

static int kn_wsa_begin(void)
{
    WSADATA w;
    if (g_wsa_refs > 0) { g_wsa_refs++; return 0; }
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
    g_wsa_refs = 1;
    return 0;
}

static void kn_wsa_end(void)
{
    if (g_wsa_refs > 0 && --g_wsa_refs == 0) WSACleanup();
}

static long long kn_mono_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER pc;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&pc);
    if (!freq.QuadPart) return 0;
    return (long long)((double)pc.QuadPart * 1000.0 / (double)freq.QuadPart);
}

static void kn_logf(int level, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    if (!kn_log_is_initialized()) return;
    if (!kn_log_would_log(KN_CH_DIAG, (kn_level)level)) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg[strcspn(msg, "\r\n")] = '\0';
    if (msg[0])
        kn_log_simple(KN_CH_DIAG, (kn_level)level, NULL, msg);
}

/* JSON-safe copy: control chars/quote/backslash -> '?', bounded, NUL. */
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

static void kn_proto_name(unsigned long p, char *out, size_t outsz)
{
    const char *n;
    if (p & SP_PROT_TLS1_3_CLIENT)       n = "TLS1.3";
    else if (p & SP_PROT_TLS1_2_CLIENT)  n = "TLS1.2";
    else if (p & SP_PROT_TLS1_1_CLIENT)  n = "TLS1.1";
    else if (p & SP_PROT_TLS1_CLIENT)    n = "TLS1.0";
    else if (p & SP_PROT_SSL3_CLIENT)    n = "SSL3";
    else if (p & SP_PROT_SSL2_CLIENT)    n = "SSL2";
    else { snprintf(out, outsz, "0x%08lx", p); return; }
    snprintf(out, outsz, "%s", n);
}

static void kn_filetime_date(const FILETIME *ft, char *out, size_t outsz)
{
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(ft, &st)) {
        snprintf(out, outsz, "invalid");
        return;
    }
    snprintf(out, outsz, "%04u-%02u-%02u", (unsigned)st.wYear % 10000u,
             (unsigned)st.wMonth % 100u, (unsigned)st.wDay % 100u);
}

static int kn_send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int kn_net_available(void) { return 1; }

/* ---------------------------------------------------------------- DNS */

int kn_net_dns_lookup(const char *host, kn_net_dns *out)
{
    struct addrinfo hints, *res = NULL, *it;
    long long t0;
    int gairc;

    if (!host || !out) return -1;
    memset(out, 0, sizeof *out);
    if (kn_wsa_begin() != 0) { out->rc = -1; out->wsa_error = WSAGetLastError(); return -1; }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    t0 = kn_mono_ms();
    gairc = getaddrinfo(host, "443", &hints, &res);
    out->ms = (long)(kn_mono_ms() - t0);
    if (gairc != 0) {
        out->rc = -1;
        out->gai_error = gairc;
        out->wsa_error = WSAGetLastError();
        freeaddrinfo(res);
        kn_wsa_end();
        return -1;
    }
    for (it = res; it; it = it->ai_next) {
        if (it->ai_family == AF_INET) {
            out->a_count++;
            if (!out->first_v4[0]) {
                struct sockaddr_in *sa = (struct sockaddr_in *)it->ai_addr;
                inet_ntop(AF_INET, &sa->sin_addr, out->first_v4,
                          sizeof out->first_v4);
            }
        } else if (it->ai_family == AF_INET6) {
            out->aaaa_count++;
            if (!out->first_v6[0]) {
                struct sockaddr_in6 *sa = (struct sockaddr_in6 *)it->ai_addr;
                inet_ntop(AF_INET6, &sa->sin6_addr, out->first_v6,
                          sizeof out->first_v6);
            }
        }
    }
    freeaddrinfo(res);
    out->rc = 0;
    kn_wsa_end();
    return 0;
}

/* ---------------------------------------------------------------- TCP */

/* Connect to host:443 (first A record), non-blocking + select for the
 * timeout. Returns the connected BLOCKING socket (timeouts set) or
 * INVALID_SOCKET with *rc filled. */
static SOCKET kn_tcp_open(const char *host, int *rc, int *wsa_err,
                          char *peer_ip, size_t peer_ipsz, long *ms_out)
{
    struct addrinfo hints, *res = NULL, *it;
    SOCKET s = INVALID_SOCKET;
    long long t0;
    int gairc;

    *rc = -1; *wsa_err = 0;
    if (peer_ip && peer_ipsz) peer_ip[0] = '\0';
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;                 /* IPv4 first: Win7 + GFN */
    hints.ai_socktype = SOCK_STREAM;
    gairc = getaddrinfo(host, "443", &hints, &res);
    if (gairc != 0 || !res) {
        *rc = -1;
        *wsa_err = WSAGetLastError();
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    t0 = kn_mono_ms();
    for (it = res; it; it = it->ai_next) {
        u_long nb = 1;
        fd_set wfds, efds;
        struct timeval tv;
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { *wsa_err = WSAGetLastError(); continue; }
        ioctlsocket(s, FIONBIO, &nb);
        if (connect(s, it->ai_addr, (int)it->ai_addrlen) == 0) {
            struct sockaddr_in *sa = (struct sockaddr_in *)it->ai_addr;
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
            if (peer_ip && peer_ipsz)
                inet_ntop(AF_INET, &sa->sin_addr, peer_ip, (socklen_t)peer_ipsz);
            *rc = 0;
            break;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            *rc = -2; *wsa_err = WSAGetLastError();
            closesocket(s); s = INVALID_SOCKET;
            continue;
        }
        FD_ZERO(&wfds); FD_SET(s, &wfds);
        FD_ZERO(&efds); FD_SET(s, &efds);
        tv.tv_sec = KN_NET_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (KN_NET_CONNECT_TIMEOUT_MS % 1000) * 1000;
        {
            int sel = select(0, NULL, &wfds, &efds, &tv);
            if (sel > 0 && FD_ISSET(s, &wfds)) {
                int so_err = 0;
                int olen = sizeof so_err;
                struct sockaddr_in *sa = (struct sockaddr_in *)it->ai_addr;
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &olen);
                if (so_err == 0) {
                    nb = 0;
                    ioctlsocket(s, FIONBIO, &nb);
                    if (peer_ip && peer_ipsz)
                        inet_ntop(AF_INET, &sa->sin_addr, peer_ip,
                                  (socklen_t)peer_ipsz);
                    *rc = 0;
                    break;
                }
                *rc = -2; *wsa_err = so_err;
            } else if (sel == 0) {
                *rc = -3;                      /* select timeout */
            } else {
                *rc = -4; *wsa_err = WSAGetLastError();
            }
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    if (s != INVALID_SOCKET) {
        DWORD to = KN_NET_IO_TIMEOUT_MS;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    }
    freeaddrinfo(res);
    if (ms_out) *ms_out = (long)(kn_mono_ms() - t0);
    return s;
}

int kn_net_tcp_connect443(const char *host, kn_net_tcp *out)
{
    SOCKET s;
    if (!host || !out) return -1;
    memset(out, 0, sizeof *out);
    if (kn_wsa_begin() != 0) { out->rc = -4; out->wsa_error = WSAGetLastError(); return -1; }
    s = kn_tcp_open(host, &out->rc, &out->wsa_error, out->peer_ip,
                    sizeof out->peer_ip, &out->ms);
    if (s != INVALID_SOCKET) closesocket(s);
    kn_wsa_end();
    return out->rc;
}

/* ---------------------------------------------------------------- TLS */

static void kn_cert_names(PCCERT_CONTEXT cert, char *subj, size_t subjsz,
                          char *iss, size_t isssz)
{
    char tmp[256];
    DWORD n;
    if (!cert || !cert->pCertInfo) {
        if (subj && subjsz) subj[0] = '\0';
        if (iss && isssz) iss[0] = '\0';
        return;
    }
    n = CertNameToStrA(cert->dwCertEncodingType, &cert->pCertInfo->Subject,
                       CERT_X500_NAME_STR, tmp, sizeof tmp);
    kn_json_copy(subj, subjsz, n ? tmp : "");
    n = CertNameToStrA(cert->dwCertEncodingType, &cert->pCertInfo->Issuer,
                       CERT_X500_NAME_STR, tmp, sizeof tmp);
    kn_json_copy(iss, isssz, n ? tmp : "");
}

/* Full TLS session: connect -> schannel handshake (verification ON) ->
 * connection info -> remote leaf (+ optional chain). One implementation
 * shared by kn_net_tls_probe and kn_net_chain_dump. */
static int kn_tls_session(const char *host, kn_net_tls *tls,
                          kn_net_chain *chain)
{
    SOCKET s;
    CredHandle cred;
    CtxtHandle ctx;
    SCHANNEL_CRED sc;
    SECURITY_INTEGER expiry;
    SECURITY_STATUS st;
    DWORD attrs = 0;
    unsigned char rbuf[KN_NET_RECV_BUF];
    size_t rused = 0;
    int have_ctx = 0, rc_tcp = 0, wsa = 0;
    long ms_tcp = 0;
    long long t0;
    PCCERT_CONTEXT remote = NULL;
    int loops = 0;

    if (tls) memset(tls, 0, sizeof *tls);
    if (chain) memset(chain, 0, sizeof *chain);
    if (kn_wsa_begin() != 0) return -1;

    s = kn_tcp_open(host, &rc_tcp, &wsa, NULL, 0, &ms_tcp);
    if (s == INVALID_SOCKET) {
        kn_logf(KN_WARN, "net probe %s: TCP connect failed rc=%d wsa=%d "
                "(%ld ms)", host, rc_tcp, wsa, ms_tcp);
        kn_wsa_end();
        if (tls) { tls->rc = -2; tls->sec_status = wsa; tls->ms = ms_tcp; }
        if (chain) { chain->rc = -2; chain->sec_status = wsa; chain->ms = ms_tcp; }
        return -2;
    }

    t0 = kn_mono_ms();
    memset(&sc, 0, sizeof sc);
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = 0;   /* STOCK defaults — that is the question */
    sc.dwFlags = 0;                 /* auto validation + SNI; never disabled */
    st = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A,
                                   SECPKG_CRED_OUTBOUND, NULL, &sc, NULL,
                                   NULL, &cred, &expiry);
    if (st != SEC_E_OK) {
        closesocket(s);
        kn_wsa_end();
        if (tls) { tls->rc = -3; tls->sec_status = (long)st; }
        if (chain) { chain->rc = -3; chain->sec_status = (long)st; }
        return -3;
    }

    for (;;) {
        SecBufferDesc obd, ibd;
        SecBuffer ob, ib[2];

        if (++loops > 64) { st = (SECURITY_STATUS)SEC_E_INTERNAL_ERROR; break; }

        memset(&ob, 0, sizeof ob);
        ob.BufferType = SECBUFFER_TOKEN;
        obd.ulVersion = SECBUFFER_VERSION;
        obd.cBuffers = 1;
        obd.pBuffers = &ob;

        ib[0].BufferType = SECBUFFER_TOKEN;
        ib[0].cbBuffer = (DWORD)rused;
        ib[0].pvBuffer = rused ? rbuf : NULL;
        ib[1].BufferType = SECBUFFER_EMPTY;
        ib[1].cbBuffer = 0;
        ib[1].pvBuffer = NULL;
        ibd.ulVersion = SECBUFFER_VERSION;
        ibd.cBuffers = 2;
        ibd.pBuffers = ib;

        st = InitializeSecurityContextA(&cred, have_ctx ? &ctx : NULL,
                (SEC_CHAR *)host,
                ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                ISC_REQ_STREAM,
                0, SECURITY_NETWORK_DREP, rused ? &ibd : NULL, 0,
                &ctx, &obd, &attrs, &expiry);
        have_ctx = 1;

        if (ob.cbBuffer && ob.pvBuffer) {
            /* send the token even on some failures (TLS alert) */
            (void)kn_send_all(s, ob.pvBuffer, (int)ob.cbBuffer);
            FreeContextBuffer(ob.pvBuffer);
        }
        if (st == SEC_E_OK) break;
        if (st != SEC_I_CONTINUE_NEEDED && st != SEC_E_INCOMPLETE_MESSAGE)
            break;                                   /* fatal (report as-is) */

        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer) {
            size_t extra = ib[1].cbBuffer;
            memmove(rbuf, rbuf + (rused - extra), extra);
            rused = extra;
        }
        if (rused >= sizeof rbuf) {
            st = (SECURITY_STATUS)SEC_E_INTERNAL_ERROR;   /* record overflow */
            break;
        }
        {
            int n = recv(s, (char *)rbuf + rused,
                         (int)(sizeof rbuf - rused), 0);
            if (n <= 0) {
                if (st == SEC_I_CONTINUE_NEEDED)
                    st = (SECURITY_STATUS)SEC_E_INCOMPLETE_MESSAGE;
                break;                               /* conn closed/timeout */
            }
            rused += (size_t)n;
        }
    }

    if (tls) {
        tls->ms = (long)(kn_mono_ms() - t0);
        tls->sec_status = (long)st;
    }
    if (chain) {
        chain->ms = (long)(kn_mono_ms() - t0);
        chain->sec_status = (long)st;
    }
    if (st != SEC_E_OK) {
        if (have_ctx) DeleteSecurityContext(&ctx);
        FreeCredentialsHandle(&cred);
        closesocket(s);
        kn_wsa_end();
        if (tls) tls->rc = -3;
        if (chain) chain->rc = -3;
        kn_logf(KN_WARN, "net.tls %s: handshake FAILED sec_status 0x%08lx "
                "(verification was ON — this is the measurement)",
                host, (unsigned long)st);
        return -3;
    }

    /* connection info: negotiated protocol/cipher */
    {
        SecPkgContext_ConnectionInfo ci;
        memset(&ci, 0, sizeof ci);
        if (QueryContextAttributesA(&ctx, SECPKG_ATTR_CONNECTION_INFO, &ci)
            == SEC_E_OK) {
            if (tls) {
                tls->protocol = (unsigned long)ci.dwProtocol;
                kn_proto_name(ci.dwProtocol, tls->protocol_name,
                              sizeof tls->protocol_name);
                tls->cipher_alg = (unsigned)ci.aiCipher;
                tls->cipher_bits = (unsigned)ci.dwCipherStrength;
                tls->hash_alg = (unsigned)ci.aiHash;
                tls->exch_alg = (unsigned)ci.aiExch;
                tls->exch_bits = (unsigned)ci.dwExchStrength;
            }
        } else if (tls) {
            tls->rc = -4;
        }
    }

    /* remote leaf certificate (+ names) */
    if (QueryContextAttributesA(&ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                                (void **)&remote) != SEC_E_OK)
        remote = NULL;
    if (tls && remote)
        kn_cert_names(remote, tls->subject, sizeof tls->subject,
                      tls->issuer, sizeof tls->issuer);

    /* chain build + policy verdict (tls.chain) */
    if (chain && remote) {
        CERT_CHAIN_PARA para;
        PCCERT_CHAIN_CONTEXT cc = NULL;
        memset(&para, 0, sizeof para);
        para.cbSize = sizeof para;
        if (CertGetCertificateChain(NULL, remote, NULL, NULL, &para, 0, NULL,
                                    &cc) && cc) {
            CERT_CHAIN_POLICY_PARA pp;
            CERT_CHAIN_POLICY_STATUS ps;
            chain->chain_trust = (unsigned long)cc->TrustStatus.dwErrorStatus;
            memset(&pp, 0, sizeof pp);
            pp.cbSize = sizeof pp;
            memset(&ps, 0, sizeof ps);
            ps.cbSize = sizeof ps;
            if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, cc,
                                                 &pp, &ps)) {
                chain->policy_error = (unsigned long)ps.dwError;
                chain->policy_chain_index = (long)ps.lChainIndex;
                chain->policy_element_index = (long)ps.lElementIndex;
            }
            if (cc->cChain >= 1 && cc->rgpChain[0]) {
                PCERT_SIMPLE_CHAIN sc2 = cc->rgpChain[0];
                DWORD i;
                for (i = 0; i < sc2->cElement &&
                            chain->depth < KN_NET_CHAIN_MAX; ++i) {
                    PCERT_CHAIN_ELEMENT el = sc2->rgpElement[i];
                    kn_net_chain_elem *e = &chain->elem[chain->depth];
                    PCCERT_CONTEXT c = el->pCertContext;
                    kn_cert_names(c, e->subject, sizeof e->subject,
                                  e->issuer, sizeof e->issuer);
                    if (c && c->pCertInfo) {
                        kn_filetime_date(&c->pCertInfo->NotBefore,
                                         e->not_before, sizeof e->not_before);
                        kn_filetime_date(&c->pCertInfo->NotAfter,
                                         e->not_after, sizeof e->not_after);
                    }
                    e->trust_error =
                        (unsigned long)el->TrustStatus.dwErrorStatus;
                    kn_logf(KN_INFO,
                            "tls.chain[%s][%d]: subject=%.60s issuer=%.60s "
                            "notAfter=%s trust=0x%08lx",
                            host, chain->depth, e->subject, e->issuer,
                            e->not_after, e->trust_error);
                    chain->depth++;
                }
            }
            CertFreeCertificateChain(cc);
        } else {
            chain->rc = -5;
        }
    }
    if (chain && chain->rc == 0 && !remote) chain->rc = -4;

    if (remote) CertFreeCertificateContext(remote);
    DeleteSecurityContext(&ctx);
    FreeCredentialsHandle(&cred);
    closesocket(s);
    kn_wsa_end();

    if (tls && tls->rc == 0) {
        kn_logf(KN_INFO, "net.tls %s: %s cipher 0x%x/%u bits, leaf '%.48s'",
                host, tls->protocol_name, tls->cipher_alg, tls->cipher_bits,
                tls->subject);
    }
    return 0;
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

#else  /* !_WIN32 — honest stub */

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

#endif /* _WIN32 */
#endif /* KN_TLS_BACKEND_MBEDTLS */

/* ------------------------------------------------- TLS stream (round XXI)
 * Honest stubs for every build WITHOUT the Win32+mbedTLS backend (the
 * real implementation lives in kn_net_mbedtls.c; the POSIX/mbedTLS build
 * gets its stubs from that file's else-section — the guards here are the
 * exact complement, so every configuration links exactly one set).
 * Callers must check kn_net_tls_stream_available() and fail loudly;
 * there is NO plaintext fallback (working agreement: never bypass TLS). */
#if !defined(KN_TLS_BACKEND_MBEDTLS)

int kn_net_tls_stream_available(void) { return 0; }

kn_net_tls_stream *kn_net_tls_stream_open(const char *host, uint16_t port,
                                          uint32_t timeout_ms,
                                          char *err, size_t errsz)
{
    (void)host; (void)port; (void)timeout_ms;
    if (err && errsz)
        snprintf(err, errsz,
                 "TLS stream needs the Win32+mbedTLS build "
                 "(KN_TLS_BACKEND=mbedtls)");
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

#endif /* !KN_TLS_BACKEND_MBEDTLS */
