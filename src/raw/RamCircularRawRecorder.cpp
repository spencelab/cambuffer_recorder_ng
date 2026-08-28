#include "cambuffer_recorder_ng/raw/RamCircularRawRecorder.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <sstream>
#include <thread>

#ifdef __linux__
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

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

    if (pf == "bayer_gbrg8" || pf == "raw8_bayer_gbrg" || pf == "bayer8_gbrg") return true;
    if ((pf == "raw8" || pf == "bayer8") && (bp == "gbrg" || bp.empty())) return true;
    return false;
}

bool isMono8Like(const std::string& pixel_format)
{
    const std::string pf = normalizeToken(pixel_format);
    return pf == "mono8" || pf == "raw8" || pf == "gray8" || pf == "grey8";
}

std::string canonicalDumpPolicy(std::string policy)
{
    policy = normalizeToken(std::move(policy));
    if (policy.empty() || policy == "pause" || policy == "pause_capture" || policy == "pause_acquisition") {
        return "pause_acquisition";
    }
    if (policy == "continue" || policy == "continue_capture" || policy == "continue_acquisition") {
        return "continue_acquisition";
    }
    return policy;
}

}  // namespace

bool RamCircularRawRecorder::start(std::shared_ptr<ICamera> camera,
                                   const CameraSettings& settings,
                                   const std::string& session_path_prefix)
{
    if (running_) return false;
    if (!camera) return false;

    camera_ = std::move(camera);
    settings_ = settings;
    session_path_prefix_ = session_path_prefix;

    std::string error;
    if (!validateAndConfigureRawMode(error)) {
        emitEvent("ram_buffer_config_failed", false, error);
        return false;
    }

    capacity_frames_ = static_cast<uint32_t>(std::max<int64_t>(
        int64_t{1}, settings_.getOr<int64_t>("ram_buffer.capacity_frames", int64_t{1500})));
    target_fps_ = settings_.getOr<double>("camera.fps", 5.0);
    hardware_trigger_ = settings_.getOr<bool>("camera.hardware_trigger", false);
    expected_hardware_fps_ = settings_.getOr<double>("camera.expected_hardware_fps", target_fps_);
    if (expected_hardware_fps_ <= 0.0) expected_hardware_fps_ = target_fps_;
    grab_timeout_ms_ = static_cast<int>(settings_.getOr<int64_t>("camera.grab_timeout_ms", int64_t{1000}));
    timeout_warn_interval_s_ = settings_.getOr<double>("camera.timeout_warn_interval_s", 5.0);
    fps_report_interval_s_ = settings_.getOr<double>("camera.fps_report_interval_s", 5.0);
    pack_rows_ = settings_.getOr<bool>("rolling.pack_rows", true);
    dump_policy_ = canonicalDumpPolicy(settings_.getOr<std::string>("ram_buffer.dump_policy", "pause_acquisition"));
    dump_wait_timeout_s_ = settings_.getOr<double>("ram_buffer.dump_wait_timeout_s", 30.0);

    if (dump_policy_ != "pause_acquisition" && dump_policy_ != "continue_acquisition") {
        emitEvent("ram_buffer_config_failed", false,
                  "ram_buffer.dump_policy='" + dump_policy_ + "' requested; supported values are pause_acquisition and continue_acquisition.");
        return false;
    }
    ping_pong_ = (dump_policy_ == "continue_acquisition");

    roll_bytes_ = static_cast<uint64_t>(settings_.getOr<int64_t>("rolling.max_file_bytes", int64_t{0}));
    if (roll_bytes_ == 0) {
        const double gib = settings_.getOr<double>("rolling.max_file_gib", 2.0);
        roll_bytes_ = static_cast<uint64_t>(gib * 1024.0 * 1024.0 * 1024.0);
    }
    if (roll_bytes_ == 0) roll_bytes_ = 2ULL * 1024ULL * 1024ULL * 1024ULL;

    run_start_utc_ns_ = systemUtcNowNs();
    write_slot_ = 0;
    frames_captured_ = 0;
    dropped_frames_ = 0;
    camera_frame_gaps_ = 0;
    camera_frame_nonmonotonic_ = 0;

    rings_[0].clear();
    rings_[0].resize(capacity_frames_);
    for (auto& slot : rings_[0]) {
        slot.data.assign(payload_bytes_, 0);
        slot.valid = false;
    }
    rings_[1].clear();
    if (ping_pong_) {
        rings_[1].resize(capacity_frames_);
        for (auto& slot : rings_[1]) {
            slot.data.assign(payload_bytes_, 0);
            slot.valid = false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pause_requested_ = false;
        paused_ = false;
        camera_started_ = true;
        active_ring_ = 0;
        drain_in_progress_ = false;
        // The first ring starts "empty", not "full" -- the very first dump
        // request must wait for a genuine fresh window (filling), same as
        // every subsequent activation. See plan section 3.
        active_ring_full_ = !ping_pong_;
        frames_captured_since_activation_ = 0;
        swap_requested_ = false;
        swap_done_ = false;
    }

    const uint32_t ring_count = ping_pong_ ? 2 : 1;
    std::ostringstream oss;
    oss << "RAM circular raw recorder starting: capacity=" << capacity_frames_
        << " frames" << (ping_pong_ ? " per ring (2 rings, ping-pong)" : "")
        << ", frame_payload=" << payload_bytes_
        << " bytes, approximate RAM="
        << (static_cast<double>(ring_count) * static_cast<double>(capacity_frames_) *
            static_cast<double>(payload_bytes_) / (1024.0 * 1024.0 * 1024.0))
        << " GiB, dump_policy=" << dump_policy_;
    emitEvent("ram_buffer_starting", true, oss.str());

    running_ = true;
    worker_ = std::thread(&RamCircularRawRecorder::loop, this);
    return true;
}

void RamCircularRawRecorder::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        pause_requested_ = false;
        paused_ = false;
        swap_requested_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    // Let an in-flight background drain finish writing and close its file
    // rather than abandoning it mid-write. runDrain() itself checks running_
    // between frames and stops early (producing a valid partial file) once
    // running_ is false, so this join is bounded to roughly one frame's
    // write time, not the full remaining drain.
    std::lock_guard<std::mutex> drain_lock(drain_mutex_);
    if (drain_thread_.joinable()) drain_thread_.join();
}

