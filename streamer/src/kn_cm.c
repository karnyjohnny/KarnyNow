/* kn_cm.c — CloudMatch session client (see kn_cm.h for provenance). */
#include "kn/kn_cm.h"
#include "kn/kn_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *kn_cm_phase(long long status)
{
    switch (status) {                 /* cloudmatch.rs:2698-2707 */
    case 1:  return "preparing";
    case 2:  return "ready";
    case 3:  return "streaming";
    case 4:
    case 5:  return "paused";
    case 6:  return "resuming";
    case 7:  return "finished";
    default: return status > 3 ? "failed" : "unknown";
    }
}

/* ------------------------------------------------------------------ body */

int kn_cm_build_create_body(const kn_cm_settings *s, char *out,
                            size_t outsz, size_t *out_len)
{
    int n;
    if (!s || !out || outsz < 2048 || !out_len) return KN_HTTP_ERR_ARGS;
    if (!s->device_id || !s->title) return KN_HTTP_ERR_ARGS;
    /* Field-for-field with build_create_body (:1643-1762) for the
     * H264/8-bit/no-HDR envelope: codec stays client-selected (never
     * sent), bitDepth=0 chromaFormat=0, reflex = fps>=120 (no cloud gsync
     * in bring-up), persistence false. UNVERIFIED-at-R7 fields
     * (documented): networkTestSessionId null (D-cm1), displayData null,
     * dlssOverrides null, appLaunchMode 0, userAge 25 (reference
     * constant), SubSessionId = device id (D-cm2 — the reference uses a
     * fresh random UUID; R7 measures whether the cloud cares). */
    n = snprintf(out, outsz,
        "{\"sessionRequestData\":{"
        "\"appId\":%lld,"
        "\"internalTitle\":\"%.120s\","
        "\"availableSupportedControllers\":[2],"
        "\"preferredController\":2,"
        "\"networkTestSessionId\":null,"
        "\"parentSessionId\":null,"
        "\"clientIdentification\":\"GFN-PC\","
        "\"deviceHashId\":\"%.128s\","
        "\"clientVersion\":\"30.0\","
        "\"sdkVersion\":\"2.0\","
        "\"streamerVersion\":\"14\","
        "\"clientPlatformName\":\"Windows\","
        "\"clientRequestMonitorSettings\":[{"
            "\"monitorId\":0,\"positionX\":0,\"positionY\":0,"
            "\"widthInPixels\":%u,\"heightInPixels\":%u,"
            "\"framesPerSecond\":%u,\"sdrHdrMode\":0,"
            "\"displayData\":null,\"hdr10PlusGamingData\":null,"
            "\"dpi\":96}],"
        "\"useOps\":true,"
        "\"audioMode\":2,"
        "\"metaData\":[{\"key\":\"wssignaling\",\"value\":\"1\"},"
                      "{\"key\":\"SubSessionId\",\"value\":\"%.64s\"},"
                      "{\"key\":\"surroundAudioInfo\",\"value\":\"2\"}],"
        "\"sdrHdrMode\":0,"
        "\"clientDisplayHdrCapabilities\":null,"
        "\"surroundAudioInfo\":0,"
        "\"remoteControllersBitmap\":0,"
        "\"clientTimezoneOffset\":%ld,"
        "\"enhancedStreamMode\":0,"
        "\"appLaunchMode\":0,"
        "\"secureRTSPSupported\":true,"
        "\"partnerCustomData\":null,"
        "\"accountLinked\":false,"
        "\"enablePersistingInGameSettings\":false,"
        "\"requestedAudioFormat\":0,"
        "\"userAge\":25,"
        "\"requestedStreamingFeatures\":{"
            "\"reflex\":%s,\"bitDepth\":0,\"cloudGsync\":false,"
            "\"enabledL4S\":false,\"supportedHidDevices\":0,\"profile\":0,"
            "\"fallbackToLogicalResolution\":false,\"chromaFormat\":0,"
            "\"prefilterMode\":0,\"prefilterSharpness\":0,"
            "\"prefilterNoiseReduction\":0,\"hudStreamingMode\":0,"
            "\"dlssOverrides\":null,\"mouseMovementFlags\":0,"
            "\"trueHdr\":false,\"hidDevices\":null},"
        "\"transport\":null}}",
        s->app_id, s->title, s->device_id,
        (unsigned)(s->width % 100000u), (unsigned)(s->height % 100000u),
        (unsigned)(s->fps % 100000u),
        s->device_id,
        s->timezone_offset_ms,
        s->fps >= 120u ? "true" : "false");
    if (n < 0 || (size_t)n >= outsz) return KN_HTTP_ERR_BIG;
    *out_len = (size_t)n;
    return KN_HTTP_OK;
}

