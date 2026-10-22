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
#include "kn/kn_diag.h"
#include "kn/kn_selftest.h"
#include "kn/knlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    printf("knvstd - KarnyNow streamer (pre-stage-1 skeleton)\n"
           "usage:\n"
           "  knvstd --selftest=<name|all> [--log-dir=DIR]\n"
           "  knvstd --diagnostics [--log-dir=DIR]\n"
           "  knvstd --list-selftests\n"
           "  knvstd --version\n"
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
        n = kn_selftest_run_all(results, 32);
    } else {
        if (kn_selftest_run(which, &results[0]) != 0) {
            printf("{\"error\":\"unknown_selftest\",\"name\":\"%s\"}\n", which);
            kn_log_shutdown();
            return 2;
        }
        n = 1;
    }

    n_doc = kn_selftest_render_json(results, n, doc, sizeof doc);
    if (n_doc) fwrite(doc, 1, n_doc, stdout);
    fputc('\n', stdout);

    for (i = 0; i < n; ++i) {
        kn_lctx c;
        kn_lctx_init(&c, KN_CH_DIAG,
                     results[i].status == KN_ST_OK ? KN_INFO :
                     results[i].status == KN_ST_SKIPPED ? KN_DEBUG : KN_WARN,
                     "selftest_result");
        kn_lctx_str(&c, "name", results[i].name);
        kn_lctx_str(&c, "status", kn_selftest_status_name(results[i].status));
        kn_lctx_i64(&c, "ms", results[i].ms);
        kn_lctx_i64(&c, "checks", results[i].checks);
        kn_lctx_i64(&c, "failures", results[i].failures);
        kn_log_emit(&c);
        if (results[i].status == KN_ST_FAILED) worst = 1;
    }
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
    int diagnostics = 0, version = 0, list = 0, i;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strncmp(a, "--selftest=", 11) == 0) selftest = a + 11;
        else if (strcmp(a, "--diagnostics") == 0) diagnostics = 1;
        else if (strcmp(a, "--version") == 0) version = 1;
        else if (strcmp(a, "--list-selftests") == 0) list = 1;
        else if (strncmp(a, "--log-dir=", 10) == 0) log_dir = a + 10;
        else { usage(); return 2; }
    }

    if (list) return print_registry();
    if (version) return print_version();
    if (selftest && selftest[0]) return run_selftests(selftest, log_dir);
    if (diagnostics) return run_diagnostics(log_dir);
    usage();
    return 2;
}
