from __future__ import annotations

import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import os
import threading

import numpy as np
from PySide6.QtCore import QRect, Qt
from PySide6.QtGui import QImage, QPainter

from .win32 import gdi32, user32, window_cloaked


class SourceError(RuntimeError):
    pass


class SourceUnavailable(SourceError):
    """A temporarily hidden source can be restored and resumed."""


@dataclass(frozen=True)
class WindowInfo:
    id: int
    title: str
    rect: QRect


def window_info(window_id: int) -> WindowInfo:
    api = user32()
    if not api.IsWindow(window_id):
        raise SourceError("The selected window was closed. Select another window.")
    if api.IsIconic(window_id) or not api.IsWindowVisible(window_id) or window_cloaked(window_id):
        raise SourceUnavailable("The selected window is minimized or hidden. Restore it before resuming.")
    rect = wintypes.RECT()
    origin = wintypes.POINT()
    if not api.GetClientRect(window_id, ctypes.byref(rect)) or not api.ClientToScreen(window_id, ctypes.byref(origin)):
        raise SourceError("The selected window is no longer available.")
    if rect.right < 2 or rect.bottom < 2:
        raise SourceUnavailable("The selected window has no visible content.")
    title = ctypes.create_unicode_buffer(api.GetWindowTextLengthW(window_id) + 1)
    api.GetWindowTextW(window_id, title, len(title))
    return WindowInfo(window_id, title.value or "Untitled window", QRect(origin.x, origin.y, rect.right, rect.bottom))


def list_windows() -> list[WindowInfo]:
    api = user32()
    result = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def visit(hwnd, _data):
        pid = wintypes.DWORD()
        api.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == os.getpid() or not api.GetWindowTextLengthW(hwnd):
            return True
        try:
            result.append(window_info(hwnd))
        except SourceError:
            pass  # A window may disappear while the menu is being populated.
        return True

    api.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    api.EnumWindows.restype = wintypes.BOOL
    if not api.EnumWindows(visit, 0):
        raise SourceError("Windows could not enumerate the available windows.")
    return sorted(result, key=lambda item: (item.title.casefold(), item.id))


def video_frame(image: QImage, size: tuple[int, int], mirrored: bool = False) -> np.ndarray:
    """Fit the complete source into a fixed movie size, padding with black."""
    if image.isNull():
        raise SourceError("No video frames are available from the selected source.")
    width, height = size
    image = image.scaled(width, height, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
    canvas = QImage(width, height, QImage.Format.Format_RGB32)
    canvas.fill(Qt.GlobalColor.black)
    painter = QPainter(canvas)
    if mirrored:
        painter.translate(width, 0)
        painter.scale(-1, 1)
    painter.drawImage((width - image.width()) // 2, (height - image.height()) // 2, image)
    painter.end()
    canvas = canvas.convertToFormat(QImage.Format.Format_RGBA8888)
    rgba = np.frombuffer(canvas.constBits(), dtype=np.uint8).reshape(height, canvas.bytesPerLine())[:, :width * 4]
    return np.ascontiguousarray(rgba.reshape(height, width, 4)[:, :, [2, 1, 0, 3]])



class BitmapInfo(ctypes.Structure):
    _fields_ = [
        ("size", wintypes.DWORD), ("width", wintypes.LONG), ("height", wintypes.LONG),
        ("planes", wintypes.WORD), ("bits", wintypes.WORD),
        ("compression", wintypes.DWORD), ("image_size", wintypes.DWORD),
        ("x_pixels", wintypes.LONG), ("y_pixels", wintypes.LONG),
        ("colors", wintypes.DWORD), ("important_colors", wintypes.DWORD),
        ("color_table", wintypes.DWORD * 3),
    ]


def _window_image(window_id: int) -> QImage:
    info = window_info(window_id)
    api, gdi = user32(), gdi32()
    bounds = wintypes.RECT()
    if not api.GetWindowRect(window_id, ctypes.byref(bounds)):
        raise SourceError("The selected window is no longer available.")
    width, height = bounds.right - bounds.left, bounds.bottom - bounds.top
    if width < 2 or height < 2 or width > 16384 or height > 16384:
        raise SourceError("The selected window has an unsupported size.")
    dc = api.GetDC(window_id)
    memory = bitmap = previous = None
    try:
        if not dc:
            raise SourceError("Windows could not open the window drawing context.")
        memory = gdi.CreateCompatibleDC(dc)
        bitmap = gdi.CreateCompatibleBitmap(dc, width, height)
        if not memory or not bitmap:
            raise SourceError("Windows could not allocate a window capture buffer.")
        previous = gdi.SelectObject(memory, bitmap)
        # PW_RENDERFULLCONTENT asks the compositor for covered window content.
        # Do not fall back to a desktop crop: it would record unrelated windows.
        if not api.PrintWindow(window_id, memory, 2):
            raise SourceError("This application cannot be captured as a window. Try screen or area recording.")
        gdi.SelectObject(memory, previous)
        previous = None
        header = BitmapInfo()
        header.size = 40  # BITMAPINFOHEADER, excluding the color table.
        header.width, header.height = width, -height  # Top-down BGRA pixels.
        header.planes, header.bits = 1, 32
        pixels = np.empty((height, width, 4), dtype=np.uint8)
        if gdi.GetDIBits(memory, bitmap, 0, height, pixels.ctypes.data, ctypes.byref(header), 0) != height:
            raise SourceError("Windows could not read the window pixels.")
        pixels[:, :, 3] = 255
        image = QImage(pixels.data, width, height, width * 4, QImage.Format.Format_RGB32)
        client = QRect(info.rect.x() - bounds.left, info.rect.y() - bounds.top, info.rect.width(), info.rect.height())
        if not image.rect().contains(client):
            raise SourceUnavailable("The window changed size during capture. Resume to try again.")
        return image.copy(client)
    finally:
        if previous:
            gdi.SelectObject(memory, previous)
        if bitmap:
            gdi.DeleteObject(bitmap)
        if memory:
            gdi.DeleteDC(memory)
        if dc:
            api.ReleaseDC(window_id, dc)


class WindowCapture:
    """Read a window's client area without blocking on an unresponsive app."""

    def __init__(self, window_id: int, size: tuple[int, int]):
        self.window_id, self.size = window_id, size
        self._request = threading.Event()
        self._done = threading.Event()
        self._stop = threading.Event()
        self._thread = None
        self._image = QImage()
        self._error = None

    def __enter__(self):
        window_info(self.window_id)
        # PrintWindow is synchronous and can hang inside another application.
        # This daemon owns all GDI resources, including cleanup if it returns late.
        self._thread = threading.Thread(target=self._run, name="youcam-window-pixels", daemon=True)
        self._thread.start()
        return self

    def _run(self):
        while True:
            self._request.wait()
            self._request.clear()
            if self._stop.is_set():
                return
            try:
                self._image = _window_image(self.window_id)
                self._error = None
            except Exception as error:
                self._error = error
            self._done.set()

    def read(self) -> np.ndarray:
        window_info(self.window_id)
        self._done.clear()
        self._request.set()
        if not self._done.wait(3):
            raise SourceError("The selected application stopped responding to capture. The recording has stopped.")
        if self._error is not None:
            raise self._error
        return video_frame(self._image, self.size)

    def close(self) -> None:
        self._stop.set()
        self._request.set()
        if self._thread is not None:
            self._thread.join(0.1)

    def __exit__(self, *_exc) -> None:
        self.close()
