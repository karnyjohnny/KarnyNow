/* knlog.c — implementation of the KarnyNow structured logging subsystem.
 * Spec: docs/BOOTSTRAP.md §7 (D13). See knlog.h for the contract and disk budget.
 *
 * Locking model: field building (kn_lctx_*) is lock-free (caller-owned stack
 * context). kn_log_emit renders both lines into caller-local buffers, then
 * takes the mutex only for sink write/rotate/flush and stats — short,
 * bounded, no allocation under the lock.
 */
#include "kn/knlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ platform shim */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <sys/types.h>
#  include <sys/stat.h>
typedef CRITICAL_SECTION kn_mutex;
static void kn_mutex_init(kn_mutex *m)    { InitializeCriticalSection(m); }
static void kn_mutex_destroy(kn_mutex *m) { DeleteCriticalSection(m); }
static void kn_mutex_lock(kn_mutex *m)    { EnterCriticalSection(m); }
static void kn_mutex_unlock(kn_mutex *m)  { LeaveCriticalSection(m); }
static unsigned long kn_thread_id(void)   { return (unsigned long)GetCurrentThreadId(); }
static unsigned long kn_process_id(void)  { return (unsigned long)GetCurrentProcessId(); }
static int kn_fsync(FILE *f)              { int fd = _fileno(f); return fd >= 0 ? _commit(fd) : -1; }
static long long kn_file_size(const char *path)
{
    struct _stat64 st;
    return _stat64(path, &st) == 0 ? (long long)st.st_size : 0;
}
static int kn_file_exists(const char *path)
{
    struct _stat64 st;
    return _stat64(path, &st) == 0;
}
/* Wall clock -> unix ms. GetSystemTimePreciseAsFileTime (Win8+) when present,
 * else GetSystemTimeAsFileTime (~15.6 ms granularity on Win7). */
typedef void (WINAPI *kn_gstpaft_t)(LPFILETIME);
static kn_gstpaft_t s_gstpaft;
static LARGE_INTEGER s_qpc_freq;
static long long kn_wall_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    if (s_gstpaft) s_gstpaft(&ft); else GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100 ns ticks since 1601-01-01 -> ms since 1970-01-01 */
    return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
}
static long long kn_mono_ms(void)
{
    LARGE_INTEGER pc;
    if (!s_qpc_freq.QuadPart) QueryPerformanceFrequency(&s_qpc_freq);
    QueryPerformanceCounter(&pc);
    if (!s_qpc_freq.QuadPart) return 0;
    return (long long)((double)pc.QuadPart * 1000.0 / (double)s_qpc_freq.QuadPart);
}
static int kn_mkdir(const char *path)
{
    if (CreateDirectoryA(path, NULL)) return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}
#else /* POSIX — host-side unit tests */
#  include <errno.h>
#  include <pthread.h>
#  include <time.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
typedef pthread_mutex_t kn_mutex;
static void kn_mutex_init(kn_mutex *m)    { pthread_mutex_init(m, NULL); }
static void kn_mutex_destroy(kn_mutex *m) { pthread_mutex_destroy(m); }
static void kn_mutex_lock(kn_mutex *m)    { pthread_mutex_lock(m); }
static void kn_mutex_unlock(kn_mutex *m)  { pthread_mutex_unlock(m); }
static unsigned long kn_thread_id(void)   { return (unsigned long)(uintptr_t)pthread_self(); }
static unsigned long kn_process_id(void)  { return (unsigned long)getpid(); }
static int kn_fsync(FILE *f)              { int fd = fileno(f); return fd >= 0 ? fsync(fd) : -1; }
static long long kn_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : 0;
}
static int kn_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}
static long long kn_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
static long long kn_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
static int kn_mkdir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1; }
#endif

/* ---------------------------------------------------------------- constants */

#define KN_LINE_BUF        4096  /* max rendered human line */
#define KN_JSON_BUF        6144  /* max rendered jsonl line */
#define KN_PATH_MAX         512
#define KN_MAX_FAILURE_CBS    8
#define KN_MAX_ALLOWED_HDRS  32
#define KN_SESSION_ID_MAX    64

