/* kn_input.h — the NVST control/input plane (round XXXVII; provenance in
 * docs/INPUT.md and docs/RTSP.md §24).
 *
 * WHY: R7 run 17 disproved the round-XXXVI hypothesis. The full receiver
 * feedback set flew for the WHOLE 242 s session (feedback.rr 242, qos
 * 4701, ka 81, ack 331, dropped 0) and the video still died at exactly
 * 10.99 s / 331 AUs @ 30.1 fps — statistically the same as run 16's
 * 10.2 s / 307 AUs with NO feedback at all. The stall cause is therefore
 * NOT receiver feedback. What runs 16 AND 17 both lacked is the one thing
 * the reference sends the instant the control channel opens and
 * INPUT_VERSION arrives: the EIGHT-MESSAGE INPUT ACTIVATION CHAIN
 * (nvst.rs:6554-6591 finish_nvst_input_handshake ->
 * nvst_input.rs:1318-1329 activation_chain). The reference's own comment
 * on the window-state member is explicit: "Advertising the previous
 * all-zero window state LEAVES THE SESSION LOOKING INACTIVE and prevents
 * the server from sending its system-cursor mode updates."
 *
 * WHAT IT IS: pure wire-format builders + parsers for that plane, plus
 * the cursor-capture handoff state machine. No sockets, no threads, no
 * allocation — identical in every build, golden-tested against
 * tools/input_reference.py, which self-proves against the reference
 * client's OWN byte-exact Rust goldens (nvst_input.rs:1768-1800) before
 * emitting kn_input_kat_vectors.h.
 *
 * WIRE SHAPE: every control message is a TLV record — command code
 * (LE u16), payload length (LE u16), payload — and a single SCTP message
 * may carry a CONCATENATION of such records (nvst_input.rs:1385-1391 and
 * the parser at :404-424 both assume the list form). kn_input_scan() is
 * that list walker.
 *
 * SECURITY/§7: no key material and no user input flows through here in
 * this round (the chain is a fixed, device-less activation); logs carry
 * counters, command names and lengths only.
 */
#ifndef KN_INPUT_H
#define KN_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- command codes
 * nvst_input.rs:25-42 plus the control-plane codes this client sends
 * from kn_rx (nvst_control.rs:3-8) — one table so the evidence lines can
 * NAME every code seen on the wire. */
enum {
    KN_INPUT_CMD_SYSTEM_CURSOR   = 0x010f,
    KN_INPUT_CMD_BITMAP_CURSOR   = 0x0110,
    KN_INPUT_CMD_KEEPALIVE       = 0x0200,
    KN_INPUT_CMD_FRAME_PACING    = 0x0203,
    KN_INPUT_CMD_FRAME_ACK       = 0x0204,
    KN_INPUT_CMD_QOS_REPORT      = 0x0207,
    KN_INPUT_CMD_REMOTE_INPUT    = 0x0206,
    KN_INPUT_CMD_ENABLE_INPUT    = 0x020b,
    KN_INPUT_CMD_GAMEPAD         = 0x020d,
    KN_INPUT_CMD_INPUT_VERSION   = 0x020e,
    KN_INPUT_CMD_FRAME_INVALID   = 0x0301,
    KN_INPUT_CMD_IDR_REQUEST     = 0x0302,
    KN_INPUT_CMD_CURSOR_CAPTURE  = 0x0308,
    KN_INPUT_CMD_CURSOR_TRACKING = 0x030d,
    KN_INPUT_CMD_NACK_V2         = 0x0317,
    KN_INPUT_CMD_WINDOW_STATE    = 0x0320,
    KN_INPUT_CMD_SYSTEM_STATE    = 0x0321
};

/* ------------------------------------------------------------- limits */
#define KN_INPUT_TLV_HDR         4u   /* code + length */
#define KN_INPUT_CMD_MAX        64u   /* longest chain message is 52 B */
#define KN_INPUT_ACTIVATION_N    8u   /* nvst_input.rs:1318-1329 */
#define KN_INPUT_WINDOW_STATE_ACTIVE 19u  /* nvst_input.rs:1376-1381 */

