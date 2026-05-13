#pragma once

#include "cambuffer_recorder_ng/settings/SettingValue.hpp"

#include <map>
#include <string>

namespace cambuffer_recorder_ng
{

class CameraSettings
{
public:
    void set(const std::string& name, SettingValue value);
    bool has(const std::string& name) const;
    const SettingValue& at(const std::string& name) const;

    template <typename T>
    T get(const std::string& name) const
    {
        return getSettingAs<T>(at(name), name);
    }

    template <typename T>
    T getOr(const std::string& name, const T& fallback) const
    {
        auto it = values_.find(name);
        if (it == values_.end()) return fallback;
        return getSettingAs<T>(it->second, name);
    }

    const std::map<std::string, SettingValue>& values() const;
    void mergeFrom(const CameraSettings& other);

private:
    std::map<std::string, SettingValue> values_;
};

}  // namespace cambuffer_recorder_ng
