#include "cambuffer_recorder_ng/FakeCamera.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <thread>

namespace cambuffer_recorder_ng
{

namespace
{
std::string normalizeToken(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        if (c == '-' || c == ' ' || c == '.') return '_';
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

uint8_t clampU8(int v)
{
    return static_cast<uint8_t>(std::max(0, std::min(255, v)));
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
}

std::string FakeCamera::canonicalPixelFormat(std::string pixel_format)
{
    pixel_format = normalizeToken(std::move(pixel_format));

    if (pixel_format.empty() || pixel_format == "rgb" || pixel_format == "rgb8") {
        return "rgb24";
    }
    if (pixel_format == "rgb24" || pixel_format == "bgr24") {
        return pixel_format;
    }
    if (pixel_format == "mono" || pixel_format == "mono8" || pixel_format == "gray8" ||
        pixel_format == "grey8" || pixel_format == "raw8") {
        return "mono8";
    }
    if (pixel_format == "bayer_gbrg8" || pixel_format == "gbrg8" ||
        pixel_format == "raw8_bayer_gbrg" || pixel_format == "bayer8_gbrg") {
        return "bayer_gbrg8";
    }

    throw std::runtime_error("FakeCamera unsupported camera.pixel_format: " + pixel_format +
                             " (supported: rgb24, mono8/raw8, bayer_gbrg8)");
}

FakeCamera::FakePixelFormat FakeCamera::parsePixelFormat(const std::string& pixel_format)
{
    if (pixel_format == "rgb24" || pixel_format == "bgr24") return FakePixelFormat::RGB24;
    if (pixel_format == "mono8") return FakePixelFormat::MONO8;
    if (pixel_format == "bayer_gbrg8") return FakePixelFormat::BAYER_GBRG8;
    throw std::runtime_error("FakeCamera unsupported canonical pixel format: " + pixel_format);
}

int FakeCamera::bytesPerPixel(FakePixelFormat pixel_format)
{
    switch (pixel_format) {
        case FakePixelFormat::RGB24: return 3;
        case FakePixelFormat::MONO8: return 1;
        case FakePixelFormat::BAYER_GBRG8: return 1;
    }
    return 1;
}

void FakeCamera::configure(const CameraSettings& requested_settings)
{
    requested_settings_ = requested_settings;
    width_ = static_cast<int>(requested_settings.getOr<int64_t>("camera.width", width_));
    height_ = static_cast<int>(requested_settings.getOr<int64_t>("camera.height", height_));
    fps_ = static_cast<int>(requested_settings.getOr<double>("camera.fps", static_cast<double>(fps_)));

    pixel_format_name_ = canonicalPixelFormat(requested_settings.getOr<std::string>("camera.pixel_format", "rgb24"));
    pixel_format_ = parsePixelFormat(pixel_format_name_);
    bytes_per_pixel_ = bytesPerPixel(pixel_format_);

    if (width_ <= 0 || height_ <= 0 || fps_ <= 0) {
        throw std::runtime_error("FakeCamera requires positive width, height, and fps");
    }

    resizeBuffer();
    effective_settings_ = requested_settings_;
    effective_settings_.set("backend", std::string{"fake"});
    effective_settings_.set("camera.width", int64_t{width_});
    effective_settings_.set("camera.height", int64_t{height_});
    effective_settings_.set("camera.fps", static_cast<double>(fps_));
    effective_settings_.set("camera.pixel_format", pixel_format_name_);
    effective_settings_.set("camera.bytes_per_pixel", int64_t{bytes_per_pixel_});
    effective_settings_.set("camera.stride_bytes", int64_t{stride_bytes_});
    if (pixel_format_ == FakePixelFormat::BAYER_GBRG8) {
        effective_settings_.set("camera.bayer_pattern", std::string{"GBRG"});
    }
}

void FakeCamera::open(int /*device_index*/)
{
    resizeBuffer();
    frame_counter_ = 0;
    opened_ = true;
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
    const auto frame_period_ns = std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_period);

    const auto now = std::chrono::steady_clock::now();
    if (next_frame_time_ > now) {
        std::this_thread::sleep_until(next_frame_time_);
    }
    next_frame_time_ += frame_period_ns;

    // If the recorder fell behind, do not spend ages trying to catch up.
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
    stride = stride_bytes_;

    ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    return true;
}

void FakeCamera::resizeBuffer()
{
    stride_bytes_ = width_ * bytes_per_pixel_;
    const auto bytes = static_cast<size_t>(stride_bytes_) * static_cast<size_t>(height_);
    buffer_.assign(bytes, 0);
}

void FakeCamera::generateFrame(uint64_t frame_index)
{
    switch (pixel_format_) {
        case FakePixelFormat::RGB24:
            generateRgb24(frame_index);
            break;
        case FakePixelFormat::MONO8:
            generateMono8(frame_index);
            break;
        case FakePixelFormat::BAYER_GBRG8:
            generateBayerGbrg8(frame_index);
            break;
    }
    drawTimingBar(frame_index);
}

void FakeCamera::generateRgb24(uint64_t frame_index)
{
    const int phase = static_cast<int>(frame_index % 256);

    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const size_t idx = static_cast<size_t>(y * stride_bytes_) + static_cast<size_t>(x) * 3;
            buffer_[idx + 0] = static_cast<uint8_t>((x + phase) & 0xff);
            buffer_[idx + 1] = static_cast<uint8_t>((y + phase * 2) & 0xff);
            buffer_[idx + 2] = static_cast<uint8_t>(((x / 2) + (y / 2) + phase * 3) & 0xff);
        }
    }
}