static const char *const k_level_names[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR" };
static const char *const k_channel_names[KN_CH__COUNT] = {
    "app", "http", "rtsp", "transport", "media", "input", "perf", "diag"
};
static const unsigned k_default_jsonl_mask =
    (1u << KN_CH_PERF) | (1u << KN_CH_TRANSPORT);

/* ------------------------------------------------------------------- sinks */

typedef struct {
    FILE       *f;
    char        base[KN_PATH_MAX]; /* "<dir>/<channel>" without extension */
    const char *ext;               /* ".log" or ".jsonl" */
    long long   bytes;             /* current file size estimate */
    int         dead;              /* open/write failed; sink disabled */
} kn_sink;

typedef struct {
    kn_sink log;   /* human */
    kn_sink jsonl; /* structured; f == NULL when channel not in jsonl mask */
} kn_channel_sinks;

/* ------------------------------------------------------------- global state */

static struct {
    int               inited;
    kn_mutex          mtx;
    kn_log_config     cfg;  /* normalized; string members must NOT be used after init */
    char              dir[KN_PATH_MAX];
    char              build_stamp[128];
    char              version[64];
    kn_level          levels[KN_CH__COUNT];
    unsigned          jsonl_mask;
    kn_channel_sinks  sinks[KN_CH__COUNT];
    char              session_id[KN_SESSION_ID_MAX];
    int               have_session_id;
    int               sealed;      /* flush+fsync every event (§7.7) */
    int               amplified;
    kn_log_stats      stats;
    kn_failure_cb     cbs[KN_MAX_FAILURE_CBS];
    void             *cb_ud[KN_MAX_FAILURE_CBS];
    int               n_cbs;
    uint64_t          rng;                 /* correlation PRNG state */
    unsigned long long corr_counter;
    long long         mono_epoch_ms;
    int               mono_epoch_set;
} G;

static char g_allowed_headers[KN_MAX_ALLOWED_HDRS][64];
static int  g_n_allowed_headers;

/* Thread-default correlations. A small fixed table keyed by thread id
 * instead of TLS: fully portable (no __declspec(thread)/_Thread_local
 * dialect issues), bounded memory, and the streamer has <16 threads.
 * Each thread only ever writes its own entry; readers of other entries
 * see stable storage, so emit can look up without the mutex. */
#define KN_TID_SLOTS 16
static struct {
    unsigned long tid;
    char          corr[KN_CORR_LEN + 1];
    int           used;
} g_tid_corr[KN_TID_SLOTS];
static unsigned g_tid_rr;

static const char *thread_corr_lookup(void)
{
    unsigned long tid = kn_thread_id();
    int i;
    for (i = 0; i < KN_TID_SLOTS; ++i)
        if (g_tid_corr[i].used && g_tid_corr[i].tid == tid)
            return g_tid_corr[i].corr;
    return NULL;
}

/* ----------------------------------------------------------------- helpers */

static const char *level_name(kn_level l)
{
    int i = (int)l;
    return (i >= KN_TRACE && i <= KN_ERROR) ? k_level_names[i - KN_TRACE] : "INFO";
}

static const char *channel_name(kn_channel c)
{
    int i = (int)c;
    return (i >= 0 && i < KN_CH__COUNT) ? k_channel_names[i] : "app";
}

/* Howard Hinnant's civil_from_days — no gmtime, no locale, no TZ database. */
static void civil_from_days(long long z, int *yy, unsigned *mm, unsigned *dd)
{
    long long era, y;
    unsigned long long doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned long long)(z - era * 146097);                 /* [0,146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  /* [0,399] */
    y = (long long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                /* [0,365] */
    mp = (5 * doy + 2) / 153;                                     /* [0,11] */
    *dd = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *mm = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *yy = (int)(y + (*mm <= 2 ? 1 : 0));
}

/* RFC 3339 UTC, millisecond resolution: 2026-10-21T14:03:22.481Z */
static void format_ts(long long unix_ms, char out[32])
{
    long long days = unix_ms / 86400000LL;
    long long rem  = unix_ms % 86400000LL;
    int y;
    unsigned m, d, hh, mi, ss, ms;
    if (rem < 0) { rem += 86400000LL; days -= 1; }
    ms = (unsigned)(rem % 1000LL);
    hh = (unsigned)(rem / 3600000LL);
    mi = (unsigned)((rem / 60000LL) % 60ULL);
    ss = (unsigned)((rem / 1000LL) % 60ULL);
    civil_from_days(days, &y, &m, &d);
    snprintf(out, 32, "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", y, m, d, hh, mi, ss, ms);
}

/* Bounded cursor-append: never lets the cursor run past the buffer, so the
 * next (dstsz - o) cannot underflow. Truncation is recorded in *trunc. */
#define KN_APP(buf, bufsz, o, trunc, ...)                              \
    do {                                                               \
        if ((o) < (bufsz)) {                                           \
            int n_ = snprintf((buf) + (o), (bufsz) - (o), __VA_ARGS__);\
            if (n_ < 0) n_ = 0;                                        \
            if ((size_t)n_ >= (bufsz) - (o)) {                         \
                (o) = (bufsz) - 1; *(trunc) = 1;                       \
            } else {                                                   \
                (o) += (size_t)n_;                                     \
            }                                                          \
        }                                                              \
    } while (0)

/* JSON string escaping into dst (bounded, always NUL-terminates). */
static void json_escape(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;
    const unsigned char *p;
    if (!src) src = "";
    if (dstsz == 0) return;
    for (p = (const unsigned char *)src; *p && o + 7 < dstsz; ++p) {
        unsigned char ch = *p;
        switch (ch) {
        case '"':  if (o + 2 < dstsz) { dst[o++] = '\\'; dst[o++] = '"'; }  break;
        case '\\': if (o + 2 < dstsz) { dst[o++] = '\\'; dst[o++] = '\\'; } break;
        case '\b': if (o + 2 < dstsz) { dst[o++] = '\\'; dst[o++] = 'b'; }  break;
        case '\f': if (o + 2 < dstsz) { dst[o++] = '\\'; dst[o++] = 'f'; }  break;
        case '\n': if (o + 2 < dstsz) { dst[o++] = '\\'; dst[o++] = 'n'; }  break;
        case '\r': if (o + 2 < dstsz) { dst[o++] = '\\'; dst[o++] = 'r'; }  break;
        case '\t': if (o + 2 < dstsz) { dst[o++] = '\\'; dst[o++] = 't'; }  break;
        default:
            if (ch < 0x20) {
                if (o + 7 < dstsz) o += (size_t)snprintf(dst + o, dstsz - o, "\\u%04x", ch);
            } else {
                dst[o++] = (char)ch;
            }
        }
    }
    dst[o] = '\0';
}

/* Human k=v rendering: quotes strings containing specials. */
static void human_value(char *dst, size_t dstsz, const char *s, int slen)
{
    int need_quote = 0, i;
    size_t o = 0;
    if (dstsz == 0) return;
    if (!s) { s = ""; slen = 0; }
    if (slen < 0) slen = (int)strlen(s);
    for (i = 0; i < slen; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 0x20 || ch == '"' || ch == '\\' || ch == '=' || ch == 0x7f) {
            need_quote = 1;
            break;
        }
    }
    if (slen == 0) need_quote = 1;
    if (!need_quote) {
        size_t n = (size_t)slen;
        if (n > dstsz - 1) n = dstsz - 1;
        memcpy(dst, s, n);
        dst[n] = '\0';
        return;
    }
    dst[o++] = '"';
    for (i = 0; i < slen && o + 6 < dstsz - 1; ++i) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
        case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
        case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
        case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
        case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
        case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
        default:
            if (ch < 0x20 || ch == 0x7f) {
                o += (size_t)snprintf(dst + o, dstsz - o, "\\x%02x", ch);
            } else {
                dst[o++] = (char)ch;
            }
        }
    }
    if (o + 2 <= dstsz) dst[o++] = '"';
    dst[o] = '\0';
}

