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
/* Feature macro before EVERY system include (nanosleep in sess_sleep,
 * round XXI — the kn_sock.c landmine, third sighting). */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_audio.h"
#include "kn/kn_cm.h"
#include "kn/kn_d3d.h"
#include "kn/kn_decode.h"
#include "kn/kn_diag.h"
#include "kn/knfec.h"
#include "kn/kn_http.h"
#include "kn/kn_net.h"
#include "kn/kn_perf.h"
#include "kn/kn_rtsp.h"
#include "kn/kn_rx.h"
#include "kn/kn_selftest.h"
#include "kn/kn_sha1.h"
#include "kn/kn_sock.h"
#include "kn/kn_ws.h"
#include "kn/knlog.h"
#include "kn/knring.h"

#include <signal.h>
#include <stdio.h>
#ifndef _WIN32
#  include <time.h>     /* sess_sleep: nanosleep (round XXI) */
#endif
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
           "  knvstd --capture [--capture-port=N] [--capture-bind=IP]\n"
           "         [--capture-out=FILE] [--capture-seconds=N]   (0 = until Ctrl-C)\n"
           "  knvstd --decode [--decode-port=N] [--decode-bind=IP]\n"
           "         [--decode-seconds=N] [--decode-srtp=SPEC] [--decode-vps=N]\n"
           "         [--decode-au-dir=DIR] [--decode-expect-frames=N]\n"
           "  knvstd --session --session-token=TOK --session-app=N\n"
           "         [--session-title=S] [--session-device=S] [--cm-host=HOST]\n"
           "         [--session-seconds=N] [--session-queue-timeout=N]\n"
           "         [--session-expect-frames=N] [--session-au-dir=DIR]\n"
           "       [--log-dir=DIR]   (WASAPI synthetic PCM unless --no-audio)\n"
           "  knvstd --diagnostics [--log-dir=DIR]\n"
           "  knvstd --list-selftests\n"
           "  knvstd --version\n"
           "loopback source order: FILE.264, ./synthetic720.264, <exe dir>/\n"
           "synthetic720.264, embedded 320x240 clip (budget_valid:false).\n"
           "exit codes: 0 ok/skipped, 1 failed verdict, 2 usage, 3 internal\n");
}

/* THE stdout-JSON string sanitizer for knvstd (name is historical — it
 * started as the loopback path formatter and T11 proved it on the
 * target). Rules: '\' and '/' both normalize to '/' (Windows paths stay
 * readable and valid), '"' gets escaped, C0 controls and bytes >= 0x7F
 * become '?'. Round XVIII hardening: Win32 argv is the ANSI codepage
 * (cp1250 on the target) — a non-ASCII path byte would be INVALID UTF-8
 * inside the JSONL contract, so it cannot pass through raw; '?' keeps the
 * line parseable and the evidence honest ("foreign byte was here").
 * Every printf-JSON site embedding an argv/path/foreign string MUST route
 * through this (the T13 r1 landmine: replay/capture summaries embedded
 * "C:\Users\..." raw — \U is not a legal JSON escape and the summary
 * line became unparseable while all 60 frames actually matched). */
