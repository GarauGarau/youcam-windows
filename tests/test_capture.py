from __future__ import annotations

import unittest

import numpy as np
from PySide6.QtCore import QRect

from youcam.capture import ExclusionManager, _subtract


class ExclusionTests(unittest.TestCase):
    def test_subtract_preserves_protected_camera_region(self):
        control = QRect(0, 0, 100, 100)
        camera = QRect(25, 25, 50, 50)
        regions = _subtract(control, camera)
        self.assertEqual(sum(region.width() * region.height() for region in regions), 7500)
        self.assertFalse(any(region.intersects(camera) for region in regions))

    def test_mask_restores_background_only_inside_target(self):
        manager = ExclusionManager()
        background = np.full((50, 50, 4), 17, dtype=np.uint8)
        manager._entries[1] = (None, QRect(10, 10, 50, 50), background)
        frame = np.full((100, 100, 4), 200, dtype=np.uint8)
        manager.mask(frame, QRect(0, 0, 100, 100), protected=QRect(30, 30, 10, 10))
        self.assertTrue(np.all(frame[10, 10] == 17))
        self.assertTrue(np.all(frame[35, 35] == 200))
        self.assertTrue(np.all(frame[5, 5] == 200))


if __name__ == "__main__":
    unittest.main()
