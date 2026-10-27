from __future__ import annotations

import threading
import time

from PySide6.QtCore import QObject, QSettings, QTimer, Signal
from PySide6.QtGui import QImage
from PySide6.QtMultimedia import QCamera, QCameraDevice, QMediaCaptureSession, QMediaDevices, QVideoSink


def device_id(device: QCameraDevice) -> str:
    return bytes(device.id()).decode("utf-8", errors="surrogateescape")


class CameraController(QObject):
    changed = Signal()
    frameChanged = Signal(QImage)

    def __init__(self, settings: QSettings):
        super().__init__()
        self.settings = settings
        self.media_devices = QMediaDevices()
        self.session = QMediaCaptureSession()
        self.sink = QVideoSink()
        self.session.setVideoSink(self.sink)
        self.sink.videoFrameChanged.connect(self._frame_changed)
        self.media_devices.videoInputsChanged.connect(self.refresh_devices)
        self.camera: QCamera | None = None
        self.devices: list[QCameraDevice] = []
        self.selected_id = ""
        self.state = "Camera paused"
        self.detail = "Show the camera from the tray menu."
        self.wants_camera = False
        self._frame_lock = threading.Lock()
        self._frame = QImage()
        self._frame_time = 0.0
        self.refresh_devices()

    def refresh_devices(self) -> None:
        self.devices = sorted(QMediaDevices.videoInputs(), key=lambda d: d.description().casefold())
        preferred = self.settings.value("cameraID", "", type=str)
        ids = [device_id(device) for device in self.devices]
        self.selected_id = next(
            (candidate for candidate in (preferred, self.selected_id, *(ids or [""])) if candidate in ids),
            "",
        )
        if self.wants_camera:
            self.start()
        else:
            self.changed.emit()

    def show(self) -> None:
        self.wants_camera = True
        self.start()

    def start(self) -> None:
        if not self.wants_camera:
            return
        device = next((item for item in self.devices if device_id(item) == self.selected_id), None)
        if device is None:
            self._set_state("No camera connected", "Connect a camera and allow desktop apps to access it in Windows Settings → Privacy & security → Camera.")
            return
        if self.camera is not None and self.camera.isActive() and device_id(self.camera.cameraDevice()) == self.selected_id:
            return
        self._dispose_camera()
        self._set_state(f"Starting {device.description()}…", "Your camera will appear here.")
        self.camera = QCamera(device)
        self.camera.errorOccurred.connect(self._error)
        self.camera.activeChanged.connect(self._active_changed)
        self.session.setCamera(self.camera)
        self.camera.start()

    def select(self, identifier: str) -> None:
        self.selected_id = identifier
        self.settings.setValue("cameraID", identifier)
        self._dispose_camera()
        if self.wants_camera:
            self.start()
        else:
            self.changed.emit()

    def retry(self) -> None:
        self.refresh_devices()
        if self.wants_camera:
            self._dispose_camera()
            self.start()

    def hide(self) -> None:
        self.wants_camera = False
        self._dispose_camera()
        self._set_state("Camera paused", "Show the camera from the tray menu.")

    def shutdown(self) -> None:
        self.wants_camera = False
        self._dispose_camera()

    def current_frame(self) -> QImage:
        with self._frame_lock:
            return self._frame.copy()

    def recording_frame(self) -> QImage:
        with self._frame_lock:
            if self._frame.isNull() or time.monotonic() - self._frame_time > 3:
                raise RuntimeError("The camera stopped sending video. Check the camera and try again.")
            return self._frame.copy()

    def _frame_changed(self, frame) -> None:
        if not frame.isValid():
            return
        image = frame.toImage()
        if image.isNull():
            return
        image = image.convertToFormat(QImage.Format.Format_RGBA8888)
        with self._frame_lock:
            self._frame = image
            self._frame_time = time.monotonic()
        self.frameChanged.emit(image)

    def _active_changed(self, active: bool) -> None:
        if active and self.camera is not None:
            self._set_state(self.camera.cameraDevice().description(), "")
        elif not active:
            with self._frame_lock:
                self._frame = QImage()

    def _error(self, _error, message: str) -> None:
        if self.wants_camera:
            with self._frame_lock:
                self._frame = QImage()
            self._set_state("Camera unavailable", message or "Choose Retry Camera from the menu.")

    def _set_state(self, title: str, detail: str) -> None:
        self.state = title
        self.detail = detail
        self.changed.emit()

    def _dispose_camera(self) -> None:
        if self.camera is not None:
            self.camera.stop()
            self.session.setCamera(None)
            self.camera.deleteLater()
            self.camera = None
        with self._frame_lock:
            self._frame = QImage()


class CameraRefreshTimer(QTimer):
    def __init__(self, camera: CameraController):
        super().__init__()
        self.setInterval(5000)
        self.timeout.connect(camera.refresh_devices)