/* ---------------------------------------------------------------- redaction */

static void lowercase_norm(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;
    for (; src && *src && o + 1 < dstsz; ++src) {
        char ch = *src;
        if (ch == '_' || ch == '-') continue;   /* insignificant separators */
        dst[o++] = (char)((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    }
    dst[o] = '\0';
}

int kn_redact_key_is_secret(const char *key)
{
    char norm[128];
    if (!key) return 0;
    lowercase_norm(norm, sizeof norm, key);
    if (strstr(norm, "token") || strstr(norm, "secret") ||
        strstr(norm, "password") || strstr(norm, "pwd") ||
        strstr(norm, "credential") ||
        strstr(norm, "authorization") || strstr(norm, "fingerprint") ||
        strstr(norm, "keyhex") || strstr(norm, "salthex") ||
        strstr(norm, "privatekey"))
        return 1;
    if (strcmp(norm, "devicecode") == 0 || strcmp(norm, "usercode") == 0)
        return 1;
    /* Prefix test via strstr (== norm) instead of strncmp: GCC 16 inlines
     * this function into callers with short literal keys ("op", "code", ...)
     * and -Wstring-compare then proves the fixed-bound strncmp constant.
     * Same semantics, no warning on the production toolchain. */
    if (strstr(norm, "srtp") == norm && (strstr(norm, "key") || strstr(norm, "salt")))
        return 1;
    return 0;
}

char *kn_redact_value(char *dst, size_t dstsz, const char *val)
{
    size_t n = val ? strlen(val) : 0;
    if (dstsz) snprintf(dst, dstsz, "<redacted:%lu>", (unsigned long)n);
    return dst;
}

static const char *const k_default_allowed_headers[] = {
    "contenttype", "useragent", "accept", "acceptlanguage",
    "nvclienttype", "nvclientid", "nvappversion",
    "nvdevicmake", "nvdevicemodel", "xnvclientidentity"
};

int kn_redact_header_allowed(const char *name)
{
    char norm[128], dn[128];
    size_t i;
    const size_t n_def = sizeof k_default_allowed_headers / sizeof k_default_allowed_headers[0];
    if (!name) return 0;
    lowercase_norm(norm, sizeof norm, name);
    for (i = 0; i < n_def; ++i) {
        lowercase_norm(dn, sizeof dn, k_default_allowed_headers[i]);
        if (strcmp(norm, dn) == 0) return 1;
    }
    for (i = 0; i < (size_t)g_n_allowed_headers; ++i) {
        lowercase_norm(dn, sizeof dn, g_allowed_headers[i]);
        if (strcmp(norm, dn) == 0) return 1;
    }
    return 0;
}

int kn_redact_allow_header(const char *name)
{
    if (!name || !name[0] || g_n_allowed_headers >= KN_MAX_ALLOWED_HDRS) return -1;
    snprintf(g_allowed_headers[g_n_allowed_headers], 64, "%s", name);
    g_n_allowed_headers++;
    return 0;
}

const char *kn_redact_auth_scheme(const char *v)
{
    if (!v) return "None";
    if (strncmp(v, "Bearer", 6) == 0) return "Bearer";
    if (strncmp(v, "GFNJWT", 6) == 0) return "GFNJWT";
    if (strncmp(v, "Basic", 5) == 0)  return "Basic";
    return "Unknown";
}

/* ------------------------------------------------------------------- sinks */

static void sink_path(const kn_sink *s, unsigned gen, char *out, size_t outsz)
{
    if (gen == 0) snprintf(out, outsz, "%s%s", s->base, s->ext);
    else          snprintf(out, outsz, "%s.%u%s", s->base, gen, s->ext);
}

static int sink_open(kn_sink *s)
{
    char path[KN_PATH_MAX];
    sink_path(s, 0, path, sizeof path);
    s->f = fopen(path, "ab");
    if (!s->f) { s->dead = 1; return -1; }
    s->bytes = kn_file_size(path);
    s->dead = 0;
    return 0;
}

static void sink_close(kn_sink *s)
{
    if (s->f) {
        fflush(s->f);
        fclose(s->f);
        s->f = NULL;
    }
}

static void sink_flush(kn_sink *s, int hard)
{
    if (!s || !s->f || s->dead) return;
    fflush(s->f);
    if (hard) (void)kn_fsync(s->f);   /* best effort; never fatal */
}

/* Caller holds G.mtx. channel.log -> channel.1.log -> channel.2.log -> drop
 * (§7.3). remove() before rename() because Win32 rename refuses to replace. */
static void sink_rotate(kn_sink *s)
{
    const unsigned gens = G.cfg.generations;
    char from[KN_PATH_MAX], to[KN_PATH_MAX];
    unsigned i;

    sink_close(s);
    if (gens < 2) {
        /* no archives configured: truncate the current file */
        sink_path(s, 0, from, sizeof from);
        s->f = fopen(from, "wb");
    } else {
        sink_path(s, gens - 1, to, sizeof to);
        remove(to);                                    /* drop oldest archive */
        for (i = gens - 1; i >= 2; --i) {              /* shift archives up */
            sink_path(s, i - 1, from, sizeof from);
            sink_path(s, i, to, sizeof to);
            if (kn_file_exists(from)) {
                remove(to);
                if (rename(from, to) != 0) G.stats.write_errors++;
            }
        }
        sink_path(s, 0, from, sizeof from);            /* current -> .1 */
        sink_path(s, 1, to, sizeof to);
        remove(to);
        if (rename(from, to) != 0) G.stats.write_errors++;
        s->f = fopen(from, "wb");
    }
    if (!s->f) { s->dead = 1; G.stats.write_errors++; return; }
    s->bytes = 0;
    G.stats.rotations++;
}

static int sink_write(kn_sink *s, const char *line, size_t len)
{
    if (!s->f || s->dead) return -1;
    if (s->bytes + (long long)len + 1 > (long long)G.cfg.rotate_bytes) sink_rotate(s);
    if (!s->f || s->dead) return -1;
    if (fwrite(line, 1, len, s->f) != len || fputc('\n', s->f) == EOF) {
        s->dead = 1;
        G.stats.write_errors++;
        return -1;
    }
    s->bytes += (long long)len + 1;
    return 0;
}

/* ------------------------------------------------------------------- init */

int kn_log_init(const kn_log_config *cfg)
{
    kn_log_config d;
    int i, any_log = 0;
    uint64_t seed;

    if (G.inited) return -3;

    memset(&d, 0, sizeof d);
    if (cfg) d = *cfg;
    if (!d.log_dir) d.log_dir = "logs";
    if (d.default_level < KN_TRACE || d.default_level > KN_ERROR) d.default_level = KN_INFO;
    if (d.rotate_bytes == 0) d.rotate_bytes = 2u * 1024u * 1024u;
    if (d.rotate_bytes < 256) d.rotate_bytes = 256;          /* tests use tiny values */
    if (d.generations == 0) d.generations = 3;
    if (d.jsonl_channels == 0) d.jsonl_channels = k_default_jsonl_mask;
    if (d.jsonl_min_level < KN_TRACE || d.jsonl_min_level > KN_ERROR) d.jsonl_min_level = KN_DEBUG;

#ifdef _WIN32
    if (!s_qpc_freq.QuadPart) QueryPerformanceFrequency(&s_qpc_freq);
    if (!s_gstpaft) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32) s_gstpaft = (kn_gstpaft_t)(void *)GetProcAddress(k32, "GetSystemTimePreciseAsFileTime");
    }
#endif

    memset(&G, 0, sizeof G);
    G.cfg = d;
    G.cfg.log_dir = NULL;          /* never dereference caller strings later */
    G.cfg.build_stamp = NULL;
    G.cfg.version = NULL;
    G.cfg.channel_levels = NULL;
    snprintf(G.dir, sizeof G.dir, "%s", d.log_dir);
    snprintf(G.build_stamp, sizeof G.build_stamp, "%s", d.build_stamp ? d.build_stamp : "unknown");
    snprintf(G.version, sizeof G.version, "%s", d.version ? d.version : "0.0.0");
    G.jsonl_mask = d.jsonl_channels;

    if (kn_mkdir(G.dir) != 0) return -1;
    kn_mutex_init(&G.mtx);

    for (i = 0; i < KN_CH__COUNT; ++i) {
        kn_level lvl = d.default_level;
        if (d.channel_levels && d.channel_levels[i] >= KN_TRACE && d.channel_levels[i] <= KN_ERROR)
            lvl = d.channel_levels[i];
        G.levels[i] = lvl;
        snprintf(G.sinks[i].log.base, KN_PATH_MAX, "%.*s/%s",
                 KN_PATH_MAX - 16, G.dir, k_channel_names[i]);
        G.sinks[i].log.ext = ".log";
        if (sink_open(&G.sinks[i].log) == 0) any_log = 1;
        if (G.jsonl_mask & (1u << i)) {
            snprintf(G.sinks[i].jsonl.base, KN_PATH_MAX, "%.*s/%s",
                     KN_PATH_MAX - 16, G.dir, k_channel_names[i]);
            G.sinks[i].jsonl.ext = ".jsonl";
            (void)sink_open(&G.sinks[i].jsonl);
        }
    }
    if (!any_log) { kn_mutex_destroy(&G.mtx); return -2; }

    seed = (uint64_t)kn_wall_ms() ^ ((uint64_t)kn_process_id() << 20)
         ^ (uint64_t)(uintptr_t)&G ^ 0x5DEECE66DULL;
    G.rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
    G.inited = 1;

    /* §7.2: build stamp once per process start. */
    {
        kn_lctx c;
        kn_lctx_init(&c, KN_CH_APP, KN_INFO, "process_start");
        kn_lctx_str(&c, "build", G.build_stamp);
        kn_lctx_str(&c, "version", G.version);
        kn_lctx_u64(&c, "pid", kn_process_id());
        kn_log_emit(&c);
    }
    return 0;
}

