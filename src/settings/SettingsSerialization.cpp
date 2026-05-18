#include "cambuffer_recorder_ng/settings/SettingsSerialization.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <cctype>

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


static std::string trimCopy(const std::string& input)
{
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

static std::string stripYamlComment(const std::string& line)
{
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_double;
        else if (c == '#' && !in_single && !in_double) return line.substr(0, i);
    }
    return line;
}

static std::string unquoteYamlString(const std::string& value)
{
    std::string v = trimCopy(value);
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                          (v.front() == '\'' && v.back() == '\''))) {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

static std::string lowerCopy(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool looksInteger(const std::string& value)
{
    if (value.empty()) return false;
    size_t i = (value[0] == '-' || value[0] == '+') ? 1 : 0;
    if (i >= value.size()) return false;
    for (; i < value.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    return true;
}

static bool looksFloating(const std::string& value)
{
    bool has_digit = false;
    bool has_float_marker = false;
    for (char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c))) has_digit = true;
        else if (c == '.' || c == 'e' || c == 'E') has_float_marker = true;
        else if (c == '-' || c == '+') {}
        else return false;
    }
    return has_digit && has_float_marker;
}

CameraSettings parseCameraSettingsYaml(const std::string& yaml_text)
{
    CameraSettings settings;
    std::istringstream input(yaml_text);
    std::string line;

    while (std::getline(input, line)) {
        line = trimCopy(stripYamlComment(line));
        if (line.empty() || line.rfind("---", 0) == 0) continue;
        if (line.rfind("- ", 0) == 0) continue;

        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trimCopy(line.substr(0, colon));
        std::string value = trimCopy(line.substr(colon + 1));

        if (key.empty()) continue;
        if (!key.empty() && ((key.front() == '"' && key.back() == '"') ||
                             (key.front() == '\'' && key.back() == '\''))) {
            key = key.substr(1, key.size() - 2);
        }

        // Ignore YAML container lines. The project settings use flat dotted keys under
        // ros__parameters, so no indentation-aware reconstruction is needed here.
        if (value.empty() || key == "cambuffer_recorder_ng" || key == "ros__parameters") {
            continue;
        }

        const std::string raw = unquoteYamlString(value);
        const std::string lower = lowerCopy(raw);

        try {
            if (lower == "true") {
                settings.set(key, true);
            } else if (lower == "false") {
                settings.set(key, false);
            } else if (looksInteger(raw)) {
                settings.set(key, static_cast<int64_t>(std::stoll(raw)));
            } else if (looksFloating(raw)) {
                settings.set(key, std::stod(raw));
            } else {
                settings.set(key, raw);
            }
        } catch (const std::exception&) {
            // If numeric parsing fails, preserve the scalar as a string rather than
            // rejecting the whole settings document.
            settings.set(key, raw);
        }
    }

    return settings;
}

void writeTextFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Could not open file for writing: " + path);
    out << text;
}

}  // namespace cambuffer_recorder_ng
