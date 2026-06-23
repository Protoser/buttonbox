"""Buttonbox companion — system-tray app that streams PC stats to the box and
reads/edits its settings and chords live. Starts minimized to the tray.

The window is a left-sidebar settings app: a short list of focused panes
(Monitor / Display / Apps / Chords / Network / Device) rather than a few
overloaded tabs, so each screen stays small and single-purpose. A persistent
header shows the connection state from every pane.

Run:   python app.py        (or pythonw app.py for no console)
"""
import os
import sys

from PySide6.QtCore import Qt, QTimer, QUrl
from PySide6.QtGui import QAction, QColor, QFont, QIcon, QPainter, QPalette, QPixmap
from PySide6.QtWebSockets import QWebSocket
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFormLayout, QFrame, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMenu,
    QProgressBar, QPushButton, QScrollArea, QStackedWidget, QSystemTrayIcon, QTabWidget,
    QVBoxLayout, QWidget,
)

import mcdu as mcdu_mod
import secrets_store
from link import DeviceLink

MCDU_WS_URL = "ws://localhost:8380/interfaces/v1/mcdu"   # FlyByWire SimBridge remote MCDU

# ---- constants mirrored from the firmware (config.h / settings) ----
NUM_HID = 14            # physical buttons, shown 1..14
OUT_MIN, OUT_MAX = 14, 31     # chord outputs (shown 15..32)
MAX_CHORDS = 18
IDLE_OPTS  = [(0, "Off"), (30, "30 s"), (120, "2 min")]
CHORD_OPTS = [30, 40, 60, 80]
BOOT_OPTS  = [(0, "Apps launcher"), (1, "Buttons"), (2, "Timer"), (3, "PC"), (4, "Shelly"),
              (5, "Music"), (6, "Menu"), (7, "WLED"), (8, "BeamNG"), (9, "Flight"), (10, "MCDU")]
APP_NAMES  = ["Buttons", "Timer", "PC", "Shelly", "Music", "Menu", "WLED", "BeamNG", "Flight", "MCDU"]  # mirror ui.cpp APPS index order
MENU_APP   = APP_NAMES.index("Menu")   # never hideable
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


def pane_title(text):
    """A bold heading at the top of a sidebar pane."""
    lbl = QLabel(text)
    f = lbl.font(); f.setBold(True); f.setPointSize(f.pointSize() + 2); lbl.setFont(f)
    lbl.setStyleSheet("margin-bottom: 6px;")
    return lbl


# ------------------------------------------------------------------ panes ------
class SettingsPane(QWidget):
    """Base for panes that push a setting on change and reload it from `cfg`.

    `_loading` guards the change handlers so loading device config doesn't echo
    a `set` straight back. Each pane reads only the cfg keys it owns, so the
    window can fan a single `cfg` line out to every pane.
    """

    def __init__(self, link):
        super().__init__()
        self.link = link
        self._loading = False

    def _set(self, key, val):
        if not self._loading and val is not None:
            self.link.set_setting(key, val)

    def _sel(self, combo, val):
        idx = combo.findData(val)
        if idx < 0:
            combo.addItem(str(val), val)
            idx = combo.findData(val)
        combo.setCurrentIndex(idx)

    def load(self, cfg):        # overridden by panes that own settings
        pass


class MonitorPane(QWidget):
    """Live read-out: PC stat bars the box is currently being fed."""

    def __init__(self):
        super().__init__()
        lay = QVBoxLayout(self)
        lay.addWidget(pane_title("Monitor"))
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


