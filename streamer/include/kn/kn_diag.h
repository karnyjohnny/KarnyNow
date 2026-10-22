/* kn_diag.h — diagnostic bundle writer (BOOTSTRAP.md §7.8, §7.7 steps 3-5).
 *
 * Produces logs/diag-<UTC>-<shortid>.zip: a STORE-ONLY zip (no compression,
 * no external dependency) with summary.json as the FIRST entry ("this is
 * what gets read first", §7.8). Written automatically on any ERROR through
 * the kn_log failure-callback hook (kn_diag_install) and on demand
 * (knvstd --diagnostics).
 *
 * Design rules (all from §7.12 / lost-data experience):
 *   - NEVER fseek over our own output. Every entry is fully materialised in
 *     RAM (CRC + size known) before its local header is written; the central
 *     directory is appended at the end. This is the exact bug that produced
 *     0-byte T4 v3 stage files.
 *   - Hard budget: cap_bytes (default 8 MiB). Enforced continuously at add
 *     time AND at finish, so RAM never exceeds ~cap. Truncation policy:
 *     lowest priority class first, largest artifact first, newest bytes kept
 *     (tail-kept for text artifacts, with a marker line prepended).
 *   - Priority classes: CORE (summary/state/build/selftest/capabilities) is
 *     never truncated; PRIMARY (current logs, wire dumps, perf) is tail-
 *     truncated; SECONDARY (rotated .1/.2 generations) is dropped whole.
 *   - A single artifact larger than the cap is tail-truncated at add time.
 *   - Recursion guard: the failure callback cannot re-enter itself (a bundle
 *     write failure logs an ERROR, which must not trigger another bundle).
 *
 * Bundle contents (§7.8), by producer:
 *   summary.json     rendered by kn_diag_finish from trigger + verdicts + artifact table
 *   capabilities.json  fed by the platform probe (selftest phase B) via add_mem
 *   logs/<channel>.log/.jsonl   collected from the log dir (kn_diag_collect_logs)
 *   wire/<socket>.txt    fed by kn_ring_dump output (transport code) via add_file
 *   state.json       auto (log stats) unless the caller adds one first
 *   perf.jsonl       fed by the perf subsystem (tail of logs/perf.jsonl if absent)
 *   selftest.json    fed by the self-test runner (kn_selftest)
 *   build.json       auto-generated from compile-time macros + runtime probe
 *
 * Thread safety: a bundle object is single-threaded (one writer). The
 * install/last-path global state is mutex-guarded.
 *
 * Memory policy: heap is used for artifact buffers ONLY (bounded by cap);
 * all fixed tables are inline. No allocation while writing the zip itself.
 */
#ifndef KN_DIAG_H
#define KN_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ limits */

#define KN_DIAG_PATH_MAX      512   /* same convention as knlog.c */
#define KN_DIAG_NAME_MAX      128   /* zip entry name (forward slashes) */
#define KN_DIAG_MAX_ARTIFACTS  64   /* 8ch*3gen*2fmt = 48 logs + headroom */
#define KN_DIAG_MAX_VERDICTS   16
#define KN_DIAG_SUBSYS_MAX     48
#define KN_DIAG_REASON_MAX    192
#define KN_DIAG_DEFAULT_CAP   (8u * 1024u * 1024u)   /* §7.8: e.g. 8 MiB */
#define KN_DIAG_ART_FILE_CAP  (2u * 1024u * 1024u)   /* per-file read ceiling */
#define KN_DIAG_MIN_KEEP      4096u /* never tail-truncate below this */

/* Priority classes for the truncation policy (lower = more protected). */
enum {
    KN_DIAG_PRIO_CORE = 0,      /* summary/state/build/selftest/capabilities */
    KN_DIAG_PRIO_PRIMARY,       /* current logs, wire dumps, perf tail */
    KN_DIAG_PRIO_SECONDARY      /* rotated generations (.1/.2), drop-first */
};

/* summary.json verdict status (§7.8: "one machine-readable verdict per
 * subsystem: ok / degraded / failed, with the reason and the evidence
 * pointer"). */
typedef enum kn_diag_status {
    KN_DIAG_OK = 0,
    KN_DIAG_DEGRADED,
    KN_DIAG_FAILED
} kn_diag_status;

typedef struct kn_diag_bundle kn_diag_bundle;   /* opaque; heap-allocated */

/* ------------------------------------------------------------------ bundle */

/* Start a bundle. out_dir = directory for the zip (normally the log dir;
 * pass NULL for "logs"). cap_bytes 0 = KN_DIAG_DEFAULT_CAP. Returns NULL on
 * alloc failure or absurd arguments. */
kn_diag_bundle *kn_diag_begin(const char *out_dir, size_t cap_bytes);

