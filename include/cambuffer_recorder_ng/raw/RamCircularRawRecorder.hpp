#pragma once

#include "cambuffer_recorder_ng/ICamera.hpp"
#include "cambuffer_recorder_ng/raw/RawRollingWriter.hpp"
#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cambuffer_recorder_ng
{

struct RamBufferDumpRequest
{
    std::string label;

    // User-facing trigger/window description. trigger_position uses neutral terms:
    //   end    = dump window ending at trigger/request time
    //   center = dump window centered on trigger/request time
    //   start  = dump window starting after trigger/request time
    //   custom = use frames_before_trigger / frames_after_trigger directly
    std::string trigger_position{"end"};
    double window_s = 4.0;
    uint32_t window_frames = 0;

    // Resolved integer frame windows used by the recorder.
    // frames_before_trigger includes the trigger frame when nonzero.
    // frames_after_trigger excludes the trigger frame.
    uint64_t frames_before_trigger = 1000;
    uint64_t frames_after_trigger = 0;

    bool allow_partial = false;

    // Anchor UTC timestamp for the trigger/request, in nanoseconds.
    // Zero means use the recorder's current capture cursor when dump() is called.
    // When nonzero, the recorder resolves the trigger frame as the newest
    // buffered frame with pc_utc_ns <= anchor_pc_utc_ns.
    uint64_t anchor_pc_utc_ns = 0;

    std::string output_prefix;
};

struct RamBufferDumpResult
{
    bool success = false;
    std::string message;
    std::string dump_prefix;
    std::string first_file;
    uint64_t first_frame_index = 0;
    uint64_t last_frame_index = 0;
    uint64_t frames_written = 0;
    uint64_t dropped_frames = 0;

    uint64_t anchor_pc_utc_ns = 0;
    uint64_t trigger_frame_index = 0;
    uint64_t trigger_frame_pc_utc_ns = 0;
    uint64_t trigger_frame_camera_timestamp_ns = 0;
    uint64_t trigger_frame_camera_frame_number = 0;

    std::vector<std::string> files_written;
};

class RamCircularRawRecorder
{
public:
    RamCircularRawRecorder() = default;
    ~RamCircularRawRecorder() { stop(); }

    bool start(std::shared_ptr<ICamera> camera,
               const CameraSettings& settings,
               const std::string& session_path_prefix);
    void stop();

    RamBufferDumpResult dump(const RamBufferDumpRequest& request);

    using EventCallback = std::function<void(const std::string& event_type, bool success, const std::string& message)>;
    void setEventCallback(EventCallback callback) { event_callback_ = std::move(callback); }

    using RolloverCallback = std::function<bool(uint32_t next_file_index)>;
    void setRolloverCallback(RolloverCallback callback) { rollover_callback_ = std::move(callback); }

    uint64_t framesCaptured() const { return frames_captured_.load(); }
    uint64_t droppedFrames() const { return dropped_frames_.load(); }
    uint64_t cameraFrameGaps() const { return camera_frame_gaps_.load(); }
    uint64_t cameraFrameNonmonotonic() const { return camera_frame_nonmonotonic_.load(); }
    uint32_t capacityFrames() const { return capacity_frames_; }

    // Ping-pong (continue_acquisition) state machine, see PINGPONG_BUFFER_PLAN.md
    // section 3. Meaningless (always "rec") when dump_policy is pause_acquisition,
    // matching GetStatus.srv's ram_buffer_state contract.
    //   "saving"  - a background drain of the just-frozen ring is in progress.
    //   "filling" - the active ring has not yet captured a full window of fresh
    //               frames since it became active.
    //   "rec"     - neither condition holds; dump requests are accepted.
    std::string ramBufferState() const;
    bool pingPongEnabled() const { return ping_pong_; }
    double lastDrainMs() const { return last_drain_ms_.load(); }

private:
    struct FrameSlot
    {
        std::vector<uint8_t> data;
        bool valid = false;
        uint64_t frame_index = 0;
        uint64_t pc_utc_ns = 0;
        uint64_t camera_timestamp_ns = 0;
        uint64_t camera_frame_number = 0;
        uint32_t payload_bytes = 0;
        uint32_t source_stride_bytes = 0;
    };

    void loop();
    bool validateAndConfigureRawMode(std::string& error);
    void copyPackedPayload(const uint8_t* src,
                           size_t src_size,
                           int width,
                           int height,
                           int source_stride,
                           FrameSlot& slot);
    bool pauseCapture(std::string& message, double& stop_ms);
    bool resumeCapture(std::string& message, double& start_ms);
    bool waitUntilCaptured(uint64_t target_frame_index, std::string& message);

    // Ping-pong support. dump() for continue_acquisition calls this instead of
    // pauseCapture()/snapshotRange()/resumeCapture(): it performs a fast,
    // synchronous ring swap (no I/O) at a quiescent point in the capture loop
    // (mirrors pauseCapture()'s handshake, but never stops the camera), then
    // hands the just-frozen ring off to a background thread for writing. This
    // is what main.cpp:12's SingleThreadedExecutor requires (see plan section 3)
    // -- dump() must return promptly so GetStatus stays reachable during a drain.
    bool swapRingsForContinueDump(uint32_t& frozen_ring_idx,
                                  uint64_t& frozen_first_frame,
                                  uint64_t& frozen_last_frame,
                                  std::string& message);
    RamBufferDumpResult dumpContinueAcquisition(const RamBufferDumpRequest& request);
    void runDrain(uint32_t frozen_ring_idx,
                  uint64_t first_frame_index,
                  uint64_t last_frame_index,
                  std::string dump_prefix,
                  bool payload_crc32_enabled);
    void lowerDrainThreadPriority();
    struct FrameCursor
    {
        uint64_t frame_index = 0;
        uint64_t pc_utc_ns = 0;
        uint64_t camera_timestamp_ns = 0;
        uint64_t camera_frame_number = 0;
    };

    bool findFrameAtOrBeforeUtc(uint64_t anchor_pc_utc_ns,
                                FrameCursor& out,
                                std::string& message);
    bool snapshotRange(std::vector<FrameSlot>& ring,
                       uint64_t first_frame_index,
                       uint64_t last_frame_index,
                       bool allow_partial,
                       std::vector<const FrameSlot*>& frames,
                       std::string& message,
                       uint64_t& actual_first,
                       uint64_t& actual_last);
    void emitEvent(const std::string& event_type, bool success, const std::string& message);

    std::shared_ptr<ICamera> camera_;
    CameraSettings settings_;
    std::string session_path_prefix_;

    // pause_acquisition (the default/fallback, unaffected by this feature) uses
    // only rings_[0], exactly as the single-ring recorder always has.
    // continue_acquisition (ping-pong) uses both rings_[0]/rings_[1], swapping
    // which one is "active" (being written to) on each accepted dump.
    std::vector<FrameSlot> rings_[2];
    bool ping_pong_{false};
    std::atomic<uint32_t> active_ring_{0};
    std::atomic<bool> drain_in_progress_{false};
    std::atomic<bool> active_ring_full_{false};
    uint64_t frames_captured_since_activation_{0};  // guarded by mutex_, written only from loop()
    std::atomic<double> last_drain_ms_{0.0};
    std::thread drain_thread_;
    std::mutex drain_mutex_;  // serializes drain_thread_ start/join against stop()

    // Ring-swap handshake between dump()/swapRingsForContinueDump() (caller
    // thread, i.e. the ROS service-callback thread) and loop() (capture
    // thread). Guarded by mutex_, mirrors pause_requested_/paused_ below.
    bool swap_requested_{false};
    bool swap_done_{false};
    uint32_t frozen_ring_after_swap_{0};
    uint64_t frozen_ring_first_frame_index_{0};
    uint64_t frozen_ring_last_frame_index_{0};

    uint32_t capacity_frames_{1100};
    uint32_t write_slot_{0};

    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t source_stride_bytes_{0};
    uint32_t payload_bytes_{0};
    RawPixelFormat raw_pixel_format_{RawPixelFormat::RAW8_BAYER_GBRG};

    double target_fps_{5.0};
    bool hardware_trigger_{false};
    double expected_hardware_fps_{0.0};
    int grab_timeout_ms_{1000};
    double timeout_warn_interval_s_{5.0};
    double fps_report_interval_s_{5.0};
    bool pack_rows_{true};
    std::string dump_policy_{"pause_acquisition"};
    double dump_wait_timeout_s_{30.0};

    uint64_t roll_bytes_{2ULL * 1024ULL * 1024ULL * 1024ULL};
    uint64_t run_start_utc_ns_{0};

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_captured_{0};
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<uint64_t> camera_frame_gaps_{0};
    std::atomic<uint64_t> camera_frame_nonmonotonic_{0};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool pause_requested_{false};
    bool paused_{false};
    bool camera_started_{true};

    EventCallback event_callback_;
    RolloverCallback rollover_callback_;
};

}  // namespace cambuffer_recorder_ng
