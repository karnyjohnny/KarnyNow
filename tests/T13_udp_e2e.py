#!/usr/bin/env python3
"""T13_udp_e2e.py — the stage-2 SOCKET-LAYER gate (loopback only, no account).

Run on the Win7 target from cmd/ConEmu (after a T10 pass, whose build is
reused by default):
    python tests\\T13_udp_e2e.py
Options:
    --knvstd PATH   use an existing binary instead of building (sandbox)
    --build-dir D   cmake build dir to use/create (default: T10's build/)
    --quick         skip the two SRTP legs

What it proves (every leg is real UDP over 127.0.0.1 — the same binary,
pipeline and clock the cloud session will use; only the peer is local):
  1. capture leg    tools/send_rx_capture.py paces a synthetic Mjolnir
                    capture into `knvstd --capture`; the recorded file must
                    hold every datagram (counters + line count).
  2. vector leg     `--replay-capture` of the LIVE-RECORDED file verifies
                    against the reference manifest (R32's mechanism: a
                    real session becomes a permanent regression vector —
                    rehearsed offline here).
  3. decode legs    paced AND blast delivery into `knvstd --decode`:
                    60/60 AUs assembled, FEC repairs, zero queue
                    overflows, real FFmpeg decode when the build has it
                    (pipeline-only otherwise — honestly reported), and the
                    decode stdout verifies against the SAME manifest as
                    the replay leg (field-compatible summary contract).
  4. SRTP legs      cm128:10 everywhere; gcm256:8 when the binary's
                    rx.vectors evidence reports the mbedTLS backend
                    (srtp_backend=1) — skipped honestly otherwise.
  5. sock.loopback  the shipped binary's own UDP self-test (gated).

Verdict semantics: on Windows a failure fails T13; on POSIX every leg
except the optional gcm256 one really runs (real sockets), so a POSIX
FAIL is a real FAIL — not a dry run. Exit code mirrors the verdict.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import T10_build_and_test as t10   # noqa: E402  (log/check/run/ROOT idioms)

ROOT = t10.ROOT
WORK = os.path.join(ROOT, "logs", "t13work")
LOGDIR = "logs/t13"
WYNIK = os.path.join(ROOT, "logs", "T13_wynik.txt")
SKROT = os.path.join(ROOT, "logs", "T13_skrot.txt")
IS_WINDOWS = sys.platform == "win32"
SENDER = os.path.join(ROOT, "tools", "send_rx_capture.py")
MAKER = os.path.join(ROOT, "tools", "make_rx_capture.py")
OUT = []
RESULTS = []
t10.OUT = OUT

CAPTURE_SECONDS = 8
DECODE_SECONDS = 25
SRTP_KEY = "000102030405060708090a0b0c0d0e0f"
SRTP_KEY256 = SRTP_KEY * 2
SRTP_SALT = "000102030405060708090a0b0c0d"
EXPECT_FRAMES = 60          # synthetic.264 clip
EXPECT_RECORDS = 119        # fec25 + loss6 + reorder0.3 over that clip
EXPECT_FEC_REPAIRED = 19    # the reference receiver's verdict (seed 1)


def log(message=""):
    t10.log(message)


def check(condition, label):
    return t10.check(condition, RESULTS, label)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def parse_last_json(text):
    """Last JSON-object line of a combined stdout+stderr capture (skips
    trailing non-JSON noise — sanitizer diagnostics in instrumented runs)."""
    for line in reversed(text.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                continue
    return None


def parse_jsonl(path):
    """All JSON objects of a jsonl file (bad lines -> None entries)."""
    docs = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                docs.append(json.loads(line))
            except ValueError:
                docs.append(None)
    return docs


def count_unparseable(docs):
    return sum(1 for d in docs if d is None)


def find_first(docs, mode, event):
    for d in docs:
        if d and d.get("mode") == mode and d.get("event") == event:
            return d
    return None


def find_summary(docs, mode):
    for d in reversed(docs):
        if d and d.get("mode") == mode and "summary" in d:
            return d["summary"]
    return None


def start_listener(knvstd, args, stdout_path):
    """Popen a knvstd listener; stdout to a file; give the socket ~1 s to
    bind before the caller starts sending."""
    fh = open(stdout_path, "wb")
    proc = subprocess.Popen([knvstd] + args, stdout=fh,
                            stderr=subprocess.DEVNULL, cwd=ROOT)
    time.sleep(1.0)
    return proc, fh


def finish_listener(proc, fh, timeout):
    try:
        rc = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = -999
    fh.close()
    return rc


def send_capture(cap_hex, port, speed):
    rc, out, _ = t10.run([sys.executable, SENDER, "--in", cap_hex,
                          "--port", str(port), "--speed", str(speed)],
                         timeout=120)
    try:
        summary = json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        summary = None
    return rc, summary


def verify_against(manifest, jsonl_path):
    """make_rx_capture.py --verify manifest < jsonl (the cross-impl gate)."""
    with open(jsonl_path, "rb") as stdin_fh:
        proc = subprocess.run([sys.executable, MAKER, "--verify", manifest],
                              stdin=stdin_fh, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, cwd=ROOT, timeout=120)
    text = proc.stdout.decode("utf-8", "replace")
    log("  verify: " + text.strip())
    return proc.returncode == 0 and "VERIFY PASS" in text


def decode_produced(summary):
    """Frames the run produced: real decode when the build has FFmpeg,
    assembled AUs in a pipeline-only build (the binary's own semantics)."""
    if summary is None:
        return -1
    if summary.get("decoder") and summary["decoder"] != "pipeline-only":
        return summary.get("frames_decoded", -1)
    return summary.get("aus", -1)


def run_decode_leg(name, knvstd, cap_hex, manifest, speed, srtp_spec,
                   au_dir):
    """One sender->decode->verify leg. Returns True when every check passed."""
    log("\n--- decode leg: %s (speed %s%s) ---" %
        (name, speed, ", srtp " + srtp_spec.split(":")[0] if srtp_spec else ""))
    port = free_port()
    out_path = os.path.join(WORK, "decode_%s.jsonl" % name)
    args = ["--decode", "--decode-bind=127.0.0.1", "--decode-port=%d" % port,
            "--decode-seconds=%d" % DECODE_SECONDS,
            "--decode-expect-frames=%d" % EXPECT_FRAMES,
            "--decode-vps=1280", "--log-dir=" + LOGDIR]
    if au_dir:
        args.append("--decode-au-dir=" + au_dir)
    if srtp_spec:
        args.append("--decode-srtp=" + srtp_spec)
    proc, fh = start_listener(knvstd, args, out_path)
    src, summary = send_capture(cap_hex, port, speed)
    rc = finish_listener(proc, fh, DECODE_SECONDS + 60)
    ok = True
    ok &= check(src == 0 and summary and
                summary.get("sent") == EXPECT_RECORDS,
                "%s: sender delivered %d records" % (name, EXPECT_RECORDS))
    ok &= check(rc == 0, "%s: knvstd --decode exit 0 (got %d)" % (name, rc))
    docs = parse_jsonl(out_path)
    dbad = count_unparseable(docs)
    ok &= check(dbad == 0, "%s: every stdout line parses as JSON "
                           "(%d unparseable)" % (name, dbad))
    begin = find_first(docs, "decode", "begin")
    if begin:
        log("  %s: rcvbuf_actual=%s (R34 headroom evidence), decoder=%s" %
            (name, begin.get("rcvbuf_actual"), begin.get("decoder")))
    dsum = find_summary(docs, "decode")
    ok &= check(dsum is not None, "%s: decode summary present" % name)
    if dsum:
        produced = decode_produced(dsum)
        ok &= check(produced == EXPECT_FRAMES,
                    "%s: produced %d/%d (decoder: %s)" %
                    (name, produced, EXPECT_FRAMES, dsum.get("decoder")))
        ok &= check(dsum.get("frames_failed", -1) == 0,
                    "%s: zero decode failures" % name)
        ok &= check(dsum.get("q_overflows", -1) == 0,
                    "%s: zero queue overflows (capacity-2 held)" % name)
        ok &= check(dsum.get("recv_errors", -1) == 0,
                    "%s: zero socket errors" % name)
        ok &= check(dsum.get("fec_repaired", -1) == EXPECT_FEC_REPAIRED,
                    "%s: fec_repaired %s == reference %d" %
                    (name, dsum.get("fec_repaired"), EXPECT_FEC_REPAIRED))
        ok &= check(dsum.get("withheld", -1) == 0 and
                    dsum.get("gaps", -1) == 0,
                    "%s: no withheld frames, no gap recoveries" % name)
        ok &= check(verify_against(manifest, out_path),
                    "%s: stdout verifies against the reference manifest" %
                    name)
    else:
        for _ in range(8):
            check(False, "%s: (skipped check — no summary)" % name)
    return bool(ok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--knvstd", default=None,
                    help="existing knvstd binary (skips the build)")
    ap.add_argument("--build-dir", default=None)
    ap.add_argument("--quick", action="store_true",
                    help="skip the SRTP legs")
    args = ap.parse_args()

    log("T13 stage-2 UDP end-to-end — " + time.strftime("%Y-%m-%d %H:%M:%S"))
    log("repo root: " + ROOT)
    log("platform: " + ("Windows" if IS_WINDOWS else "POSIX (real sockets)"))

    # deterministic workspace (the T11 wipe lesson: accumulation lies)
    for d in (WORK, os.path.join(ROOT, LOGDIR)):
        if os.path.isdir(d):
            shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d, exist_ok=True)

    # --- binary: reuse (given or T10's) or build knvstd only -------------
    knvstd = args.knvstd
    if not knvstd:
        build = args.build_dir or os.path.join(ROOT, "build")
        candidate = os.path.join(build, "knvstd.exe" if IS_WINDOWS else "knvstd")
        if os.path.isfile(candidate):
            knvstd = candidate
            log("reusing existing build: " + knvstd)
    if not knvstd:
        build = args.build_dir or os.path.join(ROOT, "build-t13")
        deps = t10.ensure_vendor_files(RESULTS)
        backend = t10.TLS_BACKEND
        if backend == "mbedtls":
            deps = t10.ensure_mbedtls_source(RESULTS) and deps
        generator = "Ninja" if IS_WINDOWS else "Unix Makefiles"
        configure = ["cmake", "--fresh", "-G", generator, "-B", build,
                     "-DCMAKE_BUILD_TYPE=Release",
                     "-DKN_TLS_BACKEND=" + backend]
        if t10.FFMPEG_PREFIX:
            configure.append("-DKN_FFMPEG_PREFIX=" + t10.FFMPEG_PREFIX)
        rc = -1
        if deps:
            rc, _, _ = t10.run(configure, timeout=600)
        configured = check(rc == 0, "CMake configure (T13)")
        if configured:
            rc, output, _ = t10.run(["cmake", "--build", build,
                                     "--target", "knvstd"], timeout=900)
            warnings = [l for l in output.splitlines() if "warning:" in l]
            if IS_WINDOWS:
                check(not warnings,
                      "zero compiler warnings (%d seen)" % len(warnings))
        else:
            check(False, "build knvstd (configure failed)")
        candidate = os.path.join(build, "knvstd.exe" if IS_WINDOWS else "knvstd")
        if os.path.isfile(candidate):
            knvstd = candidate
    ok_bin = check(bool(knvstd) and os.path.isfile(knvstd or ""),
                   "knvstd binary available")

    if not ok_bin:
        verdict_fail("no knvstd binary — every leg skipped")
        return 1

    log("knvstd: " + knvstd)

    # --- capture material (deterministic: seed 1) -------------------------
    cap_hex = os.path.join(WORK, "cap.hex")
    cap_json = os.path.join(WORK, "cap.json")
    rc, out, _ = t10.run([sys.executable, MAKER, "--out", cap_hex,
                          "--manifest", cap_json, "--fec-pct", "25",
                          "--loss", "6", "--reorder", "0.3"], timeout=120)
    check(rc == 0 and os.path.isfile(cap_hex) and os.path.isfile(cap_json),
          "capture generated (fec25/loss6/reorder0.3, seed 1)")

    # --- backend capability (decides the gcm256 leg) ----------------------
    rc, out, _ = t10.run([knvstd, "--selftest=rx.vectors",
                          "--log-dir=" + LOGDIR], timeout=120)
    backend_full = False
    doc = parse_last_json(out)
    if doc:
        rows = doc.get("results", [])
        if rows:
            backend_full = bool(rows[0].get("evidence", {}).get("srtp_backend"))
    log("  rx.vectors srtp_backend: %d (gcm256 leg %s)" %
        (1 if backend_full else 0,
         "ENABLED" if backend_full else "skipped (bare SRTP backend)"))

    # --- leg 1: capture ----------------------------------------------------
    log("\n--- capture leg (paced sender -> knvstd --capture) ---")
    port = free_port()
    live_hex = os.path.join(WORK, "live.hex")
    cap_out = os.path.join(WORK, "capture.jsonl")
    proc, fh = start_listener(knvstd, [
        "--capture", "--capture-bind=127.0.0.1", "--capture-port=%d" % port,
        "--capture-out=" + live_hex, "--capture-seconds=%d" % CAPTURE_SECONDS,
        "--log-dir=" + LOGDIR], cap_out)
    src, ssum = send_capture(cap_hex, port, 1)
    rc = finish_listener(proc, fh, CAPTURE_SECONDS + 60)
    check(src == 0 and ssum and ssum.get("sent") == EXPECT_RECORDS,
          "capture leg: sender delivered %d records" % EXPECT_RECORDS)
    check(rc == 0, "capture leg: knvstd --capture exit 0 (got %d)" % rc)
    csum = None
    cdocs = parse_jsonl(cap_out)
    bad = count_unparseable(cdocs)
    check(bad == 0, "capture leg: every stdout line parses as JSON "
                    "(%d unparseable — the round-XVIII Windows-path "
                    "escape class)" % bad)
    if cdocs:
        csum = find_summary(cdocs, "capture")
    check(csum is not None, "capture leg: summary present")
    if csum:
        check(csum.get("datagrams") == EXPECT_RECORDS,
              "capture leg: %d/%d datagrams recorded" %
              (csum.get("datagrams", -1), EXPECT_RECORDS))
        check(csum.get("file_errors", -1) == 0 and
              csum.get("recv_errors", -1) == 0,
              "capture leg: zero file/socket errors")
        check(os.path.isfile(live_hex) and os.path.getsize(live_hex) > 0,
              "capture leg: live.hex written")
        with open(live_hex, "r", encoding="utf-8", errors="replace") as f:
            lines = sum(1 for l in f if l.strip())
        check(lines == EXPECT_RECORDS,
              "capture leg: live.hex holds %d records (got %d)" %
              (EXPECT_RECORDS, lines))
        log("  rcvbuf: requested %s, applied %s" %
            (csum.get("rcvbuf_requested"), csum.get("rcvbuf_actual")))

    # --- leg 2: replay the LIVE capture against the reference -------------
    log("\n--- vector leg (replay of the live-recorded capture) ---")
    replay_out = os.path.join(WORK, "replay_live.jsonl")
    # stdout ONLY into the jsonl (a merged capture would let stderr noise —
    # e.g. sanitizer diagnostics from the vendored sha1.c in instrumented
    # builds — poison the verify input; the product's stdout contract is
    # pure json-lines either way)
    with open(replay_out, "wb") as out_fh:
        proc = subprocess.run([knvstd, "--replay-capture=" + live_hex,
                               "--log-dir=" + LOGDIR],
                              stdout=out_fh, stderr=subprocess.DEVNULL,
                              cwd=ROOT, timeout=120)
    log("$ %s --replay-capture=%s   [rc=%d]" % (knvstd, live_hex,
                                                proc.returncode))
    check(proc.returncode == 0, "vector leg: replay exit 0")
    rbad = count_unparseable(parse_jsonl(replay_out))
    check(rbad == 0, "vector leg: every replay stdout line parses as JSON "
                     "(%d unparseable)" % rbad)
    check(verify_against(cap_json, replay_out),
          "vector leg: live capture replays byte-identical to the reference")

    # --- leg 2b: JSONL contract vs backslash paths (the T13 r1 finding:
    # "C:\Users\..." embedded raw made summary lines unparseable). On
    # Windows every leg already exercises absolute backslash paths; on
    # POSIX we force the case with a literal backslash in the file name.
    if not IS_WINDOWS:
        odd_hex = os.path.join(WORK, "back\\slash.hex")
        shutil.copyfile(cap_hex, odd_hex)
        odd_out = os.path.join(WORK, "replay_backslash.jsonl")
        with open(odd_out, "wb") as out_fh:
            proc = subprocess.run([knvstd, "--replay-capture=" + odd_hex,
                                   "--log-dir=" + LOGDIR],
                                  stdout=out_fh, stderr=subprocess.DEVNULL,
                                  cwd=ROOT, timeout=120)
        obad = count_unparseable(parse_jsonl(odd_out))
        check(proc.returncode == 0 and obad == 0,
              "jsonl contract: backslash path stays parseable "
              "(%d unparseable)" % obad)
        check(verify_against(cap_json, odd_out),
              "jsonl contract: backslash-path replay verifies")

    # --- legs 3+4: decode (paced + blast) ---------------------------------
    au_dir = os.path.join(WORK, "aus_paced")
    os.makedirs(au_dir, exist_ok=True)
    run_decode_leg("paced", knvstd, cap_hex, cap_json, 1, None, au_dir)
    run_decode_leg("blast", knvstd, cap_hex, cap_json, 0, None, None)

    # --- legs 5+6: SRTP ----------------------------------------------------
    if args.quick:
        log("\nSKIP SRTP legs (--quick)")
    else:
        for name, spec, key in (
                ("cm128", "cm128:10:%s:%s" % (SRTP_KEY, SRTP_SALT), SRTP_KEY),
                ("gcm256", "gcm256:8:%s:%s" % (SRTP_KEY256, SRTP_SALT),
                 SRTP_KEY256)):
            if name == "gcm256" and not backend_full:
                log("\nSKIP gcm256 leg (binary lacks the mbedTLS SRTP "
                    "backend — bare builds are cm128-only by design)")
                continue
            pcap = os.path.join(WORK, "cap_%s.hex" % name)
            pman = os.path.join(WORK, "cap_%s.json" % name)
            rc, out, _ = t10.run([sys.executable, MAKER, "--out", pcap,
                                  "--manifest", pman, "--fec-pct", "25",
                                  "--loss", "6", "--reorder", "0.3",
                                  "--srtp", spec], timeout=120)
            check(rc == 0 and os.path.isfile(pcap),
                  "srtp %s: protected capture generated" % name)
            run_decode_leg("srtp_" + name, knvstd, pcap, pman, 0, spec, None)

    # --- leg 7: the binary's own UDP self-test -----------------------------
    log("\n--- sock.loopback self-test (shipped binary) ---")
    rc, out, _ = t10.run([knvstd, "--selftest=sock.loopback",
                          "--log-dir=" + LOGDIR], timeout=120)
    result = None
    doc = parse_last_json(out)
    if doc:
        rows = doc.get("results", [])
        if rows and rows[0].get("name") == "sock.loopback":
            result = rows[0]
    check(result is not None and result.get("status") == "ok",
          "sock.loopback ok in the target binary (%s)" %
          (result.get("reason", "no result") if result else "missing"))

    # --- jsonl hygiene ------------------------------------------------------
    if os.path.isdir(os.path.join(ROOT, LOGDIR)):
        rc, _, _ = t10.run([sys.executable,
                            os.path.join(ROOT, "tools", "check_jsonl.py"),
                            LOGDIR], timeout=120)
        check(rc == 0, "check_jsonl over %s" % LOGDIR)

    failed = [label for ok, label in RESULTS if not ok]
    if failed:
        verdict = "FAIL (%d checks failed)" % len(failed)
    else:
        verdict = "ALL PASS"
    log("\n============================================================")
    log("T13 VERDICT: %s (%d checks)" % (verdict, len(RESULTS)))
    log("work:   " + WORK)
    log("logs:   " + os.path.join(ROOT, LOGDIR))
    with open(WYNIK, "w", encoding="utf-8") as f:
        f.write("\n".join(OUT) + "\n")
    with open(SKROT, "w", encoding="utf-8") as f:
        f.write("T13 stage-2 UDP E2E — %s\n" %
                time.strftime("%Y-%m-%d %H:%M:%S"))
        f.write("verdict: %s\n" % verdict)
        f.write("checks: %d total, %d failed\n" % (len(RESULTS), len(failed)))
        for ok, label in RESULTS:
            f.write("%s %s\n" % ("PASS" if ok else "FAIL", label))
    log("written: " + WYNIK)
    log("written: " + SKROT)
    return 1 if failed else 0


def verdict_fail(reason):
    log("\nT13 VERDICT: FAIL (" + reason + ")")
    with open(WYNIK, "w", encoding="utf-8") as f:
        f.write("\n".join(OUT) + "\n")
    with open(SKROT, "w", encoding="utf-8") as f:
        f.write("T13 stage-2 UDP E2E — %s\n" %
                time.strftime("%Y-%m-%d %H:%M:%S"))
        f.write("verdict: FAIL (%s)\n" % reason)


if __name__ == "__main__":
    sys.exit(main())
