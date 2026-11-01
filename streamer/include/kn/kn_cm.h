/* kn_cm.h — CloudMatch session client (stage-2 bring-up subset).
 *
 * Provenance: cloudmatch.rs @ a49b124 — POST v2/session (:361-371 body
 * :1643-1762), headers (:2036-2059), readiness (status 2|3 + non-empty
 * rtspsEndpoints, :1524-1533), resume statuses (:1505-1512), session_info
 * (:1946-2103) incl. the rtspsEndpoints DERIVATION from connectionInfo
 * (:1989-2022 — usage 16, or usage 14 with appLevelProtocol 1|6), the
 * media/bundle connection (usage 2|17, :1866-1878) and queue_position's
 * four-source fallback (:1971-1982). Wire facts mirrored in
 * docs/RTSP.md §5; login/token: docs/AUTH.md.
 *
 * F37 re-verified against OpenNOW @ 1cb3479 (2026-10-29): the native
 * create body is build_create_body :1643-1762 (metadata trio :1674,
 * appLaunchMode :2979-2985 — "default" is 1, NOT 0; dlssOverrides =
 * dlss.rs default_overrides :22-24), cloudmatch_headers :2263-2287
 * (Bifrost UA + x-nv-client-identity + WINDOWS/DESKTOP/UNKNOWN/UNKNOWN)
 * and the endpoint filter :1989-2022 — unchanged. docs/RTSP.md §59.
 *
 * DEVIATION (documented, bounded): R22's full 31-field session context is
 * the stage-4 TUI IPC contract; this struct carries the stage-2 SUBSET
 * the media path consumes (sessionId/status/phase/queue/seat, the first
 * rtsps endpoint + its wss transform, serverIp, the bundle peer). The raw
 * JSON is available to the caller for logging (redacted) and for the
 * stage-4 upgrade. Fields marked UNVERIFIED in the .c body builder are
 * measured at R7 (the first real POST) — that gate exists precisely
 * because no captured cloud traffic backs them yet (R32's rule).
 */
#ifndef KN_CM_H
#define KN_CM_H

#include <stddef.h>
#include <stdint.h>

#include "kn/kn_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Identity constants (transcribed; NOT secrets — they ship in the
 * reference binary): docs/AUTH.md §1 + cloudmatch.rs:16-17. */
#define KN_CM_LCARS_CLIENT_ID  "ec7e38d4-03af-4b58-b131-cfb0495903ab"
#define KN_CM_CLIENT_VERSION   "2.0.87.131"
/* F35 (round N slice 4n): the web client's version, transcribed from the
 * operator's SSLKEYLOGFILE capture of play.geforcenow.com, 2026-11-01
 * (tools/wiretap): nv-client-version: 2.0.89.154 on every /v2/session
 * request. The browser profile announces THIS; the legacy profile keeps
 * the OpenNOW-transcribed 2.0.87.131. */
#define KN_CM_CLIENT_VERSION_BROWSER "2.0.89.154"
/* F37 (RTSP §59): the NATIVE client's User-Agent, byte-for-byte from
 * bifrost_user_agent (cloudmatch.rs :3025-3033, Windows leg) — it is
 * ALSO the x-nv-client-identity value (:2266-2267, :2285-2286). The
 * legacy table sent NO user-agent at all; the official native client
 * always sends this one. */
#define KN_CM_BIFROST_UA \
    "GFN-PC/30.0 (Windows NT 10.0) BifrostClientSDK/4.9 (38495286)"
#define KN_CM_DEFAULT_BASE_HOST "prod.cloudmatchbeta.nvidiagrid.net"

