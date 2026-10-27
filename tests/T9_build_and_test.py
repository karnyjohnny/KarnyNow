#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""T9_build_and_test.py — compile the whole C core with the PRODUCTION
toolchain (w64devkit GCC 16.2 via cmake+ninja) and run every unit test ON
the Windows 7 target machine.

Why this exists: the assistant's sandbox lost its compiler, and even when it
had one it was GCC 12 — the target's GCC 16 is the authoritative warning gate
(it caught -Wstring-compare that GCC 12 missed). One command, small report
back, exactly like the T# probes.

Run:
    cd /d "%USERPROFILE%\\Desktop\\NewKarnyNow\\tests"
    python T9_build_and_test.py

Produces (in tests\\):
    T9_wynik.txt  — full transcript (send if anything fails)
    T9_skrot.txt  — small summary (SEND THIS BACK)

What it does:
  1. records toolchain versions (cmake/ninja/gcc/python)
  2. cmake --fresh -G Ninja -B build -DCMAKE_BUILD_TYPE=Release  (repo root)
     — if cmake/ninja are missing, falls back to direct gcc builds with the
       canonical F1 flag set
  3. builds everything (all warnings captured verbatim)
  4. runs test_knlog.exe (KN_TEST_KEEP=1), test_knring.exe, test_knfec.exe
  5. validates the produced .jsonl logs with tools/check_jsonl.py
  6. verdict: ALL PASS or the exact list of failures

Installs nothing, touches no network, changes nothing outside build\\ and the
temporary logs/testlogs*\\ directories (removed at the end).
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                     # repo root (NewKarnyNow)
FULL = os.path.join(HERE, "T9_wynik.txt")
SKROT = os.path.join(HERE, "T9_skrot.txt")

F1_FLAGS = [
    "-std=c11", "-O2", "-Wall", "-Wextra",
    "-D_WIN32_WINNT=0x0601", "-DWINVER=0x0601", "-DNTDDI_VERSION=0x06010000",
    "-march=core2", "-msse3",
    "-static", "-static-libgcc",
    "-Wl,--major-subsystem-version,6.01", "-Wl,--minor-subsystem-version,01",
    "-Wl,--major-os-version,6.01", "-Wl,--minor-os-version,01",
]

SRC = ["streamer/src/knlog.c", "streamer/src/knring.c", "streamer/src/knfec.c",
       "streamer/src/kn_aes.c", "streamer/src/kn_sha1.c", "streamer/src/knsrtp.c",
       "third_party/tiny-aes-c/aes.c", "third_party/crypto-algorithms-sha1/sha1.c"]
TESTS = [
    ("test_knlog", "streamer/tests/test_knlog.c"),
    ("test_knring", "streamer/tests/test_knring.c"),
    ("test_knfec", "streamer/tests/test_knfec.c"),
    ("test_knsrtp", "streamer/tests/test_knsrtp.c"),
]

transcript = []
skrot = []
failures = []


def log(msg=""):
    print(msg)
    transcript.append(str(msg))


def run(args, cwd=ROOT, timeout=600, env=None):
    """Run a command, capture output, never raise."""
    log("$ " + " ".join(str(a) for a in args))
    t0 = time.time()
    try:
        p = subprocess.Popen(args, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, cwd=cwd,
                             env=env, universal_newlines=True)
        out, _ = p.communicate(timeout=timeout)
        rc = p.returncode
    except FileNotFoundError:
        return 127, "COMMAND NOT FOUND: %s" % args[0], time.time() - t0
    except Exception as exc:  # noqa: BLE001
        return -1, "EXCEPTION %s: %s" % (type(exc).__name__, exc), time.time() - t0
    out = out or ""
    log(out.rstrip())
    log("[rc=%d in %.1fs]" % (rc, time.time() - t0))
    log("")
    return rc, out, time.time() - t0


def warnings_in(text):
    return [l.strip() for l in text.splitlines() if "warning:" in l]


def cleanup_test_dirs():
    for d in ("logs/testlogs", "logs/testlogs2"):
        shutil.rmtree(os.path.join(ROOT, d), ignore_errors=True)
    for f in ("logs/testring_dump.txt",):
        try:
            os.remove(os.path.join(ROOT, f))
        except OSError:
            pass


def tool_versions():
    log("=== toolchain ===")
    for args in ([sys.executable, "--version"], ["cmake", "--version"],
                 ["ninja", "--version"], ["gcc", "--version"]):
        rc, out, _ = run(args, timeout=60)
        first = out.strip().splitlines()[0] if out.strip() else "(missing)"
        skrot.append("%-8s %s" % (os.path.basename(str(args[0]))[:8], first))
    log("")


