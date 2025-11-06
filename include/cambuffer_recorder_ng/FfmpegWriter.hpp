#pragma once
#include <string>
#include <cstdint>
#include <mutex>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace cambuffer_recorder_ng {

/**
 * @brief Simple wrapper around FFmpeg for encoding RGB frames to video.
 *
 * Usage:
 *   FfmpegWriter writer;
 *   writer.open("out.mp4", 1024, 350, 100, "libx264");
 *   writer.write_frame(rgb_ptr, rgb_stride_bytes);
 *   writer.close();
 */
class FfmpegWriter {
public:
    FfmpegWriter() = default;
    ~FfmpegWriter() { close(); }

    bool open(const std::string& filename,
              int width,
              int height,
              int fps,
              const std::string& codec_name = "libx264");

    bool write_frame(const uint8_t* rgb_data, int stride_bytes, int64_t pts_ns = 0);
    void close();

    bool is_open() const { return fmt_ctx_ != nullptr; }

private:
    std::mutex mtx_;
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVStream* stream_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVFrame* frame_yuv_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int width_ = 0, height_ = 0, fps_ = 0;
    int64_t frame_index_ = 0;
};

} // namespace cambuffer_recorder_ng

