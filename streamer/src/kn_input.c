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
static int kn_in_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static size_t kn_in_remote_packet(uint32_t input_type, const uint8_t *body,
                                  size_t blen, uint8_t *out, size_t out_len)
{
    if (out_len < 8 + blen) return 0;
    kn_in_put_be32(out + 0, (uint32_t)(4u + blen));
    kn_in_put_le32(out + 4, input_type);
    if (blen) memcpy(out + 8, body, blen);
    return 8 + blen;
}

/* Round L (F19) — the shared 0x0206 remote-input envelope, factored out
 * of haptics/mouse-relative so the four new discrete builders cannot
 * drift from the two golden-pinned ones. nvst_input.rs:956-987:
 *   inner   = remote_input_packet(type, body)
 *   padded  = max(24, ceil(len(inner)/8)*8), zero-filled
 *   stamps  = 1 + (type in {KEY_DOWN, KEY_UP, MOUSE_ABSOLUTE}) copies of
 *             the timestamp as LE u64 (BE nowhere in this envelope)
 *   payload = remote_input_packet(0x0e, padded || stamps)
 *   command = TLV(0x0206, payload)
 * Every body this project builds is <= 14 B, so inner <= 22 B and the pad
 * is always to exactly 24 — hence 44 B (one stamp) or 52 B (two). */
static int kn_in_remote_input(uint32_t input_type, const uint8_t *body,
                              size_t blen, uint64_t timestamp_us,
                              uint8_t *out, size_t out_len)
{
    uint8_t inner[8 + 16];
    uint8_t padded[24 + 16];
    uint8_t payload[8 + sizeof padded];
    size_t n, len;
    unsigned stamps;

    if (!body && blen) return -1;
    if (blen > 16u) return -1;
    n = kn_in_remote_packet(input_type, body, blen, inner, sizeof inner);
    if (!n || n > 24u) return -1;
    memset(padded, 0, sizeof padded);
    memcpy(padded, inner, n);
    len = 24u;
    stamps = (input_type == (uint32_t)KN_INPUT_TYPE_KEY_DOWN ||
              input_type == (uint32_t)KN_INPUT_TYPE_KEY_UP ||
              input_type == (uint32_t)KN_INPUT_TYPE_MOUSE_ABSOLUTE) ? 2u : 1u;
    while (stamps--) {
        kn_in_put_le64(padded + len, timestamp_us);
        len += 8u;
    }
    n = kn_in_remote_packet(0x0eu, padded, len, payload, sizeof payload);
    if (!n) return -1;
    return kn_input_command(KN_INPUT_CMD_REMOTE_INPUT, payload, n, out,
                            out_len);
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

    haptics_body[0] = enabled ? 1u : 0u;      /* u16 LE, unlike the F19
                                                 bodies which are BE */
    haptics_body[1] = 0u;
    return kn_in_remote_input(KN_INPUT_TYPE_HAPTICS, haptics_body,
                              sizeof haptics_body, timestamp_us, out,
                              out_len);
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

    kn_in_put_be16(motion_body + 0, (uint16_t)dx);
    kn_in_put_be16(motion_body + 2, (uint16_t)dy);
    motion_body[4] = 0u;
    motion_body[5] = 0u;
    return kn_in_remote_input(KN_INPUT_TYPE_MOUSE_RELATIVE, motion_body,
                              sizeof motion_body, timestamp_us, out,
                              out_len);
}

/* ------------------------------------------------- discrete input (F19)
 * Round L. Provenance for every byte is in kn_input.h; the two
 * implementations rule is satisfied by tools/input_reference.py, which
 * self-proves against the reference's OWN Rust goldens
 * (nvst_input.rs:1868-1911) before emitting kn_input_kat_vectors.h. */

int kn_input_key(uint16_t vk, uint16_t modifiers, int pressed,
                 uint64_t timestamp_us, uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:699-725: key BE u16 | modifiers BE u16 | 10 zeros. */
    uint8_t body[14];

    kn_in_put_be16(body + 0, vk);
    kn_in_put_be16(body + 2, modifiers);
    memset(body + 4, 0, 10);
    return kn_in_remote_input(pressed ? (uint32_t)KN_INPUT_TYPE_KEY_DOWN
                                      : (uint32_t)KN_INPUT_TYPE_KEY_UP,
                              body, sizeof body, timestamp_us, out, out_len);
}

