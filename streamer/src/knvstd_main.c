/* knvstd_main.c — the KarnyNow streamer executable, pre-stage-1 skeleton.
 *
 * Implements ONLY the §7.9/§7.8 entry points so self-tests and diagnostic
 * bundles are runnable standalone on the target right now:
 *
 *   knvstd --selftest=<name>     run one self-test, print its JSON result
 *   knvstd --selftest=all        run every registered self-test
 *   knvstd --diagnostics         write a diagnostic bundle on demand
 *   knvstd --version             print build.json content
 *   knvstd --log-dir=<dir>       log directory (default "logs"), combinable
 *   knvstd --list-selftests      print the registry as JSON
 *
 * stdout contract: newline-delimited UTF-8 JSON (§3, copying OpenNOW's
 * docs/core-protocol.md discipline: 1 MiB line limit — every document this
 * binary prints is far below it).
 *
 * Exit codes: 0 = everything ok/skipped, 1 = at least one FAILED verdict,
 * 2 = usage error (matches D1's console-exit convention), 3 = internal error.
 *
 * Stage 1 will grow this file into the --loopback/--capture/--replay/--real
 * streamer; the self-test dispatch stays exactly as specified in §7.9
 * ("every self-test must be runnable standalone").
 */
#include "kn/kn_audio.h"
#include "kn/kn_d3d.h"
#include "kn/kn_decode.h"
#include "kn/kn_diag.h"
#include "kn/knfec.h"
#include "kn/kn_perf.h"
#include "kn/kn_rx.h"
#include "kn/kn_selftest.h"
#include "kn/kn_sha1.h"
#include "kn/knlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <direct.h>     /* _mkdir */
#else
#  include <sys/stat.h>   /* mkdir */
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <mmsystem.h>
/* The embedded 320x240 clip is the hermetic fallback source for --loopback
 * (the real source is streamer/assets/synthetic720.264 — 720p High@5.1, the
 * shape the §6.1 CPU budget was measured on). */
#  include "kn_synthetic_clip.h"
#endif

static void usage(void)
{
    printf("knvstd - KarnyNow streamer (stage-1 skeleton)\n"
           "usage:\n"
           "  knvstd --selftest=<name|all> [--log-dir=DIR]\n"
           "  knvstd --loopback[=FILE.264] [--loopback-seconds=N] [--no-audio]\n"
           "  knvstd --replay-capture=FILE.hex [--replay-au-dir=DIR]\n"
           "         [--replay-srtp=PROFILE:TAG:KEYHEX:SALTHEX] [--replay-vps=N]\n"
           "       [--log-dir=DIR]   (WASAPI synthetic PCM unless --no-audio)\n"
           "  knvstd --diagnostics [--log-dir=DIR]\n"
           "  knvstd --list-selftests\n"
           "  knvstd --version\n"
           "loopback source order: FILE.264, ./synthetic720.264, <exe dir>/\n"
           "synthetic720.264, embedded 320x240 clip (budget_valid:false).\n"
           "exit codes: 0 ok/skipped, 1 failed verdict, 2 usage, 3 internal\n");
}

static int print_registry(void)
{
    int i, n = kn_selftest_count();
    printf("{\"selftests\":[");
    for (i = 0; i < n; ++i)
        printf("%s\"%s\"", i ? "," : "", kn_selftest_name(i));
    printf("],\"count\":%d}\n", n);
    return 0;
}

static int log_selftest_result(const kn_st_result *r)
{
    kn_lctx c;
    kn_lctx_init(&c, KN_CH_DIAG,
                 r->status == KN_ST_OK ? KN_INFO :
                 r->status == KN_ST_SKIPPED ? KN_DEBUG : KN_WARN,
                 "selftest_result");
    kn_lctx_str(&c, "name", r->name);
    kn_lctx_str(&c, "status", kn_selftest_status_name(r->status));
    kn_lctx_i64(&c, "ms", r->ms);
    kn_lctx_i64(&c, "checks", r->checks);
    kn_lctx_i64(&c, "failures", r->failures);
    kn_lctx_str(&c, "reason", r->reason);
    kn_log_emit(&c);
    return r->status == KN_ST_FAILED ? 1 : 0;
}

static int run_selftests(const char *which, const char *log_dir)
{
    static kn_st_result results[32];
    char doc[16384];
    size_t n_doc;
    int n = 0, i, worst = 0;
    kn_log_config cfg;

    /* logging so the run leaves evidence even if stdout is lost (§7.12) */
    memset(&cfg, 0, sizeof cfg);
    cfg.log_dir = log_dir;
    (void)kn_log_init(&cfg);          /* failure is tolerated: tests still run */
    kn_diag_set_selftest_provider(kn_selftest_render_relevant);

    if (strcmp(which, "all") == 0) {
        /* Run one-by-one (same order as kn_selftest_run_all) and log EVERY
         * result + a begin marker IMMEDIATELY: INFO records are soft-flushed
         * (§7.12), so if a later test kills the process (an uncaught driver
         * fault), diag.log still names the exact test that was running —
         * the T4/R2b "evidence survives the crash" rule applied here. */
        int total = kn_selftest_count();
        for (i = 0; i < total && n < (int)(sizeof results / sizeof results[0]);
             ++i) {
            const char *nm = kn_selftest_name(i);
            {
                kn_lctx b;
                kn_lctx_init(&b, KN_CH_DIAG, KN_INFO, "selftest_begin");
                kn_lctx_str(&b, "name", nm);
                kn_log_emit(&b);      /* INFO -> soft flush: survives a later AV */
            }
            if (kn_selftest_run(nm, &results[n]) == 0) {
                if (log_selftest_result(&results[n]) &&
                    kn_selftest_is_gated(nm)) worst = 1;
                n++;
            }
        }
    } else {
        if (kn_selftest_run(which, &results[0]) != 0) {
            printf("{\"error\":\"unknown_selftest\",\"name\":\"%s\"}\n", which);
            kn_log_shutdown();
            return 2;
        }
        n = 1;
        if (log_selftest_result(&results[0])) worst = 1;
    }

    n_doc = kn_selftest_render_json(results, n, doc, sizeof doc);
    if (n_doc) fwrite(doc, 1, n_doc, stdout);
    fputc('\n', stdout);

    fflush(stdout);
    kn_log_shutdown();
    return worst ? 1 : 0;
}

static int run_diagnostics(const char *log_dir)
{
    char path[KN_DIAG_PATH_MAX];
    int rc;
    kn_log_config cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.log_dir = log_dir;
    (void)kn_log_init(&cfg);
    kn_diag_set_selftest_provider(kn_selftest_render_relevant);

    path[0] = '\0';
    rc = kn_diag_write_bundle(log_dir, "manual", "E_NONE",
                              "--diagnostics requested", path, sizeof path);
    if (rc == 0) {
        printf("{\"bundle\":\"%s\"}\n", path);
        fflush(stdout);
        kn_log_shutdown();
        return 0;
    }
    printf("{\"error\":\"bundle_write_failed\",\"rc\":%d}\n", rc);
    fflush(stdout);
    kn_log_shutdown();
    return 3;
}

