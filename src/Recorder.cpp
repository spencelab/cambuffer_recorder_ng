#include "cambuffer_recorder_ng/Recorder.hpp"
#include <iostream>

namespace cambuffer_recorder_ng {

bool Recorder::start(std::function<bool(uint8_t*&, size_t&, uint64_t&, int&, int&, int&)> grab_fn,
                     const std::string& filename, int width, int height, int fps)
{
    if (running_) return false;
    grab_fn_ = std::move(grab_fn);
    filename_ = filename;
    width_ = width;
    height_ = height;
    fps_ = fps;

    if (!writer_.open(filename_, width_, height_, fps_, "libx264")) {
        std::cerr << "Recorder: failed to open FFmpeg writer\n";
        return false;
    }

    running_ = true;
    worker_ = std::thread(&Recorder::loop, this);
    return true;
}

void Recorder::loop()
{
    uint8_t* data = nullptr;
    size_t size = 0;
    uint64_t ts = 0;
    int w = 0, h = 0, stride = 0;

    while (running_) {
        if (!grab_fn_) break;

        bool ok = false;
        try {
            ok = grab_fn_(data, size, ts, w, h, stride);
        } catch (...) {
            ok = false;
        }

        if (!ok) continue;

        writer_.write_frame(data, stride);
    }

    writer_.close();
}

void Recorder::stop()
{
    running_ = false;
    if (worker_.joinable()) worker_.join();
    writer_.close();
}

} // namespace cambuffer_recorder_ng

