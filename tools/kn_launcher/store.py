# store.py — the credential vault.
#
# Windows: the token bundle is sealed with DPAPI (CryptProtectData,
# CRYPTPROTECT_UI_FORBIDDEN) — bound to the Windows user, the same
# mechanism docs/AUTH.md §5 picked for stage-4. No plaintext ever hits
# the disk.
# Other platforms (dev sandboxes): a 0600 file, WITH a warning flag the
# GUI surfaces — this tool is meant to run on the rig.
#
# The vault JSON shape:
#   {"access_token":..., "refresh_token":..., "id_token":...,
#    "client_token":..., "expires_at":<unix s>, "saved_at":<unix s>,
#    "user": {"email":..., "sub_hash":...}}
# `sub_hash` is a SHA-256 PREFIX of the account sub claim — enough to
# detect an account switch, never the claim itself (run-46 lesson).

from __future__ import annotations

import ctypes
import hashlib
import json
import os
import platform
import stat
import sys
import time
import uuid

APP_DIR_NAME = "KarnyNow"


def app_dir() -> str:
    """The per-user launcher directory (%APPDATA%/KarnyNow or XDG)."""
    if platform.system() == "Windows":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or \
            os.path.join(os.path.expanduser("~"), ".config")
    d = os.path.join(base, APP_DIR_NAME)
    os.makedirs(d, exist_ok=True)
    return d


def vault_path() -> str:
    return os.path.join(app_dir(), "vault.bin")


def device_id_path() -> str:
    return os.path.join(app_dir(), "device_id.txt")


def is_dpapi() -> bool:
    return platform.system() == "Windows"


# --------------------------------------------------------------- DPAPI
class _DATA_BLOB(ctypes.Structure):
    _fields_ = [("cbData", ctypes.c_uint32),
                ("pbData", ctypes.POINTER(ctypes.c_char))]


def _blob(data: bytes) -> _DATA_BLOB:
    buf = ctypes.create_string_buffer(data, len(data))
    return _DATA_BLOB(len(data), ctypes.cast(buf,
                                            ctypes.POINTER(ctypes.c_char)))


def _unblob(blob: _DATA_BLOB) -> bytes:
    data = ctypes.string_at(blob.pbData, blob.cbData)
    ctypes.windll.kernel32.LocalFree(blob.pbData)
    return data


def _dpapi_seal(plain: bytes) -> bytes:
    out = _DATA_BLOB()
    ok = ctypes.windll.crypt32.CryptProtectData(
        ctypes.byref(_blob(plain)), "KarnyNow vault", None, None, None,
        0x01,  # CRYPTPROTECT_UI_FORBIDDEN
        ctypes.byref(out))
    if not ok:
        raise OSError("CryptProtectData failed (%d)" %
                      ctypes.windll.kernel32.GetLastError())
    return _unblob(out)


def _dpapi_open(sealed: bytes) -> bytes:
    out = _DATA_BLOB()
    ok = ctypes.windll.crypt32.CryptUnprotectData(
        ctypes.byref(_blob(sealed)), None, None, None, None, 0x01,
        ctypes.byref(out))
    if not ok:
        raise OSError("CryptUnprotectData failed (%d)" %
                      ctypes.windll.kernel32.GetLastError())
    return _unblob(out)


# ---------------------------------------------------------------- vault
def save(bundle: dict) -> None:
    """Persist the token bundle (sealed on Windows, 0600 elsewhere)."""
    blob = json.dumps(bundle).encode("utf-8")
    path = vault_path()
    if is_dpapi():
        blob = _dpapi_seal(blob)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    if not is_dpapi():
        os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
    os.replace(tmp, path)


def load() -> dict | None:
    """The stored bundle, or None when absent/corrupt/unsealable."""
    path = vault_path()
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "rb") as f:
            blob = f.read()
        if is_dpapi():
            blob = _dpapi_open(blob)
        data = json.loads(blob.decode("utf-8"))
        return data if isinstance(data, dict) else None
    except Exception:
        return None


def wipe() -> None:
    for p in (vault_path(), vault_path() + ".tmp"):
        try:
            os.remove(p)
        except OSError:
            pass


def plaintext_warning() -> str | None:
    """Non-Windows fallback disclosure (the GUI shows it)."""
    if is_dpapi():
        return None
    return ("DPAPI unavailable on %s — the vault is a 0600 plaintext "
            "file (%s). Dev-only; run on the Windows rig for real use."
            % (platform.system(), vault_path()))


# ------------------------------------------------------------- device id
def device_id() -> str:
    """The per-install UUID (docs/AUTH.md §5: persisted, reused as the
    x-device-id AND as knvstd's deviceHashId family)."""
    path = device_id_path()
    try:
        with open(path, "r", encoding="utf-8") as f:
            value = f.read().strip()
        uuid.UUID(value)          # validate
        return value
    except Exception:
        pass
    value = str(uuid.uuid4())
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(value)
        if not is_dpapi():
            os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
    except OSError as exc:
        print("device_id persist failed: %s" % exc, file=sys.stderr)
    return value


