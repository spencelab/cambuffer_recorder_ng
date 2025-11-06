#include "cambuffer_recorder_ng/Recorder.hpp"
#include <iostream>
#include <chrono>
#include <thread>

using namespace std::chrono;

namespace cambuffer_recorder_ng
{

bool Recorder::start(const std::shared_ptr<FakeCamera>& cam,
                     const std::string& filename,
                     int width, int height, int fps)
{
    if (running_) return false;

    cam_ = cam;
    filename_ = filename;
    width_ = width;
    height_ = height;
    fps_ = fps;

    if (!writer_.open(filename_, width_, height_, fps_, "libx264")) {
        std::cerr << "Recorder: failed to open FFmpeg writer for " << filename_ << "\n";
        return false;
    }

    running_ = true;
    worker_ = std::thread(&Recorder::loop, this);
    return true;
}

void Recorder::stop()
{
    running_ = false;
    if (worker_.joinable())
        worker_.join();
    writer_.close();
}

void Recorder::loop()
{
    uint8_t* data;
    int w, h, stride;
    uint64_t ts;

    while (running_ && cam_ && cam_->grab(data, w, h, stride, ts)) {
        writer_.write_frame(data, stride);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace cambuffer_recorder_ng

