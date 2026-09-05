# AGENTS.md — cambuffer_recorder_ng

*Drafted from reading the 2026-08-25 source snapshot. Follows the lab's
architecture-repo convention: Confirmed / Current design / Planned /
Needs verification. Review and correct before trusting fully.*

## Role

High-speed camera acquisition, buffering, and recording (ROS2, C++,
`ament_cmake`). Owns camera backend interfaces (XIMEA, FakeCamera, GenTL),
raw rolling/RAM-buffer recording formats, and storage-space protection.
Does not own orchestration, GUI workflow, trigger pulse generation, or
treadmill control — see `camera_control`, `triggerbox_ros2`,
`treadmill_control`.

## Build

```bash
cd ~/ros2_ws
colcon build --packages-select cambuffer_recorder_ng
source install/setup.bash
```
Depends on `rclcpp`, `rclcpp_lifecycle`, OpenCV, FFmpeg (system, via
CMake `find_package`/pkg-config, not declared as a rosdep). **Confirmed:
no `test/` directory and no `ament_add_gtest` anywhere in this package —
there is no automated test target today.**

## Recording modes — three separate, mutually exclusive recorder classes

`CamBufferRecorderNode::startRecording()` constructs exactly one of:
- `RollingRawRecorder` — the `_rolling` modes.
- `RamCircularRawRecorder` (`src/raw/RamCircularRawRecorder.cpp`) — the
  `_ram_buffer` modes. This is the one relevant to the ping-pong feature.
- `Recorder` (wraps `FfmpegWriter`) — generic `video_mp4`/`video_rgb24`
  fallback. **Confirmed: `Recorder` does not wrap or relate to
  `RamCircularRawRecorder` at all** — they're siblings, not layers.

`BufferPool` (`include/.../BufferPool.hpp`, `src/BufferPool.cpp`) is a
fully-implemented generic buffer pool that is **compiled but unused** —
none of the three recorder classes reference it. Don't assume it backs
anything; it's dead code as of this snapshot.

## RAM-buffer mode specifics (relevant to any ping-pong work)

- Ring: `std::vector<FrameSlot> ring_` in `RamCircularRawRecorder`.
  Capacity is config-driven (`ram_buffer.capacity_frames`), **1500** in
  every shipped `*_ram_buffer*.yaml` as of this snapshot — not 384. (384
  is `ximea.buffers_queue_size`, a separate, smaller XIMEA SDK transport
  buffer — don't conflate the two.) **Decided, pending implementation:**
  dropping to 1000 (both rings, symmetric) as part of the ping-pong
  feature — see that spec and `capacity_frames_1000.patch`. 1500 was a
  relic from before hardware-triggered synchronized camera stop was in
  place.
- `dump()` today always calls `pauseCapture()` (real `camera_->stop()`)
  before writing and `resumeCapture()` (`camera_->start()`) after —
  confirmed by reading `RamCircularRawRecorder.cpp:568-742`.
- `dump_policy_` currently only accepts `"pause_acquisition"`; a
  `"continue_acquisition"` token is already recognized by
  `canonicalDumpPolicy()` but unimplemented — this is the natural name
  for a future ping-pong/non-stopping mode.
- Dump interface: ROS2 service `~/dump_buffer`
  (`cambuffer_recorder_ng::srv::DumpBuffer`), handled by
  `CamBufferRecorderNode::handleDumpBuffer()`. No server-side guard
  exists against overlapping `dump()` calls — the GUI enforces
  "one at a time" client-side only.
- `GetStatus.srv` does **not** currently expose any RAM-buffer telemetry
  (fill level, capacity, dropped frames) — needs-verification-before-use
  for anyone assuming otherwise.

## Known open issues (from the repo's own notes, not invented)

- `CMakeLists.txt`'s `install(DIRECTORY ... msg action srv ...)` still
  lists `msg`/`action` (harmless, `OPTIONAL`, but another fossil of the
  same stale layout).

## Testing-level discipline (see lab-instrumentation-architecture TESTING.md)

Default to Level 1 (build) and Level 2 (FakeCamera / software-only).
Level 3+ (real XIMEA hardware) requires a human physically present,
except on `ros2test` per ADR 0007 where Level 3 is fine unattended since
it's isolated from the live rig. Never restart/reconfigure a live
recording node without explicit confirmation in-session.
