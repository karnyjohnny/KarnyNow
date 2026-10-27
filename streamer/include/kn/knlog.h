/* knlog.h — KarnyNow structured logging subsystem (docs/BOOTSTRAP.md §7, decision D13).
 *
 * One event stream, rendered two ways (BOOTSTRAP §7.1):
 *   - human log   logs/<channel>.log    one line per event
 *   - structured  logs/<channel>.jsonl  one JSON object per line (machine-parsable)
 *
 * Design rules enforced here (from §7.12, all learned from lost data):
 *   - flush+fsync after every ERROR/WARN and in sealed mode; never lose the tail
 *   - no fseek/ftell over our own output; append + full flush only
 *   - log lengths and presence for secrets, never values (whitelist redaction, §7.11)
 *   - bounded everything: fixed field count, fixed arena, bounded line buffers;
 *     overflow is counted and reported, never fatal, never unbounded
 *
 * Platform: Win32 (Win7+, mingw-w64) primary; POSIX for host-side unit tests.
 * Wall-clock resolution on Win7 is ~15.6 ms unless the process raised the timer
 * (timeBeginPeriod) — GetSystemTimePreciseAsFileTime (Win8+) is probed at init.
 * Use mono_ms — not ts — for latency arithmetic (§7.2).
 *
 * Thread safety: all functions are safe from any thread. kn_lctx objects are
 * per-caller (stack) and must not be shared between threads.
 *
 * Disk budget (§7.3): rotate at rotate_bytes (default 2 MiB); generations =
 * total files per channel+format (default 3: current + .1 + .2). Default worst
 * case: 8 ch x 3 x 2 MiB (.log) + 2 ch (perf, transport) x 3 x 2 MiB (.jsonl)
 * = 60 MiB, far less in practice (TRACE/DEBUG off by default). The .jsonl sink
 * additionally drops TRACE events (jsonl_min_level default = KN_DEBUG).
 */
#ifndef KN_LOG_H
#define KN_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types */

/* Levels are 1-based so that a zeroed kn_log_config unambiguously means
 * "use defaults" (0 is not a valid level). Lower value = more verbose. */
typedef enum kn_level {
    KN_TRACE = 1,
    KN_DEBUG = 2,
    KN_INFO  = 3,
    KN_WARN  = 4,
    KN_ERROR = 5
} kn_level;

typedef enum kn_channel {
    KN_CH_APP = 0,       /* lifecycle, state transitions, user actions, config */
    KN_CH_HTTP,          /* OAuth, GraphQL, CloudMatch request/response pairs  */
    KN_CH_RTSP,          /* WebSocket + RTSP transcript, both SDP bodies       */
    KN_CH_TRANSPORT,     /* SRTP, ICE, DTLS, SCTP, reorder, NACK, FEC          */
    KN_CH_MEDIA,         /* decode, presentation, audio playout                */
    KN_CH_INPUT,         /* Raw Input capture, NVB control frames              */
    KN_CH_PERF,          /* periodic counters (§7.6)                           */
    KN_CH_DIAG,          /* capability probes, self-tests, bundles             */
    KN_CH__COUNT
} kn_channel;

/* Correlation ids are 12 lowercase hex chars + NUL (§7.2). */
#define KN_CORR_LEN 12

#define KN_LOG_MAX_FIELDS 32     /* fields beyond this are counted as dropped */
#define KN_LOG_ARENA      2048   /* bytes for copied string values per event  */

/* Field value model. Exposed because kn_lctx embeds a fixed array of these
 * (no heap); treat all members as internal — use the kn_lctx_* setters. */
typedef enum kn_vtype {
    KN_V_STR = 0, KN_V_I64, KN_V_U64, KN_V_HEX, KN_V_BOOL, KN_V_F64
} kn_vtype;

typedef struct kn_field {
    const char        *key;   /* borrowed (use string literals) */
    kn_vtype           type;
    const char        *sval;  /* arena-owned, NUL-terminated */
    int                slen;
    long long          ival;
    unsigned long long uval;
    double             dval;
} kn_field;

