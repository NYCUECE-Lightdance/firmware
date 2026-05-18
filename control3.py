"""Light Dance controller.

Broadcasts UDP heartbeats / time pulses to Pico W dancers, plays the music
track in sync, and previews each dancer's lights from lightdata.npz.

Run:  python control.py
"""
import os
import socket
import struct
import sys
import threading
import time

import numpy as np
from PySide6.QtCore import QObject, QThread, QUrl, Signal
from PySide6.QtMultimedia import QAudioOutput, QMediaDevices, QMediaPlayer
from PySide6.QtWidgets import QApplication

from ui import MonitorWindow, PART_NAMES

# ============================================================
# CONFIG
# ============================================================
MUSIC_FILE = r"C:\School_2025\LightDance\picow-pio-template\test music\2026_show.mp3"
MUSIC_OFFSET = 0.2  # +ve = music ahead of broadcast, -ve = behind

LIGHTDATA_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "lightdata.npz")

PORT          = 12345  # devices listen here
RESPONSE_PORT = 12346  # we listen here

# Column N+1 of a frame row is part N's color word (column 0 is the timestamp).
PART_COL = {name: i + 1 for i, name in enumerate(PART_NAMES)}

# 4-bit brightness -> 0..255 with gamma 2.2 (matches firmware brightnessFrom).
_BRI_LUT = np.array(
    [int((b / 15.0) ** 2.2 * 255 + 0.5) for b in range(16)], dtype=np.uint8)


