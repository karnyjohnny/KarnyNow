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
#include "kn/kn_cert.h"
#include "kn/kn_dtls.h"
#include "kn/kn_sctp.h"
#include "kn/kn_cm.h"
#include "kn/kn_d3d.h"
#include "kn/kn_decode.h"
#include "kn/kn_diag.h"
#include "kn/knfec.h"
#include "kn/kn_http.h"
#include "kn/kn_input.h"
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
#ifndef _WIN32
#include <pthread.h>   /* round XXVIII: the pre-PLAY pump thread (POSIX;
                          Win32 uses kernel32 CreateThread — the F1 set) */
#endif
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
           "         [--session-keep]  (leave the cloud session alive on abort\n"
           "                           so the GFN web UI can RESUME it)\n"
           "         [--session-play-wait=N]   (ms to wait for DTLS+SCTP up\n"
           "                                    before PLAY; default 2500)\n"
           "         [--session-play-settle=N] (extra ms after sctp_up before\n"
           "                                    PLAY; default 250)\n"
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

/* ---- round XXXVI: the feedback outbox (main -> pump thread) ----
 * The media loop (main thread, the kn_rx single owner) ENCODES every
 * feedback message; the pump thread (the SCTP single owner) SENDS it.
 * Bounded mutex queue, drop-new + counted (the kn_sock discipline);
 * 704-byte slots cover the worst encoder output (nack_v2 <= 647,
 * kn_rx.h). This is the same thread-boundary pattern as the kn_sctp
 * tx queue, one level up. */
#define SESS_FB_SLOTS   64u
/* Round XXXVIII: the slot grows from 704 (the worst single encoder
 * output, nack_v2 <= 647) to the reference's MAX_CONTROL_REPORT_BYTES
 * = 1071 (nvst.rs:107), because a control-report BATCH is now one
 * message: the reference appends frame acks / pacing / QoS records into
 * one buffer and sends it as ONE partial-control write (:521-548),
 * flushing when the next record would not fit or when a QoS report is
 * queued. Run 18 pushed 6138 individual messages and dropped 890 —
 * batching cuts the message count roughly by the records-per-batch
 * factor, which is the honest fix for a full outbox. */
#define SESS_FB_MAXLEN  1071u
/* the NVST profile's stream ids are 0,2,4,...,14 (nvst_input.rs:74-127) */
#define SESS_FB_SIDS    8u
#define SESS_FB_SID_SLOT(sid) (((sid) <= 14u) ? ((sid) / 2u) : 0u)
typedef struct {
    uint8_t  payload[SESS_FB_MAXLEN];
    uint16_t len;
    uint16_t sid;
} sess_fb_msg;
typedef struct sess_fb_outbox {
    sess_fb_msg q[SESS_FB_SLOTS];
    unsigned head, count;
    unsigned dropped, pushed;
    /* per-stream pushes/drops: run 18's `dropped:890` could not say
     * WHICH channel overflowed, so it could not distinguish "the pump
     * thread stalled" from "one cadence outran the association". */
    unsigned long long pushed_per_sid[SESS_FB_SIDS];
    unsigned long long dropped_per_sid[SESS_FB_SIDS];
    unsigned long long refused;      /* bad args / oversized: never queued */
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    int inited;
} sess_fb_outbox;

static void sess_fb_init(sess_fb_outbox *b)
{
    memset(b, 0, sizeof *b);
#ifdef _WIN32
    InitializeCriticalSection(&b->lock);
#else
    pthread_mutex_init(&b->lock, NULL);
#endif
    b->inited = 1;
}

static void sess_fb_destroy(sess_fb_outbox *b)
{
    if (!b->inited) return;
#ifdef _WIN32
    DeleteCriticalSection(&b->lock);
#else
    pthread_mutex_destroy(&b->lock);
#endif
    b->inited = 0;
}

static int sess_fb_push(sess_fb_outbox *b, uint16_t sid,
                        const uint8_t *data, size_t len)
{
    int rc = 0;
    if (!b || !b->inited || !data || len == 0 || len > SESS_FB_MAXLEN) {
        if (b && b->inited) b->refused++;
        return -1;
    }
#ifdef _WIN32
    EnterCriticalSection(&b->lock);
#else
    pthread_mutex_lock(&b->lock);
#endif
    if (b->count >= SESS_FB_SLOTS) {
        b->dropped++;
        b->dropped_per_sid[SESS_FB_SID_SLOT(sid)]++;
        rc = -2;
    } else {
        unsigned slot = (b->head + b->count) % SESS_FB_SLOTS;
        memcpy(b->q[slot].payload, data, len);
        b->q[slot].len = (uint16_t)len;
        b->q[slot].sid = sid;
        b->count++;
        b->pushed++;
        b->pushed_per_sid[SESS_FB_SID_SLOT(sid)]++;
    }
#ifdef _WIN32
    LeaveCriticalSection(&b->lock);
#else
    pthread_mutex_unlock(&b->lock);
#endif
    return rc;
}

static int sess_fb_pop(sess_fb_outbox *b, sess_fb_msg *out)
{
    int got = 0;
    if (!b || !b->inited) return 0;
#ifdef _WIN32
    EnterCriticalSection(&b->lock);
#else
    pthread_mutex_lock(&b->lock);
#endif
    if (b->count) {
        *out = b->q[b->head];
        b->head = (b->head + 1u) % SESS_FB_SLOTS;
        b->count--;
        got = 1;
    }
#ifdef _WIN32
    LeaveCriticalSection(&b->lock);
#else
    pthread_mutex_unlock(&b->lock);
#endif
    return got;
}

/* Round XXXVIII (D-t10): the bandwidth estimator's clock. kn_rx takes
 * `now_ms` everywhere (deterministic replay), but BandwidthEstimator does
 * its arithmetic on integer-nanosecond Durations (nvst_bandwidth.rs:60,
 * :96-108, :212) and a frame's packets usually land INSIDE one
 * millisecond — a ms-granular clock would read that train as 0 ms, hit the
 * reference's 0.5 ms floor and double the receive rate it reports.
 * kn_sock_now_us() is monotonic (QueryPerformanceCounter on Win7,
 * CLOCK_MONOTONIC elsewhere) and only differences are used. */
static int64_t sess_bw_now_ns(void *ud)
{
    (void)ud;
    return (int64_t)(kn_sock_now_us() * 1000u);
}

/* the reference's RTCP sender ssrc: 0x4F4E4F57 = "ONOW" (nvst.rs:96) */
#define SESS_RTCP_SENDER_SSRC 0x4F4E4F57u
/* SRTCP_RR_INTERVAL 1 s (:97), QOS_REPORT_INTERVAL 50 ms (nvst_control.rs
 * :19), FRAME_PACING_INTERVAL 55556 us (:18), CONTROL_KEEPALIVE_INTERVAL
 * 3 s (nvst_input.rs:11), DEFAULT_FRAME_TIME_US 16666 (:17). The
 * keepalive bytes are the reference test's golden (:1769:
 * control_keepalive(0) == 0002040000000000). */
#define SESS_RR_INTERVAL_MS      1000u
#define SESS_QOS_INTERVAL_MS     50u
#define SESS_PACING_INTERVAL_US  55556ull
#define SESS_KA_INTERVAL_MS      3000u
#define SESS_FRAME_TIME_US       16666u

/* ---- round XXXVIII: ControlReportBatch (nvst.rs:464-548) ----
 * The reference does NOT send one SCTP message per feedback record. It
 * appends frame-ack / pacing / QoS records into one buffer bounded by
 * MAX_CONTROL_REPORT_BYTES (1071), and flushes that buffer as a SINGLE
 * partial-control write when (a) the next record would not fit, or
 * (b) flush_due() — a QoS report is queued, or the oldest queued record
 * has waited QOS_REPORT_INTERVAL (50 ms). A failed send RETAINS the
 * buffer (:529-531), so nothing is silently lost.
 *
 * Run 18 pushed 6138 separate messages through a 64-slot outbox and
 * dropped 890 of them; the reference's shape produces roughly one
 * message per 50 ms instead of one per record, which is both faithful
 * and the actual fix for that drop count. Counters mirror the
 * reference's frame_acks_sent / qos_reports_sent / last_ack_frame. */
#define SESS_CTRL_BATCH_MAX   1071u   /* MAX_CONTROL_REPORT_BYTES */
#define SESS_CTRL_BATCH_MIN   (4u + KN_RX_CTRL_FRAME_ACK_PAYLOAD_LEN)
#define SESS_FRAME_ACK_RECORD_LEN (4u + KN_RX_CTRL_FRAME_ACK_PAYLOAD_LEN)

typedef struct {
    uint8_t  bytes[SESS_CTRL_BATCH_MAX];
    size_t   len;
    uint64_t queued_at_ms;      int have_queued_at;
    int      have_qos;
    unsigned pending_acks;      /* acks in the buffer, not yet flushed */
    unsigned acks, qos_reports; /* flushed totals (the reference's) */
    uint32_t last_ack_frame;    int have_last_ack_frame;
    /* evidence: why each flush happened, and what never made it */
    unsigned long long flushes, flush_capacity, flush_qos, flush_age,
                       flush_failed, records, retained;
} sess_ctrl_batch;

static void sess_cb_init(sess_ctrl_batch *b) { memset(b, 0, sizeof *b); }

static int sess_cb_fits(const sess_ctrl_batch *b, size_t record_len)
{
    return b->len + record_len <= SESS_CTRL_BATCH_MAX;
}

/* One flush = one outbox push of the whole buffer (the reference's
 * `send(&self.bytes)`). Returns 1 when the buffer is now empty. On
 * failure the buffer is RETAINED and retried on the next flush. */
static int sess_cb_flush(sess_ctrl_batch *b, sess_fb_outbox *fb,
                         const char *why)
{
    if (b->len == 0u) return 1;
    if (!fb || sess_fb_push(fb, 6, b->bytes, b->len) != 0) {
        b->flush_failed++;
        b->retained++;
        return 0;
    }
    b->flushes++;
    if (why && why[0] == 'c') b->flush_capacity++;
    else if (why && why[0] == 'q') b->flush_qos++;
    else if (why && why[0] == 'a') b->flush_age++;
    b->acks += b->pending_acks;
    b->qos_reports += b->have_qos ? 1u : 0u;
    b->len = 0u;
    b->have_queued_at = 0;
    b->have_qos = 0;
    b->pending_acks = 0;
    return 1;
}

static int sess_cb_append(sess_ctrl_batch *b, const uint8_t *record,
                          size_t len, uint64_t now_ms)
{
    if (!b || !record || len == 0u || len > SESS_CTRL_BATCH_MAX) return 0;
    if (!b->have_queued_at) {          /* queued_at.get_or_insert(now) */
        b->queued_at_ms = now_ms;
        b->have_queued_at = 1;
    }
    memcpy(b->bytes + b->len, record, len);
    b->len += len;
    b->records++;
    return 1;
}

/* flush_due (nvst.rs:509-515): a QoS report is queued, or the buffer has
 * been sitting for QOS_REPORT_INTERVAL. */
static int sess_cb_flush_due(const sess_ctrl_batch *b, uint64_t now_ms)
{
    if (b->len == 0u) return 0;
    if (b->have_qos) return 1;
    return b->have_queued_at &&
           now_ms >= b->queued_at_ms + SESS_QOS_INTERVAL_MS;
}

/* The NVST channel profile's SENDER-side reliability (nvst_input.rs
 * NVST_CHANNEL_PROFILE + str0m configure_reliability): ordered flags
 * and the PR-SCTP policy per stream id. */
static void sess_fb_sid_params(uint16_t sid, int *ordered,
                               uint16_t *pr_pol, uint32_t *pr_val)
{
    switch (sid) {
    case 4:  *ordered = 1; *pr_pol = KN_SCTP_PR_RTX; *pr_val = 2;   break;
    case 6:  *ordered = 0; *pr_pol = KN_SCTP_PR_TTL; *pr_val = 300; break;
    case 8:  *ordered = 0; *pr_pol = KN_SCTP_PR_RTX; *pr_val = 0;   break;
    case 10: *ordered = 0; *pr_pol = KN_SCTP_PR_TTL; *pr_val = 300; break;
    default: *ordered = 1; *pr_pol = 0;               *pr_val = 0;  break;
    }
}


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
    /* round XXXVI: the feedback sender state (session mode only —
     * --decode leaves fb NULL and stays silent, honestly) */
    sess_fb_outbox *fb;
    /* round XXXVIII: the server's frame-pacing latch (0x112 for stream 0,
     * nvst.rs:714-721). Default ENABLED — the reference's
     * AtomicBool::new(true) (:664). Main refreshes it from the pump's
     * volatile mirror once per media-loop pass. */
    int pacing_enabled;
    unsigned long long fb_pacing_disabled;
    uint64_t next_rr_ms, next_qos_ms, next_ka_ms, next_pacing_us;
    uint32_t rr_prior_expected, rr_prior_received;
    uint32_t qos_seq, last_fi;
    unsigned long long fb_bytes_total;
    uint64_t qos_prior_ms;
    unsigned long long qos_prior_bytes;
    /* round XXXVIII (D-t10): the 0x0207 report's own state, per
     * qos_report_at (nvst.rs:1140-1177). `completed_frame_bytes` is
     * CUMULATIVE completed-frame bytes (publish_completed_frame,
     * :987-993) and `last_sender_frame` is the last completed frame
     * INDEX (a store, not a sum). The loss field is a DELTA between two
     * QosPacketSnapshots, so the previous one is kept here (:1147-1166)
     * — run 18 sent a cumulative ratio instead, which is a different
     * quantity and never relaxes either. */
    unsigned long long completed_frame_bytes;
    uint32_t last_sender_frame;
    uint64_t transport_origin_ms;
    int      have_qos_prior;
    uint32_t qos_prior_ssrc, qos_prior_base, qos_prior_highest,
             qos_prior_received;
    unsigned long long fb_rr, fb_ack, fb_pacing, fb_qos, fb_ka,
                       fb_nack, fb_idr, fb_pli, fb_rfi;
    /* round XXXVIII: the ControlReportBatch (nvst.rs:464-548) that the
     * frame-ack / pacing / QoS records ride in. */
    sess_ctrl_batch cbatch;
    /* round XXXVIII: AU wall-clock vs RTP-timestamp telemetry. Run 18's
     * diagnosis rested on AU BYTE SIZE because the log had no way to say
     * whether the sender's 90 kHz clock and our wall clock still agreed.
     * drift_ms = (wall elapsed) - (rtp elapsed) over the whole session:
     * a rate controller slowing the encoder shows up as the RTP clock
     * falling behind the wall, and a paused encoder clock shows up as a
     * jump. Both are invisible in byte counts. */
    int      have_au_first;
    uint64_t au_first_wall_ms;
    uint32_t au_first_ts;
    uint32_t au_last_ts;
    long long au_drift_ms, au_drift_min_ms, au_drift_max_ms;
    unsigned long long au_ts_backwards;
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
    /* ---- round XXXVI: per-assembled-frame feedback (the reference's
     * control_reports path — the rig holds a pending-frame-ack window,
     * MAX_PENDING_FRAME_ACKS 512, nvst.rs:106). Frame ACK 0x0204 on
     * EVERY assembled frame (sid 6, send_partial_control); the pacing
     * report 0x0203 at the reference's 55556-us gate with a NEUTRAL
     * error (:7928-7933 — feeding network jitter into the server PID
     * is explicitly wrong per the reference comment). Both ride the
     * outbox; overflow drops are counted, never queued unbounded. */
    c->last_fi = f->frame_index;
    c->fb_bytes_total += (unsigned long long)f->bytes_len;
    /* publish_completed_frame (nvst.rs:987-993): the QoS report's
     * sender_frame_number is the LAST completed frame index and its
     * bytes_received is the CUMULATIVE completed-frame byte count. */
    c->last_sender_frame = f->frame_index;
    c->completed_frame_bytes += (unsigned long long)f->bytes_len;

    /* ---- AU wall-vs-RTP telemetry (round XXXVIII evidence) ----
     * The RTP timestamp is the SENDER's 90 kHz clock; c->now_ms is our
     * monotonic wall. Their elapsed difference is the encoder's cadence
     * as we see it — the quantity run 18's log could not show. */
    if (!c->have_au_first) {
        c->have_au_first = 1;
        c->au_first_wall_ms = c->now_ms;
        c->au_first_ts = f->timestamp;
        c->au_drift_min_ms = 0;
        c->au_drift_max_ms = 0;
    } else {
        long long wall = (long long)(c->now_ms - c->au_first_wall_ms);
        long long rtp = (long long)((uint32_t)(f->timestamp -
                                               c->au_first_ts)) / 90;
        if ((int32_t)(f->timestamp - c->au_last_ts) < 0)
            c->au_ts_backwards++;
        c->au_drift_ms = wall - rtp;
        if (c->au_drift_ms < c->au_drift_min_ms)
            c->au_drift_min_ms = c->au_drift_ms;
        if (c->au_drift_ms > c->au_drift_max_ms)
            c->au_drift_max_ms = c->au_drift_ms;
    }
    c->au_last_ts = f->timestamp;

    if (c->fb) {
        uint8_t fb[4 + KN_RX_CTRL_FRAME_ACK_PAYLOAD_LEN];
        int n = kn_rx_ctrl_frame_ack(f->frame_index, NULL,
                                     (uint32_t)f->bytes_len, fb);
        /* nvst.rs:7905-7944: the ack goes into the batch, and the batch
         * is flushed when the NEXT record would not fit. The reference
         * drains its pending-frame-ack deque in the same loop, so a full
         * batch never delays an ack by more than one flush. */
        if (n > 0) {
            if (!sess_cb_fits(&c->cbatch, (size_t)n))
                (void)sess_cb_flush(&c->cbatch, c->fb, "capacity");
            if (sess_cb_append(&c->cbatch, fb, (size_t)n, c->now_ms)) {
                c->cbatch.pending_acks++;
                c->cbatch.last_ack_frame = f->frame_index;
                c->cbatch.have_last_ack_frame = 1;
                c->fb_ack++;
            }
        }
        /* frame_pacing_feedback && frame_pacing_enabled && the interval
         * gate (nvst.rs:7922-7936). The middle term is the SERVER's: a
         * 0x112 for stream 0 with mode != 1 switches our 0x0203 reports
         * OFF, and sending them anyway feeds a PID the rig has explicitly
         * disabled. The pacing error stays NEUTRAL (:7928-7933 — feeding
         * network jitter into the server PID made its encoder cadence
         * oscillate). */
        if (c->pacing_enabled) {
            uint64_t now_us = kn_sock_now_us();
            if (now_us >= c->next_pacing_us) {
                uint8_t pb[4 + KN_RX_CTRL_FRAME_PACING_PAYLOAD_LEN];
                int pn = kn_rx_ctrl_frame_pacing(f->frame_index,
                                                 SESS_FRAME_TIME_US, 0, pb);
                if (pn > 0) {
                    if (!sess_cb_fits(&c->cbatch, (size_t)pn))
                        (void)sess_cb_flush(&c->cbatch, c->fb, "capacity");
                    if (sess_cb_append(&c->cbatch, pb, (size_t)pn,
                                       c->now_ms))
                        c->fb_pacing++;
                }
                c->next_pacing_us = now_us + SESS_PACING_INTERVAL_US;
            }
        } else {
            c->fb_pacing_disabled++;
        }
    }
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
        int sent = 0;
        c->kf_request_lines++;
        /* round XXXVI: the request now FLIES — IDR (0x0302) on the
         * partial-reliable control channel (sid 6, the nvst_control
         * command space) + a plain RTCP PLI on the rtcp1 channel
         * (sid 14, nvst.rs:8101 build_rtcp_pli via send_rtcp). The
         * stage-2 "unsent" note was the honest placeholder; run 16's
         * stall (progress_stall -> kf_request sent:false) is exactly
         * the gap this closes. */
        if (c->fb) {
            uint8_t idr[6];
            int n = kn_rx_ctrl_idr_request(idr);
            if (n > 0 && sess_fb_push(c->fb, 6, idr, (size_t)n) == 0) {
                c->fb_idr++;
                sent = 1;
            }
            if (c->v->have_bound_ssrc) {
                uint8_t pli[12];
                kn_rx_rtcp_pli(SESS_RTCP_SENDER_SSRC, c->v->bound_ssrc,
                               pli);
                if (sess_fb_push(c->fb, 14, pli, sizeof pli) == 0) {
                    c->fb_pli++;
                    sent = 1;
                }
            }
        }
        if (c->kf_request_lines <= 20) {
            printf("{\"mode\":\"decode\",\"event\":\"kf_request\","
                   "\"sent\":%d,\"n\":%llu}\n", sent,
                   (unsigned long long)(c->kf_request_lines % 1000000ull));
            fflush(stdout);
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
    /* The reference feeds BOTH text and binary frames into the RTSP
     * buffer (nvst_rtsp.rs:620-623 — Message::Binary is appended
     * lossy-UTF8); control frames (ping/pong/close) never surface here
     * (kn_ws handles them internally), so anything else is an error. */
    if ((op != KN_WS_OP_TEXT && op != KN_WS_OP_BINARY) || n + 1 > cap)
        return KN_RTSP_ERR_IO;
    memcpy(buf, w->msg, n);
    buf[n] = '\0';
    *out = n;
    return KN_RTSP_OK;
}

static uint64_t sess_now(void *c) { (void)c; return kn_sock_now_ms(); }

