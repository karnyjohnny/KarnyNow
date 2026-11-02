# test_launcher.py — the headless battery for kn_launcher (no Qt event
# loop needed; the GUI gets an offscreen import+instantiate smoke at the
# end when PyQt5 is importable).
#
# Run:  python3 tools/kn_launcher/test_launcher.py     (from repo root)

from __future__ import annotations

import json
import os
import sys
import tempfile

FAILS = 0
CHECKS = 0


def check(cond, msg):
    global FAILS, CHECKS
    CHECKS += 1
    if not cond:
        FAILS += 1
        print("FAIL %s" % msg)


# isolate the vault BEFORE importing store (app_dir reads the env)
_TMP = tempfile.mkdtemp(prefix="kn_launcher_test_")
os.environ["XDG_CONFIG_HOME"] = _TMP
os.environ["APPDATA"] = _TMP
os.environ["HOME"] = _TMP

sys.path.insert(0, os.path.join(os.path.dirname(
    os.path.abspath(__file__)), ".."))

import httpx  # noqa: E402
from kn_launcher import auth, catalog, search, store  # noqa: E402

# ------------------------------------------------------------- search
GAMES = [
    {"title": "Arknights: Endfield", "developer": "Hypergryph",
     "owned": True, "app_ids": [107057914]},
    {"title": "Cyberpunk 2077", "developer": "CD Projekt Red",
     "owned": False, "app_ids": [1]},
    {"title": "Wiedźmin 3: Dziki Gon", "developer": "CD Projekt Red",
     "owned": True, "app_ids": [2]},
    {"title": "Borderlands 4", "developer": "Gearbox", "owned": False,
     "app_ids": [3]},
    {"title": "Starfield", "developer": "Bethesda", "owned": False,
     "app_ids": [4]},
    {"title": "Fortnite", "developer": "Epic", "owned": True,
     "app_ids": [5]},
]

r = search.smart_filter(GAMES, "endfield")
check(r and r[0]["title"] == "Arknights: Endfield",
      "search: 'endfield' -> Arknights: Endfield (the operator's case)")
r = search.smart_filter(GAMES, "ark end")
check(r and r[0]["title"] == "Arknights: Endfield",
      "search: token prefixes 'ark end'")
r = search.smart_filter(GAMES, "aknd")
check(any(g["title"] == "Arknights: Endfield" for g in r),
      "search: subsequence 'aknd'")
r = search.smart_filter(GAMES, "wiedzmin")
check(r and r[0]["title"].startswith("Wiedźmin"),
      "search: diacritic folding 'wiedzmin' -> Wiedźmin")
r = search.smart_filter(GAMES, "cd projekt")
check(len(r) >= 2, "search: developer match")
r = search.smart_filter(GAMES, "")
check(len(r) == len(GAMES), "search: empty query = full list")
r = search.smart_filter(GAMES, "zzzqqq")
check(r == [], "search: no false positives")
r = search.smart_filter(GAMES, "fort")
check(r and r[0]["title"] == "Fortnite", "search: word-start ranks first")

# -------------------------------------------------------------- store
check(store.load() is None, "store: empty vault loads as None")
bundle = {"access_token": "a", "refresh_token": "r", "id_token": "i",
          "expires_at": 9999999999, "user": {"email": "x@y.z"}}
store.save(bundle)
got = store.load()
check(got and got["access_token"] == "a" and got["user"]["email"] == "x@y.z",
      "store: round-trip")
store.wipe()
check(store.load() is None, "store: wipe")
d1 = store.device_id()
d2 = store.device_id()
check(d1 == d2 and len(d1) == 36, "store: device_id stable UUID")
check(store.sub_hash("abc") == store.sub_hash("abc") and
      store.sub_hash("abc") != store.sub_hash("abd"),
      "store: sub_hash deterministic + distinguishing")
check(not store.bundle_expired({"expires_at": 9999999999}) and
      store.bundle_expired({"expires_at": 1}), "store: expiry math")

# --------------------------------------------------------------- auth
claims_b64 = ("eyJhbGciOiJub25lIn0." +
              __import__("base64").urlsafe_b64encode(json.dumps(
                  {"email": "op@example.com", "sub": "SUB-123",
                   "exp": 4000000000}).encode()).decode().rstrip("="))
