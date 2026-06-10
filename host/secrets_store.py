"""Tiny encrypted secret store for the companion (Windows DPAPI, stdlib only).

Secrets (e.g. the Shelly password) are encrypted with CryptProtectData, which
ties the ciphertext to the current Windows user account — the blob on disk is
not plaintext and cannot be decrypted by another user or on another machine.
No third-party dependency. On non-Windows or any failure, get/set are no-ops.
"""
import ctypes
import ctypes.wintypes as wt
import json
import pathlib
import sys

_STORE = pathlib.Path.home() / ".buttonbox_companion.dat"


class _DATA_BLOB(ctypes.Structure):
    _fields_ = [("cbData", wt.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]


def _blob(data: bytes) -> _DATA_BLOB:
    buf = ctypes.create_string_buffer(data, len(data))
    return _DATA_BLOB(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))


def _blob_bytes(blob: _DATA_BLOB) -> bytes:
    return ctypes.string_at(blob.pbData, blob.cbData)


def _protect(plaintext: str) -> bytes | None:
    if not sys.platform.startswith("win"):
        return None
    out = _DATA_BLOB()
    if not ctypes.windll.crypt32.CryptProtectData(
            ctypes.byref(_blob(plaintext.encode("utf-8"))),
            None, None, None, None, 0, ctypes.byref(out)):
        return None
    try:
        return _blob_bytes(out)
    finally:
        ctypes.windll.kernel32.LocalFree(out.pbData)


def _unprotect(cipher: bytes) -> str | None:
    if not sys.platform.startswith("win"):
        return None
    out = _DATA_BLOB()
    if not ctypes.windll.crypt32.CryptUnprotectData(
            ctypes.byref(_blob(cipher)),
            None, None, None, None, 0, ctypes.byref(out)):
        return None
    try:
        return _blob_bytes(out).decode("utf-8", "replace")
    finally:
        ctypes.windll.kernel32.LocalFree(out.pbData)


def _read_all() -> dict:
    try:
        return json.loads(_STORE.read_text())
    except Exception:
        return {}


def set_secret(key: str, value: str) -> None:
    """Encrypt and persist a secret under `key`. Silently no-ops on failure."""
    cipher = _protect(value)
    if cipher is None:
        return
    d = _read_all()
    d[key] = cipher.hex()
    try:
        _STORE.write_text(json.dumps(d))
    except Exception:
        pass


def get_secret(key: str) -> str | None:
    """Return the decrypted secret for `key`, or None if absent/undecryptable."""
    hexed = _read_all().get(key)
    if not hexed:
        return None
    try:
        return _unprotect(bytes.fromhex(hexed))
    except Exception:
        return None