bool RamCircularRawRecorder::validateAndConfigureRawMode(std::string& error)
{
    width_ = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.width", int64_t{0}));
    height_ = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.height", int64_t{0}));
    source_stride_bytes_ = static_cast<uint32_t>(settings_.getOr<int64_t>("camera.stride_bytes", int64_t{width_}));
    payload_bytes_ = width_ * height_;

    if (width_ == 0 || height_ == 0) {
        error = "RamCircularRawRecorder: invalid frame dimensions " + std::to_string(width_) + "x" + std::to_string(height_);
        return false;
    }
    if (source_stride_bytes_ < width_) {
        error = "RamCircularRawRecorder: source stride " + std::to_string(source_stride_bytes_) +
                " is smaller than width " + std::to_string(width_);
        return false;
    }

    const int64_t bytes_per_pixel = settings_.getOr<int64_t>("camera.bytes_per_pixel", int64_t{1});
    if (bytes_per_pixel != 1) {
        error = "RamCircularRawRecorder: RAW8 RAM buffer modes require 1 byte/pixel, got " +
                std::to_string(bytes_per_pixel);
        return false;
    }

    const std::string mode = normalizeToken(settings_.getOr<std::string>("mode", "raw8bayergbrg_ram_buffer"));
    const std::string pixel_format = settings_.getOr<std::string>("camera.pixel_format", "bayer_gbrg8");
    const std::string bayer_pattern = settings_.getOr<std::string>("camera.bayer_pattern", "GBRG");

    if (mode == "raw8bayergbrg_ram_buffer" || mode == "raw8bayergbrg_ram" || mode == "raw8bayergbrg_ring") {
        if (!isGbrgRaw8Like(pixel_format, bayer_pattern)) {
            error = "RamCircularRawRecorder: raw8bayerGBRG_ram_buffer requires one-byte GBRG Bayer input, camera.pixel_format='" +
                    pixel_format + "', camera.bayer_pattern='" + bayer_pattern + "'";
            return false;
        }
        raw_pixel_format_ = RawPixelFormat::RAW8_BAYER_GBRG;
    } else if (mode == "raw8mono_ram_buffer" || mode == "raw8mono_ram" || mode == "mono8_ram_buffer") {
        if (!isMono8Like(pixel_format)) {
            error = "RamCircularRawRecorder: raw8mono_ram_buffer requires one-byte mono input, camera.pixel_format='" +
                    pixel_format + "'";
            return false;
        }
        raw_pixel_format_ = RawPixelFormat::RAW8_MONO;
    } else {
        error = "RamCircularRawRecorder: unsupported RAM raw mode '" + mode + "'";
        return false;
    }

    return true;
}

void RamCircularRawRecorder::copyPackedPayload(const uint8_t* src,
                                               size_t src_size,
                                               int width,
                                               int height,
                                               int source_stride,
                                               FrameSlot& slot)
{
    const size_t wanted = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (slot.data.size() != wanted) slot.data.resize(wanted);

    if ((!pack_rows_ && source_stride == width) || source_stride == width) {
        const size_t to_copy = std::min(src_size, wanted);
        std::memcpy(slot.data.data(), src, to_copy);
        if (to_copy < wanted) std::memset(slot.data.data() + to_copy, 0, wanted - to_copy);
    } else {
        for (int y = 0; y < height; ++y) {
            std::memcpy(slot.data.data() + static_cast<size_t>(y) * static_cast<size_t>(width),
                        src + static_cast<size_t>(y) * static_cast<size_t>(source_stride),
                        static_cast<size_t>(width));
        }
    }
    slot.payload_bytes = static_cast<uint32_t>(wanted);
    slot.source_stride_bytes = static_cast<uint32_t>(source_stride);
}

