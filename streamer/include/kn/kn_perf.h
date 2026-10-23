/* kn_perf.h — periodic performance snapshot support (BOOTSTRAP §7.6).
 *
 * Collects the SYSTEM-side counters of the once-per-1000ms `perf` event:
 * process/system CPU %, RSS, peak RSS, handles, threads, timer resolution.
 * The video/net counters are owned by the subsystems that produce them; the
 * loopback orchestrator (knvstd_main) composes the full §7.6 field set into
 * one KN_CH_PERF record (which lands in perf.jsonl by the default jsonl
 * channel mask).
 *
 * CPU-% convention (measured, §6.1): percent OF ALL CORES — 100% means the
 * whole machine is saturated (T1's "24.8 % @ 720p30" uses this convention;
 * kn_decode's cpu_pct_of_core is per-core: divide by ncpu to compare).
 *
 * Platform: Win32 real (GetProcessTimes/GetSystemTimes/K32 memory counters/
 * GetProcessHandleCount/toolhelp thread count/timeGetDevCaps); POSIX stub
 * reports unavailable so the loopback refuses honestly off-target.
 */
#ifndef KN_PERF_H
#define KN_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kn_perf_sys {
    double cpu_process_pct;      /* % of ALL cores, since previous sample */
    double cpu_system_pct;       /* % of ALL cores, machine-wide busy */
    unsigned long long rss_kb;
    unsigned long long peak_rss_kb;
    unsigned handle_count;
    unsigned thread_count;       /* this process */
    unsigned timer_resolution_us; /* timeGetDevCaps wPeriodMin (supported min;
                                     the REAL Sleep floor is clock.resolution's
                                     job — R30: ~2.9 ms on the target) */
    unsigned ncpu;
} kn_perf_sys;

int kn_perf_available(void);     /* 1 on Win32 */
unsigned kn_perf_ncpu(void);     /* 0 when unavailable */

/* Establish the sampling baseline (call once before the first sample).
 * Returns 0 on success, -1 unavailable/failure. */
int kn_perf_init(void);

/* Fill *out with deltas since the previous sample (or since init for the
 * first call). Returns 0, or -1 when unavailable. Cheap (~4 syscalls);
 * safe to call once per second from the presentation thread. */
int kn_perf_sample(kn_perf_sys *out);

#ifdef __cplusplus
}
#endif

#endif /* KN_PERF_H */