typedef struct kn_lctx {
    kn_channel   channel;
    kn_level     level;
    const char  *msg;            /* borrowed; must outlive kn_log_emit      */
    const char  *correlation;    /* NULL -> thread default -> field omitted */
    int          nfields;
    int          dropped_fields; /* capacity overflow counter for this event */
    size_t       arena_used;
    /* internal */
    char         arena[KN_LOG_ARENA];
    kn_field     fields[KN_LOG_MAX_FIELDS];
} kn_lctx;

typedef struct kn_log_config {
    const char       *log_dir;         /* default "logs" */
    kn_level          default_level;   /* default KN_INFO */
    const kn_level   *channel_levels;  /* optional array[KN_CH__COUNT] of exact
                                          per-channel levels; NULL = default_level
                                          everywhere */
    unsigned          jsonl_channels;  /* bitmask (1 << kn_channel) that also get
                                          .jsonl; 0 = spec default (perf|transport);
                                          UINT_MAX = all channels */
    kn_level          jsonl_min_level; /* default KN_DEBUG (TRACE never in jsonl) */
    size_t            rotate_bytes;    /* default 2 MiB; floor 256 (for tests) */
    unsigned          generations;     /* files per channel+format, default 3 */
    const char       *build_stamp;     /* git rev + build config, logged once */
    const char       *version;         /* application version string */
} kn_log_config;

typedef struct kn_log_stats {
    unsigned long long events_emitted;   /* passed the level gate and rendered */
    unsigned long long events_filtered;  /* dropped by level gate              */
    unsigned long long lines_written;    /* human lines                        */
    unsigned long long json_written;     /* jsonl lines                        */
    unsigned long long bytes_written;    /* both sinks combined                */
    unsigned long long rotations;
    unsigned long long dropped_fields;   /* field-capacity overflows           */
    unsigned long long truncated_lines;  /* line-buffer overflows              */
    unsigned long long write_errors;     /* fwrite/rename/open failures        */
    unsigned long long secret_values_redacted;
    int sealed;                          /* 1 after kn_log_on_failure          */
    int amplified;                       /* 1 after kn_log_on_failure         */
} kn_log_stats;

/* Failure hook (§7.7 steps 3-4: bundle writer, self-test runner). Called
 * WITHOUT the log mutex held; callbacks may log. Up to 8 registrations. */
typedef void (*kn_failure_cb)(const char *op, const char *code,
                              const char *reason, void *userdata);

/* ------------------------------------------------------------------- init */

/* Returns 0 on success; -1 log dir cannot be created; -2 no sink could be
 * opened; -3 already initialized. Emits the process_start event carrying the
 * build stamp (§7.2 "once per process start"). */
int  kn_log_init(const kn_log_config *cfg);   /* cfg NULL = all defaults */
void kn_log_shutdown(void);                   /* flush + close everything  */
int  kn_log_is_initialized(void);

/* ---------------------------------------------------------------- levels */

/* Runtime level changes apply without restart (§7.4). */
void     kn_log_set_level(kn_channel ch, kn_level lvl);
/* Raise the verbosity of every channel to at least `at_least` (channels
 * already more verbose stay as they are; this never reduces verbosity). */
void     kn_log_set_all_levels(kn_level at_least);
kn_level kn_log_get_level(kn_channel ch);
int      kn_log_would_log(kn_channel ch, kn_level lvl); /* cheap pre-check so
                                                           callers can skip
                                                           building fields */

/* ------------------------------------------------------- event emission */

void kn_lctx_init(kn_lctx *c, kn_channel ch, kn_level lvl, const char *msg);
void kn_lctx_corr(kn_lctx *c, const char *correlation); /* NULL clears */

/* String values are copied into the event arena (safe to free/reuse after the
 * call). Keys are borrowed (use string literals). On capacity overflow the
 * field is dropped and counted — emission never fails. */
