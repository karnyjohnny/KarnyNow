/* test_kndiag.c — unit tests for the diagnostic bundle writer (kn_diag).
 *
 * Includes a minimal STORE-ONLY zip reader (EOCD scan -> central directory ->
 * local headers -> CRC verification) so the tests prove the archive is a
 * valid zip without any external tool. On the host, python's zipfile is
 * additionally run over a produced bundle by tools/check_zip.py as a second,
 * independent opinion.
 *
 * Covers:
 *   1. crc32 known-answer ("123456789" -> 0xCBF43926)
 *   2. basic bundle: summary.json is the FIRST entry, entries readable, CRCs ok
 *   3. summary.json content: trigger, verdicts, artifact table
 *   4. cap enforcement: SECONDARY dropped whole first, PRIMARY tail-kept
 *      with marker, bundle <= cap, CORE untouched
 *   5. add_file tail-keep for oversized sources + duplicate rejection
 *   6. collect_logs: generation classification (app.log PRIMARY vs app.1.log
 *      SECONDARY) and deterministic name order
 *   7. build.json: effective-macro strings present, valid JSON shape
 *   8. failure-hook integration: kn_log_on_failure -> bundle exists,
 *      kn_diag_last_bundle_path set, recursion guard survives a second failure
 *   9. empty bundle refusal (-2) and NULL-arg handling (-1)
 */
#include "kn/kn_diag.h"
#include "kn/knlog.h"

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

/* ------------------------------------------------------------- zip reader */

static unsigned char *slurp(const char *path, size_t *out_len)
{
    return (unsigned char *)kn_test_read_file(path, out_len);
}

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Find the EOCD; returns its offset or (size_t)-1. */
static size_t find_eocd(const unsigned char *z, size_t len)
{
    size_t i;
    size_t start = len > 22u + 0xFFFFu ? len - (22u + 0xFFFFu) : 0;
    if (len < 22) return (size_t)-1;
    for (i = len - 22; ; --i) {
        if (rd32(z + i) == 0x06054b50u) return i;
        if (i <= start) break;
    }
    return (size_t)-1;
}

typedef struct {
    char     name[KN_DIAG_NAME_MAX];
    uint32_t crc, size, offset;
} zip_entry;

/* Parse the central directory into entries[]. Returns count or -1. */
static int zip_list(const unsigned char *z, size_t len, zip_entry *ents, int max)
{
    size_t eo = find_eocd(z, len);
    uint16_t n, i;
    uint32_t cd_off;
    size_t p;
    if (eo == (size_t)-1) return -1;
    n = rd16(z + eo + 10);
    cd_off = rd32(z + eo + 16);
    if (cd_off >= len) return -1;
    p = cd_off;
    for (i = 0; i < n && (int)i < max; ++i) {
        uint16_t nlen, elen, clen;
        if (p + 46 > len || rd32(z + p) != 0x02014b50u) return -1;
        nlen = rd16(z + p + 28);
        elen = rd16(z + p + 30);
        clen = rd16(z + p + 32);
        ents[i].crc = rd32(z + p + 16);
        ents[i].size = rd32(z + p + 24);
        ents[i].offset = rd32(z + p + 42);
        if (nlen >= KN_DIAG_NAME_MAX) return -1;
        memcpy(ents[i].name, z + p + 46, nlen);
        ents[i].name[nlen] = '\0';
        p += 46u + nlen + elen + clen;
    }
    return (int)n;
}

/* Extract one entry by name into a malloc'd NUL-terminated buffer.
 * Verifies the local header and the CRC. Returns NULL on any problem. */
