# catalog.py — the GFN game catalog over GraphQL.
#
# Queries transcribed (minimized) from OpenNOW gfn/catalog.rs:
#   * vpcId comes from auth.verify_and_vpc (v2/serverInfo serverId).
#   * browse-all: filters {} + sortString
#     "itemMetadata.relevance:DESC,sortName:ASC"   (store_catalog)
#   * owned library: filters {"variants":{"gfn":{"library":
#     {"status":{"notEquals":"NOT_OWNED"}}}}} + the library sortString
#     (library_catalog :413).
#   * the NUMERIC launch id for knvstd --session-app is variants[].id
#     (store_launch.rs launch_id, parse::<i32>-validated) — NOT the
#     catalog UUID.
# Headers: graphql_headers (gfn.rs :3599) = GFNJWT + the LCARS identity
# + application/json + play.geforcenow.com origin/referer.

import httpx

from . import auth

BROWSE_QUERY = """query KnBrowseApps(
  $vpcId: String!, $locale: String!, $sortString: String!,
  $fetchCount: Int!, $cursor: String!, $filters: AppFilterFields!
) {
  apps(vpcId: $vpcId, language: $locale, orderBy: $sortString,
       first: $fetchCount, after: $cursor, filters: $filters) {
    numberReturned
    pageInfo { hasNextPage endCursor totalCount }
    items {
      id title developerName publisherName
      variants {
        id
        gfn { status library { status } }
      }
      gfn { playabilityState minimumMembershipTierLabel }
    }
  }
}"""

BROWSE_SORT = "itemMetadata.relevance:DESC,sortName:ASC"
LIBRARY_SORT = ("variants.gfn.library.lastPlayedDate:DESC,"
                "computedValues.libraryAddedDate:DESC,sortName:ASC")
LIBRARY_FILTERS = {"variants": {"gfn": {"library": {
    "status": {"notEquals": "NOT_OWNED"}}}}}
PAGE = 100
MAX_PAGES = 60            # bounds a pathological cursor loop (6000 apps)


def graphql_headers(token: str) -> dict:
    h = dict(auth.CM_BROWSER_HEADERS)
    h["Authorization"] = "GFNJWT " + token
    h["Content-Type"] = "application/json"
    h["nv-client-type"] = "NATIVE"          # graphql_headers (gfn.rs:3599)
    h["nv-client-streamer"] = "NVIDIA-CLASSIC"
    return h


def _page(client: httpx.Client, token: str, vpc_id: str, sort: str,
          filters: dict, cursor: str) -> dict:
    r = client.post(
        auth.GRAPHQL_URL,
        headers=graphql_headers(token),
        json={"query": BROWSE_QUERY,
              "variables": {"vpcId": vpc_id, "locale": "en_US",
                            "sortString": sort, "fetchCount": PAGE,
                            "cursor": cursor, "filters": filters}})
    r.raise_for_status()
    payload = r.json()
    if payload.get("errors"):
        raise RuntimeError("graphql: %s" % payload["errors"][0]
                           .get("message", "error"))
    return (payload.get("data") or {}).get("apps") or {}


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


def _numeric_app_ids(item: dict) -> list[int]:
    ids = []
    for v in item.get("variants") or []:
        vid = str(v.get("id") or "")
        if vid.isdigit() and int(vid) > 0:
            ids.append(int(vid))
    return ids


def _owned_by_variants(item: dict) -> bool:
    for v in item.get("variants") or []:
        lib = ((v.get("gfn") or {}).get("library") or {})
        st = lib.get("status")
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
    return {
        "id": uuid_id,
        "title": item.get("title") or "(untitled)",
        "developer": item.get("developerName") or "",
        "publisher": item.get("publisherName") or "",
        "owned": owned,
        "tier": gfn.get("minimumMembershipTierLabel") or "",
        "playability": gfn.get("playabilityState") or "",
        "app_ids": _numeric_app_ids(item),
    }


def fetch_games(token: str, vpc_id: str, progress=None,
                client: httpx.Client | None = None) -> list[dict]:
    """The full browse catalog with ownership marked. The owned-library
    query runs FIRST (small) so ownership survives even if the browse
    pagination dies halfway; browse failures then fall back to the
    owned-only list (degraded but useful)."""
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
                # merge: keep the library record, fill missing app_ids
                merged = by_id[g["id"]]
                merged["owned"] = merged["owned"] or g["owned"]
                for aid in g["app_ids"]:
                    if aid not in merged["app_ids"]:
                        merged["app_ids"].append(aid)
                if not merged["tier"]:
                    merged["tier"] = g["tier"]
            else:
                by_id[g["id"]] = g
                games.append(g)
        games.sort(key=lambda g: (not g["owned"], g["title"].lower()))
        return games
    finally:
        if own:
            client.close()
