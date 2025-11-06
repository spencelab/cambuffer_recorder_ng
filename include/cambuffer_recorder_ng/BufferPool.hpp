#pragma once
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstdint>

namespace cambuffer_recorder_ng {

/**
 * @brief Simple thread-safe pool of fixed-size buffers.
 *
 * Allocates N blocks of `frame_bytes` bytes each. Threads can acquire()
 * and release() buffers without dynamic allocation. Designed for producer/
 * consumer pipelines like the recorder.
 */
class BufferPool {
public:
    BufferPool() = default;
    BufferPool(size_t frame_bytes, size_t capacity) { allocate(frame_bytes, capacity); }

    void allocate(size_t frame_bytes, size_t capacity);

    /// Get a pointer to a free buffer (blocking if none available).
    uint8_t* acquire();

    /// Return a previously acquired buffer to the pool.
    void release(uint8_t* buf);

    size_t capacity() const { return buffers_.size(); }
    size_t frame_bytes() const { return frame_bytes_; }
    size_t available() const;

private:
    size_t frame_bytes_ = 0;
    std::vector<std::vector<uint8_t>> buffers_;   // actual storage
    std::queue<uint8_t*> free_;                   // pointers to free blocks
    mutable std::mutex mtx_;
    std::condition_variable cv_;
};

} // namespace cambuffer_recorder_ng

