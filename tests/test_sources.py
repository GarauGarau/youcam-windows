from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import numpy as np
from PySide6.QtCore import QRect, QSettings
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtWidgets import QApplication

from youcam.camera import CameraController
from youcam.capture import ExclusionManager
from youcam.core import RecordingTarget
from youcam.recorder import ScreenRecorder
from youcam.sources import SourceError, video_frame


def wait(milliseconds: int) -> None:
    # Yield to the Python capture thread as well as the Qt event loop.
    deadline = time.monotonic() + milliseconds / 1000
    while time.monotonic() < deadline:
        QApplication.processEvents()
        time.sleep(0.01)


def camera_image() -> QImage:
    image = QImage(160, 120, QImage.Format.Format_RGB32)
    image.fill(QColor("red"))
    painter = QPainter(image)
    painter.fillRect(80, 0, 80, 120, QColor("blue"))
    painter.end()
    return image


def decode(path: Path, width: int = 160, height: int = 120) -> np.ndarray:
    raw = subprocess.check_output([
        "ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1",
    ])
    return np.frombuffer(raw, dtype=np.uint8).reshape(-1, height, width, 3)


class FrameTests(unittest.TestCase):
    def test_camera_fills_output_including_corners_and_supports_mirroring(self):
        image = camera_image()
        frame = video_frame(image, (160, 120))
        np.testing.assert_array_equal(frame[0, 0], [0, 0, 255, 255])
        np.testing.assert_array_equal(frame[-1, -1], [255, 0, 0, 255])
        mirrored = video_frame(image, (160, 120), mirrored=True)
        np.testing.assert_array_equal(mirrored[0, 0], [255, 0, 0, 255])
        np.testing.assert_array_equal(mirrored[-1, -1], [0, 0, 255, 255])

    def test_resized_source_preserves_aspect_ratio_with_black_padding(self):
        frame = video_frame(camera_image(), (160, 160))
        self.assertTrue(np.all(frame[:20, :, :3] == 0))
        self.assertTrue(np.all(frame[140:, :, :3] == 0))
        np.testing.assert_array_equal(frame[20, 0], [0, 0, 255, 255])

    def test_camera_rejects_missing_and_stale_frames(self):
        app = QApplication.instance() or QApplication([])
        with tempfile.TemporaryDirectory() as directory:
            camera = CameraController(QSettings(str(Path(directory) / "camera.ini"), QSettings.Format.IniFormat))
            with self.assertRaises(RuntimeError):
                camera.recording_frame()
            with patch("youcam.camera.time.monotonic", return_value=100):
                camera._frame_changed(SimpleNamespace(isValid=lambda: True, toImage=camera_image))
                self.assertFalse(camera.recording_frame().isNull())
            with patch("youcam.camera.time.monotonic", return_value=104):
                with self.assertRaises(RuntimeError):
                    camera.recording_frame()
            camera.shutdown()


@unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "FFmpeg tools are required")
class CameraRecordingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def test_camera_only_movie_audio_pause_resume_and_no_desktop_access(self):
        image = camera_image()
        exclusions = ExclusionManager()
        recorder = ScreenRecorder(exclusions, lambda: image.copy())
        errors = []
        recorder.failed.connect(errors.append)
        target = RecordingTarget(QRect(0, 0, 160, 120), "Camera", kind="camera", mirrored=True)
        with tempfile.TemporaryDirectory() as directory, patch("youcam.recorder.mss.MSS") as desktop, patch.object(exclusions, "mask") as mask:
            output = Path(directory) / "camera.mov"
            recorder.start(target, output, True)
            for _ in range(10):
                recorder.write_audio(np.zeros(1600, dtype=np.float32).tobytes(), time.monotonic())
                wait(34)
            recorder.pause()
            self.assertEqual(recorder.phase, "paused")
            recorder.resume()
            for _ in range(10):
                recorder.write_audio(np.zeros(1600, dtype=np.float32).tobytes(), time.monotonic())
                wait(34)
            recorder.stop()
            self.assertEqual(errors, [])
            self.assertEqual(recorder.phase, "idle")
            desktop.assert_not_called()
            mask.assert_not_called()
            metadata = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(output)]))
            self.assertEqual({s["codec_type"]: s["codec_name"] for s in metadata["streams"]}, {"video": "h264", "audio": "aac"})
            frames = decode(output)
            self.assertGreater(len(frames), 10)
            np.testing.assert_allclose(frames[-1, 5, 5], [0, 0, 255], atol=5)
            np.testing.assert_allclose(frames[-1, -5, -5], [255, 0, 0], atol=5)

    def test_camera_disconnect_stops_and_saves_received_frames(self):
        frames = [camera_image()]
        recorder = ScreenRecorder(ExclusionManager(), lambda: frames[0])
        errors = []
        recorder.failed.connect(errors.append)
        with tempfile.TemporaryDirectory() as directory, patch("youcam.recorder.mss.MSS") as desktop:
            output = Path(directory) / "disconnected.mov"
            recorder.start(RecordingTarget(QRect(0, 0, 160, 120), "Camera", kind="camera"), output, False)
            wait(220)
            frames[0] = QImage()
            for _ in range(50):
                if recorder.phase == "idle":
                    break
                wait(20)
            self.assertEqual(recorder.phase, "idle")
            self.assertEqual(len(errors), 1)
            self.assertTrue(output.exists())
            self.assertGreater(len(decode(output)), 0)
            desktop.assert_not_called()

    def test_source_loss_during_pause_preserves_the_previous_segment(self):
        frames = [camera_image()]
        recorder = ScreenRecorder(ExclusionManager(), lambda: frames[0])
        errors = []
        recorder.failed.connect(errors.append)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "lost-during-pause.mov"
            recorder.start(RecordingTarget(QRect(0, 0, 160, 120), "Camera", kind="camera"), output, False)
            wait(220)
            recorder.pause()
            frames[0] = QImage()
            recorder.resume()
            for _ in range(50):
                if recorder.phase == "idle":
                    break
                wait(20)
            self.assertEqual(recorder.phase, "idle")
            self.assertEqual(len(errors), 1)
            self.assertTrue(output.exists())
            self.assertGreater(len(decode(output)), 0)
