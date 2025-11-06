#include "cambuffer_recorder_ng/FfmpegWriter.hpp"
#include <iostream>
#include "rclcpp/rclcpp.hpp"

namespace cambuffer_recorder_ng {

bool FfmpegWriter::open(const std::string& filename,
                        int width,
                        int height,
                        int fps,
                        const std::string& codec_name)
{
    std::lock_guard<std::mutex> lock(mtx_);
    width_ = width; height_ = height; fps_ = fps;
    frame_index_ = 0;

    avformat_alloc_output_context2(&fmt_ctx_, nullptr, nullptr, filename.c_str());
    if (!fmt_ctx_) {
        std::cerr << "FFmpeg: could not allocate output context.\n";
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
    if (!codec) {
        std::cerr << "FFmpeg: codec not found: " << codec_name << "\n";
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    codec_ctx_->codec_id = codec->id;
    codec_ctx_->width = width_;
    codec_ctx_->height = height_;
    codec_ctx_->time_base = {1, fps_};
    codec_ctx_->framerate = {fps_, 1};
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_->bit_rate = 8'000'000;  // 8 Mbps

    if (fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER)
        codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        std::cerr << "FFmpeg: could not open codec.\n";
        return false;
    }

    stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    stream_->id = fmt_ctx_->nb_streams - 1;
    stream_->time_base = codec_ctx_->time_base;
    avcodec_parameters_from_context(stream_->codecpar, codec_ctx_);

    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt_ctx_->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::cerr << "FFmpeg: could not open output file " << filename << "\n";
            return false;
        }
    }

    if (avformat_write_header(fmt_ctx_, nullptr) < 0)
    RCLCPP_WARN(rclcpp::get_logger("FfmpegWriter"), "Failed to write FFmpeg header");

    // Prepare scaling context from RGB24 to YUV420P
    sws_ctx_ = sws_getContext(width_, height_, AV_PIX_FMT_RGB24,
                              width_, height_, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);

    frame_yuv_ = av_frame_alloc();
    frame_yuv_->format = AV_PIX_FMT_YUV420P;
    frame_yuv_->width = width_;
    frame_yuv_->height = height_;
    av_frame_get_buffer(frame_yuv_, 32);

    pkt_ = av_packet_alloc();
    return true;
}

bool FfmpegWriter::write_frame(const uint8_t* rgb_data, int stride_bytes, int64_t pts_ns)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!fmt_ctx_ || !codec_ctx_) return false;

    const uint8_t* src_slices[1] = { rgb_data };
    int src_stride[1] = { stride_bytes };
    sws_scale(sws_ctx_, src_slices, src_stride, 0, height_,
              frame_yuv_->data, frame_yuv_->linesize);

    frame_yuv_->pts = frame_index_++;

    if (avcodec_send_frame(codec_ctx_, frame_yuv_) < 0) return false;

    while (avcodec_receive_packet(codec_ctx_, pkt_) == 0) {
        av_packet_rescale_ts(pkt_, codec_ctx_->time_base, stream_->time_base);
        pkt_->stream_index = stream_->index;
        av_interleaved_write_frame(fmt_ctx_, pkt_);
        av_packet_unref(pkt_);
    }
    return true;
}

void FfmpegWriter::close()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!fmt_ctx_) return;

    avcodec_send_frame(codec_ctx_, nullptr);
    while (avcodec_receive_packet(codec_ctx_, pkt_) == 0) {
        av_packet_rescale_ts(pkt_, codec_ctx_->time_base, stream_->time_base);
        pkt_->stream_index = stream_->index;
        av_interleaved_write_frame(fmt_ctx_, pkt_);
        av_packet_unref(pkt_);
    }

    av_write_trailer(fmt_ctx_);

    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE))
        avio_closep(&fmt_ctx_->pb);

    av_frame_free(&frame_yuv_);
    av_packet_free(&pkt_);
    sws_freeContext(sws_ctx_);
    avcodec_free_context(&codec_ctx_);
    avformat_free_context(fmt_ctx_);

    fmt_ctx_ = nullptr;
    codec_ctx_ = nullptr;
    stream_ = nullptr;
    sws_ctx_ = nullptr;
    frame_yuv_ = nullptr;
    pkt_ = nullptr;
}

} // namespace cambuffer_recorder_ng

