from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

import mss
import numpy as np
from PySide6.QtCore import QRect

from youcam.capture import ExclusionManager
from youcam.core import RecordingTarget
from youcam.recorder import FFmpegSegment, ScreenRecorder, select_encoder


def probe(path: Path) -> dict:
    result = subprocess.run(
        [shutil.which("ffprobe") or "ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return json.loads(result.stdout)


@unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "FFmpeg tools are required")
class RecorderTests(unittest.TestCase):
    def test_audio_video_segment(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "audio-video.mov"
            segment = FFmpegSegment(path, (160, 120), True, select_encoder(), fps=10)
            segment.start()
            for index in range(8):
                frame = np.zeros((120, 160, 4), dtype=np.uint8)
                frame[:, :, 0] = index * 20
                frame[:, :, 1] = 100
                frame[:, :, 2] = 200
                frame[:, :, 3] = 255
                segment.write_video(frame.tobytes())
                samples = np.full(4800, 0.05, dtype=np.float32)
                segment.write_audio(samples.tobytes())
            segment.finish()
            metadata = probe(path)
            streams = {stream["codec_type"]: stream for stream in metadata["streams"]}
            self.assertEqual(streams["video"]["codec_name"], "h264")
            self.assertEqual(streams["audio"]["codec_name"], "aac")
            self.assertEqual(streams["audio"]["sample_rate"], "48000")
            self.assertGreaterEqual(float(metadata["format"]["duration"]), 0.7)

    @unittest.skipUnless(os.environ.get("DISPLAY") or os.environ.get("YOUCAM_NATIVE_TESTS") == "1", "A live desktop is required")
    def test_real_screen_pause_resume_and_finalize(self):
        with mss.MSS() as capture:
            monitor = capture.monitors[1]
        target = RecordingTarget(
            QRect(monitor["left"] + 10, monitor["top"] + 10, 160, 120),
            "Test display",
        )
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "paused.mov"
            recorder = ScreenRecorder(ExclusionManager())
            errors: list[str] = []
            recorder.failed.connect(errors.append)
            recorder.start(target, output, False)
            self.assertEqual(recorder.phase, "recording")
            time.sleep(0.35)
            recorder.pause()
            self.assertEqual(recorder.phase, "paused")
            paused_elapsed = recorder.elapsed
            time.sleep(0.15)
            self.assertEqual(recorder.elapsed, paused_elapsed)
            recorder.resume()
            time.sleep(0.35)
            recorder.stop()
            self.assertEqual(errors, [])
            self.assertEqual(recorder.phase, "idle")
            self.assertTrue(output.exists())
            metadata = probe(output)
            video = next(stream for stream in metadata["streams"] if stream["codec_type"] == "video")
            self.assertEqual(video["codec_name"], "h264")
            self.assertEqual((video["width"], video["height"]), (160, 120))
            self.assertGreaterEqual(float(metadata["format"]["duration"]), 0.5)
            self.assertLess(float(metadata["format"]["duration"]), 2.5)
            decode = subprocess.run(
                [shutil.which("ffmpeg") or "ffmpeg", "-v", "error", "-i", str(output), "-f", "null", "-"],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
            self.assertEqual(decode.returncode, 0, decode.stderr.decode())


if __name__ == "__main__":
    unittest.main()
