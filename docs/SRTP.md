# SRTP crypto (`knsrtp`, phase 1) — implementation notes

Status: **VERIFIED ON THE TARGET (2026-10-21, manual run of the T9 build,
w64devkit GCC 16.2.0, Win7 x64): `test_knsrtp` 37 checks, 0 failures.**
Tiny-AES-c + the KDF + AES-CM keystream + all three IV builders reproduce
FIPS-197 / SP800-38A / RFC 3711 B.2 (incl. the 128-bit counter carry) and
B.3 (incl. the 94-octet six-block auth key) byte-for-byte on the production
toolchain. Build: warnings=0 (cmake, -Wall -Wextra).

Scope of phase 1: RFC 3711 KDF (AES-128-CM PRF), AES-CM keystream, and the three
IV builders. Phase 2 (next): HMAC-SHA1 authentication with `be32(roc)` in the
MAC input (nvst.rs:2820-2872), the 2048-packet replay window (nvst.rs:2635-2705),
SRTCP. AES-GCM (8/16-byte tags, nvst.rs:2711-2790) comes with the mbedTLS
vendoring at stage 2 — GCM/GHASH is NOT hand-rolled here.

## Provenance (verified against fetched sources, never memory)

Local reference copy: `artifacts/nvst-extract/nvst.rs` (OpenNOW @ `a49b124`).

| Fact | Source |
|---|---|
| Labels: SRTP key `0x00`, auth `0x01`, salt `0x02`; SRTCP key `0x03`, SRTCP salt `0x05`. "Mjolnir video is RTP, so use 0x00/0x02" | `nvst.rs:88-95` |
| Profiles GFN may negotiate: `AEAD_AES_{128,256}_GCM(_8)`, `AES_CM_{128,256}_HMAC_SHA1_{32,80}`; master key 16/32 B; salt 12 B (GCM) / 14 B (CM); GCM_8 tag = 8 bytes | `nvst.rs:301-330`, `85-86`, `1586-1640` |
| KDF: `iv = zeros16; iv[..salt]=master_salt; iv[7] ^= label;` out = AES-CM keystream of master key over iv, truncated (= RFC 3711 §4.3.1 with `label·2^64` landing in byte 7, initial derivation only — no kdr rekey) | `nvst.rs:2873-2900` |
| AES-CM: keystream block i = `E(key, be128(iv)+i)`, full 128-bit counter; partial trailing block | RFC 3711 §4.1.1; `nvst.rs:2850-2861` |
| SRTP CM IV: `salt14` at [0..14), `^= ssrc_be32` at [4..8), `^= packet_index_be48` at [8..14) | `nvst.rs:2902-2913` |
| SRTP GCM IV (RFC 7714 §9.1): `salt12`, `^= ssrc` [2..6), `^= roc` [6..10), `^= seq` [10..12) | `nvst.rs:2915-2926` |
| SRTCP GCM IV (RFC 7714 §9.2): `salt12`, `^= ssrc` [2..6), `^= srtcp_index` [6..10) | `nvst.rs:2929-2939` |
| packet_index = `(ROC << 16) | SEQ` (48-bit) | RFC 3711 §3.4 |

## KAT vectors (`streamer/tests/knsrtp_kat_vectors.h`, generated)

Generator: `python3 tools/srtp_reference.py` — pure-Python AES-128 that
**self-proves against FIPS-197 C.1, SP800-38A F.1.1, RFC 3711 B.2 (six
keystream blocks including the `FEFF→FF00` carry at indices 65279-65281) and
RFC 3711 B.3 (cipher key, cipher salt, 94-octet auth key across six counter
blocks with last-block truncation) before emitting anything.** RFC constants
are transcribed verbatim from the fetched RFC text (`rfc-editor.org/rfc/rfc3711.txt`).
nvst-specific goldens (SRTCP derivations, 20 B auth key, IV builders) are
cross-implementation vectors computed by the same proven Python core.

`test_knsrtp.c` (~40 checks): AES block goldens (incl. in-place aliasing),
B.2 keystream blocks, B.3 KDF outputs, error paths (salt > 14, out_len 0 /
> 4096, NULLs), 12-byte-salt GCM path, all three IV builders + structural
placement checks, keystream-XOR properties (matches B.2 block 0, round-trip,
split-at-block-25 == continuous, zero-length no-op).