def build_cmake():
    """Returns True if cmake+ninja path succeeded in configuring."""
    rc, out, _ = run(["cmake", "--fresh", "-G", "Ninja", "-B", "build",
                      "-DCMAKE_BUILD_TYPE=Release"], timeout=300)
    if rc != 0:
        return False
    rc, out, _ = run(["cmake", "--build", "build"], timeout=900)
    warn = warnings_in(out)
    skrot.append("build(cmake) rc=%d warnings=%d" % (rc, len(warn)))
    for w in warn[:20]:
        skrot.append("  WARN " + w)
    if rc != 0:
        failures.append("cmake build rc=%d" % rc)
        return False
    return True


def build_gcc_direct():
    skrot.append("build mode: DIRECT GCC (cmake/ninja unavailable)")
    ok = True
    if not os.path.isdir(os.path.join(ROOT, "build")):
        os.makedirs(os.path.join(ROOT, "build"))
    for name, tsrc in TESTS:
        exe = os.path.join(ROOT, "build", name + ".exe")   # absolute, see run_tests
        args = (["gcc"] + F1_FLAGS
                + ["-Istreamer/include", "-Ithird_party/tiny-aes-c",
                   "-Ithird_party/crypto-algorithms-sha1"]
                + SRC + [tsrc, "-o", exe])
        rc, out, _ = run(args, timeout=900)
        warn = warnings_in(out)
        skrot.append("build %-12s rc=%d warnings=%d" % (name, rc, len(warn)))
        for w in warn[:20]:
            skrot.append("  WARN " + w)
        if rc != 0:
            failures.append("gcc build %s rc=%d" % (name, rc))
            ok = False
    return ok


def run_tests(exe_prefix):
    env = dict(os.environ)
    env["KN_TEST_KEEP"] = "1"
    for name, _ in TESTS:
        exe_rel = os.path.join("build", exe_prefix + name + ".exe")
        exe = os.path.join(ROOT, exe_rel)
        # WINDOWS TRAP (cost one T9 round): subprocess's `cwd` does NOT apply
        # to executable-path resolution ("this directory is not considered
        # when determining the executable" — Python docs). A relative
        # "build\test_x.exe" was resolved against tests\, not the repo root,
        # giving COMMAND NOT FOUND while the file existed. Always absolute.
        if not os.path.isfile(exe):
            failures.append("missing binary " + exe_rel)
            skrot.append("%-12s MISSING" % name)
            continue
        rc, out, _ = run([exe], timeout=300, env=env)
        verdict = [l for l in out.splitlines() if "->" in l or "FAIL" in l]
        summary = verdict[-1] if verdict else out.strip().splitlines()[-1] if out.strip() else "(no output)"
        skrot.append("%-12s rc=%d  %s" % (name, rc, summary.strip()))
        for l in out.splitlines():
            if l.startswith("FAIL"):
                skrot.append("  " + l.strip())
        if rc != 0:
            failures.append("%s rc=%d" % (name, rc))


def validate_jsonl():
    rc, out, _ = run([sys.executable, os.path.join("tools", "check_jsonl.py"),
                      "logs/testlogs", "logs/testlogs2"], timeout=120)
    summary = out.strip().splitlines()[-1] if out.strip() else "(no output)"
    skrot.append("%-12s rc=%d  %s" % ("jsonl", rc, summary.strip()))
    if rc != 0:
        failures.append("check_jsonl rc=%d" % rc)


def main():
    log("T9 build+test — %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    log("repo root: %s" % ROOT)
    log("")
    skrot.append("T9 build+test skrot — %s" % time.strftime("%Y-%m-%d %H:%M:%S"))

    cleanup_test_dirs()
    tool_versions()

    have_cmake = shutil.which("cmake") and shutil.which("ninja")
    built = build_cmake() if have_cmake else False
    if built:
        run_tests("")
    else:
        if have_cmake:
            log("cmake path failed — trying direct gcc")
        if shutil.which("gcc"):
            if build_gcc_direct():
                run_tests("")
        else:
            failures.append("no compiler found (need w64devkit gcc on PATH)")
            skrot.append("NO COMPILER FOUND")

    if any(n == "test_knlog" for n, _ in TESTS):
        validate_jsonl()

    cleanup_test_dirs()

    verdict = "ALL PASS" if not failures else "FAIL: " + "; ".join(failures)
    skrot.append("")
    skrot.append("VERDICT: " + verdict)
    log("=== VERDICT: %s ===" % verdict)

    with open(FULL, "w", encoding="utf-8", errors="replace") as f:
        f.write("\n".join(transcript) + "\n")
    with open(SKROT, "w", encoding="utf-8", errors="replace") as f:
        f.write("\n".join(skrot) + "\n")
    print("")
    print("Done. Send back: T9_skrot.txt" +
          ("" if not failures else "  AND T9_wynik.txt (something failed)"))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
