from PySide6.QtWidgets import (QMainWindow, QWidget, QVBoxLayout, 
                                QHBoxLayout, QPushButton, QLabel, QLineEdit, 
                                QScrollArea, QFrame)
from PySide6.QtCore import Qt, QTimer

# ============================================================
# THEME COLORS
# ============================================================
COLORS = {
    "bg_primary": "#111113",
    "bg_secondary": "#1a1a1f",
    "bg_card": "#222228",
    "border": "#2e2e35",
    "text_primary": "#f0f0f2",
    "text_secondary": "#7a7a85",
    "accent": "#22c55e",
    "accent_hover": "#16a34a",
    "danger": "#ef4444",
    "blue": "#3b82f6",
}

WIDTH, HEIGHT = 750, 700


# ============================================================
# MAIN WINDOW
# ============================================================
class DeviceMonitorWindow(QMainWindow):
    def __init__(self, controller):
        """
        Args:
            controller: Object with these attributes/methods:
                - devices (dict)
                - current_broadcast_message (str)
                - broadcast_address (str)
                - port (int)
                - isRunning (bool)
                - start_function(window)
                - stop_function(window)
                - exit_event
                - setup_threads(window)
                - broadcast_time()
        """
        super().__init__()
        self.controller = controller
        self.init_ui()
        self.setup_timers()
        
        # Let controller setup threads with window reference
        self.controller.setup_threads(self)

    def init_ui(self):
        self.setWindowTitle("Light Dance Controller")
        self.setGeometry(100, 100, WIDTH, HEIGHT)
        self.setMinimumSize(600, 500)
        
        # Main stylesheet
        self.setStyleSheet(f"""
            QMainWindow {{
                background-color: {COLORS["bg_primary"]};
            }}
            QWidget {{
                background-color: transparent;
                color: {COLORS["text_primary"]};
            }}
            QLabel {{
                border: none;
                background: transparent;
            }}
            QScrollArea {{
                border: none;
                background-color: transparent;
            }}
            QScrollBar:vertical {{
                background: transparent;
                width: 8px;
                margin: 0;
            }}
            QScrollBar::handle:vertical {{
                background: {COLORS["border"]};
                border-radius: 4px;
                min-height: 30px;
            }}
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
                height: 0;
            }}
        """)

        # Central widget
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(40, 30, 40, 30)
        main_layout.setSpacing(20)

        # Header
        header = QWidget()
        header_layout = QVBoxLayout(header)
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(4)
        
        title = QLabel("Light Dance Controller")
        title.setStyleSheet(f"""
            font-size: 24px;
            font-weight: bold;
            color: {COLORS["text_primary"]};
            border: none;
            background: transparent;
        """)
        header_layout.addWidget(title)
        
        subtitle = QLabel("Device Monitor & Sync")
        subtitle.setStyleSheet(f"""
            font-size: 14px;
            color: {COLORS["text_secondary"]};
            border: none;
            background: transparent;
        """)
        header_layout.addWidget(subtitle)
        main_layout.addWidget(header)

        # Stats bar (including broadcast)
        stats_bar = QWidget()
        stats_layout = QHBoxLayout(stats_bar)
        stats_layout.setContentsMargins(0, 0, 0, 0)
        stats_layout.setSpacing(12)

        self.connected_label = self._create_stat_card("0", "Connected", COLORS["accent"])
        self.disconnected_label = self._create_stat_card("0", "Disconnected", COLORS["danger"])
        self.total_label = self._create_stat_card("0", "Total", COLORS["blue"])
        self.broadcast_card = self._create_broadcast_card()

        stats_layout.addWidget(self.connected_label)
        stats_layout.addWidget(self.disconnected_label)
        stats_layout.addWidget(self.total_label)
        stats_layout.addWidget(self.broadcast_card)
        main_layout.addWidget(stats_bar)

        # Device list
        device_card = QFrame()
        device_card.setStyleSheet(f"""
            QFrame {{
                background-color: {COLORS["bg_card"]};
                border: 1px solid {COLORS["border"]};
                border-radius: 10px;
            }}
        """)
        device_card_layout = QVBoxLayout(device_card)
        device_card_layout.setContentsMargins(0, 0, 0, 0)
        
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setMaximumHeight(350)  # Increased from 200 to 350
        
        scroll_widget = QWidget()
        self.device_layout = QVBoxLayout(scroll_widget)
        self.device_layout.setContentsMargins(0, 0, 0, 0)
        self.device_layout.setSpacing(0)
        self.device_layout.addStretch()
        
        scroll.setWidget(scroll_widget)
        device_card_layout.addWidget(scroll)
        main_layout.addWidget(device_card)

        # Control panel
        control_card = QFrame()
        control_card.setStyleSheet(f"""
            QFrame {{
                background-color: {COLORS["bg_card"]};
                border: 1px solid {COLORS["border"]};
                border-radius: 10px;
            }}
        """)
        control_layout = QVBoxLayout(control_card)
        control_layout.setContentsMargins(28, 28, 28, 28)
        control_layout.setSpacing(20)

        # Time header
        time_header = QWidget()
        time_header_layout = QHBoxLayout(time_header)
        time_header_layout.setContentsMargins(0, 0, 0, 0)
        
        time_label = QLabel("Start Time")
        time_label.setStyleSheet(f"font-size: 14px; color: {COLORS['text_secondary']}; border: none; background: transparent;")
        time_header_layout.addWidget(time_label)
        
        time_header_layout.addStretch()
        control_layout.addWidget(time_header)

        # Time input row
        input_row = QWidget()
        input_layout = QHBoxLayout(input_row)
        input_layout.setContentsMargins(0, 0, 0, 0)
        input_layout.setSpacing(12)

        self.time_input = QLineEdit("0")
        self.time_input.setFixedWidth(120)
        self.time_input.setMinimumHeight(56)
        self.time_input.setAlignment(Qt.AlignCenter)
        self.time_input.setStyleSheet(f"""
            QLineEdit {{
                background-color: {COLORS["bg_secondary"]};
                border: none;
                border-radius: 10px;
                padding: 12px 16px;
                font-family: 'Consolas', 'Monaco', monospace;
                font-size: 18px;
                color: {COLORS["text_primary"]};
            }}
            QLineEdit:focus {{
                background-color: {COLORS["bg_card"]};
            }}
        """)
        self.time_input.editingFinished.connect(self.on_input_changed)
        input_layout.addWidget(self.time_input)
        
        seconds_label = QLabel("seconds")
        seconds_label.setStyleSheet(f"font-size: 14px; color: {COLORS['text_secondary']}; border: none; background: transparent;")
        input_layout.addWidget(seconds_label)
        
        input_layout.addStretch()
        control_layout.addWidget(input_row)

        # Button row
        button_row = QWidget()
        button_layout = QHBoxLayout(button_row)
        button_layout.setContentsMargins(0, 12, 0, 0)
        button_layout.setSpacing(12)

        self.toggle_button = QPushButton("Start")
        self.toggle_button.setCursor(Qt.PointingHandCursor)
        self.toggle_button.setMinimumHeight(60)
        self.toggle_button.setStyleSheet(f"""
            QPushButton {{
                background-color: {COLORS["accent"]};
                color: white;
                border: none;
                border-radius: 10px;
                padding: 16px 32px;
                font-size: 18px;
                font-weight: bold;
            }}
            QPushButton:hover {{
                background-color: {COLORS["accent_hover"]};
            }}
        """)
        self.toggle_button.clicked.connect(self.toggle_playback)
        button_layout.addWidget(self.toggle_button, stretch=2)

        self.exit_button = QPushButton("Exit")
        self.exit_button.setCursor(Qt.PointingHandCursor)
        self.exit_button.setMinimumHeight(60)
        self.exit_button.setStyleSheet(f"""
            QPushButton {{
                background-color: {COLORS["bg_secondary"]};
                color: {COLORS["text_secondary"]};
                border: 1px solid {COLORS["border"]};
                border-radius: 10px;
                padding: 16px 32px;
                font-size: 18px;
                font-weight: bold;
            }}
            QPushButton:hover {{
                background-color: {COLORS["border"]};
                color: {COLORS["text_primary"]};
            }}
        """)
        self.exit_button.clicked.connect(self.exit_action)
        button_layout.addWidget(self.exit_button, stretch=1)

        control_layout.addWidget(button_row)
        main_layout.addWidget(control_card)

        # Footer
        footer = QLabel(f"{self.controller.broadcast_address}:{self.controller.port}")
        footer.setAlignment(Qt.AlignCenter)
        footer.setStyleSheet(f"""
            font-family: 'Consolas', 'Monaco', monospace;
            font-size: 11px;
            color: {COLORS["text_secondary"]};
            padding: 8px;
            border: none;
            background: transparent;
        """)
        main_layout.addWidget(footer)

        # Store device labels
        self.device_labels = {}

    def _create_stat_card(self, value, label, color):
        """Create a stat card widget"""
        card = QFrame()
        card.setStyleSheet(f"""
            QFrame {{
                background-color: {COLORS["bg_card"]};
                border: 1px solid {COLORS["border"]};
                border-radius: 8px;
            }}
        """)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(2)

        value_label = QLabel(value)
        value_label.setObjectName("value")
        value_label.setStyleSheet(f"""
            font-family: 'Consolas', 'Monaco', monospace;
            font-size: 18px;
            font-weight: bold;
            color: {color};
            border: none;
            background: transparent;
        """)
        layout.addWidget(value_label)

        text_label = QLabel(label)
        text_label.setStyleSheet(f"""
            font-size: 10px;
            color: {COLORS["text_secondary"]};
            text-transform: uppercase;
            letter-spacing: 0.5px;
            border: none;
            background: transparent;
        """)
        layout.addWidget(text_label)

        return card

    def _create_broadcast_card(self):
        """Create broadcast display card"""
        card = QFrame()
        card.setStyleSheet(f"""
            QFrame {{
                background-color: {COLORS["bg_card"]};
                border: 1px solid {COLORS["border"]};
                border-radius: 8px;
            }}
        """)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(2)

        self.broadcast_value = QLabel("0")
        self.broadcast_value.setObjectName("broadcast_value")
        self.broadcast_value.setStyleSheet(f"""
            font-family: 'Consolas', 'Monaco', monospace;
            font-size: 18px;
            font-weight: bold;
            color: {COLORS["text_primary"]};
            border: none;
            background: transparent;
        """)
        layout.addWidget(self.broadcast_value)

        label = QLabel("BROADCAST")
        label.setStyleSheet(f"""
            font-size: 10px;
            color: {COLORS["text_secondary"]};
            text-transform: uppercase;
            letter-spacing: 0.5px;
            border: none;
            background: transparent;
        """)
        layout.addWidget(label)

        return card

    def _create_device_row(self, device_id, ip, status, last_seen):
        """Create a compact device row widget"""
        row = QFrame()
        is_connected = status == "Connected"
        
        import re
        display_name = re.sub(r'(\D+)(\d+)', r'\1 \2', device_id).title()
        
        row.setStyleSheet(f"""
            QFrame {{
                background-color: transparent;
                border-bottom: 1px solid {COLORS["border"]};
            }}
            QFrame:hover {{
                background-color: rgba(255, 255, 255, 0.02);
            }}
        """)
        
        layout = QHBoxLayout(row)
        layout.setContentsMargins(12, 6, 12, 6)  # Reduced from 18,12,18,12
        layout.setSpacing(10)  # Reduced from 12

        # Icon - smaller
        icon = QLabel("  ")
        icon.setFixedSize(28, 28)  # Reduced from 36x36
        icon.setAlignment(Qt.AlignCenter)
        icon.setStyleSheet(f"""
            background-color: {COLORS["bg_secondary"]};
            border: none;
            border-radius: 6px;
            font-family: 'Consolas', 'Monaco', monospace;
            font-size: 10px;
            font-weight: bold;
            color: {COLORS["text_secondary"]};
        """)
        layout.addWidget(icon)

        # Info - inline layout for compactness
        info = QWidget()
        info_layout = QHBoxLayout(info)  # Changed to horizontal
        info_layout.setContentsMargins(0, 0, 0, 0)
        info_layout.setSpacing(8)

        name = QLabel(display_name)
        name.setStyleSheet(f"""
            font-size: 15px;
            font-weight: 500;
            color: {COLORS["text_primary"]};
            border: none;
            background: transparent;
        """)
        info_layout.addWidget(name)

        ip_label = QLabel(f"({ip})")
        ip_label.setStyleSheet(f"""
            font-family: 'Consolas', 'Monaco', monospace;
            font-size: 11px;
            color: {COLORS["text_secondary"]};
            border: none;
            background: transparent;
        """)
        info_layout.addWidget(ip_label)
        layout.addWidget(info)

        layout.addStretch()

        # Status - more compact
        status_widget = QWidget()
        status_layout = QHBoxLayout(status_widget)
        status_layout.setContentsMargins(0, 0, 0, 0)
        status_layout.setSpacing(6)

        dot = QLabel()
        dot.setFixedSize(6, 6)  # Reduced from 8x8
        dot_color = COLORS["accent"] if is_connected else COLORS["danger"]
        dot.setStyleSheet(f"""
            background-color: {dot_color};
            border: none;
            border-radius: 3px;
        """)
        status_layout.addWidget(dot)

        time_label = QLabel(last_seen)
        time_label.setStyleSheet(f"""
            font-size: 10px;
            color: {COLORS["text_secondary"]};
            border: none;
            background: transparent;
        """)
        status_layout.addWidget(time_label)
        layout.addWidget(status_widget)

        return row

    def setup_timers(self):
        # UI refresh timer
        self.update_timer = QTimer()
        self.update_timer.timeout.connect(self.update_ui)
        self.update_timer.start(50)

        # Time broadcast timer
        self.broadcast_timer = QTimer()
        self.broadcast_timer.timeout.connect(self.controller.broadcast_time)
        self.broadcast_timer.start(1000)

    def on_input_changed(self):
        """Validate input when changed"""
        try:
            value = int(self.time_input.text())
            value = max(0, value)
            self.time_input.setText(str(value))
        except ValueError:
            self.time_input.setText("0")

    def toggle_playback(self):
        """Toggle between start and stop"""
        if self.controller.isRunning:
            self.controller.stop_function(self)
        else:
            self.controller.start_function(self)

    def update_toggle_button(self, running):
        """Update button appearance based on state"""
        if running:
            self.toggle_button.setText("Stop")
            self.toggle_button.setStyleSheet(f"""
                QPushButton {{
                    background-color: {COLORS["danger"]};
                    color: white;
                    border: none;
                    border-radius: 10px;
                    padding: 16px 32px;
                    font-size: 18px;
                    font-weight: bold;
                }}
                QPushButton:hover {{
                    background-color: #dc2626;
                }}
            """)
        else:
            self.toggle_button.setText("Start")
            self.toggle_button.setStyleSheet(f"""
                QPushButton {{
                    background-color: {COLORS["accent"]};
                    color: white;
                    border: none;
                    border-radius: 10px;
                    padding: 16px 32px;
                    font-size: 18px;
                    font-weight: bold;
                }}
                QPushButton:hover {{
                    background-color: {COLORS["accent_hover"]};
                }}
            """)

    def set_time_value(self, seconds):
        """Set the time input to a specific value"""
        seconds = int(max(0, seconds))
        self.time_input.setText(str(seconds))

    def get_time_value(self):
        """Get current time value from input"""
        try:
            return int(self.time_input.text())
        except ValueError:
            return 0

    def update_ui(self):
        """Update UI with current state from controller"""
        import time as time_module
        
        # Update broadcast value
        msg = self.controller.current_broadcast_message
        self.broadcast_value.setText(msg if msg else "0")

        current_time = time_module.time()
        
        # Clear old device rows (keep the stretch at the end)
        while self.device_layout.count() > 1:
            item = self.device_layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()

        # Create device rows
        for ip, device in self.controller.devices.items():
            if device.last_response_time and current_time - device.last_response_time > 2:
                device.status = "Disconnected"

            last_seen = (
                f"{current_time - device.last_response_time:.1f}s ago"
                if device.last_response_time
                else "Never"
            )
            
            row = self._create_device_row(
                device.device_id, 
                device.ip, 
                device.status, 
                last_seen
            )
            self.device_layout.insertWidget(self.device_layout.count() - 1, row)

        # Update stats
        total = len(self.controller.devices)
        disconnected = sum(1 for d in self.controller.devices.values() if d.status == "Disconnected")
        connected = total - disconnected

        # Update stat card values
        self.connected_label.findChild(QLabel, "value").setText(str(connected))
        self.disconnected_label.findChild(QLabel, "value").setText(str(disconnected))
        self.total_label.findChild(QLabel, "value").setText(str(total))

    def exit_action(self):
        self.controller.exit_event.set()
        self.close()

    def closeEvent(self, event):
        self.controller.exit_event.set()
        event.accept()