from __future__ import annotations

import ctypes
from ctypes import wintypes
import threading

from PySide6.QtCore import QObject, Signal

from .win32 import user32


class GlobalHotkeys(QObject):
    recordRequested = Signal()
    pauseRequested = Signal()
    availableChanged = Signal(bool)

    def __init__(self):
        super().__init__()
        self.available = False
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._thread = threading.Thread(target=self._run, name="youcam-hotkeys", daemon=True)
        self._thread.start()
        self._ready.wait(2)

    def _run(self) -> None:
        registered = []
        try:
            api = user32()
            signals = {1: self.recordRequested, 2: self.pauseRequested}
            # Ctrl+Alt+Shift avoids shortcuts reserved for the Windows key.
            modifiers = 0x0001 | 0x0002 | 0x0004 | 0x4000  # ALT, CTRL, SHIFT, NOREPEAT
            for identifier, key in ((1, "R"), (2, "P")):
                if not api.RegisterHotKey(None, identifier, modifiers, ord(key)):
                    return
                registered.append(identifier)
            self.available = True
            self.availableChanged.emit(True)
            self._ready.set()
            message = wintypes.MSG()
            while not self._stop.wait(0.03):
                while api.PeekMessageW(ctypes.byref(message), None, 0x0312, 0x0312, 1):
                    signal = signals.get(message.wParam)
                    if signal is not None:
                        signal.emit()
        except (OSError, RuntimeError):
            pass
        finally:
            for identifier in registered:
                api.UnregisterHotKey(None, identifier)
            self.available = False
            self.availableChanged.emit(False)
            self._ready.set()

    def shutdown(self) -> None:
        self._stop.set()
        self._thread.join(2)
