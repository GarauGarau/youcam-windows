"""Separate GUI process: PrintWindow must not depend on the test's event loop."""
import json

from PySide6.QtCore import QTimer
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import QApplication, QWidget


class TestWindow(QWidget):
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(0, 0, self.width() // 2, self.height(), QColor("red"))
        painter.fillRect(self.width() // 2, 0, self.width(), self.height(), QColor("blue"))


app = QApplication([])
window = TestWindow()
window.setWindowTitle("YouCam native capture fixture")
window.setGeometry(100, 100, 320, 240)
window.show()
QTimer.singleShot(400, lambda: print(json.dumps({"hwnd": int(window.winId())}), flush=True))
raise SystemExit(app.exec())