b = auth.make_bundle({"access_token": "AT", "id_token": claims_b64,
                      "refresh_token": "RT", "expires_in": 3600})
check(auth.service_token(b) == claims_b64,
      "auth: service token = id_token when present")
check(b["user"]["email"] == "op@example.com" and
      "sub" not in b["user"] and
      "SUB-123" not in json.dumps(b),
      "auth: email kept, the sub claim NEVER stored (only its hash)")
check(b["user"]["sub_hash"] == store.sub_hash("SUB-123"),
      "auth: sub_hash matches")
check(abs(b["expires_at"] - (__import__("time").time() + 3600)) < 5,
      "auth: expires_at from expires_in")
b2 = auth.make_bundle({"access_token": "AT", "id_token": claims_b64})
check(b2["expires_at"] == 4000000000, "auth: expiry falls back to jwt exp")
check(auth.service_token({"access_token": "AT"}) == "AT",
      "auth: service token falls back to access_token")

# the device-authorize request shape against a mock transport
seen = {}


def _handler(request: httpx.Request) -> httpx.Response:
    seen["url"] = str(request.url)
    seen["body"] = request.content.decode()
    seen["headers"] = dict(request.headers)
    return httpx.Response(200, json={
        "device_code": "DEV123", "user_code": "WKJD-9981",
        "verification_uri": "https://login.nvidia.com/activate",
        "verification_uri_complete":
            "https://login.nvidia.com/activate?code=WKJD-9981",
        "interval": 5, "expires_in": 900})


with httpx.Client(transport=httpx.MockTransport(_handler)) as c:
    resp = auth.device_authorize(client=c)
check(seen["url"] == auth.DEVICE_AUTHORIZE_URL, "auth: authorize endpoint")
check("client_id=" + auth.STEAM_DECK_CLIENT_ID in seen["body"] and
      "scope=" in seen["body"] and "idp_id=" + auth.DEFAULT_IDP_ID in
      seen["body"] and "device_id=" in seen["body"],
      "auth: authorize form fields (AUTH.md §2)")
check(seen["headers"].get("nv-device-model") == "STEAMDECK" and
      seen["headers"].get("nv-client-type") == "BROWSER" and
      seen["headers"].get("origin") == "https://play.geforcenow.com",
      "auth: Steam-Deck browser identity headers")
url, code = auth.verification_urls(resp)
check(url == "https://login.nvidia.com/activate?code=WKJD-9981" and
      code == "WKJD-9981", "auth: verification URL with the PIN embedded")
url2, _ = auth.verification_urls(
    {"verification_uri": "https://x/activate", "user_code": "AB-1"})
check(url2 == "https://x/activate?code=AB-1",
      "auth: PIN appended when the complete URI is absent")


def _token_handler(request: httpx.Request) -> httpx.Response:
    body = request.content.decode()
    seen["token_body"] = body
    if "grant_type=urn" in body:
        return httpx.Response(400, json={"error": "authorization_pending"})
    return httpx.Response(400, json={"error": "invalid_grant"})


with httpx.Client(transport=httpx.MockTransport(_token_handler)) as c:
    res = auth.poll_token("DEV123", client=c)
check(res["state"] == "authorization_pending", "auth: pending state")
check("device_code=DEV123" in seen["token_body"] and
      "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type"
      "%3Adevice_code" in seen["token_body"],
      "auth: device_code grant form")
with httpx.Client(transport=httpx.MockTransport(_token_handler)) as c:
    res = auth.refresh("RT", client=c)
check(res["state"] == "relogin", "auth: invalid_grant -> forced re-login")

# verify_and_vpc happy + rejection paths
def _vpc_handler(request: httpx.Request) -> httpx.Response:
    seen["auth_hdr"] = request.headers.get("authorization", "")
    if "GOOD" in seen["auth_hdr"]:
        return httpx.Response(200, json={
            "requestStatus": {"statusCode": 1, "serverId": "VPC-9"}})
    return httpx.Response(401, json={})


