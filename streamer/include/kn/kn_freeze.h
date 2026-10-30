/* kn_freeze.h — round M/N (F22): the freeze-witness ring.
 *
 * WHY THIS MODULE EXISTS: run 33 died the way every dead draw died —
 * the media plane went silent while the control plane kept talking —
 * but run 33's pushed media added the missing witness: for the last
 * ~10 s the SERVER's encoder bitrate ramped 3.1 -> 0.5 Mbps at perfect
 * 30 fps pacing with eight forced IDRs at 0.533 s (docs/RTSP.md §40.4).
 * Our client saw every byte of that ramp and said nothing, because
 * nothing in the receive path looks at ENCODER output rate — gaps,
 * FEC and NACKs are all zero on a clean wind-down. F22 gives the
 * session a 1 Hz ring of video kbps / IDR count / gap count and two
 * pure decisions on it:
 *
 *   kn_fw_decay()  — the 5 s mean is below 60 % of the preceding 60 s
 *                    mean (and the window is loss-free): a rig-side
 *                    wind-down in progress, announced LIVE as
 *                    `bitrate_decay_warn` instead of discovered later
 *                    from a pushed .264;
 *   kn_fw_ramp()   — the last 10 s of the ring rendered for the
 *                    terminal `freeze_witness` line, together with the
 *                    last-media / last-audio / last-control rel_ms the
 *                    pump latches, so the next freeze arrives with its
 *                    cascade order already assembled.
 *
 * The ring holds KN_FW_SECS seconds of one unsigned kbps value, one
 * IDR count and one gap count each. No allocation, no locks: ONE
 * writer (the media-loop thread) and readers only at the 1 Hz beat or
 * at session teardown, same thread.
 *
 * Contract: pure C99, no I/O, no global state; every function here is
 * pinned by test_knfreeze.c against a SECOND, independent
 * recomputation (the test rebuilds the means from a plain array).
 */
#ifndef KN_FREEZE_H
#define KN_FREEZE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_FW_SECS 72u          /* ring depth: 60 s baseline + 5 s window
                                 * + margin                          */
#define KN_FW_BASELINE 60u      /* seconds of baseline before window  */
#define KN_FW_WINDOW 5u         /* seconds of the decay window        */
#define KN_FW_MIN_FILL (KN_FW_BASELINE + KN_FW_WINDOW + 2u)
#define KN_FW_RAMP_SECS 10u     /* seconds rendered by kn_fw_ramp     */

/* 60 % of the baseline mean or less = decay (permille-free integer
 * math: window*100 < 60*baseline). 80 % or more = recovered (rearm). */
#define KN_FW_DECAY_PCT 60u
#define KN_FW_REARM_PCT 80u

/* round N: the baseline gate. Run 34 proved the detector cannot tell
 * starvation from STILLNESS: a loading screen encodes at 100-1100 kbps
 * on its own and every valley cried wolf (ten warns, ten recoveries,
 * zero deaths). A wind-down that matters starts from a healthy stream
 * — run 33's fatal ramp had a 2.7 Mbps baseline — so decay only fires
 * when the 60 s baseline mean is at least this many kbps. */
#define KN_FW_MIN_BASELINE_KBPS 1500u

typedef struct kn_fw {
    unsigned kbps[KN_FW_SECS];
    unsigned idr[KN_FW_SECS];
    unsigned gaps[KN_FW_SECS];
    unsigned head;              /* next write slot                    */
    unsigned fill;              /* valid entries, <= KN_FW_SECS       */
} kn_fw;

void kn_fw_init(kn_fw *fw);

/* One 1 Hz observation: video kilobits this second, IDRs this second,
 * gap-recoveries this second. */
void kn_fw_add(kn_fw *fw, unsigned kbps, unsigned idr, unsigned gaps);

/* The decay decision. Returns 1 when the ring is deep enough and the
 * last KN_FW_WINDOW seconds average strictly below KN_FW_DECAY_PCT %
 * of the KN_FW_BASELINE seconds preceding them, with ZERO gaps in the
 * whole 65 s span (a lossy window is a different family — the rx
 * recovery path already owns it). Writes *avg5 and *avg60 (kbps,
 * integer) when non-NULL and returning 1. Returns 0 (and touches
 * nothing) when the ring is too shallow. */
int kn_fw_decay(const kn_fw *fw, unsigned *avg5, unsigned *avg60);

/* The rearm decision: 1 when computable and the window mean is at or
 * above KN_FW_REARM_PCT % of the baseline mean (gaps irrelevant — a
 * recovery is a recovery). */
int kn_fw_recovered(const kn_fw *fw);

/* Render the last KN_FW_RAMP_SECS seconds oldest-first as two JSON
 * arrays without braces:  "kbps":[a,b,...],"idr":[...],"gaps":[...]
 * Returns the number of characters written (excluding NUL), or -1 if
 * buf is too small (buf is then an empty string). Fewer than
 * KN_FW_RAMP_SECS entries: renders what the ring holds. */
int kn_fw_ramp(const kn_fw *fw, char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif
#endif /* KN_FREEZE_H */