class DisplayPane(SettingsPane):
    """How the box screen looks: orientation, grid labels, idle blank, boot screen."""

    def __init__(self, link):
        super().__init__(link)
        lay = QVBoxLayout(self)
        lay.addWidget(pane_title("Display"))
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

        self.boot = QComboBox()
        for val, text in BOOT_OPTS:
            self.boot.addItem(text, val)
        self.boot.currentIndexChanged.connect(lambda: self._set("boot", self.boot.currentData()))
        form.addRow("Boot screen", self.boot)

        lay.addLayout(form)
        lay.addStretch(1)

    def load(self, cfg):
        self._loading = True
        self.flip.setChecked(bool(cfg.get("flip", 0)))
        self._sel(self.labels, cfg.get("labels", 0))
        self._sel(self.idle, cfg.get("idle", 0))
        self._sel(self.boot, cfg.get("boot", 1))
        self._loading = False


class AppsPane(SettingsPane):
    """What content appears on the box: launcher app order/visibility and the
    PC-stats picker (which stats the PC page shows, up to PCSTAT_MAX)."""

    def __init__(self, link):
        super().__init__(link)
        lay = QVBoxLayout(self)
        lay.addWidget(pane_title("Apps & PC stats"))

        abox = QGroupBox("Launcher apps — drag to reorder, uncheck to hide (Menu always shown)")
        abl = QVBoxLayout(abox)
        self.app_list = QListWidget()
        self.app_list.setDragDropMode(QListWidget.InternalMove)
        self.app_list.setMaximumHeight(150)
        self.app_list.model().rowsMoved.connect(lambda *_: self._send_app_order())
        self.app_list.itemChanged.connect(lambda *_: self._send_app_hidden())
        abl.addWidget(self.app_list)
        lay.addWidget(abox)

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
        lay.addStretch(1)

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

    def _send_app_order(self):
        if self._loading:
            return
        order = [self.app_list.item(i).data(Qt.UserRole) for i in range(self.app_list.count())]
        self.link.set_setting("apporder", ",".join(str(a) for a in order))

    def _send_app_hidden(self):
        if self._loading:
            return
        mask = 0
        self.app_list.blockSignals(True)            # enforcing Menu mustn't re-fire itemChanged
        for i in range(self.app_list.count()):
            item = self.app_list.item(i)
            a = item.data(Qt.UserRole)
            if a == MENU_APP and item.checkState() != Qt.Checked:
                item.setCheckState(Qt.Checked)
            if item.checkState() == Qt.Unchecked:
                mask |= (1 << a)
        self.app_list.blockSignals(False)
        self.link.set_setting("apphidden", mask)

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

    def _remove_stat(self):
        row = self.stat_list.currentRow()
        if row >= 0:
            self.stat_list.takeItem(row)
            self.stat_note.setText("")
            self._update_add_combo()
            self._send_order()

    def load(self, cfg):
        self._loading = True
        order = cfg.get("pcorder", [0, 1, 2, 3, 4])
        self.stat_list.clear()
        for bit in order:
            if isinstance(bit, int) and 0 <= bit < len(PCSTAT_BITS):
                label = PCSTAT_BITS[bit][0]
                item = QListWidgetItem(label)
                item.setData(Qt.UserRole, bit)
                self.stat_list.addItem(item)
        self._update_add_combo()
        # Launcher app order — fixed set, drag to reorder; append any the box omits.
        # Each item has a checkbox: checked = shown, unchecked = hidden (Menu locked).
        aorder = cfg.get("apporder", list(range(len(APP_NAMES))))
        hidden = cfg.get("apphidden", 0)
        if not isinstance(hidden, int):
            hidden = 0
        self.app_list.clear()
        seen = set()
        for a in list(aorder) + list(range(len(APP_NAMES))):
            if isinstance(a, int) and 0 <= a < len(APP_NAMES) and a not in seen:
                item = QListWidgetItem(APP_NAMES[a])
                item.setData(Qt.UserRole, a)
                item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
                shown = a == MENU_APP or not (hidden >> a) & 1
                item.setCheckState(Qt.Checked if shown else Qt.Unchecked)
                if a == MENU_APP:                       # Menu can't be hidden
                    item.setFlags(item.flags() & ~Qt.ItemIsUserCheckable)
                self.app_list.addItem(item)
                seen.add(a)
        self._loading = False