with httpx.Client(transport=httpx.MockTransport(_vpc_handler)) as c:
    ok, vpc = auth.verify_and_vpc({"id_token": "GOODTOKEN"}, client=c)
check(ok and vpc == "VPC-9", "auth: verify returns the vpcId (serverId)")
check(seen["auth_hdr"].startswith("GFNJWT "), "auth: GFNJWT scheme")
with httpx.Client(transport=httpx.MockTransport(_vpc_handler)) as c:
    ok, why = auth.verify_and_vpc({"id_token": "BADTOKEN"}, client=c)
check(not ok and "rejected" in why, "auth: 401 -> rejected")

# ------------------------------------------------------------ catalog
ITEM = {"id": "uuid-1", "title": "Arknights: Endfield",
        "developerName": "Hypergryph", "publisherName": "YOStar",
        "variants": [{"id": "107057914", "appStore": "EPIC",
                      "shortName": "endfield_epic",
                      "gfn": {"status": "AVAILABLE",
                              "library": {"status": "NOT_OWNED"}}},
                     {"id": "notnumeric", "gfn": {}}],
        "gfn": {"playabilityState": "PLAYABLE",
                "minimumMembershipTierLabel": "PAID"}}
ITEM2 = json.loads(json.dumps(ITEM))
ITEM2["variants"][0]["gfn"]["library"]["status"] = "OWNED"

# The operator's own two-store example (run-103 prompt), verbatim shape:
# one title, GAIJIN free-to-play next to STEAM PLATFORM_SYNC+selected.
WT = {"id": "956b4ff0-4fa7-43ca-a565-a552723977bb", "title": "War Thunder",
      "variants": [
          {"id": "10839111", "shortName": "war_thunder_gfn_pc",
           "appStore": "GAIJIN",
           "gfn": {"status": "AVAILABLE",
                   "library": {"status": "NOT_OWNED", "selected": False}}},
          {"id": "100234911", "shortName": "war_thunder_steam_gfn_pc",
           "appStore": "STEAM",
           "gfn": {"status": "AVAILABLE",
                   "library": {"status": "PLATFORM_SYNC",
                               "selected": True}}}],
      "gfn": {"playabilityState": "PLAYABLE",
              "minimumMembershipTierLabel": None}}

g = catalog.normalize(ITEM)
check(g["app_ids"] == [107057914], "catalog: numeric variant id = appId")
check(g["owned"] is False and g["tier"] == "PAID",
      "catalog: NOT_OWNED + tier label")
check(catalog.normalize(ITEM2)["owned"] is True, "catalog: OWNED variant")
check(catalog.normalize(ITEM, {"uuid-1"})["owned"] is True,
      "catalog: library-query membership marks owned")

# --- slice 4q: the store-variant model (Steam/Epic/Gaijin appIds) ---
w = catalog.normalize(WT)
check(len(w["variants"]) == 2, "variants: both stores captured")
check([v["store"] for v in w["variants"]] == ["GAIJIN", "STEAM"],
      "variants: appStore preserved in server order")
check(w["variants"][1]["selected"] is True and
      w["variants"][1]["in_library"] is True,
      "variants: library.selected + PLATFORM_SYNC = in_library")
check(w["variants"][0]["in_library"] is False,
      "variants: NOT_OWNED is not in the library")
check(w["owned"] is True, "variants: one owned variant owns the title")
check(catalog.pick_variant_index(w["variants"]) == 1,
      "variants: pick = library.selected (the web app's choice)")
_no_sel = [{"selected": False, "in_library": False},
           {"selected": False, "in_library": True},
           {"selected": False, "in_library": False}]
check(catalog.pick_variant_index(_no_sel) == 1,
      "variants: pick falls back to in-library")
_none = [{"selected": False, "in_library": False}] * 3
check(catalog.pick_variant_index(_none) == 0, "variants: pick falls back to 0")
check(w["default_app_id"] == 100234911,
      "variants: launch id = the SELECTED variant (100234911, not 10839111)")
check(catalog.launch_app_id([{"app_id": 0, "selected": True},
                             {"app_id": 7, "selected": False}]) == 7,
      "variants: a non-numeric selected id falls through to the first "
      "numeric one")
