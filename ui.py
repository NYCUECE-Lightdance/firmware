"""Light Dance UI: dancer-figure grid with playback controls below.

Exports:
    MonitorWindow  - main window; expects (controller, players, time_provider)
    PART_NAMES     - body part order, shared with the controller
"""
import re
import time as time_module

from PySide6.QtCore import QPointF, Qt, QTimer
from PySide6.QtGui import (QBrush, QColor, QFont, QPainter, QPainterPath,
                            QPolygonF)
from PySide6.QtWidgets import (QGridLayout, QHBoxLayout, QLabel, QLineEdit,
                                QMainWindow, QPushButton, QVBoxLayout, QWidget)

# ---- theme ----
BG       = "#111113"
SURFACE  = "#1a1a1f"
TEXT     = "#f0f0f2"
DIM      = "#7a7a85"
ACCENT   = "#22c55e"
ACCENT_H = "#16a34a"
DANGER   = "#ef4444"
DANGER_H = "#dc2626"

PART_NAMES = ["hat", "face", "chestL", "chestR", "armL", "armR", "tie",
              "belt", "gloveL", "gloveR", "legL", "legR", "shoeL", "shoeR",
              "board"]

# UI labels for the prop boards. Firmware PLAYER_NUM 1/3/4/6 displays as 2/4/5/7.
PROP_PLAYERS = [2, 4, 5, 7]


def parse_player_index(device_id):
    """'player3' -> 3. Returns None on anything else."""
    if not device_id:
        return None
    m = re.match(r"player(\d+)$", device_id.strip())
    return int(m.group(1)) if m else None


def parse_prop_index(device_id):
    """'prop_p3' -> 3. Returns None on anything else."""
    if not device_id:
        return None
    m = re.match(r"prop_p(\d+)$", device_id.strip())
    return int(m.group(1)) if m else None


# ============================================================
# DANCER WIDGET — paints one figure with per-part colors
# ============================================================
class DancerWidget(QWidget):
    def __init__(self, index):
        super().__init__()
        self.index = index
        self.colors = {n: (0, 0, 0) for n in PART_NAMES}
        self.online = False
        self.setFixedSize(170, 290)

    def set_colors(self, colors):
        self.colors = colors
        self.update()

    def set_online(self, online):
        if online != self.online:
            self.online = online
            self.update()

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.fillRect(self.rect(), QColor(SURFACE))

        # online dot + label
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(QColor(ACCENT if self.online else DANGER)))
        p.drawEllipse(QPointF(self.width() / 2 - 38, 12), 4, 4)

        p.setPen(QColor(TEXT if self.online else DIM))
        f = QFont()
        f.setPointSize(8)
        f.setBold(True)
        p.setFont(f)
        p.drawText(0, 4, self.width(), 16, Qt.AlignCenter,
                   f"Dancer {self.index + 1}")

        # SVG-like figure (viewBox 10 0 222 360, group translate 0,35)
        vb_x, vb_y, vb_w, vb_h = 10, 0, 222, 360
        avail_h = self.height() - 22
        s = min(self.width() / vb_w, avail_h / vb_h)
        ox = (self.width() - vb_w * s) / 2 - vb_x * s
        oy = 22 + (avail_h - vb_h * s) / 2 - vb_y * s
        p.translate(ox, oy)
        p.scale(s, s)
        p.translate(0, 35)
        p.setPen(Qt.NoPen)

        def br(name):
            r, g, b = self.colors[name]
            return QBrush(QColor(r, g, b))

        # hat
        path = QPainterPath()
        path.moveTo(96.8, 5)
        path.lineTo(145.2, 5)
        path.lineTo(145.2, 23)
        path.lineTo(169.4, 23)
        path.lineTo(169.4, 38)
        path.lineTo(72.6, 38)
        path.lineTo(72.6, 23)
        path.lineTo(96.8, 23)
        path.closeSubpath()
        p.fillPath(path, br("hat"))

        # face
        p.setBrush(br("face"))
        p.drawEllipse(QPointF(121, 68), 30, 30)

        # body rectangles
        for name, x, y, w, h in [
            ("chestL",  72, 103, 28, 65),
            ("chestR", 142, 103, 28, 65),
            ("armL",    35, 103, 32, 65),
            ("armR",   175, 103, 32, 65),
            ("tie",    105, 103, 32, 50),
            ("belt",    78, 173, 86, 35),
            ("gloveL",  35, 173, 32, 35),
            ("gloveR", 175, 173, 32, 35),
            ("legL",    85, 213, 28, 80),
            ("legR",   129, 213, 28, 80),
            ("shoeL",   75, 298, 45, 15),
            ("shoeR",  122, 298, 45, 15),
        ]:
            p.fillRect(x, y, w, h, br(name))

        # tie tip
        p.setBrush(br("tie"))
        p.drawPolygon(QPolygonF([QPointF(105, 153),
                                  QPointF(137, 153),
                                  QPointF(121, 173)]))


