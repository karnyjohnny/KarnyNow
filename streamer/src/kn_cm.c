/* kn_cm.c — CloudMatch session client (see kn_cm.h for provenance). */
#include "kn/kn_cm.h"
#include "kn/kn_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>      /* F35: traceparent seeding */

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
    if (!s->device_id) return KN_HTTP_ERR_ARGS;

    if (s->profile == 1) {
        /* F35 (round N slice 4n): the BROWSER-PARITY body — a
         * field-for-field, key-order-for-key-order mirror of the
         * play.geforcenow.com 2.0.89.154 POST /v2/session captured by
         * the operator on 2026-11-01 (tools/wiretap, RTSP §57.2).
         * Differences from legacy: internalTitle null, controllers [],
         * platform "browser", sdk "1.0", streamerVersion the NUMBER 1,
         * secureRTSPSupported false, enhancedStreamMode/appLaunchMode 1,
         * partnerCustomData "", 8 metaData entries (fresh-UUID
         * SubSessionId, clientTimezone, GSStreamerType=WebRTC,
         * networkType, ClientImeSupport, clientPhysicalResolution),
         * monitor settings without monitorId/position/dpi/hdr10Plus,
         * displayData luminance zeros, profile 1 + dlssOverrides object
         * in requestedStreamingFeatures, and NO preferredController /
         * requestedAudioFormat / transport / mouseMovementFlags /
         * trueHdr / hidDevices keys. */
        if (!s->subsession_id || !s->tz_name) return KN_HTTP_ERR_ARGS;
        n = snprintf(out, outsz,
            "{\"sessionRequestData\":{"
            "\"appId\":%lld,"
            "\"audioMode\":2,"
            "\"remoteControllersBitmap\":0,"
            "\"sdrHdrMode\":0,"
            "\"networkTestSessionId\":null,"
            "\"availableSupportedControllers\":[],"
            "\"clientVersion\":\"30.0\","
            "\"deviceHashId\":\"%.64s\","
            "\"internalTitle\":null,"
            "\"clientPlatformName\":\"browser\","
            "\"metaData\":["
                "{\"key\":\"SubSessionId\",\"value\":\"%.64s\"},"
                "{\"key\":\"wssignaling\",\"value\":\"1\"},"
                "{\"key\":\"clientTimezone\",\"value\":\"%.64s\"},"
                "{\"key\":\"GSStreamerType\",\"value\":\"WebRTC\"},"
                "{\"key\":\"networkType\",\"value\":\"Unknown\"},"
                "{\"key\":\"ClientImeSupport\",\"value\":\"0\"},"
                "{\"key\":\"clientPhysicalResolution\","
                 "\"value\":\"{\\\"horizontalPixels\\\":%u,"
                             "\\\"verticalPixels\\\":%u}\"},"
                "{\"key\":\"surroundAudioInfo\",\"value\":\"2\"}],"
            "\"surroundAudioInfo\":0,"
            "\"clientTimezoneOffset\":%ld,"
            "\"clientIdentification\":\"GFN-PC\","
            "\"parentSessionId\":null,"
            "\"streamerVersion\":1,"
            "\"clientRequestMonitorSettings\":[{"
                "\"widthInPixels\":%u,\"heightInPixels\":%u,"
                "\"framesPerSecond\":%u,\"sdrHdrMode\":0,"
                "\"displayData\":{"
                    "\"desiredContentMaxLuminance\":0,"
                    "\"desiredContentMinLuminance\":0,"
                    "\"desiredContentMaxFrameAverageLuminance\":0},"
                "\"dpi\":0}],"
            "\"appLaunchMode\":1,"
            "\"sdkVersion\":\"1.0\","
            "\"enhancedStreamMode\":1,"
            "\"useOps\":true,"
            "\"clientDisplayHdrCapabilities\":null,"
            "\"accountLinked\":false,"
            "\"partnerCustomData\":\"\","
            "\"enablePersistingInGameSettings\":false,"
            "\"secureRTSPSupported\":false,"
            "\"userAge\":%d,"
            "\"requestedStreamingFeatures\":{"
                "\"reflex\":%s,\"bitDepth\":0,\"cloudGsync\":false,"
                "\"enabledL4S\":false,\"profile\":1,"
                "\"fallbackToLogicalResolution\":false,"
                "\"chromaFormat\":0,\"prefilterMode\":0,"
                "\"hudStreamingMode\":0,"
                "\"dlssOverrides\":{\"srMode\":0,\"srModel\":0,"
                    "\"srScale\":100,\"rrModel\":0,\"fgMode\":0,"
                    "\"fgModel\":0}}}}",
            s->app_id,
            s->device_id,
            s->subsession_id,
            s->tz_name,
            (unsigned)(s->width % 100000u), (unsigned)(s->height % 100000u),
            s->timezone_offset_ms,
            (unsigned)(s->width % 100000u), (unsigned)(s->height % 100000u),
            (unsigned)(s->fps % 100000u),
            s->user_age > 0 ? s->user_age : 25,
            s->fps >= 120u ? "true" : "false");
        if (n < 0 || (size_t)n >= outsz) return KN_HTTP_ERR_BIG;
        *out_len = (size_t)n;
        return KN_HTTP_OK;
    }

    if (!s->title) return KN_HTTP_ERR_ARGS;
    /* F37 (RTSP §59.3): OpenNOW-strict NATIVE body — field-for-field
     * with build_create_body (:1643-1762 @ 1cb3479) for the H264/8-bit/
     * no-HDR envelope: codec stays client-selected (never sent),
     * bitDepth=0 chromaFormat=0, reflex = fps>=120 (no cloud gsync in
     * bring-up), persistence false. This is the identity that
     * provisions the RTSP seat (secureRTSPSupported true +
     * streamerVersion "14" + NO GSStreamerType metadata — the server
     * answers with connectionInfo usage 16 or usage 14/alp 1|6 on
     * :322; the browser profile's WebRTC seat is unstreamable here).
     * F37 deltas vs the legacy chimera body: SubSessionId is the fresh
     * per-draw UUID when the caller supplies one (random_uuid :1675;
     * D-cm2's device-id reuse was NOT the reference — it stays only as
     * the NULL fallback), appLaunchMode is 1 ("default", :2979-2985 —
     * the legacy 0 matched no real client) and dlssOverrides is the
     * dlss.rs default_overrides object (:22-24; the legacy null was the
     * browser-diff's tell). Still UNVERIFIED-at-R7: networkTestSessionId
     * null (D-cm1 — the QT front end never sets it, so null IS the
     * reference value; kept), displayData null (:1602-1604 non-HDR),
     * userAge 25 (:1740 constant). */
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
        "\"appLaunchMode\":1,"
        "\"secureRTSPSupported\":true,"
        "\"partnerCustomData\":null,"
        "\"accountLinked\":false,"
        "\"enablePersistingInGameSettings\":false,"
        "\"requestedAudioFormat\":0,"
        "\"userAge\":%d,"
        "\"requestedStreamingFeatures\":{"
            "\"reflex\":%s,\"bitDepth\":0,\"cloudGsync\":false,"
            "\"enabledL4S\":false,\"supportedHidDevices\":0,\"profile\":0,"
            "\"fallbackToLogicalResolution\":false,\"chromaFormat\":0,"
            "\"prefilterMode\":0,\"prefilterSharpness\":0,"
            "\"prefilterNoiseReduction\":0,\"hudStreamingMode\":0,"
            "\"dlssOverrides\":{\"srMode\":0,\"srModel\":0,\"srScale\":100,"
                "\"rrModel\":0,\"fgMode\":0,\"fgModel\":0},"
            "\"mouseMovementFlags\":0,"
            "\"trueHdr\":false,\"hidDevices\":null},"
        "\"transport\":null}}",
        s->app_id, s->title, s->device_id,
        (unsigned)(s->width % 100000u), (unsigned)(s->height % 100000u),
        (unsigned)(s->fps % 100000u),
        (s->subsession_id && s->subsession_id[0]) ? s->subsession_id
                                                  : s->device_id,
        s->timezone_offset_ms,
        s->user_age > 0 ? s->user_age : 25,
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

