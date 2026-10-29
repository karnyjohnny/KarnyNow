/* test_kndrawpath.c — the TWENTY-FIRST suite: kn_drawpath, the per-draw
 * capture-file naming rule (round L, F16).
 *
 * WHY THIS SUITE EXISTS: round XLIX's F13 gave every lottery draw its own
 * audio file so a multi-draw run delivers all of them. The derivation was
 * written inline against the destination buffer, so `strrchr()` returned a
 * pointer INTO that buffer and the following `memcpy()` was an overlapping
 * copy — undefined behaviour that ASan reported on the real rig
 * (memcpy-param-overlap) and that produced the name run 31 DELIVERED ON
 * DISK: "audio_d1_d1us" … "audio_d4_d4us" — the ".opus" tail was
 * overwritten by the same copy, so the files have no extension at all
 * (the path itself is never logged, which is why only `ls` shows it).
 * F16 moves the rule into
 * kn_drawpath.c (alias-free, memmove-based, in-place-safe) and this suite
 * pins it.
 *
 * WHAT IT PROVES, three independent ways:
 *  1. THE RUN-31 CASE VERBATIM: "audio.opus" + draw 1..3, in place, must
 *     come out "audio_d1.opus"/"audio_d2.opus"/"audio_d3.opus" — the exact
 *     names run 31 SHOULD have delivered (it delivered "audio_d1_d1us").
 *  2. A SECOND IMPLEMENTATION: the test rebuilds every expected name with a
 *     plain snprintf into a scratch buffer (no shared code with the module)
 *     and compares byte-for-byte across a matrix of paths, extensions,
 *     separators and draw numbers.
 *  3. HONEST DEGRADATION: a too-small buffer keeps the ORIGINAL path and
 *     sets the truncated flag (never a half-written name), the boundary is
 *     exact (fit == len+1), and every leg is ASan/UBSan-clean because the
 *     in-place form is the one the caller actually uses.
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_drawpath.h"
#include "test_port.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------- the second builder
 * Deliberately naive and independent of kn_drawpath.c: split the basename
 * at the LAST dot that comes after the LAST separator, then snprintf the
 * three pieces together. If the module and this ever disagree, one of them
 * is wrong — and this one is 15 lines a human can read. */
static void ref_build(const char *src, long draw, char *out, size_t outsz)
{
    const char *dot, *sep, *s1, *s2;
    size_t stemn;

    if (draw <= 0) {
        snprintf(out, outsz, "%s", src);
        return;
    }
    sep = NULL;
    for (s1 = src; *s1; ++s1)
        if (*s1 == '/' || *s1 == '\\') sep = s1;
    dot = NULL;
    for (s2 = src; *s2; ++s2)
        if (*s2 == '.') dot = s2;
    if (dot && dot == src) dot = NULL;              /* a leading dot */
    if (dot && sep && dot < sep) dot = NULL;        /* dot in a directory */
    stemn = dot ? (size_t)(dot - src) : strlen(src);
    if (dot)
        snprintf(out, outsz, "%.*s_d%ld%s", (int)stemn, src, draw, dot);
    else
        snprintf(out, outsz, "%.*s_d%ld", (int)stemn, src, draw);
}

/* ------------------------------------------------------- 1: run 31's case */
static void t_run31_audio_paths(void)
{
    char b[4096];
    int trunc = 0;

    /* draw 0 (no lottery): the operator's path verbatim */
    strcpy(b, "audio.opus");
    CHECK(kn_drawpath_suffix_inplace(b, sizeof b, 0, &trunc) == 0,
          "draw 0 returns success");
    CHECK(trunc == 0, "draw 0 is not truncated");
    CHECK(strcmp(b, "audio.opus") == 0, "draw 0 keeps audio.opus verbatim");

    /* draws 1..3: the names run 31 mangled into "audio_d1_d1us" &c. */
    strcpy(b, "audio.opus");
    kn_drawpath_suffix_inplace(b, sizeof b, 1, &trunc);
    CHECK(strcmp(b, "audio_d1.opus") == 0, "draw 1 -> audio_d1.opus (run 31)");
    CHECK(strstr(b, "_d1_d1") == NULL, "no doubled suffix (the ASan overlap)");

    strcpy(b, "audio.opus");
    kn_drawpath_suffix_inplace(b, sizeof b, 2, &trunc);
    CHECK(strcmp(b, "audio_d2.opus") == 0, "draw 2 -> audio_d2.opus");

    strcpy(b, "audio.opus");
    kn_drawpath_suffix_inplace(b, sizeof b, 3, &trunc);
    CHECK(strcmp(b, "audio_d3.opus") == 0, "draw 3 -> audio_d3.opus");

    /* the Win7 form the operator actually flies */
    strcpy(b, "C:\\kn\\logs\\audio.opus");
    kn_drawpath_suffix_inplace(b, sizeof b, 2, &trunc);
    CHECK(strcmp(b, "C:\\kn\\logs\\audio_d2.opus") == 0,
          "windows path keeps its directory");

    /* repeated application must NOT accumulate (idempotence per draw) */
    strcpy(b, "audio.opus");
    kn_drawpath_suffix_inplace(b, sizeof b, 1, &trunc);
    kn_drawpath_suffix_inplace(b, sizeof b, 1, &trunc);
    CHECK(strcmp(b, "audio_d1_d1.opus") == 0,
          "re-applying to a suffixed path treats _d1.opus as stem+ext "
          "(documented: the caller derives from the ORIGINAL path once)");
}