static char *zip_extract(const unsigned char *z, size_t len, const char *name,
                         size_t *out_len)
{
    zip_entry ents[KN_DIAG_MAX_ARTIFACTS + 2];
    int n = zip_list(z, len, ents, KN_DIAG_MAX_ARTIFACTS + 2);
    int i;
    if (n < 0) return NULL;
    for (i = 0; i < n; ++i) {
        uint16_t nlen, elen;
        size_t data_at;
        char *buf;
        if (strcmp(ents[i].name, name) != 0) continue;
        if (ents[i].offset + 30 > len) return NULL;
        if (rd32(z + ents[i].offset) != 0x04034b50u) return NULL;
        if (rd16(z + ents[i].offset + 8) != 0) return NULL;      /* method: store */
        nlen = rd16(z + ents[i].offset + 26);
        elen = rd16(z + ents[i].offset + 28);
        data_at = ents[i].offset + 30u + nlen + elen;
        if (data_at + ents[i].size > len) return NULL;
        if (kn_diag_crc32(0, z + data_at, ents[i].size) != ents[i].crc) return NULL;
        buf = (char *)malloc(ents[i].size + 1);
        if (!buf) return NULL;
        memcpy(buf, z + data_at, ents[i].size);
        buf[ents[i].size] = '\0';
        if (out_len) *out_len = ents[i].size;
        return buf;
    }
    return NULL;
}

/* ---------------------------------------------------------------- helpers */

#define TESTDIR "testdiag"

static void write_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(content, f); fclose(f); }
}

static void write_blob_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

static void make_pattern(char *buf, size_t len, char seed)
{
    size_t i;
    for (i = 0; i < len; ++i) buf[i] = (char)('a' + ((seed + i / 64) % 26));
}

static long long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fclose(f);
    return sz;
}

/* Remove every file in dir, then the dir itself. */
static void purge_dir(const char *dir)
{
#ifdef _WIN32
    char pattern[KN_DIAG_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char p[KN_DIAG_PATH_MAX];
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
            char p[KN_DIAG_PATH_MAX];
            if (de->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
            remove(p);
        }
        closedir(dp);
    }
#endif
    kn_test_rmdir(dir);
}

/* ------------------------------------------------------------------ tests */

static void test_crc32_kat(void)
{
    uint32_t c;
    CHECK(kn_diag_crc32(0, "123456789", 9) == 0xCBF43926u, "crc32 KAT 123456789");
    CHECK(kn_diag_crc32(0, "", 0) == 0x00000000u, "crc32 empty");
    c = kn_diag_crc32(0, "1234", 4);
    c = kn_diag_crc32(c, "56789", 5);
    CHECK(c == 0xCBF43926u, "crc32 incremental");
}

