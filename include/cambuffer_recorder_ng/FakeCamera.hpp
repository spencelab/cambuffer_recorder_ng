#pragma once

#include "cambuffer_recorder_ng/ICamera.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cambuffer_recorder_ng
{

/**
 * @brief Synthetic camera backend for testing the recorder without hardware.
 *
 * Modes set camera.pixel_format defaults. FakeCamera only obeys camera.* settings.
 * This keeps FakeCamera useful for both normal happy-path tests and intentional
 * pipeline mismatch tests.
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
    enum class FakePixelFormat
    {
        RGB24,
        MONO8,
        BAYER_GBRG8
    };

    static std::string canonicalPixelFormat(std::string pixel_format);
    static FakePixelFormat parsePixelFormat(const std::string& pixel_format);
    static int bytesPerPixel(FakePixelFormat pixel_format);

    void resizeBuffer();
    void generateFrame(uint64_t frame_index);
    void generateRgb24(uint64_t frame_index);
    void generateMono8(uint64_t frame_index);
    void generateBayerGbrg8(uint64_t frame_index);
    void drawTimingBar(uint64_t frame_index);

    int width_{640};
    int height_{480};
    int fps_{30};
    std::string pixel_format_name_{"rgb24"};
    FakePixelFormat pixel_format_{FakePixelFormat::RGB24};
    int bytes_per_pixel_{3};
    int stride_bytes_{640 * 3};
    bool realtime_pacing_{true};

    // Test-only fault injection: fake.fail_grab_every_n_attempts (>=1) makes
    // every Nth grab() attempt fail (return false without producing a frame),
    // simulating a real hardware-trigger grab timeout. 0 (default) disables
    // this and preserves prior behavior for every existing config. Not meant
    // for production configs -- added to reproduce, in software, the exact
    // grab-timeout-adjacent race that broke continue_acquisition RAM dumps on
    // real XIMEA hardware (see RamCircularRawRecorder.cpp's slot_had_valid_frame
    // handling and PINGPONG_GUI_PLAN.md session notes, 2026-08-28).
    int64_t fail_grab_every_n_attempts_{0};
    uint64_t grab_attempt_counter_{0};

    std::atomic<bool> opened_{false};
    std::atomic<bool> running_{false};

    uint64_t frame_counter_{0};
    std::vector<uint8_t> buffer_;

    std::chrono::steady_clock::time_point next_frame_time_;
};

}  // namespace cambuffer_recorder_ng