check(catalog.launch_app_id([], "12345") == 12345,
      "variants: no variants -> a numeric UUID is the launch id")
check(catalog.launch_app_id([], "uuid-x") == 0,
      "variants: nothing numeric -> 0 (unlaunchable)")
_dupe = json.loads(json.dumps(WT))
_dupe["variants"].append(json.loads(json.dumps(WT["variants"][0])))
check(len(catalog.variant_records(_dupe)) == 2,
      "variants: duplicate ids are deduped (OpenNOW catalog.rs:850)")
check(catalog.normalize(ITEM)["default_app_id"] == 107057914,
      "variants: normalize exposes default_app_id")

# --- slice 4q: merging two views of the same title ---
_a = catalog.normalize({"id": "u", "title": "T", "variants": [],
                        "gfn": {"minimumMembershipTierLabel": ""}})
_b = catalog.normalize({"id": "u", "title": "T", "developerName": "D",
                        "variants": WT["variants"],
                        "gfn": {"minimumMembershipTierLabel": "PAID"}})
catalog.merge_records(_a, _b)
check(_a["tier"] == "PAID", "merge: a missing tier is filled")
check(_a["developer"] == "D", "merge: a missing developer is filled")
check(_a["variants"] and _a["default_app_id"] == 100234911,
      "merge: empty variants are replaced and the launch id recomputed")
_c = catalog.normalize(ITEM)
_d = catalog.normalize(ITEM2)
catalog.merge_records(_c, _d)
check(_c["owned"] is True, "merge: ownership is OR-ed")
_s = catalog.sort_games([{"title": "b", "owned": False},
                         {"title": "a", "owned": True},
                         {"title": "A", "owned": True}])
check([x["owned"] for x in _s] == [True, True, False] and
      _s[-1]["title"] == "b",
      "sort: owned first, unowned last (A-Z inside each group)")

# --- the three network operations against one mock transport ---
def _resp(items, has_next=False, cursor="", total=None):
    return httpx.Response(200, json={"data": {"apps": {
        "numberReturned": len(items),
        "pageInfo": {"hasNextPage": has_next, "endCursor": cursor,
                     "totalCount": len(items) if total is None else total},
        "items": items}}})


def _gql_handler(request: httpx.Request) -> httpx.Response:
    body = json.loads(request.content)
    v = body["variables"]
    seen.setdefault("queries", []).append(body["query"])
    if "searchString" in v:                       # SEARCH_QUERY
        q = v["searchString"].lower()
        hits = [i for i in (ITEM, ITEM2, WT) if q in i["title"].lower()]
        return _resp(hits, total=len(hits))
    if v["filters"] == catalog.LIBRARY_FILTERS:   # the owned library
        return _resp([ITEM2], total=1)
    cur = v["cursor"]                             # BROWSE_QUERY, paged
    if cur == "":
        return _resp([ITEM], True, "C1", 3)
    if cur == "C1":
        return _resp([WT], True, "C2", 3)
    return _resp([ITEM2], False, "", 3)


with httpx.Client(transport=httpx.MockTransport(_gql_handler)) as c:
    lib = catalog.fetch_library("TOK", "VPC-9", client=c)
check(len(lib) == 1 and lib[0]["owned"] is True,
      "fetch_library: ONLY the owned-library query runs (the <1 s default)")
check(all("searchQuery" not in q for q in seen["queries"]),
      "fetch_library: never sends the search field")

seen["queries"] = []
with httpx.Client(transport=httpx.MockTransport(_gql_handler)) as c:
    pages = [(len(recs), tot) for recs, tot
             in catalog.iter_browse("TOK", "VPC-9", client=c)]
check(pages == [(1, 3), (1, 3), (1, 3)],
      "iter_browse: streams one page per yield (3 pages of 1)")
check(len(seen["queries"]) == 3,
      "iter_browse: one request per page — the GUI paints between them")

with httpx.Client(transport=httpx.MockTransport(_gql_handler)) as c:
    hits = catalog.search_server("TOK", "VPC-9", "thunder", client=c)
