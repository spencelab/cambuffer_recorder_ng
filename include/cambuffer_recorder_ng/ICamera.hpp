#pragma once

#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace cambuffer_recorder_ng {

class ICamera {
public:
    virtual ~ICamera() = default;

    virtual void configure(const CameraSettings& requested_settings) { requested_settings_ = requested_settings; }
    virtual CameraSettings getEffectiveSettings() const { return effective_settings_; }
    virtual std::string backendName() const { return "unknown"; }

    virtual void open(int device_index = 0) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void close() {}

    virtual bool grab(uint8_t*& data, size_t& size, uint64_t& ts,
                      int& width, int& height, int& stride, int timeout_ms = 100) = 0;

    // Optional backend-provided frame/sequence number for the most recent successful grab().
    // XIMEA provides this as XI_IMG.nframe. Return 0 when unavailable.
    virtual uint64_t lastCameraFrameNumber() const { return 0; }

protected:
    CameraSettings requested_settings_;
    CameraSettings effective_settings_;
};

} // namespace cambuffer_recorder_ng
