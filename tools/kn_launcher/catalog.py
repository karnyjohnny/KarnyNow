# catalog.py — the GFN game catalog over GraphQL.
#
# Queries transcribed (minimized) from OpenNOW gfn/catalog.rs:
#   * vpcId comes from auth.verify_and_vpc (v2/serverInfo serverId).
#   * browse-all: filters {} + sortString
#     "itemMetadata.relevance:DESC,sortName:ASC"   (store_catalog)
#   * owned library: filters {"variants":{"gfn":{"library":
#     {"status":{"notEquals":"NOT_OWNED"}}}}} + the library sortString
#     (library_catalog :413).
#   * SEARCH: the SAME apps field with `searchQuery: $searchString`
#     (STORE_SEARCH_QUERY, catalog.rs:133-163). This is the mechanism
#     the web app and the reference both use — the server matches, so a
#     title can be found WITHOUT the client holding all ~6200 of them.
#   * the NUMERIC launch id for knvstd --session-app is variants[].id
#     (store_launch.rs launch_id, parse::<i32>-validated) — NOT the
#     catalog UUID.
# Headers: graphql_headers (gfn.rs :3599) = GFNJWT + the LCARS identity
# + application/json + play.geforcenow.com origin/referer.
#
# Round N slice 4q — THE RESPONSIVENESS REWRITE. The operator's report:
# "Pobieranie 6220 trwa za długo". The old fetch_games() walked the
# whole browse catalog (62 pages) before the list could be painted at
# all. The catalog is now THREE separate operations, cheapest first:
#   fetch_library()  — the account's OWNED titles only (the operator's
#                      "może ograniczenie zapytania realnie tylko do
#                      tych które mam"): one filtered query, a handful
#                      of pages, <1 s. This is the DEFAULT.
#   iter_browse()    — a GENERATOR over the full catalog, one page per
#                      yield, so the GUI can paint page 1 immediately
#                      and keep appending while the rest downloads.
#                      Behind the "full catalog" checkbox.
#   search_server()  — server-side `searchQuery`: finds any of the
#                      ~6200 titles in one round trip with nothing
#                      cached locally.
# Plus the disk cache in store.py, so the NEXT startup paints instantly
# with zero network.

from __future__ import annotations

import httpx

from . import auth

# The item selection, shared by browse/library/search (OpenNOW asks for
# the same three shapes and only the arguments differ). `variants` now
# carries appStore + shortName + library.selected — the fields the
# Steam/Epic picker and the launch-id rule need.
_ITEMS = """    items {
      id title developerName publisherName
      variants {
        id shortName appStore
        gfn { status library { status selected } }
      }
      gfn { playabilityState minimumMembershipTierLabel playType }
    }"""

_PAGEINFO = "numberReturned pageInfo { hasNextPage endCursor totalCount }"

BROWSE_QUERY = """query KnBrowseApps(
  $vpcId: String!, $locale: String!, $sortString: String!,
  $fetchCount: Int!, $cursor: String!, $filters: AppFilterFields!
) {
  apps(vpcId: $vpcId, language: $locale, orderBy: $sortString,
       first: $fetchCount, after: $cursor, filters: $filters) {
    %s
%s
  }
}""" % (_PAGEINFO, _ITEMS)

SEARCH_QUERY = """query KnSearchApps(
  $vpcId: String!, $locale: String!, $sortString: String!,
  $fetchCount: Int!, $cursor: String!, $searchString: String!,
  $filters: AppFilterFields!
) {
  apps(vpcId: $vpcId, language: $locale, orderBy: $sortString,
       first: $fetchCount, after: $cursor, searchQuery: $searchString,
       filters: $filters) {
    %s
%s
  }
}""" % (_PAGEINFO, _ITEMS)

BROWSE_SORT = "itemMetadata.relevance:DESC,sortName:ASC"
LIBRARY_SORT = ("variants.gfn.library.lastPlayedDate:DESC,"
                "computedValues.libraryAddedDate:DESC,sortName:ASC")