void kn_log_shutdown(void)
{
    int i;
    if (!G.inited) return;
    kn_mutex_lock(&G.mtx);
    for (i = 0; i < KN_CH__COUNT; ++i) {
        sink_flush(&G.sinks[i].log, 1);
        sink_close(&G.sinks[i].log);
        sink_flush(&G.sinks[i].jsonl, 1);
        sink_close(&G.sinks[i].jsonl);
    }
    G.inited = 0;
    kn_mutex_unlock(&G.mtx);
    kn_mutex_destroy(&G.mtx);
}

int kn_log_is_initialized(void) { return G.inited; }

/* ----------------------------------------------------------------- levels */

void kn_log_set_level(kn_channel ch, kn_level lvl)
{
    int i = (int)ch;
    if (i < 0 || i >= KN_CH__COUNT) return;
    if (lvl < KN_TRACE || lvl > KN_ERROR) return;
    if (G.inited) kn_mutex_lock(&G.mtx);
    G.levels[i] = lvl;
    if (G.inited) kn_mutex_unlock(&G.mtx);
}

void kn_log_set_all_levels(kn_level at_least)
{
    int i;
    if (!G.inited) return;
    kn_mutex_lock(&G.mtx);
    for (i = 0; i < KN_CH__COUNT; ++i)
        if (G.levels[i] > at_least) G.levels[i] = at_least; /* more verbose */
    kn_mutex_unlock(&G.mtx);
}

