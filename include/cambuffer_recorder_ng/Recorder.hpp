#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <memory>

#include "cambuffer_recorder_ng/FakeCamera.hpp"
#include "cambuffer_recorder_ng/FfmpegWriter.hpp"

namespace cambuffer_recorder_ng {

/**
 * Recorder connects a camera to an FFmpeg writer in its own thread.
 */
class Recorder
{
public:
    Recorder() = default;
    ~Recorder() { stop(); }

    bool start(const std::shared_ptr<FakeCamera>& cam,
               const std::string& filename,
               int width, int height, int fps);

    void stop();
    bool is_running() const { return running_; }

private:
    void loop();

    std::shared_ptr<FakeCamera> cam_;
    FfmpegWriter writer_;
    std::string filename_;
    int width_{0}, height_{0}, fps_{0};

    std::thread worker_;
    std::atomic<bool> running_{false};
};

} // namespace cambuffer_recorder_ng