static void test_basic_bundle(void)
{
    kn_diag_bundle *b;
    char out[KN_DIAG_PATH_MAX];
    unsigned char *z;
    size_t zlen = 0, slen = 0;
    char *summary, *got;
    zip_entry ents[16];
    int n, rc;
    const char *payload_a = "hello bundle\n";
    const char *payload_b = "{\"x\":1}\n";

    test_mkdir(TESTDIR);
    b = kn_diag_begin(TESTDIR, 0);
    CHECK(b != NULL, "begin returns bundle");
    if (!b) return;
    CHECK(kn_diag_add_mem(b, "wire/test.txt", payload_a, strlen(payload_a),
                          KN_DIAG_PRIO_PRIMARY) == 0, "add_mem ok");
    CHECK(kn_diag_add_mem(b, "selftest.json", payload_b, strlen(payload_b),
                          KN_DIAG_PRIO_CORE) == 0, "add_mem core ok");
    CHECK(kn_diag_add_mem(b, "wire/test.txt", "dup", 3, KN_DIAG_PRIO_PRIMARY) == -2,
          "duplicate rejected");
    CHECK(kn_diag_verdict(b, "unit.test", KN_DIAG_OK, "all good", "wire/test.txt") == 0,
          "verdict recorded");
    CHECK(kn_diag_has(b, "wire/test.txt") == 1, "has() finds artifact");
    CHECK(kn_diag_has(b, "nope.txt") == 0, "has() misses unknown");

    out[0] = '\0';
    rc = kn_diag_finish(b, "unit.op", "E_TEST", "deliberate test failure",
                        out, sizeof out);
    CHECK(rc == 0, "finish ok");
    CHECK(out[0] != '\0', "out_path filled");
    CHECK(strstr(out, "diag-") != NULL, "bundle name pattern");
    CHECK(strstr(out, ".zip") != NULL, "bundle is .zip");
    kn_diag_abort(b);

    z = slurp(out, &zlen);
    CHECK(z != NULL && zlen > 22, "zip file readable");
    if (!z) { remove(out); purge_dir(TESTDIR); return; }

    n = zip_list(z, zlen, ents, 16);
    CHECK(n == 3, "3 entries (summary + 2 artifacts)");
    if (n == 3) {
        CHECK(strcmp(ents[0].name, "summary.json") == 0, "summary.json is FIRST entry");
        CHECK(ents[0].offset == 0, "summary at offset 0");
    }

    summary = zip_extract(z, zlen, "summary.json", &slen);
    CHECK(summary != NULL, "summary extractable + CRC ok");
    if (summary) {
        CHECK(strstr(summary, "\"bundle_version\": 1") != NULL, "summary version");
        CHECK(strstr(summary, "\"op\": \"unit.op\"") != NULL, "summary trigger op");
        CHECK(strstr(summary, "\"code\": \"E_TEST\"") != NULL, "summary trigger code");
        CHECK(strstr(summary, "deliberate test failure") != NULL, "summary trigger reason");
        CHECK(strstr(summary, "\"subsystem\": \"unit.test\"") != NULL, "summary verdict subsystem");
        CHECK(strstr(summary, "\"status\": \"ok\"") != NULL, "summary verdict status");
        CHECK(strstr(summary, "wire/test.txt") != NULL, "summary lists artifact");
        CHECK(strstr(summary, "\"bytes\": 13") != NULL, "summary artifact bytes");
        free(summary);
    }

    got = zip_extract(z, zlen, "wire/test.txt", &slen);
    CHECK(got != NULL, "artifact A extractable + CRC ok");
    if (got) {
        CHECK(strcmp(got, payload_a) == 0, "artifact A content intact");
        free(got);
    }
    got = zip_extract(z, zlen, "selftest.json", &slen);
    CHECK(got != NULL, "artifact B extractable + CRC ok");
    if (got) {
        CHECK(strcmp(got, payload_b) == 0, "artifact B content intact");
        free(got);
    }

    free(z);
    remove(out);
    purge_dir(TESTDIR);
}

