#pragma once
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <random>
#include "cambuffer_recorder_ng/ICamera.hpp"

namespace cambuffer_recorder_ng {

/**
 * @brief Simple fake camera that generates synthetic gradient frames.
 */
class FakeCamera : public ICamera {
public:
    FakeCamera(int width = 640, int height = 480, int fps = 30)
        : width_(width), height_(height), fps_(fps), frame_bytes_(width * height)
    {
        buffer_.resize(frame_bytes_);
    }

    void open(int device_index = 0) override {}
    void start() override { running_ = true; }
    void stop() override { running_ = false; }

    bool grab(uint8_t*& data, size_t& size, uint64_t& ts,
              int& width, int& height, int& stride, int timeout_ms = 100) override
    {
        if (!running_) return false;
        static uint64_t counter = 0;
        counter++;

        // Simple moving gradient pattern
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x)
                buffer_[y * width_ + x] = static_cast<uint8_t>((x + y + counter) % 255);
        }

        data = buffer_.data();
        size = buffer_.size();
        width = width_;
        height = height_;
        stride = width_;
        ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now().time_since_epoch()).count();

        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps_));
        return true;
    }

private:
    int width_, height_, fps_;
    size_t frame_bytes_;
    bool running_{false};
    std::vector<uint8_t> buffer_;
};

} // namespace cambuffer_recorder_ng

