from __future__ import annotations

import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from PySide6.QtCore import QRect

from youcam.display import physical_rect, screen_target
from youcam.recorder import FFmpegSegment, RecordingError, select_encoder


class DisplayTests(unittest.TestCase):
    def test_mixed_dpi_preserves_native_origin_and_scales_local_offsets(self):
        screen = SimpleNamespace(geometry=lambda: QRect(-2560, 120, 1707, 960),
                                 devicePixelRatio=lambda: 1.5, name=lambda: "Left")
        logical = QRect(-2460, 160, 400, 300)
        expected = QRect(-2410, 180, 600, 450)
        self.assertEqual(physical_rect(logical, screen), expected)
        target = screen_target(logical, screen)
        self.assertEqual(target.rect, logical)
        self.assertEqual(target.capture_area, expected)
        self.assertEqual(target.pixel_size, (600, 450))

    def test_fractional_scaling_uses_both_edges_without_gaps(self):
        screen = SimpleNamespace(geometry=lambda: QRect(1920, 0, 1536, 864), devicePixelRatio=lambda: 1.25)
        first = physical_rect(QRect(1921, 0, 41, 50), screen)
        second = physical_rect(QRect(1962, 0, 41, 50), screen)
        self.assertEqual(first.x() + first.width(), second.x())


class EncoderTests(unittest.TestCase):
    def test_auto_falls_back_when_amf_probe_fails(self):
        with patch.dict(os.environ, {"YOUCAM_ENCODER": "auto"}), patch("youcam.recorder._ffmpeg", return_value="ffmpeg"), patch("youcam.recorder.amf_available", return_value=False):
            self.assertEqual(select_encoder(), "x264")

    def test_auto_uses_amf_only_after_successful_probe(self):
        with patch.dict(os.environ, {"YOUCAM_ENCODER": "auto"}), patch("youcam.recorder._ffmpeg", return_value="ffmpeg"), patch("youcam.recorder.amf_available", return_value=True):
            self.assertEqual(select_encoder(), "h264_amf")

    def test_explicit_amf_failure_is_reported(self):
        with patch.dict(os.environ, {"YOUCAM_ENCODER": "amf"}), patch("youcam.recorder._ffmpeg", return_value="ffmpeg"), patch("youcam.recorder.amf_available", return_value=False):
            with self.assertRaises(RecordingError):
                select_encoder()

    def test_software_does_not_probe_gpu(self):
        with patch.dict(os.environ, {"YOUCAM_ENCODER": "software"}), patch("youcam.recorder._ffmpeg", return_value="ffmpeg"), patch("youcam.recorder.amf_available") as probe:
            self.assertEqual(select_encoder(), "x264")
            probe.assert_not_called()

    def test_raw_video_handles_partial_pipe_writes(self):
        received = bytearray()
        def write(data):
            part = data[:7]
            received.extend(part)
            return len(part)
        segment = FFmpegSegment(Path("unused.mov"), (4, 4), False, "x264")
        segment.process = SimpleNamespace(stdin=SimpleNamespace(write=write))
        data = bytes(range(64))
        segment.write_video(data)
        self.assertEqual(received, data)
        self.assertEqual(segment.frames, 1)

    def test_closed_pipe_does_not_count_an_incomplete_frame(self):
        segment = FFmpegSegment(Path("unused.mov"), (4, 4), False, "x264")
        segment.process = SimpleNamespace(stdin=SimpleNamespace(write=lambda data: 0))
        with self.assertRaises(RecordingError):
            segment.write_video(bytes(64))
        self.assertEqual(segment.frames, 0)
