/* test_kninput.c — the SEVENTEENTH suite: kn_input, the NVST
 * control/input plane (round XXXVII; docs/INPUT.md, docs/RTSP.md §24).
 *
 * WHY THIS SUITE EXISTS: R7 run 17 disproved the round-XXXVI hypothesis
 * — the complete receiver-feedback set flew for the whole 242 s session
 * (rr 242 / qos 4701 / ka 81 / ack 331 / dropped 0) and video still died
 * at exactly 10.99 s (331 AUs @ 30.1 fps), the same shape as run 16's
 * 10.2 s with NO feedback. The piece both runs lacked is the eight-
 * message input activation chain the reference sends the instant sid 0
 * opens and INPUT_VERSION arrives (nvst.rs:6554-6591 ->
 * nvst_input.rs:1318-1329), whose window-state member the reference
 * documents as the difference between a session that looks active and
 * one that does not.
 *
 * WHAT IT PROVES, in three independent ways:
 *  1. BYTE-EXACTNESS against the reference client's OWN goldens
 *     (nvst_input.rs:1768-1800, transcribed verbatim into
 *     tools/input_reference.py, which self-proves before emitting
 *     kn_input_kat_vectors.h) — all 8 chain messages + 11 singles.
 *  2. PARSER FIDELITY against nvst_input.rs:1698-1712, including run
 *     17's LIVE rig bytes (0e 02 02 00 03 00 -> version 3) and both of
 *     the reference's fallback paths.
 *  3. BEHAVIOURAL FIDELITY: the three cursor-capture tests of
 *     nvst_cursor.rs:165-250 replayed as C command-sequence assertions
 *     (14 enable commands before a silent-host fallback, paced disable
 *     retries, handoff reported only on success, exhaustion at 8).
 * Plus: bounds robustness (every truncation of every golden, ASan-clean)
 * and the evidence serializer's JSONL safety.
 */
#if !defined(_WIN32) && (!defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L))
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "kn/kn_input.h"
#include "test_port.h"
#include "kn_input_kat_vectors.h"

#include <string.h>

/* ------------------------------------------------------- small helpers */

static int t_hexeq(const uint8_t *got, size_t glen,
                   const uint8_t *want, size_t wlen)
{
    return glen == wlen && (glen == 0 || memcmp(got, want, glen) == 0);
}

