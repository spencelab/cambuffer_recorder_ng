#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "cambuffer_recorder_ng/ICamera.hpp"
#include "cambuffer_recorder_ng/Recorder.hpp"
#include "cambuffer_recorder_ng/raw/RollingRawRecorder.hpp"
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

    std::shared_ptr<ICamera> camera_;
    std::shared_ptr<Recorder> recorder_;
    std::shared_ptr<RollingRawRecorder> rolling_raw_recorder_;

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

    int width_{640};
    int height_{480};
    int fps_{30};
};

}  // namespace cambuffer_recorder_ng
