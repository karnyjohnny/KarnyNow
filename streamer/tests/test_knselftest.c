/* test_knselftest.c — unit tests for the §7.9 self-test subsystem.
 *
 * The offline vector tests (fec.vectors, srtp.vectors) run FOR REAL here —
 * on the host they double as a second execution path over the same KAT
 * vectors the unit tests use. The Win32 tests report "skipped" on POSIX;
 * their real execution happens on the target via knvstd --selftest=<name>.
 */
#include "kn/kn_selftest.h"
#include "kn/kn_diag.h"
#include "kn/kn_decode.h"

#include "test_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define test_mkdir(p) _mkdir(p)
#else
#  include <dirent.h>
#  define test_mkdir(p) mkdir(p, 0755)
#endif

#define TESTDIR "testst"

static void purge_dir(const char *dir)
{
#ifdef _WIN32
    char pattern[512];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char p[512];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(p, sizeof p, "%s/%s", dir, fd.cFileName);
            remove(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            char p[512];
            if (de->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
            remove(p);
        }
        closedir(dp);
    }
#endif
    kn_test_rmdir(dir);
}

static void test_registry(void)
{
    int n = kn_selftest_count();
    int i, saw_fec = 0, saw_srtp = 0, saw_clock = 0, saw_console = 0;
    CHECK(n == 12, "registry holds all 12 names from §7.9");
    for (i = 0; i < n; ++i) {
        const char *nm = kn_selftest_name(i);
        CHECK(nm && nm[0], "registry name non-empty");
        if (nm) {
            if (!strcmp(nm, "fec.vectors")) saw_fec = 1;
            if (!strcmp(nm, "srtp.vectors")) saw_srtp = 1;
            if (!strcmp(nm, "clock.resolution")) saw_clock = 1;
            if (!strcmp(nm, "console.host")) saw_console = 1;
        }
    }
    CHECK(saw_fec && saw_srtp && saw_clock && saw_console, "core four registered");
    CHECK(kn_selftest_name(-1) == NULL, "name(-1) NULL");
    CHECK(kn_selftest_name(n) == NULL, "name(n) NULL");
}

static void test_fec_vectors_run(void)
{
    kn_st_result r;
    CHECK(kn_selftest_run("fec.vectors", &r) == 0, "fec.vectors runs");
    CHECK(strcmp(r.name, "fec.vectors") == 0, "result carries name");
    CHECK(r.status == KN_ST_OK, "fec.vectors OK on this build");
    CHECK(r.checks >= 80, "fec.vectors executed >= 80 KAT checks");
    CHECK(r.failures == 0, "fec.vectors zero failures");
    CHECK(strstr(r.evidence, "\"checks\":") != NULL, "fec evidence has checks");
    CHECK(strstr(r.evidence, "fec_reference.py") != NULL, "fec evidence names generator");
    CHECK(r.ms >= 0 && r.ms < 5000, "fec.vectors bounded in time");
}

static void test_srtp_vectors_run(void)
{
    kn_st_result r;
    CHECK(kn_selftest_run("srtp.vectors", &r) == 0, "srtp.vectors runs");
    CHECK(r.status == KN_ST_OK, "srtp.vectors OK on this build");
    CHECK(r.checks >= 50, "srtp.vectors executed >= 50 KAT checks");
    CHECK(r.failures == 0, "srtp.vectors zero failures");
    CHECK(strstr(r.evidence, "\"replay_ops\":96") != NULL, "replay simulation ran");
    CHECK(strstr(r.evidence, "\"replay_mismatches\":0") != NULL, "replay zero mismatches");
    CHECK(r.ms >= 0 && r.ms < 5000, "srtp.vectors bounded in time");
}