kn_level kn_log_get_level(kn_channel ch)
{
    kn_level l = KN_INFO;
    int i = (int)ch;
    if (i < 0 || i >= KN_CH__COUNT) return l;
    if (G.inited) kn_mutex_lock(&G.mtx);
    l = G.levels[i];
    if (G.inited) kn_mutex_unlock(&G.mtx);
    return l;
}

int kn_log_would_log(kn_channel ch, kn_level lvl)
{
    int i = (int)ch;
    return G.inited && i >= 0 && i < KN_CH__COUNT && lvl >= G.levels[i];
}

/* --------------------------------------------------------------- emission */

void kn_lctx_init(kn_lctx *c, kn_channel ch, kn_level lvl, const char *msg)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->channel = ch;
    c->level = lvl;
    c->msg = msg ? msg : "";
}

void kn_lctx_corr(kn_lctx *c, const char *correlation)
{
    if (c) c->correlation = correlation;
}

static kn_field *field_alloc(kn_lctx *c, const char *key)
{
    kn_field *f;
    if (!c || !key || !key[0]) return NULL;
    if (c->nfields >= KN_LOG_MAX_FIELDS) { c->dropped_fields++; return NULL; }
    f = &c->fields[c->nfields++];
    memset(f, 0, sizeof *f);
    f->key = key;
    f->slen = -1;
    return f;
}

static const char *arena_copy(kn_lctx *c, const char *s, size_t n)
{
    char *dst;
    if (!s) { s = ""; n = 0; }
    if (c->arena_used + n + 1 > KN_LOG_ARENA) {
        /* truncate into remaining space; '~' prefix marks truncation */
        size_t avail = (KN_LOG_ARENA > c->arena_used + 2)
                     ? KN_LOG_ARENA - c->arena_used - 2 : 0;
        if (avail == 0) return "";
        dst = c->arena + c->arena_used;
        dst[0] = '~';
        if (n < avail) avail = n;
        memcpy(dst + 1, s, avail);
        dst[1 + avail] = '\0';
        c->arena_used += avail + 2;
        return dst;
    }
    dst = c->arena + c->arena_used;
    memcpy(dst, s, n);
    dst[n] = '\0';
    c->arena_used += n + 1;
    return dst;
}

void kn_lctx_str(kn_lctx *c, const char *key, const char *val)
{
    kn_field *f = field_alloc(c, key);
    if (!f) return;
    if (kn_redact_key_is_secret(key)) {
        char red[32];
        f->type = KN_V_STR;
        G.stats.secret_values_redacted++;  /* approximate: benign unlocked counter */
        kn_redact_value(red, sizeof red, val);
        f->sval = arena_copy(c, red, strlen(red));
        f->slen = (int)strlen(f->sval);
        return;
    }
    f->type = KN_V_STR;
    if (!val) val = "";
    f->sval = arena_copy(c, val, strlen(val));
    f->slen = (int)strlen(f->sval);
}

void kn_lctx_nstr(kn_lctx *c, const char *key, const char *val, size_t n)
{
    kn_field *f = field_alloc(c, key);
    if (!f) return;
    f->type = KN_V_STR;
    if (!val) { val = ""; n = 0; }
    f->sval = arena_copy(c, val, n);
    f->slen = (int)strlen(f->sval);
}

