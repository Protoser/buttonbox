"""WLED JSON HTTP polling + control (stdlib only).

Used by DeviceLink when wifiMode == AUTO (2) and the companion is connected: the
PC polls the WLED device and pushes its state to the box over serial, and runs the
box's control requests so the box never needs WiFi when the companion is present.
WLED has no auth by default, so these are plain GET/POST against /json/state.
"""
import json
from urllib.request import Request, urlopen


def _get(ip: str, path: str, timeout: float = 3.0):
    with urlopen(Request(f"http://{ip}{path}"), timeout=timeout) as r:
        return json.loads(r.read().decode())


def _post(ip: str, body: dict, timeout: float = 3.0) -> bool:
    data = json.dumps(body).encode()
    req = Request(f"http://{ip}/json/state", data=data,
                  headers={"Content-Type": "application/json"})
    try:
        with urlopen(req, timeout=timeout) as r:
            r.read()
        return True
    except Exception:               # noqa: BLE001
        return False


def get_state(ip: str) -> dict | None:
    """Return {on, bri, ps} or None on any failure."""
    try:
        d = _get(ip, "/json/state")
        return {
            "on":  bool(d.get("on", False)),
            "bri": int(d.get("bri", 0)),
            "ps":  int(d.get("ps", -1)),
        }
    except Exception:               # noqa: BLE001
        return None


def set_power(ip: str, on: bool) -> bool:
    return _post(ip, {"on": bool(on)})


def set_brightness(ip: str, bri: int) -> bool:
    return _post(ip, {"bri": max(0, min(255, int(bri)))})    # absolute 0..255


def _preset_ids(ip: str) -> list[int]:
    """Sorted list of existing (saved) preset IDs, or [] if unavailable."""
    try:
        d = _get(ip, "/presets.json")
        return sorted(int(k) for k, v in d.items()
                      if k.isdigit() and int(k) >= 1 and isinstance(v, dict) and v)
    except Exception:               # noqa: BLE001
        return []


def cycle_preset(ip: str, current: int, direction: int) -> bool:
    """Step to the next (+1) / previous (-1) saved preset, wrapping. Falls back to
    WLED's built-in forward cycle string if the preset list can't be fetched."""
    ids = _preset_ids(ip)
    if not ids:
        return _post(ip, {"ps": "1~250~"})      # forward-only fallback
    if current in ids:
        target = ids[(ids.index(current) + direction) % len(ids)]
    else:
        target = ids[0] if direction > 0 else ids[-1]
    return _post(ip, {"ps": target})
