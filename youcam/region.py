from __future__ import annotations

from PySide6.QtCore import QPoint, QRect, Qt, Signal
from PySide6.QtGui import QColor, QGuiApplication, QKeyEvent, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import QHBoxLayout, QLabel, QPushButton, QWidget

from .core import constrain


def virtual_geometry() -> QRect:
    screen = QGuiApplication.primaryScreen()
    return screen.virtualGeometry() if screen is not None else QRect(0, 0, 1920, 1080)


class RegionSelector(QWidget):
    selected = Signal(QRect)
    cancelled = Signal()

    def __init__(self):
        super().__init__()
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint | Qt.WindowType.Tool)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setCursor(Qt.CursorShape.CrossCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.anchor: QPoint | None = None
        self.selection = QRect()
        self._selection_bounds = QRect()
        self._finished = False

    def begin(self) -> None:
        self._finished = False
        self.anchor = None
        self.selection = QRect()
        self.setGeometry(virtual_geometry())
        self.show()
        self.raise_()
        self.activateWindow()
        self.setFocus()
        self.grabKeyboard()
        self.grabMouse()

    def paintEvent(self, _event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(0, 0, 0, 105))
        if not self.selection.isEmpty():
            painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Clear)
            painter.fillRect(self.selection, Qt.GlobalColor.transparent)
            painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
            painter.setPen(QColor(255, 255, 255))
            painter.drawRect(self.selection)
        painter.setPen(QColor(255, 255, 255))
        font = painter.font()
        font.setPointSize(16)
        painter.setFont(font)
        message = "Drag to select a recording area · Esc to cancel"
        if not self.selection.isEmpty():
            message = f"{self.selection.width()} × {self.selection.height()} · Release to select · Esc to cancel"
        painter.drawText(self.rect().adjusted(20, 20, -20, -60), Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignBottom, message)

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.anchor = event.position().toPoint()
            screen = QGuiApplication.screenAt(event.globalPosition().toPoint()) or QGuiApplication.primaryScreen()
            self._selection_bounds = screen.geometry().translated(-self.geometry().topLeft())
            self.selection = QRect()
            self.update()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self.anchor is not None:
            self.selection = QRect(self.anchor, event.position().toPoint()).normalized().intersected(self._selection_bounds)
            self.update()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton or self.anchor is None:
            return
        self.mouseMoveEvent(event)
        if self.selection.width() >= 40 and self.selection.height() >= 40:
            self._finish(self.selection.translated(self.geometry().topLeft()))
        else:
            self.anchor = None
            self.selection = QRect()
            self.update()

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() == Qt.Key.Key_Escape:
            self._finish(None)
        else:
            super().keyPressEvent(event)

    def _finish(self, rect: QRect | None) -> None:
        if self._finished:
            return
        self._finished = True
        self.releaseKeyboard()
        self.releaseMouse()
        self.hide()
        if rect is None:
            self.cancelled.emit()
        else:
            self.selected.emit(rect)
        self.deleteLater()


class _DimOverlay(QWidget):
    def __init__(self):
        super().__init__()
        self.selection = QRect()
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowTransparentForInput
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)

    def set_selection(self, selection: QRect) -> None:
        self.selection = selection
        self.setGeometry(virtual_geometry())
        self.update()

    def paintEvent(self, _event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(0, 0, 0, 80))
        local = self.selection.translated(-self.geometry().topLeft())
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Clear)
        painter.fillRect(local, Qt.GlobalColor.transparent)
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
        painter.setPen(QColor(255, 255, 255, 220))
        painter.drawRect(local)


class _Handle(QWidget):
    dragStarted = Signal(QPoint)
    dragged = Signal(QPoint)
    finished = Signal()

    def __init__(self, move_handle: bool = False):
        super().__init__()
        self.move_handle = move_handle
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.setFixedSize(54 if move_handle else 18, 18)
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        self.setToolTip("Drag to move the recording area" if move_handle else "Drag to resize the recording area")

    def paintEvent(self, _event: QPaintEvent) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setBrush(QColor(255, 255, 255))
        painter.setPen(QColor(30, 30, 30))
        if self.move_handle:
            painter.drawRoundedRect(self.rect().adjusted(1, 1, -1, -1), 7, 7)
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "Move")
        else:
            painter.drawEllipse(self.rect().adjusted(2, 2, -2, -2))

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.dragStarted.emit(event.globalPosition().toPoint())
            event.accept()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if event.buttons() & Qt.MouseButton.LeftButton:
            self.dragged.emit(event.globalPosition().toPoint())
            event.accept()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.dragged.emit(event.globalPosition().toPoint())
            self.finished.emit()
            event.accept()


