/* test_knfreeze.c — round M/N (F22): the 26th native suite. Pins
 * kn_freeze.h three ways:
 *   (1) a SECOND INDEPENDENT IMPLEMENTATION — the test keeps its own
 *       plain unsigned array of the same observations and recomputes
 *       the window/baseline means and the ramp string from scratch,
 *       comparing against the module on 400 randomized seconds;
 *   (2) the EXACT BOUNDARY sums the header's integer inequalities
 *       imply: decay fires at sum5*20 == sum60-1 and NOT at ==sum60;
 *       rearm fires at sum5*15 == sum60 and NOT at ==sum60-1; a single
 *       gap anywhere in the 65 s span vetoes decay; fill < 67 vetoes
 *       both decisions;
 *   (3) the ramp renderer: exact JSON text for a known ring (incl. a
 *       wrapped ring), oldest-first order, the under-10 s case, and
 *       honest -1 + empty string on a too-small buffer.
 * Run: build/test_knfreeze (ctest entry test_knfreeze). */
#include "kn/kn_freeze.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long checks = 0, fails = 0;
#define CK(cond, msg)                                                  \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            fails++;                                                   \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, (msg));      \
        }                                                              \
    } while (0)

/* the second implementation: a plain ringless history */
typedef struct {
    unsigned kbps[4096], idr[4096], gaps[4096];
    unsigned n;
} hist;

static void hist_add(hist *h, unsigned k, unsigned i, unsigned g)
{
    h->kbps[h->n] = k;
    h->idr[h->n] = i;
    h->gaps[h->n] = g;
    h->n++;
}

static int hist_decay(const hist *h, unsigned *a5, unsigned *a60)
{
    unsigned long long sw = 0, sb = 0;
    unsigned i;
    if (h->n < 67u) return 0;
    for (i = h->n - 5u; i < h->n; ++i) {
        sw += h->kbps[i];
        if (h->gaps[i]) return 0;
    }
    for (i = h->n - 65u; i < h->n - 5u; ++i) {
        sb += h->kbps[i];
        if (h->gaps[i]) return 0;
    }
    if (!(sw * 20ull < sb)) return 0;
    if (a5) *a5 = (unsigned)(sw / 5u);
    if (a60) *a60 = (unsigned)(sb / 60u);
    return 1;
}

static int hist_recovered(const hist *h)
{
    unsigned long long sw = 0, sb = 0;
    unsigned i;
    if (h->n < 67u) return 0;
    for (i = h->n - 5u; i < h->n; ++i) sw += h->kbps[i];
    for (i = h->n - 65u; i < h->n - 5u; ++i) sb += h->kbps[i];
    return sw * 15ull >= sb;
}

static unsigned seed = 12345u;
static unsigned rnd(unsigned mod)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % mod;
}

