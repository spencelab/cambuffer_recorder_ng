#pragma once

#include "cambuffer_recorder_ng/settings/CameraSettings.hpp"

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>
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

    // Optional low-copy path for RAM-buffer capture. Backends may override this
    // to ask the camera SDK to write directly into dst. The default fallback
    // uses grab() and packs/copies rows into dst. On success, payload_bytes is
    // width * height and stride is the packed stride, i.e. width.
    virtual bool grabPackedInto(uint8_t* dst,
                                size_t dst_capacity,
                                size_t& payload_bytes,
                                uint64_t& ts,
                                int& width,
                                int& height,
                                int& stride,
                                int timeout_ms = 100)
    {
        if (!dst) return false;

        uint8_t* src = nullptr;
        size_t src_size = 0;
        uint64_t camera_ts = 0;
        int src_width = 0;
        int src_height = 0;
        int src_stride = 0;
        if (!grab(src, src_size, camera_ts, src_width, src_height, src_stride, timeout_ms)) {
            return false;
        }
        if (!src || src_width <= 0 || src_height <= 0 || src_stride < src_width) {
            return false;
        }

        const size_t wanted = static_cast<size_t>(src_width) * static_cast<size_t>(src_height);
        const size_t minimum_src = static_cast<size_t>(src_stride) * static_cast<size_t>(src_height);
        if (dst_capacity < wanted || src_size < minimum_src) {
            return false;
        }

        if (src_stride == src_width) {
            std::memcpy(dst, src, wanted);
        } else {
            for (int y = 0; y < src_height; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(src_width),
                            src + static_cast<size_t>(y) * static_cast<size_t>(src_stride),
                            static_cast<size_t>(src_width));
            }
        }

        payload_bytes = wanted;
        ts = camera_ts;
        width = src_width;
        height = src_height;
        stride = src_width;
        return true;
    }

    // Optional backend-provided frame/sequence number for the most recent successful grab().
    // XIMEA provides this as XI_IMG.nframe. Return 0 when unavailable.
    virtual uint64_t lastCameraFrameNumber() const { return 0; }

protected:
    CameraSettings requested_settings_;
    CameraSettings effective_settings_;
};

} // namespace cambuffer_recorder_ng
