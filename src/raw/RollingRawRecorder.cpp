#include "cambuffer_recorder_ng/raw/RollingRawRecorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace cambuffer_recorder_ng
{

bool RollingRawRecorder::start(std::shared_ptr<ICamera> camera,
                               const CameraSettings& settings,
                               const std::string& path_prefix)
{
    if (running_) return false;
    if (!camera) return false;

    camera_ = std::move(camera);
    settings_ = settings;
    target_fps_ = settings_.getOr<double>("camera.fps", 5.0);
    max_frames_ = static_cast<uint64_t>(settings_.getOr<int64_t>("rolling.max_frames", int64_t{0}));
    pack_rows_ = settings_.getOr<bool>("rolling.pack_rows", true);

    const uint32_t width = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.width", int64_t{0}));
    const uint32_t height = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.height", int64_t{0}));
    const uint32_t source_stride = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.stride_bytes", int64_t{width}));

    uint64_t roll_bytes = static_cast<uint64_t>(settings_.getOr<int64_t>("rolling.max_file_bytes", int64_t{0}));
    if (roll_bytes == 0) {
        const double gib = settings_.getOr<double>("rolling.max_file_gib", 2.0);
        roll_bytes = static_cast<uint64_t>(gib * 1024.0 * 1024.0 * 1024.0);
    }

    RawRollingWriterConfig cfg;
    cfg.path_prefix = path_prefix;
    cfg.roll_bytes = roll_bytes;
    cfg.width = width;
    cfg.height = height;
    cfg.source_stride_bytes = source_stride;
    cfg.pixel_format = RawPixelFormat::RAW8_BAYER_GBRG;
    cfg.run_start_utc_ns = systemUtcNowNs();

    if (!writer_.open(cfg)) return false;

    frames_written_ = 0;
    running_ = true;
    worker_ = std::thread(&RollingRawRecorder::loop, this);
    return true;
}

void RollingRawRecorder::stop()
{
    running_ = false;
    if (worker_.joinable()) worker_.join();
    writer_.close();
}

void RollingRawRecorder::packRows(const uint8_t* src,
                                  int width,
                                  int height,
                                  int source_stride,
                                  std::vector<uint8_t>& packed)
{
    packed.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        std::memcpy(packed.data() + static_cast<size_t>(y) * static_cast<size_t>(width),
                    src + static_cast<size_t>(y) * static_cast<size_t>(source_stride),
                    static_cast<size_t>(width));
    }
}

void RollingRawRecorder::loop()
{
    std::vector<uint8_t> packed;
    uint8_t* data = nullptr;
    size_t size = 0;
    uint64_t camera_ts = 0;
    int w = 0, h = 0, stride = 0;

    const auto frame_period = target_fps_ > 0.0
        ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(1.0 / target_fps_))
        : std::chrono::steady_clock::duration::zero();
    auto next_frame_time = std::chrono::steady_clock::now();

    while (running_) {
        if (max_frames_ > 0 && frames_written_ >= max_frames_) {
            running_ = false;
            break;
        }

        if (frame_period.count() > 0) {
            std::this_thread::sleep_until(next_frame_time);
            next_frame_time += frame_period;
            const auto now = std::chrono::steady_clock::now();
            if (next_frame_time < now - frame_period) {
                next_frame_time = now + frame_period;
            }
        }

        bool ok = false;
        try {
            ok = camera_->grab(data, size, camera_ts, w, h, stride, 1000);
        } catch (...) {
            ok = false;
        }
        if (!ok || !data || w <= 0 || h <= 0) continue;

        const uint64_t utc_ns = systemUtcNowNs();
        const uint8_t* payload = data;
        uint32_t payload_bytes = static_cast<uint32_t>(w * h);

        if (pack_rows_ || stride != w) {
            packRows(data, w, h, stride, packed);
            payload = packed.data();
            payload_bytes = static_cast<uint32_t>(packed.size());
        } else {
            payload_bytes = static_cast<uint32_t>(std::min<size_t>(size, static_cast<size_t>(w) * static_cast<size_t>(h)));
        }

        if (!writer_.writeFrame(frames_written_, utc_ns, camera_ts, payload, payload_bytes)) {
            std::cerr << "RollingRawRecorder: writeFrame failed\n";
            running_ = false;
            break;
        }
        ++frames_written_;
    }
}

}  // namespace cambuffer_recorder_ng