static void lb_json_path(char *dst, size_t dstsz, const char *src)
{
    size_t i, o = 0;
    if (!dst || dstsz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    for (i = 0; src[i] && o + 2 < dstsz; ++i) {
        if (src[i] == '\\' || src[i] == '/') dst[o++] = '/';
        else if (src[i] == '"') { dst[o++] = '\\'; dst[o++] = '"'; }
        else if ((unsigned char)src[i] < 0x20) dst[o++] = '?';
        else if ((unsigned char)src[i] >= 0x7F) dst[o++] = '?';
        else dst[o++] = src[i];
    }
    dst[o] = '\0';
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
            char esc[128];
            lb_json_path(esc, sizeof esc, which);
            printf("{\"error\":\"unknown_selftest\",\"name\":\"%s\"}\n",
                   esc);
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
        char esc[KN_DIAG_PATH_MAX + 96];
    lb_json_path(esc, sizeof esc, path);
    printf("{\"bundle\":\"%s\"}\n", esc);
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
        char esc[608];
        lb_json_path(esc, sizeof esc, cap_path);
        printf("{\"mode\":\"replay\",\"error\":\"capture_unreadable\","
               "\"path\":\"%.300s\"}\n", esc);
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
        char esc[608];
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
        lb_json_path(esc, sizeof esc, cap_path);
        printf("{\"mode\":\"replay\",\"summary\":{\"capture\":\"%.200s\","
               "\"records\":%lu,\"bad_lines\":%lu,\"authenticated\":%llu,"
               "\"frames\":%lu,\"keyframes\":%lu,\"frame_bytes\":%llu,"
               "\"fec_repaired\":%llu,\"stale\":%llu,\"gaps\":%llu,"
               "\"withheld\":%llu,\"kf_requests\":%llu,\"retransmits\":%llu,"
               "\"replay_rejected\":%llu,\"ev_overflow\":%u,"
               "\"au_write_errors\":%lu,\"vps\":%u,\"srtp_profile\":%d,"
               "\"state\":%d,\"drops\":{%s}}}\n",
               esc, records % 100000000ul, bad_lines % 100000000ul,
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

/* ------------------------------------------------------ graceful stop */
/* Ctrl-C (Win32 console handler) / SIGINT+SIGTERM (POSIX) set a flag the
 * capture/decode loops poll every iteration: the run ends with a complete
 * summary line and a flushed capture file instead of a torn write (R2b:
 * the writer must preserve evidence through an abnormal end). */
static volatile sig_atomic_t g_knv_stop;

#ifdef _WIN32
static BOOL WINAPI knv_ctrl_handler(DWORD type)
{
    (void)type;
    g_knv_stop = 1;
    return TRUE;
}
static void knv_stop_install(void) { SetConsoleCtrlHandler(knv_ctrl_handler, TRUE); }
#else
static void knv_sig_handler(int sig) { (void)sig; g_knv_stop = 1; }
static void knv_stop_install(void)
{
    signal(SIGINT, knv_sig_handler);
    signal(SIGTERM, knv_sig_handler);
}
#endif

/* RTP sequence from the unencrypted header bytes — valid even for SRTP
 * datagrams (only the payload/tag are protected). meta column of the
 * kn_ring_dump line format; 0 when the datagram is not RTP-shaped. */
static uint32_t knv_rtp_seq_meta(const uint8_t *p, size_t n)
{
    if (n >= 12 && (p[0] >> 6) == 2u)
        return ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    return 0u;
}

/* ---------------------------------------------------------- capture mode
 * knvstd --capture — the stage-2 RECORDING mechanism (R32's answer: the
 * first real Mjolnir session becomes a permanent regression vector).
 *
 * Receives raw UDP datagrams and writes them SEQUENTIALLY to a file in the
 * kn_ring_dump line format — byte-identical to what tools/make_rx_capture.py
 * emits and what --replay-capture reads:
 *     "<mono_us> <meta_%08x> <flags> <len> <hex bytes...>\n"
 * Deviation from the Next-steps wording (documented in docs/SOCK.md): the
 * PRIMARY record is a streaming file writer, not the ring — a kn_ring only
 * ever holds the LAST N records, so a ring-primary capture could not record
 * a whole session on a 2 GB machine. The kn_ring (64 KiB, §7.10 sizing) is
 * kept as a failure-diagnostic mirror and dumped into the log dir on a
 * failed run so the auto-bundle carries recent wire bytes.
 * Flush policy: every 32 records or 100 ms (bounded loss on a hard crash,
 * no fsync storm — §7.12).
 */
#define CAP_LINE_BUF   (3u * KN_SOCK_RECV_BUF + 96u)  /* " xx" per byte */
#define CAP_FLUSH_EVERY 32u

static int cap_build_line(char *line, size_t linesz, uint64_t mono_us,
                          uint32_t meta, uint32_t flags,
                          const uint8_t *d, size_t n, size_t *out_len)
{
    static const char hexd[] = "0123456789abcdef";
    size_t o, i;
    int h = snprintf(line, linesz, "%llu %08x %u %u",
                     (unsigned long long)(mono_us % 10000000000000000ull),
                     (unsigned)meta, (unsigned)(flags % 100u),
                     (unsigned)(n % 100000000u));
    if (h < 0 || (size_t)h + 3 * n + 2 > linesz) return -1;
    o = (size_t)h;
    for (i = 0; i < n; ++i) {
        line[o++] = ' ';
        line[o++] = hexd[d[i] >> 4];
        line[o++] = hexd[d[i] & 0x0Fu];
    }
    line[o++] = '\n';
    line[o] = '\0';
    *out_len = o;
    return 0;
}

static int run_capture(uint16_t port, const char *bind_ip, const char *out_path,
                       long seconds, const char *log_dir)
{
    static uint8_t pkt[KN_SOCK_RECV_BUF];
    static char line[CAP_LINE_BUF];
    kn_sock_udp sock;
    kn_ring ring;
    FILE *fp = NULL;
    char err[128];
    char ringpath[640];
    uint64_t t_start_ms, t_last_flush_us, now_us;
    unsigned long long datagrams = 0, bytes = 0, flushes = 0, file_errors = 0;
    unsigned long long since_flush = 0;
    uint32_t max_len = 0;
    size_t plen;
    int ring_ok, rc, exit_code;
    const char *stop_reason = "time";
    kn_log_config lcfg;

    knv_stop_install();
    memset(&lcfg, 0, sizeof lcfg);
    lcfg.log_dir = log_dir;
    (void)kn_log_init(&lcfg);
    kn_diag_set_selftest_provider(kn_selftest_render_relevant);
    (void)kn_diag_install(log_dir);

    err[0] = '\0';
    rc = kn_sock_udp_open(&sock, bind_ip, port, KN_SOCK_RCVBUF_DEFAULT,
                          err, sizeof err);
    if (rc != KN_SOCK_OK) {
        char esc3[256];
        lb_json_path(esc3, sizeof esc3, err);
        printf("{\"mode\":\"capture\",\"error\":\"socket_open\",\"rc\":%d,"
               "\"errno\":%d,\"detail\":\"%.100s\"}\n", rc, sock.last_error,
               esc3);
        fflush(stdout);
        kn_log_on_failure("session.capture", "E_CAPTURE_SOCKET",
                          "UDP socket open/bind failed", err);
        kn_log_shutdown();
        return 1;
    }
    fp = fopen(out_path, "wb");
    if (!fp) {
        char esc2[608];
        lb_json_path(esc2, sizeof esc2, out_path);
        printf("{\"mode\":\"capture\",\"error\":\"out_unwritable\","
               "\"path\":\"%.300s\"}\n", esc2);
        fflush(stdout);
        kn_sock_udp_close(&sock);
        kn_log_on_failure("session.capture", "E_CAPTURE_OUT",
                          "capture output file cannot be opened", out_path);
        kn_log_shutdown();
        return 1;
    }
    ring_ok = (kn_ring_init(&ring, 64u * 1024u, 1312) == 0);  /* §7.10 note:
        max_record 1312 => ~48 records at packetSize 1280; larger datagrams
        are truncated+flagged in the MIRROR only — the file record is full */

    {
        kn_lctx b;
        char iptext[16];
        kn_lctx_init(&b, KN_CH_TRANSPORT, KN_INFO, "capture_begin");
        kn_lctx_str(&b, "bind", kn_sock_ipv4_text(sock.local_addr, iptext));
        kn_lctx_i64(&b, "port", (long long)kn_sock_udp_port(&sock));
        kn_lctx_i64(&b, "seconds", (long long)seconds);
        kn_lctx_i64(&b, "rcvbuf_req", (long long)sock.rcvbuf_requested);
        kn_lctx_i64(&b, "rcvbuf_act", (long long)sock.rcvbuf_actual);
        kn_lctx_str(&b, "out", out_path);
        kn_log_emit(&b);
    }

    t_start_ms = kn_sock_now_ms();
    t_last_flush_us = kn_sock_now_us();
    while (!g_knv_stop) {
        uint64_t now_ms = kn_sock_now_ms();
        if (seconds > 0 && now_ms - t_start_ms >= (uint64_t)seconds * 1000u) {
            stop_reason = "time";
            break;
        }
        plen = 0;
        rc = kn_sock_udp_recv(&sock, pkt, sizeof pkt,
                              KN_SOCK_POLL_INTERVAL_MS, &plen, NULL);
        if (rc == 1) {
            size_t llen;
            now_us = kn_sock_now_us();
            datagrams++;
            bytes += (unsigned long long)plen;
            if (plen > max_len) max_len = (uint32_t)plen;
            if (ring_ok)
                kn_ring_append(&ring, now_us, knv_rtp_seq_meta(pkt, plen),
                               pkt, plen);
            if (cap_build_line(line, sizeof line, now_us,
                               knv_rtp_seq_meta(pkt, plen), 0u, pkt, plen,
                               &llen) == 0) {
                if (fwrite(line, 1, llen, fp) != llen) file_errors++;
            } else {
                file_errors++;
            }
            since_flush++;
            now_us = kn_sock_now_us();
            if (since_flush >= CAP_FLUSH_EVERY ||
                now_us - t_last_flush_us >= 100000u) {
                if (fflush(fp) != 0) file_errors++;
                else flushes++;
                since_flush = 0;
                t_last_flush_us = now_us;
            }
        } else if (rc < 0 && rc != KN_SOCK_ERR_MSGSIZE) {
            /* socket dead: stop with what we have (the file is flushed) */
            printf("{\"mode\":\"capture\",\"error\":\"recv\",\"rc\":%d,"
                   "\"errno\":%d}\n", rc, sock.last_error);
            fflush(stdout);
            stop_reason = "socket_error";
            break;
        }
        /* rc == 0: the 10 ms poll tick (nvst.rs:246 cadence) */
    }
    if (g_knv_stop) stop_reason = "signal";
    if (fflush(fp) != 0) file_errors++;
    else flushes++;
    {
        int fc = fclose(fp);
        fp = NULL;
        if (fc != 0) file_errors++;
    }

    exit_code = (datagrams > 0 && file_errors == 0 &&
                 strcmp(stop_reason, "socket_error") != 0) ? 0 : 1;

    {
        char escb[64], esco[608];
        lb_json_path(escb, sizeof escb, bind_ip && bind_ip[0] ? bind_ip
                                                              : "0.0.0.0");
        lb_json_path(esco, sizeof esco, out_path);
    printf("{\"mode\":\"capture\",\"summary\":{\"bind\":\"%s\",\"port\":%u,"
           "\"seconds_budget\":%ld,\"stop_reason\":\"%s\","
           "\"datagrams\":%llu,\"bytes\":%llu,\"max_len\":%u,"
           "\"rcvbuf_requested\":%u,\"rcvbuf_actual\":%u,"
           "\"recv_timeouts\":%llu,\"recv_errors\":%llu,"
           "\"ring_records\":%llu,\"ring_dropped\":%llu,\"ring_ok\":%d,"
           "\"file_errors\":%llu,\"flushes\":%llu,\"out\":\"%.300s\"}}\n",
           escb,
           (unsigned)kn_sock_udp_port(&sock), seconds, stop_reason,
           datagrams % 10000000000ull, bytes % 10000000000000ull,
           max_len % 100000000u, sock.rcvbuf_requested, sock.rcvbuf_actual,
           (unsigned long long)(sock.recv_timeouts % 10000000000ull),
           (unsigned long long)(sock.recv_errors % 1000000000ull),
           ring_ok ? (unsigned long long)(kn_ring_count(&ring) % 1000000ull)
                   : 0ull,
           ring_ok ? (unsigned long long)(ring.dropped % 1000000000ull) : 0ull,
           ring_ok ? 1 : 0,
           file_errors % 1000000000ull, flushes % 1000000000ull, esco);
    fflush(stdout);
    }

    if (exit_code != 0) {
        /* §7.10: the ring mirror rides into the auto-bundle as a .log so
         * the logs/ enumeration picks it up */
        if (ring_ok &&
            snprintf(ringpath, sizeof ringpath, "%s/wire_capture.log",
                     log_dir) < (int)sizeof ringpath)
            (void)kn_ring_dump(&ring, ringpath);
        kn_log_on_failure("session.capture",
                          datagrams == 0 ? "E_CAPTURE_EMPTY"
                                         : "E_CAPTURE_IO",
                          datagrams == 0
                              ? "no datagrams arrived within the budget "
                                "(wrong port? sender down? firewall?)"
                              : "capture file I/O errors or socket failure",
                          "check the summary JSON; the wire mirror is in the "
                          "log dir; rerun the sender (tools/send_rx_capture.py)");
    } else {
        kn_lctx e;
        kn_lctx_init(&e, KN_CH_TRANSPORT, KN_INFO, "capture_end");
        kn_lctx_i64(&e, "datagrams", (long long)(datagrams % 10000000000ll));
        kn_lctx_i64(&e, "bytes", (long long)(bytes % 10000000000000ll));
        kn_lctx_str(&e, "out", out_path);
        kn_log_emit(&e);
    }
    if (ring_ok) kn_ring_free(&ring);
    kn_sock_udp_close(&sock);
    kn_log_shutdown();
    return exit_code;
}

/* ------------------------------------------------------------ decode mode
 * knvstd --decode — the stage-2 LIVE receive path: real UDP socket -> real
 * monotonic clock -> the FULL kn_rx pipeline (SRTP when material is given,
 * reorder, FEC repair, AU assembly, RFI/NACK logic) -> a capacity-2 encoded
 * frame queue (analiza/04 §2.4: overflow = desync = clear the reference
 * chain + request a keyframe) -> the persistent kn_decode stream session.
 *
 * SINGLE-THREADED by design (documented decision, docs/SOCK.md): recv with
 * the reference's 10 ms poll cadence (nvst.rs:246), drain the AU queue
 * inline after every process/poll. The 8 MiB SO_RCVBUF (nvst.rs:232)
 * absorbs decode stalls (~16 ms worst frame at 720p on the E5500 =
 * ~500 datagrams of headroom at 30 fps). The stage-1 loopback ran the same
 * shape at 30.000 fps for 600 s (G1). If live G2 measurements ever show
 * recv backlog during decode stalls, the receive side moves to a thread —
 * the queue semantics below are already the thread-ready ones.
 *
 * The summary line is deliberately FIELD-COMPATIBLE with the replay-mode
 * summary plus make_rx_capture.py --verify's counter contract (it carries
 * "frames" as an alias of "aus"), so the SAME manifest verification runs
 * against a live socket session as against an offline replay.
 *
 * stdout: one JSON line per AU (fi/len/sha1/kf/contig/ts + decode result),
 * one per significant rx event (capped), one kf_request line per keyframe
 * flag (the control channel that would SEND it is stage 3 — until then the
 * request is recorded, never silently dropped), and a final summary.
 */
#define DEC_AU_CAP      KN_GS_DEFAULT_MAX_ACCESS_UNIT   /* 2 MiB per slot */
#define DEC_QUEUE_CAP   2u          /* analiza/04 §2.4: keep 2 (max 3) */
#define DEC_EVENT_LINES 1000u       /* stdout cap; counters stay exact */

typedef struct dec_q_slot {
    size_t   len;
    uint32_t ts, fi;
    int      kf, contig;
} dec_q_slot;

typedef struct dec_ctx {
    kn_rx_video *v;
    uint64_t     now_ms;          /* set before every pipeline call */
    /* encoded-frame queue (single-threaded: no locks needed) */
    uint8_t     *qbytes[DEC_QUEUE_CAP];
    dec_q_slot   qslot[DEC_QUEUE_CAP];
    unsigned     q_head, q_count;
    unsigned long long q_overflows, q_pushed;
    /* decode session */
    kn_decode_stream *stream;
    int          decode_available;
    int          decode_aborted;
    unsigned long long frames_out, decode_errors, aus_undecoded;
    int          last_w, last_h;
    long long    worst_decode_ms;
    /* reporting */
    const char  *au_dir;
    unsigned long long aus, key_aus, au_write_errors, event_lines,
                       event_lines_capped, kf_request_lines;
    unsigned long long rx_events_logged;
} dec_ctx;

static uint8_t g_dec_qbytes[DEC_QUEUE_CAP][DEC_AU_CAP];

/* kn_rx frame callback: runs INSIDE process_datagram/poll — copy the AU
 * (f->bytes aliases the assembler buffer) into the queue; overflow policy
 * per analiza/04 §5 "encoded queue overflow" row. */
static void dec_frame_in_cb(const kn_rx_frame *f, void *ud)
{
    dec_ctx *c = (dec_ctx *)ud;
    unsigned tail;

    if (f->bytes_len > DEC_AU_CAP) {          /* cannot happen: the pipeline
                                                 enforces max_access_unit */
        c->q_overflows++;
        return;
    }
    if (c->q_count == DEC_QUEUE_CAP) {
        /* DESYNC: the decoder never saw a frame that later packets
         * reference -> invalidate the chain (RFI owns recovery when frame
         * types are known) and request a keyframe when it does not. */
        c->q_overflows++;
        if (!kn_rx_inval_lose(&c->v->inval, c->now_ms))
            kn_rx_video_request_keyframe(c->v);
        return;                                /* drop the NEW AU */
    }
    tail = (c->q_head + c->q_count) % DEC_QUEUE_CAP;
    memcpy(c->qbytes[tail], f->bytes, f->bytes_len);
    c->qslot[tail].len = f->bytes_len;
    c->qslot[tail].ts = f->timestamp;
    c->qslot[tail].fi = f->frame_index;
    c->qslot[tail].kf = f->keyframe;
    c->qslot[tail].contig = f->contiguous;
    c->q_count++;
    c->q_pushed++;
}

/* decoded-frame callback (kn_decode stream session) */
static int dec_frame_out_cb(const uint8_t *data, int w, int h, int sy,
                            int suv, int is_nv12, long long idx, void *ud)
{
    dec_ctx *c = (dec_ctx *)ud;
    (void)data; (void)sy; (void)suv; (void)is_nv12; (void)idx;
    c->frames_out++;
    c->last_w = w;
    c->last_h = h;
    return 0;
}

static const char *dec_ev_name(int type)
{
    switch (type) {
    case KN_RX_EV_DROPPED:          return "dropped";
    case KN_RX_EV_RECOVERY_GAP:     return "recovery_gap";
    case KN_RX_EV_RECOVERY_TIMEOUT: return "recovery_timeout";
    case KN_RX_EV_LIFECYCLE:        return "lifecycle";
    case KN_RX_EV_PROGRESS_RESUMED: return "progress_resumed";
    case KN_RX_EV_PROGRESS_STALL:   return "progress_stall";
    case KN_RX_EV_PROGRESS_RECOVERY:return "progress_recovery";
    default:                        return "none";
    }
}

static void dec_drain_events(dec_ctx *c)
{
    uint32_t n = 0, i;
    const kn_rx_event *ev = kn_rx_video_events(c->v, &n);
    for (i = 0; i < n; ++i) {
        int significant = ev[i].type != KN_RX_EV_DROPPED &&
                          ev[i].type != KN_RX_EV_NONE;
        if (c->event_lines < DEC_EVENT_LINES) {
            c->event_lines++;
            printf("{\"mode\":\"decode\",\"event\":\"rx\",\"type\":\"%s\","
                   "\"drop_reason\":%d,\"detail\":%d,"
                   "\"index\":%llu,\"gap_first\":%llu,\"gap_last\":%llu,"
                   "\"idle_ms\":%llu,\"new_state\":%d}\n",
                   dec_ev_name(ev[i].type), ev[i].drop_reason, ev[i].detail,
                   (unsigned long long)(ev[i].index % 10000000000ull),
                   (unsigned long long)(ev[i].gap_first % 10000000000ull),
                   (unsigned long long)(ev[i].gap_last % 10000000000ull),
                   (unsigned long long)(ev[i].idle_ms % 1000000000ull),
                   ev[i].new_state);
        } else {
            c->event_lines_capped++;
        }
        if (significant && c->rx_events_logged < 100) {
            kn_lctx b;
            c->rx_events_logged++;
            kn_lctx_init(&b, KN_CH_TRANSPORT, KN_INFO, "rx_event");
            kn_lctx_str(&b, "type", dec_ev_name(ev[i].type));
            kn_lctx_i64(&b, "detail", (long long)ev[i].detail);
            kn_log_emit(&b);
        }
    }
}

static void dec_take_kf(dec_ctx *c)
{
    if (kn_rx_video_take_keyframe_request(c->v)) {
        c->kf_request_lines++;
        printf("{\"mode\":\"decode\",\"event\":\"kf_request\","
               "\"sent\":false,\"note\":\"control channel is stage 3\"}\n");
        if (c->kf_request_lines <= 20) {
            kn_lctx b;
            kn_lctx_init(&b, KN_CH_TRANSPORT, KN_INFO, "keyframe_request");
            kn_lctx_i64(&b, "n", (long long)(c->kf_request_lines % 1000000));
            kn_lctx_str(&b, "transport", "unsent (stage-3 control channel)");
            kn_log_emit(&b);
        }
    }
}

/* Drain the encoded queue into the decoder (+ optional AU dump). */
static void dec_drain_queue(dec_ctx *c)
{
    while (c->q_count) {
        unsigned head = c->q_head;
        const dec_q_slot *s = &c->qslot[head];
        const uint8_t *bytes = c->qbytes[head];
        uint8_t dig[KN_SHA1_DIGEST_LEN];
        char hex[2 * KN_SHA1_DIGEST_LEN + 1];
        int i, drc = 0;

        c->q_head = (head + 1) % DEC_QUEUE_CAP;
        c->q_count--;

        kn_sha1(bytes, s->len, dig);
        for (i = 0; i < KN_SHA1_DIGEST_LEN; ++i)
            sprintf(hex + 2 * i, "%02x", (unsigned)dig[i]);

        if (c->stream && !c->decode_aborted) {
            drc = kn_decode_stream_push(c->stream, bytes, s->len, s->ts,
                                        dec_frame_out_cb, c);
            if (drc == -4) c->decode_aborted = 1;
            else if (drc < 0) c->decode_errors++;
        } else {
            c->aus_undecoded++;
        }

        printf("{\"mode\":\"decode\",\"event\":\"au\",\"fi\":%u,\"len\":%lu,"
               "\"sha1\":\"%s\",\"keyframe\":%d,\"contiguous\":%d,"
               "\"ts\":%u,\"decode_rc\":%d,\"frames_out\":%llu}\n",
               (unsigned)(s->fi % 100000000u),
               (unsigned long)(s->len % 100000000ul), hex,
               s->kf ? 1 : 0, s->contig ? 1 : 0,
               (unsigned)(s->ts % 100000000u), drc,
               (unsigned long long)(c->frames_out % 10000000000ull));

        c->aus++;
        if (s->kf) c->key_aus++;
        if (c->au_dir && c->au_dir[0]) {
            char path[1200];
            FILE *fp;
            int n = snprintf(path, sizeof path, "%s/au_%05u_%c.264",
                             c->au_dir, (unsigned)(s->fi % 100000u),
                             s->kf ? 'k' : 'p');
            if (n <= 0 || (size_t)n >= sizeof path) { c->au_write_errors++; }
            else {
                fp = fopen(path, "wb");
                if (!fp) c->au_write_errors++;
                else {
                    if (s->len && fwrite(bytes, 1, s->len, fp) != s->len)
                        c->au_write_errors++;
                    fclose(fp);
                }
            }
        }
    }
}

static int run_decode(uint16_t port, const char *bind_ip, long seconds,
                      const char *srtp_spec, long vps_arg,
                      const char *au_dir, long expect_frames,
                      const char *log_dir)
{
    static uint8_t pkt[KN_SOCK_RECV_BUF];
    static dec_ctx c;
    kn_rx_video v;
    kn_rx_mem mem;
    kn_rx_video_config cfg;
    kn_rx_srtp_session sess;
    kn_sock_udp sock;
    kn_ring ring;
    uint8_t *arena = NULL;
    char err[128], ringpath[640], decerr[128];
    uint32_t vps, shard_len, pkt_cap, scratch_len, slots;
    size_t arena_need, off;
    uint64_t t_start_ms, next_poll_ms, now_ms, t_last_stats_ms;
    unsigned long long datagrams = 0;
    unsigned consecutive_errors = 0;
    int profile = KN_RX_SRTP_NONE, ring_ok, rc, exit_code, have_sess = 0;
    size_t tag_len = 0, key_len = 0, salt_len = 0;
    uint8_t key[32], salt[14];
    const char *stop_reason = "time";
    kn_log_config lcfg;

    knv_stop_install();
    memset(&c, 0, sizeof c);
    c.au_dir = au_dir;
    for (slots = 0; slots < DEC_QUEUE_CAP; ++slots)
        c.qbytes[slots] = g_dec_qbytes[slots];
    if (au_dir && au_dir[0]) {
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
    (void)kn_diag_install(log_dir);

    if (srtp_spec && srtp_spec[0]) {
        if (rp_parse_srtp(srtp_spec, &profile, &tag_len, key, &key_len,
                          salt, &salt_len) != 0) {
            printf("{\"mode\":\"decode\",\"error\":\"bad_srtp_spec\"}\n");
            fflush(stdout);
            kn_log_shutdown();
            return 2;
        }
    }

    /* --- socket --- */
    err[0] = '\0';
    rc = kn_sock_udp_open(&sock, bind_ip, port, KN_SOCK_RCVBUF_DEFAULT,
                          err, sizeof err);
    if (rc != KN_SOCK_OK) {
        char esc3[256];
        lb_json_path(esc3, sizeof esc3, err);
        printf("{\"mode\":\"decode\",\"error\":\"socket_open\",\"rc\":%d,"
               "\"errno\":%d,\"detail\":\"%.100s\"}\n", rc, sock.last_error,
               esc3);
        fflush(stdout);
        kn_log_on_failure("session.decode", "E_DECODE_SOCKET",
                          "UDP socket open/bind failed", err);
        kn_log_shutdown();
        return 1;
    }
    ring_ok = (kn_ring_init(&ring, 64u * 1024u, 1312) == 0);

    /* --- pipeline arena (replay's sizing with the LIVE vps; the capture
     * scan does not exist here — vps comes from the SDP/session, default
     * 1280 = nvst.rs:216) --- */
    vps = vps_arg > 0 ? (uint32_t)vps_arg : KN_RX_DEFAULT_VIDEO_PACKET_SIZE;
    if (vps < KN_RX_MIN_VIDEO_PACKET_SIZE) vps = KN_RX_MIN_VIDEO_PACKET_SIZE;
    if (vps > KN_RX_MAX_VIDEO_PACKET_SIZE) vps = KN_RX_MAX_VIDEO_PACKET_SIZE;
    shard_len = vps + KN_RX_FEC_HEADER_ALLOWANCE;
    pkt_cap = shard_len + 64u;               /* wire datagrams carry the
                                                SRTP tag; unprotect works
                                                in place on this buffer */
    pkt_cap = (pkt_cap + 7u) & ~7u;          /* 8-byte arena alignment */

    memset(&cfg, 0, sizeof cfg);
    cfg.codec = KN_GS_CODEC_H264;
    cfg.reorder_window_packets = KN_RX_DEFAULT_REORDER_WINDOW;
    cfg.max_access_unit_bytes = KN_GS_DEFAULT_MAX_ACCESS_UNIT;
    cfg.video_packet_size = vps;
    cfg.timeout_ms = KN_RX_DEFAULT_TIMEOUT_MS;   /* LIVE: the real 5 s */
    memset(&mem, 0, sizeof mem);
    {
        uint32_t fec_blocks = 32u, fec_shards = 64u;
        size_t pool_budget = 32u * 1024u * 1024u;   /* 2 GB machine (R11) */
        slots = cfg.reorder_window_packets + 1u + fec_blocks * fec_shards +
                16u;
        if ((size_t)slots * pkt_cap > pool_budget) {
            uint32_t fit = (uint32_t)(pool_budget / (pkt_cap ? pkt_cap : 1));
            if (fit < cfg.reorder_window_packets + 2u) {
                printf("{\"mode\":\"decode\",\"error\":\"pool_budget\","
                       "\"pkt_cap\":%u}\n", (unsigned)pkt_cap);
                fflush(stdout);
                if (ring_ok) kn_ring_free(&ring);
                kn_sock_udp_close(&sock);
                kn_log_shutdown();
                return 3;
            }
            slots = fit;
        }
        scratch_len = ((uint32_t)kn_fec_scratch_bytes(fec_shards,
                                                      shard_len) +
                       4096u + 7u) & ~7u;
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
            printf("{\"mode\":\"decode\",\"error\":\"arena_alloc\","
                   "\"need\":%lu}\n",
                   (unsigned long)(arena_need % 1000000000ul));
            fflush(stdout);
            if (ring_ok) kn_ring_free(&ring);
            kn_sock_udp_close(&sock);
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

    now_ms = kn_sock_now_ms();
    if (kn_rx_video_init(&v, &cfg, &mem, now_ms) != KN_RX_OK) {
        printf("{\"mode\":\"decode\",\"error\":\"pipeline_init\"}\n");
        fflush(stdout);
        free(arena);
        if (ring_ok) kn_ring_free(&ring);
        kn_sock_udp_close(&sock);
        kn_log_shutdown();
        return 3;
    }
    c.v = &v;
    rc = kn_rx_srtp_session_init(&sess, profile, key_len ? key : NULL,
                                 key_len, salt_len ? salt : NULL, salt_len,
                                 tag_len);
    if (rc != KN_RX_OK) {
        printf("{\"mode\":\"decode\",\"error\":\"srtp_session\",\"rc\":%d,"
               "\"backend_gcm\":%d,\"backend_256\":%d}\n", rc,
               kn_srtp_backend_gcm(), kn_srtp_backend_256());
        fflush(stdout);
        free(arena);
        if (ring_ok) kn_ring_free(&ring);
        kn_sock_udp_close(&sock);
        kn_log_shutdown();
        return 3;
    }
    kn_rx_video_set_srtp(&v, &sess);
    have_sess = 1;
    (void)have_sess;

    /* --- decoder session (stub builds run pipeline-only, honestly) --- */
    c.decode_available = kn_decode_available();
    decerr[0] = '\0';
    if (c.decode_available) {
        c.stream = kn_decode_stream_open(1, decerr, sizeof decerr);
        if (!c.stream) c.decode_available = 0;   /* fall back, report why */
    }

    {
        kn_lctx b;
        char iptext[16];
        kn_lctx_init(&b, KN_CH_TRANSPORT, KN_INFO, "decode_begin");
        kn_lctx_str(&b, "bind", kn_sock_ipv4_text(sock.local_addr, iptext));
        kn_lctx_i64(&b, "port", (long long)kn_sock_udp_port(&sock));
        kn_lctx_i64(&b, "seconds", (long long)seconds);
        kn_lctx_i64(&b, "vps", (long long)vps);
        kn_lctx_i64(&b, "srtp_profile", (long long)profile);
        kn_lctx_i64(&b, "rcvbuf_act", (long long)sock.rcvbuf_actual);
        kn_lctx_str(&b, "decoder", c.stream ? kn_decode_backend()
                                            : (decerr[0] ? decerr
                                                         : "unavailable"));
        kn_log_emit(&b);
    }
    printf("{\"mode\":\"decode\",\"event\":\"begin\",\"port\":%u,\"vps\":%u,"
           "\"srtp_profile\":%d,\"rcvbuf_actual\":%u,\"decoder\":\"%.96s\"}\n",
           (unsigned)kn_sock_udp_port(&sock), (unsigned)vps, profile,
           sock.rcvbuf_actual,
           c.stream ? kn_decode_backend() : "pipeline-only");
    fflush(stdout);

    /* --- receive loop --- */
    t_start_ms = kn_sock_now_ms();
    t_last_stats_ms = t_start_ms;
    next_poll_ms = t_start_ms;
    while (!g_knv_stop) {
        size_t plen = 0;
        now_ms = kn_sock_now_ms();
        if (seconds > 0 && now_ms - t_start_ms >= (uint64_t)seconds * 1000u) {
            stop_reason = "time";
            break;
        }
        if (expect_frames > 0 &&
            (long long)((c.stream && !c.decode_aborted) ? c.frames_out
                                                        : c.aus) >=
                expect_frames) {
            stop_reason = "expect_frames";
            break;
        }
        rc = kn_sock_udp_recv(&sock, pkt, sizeof pkt,
                              KN_SOCK_POLL_INTERVAL_MS, &plen, NULL);
        c.now_ms = kn_sock_now_ms();
        if (rc == 1) {
            consecutive_errors = 0;
            datagrams++;
            if (ring_ok)
                kn_ring_append(&ring, kn_sock_now_us(),
                               knv_rtp_seq_meta(pkt, plen), pkt, plen);
            (void)kn_rx_video_process_datagram(&v, pkt, plen, c.now_ms,
                                               dec_frame_in_cb, &c);
            dec_drain_events(&c);
            dec_take_kf(&c);
            dec_drain_queue(&c);
        } else if (rc < 0) {
            consecutive_errors++;
            if (consecutive_errors <= 5) {
                printf("{\"mode\":\"decode\",\"error\":\"recv\",\"rc\":%d,"
                       "\"errno\":%d}\n", rc, sock.last_error);
                fflush(stdout);
            }
            if (consecutive_errors >= 100u) {
                stop_reason = "socket_error";
                break;
            }
        }
        now_ms = kn_sock_now_ms();
        if (now_ms >= next_poll_ms) {
            c.now_ms = now_ms;
            (void)kn_rx_video_poll(&v, now_ms, dec_frame_in_cb, &c);
            dec_drain_events(&c);
            dec_take_kf(&c);
            dec_drain_queue(&c);
            next_poll_ms = now_ms + KN_SOCK_POLL_INTERVAL_MS;
        }
        if (now_ms - t_last_stats_ms >= 1000u) {
            kn_lctx p;
            t_last_stats_ms = now_ms;
            kn_lctx_init(&p, KN_CH_TRANSPORT, KN_INFO, "decode_stats");
            kn_lctx_i64(&p, "datagrams", (long long)(datagrams % 10000000000ll));
            kn_lctx_i64(&p, "aus", (long long)(c.aus % 1000000000ll));
            kn_lctx_i64(&p, "frames_out",
                        (long long)(c.frames_out % 1000000000ll));
            kn_lctx_i64(&p, "fec_repaired",
                        (long long)(v.fec_repaired_packets % 1000000000ll));
            kn_lctx_i64(&p, "gaps",
                        (long long)(v.packet_gap_recoveries % 1000000000ll));
            kn_lctx_i64(&p, "withheld",
                        (long long)(v.withheld_frames % 1000000000ll));
            kn_lctx_i64(&p, "q_depth", (long long)c.q_count);
            kn_lctx_i64(&p, "q_overflows",
                        (long long)(c.q_overflows % 1000000000ll));
            kn_log_emit(&p);
        }
    }
    if (g_knv_stop) stop_reason = "signal";

    /* --- end-of-stream decoder drain + summary --- */
    if (c.stream && !c.decode_aborted) {
        rc = kn_decode_stream_flush(c.stream, dec_frame_out_cb, &c);
        if (rc < 0 && rc != -4) c.decode_errors++;
    }
    {
        kn_decode_stats dst;
        char drops[512];
        size_t doff = 0;
        int d;
        kn_decode_stream_stats(c.stream, &dst);
        if (dst.decode_ms_max > c.worst_decode_ms)
            c.worst_decode_ms = dst.decode_ms_max;
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
        printf("{\"mode\":\"decode\",\"summary\":{\"port\":%u,"
               "\"seconds_budget\":%ld,\"stop_reason\":\"%s\","
               "\"datagrams\":%llu,\"authenticated\":%llu,"
               "\"aus\":%llu,\"frames\":%llu,\"keyframes\":%llu,"
               "\"frames_decoded\":%llu,\"frames_failed\":%d,"
               "\"decode_errors\":%llu,\"aus_undecoded\":%llu,"
               "\"decode_aborted\":%d,\"worst_decode_ms\":%lld,"
               "\"q_overflows\":%llu,\"fec_repaired\":%llu,\"stale\":%llu,"
               "\"gaps\":%llu,\"withheld\":%llu,\"kf_requests\":%llu,"
               "\"retransmits\":%llu,\"replay_rejected\":%llu,"
               "\"recv_timeouts\":%llu,\"recv_errors\":%llu,"
               "\"ev_overflow\":%u,\"event_lines_capped\":%llu,"
               "\"au_write_errors\":%llu,\"vps\":%u,\"srtp_profile\":%d,"
               "\"rx_state\":%d,\"decoder\":\"%.64s\",\"video\":\"%dx%d\","
               "\"wall_ms\":%llu,\"drops\":{%s}}}\n",
               (unsigned)kn_sock_udp_port(&sock), seconds, stop_reason,
               datagrams % 10000000000ull,
               (unsigned long long)(v.authenticated_packets % 10000000000ull),
               c.aus % 10000000000ull, c.aus % 10000000000ull,
               c.key_aus % 10000000000ull,
               c.frames_out % 10000000000ull, dst.frames_failed,
               c.decode_errors % 1000000000ull,
               c.aus_undecoded % 1000000000ull,
               c.decode_aborted, c.worst_decode_ms % 1000000ll,
               c.q_overflows % 1000000000ull,
               (unsigned long long)(v.fec_repaired_packets % 1000000000ull),
               (unsigned long long)(v.stale_packets % 1000000000ull),
               (unsigned long long)(v.packet_gap_recoveries % 1000000000ull),
               (unsigned long long)(v.withheld_frames % 1000000000ull),
               (unsigned long long)(v.keyframe_requests % 1000000000ull),
               (unsigned long long)(v.recovered_retransmissions % 1000000000ull),
               (unsigned long long)(v.replay_rejections % 1000000000ull),
               (unsigned long long)(sock.recv_timeouts % 10000000000ull),
               (unsigned long long)(sock.recv_errors % 1000000000ull),
               (unsigned)(v.ev_overflow % 1000000u),
               c.event_lines_capped % 1000000000ull,
               c.au_write_errors % 1000000000ull,
               (unsigned)(vps % 100000000u), profile, v.state,
               c.stream ? kn_decode_backend() : "pipeline-only",
               c.last_w % 100000, c.last_h % 100000,
               (unsigned long long)((kn_sock_now_ms() - t_start_ms) %
                                    1000000000ull),
               drops);
        fflush(stdout);
    }

    {
        long long produced = (c.stream && !c.decode_aborted)
                                 ? (long long)c.frames_out
                                 : (long long)c.aus;
        exit_code = (produced > 0 && c.au_write_errors == 0 &&
                     strcmp(stop_reason, "socket_error") != 0) ? 0 : 1;
        if (expect_frames > 0 && produced < expect_frames)
            exit_code = 1;
    }
    if (exit_code != 0) {
        if (ring_ok &&
            snprintf(ringpath, sizeof ringpath, "%s/wire_decode.log",
                     log_dir) < (int)sizeof ringpath)
            (void)kn_ring_dump(&ring, ringpath);
        kn_log_on_failure("session.decode",
                          datagrams == 0 ? "E_DECODE_NO_DATA"
                                         : "E_DECODE_NO_FRAMES",
                          datagrams == 0
                              ? "no datagrams arrived (wrong port? sender "
                                "down? firewall?)"
                              : "datagrams arrived but no access unit "
                                "completed (check the drops map + rx events)",
                          "inspect the summary JSON and the rx event lines; "
                          "the wire mirror is in the log dir; rerun with "
                          "tools/send_rx_capture.py for a known-good source");
    } else {
        kn_lctx e;
        kn_lctx_init(&e, KN_CH_TRANSPORT, KN_INFO, "decode_end");
        kn_lctx_i64(&e, "datagrams", (long long)(datagrams % 10000000000ll));
        kn_lctx_i64(&e, "aus", (long long)(c.aus % 10000000000ll));
        kn_lctx_i64(&e, "frames_out",
                    (long long)(c.frames_out % 10000000000ll));
        kn_lctx_str(&e, "stop_reason", stop_reason);
        kn_log_emit(&e);
    }

    kn_decode_stream_close(c.stream);
    c.stream = NULL;
    if (ring_ok) kn_ring_free(&ring);
    kn_sock_udp_close(&sock);
    free(arena);
    kn_log_shutdown();
    return exit_code;
}

/* ------------------------------------------------------------ session mode
 * knvstd --session — the stage-2 PRODUCTION signaling path (round XXI):
 *
 *   CloudMatch (TLS stream -> kn_http/kn_cm): POST v2/session + poll until
 *     ready (status 2|3 + rtsps endpoint) — the R7 measurement gate.
 *   RTSPS-over-WSS (second TLS stream -> kn_ws -> kn_rtsp): the full
 *     OPTIONS->DESCRIBE->SETUP->ANNOUNCE->PLAY sequence (docs/RTSP.md).
 *   Handoff (docs/RTSP.md §4): SRTP key/salt/profile + the SETUP video
 *     peer -> kn_rx session + a CONNECTED UDP socket (kernel peer filter)
 *     -> the round-XVII media loop (same dec_* machinery as --decode:
 *     capacity-2 queue, overflow policy, JSONL contract — so
 *     make_rx_capture.py --verify and the T13-style checks apply to a
 *     real session's stdout unchanged).
 *
 * SECURITY: TLS-only. kn_net_tls_stream_available()==0 (POSIX/schannel
 * builds) is an honest refusal — NO plaintext fallback exists anywhere on
 * this path. The token is used for the Authorization header only and is
 * logged as LENGTH + PRESENCE (§7). SRTP key material is never logged
 * (lengths only).
 *
 * DEVIATION D-s1 (documented, UNVERIFIED until the first real session):
 * the ANNOUNCE carries LOCALLY GENERATED ICE ufrag/pwd and a random
 * 32-byte placeholder DTLS fingerprint — the real bundle identity needs
 * the stage-3 DTLS certificate (R3 spike). The Mjolnir video flow
 * (SRTP/UDP) is independent of the bundle DTLS, so stage-2 video should
 * flow; the rig's bundle side will fail its DTLS and that is expected
 * until stage 3. The first real rig is the measurement (R32's rule).
 *
 * The media loop below deliberately DUPLICATES run_decode's loop instead
 * of refactoring the T13-verified function (churn discipline); unification
 * is planned once --session has its own target evidence.
 */

typedef struct {
    kn_net_tls_stream *s;
} tls_ctx;

static int tls_send(void *c, const uint8_t *b, size_t n)
{
    return kn_net_tls_stream_send(((tls_ctx *)c)->s, b, n);
}

static int tls_recv(void *c, uint8_t *b, size_t cap, uint32_t t)
{
    return kn_net_tls_stream_recv(((tls_ctx *)c)->s, b, cap, t);
}

static int sess_ws_send_text(void *c, const char *t, size_t n)
{
    return kn_ws_send_text((kn_ws_client *)c, t, n);
}

static int sess_ws_recv_text(void *c, char *buf, size_t cap, uint32_t t,
                             size_t *out)
{
    kn_ws_client *w = (kn_ws_client *)c;
    int op = 0;
    size_t n = 0;
    int rc = kn_ws_recv_msg(w, t, &op, &n);
    if (rc == KN_WS_ERR_TIMEOUT) return KN_RTSP_ERR_TIMEOUT;
    if (rc != KN_WS_OK) return KN_RTSP_ERR_IO;
    if (op != KN_WS_OP_TEXT || n + 1 > cap) return KN_RTSP_ERR_IO;
    memcpy(buf, w->msg, n);
    buf[n] = '\0';
    *out = n;
    return KN_RTSP_OK;
}

static uint64_t sess_now(void *c) { (void)c; return kn_sock_now_ms(); }
static void sess_sleep(uint32_t ms, void *c)
{
    (void)c;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    {
        struct timespec ts;
        ts.tv_sec = ms / 1000u;
        ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
        (void)nanosleep(&ts, NULL);
    }
#endif
}

/* "rtsps://host:port" -> host text + port (bounded, no allocation) */
static int sess_parse_rtsps(const char *url, char *host, size_t hostsz,
                            uint16_t *port)
{
    const char *h, *colon, *end;
    size_t n;
    if (!url || strncmp(url, "rtsps://", 8) != 0) return -1;
    h = url + 8;
    end = h + strcspn(h, "/?#");
    colon = end;
    while (colon > h && *colon != ':') colon--;
    if (colon == h || colon >= end) return -1;
    n = (size_t)(colon - h);
    if (n == 0 || n + 1 > hostsz) return -1;
    memcpy(host, h, n);
    host[n] = '\0';
    {
        long p = strtol(colon + 1, NULL, 10);
        if (p < 1 || p > 65535) return -1;
        *port = (uint16_t)p;
    }
    return 0;
}

/* SRTP profile string -> kn_rx profile + tag + key/salt sizes */
static int sess_profile_map(const char *name, int *profile, size_t *tag,
                            int *key_len, int *salt_len)
{
    if (strcmp(name, "AEAD_AES_128_GCM_8") == 0)
        { *profile = KN_RX_SRTP_GCM128; *tag = 8;  *key_len = 16; *salt_len = 12; }
    else if (strcmp(name, "AEAD_AES_256_GCM_8") == 0)
        { *profile = KN_RX_SRTP_GCM256; *tag = 8;  *key_len = 32; *salt_len = 12; }
    else if (strcmp(name, "AEAD_AES_128_GCM") == 0)
        { *profile = KN_RX_SRTP_GCM128; *tag = 16; *key_len = 16; *salt_len = 12; }
    else if (strcmp(name, "AEAD_AES_256_GCM") == 0)
        { *profile = KN_RX_SRTP_GCM256; *tag = 16; *key_len = 32; *salt_len = 12; }
    else if (strcmp(name, "AES_CM_128_HMAC_SHA1_32") == 0)
        { *profile = KN_RX_SRTP_CM128_HMAC_SHA1; *tag = 4;  *key_len = 16; *salt_len = 14; }
    else if (strcmp(name, "AES_CM_128_HMAC_SHA1_80") == 0)
        { *profile = KN_RX_SRTP_CM128_HMAC_SHA1; *tag = 10; *key_len = 16; *salt_len = 14; }
    else if (strcmp(name, "AES_CM_256_HMAC_SHA1_32") == 0)
        { *profile = KN_RX_SRTP_CM256_HMAC_SHA1; *tag = 4;  *key_len = 32; *salt_len = 14; }
    else if (strcmp(name, "AES_CM_256_HMAC_SHA1_80") == 0)
        { *profile = KN_RX_SRTP_CM256_HMAC_SHA1; *tag = 10; *key_len = 32; *salt_len = 14; }
    else return -1;
    return 0;
}

static void sess_hex_upper(const uint8_t *b, size_t n, char *out)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; ++i) {
        out[2 * i]     = hx[b[i] >> 4];
        out[2 * i + 1] = hx[b[i] & 0x0Fu];
    }
    out[2 * n] = '\0';
}

/* the D-s1 placeholder bundle identity: random ufrag/pwd (base64-ish hex)
 * + a random 32-byte fingerprint in colon-hex. Syntactically valid,
 * cryptographically inert until stage 3. */
static void sess_local_identity(char *ufrag, size_t ufsz, char *pwd,
                                size_t pwsz, char *fp, size_t fpsz)
{
    uint8_t raw[32];
    if (kn_ws_rand(raw, sizeof raw) != 0) {
        /* the CSPRNG refusing is fatal for the session anyway; fill
         * deterministic placeholders so the SDP stays well-formed and the
         * failure surfaces at the handshake, not as garbage */
        memset(raw, 0x5A, sizeof raw);
    }
    sess_hex_upper(raw, 8, ufrag);        (void)ufsz;   /* 16 hex chars */
    sess_hex_upper(raw + 8, 12, pwd);     (void)pwsz;   /* 24 hex chars */
    {
        size_t i, o = 0;
        for (i = 0; i < 32 && o + 3 < fpsz; ++i) {
            static const char hx[] = "0123456789ABCDEF";
            if (i) fp[o++] = ':';
            fp[o++] = hx[raw[i] >> 4];
            fp[o++] = hx[raw[i] & 0x0Fu];
        }
        fp[o] = '\0';
    }
}

#define SESS_JSON_CAP  (128u * 1024u)
#define SESS_RTSP_BUF  (KN_RTSP_MAX_RESPONSE + 4096u)

static int run_session(const char *token, const char *device_id,
                       long long app_id, const char *title,
                       const char *cm_host, long seconds,
                       long queue_timeout, long expect_frames,
                       const char *au_dir, const char *log_dir)
{
    static uint8_t cm_rxb[64 * 1024];
    static char cm_json[SESS_JSON_CAP];
    static char rtsp_buf[SESS_RTSP_BUF];
    static uint8_t ws_msg[KN_RTSP_MAX_RESPONSE + 8192];
    static uint8_t pkt[KN_SOCK_RECV_BUF];
    static dec_ctx c;
    static char ann[16384], neg[16384], hostbuf[256];

    kn_cm_settings st;
    kn_cm_session sess;
    kn_net_tls_stream *cm_tls = NULL, *sig_tls = NULL;
    kn_ws_client wsc;
    kn_rtsp_client rtc;
    kn_rtsp_response resp;
    kn_rtsp_describe desc;
    kn_rtsp_setup_result setup;
    kn_sock_udp sock;
    kn_ring ring;
    kn_rx_video v;
    kn_rx_mem mem;
    kn_rx_video_config cfg;
    kn_rx_srtp_session srtp_sess;
    uint8_t *arena = NULL;
    tls_ctx cm_io_ctx, sig_io_ctx;
    kn_http_io hio;
    kn_ws_io wio;
    kn_rtsp_msg_io rio;
    kn_rtsp_header common[3];
    char err[192], line[512];
    char ufrag[24], pwd[32], fp[128];
    char key_hex[65], salt_hex[25];
    uint8_t key_bin[32], salt_bin[14];
    uint32_t key_id = 0;
    int profile = KN_RX_SRTP_NONE, key_len = 32, salt_len = 12, have_sess = 0;
    size_t tag_len = 8;
    const char *profile_name;
    int rc, http_status = 0, ring_ok, exit_code = 1, i;
    uint16_t ws_port = 0;
    uint64_t t_start, next_poll_ms, now_ms, t_last_stats;
    unsigned long long datagrams = 0;
    unsigned consecutive_errors = 0;
    const char *stop_reason = "time";
    kn_log_config lcfg;

    memset(&desc, 0, sizeof desc);
    memset(&sess, 0, sizeof sess);
    memset(&setup, 0, sizeof setup);
    knv_stop_install();
    memset(&c, 0, sizeof c);
    for (i = 0; i < (int)DEC_QUEUE_CAP; ++i)
        c.qbytes[i] = g_dec_qbytes[i];
    if (au_dir && au_dir[0]) {
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
    (void)kn_diag_install(log_dir);
    c.au_dir = au_dir;

    if (!kn_net_tls_stream_available()) {
        printf("{\"mode\":\"session\",\"error\":\"tls_stream_unavailable\","
               "\"note\":\"the production signaling path needs the Win32 "
               "mbedTLS build (KN_TLS_BACKEND=mbedtls)\"}\n");
        fflush(stdout);
        kn_log_shutdown();
        return 3;
    }

    /* ---------------- phase A: CloudMatch ---------------- */
    memset(&st, 0, sizeof st);
    st.token = token;
    st.device_id = device_id;
    st.app_id = app_id;
    st.title = title;
    st.width = 1280; st.height = 720; st.fps = 30;      /* D14 */
    st.timezone_offset_ms = 0;                          /* UTC-neutral */

    {
        kn_lctx b;
        kn_lctx_init(&b, KN_CH_HTTP, KN_INFO, "cm_begin");
        kn_lctx_str(&b, "host", cm_host);
        kn_lctx_i64(&b, "app_id", app_id);
        kn_lctx_i64(&b, "token_len", (long long)strlen(token));
        kn_lctx_str(&b, "token", "redacted");
        kn_log_emit(&b);
    }
    cm_tls = kn_net_tls_stream_open(cm_host, 443, 15000, err, sizeof err);
    if (!cm_tls) {
        printf("{\"mode\":\"session\",\"error\":\"cm_tls\",\"detail\":\"%.150s\"}\n",
               err);
        fflush(stdout);
        kn_log_on_failure("session.cm", "E_SESSION_CM_TLS",
                          "CloudMatch TLS stream failed to open", err);
        kn_log_shutdown();
        return 1;
    }
    cm_io_ctx.s = cm_tls;
    hio.send = tls_send; hio.recv = tls_recv; hio.ctx = &cm_io_ctx;
    rc = kn_cm_create(hio, cm_host, "/", &st, 20000, sess_now, NULL,
                      cm_rxb, sizeof cm_rxb, cm_json, sizeof cm_json,
                      &http_status, &sess);
    kn_net_tls_stream_close(cm_tls);
    cm_tls = NULL;
    if (rc != KN_HTTP_OK || http_status != 200) {
        printf("{\"mode\":\"session\",\"error\":\"cm_create\",\"rc\":%d,"
               "\"http_status\":%d}\n", rc, http_status);
        fflush(stdout);
        kn_log_on_failure("session.cm", "E_SESSION_CM_CREATE",
                          "POST v2/session did not return a usable session",
                          "check the account/token/entitlements; the raw "
                          "status is in this log (R7 measurement)");
        kn_log_shutdown();
        return 1;
    }
    printf("{\"mode\":\"session\",\"event\":\"cm_created\",\"session_id\":"
           "\"%.64s\",\"status\":%lld,\"phase\":\"%s\"}\n",
           sess.session_id, (long long)sess.status, sess.phase);
    fflush(stdout);

    /* poll until ready (one fresh TLS connection per poll — Connection:
     * close semantics; 2 s cadence, bounded by queue_timeout) */
    t_start = kn_sock_now_ms();
    while (!sess.ready && !g_knv_stop) {
        if (queue_timeout > 0 &&
            kn_sock_now_ms() - t_start >= (uint64_t)queue_timeout * 1000u) {
            stop_reason = "queue_timeout";
            break;
        }
        if (sess.status == 7) { stop_reason = "terminated"; break; }
        sess_sleep(2000, NULL);
        cm_tls = kn_net_tls_stream_open(cm_host, 443, 15000, err,
                                        sizeof err);
        if (!cm_tls) { stop_reason = "cm_tls_retry"; break; }
        cm_io_ctx.s = cm_tls;
        rc = kn_cm_poll(hio, cm_host, "/", &st, sess.session_id, 20000,
                        sess_now, NULL, cm_rxb, sizeof cm_rxb, cm_json,
                        sizeof cm_json, &http_status, &sess);
        kn_net_tls_stream_close(cm_tls);
        cm_tls = NULL;
        if (rc != KN_HTTP_OK || http_status != 200) {
            stop_reason = "cm_poll_failed";
            break;
        }
        printf("{\"mode\":\"session\",\"event\":\"queue\",\"status\":%lld,"
               "\"phase\":\"%s\",\"queue_position\":%lld,\"have_queue\":%d,"
               "\"seat_step\":%lld,\"ready\":%d}\n",
               (long long)sess.status, sess.phase,
               (long long)sess.queue_position, sess.have_queue,
               (long long)sess.seat_setup_step, sess.ready);
        fflush(stdout);
    }
    if (!sess.ready) {
        printf("{\"mode\":\"session\",\"error\":\"not_ready\","
               "\"stop_reason\":\"%s\",\"status\":%lld}\n", stop_reason,
               (long long)sess.status);
        fflush(stdout);
        kn_log_on_failure("session.cm", "E_SESSION_NOT_READY",
                          "the session never reached ready state",
                          "queue position/seat step lines above show where "
                          "it stalled; entitlements/queue are server-side");
        kn_log_shutdown();
        return 1;
    }
    {
        char esc[300];
        lb_json_path(esc, sizeof esc, sess.rtsps_endpoint);
        printf("{\"mode\":\"session\",\"event\":\"ready\",\"rtsps\":\"%s\","
               "\"media_peer\":\"%.64s:%u\"}\n", esc, sess.media_ip,
               (unsigned)sess.media_port);
        fflush(stdout);
    }

    /* ---------------- phase B: RTSPS over WSS ---------------- */
    if (sess_parse_rtsps(sess.rtsps_endpoint, hostbuf, sizeof hostbuf,
                         &ws_port) != 0) {
        printf("{\"mode\":\"session\",\"error\":\"bad_rtsps_endpoint\"}\n");
        fflush(stdout);
        kn_log_shutdown();
        return 1;
    }
    sig_tls = kn_net_tls_stream_open(hostbuf, ws_port, 15000, err,
                                     sizeof err);
    if (!sig_tls) {
        char esc[200];
        lb_json_path(esc, sizeof esc, err);
        printf("{\"mode\":\"session\",\"error\":\"sig_tls\","
               "\"detail\":\"%.150s\"}\n", esc);
        fflush(stdout);
        kn_log_on_failure("session.rtsp", "E_SESSION_SIG_TLS",
                          "the signaling TLS stream failed to open", err);
        kn_log_shutdown();
        return 1;
    }
    sig_io_ctx.s = sig_tls;
    wio.send = tls_send; wio.recv = tls_recv; wio.ctx = &sig_io_ctx;
    snprintf(line, sizeof line,
             "x-nv-sessionid: %s\r\ncontent-length: 0\r\n", sess.session_id);
    rc = kn_ws_client_open(&wsc, wio, hostbuf, ws_port, "/rtsp", line,
                           15000, ws_msg, sizeof ws_msg, err, sizeof err);
    if (rc != KN_WS_OK) {
        printf("{\"mode\":\"session\",\"error\":\"ws_open\",\"rc\":%d,"
               "\"detail\":\"%.120s\"}\n", rc, err);
        fflush(stdout);
        kn_log_on_failure("session.rtsp", "E_SESSION_WS",
                          "the RTSPS WebSocket handshake failed", err);
        kn_net_tls_stream_close(sig_tls);
        kn_log_shutdown();
        return 1;
    }
    rio.send_text = sess_ws_send_text;
    rio.recv_text = sess_ws_recv_text;
    rio.ctx = &wsc;
    (void)kn_rtsp_client_init(&rtc, rio, rtsp_buf, sizeof rtsp_buf,
                              KN_RTSP_TIMEOUT_MS, sess_now, NULL,
                              sess_sleep, NULL);

    common[0].name = "X-GS-Version";   common[0].value = "14.2";
    common[1].name = "Host";           common[1].value = hostbuf;
    common[2].name = "x-nv-sessionid"; common[2].value = sess.session_id;

    rc = kn_rtsp_request(&rtc, "OPTIONS", sess.rtsps_endpoint, common, 3,
                         NULL, 0, 0, &resp);
    printf("{\"mode\":\"session\",\"event\":\"rtsp\",\"step\":\"OPTIONS\","
           "\"rc\":%d,\"status\":%d}\n", rc, rc == KN_RTSP_OK ? resp.status : 0);
    fflush(stdout);
    if (rc != KN_RTSP_OK || resp.status != 200) goto teardown;
    {
        kn_rtsp_header dh[5];
        dh[0] = common[0]; dh[1] = common[1]; dh[2] = common[2];
        dh[3].name = "Accept"; dh[3].value = "application/sdp";
        dh[4].name = "x-nv-abtesting"; dh[4].value = "2";
        rc = kn_rtsp_request(&rtc, "DESCRIBE", sess.rtsps_endpoint, dh, 5,
                             NULL, 0, 0, &resp);
    }
    printf("{\"mode\":\"session\",\"event\":\"rtsp\",\"step\":\"DESCRIBE\","
           "\"rc\":%d,\"status\":%d}\n", rc, rc == KN_RTSP_OK ? resp.status : 0);
    fflush(stdout);
    if (rc != KN_RTSP_OK || resp.status != 200) goto teardown;
    if (kn_rtsp_extract_describe(&resp, &desc) != KN_RTSP_OK) {
        printf("{\"mode\":\"session\",\"error\":\"describe_extract\"}\n");
        fflush(stdout);
        goto teardown;
    }
    if (!desc.native_bundle) {
        printf("{\"mode\":\"session\",\"error\":\"legacy_transport\","
               "\"note\":\"nativeRtcOnBundlePort!=1 — retired multi-socket "
               "transport refused (:1141)\"}\n");
        fflush(stdout);
        goto teardown;
    }
    {   /* the DESCRIBE SDP is the negotiate input later — copy it now
         * (resp.body aliases the rtsp buffer) */
        static char describe_sdp[64 * 1024];
        size_t dl = resp.body_len < sizeof describe_sdp - 1
                       ? resp.body_len : sizeof describe_sdp - 1;
        kn_rtsp_header ah[5];
        kn_rtsp_announce_params ap;
        size_t alen = 0, nlen = 0;
        memcpy(describe_sdp, resp.body, dl);
        describe_sdp[dl] = '\0';

        /* ---------------- phase C: socket + SETUP + ANNOUNCE + PLAY --- */
        err[0] = '\0';
        rc = kn_sock_udp_open(&sock, NULL, 0, KN_SOCK_RCVBUF_DEFAULT,
                              err, sizeof err);
        if (rc != KN_SOCK_OK) {
            printf("{\"mode\":\"session\",\"error\":\"media_socket\","
                   "\"detail\":\"%.120s\"}\n", err);
            fflush(stdout);
            goto teardown;
        }
        rc = kn_rtsp_setup_video(&rtc, desc.video_control,
                                 sess.rtsps_endpoint, desc.session,
                                 desc.ping_version,
                                 kn_sock_udp_port(&sock), common, 3,
                                 sess.have_media ? sess.media_ip : NULL,
                                 sess.have_media ? sess.media_port : 0,
                                 &setup);
        printf("{\"mode\":\"session\",\"event\":\"rtsp\",\"step\":\"SETUP\","
               "\"rc\":%d,\"status\":%d,\"peer\":\"%.64s:%u\",\"bundle_fb\":%d,"
               "\"rounds\":%u}\n", rc, setup.status, setup.peer_ip,
               (unsigned)setup.peer_port, setup.bundle_fallback,
               setup.rounds_slept);
        fflush(stdout);
        if (rc != KN_RTSP_OK) goto teardown_sock;

        /* SRTP material (docs/RTSP.md §2): DESCRIBE runtime key or a
         * locally generated one; salt = {key_id:024X} */
        if (desc.have_runtime_key) {
            snprintf(key_hex, sizeof key_hex, "%s", desc.runtime_key);
            key_id = desc.key_id;
        } else {
            uint8_t raw[32];
            if (kn_ws_rand(raw, sizeof raw) != 0) {
                printf("{\"mode\":\"session\",\"error\":\"csprng\"}\n");
                fflush(stdout);
                goto teardown_sock;
            }
            sess_hex_upper(raw, 32, key_hex);
            key_id = (uint32_t)(kn_sock_now_us() & 0xFFFFFFFFu) | 1u;
        }
        kn_rtsp_salt_from_key_id(key_id, salt_hex);
        profile_name = kn_rtsp_srtp_profile(NULL, describe_sdp);
        if (sess_profile_map(profile_name, &profile, &tag_len, &key_len,
                             &salt_len) != 0) {
            printf("{\"mode\":\"session\",\"error\":\"unknown_profile\","
                   "\"profile\":\"%.40s\"}\n", profile_name);
            fflush(stdout);
            goto teardown_sock;
        }
        {   /* hex -> binary (validated: runtime_key guarantees 64 hex) */
            int k;
            for (k = 0; k < key_len; ++k) {
                unsigned bv;
                if (sscanf(key_hex + 2 * k, "%2x", &bv) != 1) bv = 0;
                key_bin[k] = (uint8_t)bv;
            }
            for (k = 0; k < 12; ++k) {
                unsigned bv;
                if (sscanf(salt_hex + 2 * k, "%2x", &bv) != 1) bv = 0;
                salt_bin[k] = (uint8_t)bv;
            }
            /* CM profiles want a 14-byte salt: the 12-byte runtime salt
             * zero-extends (the KDF consumes it as the master salt; the
             * reference's material derivation is the authority — kn_rx
             * validates lengths and refuses honestly) */
            if (salt_len > 12) {
                salt_bin[12] = 0;
                salt_bin[13] = 0;
            }
        }

        /* connect the media socket to the SETUP peer BEFORE announcing:
         * the kernel picks the route's source address and getsockname
         * (inside kn_sock_udp_connect) exposes it — the ANNOUNCE then
         * carries the REAL local address like the reference's reserved
         * bundle socket does */
        {
            uint32_t peer_ip = 0;
            if (kn_sock_ipv4_parse(setup.peer_ip, &peer_ip) != 0) {
                printf("{\"mode\":\"session\",\"error\":\"peer_not_ipv4\","
                       "\"note\":\"IPv6 peers need kn_sock v6 (stage 5)\"}\n");
                fflush(stdout);
                goto teardown_sock;
            }
            if (kn_sock_udp_connect(&sock, peer_ip, setup.peer_port) !=
                KN_SOCK_OK) {
                printf("{\"mode\":\"session\",\"error\":\"media_connect\"}\n");
                fflush(stdout);
                goto teardown_sock;
            }
        }

        sess_local_identity(ufrag, sizeof ufrag, pwd, sizeof pwd,
                            fp, sizeof fp);
        memset(&ap, 0, sizeof ap);
        ap.width = 1280; ap.height = 720; ap.fps = 30;         /* D14 */
        ap.bitrate_kbps = 5000;          /* stage-5 will cap 3-6 Mbps */
        ap.bitstream_format = 0;         /* H264 (D3/D6 decode path) */
        ap.bit_depth = 0; ap.chroma_format = 1;
        ap.hdr = 0;
        snprintf(ap.key_hex, sizeof ap.key_hex, "%s", key_hex);
        ap.key_id = key_id;
        ap.bundle_port = kn_sock_udp_port(&sock);
        {
            char iptext[16];
            if (sock.local_addr == 0)
                snprintf(iptext, sizeof iptext, "127.0.0.1");
            else
                (void)kn_sock_ipv4_text(sock.local_addr, iptext);
            snprintf(ap.local_address, sizeof ap.local_address, "%s",
                     iptext);
        }
        snprintf(ap.ufrag, sizeof ap.ufrag, "%s", ufrag);
        snprintf(ap.password, sizeof ap.password, "%s", pwd);
        snprintf(ap.fingerprint, sizeof ap.fingerprint, "%s", fp);
        ap.video_port = setup.peer_port;
        ap.video_packet_size = KN_RX_DEFAULT_VIDEO_PACKET_SIZE;
        ap.rtcp_on_sctp = desc.rtcp_on_sctp;
        ap.unified_socket = desc.unified_socket;
        ap.qos_timings_v5 = 0;
        ap.dynamic_streaming_mode = 0;
        ap.adjust_res_and_fps = 0;
        if (kn_rtsp_build_announce(&ap, ann, sizeof ann, &alen) !=
            KN_RTSP_OK) {
            printf("{\"mode\":\"session\",\"error\":\"announce_build\"}\n");
            fflush(stdout);
            goto teardown_sock;
        }
        if (kn_rtsp_negotiate_announce(ann, describe_sdp, neg, sizeof neg,
                                       &nlen, NULL) != KN_RTSP_OK) {
            printf("{\"mode\":\"session\",\"error\":\"announce_negotiate\"}\n");
            fflush(stdout);
            goto teardown_sock;
        }
        ah[0] = common[0]; ah[1] = common[1]; ah[2] = common[2];
        ah[3].name = "Session"; ah[3].value = desc.session;
        ah[4].name = "Content-Type"; ah[4].value = "application/sdp";
        rc = kn_rtsp_request(&rtc, "ANNOUNCE", sess.rtsps_endpoint, ah, 5,
                             neg, nlen, 0, &resp);
        printf("{\"mode\":\"session\",\"event\":\"rtsp\",\"step\":\"ANNOUNCE\","
               "\"rc\":%d,\"status\":%d,\"sdp_bytes\":%lu}\n", rc,
               rc == KN_RTSP_OK ? resp.status : 0,
               (unsigned long)(nlen % 1000000ul));
        fflush(stdout);
        if (rc != KN_RTSP_OK || resp.status != 200) goto teardown_sock;
        if (!desc.disable_play) {
            rc = kn_rtsp_request(&rtc, "PLAY", sess.rtsps_endpoint, ah, 4,
                                 NULL, 0, 0, &resp);
            printf("{\"mode\":\"session\",\"event\":\"rtsp\",\"step\":\"PLAY\","
                   "\"rc\":%d,\"status\":%d}\n", rc,
                   rc == KN_RTSP_OK ? resp.status : 0);
            fflush(stdout);
            if (rc != KN_RTSP_OK || resp.status != 200) goto teardown_sock;
        }
        /* the handoff line — shapes and LENGTHS only, never key material
         * (docs/RTSP.md §4 logging rule) */
        printf("{\"mode\":\"session\",\"event\":\"handoff\","
               "\"video_peer\":\"%.64s:%u\",\"packet_size\":%u,"
               "\"srtp_profile\":\"%.24s\",\"tag_len\":%lu,"
               "\"key_len\":%d,\"key_id\":%u,\"key_source\":\"%s\","
               "\"rtcp_on_sctp\":%d,\"unified_socket\":%d,"
               "\"ping_version\":%u}\n",
               setup.peer_ip, (unsigned)setup.peer_port,
               (unsigned)ap.video_packet_size, profile_name,
               (unsigned long)tag_len, key_len, (unsigned)key_id,
               desc.have_runtime_key ? "describe" : "local",
               desc.rtcp_on_sctp, desc.unified_socket,
               (unsigned)setup.ping_version);
        fflush(stdout);

        /* ---------------- phase D: the media pipeline ---------------- */
        rc = kn_rx_srtp_session_init(&srtp_sess, profile, key_bin,
                                     (size_t)key_len, salt_bin,
                                     (size_t)salt_len, tag_len);
        if (rc != KN_RX_OK) {
            printf("{\"mode\":\"session\",\"error\":\"srtp_session\","
                   "\"rc\":%d,\"backend_gcm\":%d,\"backend_256\":%d}\n",
                   rc, kn_srtp_backend_gcm(), kn_srtp_backend_256());
            fflush(stdout);
            goto teardown_sock;
        }
        {   /* arena: same sizing discipline as run_decode (vps 1280) */
            uint32_t vps = KN_RX_DEFAULT_VIDEO_PACKET_SIZE;
            uint32_t shard_len = vps + KN_RX_FEC_HEADER_ALLOWANCE;
            uint32_t pkt_cap = (shard_len + 64u + 7u) & ~7u;
            uint32_t slots, scratch_len, fec_blocks = 32u, fec_shards = 64u;
            size_t arena_need, off;
            size_t pool_budget = 32u * 1024u * 1024u;
            memset(&cfg, 0, sizeof cfg);
            cfg.codec = KN_GS_CODEC_H264;
            cfg.reorder_window_packets = KN_RX_DEFAULT_REORDER_WINDOW;
            cfg.max_access_unit_bytes = KN_GS_DEFAULT_MAX_ACCESS_UNIT;
            cfg.video_packet_size = vps;
            cfg.timeout_ms = KN_RX_DEFAULT_TIMEOUT_MS;
            slots = cfg.reorder_window_packets + 1u +
                    fec_blocks * fec_shards + 16u;
            if ((size_t)slots * pkt_cap > pool_budget)
                slots = (uint32_t)(pool_budget / pkt_cap);
            scratch_len = ((uint32_t)kn_fec_scratch_bytes(fec_shards,
                                                          shard_len) +
                           4096u + 7u) & ~7u;
            arena_need = (size_t)slots * sizeof(kn_rx_pkt)
                       + (size_t)slots * pkt_cap
                       + (size_t)(cfg.reorder_window_packets + 1u) *
                             sizeof(kn_rx_reorder_entry)
                       + (size_t)fec_blocks * sizeof(kn_rx_fec_block)
                       + (size_t)fec_blocks * fec_shards * sizeof(uint32_t)
                       + scratch_len
                       + (size_t)KN_RX_NACK_MAX_PENDING *
                             sizeof(kn_rx_nack_entry)
                       + cfg.max_access_unit_bytes
                       + 16u * sizeof(kn_rx_event);
            arena = (uint8_t *)malloc(arena_need);
            if (!arena) {
                printf("{\"mode\":\"session\",\"error\":\"arena_alloc\"}\n");
                fflush(stdout);
                goto teardown_sock;
            }
            memset(&mem, 0, sizeof mem);
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
        if (kn_rx_video_init(&v, &cfg, &mem, kn_sock_now_ms()) !=
            KN_RX_OK) {
            printf("{\"mode\":\"session\",\"error\":\"pipeline_init\"}\n");
            fflush(stdout);
            free(arena);
            arena = NULL;
            goto teardown_sock;
        }
        kn_rx_video_set_srtp(&v, &srtp_sess);
        have_sess = 1;
        c.v = &v;
        c.decode_available = kn_decode_available();
        if (c.decode_available) {
            char derr[128];
            c.stream = kn_decode_stream_open(1, derr, sizeof derr);
            if (!c.stream) c.decode_available = 0;
        }
        ring_ok = (kn_ring_init(&ring, 64u * 1024u, 1312) == 0);

        /* ---------------- phase E: the media loop (run_decode shape) --- */
        t_start = kn_sock_now_ms();
        t_last_stats = t_start;
        next_poll_ms = t_start;
        stop_reason = "time";
        while (!g_knv_stop) {
            size_t plen = 0;
            now_ms = kn_sock_now_ms();
            if (seconds > 0 &&
                now_ms - t_start >= (uint64_t)seconds * 1000u) {
                stop_reason = "time";
                break;
            }
            if (expect_frames > 0 &&
                (long long)((c.stream && !c.decode_aborted) ? c.frames_out
                                                            : c.aus) >=
                    expect_frames) {
                stop_reason = "expect_frames";
                break;
            }
            rc = kn_sock_udp_recv(&sock, pkt, sizeof pkt,
                                  KN_SOCK_POLL_INTERVAL_MS, &plen, NULL);
            c.now_ms = kn_sock_now_ms();
            if (rc == 1) {
                consecutive_errors = 0;
                datagrams++;
                if (ring_ok)
                    kn_ring_append(&ring, kn_sock_now_us(),
                                   knv_rtp_seq_meta(pkt, plen), pkt, plen);
                (void)kn_rx_video_process_datagram(&v, pkt, plen, c.now_ms,
                                                   dec_frame_in_cb, &c);
                dec_drain_events(&c);
                dec_take_kf(&c);
                dec_drain_queue(&c);
            } else if (rc < 0) {
                consecutive_errors++;
                if (consecutive_errors <= 5) {
                    printf("{\"mode\":\"session\",\"error\":\"recv\","
                           "\"rc\":%d,\"errno\":%d}\n", rc,
                           sock.last_error);
                    fflush(stdout);
                }
                if (consecutive_errors >= 100u) {
                    stop_reason = "socket_error";
                    break;
                }
            }
            now_ms = kn_sock_now_ms();
            if (now_ms >= next_poll_ms) {
                c.now_ms = now_ms;
                (void)kn_rx_video_poll(&v, now_ms, dec_frame_in_cb, &c);
                dec_drain_events(&c);
                dec_take_kf(&c);
                dec_drain_queue(&c);
                next_poll_ms = now_ms + KN_SOCK_POLL_INTERVAL_MS;
            }
            if (now_ms - t_last_stats >= 1000u) {
                kn_lctx p;
                t_last_stats = now_ms;
                kn_lctx_init(&p, KN_CH_TRANSPORT, KN_INFO, "session_stats");
                kn_lctx_i64(&p, "datagrams",
                            (long long)(datagrams % 10000000000ll));
                kn_lctx_i64(&p, "aus", (long long)(c.aus % 1000000000ll));
                kn_lctx_i64(&p, "frames_out",
                            (long long)(c.frames_out % 1000000000ll));
                kn_lctx_i64(&p, "fec_repaired",
                            (long long)(v.fec_repaired_packets % 1000000000ll));
                kn_lctx_i64(&p, "gaps",
                            (long long)(v.packet_gap_recoveries % 1000000000ll));
                kn_lctx_i64(&p, "withheld",
                            (long long)(v.withheld_frames % 1000000000ll));
                kn_lctx_i64(&p, "q_depth", (long long)c.q_count);
                kn_log_emit(&p);
            }
        }
        if (g_knv_stop) stop_reason = "signal";

        if (c.stream && !c.decode_aborted)
            (void)kn_decode_stream_flush(c.stream, dec_frame_out_cb, &c);
        {   /* summary — field-compatible with the decode/replay contract
             * (make_rx_capture.py --verify consumes it unchanged) */
            char drops[512];
            kn_decode_stats dst;
            size_t doff = 0;
            int d;
            kn_decode_stream_stats(c.stream, &dst);
            for (d = 1; d < 24; ++d) {
                if (!v.drop_counters[d]) continue;
                if (doff + 48 < sizeof drops)
                    doff += (size_t)snprintf(drops + doff,
                                             sizeof drops - doff,
                                             "%s\"%d\":%llu",
                                             doff ? "," : "", d,
                                             (unsigned long long)
                                             (v.drop_counters[d] %
                                              1000000000ull));
            }
            if (doff >= sizeof drops) doff = sizeof drops - 1;
            drops[doff] = '\0';
            printf("{\"mode\":\"session\",\"summary\":{\"stop_reason\":\"%s\","
                   "\"session_id\":\"%.64s\",\"datagrams\":%llu,"
                   "\"authenticated\":%llu,\"aus\":%llu,\"frames\":%llu,"
                   "\"keyframes\":%llu,\"frames_decoded\":%llu,"
                   "\"frames_failed\":%d,\"decode_errors\":%llu,"
                   "\"q_overflows\":%llu,\"fec_repaired\":%llu,"
                   "\"stale\":%llu,\"gaps\":%llu,\"withheld\":%llu,"
                   "\"kf_requests\":%llu,\"recv_timeouts\":%llu,"
                   "\"recv_errors\":%llu,\"event_lines_capped\":%llu,"
                   "\"au_write_errors\":%llu,\"srtp_profile\":\"%.24s\","
                   "\"rx_state\":%d,\"decoder\":\"%.64s\",\"video\":\"%dx%d\","
                   "\"wall_ms\":%llu,\"drops\":{%s}}}\n",
                   stop_reason, sess.session_id,
                   datagrams % 10000000000ull,
                   (unsigned long long)(v.authenticated_packets %
                                        10000000000ull),
                   c.aus % 10000000000ull, c.aus % 10000000000ull,
                   c.key_aus % 10000000000ull,
                   c.frames_out % 10000000000ull,
                   dst.frames_failed, c.decode_errors % 1000000000ull,
                   c.q_overflows % 1000000000ull,
                   (unsigned long long)(v.fec_repaired_packets %
                                        1000000000ull),
                   (unsigned long long)(v.stale_packets % 1000000000ull),
                   (unsigned long long)(v.packet_gap_recoveries %
                                        1000000000ull),
                   (unsigned long long)(v.withheld_frames % 1000000000ull),
                   (unsigned long long)(v.keyframe_requests %
                                        1000000000ull),
                   (unsigned long long)(sock.recv_timeouts %
                                        10000000000ull),
                   (unsigned long long)(sock.recv_errors % 1000000000ull),
                   c.event_lines_capped % 1000000000ull,
                   c.au_write_errors % 1000000000ull,
                   profile_name, v.state,
                   c.stream ? kn_decode_backend() : "pipeline-only",
                   c.last_w % 100000, c.last_h % 100000,
                   (unsigned long long)((kn_sock_now_ms() - t_start) %
                                        1000000000ull),
                   drops);
            fflush(stdout);
        }
        {
            long long produced = (c.stream && !c.decode_aborted)
                                     ? (long long)c.frames_out
                                     : (long long)c.aus;
            exit_code = (datagrams > 0 && produced > 0 &&
                         c.au_write_errors == 0 &&
                         strcmp(stop_reason, "socket_error") != 0) ? 0 : 1;
            if (expect_frames > 0 && produced < expect_frames)
                exit_code = 1;
        }
        if (ring_ok) {
            if (exit_code != 0) {
                char ringpath[640];
                if (snprintf(ringpath, sizeof ringpath,
                             "%s/wire_session.log", log_dir) <
                    (int)sizeof ringpath)
                    (void)kn_ring_dump(&ring, ringpath);
            }
            kn_ring_free(&ring);
        }
        if (exit_code != 0) {
            kn_log_on_failure("session.media",
                              datagrams == 0 ? "E_SESSION_NO_MEDIA"
                                             : "E_SESSION_NO_FRAMES",
                              datagrams == 0
                                  ? "PLAY succeeded but no media datagrams "
                                    "arrived (firewall? peer routing? R34)"
                                  : "media arrived but no access unit "
                                    "completed (SRTP material? vps? drops "
                                    "map tells)",
                              "the handoff line + drops map + wire mirror "
                              "in the log dir are the R7/R32 evidence");
        }
        kn_decode_stream_close(c.stream);
        c.stream = NULL;
        free(arena);
        arena = NULL;
        kn_sock_udp_close(&sock);
        goto teardown;

    teardown_sock:
        kn_sock_udp_close(&sock);
    }
teardown:
    /* best-effort TEARDOWN (the reference's Drop path: 1 s, errors
     * ignored, sequence mismatches skipped) then close everything */
    {
        kn_rtsp_header th[4];
        kn_rtsp_response tresp;
        th[0] = common[0]; th[1] = common[1]; th[2] = common[2];
        th[3].name = "Session";
        th[3].value = desc.session[0] ? desc.session : sess.session_id;
        rtc.timeout_ms = 1000;
        (void)kn_rtsp_request(&rtc, "TEARDOWN", sess.rtsps_endpoint, th, 4,
                              NULL, 0, 1, &tresp);
    }
    kn_ws_send_close(&wsc, 1000);
    kn_net_tls_stream_close(sig_tls);
    if (cm_tls) kn_net_tls_stream_close(cm_tls);
    if (exit_code != 0 && !have_sess)
        kn_log_on_failure("session.rtsp", "E_SESSION_RTSP",
                          "the RTSPS sequence did not complete",
                          "the rtsp step lines above show where it stopped; "
                          "401/403 = token, 503 = service, timeouts = rig");
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
    int capture = 0, decode = 0, session = 0;
    const char *sess_token = NULL, *sess_device = "karnynow-win7-e5500";
    const char *sess_title = "KarnyNow session", *sess_cm_host = NULL;
    const char *sess_au_dir = NULL;
    long long sess_app = 0;
    long sess_seconds = 60, sess_queue_timeout = 180, sess_expect = 0;
    long cap_port = 4798, dec_port = 4798;
    long cap_seconds = 30, dec_seconds = 30;
    long dec_vps = 0, dec_expect = 0;
    const char *cap_bind = NULL, *dec_bind = NULL;
    const char *cap_out = "gs_capture.hex", *dec_au_dir = NULL;
    const char *dec_srtp = NULL;
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
        else if (strcmp(a, "--capture") == 0) capture = 1;
        else if (strncmp(a, "--capture-port=", 15) == 0)
            cap_port = strtol(a + 15, NULL, 10);
        else if (strncmp(a, "--capture-bind=", 15) == 0) cap_bind = a + 15;
        else if (strncmp(a, "--capture-out=", 14) == 0) cap_out = a + 14;
        else if (strncmp(a, "--capture-seconds=", 18) == 0)
            cap_seconds = strtol(a + 18, NULL, 10);
        else if (strcmp(a, "--decode") == 0) decode = 1;
        else if (strncmp(a, "--decode-port=", 14) == 0)
            dec_port = strtol(a + 14, NULL, 10);
        else if (strncmp(a, "--decode-bind=", 14) == 0) dec_bind = a + 14;
        else if (strncmp(a, "--decode-seconds=", 17) == 0)
            dec_seconds = strtol(a + 17, NULL, 10);
        else if (strncmp(a, "--decode-srtp=", 14) == 0) dec_srtp = a + 14;
        else if (strncmp(a, "--decode-vps=", 13) == 0)
            dec_vps = strtol(a + 13, NULL, 10);
        else if (strncmp(a, "--decode-au-dir=", 16) == 0)
            dec_au_dir = a + 16;
        else if (strncmp(a, "--decode-expect-frames=", 23) == 0)
            dec_expect = strtol(a + 23, NULL, 10);
        else if (strcmp(a, "--session") == 0) session = 1;
        else if (strncmp(a, "--session-token=", 16) == 0)
            sess_token = a + 16;
        else if (strncmp(a, "--session-device=", 17) == 0)
            sess_device = a + 17;
        else if (strncmp(a, "--session-title=", 16) == 0)
            sess_title = a + 16;
        else if (strncmp(a, "--session-app=", 14) == 0)
            sess_app = strtoll(a + 14, NULL, 10);
        else if (strncmp(a, "--cm-host=", 10) == 0) sess_cm_host = a + 10;
        else if (strncmp(a, "--session-seconds=", 18) == 0)
            sess_seconds = strtol(a + 18, NULL, 10);
        else if (strncmp(a, "--session-queue-timeout=", 24) == 0)
            sess_queue_timeout = strtol(a + 24, NULL, 10);
        else if (strncmp(a, "--session-expect-frames=", 24) == 0)
            sess_expect = strtol(a + 24, NULL, 10);
        else if (strncmp(a, "--session-au-dir=", 17) == 0)
            sess_au_dir = a + 17;
        else if (strcmp(a, "--diagnostics") == 0) diagnostics = 1;
        else if (strcmp(a, "--version") == 0) version = 1;
        else if (strcmp(a, "--list-selftests") == 0) list = 1;
        else if (strncmp(a, "--log-dir=", 10) == 0) log_dir = a + 10;
        else { usage(); return 2; }
    }

    if (capture || decode) {
        if (cap_port < 1 || cap_port > 65535 ||
            dec_port < 1 || dec_port > 65535 ||
            cap_seconds < 0 || cap_seconds > 86400 ||
            dec_seconds < 0 || dec_seconds > 86400 ||
            dec_vps < 0 || dec_vps > 65519 ||
            dec_expect < 0 || dec_expect > 100000000) { usage(); return 2; }
    }

    if (list) return print_registry();
    if (version) return print_version();
    if (selftest && selftest[0]) return run_selftests(selftest, log_dir);
    if (replay && replay[0])
        return run_replay(replay, replay_au_dir, replay_srtp, replay_vps,
                          log_dir);
    if (loopback) return run_loopback(loopback, loopback_seconds, no_audio,
                                      log_dir);
    if (capture)
        return run_capture((uint16_t)cap_port, cap_bind, cap_out,
                           cap_seconds, log_dir);
    if (decode)
        return run_decode((uint16_t)dec_port, dec_bind, dec_seconds,
                          dec_srtp, dec_vps, dec_au_dir, dec_expect,
                          log_dir);
    if (session) {
        if (!sess_token || !sess_token[0] || sess_app <= 0 ||
            sess_seconds < 0 || sess_seconds > 86400 ||
            sess_queue_timeout < 0 || sess_queue_timeout > 86400 ||
            sess_expect < 0 || sess_expect > 100000000) {
            usage();
            return 2;
        }
        return run_session(sess_token, sess_device, sess_app, sess_title,
                           sess_cm_host && sess_cm_host[0] ? sess_cm_host
                               : KN_CM_DEFAULT_BASE_HOST,
                           sess_seconds, sess_queue_timeout, sess_expect,
                           sess_au_dir, log_dir);
    }
    if (diagnostics) return run_diagnostics(log_dir);
    usage();
    return 2;
}