check(len(hits) == 1 and hits[0]["title"] == "War Thunder",
      "search_server: the server-side searchQuery finds the title")
check("searchQuery: $searchString" in seen["queries"][-1],
      "search_server: uses the OpenNOW STORE_SEARCH_QUERY field")
seen["queries"] = []
with httpx.Client(transport=httpx.MockTransport(_gql_handler)) as c:
    check(catalog.search_server("TOK", "VPC-9", "  ", client=c) == [],
          "search_server: a blank query costs zero requests")
check(seen["queries"] == [], "search_server: confirmed no wire traffic")

seen["queries"] = []
with httpx.Client(transport=httpx.MockTransport(_gql_handler)) as c:
    games = catalog.fetch_games("TOK", "VPC-9", client=c)
check(len(games) == 2, "fetch_games: browse U library dedup (2 unique)")
by_title = {g["title"]: g for g in games}
check(by_title["Arknights: Endfield"]["owned"] is True and
      by_title["War Thunder"]["owned"] is True,
      "fetch_games: ownership flags after the merge")
check(games[0]["owned"] is True, "fetch_games: owned sort first")
check("searchQuery" not in catalog.BROWSE_QUERY and
      "searchQuery: $searchString" in catalog.SEARCH_QUERY,
      "queries: browse has no search field, SEARCH_QUERY does (the "
      "OpenNOW transcription guard)")

# ---------------------------------------------------- params (F37 fix)
from kn_launcher import params as kn_params  # noqa: E402

split = kn_params.split_params(
    "--session-seconds=1200 --input-anti-afk=60000 --session-present "
    "--session-audio-play --session-release --session-interactive "
    "--wire-dump=logs\\wire_r1.txt")
check(split[-1] == "--wire-dump=logs\\wire_r1.txt",
      "params: Windows backslash path survives the split (the run-101 "
      "GUI regression: posix shlex ate it into 'logswire_r1.txt')")
check(len(split) == 7 and split[0] == "--session-seconds=1200",
      "params: the operator's default block splits into 7 argv entries")
check(kn_params.split_params('--session-title="My Game"') ==
      ["--session-title=My Game"],
      "params: quoted value with spaces stays one entry, quotes stripped")
check(kn_params.split_params('"--session-title=My Game"') ==
      ["--session-title=My Game"],
      "params: fully quoted entry")
check(kn_params.split_params('--x=a\\"b --y=c') == ['--x=a"b', "--y=c"],
      "params: escaped quote stays literal (CommandLineToArgvW rules)")
check(kn_params.split_params("  --a\t--b  ") == ["--a", "--b"],
      "params: runs of whitespace collapse")
check(kn_params.split_params("") == [], "params: empty box -> empty argv")

# slice 4q: the LOT A recipe from the run-104 flight plan
lot_a = kn_params.split_params(
    "--session-seconds=1200 --input-anti-afk=60000 --session-present "
    "--session-audio-play --session-release "
    "--input-script=build\\login_1h.txt")
check(lot_a[-1] == "--input-script=build\\login_1h.txt" and len(lot_a) == 6,
      "params: the LOT A macro recipe splits intact (backslash kept)")
check(kn_params.DEFAULT_PARAMS.startswith("--session-seconds=1200"),
      "params: DEFAULT_PARAMS keeps the operator's recipe")
check("--wire-dump" not in kn_params.DEFAULT_PARAMS,
      "params: DEFAULT_PARAMS no longer carries --wire-dump — the launcher "
      "injects a PER-DRAW path, so two draws cannot overwrite one dump")
check("--session-interactive" in kn_params.DEFAULT_PARAMS,
      "params: the default is still the interactive (goal G1) recipe")
check("--session-token" not in kn_params.DEFAULT_PARAMS,
      "params: the token is NEVER part of the typed parameters")

check(kn_params.flag_value(["--session-app=42"], "session-app") == "42" and
      kn_params.flag_value(["--x"], "session-app") is None,
      "params: flag_value reads --name=VALUE")
