#!/usr/bin/env python3
"""T10_build_and_test.py — build + unit tests + SELF-TESTS + diagnostic bundle
verification on the TARGET machine (Windows 7, w64devkit toolchain).

Successor to T9: same build+test gate, extended with the new deliverables
(BOOTSTRAP §7.8/§7.9):
  1. cmake configure + ninja build (zero warnings expected)
  2. eight unit-test binaries: knlog knring knfec knsrtp kndiag knselftest
     kngs knaudio
  3. knvstd --list-selftests / --selftest=all  (REAL Win32 execution of
     clock.resolution, console.host, d3d.formats, d3d.present and
     decode.synthetic — the sandbox can only compile them). d3d.present
     ALSO runs standalone first: R25 milestone 0 in its own process.
     Gating policy (round XVI, encoded in the binary): only GATED tests
     drive the exit code / T10 verdict; net.tls + tls.chain are REPORTED
     findings — rendered as a stage-2 planning banner, never a red T10.
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
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")
LOGDIR = "t10logs"                      # relative to CWD when knvstd runs
WYNIK = os.path.join(ROOT, "T10_wynik.txt")
SKROT = os.path.join(ROOT, "T10_skrot.txt")

# FFmpeg dev tree (user's own w64devkit build; MUST be --enable-static, the
# F1/D8 -static link ignores .dll.a — CMake fails with the recipe if missing).
# Candidates are probed in order; override with KN_FFMPEG_PREFIX if it moves.
def _pick_ffmpeg_prefix():
    env = os.environ.get("KN_FFMPEG_PREFIX")
    candidates = [p for p in [
        env,
        r"C:\Users\KarnyJohnny\Desktop\ffmpeg\ffmpeg-win7-dev",           # current layout
        r"C:\Users\KarnyJohnny\Desktop\ffmpeg\ffmpeg-win7-dev\usr\local", # older layout
    ] if p]
    for c in candidates:
        if os.path.isfile(os.path.join(c, "include", "libavcodec", "avcodec.h")):
            return c
    return candidates[0] if candidates else ""


FFMPEG_PREFIX = _pick_ffmpeg_prefix()
TLS_BACKEND = os.environ.get("KN_TLS_BACKEND", "schannel").strip().lower()

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


VENDOR_FILES = [
    ("third_party/tiny-aes-c/aes.c",
     "https://raw.githubusercontent.com/kokke/tiny-AES-c/"
     "23856752fbd139da0b8ca6e471a13d5bcc99a08d/aes.c",
     "f7f78b44654efd3542b6886923dd05194286807ebcfeb078ea05d21fe901ee9e"),
    ("third_party/tiny-aes-c/aes.h",
     "https://raw.githubusercontent.com/kokke/tiny-AES-c/"
     "23856752fbd139da0b8ca6e471a13d5bcc99a08d/aes.h",
     "9f74a4de3bd11621ff6e8fb9b352b060f699e6c1c67f81a22d104aacad61a23b"),
    ("third_party/crypto-algorithms-sha1/sha1.c",
     "https://raw.githubusercontent.com/B-Con/crypto-algorithms/"
     "cfbde48414baacf51fc7c74f275190881f037d32/sha1.c",
     "7e5965e83bcafc361e31aff99136ba5ebae338448d4d33496e11e071ec437029"),
    ("third_party/crypto-algorithms-sha1/sha1.h",
     "https://raw.githubusercontent.com/B-Con/crypto-algorithms/"
     "cfbde48414baacf51fc7c74f275190881f037d32/sha1.h",
     "b76cc80cfc7a760150f2d0047ad39673d472d09900115bf8361b4e30f398f5b8"),
]


def ensure_vendor_files(results):
    """Fetch the small, pinned source files omitted from git by .gitignore."""
    log("\n=== required source dependencies ===")
    ok = True
    for relpath, url, expected_hash in VENDOR_FILES:
        path = os.path.join(ROOT, relpath)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                actual_hash = hashlib.sha256(f.read()).hexdigest()
            if actual_hash != expected_hash:
                check(False, results, "%s has expected SHA-256" % relpath)
                log("  existing file differs; leaving it untouched")
                ok = False
            else:
                check(True, results, "%s already present and verified" % relpath)
            continue

        try:
            with urllib.request.urlopen(url, timeout=30) as response:
                content = response.read()
            actual_hash = hashlib.sha256(content).hexdigest()
            if actual_hash != expected_hash:
                raise ValueError("SHA-256 mismatch: %s" % actual_hash)
            parent = os.path.dirname(path)
            if not os.path.isdir(parent):
                os.makedirs(parent)
            with open(path, "wb") as f:
                f.write(content)
            check(True, results, "%s downloaded and SHA-256 verified" % relpath)
        except (OSError, urllib.error.URLError, ValueError) as exc:
            check(False, results, "%s available (%s)" % (relpath, exc))
            ok = False
    return ok


def _extract_pinned_tarball(url, expected_hash, destination):
    with urllib.request.urlopen(url, timeout=60) as response:
        archive_bytes = response.read()
    actual_hash = hashlib.sha256(archive_bytes).hexdigest()
    if actual_hash != expected_hash:
        raise ValueError("archive SHA-256 mismatch: %s" % actual_hash)

    if not os.path.isdir(destination):
        os.makedirs(destination)
    with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:gz") as archive:
        for member in archive.getmembers():
            parts = [part for part in member.name.replace("\\", "/").split("/")
                     if part]
            if not parts:
                continue
            if parts[0] in (".", ".."):
                raise ValueError("unsafe archive member: %s" % member.name)
            relative = parts[1:]
            if not relative:
                continue
            if any(part in ("", ".", "..") for part in relative):
                raise ValueError("unsafe archive member: %s" % member.name)
            path = os.path.join(destination, *relative)
            if member.isdir():
                if not os.path.isdir(path):
                    os.makedirs(path)
            elif member.isfile():
                parent = os.path.dirname(path)
                if not os.path.isdir(parent):
                    os.makedirs(parent)
                source = archive.extractfile(member)
                if source is None:
                    raise ValueError("cannot read archive member: %s" % member.name)
                with source, open(path, "wb") as target:
                    shutil.copyfileobj(source, target)
            else:
                raise ValueError("unsupported archive member: %s" % member.name)


def ensure_mbedtls_source(results):
    """Fetch pinned mbedTLS source only when the selected backend needs it."""
    source = os.path.join(ROOT, "third_party", "mbedtls")
    cmake_file = os.path.join(source, "CMakeLists.txt")
    framework_cmake = os.path.join(source, "framework", "CMakeLists.txt")
    if os.path.isfile(cmake_file) and os.path.isfile(framework_cmake):
        with open(cmake_file, "r", encoding="utf-8") as f:
            version_ok = "VERSION 3.6.0" in f.read()
        check(version_ok, results, "mbedTLS source version is 3.6.0")
        return version_ok

    vendor_root = os.path.join(ROOT, "third_party")
    stage = tempfile.mkdtemp(prefix="mbedtls-stage-", dir=vendor_root)
    staged_source = os.path.join(stage, "mbedtls")
    try:
        _extract_pinned_tarball(
            "https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/"
            "2ca6c285a0dd3f33982dd57299012dacab1ff206",
            "e78c5a58392ab1abe72b664934a2e6727b938f8d81fbeed4c59548106a747560",
            staged_source)
        _extract_pinned_tarball(
            "https://codeload.github.com/Mbed-TLS/mbedtls-framework/tar.gz/"
            "750634d3a51eb9d61b59fd5d801546927c946588",
            "4845b5ae123c036cf9ec9e0ff0478a24b2be95450fc36a0fb80e2748518b424f",
            os.path.join(staged_source, "framework"))
        with open(os.path.join(staged_source, "CMakeLists.txt"),
                  "r", encoding="utf-8") as f:
            version_ok = "VERSION 3.6.0" in f.read()
        if not version_ok:
            raise ValueError("downloaded source is not mbedTLS 3.6.0")
        if os.path.exists(source):
            raise ValueError("incomplete third_party/mbedtls exists; left untouched")
        os.rename(staged_source, source)
        check(True, results, "pinned mbedTLS 3.6.0 source downloaded and verified")
        return True
    except (OSError, urllib.error.URLError, tarfile.TarError, ValueError) as exc:
        check(False, results, "mbedTLS source available (%s)" % exc)
        return False
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def net_tls_finding(ev):
    """Render the per-host measurements available in the selected TLS probe."""
    if not isinstance(ev, dict):
        log("  (net.tls evidence missing)")
        return
    log("")
    log("  *** NET.TLS FINDING - verification remained enabled ***")
    for h in ev.get("hosts", []):
        log("  %-34s rc=%s sec=%s protocol=%s cipher_bits=%s leaf=%s" %
            (str(h.get("host", "?"))[:34], h.get("rc", "?"),
             h.get("sec", "?"), h.get("proto", "?"),
             h.get("cipher_bits", "?"), h.get("leaf", "?")))
    log("  TLS/chain failures are findings; certificate verification was not bypassed.")
    log("  *** end of finding ***")


def main():
    results = []
    log("T10 build+test+selftests — " + time.strftime("%Y-%m-%d %H:%M:%S"))
    log("repo root: " + ROOT)

    dependencies_ok = ensure_vendor_files(results)
    tls_backend_ok = TLS_BACKEND in ("schannel", "mbedtls")
    check(tls_backend_ok, results,
          "TLS backend is supported (%s)" % TLS_BACKEND)
    if TLS_BACKEND == "mbedtls":
        tls_backend_ok = ensure_mbedtls_source(results) and tls_backend_ok
    dependencies_ok = dependencies_ok and tls_backend_ok

    # ---- 0. toolchain ------------------------------------------------
    log("\n=== toolchain ===")
    for cmd in ([sys.executable, "--version"], ["cmake", "--version"],
                ["ninja", "--version"], ["gcc", "--version"]):
        run(cmd)

    # ---- 1. configure + build ----------------------------------------
    log("\n=== build ===")
    log("TLS backend: %s" % TLS_BACKEND)
    have_ffmpeg = os.path.isfile(
        os.path.join(FFMPEG_PREFIX, "include", "libavcodec", "avcodec.h"))
    log("ffmpeg dev prefix: %s (%s)" %
        (FFMPEG_PREFIX, "FOUND" if have_ffmpeg else "not found — stub build"))
    cfg_cmd = ["cmake", "--fresh", "-G", "Ninja", "-B", "build",
               "-DCMAKE_BUILD_TYPE=Release",
               "-DKN_TLS_BACKEND=" + TLS_BACKEND]
    if have_ffmpeg:
        cfg_cmd.append("-DKN_FFMPEG_PREFIX=" + FFMPEG_PREFIX)
    if dependencies_ok:
        rc, out, _ = run(cfg_cmd)
    else:
        rc, out = -1, "Required source dependencies are unavailable."
        log(out)
    check(rc == 0, results, "cmake configure")
    configure_ok = rc == 0
    if rc == 0:
        rc, out, _ = run(["cmake", "--build", "build"])
    else:
        rc, out = -1, "Skipped because configure failed."
        log(out)
    check(rc == 0, results, "ninja build")
    warns = [ln for ln in out.splitlines() if "warning:" in ln]
    check(not warns, results, "zero compiler warnings (%d seen)" % len(warns))

    # Every unit test and the diagnostic/self-test runner must be present in
    # build/. Retry missing targets explicitly before any binary is launched.
    required_bins = ["test_knfec", "test_knring", "test_knaudio", "test_kngs",
                     "test_knselftest", "knvstd", "test_kndiag", "test_knlog",
                     "test_knsrtp"]
    missing = [name for name in required_bins if not os.path.isfile(exe(name))]
    if missing and configure_ok:
        log("\nmissing build outputs: %s" % ", ".join(name + ".exe" for name in missing))
        retry_rc, _, _ = run(["cmake", "--build", "build", "--target"] + missing)
        if retry_rc != 0:
            check(False, results, "rebuild missing executable targets")
    for name in required_bins:
        check(os.path.isfile(exe(name)), results,
              "%s%s exists in build" % (name, ".exe" if IS_WINDOWS else ""))

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
            # Informational confirmation, NOT a problem: D18 requires the
            # static FFmpeg build (--enable-static --disable-shared), so the
            # dev tree has no DLLs anywhere (bin/*.exe are the CLI tools,
            # lib/*.a are what knvstd.exe links). Nothing to deploy.
            log("no FFmpeg DLLs next to knvstd.exe — static build (D18): "
                "knvstd links lib/*.a directly, there is nothing to deploy "
                "(EXPECTED, not a warning)")

    # ---- 2. unit tests ------------------------------------------------
    log("\n=== unit tests (on target) ===")
    suites = ["test_knlog", "test_knring", "test_knfec", "test_knsrtp",
              "test_kndiag", "test_knselftest", "test_kngs", "test_knaudio"]
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

    # R25 milestone 0: d3d.present runs STANDALONE first — its own process,
    # so even a hard driver fault cannot take the rest of the run's evidence
    # with it (G0.2 §4). The binary guards the fill with a VEH + permanent
    # D7 degrade latch, so a caught AV arrives here as status=failed, not as
    # a dead process; rc != 0 without JSON means something harder happened.
    milestone = None
    if IS_WINDOWS:
        rc, out, _ = run([exe("knvstd"), "--selftest=d3d.present",
                          "--log-dir=" + LOGDIR])
        try:
            milestone = json.loads(out.strip().splitlines()[-1])["results"][0]
        except Exception as exc:                   # noqa: BLE001
            log("  cannot parse milestone-0 json: %s" % exc)
        if milestone is None:
            log("  milestone-0 produced no result; rc=%d (unsigned 0x%08x)" %
                (rc, rc & 0xFFFFFFFF))
        else:
            log("  milestone-0 reason: %s" % milestone["reason"])
        check(rc == 0 and milestone is not None and
              milestone["status"] in ("ok", "degraded"), results,
              "R25 milestone 0: d3d.present standalone (%s)" %
              (milestone["status"] if milestone else "no result, rc=%d" % rc))

    rc, out, _ = run([exe("knvstd"), "--selftest=all", "--log-dir=" + LOGDIR])
    st = None
    try:
        st = json.loads(out.strip().splitlines()[-1])
    except Exception as exc:                       # noqa: BLE001
        log("  cannot parse selftest json: %s" % exc)
    # Only gated tests drive the aggregate exit code; TLS failures remain
    # visible findings and never imply that verification should be bypassed.
    check(rc == 0 and st is not None, results,
          "--selftest=all exit 0 (gated policy) + JSON")

    by_name = {}
    if st:
        for r in st.get("results", []):
            by_name[r["name"]] = r
        log("\n--- self-test verdicts ---")
        for r in st.get("results", []):
            log("  %-17s %-8s %4dms  checks=%-4d %s" %
                (r["name"], r["status"], r["ms"], r["checks"],
                 r["reason"][:80]))
        # offline vectors MUST pass on the target binary too (gs.parse is
        # portable: its KAT battery runs everywhere, capture is optional)
        for nm in ("fec.vectors", "srtp.vectors", "gs.parse"):
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
        # d3d probes (phase B): executed + usable verdict on Win32. "failed"
        # here is a REAL finding (R25/D7), not a harness problem — it fails
        # T10 on purpose so stage 1 cannot start on a broken video path.
        for nm in ("d3d.formats", "d3d.present"):
            r = by_name.get(nm)
            if IS_WINDOWS:
                check(r and r["status"] != "skipped", results,
                      "%s executed on Win32 (status=%s)" %
                      (nm, r["status"] if r else "absent"))
                check(r and r["status"] in ("ok", "degraded"), results,
                      "%s verdict usable (D7): %s" %
                      (nm, r["status"] if r else "absent"))
                if r:
                    log("  %s reason: %s" % (nm, r["reason"]))
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
        # phase C net probes: REAL measurements on the target, skipped on a
        # POSIX dry-run. Gating policy: dns + tcp443 must be ok/degraded on
        # a networked machine (FAILED there = stage-2 blocker -> red T10 on
        # purpose); net.tls / tls.chain are REPORTED (their failure is a
        # Win7-TLS FINDING to analyse, not automatically a client bug).
        for nm in ("net.dns", "net.tcp443", "net.tls", "tls.chain"):
            r = by_name.get(nm)
            if IS_WINDOWS:
                check(r and r["status"] != "skipped", results,
                      "%s executed on Win32 (status=%s)" %
                      (nm, r["status"] if r else "absent"))
                if r:
                    log("  %s reason: %s" % (nm, r["reason"]))
                if nm in ("net.dns", "net.tcp443"):
                    check(r and r["status"] in ("ok", "degraded"), results,
                          "%s usable verdict: %s" %
                          (nm, r["status"] if r else "absent"))
            else:
                check(r and r["status"] == "skipped", results,
                      "%s skipped on posix host dry-run" % nm)
        # Round XVI gating sweep, straight from the JSON's "gated" flags:
        # NO gated test may be FAILED (skipped is honest on a posix dry-run
        # or without ffmpeg); reported tests may fail as a FINDING.
        gated_bad = [rr["name"] for rr in st.get("results", [])
                     if rr.get("gated", True) and rr["status"] == "failed"]
        check(not gated_bad, results,
              "every GATED self-test usable (failed: %s)" %
              (", ".join(gated_bad) if gated_bad else "none"))
        # net.tls must include the per-host measurements this probe actually
        # collects; do not require an unimplemented environment/config matrix.
        r = by_name.get("net.tls")
        if IS_WINDOWS and r:
            ev = r.get("evidence")
            hosts = ev.get("hosts", []) if isinstance(ev, dict) else []
            evidence_ok = (len(hosts) == 3 and all(
                isinstance(host, dict) and "rc" in host and "sec" in host
                for host in hosts))
            check(evidence_ok,
                  results,
                  "net.tls evidence carries per-host handshake results")
            if r["status"] in ("failed", "degraded"):
                net_tls_finding(ev)
        # the registry has NO stubs left (round XV); decode.synthetic joins
        # the skipped list only when the build lacks FFmpeg
        skipped = [nm for nm, r in by_name.items() if r["status"] == "skipped"]
        min_skipped = 0 if have_ffmpeg else 1
        max_skipped = 0 if (have_ffmpeg and IS_WINDOWS) else (
            4 if not IS_WINDOWS else 1)
        check(min_skipped <= len(skipped) <= (max_skipped if IS_WINDOWS else 9),
              results, "no stubs left: %d skipped (platform/ffmpeg only)" %
              len(skipped))
        # decode.synthetic must REALLY run when we built with FFmpeg — and
        # since the parser-flush fix (T10 round 4: 59/60) it must be OK:
        # 60/60 frames from the embedded clip.
        r = by_name.get("decode.synthetic")
        if have_ffmpeg:
            check(r and r["status"] != "skipped", results,
                  "decode.synthetic executed (status=%s)" %
                  (r["status"] if r else "absent"))
            check(r and r["status"] == "ok", results,
                  "decode.synthetic ok — 60/60 embedded frames (status=%s)" %
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
            check("tls=" + TLS_BACKEND in str(bj.get("extras", "")), results,
                "build.json records TLS backend %s" % TLS_BACKEND)
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
