/* test_port.h — tiny portability shims so the unit tests compile and RUN
 * unmodified both on the Linux host (fast iteration) and on the Windows 7
 * target (real-environment verification). Production code does not use this. */
#ifndef KN_TEST_PORT_H
#define KN_TEST_PORT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define kn_test_rmdir(p) _rmdir(p)
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  define kn_test_rmdir(p) rmdir(p)
#endif

/* remove() is standard C and works on both (files only). */
#define kn_test_unlink(p) remove(p)

/* Not every test binary uses every helper — keep -Werror builds clean. */
#if defined(__GNUC__)
#  define KN_TEST_UNUSED __attribute__((unused))
#else
#  define KN_TEST_UNUSED
#endif

/* Read a whole (small) file into a malloc'd NUL-terminated buffer.
 * Returns NULL if the file does not exist. Caller frees. */
KN_TEST_UNUSED static char *kn_test_read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    size_t rd;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/* Minimal check harness: prints one line per failure, summary at the end. */
static int kn_test_failures;
static int kn_test_checks;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        kn_test_checks++;                                                   \
        if (!(cond)) {                                                      \
            kn_test_failures++;                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (name));         \
        }                                                                   \
    } while (0)

#define CHECK_STR_CONTAINS(hay, needle, name)                               \
    do {                                                                    \
        kn_test_checks++;                                                   \
        if (!(hay) || !strstr((hay), (needle))) {                           \
            kn_test_failures++;                                             \
            printf("FAIL %s:%d: %s (expected substring: %s)\n",             \
                   __FILE__, __LINE__, (name), (needle));                   \
        }                                                                   \
    } while (0)

#define CHECK_STR_NOT_CONTAINS(hay, needle, name)                           \
    do {                                                                    \
        kn_test_checks++;                                                   \
        if ((hay) && strstr((hay), (needle))) {                             \
            kn_test_failures++;                                             \
            printf("FAIL %s:%d: %s (forbidden substring: %s)\n",            \
                   __FILE__, __LINE__, (name), (needle));                   \
        }                                                                   \
    } while (0)

KN_TEST_UNUSED static int kn_test_summary(const char *suite)
{
    printf("%s: %d checks, %d failures -> %s\n", suite, kn_test_checks,
           kn_test_failures, kn_test_failures ? "FAIL" : "PASS");
    return kn_test_failures ? 1 : 0;
}

#endif /* KN_TEST_PORT_H */
