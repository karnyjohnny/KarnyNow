# auth.py — the OAuth 2.0 DEVICE grant (RFC 8628) for GeForce NOW.
#
# Every endpoint, constant and header set is transcribed from
# docs/AUTH.md (OpenNOW gfn.rs provenance, round XXI):
#   * device authorize: POST https://login.nvidia.com/device/authorize
#     form: client_id, scope, device_id, display_name, idp_id
#     identity: the Steam-Deck BROWSER client (AUTH.md §2 note — the
#     device flow needs no embedded browser).
#   * token poll: POST https://login.nvidia.com/token with
#     grant_type=urn:ietf:params:oauth:grant-type:device_code
#     states: authorization_pending / slow_down(+5s, cap 3600) /
#             expired_token / access_denied
#   * refresh: grant_type=refresh_token (AUTH.md §3)
#   * verify + vpcId: GET {cm_base}v2/serverInfo with GFNJWT — the
#     reference's region discovery (gfn.rs provider_regions);
#     requestStatus.serverId IS the vpcId (gfn.rs verified_vpc).
#   * service token = id_token when present, else access_token
#     (AUTH.md §3, gfn.rs service_token :254-256).
#
# The exact user_code/verification_uri field names in the device-
# authorize RESPONSE were flagged NOT VERIFIED in AUTH.md §5 — the
# code below accepts every common spelling and the GUI shows the raw
# payload keys when the known ones are missing, so the first live run
# settles it without a code change.

from __future__ import annotations

import base64
import json
import time

import httpx

from . import store

# --- public constants (docs/AUTH.md §1 — NOT secrets) -------------------
DEVICE_AUTHORIZE_URL = "https://login.nvidia.com/device/authorize"
TOKEN_URL = "https://login.nvidia.com/token"
USERINFO_URL = "https://login.nvidia.com/userinfo"
SERVICE_URLS = "https://pcs.geforcenow.com/v1/serviceUrls"
CM_BASE = "https://prod.cloudmatchbeta.nvidiagrid.net/"
GRAPHQL_URL = "https://games.geforce.com/graphql"
PUBLIC_GRAPHQL_URL = "https://public.games.geforce.com/graphql"
MES_URL = "https://mes.geforcenow.com/v4/subscriptions"

DEFAULT_IDP_ID = "PDiAhv2kJTFeQ7WOPqiQ2tRZ7lGhR2X11dXvM4TZSxg"
STEAM_DECK_CLIENT_ID = "q61ddeJrVt7O90Nl-P-N7I36yctih4Ml6FyXLrb6j-U"
LCARS_CLIENT_ID = "ec7e38d4-03af-4b58-b131-cfb0495903ab"
SCOPES = "openid consent email tk_client age"
GFN_CLIENT_VERSION = "2.0.87.131"
STEAM_DECK_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                 "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 "
                 "Safari/537.36")

DEVICE_FORM_HEADERS = {
    "Accept": "application/json, text/plain, */*",
    "Origin": "https://play.geforcenow.com",
    "Referer": "https://play.geforcenow.com/",
    "User-Agent": STEAM_DECK_UA,
    "nv-client-id": STEAM_DECK_CLIENT_ID,
    "nv-client-streamer": "WEBRTC",
    "nv-client-type": "BROWSER",
    "nv-client-platform-name": "browser",
    "nv-browser-type": "CHROME",
    "nv-device-os": "STEAMOS",
    "nv-device-type": "CONSOLE",
    "nv-device-model": "STEAMDECK",
    "nv-device-make": "VALVE",
}

PENDING = "authorization_pending"
SLOW_DOWN = "slow_down"
EXPIRED = "expired_token"
DENIED = "access_denied"


def _client(transport=None, timeout=20.0) -> httpx.Client:
    return httpx.Client(timeout=timeout, transport=transport,
                        follow_redirects=True)


# ------------------------------------------------------------------ JWT
def jwt_claims(token: str) -> dict:
    """Decode (NOT verify — display/expiry only) a JWT's payload."""
    try:
        parts = token.split(".")
        if len(parts) < 2:
            return {}
        pad = parts[1] + "=" * (-len(parts[1]) % 4)
        return json.loads(base64.urlsafe_b64decode(pad).decode("utf-8"))
    except Exception:
        return {}


