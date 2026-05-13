#pragma once

#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

#include <string>

namespace cambuffer_recorder_ng
{

std::string utcTimestampForFilename();
std::string cameraSettingsToYaml(const CameraSettings& settings, const std::string& indent = "  ");

std::string buildMetadataYaml(const std::string& run_id,
                              const std::string& backend_name,
                              const std::string& mode_name,
                              const std::string& output_kind,
                              const CameraSettings& requested_settings,
                              const CameraSettings& effective_settings);

void writeTextFile(const std::string& path, const std::string& text);

}  // namespace cambuffer_recorder_ng