## The trap this round caught (recorded so nobody repeats it)

**AES's polynomial `0x11B` is irreducible but NOT primitive**: the element
`0x02` has multiplicative order **51**, not 255. Log/exp tables for the AES
S-box must be built with generator **`0x03`**. Contrast: the FEC field uses
**`0x11D`**, where `0x02` IS a generator (`nvst.rs:3726-3739` doubles).
The first draft of `srtp_reference.py` built the S-box by doubling and
silently produced a degenerate box — caught by the self-check anchors before
any vector was emitted. That is the KAT process working as designed
(BOOTSTRAP §7.9: prove crypto offline, before the live attempt).

## AES backend

`kn_aes.h/.c` wraps vendored **tiny-AES-c** (Unlicense; `third_party/tiny-aes-c/`,
sha256 + provenance in its `PROVENANCE.md`; verified against NIST vectors by
its author, and re-verified at runtime by `test_knsrtp`). Only
`AES_init_ctx` + `AES_ECB_encrypt` are used; CTR/KDF logic lives in `knsrtp.c`
so counter semantics stay exactly RFC/nvst-controlled. Compiled with upstream
defaults (AES128, ECB+CBC+CTR on) in ALL translation units — one consistent
`struct AES_ctx` layout, compile-time asserted against the wrapper's opaque
storage. Replacement path: mbedTLS at stage 2 behind the same `kn_aes128_*`
API; the vectors must pass unchanged. AES-256 (for `*_256_*` profiles) comes
with mbedTLS too — tiny-AES is single-key-size per build and stays 128-only.

## Phase-2 status (implemented this round, awaiting T9 compile+run)

Implemented in `kn_sha1` + `knsrtp` (all zero-allocation, KAT-backed):

1. **SHA-1**: vendored Brad Conte implementation (`third_party/crypto-algorithms-sha1/`,
   public domain, sha256 + provenance recorded) behind the `kn_sha1` wrapper
   (opaque ctx + compile-time size assert). KAT: RFC 3174 §7.3 TEST1-TEST4
   (incl. the 56-byte padding edge, 1M×'a' streaming) + empty message.
2. **HMAC-SHA1** (RFC 2104 glue over the vetted hash — one-shot + incremental):
   KAT = RFC 2202 cases 1-7 verbatim (incl. >block-size key hashing).
3. **Auth tag**: `HMAC-SHA1(auth20, packet[..auth_len) || be32(roc))`, first
   `tag_len ∈ {4,10}` bytes; constant-time compare (`kn_ct_memeq`);
   authenticate-before-decrypt; failed auth leaves the packet untouched
   (nvst.rs:2820-2871 order). KAT: python-reference protect goldens
   (tag80 + tag32) over a 112-byte synthetic packet with B.3-derived session
   material; tamper cases (payload byte, tag byte) must return
   AuthenticationFailed.
4. **Replay window**: 1:1 port of nvst.rs:3121-3211 — 2048 packets,
   32×u64 age-bitmask, `guess` (δ±32768 ROC estimate, checked overflow/
   underflow), `check`, `commit` (bit-shift advance, window clear on
   advance ≥ 2048, defensive no-op instead of the reference's `.expect()`
   panic for out-of-window late commits). KAT: a 96-operation deterministic
   script (cold start, sequential, duplicates, late fills, far jump,
   ancient reject, 16-bit wrap with roc+1, pre-wrap duplicate via roc-1,
   age 2047/2048 edge, roc-0 underflow) validated against a line-by-line
   Python port of the same reference code.
5. **`unprotect_cm_hmac` derives the ROC from `packet_index >> 16`
   internally** (nvst.rs:2638-2640) — callers cannot pass an inconsistent
   (index, roc) pair.
6. **SRTCP**: NOT implemented for CM profiles, by evidence — nvst's
   `SrtcpSender::from_material` returns `None` for non-GCM material and RR
   rides the DTLS-encrypted `rtcp1` SCTP channel (nvst.rs:4346-4410,
   comment at 4398-4400). SRTCP-GCM comes with mbedTLS at stage 2/3.