/* ================================================================ loopback
 * Stage-1 offline video path (BOOTSTRAP §12 stage 1, G1): decode an H.264
 * source (file or the embedded clip), present every frame through the D7
 * rung-1 chain (NV12 DEFAULT -> per-row copy -> StretchRect(LINEAR) ->
 * letterboxed backbuffer), paced to exactly 30 fps, emitting the §7.6 perf
 * snapshot every 1000 ms. NO network, NO audio yet (WASAPI is the next
 * delta). Single-threaded by design: decode is 5-7x faster than the pace
 * (T1: 211 fps 720p decode vs 30 fps present), so pacing INSIDE the decode
 * callback needs no queue — queue_video_depth reports an honest 0.
 *
 * Pacing (D9): deadlines are absolute (t0 + n*1e6/30 us) so errors never
 * accumulate; the wait is Sleep(deadline-3ms) + QPC spin because the
 * target's real Sleep(1) floor is ~2.9 ms (R30, measured rounds 4-6).
 * A frame whose present STARTS after its deadline is counted in
 * frames_dropped (late) — loopback never skips a frame on purpose.
 *
 * Measured facts this code is built on (T10 rounds 5-7):
 *   - NV12 1280x720 DEFAULT locks with pitch 1536 != width -> the copy is
 *     PER ROW (kn_d3d_vp_present_nv12), never one memcpy;
 *   - first touch of a fresh surface costs ~28 ms once -> vp_open pre-faults
 *     (fill + one present) BEFORE t0 starts;
 *   - StretchRect+Present ~0.3-3 ms, decode 720p ~16.5 ms CPU/frame
 *     (24.8% of both cores at 30 fps; G1 budget <= ~45%).
 */

#ifdef _WIN32

#define LB_PERF_SAMPLES 160          /* >= 30 fps x 1 s + headroom */
#define LB_LATE_GRACE_US 2000        /* a frame counts as late when its
                                      * present STARTS more than this after
                                      * the deadline (frame 0's deadline is
                                      * t0 itself — the grace keeps the
                                      * callback-entry overhead of the very
                                      * first frame from counting as late) */

static long long lb_now_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER pc;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&pc);
    if (!freq.QuadPart) return 0;
    return (long long)((double)pc.QuadPart * 1000000.0 / (double)freq.QuadPart);
}

static void lb_wait_until(long long deadline_us)
{
    long long rem = deadline_us - lb_now_us();
    if (rem > 4000)
        Sleep((DWORD)((rem - 3000) / 1000));  /* R30: wake ~3 ms early... */
    while (lb_now_us() < deadline_us) { }      /* ...then QPC-spin the tail */
}

typedef struct {
    long long duration_us;
    /* source */
    char src_path[512];
    int  src_is_file;            /* 0 = embedded clip fallback */
    int  budget_valid;           /* source is the 720p budget shape */
    unsigned src_w, src_h;
    /* pacing / counters */
    long long t0_us, end_us, next_perf_us, decode_exit_us;
    unsigned long long presented, decoded, late;
    /* presenter */
    kn_d3d_vp *vp;
    int stop, vp_rc, decode_rc;
    const char *stopped_reason;
    /* audio (WASAPI synthetic — completes stage 1) */
    kn_audio *au;
    int audio_off;                 /* --no-audio (debug only; G1 needs it) */
    char audio_err[128];
    /* perf-interval sample buffers */
    unsigned long long perf_seq, perf_lines;
    unsigned long long last_decoded, last_presented, last_late;
    long long perf_t_us;                 /* start of the current interval */
    double decode_ms[LB_PERF_SAMPLES];  unsigned decode_n, decode_ovf;
    double present_ms[LB_PERF_SAMPLES]; unsigned present_n, present_ovf;
    /* run aggregates (summary) */
    double decode_sum, decode_max, present_sum, present_max, copy_sum;
    unsigned long long decode_cnt, present_cnt;
    double cpu_sum, cpu_max;
    unsigned cpu_n;
    unsigned long long rss_first, rss_last, rss_peak;
    int rss_first_set;
    unsigned refresh_initial, refresh_last;
    int dpst_seen;
    int loops;
} lb_ctx;

static double lb_p95(double *v, unsigned n)
{
    unsigned i, j, idx;
    if (n == 0) return 0.0;
    for (i = 1; i < n; ++i) {             /* insertion sort, n <= 160 */
        double k = v[i];
        for (j = i; j > 0 && v[j - 1] > k; --j) v[j] = v[j - 1];
        v[j] = k;
    }
    idx = (unsigned)((double)n * 0.95);
    if (idx >= n) idx = n - 1;
    return v[idx];
}

/* One §7.6 perf event (26 fields; the net/fec group reports honest zeros
 * until stage 2 wires them — the SCHEMA is stable from day one). */