# ============================================================
# LIGHT DATA — per-player frame timeline + color decoder
# ============================================================
class PlayerData:
    def __init__(self, frames):
        if frames.shape[0] == 0:
            self.frames = np.zeros((0, 16), dtype=np.uint32)
            self.times = np.zeros(0, dtype=np.uint32)
        else:
            self.frames = frames.astype(np.uint32, copy=False)
            self.times = self.frames[:, 0]

    def colors_at(self, ticks):
        """Return {part_name: (R, G, B)} at tick `ticks` (50ms units).
        Decodes like the firmware: brightness from bits 7..4 (gamma 2.2),
        RGB from bits 31..8, linear-blend to next frame on bit 0.
        """
        if self.times.size == 0:
            return {n: (0, 0, 0) for n in PART_NAMES}

        idx = int(np.searchsorted(self.times, ticks, side="right")) - 1
        if idx < 0:
            return {n: (0, 0, 0) for n in PART_NAMES}

        next_idx = idx + 1 if idx + 1 < self.times.size else None
        if next_idx is not None:
            t0 = int(self.times[idx]) * 50
            t1 = int(self.times[next_idx]) * 50
            blend = (ticks * 50 - t0) / (t1 - t0) if t1 > t0 else 0.0
            blend = 0.0 if blend < 0 else (1.0 if blend > 1 else blend)
        else:
            blend = 0.0

        result = {}
        for name in PART_NAMES:
            cur = int(self.frames[idx, PART_COL[name]])
            r = (cur >> 24) & 0xFF
            g = (cur >> 16) & 0xFF
            b = (cur >> 8) & 0xFF
            bri = int(_BRI_LUT[(cur >> 4) & 0x0F])
            if (cur & 1) and next_idx is not None:
                nxt = int(self.frames[next_idx, PART_COL[name]])
                nr = (nxt >> 24) & 0xFF
                ng = (nxt >> 16) & 0xFF
                nb = (nxt >> 8) & 0xFF
                r = int(r * (1 - blend) + nr * blend)
                g = int(g * (1 - blend) + ng * blend)
                b = int(b * (1 - blend) + nb * blend)
            result[name] = ((r * bri) // 255,
                            (g * bri) // 255,
                            (b * bri) // 255)
        return result


def load_lightdata(path):
    if not os.path.exists(path):
        return None
    with np.load(path) as f:
        keys = sorted((k for k in f.files if k.startswith("player_")),
                      key=lambda s: int(s.split("_")[1]))
        return [PlayerData(f[k]) for k in keys]


# ============================================================
# DEVICE STATE
# ============================================================
class DeviceState:
    def __init__(self, ip, device_id):
        self.ip = ip
        self.device_id = device_id
        self.last_response_time = None
        self.status = "Disconnected"
        self.task_status = "Waiting"


# ============================================================
# MUSIC — preloaded at startup so Start is instant
# ============================================================
class MusicPlayer(QObject):
    started = Signal()  # emits when audio actually begins playing

    def __init__(self, file_path):
        super().__init__()
        self.player = QMediaPlayer()
        self.audio_output = QAudioOutput()
        self.player.setAudioOutput(self.audio_output)

        # Route music to whatever Windows currently uses as the default
        # output device, and keep following it when devices are plugged in,
        # unplugged, or the default is changed.
        self._media_devices = QMediaDevices(self)
        self._media_devices.audioOutputsChanged.connect(self._sync_output_device)
        self._sync_output_device()

        self.startTime = 0
        self._want_play = False
        self._fired_started = False
        self.player.mediaStatusChanged.connect(self._on_status_changed)
        self.player.playbackStateChanged.connect(self._on_playback_state)
        self.player.setSource(QUrl.fromLocalFile(file_path))

    def _sync_output_device(self):
        """Point the audio output at the current Windows default device."""
        device = QMediaDevices.defaultAudioOutput()
        if device.isNull():
            return
        if self.audio_output.device() != device:
            self.audio_output.setDevice(device)
            print(f"🔊 Audio output: {device.description()}")

    def play_music(self):
        # Re-check in case the default changed while idle.
        self._sync_output_device()
        self._want_play = True
        self._fired_started = False
        status = self.player.mediaStatus()
        if status in (QMediaPlayer.MediaStatus.LoadedMedia,
                      QMediaPlayer.MediaStatus.BufferedMedia,
                      QMediaPlayer.MediaStatus.EndOfMedia):
            self._seek_and_play()

    def stop_music(self):
        self._want_play = False
        self.player.stop()

    def set_start_time(self, t):
        self.startTime = t

    def _seek_and_play(self):
        self.player.setPosition(max(0, int(self.startTime * 1000)))
        self.player.play()

    def _on_status_changed(self, status):
        if (status == QMediaPlayer.MediaStatus.LoadedMedia
                and self._want_play
                and self.player.playbackState() != QMediaPlayer.PlaybackState.PlayingState):
            self._seek_and_play()

    def _on_playback_state(self, state):
        if (state == QMediaPlayer.PlaybackState.PlayingState
                and self._want_play
                and not self._fired_started):
            self._fired_started = True
            self.started.emit()


# ============================================================
# BACKGROUND THREADS
# ============================================================
class ResponseListener(QThread):
    """Listens for UDP responses 'device_id:task_status' from devices."""
    response_received = Signal(str, str, str)

    def __init__(self, sock, exit_event):
        super().__init__()
        self.sock = sock
        self.exit_event = exit_event

    def run(self):
        while not self.exit_event.is_set():
            try:
                data, addr = self.sock.recvfrom(1024)
                msg = data.decode()
                if ":" in msg:
                    device_id, task_status = map(str.strip, msg.split(":", 1))
                else:
                    device_id, task_status = "Unknown", msg
                self.response_received.emit(addr[0], device_id, task_status)
            except socket.timeout:
                continue
            except Exception:
                pass


class HeartbeatThread(QThread):
    """Broadcasts 'heartbeat' to discover devices when not running."""
    def __init__(self, controller):
        super().__init__()
        self.controller = controller

    def run(self):
        while not self.controller.exit_event.is_set():
            if not self.controller.isRunning:
                self.controller.broadcast_message("heartbeat")

            connected = sum(1 for d in self.controller.devices.values()
                            if d.status != "Disconnected")
            time.sleep(0.1 if connected else 0.5)


# ============================================================
# CONTROLLER
# ============================================================
class Controller:
    def __init__(self):
        self.devices = {}
        self.exit_event = threading.Event()
        self.current_broadcast_message = ""

        # playback state
        self.isRunning = False
        self.rootTime = 0   # ms timestamp when broadcast clock started
        self.startTime = 0  # seconds offset configured by user
        self.music_offset = MUSIC_OFFSET
        self.count = 0

        self.port = PORT
        self.response_port = RESPONSE_PORT
        self._setup_network()

        if os.path.exists(MUSIC_FILE):
            self.music_player = MusicPlayer(MUSIC_FILE)
            self.music_player.started.connect(self._on_music_started)
        else:
            print(f"⚠ 音樂檔案不存在: {MUSIC_FILE}")
            self.music_player = None

    def _setup_network(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", self.response_port))
        self.sock.settimeout(0.1)

        # Discover local IP by opening a dummy outbound UDP "connection"
        temp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        temp.connect(("8.8.8.8", 80))
        self.local_ip = temp.getsockname()[0]
        temp.close()

        octets = self.local_ip.split(".")
        if len(octets) != 4:
            raise RuntimeError("無法自動推算廣播位址，請檢查目前的網路設定。")
        self.broadcast_address = ".".join(octets[:3]) + ".255"

        print(f"Computer IP:       {self.local_ip}")
        print(f"Broadcast Address: {self.broadcast_address}")

    def setup_threads(self, window):
        """Called by the window once it's ready to receive updates."""
        self.window = window
        self.listener = ResponseListener(self.sock, self.exit_event)
        self.listener.response_received.connect(self._update_device_status)
        self.listener.start()
        self.heartbeat_thread = HeartbeatThread(self)
        self.heartbeat_thread.start()

    def _update_device_status(self, ip, device_id, task_status):
        if ip not in self.devices:
            self.devices[ip] = DeviceState(ip, device_id)
        d = self.devices[ip]
        d.last_response_time = time.time()
        d.status = "Connected"
        d.task_status = task_status

    # ---- broadcasts ----
    def broadcast_message(self, message):
        self.current_broadcast_message = str(message)
        self.sock.sendto(str(message).encode(),
                         (self.broadcast_address, self.port))

    def broadcast_time(self):
        """Emit the current elapsed-ms big-endian uint32 once per second."""
        if not self.isRunning:
            return
        now_ms = time.time() * 1000
        if now_ms - self.rootTime >= 1000 * self.count:
            self.count += 1
            number = int(now_ms - self.rootTime + self.startTime * 1000)
            self.current_broadcast_message = str(number)
            self.sock.sendto(struct.pack("!I", number),
                             (self.broadcast_address, self.port))

    # ---- music ----
    def start_music(self):
        if self.music_player is None:
            return False
        self.music_player.set_start_time(self.startTime + self.music_offset)
        self.music_player.play_music()
        return True

    def stop_music(self):
        if self.music_player:
            self.music_player.stop_music()

    def _on_music_started(self):
        # Align broadcast clock to the moment audio actually begins.
        self.rootTime = time.time() * 1000
        self.count = 0
        self.isRunning = True

    # ---- playback control (called by UI) ----
    def start_function(self, window):
        self.startTime = window.get_time_value()
        self.count = 0
        window.update_toggle_button(True)
        if not self.start_music():
            # No music — start broadcast clock immediately.
            self.rootTime = time.time() * 1000
            self.isRunning = True

    def stop_function(self, window):
        if self.rootTime != 0:
            elapsed_ms = time.time() * 1000 - self.rootTime + self.startTime * 1000
            window.set_time_value(int(elapsed_ms / 1000))  # save for resume
        self.isRunning = False
        self.rootTime = 0
        window.update_toggle_button(False)
        self.stop_music()
        self.broadcast_message("stop")


def make_time_provider(controller):
    """Returns callable() -> current playback ticks (50ms units),
    aligned with the same clock the UDP broadcast uses."""
    def _ticks():
        if controller.isRunning and controller.rootTime != 0:
            elapsed_ms = (time.time() * 1000 - controller.rootTime
                          + controller.startTime * 1000)
        else:
            elapsed_ms = controller.startTime * 1000
        return max(0, int(elapsed_ms / 50))
    return _ticks


# ============================================================
# ENTRY POINT
# ============================================================
if __name__ == "__main__":
    app = QApplication(sys.argv)

    players = load_lightdata(LIGHTDATA_FILE)
    if players is None:
        print(f"⚠ {LIGHTDATA_FILE} not found. "
              f"Run `python fetch_lightdata.py` once with internet.")

    controller = Controller()
    window = MonitorWindow(controller, players, make_time_provider(controller))
    window.show()
    sys.exit(app.exec())
