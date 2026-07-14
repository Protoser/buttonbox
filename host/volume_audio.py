"""Windows master + per-application volume, via pycaw.

The buttonbox has no audio; this exposes the PC's Windows audio mixer so the box's
Volume page can show and change the master level and individual app levels — the
same sliders as the Windows "Volume Mixer".

Optional dependency: `pip install pycaw comtypes`. If it's missing (or we're not on
Windows) the module degrades gracefully: available() is False, read() returns None,
and the setters are no-ops, so the companion still runs — the box just shows the
"No companion" placeholder on the Volume page.
"""
try:
    from pycaw.pycaw import AudioUtilities
    _OK = True
except Exception:                       # noqa: BLE001 — pycaw/comtypes absent or not Windows
    _OK = False

MAX_APPS = 6      # box shows master + up to this many apps
NAME_LEN = 12     # keep names short; the box column is narrow


def available():
    return _OK


def _master_endpoint():
    return AudioUtilities.GetSpeakers().EndpointVolume   # IAudioEndpointVolume


def _clean(name):
    """exe name -> a short, box-drawable label (ASCII, no ':' since it's the delimiter)."""
    if name.lower().endswith(".exe"):
        name = name[:-4]
    name = (name[:1].upper() + name[1:]) if name else name
    out = "".join(c for c in name if 32 <= ord(c) < 127 and c != ":")
    return out[:NAME_LEN] or "App"


class Mixer:
    """Reads/sets Windows volumes. Sessions are aggregated by process name (so all
    of a browser's tabs are one row), sorted stably, and capped at MAX_APPS. The box
    addresses apps by row index, so the last read's order is kept for set_app()."""

    def __init__(self):
        self._order = []   # process-name keys, index == box row

    def read(self):
        """(master 0-100, [(name, vol 0-100), ...]) or None if audio is unavailable."""
        if not _OK:
            return None
        try:
            master = int(round(_master_endpoint().GetMasterVolumeLevelScalar() * 100))
        except Exception:                       # noqa: BLE001
            return None
        apps = {}                               # key(lower name) -> [display, vol]
        try:
            for s in AudioUtilities.GetAllSessions():
                if not s.Process:
                    continue
                try:
                    name = s.Process.name()
                    lvl = int(round(s.SimpleAudioVolume.GetMasterVolume() * 100))
                except Exception:               # noqa: BLE001
                    continue
                key = name.lower()
                if key not in apps:             # first session of this process wins the level
                    apps[key] = [_clean(name), lvl]
        except Exception:                       # noqa: BLE001
            return None
        keys = sorted(apps, key=lambda k: apps[k][0].lower())[:MAX_APPS]
        self._order = keys
        return master, [(apps[k][0], apps[k][1]) for k in keys]

    def set_master(self, pct):
        if not _OK:
            return
        try:
            _master_endpoint().SetMasterVolumeLevelScalar(max(0, min(100, pct)) / 100.0, None)
        except Exception:                       # noqa: BLE001
            pass

    def set_app(self, index, pct):
        """Set every session of the app at box-row `index` (resolved live by name)."""
        if not _OK:
            return
        try:
            key = self._order[index]
        except (IndexError, AttributeError):
            return
        level = max(0, min(100, pct)) / 100.0
        try:
            for s in AudioUtilities.GetAllSessions():
                if s.Process and s.Process.name().lower() == key:
                    s.SimpleAudioVolume.SetMasterVolume(level, None)
        except Exception:                       # noqa: BLE001
            pass