int main(void)
{
    kn_fw fw;
    hist h;
    unsigned t;

    /* ---- (1) second implementation over 400 randomized seconds ---- */
    kn_fw_init(&fw);
    memset(&h, 0, sizeof h);
    for (t = 0; t < 400u; ++t) {
        unsigned base = 2000u + rnd(1500u);
        unsigned k = t > 300u ? base / (1u + (t - 300u) / 10u) : base;
        unsigned idr = (t % 97u == 0u) ? 1u : 0u;
        unsigned g = (t == 250u) ? 1u : 0u;   /* one lossy second      */
        unsigned m5 = 0, m60 = 0, h5 = 0, h60 = 0;
        int d, hd, r, hr;
        kn_fw_add(&fw, k, idr, g);
        hist_add(&h, k, idr, g);
        d = kn_fw_decay(&fw, &m5, &m60);
        hd = hist_decay(&h, &h5, &h60);
        r = kn_fw_recovered(&fw);
        hr = hist_recovered(&h);
        CK(d == hd, "decay agrees with second implementation");
        CK(r == hr, "rearm agrees with second implementation");
        if (d && hd) {
            CK(m5 == h5 && m60 == h60, "decay means agree");
        }
    }
    CK(h.n == 400u, "history length");

    /* ---- (2) the exact boundary sums ---- */
    /* baseline 60 s of B kbps each: sum60 = 60B. Decay needs
     * sum5*20 < 60B i.e. sum5 < 3B; with a flat window of W: 5W*20<60B
     * i.e. W*100 < 60B i.e. W < 0.6B. B=1000: W=599 decays, 600 not. */
    kn_fw_init(&fw);
    for (t = 0; t < 62u; ++t) kn_fw_add(&fw, 1000u, 0u, 0u);
    for (t = 0; t < 5u; ++t) kn_fw_add(&fw, 600u, 0u, 0u);
    CK(kn_fw_decay(&fw, NULL, NULL) == 0, "exactly 60% is NOT decay");
    CK(kn_fw_recovered(&fw) == 0, "60% is not recovered either");
    kn_fw_init(&fw);
    for (t = 0; t < 62u; ++t) kn_fw_add(&fw, 1000u, 0u, 0u);
    for (t = 0; t < 4u; ++t) kn_fw_add(&fw, 600u, 0u, 0u);
    kn_fw_add(&fw, 599u, 0u, 0u);
    {
        unsigned a5 = 0, a60 = 0;
        CK(kn_fw_decay(&fw, &a5, &a60) == 1, "sum5*20==sum60-1 decays");
        CK(a5 == 599u && a60 == 1000u, "boundary means exact");
    }
    /* rearm boundary: sum5*15 >= sum60 i.e. W*75 >= 60B i.e. W >= 0.8B */
    kn_fw_init(&fw);
    for (t = 0; t < 62u; ++t) kn_fw_add(&fw, 1000u, 0u, 0u);
    for (t = 0; t < 5u; ++t) kn_fw_add(&fw, 799u, 0u, 0u);
    CK(kn_fw_recovered(&fw) == 0, "79.9% is not recovered");
    kn_fw_add(&fw, 804u, 0u, 0u);   /* window now 600..: recompute */
    /* window = 799*4+804 = 3999+... sums: 3196+804=4000; 4000*15=60000
     * == sum60=60000 -> recovered */
    CK(kn_fw_recovered(&fw) == 1, "sum5*15==sum60 rearms");
    /* a single gap in the span vetoes decay */
    kn_fw_init(&fw);
    for (t = 0; t < 62u; ++t) kn_fw_add(&fw, 1000u, 0u, 0u);
    for (t = 0; t < 4u; ++t) kn_fw_add(&fw, 100u, 0u, 0u);
    kn_fw_add(&fw, 100u, 0u, 1u);
    CK(kn_fw_decay(&fw, NULL, NULL) == 0, "gap in window vetoes");
    kn_fw_init(&fw);
    for (t = 0; t < 61u; ++t) kn_fw_add(&fw, 1000u, 0u, 0u);
    kn_fw_add(&fw, 1000u, 0u, 1u);
    for (t = 0; t < 5u; ++t) kn_fw_add(&fw, 100u, 0u, 0u);
    CK(kn_fw_decay(&fw, NULL, NULL) == 0, "gap in baseline vetoes");
    /* shallow ring vetoes both */
    kn_fw_init(&fw);
    for (t = 0; t < 66u; ++t) kn_fw_add(&fw, t < 60u ? 1000u : 10u, 0u, 0u);
    CK(kn_fw_decay(&fw, NULL, NULL) == 0, "fill 66 vetoes decay");
    CK(kn_fw_recovered(&fw) == 0, "fill 66 vetoes rearm");
    kn_fw_add(&fw, 10u, 0u, 0u);
    CK(kn_fw_decay(&fw, NULL, NULL) == 1, "fill 67 decides");

    /* ---- (3) the ramp renderer ---- */
    kn_fw_init(&fw);
    for (t = 0; t < 12u; ++t) kn_fw_add(&fw, 100u + t, t % 3u == 0u ? 1u : 0u,
                                        t == 11u ? 2u : 0u);
    {
        char b[512];
        int n = kn_fw_ramp(&fw, b, sizeof b);
        CK(n > 0, "ramp renders");
        CK(strcmp(b, "\"kbps\":[102,103,104,105,106,107,108,109,110,111],"
                     "\"idr\":[0,1,0,0,1,0,0,1,0,0],"
                     "\"gaps\":[0,0,0,0,0,0,0,0,0,2]") == 0,
           "ramp text exact (last 10 s, oldest first)");
        CK((size_t)n == strlen(b), "ramp length == strlen");
    }
    /* wrapped ring: 200 adds then the same check on the tail */
    kn_fw_init(&fw);
    for (t = 0; t < 200u; ++t) kn_fw_add(&fw, 7u, 0u, 0u);
    for (t = 0; t < 12u; ++t) kn_fw_add(&fw, 100u + t, 0u, 0u);
    {
        char b[512];
        CK(kn_fw_ramp(&fw, b, sizeof b) > 0, "wrapped ramp renders");
        CK(strcmp(b, "\"kbps\":[102,103,104,105,106,107,108,109,110,111],"
                     "\"idr\":[0,0,0,0,0,0,0,0,0,0],"
                     "\"gaps\":[0,0,0,0,0,0,0,0,0,0]") == 0,
           "wrapped ramp text exact");
    }
    /* under-10 s: renders what the ring holds */
    kn_fw_init(&fw);
    kn_fw_add(&fw, 5u, 1u, 0u);
    kn_fw_add(&fw, 6u, 0u, 0u);
    {
        char b[512];
        CK(kn_fw_ramp(&fw, b, sizeof b) > 0, "short ramp renders");
        CK(strcmp(b, "\"kbps\":[5,6],\"idr\":[1,0],\"gaps\":[0,0]") == 0,
           "short ramp text exact");
    }
    /* too-small buffer: -1 and empty string */
    {
        char b[8] = "garbage";
        CK(kn_fw_ramp(&fw, b, sizeof b) == -1, "small buffer -> -1");
        CK(b[0] == '\0', "small buffer -> empty string");
        CK(kn_fw_ramp(&fw, NULL, 0) == -1, "NULL buffer -> -1");
    }
    /* NULL fw everywhere is a no-op, not a crash */
    kn_fw_init(NULL);
    kn_fw_add(NULL, 1u, 0u, 0u);
    CK(kn_fw_decay(NULL, NULL, NULL) == 0, "NULL decay -> 0");
    CK(kn_fw_recovered(NULL) == 0, "NULL rearm -> 0");

    printf("%s: %ld checks, %ld failures\n",
           fails ? "TEST_KNFREEZE FAIL" : "test_knfreeze PASS",
           checks, fails);
    return fails ? 1 : 0;
}