/* -------------------------------------------------------- cadences
 * nvst_input.rs:11-12, nvst_cursor.rs:3-5. */
#define KN_INPUT_KEEPALIVE_INTERVAL_MS   3000u
#define KN_INPUT_HANDSHAKE_TIMEOUT_MS    5000u
#define KN_INPUT_CURSOR_RETRY_MS          250u
#define KN_INPUT_CURSOR_MAX_ATTEMPTS        8u
#define KN_INPUT_CURSOR_SILENT_HOST_MS   3000u

/* The window-state value the desktop client announces at frame zero. */

/* --------------------------------------------------------- builders
 * All builders return the TOTAL encoded length (header + payload), or a
 * negative value when the output buffer is too small / an argument is
 * invalid. They never write past out_len. */

/* TLV framing (nvst_input.rs:1385-1391). */
int kn_input_command(uint16_t code, const uint8_t *payload, size_t plen,
                     uint8_t *out, size_t out_len);

/* 0x0200 keepalive — 8 bytes total; the reference always sends
 * stream_value 0 (nvst.rs:7752). Golden: 00 02 04 00 00 00 00 00. */
int kn_input_keepalive(uint32_t stream_value, uint8_t *out, size_t out_len);

/* 0x020b enable-input — {stream index, counter, enabled} as 3x LE u32
 * (nvst_input.rs:1350-1357). */
int kn_input_enable_input(uint32_t counter, int enabled,
                          uint8_t *out, size_t out_len);

/* 0x020d device descriptor — 0x23, timestamp_us as BE u64, then the
 * 39-byte body template with body[9..11] = bitmap LE
 * (nvst_input.rs:1358-1371). 52 bytes total. */
int kn_input_device_descriptor(uint64_t timestamp_us, uint16_t bitmap,
                               uint8_t *out, size_t out_len);

/* 0x0308 / 0x030d — single-byte boolean payloads
 * (nvst_input.rs:1331-1337). 5 bytes total each. */
int kn_input_cursor_capture(int enabled, uint8_t *out, size_t out_len);
int kn_input_cursor_tracking(int enabled, uint8_t *out, size_t out_len);

/* 0x0320 / 0x0321 — {stream index, state, frame} as 3x LE u32
 * (nvst_input.rs:1372-1383). 16 bytes total. */
int kn_input_state_change(uint16_t code, uint32_t state, uint32_t frame,
                          uint8_t *out, size_t out_len);

/* 0x0206 remote-input carrying INPUT_HAPTICS_ENABLED(13) — the nested
 * remote_input_packet/remote_input_message derivation, timestamp LE u64
 * (nvst_input.rs:956-995, :1339-1344). 44 bytes total. */
int kn_input_haptics(uint64_t timestamp_us, int enabled,
                     uint8_t *out, size_t out_len);

/* 0x0206 remote-input carrying INPUT_MOUSE_RELATIVE(7) — round XLI.1,
 * the first discrete-input message (docs/INPUT.md §9). Body is dx BE
 * i16, dy BE i16, two zero bytes (nvst_input.rs:727-741); 7 is not
 * KEY_DOWN/KEY_UP/MOUSE_ABSOLUTE so timestamp_count = 1 (:956-987).
 * 44 bytes total; Rust golden at nvst_input.rs:1888-1911. Route:
 * ControlPartial (nvst_input.rs:969) = sid 6 — UNORDERED, PR-SCTP
 * TTL 300 ms (NVST_CHANNEL_PROFILE[3]); the caller must NOT send it
 * on the reliable sid-0 control channel. */
int kn_input_mouse_relative(int16_t dx, int16_t dy, uint64_t timestamp_us,
                            uint8_t *out, size_t out_len);

