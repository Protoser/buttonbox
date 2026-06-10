"""Buttonbox companion — system-tray app that streams PC stats to the box and
reads/edits its settings and chords live. Starts minimized to the tray.

Run:   python app.py        (or pythonw app.py for no console)
"""
import os
import sys

from PySide6.QtCore import Qt
from PySide6.QtGui import QAction, QColor, QIcon, QPainter, QPixmap
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFormLayout, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMenu,
    QProgressBar, QPushButton, QSystemTrayIcon, QTabWidget, QVBoxLayout, QWidget,
)

import secrets_store
from link import DeviceLink

# ---- constants mirrored from the firmware (config.h / settings) ----
NUM_HID = 14            # physical buttons, shown 1..14
OUT_MIN, OUT_MAX = 14, 31     # chord outputs (shown 15..32)
MAX_CHORDS = 18
IDLE_OPTS  = [(0, "Off"), (30, "30 s"), (120, "2 min")]
CHORD_OPTS = [30, 40, 60, 80]
BOOT_OPTS  = [(0, "Apps launcher"), (1, "Buttons"), (2, "Timer"), (3, "PC"), (4, "Shelly"), (5, "Menu")]
PCSTAT_BITS = [("CPU", 0), ("RAM", 1), ("GPU", 2), ("CPU Temp", 3), ("GPU Temp", 4),
               ("VRAM", 5), ("CPU Power", 6), ("GPU Power", 7)]
PCSTAT_MAX = 5      # box shows up to 5 at once
# (wire key, label, unit, bar min, bar max)
STAT_ROWS = [("cpu", "CPU", "%",   0, 100), ("ram", "RAM", "%",   0, 100), ("gpu", "GPU", "%", 0, 100),
             ("ct", "CPU Temp", "°C", 30, 100), ("gt", "GPU Temp", "°C", 30, 100),
             ("vr", "VRAM", "%", 0, 100), ("cp", "CPU Power", "W", 0, 250), ("gp", "GPU Power", "W", 0, 450)]

APP_NAME = "ButtonboxCompanion"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


# ---------------------------------------------------------------- autostart ----
def _autostart_target():
    if getattr(sys, "frozen", False):
        return f'"{sys.executable}"'
    pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    return f'"{pyw}" "{os.path.abspath(__file__)}"'


def is_autostart():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            winreg.QueryValueEx(k, APP_NAME)
            return True
    except Exception:        # noqa: BLE001
        return False


def set_autostart(enable):
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if enable:
            winreg.SetValueEx(k, APP_NAME, 0, winreg.REG_SZ, _autostart_target())
        else:
            try:
                winreg.DeleteValue(k, APP_NAME)
            except FileNotFoundError:
                pass


def make_icon():
    pix = QPixmap(64, 64)
    pix.fill(Qt.transparent)
    p = QPainter(pix)
    p.setRenderHint(QPainter.Antialiasing)
    p.setBrush(QColor("#2d8cf0"))
    p.setPen(Qt.NoPen)
    p.drawRoundedRect(6, 6, 52, 52, 12, 12)
    p.setPen(QColor("white"))
    f = p.font(); f.setBold(True); f.setPixelSize(34); p.setFont(f)
    p.drawText(pix.rect(), Qt.AlignCenter, "BB")
    p.end()
    return QIcon(pix)


def members_str(mask):
    return "+".join(str(i + 1) for i in range(NUM_HID) if mask & (1 << i)) or "(none)"