int kn_input_mouse_button(unsigned button, int pressed,
                          uint64_t timestamp_us,
                          uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:760-768: [button, 0]. Ids are the Raw-Input order. */
    uint8_t body[2];

    if (button < 1u || button > 5u) return -1;
    body[0] = (uint8_t)button;
    body[1] = 0u;
    return kn_in_remote_input(pressed ? (uint32_t)KN_INPUT_TYPE_BUTTON_DOWN
                                      : (uint32_t)KN_INPUT_TYPE_BUTTON_UP,
                              body, sizeof body, timestamp_us, out, out_len);
}

int kn_input_mouse_absolute(uint16_t x, uint16_t y, uint16_t width,
                            uint16_t height, uint64_t timestamp_us,
                            uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:742-758 over the producer's 26-byte native event
     * (opennow-streamer-core/src/lib.rs:2939-2955): x,y BE u16, the
     * constant 08 00 in place of the event's zero u16, then the viewport
     * w,h BE u16. Pixels inside w x h — NOT the /65535 normalization of
     * the server->client cursor notification. */
    uint8_t body[10];

    if (!width || !height) return -1;
    kn_in_put_be16(body + 0, x);
    kn_in_put_be16(body + 2, y);
    body[4] = 0x08u;
    body[5] = 0x00u;
    kn_in_put_be16(body + 6, width);
    kn_in_put_be16(body + 8, height);
    return kn_in_remote_input(KN_INPUT_TYPE_MOUSE_ABSOLUTE, body,
                              sizeof body, timestamp_us, out, out_len);
}

int kn_input_wheel(int16_t delta_x, int16_t delta_y, uint64_t timestamp_us,
                   uint8_t *out, size_t out_len)
{
    /* nvst_input.rs:769-782: dx BE i16 | dy BE i16 | 00 00. */
    uint8_t body[6];

    kn_in_put_be16(body + 0, (uint16_t)delta_x);
    kn_in_put_be16(body + 2, (uint16_t)delta_y);
    body[4] = 0u;
    body[5] = 0u;
    return kn_in_remote_input(KN_INPUT_TYPE_WHEEL, body, sizeof body,
                              timestamp_us, out, out_len);
}

int kn_input_route_sid(uint32_t input_type)
{
    /* nvst_input.rs:969 (route) + :72-100 (NVST_CHANNEL_PROFILE):
     * ControlPartial = sid 6, ControlReliable = sid 0. */
    switch (input_type) {
    case (uint32_t)KN_INPUT_TYPE_MOUSE_RELATIVE:
    case (uint32_t)KN_INPUT_TYPE_MOUSE_ABSOLUTE:
        return 6;
    case (uint32_t)KN_INPUT_TYPE_KEY_DOWN:
    case (uint32_t)KN_INPUT_TYPE_KEY_UP:
    case (uint32_t)KN_INPUT_TYPE_BUTTON_DOWN:
    case (uint32_t)KN_INPUT_TYPE_BUTTON_UP:
    case (uint32_t)KN_INPUT_TYPE_WHEEL:
    case (uint32_t)KN_INPUT_TYPE_HAPTICS:
        return 0;
    default:
        return -1;
    }
}

