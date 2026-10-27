#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""T8_python_deps.py — R15 probe: which Python dependencies are installable
on THIS machine (Python 3.8, Windows 7, pip only)?

What it does (read-only + downloads into .\\t8_wheels, no installs):
  1. Records python/pip/ssl versions (evidence, per BOOTSTRAP §2).
  2. For each package the TUI will need, runs
         pip download <pkg> --no-deps -d t8_wheels
     pip resolves the newest version COMPATIBLE WITH THIS interpreter, so the
     downloaded wheel filename tells us exactly which version to pin for cp38.
     Downloaded wheels are kept — they seed the local wheel directory (R15).
  3. Writes T8_wynik.txt (full transcript) and T8_skrot.txt (small summary —
     SEND THIS FILE BACK; the transcript only if something fails).

Nothing is installed and no project code runs. Network access goes only to
PyPI (pypi.org / files.pythonhosted.org). If TLS fails, the error is recorded
verbatim — we diagnose certificate problems, we never work around them.

Run:
    cd /d "%USERPROFILE%\\Desktop\\NewKarnyNow\\tests"
    python T8_python_deps.py
"""
import os
import ssl
import subprocess
import sys
import time

PACKAGES = [
    # TUI stack
    "textual",
    "rich",
    "pygments",
    "markdown-it-py",
    "mdurl",
    "platformdirs",
    "typing-extensions",
    # HTTP stack (login/catalog/CloudMatch; user's stdlib code stays as-is,
    # this is for the re-implementation per BOOTSTRAP §1)
    "httpx",
    "httpcore",
    "h11",
    "anyio",
    "sniffio",
    "idna",
    "certifi",
]

HERE = os.path.dirname(os.path.abspath(__file__))
WHEEL_DIR = os.path.join(HERE, "t8_wheels")
FULL = os.path.join(HERE, "T8_wynik.txt")
SKROT = os.path.join(HERE, "T8_skrot.txt")
PER_PKG_TIMEOUT = 180


def sh(args, timeout=PER_PKG_TIMEOUT):
    """Run a command, return (rc, stdout+stderr). Never raises."""
    try:
        p = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True, cwd=HERE)
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out or ""
    except Exception as exc:  # noqa: BLE001 - record everything, crash nothing
        return -1, "EXCEPTION %s: %s" % (type(exc).__name__, exc)


def main():
    lines = []          # full transcript
    skrot = []          # small table
    def log(s=""):
        print(s)
        lines.append(s)

    log("T8 python deps probe — %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    log("python     : %s" % sys.version.replace("\n", " "))
    log("executable : %s" % sys.executable)
    log("platform   : %s" % sys.platform)
    log("ssl        : %s" % ssl.OPENSSL_VERSION)
    rc, out = sh([sys.executable, "-m", "pip", "--version"])
    log("pip        : rc=%d %s" % (rc, out.strip().splitlines()[0] if out.strip() else out))
    log("")

    if not os.path.isdir(WHEEL_DIR):
        os.makedirs(WHEEL_DIR)
    before = set(os.listdir(WHEEL_DIR))

    skrot.append("pkg                     resolved_wheel                                    rc")
    for pkg in PACKAGES:
        log("=== pip download %s --no-deps ===" % pkg)
        rc, out = sh([sys.executable, "-m", "pip", "download", pkg,
                      "--no-deps", "-d", WHEEL_DIR])
        log(out.rstrip())
        log("rc=%d" % rc)
        log("")
        new = sorted(set(os.listdir(WHEEL_DIR)) - before)
        before |= set(new)
        wheel = new[-1] if new else ("ERROR(see T8_wynik.txt)")
        skrot.append("%-23s %-43s %d" % (pkg, wheel[:43], rc))

    log("=== environment evidence ===")
    for var in ("ConEmuPID", "ConEmuDir", "HTTP_PROXY", "HTTPS_PROXY", "PIP_INDEX_URL"):
        log("%s = %r" % (var, os.environ.get(var)))
    rc, out = sh([sys.executable, "-m", "pip", "config", "list"])
    log("pip config list rc=%d:\n%s" % (rc, out))

    skrot.append("")
    skrot.append("wheels kept in: %s" % WHEEL_DIR)
    skrot.append("python=%s ssl=%s" % (sys.version.split()[0], ssl.OPENSSL_VERSION))
    skrot.append("NOTE: any rc != 0 above = that package cannot be fetched on this machine")

    with open(FULL, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(SKROT, "w") as f:
        f.write("\n".join(skrot) + "\n")
    print("")
    print("Done. Send back: T8_skrot.txt  (and T8_wynik.txt if any rc != 0)")


if __name__ == "__main__":
    main()
