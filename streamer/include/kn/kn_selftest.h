/* kn_selftest.h — self-tests that run without an account (BOOTSTRAP.md §7.9).
 *
 * Purpose: so that "it does not work" can be narrowed down BEFORE blaming the
 * protocol. Every self-test is:
 *   - runnable standalone:  knvstd --selftest=<name>   (and --selftest=all)
 *   - machine-readable:     one JSON object per result; the bundle consumes
 *                           the same structs as selftest.json (§7.8)
 *   - non-interactive and bounded in time (worst case ~1 s each; SHA-1 TEST3
 *                           hashes 1M bytes, clock.resolution sleeps ~50 ms)
 *
 * The twelve names from §7.9, by phase:
 *   A (offline, host-testable — IMPLEMENTED):
 *     fec.vectors     Cauchy RS encode/reconstruct against KAT vectors
 *                     (separates: our matrix vs a Vandermonde mistake)
 *     srtp.vectors    AES/HMAC/KDF/IV/protect/replay against KAT vectors
 *                     (separates: wrong key material vs wrong IV vs wrong ROC)
 *   B (Win32, no network — clock/console IMPLEMENTED, d3d/decode planned):
 *     clock.resolution  timeBeginPeriod effect on Sleep(1) + wall granularity
 *                       (separates: timer granularity for jitter/pacing)
 *     console.host      ConEmu hook detection + ANSI attribute probe
 *                       (separates: D1 verification, garbage output)
 *     d3d.formats       CheckDeviceFormat/FormatConversion matrix (D7 rungs)
 *     d3d.present       timed StretchRect+Present (GPU vs CPU vs clock)
 *     decode.synthetic  decode bundled .264, report fps/CPU (decoder vs transport)
 *   C (network, target machine only — planned):
 *     net.dns / net.tcp443 / net.tls   connectivity vs TLS vs HTTP semantics
 *     tls.chain       cert chain dump for login.nvidia.com (stale Win7 store)
 *     gs.parse        parse captured packets, print 16-byte GS fields
 *
 * Planned-but-unimplemented names are REGISTERED as stubs reporting
 * status "skipped" with an explicit reason — a missing test must never look
 * like a passing one (§10.6: name your unknowns).
 *
 * Result contract: status is one of ok / degraded / failed / skipped.
 * "skipped" is non-fatal (platform or phase); "degraded" means the test ran
 * and something is off but usable; "failed" is a hard verdict.
 */
#ifndef KN_SELFTEST_H
#define KN_SELFTEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_ST_NAME_MAX      48
#define KN_ST_REASON_MAX   192
#define KN_ST_EVIDENCE_MAX 1024   /* JSON object body rendered by the test */

typedef enum kn_st_status {
    KN_ST_OK = 0,
    KN_ST_DEGRADED,
    KN_ST_FAILED,
    KN_ST_SKIPPED
} kn_st_status;

typedef struct kn_st_result {
    char      name[KN_ST_NAME_MAX];
    int       status;                  /* kn_st_status */
    long long ms;                      /* wall duration of the run */
    int       checks;                  /* vector comparisons executed */
    int       failures;                /* vector mismatches */
    char      reason[KN_ST_REASON_MAX];       /* human, English */
    char      evidence[KN_ST_EVIDENCE_MAX];   /* JSON object (may be empty) */
} kn_st_result;

/* Status name for JSON ("ok"/"degraded"/"failed"/"skipped"). */
const char *kn_selftest_status_name(int status);

/* Registry access. Names are the §7.9 identifiers. */
int         kn_selftest_count(void);
const char *kn_selftest_name(int idx);          /* NULL if out of range */

/* Run one test by name. Returns 0 and fills *r (check r->status for the
 * verdict), or -1 if the name is not registered. r->name is always set on
 * success. NULL r -> -1. */
int kn_selftest_run(const char *name, kn_st_result *r);

/* Run every registered test in table order. Fills up to `max` results,
 * returns the count run (== registry count when max is large enough). */
int kn_selftest_run_all(kn_st_result *results, int max);

/* Render results as the bundle's selftest.json document (§7.8):
 * {"generated_utc":..., "results":[{...},...]}. Bounded, always valid JSON
 * unless the buffer is too small (then "truncated": true is set and the
 * document is still closed). Returns bytes written (excluding NUL). */
size_t kn_selftest_render_json(const kn_st_result *results, int n,
                               char *buf, size_t bufsz);

/* Run the tests RELEVANT to a failure (§7.7 step 4) and render the JSON
 * document. Mapping is by case-insensitive substring of op/code:
 *   srtp|auth|crypt|kdf|roc        -> srtp.vectors
 *   fec|shard|repair               -> fec.vectors
 *   video|decode|garbl|corrupt     -> fec.vectors (+decode.synthetic later)
 *   dns|tcp|tls|net|http|wss       -> net.dns, net.tcp443, net.tls, tls.chain
 *   clock|timer|jitter|pacing      -> clock.resolution
 *   console|tui|vt|ansi            -> console.host
 *   (no match)                     -> fec.vectors + srtp.vectors (cheap proofs)
 * Signature matches kn_diag_selftest_provider_fn — wire it with
 * kn_diag_set_selftest_provider(kn_selftest_render_relevant) at startup. */
size_t kn_selftest_render_relevant(const char *op, const char *code,
                                   char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif

#endif /* KN_SELFTEST_H */
