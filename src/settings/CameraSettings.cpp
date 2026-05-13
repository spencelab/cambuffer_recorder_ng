#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

namespace cambuffer_recorder_ng
{

void CameraSettings::set(const std::string& name, SettingValue value)
{
    values_[name] = std::move(value);
}

bool CameraSettings::has(const std::string& name) const
{
    return values_.find(name) != values_.end();
}

const SettingValue& CameraSettings::at(const std::string& name) const
{
    auto it = values_.find(name);
    if (it == values_.end()) {
        throw std::runtime_error("Missing setting: " + name);
    }
    return it->second;
}

const std::map<std::string, SettingValue>& CameraSettings::values() const
{
    return values_;
}

void CameraSettings::mergeFrom(const CameraSettings& other)
{
    for (const auto& [key, value] : other.values()) {
        values_[key] = value;
    }
}

}  // namespace cambuffer_recorder_ng
