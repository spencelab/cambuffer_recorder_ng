#include "cambuffer_recorder_ng/BufferPool.hpp"

namespace cambuffer_recorder_ng {

void BufferPool::allocate(size_t frame_bytes, size_t capacity)
{
    std::lock_guard<std::mutex> lock(mtx_);
    frame_bytes_ = frame_bytes;
    buffers_.resize(capacity);
    while (!free_.empty()) free_.pop();
    for (auto& b : buffers_) {
        b.resize(frame_bytes);
        free_.push(b.data());
    }
}

uint8_t* BufferPool::acquire()
{
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [&]{ return !free_.empty(); });
    uint8_t* p = free_.front();
    free_.pop();
    return p;
}

void BufferPool::release(uint8_t* buf)
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        free_.push(buf);
    }
    cv_.notify_one();
}

size_t BufferPool::available() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return free_.size();
}

} // namespace cambuffer_recorder_ng