static void lb_emit_perf(lb_ctx *c)
{
    kn_perf_sys s;
    kn_lctx l;
    double dp95, pp95;
    unsigned hz;

    if (kn_perf_sample(&s) != 0) memset(&s, 0, sizeof s);
    dp95 = lb_p95(c->decode_ms, c->decode_n);
    pp95 = lb_p95(c->present_ms, c->present_n);
    c->decode_n = 0;
    c->present_n = 0;

    hz = c->vp ? kn_d3d_vp_refresh_hz(c->vp) : 0u;
    if (hz) {
        c->refresh_last = hz;
        if (c->refresh_initial && hz != c->refresh_initial) c->dpst_seen = 1;
    }

    kn_lctx_init(&l, KN_CH_PERF, KN_INFO, "perf");
    kn_lctx_u64(&l, "seq", c->perf_seq++);
    kn_lctx_f64(&l, "cpu_process_pct", s.cpu_process_pct);
    kn_lctx_f64(&l, "cpu_system_pct", s.cpu_system_pct);
    kn_lctx_u64(&l, "rss_kb", s.rss_kb);
    kn_lctx_u64(&l, "peak_rss_kb", s.peak_rss_kb);
    kn_lctx_u64(&l, "handle_count", s.handle_count);
    kn_lctx_u64(&l, "thread_count", s.thread_count);
    kn_lctx_u64(&l, "timer_resolution_us", s.timer_resolution_us);
    kn_lctx_u64(&l, "frames_decoded", c->decoded - c->last_decoded);
    kn_lctx_u64(&l, "frames_presented", c->presented - c->last_presented);
    kn_lctx_u64(&l, "frames_dropped", c->late - c->last_late);
    kn_lctx_f64(&l, "decode_ms_p95", dp95);
    kn_lctx_f64(&l, "present_ms_p95", pp95);
    kn_lctx_u64(&l, "queue_video_depth", 0);
    kn_lctx_i64(&l, "queue_audio_ms",
                c->au ? (long long)kn_audio_queue_ms(c->au) : 0);
    kn_lctx_u64(&l, "net_rx_bytes", 0);
    kn_lctx_u64(&l, "net_rx_packets", 0);
    kn_lctx_u64(&l, "net_rx_dropped", 0);
    kn_lctx_u64(&l, "rtt_ms", 0);
    kn_lctx_f64(&l, "loss_pct", 0.0);
    kn_lctx_u64(&l, "fec_repaired", 0);
    kn_lctx_u64(&l, "nack_sent", 0);
    kn_lctx_u64(&l, "nack_repaired", 0);
    kn_lctx_u64(&l, "idr_requested", 0);
    kn_lctx_u64(&l, "refresh_hz_measured", hz);
    kn_lctx_u64(&l, "dpst_change_detected", (unsigned long long)c->dpst_seen);
    kn_log_emit(&l);

    {   /* D20: the overlay shows the SAME number this event reports —
         * frames presented per second of the completed interval, computed
         * from the MEASURED interval wall time (never assumed 1.000 s). */
        unsigned long long dp = c->presented - c->last_presented;
        unsigned long long dl = c->late - c->last_late;
        long long now = lb_now_us();
        long long iv = now - c->perf_t_us;
        unsigned long long fps10 = iv > 0 ? (dp * 10ULL * 1000000ULL
                                             / (unsigned long long)iv) : 0ULL;
        char t[64];
        if (dl)
            snprintf(t, sizeof t, "FPS %llu.%llu LATE %llu",
                     fps10 / 10ULL, fps10 % 10ULL, dl);
        else
            snprintf(t, sizeof t, "FPS %llu.%llu", fps10 / 10ULL,
                     fps10 % 10ULL);
        if (c->vp) (void)kn_d3d_vp_set_overlay_text(c->vp, t);
        c->perf_t_us = now;
    }

    c->last_decoded = c->decoded;
    c->last_presented = c->presented;
    c->last_late = c->late;
    c->perf_lines++;
    if (s.rss_kb) {
        if (!c->rss_first_set) { c->rss_first = s.rss_kb; c->rss_first_set = 1; }
        c->rss_last = s.rss_kb;
        if (s.rss_kb > c->rss_peak) c->rss_peak = s.rss_kb;
    }
    c->cpu_sum += s.cpu_process_pct;
    c->cpu_n++;
    if (s.cpu_process_pct > c->cpu_max) c->cpu_max = s.cpu_process_pct;
}

static int lb_frame_cb(const unsigned char *data, int w, int h,
                       int stride_y, int stride_uv, int is_nv12,
                       long long idx, void *ud)
{
    lb_ctx *c = (lb_ctx *)ud;
    long long now, deadline;
    double copy_ms = 0.0, sp_ms = 0.0, dt;
    int rc;
    (void)idx;

    now = lb_now_us();
    /* decode cost per frame = gap between callbacks. The pacing wait runs
     * INSIDE this callback (measured separately), and the decoder is
     * CPU-bound between callbacks, so the gap is the decode+convert cost
     * (wall approximation of §6.1's CPU ms/frame). */
    if (c->decode_exit_us) {
        dt = (double)(now - c->decode_exit_us) / 1000.0;
        if (dt >= 0.0 && dt < 1000.0) {
            c->decode_sum += dt;
            c->decode_cnt++;
            if (dt > c->decode_max) c->decode_max = dt;
            if (c->decode_n < LB_PERF_SAMPLES) c->decode_ms[c->decode_n++] = dt;
            else c->decode_ovf++;
        }
    }
    c->decoded++;

    if (!c->vp) {
        if (!is_nv12 || w <= 0 || h <= 0) {
            c->stop = 1;
            c->vp_rc = KN_D3D_ERR_ARGS;
            c->stopped_reason = "decoder did not deliver NV12";
            return 1;
        }
        c->src_w = (unsigned)w;
        c->src_h = (unsigned)h;
        c->budget_valid = (w == 1280 && h == 720);
        c->vp = kn_d3d_vp_open((unsigned)w, (unsigned)h,
                               "KarnyNow loopback (G1)");
        if (!c->vp) {
            c->stop = 1;
            c->vp_rc = KN_D3D_ERR_INIT;
            c->stopped_reason = "presenter_open_failed";
            return 1;
        }
        c->refresh_initial = kn_d3d_vp_refresh_hz(c->vp);
        /* t0 starts AFTER the pre-fault present: the paced loop must not
         * inherit first-touch costs (measured ~28 ms once, round 5). */
        c->t0_us = lb_now_us();
        c->end_us = c->t0_us + c->duration_us;
        c->next_perf_us = c->t0_us + 1000000;
        c->perf_t_us = c->t0_us;
        /* WASAPI synthetic PCM (stage 1: "Plus WASAPI with synthetic PCM").
         * Open failure does NOT kill the video loop — it is reported via
         * §7.7 amplification + the summary, and T11 gates the full-G1
         * verdict on audio being open. */
        if (!c->audio_off) {
            c->au = kn_audio_open(440, c->audio_err, sizeof c->audio_err);
            if (!c->au)
                kn_log_on_failure("session.loopback.audio", "E_AUDIO_OPEN",
                                  c->audio_err,
                                  "video continues; full G1 requires audio — "
                                  "check the default render device");
        }
    }
    if ((unsigned)w != c->src_w || (unsigned)h != c->src_h) {
        c->stop = 1;                       /* v1: fixed dims (clip is CFR) */
        c->vp_rc = KN_D3D_ERR_DIMS;
        c->stopped_reason = "dimensions_changed_mid_stream";
        return 1;
    }
    if (now >= c->end_us) {
        c->stop = 1;
        c->stopped_reason = "elapsed";
        return 1;
    }

    deadline = c->t0_us + (long long)c->presented * 1000000LL / 30LL;
    now = lb_now_us();
    if (now > deadline + LB_LATE_GRACE_US) c->late++;
    else lb_wait_until(deadline);

    rc = kn_d3d_vp_present_nv12(c->vp, data, (unsigned)stride_y,
                                data + (size_t)stride_y * (size_t)h,
                                (unsigned)stride_uv, &copy_ms, &sp_ms);
    if (rc != KN_D3D_OK) {
        c->stop = 1;
        c->vp_rc = rc;
        c->stopped_reason = (rc == KN_D3D_ERR_CLOSED) ? "window_closed"
                          : (rc == KN_D3D_ERR_FAULT)  ? "presenter_fault"
                          : (rc == KN_D3D_ERR_LOST)   ? "presenter_lost"
                                                      : "presenter_error";
        return 1;
    }
    dt = copy_ms + sp_ms;
    c->present_sum += dt;
    c->present_cnt++;
    if (dt > c->present_max) c->present_max = dt;
    c->copy_sum += copy_ms;
    if (c->present_n < LB_PERF_SAMPLES) c->present_ms[c->present_n++] = dt;
    else c->present_ovf++;
    c->presented++;

    if (kn_d3d_vp_pump(c->vp) == KN_D3D_ERR_CLOSED) {
        c->stop = 1;
        c->stopped_reason = "window_closed";
        return 1;
    }
    for (;;) {                             /* perf due? (catch up after stalls) */
        long long t = lb_now_us();
        if (t < c->next_perf_us) break;
        lb_emit_perf(c);
        c->next_perf_us += 1000000;
        if (c->next_perf_us < t) c->next_perf_us = t + 1000000;  /* resync */
    }
    c->decode_exit_us = lb_now_us();
    return 0;
}

