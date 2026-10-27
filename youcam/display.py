from __future__ import annotations

from PySide6.QtCore import QRect

from .core import RecordingTarget


def physical_rect(rect: QRect, screen) -> QRect:
    """Map Qt logical coordinates to capture pixels on one Windows display.

    Windows screen origins stay in native pixels; only screen sizes and local
    offsets are scaled by Qt. Scaling the global origin breaks mixed-DPI setups.
    """
    geometry = screen.geometry()
    ratio = screen.devicePixelRatio()
    left = round((rect.x() - geometry.x()) * ratio)
    top = round((rect.y() - geometry.y()) * ratio)
    right = round((rect.x() + rect.width() - geometry.x()) * ratio)
    bottom = round((rect.y() + rect.height() - geometry.y()) * ratio)
    return QRect(geometry.x() + left, geometry.y() + top, right - left, bottom - top)


def screen_target(rect: QRect, screen) -> RecordingTarget:
    return RecordingTarget(QRect(rect), screen.name(), capture_rect=physical_rect(rect, screen))
