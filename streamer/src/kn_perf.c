/* kn_perf.c — §7.6 system counters (contract: kn_perf.h).
 *
 * Win32 implementation over documented-since-Vista kernel32 entry points
 * (K32 memory counters via PSAPI_VERSION 2 — NO psapi.dll import, the F1
 * import set stays kernel32/user32/winmm/d3d9/msvcrt) + toolhelp for the
 * thread count + winmm timeGetDevCaps. All deltas are computed against the
 * previous sample; the first sample after kn_perf_init measures from init.
 */
#include "kn/kn_perf.h"

#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define PSAPI_VERSION 2          /* GetProcessMemoryInfo -> K32* (kernel32) */
#include <psapi.h>
#include <tlhelp32.h>
#include <mmsystem.h>

static int g_inited;
static unsigned g_ncpu;
static unsigned g_timer_us;
static ULARGE_INTEGER g_proc_prev;      /* process kernel+user, 100 ns */
static ULARGE_INTEGER g_sys_busy_prev;  /* (kernel-idle)+user, 100 ns */
static LARGE_INTEGER g_wall_prev;       /* QPC */
static LARGE_INTEGER g_qpf;

static unsigned long long ft_100ns(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return u.QuadPart;
}

int kn_perf_available(void) { return 1; }

unsigned kn_perf_ncpu(void) { return g_ncpu; }

int kn_perf_init(void)
{
    SYSTEM_INFO si;
    FILETIME cr, ex, pk, pu;
    FILETIME s_idle, s_ker, s_usr;
    TIMECAPS tc;

    if (g_inited) return 0;
    memset(&si, 0, sizeof si);
    GetNativeSystemInfo(&si);
    g_ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
    if (!QueryPerformanceFrequency(&g_qpf) || !g_qpf.QuadPart) return -1;

    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) return -1;
    g_proc_prev.QuadPart = (unsigned long long)ft_100ns(&pk) + ft_100ns(&pu);

    if (GetSystemTimes(&s_idle, &s_ker, &s_usr)) {
        unsigned long long idle = ft_100ns(&s_idle);
        unsigned long long ker = ft_100ns(&s_ker);
        unsigned long long usr = ft_100ns(&s_usr);
        g_sys_busy_prev.QuadPart = (ker > idle ? ker - idle : 0ULL) + usr;
    } else {
        g_sys_busy_prev.QuadPart = 0;
    }
    QueryPerformanceCounter(&g_wall_prev);

    if (timeGetDevCaps(&tc, sizeof tc) == TIMERR_NOERROR && tc.wPeriodMin)
        g_timer_us = (unsigned)tc.wPeriodMin * 1000u;

    g_inited = 1;
    return 0;
}

static unsigned count_threads(void)
{
    HANDLE snap;
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    unsigned n = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&te, 0, sizeof te);
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) ++n;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

int kn_perf_sample(kn_perf_sys *out)
{
    FILETIME cr, ex, pk, pu;
    FILETIME s_idle, s_ker, s_usr;
    LARGE_INTEGER wall_now;
    unsigned long long proc_now, sys_now, wall_100ns;
    PROCESS_MEMORY_COUNTERS pmc;
    DWORD handles = 0;
    double denom;

    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!g_inited && kn_perf_init() != 0) return -1;

    QueryPerformanceCounter(&wall_now);
    {
        long long dq = wall_now.QuadPart - g_wall_prev.QuadPart;
        wall_100ns = (unsigned long long)((double)dq * 10000000.0
                                          / (double)g_qpf.QuadPart);
        g_wall_prev = wall_now;
    }
    denom = (double)wall_100ns * (double)g_ncpu;

    if (GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) {
        proc_now = (unsigned long long)ft_100ns(&pk) + ft_100ns(&pu);
        if (denom > 0.0 && proc_now >= g_proc_prev.QuadPart)
            out->cpu_process_pct = 100.0 *
                (double)(proc_now - g_proc_prev.QuadPart) / denom;
        g_proc_prev.QuadPart = proc_now;
    }
    if (GetSystemTimes(&s_idle, &s_ker, &s_usr)) {
        unsigned long long idle = ft_100ns(&s_idle);
        unsigned long long ker = ft_100ns(&s_ker);
        unsigned long long usr = ft_100ns(&s_usr);
        sys_now = (ker > idle ? ker - idle : 0ULL) + usr;
        if (denom > 0.0 && sys_now >= g_sys_busy_prev.QuadPart)
            out->cpu_system_pct = 100.0 *
                (double)(sys_now - g_sys_busy_prev.QuadPart) / denom;
        g_sys_busy_prev.QuadPart = sys_now;
    }

    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        out->rss_kb = (unsigned long long)(pmc.WorkingSetSize / 1024u);
        out->peak_rss_kb = (unsigned long long)(pmc.PeakWorkingSetSize / 1024u);
    }
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        out->handle_count = (unsigned)handles;
    out->thread_count = count_threads();
    out->timer_resolution_us = g_timer_us;
    out->ncpu = g_ncpu;
    return 0;
}

#else  /* !_WIN32 — honest stub */

int kn_perf_available(void) { return 0; }
unsigned kn_perf_ncpu(void) { return 0; }
int kn_perf_init(void) { return -1; }
int kn_perf_sample(kn_perf_sys *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    return -1;
}

#endif /* _WIN32 */