void kn_lctx_secret(kn_lctx *c, const char *key, const char *val)
{
    char red[32];
    kn_field *f = field_alloc(c, key);
    if (!f) return;
    f->type = KN_V_STR;
    G.stats.secret_values_redacted++;      /* approximate: benign unlocked counter */
    kn_redact_value(red, sizeof red, val);
    f->sval = arena_copy(c, red, strlen(red));
    f->slen = (int)strlen(f->sval);
}

void kn_lctx_i64(kn_lctx *c, const char *key, long long val)
{
    kn_field *f = field_alloc(c, key);
    if (!f) return;
    f->type = KN_V_I64;
    f->ival = val;
}

void kn_lctx_u64(kn_lctx *c, const char *key, unsigned long long val)
{
    kn_field *f = field_alloc(c, key);
    if (!f) return;
    f->type = KN_V_U64;
    f->uval = val;
}

void kn_lctx_hex(kn_lctx *c, const char *key, unsigned long long val)
{
    kn_field *f = field_alloc(c, key);
    if (!f) return;
    f->type = KN_V_HEX;
    f->uval = val;
}

void kn_lctx_bool(kn_lctx *c, const char *key, int val)
{
    kn_field *f = field_alloc(c, key);
    if (!f) return;
    f->type = KN_V_BOOL;
    f->ival = val ? 1 : 0;
}

void kn_lctx_f64(kn_lctx *c, const char *key, double val)
{
    kn_field *f = field_alloc(c, key);
    if (!f) return;
    f->type = KN_V_F64;
    f->dval = val;
}

/* Render both lines. No locks, no I/O — pure formatting into caller buffers. */
static size_t render_human(const kn_lctx *c, const char *ts, const char *corr,
                           const char *sess, char *buf, size_t bufsz, int *trunc)
{
    size_t o = 0;
    int i;
    *trunc = 0;
    KN_APP(buf, bufsz, o, trunc, "%s %-5s [%s]", ts, level_name(c->level),
           channel_name(c->channel));
    if (corr && corr[0]) KN_APP(buf, bufsz, o, trunc, " c=%s", corr);
    if (sess && sess[0]) KN_APP(buf, bufsz, o, trunc, " s=%s", sess);
    KN_APP(buf, bufsz, o, trunc, " %s", c->msg);
    for (i = 0; i < c->nfields && !*trunc; ++i) {
        const kn_field *f = &c->fields[i];
        char vbuf[1024];
        switch (f->type) {
        case KN_V_STR:  human_value(vbuf, sizeof vbuf, f->sval, f->slen); break;
        case KN_V_I64:  snprintf(vbuf, sizeof vbuf, "%lld", f->ival); break;
        case KN_V_U64:  snprintf(vbuf, sizeof vbuf, "%llu", f->uval); break;
        case KN_V_HEX:  snprintf(vbuf, sizeof vbuf, "0x%08llX", f->uval); break;
        case KN_V_BOOL: snprintf(vbuf, sizeof vbuf, "%s", f->ival ? "true" : "false"); break;
        case KN_V_F64:  snprintf(vbuf, sizeof vbuf, "%.6g", f->dval); break;
        default: vbuf[0] = '?'; vbuf[1] = '\0';
        }
        KN_APP(buf, bufsz, o, trunc, " %s=%s", f->key, vbuf);
    }
    if (c->dropped_fields && !*trunc)
        KN_APP(buf, bufsz, o, trunc, " fields_dropped=%d", c->dropped_fields);
    return o;
}

static size_t render_json(const kn_lctx *c, const char *ts, long long mono_ms,
                          const char *corr, const char *sess, char *buf,
                          size_t bufsz, int *trunc)
{
    size_t o = 0;
    int i;
    char esc[1024], vbuf[1024];
    *trunc = 0;
    KN_APP(buf, bufsz, o, trunc,
           "{\"ts\":\"%s\",\"mono_ms\":%lld,\"level\":\"%s\",\"channel\":\"%s\","
           "\"pid\":%lu,\"thread\":%lu",
           ts, mono_ms, level_name(c->level), channel_name(c->channel),
           kn_process_id(), kn_thread_id());
    if (corr && corr[0]) {
        json_escape(esc, sizeof esc, corr);
        KN_APP(buf, bufsz, o, trunc, ",\"correlation\":\"%s\"", esc);
    }
    if (sess && sess[0]) {
        json_escape(esc, sizeof esc, sess);
        KN_APP(buf, bufsz, o, trunc, ",\"session_id\":\"%s\"", esc);
    }
    json_escape(esc, sizeof esc, c->msg);
    KN_APP(buf, bufsz, o, trunc, ",\"msg\":\"%s\"", esc);
    if (c->nfields || c->dropped_fields) {
        KN_APP(buf, bufsz, o, trunc, ",\"fields\":{");
        for (i = 0; i < c->nfields && !*trunc; ++i) {
            const kn_field *f = &c->fields[i];
            json_escape(esc, sizeof esc, f->key);
            switch (f->type) {
            case KN_V_STR:
                json_escape(vbuf, sizeof vbuf, f->sval);
                KN_APP(buf, bufsz, o, trunc, "%s\"%s\":\"%s\"", i ? "," : "", esc, vbuf);
                break;
            case KN_V_I64:
                KN_APP(buf, bufsz, o, trunc, "%s\"%s\":%lld", i ? "," : "", esc, f->ival);
                break;
            case KN_V_U64:
                KN_APP(buf, bufsz, o, trunc, "%s\"%s\":%llu", i ? "," : "", esc, f->uval);
                break;
            case KN_V_HEX:
                snprintf(vbuf, sizeof vbuf, "0x%08llX", f->uval);
                KN_APP(buf, bufsz, o, trunc, "%s\"%s\":\"%s\"", i ? "," : "", esc, vbuf);
                break;
            case KN_V_BOOL:
                KN_APP(buf, bufsz, o, trunc, "%s\"%s\":%s", i ? "," : "", esc,
                       f->ival ? "true" : "false");
                break;
            case KN_V_F64:
                snprintf(vbuf, sizeof vbuf, "%.9g", f->dval);
                KN_APP(buf, bufsz, o, trunc, "%s\"%s\":%s", i ? "," : "", esc, vbuf);
                break;
            default: break;
            }
        }
        if (c->dropped_fields && !*trunc)
            KN_APP(buf, bufsz, o, trunc, "%s\"fields_dropped\":%d",
                   c->nfields ? "," : "", c->dropped_fields);
        KN_APP(buf, bufsz, o, trunc, "}");
    }
    KN_APP(buf, bufsz, o, trunc, "}");
    return o;
}

