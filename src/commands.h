#pragma once
namespace youcam::commands {
constexpr int Start = 100, Pause = 101, Panel = 102, HideCamera = 103, Mirror = 104, SelectArea = 105,
              WholeScreen = 106, CameraOnly = 107, Quit = 108, Retry = 109, Reveal = 110, Help = 111,
              SourceMenu = 112, AudioNone = 113, CameraMenu = 114, Stage = 115, CropCamera = 116,
              CropApply = 117, CropFull = 118, CropCancel = 119,
              WindowCamera = 130, StageLarger = 131, StageSmaller = 132,
              CameraPortrait = 133, PortraitReverse = 134,
              ShapeBase = 200, SizeBase = 210, CameraBase = 1000,
              AudioBase = 2000, MonitorBase = 3000, WindowBase = 4000;
}