void RamCircularRawRecorder::loop()
{
    uint64_t camera_ts = 0;
    int w = 0;
    int h = 0;
    int stride = 0;
    size_t captured_payload_bytes = 0;

    const auto frame_period = (!hardware_trigger_ && target_fps_ > 0.0)
        ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(1.0 / target_fps_))
        : std::chrono::steady_clock::duration::zero();
    auto next_frame_time = std::chrono::steady_clock::now();

    auto last_timeout_warn = std::chrono::steady_clock::now() -
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(timeout_warn_interval_s_));
    auto last_fps_report = std::chrono::steady_clock::now();
    uint64_t frames_at_last_report = 0;
    bool waiting_reported = false;
    uint64_t previous_camera_frame_number = 0;

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (pause_requested_) {
                paused_ = true;
                cv_.notify_all();
                cv_.wait(lock, [this]() { return !running_ || !pause_requested_; });
                paused_ = false;
                cv_.notify_all();
            }
            // Ring swap handshake for continue_acquisition dumps (plan section 3).
            // This is the only place a swap happens: right here, between two
            // grabs, so the ring being frozen never has a write in flight when
            // the background drain thread starts reading it. No I/O happens in
            // this block -- just index/counter bookkeeping -- so it resolves in
            // well under a frame period.
            if (ping_pong_ && swap_requested_) {
                const uint32_t old_active = active_ring_.load();
                const uint32_t new_active = 1U - old_active;
                const uint64_t total_captured = frames_captured_.load();
                frozen_ring_after_swap_ = old_active;
                frozen_ring_first_frame_index_ =
                    (total_captured >= capacity_frames_) ? (total_captured - capacity_frames_) : 0ULL;
                frozen_ring_last_frame_index_ =
                    (total_captured > 0) ? (total_captured - 1) : 0ULL;

                active_ring_.store(new_active);
                write_slot_ = 0;
                frames_captured_since_activation_ = 0;
                active_ring_full_ = false;
                drain_in_progress_ = true;

                swap_requested_ = false;
                swap_done_ = true;
                cv_.notify_all();
            }
        }
        if (!running_) break;

        if (frame_period.count() > 0) {
            std::this_thread::sleep_until(next_frame_time);
            next_frame_time += frame_period;
            const auto now = std::chrono::steady_clock::now();
            if (next_frame_time < now - frame_period) {
                next_frame_time = now + frame_period;
            }
        }

        uint32_t slot_index = 0;
        uint32_t ring_idx = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ring_idx = ping_pong_ ? active_ring_.load() : 0U;
            slot_index = write_slot_;
            // Mark the destination slot invalid before allowing the backend to
            // write into it. This prevents a concurrent dump from reading a
            // partially overwritten old frame. The ring has spare capacity for
            // the production window, so invalidating one soon-to-be-overwritten
            // slot is preferable to holding the mutex during xiGetImage().
            rings_[ring_idx][slot_index].valid = false;
        }

        // Bound to the ring/slot chosen above for the duration of this grab. If
        // a swap happens to land between here and the commit below, ring_idx
        // still names the ring this in-flight frame belongs to -- the swap
        // handshake above only ever runs when no grab is in flight, so ring_idx
        // is exactly the ring that was active_ring_ at that moment, whether or
        // not it still is by the time the commit runs.
        FrameSlot& slot = rings_[ring_idx][slot_index];
        camera_ts = 0;
        w = 0;
        h = 0;
        stride = 0;
        captured_payload_bytes = 0;

        bool ok = false;
        try {
            ok = camera_->grabPackedInto(slot.data.data(),
                                         slot.data.size(),
                                         captured_payload_bytes,
                                         camera_ts,
                                         w,
                                         h,
                                         stride,
                                         grab_timeout_ms_);
        } catch (...) {
            ok = false;
        }
        const uint64_t utc_ns_after_grab = ok ? systemUtcNowNs() : 0ULL;

        if (!ok || w <= 0 || h <= 0) {
            if (hardware_trigger_) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_timeout_warn >= std::chrono::duration<double>(timeout_warn_interval_s_)) {
                    last_timeout_warn = now;
                    const std::string message =
                        "No triggered frames received within " + std::to_string(grab_timeout_ms_) +
                        " ms; expecting approximately " + std::to_string(expected_hardware_fps_) +
                        " Hz frames from hardware trigger. Check triggerbox, cable, GPI selector, and XIMEA trigger mode.";
                    emitEvent("hardware_trigger_timeout", false, message);
                    waiting_reported = true;
                }
            }
            continue;
        }

        if (hardware_trigger_ && waiting_reported) {
            waiting_reported = false;
            emitEvent("hardware_trigger_resumed", true, "Triggered frames resumed.");
        }

        if (static_cast<uint32_t>(w) != width_ || static_cast<uint32_t>(h) != height_) {
            emitEvent("ram_buffer_frame_size_changed", false,
                      "Grab returned " + std::to_string(w) + "x" + std::to_string(h) +
                      ", expected " + std::to_string(width_) + "x" + std::to_string(height_) + "; stopping.");
            running_ = false;
            break;
        }
        if (stride != w) {
            emitEvent("ram_buffer_invalid_stride", false,
                      "grabPackedInto returned stride " + std::to_string(stride) +
                      ", expected packed stride " + std::to_string(w));
            running_ = false;
            break;
        }
        const size_t wanted_payload = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (captured_payload_bytes < wanted_payload) {
            emitEvent("ram_buffer_short_frame", false,
                      "grabPackedInto returned payload " + std::to_string(captured_payload_bytes) +
                      " but width*height is " + std::to_string(wanted_payload));
            running_ = false;
            break;
        }

        const uint64_t camera_frame_number = camera_->lastCameraFrameNumber();
        if (camera_frame_number > 0 && previous_camera_frame_number > 0) {
            if (camera_frame_number > previous_camera_frame_number + 1) {
                camera_frame_gaps_ += camera_frame_number - previous_camera_frame_number - 1;
            } else if (camera_frame_number <= previous_camera_frame_number) {
                ++camera_frame_nonmonotonic_;
            }
        }
        if (camera_frame_number > 0) previous_camera_frame_number = camera_frame_number;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            slot.valid = true;
            slot.frame_index = frames_captured_.load();
            slot.pc_utc_ns = utc_ns_after_grab;
            slot.camera_timestamp_ns = camera_ts;
            slot.camera_frame_number = camera_frame_number;
            slot.payload_bytes = static_cast<uint32_t>(wanted_payload);
            slot.source_stride_bytes = static_cast<uint32_t>(stride);

            // The swap handshake above only ever fires between iterations, so
            // ring_idx is still the active ring here -- safe to advance its
            // write cursor and (for ping-pong) its fresh-frame-since-activation
            // counter unconditionally.
            write_slot_ = (write_slot_ + 1) % capacity_frames_;
            ++frames_captured_;
            if (ping_pong_) {
                ++frames_captured_since_activation_;
                if (frames_captured_since_activation_ >= capacity_frames_) {
                    active_ring_full_ = true;
                }
            }
        }
        cv_.notify_all();

        if (hardware_trigger_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_fps_report >= std::chrono::duration<double>(fps_report_interval_s_)) {
                const uint64_t current_frames = frames_captured_.load();
                const double elapsed = std::chrono::duration<double>(now - last_fps_report).count();
                const double measured_fps = elapsed > 0.0
                    ? static_cast<double>(current_frames - frames_at_last_report) / elapsed
                    : 0.0;
                last_fps_report = now;
                frames_at_last_report = current_frames;
                emitEvent("hardware_trigger_rate", true,
                          "Hardware-triggered RAM buffer acquisition rate approximately " +
                          std::to_string(measured_fps) + " Hz; expected " +
                          std::to_string(expected_hardware_fps_) + " Hz.");
            }
        }
    }
}

