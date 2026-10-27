# NVST Cauchy RS-FEC (`knfec`) — implementation notes

Status: **VERIFIED ON THE TARGET (2026-10-21, T9/manual run, w64devkit GCC
16.2.0, Win7 x64):** build warnings=0; KAT suite 125 checks — golden tables,
Cauchy matrices, the `nvst.rs:13239` port, zero-padding and wide-block parity
all PASS, and the deterministic property round-trips ran **200/200**. Three
failures in the first run were a TEST-SIDE state leak in `test_error_paths`
(a NULL `shards[2]` carried between subcases so `-2` fired before the behaviour
under test); fixed by setting full explicit state per subcase — `knfec.c` logic
was never implicated. Re-run pending in the next T9.

## Why this module exists and why it is hand-written

BOOTSTRAP §6.5 / pitfall #3: NVIDIA GameStream FEC is **systematic Cauchy
Reed-Solomon over GF(2^8) with primitive polynomial `0x11d`**. Generic RS
libraries use a Vandermonde generator matrix; feeding NVIDIA parity to one
produces *"mathematically valid but corrupt H.264 packets"* (verbatim warning
in the reference source). The decoder then just keeps requesting IDR frames —
a miserable failure mode to debug live. Hence: own implementation, own
known-answer vectors, provenance for every constant.

## Provenance (all line numbers against OpenNOW `main` @ `a49b124`)

A full copy of the reference file lives in `artifacts/nvst-extract/nvst.rs`
(14 695 lines, fetched from raw.githubusercontent.com this session).

| Fact | Source |
|---|---|
| GF(2^8), polynomial `0x11d`, exp[512]/log[256] tables (second exp half duplicated: `exp[p]=exp[p-255]`) | `nvst.rs:3717-3742` (`gf256_tables`) |
| `mul` = exp[log a + log b] with zero short-circuit; `inv(a)` = exp[255 − log a] | `nvst.rs:3744-3760` |
| `axpy` (dst ^= c·src, fast paths c==0/c==1), `scale` (c==1 no-op) | `nvst.rs:3760-3783` |
| **Cauchy coefficient** `C[d][p] = inv((P + d) XOR p)`, coordinates must fit u8 | `nvst.rs:3785-3793` (`nvst_cauchy_coefficient`) |
| Reconstruction: first `m` PRESENT parity rows in index order; subtract known data contributions (`known_len = min(len, shard_len)`, parity resized/zero-padded to `shard_len`); Gauss-Jordan with row swaps on both matrix and shard rows | `nvst.rs:3798-3924` (`reconstruct_nvst_cauchy_data`) |
| Limits: `n ≥ 1`, `p ≥ 1`, `n + p ≤ 255`, `shards.len() == n + p` | `nvst.rs:3803-3808` |
| **Encoding rule** `parity[p] = XOR_d C[d][p] · data[d]` (confirmed by the reference's own tests building parity this way) | `nvst.rs:13095-13100`, `13193-13201`, `13245-13256` |
| Block layout from the wire: `fec_word` = LE u32 at `ext[12..16]`; `repair_percent=(w>>4)&0xff`, `shard_index=(w>>12)&0x3ff`, `data_shards=(w>>22)&0x3ff`; `parity = ceil(n·percent/100)`; `block_index=(ext[11]>>4)&3`, `last_block_index=(ext[11]>>6)&3` | `nvst.rs:3668-3701` (transport layer, stage 2/5 — documented here so the parser and this module stay consistent) |
| **Metadata caveat (pitfall #4)**: reconstructed shards carry transport metadata (RTP envelope, fecInfo) that legitimately differs from the original; receivers must restore the envelope from the block template and must NOT validate fecInfo on recovered shards | `nvst.rs:4101-4107` + tests `13100-13106`, `13199-13204` |

## Modules

| File | Role |
|---|---|
| `streamer/include/kn/knfec.h` | API + contract (zero allocation, thread-safe, error codes) |
| `streamer/src/knfec.c` | GF ops, Cauchy coefficient, encoder, Gauss-Jordan reconstruction |
| `streamer/src/knfec_tables.h` | **generated** const exp/log tables (no init order, no races) |
| `streamer/tests/knfec_kat_vectors.h` | **generated** golden vectors |
| `streamer/tests/test_knfec.c` | KAT + property tests (see below) |
| `tools/fec_reference.py` | independent Python reference; self-checks against `nvst.rs:13239`; regenerates both generated headers |

Regenerate (any machine, pure stdlib):

```
python3 tools/fec_reference.py
```

## Test inventory (`test_knfec`)

1. **Tables verified, not trusted**: the test rebuilds exp/log from the
   polynomial definition and sweeps all 65 536 products against the embedded
   tables; plus golden generator powers incl. the `0x11d` wrap (`2^8 = 0x1d`),
   32 golden multiply pairs and 7 golden inverses from the Python reference.
2. **Cauchy goldens**: full coefficient matrices for (n=5,P=2) and (n=3,P=1);
   out-of-range coordinates rejected.
3. **`nvst.rs:13239` ported 1:1**: 5×97-byte shards, `data[s][b]=(s·41+b·17)`,
   2 parity shards — encoded parity must be **byte-identical** to the Python
   reference; erase shards 1+3 → reconstruct → exact match.
4. **Zero-padding semantics**: short known shard (len 5 of 8) → reconstructed
   shard equals original + zero padding (matches `resize(shard_len,0)` /
   `known_len` rules).
5. **Wide block encode KAT**: n=12, p=4, len=64, golden parity ×4.
6. **Property round-trips**: 200 deterministic iterations (xorshift32,
   seed `0xC0FFEE` — same sequence on every platform), n∈[1,16],
   percent∈[5,50], shard_len ∈ {1,7,64,1280}, every 3rd shard randomly short,
   erasures 0..min(p,n): reconstruct must be byte-exact incl. padding.
7. **Error paths**: `n+p>255` → −4; not enough parity → −2; nothing missing →
   0; scratch too small → −5; missing out buffer → −1; shard len > shard_len →
   −1; encode argument validation.

## API semantics worth remembering at integration time (stage 2/5)

- **Zero allocation.** Shard storage, parity outputs, reconstruction scratch
  (`kn_fec_scratch_bytes(p, shard_len)` = `p·shard_len + p·p`) and per-missing
  output buffers are caller-provided — R11 (2 GB RAM) and crash-resistance.
- **Shard content is the full decrypted RTP plaintext** as the reference uses
  it; after reconstruction the transport layer must overwrite the envelope
  (RTP header + GS extension) from the block template and skip fecInfo
  validation on recovered shards (pitfall #4, `nvst.rs:4101-4107`).
- `kn_fec_parity_count(n, percent)` mirrors the wire rule
  `ceil(n·percent/100)` — use it to validate incoming block layouts.
- Cauchy coordinates require `P + d ≤ 255`; blocks with `n+p > 255` must be
  rejected at parse time (the reference does the same).
- Performance: table-driven byte-at-a-time `axpy`. Worst realistic block
  (n≈48, len≈1280) reconstruction ≈ a few hundred thousand table ops —
  microseconds-to-sub-millisecond on the T7250. **Do not optimise before
  `perf` channel measurements say so** (BOOTSTRAP: measure first).
