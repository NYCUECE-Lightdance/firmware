import sys
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QThread, Signal, QObject, QUrl
from PySide6.QtMultimedia import QMediaPlayer, QAudioOutput
import socket
import time
import threading
import struct
import os

from ui import DeviceMonitorWindow

# ============================================================
# MUSIC FILE PATH
# ============================================================
MUSIC_FILE = r"C:\School_2025\LightDance\picow-pio-template\test music\无标题视频——使用Clipchamp制作.mp3"


# ============================================================
# DEVICE STATE CLASS
# ============================================================
class DeviceState:
    def __init__(self, ip, device_id):
        self.ip = ip
        self.device_id = device_id
        self.last_response_time = None
        self.status = "Disconnected"
        self.task_status = "Waiting"


# ============================================================
# MUSIC PLAYER
# ============================================================
class MusicPlayer(QObject):
    def __init__(self):
        super().__init__()
        self.player = QMediaPlayer()
        self.audio_output = QAudioOutput()
        self.player.setAudioOutput(self.audio_output)
        self.startTime = 0

    def play_music(self, file_path):
        url = QUrl.fromLocalFile(file_path)
        self.player.setSource(url)
        self.player.setPosition(int(self.startTime * 1000))
        self.player.play()

    def stop_music(self):
        self.player.stop()

    def set_start_time(self, t):
        self.startTime = t


# ============================================================
# BACKGROUND THREADS
# ============================================================
class ResponseListener(QThread):
    """Listens for UDP responses from devices"""
    response_received = Signal(str, str, str)

    def __init__(self, sock, exit_event):
        super().__init__()
        self.sock = sock
        self.exit_event = exit_event

    def run(self):
        while not self.exit_event.is_set():
            try:
                data, addr = self.sock.recvfrom(1024)
                message = data.decode()
                device_ip = addr[0]

                if ":" in message:
                    device_id, task_status = map(str.strip, message.split(":", 1))
                else:
                    device_id, task_status = "Unknown", message

                self.response_received.emit(device_ip, device_id, task_status)
            except socket.timeout:
                continue
            except Exception:
                pass


class HeartbeatThread(QThread):
    """Periodically broadcasts heartbeat to discover devices"""
    def __init__(self, controller):
        super().__init__()
        self.controller = controller

    def run(self):
        while not self.controller.exit_event.is_set():
            if not self.controller.isRunning:
                self.controller.broadcast_message("heartbeat")

            connected_count = sum(
                1 for d in self.controller.devices.values() 
                if d.status != "Disconnected"
            )
            
            if connected_count == 0:
                time.sleep(0.5)
            else:
                time.sleep(0.1)


# ============================================================
# MAIN CONTROLLER
# ============================================================
class Controller:
    def __init__(self):
        # State
        self.devices = {}
        self.exit_event = threading.Event()
        self.current_broadcast_message = ""
        
        # Playback state
        self.isRunning = False
        self.rootTime = 0
        self.startTime = 0
        self.count = 0

        # Network setup
        self.port = 12345
        self.response_port = 12346
        self._setup_network()

        # Music player
        self.music_player = MusicPlayer()

    def _setup_network(self):
        """Setup UDP socket and broadcast address"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", self.response_port))
        self.sock.settimeout(0.1)

        # Get local IP
        temp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        temp_sock.connect(("8.8.8.8", 80))
        self.local_ip = temp_sock.getsockname()[0]
        temp_sock.close()
        print(f"Computer IP: {self.local_ip}")

        # Calculate broadcast address
        self.broadcast_address = None
        if "." in self.local_ip:
            octets = self.local_ip.split(".")
            if len(octets) == 4:
                self.broadcast_address = ".".join(octets[:3]) + ".255"

        if self.broadcast_address is None:
            raise RuntimeError("無法自動推算廣播位址，請檢查目前的網路設定。")

        print(f"Broadcast Address: {self.broadcast_address}")

    def setup_threads(self, window):
        """Setup background threads with window reference"""
        self.window = window
        
        # Response listener
        self.listener = ResponseListener(self.sock, self.exit_event)
        self.listener.response_received.connect(self._update_device_status)
        self.listener.start()

        # Heartbeat thread
        self.heartbeat_thread = HeartbeatThread(self)
        self.heartbeat_thread.start()

    def _update_device_status(self, device_ip, device_id, task_status):
        """Called when a device responds"""
        if device_ip not in self.devices:
            self.devices[device_ip] = DeviceState(device_ip, device_id)
        self.devices[device_ip].last_response_time = time.time()
        self.devices[device_ip].status = "Connected"
        self.devices[device_ip].task_status = task_status

    def broadcast_message(self, message):
        """Send a string message to all devices"""
        self.current_broadcast_message = str(message)
        self.sock.sendto(str(message).encode(), (self.broadcast_address, self.port))
        print(f"Broadcasted message: {message}")

    def broadcast_time(self):
        """Broadcasts elapsed time when running (called by timer)"""
        if self.isRunning:
            currentTime = time.time() * 1000
            if currentTime - self.rootTime >= 1000 * self.count:
                self.count += 1
                number = int(currentTime - self.rootTime + self.startTime * 1000)
                data = struct.pack("!I", number)
                self.current_broadcast_message = str(number)
                self.sock.sendto(data, (self.broadcast_address, self.port))
                print(f"Broadcasted time: {number} ms")

    # ============================================================
    # MUSIC HELPERS
    # ============================================================
    def start_music(self):
        """Start music playback from startTime offset"""
        try:
            if not os.path.exists(MUSIC_FILE):
                print(f"⚠ 音樂檔案不存在: {MUSIC_FILE}")
                return False
            self.music_player.set_start_time(self.startTime)
            self.music_player.play_music(MUSIC_FILE)
            return True
        except Exception:
            print("⚠ 無法播放音樂")
            return False

    def stop_music(self):
        """Stop music playback"""
        self.music_player.stop_music()

    # ============================================================
    # PLAYBACK CONTROL
    # ============================================================
    def start_function(self, window):
        self.startTime = window.get_time_value()  # Get from input
        self.rootTime = time.time() * 1000
        self.count = 0
        self.isRunning = True
        
        window.update_toggle_button(True)  # Show "Stop"
        self.start_music()

    def stop_function(self, window):
        elapsed_seconds = 0
        
        if self.rootTime != 0:
            elapsed_ms = time.time() * 1000 - self.rootTime + self.startTime * 1000
            elapsed_seconds = int(elapsed_ms / 1000)
            window.set_time_value(elapsed_seconds)  # Save for resume
        
        self.isRunning = False
        self.rootTime = 0
        
        window.update_toggle_button(False)  # Show "Start"
        self.stop_music()
        self.broadcast_message("stop")


# ============================================================
# ENTRY POINT
# ============================================================
if __name__ == "__main__":
    app = QApplication(sys.argv)
    
    controller = Controller()
    window = DeviceMonitorWindow(controller)
    window.show()
    
    sys.exit(app.exec())