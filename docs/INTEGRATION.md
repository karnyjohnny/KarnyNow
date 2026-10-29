# INTEGRATION NOTES — Round XLIX delta, rev 40 (**RUN 30 = F11'S FIRST FLIGHT AND IT IS A ZERO-ERROR PROOF: `armed:1 srtp_derived:1 file:1` ×2 draws, `packets` 15 610 / **40 000** = `bundle_rtp_in` (nothing dropped), `srtp_fail/malformed/toc_invalid/pt_filtered/padded/replay_reject/seq_loss/seq_reorder/seq_dup` ALL ZERO, `red_packets` = `packets` with `red_blocks` = 2× and `red_invalid:0` (the rig sends every audio packet as RFC 2198 RED + one redundancy block), `last_ms−first_ms` = `packets × 20 ms` exactly, `ogg.dropped_packets/write_errors` 0, and the delivered 2 089 866-byte file is a valid OggOpus stream (ffprobe: Opus 48 kHz stereo 199.92 s 83 kb/s; 834 pages, contiguous `seq`, monotone `granulepos` = Σ TOC samples; 39 929/39 936 readable audio packets are TOC `0xEC` = CELT-FB 20 ms).** The RFC 5764 §4.2 exporter's SERVER half is thereby proven against live traffic — 40 000/40 000 HMAC-SHA1-80 authentications cannot pass with a wrong key half. **BUT THE AUDIO IS DIGITAL SILENCE** (decoded: mean −91.0 dB, max −80.8 dB, RMS 0.51 LSB in every 10 s bucket, all 19 146 240 samples |x|<8) **because no game ever ran**: the 0x010a burst carries only `upc.exe`/`UplayService.exe`/`UplayWebCore.exe` (Ubisoft Connect; `drsProfileName:"Ultimate Paintball Challenge"` misclassification again), the AU sizes are 0.8–1.2 KB/frame = a static launcher window for 75 s / 200 s, and the operator's browser control measured Legion's boot at **≈2 min 19 s from stream start to gameplay audio** — our draws died at 75 s and 197 s of media. **THE OPERATOR ALSO RETIRES §36's "~600 s SERVER CEILING"**: the pool is Warsaw (`np-waw-01.cloudmatchbeta`), the tier is Free, the browser gets a ONE-HOUR session and has never been cut at 10 minutes (matching run 25's `beforeEventMS:3599657`); the ≈600 s cut of runs 29/29g is therefore something our sessions do — prime suspect a server-side **idle/no-input policy** (every ≈600 s draw was launcher-parked with zero input flying). Run 30 also gives the cleanest freeze anatomy yet: video+audio abrupt to zero in one second while **SCTP `0x0101` heartbeats kept arriving for ~5 s** and the WSS stayed writable (`pings 41/101, pongs 0/0, dead:0`). Ships **F12** (`sess_audio_file_close` after `sess_pump_stop` — the XLVIII close was gated on `!pump_persist`, which is 1 for every live session, so `audio_close` NEVER fired: no EOS page, no `fclose`, and draw 1's FILE* leaked across the lottery gap → the 2 505-byte scrambled page at the draw1→draw2 boundary of run 30's file), **F13** (one audio file per draw, `<stem>_d<N><ext>`), **F14** (the interleaved jiggle A/B: with `--lottery=N>1` odd draws fly the jiggle, even draws are OFF, `lottery_arm` line per draw — trap 17 satisfied inside one run) and **F15** (`--lottery-stop-on-keeper=0|1`, default 1; an A/B needs 0). No wire-format, module-interface or KAT change → every count is unchanged (native battery green zero warnings at the XLVIII baselines, cross `-Werror` ×2 arches green with all four vendor trees staged, summary canon still **113 keys**). Stamp **XLIX**. Target T10 on XLVIII: **ALL PASS 82** (`audio.vectors` 188/0, knogg 163, knarx 62). Run 31 = `--session-seconds=900 --lottery=4 --lottery-gap=60 --session-jiggle=100 --lottery-stop-on-keeper=0 --session-audio-out=audio.opus` on Watch Dogs: Legion — four alternating ON/OFF draws, long enough to see both the ≈600 s cut and Legion's ≈139 s boot to audio.)

## Rev 40 changed files (round XLIX)

- `streamer/src/knvstd_main.c` — the ONLY code file touched:
  - NEW `sess_audio_file_close(sess_pump*, sess_ice_stats*)` (F12):
    idempotent EOS-flush + `fclose` + ogg-totals mirror + the
    `audio_close` stdout/knlog lines (the XLVIII block verbatim, moved
    into a function). Called right after `sess_pump_stop(&pump)` on the
    `goto teardown` path AND the `teardown_sock:` path, plus one
    defensive call at the `teardown:` label. The old
    `if (pump.audio_file_on && !pump_persist) { … }` block is GONE.
  - `run_session(…)` signature +`long draw_index` (before `log_dir`);
    both call sites updated (single = `0L`, lottery = `draw`).
  - NEW `static char audio_path[4096]` in `run_session` + the F13
    per-draw path derivation (`pump_set_str` + `strrchr` + `memcpy`;
    `snprintf` only for the `_d%ld` tail, never for a `%s`), assigned to
    `pump.audio_path`.
  - Lottery loop (F14/F15): `const long draw_jiggle = (lottery_n > 1 &&
    (draw % 2) == 0) ? 0L : sess_jiggle_ms;`, the `lottery_arm` stdout
    line, the corrected stop rule (`if (lrc != 1) { if (lrc != 0 ||
    lottery_stop_on_keeper) break; } if (g_knv_stop) break;`), the new
    `--lottery-stop-on-keeper=` flag (variable, parse, 0/1 bounds
    check), and the usage() text for both flags.
  - Build stamp `XLVIII` → **`XLIX`**.
- `docs/RTSP.md` §37 (the run-30 record + the §36 correction).
- `docs/BUNDLE-AUDIO.md` §6 (wiring, corrected close/per-draw file),
  §7.1 (three new reaction-matrix rows), §9 (the run-30 RESULT).
- `docs/INPUT.md` §9 (the interleaved-A/B instrument now exists).
- `START-HERE.md` (header + §1 the run-30 verdict and the withdrawn
  ceiling, §2 reading order, §3 the run-30 row + the "Round XLIX
  shipped" block + the XLIX gates block, §4 the run-31 protocol
  step-by-step, §8 traps 19 rewritten / 20-21 updated / **22-25 new**,
  §10 history with the XLVIII row annotated "⚠ WITHDRAWN IN XLIX").
- `docs/BOOTSTRAP.md` §12 status marker → round XLIX (stage 3: audio
  extraction live-proven, the content capture + the idle-policy
  question open, the XLVIII ceiling finding withdrawn) + the sandbox
  gate canon block at XLIX close.

## Rev 40 traps (new this round)

1. **`pump_persist` is 1 for EVERY live session** (it is
   `pump_th && !unified_mode`, and a live run always has both). Any
   teardown work gated on `!pump_persist` is dead code on the target —
   it only runs in `--decode`/`--replay-capture`/disablePlay paths.
   Audit anything added there; the correct place for post-pump cleanup
   is immediately AFTER `sess_pump_stop()` (which joins the worker, so
   D21 single-ownership is satisfied).
2. **A lottery draw must not inherit an open FILE\* from the previous
   draw.** `memset(&pump, 0, sizeof pump)` at `run_session` entry drops
   the handle without closing it — every resource the pump owns has to
   be released before the draw returns (run 30's scrambled page is the
   field evidence). When adding a pump-owned resource, add its release
   to `sess_pump_stop`'s call site, not to the summary path.
3. **`--lottery` changes two behaviours at once now** (F13/F14): the
   audio file name AND the jiggle arm. `--lottery=1` stays
   byte-identical to XLVIII; anything ≥2 alternates. A run that needs a
   constant arm must pass `--lottery=1` (or compare `lottery_arm`
   lines, never assume).
4. **`--lottery-stop-on-keeper=0` can run for hours**: N draws ×
   `--session-seconds` + N × the CM backoff ladder. Size the operator's
   window before proposing it (run 31 = 4 × up to 900 s ≈ 60 min worst
   case + queues).
5. **Healthy audio counters + silence ≠ an audio bug.** Read the video
   (AU sizes ~1 KB/frame = static screen) and the 0x010a burst (game
   process present?) before touching `kn_arx`. And the 0x010a dumper
   caps at **16 lines per session** (F1), all of which arrive in the
   first ~400 ms — a game that launches at t+139 s is INVISIBLE to that
   channel by construction; the AU-size timeline is the app-state
   evidence for late launches.
6. **usrsctp's clone URL is `github.com/sctplab/usrsctp`** — the
   `github.com/sctp/usrsctp` form no longer resolves (git asks for a
   username, which fails in a headless sandbox). Pin
   `c625e21950600460cb5c6075616eb1622df2b06a`.
7. **F13 splits the audio path at the LAST DOT IN THE WHOLE STRING**
   (`strrchr(path, '.')`), not at the last dot of the file name, so
   `--session-audio-out=C:\my.dir\audio` (a *directory* with a dot, no
   extension) would become `C:\my_d1.dir\audio`. Pass a plain
   `name.ext` — run 31's `audio.opus` → `audio_d1.opus … audio_d4.opus`
   — and the case cannot arise. The builder is the bounded
   `pump_set_str` + `memcpy` idiom with an explicit length proof
   (`snprintf` is used only for the `_d%ld` tail, never for a `%s`), so
   a too-long path keeps the un-suffixed name instead of truncating
   mid-string and `-Wformat-truncation` has nothing to prove; the
   paranoid knvstd leg in `build_native_tests.sh` re-checks that class
   every round.
8. **A sandbox snapshot drops directories above ~128 MB**: a full
   OpenNOW clone (314 MB) vanished between tool calls. Use
   `git clone --depth 1 --filter=blob:none --sparse` +
   `git sparse-checkout set native/opennow-streamer/crates` (13 MB) and
   keep it OUTSIDE the workspace (e.g. `/tmp/refq`) — it is a scratch
   reference, not a deliverable. Note the crate layout at HEAD: the
   protocol sources live in **`opennow-streamer-transport/src/`**
   (`nvst.rs`, `nvst_input.rs`, `nvst_server_control.rs`,
   `nvst_audio_jitter.rs`, …), not `opennow-streamer-core/src/` as the
   frozen docs say.

## Rev 40 verification status (what ran WHERE)

* Sandbox (this round, all four vendor trees staged: tiny-AES-c
  `23856752f`, crypto-algorithms `cfbde484`, mbedTLS 3.6.0 `2ca6c285` +
  framework `750634d3`, usrsctp `c625e219`):
  `tools/build_native_tests.sh` → **NATIVE TESTS PASS**, zero warnings,
  every suite at its XLVIII baseline (knlog 105, knring 34, knfec 125,
  knsrtp 77, kndiag 109, **knselftest 118**, kngs 125, knrx 218,
  knaudio 22, knsock 102, knws 88, knrtsp 150, kncm 148, kncert 4,
  kndtls 6, knsctp 13, kninput 548, **knogg 163**, **knarx 62**),
  JSONL + ZIP validators green, knvstd smoke JSON single-document
  contract intact · `tools/cross_check.sh` → **CROSS-CHECK PASS ×2
  arches** (x86_64 + i686) under `-Wall -Wextra -Werror`, zero warnings,
  with the mbedTLS/usrsctp real-body legs now compiled (no SKIPs).
* ALSO re-run this round (they were listed as skippable — no
  wire-format, interface or vector change — and were run anyway, so
  the XLIX gate set is complete rather than inherited):
  `cmake -S . -B build/net-mb -G Ninja` + `ninja` + `ctest` →
  **22/22 PASS** (`CFG_RC=0 BUILD_RC=0 CTEST_RC=0`,
  `logs/cmake_mb.log`), same for `-DKN_TLS_BACKEND=schannel` →
  `build/net-sch` → **22/22 PASS** (`logs/cmake_sch.log`). cmake DOES
  build the `knvstd` target in both trees (stamp **XLIX** verified in
  `build/knvstd`, `build/net-mb/knvstd`, `build/net-sch/knvstd`) — but
  **without FFmpeg**, because the owner's `build/ffmpeg/` dev tree is
  gitignored and not in the sandbox: `kn_decode` compiles as the honest
  stub and `decode.synthetic` reports `skipped` (`CMakeLists.txt`
  :146-193's designed no-FFmpeg mode; `build.json` extras say
  `ffmpeg=disabled,tls=mbedtls`) · `tests/T10_build_and_test.py` POSIX
  dry → **ALL PASS 72**, zero compiler warnings, bundle 6 767 B,
  rc 0 (`logs/t10_xlix.log`) · `tests/T13_udp_e2e.py` →
  **ALL PASS 63** with `--knvstd build/net-mb/knvstd` (the mbedTLS
  cmake binary); **ALL PASS 51** with the bare `build/knvstd`, whose
  single SKIP is by design (the gcm256 leg — a bare build is
  cm128-only) · `sandbox_session_e2e` rebuilt from the XLIX sources
  with the header recipe under `-Wall -Wextra` (zero warnings) →
  **43 checks, 0 failures** from the repo root (`logs/e2e_xlix.log`) ·
  `tools/check_summary_skeleton.py . ../karnynow-logs/run30_logs/run30.log`
  → **PASS, skeleton 113 == live 113** (F12–F15 add and remove no key;
  the validator has now been run against a REAL target summary, not
  just a sandbox one) · knvstd CLI smoke:
  `--lottery-stop-on-keeper=2` → usage + **rc 2** (the new bounds check
  bites), and the **exact run-31 argument set** (`--session-seconds=900
  --lottery=4 --lottery-gap=60 --session-jiggle=100
  --lottery-stop-on-keeper=0 --session-audio-out=audio.opus`) against a
  dead `--cm-host` → it clears the bounds check and prints
  `{"event":"lottery_arm","draw":1,"jiggle_ms":100}` **before** failing
  `tls_stream_unavailable` (the bare sandbox build has no TLS backend) →
  `lottery_draw rc:3` → `lottery_summary draws:1 last_rc:3`. So F14's
  arm line is live, no argument in `START-HERE.md` §4 Step 4 can be
  rejected on the target, and the F6b rc-2/3 abort contract is intact (a
  hard failure still does NOT re-draw).
* Not run in the sandbox (needs the target): the sanitizer legs on the
  changed file and the real TLS/`--session` path. The full set runs on
  the Win7 target as T10 (expect **ALL PASS 82**, unchanged — XLIX adds
  no suite, self-test or check) + T13 + run 31.
* Off-box evidence work on the delivered run-30 files (not a repo gate):
  Ogg page walk + CRC/`granulepos`/`seq` verification in Python,
  `ffprobe`/`ffmpeg` decode + `volumedetect`, per-10 s RMS profile,
  Opus packet/TOC histogram, AU-size timeline from the 8 163 `au`
  lines, per-second `session_stats` diff of `transport.jsonl`.

---

# INTEGRATION NOTES — Round XLVIII delta, rev 39 (**F11 SHIPPED (sandbox-proven): the bundle-audio extraction stage — `kn_arx` (SRTP/RED/TOC receive pipeline, 14-step order pinned by `tools/arx_reference.py`) + `kn_ogg` (zero-alloc OggOpus muxer, byte-exact vs `tools/oggopus_reference.py`) + the RFC 5764 §4.2 DTLS-SRTP exporter in `kn_dtls` (`mbedtls_ssl_set_export_keys_cb` → `kn_dtls_srtp_export(out60)`; server half → RFC 3711 KDF labels 0/1/2; stub/schannel honest −1) + `--session-audio-out=F.opus` + `audio_arm`/`audio_close` events + the 34-key summary `"audio"` block (new skeleton canon 113 recursive keys, validated by the NEW `tools/check_summary_skeleton.py` — the old block-scan validator and its 108-vs-112 mystery are retired) + `audio.vectors` self-test (188 checks, gated, registry 16) + suites `test_knogg` (163) / `test_knarx` (62) → ctest 22/22 ×2 backends, T10 dry 72 (target expectation **82**), native battery green zero warnings, cross `-Werror` ×2 arches green, e2e 43/43.** Runs 29/29g analysis (RTSP §36): the **~600 s server ceiling** measured from both sides (polite `0x0109`+TERM at 599.1–599.2 s; silent gradual decay at 594 s — decay = ceiling, abrupt silence = rig freeze), **Watch Dogs: Legion = the first gameplay-reaching app: a flawless 599.1 s / 17 970-AU `server_termination` keeper**; F10 labels matched 6/6 draws; F7 backoff matched every queue. Run 30 = `--session-seconds=570 --lottery=2 --session-audio-out=audio.opus`, NO au-dir; a `"time"` keeper below the 594 s floor + a playable ~570 s .opus. **App-id canon: 107057914 = Arknights: Endfield** (ALWAYS was — earlier docs saying "Arknights launcher-parked" mean Endfield), **100361011 = Watch Dogs: Legion**. Stamp **XLVIII**.)

## Rev 39 changed files (round XLVIII)

- NEW `streamer/include/kn/kn_arx.h`, `streamer/src/kn_arx.c` — the
  bundle-audio receive pipeline (contract + decisions in
  `docs/BUNDLE-AUDIO.md` §3/§8; 17 counters + first/last_ms).
- NEW `streamer/include/kn/kn_ogg.h`, `streamer/src/kn_ogg.c` — the
  OggOpus capture muxer (BUNDLE-AUDIO §4; CRC table GENERATED +
  Python-verified in-file after the hand-typed 160-entry incident).
- NEW `streamer/src/kn_audio_rx_kat_runner.h` — the shared KAT battery
  runner (unit tests + the `audio.vectors` production self-test).
- NEW `streamer/tests/test_knogg.c`, `streamer/tests/test_knarx.c` —
  unit suites (differential + property legs).
- NEW `tools/oggopus_reference.py`, `tools/arx_reference.py` — the
  independent Python references (both `--check`/self-prove before
  emitting; arx_reference had a POINTER-ARRAY emission bug fixed this
  round: groups emitted `vec*[]` while the runner read `vec[]` — ASan
  caught it on the first link; generators now emit struct arrays).
- NEW `streamer/src/kn_ogg_kat_vectors.h` (3296 lines),
  `streamer/src/kn_arx_kat_vectors.h` (12 groups / 21 packets).
- NEW `docs/BUNDLE-AUDIO.md`; NEW `tools/check_summary_skeleton.py`.
- `streamer/src/kn_dtls.c` / `include/kn/kn_dtls.h` — export-keys cb +
  `kn_dtls_srtp_export` (mbedtls leg; stub −1); struct captures
  master+randoms (never logged).
- `streamer/src/kn_selftest.c` — `audio.vectors` (registry 16).
- `streamer/src/knvstd_main.c` — CLI flag, arm/demux/teardown wiring,
  pump fields + `sess_ice_stats` audio mirror (snapshot-safe), summary
  `"audio"` block, help text, stamp XLVIII (~6940 lines).
- `tests/test_knselftest.c` — registry pins 15→16, host ok_count 6→7,
  results capacity 24.
- `tests/T10_build_and_test.py` — suites +test_knogg/test_knarx,
  registry pin 16, portable-must-pass list +audio.vectors (dry 72;
  target expectation 82).
- `CMakeLists.txt` — kncore +kn_ogg/kn_arx; ctest foreach +knogg +knarx
  (22 tests).
- `tools/build_native_tests.sh` — KNCORE +2 sources; build+run lines
  for the two new suites (rc23/rc24 in the PASS tally).
- `tools/cross_check.sh` — per-file cross compiles +2; KCORE +2 (the
  kn_dtls REAL-body leg already covers the exporter).
- `docs/RTSP.md` §36 (runs 29/29g + F11); `docs/AUDIO.md` naming
  cross-ref; `docs/BOOTSTRAP.md` §12 status marker + gate counts;
  `START-HERE.md` (XLVIII state + run-30 protocol + reaction matrix);
  `tests/README.md` (T10 counts).

## Rev 39 traps (new this round)

1. **Never hand-type const tables** — the first kn_ogg CRC table was
   written from memory: 160/256 entries, wrong from index 24. Generate
   + verify in-file (the bitwise differential in test_knogg is now the
   permanent guard).
2. **RFC 6716 TOC**: 0xF8 = CELT-FB 20 ms = 960 samples; 0xC0 = SWB
   2.5 ms = 120; 0xE0 = FB 2.5 ms = 120. The 16-31 config block is
   bandwidth-major, duration-minor.
3. **arx ordering IS semantics**: replay BEFORE seq accounting (true
   dups → replay_reject, seq_dup stays 0); SSRC-change reset BEFORE the
   replay guess; padded++ on the P bit even if the pad is invalid;
   seq accounting right after SRTP commit, BEFORE padding/RED/TOC;
   header-walk overflow = malformed, not srtp_fail.
4. **OggOpus body offset**: the OpusHead magic sits at page offset
   **28** (27-byte header + 1 lacing byte), not 27.
5. **Native test link line** must carry the third_party .c files
   (`third_party/tiny-aes-c/aes.c`,
   `third_party/crypto-algorithms-sha1/sha1.c`) — kn_aes/kn_sha1 are
   wrappers only.
6. **Sandbox toolchain is NOT persistent** — every fresh session:
   `apt-get update && apt-get install gcc cmake make ninja-build`
   (mingw-w64 for the cross leg; it is huge — install in background).
7. Summary canon: **113 recursive keys** (drops/outbox_by_sid emptied).
   Validate with `tools/check_summary_skeleton.py <repo> <live.log>` —
   skeleton==live is the gate, not a fixed block count.

---

# INTEGRATION NOTES — Round XLVII delta, rev 38 (RUNS 28 + 28b = **THE FIRST KEEPERS — STAGE 2 IS CLOSED.** Run 28 (jiggle OFF, `--lottery=10`) drew 33.2 s → 201.6 s (a threefold endurance record: 6049/6049 AUs, 91 941/91 941 datagrams, 90 FEC repairs, zero gaps) → **a FULL-LENGTH KEEPER: `stop_reason:"time"`, wall 360.0 s, media 359.96 s, 10 800/10 800 AUs decoded (exactly 30 fps × 360 s), 169 036/169 036 datagrams authenticated, zero gaps/errors/recoveries, ONE keyframe, idr/pli never needed, drift −3 ms** — and the lottery stopped on it by design (rc 0, exit 0). Run 28b (era re-control, jiggle ON) produced **a second keeper on its first draw** (359.95 s, 10 800/10 800, 171 427/171 427 datagrams, `jiggle:3533` flown, zero gaps). **The 28b ON-keeper forces the honest correction of the XLVI verdict**: the same evening drifted favourable for BOTH settings (band draws 18:00–20:30Z → OFF tails 20:41Z → OFF keeper 22:05Z → ON keeper 22:22Z), so "the jiggle is a depressant" is DOWNGRADED to *suggestive, era-confounded*; the default stays OFF (not necessary for any survival class, not invariably fatal, asymmetric bot-signature risk). Keepers' control plane: 0x0101 heartbeats at ~1 Hz for the full 360 s (333 by rel 340 s) — §34's healthy-draw indicator confirmed at session scale. No SESSION_LENGTH_TIMER in either (2 of last 10 draws). Ships **F10**: knlog failure labels follow the stop_reason (run 28's draws 1–2 logged `E_SESSION_NO_FRAMES` "no access unit completed" right after decoding 998/6049 AUs perfectly — the pre-D8 label pair predates the recovery contract F6a made reachable; new codes: `E_SESSION_RECOVERY_EXHAUSTED`, `E_SESSION_SERVER_TERMINATION`, `E_SESSION_SOCKET_ERROR`, `E_SESSION_FAILED_VERDICT` join the original two). Stamp **XLVII**. Target T10 on XLVI: ALL PASS 79. Run 29 = the 30-minute soak, optionally the first gameplay app.)

> **Rev 38 supersedes rev 1-37 as the current delta.** Canonical state:
> **`START-HERE.md`** (repo root). Design record: **`docs/RTSP.md` §35**
> (the keeper anatomy, the corrected jiggle verdict, F10, the stage-2
> closure statement, the run-29 plan) + **`docs/INPUT.md` §9** (the
> downgraded verdict) + **`docs/BOOTSTRAP.md` §12** (status marker).

## Changed files (round XLVII)

