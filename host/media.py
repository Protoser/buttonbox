"""Now-playing media via the Windows global media session (GSMTC).

This is the same session the keyboard media keys drive: Spotify, the browser
(YouTube / SoundCloud / etc. via the page's Media Session API), and most native
players all register here, so `title` reflects the current song or video.

Reading and controlling needs WinRT, exposed to Python by Microsoft's pywinrt
packages (`pip install winrt-Windows.Media.Control winrt-Windows.Foundation`,
which ship prebuilt wheels). The older single `winsdk` package exposes the same
API and is accepted as a fallback. If neither is present the module degrades
gracefully: `available()` is False, `read_now_playing()` returns a neutral
"nothing playing" result, and `control()` is a no-op.
"""
import asyncio

try:                                    # modern pywinrt (prebuilt wheels, incl. 3.13)
    from winrt.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as _MediaManager,
        GlobalSystemMediaTransportControlsSessionPlaybackStatus as _Status,
    )
    _OK = True
except Exception:                       # noqa: BLE001
    try:                                # legacy all-in-one winsdk
        from winsdk.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionManager as _MediaManager,
            GlobalSystemMediaTransportControlsSessionPlaybackStatus as _Status,
        )
        _OK = True
    except Exception:                   # noqa: BLE001 — WinRT absent or not on Windows
        _OK = False

# The box draws titles with a 6x12 font that covers ASCII + Cyrillic, so keep
# those characters and drop the rest (other scripts have no glyphs). First map a
# few common typographic characters down to ASCII.
_SUBST = {
    "–": "-", "—": "-", "‒": "-", "−": "-",   # dashes
    "‘": "'", "’": "'", "‚": ",",                  # single quotes
    "“": '"', "”": '"', "„": '"',                  # double quotes
    "…": "...", "•": "*", " ": " ", "→": "->",
}


def _sanitize(s: str) -> str:
    """Map typographic punctuation to ASCII, then keep only glyphs the box's
    font can draw: printable ASCII (0x20-0x7E) and Cyrillic (U+0400-U+04FF)."""
    if not s:
        return ""
    for k, v in _SUBST.items():
        s = s.replace(k, v)
    out = [ch for ch in s
           if 0x20 <= ord(ch) <= 0x7E or 0x0400 <= ord(ch) <= 0x04FF]
    return "".join(out).strip()


def _cap_bytes(s: str, max_bytes: int) -> str:
    """Trim to max_bytes of UTF-8 without splitting a multi-byte character (the
    box's title buffer is 64 bytes; a half character would render as garbage)."""
    b = s.encode("utf-8")
    if len(b) <= max_bytes:
        return s
    return b[:max_bytes].decode("utf-8", "ignore")


def available() -> bool:
    """True if WinRT media-session access is usable."""
    return _OK


async def _session():
    mgr = await _MediaManager.request_async()
    return mgr.get_current_session()


async def _read():
    s = await _session()
    if s is None:
        return {"state": 0, "title": ""}
    props = await s.try_get_media_properties_async()
    status = s.get_playback_info().playback_status
    state = 1 if status == _Status.PLAYING else (2 if status == _Status.PAUSED else 0)
    title = _sanitize(props.title or "")
    artist = _sanitize(props.artist or "")
    if artist and title:
        full = f"{artist} - {title}"
    else:
        full = title or artist
    return {"state": state, "title": _cap_bytes(full, 60)}


def read_now_playing():
    """Return {'state': 0|1|2, 'title': str} or None if media access failed."""
    if not _OK:
        return {"state": 0, "title": ""}
    try:
        return asyncio.run(_read())
    except Exception:                   # noqa: BLE001
        return None


async def _ctl(cmd: str):
    s = await _session()
    if s is None:
        return
    if cmd == "next":
        await s.try_skip_next_async()
    elif cmd == "prev":
        await s.try_skip_previous_async()
    else:                               # playpause
        await s.try_toggle_play_pause_async()


def control(cmd: str):
    """Run a transport command: 'prev', 'next', or 'playpause'."""
    if not _OK:
        return
    try:
        asyncio.run(_ctl(cmd))
    except Exception:                   # noqa: BLE001
        pass
