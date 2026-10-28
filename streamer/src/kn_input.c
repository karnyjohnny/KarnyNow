/* kn_input.c — the NVST control/input plane. See kn_input.h for the WHY.
 *
 * Every byte here is a port of a named line in the reference client at
 * commit a49b124; tools/input_reference.py is the independent Python
 * model of the same rules and self-proves against the reference's own
 * byte-exact Rust goldens before emitting kn_input_kat_vectors.h.
 * test_kninput replays BOTH the goldens and the three reference
 * cursor-state-machine tests (nvst_cursor.rs:165-250) as C.
 */
#include "kn/kn_input.h"

#include <stdio.h>      /* snprintf (the histogram's JSON serializer) */
#include <string.h>

/* ------------------------------------------------------------- helpers */

static void kn_in_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void kn_in_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void kn_in_put_le64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; ++i)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xffull);
}

static void kn_in_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static void kn_in_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static void kn_in_put_be64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; ++i)
        p[7 - i] = (uint8_t)((v >> (8 * i)) & 0xffull);
}

static uint16_t kn_in_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------- builders */

int kn_input_command(uint16_t code, const uint8_t *payload, size_t plen,
                     uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:1385-1391 */
    if (!out || (plen && !payload)) return -1;
    if (plen > 0xffffu) return -1;
    if (out_len < KN_INPUT_TLV_HDR + plen) return -2;
    kn_in_put_le16(out + 0, code);
    kn_in_put_le16(out + 2, (uint16_t)plen);
    if (plen) memcpy(out + KN_INPUT_TLV_HDR, payload, plen);
    return (int)(KN_INPUT_TLV_HDR + plen);
}

int kn_input_keepalive(uint32_t stream_value, uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:1346-1348 — golden 0002040000000000 for value 0 */
    uint8_t payload[4];
    kn_in_put_le32(payload, stream_value);
    return kn_input_command(KN_INPUT_CMD_KEEPALIVE, payload, sizeof payload,
                            out, out_len);
}

int kn_input_enable_input(uint32_t counter, int enabled,
                          uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:1350-1357 — {stream index 0, counter, enabled} */
    uint8_t payload[12];
    kn_in_put_le32(payload + 0, 0u);
    kn_in_put_le32(payload + 4, counter);
    kn_in_put_le32(payload + 8, enabled ? 1u : 0u);
    return kn_input_command(KN_INPUT_CMD_ENABLE_INPUT, payload, sizeof payload,
                            out, out_len);
}