LIBRARY_FILTERS = {"variants": {"gfn": {"library": {
    "status": {"notEquals": "NOT_OWNED"}}}}}
PAGE = 100
MAX_PAGES = 60            # bounds a pathological cursor loop (6000 apps)
SEARCH_LIMIT = 40         # one server round trip; the GUI caps anyway

# OpenNOW catalog.rs:821 — the library statuses that mean "the account
# actually has this variant" (NOT_OWNED / null mean it does not).
IN_LIBRARY_STATUSES = ("MANUAL", "PLATFORM_SYNC", "IN_LIBRARY")


def graphql_headers(token: str) -> dict:
    h = dict(auth.CM_BROWSER_HEADERS)
    h["Authorization"] = "GFNJWT " + token
    h["Content-Type"] = "application/json"
    h["nv-client-type"] = "NATIVE"          # graphql_headers (gfn.rs:3599)
    h["nv-client-streamer"] = "NVIDIA-CLASSIC"
    return h


def _apps(client: httpx.Client, token: str, query: str,
          variables: dict) -> dict:
    r = client.post(auth.GRAPHQL_URL, headers=graphql_headers(token),
                    json={"query": query, "variables": variables})
    r.raise_for_status()
    payload = r.json()
    if payload.get("errors"):
        raise RuntimeError("graphql: %s" % payload["errors"][0]
                           .get("message", "error"))
    return (payload.get("data") or {}).get("apps") or {}


def _page(client: httpx.Client, token: str, vpc_id: str, sort: str,
          filters: dict, cursor: str) -> dict:
    return _apps(client, token, BROWSE_QUERY,
                 {"vpcId": vpc_id, "locale": "en_US", "sortString": sort,
                  "fetchCount": PAGE, "cursor": cursor, "filters": filters})


def _walk(client, token, vpc_id, sort, filters, progress=None) -> list[dict]:
    out, cursor, pages = [], "", 0
    while pages < MAX_PAGES:
        apps = _page(client, token, vpc_id, sort, filters, cursor)
        items = apps.get("items") or []
        out.extend(items)
        if progress:
            progress(len(out), (apps.get("pageInfo") or {})
                     .get("totalCount"))
        pi = apps.get("pageInfo") or {}
        pages += 1
        if not pi.get("hasNextPage") or not pi.get("endCursor"):
            break
        cursor = pi["endCursor"]
    return out


# ------------------------------------------------------------ variants
def _numeric_id(raw) -> int:
    """variants[].id -> the int knvstd --session-app wants, or 0."""
    s = str(raw or "").strip()
    return int(s) if s.isdigit() and int(s) > 0 else 0


def variant_records(item: dict) -> list[dict]:
    """Raw GraphQL item -> the launcher's variant list, IN SERVER ORDER.

    One GFN title can be offered from several stores (the operator's War
    Thunder example: GAIJIN 10839111 free-to-play next to STEAM
    100234911 PLATFORM_SYNC), and each is a DIFFERENT numeric appId —
    so the launcher must let the operator choose which one flies.
    Fields mirror OpenNOW catalog.rs:817-848.
    """
    out, seen = [], set()
    for v in item.get("variants") or []:
        vid = str(v.get("id") or "")
        if not vid or vid in seen:            # reference dedupes by id
            continue
        seen.add(vid)
        gfn = v.get("gfn") or {}
        lib = gfn.get("library") or {}
        status = lib.get("status") or ""
        out.append({
            "id": vid,
            "app_id": _numeric_id(vid),
            "store": v.get("appStore") or "UNKNOWN",
            "short_name": v.get("shortName") or "",
            "selected": bool(lib.get("selected")),
            "status": status,
            "in_library": status in IN_LIBRARY_STATUSES,
            "gfn_status": gfn.get("status") or "",
        })
    return out


