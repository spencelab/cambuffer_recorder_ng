#pragma once

#include "cambuffer_recorder_ng/ICamera.hpp"

#ifdef HAVE_XIMEA

#include <m3api/xiApi.h>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace cambuffer_recorder_ng {

class XiCamera : public ICamera {
public:
    XiCamera() = default;
    ~XiCamera() override = default;

    void configure(const CameraSettings& requested_settings) override;
    CameraSettings getEffectiveSettings() const override { return effective_settings_; }
    std::string backendName() const override { return "xiapi"; }

    void open(int device_index = 0) override;
    void start() override;
    void stop() override;
    void close() override;
    bool grab(uint8_t*& data, size_t& size, uint64_t& ts,
              int& width, int& height, int& stride, int timeout_ms = 100) override;
    bool grabPackedInto(uint8_t* dst,
                        size_t dst_capacity,
                        size_t& payload_bytes,
                        uint64_t& ts,
                        int& width,
                        int& height,
                        int& stride,
                        int timeout_ms = 100) override;
    uint64_t lastCameraFrameNumber() const override { return last_frame_number_; }

private:
    HANDLE handle_{nullptr};
    XI_IMG image_{};
    int width_{0}, height_{0};
    int offset_x_{0}, offset_y_{0};
    int stride_bytes_{0};
    int padding_x_{0};
    int image_data_format_{0};
    double exposure_us_{2000.0};
    double gain_db_{0.0};
    bool hardware_trigger_{false};
    int gpi_selector_{1};
    std::string trigger_edge_{"rising"};
    int buffers_queue_size_{16};
    bool running_{false};
    uint64_t last_frame_number_{0};
};

} // namespace cambuffer_recorder_ng

#endif  // HAVE_XIMEA
