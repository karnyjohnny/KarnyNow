
#!/usr/bin/env python3
"""fec_reference.py — independent Python reference for the NVST Cauchy RS-FEC
and generator of the C known-answer vectors.

Provenance (all facts verified against OpenNOW main @ a49b124, file
native/opennow-streamer/crates/opennow-streamer-transport/src/nvst.rs —
a copy lives in artifacts/nvst-extract/nvst.rs):

  * GF(2^8), primitive polynomial 0x11d, exp[512]/log[256] tables
    built exactly as gf256_tables()               (nvst.rs:3717-3742)
  * gf256_multiply / inverse / axpy / scale        (nvst.rs:3744-3783)
  * Cauchy coefficient:
        C[d][p] = inv((parity_shards + d) XOR p)   (nvst.rs:3785-3793)
    NOT a Vandermonde matrix — generic RS crates produce "mathematically
    valid but corrupt H.264" against NVIDIA parity (nvst.rs:3795-3797).
  * Encoding rule (confirmed by the reference's own unit tests,
    nvst.rs:13095-13100, 13193-13201, 13245-13256):
        parity[p] = XOR_d  C[d][p] * data[d]       (byte-wise over GF)
    with shards shorter than shard_len treated as zero-padded.
  * Reconstruction = Gauss-Jordan on the missing-data Cauchy submatrix,
    first `missing.len()` PRESENT parity rows in index order
                                             (nvst.rs:3798-3924)
  * KAT vector ported 1:1 from the reference test
    nvst_cauchy_fec_reconstructs_multiple_missing_data_shards
                                             (nvst.rs:13239-13264)

Usage:
    python3 tools/fec_reference.py            # self-check + emit C vectors
Emits:
    streamer/src/knfec_kat_vectors.h        (checked into the repo)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "streamer", "src", "knfec_kat_vectors.h")

# ------------------------------------------------------------------ GF(2^8)

EXP = [0] * 512
LOG = [0] * 256


def _build_tables():
    v = 1
    for power in range(255):
        EXP[power] = v
        LOG[v] = power
        v <<= 1
        if v & 0x100:
            v ^= 0x11D
    for power in range(255, 512):
        EXP[power] = EXP[power - 255]


_build_tables()


def gmul(a, b):
    if a == 0 or b == 0:
        return 0
    return EXP[LOG[a] + LOG[b]]


def ginv(a):
    if a == 0:
        raise ZeroDivisionError("gf256_inverse(0)")
    return EXP[255 - LOG[a]]


def axpy(dst, src, coeff):
    """dst ^= coeff * src (over the common prefix), like gf256_axpy."""
    if coeff == 0:
        return
    for i, s in enumerate(src[: len(dst)]):
        dst[i] ^= gmul(coeff, s)


def cauchy(d, p, parity_shards):
    dc = parity_shards + d
    if dc > 0xFF or p > 0xFF:
        raise ValueError("coordinate out of u8 range")
    return ginv(dc ^ p)


# ------------------------------------------------------------ encode/repair

def fec_encode(data, parity_shards, shard_len):
    n = len(data)
    out = []
    for p in range(parity_shards):
        buf = bytearray(shard_len)
        for d in range(n):
            axpy(buf, bytes(data[d][:shard_len]), cauchy(d, p, parity_shards))
        out.append(bytes(buf))
    return out


def fec_reconstruct(shards, n, parity_shards, shard_len):
    """shards: list(n + parity_shards) of bytes|None. Returns
    {data_index: recovered_bytes} for every missing data shard."""
    missing = [i for i in range(n) if shards[i] is None]
    if not missing:
        return {}
    rows = []
    for j in range(n, n + parity_shards):
        if shards[j] is not None:
            rows.append(j)
            if len(rows) == len(missing):
                break
    if len(rows) != len(missing):
        raise ValueError("insufficient parity shards")
    m = len(missing)

    matrix = [[cauchy(missing[c], rows[r] - n, parity_shards) for c in range(m)]
              for r in range(m)]
    recovered = [bytearray(shards[j][:shard_len].ljust(shard_len, b"\0"))
                 for j in rows]

    for r, j in enumerate(rows):           # subtract known data contributions
        for d in range(n):
            if shards[d] is None:
                continue
            axpy(recovered[r], bytes(shards[d][:shard_len]),
                 cauchy(d, j - n, parity_shards))

    for col in range(m):                   # Gauss-Jordan -> identity
        pivot = next(r for r in range(col, m) if matrix[r][col] != 0)
        matrix[col], matrix[pivot] = matrix[pivot], matrix[col]
        recovered[col], recovered[pivot] = recovered[pivot], recovered[col]
        inv = ginv(matrix[col][col])
        matrix[col] = [gmul(x, inv) for x in matrix[col]]
        recovered[col] = bytearray(gmul(x, inv) for x in recovered[col])
        for r in range(m):
            if r == col:
                continue
            c = matrix[r][col]
            if c == 0:
                continue
            for k in range(col, m):
                matrix[r][k] ^= gmul(c, matrix[col][k])
            for k in range(shard_len):
                recovered[r][k] ^= gmul(c, recovered[col][k])

    return {missing[i]: bytes(recovered[i]) for i in range(m)}


# ------------------------------------------------------------------- checks

def ref_data_13239():
    """The reference test's data: 5 shards x 97 bytes (nvst.rs:13240-13244)."""
    return [bytes(((s * 41 + b * 17) & 0xFF) for b in range(97)) for s in range(5)]


