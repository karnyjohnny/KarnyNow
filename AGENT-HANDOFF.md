# AGENT-HANDOFF — recovery document for the next implementation agent

> **FRESHNESS WARNING (added round IX, 2026-10-23): §3–§5 below are FROZEN
> HISTORY as of impl round V (2026-10-21).** The authoritative living state
> is `PROJECT-STATE.md` — read its "Bootstrapping a NEW agent session",
> "Current status", "Next steps" and the session log (rounds I–IX) FIRST.
> The build/test gate is `tests/T10_build_and_test.py` (T9 is superseded).
> §1–§2 of this file (mission summary + reading order) remain useful; add
> `docs/D3D.md` to the reading list (kn_d3d provenance + VEH crash guard).

> **If you are reading this as the acting agent:** the previous conversation was
> interrupted or lost. This file tells you exactly where the work stands, what to do
> next, and what the human must run/send. Read it fully, then read the documents
> listed in §2 in order. Do not re-derive anything that is already measured — the
> whole point of this paper trail is that you don't have to.
>
> Last updated: **2026-10-21, implementation round V** (research and impl
> rounds I–V all happened on 2026-10-20/21).

## 1. What this project is (30 seconds)

**KarnyNow** — a GeForce NOW client for a Dell Latitude E5500 (Win7 x64, Core 2 Duo
T7250, 2 GB RAM, Intel GMA 4500MHD) where neither the official NVIDIA client nor
OpenNOW runs. Architecture: **Python 3.8 TUI** (login/catalog/queue, Textual/Rich,
ConEmu required) + **plain-C streamer `knvstd`** (NVST transport, SRTP, FEC, FFmpeg
h264 software decode, D3D9 presentation, WASAPI, Raw Input), two processes talking
newline-delimited JSON over stdio. Reference: OpenNOW `main` @ `a49b124` (MIT) —
protocol knowledge only, no code. Hard rules: never bypass login/entitlements/queue,
never disable TLS verification, no own crypto (vetted libraries; the RS-FEC and SRTP
*glue* are hand-written because NVIDIA's FEC is a nonstandard Cauchy matrix — see
pitfall #3), MOCK mode never touches the network, **everything in the repo is
English** (chat with the user may be Polish — he writes Polish).

## 2. Reading order (all paths relative to this repo)

| # | Document | What it gives you |
|---|---|---|
| 1 | `docs/research/BOOTSTRAP.md` (research agent's bootstrap; shipped inside this repo) | **The map.** Mission, hardware table, architecture, decisions D1–D13 with reversal conditions, research index into OpenNOW (file:line), verified measurements (CPU budget, console, toolchain, D3D9), **§7 logging spec (12 subsections — the most important section)**, pitfalls, risk register R1–R24, stage plan + gates, F1 canonical compiler flags |
| 2 | `PROJECT-STATE.md` | **Living state.** Gate status (stage 0 COMPLETE), decisions D14–D17, risk deltas (R2/R2b/R15/R17 closed, R9 mitigated, R25/R26 new), ordered next steps, session log impl rounds I–II |
| 3 | `docs/analysis/G0.2-T4v4-RESULTS.md` | The D3D9 presentation verdict with all HRESULTs; settled D7 chain; the exact crashing call (SYSTEMMEM NV12 **fill**); the one remaining unknown (R25) |
| 4 | `docs/LOGGING.md` | `knlog`/`knring` conformance table vs BOOTSTRAP §7, API quick start, design notes (1-based levels, no TLS primitives, flush policy) |
| 5 | `docs/FEC.md` | `knfec` provenance: every rule with its `nvst.rs` line number, test inventory, integration semantics (pitfall #4 envelope restore) |
| 5b | `docs/SRTP.md` | `kn_aes`/`knsrtp` phase-1 provenance (labels, KDF, CM, IVs with line refs), KAT inventory, the 0x11B-not-primitive trap, phase-2 checklist (HMAC/replay/SRTCP/mbedTLS) |
| 6 | `artifacts/nvst-extract/nvst.rs` | Full reference transport source @ a49b124 (14 695 lines). If missing, refetch: `https://raw.githubusercontent.com/OpenCloudGaming/OpenNOW/a49b124/native/opennow-streamer/crates/opennow-streamer-transport/src/nvst.rs` |

Research-phase Polish notes (`analiza/04-NVST-SPEC-BINARIA.md` = byte-level NVST
spec, `analiza/05` = review of the user's Python code) live in the **research
workspace**, not here; their essence is in BOOTSTRAP §5–§9.
`docs/research/HANDOFF-research.md` (research handoff, Polish) contains F1/F2/F3
(flags, C library stack, reading order) — §6 of that file is still load-bearing.

## 3. Exactly where the work stands (2026-10-21, end of impl round V)

**Done and verified ON THE TARGET (Win7 x64, w64devkit GCC 16.2, cmake 4.4.3 + ninja):**
- Stage 0 gates all closed. Presentation path settled (D7): NV12 DEFAULT →
  `StretchRect(LINEAR)` → X8R8G8B8 backbuffer, ~3.0 ms incl. Present; fallbacks
  YUY2 → RGB → GDI. SYSTEMMEM NV12 is banned on this driver (fill → 0xC0000005,
  isolated to the exact call, both arches). Offscreen conversion targets refused
  (`D3DERR_INVALIDCALL`) — conversion works only into the backbuffer.
- Build gate: T9 cmake build **rc=0, warnings=0** — confirms the GCC-16
  `-Wstring-compare` fix in `knlog.c`.
- `knlog` + `knring` (BOOTSTRAP §7 core): **105 + 34 checks PASS** on the target
  (twice), JSONL schema-validated.
- `knsrtp` phase 1 + `kn_aes` (vendored tiny-AES-c, Unlicense, sha256 recorded):
  **37/37 PASS** — FIPS-197 C.1, SP800-38A F.1.1, RFC 3711 B.2 (six keystream
  blocks incl. the FEFF→FF00 128-bit counter carry) and B.3 (cipher key, cipher
  salt, 94-octet auth key across six blocks with truncation) byte-exact, plus
  the nvst.rs IV builders (CM / GCM §9.1 / SRTCP GCM §9.2). See `docs/SRTP.md`.
- `knfec` (Cauchy RS-FEC over GF(2^8)/0x11d): golden tables/matrices, the
  `nvst.rs:13239` port, zero-padding semantics, wide-block parity: all PASS;
  deterministic property round-trips **200/200**. The first T9 run showed
  122/125 — the 3 failures were a TEST-SIDE state leak in `test_error_paths`
  (NULL `shards[2]` carried between subcases → `-2` fired before the behaviour
  under test); fixed with full explicit state per subcase. `knfec.c` logic was
  never implicated. See `docs/FEC.md`.
- Python deps (R15): T8 resolved all 14 pins on the target (rc=0 each),
  `python/requirements.txt` = RESOLVED (textual 6.2.1, rich 14.3.4,
  httpx 0.28.1, …), wheels cached on target in `tests\t8_wheels\`.

**Fixed after the first T9 run, awaiting a clean re-run:**
- `streamer/tests/test_knfec.c` — error-path subcases set full explicit state.
- `tests/T9_build_and_test.py` — Windows trap: `subprocess`' `cwd` does NOT
  apply to executable-path resolution, so relative `build\test_*.exe` paths
  resolved against `tests\` → rc=127 COMMAND NOT FOUND while the binaries
  existed (the user ran them manually — results above). Now absolute paths,
  with a warning comment for future editors.

**T9 round 1 (2026-10-21): VERDICT ALL PASS** — cmake rc=0 warnings=0;
knlog 105, knring 34, knfec 125 (property 200/200), knsrtp 37; JSONL PASS.
Phase-1 verification chapter CLOSED.

**Written since, awaiting the next T9 run (uncompiled):**
- SRTP **phase 2**: `kn_sha1` (vendored Brad Conte SHA-1, public domain,
  sha256 in PROVENANCE.md; RFC 3174 TEST1-4 + empty-message KAT), HMAC-SHA1
  (RFC 2104 glue; RFC 2202 cases 1-7 KAT), `kn_srtp_auth_tag`/`verify_tag`
  (be32(roc) suffix, constant-time compare), `protect/unprotect_cm_hmac`
  (in-place, authenticate-before-decrypt, ROC derived internally from
  packet_index per nvst.rs:2638), `kn_srtp_replay` (1:1 port of
  nvst.rs:3121-3211: 2048-packet window, 32xu64 bitmask, guess/check/commit,
  defensive no-op where the reference .expect()s).
- Evidence recorded: **SRTCP-CM is NOT needed** (nvst SRTCP is GCM-only; RR
  rides the DTLS-encrypted rtcp1 SCTP channel — nvst.rs:4346-4410).
- `tools/srtp_reference.py` p2 self-check passed in the sandbox (hashlib/hmac
  oracles; it caught a TEST4 transcription error before emission).
- `test_knsrtp` extended to ~72 checks (96-op replay script vs python port,
  protect goldens tag80/tag32, tamper cases, window edge cases).

**Awaiting from the user:** T9 re-run → `T9_skrot.txt` (expected:
build warnings=0; knlog 105, knring 34, knfec 125, knsrtp ~72 — ALL PASS).

## 4. FIRST ACTION for the next agent

1. Ask the user to run (or check whether he already ran):
   ```bat
   cd /d "%USERPROFILE%\Desktop\NewKarnyNow\tests"
   python T9_build_and_test.py
   ```
   and attach `T9_skrot.txt` (+ `T9_wynik.txt` on failure).
2. If T9 reports warnings/errors: fix them, re-issue T9. **Zero warnings under
   GCC 16.2 is the bar** (it is stricter than the old sandbox GCC 12 — it
   caught `-Wstring-compare` that GCC 12 missed).
3. Expected green output: `build(cmake) rc=0 warnings=0`, knlog 105 checks,
   knring 34, knfec 125, knsrtp 37 — all `-> PASS`, jsonl validation PASS,
   `VERDICT: ALL PASS`. Then update `PROJECT-STATE.md` and continue the task
   queue (§5). Note: as of round IV every module except the test_knfec
   error-path fix has already passed on the target; a green re-run closes the
   crypto/FEC verification chapter entirely.

## 5. Task queue (ordered; details in PROJECT-STATE "Next steps")

1. **T9 green on phase 2** (gate for everything below; phase 1 is already
   ALL PASS from round 1).
2. **Diagnostic bundle (§7.8)**: store-only ZIP writer in C, `summary.json`
   first, registered as a `kn_log_on_failure` callback; `kn_ring_dump` wired in;
   8 MiB cap, truncate-largest-first.
2b. **mbedTLS 3.6 LTS vendoring** (stage-2 prerequisite, own T9 round —
   big drop, keep it isolated): TLS/WSS/DTLS + AES-GCM(_8) + AES-256 CM;
   F1 flags + `_WIN32_WINNT=0x0601` for every vendored file (R14);
   `kn_aes`/`kn_sha1` switch backends behind unchanged APIs, KATs must pass
   identically. SRTCP-GCM (nvst.rs:4346+) lands with it.
4. **`knvstd` skeleton (stage 1 proper)**: `--loopback` reads `.264` →
   FFmpeg `libavcodec` h264, `threads=1`, NV12 → locked DEFAULT NV12 surface →
   StretchRect → Present in a 1280×800 `WS_POPUP` window (letterbox dest rect
   (0,40,1280,720)); WASAPI shared-mode synthetic PCM; perf snapshot emitter
   (§7.6) on the `perf` channel; console-host check belongs to the Python side.
   **Milestone 0 (R25):** isolated `--selftest=d3d.present` that creates DEFAULT
   NV12 → LockRect(NOSYSLOCK) → fills every byte → UnlockRect → StretchRect →
   Present, once, in its own process, before any long run.
   **Gate G1:** steady 30 fps × 10 min, CPU within the T1 budget (720p30 ≈ 24.8 %
   of both cores × up-to-1.8 content factor), RSS flat, complete self-explanatory
   diagnostic bundle, no network involved.
5. **Stage 2** per BOOTSTRAP §12 (WSS+RTSP, Mjolnir, SRTP unprotect, GS header,
   AU assembly; `--capture` mode; gate G2). R22: copy the **31-field**
   `session_info()` contract (`cloudmatch.rs:2070-2102`) as the IPC payload.

## 6. Binding working agreements (learned the hard way)

- **R26: the sandbox has no compiler and must not install one** (apt fails with
  infrastructure errors; packages never persist across turns anyway). The user's
  machine is the compile authority (T9). Consequence: review every C line by
  hand before handing over; keep per-round C deltas small; never claim
  "compiles clean" without a T9/native result.
- Measure first, then code; every stage has a gate; a failed gate is information.
- Every probe/test the user runs must emit a **small `*_skrot.txt`** next to the
  full output (a 150 kB attachment was once truncated and cost a whole round).
- Isolate anything that can crash (separate process; marker before risky calls).
- Logging rules §7.12 are non-negotiable (flush on ERROR, no fseek over own
  output, lengths-not-values for secrets, `code`/`reason`/`next_action` on every
  ERROR).
- D12: repo/logs/UI/comments/commits in **English**. Chat with user: Polish OK.
- The user runs tests willingly and fast — when in doubt, write a 2-minute T#
  probe instead of speculating for an hour.

## 7. Landmine list (details in the referenced docs)

1. GS extension body is **little-endian** (RTP header is big-endian) — §8.1.
2. `fec_current_block`/`fec_last_block` live in `ext[11]` top bits — §8.2 /
   `nvst.rs:3696-3697`.
3. FEC matrix is **Cauchy** (`C[d][p]=inv((P+d)^p)`), never Vandermonde — §8.3 /
   `docs/FEC.md`.
4. Reconstructed shards carry **transport metadata** that legitimately differs —
   restore the envelope from the template, never validate fecInfo on recovered
   shards (§8.4 / `nvst.rs:4101-4107`).
5. NVB activation needs **window state 19** in command `0x0320` (§8.5).
6. Never SYSTEMMEM NV12 (driver AV on fill); StretchRect conversion only into
   the backbuffer; `CheckDeviceFormatConversion` before every StretchRect;
   permanent degrade on AV (G0.2 doc §5).
7. `_WIN32_WINNT=0x0601` + `-march=core2 -msse3` for **our code and every
   vendored library** (F1/R14); verify via `-dM`.
8. `threads=1` on the decoder (D4); no DXVA2 ever (D5); no CPU scaling (D6).
9. Win7 wall clock ~15.6 ms; use `mono_ms` for latency arithmetic; DPST may
   drop refresh to 40 Hz on battery (D9 — measure Present intervals).
10. pip/py3.8: httpx 0.28 removed `proxies=`; textual 6.x ≠ old docs — code
    against the installed versions (requirements.txt notes).
11. **AES's `0x11B` is irreducible but NOT primitive** (ord(0x02) = 51): the
    S-box/log tables need generator **0x03**. FEC's `0x11D` DOES have 0x02 as a
    generator. Mixing the two silently produces degenerate tables — caught once
    already (impl round III); the `srtp_reference.py` self-check is the guard.
12. RFC 3711 appendix numbering: **B.2 = AES-CM, B.3 = KDF** (B.1 = AES-f8,
    unused by GFN). Memory said otherwise; the fetched RFC text is the truth.

## 8. Files to attach for a NEW agent (user checklist)

**Send these, nothing else is required:**
1. `karnynow-impl-round2.zip` — the ENTIRE workspace tree. It is
   self-contained: this recovery file, `PROJECT-STATE.md`, full copies of the
   research `BOOTSTRAP.md` + `HANDOFF` (under `docs/research/`), all C sources
   + tests + generated vectors, tools, T8/T9 probes, resolved
   `requirements.txt`, CMakeLists, T4 raw results (`artifacts/T4-v4/`),
   T8 summary (`artifacts/T8/`) and the full reference `nvst.rs`
   (`artifacts/nvst-extract/`, 565 kB — drop it from the zip only if size is a
   problem; refetch URL in §2).
2. The newest user-run artefacts produced AFTER this zip: `T9_skrot.txt`
   (+ `T9_wynik.txt` if anything failed), and any later `T#` outputs.

**Size guidance:** keep single attachments ≤ ~150 kB where possible (a 150 kB
JSON was once truncated and cost a whole measurement round). The zip is
~750 kB with nvst.rs, ~200 kB without — both fine as one attachment.

## 9. Workspace file map

```
karnynow/
├── AGENT-HANDOFF.md            ← THIS FILE (recovery doc)
├── PROJECT-STATE.md            ← living state: gates, D14-D17, risks, next steps, session log
├── CMakeLists.txt              ← target build (cmake+ninja, F1 flags for MINGW)
├── docs/
│   ├── LOGGING.md              ← knlog/knring conformance + API guide
│   ├── FEC.md                  ← knfec provenance (nvst.rs line refs) + semantics
│   ├── analysis/G0.2-T4v4-RESULTS.md ← presentation verdict, D7 settled
│   └── research/{BOOTSTRAP.md, HANDOFF-research.md} ← research-phase canon
├── streamer/
│   ├── include/kn/{knlog.h,knring.h,knfec.h,kn_aes.h,kn_sha1.h,knsrtp.h}
│   ├── src/{knlog.c,knring.c,knfec.c,knfec_tables.h(gen),kn_aes.c,kn_sha1.c,knsrtp.c}
│   └── tests/{test_knlog.c,test_knring.c,test_knfec.c,test_knsrtp.c,test_port.h,
│              knfec_kat_vectors.h(gen),knsrtp_kat_vectors.h(gen)}
├── third_party/
│   ├── tiny-aes-c/{aes.h,aes.c,PROVENANCE.md}            ← vendored, Unlicense
│   └── crypto-algorithms-sha1/{sha1.h,sha1.c,PROVENANCE.md} ← vendored, public domain
├── python/requirements.txt     ← RESOLVED cp38 pins (T8)
├── tests/{T8_python_deps.py,T9_build_and_test.py}   ← user-run probes
├── tools/{build_native_tests.sh,cross_check.sh,check_jsonl.py,
│          fec_reference.py,srtp_reference.py}
│       (the two .sh are sandbox-era; only usable if a future sandbox has gcc —
│        check `which gcc` first; T9 supersedes them on the target)
└── artifacts/{T4-v4/*,T8/T8_skrot.txt,nvst-extract/nvst.rs}
```

User-side repo: `C:\Users\KarnyJohnny\Desktop\NewKarnyNow\` (contains also the
research `tests\T1…T7` harnesses and `tests\t8_wheels\`).
