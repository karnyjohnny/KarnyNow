/* kn_diag.c — diagnostic bundle writer. Contract + design rules in kn_diag.h
 * (docs/BOOTSTRAP.md §7.8; §7.12 rules: never fseek over our own output, flush
 * after every ERROR, isolate crash risks, bounded everything).
 *
 * Zip format: store-only (method 0), no ZIP64 (cap keeps us far below 4 GiB),
 * sequential write — local header + data per entry, then central directory,
 * then EOCD. Every entry is fully materialised in RAM before its header is
 * written, so sizes and CRCs are known up front and the file pointer never
 * moves backwards. (T4 v3 lost 13 stages of measurements to a fseek-based
 * writer; this is why that pattern is banned, §7.12.)
 */
#ifndef _WIN32
/* POSIX declarations (clock_gettime, fileno, opendir) under strict -std=c11:
 * request them explicitly so this TU compiles standalone without -pthread's
 * implicit _REENTRANT. MUST be the first thing in the file — the first
 * system header include locks in features.h for the whole TU. */
#if !defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L)
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "kn/kn_diag.h"
#include "kn/knlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ platform shim */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>          /* _fileno, _commit */
typedef CRITICAL_SECTION kn_diag_mutex;
static void kn_diag_mutex_init(kn_diag_mutex *m)    { InitializeCriticalSection(m); }
static void kn_diag_mutex_lock(kn_diag_mutex *m)    { EnterCriticalSection(m); }
static void kn_diag_mutex_unlock(kn_diag_mutex *m)  { LeaveCriticalSection(m); }
static int kn_diag_fsync(FILE *f) { int fd = _fileno(f); return fd >= 0 ? _commit(fd) : -1; }
static long long kn_diag_wall_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);   /* ~15.6 ms granularity is fine for names */
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
}
static unsigned long long kn_diag_mono_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER pc;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&pc);
    if (!freq.QuadPart) return 0;
    return (unsigned long long)((double)pc.QuadPart * 1000000.0 / (double)freq.QuadPart);
}
static unsigned long kn_diag_pid(void) { return (unsigned long)GetCurrentProcessId(); }
/* Local calendar for zip DOS stamps; UTC civil date derived from wall_ms. */
static void kn_diag_local_ymd_hms(unsigned *y, unsigned *mo, unsigned *d,
                                  unsigned *h, unsigned *mi, unsigned *s)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    *y = st.wYear; *mo = st.wMonth; *d = st.wDay;
    *h = st.wHour; *mi = st.wMinute; *s = st.wSecond;
}
/* Directory listing: file basenames only (no paths). Returns count or -1. */
static int kn_diag_list_dir(const char *dir, char names[][KN_DIAG_NAME_MAX], int max)
{
    char pattern[KN_DIAG_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        /* names that cannot fit the entry table are not our artifacts */
        if (strlen(fd.cFileName) < KN_DIAG_NAME_MAX) {
            if (n < max) {
                size_t l = strlen(fd.cFileName);
                memcpy(names[n], fd.cFileName, l);
                names[n][l] = '\0';
            }
            ++n;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n < max ? n : max;
}
#else /* POSIX — host-side unit tests */
#  include <time.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <pthread.h>
typedef pthread_mutex_t kn_diag_mutex;
static void kn_diag_mutex_init(kn_diag_mutex *m)    { pthread_mutex_init(m, NULL); }
static void kn_diag_mutex_lock(kn_diag_mutex *m)    { pthread_mutex_lock(m); }
static void kn_diag_mutex_unlock(kn_diag_mutex *m)  { pthread_mutex_unlock(m); }
static int kn_diag_fsync(FILE *f) { int fd = fileno(f); return fd >= 0 ? fsync(fd) : -1; }
static long long kn_diag_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
static unsigned long long kn_diag_mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ULL + (unsigned long long)(ts.tv_nsec / 1000);
}
static unsigned long kn_diag_pid(void) { return (unsigned long)getpid(); }
static void kn_diag_local_ymd_hms(unsigned *y, unsigned *mo, unsigned *d,
                                  unsigned *h, unsigned *mi, unsigned *s)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    *y = (unsigned)(tmv.tm_year + 1900); *mo = (unsigned)(tmv.tm_mon + 1);
    *d = (unsigned)tmv.tm_mday; *h = (unsigned)tmv.tm_hour;
    *mi = (unsigned)tmv.tm_min; *s = (unsigned)tmv.tm_sec;
}
static int kn_diag_list_dir(const char *dir, char names[][KN_DIAG_NAME_MAX], int max)
{
    DIR *dp = opendir(dir);
    struct dirent *de;
    int n = 0;
    if (!dp) return -1;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        /* names that cannot fit the entry table are not our artifacts;
         * memcpy instead of snprintf so the strlen bound is explicit for
         * -Wformat-truncation at every optimization level */
        if (strlen(de->d_name) < KN_DIAG_NAME_MAX) {
            if (n < max) {
                size_t l = strlen(de->d_name);
                memcpy(names[n], de->d_name, l);
                names[n][l] = '\0';
            }
            ++n;
        }
    }
    closedir(dp);
    return n < max ? n : max;
}
#endif

/* ------------------------------------------------------------------- crc32 */

/* Standard zip CRC-32: reflected polynomial 0xEDB88320. Table built once;
 * concurrent identical writes of the same values are benign. */
static uint32_t g_crc_table[256];
static int g_crc_table_ready;

static void crc_table_build(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; ++i) {
        c = i;
        for (j = 0; j < 8; ++j) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
    g_crc_table_ready = 1;
}

