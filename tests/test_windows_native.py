from __future__ import annotations

import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import mss
import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication, QWidget

from youcam.capture import ExclusionManager
from youcam.core import RecordingTarget
from youcam.hotkeys import GlobalHotkeys
from youcam.recorder import ScreenRecorder
from youcam.sources import SourceError, SourceUnavailable, WindowCapture, list_windows, window_info
from youcam.win32 import exclude_from_capture, user32


def wait(seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        QApplication.processEvents()
        time.sleep(0.01)


@unittest.skipUnless(sys.platform == "win32" and os.environ.get("YOUCAM_NATIVE_TESTS") == "1",
                     "Run test-native.cmd on a Windows desktop")
class WindowsNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setQuitOnLastWindowClosed(False)
        cls.api = user32()
        cls.api.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        cls.api.ShowWindow.restype = wintypes.BOOL
        cls.api.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                         ctypes.c_int, ctypes.c_int, wintypes.UINT]
        cls.api.SetWindowPos.restype = wintypes.BOOL

    def setUp(self):
        self.child = subprocess.Popen([sys.executable, "-m", "tests.window_fixture"],
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.addCleanup(self.stop_child)
        lines = queue.Queue()
        threading.Thread(target=lambda: lines.put(self.child.stdout.readline()), daemon=True).start()
        self.hwnd = json.loads(lines.get(timeout=15))["hwnd"]
        self.info = window_info(self.hwnd)
        wait(0.2)

    def stop_child(self):
        self.child.terminate()
        self.child.wait(timeout=10)
        self.child.stdout.close()

    def test_window_capture_ignores_occlusion_and_tracks_resize(self):
        self.assertIn(self.hwnd, [item.id for item in list_windows()])
        cover = QWidget()
        cover.setWindowFlags(Qt.WindowType.WindowStaysOnTopHint)
        cover.setStyleSheet("background: #00ff00")
        cover.setGeometry(80, 80, 440, 340)
        cover.show()
        self.addCleanup(cover.close)
        wait(0.3)
        with WindowCapture(self.hwnd, (320, 240)) as capture:
            frame = capture.read()
            np.testing.assert_array_equal(frame[120, 80, :3], [0, 0, 255])
            np.testing.assert_array_equal(frame[120, 240, :3], [255, 0, 0])
            self.api.SetWindowPos(self.hwnd, None, 180, 160, 450, 350, 0x0004 | 0x0010)
            wait(0.3)
            resized = capture.read()
            self.assertEqual(resized.shape, (240, 320, 4))
            np.testing.assert_array_equal(resized[120, 80, :3], [0, 0, 255])

    def test_minimize_and_close_are_distinguished(self):
        self.api.ShowWindow(self.hwnd, 6)  # SW_MINIMIZE
        wait(0.2)
        with self.assertRaises(SourceUnavailable):
            window_info(self.hwnd)
        self.api.ShowWindow(self.hwnd, 9)  # SW_RESTORE
        wait(0.2)
        self.assertEqual(window_info(self.hwnd).id, self.hwnd)
        self.child.terminate()
        self.child.wait(timeout=10)
        with self.assertRaises(SourceError):
            window_info(self.hwnd)

    def test_hotkeys_release_registration_after_conflict(self):
        first = GlobalHotkeys()
        self.addCleanup(first.shutdown)
        self.assertTrue(first.available)
        second = GlobalHotkeys()
        self.addCleanup(second.shutdown)
        self.assertFalse(second.available)
        first.shutdown()
        third = GlobalHotkeys()
        self.addCleanup(third.shutdown)
        self.assertTrue(third.available)

    def test_native_exclusion_leaves_background_visible(self):
        cover = QWidget()
        cover.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint | Qt.WindowType.Tool)
        cover.setStyleSheet("background: #00ff00")
        cover.setGeometry(100, 100, 320, 240)
        self.assertTrue(exclude_from_capture(int(cover.winId())))
        cover.show()
        self.addCleanup(cover.close)
        wait(0.4)
        area = self.info.rect
        with mss.MSS() as capture:
            frame = np.asarray(capture.grab({"left": area.x() + area.width() // 4,
                                            "top": area.y() + area.height() // 2, "width": 1, "height": 1}))
        np.testing.assert_array_equal(frame[0, 0, :3], [0, 0, 255])

    def test_window_movie_minimize_pause_restore_resume_and_save(self):
        recorder = ScreenRecorder(ExclusionManager())
        self.addCleanup(recorder.stop)
        errors, interruptions = [], []
        recorder.failed.connect(errors.append)
        recorder.interrupted.connect(interruptions.append)
        with tempfile.TemporaryDirectory(prefix="YouCam prova è l'") as directory:
            output = Path(directory) / "native.mov"
            recorder.start(RecordingTarget(self.info.rect, self.info.title, kind="window", window_id=self.hwnd), output, False)
            wait(0.5)
            self.api.ShowWindow(self.hwnd, 6)
            wait(0.5)
            self.assertEqual(recorder.phase, "paused", errors)
            self.assertTrue(interruptions)
            self.api.ShowWindow(self.hwnd, 9)
            wait(0.2)
            recorder.resume()
            wait(0.5)
            recorder.stop()
            self.assertEqual(errors, [])
            self.assertTrue(output.is_file())
            decoded = subprocess.run(["ffmpeg", "-v", "error", "-i", str(output), "-f", "null", "-"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(decoded.returncode, 0, decoded.stderr.decode(errors="replace"))