/* F35: W3C traceparent randomness without pulling in a RNG dependency —
 * splitmix64 over a time-seeded state, three draws per header
 * (00-<32 hex>-<16 hex>-01). NOT cryptographic; it only has to look
 * fresh per request, exactly like the browser's tracing SDK output. */
static unsigned long long cm_tp_next(void)
{
    static unsigned long long state = 0;
    unsigned long long z;
    if (!state)
        state = ((unsigned long long)time(NULL) << 21) ^
                0x9E3779B97F4A7C15ull;
    state += 0x9E3779B97F4A7C15ull;
    z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int kn_cm_fixed_headers(const kn_cm_settings *s, kn_http_header *h, int max)
{
    /* Values transcribed from cloudmatch_headers (:2036-2059). The
     * Authorization header is added by the CALLER (it holds the token —
     * kept out of this table so the table can never leak it wholesale). */
    int n = 0;
    static char tp[64];        /* traceparent buffer (single-threaded
                                  handshake; static keeps the table's
                                  const-char* contract) */
    if (!s || !h) return 0;

    if (s->profile == 1) {
        /* F35 (round N slice 4n): the BROWSER header set, transcribed
         * from the operator's 2026-11-01 wiretap of play.geforcenow.com
         * (RTSP §57.2): BROWSER/WEBRTC/CHROME, WINDOWS/DESKTOP with
         * UNKNOWN make/model (the web client knows no hardware),
         * client version 2.0.89.154, application/json (NOT the legacy
         * text/plain quirk), Chrome-109 UA + client hints, play.
         * geforcenow.com origin, sec-fetch-*, pl-PL accept-language and
         * a fresh W3C traceparent. Deliberately OMITTED: accept-encoding
         * (our HTTP reader has no gzip decoder — an honest mismatch,
         * documented) and HTTP/2 (we speak HTTP/1.1; residual). */
        unsigned long long t1 = cm_tp_next(), t2 = cm_tp_next(),
                           t3 = cm_tp_next();
        if (max < 22) return 0;
        snprintf(tp, sizeof tp,
                 "00-%08lx%08lx%08lx%08lx-%08lx%08lx-01",
                 (unsigned long)(t1 >> 32), (unsigned long)(t1 & 0xFFFFFFFFul),
                 (unsigned long)(t2 >> 32), (unsigned long)(t2 & 0xFFFFFFFFul),
                 (unsigned long)(t3 >> 32), (unsigned long)(t3 & 0xFFFFFFFFul));
        h[n].name = "sec-ch-ua";
        h[n].value = "\"Not_A Brand\";v=\"99\", \"Google Chrome\";v=\"109\", "
                     "\"Chromium\";v=\"109\""; n++;
        h[n].name = "nv-device-model";    h[n].value = "UNKNOWN"; n++;
        h[n].name = "nv-browser-type";    h[n].value = "CHROME"; n++;
        h[n].name = "nv-device-os";       h[n].value = "WINDOWS"; n++;
        h[n].name = "nv-client-type";     h[n].value = "BROWSER"; n++;
        h[n].name = "nv-device-make";     h[n].value = "UNKNOWN"; n++;
        h[n].name = "sec-ch-ua-platform"; h[n].value = "\"Windows\""; n++;
        h[n].name = "nv-client-streamer"; h[n].value = "WEBRTC"; n++;
        h[n].name = "nv-client-id";       h[n].value = KN_CM_LCARS_CLIENT_ID; n++;
        h[n].name = "traceparent";        h[n].value = tp; n++;
        h[n].name = "sec-ch-ua-mobile";   h[n].value = "?0"; n++;
        h[n].name = "user-agent";
        h[n].value = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/109.0.0.0 Safari/537.36"; n++;
        h[n].name = "content-type";       h[n].value = "application/json"; n++;
        h[n].name = "nv-device-type";     h[n].value = "DESKTOP"; n++;
        h[n].name = "nv-client-version";  h[n].value = KN_CM_CLIENT_VERSION_BROWSER; n++;
        h[n].name = "x-device-id";
        h[n].value = s->device_id ? s->device_id : ""; n++;
        h[n].name = "accept";             h[n].value = "*/*"; n++;
        h[n].name = "origin";             h[n].value = "https://play.geforcenow.com"; n++;
        h[n].name = "sec-fetch-site";     h[n].value = "cross-site"; n++;
        h[n].name = "sec-fetch-mode";     h[n].value = "cors"; n++;
        h[n].name = "sec-fetch-dest";     h[n].value = "empty"; n++;
        h[n].name = "accept-language";
        h[n].value = "pl-PL,pl;q=0.9,en-US;q=0.8,en;q=0.7"; n++;
        return n;
    }

    /* F37 (RTSP §59.3): the NATIVE table, byte-for-byte from
     * cloudmatch_headers (:2263-2287 @ 1cb3479): the Bifrost UA FIRST,
     * the text/plain JSON quirk, LCARS id, NVIDIA-CLASSIC/NATIVE,
     * 2.0.87.131, the device quartet WINDOWS/DESKTOP/UNKNOWN/UNKNOWN
     * (the legacy "Windows 7"/PC/Dell/"Latitude E5500" chimera described
     * a machine no GFN client reports — OpenNOW's device_*() are
     * compile-time constants), x-device-id and x-nv-client-identity
     * (= the UA again). Authorization is appended by cm_round, landing
     * last instead of second (order after the first line is not
     * semantic in HTTP; disclosed residual, same as F35's). */
    if (s->profile == 2) {
        /* F41 (round N slice 4r): the RUN-46 identity, restored
         * byte-for-byte. This is the header table that flew EVERY long
         * session in project history — run 46 draw b's 1020.0 s keeper
         * (`stop_reason:"time"`, our own timer ended a healthy session),
         * draw a's 1174.8 s, draw 46's 1084.9 s, and runs 60/61's two
         * 1200 s keepers. F37 (slice 4o) replaced it with OpenNOW's
         * compile-time device constants and the ceiling collapsed:
         * run 102 maxed at 317.6 s, run 103 at 21.0 s, run 104 at
         * 15.9 s — 0/8 draws over 21 s on the F37 identity versus
         * 1020-1200 s on this one.
         * The deltas F37 introduced, all reverted here: NO user-agent
         * header at all, NO x-nv-client-identity, and the device quartet
         * is "Windows 7"/PC/Dell/"Latitude E5500" instead of
         * WINDOWS/DESKTOP/UNKNOWN/UNKNOWN. F37's own comment calls the
         * legacy quartet "a chimera describing a machine no GFN client
         * reports" — and the chimera is what the cloud kept alive for
         * twenty minutes. Content-Type keeps the text/plain quirk, and
         * nv-client-version stays 2.0.87.131 (identical in both), so
         * this profile isolates the device/UA delta and nothing else.
         * The BODY is unchanged (still F37's): a header-only A/B. If it
         * does not bring the long flights back, the residual suspects
         * are the body's F37 additions (srMode/srModel/srScale/rrModel/
         * fgMode/fgModel, the dlssOverrides default object, appLaunchMode
         * and the per-flight fresh SubSessionId) — RTSP §62. */
        if (max < 10) return 0;
        h[n].name = "Content-Type";       h[n].value = "text/plain"; n++;
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

    if (max < 12) return 0;
    h[n].name = "user-agent";         h[n].value = KN_CM_BIFROST_UA; n++;
    h[n].name = "Content-Type";       h[n].value = "text/plain"; n++;  /* quirk */
    h[n].name = "nv-client-id";       h[n].value = KN_CM_LCARS_CLIENT_ID; n++;
    h[n].name = "nv-client-streamer"; h[n].value = "NVIDIA-CLASSIC"; n++;
    h[n].name = "nv-client-type";     h[n].value = "NATIVE"; n++;
    h[n].name = "nv-client-version";  h[n].value = KN_CM_CLIENT_VERSION; n++;
    h[n].name = "nv-device-os";       h[n].value = "WINDOWS"; n++;
    h[n].name = "nv-device-type";     h[n].value = "DESKTOP"; n++;
    h[n].name = "nv-device-make";     h[n].value = "UNKNOWN"; n++;
    h[n].name = "nv-device-model";    h[n].value = "UNKNOWN"; n++;
    h[n].name = "x-device-id";
    h[n].value = s->device_id ? s->device_id : ""; n++;
    h[n].name = "x-nv-client-identity"; h[n].value = KN_CM_BIFROST_UA; n++;
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
    {   /* F36: the free-tier ad gate (run-100). sessionAds is the FIRST
         * key of the session object in every queued poll of a browser-
         * identity session; sessionAdsRequired rides alongside it. Only
         * the first ad entry is tracked — the reference plays ads[0]
         * (QueueAdPlayback.qml) and a fresh offer arrives as a new adId
         * on a later poll. */
        const kn_jnode *ads = kn_json_get(&d, session, "sessionAds");
        int b;
        if (kn_json_bool(kn_json_get(&d, session, "sessionAdsRequired"),
                         &b) == KN_JSON_OK)
            out->ads_required = b;
        if (ads && kn_json_len(&d, ads) > 0) {
            const kn_jnode *a0 = kn_json_at(&d, ads, 0);
            if (a0 &&
                jstr(&d, kn_json_get(&d, a0, "adId"),
                     out->ad_id, sizeof out->ad_id) == 0 &&
                out->ad_id[0]) {
                out->have_ad = 1;
                (void)jint(kn_json_get(&d, a0, "adState"), &out->ad_state);
                (void)jint(kn_json_get(&d, a0, "adLengthInSeconds"),
                           &out->ad_len_s);
                (void)jstr(&d, kn_json_get(&d, a0, "adUrl"),
                           out->ad_url, sizeof out->ad_url);
            }
        }
    }

    conns = kn_json_get(&d, session, "connectionInfo");
    nconns = kn_json_len(&d, conns);
    out->conn_count = nconns > 0 ? nconns : 0;
    out->conn0_usage = out->conn0_alp = out->conn0_port = -1;
    for (i = 0; nconns > 0 && i < nconns; ++i) {
        const kn_jnode *conn = kn_json_at(&d, conns, i);
        long long usage = -1;
        char ep[256];
        if (!conn) continue;
        if (i == 0) {           /* F37: the unusable-seat witness (§59.2) */
            (void)jint(kn_json_get(&d, conn, "usage"), &out->conn0_usage);
            (void)jint(kn_json_get(&d, conn, "appLevelProtocol"),
                       &out->conn0_alp);
            (void)jint(kn_json_get(&d, conn, "port"), &out->conn0_port);
            (void)jstr(&d, kn_json_get(&d, conn, "resourcePath"),
                       out->conn0_path, sizeof out->conn0_path);
        }
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

/* F38: the trusted-host gate extracted verbatim from
 * kn_cm_control_base — scheme strip (:2415-2416), charset whitelist
 * (:2403-2409), then the IPv6/IPv4/domain public checks. Returns 1 and
 * copies the normalized host on success. Both control_base and
 * parse_conflict share it so a conflict host can never be less trusted
 * than a polled one. */
static int cm_host_trusted(const char *raw, char *out, size_t outsz)
{
    const char *p;
    size_t len, i;
    if (!raw || !out || outsz < 2) return 0;
    p = raw;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strstr(p, "://")) return 0;
    if (!p[0]) return 0;
    len = strlen(p);
    if (len + 1 > outsz) return 0;
    for (i = 0; i < len; ++i) {
        char c = p[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                 c == ':' || c == '[' || c == ']';
        if (!ok) return 0;
    }
    if (p[0] == '[') {
        const char *close = strchr(p, ']');
        if (!close || close[1] != '\0' || close == p + 1) return 0;
        if (!cm_ipv6_public(p + 1, (size_t)(close - p - 1))) return 0;
    } else if (strchr(p, ':')) {
        if (!cm_ipv6_public(p, len)) return 0;
    } else if (cm_is_ipv4_text(p)) {
        if (!cm_ipv4_public(p)) return 0;
    } else if (!cm_domain_public(p, len)) {
        return 0;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

int kn_cm_control_base(const kn_cm_session *s, char *host_out, size_t hostsz,
                       uint16_t *port_out)
{
    if (!s || !s->have_control || !host_out || hostsz < 2 || !port_out)
        return 0;
    if (s->control_port != 443) return 0;             /* (:1976-1980) */
    if (!cm_host_trusted(s->control_host, host_out, hostsz)) return 0;
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
    kn_http_header h[24];      /* F35: the browser table needs 22 + auth */
    kn_http_response resp;
    char auth[2048];
    int nh, rc;

    if (kn_cm_auth_header(s, auth, sizeof auth) != KN_HTTP_OK)
        return KN_HTTP_ERR_ARGS;
    nh = kn_cm_fixed_headers(s, h, 22);
    if (nh == 0) return KN_HTTP_ERR_ARGS;
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

/* F36: the ad-report body — key order transcribed from the browser's own
 * IMA PUT in the R2 capture (gfn_ref_h2.json, stream 15):
 * adId, adAction, clientTimestamp, [cancelReason], watchedTimeInMs,
 * pausedTimeInMs, adDurationInMs. The outer envelope is the
 * reference's {"action":6,"adUpdates":[{...}]} (cloudmatch.rs
 * report_ad). clientTimestamp is UNIX SECONDS (capture: 1793523113). */
int kn_cm_build_ad_body(const char *ad_id, int ad_action,
                        long long client_ts, long long watched_ms,
                        long long paused_ms, long long duration_ms,
                        const char *cancel_reason,
                        char *out, size_t outsz, size_t *out_len)
{
    int n;
    if (!ad_id || !ad_id[0] || !out || !out_len) return KN_HTTP_ERR_ARGS;
    if (ad_action < KN_CM_AD_START || ad_action > KN_CM_AD_CANCEL)
        return KN_HTTP_ERR_ARGS;
    if (watched_ms < 0) watched_ms = 0;
    if (paused_ms < 0) paused_ms = 0;
    if (duration_ms < 0) duration_ms = 0;
    if (cancel_reason && cancel_reason[0])
        n = snprintf(out, outsz,
            "{\"action\":6,\"adUpdates\":[{"
            "\"adId\":\"%.64s\",\"adAction\":%d,\"clientTimestamp\":%lld,"
            "\"cancelReason\":\"%.64s\",\"watchedTimeInMs\":%lld,"
            "\"pausedTimeInMs\":%lld,\"adDurationInMs\":%lld}]}",
            ad_id, ad_action, client_ts, cancel_reason,
            watched_ms, paused_ms, duration_ms);
    else
        n = snprintf(out, outsz,
            "{\"action\":6,\"adUpdates\":[{"
            "\"adId\":\"%.64s\",\"adAction\":%d,\"clientTimestamp\":%lld,"
            "\"watchedTimeInMs\":%lld,\"pausedTimeInMs\":%lld,"
            "\"adDurationInMs\":%lld}]}",
            ad_id, ad_action, client_ts,
            watched_ms, paused_ms, duration_ms);
    if (n < 0 || (size_t)n >= outsz) return KN_HTTP_ERR_BIG;
    *out_len = (size_t)n;
    return KN_HTTP_OK;
}

int kn_cm_report_ad(kn_http_io io, const char *host, const char *base_path,
                    const kn_cm_settings *s, const char *session_id,
                    const char *ad_id,
                    int ad_action, long long watched_ms, long long paused_ms,
                    long long duration_ms, const char *cancel_reason,
                    uint32_t timeout_ms,
                    uint64_t (*now_ms)(void *), void *now_ctx,
                    uint8_t *rxb, size_t rxbsz, char *jsonbuf,
                    size_t jsoncap, int *http_status, kn_cm_session *out)
{
    char path[320], body[512];
    size_t blen = 0;
    int n;
    if (!host || !s || !session_id || !session_id[0] || !ad_id || !ad_id[0])
        return KN_HTTP_ERR_ARGS;
    if (kn_cm_build_ad_body(ad_id, ad_action, (long long)time(NULL),
                            watched_ms, paused_ms, duration_ms,
                            cancel_reason, body, sizeof body,
                            &blen) != KN_HTTP_OK)
        return KN_HTTP_ERR_ARGS;
    n = snprintf(path, sizeof path, "%sv2/session/%.64s",
                 base_path ? base_path : "/", session_id);
    if (n < 0 || (size_t)n >= sizeof path) return KN_HTTP_ERR_ARGS;
    return cm_round(io, "PUT", host, path, s, body, blen, timeout_ms,
                    now_ms, now_ctx, rxb, rxbsz, jsonbuf, jsoncap,
                    http_status, out);
}

/* ------------------------------------------------------- F38: resume */

int kn_cm_parse_conflict(const char *json, kn_cm_conflict *out)
{
    kn_jdoc d;
    const kn_jnode *root, *src[2];
    int c, i;

    if (!json || !out) return KN_HTTP_ERR_ARGS;
    memset(out, 0, sizeof *out);
    out->control_port = 443;
    out->echo_app_launch_mode = -1;
    out->echo_persist = -1;
    if (kn_json_parse(&d, json, strlen(json), NULL) != KN_JSON_OK) return -1;
    root = kn_json_root(&d);
    /* the reference's candidate chain (:889-895): otherUserSessions
     * first, then the payload's own session object. */
    src[0] = kn_json_get(&d, root, "otherUserSessions");
    src[1] = kn_json_get(&d, root, "session");
    for (c = 0; c < 2; ++c) {
        int cnt = 0;
        if (!src[c]) continue;
        cnt = (c == 0) ? kn_json_len(&d, src[c]) : 1;
        for (i = 0; i < cnt; ++i) {
            const kn_jnode *sn =
                (c == 0) ? kn_json_at(&d, src[c], i) : src[c];
            const kn_jnode *srd, *sci;
            long long status = -1, appid = 0, cport = 443;
            char sid[64], host[256];
            int b;
            if (!sn) continue;
            if (jstr(&d, kn_json_get(&d, sn, "sessionId"),
                     sid, sizeof sid) != 0 || !sid[0])
                continue;
            (void)jint(kn_json_get(&d, sn, "status"), &status);
            if (status < 2 || status > 5) continue;   /* :1506-1515 */
            srd = kn_json_get(&d, sn, "sessionRequestData");
            (void)jint(kn_json_get(&d, srd, "appId"), &appid);
            if (appid <= 0) continue;                 /* :893-894 */
            sci = kn_json_get(&d, sn, "sessionControlInfo");
            if (jstr(&d, kn_json_get(&d, sci, "ip"),
                     host, sizeof host) != 0 || !host[0])
                continue;
            (void)jint(kn_json_get(&d, sci, "port"), &cport);
            if (cport != 443) continue;               /* :1976-1980 */
            if (!cm_host_trusted(host, out->control_host,
                                 sizeof out->control_host))
                continue;                             /* :895-899 */
            snprintf(out->session_id, sizeof out->session_id, "%s", sid);
            out->status = status;
            out->app_id = appid;
            (void)jint(kn_json_get(&d, srd, "appLaunchMode"),
                       &out->echo_app_launch_mode);
            if (kn_json_bool(kn_json_get(&d, srd,
                                         "enablePersistingInGameSettings"),
                             &b) == KN_JSON_OK)
                out->echo_persist = b ? 1 : 0;
            (void)jstr(&d, kn_json_get(&d, srd, "clientPlatformName"),
                       out->echo_platform, sizeof out->echo_platform);
            return 0;
        }
    }
    return -1;                    /* nothing resumable — re-queue path */
}

int kn_cm_build_resume_body(const kn_cm_settings *s,
                            const kn_cm_conflict *cf,
                            char *out, size_t outsz, size_t *out_len)
{
    char srd[4096];
    const char *platform = "Windows";
    int n, m = 0;

    if (!s || !s->title || !s->device_id || !out || !out_len)
        return KN_HTTP_ERR_ARGS;
    if (cf && cf->echo_platform[0]) platform = cf->echo_platform;

    /* the subset (:1546-1567) — same value sources as the create body;
     * clientPlatformName honours the echo (:1589-1597 replaces it). */
    n = snprintf(srd, sizeof srd,
        "{\"appId\":%lld,"
        "\"audioMode\":2,"
        "\"remoteControllersBitmap\":0,"
        "\"sdrHdrMode\":0,"
        "\"networkTestSessionId\":null,"
        "\"availableSupportedControllers\":[2],"
        "\"preferredController\":2,"
        "\"clientVersion\":\"30.0\","
        "\"deviceHashId\":\"%.128s\","
        "\"internalTitle\":\"%.120s\","
        "\"clientPlatformName\":\"%.31s\","
        "\"surroundAudioInfo\":0,"
        "\"clientTimezoneOffset\":%ld,"
        "\"clientIdentification\":\"GFN-PC\","
        "\"parentSessionId\":null,"
        "\"streamerVersion\":\"14\","
        "\"secureRTSPSupported\":true,"
        "\"metaData\":[{\"key\":\"wssignaling\",\"value\":\"1\"},"
                      "{\"key\":\"SubSessionId\",\"value\":\"%.64s\"},"
                      "{\"key\":\"surroundAudioInfo\",\"value\":\"2\"}]",
        s->app_id, s->device_id, s->title, platform,
        s->timezone_offset_ms,
        (s->subsession_id && s->subsession_id[0]) ? s->subsession_id
                                                  : s->device_id);
    if (n < 0 || (size_t)n >= sizeof srd) return KN_HTTP_ERR_BIG;
    /* the echoed keys — inserted ONLY when the original session carried
     * them non-null (:1589-1597); a duplicate key would be malformed. */
    if (cf && cf->echo_app_launch_mode >= 0) {
        m = snprintf(srd + n, sizeof srd - (size_t)n,
                     ",\"appLaunchMode\":%lld", cf->echo_app_launch_mode);
        if (m < 0 || n + m >= (int)sizeof srd) return KN_HTTP_ERR_BIG;
        n += m;
    }
    if (cf && cf->echo_persist >= 0) {
        m = snprintf(srd + n, sizeof srd - (size_t)n,
                     ",\"enablePersistingInGameSettings\":%s",
                     cf->echo_persist ? "true" : "false");
        if (m < 0 || n + m >= (int)sizeof srd) return KN_HTTP_ERR_BIG;
        n += m;
    }
    m = snprintf(srd + n, sizeof srd - (size_t)n, "}");
    if (m < 0 || n + m >= (int)sizeof srd) return KN_HTTP_ERR_BIG;
    n += m;

    m = snprintf(out, outsz,
        "{\"action\":2,\"data\":\"RESUME\",\"sessionRequestData\":%s,"
        "\"metaData\":null,\"adUpdates\":null}", srd);
    if (m < 0 || (size_t)m >= outsz) return KN_HTTP_ERR_BIG;
    *out_len = (size_t)m;
    return KN_HTTP_OK;
}

int kn_cm_resume(kn_http_io io, const char *host, const char *base_path,
                 const kn_cm_settings *s, const kn_cm_conflict *cf,
                 uint32_t timeout_ms,
                 uint64_t (*now_ms)(void *), void *now_ctx,
                 uint8_t *rxb, size_t rxbsz, char *jsonbuf, size_t jsoncap,
                 int *http_status, kn_cm_session *out)
{
    char path[448], body[6144];
    size_t blen = 0;
    int n;
    if (!host || !s || !cf || !cf->session_id[0])
        return KN_HTTP_ERR_ARGS;
    if (kn_cm_build_resume_body(s, cf, body, sizeof body,
                                &blen) != KN_HTTP_OK)
        return KN_HTTP_ERR_ARGS;
    n = snprintf(path, sizeof path,
                 "%sv2/session/%.64s?keyboardLayout=en-US&languageCode=en_US",
                 base_path ? base_path : "/", cf->session_id);
    if (n < 0 || (size_t)n >= sizeof path) return KN_HTTP_ERR_ARGS;
    return cm_round(io, "PUT", host, path, s, body, blen, timeout_ms,
                    now_ms, now_ctx, rxb, rxbsz, jsonbuf, jsoncap,
                    http_status, out);
}