# ------------------------------------------------------------------- tabs ------
class MonitorTab(QWidget):
    def __init__(self):
        super().__init__()
        lay = QVBoxLayout(self)
        self.status = QLabel("Disconnected")
        self.status.setStyleSheet("font-weight: bold;")
        lay.addWidget(self.status)
        self.bars = {}
        form = QFormLayout()
        for key, label, unit, mn, mx in STAT_ROWS:
            bar = QProgressBar()
            bar.setRange(mn, mx)
            bar.setFormat(f"%v {unit}")
            bar.setValue(mn)
            self.bars[key] = (bar, mn, mx)
            form.addRow(label, bar)
        lay.addLayout(form)
        lay.addStretch(1)
        self.lhm = QLabel("")
        self.lhm.setWordWrap(True)
        self.lhm.setStyleSheet("color: #888;")
        lay.addWidget(self.lhm)

    def set_stats(self, vals):
        for key, (bar, mn, mx) in self.bars.items():
            if key in vals:
                bar.setValue(max(mn, min(mx, vals[key])))


class DeviceTab(QWidget):
    def __init__(self, link):
        super().__init__()
        self.link = link
        self._loading = False
        lay = QVBoxLayout(self)
        form = QFormLayout()

        self.flip = QCheckBox("Mounted upside-down (rotate 180°)")
        self.flip.toggled.connect(lambda v: self._set("flip", 1 if v else 0))
        form.addRow("Rotate", self.flip)

        self.labels = QComboBox()
        self.labels.addItem("HID number", 0)
        self.labels.addItem("GPIO", 1)
        self.labels.currentIndexChanged.connect(lambda: self._set("labels", self.labels.currentData()))
        form.addRow("Grid labels", self.labels)

        self.idle = QComboBox()
        for val, text in IDLE_OPTS:
            self.idle.addItem(text, val)
        self.idle.currentIndexChanged.connect(lambda: self._set("idle", self.idle.currentData()))
        form.addRow("Idle blank", self.idle)

        self.chord = QComboBox()
        for val in CHORD_OPTS:
            self.chord.addItem(f"{val} ms", val)
        self.chord.currentIndexChanged.connect(lambda: self._set("chord", self.chord.currentData()))
        form.addRow("Chord window", self.chord)

        self.boot = QComboBox()
        for val, text in BOOT_OPTS:
            self.boot.addItem(text, val)
        self.boot.currentIndexChanged.connect(lambda: self._set("boot", self.boot.currentData()))
        form.addRow("Boot screen", self.boot)
        lay.addLayout(form)

        box = QGroupBox(f"PC stats shown on the box — drag to reorder (up to {PCSTAT_MAX})")
        bl = QVBoxLayout(box)
        self.stat_list = QListWidget()
        self.stat_list.setDragDropMode(QListWidget.InternalMove)
        self.stat_list.setMaximumHeight(115)
        self.stat_list.model().rowsMoved.connect(lambda *_: self._send_order())
        bl.addWidget(self.stat_list)
        addrow = QHBoxLayout()
        addrow.addWidget(QLabel("Add:"))
        self.stat_add_combo = QComboBox()
        addrow.addWidget(self.stat_add_combo)
        add_btn = QPushButton("Add")
        add_btn.clicked.connect(self._add_stat)
        addrow.addWidget(add_btn)
        rem_btn = QPushButton("Remove")
        rem_btn.clicked.connect(self._remove_stat)
        addrow.addWidget(rem_btn)
        bl.addLayout(addrow)
        self.stat_note = QLabel("")
        self.stat_note.setStyleSheet("color: #888;")
        bl.addWidget(self.stat_note)
        lay.addWidget(box)

        row = QHBoxLayout()
        flash = QPushButton("Put device in Flash Mode")
        flash.clicked.connect(self.link.flash)
        row.addWidget(flash)
        refresh = QPushButton("Refresh")
        refresh.clicked.connect(self.link.request_config)
        row.addWidget(refresh)
        row.addStretch(1)
        lay.addLayout(row)

        conn = QGroupBox("Connectivity (WiFi + Shelly)")
        cl = QFormLayout(conn)
        self.wifi_mode_combo = QComboBox()
        for val, lbl in ((2, "Auto — via PC when companion is connected"), (1, "Always On"), (0, "Always Off")):
            self.wifi_mode_combo.addItem(lbl, val)
        cl.addRow("WiFi Mode", self.wifi_mode_combo)
        self.wifi_ssid = QLineEdit(); self.wifi_ssid.setPlaceholderText("e.g. MyHomeNetwork")
        cl.addRow("WiFi SSID", self.wifi_ssid)
        self.wifi_pass = QLineEdit(); self.wifi_pass.setEchoMode(QLineEdit.Password)
        self.wifi_pass.setPlaceholderText("WiFi password")
        cl.addRow("WiFi password", self.wifi_pass)
        self.shelly_ip = QLineEdit(); self.shelly_ip.setPlaceholderText("e.g. 192.168.1.100")
        cl.addRow("Shelly IP", self.shelly_ip)
        self.shelly_user = QLineEdit(); self.shelly_user.setPlaceholderText("admin")
        cl.addRow("Shelly user", self.shelly_user)
        self.shelly_pass = QLineEdit(); self.shelly_pass.setEchoMode(QLineEdit.Password)
        self.shelly_pass.setPlaceholderText("Shelly password")
        cl.addRow("Shelly password", self.shelly_pass)
        conn_apply = QPushButton("Apply")
        conn_apply.clicked.connect(self._apply_connectivity)
        cl.addRow("", conn_apply)
        lay.addWidget(conn)
        lay.addStretch(1)

    def _set(self, key, val):
        if not self._loading and val is not None:
            self.link.set_setting(key, val)

    def _get_order(self):
        bits = [self.stat_list.item(i).data(Qt.UserRole)
                for i in range(self.stat_list.count())]
        while len(bits) < PCSTAT_MAX:
            bits.append(255)
        return bits[:PCSTAT_MAX]

    def _send_order(self):
        if self._loading:
            return
        order = self._get_order()
        self.link.set_setting("pcorder", ",".join(str(b) for b in order))

    def _update_add_combo(self):
        current = {self.stat_list.item(i).data(Qt.UserRole)
                   for i in range(self.stat_list.count())}
        self.stat_add_combo.clear()
        for label, bit in PCSTAT_BITS:
            if bit not in current:
                self.stat_add_combo.addItem(label, bit)
        self.stat_add_combo.setEnabled(
            self.stat_list.count() < PCSTAT_MAX and self.stat_add_combo.count() > 0)

    def _add_stat(self):
        if self.stat_list.count() >= PCSTAT_MAX:
            self.stat_note.setText(f"You can show up to {PCSTAT_MAX} stats at once.")
            return
        bit = self.stat_add_combo.currentData()
        if bit is None:
            return
        label = self.stat_add_combo.currentText()
        item = QListWidgetItem(label)
        item.setData(Qt.UserRole, bit)
        self.stat_list.addItem(item)
        self.stat_note.setText("")
        self._update_add_combo()
        self._send_order()

    def _apply_connectivity(self):
        for k, w in (("wifi_ssid", self.wifi_ssid), ("wifi_pass", self.wifi_pass),
                     ("shelly_ip", self.shelly_ip), ("shelly_user", self.shelly_user)):
            if w.text():
                self.link.set_setting(k, w.text())
        if self.shelly_pass.text():
            self.link.set_setting("shelly_pass", self.shelly_pass.text())
            self.link.set_shelly_pass(self.shelly_pass.text())
            secrets_store.set_secret("shelly_pass", self.shelly_pass.text())  # encrypted (DPAPI)
        self.link.set_setting("wifi_mode", self.wifi_mode_combo.currentData())

    def _remove_stat(self):
        row = self.stat_list.currentRow()
        if row >= 0:
            self.stat_list.takeItem(row)
            self.stat_note.setText("")
            self._update_add_combo()
            self._send_order()

    def _sel(self, combo, val):
        idx = combo.findData(val)
        if idx < 0:
            combo.addItem(str(val), val)
            idx = combo.findData(val)
        combo.setCurrentIndex(idx)

    def load(self, cfg):
        self._loading = True
        self.flip.setChecked(bool(cfg.get("flip", 0)))
        self._sel(self.labels, cfg.get("labels", 0))
        self._sel(self.idle, cfg.get("idle", 0))
        self._sel(self.chord, cfg.get("chord", 40))
        self._sel(self.boot, cfg.get("boot", 1))
        order = cfg.get("pcorder", [0, 1, 2, 3, 4])
        self.stat_list.clear()
        for bit in order:
            if isinstance(bit, int) and 0 <= bit < len(PCSTAT_BITS):
                label = PCSTAT_BITS[bit][0]
                item = QListWidgetItem(label)
                item.setData(Qt.UserRole, bit)
                self.stat_list.addItem(item)
        self._update_add_combo()
        # Non-secret connectivity fields (passwords are never echoed back)
        if "wssid"  in cfg: self.wifi_ssid.setText(cfg["wssid"])
        if "ship"   in cfg: self.shelly_ip.setText(cfg["ship"])
        if "shuser" in cfg: self.shelly_user.setText(cfg["shuser"])
        if "wmode"  in cfg:
            idx = self.wifi_mode_combo.findData(cfg["wmode"])
            if idx >= 0:
                self.wifi_mode_combo.setCurrentIndex(idx)
        self._loading = False