def self_check():
    # tables
    assert EXP[0] == 1 and EXP[1] == 2 and EXP[8] == 29, "generator wraps via 0x11d"
    for a in range(1, 256):
        assert gmul(a, ginv(a)) == 1
        assert gmul(a, 1) == a and gmul(a, 0) == 0
    # port of nvst.rs:13239 nvst_cauchy_fec_reconstructs_multiple_missing_data_shards
    data = ref_data_13239()
    parity = fec_encode(data, 2, 97)
    shards = [d for d in data] + list(parity)
    shards[1] = None
    shards[3] = None
    got = fec_reconstruct(shards, 5, 2, 97)
    assert got[1] == data[1] and got[3] == data[3], "reference vector failed"
    print("self-check OK (tables, inverses, nvst.rs:13239 vector)")
    return data, parity


# ------------------------------------------------------------------ emission

def c_bytes(name, data, per_line=16):
    lines = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    row = []
    for b in data:
        row.append("0x%02x" % b)
        if len(row) == per_line:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines.append("};")
    return "\n".join(lines)


def emit(data5, parity5):
    parts = []
    parts.append("""/* knfec_kat_vectors.h -- GENERATED FILE, do not edit by hand.
 * Generator: python3 tools/fec_reference.py   (run from the repo root)
 *
 * Known-answer vectors for the NVST Cauchy RS-FEC (BOOTSTRAP sec. 7.9
 * "fec.vectors"). Every constant below was produced by the independent
 * Python reference in tools/fec_reference.py, which is a line-by-line port
 * of nvst.rs @ a49b124 (gf256_tables 3717-3742, nvst_cauchy_coefficient
 * 3785-3793, reconstruct_nvst_cauchy_data 3798-3924) and reproduces the
 * reference implementation's own unit test nvst.rs:13239.
 *
 * If the C implementation disagrees with these vectors, the C code is wrong
 * (or the tables were mis-transcribed) — never "fix" a vector to match code.
 */
#ifndef KN_FEC_KAT_VECTORS_H
#define KN_FEC_KAT_VECTORS_H
""")

    # 1. generator-sequence golden values (first 16 powers + the 0x11d wrap)
    parts.append("/* first powers of the generator (alpha=2), 0x11d wrap at 2^8 */")
    parts.append(c_bytes("k_kat_gf_exp", bytes(EXP[:16])))
    parts.append("static const unsigned char k_kat_gf_exp_wrap = 0x%02x; /* 2^8 */" % EXP[8])

    # 2. multiplication golden pairs
    pairs = [(0x53, 0xCA), (0x02, 0x80), (0xFF, 0xFF), (0x1D, 0x02), (0x80, 0x80),
             (0x03, 0x07), (0xE7, 0x11), (0x01, 0xA5), (0xAA, 0x55), (0xFE, 0x02)]
    seed = 0x5EED
    rnd = seed
    def xorshift():
        nonlocal rnd
        rnd ^= (rnd << 13) & 0xFFFFFFFF
        rnd ^= rnd >> 17
        rnd ^= (rnd << 5) & 0xFFFFFFFF
        rnd &= 0xFFFFFFFF
        return rnd
    for _ in range(22):
        pairs.append((xorshift() & 0xFF, xorshift() & 0xFF))
    parts.append("/* gf-mul golden pairs: {a, b, a*b} */")
    rows = ",\n".join("    { 0x%02x, 0x%02x, 0x%02x }" % (a, b, gmul(a, b))
                      for a, b in pairs)
    parts.append("static const unsigned char k_kat_gf_mul[][3] = {\n%s\n};" % rows)
    parts.append("#define K_KAT_GF_MUL_COUNT %d" % len(pairs))

    # 3. inverse golden values
    invs = [0x02, 0x03, 0x1D, 0xFF, 0x80, 0x53, 0xCA]
    rows = ", ".join("0x%02x" % ginv(v) for v in invs)
    parts.append("/* gf-inv golden values, order matches k_kat_gf_inv_inputs */")
    parts.append("static const unsigned char k_kat_gf_inv_inputs[] = { %s };"
                 % ", ".join("0x%02x" % v for v in invs))
    parts.append("static const unsigned char k_kat_gf_inv[] = { %s };" % rows)

    # 4. Cauchy coefficient matrices
    for (n, p) in ((5, 2), (3, 1)):
        m = [[cauchy(d, pp, p) for pp in range(p)] for d in range(n)]
        rows = ",\n".join("    { %s }" % ", ".join("0x%02x" % c for c in row)
                          for row in m)
        parts.append("/* C[d][p] = inv((%d + d) XOR p), nvst.rs:3785-3793 */" % p)
        parts.append("static const unsigned char k_kat_cauchy_%d_%d[%d][%d] = {\n%s\n};"
                     % (n, p, n, p, rows))

    # 5. the nvst.rs:13239 vector (data rule + golden parity)
    parts.append("""/* nvst.rs:13239 reference test:
 *   data[s][b] = (unsigned char)(s * 41 + b * 17), 5 shards x 97 bytes
 *   parity_shards = 2; erase data shards 1 and 3; reconstruct.
 * Golden parity below must equal kn_fec_encode() output byte-for-byte. */
#define K_KAT_NVST_SHARDS     5
#define K_KAT_NVST_SHARD_LEN  97
#define K_KAT_NVST_PARITY     2""")
    for pi, par in enumerate(parity5):
        parts.append(c_bytes("k_kat_nvst_parity_%d" % pi, par))

    # 6. short-shard (zero-padding) vector: n=3 p=1 shard_len=8 lens 8/5/8
    data3 = [bytes(((d * 53 + b * 29) & 0xFF) for b in range(l))
             for d, l in enumerate((8, 5, 8))]
    par3 = fec_encode(data3, 1, 8)
    shards = [data3[0], None, data3[2], par3[0]]
    got3 = fec_reconstruct(shards, 3, 1, 8)
    assert got3[1] == data3[1].ljust(8, b"\0")
    parts.append("""/* zero-padding semantics (nvst.rs:3852-3855 resize, 3867-3873 known_len):
 *   n=3 p=1 shard_len=8; data lens {8,5,8}; data[d][b]=(d*53+b*29)&0xFF
 *   reconstruct of the missing shard 1 yields its 5 bytes + 3 zero bytes. */""")
    parts.append(c_bytes("k_kat_short_data0", data3[0]))
    parts.append(c_bytes("k_kat_short_data1_padded", data3[1].ljust(8, b"\0")))
    parts.append(c_bytes("k_kat_short_data2", data3[2]))
    parts.append(c_bytes("k_kat_short_parity", par3[0]))

    # 7. wider block: n=12 p=4 shard_len=64
    data12 = [bytes(((d * 53 + b * 29 + d * b * 7) & 0xFF) for b in range(64))
              for d in range(12)]
    par12 = fec_encode(data12, 4, 64)
    parts.append("""/* wider block: n=12 p=4 shard_len=64;
 *   data[d][b] = (d*53 + b*29 + d*b*7) & 0xFF.
 *   Golden parity for encode KAT; reconstruction is exercised against the
 *   generated data directly in the property test. */
#define K_KAT_WIDE_N 12
#define K_KAT_WIDE_P 4
#define K_KAT_WIDE_LEN 64""")
    for pi, par in enumerate(par12):
        parts.append(c_bytes("k_kat_wide_parity_%d" % pi, par))

    parts.append("\n#endif /* KN_FEC_KAT_VECTORS_H */")
    text = "\n\n".join(parts) + "\n"
    text = text.encode("ascii", "replace").decode("ascii")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="ascii") as f:
        f.write(text)
    print("emitted %s (%d bytes)" % (OUT, len(text)))


