"""Background serial link to the buttonbox.

Owns the serial port on its own thread: discovers the box by USB VID, streams PC
telemetry, reads config/chord replies, and accepts commands from the GUI via a
thread-safe queue. Any unplug raises inside the thread, is caught, and drops back
to rediscovery -- the GUI never sees an exception and reconnects automatically.
"""
import queue
import time

import serial
import serial.tools.list_ports as list_ports
from PySide6.QtCore import QThread, Signal

import sensors

VID_ESP32S3 = 0x303A


class DeviceLink(QThread):
    connected      = Signal(str)     # port name
    disconnected   = Signal()
    configReceived = Signal(dict)    # {flip,labels,idle,chord,boot,pcorder,nchords}
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

    def stop(self):
        self._running = False

    @staticmethod
    def _find_port():
        for p in list_ports.comports():
            if (p.vid or 0) == VID_ESP32S3:
                return p.device
        return None

    def run(self):
        ok, err = sensors.init()
        self.lhmStatus.emit(ok, "" if ok else str(err))

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

                vals = sensors.read_stats()
                if vals:
                    self.statsRead.emit(vals)
                    ser.write((sensors.encode(vals) + "\n").encode())

                buf += ser.read(4096)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode(errors="replace").strip()
                    if not line:
                        continue
                    if line.startswith("cfg "):
                        cfg = self._parse_cfg(line)
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
                if k == "pcorder":
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
