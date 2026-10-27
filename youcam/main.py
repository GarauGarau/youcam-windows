from __future__ import annotations

import argparse
import os
import sys

from PySide6.QtWidgets import QApplication


def main() -> None:
    parser = argparse.ArgumentParser(prog="youcam")
    parser.add_argument("--smoke-test", action="store_true")
    parser.add_argument("--preview", action="store_true")
    args = parser.parse_args()
    if sys.platform != "win32" and not (args.smoke_test or args.preview):
        parser.error("This port requires Windows 11. Use the original Linux port on Linux.")
    # QCamera uses Qt's bundled FFmpeg backend; this is independent of the
    # command-line FFmpeg used to encode the final movie.
    os.environ.setdefault("QT_MEDIA_BACKEND", "ffmpeg")
    app = QApplication(sys.argv[:1])
    app.setApplicationName("YouCam")
    app.setOrganizationName("YouCam")
    app.setDesktopFileName("youcam")
    app.setQuitOnLastWindowClosed(False)
    if args.smoke_test:
        from .smoke import run_smoke_test
        raise SystemExit(run_smoke_test(app))
    from .app import YouCam
    youcam = YouCam(preview=args.preview)
    app.youcam = youcam
    raise SystemExit(app.exec())
