
#!/usr/bin/env python3
"""check_jsonl.py — strict validator for knlog .jsonl output.

Every line must be a complete JSON object with the mandatory §7.2 fields.
This is the "test the boring parts on the host" companion to test_knlog:
the C test writes the files, this script proves they are machine-parsable
(the property the diagnostic bundle depends on).

Usage: python3 tools/check_jsonl.py <dir> [<dir> ...]
Exit code 0 = all lines valid.
"""
import json
import os
import re
import sys

TS_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")
LEVELS = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"}
CHANNELS = {"app", "http", "rtsp", "transport", "media", "input", "perf", "diag"}
CORR_RE = re.compile(r"^[0-9a-f]{12}$")


def check_line(path, lineno, line, errors):
    def err(msg):
        errors.append(f"{path}:{lineno}: {msg}")

    try:
        obj = json.loads(line)
    except json.JSONDecodeError as e:
        err(f"invalid JSON: {e}")
        return
    if not isinstance(obj, dict):
        err("top level is not an object")
        return
    for key in ("ts", "mono_ms", "level", "channel", "pid", "thread", "msg"):
        if key not in obj:
            err(f"missing mandatory field '{key}'")
    ts = obj.get("ts")
    if isinstance(ts, str) and not TS_RE.match(ts):
        err(f"ts not RFC3339-UTC-millis: {ts!r}")
    if not isinstance(obj.get("mono_ms"), int):
        err(f"mono_ms not an integer: {obj.get('mono_ms')!r}")
    if obj.get("level") not in LEVELS:
        err(f"bad level: {obj.get('level')!r}")
    if obj.get("channel") not in CHANNELS:
        err(f"bad channel: {obj.get('channel')!r}")
    if not isinstance(obj.get("pid"), int) or not isinstance(obj.get("thread"), int):
        err("pid/thread not integers")
    if not isinstance(obj.get("msg"), str):
        err("msg not a string")
    if "correlation" in obj and not (
        isinstance(obj["correlation"], str) and CORR_RE.match(obj["correlation"])
    ):
        err(f"correlation not 12 lowercase hex chars: {obj['correlation']!r}")
    if "session_id" in obj and not isinstance(obj["session_id"], str):
        err("session_id not a string")
    if "fields" in obj and not isinstance(obj["fields"], dict):
        err("fields not an object")
    if obj.get("level") == "TRACE":
        err("TRACE event leaked into jsonl (jsonl_min_level violation)")
    # secret hygiene: no value may look like raw secret material
    blob = json.dumps(obj)
    if "supersecret" in blob or "=xyz" in blob:
        err("test secret value leaked into jsonl")


def main(argv):
    dirs = argv[1:] or ["testlogs"]
    errors = []
    files = 0
    lines = 0
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".jsonl"):
                continue
            path = os.path.join(d, name)
            files += 1
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for lineno, line in enumerate(f, 1):
                    line = line.strip()
                    if not line:
                        continue
                    lines += 1
                    check_line(path, lineno, line, errors)
    if errors:
        print(f"JSONL VALIDATION FAIL: {len(errors)} error(s) in {lines} line(s)")
        for e in errors[:40]:
            print("  " + e)
        return 1
    print(f"JSONL VALIDATION PASS: {lines} line(s) in {files} file(s), all valid")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))


