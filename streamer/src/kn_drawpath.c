/* SPDX-License-Identifier: CC-BY-4.0
 *
 * kn_drawpath.c — see kn/kn_drawpath.h.
 *
 * Round L (F16): the F13 per-draw naming rule, extracted from knvstd_main.c
 * and made alias-free. The old inline version took `dot = strrchr(dst, '.')`
 * (a pointer INTO the destination) and then memcpy'd the extension forward —
 * an overlapping copy (undefined behaviour, ASan: memcpy-param-overlap) that
 * produced run 31's "audio_d1_d1us" (doubled suffix, extension destroyed).
 * This version copies the extension and the
 * suffix into local buffers FIRST and uses memmove for every write into the
 * destination, so the in-place idiom (dst == src) is defined behaviour.
 */
#include <stdio.h>
#include <string.h>

#include "kn/kn_drawpath.h"

int kn_drawpath_suffix_str(long draw_index, char *dst, size_t dstsz)
{
    char tmp[KN_DRAWSUF_MAX];
    int n;

    if (!dst || dstsz == 0u) return -1;
    if (draw_index <= 0) {
        dst[0] = '\0';
        return 0;
    }

    n = snprintf(tmp, sizeof tmp, "_d%ld", draw_index);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;   /* cannot happen */
    if ((size_t)n >= dstsz) return -1;
    memcpy(dst, tmp, (size_t)n + 1u);
    return n;
}

char *kn_drawpath_suffix(const char *src, long draw_index, char *dst,
                         size_t dstsz, int *truncated)
{
    char suf[KN_DRAWSUF_MAX];
    char ext[32];                 /* ".opus"/".264"/anything real; 32+ chars
                                   * is treated as "no extension" (that is
                                   * not a capture file) */
    size_t srcn, stem, extn = 0, sufn, keep;
    int have_ext = 0, inplace;

    if (truncated) *truncated = 0;
    if (!src || !dst || dstsz == 0u) return NULL;

    srcn = strlen(src);
    keep = (srcn >= dstsz) ? dstsz - 1u : srcn;
    inplace = (src == dst);

    /* ---- the no-suffix paths: copy the original, verbatim ---- */
    if (draw_index <= 0 ||
        kn_drawpath_suffix_str(draw_index, suf, sizeof suf) < 0) {
        if (!inplace) memcpy(dst, src, keep);
        else if (keep < srcn) memmove(dst, src, keep);
        dst[keep] = '\0';
        if (truncated && keep != srcn) *truncated = 1;
        return dst;
    }
    sufn = strlen(suf);

    /* ---- the extension: the LAST '.' in the final path component ----
     * A dot in a directory name ("dir.ext/capture") is NOT an extension.
     * Both separators are honoured (Win7 + POSIX). */
    {
        const char *dot = strrchr(src, '.');
        const char *slash = strrchr(src, '/');
        const char *bslash = strrchr(src, '\\');
        const char *sep = (slash && bslash)
                              ? (slash > bslash ? slash : bslash)
                              : (slash ? slash : bslash);
        if (dot && dot > src && (!sep || dot > sep)) {
            extn = strlen(dot);
            if (extn < sizeof ext) {
                memcpy(ext, dot, extn + 1u);      /* local copy: no aliasing */
                have_ext = 1;
            }
        }
    }
    stem = have_ext ? (srcn - extn) : srcn;

    /* ---- no room: honestly keep the original path and say so ---- */
    if (stem + sufn + extn + 1u > dstsz) {
        if (!inplace) memcpy(dst, src, keep);
        else if (keep < srcn) memmove(dst, src, keep);
        dst[keep] = '\0';
        if (truncated) *truncated = 1;
        return dst;
    }

    /* ---- build: stem | suffix | extension ----
     * Highest addresses first, and always from a LOCAL buffer, so nothing
     * read below can have been overwritten by a write above. When the call
     * is in-place the stem bytes are already where they belong. */
    if (have_ext)
        memmove(dst + stem + sufn, ext, extn + 1u);       /* NUL included */
    else
        dst[stem + sufn] = '\0';
    memmove(dst + stem, suf, sufn);
    if (!inplace) memmove(dst, src, stem);
    return dst;
}

int kn_drawpath_suffix_inplace(char *buf, size_t bufsz, long draw_index,
                               int *truncated)
{
    int trunc = 0;
    if (!buf || bufsz == 0u) return -1;
    if (!kn_drawpath_suffix(buf, draw_index, buf, bufsz, &trunc)) return -1;
    if (truncated) *truncated = trunc;
    return trunc ? -1 : 0;
}