/* ------------------------------------------------- discrete input (F19)
 * Round L: the keyboard/mouse plane the login wall made load-bearing
 * (run 31: Watch Dogs: Legion never got past Ubisoft Connect in four
 * draws because the rig's launcher asks for credentials and this client
 * had no way to type them — only the +/-1 px jiggle ever flew).
 *
 * The wire `key` field is a WINDOWS VIRTUAL-KEY CODE, big-endian u16.
 * Oracle: OpenNOW docs/keyboard-mapping.md ("Wire VK" column: 0xbd,
 * 0xbb, 0xdb, 0xc0, 0xbc, 0xbe, 0xbf, 0xe2 = the VK_OEM_* family) and
 * CapturedInput::Key { virtual_key: u16, modifiers: u16 }
 * (opennow-streamer-platform/src/media.rs:617-621). Modifiers are the
 * bitmask of ShortcutChord::parse (media.rs:292-319). Button ids are the
 * Raw-Input order (windows_raw_input.rs:326-332).
 *
 * Every builder below returns the TOTAL TLV length or a negative value
 * (-1 bad argument, -2 output buffer too small), and every message rides
 * the 0x0206 remote-input envelope: key/absolute are 52 bytes with TWO
 * LE-u64 timestamp copies, button/wheel/relative are 44 bytes with one.
 * Use kn_input_route_sid() for the channel — motion rides ControlPartial
 * (sid 6, unordered, PR-SCTP TTL 300 ms), everything else
 * ControlReliable (sid 0, ordered). Both halves are pinned by
 * tools/input_reference.py against the reference's OWN Rust goldens
 * (nvst_input.rs:1868-1911). */
enum {
    KN_INPUT_TYPE_HEARTBEAT      = 2,
    KN_INPUT_TYPE_KEY_DOWN       = 3,
    KN_INPUT_TYPE_KEY_UP         = 4,
    KN_INPUT_TYPE_MOUSE_ABSOLUTE = 5,
    KN_INPUT_TYPE_MOUSE_RELATIVE = 7,
    KN_INPUT_TYPE_BUTTON_DOWN    = 8,
    KN_INPUT_TYPE_BUTTON_UP      = 9,
    KN_INPUT_TYPE_WHEEL          = 10,
    KN_INPUT_TYPE_GAMEPAD        = 12,
    KN_INPUT_TYPE_HAPTICS        = 13,
    KN_INPUT_TYPE_LOCK_KEYS      = 19,
    KN_INPUT_TYPE_TEXT           = 23
};

/* Modifier bits (media.rs:292-319). */
#define KN_INPUT_MOD_SHIFT 0x01u
#define KN_INPUT_MOD_CTRL  0x02u
#define KN_INPUT_MOD_ALT   0x04u
#define KN_INPUT_MOD_META  0x08u

/* Mouse button ids (windows_raw_input.rs:326-332). */
#define KN_INPUT_BTN_LEFT    1u
#define KN_INPUT_BTN_MIDDLE  2u
#define KN_INPUT_BTN_RIGHT   3u
#define KN_INPUT_BTN_X1      4u
#define KN_INPUT_BTN_X2      5u

/* The reference's anti-AFK pulse is VK 0x7C (F13) down+up with no
 * modifiers — opennow-streamer-core/src/lib.rs:1604-1648
 * (anti_afk_pulse). F13 is bound to nothing in any normal layout, which
 * is exactly why the reference chose it: it keeps the session looking
 * active without touching the game. */
#define KN_INPUT_VK_ANTI_AFK 0x7Cu

/* 0x0206 KEY_DOWN(3)/KEY_UP(4): vk BE u16 | modifiers BE u16 | 10 zero
 * bytes -> 52 B total, ControlReliable. Rust golden: vk 0x41 mods 1
 * ts 0x015b15b1 (nvst_input.rs:1868-1891). */
int kn_input_key(uint16_t vk, uint16_t modifiers, int pressed,
                 uint64_t timestamp_us, uint8_t *out, size_t out_len);

/* 0x0206 BUTTON_DOWN(8)/BUTTON_UP(9): [button, 0] -> 44 B,
 * ControlReliable. `button` must be 1..5. */
