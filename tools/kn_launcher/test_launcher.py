# test_launcher.py — the headless battery for kn_launcher (no Qt event
# loop needed; the GUI gets an offscreen import+instantiate smoke at the
# end when PyQt5 is importable).
#
# Run:  python3 tools/kn_launcher/test_launcher.py     (from repo root)

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
        "variants": [{"id": "107057914",
                      "gfn": {"status": "AVAILABLE",
                              "library": {"status": "NOT_OWNED"}}},
                     {"id": "notnumeric", "gfn": {}}],
        "gfn": {"playabilityState": "PLAYABLE",
                "minimumMembershipTierLabel": "PAID"}}
g = catalog.normalize(ITEM)
check(g["app_ids"] == [107057914], "catalog: numeric variant id = appId")
check(g["owned"] is False and g["tier"] == "PAID",
      "catalog: NOT_OWNED + tier label")
ITEM2 = json.loads(json.dumps(ITEM))
ITEM2["variants"][0]["gfn"]["library"]["status"] = "OWNED"
check(catalog.normalize(ITEM2)["owned"] is True, "catalog: OWNED variant")
check(catalog.normalize(ITEM, {"uuid-1"})["owned"] is True,
      "catalog: library-query membership marks owned")


def _gql_handler(request: httpx.Request) -> httpx.Response:
    body = json.loads(request.content)
    variables = body["variables"]
    owned_only = variables["filters"] == catalog.LIBRARY_FILTERS
    if owned_only:
        items = [ITEM2]
        has_next, cursor, total = False, "", 1
    else:
        items = [ITEM, ITEM2,
                 {"id": "uuid-2", "title": "Fortnite", "variants": [],
                  "gfn": {}}]
        has_next, cursor, total = False, "", 3
    return httpx.Response(200, json={"data": {"apps": {
        "numberReturned": len(items),
        "pageInfo": {"hasNextPage": has_next, "endCursor": cursor,
                     "totalCount": total},
        "items": items}}})


with httpx.Client(transport=httpx.MockTransport(_gql_handler)) as c:
    games = catalog.fetch_games("TOK", "VPC-9", client=c)
check(len(games) == 2, "catalog: browse ∪ library dedup (2 unique)")
by_title = {g["title"]: g for g in games}
check(by_title["Arknights: Endfield"]["owned"] is True and
      by_title["Fortnite"]["owned"] is False,
      "catalog: ownership flags after the merge")
check(games[0]["owned"] is True, "catalog: owned sort first")

# ----------------------------------------------------------------- GUI
try:
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from kn_launcher import gui  # noqa: F401
    from PyQt5.QtWidgets import QApplication
    app = QApplication.instance() or QApplication([])
    win = gui.MainWindow()
    check(win.tabs.count() == 2 and not win.tabs.isTabEnabled(1),
          "gui: two tabs, tab 2 locked before login")
    win.games = GAMES
    win.tabs.setTabEnabled(1, True)
    win._render_games()
    check(win.game_list.count() == len(GAMES),
          "gui: game list renders")
    win.search_box.setText("endfield")
    check(win.game_list.count() >= 1 and
          "Arknights: Endfield" in win.game_list.item(0).text(),
          "gui: smart search in the list ('endfield' first)")
    win.search_box.setText("")
    import re as _re
    check(_re.match(r"^run\d+\.log$", win.logname.text()),
          "gui: runNNN.log auto-suggestion")
    check(gui.DEFAULT_PARAMS.startswith("--session-seconds=1200"),
          "gui: the operator's default parameter block")
    check("--session-token" not in gui.DEFAULT_PARAMS,
          "gui: the token is NEVER part of the typed parameters")
    win.close()
except ImportError as exc:
    print("(gui smoke skipped: %s)" % exc)

print("kn_launcher tests: %d checks, %d failures -> %s" %
      (CHECKS, FAILS, "PASS" if not FAILS else "FAIL"))
sys.exit(1 if FAILS else 0)
