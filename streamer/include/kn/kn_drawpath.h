/* SPDX-License-Identifier: CC-BY-4.0
 *
 * kn_drawpath.h — the per-draw capture-file naming rule (round L, F16).
 *
 * WHY IT EXISTS: round XLIX's F13 gave a lottery draw its own audio file
 * ("<stem>_d<draw><ext>") so a multi-draw run delivers EVERY draw's audio
 * instead of the last one overwriting the others. The derivation was
 * written inline against the destination buffer itself, so `strrchr()`
 * returned a pointer INTO that buffer and the following `memcpy()` was an
 * overlapping copy — ASan flagged it, and run 31 delivered four audio
 * files named "audio_d1_d1us" … "audio_d4_d4us": the suffix was doubled
 * AND the ".opus" tail was overwritten by the copy's NUL, so the files
 * had no extension at all.
 *
 * The rule now lives in ONE place, is pure (no global state), never
 * aliases (the extension is copied to a local buffer before anything is
 * written), and is shared by the audio path, the video-stream path and
 * the per-draw AU dumps (F17). The self-test battery exercises it.
 *
 * THE RULE (identical for every capture file):
 *   draw 0            -> the operator's path verbatim ("audio.opus")
 *   draw N>0          -> "<stem>_d<N><ext>"    ("audio_d1.opus")
 *   no extension      -> "<path>_d<N>"         ("capture_d2")
 *   dot in a directory component only ("dir.ext/cap")
 *                      -> treated as no extension ("dir.ext/cap_d1")
 *   no room in the destination buffer
 *                      -> the ORIGINAL path, honestly un-suffixed, with
 *                         *truncated set so the caller can say so
 */
#ifndef KN_KN_DRAWPATH_H
#define KN_KN_DRAWPATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest suffix this module can produce: "_d" + a 64-bit decimal +
 * NUL. 24 bytes covers it with room to spare. */
#define KN_DRAWSUF_MAX 24u

/* Write the per-draw form of `src` into dst[dstsz].
 *
 *   src        the operator-supplied path (NUL-terminated, non-NULL)
 *   draw_index 0 = no suffix (the path is copied verbatim)
 *   dst/dstsz  the destination; dstsz MUST be > 0. `dst` may be the same
 *              buffer `src` points into (the common case: the caller has
 *              already copied the operator's path there) — nothing is
 *              read from `dst` after the first byte is written, so the
 *              in-place form is safe by construction.
 *   truncated  optional; set to 1 when the suffixed form did not fit and
 *              the ORIGINAL path was kept instead, else 0.
 *
 * Returns dst on success (always non-NULL when dstsz > 0), or NULL when
 * src/dst are NULL or dstsz == 0. dst is always NUL-terminated.
 */
char *kn_drawpath_suffix(const char *src, long draw_index, char *dst,
                         size_t dstsz, int *truncated);

/* In-place convenience wrapper: rewrites buf[bufsz] (which already holds
 * the operator's path) into its per-draw form. Returns 0 on success, or
 * -1 when it did not fit (buf then still holds the original path).
 * `truncated` is optional. */
int kn_drawpath_suffix_inplace(char *buf, size_t bufsz, long draw_index,
                               int *truncated);

/* Render ONLY the suffix ("_d1", "" for draw 0) into dst. Returns the
 * number of characters written (excluding NUL), or -1 on bad input.
 * Useful for callers that build several names from one suffix. */
int kn_drawpath_suffix_str(long draw_index, char *dst, size_t dstsz);

#ifdef __cplusplus
}
#endif

#endif /* KN_KN_DRAWPATH_H */
