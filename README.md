# YouCam for Windows

Fork of [antirez/youcam](https://github.com/antirez/youcam), adapted for **Windows 11 x64**. Floating webcam and screen recorder. [MIT license](LICENSE).

## Download

[**Download YouCam.exe**](https://github.com/GarauGarau/youcam-windows/releases/latest/download/YouCam.exe) and run it. No installation needed.

## Changes and additions

- Rewritten in C++ using native Windows APIs.
- Record a screen, an area, a window with an optional webcam overlay, or the webcam alone.
- Vertical camera recording with a plain portrait preview and optional frame crop.
- Full-resolution capture and MP4 recording at a constant 30 fps.
- Webcam cropping, rounded corners and shadows.
- Full-screen camera stage with an animated background.
- Microphone selection, volume control and pause/resume.
- Lossless float32 WAV audio alongside each MP4, at the microphone's native sample rate and channel count.

## Use

Choose your webcam from **Camera…**. Your choice is remembered; no brand is preferred.

Drag to move, scroll to resize, double-click to crop. Right-click for settings. Choose **Recording source** and **Audio input**, then record. Videos are saved to your Desktop. System audio is not recorded.

- **F11 / Esc:** open / close the camera stage.
- **Ctrl+Alt+Shift+R:** start / stop recording.
- **Ctrl+Alt+Shift+P:** pause / resume.

For a physically rotated camera such as an OBSBOT, choose **Recording source → Vertical camera (video + audio + WAV)**.
Turn the camera before starting: a landscape stream is rotated 90° clockwise at full resolution
(for example, 3840 × 2160 becomes 2160 × 3840); a native portrait stream keeps its orientation.
If the picture is upside down, right-click and choose **Flip vertical camera 180°**.
Double-click to crop the upright frame, then **Apply crop**; **Full frame** restores it.
The preview follows the crop's proportions. This mode records only the camera pixels, with no
mirroring, shapes, borders or background. Its crop is remembered separately from the other camera modes.
Select your microphone in **Audio input** to save an MP4 with audio and the matching uncompressed
float32 WAV on the Desktop. **No audio** saves only the video. Rotate and crop before recording;
if the camera changes stream dimensions during recording, the current video is saved and a new recording is needed.

## Audio for editing

With a microphone enabled, recording creates two files with the same name on your Desktop:

- **MP4**: video with AAC audio at 192 kb/s for playback.
- **WAV**: the lossless audio master for Fast Audio or your editor. Align it with the start of the video, and mute the MP4's audio to avoid doubling it. Start delays, pauses and the final duration are handled automatically.

Capture uses the endpoint's native format and requests WASAPI RAW mode to bypass optional Windows speech effects. If RAW is unavailable, the endpoint's normal shared mode is used. All input channels remain separate in the WAV; MP4 audio is converted to mono/stereo and 44.1/48 kHz as needed for AAC compatibility. No denoising, EQ, compressor or gate is applied by YouCam.

The WAV stores 32-bit floating point samples, preserving the microphone's available precision and software gain headroom. This does not turn a 16/24-bit microphone into a 32-bit ADC or repair clipping in the transmitter. At 48 kHz stereo it uses about 1.38 GB/hour; recordings beyond 4 GB use RF64. Keep the microphone gain low enough to avoid hardware overload and do voice processing in Fast Audio.

For a DJI Mic Mini, use its USB receiver. The connected receiver tested at 48 kHz / 24-bit stereo; RAW capture exposed 48 kHz stereo float and retained detail below the 16-bit level. Receiver noise cancellation and gain settings remain hardware settings; YouCam does not change them.

## Build

Install Microsoft C++ Build Tools with the Windows SDK and CMake. From a Developer Command Prompt:

```bat
build.cmd
test.cmd
```

Output: `build\Release\YouCam.exe`. Windows N may need the Media Feature Pack.

`audio_tests` checks lossless precision, channels, gain headroom, timing and AAC compatibility. `sync_tests` decodes both the MP4 and WAV to check synchronization, including pause/resume. Hardware checks are opt-in: `build\Release\microphone_check.exe` probes connected DJI inputs; add `--capture path.wav` to save a three-second local test. It never uploads audio.

Version 2.1.1 reduces per-frame work while preserving recording behavior.
[Refactor validation and reproducible comparison](docs/REFACTOR_VALIDATION.md).