# ---------------------------------------------------------------- helper
def sub_hash(sub: str) -> str:
    """Account-switch detector: a saltless SHA-256 prefix of the sub
    claim. The claim itself is never stored (run-46 lesson)."""
    return hashlib.sha256(("kn-launcher|" + sub).encode("utf-8")) \
                    .hexdigest()[:16]


def bundle_expiry(bundle: dict) -> float:
    """expires_at with a jwt-exp fallback; 0 when unknown."""
    exp = bundle.get("expires_at") or 0
    return float(exp)


def bundle_expired(bundle: dict, skew_s: int = 600) -> bool:
    exp = bundle_expiry(bundle)
    return bool(exp) and time.time() >= exp - skew_s


# ------------------------------------------------------- catalog cache
# Slice 4q: the operator's complaint was startup latency, not the
# network — "Pobieranie 6220 trwa za długo". The cheapest fix is to
# never wait on the network to PAINT: the last catalog is kept on disk
# and rendered the instant the tab opens, while a background refresh
# corrects it. The cache lives next to the vault (NOT in the repo) and
# holds no credentials — only public catalog metadata.
CACHE_NAME = "catalog_cache.json"
CACHE_SCHEMA = 1
CACHE_MAX_AGE_S = 7 * 24 * 3600        # a week; the catalog shifts slowly


def cache_path() -> str:
    return os.path.join(app_dir(), CACHE_NAME)


def save_catalog(games: list, vpc_id: str = "", kind: str = "library") -> bool:
    """Atomic write of the catalog cache. Returns False on any failure —
    a cache is an optimization and must never break the launcher."""
    payload = {"schema": CACHE_SCHEMA, "saved_at": time.time(),
               "vpc_id": vpc_id or "", "kind": kind or "library",
               "count": len(games or []), "games": games or []}
    path = cache_path()
    try:
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, separators=(",", ":"))
        if not is_dpapi():
            os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
        os.replace(tmp, path)
        return True
    except (OSError, TypeError, ValueError) as exc:
        print("catalog cache save failed: %s" % exc, file=sys.stderr)
        return False


def load_catalog() -> dict:
    """The cache envelope, or an empty one. NEVER raises and never
    returns None, so the caller can read .get("games") unconditionally
    — a corrupt/truncated cache must degrade to "empty list", not to a
    crash before the window exists (the F37 lesson)."""
    empty = {"schema": CACHE_SCHEMA, "saved_at": 0.0, "vpc_id": "",
             "kind": "", "count": 0, "games": []}
    path = cache_path()
    if not os.path.isfile(path):
        return empty
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict) or data.get("schema") != CACHE_SCHEMA:
            return empty
        games = data.get("games")
        if not isinstance(games, list):
            return empty
        data["games"] = [g for g in games if isinstance(g, dict)]
        data["count"] = len(data["games"])
        data.setdefault("saved_at", 0.0)
        return data
    except (OSError, ValueError):
        return empty


def cache_age_s(meta: dict) -> float:
    """Seconds since the cache was written; inf when it never was."""
    try:
        saved = float((meta or {}).get("saved_at") or 0.0)
    except (TypeError, ValueError):
        return float("inf")
    if saved <= 0:
        return float("inf")
    return max(0.0, time.time() - saved)


def cache_usable(meta: dict, max_age_s: float = CACHE_MAX_AGE_S) -> bool:
    """Paint-worthy: has games and is not ancient."""
    return bool((meta or {}).get("games")) and cache_age_s(meta) <= max_age_s


def cache_summary(meta: dict) -> str:
    """One human line for the status bar."""
    if not (meta or {}).get("games"):
        return "no cache yet"
    age = cache_age_s(meta)
    if age < 90:
        when = "%d s ago" % age
    elif age < 7200:
        when = "%d min ago" % (age / 60)
    elif age < 172800:
        when = "%d h ago" % (age / 3600)
    else:
        when = "%d d ago" % (age / 86400)
    return "%d games cached %s (%s)" % (meta.get("count", 0), when,
                                        meta.get("kind") or "?")


# --------------------------------------------------------- run folders
# Slice 4q: "cały czas ręcznie robie foldery typu run103_logs i sam
# przenoszę pliki z folderu logs, co jest bez sensu". The log FILE
# became a log FOLDER: one directory per run, one numbered subdirectory
# per DRAW, and the launcher puts everything for that draw inside it
# (knvstd's own logs via --log-dir, the wire dump, the console copy and
# a manifest). Archiving a run is now `zip run104_logs`, and a draw can
# never again end up with no logs (run103/3 did exactly that).
RUN_RE = r"^run(\d+)(?:_logs)?$"


def run_number(name: str) -> int:
    """104 from "run104_logs" or "run104.log"; -1 when not a run name."""
    import re as _re
    m = _re.match(RUN_RE, (name or "").strip())
    return int(m.group(1)) if m else -1