static void test_cap_enforcement(void)
{
    /* cap 256 KiB; three PRIMARY x 100 KiB + two SECONDARY x 100 KiB.
     * Expected: SECONDARY dropped whole (lowest class first), the largest
     * PRIMARY halved (>= 2x MIN_KEEP so halving strictly decreases) until the
     * bundle fits the finish target (cap - summary margin), CORE untouched. */
    const size_t cap = 256u * 1024u;
    const size_t big = 100u * 1024u;
    kn_diag_bundle *b = kn_diag_begin(TESTDIR, cap);
    char *blob = (char *)malloc(big);
    char out[KN_DIAG_PATH_MAX];
    unsigned char *z;
    size_t zlen = 0, slen = 0;
    char *summary, *kept;
    long long sz;
    int rc;

    CHECK(b != NULL && blob != NULL, "cap test setup");
    if (!b || !blob) { free(blob); kn_diag_abort(b); return; }
    test_mkdir(TESTDIR);

    make_pattern(blob, big, 'p');
    CHECK(kn_diag_add_mem(b, "wire/primary1.txt", blob, big, KN_DIAG_PRIO_PRIMARY) == 0,
          "primary1 added");
    make_pattern(blob, big, 'q');
    CHECK(kn_diag_add_mem(b, "wire/primary2.txt", blob, big, KN_DIAG_PRIO_PRIMARY) == 0,
          "primary2 added");
    make_pattern(blob, big, 'r');
    CHECK(kn_diag_add_mem(b, "wire/primary3.txt", blob, big, KN_DIAG_PRIO_PRIMARY) == 0,
          "primary3 added");
    make_pattern(blob, big, 's');
    CHECK(kn_diag_add_mem(b, "logs/app.1.log", blob, big, KN_DIAG_PRIO_SECONDARY) == 0,
          "secondary1 added");
    make_pattern(blob, big, 't');
    CHECK(kn_diag_add_mem(b, "logs/app.2.log", blob, big, KN_DIAG_PRIO_SECONDARY) == 0,
          "secondary2 added");
    CHECK(kn_diag_add_mem(b, "state.json", "{\"machines\":[]}", 15, KN_DIAG_PRIO_CORE) == 0,
          "core added");
    free(blob);

    rc = kn_diag_finish(b, "cap.test", "E_CAP", "cap enforcement test", out, sizeof out);
    CHECK(rc == 0, "cap: finish ok");
    kn_diag_abort(b);

    sz = file_size(out);
    CHECK(sz > 0, "cap: zip written");
    CHECK(sz <= (long long)cap, "cap: bundle within cap");

    z = slurp(out, &zlen);
    CHECK(z != NULL, "cap: zip readable");
    if (z) {
        summary = zip_extract(z, zlen, "summary.json", &slen);
        CHECK(summary != NULL, "cap: summary intact");
        if (summary) {
            CHECK(strstr(summary, "logs/app.1.log") != NULL, "cap: secondary1 listed");
            CHECK(strstr(summary, "\"dropped\": true") != NULL, "cap: drops reported");
            CHECK(strstr(summary, "\"truncated\": true") != NULL, "cap: truncation reported");
            CHECK(strstr(summary, "\"over_cap\": false") != NULL, "cap: not over cap");
            free(summary);
        }
        kept = zip_extract(z, zlen, "state.json", &slen);
        CHECK(kept != NULL, "cap: CORE artifact present");
        if (kept) {
            CHECK(strcmp(kept, "{\"machines\":[]}") == 0, "cap: CORE content intact");
            free(kept);
        }
        /* every surviving PRIMARY either kept whole or carries the marker */
        kept = zip_extract(z, zlen, "wire/primary1.txt", &slen);
        if (kept) {
            CHECK(slen == big || strstr(kept, "[kn_diag] truncated") == kept,
                  "cap: primary1 whole-or-marked");
            free(kept);
        }
        kept = zip_extract(z, zlen, "wire/primary3.txt", &slen);
        if (kept) {
            CHECK(slen == big || strstr(kept, "[kn_diag] truncated") == kept,
                  "cap: primary3 whole-or-marked");
            free(kept);
        }
        free(z);
    }
    remove(out);
    purge_dir(TESTDIR);
}