/* Round XXIV: the failure companion for the rtsp step events — kn_rtsp's
 * human err[160] plus the WS-layer verdict (last_error, the peer's close
 * code, and the text/binary frame counters), so a failed step is wire
 * EVIDENCE instead of a bare rc (R7 run-5's DESCRIBE -2/0 taught that). */
static void sess_rtsp_detail(const char *step, const kn_rtsp_client *rtc,
                             const kn_ws_client *wsc,
                             const kn_rtsp_response *resp)
{
    char esc[192];
    lb_json_path(esc, sizeof esc, rtc->err);
    printf("{\"mode\":\"session\",\"event\":\"rtsp_detail\",\"step\":\"%s\","
           "\"detail\":\"%s\",\"ws_err\":%d,\"ws_close\":%d,"
           "\"ws_text\":%llu,\"ws_bin\":%llu,\"ws_pings_in\":%llu,"
           "\"ws_pongs_in\":%llu,\"ws_pongs_out\":%llu,"
           "\"ws_closes_in\":%llu,\"ws_frames_out\":%llu}\n", step, esc,
           wsc->last_error, wsc->last_close_code, wsc->frames_in_text,
           wsc->frames_in_binary, wsc->pings_in, wsc->pongs_in,
           wsc->pongs_out, wsc->closes_in, wsc->frames_out);
    fflush(stdout);
    /* Round XXXII: when the rig DID answer with a non-200 (run 13's
     * PLAY 400 — an explicit refusal, unlike runs 9-12's silence), the
     * response itself is the evidence: reason phrase, EVERY header, and
     * a bounded body excerpt (sanitized per the round-XVIII rule). The
     * rig's own words name the missing piece — never guess when you can
     * read. */
    if (resp && resp->status != 200) {
        char nesc[64], vesc[192], resc[64], besc[320];
        int h;
        printf("{\"mode\":\"session\",\"event\":\"rtsp_response\","
               "\"step\":\"%s\",\"status\":%d,", step, resp->status);
        lb_json_path(resc, sizeof resc, resp->status_text);
        printf("\"reason\":\"%s\",\"headers\":[", resc);
        for (h = 0; h < resp->hdr_count && h < KN_RTSP_MAX_HEADERS; ++h) {
            lb_json_path(nesc, sizeof nesc, resp->names[h]);
            lb_json_path(vesc, sizeof vesc, resp->values[h]);
            printf("%s{\"n\":\"%s\",\"v\":\"%s\"}", h ? "," : "",
                   nesc, vesc);
        }
        printf("],");
        {
            size_t bl = resp->body_len < 240 ? resp->body_len : 240;
            char btmp[256];
            memcpy(btmp, resp->body ? resp->body : "", bl);
            btmp[bl] = '\0';
            lb_json_path(besc, sizeof besc, btmp);
            printf("\"body_len\":%lu,\"body\":\"%s\"}\n",
                   (unsigned long)(resp->body_len % 1000000ul), besc);
        }
        fflush(stdout);
    }
}

/* ---- round XXV: the pre-PLAY ICE/NATT pump (deviation D-s2) ----
 * R7 run-6 measurement: OPTIONS/DESCRIBE/SETUP/ANNOUNCE all 200 on the
 * real rig, PLAY rejected with 455 "Method Not Valid in This State" —
 * with ZERO UDP sent before it. The reference spawns its bundle receiver
 * thread BETWEEN announce() and finish()/PLAY (lib.rs:894→937→1117) and
 * that thread's first loop iteration fires immediately (nvst.rs:7438
 * pre-arms last_hole_punch): 3× STUN binding request + 1× NATT hole-
 * punch ping per 20 ms tick (PING_INTERVAL_BEFORE_CONNECTION :242, the
 * burst :7842-7900) at bundle_peer() = the CloudMatch bundle peer else
 * the SETUP video peer (:1880), while answering the rig's binding
 * requests (:5151-5172). This pump is the SEQUENTIAL form of that burst
 * (bounded budget, called before PLAY and re-run between 455 retries).
 * Full ICE/DTLS stays the stage-3 R3 spike; this is exactly the hole-
 * punch traffic that gates the rig's PLAY state machine. */
typedef struct {
    unsigned stun_sent, natt_sent, req_in, answered, ok_in, invalid_in,
             send_errors;
    unsigned pongs_sent;                /* the literal PING->PONG leg
                                           (nvst.rs:8605-8614) */
    /* round XXXI (R3-B): the DTLS handshake state, mirrored from
     * kn_dtls by the pump thread (the snapshot struct carries them to
     * the main thread while the worker runs) */
    unsigned dtls_armed, dtls_started, dtls_ready, dtls_fail_rc;
    unsigned dtls_in, dtls_out;
    int      dtls_last_rc, dtls_verify;
    uint64_t dtls_ready_ms;
    /* round XXXII: the rig's post-handshake app data (run 13: exactly
     * one extra inbound record after dtls_ready — its head, captured
     * verbatim by kn_dtls, is R3-C's design input) */
    unsigned dtls_app_records, dtls_app_bytes, dtls_first_app_len;
    uint8_t  dtls_first_app[64];
    unsigned vid_ka_sent;
    /* round XXXIII (R3-C): the SCTP association over the DTLS app
     * layer (run 14: the rig sends NO app data itself and answers PLAY
     * 400 without our INIT — start_sctp(true) is the client's job,
     * nvst.rs:8221) */
    unsigned sctp_armed, sctp_up, sctp_fail;
    unsigned sctp_opened, sctp_acked;      /* our OPENs sent / ACKed */
    unsigned sctp_rig_opened, sctp_rig_acked; /* rig's OPENs / our ACKs */
    unsigned sctp_msgs_in;                 /* non-DCEP rig messages */
    /* round XXXVIII: why the association went down (see sess_sctp_event) */
    unsigned sctp_down_state, sctp_down_error;
    int      sctp_down_errno;
    unsigned sctp_first_sid, sctp_first_ppi, sctp_first_len;
    uint8_t  sctp_first[16];               /* the first rig message head */
    uint64_t sctp_up_ms;
    /* round XXXIV (run 15's evidence gaps): the nomination counter,
     * the kn_dtls post-ready RAW record capture (the mystery record —
     * captured since XXXIII but never printed!), peer_closed, and the
     * kn_sctp packet counters (proof the INIT actually left) */
    unsigned nom_sent;
    unsigned dtls_peer_closed, dtls_post_first_len;
    uint8_t  dtls_post_first[32];
    unsigned sctp_out, sctp_in;
    /* round XXXVI: bundle-socket media classification (run 16's
     * invalid_in:175 was almost certainly the rig's AUDIO RTP on the
     * bundle — rtcAudioOnNativeBundle:1; counted separately now,
     * consumed at the audio stage) + the feedback-outbox send counter */
    unsigned bundle_rtp_in, bundle_rtcp_in, fb_sent;
    /* round XXXVII: the control/input plane (docs/INPUT.md). Run 17
     * disproved the round-XXXVI hypothesis — the whole feedback set flew
     * for 242 s (rr 242 / qos 4701 / ka 81 / ack 331 / dropped 0) and
     * video still died at exactly 10.99 s, the same shape as run 16's
     * 10.2 s with NO feedback. What both runs lacked is the input
     * activation chain (nvst.rs:6554-6591). These counters say whether
     * it flew, and what the rig said back — run 17 left its 20 control
     * messages uninterpreted, which must never happen again. */
    int      input_version;        /* the rig's negotiated 0x020e, -1 = none */
    unsigned input_ready;          /* control open + version + chain sent */
    unsigned ctrl_msgs;            /* inbound non-DCEP messages */
    unsigned ctrl_records;         /* TLV records walked inside them */
    unsigned activation_sent, activation_fail, activation_attempts;
    unsigned cursor_state;         /* kn_input_cursor_state */
    unsigned cursor_enables, cursor_trackings, cursor_disables;
    unsigned cursor_handoffs;
    unsigned ctrl_hist_overflow;
    char     ctrl_hist[768];       /* [["020e",1],...] — the code histogram */
    /* round XXXVIII: the SERVER-CONTROL plane (nvst_server_control.rs,
     * decoded by kn_sctl_walk). Run 18 named the rig's 23 control
     * messages but decoded none of them, so an idle warning, a
     * termination or a frame-pacing mode change would have been
     * invisible. These are the reference's own observable outputs:
     * the three state latches plus its ServerControlDiagnostics
     * counters (:148-153), which its 10-second nvst-control line
     * prints verbatim (:7985-7997). */
    unsigned sctl_msgs;            /* messages walked (sid 0 + sid 6) */
    unsigned sctl_records;         /* records kn_sctl_walk produced */
    unsigned sctl_timer, sctl_termination, sctl_pacing;
    unsigned sctl_unhandled, sctl_unknown, sctl_malformed;
    unsigned sctl_invalid_stream;
    unsigned sctl_last_unknown;    /* the reference's last_unknown */
    unsigned idle_warning;         /* 0x104 active / 0x105 clears */
    unsigned idle_warnings, idle_expiries;
    unsigned pacing_enabled;       /* KN_SCTL_PACING_DEFAULT_ENABLED = 1 */
    unsigned server_termination;   /* 0x109 seen: the session is over */
    unsigned term_wire_code, term_reason, term_extended;
    int      term_resumable;       /* -1 = undecidable */
    uint64_t pump_ms;
} sess_ice_stats;

/* Round XXVIII — the pump context + its optional worker thread. Run 9
 * measured that the SEQUENTIAL pump (400 ms of bursts, then silence
 * during the PLAY exchange) still drew PLAY 455 despite a fully
 * bidirectional ICE exchange (stun_sent 147, req_in 99, answered 99,
 * ok_in 327, invalid_in 0), and that a re-PLAY on the 455'd WS hit a
 * DEAD channel (rc -2, ws_close -1, no close frame — the rig drops the
 * RTSP context after the refusal). The reference never goes quiet: its
 * bundle receiver thread bursts every 20 ms THROUGH announce → PLAY →
 * response and for the whole session (lib.rs:894→937→1117; nvst.rs
 * :7438/:7842-7900). The pump therefore runs on a thread spanning
 * exactly the PLAY request/response window, joined before the socket
 * returns to the media loop. Single-owner discipline holds: while the
 * thread runs, the main thread touches only the WS/TLS stream; the
 * stats are read after the join. D21 is untouched — the MEDIA loop
 * stays single-threaded (the thread is gone before the first media
 * tick). Thread APIs: kernel32 CreateThread/WaitForSingleObject/
 * CloseHandle (already in the F1 import set) / POSIX pthread (the
 * native build already links -pthread). */
/* round XXXVII: one inbound control message, captured verbatim (the
 * run-17 evidence gap: the rig sent 20 messages and we could only name
 * the first). Head-only capture, bounded, no allocation. */
#define SESS_CTRL_RAW_HEAD  48u   /* captured bytes per inbound message */
#define SESS_CTRL_RAW_MAX    8u   /* how many are dumped verbatim */
typedef struct {
    unsigned sid, ppi, len, head_len;
    uint8_t  head[SESS_CTRL_RAW_HEAD];
} sess_ctrl_raw;

typedef struct {
    kn_sock_udp *sock;
    uint32_t dest_ip;
    uint16_t dest_port;
    char remote_ufrag[288], remote_pwd[128];
    char local_ufrag[32], local_pwd[48], natt[288];
    uint8_t rx[2048];
    sess_ice_stats st;
    /* R3-B: the armed DTLS client (main creates it BEFORE the thread
     * starts; the thread is the only poller/injector while it runs;
     * main frees it AFTER the join — single-owner discipline). NULL =
     * not armed (honest STUN/NATT-only pumping, like runs 9-12). */
    struct kn_dtls *dtls;
    /* R3-C: the SCTP association (created by the thread the moment
     * DTLS goes ready; closed by main after the join — same single-
     * owner contract as dtls) */
    struct kn_sctp *sctp;
    int sctp_tried;
    /* round XXXVI: the feedback outbox (main pushes, this thread drains
     * into kn_sctp — the media loop's encoded feedback flies here) */
    struct sess_fb_outbox *fb;
    /* Round XXXII: the pre-PLAY Mjolnir NATT keepalive — the reference
     * arms BOTH workers before finish()/PLAY (lib.rs:937/:990): the
     * bundle worker (STUN+DTLS, above) AND the raw video worker whose
     * keepalive pings from port N are "WHAT ROUTES VIDEO HERE"
     * (:8849-8905). Run 13 left the announced video port silent before
     * PLAY — this closes that gap. SEND-ONLY on the video socket (the
     * thread never recvs there — inbound media stays queued in the 8
     * MiB socket buffer for the media loop); main raises vid_handoff
     * right before the media loop takes the socket over (a brief
     * duplicate-ping overlap is harmless by design). */
    kn_sock_udp *vid_sock;
    uint32_t vid_ip;
    uint16_t vid_port;
    char vid_payload[64];
    volatile int vid_handoff;
    unsigned vid_ka_sent;
    /* the stats snapshot channel: while the thread runs, main cannot
     * read st directly (torn 64-bit reads on i686, half-updated
     * counters) — it raises snap_req, the thread copies st into snap
     * at its next loop pass (<= ~25 ms) and raises snap_done. */
    /* round XXXVII: the control/input plane. It lives HERE, in the pump
     * thread — that thread owns the SCTP socket and the mbedTLS context
     * (single-owner discipline), and the reference runs the same logic in
     * its bundle worker (nvst.rs:7515-7531 / :7756-7766). Main only ever
     * reads the snapshot. */
    kn_input_cursor cursor;
    kn_input_hist   ctrl_hist;
    /* round XXXVIII: the server-control decoder state. It lives in the
     * pump thread because that thread owns the SCTP socket; the latches
     * main needs (frame pacing, termination) are mirrored volatile. */
    kn_sctl_diag sctl_diag;
    unsigned sctl_msgs, sctl_records;
    unsigned sctl_timer, sctl_terms, sctl_pacing;
    unsigned sctl_unhandled, sctl_idle_warnings, sctl_idle_expiries;
    volatile unsigned sctl_idle_warning;
    volatile unsigned sctl_pacing_enabled;
    volatile unsigned sctl_termination;
    unsigned sctl_term_wire, sctl_term_reason, sctl_term_extended;
    int      sctl_term_resumable;
    int      input_version;          /* -1 until the rig's 0x020e lands */
    unsigned activation_sent;        /* latch: the chain flies once */
    unsigned activation_attempts;
    uint64_t origin_us;              /* transport origin (chain timestamps) */
    uint64_t next_ctrl_log_ms;
    unsigned ctrl_raw_n;
    sess_ctrl_raw ctrl_raw[SESS_CTRL_RAW_MAX];
    volatile int snap_req, snap_done;
    sess_ice_stats snap;
    volatile int stop;
    int started;
#ifdef _WIN32
    uintptr_t th;                        /* HANDLE */
#else
    pthread_t th;
#endif
} sess_pump;

/* bounded string fill — snprintf("%s") into a parameter-sized dst is
 * what -Wformat-truncation cannot prove (the kn_cm.c precedent) */
