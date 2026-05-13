#pragma once

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

namespace cambuffer_recorder_ng
{

using SettingValue = std::variant<bool, int64_t, double, std::string>;

inline std::string settingValueToString(const SettingValue& value)
{
    return std::visit([](const auto& v) -> std::string {
        std::ostringstream oss;
        oss << v;
        return oss.str();
    }, value);
}

template <typename T>
T getSettingAs(const SettingValue& value, const std::string& name)
{
    if (!std::holds_alternative<T>(value)) {
        throw std::runtime_error("Setting '" + name + "' has wrong type");
    }
    return std::get<T>(value);
}

}  // namespace cambuffer_recorder_ng