uint32_t kn_diag_crc32(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    if (!g_crc_table_ready) crc_table_build();
    crc = ~crc;
    for (i = 0; i < len; ++i) crc = g_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------- little-endian out */

static void wr16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------ time helpers */

/* Howard Hinnant's civil_from_days (same algorithm as knlog.c). */
static void civil_from_days(long long z, int *yy, unsigned *mm, unsigned *dd)
{
    long long era, y;
    unsigned long long doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned long long)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (long long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *dd = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *mm = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *yy = (int)(y + (*mm <= 2 ? 1 : 0));
}

/* "20261022T101530Z" from wall clock (UTC). */
static void utc_compact(char out[20])
{
    long long ms = kn_diag_wall_ms();
    long long days = ms / 86400000LL, rem = ms % 86400000LL;
    int y;
    unsigned m, d;
    if (rem < 0) { rem += 86400000LL; days -= 1; }
    civil_from_days(days, &y, &m, &d);
    /* components modulo-bounded: GCC 16 -Wformat-truncation needs provable
     * widths (T10 round 1 finding; GCC 12 did not flag this) */
    snprintf(out, 20, "%04u%02u%02dT%02u%02u%02uZ",
             (unsigned)y % 10000u, m % 100u, d % 100u,
             (unsigned)(rem / 3600000LL) % 24u,
             (unsigned)((rem / 60000LL) % 60ULL) % 60u,
             (unsigned)((rem / 1000LL) % 60ULL) % 60u);
}

/* RFC 3339 UTC with ms, for summary.json ("2026-10-22T10:15:30.123Z"). */
static void utc_rfc3339(char out[32])
{
    long long ms = kn_diag_wall_ms();
    long long days = ms / 86400000LL, rem = ms % 86400000LL;
    int y;
    unsigned m, d;
    if (rem < 0) { rem += 86400000LL; days -= 1; }
    civil_from_days(days, &y, &m, &d);
    snprintf(out, 32, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
             (unsigned)y % 10000u, m % 100u, d % 100u,
             (unsigned)(rem / 3600000LL) % 24u,
             (unsigned)((rem / 60000LL) % 60ULL) % 60u,
             (unsigned)((rem / 1000LL) % 60ULL) % 60u,
             (unsigned)(rem % 1000LL) % 1000u);
}

static void dos_stamp(uint16_t *dostime, uint16_t *dosdate)
{
    unsigned y, mo, d, h, mi, s;
    kn_diag_local_ymd_hms(&y, &mo, &d, &h, &mi, &s);
    if (y < 1980) y = 1980;                       /* DOS epoch floor */
    *dostime = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
    *dosdate = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
}

static uint32_t shortid8(void)
{
    static unsigned long long counter;
    unsigned long long x = kn_diag_mono_us();
    x ^= (unsigned long long)kn_diag_pid() << 20;
    x ^= (++counter) * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ULL; x ^= x >> 32;
    return (uint32_t)(x & 0xFFFFFFFFULL);
}

/* ------------------------------------------------------------------ types */

typedef struct {
    char           name[KN_DIAG_NAME_MAX];  /* zip path, forward slashes */
    unsigned char *data;                    /* heap-owned; NULL when dropped */
    size_t         len;                     /* current bytes */
    unsigned long long orig_len;            /* bytes as first added */
    int            prio;
    int            dropped;                 /* removed whole by cap policy */
    int            truncated;               /* tail-kept at least once */
    uint32_t       offset;                  /* local-header offset, set at write */
} kn_diag_art;

typedef struct {
    char           subsystem[KN_DIAG_SUBSYS_MAX];
    int            status;
    char           reason[KN_DIAG_REASON_MAX];
    char           evidence[KN_DIAG_NAME_MAX];
} kn_diag_verdict_rec;

struct kn_diag_bundle {
    /* out_dir is shorter than KN_DIAG_PATH_MAX on purpose: the zip name
     * adds "/diag-" + 15 (UTC) + "-" + 8 (shortid) + ".zip" = up to 36
     * chars, and the composition must not truncate (-Wformat-truncation). */
    char               out_dir[KN_DIAG_PATH_MAX - 64];
    size_t             cap;
    kn_diag_art        arts[KN_DIAG_MAX_ARTIFACTS];
    size_t             narts;
    unsigned long long arts_overflow;       /* adds rejected: table full */
    kn_diag_verdict_rec verdicts[KN_DIAG_MAX_VERDICTS];
    size_t             nverdicts;
    char               trigger_op[64];
    char               trigger_code[64];
    char               trigger_reason[256];
    int                over_cap;            /* CORE alone exceeded the cap */
    int                summary_truncated;
};

/* JSON string escaping (same bounded algorithm as knlog.c). */
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

/* Bounded cursor-append (same guard pattern as knlog.c). */
#define KN_APP(buf, bufsz, o, trunc, ...)                                \
    do {                                                                  \
        if ((o) < (bufsz)) {                                              \
            int n_ = snprintf((buf) + (o), (bufsz) - (o), __VA_ARGS__);   \
            if (n_ < 0) n_ = 0;                                           \
            if ((size_t)n_ >= (bufsz) - (o)) { (o) = (bufsz) - 1; *(trunc) = 1; } \
            else { (o) += (size_t)n_; }                                   \
        }                                                                 \
    } while (0)

/* Bounded copy + backslash normalization (no snprintf: the source length is
 * unbounded and -Wformat-truncation would have to assume the worst). */
static void norm_zip_path(char *dst, size_t dstsz, const char *src)
{
    size_t n, i;
    if (!dst || dstsz == 0) return;
    if (!src) src = "";
    n = strlen(src);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    for (i = 0; i < n; ++i) if (dst[i] == '\\') dst[i] = '/';
}

/* --------------------------------------------------------- artifact table */

/* Zip bookkeeping bytes per entry: local header 30 + central 46 + name twice. */
static size_t art_zip_overhead(const kn_diag_art *a)
{
    return 30u + 46u + 2u * strlen(a->name);
}

static size_t bundle_total(const kn_diag_bundle *b)
{
    size_t total = 22u;                       /* EOCD */
    size_t i;
    for (i = 0; i < b->narts; ++i)
        if (!b->arts[i].dropped) total += b->arts[i].len + art_zip_overhead(&b->arts[i]);
    return total;
}

static int art_find(const kn_diag_bundle *b, const char *zip_path)
{
    size_t i;
    char norm[KN_DIAG_NAME_MAX];
    norm_zip_path(norm, sizeof norm, zip_path);
    for (i = 0; i < b->narts; ++i)
        if (strcmp(b->arts[i].name, norm) == 0) return (int)i;
    return -1;
}

/* Rebuild an artifact's buffer as: marker line + newest `keep` bytes.
 * Returns 0 ok, -1 alloc failure (artifact left untouched). */
static int art_tail_keep(kn_diag_art *a, size_t keep, unsigned long long orig_total)
{
    char marker[96];
    size_t mlen;
    unsigned char *nb;
    mlen = (size_t)snprintf(marker, sizeof marker,
        "[kn_diag] truncated: kept newest %u of %llu bytes\n",
        (unsigned)keep, orig_total);
    nb = (unsigned char *)malloc(mlen + keep);
    if (!nb) return -1;
    memcpy(nb, marker, mlen);
    memcpy(nb + mlen, a->data + (a->len - keep), keep);
    free(a->data);
    a->data = nb;
    a->len = mlen + keep;
    a->truncated = 1;
    return 0;
}

static void art_drop(kn_diag_art *a)
{
    free(a->data);
    a->data = NULL;
    a->len = 0;
    a->dropped = 1;
}

/* Index of the largest non-dropped artifact of `prio` (ties -> highest
 * index, i.e. the most recently added). -1 if none. */
static int pick_largest(const kn_diag_bundle *b, int prio, size_t min_len)
{
    int best = -1;
    size_t i;
    for (i = 0; i < b->narts; ++i) {
        const kn_diag_art *a = &b->arts[i];
        if (a->dropped || a->prio != prio || a->len < min_len) continue;
        if (best < 0 || a->len >= b->arts[best].len) best = (int)i;
    }
    return best;
}

/* §7.8: "Size cap the bundle and truncate the largest artefacts first,
 * newest-kept." Deterministic order: drop SECONDARY whole (oldest log
 * generations) -> halve the largest PRIMARY (>= 2*MIN_KEEP so the halving
 * strictly decreases) -> drop PRIMARY at the floor -> CORE untouched; if
 * CORE alone busts the cap, record over_cap and write anyway (a too-big
 * bundle beats a missing one — the cap exists for the 2 GB budget, and the
 * CORE set is a few KiB by construction). */
static void enforce_cap(kn_diag_bundle *b, size_t target)
{
    for (;;) {
        int idx;
        if (bundle_total(b) <= target) { b->over_cap = 0; return; }
        idx = pick_largest(b, KN_DIAG_PRIO_SECONDARY, 1);
        if (idx >= 0) { art_drop(&b->arts[idx]); continue; }
        idx = pick_largest(b, KN_DIAG_PRIO_PRIMARY, 2u * KN_DIAG_MIN_KEEP);
        if (idx >= 0) {
            if (art_tail_keep(&b->arts[idx], b->arts[idx].len / 2,
                              b->arts[idx].orig_len) != 0)
                art_drop(&b->arts[idx]);
            continue;
        }
        idx = pick_largest(b, KN_DIAG_PRIO_PRIMARY, 1);
        if (idx >= 0) { art_drop(&b->arts[idx]); continue; }
        b->over_cap = 1;
        return;
    }
}

/* Takes ownership of `data` (heap). Inserts into the table and re-runs the
 * cap policy so RAM stays bounded at add time, not only at finish. */
static int art_insert(kn_diag_bundle *b, const char *zip_path,
                      unsigned char *data, size_t len, int prio)
{
    kn_diag_art *a;
    if (b->narts >= KN_DIAG_MAX_ARTIFACTS) {
        free(data);
        b->arts_overflow++;
        return -3;
    }
    a = &b->arts[b->narts];
    memset(a, 0, sizeof *a);
    norm_zip_path(a->name, sizeof a->name, zip_path);
    a->data = data;
    a->len = len;
    a->orig_len = (unsigned long long)len;
    a->prio = prio;
    b->narts++;
    enforce_cap(b, b->cap);
    return 0;
}

/* ------------------------------------------------------------------ begin */

kn_diag_bundle *kn_diag_begin(const char *out_dir, size_t cap_bytes)
{
    kn_diag_bundle *b;
    if (!out_dir) out_dir = "logs";
    if (cap_bytes == 0) cap_bytes = KN_DIAG_DEFAULT_CAP;
    if (cap_bytes < 64u * 1024u) cap_bytes = 64u * 1024u;   /* sane floor */
    b = (kn_diag_bundle *)calloc(1, sizeof *b);
    if (!b) return NULL;
    if (strlen(out_dir) >= sizeof b->out_dir) {            /* would truncate */
        free(b);
        return NULL;
    }
    snprintf(b->out_dir, sizeof b->out_dir, "%s", out_dir);
    b->cap = cap_bytes;
    return b;
}

void kn_diag_abort(kn_diag_bundle *b)
{
    size_t i;
    if (!b) return;
    for (i = 0; i < b->narts; ++i) free(b->arts[i].data);
    free(b);
}

int kn_diag_has(kn_diag_bundle *b, const char *zip_path)
{
    return (b && zip_path) ? (art_find(b, zip_path) >= 0) : 0;
}

int kn_diag_add_mem(kn_diag_bundle *b, const char *zip_path,
                    const void *data, size_t len, int prio)
{
    int rc;
    size_t file_cap;
    unsigned char *copy;
    if (!b || !zip_path || !zip_path[0] || (!data && len)) return -1;
    if (strlen(zip_path) >= KN_DIAG_NAME_MAX) return -1;   /* name too long */
    if (art_find(b, zip_path) >= 0) return -2;
    if (prio < KN_DIAG_PRIO_CORE || prio > KN_DIAG_PRIO_SECONDARY) prio = KN_DIAG_PRIO_PRIMARY;

    /* Single artifact bigger than the cap: tail-keep at add time (§7.8 cap,
     * and the 2 GB RAM budget — never hold more than the cap in one piece). */
    file_cap = b->cap < KN_DIAG_ART_FILE_CAP ? b->cap : KN_DIAG_ART_FILE_CAP;
    if (len > file_cap) {
        char marker[96];
        size_t mlen = (size_t)snprintf(marker, sizeof marker,
            "[kn_diag] truncated: kept newest %u of %llu bytes\n",
            (unsigned)file_cap, (unsigned long long)len);
        copy = (unsigned char *)malloc(mlen + file_cap);
        if (!copy) return -1;
        memcpy(copy, marker, mlen);
        memcpy(copy + mlen, (const unsigned char *)data + (len - file_cap), file_cap);
        len = mlen + file_cap;
    } else {
        copy = (unsigned char *)malloc(len ? len : 1);
        if (!copy) return -1;
        if (len) memcpy(copy, data, len);
    }
    rc = art_insert(b, zip_path, copy, len, prio);
    return rc;
}

int kn_diag_add_file(kn_diag_bundle *b, const char *zip_path,
                     const char *src_path, int prio)
{
    FILE *f;
    long sz;
    size_t file_cap, want;
    unsigned char *buf;
    size_t rd;
    int rc;

    if (!b || !zip_path || !zip_path[0] || !src_path) return -1;
    if (strlen(zip_path) >= KN_DIAG_NAME_MAX) return -1;   /* name too long */
    if (art_find(b, zip_path) >= 0) return -2;

    f = fopen(src_path, "rb");
    if (!f) return -1;
    /* fseek/ftell on SOMEONE ELSE'S file is fine — the §7.12 ban is about
     * seeking over our own output while composing it. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);

    file_cap = b->cap < KN_DIAG_ART_FILE_CAP ? b->cap : KN_DIAG_ART_FILE_CAP;
    want = (size_t)sz > file_cap ? file_cap : (size_t)sz;
    if ((size_t)sz > want && fseek(f, sz - (long)want, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    /* marker headroom + payload */
    buf = (unsigned char *)malloc(want + 96 + ((size_t)sz > want ? 96 : 0));
    if (!buf) { fclose(f); return -1; }
    rd = 0;
    if (want) rd = fread(buf, 1, want, f);
    fclose(f);
    if (rd != want) { free(buf); return -1; }

    if ((size_t)sz > want) {
        /* prepend the truncation marker, keep the NEWEST bytes (§7.8) */
        char marker[96];
        size_t mlen = (size_t)snprintf(marker, sizeof marker,
            "[kn_diag] source truncated: kept newest %u of %ld bytes\n",
            (unsigned)want, sz);
        memmove(buf + mlen, buf, want);
        memcpy(buf, marker, mlen);
        rd = want + mlen;
    }
    rc = art_insert(b, zip_path, buf, rd, prio);
    return rc;
}

/* ---------------------------------------------------------------- verdicts */

int kn_diag_verdict(kn_diag_bundle *b, const char *subsystem,
                    kn_diag_status status, const char *reason,
                    const char *evidence)
{
    kn_diag_verdict_rec *v;
    if (!b || !subsystem || !subsystem[0]) return -1;
    if (b->nverdicts >= KN_DIAG_MAX_VERDICTS) return -3;
    v = &b->verdicts[b->nverdicts++];
    memset(v, 0, sizeof *v);
    snprintf(v->subsystem, sizeof v->subsystem, "%s", subsystem);
    v->status = (int)status;
    snprintf(v->reason, sizeof v->reason, "%s", reason ? reason : "");
    if (evidence) snprintf(v->evidence, sizeof v->evidence, "%s", evidence);
    return 0;
}

/* ------------------------------------------------------------ collect_logs */

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* "app.1.jsonl" -> generation 1; "app.log" -> 0; "app.12.log" -> 12. */
static unsigned log_generation(const char *name)
{
    const char *dot_ext = strrchr(name, '.');
    const char *p;
    if (!dot_ext || dot_ext == name) return 0;
    /* walk back over the segment before the extension */
    p = dot_ext - 1;
    while (p > name && p[0] >= '0' && p[0] <= '9') --p;
    if (p[0] != '.' || p + 1 >= dot_ext) return 0;
    return (unsigned)atoi(p + 1);
}

int kn_diag_collect_logs(kn_diag_bundle *b, const char *logs_dir)
{
    char (*names)[KN_DIAG_NAME_MAX];
    int n, i, added = 0;
    if (!b || !logs_dir) return -1;
    names = (char (*)[KN_DIAG_NAME_MAX])malloc(KN_DIAG_MAX_ARTIFACTS * KN_DIAG_NAME_MAX);
    if (!names) return -1;
    n = kn_diag_list_dir(logs_dir, names, KN_DIAG_MAX_ARTIFACTS);
    if (n < 0) { free(names); return -1; }
    qsort(names, (size_t)n, KN_DIAG_NAME_MAX, name_cmp);   /* deterministic */
    for (i = 0; i < n; ++i) {
        const char *nm = names[i];
        size_t nlen = strlen(nm);
        char zip_path[KN_DIAG_NAME_MAX + 8];
        char src[KN_DIAG_PATH_MAX];
        int is_log  = nlen > 4 && strcmp(nm + nlen - 4, ".log") == 0;
        int is_json = nlen > 6 && strcmp(nm + nlen - 6, ".jsonl") == 0;
        int rc;
        if (!is_log && !is_json) continue;
        snprintf(zip_path, sizeof zip_path, "logs/%s", nm);
        snprintf(src, sizeof src, "%s/%s", logs_dir, nm);
        rc = kn_diag_add_file(b, zip_path, src,
                              log_generation(nm) == 0 ? KN_DIAG_PRIO_PRIMARY
                                                      : KN_DIAG_PRIO_SECONDARY);
        if (rc == 0) added++;
        else if (rc != -2 && rc != -3) { free(names); return -1; }   /* read error */
    }
    free(names);
    return added;
}

/* ---------------------------------------------------------------- build.json */

#define KN_DIAG_XSTR(x) KN_DIAG_STR(x)
#define KN_DIAG_STR(x)  #x

#ifndef KN_DIAG_BUILD_GIT_REV
#  define KN_DIAG_BUILD_GIT_REV "unknown"
#endif
#ifndef KN_DIAG_BUILD_GIT_DIRTY
#  define KN_DIAG_BUILD_GIT_DIRTY "unknown"
#endif
#ifndef KN_DIAG_BUILD_FLAGS
#  define KN_DIAG_BUILD_FLAGS "unknown (inject KN_DIAG_BUILD_FLAGS at compile time)"
#endif
#ifndef KN_DIAG_BUILD_EXTRA
#  define KN_DIAG_BUILD_EXTRA ""
#endif

#if defined(__GNUC__)
#  define KN_DIAG_COMPILER "gcc " __VERSION__
#elif defined(_MSC_VER)
#  define KN_DIAG_COMPILER "msvc"
#else
#  define KN_DIAG_COMPILER "unknown"
#endif

#ifdef _WIN32_WINNT
#  define KN_DIAG_WINNT_EFFECTIVE KN_DIAG_XSTR(_WIN32_WINNT)
#else
#  define KN_DIAG_WINNT_EFFECTIVE "undefined"
#endif
#ifdef WINVER
#  define KN_DIAG_WINVER_EFFECTIVE KN_DIAG_XSTR(WINVER)
#else
#  define KN_DIAG_WINVER_EFFECTIVE "undefined"
#endif
#ifdef NTDDI_VERSION
#  define KN_DIAG_NTDI_EFFECTIVE KN_DIAG_XSTR(NTDDI_VERSION)
#else
#  define KN_DIAG_NTDI_EFFECTIVE "undefined"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  define KN_DIAG_ARCH "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
#  define KN_DIAG_ARCH "x86"
#else
#  define KN_DIAG_ARCH "other"
#endif

size_t kn_diag_render_build_json(char *buf, size_t bufsz)
{
    size_t o = 0;
    int trunc = 0;
    char ts[32], esc[512], escx[512];
    if (!buf || bufsz == 0) return 0;
    utc_rfc3339(ts);
    json_escape(esc, sizeof esc, KN_DIAG_BUILD_FLAGS);
    json_escape(escx, sizeof escx, KN_DIAG_BUILD_EXTRA);
    KN_APP(buf, bufsz, o, &trunc,
        "{\n"
        "  \"git_rev\": \"%s\",\n"
        "  \"git_dirty\": \"%s\",\n"
        "  \"compiler\": \"%s\",\n"
        "  \"flags\": \"%s\",\n"
        "  \"win32_winnt_effective\": \"%s\",\n"
        "  \"winver_effective\": \"%s\",\n"
        "  \"ntddi_effective\": \"%s\",\n"
        "  \"arch\": \"%s\",\n"
        "  \"pointer_bytes\": %u,\n"
        "  \"libs\": {\"aes\": \"tiny-AES-c AES128 ECB+CBC+CTR\", "
        "\"sha1\": \"crypto-algorithms (Brad Conte)\", "
        "\"fec\": \"in-tree Cauchy RS GF(2^8)/0x11d (nvst.rs a49b124)\", "
        "\"srtp\": \"in-tree phase1+2 (AES-CM/HMAC-SHA1, RFC 3711 KDF)\"},\n"
        "  \"extras\": \"%s\",\n"
        "  \"generated_utc\": \"%s\",\n"
        "  \"truncated\": %s\n"
        "}\n",
        KN_DIAG_BUILD_GIT_REV, KN_DIAG_BUILD_GIT_DIRTY, KN_DIAG_COMPILER, esc,
        KN_DIAG_WINNT_EFFECTIVE, KN_DIAG_WINVER_EFFECTIVE, KN_DIAG_NTDI_EFFECTIVE,
        KN_DIAG_ARCH, (unsigned)sizeof(void *), escx, ts,
        trunc ? "true" : "false");
    return o;
}

/* --------------------------------------------------------------- summary */

/* Reserve for summary.json while enforcing the cap on the rest. The summary
 * is bounded: verdicts <= 16 * ~350 B, artifacts <= 64 * ~120 B => < 16 KiB
 * even with every table full. */
#define KN_DIAG_SUMMARY_MARGIN (16u * 1024u)

static const char *status_name(int s)
{
    switch (s) {
    case KN_DIAG_OK:       return "ok";
    case KN_DIAG_DEGRADED: return "degraded";
    case KN_DIAG_FAILED:    return "failed";
    default:               return "unknown";
    }
}

static size_t render_summary(kn_diag_bundle *b, char *buf, size_t bufsz)
{
    size_t o = 0, i;
    int trunc = 0;
    char ts[32], e_op[96], e_code[96], e_reason[384];
    utc_rfc3339(ts);
    json_escape(e_op, sizeof e_op, b->trigger_op[0] ? b->trigger_op : "manual");
    json_escape(e_code, sizeof e_code, b->trigger_code);
    json_escape(e_reason, sizeof e_reason, b->trigger_reason);

    KN_APP(buf, bufsz, o, &trunc,
        "{\n"
        "  \"bundle_version\": 1,\n"
        "  \"generated_utc\": \"%s\",\n"
        "  \"trigger\": {\"op\": \"%s\", \"code\": \"%s\", \"reason\": \"%s\"},\n"
        "  \"cap_bytes\": %llu,\n"
        "  \"over_cap\": %s,\n"
        "  \"verdicts\": [",
        ts, e_op, e_code, e_reason,
        (unsigned long long)b->cap, b->over_cap ? "true" : "false");
    for (i = 0; i < b->nverdicts; ++i) {
        char es[KN_DIAG_SUBSYS_MAX * 2], er[KN_DIAG_REASON_MAX * 2], ee[KN_DIAG_NAME_MAX * 2];
        json_escape(es, sizeof es, b->verdicts[i].subsystem);
        json_escape(er, sizeof er, b->verdicts[i].reason);
        json_escape(ee, sizeof ee, b->verdicts[i].evidence);
        KN_APP(buf, bufsz, o, &trunc, "%s\n    {\"subsystem\": \"%s\", \"status\": \"%s\", "
               "\"reason\": \"%s\", \"evidence\": \"%s\"}",
               i ? "," : "", es, status_name(b->verdicts[i].status), er, ee);
    }
    KN_APP(buf, bufsz, o, &trunc, "\n  ],\n  \"artifacts\": [");
    for (i = 0; i < b->narts; ++i) {
        const kn_diag_art *a = &b->arts[i];
        char en[KN_DIAG_NAME_MAX * 2];
        json_escape(en, sizeof en, a->name);
        KN_APP(buf, bufsz, o, &trunc,
               "%s\n    {\"name\": \"%s\", \"prio\": %d, \"bytes\": %llu, \"orig_bytes\": %llu%s%s}",
               i ? "," : "", en, a->prio,
               (unsigned long long)(a->dropped ? 0 : a->len),
               a->orig_len,
               a->truncated ? ", \"truncated\": true" : "",
               a->dropped ? ", \"dropped\": true" : "");
    }
    KN_APP(buf, bufsz, o, &trunc,
        "\n  ],\n  \"arts_overflow\": %llu,\n  \"summary_truncated\": %s\n}\n",
        b->arts_overflow, (trunc || b->summary_truncated) ? "true" : "false");
    b->summary_truncated = trunc;
    return o;
}

/* ------------------------------------------------------------- zip writer */

typedef struct {
    FILE    *f;
    int      error;
    uint32_t pos;
} zip_w;

static void zw_write(zip_w *z, const void *data, size_t len)
{
    if (z->error) return;
    if (len && fwrite(data, 1, len, z->f) != len) z->error = 1;
    z->pos += (uint32_t)len;
}

static void zw_u16(zip_w *z, uint16_t v) { unsigned char b[2]; wr16(b, v); zw_write(z, b, 2); }
static void zw_u32(zip_w *z, uint32_t v) { unsigned char b[4]; wr32(b, v); zw_write(z, b, 4); }

static void zw_local_header(zip_w *z, const char *name, uint32_t crc,
                            uint32_t size, uint16_t dostime, uint16_t dosdate)
{
    uint16_t nlen = (uint16_t)strlen(name);
    zw_u32(z, 0x04034b50u);      /* local file header signature */
    zw_u16(z, 20);               /* version needed to extract */
    zw_u16(z, 0);                /* flags: no data descriptor, names are ASCII */
    zw_u16(z, 0);                /* method: store */
    zw_u16(z, dostime);
    zw_u16(z, dosdate);
    zw_u32(z, crc);
    zw_u32(z, size);             /* compressed == uncompressed (store) */
    zw_u32(z, size);
    zw_u16(z, nlen);
    zw_u16(z, 0);                /* extra len */
    zw_write(z, name, nlen);
}

static void zw_central_header(zip_w *z, const char *name, uint32_t crc,
                              uint32_t size, uint16_t dostime, uint16_t dosdate,
                              uint32_t offset)
{
    uint16_t nlen = (uint16_t)strlen(name);
    zw_u32(z, 0x02014b50u);      /* central directory header signature */
    zw_u16(z, 20);               /* version made by */
    zw_u16(z, 20);               /* version needed */
    zw_u16(z, 0);                /* flags */
    zw_u16(z, 0);                /* method: store */
    zw_u16(z, dostime);
    zw_u16(z, dosdate);
    zw_u32(z, crc);
    zw_u32(z, size);
    zw_u32(z, size);
    zw_u16(z, nlen);
    zw_u16(z, 0);                /* extra len */
    zw_u16(z, 0);                /* comment len */
    zw_u16(z, 0);                /* disk number start */
    zw_u16(z, 0);                /* internal attrs */
    zw_u32(z, 0);                /* external attrs */
    zw_u32(z, offset);           /* local header offset */
    zw_write(z, name, nlen);
}

/* ------------------------------------------------------------------ finish */

int kn_diag_finish(kn_diag_bundle *b, const char *trigger_op,
                   const char *trigger_code, const char *trigger_reason,
                   char *out_path, size_t out_path_sz)
{
    char utc[20];
    char zip_path[KN_DIAG_PATH_MAX];
    char *summary;
    size_t summary_len = 0, target;
    size_t i;
    FILE *f;
    zip_w z;
    uint16_t dostime, dosdate;
    uint16_t entries = 0;
    uint32_t cd_offset, cd_size;
    uint32_t summary_crc;

    if (!b || !out_path || out_path_sz < KN_DIAG_PATH_MAX) return -1;
    if (b->narts == 0) return -2;                     /* refuse empty bundles */

    snprintf(b->trigger_op, sizeof b->trigger_op, "%s", trigger_op ? trigger_op : "");
    snprintf(b->trigger_code, sizeof b->trigger_code, "%s", trigger_code ? trigger_code : "");
    snprintf(b->trigger_reason, sizeof b->trigger_reason, "%s", trigger_reason ? trigger_reason : "");

    /* Final cap pass with room reserved for the summary itself. */
    target = b->cap > KN_DIAG_SUMMARY_MARGIN + 1024u
           ? b->cap - KN_DIAG_SUMMARY_MARGIN : b->cap / 2;
    enforce_cap(b, target);

    summary = (char *)malloc(KN_DIAG_SUMMARY_MARGIN);
    if (!summary) return -1;
    summary_len = render_summary(b, summary, KN_DIAG_SUMMARY_MARGIN);
    summary_crc = kn_diag_crc32(0, summary, summary_len);

    utc_compact(utc);
    snprintf(zip_path, sizeof zip_path, "%s/diag-%s-%08x.zip",
             b->out_dir, utc, shortid8());
    snprintf(out_path, out_path_sz, "%s", zip_path);

    f = fopen(zip_path, "wb");
    if (!f) { free(summary); return -3; }
    memset(&z, 0, sizeof z);
    z.f = f;
    dos_stamp(&dostime, &dosdate);

    /* summary.json first — "this is what gets read first" (§7.8). */
    zw_local_header(&z, "summary.json", summary_crc, (uint32_t)summary_len,
                    dostime, dosdate);
    zw_write(&z, summary, summary_len);
    entries = 1;

    for (i = 0; i < b->narts && !z.error; ++i) {
        kn_diag_art *a = &b->arts[i];
        uint32_t crc;
        if (a->dropped) continue;
        a->offset = z.pos;
        crc = kn_diag_crc32(0, a->data, a->len);
        zw_local_header(&z, a->name, crc, (uint32_t)a->len, dostime, dosdate);
        zw_write(&z, a->data, a->len);
        entries++;
    }

    cd_offset = z.pos;
    /* central directory: summary entry, then artifacts in table order */
    zw_central_header(&z, "summary.json", summary_crc, (uint32_t)summary_len,
                      dostime, dosdate, 0);
    for (i = 0; i < b->narts && !z.error; ++i) {
        const kn_diag_art *a = &b->arts[i];
        uint32_t crc;
        if (a->dropped) continue;
        crc = kn_diag_crc32(0, a->data, a->len);
        zw_central_header(&z, a->name, crc, (uint32_t)a->len, dostime, dosdate,
                          a->offset);
    }
    cd_size = z.pos - cd_offset;

    zw_u32(&z, 0x06054b50u);     /* EOCD signature */
    zw_u16(&z, 0);               /* this disk */
    zw_u16(&z, 0);               /* disk with CD */
    zw_u16(&z, entries);
    zw_u16(&z, entries);
    zw_u32(&z, cd_size);
    zw_u32(&z, cd_offset);
    zw_u16(&z, 0);               /* comment len */

    fflush(f);
    if (!z.error) (void)kn_diag_fsync(f);              /* §7.12: seal it */
    if (fclose(f) != 0) z.error = 1;
    free(summary);

    if (z.error) { remove(zip_path); return -3; }
    return 0;
}

/* ------------------------------------------------------- auto-integration */

static kn_diag_mutex   g_hook_mtx;
static int             g_hook_mtx_ready;   /* set once, at install (startup, single-threaded) */
static int             g_installed;
static int             g_in_hook;          /* same-thread recursion guard */
static char            g_hook_dir[KN_DIAG_PATH_MAX];
static char            g_last_path[KN_DIAG_PATH_MAX];
static int             g_last_set;
static unsigned long long g_last_bytes;

/* Auto build.json + state.json + logging verdict, then finish. */
static kn_diag_selftest_provider_fn g_st_provider;

void kn_diag_set_selftest_provider(kn_diag_selftest_provider_fn fn)
{
    g_st_provider = fn;      /* set once at startup, before threads */
}

int kn_diag_write_bundle(const char *logs_dir, const char *trigger_op,
                         const char *trigger_code, const char *trigger_reason,
                         char *out_path, size_t out_path_sz)
{
    kn_diag_bundle *b;
    kn_log_stats st;
    char build[4096];
    char state[1024];
    size_t n;
    int rc;

    if (!logs_dir) logs_dir = "logs";
    b = kn_diag_begin(logs_dir, 0);
    if (!b) return -1;

    /* logs/<channel>.*: current generations PRIMARY, rotated SECONDARY */
    (void)kn_diag_collect_logs(b, logs_dir);   /* dir may not exist yet: tolerate */

    /* build.json unless the caller supplied one */
    if (!kn_diag_has(b, "build.json")) {
        n = kn_diag_render_build_json(build, sizeof build);
        if (n) (void)kn_diag_add_mem(b, "build.json", build, n, KN_DIAG_PRIO_CORE);
    }

    /* state.json v1: the logging subsystem's own state (the richer state
     * machine registry lands with the subsystems that own state machines). */
    if (!kn_diag_has(b, "state.json")) {
        memset(&st, 0, sizeof st);
        if (kn_log_is_initialized()) {
            kn_log_get_stats(&st);
            snprintf(state, sizeof state,
                "{\"subsystems\":{\"logging\":{\"initialized\":true,"
                "\"events_emitted\":%llu,\"events_filtered\":%llu,"
                "\"lines_written\":%llu,\"json_written\":%llu,"
                "\"bytes_written\":%llu,\"rotations\":%llu,"
                "\"dropped_fields\":%llu,\"truncated_lines\":%llu,"
                "\"write_errors\":%llu,\"secret_values_redacted\":%llu,"
                "\"sealed\":%d,\"amplified\":%d}}}\n",
                st.events_emitted, st.events_filtered, st.lines_written,
                st.json_written, st.bytes_written, st.rotations,
                st.dropped_fields, st.truncated_lines, st.write_errors,
                st.secret_values_redacted, st.sealed, st.amplified);
        } else {
            snprintf(state, sizeof state,
                "{\"subsystems\":{\"logging\":{\"initialized\":false}}}\n");
        }
        (void)kn_diag_add_mem(b, "state.json", state, strlen(state),
                              KN_DIAG_PRIO_CORE);
    }

    /* selftest.json (§7.7 step 4): run the tests RELEVANT to this failure via
     * the provider wired at startup (kn_selftest_render_relevant). Skipped
     * when the caller already supplied the artifact or no provider is set. */
    if (g_st_provider && !kn_diag_has(b, "selftest.json")) {
        char *st_buf = (char *)malloc(KN_DIAG_SUMMARY_MARGIN);
        if (st_buf) {
            size_t st_n = g_st_provider(trigger_op ? trigger_op : "",
                                        trigger_code ? trigger_code : "",
                                        st_buf, KN_DIAG_SUMMARY_MARGIN);
            if (st_n)
                (void)kn_diag_add_mem(b, "selftest.json", st_buf, st_n,
                                      KN_DIAG_PRIO_CORE);
            free(st_buf);
        }
    }

    /* "logging" verdict from stats (§7.8: one verdict per subsystem) */
    if (kn_log_is_initialized()) {
        kn_log_get_stats(&st);
        if (st.write_errors == 0)
            (void)kn_diag_verdict(b, "logging", KN_DIAG_OK,
                                  "no write errors", "state.json#/subsystems/logging");
        else
            (void)kn_diag_verdict(b, "logging", KN_DIAG_DEGRADED,
                                  "sink write errors recorded", "state.json#/subsystems/logging");
    } else {
        (void)kn_diag_verdict(b, "logging", KN_DIAG_FAILED,
                              "logging subsystem not initialized", "");
    }

    rc = kn_diag_finish(b, trigger_op, trigger_code, trigger_reason,
                        out_path, out_path_sz);
    kn_diag_abort(b);
    return rc;
}

/* The failure hook (§7.7 step 3). Called by kn_log_on_failure AFTER the
 * ERROR event hit the sealed sinks, so the bundle captures it. Callbacks may
 * log (knlog.h contract) — we report the bundle path on the diag channel. */
static void kn_diag_failure_hook(const char *op, const char *code,
                                 const char *reason, void *userdata)
{
    char path[KN_DIAG_PATH_MAX];
    long long bytes = -1;
    int rc;
    (void)userdata;

    if (g_in_hook) return;            /* a bundle-write ERROR must not recurse */
    g_in_hook = 1;

    path[0] = '\0';
    rc = kn_diag_write_bundle(g_hook_dir, op, code, reason, path, sizeof path);
    if (rc == 0) {
        FILE *f = fopen(path, "rb");
        if (f) {
            if (fseek(f, 0, SEEK_END) == 0) bytes = ftell(f);
            fclose(f);
        }
        if (g_hook_mtx_ready) kn_diag_mutex_lock(&g_hook_mtx);
        snprintf(g_last_path, sizeof g_last_path, "%s", path);
        g_last_set = 1;
        g_last_bytes = bytes > 0 ? (unsigned long long)bytes : 0;
        if (g_hook_mtx_ready) kn_diag_mutex_unlock(&g_hook_mtx);

        {
            kn_lctx c;
            kn_lctx_init(&c, KN_CH_DIAG, KN_INFO, "bundle_written");
            kn_lctx_str(&c, "path", path);
            kn_lctx_i64(&c, "bytes", bytes);
            kn_lctx_str(&c, "trigger_op", op ? op : "unknown");
            kn_lctx_str(&c, "trigger_code", code ? code : "unknown");
            kn_log_emit(&c);
        }
    } else {
        kn_lctx c;
        kn_lctx_init(&c, KN_CH_DIAG, KN_ERROR, "bundle_write_failed");
        kn_lctx_i64(&c, "rc", rc);
        kn_lctx_str(&c, "trigger_op", op ? op : "unknown");
        kn_lctx_str(&c, "next_action", "check log dir permissions and free disk");
        kn_log_emit(&c);
        /* NOTE: deliberately NOT kn_log_on_failure here — that would recurse
         * into this hook (guarded anyway) and add no information. */
    }
    g_in_hook = 0;
}

int kn_diag_install(const char *logs_dir)
{
    int rc;
    if (!logs_dir) logs_dir = "logs";
    if (!g_hook_mtx_ready) {           /* startup, single-threaded (documented) */
        kn_diag_mutex_init(&g_hook_mtx);
        g_hook_mtx_ready = 1;
    }
    snprintf(g_hook_dir, sizeof g_hook_dir, "%s", logs_dir);
    if (g_installed) return 0;
    rc = kn_log_failure_cb_register(kn_diag_failure_hook, NULL);
    if (rc == 0) g_installed = 1;
    return rc;
}

const char *kn_diag_last_bundle_path(void)
{
    /* Static storage; a concurrent hook write is best-effort (the buffer is
     * always NUL-terminated and only replaced, never resized). Callers that
     * must hold the value across calls copy it. */
    return g_last_set ? g_last_path : NULL;
}
