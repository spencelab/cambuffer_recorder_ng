#include "cambuffer_recorder_ng/raw/RollingRawRecorder.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iostream>
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

bool isGbrgRaw8Like(const std::string& pixel_format, const std::string& bayer_pattern)
{
    const std::string pf = normalizeToken(pixel_format);
    const std::string bp = normalizeToken(bayer_pattern);

    if (pf == "bayer_gbrg8" || pf == "raw8_bayer_gbrg" || pf == "bayer8_gbrg") {
        return true;
    }

    // Legacy configs may still say raw8 plus a separate Bayer pattern.
    if ((pf == "raw8" || pf == "bayer8") && (bp == "gbrg" || bp.empty())) {
        return true;
    }

    return false;
}
}  // namespace

bool RollingRawRecorder::start(std::shared_ptr<ICamera> camera,
                               const CameraSettings& settings,
                               const std::string& path_prefix)
{
    if (running_) return false;
    if (!camera) return false;

    camera_ = std::move(camera);
    settings_ = settings;
    target_fps_ = settings_.getOr<double>("camera.fps", 5.0);
    hardware_trigger_ = settings_.getOr<bool>("camera.hardware_trigger", false);
    expected_hardware_fps_ = settings_.getOr<double>("camera.expected_hardware_fps", target_fps_);
    grab_timeout_ms_ = static_cast<int>(settings_.getOr<int64_t>("camera.grab_timeout_ms", int64_t{1000}));
    timeout_warn_interval_s_ = settings_.getOr<double>("camera.timeout_warn_interval_s", 5.0);
    fps_report_interval_s_ = settings_.getOr<double>("camera.fps_report_interval_s", 5.0);
    max_frames_ = static_cast<uint64_t>(settings_.getOr<int64_t>("rolling.max_frames", int64_t{0}));
    pack_rows_ = settings_.getOr<bool>("rolling.pack_rows", true);

    const uint32_t width = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.width", int64_t{0}));
    const uint32_t height = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.height", int64_t{0}));
    const uint32_t source_stride = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.stride_bytes", int64_t{width}));
    const std::string pixel_format = settings_.getOr<std::string>("camera.pixel_format", "bayer_gbrg8");
    const std::string bayer_pattern = settings_.getOr<std::string>("camera.bayer_pattern", "GBRG");

    if (width == 0 || height == 0) {
        std::cerr << "RollingRawRecorder: invalid frame dimensions " << width << "x" << height << "\n";
        return false;
    }

    if (!isGbrgRaw8Like(pixel_format, bayer_pattern)) {
        std::cerr << "RollingRawRecorder: raw8bayerGBRG_rolling requires one-byte GBRG Bayer input, "
                  << "but camera.pixel_format='" << pixel_format
                  << "' and camera.bayer_pattern='" << bayer_pattern << "'\n";
        return false;
    }

    const int64_t bytes_per_pixel = settings_.getOr<int64_t>("camera.bytes_per_pixel", int64_t{1});
    if (bytes_per_pixel != 1) {
        std::cerr << "RollingRawRecorder: raw8bayerGBRG_rolling requires 1 byte/pixel, got "
                  << bytes_per_pixel << " bytes/pixel\n";
        return false;
    }

    if (source_stride < width) {
        std::cerr << "RollingRawRecorder: source stride " << source_stride
                  << " is smaller than width " << width << "\n";
        return false;
    }

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

    // In software-timed/free-run mode the recorder owns the pacing.
    // In hardware-trigger mode the camera/triggerbox owns the pacing, and grab()
    // blocks up to camera.grab_timeout_ms while we wait for the next trigger.
    const auto frame_period = (!hardware_trigger_ && target_fps_ > 0.0)
        ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(1.0 / target_fps_))
        : std::chrono::steady_clock::duration::zero();
    auto next_frame_time = std::chrono::steady_clock::now();

    auto last_timeout_warn = std::chrono::steady_clock::now() -
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(timeout_warn_interval_s_));
    auto last_fps_report = std::chrono::steady_clock::now();
    uint64_t frames_at_last_report = frames_written_.load();
    bool waiting_reported = false;

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
            ok = camera_->grab(data, size, camera_ts, w, h, stride, grab_timeout_ms_);
        } catch (...) {
            ok = false;
        }
        // Capture the host/PC UTC timestamp immediately after grab() returns.
        // This keeps pc_utc_ns as close as practical to xiGetImage() return time,
        // before row packing, validation, file rollover, or disk writes can add jitter.
        const uint64_t utc_ns_after_grab = ok ? systemUtcNowNs() : 0ULL;

        if (!ok || !data || w <= 0 || h <= 0) {
            if (hardware_trigger_) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_timeout_warn >= std::chrono::duration<double>(timeout_warn_interval_s_)) {
                    last_timeout_warn = now;
                    const std::string message =
                        "No triggered frames received within " + std::to_string(grab_timeout_ms_) +
                        " ms; expecting approximately " + std::to_string(expected_hardware_fps_) +
                        " Hz frames from hardware trigger. Check triggerbox, cable, GPI selector, and XIMEA trigger mode.";
                    std::cerr << "RollingRawRecorder: " << message << "\n";
                    if (event_callback_) event_callback_("hardware_trigger_timeout", false, message);
                    waiting_reported = true;
                }
            }
            continue;
        }

        if (hardware_trigger_ && waiting_reported) {
            waiting_reported = false;
            const std::string message = "Triggered frames resumed.";
            std::cerr << "RollingRawRecorder: " << message << "\n";
            if (event_callback_) event_callback_("hardware_trigger_resumed", true, message);
        }

        if (stride < w) {
            std::cerr << "RollingRawRecorder: grab returned invalid stride " << stride
                      << " for width " << w << "\n";
            running_ = false;
            break;
        }

        const size_t minimum_bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
        if (size < minimum_bytes) {
            std::cerr << "RollingRawRecorder: grab returned size " << size
                      << " but stride*height is " << minimum_bytes << "\n";
            running_ = false;
            break;
        }

        const uint64_t utc_ns = utc_ns_after_grab;
        const uint8_t* payload = data;
        uint32_t payload_bytes = static_cast<uint32_t>(w * h);

        if (pack_rows_ || stride != w) {
            packRows(data, w, h, stride, packed);
            payload = packed.data();
            payload_bytes = static_cast<uint32_t>(packed.size());
        } else {
            payload_bytes = static_cast<uint32_t>(std::min<size_t>(size, static_cast<size_t>(w) * static_cast<size_t>(h)));
        }

        const uint64_t camera_frame_number = camera_->lastCameraFrameNumber();

        if (!writer_.writeFrame(frames_written_, utc_ns, camera_ts, camera_frame_number, payload, payload_bytes)) {
            std::cerr << "RollingRawRecorder: writeFrame failed\n";
            if (event_callback_) event_callback_("rolling_write_failed", false, "Rolling raw writeFrame failed.");
            running_ = false;
            break;
        }
        ++frames_written_;

        if (hardware_trigger_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_fps_report >= std::chrono::duration<double>(fps_report_interval_s_)) {
                const uint64_t current_frames = frames_written_.load();
                const double elapsed = std::chrono::duration<double>(now - last_fps_report).count();
                const double measured_fps = elapsed > 0.0
                    ? static_cast<double>(current_frames - frames_at_last_report) / elapsed
                    : 0.0;
                last_fps_report = now;
                frames_at_last_report = current_frames;
                const std::string message =
                    "Hardware-triggered acquisition rate approximately " +
                    std::to_string(measured_fps) + " Hz; expected " +
                    std::to_string(expected_hardware_fps_) + " Hz.";
                std::cerr << "RollingRawRecorder: " << message << "\n";
                if (event_callback_) event_callback_("hardware_trigger_rate", true, message);
            }
        }
    }
}

}  // namespace cambuffer_recorder_ng
