#!/usr/bin/env python3
"""decode_jwt_claims.py — LOCAL, offline GFNJWT claim-structure decoder.

Purpose: compare the CLAIM STRUCTURE (key names + non-sensitive values) of
the token our client flies vs the token the official browser client gets —
without any real credential ever leaving the operator's machine or being
pasted into chat/logs.

Usage:
    python decode_jwt_claims.py < token.txt
    python decode_jwt_claims.py "GFNJWT eyJhbGciOi..."
    python decode_jwt_claims.py            (then paste, EOF with Ctrl+Z/D)

Input may be the bare token or a whole header/line containing it; the
first JWT-shaped triple (xxx.yyy.zzz of base64url) is used. Output is safe
to share: every value of an identity-ish claim key is masked to its first
3 characters + length. The signature is NEVER printed.
"""
import base64
import json
import re
import sys

MASK_KEY_RE = re.compile(
    r"(id|uid|uuid|sub|email|name|user|acct|account|token|ip|addr|phone|"
    r"session|sid|auth|cred|secret|key)", re.I)


def b64url_decode(seg: str) -> bytes:
    pad = "=" * (-len(seg) % 4)
    return base64.urlsafe_b64decode(seg + pad)


def mask(key: str, val):
    if isinstance(val, (dict, list)):
        return redact(val, key)
    s = str(val)
    if MASK_KEY_RE.search(key) and len(s) > 3:
        return s[:3] + "...<masked len=%d>" % len(s)
    return val


def redact(obj, parent_key=""):
    if isinstance(obj, dict):
        return {k: mask(k, v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [mask(parent_key, v) for v in obj]
    return obj


def main() -> int:
    raw = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else sys.stdin.read()
    m = re.search(r"([A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{4,})",
                  raw)
    if not m:
        print("no JWT-shaped triple found in input", file=sys.stderr)
        return 2
    header_seg, payload_seg, sig_seg = m.group(1).split(".")[:3]
    try:
        header = json.loads(b64url_decode(header_seg))
        payload = json.loads(b64url_decode(payload_seg))
    except Exception as exc:  # noqa: BLE001 — honest failure mode
        print("decode failed: %s" % exc, file=sys.stderr)
        return 2
    out = {
        "alg_header": header,
        "claims_masked": redact(payload),
        "claim_keys": sorted(payload.keys()) if isinstance(payload, dict) else None,
        "signature": "<not printed, %d chars>" % len(sig_seg),
    }
    print(json.dumps(out, indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