class _AreaControls(QWidget):
    selectRequested = Signal()
    wholeScreenRequested = Signal()

    def __init__(self):
        super().__init__()
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.setWindowTitle("YouCam Recording Area")
        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 6, 10, 6)
        self.size_label = QLabel()
        self.select_button = QPushButton("Select Area…")
        self.select_button.setToolTip("Draw a new recording area. Esc keeps the current area.")
        self.whole_screen_button = QPushButton("Whole Screen")
        self.whole_screen_button.setToolTip("Remove the selected area and record the whole screen")
        layout.addWidget(self.size_label)
        layout.addWidget(self.select_button)
        layout.addWidget(self.whole_screen_button)
        self.select_button.clicked.connect(self.selectRequested)
        self.whole_screen_button.clicked.connect(self.wholeScreenRequested)


class RegionEditor(QWidget):
    changed = Signal(QRect)
    selectRequested = Signal()
    wholeScreenRequested = Signal()

    def __init__(self):
        super().__init__()
        self.selection = QRect()
        self.overlay = _DimOverlay()
        self.handles = [_Handle(move_handle=index == 4) for index in range(5)]
        self.controls = _AreaControls()
        self.controls.selectRequested.connect(self.selectRequested)
        self.controls.wholeScreenRequested.connect(self.wholeScreenRequested)
        self._drag_start = QPoint()
        self._start_rect = QRect()
        self._drag_bounds = QRect()
        for index, handle in enumerate(self.handles):
            handle.dragStarted.connect(self._begin_drag)
            handle.dragged.connect(lambda point, i=index: self._drag(i, point))
            handle.finished.connect(self._finish_drag)

    def set_selection(self, selection: QRect) -> None:
        self.selection = selection.normalized()
        self.overlay.set_selection(self.selection)
        screen = QGuiApplication.screenAt(self.selection.center()) or QGuiApplication.primaryScreen()
        bounds = screen.geometry()
        positions = [
            self.selection.topLeft(),
            self.selection.topRight(),
            self.selection.bottomRight(),
            self.selection.bottomLeft(),
            QPoint(self.selection.center().x(), self.selection.top()),
        ]
        for handle, point in zip(self.handles, positions):
            frame = QRect(point - QPoint(handle.width() // 2, handle.height() // 2), handle.size())
            handle.move(constrain(frame, bounds).topLeft())
        self.controls.size_label.setText(f"{self.selection.width()} × {self.selection.height()}")
        self.controls.adjustSize()
        available = screen.availableGeometry()
        y = self.selection.top() - self.controls.height() - 20
        if y < available.top():
            y = self.selection.bottom() + 20
            if y + self.controls.height() > available.y() + available.height():
                y = self.selection.top() + 20
        frame = QRect(QPoint(self.selection.center().x() - self.controls.width() // 2, y), self.controls.size())
        self.controls.move(constrain(frame, available).topLeft())

    def show_editor(self, selection: QRect) -> None:
        self.set_selection(selection)
        self.overlay.show()
        for handle in self.handles:
            handle.show()
            handle.raise_()
        self.controls.show()
        self.controls.raise_()

    def hide_editor(self) -> None:
        self.overlay.hide()
        self.controls.hide()
        for handle in self.handles:
            handle.hide()

    def _begin_drag(self, point: QPoint) -> None:
        self._drag_start = point
        self._start_rect = QRect(self.selection)
        screen = QGuiApplication.screenAt(self.selection.center()) or QGuiApplication.primaryScreen()
        self._drag_bounds = screen.geometry()

    def _drag(self, index: int, point: QPoint) -> None:
        original = self._start_rect
        delta = point - self._drag_start
        bounds = self._drag_bounds
        if index == 4:
            candidate = QRect(original)
            candidate.moveTopLeft(original.topLeft() + delta)
            self.set_selection(constrain(candidate, bounds))
            return
        candidate = QRect(original)
        if index in (0, 3):
            candidate.setLeft(max(bounds.left(), min(original.left() + delta.x(), original.right() - 39)))
        else:
            candidate.setRight(min(bounds.right(), max(original.right() + delta.x(), original.left() + 39)))
        if index in (0, 1):
            candidate.setTop(max(bounds.top(), min(original.top() + delta.y(), original.bottom() - 39)))
        else:
            candidate.setBottom(min(bounds.bottom(), max(original.bottom() + delta.y(), original.top() + 39)))
        self.set_selection(candidate)

    def _finish_drag(self) -> None:
        self.changed.emit(QRect(self.selection))