int kn_input_device_descriptor(uint64_t timestamp_us, uint16_t bitmap,
                               uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:1358-1371. The body template is copied verbatim;
     * body[9..11] is the device bitmap (LE). */
    static const uint8_t body[39] = {
        0x22, 0x0c, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x14, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x55, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t payload[1 + 8 + sizeof body];
    payload[0] = 0x23u;
    kn_in_put_be64(payload + 1, timestamp_us);       /* BE, unlike haptics */
    memcpy(payload + 9, body, sizeof body);
    kn_in_put_le16(payload + 9 + 9, bitmap);         /* body[9..11] */
    return kn_input_command(KN_INPUT_CMD_GAMEPAD, payload, sizeof payload,
                            out, out_len);
}

int kn_input_cursor_capture(int enabled, uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:1331-1333 */
    uint8_t payload[1];
    payload[0] = enabled ? 1u : 0u;
    return kn_input_command(KN_INPUT_CMD_CURSOR_CAPTURE, payload,
                            sizeof payload, out, out_len);
}

int kn_input_cursor_tracking(int enabled, uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:1335-1337 */
    uint8_t payload[1];
    payload[0] = enabled ? 1u : 0u;
    return kn_input_command(KN_INPUT_CMD_CURSOR_TRACKING, payload,
                            sizeof payload, out, out_len);
}

int kn_input_state_change(uint16_t code, uint32_t state, uint32_t frame,
                          uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:1372-1383 — {stream index 0, state, frame number} */
    uint8_t payload[12];
    kn_in_put_le32(payload + 0, 0u);
    kn_in_put_le32(payload + 4, state);
    kn_in_put_le32(payload + 8, frame);
    return kn_input_command(code, payload, sizeof payload, out, out_len);
}

/* remote_input_packet(input_type, body) — nvst_input.rs:989-995:
 * BE u32 (4 + body length), type LE u32, body. */
static size_t kn_in_remote_packet(uint32_t input_type, const uint8_t *body,
                                  size_t blen, uint8_t *out, size_t out_len)
{
    if (out_len < 8 + blen) return 0;
    kn_in_put_be32(out + 0, (uint32_t)(4u + blen));
    kn_in_put_le32(out + 4, input_type);
    if (blen) memcpy(out + 8, body, blen);
    return 8 + blen;
}

int kn_input_haptics(uint64_t timestamp_us, int enabled,
                     uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:1339-1344 -> remote_input_message(:956-987) with
     * INPUT_HAPTICS_ENABLED (13):
     *   inner   = remote_input_packet(13, u16 LE enabled)        -> 10 B
     *   padded  = max(24, ceil(10/8)*8 = 16) = 24                -> 24 B
     *   timestamps: 13 is not KEY_DOWN/KEY_UP/MOUSE_ABSOLUTE, so
     *               timestamp_count = 0 + 1 = 1 -> append ts LE u64
     *   body    = 24 + 8                                         -> 32 B
     *   payload = remote_input_packet(0x0e, body)                -> 40 B
     *   command = TLV(0x0206, payload)                           -> 44 B
     * Route: ControlReliable (sid 0) — 13 is not MOUSE_*. */
    uint8_t haptics_body[2];
    uint8_t inner[8 + sizeof haptics_body];
    uint8_t body[32];
    uint8_t payload[8 + sizeof body];
    size_t inner_len, body_len, payload_len;

    haptics_body[0] = enabled ? 1u : 0u;
    haptics_body[1] = 0u;
    inner_len = kn_in_remote_packet(13u, haptics_body, sizeof haptics_body,
                                    inner, sizeof inner);
    if (!inner_len || inner_len > 24u) return -1;
    memset(body, 0, sizeof body);
    memcpy(body, inner, inner_len);           /* pad-to-24 with zeros */
    body_len = 24u;
    kn_in_put_le64(body + body_len, timestamp_us);   /* LE here (not BE) */
    body_len += 8u;
    payload_len = kn_in_remote_packet(0x0eu, body, body_len, payload,
                                      sizeof payload);
    if (!payload_len) return -1;
    return kn_input_command(KN_INPUT_CMD_REMOTE_INPUT, payload, payload_len,
                            out, out_len);
}

int kn_input_mouse_relative(int16_t dx, int16_t dy, uint64_t timestamp_us,
                            uint8_t *out, size_t out_len)
{
    /* Round XLI.1 — nvst_input.rs:727-741 (the RELATIVE encode arm) fed
     * through remote_input_message (:956-987) with INPUT_MOUSE_RELATIVE
     * (7):
     *   body    = dx BE i16 | dy BE i16 | 00 00                      ->  6 B
     *   inner   = remote_input_packet(7, body)                       -> 14 B
     *   padded  = max(24, ceil(14/8)*8 = 16) = 24                    -> 24 B
     *   timestamps: 7 is not KEY_DOWN/KEY_UP/MOUSE_ABSOLUTE, so
     *               timestamp_count = 0 + 1 = 1 -> append ts LE u64
     *   payload = remote_input_packet(0x0e, 24 + 8)                  -> 40 B
     *   command = TLV(0x0206, payload)                               -> 44 B
     * Route: ControlPartial (nvst_input.rs:969) = sid 6, unordered,
     * PR-SCTP TTL 300 ms (NVST_CHANNEL_PROFILE[3]) — the reference's
     * own comment: ordered PR-SCTP would let one lost report block
     * every newer one for 300 ms ("delayed motion followed by
     * catch-up"). Rust golden: nvst_input.rs:1888-1911, dx=dy=24 at
     * ts=0x01861330 — pinned in test_kninput AND tools/input_reference.py
     * (two implementations). */
    uint8_t motion_body[6];
    uint8_t inner[8 + sizeof motion_body];
    uint8_t body[32];
    uint8_t payload[8 + sizeof body];
    size_t inner_len, body_len, payload_len;

    kn_in_put_be16(motion_body + 0, (uint16_t)dx);
    kn_in_put_be16(motion_body + 2, (uint16_t)dy);
    motion_body[4] = 0u;
    motion_body[5] = 0u;
    inner_len = kn_in_remote_packet(7u, motion_body, sizeof motion_body,
                                    inner, sizeof inner);
    if (!inner_len || inner_len > 24u) return -1;
    memset(body, 0, sizeof body);
    memcpy(body, inner, inner_len);           /* pad-to-24 with zeros */
    body_len = 24u;
    kn_in_put_le64(body + body_len, timestamp_us);   /* LE here (not BE) */
    body_len += 8u;
    payload_len = kn_in_remote_packet(0x0eu, body, body_len, payload,
                                      sizeof payload);
    if (!payload_len) return -1;
    return kn_input_command(KN_INPUT_CMD_REMOTE_INPUT, payload, payload_len,
                            out, out_len);
}

int kn_input_activation_chain(
    uint64_t timestamp_us,
    uint8_t out[KN_INPUT_ACTIVATION_N][KN_INPUT_CMD_MAX],
    size_t lens[KN_INPUT_ACTIVATION_N])
{
    /* nvst_input.rs:1318-1329 — order matters (input is disabled before
     * the descriptor lands and re-enabled after the client state). */
    int n;
    if (!out || !lens) return -1;
    memset(out, 0, KN_INPUT_ACTIVATION_N * KN_INPUT_CMD_MAX);

    n = kn_input_enable_input(1u, 0, out[0], KN_INPUT_CMD_MAX);
    if (n <= 0) return -1;
    lens[0] = (size_t)n;

    n = kn_input_device_descriptor(timestamp_us, 0u, out[1], KN_INPUT_CMD_MAX);
    if (n <= 0) return -1;
    lens[1] = (size_t)n;

    n = kn_input_cursor_capture(1, out[2], KN_INPUT_CMD_MAX);
    if (n <= 0) return -1;
    lens[2] = (size_t)n;

    n = kn_input_cursor_tracking(1, out[3], KN_INPUT_CMD_MAX);
    if (n <= 0) return -1;
    lens[3] = (size_t)n;

    n = kn_input_state_change(KN_INPUT_CMD_WINDOW_STATE,
                              KN_INPUT_WINDOW_STATE_ACTIVE, 0u,
                              out[4], KN_INPUT_CMD_MAX);
    if (n <= 0) return -1;
    lens[4] = (size_t)n;

    n = kn_input_state_change(KN_INPUT_CMD_SYSTEM_STATE, 0u, 0u,
                              out[5], KN_INPUT_CMD_MAX);
    if (n <= 0) return -1;
    lens[5] = (size_t)n;

    n = kn_input_enable_input(1u, 1, out[6], KN_INPUT_CMD_MAX);
    if (n <= 0) return -1;
    lens[6] = (size_t)n;

    n = kn_input_haptics(timestamp_us, 1, out[7], KN_INPUT_CMD_MAX);
    if (n <= 0) return -1;
    lens[7] = (size_t)n;

    return (int)KN_INPUT_ACTIVATION_N;
}

/* -------------------------------------------------------------- parsers */

size_t kn_input_scan(const uint8_t *bytes, size_t len,
                     kn_input_cmd_fn cb, void *ctx)
{
    /* nvst_input.rs:404-424 (the record walk). A record whose declared
     * payload would overrun the buffer ends the walk. */
    size_t cursor = 0, n = 0;
    if (!bytes) return 0;
    while (cursor + KN_INPUT_TLV_HDR <= len) {
        uint16_t code = kn_in_get_le16(bytes + cursor);
        uint16_t plen = kn_in_get_le16(bytes + cursor + 2);
        size_t start = cursor + KN_INPUT_TLV_HDR;
        size_t end = start + (size_t)plen;
        if (end > len || end < start) break;
        if (cb) cb(ctx, code, bytes + start, (size_t)plen);
        n++;
        cursor = end;
    }
    return n;
}

int kn_input_protocol_version(const uint8_t *bytes, size_t len)
{
    /* nvst_input.rs:404-441 — EXACT semantics, both fallbacks included.
     * Fallback #1: the first record is INPUT_VERSION but its declared
     * payload overran -> read data[2..4] verbatim (or 2 when shorter).
     * Fallback #2: data[0] == 0x0e -> return the first u16 as-is. */
    size_t cursor = 0;
    uint16_t first;
    if (!bytes || len < 2u) return -1;
    while (cursor + KN_INPUT_TLV_HDR <= len) {
        uint16_t code = kn_in_get_le16(bytes + cursor);
        uint16_t plen = kn_in_get_le16(bytes + cursor + 2);
        size_t start = cursor + KN_INPUT_TLV_HDR;
        size_t end = start + (size_t)plen;
        if (end > len || end < start) break;
        if (code == KN_INPUT_CMD_INPUT_VERSION && plen >= 2u)
            return (int)kn_in_get_le16(bytes + start);
        cursor = end;
    }
    first = kn_in_get_le16(bytes);
    if (first == KN_INPUT_CMD_INPUT_VERSION)
        return (len >= 4u) ? (int)kn_in_get_le16(bytes + 2) : 2;
    if (bytes[0] == 0x0eu) return (int)first;
    return -1;
}

typedef struct {
    int found;
} kn_in_cursor_scan_ctx;

static void kn_in_cursor_scan_cb(void *vctx, uint16_t code,
                                 const uint8_t *payload, size_t plen)
{
    kn_in_cursor_scan_ctx *ctx = (kn_in_cursor_scan_ctx *)vctx;
    (void)payload;
    (void)plen;
    if (code == KN_INPUT_CMD_SYSTEM_CURSOR || code == KN_INPUT_CMD_BITMAP_CURSOR)
        ctx->found = 1;
}

int kn_input_has_cursor_notification(const uint8_t *bytes, size_t len)
{
    /* nvst.rs:8322-8326 — a cursor command anywhere in the record list
     * drives cursor_capture.notify(). */
    kn_in_cursor_scan_ctx ctx;
    ctx.found = 0;
    (void)kn_input_scan(bytes, len, kn_in_cursor_scan_cb, &ctx);
    return ctx.found;
}

const char *kn_input_command_name(uint16_t code)
{
    switch (code) {
    case KN_INPUT_CMD_SYSTEM_CURSOR:   return "system-cursor";
    case KN_INPUT_CMD_BITMAP_CURSOR:   return "bitmap-cursor";
    case KN_INPUT_CMD_KEEPALIVE:       return "keepalive";
    case KN_INPUT_CMD_FRAME_PACING:    return "frame-pacing";
    case KN_INPUT_CMD_FRAME_ACK:       return "frame-ack";
    case KN_INPUT_CMD_REMOTE_INPUT:    return "remote-input";
    case KN_INPUT_CMD_QOS_REPORT:      return "qos-report";
    case KN_INPUT_CMD_ENABLE_INPUT:    return "enable-input";
    case KN_INPUT_CMD_GAMEPAD:         return "gamepad-descriptor";
    case KN_INPUT_CMD_INPUT_VERSION:   return "input-version";
    case KN_INPUT_CMD_FRAME_INVALID:   return "frame-invalidation";
    case KN_INPUT_CMD_IDR_REQUEST:     return "idr-request";
    case KN_INPUT_CMD_CURSOR_CAPTURE:  return "mouse-cursor-capture";
    case KN_INPUT_CMD_CURSOR_TRACKING: return "track-remote-cursor";
    case KN_INPUT_CMD_NACK_V2:         return "nack-v2";
    case KN_INPUT_CMD_WINDOW_STATE:    return "window-state";
    case KN_INPUT_CMD_SYSTEM_STATE:    return "system-state";
    default:                           return NULL;
    }
}

/* ------------------------------------------------------------- histogram */

void kn_input_hist_init(kn_input_hist *h)
{
    if (!h) return;
    memset(h, 0, sizeof *h);
}

void kn_input_hist_add(kn_input_hist *h, uint16_t code)
{
    size_t i;
    if (!h) return;
    h->records++;
    for (i = 0; i < h->n; ++i) {
        if (h->slot[i].code == code) {
            h->slot[i].count++;
            return;
        }
    }
    if (h->n >= KN_INPUT_HIST_SLOTS) {
        h->overflow++;
        return;
    }
    h->slot[h->n].code = code;
    h->slot[h->n].count = 1u;
    h->n++;
}

int kn_input_hist_json(const kn_input_hist *h, char *out, size_t out_len)
{
    size_t used = 0, i;
    if (!h || !out || out_len < 3u) return -1;
    out[used++] = '[';
    for (i = 0; i < h->n; ++i) {
        char tmp[32];
        int n = snprintf(tmp, sizeof tmp, "%s[\"%04x\",%lu]",
                         i ? "," : "", (unsigned)h->slot[i].code,
                         (unsigned long)h->slot[i].count);
        if (n <= 0 || (size_t)n >= sizeof tmp) return -1;
        if (used + (size_t)n + 2u > out_len) return -1;  /* ']' + NUL */
        memcpy(out + used, tmp, (size_t)n);
        used += (size_t)n;
    }
    out[used++] = ']';
    out[used] = '\0';
    return (int)used;
}

/* ------------------------------------------------- cursor-capture machine */

void kn_input_cursor_init(kn_input_cursor *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->state = KN_INPUT_CURSOR_INACTIVE;
    c->reason = NULL;
}

void kn_input_cursor_activate(kn_input_cursor *c, uint64_t now_ms)
{
    /* nvst_cursor.rs:52-58 — attempts starts at 1 because the activation
     * chain itself already sent the first capture(true). */
    if (!c) return;
    c->state = KN_INPUT_CURSOR_WAITING;
    c->deadline_ms = now_ms + KN_INPUT_CURSOR_SILENT_HOST_MS;
    c->retry_at_ms = now_ms + KN_INPUT_CURSOR_RETRY_MS;
    c->attempts = 1u;
    c->reason = NULL;
}

void kn_input_cursor_reset(kn_input_cursor *c)
{
    kn_input_cursor_init(c);
}

void kn_input_cursor_notify(kn_input_cursor *c, uint64_t now_ms)
{
    /* nvst_cursor.rs:64-72 — only Waiting reacts. */
    if (!c || c->state != KN_INPUT_CURSOR_WAITING) return;
    c->state = KN_INPUT_CURSOR_DISABLING;
    c->retry_at_ms = now_ms;
    c->attempts = 0u;
    c->reason = "cursor-notification";
}

int kn_input_cursor_update(kn_input_cursor *c, uint64_t now_ms,
                           kn_input_cursor_send send, void *ctx)
{
    /* nvst_cursor.rs:74-126. The silent-host deadline is evaluated
     * FIRST, in the same call, and falls straight through into the
     * Disabling branch (that is what the reference's match does). */
    if (!c) return 0;
    if (c->state == KN_INPUT_CURSOR_WAITING && now_ms >= c->deadline_ms) {
        c->state = KN_INPUT_CURSOR_DISABLING;
        c->retry_at_ms = now_ms;
        c->attempts = 0u;
        c->reason = "silent-host-timeout";
    }
    if (c->state == KN_INPUT_CURSOR_WAITING) {
        if (c->attempts < KN_INPUT_CURSOR_MAX_ATTEMPTS &&
            now_ms >= c->retry_at_ms) {
            int sent_capture, sent_tracking;
            c->attempts++;
            sent_capture = send
                ? send(ctx, KN_INPUT_CURSOR_CMD_CAPTURE_ON) : 0;
            sent_tracking = send
                ? send(ctx, KN_INPUT_CURSOR_CMD_TRACKING) : 0;
            if (sent_capture) c->enables++;
            if (sent_tracking) c->trackings++;
            c->retry_at_ms = now_ms + KN_INPUT_CURSOR_RETRY_MS;
        }
        return 0;
    }
    if (c->state == KN_INPUT_CURSOR_DISABLING && now_ms >= c->retry_at_ms) {
        int sent = send ? send(ctx, KN_INPUT_CURSOR_CMD_CAPTURE_OFF) : 0;
        c->attempts++;
        if (sent) {
            c->disables++;
            c->state = KN_INPUT_CURSOR_LOCAL;
            c->handoffs++;
            return 1;
        }
        if (c->attempts == KN_INPUT_CURSOR_MAX_ATTEMPTS)
            c->state = KN_INPUT_CURSOR_EXHAUSTED;
        else
            c->retry_at_ms = now_ms + KN_INPUT_CURSOR_RETRY_MS;
    }
    return 0;
}

const char *kn_input_cursor_state_name(kn_input_cursor_state s)
{
    switch (s) {
    case KN_INPUT_CURSOR_INACTIVE:  return "inactive";
    case KN_INPUT_CURSOR_WAITING:   return "waiting";
    case KN_INPUT_CURSOR_DISABLING: return "disabling";
    case KN_INPUT_CURSOR_LOCAL:     return "local";
    case KN_INPUT_CURSOR_EXHAUSTED: return "exhausted";
    default:                        return "?";
    }
}
