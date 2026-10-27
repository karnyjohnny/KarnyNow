/* knring.h — bounded raw-capture ring buffers (BOOTSTRAP §7.10).
 *
 * Keeps the last N fixed-size records of raw bytes (pre-decryption datagrams
 * for Mjolnir/bundle sockets, WebSocket frames for signalling). Dumped into
 * the diagnostic bundle ONLY on failure. Hard-bounded by design: total memory
 * = nslots * slot_size, allocated once at init, never grown.
 *
 * Slot layout (fixed 32-byte header + payload), all fields little-endian,
 * accessed via memcpy (no alignment assumptions):
 *   u32 magic   'K''N''R''1'  (0x31524E4B) — valid-record marker
 *   u32 flags   bit0 = payload truncated to fit the slot
 *   u64 mono_us monotonic microseconds at append time (caller-provided)
 *   u32 meta    caller metadata (e.g. socket id, direction, packet size)
 *   u32 len     payload bytes stored (<= payload capacity)
 *   u64 reserved
 *
 * Overwrite policy: when full, the OLDEST slot is overwritten (append never
 * fails, never allocates, never blocks — one memcpy + counters, safe to call
 * from a receive loop). kn_ring_dump() writes all valid records oldest-first
 * as text lines: "<mono_us> <meta_hex> <flags> <len> <hex...>".
 *
 * Sizing note: for Mjolnir with packetSize=1280, a 64 KiB ring with the
 * default 2080-byte slot holds ~31 records; init with max_record=1312 to get
 * ~48. Choose max_record per socket at init.
 */
#ifndef KN_RING_H
#define KN_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KN_RING_MAGIC   0x31524E4Bu /* 'KNR1' little-endian */
#define KN_RING_HEADER  32u
#define KN_RING_TRUNCATED 0x1u

typedef struct kn_ring {
    unsigned char *buf;
    size_t   slot_size;       /* header + payload capacity */
    size_t   nslots;
    unsigned long long wseq;  /* total appends ever (monotonic) */
    unsigned long long dropped; /* oldest records overwritten while full */
    unsigned long long truncated; /* appends whose payload did not fit */
    unsigned long long bytes_in;  /* payload bytes accepted (post-truncation) */
} kn_ring;

/* Allocates total_bytes (rounded down to whole slots; minimum 1 slot).
 * max_record = largest payload to store verbatim (larger -> truncated flag);
 * max_record 0 = default 2048. Returns 0 on success, -1 on alloc failure or
 * absurd arguments. */
int  kn_ring_init(kn_ring *r, size_t total_bytes, size_t max_record);
void kn_ring_free(kn_ring *r);

/* Append one record. mono_us: caller's monotonic microseconds. meta: opaque
 * u32 (socket/direction/size packed by the caller). Never fails: full rings
 * overwrite the oldest slot; oversized payloads are truncated + flagged. */
void kn_ring_append(kn_ring *r, uint64_t mono_us, uint32_t meta,
                    const void *data, size_t len);

/* Number of valid records currently held (<= nslots). */
size_t kn_ring_count(const kn_ring *r);

/* Visit records oldest-first. Visitor returns 0 to continue, non-zero to stop.
 * payload/len are the stored (possibly truncated) bytes; flags per KN_RING_*.
 * Returns records visited (including the one on which the visitor asked to
 * stop). NULL-safe (r or visit NULL -> 0). */
typedef int (*kn_ring_visit_fn)(uint64_t mono_us, uint32_t meta, uint32_t flags,
                                const unsigned char *payload, uint32_t len,
                                void *userdata);
size_t kn_ring_visit(const kn_ring *r, kn_ring_visit_fn visit, void *userdata);

/* Dump all valid records oldest-first to `path` as text lines (rewrites the
 * whole file; no fseek over own output, per §7.12). Returns record count,
 * or -1 on file error. */
long long kn_ring_dump(const kn_ring *r, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* KN_RING_H */