int kn_cm_auth_header(const kn_cm_settings *s, char *out, size_t outsz)
{
    int n;
    if (!s || !s->token || !out || outsz < 16) return KN_HTTP_ERR_ARGS;
    n = snprintf(out, outsz, "GFNJWT %s", s->token);
    if (n < 0 || (size_t)n >= outsz) return KN_HTTP_ERR_BIG;
    return KN_HTTP_OK;
}

int kn_cm_fixed_headers(const kn_cm_settings *s, kn_http_header *h, int max)
{
    /* Values transcribed from cloudmatch_headers (:2036-2059). The
     * Authorization header is added by the CALLER (it holds the token —
     * kept out of this table so the table can never leak it wholesale). */
    int n = 0;
    if (!s || !h || max < 10) return 0;
    h[n].name = "Content-Type";       h[n].value = "text/plain"; n++;  /* quirk */
    h[n].name = "nv-client-id";       h[n].value = KN_CM_LCARS_CLIENT_ID; n++;
    h[n].name = "nv-client-streamer"; h[n].value = "NVIDIA-CLASSIC"; n++;
    h[n].name = "nv-client-type";     h[n].value = "NATIVE"; n++;
    h[n].name = "nv-client-version";  h[n].value = KN_CM_CLIENT_VERSION; n++;
    h[n].name = "nv-device-os";       h[n].value = "Windows 7"; n++;
    h[n].name = "nv-device-type";     h[n].value = "PC"; n++;
    h[n].name = "nv-device-make";     h[n].value = "Dell"; n++;
    h[n].name = "nv-device-model";    h[n].value = "Latitude E5500"; n++;
    h[n].name = "x-device-id";
    h[n].value = s->device_id ? s->device_id : ""; n++;
    return n;
}

/* -------------------------------------------------------------- extract */

static int jstr(const kn_jdoc *d, const kn_jnode *n, char *out, size_t outsz)
{
    (void)d;
    if (!n) return -1;
    return kn_json_str_copy(n, out, outsz) == KN_JSON_OK ? 0 : -1;
}

static int jint(const kn_jnode *n, long long *out)
{
    return n && kn_json_int(n, out) == KN_JSON_OK ? 0 : -1;
}

/* The rtsps-endpoint derivation (:1989-2022) for one connectionInfo
 * entry. Returns 1 when the connection is a SIGNALING one and an
 * endpoint was formed (out gets "rtsps://host:port"); ip_out receives
 * the raw ip for the serverIp fallback chain. */
