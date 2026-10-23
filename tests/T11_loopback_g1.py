#!/usr/bin/env python3
"""T11_loopback_g1.py — the G1 (video half) gate on the TARGET machine.

Runs `knvstd --loopback` against the 720p High@5.1 asset (the shape the
§6.1 CPU budget was measured on) for the gate duration, then verifies the
G1 criteria (BOOTSTRAP §12 stage 1) FROM THE EVIDENCE:

  * steady 30 fps          — summary fps_mean + per-second perf.jsonl deltas
  * CPU within budget      — cpu_process_pct mean <= 45 (% of ALL cores,
                             §6.1 convention; 720p30 decode+NV12 = 28.9%)
  * RSS flat               — last-minute mean vs first-minute mean <= +4 MiB
  * bundle complete        — post-run --diagnostics + zipfile validation
  * no network involved    — the mode is loopback by construction
  * no WARN/ERROR anywhere in t11logs/*.log

Prerequisite: T10 has built build\\knvstd.exe (T11 does NOT build; run T10
first after syncing). The WASAPI half of stage 1 is NOT gated here yet —
this is "G1-video"; the audio delta extends this harness (docs/LOOPBACK.md).

Usage (ConEmu, repo checkout):
    cd /d "%USERPROFILE%\\Desktop\\NewKarnyNow\\tests"
    python T11_loopback_g1.py            # full gate: 600 s
    python T11_loopback_g1.py --quick    # smoke: 30 s
    python T11_loopback_g1.py --seconds=120 [--source=PATH]

Writes T11_wynik.txt + T11_skrot.txt next to the repo root. Send both back.
"""
import json
import os
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")
LOGDIR = "t11logs"                      # relative to ROOT when knvstd runs
WYNIK = os.path.join(ROOT, "T11_wynik.txt")
SKROT = os.path.join(ROOT, "T11_skrot.txt")
DEFAULT_SOURCE = os.path.join(ROOT, "streamer", "assets", "synthetic720.264")

IS_WINDOWS = sys.platform == "win32"
OUT = []


def log(msg=""):
    OUT.append(str(msg))
    print(msg)


def check(cond, results, label):
    results.append((bool(cond), label))
    log(("PASS " if cond else "FAIL ") + label)
    return bool(cond)


def exe(name):
    return os.path.join(BUILD, name + (".exe" if IS_WINDOWS else ""))


