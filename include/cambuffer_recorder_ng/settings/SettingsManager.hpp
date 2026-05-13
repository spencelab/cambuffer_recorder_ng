#pragma once

#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <string>

namespace cambuffer_recorder_ng
{

class SettingsManager
{
public:
    explicit SettingsManager(rclcpp_lifecycle::LifecycleNode& node);

    CameraSettings buildRequestedSettings();

    static CameraSettings defaultsForBackend(const std::string& backend_name);
    static CameraSettings defaultsForMode(const std::string& mode_name);

private:
    rclcpp_lifecycle::LifecycleNode& node_;

    void declareParameters();
    CameraSettings readRosOverrides();
};

std::string normalizeBackendName(std::string backend);
std::string normalizeModeName(std::string mode);

}  // namespace cambuffer_recorder_ng
