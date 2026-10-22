/* test_knlog.c — unit tests for the knlog subsystem. Runs unmodified on the
 * Linux host (fast iteration) and on the Windows 7 target (real verification
 * of file, rename and flush semantics). Set KN_TEST_KEEP=1 to keep the log
 * directories for external inspection (tools/check_jsonl.py). */
#include "kn/knlog.h"
#include "test_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define DIR1 "testlogs"
#define DIR2 "testlogs2"

static const char *const k_channels[KN_CH__COUNT] = {
    "app", "http", "rtsp", "transport", "media", "input", "perf", "diag"
};

static void cleanup_dir(const char *dir)
{
    char path[256];
    int i, gen;
    for (i = 0; i < KN_CH__COUNT; ++i) {
        for (gen = 0; gen <= 4; ++gen) {
            if (gen == 0) snprintf(path, sizeof path, "%s/%s.log", dir, k_channels[i]);
            else          snprintf(path, sizeof path, "%s/%s.%d.log", dir, k_channels[i], gen);
            kn_test_unlink(path);
            if (gen == 0) snprintf(path, sizeof path, "%s/%s.jsonl", dir, k_channels[i]);
            else          snprintf(path, sizeof path, "%s/%s.%d.jsonl", dir, k_channels[i], gen);
            kn_test_unlink(path);
        }
    }
    kn_test_rmdir(dir);
}

/* Return a malloc'd copy of the first line containing `needle`, or NULL. */
static char *find_line(const char *filebuf, const char *needle)
{
    const char *p = filebuf;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = (char *)malloc(len + 1);
        if (!line) return NULL;
        memcpy(line, p, len);
        line[len] = '\0';
        if (strstr(line, needle)) return line;
        free(line);
        if (!eol) break;
        p = eol + 1;
    }
    return NULL;
}

