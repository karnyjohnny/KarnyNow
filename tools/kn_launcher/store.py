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