bool RamCircularRawRecorder::waitUntilCaptured(uint64_t target_frame_index, std::string& message)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(dump_wait_timeout_s_));
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ok = cv_.wait_until(lock, deadline, [this, target_frame_index]() {
        return !running_ || frames_captured_.load() > target_frame_index;
    });
    if (!ok || frames_captured_.load() <= target_frame_index) {
        message = "Timed out waiting for post-trigger frames through frame_index " +
                  std::to_string(target_frame_index) + ". Captured through " +
                  (frames_captured_.load() == 0 ? std::string{"none"} : std::to_string(frames_captured_.load() - 1)) + ".";
        return false;
    }
    return true;
}

bool RamCircularRawRecorder::findFrameAtOrBeforeUtc(uint64_t anchor_pc_utc_ns,
                                                      FrameCursor& out,
                                                      std::string& message)
{
    if (anchor_pc_utc_ns == 0) {
        message = "Cannot resolve trigger frame from a zero anchor_pc_utc_ns.";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    bool found = false;
    uint64_t earliest_pc = 0;
    uint64_t latest_pc = 0;
    uint64_t earliest_frame = 0;
    uint64_t latest_frame = 0;

    // Only meaningful for pause_acquisition, which never swaps -- rings_[0] is
    // the sole ring in that mode.
    for (const auto& slot : rings_[0]) {
        if (!slot.valid || slot.payload_bytes == 0 || slot.pc_utc_ns == 0) continue;

        if (earliest_pc == 0 || slot.pc_utc_ns < earliest_pc) {
            earliest_pc = slot.pc_utc_ns;
            earliest_frame = slot.frame_index;
        }
        if (latest_pc == 0 || slot.pc_utc_ns > latest_pc) {
            latest_pc = slot.pc_utc_ns;
            latest_frame = slot.frame_index;
        }

        if (slot.pc_utc_ns <= anchor_pc_utc_ns &&
            (!found || slot.pc_utc_ns > out.pc_utc_ns)) {
            out.frame_index = slot.frame_index;
            out.pc_utc_ns = slot.pc_utc_ns;
            out.camera_timestamp_ns = slot.camera_timestamp_ns;
            out.camera_frame_number = slot.camera_frame_number;
            found = true;
        }
    }

    if (!found) {
        if (earliest_pc == 0) {
            message = "No valid frames are currently in the RAM buffer.";
        } else {
            message = "Could not find a buffered frame at or before anchor_pc_utc_ns " +
                      std::to_string(anchor_pc_utc_ns) + ". Earliest buffered frame is " +
                      std::to_string(earliest_frame) + " at pc_utc_ns " +
                      std::to_string(earliest_pc) + "; latest buffered frame is " +
                      std::to_string(latest_frame) + " at pc_utc_ns " +
                      std::to_string(latest_pc) + ".";
        }
        return false;
    }

    return true;
}

bool RamCircularRawRecorder::pauseCapture(std::string& message, double& stop_ms)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pause_requested_ = true;
        cv_.notify_all();
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(std::max(100, grab_timeout_ms_ + 250));
        const bool ok = cv_.wait_until(lock, deadline, [this]() { return !running_ || paused_; });
        if (!ok || !paused_) {
            message = "Timed out waiting for capture thread to pause before RAM dump.";
            pause_requested_ = false;
            cv_.notify_all();
            return false;
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    try {
        camera_->stop();
    } catch (const std::exception& e) {
        message = std::string("camera stop before RAM dump failed: ") + e.what();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pause_requested_ = false;
        }
        cv_.notify_all();
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    stop_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_started_ = false;
    }
    return true;
}