def service_token(bundle: dict) -> str:
    """AUTH.md §3: the CloudMatch/GraphQL credential is the id_token
    when present, else the access_token."""
    return bundle.get("id_token") or bundle.get("access_token") or ""


# ------------------------------------------------------- device authorize
def device_authorize(client: httpx.Client | None = None,
                     display_name: str = "KarnyNow") -> dict:
    """POST the device-authorize form. Returns the raw JSON dict:
    device_code + the user-facing code/URI + interval (field names per
    AUTH.md §5 are measured live — see module header)."""
    own = client is None
    client = client or _client()
    try:
        form = {
            "client_id": STEAM_DECK_CLIENT_ID,
            "scope": SCOPES,
            "device_id": store.device_id(),
            "display_name": display_name,
            "idp_id": DEFAULT_IDP_ID,
        }
        headers = dict(DEVICE_FORM_HEADERS)
        headers["x-device-id"] = form["device_id"]
        headers["Content-Type"] = \
            "application/x-www-form-urlencoded; charset=UTF-8"
        r = client.post(DEVICE_AUTHORIZE_URL, data=form, headers=headers)
        r.raise_for_status()
        return r.json()
    finally:
        if own:
            client.close()


def verification_urls(resp: dict) -> tuple[str, str]:
    """(url_to_show, user_code) from a device-authorize response —
    tolerating the unverified field names (AUTH.md §5). The URL may
    already embed the PIN (verification_uri_complete)."""
    url = (resp.get("verification_uri_complete")
           or resp.get("verificationUriComplete")
           or resp.get("verification_uri")
           or resp.get("verificationUri") or "")
    code = str(resp.get("user_code") or resp.get("userCode")
               or resp.get("code") or "")
    if url and code and code not in url:
        sep = "&" if "?" in url else "?"
        # common NVIDIA activation pattern; harmless if the page ignores
        # the parameter — the code is shown separately either way
        url = url + sep + "code=" + code
    return url, code


def poll_token(device_code: str, client: httpx.Client | None = None) -> dict:
    """One token-poll POST. Returns {"state": ..., ...}:
    state="ok" carries the token bundle fields; pending states carry
    their OAuth error verbatim."""
    own = client is None
    client = client or _client()
    try:
        form = {
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "device_code": device_code,
            "client_id": STEAM_DECK_CLIENT_ID,
        }
        headers = dict(DEVICE_FORM_HEADERS)
        headers["Content-Type"] = \
            "application/x-www-form-urlencoded; charset=UTF-8"
        r = client.post(TOKEN_URL, data=form, headers=headers)
        try:
            payload = r.json()
        except ValueError:
            return {"state": "http_error", "status": r.status_code}
        err = payload.get("error") or ""
        if r.status_code == 200 and payload.get("access_token"):
            return {"state": "ok", "tokens": payload}
        if err in (PENDING, SLOW_DOWN, EXPIRED, DENIED):
            return {"state": err}
        return {"state": "error", "error": err or ("http %d" % r.status_code),
                "detail": {k: v for k, v in payload.items()
                           if k != "access_token"}}
    finally:
        if own:
            client.close()


def refresh(refresh_token: str, client: httpx.Client | None = None) -> dict:
    """grant_type=refresh_token (AUTH.md §3). Same {"state":...} shape."""
    own = client is None
    client = client or _client()
    try:
        form = {
            "grant_type": "refresh_token",
            "refresh_token": refresh_token,
            "client_id": STEAM_DECK_CLIENT_ID,
        }
        headers = dict(DEVICE_FORM_HEADERS)
        headers["Content-Type"] = \
            "application/x-www-form-urlencoded; charset=UTF-8"
        r = client.post(TOKEN_URL, data=form, headers=headers)
        try:
            payload = r.json()
        except ValueError:
            return {"state": "http_error", "status": r.status_code}
        if r.status_code == 200 and payload.get("access_token"):
            return {"state": "ok", "tokens": payload}
        err = payload.get("error") or ""
        if err in ("invalid_grant", "invalid_token", "token_revoked"):
            return {"state": "relogin"}     # AUTH.md §3: force re-login
        return {"state": "error", "error": err or ("http %d" % r.status_code)}
    finally:
        if own:
            client.close()