static void test_platform_and_stubs(void)
{
    /* decode.synthetic is IMPLEMENTED (skips only when the build lacks
     * FFmpeg or the clip is missing), so it is not in the stub list */
    static const char *stubs[] = {
        "d3d.formats", "d3d.present", "gs.parse",
        "net.dns", "net.tcp443", "net.tls", "tls.chain"
    };
    size_t i;
    kn_st_result r;
    for (i = 0; i < sizeof stubs / sizeof stubs[0]; ++i) {
        CHECK(kn_selftest_run(stubs[i], &r) == 0, "stub registered");
        CHECK(r.status == KN_ST_SKIPPED, "stub reports skipped, never ok");
        CHECK(strstr(r.reason, "not implemented") != NULL, "stub says not implemented");
    }
    /* decode.synthetic: honest status in every build mode */
    CHECK(kn_selftest_run("decode.synthetic", &r) == 0, "decode.synthetic runs");
    if (!kn_decode_available()) {
        CHECK(r.status == KN_ST_SKIPPED, "decode skipped without FFmpeg");
        CHECK(strstr(r.reason, "without FFmpeg") != NULL, "decode skip names FFmpeg");
    } else {
        CHECK(r.status != KN_ST_FAILED || r.failures > 0,
              "decode failure is reported with evidence");
        CHECK(strstr(r.reason, "not implemented") == NULL,
              "decode is not a stub when FFmpeg is linked");
    }
#ifndef _WIN32
    CHECK(kn_selftest_run("clock.resolution", &r) == 0 && r.status == KN_ST_SKIPPED,
          "clock.resolution skipped on posix host");
    CHECK(kn_selftest_run("console.host", &r) == 0 && r.status == KN_ST_SKIPPED,
          "console.host skipped on posix host");
    CHECK(strstr(r.reason, "target machine") != NULL, "skip reason points to target");
#endif
}

static void test_unknown_and_args(void)
{
    kn_st_result r;
    CHECK(kn_selftest_run("no.such.test", &r) == -1, "unknown name -> -1");
    CHECK(kn_selftest_run(NULL, &r) == -1, "NULL name -> -1");
    CHECK(kn_selftest_run("fec.vectors", NULL) == -1, "NULL result -> -1");
    CHECK(kn_selftest_run_all(NULL, 4) == 0, "run_all NULL -> 0");
    CHECK(kn_selftest_run_all(&r, 0) == 0, "run_all max 0 -> 0");
    CHECK(kn_selftest_run_all(&r, 1) == 1, "run_all respects max");
}

static void test_run_all_and_render(void)
{
    static kn_st_result results[16];
    char doc[16384];
    size_t n_doc;
    int n = kn_selftest_run_all(results, 16);
    int i, ok_count = 0, skipped_count = 0;

    CHECK(n == 12, "run_all executes the whole registry");
    for (i = 0; i < n; ++i) {
        if (results[i].status == KN_ST_OK) ok_count++;
        if (results[i].status == KN_ST_SKIPPED) skipped_count++;
    }
    /* fec/srtp vectors must never FAIL on a healthy build (any platform) */
    for (i = 0; i < n; ++i)
        if (!strcmp(results[i].name, "fec.vectors") ||
            !strcmp(results[i].name, "srtp.vectors"))
            CHECK(results[i].status == KN_ST_OK, "offline vector test OK");
#ifdef _WIN32
    /* on the target, clock.resolution and console.host REALLY run.
     * console.host may legitimately report failed WITHOUT ConEmu (that is
     * the D1 verdict, not a test bug), so only require "executed".
     * decode.synthetic skips only when FFmpeg/clip are absent (7 or 8
     * skipped total), and must never be a silent stub when linked. */
    for (i = 0; i < n; ++i)
        if (!strcmp(results[i].name, "clock.resolution") ||
            !strcmp(results[i].name, "console.host"))
            CHECK(results[i].status != KN_ST_SKIPPED, "win32 test executed");
    CHECK(ok_count >= 2, "at least the offline vector tests are OK on target");
    CHECK(skipped_count == 7 || skipped_count == 8,
          "7 stubs (+decode without ffmpeg/clip) skipped");
    for (i = 0; i < n; ++i)
        if (!strcmp(results[i].name, "decode.synthetic"))
            CHECK(results[i].status != KN_ST_FAILED || kn_decode_available() == 0
                  || results[i].failures > 0,
                  "decode did not fail silently");
#else
    CHECK(ok_count == 2, "exactly the two offline vector tests are OK (host)");
    CHECK(skipped_count == 10, "the rest honestly report skipped (host)");
#endif

    n_doc = kn_selftest_render_json(results, n, doc, sizeof doc);
    CHECK(n_doc > 100 && n_doc < sizeof doc, "render produced a document");
    CHECK(doc[0] == '{', "document is a JSON object");
    CHECK(strchr(doc, '\n') == NULL, "document is ONE line (JSON-lines contract)");
    CHECK(strstr(doc, "\"generated_utc\":") != NULL, "document has timestamp");
    CHECK(strstr(doc, "\"name\":\"fec.vectors\"") != NULL, "document lists fec");
    CHECK(strstr(doc, "\"name\":\"tls.chain\"") != NULL, "document lists tls.chain");
    CHECK(strstr(doc, "\"truncated\":false}") != NULL, "document closed cleanly");

    /* KN_TEST_KEEP=1: leave the rendered document on disk so the build
     * script can validate it with python json (independent parser) */
    if (getenv("KN_TEST_KEEP")) {
        FILE *f = fopen("selftest_render_check.json", "wb");
        if (f) { fwrite(doc, 1, n_doc, f); fputc('\n', f); fclose(f); }
    }
}