class ChordsPane(SettingsPane):
    """Chord rules and their timing window — pressing the members together emits
    the output button. The chord-window setting lives here with the chords it
    governs (it arrives on the `cfg` line; the list on `chd` lines)."""

    def __init__(self, link):
        super().__init__(link)
        self.chords = []
        lay = QVBoxLayout(self)
        lay.addWidget(pane_title("Chords"))

        form = QFormLayout()
        self.chord = QComboBox()
        for val in CHORD_OPTS:
            self.chord.addItem(f"{val} ms", val)
        self.chord.currentIndexChanged.connect(lambda: self._set("chord", self.chord.currentData()))
        form.addRow("Chord window", self.chord)
        lay.addLayout(form)

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

    def load(self, cfg):
        self._loading = True
        self._sel(self.chord, cfg.get("chord", 40))
        self._loading = False

    def set_chords(self, chords):
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


class NetworkPane(SettingsPane):
    """WiFi + Shelly + WLED connectivity. Edits are staged in the fields and
    pushed together on Apply (the password also goes to the encrypted store)."""

    def __init__(self, link):
        super().__init__(link)
        lay = QVBoxLayout(self)
        lay.addWidget(pane_title("Connectivity"))

        cl = QFormLayout()
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
        self.wled_ip = QLineEdit(); self.wled_ip.setPlaceholderText("e.g. 192.168.1.101")
        cl.addRow("WLED IP", self.wled_ip)
        conn_apply = QPushButton("Apply")
        conn_apply.clicked.connect(self._apply_connectivity)
        cl.addRow("", conn_apply)
        lay.addLayout(cl)
        lay.addStretch(1)

    def _apply_connectivity(self):
        for k, w in (("wifi_ssid", self.wifi_ssid), ("wifi_pass", self.wifi_pass),
                     ("shelly_ip", self.shelly_ip), ("shelly_user", self.shelly_user),
                     ("wled_ip", self.wled_ip)):
            if w.text():
                self.link.set_setting(k, w.text())
        if self.shelly_pass.text():
            self.link.set_setting("shelly_pass", self.shelly_pass.text())
            self.link.set_shelly_pass(self.shelly_pass.text())
            secrets_store.set_secret("shelly_pass", self.shelly_pass.text())  # encrypted (DPAPI)
        self.link.set_setting("wifi_mode", self.wifi_mode_combo.currentData())

    def load(self, cfg):
        self._loading = True
        # Non-secret connectivity fields (passwords are never echoed back)
        if "wssid"  in cfg: self.wifi_ssid.setText(cfg["wssid"])
        if "ship"   in cfg: self.shelly_ip.setText(cfg["ship"])
        if "shuser" in cfg: self.shelly_user.setText(cfg["shuser"])
        if "wledip" in cfg: self.wled_ip.setText(cfg["wledip"])
        if "wmode"  in cfg:
            idx = self.wifi_mode_combo.findData(cfg["wmode"])
            if idx >= 0:
                self.wifi_mode_combo.setCurrentIndex(idx)
        self._loading = False


class DevicePane(QWidget):
    """Device actions: flash mode, re-read config, and Windows autostart."""

    def __init__(self, link):
        super().__init__()
        self.link = link
        lay = QVBoxLayout(self)
        lay.addWidget(pane_title("Device"))

        row = QHBoxLayout()
        flash = QPushButton("Put device in Flash Mode")
        flash.clicked.connect(self.link.flash)
        row.addWidget(flash)
        refresh = QPushButton("Refresh")
        refresh.clicked.connect(self.link.request_config)
        row.addWidget(refresh)
        row.addStretch(1)
        lay.addLayout(row)

        self.autostart = QCheckBox("Start companion at login")
        lay.addWidget(self.autostart)

        hint = QLabel("Flash Mode reboots the box into its bootloader so you can upload "
                      "new firmware; the companion reconnects automatically.")
        hint.setWordWrap(True)
        hint.setStyleSheet("color: #888;")
        lay.addWidget(hint)
        lay.addStretch(1)


