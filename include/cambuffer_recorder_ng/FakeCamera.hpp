#pragma once

#include "cambuffer_recorder_ng/ICamera.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cambuffer_recorder_ng
{

/**
 * @brief Synthetic camera backend for testing the recorder without hardware.
 *
 * FakeCamera currently emits RGB24 frames because FfmpegWriter expects RGB24
 * input before converting to YUV420P. The output buffer is owned by FakeCamera
 * and remains valid until the next grab() call.
 */
class FakeCamera : public ICamera
{
public:
    FakeCamera(int width = 640, int height = 480, int fps = 30);
    ~FakeCamera() override = default;

    void configure(const CameraSettings& requested_settings) override;
    CameraSettings getEffectiveSettings() const override { return effective_settings_; }
    std::string backendName() const override { return "fake"; }

    void open(int device_index = 0) override;
    void start() override;
    void stop() override;
    void close() override;

    bool grab(uint8_t*& data,
              size_t& size,
              uint64_t& ts,
              int& width,
              int& height,
              int& stride,
              int timeout_ms = 100) override;

private:
    void resizeBuffer();
    void generateFrame(uint64_t frame_index);

    int width_{640};
    int height_{480};
    int fps_{30};

    static constexpr int channels_ = 3;  // RGB24

    std::atomic<bool> opened_{false};
    std::atomic<bool> running_{false};

    uint64_t frame_counter_{0};
    std::vector<uint8_t> buffer_;

    std::chrono::steady_clock::time_point next_frame_time_;
};

}  // namespace cambuffer_recorder_ng