static void test_render_relevant(void)
{
    char doc[16384];

    /* SRTP-flavoured failure -> srtp.vectors only */
    CHECK(kn_selftest_render_relevant("session.stream", "SRTP_AUTH_FAILED",
                                      doc, sizeof doc) > 0, "relevant render ok");
    CHECK(strstr(doc, "\"name\":\"srtp.vectors\"") != NULL, "auth failure -> srtp.vectors");
    CHECK(strstr(doc, "\"name\":\"fec.vectors\"") == NULL, "auth failure skips fec");

    /* FEC/video failure -> fec.vectors */
    CHECK(kn_selftest_render_relevant("media", "E_VIDEO_CORRUPT", doc, sizeof doc) > 0,
          "video render ok");
    CHECK(strstr(doc, "\"name\":\"fec.vectors\"") != NULL, "corrupt video -> fec.vectors");
    CHECK(strstr(doc, "\"name\":\"srtp.vectors\"") == NULL, "video failure skips srtp");

    /* network failure -> the four net/tls names (skipped stubs today) */
    CHECK(kn_selftest_render_relevant("login", "E_DNS_TIMEOUT", doc, sizeof doc) > 0,
          "net render ok");
    CHECK(strstr(doc, "\"name\":\"net.dns\"") != NULL, "dns failure -> net.dns");
    CHECK(strstr(doc, "\"name\":\"tls.chain\"") != NULL, "dns failure -> tls.chain");

    /* unknown code -> both cheap offline proofs (default) */
    CHECK(kn_selftest_render_relevant("whatever", "E_SOMETHING_ODD",
                                      doc, sizeof doc) > 0, "default render ok");
    CHECK(strstr(doc, "\"name\":\"fec.vectors\"") != NULL, "default includes fec");
    CHECK(strstr(doc, "\"name\":\"srtp.vectors\"") != NULL, "default includes srtp");
}

static void test_diag_provider_integration(void)
{
    /* kn_selftest_render_relevant must be assignable to the kn_diag provider
     * typedef (compile-time signature match) and its output must land in the
     * bundle as selftest.json when a failure triggers a write. */
    kn_diag_selftest_provider_fn fn = kn_selftest_render_relevant;
    char out[KN_DIAG_PATH_MAX];
    unsigned char *z;
    size_t zlen = 0;
    char *st;
    int rc;

    CHECK(fn != NULL, "provider signature matches typedef");
    kn_diag_set_selftest_provider(fn);

    purge_dir(TESTDIR);
    test_mkdir(TESTDIR);
    rc = kn_diag_write_bundle(TESTDIR, "provider.test", "E_FEC_REPAIR_FAILED",
                              "integration", out, sizeof out);
    CHECK(rc == 0, "write_bundle with provider ok");
    z = (unsigned char *)kn_test_read_file(out, &zlen);
    CHECK(z != NULL, "bundle readable");
    if (z) {
        /* reuse the same minimal reader approach: summary first, then look
         * for selftest.json by brute-force scan of local headers */
        size_t i;
        int found = 0;
        for (i = 0; i + 30 < zlen; ++i) {
            if (z[i] == 0x50 && z[i+1] == 0x4b && z[i+2] == 0x03 && z[i+3] == 0x04) {
                unsigned nl = (unsigned)(z[i+26] | (z[i+27] << 8));
                if (nl == 13 && memcmp(z + i + 30, "selftest.json", 13) == 0) {
                    found = 1;
                    break;
                }
            }
        }
        CHECK(found == 1, "bundle contains selftest.json entry");
        free(z);
    }
    /* extract via summary listing would need the full reader; the entry scan
     * above plus the unit-tested zip writer is sufficient evidence here.
     * Full content validation happens through tools/check_zip.py + the
     * knvstd --diagnostics smoke run in build_native_tests.sh. */
    st = out;
    remove(st);
    purge_dir(TESTDIR);
    kn_diag_set_selftest_provider(NULL);
}

int main(void)
{
    test_registry();
    test_fec_vectors_run();
    test_srtp_vectors_run();
    test_platform_and_stubs();
    test_unknown_and_args();
    test_run_all_and_render();
    test_render_relevant();
    test_diag_provider_integration();
    if (!getenv("KN_TEST_KEEP")) remove("selftest_render_check.json");
    return kn_test_summary("test_knselftest");
}