KN_TEST_UNUSED static void t_dump(const char *tag, const uint8_t *b, size_t n)
{
    size_t i;
    fprintf(stderr, "  %s (%u):", tag, (unsigned)n);
    for (i = 0; i < n; ++i) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

/* the cursor state machine's recording sink */
typedef struct {
    kn_input_cursor_cmd cmds[64];
    size_t n;
    int ok;                    /* what send() reports */
} t_rec;

static int t_rec_send(void *ctx, kn_input_cursor_cmd cmd)
{
    t_rec *r = (t_rec *)ctx;
    if (r->n < sizeof r->cmds / sizeof r->cmds[0]) r->cmds[r->n++] = cmd;
    return r->ok;
}

static void t_rec_init(t_rec *r, int ok)
{
    memset(r, 0, sizeof *r);
    r->ok = ok;
}

/* the reference expresses "no command may be emitted here" with a
 * panicking closure (nvst_cursor.rs:271/:277); this is the C shape. */
static int t_never;
static int t_never_send(void *ctx, kn_input_cursor_cmd cmd)
{
    (void)ctx; (void)cmd;
    t_never++;
    return 1;
}

/* the scan sink: records the codes it was handed */
typedef struct {
    uint16_t codes[32];
    size_t   plen[32];
    size_t   n;
} t_scanrec;

static void t_scan_cb(void *ctx, uint16_t code, const uint8_t *payload,
                      size_t plen)
{
    t_scanrec *s = (t_scanrec *)ctx;
    (void)payload;
    if (s->n < sizeof s->codes / sizeof s->codes[0]) {
        s->codes[s->n] = code;
        s->plen[s->n] = plen;
        s->n++;
    }
}

/* ------------------------------------------------- 1. byte-exact goldens */

static void t_activation_chain(void)
{
    uint8_t out[KN_INPUT_ACTIVATION_N][KN_INPUT_CMD_MAX];
    size_t lens[KN_INPUT_ACTIVATION_N];
    size_t i, total = 0;
    int n = kn_input_activation_chain(KN_INPUT_KAT_TS_US, out, lens);

    CHECK(n == (int)KN_INPUT_ACTIVATION_N, "activation chain builds 8 messages");
    for (i = 0; i < KN_INPUT_ACTIVATION_N; ++i) {
        char name[64];
        const kn_input_cmd_kat *k = &kn_input_kat_activation[i];
        snprintf(name, sizeof name, "chain[%u] %s byte-exact", (unsigned)i,
                 k->what);
        CHECK(lens[i] == k->len, name);
        CHECK(t_hexeq(out[i], lens[i], k->bytes, k->len), name);
        /* the TLV header must agree with the record length */
        CHECK(out[i][0] == (uint8_t)(k->code & 0xffu) &&
              out[i][1] == (uint8_t)(k->code >> 8), "chain code LE");
        CHECK((size_t)(out[i][2] | (out[i][3] << 8)) + 4u == lens[i],
              "chain payload length field");
        total += lens[i];
    }
    CHECK(total == 170u, "chain total bytes 16+52+5+5+16+16+16+44 = 170");
    /* every message fits the documented ceiling */
    for (i = 0; i < KN_INPUT_ACTIVATION_N; ++i)
        CHECK(lens[i] <= KN_INPUT_CMD_MAX, "chain message <= CMD_MAX");
    /* NULL contracts */
    CHECK(kn_input_activation_chain(1u, NULL, lens) == -1, "chain NULL out");
    CHECK(kn_input_activation_chain(1u, out, NULL) == -1, "chain NULL lens");
}

static void t_singles(void)
{
    size_t i;
    for (i = 0; i < kn_input_kat_singles_n; ++i) {
        const kn_input_cmd_kat *k = &kn_input_kat_singles[i];
        uint8_t buf[KN_INPUT_CMD_MAX];
        int n = -1;
        char name[80];
        snprintf(name, sizeof name, "single %s byte-exact", k->what);
        /* rebuild by name */
        if (strcmp(k->what, "keepalive0") == 0)
            n = kn_input_keepalive(0u, buf, sizeof buf);
        else if (strcmp(k->what, "capture_on") == 0)
            n = kn_input_cursor_capture(1, buf, sizeof buf);
        else if (strcmp(k->what, "capture_off") == 0)
            n = kn_input_cursor_capture(0, buf, sizeof buf);
        else if (strcmp(k->what, "tracking_on") == 0)
            n = kn_input_cursor_tracking(1, buf, sizeof buf);
        else if (strcmp(k->what, "tracking_off") == 0)
            n = kn_input_cursor_tracking(0, buf, sizeof buf);
        else if (strcmp(k->what, "window19") == 0)
            n = kn_input_state_change(KN_INPUT_CMD_WINDOW_STATE,
                                      KN_INPUT_WINDOW_STATE_ACTIVE, 0u,
                                      buf, sizeof buf);
        else if (strcmp(k->what, "system0") == 0)
            n = kn_input_state_change(KN_INPUT_CMD_SYSTEM_STATE, 0u, 0u,
                                      buf, sizeof buf);
        else if (strcmp(k->what, "enable_off") == 0)
            n = kn_input_enable_input(1u, 0, buf, sizeof buf);
        else if (strcmp(k->what, "enable_on") == 0)
            n = kn_input_enable_input(1u, 1, buf, sizeof buf);
        else if (strcmp(k->what, "haptics") == 0)
            n = kn_input_haptics(KN_INPUT_KAT_TS_US, 1, buf, sizeof buf);
        else if (strcmp(k->what, "descriptor") == 0)
            n = kn_input_device_descriptor(KN_INPUT_KAT_TS_US, 0u, buf,
                                           sizeof buf);
        CHECK(n > 0, name);
        if (n > 0)
            CHECK(t_hexeq(buf, (size_t)n, k->bytes, k->len), name);
    }
    CHECK(kn_input_kat_singles_n == 11u, "eleven single goldens");
}

static void t_builder_edges(void)
{
    uint8_t buf[KN_INPUT_CMD_MAX];
    uint8_t payload[12] = { 0 };
    /* exact-fit and one-byte-short buffers */
    CHECK(kn_input_command(0x0200u, payload, sizeof payload, buf, 16) == 16,
          "command exact fit");
    CHECK(kn_input_command(0x0200u, payload, sizeof payload, buf, 15) == -2,
          "command buffer too small");
    CHECK(kn_input_command(0x0200u, NULL, 0, buf, 4) == 4,
          "command empty payload");
    CHECK(kn_input_command(0x0200u, NULL, 4, buf, 16) == -1,
          "command NULL payload with length refused");
    CHECK(kn_input_command(0x0200u, payload, 4, NULL, 16) == -1,
          "command NULL out refused");
    /* every builder reports the honest shortfall, never a partial write */
    CHECK(kn_input_keepalive(0u, buf, 7) == -2, "keepalive needs 8");
    CHECK(kn_input_enable_input(1u, 1, buf, 15) == -2, "enable_input needs 16");
    CHECK(kn_input_device_descriptor(1u, 0u, buf, 51) == -2,
          "descriptor needs 52");
    CHECK(kn_input_haptics(1u, 1, buf, 43) == -2, "haptics needs 44");
    CHECK(kn_input_cursor_capture(1, buf, 4) == -2, "capture needs 5");
    CHECK(kn_input_state_change(KN_INPUT_CMD_WINDOW_STATE, 1u, 0u, buf, 15)
              == -2, "state_change needs 16");
    /* the documented sizes */
    CHECK(kn_input_keepalive(0u, buf, sizeof buf) == 8, "keepalive is 8 B");
    CHECK(kn_input_device_descriptor(0u, 0u, buf, sizeof buf) == 52,
          "descriptor is 52 B");
    CHECK(kn_input_haptics(0u, 1, buf, sizeof buf) == 44, "haptics is 44 B");
    /* the bitmap really lands in body[9..11] (payload offset 18) */
    CHECK(kn_input_device_descriptor(0u, 0x1234u, buf, sizeof buf) == 52 &&
          buf[4 + 18] == 0x34 && buf[4 + 19] == 0x12, "descriptor bitmap LE");
    /* the timestamp endianness split: BE in the descriptor, LE in haptics */
    CHECK(kn_input_device_descriptor(0x0102030405060708ull, 0u, buf,
                                     sizeof buf) == 52 &&
          buf[5] == 0x01 && buf[12] == 0x08, "descriptor timestamp is BE");
    CHECK(kn_input_haptics(0x0102030405060708ull, 1, buf, sizeof buf) == 44 &&
          buf[4 + 8 + 24] == 0x08 && buf[4 + 8 + 31] == 0x01,
          "haptics timestamp is LE");
    /* keepalive with a nonzero stream value stays well-formed */
    CHECK(kn_input_keepalive(0xdeadbeefu, buf, sizeof buf) == 8 &&
          buf[4] == 0xef && buf[7] == 0xde, "keepalive stream value LE");
}

/* --------------------------------------------------- 2. parser fidelity */

static void t_versions(void)
{
    size_t i;
    for (i = 0; i < kn_input_kat_versions_n; ++i) {
        const kn_input_ver_kat *k = &kn_input_kat_versions[i];
        int got = kn_input_protocol_version(k->bytes, k->len);
        char name[64];
        snprintf(name, sizeof name, "version vector[%u] -> %d", (unsigned)i,
                 k->want);
        CHECK(got == k->want, name);
    }
    CHECK(kn_input_kat_versions_n == 7u, "seven version vectors");
    /* run 17's LIVE bytes, spelled out: sid 0, ppi 53, len 6 */
    {
        static const uint8_t live17[6] = { 0x0e, 0x02, 0x02, 0x00, 0x03, 0x00 };
        CHECK(kn_input_protocol_version(live17, sizeof live17) == 3,
              "run 17 rig INPUT_VERSION negotiates 3");
        CHECK(kn_input_scan(live17, sizeof live17, NULL, NULL) == 1u,
              "run 17 bytes are exactly one TLV record");
        CHECK(!kn_input_has_cursor_notification(live17, sizeof live17),
              "run 17 INPUT_VERSION is not a cursor notification");
    }
    CHECK(kn_input_protocol_version(NULL, 6) == -1, "version NULL bytes");
    CHECK(kn_input_protocol_version((const uint8_t *)"", 0) == -1,
          "version empty");
}

static void t_scan(void)
{
    /* a concatenated record list, as the reference batches them
     * (nvst.rs:7903-7970 sends several TLVs in ONE SCTP message) */
    uint8_t batch[KN_INPUT_CMD_MAX * 3];
    size_t off = 0, walked;
    t_scanrec s;
    int n;

    memset(&s, 0, sizeof s);
    n = kn_input_keepalive(0u, batch + off, sizeof batch - off);
    CHECK(n == 8, "batch keepalive");
    off += (size_t)n;
    n = kn_input_state_change(KN_INPUT_CMD_WINDOW_STATE, 19u, 0u,
                              batch + off, sizeof batch - off);
    CHECK(n == 16, "batch window state");
    off += (size_t)n;
    n = kn_input_cursor_capture(1, batch + off, sizeof batch - off);
    CHECK(n == 5, "batch cursor capture");
    off += (size_t)n;

    walked = kn_input_scan(batch, off, t_scan_cb, &s);
    CHECK(walked == 3u, "scan walks three concatenated records");
    CHECK(s.n == 3u, "scan delivered three records");
    CHECK(s.codes[0] == KN_INPUT_CMD_KEEPALIVE && s.plen[0] == 4u,
          "scan record 0 = keepalive/4");
    CHECK(s.codes[1] == KN_INPUT_CMD_WINDOW_STATE && s.plen[1] == 12u,
          "scan record 1 = window-state/12");
    CHECK(s.codes[2] == KN_INPUT_CMD_CURSOR_CAPTURE && s.plen[2] == 1u,
          "scan record 2 = cursor-capture/1");
    /* 0x0308 is OUR capture command; the notification codes the rig
     * sends are 0x010f/0x0110 — the detector must not confuse them. */
    CHECK(kn_input_has_cursor_notification(batch, off) == 0,
          "our own capture(true) is not a server cursor notification");
    /* now a batch that really carries one (nvst_cursor.rs:186-189) */
    {
        static const uint8_t bitmap[12] = {
            0x10, 0x01, 0x08, 0x00, 0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 0
        };
        uint8_t mixed[64];
        size_t moff = 0;
        int nn = kn_input_keepalive(0u, mixed, sizeof mixed);
        CHECK(nn == 8, "mixed batch keepalive");
        moff += (size_t)nn;
        memcpy(mixed + moff, bitmap, sizeof bitmap);
        moff += sizeof bitmap;
        nn = kn_input_cursor_capture(1, mixed + moff, sizeof mixed - moff);
        CHECK(nn == 5, "mixed batch capture");
        moff += (size_t)nn;
        CHECK(kn_input_has_cursor_notification(mixed, moff) == 1,
              "a bitmap-cursor record inside a batch is detected");
        CHECK(kn_input_scan(mixed, moff, NULL, NULL) == 3u,
              "the mixed batch walks as three records");
    }

    /* a zero-length record still advances (no infinite loop) */
    {
        static const uint8_t zero[8] = { 0x00, 0x02, 0x00, 0x00,
                                         0x00, 0x02, 0x00, 0x00 };
        memset(&s, 0, sizeof s);
        CHECK(kn_input_scan(zero, sizeof zero, t_scan_cb, &s) == 2u,
              "two zero-length records walk");
        CHECK(s.n == 2u && s.plen[0] == 0u && s.plen[1] == 0u,
              "zero-length payloads reported");
    }
    /* a declared length that overruns ends the walk */
    {
        static const uint8_t bad[6] = { 0x0e, 0x02, 0xff, 0x00, 0x03, 0x00 };
        memset(&s, 0, sizeof s);
        CHECK(kn_input_scan(bad, sizeof bad, t_scan_cb, &s) == 0u,
              "overrunning record ends the walk");
    }
    CHECK(kn_input_scan(NULL, 8, t_scan_cb, &s) == 0u, "scan NULL bytes");
    CHECK(kn_input_scan(batch, off, NULL, NULL) == 3u,
          "scan without a callback still counts");
}

static void t_cursor_notify_vectors(void)
{
    size_t i;
    for (i = 0; i < kn_input_kat_cursor_n; ++i) {
        const kn_input_cursor_kat *k = &kn_input_kat_cursor[i];
        int got = kn_input_has_cursor_notification(k->bytes, k->len);
        char name[64];
        snprintf(name, sizeof name, "cursor-notify vector[%u] -> %d",
                 (unsigned)i, k->want);
        CHECK(got == k->want, name);
    }
    CHECK(kn_input_kat_cursor_n == 7u, "seven cursor-notification vectors");
    CHECK(kn_input_has_cursor_notification(NULL, 16) == 0,
          "cursor-notify NULL bytes");
    CHECK(kn_input_has_cursor_notification((const uint8_t *)"x", 0) == 0,
          "cursor-notify empty buffer");
}

/* ------------------------------------- 3. cursor machine (nvst_cursor.rs) */

static void t_cursor_notification_handoff(void)
{
    /* nvst_cursor.rs:165-183 notification_hands_off_once_and_stops_
     * enable_retries — exactly one CAPTURE_OFF, then silence. */
    kn_input_cursor c;
    t_rec r;
    uint64_t now = 1000u;
    kn_input_cursor_init(&c);
    t_rec_init(&r, 1);
    kn_input_cursor_activate(&c, now);
    CHECK(c.state == KN_INPUT_CURSOR_WAITING, "activate -> waiting");
    CHECK(c.attempts == 1u, "activate counts the chain's own enable");
    kn_input_cursor_notify(&c, now);
    CHECK(c.state == KN_INPUT_CURSOR_DISABLING, "notify -> disabling");
    CHECK(c.reason && strcmp(c.reason, "cursor-notification") == 0,
          "notify reason");
    CHECK(kn_input_cursor_update(&c, now, t_rec_send, &r) == 1,
          "handoff reported on the first successful disable");
    CHECK(c.state == KN_INPUT_CURSOR_LOCAL, "state -> local");
    kn_input_cursor_notify(&c, now + KN_INPUT_CURSOR_RETRY_MS);
    CHECK(kn_input_cursor_update(&c, now + KN_INPUT_CURSOR_SILENT_HOST_MS,
                                 t_rec_send, &r) == 0,
          "a handed-off client stops reacting");
    CHECK(r.n == 1u, "exactly one command emitted");
    CHECK(r.cmds[0] == KN_INPUT_CURSOR_CMD_CAPTURE_OFF,
          "the one command is capture(false)");
    CHECK(c.disables == 1u && c.handoffs == 1u, "disable/handoff counters");
}

static void t_cursor_silent_host(void)
{
    /* nvst_cursor.rs:204-227 silent_host_falls_back_after_bounded_enable_
     * retries — (MAX_ATTEMPTS-1) x [capture(true), tracking], then the
     * 3 s deadline hands off in the SAME update call. */
    kn_input_cursor c;
    t_rec r;
    uint64_t now = 5000u;
    int tick, handoff;
    size_t i;
    int all_pairs = 1;

    kn_input_cursor_init(&c);
    t_rec_init(&r, 1);
    kn_input_cursor_activate(&c, now);
    for (tick = 0; tick < 12; ++tick) {
        uint64_t t = now + (uint64_t)KN_INPUT_CURSOR_RETRY_MS * (uint64_t)tick;
        if (kn_input_cursor_update(&c, t, t_rec_send, &r) != 0) all_pairs = 0;
    }
    CHECK(all_pairs, "no handoff during the enable-retry window");
    CHECK(r.n == (size_t)(KN_INPUT_CURSOR_MAX_ATTEMPTS - 1u) * 2u,
          "seven enable retries = fourteen commands");
    for (i = 0; i + 1 < r.n && i + 1 < 14u; i += 2u)
        if (r.cmds[i] != KN_INPUT_CURSOR_CMD_CAPTURE_ON ||
            r.cmds[i + 1] != KN_INPUT_CURSOR_CMD_TRACKING) all_pairs = 0;
    CHECK(all_pairs, "every retry is [capture(true), tracking]");
    CHECK(c.attempts == KN_INPUT_CURSOR_MAX_ATTEMPTS, "attempts capped at 8");
    CHECK(c.enables == 7u && c.trackings == 7u, "enable/tracking counters");
    handoff = kn_input_cursor_update(&c,
                                     now + KN_INPUT_CURSOR_SILENT_HOST_MS,
                                     t_rec_send, &r);
    CHECK(handoff == 1, "the silent-host deadline hands off");
    CHECK(r.n == 15u && r.cmds[14] == KN_INPUT_CURSOR_CMD_CAPTURE_OFF,
          "the fifteenth command is capture(false)");
    CHECK(c.state == KN_INPUT_CURSOR_LOCAL, "silent host -> local");
    CHECK(c.reason && strcmp(c.reason, "silent-host-timeout") == 0,
          "silent-host reason");
    CHECK(c.disables == 1u, "one disable");
}

static void t_cursor_disable_is_paced(void)
{
    /* nvst_cursor.rs:228-245 failed_disable_is_paced_and_reports_handoff_
     * only_on_success */
    kn_input_cursor c;
    t_rec r;
    uint64_t now = 900u;
    kn_input_cursor_init(&c);
    t_rec_init(&r, 0);                       /* every send fails */
    kn_input_cursor_activate(&c, now);
    kn_input_cursor_notify(&c, now);
    CHECK(kn_input_cursor_update(&c, now, t_rec_send, &r) == 0,
          "a failed disable reports no handoff");
    CHECK(r.n == 1u, "the failed disable was attempted once");
    CHECK(c.state == KN_INPUT_CURSOR_DISABLING, "still disabling");
    kn_input_cursor_notify(&c, now);         /* must not re-arm */
    CHECK(kn_input_cursor_update(&c, now, t_rec_send, &r) == 0,
          "the retry is paced, not immediate");
    CHECK(r.n == 1u, "no command before the retry interval");
    r.ok = 1;
    CHECK(kn_input_cursor_update(&c, now + KN_INPUT_CURSOR_RETRY_MS,
                                 t_rec_send, &r) == 1,
          "handoff on the paced retry");
    CHECK(r.n == 2u, "two attempts total");
    CHECK(c.attempts == 2u, "attempts counted across failures");
}

static void t_cursor_disable_exhaustion(void)
{
    /* nvst_cursor.rs:246-266 exhausted_disable_retries_do_not_report_
     * local_composition */
    kn_input_cursor c;
    t_rec r;
    uint64_t now = 0u;
    unsigned i;
    int any_handoff = 0;
    kn_input_cursor_init(&c);
    t_rec_init(&r, 0);
    kn_input_cursor_activate(&c, now);
    kn_input_cursor_notify(&c, now);
    for (i = 0; i < KN_INPUT_CURSOR_MAX_ATTEMPTS + 3u; ++i) {
        uint64_t t = now + (uint64_t)KN_INPUT_CURSOR_RETRY_MS * i;
        if (kn_input_cursor_update(&c, t, t_rec_send, &r)) any_handoff = 1;
    }
    CHECK(!any_handoff, "exhausted retries never report local composition");
    CHECK(c.state == KN_INPUT_CURSOR_EXHAUSTED, "state -> exhausted");
    CHECK(c.attempts == KN_INPUT_CURSOR_MAX_ATTEMPTS, "eight attempts");
    CHECK(r.n == KN_INPUT_CURSOR_MAX_ATTEMPTS, "eight disable commands");
    CHECK(c.handoffs == 0u && c.disables == 0u, "no handoff counters");
    /* nvst_cursor.rs:262-264 — activate() straight out of EXHAUSTED
     * re-arms the handoff, without a reset in between. */
    t_rec_init(&r, 1);
    kn_input_cursor_activate(&c, now);
    kn_input_cursor_notify(&c, now);
    CHECK(kn_input_cursor_update(&c, now, t_rec_send, &r) == 1,
          "activate from exhausted re-arms the handoff");
    CHECK(c.state == KN_INPUT_CURSOR_LOCAL, "re-armed machine reaches local");

    /* reset re-arms the deadline from scratch (nvst_cursor.rs:267-286) */
    kn_input_cursor_reset(&c);
    CHECK(c.state == KN_INPUT_CURSOR_INACTIVE, "reset -> inactive");
    kn_input_cursor_activate(&c, 7000u);
    CHECK(c.state == KN_INPUT_CURSOR_WAITING && c.attempts == 1u &&
          c.deadline_ms == 7000u + KN_INPUT_CURSOR_SILENT_HOST_MS,
          "reactivation re-arms the deadline");
}

static void t_cursor_reset_semantics(void)
{
    /* nvst_cursor.rs:267-286 reset_cancels_retries_and_reactivation_
     * rearms_local_handoff — the three "nothing may be sent" legs plus a
     * completed handoff that is then re-armed by activate() alone. */
    kn_input_cursor c;
    uint64_t now = 100u;
    t_never = 0;
    kn_input_cursor_init(&c);

    /* (a) notify on an INACTIVE machine is inert */
    kn_input_cursor_notify(&c, now);
    CHECK(kn_input_cursor_update(&c, now, t_never_send, NULL) == 0,
          "an inactive machine never sends");

    /* (b) reset() cancels a pending handoff */
    kn_input_cursor_activate(&c, now);
    kn_input_cursor_notify(&c, now);
    kn_input_cursor_reset(&c);
    CHECK(kn_input_cursor_update(&c, now + KN_INPUT_CURSOR_SILENT_HOST_MS,
                                 t_never_send, NULL) == 0,
          "reset cancels the pending disable");
    CHECK(t_never == 0, "no command escaped a reset machine");

    /* (c) a completed handoff, then activate() alone re-arms the
     * silent-host path (no notify needed) */
    kn_input_cursor_activate(&c, now);
    kn_input_cursor_notify(&c, now);
    CHECK(kn_input_cursor_update(&c, now, t_never_send, NULL) == 1,
          "handoff completes");
    kn_input_cursor_activate(&c, now);
    CHECK(c.state == KN_INPUT_CURSOR_WAITING, "activate re-arms after local");
    CHECK(kn_input_cursor_update(&c, now + KN_INPUT_CURSOR_SILENT_HOST_MS,
                                 t_never_send, NULL) == 1,
          "the re-armed silent-host path hands off again");
    /* one enable pair is emitted at the 250 ms tick inside that window,
     * plus the two disables: never more than the documented ceiling */
    CHECK(t_never <= 3, "command emissions stay bounded");
}

static void t_cursor_failed_enables_keep_deadline(void)
{
    /* nvst_cursor.rs:287+ failed_enable_retries_do_not_cancel_silent_host_
     * deadline — a failing send must not extend the 3 s fallback. */
    kn_input_cursor c;
    t_rec r;
    uint64_t now = 4242u;
    int tick;
    uint64_t deadline;
    kn_input_cursor_init(&c);
    t_rec_init(&r, 0);
    kn_input_cursor_activate(&c, now);
    deadline = c.deadline_ms;
    for (tick = 0; tick < 12; ++tick)
        (void)kn_input_cursor_update(&c,
                                     now + (uint64_t)KN_INPUT_CURSOR_RETRY_MS *
                                               (uint64_t)tick,
                                     t_rec_send, &r);
    CHECK(c.deadline_ms == deadline, "failed enables never move the deadline");
    CHECK(c.state == KN_INPUT_CURSOR_WAITING, "still waiting after failures");
    /* the reference's second closure succeeds (nvst_cursor.rs:291-295):
     * the point of the test is that the DEADLINE survived the failed
     * enables, not that a failing disable hands off. */
    r.ok = 1;
    CHECK(kn_input_cursor_update(&c, now + KN_INPUT_CURSOR_SILENT_HOST_MS,
                                 t_rec_send, &r) == 1,
          "the deadline still falls back");
    CHECK(c.state == KN_INPUT_CURSOR_LOCAL, "fallback -> local");
    /* update on an untouched machine is a no-op */
    kn_input_cursor_init(&c);
    CHECK(kn_input_cursor_update(&c, now, t_rec_send, &r) == 0,
          "inactive machine emits nothing");
    CHECK(kn_input_cursor_update(NULL, now, t_rec_send, &r) == 0,
          "update NULL cursor");
    kn_input_cursor_notify(NULL, now);       /* must not crash */
    kn_input_cursor_activate(NULL, now);
    kn_input_cursor_reset(NULL);
    kn_input_cursor_init(NULL);
    CHECK(1, "NULL cursor contracts survive");
}

static void t_cursor_state_names(void)
{
    CHECK(strcmp(kn_input_cursor_state_name(KN_INPUT_CURSOR_INACTIVE),
                 "inactive") == 0, "state name inactive");
    CHECK(strcmp(kn_input_cursor_state_name(KN_INPUT_CURSOR_WAITING),
                 "waiting") == 0, "state name waiting");
    CHECK(strcmp(kn_input_cursor_state_name(KN_INPUT_CURSOR_DISABLING),
                 "disabling") == 0, "state name disabling");
    CHECK(strcmp(kn_input_cursor_state_name(KN_INPUT_CURSOR_LOCAL),
                 "local") == 0, "state name local");
    CHECK(strcmp(kn_input_cursor_state_name(KN_INPUT_CURSOR_EXHAUSTED),
                 "exhausted") == 0, "state name exhausted");
    CHECK(strcmp(kn_input_cursor_state_name((kn_input_cursor_state)99),
                 "?") == 0, "state name unknown");
}

/* ------------------------------------------- 4. names + the evidence side */

static void t_command_names(void)
{
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_INPUT_VERSION),
                 "input-version") == 0, "name input-version");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_ENABLE_INPUT),
                 "enable-input") == 0, "name enable-input");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_WINDOW_STATE),
                 "window-state") == 0, "name window-state");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_SYSTEM_STATE),
                 "system-state") == 0, "name system-state");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_GAMEPAD),
                 "gamepad-descriptor") == 0, "name gamepad-descriptor");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_CURSOR_CAPTURE),
                 "mouse-cursor-capture") == 0, "name cursor-capture");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_CURSOR_TRACKING),
                 "track-remote-cursor") == 0, "name cursor-tracking");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_REMOTE_INPUT),
                 "remote-input") == 0, "name remote-input");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_KEEPALIVE),
                 "keepalive") == 0, "name keepalive");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_SYSTEM_CURSOR),
                 "system-cursor") == 0, "name system-cursor");
    CHECK(strcmp(kn_input_command_name(KN_INPUT_CMD_BITMAP_CURSOR),
                 "bitmap-cursor") == 0, "name bitmap-cursor");
    /* the codes this client SENDS from kn_rx must also be named, so an
     * evidence line never prints a bare hex number for our own traffic */
    CHECK(kn_input_command_name(KN_INPUT_CMD_FRAME_ACK) != NULL,
          "name frame-ack");
    CHECK(kn_input_command_name(KN_INPUT_CMD_FRAME_PACING) != NULL,
          "name frame-pacing");
    CHECK(kn_input_command_name(KN_INPUT_CMD_QOS_REPORT) != NULL,
          "name qos-report");
    CHECK(kn_input_command_name(KN_INPUT_CMD_NACK_V2) != NULL, "name nack-v2");
    CHECK(kn_input_command_name(KN_INPUT_CMD_IDR_REQUEST) != NULL,
          "name idr-request");
    CHECK(kn_input_command_name(KN_INPUT_CMD_FRAME_INVALID) != NULL,
          "name frame-invalidation");
    CHECK(kn_input_command_name(0x1234u) == NULL, "unknown code has no name");
}