typedef struct kn_cm_settings {
    const char *token;         /* GFNJWT service token (id_token) — the
                                  logging layer must never see it (§7) */
    const char *device_id;     /* deviceHashId + x-device-id */
    long long   app_id;        /* numeric GFN appId */
    const char *title;         /* internalTitle (legacy profile only —
                                  the browser profile sends null) */
    uint32_t    width, height, fps;
    long        timezone_offset_ms;   /* clientTimezoneOffset */
    /* F35 (round N slice 4n): the identity profile knob.
     * F37 (RTSP §59) CORRECTS F35's premise and flips the default:
     * the browser identity makes CloudMatch provision a WEBRTC seat —
     * connectionInfo usage 14 with appLevelProtocol 5 on :443 /nvst/,
     * the JSON sign_in protocol (gfn_ref_urls.txt) — which this RTSP
     * client can never consume (run 101: ready:0 for 217 s, then the
     * cloud's INTERNAL_ERROR teardown). OpenNOW rejects the same shape
     * (cloudmatch.rs :1989-2022 accepts only usage 16 or usage 14 with
     * alp 1|6) and provisions its RTSP seat with the NATIVE identity:
     * secureRTSPSupported true + streamerVersion "14" + NO
     * GSStreamerType metadata. Profile 0 is therefore OpenNOW-strict
     * NATIVE (the DEFAULT and the only streamable identity); profile 1
     * stays reachable for CloudMatch-level A/B experiments ONLY — it
     * queues and handles ads faithfully but its seat is unstreamable
     * here by construction. */
    int         profile;       /* 0 = OpenNOW-strict native (DEFAULT),
                                  1 = browser parity (CM experiments;
                                      provisions a WebRTC-only seat) */
    const char *subsession_id; /* metaData SubSessionId — a FRESH random
                                  UUID per session (OpenNOW random_uuid,
                                  cloudmatch.rs :1675). F37: BOTH
                                  profiles use it; NULL/empty degrades
                                  to the device id (the old D-cm2
                                  behaviour) so no caller can crash the
                                  native leg. Required for profile 1. */
    const char *tz_name;       /* metaData clientTimezone IANA name,
                                  e.g. "Europe/Warsaw"; required for
                                  profile 1 (the native body carries the
                                  offset only, like OpenNOW's) */
    int         user_age;      /* userAge; <= 0 falls back to 25 */
} kn_cm_settings;

/* session_phase mapping (cloudmatch.rs:2698-2707). */
const char *kn_cm_phase(long long status);