int kn_input_mouse_button(unsigned button, int pressed,
                          uint64_t timestamp_us,
                          uint8_t *out, size_t out_len);

/* 0x0206 MOUSE_ABSOLUTE(5): x BE u16 | y BE u16 | 08 00 | w BE u16 |
 * h BE u16 -> 52 B, ControlPartial. Coordinates are PIXELS inside the
 * w x h viewport; w and h must be non-zero (the reference FFI rejects a
 * zero viewport, opennow-streamer-ffi/src/lib.rs:1088-1108). */
int kn_input_mouse_absolute(uint16_t x, uint16_t y, uint16_t width,
                            uint16_t height, uint64_t timestamp_us,
                            uint8_t *out, size_t out_len);

/* 0x0206 WHEEL(10): dx BE i16 | dy BE i16 | 00 00 -> 44 B,
 * ControlReliable (the reference's mouse-input doc: "Buttons and wheel
 * use reliable control" — only MOTION rides sid 6). Windows has no
 * RI_MOUSE_HWHEEL, so dx is normally 0 and one notch is +/-120. */
int kn_input_wheel(int16_t delta_x, int16_t delta_y,
                   uint64_t timestamp_us, uint8_t *out, size_t out_len);

/* The SCTP stream id a discrete-input type rides: 6 for MOUSE_RELATIVE
 * and MOUSE_ABSOLUTE (ControlPartial), 0 for everything else this
 * project builds (ControlReliable), -1 for a type with no route here
 * (gamepad rides InputPartial, text is not built yet). */
int kn_input_route_sid(uint32_t input_type);

/* Name -> Windows VK, mirroring the reference's shortcut_virtual_key
 * (media.rs:323-357): a single ASCII letter/digit maps to its uppercase
 * code, "f1".."f24" to 0x70 + n, plus the named keys (enter, esc, tab,
 * space, arrows, ...). Case-insensitive. Returns the VK or -1. */
int kn_input_vk_from_name(const char *name);

/* One ASCII character -> (VK, modifier bits) for a US layout, the table
 * the `type` command of the input script uses. Letters and digits need
 * no modifiers; the shifted US punctuation sets KN_INPUT_MOD_SHIFT.
 * Returns 0 and fills the vk/mods outputs, or -1 for a character the
 * table does not cover (non-ASCII, e.g. Polish diacritics — the caller
 * must count and report those rather than silently dropping them). */
int kn_input_vk_from_ascii(int ch, uint16_t *vk, uint16_t *mods);

/* The whole chain, in send order (nvst_input.rs:1318-1329). Fills
 * lens[0..7]; returns the number of messages built (8) or -1. Every
 * message is <= KN_INPUT_CMD_MAX bytes. */
int kn_input_activation_chain(uint64_t timestamp_us,
                              uint8_t out[KN_INPUT_ACTIVATION_N][KN_INPUT_CMD_MAX],
                              size_t lens[KN_INPUT_ACTIVATION_N]);

/* ---------------------------------------------------------- parsers */

/* One TLV record as walked by kn_input_scan. */
typedef void (*kn_input_cmd_fn)(void *ctx, uint16_t code,
                                const uint8_t *payload, size_t plen);

/* Walks the concatenated-record list (nvst_input.rs:404-424). A record
 * whose declared length overruns the buffer ends the walk (the reference
 * `break`s). Returns the number of complete records delivered. */
size_t kn_input_scan(const uint8_t *bytes, size_t len,
                     kn_input_cmd_fn cb, void *ctx);

/* input_protocol_version() with the reference's EXACT semantics
 * including both fallbacks (nvst_input.rs:404-441):
 *   >= 0 : the negotiated version
 *     -1 : None (too short, or no version record and data[0] != 0x0e)
 * Run 17's live bytes 0e 02 02 00 03 00 -> 3. */
int kn_input_protocol_version(const uint8_t *bytes, size_t len);