static int conn_signaling(const kn_jdoc *d, const kn_jnode *conn,
                          char *out, size_t outsz, char *ip_out,
                          size_t ip_sz)
{
    long long usage = -1, alp = -1, port = -1;
    char ip[128];
    (void)jint(kn_json_get(d, conn, "usage"), &usage);
    (void)jint(kn_json_get(d, conn, "appLevelProtocol"), &alp);
    if (!(usage == 16 || (usage == 14 && (alp == 1 || alp == 6))))
        return 0;
    ip[0] = '\0';
    if (jstr(d, kn_json_get(d, conn, "ip"), ip, sizeof ip) != 0) {
        if (jstr(d, kn_json_get(d, conn, "resourcePath"), ip, sizeof ip) != 0)
            return 0;
    }
    if (ip_out && ip_sz) {              /* bounded copy (no snprintf —
                                           the destination size is a
                                           parameter, which the default
                                           -Wformat-truncation cannot
                                           prove and must not warn on) */
        size_t l = strlen(ip);
        if (l >= ip_sz) l = ip_sz - 1;
        memcpy(ip_out, ip, l);
        ip_out[l] = '\0';
    }
    if (strncmp(ip, "rtsps://", 8) == 0) {
        const char *host = ip + 8;
        if (!host[0] || !strchr(host, ':')) return 0;   /* explicit port
                                                           required (:2005) */
        snprintf(out, outsz, "%s", ip);
        return 1;
    }
    if (strpbrk(ip, "/@?#")) return 0;                  /* (:2011) */
    if (jint(kn_json_get(d, conn, "port"), &port) != 0 ||
        port < 1 || port > 65535)
        return 0;
    if (strchr(ip, ':'))                                /* IPv6 bracketed */
        snprintf(out, outsz, "rtsps://[%s]:%lld", ip, port);
    else
        snprintf(out, outsz, "rtsps://%s:%lld", ip, port);
    return 1;
}

