/*
g++ xi_grab_debayer_ffmpeg_timing.cpp -o xi_grab_debayer_ffmpeg_timing \
    -I/opt/XIMEA/include -lm3api \
    $(pkg-config --cflags --libs libavcodec libavformat libavutil libswscale) \
    -pthread
*/

#include "m3api/xiApi.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

// ----------------------------------------------------
// Simple DebayerHalf (RGGB -> grayscale 8-bit)
// ----------------------------------------------------
inline void debayer_half(const uint8_t* src, int src_w, int src_h, int src_stride,
                         std::vector<uint8_t>& dst)
{
    int dst_w = src_w / 2;
    int dst_h = src_h / 2;
    dst.resize(dst_w * dst_h);

    for (int y = 0; y < dst_h; ++y) {
        const uint8_t* row0 = src + (2 * y) * src_stride;
        const uint8_t* row1 = row0 + src_stride;
        for (int x = 0; x < dst_w; ++x) {
            int r = row0[2 * x];
            int g1 = row0[2 * x + 1];
            int g2 = row1[2 * x];
            int b = row1[2 * x + 1];
            dst[y * dst_w + x] = static_cast<uint8_t>((r + g1 + g2 + b) / 4);
        }
    }
}

// ----------------------------------------------------
// FFmpeg Writer setup
// ----------------------------------------------------
struct FFmpegWriter {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVStream* stream = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int frame_index = 0;
    int width = 0, height = 0;

    bool open(const std::string& path, int w, int h, int fps) {
        width = w;
        height = h;
        avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr, path.c_str());
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        codec_ctx = avcodec_alloc_context3(codec);
        codec_ctx->bit_rate = 4000000;
        codec_ctx->width = w;
        codec_ctx->height = h;
        codec_ctx->time_base = {1, fps};
        codec_ctx->framerate = {fps, 1};
        codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        codec_ctx->gop_size = 12;
        codec_ctx->max_b_frames = 2;
        av_opt_set(codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(codec_ctx->priv_data, "tune", "zerolatency", 0);

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cerr << "Could not open codec\n";
            return false;
        }

        stream = avformat_new_stream(fmt_ctx, nullptr);
        avcodec_parameters_from_context(stream->codecpar, codec_ctx);
        stream->time_base = codec_ctx->time_base;

        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_open(&fmt_ctx->pb, path.c_str(), AVIO_FLAG_WRITE);

        avformat_write_header(fmt_ctx, nullptr);

        sws_ctx = sws_getContext(w, h, AV_PIX_FMT_GRAY8,
                                 w, h, AV_PIX_FMT_YUV420P,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = w;
        frame->height = h;
        av_frame_get_buffer(frame, 32);

        pkt = av_packet_alloc();
        return true;
    }

    void encode_frame(const std::vector<uint8_t>& gray) {
        const uint8_t* src_planes[1] = {gray.data()};
        int src_stride[1] = {width};

        sws_scale(sws_ctx, src_planes, src_stride, 0, height,
                  frame->data, frame->linesize);
        frame->pts = frame_index++;

        if (avcodec_send_frame(codec_ctx, frame) >= 0) {
            while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
                av_interleaved_write_frame(fmt_ctx, pkt);
                av_packet_unref(pkt);
            }
        }
    }

    void close() {
        avcodec_send_frame(codec_ctx, nullptr);
        while (avcodec_receive_packet(codec_ctx, pkt) == 0)
            av_interleaved_write_frame(fmt_ctx, pkt);
        av_write_trailer(fmt_ctx);
        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_close(fmt_ctx->pb);
        avcodec_free_context(&codec_ctx);
        avformat_free_context(fmt_ctx);
        sws_freeContext(sws_ctx);
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }
};

// ----------------------------------------------------
// Main timing test
// ----------------------------------------------------
int main() {
    HANDLE h = nullptr;
    XI_RETURN stat = xiOpenDevice(0, &h);
    if (stat != XI_OK) {
        std::cerr << "Failed to open camera: " << stat << std::endl;
        return -1;
    }

    // ROI and settings
    xiSetParamInt(h, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(h, XI_PRM_WIDTH, 2048);
    xiSetParamInt(h, XI_PRM_HEIGHT, 700);
    xiSetParamInt(h, XI_PRM_EXPOSURE, 2000);

    xiStartAcquisition(h);
    XI_IMG img = {sizeof(XI_IMG)};
    std::vector<uint8_t> debayered;

    FFmpegWriter writer;
    int out_w = 1024, out_h = 350, fps = 300;
    writer.open("xi_debayer_ffmpeg_test.mp4", out_w, out_h, fps);

    const int N = 200;
    double t_grab = 0, t_proc = 0, t_enc = 0;
    int frames = 0;

    for (int i = 0; i < N; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        if (xiGetImage(h, 100, &img) == XI_OK) {
            auto t1 = std::chrono::high_resolution_clock::now();
            int stride = img.width + img.padding_x;
            debayer_half(static_cast<uint8_t*>(img.bp),
                         img.width, img.height, stride, debayered);
            auto t2 = std::chrono::high_resolution_clock::now();
            writer.encode_frame(debayered);
            auto t3 = std::chrono::high_resolution_clock::now();

            t_grab += std::chrono::duration<double, std::milli>(t1 - t0).count();
            t_proc += std::chrono::duration<double, std::milli>(t2 - t1).count();
            t_enc  += std::chrono::duration<double, std::milli>(t3 - t2).count();
            frames++;
        }
    }

    writer.close();
    xiStopAcquisition(h);
    xiCloseDevice(h);

    std::cout << "\nCaptured " << frames << " frames\n";
    std::cout << "Avg grab: " << t_grab / frames << " ms\n";
    std::cout << "Avg process: " << t_proc / frames << " ms\n";
    std::cout << "Avg encode: " << t_enc / frames << " ms\n";
    std::cout << "Total FPS: "
              << 1000.0 / ((t_grab + t_proc + t_enc) / frames)
              << "\n";

    return 0;
}