static int lb_file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Resolve the loopback source: explicit arg > ./synthetic720.264 >
 * <exe dir>/synthetic720.264 > embedded 320x240 (budget_valid:false). */
static void lb_resolve_source(lb_ctx *c, const char *arg)
{
    if (arg && arg[0]) {
        snprintf(c->src_path, sizeof c->src_path, "%s", arg);
        c->src_is_file = 1;
        return;
    }
    if (lb_file_exists("synthetic720.264")) {
        snprintf(c->src_path, sizeof c->src_path, "synthetic720.264");
        c->src_is_file = 1;
        return;
    }
    {
        char exe[512];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe - 32);
        if (n > 0 && n < sizeof exe - 32) {
            char *slash = strrchr(exe, '\\');
            if (!slash) slash = strrchr(exe, '/');
            if (slash) {
                snprintf(slash + 1, sizeof exe - (size_t)(slash + 1 - exe),
                         "synthetic720.264");
                if (lb_file_exists(exe)) {
                    snprintf(c->src_path, sizeof c->src_path, "%s", exe);
                    c->src_is_file = 1;
                    return;
                }
            }
        }
    }
    c->src_is_file = 0;                    /* embedded fallback */
}

static void lb_json_path(char *dst, size_t dstsz, const char *src)
{
    size_t i, o = 0;
    for (i = 0; src[i] && o + 2 < dstsz; ++i) {
        if (src[i] == '\\' || src[i] == '/') dst[o++] = '/';
        else if (src[i] == '"') { dst[o++] = '\\'; dst[o++] = '"'; }
        else if ((unsigned char)src[i] < 0x20) dst[o++] = '?';
        else dst[o++] = src[i];
    }
    dst[o] = '\0';
}

static int run_loopback(const char *src_arg, int seconds, int no_audio,
                        const char *log_dir)
{
    lb_ctx c;
    kn_log_config cfg;
    kn_decode_stats st;
    char jpath[600];
    double secs_run, fps_mean;
    int clean;

    memset(&c, 0, sizeof c);
    c.duration_us = (long long)seconds * 1000000LL;
    c.stopped_reason = "init_failed";
    c.audio_off = no_audio;

    memset(&cfg, 0, sizeof cfg);
    cfg.log_dir = log_dir;
    (void)kn_log_init(&cfg);
    kn_diag_set_selftest_provider(kn_selftest_render_relevant);
    (void)kn_diag_install(log_dir);       /* §7.7: auto bundle on failure */

    lb_resolve_source(&c, src_arg);
    if (c.src_is_file && !lb_file_exists(c.src_path)) {
        printf("{\"mode\":\"loopback\",\"error\":\"source_unreadable\","
               "\"source\":\"%s\"}\n", c.src_path);
        fflush(stdout);
        kn_log_shutdown();
        return 1;
    }
    if (!c.src_is_file) {
        kn_lctx w;
        kn_lctx_init(&w, KN_CH_MEDIA, KN_WARN, "loopback_source_fallback");
        kn_lctx_str(&w, "reason", "synthetic720.264 not found — using the "
                    "EMBEDDED 320x240 clip; CPU numbers do NOT validate the "
                    "720p budget (budget_valid:false)");
        kn_log_emit(&w);
    }
    if (!kn_decode_available() || !kn_d3d_available()) {
        printf("{\"mode\":\"loopback\",\"error\":\"build_cannot_loopback\","
               "\"decode\":%d,\"d3d\":%d}\n", kn_decode_available(),
               kn_d3d_available());
        fflush(stdout);
        kn_log_shutdown();
        return 3;
    }

    {
        kn_lctx b;
        kn_lctx_init(&b, KN_CH_APP, KN_INFO, "loopback_begin");
        kn_lctx_str(&b, "source", c.src_is_file ? c.src_path : "<embedded>");
        kn_lctx_i64(&b, "seconds", seconds);
        kn_log_emit(&b);
    }
    timeBeginPeriod(1);
    while (!c.stop) {
        int rc;
        memset(&st, 0, sizeof st);
        if (c.src_is_file)
            rc = kn_decode_h264_file(c.src_path, 1, lb_frame_cb, &c, &st);
        else
            rc = kn_decode_h264_mem(k_synthetic_clip, KN_SYNTH_CLIP_LEN, 1,
                                    lb_frame_cb, &c, &st);
        c.loops++;
        if (c.stop) break;
        if (rc == 0) {
            if (c.decoded == 0) {          /* clip decoded to nothing */
                c.stop = 1;
                c.decode_rc = rc;
                c.stopped_reason = "decode_error";
                break;
            }
            continue;                      /* loop the clip until end_us */
        }
        if (rc == -4) {                    /* callback abort: stop flag set */
            if (!c.stop) { c.stop = 1; c.stopped_reason = "elapsed"; }
            break;
        }
        c.stop = 1;
        c.decode_rc = rc;
        c.stopped_reason = "decode_error";
        break;
    }
    timeEndPeriod(1);

    /* final partial perf interval (keeps perf_lines ~ seconds_run) */
    if (c.decode_n || c.present_n) lb_emit_perf(&c);

    clean = (c.decoded > 0 && c.presented > 0 &&
             strcmp(c.stopped_reason, "elapsed") == 0 &&
             c.vp_rc == 0 && c.decode_rc == 0 &&
             (c.audio_off || c.au != NULL) &&
             !kn_d3d_permanently_degraded());
    if (!clean) {
        char code[32];
        snprintf(code, sizeof code, "E_LOOPBACK_%d_%d",
                 c.vp_rc % 1000, c.decode_rc % 1000);
        kn_log_on_failure("session.loopback", code, c.stopped_reason,
                          "inspect perf.jsonl + media.log; the auto-bundle "
                          "path is in this log dir");
    }

    secs_run = (double)((c.t0_us ? (lb_now_us() - c.t0_us) : 0)) / 1000000.0;
    fps_mean = secs_run > 0.0 ? (double)c.presented / secs_run : 0.0;
    lb_json_path(jpath, sizeof jpath,
                 c.src_is_file ? c.src_path : "<embedded-320x240>");
    printf("{\"mode\":\"loopback\",\"source\":\"%s\",\"source_w\":%u,"
           "\"source_h\":%u,\"budget_valid\":%s,\"seconds_requested\":%d,"
           "\"seconds_run\":%.2f,\"loops\":%d,\"frames_decoded\":%llu,"
           "\"frames_presented\":%llu,\"frames_late\":%llu,"
           "\"fps_mean\":%.3f,\"decode_ms_mean\":%.2f,\"decode_ms_max\":%.2f,"
           "\"present_ms_mean\":%.2f,\"present_ms_max\":%.2f,"
           "\"copy_ms_mean\":%.2f,\"cpu_process_pct_mean\":%.1f,"
           "\"cpu_process_pct_max\":%.1f,\"cpu_convention\":\"pct_of_all_cores\","
           "\"rss_kb_first\":%llu,\"rss_kb_last\":%llu,\"rss_kb_peak\":%llu,"
           "\"refresh_hz_initial\":%u,\"refresh_hz_last\":%u,"
           "\"dpst_change_detected\":%d,\"perf_lines\":%llu,"
           "\"stopped_reason\":\"%s\",\"vp_rc\":%d,\"decode_rc\":%d,"
           "\"fault\":\"0x%08lx\",\"fault_addr\":\"0x%016llx\"",
           jpath, c.src_w % 100000u, c.src_h % 100000u,
           c.budget_valid ? "true" : "false", seconds % 100000,
           secs_run, c.loops % 1000000, c.decoded, c.presented, c.late,
           fps_mean,
           c.decode_cnt ? c.decode_sum / (double)c.decode_cnt : 0.0,
           c.decode_max,
           c.present_cnt ? c.present_sum / (double)c.present_cnt : 0.0,
           c.present_max,
           c.present_cnt ? c.copy_sum / (double)c.present_cnt : 0.0,
           c.cpu_n ? c.cpu_sum / (double)c.cpu_n : 0.0, c.cpu_max,
           c.rss_first, c.rss_last, c.rss_peak,
           c.refresh_initial % 100000u, c.refresh_last % 100000u,
           c.dpst_seen, c.perf_lines, c.stopped_reason, c.vp_rc, c.decode_rc,
           c.vp ? kn_d3d_vp_fault_code(c.vp) : 0ul,
           c.vp ? kn_d3d_vp_fault_addr(c.vp) : 0ull);
    /* audio sub-object — the WASAPI half of stage 1 (full G1) */
    if (c.audio_off) {
        printf(",\"audio\":{\"opened\":false,\"reason\":\"disabled\"}");
    } else if (!c.au) {
        char aerr[160];
        lb_json_path(aerr, sizeof aerr, c.audio_err);
        printf(",\"audio\":{\"opened\":false,\"reason\":\"%s\"}", aerr);
    } else {
        kn_audio_stats as;
        char fdesc[64];
        const kn_audio_fmt *af = kn_audio_format(c.au);
        (void)kn_audio_get_stats(c.au, &as);
        kn_audio_fmt_describe(af, fdesc, sizeof fdesc);
        printf(",\"audio\":{\"opened\":true,\"format\":\"%s\",\"events\":%llu,"
               "\"frames_written\":%llu,\"expected_frames\":%llu,"
               "\"starved\":%llu,\"wait_timeouts\":%llu,"
               "\"buffer_errors\":%llu,\"queue_ms_last\":%u}",
               fdesc, as.events, as.frames_written,
               af ? (unsigned long long)((double)af->rate * secs_run) : 0ull,
               as.starved, as.wait_timeouts, as.buffer_errors,
               as.queue_ms % 100000u);
    }
    {
        const char *bundle = kn_diag_last_bundle_path();
        if (bundle && bundle[0]) {
            char jb[600];
            lb_json_path(jb, sizeof jb, bundle);
            printf(",\"bundle\":\"%s\"", jb);
        }
    }
    printf("}\n");
    fflush(stdout);

    kn_audio_close(c.au);
    kn_d3d_vp_close(c.vp);
    kn_log_shutdown();
    return clean ? 0 : 1;
}