typedef struct kn_cm_session {
    char      session_id[64];
    long long status;
    const char *phase;         /* static string from kn_cm_phase */
    long long queue_position;  int have_queue;   /* first >0 of the four
                                                    sources (:1971-1982) */
    long long seat_setup_step; int have_seat;    /* seatSetupInfo.seatSetupStep */
    char      rtsps_endpoint[256]; int have_rtsps;  /* first derived */
    char      signaling_url[256];   /* rtsps:// -> wss:// (:2046-2048) */
    char      server_ip[64];        /* signaling conn ip, else
                                       sessionControlInfo.ip (:1966-1969) */
    char      media_ip[64];         /* bundle peer (usage 2|17) */
    uint16_t  media_port;
    int       have_media;
    int       resumable;            /* status 2..5 (:1505-1512) */
    int       ready;                /* status 2|3 AND have_rtsps */
    /* Round XXIII (the R7 run-1 finding): the REGIONAL control node that
     * owns this session — session.sessionControlInfo.{ip,port}
     * (:1966-1984). Every poll/stop must target it (the reference learns
     * it into control_base at create :401-414/:1149-1152 and polls ONLY
     * there :652-656); polling the global create front instead returns
     * stale snapshots (frozen queuePosition — measured live 2026-10-25). */
    char      control_host[128];
    uint16_t  control_port;         /* 443 when absent (:1978) */
    int       have_control;
    /* requestStatus mirror (:2310-2352). Filled even when the session
     * extraction FAILS, so callers can surface the cloud-side reason
     * (e.g. statusCode 11 SESSION_LIMIT). 0/"" = absent. */
    long long req_status_code;
    char      req_status_desc[64];
    long long unified_error;          /* requestStatus.unifiedErrorCode
                                         (:2343) — 0 = absent */
    long long error_code;             /* session.errorCode — 1 while queued
                                         (run-5 capture); the rejected-payload
                                         diagnostic (round XXVII) */
    /* F36 (run-100 read): the FREE-TIER AD GATE. A browser-identity
     * session gets sessionAds offered from the first poll (it is the
     * FIRST key of the session object); ignoring it ends the session
     * with statusCode 79 SESSION_WAITING_ADS_TIME_EXPIRED after the
     * ~120 s window + grace. The client must fetch the VAST adUrl,
     * honour its trackers and report start/finish (or cancel) via
     * PUT v2/session/{id} {"action":6,"adUpdates":[...]} — the exact
     * mechanism transcribed from OpenNOW cloudmatch.rs report_ad +
     * QueueAdPlayback.qml and from the browser's own IMA cancel PUT in
     * the R2 capture (gfn_ref_h2.json). */
    char      ad_id[64];              /* sessionAds[0].adId — "" = none */
    int       have_ad;                /* a non-empty sessionAds[0].adId */
    long long ad_state;               /* sessionAds[0].adState (1=offered) */
    long long ad_len_s;               /* sessionAds[0].adLengthInSeconds */
    char      ad_url[512];            /* sessionAds[0].adUrl (VAST/IMA) */
    int       ads_required;           /* session.sessionAdsRequired */
    /* F37 (run-101 read, RTSP §59.2): the unusable-seat witness. A
     * browser-identity session reaches status 2 with connectionInfo
     * entries that conn_signaling REJECTS (usage 14 + appLevelProtocol
     * 5 = the WebRTC /nvst/ sign_in endpoint on :443) — run 101 polled
     * that "ready:0" state for 217 s in silence until the cloud tore
     * the seat down with INTERNAL_ERROR 02F12011. These fields let the
     * session loop name the failure on its FIRST poll instead: the
     * caller emits one conn_unusable event and stops with a stop_reason
     * that points at --cm-profile. First entry only (a diagnostic, not
     * a model of the array). */
    int       conn_count;             /* connectionInfo entries seen */
    long long conn0_usage;            /* entry[0].usage (-1 = absent) */
    long long conn0_alp;              /* entry[0].appLevelProtocol */
    long long conn0_port;             /* entry[0].port */
    char      conn0_path[48];         /* entry[0].resourcePath */
} kn_cm_session;

/* Build the POST v2/session body ({"sessionRequestData":{...}}) —
 * field-for-field with build_create_body for the H264/8-bit/no-HDR
 * envelope (codec is client-selected and never sent; the reference's
 * alliance-WebRTC variant is out of scope). Returns 0 / negative.
 * networkTestSessionId is sent as null (bring-up deviation D-cm1: the
 * reference tolerates "not_requested"; R7 measures whether the cloud
 * accepts it for this account/zone). */
int kn_cm_build_create_body(const kn_cm_settings *s, char *out,
                            size_t outsz, size_t *out_len);

/* The Authorization header value ("GFNJWT <token>") — the token itself
 * is NEVER logged; this buffer is for the request only. */
int kn_cm_auth_header(const kn_cm_settings *s, char *out, size_t outsz);

/* Fixed nv-client / nv-device header set (values transcribed from
 * cloudmatch_headers + the platform constants; Content-Type text/plain
 * quirk included). Fills up to `max` entries; returns the count. */
int kn_cm_fixed_headers(const kn_cm_settings *s, kn_http_header *h, int max);

/* Extract the session subset from a CloudMatch response BODY (the
 * {"requestStatus":..,"session":{..}} payload). Returns 0 when a
 * non-empty sessionId was present (the reference's hard requirement,
 * :1953-1955), -1 otherwise; fields are best-effort filled either way. */
int kn_cm_extract_session(const char *json_text, kn_cm_session *out);

