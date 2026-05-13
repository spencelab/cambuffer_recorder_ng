#include "cambuffer_recorder_ng/settings/SettingsSerialization.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace cambuffer_recorder_ng
{

std::string utcTimestampForFilename()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%dT%H%M%SZ");
    return oss.str();
}

static std::string escapeYamlString(const std::string& s)
{
    std::ostringstream oss;
    oss << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') oss << '\\';
        oss << c;
    }
    oss << '"';
    return oss.str();
}

static std::string yamlScalar(const SettingValue& value)
{
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        std::ostringstream oss;
        if constexpr (std::is_same_v<T, std::string>) {
            return escapeYamlString(v);
        } else if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else {
            oss << std::setprecision(12) << v;
            return oss.str();
        }
    }, value);
}

std::string cameraSettingsToYaml(const CameraSettings& settings, const std::string& indent)
{
    std::ostringstream oss;
    for (const auto& [key, value] : settings.values()) {
        oss << indent << key << ": " << yamlScalar(value) << "\n";
    }
    return oss.str();
}

std::string buildMetadataYaml(const std::string& run_id,
                              const std::string& backend_name,
                              const std::string& mode_name,
                              const std::string& output_kind,
                              const CameraSettings& requested_settings,
                              const CameraSettings& effective_settings)
{
    std::ostringstream out;
    out << "cambuffer_recorder_ng:\n";
    out << "  run_id: \"" << run_id << "\"\n";
    out << "  camera_backend: \"" << backend_name << "\"\n";
    out << "  mode: \"" << mode_name << "\"\n";
    out << "  output_kind: \"" << output_kind << "\"\n";
    out << "  requested_settings:\n";
    out << cameraSettingsToYaml(requested_settings, "    ");
    out << "  effective_settings:\n";
    out << cameraSettingsToYaml(effective_settings, "    ");
    return out.str();
}

void writeTextFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Could not open file for writing: " + path);
    out << text;
}

}  // namespace cambuffer_recorder_ng
