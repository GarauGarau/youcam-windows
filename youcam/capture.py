from __future__ import annotations

import threading
import sys

import numpy as np
from PySide6.QtCore import QEvent, QObject, QPoint, QRect
from PySide6.QtGui import QGuiApplication, QHideEvent, QImage
from PySide6.QtWidgets import QMenu, QWidget

from .display import physical_rect
from .win32 import exclude_from_capture


class ControlExclusions(QObject):
    """Mark each native control window when Qt creates or recreates its HWND."""

    def __init__(self, camera: QWidget, parent=None):
        super().__init__(parent)
        self.camera = camera
        self._updating = False

    def eventFilter(self, watched, event):
        if (not self._updating and sys.platform == "win32"
                and QGuiApplication.platformName() == "windows"
                and event.type() in (QEvent.Type.Show, QEvent.Type.WinIdChange)
                and isinstance(watched, QWidget) and watched.isWindow()
                and watched is not self.camera):
            self._updating = True
            try:
                exclude_from_capture(int(watched.winId()))
            finally:
                self._updating = False
        return False


def _subtract(rect: QRect, cut: QRect | None) -> list[QRect]:
    if cut is None:
        return [rect]
    blocked = rect.intersected(cut)
    if blocked.isEmpty():
        return [rect]
    regions: list[QRect] = []
    if blocked.y() > rect.y():
        regions.append(QRect(rect.x(), rect.y(), rect.width(), blocked.y() - rect.y()))
    bottom = blocked.y() + blocked.height()
    rect_bottom = rect.y() + rect.height()
    if bottom < rect_bottom:
        regions.append(QRect(rect.x(), bottom, rect.width(), rect_bottom - bottom))
    if blocked.x() > rect.x():
        regions.append(QRect(rect.x(), blocked.y(), blocked.x() - rect.x(), blocked.height()))
    right = blocked.x() + blocked.width()
    rect_right = rect.x() + rect.width()
    if right < rect_right:
        regions.append(QRect(right, blocked.y(), rect_right - right, blocked.height()))
    return regions


class ExclusionManager:
    def __init__(self):
        self._lock = threading.Lock()
        self._entries: dict[int, tuple[QWidget, QRect, np.ndarray]] = {}

    def add(self, widget: QWidget, rect: QRect | None = None) -> None:
        if sys.platform == "win32" and QGuiApplication.platformName() == "windows":
            if exclude_from_capture(int(widget.winId())):
                self.remove(widget)
                return
        if widget.isVisible() and id(widget) not in self._entries:
            return
        rect = QRect(rect or widget.frameGeometry())
        if rect.width() < 1 or rect.height() < 1:
            return
        screen = QGuiApplication.screenAt(rect.center()) or QGuiApplication.primaryScreen()
        if screen is None:
            return
        image = screen.grabWindow(0, rect.x(), rect.y(), rect.width(), rect.height()).toImage()
        image = image.convertToFormat(QImage.Format.Format_RGBA8888)
        if image.isNull():
            return
        lines = np.frombuffer(image.constBits(), dtype=np.uint8).reshape(image.height(), image.bytesPerLine())
        rgba = lines[:, :image.width() * 4].reshape(image.height(), image.width(), 4).copy()
        bgra = np.ascontiguousarray(rgba[:, :, [2, 1, 0, 3]])
        if sys.platform == "win32":
            rect = physical_rect(rect, screen)
        with self._lock:
            self._entries[id(widget)] = (widget, rect, bgra)

    def remove(self, widget: QWidget) -> None:
        with self._lock:
            self._entries.pop(id(widget), None)

    def mask(self, frame: np.ndarray, target: QRect, protected: QRect | None = None) -> None:
        with self._lock:
            entries = list(self._entries.values())
        for _widget, rect, background in entries:
            intersection = target.intersected(rect)
            if intersection.isEmpty():
                continue
            for region in _subtract(intersection, protected):
                dx = region.x() - target.x()
                dy = region.y() - target.y()
                sx = region.x() - rect.x()
                sy = region.y() - rect.y()
                if sx < 0 or sy < 0 or sx + region.width() > background.shape[1] or sy + region.height() > background.shape[0]:
                    continue
                frame[dy:dy + region.height(), dx:dx + region.width()] = background[sy:sy + region.height(), sx:sx + region.width()]


class SnapshotMenu(QMenu):
    def __init__(self, exclusions: ExclusionManager, title: str = "", parent: QWidget | None = None):
        super().__init__(title, parent)
        self.exclusions = exclusions
        self.aboutToShow.connect(self._snapshot_submenu)

    def _snapshot_submenu(self) -> None:
        if not self.isVisible():
            self.exclusions.add(self)

    def popup(self, pos: QPoint, action=None) -> None:
        self.adjustSize()
        screen = QGuiApplication.screenAt(pos) or QGuiApplication.primaryScreen()
        if screen is not None:
            available = screen.availableGeometry()
            size = self.sizeHint()
            pos = QPoint(
                max(available.x(), min(pos.x(), available.x() + available.width() - size.width())),
                max(available.y(), min(pos.y(), available.y() + available.height() - size.height())),
            )
        self.exclusions.add(self, QRect(pos, self.sizeHint()))
        super().popup(pos, action)

    def hideEvent(self, event: QHideEvent) -> None:
        super().hideEvent(event)
        self.exclusions.remove(self)