static int is_lower_hex12(const char *s)
{
    int i;
    if (!s || strlen(s) != KN_CORR_LEN) return 0;
    for (i = 0; i < KN_CORR_LEN; ++i) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------- failure cb */

static int  g_cb_calls;
static char g_cb_op[64], g_cb_code[64], g_cb_reason[128];

static void fail_cb(const char *op, const char *code, const char *reason, void *ud)
{
    (void)ud;
    g_cb_calls++;
    snprintf(g_cb_op, sizeof g_cb_op, "%s", op);
    snprintf(g_cb_code, sizeof g_cb_code, "%s", code);
    snprintf(g_cb_reason, sizeof g_cb_reason, "%s", reason);
}

/* ------------------------------------------------------------------ tests */

static void test_init_bad_dir(void)
{
    kn_log_config bad;
    memset(&bad, 0, sizeof bad);
    bad.log_dir = "no_such_root_dir/deep/deeper";
    CHECK(kn_log_init(&bad) == -1, "init with uncreatable dir returns -1");
    CHECK(kn_log_is_initialized() == 0, "not initialized after failed init");
}

static void test_instance_1(void)
{
    kn_log_config cfg;
    kn_log_stats st;
    char *logbuf, *jsonbuf, *line;
    char corr_a[KN_CORR_LEN + 1], corr_b[KN_CORR_LEN + 1];
    int i, rc;

    memset(&cfg, 0, sizeof cfg);
    cfg.log_dir = DIR1;
    cfg.default_level = KN_INFO;
    cfg.jsonl_channels = UINT_MAX;      /* all channels get .jsonl in tests */
    cfg.rotate_bytes = 512;             /* tiny, to exercise rotation */
    cfg.generations = 3;
    cfg.build_stamp = "deadbeef-test-x86_64-gcc";
    cfg.version = "0.0.1";
    rc = kn_log_init(&cfg);
    CHECK(rc == 0, "init succeeds");
    CHECK(kn_log_init(&cfg) == -3, "double init returns -3");

    /* process_start with build stamp (§7.2) */
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/app.log", NULL);
    CHECK_STR_CONTAINS(logbuf, "process_start", "process_start emitted at init");
    CHECK_STR_CONTAINS(logbuf, "build=deadbeef-test-x86_64-gcc", "build stamp present");
    free(logbuf);

    /* -- fields, quoting, redaction ------------------------------------ */
    {
        kn_lctx c;
        kn_lctx_init(&c, KN_CH_RTSP, KN_INFO, "SETUP response");
        kn_lctx_corr(&c, "abc123def456");
        kn_lctx_str(&c, "transport", "udp;unicast;client_port=49005");
        kn_lctx_i64(&c, "status", 200);
        kn_lctx_hex(&c, "hr", 0x8876086Cull);
        kn_lctx_bool(&c, "ok", 1);
        kn_lctx_f64(&c, "elapsed_ms", 12.5);
        kn_lctx_str(&c, "sdp_line", "v=0\r\no=- 0 0 IN IP4");
        kn_lctx_str(&c, "accessToken", "supersecret");           /* auto-redacted by key */
        kn_lctx_secret(&c, "iceCredential", "xyz");               /* explicit redaction */
        kn_log_emit(&c);
    }
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/rtsp.log", NULL);
    jsonbuf = kn_test_read_file(DIR1 "/rtsp.jsonl", NULL);
    CHECK_STR_CONTAINS(logbuf, "INFO  [rtsp]", "human level+channel format");
    CHECK_STR_CONTAINS(logbuf, "c=abc123def456", "correlation rendered");
    CHECK_STR_CONTAINS(logbuf, "SETUP response", "message rendered");
    CHECK_STR_CONTAINS(logbuf, "status=200", "i64 field");
    CHECK_STR_CONTAINS(logbuf, "hr=0x8876086C", "hex field");
    CHECK_STR_CONTAINS(logbuf, "ok=true", "bool field");
    CHECK_STR_CONTAINS(logbuf, "elapsed_ms=12.5", "f64 field");
    CHECK_STR_CONTAINS(logbuf, "transport=\"udp;unicast;client_port=49005\"",
                       "string with specials quoted");
    CHECK_STR_CONTAINS(logbuf, "accessToken=<redacted:11>", "auto key redaction");
    CHECK_STR_CONTAINS(logbuf, "iceCredential=<redacted:3>", "explicit redaction");
    CHECK_STR_NOT_CONTAINS(logbuf, "supersecret", "secret value never logged");
    CHECK_STR_NOT_CONTAINS(logbuf, "=xyz", "explicit secret value never logged");
    CHECK(logbuf && strstr(logbuf, "\\r\\n") != NULL, "CRLF escaped in human line");

    CHECK_STR_CONTAINS(jsonbuf, "\"level\":\"INFO\"", "jsonl level");
    CHECK_STR_CONTAINS(jsonbuf, "\"channel\":\"rtsp\"", "jsonl channel");
    CHECK_STR_CONTAINS(jsonbuf, "\"correlation\":\"abc123def456\"", "jsonl correlation");
    CHECK_STR_CONTAINS(jsonbuf, "\"status\":200", "jsonl number");
    CHECK_STR_CONTAINS(jsonbuf, "\"hr\":\"0x8876086C\"", "jsonl hex as string");
    CHECK_STR_CONTAINS(jsonbuf, "\"ok\":true", "jsonl bool");
    CHECK_STR_CONTAINS(jsonbuf, "<redacted:11>", "jsonl redaction");
    CHECK_STR_NOT_CONTAINS(jsonbuf, "supersecret", "jsonl has no secret values");
    CHECK(jsonbuf && strstr(jsonbuf, "\\r\\n") != NULL, "CRLF escaped in jsonl");
    CHECK(jsonbuf && strchr(jsonbuf, '\n') != NULL, "jsonl is line-delimited");
    free(logbuf);
    free(jsonbuf);

    /* -- level gate applies at runtime without restart (§7.4) ---------- */
    kn_log_set_level(KN_CH_RTSP, KN_WARN);
    CHECK(kn_log_would_log(KN_CH_RTSP, KN_INFO) == 0, "would_log false below gate");
    CHECK(kn_log_would_log(KN_CH_RTSP, KN_WARN) == 1, "would_log true at gate");
    kn_log_simple(KN_CH_RTSP, KN_INFO, NULL, "should_not_appear");
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/rtsp.log", NULL);
    CHECK_STR_NOT_CONTAINS(logbuf, "should_not_appear", "INFO filtered at WARN gate");
    free(logbuf);
    kn_log_simple(KN_CH_RTSP, KN_WARN, NULL, "warn_appears");
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/rtsp.log", NULL);
    CHECK_STR_CONTAINS(logbuf, "WARN  [rtsp] warn_appears", "WARN passes gate");
    free(logbuf);
    kn_log_set_level(KN_CH_RTSP, KN_INFO);
    kn_log_get_stats(&st);
    CHECK(st.events_filtered >= 1, "filtered events counted");

    /* -- rotation: 512 B files, 3 generations --------------------------- */
    for (i = 0; i < 40; ++i) {
        char msg[96];
        snprintf(msg, sizeof msg, "rotation_probe_%02d padding-padding-padding", i);
        kn_log_simple(KN_CH_DIAG, KN_INFO, NULL, msg);
    }
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/diag.log", NULL);
    CHECK(logbuf != NULL, "diag.log exists after rotation");
    {
        size_t cur = 0;
        char *cur_buf = kn_test_read_file(DIR1 "/diag.log", &cur);
        free(cur_buf);
        CHECK(cur > 0 && cur < 512 + 256, "current file within rotate bound + one line");
        free(logbuf);
        logbuf = kn_test_read_file(DIR1 "/diag.1.log", NULL);
        CHECK(logbuf != NULL, "generation .1 exists");
        free(logbuf);
        logbuf = kn_test_read_file(DIR1 "/diag.2.log", NULL);
        CHECK(logbuf != NULL, "generation .2 exists");
        free(logbuf);
        logbuf = kn_test_read_file(DIR1 "/diag.3.log", NULL);
        CHECK(logbuf == NULL, "generation .3 never created (3 total files)");
        free(logbuf);
        kn_log_get_stats(&st);
        CHECK(st.rotations >= 5, "rotations counted");
    }

    /* -- correlation ids ------------------------------------------------ */
    CHECK(is_lower_hex12(kn_log_new_correlation(corr_a)), "correlation A format");
    CHECK(is_lower_hex12(kn_log_new_correlation(corr_b)), "correlation B format");
    CHECK(strcmp(corr_a, corr_b) != 0, "correlations unique");

    kn_log_set_thread_correlation("beefcafe0001");
    CHECK(kn_log_get_thread_correlation() &&
          strcmp(kn_log_get_thread_correlation(), "beefcafe0001") == 0,
          "thread correlation stored");
    kn_log_simple(KN_CH_APP, KN_INFO, NULL, "corr_default_used");
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/app.log", NULL);
    line = find_line(logbuf, "corr_default_used");
    CHECK(line && strstr(line, "c=beefcafe0001"), "thread-default correlation applied");
    free(line);
    free(logbuf);

    kn_log_set_thread_correlation(NULL);
    kn_log_simple(KN_CH_APP, KN_INFO, NULL, "corr_cleared_marker");
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/app.log", NULL);
    line = find_line(logbuf, "corr_cleared_marker");
    CHECK(line && !strstr(line, " c="), "cleared correlation omitted");
    free(line);
    free(logbuf);

    /* -- session id ------------------------------------------------------ */
    kn_log_set_session_id("sess-42");
    kn_log_simple(KN_CH_MEDIA, KN_INFO, NULL, "session_marker_on");
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/media.log", NULL);
    jsonbuf = kn_test_read_file(DIR1 "/media.jsonl", NULL);
    CHECK_STR_CONTAINS(logbuf, "s=sess-42", "session id in human line");
    CHECK_STR_CONTAINS(jsonbuf, "\"session_id\":\"sess-42\"", "session id in jsonl");
    free(logbuf);
    free(jsonbuf);
    kn_log_set_session_id(NULL);
    kn_log_simple(KN_CH_MEDIA, KN_INFO, NULL, "session_marker_off");
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/media.log", NULL);
    line = find_line(logbuf, "session_marker_off");
    CHECK(line && !strstr(line, " s="), "cleared session id omitted");
    free(line);
    free(logbuf);

    /* -- field overflow is counted, never fatal -------------------------- */
    {
        kn_lctx c;
        kn_lctx_init(&c, KN_CH_APP, KN_INFO, "overflow_test");
        for (i = 0; i < 40; ++i) kn_lctx_i64(&c, "f", i);
        CHECK(c.dropped_fields == 8, "8 fields dropped at capacity");
        kn_log_emit(&c);
    }
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/app.log", NULL);
    line = find_line(logbuf, "overflow_test");
    CHECK(line && strstr(line, "fields_dropped=8"), "fields_dropped rendered");
    free(line);
    free(logbuf);
    kn_log_get_stats(&st);
    CHECK(st.dropped_fields >= 8, "dropped fields counted globally");

    /* -- long value truncation into the arena ----------------------------- */
    {
        kn_lctx c;
        char big[KN_LOG_ARENA + 512];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        kn_lctx_init(&c, KN_CH_APP, KN_INFO, "arena_overflow_test");
        kn_lctx_str(&c, "blob", big);
        kn_log_emit(&c);   /* must not crash, must truncate */
    }
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/app.log", NULL);
    CHECK_STR_CONTAINS(logbuf, "arena_overflow_test", "oversized value survived (truncated)");
    CHECK(logbuf && strstr(logbuf, "blob=~xxx") != NULL, "truncation marker rendered");
    free(logbuf);

    /* -- failure amplification (§7.7) ------------------------------------- */
    CHECK(kn_log_failure_cb_register(fail_cb, NULL) == 0, "failure cb registered");
    kn_log_on_failure("session.create", "CM_HTTP_400", "create rejected",
                      "fix body and retry");
    CHECK(g_cb_calls == 1, "failure callback invoked once");
    CHECK(strcmp(g_cb_op, "session.create") == 0, "callback got op");
    CHECK(strcmp(g_cb_code, "CM_HTTP_400") == 0, "callback got code");
    CHECK(strcmp(g_cb_reason, "create rejected") == 0, "callback got reason");

    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR1 "/app.log", NULL);
    CHECK_STR_CONTAINS(logbuf, "ERROR [app] operation_failed", "failure ERROR line");
    CHECK_STR_CONTAINS(logbuf, "code=CM_HTTP_400", "failure code field");
    CHECK_STR_CONTAINS(logbuf, "next_action=", "failure next_action field");
    free(logbuf);

    CHECK(kn_log_get_level(KN_CH_MEDIA) == KN_DEBUG, "media raised to DEBUG");
    CHECK(kn_log_get_level(KN_CH_TRANSPORT) == KN_TRACE, "transport raised to TRACE");
    kn_log_get_stats(&st);
    CHECK(st.sealed == 1, "sealed after failure");
    CHECK(st.amplified == 1, "amplified after failure");

    /* after amplification, DEBUG events flow without restart */
    kn_log_simple(KN_CH_MEDIA, KN_DEBUG, NULL, "post_amplify_debug");
    logbuf = kn_test_read_file(DIR1 "/media.log", NULL); /* sealed: flushed on write */
    CHECK_STR_CONTAINS(logbuf, "post_amplify_debug", "DEBUG flows after amplification");
    free(logbuf);

    kn_log_get_stats(&st);
    CHECK(st.events_emitted > 0 && st.lines_written > 0 && st.bytes_written > 0,
          "stats populated");
    CHECK(st.write_errors == 0, "no write errors during tests");

    kn_log_shutdown();
    CHECK(kn_log_is_initialized() == 0, "shutdown clears initialized flag");
}