int kn_cm_extract_session(const char *json_text, kn_cm_session *out)
{
    kn_jdoc d;
    const kn_jnode *root, *session, *conns, *n;
    int i, nconns;

    if (!json_text || !out) return KN_HTTP_ERR_ARGS;
    memset(out, 0, sizeof *out);
    if (kn_json_parse(&d, json_text, strlen(json_text), NULL) != KN_JSON_OK)
        return -1;
    root = kn_json_root(&d);

    /* requestStatus mirror (:2310-2352) — filled BEFORE the session
     * checks so a REJECTED payload (no session / no sessionId) still
     * carries the cloud-side reason for the caller's diagnostics. */
    {
        const kn_jnode *rs = kn_json_get(&d, root, "requestStatus");
        (void)jint(kn_json_get(&d, rs, "statusCode"), &out->req_status_code);
        (void)jint(kn_json_get(&d, rs, "unifiedErrorCode"),
                   &out->unified_error);
        (void)jstr(&d, kn_json_get(&d, rs, "statusDescription"),
                   out->req_status_desc, sizeof out->req_status_desc);
    }

    session = kn_json_get(&d, root, "session");
    if (!session) return -1;

    /* status/phase/errorCode are filled BEFORE the hard sessionId gate:
     * rejected payloads still carry diagnostics there — run 8's 503 body
     * was a session SKELETON (sessionControlInfo.ip:null) whose deep
     * fields a bounded excerpt cannot show (round XXVII; the reference's
     * cloudmatch_http_error likewise surfaces payload fields, :2377). */
    (void)jint(kn_json_get(&d, session, "status"), &out->status);
    out->phase = kn_cm_phase(out->status);
    (void)jint(kn_json_get(&d, session, "errorCode"), &out->error_code);

    if (jstr(&d, kn_json_get(&d, session, "sessionId"),
             out->session_id, sizeof out->session_id) != 0 ||
        !out->session_id[0])
        return -1;                       /* the hard requirement (:1953) */
    out->resumable = (out->status >= 2 && out->status <= 5);

    /* queue position: the four-source fallback, first value > 0 */
    {
        static const char *const qpaths[4][2] = {
            { "queuePosition", NULL },
            { "seatSetupInfo", "queuePosition" },
            { "sessionProgress", "queuePosition" },
            { "progressInfo", "queuePosition" }
        };
        for (i = 0; i < 4 && !out->have_queue; ++i) {
            const kn_jnode *q = kn_json_get(&d, session, qpaths[i][0]);
            long long v;
            if (qpaths[i][1]) q = kn_json_get(&d, q, qpaths[i][1]);
            if (jint(q, &v) == 0 && v > 0) {
                out->queue_position = v;
                out->have_queue = 1;
            }
        }
    }
    {   /* seatSetupInfo.seatSetupStep */
        const kn_jnode *ssi = kn_json_get(&d, session, "seatSetupInfo");
        long long v;
        if (jint(kn_json_get(&d, ssi, "seatSetupStep"), &v) == 0) {
            out->seat_setup_step = v;
            out->have_seat = 1;
        }
    }

    conns = kn_json_get(&d, session, "connectionInfo");
    nconns = kn_json_len(&d, conns);
    for (i = 0; nconns > 0 && i < nconns; ++i) {
        const kn_jnode *conn = kn_json_at(&d, conns, i);
        long long usage = -1;
        char ep[256];
        if (!conn) continue;
        if (!out->have_rtsps &&
            conn_signaling(&d, conn, ep, sizeof ep,
                           out->server_ip[0] ? NULL : out->server_ip,
                           sizeof out->server_ip)) {
            snprintf(out->rtsps_endpoint, sizeof out->rtsps_endpoint,
                     "%s", ep);
            out->have_rtsps = 1;
            /* signalingUrl = the endpoint with rtsps:// -> wss:// */
            snprintf(out->signaling_url, sizeof out->signaling_url,
                     "wss://%s", ep + 8);
        }
        /* the bundle/media connection: usage 2 or 17 (:2050, :1866) */
        (void)jint(kn_json_get(&d, conn, "usage"), &usage);
        if (!out->have_media && (usage == 2 || usage == 17)) {
            char ip[128];
            long long port = -1;
            if (jstr(&d, kn_json_get(&d, conn, "ip"), ip, sizeof ip) == 0 ||
                jstr(&d, kn_json_get(&d, conn, "resourcePath"),
                     ip, sizeof ip) == 0) {
                if (jint(kn_json_get(&d, conn, "port"), &port) == 0 &&
                    port >= 1 && port <= 65535) {
                    /* %.63s: bounded source keeps the default-level
                     * -Wformat-truncation provably silent (ip[128] ->
                     * media_ip[64]); overlong hosts truncate honestly */
                    snprintf(out->media_ip, sizeof out->media_ip,
                             "%.63s", ip);
                    out->media_port = (uint16_t)port;
                    out->have_media = 1;
                }
            }
        }
    }
    /* the control node this session lives on (:1966-1984) — the poll/stop
     * target (round XXIII, the R7 run-1 finding). Port defaults to 443
     * (:1978 is_none_or(443)); an out-of-range port degrades to 443 and
     * the trust gate in kn_cm_control_base makes the final decision. */
    {
        const kn_jnode *sci = kn_json_get(&d, session, "sessionControlInfo");
        long long cport = 443;
        if (jstr(&d, kn_json_get(&d, sci, "ip"), out->control_host,
                 sizeof out->control_host) == 0 && out->control_host[0]) {
            (void)jint(kn_json_get(&d, sci, "port"), &cport);
            if (cport < 1 || cport > 65535) cport = 443;
            out->control_port = (uint16_t)cport;
            out->have_control = 1;
        }
    }
    /* serverIp fallback: sessionControlInfo.ip (:1966) */
    if (!out->server_ip[0]) {
        const kn_jnode *sci = kn_json_get(&d, session, "sessionControlInfo");
        (void)jstr(&d, kn_json_get(&d, sci, "ip"),
                   out->server_ip, sizeof out->server_ip);
    }
    out->ready = (out->status == 2 || out->status == 3) && out->have_rtsps;
    (void)n;
    return 0;
}