bool RamCircularRawRecorder::resumeCapture(std::string& message, double& start_ms)
{
    const auto t0 = std::chrono::steady_clock::now();
    try {
        camera_->start();
    } catch (const std::exception& e) {
        message = std::string("camera start after RAM dump failed: ") + e.what();
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    start_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_started_ = true;
        pause_requested_ = false;
    }
    cv_.notify_all();
    return true;
}

bool RamCircularRawRecorder::snapshotRange(std::vector<FrameSlot>& ring,
                                           uint64_t first_frame_index,
                                           uint64_t last_frame_index,
                                           bool allow_partial,
                                           std::vector<const FrameSlot*>& frames,
                                           std::string& message,
                                           uint64_t& actual_first,
                                           uint64_t& actual_last)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const uint64_t captured = frames_captured_.load();
    if (captured == 0 || last_frame_index >= captured) {
        message = "Requested dump range is not fully captured yet.";
        return false;
    }

    const uint64_t earliest = (captured > capacity_frames_) ? (captured - capacity_frames_) : 0ULL;
    if (first_frame_index < earliest) {
        if (!allow_partial) {
            message = "Requested dump starts at frame_index " + std::to_string(first_frame_index) +
                      ", but earliest frame still in RAM is " + std::to_string(earliest) +
                      ". Increase ram_buffer.capacity_frames or set allow_partial=true.";
            return false;
        }
        first_frame_index = earliest;
    }

    frames.clear();
    frames.reserve(static_cast<size_t>(last_frame_index - first_frame_index + 1));
    for (uint64_t frame_index = first_frame_index; frame_index <= last_frame_index; ++frame_index) {
        bool found = false;
        for (const auto& slot : ring) {
            if (slot.valid && slot.frame_index == frame_index) {
                frames.push_back(&slot);
                found = true;
                break;
            }
        }
        if (!found) {
            message = "Frame_index " + std::to_string(frame_index) + " was not found in RAM buffer.";
            return false;
        }
    }

    actual_first = first_frame_index;
    actual_last = last_frame_index;
    return true;
}