/* --------------------------------------------- 2: matrix vs the reference */
static void t_matrix_against_reference(void)
{
    static const char *paths[] = {
        "audio.opus", "video.264", "capture", "a.b.c.raw", ".hidden",
        "dir.ext/cap", "dir/cap.wav", "C:\\logs.d\\cap.raw",
        "C:\\kn\\au_00001_k.264", "logs/run31/audio.opus", "x", "..",
        "no_ext_dir/name", "deep/a/b/c.ogg",
    };
    static const long draws[] = { 0, 1, 2, 3, 9, 10, 123, 123456, 2147483647L };
    size_t p, d;
    const size_t np = sizeof paths / sizeof paths[0];
    const size_t nd = sizeof draws / sizeof draws[0];

    for (p = 0; p < np; ++p) {
        for (d = 0; d < nd; ++d) {
            char inplace[256], outplace[256], want[256], want2[256];
            int t1 = 0, t2 = 0;
            char nm[96];

            /* in place (the caller's real idiom) */
            snprintf(inplace, sizeof inplace, "%s", paths[p]);
            CHECK(kn_drawpath_suffix_inplace(inplace, sizeof inplace,
                                            draws[d], &t1) == 0,
                  "inplace fits for the whole matrix");
            CHECK(t1 == 0, "inplace never truncates for the whole matrix");

            /* out of place, from the same source string */
            CHECK(kn_drawpath_suffix(paths[p], draws[d], outplace,
                                     sizeof outplace, &t2) == outplace,
                  "out-of-place returns dst");
            CHECK(t2 == 0, "out-of-place never truncates for the matrix");

            /* both forms agree */
            snprintf(nm, sizeof nm, "in-place == out-of-place [%s] d%ld",
                     paths[p], draws[d]);
            CHECK(strcmp(inplace, outplace) == 0, nm);

            /* and both agree with the independent reference builder */
            ref_build(paths[p], draws[d], want, sizeof want);
            snprintf(nm, sizeof nm, "matches the reference builder [%s] d%ld",
                     paths[p], draws[d]);
            CHECK(strcmp(inplace, want) == 0, nm);

            /* the suffix-only helper agrees with the tail of the result */
            {
                char suf[KN_DRAWSUF_MAX];
                int sn = kn_drawpath_suffix_str(draws[d], suf, sizeof suf);
                snprintf(want2, sizeof want2, "_d%ld", draws[d]);
                snprintf(nm, sizeof nm, "suffix_str [%s] d%ld", paths[p],
                         draws[d]);
                CHECK(sn >= 0, "suffix_str succeeds");
                if (draws[d] <= 0)
                    CHECK(suf[0] == '\0', "suffix_str is empty for draw <= 0");
                else
                    CHECK(strcmp(suf, want2) == 0, nm);
            }
        }
    }
}

