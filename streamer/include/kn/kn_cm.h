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
#define KN_CM_DEFAULT_BASE_HOST "prod.cloudmatchbeta.nvidiagrid.net"

typedef struct kn_cm_settings {
    const char *token;         /* GFNJWT service token (id_token) — the
                                  logging layer must never see it (§7) */
    const char *device_id;     /* deviceHashId + x-device-id */
    long long   app_id;        /* numeric GFN appId */
    const char *title;         /* internalTitle */
    uint32_t    width, height, fps;
    long        timezone_offset_ms;   /* clientTimezoneOffset */
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

#ifdef __cplusplus
}
#endif

#endif /* KN_CM_H */