/* --------------------------------------------- learned control host gate
 * Round XXIII. The C mirror of trusted_learned_server_base
 * (cloudmatch.rs:2414-2424) + trusted_cloudmatch_base (:2400-2412) +
 * public_session_host/_ipv4 (:2426-2456), bounded and allocation-free.
 * Documented simplifications vs the reference's Url/IpAddr parsing: an
 * unbracketed "ipv6:port" string is rejected outright (real captures
 * carry plain domain names — the user's 2026-10-25 evidence:
 * sessionControlInfo.ip = "np-waw-01.cloudmatchbeta.nvidiagrid.net"),
 * IPv6 scope IDs are rejected, and an IPv6 address with an embedded
 * IPv4 tail has that tail checked against the IPv4 rules. Every reject
 * just means "fall back to the create host" — the safe direction. */

static int cm_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* strict dotted-quad public check (public_session_ipv4, :2443-2456) */
static int cm_ipv4_public(const char *h)
{
    unsigned o[4];
    int i;
    const char *p = h;
    for (i = 0; i < 4; ++i) {
        const char *start = p;
        long v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
        if (p == start || v > 255) return 0;
        if (p - start > 1 && *start == '0') return 0;   /* no leading zeros */
        o[i] = (unsigned)v;
        if (i < 3) {
            if (*p != '.') return 0;
            ++p;
        }
    }
    if (*p) return 0;
    if (o[0] == 0 || o[0] >= 240) return 0;         /* 0/8 + reserved (:2450) */
    if (o[0] == 10) return 0;                       /* private */
    if (o[0] == 127) return 0;                      /* loopback */
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return 0;  /* private */
    if (o[0] == 192 && o[1] == 168) return 0;       /* private */
    if (o[0] == 169 && o[1] == 254) return 0;       /* link-local */
    if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) return 0; /* CGNAT (:2454) */
    if (o[0] >= 224) return 0;                      /* multicast (240+ gone) */
    return 1;
}

static int cm_is_ipv4_text(const char *h)
{
    if (!strchr(h, '.')) return 0;
    for (; *h; ++h)
        if (!((*h >= '0' && *h <= '9') || *h == '.')) return 0;
    return 1;
}

/* IPv6 text check: charset + the non-public prefix classes of
 * public_session_host (:2429-2441). s may carry an embedded IPv4 tail.
 * Documented simplification vs the reference's IpAddr parse: reserved
 * forms are caught by exact/prefix comparison ("::", "::1", ff/fc/fd/
 * fe8-feb heads), not by a full address decode. */
static int cm_ipv6_public(const char *s, size_t n)
{
    size_t i;
    int colons = 0;
    char head[3];
    if (n == 0 || n > 46) return 0;
    for (i = 0; i < n; ++i) {
        char c = s[i];
        if (c == ':') { ++colons; continue; }
        if (c == '.') continue;               /* embedded IPv4 tail */
        if (cm_hexval(c) < 0) return 0;
    }
    if (colons < 2) return 0;
    /* embedded IPv4 tail (e.g. "::ffff:127.0.0.1"): the dotted quad
     * after the last ':' must itself be public */
    if (memchr(s, '.', n)) {
        const char *v4 = s + n;
        while (v4 > s && *v4 != ':') --v4;
        if (*v4 == ':') ++v4;
        if (!v4[0] || !cm_ipv4_public(v4)) return 0;
    }
    /* unspecified + loopback */
    if ((n == 2 && s[0] == ':' && s[1] == ':') ||
        (n == 3 && s[0] == ':' && s[1] == ':' && s[2] == '1'))
        return 0;
    /* first-hextet prefix classes: ff00::/8 multicast, fc00::/7 ULA,
     * fe80::/10 link-local */
    head[0] = head[1] = head[2] = '\0';
    for (i = 0; i < n && s[i] != ':' && i < 2; ++i)
        head[i] = (char)(s[i] | 0x20);
    if (head[0] == 'f') {
        if (head[1] == 'f' || head[1] == 'c' || head[1] == 'd')
            return 0;
        if (head[1] == 'e') {
            char c3 = (i < n && s[i] != ':') ? (char)(s[i] | 0x20) : '\0';
            if (c3 >= '8' && c3 <= 'b') return 0;     /* fe8x-febx */
        }
    }
    return 1;
}

