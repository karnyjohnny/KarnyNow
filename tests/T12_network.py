#!/usr/bin/env python3
"""T12_network.py — build knvstd and run only phase-C network probes.

Run on the Win7 target from cmd/ConEmu:
    set KN_TLS_BACKEND=mbedtls
    python tests\\T12_network.py

T12 does not run unit suites, D3D, decoder, console, or clock tests. With
Schannel selected it reports TLS as a finding; with mbedTLS selected TLS
and chain verification are acceptance gates.
"""
import json
import os
import sys
import time
import zipfile

import T10_build_and_test as t10

ROOT = t10.ROOT
BUILD = os.path.join(ROOT, "build-network")
LOGDIR = "t12logs"
WYNIK = os.path.join(ROOT, "T12_wynik.txt")
SKROT = os.path.join(ROOT, "T12_skrot.txt")
TLS_BACKEND = os.environ.get("KN_TLS_BACKEND", "mbedtls").strip().lower()
IS_WINDOWS = sys.platform == "win32"
OUT = []
RESULTS = []
t10.OUT = OUT
t10.BUILD = BUILD
t10.TLS_BACKEND = TLS_BACKEND


def log(message=""):
    t10.log(message)


def check(condition, label):
    return t10.check(condition, RESULTS, label)


def parse_last_json(text):
    try:
        return json.loads(text.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return None


def main():
    log("T12 network-only — " + time.strftime("%Y-%m-%d %H:%M:%S"))
    log("repo root: " + ROOT)
    log("TLS backend: " + TLS_BACKEND)
    log("scope: DNS, TCP/443, TLS handshake, certificate chain")

    dependencies_ok = t10.ensure_vendor_files(RESULTS)
    backend_ok = TLS_BACKEND in ("schannel", "mbedtls")
    check(backend_ok, "TLS backend is supported")
    if TLS_BACKEND == "mbedtls":
        backend_ok = t10.ensure_mbedtls_source(RESULTS) and backend_ok
    dependencies_ok = dependencies_ok and backend_ok

    generator = "Ninja" if IS_WINDOWS else "Unix Makefiles"
    configure = ["cmake", "--fresh", "-G", generator, "-B", BUILD,
                 "-DCMAKE_BUILD_TYPE=Release",
                 "-DKN_TLS_BACKEND=" + TLS_BACKEND,
                 "-DKN_FFMPEG_PREFIX="]

    if dependencies_ok:
        rc, _, _ = t10.run(configure)
    else:
        rc = -1
        log("required source dependencies are unavailable; configure skipped")
    configured = check(rc == 0, "CMake configure")

    if configured:
        rc, output, _ = t10.run(["cmake", "--build", BUILD,
                                 "--target", "knvstd"])
    else:
        rc, output = -1, "Skipped because configure failed."
    built = check(rc == 0, "build knvstd only")
    warnings = [line for line in output.splitlines() if "warning:" in line]
    if not built:
        log("SKIP compiler warning check because knvstd build failed")
    elif IS_WINDOWS:
        check(not warnings, "zero compiler warnings (%d seen)" % len(warnings))
    else:
        log("POSIX compiler warnings (informational): %d" % len(warnings))
    knvstd = os.path.join(BUILD, "knvstd.exe" if IS_WINDOWS else "knvstd")
    check(os.path.isfile(knvstd), "knvstd exists in network build")

    parsed = {}
    if built and os.path.isfile(knvstd):
        for name in ("net.dns", "net.tcp443", "net.tls", "tls.chain"):
            rc, output, _ = t10.run([
                knvstd, "--selftest=" + name, "--log-dir=" + LOGDIR])
            document = parse_last_json(output)
            result = None
            if document:
                rows = document.get("results", [])
                if len(rows) == 1 and rows[0].get("name") == name:
                    result = rows[0]
            parsed[name] = result
            check(result is not None, "%s produced one JSON result" % name)
            if result is None:
                continue
            if not IS_WINDOWS:
                check(result.get("status") == "skipped",
                      "%s skipped honestly on POSIX" % name)
            else:
                check(result.get("status") != "skipped",
                    "%s executed (status=%s)" % (name, result.get("status")))
                if name in ("net.dns", "net.tcp443"):
                  check(result.get("status") in ("ok", "degraded"),
                      "%s passed network gate" % name)
                elif TLS_BACKEND == "mbedtls":
                  check(result.get("status") == "ok",
                      "%s passed mbedTLS acceptance gate" % name)
                else:
                  log("TLS finding (%s): %s" %
                    (name, result.get("reason", "")))

        tls = parsed.get("net.tls")
        if tls and TLS_BACKEND == "mbedtls" and IS_WINDOWS:
            evidence = tls.get("evidence", {})
            hosts = evidence.get("hosts", []) if isinstance(evidence, dict) else []
            check(len(hosts) == 3 and all(
                host.get("rc") == 0 and host.get("sec") == "0x00000000"
                for host in hosts),
                "mbedTLS verified NVIDIA endpoints and Cloudflare")
        elif tls and TLS_BACKEND == "schannel" and IS_WINDOWS:
            t10.net_tls_finding(tls.get("evidence"))

        chain = parsed.get("tls.chain")
        if chain and TLS_BACKEND == "mbedtls" and IS_WINDOWS:
            evidence = chain.get("evidence", {})
            check(evidence.get("depth", 0) >= 1 and
                  evidence.get("trust") == "0x00000000" and
                  evidence.get("policy") == "0x00000000",
                  "mbedTLS chain built and verified")

        rc, output, _ = t10.run([
            knvstd, "--diagnostics", "--log-dir=" + LOGDIR])
        diagnostics = parse_last_json(output)
        bundle = diagnostics.get("bundle") if diagnostics else None
        if bundle and not os.path.isabs(bundle):
            bundle = os.path.join(ROOT, bundle)
        check(rc == 0 and bundle and os.path.isfile(bundle),
              "network diagnostic bundle written")
        if bundle and os.path.isfile(bundle):
            with zipfile.ZipFile(bundle) as archive:
                build_info = json.loads(archive.read("build.json").decode("utf-8"))
            check("tls=" + TLS_BACKEND in str(build_info.get("extras", "")),
                  "bundle records TLS backend %s" % TLS_BACKEND)

    if os.path.isdir(os.path.join(ROOT, LOGDIR)):
        rc, _, _ = t10.run([sys.executable,
                            os.path.join(ROOT, "tools", "check_jsonl.py"), LOGDIR])
        check(rc == 0, "check_jsonl over %s" % LOGDIR)

    failed = [label for ok, label in RESULTS if not ok]
    dns_tcp_ok = all(parsed.get(name) and
                     parsed[name].get("status") in ("ok", "degraded")
                     for name in ("net.dns", "net.tcp443"))
    tls_ok = all(parsed.get(name) and parsed[name].get("status") == "ok"
                 for name in ("net.tls", "tls.chain"))
    if not IS_WINDOWS:
        verdict = "DRY-RUN ONLY (POSIX; network probes were skipped)"
    elif failed:
        verdict = "FAIL (%d checks failed)" % len(failed)
    elif TLS_BACKEND == "mbedtls" and tls_ok:
        verdict = "ALL PASS"
    elif TLS_BACKEND == "schannel" and dns_tcp_ok and not tls_ok:
        verdict = "NETWORK PASS; SCHANNEL TLS FINDING"
    else:
        verdict = "NETWORK PASS"

    log("\nT12 VERDICT: " + verdict)
    with open(WYNIK, "w", encoding="utf-8") as result_file:
        result_file.write("\n".join(OUT) + "\n")
    with open(SKROT, "w", encoding="utf-8") as summary_file:
        summary_file.write("T12 network-only — %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        summary_file.write("backend: %s\n" % TLS_BACKEND)
        summary_file.write("verdict: %s\n" % verdict)
        for name in ("net.dns", "net.tcp443", "net.tls", "tls.chain"):
            result = parsed.get(name)
            if result:
                summary_file.write("%s: %s — %s\n" %
                                   (name, result.get("status"),
                                    result.get("reason", "")))
        summary_file.write("checks: %d total, %d failed\n" %
                           (len(RESULTS), len(failed)))
        for ok, label in RESULTS:
            summary_file.write("%s %s\n" % ("PASS" if ok else "FAIL", label))
    log("written: " + WYNIK)
    log("written: " + SKROT)
    return 1 if failed and IS_WINDOWS else 0


if __name__ == "__main__":
    sys.exit(main())