Still deferred to stage 2 (mbedTLS): AES-256 CM profiles, AES-GCM(_8),
TLS/WSS/DTLS. The `kn_aes`/`kn_sha1` wrapper APIs are the switch points —
the KATs must pass unchanged across backends.

## Phase 3 (round XVI — DELIVERED, sandbox cross-verified; target pending)

The mbedTLS backend landed with the TLS work, so phase 3 shipped as part
of the stage-2 `kn_rx` receive path:

1. **KDF256** (`kn_srtp_kdf256`): the RFC 3711 construction with the
   AES-256-CM PRF (`derive_aes_cm_key`, nvst.rs:2873-2886 — `Aes256Ctr =
   Ctr128BE`, so the existing full-128-bit counter carry applies).
2. **AES-256-CM keystream** (`kn_srtp_aes256_cm_keystream_xor`) for the
   CM256 profile (auth = the same 20-byte-key HMAC-SHA1 — key-size
   independent).
3. **AES-GCM 128/256** (`kn_srtp_gcm_auth_{encrypt,decrypt}`, tags 4..16 —
   the Mjolnir policy is the 8-byte `aes_gcm_*_8_auth`): AAD = RTP header
   region, IV = the RFC 7714 builder from phase 1, constant-time verdicts
   via mbedTLS; on auth failure the tentatively decrypted region is
   SCRUBBED (the one-call mbedTLS API decrypts before verifying — a
   documented deviation from the reference's verify-then-decrypt; the
   pipeline drops the packet either way).
4. **Session layer** (`kn_rx_srtp_session_init` + `kn_rx_srtp_unprotect`
   in kn_rx): the four `NvstSrtpMaterial` profiles (CM128/CM256/GCM128/
   GCM256) with the reference's exact derivation map (nvst.rs:2576-2633)
   and the exact unprotect order (parse → guess → cipher → check →
   plaintext padding validation → commit). The legacy `nvstVideo` default
   is **AEAD_AES_256_GCM with the 8-byte tag** — that profile is the
   production expectation, CM128-HMAC80 the classic GameStream fallback.
5. **Backend gating**: bare builds (no `KN_TLS_BACKEND_MBEDTLS`) compile
   honest `KN_SRTP_ERR_NO_BACKEND`/`KN_RX_ERR_NO_CIPHER` stubs — never
   fake crypto; `kn_srtp_backend_{256,gcm}()` report capability.

Cross-verification: `tools/rx_reference.py` implements an INDEPENDENT
pure-Python AES-256/GHASH/GCM/KDF256 (self-proved against FIPS-197 C.3,
the GCM spec case-1/case-2 vectors and the H = E(K0,0) constant, and
cross-checked against srtp_reference's proved AES-128) and emits session
derivations + protected datagrams for all six profile/tag combinations;
the C side (mbedTLS) must reproduce them byte-for-byte in both
directions. All pass in the sandbox CMake build (test_knrx 209 checks,
`rx.vectors` 153 from the shipped binary); the bare build checks the
honest refusal instead (126/184). The `--replay-capture` E/F matrix
exercises CM128 (bare + cmake) and GCM256 (cmake) end-to-end.

SRTCP-GCM (Receiver Reports, nvst.rs:4346-4410) stays stage 3 with the
SCTP/RTCP transport decision.

## Phase-1 checklist (kept for provenance)

1. Vendor a vetted SHA-1/HMAC-SHA1 (or mbedTLS's) — never hand-roll.
2. Auth: `M = auth_tag(AAD || ciphertext || padding)` where AAD = RTP header +
   CSRC, and the ROC enters as **`be32(roc)` appended after the payload**
   (`nvst.rs:2820-2872` — the `be32(roc)` placement is pitfall material).
3. Replay window 2048 packets + ROC estimation from seq (`nvst.rs:2635-2705`).
4. SRTCP: E-bit + index handling (`nvst.rs:4357-4372`).
5. KAT: RFC 3711 B.2/B.3 do not cover HMAC — use libsrtp-compatible vectors
   or record `UNVERIFIED` until a capture exists (§10.6 rule: name unknowns).
