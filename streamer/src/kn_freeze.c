/* kn_freeze.c — round M/N (F22): the freeze-witness ring (contract:
 * kn_freeze.h). Pure integer math on a fixed ring; see the header for
 * WHY. Every decision is pinned twice in test_knfreeze.c: once against
 * a second, independent recomputation from a plain array, once against
 * the exact boundary sums the header's permille-free inequalities
 * imply (sum5*20 < sum60 for decay, sum5*15 >= sum60 for rearm). */
#include "kn/kn_freeze.h"

#include <stdio.h>

void kn_fw_init(kn_fw *fw)
{
    unsigned i;
    if (!fw) return;
    for (i = 0; i < KN_FW_SECS; ++i) {
        fw->kbps[i] = 0u;
        fw->idr[i] = 0u;
        fw->gaps[i] = 0u;
    }
    fw->head = 0u;
    fw->fill = 0u;
}

void kn_fw_add(kn_fw *fw, unsigned kbps, unsigned idr, unsigned gaps)
{
    if (!fw) return;
    fw->kbps[fw->head] = kbps;
    fw->idr[fw->head] = idr;
    fw->gaps[fw->head] = gaps;
    fw->head = (fw->head + 1u) % KN_FW_SECS;
    if (fw->fill < KN_FW_SECS) fw->fill++;
}

/* oldest-first index -> ring slot */
static unsigned fw_slot(const kn_fw *fw, unsigned oldest_first)
{
    return (fw->head + KN_FW_SECS * 2u - fw->fill + oldest_first) %
           KN_FW_SECS;
}

/* sums over [from,to) in oldest-first coordinates; caller bounds them */
static void fw_sums(const kn_fw *fw, unsigned from, unsigned to,
                    unsigned long long *s5_kbps, unsigned long long *sgaps)
{
    unsigned i;
    *s5_kbps = 0ull;
    *sgaps = 0ull;
    for (i = from; i < to; ++i) {
        unsigned s = fw_slot(fw, i);
        *s5_kbps += fw->kbps[s];
        *sgaps += fw->gaps[s];
    }
}

int kn_fw_decay(const kn_fw *fw, unsigned *avg5, unsigned *avg60)
{
    unsigned long long sw = 0ull, sb = 0ull, sg = 0ull, g2 = 0ull;
    if (!fw || fw->fill < KN_FW_MIN_FILL) return 0;
    fw_sums(fw, fw->fill - KN_FW_WINDOW, fw->fill, &sw, &sg);
    fw_sums(fw, fw->fill - KN_FW_MIN_FILL + 2u,
            fw->fill - KN_FW_WINDOW, &sb, &g2);
    if (sg != 0ull || g2 != 0ull) return 0;   /* lossy = other family */
    if (!(sw * 20ull < sb)) return 0;         /* avg5*100 < avg60*60  */
    if (avg5) *avg5 = (unsigned)(sw / KN_FW_WINDOW);
    if (avg60) *avg60 = (unsigned)(sb / KN_FW_BASELINE);
    return 1;
}

int kn_fw_recovered(const kn_fw *fw)
{
    unsigned long long sw = 0ull, sb = 0ull, sg = 0ull, g2 = 0ull;
    if (!fw || fw->fill < KN_FW_MIN_FILL) return 0;
    fw_sums(fw, fw->fill - KN_FW_WINDOW, fw->fill, &sw, &sg);
    fw_sums(fw, fw->fill - KN_FW_MIN_FILL + 2u,
            fw->fill - KN_FW_WINDOW, &sb, &g2);
    (void)sg;
    (void)g2;
    return sw * 15ull >= sb;                  /* avg5*100 >= avg60*80 */
}

int kn_fw_ramp(const kn_fw *fw, char *buf, size_t bufsz)
{
    unsigned n, i, start;
    int off = 0;
    const char *names[3] = { "kbps", "idr", "gaps" };
    if (!buf || bufsz == 0u) return -1;
    buf[0] = '\0';
    if (!fw) return -1;
    n = fw->fill < KN_FW_RAMP_SECS ? fw->fill : KN_FW_RAMP_SECS;
    start = fw->fill - n;
    for (i = 0; i < 3u; ++i) {
        unsigned j;
        int w;
        w = snprintf(buf + off, bufsz - (size_t)off, "%s%s\":[",
                     i ? ",\"": "\"", names[i]);
        if (w < 0 || (size_t)w >= bufsz - (size_t)off) { buf[0] = '\0';
                                                        return -1; }
        off += w;
        for (j = 0; j < n; ++j) {
            unsigned s = fw_slot(fw, start + j);
            unsigned val = i == 0u ? fw->kbps[s]
                         : i == 1u ? fw->idr[s] : fw->gaps[s];
            w = snprintf(buf + off, bufsz - (size_t)off, "%s%u",
                         j ? "," : "", val);
            if (w < 0 || (size_t)w >= bufsz - (size_t)off) {
                buf[0] = '\0';
                return -1;
            }
            off += w;
        }
        w = snprintf(buf + off, bufsz - (size_t)off, "]");
        if (w < 0 || (size_t)w >= bufsz - (size_t)off) { buf[0] = '\0';
                                                        return -1; }
        off += w;
    }
    return off;
}