# ============================================================
# PROP STATUS — name + connection dot, one per prop player
# ============================================================
class PropWidget(QWidget):
    def __init__(self, player_num):
        super().__init__()
        self.player_num = player_num
        self.online = False
        self.setFixedSize(110, 32)

    def set_online(self, online):
        if online != self.online:
            self.online = online
            self.update()

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.fillRect(self.rect(), QColor(SURFACE))

        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(QColor(ACCENT if self.online else DANGER)))
        p.drawEllipse(QPointF(14, self.height() / 2), 4, 4)

        p.setPen(QColor(TEXT if self.online else DIM))
        f = QFont()
        f.setPointSize(9)
        f.setBold(True)
        p.setFont(f)
        p.drawText(28, 0, self.width() - 32, self.height(),
                   Qt.AlignVCenter | Qt.AlignLeft,
                   f"Prop p{self.player_num}")


# ============================================================
# MAIN WINDOW
# ============================================================
class MonitorWindow(QMainWindow):
    def __init__(self, controller, players, time_provider):
        super().__init__()
        self.controller = controller
        self.players = players
        self.time_provider = time_provider
        self.dancers = []
        self.props = {}  # player_num -> PropWidget

        self._init_ui()
        self._setup_timers()
        self.controller.setup_threads(self)

    def _init_ui(self):
        self.setWindowTitle("Light Dance Controller")
        self.setStyleSheet(f"""
            QMainWindow {{ background-color: {BG}; }}
            QWidget    {{ background-color: transparent; color: {TEXT}; }}
            QLabel     {{ border: none; background: transparent; }}
        """)

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(20, 20, 20, 20)
        root.setSpacing(16)
        root.addSpacing(40)

        # dancer grid (or fallback message if no light data)
        n = len(self.players) if self.players is not None else 0
        if n == 0:
            warn = QLabel("lightdata.npz not found.\n"
                          "Run: python fetch_lightdata.py")
            warn.setAlignment(Qt.AlignCenter)
            warn.setStyleSheet(
                f"font-size: 14px; color: {DANGER}; padding: 60px;")
            root.addWidget(warn, stretch=1)
        else:
            grid_host = QWidget()
            grid = QGridLayout(grid_host)
            grid.setContentsMargins(0, 0, 0, 0)
            grid.setSpacing(8)
            cols = 7 if n > 7 else max(n, 1)
            for i in range(n):
                w = DancerWidget(i)
                self.dancers.append(w)
                grid.addWidget(w, i // cols, i % cols)
            root.addWidget(grid_host, stretch=1)

        # prop status row, one indicator per known prop player
        prop_host = QWidget()
        prop_row = QHBoxLayout(prop_host)
        prop_row.setContentsMargins(0, 0, 0, 0)
        prop_row.setSpacing(8)
        prop_row.addStretch()
        for pn in PROP_PLAYERS:
            w = PropWidget(pn)
            self.props[pn] = w
            prop_row.addWidget(w)
        prop_row.addStretch()
        root.addWidget(prop_host)

        root.addSpacing(40)

        # bottom controls: [broadcast] ... [input] seconds [Start] [Exit]
        controls = QWidget()
        cl = QHBoxLayout(controls)
        cl.setContentsMargins(0, 0, 0, 0)
        cl.setSpacing(10)

        self.broadcast_label = QLabel("0")
        self.broadcast_label.setStyleSheet(
            f"font-family: 'Consolas', 'Monaco', monospace; "
            f"font-size: 13px; color: {TEXT};")
        cl.addWidget(self.broadcast_label)

        cl.addStretch()

        self.time_input = QLineEdit("0")
        self.time_input.setFixedWidth(100)
        self.time_input.setMinimumHeight(44)
        self.time_input.setAlignment(Qt.AlignCenter)
        self.time_input.setStyleSheet(f"""
            QLineEdit {{
                background-color: {SURFACE};
                border: none; border-radius: 8px;
                padding: 8px 14px;
                font-family: 'Consolas', 'Monaco', monospace;
                font-size: 16px; color: {TEXT};
            }}
        """)
        self.time_input.editingFinished.connect(self._on_input_changed)
        cl.addWidget(self.time_input)

        sec_label = QLabel("seconds")
        sec_label.setStyleSheet(f"font-size: 13px; color: {DIM};")
        cl.addWidget(sec_label)

        self.toggle_button = QPushButton("Start")
        self.toggle_button.setCursor(Qt.PointingHandCursor)
        self.toggle_button.setMinimumHeight(44)
        self.toggle_button.setMinimumWidth(140)
        self._style_toggle(False)
        self.toggle_button.clicked.connect(self._toggle_playback)
        cl.addWidget(self.toggle_button)

        self.exit_button = QPushButton("Exit")
        self.exit_button.setCursor(Qt.PointingHandCursor)
        self.exit_button.setMinimumHeight(44)
        self.exit_button.setMinimumWidth(90)
        self.exit_button.setStyleSheet(f"""
            QPushButton {{
                background-color: transparent;
                color: {DIM};
                border: 1px solid {DIM};
                border-radius: 8px;
                padding: 8px 20px; font-size: 15px;
            }}
            QPushButton:hover {{ color: {TEXT}; border-color: {TEXT}; }}
        """)
        self.exit_button.clicked.connect(self._exit_action)
        cl.addWidget(self.exit_button)

        root.addWidget(controls)

    def _style_toggle(self, running):
        bg = DANGER if running else ACCENT
        hover = DANGER_H if running else ACCENT_H
        self.toggle_button.setText("Stop" if running else "Start")
        self.toggle_button.setStyleSheet(f"""
            QPushButton {{
                background-color: {bg}; color: white; border: none;
                border-radius: 8px; padding: 8px 20px;
                font-size: 16px; font-weight: bold;
            }}
            QPushButton:hover {{ background-color: {hover}; }}
        """)

    def _setup_timers(self):
        self.update_timer = QTimer(self)
        self.update_timer.timeout.connect(self._update_ui)
        self.update_timer.start(33)  # ~30 Hz

        self.broadcast_timer = QTimer(self)
        self.broadcast_timer.timeout.connect(self.controller.broadcast_time)
        self.broadcast_timer.start(1000)

    # ---- callbacks the Controller calls back into ----
    def update_toggle_button(self, running):
        self._style_toggle(running)

    def get_time_value(self):
        try:
            time_str_raw = self.time_input.text().strip()
            if ":" in time_str_raw:
                # if format MM:SS return MM*60 + SS
                parts = time_str_raw.split(":")
                if len(parts) == 2:
                    minutes = float(parts[0]) if parts[0] else 0
                    seconds = float(parts[1]) if parts[1] else 0
                    return (minutes * 60) + seconds
            return int(float(time_str_raw))
        except ValueError:
            return 0

    def set_time_value(self, seconds):
        self.time_input.setText(str(int(max(0, seconds))))

    # ---- UI handlers ----
    def _on_input_changed(self):
        try:
            text = self.time_input.text().strip()
            if ":" in text:
                parts = text.split(":", 1)
                if len(parts) == 2:
                    minutes = int(parts[0]) if parts[0] else 0
                    seconds = int(parts[1]) if parts[1] else 0
                    v = max(0, minutes * 60 + seconds)
                else:
                    v = 0
            else:
                v = max(0, int(float(text)))
            self.time_input.setText(str(v))
        except ValueError:
            self.time_input.setText("0")

    def _toggle_playback(self):
        if self.controller.isRunning:
            self.controller.stop_function(self)
        else:
            self.controller.start_function(self)

    def _exit_action(self):
        self.controller.exit_event.set()
        self.close()

    def closeEvent(self, event):
        self.controller.exit_event.set()
        event.accept()

    # ---- per-tick refresh ----
    def _update_ui(self):
        msg = self.controller.current_broadcast_message
        self.broadcast_label.setText(msg if msg else "0")

        now = time_module.time()
        online_indices = set()
        online_props = set()
        for device in self.controller.devices.values():
            if device.last_response_time and now - device.last_response_time > 2:
                device.status = "Disconnected"
            if device.status == "Connected":
                idx = parse_player_index(device.device_id)
                if idx is not None:
                    online_indices.add(idx)
                    continue
                pidx = parse_prop_index(device.device_id)
                if pidx is not None:
                    online_props.add(pidx + 1)

        if self.dancers and self.players is not None:
            ticks = self.time_provider()
            for i, w in enumerate(self.dancers):
                w.set_online(i in online_indices)
                w.set_colors(self.players[i].colors_at(ticks))

        for pn, w in self.props.items():
            w.set_online(pn in online_props)
