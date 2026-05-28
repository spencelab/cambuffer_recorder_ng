#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace cambuffer_recorder_ng
{

struct StorageStatus
{
    bool ok = false;
    std::uint64_t capacity_bytes = 0;
    std::uint64_t free_bytes = 0;
    std::uint64_t available_bytes = 0;
    std::string checked_path;
    std::string error;
};

inline std::filesystem::path storageCheckDirectoryForPath(const std::string& target_path)
{
    std::filesystem::path p(target_path.empty() ? "." : target_path);

    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        return p;
    }

    if (p.has_parent_path()) {
        return p.parent_path();
    }

    return std::filesystem::current_path(ec);
}

inline StorageStatus getStorageStatusForPath(const std::string& target_path)
{
    StorageStatus out;

    try {
        const auto dir = storageCheckDirectoryForPath(target_path);
        out.checked_path = dir.string();

        std::error_code ec;
        const auto info = std::filesystem::space(dir, ec);
        if (ec) {
            out.error = ec.message();
            return out;
        }

        out.capacity_bytes = info.capacity;
        out.free_bytes = info.free;
        out.available_bytes = info.available;
        out.ok = true;
        return out;
    } catch (const std::exception& e) {
        out.error = e.what();
        return out;
    }
}

inline double bytesToGib(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

}  // namespace cambuffer_recorder_ng