# ---------------------------------------------------------- bundle build
def make_bundle(tokens: dict) -> dict:
    """Token response -> the vault bundle (expiry from the JWT when the
    response omits expires_in)."""
    bundle = {
        "access_token": tokens.get("access_token", ""),
        "refresh_token": tokens.get("refresh_token", ""),
        "id_token": tokens.get("id_token", ""),
        "client_token": tokens.get("client_token", ""),
        "saved_at": int(time.time()),
    }
    claims = jwt_claims(bundle["id_token"] or bundle["access_token"])
    exp = tokens.get("expires_in")
    if exp:
        bundle["expires_at"] = int(time.time()) + int(exp)
    elif claims.get("exp"):
        bundle["expires_at"] = int(claims["exp"])
    else:
        bundle["expires_at"] = 0
    user = {}
    if claims.get("email"):
        user["email"] = claims["email"]
    if claims.get("sub"):
        user["sub_hash"] = store.sub_hash(str(claims["sub"]))
    bundle["user"] = user
    return bundle


# -------------------------------------------------------------- verify
CM_BROWSER_HEADERS = {
    "Accept": "application/json, text/plain, */*",
    "User-Agent": STEAM_DECK_UA,
    "nv-client-id": LCARS_CLIENT_ID,
    "nv-client-streamer": "WEBRTC",
    "nv-client-type": "BROWSER",
    "nv-client-version": "2.0.89.154",
    "nv-browser-type": "CHROME",
    "nv-device-os": "WINDOWS",
    "nv-device-type": "DESKTOP",
    "nv-device-make": "UNKNOWN",
    "nv-device-model": "UNKNOWN",
    "Origin": "https://play.geforcenow.com",
    "Referer": "https://play.geforcenow.com/",
}


def verify_and_vpc(bundle: dict, client: httpx.Client | None = None):
    """The live-token check AND the vpcId discovery in one call:
    GET {CM_BASE}v2/serverInfo with GFNJWT (the reference's region
    discovery). Returns (ok: bool, vpc_id_or_reason: str)."""
    own = client is None
    client = client or _client()
    try:
        token = service_token(bundle)
        if not token:
            return False, "no token in the vault"
        headers = dict(CM_BROWSER_HEADERS)
        headers["Authorization"] = "GFNJWT " + token
        headers["x-device-id"] = store.device_id()
        r = client.get(CM_BASE + "v2/serverInfo", headers=headers)
        if r.status_code in (401, 403):
            return False, "token rejected (%d)" % r.status_code
        r.raise_for_status()
        payload = r.json()
        rs = payload.get("requestStatus") or {}
        code = rs.get("statusCode")
        if code is not None and int(code) != 1:
            return False, "serverInfo status %s" % code
        vpc = rs.get("serverId") or ""
        if not vpc:
            return False, "serverInfo without serverId"
        return True, str(vpc)
    except httpx.HTTPError as exc:
        return False, "network: %s" % exc
    finally:
        if own:
            client.close()


def ensure_fresh(bundle: dict, client: httpx.Client | None = None):
    """Refresh when inside the 10-min window (AUTH.md §3). Returns
    (bundle, changed: bool). A refresh failure keeps the old bundle —
    verify_and_vpc is the authority on liveness."""
    if not store.bundle_expired(bundle, skew_s=600):
        return bundle, False
    rt = bundle.get("refresh_token")
    if not rt:
        return bundle, False
    res = refresh(rt, client=client)
    if res.get("state") != "ok":
        return bundle, False
    fresh = make_bundle(res["tokens"])
    # a refresh response may omit the refresh token — carry the old one
    if not fresh.get("refresh_token"):
        fresh["refresh_token"] = rt
    if not fresh.get("id_token"):
        fresh["id_token"] = bundle.get("id_token", "")
    fresh["user"] = bundle.get("user") or fresh.get("user") or {}
    return fresh, True