static void test_add_file_and_tail(void)
{
    kn_diag_bundle *b = kn_diag_begin(TESTDIR, 64u * 1024u); /* small cap */
    char path[KN_DIAG_PATH_MAX];
    char out[KN_DIAG_PATH_MAX];
    char *big = (char *)malloc(128u * 1024u);
    unsigned char *z;
    size_t zlen = 0, slen = 0;
    char *got;
    long long sz;

    CHECK(b != NULL && big != NULL, "add_file test setup");
    if (!b || !big) { free(big); kn_diag_abort(b); return; }
    test_mkdir(TESTDIR);

    snprintf(path, sizeof path, "%s/big.txt", TESTDIR);
    make_pattern(big, 128u * 1024u, 'z');
    /* unique sentinel at the very end: proves the NEWEST bytes survive both
     * the file-tail keep and any cap-pass halving */
    big[128u * 1024u - 3] = 'Z';
    big[128u * 1024u - 2] = '!';
    big[128u * 1024u - 1] = '\n';
    write_blob_file(path, big, 128u * 1024u);
    free(big);

    /* source (128 KiB) > per-file ceiling (cap = 64 KiB) -> tail-keep */
    CHECK(kn_diag_add_file(b, "wire/big.txt", path, KN_DIAG_PRIO_PRIMARY) == 0,
          "add_file oversized ok");
    CHECK(kn_diag_add_file(b, "wire/missing.txt", TESTDIR "/nope.txt",
                           KN_DIAG_PRIO_PRIMARY) == -1, "add_file missing -> -1");

    CHECK(kn_diag_finish(b, NULL, NULL, NULL, out, sizeof out) == 0,
          "add_file: finish ok (NULL trigger = manual)");
    kn_diag_abort(b);

    sz = file_size(out);
    CHECK(sz > 0 && sz <= 64 * 1024 + 2048, "add_file: within cap (+ zip bookkeeping)");

    z = slurp(out, &zlen);
    CHECK(z != NULL, "add_file: zip readable");
    if (z) {
        got = zip_extract(z, zlen, "wire/big.txt", &slen);
        CHECK(got != NULL, "add_file: tail-kept artifact readable");
        if (got) {
            /* possibly two stacked markers (file-tail marker, then cap-pass
             * halving marker); the first marker may itself be halved away —
             * what MUST hold: a marker opens the data and the newest bytes
             * (the sentinel) survive at the end */
            CHECK(strstr(got, "[kn_diag]") == got, "add_file: marker is first line");
            CHECK(slen >= 3 && strcmp(got + slen - 3, "Z!\n") == 0,
                  "add_file: newest bytes (sentinel) survived");
            free(got);
        }
        got = zip_extract(z, zlen, "summary.json", &slen);
        CHECK(got != NULL && strstr(got, "\"op\": \"manual\"") != NULL,
              "add_file: NULL trigger renders as manual");
        free(got);
        free(z);
    }
    remove(path);
    remove(out);
    purge_dir(TESTDIR);
}

static void test_collect_logs(void)
{
    kn_diag_bundle *b;
    char path[KN_DIAG_PATH_MAX], out[KN_DIAG_PATH_MAX];
    unsigned char *z;
    size_t zlen = 0, slen = 0;
    char *summary;
    int added;

    purge_dir(TESTDIR);
    test_mkdir(TESTDIR);
    snprintf(path, sizeof path, "%s/app.log", TESTDIR);
    write_text_file(path, "current app line\n");
    snprintf(path, sizeof path, "%s/app.1.log", TESTDIR);
    write_text_file(path, "rotated app line\n");
    snprintf(path, sizeof path, "%s/transport.jsonl", TESTDIR);
    write_text_file(path, "{\"ts\":\"x\"}\n");
    snprintf(path, sizeof path, "%s/notes.txt", TESTDIR);
    write_text_file(path, "not a log\n");

    b = kn_diag_begin(TESTDIR, 0);
    CHECK(b != NULL, "collect: begin ok");
    if (!b) { purge_dir(TESTDIR); return; }
    added = kn_diag_collect_logs(b, TESTDIR);
    CHECK(added == 3, "collect: 3 log artifacts (txt ignored)");
    CHECK(kn_diag_has(b, "logs/app.log") == 1, "collect: app.log present");
    CHECK(kn_diag_has(b, "logs/app.1.log") == 1, "collect: app.1.log present");
    CHECK(kn_diag_has(b, "logs/transport.jsonl") == 1, "collect: transport.jsonl present");
    CHECK(kn_diag_has(b, "logs/notes.txt") == 0, "collect: non-log ignored");

    CHECK(kn_diag_finish(b, "collect.test", NULL, NULL, out, sizeof out) == 0,
          "collect: finish ok");
    kn_diag_abort(b);

    z = slurp(out, &zlen);
    CHECK(z != NULL, "collect: zip readable");
    if (z) {
        char *e = zip_extract(z, zlen, "logs/app.1.log", &slen);
        CHECK(e != NULL && strcmp(e, "rotated app line\n") == 0,
              "collect: rotated content intact");
        free(e);
        summary = zip_extract(z, zlen, "summary.json", &slen);
        CHECK(summary != NULL, "collect: summary present");
        if (summary) {
            /* name-sorted artifact order: app.1.log < app.log < transport.jsonl */
            char *p1 = strstr(summary, "logs/app.1.log");
            char *p2 = strstr(summary, "logs/app.log");
            char *p3 = strstr(summary, "logs/transport.jsonl");
            CHECK(p1 && p2 && p3 && p1 < p2 && p2 < p3, "collect: deterministic sorted order");
            CHECK(strstr(summary, "\"prio\": 2") != NULL, "collect: rotated = SECONDARY");
            CHECK(strstr(summary, "\"prio\": 1") != NULL, "collect: current = PRIMARY");
            free(summary);
        }
        free(z);
    }
    remove(out);
    purge_dir(TESTDIR);
}