void kn_log_emit(kn_lctx *c)
{
    char hbuf[KN_LINE_BUF], jbuf[KN_JSON_BUF], ts[32];
    char sess[KN_SESSION_ID_MAX];
    const char *corr;
    size_t hlen, jlen = 0;
    long long mono, epoch;
    int sealed, do_jsonl, htrunc, jtrunc;

    if (!c) return;
    if (!G.inited) return;
    if ((int)c->channel < 0 || (int)c->channel >= KN_CH__COUNT) return;
    if (c->level < KN_TRACE || c->level > KN_ERROR) return;

    kn_mutex_lock(&G.mtx);
    if (c->level < G.levels[c->channel]) {
        G.stats.events_filtered++;
        kn_mutex_unlock(&G.mtx);
        return;
    }
    if (!G.mono_epoch_set) { G.mono_epoch_set = 1; G.mono_epoch_ms = kn_mono_ms(); }
    epoch = G.mono_epoch_ms;
    snprintf(sess, sizeof sess, "%s", G.have_session_id ? G.session_id : "");
    sealed = G.sealed;
    do_jsonl = (G.jsonl_mask & (1u << (int)c->channel)) &&
               c->level >= G.cfg.jsonl_min_level;
    kn_mutex_unlock(&G.mtx);

    corr = c->correlation;
    if ((!corr || !corr[0])) corr = thread_corr_lookup();
    if (corr && !corr[0]) corr = NULL;

    format_ts(kn_wall_ms(), ts);
    mono = kn_mono_ms() - epoch;

    hlen = render_human(c, ts, corr, sess[0] ? sess : NULL, hbuf, sizeof hbuf, &htrunc);
    if (do_jsonl)
        jlen = render_json(c, ts, mono, corr, sess[0] ? sess : NULL, jbuf,
                           sizeof jbuf, &jtrunc);

    kn_mutex_lock(&G.mtx);
    {
        kn_sink *s = &G.sinks[c->channel].log;
        if (sink_write(s, hbuf, hlen) == 0) {
            G.stats.lines_written++;
            G.stats.bytes_written += (unsigned long long)hlen + 1;
        }
        /* flush policy (§7.12): WARN+ always hard-flush; sealed flushes all;
         * INFO soft-flush (fflush); DEBUG/TRACE stay buffered. */
        if (sealed || c->level >= KN_WARN) sink_flush(s, 1);
        else if (c->level == KN_INFO) sink_flush(s, 0);
    }
    if (do_jsonl && jlen) {
        kn_sink *s = &G.sinks[c->channel].jsonl;
        if (sink_write(s, jbuf, jlen) == 0) {
            G.stats.json_written++;
            G.stats.bytes_written += (unsigned long long)jlen + 1;
        }
        if (sealed || c->level >= KN_WARN) sink_flush(s, 1);
    }
    G.stats.events_emitted++;
    if (htrunc) G.stats.truncated_lines++;
    if (c->dropped_fields) G.stats.dropped_fields += (unsigned long long)c->dropped_fields;
    kn_mutex_unlock(&G.mtx);
}

void kn_log_simple(kn_channel ch, kn_level lvl, const char *correlation,
                   const char *msg)
{
    kn_lctx c;
    kn_lctx_init(&c, ch, lvl, msg);
    kn_lctx_corr(&c, correlation);
    kn_log_emit(&c);
}

/* ------------------------------------------------- correlation & session */

static uint64_t rng_next(void)
{
    /* xorshift64* — non-security ids only. Caller holds G.mtx OR G not inited. */
    uint64_t x = G.rng;
    if (!x) x = 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    G.rng = x;
    return x * 2685821657736338717ULL;
}

