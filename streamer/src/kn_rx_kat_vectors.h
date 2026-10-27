/* kn_rx_kat_vectors.h -- GENERATED FILE, do not edit by hand.
 * Source: tools/rx_reference.py (independent Python port of
 * nvst.rs / nvst_control.rs / nvst_invalidation.rs @ a49b124).
 * Regenerate: python3 tools/rx_reference.py   (self-proves first).
 */
#ifndef KN_RX_KAT_VECTORS_H
#define KN_RX_KAT_VECTORS_H

#include <stdint.h>
#include <stddef.h>

/* step kinds */
enum { KN_RX_KAT_DATA = 0, KN_RX_KAT_PARITY, KN_RX_KAT_POLL,
       KN_RX_KAT_LIFECYCLE, KN_RX_KAT_NACK_TAKE, KN_RX_KAT_KF_TAKE,
       KN_RX_KAT_RFI_TAKE, KN_RX_KAT_RAW };
/* lifecycle ops */
enum { KN_RX_KAT_PAUSE = 0, KN_RX_KAT_RESUME, KN_RX_KAT_RECOVER,
       KN_RX_KAT_STOP };

typedef struct {
    int      kind;
    uint64_t now_ms;
    int      deliver;
    uint64_t index;
    uint32_t seq, ts, ssrc;
    unsigned pt, marker;
    unsigned flags, spi, fi, cur, last, pct, fidx, fsrc;
    int      gs_wire;           /* -1 = no GS frame header */
    unsigned nal_type;
    uint32_t body_seed, body_len;
    int      aud_tail;
    unsigned d_shards, p_idx;
    int      n_refs;
    const int *refs;            /* data-step ids feeding the fold */
    uint32_t shard_len;
    int      op;                /* lifecycle op / nack rtt ms */
    int      blob_off;          /* RAW: byte range in k_rx_blob */
    uint32_t blob_len;
    int      via_datagram;      /* RAW: feed process_datagram */
} kn_rx_kat_step;

typedef struct { int type, reason, detail; uint64_t a, b; } kn_rx_kat_ev;
typedef struct {
    uint32_t frame_index, len;
    uint64_t hash;              /* first 8 bytes of SHA-1, BE */
    int keyframe, contiguous, wire, known;
    uint32_t first_spi, ts;
} kn_rx_kat_frame;
typedef struct {
    int n_ev, ev_off, n_frames, frames_off;
    int nack_have; uint64_t nack_first, nack_last;
    int kf_expect;              /* -1 = not a kf_take step */
    int rfi_have; uint64_t rfi_first, rfi_last;
    int state_expect;           /* -1 = don't check */
} kn_rx_kat_step_exp;
typedef struct {
    const char *name;
    int codec;
    uint32_t window, au_cap, vps, timeout_ms, startup_ms, stall_ms,
             grace_ms;
    int have_pt; unsigned pt;
    int have_ssrc; uint32_t ssrc;
    uint32_t fec_max_blocks, fec_max_shards, nack_capacity;
    int srtp_profile;           /* 0 none,1 cm128,2 cm256,3 gcm128,4 gcm256 */
    int srtp_tag_len;
    int master_off, master_len, salt_off, salt_len;
    int enc_off, enc_len, auth_off, auth_len, dsalt_off, dsalt_len;
    int steps_off, n_steps;
    uint64_t c_auth, c_fec_pkts, c_fec_rep, c_frames, c_stale, c_gaps,
             c_withheld, c_kf, c_retrans, c_replay;
} kn_rx_kat_scenario;

