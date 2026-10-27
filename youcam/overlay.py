from __future__ import annotations

from PySide6.QtCore import QPoint, QRect, Qt, Signal
from PySide6.QtGui import QColor, QImage, QMouseEvent, QPainter, QPainterPath, QPaintEvent, QRegion, QResizeEvent, QWheelEvent
from PySide6.QtWidgets import QWidget

from .core import CameraShape


class CameraOverlay(QWidget):
    moved = Signal()
    resizeRequested = Signal(int)
    contextMenuRequested = Signal(QPoint)

    def __init__(self):
        super().__init__()
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.setCursor(Qt.CursorShape.OpenHandCursor)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.NoContextMenu)
        self.shape = CameraShape.CIRCLE
        self.mirrored = True
        self.image = QImage()
        self.heading = "Starting camera…"
        self.detail = "Your camera will appear here."
        self._drag_position: QPoint | None = None
        self.setToolTip("Drag to move. Scroll to resize. Right-click for YouCam controls.")

    def set_shape(self, shape: CameraShape) -> None:
        self.shape = shape
        self._update_mask()
        self.update()

    def display_frame(self, image: QImage) -> None:
        self.image = image
        self.update()

    def display_state(self, heading: str, detail: str) -> None:
        self.heading = heading
        self.detail = detail
        self.update()

    def resizeEvent(self, event: QResizeEvent) -> None:
        super().resizeEvent(event)
        self._update_mask()

    def _update_mask(self) -> None:
        if self.shape is CameraShape.CIRCLE:
            self.setMask(QRegion(self.rect(), QRegion.RegionType.Ellipse))
        else:
            self.clearMask()
        self.update()

    def paintEvent(self, _event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        path = QPainterPath()
        if self.shape is CameraShape.CIRCLE:
            path.addEllipse(self.rect())
        else:
            path.addRoundedRect(self.rect(), 14, 14)
        painter.setClipPath(path)
        painter.fillPath(path, QColor(23, 23, 23))
        if not self.image.isNull():
            source = self.image
            scale = max(self.width() / source.width(), self.height() / source.height())
            width = round(source.width() * scale)
            height = round(source.height() * scale)
            target = QRect((self.width() - width) // 2, (self.height() - height) // 2, width, height)
            if self.mirrored:
                painter.save()
                painter.translate(self.width(), 0)
                painter.scale(-1, 1)
                painter.drawImage(target, source)
                painter.restore()
            else:
                painter.drawImage(target, source)
            return
        painter.setPen(QColor(255, 255, 255, 230))
        font = painter.font()
        font.setPointSize(11 if self.height() < 160 else 14)
        font.setBold(True)
        painter.setFont(font)
        heading_rect = self.rect().adjusted(12, 0, -12, 0)
        heading_rect.setTop(self.height() // 2 - 28)
        painter.drawText(heading_rect, Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop, self.heading)
        if self.height() >= 160 and self.width() >= 200:
            font.setPointSize(10)
            font.setBold(False)
            painter.setFont(font)
            painter.setPen(QColor(255, 255, 255, 170))
            detail_rect = self.rect().adjusted(24, self.height() // 2 + 8, -24, -12)
            painter.drawText(detail_rect, Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop | Qt.TextFlag.TextWordWrap, self.detail)

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.RightButton:
            self.contextMenuRequested.emit(event.globalPosition().toPoint())
            event.accept()
            return
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_position = event.globalPosition().toPoint() - self.pos()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            event.accept()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._drag_position is not None and event.buttons() & Qt.MouseButton.LeftButton:
            self.move(event.globalPosition().toPoint() - self._drag_position)
            event.accept()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton and self._drag_position is not None:
            self._drag_position = None
            self.setCursor(Qt.CursorShape.OpenHandCursor)
            self.moved.emit()
            event.accept()

    def wheelEvent(self, event: QWheelEvent) -> None:
        delta = event.angleDelta().y()
        if delta:
            self.resizeRequested.emit(delta)
            event.accept()
