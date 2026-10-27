from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from pathlib import Path

import numpy as np
from PySide6.QtCore import QRect, QSize


class CameraShape(str, Enum):
    CIRCLE = "circle"
    SQUARE = "square"
    RECTANGLE = "rectangle"

    @property
    def title(self) -> str:
        return {
            CameraShape.CIRCLE: "Circle",
            CameraShape.SQUARE: "Square",
            CameraShape.RECTANGLE: "Rectangle (16:9)",
        }[self]

    @property
    def aspect_ratio(self) -> float:
        return 16 / 9 if self is CameraShape.RECTANGLE else 1.0


MINIMUM_WIDTH = 120
MAXIMUM_WIDTH = 960


def size_for_width(width: float, shape: CameraShape, screen: QRect) -> QSize:
    limit = min(MAXIMUM_WIDTH, screen.width(), screen.height() * shape.aspect_ratio)
    safe_width = width if np.isfinite(width) else 280
    safe_width = min(max(safe_width, MINIMUM_WIDTH), limit)
    return QSize(round(safe_width), round(safe_width / shape.aspect_ratio))


def constrain(frame: QRect, screen: QRect) -> QRect:
    x = max(screen.x(), min(frame.x(), screen.x() + screen.width() - frame.width()))
    y = max(screen.y(), min(frame.y(), screen.y() + screen.height() - frame.height()))
    return QRect(x, y, frame.width(), frame.height())


def resized(frame: QRect, width: float, shape: CameraShape, screen: QRect) -> QRect:
    size = size_for_width(width, shape, screen)
    return constrain(QRect(frame.x(), frame.y(), size.width(), size.height()), screen)


def initial_frame(width: float, shape: CameraShape, screen: QRect) -> QRect:
    size = size_for_width(width, shape, screen)
    frame = QRect(
        screen.x() + screen.width() - size.width() - 32,
        screen.y() + screen.height() - size.height() - 32,
        size.width(),
        size.height(),
    )
    return constrain(frame, screen)


def even_size(width: int, height: int) -> tuple[int, int]:
    if width < 2 or height < 2:
        raise ValueError("The recording area is too small.")
    if width > 4096 or height > 4096:
        raise ValueError("The recording area exceeds the 4096-pixel encoder limit.")
    return max(2, width // 2 * 2), max(2, height // 2 * 2)


@dataclass(frozen=True)
class RecordingTarget:
    rect: QRect
    display_name: str
    kind: str = "screen"
    window_id: int | None = None
    mirrored: bool = False
    capture_rect: QRect | None = None

    @property
    def capture_area(self) -> QRect:
        return QRect(self.capture_rect if self.capture_rect is not None else self.rect)

    @property
    def pixel_size(self) -> tuple[int, int]:
        area = self.capture_area
        return even_size(area.width(), area.height())

    @property
    def title(self) -> str:
        if self.kind == "camera":
            return "Camera Only (Full Frame)"
        if self.kind == "window":
            return f"Window — {self.display_name}"
        return f"{self.rect.width()} × {self.rect.height()} on {self.display_name}"


def next_recording_path(directory: Path, now: datetime | None = None) -> Path:
    now = now or datetime.now()
    months = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
    stamp = f"{now.day:02d}{months[now.month - 1]}-{now:%H.%M.%S}"
    base = directory / f"YouCam-Recording-{stamp}"
    path = Path(f"{base}.mov")
    suffix = 2
    while path.exists():
        path = directory / f"{base.name}-{suffix}.mov"
        suffix += 1
    return path


def apply_gain(samples: np.ndarray, gain_db: float) -> tuple[np.ndarray, float, bool]:
    source = np.nan_to_num(samples.astype(np.float32, copy=False), nan=0.0, posinf=0.0, neginf=0.0)
    output = source * np.float32(10 ** (gain_db / 20))
    peak = float(np.max(np.abs(output))) if output.size else 0.0
    clipped = bool(output.size and (np.any(np.abs(source) >= 0.999) or np.any(np.abs(output) >= 0.999)))
    return np.clip(output, -1, 1).astype(np.float32, copy=False), peak, clipped


def save_area(settings, mode: str, screen_name: str, rect: QRect) -> None:
    settings.setValue("areaMode", mode)
    settings.setValue("areaScreen", screen_name)
    settings.setValue("areaRect", rect)


def load_area(settings, screens) -> tuple[str, str, QRect]:
    mode = settings.value("areaMode", "whole", type=str)
    screen_name = settings.value("areaScreen", "", type=str)
    rect = settings.value("areaRect", QRect())
    if mode == "camera":
        return "camera", "", QRect()
    # Native window handles are valid only for the current window's lifetime.
    if mode == "window":
        return "whole", "", QRect()
    if mode == "whole":
        return "whole", screen_name, rect if isinstance(rect, QRect) else QRect()
    screen = next((item for item in screens if item.name() == screen_name), None)
    if screen is None:
        return "whole", screen_name, rect if isinstance(rect, QRect) else QRect()
    if mode == "display":
        return "display", screen_name, QRect()
    if mode == "region" and isinstance(rect, QRect) and screen.geometry().contains(rect) and rect.width() >= 40 and rect.height() >= 40:
        return "region", screen_name, rect
    return "whole", screen_name, rect if isinstance(rect, QRect) else QRect()