static const int k_step_refs0[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs1[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs2[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs3[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs4[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs5[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs6[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs7[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs8[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs9[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs10[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs11[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs12[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs13[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs14[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs15[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs16[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs17[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs18[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs19[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs20[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs21[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs22[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs23[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs24[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs25[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs26[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs27[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs28[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs29[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs30[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs31[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs32[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs33[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs34[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs35[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs36[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs37[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs38[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs39[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs40[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs41[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs42[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs43[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs44[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs45[12] = { 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs46[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs47[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs48[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs49[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs50[12] = { 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs51[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs52[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs53[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs54[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs55[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs56[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs57[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs58[12] = { 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs59[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs60[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs61[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs62[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs63[12] = { 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs64[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs65[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs66[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs67[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs68[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs69[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs70[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs71[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs72[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs73[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs74[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs75[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs76[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs77[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs78[12] = { 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs79[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs80[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs81[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs82[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs83[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs84[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs85[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs86[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs87[12] = { 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs88[12] = { 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs89[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs90[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs91[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs92[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs93[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs94[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs95[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs96[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs97[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs98[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs99[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs100[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs101[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs102[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs103[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs104[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs105[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs106[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs107[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs108[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs109[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs110[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs111[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs112[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs113[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs114[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs115[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs116[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs117[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs118[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs119[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs120[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs121[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs122[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs123[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs124[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs125[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs126[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs127[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs128[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs129[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs130[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs131[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs132[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs133[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs134[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs135[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs136[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs137[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs138[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs139[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs140[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs141[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs142[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs143[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs144[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs145[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs146[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs147[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs148[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs149[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs150[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs151[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs152[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs153[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs154[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs155[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs156[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs157[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs158[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs159[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs160[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs161[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs162[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int k_step_refs163[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static const kn_rx_kat_step k_rx_steps[] = {
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 100u, 12u, 0, 0u, 0u, 0, k_step_refs0, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 5u, 101u, 10u, 0, 0u, 0u, 0, k_step_refs1, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 3u, 3u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 5u, 102u, 14u, 0, 0u, 0u, 0, k_step_refs2, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16915060u, 287454020u, 96u, 0u, 5u, 4u, 2u, 0u, 0u, 0u, 0u, 0u, 1, 1u, 200u, 12u, 0, 0u, 0u, 0, k_step_refs3, 0u, 0, -1, 0u, 0 },
    { 0, 1004ULL, 1, 5ULL, 5u, 16915060u, 287454020u, 96u, 0u, 1u, 5u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 201u, 10u, 0, 0u, 0u, 0, k_step_refs4, 0u, 0, -1, 0u, 0 },
    { 0, 1005ULL, 1, 6ULL, 6u, 16915060u, 287454020u, 96u, 0u, 3u, 6u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 202u, 14u, 0, 0u, 0u, 0, k_step_refs5, 0u, 0, -1, 0u, 0 },
    { 0, 1006ULL, 1, 7ULL, 7u, 16918060u, 287454020u, 96u, 0u, 5u, 7u, 3u, 0u, 0u, 0u, 0u, 0u, 1, 1u, 300u, 20u, 0, 0u, 0u, 0, k_step_refs6, 0u, 0, -1, 0u, 0 },
    { 0, 1007ULL, 1, 8ULL, 8u, 16918060u, 287454020u, 96u, 0u, 3u, 8u, 3u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 301u, 8u, 0, 0u, 0u, 0, k_step_refs7, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 100u, 12u, 0, 0u, 0u, 0, k_step_refs8, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 5u, 101u, 10u, 0, 0u, 0u, 0, k_step_refs9, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 3u, 3u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 5u, 102u, 14u, 1, 0u, 0u, 0, k_step_refs10, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16915060u, 287454020u, 96u, 0u, 5u, 4u, 2u, 0u, 0u, 0u, 0u, 0u, 1, 1u, 200u, 12u, 0, 0u, 0u, 0, k_step_refs11, 0u, 0, -1, 0u, 0 },
    { 0, 1004ULL, 1, 5ULL, 5u, 16915060u, 287454020u, 96u, 0u, 1u, 5u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 201u, 10u, 0, 0u, 0u, 0, k_step_refs12, 0u, 0, -1, 0u, 0 },
    { 0, 1005ULL, 1, 6ULL, 6u, 16915060u, 287454020u, 96u, 0u, 3u, 6u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 202u, 14u, 1, 0u, 0u, 0, k_step_refs13, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 100u, 12u, 0, 0u, 0u, 0, k_step_refs14, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 3u, 3u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 102u, 14u, 0, 0u, 0u, 0, k_step_refs15, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 101u, 10u, 0, 0u, 0u, 0, k_step_refs16, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16915060u, 287454020u, 96u, 0u, 5u, 4u, 2u, 0u, 0u, 0u, 0u, 0u, 1, 5u, 200u, 11u, 0, 0u, 0u, 0, k_step_refs17, 0u, 0, -1, 0u, 0 },
    { 0, 1004ULL, 1, 6ULL, 6u, 16915060u, 287454020u, 96u, 0u, 3u, 6u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 202u, 9u, 0, 0u, 0u, 0, k_step_refs18, 0u, 0, -1, 0u, 0 },
    { 0, 1005ULL, 1, 5ULL, 5u, 16915060u, 287454020u, 96u, 0u, 1u, 5u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 201u, 13u, 0, 0u, 0u, 0, k_step_refs19, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 100u, 12u, 0, 0u, 0u, 0, k_step_refs20, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 101u, 10u, 0, 0u, 0u, 0, k_step_refs21, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 3u, 2u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 101u, 10u, 0, 0u, 0u, 0, k_step_refs22, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 3ULL, 3u, 16915060u, 287454020u, 96u, 0u, 5u, 3u, 2u, 0u, 0u, 0u, 0u, 0u, 1, 5u, 200u, 12u, 0, 0u, 0u, 0, k_step_refs23, 0u, 0, -1, 0u, 0 },
    { 0, 1004ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 100u, 12u, 0, 0u, 0u, 0, k_step_refs24, 0u, 0, -1, 0u, 0 },
    { 0, 1005ULL, 1, 4ULL, 4u, 16915060u, 287454020u, 96u, 0u, 3u, 4u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 201u, 10u, 0, 0u, 0u, 0, k_step_refs25, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 100u, 12u, 0, 0u, 0u, 0, k_step_refs26, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 3u, 3u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 102u, 10u, 0, 0u, 0u, 0, k_step_refs27, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 3u, 3u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 102u, 10u, 0, 0u, 0u, 0, k_step_refs28, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 101u, 8u, 0, 0u, 0u, 0, k_step_refs29, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 100u, 12u, 0, 0u, 0u, 0, k_step_refs30, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 6ULL, 6u, 16912060u, 287454020u, 96u, 0u, 3u, 6u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 101u, 10u, 0, 0u, 0u, 0, k_step_refs31, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 7ULL, 7u, 16915060u, 287454020u, 96u, 0u, 7u, 7u, 2u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 200u, 16u, 0, 0u, 0u, 0, k_step_refs32, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 100u, 12u, 0, 0u, 0u, 0, k_step_refs33, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 3u, 3u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 102u, 10u, 0, 0u, 0u, 0, k_step_refs34, 0u, 0, -1, 0u, 0 },
    { 4, 1003ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs35, 0u, 0, -1, 0u, 0 },
    { 0, 1020ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 101u, 8u, 0, 0u, 0u, 0, k_step_refs36, 0u, 0, -1, 0u, 0 },
    { 7, 1000ULL, 1, 1ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs37, 0u, 0, 0, 12u, 0 },
    { 7, 1001ULL, 1, 3ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs38, 0u, 0, 12, 12u, 0 },
    { 7, 1052ULL, 1, 4ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs39, 0u, 0, 24, 12u, 0 },
    { 7, 1053ULL, 1, 5ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs40, 0u, 0, 36, 12u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 25u, 0u, 4u, 2, 5u, 500u, 232u, 0, 0u, 0u, 0, k_step_refs41, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 25u, 1u, 4u, -1, 1u, 501u, 236u, 0, 0u, 0u, 0, k_step_refs42, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 0, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 1u, 3u, 1u, 0u, 0u, 25u, 2u, 4u, -1, 1u, 502u, 236u, 0, 0u, 0u, 0, k_step_refs43, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16912060u, 287454020u, 96u, 0u, 3u, 4u, 1u, 0u, 0u, 25u, 3u, 4u, -1, 1u, 503u, 236u, 0, 0u, 0u, 0, k_step_refs44, 0u, 0, -1, 0u, 0 },
    { 1, 1004ULL, 1, 5ULL, 5u, 16912060u, 287454020u, 96u, 0u, 0u, 0u, 1u, 0u, 0u, 25u, 0u, 0u, -1, 0u, 0u, 0u, 0, 4u, 0u, 4, k_step_refs45, 272u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 25u, 0u, 4u, 2, 5u, 500u, 232u, 0, 0u, 0u, 0, k_step_refs46, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 25u, 1u, 4u, -1, 1u, 501u, 236u, 0, 0u, 0u, 0, k_step_refs47, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 1u, 3u, 1u, 0u, 0u, 25u, 2u, 4u, -1, 1u, 502u, 236u, 0, 0u, 0u, 0, k_step_refs48, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16912060u, 287454020u, 96u, 0u, 3u, 4u, 1u, 0u, 0u, 25u, 3u, 4u, -1, 1u, 503u, 236u, 0, 0u, 0u, 0, k_step_refs49, 0u, 0, -1, 0u, 0 },
    { 1, 1004ULL, 1, 5ULL, 5u, 16912060u, 287454020u, 96u, 0u, 0u, 0u, 1u, 0u, 0u, 25u, 0u, 0u, -1, 0u, 0u, 0u, 0, 4u, 0u, 4, k_step_refs50, 272u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 0, 100ULL, 100u, 16930060u, 287454020u, 96u, 0u, 7u, 100u, 7u, 0u, 0u, 200u, 0u, 1u, 2, 5u, 700u, 40u, 0, 0u, 0u, 0, k_step_refs51, 0u, 0, -1, 0u, 0 },
    { 1, 1001ULL, 1, 101ULL, 101u, 16930060u, 287454020u, 96u, 0u, 0u, 0u, 7u, 0u, 0u, 200u, 0u, 0u, -1, 0u, 0u, 0u, 0, 1u, 0u, 1, k_step_refs52, 272u, 0, -1, 0u, 0 },
    { 1, 1002ULL, 1, 102ULL, 102u, 16930060u, 287454020u, 96u, 0u, 0u, 0u, 7u, 0u, 0u, 200u, 0u, 0u, -1, 0u, 0u, 0u, 0, 1u, 1u, 1, k_step_refs53, 272u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 25u, 0u, 4u, 2, 5u, 800u, 232u, 0, 0u, 0u, 0, k_step_refs54, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 0, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 25u, 1u, 4u, -1, 1u, 801u, 236u, 0, 0u, 0u, 0, k_step_refs55, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 0, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 1u, 3u, 1u, 0u, 0u, 25u, 2u, 4u, -1, 1u, 802u, 236u, 0, 0u, 0u, 0, k_step_refs56, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16912060u, 287454020u, 96u, 0u, 3u, 4u, 1u, 0u, 0u, 25u, 3u, 4u, -1, 1u, 803u, 236u, 0, 0u, 0u, 0, k_step_refs57, 0u, 0, -1, 0u, 0 },
    { 1, 1004ULL, 1, 5ULL, 5u, 16912060u, 287454020u, 96u, 0u, 0u, 0u, 1u, 0u, 0u, 25u, 0u, 0u, -1, 0u, 0u, 0u, 0, 4u, 0u, 4, k_step_refs58, 272u, 0, -1, 0u, 0 },
    { 0, 1005ULL, 1, 6ULL, 6u, 16915060u, 287454020u, 96u, 0u, 5u, 6u, 2u, 0u, 0u, 25u, 0u, 4u, 2, 5u, 850u, 232u, 0, 0u, 0u, 0, k_step_refs59, 0u, 0, -1, 0u, 0 },
    { 0, 1006ULL, 1, 7ULL, 7u, 16915060u, 287454020u, 96u, 0u, 1u, 7u, 2u, 0u, 0u, 25u, 1u, 4u, -1, 1u, 851u, 236u, 0, 0u, 0u, 0, k_step_refs60, 0u, 0, -1, 0u, 0 },
    { 0, 1007ULL, 1, 8ULL, 8u, 16915060u, 287454020u, 96u, 0u, 1u, 8u, 2u, 0u, 0u, 25u, 2u, 4u, -1, 1u, 852u, 236u, 0, 0u, 0u, 0, k_step_refs61, 0u, 0, -1, 0u, 0 },
    { 0, 1008ULL, 1, 9ULL, 9u, 16915060u, 287454020u, 96u, 0u, 3u, 9u, 2u, 0u, 0u, 25u, 3u, 4u, -1, 1u, 853u, 236u, 0, 0u, 0u, 0, k_step_refs62, 0u, 0, -1, 0u, 0 },
    { 1, 1009ULL, 1, 10ULL, 10u, 16915060u, 287454020u, 96u, 0u, 0u, 0u, 2u, 0u, 0u, 25u, 0u, 0u, -1, 0u, 0u, 0u, 0, 4u, 0u, 4, k_step_refs63, 272u, 0, -1, 0u, 0 },
    { 2, 1100ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs64, 0u, 0, -1, 0u, 0 },
    { 0, 1010ULL, 1, 11ULL, 11u, 16918060u, 287454020u, 96u, 0u, 5u, 11u, 3u, 0u, 0u, 25u, 0u, 4u, 2, 5u, 900u, 232u, 0, 0u, 0u, 0, k_step_refs65, 0u, 0, -1, 0u, 0 },
    { 0, 1011ULL, 1, 12ULL, 12u, 16918060u, 287454020u, 96u, 0u, 1u, 12u, 3u, 0u, 0u, 25u, 1u, 4u, -1, 1u, 901u, 236u, 0, 0u, 0u, 0, k_step_refs66, 0u, 0, -1, 0u, 0 },
    { 0, 1012ULL, 1, 13ULL, 13u, 16918060u, 287454020u, 96u, 0u, 1u, 13u, 3u, 0u, 0u, 25u, 2u, 4u, -1, 1u, 902u, 236u, 0, 0u, 0u, 0, k_step_refs67, 0u, 0, -1, 0u, 0 },
    { 0, 1013ULL, 1, 14ULL, 14u, 16918060u, 287454020u, 96u, 0u, 3u, 14u, 3u, 0u, 0u, 25u, 3u, 4u, -1, 1u, 903u, 236u, 0, 0u, 0u, 0, k_step_refs68, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 25u, 0u, 4u, 2, 5u, 950u, 40u, 0, 0u, 0u, 0, k_step_refs69, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 25u, 1u, 4u, -1, 1u, 951u, 44u, 0, 0u, 0u, 0, k_step_refs70, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 25u, 1u, 4u, -1, 1u, 951u, 44u, 0, 0u, 0u, 0, k_step_refs71, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 1u, 3u, 1u, 0u, 0u, 25u, 2u, 4u, -1, 1u, 952u, 44u, 0, 0u, 0u, 0, k_step_refs72, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16912060u, 287454020u, 96u, 0u, 3u, 4u, 1u, 0u, 0u, 25u, 3u, 4u, -1, 1u, 953u, 44u, 0, 0u, 0u, 0, k_step_refs73, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 25u, 0u, 4u, 2, 5u, 970u, 40u, 0, 0u, 0u, 0, k_step_refs74, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 17206060u, 287454020u, 96u, 0u, 1u, 2u, 99u, 0u, 0u, 25u, 1u, 4u, -1, 1u, 971u, 44u, 0, 0u, 0u, 0, k_step_refs75, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 1u, 3u, 1u, 0u, 0u, 25u, 2u, 4u, -1, 1u, 972u, 44u, 0, 0u, 0u, 0, k_step_refs76, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16912060u, 287454020u, 96u, 0u, 3u, 4u, 1u, 0u, 0u, 25u, 3u, 4u, -1, 1u, 973u, 44u, 0, 0u, 0u, 0, k_step_refs77, 0u, 0, -1, 0u, 0 },
    { 1, 1004ULL, 1, 5ULL, 5u, 16912060u, 287454020u, 96u, 0u, 0u, 0u, 1u, 0u, 0u, 25u, 0u, 0u, -1, 0u, 0u, 0u, 0, 4u, 0u, 4, k_step_refs78, 272u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16972060u, 287454020u, 96u, 0u, 5u, 1u, 21u, 0u, 1u, 25u, 0u, 4u, 2, 5u, 1000u, 232u, 0, 0u, 0u, 0, k_step_refs79, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16972060u, 287454020u, 96u, 0u, 1u, 2u, 21u, 0u, 1u, 25u, 1u, 4u, -1, 1u, 1001u, 236u, 0, 0u, 0u, 0, k_step_refs80, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16972060u, 287454020u, 96u, 0u, 1u, 3u, 21u, 0u, 1u, 25u, 2u, 4u, -1, 1u, 1002u, 236u, 0, 0u, 0u, 0, k_step_refs81, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16972060u, 287454020u, 96u, 0u, 1u, 4u, 21u, 0u, 1u, 25u, 3u, 4u, -1, 1u, 1003u, 236u, 0, 0u, 0u, 0, k_step_refs82, 0u, 0, -1, 0u, 0 },
    { 0, 1004ULL, 1, 6ULL, 6u, 16972060u, 287454020u, 96u, 0u, 1u, 5u, 21u, 1u, 1u, 25u, 0u, 4u, -1, 1u, 1010u, 236u, 0, 0u, 0u, 0, k_step_refs83, 0u, 0, -1, 0u, 0 },
    { 0, 1005ULL, 1, 7ULL, 7u, 16972060u, 287454020u, 96u, 0u, 1u, 6u, 21u, 1u, 1u, 25u, 1u, 4u, -1, 1u, 1011u, 236u, 0, 0u, 0u, 0, k_step_refs84, 0u, 0, -1, 0u, 0 },
    { 0, 1006ULL, 0, 8ULL, 8u, 16972060u, 287454020u, 96u, 0u, 1u, 7u, 21u, 1u, 1u, 25u, 2u, 4u, -1, 1u, 1012u, 236u, 0, 0u, 0u, 0, k_step_refs85, 0u, 0, -1, 0u, 0 },
    { 0, 1007ULL, 1, 9ULL, 9u, 16972060u, 287454020u, 96u, 0u, 3u, 8u, 21u, 1u, 1u, 25u, 3u, 4u, -1, 1u, 1013u, 236u, 0, 0u, 0u, 0, k_step_refs86, 0u, 0, -1, 0u, 0 },
    { 1, 1008ULL, 1, 5ULL, 5u, 16972060u, 287454020u, 96u, 0u, 0u, 0u, 21u, 0u, 1u, 25u, 0u, 0u, -1, 0u, 0u, 0u, 0, 4u, 0u, 4, k_step_refs87, 272u, 0, -1, 0u, 0 },
    { 1, 1009ULL, 1, 10ULL, 10u, 16972060u, 287454020u, 96u, 0u, 0u, 0u, 21u, 1u, 1u, 25u, 0u, 0u, -1, 0u, 0u, 0u, 0, 4u, 0u, 4, k_step_refs88, 272u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 5u, 1100u, 12u, 0, 0u, 0u, 0, k_step_refs89, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 2u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 1101u, 10u, 0, 0u, 0u, 0, k_step_refs90, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16915060u, 287454020u, 96u, 0u, 5u, 3u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 5u, 1200u, 12u, 0, 0u, 0u, 0, k_step_refs91, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16915060u, 287454020u, 96u, 0u, 3u, 4u, 2u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 1201u, 10u, 0, 0u, 0u, 0, k_step_refs92, 0u, 0, -1, 0u, 0 },
    { 5, 1004ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs93, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 1300u, 12u, 0, 0u, 0u, 0, k_step_refs94, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16912060u, 287454020u, 96u, 0u, 1u, 12u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 1301u, 10u, 0, 0u, 0u, 0, k_step_refs95, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16912060u, 287454020u, 96u, 0u, 3u, 3u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 1302u, 10u, 0, 0u, 0u, 0, k_step_refs96, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 4ULL, 4u, 16915060u, 287454020u, 96u, 0u, 7u, 4u, 2u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 1400u, 20u, 0, 0u, 0u, 0, k_step_refs97, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 7u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 1500u, 20u, 0, 0u, 0u, 0, k_step_refs98, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 5ULL, 5u, 16918060u, 287454020u, 96u, 0u, 5u, 5u, 3u, 0u, 0u, 0u, 0u, 0u, 1, 5u, 1600u, 12u, 0, 0u, 0u, 0, k_step_refs99, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 6ULL, 6u, 16918060u, 287454020u, 96u, 0u, 3u, 6u, 3u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 1601u, 10u, 0, 0u, 0u, 0, k_step_refs100, 0u, 0, -1, 0u, 0 },
    { 6, 1003ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs101, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 7ULL, 7u, 16921060u, 287454020u, 96u, 0u, 7u, 7u, 4u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 1700u, 20u, 0, 0u, 0u, 0, k_step_refs102, 0u, 0, -1, 0u, 0 },
    { 5, 1004ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs103, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 7u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 1800u, 20u, 0, 0u, 0u, 0, k_step_refs104, 0u, 0, -1, 0u, 0 },
    { 2, 1499ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs105, 0u, 0, -1, 0u, 0 },
    { 2, 2000ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs106, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16915060u, 287454020u, 96u, 0u, 7u, 2u, 2u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 1900u, 16u, 0, 0u, 0u, 0, k_step_refs107, 0u, 0, -1, 0u, 0 },
    { 3, 1002ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs108, 0u, 2, -1, 0u, 0 },
    { 0, 1003ULL, 1, 3ULL, 3u, 16918060u, 287454020u, 96u, 0u, 7u, 3u, 3u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 1950u, 16u, 0, 0u, 0u, 0, k_step_refs109, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 7u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2000u, 20u, 0, 0u, 0u, 0, k_step_refs110, 0u, 0, -1, 0u, 0 },
    { 3, 1001ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs111, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 2ULL, 2u, 16915060u, 287454020u, 96u, 0u, 7u, 2u, 2u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2050u, 16u, 0, 0u, 0u, 0, k_step_refs112, 0u, 0, -1, 0u, 0 },
    { 3, 1003ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs113, 0u, 1, -1, 0u, 0 },
    { 0, 1004ULL, 1, 3ULL, 3u, 16918060u, 287454020u, 96u, 0u, 7u, 3u, 3u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2100u, 16u, 0, 0u, 0u, 0, k_step_refs114, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2200u, 12u, 0, 0u, 0u, 0, k_step_refs115, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 4ULL, 4u, 16912060u, 287454020u, 96u, 0u, 3u, 4u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 2203u, 10u, 0, 0u, 0u, 0, k_step_refs116, 0u, 0, -1, 0u, 0 },
    { 4, 1002ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs117, 0u, 0, -1, 0u, 0 },
    { 4, 1003ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs118, 0u, 0, -1, 0u, 0 },
    { 4, 1008ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs119, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 5u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 5u, 2300u, 12u, 0, 0u, 0u, 0, k_step_refs120, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 9ULL, 9u, 16912060u, 287454020u, 96u, 0u, 3u, 9u, 1u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 2301u, 10u, 0, 0u, 0u, 0, k_step_refs121, 0u, 0, -1, 0u, 0 },
    { 5, 1002ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs122, 0u, 0, -1, 0u, 0 },
    { 5, 1002ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs123, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 10ULL, 10u, 16915060u, 287454020u, 96u, 0u, 7u, 10u, 2u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2400u, 16u, 0, 0u, 0u, 0, k_step_refs124, 0u, 0, -1, 0u, 0 },
    { 5, 1003ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs125, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 16777214ULL, 65534u, 17002060u, 287454020u, 96u, 0u, 5u, 16777214u, 31u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2500u, 12u, 0, 0u, 0u, 0, k_step_refs126, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 16777215ULL, 65535u, 17002060u, 287454020u, 96u, 0u, 1u, 16777215u, 31u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 2501u, 10u, 0, 0u, 0u, 0, k_step_refs127, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 16777216ULL, 0u, 17002060u, 287454020u, 96u, 0u, 3u, 0u, 31u, 0u, 0u, 0u, 0u, 0u, -1, 1u, 2502u, 10u, 0, 0u, 0u, 0, k_step_refs128, 0u, 0, -1, 0u, 0 },
    { 0, 1003ULL, 1, 16777217ULL, 1u, 17005060u, 287454020u, 96u, 0u, 7u, 1u, 32u, 0u, 0u, 0u, 0u, 0u, 1, 5u, 2600u, 16u, 0, 0u, 0u, 0, k_step_refs129, 0u, 0, -1, 0u, 0 },
    { 7, 1000ULL, 1, 1ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs130, 0u, 0, 48, 12u, 0 },
    { 7, 1001ULL, 1, 9ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs131, 0u, 0, 60, 12u, 0 },
    { 7, 1001ULL, 1, 1ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs132, 0u, 0, 72, 12u, 0 },
    { 7, 1003ULL, 1, 3ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs133, 0u, 0, 84, 12u, 0 },
    { 7, 1005ULL, 1, 5ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs134, 0u, 0, 96, 12u, 0 },
    { 7, 1006ULL, 1, 6ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs135, 0u, 0, 108, 12u, 0 },
    { 7, 1007ULL, 1, 7ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs136, 0u, 0, 120, 12u, 0 },
    { 7, 1000ULL, 1, 1ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs137, 0u, 0, 132, 12u, 0 },
    { 7, 1001ULL, 1, 2ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs138, 0u, 0, 144, 12u, 0 },
    { 7, 1002ULL, 1, 3ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs139, 0u, 0, 156, 4u, 0 },
    { 7, 1003ULL, 1, 4ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs140, 0u, 0, 160, 15u, 0 },
    { 0, 1000ULL, 1, 50ULL, 50u, 16936060u, 287454020u, 96u, 0u, 7u, 50u, 9u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2700u, 16u, 0, 0u, 0u, 0, k_step_refs141, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 3735928559u, 96u, 0u, 7u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2800u, 16u, 0, 0u, 0u, 0, k_step_refs142, 0u, 0, -1, 0u, 0 },
    { 0, 1001ULL, 1, 2ULL, 2u, 16915060u, 3735928559u, 97u, 0u, 7u, 2u, 2u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2810u, 16u, 0, 0u, 0u, 0, k_step_refs143, 0u, 0, -1, 0u, 0 },
    { 0, 1002ULL, 1, 3ULL, 3u, 16918060u, 48879u, 96u, 0u, 7u, 3u, 3u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2820u, 16u, 0, 0u, 0u, 0, k_step_refs144, 0u, 0, -1, 0u, 0 },
    { 0, 1000ULL, 1, 1ULL, 1u, 16912060u, 287454020u, 96u, 0u, 7u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2900u, 20u, 0, 0u, 0u, 0, k_step_refs145, 0u, 0, -1, 0u, 0 },
    { 2, 1099ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs146, 0u, 0, -1, 0u, 0 },
    { 2, 1101ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs147, 0u, 0, -1, 0u, 0 },
    { 5, 1001ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs148, 0u, 0, -1, 0u, 0 },
    { 2, 1202ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs149, 0u, 0, -1, 0u, 0 },
    { 3, 1001ULL, 1, 0ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs150, 0u, 2, -1, 0u, 0 },
    { 0, 1002ULL, 1, 2ULL, 2u, 16915060u, 287454020u, 96u, 0u, 7u, 2u, 2u, 0u, 0u, 0u, 0u, 0u, 2, 5u, 2950u, 20u, 0, 0u, 0u, 0, k_step_refs151, 0u, 0, -1, 0u, 0 },
    { 7, 1000ULL, 1, 1ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs152, 0u, 0, 175, 70u, 1 },
    { 7, 1001ULL, 1, 2ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs153, 0u, 0, 245, 62u, 1 },
    { 7, 1002ULL, 1, 3ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs154, 0u, 0, 307, 70u, 1 },
    { 7, 1003ULL, 1, 4ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs155, 0u, 0, 377, 62u, 1 },
    { 7, 1000ULL, 1, 1ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs156, 0u, 0, 519, 70u, 1 },
    { 7, 1001ULL, 1, 2ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs157, 0u, 0, 589, 62u, 1 },
    { 7, 1002ULL, 1, 3ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs158, 0u, 0, 651, 70u, 1 },
    { 7, 1003ULL, 1, 4ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs159, 0u, 0, 721, 62u, 1 },
    { 7, 1000ULL, 1, 1ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs160, 0u, 0, 895, 68u, 1 },
    { 7, 1001ULL, 1, 2ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs161, 0u, 0, 963, 60u, 1 },
    { 7, 1002ULL, 1, 3ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs162, 0u, 0, 1023, 68u, 1 },
    { 7, 1003ULL, 1, 4ULL, 0u, 0u, 0u, 96u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, -1, 0u, 0u, 0u, 0, 0u, 0u, 0, k_step_refs163, 0u, 0, 1091, 60u, 1 },
};
#define K_RX_STEPS_N 164

static const kn_rx_kat_ev k_rx_evs[] = {
    { 1, 9, 0, 2ULL, 0ULL },
    { 1, 9, 0, 1ULL, 0ULL },
    { 1, 10, 0, 3ULL, 0ULL },
    { 2, 0, 0, 2ULL, 5ULL },
    { 1, 12, 0, 6ULL, 0ULL },
    { 1, 4, -6, 1ULL, 0ULL },
    { 2, 0, 0, 2ULL, 2ULL },
    { 1, 4, -6, 3ULL, 0ULL },
    { 1, 4, -6, 4ULL, 0ULL },
    { 1, 4, -6, 5ULL, 0ULL },
    { 2, 0, 0, 2ULL, 3ULL },
    { 1, 10, 0, 2ULL, 0ULL },
    { 1, 11, 0, 2ULL, 0ULL },
    { 1, 13, 0, 3ULL, 0ULL },
    { 1, 13, 0, 2ULL, 0ULL },
    { 1, 12, 0, 3ULL, 0ULL },
    { 3, 0, 0, 1000ULL, 0ULL },
    { 1, 3, 0, 2ULL, 0ULL },
    { 4, 0, 0, 0ULL, 0ULL },
    { 4, 0, 0, 1ULL, 0ULL },
    { 1, 1, 0, 2ULL, 0ULL },
    { 4, 0, 0, 0ULL, 0ULL },
    { 2, 0, 0, 2ULL, 8ULL },
    { 1, 12, 0, 9ULL, 0ULL },
    { 1, 4, -6, 1ULL, 0ULL },
    { 2, 0, 0, 2ULL, 8ULL },
    { 1, 4, -6, 9ULL, 0ULL },
    { 1, 4, -6, 1ULL, 0ULL },
    { 2, 0, 0, 2ULL, 2ULL },
    { 1, 4, -6, 3ULL, 0ULL },
    { 1, 4, -6, 1ULL, 0ULL },
    { 1, 4, -2, 2ULL, 0ULL },
    { 1, 4, -1, 3ULL, 0ULL },
    { 1, 4, -5, 4ULL, 0ULL },
    { 2, 0, 0, 2ULL, 49ULL },
    { 1, 7, 0, 2ULL, 0ULL },
    { 1, 8, 0, 3ULL, 0ULL },
    { 6, 0, 0, 101ULL, 0ULL },
    { 7, 0, 0, 202ULL, 0ULL },
    { 4, 0, 0, 0ULL, 0ULL },
};

static const kn_rx_kat_frame k_rx_frames[] = {
    { 1u, 48u, 0x006c5b9be2e14fd5ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 48u, 0x5c4451f7db6f626bULL, 0, 1, 1, 1, 4u, 16915060u },
    { 3u, 36u, 0xafe6ec70a5ccf996ULL, 0, 1, 1, 1, 7u, 16918060u },
    { 1u, 48u, 0x006c5b9be2e14fd5ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 48u, 0x5c4451f7db6f626bULL, 0, 1, 1, 1, 4u, 16915060u },
    { 1u, 48u, 0x98e037bad470ed97ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 45u, 0xd13c8cc41302a4a1ULL, 1, 1, 1, 1, 4u, 16915060u },
    { 1u, 30u, 0x15cca627fa1e0b89ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 30u, 0xa41f1a15bb038234ULL, 1, 1, 1, 1, 3u, 16915060u },
    { 1u, 42u, 0x6c9cca7bb22ab765ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 20u, 0x41016b6d4ce496eeULL, 1, 0, 2, 1, 7u, 16915060u },
    { 1u, 42u, 0x6c9cca7bb22ab765ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 1u, 956u, 0x6cb7d82b13752267ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 1u, 956u, 0x6cb7d82b13752267ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 7u, 236u, 0x8a59c23365ee0abcULL, 1, 1, 2, 1, 100u, 16930060u },
    { 2u, 956u, 0x2f11be13c6f1d27bULL, 1, 0, 2, 1, 6u, 16915060u },
    { 3u, 956u, 0xec40256e92d4e40fULL, 1, 1, 2, 1, 11u, 16918060u },
    { 1u, 188u, 0x9a89f8cb1f80aaefULL, 1, 1, 2, 1, 1u, 16912060u },
    { 1u, 380u, 0x571031069af1ea01ULL, 1, 0, 2, 1, 1u, 16912060u },
    { 21u, 1916u, 0x8a1cd66cb02582bcULL, 1, 1, 2, 1, 1u, 16972060u },
    { 2u, 30u, 0x937d87e50162773cULL, 1, 0, 0, 0, 3u, 16915060u },
    { 2u, 24u, 0x506e23923c9f575aULL, 1, 0, 2, 1, 4u, 16915060u },
    { 1u, 24u, 0x7b2572f405d009aeULL, 1, 1, 2, 1, 1u, 16912060u },
    { 1u, 24u, 0x940a4759ccc4b896ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 3u, 20u, 0x7008b4a64a01b645ULL, 1, 0, 2, 1, 3u, 16918060u },
    { 1u, 24u, 0x687319b2e1a333a0ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 3u, 20u, 0xb621d38fd5cc9d74ULL, 1, 0, 2, 1, 3u, 16918060u },
    { 2u, 20u, 0xf6044fc9b39b937bULL, 1, 0, 2, 1, 10u, 16915060u },
    { 31u, 44u, 0xe331f7012eccfe5fULL, 1, 1, 2, 1, 16777214u, 17002060u },
    { 32u, 20u, 0xa46b55ba04ba0420ULL, 1, 1, 1, 1, 1u, 17005060u },
    { 9u, 20u, 0x7a26b74c90378e4fULL, 1, 0, 2, 1, 50u, 16936060u },
    { 1u, 20u, 0x6c2a1bcbe3481dd5ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 1u, 24u, 0x865f99b62c18349fULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 24u, 0x651e8e29f1bee07cULL, 1, 0, 2, 1, 2u, 16915060u },
    { 1u, 44u, 0x95cbe78870136cc3ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 44u, 0xe73262ed6dd53f1eULL, 0, 1, 1, 1, 3u, 16915060u },
    { 1u, 44u, 0x95cbe78870136cc3ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 44u, 0xe73262ed6dd53f1eULL, 0, 1, 1, 1, 3u, 16915060u },
    { 1u, 44u, 0x95cbe78870136cc3ULL, 1, 1, 2, 1, 1u, 16912060u },
    { 2u, 44u, 0xe73262ed6dd53f1eULL, 0, 1, 1, 1, 3u, 16915060u },
};

static const kn_rx_kat_step_exp k_rx_exps[] = {
    { 0, 0, 0, 0, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 0, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 1, 0, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 1, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 1, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 1, 1, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 2, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 1, 2, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 3, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 3, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 1, 3, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 4, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 4, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 1, 4, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 5, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 5, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 1, 5, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 6, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 6, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 1, 6, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 0, 7, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 0, 1, 7, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 0, 0, 8, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 1, 0, 8, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 1, 0, 8, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 2, 1, 8, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 2, 0, 9, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 2, 0, 9, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 2, 0, 9, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 3, 1, 9, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 3, 0, 10, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 2, 3, 0, 10, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 5, 1, 10, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 5, 0, 11, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 5, 0, 11, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 5, 0, 11, 1, 2ULL, 2ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 5, 1, 11, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 5, 0, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 6, 0, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 6, 0, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 4, 6, 0, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 1, 12, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 13, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 13, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 13, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 1, 13, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 14, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 14, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 1, 14, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 10, 0, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 10, 1, 15, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 11, 0, 16, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 11, 0, 16, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 11, 0, 16, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 11, 1, 16, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 11, 0, 17, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 11, 0, 17, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 11, 0, 17, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 12, 0, 17, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 12, 1, 17, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 12, 0, 18, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 12, 0, 18, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 18, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 18, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 1, 18, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 1, 19, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 20, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 13, 0, 20, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 13, 0, 20, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 14, 1, 20, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 14, 0, 21, 0, 0ULL, 0ULL, 0, 0, 0ULL, 0ULL, -1 },
    { 0, 14, 0, 21, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 14, 0, 21, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 15, 0, 21, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 1, 21, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 1, 22, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 0, 23, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 0, 23, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 0, 23, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 0, 23, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 0, 23, 0, 0ULL, 0ULL, 0, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 1, 23, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 16, 0, 24, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 16, 0, 24, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, 2 },
    { 1, 17, 0, 24, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 18, 0, 24, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, 0 },
    { 0, 19, 1, 24, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 19, 1, 25, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 19, 0, 26, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, 1 },
    { 1, 20, 0, 26, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 21, 0, 26, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, 0 },
    { 0, 22, 1, 26, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 22, 0, 27, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 22, 0, 27, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 22, 0, 27, 1, 2ULL, 3ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 22, 0, 27, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 22, 0, 27, 1, 2ULL, 3ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 22, 0, 27, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 2, 22, 0, 27, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 24, 0, 27, 0, 0ULL, 0ULL, 1, 0, 0ULL, 0ULL, -1 },
    { 0, 24, 0, 27, 0, 0ULL, 0ULL, 0, 0, 0ULL, 0ULL, -1 },
    { 0, 24, 1, 27, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 24, 0, 28, 0, 0ULL, 0ULL, 0, 0, 0ULL, 0ULL, -1 },
    { 0, 24, 0, 28, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 24, 0, 28, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 24, 1, 28, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 24, 1, 29, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 24, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 2, 25, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 27, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 28, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 28, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 28, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 2, 28, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 30, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 31, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 32, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 33, 0, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 34, 1, 30, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 35, 1, 31, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 35, 0, 32, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 36, 0, 32, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 37, 1, 32, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 37, 0, 33, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 1, 37, 0, 33, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 38, 0, 33, 0, 0ULL, 0ULL, 1, 0, 0ULL, 0ULL, -1 },
    { 1, 38, 0, 33, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, 2 },
    { 1, 39, 0, 33, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, 0 },
    { 0, 40, 1, 33, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 0, 34, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 1, 34, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 0, 35, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 1, 35, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 0, 36, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 1, 36, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 0, 37, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 1, 37, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 0, 38, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 1, 38, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 0, 39, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
    { 0, 40, 1, 39, 0, 0ULL, 0ULL, -1, 0, 0ULL, 0ULL, -1 },
};

static const kn_rx_kat_scenario k_rx_scenarios[] = {
    { "inorder_frames", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 0, 8, 8ULL, 0ULL, 0ULL, 3ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "inorder_frames_aud", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 8, 6, 6ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "reorder_swap", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 14, 6, 6ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "dup_stale", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 20, 6, 6ULL, 0ULL, 0ULL, 2ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "dup_buffered", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 26, 4, 4ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "gap_recovery_window", 0, 4u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 30, 3, 3ULL, 0ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "gap_fill_in_time", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 33, 4, 3ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 1ULL, 0ULL },
    { "deadline_52ms", 0, 1024u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 37, 4, 4ULL, 0ULL, 0ULL, 0ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "fec_repair_e2e", 0, 16u, 4096u, 256u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 41, 5, 4ULL, 1ULL, 1ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "fec_repair_baseline", 0, 16u, 4096u, 256u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 46, 5, 5ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "fec_parity_only", 0, 16u, 4096u, 256u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 51, 3, 2ULL, 2ULL, 1ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "fec_unrecoverable", 0, 16u, 4096u, 256u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 54, 15, 12ULL, 2ULL, 0ULL, 2ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "fec_dup_shard", 0, 16u, 4096u, 256u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 69, 5, 5ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "fec_mismatch", 0, 16u, 4096u, 256u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 74, 5, 5ULL, 1ULL, 1ULL, 1ULL, 0ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "fec_multiblock", 0, 24u, 4096u, 256u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 79, 10, 9ULL, 2ULL, 1ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "sof_before_eof", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 89, 5, 4ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "spi_discontinuity", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 94, 4, 4ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "rfi_withhold_flow", 0, 4u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 98, 6, 4ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "timeout_lifecycle", 0, 8u, 4096u, 1280u, 500u, 500u, 500u, 500u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 104, 6, 2ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "pause_resume", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 110, 5, 2ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "nack_flow", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 115, 5, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "keyframe_flag", 0, 4u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 120, 6, 3ULL, 0ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "spi_wrap", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 126, 4, 4ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "huge_jump_empty", 0, 4u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 130, 2, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "capacity_recovery", 0, 4u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 132, 5, 5ULL, 0ULL, 0ULL, 0ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "malformed", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 137, 5, 2ULL, 0ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "filters", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 1, 96u, 1, 3735928559u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 142, 3, 1ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "watchdog_flow", 0, 8u, 4096u, 1280u, 5000u, 5000u, 100u, 100u, 0, 0u, 0, 0u, 8u, 16u, 64u, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 145, 7, 2ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL },
    { "srtp_cm128_stream", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 1, 10, 439, 16, 455, 14, 469, 16, 485, 20, 505, 14, 152, 4, 4ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "srtp_cm256_stream", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 2, 10, 783, 32, 815, 14, 829, 32, 861, 20, 881, 14, 156, 4, 4ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
    { "srtp_gcm256_stream", 0, 8u, 4096u, 1280u, 5000u, 5000u, 5000u, 5000u, 0, 0u, 0, 0u, 8u, 16u, 64u, 4, 8, 1151, 32, 1183, 12, 1195, 32, -1, 0, 1227, 12, 160, 4, 4ULL, 0ULL, 0ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL },
};
#define K_RX_SCENARIOS_N 31

typedef struct {
    int op;   /* 0 request,1 take,2 resolve,3 clear,4 mark_failed,
               5 marker: fresh 2048-tracker, request 0..99, take */
    uint64_t a, b, now;
    unsigned rtt;
    int exp_have; uint64_t exp_first, exp_last;
    int exp_kf; uint32_t exp_count;
} kn_rx_kat_nack_op;
static const kn_rx_kat_nack_op k_rx_nack_ops[] = {
    { 0, 10ULL, 12ULL, 0ULL, 0u, 0, 0ULL, 0ULL, -1, 3u },
    { 1, 0ULL, 0ULL, 0ULL, 0u, 1, 10ULL, 12ULL, -1, 3u },
    { 1, 0ULL, 0ULL, 0ULL, 0u, 0, 0ULL, 0ULL, -1, 3u },
    { 1, 0ULL, 0ULL, 4ULL, 0u, 1, 10ULL, 12ULL, -1, 3u },
    { 1, 0ULL, 0ULL, 8ULL, 0u, 1, 10ULL, 12ULL, -1, 3u },
    { 1, 0ULL, 0ULL, 12ULL, 0u, 0, 0ULL, 0ULL, -1, 3u },
    { 2, 11ULL, 0ULL, 12ULL, 0u, 1, 0ULL, 0ULL, -1, 2u },
    { 2, 11ULL, 0ULL, 12ULL, 0u, 0, 0ULL, 0ULL, -1, 2u },
    { 3, 0ULL, 0ULL, 0ULL, 0u, 0, 0ULL, 0ULL, -1, 0u },
    { 0, 5ULL, 5ULL, 100ULL, 0u, 0, 0ULL, 0ULL, -1, 1u },
    { 1, 0ULL, 0ULL, 100ULL, 30u, 1, 5ULL, 5ULL, -1, 1u },
    { 1, 0ULL, 0ULL, 120ULL, 30u, 0, 0ULL, 0ULL, -1, 1u },
    { 1, 0ULL, 0ULL, 134ULL, 30u, 0, 0ULL, 0ULL, -1, 1u },
    { 0, 200ULL, 200ULL, 140ULL, 0u, 0, 0ULL, 0ULL, -1, 2u },
    { 1, 0ULL, 0ULL, 200ULL, 10u, 0, 0ULL, 0ULL, -1, 0u },
    { 1, 0ULL, 0ULL, 214ULL, 10u, 0, 0ULL, 0ULL, -1, 0u },
    { 1, 0ULL, 0ULL, 228ULL, 10u, 0, 0ULL, 0ULL, -1, 0u },
    { 1, 0ULL, 0ULL, 242ULL, 10u, 0, 0ULL, 0ULL, -1, 0u },
    { 0, 300ULL, 300ULL, 1000ULL, 0u, 0, 0ULL, 0ULL, -1, 1u },
    { 1, 0ULL, 0ULL, 1000ULL, 0u, 1, 300ULL, 300ULL, -1, 1u },
    { 4, 300ULL, 300ULL, 1000ULL, 0u, 0, 0ULL, 0ULL, -1, 1u },
    { 1, 0ULL, 0ULL, 1004ULL, 0u, 1, 300ULL, 300ULL, -1, 1u },
    { 3, 0ULL, 0ULL, 0ULL, 0u, 0, 0ULL, 0ULL, -1, 0u },
    { 0, 400ULL, 409ULL, 2000ULL, 0u, 0, 0ULL, 0ULL, 1, 4u },
    { 5, 0ULL, 0ULL, 0ULL, 0u, 0, 0ULL, 0ULL, -1, 0u },
};
#define K_RX_NACK_OPS_N 25
#define K_RX_NACK_OP5_FIRST 0ULL
#define K_RX_NACK_OP5_LAST  63ULL

typedef struct {
    int op;   /* 0 lose,1 admit,2 backstop,3 take,4 restore,
               5 record_sent,6 queue */
    uint64_t now; uint32_t fi;
    int wire, known, keyframe, loss;
    uint64_t a, b;
    int exp_rc, exp_contig, exp_kf;
    int64_t exp_pfirst, exp_plast, exp_ufirst, exp_ulast;
    int exp_active;
    uint64_t exp_dropped, exp_rec_i, exp_backstops;
} kn_rx_kat_inval_op;
static const kn_rx_kat_inval_op k_rx_inval_ops[] = {
    { 0, 10ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 0, -1, 0, -1LL, -1LL, -1LL, -1LL, 0, 0ULL, 0ULL, 0ULL },
    { 1, 10ULL, 1u, 1, 1, 0, 0, 0ULL, 0ULL, 1, 1, 0, -1LL, -1LL, -1LL, -1LL, 0, 0ULL, 0ULL, 0ULL },
    { 0, 20ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 1, -1, 0, -1LL, -1LL, -1LL, -1LL, 1, 0ULL, 0ULL, 0ULL },
    { 1, 21ULL, 3u, 1, 1, 0, 1, 0ULL, 0ULL, 0, -1, 0, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 0ULL },
    { 3, 21ULL, 0u, 0, 0, 0, 0, 2ULL, 2ULL, 1, -1, 0, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 0ULL },
    { 4, 21ULL, 0u, 0, 0, 0, 0, 2ULL, 2ULL, 0, -1, 0, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 0ULL },
    { 5, 30ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 0, -1, 0, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 0ULL },
    { 2, 120ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 0, -1, 0, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 0ULL },
    { 2, 131ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 0, -1, 0, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 0ULL },
    { 2, 269ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 0, -1, 0, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 0ULL },
    { 2, 270ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 0, -1, 1, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 1ULL },
    { 2, 600ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 0, -1, 0, 2LL, 2LL, 2LL, 2LL, 1, 1ULL, 0ULL, 1ULL },
    { 1, 300ULL, 4u, 2, 1, 1, 1, 0ULL, 0ULL, 1, 1, 0, -1LL, -1LL, -1LL, -1LL, 0, 1ULL, 1ULL, 1ULL },
    { 0, 400ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 1, -1, 0, -1LL, -1LL, -1LL, -1LL, 1, 1ULL, 1ULL, 1ULL },
    { 1, 401ULL, 5u, 3, 1, 0, 1, 0ULL, 0ULL, 1, 1, 1, -1LL, -1LL, -1LL, -1LL, 1, 1ULL, 1ULL, 2ULL },
    { 0, 500ULL, 0u, 0, 0, 0, 0, 0ULL, 0ULL, 1, -1, 0, -1LL, -1LL, -1LL, -1LL, 1, 1ULL, 1ULL, 2ULL },
    { 1, 501ULL, 6u, 0, 0, 0, 0, 0ULL, 0ULL, 1, 0, 1, -1LL, -1LL, -1LL, -1LL, 0, 1ULL, 1ULL, 2ULL },
    { 1, 600ULL, 7u, 4, 1, 0, 0, 0ULL, 0ULL, 1, 1, 0, -1LL, -1LL, -1LL, -1LL, 0, 1ULL, 1ULL, 2ULL },
};
#define K_RX_INVAL_OPS_N 18

typedef struct {
    const char *name; int profile, tag_len;
    int master_off, master_len, salt_off, salt_len;
    int enc_off, enc_len, auth_off, auth_len, dsalt_off, dsalt_len;
    uint64_t index;
    int plain_off, plain_len, dgram_off, dgram_len;
} kn_rx_kat_srtp;
static const kn_rx_kat_srtp k_rx_srtp_vecs[] = {
    { "cm128_tag10", 1, 10, 2448, 16, 2464, 14, 2478, 16, 2494, 20, 2514, 14, 7ULL, 2528, 64, 2592, 74 },
    { "cm128_tag4", 1, 4, 2666, 16, 2682, 14, 2696, 16, 2712, 20, 2732, 14, 7ULL, 2746, 64, 2810, 68 },
    { "cm256_tag10", 2, 10, 2878, 32, 2910, 14, 2924, 32, 2956, 20, 2976, 14, 7ULL, 2990, 64, 3054, 74 },
    { "gcm128_tag8", 3, 8, 3128, 16, 3144, 12, 3156, 16, -1, 0, 3172, 12, 7ULL, 3184, 64, 3248, 72 },
    { "gcm256_tag8", 4, 8, 3320, 32, 3352, 12, 3364, 32, -1, 0, 3396, 12, 7ULL, 3408, 64, 3472, 72 },
    { "gcm256_tag16", 4, 16, 3544, 32, 3576, 12, 3588, 32, -1, 0, 3620, 12, 7ULL, 3632, 64, 3696, 80 },
};
#define K_RX_SRTP_VECS_N 6

typedef struct { const char *name; unsigned stream; int n;
    int miss_off, exp_off, exp_len; } kn_rx_kat_nackv2;
static const kn_rx_kat_nackv2 k_rx_nackv2[] = {
    { "single", 0u, 1, 0, 1239, 17 },
    { "two_records", 7u, 4, 1, 1256, 27 },
    { "wrap_grouping", 0u, 5, 5, 1283, 27 },
    { "contiguous_64", 0u, 64, 10, 1310, 17 },
    { "sparse_64", 0u, 64, 74, 1327, 647 },
    { "empty_rejected", 0u, 0, 0, -1, 0 },
    { "oversize_rejected", 0u, 65, 138, -1, 0 },
};
#define K_RX_NACKV2_N 7

typedef struct { const char *name; uint64_t first, last;
    int exp_off, exp_len; } kn_rx_kat_rtcp_nack;
static const kn_rx_kat_rtcp_nack k_rx_rtcp_nacks[] = {
    { "wrap_65534_65536", 65534ULL, 65536ULL, 1974, 16 },
    { "single", 100ULL, 100ULL, 1990, 16 },
    { "sixteen", 1000ULL, 1016ULL, 2006, 16 },
    { "two_entries", 1000ULL, 1020ULL, 2022, 20 },
    { "cap_64", 0ULL, 200ULL, 2042, 28 },
};
#define K_RX_RTCP_NACKS_N 5
#define K_RX_RTCP_PLI_OFF 2070

typedef struct { const char *name; int kind;
    uint64_t args[12]; int nargs; uint64_t fbits; int have_f;
    int exp_off, exp_len; } kn_rx_kat_ctrl;
static const kn_rx_kat_ctrl k_rx_ctrls[] = {
    { "idr", 0, { 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL }, 0, 0x0000000000000000ULL, 0, 2082, 6 },
    { "rfi_range", 1, { 5ULL, 9ULL, 2ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL }, 3, 0x0000000000000000ULL, 0, 2088, 28 },
    { "frame_ack_f", 2, { 7ULL, 99ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL }, 2, 0x40934a0000000000ULL, 1, 2116, 106 },
    { "frame_ack_none", 2, { 8ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL }, 2, 0x0000000000000000ULL, 0, 2222, 106 },
    { "pacing_clamped", 3, { 12ULL, 16666ULL, 20000ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL }, 3, 0x0000000000000000ULL, 0, 2328, 32 },
    { "pacing_small", 3, { 13ULL, 16666ULL, 100ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL }, 3, 0x0000000000000000ULL, 0, 2360, 32 },
    { "qos", 4, { 3ULL, 77ULL, 12345ULL, 678ULL, 250ULL, 15ULL, 42ULL, 287454020ULL, 2ULL, 4500ULL, 900ULL, 0ULL }, 11, 0x0000000000000000ULL, 0, 2392, 56 },
};
#define K_RX_CTRLS_N 7

static const uint16_t k_rx_miss[] = {
    4660, 4660, 4661, 4724, 4725, 65534, 65535, 0, 62, 63, 0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
    38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
    54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 0, 65, 130, 195, 260, 325,
    390, 455, 520, 585, 650, 715, 780, 845, 910, 975, 1040, 1105, 1170, 1235, 1300, 1365,
    1430, 1495, 1560, 1625, 1690, 1755, 1820, 1885, 1950, 2015, 2080, 2145, 2210, 2275, 2340, 2405,
    2470, 2535, 2600, 2665, 2730, 2795, 2860, 2925, 2990, 3055, 3120, 3185, 3250, 3315, 3380, 3445,
    3510, 3575, 3640, 3705, 3770, 3835, 3900, 3965, 4030, 4095, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const unsigned char k_rx_blob[] = {
    0x80, 0x60, 0x00, 0x01, 0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x03,
    0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x04, 0x00, 0x01, 0x5f, 0x90,
    0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x05, 0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x01,
    0x80, 0x60, 0x00, 0x01, 0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x09,
    0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x01, 0x00, 0x01, 0x5f, 0x90,
    0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x03, 0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x01,
    0x80, 0x60, 0x00, 0x05, 0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x06,
    0x00, 0x01, 0x5f, 0x90, 0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x07, 0x00, 0x01, 0x5f, 0x90,
    0x00, 0x00, 0x00, 0x01, 0x80, 0x60, 0x00, 0x01, 0x00, 0x01, 0x5f, 0x90, 0x11, 0x22, 0x33, 0x44,
    0xc0, 0x60, 0x00, 0x02, 0x00, 0x01, 0x5f, 0x90, 0x11, 0x22, 0x33, 0x44, 0x80, 0x60, 0x00, 0x03,
    0xa0, 0x60, 0x00, 0x04, 0x00, 0x01, 0x5f, 0x90, 0x11, 0x22, 0x33, 0x44, 0x01, 0x02, 0x00, 0x90,
    0x60, 0x00, 0x01, 0x01, 0x02, 0x0e, 0xbc, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45,
    0x6e, 0xf3, 0x3b, 0x89, 0x11, 0x09, 0xd7, 0xf3, 0x02, 0x60, 0x2c, 0x53, 0x6d, 0x21, 0x99, 0xa8,
    0x66, 0xd1, 0x8b, 0x28, 0x4c, 0xaa, 0xd2, 0xf9, 0xb9, 0x5b, 0xd9, 0xd5, 0x5b, 0x4a, 0xcb, 0x90,
    0x96, 0xf0, 0xc4, 0xa0, 0x2a, 0x90, 0x60, 0x00, 0x02, 0x01, 0x02, 0x0e, 0xbc, 0x11, 0x22, 0x33,
    0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x86, 0x90, 0x9b, 0x8d, 0xd9, 0xf7, 0xa3, 0x14, 0x6a, 0x4b,
    0x63, 0x01, 0x5b, 0x5c, 0x03, 0x09, 0x5b, 0x72, 0xda, 0xdb, 0xd7, 0xf1, 0xcf, 0xcc, 0x41, 0xa4,
    0x59, 0x9d, 0x2e, 0x90, 0x60, 0x00, 0x03, 0x01, 0x02, 0x1a, 0x74, 0x11, 0x22, 0x33, 0x44, 0x47,
    0x53, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x09, 0x43, 0x6a, 0xce, 0x00, 0xaf, 0x38, 0xf1, 0x96, 0x29, 0x35, 0xd1, 0xa5,
    0x95, 0x2f, 0x4e, 0xd3, 0xfc, 0xc0, 0x25, 0xa9, 0x64, 0x13, 0x66, 0xce, 0x62, 0xc4, 0x4e, 0xb4,
    0xf6, 0xcc, 0xd2, 0x17, 0x26, 0x49, 0x94, 0xa5, 0xb3, 0x90, 0x60, 0x00, 0x04, 0x01, 0x02, 0x1a,
    0x74, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x91, 0x16, 0xc4, 0x48, 0xd9, 0x33, 0x5e,
    0x32, 0x9b, 0x4c, 0xa5, 0xc5, 0x7f, 0xe2, 0xea, 0x43, 0x90, 0x25, 0x19, 0x14, 0x85, 0x3e, 0xf5,
    0x95, 0xe0, 0x4e, 0xcf, 0xb3, 0x05, 0x7d, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9e,
    0xca, 0x93, 0x5e, 0x12, 0x34, 0x2a, 0xe7, 0x5e, 0x93, 0xe4, 0x26, 0x77, 0x24, 0xd2, 0x6f, 0xcf,
    0x51, 0x8a, 0xc0, 0x12, 0x0c, 0xa1, 0x1a, 0x45, 0x79, 0xf7, 0xde, 0xa8, 0x16, 0xe2, 0x9b, 0xa1,
    0xe0, 0x5e, 0x81, 0x4e, 0x15, 0xcc, 0x37, 0x0b, 0x91, 0xc3, 0x3e, 0x54, 0x22, 0xfc, 0x04, 0x74,
    0xb3, 0xa5, 0x1d, 0xd7, 0x3e, 0x03, 0x38, 0x90, 0x60, 0x00, 0x01, 0x01, 0x02, 0x0e, 0xbc, 0x11,
    0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0xbf, 0x09, 0x6f, 0x1e, 0x9e, 0x33, 0x0b, 0x00,
    0x98, 0x7b, 0x82, 0x3d, 0xf0, 0xce, 0x63, 0xb2, 0x2a, 0xea, 0x03, 0x4b, 0x42, 0x35, 0x55, 0x77,
    0x80, 0x1d, 0x58, 0xf1, 0x2f, 0xf8, 0x37, 0x44, 0x78, 0x68, 0xb9, 0x90, 0x38, 0x90, 0x60, 0x00,
    0x02, 0x01, 0x02, 0x0e, 0xbc, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x02, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa3, 0x6d, 0x57,
    0xc3, 0xe7, 0xd3, 0xc2, 0x6e, 0x9e, 0x97, 0x93, 0xf5, 0x1c, 0x60, 0xbf, 0xde, 0xb3, 0x9f, 0x6c,
    0x33, 0x02, 0x28, 0x96, 0x11, 0x84, 0x6f, 0x5a, 0x35, 0x6c, 0x90, 0x90, 0x60, 0x00, 0x03, 0x01,
    0x02, 0x1a, 0x74, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3b, 0x23, 0x0f, 0xf4, 0xcc,
    0xd9, 0x05, 0x20, 0x4f, 0xd2, 0x04, 0x53, 0x13, 0xb4, 0x51, 0x07, 0x27, 0x47, 0x64, 0x1b, 0x0d,
    0x6c, 0xa9, 0xf1, 0x63, 0xe1, 0xa0, 0x59, 0xe2, 0xd0, 0xc1, 0xf4, 0x59, 0xcf, 0x58, 0x16, 0x8b,
    0xf1, 0x90, 0x60, 0x00, 0x04, 0x01, 0x02, 0x1a, 0x74, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00,
    0x04, 0x00, 0x04, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x29, 0x0b, 0xde, 0xfa, 0x33, 0xb1, 0x76, 0x71, 0x44, 0x49, 0xbb, 0xae, 0x9c, 0x8a, 0x47,
    0x87, 0x82, 0xea, 0xa8, 0xd0, 0x21, 0x80, 0x00, 0x2b, 0x2f, 0xab, 0x71, 0x37, 0x80, 0xa6, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9e, 0xca, 0x93, 0x5e, 0x12, 0x34, 0x3c, 0xc4, 0x89,
    0xa5, 0xc4, 0x05, 0xfc, 0xee, 0x7d, 0xb0, 0x38, 0xc6, 0x76, 0xad, 0xf9, 0x9b, 0xc9, 0x6b, 0xf9,
    0x72, 0x0a, 0x15, 0x88, 0xce, 0x90, 0x3b, 0x4a, 0x9a, 0x59, 0xc7, 0xf7, 0x6b, 0x78, 0xcd, 0xd9,
    0x50, 0xb4, 0x28, 0x3c, 0x17, 0xc1, 0xfe, 0xc3, 0xda, 0x08, 0xe6, 0xbf, 0xc3, 0x6a, 0xde, 0xf4,
    0x99, 0xbe, 0x06, 0xe4, 0x37, 0xd1, 0x5b, 0xa8, 0xdc, 0x83, 0xdf, 0x64, 0x58, 0xc9, 0x83, 0x90,
    0x60, 0x00, 0x01, 0x01, 0x02, 0x0e, 0xbc, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xeb,
    0x6d, 0xbc, 0x7e, 0x9a, 0x54, 0x1d, 0x1a, 0x6a, 0x3f, 0xb5, 0xd6, 0x90, 0xb1, 0x79, 0xb6, 0x69,
    0xb9, 0x68, 0xe8, 0xcf, 0x44, 0x22, 0xcb, 0x73, 0x76, 0xce, 0x94, 0xe6, 0xb2, 0xfd, 0xfb, 0x88,
    0xe1, 0x80, 0xc1, 0x90, 0x60, 0x00, 0x02, 0x01, 0x02, 0x0e, 0xbc, 0x11, 0x22, 0x33, 0x44, 0x47,
    0x53, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x6e, 0xfe, 0xff, 0xf1, 0xf2, 0x80, 0x50, 0x57, 0xd0, 0x05, 0x2f, 0x52, 0xc0,
    0x5a, 0xc9, 0x6a, 0x03, 0x98, 0xeb, 0xbe, 0x09, 0x50, 0x32, 0x1d, 0x97, 0x45, 0xb8, 0x72, 0x90,
    0x60, 0x00, 0x03, 0x01, 0x02, 0x1a, 0x74, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00,
    0x03, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf3,
    0x5c, 0x32, 0x60, 0xe5, 0xee, 0x96, 0x40, 0x9d, 0x12, 0x97, 0xd4, 0x58, 0x16, 0x95, 0x82, 0x11,
    0x94, 0xf8, 0xaa, 0xe0, 0x8e, 0x10, 0x61, 0xec, 0x3b, 0x56, 0xa5, 0x79, 0x62, 0xb7, 0x16, 0xb9,
    0x51, 0x67, 0xfc, 0x90, 0x60, 0x00, 0x04, 0x01, 0x02, 0x1a, 0x74, 0x11, 0x22, 0x33, 0x44, 0x47,
    0x53, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x9b, 0x4c, 0x0d, 0x41, 0x1d, 0x96, 0x5c, 0x42, 0x38, 0x24, 0xfb, 0xe5, 0x04,
    0x11, 0x32, 0x26, 0x9e, 0x8a, 0xb7, 0x71, 0x1c, 0x07, 0x89, 0x4c, 0x5f, 0x63, 0x89, 0xe6, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9e, 0xca, 0x93, 0x5e, 0x0e, 0x44, 0xa0, 0xb0, 0xe7,
    0xf1, 0xbd, 0xbb, 0x29, 0x8c, 0xbe, 0xe5, 0x2c, 0x9f, 0x8a, 0xc1, 0xc3, 0x77, 0x26, 0x76, 0x8c,
    0x94, 0x6f, 0x59, 0xbd, 0xaa, 0xa0, 0x99, 0x60, 0x8c, 0xbf, 0x66, 0x78, 0xb9, 0xd9, 0x7b, 0x7f,
    0xfb, 0x37, 0xca, 0xd5, 0x39, 0xa2, 0x9d, 0x17, 0x03, 0x0d, 0x00, 0x02, 0x00, 0x01, 0x34, 0x12,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x03, 0x17, 0x00, 0x02, 0x07, 0x02, 0x34,
    0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x75, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x17, 0x03, 0x17, 0x00, 0x02, 0x00, 0x02, 0xfe, 0xff, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x03,
    0x0d, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x17,
    0x03, 0x83, 0x02, 0x02, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xc3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8a, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xcb, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x8e, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcf, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x51, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x92, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xd3, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x96, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x05, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x59, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9a, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xdb, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x07,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5d, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x9e, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdf, 0x07, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x61, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa2, 0x08, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xe3, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x09,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x65, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xa6, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe7, 0x09, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x69, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xeb, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x0b,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xae, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xef, 0x0b, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x71, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb2, 0x0c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xf3, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x0d,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x75, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xb6, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf7, 0x0d, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x79, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xba, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xfb, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x0f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7d, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xbe, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x0f, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0xcd, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0xff, 0xfe, 0x00, 0x03, 0x81, 0xcd, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x00, 0x64, 0x00, 0x00, 0x81, 0xcd, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x03, 0xe8, 0xff, 0xff, 0x81, 0xcd, 0x00, 0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x03, 0xe8, 0xff, 0xff, 0x03, 0xf9, 0x00, 0x07, 0x81, 0xcd, 0x00, 0x06, 0x01, 0x02,
    0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0xff, 0xff, 0x00, 0x11, 0xff, 0xff, 0x00, 0x22,
    0xff, 0xff, 0x00, 0x33, 0x0f, 0xff, 0x81, 0xce, 0x00, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x02, 0x03, 0x02, 0x00, 0x00, 0x00, 0x01, 0x03, 0x18, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x02, 0x66, 0x00, 0x01, 0x00, 0x09, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x93, 0x40, 0x00, 0x00, 0x80, 0xbf,
    0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf,
    0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x04, 0x02,
    0x66, 0x00, 0x01, 0x00, 0x09, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00,
    0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00,
    0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x03, 0x02, 0x1c, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x1a, 0x41, 0x00, 0x00,
    0x1a, 0x41, 0x00, 0x00, 0x1a, 0x34, 0x00, 0x00, 0x03, 0x02, 0x1c, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x1a, 0x41, 0x00, 0x00,
    0x64, 0x00, 0x00, 0x00, 0x1a, 0x34, 0x00, 0x00, 0x07, 0x02, 0x34, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x39, 0x30, 0x00, 0x00,
    0xa6, 0x02, 0x00, 0x00, 0xfa, 0x00, 0x0f, 0x00, 0x2a, 0x00, 0xe8, 0x03, 0xe8, 0x03, 0x00, 0x00,
    0x44, 0x33, 0x22, 0x11, 0x02, 0x00, 0x00, 0x00, 0x94, 0x11, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9e, 0xca, 0x93, 0x5e, 0x12, 0x34, 0x2a, 0xe7,
    0x5e, 0x93, 0xe4, 0x26, 0x77, 0x24, 0xd2, 0x6f, 0xcf, 0x51, 0x8a, 0xc0, 0x12, 0x0c, 0xa1, 0x1a,
    0x45, 0x79, 0xf7, 0xde, 0xa8, 0x16, 0xe2, 0x9b, 0xa1, 0xe0, 0x5e, 0x81, 0x4e, 0x15, 0xcc, 0x37,
    0x0b, 0x91, 0xc3, 0x3e, 0x54, 0x22, 0xfc, 0x04, 0x74, 0xb3, 0xa5, 0x1d, 0xd7, 0x3e, 0x03, 0x38,
    0x90, 0x60, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x53, 0x00, 0x02, 0x00, 0x00, 0x01, 0x05, 0x51, 0x9b, 0xcb, 0x05, 0x7b, 0x91, 0xcf, 0xa1,
    0xbf, 0xab, 0x5f, 0x85, 0x19, 0x15, 0x0f, 0x6f, 0xf5, 0x4b, 0x4b, 0x77, 0x1d, 0x0d, 0xdf, 0x31,
    0x90, 0x60, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x98, 0xe8, 0x3f, 0x4c, 0x65, 0x22, 0xf2, 0x9d, 0x42, 0xfd, 0xad, 0x3e, 0x2a, 0xf2, 0x3f, 0x60,
    0x68, 0x6a, 0xb4, 0x2c, 0xd0, 0xae, 0x99, 0x36, 0x35, 0xaa, 0x08, 0xc4, 0xc5, 0xc8, 0x89, 0x17,
    0xa9, 0x22, 0x6a, 0x58, 0x8e, 0xd0, 0xd6, 0x51, 0xe6, 0x21, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x9e, 0xca, 0x93, 0x5e, 0x12, 0x34, 0x2a, 0xe7, 0x5e, 0x93, 0xe4, 0x26, 0x77, 0x24,
    0xd2, 0x6f, 0xcf, 0x51, 0x8a, 0xc0, 0x12, 0x0c, 0xa1, 0x1a, 0x45, 0x79, 0xf7, 0xde, 0xa8, 0x16,
    0xe2, 0x9b, 0xa1, 0xe0, 0x5e, 0x81, 0x4e, 0x15, 0xcc, 0x37, 0x0b, 0x91, 0xc3, 0x3e, 0x54, 0x22,
    0xfc, 0x04, 0x74, 0xb3, 0xa5, 0x1d, 0xd7, 0x3e, 0x03, 0x38, 0x90, 0x60, 0x00, 0x07, 0x01, 0x02,
    0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x4d, 0x00,
    0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x53, 0x00, 0x02, 0x00, 0x00,
    0x01, 0x05, 0x51, 0x9b, 0xcb, 0x05, 0x7b, 0x91, 0xcf, 0xa1, 0xbf, 0xab, 0x5f, 0x85, 0x19, 0x15,
    0x0f, 0x6f, 0xf5, 0x4b, 0x4b, 0x77, 0x1d, 0x0d, 0xdf, 0x31, 0x90, 0x60, 0x00, 0x07, 0x01, 0x02,
    0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x4d, 0x00,
    0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x98, 0xe8, 0x3f, 0x4c, 0x65, 0x22,
    0xf2, 0x9d, 0x42, 0xfd, 0xad, 0x3e, 0x2a, 0xf2, 0x3f, 0x60, 0x68, 0x6a, 0xb4, 0x2c, 0xd0, 0xae,
    0x99, 0x36, 0x35, 0xaa, 0x08, 0xc4, 0xc5, 0xc8, 0x89, 0x17, 0xa9, 0x22, 0x6a, 0x58, 0x00, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9e, 0xca, 0x93, 0x5e, 0x12, 0x34, 0x3c, 0xc4, 0x89, 0xa5,
    0xc4, 0x05, 0xfc, 0xee, 0x7d, 0xb0, 0x38, 0xc6, 0x76, 0xad, 0xf9, 0x9b, 0xc9, 0x6b, 0xf9, 0x72,
    0x0a, 0x15, 0x88, 0xce, 0x90, 0x3b, 0x4a, 0x9a, 0x59, 0xc7, 0xf7, 0x6b, 0x78, 0xcd, 0xd9, 0x50,
    0xb4, 0x28, 0x3c, 0x17, 0xc1, 0xfe, 0xc3, 0xda, 0x08, 0xe6, 0xbf, 0xc3, 0x6a, 0xde, 0xf4, 0x99,
    0xbe, 0x06, 0xe4, 0x37, 0xd1, 0x5b, 0xa8, 0xdc, 0x83, 0xdf, 0x64, 0x58, 0xc9, 0x83, 0x90, 0x60,
    0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x07,
    0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x53,
    0x00, 0x02, 0x00, 0x00, 0x01, 0x05, 0x51, 0x9b, 0xcb, 0x05, 0x7b, 0x91, 0xcf, 0xa1, 0xbf, 0xab,
    0x5f, 0x85, 0x19, 0x15, 0x0f, 0x6f, 0xf5, 0x4b, 0x4b, 0x77, 0x1d, 0x0d, 0xdf, 0x31, 0x90, 0x60,
    0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04, 0x00, 0x07,
    0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x51,
    0x53, 0x59, 0x6f, 0xf6, 0xce, 0xd7, 0x02, 0xc9, 0x93, 0xb9, 0x8a, 0x74, 0xee, 0x0a, 0xcb, 0xf2,
    0x1c, 0xf2, 0x4b, 0x95, 0x11, 0xb8, 0x92, 0xc0, 0xd0, 0x5b, 0xae, 0xef, 0x86, 0x59, 0xf5, 0xa9,
    0x91, 0x82, 0xec, 0x77, 0xf8, 0xa5, 0xe7, 0x70, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9e, 0xca, 0x93, 0x5e, 0x5e, 0x14, 0x6b, 0x73, 0xf0, 0x8f, 0x28, 0xf8, 0x07, 0xcb, 0x7f, 0xec,
    0xec, 0x2f, 0x57, 0xfc, 0x8f, 0xcc, 0x03, 0xa7, 0xfd, 0x3e, 0xa4, 0x37, 0x18, 0x0a, 0xfd, 0x33,
    0x90, 0x60, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x53, 0x00, 0x02, 0x00, 0x00, 0x01, 0x05, 0x51, 0x9b, 0xcb, 0x05, 0x7b, 0x91, 0xcf, 0xa1,
    0xbf, 0xab, 0x5f, 0x85, 0x19, 0x15, 0x0f, 0x6f, 0xf5, 0x4b, 0x4b, 0x77, 0x1d, 0x0d, 0xdf, 0x31,
    0x90, 0x60, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x5e, 0x72, 0x1d, 0x64, 0x90, 0x3e, 0x8a, 0x6e, 0x9a, 0xb3, 0xfa, 0x1b, 0x07, 0x83, 0xe2, 0x4d,
    0xa9, 0x1a, 0x30, 0xe2, 0xdf, 0x0f, 0x97, 0x96, 0xff, 0xfd, 0x4e, 0x4c, 0x1c, 0x2a, 0x39, 0x2e,
    0xf7, 0x3e, 0x54, 0x14, 0x69, 0x73, 0x5d, 0x55, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9e, 0xca, 0x93, 0x5e, 0x0e, 0x44, 0xa0, 0xb0, 0xe7, 0xf1, 0xbd, 0xbb, 0x29, 0x8c, 0xbe, 0xe5,
    0x2c, 0x9f, 0x8a, 0xc1, 0xc3, 0x77, 0x26, 0x76, 0x8c, 0x94, 0x6f, 0x59, 0xbd, 0xaa, 0xa0, 0x99,
    0x60, 0x8c, 0xbf, 0x66, 0x78, 0xb9, 0xd9, 0x7b, 0x7f, 0xfb, 0x37, 0xca, 0xd5, 0x39, 0xa2, 0x9d,
    0x90, 0x60, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x53, 0x00, 0x02, 0x00, 0x00, 0x01, 0x05, 0x51, 0x9b, 0xcb, 0x05, 0x7b, 0x91, 0xcf, 0xa1,
    0xbf, 0xab, 0x5f, 0x85, 0x19, 0x15, 0x0f, 0x6f, 0xf5, 0x4b, 0x4b, 0x77, 0x1d, 0x0d, 0xdf, 0x31,
    0x90, 0x60, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf6, 0x7c, 0x12, 0x23, 0x7a, 0x06, 0x67, 0x72, 0x84, 0x4e, 0x03, 0xa9, 0xc7, 0x23, 0x4b, 0x00,
    0x63, 0x60, 0x9c, 0xde, 0xe7, 0x75, 0x56, 0x59, 0xb8, 0x49, 0x3a, 0xed, 0x00, 0xf3, 0x60, 0x92,
    0xfe, 0x44, 0xb7, 0xb6, 0xa9, 0x90, 0xc4, 0x66, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9e, 0xca, 0x93, 0x5e, 0x0e, 0x44, 0xa0, 0xb0, 0xe7, 0xf1, 0xbd, 0xbb, 0x29, 0x8c, 0xbe, 0xe5,
    0x2c, 0x9f, 0x8a, 0xc1, 0xc3, 0x77, 0x26, 0x76, 0x8c, 0x94, 0x6f, 0x59, 0xbd, 0xaa, 0xa0, 0x99,
    0x60, 0x8c, 0xbf, 0x66, 0x78, 0xb9, 0xd9, 0x7b, 0x7f, 0xfb, 0x37, 0xca, 0xd5, 0x39, 0xa2, 0x9d,
    0x90, 0x60, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x53, 0x00, 0x02, 0x00, 0x00, 0x01, 0x05, 0x51, 0x9b, 0xcb, 0x05, 0x7b, 0x91, 0xcf, 0xa1,
    0xbf, 0xab, 0x5f, 0x85, 0x19, 0x15, 0x0f, 0x6f, 0xf5, 0x4b, 0x4b, 0x77, 0x1d, 0x0d, 0xdf, 0x31,
    0x90, 0x60, 0x00, 0x07, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x47, 0x53, 0x00, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf6, 0x7c, 0x12, 0x23, 0x7a, 0x06, 0x67, 0x72, 0x84, 0x4e, 0x03, 0xa9, 0xc7, 0x23, 0x4b, 0x00,
    0x63, 0x60, 0x9c, 0xde, 0xe7, 0x75, 0x56, 0x59, 0xb8, 0x49, 0x3a, 0xed, 0x00, 0xf3, 0x60, 0x92,
    0xfe, 0x44, 0xb7, 0xb6, 0xa9, 0x90, 0xc4, 0x66, 0xe4, 0x4c, 0x99, 0x74, 0x23, 0x12, 0x11, 0x15,
};
#define K_RX_BLOB_LEN 3776

#endif /* KN_RX_KAT_VECTORS_H */
