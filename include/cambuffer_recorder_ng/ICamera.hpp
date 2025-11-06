#pragma once
#include <cstddef>
#include <string>
#include <cstdint>

namespace cambuffer_recorder_ng {

struct FrameView {
  uint8_t* data;
  size_t size_bytes;
  int width, height, stride;
  uint64_t timestamp_ns;
};

class ICamera {
public:
  virtual ~ICamera() = default;
  virtual void open() = 0;
  virtual void close() = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual bool grab(FrameView& out, int timeout_ms) = 0;
};

} // namespace cambuffer_recorder_ng