RamBufferDumpResult RamCircularRawRecorder::dump(const RamBufferDumpRequest& request)
{
    if (ping_pong_) {
        return dumpContinueAcquisition(request);
    }

    RamBufferDumpResult result;
    result.dump_prefix = request.output_prefix.empty() ? session_path_prefix_ + "_dump" : request.output_prefix;
    result.dropped_frames = dropped_frames_.load();

    if (!running_) {
        result.message = "RAM circular recorder is not running.";
        return result;
    }

    const uint64_t before_frames = request.frames_before_trigger;
    const uint64_t after_frames = request.frames_after_trigger;

    if (before_frames == 0 && after_frames == 0) {
        result.message = "RAM dump requested zero frames. Set ram_buffer.default_window_s/window_frames or provide a nonzero dump window.";
        return result;
    }

    const uint64_t captured_at_request = frames_captured_.load();
    if (captured_at_request == 0) {
        result.message = "No frames have been captured yet; nothing to dump.";
        return result;
    }

    // If the dump window ends at the trigger/request frame, pause first and define
    // the trigger frame as the last frame captured before the pause. This is the
    // classic lab "post-trigger" behavior: hit dump after the event and write the
    // previous N frames. It also avoids losing the oldest requested frame during
    // the small service-thread/capture-thread race.
    //
    // If the window extends after the trigger/request frame, keep the request-time
    // trigger frame, wait until the required future frames have arrived, then pause
    // acquisition and dump.
    uint64_t trigger_frame = 0;
    uint64_t target_last = 0;

    FrameCursor anchor_slot;
    std::string anchor_message;
    if (request.anchor_pc_utc_ns != 0) {
        if (!findFrameAtOrBeforeUtc(request.anchor_pc_utc_ns, anchor_slot, anchor_message)) {
            result.message = anchor_message;
            return result;
        }
        trigger_frame = anchor_slot.frame_index;
        result.anchor_pc_utc_ns = request.anchor_pc_utc_ns;
        result.trigger_frame_index = anchor_slot.frame_index;
        result.trigger_frame_pc_utc_ns = anchor_slot.pc_utc_ns;
        result.trigger_frame_camera_timestamp_ns = anchor_slot.camera_timestamp_ns;
        result.trigger_frame_camera_frame_number = anchor_slot.camera_frame_number;
    } else {
        // Legacy fallback: if no explicit anchor was supplied, use the current
        // capture cursor as the trigger frame. New callers should provide
        // anchor_pc_utc_ns so post-trigger dumps are anchored before any
        // storage checks, scheduling jitter, or pause/stop latency.
        trigger_frame = captured_at_request - 1;
        result.trigger_frame_index = trigger_frame;
    }

    target_last = trigger_frame + after_frames;
    if (after_frames != 0) {
        std::string wait_message;
        if (!waitUntilCaptured(target_last, wait_message)) {
            result.message = wait_message;
            return result;
        }
    }

    std::string pause_message;
    double stop_ms = 0.0;
    if (!pauseCapture(pause_message, stop_ms)) {
        result.message = pause_message;
        return result;
    }
    emitEvent("ram_buffer_pause_acquisition", true,
              "Paused acquisition for RAM dump; camera_->stop() took " + std::to_string(stop_ms) + " ms. " +
              "Anchor pc_utc_ns=" + std::to_string(result.anchor_pc_utc_ns) +
              ", trigger_frame_index=" + std::to_string(result.trigger_frame_index) +
              ", trigger_frame_pc_utc_ns=" + std::to_string(result.trigger_frame_pc_utc_ns) + ".");

    const uint64_t wanted_first = (before_frames == 0)
        ? trigger_frame + 1
        : (trigger_frame >= (before_frames - 1) ? trigger_frame - before_frames + 1 : 0ULL);
    const uint64_t wanted_last = target_last;

    if (wanted_first > wanted_last) {
        std::string resume_message;
        double start_ms = 0.0;
        (void)resumeCapture(resume_message, start_ms);
        result.message = "RAM dump resolved to an empty frame range.";
        return result;
    }

    std::vector<const FrameSlot*> frames;
    uint64_t actual_first = 0;
    uint64_t actual_last = 0;
    std::string snapshot_message;
    bool snapshot_ok = snapshotRange(rings_[0], wanted_first, wanted_last, request.allow_partial,
                                     frames, snapshot_message, actual_first, actual_last);

    bool write_ok = false;
    std::string write_message;
    if (snapshot_ok) {
        RawRollingWriter writer;
        writer.setRolloverCallback([this](uint32_t next_file_index) {
            if (!rollover_callback_) return true;
            return rollover_callback_(next_file_index);
        });

        RawRollingWriterConfig cfg;
        cfg.path_prefix = result.dump_prefix;
        cfg.roll_bytes = roll_bytes_;
        cfg.width = width_;
        cfg.height = height_;
        cfg.source_stride_bytes = source_stride_bytes_;
        cfg.pixel_format = raw_pixel_format_;
        cfg.run_start_utc_ns = run_start_utc_ns_;
        cfg.payload_crc32_enabled = settings_.getOr<bool>("raw.payload_crc32_enabled", true);

        if (!writer.open(cfg)) {
            write_message = "Could not open CBRRAW writer for dump prefix '" + result.dump_prefix + "'.";
        } else {
            write_ok = true;
            for (const auto* frame : frames) {
                if (!writer.writeFrame(frame->frame_index,
                                       frame->pc_utc_ns,
                                       frame->camera_timestamp_ns,
                                       frame->camera_frame_number,
                                       frame->data.data(),
                                       frame->payload_bytes)) {
                    write_ok = false;
                    write_message = "CBRRAW writeFrame failed during RAM dump.";
                    break;
                }
            }
            writer.close();
            result.files_written = writer.filesWritten();
            if (!result.files_written.empty()) result.first_file = result.files_written.front();
        }
    }

    std::string resume_message;
    double start_ms = 0.0;
    const bool resume_ok = resumeCapture(resume_message, start_ms);
    emitEvent("ram_buffer_resume_acquisition", resume_ok,
              resume_ok
                  ? "Resumed acquisition after RAM dump; camera_->start() took " + std::to_string(start_ms) + " ms."
                  : resume_message);

    if (!snapshot_ok) {
        result.message = snapshot_message;
        return result;
    }
    if (!write_ok) {
        result.message = write_message.empty() ? "RAM dump write failed." : write_message;
        return result;
    }
    if (!resume_ok) {
        result.message = resume_message;
        return result;
    }

    result.success = true;
    result.first_frame_index = actual_first;
    result.last_frame_index = actual_last;
    result.frames_written = static_cast<uint64_t>(frames.size());

    std::ostringstream oss;
    oss << "Dumped " << result.frames_written << " RAM-buffered frames ["
        << result.first_frame_index << ", " << result.last_frame_index << "] to "
        << result.dump_prefix << "_####.cbrraw";
    result.message = oss.str();
    emitEvent("ram_buffer_dump_complete", true, result.message);
    return result;
}

