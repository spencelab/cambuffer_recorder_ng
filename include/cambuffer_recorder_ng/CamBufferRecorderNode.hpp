#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "cambuffer_recorder_ng/ICamera.hpp"
#include "cambuffer_recorder_ng/Recorder.hpp"
#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"
#include "cambuffer_recorder_ng/settings/SettingsManager.hpp"

namespace cambuffer_recorder_ng
{

/**
 * @brief Lifecycle node that wraps a selectable camera backend and Recorder.
 *
 * Backend-specific camera headers are intentionally not included here. They are
 * included only in the .cpp and only when the matching HAVE_* compile definition
 * is present, so missing SDKs do not break builds that only use FakeCamera.
 */
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
    std::string metadataPathForOutput(const std::string& output_path) const;

    std::shared_ptr<ICamera> camera_;
    std::shared_ptr<Recorder> recorder_;
    std::unique_ptr<SettingsManager> settings_manager_;

    std::thread worker_;
    std::atomic<bool> running_{false};

    CameraSettings requested_settings_;
    CameraSettings effective_settings_;

    std::string backend_{"fake"};
    int width_{640};
    int height_{480};
    int fps_{30};
};

}  // namespace cambuffer_recorder_ng