def suggest_run_name(logs_root) -> str:
    """The next free "runNNN_logs". Scans BOTH the new folder spelling
    and the legacy runNNN.log files, so an operator who numbered runs
    by hand up to 103 is offered 104, not 100."""
    best = 99
    try:
        entries = os.listdir(str(logs_root))
    except OSError:
        entries = []
    for name in entries:
        n = run_number(name)
        if n < 0 and name.endswith(".log"):
            n = run_number(name[:-4])
        if n > best:
            best = n
    return "run%d_logs" % (best + 1)


def valid_run_name(name: str) -> bool:
    """runNNN or runNNN_logs, digits only — this string becomes a
    directory name on Windows, so it is validated before use."""
    import re as _re
    return bool(_re.match(r"^run\d{1,4}(_logs)?$", (name or "").strip()))


def draw_dirs(logs_root, run_name: str) -> list:
    """The existing numeric draw subdirectories of a run, sorted."""
    root = os.path.join(str(logs_root), run_name)
    out = []
    try:
        entries = os.listdir(root)
    except OSError:
        return out
    for name in entries:
        if name.isdigit() and os.path.isdir(os.path.join(root, name)):
            out.append(int(name))
    out.sort()
    return out


def next_draw_index(logs_root, run_name: str) -> int:
    """The next free draw number inside a run folder (1-based)."""
    have = draw_dirs(logs_root, run_name)
    return (have[-1] + 1) if have else 1


def open_draw_dir(logs_root, run_name: str, index: int = 0):
    """Create logs/<run_name>/<index> and return (path, index).

    index=0 means "allocate the next free one", which is what a Launch
    click does — so draw 2 of run104 lands in logs/run104_logs/2 with
    no operator bookkeeping at all.
    """
    if index <= 0:
        index = next_draw_index(logs_root, run_name)
    path = os.path.join(str(logs_root), run_name, str(index))
    os.makedirs(path, exist_ok=True)
    return path, index


MANIFEST_ORDER = ("run", "draw", "started_at", "utc", "game_title",                  "game_uuid", "app_id", "app_store", "variant_short_name",
                  "owned", "device_id", "knvstd", "argv_redacted", "params",
                  "log_dir", "wire_dump", "console_log")


def write_manifest(path: str, fields: dict) -> bool:
    """manifest.txt — the draw's birth certificate: what was launched,
    when, which game AND WHICH STORE VARIANT, with which flags. Written
    BEFORE the flight starts so a crash still leaves the provenance
    behind. Unknown keys are appended sorted after the known ones."""
    try:
        lines = []
        keys = [k for k in MANIFEST_ORDER if k in fields]
        keys += sorted(k for k in fields if k not in MANIFEST_ORDER)
        for k in keys:
            v = fields[k]
            if isinstance(v, (list, tuple)):
                v = " ".join(str(x) for x in v)
            lines.append("%-20s %s" % (k + ":", v))
        with open(path, "w", encoding="utf-8") as f:
            f.write("# KarnyNow launcher — draw manifest (slice 4q)\n")
            f.write("\n".join(lines) + "\n")
        return True
    except OSError as exc:
        print("manifest write failed: %s" % exc, file=sys.stderr)
        return False


# ------------------------------------------------------- last run memory
# Slice 4r (F42): run 104 exposed the gap the log-folder rewrite left.
# The operator flew four draws across four launcher restarts and each
# restart suggested the NEXT run number, so the manifests read run104,
# run105, run106 and run107 — and the four folders had to be consolidated
# into run104_logs/1..4 BY HAND, which is exactly the bookkeeping the
# rewrite was supposed to kill. The launcher now remembers the run it
# last flew and continues it across restarts; a "new run" button is the
# only way to advance the number.
LAST_RUN_NAME = "last_run.txt"


def last_run_path() -> str:
    return os.path.join(app_dir(), LAST_RUN_NAME)


def save_last_run(name: str) -> bool:
    """Remember the run the launcher last flew (best effort)."""
    name = (name or "").strip()
    if not valid_run_name(name):
        return False
    try:
        with open(last_run_path(), "w", encoding="utf-8") as f:
            f.write(name)
        if not is_dpapi():
            os.chmod(last_run_path(), stat.S_IRUSR | stat.S_IWUSR)
        return True
    except OSError:
        return False


def load_last_run() -> str:
    """The remembered run name, or "" when absent/invalid."""
    try:
        with open(last_run_path(), "r", encoding="utf-8") as f:
            name = f.read().strip()
    except OSError:
        return ""
    return name if valid_run_name(name) else ""


def continuing_run(logs_root, current: str = "") -> str:
    """The run name a fresh launcher start should offer: the remembered
    one when it is still a valid name, else the next free number."""
    last = load_last_run()
    if last and last != (current or "").strip():
        return last
    if last:
        return last
    return suggest_run_name(logs_root)


def bump_run_name(logs_root, current: str) -> str:
    """The next run number that is NOT the current one — what the "new
    run" button does. suggest_run_name already skips past every existing
    runNNN_logs folder and legacy runNNN.log file."""
    nxt = suggest_run_name(logs_root)
    if nxt == (current or "").strip():
        n = run_number(nxt)
        nxt = "run%d_logs" % (n + 1 if n > 0 else 100)
    return nxt
