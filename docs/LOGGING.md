# Logging subsystem (`knlog`) — implementation status

Spec: `BOOTSTRAP.md` §7 (decision D13: first-class subsystem, stage 1, never retrofitted).
Language: English everywhere (D12). This module is the *instrument* for every live
attempt — login, queue and streaming cannot be tested in a sandbox, so the log is the
only evidence (BOOTSTRAP §7 preamble).

## Modules

| File | Role |
|---|---|
| `include/kn/knlog.h` | public API: levels, channels, config, event context, correlation, session id, failure amplification, redaction helpers |
| `src/knlog.c` | implementation: dual sinks (.log human + .jsonl structured), rotation, flush policy, runtime levels, stats |
| `include/kn/knring.h`, `src/knring.c` | §7.10 bounded raw-capture ring buffers (fixed slots, oldest-overwritten, dump-on-failure) |
| `tests/test_knlog.c` | 105 checks — runs on the Linux host **and** on the Windows 7 target (`ctest`) |
| `tests/test_knring.c` | 34 checks — same |
| `tools/check_jsonl.py` | strict JSON parse + §7.2 schema validation of every `.jsonl` line |
| `tools/build_native_tests.sh` | host build + run + jsonl validation (one command) |
| `tools/cross_check.sh` | R21 gate: mingw x64+i686 cross-compile, `-Wall -Wextra -Werror`, F1 flag verification via `-dM` |

## Spec conformance

| §7 item | Status |
|---|---|
| 7.1 two outputs, one event stream | **done** — one `kn_log_emit`, rendered to `<ch>.log` + `<ch>.jsonl` |
| 7.2 mandatory fields (ts RFC3339-ms UTC, mono_ms, level, channel, correlation, session_id, pid, thread, build once per start) | **done** — build stamp emitted as `process_start` at init |
| 7.3 channels (8) + rotation 2 MiB × 3 generations; jsonl default only perf+transport; documented budget (60 MiB worst case) | **done** — sizes configurable; Windows-safe rotation (`remove` before `rename`) |
| 7.4 levels, default INFO, runtime changes without restart | **done** — `kn_log_set_level` / `kn_log_set_all_levels` |
| 7.5 per-stage checklists | belongs to each subsystem that logs (auth/http/rtsp/transport/media/input) — module provides the primitives (`kn_lctx_*`, redaction) they must use |
| 7.6 periodic perf snapshot | stage 1 — a caller-side emitter will use `KN_CH_PERF` + the field API (all §7.6 counters fit in one event) |
| 7.7 failure amplification | **hook done** — `kn_log_on_failure()` emits ERROR(code/reason/next_action), raises levels (all ≥ DEBUG, transport TRACE), seals (flush+fsync every event), invokes registered callbacks (bundle writer / self-test runner plug in here) |
| 7.8 diagnostic bundle | stage 1, next — writer registers as a failure callback; knlog already seals files so the bundle sees a complete tail |
| 7.9 self-tests | stage 1 — `knvstd --selftest=…` harness; ring dump + jsonl validation are the first two |
| 7.10 bounded raw ring buffers | **done** — `knring` (64 KiB/socket default, fixed slots, hexdump-on-failure via `kn_ring_dump`) |
| 7.11 whitelist redaction | **done (C side)** — `kn_redact_key_is_secret` (spec patterns + `pwd`), `<redacted:N>` lengths, header whitelist + `kn_redact_allow_header`, auth scheme extraction; auto-applied by `kn_lctx_str` on secret-like keys. Python-side boundary mirror: stage 4 |
| 7.12 rules from lost data | **done** — flush+fsync on WARN/ERROR and everything after sealing; no `fseek` over own output anywhere (ring dump rewrites whole files; jsonl is append+flush); isolation via callbacks, not in-module risk |

## API quick start (C)

```c
kn_log_config cfg = {0};
cfg.log_dir = "logs";
cfg.build_stamp = "a1b2c3d release x86_64-w64-mingw32-gcc-16.2.0";
kn_log_init(&cfg);                       /* defaults: INFO, 2 MiB x 3, jsonl=perf|transport */

char corr[KN_CORR_LEN + 1];
kn_log_new_correlation(corr);            /* one per user-visible operation */

kn_lctx c;
kn_lctx_init(&c, KN_CH_RTSP, KN_INFO, "SETUP response");
kn_lctx_corr(&c, corr);
kn_lctx_i64(&c, "status", 200);
kn_lctx_str(&c, "transport", "udp;unicast;client_port=49005");
kn_lctx_secret(&c, "srtpAesKeyHex", key_hex);   /* -> <redacted:N> */
kn_log_emit(&c);

/* on any operation failure — the single choke point (§7.7): */
kn_log_on_failure("session.create", "CM_HTTP_400", "create rejected",
                  "retry with corrected body");
```

Output:

```
2026-10-21T14:03:22.481Z INFO  [rtsp] c=a1b2c3d4e5f6 SETUP response status=200 transport="udp;unicast;client_port=49005" srtpAesKeyHex=<redacted:64>
{"ts":"2026-10-21T14:03:22.481Z","mono_ms":81234,"level":"INFO","channel":"rtsp","pid":4321,"thread":5678,"correlation":"a1b2c3d4e5f6","msg":"SETUP response","fields":{"status":200,"transport":"udp;unicast;client_port=49005","srtpAesKeyHex":"<redacted:64>"}}
```

## Design notes worth knowing before extending

- **Levels are 1-based** (`KN_TRACE=1 … KN_ERROR=5`) so a zeroed `kn_log_config`
  unambiguously means "use spec defaults" (a 0-based enum made TRACE
  indistinguishable from "unset" — caught by the unit test, not by review).
- **No TLS primitives**: thread-default correlations live in a 16-entry table keyed
  by thread id (`__declspec(thread)` was ignored by at least one mingw toolchain —
  the table is portable and bounded; the streamer has <16 threads).
- **Bounded by construction**: 32 fields + 2 KiB arena per event (stack), 4/6 KiB
  render buffers, overflow counted (`fields_dropped`, `truncated_lines`), never fatal.
- **Locking**: field building is lock-free; the mutex covers only sink
  write/rotate/flush + stats. Emission renders before locking.
- **Rotation** removes the destination before `rename` (Win32 refuses to replace),
  and rotates proactively (a file never exceeds `rotate_bytes` by more than one line).
- **Flush policy**: ERROR/WARN → `fflush`+`fsync`; INFO → `fflush`; DEBUG/TRACE →
  buffered — until sealing (after `kn_log_on_failure`) when everything hard-flushes.
- **Stats** (`kn_log_get_stats`) include `secret_values_redacted`, `write_errors`,
  `rotations` — the logging subsystem is itself observable (a dead sink is counted,
  never crashes the caller).

## How to run the tests

Host (Linux, this sandbox): `sh tools/build_native_tests.sh`
Cross gate (Win7 targets):  `sh tools/cross_check.sh`
On the Windows machine:

```bat
cd /d "%USERPROFILE%\Desktop\NewKarnyNow"
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build
build\test_knlog.exe & build\test_knring.exe
```

(That run additionally verifies real Win7 file/rename/flush semantics —
worth doing once before stage-1 integration.)
