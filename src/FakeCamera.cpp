#include "cambuffer_recorder_ng/FakeCamera.hpp"
#include <thread>

using namespace std::chrono;

namespace cambuffer_recorder_ng {

FakeCamera::FakeCamera(int w, int h, int fps)
    : width_(w), height_(h), fps_(fps), buffer_(w*h, 0)
{}

void FakeCamera::open()  { running_ = false; }
void FakeCamera::close() { running_ = false; }
void FakeCamera::start() { running_ = true; last_ts_ = steady_clock::now(); }
void FakeCamera::stop()  { running_ = false; }

bool FakeCamera::grab(uint8_t*& data, int& w, int& h, int& stride, uint64_t& ts)
{
    if (!running_) return false;
    auto now = steady_clock::now();
    auto dt = duration_cast<milliseconds>(now - last_ts_).count();
    if (dt < 1000 / fps_) std::this_thread::sleep_for(milliseconds(1000 / fps_ - dt));
    last_ts_ = steady_clock::now();

    // generate simple gradient
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x)
            buffer_[y * width_ + x] = static_cast<uint8_t>((x + y + frame_idx_) % 256);

    data = buffer_.data();
    w = width_;
    h = height_;
    stride = width_;
    ts = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    frame_idx_++;
    return true;
}

} // namespace cambuffer_recorder_ng

