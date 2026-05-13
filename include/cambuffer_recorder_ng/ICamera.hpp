#pragma once

#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace cambuffer_recorder_ng
{

class ICamera
{
public:
    virtual ~ICamera() = default;

    /**
     * @brief Apply requested settings before open()/start().
     *
     * Backends may override this to validate and apply settings. The default
     * stores the requested settings as the effective settings, which keeps older
     * backends working while still making metadata available.
     */
    virtual void configure(const CameraSettings& requested_settings)
    {
        requested_settings_ = requested_settings;
        effective_settings_ = requested_settings;
    }

    virtual const CameraSettings& requestedSettings() const
    {
        return requested_settings_;
    }

    virtual const CameraSettings& effectiveSettings() const
    {
        return effective_settings_;
    }

    virtual std::string backendName() const
    {
        return "unknown";
    }

    virtual void open(int device_index = 0) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void close() {}

    virtual bool grab(uint8_t*& data,
                      size_t& size,
                      uint64_t& ts,
                      int& width,
                      int& height,
                      int& stride,
                      int timeout_ms = 100) = 0;

protected:
    CameraSettings requested_settings_;
    CameraSettings effective_settings_;
};

}  // namespace cambuffer_recorder_ng