static void t_hist(void)
{
    kn_input_hist h;
    char buf[1024];
    int n;
    kn_input_hist_init(&h);
    n = kn_input_hist_json(&h, buf, sizeof buf);
    CHECK(n == 2 && strcmp(buf, "[]") == 0, "empty histogram is []");

    kn_input_hist_add(&h, KN_INPUT_CMD_INPUT_VERSION);
    {
        int i;
        for (i = 0; i < 5; ++i) kn_input_hist_add(&h, KN_INPUT_CMD_KEEPALIVE);
    }
    kn_input_hist_add(&h, KN_INPUT_CMD_SYSTEM_CURSOR);
    n = kn_input_hist_json(&h, buf, sizeof buf);
    CHECK(n > 0, "histogram serializes");
    CHECK(strcmp(buf, "[[\"020e\",1],[\"0200\",5],[\"010f\",1]]") == 0,
          "histogram JSON is first-sighting ordered");
    CHECK(h.records == 7u, "histogram counts every record");
    CHECK(h.n == 3u, "three distinct codes");

    /* a too-small buffer is a HARD failure, never truncated JSONL */
    CHECK(kn_input_hist_json(&h, buf, 3) == -1, "histogram refuses 3 bytes");
    CHECK(kn_input_hist_json(&h, buf, 20) == -1, "histogram refuses 20 bytes");
    CHECK(kn_input_hist_json(&h, NULL, 64) == -1, "histogram NULL out");
    CHECK(kn_input_hist_json(NULL, buf, sizeof buf) == -1, "histogram NULL h");
    CHECK(kn_input_hist_json(&h, buf, 0) == -1, "histogram zero length");

    /* slot exhaustion is counted, not silently dropped */
    kn_input_hist_init(&h);
    {
        unsigned i;
        for (i = 0; i < KN_INPUT_HIST_SLOTS + 8u; ++i)
            kn_input_hist_add(&h, (uint16_t)(0x1000u + i));
    }
    CHECK(h.n == KN_INPUT_HIST_SLOTS, "histogram caps at the slot count");
    CHECK(h.overflow == 8u, "overflow counted honestly");
    CHECK(h.records == KN_INPUT_HIST_SLOTS + 8u, "records count everything");
    n = kn_input_hist_json(&h, buf, sizeof buf);
    CHECK(n > 0 && buf[0] == '[' && buf[(size_t)n - 1] == ']',
          "a full histogram still serializes");
    kn_input_hist_add(NULL, 1u);
    kn_input_hist_init(NULL);
    CHECK(1, "histogram NULL contracts survive");
}