const char *kn_log_new_correlation(char out[KN_CORR_LEN + 1])
{
    static const char hexd[] = "0123456789abcdef";
    unsigned long long v1, v2;
    int i;
    if (!out) return NULL;
    if (G.inited) kn_mutex_lock(&G.mtx);
    v1 = rng_next();
    v2 = rng_next() ^ (++G.corr_counter * 0x9E3779B97F4A7C15ULL);
    if (G.inited) kn_mutex_unlock(&G.mtx);
    for (i = 0; i < KN_CORR_LEN; ++i) {
        unsigned long long src = (i < 6) ? v1 : v2;
        out[i] = hexd[(unsigned)((src >> ((i % 8) * 8)) & 0xFULL)];
    }
    out[KN_CORR_LEN] = '\0';
    return out;
}

void kn_log_set_thread_correlation(const char *corr)
{
    unsigned long tid = kn_thread_id();
    int i, free_slot = -1;
    for (i = 0; i < KN_TID_SLOTS; ++i) {
        if (g_tid_corr[i].used && g_tid_corr[i].tid == tid) { free_slot = i; break; }
        if (free_slot < 0 && !g_tid_corr[i].used) free_slot = i;
    }
    if (free_slot < 0) {                    /* table full: round-robin eviction */
        free_slot = (int)(g_tid_rr % KN_TID_SLOTS);
        g_tid_rr++;
    }
    if (!corr || !corr[0]) { g_tid_corr[free_slot].used = 0; return; }
    g_tid_corr[free_slot].tid = tid;
    snprintf(g_tid_corr[free_slot].corr, KN_CORR_LEN + 1, "%s", corr);
    g_tid_corr[free_slot].used = 1;
}

const char *kn_log_get_thread_correlation(void)
{
    return thread_corr_lookup();
}

void kn_log_set_session_id(const char *session_id)
{
    if (!G.inited) return;
    kn_mutex_lock(&G.mtx);
    if (!session_id || !session_id[0]) {
        G.have_session_id = 0;
        G.session_id[0] = '\0';
    } else {
        snprintf(G.session_id, sizeof G.session_id, "%s", session_id);
        G.have_session_id = 1;
    }
    kn_mutex_unlock(&G.mtx);
}

/* -------------------------------------------------- failure amplification */

int kn_log_failure_cb_register(kn_failure_cb cb, void *userdata)
{
    int rc = 0;
    if (!cb) return -1;
    if (G.inited) kn_mutex_lock(&G.mtx);
    if (G.n_cbs >= KN_MAX_FAILURE_CBS) rc = -1;
    else {
        G.cbs[G.n_cbs] = cb;
        G.cb_ud[G.n_cbs] = userdata;
        G.n_cbs++;
    }
    if (G.inited) kn_mutex_unlock(&G.mtx);
    return rc;
}

void kn_log_on_failure(const char *op, const char *code, const char *reason,
                       const char *next_action)
{
    kn_failure_cb cbs[KN_MAX_FAILURE_CBS];
    void *uds[KN_MAX_FAILURE_CBS];
    int n = 0, i;

    if (!G.inited) return;

    /* 1. the ERROR event itself (§7.12: code + reason + next_action, always) */
    {
        kn_lctx c;
        kn_lctx_init(&c, KN_CH_APP, KN_ERROR, "operation_failed");
        kn_lctx_str(&c, "op", op ? op : "unknown");
        kn_lctx_str(&c, "code", code ? code : "unknown");
        kn_lctx_str(&c, "reason", reason ? reason : "");
        kn_lctx_str(&c, "next_action", next_action ? next_action : "");
        kn_lctx_bool(&c, "amplified", 1);
        kn_log_emit(&c);
    }

    kn_mutex_lock(&G.mtx);
    /* 2. raise verbosity: all channels at least DEBUG, transport TRACE (§7.7.1).
     * NOTE: levels are 1-based, lower = more verbose — comparisons are ">". */
    for (i = 0; i < KN_CH__COUNT; ++i)
        if (G.levels[i] > KN_DEBUG) G.levels[i] = KN_DEBUG;
    if (G.levels[KN_CH_TRANSPORT] > KN_TRACE) G.levels[KN_CH_TRANSPORT] = KN_TRACE;
    G.amplified = 1;
    /* 3. seal: flush+fsync every subsequent event (§7.7.2) */
    G.sealed = 1;
    for (i = 0; i < KN_CH__COUNT; ++i) {
        sink_flush(&G.sinks[i].log, 1);
        sink_flush(&G.sinks[i].jsonl, 1);
    }
    n = G.n_cbs;
    memcpy(cbs, G.cbs, sizeof cbs);
    memcpy(uds, G.cb_ud, sizeof uds);
    kn_mutex_unlock(&G.mtx);

    /* 4. callbacks (bundle writer, self-test runner) — outside the lock */
    for (i = 0; i < n; ++i)
        if (cbs[i]) cbs[i](op ? op : "unknown", code ? code : "unknown",
                           reason ? reason : "", uds[i]);
}

void kn_log_flush_all(void)
{
    int i;
    if (!G.inited) return;
    kn_mutex_lock(&G.mtx);
    for (i = 0; i < KN_CH__COUNT; ++i) {
        sink_flush(&G.sinks[i].log, 1);
        sink_flush(&G.sinks[i].jsonl, 1);
    }
    kn_mutex_unlock(&G.mtx);
}

void kn_log_get_stats(kn_log_stats *out)
{
    if (!out) return;
    if (G.inited) kn_mutex_lock(&G.mtx);
    *out = G.stats;
    out->sealed = G.sealed;
    out->amplified = G.amplified;
    if (G.inited) kn_mutex_unlock(&G.mtx);
}
