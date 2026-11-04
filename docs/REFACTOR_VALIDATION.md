# YouCam 2.1.1: conservative recording refactor

Local validation on Windows x64, 2026-10-31. Reference: `5c3ef9a`
(YouCam 2.1.0, including native microphone capture and lossless WAV).

## Changes

- Reuse the CPU NV12 conversion buffer between frames. The encoder still
  receives its own Media Foundation buffer and sample; no mutable storage is
  shared with an encoder that may consume samples asynchronously.
- Copy whole BGRA rows when fitting without mirroring or scaling. Preserve
  the original rule that duplicates odd edge pixels instead of cropping them.
  Compute the source row once per destination row when scaling.
- Use one temporary transformed frame for composition, crop and fitting,
  replacing three temporaries and the final four-way ownership selection.
  Unmodified frames still share their immutable captured image.
- Use one pixel-conversion path for the camera's 2D and contiguous buffers.
  Keep the existing locks, validation, RGB row origins, signed strides, NV12
  color range and timestamp handling.
- Calculate terrain values that are constant for a row/frame outside the
  inner loop. Contour generation, draw order and animation rate are unchanged.

No capture resolution, codec selection, bitrate, 30 fps scheduling, recording
clock, pause semantics, UI command, saved setting or file naming change.
Microphone capture and WAV writer source files are unchanged. No new dependency.

## Equivalence and functional checks

An optional `reference_checks` executable compiles the original snapshot in a
separate namespace beside the current code. It compares exact bytes, dimensions
and capture timestamps; it does not compare two calls to the current algorithm.

**469,042,344 identical bytes** across fitting, padding, mirror, scaling, NV12
conversion and terrain rendering. Cases include 1-pixel images, odd dimensions,
1080p/4K, different pixel patterns, repeated/reallocated conversion buffers,
and terrain at 0, 0.1, 8 and 1234.5 seconds.

All **8 CTest suites** passed on the final Release build:

| Suite | Coverage |
|---|---|
| core | Resolution selection, 8K extents, padding, mirror, color conversion, timeline |
| visuals | Composition, shadows, alpha, crop, camera placement and stage |
| interactions | Application commands and window interactions |
| audio | Exact float precision, headroom, channel separation, delay/gaps, no overwrite and AAC compatibility at 44.1/48/96 kHz |
| sync | Decode MP4 and WAV; CFR, delayed/irregular frames, audio/video markers and pause/resume |
| native | Real Windows capture, 4K encoding/decoding, region/window capture, resizing, occlusion, overlay, minimize/resume and camera loss |
| ui | Standalone executable startup/shutdown |
| reference | Exact comparison described above |

Hardware check: two consecutive switches through both connected cameras
(OBSBOT Meet 2 and Integrated Camera), with live frames received after each
switch and successful shutdown. No camera images were saved by this check.
The DJI microphone was not enumerated during this session; its physical capture
could not be retested. The audio pipeline is unchanged and passed the synthetic
precision, compatibility and synchronization suites.

The initial, unmodified baseline had one transient native-test failure in the
window-composition color assertion. The isolated rerun passed; the final full
suite also passed without changing or relaxing that assertion.

## Performance

Same Release compiler settings, same machine, five alternating before/after
trials. Median milliseconds per operation; allocation/reuse is included.

| Operation | Reference | 2.1.1 | Reduction in time |
|---|---:|---:|---:|
| 4K BGRA → NV12 | 17.341 ms | 15.795 ms | 8.9% |
| 4K row copy + odd-edge padding | 7.918 ms | 5.482 ms | 30.8% |
| 4K terrain | 25.480 ms | 24.987 ms | Essentially unchanged |

These are component measurements, not claims about total CPU usage or recording
speed. The NV12 scratch buffer retains one frame's storage during recording
(about 12.4 MB at 4K); this avoids repeated allocation and initialization. It is
owned by the recording's `Movie` and released when that object is destroyed.

## Reproduce

From a Windows Developer Command Prompt with CMake available:

```powershell
New-Item -ItemType Directory -Path build/reference -Force
git archive 5c3ef9a --format=zip --output=build/reference/source.zip src
Expand-Archive -LiteralPath build/reference/source.zip -DestinationPath build/reference/source
$reference = (Resolve-Path build/reference/source).Path.Replace('\', '/')
cmake -S . -B build -A x64 "-DYOUCAM_REFERENCE_SOURCE=$reference"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
build/Release/reference_checks.exe --benchmark
```

Set `-DYOUCAM_REFERENCE_SOURCE=` to return to the normal build. The snapshot,
test media and benchmarks are not included in the distributed executable.