check(kn_params.has_flag(["--session-present"], "session-present") and
      kn_params.has_flag(["--log-dir=x"], "log-dir") and
      not kn_params.has_flag(["--log-directory=x"], "log-dir"),
      "params: has_flag matches the boolean AND the value form, no prefix "
      "false positives")
check(kn_params.drop_flags(["--a", "--log-dir=x", "--wire-dump=y", "--b"],
                           ["log-dir", "wire-dump"]) == ["--a", "--b"],
      "params: drop_flags removes both injected flags")
_df = kn_params.draw_flags(os.path.join("logs", "run104_logs", "1"))
check(_df[0].startswith("--log-dir=") and
      _df[1].endswith(kn_params.WIRE_DUMP_NAME) and
      os.path.dirname(_df[1].split("=", 1)[1]) == os.path.join(
          "logs", "run104_logs", "1"),
      "params: draw_flags points --log-dir AND --wire-dump inside the draw")
_in = kn_params.inject_draw_flags(
    ["knvstd", "--session", "--wire-dump=logs\\old.txt"],
    os.path.join("logs", "run104_logs", "2"))
check(sum(1 for a in _in if a.startswith("--wire-dump=")) == 1 and
      _in[-1].endswith("wire_r1.txt"),
      "params: inject_draw_flags REPLACES an operator-typed dump (never "
      "duplicates it)")
_red = kn_params.redact_argv(["--session-token=SUPERSECRET", "--session"],
                             "SUPERSECRET")
check("SUPERSECRET" not in " ".join(_red) and "redacted:11" in _red[0],
      "params: redact_argv hides the token but keeps its length")

# --------------------------------------------- python 3.8 target guard
# The rig runs Python 3.8.20 on Win7 (tests/requirements.txt). Two
# guards so the "failed gui" class (PEP 604/585 annotations raising
# TypeError AT IMPORT on 3.8) can never ship again:
#   (a) every module carries `from __future__ import annotations`
#       (annotations become lazy strings — never evaluated);
#   (b) every module still parses under the 3.8 grammar (ast
#       feature_version) — catches match-statements & friends.
import ast  # noqa: E402

_pkg_dir = os.path.dirname(os.path.abspath(kn_params.__file__))
_mods = sorted(f for f in os.listdir(_pkg_dir) if f.endswith(".py"))
check(len(_mods) >= 8, "py38: the package modules are all visible")
for _m in _mods:
    _src = open(os.path.join(_pkg_dir, _m), encoding="utf-8").read()
    check("from __future__ import annotations" in _src,
          "py38: %s has the lazy-annotations future import" % _m)
    try:
        ast.parse(_src, filename=_m, feature_version=(3, 8))
        _ok = True
    except SyntaxError as _exc:
        _ok = False
        print("  %s: 3.8 syntax error: %s" % (_m, _exc))
    check(_ok, "py38: %s parses under the Python 3.8 grammar" % _m)

# --------------------------------------- store: cache + run folders (4q)
_LOGS = tempfile.mkdtemp(prefix="kn_logs_")
check(store.suggest_run_name(_LOGS) == "run100_logs",
      "store: an empty logs dir suggests run100_logs")
for _n in ("run100.log", "run101.log", "run103.log", "wire_r1.txt"):
    open(os.path.join(_LOGS, _n), "w").close()
check(store.suggest_run_name(_LOGS) == "run104_logs",
      "store: LEGACY runNNN.log files count, so 103 -> run104_logs (the "
      "operator numbers from 100)")
os.makedirs(os.path.join(_LOGS, "run107_logs", "1"))
os.makedirs(os.path.join(_LOGS, "run107_logs", "2"))
check(store.suggest_run_name(_LOGS) == "run108_logs",
      "store: run FOLDERS count too")
check(store.run_number("run104_logs") == 104 and
      store.run_number("run104") == 104 and
      store.run_number("run104.log") == -1 and
      store.run_number("wire_r1.txt") == -1,
      "store: run_number is strict (suggest_run_name strips the legacy .log)")
check(store.valid_run_name("run104_logs") and store.valid_run_name("run104")
      and not store.valid_run_name("../evil") and
      not store.valid_run_name("run104 logs") and
      not store.valid_run_name(""),
      "store: valid_run_name rejects anything that is not a safe directory")
