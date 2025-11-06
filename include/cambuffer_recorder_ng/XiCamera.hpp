#pragma once
#include "cambuffer_recorder_ng/ICamera.hpp"
#include <m3api/xiApi.h>
#include <cstdint>
#include <cstddef>

namespace cambuffer_recorder_ng {

class XiCamera : public ICamera {
public:
    XiCamera() = default;
    ~XiCamera() override = default;

    void open(int device_index = 0) override;
    void start() override;
    void stop() override;
    void close() override;
    bool grab(uint8_t*& data, size_t& size, uint64_t& ts,
              int& width, int& height, int& stride, int timeout_ms = 100) override;

private:
    HANDLE handle_{nullptr};
    XI_IMG image_{};
    int width_{0}, height_{0};
    bool running_{false};
};

} // namespace cambuffer_recorder_ng