static void pump_set_str(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- round XXXIII (R3-C): the SCTP layer inside the pump thread ----
 * The reference's bundle worker, the instant DTLS connects:
 *   Event::Connected -> dtls_ready = true -> start_sctp(true) ->
 *   NvstInputChannels::create (nvst.rs:8214-8233) — the CLIENT
 *   initiates the association and opens the 8-channel NVST profile.
 * Run 14 measured the rig waiting for exactly this: it sent no app
 * data itself (app_records:0) and answered PLAY with an explicit 400.
 * Wire path: usrsctp AF_CONN -> kn_sctp tx queue (thread boundary) ->
 * kn_dtls_write_app -> the bundle socket; inbound: the pump's DTLS
 * demux -> kn_dtls app ring -> kn_sctp_inject. */
static int sess_sctp_write(void *ctx, const uint8_t *data, size_t len)
{
    sess_pump *p = (sess_pump *)ctx;
    if (!p->dtls) return -1;
    return kn_dtls_write_app(p->dtls, data, len) == KN_DTLS_OK ? 0 : -1;
}

/* ---- round XXXVII: the control/input plane (docs/INPUT.md) ---- */

/* The reference's control_is_open() (nvst_input.rs:357-359) is "the
 * reliable control channel exists". Our equivalent: the association is
 * up and every DCEP OPEN we sent was ACKed (run 17: opened 8 acked 8). */
static int sess_ctrl_open(sess_pump *p)
{
    return p->st.sctp_opened > 0u && p->st.sctp_acked >= p->st.sctp_opened;
}

/* sid 0 = CONTROL_RELIABLE_LABEL: ordered, reliable, no PR-SCTP policy
 * (nvst_input.rs:72-78). Every activation/cursor message rides here. */
static int sess_ctrl_send(sess_pump *p, const uint8_t *msg, size_t len)
{
    if (!p->sctp || !kn_sctp_established(p->sctp)) return 0;
    return kn_sctp_send(p->sctp, 0, KN_SCTP_PPI_BINARY, 1, msg, len)
               == KN_SCTP_OK;
}

/* the cursor machine's sink (nvst_cursor.rs emits Capture(bool)/Tracking) */
static int sess_cursor_send(void *ctx, kn_input_cursor_cmd cmd)
{
    sess_pump *p = (sess_pump *)ctx;
    uint8_t buf[KN_INPUT_CMD_MAX];
    int n = -1;
    if (cmd == KN_INPUT_CURSOR_CMD_CAPTURE_ON)
        n = kn_input_cursor_capture(1, buf, sizeof buf);
    else if (cmd == KN_INPUT_CURSOR_CMD_CAPTURE_OFF)
        n = kn_input_cursor_capture(0, buf, sizeof buf);
    else if (cmd == KN_INPUT_CURSOR_CMD_TRACKING)
        n = kn_input_cursor_tracking(1, buf, sizeof buf);
    if (n <= 0) return 0;
    return sess_ctrl_send(p, buf, (size_t)n);
}

/* the TLV histogram sink: every record the rig sends gets counted */
static void sess_ctrl_hist_cb(void *ctx, uint16_t code,
                              const uint8_t *payload, size_t plen)
{
    sess_pump *p = (sess_pump *)ctx;
    (void)payload; (void)plen;
    kn_input_hist_add(&p->ctrl_hist, code);
}

/* a compact "020e:input-version,0200:keepalive" rendering for evidence */
typedef struct { char *buf; size_t cap, used; } sess_ctrl_names;

static void sess_ctrl_names_cb(void *ctx, uint16_t code,
                               const uint8_t *payload, size_t plen)
{
    sess_ctrl_names *n = (sess_ctrl_names *)ctx;
    const char *nm = kn_input_command_name(code);
    char tmp[48];
    int k;
    (void)payload; (void)plen;
    if (nm)
        k = snprintf(tmp, sizeof tmp, "%s%04x:%s", n->used ? "," : "",
                     (unsigned)code, nm);
    else
        k = snprintf(tmp, sizeof tmp, "%s%04x", n->used ? "," : "",
                     (unsigned)code);
    if (k <= 0 || (size_t)k >= sizeof tmp) return;
    if (n->used + (size_t)k + 1u > n->cap) return;
    memcpy(n->buf + n->used, tmp, (size_t)k);
    n->used += (size_t)k;
    n->buf[n->used] = '\0';
}

static void sess_ctrl_refresh_hist(sess_pump *p)
{
    if (kn_input_hist_json(&p->ctrl_hist, p->st.ctrl_hist,
                           sizeof p->st.ctrl_hist) < 0)
        pump_set_str(p->st.ctrl_hist, sizeof p->st.ctrl_hist,
                     "[\"hist-too-large\"]");
    p->st.ctrl_hist_overflow = p->ctrl_hist.overflow;
}

/* ---- round XXXVIII: the server-control plane ----
 * NvstFeedbackState::receive_server_control (nvst.rs:694-730), which the
 * bundle worker runs for BOTH control channels — control_reliable (sid 0)
 * and control_partial (sid 6), :8262-8272. Four codes are not ignorable:
 *   0x104/0x105  the idle-warning latch (active / expired)
 *   0x109        server termination — the reference clears the idle
 *                warning, publishes NvstServerTermination and returns
 *                true, which STOPS the receive loop (:707-713 + :8269)
 *   0x112        frame-pacing mode for stream 0 — this is the server
 *                switching our 0x0203 reports on and off (:714-721)
 * Everything else is Unhandled (counted) or Unknown (counted, with the
 * code kept as last_unknown). MALFORMED records are counted and the walk
 * CONTINUES — a bad record is never terminal, which is the whole point of
 * the reference's `Err(_) => malformed += 1` arm.
 * Returns 1 when the server terminated the session. */
static int sess_sctl_ingest(sess_pump *p, uint16_t sid, const uint8_t *data,
                            size_t len)
{
    kn_sctl_msg msgs[16];
    size_t n, i;
    int terminated = 0;

    if (sid != 0u && sid != 6u) return 0;      /* the two control channels */
    n = kn_sctl_walk(data, len, msgs, sizeof msgs / sizeof msgs[0]);
    if (n == 0u) return 0;
    p->sctl_msgs++;
    p->sctl_records += (unsigned)n;

    for (i = 0; i < n; i++) {
        kn_sctl_diag_add(&p->sctl_diag, &msgs[i]);
        switch (msgs[i].kind) {
        case KN_SCTL_TIMER:
            p->sctl_timer++;
            /* idle_warning_active.store(timer.active) (:702-706) */
            if (msgs[i].timer_active) {
                if (!p->sctl_idle_warning) p->sctl_idle_warnings++;
                p->sctl_idle_warning = 1u;
            } else {
                if (p->sctl_idle_warning) p->sctl_idle_expiries++;
                p->sctl_idle_warning = 0u;
            }
            printf("{\"mode\":\"session\",\"event\":\"session_timer\","
                   "\"type\":%u,\"seconds_left\":%u,\"active\":%u,"
                   "\"sid\":%u,\"t_ms\":%llu}\n",
                   (unsigned)msgs[i].timer_type, msgs[i].seconds_left,
                   msgs[i].timer_active, (unsigned)sid,
                   (unsigned long long)(kn_sock_now_ms() % 1000000000ull));
            fflush(stdout);
            break;
        case KN_SCTL_TERMINATION:
            p->sctl_terms++;
            p->sctl_idle_warning = 0u;         /* :708 clears the warning */
            p->sctl_term_wire = msgs[i].wire_code;
            if (msgs[i].have_reason) p->sctl_term_reason = msgs[i].reason;
            if (msgs[i].have_extended)
                p->sctl_term_extended = msgs[i].extended_code;
            p->sctl_term_resumable = msgs[i].have_resumable
                                         ? msgs[i].resumable : -1;
            printf("{\"mode\":\"session\",\"event\":\"server_termination\","
                   "\"wire_code\":%u,\"reason\":%u,\"extended\":%u,"
                   "\"resumable\":%d,\"sid\":%u}\n",
                   msgs[i].wire_code,
                   msgs[i].have_reason ? msgs[i].reason : 0u,
                   msgs[i].have_extended ? msgs[i].extended_code : 0u,
                   p->sctl_term_resumable, (unsigned)sid);
            fflush(stdout);
            p->sctl_termination = 1u;
            terminated = 1;
            break;
        case KN_SCTL_PACING_MODE:
            p->sctl_pacing++;
            /* stream 0 only; any other stream just counts invalid_stream
             * (:714-721). The latch default is ENABLED (nvst.rs:664). */
            if (msgs[i].stream_index == 0u) {
                unsigned want = msgs[i].pacing_interval ? 1u : 0u;
                if (want != p->sctl_pacing_enabled) {
                    printf("{\"mode\":\"session\",\"event\":\"frame_pacing\","
                           "\"enabled\":%u,\"mode\":%d,\"sid\":%u}\n",
                           want, msgs[i].pacing_interval, (unsigned)sid);
                    fflush(stdout);
                }
                p->sctl_pacing_enabled = want;
            }
            break;
        case KN_SCTL_UNHANDLED:
            p->sctl_unhandled++;
            break;
        case KN_SCTL_UNKNOWN:
            /* counted by kn_sctl_diag_add; named here so the log says
             * WHICH code the rig sent that we do not model */
            printf("{\"mode\":\"session\",\"event\":\"sctl_unknown\","
                   "\"code\":%u,\"sid\":%u}\n", (unsigned)msgs[i].code,
                   (unsigned)sid);
            fflush(stdout);
            break;
        case KN_SCTL_MALFORMED:
            printf("{\"mode\":\"session\",\"event\":\"sctl_malformed\","
                   "\"error\":%d,\"code\":%u,\"sid\":%u,\"len\":%u}\n",
                   (int)msgs[i].error, (unsigned)msgs[i].code,
                   (unsigned)sid, (unsigned)len);
            fflush(stdout);
            break;
        default:
            break;
        }
    }
    if (n == sizeof msgs / sizeof msgs[0]) {
        /* a bigger buffer than 16 records: say so instead of implying the
         * walk saw everything (the reference bounds it at 256) */
        printf("{\"mode\":\"session\",\"event\":\"sctl_truncated_walk\","
               "\"records\":%u,\"sid\":%u,\"len\":%u}\n", (unsigned)n,
               (unsigned)sid, (unsigned)len);
        fflush(stdout);
    }
    return terminated;
}

/* One inbound non-DCEP message: capture it verbatim (bounded), name its
 * records, count them, and react — INPUT_VERSION latches the negotiated
 * version (nvst_input.rs:339-351), a cursor notification drives the
 * handoff machine (nvst.rs:8322-8326). */
static void sess_ctrl_ingest(sess_pump *p, uint16_t sid, uint32_t ppi,
                             const uint8_t *data, size_t len)
{
    static const char hx[] = "0123456789abcdef";
    char hex[2 * SESS_CTRL_RAW_HEAD + 1];
    char names[256];
    sess_ctrl_names nc;
    size_t recs;
    unsigned slot;

    p->st.ctrl_msgs++;
    recs = kn_input_scan(data, len, sess_ctrl_hist_cb, p);
    p->st.ctrl_records += (unsigned)recs;
    sess_ctrl_refresh_hist(p);

    /* round XXXVIII: decode it as SERVER CONTROL too (sid 0 + sid 6, the
     * reference's two control channels, nvst.rs:8262-8272). The 0x02xx
     * input-plane scan above and this 0x01xx walk are different planes on
     * the same bytes — the rig's run-18 inventory (0x020e, 0x0111,
     * 0x010a, 0x010f, 0x0106, 0x010e, 0x0101) is Unhandled/Unknown here,
     * which is exactly what the reference's own decoder makes of it. */
    if (sess_sctl_ingest(p, sid, data, len))
        p->st.server_termination = 1u;

    nc.buf = names; nc.cap = sizeof names; nc.used = 0; names[0] = '\0';
    (void)kn_input_scan(data, len, sess_ctrl_names_cb, &nc);

    slot = p->ctrl_raw_n;
    if (slot < SESS_CTRL_RAW_MAX) {
        size_t c = len < sizeof p->ctrl_raw[slot].head
                       ? len : sizeof p->ctrl_raw[slot].head;
        size_t i;
        p->ctrl_raw[slot].sid = sid;
        p->ctrl_raw[slot].ppi = ppi;
        p->ctrl_raw[slot].len = (unsigned)len;
        p->ctrl_raw[slot].head_len = (unsigned)c;
        memcpy(p->ctrl_raw[slot].head, data, c);
        for (i = 0; i < c; ++i) {
            hex[2 * i]     = hx[p->ctrl_raw[slot].head[i] >> 4];
            hex[2 * i + 1] = hx[p->ctrl_raw[slot].head[i] & 0xFu];
        }
        hex[2 * c] = '\0';
        p->ctrl_raw_n++;
        printf("{\"mode\":\"session\",\"event\":\"ctrl_msg\",\"n\":%u,"
               "\"sid\":%u,\"ppi\":%u,\"len\":%u,\"records\":%u,"
               "\"codes\":\"%.255s\",\"hex\":\"%.96s\"}\n",
               slot + 1u, (unsigned)sid, (unsigned)ppi, (unsigned)len,
               (unsigned)recs, names, hex);
        fflush(stdout);
    }

    /* the handshake only negotiates on the reliable control stream
     * (nvst_input.rs:339-351 checks id == channels.control_reliable) */
    if (sid != 0u) return;
    if (p->input_version < 0) {
        int v = kn_input_protocol_version(data, len);
        if (v >= 0) {
            p->input_version = v;
            p->st.input_version = v;
            printf("{\"mode\":\"session\",\"event\":\"input_version\","
                   "\"version\":%d,\"sid\":%u,\"len\":%u,\"records\":%u}\n",
                   v, (unsigned)sid, (unsigned)len, (unsigned)recs);
            fflush(stdout);
        }
    }
    if (kn_input_has_cursor_notification(data, len))
        kn_input_cursor_notify(&p->cursor, kn_sock_now_ms());
}

/* finish_nvst_input_handshake (nvst.rs:6554-6591): once the control
 * channel is open AND the version has arrived, send all eight activation
 * messages, arm the cursor machine, and mark input ready. The reference
 * retries the WHOLE chain when any message fails to queue; we bound that
 * at 8 attempts so a dead association cannot spin (a failed association
 * is already reported through sctp_fail). */
static void sess_ctrl_try_activate(sess_pump *p)
{
    uint8_t chain[KN_INPUT_ACTIVATION_N][KN_INPUT_CMD_MAX];
    size_t lens[KN_INPUT_ACTIVATION_N];
    uint64_t now_ms = kn_sock_now_ms();
    uint64_t ts_us;
    unsigned i, sent = 0, fail = 0, bytes = 0;

    if (p->activation_sent) return;
    if (p->input_version < 0) return;
    if (!sess_ctrl_open(p)) return;
    if (p->activation_attempts >= 8u) return;
    p->activation_attempts++;
    p->st.activation_attempts = p->activation_attempts;

    ts_us = kn_sock_now_us() - p->origin_us;
    if (kn_input_activation_chain(ts_us, chain, lens) !=
            (int)KN_INPUT_ACTIVATION_N) {
        p->st.activation_fail = (unsigned)KN_INPUT_ACTIVATION_N;
        printf("{\"mode\":\"session\",\"event\":\"input_activation\","
               "\"sent\":0,\"failed\":%u,\"detail\":\"build\"}\n",
               (unsigned)KN_INPUT_ACTIVATION_N);
        fflush(stdout);
        return;
    }
    for (i = 0; i < KN_INPUT_ACTIVATION_N; ++i) {
        if (sess_ctrl_send(p, chain[i], lens[i])) {
            sent++;
            bytes += (unsigned)lens[i];
        } else {
            fail++;
        }
    }
    p->st.activation_sent = sent;
    p->st.activation_fail = fail;
    if (fail == 0u) {
        p->activation_sent = 1u;
        p->st.input_ready = 1u;
        /* nvst.rs:7519-7522 — the cursor machine arms with the chain */
        kn_input_cursor_activate(&p->cursor, now_ms);
    }
    printf("{\"mode\":\"session\",\"event\":\"input_activation\","
           "\"sent\":%u,\"failed\":%u,\"bytes\":%u,\"ts_us\":%llu,"
           "\"version\":%d,\"attempt\":%u,\"ready\":%u}\n",
           sent, fail, bytes, (unsigned long long)ts_us, p->input_version,
           p->activation_attempts, p->st.input_ready);
    fflush(stdout);
}

/* ---- round XXXVIII (R3-D): the WSS control-channel keepalive ----
 * The reference runs a dedicated worker over the RTSPS WebSocket for
 * the whole post-PLAY session (nvst_rtsp.rs:840-935): every
 * KEEPALIVE_INTERVAL (2 s) it sends Ping(seq as u64 BE), matches the
 * pong payload against the outstanding sequence (:895-901), records
 * the RTT sample, and expires it after CONTROL_PING_EXPIRY (5 s,
 * :269-274). An outstanding ping older than the interval is dropped
 * (:865-870). Every pass also drains RTSP responses (:906-921) and a
 * hard error ends the worker (:902-905).
 *
 * Run 18's WSS went silent after PLAY and nothing noticed: when video
 * stalled at 30.5 s we could not say whether the control channel was
 * alive. These counters make that a logged fact.
 *
 * Ownership: whoever set `have` services it — the pump thread when it
 * persists post-PLAY (the reference's shape: a worker beside the media
 * loop), else the media loop itself. Never both: main raises the hand
 * BEFORE the loop and kn_rtsp does not touch the socket again until
 * TEARDOWN, which runs after sess_pump_stop + sess_ctrl_ping_stop.
 * The 1 ms recv bound is the reference's non-blocking read
 * (CONTROL_IO_TIMEOUT = 100 ms in its dedicated worker; a media-loop
 * pass cannot afford that, and 1 ms keeps its ~10 ms cadence). */
#define SESS_CTRL_PING_INTERVAL_MS 2000u   /* KEEPALIVE_INTERVAL */
#define SESS_CTRL_PING_EXPIRY_MS   5000u   /* CONTROL_PING_EXPIRY */
#define SESS_CTRL_PING_RECV_MS     1u      /* bounded drain per pass */

typedef struct {
    kn_ws_client *ws;
    int      have;               /* service armed (post-PLAY) */
    int      dead;               /* hard error: the worker's break */
    uint64_t next_ping_ms;
    uint64_t outstanding_seq;    /* 0 = none outstanding */
    uint64_t outstanding_ms;
    unsigned long long seq;      /* wrapping_add(1) per ping */
    unsigned long long pings_sent, ping_send_failed;
    unsigned long long pongs_matched, pongs_unmatched;
    unsigned long long expired;  /* outstanding aged out (no pong) */
    unsigned long long msgs_drained;
    /* the reference's NvstControlPing sample: (received_at, elapsed) */
    int      have_sample;
    uint64_t sample_ms;
    uint64_t sample_rtt_us;
    uint64_t rtt_last_us, rtt_min_us, rtt_max_us;
} sess_ctrl_ping;

static void sess_ctrl_ping_init(sess_ctrl_ping *k, kn_ws_client *ws)
{
    memset(k, 0, sizeof *k);
    k->ws = ws;
    k->rtt_min_us = 0xFFFFFFFFFFFFFFFFull;
}

static void sess_ctrl_ping_stop(sess_ctrl_ping *k)
{
    if (!k->have) return;
    k->have = 0;
    printf("{\"mode\":\"session\",\"event\":\"ctrl_ping_stop\","
           "\"pings\":%llu,\"pongs\":%llu,\"unmatched\":%llu,"
           "\"expired\":%llu,\"drained\":%llu,\"dead\":%d,"
           "\"rtt_last_us\":%llu,\"rtt_min_us\":%llu,\"rtt_max_us\":%llu,"
           "\"sample_ms\":%llu}\n",
           k->pings_sent % 1000000000ull, k->pongs_matched % 1000000000ull,
           k->pongs_unmatched % 1000000000ull, k->expired % 1000000000ull,
           k->msgs_drained % 1000000000ull, k->dead,
           (unsigned long long)k->rtt_last_us,
           (unsigned long long)(k->rtt_min_us == 0xFFFFFFFFFFFFFFFFull
                                    ? 0ull : k->rtt_min_us),
           (unsigned long long)k->rtt_max_us,
           (unsigned long long)(k->have_sample ? k->sample_rtt_us : 0ull));
    fflush(stdout);
}

static void sess_ctrl_ping_service(sess_ctrl_ping *k, uint64_t now_ms)
{
    unsigned long long seen;
    if (!k->have || k->dead || !k->ws) return;

    /* the sample expires (ping_ms, nvst_rtsp.rs:266-274) */
    if (k->have_sample && now_ms - k->sample_ms >= SESS_CTRL_PING_EXPIRY_MS)
        k->have_sample = 0;
    /* an outstanding ping older than the interval is dropped (:865-870) */
    if (k->outstanding_seq &&
        now_ms - k->outstanding_ms >= SESS_CTRL_PING_INTERVAL_MS) {
        k->outstanding_seq = 0;
        k->have_sample = 0;
        k->expired++;
    }
    if (!k->outstanding_seq &&
        now_ms - k->next_ping_ms >= SESS_CTRL_PING_INTERVAL_MS) {
        uint8_t pl[8];
        int i;
        k->seq++;                              /* wrapping_add */
        for (i = 0; i < 8; i++)                /* to_be_bytes */
            pl[i] = (uint8_t)(k->seq >> (8 * (7 - i)));
        if (kn_ws_send_ping(k->ws, pl, sizeof pl) == KN_WS_OK) {
            k->pings_sent++;
            k->outstanding_seq = k->seq;
            k->outstanding_ms = now_ms;
        } else {
            k->ping_send_failed++;
            k->dead = 1;                       /* the worker's break */
        }
        k->next_ping_ms = now_ms;
    }

    /* drain: kn_ws answers pings and closes internally; text/binary
     * surface here (the reference pushes them into its RTSP buffer and
     * take_rtsp_response discards post-PLAY stragglers) */
    for (;;) {
        int op = 0;
        size_t n = 0;
        int rc = kn_ws_recv_msg(k->ws, SESS_CTRL_PING_RECV_MS, &op, &n);
        if (rc == KN_WS_ERR_TIMEOUT) break;    /* WouldBlock: keep going */
        if (rc != KN_WS_OK) {                  /* CLOSED / IO / BIG */
            k->dead = 1;
            break;
        }
        k->msgs_drained++;
    }
    /* pong match (:895-901): payload == outstanding seq, big-endian */
    seen = k->ws->pong_seen;
    for (;;) {
        uint8_t pl[KN_WS_MAX_CONTROL_PAYLOAD];
        size_t pln = 0;
        uint64_t got = 0;
        size_t z;
        if (!kn_ws_take_pong(k->ws, &seen, pl, sizeof pl, &pln)) break;
        if (pln != 8) { k->pongs_unmatched++; continue; }
        for (z = 0; z < 8; z++) got = (got << 8) | pl[z];
        if (k->outstanding_seq && got == k->outstanding_seq) {
            uint64_t rtt = (now_ms - k->outstanding_ms) * 1000ull;
            k->pongs_matched++;
            k->outstanding_seq = 0;
            k->have_sample = 1;
            k->sample_ms = now_ms;
            k->sample_rtt_us = rtt;
            k->rtt_last_us = rtt;
            if (rtt < k->rtt_min_us) k->rtt_min_us = rtt;
            if (rtt > k->rtt_max_us) k->rtt_max_us = rtt;
        } else {
            k->pongs_unmatched++;
        }
    }
}


/* one pump pass of the control plane: the handshake, then the cursor
 * machine (nvst.rs:7515-7531 and :7756-7766), then a 30 s evidence beat
 * so a long session shows the rig's command mix without a flood. */
static void sess_ctrl_service(sess_pump *p)
{
    uint64_t now_ms;
    if (!p->sctp) return;
    sess_ctrl_try_activate(p);
    now_ms = kn_sock_now_ms();
    if (kn_input_cursor_update(&p->cursor, now_ms, sess_cursor_send, p)) {
        printf("{\"mode\":\"session\",\"event\":\"cursor_handoff\","
               "\"reason\":\"%.32s\",\"enables\":%u,\"disables\":%u}\n",
               p->cursor.reason ? p->cursor.reason : "",
               p->cursor.enables, p->cursor.disables);
        fflush(stdout);
    }
    p->st.cursor_state = (unsigned)p->cursor.state;
    p->st.cursor_enables = p->cursor.enables;
    p->st.cursor_trackings = p->cursor.trackings;
    p->st.cursor_disables = p->cursor.disables;
    p->st.cursor_handoffs = p->cursor.handoffs;
    /* round XXXVIII: mirror the server-control plane for main (the
     * snapshot channel is the only legal read path while this thread
     * runs). The pacing latch gates the media loop's 0x0203 reports. */
    p->st.sctl_msgs = p->sctl_msgs;
    p->st.sctl_records = p->sctl_records;
    p->st.sctl_timer = p->sctl_timer;
    p->st.sctl_termination = p->sctl_terms;
    p->st.server_termination = p->sctl_termination;
    p->st.sctl_pacing = p->sctl_pacing;
    p->st.sctl_unhandled = p->sctl_unhandled;
    p->st.sctl_unknown = (unsigned)(p->sctl_diag.unknown % 1000000u);
    p->st.sctl_malformed = (unsigned)(p->sctl_diag.malformed % 1000000u);
    p->st.sctl_invalid_stream =
        (unsigned)(p->sctl_diag.invalid_stream % 1000000u);
    p->st.sctl_last_unknown = p->sctl_diag.have_last_unknown
                                  ? (unsigned)p->sctl_diag.last_unknown : 0u;
    p->st.idle_warning = p->sctl_idle_warning;
    p->st.idle_warnings = p->sctl_idle_warnings;
    p->st.idle_expiries = p->sctl_idle_expiries;
    p->st.pacing_enabled = p->sctl_pacing_enabled;
    p->st.term_wire_code = p->sctl_term_wire;
    p->st.term_reason = p->sctl_term_reason;
    p->st.term_extended = p->sctl_term_extended;
    p->st.term_resumable = p->sctl_term_resumable;
    if (now_ms >= p->next_ctrl_log_ms) {
        p->next_ctrl_log_ms = now_ms + 30000u;
        sess_ctrl_refresh_hist(p);
        printf("{\"mode\":\"session\",\"event\":\"ctrl_in\",\"t_ms\":%llu,"
               "\"msgs\":%u,\"records\":%u,\"version\":%d,\"ready\":%u,"
               "\"activation\":%u,\"cursor\":\"%s\",\"codes\":%s}\n",
               (unsigned long long)now_ms, p->st.ctrl_msgs,
               p->st.ctrl_records, p->input_version, p->st.input_ready,
               p->st.activation_sent,
               kn_input_cursor_state_name(p->cursor.state), p->st.ctrl_hist);
        fflush(stdout);
        /* the reference's own 10-second nvst-control line (:7985-7997),
         * minus the keepalive-health fields (no control keepalive worker
         * yet — R3-D). Printed on the same 30 s beat as ctrl_in so the
         * two lines always pair up in the log. */
        printf("{\"mode\":\"session\",\"event\":\"sctl_stats\","
               "\"unknown_commands\":%llu,\"last_unknown_command\":%u,"
               "\"malformed_commands\":%llu,\"invalid_stream_commands\":%llu,"
               "\"timers\":%u,\"terminations\":%u,\"pacing_msgs\":%u,"
               "\"unhandled\":%u,\"idle_warning\":%u,\"pacing_enabled\":%u}\n",
               (unsigned long long)(p->sctl_diag.unknown % 1000000000ull),
               p->st.sctl_last_unknown,
               (unsigned long long)(p->sctl_diag.malformed % 1000000000ull),
               (unsigned long long)(p->sctl_diag.invalid_stream %
                                    1000000000ull),
               p->sctl_timer, p->sctl_terms, p->sctl_pacing,
               p->sctl_unhandled, p->sctl_idle_warning,
               p->sctl_pacing_enabled);
        fflush(stdout);
    }
}

static void sess_sctp_msg(void *ctx, uint16_t sid, uint16_t ssn,
                          uint32_t ppi, int unordered,
                          const uint8_t *data, size_t len)
{
    sess_pump *p = (sess_pump *)ctx;
    (void)ssn; (void)unordered;
    if (ppi == KN_SCTP_PPI_DCEP && len >= 1) {
        if (data[0] == KN_SCTP_DCEP_MSG_OPEN) {
            /* the rig opened a channel toward us: answer with the ACK
             * (str0m sctp/mod.rs:1046-1060 does exactly this) */
            uint8_t ack[4];
            size_t alen = 0;
            p->st.sctp_rig_opened++;
            if (kn_sctp_dcep_ack_build(ack, sizeof ack, &alen) ==
                    KN_SCTP_OK &&
                kn_sctp_send(p->sctp, sid, KN_SCTP_PPI_DCEP, 1,
                             ack, alen) == KN_SCTP_OK)
                p->st.sctp_rig_acked++;
            return;
        }
        if (data[0] == KN_SCTP_DCEP_MSG_ACK) {
            p->st.sctp_acked++;
            if (p->st.sctp_acked == p->st.sctp_opened &&
                p->st.sctp_opened > 0) {
                printf("{\"mode\":\"session\",\"event\":\"sctp_channels\","
                       "\"opened\":%u,\"acked\":%u,\"rig_opened\":%u}\n",
                       p->st.sctp_opened, p->st.sctp_acked,
                       p->st.sctp_rig_opened);
                fflush(stdout);
            }
            return;
        }
    }
    /* any other message: the rig's control/rtcp payload. Round XXXVII
     * interprets it instead of only capturing the first one's head — the
     * D-t4 discipline (never silently dropped) now extends to naming
     * every TLV record and driving the input handshake from it. */
    p->st.sctp_msgs_in++;
    sess_ctrl_ingest(p, sid, ppi, data, len);
    if (p->st.sctp_first_len == 0 && p->st.sctp_first_ppi == 0) {
        size_t c = len < sizeof p->st.sctp_first
                       ? len : sizeof p->st.sctp_first;
        memcpy(p->st.sctp_first, data, c);
        p->st.sctp_first_sid = sid;
        p->st.sctp_first_ppi = ppi;
        p->st.sctp_first_len = (unsigned)len;
    }
}

static void sess_sctp_event(void *ctx, int established)
{
    sess_pump *p = (sess_pump *)ctx;
    if (established) {
        int n;
        if (p->st.sctp_up) return;         /* COMM_UP is one-shot here */
        p->st.sctp_up = 1;
        p->st.sctp_up_ms = kn_sctp_up_ms(p->sctp);
        printf("{\"mode\":\"session\",\"event\":\"sctp_up\","
               "\"ms\":%llu}\n",
               (unsigned long long)p->st.sctp_up_ms);
        fflush(stdout);
        /* the reference opens the channels the instant the association
         * is up (nvst.rs:8224-8233) */
        n = kn_sctp_open_channels(p->sctp, kn_sctp_nvst_channels,
                                  KN_SCTP_NVST_N_CHANNELS);
        p->st.sctp_opened = (unsigned)n;
    } else {
        /* Round XXXVIII: name the cause. Run 18's `sctp_down` with
         * sctp_fail:0 was only readable as SHUTDOWN_COMP by knowing
         * kn_sctp's internal lost-flag rule; the state integer and its
         * name now ride in the line itself, plus usrsctp's sac_error.
         * A graceful peer shutdown and a lost path are different
         * diagnoses: the first means the rig DECIDED the session was
         * over (what a rate controller at its floor does), the second
         * means the transport died underneath us. */
        kn_sctp_stats ds;
        memset(&ds, 0, sizeof ds);
        if (p->sctp) kn_sctp_stats_get(p->sctp, &ds);
        p->st.sctp_down_state = ds.have_assoc_state ? ds.assoc_state : 0u;
        p->st.sctp_down_error = ds.assoc_error;
        p->st.sctp_down_errno = ds.last_errno;
        printf("{\"mode\":\"session\",\"event\":\"sctp_down\","
               "\"cause\":\"%s\",\"assoc_state\":%u,\"assoc_error\":%u,"
               "\"errno\":%d,\"lost\":%d,\"msgs_in\":%u,\"sent\":%u,"
               "\"send_errors\":%u,\"txq_dropped\":%u,\"t_ms\":%llu}\n",
               kn_sctp_assoc_state_name(ds.assoc_state), ds.assoc_state,
               ds.assoc_error, ds.last_errno, ds.lost, ds.msgs_in,
               ds.sends, ds.send_errors, ds.txq_dropped,
               (unsigned long long)(kn_sock_now_ms() % 1000000000ull));
        fflush(stdout);
    }
}

/* one pump pass of the SCTP layer: DTLS app ring -> inject -> poll
 * (flushes tx through sess_sctp_write, up-calls msgs/events) */
static void sess_sctp_service(sess_pump *p)
{
    uint8_t abuf[KN_DTLS_MAX_RECORD];
    size_t alen = 0;
    int guard = 0;
    while (kn_dtls_read_app(p->dtls, abuf, sizeof abuf, &alen) == 1 &&
           ++guard < 64)
        (void)kn_sctp_inject(p->sctp, abuf, alen);
    if (kn_sctp_poll(p->sctp, sess_sctp_msg, p,
                     sess_sctp_event, p) == KN_SCTP_ERR_LOST)
        p->st.sctp_fail = 1;
    /* the feedback outbox drain (bounded per pass — the cadences live
     * on the main thread; this only ships what it queued). kn_sctp_send
     * refuses before COMM_UP, so pre-association pushes are dropped at
     * the source (sess_fb_push stays counted, the send counter honest). */
    if (p->fb && kn_sctp_established(p->sctp)) {
        sess_fb_msg m;
        guard = 0;
        while (sess_fb_pop(p->fb, &m) && ++guard <= 32) {
            int ord;
            uint16_t pol;
            uint32_t val;
            sess_fb_sid_params(m.sid, &ord, &pol, &val);
            if (kn_sctp_send_pr(p->sctp, m.sid, KN_SCTP_PPI_BINARY, ord,
                                pol, val, m.payload, m.len) == KN_SCTP_OK)
                p->st.fb_sent++;
        }
    }
    {   /* mirror the counters for the snapshot / evidence lines */
        kn_sctp_stats ss;
        kn_sctp_stats_get(p->sctp, &ss);
        p->st.sctp_msgs_in = ss.msgs_in;
        p->st.sctp_out = ss.pkts_out;
        p->st.sctp_in = ss.pkts_in;
        if (ss.send_errors && !p->st.sctp_fail) p->st.sctp_fail = 3;
        if (ss.lost) p->st.sctp_fail = 1;
    }
}

static void sess_ice_pump_run(sess_pump *p, uint32_t budget_ms)
{
    uint64_t t0 = kn_sock_now_ms();
    uint64_t t_end = budget_ms ? t0 + budget_ms : ~(uint64_t)0;
    uint64_t last_burst = t0 - 20u;            /* fire at once (:7438) */
    uint64_t last_vid_ka = t0 - 20u;           /* and the video leg too */
    const uint64_t ms_base = p->st.pump_ms;    /* sequential merges */
    while (!p->stop && !g_knv_stop && kn_sock_now_ms() < t_end) {
        uint8_t tx[KN_SOCK_STUN_MAX_PKT];
        uint8_t tid[12];
        char uname[400];
        size_t plen;
        int i, n, nominate;
        unsigned cadence;
        /* live total (round XXXII: the persist-mode snapshot used to
         * read 0 — pump_ms only landed at thread exit) */
        p->st.pump_ms = ms_base + (kn_sock_now_ms() - t0);
        if (p->snap_req && !p->snap_done) {    /* main wants the stats */
            p->snap = p->st;
            p->snap_done = 1;
        }
        /* the pre-PLAY Mjolnir NATT keepalive from the video socket
         * (:8849-8905 — the same shape the media loop sends post-PLAY;
         * 20 ms until handoff, send-only, never recv here) */
        if (p->vid_sock && !p->vid_handoff &&
            kn_sock_now_ms() - last_vid_ka >= 20u) {
            last_vid_ka = kn_sock_now_ms();
            snprintf(uname, sizeof uname, "%s:%s", p->vid_payload,
                     p->local_ufrag);
            if (kn_ws_rand(tid, sizeof tid) == 0 &&
                kn_sock_stun_build(tx, sizeof tx, &plen,
                                   KN_SOCK_STUN_BINDING_REQUEST, tid,
                                   (const uint8_t *)p->remote_pwd,
                                   strlen(p->remote_pwd),
                                   KN_SOCK_STUN_ATTR_USERNAME,
                                   (const uint8_t *)uname,
                                   strlen(uname)) == KN_SOCK_OK &&
                kn_sock_udp_sendto(p->vid_sock, p->vid_ip, p->vid_port,
                                   tx, plen) == KN_SOCK_OK) {
                p->vid_ka_sent++;
                p->st.vid_ka_sent = p->vid_ka_sent;
            }
        }
        /* the reference bundle worker's cadence (:7837-7846): 20 ms of
         * 3xSTUN+1xNATT until dtls_ready, then NATT-only every 100 ms
         * (PING_INTERVAL_AFTER_CONNECTION :243). STUN requests from the
         * rig keep being answered in BOTH regimes (below). */
        cadence = p->st.dtls_ready ? 100u : 20u;
        /* RFC 8445 nomination (deviation D-t8, retiring D-t1): once the
         * pair is proven by REAL inbound traffic, our binding requests
         * carry USE-CANDIDATE — the controlling-agent flag str0m's ICE
         * agent sends in the reference (it runs alongside the hand-built
         * bursts there: every STUN datagram feeds rtc.handle_input,
         * nvst.rs:8652). Run 15's hypothesis: the rig gates its data
         * plane on the SELECTED pair. After dtls_ready the hand-built
         * layer is NATT-only, but str0m keeps its nominated pair alive —
         * we mirror that with ONE nominated request per 100 ms tick. */
        nominate = (p->st.req_in || p->st.ok_in);
        if (kn_sock_now_ms() - last_burst >= cadence) {
            if (!p->st.dtls_ready) {
                snprintf(uname, sizeof uname, "%s:%s", p->remote_ufrag,
                         p->local_ufrag);
                for (i = 0; i < 3; ++i) {      /* the ICE burst (:7848) */
                    if (kn_ws_rand(tid, sizeof tid) != 0) break;
                    if (kn_sock_stun_build2(tx, sizeof tx, &plen,
                                            KN_SOCK_STUN_BINDING_REQUEST,
                                            tid,
                                            (const uint8_t *)p->remote_pwd,
                                            strlen(p->remote_pwd),
                                            KN_SOCK_STUN_ATTR_USERNAME,
                                            (const uint8_t *)uname,
                                            strlen(uname),
                                            nominate
                                                ? KN_SOCK_STUN_ATTR_USE_CANDIDATE
                                                : 0,
                                            NULL, 0) != KN_SOCK_OK)
                        continue;
                    if (kn_sock_udp_sendto(p->sock, p->dest_ip, p->dest_port,
                                           tx, plen) == KN_SOCK_OK) {
                        p->st.stun_sent++;
                        if (nominate) p->st.nom_sent++;
                    } else {
                        p->st.send_errors++;
                    }
                }
            } else if (nominate) {
                /* the post-ready nominated keepalive (the str0m agent's
                 * selected-pair traffic; the NATT ping follows below) */
                snprintf(uname, sizeof uname, "%s:%s", p->remote_ufrag,
                         p->local_ufrag);
                if (kn_ws_rand(tid, sizeof tid) == 0 &&
                    kn_sock_stun_build2(tx, sizeof tx, &plen,
                                        KN_SOCK_STUN_BINDING_REQUEST, tid,
                                        (const uint8_t *)p->remote_pwd,
                                        strlen(p->remote_pwd),
                                        KN_SOCK_STUN_ATTR_USERNAME,
                                        (const uint8_t *)uname,
                                        strlen(uname),
                                        KN_SOCK_STUN_ATTR_USE_CANDIDATE,
                                        NULL, 0) == KN_SOCK_OK) {
                    if (kn_sock_udp_sendto(p->sock, p->dest_ip,
                                           p->dest_port, tx,
                                           plen) == KN_SOCK_OK) {
                        p->st.stun_sent++;
                        p->st.nom_sent++;
                    } else {
                        p->st.send_errors++;
                    }
                }
            }
            /* the NATT hole-punch ping (:5033-5050, :7868-7890) */
            snprintf(uname, sizeof uname, "%s:%s", p->natt, p->local_ufrag);
            if (kn_ws_rand(tid, sizeof tid) == 0 &&
                kn_sock_stun_build(tx, sizeof tx, &plen,
                                   KN_SOCK_STUN_BINDING_REQUEST, tid,
                                   (const uint8_t *)p->remote_pwd,
                                   strlen(p->remote_pwd),
                                   KN_SOCK_STUN_ATTR_USERNAME,
                                   (const uint8_t *)uname,
                                   strlen(uname)) == KN_SOCK_OK) {
                if (kn_sock_udp_sendto(p->sock, p->dest_ip, p->dest_port,
                                       tx, plen) == KN_SOCK_OK)
                    p->st.natt_sent++;
                else
                    p->st.send_errors++;
            }
            last_burst = kn_sock_now_ms();
        }
        /* R3-B: the DTLS handshake, gated EXACTLY like the reference —
         * the ClientHello arms at the first REAL inbound STUN evidence
         * (a rig request answered or a validated success; nvst.rs
         * :8196-8198: "skips setupDtls until a real inbound STUN. Do
         * not synthesize Binding Success"). From there every loop pass
         * steps the handshake (non-blocking; retransmissions are
         * timer-driven inside kn_dtls) while the bursts continue. */
        if (p->dtls && !p->st.dtls_ready && p->st.dtls_fail_rc == 0) {
            if (!p->st.dtls_started && (p->st.req_in || p->st.ok_in))
                p->st.dtls_started = 1;
            if (p->st.dtls_started) {
                int drc = kn_dtls_poll(p->dtls);
                if (drc == KN_DTLS_READY) {
                    char cs[64];
                    kn_dtls_stats ds;
                    p->st.dtls_ready = 1;
                    p->st.dtls_ready_ms = kn_dtls_ready_ms(p->dtls);
                    kn_dtls_stats_get(p->dtls, &ds);
                    p->st.dtls_in = ds.records_in;
                    p->st.dtls_out = ds.records_out;
                    p->st.dtls_verify = ds.verify_result;
                    p->st.dtls_app_records = ds.app_records;
                    p->st.dtls_app_bytes = ds.app_bytes;
                    p->st.dtls_first_app_len = ds.first_app_len;
                    memcpy(p->st.dtls_first_app, ds.first_app,
                           sizeof p->st.dtls_first_app);
                    (void)kn_dtls_ciphersuite(p->dtls, cs, sizeof cs);
                    printf("{\"mode\":\"session\",\"event\":\"dtls_ready\","
                           "\"ms\":%llu,\"version\":\"%.16s\","
                           "\"cipher\":\"%.48s\",\"records_in\":%u,"
                           "\"records_out\":%u,\"verify\":%d,"
                           "\"srtp_prof\":%u}\n",
                           (unsigned long long)p->st.dtls_ready_ms,
                           kn_dtls_version(p->dtls), cs, ds.records_in,
                           ds.records_out, ds.verify_result,
                           kn_dtls_srtp_profile(p->dtls));
                    fflush(stdout);
                } else if (drc < 0) {
                    kn_dtls_stats ds;
                    p->st.dtls_fail_rc = (unsigned)(-drc);
                    kn_dtls_stats_get(p->dtls, &ds);
                    p->st.dtls_last_rc = ds.last_rc;
                    p->st.dtls_verify = ds.verify_result;
                    p->st.dtls_in = ds.records_in;
                    p->st.dtls_out = ds.records_out;
                    printf("{\"mode\":\"session\",\"event\":\"dtls_fail\","
                           "\"rc\":%d,\"mbedtls_rc\":%d,\"verify\":%d,"
                           "\"pin_matched\":%d,\"records_in\":%u,"
                           "\"records_out\":%u,\"send_errors\":%u}\n",
                           drc, ds.last_rc, ds.verify_result,
                           ds.pin_matched, ds.records_in, ds.records_out,
                           ds.send_errors);
                    fflush(stdout);
                }
            }
        } else if (p->dtls && p->st.dtls_ready) {
            /* keep the established DTLS session serviced: poll (drains
             * app records into the ring, answers post-handshake
             * traffic), then hand the ring to the SCTP layer (R3-C) */
            kn_dtls_stats ds;
            (void)kn_dtls_poll(p->dtls);
            kn_dtls_stats_get(p->dtls, &ds);
            p->st.dtls_in = ds.records_in;
            p->st.dtls_out = ds.records_out;
            p->st.dtls_app_records = ds.app_records;
            p->st.dtls_app_bytes = ds.app_bytes;
            p->st.dtls_first_app_len = ds.first_app_len;
            memcpy(p->st.dtls_first_app, ds.first_app,
                   sizeof p->st.dtls_first_app);
            p->st.dtls_peer_closed = ds.peer_closed;
            p->st.dtls_post_first_len = ds.first_post_ready_len;
            memcpy(p->st.dtls_post_first, ds.first_post_ready,
                   sizeof p->st.dtls_post_first);
            if (!p->sctp && !p->sctp_tried) {
                char serr[160];
                p->sctp_tried = 1;
                serr[0] = '\0';
                p->sctp = kn_sctp_open(sess_sctp_write, p,
                                       serr, sizeof serr);
                p->st.sctp_armed = p->sctp ? 1u : 0u;
                if (!p->sctp) p->st.sctp_fail = 2;
                printf("{\"mode\":\"session\",\"event\":\"sctp_arm\","
                       "\"armed\":%d,\"detail\":\"%.120s\"}\n",
                       p->sctp ? 1 : 0, serr);
                fflush(stdout);
            }
            if (p->sctp)
                sess_sctp_service(p);
            sess_ctrl_service(p);        /* round XXXVII */
        }
        {   /* drain inbound: answer the rig's binding requests, count
             * its binding successes (handle_stun_datagram :5151-5172),
             * feed DTLS records to the handshake, PONG the literal
             * PING (:8605-8614) */
            size_t rlen = 0, ulen = 0;
            uint16_t mtype = 0, xport = 0;
            uint32_t xip = 0;
            uint8_t tid_in[12];
            char user[400];
            n = kn_sock_udp_recv(p->sock, p->rx, sizeof p->rx, 5, &rlen,
                                 NULL);
            if (n != 1) continue;              /* timeout/error: keep pumping */
            if (rlen == 4 && memcmp(p->rx, "PING", 4) == 0) {
                if (kn_sock_udp_sendto(p->sock, p->dest_ip, p->dest_port,
                                       (const uint8_t *)"PONG",
                                       4) == KN_SOCK_OK)
                    p->st.pongs_sent++;
                continue;
            }
            if (!kn_sock_stun_inspect(p->rx, rlen, &mtype, tid_in, user,
                                      sizeof user, &ulen, &xip, &xport)) {
                /* the demux order of the reference worker: STUN by
                 * magic cookie first, then DTLS by content type
                 * (looks_like_dtls :6345 — first byte 20..63) */
                if (kn_dtls_peek(p->rx, rlen) && p->dtls) {
                    (void)kn_dtls_inject(p->dtls, p->rx, rlen);
                    /* a fresh record may advance the handshake at once */
                    continue;
                }
                /* round XXXVI: RTP/RTCP on the bundle = the rig's AUDIO
                 * (rtcAudioOnNativeBundle:1 — the reference admits it via
                 * audio_streams, :8620-8627; our audio stage comes later)
                 * — classified, not "invalid" (run 16: invalid_in:175). */
                if (rlen >= 12 && (p->rx[0] >> 6) == 2) {
                    if (p->rx[1] >= 192 && p->rx[1] <= 223)
                        p->st.bundle_rtcp_in++;
                    else
                        p->st.bundle_rtp_in++;
                    continue;
                }
                p->st.invalid_in++;
                continue;
            }
            if (mtype == KN_SOCK_STUN_BINDING_REQUEST) {
                char expect[400];
                p->st.req_in++;
                snprintf(expect, sizeof expect, "%s:%s", p->local_ufrag,
                         p->remote_ufrag);
                if (ulen == strlen(expect) &&
                    memcmp(user, expect, ulen) == 0 &&
                    kn_sock_stun_check_integrity(p->rx, rlen,
                                                 (const uint8_t *)p->local_pwd,
                                                 strlen(p->local_pwd))) {
                    /* the reference echoes the request's SOURCE (:5166);
                     * the socket is CONNECTED to dest (the kernel peer
                     * filter), so source == dest is exact, not a guess */
                    uint8_t xa[8];
                    size_t xlen;
                    (void)xip; (void)xport;
                    if (kn_sock_stun_xor_addr_v4(p->dest_ip, p->dest_port,
                                                 xa) == KN_SOCK_OK &&
                        kn_sock_stun_build(tx, sizeof tx, &xlen,
                                           KN_SOCK_STUN_BINDING_SUCCESS,
                                           tid_in,
                                           (const uint8_t *)p->local_pwd,
                                           strlen(p->local_pwd),
                                           KN_SOCK_STUN_ATTR_XOR_MAPPED_ADDRESS,
                                           xa, sizeof xa) == KN_SOCK_OK &&
                        kn_sock_udp_sendto(p->sock, p->dest_ip,
                                           p->dest_port, tx,
                                           xlen) == KN_SOCK_OK)
                        p->st.answered++;
                }
            } else if (mtype == KN_SOCK_STUN_BINDING_SUCCESS) {
                if (kn_sock_stun_check_integrity(p->rx, rlen,
                                                 (const uint8_t *)p->remote_pwd,
                                                 strlen(p->remote_pwd)))
                    p->st.ok_in++;
                else
                    p->st.invalid_in++;
            } else {
                p->st.invalid_in++;
            }
        }
    }
    p->st.pump_ms = ms_base + (kn_sock_now_ms() - t0);
    if (p->snap_req && !p->snap_done) {        /* a late request still
                                                  gets an answer */
        p->snap = p->st;
        p->snap_done = 1;
    }
}

#ifdef _WIN32
static DWORD WINAPI sess_pump_entry(void *a)
{
    sess_ice_pump_run((sess_pump *)a, 0);
    return 0;
}
#else
static void *sess_pump_entry(void *a)
{
    sess_ice_pump_run((sess_pump *)a, 0);
    return NULL;
}
#endif

static int sess_pump_start(sess_pump *p)
{
    p->stop = 0;
#ifdef _WIN32
    {
        HANDLE h = CreateThread(NULL, 0, sess_pump_entry, p, 0, NULL);
        if (!h) return -1;
        p->th = (uintptr_t)h;
    }
#else
    if (pthread_create(&p->th, NULL, sess_pump_entry, p) != 0) return -1;
#endif
    p->started = 1;
    return 0;
}

static void sess_pump_stop(sess_pump *p)
{
    if (!p->started) return;
    p->stop = 1;
#ifdef _WIN32
    /* the pump checks stop at least every ~25 ms (5 ms recv timeout +
     * the 20 ms cadence), so the join is bounded in practice */
    (void)WaitForSingleObject((HANDLE)p->th, INFINITE);
    CloseHandle((HANDLE)p->th);
#else
    (void)pthread_join(p->th, NULL);
#endif
    p->started = 0;
}

/* the sequential form (the spawn-failure fallback and the disablePlay
 * path): the same core on a bounded budget, stats merged into the
 * caller's struct. */
static void sess_ice_pump(kn_sock_udp *sock, uint32_t dest_ip,
                          uint16_t dest_port, const char *remote_ufrag,
                          const char *remote_pwd, const char *local_ufrag,
                          const char *local_pwd, const char *natt_payload,
                          uint8_t *rx, size_t rxsz, uint32_t budget_ms,
                          sess_ice_stats *st)
{
    sess_pump p;
    (void)rx; (void)rxsz;      /* the ctx carries its own 2 KiB buffer —
                                  pre-PLAY traffic is STUN-sized */
    memset(&p, 0, sizeof p);
    p.sock = sock;
    p.dest_ip = dest_ip;
    p.dest_port = dest_port;
    pump_set_str(p.remote_ufrag, sizeof p.remote_ufrag, remote_ufrag);
    pump_set_str(p.remote_pwd, sizeof p.remote_pwd, remote_pwd);
    pump_set_str(p.local_ufrag, sizeof p.local_ufrag, local_ufrag);
    pump_set_str(p.local_pwd, sizeof p.local_pwd, local_pwd);
    pump_set_str(p.natt, sizeof p.natt, natt_payload);
    sess_ice_pump_run(&p, budget_ms);
    st->stun_sent += p.st.stun_sent;
    st->natt_sent += p.st.natt_sent;
    st->req_in += p.st.req_in;
    st->answered += p.st.answered;
    st->ok_in += p.st.ok_in;
    st->invalid_in += p.st.invalid_in;
    st->send_errors += p.st.send_errors;
    st->pongs_sent += p.st.pongs_sent;
    st->nom_sent += p.st.nom_sent;
    st->pump_ms += p.st.pump_ms;
    /* the dtls/sctp fields stay zero on the sequential path */
}
static void sess_sleep(uint32_t ms, void *c);   /* fwd (snapshot helper) */

/* R3-B: the reference's socket-pair reservation
 * (reserve_nvst_socket_pair_preferred, nvst.rs:5953-6000). Video
 * (Mjolnir) on N, the ICE/DTLS bundle on N+1 — the official Windows
 * client reserves 49005/49006 FIRST (useReserved=1) because "some
 * cloud seats do not route the dedicated Mjolnir video flow back to an
 * arbitrary source port" (:5941-5944), and only falls back to a
 * dynamic adjacent pair (<= 32 attempts). Returns KN_SOCK_OK with both
 * sockets open, or a negative error (nothing left open). */
#define SESS_VIDEO_PORT_PREF 49005u
static int sess_reserve_pair(kn_sock_udp *vid, kn_sock_udp *bun,
                             const char **selection, char *err, size_t errsz)
{
    int attempt;
    if (kn_sock_udp_open(vid, NULL, SESS_VIDEO_PORT_PREF,
                         KN_SOCK_RCVBUF_DEFAULT, err, errsz) == KN_SOCK_OK) {
        if (kn_sock_udp_open(bun, NULL, (uint16_t)(SESS_VIDEO_PORT_PREF + 1),
                             KN_SOCK_RCVBUF_DEFAULT, err,
                             errsz) == KN_SOCK_OK) {
            *selection = "preferred";
            return KN_SOCK_OK;
        }
        kn_sock_udp_close(vid);
    }
    for (attempt = 0; attempt < 32; ++attempt) {
        uint16_t vp;
        if (err && errsz) err[0] = '\0';
        if (kn_sock_udp_open(vid, NULL, 0, KN_SOCK_RCVBUF_DEFAULT,
                             err, errsz) != KN_SOCK_OK)
            continue;
        vp = kn_sock_udp_port(vid);
        if (vp != 0 && vp != 65535u &&
            kn_sock_udp_open(bun, NULL, (uint16_t)(vp + 1u),
                             KN_SOCK_RCVBUF_DEFAULT, err,
                             errsz) == KN_SOCK_OK) {
            *selection = "dynamic";
            return KN_SOCK_OK;
        }
        kn_sock_udp_close(vid);
    }
    if (err && errsz && err[0] == '\0') {
        /* bounded copy, not snprintf: the paranoid =2 gate on this file
         * cannot prove errsz > 1 (the round-XI discipline) */
        static const char m[] = "no adjacent UDP pair available";
        size_t n = sizeof m - 1;
        if (n > errsz - 1) n = errsz - 1;
        memcpy(err, m, n);
        err[n] = '\0';
    }
    return KN_SOCK_ERR_SOCKET;
}

/* The snapshot handshake for a RUNNING pump thread: raise snap_req,
 * the worker copies its stats at its next loop pass (<= ~25 ms: 5 ms
 * recv timeout + the 20 ms cadence) and raises snap_done. Bounded wait
 * (500 ms); on a stuck worker the caller gets the last snapshot — the
 * evidence degrades, never the session. */
static void sess_pump_snapshot(sess_pump *p, sess_ice_stats *out)
{
    if (!p->started) {
        *out = p->st;
        return;
    }
    p->snap_done = 0;
    p->snap_req = 1;
    {
        uint64_t t0 = kn_sock_now_ms();
        while (!p->snap_done && kn_sock_now_ms() - t0 < 500u)
            sess_sleep(2u, NULL);
    }
    p->snap_req = 0;
    *out = p->snap_done ? p->snap : p->st;
}

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

/* The bundle identity: the OFFICIAL GenerateIceCredentials shapes
 * (nvst.rs:5545-5559 — the base64-ish alphabet, a 4-char ufrag and a
 * 22-char password; :5564-5565: str0m's default 16-char ufrag is
 * REJECTED by Bifrost length checks). Round XXIX: our old 16/24-hex
 * shapes were tolerated at ANNOUNCE+ICE (runs 9/10) but fidelity is
 * the R32 rule and the R3 DTLS phase reuses this identity. The
 * fingerprint stays a random 32-byte colon-hex PLACEHOLDER until R3-A
 * ships the real certificate (deviation D-s1; the colon-uppercase-hex
 * format itself matches nvst_fingerprint_hex :5596-5603). */
static void sess_local_identity(char *ufrag, size_t ufsz, char *pwd,
                                size_t pwsz, char *fp, size_t fpsz)
{
    static const char alpha[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/";
    uint8_t raw[32];
    size_t i;
    if (kn_ws_rand(raw, sizeof raw) != 0) {
        /* the CSPRNG refusing is fatal for the session anyway; fill
         * deterministic placeholders so the SDP stays well-formed and the
         * failure surfaces at the handshake, not as garbage */
        memset(raw, 0x5A, sizeof raw);
    }
    (void)ufsz; (void)pwsz;               /* callers pass [24] and [32] */
    for (i = 0; i < 4u; ++i)  ufrag[i] = alpha[raw[i] & 0x3Fu];
    ufrag[4] = '\0';
    for (i = 0; i < 22u; ++i) pwd[i] = alpha[raw[4u + i] & 0x3Fu];
    pwd[22] = '\0';
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
/* Round XXIII: the production WebRTC client polls the session every
 * ~4.1 s (the user's DevTools capture, 2026-10-25: Δ = 4.15/4.12/4.42 s).
 * --session mirrors that cadence instead of the round-XXI 2 s guess. */
#define SESS_POLL_MS   4000u

/* cloudmatch.rs get_session :1369-1377 — the statuses worth an immediate
 * in-round retry (3 attempts, 250/750 ms backoff) */
static int cm_status_transient(int status)
{
    return status == 408 || status == 425 || status == 429 ||
           status == 500 || status == 502 || status == 503 || status == 504;
}

/* D-s4 (round XXVI — the R7 run-7 lesson): a gentle backoff while poll
 * rounds keep failing, 4 s → 6 s → 9 s → 13.5 s → capped at 20 s, reset
 * on any success. Run 7 measured a 503 storm (15 consecutive 503s over
 * ~30 s, and a same-moment DELETE to the SAME host answered 200 — the
 * node was up; the GET path for that session record was not) starting at
 * the ~2-minute step-1 mark where runs 5/6 transitioned to seat_step 3:
 * transient statuses are OUTLIVED (bounded only by queue_timeout, like
 * the reference UI which never gives up on a poll error), with
 * progressively lighter traffic instead of a fixed-rate hammer. */
static uint32_t sess_poll_gap(unsigned consec_fails)
{
    uint32_t gap = SESS_POLL_MS;
    unsigned i;
    for (i = 0; i < consec_fails; ++i) {
        if (gap >= 20000u) break;
        gap += gap / 2u;
    }
    return gap > 20000u ? 20000u : gap;
}

static int run_session(const char *token, const char *device_id,
                       long long app_id, const char *title,
                       const char *cm_host, long seconds,
                       long queue_timeout, int keep_on_abort,
                       long expect_frames, long play_wait_ms,
                       long play_settle_ms,
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
    sess_ctrl_ping ctrl_ping;          /* R3-D: the WSS keepalive */
    kn_rtsp_client rtc;
    kn_rtsp_response resp;
    kn_rtsp_describe desc;
    kn_rtsp_setup_result setup;
    kn_sock_udp sock;                   /* video (Mjolnir) socket, port N */
    kn_sock_udp bsock;                  /* R3-B: ICE/DTLS bundle, port N+1
                                           (the reference's pair layout,
                                           nvst.rs:5472-5541/:5933-6000) */
    int have_sock = 0, have_bsock = 0;  /* open flags for the teardown */
    kn_cert *cert = NULL;               /* R3-A identity, kept ALIVE for
                                           the R3-B handshake (R3-A freed
                                           it right after fingerprinting) */
    kn_dtls *dtls = NULL;               /* armed into the pump when the
                                           DESCRIBE fp + a real cert exist */
    int unified_mode = 0;               /* described || bundle_fallback
                                           (nvst_rtsp.rs:1205) */
    const char *pair_sel = "none";      /* sock_pair evidence: preferred |
                                           dynamic (nvst.rs:5953-6000) */
    int pump_persist = 0;               /* the bundle worker outlives PLAY
                                           (non-unified only, D-t2) */
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
    char authbuf[288];                  /* round XXIV: the RTSP Host header
                                           authority (host:port — the
                                           reference strips only the
                                           scheme, nvst_rtsp.rs:1070-76) */
    char poll_host[256];                /* round XXIII: the learned control
                                           node (fallback = cm_host) */
    const char *poll_src;
    uint16_t poll_port = 443;
    unsigned poll_fails = 0;            /* consecutive failed poll rounds */
    unsigned consec_transport = 0, consec_hard = 0;  /* the two BOUNDED
                                           failure classes (XXVI) — the
                                           transient class is outlived and
                                           needs no counter (XXVII: the
                                           target GCC 16.2 flagged the
                                           write-only one, T10 r20) */
    /* round XXV: the pre-PLAY ICE burst state (see sess_ice_pump) */
    char ice_remote_ufrag[288];         /* the resolved ladder output */
    char ice_natt[288];                 /* NATT payload: bundle user | PING */
    uint32_t ice_dest_ip = 0;
    uint16_t ice_dest_port = 0;
    uint32_t vid_peer_ip = 0;           /* R3-B: the EFFECTIVE video peer
                                           (unified => the bundle peer,
                                           nvst_rtsp.rs:1207-1214) */
    uint16_t vid_peer_port = 0;
    kn_sock_udp *ms = NULL;             /* the media-loop socket: bundle
                                           in unified mode, video otherwise */
    int ice_enabled = 0, play_attempts = 0, pump_th = 0;
    int ka_on = 0;                        /* round XXVIII: the Mjolnir
                                             NATT keepalive (nvst.rs
                                             :8849-8905) */
    const char *ka_payload = "PING";
    uint32_t ka_ip = 0;
    uint16_t ka_port = 0;
    uint64_t next_ka_ms = 0;
    unsigned long long ka_sent = 0;
    unsigned relearns = 0;                /* cm_control re-learn events */
    static sess_pump pump;
    static sess_fb_outbox fb;           /* round XXXVI: main -> pump */
    sess_ice_stats ice_st;
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
    uint64_t ctrl_ping_next_ms = 0;    /* R3-D service beat */
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
        char esc[96];
        lb_json_path(esc, sizeof esc, sess.req_status_desc);
        printf("{\"mode\":\"session\",\"error\":\"cm_create\",\"rc\":%d,"
               "\"http_status\":%d,\"cloud_status\":%lld,\"desc\":\"%s\"}\n",
               rc, http_status, (long long)sess.req_status_code, esc);
        fflush(stdout);
        kn_log_on_failure("session.cm", "E_SESSION_CM_CREATE",
                          "POST v2/session did not return a usable session",
                          "check the account/token/entitlements; the raw "
                          "status is in this log (R7 measurement); "
                          "cloud_status 11 = SESSION_LIMIT (an earlier "
                          "session is still alive server-side)");
        kn_log_shutdown();
        return 1;
    }
    printf("{\"mode\":\"session\",\"event\":\"cm_created\",\"session_id\":"
           "\"%.64s\",\"status\":%lld,\"phase\":\"%s\",\"queue_position\":"
           "%lld,\"have_queue\":%d}\n",
           sess.session_id, (long long)sess.status, sess.phase,
           (long long)sess.queue_position, sess.have_queue);
    fflush(stdout);

    /* Poll-target switch (round XXIII — the R7 run-1 finding): the create
     * response nominates the REGIONAL control node that owns the session
     * (session.sessionControlInfo.ip — the user's browser capture shows
     * the official client polling exactly that host, np-waw-01.cloudmatch
     * beta.nvidiagrid.net, at ~4.1 s, while runs 1-4 polled the global
     * create front and saw a FROZEN queuePosition + intermittent non-200s
     * -> cm_poll_failed). The reference learns the same control_base
     * (cloudmatch.rs:401-414, :1149-1152) and every poll goes there
     * (:652-656 -> get_session :1356). kn_cm_control_base applies the
     * reference's trust rules; an untrusted/absent value falls back to
     * the create host (:1984 unwrap_or semantics). */
    if (kn_cm_control_base(&sess, poll_host, sizeof poll_host, &poll_port))
        poll_src = "sessionControlInfo";
    else {
        snprintf(poll_host, sizeof poll_host, "%.250s", cm_host);
        poll_port = 443;
        poll_src = "create-host";
    }
    {
        char esc[300];
        lb_json_path(esc, sizeof esc, poll_host);
        printf("{\"mode\":\"session\",\"event\":\"cm_control\","
               "\"poll_host\":\"%s\",\"port\":%u,\"source\":\"%s\"}\n",
               esc, (unsigned)poll_port, poll_src);
        fflush(stdout);
    }

    /* Poll until ready: one fresh TLS connection per attempt
     * (Connection: close), SESS_POLL_MS cadence, bounded by queue_timeout.
     * A round retries transient outcomes up to 3 attempts (250/750 ms —
     * get_session :1369-1394); a still-failed round is tolerated up to 4
     * consecutive times before cm_poll_failed ends the wait. Failed rounds
     * NEVER clobber the live session state (poll into a temp + commit on
     * success — kn_cm_extract_session memsets its output before parsing,
     * so a bad payload would otherwise wipe session_id). */
    t_start = kn_sock_now_ms();
    while (!sess.ready && !g_knv_stop) {
        kn_cm_session ps;
        int attempt, round_ok = 0;
        if (queue_timeout > 0 &&
            kn_sock_now_ms() - t_start >= (uint64_t)queue_timeout * 1000u) {
            stop_reason = "queue_timeout";
            break;
        }
        if (sess.status == 7) { stop_reason = "terminated"; break; }
        sess_sleep(sess_poll_gap(poll_fails), NULL);
        memset(&ps, 0, sizeof ps);
        cm_json[0] = '\0';   /* the body excerpt below must be THIS round's */
        rc = KN_HTTP_ERR_IO;
        http_status = 0;
        for (attempt = 0; attempt < 3 && !g_knv_stop; ++attempt) {
            memset(&ps, 0, sizeof ps);
            cm_tls = kn_net_tls_stream_open(poll_host, poll_port, 15000,
                                            err, sizeof err);
            if (cm_tls) {
                cm_io_ctx.s = cm_tls;
                rc = kn_cm_poll(hio, poll_host, "/", &st, sess.session_id,
                                20000, sess_now, NULL, cm_rxb,
                                sizeof cm_rxb, cm_json, sizeof cm_json,
                                &http_status, &ps);
                kn_net_tls_stream_close(cm_tls);
                cm_tls = NULL;
            } else {
                rc = KN_HTTP_ERR_IO;
                http_status = 0;
            }
            if (rc == KN_HTTP_OK && http_status == 200 &&
                (ps.req_status_code == 0 || ps.req_status_code == 1)) {
                round_ok = 1;
                break;
            }
            /* 404 = the cloud dropped the session (:664-671); non-
             * transient HTTP statuses are not worth an in-round retry */
            if (rc == KN_HTTP_OK && http_status != 0 &&
                !cm_status_transient(http_status))
                break;
            if (attempt < 2) sess_sleep(attempt == 0 ? 250u : 750u, NULL);
        }
        if (round_ok) {
            poll_fails = 0;
            consec_transport = 0;
            consec_hard = 0;
            sess = ps;
            /* the reference RE-LEARNS the control node from every
             * successful poll (session_info derives streamingBaseUrl from
             * THIS payload's sessionControlInfo :1966-1984; store_active
             * saves it as the next poll's control_base :1333-1336) — a
             * session that migrates nodes mid-queue is followed. The
             * trust gate + keep-on-fallback are the :1984 semantics (a
             * run-8-style ip:null keeps the current host). */
            {
                char nh[256];
                uint16_t np = 443;
                if (kn_cm_control_base(&sess, nh, sizeof nh, &np) &&
                    (np != poll_port || strcmp(nh, poll_host) != 0)) {
                    char esc[300];
                    size_t nl = strlen(nh);
                    memcpy(poll_host, nh, nl);
                    poll_host[nl] = '\0';
                    poll_port = np;
                    lb_json_path(esc, sizeof esc, poll_host);
                    relearns++;
                    printf("{\"mode\":\"session\",\"event\":\"cm_control\","
                           "\"poll_host\":\"%s\",\"port\":%u,"
                           "\"source\":\"relearned\",\"n\":%u}\n", esc,
                           (unsigned)poll_port, relearns);
                    fflush(stdout);
                }
            }
            printf("{\"mode\":\"session\",\"event\":\"queue\",\"status\":%lld,"
                   "\"phase\":\"%s\",\"queue_position\":%lld,\"have_queue\":%d,"
                   "\"seat_step\":%lld,\"ready\":%d}\n",
                   (long long)sess.status, sess.phase,
                   (long long)sess.queue_position, sess.have_queue,
                   (long long)sess.seat_setup_step, sess.ready);
            fflush(stdout);
            continue;
        }
        if (rc == KN_HTTP_OK && http_status == 404) {
            stop_reason = "session_not_found";
            break;
        }
        if (g_knv_stop) { stop_reason = "interrupted"; break; }
        ++poll_fails;
        /* round XXVI failure classes: transport (no HTTP response at all)
         * is bounded at 10; a non-transient HTTP status or a cloud-side
         * statusCode != 1 (hard rejection — 401/403/409/SESSION_LIMIT...)
         * at 5; TRANSIENT statuses (503 storms — run 7) are OUTLIVED
         * until queue_timeout, like the reference UI which keeps polling
         * through any get_session error. */
        if (rc != KN_HTTP_OK) {
            ++consec_transport;
            consec_hard = 0;
        } else if (cm_status_transient(http_status)) {
            consec_transport = 0;       /* outlived — no bound, no counter */
            consec_hard = 0;
        } else {
            ++consec_hard;
            consec_transport = 0;
        }
        {   /* a failed round is EVIDENCE, not silence: rc (transport),
             * the raw HTTP status, the cloud-side requestStatus (the
             * extract fills it even when the session body is rejected),
             * the deep session fields of a JSON error body (run 8's 503
             * carried a session SKELETON — sessionControlInfo.ip:null —
             * whose status/errorCode sit PAST any bounded excerpt; the
             * reference's cloudmatch_http_error likewise reads the
             * payload, :2377), a body excerpt for unexpected shapes, the
             * failure class and the next poll gap */
            char esc[96], escb[240];
            long long sess_status = 0, err_code = 0, unified = 0;
            const char *cls = rc != KN_HTTP_OK ? "transport"
                            : cm_status_transient(http_status) ? "transient"
                            : "hard";
            if (rc == KN_HTTP_OK && cm_json[0] == '{') {
                kn_cm_session dg;
                memset(&dg, 0, sizeof dg);
                (void)kn_cm_extract_session(cm_json, &dg);
                sess_status = dg.status;
                err_code = dg.error_code;
                unified = dg.unified_error;
                if (ps.req_status_code == 0)
                    ps.req_status_code = dg.req_status_code;
                if (!ps.req_status_desc[0] && dg.req_status_desc[0])
                    snprintf(ps.req_status_desc, sizeof ps.req_status_desc,
                             "%.63s", dg.req_status_desc);
            }
            lb_json_path(esc, sizeof esc, ps.req_status_desc);
            lb_json_path(escb, sizeof escb, cm_json);
            printf("{\"mode\":\"session\",\"event\":\"cm_poll_error\","
                   "\"rc\":%d,\"http_status\":%d,\"cloud_status\":%lld,"
                   "\"unified_error\":%lld,\"sess_status\":%lld,"
                   "\"err_code\":%lld,\"desc\":\"%s\",\"body\":\"%s\","
                   "\"consecutive\":%u,\"class\":\"%s\",\"gap_ms\":%u}\n",
                   rc, http_status, (long long)ps.req_status_code,
                   unified, sess_status, err_code, esc, escb,
                   poll_fails, cls, (unsigned)sess_poll_gap(poll_fails));
            fflush(stdout);
        }
        if (consec_transport >= 10) {
            stop_reason = "cm_poll_failed";
            break;
        }
        if (consec_hard >= 5) {
            stop_reason = "cm_poll_rejected";
            break;
        }
    }
    if (!sess.ready) {
        printf("{\"mode\":\"session\",\"error\":\"not_ready\","
               "\"stop_reason\":\"%s\",\"status\":%lld,\"rc\":%d,"
               "\"http_status\":%d}\n", stop_reason, (long long)sess.status,
               rc, http_status);
        fflush(stdout);
        /* Best-effort cancel (the reference's stop_at :789-800 — DELETE
         * v2/session/{id}): an abandoned queue slot orphans a live
         * session, and the next create can hit SESSION_LIMIT (:2354+;
         * R7 runs 1-4 each left one behind — no cancel existed then).
         * Skipped for session_not_found (404 = already gone). One
         * attempt, no retry: cleanup, not the mission. */
        if (sess.session_id[0] && keep_on_abort &&
            strcmp(stop_reason, "session_not_found") != 0) {
            /* --session-keep: leave the queued session alive on the
             * server (the user's run-6 browser-resume workflow / R23
             * experiments). Honest evidence line instead of the DELETE. */
            printf("{\"mode\":\"session\",\"event\":\"cm_cancel\","
                   "\"skipped\":\"session-keep\"}\n");
            fflush(stdout);
        } else if (sess.session_id[0] &&
            strcmp(stop_reason, "session_not_found") != 0) {
            int c_rc = KN_HTTP_ERR_IO, c_status = 0;
            cm_tls = kn_net_tls_stream_open(poll_host, poll_port, 15000,
                                            err, sizeof err);
            if (cm_tls) {
                cm_io_ctx.s = cm_tls;
                c_rc = kn_cm_cancel(hio, poll_host, "/", &st,
                                    sess.session_id, 20000, sess_now, NULL,
                                    cm_rxb, sizeof cm_rxb, cm_json,
                                    sizeof cm_json, &c_status);
                kn_net_tls_stream_close(cm_tls);
                cm_tls = NULL;
            }
            printf("{\"mode\":\"session\",\"event\":\"cm_cancel\",\"rc\":%d,"
                   "\"http_status\":%d}\n", c_rc, c_status);
            fflush(stdout);
        }
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
               "\"media_peer\":\"%.64s:%u\",\"relearns\":%u}\n", esc,
               sess.media_ip, (unsigned)sess.media_port, relearns);
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
    {   /* Host header = the target minus the scheme, INCLUDING :port —
         * nvst_rtsp.rs:1070-1076 strips only "rtsps://"; run 5 sent the
         * bare host. The endpoint prefix is verified by parse_rtsps. */
        const char *a = sess.rtsps_endpoint + 8;
        size_t al = strcspn(a, "/?#");
        if (al == 0 || al >= sizeof authbuf) {
            printf("{\"mode\":\"session\",\"error\":\"bad_rtsps_endpoint\","
                   "\"note\":\"authority\"}\n");
            fflush(stdout);
            kn_log_shutdown();
            return 1;
        }
        memcpy(authbuf, a, al);
        authbuf[al] = '\0';
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
    sess_ctrl_ping_init(&ctrl_ping, &wsc);   /* armed after PLAY */

    rio.send_text = sess_ws_send_text;
    rio.recv_text = sess_ws_recv_text;
    rio.ctx = &wsc;
    (void)kn_rtsp_client_init(&rtc, rio, rtsp_buf, sizeof rtsp_buf,
                              KN_RTSP_TIMEOUT_MS, sess_now, NULL,
                              sess_sleep, NULL);

    common[0].name = "X-GS-Version";   common[0].value = "14.2";
    common[1].name = "Host";           common[1].value = authbuf;
    common[2].name = "x-nv-sessionid"; common[2].value = sess.session_id;

    rc = kn_rtsp_request(&rtc, "OPTIONS", sess.rtsps_endpoint, common, 3,
                         NULL, 0, 0, &resp);
    printf("{\"mode\":\"session\",\"event\":\"rtsp\",\"step\":\"OPTIONS\","
           "\"rc\":%d,\"status\":%d}\n", rc, rc == KN_RTSP_OK ? resp.status : 0);
    fflush(stdout);
    if (rc != KN_RTSP_OK || resp.status != 200) {
        sess_rtsp_detail("OPTIONS", &rtc, &wsc,
                                     rc == KN_RTSP_OK ? &resp : NULL);
        goto teardown;
    }
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
    if (rc != KN_RTSP_OK || resp.status != 200) {
        sess_rtsp_detail("DESCRIBE", &rtc, &wsc,
                                     rc == KN_RTSP_OK ? &resp : NULL);
        goto teardown;
    }
    {   /* round XXXII evidence: the rig's Server header (RTSP.md §16
         * open question 5 — do the 455/timeout/400 refusal modes
         * correlate with rig software versions? every run now says) */
        int h;
        for (h = 0; h < resp.hdr_count; ++h) {
            if (resp.names[h][0] == 'S' || resp.names[h][0] == 's') {
                if ((resp.names[h][1] == 'e' || resp.names[h][1] == 'E') &&
                    (resp.names[h][2] == 'r' || resp.names[h][2] == 'R')) {
                    char sesc[192];
                    lb_json_path(sesc, sizeof sesc, resp.values[h]);
                    printf("{\"mode\":\"session\",\"event\":\"rig_info\","
                           "\"server\":\"%s\"}\n", sesc);
                    fflush(stdout);
                    break;
                }
            }
        }
    }
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
        /* R3-B: reserve the reference's socket PAIR before SETUP so the
         * announced X-GS-ClientPort range stays truthful — video
         * (Mjolnir) on N, the ICE/DTLS bundle on N+1 (:5484: "the server
         * still relies on this pairing ... Reserving the bundle first
         * reverses those ports and causes some seats to keep all video
         * off the client entirely"). Preferred 49005/49006 first (the
         * official useReserved pair, :5941-5944: some seats do not route
         * Mjolnir video to an arbitrary source port), then <= 32 dynamic
         * adjacent attempts (:5953-6000), then an honest failure. */
        err[0] = '\0';
        rc = sess_reserve_pair(&sock, &bsock, &pair_sel, err, sizeof err);
        if (rc != KN_SOCK_OK) {
            printf("{\"mode\":\"session\",\"error\":\"media_socket\","
                   "\"detail\":\"%.120s\"}\n", err);
            fflush(stdout);
            goto teardown;
        }
        have_sock = have_bsock = 1;
        printf("{\"mode\":\"session\",\"event\":\"sock_pair\","
               "\"video_port\":%u,\"bundle_port\":%u,\"selection\":\"%s\","
               "\"unified\":%d,\"rcvbuf_video\":%u,\"rcvbuf_bundle\":%u}\n",
               (unsigned)kn_sock_udp_port(&sock),
               (unsigned)kn_sock_udp_port(&bsock), pair_sel,
               desc.unified_socket ? 1 : 0, sock.rcvbuf_actual,
               bsock.rcvbuf_actual);
        fflush(stdout);
        /* SETUP's X-GS-ClientPort = the BUNDLE port when the seat
         * described unified-socket mode, the VIDEO port otherwise
         * (nvst_rtsp.rs:1176-1183) */
        rc = kn_rtsp_setup_video(&rtc, desc.video_control,
                                 sess.rtsps_endpoint, desc.session,
                                 desc.ping_version,
                                 desc.unified_socket
                                     ? kn_sock_udp_port(&bsock)
                                     : kn_sock_udp_port(&sock),
                                 common, 3,
                                 sess.have_media ? sess.media_ip : NULL,
                                 sess.have_media ? sess.media_port : 0,
                                 &setup);
        printf("{\"mode\":\"session\",\"event\":\"rtsp\",\"step\":\"SETUP\","
               "\"rc\":%d,\"status\":%d,\"peer\":\"%.64s:%u\",\"bundle_fb\":%d,"
               "\"rounds\":%u}\n", rc, setup.status, setup.peer_ip,
               (unsigned)setup.peer_port, setup.bundle_fallback,
               setup.rounds_slept);
        fflush(stdout);
        if (rc != KN_RTSP_OK) {
            sess_rtsp_detail("SETUP", &rtc, &wsc,
                                     rc == KN_RTSP_OK ? &resp : NULL);
            goto teardown_sock;
        }

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

        /* R3-B: the video peer parse stays the hard gate (IPv6 peers are
         * stage 5), and the BUNDLE socket is connected to the SETUP peer
         * BEFORE announcing: the kernel picks the route's source address
         * and getsockname (inside kn_sock_udp_connect) exposes it — the
         * ANNOUNCE's clientPorts.localAddress + a=candidate then carry
         * the REAL routed address of the bundle socket, exactly like the
         * reference's advertised_local_address_for(bundle_peer)
         * (nvst.rs:6119-6127). The bundle socket later re-connects to
         * ice_dest when the CloudMatch media peer differs; the VIDEO
         * socket is connected to its peer at media-loop start. */
        {
            uint32_t peer_ip = 0;
            if (kn_sock_ipv4_parse(setup.peer_ip, &peer_ip) != 0) {
                printf("{\"mode\":\"session\",\"error\":\"peer_not_ipv4\","
                       "\"note\":\"IPv6 peers need kn_sock v6 (stage 5)\"}\n");
                fflush(stdout);
                goto teardown_sock;
            }
            if (kn_sock_udp_connect(&bsock, peer_ip, setup.peer_port) !=
                KN_SOCK_OK) {
                printf("{\"mode\":\"session\",\"error\":\"media_connect\"}\n");
                fflush(stdout);
                goto teardown_sock;
            }
        }
        unified_mode = desc.unified_socket || setup.bundle_fallback;

        sess_local_identity(ufrag, sizeof ufrag, pwd, sizeof pwd,
                            fp, sizeof fp);
        {   /* round XXX (R3-A): announce a REAL certificate fingerprint
             * when the build carries the mbedTLS backend (docs/DTLS.md —
             * the rig pins this fingerprint; the R3-B DTLS handshake will
             * present the matching certificate). Without it, the D-s1
             * placeholder stays and the evidence line SAYS so. The cert
             * is freed right after fingerprinting in R3-A; R3-B keeps it
             * alive for the handshake. Lengths/shapes only in the log
             * (§7) — the fingerprint itself is public (it travels in the
             * ANNOUNCE). */
            const char *cert_src = "placeholder";
            if (kn_cert_available()) {
                cert = kn_cert_generate(err, sizeof err);
                if (cert) {
                    if (kn_cert_fingerprint(cert, fp, sizeof fp) == 0)
                        cert_src = "real";
                    else {
                        kn_cert_free(cert);   /* unusable: back to the
                                                 honest placeholder */
                        cert = NULL;
                    }
                }
            }
            /* R3-B: the cert stays ALIVE now — the kn_dtls handshake
             * below presents exactly the certificate whose fingerprint
             * the ANNOUNCE carries (kn_cert_pk/kn_cert_x509 handles). */
            printf("{\"mode\":\"session\",\"event\":\"identity\","
                   "\"ufrag_len\":%d,\"pwd_len\":%d,\"fp_len\":%d,"
                   "\"cert\":\"%s\"}\n", (int)strlen(ufrag),
                   (int)strlen(pwd), (int)strlen(fp), cert_src);
            fflush(stdout);
        }
        memset(&ap, 0, sizeof ap);
        ap.width = 1280; ap.height = 720; ap.fps = 30;         /* D14 */
        ap.bitrate_kbps = 5000;          /* stage-5 will cap 3-6 Mbps */
        ap.bitstream_format = 0;         /* H264 (D3/D6 decode path) */
        ap.bit_depth = 0; ap.chroma_format = 1;
        ap.hdr = 0;
        snprintf(ap.key_hex, sizeof ap.key_hex, "%s", key_hex);
        ap.key_id = key_id;
        ap.bundle_port = kn_sock_udp_port(&bsock);   /* clientBundlePort +
                                                        a=candidate = the
                                                        BUNDLE socket (the
                                                        reference test pins
                                                        clientBundlePort:
                                                        49006, :2955) */
        {
            char iptext[16];
            if (bsock.local_addr == 0)
                snprintf(iptext, sizeof iptext, "127.0.0.1");
            else
                (void)kn_sock_ipv4_text(bsock.local_addr, iptext);
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
        if (rc != KN_RTSP_OK || resp.status != 200) {
            sess_rtsp_detail("ANNOUNCE", &rtc, &wsc,
                                     rc == KN_RTSP_OK ? &resp : NULL);
            goto teardown_sock;
        }
        /* ---- round XXV: pre-PLAY ICE burst (D-s2) ----
         * Credentials + destination exactly per the reference's handoff
         * (nvst_rtsp.rs:1244-1276, nvst.rs:1729-1746): the credential
         * gate is pingVersion==6 or a remote DTLS fingerprint; ping
         * version 6 WITHOUT a SETUP x-nv-ping-payload is a hard
         * missing-ice-credentials error there (:1248-1253) — deviation
         * D-s3: we skip the pump honestly and still try PLAY (bring-up
         * maximizes the measurement). Destination = the CloudMatch
         * bundle peer else the SETUP video peer (bundle_peer() :1880).
         * Evidence line carries LENGTHS only, never values (§7). */
        memset(&ice_st, 0, sizeof ice_st);
        {
            char bundle_user[288], dtxt[24];
            int have_bu = kn_rtsp_bundle_natt_username(&desc, bundle_user,
                                                       sizeof bundle_user);
            const char *payload = setup.have_ping_payload
                                      ? setup.ping_payload : NULL;
            const char *dest_text = sess.have_media ? sess.media_ip
                                                    : setup.peer_ip;
            uint16_t dest_port = sess.have_media ? sess.media_port
                                                 : setup.peer_port;
            int creds_ok = (setup.ping_version == 6 || desc.remote_fp[0]);
            if (setup.ping_version == 6 && !payload)
                creds_ok = 0;              /* :1248-1253 (D-s3: skip) */
            if (creds_ok && dest_port &&
                kn_sock_ipv4_parse(dest_text, &ice_dest_ip) == 0 &&
                desc.remote_password[0] &&
                kn_rtsp_resolve_remote_ufrag(payload, &desc,
                                             have_bu ? bundle_user : NULL,
                                             setup.ping_version,
                                             ice_remote_ufrag,
                                             sizeof ice_remote_ufrag)) {
                ice_dest_port = dest_port;
                snprintf(ice_natt, sizeof ice_natt, "%s",
                         have_bu ? bundle_user : "PING");  /* :7398-7400 */
                ice_enabled = 1;
            }
            printf("{\"mode\":\"session\",\"event\":\"ice_creds\","
                   "\"enabled\":%d,\"ping_ver\":%u,\"dest\":\"%.15s:%u\","
                   "\"remote_ufrag_len\":%d,\"bundle_user_len\":%d,"
                   "\"natt_len\":%d,\"setup_payload_len\":%d,"
                   "\"remote_pw\":%d,\"remote_fp\":%d,\"unified\":%d}\n",
                   ice_enabled, (unsigned)setup.ping_version,
                   ice_enabled ? kn_sock_ipv4_text(ice_dest_ip, dtxt) : "",
                   (unsigned)ice_dest_port,
                   ice_enabled ? (int)strlen(ice_remote_ufrag) : 0,
                   have_bu ? (int)strlen(bundle_user) : 0,
                   ice_enabled ? (int)strlen(ice_natt) : 0,
                   setup.have_ping_payload
                       ? (int)strlen(setup.ping_payload) : 0,
                   desc.remote_password[0] ? 1 : 0,
                   desc.remote_fp[0] ? 1 : 0,
                   unified_mode ? 1 : 0);
            fflush(stdout);
        }
        /* R3-B socket routing (the reference's split, nvst_rtsp.rs
         * :1205-1222): the BUNDLE socket's peer for life is the bundle
         * peer (ice_dest — the CloudMatch media endpoint else the SETUP
         * peer); the VIDEO socket (non-unified) is connected to the
         * SETUP video peer at media-loop start. In unified mode the
         * bundle socket ALSO carries the media flow (the video socket
         * stays reserved-but-idle, exactly like the reference's pair).
         * The old save/restore reconnect dance is GONE: each socket has
         * exactly one peer for life now. */
        if (ice_enabled &&
            (bsock.peer_addr != ice_dest_ip ||
             bsock.peer_port != ice_dest_port))
            (void)kn_sock_udp_connect(&bsock, ice_dest_ip, ice_dest_port);
        /* ---- R3-B: arm the DTLS client (the decisive round) ----
         * The handshake itself starts inside the pump thread, gated on
         * the first REAL inbound STUN (the :8196 rule — never
         * synthesized). Requirements: working ICE (dest + creds), the
         * DESCRIBE remote fingerprint to pin, and the real kn_cert
         * identity (a placeholder fingerprint has no keypair behind it —
         * announcing one and handshaking another would be a lie). */
        {
            const char *no_arm = NULL;
            char arm_detail[160];
            arm_detail[0] = '\0';
            if (!ice_enabled)          no_arm = "ice_disabled";
            else if (!desc.remote_fp[0]) no_arm = "no_remote_fp";
            else if (!cert)            no_arm = "no_real_cert";
            else if (!kn_dtls_available()) no_arm = "backend_unavailable";
            if (!no_arm) {
                err[0] = '\0';
                dtls = kn_dtls_client(&bsock, ice_dest_ip, ice_dest_port,
                                      cert, desc.remote_fp, err, sizeof err);
                if (!dtls) {
                    no_arm = "client_init";
                    lb_json_path(arm_detail, sizeof arm_detail, err);
                }
            }
            printf("{\"mode\":\"session\",\"event\":\"dtls_arm\","
                   "\"armed\":%d,\"reason\":\"%s\",\"detail\":\"%s\","
                   "\"pin_len\":%d,\"cert_real\":%d,\"available\":%d}\n",
                   dtls ? 1 : 0, no_arm ? no_arm : "", arm_detail,
                   (int)strlen(desc.remote_fp), cert ? 1 : 0,
                   kn_dtls_available());
            fflush(stdout);
        }
        /* Round XXVIII: the pump runs CONCURRENTLY with the PLAY
         * exchange (the reference's receiver thread never goes quiet
         * across announce→PLAY→response — run 9 showed the sequential
         * 400 ms-then-silence shape still drew 455 with a fully alive
         * ICE exchange) and PLAY is ONE-SHOT: run 9 also proved a
         * re-PLAY on the 455'd WS hits a dead channel (rc -2,
         * ws_close -1 — the rig drops the RTSP context after the
         * refusal). A failed PLAY goes to TEARDOWN as before; the
         * session stays resumable (phase B-E policy). */
        memset(&pump, 0, sizeof pump);
        sess_fb_init(&fb);
        /* round XXXVII: the control/input plane's initial state. The
         * transport origin feeds the activation chain's timestamps
         * (nvst.rs:6564-6567 uses transport_origin.elapsed()), and the
         * version starts "not negotiated" exactly like the reference's
         * Option<u16>::None. */
        pump.input_version = -1;
        pump.st.input_version = -1;
        /* round XXXVIII: the server-control decoder's initial state. The
         * frame-pacing latch starts ENABLED (nvst.rs:664
         * AtomicBool::new(true)) — 0x112 mode 1 keeps it, anything else
         * clears it. Diagnostics start at zero and SATURATE. */
        kn_sctl_diag_init(&pump.sctl_diag);
        pump.sctl_pacing_enabled = KN_SCTL_PACING_DEFAULT_ENABLED;
        pump.st.pacing_enabled = KN_SCTL_PACING_DEFAULT_ENABLED;
        pump.sctl_term_resumable = -1;
        pump.st.term_resumable = -1;
        pump.origin_us = kn_sock_now_us();
        pump.next_ctrl_log_ms = kn_sock_now_ms() + 10000u;
        kn_input_cursor_init(&pump.cursor);
        kn_input_hist_init(&pump.ctrl_hist);
        pump_set_str(pump.st.ctrl_hist, sizeof pump.st.ctrl_hist, "[]");
        if (ice_enabled) {
            pump.fb = &fb;            /* the feedback drain (XXXVI) */
            pump.sock = &bsock;       /* R3-B: the pump owns the BUNDLE
                                       * socket (ICE + DTLS live there);
                                       * the video socket is untouched
                                       * until the media loop */
            pump.dest_ip = ice_dest_ip;
            pump.dest_port = ice_dest_port;
            pump.dtls = dtls;         /* NULL = STUN/NATT-only (honest) */
            pump_set_str(pump.remote_ufrag, sizeof pump.remote_ufrag,
                         ice_remote_ufrag);
            pump_set_str(pump.remote_pwd, sizeof pump.remote_pwd,
                         desc.remote_password);
            pump_set_str(pump.local_ufrag, sizeof pump.local_ufrag, ufrag);
            pump_set_str(pump.local_pwd, sizeof pump.local_pwd, pwd);
            pump_set_str(pump.natt, sizeof pump.natt, ice_natt);
            if (dtls) pump.st.dtls_armed = 1;
            if (!unified_mode) {
                /* the reference arms the Mjolnir worker BEFORE PLAY
                 * (lib.rs:990) — its NATT keepalives from port N route
                 * the video (:8849-8905). Run 13 left the announced
                 * video port silent pre-PLAY; the pump thread now
                 * send-only pings it at 20 ms until the media loop
                 * takes over (vid_handoff). */
                uint32_t vip = 0;
                if (kn_sock_ipv4_parse(setup.peer_ip, &vip) == 0 &&
                    setup.peer_port) {
                    pump.vid_sock = &sock;
                    pump.vid_ip = vip;
                    pump.vid_port = setup.peer_port;
                    pump_set_str(pump.vid_payload, sizeof pump.vid_payload,
                                 setup.have_ping_payload ? setup.ping_payload
                                                         : "PING");
                }
            }
        }
        /* R3-B: in the NON-unified layout the pump thread becomes the
         * reference's bundle worker for the WHOLE session (it keeps
         * answering STUN, NATT-pinging at the post-ready 100 ms and
         * servicing the DTLS session while the media loop runs on the
         * separate video socket — single-owner discipline holds, D21's
         * media loop untouched). Unified mode keeps the round-XXVIII
         * handoff shape: one socket, one owner, join before media
         * (deviation D-t2 — bundle servicing there is R3-C scope). */
        if (!desc.disable_play) {
            play_attempts = 1;
            if (ice_enabled) {
                pump_th = (sess_pump_start(&pump) == 0);
                /* persist ONLY with a live thread AND the split-socket
                 * layout (unified mode hands the single socket to the
                 * media loop — join first, D-t2) */
                pump_persist = pump_th && !unified_mode;
                if (!pump_th)      /* honest sequential fallback */
                    sess_ice_pump(&bsock, ice_dest_ip, ice_dest_port,
                                  ice_remote_ufrag, desc.remote_password,
                                  ufrag, pwd, ice_natt, pkt, sizeof pkt,
                                  400u, &pump.st);
                else {
                    /* round XXIX warm-up: guarantee PLAY never lands
                     * before the first bursts; round XXXIII: then WAIT
                     * (bounded) for the DTLS handshake AND the SCTP
                     * association before PLAY — runs 13/14 sent PLAY
                     * ~12 ms after dtls_ready with zero SCTP and drew
                     * the rig's explicit 400. The reference's finish()
                     * lands only after its engine plumbing gave the
                     * receivers a head start (lib.rs:937 -> :1117),
                     * with start_sctp(true) firing AT dtls_ready
                     * (:8221). Bounded on purpose: a dead DTLS/SCTP
                     * must never eat the PLAY budget — PLAY is still
                     * attempted (deviation D-t5, both knobs tunable). */
                    sess_ice_stats gsnap;
                    uint64_t t_wait = kn_sock_now_ms();
                    sess_sleep(100u, NULL);      /* first-burst head start */
                    for (;;) {
                        sess_pump_snapshot(&pump, &gsnap);
                        if (gsnap.sctp_up || gsnap.dtls_fail_rc ||
                            gsnap.sctp_fail || g_knv_stop)
                            break;
                        if (play_wait_ms >= 0 &&
                            kn_sock_now_ms() - t_wait >=
                                (uint64_t)play_wait_ms)
                            break;
                        sess_sleep(25u, NULL);
                    }
                    if (gsnap.sctp_up && play_settle_ms > 0)
                        sess_sleep((uint32_t)play_settle_ms, NULL);
                }
            }
            rc = kn_rtsp_request(&rtc, "PLAY", sess.rtsps_endpoint,
                                 ah, 4, NULL, 0, 0, &resp);
            printf("{\"mode\":\"session\",\"event\":\"rtsp\","
                   "\"step\":\"PLAY\",\"rc\":%d,\"status\":%d,"
                   "\"attempt\":%d}\n", rc,
                   rc == KN_RTSP_OK ? resp.status : 0, play_attempts);
            fflush(stdout);
        } else if (ice_enabled) {
            /* a disablePlay seat: the reference's receiver still pings —
             * a short sequential window for routing/evidence (no DTLS on
             * the sequential path; a disablePlay seat never reached a
             * live run yet) */
            sess_ice_pump(&bsock, ice_dest_ip, ice_dest_port,
                          ice_remote_ufrag, desc.remote_password,
                          ufrag, pwd, ice_natt, pkt, sizeof pkt,
                          400u, &pump.st);
        }
        if (pump_persist) {
            /* the worker keeps running through the media loop — read
             * the PLAY-window stats through the snapshot handshake
             * (never touch pump.st directly while the thread lives) */
            sess_pump_snapshot(&pump, &ice_st);
        } else {
            sess_pump_stop(&pump);    /* join BEFORE any socket handoff */
            if (ice_enabled) ice_st = pump.st;
        }
        if (ice_enabled) {
            char app_hex[4 * 64 + 1];   /* x4: -Wformat-truncation=2
                                         * cannot bound hex digits at
                                         * 16 (they are); the slack
                                         * silences the over-approx */
            char sctp_hex[4 * 16 + 1];
            char post_hex[4 * 32 + 1];
            {   /* the first app record's captured head, as hex (the
                 * rig's post-DTLS message — R3-C's design input) */
                static const char hx[] = "0123456789abcdef";
                unsigned hb, hn = ice_st.dtls_first_app_len;
                if (hn > 64) hn = 64;
                for (hb = 0; hb < hn; ++hb) {
                    app_hex[2 * hb]     = hx[ice_st.dtls_first_app[hb] >> 4];
                    app_hex[2 * hb + 1] = hx[ice_st.dtls_first_app[hb] & 0xFu];
                }
                app_hex[2 * hn] = '\0';
                hn = ice_st.sctp_first_len;
                if (hn > 16) hn = 16;
                for (hb = 0; hb < hn; ++hb) {
                    sctp_hex[2 * hb]     = hx[ice_st.sctp_first[hb] >> 4];
                    sctp_hex[2 * hb + 1] = hx[ice_st.sctp_first[hb] & 0xFu];
                }
                sctp_hex[2 * hn] = '\0';
                hn = ice_st.dtls_post_first_len;
                if (hn > 32) hn = 32;
                for (hb = 0; hb < hn; ++hb) {
                    post_hex[2 * hb]     = hx[ice_st.dtls_post_first[hb] >> 4];
                    post_hex[2 * hb + 1] = hx[ice_st.dtls_post_first[hb] & 0xFu];
                }
                post_hex[2 * hn] = '\0';
            }
            printf("{\"mode\":\"session\",\"event\":\"ice_preplay\","
                   "\"stun_sent\":%u,\"natt_sent\":%u,\"req_in\":%u,"
                   "\"answered\":%u,\"ok_in\":%u,\"invalid_in\":%u,"
                   "\"send_errors\":%u,\"pongs\":%u,\"pump_ms\":%llu,"
                   "\"play_attempts\":%d,\"thread\":%d,\"persist\":%d,"
                   "\"dtls_armed\":%u,\"dtls_started\":%u,"
                   "\"dtls_ready\":%u,\"dtls_ms\":%llu,\"dtls_in\":%u,"
                   "\"dtls_out\":%u,\"dtls_fail_rc\":%u,\"vid_ka\":%u,"
                   "\"app_records\":%u,\"app_bytes\":%u,"
                   "\"app_first_len\":%u,\"app_first\":\"%s\","
                   "\"sctp_armed\":%u,\"sctp_up\":%u,\"sctp_ms\":%llu,"
                   "\"sctp_opened\":%u,\"sctp_acked\":%u,"
                   "\"sctp_rig_opened\":%u,\"sctp_msgs_in\":%u,"
                   "\"sctp_fail\":%u,\"sctp_first_sid\":%u,"
                   "\"sctp_first_ppi\":%u,\"sctp_first_len\":%u,"
                   "\"sctp_first\":\"%s\",\"nom_sent\":%u,"
                   "\"sctp_out\":%u,\"sctp_in\":%u,"
                   "\"peer_closed\":%u,\"post_first_len\":%u,"
                   "\"post_first\":\"%s\"}\n",
                   ice_st.stun_sent, ice_st.natt_sent, ice_st.req_in,
                   ice_st.answered, ice_st.ok_in, ice_st.invalid_in,
                   ice_st.send_errors, ice_st.pongs_sent,
                   (unsigned long long)ice_st.pump_ms, play_attempts,
                   pump_th, pump_persist,
                   ice_st.dtls_armed, ice_st.dtls_started,
                   ice_st.dtls_ready,
                   (unsigned long long)ice_st.dtls_ready_ms,
                   ice_st.dtls_in, ice_st.dtls_out, ice_st.dtls_fail_rc,
                   ice_st.vid_ka_sent, ice_st.dtls_app_records,
                   ice_st.dtls_app_bytes, ice_st.dtls_first_app_len,
                   app_hex,
                   ice_st.sctp_armed, ice_st.sctp_up,
                   (unsigned long long)ice_st.sctp_up_ms,
                   ice_st.sctp_opened, ice_st.sctp_acked,
                   ice_st.sctp_rig_opened, ice_st.sctp_msgs_in,
                   ice_st.sctp_fail, ice_st.sctp_first_sid,
                   ice_st.sctp_first_ppi, ice_st.sctp_first_len,
                   sctp_hex, ice_st.nom_sent, ice_st.sctp_out,
                   ice_st.sctp_in, ice_st.dtls_peer_closed,
                   ice_st.dtls_post_first_len, post_hex);
            fflush(stdout);
        }
        if (!desc.disable_play && (rc != KN_RTSP_OK || resp.status != 200)) {
            sess_rtsp_detail("PLAY", &rtc, &wsc,
                                     rc == KN_RTSP_OK ? &resp : NULL);
            goto teardown_sock;
        }
        /* R3-D: the control keepalive arms the moment PLAY lands —
         * exactly when the reference spawns its rtsps worker
         * (nvst_rtsp.rs:840-846). Main owns the WSS from here to
         * TEARDOWN; the media loop services it (1 ms bounded drain
         * per pass), so single-owner discipline is untouched. */
        ctrl_ping.have = 1;
        ctrl_ping.next_ping_ms = kn_sock_now_ms();

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
            /* D-t10: the bandwidth estimator's monotonic ns clock. Only
             * the LIVE session sets it — --decode and the KAT runner stay
             * on the deterministic ms fallback so replays reproduce. */
            cfg.now_ns = sess_bw_now_ns;
            cfg.now_ns_ud = NULL;
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
        /* round XXXVI: the feedback sender arms with the pipeline (the
         * cadence anchors at 0 fire on the first media tick) */
        c.fb = &fb;
        sess_cb_init(&c.cbatch);      /* round XXXVIII: the report batch */
        c.pacing_enabled = KN_SCTL_PACING_DEFAULT_ENABLED;
        c.next_rr_ms = 0;
        c.next_qos_ms = 0;
        c.next_ka_ms = 0;
        c.next_pacing_us = 0;
        c.qos_prior_ms = 0;
        c.qos_prior_bytes = 0;
        c.rr_prior_expected = 0;
        c.rr_prior_received = 0;
        /* round XXXVIII: the 0x0207 plane's origin. `elapsed` in the
         * reference is `now - transport_origin` (nvst.rs:7952), and
         * transport_origin is taken when the transport work starts
         * (:6197) — pump.origin_us is our same instant (set above, and
         * the activation chain already timestamps against it). */
        c.transport_origin_ms = pump.origin_us / 1000u;
        c.have_qos_prior = 0;
        c.completed_frame_bytes = 0;
        c.last_sender_frame = 0;
        c.decode_available = kn_decode_available();
        if (c.decode_available) {
            char derr[128];
            c.stream = kn_decode_stream_open(1, derr, sizeof derr);
            if (!c.stream) c.decode_available = 0;
        }
        ring_ok = (kn_ring_init(&ring, 64u * 1024u, 1312) == 0);

        /* ---------------- phase E: the media loop (run_decode shape) --- */
        /* Round XXVIII: the Mjolnir NATT keepalive (nvst.rs:8849-8905 —
         * USERNAME "{setup x-nv-ping-payload}:{local ufrag}", key = the
         * DESCRIBE remote password, to video_peer, every 20 ms until the
         * first inbound then every 100 ms; the reference's comment: the
         * NATT keepalive pings are WHAT ROUTES VIDEO HERE). Sent inline
         * from the loop — D21's single-threaded media loop is untouched;
         * the reference runs it on its mjolnir receiver thread. */
        /* R3-B media routing: unified mode collapses video onto the
         * bundle socket (the reference: video_peer = bundle_peer,
         * nvst_rtsp.rs:1207-1214, and the bundle worker feeds
         * process_datagram itself, :8616-8640); non-unified runs the
         * media loop on the dedicated Mjolnir video socket, connected
         * here to the SETUP video peer (the kernel filter keeps stray
         * traffic out; the keepalive sendto below rides the same
         * socket — "the NATT keepalive pings are WHAT ROUTES VIDEO
         * HERE", :8849-8905). */
        ms = unified_mode ? &bsock : &sock;
        pump.vid_handoff = 1;       /* the thread stops pinging the video
                                     * socket; the media loop's own ka
                                     * takes over (a duplicate ping or
                                     * two during the overlap is
                                     * harmless) */
        if (!unified_mode) {
            if (kn_sock_ipv4_parse(setup.peer_ip, &vid_peer_ip) == 0) {
                vid_peer_port = setup.peer_port;
                if (kn_sock_udp_connect(&sock, vid_peer_ip, vid_peer_port)
                    != KN_SOCK_OK)
                    printf("{\"mode\":\"session\",\"event\":\"note\","
                           "\"note\":\"video_socket_unconnected\"}\n");
            }
        } else if (ice_enabled) {
            vid_peer_ip = ice_dest_ip;
            vid_peer_port = ice_dest_port;
        } else {
            if (kn_sock_ipv4_parse(setup.peer_ip, &vid_peer_ip) == 0)
                vid_peer_port = setup.peer_port;
        }
        fflush(stdout);
        ka_on = desc.remote_password[0] &&
                (setup.ping_version == 6 || desc.remote_fp[0]);
        ka_payload = setup.have_ping_payload ? setup.ping_payload : "PING";
        ka_ip = vid_peer_ip;
        ka_port = vid_peer_port;
        if (!ka_ip || !ka_port)
            ka_on = 0;
        next_ka_ms = 0;
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
            rc = kn_sock_udp_recv(ms, pkt, sizeof pkt,
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
                           ms->last_error);
                    fflush(stdout);
                }
                if (consecutive_errors >= 100u) {
                    stop_reason = "socket_error";
                    break;
                }
            }
            now_ms = kn_sock_now_ms();
            /* Round XXXVIII: the server-control plane reaches the media
             * loop through two volatile latches the pump thread owns.
             * A 0x109 termination ENDS the session — the reference's
             * receive_server_control returns true and its caller does
             * `input_ready.store(false); break 'bundle` (nvst.rs:707-713
             * + :8265-8271) rather than keep reading media from a server
             * that has already said the session is over. A 0x112 for
             * stream 0 gates the 0x0203 pacing reports (:7923-7925).
             * Both are single-word volatile reads: no lock, no tearing,
             * and the pump thread is the only writer. */
            if (ice_enabled) {
                if (pump.sctl_termination) {
                    stop_reason = "server_termination";
                    break;
                }
                c.pacing_enabled = (int)pump.sctl_pacing_enabled;
            }
            /* R3-D: the WSS control keepalive (nvst_rtsp.rs:844-935).
             * Serviced here rather than in a worker thread: the pump
             * thread owns the bundle socket, not the WSS, and the
             * media loop is the only other long-lived context. The
             * 50 ms service beat bounds the 1 ms recv drain to one
             * pass in 50 (~0.02 ms/ms of loop time); the reference
             * worker reads every 100 ms, so ping/pong timing (2 s
             * interval, ms-resolution RTT) is unaffected. Runs in
             * BOTH ice and plain modes. */
            if (ctrl_ping.have &&
                now_ms - ctrl_ping_next_ms >= 50u) {
                ctrl_ping_next_ms = now_ms;
                sess_ctrl_ping_service(&ctrl_ping, now_ms);
            }
            if (now_ms >= next_poll_ms) {
                c.now_ms = now_ms;
                (void)kn_rx_video_poll(&v, now_ms, dec_frame_in_cb, &c);
                dec_drain_events(&c);
                dec_take_kf(&c);
                dec_drain_queue(&c);
                next_poll_ms = now_ms + KN_SOCK_POLL_INTERVAL_MS;
            }
            /* ---- round XXXVI: the periodic feedback senders. Every
             * cadence/byte is the reference's (RR 1 s sid 14 :8041-8060;
             * QoS 50 ms sid 6; keepalive 3 s sid 0, the byte-exact
             * golden of nvst_input.rs:1769; NACK-v2 sid 6 + RTCP NACK
             * sid 14 from the kn_rx tracker's own interval policy; RFI
             * sid 6). Run 16 PROVED the rig stops video without them
             * (~10 s, 307 frames, then progress_stall — "without any
             * receiver feedback the server stops video", :4320-4323).
             * Encoding happens HERE (main = the kn_rx single owner);
             * the pump thread ships it (D21 untouched). */
            if (c.fb) {
                uint32_t f_ssrc = 0, f_base = 0, f_high = 0;
                uint64_t f_recv = 0;
                int have_rr = kn_rx_video_rr_snapshot(&v, &f_ssrc, &f_base,
                                                      &f_high, &f_recv);
                if (have_rr && now_ms >= c.next_rr_ms) {
                    uint32_t expected = f_high - f_base + 1u;
                    uint32_t rec32 = (uint32_t)(f_recv & 0xFFFFFFFFull);
                    uint32_t de = expected - c.rr_prior_expected;
                    uint32_t dr = rec32 - c.rr_prior_received;
                    uint8_t frac = 0;
                    uint8_t rr[32];
                    if (c.rr_prior_expected && de > 0) {
                        uint64_t lost = de > dr ? (uint64_t)(de - dr) : 0;
                        uint64_t f256 = (lost << 8) / de;
                        frac = f256 > 255u ? 255u : (uint8_t)f256;
                    }
                    kn_rx_rtcp_rr(SESS_RTCP_SENDER_SSRC, f_ssrc, frac,
                                  (int32_t)((int64_t)expected -
                                            (int64_t)rec32),
                                  f_high, 0u, rr);
                    if (sess_fb_push(c.fb, 14, rr, sizeof rr) == 0)
                        c.fb_rr++;
                    c.rr_prior_expected = expected;
                    c.rr_prior_received = rec32;
                    c.next_rr_ms = now_ms + SESS_RR_INTERVAL_MS;
                }
                if (now_ms >= c.next_qos_ms) {
                    kn_rx_qos q;
                    kn_bw_feedback bw;
                    uint8_t qb[4 + KN_RX_CTRL_QOS_PAYLOAD_LEN];
                    uint32_t s_ssrc = 0, s_base = 0, s_high = 0,
                             s_recv = 0;
                    int have_snap, n;
                    memset(&q, 0, sizeof q);
                    q.sequence = ++c.qos_seq;             /* wrapping_add */
                    /* qos_report_at (nvst.rs:1140-1177). The frame plane
                     * is publish_completed_frame's (:987-993): the LAST
                     * completed frame index and the CUMULATIVE completed
                     * bytes, not this process's feedback byte total. */
                    q.sender_frame_number = c.last_sender_frame;
                    q.bytes_received =
                        (uint32_t)(c.completed_frame_bytes & 0xFFFFFFFFull);
                    /* The packet snapshot is the QoS one (nvst.rs:900-914,
                     * u32 with `received` saturating), NOT the u64 RR
                     * counters — and the loss field is the DELTA between
                     * the previous snapshot and this one, guarded exactly
                     * like the reference's filter (:1147-1166): same ssrc,
                     * same base, both counters monotonic. Anything else
                     * reports 0 rather than a bogus ratio. */
                    have_snap = kn_rx_video_qos_snapshot(&v, &s_ssrc,
                                                         &s_base, &s_high,
                                                         &s_recv);
                    if (have_snap) {
                        q.have_packet_snapshot = 1;
                        q.snap_ssrc = s_ssrc;
                        q.snap_base = s_base;
                        q.snap_highest = s_high;
                        q.snap_received = s_recv;
                        if (c.have_qos_prior &&
                            c.qos_prior_ssrc == s_ssrc &&
                            c.qos_prior_base == s_base &&
                            s_high >= c.qos_prior_highest &&
                            s_recv >= c.qos_prior_received) {
                            uint32_t expected = s_high - c.qos_prior_highest;
                            uint32_t received = s_recv - c.qos_prior_received;
                            if (expected)
                                q.loss_per_ten_thousand = (uint16_t)
                                    (((uint64_t)(expected > received
                                                 ? expected - received : 0u)
                                      * 10000ull) / (uint64_t)expected);
                        }
                        c.have_qos_prior = 1;
                        c.qos_prior_ssrc = s_ssrc;
                        c.qos_prior_base = s_base;
                        c.qos_prior_highest = s_high;
                        c.qos_prior_received = s_recv;
                    }
                    /* client_time_90khz = elapsed.as_millis() * 90
                     * (:1172, elapsed = now - transport_origin). Run 18
                     * sent an absolute 90 kHz wall reading here, which is
                     * a different quantity and wraps every 13 h. */
                    q.client_time_90khz = (uint32_t)
                        ((now_ms > c.transport_origin_ms
                              ? now_ms - c.transport_origin_ms : 0u) * 90u);
                    /* D-t10 FIXED: the seven estimator fields, from
                     * BandwidthEstimator::report(now) — the same call the
                     * reference makes inside qos_report_at (:1146). Zeros
                     * here are the reference's own gate (no packet yet, or
                     * fewer than 100 completed frames for
                     * utilization/estimate), never a missing port. */
                    if (kn_rx_video_bw_report(&v, 0, &bw)) {
                        q.bw_minimum_server_time = bw.minimum_server_time;
                        q.bw_median_server_time = bw.median_server_time;
                        q.bw_queue_delay_us = bw.queue_delay_us;
                        q.bw_lossy_frames = bw.lossy_frames;
                        q.bw_estimate_kbps = bw.estimate_kbps;
                        q.bw_jitter_us = bw.jitter_us;
                        q.bw_utilization_percent = bw.utilization_percent;
                    }
                    n = kn_rx_ctrl_qos_report(&q, qb);
                    /* nvst.rs:7946-7967: the QoS record is appended to
                     * the batch, and because flush_due() is true the
                     * instant a QoS report is queued (:509-515), the
                     * batch goes out as ONE partial-control write here —
                     * carrying whatever frame acks/pacing records were
                     * waiting with it. That is the reference's cadence:
                     * one control message per 50 ms, not one per record. */
                    if (n > 0) {
                        if (!sess_cb_fits(&c.cbatch, (size_t)n))
                            (void)sess_cb_flush(&c.cbatch, c.fb, "capacity");
                        if (sess_cb_append(&c.cbatch, qb, (size_t)n,
                                           now_ms)) {
                            c.cbatch.have_qos = 1;
                            c.fb_qos++;
                        }
                        (void)sess_cb_flush(&c.cbatch, c.fb, "qos");
                    }
                    c.qos_prior_ms = now_ms;
                    c.qos_prior_bytes = c.fb_bytes_total;
                    c.next_qos_ms = now_ms + SESS_QOS_INTERVAL_MS;
                }
                if (now_ms >= c.next_ka_ms) {
                    static const uint8_t ka_msg[8] =
                        { 0x00, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
                    if (sess_fb_push(c.fb, 0, ka_msg, sizeof ka_msg) == 0)
                        c.fb_ka++;
                    c.next_ka_ms = now_ms + SESS_KA_INTERVAL_MS;
                }
                if (have_rr) {
                    uint64_t nf = 0, nl = 0;
                    int guard = 0;
                    while (guard++ < 8 &&
                           kn_rx_nack_take(&v.nack, now_ms, 0, &nf,
                                           &nl) == 1) {
                        uint16_t miss[64];
                        uint8_t nb[704], rn[64];
                        uint32_t cnt = 0;
                        uint64_t i2;
                        int n2;
                        size_t rl;
                        for (i2 = nf; i2 <= nl && cnt < 64u; ++i2)
                            miss[cnt++] = (uint16_t)(i2 & 0xFFFFull);
                        n2 = kn_rx_ctrl_nack_v2(0, miss, cnt, nb, sizeof nb);
                        if (n2 > 0 &&
                            sess_fb_push(c.fb, 6, nb, (size_t)n2) == 0)
                            c.fb_nack++;
                        rl = kn_rx_rtcp_nack(SESS_RTCP_SENDER_SSRC, f_ssrc,
                                           nf, nl, rn, sizeof rn);
                        if (rl && sess_fb_push(c.fb, 14, rn, rl) == 0)
                            c.fb_nack++;
                    }
                }
                {
                    uint64_t rf = 0, rlast = 0;
                    if (kn_rx_inval_take_rfi(&v.inval, &rf, &rlast)) {
                        uint8_t ob[4 +
                            KN_RX_CTRL_FRAME_INVALIDATION_PAYLOAD_LEN];
                        int n3 = kn_rx_ctrl_frame_invalidation(
                                     rf, rlast,
                                     (uint16_t)v.last_stream_packet_index,
                                     ob);
                        if (n3 > 0) {
                            if (sess_fb_push(c.fb, 6, ob, (size_t)n3) == 0)
                                c.fb_rfi++;
                            else
                                kn_rx_inval_restore_rfi(&v.inval, rf,
                                                        rlast);
                        }
                    }
                }
            }
            /* ControlReportBatch's age flush (nvst.rs:7903-7909): a
             * buffered record never waits longer than QOS_REPORT_INTERVAL
             * even when no QoS report is due (e.g. the pipeline stalled
             * and only frame acks are queued). */
            if (c.fb && sess_cb_flush_due(&c.cbatch, now_ms))
                (void)sess_cb_flush(&c.cbatch, c.fb, "age");
            if (ka_on && now_ms >= next_ka_ms) {
                uint8_t ktid[12], ktx[KN_SOCK_STUN_MAX_PKT];
                size_t ktlen;
                char kuname[128];
                if (kn_ws_rand(ktid, sizeof ktid) == 0) {
                    snprintf(kuname, sizeof kuname, "%s:%s", ka_payload,
                             ufrag);
                    if (kn_sock_stun_build(ktx, sizeof ktx, &ktlen,
                            KN_SOCK_STUN_BINDING_REQUEST, ktid,
                            (const uint8_t *)desc.remote_password,
                            strlen(desc.remote_password),
                            KN_SOCK_STUN_ATTR_USERNAME,
                            (const uint8_t *)kuname,
                            strlen(kuname)) == KN_SOCK_OK &&
                        kn_sock_udp_sendto(ms, ka_ip, ka_port, ktx,
                                           ktlen) == KN_SOCK_OK)
                        ka_sent++;
                }
                next_ka_ms = now_ms + (datagrams ? 100u : 20u);
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
                /* round XXXVIII (D-t10): the estimator plane the reference
                 * prints on its nvst-bundle line — qosBweKbps /
                 * qosUtilPercent / qosOwdUs / qosJitterUs (:8003-8017).
                 * Run 18's log could not distinguish "the estimator never
                 * ran" from "it ran and said zero", which is the exact
                 * question this round answers. bw_frames says whether the
                 * 100-frame gate is open, i.e. whether estimate and
                 * utilization are live or still honestly zero. */
                {
                    kn_bw_feedback bwf;
                    uint64_t bs = 0, bp = 0, br = 0, bt = 0, bn = 0;
                    memset(&bwf, 0, sizeof bwf);
                    (void)kn_rx_video_bw_report(&v, 0, &bwf);
                    kn_rx_video_bw_counters(&v, &bs, &bp, &br, &bt, &bn);
                    kn_lctx_i64(&p, "bw_frames",
                                (long long)kn_rx_video_bw_frames_observed(&v));
                    kn_lctx_i64(&p, "bw_samples",
                                (long long)(bs % 1000000000ull));
                    kn_lctx_i64(&p, "bw_parity",
                                (long long)(bp % 1000000000ull));
                    kn_lctx_i64(&p, "bw_skip_reorder",
                                (long long)(br % 1000000000ull));
                    kn_lctx_i64(&p, "bw_skip_retrans",
                                (long long)(bt % 1000000000ull));
                    kn_lctx_i64(&p, "bw_skip_novideo",
                                (long long)(bn % 1000000000ull));
                    kn_lctx_i64(&p, "qos_bwe_kbps",
                                (long long)bwf.estimate_kbps);
                    kn_lctx_i64(&p, "qos_util_pct",
                                (long long)bwf.utilization_percent);
                    kn_lctx_i64(&p, "qos_owd_us",
                                (long long)bwf.queue_delay_us);
                    kn_lctx_i64(&p, "qos_jitter_us",
                                (long long)bwf.jitter_us);
                    kn_lctx_i64(&p, "qos_lossy",
                                (long long)bwf.lossy_frames);
                    kn_lctx_i64(&p, "qos_min_server_time",
                                (long long)bwf.minimum_server_time);
                }
                /* R3-D: the control keepalive health, on the same beat */
                if (ctrl_ping.have || ctrl_ping.pings_sent) {
                    kn_lctx_i64(&p, "cping_sent",
                                (long long)(ctrl_ping.pings_sent %
                                            1000000000ll));
                    kn_lctx_i64(&p, "cping_pongs",
                                (long long)(ctrl_ping.pongs_matched %
                                            1000000000ll));
                    kn_lctx_i64(&p, "cping_expired",
                                (long long)(ctrl_ping.expired %
                                            1000000000ll));
                    kn_lctx_i64(&p, "cping_rtt_us",
                                (long long)(ctrl_ping.have_sample
                                    ? ctrl_ping.sample_rtt_us : 0ull));
                    kn_lctx_i64(&p, "cping_dead", ctrl_ping.dead);
                }
                kn_log_emit(&p);
            }
        }
        if (g_knv_stop) stop_reason = "signal";
        if (ka_on) {
            printf("{\"mode\":\"session\",\"event\":\"natt_keepalive\","
                   "\"sent\":%llu}\n", ka_sent % 1000000000ull);
            fflush(stdout);
        }

        /* round XXXVII: the control/input plane's final state, from a
         * FRESH snapshot (the pump thread still owns it). Run 17's
         * summary could not say whether the rig had talked to us at all
         * after the stall — this closes that gap: every TLV code the rig
         * sent, counted, plus the activation/cursor outcome. */
        if (ice_enabled) {
            sess_pump_snapshot(&pump, &ice_st);
            printf("{\"mode\":\"session\",\"event\":\"ctrl_summary\","
                   "\"msgs\":%u,\"records\":%u,\"version\":%d,"
                   "\"input_ready\":%u,\"activation_sent\":%u,"
                   "\"activation_fail\":%u,\"activation_attempts\":%u,"
                   "\"cursor\":\"%s\",\"cursor_enables\":%u,"
                   "\"cursor_trackings\":%u,\"cursor_disables\":%u,"
                   "\"cursor_handoffs\":%u,\"hist_overflow\":%u,"
                   "\"codes\":%s}\n",
                   ice_st.ctrl_msgs, ice_st.ctrl_records,
                   ice_st.input_version, ice_st.input_ready,
                   ice_st.activation_sent, ice_st.activation_fail,
                   ice_st.activation_attempts,
                   kn_input_cursor_state_name(
                       (kn_input_cursor_state)ice_st.cursor_state),
                   ice_st.cursor_enables, ice_st.cursor_trackings,
                   ice_st.cursor_disables, ice_st.cursor_handoffs,
                   ice_st.ctrl_hist_overflow, ice_st.ctrl_hist);
            fflush(stdout);
        }

        if (c.stream && !c.decode_aborted)
            (void)kn_decode_stream_flush(c.stream, dec_frame_out_cb, &c);
        {   /* summary — field-compatible with the decode/replay contract
             * (make_rx_capture.py --verify consumes it unchanged) */
            char drops[512];
            char sid_json[320];
            kn_bw_feedback bwf;
            uint64_t bw_samples = 0, bw_parity = 0, bw_skip_re = 0,
                     bw_skip_rt = 0, bw_skip_nv = 0;
            unsigned si;
            kn_decode_stats dst;
            size_t doff = 0;

            /* The final estimator report + its feed counters: this block
             * is what says whether D-t10 is closed. frames_observed below
             * 100 would mean estimate/utilization are the reference's OWN
             * gated zeros (nvst.rs:1068-1069), not a missing port. */
            memset(&bwf, 0, sizeof bwf);
            (void)kn_rx_video_bw_report(&v, 0, &bwf);
            kn_rx_video_bw_counters(&v, &bw_samples, &bw_parity, &bw_skip_re,
                                    &bw_skip_rt, &bw_skip_nv);
            /* per-sid outbox pushes/drops: run 18's dropped:890 could not
             * name which channel overflowed */
            sid_json[0] = '\0';
            for (si = 0; si < SESS_FB_SIDS; si++) {
                size_t sl = strlen(sid_json);
                if (sl + 40 >= sizeof sid_json) break;
                snprintf(sid_json + sl, sizeof sid_json - sl,
                         "%s\"%u\":[%llu,%llu]",
                         strlen(sid_json) ? "," : "",
                         si * 2u,
                         fb.pushed_per_sid[si] % 1000000000ull,
                         fb.dropped_per_sid[si] % 1000000000ull);
            }
            if (sid_json[0] == '\0')
                snprintf(sid_json, sizeof sid_json, "{}");
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
                   "\"wall_ms\":%llu,\"drops\":{%s},"
                   "\"feedback\":{\"rr\":%llu,\"ack\":%llu,\"pacing\":%llu,"
                   "\"qos\":%llu,\"ka\":%llu,\"nack\":%llu,\"idr\":%llu,"
                   "\"pli\":%llu,\"rfi\":%llu,\"pushed\":%llu,"
                   "\"dropped\":%llu,"
                   "\"pacing_disabled_skips\":%llu},"
                   "\"batch\":{\"flushes\":%llu,\"records\":%u,"
                   "\"acks_sent\":%u,\"qos_sent\":%u,"
                   "\"last_ack_frame\":%u,\"by_cause\":{\"capacity\":%llu,"
                   "\"qos\":%llu,\"age\":%llu},\"failed\":%llu,"
                   "\"retained\":%llu,\"bytes_pending\":%u},"
                   "\"outbox_by_sid\":%s,"
                   "\"bwe\":{\"frames_observed\":%u,\"samples\":%u,"
                   "\"parity\":%u,\"skip_reordered\":%u,"
                   "\"skip_retransmitted\":%u,\"skip_no_video\":%u,"
                   "\"estimate_kbps\":%u,\"utilization_pct\":%u,"
                   "\"min_server_time\":%u,\"median_server_time\":%u,"
                   "\"queue_delay_us\":%u,\"jitter_us\":%u,"
                   "\"lossy_frames\":%u},"
                   "\"au_clock\":{\"drift_ms\":%lld,\"drift_min_ms\":%lld,"
                   "\"drift_max_ms\":%lld,\"ts_backwards\":%llu,"
                   "\"wall_ms\":%llu,\"rtp_ms\":%llu}}}\n",
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
                   (unsigned long long)(ms->recv_timeouts %
                                        10000000000ull),
                   (unsigned long long)(ms->recv_errors % 1000000000ull),
                   c.event_lines_capped % 1000000000ull,
                   c.au_write_errors % 1000000000ull,
                   profile_name, v.state,
                   c.stream ? kn_decode_backend() : "pipeline-only",
                   c.last_w % 100000, c.last_h % 100000,
                   (unsigned long long)((kn_sock_now_ms() - t_start) %
                                        1000000000ull),
                   drops,
                   c.fb_rr % 1000000ull, c.fb_ack % 1000000000ull,
                   c.fb_pacing % 1000000000ull, c.fb_qos % 1000000000ull,
                   c.fb_ka % 1000000ull, c.fb_nack % 1000000ull,
                   c.fb_idr % 1000000ull, c.fb_pli % 1000000ull,
                   c.fb_rfi % 1000000ull, fb.pushed % 1000000000ull,
                   fb.dropped % 1000000ull,
                   c.fb_pacing_disabled % 1000000000ull,
                   c.cbatch.flushes % 1000000000ull,
                   (unsigned)c.cbatch.records % 1000000u,
                   c.cbatch.acks % 1000000u, c.cbatch.qos_reports % 1000000u,
                   c.cbatch.have_last_ack_frame ? c.cbatch.last_ack_frame : 0u,
                   c.cbatch.flush_capacity % 1000000000ull,
                   c.cbatch.flush_qos % 1000000000ull,
                   c.cbatch.flush_age % 1000000000ull,
                   (unsigned long long)(c.cbatch.flush_failed % 1000000000ull),
                   c.cbatch.retained % 1000000ull,
                   (unsigned)c.cbatch.len,
                   sid_json,
                   kn_rx_video_bw_frames_observed(&v),
                   (unsigned)(bw_samples % 1000000000ull),
                   (unsigned)(bw_parity % 1000000000ull),
                   (unsigned)(bw_skip_re % 1000000000ull),
                   (unsigned)(bw_skip_rt % 1000000000ull),
                   (unsigned)(bw_skip_nv % 1000000000ull),
                   bwf.estimate_kbps % 1000000u,
                   (unsigned)bwf.utilization_percent,
                   bwf.minimum_server_time, bwf.median_server_time,
                   bwf.queue_delay_us, (unsigned)bwf.jitter_us,
                   bwf.lossy_frames,
                   c.au_drift_ms, c.au_drift_min_ms, c.au_drift_max_ms,
                   c.au_ts_backwards % 1000000000ull,
                   (unsigned long long)(c.have_au_first
                       ? (kn_sock_now_ms() - c.au_first_wall_ms) % 1000000000ull : 0ull),
                   (unsigned long long)(c.have_au_first
                       ? ((uint32_t)(c.au_last_ts - c.au_first_ts) / 90u)
                         % 1000000000ull : 0ull));
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
        /* R3-B/C stop order: join the bundle worker FIRST (it owns
         * bsock, the dtls + sctp contexts while running), then close
         * sctp (its write path is dtls), then free dtls, then sockets */
        sess_pump_stop(&pump);
        sess_fb_destroy(&fb);
        if (pump.sctp) { kn_sctp_close(pump.sctp); pump.sctp = NULL; }
        if (dtls) { kn_dtls_free(dtls); dtls = NULL; }
        if (have_sock)  { kn_sock_udp_close(&sock);  have_sock = 0; }
        if (have_bsock) { kn_sock_udp_close(&bsock); have_bsock = 0; }
        goto teardown;

    teardown_sock:
        sess_pump_stop(&pump);          /* idempotent (started flag) */
        sess_fb_destroy(&fb);
        if (pump.sctp) { kn_sctp_close(pump.sctp); pump.sctp = NULL; }
        if (dtls) { kn_dtls_free(dtls); dtls = NULL; }
        if (have_sock)  { kn_sock_udp_close(&sock);  have_sock = 0; }
        if (have_bsock) { kn_sock_udp_close(&bsock); have_bsock = 0; }
    }
teardown:
    /* best-effort TEARDOWN (the reference's Drop path: 1 s, errors
     * ignored, sequence mismatches skipped) then close everything */
    sess_ctrl_ping_stop(&ctrl_ping);   /* R3-D: join before TEARDOWN */
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
    if (cert) { kn_cert_free(cert); cert = NULL; }   /* after kn_dtls */
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
    /* queue default 600 s (round XXIII): the R7 runs showed real queues
     * 160+ deep; 180 s risked aborting a legitimate wait (the abort now
     * also cancels the session, but a seat is a seat) */
    long sess_seconds = 60, sess_queue_timeout = 600, sess_expect = 0;
    long sess_play_wait = 2500, sess_play_settle = 250;   /* round XXXIII
        PLAY gating: the reference's finish() lands after its receiver
        plumbing gave DTLS+SCTP a head start (lib.rs:937->1117); runs
        13/14 sent PLAY ~12 ms after dtls_ready and drew 400. A bounded
        wait for sctp_up + a settle window remove the timing confound. */
    long cap_port = 4798, dec_port = 4798;
    long cap_seconds = 30, dec_seconds = 30;
    long dec_vps = 0, dec_expect = 0;
    const char *cap_bind = NULL, *dec_bind = NULL;
    const char *cap_out = "gs_capture.hex", *dec_au_dir = NULL;
    const char *dec_srtp = NULL;
    int diagnostics = 0, version = 0, list = 0, sess_keep = 0, i;

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
        else if (strcmp(a, "--session-keep") == 0) sess_keep = 1;
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
        else if (strncmp(a, "--session-play-wait=", 20) == 0)
            sess_play_wait = strtol(a + 20, NULL, 10);
        else if (strncmp(a, "--session-play-settle=", 22) == 0)
            sess_play_settle = strtol(a + 22, NULL, 10);
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
                           sess_seconds, sess_queue_timeout, sess_keep,
                           sess_expect, sess_play_wait, sess_play_settle,
                           sess_au_dir, log_dir);
    }
    if (diagnostics) return run_diagnostics(log_dir);
    usage();
    return 2;
}
