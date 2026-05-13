#include "cambuffer_recorder_ng/FfmpegWriter.hpp"
#include "cambuffer_recorder_ng/raw/RawRollingFormat.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace cambuffer_recorder_ng;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "Usage: raw_rolling_to_mp4 <input.cbrraw> <output.mp4> [max_frames] [fps]\n";
        return 2;
    }

    const std::string input = argv[1];
    const std::string output = argv[2];
    const uint64_t max_frames = (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 0;
    const int fps = (argc > 4) ? std::atoi(argv[4]) : 30;

    FILE* fp = fopen(input.c_str(), "rb");
    if (!fp) { perror("fopen"); return 1; }

    RawRollingFileHeader fh{};
    if (fread(&fh, 1, sizeof(fh), fp) != sizeof(fh) || fh.magic != CBRRAW_FILE_MAGIC) {
        std::cerr << "Bad or missing cbrraw file header\n";
        fclose(fp);
        return 1;
    }

    FfmpegWriter writer;
    if (!writer.open(output, static_cast<int>(fh.width), static_cast<int>(fh.height), fps, "libx264")) {
        std::cerr << "Could not open output movie " << output << "\n";
        fclose(fp);
        return 1;
    }

    std::vector<uint8_t> raw(static_cast<size_t>(fh.width) * static_cast<size_t>(fh.height));
    cv::Mat raw_mat(static_cast<int>(fh.height), static_cast<int>(fh.width), CV_8UC1, raw.data());
    cv::Mat rgb;

    uint64_t written = 0;
    while (max_frames == 0 || written < max_frames) {
        RawRollingFrameHeader rh{};
        const size_t got = fread(&rh, 1, sizeof(rh), fp);
        if (got == 0) break;
        if (got != sizeof(rh) || rh.magic != CBRRAW_FRAME_MAGIC) {
            std::cerr << "Bad frame header after " << written << " frames\n";
            break;
        }
        if (rh.payload_bytes != raw.size()) {
            std::cerr << "Unexpected payload size " << rh.payload_bytes << ", expected " << raw.size() << "\n";
            break;
        }
        if (fread(raw.data(), 1, raw.size(), fp) != raw.size()) {
            std::cerr << "Short payload after " << written << " frames\n";
            break;
        }

        // XIMEA default requested here: Bayer GBRG. OpenCV BayerGB2RGB corresponds to GBRG ordering.
        if (rh.pixel_format == static_cast<uint32_t>(RawPixelFormat::RAW8_BAYER_GBRG)) {
            cv::cvtColor(raw_mat, rgb, cv::COLOR_BayerGB2RGB);
        } else {
            cv::cvtColor(raw_mat, rgb, cv::COLOR_GRAY2RGB);
        }

        writer.write_frame(rgb.data, static_cast<int>(rgb.step));
        ++written;
    }

    writer.close();
    fclose(fp);
    std::cerr << "wrote " << written << " frames to " << output << "\n";
    return 0;
}
