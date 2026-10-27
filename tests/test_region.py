from __future__ import annotations

from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from PySide6.QtCore import QCoreApplication, QEvent, QPoint, QPointF, QRect, QSettings, Qt, QTimer
from PySide6.QtGui import QColor, QImage, QMouseEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from youcam.app import YouCam
from youcam.core import RecordingTarget, load_area
from youcam.sources import WindowInfo


class RegionInteractionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setQuitOnLastWindowClosed(False)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.settings = QSettings(str(Path(self.directory.name) / "settings.ini"), QSettings.Format.IniFormat)
        self.settings.setValue("audioID", "none")
        with patch("youcam.app.QSettings", return_value=self.settings):
            self.youcam = YouCam(preview=True)
        self.screen = self.app.primaryScreen()
        self.bounds = self.screen.geometry()
        self.rect = QRect(self.bounds.x() + 80, self.bounds.y() + 100, 240, 180)
        self.youcam.set_area("region", self.screen.name(), self.rect)
        self.app.processEvents()

    def tearDown(self):
        if self.youcam.selector is not None:
            QTest.keyClick(self.youcam.selector, Qt.Key.Key_Escape)
        self.youcam.timer.stop()
        self.youcam.shutdown()
        self.youcam.tray.hide()
        for widget in self.app.topLevelWidgets():
            widget.hide()
            widget.deleteLater()
        self.youcam.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        self.directory.cleanup()

    def drag_handle(self, index: int, delta: QPoint) -> None:
        handle = self.youcam.editor.handles[index]
        grab = QPoint(handle.width() // 2 + 3, handle.height() // 2 + 2)
        end = handle.mapToGlobal(grab) + delta
        QTest.mousePress(handle, Qt.MouseButton.LeftButton, pos=grab)
        move = QMouseEvent(
            QEvent.Type.MouseMove, QPointF(handle.mapFromGlobal(end)), QPointF(end),
            Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
        )
        QApplication.sendEvent(handle, move)
        release = QMouseEvent(
            QEvent.Type.MouseButtonRelease, QPointF(handle.mapFromGlobal(end)), QPointF(end),
            Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
        )
        QApplication.sendEvent(handle, release)
        self.app.processEvents()

    def test_whole_screen_button_removes_region_and_saves_choice(self):
        QTest.mouseClick(self.youcam.editor.controls.whole_screen_button, Qt.MouseButton.LeftButton)
        self.assertEqual(self.youcam.area_mode, "whole")
        self.assertEqual(self.youcam.selected_target().rect, self.bounds)
        self.assertEqual(load_area(self.settings, [self.screen])[0], "whole")
        self.assertFalse(self.youcam.editor.overlay.isVisible())
        self.assertFalse(self.youcam.editor.controls.isVisible())
        self.assertTrue(all(not handle.isVisible() for handle in self.youcam.editor.handles))

    def test_cancel_reselection_restores_previous_area_and_controls(self):
        QTest.mouseClick(self.youcam.editor.controls.select_button, Qt.MouseButton.LeftButton)
        self.assertIsNotNone(self.youcam.selector)
        self.assertFalse(self.youcam.editor.controls.isVisible())
        QTest.keyClick(self.youcam.selector, Qt.Key.Key_Escape)
        self.assertIsNone(self.youcam.selector)
        self.assertEqual(self.youcam.selected_target().rect, self.rect)
        self.assertEqual(load_area(self.settings, [self.screen]), ("region", self.screen.name(), self.rect))
        self.assertTrue(self.youcam.editor.controls.isVisible())
        self.assertTrue(all(handle.isVisible() for handle in self.youcam.editor.handles))

    def test_reselection_replaces_area(self):
        QTest.mouseClick(self.youcam.editor.controls.select_button, Qt.MouseButton.LeftButton)
        selector = self.youcam.selector
        start = self.bounds.topLeft() + QPoint(150, 180)
        end = start + QPoint(310, 230)
        QTest.mousePress(selector, Qt.MouseButton.LeftButton, pos=selector.mapFromGlobal(start))
        QTest.mouseRelease(selector, Qt.MouseButton.LeftButton, pos=selector.mapFromGlobal(end))
        expected = QRect(start, end)
        self.assertIsNone(self.youcam.selector)
        self.assertEqual(self.youcam.selected_target().rect, expected)
        self.assertEqual(load_area(self.settings, [self.screen])[2], expected)
        self.assertTrue(self.youcam.editor.controls.isVisible())

    def test_corner_drag_preserves_grab_offset_and_saves_area(self):
        self.drag_handle(2, QPoint(60, 45))
        expected = QRect(self.rect.x(), self.rect.y(), self.rect.width() + 60, self.rect.height() + 45)
        self.assertEqual(self.youcam.selected_target().rect, expected)
        self.assertEqual(load_area(self.settings, [self.screen])[2], expected)

    def test_corner_click_does_not_resize(self):
        self.drag_handle(2, QPoint())
        self.assertEqual(self.youcam.selected_target().rect, self.rect)

    def test_resize_stays_on_display_and_retains_minimum_size(self):
        for index, delta in enumerate((QPoint(-5000, -5000), QPoint(5000, -5000), QPoint(5000, 5000), QPoint(-5000, 5000))):
            with self.subTest(corner=index):
                self.youcam.set_area("region", self.screen.name(), self.rect)
                self.drag_handle(index, delta)
                target = self.youcam.selected_target()
                self.assertIsNotNone(target)
                self.assertTrue(self.bounds.contains(target.rect))
                self.youcam.set_area("region", self.screen.name(), self.rect)
                self.drag_handle(index, -delta)
                self.assertEqual(self.youcam.selected_target().rect.size().width(), 40)
                self.assertEqual(self.youcam.selected_target().rect.size().height(), 40)

    def test_move_clamps_to_display_edge(self):
        self.drag_handle(4, QPoint(5000, 5000))
        target = self.youcam.selected_target()
        self.assertIsNotNone(target)
        self.assertEqual(target.rect.size(), self.rect.size())
        self.assertEqual(target.rect.bottomRight(), self.bounds.bottomRight())
        self.assertTrue(self.screen.availableGeometry().contains(self.youcam.editor.controls.geometry()))
        self.assertTrue(all(self.bounds.contains(handle.geometry()) for handle in self.youcam.editor.handles))

    def test_recording_hides_edit_controls_until_stopped(self):
        for phase in ("recording", "paused", "finishing"):
            self.youcam.recorder.phase = phase
            self.youcam._recording_changed()
            self.assertFalse(self.youcam.editor.controls.isVisible())
            self.assertFalse(self.youcam.editor.overlay.isVisible())
            self.youcam.set_area("whole")
            self.youcam._area_edited(QRect(0, 0, 100, 100))
            self.assertEqual(self.youcam.area_mode, "region")
            self.assertEqual(self.youcam.area_rect, self.rect)
        self.youcam.recorder.phase = "idle"
        self.youcam._recording_changed()
        self.assertTrue(self.youcam.editor.controls.isVisible())

    def test_area_menu_allows_return_to_whole_screen(self):
        self.youcam.show_area_menu(self.bounds.topLeft() + QPoint(40, 40))
        self.app.processEvents()
        menu = self.app.activePopupWidget()
        self.assertIsNotNone(menu)
        action = next(action for action in menu.actions() if action.text() == "Whole Screen")
        QTest.mouseClick(menu, Qt.MouseButton.LeftButton, pos=menu.actionGeometry(action).center())
        self.assertEqual(self.youcam.area_mode, "whole")
        self.assertFalse(self.youcam.editor.controls.isVisible())

    def test_camera_source_uses_full_frame_and_keeps_camera_running_with_hidden_preview(self):
        image = QImage(640, 360, QImage.Format.Format_RGB32)
        image.fill(QColor("red"))
        with patch.object(self.youcam.camera, "current_frame", return_value=image):
            menu = self.youcam._make_area_menu()
            action = next(action for action in menu.actions() if action.text() == "Camera Only (Full Frame)")
            action.trigger()
            target = self.youcam.selected_target()
            self.assertEqual(target.kind, "camera")
            self.assertEqual(target.pixel_size, (640, 360))
            self.assertTrue(target.mirrored)
            self.assertFalse(self.youcam.editor.controls.isVisible())
            with patch.object(self.youcam.camera, "hide") as hide:
                self.youcam.toggle_visibility()
                self.assertFalse(self.youcam.overlay.isVisible())
                hide.assert_not_called()
                self.assertEqual(self.youcam.selected_target().kind, "camera")

    def test_switching_source_starts_camera_and_releases_it_when_preview_is_hidden(self):
        self.youcam.preview = False
        with patch.object(self.youcam.camera, "show") as show, patch.object(self.youcam.camera, "hide") as hide:
            self.youcam.set_area("camera")
            show.assert_called_once()
            self.youcam.toggle_visibility()
            hide.assert_not_called()
            self.youcam.set_area("whole")
            hide.assert_called_once()
        self.youcam.preview = True

    def test_window_menu_selects_window_and_missing_window_does_not_fall_back_to_desktop(self):
        window = WindowInfo(123, "Test window", QRect(50, 60, 400, 300))
        with patch("youcam.app.list_windows", return_value=[window]), patch("youcam.app.window_info", return_value=window):
            menu = self.youcam._make_area_menu()
            windows = next(action.menu() for action in menu.actions() if action.text() == "Window")
            windows.actions()[0].trigger()
            target = self.youcam.selected_target()
            self.assertEqual(target.kind, "window")
            self.assertEqual(target.window_id, 123)
            self.assertEqual(target.pixel_size, (400, 300))
            self.assertFalse(self.youcam.editor.controls.isVisible())
        with patch("youcam.app.window_info", side_effect=RuntimeError("Window closed")):
            self.assertIsNone(self.youcam.selected_target())
            self.assertEqual(self.youcam.area_mode, "window")

    def test_cancel_area_selection_keeps_camera_source(self):
        self.youcam.set_area("camera")
        self.youcam.select_area()
        QTest.keyClick(self.youcam.selector, Qt.Key.Key_Escape)
        self.assertEqual(self.youcam.area_mode, "camera")
        self.assertFalse(self.youcam.editor.controls.isVisible())

    def start_movie(self) -> Path:
        image = QImage(160, 120, QImage.Format.Format_RGB32)
        image.fill(QColor("blue"))
        self.youcam.set_area("camera")
        self.youcam.recorder.camera_frame = lambda: image.copy()
        path = Path(self.directory.name) / "source-change.mov"
        self.youcam.recorder.start(RecordingTarget(QRect(0, 0, 160, 120), "Camera", kind="camera"), path, False)
        deadline = time.monotonic() + 0.25
        while time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(0.01)
        self.assertEqual(self.youcam.recorder.phase, "recording")
        return path

    def answer_source_dialog(self, accept: bool, observations: list) -> None:
        def answer():
            dialog = self.youcam._source_dialog
            observations.append((self.youcam.recorder.phase, self.youcam.area_mode,
                                 dialog.defaultButton().text(), self.youcam.panel.area_button.isEnabled()))
            button = next(button for button in dialog.buttons()
                          if (button.text() == "Save and Switch") == accept)
            QTest.mouseClick(button, Qt.MouseButton.LeftButton)
        QTimer.singleShot(0, answer)

    def test_change_source_can_be_cancelled_and_resumes_same_recording(self):
        path = self.start_movie()
        observations = []
        self.answer_source_dialog(False, observations)
        self.youcam.request_area("whole")
        self.assertEqual(observations, [("paused", "camera", "Keep Recording", False)])
        self.assertEqual(self.youcam.area_mode, "camera")
        self.assertEqual(self.youcam.recorder.phase, "recording")
        self.assertEqual(self.youcam.recorder.output_path, path)
        self.assertIsNone(self.youcam.recorder.last_path)
        self.assertFalse(self.youcam._source_change_pending)
        self.youcam.recorder.stop()
        self.assertTrue(path.exists())

    def test_confirmed_change_saves_before_switching_and_does_not_start_another_movie(self):
        path = self.start_movie()
        observations = []
        self.answer_source_dialog(True, observations)
        self.youcam.request_area("whole")
        self.assertEqual(self.youcam.area_mode, "whole")
        self.assertEqual(self.youcam.recorder.phase, "idle")
        self.assertEqual(self.youcam.recorder.last_path, path)
        self.assertTrue(path.exists())
        self.assertTrue(self.youcam.panel.notice.isVisible())
        self.assertEqual(self.youcam.panel.notice_title.text(), "Recording saved")
        self.assertEqual(self.youcam.panel.notice_path, str(path))

    def test_cancelling_source_change_keeps_a_previously_paused_movie_paused(self):
        self.start_movie()
        self.youcam.recorder.pause()
        observations = []
        self.answer_source_dialog(False, observations)
        self.youcam.request_area("whole")
        self.assertEqual(observations[0][2], "Keep Paused")
        self.assertEqual(self.youcam.recorder.phase, "paused")
        self.assertEqual(self.youcam.area_mode, "camera")

    def test_save_failure_does_not_change_source_or_claim_success(self):
        path = self.start_movie()
        with patch.object(self.youcam, "_ask_source_change", return_value=True), \
                patch.object(self.youcam.recorder, "_finalize", side_effect=RuntimeError("Disk full")):
            self.youcam.request_area("whole")
        self.assertEqual(self.youcam.area_mode, "camera")
        self.assertEqual(self.youcam.recorder.phase, "idle")
        self.assertIsNone(self.youcam.recorder.last_path)
        self.assertFalse(path.exists())
        self.assertIn("could not be saved", self.youcam.panel.notice_text.text())
        self.assertFalse(self.youcam.panel.reveal_button.isVisible())

    def test_source_actions_enabled_during_recording_but_same_source_needs_no_confirmation(self):
        self.start_movie()
        menu = self.youcam._make_area_menu()
        self.assertTrue(self.youcam.panel.area_button.isEnabled())
        for title in ("Whole Screen", "Select Area…", "Window", "Camera Only (Full Frame)"):
            self.assertTrue(next(action for action in menu.actions() if action.text() == title).isEnabled())
        with patch.object(self.youcam, "_ask_source_change") as ask:
            self.youcam.request_area("camera")
            ask.assert_not_called()
        self.assertEqual(self.youcam.recorder.phase, "recording")

    def test_area_picker_does_not_start_if_source_change_is_cancelled(self):
        self.start_movie()
        with patch.object(self.youcam, "_ask_source_change", return_value=False):
            self.youcam.select_area()
        self.assertIsNone(self.youcam.selector)
        self.assertEqual(self.youcam.recorder.phase, "recording")
        with patch.object(self.youcam, "_ask_source_change", return_value=True):
            self.youcam.select_area()
        self.assertIsNotNone(self.youcam.selector)
        self.assertEqual(self.youcam.recorder.phase, "idle")
        QTest.keyClick(self.youcam.selector, Qt.Key.Key_Escape)
        self.assertEqual(self.youcam.area_mode, "camera")

    def test_interruption_notice_persists_and_stop_updates_it_to_saved(self):
        path = self.start_movie()
        self.youcam.recorder.interruption_reason = "Window minimized"
        self.youcam.recorder.pause()
        with patch.object(self.youcam.tray, "showMessage") as notify:
            self.youcam._recording_interrupted("Window minimized")
            notify.assert_called_once()
        self.assertTrue(self.youcam.panel.isVisible())
        self.assertIn("Resume", self.youcam.panel.notice_text.text())
        self.youcam._update_recording_ui()
        self.assertTrue(self.youcam.panel.notice.isVisible())
        self.youcam.stop_recording()
        self.assertEqual(self.youcam.panel.notice_title.text(), "Recording saved")
        self.assertEqual(self.youcam.panel.notice_path, str(path))
        with patch("youcam.app.QDesktopServices.openUrl") as reveal:
            QTest.mouseClick(self.youcam.panel.reveal_button, Qt.MouseButton.LeftButton)
            self.assertEqual(Path(reveal.call_args.args[0].toLocalFile()), path.parent)

    def test_closed_window_while_paused_saves_and_shows_persistent_notice(self):
        path = self.start_movie()
        self.youcam.recorder.pause()
        self.youcam.recorder.target = RecordingTarget(QRect(0, 0, 160, 120), "Window", kind="window", window_id=123)
        with patch("youcam.app.window_info", side_effect=RuntimeError("Closed")):
            self.youcam._check_paused_window()
        self.assertEqual(self.youcam.recorder.phase, "idle")
        self.assertTrue(path.exists())
        self.assertEqual(self.youcam.panel.notice_title.text(), "Recording stopped")
        self.assertIn(str(path), self.youcam.panel.notice_text.text())

    def test_pending_confirmation_blocks_shortcuts_and_duplicate_requests(self):
        self.start_movie()
        observations = []
        def decide(_destination, _was_recording):
            self.youcam.toggle_pause()
            self.youcam.stop_recording()
            self.youcam.request_area("whole")
            observations.append(self.youcam.recorder.phase)
            return False
        with patch.object(self.youcam, "_ask_source_change", side_effect=decide) as ask:
            self.youcam.request_area("whole")
            ask.assert_called_once()
        self.assertEqual(observations, ["paused"])
        self.assertEqual(self.youcam.recorder.phase, "recording")


if __name__ == "__main__":
    unittest.main()