#else  /* !_WIN32 */

static int run_loopback(const char *src_arg, int seconds, int no_audio,
                        const char *log_dir)
{
    (void)src_arg; (void)seconds; (void)no_audio; (void)log_dir;
    printf("{\"mode\":\"loopback\",\"error\":\"loopback_requires_win32\","
           "\"detail\":\"D3D9 presentation is Win32-only; run on the target\"}"
           "\n");
    fflush(stdout);
    return 3;
}

#endif /* _WIN32 */

/* ------------------------------------------------------------ replay mode
 * knvstd --replay-capture=FILE.hex — the stage-2 OFFLINE transport gate:
 * feed a kn_ring_dump hex capture (tools/make_rx_capture.py, or a real
 * stage-2 receive-loop dump) through the FULL kn_rx pipeline (SRTP when
 * material is given, reorder, FEC repair, AU assembly, RFI/NACK logic) on
 * the capture's own millisecond timeline. Emits one JSON line per assembled
 * access unit (fi/len/sha1/keyframe/contiguous — the G2 ffprobe story:
 * --replay-au-dir writes the AUs as .264 files) and a final summary JSON.
 * Portable: runs identically on the host (sandbox proofs) and the target.
 */
typedef struct rp_ctx {
    const char *au_dir;
    unsigned long aus, key_aus, write_errors;
    unsigned long long bytes;
} rp_ctx;

static void rp_frame_cb(const kn_rx_frame *f, void *ud)
{
    rp_ctx *c = (rp_ctx *)ud;
    uint8_t dig[KN_SHA1_DIGEST_LEN];
    char hex[2 * KN_SHA1_DIGEST_LEN + 1];
    int i;
    kn_sha1(f->bytes, f->bytes_len, dig);
    for (i = 0; i < KN_SHA1_DIGEST_LEN; ++i)
        sprintf(hex + 2 * i, "%02x", (unsigned)dig[i]);
    printf("{\"mode\":\"replay\",\"event\":\"au\",\"fi\":%u,\"len\":%lu,"
           "\"sha1\":\"%s\",\"keyframe\":%d,\"contiguous\":%d,\"ts\":%u,"
           "\"wire\":%u}\n",
           (unsigned)(f->frame_index % 100000000u),
           (unsigned long)(f->bytes_len % 100000000ul), hex,
           f->keyframe ? 1 : 0, f->contiguous ? 1 : 0,
           (unsigned)(f->timestamp % 100000000u),
           (unsigned)(f->frame_type_wire % 1000u));
    c->aus++;
    if (f->keyframe) c->key_aus++;
    c->bytes += (unsigned long long)f->bytes_len;
    if (c->au_dir && c->au_dir[0]) {
        char path[1200];
        FILE *fp;
        int n = snprintf(path, sizeof path, "%s/au_%05u_%c.264", c->au_dir,
                         (unsigned)(f->frame_index % 100000u),
                         f->keyframe ? 'k' : 'p');
        if (n <= 0 || (size_t)n >= sizeof path) { c->write_errors++; return; }
        fp = fopen(path, "wb");
        if (!fp) { c->write_errors++; return; }
        if (f->bytes_len &&
            fwrite(f->bytes, 1, f->bytes_len, fp) != f->bytes_len)
            c->write_errors++;
        fclose(fp);
    }
}