std::string RamCircularRawRecorder::ramBufferState() const
{
    if (!ping_pong_) return "rec";
    if (drain_in_progress_.load()) return "saving";
    if (!active_ring_full_.load()) return "filling";
    return "rec";
}

bool RamCircularRawRecorder::swapRingsForContinueDump(uint32_t& frozen_ring_idx,
                                                       uint64_t& frozen_first_frame,
                                                       uint64_t& frozen_last_frame,
                                                       std::string& message)
{
    std::unique_lock<std::mutex> lock(mutex_);
    swap_requested_ = true;
    swap_done_ = false;
    cv_.notify_all();
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(100, grab_timeout_ms_ + 250));
    const bool ok = cv_.wait_until(lock, deadline, [this]() { return !running_ || swap_done_; });
    if (!running_) {
        swap_requested_ = false;
        message = "RAM circular recorder stopped while waiting for ring swap.";
        return false;
    }
    if (!ok || !swap_done_) {
        swap_requested_ = false;
        message = "Timed out waiting for capture thread to complete the ring swap.";
        return false;
    }
    frozen_ring_idx = frozen_ring_after_swap_;
    frozen_first_frame = frozen_ring_first_frame_index_;
    frozen_last_frame = frozen_ring_last_frame_index_;
    swap_done_ = false;
    return true;
}

RamBufferDumpResult RamCircularRawRecorder::dumpContinueAcquisition(const RamBufferDumpRequest& request)
{
    RamBufferDumpResult result;
    result.dropped_frames = dropped_frames_.load();

    if (!running_) {
        result.message = "RAM circular recorder is not running.";
        return result;
    }

    // Concurrent-dump policy (plan section 5): reject immediately, don't queue.
    // handleDumpBuffer() runs on cambuffer_recorder_ng's single-threaded ROS
    // executor (main.cpp), so dump() calls can never overlap with each other --
    // only against the capture/drain threads, which is what these two flags
    // report.
    if (drain_in_progress_.load()) {
        result.message = "RAM dump rejected: a previous dump is still saving (drain in progress). "
                          "Wait for the current dump to finish, then try again.";
        return result;
    }
    if (!active_ring_full_.load()) {
        result.message = "RAM dump rejected: the active ring is still filling -- it has not yet "
                          "captured a full fresh window since it last became active. Try again shortly.";
        return result;
    }

    // continue_acquisition always dumps the entire just-frozen ring (the most
    // recent capacity_frames_ frames), not an arbitrary trigger_position/
    // window_s-selected sub-window -- PINGPONG_BUFFER_PLAN.md section 3/6 does
    // not explicitly re-address window selection for this mode, so this is
    // this session's interpretation, made to satisfy the plan's own "no frames
    // predating this ring's activation" and "4 seconds of buffer depth"
    // framing. Flagged Needs verification in the session report.
    uint32_t frozen_ring_idx = 0;
    uint64_t frozen_first = 0;
    uint64_t frozen_last = 0;
    std::string swap_message;
    if (!swapRingsForContinueDump(frozen_ring_idx, frozen_first, frozen_last, swap_message)) {
        result.message = swap_message;
        return result;
    }

    result.dump_prefix = request.output_prefix.empty() ? session_path_prefix_ + "_dump" : request.output_prefix;
    result.first_frame_index = frozen_first;
    result.last_frame_index = frozen_last;
    result.frames_written = frozen_last - frozen_first + 1;
    result.success = true;

    std::ostringstream oss;
    oss << "RAM dump accepted (continue_acquisition): saving " << result.frames_written
        << " frames [" << frozen_first << ", " << frozen_last << "] from ring "
        << frozen_ring_idx << " to " << result.dump_prefix
        << "_####.cbrraw in the background; acquisition continues uninterrupted.";
    result.message = oss.str();
    emitEvent("ram_buffer_dump_accepted", true, result.message);

    const bool crc_enabled = settings_.getOr<bool>("raw.payload_crc32_enabled", true);
    {
        std::lock_guard<std::mutex> drain_lock(drain_mutex_);
        // Safety net: state machine guarantees the previous drain_thread_ has
        // already finished (drain_in_progress_ was false above), so this join
        // is instantaneous -- it just reclaims the thread handle.
        if (drain_thread_.joinable()) drain_thread_.join();
        drain_thread_ = std::thread(&RamCircularRawRecorder::runDrain, this,
                                    frozen_ring_idx, frozen_first, frozen_last,
                                    result.dump_prefix, crc_enabled);
    }

    return result;
}

