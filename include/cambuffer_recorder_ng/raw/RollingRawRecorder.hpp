#pragma once

#include "cambuffer_recorder_ng/ICamera.hpp"
#include "cambuffer_recorder_ng/raw/RawRollingWriter.hpp"
#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cambuffer_recorder_ng
{

struct RollingAcquisitionDiagnostics
{
    uint64_t frames_written{0};
    uint64_t camera_frame_gaps{0};
    uint64_t camera_frame_nonmonotonic{0};
    double max_write_frame_ms{0.0};
    uint64_t writes_over_10ms{0};
    uint64_t writes_over_50ms{0};
    uint64_t writes_over_100ms{0};
    uint64_t writes_over_500ms{0};
    int worker_sched_policy{-1};
    int worker_sched_priority{-1};
    int64_t rlimit_rtprio_soft{-1};
    int64_t rlimit_rtprio_hard{-1};
};

class RollingRawRecorder
{
public:
    RollingRawRecorder() = default;
    ~RollingRawRecorder() { stop(); }

    bool start(std::shared_ptr<ICamera> camera,
               const CameraSettings& settings,
               const std::string& path_prefix);
    void stop();

    using EventCallback = std::function<void(const std::string& event_type, bool success, const std::string& message)>;
    void setEventCallback(EventCallback callback) { event_callback_ = std::move(callback); }

    // Called immediately before a rolling-file rollover. Return false to stop
    // capture cleanly before the next file is opened.
    using RolloverCallback = std::function<bool(uint32_t next_file_index)>;
    void setRolloverCallback(RolloverCallback callback) { rollover_callback_ = std::move(callback); }

    uint64_t framesWritten() const { return frames_written_; }
    const std::vector<std::string>& filesWritten() const { return writer_.filesWritten(); }
    RollingAcquisitionDiagnostics diagnostics() const { return diagnostics_; }

private:
    void loop();
    static void packRows(const uint8_t* src,
                         int width,
                         int height,
                         int source_stride,
                         std::vector<uint8_t>& packed);

    std::shared_ptr<ICamera> camera_;
    CameraSettings settings_;
    RawRollingWriter writer_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_written_{0};

    double target_fps_{5.0};
    uint64_t max_frames_{0};
    bool pack_rows_{true};
    bool hardware_trigger_{false};
    double expected_hardware_fps_{0.0};
    int grab_timeout_ms_{1000};
    double timeout_warn_interval_s_{5.0};
    double fps_report_interval_s_{5.0};
    EventCallback event_callback_;
    RolloverCallback rollover_callback_;
    RollingAcquisitionDiagnostics diagnostics_;
};

}  // namespace cambuffer_recorder_ng