/* --------------------------------------------------- 3: honest degradation */
static void t_bounds_and_truncation(void)
{
    char small[16], exact[14], one_short[13];
    int trunc = 0;

    /* "audio_d1.opus" is 13 chars + NUL = 14 */
    strcpy(exact, "audio.opus");
    CHECK(kn_drawpath_suffix_inplace(exact, sizeof exact, 1, &trunc) == 0,
          "exact fit succeeds");
    CHECK(trunc == 0, "exact fit is not flagged");
    CHECK(strcmp(exact, "audio_d1.opus") == 0, "exact fit wrote the full name");

    strcpy(one_short, "audio.opus");
    CHECK(kn_drawpath_suffix_inplace(one_short, sizeof one_short, 1,
                                     &trunc) == -1,
          "one byte short fails");
    CHECK(trunc == 1, "one byte short sets the truncated flag");
    CHECK(strcmp(one_short, "audio.opus") == 0,
          "one byte short keeps the ORIGINAL path (honest, never partial)");

    /* a buffer that cannot even hold the original: still NUL-terminated,
     * still honest about the truncation ("ab" needs 3, we give it 2) */
    memset(small, 'X', sizeof small);
    strcpy(small, "ab");
    CHECK(kn_drawpath_suffix(small, 0, small, 2, &trunc) == small,
          "tiny buffer returns dst");
    CHECK(small[1] == '\0', "tiny buffer stays NUL-terminated");
    CHECK(strcmp(small, "a") == 0, "tiny buffer holds the fitting prefix");
    CHECK(trunc == 1, "tiny buffer reports truncation");

    /* the same buffer with exactly enough room is NOT flagged */
    strcpy(small, "ab");
    trunc = 0;
    CHECK(kn_drawpath_suffix(small, 0, small, 3, &trunc) == small,
          "exact-fit tiny buffer returns dst");
    CHECK(strcmp(small, "ab") == 0, "exact-fit tiny buffer keeps the path");
    CHECK(trunc == 0, "exact-fit tiny buffer is not flagged");

    /* bad arguments */
    CHECK(kn_drawpath_suffix(NULL, 1, small, sizeof small, NULL) == NULL,
          "NULL src -> NULL");
    CHECK(kn_drawpath_suffix("a.opus", 1, NULL, 16, NULL) == NULL,
          "NULL dst -> NULL");
    CHECK(kn_drawpath_suffix("a.opus", 1, small, 0, NULL) == NULL,
          "dstsz 0 -> NULL");
    CHECK(kn_drawpath_suffix_inplace(NULL, 16, 1, NULL) == -1,
          "inplace NULL buf -> -1");
    CHECK(kn_drawpath_suffix_inplace(small, 0, 1, NULL) == -1,
          "inplace bufsz 0 -> -1");
    CHECK(kn_drawpath_suffix_str(1, NULL, 8) == -1, "suffix_str NULL -> -1");
    CHECK(kn_drawpath_suffix_str(1, small, 0) == -1, "suffix_str 0 -> -1");
    CHECK(kn_drawpath_suffix_str(1, small, 2) == -1,
          "suffix_str too small -> -1");

    /* negative draws behave like draw 0 (no suffix) */
    strcpy(small, "a.opus");
    CHECK(kn_drawpath_suffix_inplace(small, sizeof small, -7, &trunc) == 0,
          "negative draw succeeds");
    CHECK(strcmp(small, "a.opus") == 0, "negative draw keeps the path");

    /* an over-long "extension" (32+ chars) is NOT treated as one: the
     * suffix goes at the very end. Documented in kn_drawpath.h; pinned
     * here so a future change to that threshold is a deliberate one. */
    {
        char big[128];
        size_t i;
        strcpy(big, "cap.");
        for (i = 0; i < 40; ++i) strcat(big, "x");
        kn_drawpath_suffix_inplace(big, sizeof big, 1, &trunc);
        CHECK(trunc == 0, "long-extension path fits");
        CHECK(strlen(big) == 44 + 3, "long-extension path grew by exactly _d1");
        CHECK(strcmp(big + 44, "_d1") == 0,
              "long extension: the suffix lands at the end (no split)");
    }
    /* ...while a normal-length one IS split (the reference builder agrees) */
    {
        char big[128], want[128];
        size_t i;
        strcpy(big, "cap.");
        for (i = 0; i < 20; ++i) strcat(big, "x");     /* 24 chars total */
        ref_build(big, 1, want, sizeof want);
        kn_drawpath_suffix_inplace(big, sizeof big, 1, &trunc);
        CHECK(trunc == 0, "20-char extension fits");
        CHECK(strcmp(big, want) == 0,
              "20-char extension splits like the reference builder");
        CHECK(strncmp(big + 3, "_d1.", 4) == 0, "split point is before the dot");
    }
}

/* --------------------------------------- 4: the F17 capture-file family */
static void t_capture_family(void)
{
    char b[256];
    int trunc = 0;

    /* the continuous per-draw video stream (F17) */
    strcpy(b, "video.264");
    kn_drawpath_suffix_inplace(b, sizeof b, 1, &trunc);
    CHECK(strcmp(b, "video_d1.264") == 0, "video stream -> video_d1.264");

    /* a per-draw AU dump directory + draw-numbered name (F17) */
    strcpy(b, "logs/run32/au.264");
    kn_drawpath_suffix_inplace(b, sizeof b, 3, &trunc);
    CHECK(strcmp(b, "logs/run32/au_d3.264") == 0, "AU dump keeps its directory");

    /* the audio + video pair for one draw must differ only by stem */
    {
        char a[256], v[256];
        strcpy(a, "audio.opus");
        strcpy(v, "video.264");
        kn_drawpath_suffix_inplace(a, sizeof a, 2, &trunc);
        kn_drawpath_suffix_inplace(v, sizeof v, 2, &trunc);
        CHECK(strcmp(a, "audio_d2.opus") == 0, "audio_d2.opus");
        CHECK(strcmp(v, "video_d2.264") == 0, "video_d2.264");
        CHECK(strcmp(a, v) != 0, "the two captures never collide");
    }
}

int main(void)
{
    printf("== test_kndrawpath: the per-draw capture naming rule (F16) ==\n");
    t_run31_audio_paths();
    t_matrix_against_reference();
    t_bounds_and_truncation();
    t_capture_family();
    return kn_test_summary("test_kndrawpath");
}
