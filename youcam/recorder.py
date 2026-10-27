from __future__ import annotations

import functools
from contextlib import ExitStack
from collections.abc import Callable
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

import mss
import numpy as np
from PySide6.QtCore import QObject, QRect, Signal
from PySide6.QtGui import QImage

from .capture import ExclusionManager
from .core import RecordingTarget
from .sources import SourceUnavailable, WindowCapture, video_frame


PROCESS_FLAGS = {"creationflags": 0x08000000} if sys.platform == "win32" else {}


class RecordingError(RuntimeError):
    pass


def _ffmpeg() -> str:
    executable = shutil.which(os.environ.get("YOUCAM_FFMPEG", "ffmpeg"))
    if executable is None:
        raise RecordingError("FFmpeg was not found. Run setup.cmd, then launch with run.cmd from Miniforge Prompt.")
    return executable


@functools.lru_cache(maxsize=1)
def amf_available() -> bool:
    if sys.platform != "win32":
        return False
    command = [
        _ffmpeg(), "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "color=c=black:s=128x128:r=5",
        "-frames:v", "2", "-pix_fmt", "nv12", "-c:v", "h264_amf",
        "-usage", "transcoding", "-quality", "balanced", "-rc", "cqp",
        "-qp_i", "20", "-qp_p", "20", "-f", "null", "-",
    ]
    try:
        return subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=15, **PROCESS_FLAGS).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def select_encoder() -> str:
    _ffmpeg()
    requested = os.environ.get("YOUCAM_ENCODER", "auto").lower()
    if requested in ("software", "x264"):
        return "x264"
    if requested in ("amf", "hardware"):
        if not amf_available():
            raise RecordingError("AMD AMF was requested but a test encode failed. Check the Radeon driver and FFmpeg AMF support, or use YOUCAM_ENCODER=software.")
        return "h264_amf"
    if requested != "auto":
        raise RecordingError("YOUCAM_ENCODER must be auto, software, or amf.")
    return "h264_amf" if amf_available() else "x264"


