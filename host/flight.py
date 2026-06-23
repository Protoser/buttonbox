"""Microsoft Flight Simulator telemetry via SimConnect.

Reads live instruments from MSFS (2020/2024; also FSX/P3D) using the `SimConnect`
Python package (`pip install SimConnect`), which bundles SimConnect.dll and talks
to the running sim. `Listener` owns the connection: `open()` connects (False if the
sim isn't running, so the caller retries), `read_latest()` polls the simvars and
returns the compact dict the box's `flight` serial line carries, `close()` drops
the link. If the package is missing the module degrades gracefully: `available()`
is False and the producer stays idle, exactly like media.py without WinRT.

Unit notes (SimConnect native units, converted here to what the box expects):
  - headings come back in RADIANS -> degrees
  - vertical speed uses VELOCITY_WORLD_Y (feet/second) -> feet/minute
  - gear/flaps/spoilers/throttle are percent (0..100)
Verify these against the sim during bring-up (see the plan's verification steps);
if a value reads wildly off, the unit assumption for that simvar is the suspect.
"""
import math

try:
    from SimConnect import SimConnect, AircraftRequests
    _OK = True
    # Quiet the library's own logger so a broken pipe (MSFS closed) doesn't spam the
    # console from its receiver thread. Harmless if the library doesn't use logging.
    import logging
    logging.getLogger("SimConnect").setLevel(logging.CRITICAL)
except Exception:                       # noqa: BLE001 — package absent or not on Windows
    _OK = False

# ENGINE_TYPE simvar -> the box's etyp (0 piston, 1 jet, 2 turboprop).
# SimConnect: 0 piston, 1 jet, 2 none, 3 helo-turbine, 4 unsupported, 5 turboprop.
_ENG_TYPE = {0: 0, 1: 1, 5: 2}


def available() -> bool:
    """True if SimConnect can be used (the package imported)."""
    return _OK


def _num(v, default=0.0):
    """SimConnect returns None while the sim is on a loading screen (and for any
    var name it doesn't know); coerce that (and any non-numeric) to a default so
    one missing var doesn't sink the whole line."""
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def _pct(v):
    """A 0..100 percentage. SimConnect's "percent over 100" vars come back as a
    0..1 fraction in some library versions and 0..100 in others; accept either."""
    x = _num(v)
    if 0.0 < x <= 1.0:                   # fraction -> percent
        x *= 100.0
    return max(0, min(100, int(round(x))))


class Listener:
    """An open SimConnect link that yields the latest instrument snapshot."""

    def __init__(self):
        self._sm = None
        self._aq = None

    def open(self) -> bool:
        """Connect to the running sim. Returns False if MSFS isn't up (the
        SimConnect() constructor raises), so the caller can retry later."""
        if not _OK:
            return False
        try:
            self._sm = SimConnect()
            # _time caches each var for N ms so repeated gets in one read are cheap.
            self._aq = AircraftRequests(self._sm, _time=200)
            return True
        except Exception:               # noqa: BLE001 — sim not running / SDK missing
            self.close()
            return False

    def close(self):
        sm = self._sm
        self._sm = None                 # drop refs first so a concurrent read bails
        self._aq = None
        if sm is not None:
            try:
                sm.quit = 1             # stop the library's receiver thread promptly
            except Exception:           # noqa: BLE001
                pass
            try:
                sm.exit()
            except Exception:           # noqa: BLE001 — pipe already broken
                pass

    def read_latest(self) -> dict | None:
        """Poll the simvars and return the box's field set, or None if the link
        died (caller reopens). A fresh-but-not-ready sim returns zeros, not None."""
        aq, sm = self._aq, self._sm
        if aq is None or sm is None:
            return None
        if getattr(sm, "quit", 0):       # sim sent QUIT (MSFS closed) -> drop the link
            return None
        try:
            g = aq.get
            # Heading & AP heading select come back in radians; AP alt/hdg-dir vary,
            # so normalize all angles through degrees().
            hdg = int(round(math.degrees(_num(g("PLANE_HEADING_DEGREES_MAGNETIC"))))) % 360
            vs  = int(round(_num(g("VELOCITY_WORLD_Y")) * 60.0))   # ft/s -> ft/min
            # SimConnect sign convention: pitch + = nose DOWN, bank + = roll LEFT.
            # The box wants pitch + = nose UP and bank + = roll RIGHT, so negate both.
            pit = max(-90, min(90, -int(round(math.degrees(_num(g("PLANE_PITCH_DEGREES")))))))
            bnk = max(-90, min(90, -int(round(math.degrees(_num(g("PLANE_BANK_DEGREES")))))))

            modes = 0
            if _num(g("AUTOPILOT_HEADING_LOCK")):  modes |= 0x01   # AP_HDG
            if _num(g("AUTOPILOT_ALTITUDE_LOCK")): modes |= 0x02   # AP_ALT
            if _num(g("AUTOPILOT_NAV1_LOCK")):     modes |= 0x04   # AP_NAV
            if _num(g("AUTOPILOT_APPROACH_HOLD")): modes |= 0x08   # AP_APR

            etyp = _ENG_TYPE.get(int(_num(g("ENGINE_TYPE"))), 0)
            neng = max(1, min(4, int(_num(g("NUMBER_OF_ENGINES"), 1))))

            def _engval(i):                                        # per-engine gauge
                if etyp == 1:                                      # jet: N1 % -> x10
                    return max(0, int(round(_num(g(f"TURB_ENG_N1:{i}")) * 10)))
                return max(0, int(round(_num(g(f"GENERAL_ENG_RPM:{i}")))))  # RPM
            eng1, eng2, eng3, eng4 = _engval(1), _engval(2), _engval(3), _engval(4)

            cap = _num(g("FUEL_TOTAL_CAPACITY"))
            fuel = int(round(_num(g("FUEL_TOTAL_QUANTITY")) / cap * 100)) if cap > 0 else 0

            return {
                "ias":   max(0, int(round(_num(g("AIRSPEED_INDICATED"))))),
                "alt":   int(round(_num(g("INDICATED_ALTITUDE")))),
                "hdg":   hdg,
                "vs":    vs,
                "pit":   pit,
                "bnk":   bnk,
                "apm":   1 if _num(g("AUTOPILOT_MASTER")) else 0,
                "aphdg": int(round(_num(g("AUTOPILOT_HEADING_LOCK_DIR")))) % 360,
                "apalt": int(round(_num(g("AUTOPILOT_ALTITUDE_LOCK_VAR")))),
                "apmode": modes,
                "gear":  _pct(g("GEAR_TOTAL_PCT_EXTENDED")),
                "flaps": _pct(g("FLAPS_HANDLE_PERCENT")),
                "pbrk":  1 if _num(g("BRAKE_PARKING_POSITION")) else 0,
                "splr":  _pct(g("SPOILERS_HANDLE_POSITION")),
                "thr":   _pct(g("GENERAL_ENG_THROTTLE_LEVER_POSITION:1")),
                "eng":   eng1,
                "eng2":  eng2,
                "eng3":  eng3,
                "eng4":  eng4,
                "neng":  neng,
                "fuel":  max(0, min(100, fuel)),
                "etyp":  etyp,
            }
        except OSError:                 # link dropped (sim closed) -> reopen
            return None
        except Exception:               # noqa: BLE001 — transient request hiccup
            return None


def encode(vals: dict) -> str:
    """Serialize a flight dict to the wire format: 'ias:120 alt:5500 ...'."""
    return " ".join(f"{k}:{v}" for k, v in vals.items())
