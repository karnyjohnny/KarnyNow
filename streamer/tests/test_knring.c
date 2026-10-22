/* test_knring.c — unit tests for the bounded raw-capture ring buffer. */
#include "kn/knring.h"
#include "test_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_PATH "testring_dump.txt"

typedef struct {
    uint64_t mono[64];
    uint32_t meta[64];
    uint32_t flags[64];
    uint32_t len[64];
    unsigned char first_byte[64];
    int n;
    int stop_after;      /* <0 = never */
} collector;

static int collect(uint64_t mono_us, uint32_t meta, uint32_t flags,
                   const unsigned char *payload, uint32_t len, void *ud)
{
    collector *c = (collector *)ud;
    if (c->n >= 64) return 1;
    c->mono[c->n] = mono_us;
    c->meta[c->n] = meta;
    c->flags[c->n] = flags;
    c->len[c->n] = len;
    c->first_byte[c->n] = len ? payload[0] : 0;
    c->n++;
    return (c->stop_after >= 0 && c->n > c->stop_after) ? 1 : 0;
}

static void fill_pattern(unsigned char *dst, size_t n, unsigned seed)
{
    size_t i;
    for (i = 0; i < n; ++i) dst[i] = (unsigned char)((seed * 31u + (unsigned)i * 7u) & 0xFF);
}

int main(void)
{
    kn_ring r;
    collector col;
    unsigned char payload[256];
    long long dumped;
    char *txt;
    int i;

    /* -- init edge cases ------------------------------------------------ */
    memset(&r, 0, sizeof r);
    CHECK(kn_ring_init(NULL, 4096, 100) == -1, "NULL ring rejected");
    CHECK(kn_ring_init(&r, 64, 10000) == -1, "no whole slot fits -> -1");
    CHECK(kn_ring_init(&r, 4096, 100) == 0, "normal init");
    /* slot = align8(32+100) = 136; nslots = 4096/136 = 30 */
    CHECK(r.nslots == 30, "slot count as designed");
    CHECK(r.slot_size == 136, "slot size as designed");
    CHECK(kn_ring_count(&r) == 0, "empty ring count 0");

    /* empty dump -> 0 records, file created */
    dumped = kn_ring_dump(&r, DUMP_PATH);
    CHECK(dumped == 0, "empty dump returns 0");

    /* -- basic append/visit --------------------------------------------- */
    for (i = 0; i < 5; ++i) {
        fill_pattern(payload, 50, (unsigned)i);
        kn_ring_append(&r, 1000ULL + (uint64_t)i, (uint32_t)(0xA0 + i), payload, 50);
    }
    CHECK(kn_ring_count(&r) == 5, "count after 5 appends");
    memset(&col, 0, sizeof col);
    col.stop_after = -1;
    CHECK(kn_ring_visit(&r, collect, &col) == 5, "visit sees 5 records");
    CHECK(col.meta[0] == 0xA0 && col.meta[4] == 0xA4, "visit order oldest-first");
    CHECK(col.mono[0] == 1000 && col.mono[4] == 1004, "mono preserved and ordered");
    CHECK(col.len[0] == 50 && col.flags[0] == 0, "len and flags preserved");
    {
        unsigned char expect[256];
        fill_pattern(expect, 50, 3);
        fill_pattern(payload, 50, 3);
        CHECK(memcmp(payload, expect, 50) == 0, "pattern generator deterministic");
        CHECK(col.first_byte[3] == expect[0], "payload first byte matches record 3");
    }

    /* -- wrap-around: oldest overwritten, order preserved ---------------- */
    for (i = 5; i < 40; ++i) {
        fill_pattern(payload, 60, (unsigned)i);
        kn_ring_append(&r, 1000ULL + (uint64_t)i, (uint32_t)(0xA0 + i), payload, 60);
    }
    CHECK(kn_ring_count(&r) == 30, "count capped at nslots");
    CHECK(r.dropped == 10, "dropped counts overwritten records");
    memset(&col, 0, sizeof col);
    col.stop_after = -1;
    CHECK(kn_ring_visit(&r, collect, &col) == 30, "visit sees 30 after wrap");
    CHECK(col.meta[0] == 0xA0 + 10, "oldest survivor is record 10");
    CHECK(col.meta[29] == 0xA0 + 39, "newest is record 39");
    CHECK(col.mono[0] == 1010 && col.mono[29] == 1039, "mono ordered across wrap");

    /* -- truncation of oversized payloads -------------------------------- */
    {
        unsigned char big[256];
        memset(big, 0xEE, sizeof big);
        size_t before = r.truncated;
        kn_ring_append(&r, 5000, 0xBB, big, 200);   /* cap is 104 (136-32) */
        CHECK(r.truncated == before + 1, "truncation counted");
        memset(&col, 0, sizeof col);
        col.stop_after = -1;
        kn_ring_visit(&r, collect, &col);
        CHECK(col.n == 30, "still 30 records");
        CHECK(col.meta[29] == 0xBB, "oversized record is newest");
        CHECK(col.len[29] == 104, "payload truncated to slot capacity");
        CHECK(col.flags[29] == KN_RING_TRUNCATED, "truncated flag set");
    }

    /* -- dump content ------------------------------------------------------ */
    dumped = kn_ring_dump(&r, DUMP_PATH);
    CHECK(dumped == 30, "dump returns record count");
    txt = kn_test_read_file(DUMP_PATH, NULL);
    CHECK(txt != NULL, "dump file readable");
    if (txt) {
        int lines = 0;
        const char *p = txt;
        while (*p) { if (*p == '\n') lines++; p++; }
        CHECK(lines == 30, "dump has one line per record");
        CHECK(strstr(txt, " 000000bb 1 68 ") != NULL ||
              strstr(txt, " 000000bb 1 ") != NULL, "truncated record in dump");
        CHECK(strstr(txt, "ee ee") != NULL, "payload hex present");
    }
    free(txt);

    /* -- corrupt slot: visit skips it, never crashes ------------------------ */
    {
        size_t idx = (size_t)((r.wseq >= r.nslots ? r.wseq % r.nslots : 0) + 5) % r.nslots;
        unsigned char *slot = r.buf + idx * r.slot_size;
        slot[0] = 0xDE; slot[1] = 0xAD; slot[2] = 0xBE; slot[3] = 0xEF; /* kill magic */
        memset(&col, 0, sizeof col);
        col.stop_after = -1;
        {
            size_t seen = kn_ring_visit(&r, collect, &col);
            CHECK(seen == 29, "corrupt slot skipped");
        }
    }

    /* -- early stop ------------------------------------------------------------ */
    memset(&col, 0, sizeof col);
    col.stop_after = 2;   /* stop once n > 2 -> 3 records */
    CHECK(kn_ring_visit(&r, collect, &col) == 3, "visitor early stop honoured");

    kn_ring_free(&r);
    CHECK(r.buf == NULL, "free clears buffer");
    kn_ring_append(&r, 1, 2, "x", 1);   /* must be a silent no-op */
    CHECK(kn_ring_count(&r) == 0, "append after free is a no-op");
    kn_ring_free(&r);                   /* double free must be safe */
    kn_ring_free(NULL);

    kn_test_unlink(DUMP_PATH);
    for (i = 0; i < 1; ++i) { }         /* keep -Wall quiet about i */
    return kn_test_summary("test_knring");
}
