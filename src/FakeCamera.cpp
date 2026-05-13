#include "cambuffer_recorder_ng/FakeCamera.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace cambuffer_recorder_ng
{

namespace
{

int positiveIntOrThrow(int64_t value, const std::string& name)
{
    if (value <= 0) {
        throw std::runtime_error("FakeCamera setting '" + name + "' must be positive");
    }
    return static_cast<int>(value);
}

}  // namespace

FakeCamera::FakeCamera(int width, int height, int fps)
    : width_(width), height_(height), fps_(fps)
{
    if (width_ <= 0 || height_ <= 0) {
        throw std::runtime_error("FakeCamera requires positive width and height");
    }
    if (fps_ <= 0) {
        throw std::runtime_error("FakeCamera requires positive fps");
    }

    resizeBuffer();
    updateEffectiveSettings();
}

void FakeCamera::configure(const CameraSettings& requested_settings)
{
    requested_settings_ = requested_settings;

    width_ = positiveIntOrThrow(requested_settings.getOr<int64_t>("width", width_), "width");
    height_ = positiveIntOrThrow(requested_settings.getOr<int64_t>("height", height_), "height");
    fps_ = positiveIntOrThrow(requested_settings.getOr<int64_t>("fps", fps_), "fps");

    resizeBuffer();
    updateEffectiveSettings();
}

void FakeCamera::open(int /*device_index*/)
{
    resizeBuffer();
    frame_counter_ = 0;
    opened_ = true;
    updateEffectiveSettings();
}

void FakeCamera::start()
{
    if (!opened_) {
        open(0);
    }

    running_ = true;
    next_frame_time_ = std::chrono::steady_clock::now();
}

void FakeCamera::stop()
{
    running_ = false;
}

void FakeCamera::close()
{
    stop();
    opened_ = false;
}

bool FakeCamera::grab(uint8_t*& data,
                      size_t& size,
                      uint64_t& ts,
                      int& width,
                      int& height,
                      int& stride,
                      int /*timeout_ms*/)
{
    if (!running_) {
        return false;
    }

    const auto frame_period = std::chrono::duration<double>(1.0 / static_cast<double>(fps_));
    const auto frame_period_ns =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_period);

    const auto now = std::chrono::steady_clock::now();
    if (next_frame_time_ > now) {
        std::this_thread::sleep_until(next_frame_time_);
    }
    next_frame_time_ += frame_period_ns;

    const auto after_sleep = std::chrono::steady_clock::now();
    if (next_frame_time_ < after_sleep - frame_period_ns) {
        next_frame_time_ = after_sleep + frame_period_ns;
    }

    ++frame_counter_;
    generateFrame(frame_counter_);

    data = buffer_.data();
    size = buffer_.size();
    width = width_;
    height = height_;
    stride = width_ * channels_;

    ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    return true;
}

void FakeCamera::resizeBuffer()
{
    const auto bytes = static_cast<size_t>(width_) *
                       static_cast<size_t>(height_) *
                       static_cast<size_t>(channels_);
    buffer_.assign(bytes, 0);
}

void FakeCamera::updateEffectiveSettings()
{
    effective_settings_ = requested_settings_;
    effective_settings_.set("backend", std::string{"fake"});
    effective_settings_.set("width", static_cast<int64_t>(width_));
    effective_settings_.set("height", static_cast<int64_t>(height_));
    effective_settings_.set("fps", static_cast<int64_t>(fps_));
    effective_settings_.set("frame_rate_hz", static_cast<double>(fps_));
    effective_settings_.set("pixel_format", std::string{"rgb24"});
    effective_settings_.set("stride_bytes", static_cast<int64_t>(width_ * channels_));
    effective_settings_.set("frame_size_bytes", static_cast<int64_t>(buffer_.size()));
}

void FakeCamera::generateFrame(uint64_t frame_index)
{
    const int phase = static_cast<int>(frame_index % 256);

    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const size_t idx = static_cast<size_t>(y * width_ + x) * channels_;

            const uint8_t r = static_cast<uint8_t>((x + phase) & 0xff);
            const uint8_t g = static_cast<uint8_t>((y + phase * 2) & 0xff);
            const uint8_t b = static_cast<uint8_t>(((x / 2) + (y / 2) + phase * 3) & 0xff);

            buffer_[idx + 0] = r;
            buffer_[idx + 1] = g;
            buffer_[idx + 2] = b;
        }
    }

    const int bar_x = static_cast<int>((frame_index * 7) % static_cast<uint64_t>(std::max(width_, 1)));
    const int bar_width = std::max(2, width_ / 80);
    const int x0 = std::max(0, bar_x - bar_width / 2);
    const int x1 = std::min(width_, x0 + bar_width);

    for (int y = 0; y < height_; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t idx = static_cast<size_t>(y * width_ + x) * channels_;
            buffer_[idx + 0] = 255;
            buffer_[idx + 1] = 255;
            buffer_[idx + 2] = 255;
        }
    }
}

}  // namespace cambuffer_recorder_ng
