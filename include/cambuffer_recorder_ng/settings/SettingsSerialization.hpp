#pragma once

#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

#include <string>

namespace cambuffer_recorder_ng
{

std::string utcTimestampForFilename();
std::string cameraSettingsToYaml(const CameraSettings& settings, const std::string& indent = "  ");

// Minimal YAML parser for flat camera settings documents. It accepts lines like
// camera.width: 2048, quoted strings, booleans, integers, and doubles.
// Container lines such as cambuffer_recorder_ng: and ros__parameters: are ignored.
CameraSettings parseCameraSettingsYaml(const std::string& yaml_text);

std::string buildMetadataYaml(const std::string& run_id,
                              const std::string& backend_name,
                              const std::string& mode_name,
                              const std::string& output_kind,
                              const CameraSettings& requested_settings,
                              const CameraSettings& effective_settings);

void writeTextFile(const std::string& path, const std::string& text);

}  // namespace cambuffer_recorder_ng