static void test_instance_2_defaults(void)
{
    kn_log_config cfg;
    char *logbuf, *jsonbuf;
    FILE *probe;

    memset(&cfg, 0, sizeof cfg);
    cfg.log_dir = DIR2;                  /* everything else: spec defaults */
    CHECK(kn_log_init(&cfg) == 0, "re-init after shutdown works");

    kn_log_simple(KN_CH_APP, KN_INFO, NULL, "default_mask_app_event");
    kn_log_simple(KN_CH_PERF, KN_INFO, NULL, "default_mask_perf_event");
    kn_log_flush_all();

    probe = fopen(DIR2 "/app.jsonl", "rb");
    CHECK(probe == NULL, "default jsonl mask excludes app");
    if (probe) fclose(probe);
    probe = fopen(DIR2 "/perf.jsonl", "rb");
    CHECK(probe != NULL, "default jsonl mask includes perf");
    if (probe) fclose(probe);

    /* jsonl drops TRACE (jsonl_min_level = DEBUG) but .log keeps it */
    kn_log_set_level(KN_CH_TRANSPORT, KN_TRACE);
    kn_log_simple(KN_CH_TRANSPORT, KN_TRACE, NULL, "trace_not_in_jsonl");
    kn_log_simple(KN_CH_TRANSPORT, KN_DEBUG, NULL, "debug_in_jsonl");
    kn_log_flush_all();
    logbuf = kn_test_read_file(DIR2 "/transport.log", NULL);
    jsonbuf = kn_test_read_file(DIR2 "/transport.jsonl", NULL);
    CHECK_STR_CONTAINS(logbuf, "trace_not_in_jsonl", "TRACE present in human log");
    CHECK_STR_NOT_CONTAINS(jsonbuf, "trace_not_in_jsonl", "TRACE excluded from jsonl");
    CHECK_STR_CONTAINS(jsonbuf, "debug_in_jsonl", "DEBUG present in jsonl");
    free(logbuf);
    free(jsonbuf);

    kn_log_shutdown();
}