/* domain shape: labels of [A-Za-z0-9-] (no edge hyphens), >= 1 dot, no
 * ".local" suffix (:2427-2429), total <= 253 */
static int cm_domain_public(const char *h, size_t len)
{
    size_t i, lab = 0;
    int dots = 0;
    if (len == 0 || len > 253) return 0;
    for (i = 0; i <= len; ++i) {
        char c = (i < len) ? h[i] : '.';
        if (c == '.') {
            if (lab == 0) return 0;                   /* empty/edge label */
            if (h[i - lab] == '-' || h[i - 1] == '-') return 0;
            if (i < len) ++dots;
            lab = 0;
        } else {
            ++lab;
        }
    }
    if (dots == 0) return 0;                          /* no TLD (:2428) */
    if (len >= 6) {                                   /* ".local" suffix */
        const char *t = h + len - 6;
        if (t[0] == '.' &&
            (t[1] | 0x20) == 'l' && (t[2] | 0x20) == 'o' &&
            (t[3] | 0x20) == 'c' && (t[4] | 0x20) == 'a' &&
            (t[5] | 0x20) == 'l')
            return 0;
    }
    return 1;
}

int kn_cm_control_base(const kn_cm_session *s, char *host_out, size_t hostsz,
                       uint16_t *port_out)
{
    const char *p;
    size_t len, i;
    if (!s || !s->have_control || !host_out || hostsz < 2 || !port_out)
        return 0;
    if (s->control_port != 443) return 0;             /* (:1976-1980) */
    p = s->control_host;
    if (strncmp(p, "https://", 8) == 0) p += 8;       /* (:2415-2416) */
    else if (strstr(p, "://")) return 0;              /* any other scheme */
    if (!p[0]) return 0;
    len = strlen(p);
    if (len + 1 > hostsz) return 0;
    /* charset whitelist — also rejects the userinfo/query/fragment
     * carriers '/', '@', '?', '#' and whitespace (:2403-2409) */
    for (i = 0; i < len; ++i) {
        char c = p[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                 c == ':' || c == '[' || c == ']';
        if (!ok) return 0;
    }
    if (p[0] == '[') {                                /* bracketed IPv6 */
        const char *close = strchr(p, ']');
        if (!close || close[1] != '\0' || close == p + 1) return 0;
        if (!cm_ipv6_public(p + 1, (size_t)(close - p - 1))) return 0;
    } else if (strchr(p, ':')) {                      /* bare IPv6 text */
        if (!cm_ipv6_public(p, len)) return 0;
    } else if (cm_is_ipv4_text(p)) {
        if (!cm_ipv4_public(p)) return 0;
    } else if (!cm_domain_public(p, len)) {
        return 0;
    }
    memcpy(host_out, p, len);
    host_out[len] = '\0';
    *port_out = 443;
    return 1;
}

/* --------------------------------------------------------- network ops */

static int cm_round(kn_http_io io, const char *method, const char *host,
                    const char *path, const kn_cm_settings *s,
                    const char *body, size_t body_len, uint32_t timeout_ms,
                    uint64_t (*now_ms)(void *), void *now_ctx,
                    uint8_t *rxb, size_t rxbsz, char *jsonbuf,
                    size_t jsoncap, int *http_status, kn_cm_session *out)
{
    kn_http_header h[12];
    kn_http_response resp;
    char auth[2048];
    int nh, rc;

    if (kn_cm_auth_header(s, auth, sizeof auth) != KN_HTTP_OK)
        return KN_HTTP_ERR_ARGS;
    nh = kn_cm_fixed_headers(s, h, 10);
    h[nh].name = "Authorization";
    h[nh].value = auth;
    nh++;
    rc = kn_http_request(io, method, path, host, h, nh, body, body_len,
                         timeout_ms, now_ms, now_ctx, rxb, rxbsz,
                         jsonbuf, jsoncap, &resp);
    /* the token never outlives this frame: scrub the local copy */
    memset(auth, 0, sizeof auth);
    if (rc != KN_HTTP_OK) return rc;
    if (http_status) *http_status = resp.status;
    if (resp.status != 200) return KN_HTTP_OK;    /* caller inspects */
    if (!out) return KN_HTTP_OK;   /* DELETE: the body is not inspected */
    if (resp.truncated) return KN_HTTP_ERR_BIG;
    return kn_cm_extract_session(jsonbuf, out) == 0 ? KN_HTTP_OK
                                                    : KN_HTTP_ERR_PROTO;
}

int kn_cm_create(kn_http_io io, const char *host, const char *base_path,
                 const kn_cm_settings *s, uint32_t timeout_ms,
                 uint64_t (*now_ms)(void *), void *now_ctx,
                 uint8_t *rxb, size_t rxbsz, char *jsonbuf, size_t jsoncap,
                 int *http_status, kn_cm_session *out)
{
    char path[256], body[8192];
    size_t blen = 0;
    int n;
    if (!host || !s || !out) return KN_HTTP_ERR_ARGS;
    if (kn_cm_build_create_body(s, body, sizeof body, &blen) != KN_HTTP_OK)
        return KN_HTTP_ERR_ARGS;
    n = snprintf(path, sizeof path, "%sv2/session",
                 base_path ? base_path : "/");
    if (n < 0 || (size_t)n >= sizeof path) return KN_HTTP_ERR_ARGS;
    return cm_round(io, "POST", host, path, s, body, blen, timeout_ms,
                    now_ms, now_ctx, rxb, rxbsz, jsonbuf, jsoncap,
                    http_status, out);
}

int kn_cm_poll(kn_http_io io, const char *host, const char *base_path,
               const kn_cm_settings *s, const char *session_id,
               uint32_t timeout_ms,
               uint64_t (*now_ms)(void *), void *now_ctx,
               uint8_t *rxb, size_t rxbsz, char *jsonbuf, size_t jsoncap,
               int *http_status, kn_cm_session *out)
{
    char path[320];
    int n;
    if (!host || !s || !session_id || !out) return KN_HTTP_ERR_ARGS;
    n = snprintf(path, sizeof path, "%sv2/session/%.64s",
                 base_path ? base_path : "/", session_id);
    if (n < 0 || (size_t)n >= sizeof path) return KN_HTTP_ERR_ARGS;
    return cm_round(io, "GET", host, path, s, NULL, 0, timeout_ms,
                    now_ms, now_ctx, rxb, rxbsz, jsonbuf, jsoncap,
                    http_status, out);
}

int kn_cm_cancel(kn_http_io io, const char *host, const char *base_path,
                 const kn_cm_settings *s, const char *session_id,
                 uint32_t timeout_ms,
                 uint64_t (*now_ms)(void *), void *now_ctx,
                 uint8_t *rxb, size_t rxbsz, char *jsonbuf, size_t jsoncap,
                 int *http_status)
{
    char path[320];
    int n;
    if (!host || !s || !session_id || !session_id[0]) return KN_HTTP_ERR_ARGS;
    n = snprintf(path, sizeof path, "%sv2/session/%.64s",
                 base_path ? base_path : "/", session_id);
    if (n < 0 || (size_t)n >= sizeof path) return KN_HTTP_ERR_ARGS;
    return cm_round(io, "DELETE", host, path, s, NULL, 0, timeout_ms,
                    now_ms, now_ctx, rxb, rxbsz, jsonbuf, jsoncap,
                    http_status, NULL);
}
