#!/usr/bin/env python3
"""Session-summary skeleton validator (round XLVIII; replaces the old
check_summary_printf.py block-scan approach and its 108-vs-112 mystery). Method: extract the summary printf format string from
knvstd_main.c, turn it into a JSON skeleton (all printf conversions ->
neutral values, %s -> ""), parse it, and compare the RECURSIVE KEY SET
against a live summary capture with the dynamic objects (drops,
outbox_by_sid) emptied. Canon = skeleton keyset == live keyset.

usage: python3 tools/check_summary_skeleton.py <repo_dir> <live_stdout_log>

canon: skeleton recursive-key set == live summary key set (with the two
dynamic objects drops/outbox_by_sid emptied). XLVIII skeleton = 113 keys
(79 through XLVII + the 34-key "audio" block).
"""
import io, json, re, sys

def extract_format(src):
    i = src.index('printf("{\\"mode\\":\\"session\\",\\"summary\\":')
    j = src.index('\\n",', i) + 3   # +3: include the \n and the closing
                                    # quote of the LAST C literal, else the
                                    # fragment regex drops it (unterminated)
    body = src[i:j]
    # join adjacent C string literals: ",\n<ws>" between fragments
    frag = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    return "".join(frag)

def skeleton(fmt):
    # C-unescape
    s = fmt.replace('\\\\"', '"').replace("\\\\n", "").replace("\\\\", "\\")
    s = s.replace('\\"', '"').replace("\\n", "")
    # printf conversions -> neutral JSON values
    s = re.sub(r'%[-+ #0]*[\d*]*(?:\.[\d*]+)?(?:ll|h|L)?[diouxX]', '0', s)
    s = re.sub(r'%[-+ #0]*[\d*]*(?:\.[\d*]+)?s', '', s)
    return s

def keys_recursive(o, prefix=""):
    out = set()
    if isinstance(o, dict):
        for k, v in o.items():
            p = prefix + "." + k if prefix else k
            out.add(p)
            out |= keys_recursive(v, p)
    elif isinstance(o, list):
        for idx, v in enumerate(o):
            out |= keys_recursive(v, prefix + "[]")
    return out

def live_summary(path):
    best = None
    for line in io.open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if '"summary"' not in line or not line.startswith("{"):
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        s = d.get("summary")
        if isinstance(s, dict):
            best = s
    return best

def main():
    repo, live = sys.argv[1], sys.argv[2]
    src = io.open(repo + "/streamer/src/knvstd_main.c", encoding="utf-8").read()
    skel = json.loads(skeleton(extract_format(src)))["summary"]
    lv = live_summary(live)
    if lv is None:
        print("FAIL: no session summary found in", live); return 1
    lv = json.loads(json.dumps(lv))
    lv["drops"] = {}            # dynamic: keyed by drop cause
    lv["outbox_by_sid"] = {}    # dynamic: keyed by sctp sid
    sk, lk = keys_recursive(skel), keys_recursive(lv)
    print("skeleton recursive keys:", len(sk))
    print("live     recursive keys:", len(lk))
    only_skel = sorted(sk - lk)
    only_live = sorted(lk - sk)
    print("only in skeleton:", only_skel if only_skel else "(none)")
    print("only in live    :", only_live if only_live else "(none)")
    if not only_skel and not only_live:
        print("PASS: summary skeleton == live summary key set")
        return 0
    print("FAIL: key sets differ")
    return 1

if __name__ == "__main__":
    sys.exit(main())