/* Add one artifact. zip_path uses forward slashes ("logs/app.log").
 * add_mem COPIES the data; add_file reads the whole file (tail-keeping if
 * it exceeds KN_DIAG_ART_FILE_CAP or the bundle cap). Duplicate zip_path is
 * rejected (-2). Returns 0 ok, -1 arg/alloc/read error, -2 duplicate,
 * -3 artifact table full. */
int kn_diag_add_mem (kn_diag_bundle *b, const char *zip_path,
                     const void *data, size_t len, int prio);
int kn_diag_add_file(kn_diag_bundle *b, const char *zip_path,
                     const char *src_path, int prio);

/* True if an artifact with this zip_path is already present. */
int kn_diag_has(kn_diag_bundle *b, const char *zip_path);

/* Record one subsystem verdict for summary.json. Copies its inputs.
 * Returns 0 ok, -1 arg error, -3 verdict table full. */
int kn_diag_verdict(kn_diag_bundle *b, const char *subsystem,
                    kn_diag_status status, const char *reason,
                    const char *evidence);

/* Enumerate *.log / *.jsonl in logs_dir and add them under "logs/".
 * Current generation -> PRIMARY, rotated (.1/.2/...) -> SECONDARY.
 * Entries are name-sorted for deterministic order. Returns count added
 * (>= 0), or -1 on directory error. */
int kn_diag_collect_logs(kn_diag_bundle *b, const char *logs_dir);

/* Render summary.json, enforce the cap, write the zip sequentially
 * (local headers + data, central directory, EOCD), fflush + fsync + fclose.
 * trigger_* may be NULL (on-demand bundle). On success returns 0 and
 * out_path (>= KN_DIAG_PATH_MAX) receives the full zip path.
 * Returns -1 arg error, -2 no artifacts (refuses to write an empty bundle),
 * -3 zip write error (a partial file is removed). */
int kn_diag_finish(kn_diag_bundle *b, const char *trigger_op,
                   const char *trigger_code, const char *trigger_reason,
                   char *out_path, size_t out_path_sz);

/* Free everything without writing. NULL-safe. */
void kn_diag_abort(kn_diag_bundle *b);

/* ------------------------------------------------------- auto-integration */

/* One-shot bundle used by the failure hook and --diagnostics:
 * begin -> collect_logs -> auto build.json + state.json (unless present) ->
 * "logging" verdict from kn_log_get_stats -> finish.
 * Returns 0 on success with out_path filled. Safe to call with logging
 * uninitialized (verdict then reports that). */
int kn_diag_write_bundle(const char *logs_dir, const char *trigger_op,
                         const char *trigger_code, const char *trigger_reason,
                         char *out_path, size_t out_path_sz);

/* Register kn_diag as a kn_log failure callback (§7.7 step 3). Idempotent:
 * a second call with the same dir returns 0 without re-registering.
 * logs_dir is copied. Returns 0 ok, -1 callback table full / arg error. */
int kn_diag_install(const char *logs_dir);

/* Path of the most recent bundle written through the installed hook
 * (static storage; NULL until the first bundle). For the TUI one-screen
 * summary (§7.7 step 5: "the exact path of the bundle to send"). */
const char *kn_diag_last_bundle_path(void);

/* Optional self-test provider (§7.7 step 4: "run the relevant self-test and
 * include its result"). Renders the selftest.json content for the failed
 * op/code into buf; returns byte count, 0 = nothing to add.
 * kn_selftest_render_relevant matches this signature exactly — wire it at
 * startup with kn_diag_set_selftest_provider(kn_selftest_render_relevant).
 * Set once before threads start; NULL clears. write_bundle calls it unless
 * a "selftest.json" artifact was already added. */
typedef size_t (*kn_diag_selftest_provider_fn)(const char *op, const char *code,
                                               char *buf, size_t bufsz);
void kn_diag_set_selftest_provider(kn_diag_selftest_provider_fn fn);

/* ------------------------------------------------------------------ build */

/* Render build.json (§7.8: git rev, dirty flag, compiler version and exact
 * flags, _WIN32_WINNT/WINVER/NTDDI_VERSION actually in effect, library
 * versions) into buf. Compile-time inputs come from KN_DIAG_BUILD_* macros
 * (CMake-injected, default "unknown"); the effective Windows-target macros
 * are stringified here so the file records what the COMPILER saw — the §6.3
 * traps are invisible otherwise. Returns bytes written (excluding NUL). */
size_t kn_diag_render_build_json(char *buf, size_t bufsz);

/* Standard CRC-32 (reflected 0xEDB88320, as used by zip). Exported for the
 * unit test's known-answer check ("123456789" -> 0xCBF43926). */
uint32_t kn_diag_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KN_DIAG_H */
