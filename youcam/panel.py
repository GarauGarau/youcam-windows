from __future__ import annotations

import math

from PySide6.QtCore import QPoint, Qt, Signal
from PySide6.QtGui import QCloseEvent, QMouseEvent
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QProgressBar, QPushButton, QSlider, QVBoxLayout, QWidget


class RecordingPanel(QWidget):
    startRequested = Signal()
    pauseRequested = Signal()
    stopRequested = Signal()
    areaMenuRequested = Signal(QPoint)
    hideRequested = Signal()
    gainChanged = Signal(float)
    contextMenuRequested = Signal(QPoint)
    revealRequested = Signal(str)

    def __init__(self, gain_db: float):
        super().__init__()
        self.setWindowFlags(Qt.WindowType.Tool | Qt.WindowType.WindowStaysOnTopHint)
        self.setWindowTitle("YouCam Recording")
        self.setMinimumWidth(480)
        layout = QVBoxLayout(self)
        self.status_label = QLabel("Ready to record")
        self.status_label.setStyleSheet("font-weight: 600")
        layout.addWidget(self.status_label)
        self.area_label = QLabel("Whole Screen")
        layout.addWidget(self.area_label)
        self.source_hint = QLabel()
        self.source_hint.setWordWrap(True)
        layout.addWidget(self.source_hint)
        self.notice = QFrame()
        self.notice.setFrameShape(QFrame.Shape.StyledPanel)
        notice_layout = QVBoxLayout(self.notice)
        self.notice_title = QLabel()
        self.notice_title.setTextFormat(Qt.TextFormat.PlainText)
        self.notice_title.setStyleSheet("font-weight: 600")
        self.notice_text = QLabel()
        self.notice_text.setTextFormat(Qt.TextFormat.PlainText)
        self.notice_text.setWordWrap(True)
        self.notice_path = ""
        notice_layout.addWidget(self.notice_title)
        notice_layout.addWidget(self.notice_text)
        notice_actions = QHBoxLayout()
        self.reveal_button = QPushButton("Show Saved Video")
        self.dismiss_button = QPushButton("Dismiss")
        notice_actions.addWidget(self.reveal_button)
        notice_actions.addWidget(self.dismiss_button)
        notice_layout.addLayout(notice_actions)
        self.reveal_button.clicked.connect(lambda: self.revealRequested.emit(self.notice_path))
        self.dismiss_button.clicked.connect(self.clear_notice)
        layout.addWidget(self.notice)
        self.notice.hide()
        buttons = QHBoxLayout()
        self.start_button = QPushButton("Record")
        self.pause_button = QPushButton("Pause")
        self.stop_button = QPushButton("Stop")
        self.area_button = QPushButton("Recording Source")
        self.hide_button = QPushButton("Hide")
        for button in (self.start_button, self.pause_button, self.stop_button, self.area_button, self.hide_button):
            buttons.addWidget(button)
        layout.addLayout(buttons)
        gain_row = QHBoxLayout()
        gain_row.addWidget(QLabel("Microphone gain"))
        self.gain_slider = QSlider(Qt.Orientation.Horizontal)
        self.gain_slider.setRange(-36, 12)
        self.gain_slider.setValue(round(gain_db))
        self.gain_label = QLabel(f"{gain_db:+.0f} dB")
        gain_row.addWidget(self.gain_slider, 1)
        gain_row.addWidget(self.gain_label)
        layout.addLayout(gain_row)
        self.meter = QProgressBar()
        self.meter.setRange(0, 60)
        self.meter.setValue(0)
        self.meter.setFormat("Microphone level")
        layout.addWidget(self.meter)
        self.start_button.clicked.connect(self._start)
        self.pause_button.clicked.connect(self.pauseRequested)
        self.stop_button.clicked.connect(self.stopRequested)
        self.hide_button.clicked.connect(self.hideRequested)
        self.area_button.clicked.connect(lambda: self.areaMenuRequested.emit(self.area_button.mapToGlobal(QPoint(0, self.area_button.height()))))
        self.gain_slider.valueChanged.connect(self._gain)
        self.update_state("idle", "Ready to record", "Whole Screen")

    def _start(self) -> None:
        if self.start_button.text() == "Resume":
            self.pauseRequested.emit()
        else:
            self.startRequested.emit()

    def _gain(self, value: int) -> None:
        self.gain_label.setText(f"{value:+d} dB")
        self.gainChanged.emit(float(value))

    def update_level(self, peak: float, clipped: bool) -> None:
        decibels = max(-60.0, 20 * math.log10(peak)) if peak > 0 else -60.0
        self.meter.setValue(round(decibels + 60))
        color = "#e53935" if clipped else "#35a853"
        self.meter.setStyleSheet(f"QProgressBar::chunk {{ background-color: {color}; }}")

    def show_notice(self, title: str, text: str, path: str = "") -> None:
        self.notice_title.setText(title)
        self.notice_text.setText(text)
        self.notice_path = path
        self.reveal_button.setVisible(bool(path))
        self.notice.show()

    def clear_notice(self) -> None:
        self.notice.hide()
        self.notice_path = ""

    def update_state(self, phase: str, status: str, area: str, interaction_pending: bool = False) -> None:
        self.status_label.setText(status)
        self.area_label.setText(f"Source: {area}")
        self.source_hint.setText("Minimizing this window pauses recording; closing it saves and stops." if area.startswith("Window —") else "")
        self.source_hint.setVisible(bool(self.source_hint.text()))
        self.start_button.setEnabled(phase in ("idle", "paused") and not interaction_pending)
        self.start_button.setText("Resume" if phase == "paused" else "Record")
        self.pause_button.setEnabled(phase == "recording" and not interaction_pending)
        self.stop_button.setEnabled(phase in ("recording", "paused") and not interaction_pending)
        self.area_button.setEnabled(phase in ("idle", "recording", "paused") and not interaction_pending)
        self.area_button.setToolTip("Choose a source. During recording, you will be asked to save before switching.")

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.RightButton:
            self.contextMenuRequested.emit(event.globalPosition().toPoint())
            event.accept()
        else:
            super().mousePressEvent(event)

    def closeEvent(self, event: QCloseEvent) -> None:
        event.ignore()
        self.hideRequested.emit()
