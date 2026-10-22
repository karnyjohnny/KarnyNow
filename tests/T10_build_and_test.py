#!/usr/bin/env python3
"""T10_build_and_test.py — build + unit tests + SELF-TESTS + diagnostic bundle
verification on the TARGET machine (Windows 7, w64devkit toolchain).

Successor to T9: same build+test gate, extended with the new deliverables
(BOOTSTRAP §7.8/§7.9):
  1. cmake configure + ninja build (zero warnings expected)
  2. six unit-test binaries: knlog knring knfec knsrtp kndiag knselftest
  3. knvstd --list-selftests / --selftest=all  (REAL Win32 execution of
     clock.resolution and console.host — the sandbox can only compile them)
  4. knvstd --diagnostics -> bundle validated with python's zipfile
     (independent reader): summary.json first, CRCs, required entries,
     build.json shows the effective _WIN32_WINNT
  5. check_jsonl.py over the run's log dir
  6. PE subsystem version check on knvstd.exe (must say 6.01 = Win7)

Pure Python 3.8 stdlib. Run from cmd/ConEmu:
    cd /d "%USERPROFILE%\\Desktop\\NewKarnyNow\\tests"
    python T10_build_and_test.py

Writes T10_wynik.txt (full log) and T10_skrot.txt (small verdict) NEXT TO
the repo root (..\\). Send both back; keep logs\\t10_* and the bundle on disk
in case of failure.
"""
import glob
import json
import os
import struct
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")
LOGDIR = "t10logs"                      # relative to CWD when knvstd runs
WYNIK = os.path.join(ROOT, "T10_wynik.txt")
SKROT = os.path.join(ROOT, "T10_skrot.txt")

# FFmpeg dev tree (user's own w64devkit build; MUST be --enable-static, the
# F1/D8 -static link ignores .dll.a — CMake fails with the recipe if missing).
# Override with the KN_FFMPEG_PREFIX environment variable if it moves.
FFMPEG_PREFIX = os.environ.get(
    "KN_FFMPEG_PREFIX",
    r"C:\Users\KarnyJohnny\Desktop\ffmpeg\ffmpeg-win7-dev\usr\local")

OUT = []


def log(msg=""):
    OUT.append(str(msg))
    print(msg)


def run(cmd, cwd=ROOT, timeout=300):
    """Run a command, log it T9-style, return (rc, stdout+stderr, seconds)."""
    log("\n$ " + " ".join(cmd))
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        out = p.stdout.decode("utf-8", "replace")
        rc = p.returncode
    except subprocess.TimeoutExpired:
        out = "TIMEOUT after %ds" % timeout
        rc = -999
    except OSError as exc:
        out = "OSError: %s" % exc
        rc = -998
    dt = time.time() - t0
    if out.strip():
        log(out.rstrip())
    log("[rc=%d in %.1fs]" % (rc, dt))
    return rc, out, dt


def exe(name):
    return os.path.join(BUILD, name + (".exe" if os.name == "nt" else ""))


IS_WINDOWS = sys.platform == "win32"


