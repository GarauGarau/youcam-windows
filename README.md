# YouCam for Windows

Fork of [antirez/youcam](https://github.com/antirez/youcam), adapted for **Windows 11 x64**. Floating webcam and screen recorder. [MIT license](LICENSE).

## Download

[**Download YouCam.exe**](https://github.com/GarauGarau/youcam-windows/releases/latest/download/YouCam.exe) and run it. No installation needed.

## Changes and additions

- Rewritten in C++ using native Windows APIs.
- Record a screen, an area, a window with an optional webcam overlay, or the webcam alone.
- Full-resolution capture and MP4 recording at a constant 30 fps.
- Webcam cropping, rounded corners and shadows.
- Full-screen camera stage with an animated background.
- Microphone selection, volume control and pause/resume.

## Use

Choose your webcam from **Camera…**. Your choice is remembered; no brand is preferred.

Drag to move, scroll to resize, double-click to crop. Right-click for settings. Choose **Recording source** and **Audio input**, then record. Videos are saved to your Desktop. System audio is not recorded.

- **F11 / Esc:** open / close the camera stage.
- **Ctrl+Alt+Shift+R:** start / stop recording.
- **Ctrl+Alt+Shift+P:** pause / resume.

## Build

Install Microsoft C++ Build Tools with the Windows SDK and CMake. From a Developer Command Prompt:

```bat
build.cmd
test.cmd
```

Output: `build\Release\YouCam.exe`. Windows N may need the Media Feature Pack.