void kn_lctx_str(kn_lctx *c, const char *key, const char *val);
void kn_lctx_nstr(kn_lctx *c, const char *key, const char *val, size_t n);
void kn_lctx_i64(kn_lctx *c, const char *key, long long val);
void kn_lctx_u64(kn_lctx *c, const char *key, unsigned long long val);
void kn_lctx_hex(kn_lctx *c, const char *key, unsigned long long val); /* 0x%08X */
void kn_lctx_bool(kn_lctx *c, const char *key, int val);
void kn_lctx_f64(kn_lctx *c, const char *key, double val);

/* Explicit secret: value is replaced by "<redacted:N>" (N = byte length, so
 * present-vs-absent-vs-empty stays distinguishable, §7.12). kn_lctx_str also
 * auto-detects secret-like keys (kn_redact_key_is_secret) as defence in depth. */
void kn_lctx_secret(kn_lctx *c, const char *key, const char *val);

/* Renders to both sinks, applies rotation + flush policy. Safe from any
 * thread. Does nothing (counted) if the level gate rejects or log is down. */
void kn_log_emit(kn_lctx *c);

/* One-shot convenience: event with a message and no fields. */
void kn_log_simple(kn_channel ch, kn_level lvl, const char *correlation,
                   const char *msg);

/* ------------------------------------------------- correlation & session */

/* Generates a fresh 12-hex-char id into out (must be >= KN_CORR_LEN+1).
 * Returns out. One per user-visible operation (§7.2), then propagate. */
const char *kn_log_new_correlation(char out[KN_CORR_LEN + 1]);

/* Thread-default correlation used when kn_lctx has none. Stored in TLS,
 * copied internally; NULL clears. */
void        kn_log_set_thread_correlation(const char *corr);
const char *kn_log_get_thread_correlation(void); /* TLS buffer or NULL */

/* session_id field on every event while set (§7.2). Never set during auth.
 * NULL clears. Copied internally (max 63 chars). */
void kn_log_set_session_id(const char *session_id);

/* -------------------------------------------------- failure amplification */

/* §7.7: emit ERROR(app, operation_failed{op,code,reason,next_action}),
 * raise all channels to >= DEBUG (transport to TRACE), seal (flush+fsync
 * every event from now on), then invoke registered callbacks in order.
 * Every subsystem calls this on operation failure — the single choke point,
 * so amplification cannot be forgotten in one module. */
void kn_log_on_failure(const char *op, const char *code, const char *reason,
                       const char *next_action);
int  kn_log_failure_cb_register(kn_failure_cb cb, void *userdata); /* 0 ok, -1 full */

/* Explicit flush of every open sink (fflush + fsync equivalent). */
void kn_log_flush_all(void);

void kn_log_get_stats(kn_log_stats *out);

/* ---------------------------------------------------------------- redaction */

/* Whitelist redaction (§7.11), also usable at HTTP boundaries:
 * - key/name matching is case-insensitive; '_' and '-' are insignificant
 * - secret patterns: token, secret, password, credential, authorization,
 *   fingerprint, keyhex, salthex, privatekey; exact devicecode/usercode;
 *   srtp*key*, srtp*salt*
 * Returns 1 if the key must be redacted. */
int kn_redact_key_is_secret(const char *key);

/* Renders "<redacted:N>" into dst (N = strlen(val), val may be NULL -> N=0).
 * Returns dst. */
char *kn_redact_value(char *dst, size_t dstsz, const char *val);

/* Header whitelist (§7.5): only known-safe names may carry values into logs.
 * Default list: content-type, user-agent, accept, accept-language,
 * nv-client-type, nv-client-id, nv-app-version, nv-device-make,
 * nv-device-model, x-nv-client-identity. Extra names can be registered
 * (copied; up to 32). Returns 1 if the value may be logged verbatim. */
int kn_redact_header_allowed(const char *name);
int kn_redact_allow_header(const char *name); /* 0 ok, -1 full */

/* "Bearer xxx.yyy" -> "Bearer" (static string). Unknown -> "Unknown". */
const char *kn_redact_auth_scheme(const char *authorization_value);

#ifdef __cplusplus
}
#endif

#endif /* KN_LOG_H */
