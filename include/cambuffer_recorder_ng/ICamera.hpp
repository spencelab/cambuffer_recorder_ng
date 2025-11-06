#pragma once
#include <cstdint>
#include <cstddef>
#include <stdexcept>

namespace cambuffer_recorder_ng {

class ICamera {
public:
    virtual ~ICamera() = default;

    virtual void open(int device_index = 0) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void close() {}

    virtual bool grab(uint8_t*& data, size_t& size, uint64_t& ts,
                      int& width, int& height, int& stride, int timeout_ms = 100) = 0;
};

} // namespace cambuffer_recorder_ng