void FakeCamera::generateMono8(uint64_t frame_index)
{
    const int phase = static_cast<int>(frame_index % 256);

    for (int y = 0; y < height_; ++y) {
        uint8_t* row = buffer_.data() + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes_);
        for (int x = 0; x < width_; ++x) {
            row[x] = static_cast<uint8_t>((x / 2 + y / 3 + phase * 3) & 0xff);
        }
    }
}

void FakeCamera::generateBayerGbrg8(uint64_t frame_index)
{
    const int phase = static_cast<int>(frame_index % 256);

    // Build an imaginary RGB scene, then sample it through a GBRG Bayer mosaic:
    // even row:  G B G B ...
    // odd row:   R G R G ...
    for (int y = 0; y < height_; ++y) {
        uint8_t* row = buffer_.data() + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes_);
        for (int x = 0; x < width_; ++x) {
            const int r = (x + phase * 2) & 0xff;
            const int g = (y + phase * 3) & 0xff;
            const int b = ((x / 2) + (y / 2) + phase * 5) & 0xff;

            const bool even_y = (y & 1) == 0;
            const bool even_x = (x & 1) == 0;

            if (even_y) {
                row[x] = even_x ? clampU8(g) : clampU8(b);  // G B
            } else {
                row[x] = even_x ? clampU8(r) : clampU8(g);  // R G
            }
        }
    }
}

void FakeCamera::drawTimingBar(uint64_t frame_index)
{
    const int bar_x = static_cast<int>((frame_index * 7) % static_cast<uint64_t>(std::max(width_, 1)));
    const int bar_width = std::max(2, width_ / 80);
    const int x0 = std::max(0, bar_x - bar_width / 2);
    const int x1 = std::min(width_, x0 + bar_width);

    for (int y = 0; y < height_; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(stride_bytes_) +
                               static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel_);
            if (pixel_format_ == FakePixelFormat::RGB24) {
                buffer_[idx + 0] = 255;
                buffer_[idx + 1] = 255;
                buffer_[idx + 2] = 255;
            } else {
                buffer_[idx] = 255;
            }
        }
    }
}

}  // namespace cambuffer_recorder_ng
