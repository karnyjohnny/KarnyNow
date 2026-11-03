
#!/usr/bin/env python3
"""srtp_reference.py — independent Python reference for SRTP crypto phase 1
and generator of the C known-answer vectors (BOOTSTRAP §7.9 "srtp.vectors").

The AES-128 core here is pure Python (test scaffolding ONLY — production
crypto runs on the vendored vetted tiny-AES-c via kn_aes; see
third_party/tiny-aes-c/PROVENANCE.md). Before emitting anything, this script
PROVES its AES against constants transcribed verbatim from:
  * FIPS-197 C.1 / SP800-38A F.1.1 (AES-128 ECB blocks)
  * RFC 3711 Appendix B.2 (AES-CM keystream blocks, incl. the FEFF->FF00
    128-bit counter carry at blocks 65279-65281)
  * RFC 3711 Appendix B.3 (KDF: cipher key label 0x00, cipher salt 0x02,
    94-octet auth key 0x01 over six counter blocks with last-block truncation)
If any self-check fails, NOTHING is emitted — the vectors are then wrong, not
the code under test.

KDF/IV semantics mirror nvst.rs @ a49b124 (local copy: artifacts/nvst-extract/):
  derive_aes128_cm_key  2888-2900   (iv[7] ^= label; AES-CM keystream)
  srtp_aes_cm_iv        2902-2913   (salt14; ^=ssrc@[4..8]; ^=index48@[8..14])
  srtp_gcm_iv           2915-2926   (RFC 7714 §9.1)
  srtcp_gcm_iv          2929-2939   (RFC 7714 §9.2)
  labels                88-95       (0x00/0x01/0x02 SRTP, 0x03/0x05 SRTCP)

SRTCP-over-CM semantics (phase 2b, slice 5l) mirror str0m rtp/srtp.rs:
  labels                14-20       (0x03/0x04/0x05 — nvst is GCM-only for
                                     SRTCP and never derives label 0x04)
  protect_rtcp          464-508     (CM branch: 8-octet plaintext prefix,
                                     E||index BE32, tag over everything)
  unprotect_rtcp        562-620     (tag FIRST, E=0 tolerated)
  rtp_iv                crypto/srtp.rs:~220 (BE64 index at [6..14) — for a
                                     31-bit SRTCP index byte-identical to
                                     RFC 3711 §4.1.1 (i*2^16) at [10..14))
Self-check proves the IV two ways (RFC integer formula + str0m byte literal)
and round-trips every golden before emitting.

Usage:  python3 tools/srtp_reference.py
Emits:  streamer/src/knsrtp_kat_vectors.h
"""
import hashlib
import hmac as pyhmac
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "streamer", "src", "knsrtp_kat_vectors.h")

# ---------------------------------------------------------------- AES-128

def _make_sbox():
    # AES's field polynomial 0x11B is irreducible but NOT primitive: the
    # element 0x02 has order 51, so log/exp tables must be built with the
    # primitive generator 0x03. (Contrast: the FEC field uses 0x11D, where
    # 0x02 IS a generator — see tools/fec_reference.py. Mixing the two is a
    # classic trap; the FIPS-197/RFC-3711 checks below are the guard.)
    def mul11b(a, b):
        r = 0
        while b:
            if b & 1:
                r ^= a
            a <<= 1
            if a & 0x100:
                a ^= 0x11B
            b >>= 1
        return r

    exp = [0] * 512
    log = [0] * 256
    v = 1
    for i in range(255):
        exp[i] = v
        log[v] = i
        v = mul11b(v, 3)
    for i in range(255, 512):
        exp[i] = exp[i - 255]

    def inv(a):
        return 0 if a == 0 else exp[255 - log[a]]

    sbox = []
    for x in range(256):
        y = inv(x)
        r = 0
        for i in range(8):
            b = (((y >> i) & 1) ^ ((y >> ((i + 4) % 8)) & 1) ^
                 ((y >> ((i + 5) % 8)) & 1) ^ ((y >> ((i + 6) % 8)) & 1) ^
                 ((y >> ((i + 7) % 8)) & 1) ^ ((0x63 >> i) & 1))
            r |= b << i
        sbox.append(r)
    assert len(set(sbox)) == 256, "sbox must be a permutation"
    return sbox


SBOX = _make_sbox()


def _xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1) & 0xFF


_KEY_CACHE = {}