/* Networked operations (deadline via the injected clock; rxb/jsonbuf are
 * caller-owned scratch/dest). *http_status receives the raw code.
 * create: POST {base_path}v2/session. poll: GET {base_path}v2/session/{id}.
 * base_path ends with '/' ("" = root). Returns KN_HTTP_OK when a
 * response was parsed (check http_status: 200 = usable body), else the
 * negative transport code. */
int kn_cm_create(kn_http_io io, const char *host, const char *base_path,
                 const kn_cm_settings *s, uint32_t timeout_ms,
                 uint64_t (*now_ms)(void *), void *now_ctx,
                 uint8_t *rxb, size_t rxbsz, char *jsonbuf, size_t jsoncap,
                 int *http_status, kn_cm_session *out);
int kn_cm_poll(kn_http_io io, const char *host, const char *base_path,
               const kn_cm_settings *s, const char *session_id,
               uint32_t timeout_ms,
               uint64_t (*now_ms)(void *), void *now_ctx,
               uint8_t *rxb, size_t rxbsz, char *jsonbuf, size_t jsoncap,
               int *http_status, kn_cm_session *out);

/* Validate + normalize the LEARNED control node (sessionControlInfo.ip)
 * into a pollable host. The C mirror of trusted_learned_server_base +
 * trusted_cloudmatch_base + public_session_host (cloudmatch.rs
 * :2400-2456) and the port==443 gate (:1976-1980): https-only, no
 * userinfo/query/fragment, port 443, public IPv4/IPv6/domain (private,
 * loopback, link-local, CGNAT, multicast, reserved and .local are all
 * rejected — a response can never redirect polling into a private
 * address). Returns 1 with host_out/port_out filled when pollable; 0
 * when absent/untrusted — the caller MUST then fall back to the create
 * host (the reference's unwrap_or(base) semantics, :1984). */
int kn_cm_control_base(const kn_cm_session *s, char *host_out,
                       size_t hostsz, uint16_t *port_out);

/* DELETE {base_path}v2/session/{id} — the reference's stop_at
 * (:789-800). Best-effort cleanup so an aborted queue wait does not
 * orphan a session (a pending one blocks the next create with
 * SESSION_LIMIT, :2354+). Mirrors validate_delete_response (:2728):
 * 2xx and 404 both mean "gone" — the CALLER decides via *http_status. */
int kn_cm_cancel(kn_http_io io, const char *host, const char *base_path,
                 const kn_cm_settings *s, const char *session_id,
                 uint32_t timeout_ms,
                 uint64_t (*now_ms)(void *), void *now_ctx,
                 uint8_t *rxb, size_t rxbsz, char *jsonbuf, size_t jsoncap,
                 int *http_status);

/* F38 (run-102 read, RTSP §60.2): SESSION RESUME. A 403
 * SESSION_LIMIT_EXCEEDED create response carries the account's live
 * session(s) — run 102 proved it: `otherUserSessions[]` held the
 * previous draw's orphan (status 3, its own control node, and the
 * NATIVE :322 RTSPS connectionInfo). The reference attaches instead of
 * re-queueing (cloudmatch.rs session.claim :1160-1230 +
 * session_requires_resume :1506-1515: status 2..=5 is resumable, 1|6 is
 * not, anything else is an error): PUT v2/session/{id} with the RESUME
 * body on the session's control node, then poll until fresh ready.
 * parse_conflict mirrors the reference's candidate chain (:889-895:
 * otherUserSessions first, then session), filtering appId > 0, status
 * 2..=5, and a trusted public control host (the same gate as
 * kn_cm_control_base). Returns 0 with out filled, negative when no
 * resumable candidate exists. */
typedef struct {
    char      session_id[64];
    char      control_host[128];
    uint16_t  control_port;             /* always 443 when parsed */
    long long status;
    long long app_id;
    /* echoed sessionRequestData of the matched session — the resume
     * body re-sends appLaunchMode / enablePersistingInGameSettings /
     * clientPlatformName from it (:1589-1597). -1/"" = absent. */
    long long echo_app_launch_mode;
    int       echo_persist;             /* -1 absent, 0 false, 1 true */
    char      echo_platform[32];
} kn_cm_conflict;
int kn_cm_parse_conflict(const char *json, kn_cm_conflict *out);