def emit_tables():
    """Emit streamer/src/knfec_tables.h — the const GF(2^8)/0x11d tables,
    generated from the same builder as the reference (nvst.rs:3717-3742).
    Embedding them removes any init-ordering or thread-race concern."""
    path = os.path.join(ROOT, "streamer", "src", "knfec_tables.h")
    def arr(name, values):
        rows = []
        for i in range(0, len(values), 16):
            rows.append("    " + ", ".join("0x%02x" % v for v in values[i:i + 16]) + ",")
        return "static const unsigned char %s[%d] = {\n%s\n};" % (name, len(values), "\n".join(rows))
    text = "\n".join([
        "/* knfec_tables.h -- GENERATED FILE, do not edit by hand.",
        " * Generator: python3 tools/fec_reference.py   (run from the repo root)",
        " *",
        " * GF(2^8) log/exp tables, primitive polynomial 0x11d, built exactly as",
        " * gf256_tables() in nvst.rs @ a49b124 (lines 3717-3742): exponent has 512",
        " * entries with the second half duplicated (exp[p] = exp[p-255] for p>=255),",
        " * so multiply never needs a modulo. log[0] is unused (log of 0 undefined).",
        " * test_knfec verifies these tables against the algorithmic definition and",
        " * against golden values in knfec_kat_vectors.h.",
        " */",
        "#ifndef KN_FEC_TABLES_H",
        "#define KN_FEC_TABLES_H",
        "",
        arr("kn_gf_exp_table", EXP),
        "",
        arr("kn_gf_log_table", LOG),
        "",
        "#endif /* KN_FEC_TABLES_H */",
        ""])
    text = text.encode("ascii", "replace").decode("ascii")
    with open(path, "w", encoding="ascii") as f:
        f.write(text)
    print("emitted %s (%d bytes)" % (path, len(text)))


def main():
    data5, parity5 = self_check()
    emit(data5, parity5)
    emit_tables()
    return 0


if __name__ == "__main__":
    sys.exit(main())