def pe_subsystem_version(path):
    """Parse (Major, Minor) SubsystemVersion from a PE32/PE32+ file."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return None
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        return None
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    # PE32: 0x10b, PE32+: 0x20b; SubsystemVersion at opt+48 both layouts
    if magic not in (0x10B, 0x20B):
        return None
    major, minor = struct.unpack_from("<HH", data, opt_off + 48)
    return major, minor


def check(cond, results, label):
    results.append((bool(cond), label))
    log(("PASS " if cond else "FAIL ") + label)
    return bool(cond)


def main():
    results = []
    log("T10 build+test+selftests — " + time.strftime("%Y-%m-%d %H:%M:%S"))
    log("repo root: " + ROOT)

    # ---- 0. toolchain ------------------------------------------------
    log("\n=== toolchain ===")
    for cmd in ([sys.executable, "--version"], ["cmake", "--version"],
                ["ninja", "--version"], ["gcc", "--version"]):
        run(cmd)

    # ---- 1. configure + build ----------------------------------------
    log("\n=== build ===")
    have_ffmpeg = os.path.isfile(
        os.path.join(FFMPEG_PREFIX, "include", "libavcodec", "avcodec.h"))
    log("ffmpeg dev prefix: %s (%s)" %
        (FFMPEG_PREFIX, "FOUND" if have_ffmpeg else "not found — stub build"))
    cfg_cmd = ["cmake", "--fresh", "-G", "Ninja", "-B", "build",
               "-DCMAKE_BUILD_TYPE=Release"]
    if have_ffmpeg:
        cfg_cmd.append("-DKN_FFMPEG_PREFIX=" + FFMPEG_PREFIX)
    rc, out, _ = run(cfg_cmd)
    check(rc == 0, results, "cmake configure")
    rc, out, _ = run(["cmake", "--build", "build"])
    check(rc == 0, results, "ninja build")
    warns = [ln for ln in out.splitlines() if "warning:" in ln]
    check(not warns, results, "zero compiler warnings (%d seen)" % len(warns))

    # ---- 1b. FFmpeg runtime layout ---------------------------------------
    # decode.synthetic uses the clip EMBEDDED in the binary (hermetic — the
    # target FFmpeg build has no encoder and files can go missing), so there
    # is nothing to generate here. With a static FFmpeg (the required build,
    # see CMake guard) no DLLs are needed either; report what we see.
    if have_ffmpeg:
        dlls = glob.glob(os.path.join(BUILD, "av*.dll")) + \
            glob.glob(os.path.join(BUILD, "sw*.dll"))
        if dlls:
            log("FFmpeg DLLs next to knvstd.exe: %d (shared build)" % len(dlls))
        else:
            log("no FFmpeg DLLs next to knvstd.exe (static build — expected)")

    # ---- 2. unit tests ------------------------------------------------
    log("\n=== unit tests (on target) ===")
    suites = ["test_knlog", "test_knring", "test_knfec", "test_knsrtp",
              "test_kndiag", "test_knselftest"]
    for t in suites:
        rc, out, _ = run([exe(t)])
        ok = rc == 0 and "PASS" in out
        check(ok, results, "%s -> PASS" % t)

    # ---- 3. knvstd self-tests (REAL Win32) ----------------------------
    log("\n=== knvstd self-tests ===")
    rc, out, _ = run([exe("knvstd"), "--list-selftests"])
    names = []
    try:
        names = json.loads(out.strip().splitlines()[-1])["selftests"]
    except Exception as exc:                       # noqa: BLE001
        log("  cannot parse list: %s" % exc)
    check(rc == 0 and len(names) == 12, results,
          "registry lists 12 self-tests")

    rc, out, _ = run([exe("knvstd"), "--selftest=all", "--log-dir=" + LOGDIR])
    st = None
    try:
        st = json.loads(out.strip().splitlines()[-1])
    except Exception as exc:                       # noqa: BLE001
        log("  cannot parse selftest json: %s" % exc)
    check(rc == 0 and st is not None, results, "--selftest=all exit 0 + JSON")

    by_name = {}
    if st:
        for r in st.get("results", []):
            by_name[r["name"]] = r
        log("\n--- self-test verdicts ---")
        for r in st.get("results", []):
            log("  %-17s %-8s %4dms  checks=%-4d %s" %
                (r["name"], r["status"], r["ms"], r["checks"],
                 r["reason"][:80]))
        # offline vectors MUST pass on the target binary too
        for nm in ("fec.vectors", "srtp.vectors"):
            r = by_name.get(nm)
            check(r and r["status"] == "ok" and r["failures"] == 0, results,
                  "%s ok in the target binary" % nm)
        # Win32-only tests must have REALLY run (not skipped) on the target;
        # on a POSIX host dry-run they are expected to report skipped
        for nm in ("clock.resolution", "console.host"):
            r = by_name.get(nm)
            if IS_WINDOWS:
                check(r and r["status"] != "skipped", results,
                      "%s executed on Win32 (status=%s)" %
                      (nm, r["status"] if r else "absent"))
            else:
                check(r and r["status"] == "skipped", results,
                      "%s skipped on posix host dry-run" % nm)
        # console.host is the D1 gate: it must not fail under ConEmu.
        # (If run outside ConEmu this FAILS by design — that is the point.)
        if IS_WINDOWS:
            r = by_name.get("console.host")
            if r:
                check(r["status"] in ("ok", "degraded"), results,
                      "console.host not failed (D1): %s" % r["status"])
        # phase B/C stubs report skipped honestly (7 permanent stubs;
        # decode.synthetic joins them only when the build lacks FFmpeg —
        # its clip is embedded, so with FFmpeg it always runs)
        skipped = [nm for nm, r in by_name.items() if r["status"] == "skipped"]
        min_skipped = 7 if have_ffmpeg else 8
        check(len(skipped) >= min_skipped, results,
              "phase B/C stubs honestly skipped (%d, min %d)" %
              (len(skipped), min_skipped))
        # decode.synthetic must REALLY run when we built with FFmpeg
        r = by_name.get("decode.synthetic")
        if have_ffmpeg:
            check(r and r["status"] != "skipped", results,
                  "decode.synthetic executed (status=%s)" %
                  (r["status"] if r else "absent"))
            if r:
                log("  decode.synthetic reason: %s" % r["reason"])
        elif r:
            check(r["status"] == "skipped", results,
                  "decode.synthetic honestly skipped without ffmpeg")

    # ---- 4. diagnostic bundle -----------------------------------------
    log("\n=== diagnostic bundle ===")
    rc, out, _ = run([exe("knvstd"), "--diagnostics", "--log-dir=" + LOGDIR])
    bundle = None
    try:
        bundle = json.loads(out.strip().splitlines()[-1])["bundle"]
        # knvstd echoes the path relative to ITS cwd (ROOT); resolve for checks
        if bundle and not os.path.isabs(bundle):
            bundle = os.path.join(ROOT, bundle)
    except Exception as exc:                       # noqa: BLE001
        log("  cannot parse diagnostics json: %s" % exc)
    check(rc == 0 and bundle and os.path.isfile(bundle), results,
          "bundle written: %s" % bundle)

    if bundle and os.path.isfile(bundle):
        zf = zipfile.ZipFile(bundle)
        bad = zf.testzip()
        names_z = zf.namelist()
        check(bad is None, results, "zip CRCs valid (testzip)")
        check(names_z and names_z[0] == "summary.json", results,
              "summary.json is the first entry")
        for req in ("build.json", "state.json", "selftest.json"):
            check(req in names_z, results, "bundle contains " + req)
        check(any(n.startswith("logs/") for n in names_z), results,
              "bundle contains logs/")
        try:
            bj = json.loads(zf.read("build.json").decode("utf-8"))
            if IS_WINDOWS:
                v = bj.get("win32_winnt_effective", "")
                try:
                    winnt_ok = int(str(v), 16) == 0x601
                except ValueError:
                    winnt_ok = False
                check(winnt_ok, results,
                      "build.json: effective _WIN32_WINNT == 0x0601 (got %r)" % v)
            log("  build.json: git=%s dirty=%s arch=%s compiler=%s" %
                (bj.get("git_rev"), bj.get("git_dirty"), bj.get("arch"),
                 str(bj.get("compiler"))[:40]))
            log("  build.json flags: %s" % bj.get("flags"))
            log("  build.json extras: %s" % bj.get("extras"))
            if have_ffmpeg:
                check("ffmpeg=" in str(bj.get("extras", "")) and
                      "disabled" not in str(bj.get("extras", "")), results,
                      "build.json extras record the FFmpeg prefix")
        except Exception as exc:                   # noqa: BLE001
            check(False, results, "build.json parse: %s" % exc)
        try:
            sj = json.loads(zf.read("summary.json").decode("utf-8"))
            check(sj.get("trigger", {}).get("op") == "manual", results,
                  "summary.json trigger recorded")
            check(sj.get("over_cap") is False, results, "summary: within cap")
        except Exception as exc:                   # noqa: BLE001
            check(False, results, "summary.json parse: %s" % exc)
        size = os.path.getsize(bundle)
        check(size <= 8 * 1024 * 1024, results,
              "bundle size %d B <= 8 MiB" % size)
        zf.close()

    # ---- 5. jsonl validation ------------------------------------------
    log("\n=== jsonl validation ===")
    rc, out, _ = run([sys.executable, os.path.join(ROOT, "tools", "check_jsonl.py"),
                      LOGDIR])
    check(rc == 0, results, "check_jsonl over %s" % LOGDIR)

    # ---- 6. PE subsystem version ---------------------------------------
    log("\n=== PE check ===")
    if IS_WINDOWS:
        if os.path.isfile(exe("knvstd")):
            v = pe_subsystem_version(exe("knvstd"))
            check(v == (6, 1), results,
                  "knvstd.exe SubsystemVersion == 6.01 (got %s)" % (v,))
        else:
            check(False, results, "knvstd.exe exists for the PE check")
    else:
        check(os.path.isfile(exe("knvstd")), results,
              "knvstd binary exists (PE check is Windows-only)")

    # ---- verdict --------------------------------------------------------
    failed = [label for ok, label in results if not ok]
    log("\n" + "=" * 60)
    if failed:
        log("T10 VERDICT: FAIL (%d/%d checks failed)" % (len(failed), len(results)))
        for f in failed:
            log("  FAILED: " + f)
    else:
        log("T10 VERDICT: ALL PASS (%d checks)" % len(results))
    log("bundle: %s" % bundle)
    log("logs:   %s" % os.path.join(ROOT, LOGDIR))

    with open(WYNIK, "w", encoding="utf-8") as f:
        f.write("\n".join(OUT) + "\n")
    with open(SKROT, "w", encoding="utf-8") as f:
        f.write("T10 skrot — %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        f.write("VERDICT: %s\n" % ("ALL PASS" if not failed else
                                    "FAIL (%d)" % len(failed)))
        f.write("checks: %d total, %d failed\n" % (len(results), len(failed)))
        for ok, label in results:
            f.write("%s %s\n" % ("PASS" if ok else "FAIL", label))
        if st:
            f.write("\nselftests (status/ms/checks):\n")
            for r in st.get("results", []):
                f.write("  %-17s %-8s %4dms checks=%d fail=%d\n" %
                        (r["name"], r["status"], r["ms"], r["checks"],
                         r["failures"]))
                f.write("      reason: %s\n" % r["reason"][:160])
        f.write("\nbundle: %s\n" % bundle)
        if bundle and os.path.isfile(bundle):
            f.write("bundle bytes: %d\n" % os.path.getsize(bundle))

    print("\nwritten: %s" % WYNIK)
    print("written: %s" % SKROT)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
