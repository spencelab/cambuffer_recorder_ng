#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <string>
#include "cambuffer_recorder_ng/FfmpegWriter.hpp"

namespace cambuffer_recorder_ng {

class Recorder {
public:
    Recorder() = default;
    ~Recorder() { stop(); }

    // Updated grab callback: (data, size, ts, w, h, stride)
    bool start(std::function<bool(uint8_t*&, size_t&, uint64_t&, int&, int&, int&)> grab_fn,
               const std::string& filename,
               int width, int height, int fps);

    void stop();

private:
    void loop();

    std::thread worker_;
    std::atomic<bool> running_{false};

    std::function<bool(uint8_t*&, size_t&, uint64_t&, int&, int&, int&)> grab_fn_;
    FfmpegWriter writer_;
    std::string filename_;
    int width_ = 0, height_ = 0, fps_ = 0;
};

} // namespace cambuffer_recorder_ng

