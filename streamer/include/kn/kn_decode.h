/* kn_decode.h — H.264 decode subsystem (FFmpeg libavcodec, D3/D4).
 *
 * Pipeline (BOOTSTRAP §6.1 measurements, D4/D6):
 *   Annex-B .264 -> av_parser frame split -> libavcodec h264 (threads = 1,
 *   D4: 211 fps 1t vs 191 fps 2t on the 2-core Merom) -> yuv420p AVFrame ->
 *   optional swscale to NV12 ("NV12 ~= free" per §6.1; the DEAD path is
 *   UpdateSurface, §9 R2 — the NV12 buffer produced here is meant to be
 *   copied into a lockable D3DPOOL_DEFAULT NV12 surface, never uploaded via
 *   UpdateSurface) -> per-frame callback.
 *
 * NO SCALING here, ever (D6: CPU scale = +11.8 ms/frame). Scaling is the
 * GPU's job (StretchRect) in stage 1.
 *
 * Build modes:
 *   KN_HAVE_FFMPEG defined (CMake KN_FFMPEG_PREFIX points at a dev tree):
 *       real implementation, links -lavcodec -lswscale -lavutil.
 *   not defined (default, e.g. host unit-test builds):
 *       compiling stub — kn_decode_available() == 0, decode returns -1,
 *       the decode.synthetic self-test honestly reports "skipped".
 *
 * Verified against FFmpeg headers at commit bf0e831606938f530326c9679591109407fa8d0d
 * (libavcodec 63 / libavutil 61 / libswscale 10 — the user's own
 * --enable-shared w64devkit build for Win7). Only the send/receive API
 * (stable since 2016) is used.
 */
#ifndef KN_DECODE_H
#define KN_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kn_decode_stats {
    int       frames_decoded;
    int       frames_failed;      /* receive/convert errors */
    int       width, height;      /* last decoded frame */
    long long wall_ms;            /* whole run, monotonic */
    long long cpu_ms;             /* process CPU time delta */
    double    fps;                /* frames / wall_s */
    double    cpu_pct_of_core;    /* cpu/wall*100; 200% == both cores (§6.1
                                     convention: divide by 2 for "% of both") */
    long long decode_ms_max;      /* worst per-frame decode+convert */
} kn_decode_stats;

/* Per-frame callback. data points at ONE frame:
 *   is_nv12=1: data = Y plane (stride_y), followed by interleaved UV at
 *              data + stride_y*height (stride_uv); buffer owned by kn_decode,
 *              valid only during the call.
 *   is_nv12=0: raw AVFrame planes are NOT passed; data = Y plane only with
 *              stride_y (i420 passthrough mode for capture/dump paths).
 * Return 0 to continue, non-zero to abort the run (kn_decode returns -4,
 * stats stay filled — the abort is a normal control path). */
typedef int (*kn_decode_frame_fn)(const uint8_t *data, int width, int height,
                                  int stride_y, int stride_uv, int is_nv12,
                                  long long frame_index, void *userdata);

/* 1 when linked against real FFmpeg, 0 for the stub build. */
int kn_decode_available(void);

/* Human backend string for logs/evidence, e.g. "libavcodec 63.x (threads=1)"
 * or "stub (built without FFmpeg)". Static storage. */
const char *kn_decode_backend(void);

/* Decode a whole Annex-B H.264 file. Returns:
 *   0  success (EOF reached; stats filled; callback saw every frame)
 *  -1  bad args / stub build (kn_decode_available()==0)
 *  -2  cannot open the file (or bigger than KN_DECODE_MAX_FILE)
 *  -3  decoder init failed (find/open)
 *  -4  callback asked to abort (stats filled)
 *  -5  read error
 * Corrupt frames increment stats->frames_failed and do NOT stop the run —
 * a live stream will have garbage in it; the decoder must survive it. */
int kn_decode_h264_file(const char *path, int to_nv12,
                        kn_decode_frame_fn cb, void *userdata,
                        kn_decode_stats *stats);

/* Same pipeline from a memory buffer (Annex-B). The input is copied once
 * into a padded buffer (the bitstream readers may over-read within
 * AV_INPUT_BUFFER_PADDING_SIZE — documented FFmpeg requirement), so the
 * caller's memory (e.g. a const embedded clip) is never touched.
 * Same return codes as the file variant except -2 (nothing to open) and -5. */
int kn_decode_h264_mem(const uint8_t *data, size_t len, int to_nv12,
                       kn_decode_frame_fn cb, void *userdata,
                       kn_decode_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* KN_DECODE_H */
