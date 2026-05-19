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

    uint64_t framesWritten() const { return frames_written_; }
    const std::vector<std::string>& filesWritten() const { return writer_.filesWritten(); }

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
};

}  // namespace cambuffer_recorder_ng