check(store.draw_dirs(_LOGS, "run107_logs") == [1, 2] and
      store.next_draw_index(_LOGS, "run107_logs") == 3,
      "store: draw numbering continues where the operator left off")
_p, _i = store.open_draw_dir(_LOGS, "run107_logs")
check(_i == 3 and os.path.isdir(_p) and _p.endswith(os.sep + "3"),
      "store: open_draw_dir allocates AND creates the next draw folder")
_p2, _i2 = store.open_draw_dir(_LOGS, "run107_logs")
check(_i2 == 4, "store: two launches in one run never collide (run103/3 "
                "had no logs at all — that class is gone)")

check(store.load_catalog()["games"] == [], "store: no cache -> empty list")
store.save_catalog([{"id": "u1", "title": "T", "owned": True,
                     "app_ids": [1], "variants": [], "default_app_id": 1}],
                   "VPC-9", "library")
_meta = store.load_catalog()
check(_meta["count"] == 1 and _meta["games"][0]["title"] == "T" and
      _meta["vpc_id"] == "VPC-9" and _meta["kind"] == "library",
      "store: catalog cache round-trip")
check(store.cache_age_s(_meta) < 5 and store.cache_usable(_meta),
      "store: a fresh cache is usable")
check(store.cache_age_s({}) == float("inf") and
      not store.cache_usable({}),
      "store: a cache that never existed is not usable (and does not raise)")
check("1 games" in store.cache_summary(_meta) and "library" in
      store.cache_summary(_meta),
      "store: cache_summary is one human line")
open(store.cache_path(), "w").write("{ this is not json")
_bad = store.load_catalog()
check(_bad["games"] == [] and _bad["count"] == 0,
      "store: a CORRUPT cache degrades to empty instead of crashing before "
      "the window exists (the F37 lesson)")
store.save_catalog([{"id": "u1", "title": "T"}], "V", "full")
_mf = os.path.join(_LOGS, "manifest.txt")
check(store.write_manifest(_mf, {
    "run": "run104_logs", "draw": 1, "game_title": "War Thunder",
    "app_id": 100234911, "app_store": "STEAM",
    "argv_redacted": ["knvstd", "--session-token=<redacted:11>"],
    "unexpected_key": "sorted last"}) and os.path.isfile(_mf),
    "store: write_manifest writes the birth certificate")
_mtxt = open(_mf, encoding="utf-8").read()
check("War Thunder" in _mtxt and "100234911" in _mtxt and
      "STEAM" in _mtxt and "run104_logs" in _mtxt,
      "store: the manifest names the run, the game AND the store variant")
check("redacted" in _mtxt and "SUPERSECRET" not in _mtxt,
      "store: the manifest carries the redacted argv only")
check(_mtxt.index("game_title") < _mtxt.index("unexpected_key"),
      "store: known manifest keys keep their order, extras sort last")

