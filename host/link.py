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

import media
import sensors
import shelly_poll

VID_ESP32S3 = 0x303A


class DeviceLink(QThread):
    connected      = Signal(str)     # port name
    disconnected   = Signal()
    configReceived = Signal(dict)    # {flip,labels,idle,chord,boot,pcorder,apporder,nchords}
    chordsReceived = Signal(list)    # [{index,members,output}, ...]
    statsRead      = Signal(dict)    # {cpu,ram,gpu,ct,gt}
    lhmStatus      = Signal(bool, str)
    flashStarted   = Signal()        # emitted when flash command is sent

    def __init__(self, interval=0.5, parent=None):
        super().__init__(parent)
        self.interval = interval
        self._cmds = queue.Queue()
        self._running = True
        self._reconnect_after = 0.0   # epoch timestamp; pause port scan until then

        # Shelly auto-polling state (mode 2: companion polls via PC)
        self._shelly_mode  = 2        # mirrors device wmode; 0=off 1=on 2=auto
        self._shelly_ip    = ""
        self._shelly_user  = ""
        self._shelly_pass  = ""       # set by set_shelly_pass(); never echoed by device
        self._shelly_toggle_event = threading.Event()
        self._shelly_result: dict | None = None
        self._shelly_lock   = threading.Lock()

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

    def run(self):
        ok, err = sensors.init()
        self.lhmStatus.emit(ok, "" if ok else str(err))

        shelly_thread = threading.Thread(target=self._shelly_loop, daemon=True, name="shelly-poll")
        shelly_thread.start()
        sensor_thread = threading.Thread(target=self._sensor_loop, daemon=True, name="sensor-read")
        sensor_thread.start()
        music_thread = threading.Thread(target=self._music_loop, daemon=True, name="music-read")
        music_thread.start()

        ser = None
        buf = b""
        exp_chords = 0
        chords = []

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
                    ser = serial.Serial(port, 115200, timeout=0)
                    time.sleep(0.3)
                    buf = b""
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
                        if "ship"   in cfg: self._shelly_ip   = cfg["ship"]
                        if "shuser" in cfg: self._shelly_user = cfg["shuser"]
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
                    elif line.startswith("mctl "):
                        self._music_cmds.put(line[5:].strip())

                # Forward any pending Shelly result from the background poller
                with self._shelly_lock:
                    st = self._shelly_result
                    self._shelly_result = None
                if st is not None:
                    msg = (f"shelly out:{int(st['output'])} apower:{st['apower']:.2f}"
                           f" voltage:{st['voltage']:.1f} current:{st['current']:.3f}"
                           f" temp:{st['tempC']:.1f}\n")
                    ser.write(msg.encode())

                time.sleep(self.interval)
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
                if k in ("pcorder", "apporder"):
                    try:
                        d[k] = [int(x) for x in v.split(",")]
                    except ValueError:
                        pass
                elif k in ("wssid", "ship", "shuser"):
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
