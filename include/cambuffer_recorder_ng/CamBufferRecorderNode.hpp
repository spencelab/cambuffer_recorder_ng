#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <rclcpp_lifecycle/lifecycle_publisher.hpp>

#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int64.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "cambuffer_recorder_ng/srv/apply_settings.hpp"
#include "cambuffer_recorder_ng/srv/get_status.hpp"
#include "cambuffer_recorder_ng/srv/dump_buffer.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "cambuffer_recorder_ng/ICamera.hpp"
#include "cambuffer_recorder_ng/Recorder.hpp"
#include "cambuffer_recorder_ng/raw/RollingRawRecorder.hpp"
#include "cambuffer_recorder_ng/raw/RamCircularRawRecorder.hpp"
#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

namespace cambuffer_recorder_ng
{

class CamBufferRecorderNode : public rclcpp_lifecycle::LifecycleNode
{
public:
    CamBufferRecorderNode();

    using CallbackReturn =
        rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

protected:
    CallbackReturn on_configure(const rclcpp_lifecycle::State &) override;
    CallbackReturn on_activate(const rclcpp_lifecycle::State &) override;
    CallbackReturn on_deactivate(const rclcpp_lifecycle::State &) override;
    CallbackReturn on_shutdown(const rclcpp_lifecycle::State &) override;

private:
    void run_loop();

    bool configureFromSettings(const CameraSettings& settings, std::string& message);
    bool startRecording(std::string& message);
    bool stopRecording(std::string& message);
    void cleanupCameraAndRecorders();

    CameraSettings resolveSettingsFromOverrides(const CameraSettings& overrides, bool merge_with_current) const;

    void handleApplySettings(
        const std::shared_ptr<cambuffer_recorder_ng::srv::ApplySettings::Request> request,
        std::shared_ptr<cambuffer_recorder_ng::srv::ApplySettings::Response> response);
    void handleStartRecording(
        const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response);
    void handleStopRecording(
        const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response);
    void handleGetStatus(
        const std::shared_ptr<cambuffer_recorder_ng::srv::GetStatus::Request> request,
        std::shared_ptr<cambuffer_recorder_ng::srv::GetStatus::Response> response);
    void handleDumpBuffer(
        const std::shared_ptr<cambuffer_recorder_ng::srv::DumpBuffer::Request> request,
        std::shared_ptr<cambuffer_recorder_ng::srv::DumpBuffer::Response> response);

    void publishSettingsEvent(const std::string& event_type, bool success, const std::string& message);
    void publishRecordingEvent(const std::string& event_type, bool success, const std::string& message);

    std::string storageTargetPath() const;
    bool checkStorageOrLog(const std::string& context, std::string& message);
    void publishStorageStatus();

    std::shared_ptr<ICamera> camera_;
    std::shared_ptr<Recorder> recorder_;
    std::shared_ptr<RollingRawRecorder> rolling_raw_recorder_;
    std::shared_ptr<RamCircularRawRecorder> ram_raw_recorder_;

    rclcpp::Service<cambuffer_recorder_ng::srv::ApplySettings>::SharedPtr apply_settings_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr start_recording_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stop_recording_srv_;
    rclcpp::Service<cambuffer_recorder_ng::srv::GetStatus>::SharedPtr get_status_srv_;
    rclcpp::Service<cambuffer_recorder_ng::srv::DumpBuffer>::SharedPtr dump_buffer_srv_;

    rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::String>::SharedPtr settings_event_pub_;
    rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::String>::SharedPtr recording_event_pub_;
    rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::UInt64>::SharedPtr storage_free_bytes_pub_;
    rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::Float64>::SharedPtr storage_free_gib_pub_;
    rclcpp::TimerBase::SharedPtr storage_status_timer_;

    std::thread worker_;
    std::atomic<bool> running_{false};

    CameraSettings requested_settings_;
    CameraSettings effective_settings_;

    std::string backend_{"fake"};
    std::string mode_{"video_rgb24"};
    std::string output_kind_{"video_mp4"};
    std::string run_id_;
    std::string output_path_;
    std::string metadata_path_;
    std::string rolling_path_prefix_;

    uint64_t dump_counter_{0};

    bool configured_{false};
    bool recording_{false};

    double min_free_space_gib_{8.0};
    uint64_t min_free_space_bytes_{8ULL * 1024ULL * 1024ULL * 1024ULL};

    int width_{640};
    int height_{480};
    int fps_{30};
};

}  // namespace cambuffer_recorder_ng
