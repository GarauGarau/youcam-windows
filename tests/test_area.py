from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from PySide6.QtCore import QRect, QSettings

from youcam.core import load_area, save_area


class FakeScreen:
    def __init__(self, name: str, geometry: QRect):
        self._name = name
        self._geometry = geometry

    def name(self) -> str:
        return self._name

    def geometry(self) -> QRect:
        return QRect(self._geometry)


class AreaPreferenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.settings = QSettings(str(Path(self.directory.name) / "settings.ini"), QSettings.Format.IniFormat)
        self.screen = FakeScreen("Display-1", QRect(0, 0, 1920, 1080))

    def tearDown(self):
        self.directory.cleanup()

    def test_region_round_trip(self):
        rect = QRect(100, 120, 800, 600)
        save_area(self.settings, "region", "Display-1", rect)
        self.assertEqual(load_area(self.settings, [self.screen]), ("region", "Display-1", rect))

    def test_disconnected_display_falls_back_to_whole_screen(self):
        save_area(self.settings, "display", "Disconnected", QRect())
        self.assertEqual(load_area(self.settings, [self.screen])[0], "whole")

    def test_invalid_or_outside_region_falls_back(self):
        for rect in (QRect(10, 10, 20, 100), QRect(1800, 1000, 800, 600)):
            save_area(self.settings, "region", "Display-1", rect)
            self.assertEqual(load_area(self.settings, [self.screen])[0], "whole")

    def test_connected_display_round_trip(self):
        save_area(self.settings, "display", "Display-1", QRect())
        self.assertEqual(load_area(self.settings, [self.screen]), ("display", "Display-1", QRect()))

    def test_camera_source_is_restored_independently_of_displays(self):
        save_area(self.settings, "camera", "Disconnected", QRect())
        self.assertEqual(load_area(self.settings, []), ("camera", "", QRect()))

    def test_window_source_is_not_restored_with_a_stale_window_id(self):
        save_area(self.settings, "window", "Display-1", QRect())
        self.assertEqual(load_area(self.settings, [self.screen])[0], "whole")


if __name__ == "__main__":
    unittest.main()