/* Nonzero when the record list carries a server cursor notification
 * (0x010f system / 0x0110 bitmap) — the trigger for
 * kn_input_cursor_notify (nvst.rs:8322-8326). */
int kn_input_has_cursor_notification(const uint8_t *bytes, size_t len);

/* A short stable lowercase name for evidence lines ("input-version",
 * "qos-report", ...); "unknown-0xNNNN" formatting is the caller's job —
 * this returns NULL for codes it does not know. */
const char *kn_input_command_name(uint16_t code);

/* ------------------------------------------------- code histogram
 * Evidence for the "we never answered the rig's 20 control messages"
 * gap: every inbound record's code, counted, serialized as a compact
 * JSON array of [code-hex, count] pairs sorted by first sighting. */
#define KN_INPUT_HIST_SLOTS 32
typedef struct {
    uint16_t code;
    uint32_t count;
} kn_input_hist_slot;
typedef struct {
    kn_input_hist_slot slot[KN_INPUT_HIST_SLOTS];
    size_t   n;
    uint32_t records;      /* every record walked */
    uint32_t overflow;     /* records whose code found no free slot */
} kn_input_hist;

void   kn_input_hist_init(kn_input_hist *h);
void   kn_input_hist_add(kn_input_hist *h, uint16_t code);
/* Writes [["020e",1],["0200",5]] (or [] when empty). Returns the number
 * of characters written (excluding the NUL), or -1 when out_len is too
 * small — the caller must treat a truncated buffer as a hard failure, so
 * this never emits malformed JSONL. */
int    kn_input_hist_json(const kn_input_hist *h, char *out, size_t out_len);

/* ------------------------------------------- cursor-capture handoff
 * nvst_cursor.rs in full: after the activation chain turns server-side
 * cursor composition ON, the client waits for the rig's first cursor
 * notification (or 3 s of silence), retries the enable every 250 ms up
 * to 8 attempts, then sends capture(false) and renders the cursor
 * locally. The three reference tests (nvst_cursor.rs:165-250) pin the
 * exact command sequences; test_kninput replays all three. */
typedef enum {
    KN_INPUT_CURSOR_INACTIVE = 0,
    KN_INPUT_CURSOR_WAITING,
    KN_INPUT_CURSOR_DISABLING,
    KN_INPUT_CURSOR_LOCAL,
    KN_INPUT_CURSOR_EXHAUSTED
} kn_input_cursor_state;

typedef enum {
    KN_INPUT_CURSOR_CMD_CAPTURE_ON = 0,
    KN_INPUT_CURSOR_CMD_CAPTURE_OFF = 1,
    KN_INPUT_CURSOR_CMD_TRACKING = 2
} kn_input_cursor_cmd;

typedef struct {
    kn_input_cursor_state state;
    uint64_t    deadline_ms;
    uint64_t    retry_at_ms;
    unsigned    attempts;
    const char *reason;       /* "cursor-notification" | "silent-host-timeout" */
    /* evidence counters (never affect the state machine) */
    unsigned    enables, trackings, disables, handoffs;
} kn_input_cursor;

/* Returns nonzero when the command was queued for sending (the reference
 * treats a false as "retry later at the same pace"). */
typedef int (*kn_input_cursor_send)(void *ctx, kn_input_cursor_cmd cmd);

void        kn_input_cursor_init(kn_input_cursor *c);
void        kn_input_cursor_activate(kn_input_cursor *c, uint64_t now_ms);
void        kn_input_cursor_reset(kn_input_cursor *c);
void        kn_input_cursor_notify(kn_input_cursor *c, uint64_t now_ms);
/* Returns 1 exactly when the disable succeeded and the client handed off
 * to local cursor composition (state -> LOCAL). */
int         kn_input_cursor_update(kn_input_cursor *c, uint64_t now_ms,
                                   kn_input_cursor_send send, void *ctx);
const char *kn_input_cursor_state_name(kn_input_cursor_state s);

#ifdef __cplusplus
}
#endif

#endif /* KN_INPUT_H */