def pick_variant_index(variants: list) -> int:
    """OpenNOW catalog.rs:854-864 verbatim: the variant the WEB APP
    would launch — first `library.selected == true`, else first
    in-library, else 0. The launcher pre-selects this one so the
    operator's default flight matches the browser's."""
    for i, v in enumerate(variants):
        if v.get("selected"):
            return i
    for i, v in enumerate(variants):
        if v.get("in_library"):
            return i
    return 0


def launch_app_id(variants: list, uuid_id: str = "") -> int:
    """OpenNOW catalog.rs:865-880 (launch_id): the SELECTED variant's id
    when it is all-digits, else the first all-digit variant id, else the
    app id itself when that is numeric, else 0 (= unlaunchable)."""
    if variants:
        pick = variants[pick_variant_index(variants)]
        if pick["app_id"]:
            return pick["app_id"]
        for v in variants:
            if v["app_id"]:
                return v["app_id"]
    s = str(uuid_id or "").strip()
    return int(s) if s.isdigit() and int(s) > 0 else 0


def _numeric_app_ids(item: dict) -> list[int]:
    ids = []
    for v in item.get("variants") or []:
        aid = _numeric_id(v.get("id"))
        if aid and aid not in ids:
            ids.append(aid)
    return ids


def _owned_by_variants(item: dict) -> bool:
    for v in item.get("variants") or []:
        st = ((v.get("gfn") or {}).get("library") or {}).get("status")
        if st and st != "NOT_OWNED":
            return True
    return False


def normalize(item: dict, owned_ids: set | None = None) -> dict:
    """Raw GraphQL item -> the launcher's flat game record."""
    uuid_id = item.get("id") or ""
    owned = _owned_by_variants(item)
    if owned_ids is not None and uuid_id in owned_ids:
        owned = True
    gfn = item.get("gfn") or {}
    variants = variant_records(item)
    return {
        "id": uuid_id,
        "title": item.get("title") or "(untitled)",
        "developer": item.get("developerName") or "",
        "publisher": item.get("publisherName") or "",
        "owned": owned,
        "tier": gfn.get("minimumMembershipTierLabel") or "",
        "playability": gfn.get("playabilityState") or "",
        "app_ids": _numeric_app_ids(item),
        # slice 4q: the store variants + the pre-picked launch id
        "variants": variants,
        "variant_index": pick_variant_index(variants) if variants else -1,
        "default_app_id": launch_app_id(variants, uuid_id),
    }


def merge_records(dst: dict, src: dict) -> dict:
    """Fold a later record into an earlier one for the same UUID.

    The library query and the browse query return the same title with
    DIFFERENT field coverage (browse carries playability/tier, library
    carries the authoritative selected/in-library flags), so neither
    alone is right. Union of app_ids; variants are replaced only when
    the incoming record has them and the kept one does not; ownership
    and tier are OR-ed/filled. Returns dst (mutated in place, so the
    caller's list order is preserved).
    """
    dst["owned"] = bool(dst.get("owned")) or bool(src.get("owned"))
    if not dst.get("tier"):
        dst["tier"] = src.get("tier") or ""
    if not dst.get("playability"):
        dst["playability"] = src.get("playability") or ""
    if not dst.get("developer"):
        dst["developer"] = src.get("developer") or ""
    for aid in src.get("app_ids") or []:
        if aid not in dst["app_ids"]:
            dst["app_ids"].append(aid)
    if src.get("variants") and not dst.get("variants"):
        dst["variants"] = src["variants"]
    if dst.get("variants"):
        dst["variant_index"] = pick_variant_index(dst["variants"])
        dst["default_app_id"] = launch_app_id(dst["variants"],
                                              dst.get("id", ""))
    elif src.get("default_app_id") and not dst.get("default_app_id"):
        dst["default_app_id"] = src["default_app_id"]
    return dst


def sort_games(games: list) -> list:
    """Owned first, then A-Z (the launcher's list order)."""
    games.sort(key=lambda g: (not g.get("owned"),
                              (g.get("title") or "").lower()))
    return games