def main(argv):
    seconds = 600
    source = DEFAULT_SOURCE
    quick = False
    for a in argv[1:]:
        if a == "--quick":
            quick = True
            seconds = 30
        elif a.startswith("--seconds="):
            seconds = max(5, min(86400, int(a.split("=", 1)[1])))
        elif a.startswith("--source="):
            source = a.split("=", 1)[1]
        else:
            log("unknown arg: %s" % a)
            return 2

    results = []
    log("T11 loopback G1-video — %s — " % time.strftime("%Y-%m-%d %H:%M:%S")
        + ("QUICK 30 s smoke" if quick else "FULL G1 %d s" % seconds))
    log("repo root: " + ROOT)
    log("source:    " + source)

    if not IS_WINDOWS:
        log("\nT11 VERDICT: SKIPPED (POSIX host — loopback is Win32-only; "
            "knvstd refuses it honestly with rc=3)")
        check(True, results, "posix skip is honest")
        _write(results, None, None)
        return 0

    if not os.path.isfile(exe("knvstd")):
        log("build\\knvstd.exe missing — run T10 first")
        check(False, results, "knvstd.exe exists (run T10 first)")
        _write(results, None, None)
        return 1
    if not os.path.isfile(source):
        log("source clip missing: %s" % source)
        check(False, results, "720p source asset exists")
        _write(results, None, None)
        return 1

    # ---- the run -------------------------------------------------------
    # Clean evidence: wipe t11logs first — perf.jsonl/logs ACCUMULATE across
    # runs (round XIII: a --quick smoke added 31 lines to the 600-s analysis,
    # 632 vs 601). Every T11 run must start from an empty log dir.
    logdir = os.path.join(ROOT, LOGDIR)
    if os.path.isdir(logdir):
        for fn in os.listdir(logdir):
            fp = os.path.join(logdir, fn)
            if os.path.isfile(fp):
                os.remove(fp)
        log("t11logs wiped (%s kept)" % LOGDIR)

    cmd = [exe("knvstd"), "--loopback=" + source,
           "--loopback-seconds=%d" % seconds, "--log-dir=" + LOGDIR]
    log("\n$ " + " ".join(cmd))
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=seconds + 180)
        out = p.stdout.decode("utf-8", "replace")
        rc = p.returncode
    except subprocess.TimeoutExpired:
        out = "TIMEOUT after %ds" % (seconds + 180)
        rc = -999
    dt = time.time() - t0
    if out.strip():
        log(out.rstrip())
    log("[rc=%d in %.1fs]" % (rc, dt))

    summary = None
    try:
        summary = json.loads(out.strip().splitlines()[-1])
    except Exception as exc:                       # noqa: BLE001
        log("  cannot parse summary json: %s" % exc)
    check(rc == 0 and summary is not None and
          summary.get("stopped_reason") == "elapsed", results,
          "loopback ran to completion (rc=%d, reason=%s)" %
          (rc, summary.get("stopped_reason") if summary else "no summary"))
    if not summary:
        _write(results, summary, None)
        return 1

    log("\n--- summary ---")
    for k in ("source", "source_w", "source_h", "budget_valid", "seconds_run",
              "loops", "frames_decoded", "frames_presented", "frames_late",
              "fps_mean", "decode_ms_mean", "decode_ms_max",
              "present_ms_mean", "present_ms_max", "copy_ms_mean",
              "cpu_process_pct_mean", "cpu_process_pct_max", "rss_kb_first",
              "rss_kb_last", "rss_kb_peak", "refresh_hz_initial",
              "refresh_hz_last", "dpst_change_detected", "perf_lines",
              "vp_rc", "decode_rc", "fault"):
        log("  %-24s %s" % (k, summary.get(k)))

    check(summary.get("budget_valid") is True and
          summary.get("source_w") == 1280 and summary.get("source_h") == 720,
          results, "source is the 720p budget shape (G1 validity)")
    fps = float(summary.get("fps_mean") or 0.0)
    check(fps >= 29.4, results, "steady 30 fps (mean %.3f >= 29.4)" % fps)
    presented = int(summary.get("frames_presented") or 0)
    late = int(summary.get("frames_late") or 0)
    late_budget = max(5, presented // 500)          # 0.2 %
    check(late <= late_budget, results,
          "frames_late %d <= %d (0.2%% budget)" % (late, late_budget))
    cpu_mean = float(summary.get("cpu_process_pct_mean") or 0.0)
    check(cpu_mean <= 45.0, results,
          "CPU within budget: %.1f%% of all cores <= 45%% (§6.1: decode+NV12 "
          "alone = 28.9%%)" % cpu_mean)

    # ---- perf.jsonl ------------------------------------------------------
    log("\n--- perf.jsonl analysis ---")
    perf_path = os.path.join(ROOT, LOGDIR, "perf.jsonl")
    perf = []
    if os.path.isfile(perf_path):
        with open(perf_path, "r", encoding="utf-8", errors="replace") as f:
            for ln in f:
                ln = ln.strip()
                if not ln:
                    continue
                try:
                    rec = json.loads(ln)
                    if rec.get("msg") == "perf":
                        perf.append(rec.get("fields", {}))
                except Exception:                    # noqa: BLE001
                    pass
    check(len(perf) >= int(seconds * 0.95), results,
          "perf snapshots: %d lines (>= 95%% of %d s)" % (len(perf), seconds))
    if perf:
        deltas = [int(f.get("frames_presented") or 0) for f in perf]
        good = sum(1 for d in deltas if 27 <= d <= 33)
        outliers = [(i, d) for i, d in enumerate(deltas)
                    if not 27 <= d <= 33]
        check(good >= int(len(deltas) * 0.90), results,
              "per-second presented in [27,33]: %d/%d lines" %
              (good, len(deltas)))
        if outliers:
            log("  outlier seconds (idx, frames): %s" % outliers[:12])
        rss = [int(f.get("rss_kb") or 0) for f in perf if f.get("rss_kb")]
        if len(rss) >= 8:
            head = rss[:max(1, len(rss) // 10)]
            tail = rss[-max(1, len(rss) // 10):]
            drift = (sum(tail) // len(tail)) - (sum(head) // len(head))
            check(drift <= 4096, results,
                  "RSS flat: drift %+d kB (first-10%% vs last-10%%) <= +4096 kB"
                  % drift)
            log("  rss_kb: first=%d last=%d peak=%d drift=%+d" %
                (rss[0], rss[-1], max(rss), drift))
        dpst = [f for f in perf if int(f.get("dpst_change_detected") or 0)]
        check(not dpst, results,
              "no DPST refresh change during the run (%d flags)" % len(dpst))
        cpu_max_line = max(float(f.get("cpu_process_pct") or 0.0)
                           for f in perf)
        log("  cpu_process_pct: max single second %.1f%%" % cpu_max_line)
        dec_p95 = [float(f.get("decode_ms_p95") or 0.0) for f in perf]
        pres_p95 = [float(f.get("present_ms_p95") or 0.0) for f in perf]
        log("  decode_ms_p95: max %.2f ms   present_ms_p95: max %.2f ms" %
            (max(dec_p95), max(pres_p95)))

    # ---- logs clean ------------------------------------------------------
    log("\n--- log scan ---")
    logdir = os.path.join(ROOT, LOGDIR)
    errs, warns = [], []
    if os.path.isdir(logdir):
        for fn in sorted(os.listdir(logdir)):
            if not fn.endswith(".log"):
                continue
            with open(os.path.join(logdir, fn), "r", encoding="utf-8",
                      errors="replace") as f:
                for ln in f:
                    if " ERROR " in ln:
                        errs.append((fn, ln.rstrip()[:160]))
                    elif " WARN " in ln:
                        warns.append((fn, ln.rstrip()[:160]))
    check(not errs, results, "no ERROR in t11logs/*.log (%d)" % len(errs))
    check(not warns, results, "no WARN in t11logs/*.log (%d)" % len(warns))
    for fn, ln in (errs + warns)[:10]:
        log("  %s: %s" % (fn, ln))

    # ---- jsonl + bundle ----------------------------------------------------
    log("\n--- jsonl + bundle ---")
    p = subprocess.run([sys.executable,
                        os.path.join(ROOT, "tools", "check_jsonl.py"), LOGDIR],
                       cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    log(p.stdout.decode("utf-8", "replace").rstrip())
    check(p.returncode == 0, results, "check_jsonl over t11logs")

    p = subprocess.run([exe("knvstd"), "--diagnostics",
                        "--log-dir=" + LOGDIR], cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    bundle = None
    try:
        bundle = json.loads(
            p.stdout.decode("utf-8", "replace").strip().splitlines()[-1]
        )["bundle"]
        if bundle and not os.path.isabs(bundle):
            bundle = os.path.join(ROOT, bundle)
    except Exception as exc:                       # noqa: BLE001
        log("  cannot parse diagnostics json: %s" % exc)
    check(p.returncode == 0 and bundle and os.path.isfile(bundle), results,
          "post-run bundle written: %s" % bundle)
    if bundle and os.path.isfile(bundle):
        zf = zipfile.ZipFile(bundle)
        names = zf.namelist()
        check(zf.testzip() is None, results, "bundle CRCs valid")
        check(names and names[0] == "summary.json", results,
              "summary.json first")
        for req in ("build.json", "state.json", "selftest.json"):
            check(req in names, results, "bundle contains " + req)
        check(any(n.startswith("logs/") and n.endswith("perf.jsonl")
                  for n in names), results,
              "bundle contains logs/perf.jsonl (the G1 evidence)")
        check(os.path.getsize(bundle) <= 8 * 1024 * 1024, results,
              "bundle %d B <= 8 MiB" % os.path.getsize(bundle))
        zf.close()

    _write(results, summary, seconds)
    failed = [lbl for ok, lbl in results if not ok]
    return 1 if failed else 0


def _write(results, summary, seconds):
    failed = [lbl for ok, lbl in results if not ok]
    log("\n" + "=" * 60)
    if not IS_WINDOWS:
        log("T11 VERDICT: SKIPPED (posix)")
    elif failed:
        log("T11 VERDICT: FAIL (%d/%d checks failed)" %
            (len(failed), len(results)))
        for f in failed:
            log("  FAILED: " + f)
    else:
        log("T11 VERDICT: G1-VIDEO PASS (%d checks, %s s)" %
            (len(results), seconds if seconds else "?"))
    with open(WYNIK, "w", encoding="utf-8") as f:
        f.write("\n".join(OUT) + "\n")
    with open(SKROT, "w", encoding="utf-8") as f:
        f.write("T11 skrot — %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        if not IS_WINDOWS:
            f.write("VERDICT: SKIPPED (posix)\n")
        else:
            f.write("VERDICT: %s\n" % ("G1-VIDEO PASS" if not failed else
                                       "FAIL (%d)" % len(failed)))
        f.write("checks: %d total, %d failed\n" % (len(results), len(failed)))
        for ok, label in results:
            f.write("%s %s\n" % ("PASS" if ok else "FAIL", label))
        if summary:
            f.write("\nsummary:\n")
            for k in sorted(summary.keys()):
                f.write("  %-24s %s\n" % (k, summary[k]))
    print("\nwritten: %s" % WYNIK)
    print("written: %s" % SKROT)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