static void test_build_json(void)
{
    char buf[4096];
    size_t n = kn_diag_render_build_json(buf, sizeof buf);
    CHECK(n > 2 && n < sizeof buf, "build.json rendered");
    if (n <= 2) return;
    CHECK(strstr(buf, "\"git_rev\":") != NULL, "build.json git_rev");
    CHECK(strstr(buf, "\"compiler\":") != NULL, "build.json compiler");
    CHECK(strstr(buf, "\"flags\":") != NULL, "build.json flags");
    CHECK(strstr(buf, "\"win32_winnt_effective\":") != NULL, "build.json winnt");
#ifdef _WIN32
    CHECK(strstr(buf, "0x0601") != NULL, "build.json records effective 0x0601");
#endif
    CHECK(strstr(buf, "\"arch\":") != NULL, "build.json arch");
    CHECK(strstr(buf, "Cauchy") != NULL, "build.json libs mention FEC provenance");
    CHECK(buf[0] == '{', "build.json opens brace");
    CHECK(buf[n - 2] == '}' && buf[n - 1] == '\n', "build.json closes brace");
}

static void test_failure_hook_integration(void)
{
    kn_log_config cfg;
    const char *last;
    kn_log_stats st;
    size_t zlen = 0, slen = 0, ll = 0;
    unsigned char *z;
    char *summary, *logtext;
    /* KN_TEST_KEEP=1 preserves the first bundle in testdiag/ for external
     * inspection (tools/check_zip.py — python zipfile as the independent
     * second opinion, same pattern as test_knlog + check_jsonl.py). */
    int keep = getenv("KN_TEST_KEEP") != NULL;

    purge_dir(TESTDIR);
    memset(&cfg, 0, sizeof cfg);
    cfg.log_dir = TESTDIR;
    CHECK(kn_log_init(&cfg) == 0, "hook: log init");
    CHECK(kn_diag_install(TESTDIR) == 0, "hook: install ok");
    CHECK(kn_diag_install(TESTDIR) == 0, "hook: install idempotent");

    kn_log_simple(KN_CH_APP, KN_INFO, NULL, "before_failure");

    /* the real thing: ERROR -> amplification -> hook -> bundle on disk */
    kn_log_on_failure("hook.test", "E_HOOK", "integration test failure",
                      "read the bundle");
    last = kn_diag_last_bundle_path();
    CHECK(last != NULL, "hook: last bundle path set");
    if (last) {
        long long sz = file_size(last);
        CHECK(sz > 22, "hook: bundle file exists and is non-trivial");
        z = slurp(last, &zlen);
        CHECK(z != NULL, "hook: bundle readable");
        if (z) {
            summary = zip_extract(z, zlen, "summary.json", &slen);
            CHECK(summary != NULL, "hook: bundle has valid summary");
            if (summary) {
                CHECK(strstr(summary, "\"code\": \"E_HOOK\"") != NULL,
                      "hook: trigger recorded in summary");
                CHECK(strstr(summary, "logs/app.log") != NULL,
                      "hook: logs collected into bundle");
                CHECK(strstr(summary, "\"subsystem\": \"logging\"") != NULL,
                      "hook: logging verdict present");
                free(summary);
            }
            summary = zip_extract(z, zlen, "build.json", &slen);
            CHECK(summary != NULL && strstr(summary, "git_rev") != NULL,
                  "hook: build.json auto-added");
            free(summary);
            summary = zip_extract(z, zlen, "state.json", &slen);
            CHECK(summary != NULL && strstr(summary, "\"sealed\":1") != NULL &&
                  strstr(summary, "\"amplified\":1") != NULL,
                  "hook: state.json shows sealed+amplified");
            free(summary);
            free(z);
        }
        /* the ERROR itself must be inside the sealed logs on disk */
        logtext = kn_test_read_file(TESTDIR "/app.log", &ll);
        CHECK(logtext != NULL && strstr(logtext, "E_HOOK") != NULL,
              "hook: ERROR event on disk");
        free(logtext);
        logtext = kn_test_read_file(TESTDIR "/diag.log", &ll);
        CHECK(logtext != NULL && strstr(logtext, "bundle_written") != NULL,
              "hook: bundle_written event logged on diag channel");
        free(logtext);
        if (!keep) remove(last);
    }

    /* recursion guard + second failure: must not loop, must produce a bundle */
    kn_log_on_failure("hook.test2", "E_HOOK2", "second failure", "none");
    last = kn_diag_last_bundle_path();
    CHECK(last != NULL, "hook: second bundle path");
    if (last) {
        CHECK(strstr(last, TESTDIR) == last, "hook: bundle lives in the log dir");
        remove(last);   /* always remove the second one */
    }

    kn_log_get_stats(&st);
    CHECK(st.sealed == 1 && st.amplified == 1, "hook: log sealed+amplified");
    kn_log_shutdown();
    if (!keep) purge_dir(TESTDIR);
}