static int rp_hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int rp_hexdecode(const char *s, uint8_t *out, size_t out_cap,
                        size_t *out_len)
{
    size_t n = 0;
    while (s[0] && s[1]) {
        int hi = rp_hexval(s[0]), lo = rp_hexval(s[1]);
        if (hi < 0 || lo < 0) return -1;
        if (n >= out_cap) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    *out_len = n;
    return 0;
}

/* PROFILE:TAG:KEYHEX:SALTHEX  (profile: none|cm128|cm256|gcm128|gcm256) */
static int rp_parse_srtp(const char *spec, int *profile, size_t *tag,
                         uint8_t *key, size_t *key_len,
                         uint8_t *salt, size_t *salt_len)
{
    char buf[512];
    char *part[4];
    int i;
    size_t n;
    if (!spec || strlen(spec) >= sizeof buf) return -1;
    memcpy(buf, spec, strlen(spec) + 1);
    part[0] = buf;
    for (i = 1; i < 4; ++i) {
        char *c = strchr(part[i - 1], ':');
        if (!c) return -1;
        *c = '\0';
        part[i] = c + 1;
    }
    if (strcmp(part[0], "none") == 0)       *profile = KN_RX_SRTP_NONE;
    else if (strcmp(part[0], "cm128") == 0) *profile = KN_RX_SRTP_CM128_HMAC_SHA1;
    else if (strcmp(part[0], "cm256") == 0) *profile = KN_RX_SRTP_CM256_HMAC_SHA1;
    else if (strcmp(part[0], "gcm128") == 0)*profile = KN_RX_SRTP_GCM128;
    else if (strcmp(part[0], "gcm256") == 0)*profile = KN_RX_SRTP_GCM256;
    else return -1;
    {
        long t = strtol(part[1], NULL, 10);
        if (t <= 0 || t > 16) return -1;
        *tag = (size_t)t;
    }
    if (*profile == KN_RX_SRTP_NONE) { *key_len = 0; *salt_len = 0; return 0; }
    if (rp_hexdecode(part[2], key, 32, &n) != 0) return -1;
    *key_len = n;
    if (rp_hexdecode(part[3], salt, 14, &n) != 0) return -1;
    *salt_len = n;
    return 0;
}

#define RP_LINE_CAP  262144
#define RP_PKT_CAP   65600

static int run_replay(const char *cap_path, const char *au_dir,
                      const char *srtp_spec, long vps_override,
                      const char *log_dir)
{
    static char line[RP_LINE_CAP];
    static uint8_t pkt[RP_PKT_CAP];
    static rp_ctx rc_ctx;
    kn_rx_video v;
    kn_rx_mem mem;
    kn_rx_video_config cfg;
    kn_rx_srtp_session sess;
    uint8_t *arena = NULL;
    FILE *f = NULL;
    unsigned long records = 0, bad_lines = 0, linecap = 0;
    uint32_t max_rec = 0;
    uint64_t t_last_ms = 0;
    int profile = KN_RX_SRTP_NONE, rc, exit_code = 3;
    size_t tag_len = 0, key_len = 0, salt_len = 0;
    uint8_t key[32], salt[14];
    uint32_t slots, pkt_cap, shard_len, vps, scratch_len;
    size_t arena_need, off;
    kn_log_config lcfg;

    memset(&rc_ctx, 0, sizeof rc_ctx);
    rc_ctx.au_dir = au_dir;
    if (au_dir && au_dir[0]) {
        /* best-effort create (the summary's au_write_errors is the honest
         * verdict when this fails — no silent fallback) */
#ifdef _WIN32
        (void)_mkdir(au_dir);
#else
        (void)mkdir(au_dir, 0777);
#endif
    }
    memset(&lcfg, 0, sizeof lcfg);
    lcfg.log_dir = log_dir;
    (void)kn_log_init(&lcfg);
    kn_diag_set_selftest_provider(kn_selftest_render_relevant);
    (void)kn_diag_install(log_dir);       /* §7.7: auto bundle on failure */

    if (srtp_spec && srtp_spec[0]) {
        if (rp_parse_srtp(srtp_spec, &profile, &tag_len, key, &key_len,
                          salt, &salt_len) != 0) {
            printf("{\"mode\":\"replay\",\"error\":\"bad_srtp_spec\"}\n");
            fflush(stdout);
            kn_log_shutdown();
            return 2;
        }
    }

    f = fopen(cap_path, "rb");
    if (!f) {
        printf("{\"mode\":\"replay\",\"error\":\"capture_unreadable\","
               "\"path\":\"%.300s\"}\n", cap_path);
        fflush(stdout);
        kn_log_shutdown();
        return 1;
    }
    /* pass 1: record count + the largest record (sizes the pool) */
    while (fgets(line, (int)sizeof line, f)) {
        kn_gs_dump_rec rec;
        size_t plen = 0;
        if (!line[0] || line[0] == '\n') continue;
        if (kn_gs_hexdump_parse_line(line, &rec, pkt, sizeof pkt, &plen)
            != KN_GS_OK) {
            bad_lines++;
            continue;
        }
        records++;
        if (plen > max_rec) max_rec = (uint32_t)plen;
        if (strlen(line) + 1 >= sizeof line) linecap++;  /* truncated line */
    }
    if (records == 0 || max_rec < 12) {
        printf("{\"mode\":\"replay\",\"error\":\"capture_empty_or_invalid\","
               "\"records\":%lu,\"bad_lines\":%lu}\n",
               records % 100000000ul, bad_lines % 100000000ul);
        fflush(stdout);
        fclose(f);
        kn_log_shutdown();
        return 1;
    }

    /* config: vps from the override or the largest PLAINTEXT record (a
     * full-size capture packet IS shard_len; see make_rx_capture.py
     * manifest). With an SRTP profile the records carry the auth tag on
     * the wire — deriving vps from the protected size would inflate
     * shard_len by tag_len and every FEC-recovered packet would grow
     * trailing garbage (found by the E/F capture matrix). */
    {
        uint32_t plain_max = max_rec;
        if (profile != KN_RX_SRTP_NONE && max_rec > (uint32_t)tag_len)
            plain_max = max_rec - (uint32_t)tag_len;
        vps = (vps_override > 0) ? (uint32_t)vps_override
                                 : (plain_max >= 16u ? plain_max - 16u
                                                     : 1280u);
    }
    if (vps < KN_RX_MIN_VIDEO_PACKET_SIZE) vps = KN_RX_MIN_VIDEO_PACKET_SIZE;
    if (vps > KN_RX_MAX_VIDEO_PACKET_SIZE) vps = KN_RX_MAX_VIDEO_PACKET_SIZE;
    shard_len = vps + KN_RX_FEC_HEADER_ALLOWANCE;
    pkt_cap = (max_rec > shard_len ? max_rec : shard_len) + 64u;
    /* every arena region starts 8-byte aligned (the pool byte region size
     * is slots*pkt_cap; struct regions carry uint64_t — misalignment is UB
     * and UBSan rightly aborts on it) */
    pkt_cap = (pkt_cap + 7u) & ~7u;

    memset(&cfg, 0, sizeof cfg);
    cfg.codec = KN_GS_CODEC_H264;
    cfg.reorder_window_packets = KN_RX_DEFAULT_REORDER_WINDOW;
    cfg.max_access_unit_bytes = KN_GS_DEFAULT_MAX_ACCESS_UNIT;
    cfg.video_packet_size = vps;
    cfg.timeout_ms = KN_RX_MAX_TIMEOUT_MS;   /* the capture timeline drives
                                                reorder/FEC deadlines; the
                                                stream timeout must not fire
                                                across capture gaps */
    memset(&mem, 0, sizeof mem);
    {
        uint32_t fec_blocks = 32u, fec_shards = 64u;
        size_t pool_budget = 32u * 1024u * 1024u;   /* 2 GB machine (R11) */
        slots = cfg.reorder_window_packets + 1u + fec_blocks * fec_shards +
                16u;
        if ((size_t)slots * pkt_cap > pool_budget) {
            uint32_t fit = (uint32_t)(pool_budget / (pkt_cap ? pkt_cap : 1));
            if (fit < cfg.reorder_window_packets + 2u) {
                printf("{\"mode\":\"replay\",\"error\":\"pool_budget\","
                       "\"pkt_cap\":%u}\n", (unsigned)pkt_cap);
                fflush(stdout);
                fclose(f);
                kn_log_shutdown();
                return 3;
            }
            slots = fit;
        }
        scratch_len = ((uint32_t)kn_fec_scratch_bytes(fec_shards,
                                                      shard_len) +
                       4096u + 7u) & ~7u;   /* kn_fec_scratch_bytes can be
                                               odd (p*p) — keep the arena
                                               8-aligned */
        arena_need = (size_t)slots * sizeof(kn_rx_pkt)
                   + (size_t)slots * pkt_cap
                   + (size_t)(cfg.reorder_window_packets + 1u) *
                         sizeof(kn_rx_reorder_entry)
                   + (size_t)fec_blocks * sizeof(kn_rx_fec_block)
                   + (size_t)fec_blocks * fec_shards * sizeof(uint32_t)
                   + scratch_len
                   + (size_t)KN_RX_NACK_MAX_PENDING * sizeof(kn_rx_nack_entry)
                   + cfg.max_access_unit_bytes
                   + 16u * sizeof(kn_rx_event);
        arena = (uint8_t *)malloc(arena_need);
        if (!arena) {
            printf("{\"mode\":\"replay\",\"error\":\"arena_alloc\","
                   "\"need\":%lu}\n",
                   (unsigned long)(arena_need % 1000000000ul));
            fflush(stdout);
            fclose(f);
            kn_log_shutdown();
            return 3;
        }
        off = 0;
        mem.pool_meta = (kn_rx_pkt *)(void *)(arena + off);
        off += (size_t)slots * sizeof(kn_rx_pkt);
        mem.pool_bytes = arena + off;
        off += (size_t)slots * pkt_cap;
        mem.pool_slots = slots;
        mem.pool_pkt_cap = pkt_cap;
        mem.reorder_map = (kn_rx_reorder_entry *)(void *)(arena + off);
        off += (size_t)(cfg.reorder_window_packets + 1u) *
               sizeof(kn_rx_reorder_entry);
        mem.fec_blocks = (kn_rx_fec_block *)(void *)(arena + off);
        off += (size_t)fec_blocks * sizeof(kn_rx_fec_block);
        mem.fec_shard_arena = (uint32_t *)(void *)(arena + off);
        off += (size_t)fec_blocks * fec_shards * sizeof(uint32_t);
        mem.fec_max_blocks = fec_blocks;
        mem.fec_max_shards = fec_shards;
        mem.fec_scratch = arena + off;
        off += scratch_len;
        mem.fec_scratch_len = scratch_len;
        mem.nack_table = (kn_rx_nack_entry *)(void *)(arena + off);
        off += (size_t)KN_RX_NACK_MAX_PENDING * sizeof(kn_rx_nack_entry);
        mem.nack_capacity = KN_RX_NACK_MAX_PENDING;
        mem.au_buffer = arena + off;
        off += cfg.max_access_unit_bytes;
        mem.au_buffer_capacity = cfg.max_access_unit_bytes;
        mem.events = (kn_rx_event *)(void *)(arena + off);
        mem.events_capacity = 16;
    }

    if (kn_rx_video_init(&v, &cfg, &mem, 0) != KN_RX_OK) {
        printf("{\"mode\":\"replay\",\"error\":\"pipeline_init\"}\n");
        fflush(stdout);
        free(arena);
        fclose(f);
        kn_log_shutdown();
        return 3;
    }
    rc = kn_rx_srtp_session_init(&sess, profile, key_len ? key : NULL,
                                 key_len, salt_len ? salt : NULL, salt_len,
                                 tag_len);
    if (rc != KN_RX_OK) {
        printf("{\"mode\":\"replay\",\"error\":\"srtp_session\","
               "\"rc\":%d,\"backend_gcm\":%d,\"backend_256\":%d}\n", rc,
               kn_srtp_backend_gcm(), kn_srtp_backend_256());
        fflush(stdout);
        free(arena);
        fclose(f);
        kn_log_shutdown();
        return 3;
    }
    kn_rx_video_set_srtp(&v, &sess);

    {
        kn_lctx b;
        kn_lctx_init(&b, KN_CH_MEDIA, KN_INFO, "replay_begin");
        kn_lctx_str(&b, "capture", cap_path);
        kn_lctx_i64(&b, "records", (long long)records);
        kn_lctx_i64(&b, "vps", (long long)vps);
        kn_lctx_i64(&b, "srtp_profile", (long long)profile);
        kn_log_emit(&b);
    }

    /* pass 2: replay on the capture timeline */
    rewind(f);
    while (fgets(line, (int)sizeof line, f)) {
        kn_gs_dump_rec rec;
        size_t plen = 0;
        uint64_t now_ms;
        if (!line[0] || line[0] == '\n') continue;
        if (kn_gs_hexdump_parse_line(line, &rec, pkt, sizeof pkt, &plen)
            != KN_GS_OK)
            continue;                       /* counted in pass 1 */
        now_ms = rec.mono_us / 1000u;
        if (now_ms < t_last_ms) now_ms = t_last_ms;   /* monotonic guard */
        t_last_ms = now_ms;
        (void)kn_rx_video_process_datagram(&v, pkt, plen, now_ms,
                                           rp_frame_cb, &rc_ctx);
        (void)kn_rx_video_poll(&v, now_ms, rp_frame_cb, &rc_ctx);
    }
    fclose(f);
    f = NULL;
    (void)linecap;

    {
        char drops[512];
        size_t doff = 0;
        int d;
        for (d = 1; d < 24; ++d) {
            if (!v.drop_counters[d]) continue;
            if (doff + 48 < sizeof drops)
                doff += (size_t)snprintf(drops + doff, sizeof drops - doff,
                                         "%s\"%d\":%llu", doff ? "," : "",
                                         d,
                                         (unsigned long long)
                                         (v.drop_counters[d] %
                                          1000000000ull));
        }
        if (doff >= sizeof drops) doff = sizeof drops - 1;
        drops[doff] = '\0';
        printf("{\"mode\":\"replay\",\"summary\":{\"capture\":\"%.200s\","
               "\"records\":%lu,\"bad_lines\":%lu,\"authenticated\":%llu,"
               "\"frames\":%lu,\"keyframes\":%lu,\"frame_bytes\":%llu,"
               "\"fec_repaired\":%llu,\"stale\":%llu,\"gaps\":%llu,"
               "\"withheld\":%llu,\"kf_requests\":%llu,\"retransmits\":%llu,"
               "\"replay_rejected\":%llu,\"ev_overflow\":%u,"
               "\"au_write_errors\":%lu,\"vps\":%u,\"srtp_profile\":%d,"
               "\"state\":%d,\"drops\":{%s}}}\n",
               cap_path, records % 100000000ul, bad_lines % 100000000ul,
               (unsigned long long)(v.authenticated_packets % 10000000000ull),
               rc_ctx.aus % 100000000ul, rc_ctx.key_aus % 100000000ul,
               (unsigned long long)(rc_ctx.bytes % 1000000000000ull),
               (unsigned long long)(v.fec_repaired_packets % 1000000000ull),
               (unsigned long long)(v.stale_packets % 1000000000ull),
               (unsigned long long)(v.packet_gap_recoveries % 1000000000ull),
               (unsigned long long)(v.withheld_frames % 1000000000ull),
               (unsigned long long)(v.keyframe_requests % 1000000000ull),
               (unsigned long long)(v.recovered_retransmissions %
                                    1000000000ull),
               (unsigned long long)(v.replay_rejections % 1000000000ull),
               (unsigned)(v.ev_overflow % 1000000u),
               rc_ctx.write_errors % 100000000ul,
               (unsigned)(vps % 100000000u), profile, v.state, drops);
        fflush(stdout);
    }

    exit_code = (rc_ctx.aus > 0 && rc_ctx.write_errors == 0) ? 0 : 1;
    if (exit_code != 0) {
        kn_log_on_failure("session.replay",
                          rc_ctx.aus == 0 ? "E_REPLAY_NO_FRAMES"
                                          : "E_REPLAY_AU_WRITE",
                          rc_ctx.aus == 0
                              ? "the capture replayed but no access unit "
                                "completed (check drops in the summary)"
                              : "AU file writes failed (disk/permissions?)",
                          "inspect the summary JSON drops map + media.log; "
                          "the auto-bundle path is in this log dir");
    } else {
        kn_lctx e;
        kn_lctx_init(&e, KN_CH_MEDIA, KN_INFO, "replay_end");
        kn_lctx_i64(&e, "frames", (long long)rc_ctx.aus);
        kn_lctx_i64(&e, "fec_repaired",
                    (long long)(v.fec_repaired_packets % 1000000000ull));
        kn_log_emit(&e);
    }
    free(arena);
    kn_log_shutdown();
    return exit_code;
}

static int print_version(void)
{
    static char build[4096];
    size_t n = kn_diag_render_build_json(build, sizeof build);
    if (!n) return 3;
    /* build json is multi-line pretty JSON; flatten to one line for stdout */
    {
        size_t i, o = 0;
        for (i = 0; i < n; ++i)
            if (build[i] != '\n') build[o++] = build[i];
        build[o] = '\0';
        printf("%s\n", build);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *log_dir = "logs";
    const char *selftest = NULL;
    const char *loopback = NULL;          /* "" = default source discovery */
    const char *replay = NULL;
    const char *replay_au_dir = NULL;
    const char *replay_srtp = NULL;
    long replay_vps = 0;
    int loopback_seconds = 600;           /* G1 default: 10 minutes */
    int no_audio = 0;
    int diagnostics = 0, version = 0, list = 0, i;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strncmp(a, "--selftest=", 11) == 0) selftest = a + 11;
        else if (strcmp(a, "--loopback") == 0) loopback = "";
        else if (strncmp(a, "--loopback=", 11) == 0) loopback = a + 11;
        else if (strcmp(a, "--no-audio") == 0) no_audio = 1;
        else if (strncmp(a, "--replay-capture=", 17) == 0) replay = a + 17;
        else if (strncmp(a, "--replay-au-dir=", 16) == 0)
            replay_au_dir = a + 16;
        else if (strncmp(a, "--replay-srtp=", 14) == 0) replay_srtp = a + 14;
        else if (strncmp(a, "--replay-vps=", 13) == 0) {
            replay_vps = strtol(a + 13, NULL, 10);
            if (replay_vps < 0 || replay_vps > 65519) { usage(); return 2; }
        }
        else if (strncmp(a, "--loopback-seconds=", 19) == 0) {
            long v = strtol(a + 19, NULL, 10);
            if (v < 1 || v > 86400) { usage(); return 2; }
            loopback_seconds = (int)v;
        }
        else if (strcmp(a, "--diagnostics") == 0) diagnostics = 1;
        else if (strcmp(a, "--version") == 0) version = 1;
        else if (strcmp(a, "--list-selftests") == 0) list = 1;
        else if (strncmp(a, "--log-dir=", 10) == 0) log_dir = a + 10;
        else { usage(); return 2; }
    }

    if (list) return print_registry();
    if (version) return print_version();
    if (selftest && selftest[0]) return run_selftests(selftest, log_dir);
    if (replay && replay[0])
        return run_replay(replay, replay_au_dir, replay_srtp, replay_vps,
                          log_dir);
    if (loopback) return run_loopback(loopback, loopback_seconds, no_audio,
                                      log_dir);
    if (diagnostics) return run_diagnostics(log_dir);
    usage();
    return 2;
}