class ChordsTab(QWidget):
    def __init__(self, link):
        super().__init__()
        self.link = link
        self.chords = []
        lay = QVBoxLayout(self)

        lay.addWidget(QLabel("Current chords (press the members together → emit the output):"))
        self.list = QListWidget()
        lay.addWidget(self.list, 1)
        delbtn = QPushButton("Delete selected")
        delbtn.clicked.connect(self._delete)
        lay.addWidget(delbtn)

        box = QGroupBox("Add chord")
        bl = QVBoxLayout(box)
        bl.addWidget(QLabel("Members (pick 2+):"))
        grid = QGridLayout()
        self.member_cbs = []
        for i in range(NUM_HID):
            cb = QCheckBox(str(i + 1))
            self.member_cbs.append(cb)
            grid.addWidget(cb, i // 7, i % 7)
        bl.addLayout(grid)
        row = QHBoxLayout()
        row.addWidget(QLabel("Output:"))
        self.out = QComboBox()
        for o in range(OUT_MIN, OUT_MAX + 1):
            self.out.addItem(f"Button {o + 1}", o)
        row.addWidget(self.out)
        addbtn = QPushButton("Add")
        addbtn.clicked.connect(self._add)
        row.addWidget(addbtn)
        row.addStretch(1)
        bl.addLayout(row)
        lay.addWidget(box)

        self.note = QLabel("")
        self.note.setStyleSheet("color: #888;")
        lay.addWidget(self.note)

    def load(self, chords):
        self.chords = chords
        self.list.clear()
        for c in chords:
            self.list.addItem(f"#{c['index']}   {members_str(c['members'])}  →  Button {c['output'] + 1}")
        used = {c["output"] for c in chords}
        free = next((o for o in range(OUT_MIN, OUT_MAX + 1) if o not in used), OUT_MIN)
        self.out.setCurrentIndex(self.out.findData(free))
        self.note.setText(f"{len(chords)} / {MAX_CHORDS} chords used.")

    def _add(self):
        mask = sum((1 << i) for i, cb in enumerate(self.member_cbs) if cb.isChecked())
        if bin(mask).count("1") < 2:
            self.note.setText("Pick at least 2 member buttons.")
            return
        if len(self.chords) >= MAX_CHORDS:
            self.note.setText(f"Chord limit reached ({MAX_CHORDS}).")
            return
        self.link.add_chord(mask, self.out.currentData())
        for cb in self.member_cbs:
            cb.setChecked(False)

    def _delete(self):
        row = self.list.currentRow()
        if 0 <= row < len(self.chords):
            self.link.del_chord(self.chords[row]["index"])


# ---------------------------------------------------------------- window -------
class MainWindow(QMainWindow):
    def __init__(self, link, tray):
        super().__init__()
        self.link = link
        self.tray = tray
        self._quitting = False
        self.setWindowTitle("Buttonbox Companion")
        self.resize(420, 460)

        self.monitor = MonitorTab()
        self.device = DeviceTab(link)
        self.chords = ChordsTab(link)
        tabs = QTabWidget()
        tabs.addTab(self.monitor, "Monitor")
        tabs.addTab(self.device, "Device")
        tabs.addTab(self.chords, "Chords")
        self.setCentralWidget(tabs)

        link.connected.connect(self._on_connected)
        link.disconnected.connect(self._on_disconnected)
        link.statsRead.connect(self.monitor.set_stats)
        link.configReceived.connect(self.device.load)
        link.chordsReceived.connect(self.chords.load)
        link.lhmStatus.connect(self._on_lhm)
        link.flashStarted.connect(self._on_flash_started)

    def _on_connected(self, port):
        self.monitor.status.setText(f"Connected  ({port})")
        self.monitor.status.setStyleSheet("font-weight: bold; color: #2e7d32;")
        self.tray.setToolTip(f"Buttonbox Companion — {port}")

    def _on_disconnected(self):
        self.monitor.status.setText("Disconnected — searching for device…")
        self.monitor.status.setStyleSheet("font-weight: bold; color: #c62828;")
        self.tray.setToolTip("Buttonbox Companion — searching…")

    def _on_flash_started(self):
        self.monitor.status.setText("Flashing — reconnecting automatically in ~35 s…")
        self.monitor.status.setStyleSheet("font-weight: bold; color: #e65100;")
        self.tray.setToolTip("Buttonbox Companion — flashing…")

    def _on_lhm(self, ok, err):
        self.monitor.lhm.setText(
            "Temperatures via LibreHardwareMonitor: OK." if ok else
            f"CPU temp/GPU limited (LibreHardwareMonitor not loaded): {err}")

    def closeEvent(self, e):
        if self._quitting:
            e.accept()
        else:                       # close-to-tray
            e.ignore()
            self.hide()


# ------------------------------------------------------------------- main ------
def main():
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    icon = make_icon()
    app.setWindowIcon(icon)

    link = DeviceLink()
    tray = QSystemTrayIcon(icon)
    tray.setToolTip("Buttonbox Companion — searching…")
    win = MainWindow(link, tray)

    menu = QMenu()
    act_show = QAction("Show", menu); menu.addAction(act_show)
    act_auto = QAction("Start at login", menu, checkable=True)
    act_auto.setChecked(is_autostart())
    menu.addAction(act_auto)
    act_flash = QAction("Flash device", menu); menu.addAction(act_flash)
    menu.addSeparator()
    act_quit = QAction("Quit", menu); menu.addAction(act_quit)
    tray.setContextMenu(menu)

    def toggle_window():
        if win.isVisible():
            win.hide()
        else:
            win.showNormal(); win.raise_(); win.activateWindow()

    def on_activated(reason):
        if reason in (QSystemTrayIcon.Trigger, QSystemTrayIcon.DoubleClick):
            toggle_window()

    def quit_app():
        win._quitting = True
        link.stop(); link.wait(1500)
        tray.hide()
        app.quit()

    act_show.triggered.connect(lambda: (win.showNormal(), win.raise_(), win.activateWindow()))
    act_auto.toggled.connect(set_autostart)
    act_flash.triggered.connect(link.flash)
    act_quit.triggered.connect(quit_app)
    tray.activated.connect(on_activated)

    # Restore the saved Shelly password (DPAPI-decrypted) so auto-polling works
    # immediately on connect — device never echoes passwords back.
    saved_pw = secrets_store.get_secret("shelly_pass")
    if saved_pw:
        link.set_shelly_pass(saved_pw)
        win.device.shelly_pass.setText(saved_pw)   # masked field (echo = Password)

    tray.show()
    link.start()                    # starts minimized: window stays hidden
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