| file | change |
|---|---|
| `streamer/src/knvstd_main.c` | **F10**: the single `kn_log_on_failure("session.media", ...)` site after the media loop — the pre-D8 `datagrams==0 ? NO_MEDIA : NO_FRAMES` pair replaced by a stop_reason-driven selection: `E_SESSION_NO_MEDIA` (0 datagrams) / `E_SESSION_RECOVERY_EXHAUSTED` (the dominant dead-draw class; reason text points at the recovery block + transport channel) / `E_SESSION_SERVER_TERMINATION` (0x109) / `E_SESSION_SOCKET_ERROR` / `E_SESSION_NO_FRAMES` (datagrams but zero AUs — the original meaning) / `E_SESSION_FAILED_VERDICT` (contract under-delivery). knlog/diag layer only — stdout untouched. Build stamp `XLVI`→`XLVII`. 6780 → 6808 lines. No wire-format changes. |
| `docs/RTSP.md` | §35 — the full round-XLVII record (keeper tables + anatomy, the corrected verdict, F10, stage-2 closure, run-29 plan). |
| `docs/INPUT.md` | §9 second correction: the depressant verdict DOWNGRADED (28b's ON keeper); what survives (not necessary / not fatal / default stays OFF / interleaved-A-B requirement). |
| `docs/BOOTSTRAP.md` | §12 status marker: **stage 2 CLOSED** (definitions unchanged — the note only records progress). |
| `START-HERE.md` | state → XLVII + **stage 3 of 5**; runs 28/28b rows; the XLVII-shipped block; §4 = run 29 (30-min soak, step-by-step); trap 17 (same-evening-interleaved A/B rule); history +XLVII. |

## Integration notes / traps for the next agent

1. **STAGE 2 IS CLOSED — the project is at stage 3** (audio bundle /
   Opus first, then G3 keyboard-wheel-text, G4 cursor payloads, G1
   handshake timeout; D-t13 watchdog last). Do not open new stage-2
   media/transport work without a keeper-breaking regression.
2. **The jiggle verdict is DOWNGRADED, not reversed-reversed**: the
   XLVI "depressant" conviction is unsound (28b's ON keeper), the
   run-27 A/B stays *suggestive*, and the practical decisions stand:
   default OFF, handshake untouched, no periodic input patterns
   without a survival test. Do not cite §34/rev 37 as settled science
   — cite §35.3/this rev.
3. **Era drift makes cross-evening A/Bs unreadable (trap 17)**: the
   same client+app went 5/5 band draws → 2/2 keepers within ~4 hours
   (Oct 28 18:00→22:30Z). Any future input/policy A/B MUST interleave
   the arms inside one run (or one evening at minimum) — never
   compare against a previous evening's draws.
4. **The failure-label taxonomy changed (F10)**: knlog/diag consumers
   keyed on `E_SESSION_NO_FRAMES` must know it now ONLY means
   "datagrams but zero AUs"; the common dead-draw case is
   `E_SESSION_RECOVERY_EXHAUSTED`. The diag zip's `operation_failed`
   lines and `app.log` carry the new codes from run 29 on.
5. **Keeper signature (the healthy-draw checklist)**: `stop_reason:
   "time"` + `idr:0, pli:0, kf:1` + `recovery {attempts:0}` + 0x0101
   heartbeats past 10 s + drift within ±10 ms over the session. A
   draw that arms idr/pli > 0 is already in trouble (every dead draw
   had 50–92 retries).
6. **Do NOT pass `--session-au-dir` for long draws**: a 30-min keeper
   writes ~54 000 AU files (~800 MB) to the operator's laptop for
   evidence the stdout telemetry already carries. Targeted short
   draws are the cheap way to capture AUs; the operator's push
   excludes `.264` anyway (11 000-file pushes kill the laptop) — ask
   by message for specific AU files when needed.
7. Evidence workflow (since rev 37): the operator pushes whole runs
   to **`github.com/karnyjohnny/karnynow-logs`** — now restructured
   to hold ALL rounds (`runNN_logs/` folders, `.264` excluded). The
   agent clones, copies the current run's folder into the workspace
   IMMEDIATELY, and analyzes from there. Runs 22–28b are archived in
   the workspace (`run22_logs/` … `run28b_logs/`) and in the repo.
8. Everything from rev 36 stands except where corrected here (F6a/F6b
   contracts — now field-proven by the keepers' rc 0 early stop; F7
   schedule — draws 2–3 of run 28 queued on the 4th create after
   exactly 20+40+80 s, twice more; `cm_create_backoff` naming; knlog
   APPEND across draws; printf ritual — 112 blocks).

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **NATIVE TESTS PASS** (per-suite counts unchanged; paranoid legs incl.) |
| `tools/cross_check.sh` ×2 arches | **CROSS-CHECK PASS**, zero warnings |
| ctest native / mbedTLS / schannel | **20/20 ×3** |
| T10 POSIX dry | **69 checks, 0 failed** |
| T13 | **ALL PASS 63** |
| summary-printf validator | **112** blocks, 0 mismatches; summary **74 = 74**; skeleton `json.loads` PASS |
| `sandbox_session_e2e` | **43/43 PASS** |
| stamp check | build line reads `"round":"XLVII"`; lottery mechanics re-verified on the TLS-less build |
| ASan/UBSan | carried from XLI.1 (no wire/decoder code touched XLIII–XLVII; F10 is label selection only) |

## Target expectations (Win7, run 29)

- **T10**: ALL PASS **79**, knrx **243**, kninput **548**,
  `input.vectors` **50** — unchanged (F10 touched no test file).
- First stdout line of every run: `"round":"XLVII"`.
- **Run 29 — the 30-minute soak** (NO `--session-au-dir` — trap 6):
  `knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=1800 --lottery=3 --lottery-gap=30 > logs\run29.log`
  A keeper now means a **1800 s** survival (rc 0, early stop). If a
  gameplay-reaching app id is available, substitute `--session-app=`
  — that makes run 29 the first interactive-content test (the
  estimator's real exam: scene-cut bitrate storms vs the parked
  portal's static screen).
- Expected shapes: per draw the full session lines + summary;
  `cm_create_backoff` waits between draws are normal; a dead draw now
  logs `E_SESSION_RECOVERY_EXHAUSTED` in `app.log`/diag (F10) instead
  of the misleading `E_SESSION_NO_FRAMES`.
- Deliver via **karnynow-logs**: `run29.log` + the whole `logs/`
  directory + the T10 pair (no `.264` — as usual).

---

# INTEGRATION NOTES — Round XLVI delta, rev 37 (RUN 27 = THE JIGGLE A/B, DECIDED: **THE SYNTHETIC MOUSE JIGGLE IS A SURVIVAL DEPRESSANT.** Five jiggle-OFF draws — same evening as the ON draws, same app, five FRESH rigs (a different IP per draw) — lived **10.32 / 21.81 / 15.39 / 52.19 / 22.87 s** of media: four of five beyond the jiggle-ON maximum (13.2 s), and draw 4 set the new project record — **52.19 s of continuous 30 fps video, 1546/1546 AUs decoded, 13 071/13 071 datagrams authenticated, 1 gap, zero decode errors, BWE 37.8 Mbps** (previous record: run 22's ~41 s — also jiggle-off). The five ON draws (runs 23–26): 10.6/10.4/11.6/13.2/11.3 — five of five inside the band; P(all five ≤ 13.2 s | OFF population) ≈ 0.03 %. The XLI.1 jiggle was TRUNCATING every draw. **F9 flips the default to OFF** (`--session-jiggle=100` restores legacy); XLII's "jiggle stays ON as a controlled constant" is REVERSED on evidence, and the run-16→22 "fidelity ladder" gets its final reinterpretation: survival differences were the jiggle penalty + rig variance, never client fidelity. The activation handshake is EXONERATED (flew 5/5 in the OFF draws; long draws came with it — only the periodic MOUSE_RELATIVE motion is implicated). Run 27 also field-proved **F7** (draws 2–5 each hit the seat cooldown and queued after exactly three escalating backoffs 20+40+80 s, fourth create succeeding — the cooldown is ~2–3 min; no 429 at this cadence) and **F8** (zero `input_jiggle` lines, `feedback.jiggle:0` ×5). No SESSION_LENGTH_TIMER in any draw (2 of the last 8 draws carried one — intermittent). Target T10 on XLV: ALL PASS 79. Stamp **XLVI**. Run 28 = the keeper hunt, ten draws at default OFF.)

> **Rev 37 supersedes rev 1-36 as the current delta.** Canonical state:
> **`START-HERE.md`** (repo root). Design record: **`docs/RTSP.md` §34**
> (the A/B table, per-draw detail, the F7 cooldown measurement, the
> 16-draw survival record, the run-28 read matrix) + **`docs/INPUT.md`
> §9 corrected verdict** (the jiggle-is-a-depressant addendum).

## Changed files (round XLVI)

| file | change |
|---|---|
| `streamer/src/knvstd_main.c` | **F9**: `sess_jiggle_ms` default `SESS_JIGGLE_INTERVAL_MS` → **0** (the flag, validation and media-loop gate are the XLV F8 machinery — unchanged); usage() text records the new default and the verdict; the F8 comment in `run_session` and the `SESS_JIGGLE_INTERVAL_MS` define carry the run-27 evidence block. Build stamp `XLV`→`XLVI`. 6765 → 6780 lines. No wire-format changes. |
| `docs/RTSP.md` | §34 — the full round-XLVI record. |
| `docs/INPUT.md` | §9 **corrected verdict** addendum: the jiggle is a depressant, default OFF, the handshake exonerated, G3 caveat (measure survival impact before adopting any periodic input pattern). |
| `START-HERE.md` | state → XLVI; run-27 row; the XLVI-shipped block; §4 = run 28 (keeper hunt, step-by-step); §11 = the NEW evidence workflow (the `karnynow-logs` git repo); history +XLVI. |

## Integration notes / traps for the next agent

1. **THE JIGGLE DEFAULT IS NOW OFF (F9)** — any procedure, script or
   doc fragment still assuming "the jiggle flies by default" is
   STALE (everything written XLII–XLV). `feedback.jiggle:0` + no
   `input_jiggle` line in stdout is the OFF proof; `--session-jiggle=100`
   is the legacy ON. Do not "restore" the ON default without a new A/B.
2. **Input's role, final form**: the activation handshake (version 3,
   enable-input chain) is REQUIRED for long draws (run 18's 11→30.5 s
   move) and NOT harmful (flew in all run-27 draws); the periodic
   synthetic MOUSE MOTION is a depressant (run 27 vs runs 23–26). If
   G3 (keyboard/wheel/text) ever ships, it must be survival-tested
   before adoption — no periodic patterns by default.
3. **The seat cooldown is ~2–3 minutes** (run 27: four of four draws
   queued on the 4th create after 30 s gap + 140 s of escalating
   backoff; run 26: one draw needed only 20 s, one exceeded 90 s).
   F7's 20/40/80/160 s schedule matches the field; the 429 limiter
   (run 26) fires at ~7 creates in 6 min — a ≥ ~3 min/draw cadence
   stays under it. Do not shorten the schedule without new evidence.
4. **Evidence workflow CHANGED (run 27 onward)**: the operator pushes
   the whole `logs/` directory + stdout capture to
   **`github.com/karnyjohnny/karnynow-logs`** (cleaned and re-pushed
   each run — no more .txt renames, no 10-file upload limit). The
   agent CLONES it, archives a copy under `runNN_logs/` in the
   workspace IMMEDIATELY (the repo is force-cleaned each round — the
   workspace archive is the only history), then analyzes. Run 27
   arrived complete: `run27.log`, `transport.{log,jsonl}`,
   `diag{,.1,.2}.log` + the zip bundle, `wire_session.log`, T10 pair,
   `aus_r27/` (1587 AU files).
5. **Long draws keep the control plane alive**: draw 4's `ctrl_in`
   reported 53 msgs at `rel_ms:40005` (0x0101 heartbeats flowing at
   ~t+40 s) — the "ALL control ≤10 s" signature of in-band draws is a
   property of the EARLY DEATH, not of the session. Presence of
   0x0101 past 10 s is now a healthy-draw indicator.
6. Everything from rev 36 stands (F6a exit-code contract, F6b lottery
   rc semantics, `cm_create_backoff` naming + `"status"` field, knlog
   APPEND across draws, SESSION_LENGTH_TIMER intermittency, drs
   profile fields, knrx 218/243, printf ritual — 112 blocks).

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **NATIVE TESTS PASS** (per-suite counts unchanged; paranoid legs incl.) |
| `tools/cross_check.sh` ×2 arches | **CROSS-CHECK PASS**, zero warnings |
| ctest native / mbedTLS / schannel | **20/20 ×3** |
| T10 POSIX dry | **69 checks, 0 failed** |
| T13 | **ALL PASS 63** |
| summary-printf validator | **112** blocks, 0 mismatches; summary **74 = 74**; skeleton `json.loads` PASS |
| `sandbox_session_e2e` | **43/43 PASS** (from the repo root) |
| F9 field tests | usage() renders the new default text; `--session-jiggle=100` accepted (arg-valid); `--lottery=5` on the TLS-less build → stop after draw 1 rc:3, stamp `"XLVI"` |
| ASan/UBSan | carried from XLI.1 (no wire/decoder code touched XLIII–XLVI) |

## Target expectations (Win7, run 28)

- **T10**: ALL PASS **79**, knrx **243**, kninput **548**,
  `input.vectors` **50** — unchanged (XLVI touched no test file).
- First stdout line of every run: `"round":"XLVI"`.
- **Run 28 — the keeper hunt** (ten draws, default OFF — NO jiggle
  flag needed):
  `knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=360 --lottery=10 --lottery-gap=30 --session-au-dir=logs\aus_r28 > logs\run28.log`
- Per draw: NO `input_jiggle` line and `"jiggle":0` in every summary
  (the OFF proof — now the default); `cm_create_backoff` waits of
  20–160 s between draws are NORMAL. A draw surviving the full 360 s
  ⇒ `lottery_draw rc:0`, early stop, process exit **0** — the first
  KEEPER. Expect up to ~60–80 minutes for ten draws.
- Optional era re-control (cheap, one extra command after the hunt):
  `--lottery=2 --session-jiggle=100` — if both draws land in-band
  while the evening's OFF draws did not, the A/B is re-confirmed
  against rig-pool drift.
- Deliver: push `run28.log` + the whole `logs/` directory (+ the T10
  pair) to the **karnynow-logs** repo, as with run 27.

---

# INTEGRATION NOTES — Round XLV delta, rev 36 (RUN 26 = THE LOTTERY'S FIRST REAL SITTING: 5 draws, 2 real sessions, 3 lost at the CloudMatch door — and the door refusals are now MEASURED: the seat cooldown can exceed **90 s** (draw 2: 30 s gap + F4's 3×20 s retries, still 403) and repeated creates trip an **HTTP 429 rate limiter** (draws 3 and 5; F4 only retried 403, so both died instantly). F6a/F6b passed their live proof — every dead draw exited 1 honestly, the lottery ran all 5, `wire_session.log` (the exit≠0 ring dump) reappeared. The two real draws died in the band again (**13.15 s** and **11.34 s**; 379/379 + 342/342 AUs; draw 1 lossy — gaps 1, fec 1, withheld 16 — decoded perfectly), completing the pattern that reopens the jiggle question as a controlled A/B: **all FIVE jiggle-on draws ever died at 10.4–13.2 s; the only TWO long draws in history (runs 18/22: 30.5/41 s) were activation-WITHOUT-jiggle.** XLII eliminated input as a saviour but never tested it as a depressant. Ships **F7** (cm_create patience: retry 403 + the whole `cm_status_transient` class incl. 429, escalating **20/40/80/160 s**, event renamed `cm_create_backoff` + `"status"` field) and **F8** (`--session-jiggle=MS`, 0–10000, default 100 unchanged, **0 = OFF** — the A/B knob). No SESSION_LENGTH_TIMER in either draw (delivery is intermittent: present 23/25, absent 24/26×2 — the one-hour-budget fact of §32.2 stands). Stamp **XLV**. Run 27 = the jiggle-OFF lottery.)

> **Rev 36 supersedes rev 1-35 as the current delta.** Canonical state:
> **`START-HERE.md`** (repo root). Design record: **`docs/RTSP.md` §33**
> (run 26 draw-by-draw, the F7 schedule, the jiggle A/B rationale and
> the 11-draw table, the run-27 read matrix).

## Changed files (round XLV)

| file | change |
|---|---|
| `streamer/src/knvstd_main.c` | **F7**: the F4 retry loop widened — retry iff `http_status == 403 \|\| cm_status_transient(http_status)` (408/425/429/500/502/503/504 — the polling loop's existing class), escalating wait `SESS_CM_BACKOFF_BASE_MS << attempt` = 20/40/80/160 s, `SESS_CM_RETRIES 4` (5 attempts, ≤ ~5 min patience per draw); event line renamed `cm_create_403_backoff` → **`cm_create_backoff`** with `"status"` added (shape: `{"status":%d,"attempt":%d,"wait_ms":%u}`). Old defines `SESS_CM_403_*` replaced. **F8**: `run_session()` gains a `long jiggle_ms` parameter (before `au_dir`); `const uint32_t jiggle_interval` replaces both `SESS_JIGGLE_INTERVAL_MS` uses in the media loop and the gate becomes `jiggle_interval &&`; new flag `--session-jiggle=MS` (0–10000, default `SESS_JIGGLE_INTERVAL_MS` = 100; 0 disables the block — no `input_jiggle` line, `feedback.jiggle` stays 0); both call sites pass it; validation + usage() updated (the three XLIII usage lines also got their missing source indentation). Build stamp `XLIV`→`XLV`. 6744 → 6765 lines. No wire-format changes. |
| `docs/RTSP.md` | §33 — the full round-XLV record. |
| `START-HERE.md` | state → XLV; run-26 row; the XLV-shipped block; §4 = run 27 (jiggle-OFF lottery, same simple step-by-step); trap: transport channels APPEND across draws; history +XLV. |

## Integration notes / traps for the next agent

1. **`cm_create_backoff` replaced `cm_create_403_backoff`** (XLV):
   log parsers/regexes keyed on the old name must be updated; the new
   line carries `"status"` so 403 (seat) and 429 (rate limit) are
   distinguishable in evidence. Patience budget per draw: 20+40+80+160
   = 300 s worst case — a 5-draw lottery can now legitimately take
   ~30+ minutes without anything being wrong.
2. **The knlog channel files APPEND across lottery draws within one
   process** (field-corrected in run 26: `transport.log` held 44
   session_stats lines spanning draw 1 at 19:33Z and draw 4 at 19:39Z;
   draw 5's cm-failure added nothing). Rev 34/35's "overwritten per
   draw" was WRONG — `kn_log_init` appends. The `logs/` directory of a
   lottery run therefore holds the WHOLE run's knlog evidence (diag
   rotates at ~2 MB: `diag.1.log` + `diag.log`), and stdout still
   holds every draw's summary. Deliver both, as always.
3. **The jiggle is a VARIABLE again (F8)** — XLII's "jiggle stays ON
   as a controlled constant" is superseded by run 26's pattern (5/5
   jiggle-on draws in the band; 2/2 long draws jiggle-off). Until run
   27 answers it, do NOT cite "input eliminated" without the
   direction caveat: input is eliminated as a SAVIOUR (run 23),
   untested as a DEPRESSANT (run 27's job). Default remains 100 ms —
   the flag changes nothing unless asked.
4. **`wire_session.log` is back in the evidence set**: the ring dump
   only writes when exit ≠ 0, which the pre-F6 exit code suppressed on
   frozen sessions. It is draw-N's media ring (last failed draw with a
   ring); 167 KB in run 26 = draw 4's media bytes.
5. **SESSION_LENGTH_TIMER delivery is intermittent** (present runs
   23/25, absent 24/26×2): never treat its absence as a protocol
   regression, and keep F2's 2400 cap — when it arrives it is 567 B /
   1126 hex chars.
6. Everything from rev 35 stands (F6a exit-code contract: 0 = the
   draw survived; F6b lottery rc semantics; 403 = seat cooldown;
   run_session re-callability; drsAppName/drsProfileName in the 0x010a
   tails; knrx 218 raw / 243 cmake; summary-printf ritual — 112
   blocks).

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **NATIVE TESTS PASS** (per-suite counts unchanged; paranoid legs incl.) |
| `tools/cross_check.sh` ×2 arches | **CROSS-CHECK PASS**, zero warnings |
| ctest native / mbedTLS / schannel | **20/20 ×3** |
| T10 POSIX dry | **69 checks, 0 failed** |
| T13 | **ALL PASS 63** |
| summary-printf validator | **112** blocks, 0 mismatches; summary **74 = 74**; skeleton `json.loads` PASS (the renamed `cm_create_backoff` block re-validated in place) |
| F8 field tests | `--session-jiggle=10001` / `=-1` → usage exit 2; usage() renders the flag; `--lottery=5 --session-jiggle=0` on the TLS-less build → stops after draw 1 `rc:3`, stamp `"XLV"`, envelope `json.loads` 4/4 |
| `sandbox_session_e2e` | **43/43 PASS** (from the repo root — the binary is cwd-sensitive for the fake-rig spawn) |
| ASan/UBSan | carried from XLI.1 (no wire/decoder code touched XLIII–XLV) |

## Target expectations (Win7, run 27)

- **T10**: ALL PASS **79**, knrx **243**, kninput **548**,
  `input.vectors` **50** — unchanged (XLV touched no test file).
- First stdout line of every run: `"round":"XLV"`.
- **Run 27** (the jiggle-OFF lottery — one flag added to run 26's
  command):
  `knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=360 --lottery=5 --lottery-gap=30 --session-jiggle=0 --session-au-dir=logs\aus_r27 > logs\run27.log`
- Per draw: NO `input_jiggle` line, summary `feedback.jiggle` **0**
  (the off-switch proof); `cm_create_backoff {"status":403|429,...}`
  lines are NORMAL between draws (F7 patience — waits up to 160 s);
  envelope lines as in run 26. A surviving draw → `lottery_draw rc:0`
  + early stop + process exit 0.
- Deliver: `run27.log` (whole file) + the entire `logs/` directory
  (appends across draws — it holds the whole run) + the fresh T10 pair.

---

# INTEGRATION NOTES — Round XLIV delta, rev 35 (RUN 25 = THE LOTTERY'S FIRST LIVE FLIGHT — IT STOPPED AT DRAW 1 OF 5 AND THE REASON IS OUR OWN BUG, FOUND AND FIXED: `run_session`'s exit code returned **0 for any session that received ≥1 datagram and wasn't `socket_error`**, so `recovery_exhausted` exited 0 (runs 23–25 all did — the code was simply never consumed before F5) and the lottery's "rc 0 = keeper" rule misread the dead draw as a survivor. **F6a** makes the exit code honest per the documented contract (only the natural endings `"time"` / `"expect_frames"` can exit 0; every terminal/interrupted stop_reason exits 1) and **F6b** corrects the lottery stop rule (only rc 1 re-draws; rc 0 = keeper; rc 2/3 = abort; Ctrl-C aborts). The single draw still paid for the run: **F2 read the FULL `SESSION_LENGTH_TIMER` for the first time — `beforeEventMS: 3599657` ≈ a ONE-HOUR session budget — killing the session-limit theory of the early freezes**; the draw was the first LOSSY one on record (gaps 2, fec_repaired 6, nack 5, withheld 90, lossy_frames 5, yet 257/257 decoded; BWE all-time peak 45.9 Mbps) on a DIFFERENT rig IP (80.250.97.36); 0x0101 returned; the EA-Desktop drsProfile misclassification reproduced exactly; media died at 11.59 s — the ~10–12 s band is now **5 of 9 draws**. Stamp **XLIV**. Run 26 = the same command, this time it will actually draw five times.)

> **Rev 35 supersedes rev 1-34 as the current delta.** Canonical state:
> **`START-HERE.md`** (repo root). Design record: **`docs/RTSP.md` §32**
> (run 25 in full: the clone table, the SESSION_LENGTH_TIMER body, the
> F6 bug anatomy, the 9-draw distribution, the run-26 read matrix).

## Changed files (round XLIV)

| file | change |
|---|---|
| `streamer/src/knvstd_main.c` | **F6a**: the session exit-code block — old rule `(datagrams>0 && produced>0 && no au_write_errors && stop_reason != "socket_error") ? 0 : 1` replaced by `natural && datagrams>0 && produced>0 && no au_write_errors` where `natural = stop_reason is "time" or "expect_frames"`; the `expect_frames` under-delivery override stays. **F6b**: the lottery driver's `if (lrc == 0) break;` → `if (lrc != 1 \|\| g_knv_stop) break;` + the driver comment now states the rc semantics. Build stamp `XLIII`→`XLIV`. 6728 → 6744 lines. No wire-format, no protocol, no test-file changes. |
| `docs/RTSP.md` | §32 — the full round-XLIV record. |
| `START-HERE.md` | state → XLIV; run-25 row; the XLIV-shipped block; §4 = run 26 (same simple step-by-step shape); history +XLIV. |

## Integration notes / traps for the next agent

1. **The live-session exit code changed meaning (F6a)**: a session that
   ends `recovery_exhausted` / `server_termination` / `signal` /
   `queue_timeout` / cm-failures now exits **1** (it always should have
   per usage()'s "1 failed verdict" — runs 23–25 exited 0 only because
   nothing consumed the code). Exit **0** now MEANS "the draw survived
   its full `--session-seconds` (or met `--session-expect-frames`)".
   Any future scripting on knvstd's exit code can trust this. No gate
   consumes the live-session exit code (T10/T13/e2e never touch it).
2. **Lottery rc semantics (F6b)**: 0 = keeper (stop, the rig is
   alive — do NOT re-draw it away), 1 = dead draw (re-draw), 2/3 =
   usage/internal (stop — a re-draw cannot fix those), Ctrl-C = stop.
   The sandbox proof of the abort path: `--lottery=5` on the TLS-less
   POSIX build now ends after draw 1 with `last_rc:3`.
3. **`SESSION_LENGTH_TIMER` is read** (run 25, F2): `timerData
   {timerType:"DEFAULT", presentDurationMS:0, beforeEventMS:3599657}`
   — a one-hour budget granted ~1.7 s into the draw. Do NOT re-open
   session-limit/idle-timer theories for the ~10–12 s freezes; the
   server's own stated budget had 59+ minutes left. (Its arrival time
   varies per draw: run 23 ~12.9 s, run 25 1.67 s, run 24 absent.)
4. **`sctl_unknown code 526` = opcode 0x020e** — appears once per run
   (23/24/25); the reference's decode table does not know it either
   (falls into its Unknown bucket). Counted in `sctl_stats
   .unknown_commands`, harmless. Do not chase it without a reference
   update.
5. **The ~10–12 s band is the modal failure** (5 of 9 draws: runs 16,
   17, 23, 24, 25 — two rig IPs, with/without activation, jiggle,
   0x0101, packet loss, and with a 1-hour budget granted). Run 25 was
   also the first LOSSY draw (gaps 2, fec 6, nack 5, withheld 90) and
   decoded 257/257 anyway — the receive pipeline is now loss-proven
   live. The remaining unexplained variance is the long-draw mode
   (30.5/41 s): the lottery is the instrument; a gameplay-reaching app
   id remains the single most valuable missing input.
6. Everything from rev 34 stands (403 = seat cooldown + F4 backoff;
   lottery shares one process — run_session re-callability audit;
   knlog channels are overwritten per draw — stdout is the per-draw
   record; drsAppName/drsProfileName live in the 0x010a tails;
   knrx 218 raw / 243 cmake; summary-printf ritual — 112 blocks).

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **NATIVE TESTS PASS** (per-suite counts unchanged: knrx 218, kninput 548, knbw 387, knselftest 117; paranoid legs incl.) |
| `tools/cross_check.sh` ×2 arches | **CROSS-CHECK PASS**, zero warnings |
| ctest mbedTLS / schannel / native | **20/20 / 20/20 / 20/20** |
| T10 POSIX dry | **69 checks, 0 failed** |
| T13 | **ALL PASS 63** |
| summary-printf validator | **112** blocks, 0 mismatches; session summary **74 = 74**; skeleton `json.loads` PASS |
| F6b field test | `--lottery=5 --lottery-gap=1` on the TLS-less build: stops after draw 1, `lottery_draw rc:3` + `lottery_summary draws:1 of:5 last_rc:3`, exit 3, `json.loads` 4/4, stamp `"XLIV"` |
| `sandbox_session_e2e` | 43/43 carried from XLIII (e2e links no file XLIV touched) |
| ASan/UBSan | carried from XLI.1 (no wire/decoder code touched in XLIII/XLIV) |

## Target expectations (Win7, run 26)

- **T10**: ALL PASS **79**, knrx **243**, kninput **548**,
  `input.vectors` **50** — run 25's fresh T10 confirmed ALL PASS 79 on
  the XLIII build; XLIV changes no test file, so the counts stand.
- First stdout line of every run: `"round":"XLIV"`.
- **Run 26** (identical command shape to run 25 — only the au-dir/log
  names change):
  `knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=360 --lottery=5 --lottery-gap=30 --session-au-dir=logs\aus_r26 > logs\run26.log`
- Expected stdout: build stamp once; per draw the full session lines +
  summary + `lottery_draw`; `lottery_gap` between draws;
  `cm_create_403_backoff` whenever a seat cooldown fires (F4 — with a
  30 s gap some 403s are expected and fine); final `lottery_summary`.
  Five dead draws ⇒ `draws:5, last_rc:1` and process exit **1** (new
  F6a semantics — NOT an error, it means "no keeper this evening").
  A surviving draw ⇒ `lottery_draw rc:0`, early stop, exit **0**.
- Deliver: `run26.log` (ALL summaries live in it) + the `logs/`
  directory (last draw's channels) + the fresh T10 pair.

---

# INTEGRATION NOTES — Round XLIII delta, rev 34 (RUN 24 = LOTTERY DRAW #4 AND A NEAR-EXACT CLONE OF RUN 23: freeze at **10.43 s** of media vs 10.58 s, `recovery_exhausted` at wall 20.54 s vs 20.56 s, same app state (Arknights' Qt launcher portal), everything else matching to the counters. The round-XLII instruments paid off on first flight: **F3** proves ALL control traffic arrived inside the first 10 s (`ctrl_in rel_ms:10000`, 15 msgs, no 0x0101) — the freeze lands on an already-silent control plane; **F2** reads the eight-notification startup burst (t+257–264 ms) IN FULL — two payloads (936/970 hex chars) were truncated at the old 800 cap in runs 22–23 — exposing the rig's own classification: `QtWebEngineProcess.exe ... "drsProfileName":"EA Desktop"` on an Arknights session, final foreground `Games.exe fullscreen:false`. This draw carried NO `SESSION_LENGTH_TIMER` (run 23 had one — the notification set varies per draw; the full timer body is still unread). The ~10.5 s mode is now the plurality of the survival distribution (4 of 8 draws: 10.2/11.0/10.6/10.4 vs 30.5/41 and two ~1.2 s drips). The user's field probe closed the deferred restart-loop question: cm_create fired seconds after a recovery_exhausted exit returned a bare **HTTP 403** twice (cloud_status 0, empty desc) while the SAME token queued fine after a pause ⇒ **403 = seat cooldown, token reusable**. This round ships **F4** (cm_create 403 backoff: 20 s × 3 retries, TLS rebuilt per retry, `cm_create_403_backoff` event line) and **F5** (`--lottery=N` / `--lottery-gap=SEC`: one invocation re-draws the whole bring-up N times with `lottery_gap`/`lottery_draw`/`lottery_summary` envelope lines, early stop on rc==0; in-process after a full re-callability audit of `run_session()`). Stamp **XLIII**. Run 25 = the first unattended lottery, five draws in one command.)

> **Rev 34 supersedes rev 1-33 as the current delta.** Canonical state:
> **`START-HERE.md`** (repo root). Design record: **`docs/RTSP.md` §31**
> (run 24 in full: the clone table, the F2/F3 first yields, the
> EA-Desktop-profile discovery, the survival distribution, the 403 seat
> probe, the F4/F5 design, the run-25 read matrix).

## Changed files (round XLIII)

| file | change |
|---|---|
| `streamer/src/knvstd_main.c` | **F4**: the TLS-open → `kn_cm_create` → TLS-close sequence became a retry loop — on `http_status == 403` only (every other failure aborts instantly as before) print `cm_create_403_backoff`, sleep `SESS_CM_403_BACKOFF_MS` (20 000), rebuild TLS, retry up to `SESS_CM_403_RETRIES` (3) ⇒ ≤ 4 attempts / ≤ 60 s of backoff. **F5**: `--lottery=N` (1–100, default 1 = byte-identical legacy single-draw path) and `--lottery-gap=SEC` (0–3600, default 20) parsed + bounds-checked; for N > 1 main() loops `run_session()` with `lottery_gap` sleeps, a `lottery_draw {draw,of,rc}` line per draw, a final `lottery_summary {draws,of,last_rc}`, exit code = last rc, and early stop on rc == 0. usage() documents both flags. Build stamp `XLII`→`XLIII`. 6646 → 6728 lines. **No wire-format, no state-machine changes.** |
| `docs/RTSP.md` | §31 — the full round-XLIII record (run-24 clone table, the F2/F3 yields, the notification burst decoded, EA-Desktop profile, distribution update, 403 probe, F4/F5 design + audit, run-25 read matrix). |
| `START-HERE.md` | state → XLIII; run-24 row; the XLIII-shipped block; sandbox-gates block refreshed; §4 = run 25 as a simple step-by-step procedure (operator request); history +XLIII. |

## Integration notes / traps for the next agent

1. **A bare cm_create 403 is a seat cooldown, not a dead token**
   (run24v2 field probe: two instant 403s after exit, then the SAME
   token queued after a pause). F4 automates the pause. If all four
   attempts 403 (~60 s), the seat may be genuinely stuck server-side —
   the legacy `error:"cm_create"` line prints as before. The JSON-body
   sibling `cloud_status 11 = SESSION_LIMIT` is a different signal and
   is NOT retried by F4.
2. **Lottery draws share ONE process.** `run_session()` is re-callable
   by construction — every stateful static is re-initialised at entry
   (`memset &c`, `memset &pump` + `sess_fb_init(&fb)`) and destroyed
   before return (`sess_pump_stop` joins the worker,
   `sess_fb_destroy`); the kn_log init/shutdown cycle is the same one
   the selftest battery runs back-to-back in one process. The sandbox
   could only exercise the loop over FAILING draws (the POSIX build has
   no TLS backend — `tls_stream_unavailable`); two live sessions in one
   process is a run-25-first. If a second draw ever misbehaves at
   bring-up, this audit is the first place to look.
3. **The `logs/` knlog channels are overwritten by each draw** — with
   `--lottery`, stdout is the per-draw record (each draw prints its
   full summary + the envelope lines). Per-draw knlog channels would
   need a `--log-dir` per invocation; deliberately not automated.
4. **The rig's app classification is visible in the 0x010a tails**:
   `drsAppName`/`drsProfileName` (run 24: the Arknights session ran
   under the **EA Desktop** profile — the rig classifies by the window
   it sees, `QtWebEngineProcess.exe`, not by our cmsId). These fields
   were invisible before F2 (they sit past hex char 800).
5. **Survival distribution after 8 draws** (runs 16–24): ~10.5 s mode
   ×4 (10.2, 11.0, 10.6, 10.4), long ×2 (30.5, 41), drip ×2 (~1.2).
   Client fidelity is not the dial (runs 16/17 had no activation and
   no jiggle; 23/24 had everything — same modal lifetime). The lottery
   is now the instrument; run 25 (N=5) measures the distribution in
   one sitting.
6. Everything from rev 33 stands (knrx counts are RECIPE-dependent
   218 raw / 243 cmake+target; the fidelity ladder is dead; WSS is
   send-only in practice — pongs have never arrived; work from
   uploaded FILES, never pastes; the summary-printf ritual — now
   **112** JSON printf blocks, session summary **74 = 74**).

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **NATIVE TESTS PASS** — per-suite raw-recipe counts unchanged (knrx 218, kninput 548, knbw 387, knselftest 117, kat 126, scenarios 31/31), incl. the paranoid legs + jsonl/zip/json validators |
| `tools/cross_check.sh` ×2 arches | **CROSS-CHECK PASS**, zero warnings under `-Wall -Wextra -Werror` |
| ctest mbedTLS / schannel | **20/20 / 20/20** (knvstd relinked in both) |
| cmake-native ctest | 20/20 (knrx 243 recipe) |
| `sandbox_session_e2e` | **43/43 PASS** (rebuilt per the header recipe — the stale binary in `build/` pre-dated XLII) |
| T10 POSIX dry | **69 checks, 0 failed** |
| T13 | **ALL PASS 63** |
| summary-printf validator | **112** JSON printf blocks, 0 mismatches (+4 new: `cm_create_403_backoff`, `lottery_gap`, `lottery_draw`, `lottery_summary`); session summary **74 = 74**; skeleton `json.loads` PASS |
| lottery mechanics field test | `--lottery=3 --lottery-gap=2` against the TLS-less POSIX build: 3 draws, 2 gaps, wall 4.0 s (gap timing exact), envelope lines **json.loads 10/10**, exit code propagated (3); bounds `--lottery=0`, `--lottery=101`, `--lottery-gap=99999` → usage, exit 2; default single draw emits ZERO lottery lines |
| `--selftest=all` battery | all runnable suites ok (fec 88, srtp 52, input 50, gs 78, rx 126, sock 9), single-JSON contract intact |

ASan/UBSan legs were not re-run this round (no wire/decoder code
touched — F4/F5 live entirely in `knvstd_main.c`'s session driver, and
the XLI.1 sanitized run of knrx/kninput/knbw remains the reference).

## Target expectations (Win7, run 25)

- **T10**: ALL PASS **79**, knrx **243**, kninput **548**,
  `input.vectors` **50** — unchanged; round XLIII touched no test file.
- Every knvstd run's first stdout line: `"round":"XLIII"`.
- **Run 25** (five draws, 30 s apart, one command):
  `knvstd.exe --session --session-token=<tok> --session-app=107057914 --session-seconds=360 --lottery=5 --lottery-gap=30 --session-au-dir=logs\aus_r25 > logs\run25.log`
- Expected stdout shape: build stamp once; per draw k the usual
  queue/activation/jiggle/media/summary lines then
  `lottery_draw {"draw":k,"of":5,"rc":R}`; between draws
  `lottery_gap {"draw":k+1,"wait_ms":30000}`; on a seat collision
  `cm_create_403_backoff {"attempt":1..3,"wait_ms":20000}`; at the end
  `lottery_summary {"draws":K,"of":5,"last_rc":R}`. A draw that
  survives its full `--session-seconds` (or meets
  `--session-expect-frames`) returns rc 0 and STOPS the lottery.
- Deliver: `run25.log` (the whole file — five summaries live in it) +
  the `logs/` directory (last draw's channels) + the fresh
  `T10_skrot/wynik` pair.

---

# INTEGRATION NOTES — Round XLII delta, rev 33 (RUN 23 = THE DECISION-MATRIX RUN: **the XLI.1 jiggle flew — 198 relative-mouse reports on sid 6 from t+1.3 s, 0 drops — and the server froze EVERY plane anyway at 10.58 s of pristine 30.15 fps video** (319/319 decoded, zero gaps, BWE warm at 34.8 Mbps peak). Per RTSP §29.7 that is branch 2: **input is eliminated as a survival factor and the run-16→22 fidelity ladder is dead** — media survival across runs 16–23 (10.2, 11.0, 30.5, ~1.3, ~1.2, ~41, 10.6 s) is a rig-side lottery. **D8's recovery contract made its live debut exactly to spec**: stall+5 s → recover + 50 IDR/PLI → +5 s → terminal → `stop_reason:"recovery_exhausted"`, clean exit at 20.6 s instead of run 22's 248 s of dead air. The 0x010a sequence is IDENTICAL to run 22's: Arknights: Endfield never got past its own Qt launcher portal (`QtWebEngineProcess.exe`, foreground lost to pid 0) in EITHER run — the session was parked on a login screen while the encoder died at a random time. This round ships the two instrumentation gaps that cost run 23: **F2** the 0x010a hex cap 800→2400 (the 566-byte `SESSION_LENGTH_TIMER` body was truncated by our OWN cap in both runs — never read) and **F3** `rel_ms` on `ctrl_msg`/`sctl_010a`/`ctrl_in` (notifications could not be ordered against the freeze). Stamp **XLII**. Run 24 changes the one variable left: an app that REACHES GAMEPLAY without login.)

> **Rev 33 supersedes rev 1-32 as the current delta.** Canonical state:
> **`START-HERE.md`** (repo root). Design record: **`docs/RTSP.md` §30**
> (run 23 in full: the timeline, the per-second freeze table, the D8
> debut, the ladder-is-dead analysis, the run-24 plan) + **`docs/INPUT.md`
> §9** (the jiggle's live verdict).

## Changed files (round XLII)

| file | change |
|---|---|
| `streamer/src/knvstd_main.c` | **F2**: `sess_ctrl_dump_010a` hexbuf 801→2401, `%.800s`→`%.2400s` (covers every 0x010a seen — max 566 B — with 2× headroom; the 16-line cap stays). **F3**: `rel_ms` (session-relative ms, `(now_us − pump.origin_us)/1000` — the activation epoch) added to `ctrl_msg`, `sctl_010a`, `ctrl_in` (t_ms kept verbatim next to it). Build stamp `XLI.1`→`XLII`. No wire-format, no state-machine changes. |
| `docs/RTSP.md` | §30 — the full round-XLII record (run-23 verdict, target-count correction, the freeze tables, the 010a comparison, the lottery conclusion, run-24 plan). |
| `docs/INPUT.md` | §9 verdict note: the jiggle's live result (flew, freeze unchanged → input eliminated; jiggle stays ON as a controlled constant). |
| `START-HERE.md` | state → XLII; run-23 row; §4 = run 24 (different app, optional restart draws, corrected target counts); trap: knrx count is RECIPE-dependent; history +XLII. |

## Integration notes / traps for the next agent

1. **`test_knrx`'s check count is BUILD-RECIPE-dependent, not
   platform-dependent**: the raw-gcc script (`build_native_tests.sh`,
   subset link) reports **218**; the cmake/ninja build (T10 — sandbox
   dry AND Win7) links full `kncore` and reports **243** (+25). The
   XL/XLI docs' "expect 218 on target" was wrong; the T10 VERDICT
   (ALL PASS 79) was never affected. `test_kninput` 548 and
   `input.vectors` 50 are recipe-invariant (pure wire code).
2. **The fidelity ladder is DEAD — do not rebuild a theory on
   client-side completeness alone.** Runs 20 (~1.3 s media) and 21
   (~1.2 s) already contradicted monotonicity; run 23 (max fidelity
   INCLUDING input) landed at 10.6 s. Media survival is a rig-side
   lottery (1.2–41 s over seven runs). The one client-side variable
   never yet tested: an app that reaches GAMEPLAY (runs 20–23 all
   parked on Arknights' Qt launcher portal).
3. **WSS pongs have NEVER arrived in any run** (23: 10 pings/0 pongs
   in 20 s; 22: 108/0 over 216 s). Treat the WSS control channel as
   send-only in practice; `ctrl_ping_dead` dates its TCP-level death,
   not a pong regression.
4. F2's longer `sctl_010a` lines (~2.5 KB max) are still strict-JSONL
   single lines — but user PASTE truncation (~76 chars/line in run 18)
   would mangle them; always work from the uploaded FILE.
5. The 0x010a notification sequence (foregroundWindow → Launcher →
   Games.exe → Launcher-exit → QtWebEngineProcess → foreground pid 0
   → SESSION_LENGTH_TIMER) is the rig's app-lifecycle feed. Run 24's
   `rel_ms` ordering + the full timer body are the two new degrees of
   freedom; compare against run 23's §30.6 baseline.
6. Everything else from rev 32 stands (sid map 2×index, mixed
   endianness, the volatile-latch pattern, the sanitizer recipe, the
   generated-KAT rule, the summary-printf ritual — now 108 blocks).

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **18/18, 2378 checks** (raw recipe: knrx 218, kninput 548, knbw 387), incl. paranoid legs + jsonl/zip/json validators |
| paranoid knvstd ×9 + kn_rx/kn_bw/kn_input ×3 | clean, `-Werror` |
| `tools/cross_check.sh` ×2 arches | PASS, zero warnings |
| ctest mbedTLS / schannel | **20/20 / 20/20** (zero build warnings) |
| ASan+UBSan knrx / kninput / knbw | **218 / 548 / 387** clean |
| `sandbox_session_e2e` | **43/43 PASS** |
| T10 POSIX dry | **ALL PASS 69** (cmake recipe: knrx **243** = target — trap 1) |
| T13 | **ALL PASS 63** |
| summary-printf validation | **108** JSON printf blocks specs=args; session summary **74 = 74**; skeleton `json.loads` PASS; the 3 new `rel_ms` line shapes `json.loads` 3/3 |

# INTEGRATION NOTES — Round XLI.1 delta, rev 32 (THE OPERATOR'S ANSWERS KILLED THE TAKEOVER THEORY AND REVIVED THE IDLE/NO-INPUT POLICY AS THE PRIME RUN-22 FREEZE SUSPECT: the web client was NEVER attached during the run — ours was the sole receiver — the session app matched the 0x010a shortName (107057914), and ZERO input ever reached the session. Survival is monotone in client fidelity (16/17 ~10-11 s → 18 ~30.5 s → 22 ~45 s) with exactly one item missing, so START-HERE §4.4's reconsideration condition is met and the deferred synthetic-input slice ships NOW: `INPUT_MOUSE_RELATIVE` byte-exact against the reference's OWN Rust golden (nvst_input.rs:1888-1911), routed to control_partial (sid 6, unordered PR-SCTP TTL 300 ms, nvst_input.rs:969 + NVST_CHANNEL_PROFILE[3]), sent as a gated 10 Hz ±1 px zero-net-drift jiggle from the media loop. Build stamp XLI → **XLI.1**. Run 23 decides: survival past ~45 s ⇒ idle policy confirmed; freeze with the jiggle demonstrably flying ⇒ input eliminated, rig-side freeze — and round XLI's D8 recovery contract dates it.)

> **Rev 32 supersedes rev 1-31 as the current delta.** Canonical state:
> **`START-HERE.md`** (repo root). Design record: **`docs/RTSP.md` §29**
> (the answers, the hypothesis ladder, the wire format, the decision
> matrix) + **`docs/INPUT.md` §9** (G3's first slice in full).

## Changed files (round XLI.1)

| file | change |
|---|---|
| `tools/input_reference.py` | NEW `mouse_relative(dx, dy, ts_us)` builder (remote_input_packet(7, dx BE i16 + dy BE i16 + 00 00) → remote_input_message, ONE LE-u64 ts tail); `self_prove()` pins the reference's OWN byte-exact Rust golden (nvst_input.rs:1888-1911, dx=dy=24 @ ts=0x01861330) + the negative/extreme BE-i16 encodings + wire offsets; two new emitted singles (`mouse_rel`, `mouse_rel_neg`) → 13. |
| `streamer/src/kn_input_kat_vectors.h` | **regenerated** (13 singles) — do not edit by hand. |
| `streamer/include/kn/kn_input.h` | `kn_input_mouse_relative` decl + the routing contract in the comment (sid 6, NOT the reliable sid 0). |
| `streamer/src/kn_input.c` | `kn_in_put_be16` helper + `kn_input_mouse_relative` (the haptics twin: inner 14 B → pad-24 → +1 LE-u64 ts → remote_input_packet(0x0e) 40 B → TLV 0x0206 = 44 B total). |
| `streamer/tests/test_kninput.c` | singles dispatch +2; `kn_input_kat_singles_n` 11→**13**; NEW `t_mouse_relative_golden` (the Rust 44 bytes inline, BE-i16 −1/−2 + −32768/32767, the LE-u64 ts tail, −2/−1 buffer contracts). **446 → 548 checks.** |
| `streamer/src/kn_selftest.c` | `input.vectors` singles dispatch +2 → **48 → 50 checks** from the SHIPPED binary (identical on Win7 — no backend split). |
| `streamer/src/knvstd_main.c` | `SESS_JIGGLE_INTERVAL_MS` (100); cadence ctx `next_jiggle_ms`/`jiggle_dx`/`jiggle_first_logged`/`fb_jiggle`/`jiggle_build_fail`; pump `volatile unsigned input_active` mirror (single writer `sess_ctrl_try_activate`, the `sctl_*` pattern) + its set on chain success; the media-loop jiggle block after the keepalive cadence (gate `pump_persist && pump.input_active`, `sess_fb_push(fb, 6, …)`, alternating ±1 px, ts on `pump.origin_us`); `input_jiggle`/`first_send` stdout line; summary `feedback.jiggle` (**73 → 74** specs=args); build stamp `XLI`→`XLI.1`. |
| `docs/INPUT.md` | G3 → **partial**; NEW §9 (the wire table, the golden, routing, cadence, evidence, the run-23 decision); counts 548/50. |
| `docs/RTSP.md` | §29 — the full round-XLI.1 record. |
| `START-HERE.md` | state → XLI.1; run-23 plan updated (the takeover branch is DEAD; the freeze branch now self-labels via D8). |

## Integration notes / traps for the next agent

1. **control_partial is sid 6, NOT sid 4.** NVST_CHANNEL_PROFILE's
   creation INDEX (3) and its SID (6) differ — every channel's sid is
   2×index (0,2,4,6,8,10,12,14). `sess_fb_sid_params` case 6 already
   carries the sender-side policy (unordered + PR-TTL 300); a mouse
   message on sid 0 (reliable/ordered) would be a fidelity break the
   reference's own comment warns about (one lost report stalls every
   newer one for 300 ms).
2. **Relative-mouse endianness is mixed by design**: dx/dy are BE i16
   (signed — negatives are 0xff..; the i16 extremes are test-pinned),
   the inner type/lengths are LE32/BE32 per the envelope, and the
   timestamp tail is ONE LE u64 (key/absolute carry TWO). The Rust
   golden's native packet stores its ts BE u64 at native[14..22] but
   the wire tail is LE — do not "harmonize".
3. **The jiggle gate reads `pump.input_active`, never
   `pump.activation_sent`** from the main thread — the volatile mirror
   is the sanctioned single-word cross-thread read (the `sctl_*`
   precedent); `activation_sent` is pump-private.
4. **Sanitizer recipe**: sanitize the `kn_*` sources + tests, build
   third_party objects UNSANITIZED and link — crypto-algorithms'
   `sha1.c:26` trips UBSan's shift check on every call (pre-existing,
   by that library's design; not ours to fix).
5. The KAT header is GENERATED — `python3 tools/input_reference.py`
   after any builder change; regeneration must stay byte-identical and
   `self_prove()` must pass BEFORE emission (vectors-are-trusted rule).
6. Summary printf trap #5 still applies: specs=args is now **74=74**;
   `/tmp/check_summary_printf.py` (session-local) validates 108 JSON
   printf blocks. Any new summary field moves that count.

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/input_reference.py` self-proof | 8/8 activation + **1/1 relative-mouse Rust golden** byte-exact; 7 parser + 7 cursor vectors |
| `tools/build_native_tests.sh` | **18/18, 2378 checks** (2276 + 102; kninput 446→**548**), incl. paranoid legs, jsonl+zip+json validators |
| `knvstd --selftest=input.vectors` | **50 checks, 0 failures** (shipped-binary re-proof) |
| paranoid knvstd ×9 (c11/c17/gnu11 × pedantic / format-truncation=2 / unused-but-set+shadow) | clean, `-Werror` |
| kn_rx / kn_bw / kn_input ×3 | clean |
| `tools/cross_check.sh` ×2 arches | PASS, zero warnings (incl. the `-DKN_TLS_BACKEND_MBEDTLS=1` legs) |
| ctest, mbedTLS backend | **20/20 PASS** |
| ctest, schannel backend | **20/20 PASS** |
| ASan+UBSan knrx / kninput / knbw | **218 / 548 / 387** clean (third_party unsanitized — trap 4) |
| `sandbox_session_e2e` | **43/43 PASS** |
| T10 POSIX dry | **ALL PASS 69** (target expectation unchanged: **79**) |
| T13 | **ALL PASS 63** |
| summary-printf validation | all **108** JSON printf blocks specs=args; session summary **74 = 74**; strict-JSON skeleton `json.loads` PASS; the new `input_jiggle` line + `feedback.jiggle` key `json.loads`-verified |

# INTEGRATION NOTES — Round XLI delta, rev 31 (RUN 22 = THE LOOP FIX PROVEN LIVE AND THE FIRST SUSTAINED REAL-RATE GFN VIDEO: `loop_ticks` 42 882 = 143/s, `recv_timeouts` 27 095 = ticks − datagrams − 1 — the run-20/21 paradox inverted into the proof; QoS 5861 = 19.5/s; 15 786/15 786 datagrams authenticated; 1139 AUs of which 1013 = a continuous 29.98 fps @ ~3.7 Mbps stream, zero media gaps, zero decode errors; the BWE estimator left warm-up for the first time (frames_observed 1136 ⇒ estimate 25 398 kbps); the 0x010a mystery DECODED by F1 (JSON `NvExtendedCommandMessage` app-lifecycle notifications — no client response expected). THEN THE SERVER FROZE EVERY PLANE at ~t+30–45 s: video, the ~1 Hz 0x0101 heartbeat, WSS pongs; no termination/idle-warning/teardown; the WSS only errored at ~t+216 s (TCP retransmit exhaustion); 2535 reliable IDR+PLI retries went unanswered. Run 22 also exposed OUR dead end: knvstd never called `kn_rx_video_recover` — RECOVERY_REQUIRED drops every packet forever. This round ships **D8, the reference's bounded recovery contract** (`attempt_nvst_recovery`, limit 1, terminal `recovery_exhausted`), the `ctrl_ping_dead` evidence line, the summary `recovery` block, and the XLI build stamp.)

> **Rev 31 supersedes rev 1-30 as the current delta.** Canonical state:
> **`START-HERE.md`** (repo root). Design record for run 22 + round XLI:
> **`docs/RTSP.md` §28** (evidence, the freeze forensics, the reference
> recovery contract, the run-23 plan + the open user-procedure questions).

## Changed files (round XLI)

| file | change |
|---|---|
| `streamer/src/knvstd_main.c` | **D8**: `dec_recovery_attempt` (the reference's `attempt_nvst_recovery`, opennow-streamer-core lib.rs:2448-2477, LIMIT=1 :127) + `SESS_RECOVERY_ATTEMPT_LIMIT`; `dec_drain_events` captures `RECOVERY_TIMEOUT`/`PROGRESS_RECOVERY` and acts AFTER the print loop (recover() does ev_reset+lifecycle_push — acting mid-iteration would rewrite the array being walked); session media loop breaks on `recovery_exhausted` → `stop_reason:"recovery_exhausted"` (poll path only — the sole origin of those events); `dec_ctx` + `recovery_attempts`/`recovery_exhausted`; gated on `c->fb` (session mode only — `--decode`/`--replay-capture` keep their historical behaviour, T13 untouched). `ctrl_ping_dead` JSONL at BOTH dead transitions (send_failed / recv+rc). Summary grows the trailing `recovery` block (71→73 specs=args). Build stamp `XL`→`XLI`. |
| `streamer/tests/test_knrx.c` | NEW `test_round_xli_recovery` (+15 checks → **218**): the 250 ms timeout transition (249 silent), the RECOVERY_REQUIRED drop guard (reason 3), `recover()` contract (→1, RUNNING, Lifecycle event, origin re-anchored, no-op from RUNNING), **the keyframe latch SURVIVES the media reset** (the D3 retry loop's lifeline), kn_rx recover is NOT attempt-limited (the limit is the session layer's). `prop_h_init` → delegates to NEW `prop_h_init_to` (timeout param); all existing call sites unchanged. |
| `tools/sandbox_session_e2e.c` | header build recipe fixed: +`kn_bw.c` (the round-XXXVIII link dep was missing from the comment — the binary recipe, not the harness, changed). |
| `docs/RTSP.md` | §28 — the full round-XLI record (run-22 evidence, the 0x010a decode table, the media timeline, the freeze forensics + the three candidates, the reference recovery contract, D8/D-t13, gates, the run-23 plan + questions). |
| `docs/RX.md` | recovery-contract note + the D-t13 deferral pointer (decode-side watchdog). |
| `START-HERE.md` | state → round XLI closed; run-22 row; §4 next = target T10 (expect ALL PASS 79, knrx 218) + the run-22 procedure answers + run 23 (stdout **AND `logs/`**); history table +XLI. |

## Integration notes / traps for the next agent

1. **The recovery action must run AFTER the event-print loop** —
   `kn_rx_video_recover()` calls `ev_reset()` + `lifecycle_push()`,
   i.e. it REWRITES the event array `dec_drain_events` is iterating.
   The pattern (capture intent in the loop, act after it) is now the
   contract; do not "simplify" it into the loop body.
2. `RECOVERY_TIMEOUT` / `PROGRESS_RECOVERY` are emitted ONLY by
   `kn_rx_video_poll` — never by `process_datagram`/`process_packet`
   (those only drop in non-RUNNING states). The terminal-break wiring
   sits on the poll path for that reason; a future refactor moving the
   check must keep it after EVERY drain that can follow a poll.
3. The attempt LIMIT lives in the session layer (`dec_ctx`), NOT in
   kn_rx — `kn_rx_video_recover()` is deliberately unlimited (the
   reference's receiver.recover() is too; lib.rs owns the budget).
   Test-pinned both ways.
4. `stop_reason:"recovery_exhausted"` keeps the historical exit-code
   policy (video produced ⇒ exit 0, like `server_termination`) — the
   verdict lives in the summary, not the process code.
5. Run-22-class freeze forensics from stdout alone: heartbeats frozen
   across ctrl_in snapshots + `pings × 2 s` ⇒ WSS death moment +
   `2535 × 100 ms` ⇒ last-datagram moment. Run 23's `ctrl_ping_dead`
   line and the (MANDATORY) `logs/` 1 Hz `session_stats` make both
   direct.
6. `min_server_time` > `median_server_time` in the bwe block is
   CORRECT, not a bug: `server_time(transit) = (now_ms − transit) × 90`
   (nvst_bandwidth.rs:220-227) — the MINIMUM transit maps to the
   MAXIMUM server_time; the median rides the ring-16 of recent transits.
   Do not "fix" the ordering.

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **18/18, 2276 checks** (knrx 203→**218**), incl. paranoid legs, jsonl+zip+json validators |
| paranoid knvstd ×9 (c11/c17/gnu11 × pedantic / format-truncation=2 / unused-but-set+shadow) | clean, `-Werror` |
| kn_bw/kn_rx ×3 | clean |
| `tools/cross_check.sh` ×2 arches | PASS, zero warnings (incl. the `-DKN_TLS_BACKEND_MBEDTLS=1` legs) |
| ctest, mbedTLS backend | **20/20 PASS** |
| ctest, schannel backend | **20/20 PASS** |
| ASan+UBSan knrx / knbw | **218** / 387 clean |
| `sandbox_session_e2e` | **43/43 PASS** |
| T10 POSIX dry | **ALL PASS 69** |
| T13 | **ALL PASS 63** |
| summary-printf validation | all 107 JSON printf blocks specs=args; session summary **73 = 73**; strict-JSON skeleton `json.loads` PASS; the 4 new stdout line shapes `json.loads` 4/4 |

# INTEGRATION NOTES — Round XL delta, rev 30 (THE RUN-18→20 REGRESSION ROOT-CAUSED AND FIXED: Windows `SO_RCVTIMEO` rounds to the AFD timer granularity (~500 ms), so the round-XXXVIII R3-D WSS control drain — `kn_ws_recv_msg(ws, 1 ms)` from the media loop's 50 ms beat — blocked ~500 ms per tick and phase-locked the ENTIRE loop to the server's 2 Hz datagram cadence: run 21 QoS "20/s" actually flew at 597/300 s = 2/s, NATT 597, and `recv_timeouts=1` against a 10 ms poll was the paradox that named it (a live tick would show ~29 400). Run 18 is the control: QoS 16.4/s on XXXVII code. Fix: `select()`-based bounded recv in `kn_net_tls_stream_recv` (ms-accurate on winsock) with a `mbedtls_ssl_check_pending()` guard; `SO_RCVTIMEO` remains only as the partial-record safety net. Ships with the full D1–D7 reference-fidelity set (reliable-sid IDR + the 100 ms retry loop, real RFC 3550 RR jitter, frame-ack `first_packet_time_ms`, standalone pacing before ack, NACK mode split, RFI sid 0), C18 build stamp, C19 `loop_ticks` telemetry, F1 the 0x010a payload dumper, and +9 knrx goldens. `kn_sock.c` audited and CORRECT — untouched.)

## Changed files (round XL)

| file | change |
|---|---|
| `streamer/src/kn_net_mbedtls.c` | **THE FIX** — `kn_net_tls_stream_recv` (Win32 body): `select()` before `mbedtls_ssl_read`, `mbedtls_ssl_check_pending()` guard, `SO_RCVTIMEO` kept as safety net; return semantics unchanged (0 retryable / −2 EOF-close / −1 fatal) |
| `streamer/src/knvstd_main.c` | D1/D2 IDR+RFI → sid 0; D3 `dec_kf_service` 100 ms armed retry (IDR sid 0 + PLI sid 14 when `rtcp_fb_open && have_bound_ssrc`; episode/attempt logging; failed pushes consume the interval) + `SESS_IDR_RETRY_INTERVAL_MS`; `rtcp_fb_open` initialized at session start (`!unified_mode && desc.rtcp_on_sctp`); D4 RR gate + 6-arg `rr_snapshot` call; D5 NACK mode split; D6 frame-ack `first_packet_ms` (clamped ≥ 0 vs `transport_origin_ms`); D7 standalone pacing push (sid 6, 55.556 ms, before ack); C18 build stamp (run modes only — utility modes keep their single-JSON-document stdout contract); C19 `loop_ticks` (media loop → `session_stats` jsonl + summary); F1 `sess_ctrl_dump_010a` self-contained TLV walker (≤ 16 lines/session, ≤ 800 hex chars) |
| `streamer/src/kn_rx.c` | D4 RFC 3550 interarrival-jitter EWMA (relative arrival ×90, ZERO-EXTENDING u32 wrap on `ts − first_ts`, first packet latches only, `ts == last` skips, /16, clamp+truncate) feeding `rr_snapshot`'s new 6th out-param; D6 `cur_au_first_ms` at the frame boundary → `kn_rx_frame.first_packet_ms`; D3 `kn_rx_video_keyframe_request_pending` (non-clearing latch read) |
| `streamer/include/kn/kn_rx.h` | `first_packet_ms` in `kn_rx_frame`; jitter timing fields; 6-arg `rr_snapshot`; pending-latch decl |
| `streamer/src/kn_rx_kat_runner.h` | shared collector: `first_ms[]` kept OUTSIDE `rxt_frame` in a parallel array — P2/P3/P5 memcmp whole `rxt_frame` structs for content identity across feed orders, and arrival wall clocks legitimately DIFFER between feeds (this trap bit mid-round; the array is the fix) |
| `streamer/tests/test_knrx.c` | `rr_snapshot` NULL-safety → 6 args; NEW section `test_round_xl_feedback`: jitter golden **369** (hand-derived 131.25 → 254.296875 → 369.60546875 → trunc), per-frame `first_packet_ms`, pre-bind snapshot 0, pending non-clearing + take-clears contract (203 checks total) |
| `docs/RTSP.md` | §27 — the full round-XL record (evidence tables, forensics, root cause, fixes, run-22 plan) |
| `START-HERE.md` | state → round XL closed; run-22 plan; new traps (§8.x): SO_RCVTIMEO granularity, rxt_frame memcmp, build-stamp vs single-document stdout |

## Integration notes / traps for the next agent

1. **`SO_RCVTIMEO` on Windows is ~500 ms-granular (AFD timer); `select()`
   is ms-accurate.** Any "short bounded blocking recv" on winsock MUST go
   through select/poll, never SO_RCVTIMEO. This single line cost runs
   20+21 and two rounds of hypotheses.
2. `kn_net_tls_stream_recv` has THREE bodies: the real Win32+mbedTLS one
   (kn_net_mbedtls.c — the production path; cmake default and target
   backend are mbedTLS, CMakeLists :91), the honest `!MBEDTLS` stub
   (kn_net.c, returns −1) and the POSIX stub (kn_net_mbedtls.c tail).
   A fix must land in the Win32 body; only the cross `-DKN_TLS_BACKEND_
   MBEDTLS=1` legs compile it in the sandbox — no runtime test possible
   off-target, `loop_ticks` in run 22 is the witness.
3. `recv_timeouts=1` + iterations = datagrams+1 + every periodic counter
   at datagram-rate or an integer divisor = **loop pinned to an external
   cadence**. With C19 shipped, run 22 states it directly (`loop_ticks`
   ≈ 100/s healthy).
4. Shared test structs that property tests memcmp must not gain
   feed-order-dependent fields — use parallel arrays in the collector
   (rxt_frame/rxt_col precedent).
5. The stdout build stamp must skip `--version/--list-selftests/
   --selftest/--diagnostics`: the native gate `json.load`s those captures
   as SINGLE documents; one extra leading line = "Extra data" + the
   diag-bundle path check fails.
6. The server drip has TWO independent depressants: our 2 Hz feedback
   (fixed) and the idle/static game screen (`rtp_ms` 1182 vs 300 s wall —
   an idle GFN encoder emits ~nothing regardless of the client). Run 22
   is only interpretable with the user ACTIVE at handoff.

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **18/18, 2261 checks** (knrx 194→**203**), incl. paranoid legs, jsonl+zip+json validators |
| paranoid knvstd ×9 (c11/c17/gnu11 × pedantic / format-truncation=2 / unused-but-set+shadow) | clean, `-Werror` |
| kn_bw/kn_rx ×3 | clean |
| `tools/cross_check.sh` ×2 arches | PASS, zero warnings — incl. the `-DKN_TLS_BACKEND_MBEDTLS=1` legs (the fixed TLS body) and the real usrsctp link |
| ctest, mbedTLS backend | **20/20 PASS** |
| ctest, schannel backend | **20/20 PASS** |
| ASan+UBSan knbw / knrx | 387 / 203 clean |
| `sandbox_session_e2e` | **43/43 PASS** |
| T10 POSIX dry | **ALL PASS 69** |
| T13 | **ALL PASS 63** |
| summary-printf validation | all 99 JSON printf blocks specs=args; session summary **71 = 71**; strict-JSON skeleton `json.loads` PASS |

# INTEGRATION NOTES — Round XXXIX delta, rev 29 (TARGET PROOF + REPO CLEANUP: T10 **ALL PASS 79 on Win7** — test_knbw reported 387 checks on target = sandbox, so round XXXVIII is fully target-verified. R7 run 20 flew the whole new evidence set and PROVED the feedback plane healthy (bwe skip_* all zero, batch flushes 796 / failed 0 / retained 0, outbox dropped 0 on every sid, terminations 0, pacing never disabled, cping never dead) — yet the server drip-fed video from the FIRST SECOND: 40 AUs in 400 s (1.275 s of media time) in ~5 s bursts, then one gap → RECOVERY_REQUIRED until the end with 42 unanswered IDR/PLI. `frames_observed:40 < 100` ⇒ estimate 0 is the reference's own warm-up zero. STAGE 2 REMAINS OPEN; the decisive missing evidence is the 1 Hz `session_stats` series, which lives in the knlog `logs/` files, not on stdout. This round also reorganizes the repository into its final layout and fixes the `outbox_by_sid` JSON-braces bug run 20 caught.)

> **Rev 29 supersedes rev 1-28.** Canonical state: **`START-HERE.md`** (repo root).
> Design record for run 20: **`docs/RTSP.md` §26** (timeline, verdict,
> hypotheses H1/H2, the run-21 plan).
>
> ## The round-XXXIX repo layout (the big move)
>
> | old path | new path |
> |---|---|
> | `PROJECT-STATE.md`, `HANDOFF.md`, `AGENT-HANDOFF.md` | `docs/archive/` |
> | `analiza/` | `docs/archive/analiza/` |
> | `INTEGRATION.md` (this file) | `docs/INTEGRATION.md` |
> | `BOOTSTRAP.md` | `docs/BOOTSTRAP.md` |
> | `src/` (openh264 + d3d9/dxva2 reference headers, unused by the build) | `docs/reference/` |
> | `python/requirements.txt` | `tests/requirements.txt` |
> | `tools_check_c_defs.py` / `tools_check_d3d_calls.py` / `tools_test_json_layer.sh` / `tools_verify_c.sh` | `tools/check_c_defs.py` / `tools/check_d3d_calls.py` / `tools/test_json_layer.sh` / `tools/verify_c.sh` |
> | root `T10_wynik.txt`, `T10_skrot.txt`, `T13_*` | `logs/T10_wynik.txt` etc. |
> | `t10logs/ t11logs/ t12logs/ t13logs/ t13work/ testlogs/ testlogs2/ testdiag/ testst/ knvstd_testlogs/` | `logs/t10/ logs/t11/ logs/t12/ logs/t13/ logs/t13work/ logs/testlogs/ logs/testlogs2/ logs/testdiag/ logs/testst/ logs/selftest/` |
> | `build-network/` (T12) | `build/network/` |
> | ffmpeg dev tree on the user's Desktop | **`build/ffmpeg/`** (tracked!) |
> | `aus_real_test/` (322 files, 5 MB, tracked since before it was ignored) | **untracked** (`git rm --cached`; kept on disk, ignored) |
> | (no README) | **`README.md`** — layout, builds, FFmpeg/third_party provisioning, conventions, acknowledgements & licences |
>
> Root now contains exactly: `README.md`, `START-HERE.md`, `CMakeLists.txt`,
> `.gitignore` + the directories `docs/ streamer/ tests/ tools/ third_party/
> build/ logs/` (the last four ignored except `build/ffmpeg/`).
>
> ## New files
>
> | file | what |
> |---|---|
> | `README.md` | the public front page: what the project is, the layout tree, Win7/sandbox build+test quick starts, the FFmpeg and third_party provisioning rules, the conventions (logs/, build/, zero warnings, two-implementations rule), and the full acknowledgements/licence table (OpenNOW MIT © 2025 Zortos; FFmpeg LGPL-2.1+; mbedTLS Apache-2.0 OR GPL-2.0+; usrsctp BSD-3; tiny-AES-c public domain; Brad Conte SHA-1 as-is; OpenH264/mingw headers reference-only) |
> | `build/ffmpeg/README.md` | the drop-here instructions for the user's prebuilt FFmpeg tree (static libs mandatory; the one TRACKED path under build/) |
> | `docs/archive/README.md` | guide to the frozen archive (what each doc is + the note that paths inside describe the pre-XXXIX layout) |
>
> ## Changed files
>
> | file | change |
> |---|---|
> | `.gitignore` | rewritten around the conventions: `/build/*` + `!/build/ffmpeg/`, `/logs/`, `/third_party/`, captures, python, harness scratch, agent scratch, a legacy section for the pre-cleanup dirs; dropped dead entries (`src/opennow*`, explicit per-file T-verdict lines now covered by `/logs/`) |
> | `CMakeLists.txt` | `KN_FFMPEG_PREFIX` auto-detect: empty + `build/ffmpeg/include/libavcodec/avcodec.h` exists → use `build/ffmpeg` |
> | `streamer/src/knvstd_main.c` | **the run-20 catch**: summary `"outbox_by_sid":%s` → `"outbox_by_sid":{%s}` and the empty-buffer special case removed (it used to emit `{}` into a `%s` that now carries its own braces → would have become `{{}}`). The summary line is strict JSON again |
> | `tests/T10_build_and_test.py` | `LOGDIR=logs/t10` (pre-created), verdicts → `logs/`, FFmpeg candidates = env override + `build/ffmpeg` (the two Desktop paths DELETED), docstring |
> | `tests/T9/T11/T12/T13` | same treatment: log/work dirs under `logs/`, verdicts under `logs/`, T12 build dir → `build/network`, pre-`makedirs` where knvstd's single-level mkdir needs the parent |
> | `tools/build_native_tests.sh` | `mkdir -p build logs`; knvstd smoke evidence → `logs/` (`--log-dir=logs/selftest`); check_jsonl/check_zip/json-parse legs → `logs/...`; the final `rm -rf` is precise (never wipes the user's manual logs) |
> | `streamer/tests/test_port.h` | + `kn_test_mkdir` (portable single-level mkdir) |
> | `streamer/tests/test_knlog.c` / `test_kndiag.c` / `test_knselftest.c` / `test_knring.c` | scratch dirs moved under `logs/` (`logs/testlogs{,2}`, `logs/testdiag`, `logs/testst`, `logs/testring_dump.txt`, `logs/selftest_render_check.json`); each binary creates `logs/` itself (knlog's own mkdir is single-level) |
> | `docs/BOOTSTRAP.md`, `docs/SOCK.md`, `tests/README.md`, `tests/T4_d3d9_present.c`, `tools/verify_c.sh`, `tools/test_json_layer.sh`, 6 streamer comment headers | path references updated to the new layout |
> | `START-HERE.md` | §2 reading order, §3 run-20 row + the XXXIX ship list, §4 rewritten (run-20 knlog evidence → round XL investigation → run 21), §8 trap 5 extended (the braces lesson: eyeball a REAL emitted summary once per round), §9 the new repo map, §10 round XXXIX row, §11 "stdout + logs/ = the full evidence" |
> | `docs/RTSP.md` | §26 (run 20) |
>
> ## Gate results (sandbox, re-run after the reorganization)
>
> | gate | result |
> |---|---|
> | `tools/build_native_tests.sh` | **NATIVE TESTS PASS** — 18 suites, **2252 checks** (knbw 387, kninput 446, knws 88, knrx 194 + its 126-check KAT battery 31/31); all evidence now under `logs/` |
> | `tools/cross_check.sh` | **PASS ×2** (x86_64 + i686), zero warnings under `-Wall -Wextra -Werror` |
> | ctest (cmake) | **20/20 mbedTLS + 20/20 schannel**, zero warnings |
> | paranoid matrix | knvstd_main ×9 (c11/c17/gnu11 × pedantic / format-truncation=2 / unused-but-set+shadow) **clean** + kn_bw/kn_rx ×3 clean. The `-Wshadow` leg (beyond the declared gate) caught ONE pre-existing shadow from round XXXVIII (`sess_local_identity`: inner `size_t i` under outer `size_t i`) — fixed (inner → `j`), re-verified ×3 |
> | ASan+UBSan | test_knbw 387/387 + test_knrx 194/194 clean |
> | `sandbox_session_e2e` | **43/43** (fake_rig over real loopback sockets) |
> | T10 POSIX dry | **ALL PASS 69** (diag bundle lands in `logs/t10/`) |
> | T13 | **ALL PASS 63** |
> | summary printf validation | 70 specs = 70 args; strict-JSON skeleton `json.loads` PASS (both bare `%s` — drops + outbox_by_sid — now in braced map position) |
>
> ## Integration traps (new in rev 29)
>
> 1. **`build/ffmpeg/` is the ONE tracked path under `build/`** — the
>    .gitignore uses `/build/*` + `!/build/ffmpeg/`. Do not "simplify"
>    either line or the user's FFmpeg tree silently leaves the repo.
> 2. **knlog's mkdir is single-level** (`CreateDirectoryA`/`mkdir`) — any
>    harness passing `--log-dir=logs/x` must create `logs/` first (the
>    T-scripts `os.makedirs`, the test binaries `kn_test_mkdir("logs")`,
>    build_native_tests.sh `mkdir -p logs`).
> 3. **A live run's evidence is stdout PLUS the `logs/` knlog files** —
>    the 1 Hz `session_stats` (bwe evolution, cping_*) only exist there;
>    run 20's stdout-only capture could not answer cause-vs-effect.
> 4. Frozen archives (`docs/archive/**`, `docs/research/**`, old
>    INTEGRATION revs) intentionally keep pre-XXXIX paths — they are
>    historical records; do not "fix" them.

# INTEGRATION NOTES — Round XXXVIII delta, rev 28 (R7 run 18 = THE ACTIVATION CHAIN WORKED: the stall moved 10.99 s -> 30.5 s for the first time — 562 AUs, 13 keyframes, 6665/6665 authenticated, 0 decode errors, feedback present the whole 300 s — and the AU frame-size decay under zero loss / flat RTT names the next gate: the server rate control ramping down against our ZERO bandwidth-estimator fields in QoS 0x0207 (D-t10). The nvst_bandwidth.rs port ships, plus the evidence set run 18 demanded: ControlReportBatch + per-sid outbox counters (the dropped:890 fix), server-control latches (0x109 termination / 0x112 pacing), SCTP cause codes, AU wall-vs-ts telemetry, R3-D the WSS control keepalive)

> **Rev 28 supersedes rev 1-27.** Canonical state: **`START-HERE.md`** (the living handoff since round XXXVIII; `PROJECT-STATE.md` is the frozen rounds I–XXXVIII archive).
> Design records: **`docs/RTSP.md` §25** (NEW — run 18, the diagnosis,
> the round-XXXVIII evidence set, the run-19 decision matrix),
> `docs/INPUT.md` (the control/input plane, unchanged),
> `docs/SCTP.md` (the association layer + the new cause codes).
>
> **Round XXXVIII headline: run 18 is the first POSITIVE movement in the
> stall series** (10.2 -> 10.99 -> **30.5 s**) and it arrived exactly
> when the activation chain started flying (`sent:8 failed:0 ready:1`).
> What remains is not a startup gap but a sustained-rate gap: the rig's
> encoder decays while our QoS reports `estimate_kbps:0,
> utilization:0` forever — the reference's server PID reads those fields
> (nvst.rs:7924-7927 -> nvst_control.rs:133-156). D-t10 was the last
> open wire-fidelity item on the feedback plane; this round closes it
> with a two-implementation-pinned port.

## New files

| file | what | provenance |
|---|---|---|
| `streamer/include/kn/kn_bw.h` | the estimator + server-control API: `kn_bw_*` (init/observe/observe_parity/report/reset/frames_observed), `kn_bw_feedback` (the seven QoS fields), `kn_sctl_walk` + `kn_sctl_diag`, `KN_SCTL_PACING_DEFAULT_ENABLED=1` | nvst_bandwidth.rs + nvst_server_control.rs @ a49b124 |
| `streamer/src/kn_bw.c` | the implementation — pure computation, NO sockets/threads/alloc; the clock arrives as int64 ns parameters; every Rust-`as`-cast site uses the saturating helpers (`bw_sat_i64/u64` + masks) because Rust saturates and C is UB | line-for-line port; parity truths in test_knbw |
| `streamer/src/kn_bw_kat_vectors.h` | **GENERATED** — do not edit; regeneration is byte-identical (`tools/bw_reference.py`) | |
| `streamer/tests/test_knbw.c` | **387 checks** — the 18th suite: the Rust goldens (minimum_server_time=270090, estimate_kbps=4800, utilization=5, queue_delay_us=1000, reorder lossy=2, parity receive_kbps=6400.0), backward_clock (68 frames/50 lossy), stale-frame parity (1 loss 0 frames, jitter 121 us), the batched-server-control walk ([0x104][0x10A][0x112] -> 3 records, pacing on LAST), every truncation, the sctl iterator semantics | replays the vectors AND the hand-checked truths |
| `tools/bw_reference.py` | the independent Python model (the two-implementations rule). It self-proves against the reference's OWN Rust goldens BEFORE emitting the header. **Its scenario scripts (:534-620) are the ONLY authoritative source — never hand-write C scenarios from memory** | |

## Changed files

| file | change |
|---|---|
| `streamer/include/kn/kn_rx.h` | `kn_rx_config` gains `now_ns` (+ud) — the estimator's monotonic ns clock (us socket clock x1000 in production; NULL -> now_ms x 1e6 deterministic fallback for replay/tests); `kn_rx_qos` gains the seven BWE fields; 5 new prototypes (`kn_rx_video_bw_report/bw_frames_observed/bw_counters/bw_reset`, the qos snapshot) |
| `streamer/src/kn_rx.c` | the feed: `process_authenticated` takes `feed_bw` (datagram path=1, plaintext process_packet path=0 — **FINAL, do not relitigate**), at the reference site (post-resolve_nack, pre-FEC/reorder; tuple only when `!retransmitted && !reordered` AND RTP+GS parse; bytes = RTP payload len; ssrc change -> estimator reset; parity -> `observe_parity` only). PITFALL pinned by tests: `reordered` must use the PREVIOUS `highest_sequence_received`. `kn_rx_video_rr_snapshot` + the QoS accessors |
| `streamer/src/knvstd_main.c` | (a) the QoS 0x0207 rewrite — all seven BWE fields with the reference semantics (sender_frame = last COMPLETED frame INDEX, bytes_received = CUMULATIVE, loss = guarded DELTA 10000x(expected-received)/expected with the same-ssrc/same-base/monotonic guard, client_time_90khz = (elapsed_ms x 90) wrapping since transport_origin, const-1000 u16 at @30 AND @32, RR cumulative_lost clamped [-0x800000, 0x7fffff]); (b) `sess_cb_*` ControlReportBatch (MAX 1071, queued_at on first record, flush_due = QoS present OR age >= 50 ms, one flush = ONE sid-6 push, retain-on-failure) wired into frame-ack/pacing/QoS; (c) outbox maxlen 704->1071 + `pushed_per_sid[8]`/`dropped_per_sid[8]`/`refused`; (d) `sess_sctl_ingest` (sid 0 AND sid 6) + the pump's volatile latches + the 30 s `sctl_stats` line; (e) the media loop reads the latches each pass — `sctl_termination` -> `stop_reason:"server_termination"` + break, `sctl_pacing_enabled` gates the 0x0203 sender (`fb_pacing_gated/disabled` counters); (f) AU wall-vs-ts telemetry in dec_ctx (`au_drift_ms/min/max`, `au_ts_backwards`); (g) R3-D `sess_ctrl_ping` (2 s u64-BE pings, pong-match RTT sample, 5 s expiry, 1 ms drain on a 50 ms beat, `cping_*` on the 1 s beat, `ctrl_ping_stop` at teardown); (h) the summary grows `batch`/`outbox_by_sid`/`bwe`/`au_clock` blocks APPENDED after `feedback` — `make_rx_capture.py --verify` untouched |
| `streamer/include/kn/kn_ws.h` + `streamer/src/kn_ws.c` | pong payload capture (`last_pong[125]`/`last_pong_len`/`last_pong_seq` + the caller-cursor `pong_seen`) and `kn_ws_take_pong` (consume-once) — the reference matches the pong payload against its ping sequence (nvst_rtsp.rs:895-901) |
| `streamer/tests/test_knws.c` | +5 R3-D checks (payload capture, u64-BE round-trip, consume-once cursor) — suite 83 -> **88** |
| `streamer/include/kn/kn_sctp.h` + `streamer/src/kn_sctp.c` | assoc_state/error tracking + `kn_sctp_assoc_state_name` — `sctp_down` now prints SHUTDOWN_COMPLETE vs COMM_LOST vs the rest (run 18's graceful death was an inference; run 19's is a fact) |
| `CMakeLists.txt` | `kn_bw.c` into kncore; `knbw` into the test foreach |
| `tools/build_native_tests.sh` | `kn_bw.c` into `$KNCORE` AND the direct-link `test_knrx` leg; the new `test_knbw` compile+run leg |
| `tools/cross_check.sh` | the `kn_bw.c` `-c` leg per arch; `test_knbw_$arch.exe` link leg; `kn_bw.c` into `$KCORE` and the `test_knrx` cross leg |
| `tests/T10_build_and_test.py` | `"test_knbw"` into the suites list -> **the target count goes 78 -> 79** |
| `docs/RTSP.md` | §25 (run 18 — the diagnosis, the shipped set, the run-19 matrix) |
| `PROJECT-STATE.md` | status header, R35 updated + R36 NEW, Next steps, session log |

## Integration notes / traps for the next agent

1. **`test_knbw` is backend-independent.** `kn_bw.c` is pure computation
   (no sockets, no clock of its own, no allocation) — sandbox and target
   run the SAME 387 checks. No CMake define needed.
2. **The CLOCK-EXACTNESS RULE is load-bearing.** The reference does all
   clock math on integer-ns Durations and only then `as_secs_f64`; a
   float clock broke utilization in early parity runs. `kn_bw.c` AND
   `bw_reference.py` are both int64-ns end to end. Production feeds the
   us socket clock x1000 via `cfg.now_ns`; do not "simplify" to ms —
   sub-ms arrival granularity is what keeps `received_ms` off the 0.5 ms
   clamp floor (ms-granular arrivals read 0 ms -> clamp -> 2x
   receive-rate error).
3. **Do NOT send real BWE before 100 observed frames.** The reference
   zero-gates the report until then (nvst.rs:1068-1069); the port
   replicates it. A run-19 summary showing `frames_observed < 100` with
   zero estimates is CORRECT behavior, not a broken port.
4. **The feed decision is FINAL** (it went through two reversals during
   the port): `process_authenticated(feed_bw)` — datagram path 1,
   plaintext `process_packet` path 0. Do not relitigate without new
   reference evidence.
5. **Rust `as` casts saturate/reinterpret; C is UB.** `kn_bw.c` uses
   `bw_sat_i64`/`bw_sat_u64` + explicit masks everywhere. Do not
   simplify them away.
6. **`completed_frame_bytes` is CUMULATIVE and `sender_frame_number` is
   the last COMPLETED frame index** (store-on-complete), not per-report
   deltas. The loss field IS a delta snapshot, guarded by
   same-ssrc + same-base + monotonic.
7. **Server-control iterator semantics are FINAL**
   (nvst_server_control.rs:107-146 + its test): Oversized yields once at
   construction (bytes emptied); BOUNDS errors empty bytes WITHOUT an
   error and yield once; DECODE errors consume one record and the walk
   continues. `0x0100` is NOT in the Unhandled set — it decodes as
   Unknown; bulk tests use the `0x0101` family.
8. **The batch changes the outbox's message shape.** One sid-6 push now
   carries up to 1071 bytes (multiple concatenated records) — exactly
   the reference's `send(&self.bytes)`. Anything that assumed
   one-record-per-message (nothing in-tree does; `--verify` reads the
   summary only) must be updated.
9. **The R3-D service runs on the MAIN thread** (the media loop), not
   the pump: the pump owns the bundle socket, main owns the WSS from
   handshake to TEARDOWN (single-owner discipline, D21 untouched). The
   50 ms service beat bounds the 1 ms drain; the reference's dedicated
   worker reads every 100 ms, so ping/pong timing is unaffected.
10. **Build-leg completeness trap:** `kn_rx.c` now references `kn_bw`,
    and FOUR places link `kn_rx.o` directly (native test_knrx, the cross
    test_knrx leg, `$KCORE`, CMake kncore). When adding a new core
    module, grep every script for direct `kn_rx.c` links — this round
    the native gate caught it, the cross gate caught the rest.
11. **Summary-printf patching traps (hit and fixed this round):** two
    identical `char drops[512]` anchors exist (replay + session) —
    search from the END or pin the function; a format-line edit that
    drops the closing quote silently turns the printf into a comma
    expression (the compiler only says "too many arguments"); after any
    format edit, re-validate the JSON by literal extraction (specs vs
    args count + `json.loads` with `%dx%d` pre-replaced).

## Gate results (sandbox, this round)

| gate | result |
|---|---|
| `tools/build_native_tests.sh` | **NATIVE TESTS PASS** — 17 suites incl. **test_knbw 387**, test_knws **88** (+5 R3-D), test_kninput 446, test_knselftest 117; jsonl + zip validation; the paranoid knvstd leg |
| `tools/cross_check.sh` | **CROSS-CHECK PASS** ×2 (x86_64 + i686), zero warnings under `-Wall -Wextra -Werror`, incl. the NEW kn_bw `-c` legs, the test_knbw link legs, kn_bw in $KCORE + the test_knrx leg |
| ctest, mbedTLS backend | **20/20 PASS** (+test_knbw), zero build warnings |
| ctest, schannel backend | **20/20 PASS**, zero build warnings |
| ASan+UBSan on `test_knbw` | **387/387, zero diagnostics** |
| ASan+UBSan on `test_knrx` (the patched feed) | **194/194, zero diagnostics** |
| `tools/sandbox_session_e2e` | **43/43 PASS** (kn_bw added to its link line) |
| `tests/T10_build_and_test.py` (POSIX dry) | **ALL PASS (69 checks)** — was 68: +1 for the new suite |
| `tests/T13_udp_e2e.py` | **ALL PASS (63 checks)** — unchanged |
| paranoid matrix | knvstd ×9 (−O0/−O2/−O3 × bare/mbedTLS/usrsctp) + kn_bw/kn_rx ×3, all clean with `-Wunused-but-set-variable -Wformat-truncation=2` |

> **Pre-existing, NOT introduced this round:** `kn_ws.c` has two
> `-Wformat-truncation=2` diagnostics at HEAD (verified via stash) —
> outside the declared paranoid gate (knvstd + kn_input legs), same
> class as the recorded `kn_selftest.c` pair. Fixing them needs the R28
> value-range bounding treatment and belongs in its own round.

**Next (user side):** sync the changed files, run
`python tests\T10_build_and_test.py` -> expect **ALL PASS 79**, zero
warnings; `test_knbw` must report **387 checks** on the target (same
pure-computation code as the sandbox — that is the Win7-side proof the
estimator matches the reference's goldens). Then **R7 run 19**
(`--session-seconds=300`, untruncated log if possible): paste `handoff`,
one mid-session `session_stats`, `sctl_stats`, `ctrl_ping_stop`,
`ctrl_summary`, the FULL summary (all four new blocks),
`sctp_down`/`ice_summary` — see docs/RTSP.md §25.4 for the decision
matrix. Sustained video + nonzero `bwe.estimate_kbps` past frame 100 =
**STAGE 2 DONE** and the presentation wiring becomes the main track.

---

---

# Round XXXVI delta, rev 26 (R7 run 16 = PLAY 200 — THE FIRST REAL GFN VIDEO THROUGH THE FULL STACK: 4900/4900 authenticated, 307 frames decoded, R32's final stage-2 gate PASSED; the ~10 s feedback stall → the reference's FULL sender set ships: RR/frame-ack/pacing/QoS/keepalive/IDR+PLI/NACK/RFI over the live channels)

> **Rev 26 supersedes rev 1-25.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §23** (run 16 — the breakthrough —
> + the feedback round + the run-17 matrix), `docs/SCTP.md` §8 (all
> three questions answered), `docs/RX.md`/`docs/SRTP.md` (R32 CLOSED
> in the risk register).
>
> **Round XXXVI headline: run 16 broke the wall — `srtp_prof:1` (the
> rig negotiated use_srtp) + `nom_sent:18` → `sctp_up ms:52` (the rig
> answered the INIT run 15 ignored) → `sctp_channels 8/8 acked` →
> **PLAY 200, the first in 16 runs** → handoff (AEAD_AES_256_GCM_8,
> local key) → **4900/4900 datagrams authenticated, 307 AUs, 307
> frames decoded 1280×720, ffprobe-verified h264 High bt709 — R32's
> FINAL STAGE-2 GATE PASSED.** The stream stalled at ~10.2 s exactly
> as the reference documents ("without any receiver feedback the
> server stops video", nvst.rs:4320-4323) with our kf_request still
> unsent — so this round ships the COMPLETE feedback sender set. New
> facts: the mystery post-ready record = app data epoch 1 (benign
> SCTP); run 15's invalid_in:175 = the rig's AUDIO RTP on the bundle
> (now classified); the rig sent COMMAND_INPUT_VERSION 0x020e + 21
> more control messages on sid 0 (the stage-3 input layer awaits).**
>
> ## Changed files (round XXXVI)
> 1. `streamer/{include/kn,src}/kn_rx.*` — NEW `kn_rx_rtcp_rr()` (the
>    reference's build_rtcp_receiver_report :2983-2997 byte-for-byte:
>    0x81/201/len-7, sender "ONOW" 0x4F4E4F57, 24-bit clamped
>    cumulative, LSR=DLSR=0) + NEW `kn_rx_video_rr_snapshot()` and the
>    `base_sequence`/`have_base_sequence` anchor in kn_rx_video (the
>    reference's :1225 expected-count base; set at the first
>    authenticated packet beside the existing highest-seq bookkeeping).
> 2. `streamer/{include/kn,src}/kn_sctp.{h,c}` — NEW
>    `kn_sctp_send_pr()` (SCTP_SENDV_SPA: sndinfo+prinfo in one
>    sendv) for the sender-side PR-SCTP policies; `kn_sctp_send` stays
>    the reliable wrapper (unchanged semantics).
> 3. `streamer/src/knvstd_main.c` —
>    * **the feedback outbox** (`sess_fb_outbox`: 64×704 bounded mutex
>      ring, drop-new counted; push = the media loop/main, pop = the
>      pump thread's `sess_sctp_service`) + `sess_fb_sid_params` (the
>      profile's ordered/PR mapping: sid 4 RTX 2, sid 6/10 TTL 300,
>      the rest ordered-reliable). The media loop ENCODES (kn_rx's
>      single owner — D21 untouched), the pump SHIPS (SCTP's single
>      owner).
>    * **cadences** (all the reference's): RR 1 s → sid 14 (fraction =
>      the :1234-1247 interval-delta arithmetic over the snapshot);
>      frame ACK 0x0204 per assembled AU + pacing 0x0203 @55 556 µs
>      neutral-error (:7928-7933) → sid 6, hooked in dec_frame_in_cb;
>      QoS 0x0207 @50 ms → sid 6 (D-t10: estimator zeros +
>      measured-downlink estimate); keepalive 0x0200 @3 s → sid 0 (the
>      byte-exact golden of nvst_input.rs:1769).
>    * **the recovery senders fly**: dec_take_kf → IDR 0x0302 (sid 6)
>      + PLI (sid 14); the NACK drain (kn_rx_nack_take's own policy) →
>      NACK-v2 (sid 6) + RTCP NACK (sid 14) per :3075-3078; RFI 0x0301
>      (sid 6) with restore-on-drop. The "unsent (stage-3)" notes are
>      GONE; kf_request lines now report sent:true/false honestly.
>    * **bundle demux**: RTP/RTCP-looking (v==2, pt ranges) →
>      `bundle_rtp_in`/`bundle_rtcp_in` instead of `invalid_in` (the
>      rig's audio on 49006 — the audio stage's input).
>    * Evidence: session_stats += fb_rr/fb_ack/fb_qos; the summary +=
>      the `feedback` block (rr/ack/pacing/qos/ka/nack/idr/pli/rfi/
>      pushed/dropped); ice_preplay += bundle_rtp/bundle_rtcp/fb_sent;
>      sess_ice_stats carries the snapshot-safe copies. Teardown
>      destroys the outbox after the join (both paths).
> 4. `streamer/tests/test_knrx.c` — 215→**219**: the RR goldens
>    (independent Python transcription of the reference builder:
>    negative-loss two's-complement, BOTH clamps, LSR/DLSR zeros) +
>    the NULL-safe snapshot guard.
> 5. `streamer/tests/test_knsctp.c` — 33→**35**: the send_pr legs
>    (SPA path: unordered TTL-300 on sid 6 arrives intact); the loss
>    leg hardened (polls until the one-shot drop actually fires —
>    usrsctp may delay/bundle output under traffic; timing-not-
>    semantics fix caught by the first ctest run).
> 6. Docs: `docs/RTSP.md` §23, `docs/SCTP.md` §8 (answered),
>    `PROJECT-STATE.md` (round XXXVI + **R32 CLOSED** in the risk
>    register), this file.
>
> ## Verification status (what ran WHERE, round XXXVI)
> * Sandbox: native 16 suites PASS (test_knrx 219); cross x64+i686
>   -Werror PASS; paranoid knvstd legs clean ×3 with both defines;
>   ctest **18/18 ×2** (fresh); ASan+UBSan+LSan test_knsctp **35/35**;
>   test_kndtls 39/39; the OpenSSL interop ×2 PASS; E2E **43/43**; T10
>   POSIX dry **66**; T13 **63**.
> * Target (round XXXVI's evidence, user-reported): T10 ALL PASS
>   **76** zero warnings; **R7 run 16 — PLAY 200 + real video** (docs/
>   RTSP.md §23) + the ffprobe verification of the dumped keyframe.
> * Target (PENDING — round XXXVII asks for): T10 (expect **76**) +
>   **R7 run 17** — the SUSTAINED-stream run (matrix in PROJECT-STATE
>   Next steps 2; paste the session_stats progression + the FULL
>   summary with the feedback block).
>
> ## Notes for the target build
> * Sync list (round XXXVI): kn_rx.{h,c}, kn_sctp.{h,c},
>   knvstd_main.c, test_knrx.c, test_knsctp.c + docs. The T10 harness
>   is UNCHANGED (expectation **76**).
> * No new libraries/imports: the outbox uses the same kernel32/
>   pthread primitives as the pump thread; send_pr is one more usrsctp
>   entry point (statically linked already).

# Round XXXV delta, rev 25 (sandbox-only round between runs: the reference re-read found fidelity gap #2 — str0m negotiates DTLS-SRTP (use_srtp) and the bundle audio's keys come from it → MBEDTLS_SSL_DTLS_SRTP enabled via user-config, kn_dtls offers 0x0001 (D-t9), wire-pinned by an independent parser + negotiated through OpenSSL)

> **Rev 25 supersedes rev 1-24.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §22** (the finding + the shipped
> delta + the run-16 two-hypothesis matrix), §21 (run 15 + the
> nomination), `docs/SCTP.md`, `docs/DTLS.md` (Q3 updated).
>
> **Round XXXV headline: str0m's DTLS emits `KeyingMaterial(km,
> profile)` right after Connected (vendor lib.rs:1648-1658 →
> session.rs:259-272) — the reference's ClientHello carries the RFC
> 5764 `use_srtp` extension, offering [AEAD_AES_256_GCM 0x0008,
> AEAD_AES_128_GCM 0x0007, AES128_CM_SHA1_80 0x0001]
> (crypto/srtp.rs ALL), and the bundle audio (rtcAudioOnNativeBundle:1
> — which OUR announce mirrors) takes its SRTP keys from that
> negotiation. Runs 13-15 sent NO use_srtp (D-t3). Shipped: mbedTLS's
> MBEDTLS_SSL_DTLS_SRTP enabled through the sanctioned
> MBEDTLS_USER_CONFIG_FILE (vendor tree untouched), kn_dtls offers
> [0x0001] no-MKI (D-t9 = the intersection with mbedTLS 3.6's
> ceiling; RFC 5764 omission semantics make it regression-proof,
> test-pinned), `kn_dtls_srtp_profile()` + `srtp_prof` in the
> dtls_ready evidence. Run 16 now tests nomination (D-t8) + use_srtp
> (D-t9) TOGETHER — deconfounded by the evidence lines.**
>
> ## Changed files (round XXXV)
> 1. `streamer/src/kn_mbedtls_user_config.h` (NEW) — the additive user
>    config: `#define MBEDTLS_SSL_DTLS_SRTP` (requires only
>    MBEDTLS_SSL_PROTO_DTLS, on by default). **ABI WARNING documented
>    in the header: the define changes the layout of
>    mbedtls_ssl_context/config — EVERY TU including mbedtls/ssl.h
>    must see it** (CMake: PUBLIC on the mbedtls target → all
>    consumers inherit; cross_check: the flag on all four mbedtls
>    legs; ad-hoc sanitizer/manual builds linking the CMake libraries:
>    add `-DMBEDTLS_USER_CONFIG_FILE='"kn_mbedtls_user_config.h"'`
>    with -Istreamer/src — the round-XXXV ASan legs did).
> 2. `CMakeLists.txt` — `target_compile_definitions(mbedtls PUBLIC
>    "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/streamer/
>    src/kn_mbedtls_user_config.h\"")` in the mbedtls block (absolute
>    path — streamer/src is not on the mbedtls target's include path).
> 3. `streamer/{include/kn,src}/kn_dtls.{h,c}` — the use_srtp offer
>    (`#ifdef MBEDTLS_SSL_DTLS_SRTP`, graceful no-op without: a static
>    [0x0001, UNSET] list → conf_dtls_srtp_protection_profiles) +
>    `kn_dtls_srtp_profile()` (the negotiated id via
>    `MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)` — the 3.x field
>    convention, kn_net_mbedtls precedent) + the stub twin. The
>    keying material is NOT extracted (bundle audio = stage 3; §7
>    shapes-only — the id rides the evidence line).
> 4. `streamer/src/knvstd_main.c` — `dtls_ready` grew `"srtp_prof"`
>    (1 = the rig negotiated 0x0001; 0 = RFC omission, the runs-13-15
>    wire).
> 5. `streamer/tests/test_kndtls.c` — 32→**39 checks**: (a) the
>    ClientHello capture leg — a SECOND loopback socket receives the
>    first flight (kn_dtls's BIO owns its send path — no write_cb
>    exists; the first draft wrongly borrowed the kn_sctp capture
>    pattern and saw zero packets) and an INDEPENDENT hand parser
>    (record 13 B + hs header 12 B + CH body walk + extension scan)
>    asserts use_srtp 0x000e with ONE profile 0x0001 and an empty MKI
>    — the parser caught its own bug against the live wire: RFC 5764's
>    SrtpProfiles is a BYTE-LENGTH vector (`<2..2^16-1>`), not a
>    count; (b) negotiation asserted on BOTH ends of the loopback pair
>    (0x0001 chosen); (c) the NEGATIVE leg: a profile-less server
>    completes the handshake with profile 0 — pinning that the offer
>    can never regress the runs-13-15 wire. tsrv_init grew with_srtp.
> 6. `tools/dtls_openssl_check.py` — second leg: `s_server -dtls1_2
>    -use_srtp SRTP_AES128_CM_SHA1_80` (OpenSSL's flag name measured:
>    `-use_srtp`, NOT `-srtp`) → the client asserts
>    `srtp_profile=0x0001`; **an INDEPENDENT stack negotiates our
>    offer**. The plain leg now also asserts 0x0000 (omission
>    semantics cross-verified). `test_kndtls --openssl PORT FP [srtp]`.
> 7. `tools/cross_check.sh` — the user-config define on all FOUR
>    mbedtls legs (kn_net_mbedtls, kn_cert real, kn_dtls real,
>    test_kndtls real section; bare-name form — CFLAGS carries
>    -Istreamer/src). One -Werror=comment catch fixed pre-ship (a `/*`
>    inside the new parser's doc comment).
> 8. Docs: `docs/RTSP.md` §22, `docs/DTLS.md` (Q3 updated: the live
>    hypotheses are nomination + use_srtp), `PROJECT-STATE.md` (round
>    XXXV), this file.
>
> ## Verification status (what ran WHERE, round XXXV)
> * Sandbox: native 16 suites PASS; cross x64+i686 -Werror PASS incl.
>   the four re-defined mbedtls legs + the usrsctp link leg; paranoid
>   knvstd legs clean ×3; ctest **18/18 ×2 with FRESH rebuilds** (the
>   layout change demands it); ASan+UBSan+LSan test_kndtls **39/39**;
>   the OpenSSL interop BOTH legs PASS (0x0000 plain / 0x0001
>   -use_srtp); sandbox_session_e2e **43/43**; T10 POSIX dry ALL PASS
>   **66**; T13 ALL PASS **63**.
> * Target (PENDING — round XXXVI asks for): T10 (expect ALL PASS
>   **76**, zero warnings; the first build recompiles mbedTLS with the
>   new layout, ~1 min extra) + **R7 run 16** — the two-hypothesis run
>   (matrix in PROJECT-STATE Next steps 2 + RTSP.md §22). Paste
>   dtls_ready (srtp_prof!), sctp_arm/sctp_up, PLAY, rtsp_response,
>   and the FULL ice_preplay (nom_sent/post_first/sctp_out).
>
> ## Notes for the target build
> * Sync list (round XXXIV+XXXV together): kn_sock.{h,c}, kn_dtls.{h,c},
>   knvstd_main.c, test_knsock.c, test_kndtls.c, test_knsctp.c,
>   kn_mbedtls_user_config.h (NEW), CMakeLists.txt,
>   tools/{cross_check.sh,dtls_openssl_check.py}, docs. T10 harness
>   UNCHANGED since rev 23b (expectation **76**).
> * The mbedTLS layout change is ABI-internal to the build (all TUs
>   recompile together via the PUBLIC define; no shipped interface
>   changes). If a stale build dir lingers on the target, T10's
>   `cmake --fresh` already handles it.

# Round XXXIV delta, rev 24 (R7 run 15 = usrsctp LIVE on Win7: the SCTP INIT flew inside DTLS and the rig stayed silent; PLAY 400 #4 → the ICE nomination hypothesis: USE-CANDIDATE ships with independent goldens; the mystery post-ready record finally prints; the seventh target catch fixed)

> **Rev 24 supersedes rev 1-23.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §21** (run 15 + the nomination
> analysis + the run-16 matrix), `docs/SCTP.md` §8 (the INIT-silence
> measurement), `docs/DTLS.md` (D-t8 retires D-t1).
>
> **Round XXXIV headline: run 15 proved the R3-C stack live (sctp_arm
> armed:1 — usrsctp up on Win7, the INIT left inside a DTLS app
> record, dtls_out 3→4) and the rig IGNORED it (sctp_up:0 after the
> full bounded wait; app_records:0; no COMM_LOST — silence) with PLAY
> 400 #4 across two rigs. The remaining wire difference vs the
> reference: its str0m CONTROLLING agent nominates the valid pair
> (USE-CANDIDATE) while ours never did — a Bifrost-class rig plausibly
> gates the data plane on the SELECTED pair (DTLS gates only on
> connectivity — four successful handshakes fit). Shipped:
> kn_sock_stun_build2 + the nomination (same real-STUN gate as the
> DTLS arm; post-ready one nominated request per 100 ms tick; D-t8),
> byte-exact vs an independently generated golden whose Python builder
> self-proved against the round-XXV goldens first (incl. GFN's
> non-RFC 0x5354554E fingerprint XOR); the run-15 evidence gaps
> closed (post_first/peer_closed/nom_sent/sctp_out/sctp_in in
> ice_preplay — the thrice-seen mystery post-ready record finally
> gets its ContentType printed); the target's 3 -Wmaybe-uninitialized
> warnings fixed (the SEVENTH target catch). Gates: test_knsock
> 98→102 (ASan clean), ctest 18/18 ×2, cross ×2 + the usrsctp link
> leg, native 16, E2E 43/43, T10 dry 66, T13 63.**
>
> ## Changed files (round XXXIV)
> 1. `streamer/{include/kn,src}/kn_sock.*` — `KN_SOCK_STUN_ATTR_USE_
>    CANDIDATE 0x0025` + `kn_sock_stun_build2` (the two-attribute
>    form; attr2_type 0 omits it; `kn_sock_stun_build` is now the
>    attr2-less wrapper — wire-identical, the existing goldens
>    re-prove it). Attribute order attr1→attr2 is wire-pinned by the
>    new golden.
> 2. `streamer/tests/test_knsock.c` — 98→**102 checks**: the
>    nominated-request golden (USERNAME+USE-CANDIDATE, byte-exact,
>    generated by the independent Python builder AFTER it reproduced
>    k_req/k_succ byte-for-byte — the vectors-self-prove rule),
>    integrity+inspect round-trips past the extra attribute, the
>    undersized-buffer refusal.
> 3. `streamer/src/knvstd_main.c` — the pump nominates: the gate is
>    `(req_in || ok_in)` — the SAME first-real-inbound-STUN rule as
>    the DTLS arm (:8196, never synthesized); pre-ready the 3× bursts
>    carry the flag (`build2`), post-ready ONE nominated request per
>    100 ms tick beside the NATT ping (the str0m selected-pair
>    traffic mirror — deviation **D-t8 retires D-t1**). ice_preplay
>    grew `nom_sent`, `post_first`/`post_first_len` (the kn_dtls
>    inject-level RAW capture of the first post-ready record — the
>    mystery record's ContentType: 0x14 CCS / 0x15 alert / 0x16
>    handshake / 0x17 app), `peer_closed`, `sctp_out`/`sctp_in` (the
>    kn_sctp packet counters — the INIT-left proof at the SCTP layer);
>    `sctp_fail` class 3 = send errors. sess_ice_stats carries the
>    snapshot-safe copies.
> 4. `streamer/tests/test_knsctp.c` — the seventh-target-catch fix:
>    both `t_sctp_pkt p` declarations zero-init (GCC 16.2's
>    -Wmaybe-uninitialized cannot prove the conditional-fill invariant
>    through the `prc == 0 &&` short-circuit; sandbox GCC 12 silent —
>    the two-tier gate working as designed).
> 5. Docs: `docs/RTSP.md` §21, `docs/SCTP.md` §8 (the answer),
>    `docs/DTLS.md` (D-t8), `PROJECT-STATE.md` (round XXXIV).
>
> ## Verification status (what ran WHERE, round XXXIV)
> * Sandbox: native 16 suites PASS (test_knsock 102); cross x64+i686
>   -Werror PASS incl. the usrsctp cross-build + F1 link leg; paranoid
>   knvstd legs clean ×3 with BOTH backend defines; ctest **18/18 ×2**;
>   ASan+UBSan+LSan: test_knsctp 33/33 + test_knsock 102/102;
>   sandbox_session_e2e **43/43**; T10 POSIX dry ALL PASS **66**; T13
>   ALL PASS **63**. Toolchain note: the sandbox reset between messages
>   (R26) and the apt mirror hung — recovered via round XXVI's extended
>   fallback (aliyun → deb.debian.org rewrite).
> * Target (round XXXIV's evidence, user-reported): T10 **FAIL 3/76 =
>   ONLY the 3 warnings** (everything else passed — incl. the usrsctp
>   Win32 download/build/link, proving the rev-23b hotfix); R7 run 15
>   (docs/RTSP.md §21).
> * Target (PENDING — round XXXV asks for): T10 (expect ALL PASS
>   **76**, zero warnings) + **R7 run 16** — the nomination run;
>   matrix in PROJECT-STATE Next steps 2. Paste the FULL ice_preplay
>   (nom_sent/post_first/sctp_out are the new deciders).
>
> ## Notes for the target build
> * No new files to wire: kn_sock.{h,c}, test_knsock.c, test_knsctp.c,
>   knvstd_main.c changed; the gates/T10 harness are unchanged since
>   rev 23b (expectation stays **76**).
> * The nomination changes ONLY the STUN attribute list on our own
>   requests — a rig that ignores USE-CANDIDATE behaves exactly as in
>   runs 9-15 (the attribute is integrity-covered like everything
>   else; four rigs have been validating our MI all along).

# Round XXXIII delta, rev 23 (R7 run 14 = the 400 captured: empty body, echo headers, app_records:0 → the rig waits for OUR SCTP INIT → R3-C SHIPPED: kn_sctp on vendored usrsctp + the 8-channel DCEP profile + bounded PLAY gating)

> **Rev 23 supersedes rev 1-22.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/SCTP.md`** (NEW — kn_sctp's full record:
> wire parameters, thread map, the SEVEN usrsctp landmines, deviations
> D-t4..D-t7), `docs/RTSP.md` §20 (run 14 + the delta + the run-15
> matrix), §19 (run 13), `docs/DTLS.md` (R3-C marked SHIPPED).
>
> **Round XXXIII headline: run 14's captures forced the verdict —
> PLAY's 400 says nothing (`reason:"OK"`, empty body, echo headers) and
> `app_records:0` proves the rig initiates no SCTP; the reference
> client does (`start_sctp(true)` at dtls_ready, then the 8-channel
> DCEP profile, all before finish()/PLAY). R3-C shipped: `kn_sctp`
> (vendored usrsctp c625e219, AF_CONN over the new kn_dtls app bridge,
> sctp-proto-exact wire parameters, the full NVST channel profile) +
> the independent Python reference + the 33-check suite + PLAY gated on
> sctp_up (D-t5). Target expectation: T10 ALL PASS **76** (the harness
> now fetches the pinned usrsctp tarball), then R7 run 15.**
>
> ## Changed files (round XXXIII)
> 1. `streamer/{include/kn,src}/kn_sctp.{h,c}` (NEW) — the R3-C module:
>    real body under KN_SCTP_BACKEND_USRSCTP (CMake auto-detects
>    third_party/usrsctp, BOTH platforms), honest stub elsewhere.
>    AF_CONN routing (one registered key per process — the
>    libdatachannel pattern; usrsctp opens no real sockets, DTLS is
>    the wire); the bounded mutex tx queue is the thread boundary
>    (usrsctp's timer thread emits into it; only kn_sctp_poll — the
>    pump thread — touches the DTLS context); wire parameters mirror
>    sctp-proto exactly (5000↔5000, OS/MIS 65535, a_rwnd 1 MiB, RTO
>    3000/1000/60000 via sysctls+RTOINFO, unlimited INIT attempts,
>    MAXSEG 1200); PURE DCEP marshal/parse (RFC 8832, strict type set)
>    identical in every build; `kn_sctp_nvst_channels[8]` = the
>    reference profile verbatim (sids 0-14, labels, 0x00/0x01/0x82/0x81,
>    params 0/2/300, priority 0, protocol ""); `kn_sctp_open_channels`
>    fires the burst ordered+reliable PPI 50 per stream; test-only
>    server/peer opens (shared-key + port-routed loopback — landmine
>    #4 in SCTP.md §5).
> 2. `streamer/{include/kn,src}/kn_dtls.{h,c}` — the app layer became a
>    BRIDGE: `kn_dtls_write_app` (mbedtls_ssl_write when ready; the
>    SCTP tx path) + `kn_dtls_read_app` (a bounded 8×2048 decrypted-app
>    ring; the SCTP rx path) + `first_post_ready` (the inject-level RAW
>    capture of the first post-ready record — run 15 names the mystery
>    record run 13/14 saw: ContentType 20/21/22/23) + `app_q_dropped`;
>    the D-t4 counted discard stays as the no-consumer fallback.
> 3. `streamer/src/knvstd_main.c` — the pump thread arms kn_sctp the
>    moment dtls_ready fires (write_cb = kn_dtls_write_app; the DTLS
>    app ring feeds kn_sctp_inject each pass); on COMM_UP it opens all
>    8 channels; rig-side OPENs get ACKs (str0m :1046-1060); the first
>    rig message head is captured. PLAY gating (D-t5): after the 100 ms
>    burst head start, main waits (snapshot-polled, bounded by
>    `--session-play-wait`, default 2500 ms) for sctp_up — or for
>    dtls/sctp failure or Ctrl-C — then settles `--session-play-settle`
>    (default 250 ms); PLAY ALWAYS flies afterwards (a dead data plane
>    never eats the measurement). New evidence: `sctp_arm` (with the
>    refusal detail), `sctp_up {"ms"}`, `sctp_channels {"opened",
>    "acked","rig_opened"}`, `sctp_down`; ice_preplay grew sctp_armed/
>    up/ms/opened/acked/rig_opened/msgs_in/fail/first_sid/first_ppi/
>    first_len/first(hex). Teardown order: join → kn_sctp_close →
>    kn_dtls_free → sockets → cert. run_session grew the two play
>    parameters; usage + argv parsing updated.
> 4. `streamer/tests/test_knsctp.c` (NEW, 16th suite) — self-adaptive:
>    REAL battery 33 checks / stub contract 13. The real legs: the
>    Python goldens ↔ builder byte-equality (all 8 channels) + parse
>    round-trips + the 6-vector malformed battery + CRC-32c goldens +
>    the golden INIT through the independent parser; the LIVE usrsctp
>    INIT captured at the write_cb and verified independently (ports
>    5000↔5000, vtag 0, CRC-32c, OS/MIS 65535/65535, a_rwnd ≥ 64 KiB);
>    the full 4-way association over the port-routed memory relay; the
>    channel burst received with exact sids/labels/types/params + ACKed
>    both ways; user data sid 0 + sid 14; one-shot loss → SACK/fast-
>    retransmit recovery; garbage inject tolerated; stats coherence
>    (honest about the shared-key topology: the server queue carries
>    both directions). ASan+UBSan+LSan clean.
> 5. `tools/sctp_reference.py` (NEW) + generated
>    `streamer/src/kn_sctp_kat_vectors.h` — the independent Python
>    (self-proves CRC-32c against RFC 3720 B.4 = 0xE3069283 and
>    hand-transcribed DCEP goldens BEFORE emitting; byte-stable
>    regeneration). It caught its own bug pre-ship (a valid ACK had
>    been filed under the malformed vectors).
> 6. `third_party/usrsctp` (GITIGNORED, vendored like mbedtls) —
>    pinned master c625e21950600460cb5c6075616eb1622df2b06a
>    (2026-10-07), tarball SHA-256
>    090b9d03bf068ce5989576c8e0ff6979d3031ea85e405acd263206f19166b41e;
>    `tests/T10_build_and_test.py` downloads+verifies+extracts it
>    (ensure_usrsctp_source, the mbedtls pattern) and gates the build
>    on it; suites += test_knsctp → target expectation **76**.
> 7. `CMakeLists.txt` — kn_sctp.c into kncore; knsctp into the foreach
>    (ctest 17→18 ×2); the usrsctp block: add_subdirectory with
>    sctp_debug/werror/invariants/programs/shared/fuzzer OFF + inet6
>    OFF, scoped `-w` on the vendored target ONLY (our files stay
>    strict — the mbedtls-suppression precedent), KN_SCTP_BACKEND_
>    USRSCTP on kncore + test_knsctp, ws2_32 on Win32, an explicit
>    ENABLED/stub status line.
> 8. `tools/build_native_tests.sh` (KNCORE += kn_sctp.c; the 16th suite
>    builds stub + runs as rc20) and `tools/cross_check.sh` (kn_sctp
>    stub -c + the REAL-body leg + the test's REAL-section leg under
>    the exact target flags ×2 arches whenever third_party/usrsctp
>    exists — the round-XXII rule; the stub suite link; KCORE +=).
> 9. Docs: `docs/SCTP.md` (NEW — §2 the wire-parameter provenance
>    table, §3 the channel profile, §4 the thread map, §5 the SEVEN
>    landmines, §6 verification, §7 deviations D-t4..D-t7, §8 run-15
>    questions), `docs/RTSP.md` §20, `docs/DTLS.md` (R3-C SHIPPED +
>    the remaining stage-3 send scope), `PROJECT-STATE.md` (round
>    XXXIII), `tests/README.md` untouched (no new target harness).
>
> ## Verification status (what ran WHERE, round XXXIII)
> * Sandbox: native 16 suites PASS; cross mingw x64+i686 -Werror PASS
>   incl. the kn_sctp real-body + test-real-section legs; paranoid
>   knvstd legs clean with BOTH backend defines ×3 opt levels; ctest
>   **18/18 ×2** (mbedtls: test_kndtls 32 + test_knsctp 33 real;
>   schannel: knsctp still REAL — usrsctp is TLS-independent —
>   kndtls stub); ASan+UBSan+LSan clean on both new suites; the
>   OpenSSL-3 DTLS interop PASS (re-run after the bridge changes);
>   sandbox_session_e2e **43/43** (unchanged by design — the rig
>   speaks no SCTP yet; upgrading it is a stage-3 candidate); T10
>   POSIX dry ALL PASS **66**; T13 ALL PASS **63**.
> * Target (round XXXIII's evidence, user-reported): T10 ALL PASS
>   **74** (the round-XXXII tree); R7 run 14 (docs/RTSP.md §20).
> * Target (PENDING — round XXXIV asks for): T10 (expect **76**, the
>   usrsctp download on first run) + **R7 run 15** (the decision
>   matrix in PROJECT-STATE Next steps 2; paste sock_pair/dtls_ready/
>   sctp_arm/sctp_up/sctp_channels/PLAY/rtsp_response/ice_preplay
>   verbatim, + handoff/summary on PLAY 200).
>
> ## Notes for the target build
> * **rev 23b HOTFIX (the sixth target catch):** the first target T10
>   died at configure — our `target_link_libraries(usrsctp PUBLIC
>   ws2_32)` mixed keyword signature with usrsctp's own PLAIN
>   `target_link_libraries(usrsctp ws2_32 iphlpapi)` (its CMakeLists
>   :195-198; WIN32-only branch — invisible to the POSIX sandbox).
>   Fixed by DELETING our line (usrsctp links its own Win32 deps) and
>   gated permanently: cross_check.sh now cross-builds usrsctp for
>   mingw x64 and LINKS the real suite under the exact F1 flags —
>   imports verified: KERNEL32/msvcrt/WS2_32/IPHLPAPI only, no
>   winpthread DLL. Target expectation corrected 75 → **76** (the
>   failed run's own 3/76 count proved it).
> * First T10 run downloads the ~0.8 MB pinned usrsctp tarball
>   (SHA-256 verified before extraction); the build grows by ~25 s
>   (usrsctp static). The vendored target compiles with scoped `-w`
>   (the mbedtls-suppression precedent); T10's zero-warning gate greps
>   the WHOLE ninja log for "warning:", so if GCC 16.2 ever surfaces a
>   class `-w` does not cover, the gate flags it — the two-tier rule
>   working as designed. The sandbox GCC 12 build is warning-free.
> * No new PE imports: usrsctp is static; AF_CONN opens no sockets
>   (ws2_32 already linked for kn_sock).
> * kn_sctp's real body needs NO code change on Win32 — the sandbox
>   cross legs already compiled it under the exact target flags ×2.


# Round XXXII delta, rev 22 (R7 run 13 = THE DTLS HANDSHAKE COMPLETED ON A LIVE PRODUCTION RIG — R3-B live-closed; PLAY went silence→explicit 400 with the body uncaught → the instrumentation round: the full non-200 response capture, the rig's post-DTLS app-record capture, the pre-PLAY video-socket keepalive)

> **Rev 22 supersedes rev 1-21.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §19** (run 13 + the round-XXXII
> delta + the run-14 decision matrix), §18 (R3-B design + the mbedTLS
> landmines), `docs/DTLS.md` (R3-B LIVE-PROVED; open questions 3-6
> updated).
>
> **Round XXXII headline: run 13 landed the project's deepest live
> datum yet — `dtls_ready {"ms":388,"version":"DTLSv1.2","cipher":
> "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256","verify":0}` on a
> production rig (and again in 293 ms on a RESUME-at-create re-attach
> with a fresh cert): the rig answered our client-active ClientHello,
> requested+verified our certificate against the ANNOUNCE'd
> fingerprint, our pin verified its cert. `sock_pair` bound the
> preferred 49005/49006 and the seat described `unified:0` — the
> two-socket layout IS the production path. PLAY moved from silence to
> an explicit **400** (the rig now evaluates the request) but the body
> was NOT captured, and the rig sent exactly ONE post-handshake app
> record that D-t4 discarded. At PLAY time the reference additionally
> has: the SCTP INIT (start_sctp(true) the instant DTLS connects,
> :8214-8223 → R3-C) and the PRE-PLAY Mjolnir NATT keepalives from the
> video socket (lib.rs:990 arms that worker before finish();
> :8849-8905) — run 13 left 49005 silent. This round ships the
> instrumentation that ends the guessing and that last fidelity piece;
> NO hand-rolled SCTP (poison precedent — run 14's captures decide
> R3-C).**
>
> ## Changed files (round XXXII)
> 1. `streamer/src/knvstd_main.c` —
>    * `sess_rtsp_detail` grew a nullable `resp` parameter and emits
>      the NEW **`rtsp_response`** line on any non-200: reason phrase,
>      EVERY header (`{"n":..,"v":..}` pairs, each through the
>      lb_json_path sanitizer) and a 240-byte body excerpt — the rig's
>      own words about run 13's 400 land in run 14's log.
>    * NEW **`rig_info`** line: the DESCRIBE response's `Server`
>      header (RTSP.md §16 open question 5 — refusal-mode/version
>      correlation accumulates per run now).
>    * The pump thread grew the **pre-PLAY video-socket keepalive**:
>      `vid_sock/vid_ip/vid_port/vid_payload` + the 20 ms NATT ping
>      (the media loop's exact shape, :8849-8905) — SEND-ONLY (it
>      never recvs the video socket; inbound media queues in the 8 MiB
>      buffer for the media loop), stopped by main's `vid_handoff`
>      flag at media-loop entry (duplicate-ping overlap harmless);
>      wired in non-unified mode only; `vid_ka` counter.
>    * `pump_ms` accumulates LIVE in the loop (the persist-mode
>      snapshot used to read 0 — run 13's artifact).
>    * `ice_preplay` grew `vid_ka`, `app_records`, `app_bytes`,
>      `app_first_len`, `app_first` (hex); `sess_ice_stats` carries
>      the snapshot-safe copies (main never touches kn_dtls while the
>      thread runs).
>    * One paranoid-gate fix pre-ship: sess_reserve_pair's fallback
>      message uses a bounded memcpy, not snprintf (the errsz==1
>      -Wformat-truncation=2 over-approximation class; the file's
>      permanent =2 gate stays clean ×4 optimization levels).
> 2. `streamer/{include/kn,src}/kn_dtls.{h,c}` — the FIRST app-data
>    record is captured verbatim (≤ KN_DTLS_FIRST_APP_CAP=64 bytes +
>    the true length) inside the drain path; stats grew
>    `first_app`/`first_app_len`. This is R3-C's design input: run 13
>    proved the rig sends exactly one app record after the handshake —
>    run 14 names its protocol (an SCTP common header is
>    unmistakable in hex).
> 3. `streamer/tests/test_kndtls.c` — 31→**32 checks**: the app-data
>    leg now asserts the first-record capture byte-exact against the
>    test server's "kn-r3b" record.
> 4. `docs/RTSP.md` §19 (run 13 ×2 invocations + the reference
>    re-read + the delta + the run-14 matrix), `docs/DTLS.md`
>    (R3-B LIVE-PROVED; Q3 partially / Q4 fully / Q5 instrumented /
>    Q6 answered+measured), `PROJECT-STATE.md` (round XXXII),
>    `tests/T10_build_and_test.py` UNCHANGED this round (no new
>    suites — the expectation stays **74**).
>
> ## Verification status (what ran WHERE, round XXXII)
> * Sandbox: native 15 suites PASS; cross mingw x64+i686 -Werror PASS
>   incl. the kn_dtls real-body + test real-section legs; paranoid
>   knvstd legs clean -O0..-O3; ctest **17/17 ×2** (test_kndtls real
>   32/32 / stub 6/6); ASan+UBSan+LSan clean; the OpenSSL-3 interop
>   PASS; sandbox_session_e2e **43/43**; T10 POSIX dry ALL PASS 64;
>   T13 ALL PASS 63.
> * Target (round XXXII's evidence, user-reported): T10 **ALL PASS
>   74** zero warnings; R7 run 13 ×2 invocations (docs/RTSP.md §19).
> * Target (PENDING — round XXXIII asks for): T10 (expect 74) + **R7
>   run 14** — paste rtsp_response/rig_info/sock_pair/dtls_arm/
>   dtls_ready/PLAY/ice_preplay verbatim (+ handoff/summary if PLAY
>   200).
>
> ## Notes for the target build
> * No new files to wire this round beyond syncing the three changed
>   sources + two headers' consumers: knvstd_main.c, kn_dtls.{h,c},
>   test_kndtls.c (the gates and T10 harness are unchanged since
>   round XXXI's sync).
> * Behaviour notes: the video socket now emits NATT pings from the
>   moment the pump thread starts (pre-PLAY) — a firewall prompt, if
>   the machine ever shows one, would appear there (run 13 already
>   proved inbound UDP flows on 49006; 49005 send-side is new).



# Round XXXI delta, rev 21 (R7 run 12 = the real fingerprint flew and PLAY STILL refused → R3-B SHIPPED: kn_dtls, the DTLS 1.2 ACTIVE client on the bundle socket + the reference's N/N+1 socket pair + the pump-thread DTLS state machine)

> **Rev 21 supersedes rev 1-20.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/RTSP.md` §18** (run 12 + the R3-B design +
> the three mbedTLS landmines), `docs/DTLS.md` (R3-A/R3-B SHIPPED).
>
> **Round XXXI headline: run 12 finalized the fingerprint question — a
> 4th rig (80-84-170-153) accepted the ANNOUNCE carrying the REAL
> P-256 fingerprint (`identity cert:real fp_len 95`, ANNOUNCE 200,
> 4137 B) and then held PLAY in the exact run-11 silence (timeout,
> ws_pings_in 0, ws_closes_in 0, frames_out 5 vs text 4) while the
> pump saturated UDP beside it (20921 ms, 2841+947, 126/126 answered,
> 3788 validated, invalid 0). The announced fingerprint ALONE is not
> the gate — the handshake behind it is. R3-B SHIPPED: `kn_dtls` (the
> portable-mbedTLS DTLS 1.2 ACTIVE client, fingerprint-pinned via the
> double gate, inject-queue BIO, ms-clock retransmission timers), the
> reference's socket pair (video N / bundle N+1, preferred
> 49005/49006), the pump-thread DTLS state machine (ClientHello gated
> on the first REAL inbound STUN, post-ready 100 ms NATT-only cadence,
> the worker persists through the media loop in non-unified mode), and
> the evidence lines (`sock_pair`, `dtls_arm`, `dtls_ready`/`dtls_fail`,
> the grown `ice_preplay`, `unified` in `ice_creds`).**
>
> ## Changed files (round XXXI)
> 1. `streamer/{include/kn,src}/kn_dtls.{h,c}` (NEW) — the R3-B module:
>    real body under KN_TLS_BACKEND_MBEDTLS on BOTH platforms (the
>    kn_cert discipline), honest stub elsewhere (`available()==0`,
>    loud refusal). DTLS 1.2 exactly (min=max), the kn_cert handles
>    as own_cert (R3-A kept them for this), MTU 1200, HVR automatic
>    (client side). PINNING = the double gate: the verify callback
>    clears chain flags ONLY on SHA-256(peer DER) == the pinned
>    DESCRIBE fingerprint, and poll() refuses READY unless
>    verify_result==0 AND pin_matched (latching KN_DTLS_ERR_PIN +
>    close_notify). VERIFY_OPTIONAL is REQUIRED for this pattern —
>    mbedTLS fails VERIFY_REQUIRED without a CA chain before any
>    callback (ssl_tls.c:8071); landmine documented in RTSP.md §18.
>    BIO recv returns WANT_READ on an empty queue, NEVER TIMEOUT
>    (a BIO TIMEOUT is read as an expired retransmission delay →
>    resend() → a NULL-flight segfault; landmine #2). Post-ready app
>    records are drained and counted (D-t4 — R3-C hooks here).
> 2. `streamer/tests/test_kndtls.c` (NEW, 15th suite) — self-adaptive:
>    REAL battery (31 checks) under the CMake mbedtls backend, the
>    stub contract (6) elsewhere. The in-test server is shaped like
>    the rig: HVR cookies ON, the CLIENT cert pinned server-side
>    (pinning BOTH directions), the same inject-queue BIO discipline;
>    legs: full handshake over real loopback UDP, DTLSv1.2 +
>    ECDHE-ECDSA assertions, first-flight loss in BOTH directions
>    (the timer callbacks earn their keep), wrong-pin refusal at the
>    ready gate (fatal latch + dirty verify flags preserved),
>    inject-queue overflow (drop-new, counted), mid-handshake free
>    (ASan bait), NULL guards. The server implements the documented
>    HVR contract: HELLO_VERIFY_REQUIRED → session_reset + re-armed
>    transport id (landmine #3). PLUS the `--openssl <port> <fp>` mode
>    for the cross-implementation leg.
> 3. `tools/dtls_openssl_check.py` (NEW) — the independent-stack gate
>    (sandbox-only; the target has no openssl): generates an OpenSSL
>    ECDSA P-256 self-signed cert, runs `openssl s_server -dtls1_2`,
>    drives `test_kndtls --openssl` pinned to that cert's fingerprint.
>    PASS proves our ClientHello/cookie-round/cert/Finished interop
>    with a NON-mbedTLS DTLS stack (measured: DTLSv1.2,
>    TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256, ~24 ms).
> 4. `streamer/src/knvstd_main.c` — the phase-C/D/E rework:
>    `sess_reserve_pair` (49005/49006 preferred → <= 32 dynamic
>    adjacent attempts → honest failure; the `sock_pair` evidence
>    line); SETUP's client port = unified ? bundle : video
>    (nvst_rtsp.rs:1176-1183); the BUNDLE socket is connected to the
>    SETUP peer pre-ANNOUNCE (localAddress + clientBundlePort +
>    a=candidate come from it — the reference test pins 49006) and to
>    ice_dest after; the video socket connects to the video peer at
>    media start; the save/restore reconnect dance is GONE (one peer
>    per socket for life). The cert stays ALIVE (R3-A freed it after
>    fingerprinting) and arms `kn_dtls_client` when ice + remote_fp +
>    real cert + backend all exist (`dtls_arm` line, lengths only).
>    The pump: demux PING→PONG | STUN | DTLS(20..63, kn_dtls_peek) |
>    invalid; the DTLS arm gate = the first real inbound STUN
>    (req_in||ok_in — the :8196 rule); cadence 20 ms (3×STUN+1×NATT)
>    → post-ready 100 ms NATT-only (:7837-7846); `dtls_ready` /
>    `dtls_fail` emitted FROM THE THREAD at the transition (timing is
>    evidence; stdio locks keep lines atomic); the snapshot handshake
>    (snap_req/snap_done) lets main read the counters while the
>    worker runs; `pump_persist` (non-unified + thread) keeps the
>    bundle worker alive through the media loop (D21 untouched — the
>    media pipeline stays single-threaded on its own socket);
>    teardown order = join → kn_dtls_free → close sockets → (teardown
>    label) kn_cert_free. `ice_preplay` grew pongs/persist + the six
>    dtls counters; `ice_creds` grew `unified` (answers DTLS.md open
>    question 4 even when PLAY fails). The sequential fallback and the
>    disablePlay window keep the STUN/NATT-only shape (documented).
> 5. `CMakeLists.txt` — kn_dtls.c into kncore; kndtls into the test
>    foreach (ctest 16→17 ×2); `target_compile_definitions(test_kndtls
>    PRIVATE KN_TLS_BACKEND_MBEDTLS=1)` under the mbedtls backend (the
>    suite's real section compiles only there — schannel builds run the
>    stub battery, the honest split).
> 6. `tools/build_native_tests.sh` — KNCORE += kn_dtls.c; the 15th
>    suite builds (stub) + runs as rc19.
> 7. `tools/cross_check.sh` — kn_dtls stub -c + the REAL-body legs
>    (kn_dtls.c AND test_kndtls.c's real section) under the exact
>    target flags ×2 arches whenever third_party/mbedtls exists (the
>    round-XXII rule); the stub suite link (needs -lws2_32 via
>    kn_sock.c); KCORE += kn_dtls.c.
> 8. `tests/T10_build_and_test.py` — suites += test_kndtls (15) →
>    target expectation **74** (run 12's T10 reported 72, not the
>    round-XXX-expected 73: the target harness predates the kncert
>    addition — SYNC THIS FILE from the repo before the next run).
> 9. `docs/RTSP.md` §18 — run 12 + the R3-B design record + the three
>    mbedTLS landmines. `docs/DTLS.md` — R3-B SHIPPED; open questions
>    1/2 answered, 6 added (does the rig request the client cert; does
>    it tolerate a DTLS-complete but SCTP-silent client — D-t4).
> 10. `PROJECT-STATE.md` — status/Next steps/session log round XXXI.
>
> ## Verification status (what ran WHERE, round XXXI)
> * Sandbox: native 15 suites PASS (stub legs); cross mingw x64+i686
>   -Werror PASS incl. the kn_dtls real-body + test real-section legs;
>   ctest 17/17 ×2 backends (mbedtls: test_kndtls REAL 31/31;
>   schannel: stub 6/6); ASan+UBSan+LSan clean on the real battery;
>   the OpenSSL interop leg PASS; paranoid knvstd legs
>   (-Wunused-but-set-variable -Wformat-truncation=2) clean host+cross;
>   sandbox_session_e2e 43/43 (unchanged by design — the rig doesn't
>   speak DTLS; upgrading it is an R3-C-era candidate); T10 POSIX dry
>   ALL PASS 64; T13 ALL PASS 63.
> * Target (PENDING — round XXXII asks for): `python
>   tests\T10_build_and_test.py` with the SYNCED harness (expect ALL
>   PASS **74**, zero warnings — test_kndtls runs the REAL battery in
>   the target's mbedtls build) then **R7 run 13** (the same --session
>   command; watch sock_pair → dtls_arm → dtls_ready/dtls_fail → PLAY).
>
> ## Notes for the target build
> * No new link libraries: kn_dtls uses the mbedtls/mbedx509/
>   mbedcrypto statics kncore already links (MBEDTLS_SSL_PROTO_DTLS +
>   MBEDTLS_SSL_COOKIE_C are ON in the pinned 3.6.0 default config).
> * No new PE imports (F1 holds): mbedTLS is static; the pump thread
>   reuses the round-XXVIII kernel32 CreateThread.
> * The kn_dtls real body compiles ONLY with the backend define — the
>   target's T10 (mbedtls default) exercises it for real; a schannel
>   fallback build gets the honest stub and knvstd logs
>   `dtls_arm {"armed":0,"reason":"backend_unavailable"}`.



# Round XXX delta, rev 20 (R7 run 11 = the FINAL elimination datum: PLAY refused with a saturated pump + TOTAL WS silence → R3-A SHIPPED: kn_cert, the real ECDSA P-256 self-signed identity replacing D-s1's placeholder fingerprint)

> **Rev 20 supersedes rev 1-19.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/DTLS.md`** (R3-A now SHIPPED, R3-B/C/D
> planned), `docs/RTSP.md` §17 (run 11 + R3-A), §16 (the elimination
> table), §15/§12.
>
> **Round XXX headline: run 11 completed the elimination — a third rig
> (77-111-249-146) took the full chain to ANNOUNCE 200 (4137 B — the
> new 4/22 identity shapes live on the wire) and then held PLAY in
> TOTAL silence (timeout; ws_pings_in 0, ws_closes_in 0, frames_out 5
> vs text 4) while our pump thread saturated the UDP path beside it
> (pump_ms 20920, 2820+940 sent, 126/126 rig probes answered, 3753 rig
> successes validated, invalid 0). Three rigs, three refusals, one
> constant: no real DTLS behind the announced fingerprint. So R3-A
> SHIPPED: `kn_cert` — the portable-mbedTLS self-signed identity
> (ECDSA P-256, SHA-256 fingerprint over the exact DER in the
> announced colon-hex form, fixed 2020-2035 validity, random serial;
> parsed x509+pk kept for R3-B behind opaque handles; a selfcheck KAT
> proves SHA-256(der)==fp + byte-identical re-parse + sign/verify
> through the certificate's public key). The CMake mbedtls backend
> compiles the REAL body on both platforms → the sandbox ctest runs
> the real battery (10 checks, ASan+UBSan clean after it caught a
> double-free in the first test draft); every other build gets the
> honest stub (4 checks). knvstd announces the real fingerprint when
> available and logs it: the new `identity` event
> ({"ufrag_len":4,"pwd_len":22,"fp_len":95,"cert":"real"}).**
>
> ## Changed files (round XXX)
> 1. `streamer/{include/kn,src}/kn_cert.{h,c}` (NEW) — the R3-A module
>    (real body + honest stub, the kn_net discipline; the header is
>    mbedTLS-free — R3-B casts the opaque handles back).
> 2. `streamer/tests/test_kncert.c` (NEW) — the FOURTEENTH suite,
>    self-adaptive: real battery under the mbedtls backend (10 checks),
>    stub contract elsewhere (4).
> 3. `streamer/src/knvstd_main.c` — the identity block: kn_cert
>    generate → fingerprint → free (R3-A only announces; R3-B keeps it
>    alive for the handshake), the `identity` evidence line, the
>    placeholder fallback stays honest.
> 4. Build wiring: `CMakeLists.txt` (kn_cert.c into kncore; kncert into
>    the test foreach → ctest 15→**16** both backends),
>    `tools/build_native_tests.sh` (KNCORE + the stub-mode suite run,
>    rc18), `tools/cross_check.sh` (the stub -c leg + the REAL-body
>    cross-compile under the exact target flags whenever
>    third_party/mbedtls exists — the round-XXII rule — + the suite
>    link), `tests/T10_build_and_test.py` (suites += test_kncert →
>    **target expectation 72 → 73**).
> 5. `docs/RTSP.md` §17, `docs/DTLS.md` (R3-A marked SHIPPED), this
>    file, `PROJECT-STATE.md`.
>
> ## Verification (round XXX, sandbox)
> **test_kncert real battery 10/10 under ASan+UBSan** (CMake mbedtls
> backend; it caught the double-free pre-ship) · stub contract 4/4 in
> the shell gates · native **14 suites** PASS · cross x64+i686 -Werror
> PASS incl. the kn_cert real-body legs ×2 · paranoid knvstd legs ×3
> clean · CMake fresh both backends 0 warnings + ctest **16/16 ×2**
> (mbedtls: real 10/10; schannel: stub 4/4 — the honest split) ·
> sandbox_session_e2e **43/43** (unchanged; the rig doesn't validate
> fingerprints yet — an R3-B-era upgrade candidate) · T10 POSIX dry
> ALL PASS **63** (62 + the new suite).
> **Target expectations:** next T10 → ALL PASS **73** (suite +1; zero
> warnings). Then **R7 run 12**: same command — watch the `identity`
> line (`"cert":"real"`, fp_len 95). If PLAY still refuses (the
> leading theory: the gate needs the actual handshake), R3-B (kn_dtls
> on the bundle socket inside the pump thread) is the decisive round.

---

# Round XXIX delta, rev 19 (R7 run 10: the pump thread live-proven, PLAY went 455→SILENCE → the elimination table is empty except D-s1; R3 (real DTLS) promoted to the stage-2 PLAY blocker — docs/DTLS.md; the official ICE identity shapes mirrored)

> **Rev 19 supersedes rev 1-18.** Canonical state: `PROJECT-STATE.md`.
> Design records: **`docs/DTLS.md`** (NEW — the R3 spike plan),
> `docs/RTSP.md` §16 (run 10 + the elimination table), §15, §12.
>
> **Round XXIX headline: run 10 closed the last transport difference —
> the pump thread ran through the ENTIRE 20 s PLAY window
> (`"thread":1`, pump_ms 20525, 2760 STUN + 920 NATT, the rig's 123
> probes ALL answered, 3671 rig successes validated, invalid 0) — and
> PLAY got NO response at all (rc -3 timeout, ws_text stuck at 4, no
> close frame). A different rig than run 9 refused differently
> (instant 455 vs hold-until-timeout), same root state: the transport
> never reaches "PLAY-valid". With the wire shape, the bursts, their
> continuity through the exchange and the credential ladder ALL proven
> live, the elimination table has one row left: the announced DTLS
> identity (D-s1 placeholder, no DTLS stack). The reference announces
> a REAL certificate's fingerprint and runs ICE+DTLS on the bundle from
> receiver-spawn time (nvst.rs:5562-5603, :6106-6160 — client is ICE
> CONTROLLING, remote fingerprint PINNED from DESCRIBE, video SRTP
> keys stay ANNOUNCE-derived, DTLS carries SCTP/rtcp1+input). R3 is
> promoted to the stage-2 PLAY blocker; docs/DTLS.md phases it
> (R3-A portable-mbedTLS certificate identity → R3-B DTLS client on
> the bundle → R3-C usrsctp channels → R3-D WS control pings), each
> phase sandbox-KAT'd before its live verdict, with the round-XXII
> gate rule baked in. Small round-XXIX deltas while the verdict
> stands: the official ICE identity shapes (4-char ufrag / 22-char
> password over the base64-ish alphabet — nvst.rs:5545-5565; our
> 16/24-hex was tolerated but Bifrost length-checks 16-char ufrags),
> a 400 ms pump warm-up so PLAY can never outrun the first bursts
> (run 10 raced the thread start), and the full WS counter set in
> rtsp_detail (pings/pongs/closes/frames_out — run 11 will show what
> the rig does on WS during the hold).**
>
> ## Changed files (round XXIX)
> 1. `streamer/src/knvstd_main.c` — `sess_local_identity` mirrors
>    generate_gfn_local_ice_credentials exactly (alphabet, 4/22);
>    the PLAY path gains the 400 ms warm-up when the pump thread
>    spawns; `sess_rtsp_detail` reports ws_pings_in/ws_pongs_in/
>    ws_pongs_out/ws_closes_in/ws_frames_out.
> 2. `docs/DTLS.md` (NEW — the R3 plan with provenance + open
>    questions), `docs/RTSP.md` §16, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXIX, sandbox)
> native 13 suites PASS (libraries unchanged — the identity/warm-up/
> counters are knvstd glue; E2E keeps its own static identity by
> design) · cross x64+i686 `-Werror` PASS · paranoid
> `-Wunused-but-set-variable -Wformat-truncation=2` clean on
> knvstd_main.c ×3 compilers · sandbox_session_e2e **43/43** · CMake
> both backends 0 warnings + ctest 15/15 ×2 · T10 dry ALL PASS 62.
> **Target expectations:** next T10 → ALL PASS **72**, zero warnings.
> Then **R7 run 11** (same command): the expected verdict is PLAY
> still refused (455 or timeout) — the FINAL elimination datum — with
> the enriched counters showing the rig's WS behaviour during the
> hold; queue/ICE evidence keeps accumulating. After that R3-A starts
> (docs/DTLS.md).

---

# Round XXVIII delta, rev 18 (R7 run 9: the control node MIGRATES constantly — re-learning live-proven, ZERO 503s; ICE fully bidirectional yet PLAY still 455 → the pump goes concurrent, PLAY goes one-shot, the Mjolnir NATT keepalive lands in the media loop)

> **Rev 18 supersedes rev 1-17.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §15** (run 9 + this round), §14 (run
> 8), §13 (the 503-window policy), §12 (the pump's birth).
>
> **Round XXVIII headline: run 9 validated TWO rounds of fixes live and
> exposed the next layer. (1) The XXVII control-node re-learning fired
> ~25 times in one wait — the session's nominated front migrates across
> np-ams/sth/lon/par/frk almost every poll — and with it the 503 class
> VANISHED (zero 503s across ~28 polls; runs 7/8 were us pinned to a
> node the record had drifted from, which is what its ip:null skeleton
> was saying). (2) The XXV STUN toolkit is live-proven byte-correct:
> ice_creds enabled with the pingVersion-6 bundle-username ladder leg
> (lengths 13/13/13 — the real DESCRIBE carries iceUsernameFragment +
> serverBundlePort), and ice_preplay shows a fully bidirectional,
> integrity-validated exchange (stun 147, natt 49, rig requests 99 —
> ALL answered, rig successes 327, invalid 0). (3) PLAY STILL 455 — so
> burst existence is necessary but not sufficient; the remaining deltas
> vs the reference: our sequential pump goes QUIET during the PLAY
> exchange (the reference's receiver thread never stops bursting) and
> D-s1 (no real DTLS identity — unproven either way, the rig sent no
> DTLS: invalid_in 0). (4) The 455 retry hit a DEAD WS (rc -2,
> ws_close -1, no close frame — the rig drops the RTSP context after
> refusing). Fixes: the pump moves to a bounded worker thread spanning
> exactly the PLAY exchange (kernel32/pthread; joined before the media
> loop — D21 untouched; honest sequential fallback), PLAY becomes
> ONE-SHOT, and the media loop gains the reference's Mjolnir NATT
> keepalive (nvst.rs:8849-8905 — "the pings are what ROUTES video
> here": USERNAME {setup payload}:{local ufrag} to the video peer, 20
> ms pre-first-inbound then 100 ms, inline in the loop's tick).**
>
> ## Changed files (round XXVIII)
> 1. `streamer/src/knvstd_main.c` — the pump refactor (sess_pump ctx +
>    sess_ice_pump_run core + thread entry/start/stop + the sequential
>    wrapper kept as fallback), the one-shot PLAY block (retry loop
>    removed; `ice_preplay` gains `"thread"`), the phase-E NATT
>    keepalive (+ the `natt_keepalive {"sent":N}` evidence line; the
>    summary schema untouched for make_rx_capture --verify), the
>    relearn counter (`cm_control` `"n"`, `ready` `"relearns"`), and the
>    POSIX `<pthread.h>` include (Win32 uses the F1-set kernel32
>    thread APIs — no new imports).
> 2. `docs/RTSP.md` §15, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXVIII, sandbox)
> native 13 suites PASS (counts unchanged: knsock 98, knrtsp 150, kncm
> 148 — the delta is knvstd glue) · cross x64+i686 `-Werror` PASS incl.
> the paranoid knvstd legs (host+x64+i686, `-Wunused-but-set-variable`
> `-Wformat-truncation=2`) · sandbox_session_e2e **43/43** (keeps its
> own sequential pump = the independent wire-shape implementation; the
> thread scheduling is Win32-target territory — POSIX session mode
> refuses at the TLS gate) · CMake fresh both backends 0 warnings +
> ctest **15/15 ×2** · T10 POSIX dry ALL PASS **62**.
> **Target expectations:** next T10 → ALL PASS **72**, zero warnings.
> Then **R7 run 10**: PLAY 200 → `natt_keepalive.sent` + real Mjolnir
> SRTP in the summary (R32's final stage-2 gate + the R34 firewall
> answer); PLAY still 455 with `"thread":1` + rich ice counters → the
> DTLS identity (D-s1) becomes the prime suspect and stage-3 R3 becomes
> the prerequisite for PLAY.

---

# Round XXVII delta, rev 17 (R7 run 8: the 503 body speaks — an app-level session skeleton with sessionControlInfo.ip:null; the T10-r20 warning fixed + gated permanently; control-node re-learning; --session-keep)

> **Rev 17 supersedes rev 1-16.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §14** (run 8 + this round), §13 (the
> 503-window policy), §12 (the pre-PLAY pump).
>
> **Round XXVII headline: run 8 proved the round-XXVI policy end-to-end
> — the client RODE the 503 window (ten transient rounds, the D-s4
> backoff visible: gap_ms 6000→9000→13500→20000) until the user's
> Ctrl-C, then cm_cancel 200. And the body excerpt made the 503 TALK:
> it is CloudMatch's OWN JSON — a session skeleton with
> `sessionControlInfo.ip:null` and the resourcePath still echoing the
> id (the front knows the record but cannot name its owning state — the
> §13 seat-transition window from the inside; the record is alive, the
> same-window DELETE answered 200). This round parses the deep fields
> (status/errorCode/requestStatus incl. unifiedErrorCode — now filled
> even on rejected payloads) into the cm_poll_error line, fixes the
> T10-r20 warning (write-only consec_transient — target GCC 16.2's
> -Wunused-but-set-variable is NOT in sandbox GCC 12's -Wall; the fifth
> target catch, and both gate scripts now carry a PERMANENT paranoid
> knvstd leg), closes a real reference-fidelity gap (the control node is
> RE-LEARNED from every successful poll like store_active :1333-1336 —
> a mid-queue node migration is followed, ip:null keeps the current
> host), and adds --session-keep (skip the abort cancel; the user's
> browser-RESUME workflow).**
>
> ## Changed files (round XXVII)
> 1. `streamer/src/knvstd_main.c` — consec_transient removed (the T10
>    catch); the cm_poll_error diag-view (parses non-200 JSON bodies:
>    `unified_error`/`sess_status`/`err_code` + desc fallback; excerpt
>    160→240); per-poll control-node re-learning (+ `cm_control`
>    `source:"relearned"` events); `--session-keep` (usage text incl.).
> 2. `streamer/{include/kn,src}/kn_cm.*` — `kn_cm_session.error_code` +
>    `.unified_error`; status/phase/errorCode filled BEFORE the
>    sessionId gate so rejected payloads stay diagnosable.
> 3. `streamer/tests/test_kncm.c` — the run-8 503-skeleton fixture
>    (rejected but fully diagnosed; ip:null → no control host),
>    unifiedErrorCode in the SESSION_LIMIT fixture, errorCode pinned on
>    the run-5 capture fixture: **147 → 148 checks**.
> 4. `tools/build_native_tests.sh` + `tools/cross_check.sh` — the
>    PERMANENT paranoid knvstd leg (`-Werror
>    -Wunused-but-set-variable -Wformat-truncation=2`; host + both
>    cross arches) — the round-XXII gate-hardening pattern applied to
>    the fifth target-catch class.
> 5. `docs/RTSP.md` §14, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXVII, sandbox)
> native 13 suites PASS (kncm 148, knrtsp 150, knsock 98) · cross
> x64+i686 `-Werror` PASS incl. the new paranoid legs (3 .o produced) ·
> sandbox_session_e2e **43/43** · CMake fresh both backends 0 warnings +
> ctest **15/15 ×2** · T10 POSIX dry ALL PASS **62**.
> **Target expectations:** next T10 → ALL PASS **72** with ZERO
> warnings (the r20 failure was exactly the removed counter). Then
> **R7 run 9** — the round-XXV PLAY pump is STILL awaiting its first
> live try; expect either the seat transition → ice_creds/ice_preplay →
> PLAY(455?)→200 → first media, or a 503 window whose parsed fields
> (sess_status/err_code/unified_error/cloud_status/desc) name the
> cloud-side state — and a `relearned` cm_control line if the session
> migrates nodes.

---

# Round XXVI delta, rev 16 (R7 run 7: the ~2-minute seat-transition 503 storm — transient statuses are now OUTLIVED (D-s4), hard rejections and transport deaths stay bounded, the 503 body is captured)

> **Rev 16 supersedes rev 1-15.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §13** (the run-7 measurement + the
> failure-class policy), §12 (run 6 + the pre-PLAY pump), §11/§10.
>
> **Round XXVI headline: run 7 (two attempts) measured a new cloud-side
> window — after ~2 minutes of static `seat_step:1` (the SAME duration
> runs 5/6 spent there before transitioning to step 3), every GET poll
> started answering 503 while a same-moment DELETE to the same host
> answered 200 (and the user watched the session disappear from the web
> UI — the round-XXIII cancel is target-proven; the immediate re-create
> hit no SESSION_LIMIT). The old policy killed the wait after 5 such
> rounds (~30 s) — right inside the window the seat transition needs.
> The reference never gives up on transient poll errors (the UI cadence
> just continues; only 404 clears). Round XXVI mirrors that with a
> failure-CLASS policy: transient 4xx/5xx are OUTLIVED until
> queue_timeout, transport deaths bounded at 10 rounds, hard HTTP/cloud
> rejections bounded at 5 (new stop_reason `cm_poll_rejected`), a gentle
> 4→6→9→13.5→20 s backoff while failing (D-s4), and the `cm_poll_error`
> line now carries the response BODY excerpt + class + next gap — run 8
> will name the 503's sender.**
>
> ## Changed files (round XXVI)
> 1. `streamer/src/knvstd_main.c` — phase A poll loop only: the
>    failure-class counters (consec_transient/transport/hard) + the new
>    give-up bounds, `sess_poll_gap` (the D-s4 backoff), the upgraded
>    `cm_poll_error` evidence line (body/class/gap_ms; `cm_json` is
>    cleared per round so the excerpt is always THIS round's), and the
>    `cm_poll_rejected` stop_reason. No library/API changes; phases B-E
>    and the round-XXV pump untouched.
> 2. `docs/RTSP.md` §13, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXVI, sandbox)
> native 13 suites PASS (knsock 98, knrtsp 150, kncm 147 — unchanged,
> the delta is knvstd glue) · cross x64+i686 `-Werror` PASS (with the
> kn_net_mbedtls legs — third_party/mbedtls present) · paranoid
> `-Wformat-truncation=2` clean on knvstd_main.c (host -O1 + mingw -O2
> ×2) · sandbox_session_e2e **43/43** · CMake 0 warnings + ctest
> **15/15 ×2** · T10 POSIX dry ALL PASS **62**. The policy itself is
> Win32-TLS-gated — its proof is run 8.
> **Target expectations:** next T10 → ALL PASS **72** (suite count
> unchanged). Then **R7 run 8** — either the step-3 transition → PLAY →
> first media, or a ridden-out 503 window whose `body` excerpts name the
> source; `queue_timeout` (600 s) + `cm_cancel` bound the worst case.

---

# Round XXV delta, rev 15 (R7 run 6: DESCRIBE/SETUP/ANNOUNCE 200 LIVE — PLAY 455 root-caused: the reference's pre-PLAY hole-punch burst was missing → the STUN toolkit + the credential ladder + the pump)

> **Rev 15 supersedes rev 1-14.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §12** (the run-6 measurement + the
> fix), §11 (run 5), §10 (runs 1-4).
>
> **Round XXV headline: run 6 walked the whole live signaling chain —
> queue → ready → OPTIONS/DESCRIBE/SETUP/ANNOUNCE all 200 on the real
> rig (the round-XXIV DESCRIBE fix confirmed live; SETUP peered on the
> first sweep round; ANNOUNCE carried the 4164-byte negotiated SDP with
> the D-s1 placeholder identity and was ACCEPTED) — and PLAY was
> rejected 455 "Method Not Valid in This State" with a healthy WS layer
> (rtsp_detail: ws_err 0, no close, 5 text frames). Root cause: the
> reference spawns its media receivers BETWEEN announce() and PLAY
> (lib.rs:894→937→1117) and the bundle thread immediately fires the
> hole-punch burst (3× STUN binding + 1× NATT ping per 20 ms,
> nvst.rs:7438/:7842-7900, dest = bundle_peer() :1880) — we sent PLAY
> with zero UDP egress. Fixed with the sequential pre-PLAY pump (D-s2)
> + the reference's exact STUN wire shapes + its remote-ufrag ladder.
> Bonus catch: the ladder fixtures exposed a pre-existing
> kn_rtsp_sdp_attribute bug (first-match instead of the reference's
> LAST-match across ";;" segments). The user also proved R23 RESUME is
> real: run 6's session was left alive and played in the browser.**
>
> ## Changed files (round XXV)
> 1. `streamer/{include/kn,src}/kn_sock.*` — the STUN toolkit: crc32
>    (IEEE reflected), stun_build (byte-exact port of
>    build_authenticated_stun_packet :4977-5012 — MI HMAC-SHA1 with the
>    RFC 5389 §10.5 length adjustment + FINGERPRINT CRC^0x5354554E),
>    xor_addr_v4, inspect (the :5131-5146 gate — magic/exact-length/
>    MANDATORY fingerprint), check_integrity (:139-160, const-time).
>    test_knsock now links kn_sha1 (+ the vendored sha1) — build wiring
>    updated in both gate scripts.
> 2. `streamer/{include/kn,src}/kn_rtsp.*` — the ladder: increment_hex
>    (:2305), bundle_natt_username (:1327), resolve_remote_ufrag
>    (:2329); describe extracts ufrag_v1 + server_bundle_port; **the
>    sdp_attribute LAST-match fix** (".last()" semantics — pinned by a
>    dedicated check; media_control legitimately stays first-segment).
> 3. `streamer/src/knvstd_main.c` — phase C/D: `ice_creds` (lengths
>    only) + the credential gate (:1729, D-s3 honest skip on the
>    ping6-no-payload error case :1248) + bundle-peer re-connect/
>    restore + `sess_ice_pump` (20 ms bursts, answers the rig's
>    requests, counts successes) + PLAY 455-retry (3 attempts, 600 ms
>    re-pump) + the `ice_preplay` counters line + `attempt` in the PLAY
>    event.
> 4. `streamer/tests/test_knsock.c` — test_stun: goldens from an
>    independent Python implementation (hmac/hashlib/zlib), RFC 2202
>    HMAC, tamper/short/no-fingerprint rejects, xor-addr round-trip:
>    **82 → 98 checks**.
> 5. `streamer/tests/test_knrtsp.c` — test_ice_ladder (fixtures
>    transcribed from the reference's own tests :3860-3920) + the
>    last-wins sdp pin: **132 → 150 checks**.
> 6. `tools/fake_rig.py` + `tools/sandbox_session_e2e.c` — the rig
>    runs an independent Python STUN validator/responder on the media
>    socket and GATES PLAY (first PLAY always 455; later need ≥1
>    validated request); the harness pumps, asserts the ladder
>    (a1b24800→a1b24801) and the 455→200 sequence: **36 → 43 checks**
>    (rig evidence: ice_req 12, ice_resp 12, play_455 1, plays 1).
> 7. `docs/RTSP.md` §12, this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXV, sandbox)
> native **13 suites PASS** (knsock 98, knrtsp 150, kncm 147) · cross
> x64+i686 `-Werror` PASS · ASan+UBSan clean on both touched suites
> (the single known vendored sha1.c:26 note aside) · kn_rtsp.c +
> knvstd_main.c paranoid `-Wformat-truncation=2` clean (host -O1 +
> mingw -O2 ×2); kn_sock.c's two =2 hits are PRE-EXISTING round-XVII
> honest-truncation error strings (standard gates clean — left alone,
> churn discipline) · CMake fresh both backends 0 warnings + ctest
> **15/15 ×2** · sandbox_session_e2e **43/43** · T10 POSIX dry ALL PASS
> **62** (baseline).
> **Target expectations:** next T10 → ALL PASS **72** (suite count
> unchanged). Then **R7 run 7**: expect `ice_creds enabled:1`,
> `ice_preplay` counters > 0, PLAY attempt 1 possibly 455 → attempt 2
> 200, then `handoff` + the media loop (the first real Mjolnir SRTP →
> kn_rx → AUs = R32's final stage-2 gate). If PLAY still 455s:
> `ice_preplay.ok_in` (did the rig answer?) + `ice_creds` (which ladder
> leg fired) localize it.

---

# Round XXIV delta, rev 14 (R7 run 5: FIRST READY SESSION — the queue fix confirmed live; DESCRIBE died on the 8 KiB per-message scratch → fixed + binary frames + Host:port + rtsp_detail instrumentation)

> **Rev 14 supersedes rev 1-13.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §11** (the run-5 measurement + the
> fix), §10 (run 1-4 + the control-host fix), §9 (round XXI deviations).
>
> **Round XXIV headline: R7 run 5 took the round-XXIII fix to the live
> cloud — `cm_control` switched to np-waw-01 (source sessionControlInfo),
> the session progressed (seat_step 1→3) and reached `status:2 ready`
> with a real rtsps endpoint; the RTSPS/WSS transport then answered
> OPTIONS 200 on a REAL GFN rig and died at `DESCRIBE rc:-2 status:0`.
> Root-cause candidates (both produce exactly that signature at the
> first big response; both fixed): kn_rtsp_request read each WS message
> into an 8 KiB scratch — real DESCRIBE SDPs are bigger, while the
> reference accumulates whole messages up to the 4 MiB total
> (nvst_rtsp.rs:559-566, :614-623) — and the adapters refused BINARY
> frames the reference accepts (:621). Also fixed: the Host header now
> carries :port like the reference (:1070-1076). Failed rtsp steps now
> emit an `rtsp_detail` evidence line (kn_rtsp err + WS close code +
> frame counters), so run 6 measures even if it fails elsewhere.**
>
> ## Changed files (round XXIV)
> 1. `streamer/src/kn_rtsp.c` — `kn_rtsp_request` reads DIRECTLY into
>    the accumulation buffer's tail (no per-message cap below the 4 MiB
>    total; runaway-shim guard kept; the seam contract is unchanged).
> 2. `streamer/src/knvstd_main.c` — `sess_ws_recv_text` accepts
>    TEXT|BINARY; Host = the rtsps authority WITH the port; NEW
>    `sess_rtsp_detail` (err[160] + ws_err/ws_close/ws_text/ws_bin)
>    emitted at all five step-failure sites (OPTIONS/DESCRIBE/SETUP/
>    ANNOUNCE/PLAY).
> 3. `tools/sandbox_session_e2e.c` — the same binary acceptance + Host
>    authority (the two implementations stay mirrored).
> 4. `tools/fake_rig.py` — the DESCRIBE SDP is padded to ~11 KiB and
>    delivered as TWO binary messages (the run-5 shape, exercised over
>    real sockets by the E2E).
> 5. `streamer/tests/test_knrtsp.c` — `test_big_messages`: a >19 KiB
>    single-message DESCRIBE parses (the old scratch died here) + the
>    same response split across two messages accumulates: **127 → 132
>    checks**.
> 6. `docs/RTSP.md` §11 (the run-5 record incl. the queue-position
>    observation: the NATIVE queue published step transitions, not
>    per-poll ticks — NOT a defect, keep observing), this file,
>    `PROJECT-STATE.md`.
>
> ## Verification (round XXIV, sandbox)
> native 13 suites PASS (test_knrtsp 132/132, test_kncm 147/147) ·
> cross x64+i686 `-Werror` PASS · paranoid `-Wformat-truncation=2` clean
> (kn_rtsp.c/knvstd_main.c, host -O1 + mingw -O2 both arches) · CMake
> 0 warnings + ctest **15/15 ×2** · sandbox_session_e2e **36/36** with
> the new rig behaviour (binary + split + padded SDP over real loopback
> sockets; announce key echo + plays=1 asserted by the rig) · T10 POSIX
> dry-run ALL PASS **62** (baseline).
> **Target expectations:** T10 r19 → ALL PASS **72** (suite count
> unchanged). Then **R7 run 6**: same command; expect DESCRIBE 200 →
> the SETUP sweep → ANNOUNCE (D-s1's first real verdict) → PLAY → the
> media loop; any failure now names its stage in `rtsp_detail`.

---

# Round XXIII delta, rev 13 (R7 runs 1-4: the first REAL CloudMatch traffic — the control-host bug FOUND + FIXED)

> **Rev 13 supersedes rev 1-12b.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §10** (the R7 measurement + the fix),
> §9 (round XXI status + deviations D-s1/D-cm1..3), `docs/AUTH.md`.
>
> **Round XXIII headline: the user ran `knvstd --session` against the
> LIVE production backend for the first time (T10 r17 build, ALL PASS 72;
> a real pasted GFNJWT + appId 107057914). Create works — 4/4 real
> sessions with plausible live queue positions. The queue wait FAILED:
> polls froze at a stale queuePosition (40+ identical lines) and ended in
> `cm_poll_failed`. Root cause: the poll loop stayed on the global create
> front (`prod.cloudmatchbeta.nvidiagrid.net`) instead of switching to
> the REGIONAL control node the create response nominates
> (`sessionControlInfo.ip` — `np-waw-01.cloudmatchbeta.nvidiagrid.net`
> in the user's DevTools capture; exactly where the official client polls
> at ~4.1 s and exactly what cloudmatch.rs does: control_base learned at
> :401-414/:1149-1152/:1966-1984, every poll goes there :652-656).
> Fixed + the poll loop hardened to the reference's retry/404 semantics
> + a best-effort DELETE cancel so an aborted wait no longer orphans a
> session (runs 1-4 each left one behind).**
>
> ## Changed files (round XXIII)
> 1. `streamer/{include/kn,src}/kn_cm.*` — `kn_cm_session` learns
>    `control_host/control_port/have_control` (sessionControlInfo) and
>    `req_status_code/req_status_desc` (requestStatus — filled even when
>    the session extraction fails, so a rejected payload still says why);
>    NEW `kn_cm_control_base` (the trust mirror of
>    `trusted_learned_server_base` + `public_session_host/_ipv4`,
>    cloudmatch.rs:2400-2456: https-only, port 443, private/loopback/
>    link-local/CGNAT/multicast/reserved/.local/userinfo/path all
>    rejected — a response can never redirect polling into a private
>    address; 0 = fall back to the create host, which is also what keeps
>    the loopback fake-rig E2E unchanged); NEW `kn_cm_cancel` (DELETE
>    `v2/session/{id}` = the reference's stop_at :789-800; `cm_round`
>    now tolerates `out == NULL`).
> 2. `streamer/src/knvstd_main.c` — phase A: the poll-target switch (new
>    `cm_control` JSONL event = the R7 evidence line), cadence 2 s → 4 s
>    (`SESS_POLL_MS`, the capture's Δ≈4.1 s), per-round 3-attempt
>    transient retry (408/425/429/5xx, 250/750 ms — get_session
>    :1369-1394), 404 → `session_not_found`, requestStatus.statusCode≠1
>    → failed round with the cloud's description in a `cm_poll_error`
>    event, failed rounds never clobber the live session state
>    (poll-into-temp + commit), 4 consecutive bad rounds tolerated before
>    `cm_poll_failed`, the `not_ready` line carries `rc`+`http_status`,
>    the `cm_create` error line carries the cloud's requestStatus
>    (cloud_status 11 = SESSION_LIMIT — runs 1-4 orphaned four queued
>    sessions), best-effort `cm_cancel` on every not_ready exit except
>    404, `cm_created` now includes the initial queue_position,
>    `--session-queue-timeout` default 180 → 600 s.
> 3. `streamer/tests/test_kncm.c` — NEW control-base battery (the exact
>    R7 capture fixture + the full accept/reject matrix + requestStatus-
>    on-failed-extract) and the cancel wire checks (DELETE line, Host,
>    GFNJWT header, 204/404/200-with-body, empty-id guard): **91 → 147
>    checks**.
> 4. `tools/fake_rig.py` — answers `DELETE /v2/session/{id}` with 204 No
>    Content (the validate_delete_response shape); E2E legs unchanged.
> 5. `docs/RTSP.md` §10 (R7 runs 1-4 + the fix + the honest not-covered
>    list), this file, `PROJECT-STATE.md`.
>
> ## Verification (round XXIII, sandbox)
> native **13 suites PASS** (test_kncm 147/147, incl. ASan+UBSan+LSan
> run) · cross x64+i686 `-Werror` PASS · kn_cm.c + knvstd_main.c clean
> under `-Wformat-truncation=2` at -O1 host / -O2 mingw (x64+i686) ·
> CMake fresh configure both backends: 0 warnings, ctest **15/15 ×2** ·
> sandbox_session_e2e **36/36** (exercises the control-host FALLBACK
> path: the rig's 127.0.0.1 is rejected by the trust gate → create-host;
> the ACCEPT path is unit-pinned by the capture fixture — a public-host
> loopback rig is impossible without DNS games) · T10 POSIX dry-run ALL
> PASS **62** (62 is also the pre-delta baseline on this tree — the
> round-XXI "61" predates a check added since; verified by a stashed
> rerun).
> **Target expectations:** T10 r18 → ALL PASS **72** (suite count
> unchanged; test_kncm's internal count is not a T10 check). Then **R7
> run 5** = the same `--session` command: expect `cm_control` with
> `source:"sessionControlInfo"`, a MOVING queue_position, and — if a
> seat arrives — phase B onward (D-s1 gets its first real answer there).
>
> ## Known gaps carried forward (honest)
> * Phases B-E aborts still don't cancel the session (churn discipline;
>   next round).
> * IPv6 trust checks are prefix-class approximations, not a full
>   IpAddr decode (documented in kn_cm.c; reject-direction only affects
>   exotic control hosts, which fall back to the create host).
> * D-cm1/D-cm2 measured OK AT CREATE (4/4 real sessions); seat/streaming
>   stages still unmeasured.

---

# Round XXII delta, rev 12b (T10 r16 build-fix + gate hardening)

> **Rev 12b (round XXII):** target T10 r16 caught a round-XXI uncounted-
> replace edit in `kn_net_mbedtls.c` (port snippet landed in the DNS
> lookup; the Win32 body had never been sandbox-compiled). Fixed (DNS
> literal restored) + a second latent bug fixed (missing `<stdlib.h>` —
> i686-only) + **`cross_check.sh` now cross-compiles kn_net_mbedtls.c on
> both arches whenever third_party/mbedtls exists** (the permanent gap
> closure). All gates re-run green; target expects T10 r17 ALL PASS 72.
> Full story: PROJECT-STATE.md round-XXII log, artifacts/T10-round16/.

---

# Round XXI delta, rev 12 (STAGE 2 PRODUCTION PATH: TLS stream + kn_cm/kn_http/kn_json + fake-rig E2E + knvstd --session)

> **Rev 12 supersedes rev 1-11.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md` §9** (round-XXI status + deviations
> D-s1/D-cm1..3) and **`docs/AUTH.md`** (the OAuth device-flow research,
> login implementation = stage 4).
>
> **Round XXI headline: the stage-2 production signaling path is BUILT
> end-to-end — CloudMatch (POST v2/session + poll + session extraction)
> over a persistent verified TLS-1.2 stream, RTSPS-over-WSS with the
> round-XX cores, the SRTP/peer handoff into kn_rx, and `knvstd
> --session` glue with the connected-UDP media loop — proven against an
> independent Python rig over real loopback sockets (36 checks) before
> any target run.** The only remaining unknowns are cloud-side (R7) and
> documented as D-s1/D-cm1/D-cm2.
>
> ## New files (round XXI)
> 1. `streamer/{include/kn,src}/kn_json.*` — minimal zero-alloc JSON
>    reader (1024-node pool, depth 32, raw spans, unescape incl.
>    surrogate pairs, int64-exact).
> 2. `streamer/{include/kn,src}/kn_http.*` — HTTP/1.1 one-shot client on
>    a stream seam (CL/chunked/read-to-EOF, resumable chunk parser,
>    injected clock).
> 3. `streamer/{include/kn,src}/kn_cm.*` — CloudMatch: the create body
>    (field-for-field with build_create_body), GFNJWT + nv-client header
>    set (Content-Type text/plain quirk), create/poll, and the
>    session_info extraction subset (rtspsEndpoints DERIVATION from
>    connectionInfo usage 16/14+alp{1,6}, media usage 2|17, queue
>    4-source fallback, ready/resumable, phase table).
> 4. `streamer/tests/test_kncm.c` — the THIRTEENTH suite (91 checks:
>    json/http/cm sections, fixtures per cloudmatch.rs:4073-4085).
> 5. `tools/fake_rig.py` — the independent Python rig (HTTP + WS server,
>    own RFC 6455 framing; asserts the ANNOUNCE key echo + PLAY count).
> 6. `tools/sandbox_session_e2e.c` — sandbox-only E2E harness (36
>    checks) driving kn_cm→kn_ws→kn_rtsp→handoff→connected UDP against
>    the rig over real sockets.
> 7. `docs/AUTH.md` — OAuth device-flow research record (endpoints,
>    forms, headers, token semantics; implementation = stage 4).
>
> ## Changed files (round XXI)
> 1. `streamer/include/kn/kn_net.h` + `src/kn_net_mbedtls.c` +
>    `src/kn_net.c` — the persistent TLS-1.2 stream API
>    (`kn_net_tls_stream_*`, Win32+mbedTLS only; complementary honest
>    stubs elsewhere; kn_tcp_open gained a port parameter for 322).
> 2. `streamer/{include/kn,src}/kn_sock.*` — `kn_sock_udp_connect`
>    (kernel peer filter + getsockname local-address refresh);
>    test_knsock 70→82 checks.
> 3. `streamer/src/knvstd_main.c` — `--session` mode (phases A-E per
>    docs/RTSP.md §9; token length-only logging; D-s1 placeholder ICE
>    identity; TEARDOWN on exit) + usage/dispatch.
> 4. Build wiring: CMakeLists + both shell gates + T10 (13 suites;
>    target r16 expects 72 checks) + .gitignore unchanged.
>
> ## Verification (round XXI, sandbox)
> native 13 suites 1309 PASS · cross x64+i686 -Werror PASS · knvstd
> =2-paranoid clean (host -O1, mingw -O2) · CMake 0 warnings + ctest
> 15/15 x2 backends · T13 ALL PASS 51/51(sanitized)/63(mbedTLS) ·
> sandbox_session_e2e 36/36 · T10 dry 61. Self-review/KAT catches:
> the chunked-parser stranded cursor (1-byte KAT), the json root-index
> confusion (cleaned pre-ship), kn_net.c's missing top-level stdio
> (POSIX stubs), hex math in the E2E expectations (1194884=0x123B84).
> NOT target-compiled yet: the Win32 body of kn_net_tls_stream_* and
> --session's Win32 sleep path (T10 r16 is their first compile; both
> mirror verified patterns line-for-line).

---

## Round XX (rev 11, superseded — detail below)

> **Rev 11 supersedes rev 1-10b.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/RTSP.md`** — §1-5 are the wire specification
> (read from nvst_rtsp.rs/cloudmatch.rs @ a49b124 with line refs), §8 is
> this round's implementation status + deviations D-w1..D-r4.
>
> **Round XX headline: the stage-2 signaling cores are implemented and
> sandbox-proven — RFC 6455 client framing (`kn_ws`, 84 checks incl. the
> RFC goldens) and the full RTSP-over-WS state machine (`kn_rtsp`, 127
> checks with every fixture transcribed from the reference's own tests,
> incl. a scripted OPTIONS→DESCRIBE→SETUP→ANNOUNCE→PLAY E2E).** No
> production wiring yet: the TLS stream transport, CloudMatch, OAuth and
> `--session` are round XXI. No existing module changed behaviour (kn_rx /
> kn_sock / knvstd untouched this round).
## New files (round XX)
1. `streamer/include/kn/kn_ws.h` + `streamer/src/kn_ws.c` — RFC 6455
   client framing over an injected byte-stream seam (`kn_ws_io`);
   handshake with CSPRNG key + accept verification; masked sends;
   resumable mid-frame timeouts; control-frame handling (ping→pong echo,
   close answered); bounded caller-owned reassembly; exposed test hooks
   `kn_ws_base64` / `kn_ws_accept` / `kn_ws_rand`. Win32 RNG = `rand_s()`
   (msvcrt — NO new PE import); POSIX = getrandom/urandom fallback.
2. `streamer/tests/test_knws.c` — the ELEVENTH suite (84 checks):
   RFC 4648 + RFC 6455 §1.3 goldens, handshake request/response classes,
   wire-exact frames at every length-boundary, fragmentation, control
   frames, all protocol-error classes, oversize, 1-byte-chunk recv,
   timeout resume. In-memory duplex shim (the framing contract is
   transport-independent by design; real-socket/TLS E2E is the round-XXI
   fake-rig gate).
3. `streamer/include/kn/kn_rtsp.h` + `streamer/src/kn_rtsp.c` — the pure
   RTSP-over-WS client: serializer/parser/request loop (CSeq==Request-Id,
   Bifrost CRLF padding, Request-Id fallback, TEARDOWN mismatch-skip,
   20 s / 4 MiB limits), SDP toolset (`sdp_attribute` with the `||`/`;;`
   quirks, `media_control`, `runtime_key` + `{key_id:024X}` salt, the
   8-profile scan defaulting AEAD_AES_256_GCM_8, `parse_video_peer` with
   the span<16 collapse, the URI ladder), `extract_describe`, the full
   SETUP sweep (poison-rule stop, bundle fallback, 3×3 s injected-sleep
   retries, remaining-deadline narrowing), `build_announce` (complete
   §3 attribute list) and `negotiate_announce` (16-rule range-validated
   offer resolution + clamps + line rewrite).
4. `streamer/tests/test_knrtsp.c` — the TWELFTH suite (127 checks):
   reference-test fixtures transcribed (nvst_rtsp.rs:3443-3470 parser
   cases, :3470+ official control, :2923+ announce params), scripted-
   server full-sequence E2E with byte-exact request inspection, every
   SETUP branch (happy/400-ladder/peerless+bundle/peerless-exhaust/
   fatal-500/client-udp-form), TEARDOWN skip semantics, error paths.
   Injected clock+sleep keep retry rounds instant and countable.

## Changed files (round XX)
1. `CMakeLists.txt` — kn_ws.c + kn_rtsp.c into kncore; knws + knrtsp in
   the test foreach (ctest 12→14).
2. `tools/build_native_tests.sh` + `tools/cross_check.sh` — the two new
   suites + module compiles (12 native suites, 1183 checks).
3. `tests/T10_build_and_test.py` — suites += test_knws, test_knrtsp
   (target r15 expects 71 checks = r14's 69 + 2).
4. `docs/RTSP.md` — §8 implementation status + deviations D-w1..D-r4.

## Verification status (round XX, sandbox)
native 12 suites 1183 checks PASS · cross mingw x64+i686 -Werror PASS ·
ASan+UBSan+LSan clean on both new suites (the single runtime error is the
known vendored sha1.c:26 note, triggered by SHA-1 padding — streamer code
has zero diagnostics) · -Wformat-truncation=2 clean -O0..-O3 on kn_ws.c,
kn_rtsp.c AND test_knrtsp.c · CMake ctest 14/14 both TLS backends ·
T10 POSIX dry-run ALL PASS 60. Self-review catches this round (all fixed
pre-gate): the kn_ws mid-payload timeout was NOT resumable in the first
draft (frame state added), the RT_FOR_LINES macro ran its body per
character (LF-only KAT caught it), the parser status-line scan leaked
into the header block (GARBAGE KAT caught it), blank SDP lines aborted
the attribute scan (the `;;` quirk KAT caught it).



> **Rev 10b (round XVIII)** — the target T13 r1 finding, fixed:
> 1. `streamer/src/knvstd_main.c` — `lb_json_path` (the loopback sanitizer
>    T11 proved) promoted to the platform-neutral section as THE
>    stdout-JSON string escaper, hardened with a `>=0x7F -> '?'` rule
>    (cp1250 argv); every argv/path/foreign-string printf-JSON site routed
>    through it (unknown_selftest name, diagnostics bundle, replay
>    error+summary, capture error/detail/summary, decode error detail).
>    Root cause of T13 r1's only 2 failures: `"out":"C:\Users\..."` raw —
>    `\U` is not a legal JSON escape, summary lines became unparseable.
> 2. `streamer/src/kn_selftest.c` — sibling `st_json_str`; gs.parse's
>    KN_GS_CAPTURE path sanitized (=2 paranoid count unchanged at 9).
> 3. `tests/T13_udp_e2e.py` — per-leg "every stdout line parses as JSON"
>    checks, `rcvbuf_actual` logged per decode leg (R34), POSIX
>    backslash-path contract leg. Target expectation: T10 r14 ALL PASS
>    69 (rebuild is MANDATORY before T13), T13 r2 ALL PASS 61.
> 4. `docs/SOCK.md` — landmine 6 + §7 target record (T10 r13 ALL PASS 69,
>    T13 r1 analysis). Evidence: `artifacts/T13-round1/`.
> Sandbox re-verification: 10 suites 972 PASS, cross PASS, ctest 12/12
> x2, T13 ALL PASS 51/51(sanitized)/63(mbedTLS+gcm256), T10 dry 58.


> **Rev 10 supersedes rev 1-9.** Canonical state: `PROJECT-STATE.md`.
> Design record: **`docs/SOCK.md`** (provenance table with corrected
> nvst.rs line refs, landmines 1-5, the single-thread decision, the E2E
> graph and all sandbox evidence), plus `docs/RX.md` §Stage-2 remainders
> (sockets bullet now DONE).
>
> **Round XVII headline: the stage-2 socket layer is implemented and proven
> end-to-end on loopback — sender → live capture → replay-verify → live
> decode (paced + blast + SRTP cm128/gcm256) — under ASan+UBSan+LSan, on
> three build configurations, before the target ever saw it.** The pure
> kn_rx core is UNCHANGED (one additive API); the round-XVI KAT battery
> still pins every behaviour.
>
> Context this round also CLOSED two target items: T10 round 12
> (**ALL PASS 67 checks** — test_knrx 209/209 on Win7, rx.vectors 153/153,
> registry 13, zero warnings) and the G2-offline demo from `idk_g2.txt`
> (replay verified byte-identical; the demo's `--verify` failure was a
> missing `> replay.jsonl` redirect on the knvstd command — reconstructed
> and PASSED in the sandbox: `VERIFY PASS: 60 frames byte-identical`).

## New files
1. `streamer/include/kn/kn_sock.h` + `streamer/src/kn_sock.c` — the
   bounded UDP layer: refcounted WSAStartup (Win32) / no-op (POSIX),
   monotonic µs/ms clock (QPC / CLOCK_MONOTONIC), IPv4 parse/text,
   open/bind with SO_RCVBUF request + getsockopt READBACK (Linux caps,
   Windows honours — both reported), select-based recv with timeout and
   honest per-platform truncation semantics, sendto, lifetime counters.
   BOTH platforms real (no stubs). Zero allocation.
2. `streamer/tests/test_knsock.c` — the TENTH suite (70 checks):
   clock, ipv4 helpers, lifecycle/refcounts/closed-socket discipline,
   100-datagram varying-size round-trip byte-exact, timeout path,
   65 507 B max datagram, 512-datagram burst drain with seq markers,
   the PLATFORM-DELTA truncation case (Win32 WSAEMSGSIZE-loss vs POSIX
   silent truncation), bad-argument matrix. Section-isolated, never
   aborts (round-XVI suite rule).
3. `tools/send_rx_capture.py` — paced UDP sender for kn_ring_dump-format
   captures (the offline stand-in for a real Mjolnir session): hybrid
   sleep+spin pacing on the capture's µs timeline (sub-ms jitter even
   where Sleep() floors at ~2.9 ms — R30), `--speed 0` blast mode,
   json-lines summary, py3.8-compatible.
4. `tests/T13_udp_e2e.py` — the stage-2 socket gate (loopback only, no
   account): capture leg → vector leg (replay of the LIVE-RECORDED file
   verified against the reference manifest) → decode paced → decode blast
   → SRTP cm128 → SRTP gcm256 (auto-enabled from the binary's
   rx.vectors `srtp_backend` evidence) → `sock.loopback` from the shipped
   binary → check_jsonl. Reuses T10's build; wipes t13work/t13logs at
   start; T13_wynik.txt + T13_skrot.txt. Runs on POSIX for real (not a
   dry-run — real sockets; only the gcm256 leg may skip in bare builds).
5. `tools/sandbox_stream_decode.c` — sandbox-only functional proof of the
   persistent decoder session (NOT a gate, like sandbox_decode_check.c):
   60/60 through per-AU pushes, corrupt-AU survival, sticky abort, and
   the CONTROL case that justifies the API (one-shot per AU delivers 2 of
   60 — P-frames die without references).
6. `tools/fetch_ffmpeg_headers_sandbox.py` — scripts BOOTSTRAP step 4
   (pinned FFmpeg headers at bf0e8316 + avconfig/ffversion stand-ins);
   sandbox convenience, writes into the gitignored third_party/.
7. `docs/SOCK.md` — the round-XVII design record (this delta's canon).

## Changed files
1. `streamer/include/kn/kn_decode.h` + `streamer/src/kn_decode.c` —
   NEW persistent stream session: `kn_decode_stream_open/push/flush/
   stats/close`. One complete Annex-B AU per push (NO parser on this
   path — the round-IX parser-flush landmine cannot occur; end-of-stream
   is `flush()`), codec context persists so inter-frame references
   survive, corrupt AUs counted and the session continues, sticky
   callback abort (-4), cumulative stats, honest stubs in bare builds.
   Reuses kd_ctx/kd_deliver/kd_send_drain unchanged. LSan caught (and
   this round fixed) a `k.nvbuf` leak in close().
2. `streamer/include/kn/kn_rx.h` + `streamer/src/kn_rx.c` — ONE additive
   API: `kn_rx_video_request_keyframe()` (public wrapper over the
   internal `request_keyframe()` helper — same flag, same counter
   discipline). Used by --decode's queue-overflow path. No behaviour
   change to anything the KAT battery pins.
3. `streamer/tests/test_knrx.c` — +6 checks for the new API
   (take-and-clear, collapse-on-repeat, counter counts SETS, NULL-safe):
   184→190 bare, 209→215 mbedTLS.
4. `streamer/src/kn_selftest.c` + `include/kn/kn_selftest.h` — the 14th
   self-test `sock.loopback` (PORTABLE + GATED: 64-datagram burst
   byte-exact, honest 50 ms timeout, SO_RCVBUF readback, closed-socket
   ERR_STATE; ~50 ms). Relevance mapping learned
   `socket/udp/recv/bind/capture/decode` → re-runs sock.loopback
   (session.capture/session.decode failures re-prove the UDP layer).
5. `streamer/tests/test_knselftest.c` — registry 13→14 (two sites),
   host ok_count 4→5, sock.loopback not-skipped+ok on BOTH platforms,
   two new relevance cases (E_CAPTURE_SOCKET, E_DECODE_NO_DATA):
   108→115 checks host.
6. `streamer/src/knvstd_main.c` — graceful-stop plumbing
   (SetConsoleCtrlHandler / SIGINT+SIGTERM → flag polled every loop
   iteration → clean summary + flush); `--capture` (streaming
   kn_ring_dump-format file writer with 32-records/100 ms flush policy,
   64 KiB ring mirror dumped to `<log_dir>/wire_capture.log` on failure,
   RTP-seq meta from the unencrypted header, empty capture = FAILED +
   amplification); `--decode` (single-threaded recv→process→poll loop at
   the reference's 10 ms cadence on the QPC-ms clock, capacity-2 AU queue
   with the reference overflow policy — drop-new + `kn_rx_inval_lose` +
   request_keyframe, per-AU JSONL with sha1, capped rx-event JSONL,
   kf_request lines (unsent until stage 3 — recorded, never dropped),
   1 s transport stats log lines, verify-compatible summary incl.
   `"frames"` alias, ring mirror, `--decode-expect-frames`, AU dump dir,
   pipeline-only honesty in stub builds). usage() + main() dispatch for
   both modes with full argument range validation.
7. `CMakeLists.txt` — kn_sock.c into kncore; `knsock` in the test
   foreach (ctest 11→12).
8. `tools/build_native_tests.sh` + `tools/cross_check.sh` — kn_sock.c +
   test_knsock wiring (10th suite; cross links -lws2_32).
9. `tests/T10_build_and_test.py` — suites += test_knsock; registry check
   13→14; the portable-battery loop (fec/srtp/gs/rx) += sock.loopback
   ("ok in the target binary" — a skip would be a stub, it is real on
   every platform). POSIX dry-run: 57→58 checks.
10. `tests/README.md` — round-XVII header (T13 usage) + the G2-offline
    demo block FIXED: the `> replay.jsonl` redirect is now IN the command
    (the idk_g2.txt failure mode), with an explicit warning line.
11. `.gitignore` — /t13logs/ + /t13work/.
12. `docs/RX.md` — the "Sockets" remainder bullet → DONE (round XVII)
    with the corrected `nvst.rs:235` line ref (the old text cited :222 —
    stale even before this round; :235 is the constant, :5682 the
    setsockopt call site, verified against a49b124).

## Verification status (what ran WHERE)
* Sandbox (this round): native 10 suites PASS (test_knrx 190 bare;
  test_knsock 70); cross mingw x64+i686 -Werror PASS incl. kn_sock and
  the extended knvstd; -Wformat-truncation=2 clean at -O0/-O2/-O3 on
  every touched file (R28 pre-check; the two host-only =2 notes in
  kn_sock/kn_decode are errsz==1 over-approximations — bounded-safe,
  same class as round XI's decision); ctest 12/12 in BOTH CMake
  configurations (schannel + mbedTLS, tls=mbedtls recorded);
  ASan+UBSan+LSan: test_knrx 190/190, test_knsock 70/70 with ZERO
  streamer-code diagnostics (the single runtime error is the PRE-EXISTING
  vendored sha1.c:26 int-promotion UB — pinned by hash, must not be
  edited; sanitizer runs use recover mode, see docs/SOCK.md §4);
  T13 ALL PASS on three builds (44 checks FFmpeg-bare, 44 under
  sanitizers, 55 with mbedTLS incl. the gcm256 leg); T10 POSIX dry-run
  ALL PASS 58; sandbox_stream_decode 23 checks PASS; cross-machine
  parity: target's idk_g2 console JSONL == sandbox replay output
  byte-identical (60 AU lines + counters), regenerated capture matches
  the target's 119 records / 360 333 B.
* Target (PENDING — round XVIII asks for): `python
  tests\T10_build_and_test.py` (expect ALL PASS ~69: 10 suites,
  registry 14, test_knrx 215 in the mbedTLS build) and `python
  tests\T13_udp_e2e.py` (expect ALL PASS 55 — first Winsock execution
  of the live modes; watch the `rcvbuf_actual` line: Windows should
  apply the full 8 MiB). No GPU/T11 rerun needed (no graphics touched).