# ------------------------------------------------- the three operations
def fetch_library(token: str, vpc_id: str, progress=None,
                  client: httpx.Client | None = None) -> list[dict]:
    """DEFAULT path: ONLY the account's owned titles ("tylko te, które
    mam"). One filtered query, a handful of pages, <1 s — versus the
    62-page full walk that made startup feel hung."""
    own = client is None
    client = client or auth._client(timeout=30.0)
    try:
        lib = _walk(client, token, vpc_id, LIBRARY_SORT, LIBRARY_FILTERS,
                    progress=progress)
        owned_ids = {(i.get("id") or "") for i in lib}
        return sort_games([normalize(i, owned_ids) for i in lib])
    finally:
        if own:
            client.close()


def iter_browse(token: str, vpc_id: str, owned_ids=None,
                client: httpx.Client | None = None):
    """GENERATOR over the full catalog, one page per yield.

    Yields (records, total) where `records` are the NORMALIZED items of
    the page just fetched and `total` is pageInfo.totalCount (None until
    the server says). The GUI paints page 1 the moment it lands and
    keeps appending — the operator watches the list grow instead of
    watching a spinner for a minute.
    """
    own = client is None
    client = client or auth._client(timeout=30.0)
    try:
        cursor, pages = "", 0
        while pages < MAX_PAGES:
            apps = _page(client, token, vpc_id, BROWSE_SORT, {}, cursor)
            items = apps.get("items") or []
            pi = apps.get("pageInfo") or {}
            yield [normalize(i, owned_ids) for i in items], \
                pi.get("totalCount")
            pages += 1
            if not pi.get("hasNextPage") or not pi.get("endCursor"):
                break
            cursor = pi["endCursor"]
    finally:
        if own:
            client.close()


def search_server(token: str, vpc_id: str, text: str,
                  limit: int = SEARCH_LIMIT,
                  owned_ids=None,
                  client: httpx.Client | None = None) -> list[dict]:
    """Server-side title search (`searchQuery`) — finds any of the
    ~6200 catalog titles in ONE round trip, with nothing cached
    locally. This is why the launcher no longer needs the full catalog
    resident to be useful."""
    q = (text or "").strip()
    if not q:
        return []
    own = client is None
    client = client or auth._client(timeout=20.0)
    try:
        apps = _apps(client, token, SEARCH_QUERY,
                     {"vpcId": vpc_id, "locale": "en_US",
                      "sortString": BROWSE_SORT, "fetchCount": limit,
                      "cursor": "", "searchString": q, "filters": {}})
        return [normalize(i, owned_ids) for i in (apps.get("items") or [])]
    finally:
        if own:
            client.close()


def fetch_games(token: str, vpc_id: str, progress=None,
                client: httpx.Client | None = None) -> list[dict]:
    """The FULL browse catalog with ownership marked (the "full catalog"
    checkbox / the one-shot path kept for scripts and tests). The
    owned-library query runs FIRST (small) so ownership survives even
    if the browse pagination dies halfway; browse failures then fall
    back to the owned-only list (degraded but useful)."""
    own = client is None
    client = client or auth._client(timeout=30.0)
    try:
        lib = _walk(client, token, vpc_id, LIBRARY_SORT, LIBRARY_FILTERS,
                    progress=None)
        owned_ids = {(i.get("id") or "") for i in lib}
        games = [normalize(i, owned_ids) for i in lib]
        try:
            browse = _walk(client, token, vpc_id, BROWSE_SORT, {},
                           progress=progress)
        except Exception as exc:
            if progress:
                progress(-1, "browse failed (%s) — showing the owned "
                             "library only" % exc)
            return games
        by_id = {g["id"]: g for g in games}
        for item in browse:
            g = normalize(item, owned_ids)
            if g["id"] in by_id:
                merge_records(by_id[g["id"]], g)
            else:
                by_id[g["id"]] = g
                games.append(g)
        return sort_games(games)
    finally:
        if own:
            client.close()
