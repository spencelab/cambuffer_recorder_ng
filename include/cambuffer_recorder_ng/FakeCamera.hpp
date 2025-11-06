#pragma once
#include <vector>
#include <cstdint>
#include <chrono>

namespace cambuffer_recorder_ng {

class FakeCamera {
public:
    FakeCamera(int width, int height, int fps);
    void open();
    void close();
    void start();
    void stop();
    bool grab(uint8_t*& data, int& width, int& height, int& stride, uint64_t& timestamp_ns);

private:
    int width_;
    int height_;
    int fps_;
    std::vector<uint8_t> buffer_;
    bool running_{false};
    uint64_t frame_idx_{0};
    std::chrono::steady_clock::time_point last_ts_;
};

} // namespace cambuffer_recorder_ng

