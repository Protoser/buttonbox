"""BeamNG.drive telemetry via its built-in OutGauge UDP protocol.

BeamNG can emit a small dashboard packet over UDP (enable it in-game under
Options > Other > Protocols > "OutGauge UDP protocol", default 127.0.0.1:4444).
It's the same packet Live For Speed uses, so the layout below is the standard LFS
OutGauge struct. BeamNG fills gear / speed / rpm / turbo / engine+oil temp / fuel /
pedals / flags / showLights and leaves the rest (time, car, plid, oil pressure,
display strings) zero/empty.

`Listener` owns a UDP socket; `recv_latest()` blocks up to a timeout then drains any
backlog so we always forward the freshest packet. `parse()` decodes one datagram to
the compact dict the box's `beamng` serial line carries. Stdlib only.
"""
import socket
import struct

HOST = "127.0.0.1"
PORT = 4444

# OutGauge `flags` (OG_*) bits we use.
OG_TURBO = 0x2000   # turbo gauge applies to this car
OG_KM    = 0x4000   # set = km/h, clear = mph

# LFS OutGauge packet: little-endian, packed. BeamNG sends it with or without the
# trailing optional `int ID` (only present if an OutGauge ID is configured).
_FMT_NO_ID = "<I4sHbb7fII3f16s16s"
_FMT_ID    = _FMT_NO_ID + "i"
_SZ_NO_ID  = struct.calcsize(_FMT_NO_ID)   # 92
_SZ_ID     = struct.calcsize(_FMT_ID)      # 96


def available() -> bool:
    """True if telemetry can be received (UDP is stdlib, so always)."""
    return True


def parse(packet: bytes) -> dict | None:
    """Decode one OutGauge datagram to the box's field set, or None if the size
    doesn't match. Speed is converted to the unit BeamNG is set to display."""
    if len(packet) == _SZ_ID:
        vals = struct.unpack(_FMT_ID, packet)
    elif len(packet) == _SZ_NO_ID:
        vals = struct.unpack(_FMT_NO_ID, packet)
    else:
        return None
    (_time, _car, flags, gear, _plid, speed, rpm, turbo, eng_temp, fuel,
     _oil_press, oil_temp, _dash_lights, show_lights, _thr, _brk, _clt,
     _disp1, _disp2, *_rest) = vals

    km = bool(flags & OG_KM)
    spd = speed * (3.6 if km else 2.236936)         # m/s -> km/h or mph
    return {
        "gear":   int(gear),                        # raw: 0=R, 1=N, 2=1st gear ...
        "spd":    max(0, int(round(spd))),
        "unit":   0 if km else 1,                   # 0 = km/h, 1 = mph
        "rpm":    max(0, int(round(rpm))),
        "fuel":   int(round(min(1.0, max(0.0, fuel)) * 100)),
        "et":     int(round(eng_temp)),
        "ot":     int(round(oil_temp)),
        "tb":     max(0, int(round(turbo * 10))),   # tenths of a bar
        "tf":     1 if (flags & OG_TURBO) else 0,
        "lights": int(show_lights) & 0xFFFFFFFF,    # box masks the DL_* bits
    }


class Listener:
    """A bound UDP socket that yields the most recent OutGauge packet."""

    def __init__(self, host: str = HOST, port: int = PORT):
        self.host = host
        self.port = port
        self._sock: socket.socket | None = None

    def open(self) -> bool:
        """Bind the socket. Returns False if the port is unavailable (e.g. another
        OutGauge consumer already owns it); caller may retry later."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((self.host, self.port))
            self._sock = s
            return True
        except OSError:
            self.close()
            return False

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def recv_latest(self, timeout: float = 0.5) -> dict | None:
        """Block up to `timeout` for a packet, then drain any queued behind it and
        return the freshest parsed dict. None on timeout. Raises OSError if the
        socket dies (caller reopens)."""
        if self._sock is None:
            return None
        self._sock.settimeout(timeout)
        try:
            data, _ = self._sock.recvfrom(512)
        except socket.timeout:
            return None
        self._sock.settimeout(0.0)                  # drain backlog, keep the last
        try:
            while True:
                data, _ = self._sock.recvfrom(512)
        except (BlockingIOError, socket.timeout):
            pass
        return parse(data)
