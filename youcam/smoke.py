from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import traceback

import numpy as np
from PySide6.QtCore import QRect
from PySide6.QtGui import QColor, QImage, QPainter

from .capture import ExclusionManager
from .core import CameraShape
from .overlay import CameraOverlay
from .recorder import FFmpegSegment, select_encoder


def _probe(path: Path) -> dict:
    result = subprocess.run(
        [shutil.which("ffprobe") or "ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return json.loads(result.stdout)


def run_smoke_test(app) -> int:
    try:
        overlay = CameraOverlay()
        overlay.setGeometry(QRect(0, 0, 280, 280))
        overlay.set_shape(CameraShape.CIRCLE)
        image = QImage(640, 360, QImage.Format.Format_RGBA8888)
        image.fill(QColor(35, 110, 180))
        painter = QPainter(image)
        painter.setPen(QColor(255, 255, 255))
        painter.drawText(image.rect(), 0x84, "Synthetic camera")
        painter.end()
        overlay.display_frame(image)
        overlay.show()
        app.processEvents()
        rendered = overlay.grab().toImage()
        if rendered.isNull() or rendered.deviceIndependentSize().toSize() != overlay.size():
            raise RuntimeError("The camera overlay did not render.")
        overlay.close()

        with tempfile.TemporaryDirectory() as directory:
            movie = Path(directory) / "smoke.mov"
            segment = FFmpegSegment(movie, (320, 240), True, select_encoder(), fps=30)
            segment.start()
            for index in range(15):
                frame = np.zeros((240, 320, 4), dtype=np.uint8)
                frame[:, :, 0] = (index * 13) % 255
                frame[:, :, 1] = np.arange(320, dtype=np.uint8)
                frame[:, :, 2] = 180
                frame[:, :, 3] = 255
                segment.write_video(frame.tobytes())
                samples = np.zeros(16000, dtype=np.float32)
                samples[::200] = 0.1
                segment.write_audio(samples.tobytes())
            segment.finish()
            metadata = _probe(movie)
            codecs = {stream["codec_name"] for stream in metadata["streams"]}
            if "h264" not in codecs or "aac" not in codecs:
                raise RuntimeError(f"Unexpected recording streams: {codecs}")
            subprocess.run(
                [shutil.which("ffmpeg") or "ffmpeg", "-v", "error", "-i", str(movie), "-f", "null", "-"],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
        print("YouCam smoke test passed: overlay rendered; H.264/AAC recording encoded and decoded.")
        return 0
    except Exception:
        traceback.print_exc()
        return 1