static void test_redaction_helpers(void)
{
    char red[32];
    CHECK(kn_redact_key_is_secret("Authorization") == 1, "Authorization secret");
    CHECK(kn_redact_key_is_secret("access_token") == 1, "access_token secret");
    CHECK(kn_redact_key_is_secret("refreshToken") == 1, "refreshToken secret");
    CHECK(kn_redact_key_is_secret("srtpAesKeyHex") == 1, "srtpAesKeyHex secret");
    CHECK(kn_redact_key_is_secret("srtpSaltHex") == 1, "srtpSaltHex secret");
    CHECK(kn_redact_key_is_secret("srtpMasterKey") == 1, "srtpMasterKey secret");
    CHECK(kn_redact_key_is_secret("icePassword") == 1, "icePassword secret");
    CHECK(kn_redact_key_is_secret("ice_pwd") == 1, "ice_pwd (password substring)");
    CHECK(kn_redact_key_is_secret("user_code") == 1, "user_code secret");
    CHECK(kn_redact_key_is_secret("device_code") == 1, "device_code secret");
    CHECK(kn_redact_key_is_secret("dtlsFingerprint") == 1, "fingerprint secret");
    CHECK(kn_redact_key_is_secret("client_secret") == 1, "client_secret secret");
    CHECK(kn_redact_key_is_secret("Content-Type") == 0, "Content-Type not secret");
    CHECK(kn_redact_key_is_secret("nv-client-id") == 0, "nv-client-id not secret");
    CHECK(kn_redact_key_is_secret("queuePosition") == 0, "queuePosition not secret");
    CHECK(kn_redact_key_is_secret("master_key_len") == 0, "lengths are not secrets");

    kn_redact_value(red, sizeof red, "abc123");
    CHECK(strcmp(red, "<redacted:6>") == 0, "redacted value format");
    kn_redact_value(red, sizeof red, NULL);
    CHECK(strcmp(red, "<redacted:0>") == 0, "NULL value -> length 0");

    CHECK(kn_redact_header_allowed("User-Agent") == 1, "User-Agent whitelisted");
    CHECK(kn_redact_header_allowed("content-type") == 1, "content-type whitelisted");
    CHECK(kn_redact_header_allowed("NV-Client-Type") == 1, "case-insensitive whitelist");
    CHECK(kn_redact_header_allowed("Authorization") == 0, "Authorization not whitelisted");
    CHECK(kn_redact_header_allowed("Cookie") == 0, "Cookie not whitelisted");
    CHECK(kn_redact_allow_header("x-nv-ping") == 0, "extra header registered");
    CHECK(kn_redact_header_allowed("X-NV_PING") == 1, "registered header matches");

    CHECK(strcmp(kn_redact_auth_scheme("GFNJWT abc.def.ghi"), "GFNJWT") == 0,
          "GFNJWT scheme extracted");
    CHECK(strcmp(kn_redact_auth_scheme("Bearer ya29.xxx"), "Bearer") == 0,
          "Bearer scheme extracted");
    CHECK(strcmp(kn_redact_auth_scheme("weird value"), "Unknown") == 0,
          "unknown scheme");
    CHECK(strcmp(kn_redact_auth_scheme(NULL), "None") == 0, "NULL scheme");
}

int main(void)
{
    int keep = getenv("KN_TEST_KEEP") != NULL;

    /* Always start from a clean state; KN_TEST_KEEP only preserves the
     * directories AFTER the run (for tools/check_jsonl.py inspection). */
    cleanup_dir(DIR1);
    cleanup_dir(DIR2);
    test_redaction_helpers();   /* pure, no init needed */
    test_init_bad_dir();
    test_instance_1();
    test_instance_2_defaults();
    if (!keep) { cleanup_dir(DIR1); cleanup_dir(DIR2); }
    return kn_test_summary("test_knlog");
}