/* --------------------------------------------------- 5. bounds robustness
 * Every truncation of every golden, parsed. Under ASan/UBSan this is a
 * real out-of-bounds proof for the TLV walker (the R33 discipline: the
 * parser sees attacker-shaped lengths from a live rig). */
static void t_truncations(void)
{
    size_t i, cut, worst = 0, whole;
    for (i = 0; i < kn_input_kat_singles_n; ++i) {
        const kn_input_cmd_kat *k = &kn_input_kat_singles[i];
        whole = kn_input_scan(k->bytes, k->len, NULL, NULL);
        for (cut = 0; cut <= k->len; ++cut) {
            int v = kn_input_protocol_version(k->bytes, cut);
            size_t w = kn_input_scan(k->bytes, cut, NULL, NULL);
            int cn = kn_input_has_cursor_notification(k->bytes, cut);
            (void)v; (void)cn;
            if (w > worst) worst = w;
            /* a truncation can never invent more records than the whole */
            CHECK(w <= whole, "truncation never invents records");
        }
    }
    CHECK(worst <= 3u, "no truncation walked an absurd record count");
    /* a lying length field must not read past the buffer */
    {
        static const uint8_t lie[8] = { 0x0e, 0x02, 0xff, 0xff,
                                        0x00, 0x00, 0x00, 0x00 };
        CHECK(kn_input_scan(lie, sizeof lie, NULL, NULL) == 0u,
              "a 65535-byte claim in an 8-byte buffer ends the walk");
        CHECK(kn_input_protocol_version(lie, sizeof lie) == 0xffff,
              "the fallback still reads data[2..4] verbatim");
    }
    /* one byte past a complete record is ignored, not misparsed */
    {
        uint8_t buf[16];
        int n = kn_input_keepalive(0u, buf, sizeof buf);
        CHECK(n == 8, "keepalive for the tail test");
        buf[8] = 0xff;
        CHECK(kn_input_scan(buf, 9, NULL, NULL) == 1u,
              "a one-byte tail is not a record");
        CHECK(kn_input_protocol_version(buf, 9) == -1,
              "a keepalive-only buffer has no version");
    }
}

