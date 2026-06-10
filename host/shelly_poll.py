"""Shelly Gen2 HTTP polling with SHA-256 Digest authentication (stdlib only).

Used by DeviceLink when wifiMode == WIFI_AUTO and the companion is connected.
The PC polls the Shelly device and pushes the state to the ESP32 over serial
so the box never needs to connect to WiFi itself when the companion is present.
"""
import hashlib
import json
import re
import time
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError


def _sha256hex(s: str) -> str:
    return hashlib.sha256(s.encode()).hexdigest()


def _parse_www_auth(header: str) -> dict:
    params: dict = {}
    for m in re.finditer(r'(\w+)=(?:"([^"]*)"|([\w/+.\-]+))', header):
        params[m.group(1)] = m.group(2) if m.group(2) is not None else m.group(3)
    return params


def _shelly_get(ip: str, path: str, user: str, password: str, timeout: float = 4.0) -> dict:
    """GET Shelly endpoint with SHA-256 Digest auth. Returns parsed JSON dict."""
    url = f"http://{ip}{path}"
    try:
        with urlopen(Request(url), timeout=timeout) as resp:
            return json.loads(resp.read().decode())
    except HTTPError as e:
        if e.code != 401:
            raise
        www_auth = e.headers.get("WWW-Authenticate", "")

    params = _parse_www_auth(www_auth)
    realm  = params.get("realm", "")
    nonce  = params.get("nonce", "")
    if not realm or not nonce:
        raise ValueError(f"Digest auth missing realm/nonce in: {www_auth!r}")

    ha1      = _sha256hex(f"{user}:{realm}:{password}")
    ha2      = _sha256hex(f"GET:{path}")
    nc       = "00000001"
    cnonce   = "ab12cd34"
    response = _sha256hex(f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}")

    auth_hdr = (
        f'Digest username="{user}", realm="{realm}", nonce="{nonce}", uri="{path}",'
        f' nc={nc}, cnonce="{cnonce}", qop=auth, response="{response}", algorithm=SHA-256'
    )
    req2 = Request(url, headers={"Authorization": auth_hdr})
    with urlopen(req2, timeout=timeout) as resp2:
        return json.loads(resp2.read().decode())


def get_status(ip: str, user: str, password: str) -> dict | None:
    """Return Switch.GetStatus as a normalized dict, or None on any failure."""
    try:
        data = _shelly_get(ip, "/rpc/Switch.GetStatus?id=0", user, password)
        temp = data.get("temperature") or {}
        return {
            "output":  bool(data.get("output", False)),
            "apower":  float(data.get("apower", 0.0)),
            "voltage": float(data.get("voltage", 0.0)),
            "current": float(data.get("current", 0.0)),
            "tempC":   float(temp.get("tC", 0.0)),
        }
    except Exception:
        return None


def toggle_and_status(ip: str, user: str, password: str) -> dict | None:
    """Toggle the switch then return updated status, or None on failure."""
    try:
        _shelly_get(ip, "/rpc/Switch.Toggle?id=0", user, password)
        time.sleep(0.3)
        return get_status(ip, user, password)
    except Exception:
        return None