/* The RESUME body — key set and value sources from build_resume_body
 * (:1540-1599): a fresh create body's subset (appId, audioMode,
 * remoteControllersBitmap, sdrHdrMode, networkTestSessionId,
 * availableSupportedControllers, preferredController, clientVersion,
 * deviceHashId, internalTitle, clientPlatformName, surroundAudioInfo,
 * clientTimezoneOffset, clientIdentification, parentSessionId,
 * streamerVersion, secureRTSPSupported, metaData) wrapped in
 * {"action":2,"data":"RESUME","sessionRequestData":{...},
 *  "metaData":null,"adUpdates":null}.
 * Resume must NOT renegotiate codec/geometry/fps/bitrate — the monitor
 * settings, streaming features and sdkVersion keys are absent by
 * design. `cf` supplies the echoed fields (NULL = omit all three). */
int kn_cm_build_resume_body(const kn_cm_settings *s,
                            const kn_cm_conflict *cf,
                            char *out, size_t outsz, size_t *out_len);

/* PUT {base_path}v2/session/{id}?keyboardLayout=en-US&languageCode=en_US
 * (append_session_preferences defaults, language.rs :75-83) on the
 * conflict's control host. The response is a full session payload —
 * extract fills `out` (may be NULL). Same header table as every other
 * CM round (content-type text/plain residual: the reference's .json()
 * sends application/json — the F36 ad-report precedent proved the
 * server accepts ours; disclosed). */
int kn_cm_resume(kn_http_io io, const char *host, const char *base_path,
                 const kn_cm_settings *s, const kn_cm_conflict *cf,
                 uint32_t timeout_ms,
                 uint64_t (*now_ms)(void *), void *now_ctx,
                 uint8_t *rxb, size_t rxbsz, char *jsonbuf, size_t jsoncap,
                 int *http_status, kn_cm_session *out);

/* F36 (run-100 read): the queue-ad report. ad_action takes the
 * reference's mapping (cloudmatch.rs report_ad): 1 start, 2 pause,
 * 3 resume, 4 finish, 5 cancel (KN_CM_AD_* below). The body mirrors the
 * browser IMA PUT captured in gfn_ref_h2.json byte-for-byte in key
 * order: {"action":6,"adUpdates":[{"adId":..,"adAction":N,
 * "clientTimestamp":<unix s>[,"cancelReason":..],"watchedTimeInMs":W,
 * "pausedTimeInMs":P,"adDurationInMs":D}]}. cancel_reason NULL omits the
 * key. The response is a full session payload — extract fills `out`
 * (may be NULL). PUT {base_path}v2/session/{id} on the CONTROL host. */
#define KN_CM_AD_START   1
#define KN_CM_AD_PAUSE   2
#define KN_CM_AD_RESUME  3
#define KN_CM_AD_FINISH  4
#define KN_CM_AD_CANCEL  5
int kn_cm_build_ad_body(const char *ad_id, int ad_action,
                        long long client_ts, long long watched_ms,
                        long long paused_ms, long long duration_ms,
                        const char *cancel_reason,
                        char *out, size_t outsz, size_t *out_len);
int kn_cm_report_ad(kn_http_io io, const char *host, const char *base_path,
                    const kn_cm_settings *s, const char *session_id,
                    const char *ad_id,
                    int ad_action, long long watched_ms, long long paused_ms,
                    long long duration_ms, const char *cancel_reason,
                    uint32_t timeout_ms,
                    uint64_t (*now_ms)(void *), void *now_ctx,
                    uint8_t *rxb, size_t rxbsz, char *jsonbuf,
                    size_t jsoncap, int *http_status, kn_cm_session *out);

#ifdef __cplusplus
}
#endif

#endif /* KN_CM_H */
