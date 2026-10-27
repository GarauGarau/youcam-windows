from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QObject, QPoint, QRect, QSettings, QStandardPaths, Qt, QTimer, QUrl
from PySide6.QtGui import QAction, QDesktopServices, QGuiApplication, QIcon, QImage
from PySide6.QtMultimedia import QMediaDevices
from PySide6.QtWidgets import (
    QApplication, QHBoxLayout, QLabel, QMenu, QMessageBox, QProgressBar,
    QSlider, QStyle, QSystemTrayIcon, QWidget, QWidgetAction,
)

from .audio import AudioController, audio_device_id
from .camera import CameraController, device_id
from .capture import ControlExclusions, ExclusionManager, SnapshotMenu
from .core import CameraShape, RecordingTarget, constrain, initial_frame, load_area, next_recording_path, resized, save_area
from .hotkeys import GlobalHotkeys
from .display import physical_rect, screen_target
from .overlay import CameraOverlay
from .panel import RecordingPanel
from .recorder import ScreenRecorder
from .region import RegionEditor, RegionSelector
from .sources import SourceUnavailable, WindowInfo, list_windows, window_info


class YouCam(QObject):
    def __init__(self, preview: bool = False):
        super().__init__()
        self.preview = preview
        self.quitting = False
        self.settings = QSettings("YouCam", "YouCamWindows")
        self.exclusions = ExclusionManager()
        self.camera = CameraController(self.settings)
        self.audio = AudioController(self.settings)
        self.recorder = ScreenRecorder(self.exclusions, self.camera.recording_frame)
        self.overlay = CameraOverlay()
        self.control_exclusions = ControlExclusions(self.overlay, self)
        QApplication.instance().installEventFilter(self.control_exclusions)
        self.panel = RecordingPanel(self.audio.gain_db)
        self.editor = RegionEditor()
        self.selector: RegionSelector | None = None
        self._source_change_pending = False
        self._source_dialog: QMessageBox | None = None
        self.visible = True
        try:
            self.shape = CameraShape(self.settings.value("shape", "circle", type=str))
        except ValueError:
            self.shape = CameraShape.CIRCLE
        self.overlay.set_shape(self.shape)
        self.overlay.mirrored = self.settings.value("mirrored", True, type=bool)
        self.area_mode, self.area_screen, self.area_rect = load_area(self.settings, QGuiApplication.screens())
        self.selected_window: WindowInfo | None = None
        self.menu = SnapshotMenu(self.exclusions)
        self.menu.aboutToShow.connect(lambda: self._populate(self.menu))
        self._connect()
        self._restore_geometry()
        self._build_tray()
        self.overlay.show()
        self.recorder.set_protected_rect(physical_rect(self.overlay.frameGeometry(), self._current_screen()))
        if preview:
            self.overlay.display_state("Camera paused", "Preview mode")
        else:
            self.camera.show()
            self.hotkeys = GlobalHotkeys()
            self.hotkeys.recordRequested.connect(self.toggle_recording)
            self.hotkeys.pauseRequested.connect(self.toggle_pause)
        self._refresh_editor()
        self._update_recording_ui()
        self.timer = QTimer(self)
        self.timer.setInterval(250)
        self.timer.timeout.connect(self._update_recording_ui)
        self.timer.timeout.connect(self._check_paused_window)
        self.timer.start()
        QApplication.instance().aboutToQuit.connect(self.shutdown)

    def _connect(self) -> None:
        self.overlay.moved.connect(self._overlay_moved)
        self.overlay.resizeRequested.connect(self._scroll_resize)
        self.overlay.contextMenuRequested.connect(self.show_menu)
        self.camera.changed.connect(self._camera_changed)
        self.camera.frameChanged.connect(self.overlay.display_frame)
        self.audio.changed.connect(self._update_recording_ui)
        self.audio.levelChanged.connect(self.panel.update_level)
        self.audio.failed.connect(self._audio_failure)
        self.audio.samplesReady.connect(self.recorder.write_audio)
        self.recorder.changed.connect(self._recording_changed)
        self.recorder.failed.connect(self._recording_failed)
        self.recorder.interrupted.connect(self._recording_interrupted)
        self.panel.startRequested.connect(self.toggle_recording)
        self.panel.pauseRequested.connect(self.toggle_pause)
        self.panel.stopRequested.connect(self.stop_recording)
        self.panel.hideRequested.connect(self.toggle_panel)
        self.panel.areaMenuRequested.connect(self.show_area_menu)
        self.panel.gainChanged.connect(self.audio.set_gain)
        self.panel.contextMenuRequested.connect(self.show_menu)
        self.panel.revealRequested.connect(lambda path: QDesktopServices.openUrl(QUrl.fromLocalFile(str(Path(path).parent))))
        self.editor.changed.connect(self._area_edited)
        self.editor.selectRequested.connect(self.select_area)
        self.editor.wholeScreenRequested.connect(lambda: self.request_area("whole"))
        QGuiApplication.instance().screenAdded.connect(lambda _screen: self._screens_changed())
        QGuiApplication.instance().screenRemoved.connect(lambda _screen: self._screens_changed())

    def _build_tray(self) -> None:
        icon = QIcon.fromTheme("camera-video")
        if icon.isNull():
            icon = QApplication.style().standardIcon(QStyle.StandardPixmap.SP_MediaPlay)
        self.tray = QSystemTrayIcon(icon, self)
        self.tray.setToolTip("YouCam — camera overlay")
        self.tray.setContextMenu(self.menu)
        self.tray.activated.connect(self._tray_activated)
        self.tray.show()

    def _tray_activated(self, reason) -> None:
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            self.toggle_panel()

    def _restore_geometry(self) -> None:
        screen = QGuiApplication.primaryScreen()
        available = screen.availableGeometry()
        width = self.settings.value("width", 280.0, type=float)
        if self.settings.contains("originX") and self.settings.contains("originY"):
            x = self.settings.value("originX", 0.0, type=float)
            y = self.settings.value("originY", 0.0, type=float)
            candidate = resized(QRect(round(x), round(y), 280, 280), width, self.shape, available)
            screens = QGuiApplication.screens()
            if any(screen.availableGeometry().intersects(candidate) for screen in screens):
                destination = max(screens, key=lambda item: item.availableGeometry().intersected(candidate).width() * item.availableGeometry().intersected(candidate).height())
                candidate = resized(candidate, width, self.shape, destination.availableGeometry())
                self.overlay.setGeometry(candidate)
                return
        self.overlay.setGeometry(initial_frame(width, self.shape, available))

    def _current_screen(self):
        return QGuiApplication.screenAt(self.overlay.frameGeometry().center()) or QGuiApplication.primaryScreen()

    def _save_geometry(self) -> None:
        frame = self.overlay.frameGeometry()
        self.settings.setValue("width", frame.width())
        self.settings.setValue("originX", frame.x())
        self.settings.setValue("originY", frame.y())
        self.recorder.set_protected_rect(physical_rect(frame, self._current_screen()) if self.visible else None)

    def _overlay_moved(self) -> None:
        screen = self._current_screen()
        if screen is not None:
            self.overlay.setGeometry(constrain(self.overlay.frameGeometry(), screen.availableGeometry()))
        self._save_geometry()

    def _scroll_resize(self, delta: int) -> None:
        self.resize_overlay(self.overlay.width() + delta / 4)

    def resize_overlay(self, width: float) -> None:
        screen = self._current_screen()
        if screen is None:
            return
        self.overlay.setGeometry(resized(self.overlay.frameGeometry(), width, self.shape, screen.availableGeometry()))
        self._save_geometry()

    def _camera_changed(self) -> None:
        active = self.camera.camera is not None and self.camera.camera.isActive()
        if not active or self.camera.state in ("Camera unavailable", "No camera connected"):
            self.overlay.display_frame(QImage())
        self.overlay.display_state(self.camera.state, self.camera.detail)
        self._update_recording_ui()

    def _new_menu(self, title: str = "", parent: QWidget | None = None) -> SnapshotMenu:
        return SnapshotMenu(self.exclusions, title, parent)

    def show_menu(self, position: QPoint) -> None:
        self._populate(self.menu)
        self.menu.popup(position)

    def _action(self, menu: QMenu, text: str, callback=None, enabled: bool = True, checked: bool = False) -> QAction:
        action = QAction(text, menu)
        action.setEnabled(enabled)
        if checked:
            action.setCheckable(True)
            action.setChecked(True)
        if callback is not None:
            action.triggered.connect(callback)
        menu.addAction(action)
        return action

    def _populate(self, menu: QMenu) -> None:
        menu.clear()
        self._action(menu, "YouCam", enabled=False)
        self._action(menu, self.camera.state, enabled=False)
        self._action(menu, self.recorder.status, enabled=False)
        self._action(menu, f"Source: {self.recording_area_name()}", enabled=False)
        menu.addSeparator()
        busy = self.recorder.busy
        self._action(menu, "Stop Recording" if busy else "Start Recording", self.toggle_recording,
                     self.recorder.phase != "finishing" and self.selector is None and not self._source_change_pending)
        if self.recorder.phase in ("recording", "paused"):
            title = "Resume Recording" if self.recorder.phase == "paused" else "Pause Recording"
            self._action(menu, title, self.toggle_pause, not self._source_change_pending)
        menu.addMenu(self._make_area_menu(menu)).setEnabled(self._source_actions_enabled())
        self._action(menu, "Hide Recording Panel" if self.panel.isVisible() else "Show Recording Panel", self.toggle_panel)
        if not self.preview and not self.hotkeys.available:
            self._action(menu, "Global shortcuts unavailable", enabled=False)
        if self.recorder.last_path is not None:
            self._action(menu, "Show Last Recording in File Manager", self._reveal_recording)
        menu.addSeparator()
        camera_title = "Hide Camera Preview" if self.visible else "Show Camera Preview"
        if self.area_mode != "camera":
            camera_title = "Hide Camera" if self.visible else "Show Camera"
        self._action(menu, camera_title, self.toggle_visibility)
        cameras = self._new_menu("Camera", menu)
        if not self.camera.devices:
            self._action(cameras, "No cameras available", enabled=False)
        for device in self.camera.devices:
            identifier = device_id(device)
            self._action(cameras, device.description(), lambda _=False, value=identifier: self.camera.select(value),
                         enabled=not (busy and self.area_mode == "camera"), checked=identifier == self.camera.selected_id)
        menu.addMenu(cameras)
        menu.addMenu(self._make_audio_menu(menu))
        shapes = self._new_menu("Shape", menu)
        for shape in CameraShape:
            self._action(shapes, shape.title, lambda _=False, value=shape: self._select_shape(value), checked=shape is self.shape)
        menu.addMenu(shapes)
        sizes = self._new_menu("Size", menu)
        for title, width in (("Small", 180), ("Medium", 280), ("Large", 400), ("Extra Large", 560)):
            self._action(sizes, title, lambda _=False, value=width: self.resize_overlay(value), checked=abs(self.overlay.width() - width) < 2)
        sizes.addSeparator()
        self._action(sizes, "Increase Size", lambda: self.resize_overlay(self.overlay.width() * 1.15))
        self._action(sizes, "Decrease Size", lambda: self.resize_overlay(self.overlay.width() / 1.15))
        menu.addMenu(sizes)
        self._action(menu, "Mirror Camera", self._toggle_mirror,
                     enabled=not (busy and self.area_mode == "camera"), checked=self.overlay.mirrored)
        self._action(menu, "Move to Bottom Right", self._reset_position)
        menu.addSeparator()
        self._action(menu, "Retry Camera", self.camera.retry, enabled=not (busy and self.area_mode == "camera"))
        self._action(menu, "How to Use YouCam", self._show_help)
        menu.addSeparator()
        self._action(menu, "Quit YouCam", QApplication.quit)

    def _make_audio_menu(self, parent: QWidget) -> QMenu:
        menu = self._new_menu("Audio Input", parent)
        self._action(menu, "No Audio", lambda: self.audio.select("none"), not self.recorder.busy, not self.audio.has_audio)
        for device in self.audio.devices:
            identifier = audio_device_id(device)
            self._action(menu, device.description(), lambda _=False, value=identifier: self.audio.select(value), not self.recorder.busy, identifier == self.audio.selected_id)
        menu.addSeparator()
        container = QWidget()
        row = QHBoxLayout(container)
        row.setContentsMargins(6, 2, 6, 2)
        row.addWidget(QLabel("Gain"))
        slider = QSlider(Qt.Orientation.Horizontal)
        slider.setRange(-36, 12)
        slider.setValue(round(self.audio.gain_db))
        slider.valueChanged.connect(self.audio.set_gain)
        row.addWidget(slider)
        meter = QProgressBar()
        meter.setRange(0, 60)
        meter.setFixedWidth(80)
        self.audio.levelChanged.connect(lambda peak, _clipped, widget=meter: widget.setValue(round(max(0.0, peak) * 60)))
        action = QWidgetAction(menu)
        action.setDefaultWidget(container)
        menu.addAction(action)
        menu.aboutToShow.connect(lambda: self.audio.monitor("menu", True))
        menu.aboutToHide.connect(lambda: self.audio.release("menu"))
        return menu

    def _make_area_menu(self, parent: QWidget | None = None) -> QMenu:
        menu = self._new_menu("Recording Source", parent)
        enabled = self._source_actions_enabled()
        self._action(menu, "Whole Screen", lambda: self.request_area("whole"), enabled, self.area_mode == "whole")
        self._action(menu, "Select Area…", self.select_area, enabled, self.area_mode == "region")
        screens = QGuiApplication.screens()
        if len(screens) > 1:
            menu.addSeparator()
            for screen in screens:
                name = screen.name()
                self._action(menu, f"Whole Screen — {name}", lambda _=False, value=name: self.request_area("display", value), enabled, self.area_mode == "display" and self.area_screen == name)
        menu.addSeparator()
        windows = self._new_menu("Window", menu)
        self._populate_window_menu(windows)
        windows.aboutToShow.connect(lambda: self._populate_window_menu(windows))
        menu.addMenu(windows).setEnabled(enabled)
        camera = self._action(menu, "Camera Only (Full Frame)", lambda: self.request_area("camera"), enabled, self.area_mode == "camera")
        camera.setToolTip("Record the complete camera image, without the desktop or the overlay shape")
        return menu

    def _populate_window_menu(self, menu: QMenu) -> None:
        menu.clear()
        try:
            windows = list_windows()
        except Exception:
            self._action(menu, "Window list unavailable (Windows required)", enabled=False)
            return
        if not windows:
            self._action(menu, "No windows available — open or restore a window", enabled=False)
        enabled = self._source_actions_enabled()
        for window in windows:
            selected = self.area_mode == "window" and self.selected_window is not None and self.selected_window.id == window.id
            title = f"{window.title.replace('&', '&&')} ({window.rect.width()} × {window.rect.height()})"
            self._action(menu, title, lambda _=False, value=window: self.select_window(value), enabled, selected)

    def select_window(self, window: WindowInfo) -> None:
        if not self._source_actions_enabled():
            return
        if self.area_mode == "window" and self.selected_window is not None and self.selected_window.id == window.id:
            return
        try:
            window_info(window.id)
            if not self._confirm_source_change(f"Window — {window.title}"):
                return
            # Recheck after the dialog: the window may have closed meanwhile.
            self.selected_window = window_info(window.id)
        except Exception as exc:
            self._show_error(str(exc))
            return
        self.set_area("window")

    def _source_actions_enabled(self) -> bool:
        return (self.recorder.phase in ("idle", "recording", "paused")
                and self.selector is None and not self._source_change_pending and not self.quitting)

    def request_area(self, mode: str, screen_name: str = "") -> None:
        if not self._source_actions_enabled():
            return
        if self.area_mode == mode and (mode != "display" or self.area_screen == screen_name):
            return
        title = "Camera Only (Full Frame)" if mode == "camera" else f"Whole Screen — {screen_name}" if screen_name else "Whole Screen"
        if self._confirm_source_change(title):
            self.set_area(mode, screen_name)

    def _confirm_source_change(self, destination: str) -> bool:
        if not self._source_actions_enabled():
            return False
        if not self.recorder.busy:
            return True
        was_recording = self.recorder.phase == "recording"
        self._source_change_pending = True
        try:
            # No confirmation dialog or source picker should enter the movie.
            if was_recording:
                self.recorder.pause()
            if self.recorder.phase != "paused":
                return False
            self._update_recording_ui()
            accepted = self._ask_source_change(destination, was_recording)
            if self.quitting:
                return False
            if not accepted:
                if was_recording and self.recorder.phase == "paused":
                    self.recorder.resume()
                return False
            if self.recorder.phase != "paused":
                return False
            self.recorder.stop()
            if self.recorder.phase != "idle" or self.recorder.last_path is None:
                return False
            path = str(self.recorder.last_path)
            self._show_notice("Recording saved", f"Saved to {path}\nPress Record to start a new video with the new source.", path, warning=False)
            return True
        finally:
            self._source_change_pending = False
            self._update_recording_ui()

    def _ask_source_change(self, destination: str, was_recording: bool) -> bool:
        dialog = QMessageBox(self.panel if self.panel.isVisible() else None)
        dialog.setWindowTitle("Change recording source?")
        dialog.setIcon(QMessageBox.Icon.Question)
        dialog.setTextFormat(Qt.TextFormat.PlainText)
        dialog.setText(f"Save the current recording and switch to {destination}?")
        dialog.setInformativeText(
            "Recording is paused while you choose. Your current video will be saved; "
            "press Record to start a separate video with the new source."
        )
        accept = dialog.addButton("Save and Switch", QMessageBox.ButtonRole.AcceptRole)
        cancel = dialog.addButton("Keep Recording" if was_recording else "Keep Paused", QMessageBox.ButtonRole.RejectRole)
        dialog.setDefaultButton(cancel)
        dialog.setEscapeButton(cancel)
        self._source_dialog = dialog
        try:
            dialog.exec()
            return dialog.clickedButton() is accept
        finally:
            self._source_dialog = None
            dialog.deleteLater()

    def show_area_menu(self, position: QPoint) -> None:
        menu = self._make_area_menu()
        menu.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        menu.popup(position)

    def _select_shape(self, shape: CameraShape) -> None:
        self.shape = shape
        self.settings.setValue("shape", shape.value)
        self.overlay.set_shape(shape)
        self.resize_overlay(self.overlay.width())

    def _toggle_mirror(self) -> None:
        self.overlay.mirrored = not self.overlay.mirrored
        self.settings.setValue("mirrored", self.overlay.mirrored)
        self.overlay.update()

    def _reset_position(self) -> None:
        screen = self._current_screen()
        if screen is not None:
            self.overlay.setGeometry(initial_frame(self.overlay.width(), self.shape, screen.availableGeometry()))
            self._save_geometry()

    def toggle_visibility(self) -> None:
        self.visible = not self.visible
        if self.visible:
            self.overlay.show()
            if not self.preview:
                self.camera.show()
        else:
            self.overlay.hide()
            if self.area_mode != "camera":
                self.camera.hide()
        self.recorder.set_protected_rect(physical_rect(self.overlay.frameGeometry(), self._current_screen()) if self.visible else None)
        self._update_recording_ui()

    def toggle_panel(self) -> None:
        if self.panel.isVisible():
            self.panel.hide()
            self.exclusions.remove(self.panel)
            self.audio.release("panel")
            return
        self.show_panel()

    def show_panel(self, monitor_audio: bool = True) -> None:
        screen = self._current_screen()
        self.panel.adjustSize()
        if screen is not None:
            available = screen.availableGeometry()
            self.panel.move(available.center().x() - self.panel.width() // 2, available.y() + available.height() - self.panel.height() - 40)
        self.exclusions.add(self.panel)
        self.panel.show()
        if monitor_audio:
            self.audio.acquire("panel")
        self._update_recording_ui()

    def selected_target(self) -> RecordingTarget | None:
        if self.area_mode == "camera":
            image = self.camera.current_frame()
            return (RecordingTarget(QRect(QPoint(), image.size()), self.camera.state, kind="camera", mirrored=self.overlay.mirrored)
                    if not image.isNull() else None)
        if self.area_mode == "window":
            if self.selected_window is None:
                return None
            try:
                window = window_info(self.selected_window.id)
            except Exception:
                return None
            return RecordingTarget(window.rect, window.title, kind="window", window_id=window.id)
        if self.area_mode == "whole":
            screen = self._current_screen()
            return screen_target(QRect(screen.geometry()), screen) if screen is not None else None
        screen = next((item for item in QGuiApplication.screens() if item.name() == self.area_screen), None)
        if screen is None:
            return None
        if self.area_mode == "display":
            return screen_target(QRect(screen.geometry()), screen)
        if self.area_mode == "region" and screen.geometry().contains(self.area_rect) and self.area_rect.width() >= 40 and self.area_rect.height() >= 40:
            return screen_target(QRect(self.area_rect), screen)
        return None

    def recording_area_name(self) -> str:
        if self.area_mode == "camera":
            return "Camera Only (Full Frame)"
        if self.area_mode == "window":
            return f"Window — {self.selected_window.title}" if self.selected_window else "No window selected"
        if self.area_mode == "whole":
            return "Whole Screen"
        if self.area_mode == "display":
            return self.area_screen or "Disconnected Display"
        return f"Selected Area ({self.area_rect.width()} × {self.area_rect.height()})"

    def set_area(self, mode: str, screen_name: str = "", rect: QRect | None = None) -> None:
        if self.recorder.busy or self.selector is not None:
            return
        previous_mode = self.area_mode
        self.area_mode = mode
        if screen_name:
            self.area_screen = screen_name
        elif mode in ("region", "display"):
            screen = self._current_screen()
            self.area_screen = screen.name() if screen is not None else ""
        if rect is not None:
            self.area_rect = QRect(rect)
        save_area(self.settings, mode, self.area_screen, self.area_rect)
        if not self.preview:
            if mode == "camera":
                self.camera.show()
            elif previous_mode == "camera" and not self.visible:
                self.camera.hide()
        self._refresh_editor()
        self._update_recording_ui()

    def select_area(self) -> None:
        if not self._confirm_source_change("a new screen area"):
            return
        self.editor.hide_editor()
        self.selector = RegionSelector()
        self.selector.selected.connect(self._area_selected)
        self.selector.cancelled.connect(self._area_cancelled)
        self.selector.begin()
        self._update_recording_ui()

    def _area_selected(self, rect: QRect) -> None:
        screen = QGuiApplication.screenAt(rect.center()) or QGuiApplication.primaryScreen()
        self.selector = None
        self.set_area("region", screen.name(), rect)

    def _area_cancelled(self) -> None:
        self.selector = None
        self._refresh_editor()
        self._update_recording_ui()

    def _area_edited(self, rect: QRect) -> None:
        if self.recorder.busy or self.selector is not None:
            return
        self.area_rect = QRect(rect)
        self.settings.setValue("areaRect", rect)
        self._update_recording_ui()

    def _refresh_editor(self) -> None:
        target = self.selected_target() if self.area_mode == "region" else None
        if self.area_mode == "region" and target is not None and not self.recorder.busy and self.selector is None and not self.quitting:
            self.editor.show_editor(target.rect)
        else:
            self.editor.hide_editor()

    def _screens_changed(self) -> None:
        self._overlay_moved()
        if self.area_mode in ("whole", "display", "region") and self.selected_target() is None:
            if self.recorder.busy:
                self.recorder.stop()
            self.set_area("whole")
        self._refresh_editor()

    def toggle_recording(self) -> None:
        if self.preview or self._source_change_pending:
            return
        if self.recorder.busy:
            self.stop_recording()
            return
        if self.selector is not None:
            return
        target = self.selected_target()
        if target is None:
            if self.area_mode == "camera":
                self._show_error("The camera is still starting or unavailable. Wait for its preview, or select another Camera, then try again.")
            elif self.area_mode == "window":
                self._show_error("The selected window is closed or minimized. Restore it or choose another window in Recording Source → Window.")
            else:
                self._show_error("The selected display or recording area is no longer available.")
            return
        self.audio.acquire("recording")
        if self.audio.has_audio and self.audio.source is None:
            self.audio.release("recording")
            self._show_error("The selected microphone could not be opened. Choose another input or No Audio.")
            return
        desktop = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.DesktopLocation)
        directory = Path(desktop) if desktop else Path.home() / "Desktop"
        self.recorder.start(target, next_recording_path(directory), self.audio.has_audio)

    def toggle_pause(self) -> None:
        if not self._source_change_pending:
            self.recorder.toggle_pause()

    def stop_recording(self) -> None:
        if not self._source_change_pending:
            was_interrupted = bool(self.recorder.interruption_reason)
            self.recorder.stop()
            if was_interrupted and self.recorder.last_path is not None:
                path = str(self.recorder.last_path)
                self._show_notice("Recording saved", f"Your recording was saved to {path}", path, warning=False)

    def _check_paused_window(self) -> None:
        target = self.recorder.target
        if (self.quitting or self._source_change_pending or self.recorder.phase != "paused"
                or target is None or target.kind != "window" or target.window_id is None):
            return
        try:
            window_info(target.window_id)
        except SourceUnavailable:
            return
        except Exception:
            self.recorder.stop()
            if self.recorder.last_path is not None:
                self._recording_failed("The selected window was closed or became unavailable.")

    def _recording_changed(self) -> None:
        if self.recorder.phase == "recording":
            self.panel.clear_notice()
        if self.recorder.busy:
            self.editor.hide_editor()
        else:
            self.audio.release("recording")
            self._refresh_editor()
        self._update_recording_ui()

    def _update_recording_ui(self) -> None:
        self.panel.update_state(self.recorder.phase, self.recorder.status, self.recording_area_name(),
                                self._source_change_pending or self.selector is not None)
        self.tray.setToolTip(f"YouCam — {self.recorder.status if self.recorder.busy else self.camera.state}")

    def _reveal_recording(self) -> None:
        if self.recorder.last_path is not None:
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(self.recorder.last_path.parent)))

    def _audio_failure(self, message: str) -> None:
        if self.recorder.busy:
            self.recorder.stop()
            self._recording_failed(message)
        else:
            self._show_error(message)

    def _recording_interrupted(self, message: str) -> None:
        self._show_notice(
            "Recording paused — source unavailable",
            f"{message}\nRestore the window, then click Resume to continue the same video. "
            "Click Stop to save it, or choose another Recording Source. No video or audio is recorded while paused.",
        )

    def _recording_failed(self, message: str) -> None:
        path = str(self.recorder.last_path) if self.recorder.last_path is not None else ""
        result = f"Your recording was saved to {path}" if path else "The recording could not be saved."
        self._show_notice("Recording stopped", f"{message}\n{result}", path)

    def _show_notice(self, title: str, message: str, path: str = "", warning: bool = True) -> None:
        if self.quitting:
            return
        self.panel.show_notice(title, message, path)
        self.show_panel(monitor_audio=False)
        self.panel.raise_()
        icon = QSystemTrayIcon.MessageIcon.Warning if warning else QSystemTrayIcon.MessageIcon.Information
        self.tray.showMessage(title, message, icon, 8000)

    def _show_error(self, message: str) -> None:
        if self.quitting or self.preview:
            return
        QMessageBox.critical(self.panel if self.panel.isVisible() else None, "YouCam", message)

    def _show_help(self) -> None:
        QMessageBox.information(
            None, "Your camera, without the window.",
            "Drag the camera to move it. Scroll to resize. Right-click the camera or recording panel for controls.\n\n"
            "Choose Recording Source → Select Area and drag a rectangle. Use the white corner handles to resize it and Move to reposition it before recording. "
            "The buttons beside the selection let you select a new area or return to Whole Screen. Esc cancels a new selection and keeps the previous area. Your choice is remembered.\n\n"
            "Recording Source → Window records only the chosen window, even when moved or covered by another window. Minimizing it pauses recording and shows an alert; restore it and click Resume to continue the same video. Closing it saves and stops, with an alert showing the saved file. "
            "Camera Only (Full Frame) records the complete camera image without the desktop. Hide Camera Preview does not interrupt it. Changing sources during recording asks you to save and switch, or keep recording.\n\n"
            "Movies are saved automatically to your Desktop. Ctrl+Alt+Shift+R starts/stops; Ctrl+Alt+Shift+P pauses/resumes.\n\n"
            "Audio Input selects the microphone and provides gain and a live meter. System/app audio is not recorded. Hide Camera records only the screen.\n\n"
            "Windows 11: AMD AMF is used when a test encode succeeds, with automatic x264 fallback. Some protected or accelerated applications cannot be captured as individual windows; use screen or area recording instead.",
        )

    def shutdown(self) -> None:
        if self.quitting:
            return
        self.quitting = True
        QApplication.instance().removeEventFilter(self.control_exclusions)
        if self._source_dialog is not None:
            self._source_dialog.reject()
        self._save_geometry()
        self.editor.hide_editor()
        if self.recorder.busy:
            self.recorder.stop()
        self.audio.shutdown()
        self.camera.shutdown()
        if not self.preview and hasattr(self, "hotkeys"):
            self.hotkeys.shutdown()