# ----------------------------------------------------------------- GUI
try:
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from kn_launcher import gui  # noqa: F401
    from PyQt5.QtWidgets import QApplication
    app = QApplication.instance() or QApplication([])
    store.save_catalog([dict(x) for x in
                        [catalog.normalize(WT), catalog.normalize(ITEM2)]],
                       "VPC-9", "library")
    win = gui.MainWindow()
    check(win.tabs.count() == 2 and not win.tabs.isTabEnabled(1),
          "gui: two tabs, tab 2 locked before login")
    check(len(win.games) == 2 and win.games_kind == "cache" and
          win.game_list.count() == 2,
          "gui: the list paints from the DISK CACHE at startup, before any "
          "network (slice 4q)")
    check("cached" in win.catalog_status.text(),
          "gui: the status bar says the list came from the cache")
    check(win.full_chk.isChecked() is False,
          "gui: 'full catalog' is OFF by default — a refresh pulls only the "
          "owned library")
    win.games = GAMES
    win.games_kind = ""
    win.tabs.setTabEnabled(1, True)
    win._render_games()
    check(win.game_list.count() == len(GAMES), "gui: game list renders")
    win.search_box.setText("endfield")
    check(win.game_list.count() >= 1 and
          "Arknights: Endfield" in win.game_list.item(0).text(),
          "gui: smart search in the list ('endfield' first)")
    win.search_box.setText("")
    import re as _re
    check(_re.match(r"^run\d+_logs$", win.logfolder.text()),
          "gui: runNNN_logs auto-suggestion (the log FILE became a FOLDER)")
    win.logfolder.setText("run900_logs")
    win._update_draw_hint()
    check("run900_logs" in win.draw_hint.text() and
          win.launch_btn.isEnabled(),
          "gui: the draw hint shows where the next launch writes")
    win.logfolder.setText("not a run")
    win._update_draw_hint()
    check(not win.launch_btn.isEnabled() and "⚠" in win.draw_hint.text(),
          "gui: an invalid run name disables Launch instead of making a "
          "directory out of it")
    win.logfolder.setText("run900_logs")
    win._update_draw_hint()
    check(gui.DEFAULT_PARAMS.startswith("--session-seconds=1200"),
          "gui: the operator's default parameter block")
    check("--session-token" not in gui.DEFAULT_PARAMS,
          "gui: the token is NEVER part of the typed parameters")

    # the store-variant picker, on the operator's War Thunder example
    win.games = [catalog.normalize(WT), catalog.normalize(ITEM)]
    win._render_games()
    check("2 stores" in win.game_list.item(0).text(),
          "gui: a multi-store title advertises its variant count in the list")
    win.game_list.setCurrentRow(0)
    check(win.variant_box.count() == 2 and not win.variant_box.isHidden(),
          "gui: the variant picker appears for a 2-store title")
    check(win.variant_box.currentText().startswith("STEAM"),
          "gui: the picker pre-selects the web app's variant (STEAM)")
    check("100234911" in win.variant_box.currentText() and
          win._selected_app_id() == 100234911,
          "gui: --session-app would get the PICKED variant's id")
    check("web default" in win.variant_box.currentText(),
          "gui: the picker labels the variant the web app selected")
    win.variant_box.setCurrentIndex(0)
    check(win._selected_app_id() == 10839111,
          "gui: choosing GAIJIN changes the id that flies")
    win.game_list.setCurrentRow(1)
    check(win.variant_box.count() <= 1 and win.variant_box.isHidden(),
          "gui: a single-variant title hides the picker")
    check(win._selected_app_id() == 107057914,
          "gui: a single-variant title still resolves its app id")

    # server-side search results rank above the local list
    win.games = [catalog.normalize(ITEM2)]
    win._render_games()
    check(win.game_list.count() == 1 and
          "Arknights" in win.game_list.item(0).text(),
          "gui: the library-only list holds just the owned title")
    win.search_box.setText("thunder")
    check(win.game_list.count() == 0,
          "gui: 'thunder' matches nothing locally — this is the case the "
          "server-side search exists for")
    win._search_done([catalog.normalize(WT)], "thunder")
    check(win.game_list.count() == 1 and
          "War Thunder" in win.game_list.item(0).text(),
          "gui: the server hit paints a title the local list never had")
    win._search_done([catalog.normalize(ITEM)], "stale")
    check(win.game_list.count() == 1 and
          "War Thunder" in win.game_list.item(0).text(),
          "gui: a STALE server reply (another query) is discarded")
    win.search_box.setText("")
    win.search_hits, win.search_query = [], ""
    win._render_games()

    # the run folder is turned into a real per-draw path at launch time
    _d, _n = store.open_draw_dir(_LOGS, "run900_logs")
    check(_n == 1 and os.path.isdir(_d),
          "gui: launch-time draw allocation starts at 1 for a fresh run")
    check(win._console_log_name("run104_logs") == "run104.log" and
          win._console_log_name("run104") == "run104.log",
          "gui: the console copy inside the draw folder is runNNN.log")
    win.close()
except ImportError as exc:
    print("(gui smoke skipped: %s)" % exc)

print("kn_launcher tests: %d checks, %d failures -> %s" %
      (CHECKS, FAILS, "PASS" if not FAILS else "FAIL"))
sys.exit(1 if FAILS else 0)
