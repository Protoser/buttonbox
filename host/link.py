"""Background serial link to the buttonbox.

Owns the serial port on its own thread: discovers the box by USB VID, streams PC
telemetry, reads config/chord replies, and accepts commands from the GUI via a
thread-safe queue. Any unplug raises inside the thread, is caught, and drops back
to rediscovery -- the GUI never sees an exception and reconnects automatically.
"""
import queue
import threading
import time

import serial
import serial.tools.list_ports as list_ports
from PySide6.QtCore import QThread, Signal

import beamng
import flight
import media
import sensors
import shelly_poll
import wled_poll

VID_ESP32S3 = 0x303A


class DeviceLink(QThread):
    connected      = Signal(str)     # port name
    disconnected   = Signal()
    configReceived = Signal(dict)    # {flip,labels,idle,chord,boot,pcorder,apporder,apphidden,nchords}
    chordsReceived = Signal(list)    # [{index,members,output}, ...]
    statsRead      = Signal(dict)    # {cpu,ram,gpu,ct,gt}
    lhmStatus      = Signal(bool, str)
    flashStarted   = Signal()        # emitted when flash command is sent
    mcduKey        = Signal(int)     # box MCDU-page key press -> MCDU output index (see mcdu.py)

    def __init__(self, interval=0.5, parent=None):
        super().__init__(parent)
        self.interval = interval
        self._cmds = queue.Queue()
        self._running = True
        self._reconnect_after = 0.0   # epoch timestamp; pause port scan until then
        self._last_time_sent  = 0.0   # epoch of last clock sync sent (resent hourly)

        # Shelly auto-polling state (mode 2: companion polls via PC)
        self._shelly_mode  = 2        # mirrors device wmode; 0=off 1=on 2=auto
        self._shelly_ip    = ""
        self._shelly_user  = ""
        self._shelly_pass  = ""       # set by set_shelly_pass(); never echoed by device
        self._shelly_toggle_event = threading.Event()
        self._shelly_result: dict | None = None
        self._shelly_lock   = threading.Lock()

        # WLED auto-control state (mode 2: companion polls + drives via PC). Shares
        # the device's wmode (mirrored on every cfg) with Shelly. Control requests
        # arrive from the box as "wledcmd <action>" lines and run on _wled_loop.
        self._wled_mode    = 2
        self._wled_ip      = ""
        self._wled_last_ps = -1            # last polled preset, for next/prev cycling
        self._wled_cmds: queue.Queue = queue.Queue()
        self._wled_event   = threading.Event()
        self._wled_result: dict | None = None
        self._wled_lock    = threading.Lock()

        # Latest PC sensor reading, produced by _sensor_loop. An LHM .Update() can
        # block for a while; doing it off the serial thread keeps the telemetry
        # heartbeat steady so the box never falls back to "Waiting for PC..." just
        # because one read ran long.
        self._latest_stats: dict = {}
        self._stats_lock = threading.Lock()

        # Now-playing media, produced by _music_loop (WinRT calls are slow and
        # async, so they stay off the serial thread too). Control commands from
        # the box arrive on _music_cmds and run on that same loop.
        self._music: dict = {"state": 0, "title": ""}
        self._music_lock = threading.Lock()
        self._music_cmds: queue.Queue = queue.Queue()

        # BeamNG OutGauge telemetry, produced by _beamng_loop (blocking UDP recv
        # stays off the serial thread). _beamng_ts is a monotonic stamp the serial
        # loop uses to decide "BeamNG actively driving" vs idle.
        self._beamng: dict = {}
        self._beamng_ts: float = 0.0
        self._beamng_lock = threading.Lock()

        # MSFS telemetry, produced by _flight_loop (SimConnect polling stays off the
        # serial thread). _flight_ts is a monotonic stamp the serial loop uses to
        # decide "MSFS actively running" vs idle.
        self._flight: dict = {}
        self._flight_ts: float = 0.0
        self._flight_lock = threading.Lock()

        # FlyByWire MCDU mirror. The QWebSocket lives in the GUI (McduPane); it pushes
        # the flattened 14-row screen here via set_mcdu(). The serial loop heartbeats
        # "mcdu act:" and sends only the rows that changed (tracked in _mcdu_dirty).
        self._mcdu_connected = False
        self._mcdu_rows = [""] * 14
        self._mcdu_dirty: set[int] = set()
        self._mcdu_lock = threading.Lock()

    # ---- called from the GUI thread (just enqueue; the thread does the I/O) ----
    def send(self, line):       self._cmds.put(line)
    def request_config(self):   self.send("get")
    def set_setting(self, k, v): self.send(f"set {k}:{v}")
    def add_chord(self, mask, out): self.send(f"chord add {int(mask)}:{int(out)}")
    def del_chord(self, i):     self.send(f"chord del {int(i)}")
    def flash(self):
        self.send("flash")
        self._reconnect_after = time.time() + 35   # pause scan while esptool uploads
        self.flashStarted.emit()

    def set_shelly_pass(self, pw: str):
        """Store Shelly password for auto-polling (never echoed back by device)."""
        self._shelly_pass = pw

    def set_mcdu(self, connected: bool, rows):
        """Push the latest MCDU screen (called from the GUI / QWebSocket thread).
        Caches connection state and marks changed rows dirty for the serial loop."""
        with self._mcdu_lock:
            if connected and not self._mcdu_connected:
                self._mcdu_dirty = set(range(14))      # (re)connect: resend everything
            self._mcdu_connected = connected
            if rows is not None:
                for i, r in enumerate(rows[:14]):
                    if r != self._mcdu_rows[i]:
                        self._mcdu_rows[i] = r
                        self._mcdu_dirty.add(i)

    def stop(self):
        self._running = False

    @staticmethod
    def _find_port():
        for p in list_ports.comports():
            if (p.vid or 0) == VID_ESP32S3:
                return p.device
        return None

    def _shelly_loop(self):
        """Background thread: polls Shelly via PC HTTP when mode==AUTO (2) and
        companion is connected; pushes state back via _shelly_result for the
        serial loop to forward to the device. HTTP never blocks the serial loop."""
        while self._running:
            # Wake up every 3 s, or immediately when a toggle is requested
            toggled = self._shelly_toggle_event.wait(timeout=3.0)
            if not self._running:
                break
            if self._shelly_mode != 2 or not self._shelly_ip or not self._shelly_user or not self._shelly_pass:
                continue
            if toggled:
                self._shelly_toggle_event.clear()
                st = shelly_poll.toggle_and_status(self._shelly_ip, self._shelly_user, self._shelly_pass)
            else:
                st = shelly_poll.get_status(self._shelly_ip, self._shelly_user, self._shelly_pass)
            if st is not None:
                with self._shelly_lock:
                    self._shelly_result = st

    def _run_wled_cmd(self, cmd: str):
        ip = self._wled_ip
        if   cmd == "on":   wled_poll.set_power(ip, True)
        elif cmd == "off":  wled_poll.set_power(ip, False)
        elif cmd.startswith("bri "):                        # absolute brightness, sent on release
            try:
                wled_poll.set_brightness(ip, int(cmd.split()[1]))
            except (ValueError, IndexError):
                pass
        elif cmd == "ps+":  wled_poll.cycle_preset(ip, self._wled_last_ps, +1)
        elif cmd == "ps-":  wled_poll.cycle_preset(ip, self._wled_last_ps, -1)

    def _wled_loop(self):
        """Background thread: when mode==AUTO and the box has a WLED IP, run any
        queued control requests then poll state, pushing it back via _wled_result
        for the serial loop to forward. HTTP never blocks the serial loop."""
        while self._running:
            triggered = self._wled_event.wait(timeout=3.0)
            self._wled_event.clear()
            if not self._running:
                break
            if self._wled_mode != 2 or not self._wled_ip:
                # Drop any commands queued while not in companion mode.
                with self._wled_cmds.mutex:
                    self._wled_cmds.queue.clear()
                continue
            if triggered:
                try:
                    while True:
                        self._run_wled_cmd(self._wled_cmds.get_nowait())
                except queue.Empty:
                    pass
            st = wled_poll.get_state(self._wled_ip)
            if st is not None:
                self._wled_last_ps = st["ps"]
                with self._wled_lock:
                    self._wled_result = st

    def _sensor_loop(self):
        """Background thread: read PC sensors at `interval` and cache the latest
        values. A slow LHM .Update() stays off the serial thread, which always
        writes the most recent cached values on a steady cadence."""
        while self._running:
            try:
                vals = sensors.read_stats()
            except Exception:               # noqa: BLE001
                vals = None
            if vals:
                with self._stats_lock:
                    self._latest_stats = vals
            time.sleep(self.interval)

    def _music_loop(self):
        """Background thread: run any queued transport commands, then refresh the
        now-playing snapshot. WinRT calls live here so the serial loop only ever
        reads the cached dict."""
        while self._running:
            try:
                while True:
                    media.control(self._music_cmds.get_nowait())
            except queue.Empty:
                pass
            vals = media.read_now_playing()
            if vals is not None:
                with self._music_lock:
                    self._music = vals
            time.sleep(1.0)

    def _beamng_loop(self):
        """Background thread: own the OutGauge UDP socket, block for the latest
        packet and cache it with a timestamp. The blocking recv stays off the
        serial thread; the serial loop only ever reads the cached dict."""
        listener = beamng.Listener()
        opened = False
        while self._running:
            if not opened:
                opened = listener.open()
                if not opened:
                    time.sleep(2.0)    # port busy / unavailable -> retry later
                    continue
            try:
                vals = listener.recv_latest(timeout=0.5)
            except OSError:
                listener.close()
                opened = False
                continue
            if vals is not None:
                with self._beamng_lock:
                    self._beamng = vals
                    self._beamng_ts = time.monotonic()
        listener.close()

    def _flight_loop(self):
        """Background thread: own the SimConnect link, poll MSFS instruments and
        cache them with a timestamp. The SimConnect I/O stays off the serial thread;
        the serial loop only ever reads the cached dict. read_latest() returns None
        when the link drops (sim closed) so we reconnect."""
        listener = flight.Listener()
        opened = False
        while self._running:
            if not opened:
                opened = listener.open()
                if not opened:
                    time.sleep(2.0)    # MSFS not running / SimConnect absent -> retry
                    continue
            # Any failure here (e.g. closing MSFS breaks the SimConnect pipe) must not
            # kill the thread: tear the link down and reconnect.
            try:
                vals = listener.read_latest()
            except Exception:          # noqa: BLE001
                vals = None
            if vals is None:
                try:
                    listener.close()
                except Exception:      # noqa: BLE001
                    pass
                opened = False
                time.sleep(1.0)
                continue
            with self._flight_lock:
                self._flight = vals
                self._flight_ts = time.monotonic()
            time.sleep(0.2)            # ~5 Hz instrument refresh
        listener.close()

    def run(self):
        ok, err = sensors.init()
        self.lhmStatus.emit(ok, "" if ok else str(err))

        shelly_thread = threading.Thread(target=self._shelly_loop, daemon=True, name="shelly-poll")
        shelly_thread.start()
        wled_thread = threading.Thread(target=self._wled_loop, daemon=True, name="wled-poll")
        wled_thread.start()
        sensor_thread = threading.Thread(target=self._sensor_loop, daemon=True, name="sensor-read")
        sensor_thread.start()
        music_thread = threading.Thread(target=self._music_loop, daemon=True, name="music-read")
        music_thread.start()
        beamng_thread = threading.Thread(target=self._beamng_loop, daemon=True, name="beamng-read")
        beamng_thread.start()
        flight_thread = threading.Thread(target=self._flight_loop, daemon=True, name="flight-read")
        flight_thread.start()

        ser = None
        buf = b""
        exp_chords = 0
        chords = []
        last_slow = 0.0          # monotonic of last slow telemetry write (sensors/music/clock)

        while self._running:
            # --- (re)connect ---
            if ser is None:
                if time.time() < self._reconnect_after:
                    time.sleep(2.0)
                    continue
                port = self._find_port()
                if not port:
                    time.sleep(1.0)
                    continue
                try:
                    # write_timeout so a stalled box can never block this thread forever
                    # (the firmware also bounds its own writes); a rare timeout just
                    # drops to reconnect rather than hanging.
                    ser = serial.Serial(port, 115200, timeout=0, write_timeout=1.0)
                    time.sleep(0.3)
                    buf = b""
                    self._last_time_sent = 0.0      # push the clock right after reconnecting
                    with self._mcdu_lock:          # resend the whole MCDU to the fresh device
                        self._mcdu_dirty = set(range(14))
                    self.connected.emit(port)
                    self._cmds.put("get")
                except Exception:           # noqa: BLE001
                    ser = None
                    time.sleep(1.0)
                    continue

            # --- normal operation ---
            try:
                while not self._cmds.empty():
                    cmd = self._cmds.get_nowait()
                    ser.write((cmd + "\n").encode())

                # Slow telemetry (sensors / now-playing / clock) only needs ~0.5 s;
                # BeamNG below is sent every fast tick, so gate these on self.interval.
                now = time.monotonic()
                if now - last_slow >= self.interval:
                    last_slow = now
                    # Write the most recent cached sensor values (read on _sensor_loop)
                    with self._stats_lock:
                        vals = dict(self._latest_stats)
                    if vals:
                        self.statsRead.emit(vals)
                        ser.write((sensors.encode(vals) + "\n").encode())

                    # Now-playing line for the Music page (state + sanitized title)
                    with self._music_lock:
                        m = dict(self._music)
                    ser.write(f"music {m['state']} {m['title']}\n".encode())

                    # Clock sync for the header clock (box has no RTC): UTC epoch + local
                    # offset. Sent on connect then hourly -- the box keeps ticking on its
                    # own system clock and also re-syncs from NTP, so no steady stream.
                    if time.time() - self._last_time_sent >= 3600:
                        lt = time.localtime()
                        off = -(time.altzone if lt.tm_isdst > 0 else time.timezone)
                        ser.write(f"time {int(time.time())} {off}\n".encode())
                        self._last_time_sent = time.time()

                    # MCDU mirror: act heartbeat (keeps a static screen fresh) + only the
                    # rows that changed since the last tick.
                    with self._mcdu_lock:
                        mcdu_on = self._mcdu_connected
                        dirty = sorted(self._mcdu_dirty)
                        mrows = list(self._mcdu_rows)
                        self._mcdu_dirty.clear()
                    ser.write(b"mcdu act:1\n" if mcdu_on else b"mcdu act:0\n")
                    if mcdu_on:
                        for i in dirty:
                            ser.write(f"mcdu {i}:{mrows[i]}\n".encode())

                # BeamNG telemetry -- sent every fast tick (~100 ms) so turn signals
                # and the RPM bar track the game. act:1 only while packets are arriving
                # (driving); otherwise act:0 so the box shows "BeamNG idle".
                with self._beamng_lock:
                    b = dict(self._beamng)
                    bts = self._beamng_ts
                if b and (time.monotonic() - bts) < 1.0:
                    ser.write((f"beamng act:1 gear:{b['gear']} spd:{b['spd']} unit:{b['unit']} "
                               f"rpm:{b['rpm']} fuel:{b['fuel']} et:{b['et']} ot:{b['ot']} "
                               f"tb:{b['tb']} tf:{b['tf']} lights:{b['lights']}\n").encode())
                else:
                    ser.write(b"beamng act:0\n")

                # MSFS telemetry -- same fast cadence so instruments track the sim.
                # act:1 only while SimConnect reads are fresh; otherwise act:0.
                with self._flight_lock:
                    f = dict(self._flight)
                    fts = self._flight_ts
                if f and (time.monotonic() - fts) < 1.0:
                    ser.write((f"flight act:1 ias:{f['ias']} alt:{f['alt']} hdg:{f['hdg']} "
                               f"vs:{f['vs']} pit:{f['pit']} bnk:{f['bnk']} apm:{f['apm']} "
                               f"aphdg:{f['aphdg']} apalt:{f['apalt']} apmode:{f['apmode']} "
                               f"gear:{f['gear']} flaps:{f['flaps']} pbrk:{f['pbrk']} "
                               f"splr:{f['splr']} thr:{f['thr']} eng:{f['eng']} eng2:{f['eng2']} "
                               f"eng3:{f['eng3']} eng4:{f['eng4']} neng:{f['neng']} "
                               f"fuel:{f['fuel']} etyp:{f['etyp']}\n").encode())
                else:
                    ser.write(b"flight act:0\n")

                buf += ser.read(4096)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode(errors="replace").strip()
                    if not line:
                        continue
                    if line.startswith("cfg "):
                        cfg = self._parse_cfg(line)
                        # Mirror shelly config for auto-polling
                        self._shelly_mode = cfg.get("wmode", self._shelly_mode)
                        self._wled_mode   = cfg.get("wmode", self._wled_mode)
                        if "ship"   in cfg: self._shelly_ip   = cfg["ship"]
                        if "shuser" in cfg: self._shelly_user = cfg["shuser"]
                        if "wledip" in cfg: self._wled_ip     = cfg["wledip"]
                        self.configReceived.emit(cfg)
                        exp_chords = cfg.get("nchords", 0)
                        chords = []
                        if exp_chords == 0:
                            self.chordsReceived.emit([])
                    elif line.startswith("chd "):
                        c = self._parse_chd(line)
                        if c is not None:
                            chords.append(c)
                            if len(chords) >= exp_chords:
                                self.chordsReceived.emit(list(chords))
                    elif line == "shelly_toggle":
                        self._shelly_toggle_event.set()
                    elif line.startswith("wledcmd "):
                        self._wled_cmds.put(line[8:].strip())
                        self._wled_event.set()
                    elif line.startswith("mctl "):
                        self._music_cmds.put(line[5:].strip())
                    elif line.startswith("mcdukey "):
                        try:
                            self.mcduKey.emit(int(line[8:].strip()))
                        except ValueError:
                            pass

                # Forward any pending Shelly result from the background poller
                with self._shelly_lock:
                    st = self._shelly_result
                    self._shelly_result = None
                if st is not None:
                    msg = (f"shelly out:{int(st['output'])} apower:{st['apower']:.2f}"
                           f" voltage:{st['voltage']:.1f} current:{st['current']:.3f}"
                           f" temp:{st['tempC']:.1f}\n")
                    ser.write(msg.encode())

                # Forward any pending WLED state from the background poller
                with self._wled_lock:
                    wst = self._wled_result
                    self._wled_result = None
                if wst is not None:
                    ser.write(f"wled on:{int(wst['on'])} bri:{wst['bri']} ps:{wst['ps']}\n".encode())

                time.sleep(0.1)          # fast tick: BeamNG at ~10 Hz, slow telemetry gated above
            except (serial.SerialException, OSError):
                try:
                    ser.close()
                except Exception:           # noqa: BLE001
                    pass
                ser = None
                self.disconnected.emit()    # -> back to rediscovery, no crash

        if ser:
            try:
                ser.close()
            except Exception:               # noqa: BLE001
                pass

    @staticmethod
    def _parse_cfg(line):
        d = {}
        for tok in line.split()[1:]:
            if ":" in tok:
                k, v = tok.split(":", 1)
                if k in ("pcorder", "apporder", "mcdumap"):
                    try:
                        d[k] = [int(x) for x in v.split(",")]
                    except ValueError:
                        pass
                elif k in ("wssid", "ship", "shuser", "wledip"):
                    d[k] = v          # keep as string
                else:
                    try:
                        d[k] = int(v)
                    except ValueError:
                        pass
        return d

    @staticmethod
    def _parse_chd(line):
        try:
            _, rest = line.split(" ", 1)
            i, mask, out = rest.split(":")
            return {"index": int(i), "members": int(mask), "output": int(out)}
        except Exception:                   # noqa: BLE001
            return None