int kn_input_vk_from_name(const char *name)
{
    /* media.rs:323-357 shortcut_virtual_key, plus the four modifier VKs
     * (0x10/0x11/0x12/0x5B) as an extension — a script has to be able to
     * hold a modifier down as a key, which the reference never needs
     * because it parses chords instead. */
    static const struct { const char *n; int vk; } tbl[] = {
        { "enter", 0x0D }, { "return", 0x0D }, { "escape", 0x1B },
        { "esc", 0x1B }, { "backspace", 0x08 }, { "tab", 0x09 },
        { "space", 0x20 }, { "left", 0x25 }, { "up", 0x26 },
        { "right", 0x27 }, { "down", 0x28 }, { "insert", 0x2D },
        { "delete", 0x2E }, { "del", 0x2E }, { "home", 0x24 },
        { "end", 0x23 }, { "pageup", 0x21 }, { "pgup", 0x21 },
        { "pagedown", 0x22 }, { "pgdown", 0x22 }, { "printscreen", 0x2A },
        { "print", 0x2A }, { "pause", 0x13 },
        { "shift", 0x10 }, { "ctrl", 0x11 }, { "control", 0x11 },
        { "alt", 0x12 }, { "win", 0x5B }, { "meta", 0x5B },
        { "cmd", 0x5B }, { "super", 0x5B }
    };
    char low[24];
    size_t i, n = 0;

    if (!name || !name[0]) return -1;
    while (name[n] && n + 1 < sizeof low) {
        char c = name[n];
        low[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        n++;
    }
    if (name[n]) return -1;                  /* too long to be a key name */
    low[n] = '\0';
    if (n == 1u) {
        char c = low[0];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            return (int)(unsigned char)(c >= 'a' && c <= 'z'
                                            ? (char)(c - 'a' + 'A') : c);
        return -1;
    }
    if (low[0] == 'f') {                     /* f1..f24 -> 0x70 + n */
        int num = 0;
        for (i = 1; i < n; ++i) {
            if (low[i] < '0' || low[i] > '9') { num = -1; break; }
            num = num * 10 + (low[i] - '0');
            if (num > 24) { num = -1; break; }
        }
        if (num >= 1 && num <= 24) return 0x6F + num;
        if (num == -1) return -1;
    }
    for (i = 0; i < sizeof tbl / sizeof tbl[0]; ++i)
        if (kn_in_streq(low, tbl[i].n)) return tbl[i].vk;
    return -1;
}

int kn_input_vk_from_ascii(int ch, uint16_t *vk, uint16_t *mods)
{
    /* The US-layout table (VK_OEM_* family; the same codes
     * docs/keyboard-mapping.md lists as the wire VKs). A session whose
     * remote layout is not US will disagree on the punctuation rows —
     * documented, and the reason `type` reports what it could not map. */
    static const struct { char c; uint16_t vk; uint16_t m; } pun[] = {
        { '!', 0x31, KN_INPUT_MOD_SHIFT }, { '@', 0x32, KN_INPUT_MOD_SHIFT },
        { '#', 0x33, KN_INPUT_MOD_SHIFT }, { '$', 0x34, KN_INPUT_MOD_SHIFT },
        { '%', 0x35, KN_INPUT_MOD_SHIFT }, { '^', 0x36, KN_INPUT_MOD_SHIFT },
        { '&', 0x37, KN_INPUT_MOD_SHIFT }, { '*', 0x38, KN_INPUT_MOD_SHIFT },
        { '(', 0x39, KN_INPUT_MOD_SHIFT }, { ')', 0x30, KN_INPUT_MOD_SHIFT },
        { '_', 0xBD, KN_INPUT_MOD_SHIFT }, { '+', 0xBB, KN_INPUT_MOD_SHIFT },
        { '{', 0xDB, KN_INPUT_MOD_SHIFT }, { '}', 0xDD, KN_INPUT_MOD_SHIFT },
        { '|', 0xDC, KN_INPUT_MOD_SHIFT }, { ':', 0xBA, KN_INPUT_MOD_SHIFT },
        { '"', 0xDE, KN_INPUT_MOD_SHIFT }, { '<', 0xBC, KN_INPUT_MOD_SHIFT },
        { '>', 0xBE, KN_INPUT_MOD_SHIFT }, { '?', 0xBF, KN_INPUT_MOD_SHIFT },
        { '~', 0xC0, KN_INPUT_MOD_SHIFT },
        { '-', 0xBD, 0 }, { '=', 0xBB, 0 }, { '[', 0xDB, 0 },
        { ']', 0xDD, 0 }, { '\\', 0xDC, 0 }, { ';', 0xBA, 0 },
        { '\'', 0xDE, 0 }, { ',', 0xBC, 0 }, { '.', 0xBE, 0 },
        { '/', 0xBF, 0 }, { '`', 0xC0, 0 },
        { '\n', 0x0D, 0 }, { '\t', 0x09, 0 }, { '\r', 0x0D, 0 }
    };
    size_t i;

    if (!vk || !mods) return -1;
    if (ch >= 'a' && ch <= 'z') { *vk = (uint16_t)(ch - 'a' + 'A'); *mods = 0;
                                  return 0; }
    if (ch >= 'A' && ch <= 'Z') { *vk = (uint16_t)ch;
                                  *mods = KN_INPUT_MOD_SHIFT; return 0; }
    if (ch >= '0' && ch <= '9') { *vk = (uint16_t)ch; *mods = 0; return 0; }
    if (ch == ' ') { *vk = 0x20u; *mods = 0; return 0; }
    for (i = 0; i < sizeof pun / sizeof pun[0]; ++i)
        if (pun[i].c == (char)ch) { *vk = pun[i].vk; *mods = pun[i].m;
                                    return 0; }
    return -1;
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