static void test_edge_cases(void)
{
    kn_diag_bundle *b = kn_diag_begin(NULL, 0);   /* defaults */
    kn_diag_bundle *b2;
    char out[KN_DIAG_PATH_MAX];
    CHECK(b != NULL, "edge: defaults ok");
    if (b) {
        CHECK(kn_diag_finish(b, NULL, NULL, NULL, out, sizeof out) == -2,
              "edge: empty bundle refused");
        kn_diag_abort(b);
    }
    kn_diag_abort(NULL);                          /* must not crash */
    CHECK(kn_diag_add_mem(NULL, "a", "b", 1, 0) == -1, "edge: NULL bundle add");
    CHECK(kn_diag_verdict(NULL, "s", KN_DIAG_OK, NULL, NULL) == -1, "edge: NULL verdict");
    CHECK(kn_diag_collect_logs(NULL, "x") == -1, "edge: NULL collect");

    b2 = kn_diag_begin("nonexistent-dir-xyz", 0);
    CHECK(b2 != NULL, "edge: begin does not touch disk");
    if (b2) {
        CHECK(kn_diag_add_mem(b2, "a.txt", "x", 1, KN_DIAG_PRIO_CORE) == 0, "edge: add ok");
        CHECK(kn_diag_finish(b2, NULL, NULL, NULL, out, sizeof out) == -3,
              "edge: unwritable dir -> -3");
        kn_diag_abort(b2);
    }

    b = kn_diag_begin(TESTDIR, 1);   /* absurd cap -> floored to 64 KiB */
    CHECK(b != NULL, "edge: tiny cap floored");
    kn_diag_abort(b);
}

int main(void)
{
    test_crc32_kat();
    test_basic_bundle();
    test_cap_enforcement();
    test_add_file_and_tail();
    test_collect_logs();
    test_build_json();
    test_edge_cases();
    test_failure_hook_integration();
    return kn_test_summary("test_kndiag");
}