void RamCircularRawRecorder::runDrain(uint32_t frozen_ring_idx,
                                      uint64_t first_frame_index,
                                      uint64_t last_frame_index,
                                      std::string dump_prefix,
                                      bool payload_crc32_enabled)
{
    lowerDrainThreadPriority();

    const auto t0 = std::chrono::steady_clock::now();

    std::vector<const FrameSlot*> frames;
    uint64_t actual_first = 0;
    uint64_t actual_last = 0;
    std::string snapshot_message;
    const bool snapshot_ok = snapshotRange(rings_[frozen_ring_idx], first_frame_index, last_frame_index,
                                           /*allow_partial=*/false, frames, snapshot_message,
                                           actual_first, actual_last);
    if (!snapshot_ok) {
        last_drain_ms_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        drain_in_progress_ = false;
        emitEvent("ram_buffer_dump_failed", false,
                  "Background RAM dump snapshot failed: " + snapshot_message);
        return;
    }

    RawRollingWriter writer;
    writer.setRolloverCallback([this](uint32_t next_file_index) {
        if (!rollover_callback_) return true;
        return rollover_callback_(next_file_index);
    });

    RawRollingWriterConfig cfg;
    cfg.path_prefix = dump_prefix;
    cfg.roll_bytes = roll_bytes_;
    cfg.width = width_;
    cfg.height = height_;
    cfg.source_stride_bytes = source_stride_bytes_;
    cfg.pixel_format = raw_pixel_format_;
    cfg.run_start_utc_ns = run_start_utc_ns_;
    cfg.payload_crc32_enabled = payload_crc32_enabled;

    bool write_ok = false;
    std::string write_message;
    uint64_t frames_written = 0;
    if (!writer.open(cfg)) {
        write_message = "Could not open CBRRAW writer for dump prefix '" + dump_prefix + "'.";
    } else {
        write_ok = true;
        for (const auto* frame : frames) {
            // Stop early rather than block process shutdown (see stop()).
            // Whatever has already been written stays a valid, closed,
            // audit-able partial file -- the "Ctrl-C mid-drain" acceptance
            // criterion in plan section 6.
            if (!running_) {
                write_message = "RAM circular recorder stopped mid-drain; finalized a partial dump.";
                break;
            }
            if (!writer.writeFrame(frame->frame_index, frame->pc_utc_ns, frame->camera_timestamp_ns,
                                   frame->camera_frame_number, frame->data.data(), frame->payload_bytes)) {
                write_ok = false;
                write_message = "CBRRAW writeFrame failed during background RAM dump.";
                break;
            }
            ++frames_written;
        }
        writer.close();
    }

    const double drain_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    last_drain_ms_ = drain_ms;

    // Clear drain_in_progress_ only once the file is fully closed -- GetStatus
    // reporting "rec"/"filling" should mean the previous dump's file is
    // already safely on disk, not merely "expected to finish soon".
    drain_in_progress_ = false;

    std::ostringstream oss;
    oss << "Background RAM dump complete: wrote " << frames_written << "/" << frames.size()
        << " frames [" << actual_first << ", " << actual_last << "] to " << dump_prefix
        << "_####.cbrraw in " << drain_ms << " ms.";
    if (!write_message.empty()) oss << " (" << write_message << ")";
    emitEvent("ram_buffer_dump_complete", write_ok, oss.str());
}

void RamCircularRawRecorder::lowerDrainThreadPriority()
{
#ifdef __linux__
    // Plan section 6 (Level 3): under any contention, the OS should always
    // favor the realtime capture thread over the background drain write.
    // Capture in this package does not currently request a realtime
    // scheduling policy (grabPackedInto() runs on plain SCHED_OTHER too), so
    // this is largely a no-op today beyond the nice() bump -- it is here so
    // the drain thread still does the right thing if capture priority is ever
    // raised to a realtime policy.
    sched_param sp{};
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    errno = 0;
    nice(5);  // best-effort; failure just leaves the process's default niceness
#endif
}

void RamCircularRawRecorder::emitEvent(const std::string& event_type, bool success, const std::string& message)
{
    if (event_callback_) event_callback_(event_type, success, message);
}

}  // namespace cambuffer_recorder_ng