# ---------------------------------------------------------------- window -------
class McduPane(QWidget):
    """FlyByWire MCDU: mirrors SimBridge's MCDU to the box and drives it.

    Owns a QWebSocket to SimBridge (event-driven, GUI thread). Incoming screen
    frames are flattened (mcdu.parse_update) and pushed to the box via link.set_mcdu().
    The box owns the button->output map (settings.mcduMap, editable here and on the
    device); it applies the map itself and reports key outputs as link.mcduKey, which
    we translate to events. A clickable faceplate plus physical-keyboard capture
    (while focused) send keys too. All paths end at `event:left:<NAME>`.
    """

    _KEY_LABEL = {"DIV": "/", "SP": "SP", "DOT": ".", "PLUSMINUS": "+/-"}
    # Physical buttons in device layout order: 5 rows x 2 grid (HID 0..9), then the
    # 4 nav keys (10..13). Label "B<n>" matches the on-device editor's numbering.
    _BTN_LABELS = [(f"B{i + 1}", i) for i in range(10)] + \
                  [("UP", 10), ("DOWN", 11), ("ENTER", 12), ("BACK", 13)]
    _SPECIAL_KEYS = {Qt.Key_Backspace: "CLR", Qt.Key_Delete: "CLR", Qt.Key_Left: "LEFT",
                     Qt.Key_Right: "RIGHT", Qt.Key_Up: "UP", Qt.Key_Down: "DOWN"}

    def __init__(self, link):
        super().__init__()
        self.link = link
        self._ws_connected = False
        self._loading = False           # guard so loading the device map doesn't echo a set
        self.setFocusPolicy(Qt.StrongFocus)

        lay = QVBoxLayout(self)
        lay.addWidget(pane_title("Flight Computer (MCDU)"))
        self.status_lbl = QLabel("MCDU: connecting…")
        self.status_lbl.setStyleSheet("color: #888;")
        lay.addWidget(self.status_lbl)

        tabs = QTabWidget()
        tabs.addTab(self._build_mcdu_tab(), "MCDU")
        tabs.addTab(self._build_remap_tab(), "Button map")
        lay.addWidget(tabs, 1)

        # WebSocket to SimBridge, with auto-reconnect.
        self.ws = QWebSocket()
        self.ws.connected.connect(self._on_ws_connected)
        self.ws.disconnected.connect(self._on_ws_disconnected)
        self.ws.textMessageReceived.connect(self._on_ws_message)
        err_sig = getattr(self.ws, "errorOccurred", None) or getattr(self.ws, "error", None)
        if err_sig is not None:
            err_sig.connect(lambda *_: self._schedule_reconnect())
        self._reconnect = QTimer(self)
        self._reconnect.setSingleShot(True)
        self._reconnect.setInterval(3000)
        self._reconnect.timeout.connect(self._connect_ws)
        link.mcduKey.connect(self.on_box_key)
        self._connect_ws()

    # ---- UI builders --------------------------------------------------------
    def _build_mcdu_tab(self):
        w = QWidget()
        v = QVBoxLayout(w)
        # On-PC mirror of the box screen.
        self.screen = QLabel("\n" * 13)
        mono = QFont("Consolas"); mono.setStyleHint(QFont.StyleHint.Monospace); mono.setPointSize(10)
        self.screen.setFont(mono)
        self.screen.setStyleSheet("background:#0a1f12; color:#7CFC00; padding:6px;")
        self.screen.setTextInteractionFlags(Qt.NoTextInteraction)
        v.addWidget(self.screen)

        hint = QLabel("Click keys, or focus this pane and type (letters/digits, "
                      "space, / . + −, Backspace = CLR, arrows = slew).")
        hint.setWordWrap(True); hint.setStyleSheet("color:#888;")
        v.addWidget(hint)

        face = QWidget()
        fl = QVBoxLayout(face); fl.setContentsMargins(0, 0, 0, 0)
        fl.addLayout(self._key_row(mcdu_mod.LSK_LEFT + mcdu_mod.LSK_RIGHT))
        fl.addLayout(self._key_grid(mcdu_mod.FUNCTION, 6))
        fl.addLayout(self._key_row(mcdu_mod.SLEW + mcdu_mod.EDIT))
        fl.addLayout(self._key_grid(mcdu_mod.LETTERS, 7))
        fl.addLayout(self._key_row(mcdu_mod.DIGITS))
        scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setWidget(face)
        scroll.setFrameShape(QFrame.NoFrame)
        v.addWidget(scroll, 1)
        return w

    def _key_button(self, name):
        btn = QPushButton(self._KEY_LABEL.get(name, name))
        btn.setFocusPolicy(Qt.NoFocus)          # keep keyboard focus on the pane
        btn.setFixedHeight(26)
        btn.clicked.connect(lambda _=False, n=name: self._send_key(n))
        return btn

    def _key_row(self, names):
        row = QHBoxLayout()
        for n in names:
            row.addWidget(self._key_button(n))
        return row

    def _key_grid(self, names, cols):
        grid = QGridLayout()
        for i, n in enumerate(names):
            grid.addWidget(self._key_button(n), i // cols, i % cols)
        return grid

    def _build_remap_tab(self):
        w = QWidget()
        v = QVBoxLayout(w)
        v.addWidget(QLabel("Map each device button (while the MCDU app is open) to an "
                           "MCDU output. Saved on the box; the menu/toggle button always "
                           "exits to the launcher. Also editable on the box: Menu → MCDU Keys."))
        grid = QGridLayout()
        self._combos = {}
        for slot, (label, idx) in enumerate(self._BTN_LABELS):
            combo = QComboBox()
            combo.setFocusPolicy(Qt.StrongFocus)
            for out_idx, (out_label, _ev) in enumerate(mcdu_mod.MCDU_OUTPUTS):
                combo.addItem(out_label, out_idx)
            combo.currentIndexChanged.connect(lambda *_: self._send_map())
            r, c = slot // 2, (slot % 2) * 2
            grid.addWidget(QLabel(label), r, c)
            grid.addWidget(combo, r, c + 1)
            self._combos[idx] = combo
        v.addLayout(grid)
        v.addStretch(1)
        return w

    # ---- map sync (device setting) -----------------------------------------
    def load(self, cfg):
        """Populate the remap dropdowns from the device's cfg (called on connect)."""
        m = cfg.get("mcdumap")
        if not m:
            return
        self._loading = True
        for idx, combo in self._combos.items():
            if idx < len(m):
                combo.setCurrentIndex(max(0, combo.findData(m[idx])))
        self._loading = False

    def _send_map(self):
        if self._loading:
            return
        out = [str(self._combos[i].currentData() or 0) for i in range(len(self._BTN_LABELS))]
        self.link.set_setting("mcdumap", ",".join(out))

    # ---- WebSocket ----------------------------------------------------------
    def _connect_ws(self):
        if not self._ws_connected:
            self.ws.open(QUrl(MCDU_WS_URL))

    def _schedule_reconnect(self):
        if not self._ws_connected and not self._reconnect.isActive():
            self._reconnect.start()

    def _on_ws_connected(self):
        self._ws_connected = True
        self.status_lbl.setText("MCDU: connected to SimBridge")
        self.link.set_mcdu(True, None)

    def _on_ws_disconnected(self):
        self._ws_connected = False
        self.status_lbl.setText("MCDU: SimBridge not found — is it running with an FBW aircraft? (retrying)")
        self.link.set_mcdu(False, None)
        self._schedule_reconnect()

    def _on_ws_message(self, message):
        out = mcdu_mod.parse_update(message)
        if out is None:
            return
        self.link.set_mcdu(True, out["rows"])
        self.screen.setText("\n".join(out["rows"]))

    def _send_key(self, name):
        if self._ws_connected and name in mcdu_mod.KEY_EVENTS:
            self.ws.sendTextMessage(f"event:left:{name}")

    # ---- box buttons + keyboard --------------------------------------------
    def on_box_key(self, out_idx):
        # The box already applied its map (and handles scroll locally); it only sends
        # real MCDU key outputs as 'mcdukey <out_idx>'. Translate and forward.
        ev = mcdu_mod.output_event(out_idx)
        if ev:
            self._send_key(ev)

    def keyPressEvent(self, e):
        name = mcdu_mod.KEYBOARD_CHAR_MAP.get(e.text()) or self._SPECIAL_KEYS.get(e.key())
        if name:
            self._send_key(name)
            e.accept()
        else:
            super().keyPressEvent(e)

    def showEvent(self, e):
        super().showEvent(e)
        self.setFocus()


class FlightPane(SettingsPane):
    """Flight-app utility settings: display units and the engine gauge style. Engine
    count follows the aircraft automatically (reported by SimConnect)."""

    def __init__(self, link):
        super().__init__(link)
        lay = QVBoxLayout(self)
        lay.addWidget(pane_title("Flight"))
        form = QFormLayout()
        self.speed = QComboBox(); self.speed.addItem("Knots", 0); self.speed.addItem("MPH", 1)
        self.alt = QComboBox(); self.alt.addItem("Feet", 0); self.alt.addItem("Metres", 1)
        self.eng = QComboBox(); self.eng.addItem("Dial gauges", 0); self.eng.addItem("EICAS bars", 1)
        for c in (self.speed, self.alt, self.eng):
            c.currentIndexChanged.connect(self._apply)
        form.addRow("Airspeed units", self.speed)
        form.addRow("Altitude units", self.alt)
        form.addRow("Engine gauges", self.eng)
        lay.addLayout(form)
        note = QLabel("Engine count (1–4) follows the aircraft automatically.")
        note.setStyleSheet("color: #888;"); note.setWordWrap(True)
        lay.addWidget(note)
        lay.addStretch(1)

    def _apply(self, *_):
        if self._loading:
            return
        funits = (1 if self.speed.currentData() else 0) | (2 if self.alt.currentData() else 0)
        self.link.set_setting("funits", funits)
        self.link.set_setting("engsty", self.eng.currentData())

    def load(self, cfg):
        self._loading = True
        if "funits" in cfg:
            self.speed.setCurrentIndex(1 if cfg["funits"] & 1 else 0)
            self.alt.setCurrentIndex(1 if cfg["funits"] & 2 else 0)
        if "engsty" in cfg:
            self.eng.setCurrentIndex(1 if cfg["engsty"] else 0)
        self._loading = False


class MainWindow(QMainWindow):
    def __init__(self, link, tray):
        super().__init__()
        self.link = link
        self.tray = tray
        self._quitting = False
        self.setWindowTitle("Buttonbox Companion")
        self.resize(580, 470)

        self.monitor = MonitorPane()
        self.display = DisplayPane(link)
        self.apps = AppsPane(link)
        self.chords = ChordsPane(link)
        self.network = NetworkPane(link)
        self.device = DevicePane(link)
        self.flight = FlightPane(link)
        self.mcdu = McduPane(link)
        panes = [("Monitor", self.monitor), ("Display", self.display), ("Apps", self.apps),
                 ("Chords", self.chords), ("Flight", self.flight), ("MCDU", self.mcdu),
                 ("Network", self.network), ("Device", self.device)]

        self.nav = QListWidget()
        self.nav.setFixedWidth(132)
        self.nav.setFrameShape(QFrame.NoFrame)
        # Drive the rail from the window's own palette so it blends with the panes
        # on any theme (light or dark). Hard-coded colours looked like a white slab.
        pal = self.palette()
        win_col = pal.color(QPalette.Window).name()
        div_col = pal.color(QPalette.Mid).name()
        hl_col  = pal.color(QPalette.Highlight).name()
        hlt_col = pal.color(QPalette.HighlightedText).name()
        txt_col = pal.color(QPalette.WindowText).name()
        self.nav.setStyleSheet(
            f"QListWidget {{ background: {win_col}; border: none;"
            f" border-right: 1px solid {div_col}; outline: 0; }}"
            f"QListWidget::item {{ padding: 9px 14px; border: none; color: {txt_col}; }}"
            f"QListWidget::item:selected {{ background: {hl_col}; color: {hlt_col}; }}")
        self.stack = QStackedWidget()
        for name, pane in panes:
            self.nav.addItem(QListWidgetItem(name))
            self.stack.addWidget(pane)
        self.nav.currentRowChanged.connect(self.stack.setCurrentIndex)
        self.nav.setCurrentRow(0)

        self.status = QLabel("Disconnected — searching for device…")
        self.status.setStyleSheet("font-weight: bold; padding: 8px 10px; color: #c62828;")

        body = QHBoxLayout()
        body.setContentsMargins(0, 0, 0, 0)
        body.addWidget(self.nav)
        body.addWidget(self.stack, 1)
        root = QVBoxLayout()
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        root.addWidget(self.status)
        root.addLayout(body, 1)
        central = QWidget()
        central.setLayout(root)
        self.setCentralWidget(central)

        link.connected.connect(self._on_connected)
        link.disconnected.connect(self._on_disconnected)
        link.statsRead.connect(self.monitor.set_stats)
        link.configReceived.connect(self._on_config)
        link.chordsReceived.connect(self.chords.set_chords)
        link.lhmStatus.connect(self._on_lhm)
        link.flashStarted.connect(self._on_flash_started)

    def _on_config(self, cfg):
        self.display.load(cfg)
        self.apps.load(cfg)
        self.chords.load(cfg)
        self.network.load(cfg)
        self.flight.load(cfg)
        self.mcdu.load(cfg)

    def _on_connected(self, port):
        self.status.setText(f"Connected  ({port})")
        self.status.setStyleSheet("font-weight: bold; padding: 8px 10px; color: #2e7d32;")
        self.tray.setToolTip(f"Buttonbox Companion — {port}")

    def _on_disconnected(self):
        self.status.setText("Disconnected — searching for device…")
        self.status.setStyleSheet("font-weight: bold; padding: 8px 10px; color: #c62828;")
        self.tray.setToolTip("Buttonbox Companion — searching…")

    def _on_flash_started(self):
        self.status.setText("Flashing — reconnecting automatically in ~35 s…")
        self.status.setStyleSheet("font-weight: bold; padding: 8px 10px; color: #e65100;")
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

    # Autostart is reachable from both the tray and the Device pane; keep the two
    # controls in sync and write the registry once, from a single handler.
    win.device.autostart.setChecked(is_autostart())

    def apply_autostart(enabled):
        set_autostart(enabled)
        for w in (act_auto, win.device.autostart):
            w.blockSignals(True)
            w.setChecked(enabled)
            w.blockSignals(False)

    act_auto.toggled.connect(apply_autostart)
    win.device.autostart.toggled.connect(apply_autostart)
    act_show.triggered.connect(lambda: (win.showNormal(), win.raise_(), win.activateWindow()))
    act_flash.triggered.connect(link.flash)
    act_quit.triggered.connect(quit_app)
    tray.activated.connect(on_activated)

    # Restore the saved Shelly password (DPAPI-decrypted) so auto-polling works
    # immediately on connect — device never echoes passwords back.
    saved_pw = secrets_store.get_secret("shelly_pass")
    if saved_pw:
        link.set_shelly_pass(saved_pw)
        win.network.shelly_pass.setText(saved_pw)   # masked field (echo = Password)

    tray.show()
    link.start()                    # starts minimized: window stays hidden
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
