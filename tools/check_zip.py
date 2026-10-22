#!/usr/bin/env python3
"""check_zip.py — independent validation of kn_diag bundles (§7.8).

Second opinion to the in-test C zip reader: uses python's zipfile (a
completely separate implementation) to verify that a diagnostic bundle is a
valid archive, that summary.json is the FIRST entry and parses as JSON, and
that the entry set is sane.

Usage:
    python3 tools/check_zip.py <bundle.zip> [<bundle2.zip> ...]
    python3 tools/check_zip.py --demo      # generate nothing; validate files given

Exit code 0 = all bundles valid, 1 = any failure (prints a short report).
"""
import json
import sys
import zipfile


def check_bundle(path: str) -> list:
    problems = []
    try:
        zf = zipfile.ZipFile(path)
    except Exception as exc:  # noqa: BLE001 — report anything
        return [f"{path}: cannot open as zip: {exc}"]

    with zf:
        bad = zf.testzip()  # CRC + header check on every entry
        if bad is not None:
            problems.append(f"{path}: corrupt entry: {bad}")

        names = zf.namelist()
        if not names:
            problems.append(f"{path}: archive is empty")
            return problems

        if names[0] != "summary.json":
            problems.append(f"{path}: first entry is {names[0]!r}, expected summary.json")

        try:
            summary = json.loads(zf.read("summary.json").decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            problems.append(f"{path}: summary.json is not valid JSON: {exc}")
            return problems

        for key in ("bundle_version", "generated_utc", "trigger", "verdicts",
                    "artifacts", "cap_bytes"):
            if key not in summary:
                problems.append(f"{path}: summary.json missing key {key!r}")

        # every stored entry must be method 0 (store-only, §7.8)
        for info in zf.infolist():
            if info.compress_type != zipfile.ZIP_STORED:
                problems.append(f"{path}: entry {info.filename!r} compressed "
                                f"(method {info.compress_type}), expected store-only")

        # artifact names in the summary must match actual entries
        listed = {a["name"] for a in summary.get("artifacts", [])
                  if not a.get("dropped")}
        actual = set(names) - {"summary.json"}
        if listed != actual:
            problems.append(f"{path}: summary artifact table {sorted(listed)} "
                            f"!= zip entries {sorted(actual)}")

    return problems


def main(argv) -> int:
    paths = [a for a in argv[1:] if not a.startswith("--")]
    if not paths:
        print(__doc__)
        return 1
    all_problems = []
    for p in paths:
        all_problems.extend(check_bundle(p))
    if all_problems:
        for prob in all_problems:
            print("FAIL", prob)
        print(f"ZIP VALIDATION FAIL: {len(all_problems)} problem(s) in {len(paths)} bundle(s)")
        return 1
    print(f"ZIP VALIDATION PASS: {len(paths)} bundle(s), store-only, "
          f"summary.json first, CRCs ok, artifact tables consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