/* ------------------------------------------------------------- constants */

static void t_constants(void)
{
    /* nvst_input.rs:11-12 and nvst_cursor.rs:3-5, transcribed */
    CHECK(KN_INPUT_KEEPALIVE_INTERVAL_MS == 3000u, "keepalive interval 3 s");
    CHECK(KN_INPUT_HANDSHAKE_TIMEOUT_MS == 5000u, "handshake timeout 5 s");
    CHECK(KN_INPUT_CURSOR_RETRY_MS == 250u, "cursor retry 250 ms");
    CHECK(KN_INPUT_CURSOR_MAX_ATTEMPTS == 8u, "cursor max attempts 8");
    CHECK(KN_INPUT_CURSOR_SILENT_HOST_MS == 3000u, "silent host 3 s");
    CHECK(KN_INPUT_WINDOW_STATE_ACTIVE == 19u, "window state 19");
    CHECK(KN_INPUT_ACTIVATION_N == 8u, "eight activation messages");
    CHECK(KN_INPUT_CMD_MAX == 64u, "command ceiling 64");
    /* the codes, against nvst_input.rs:25-42 / nvst_control.rs:3-8 */
    CHECK(KN_INPUT_CMD_SYSTEM_CURSOR == 0x010f, "code 0x010f");
    CHECK(KN_INPUT_CMD_BITMAP_CURSOR == 0x0110, "code 0x0110");
    CHECK(KN_INPUT_CMD_KEEPALIVE == 0x0200, "code 0x0200");
    CHECK(KN_INPUT_CMD_FRAME_PACING == 0x0203, "code 0x0203");
    CHECK(KN_INPUT_CMD_FRAME_ACK == 0x0204, "code 0x0204");
    CHECK(KN_INPUT_CMD_REMOTE_INPUT == 0x0206, "code 0x0206");
    CHECK(KN_INPUT_CMD_QOS_REPORT == 0x0207, "code 0x0207");
    CHECK(KN_INPUT_CMD_ENABLE_INPUT == 0x020b, "code 0x020b");
    CHECK(KN_INPUT_CMD_GAMEPAD == 0x020d, "code 0x020d");
    CHECK(KN_INPUT_CMD_INPUT_VERSION == 0x020e, "code 0x020e");
    CHECK(KN_INPUT_CMD_FRAME_INVALID == 0x0301, "code 0x0301");
    CHECK(KN_INPUT_CMD_IDR_REQUEST == 0x0302, "code 0x0302");
    CHECK(KN_INPUT_CMD_CURSOR_CAPTURE == 0x0308, "code 0x0308");
    CHECK(KN_INPUT_CMD_CURSOR_TRACKING == 0x030d, "code 0x030d");
    CHECK(KN_INPUT_CMD_NACK_V2 == 0x0317, "code 0x0317");
    CHECK(KN_INPUT_CMD_WINDOW_STATE == 0x0320, "code 0x0320");
    CHECK(KN_INPUT_CMD_SYSTEM_STATE == 0x0321, "code 0x0321");
}

int main(void)
{
    printf("== test_kninput: the NVST control/input plane ==\n");
    t_constants();
    t_activation_chain();
    t_singles();
    t_builder_edges();
    t_versions();
    t_scan();
    t_cursor_notify_vectors();
    t_cursor_notification_handoff();
    t_cursor_silent_host();
    t_cursor_disable_is_paced();
    t_cursor_disable_exhaustion();
    t_cursor_reset_semantics();
    t_cursor_failed_enables_keep_deadline();
    t_cursor_state_names();
    t_command_names();
    t_hist();
    t_truncations();
    return kn_test_summary("test_kninput");
}