def _expand_key(key):
    w = _KEY_CACHE.get(key)
    if w is not None:
        return w
    w = [list(key[4 * i:4 * i + 4]) for i in range(4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        w.append([w[i - 4][j] ^ t[j] for j in range(4)])
    _KEY_CACHE[key] = w
    return w


def aes128_encrypt(key, block):
    """AES-128 single-block encrypt. key/block: bytes(16). Returns bytes(16)."""
    w = _expand_key(bytes(key))
    s = [[block[c * 4 + r] for c in range(4)] for r in range(4)]

    def add_rk(rnd):
        for c in range(4):
            for r in range(4):
                s[r][c] ^= w[rnd * 4 + c][r]

    add_rk(0)
    for rnd in range(1, 10):
        for r in range(4):
            for c in range(4):
                s[r][c] = SBOX[s[r][c]]
        for r in range(1, 4):
            s[r] = s[r][r:] + s[r][:r]
        for c in range(4):
            a = [s[r][c] for r in range(4)]
            t = a[0] ^ a[1] ^ a[2] ^ a[3]
            for r in range(4):
                s[r][c] = a[r] ^ t ^ _xtime(a[r] ^ a[(r + 1) % 4])
        add_rk(rnd)
    for r in range(4):
        for c in range(4):
            s[r][c] = SBOX[s[r][c]]
    for r in range(1, 4):
        s[r] = s[r][r:] + s[r][:r]
    add_rk(10)
    return bytes(s[r][c] for c in range(4) for r in range(4))


def _be128_add(block, n):
    return ((int.from_bytes(block, "big") + n) % (1 << 128)).to_bytes(16, "big")


# ------------------------------------------------------- SRTP primitives

def kdf128(master_key, master_salt, label, out_len):
    """RFC 3711 §4.3.1 via nvst.rs:2888-2900 (initial derivation, kdr=0)."""
    iv = bytearray(16)
    iv[:len(master_salt)] = master_salt
    iv[7] ^= label
    out = bytearray()
    ctr = bytes(iv)
    while len(out) < out_len:
        out += aes128_encrypt(master_key, ctr)
        ctr = _be128_add(ctr, 1)
    return bytes(out[:out_len])


def cm_keystream_block(key, iv, block_index):
    return aes128_encrypt(key, _be128_add(iv, block_index))


def packet_index(roc, seq):
    return ((roc & 0xFFFFFFFF) << 16) | (seq & 0xFFFF)


def srtp_aes_cm_iv(session_salt14, ssrc, pidx):
    """nvst.rs:2902-2913."""
    iv = bytearray(16)
    iv[:14] = session_salt14
    for i in range(4):
        iv[4 + i] ^= (ssrc >> (24 - 8 * i)) & 0xFF
    for i in range(6):
        iv[8 + i] ^= (pidx >> (40 - 8 * i)) & 0xFF
    return bytes(iv)


def srtp_gcm_iv(session_salt12, ssrc, roc, seq):
    """nvst.rs:2915-2926 (RFC 7714 §9.1)."""
    iv = bytearray(session_salt12)
    for i in range(4):
        iv[2 + i] ^= (ssrc >> (24 - 8 * i)) & 0xFF
        iv[6 + i] ^= (roc >> (24 - 8 * i)) & 0xFF
    iv[10] ^= (seq >> 8) & 0xFF
    iv[11] ^= seq & 0xFF
    return bytes(iv)


def srtcp_gcm_iv(session_salt12, ssrc, srtcp_index):
    """nvst.rs:2929-2939 (RFC 7714 §9.2)."""
    iv = bytearray(session_salt12)
    for i in range(4):
        iv[2 + i] ^= (ssrc >> (24 - 8 * i)) & 0xFF
        iv[6 + i] ^= (srtcp_index >> (24 - 8 * i)) & 0xFF
    return bytes(iv)


# ------------------------------------------------------------- self-checks

H = bytes.fromhex


def self_check():
    assert SBOX[0x00] == 0x63 and SBOX[0x01] == 0x7C, "sbox anchors"
    assert SBOX[0x7F] == 0xD2 and SBOX[0xFF] == 0x16, "sbox anchors 2"

    # FIPS-197 C.1
    assert aes128_encrypt(H("000102030405060708090a0b0c0d0e0f"),
                          H("00112233445566778899aabbccddeeff")) == \
        H("69c4e0d86a7b0430d8cdb78070b4c55a"), "FIPS-197 C.1"
    # SP800-38A F.1.1 block 1
    assert aes128_encrypt(H("2b7e151628aed2a6abf7158809cf4f3c"),
                          H("6bc1bee22e409f96e93d7e117393172a")) == \
        H("3ad77bb40d7a3660a89ecaf32466ef97"), "SP800-38A F.1.1"

    # RFC 3711 B.2 -- AES-CM keystream (six blocks incl. the counter carry)
    b2_key = H("2B7E151628AED2A6ABF7158809CF4F3C")
    b2_off = H("F0F1F2F3F4F5F6F7F8F9FAFBFCFD0000")
    b2 = [
        (0,     "E03EAD0935C95E80E166B16DD92B4EB4"),
        (1,     "D23513162B02D0F72A43A2FE4A5F97AB"),
        (2,     "41E95B3BB0A2E8DD477901E4FCA894C0"),
        (65279, "EC8CDF7398607CB0F2D21675EA9EA1E4"),
        (65280, "362B7C3C6773516318A077D7FC5073AE"),
        (65281, "6A2CC3787889374FBEB4C81B17BA6C44"),
    ]
    for idx, ks in b2:
        got = cm_keystream_block(b2_key, b2_off, idx)
        assert got == H(ks), "B.2 block %d: %s != %s" % (idx, got.hex(), ks)
    # the offset+65279 counter must be exactly ...FCFDFEFF (carry check)
    assert _be128_add(b2_off, 65279) == H("F0F1F2F3F4F5F6F7F8F9FAFBFCFDFEFF")
    assert _be128_add(b2_off, 65280) == H("F0F1F2F3F4F5F6F7F8F9FAFBFCFDFF00")

    # RFC 3711 B.3 -- KDF
    master = H("E1F97A0D3E018BE0D64FA32C06DE4139")
    salt = H("0EC675AD498AFEEBB6960B3AABE6")
    assert kdf128(master, salt, 0x00, 16) == H("C61E7A93744F39EE10734AFE3FF7A087"), "B.3 cipher key"
    assert kdf128(master, salt, 0x02, 16) == H("30CBBC08863D8C85D49DB34A9AE17AC6"), "B.3 salt block0"
    assert kdf128(master, salt, 0x02, 14) == H("30CBBC08863D8C85D49DB34A9AE1"), "B.3 cipher salt"
    auth94 = (H("CEBE321F6FF7716B6FD4AB49AF256A15") + H("6D38BAA48F0A0ACF3C34E2359E6CDBCE") +
              H("E049646C43D9327AD175578EF7227098") + H("6371C10C9A369AC2F94A8C5FBCDDDC25") +
              H("6D6E919A48B610EF17C2041E47403576") + H("6B68642C59BBFC2F34DB60DBDFB2"))
    assert len(auth94) == 94
    assert kdf128(master, salt, 0x01, 94) == auth94, "B.3 auth key (94 octets)"
    print("self-check OK (sbox, FIPS-197, SP800-38A, RFC3711 B.2 x6, B.3 KDF)")
    return master, salt, b2_key, b2_off, b2, auth94



# ------------------------------------------------- phase 2: SHA-1/HMAC/SRTP

RFC3174_VECTORS = [
    ("abc", b"abc", 1, "A9993E364706816ABA3E25717850C26C9CD0D89D"),
    ("t2", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
     "84983E441C3BD26EBAAE4AA1F95129E5E54670F1"),
    ("t3", b"a", 1000000, "34AA973CD4C4DAA4F61EEB2BDBAD27316534016F"),
    ("t4", b"01234567012345670123456701234567" * 2, 10,   # TEST4a TEST4b, x10
     "DEA356A2CDDD90C7A7ECEDC5EBB563934F460452"),
]

RFC2202_CASES = [
    (bytes.fromhex("0b" * 20), b"Hi There",
     "b617318655057264e28bc0b6fb378c8ef146be00"),
    (b"Jefe", b"what do ya want for nothing?",
     "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"),
    (bytes.fromhex("aa" * 20), bytes.fromhex("dd" * 50),
     "125d7342b9ac11cd91a39af48aa17b4f63f175d3"),
    (bytes.fromhex("0102030405060708090a0b0c0d0e0f10111213141516171819"),
     bytes.fromhex("cd" * 50), "4c9007f4026250c6bc8414f9bf50c86c2d7235da"),
    (bytes.fromhex("0c" * 20), b"Test With Truncation",
     "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04"),
    (bytes.fromhex("aa" * 80),
     b"Test Using Larger Than Block-Size Key - Hash Key First",
     "aa4ae5e15272d00e95705637ce8a3b55ed402112"),
    (bytes.fromhex("aa" * 80),
     b"Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data",
     "e8e99d0f45237d786d6bbaa7965c7808bbff1a91"),
]


class ReplayWindow(object):
    """Line-by-line port of nvst.rs:3121-3211 (ReplayWindow)."""
    WINDOW = 2048
    WORDS = 32

    def __init__(self):
        self.highest = None
        self.seen = [0] * self.WORDS

    def guess(self, seq):
        if self.highest is None:
            return (0, seq)
        roc = self.highest >> 16
        hs = self.highest & 0xFFFF
        delta = seq - hs                      # i32 semantics (values < 2^16)
        if delta < -32768:
            if roc == 0xFFFFFFFF:
                return (-1, 0)
            groc = roc + 1
        elif delta > 32768:
            if roc == 0:
                return (-1, 0)
            groc = roc - 1
        else:
            groc = roc
        return (0, (groc << 16) | seq)

    def check(self, idx):
        if self.highest is None:
            return 0
        if idx > self.highest:
            return 0
        age = self.highest - idx
        if age >= self.WINDOW:
            return 1
        if (self.seen[age // 64] >> (age % 64)) & 1:
            return 1
        return 0

    def commit(self, idx):
        if self.highest is None:
            self.highest = idx
            self.seen = [0] * self.WORDS
            self.seen[0] = 1
            return
        if idx > self.highest:
            adv = idx - self.highest
            if adv >= self.WINDOW:
                self.seen = [0] * self.WORDS
            else:
                ws, bs = adv // 64, adv % 64
                new = [0] * self.WORDS
                for dst in range(self.WORDS - 1, -1, -1):
                    if dst < ws:
                        new[dst] = 0
                    else:
                        srcv = (self.seen[dst - ws] << bs) & 0xFFFFFFFFFFFFFFFF
                        if bs and dst - ws > 0:
                            srcv |= self.seen[dst - ws - 1] >> (64 - bs)
                        new[dst] = srcv
                self.seen = new
            self.seen[0] |= 1
            self.highest = idx
        else:
            age = self.highest - idx
            if age < self.WINDOW:
                self.seen[age // 64] |= 1 << (age % 64)


def replay_op_stream():
    """Deterministic operation script exercising every branch of the window."""
    rw = ReplayWindow()
    ops = []

    def op(seq):
        rc, idx = rw.guess(seq)
        if rc == -1:
            ops.append((seq, -1, 0, 0))
            return
        cr = rw.check(idx)
        if cr == 0:
            rw.commit(idx)
        ops.append((seq, 0, idx, cr))

    op(1000)                                 # first packet (no history)
    for s in range(1001, 1051):
        op(s)                                # 50 sequential
    op(1025)                                 # duplicate -> reject
    op(60000)                                # delta>32768 with roc=0 -> underflow rc=-1
    op(1049)                                 # duplicate of last
    op(1055)                                 # gap ahead
    op(1052)                                 # late fill (in window)
    op(1052)                                 # duplicate of late fill -> reject
    for s in range(1056, 1080):
        op(s)
    op(20000)                                # far jump (advance >= window -> clear)
    op(1000)                                 # ancient -> reject (age)
    op(19999)                                # age 1, unseen -> accept
    for s in range(65530, 65536):
        op(s)                                # approach the 16-bit wrap
    op(0)                                    # wrap: delta<-32768 -> roc+1 -> accept
    op(65535)                                # pre-wrap duplicate (roc-1 guess) -> reject
    op(1)
    op(2)                                    # highest = 65538 (roc 1)
    op(63491)                                # delta>32768 -> roc0, age 2047 -> accept
    op(63490)                                # age 2048 -> reject (window edge)
    return ops


def p2_vectors():
    """SRTP protect goldens + replay op table."""
    master = H("E1F97A0D3E018BE0D64FA32C06DE4139")
    salt = H("0EC675AD498AFEEBB6960B3AABE6")
    enc = kdf128(master, salt, 0x00, 16)
    csalt = kdf128(master, salt, 0x02, 14)
    auth = kdf128(master, salt, 0x01, 20)
    ssrc = 0xDEADBEEF
    seq = 1
    roc = 0
    pidx = packet_index(roc, seq)
    header = (bytes([0x80, 0x60, (seq >> 8) & 0xFF, seq & 0xFF]) +
              (0x11223344).to_bytes(4, "big") + ssrc.to_bytes(4, "big"))
    payload = bytes(((i * 37 + 11) & 0xFF) for i in range(100))
    plain = header + payload                       # 112 bytes, payload_offset 12
    assert len(plain) == 112 and len(header) == 12
    iv = srtp_aes_cm_iv(csalt, ssrc, pidx)
    ks = b"".join(cm_keystream_block(enc, iv, i) for i in range(7))[:100]
    ct = plain[:12] + bytes(a ^ b for a, b in zip(plain[12:], ks))
    mac_in = ct + roc.to_bytes(4, "big")
    tag = pyhmac.new(auth, mac_in, hashlib.sha1).digest()
    protected10 = ct + tag[:10]
    protected4 = ct + tag[:4]
    ops = replay_op_stream()
    return dict(enc=enc, csalt=csalt, auth=auth, ssrc=ssrc, seq=seq, roc=roc,
                pidx=pidx, plain=plain, protected10=protected10,
                protected4=protected4, ops=ops)


def self_check_p2():
    # RFC 3174 SHA-1 (hashlib = trusted oracle; constants verbatim from RFC text)
    for name, data, count, expect in RFC3174_VECTORS:
        h = hashlib.sha1(data * count).hexdigest().upper()
        assert h == expect, "RFC3174 %s: %s != %s" % (name, h, expect)
    # RFC 2202 HMAC-SHA1
    for i, (key, data, expect) in enumerate(RFC2202_CASES, 1):
        h = pyhmac.new(key, data, hashlib.sha1).hexdigest()
        assert h == expect, "RFC2202 case %d: %s != %s" % (i, h, expect)
    v = p2_vectors()
    # session material must equal the phase-1 KDF goldens (B.3 constants)
    assert v["enc"] == H("C61E7A93744F39EE10734AFE3FF7A087")
    assert v["csalt"] == H("30CBBC08863D8C85D49DB34A9AE1")
    # replay stream sanity: contains accepts, rejects and the underflow case
    kinds = set(o[1] for o in v["ops"])
    assert -1 in kinds and 0 in kinds
    assert any(o[3] == 1 for o in v["ops"]) and any(o[3] == 0 for o in v["ops"])
    print("self-check p2 OK (RFC3174 x4, RFC2202 x7, protect goldens, %d replay ops)"
          % len(v["ops"]))
    return v


# ------------------------------------------------- phase 2b: SRTCP over CM
# Provenance: RFC 3711 §4.1.1 (IV = (k_s*2^16) XOR (SSRC*2^64) XOR (i*2^16),
# i = the 31-bit SRTCP index; k_e/k_s replaced by the SRTCP session pair) and
# §3.4/§4.2.1 (first 8 octets plaintext; tag over the whole packet INCLUDING
# the E||index field, NO roc). Cross-checked line-by-line against str0m
# rtp/srtp.rs:464-620 (protect_rtcp/unprotect_rtcp CM branch, labels 0x03 /
# 0x04 / 0x05 at rtp/srtp.rs:18-20) — str0m builds the same IV by calling
# rtp_iv(salt14, ssrc, srtcp_index as u64): for a 31-bit index the BE64 XOR
# at bytes [6..14) is byte-identical to BE32 at [10..14).

def srtcp_cm_iv(session_salt14, ssrc, srtcp_index):
    """RFC 3711 §4.1.1, byte-level form (== str0m rtp_iv for index < 2^31)."""
    assert 0 <= srtcp_index < (1 << 31), "SRTCP index is 31-bit"
    iv = bytearray(16)
    iv[:14] = session_salt14                  # k_s * 2^16
    for i in range(4):
        iv[4 + i] ^= (ssrc >> (24 - 8 * i)) & 0xFF          # SSRC * 2^64
    for i in range(4):
        iv[10 + i] ^= (srtcp_index >> (24 - 8 * i)) & 0xFF  # i * 2^16
    return bytes(iv)


def srtcp_cm_keystream(enc_key, iv, nbytes):
    return b"".join(cm_keystream_block(enc_key, iv, i)
                    for i in range((nbytes + 15) // 16))[:nbytes]


def srtcp_cm_protect(plain, srtcp_index, enc_key, auth_key, salt, tag_len=10,
                     encrypt=True):
    """str0m rtp/srtp.rs:464-508 protect_rtcp, CM branch.

    encrypt=False builds the E=0 (plaintext-but-authenticated) variant that
    str0m unprotect_rtcp tolerates (rtp/srtp.rs:598-601) — no real sender
    produces it, but the C unprotect must handle it identically."""
    assert len(plain) >= 12, "real RTCP packets are >= 12 octets (PLI floor)"
    ssrc = int.from_bytes(plain[4:8], "big")
    if encrypt:
        iv = srtcp_cm_iv(salt, ssrc, srtcp_index)
        ks = srtcp_cm_keystream(enc_key, iv, len(plain) - 8)
        body = plain[:8] + bytes(a ^ b for a, b in zip(plain[8:], ks))
    else:
        body = bytes(plain)
    e_and_si = ((0x80000000 if encrypt else 0) | srtcp_index).to_bytes(4, "big")
    body += e_and_si
    tag = pyhmac.new(auth_key, body, hashlib.sha1).digest()[:tag_len]
    return body + tag


def srtcp_cm_unprotect(protected, enc_key, auth_key, salt, tag_len=10):
    """str0m rtp/srtp.rs:562-620 unprotect_rtcp, CM branch. Returns
    (plain, srtcp_index, e) or None on AuthenticationFailed. Tag first,
    decrypt only when E=1 — the C implementation must match this order."""
    if len(protected) < 8 + 4 + tag_len:
        return None
    hmac_start = len(protected) - tag_len
    tag = pyhmac.new(auth_key, protected[:hmac_start], hashlib.sha1).digest()
    if tag[:tag_len] != protected[hmac_start:]:
        return None
    idx_start = hmac_start - 4
    e_and_si = int.from_bytes(protected[idx_start:hmac_start], "big")
    e = (e_and_si >> 31) & 1
    srtcp_index = e_and_si & 0x7FFFFFFF
    plain = protected[:idx_start]
    if e:
        ssrc = int.from_bytes(plain[4:8], "big")
        iv = srtcp_cm_iv(salt, ssrc, srtcp_index)
        ks = srtcp_cm_keystream(enc_key, iv, len(plain) - 8)
        plain = plain[:8] + bytes(a ^ b for a, b in zip(plain[8:], ks))
    return (plain, srtcp_index, e)


def srtcp_plains():
    """Realistic probe-shaped RTCP packets (crypto is content-agnostic, but
    goldens should look like the wire). Sender SSRC = the probe's
    SESS_RTCP_SENDER_SSRC 0x4F4E4F57 ("ONOW")."""
    sender = 0x4F4E4F57

    # 32B Receiver Report: RC=1, PT 201, media ssrc 1 (audio), ext_seq
    # 0x910, jitter 40, LSR/DLSR from a remembered SR.
    rr = bytearray(32)
    rr[0], rr[1] = 0x81, 0xC9
    rr[2:4] = (7).to_bytes(2, "big")
    rr[4:8] = sender.to_bytes(4, "big")
    rr[8:12] = (1).to_bytes(4, "big")
    rr[13:16] = (0).to_bytes(3, "big")          # frac_lost 0, cum_lost 0
    rr[16:20] = (0x910).to_bytes(4, "big")
    rr[20:24] = (40).to_bytes(4, "big")
    rr[24:28] = (0x11223344).to_bytes(4, "big")
    rr[28:32] = (0x1234).to_bytes(4, "big")

    # 12B PLI: FMT=1, PT 206, media ssrc 2 (video).
    pli = bytes([0x81, 0xCE, 0x00, 0x02]) + sender.to_bytes(4, "big") + \
        (2).to_bytes(4, "big")

    # 32B TWCC feedback (draft-holmer-rmcat-transport-wide-cc-extensions-01):
    # FMT=15, PT 205, P-bit set, media ssrc 2, base_seq 1, count 9,
    # ref_time 1, fb_count 0, one Run-Length chunk (9 received), nine small
    # deltas (+1 ms quantized), 1 pad byte.
    tw = bytearray(32)
    tw[0], tw[1] = 0xAF, 0xCD
    tw[2:4] = (7).to_bytes(2, "big")
    tw[4:8] = sender.to_bytes(4, "big")
    tw[8:12] = (2).to_bytes(4, "big")
    tw[12:14] = (1).to_bytes(2, "big")           # base_seq
    tw[14:16] = (9).to_bytes(2, "big")           # packet status count
    tw[16:19] = (1).to_bytes(3, "big")           # reference time (24-bit)
    tw[19] = 0                                    # fb packet count
    tw[20:22] = (0x0009).to_bytes(2, "big")      # Run Length: 9x received
    tw[22:31] = bytes([0x04] * 9)                # small deltas (+1ms)
    tw[31] = 1                                    # RFC 3550 pad count

    assert len(bytes(rr)) == 32 and len(pli) == 12 and len(bytes(tw)) == 32
    return bytes(rr), pli, bytes(tw)


def srtcp_vectors():
    """SRTCP-CM protect/unprotect goldens from the RFC 3711 B.3 master."""
    master = H("E1F97A0D3E018BE0D64FA32C06DE4139")
    salt = H("0EC675AD498AFEEBB6960B3AABE6")
    renc = kdf128(master, salt, 0x03, 16)
    rsalt = kdf128(master, salt, 0x05, 14)
    rauth = kdf128(master, salt, 0x04, 20)
    rr, pli, tw = srtcp_plains()

    # (name, plain, index, tag_len, encrypt)
    specs = [
        ("rr32",   rr,  1,          10, True),
        ("pli12",  pli, 2,          10, True),
        ("twcc32", tw,  0,          10, True),
        ("maxidx", rr,  0x7FFFFFFF, 10, True),   # E||idx = FF FF FF FF
        ("tag4",   pli, 3,           4, True),   # HMAC_SHA1_32 branch
        ("e0",     tw,  5,          10, False),  # plaintext-but-authenticated
    ]
    cases = []
    for name, plain, index, tag_len, enc in specs:
        protected = srtcp_cm_protect(plain, index, renc, rauth, rsalt,
                                     tag_len, enc)
        cases.append(dict(name=name, plain=plain, index=index,
                          tag_len=tag_len, e=1 if enc else 0,
                          protected=protected))
    return dict(renc=renc, rsalt=rsalt, rauth=rauth, cases=cases)


def self_check_srtcp():
    v = srtcp_vectors()
    renc, rsalt, rauth = v["renc"], v["rsalt"], v["rauth"]

    # the enc key / salt must equal the already-emitted phase-1 goldens
    # (same KDF, labels 0x03 / 0x05 — emit() re-derives from the B.3 master)
    master = H("E1F97A0D3E018BE0D64FA32C06DE4139")
    salt = H("0EC675AD498AFEEBB6960B3AABE6")
    assert renc == kdf128(master, salt, 0x03, 16)
    assert rsalt == kdf128(master, salt, 0x05, 14)
    assert rauth == kdf128(master, salt, 0x04, 20)

    for c in v["cases"]:
        plain, protected = c["plain"], c["protected"]
        index, tag_len = c["index"], c["tag_len"]
        ssrc = int.from_bytes(plain[4:8], "big")

        # (1) RFC 3711 §4.1.1 formula as 128-bit INTEGER arithmetic — an
        # independent formulation of the byte-level builder above.
        iv_bytes = srtcp_cm_iv(rsalt, ssrc, index)
        iv_int = ((int.from_bytes(rsalt, "big") << 16) ^
                  (ssrc << 64) ^ (index << 16))
        assert iv_bytes == iv_int.to_bytes(16, "big"), "IV formula: %s" % c["name"]

        # (2) str0m rtp_iv literal: BE64(index as u64) XORed at bytes
        # [6..14), ssrc COPIED at [4..8), salt XORed over [0..14).
        siv = bytearray(16)
        siv[4:8] = ssrc.to_bytes(4, "big")
        be64 = index.to_bytes(8, "big")
        for i in range(8):
            siv[i + 6] ^= be64[i]
        for i in range(14):
            siv[i] ^= rsalt[i]
        assert bytes(siv) == iv_bytes, "str0m rtp_iv: %s" % c["name"]

        # (3) structural invariants: 8-octet plaintext prefix (E=1 only),
        # exact lengths, E bit placement.
        if c["e"]:
            assert protected[:8] == plain[:8], "prefix: %s" % c["name"]
        assert len(protected) == len(plain) + 4 + tag_len
        e_and_si = int.from_bytes(protected[-4 - tag_len:-tag_len], "big")
        assert e_and_si == ((0x80000000 if c["e"] else 0) | index)

        # (4) round trip through the python unprotect mirror; and a
        # one-bit tag corruption must fail (sha1_hmac oracle).
        rt = srtcp_cm_unprotect(protected, renc, rauth, rsalt, tag_len)
        assert rt == (plain, index, c["e"]), "round trip: %s" % c["name"]
        bad = bytearray(protected)
        bad[-1] ^= 0x01
        assert srtcp_cm_unprotect(bytes(bad), renc, rauth, rsalt,
                                  tag_len) is None, "bad tag: %s" % c["name"]

    # IV golden input sanity (emitted alongside the existing IV builders)
    iv = srtcp_cm_iv(bytes(range(14)), 0xDEADBEEF, 0x09ABCDEF)
    assert len(iv) == 16 and iv[14:] == bytes(2)

    print("self-check srtcp OK (%d cases: RFC integer formula, str0m rtp_iv, "
          "round trips, corruption rejects)" % len(v["cases"]))
    return v


# ----------------------------------------------------------------- emit

def c_arr(name, data, per_line=16):
    rows = []
    for i in range(0, len(data), per_line):
        rows.append("    " + ", ".join("0x%02x" % b for b in data[i:i + per_line]) + ",")
    return "static const unsigned char %s[%d] = {\n%s\n};" % (name, len(data), "\n".join(rows))


def emit(master, salt, b2_key, b2_off, b2, auth94, p2, sc):
    P = []
    P.append("""/* knsrtp_kat_vectors.h -- GENERATED FILE, do not edit by hand.
 * Generator: python3 tools/srtp_reference.py   (run from the repo root)
 *
 * Known-answer vectors for SRTP crypto phase 1 (BOOTSTRAP sec. 7.9
 * "srtp.vectors"). Sources, in precedence order:
 *   - RFC 3711 Appendix B.2 (AES-CM) and B.3 (KDF): constants transcribed
 *     VERBATIM from the RFC text; the generator self-checks its own AES
 *     against them before emitting anything.
 *   - FIPS-197 C.1 / SP800-38A F.1.1: AES-128 block goldens.
 *   - nvst.rs @ a49b124 lines 88-95, 2888-2939: labels, KDF and IV layouts.
 *     Goldens for the nvst-specific items (SRTCP derivations, IV builders,
 *     20-byte auth key) are computed by the Python reference -- they are
 *     cross-implementation vectors, not RFC text.
 * If the C code disagrees with an RFC-sourced vector, the C code is wrong.
 * Never "fix" a vector to match code.
 */
#ifndef KN_SRTP_KAT_VECTORS_H
#define KN_SRTP_KAT_VECTORS_H
""")

    P.append("/* AES-128 block goldens */")
    P.append(c_arr("k_kat_aes_fips197_key", H("000102030405060708090a0b0c0d0e0f")))
    P.append(c_arr("k_kat_aes_fips197_pt", H("00112233445566778899aabbccddeeff")))
    P.append(c_arr("k_kat_aes_fips197_ct", H("69c4e0d86a7b0430d8cdb78070b4c55a")))
    P.append(c_arr("k_kat_aes_sp80038a_key", H("2b7e151628aed2a6abf7158809cf4f3c")))
    P.append(c_arr("k_kat_aes_sp80038a_pt", H("6bc1bee22e409f96e93d7e117393172a")))
    P.append(c_arr("k_kat_aes_sp80038a_ct", H("3ad77bb40d7a3660a89ecaf32466ef97")))

    P.append("""/* RFC 3711 B.2 -- AES-CM keystream: block i = E(key, offset + i),
 * 128-bit counter; indices 65279-65281 exercise the FEFF->FF00 carry. */""")
    P.append(c_arr("k_kat_b2_key", b2_key))
    P.append(c_arr("k_kat_b2_offset", b2_off))
    idxs = ", ".join("%du" % i for i, _ in b2)
    P.append("static const unsigned int k_kat_b2_idx[6] = { %s };" % idxs)
    rows = ",\n".join("    { %s }" % ", ".join("0x%02x" % b for b in H(ks))
                      for _, ks in b2)
    P.append("static const unsigned char k_kat_b2_ks[6][16] = {\n%s\n};" % rows)

    P.append("""/* RFC 3711 B.3 -- KDF (master key + 14-octet master salt):
 * label 0x00 -> 16B cipher key; 0x02 -> 14B cipher salt (+full block0);
 * 0x01 -> 94-octet auth key over six counter blocks, last truncated to 14. */""")
    P.append(c_arr("k_kat_b3_master", master))
    P.append(c_arr("k_kat_b3_salt", salt))
    P.append(c_arr("k_kat_b3_cipher_key", kdf128(master, salt, 0x00, 16)))
    P.append(c_arr("k_kat_b3_cipher_salt", kdf128(master, salt, 0x02, 14)))
    P.append(c_arr("k_kat_b3_salt_block0", kdf128(master, salt, 0x02, 16)))
    P.append(c_arr("k_kat_b3_auth_key", auth94))
    P.append("#define K_KAT_B3_AUTH_KEY_LEN 94")

    P.append("""/* Cross-implementation goldens (Python reference, nvst.rs semantics):
 * 20-byte auth key (label 0x01, libsrtp CM-128 style), SRTCP key (0x03)
 * and SRTCP salt (0x05, 14B CM style) from the SAME B.3 master material. */""")
    P.append(c_arr("k_kat_auth20", kdf128(master, salt, 0x01, 20)))
    P.append(c_arr("k_kat_srtcp_key", kdf128(master, salt, 0x03, 16)))
    P.append(c_arr("k_kat_srtcp_salt", kdf128(master, salt, 0x05, 14)))

    # IV goldens
    salt14 = bytes(range(14))
    salt12 = bytes(range(12))
    ssrc = 0xDEADBEEF
    roc = 1
    seq = 0x1234
    pidx = packet_index(roc, seq)
    srtcp_index = 0x89ABCDEF
    assert pidx == 0x11234
    P.append("""/* IV builders (nvst.rs:2902-2939). Inputs:
 *   salt14 = 00..0d, salt12 = 00..0b, ssrc = 0xdeadbeef,
 *   roc = 1, seq = 0x1234  ->  packet_index = 0x11234
 *   srtcp_index = 0x89abcdef */""")
    P.append(c_arr("k_kat_iv_salt14", salt14))
    P.append(c_arr("k_kat_iv_salt12", salt12))
    P.append("#define K_KAT_IV_SSRC        0xdeadbeefu")
    P.append("#define K_KAT_IV_ROC         1u")
    P.append("#define K_KAT_IV_SEQ         0x1234u")
    P.append("#define K_KAT_IV_PKT_INDEX   0x11234ull")
    P.append("#define K_KAT_SRTCP_INDEX    0x89abcdefu")
    P.append(c_arr("k_kat_iv_cm", srtp_aes_cm_iv(salt14, ssrc, pidx)))
    P.append(c_arr("k_kat_iv_gcm", srtp_gcm_iv(salt12, ssrc, roc, seq)))
    P.append(c_arr("k_kat_iv_srtcp_gcm", srtcp_gcm_iv(salt12, ssrc, srtcp_index)))


    # ---------------- phase 2: SHA-1 / HMAC / SRTP auth / replay ----------------
    P.append("""/* ===== phase 2 ===== */

/* SHA-1 goldens: RFC 3174 sec. 7.3 TEST1/TEST2/TEST4 + empty message
 * (hashlib oracle). TEST3 (1,000,000 x 'a') is checked in C directly
 * against the RFC constant embedded here. */""")
    P.append(c_arr("k_kat_sha1_empty", hashlib.sha1(b"").digest()))
    for name, data, count, expect in RFC3174_VECTORS:
        if name == "abc":
            P.append(c_arr("k_kat_sha1_abc", H(expect)))
        elif name == "t2":
            P.append(c_arr("k_kat_sha1_t2", H(expect)))
            P.append(c_arr("k_kat_sha1_t2_msg", data))
        elif name == "t3":
            P.append(c_arr("k_kat_sha1_t3", H(expect)))
        elif name == "t4":
            P.append(c_arr("k_kat_sha1_t4", H(expect)))
            P.append(c_arr("k_kat_sha1_t4_msg", data))

    # RFC 2202 HMAC cases as one blob + offset tables
    blob = b""
    key_off, key_len, data_off, data_len = [], [], [], []
    for key, data, _ in RFC2202_CASES:
        key_off.append(len(blob)); key_len.append(len(key)); blob += key
        data_off.append(len(blob)); data_len.append(len(data)); blob += data
    P.append("""/* RFC 2202 sec. 3 HMAC-SHA1 cases 1-7, flattened:
 * key = blob[key_off[i] .. +key_len[i]], data = blob[data_off[i] .. +data_len[i]] */""")
    P.append(c_arr("k_kat_hmac_blob", blob, 24))
    P.append("static const unsigned short k_kat_hmac_key_off[7] = { %s };"
             % ", ".join(str(x) for x in key_off))
    P.append("static const unsigned char k_kat_hmac_key_len[7] = { %s };"
             % ", ".join(str(x) for x in key_len))
    P.append("static const unsigned short k_kat_hmac_data_off[7] = { %s };"
             % ", ".join(str(x) for x in data_off))
    P.append("static const unsigned char k_kat_hmac_data_len[7] = { %s };"
             % ", ".join(str(x) for x in data_len))
    rows = ",\n".join("    { %s }" % ", ".join("0x%02x" % b for b in H(dg))
                       for _, _, dg in RFC2202_CASES)
    P.append("static const unsigned char k_kat_hmac_digest[7][20] = {\n%s\n};" % rows)

    # SRTP protect goldens
    P.append("""/* SRTP AES_CM_128_HMAC_SHA1 protect goldens (python reference; session
 * material derived from the RFC 3711 B.3 master via the proven KDF):
 * RTP header(12B: 80 60 00 01 | ts 11223344 | ssrc deadbeef) + 100B payload
 * (i*37+1)&0xFF; packet_index = 1 (roc=0, seq=1); tag over ciphertext||be32(0).
 * Session keys == phase-1 goldens: enc = k_kat_b3_cipher_key,
 * salt = k_kat_b3_cipher_salt, auth = k_kat_auth20. */
#define K_KAT_P2_SSRC          0xdeadbeefu
#define K_KAT_P2_SEQ           1u
#define K_KAT_P2_ROC           0u
#define K_KAT_P2_PKT_INDEX     1ull
#define K_KAT_P2_PAYLOAD_OFF   12u
#define K_KAT_P2_PLAIN_LEN     112u""")
    P.append(c_arr("k_kat_p2_plain", p2["plain"]))
    P.append(c_arr("k_kat_p2_protected10", p2["protected10"]))
    P.append(c_arr("k_kat_p2_protected4", p2["protected4"]))

    # replay op table
    ops = p2["ops"]
    P.append("""/* Replay-window operation script vs the line-by-line python port of
 * nvst.rs:3121-3211. Per op: feed seq to guess(); if guess_rc==0 compare
 * index, run check(), and commit() iff check==0. Covers: cold start,
 * sequential, duplicates, late fills, far jump (window clear), ancient
 * reject, 16-bit wrap with roc+1, pre-wrap duplicate via roc-1, the
 * age==2047 accept / age==2048 reject edge, and roc-0 underflow (rc=-1). */
#define K_KAT_REPLAY_N %d""" % len(ops))
    P.append("static const unsigned short k_kat_replay_seq[%d] = { %s };"
             % (len(ops), ", ".join(str(o[0]) for o in ops)))
    P.append("static const signed char k_kat_replay_guess_rc[%d] = { %s };"
             % (len(ops), ", ".join(str(o[1]) for o in ops)))
    P.append("static const unsigned long long k_kat_replay_index[%d] = { %s };"
             % (len(ops), ", ".join("%dull" % o[2] for o in ops)))
    P.append("static const unsigned char k_kat_replay_check_rc[%d] = { %s };"
             % (len(ops), ", ".join(str(o[3]) for o in ops)))

    # ---------------- phase 2b: SRTCP over AES-CM + HMAC-SHA1 ----------------
    P.append("""/* ===== phase 2b: SRTCP-CM (slice 5l) =====

 * RFC 3711 §4.1.1/§3.4/§4.2.1 SRTCP goldens; str0m rtp/srtp.rs:464-620
 * cross-checked (labels 0x03/0x04/0x05, tag over packet INCLUDING E||index,
 * NO roc, tag verified before decryption, E=0 tolerated on unprotect).
 * Session material derives from the same B.3 master as everything above:
 *   renc  = k_kat_srtcp_key  (label 0x03, 16B)
 *   rsalt = k_kat_srtcp_salt (label 0x05, 14B)
 *   rauth = k_kat_srtcp_auth (label 0x04, 20B)  <-- NEW, str0m-only golden
 * Case table (K_KAT_SRTCP_NCASES), per case i:
 *   plain    = blob[plain_off[i] .. +plain_len[i]]
 *   expected = blob[prot_off[i]  .. +total_len[i]]   (E=1 unless e[i]==0)
 *   protect(plain, index[i], tag_len[i]) == expected
 *   unprotect(expected) == (plain, index[i], e[i])
 * Cases: rr32 (RC=1 RR, sender 4f4e4f57, idx 1), pli12 (idx 2), twcc32
 * (draft-holmer feedback, idx 0), maxidx (idx 0x7fffffff -> E||i = FFFFFFFF),
 * tag4 (HMAC_SHA1_32 branch), e0 (E=0 plaintext-but-authenticated). */
#define K_KAT_SRTCP_NCASES %d""" % len(sc["cases"]))
    P.append(c_arr("k_kat_srtcp_auth", sc["rauth"]))
    blob = b""
    plain_off, prot_off, plain_len, total_len, index_a, taglen_a, e_a, names = \
        [], [], [], [], [], [], [], []
    for c in sc["cases"]:
        plain_off.append(len(blob)); plain_len.append(len(c["plain"]))
        blob += c["plain"]
        prot_off.append(len(blob)); total_len.append(len(c["protected"]))
        blob += c["protected"]
        index_a.append(c["index"]); taglen_a.append(c["tag_len"])
        e_a.append(c["e"]); names.append(c["name"])
    P.append(c_arr("k_kat_srtcp_blob", blob, 24))
    P.append("/* case order: %s */" % ", ".join(names))
    n = len(sc["cases"])
    P.append("static const unsigned short k_kat_srtcp_plain_off[%d] = { %s };"
             % (n, ", ".join(str(x) for x in plain_off)))
    P.append("static const unsigned short k_kat_srtcp_plain_len[%d] = { %s };"
             % (n, ", ".join(str(x) for x in plain_len)))
    P.append("static const unsigned short k_kat_srtcp_prot_off[%d] = { %s };"
             % (n, ", ".join(str(x) for x in prot_off)))
    P.append("static const unsigned short k_kat_srtcp_total_len[%d] = { %s };"
             % (n, ", ".join(str(x) for x in total_len)))
    P.append("static const unsigned int k_kat_srtcp_index[%d] = { %s };"
             % (n, ", ".join("0x%xu" % x for x in index_a)))
    P.append("static const unsigned char k_kat_srtcp_tag_len[%d] = { %s };"
             % (n, ", ".join(str(x) for x in taglen_a)))
    P.append("static const unsigned char k_kat_srtcp_e[%d] = { %s };"
             % (n, ", ".join(str(x) for x in e_a)))

    # SRTCP CM IV golden (joins the existing IV-builder goldens; same
    # salt14 = 00..0d and ssrc = deadbeef, index = 0x09abcdef (31-bit safe))
    P.append("""/* SRTCP AES-CM IV (RFC 3711 §4.1.1: (k_s*2^16)^(SSRC*2^64)^(i*2^16)).
 * Inputs: salt14 = 00..0d, ssrc = 0xdeadbeef, index = 0x09abcdef. */
#define K_KAT_IV_SRTCP_CM_INDEX 0x09abcdefu""")
    P.append(c_arr("k_kat_iv_srtcp_cm",
                   srtcp_cm_iv(bytes(range(14)), 0xDEADBEEF, 0x09ABCDEF)))

    P.append("\n#endif /* KN_SRTP_KAT_VECTORS_H */")
    text = "\n\n".join(P) + "\n"
    text = text.encode("ascii", "replace").decode("ascii")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="ascii") as f:
        f.write(text)
    print("emitted %s (%d bytes)" % (OUT, len(text)))


def main():
    master, salt, b2_key, b2_off, b2, auth94 = self_check()
    p2 = self_check_p2()
    sc = self_check_srtcp()
    emit(master, salt, b2_key, b2_off, b2, auth94, p2, sc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
