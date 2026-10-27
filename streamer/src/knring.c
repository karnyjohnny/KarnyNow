/* knring.c — bounded raw-capture ring buffers (BOOTSTRAP §7.10).
 * Fixed-size slots, oldest-overwritten, dump-on-failure only.
 * All header fields are stored little-endian byte-by-byte (no aliasing,
 * no platform struct layout assumptions). */
#include "kn/knring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ byte accessors */

static void wr_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u64(unsigned char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static uint64_t rd_u64(const unsigned char *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

/* Slot header (32 bytes):
 *   0  u32 magic | 4 u32 flags | 8 u64 mono_us | 16 u32 meta | 20 u32 len | 24 u64 rsv */
#define OFF_MAGIC 0u
#define OFF_FLAGS 4u
#define OFF_MONO  8u
#define OFF_META  16u
#define OFF_LEN   20u

static size_t align8(size_t v) { return (v + 7u) & ~(size_t)7u; }

/* ------------------------------------------------------------------ lifecycle */

int kn_ring_init(kn_ring *r, size_t total_bytes, size_t max_record)
{
    size_t slot, nslots;
    if (!r || total_bytes < KN_RING_HEADER + 8) return -1;
    if (max_record == 0) max_record = 2048;
    slot = align8(KN_RING_HEADER + max_record);
    nslots = total_bytes / slot;
    if (nslots < 1) return -1;
    r->buf = (unsigned char *)calloc(nslots, slot);
    if (!r->buf) return -1;
    r->slot_size = slot;
    r->nslots = nslots;
    r->wseq = 0;
    r->dropped = 0;
    r->truncated = 0;
    r->bytes_in = 0;
    return 0;
}

void kn_ring_free(kn_ring *r)
{
    if (!r) return;
    free(r->buf);
    memset(r, 0, sizeof *r);
}

/* ------------------------------------------------------------------ append */

void kn_ring_append(kn_ring *r, uint64_t mono_us, uint32_t meta,
                    const void *data, size_t len)
{
    unsigned char *slot;
    uint32_t flags = 0, stored;
    size_t idx, payload_cap;
    if (!r || !r->buf) return;
    payload_cap = r->slot_size - KN_RING_HEADER;
    stored = (uint32_t)(len > payload_cap ? payload_cap : len);
    if (len > payload_cap) { flags |= KN_RING_TRUNCATED; r->truncated++; }

    idx = (size_t)(r->wseq % (unsigned long long)r->nslots);
    slot = r->buf + idx * r->slot_size;

    wr_u32(slot + OFF_MAGIC, KN_RING_MAGIC);
    wr_u32(slot + OFF_FLAGS, flags);
    wr_u64(slot + OFF_MONO, mono_us);
    wr_u32(slot + OFF_META, meta);
    wr_u32(slot + OFF_LEN, stored);
    wr_u64(slot + 24u, 0);
    if (stored && data) memcpy(slot + KN_RING_HEADER, data, stored);

    if (r->wseq >= (unsigned long long)r->nslots) r->dropped++;
    r->wseq++;
    r->bytes_in += stored;
}

size_t kn_ring_count(const kn_ring *r)
{
    if (!r || !r->buf) return 0;
    return r->wseq < (unsigned long long)r->nslots ? (size_t)r->wseq : r->nslots;
}

/* Oldest-first slot index for record k (0 <= k < count). */
static size_t nth_oldest(const kn_ring *r, size_t k)
{
    size_t oldest = (r->wseq >= (unsigned long long)r->nslots)
                  ? (size_t)(r->wseq % (unsigned long long)r->nslots)
                  : 0;
    return (oldest + k) % r->nslots;
}

size_t kn_ring_visit(const kn_ring *r, kn_ring_visit_fn visit, void *userdata)
{
    size_t n, k, visited = 0;
    if (!r || !r->buf || !visit) return 0;
    n = kn_ring_count(r);
    for (k = 0; k < n; ++k) {
        const unsigned char *slot = r->buf + nth_oldest(r, k) * r->slot_size;
        uint32_t magic = rd_u32(slot + OFF_MAGIC);
        uint32_t len, meta, flags;
        if (magic != KN_RING_MAGIC) continue;   /* corrupt slot: skip, keep going */
        flags = rd_u32(slot + OFF_FLAGS);
        len = rd_u32(slot + OFF_LEN);
        meta = rd_u32(slot + OFF_META);
        if (len > (uint32_t)(r->slot_size - KN_RING_HEADER)) continue; /* corrupt */
        {
            int rc = visit(rd_u64(slot + OFF_MONO), meta, flags,
                           slot + KN_RING_HEADER, len, userdata);
            visited++;                 /* the stop-requesting record counts too */
            if (rc != 0) break;
        }
    }
    return visited;
}

/* ------------------------------------------------------------------- dump */

typedef struct {
    FILE *f;
    long long n;
    int err;
} dump_ctx;

static int dump_visit(uint64_t mono_us, uint32_t meta, uint32_t flags,
                      const unsigned char *payload, uint32_t len, void *ud)
{
    dump_ctx *d = (dump_ctx *)ud;
    uint32_t i;
    if (d->err) return 1;
    if (fprintf(d->f, "%llu %08x %u %u",
                (unsigned long long)mono_us, (unsigned)meta,
                (unsigned)flags, (unsigned)len) < 0) { d->err = 1; return 1; }
    for (i = 0; i < len; ++i) {
        if (fprintf(d->f, " %02x", payload[i]) < 0) { d->err = 1; return 1; }
    }
    if (fputc('\n', d->f) == EOF) { d->err = 1; return 1; }
    d->n++;
    return 0;
}

long long kn_ring_dump(const kn_ring *r, const char *path)
{
    dump_ctx d;
    if (!r || !path) return -1;
    d.f = fopen(path, "wb");   /* rewrite whole file; no fseek over own output */
    if (!d.f) return -1;
    d.n = 0;
    d.err = 0;
    kn_ring_visit(r, dump_visit, &d);
    if (fclose(d.f) != 0) return -1;
    return d.err ? -1 : d.n;
}
