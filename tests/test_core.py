from __future__ import annotations

from datetime import datetime
from pathlib import Path
import tempfile
import unittest

import numpy as np
from PySide6.QtCore import QRect

from youcam.core import CameraShape, apply_gain, constrain, even_size, initial_frame, next_recording_path, resized, size_for_width


class GeometryTests(unittest.TestCase):
    def setUp(self):
        self.screen = QRect(100, 50, 1920, 1080)

    def test_rectangle_aspect_ratio_and_limits(self):
        size = size_for_width(400, CameraShape.RECTANGLE, self.screen)
        self.assertEqual((size.width(), size.height()), (400, 225))
        self.assertEqual(size_for_width(1, CameraShape.CIRCLE, self.screen).width(), 120)
        self.assertEqual(size_for_width(100000, CameraShape.CIRCLE, self.screen).width(), 960)
        self.assertEqual(size_for_width(float("nan"), CameraShape.CIRCLE, self.screen).width(), 280)

    def test_resize_keeps_top_left_and_constrains_to_screen(self):
        frame = QRect(1900, 1000, 280, 280)
        result = resized(frame, 400, CameraShape.CIRCLE, self.screen)
        self.assertEqual(result.x() + result.width(), self.screen.x() + self.screen.width())
        self.assertEqual(result.y() + result.height(), self.screen.y() + self.screen.height())
        self.assertEqual(constrain(result, self.screen), result)

    def test_initial_position_is_inside_bottom_right(self):
        frame = initial_frame(280, CameraShape.CIRCLE, self.screen)
        self.assertTrue(self.screen.contains(frame))
        self.assertEqual(frame.x() + frame.width() + 32, self.screen.x() + self.screen.width())
        self.assertEqual(frame.y() + frame.height() + 32, self.screen.y() + self.screen.height())

    def test_encoder_dimensions_are_even_and_bounded(self):
        self.assertEqual(even_size(1919, 1079), (1918, 1078))
        with self.assertRaises(ValueError):
            even_size(1, 100)
        with self.assertRaises(ValueError):
            even_size(5000, 100)


class FileTests(unittest.TestCase):
    def test_recording_names_do_not_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            now = datetime(2026, 10, 27, 9, 8, 7)
            first = next_recording_path(root, now)
            self.assertEqual(first.name, "YouCam-Recording-27Oct-09.08.07.mov")
            first.touch()
            second = next_recording_path(root, now)
            self.assertEqual(second.name, "YouCam-Recording-27Oct-09.08.07-2.mov")


class AudioTests(unittest.TestCase):
    def test_gain_meter_and_clipping(self):
        samples = np.array([0.25, -0.75, np.nan, np.inf], dtype=np.float32)
        output, peak, clipped = apply_gain(samples, 6)
        self.assertAlmostEqual(float(output[0]), 0.25 * 10 ** (6 / 20), places=5)
        self.assertGreater(peak, 0.99)
        self.assertTrue(clipped)
        self.assertTrue(np.isfinite(output).all())
        self.assertLessEqual(float(np.max(np.abs(output))), 1.0)

    def test_silence(self):
        output, peak, clipped = apply_gain(np.zeros(100, dtype=np.float32), 0)
        self.assertEqual(peak, 0)
        self.assertFalse(clipped)
        self.assertEqual(output.dtype, np.float32)


if __name__ == "__main__":
    unittest.main()