class FFmpegSegment:
    def __init__(self, path: Path, size: tuple[int, int], audio: bool, encoder: str, fps: int = 30):
        self.path = path
        self.video_path = path.with_name(f"{path.stem}.video.mov")
        self.audio_path = path.with_name(f"{path.stem}.audio.f32")
        self.size = size
        self.audio = audio
        self.encoder = encoder
        self.fps = fps
        self.process: subprocess.Popen | None = None
        self.audio_file = None
        self.frames = 0

    def start(self) -> None:
        width, height = self.size
        command = [
            _ffmpeg(), "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo", "-pix_fmt", "bgra", "-s:v", f"{width}x{height}",
            "-r", str(self.fps), "-i", "pipe:0", "-map", "0:v:0",
        ]
        if self.encoder == "h264_amf":
            command += [
                "-c:v", "h264_amf", "-usage", "transcoding", "-quality", "balanced",
                "-rc", "cqp", "-qp_i", "20", "-qp_p", "20",
                "-pix_fmt", "nv12", "-g", str(self.fps * 2),
            ]
        else:
            command += [
                "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
                "-pix_fmt", "yuv420p", "-threads", "4", "-g", str(self.fps * 2),
            ]
        command += ["-movflags", "+faststart", str(self.video_path)]
        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, bufsize=0, **PROCESS_FLAGS,
        )
        if self.audio:
            try:
                self.audio_file = self.audio_path.open("wb", buffering=0)
            except Exception:
                self.abort()
                raise

    def write_video(self, data: bytes) -> None:
        if self.process is None or self.process.stdin is None:
            raise RecordingError("The video encoder is not running.")
        try:
            # Raw pipes can accept only part of a large frame on Windows.
            remaining = memoryview(data)
            while remaining:
                written = self.process.stdin.write(remaining)
                if not written:
                    raise BrokenPipeError("The encoder pipe was closed.")
                remaining = remaining[written:]
            self.frames += 1
        except BrokenPipeError as error:
            raise RecordingError("The video encoder stopped unexpectedly.") from error

    def write_audio(self, data: bytes) -> None:
        if self.audio_file is None:
            return
        try:
            self.audio_file.write(data)
        except OSError as error:
            raise RecordingError("The microphone samples could not be written. Check available disk space.") from error

    def finish(self) -> None:
        if self.process is None:
            return
        if self.process.stdin is not None:
            stdin = self.process.stdin
            self.process.stdin = None
            stdin.close()
        if self.audio_file is not None:
            self.audio_file.close()
            self.audio_file = None
        try:
            _, stderr = self.process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            _, stderr = self.process.communicate()
            raise RecordingError("FFmpeg did not finish the recording in time.")
        if self.process.returncode != 0:
            detail = stderr.decode(errors="replace").strip()
            raise RecordingError(detail or "FFmpeg could not save the recording.")
        self.process = None
        if not self.audio:
            os.replace(self.video_path, self.path)
            return
        command = [
            _ffmpeg(), "-y", "-hide_banner", "-loglevel", "error",
            "-i", str(self.video_path),
            "-f", "f32le", "-ar", "48000", "-ac", "1", "-i", str(self.audio_path),
            "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy",
            "-c:a", "aac", "-b:a", "96k", "-shortest", "-movflags", "+faststart", str(self.path),
        ]
        result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60, **PROCESS_FLAGS)
        if result.returncode != 0:
            raise RecordingError(result.stderr.decode(errors="replace").strip() or "The audio and video could not be joined.")
        self.video_path.unlink(missing_ok=True)
        self.audio_path.unlink(missing_ok=True)

    def abort(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.communicate()
        if self.audio_file is not None:
            self.audio_file.close()
            self.audio_file = None
        self.process = None


class ScreenRecorder(QObject):
    changed = Signal()
    failed = Signal(str)
    finished = Signal(str)
    interrupted = Signal(str)
    workerInterrupted = Signal(object, str)
    workerFailed = Signal(object, str)

    def __init__(self, exclusions: ExclusionManager, camera_frame: Callable[[], QImage] | None = None):
        super().__init__()
        self.exclusions = exclusions
        self.camera_frame = camera_frame
        self.phase = "idle"
        self.target: RecordingTarget | None = None
        self.output_path: Path | None = None
        self.last_path: Path | None = None
        self.encoder = ""
        self._tempdir: tempfile.TemporaryDirectory | None = None
        self._segments: list[FFmpegSegment] = []
        self._current: FFmpegSegment | None = None
        self._thread: threading.Thread | None = None
        self._stop_capture = threading.Event()
        self._elapsed_before = 0.0
        self._segment_started = 0.0
        self._audio_enabled = False
        self.interruption_reason = ""
        self._failure_message = ""
        self._protected_lock = threading.Lock()
        self._protected: QRect | None = None
        self.workerFailed.connect(self._worker_failure)
        self.workerInterrupted.connect(self._worker_interruption)

    @property
    def busy(self) -> bool:
        return self.phase != "idle"

    @property
    def elapsed(self) -> float:
        if self.phase == "recording":
            return self._elapsed_before + max(0.0, time.monotonic() - self._segment_started)
        return self._elapsed_before

    @property
    def time_label(self) -> str:
        seconds = max(0, int(self.elapsed))
        return f"{seconds // 3600:02d}:{seconds // 60 % 60:02d}:{seconds % 60:02d}"

    @property
    def status(self) -> str:
        if self.phase == "idle":
            return "Saved to Desktop" if self.last_path else "Ready to record"
        if self.phase == "starting":
            return "Starting recording…"
        if self.phase == "recording":
            return f"Recording · {self.time_label}"
        if self.phase == "paused":
            if self.interruption_reason:
                return f"Paused · {self.time_label} · Source unavailable"
            return f"Paused · {self.time_label}"
        return "Saving recording…"

    def set_protected_rect(self, rect: QRect | None) -> None:
        with self._protected_lock:
            self._protected = QRect(rect) if rect is not None else None

    def start(self, target: RecordingTarget, output_path: Path, audio: bool) -> None:
        if self.busy:
            return
        self.phase = "starting"
        self.target = target
        self.output_path = output_path
        self.last_path = None
        self._elapsed_before = 0.0
        self._segments = []
        self._audio_enabled = audio
        self.interruption_reason = ""
        self._failure_message = ""
        self.changed.emit()
        try:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            target.pixel_size
            self.encoder = select_encoder()
            self._tempdir = tempfile.TemporaryDirectory(prefix=".youcam-", dir=output_path.parent)
            self._start_segment()
        except Exception as error:
            self._cleanup()
            self.phase = "idle"
            self.changed.emit()
            self.failed.emit(str(error))

    def pause(self) -> None:
        if self.phase != "recording":
            return
        self._elapsed_before = self.elapsed
        try:
            self._finish_segment()
            self.phase = "paused"
        except Exception as error:
            self._fail(str(error))
            return
        self.changed.emit()

    def resume(self) -> None:
        if self.phase != "paused":
            return
        try:
            self.interruption_reason = ""
            self._start_segment()
        except Exception as error:
            self._fail(str(error))
            return
        self.changed.emit()

    def toggle_pause(self) -> None:
        if self.phase == "recording":
            self.pause()
        elif self.phase == "paused":
            self.resume()

    def stop(self) -> None:
        if self.phase not in ("recording", "paused"):
            return
        previous = self.phase
        self.phase = "finishing"
        self.changed.emit()
        if previous == "recording":
            self._elapsed_before += max(0.0, time.monotonic() - self._segment_started)
        try:
            if self._current is not None:
                self._finish_segment()
            self._finalize()
            self.last_path = self.output_path
        except Exception as error:
            message = self._failure_message or str(error)
            self._cleanup()
            self.phase = "idle"
            self.changed.emit()
            self.failed.emit(message)
            return
        self._cleanup()
        self.phase = "idle"
        self.changed.emit()
        self.finished.emit(str(self.output_path))
        if self._failure_message:
            self.failed.emit(self._failure_message)

    def write_audio(self, data: bytes, _timestamp: float) -> None:
        if self.phase != "recording" or self._current is None:
            return
        try:
            self._current.write_audio(data)
        except Exception as error:
            self._failure_message = str(error)
            self.stop()

    def _start_segment(self) -> None:
        if self.target is None or self._tempdir is None:
            raise RecordingError("No recording target was selected.")
        path = Path(self._tempdir.name) / f"segment-{len(self._segments):04d}.mov"
        segment = FFmpegSegment(path, self.target.pixel_size, self._audio_enabled, self.encoder)
        segment.start()
        self._segments.append(segment)
        self._current = segment
        self._stop_capture = threading.Event()
        self._thread = threading.Thread(
            target=self._capture_loop, args=(segment, self._stop_capture),
            name="youcam-screen-capture", daemon=True,
        )
        self._segment_started = time.monotonic()
        self.phase = "recording"
        self._thread.start()
        self.changed.emit()

    def _capture_loop(self, segment: FFmpegSegment, stop_event: threading.Event) -> None:
        try:
            if self.target is None:
                return
            target = self.target
            width, height = target.pixel_size
            monitor = {
                "left": target.capture_area.x(), "top": target.capture_area.y(),
                "width": target.capture_area.width(), "height": target.capture_area.height(),
            }
            interval = 1.0 / segment.fps
            next_tick = time.monotonic()
            with ExitStack() as stack:
                if target.kind == "window":
                    if target.window_id is None:
                        raise RecordingError("No window was selected.")
                    capture = stack.enter_context(WindowCapture(target.window_id, (width, height)))
                elif target.kind == "camera":
                    if self.camera_frame is None:
                        raise RecordingError("No camera is available.")
                elif target.kind == "screen":
                    capture = stack.enter_context(mss.MSS())
                else:
                    raise RecordingError("The recording source is not supported.")
                while not stop_event.is_set():
                    now = time.monotonic()
                    if next_tick > now:
                        stop_event.wait(next_tick - now)
                        continue
                    if target.kind == "window":
                        frame = capture.read()
                    elif target.kind == "camera":
                        frame = video_frame(self.camera_frame(), (width, height), target.mirrored)
                    else:
                        screenshot = capture.grab(monitor)
                        frame = np.asarray(screenshot)
                        if frame.shape[0] < height or frame.shape[1] < width:
                            raise RecordingError("The selected display is no longer available.")
                        frame = np.ascontiguousarray(frame[:height, :width])
                        with self._protected_lock:
                            protected = QRect(self._protected) if self._protected is not None else None
                        self.exclusions.mask(frame, target.capture_area, protected)
                    data = frame.tobytes()
                    segment.write_video(data)
                    next_tick += interval
                    now = time.monotonic()
                    missed = max(0, int((now - next_tick) / interval)) if now >= next_tick else 0
                    for _ in range(missed):
                        if stop_event.is_set():
                            break
                        segment.write_video(data)
                        next_tick += interval
        except SourceUnavailable as error:
            self.workerInterrupted.emit(segment, str(error))
        except Exception as error:
            self.workerFailed.emit(segment, str(error))

    def _finish_segment(self) -> None:
        self._stop_capture.set()
        if self._thread is not None:
            self._thread.join(20)
            if self._thread.is_alive():
                raise RecordingError("Screen capture did not stop in time.")
        self._thread = None
        if self._current is not None:
            if self._current.frames:
                self._current.finish()
            else:
                # A resume attempt while the source is hidden has no video.
                # Keep earlier segments and omit this empty one from the movie.
                self._current.abort()
                self._segments.remove(self._current)
            self._current = None

    def _finalize(self) -> None:
        if self.output_path is None or not self._segments or sum(segment.frames for segment in self._segments) == 0:
            raise RecordingError("No video frames were received; the recording was not saved.")
        if len(self._segments) == 1:
            os.replace(self._segments[0].path, self.output_path)
            return
        if self._tempdir is None:
            raise RecordingError("The recording temporary directory is unavailable.")
        listing = Path(self._tempdir.name) / "concat.txt"
        listing.write_text("".join(f"file '{segment.path.name}'\n" for segment in self._segments), encoding="utf-8")
        command = [
            _ffmpeg(), "-y", "-hide_banner", "-loglevel", "error",
            "-f", "concat", "-safe", "0", "-i", str(listing),
            "-c", "copy", "-movflags", "+faststart", str(self.output_path),
        ]
        result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60, **PROCESS_FLAGS)
        if result.returncode != 0:
            raise RecordingError(result.stderr.decode(errors="replace").strip() or "The recording segments could not be joined.")

    def _worker_failure(self, segment: FFmpegSegment, message: str) -> None:
        if segment is not self._current or self.phase != "recording":
            return
        self._failure_message = message
        self.stop()

    def _worker_interruption(self, segment: FFmpegSegment, message: str) -> None:
        if segment is not self._current or self.phase != "recording":
            return
        self.interruption_reason = message
        self.pause()
        if self.phase == "paused":
            self.interrupted.emit(message)

    def _fail(self, message: str) -> None:
        self._cleanup()
        self.phase = "idle"
        self.changed.emit()
        self.failed.emit(message)

    def _cleanup(self) -> None:
        self._stop_capture.set()
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(5)
        self._thread = None
        if self._current is not None:
            self._current.abort()
            self._current = None
        if self._tempdir is not None:
            self._tempdir.cleanup()
            self._tempdir = None
        self._segments = []
