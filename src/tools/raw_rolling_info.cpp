#include "cambuffer_recorder_ng/raw/RawRollingFormat.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace cambuffer_recorder_ng;

static std::string pixelFormatName(uint32_t fmt)
{
    switch (static_cast<RawPixelFormat>(fmt)) {
        case RawPixelFormat::RAW8_BAYER_GBRG: return "RAW8_BAYER_GBRG";
        case RawPixelFormat::RAW8_MONO: return "RAW8_MONO";
        default: return "UNKNOWN(" + std::to_string(fmt) + ")";
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: raw_rolling_info <file.cbrraw>\n";
        return 2;
    }

    FILE* fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 1; }

    RawRollingFileHeader fh{};
    if (fread(&fh, 1, sizeof(fh), fp) != sizeof(fh)) {
        std::cerr << "Could not read file header\n";
        fclose(fp);
        return 1;
    }

    if (fh.magic != CBRRAW_FILE_MAGIC) {
        std::cerr << "Bad file magic. Not a cbrraw file?\n";
        fclose(fp);
        return 1;
    }

    std::cout << "file: " << argv[1] << "\n";
    std::cout << "version: " << fh.version << "\n";
    std::cout << "file_index: " << fh.file_index << "\n";
    std::cout << "run_start_utc_ns: " << fh.run_start_utc_ns << "\n";
    std::cout << "file_start_utc_ns: " << fh.file_start_utc_ns << "\n";
    std::cout << "width: " << fh.width << "\n";
    std::cout << "height: " << fh.height << "\n";
    std::cout << "source_stride_bytes: " << fh.source_stride_bytes << "\n";
    std::cout << "packed_stride_bytes: " << fh.packed_stride_bytes << "\n";
    std::cout << "pixel_format: " << pixelFormatName(fh.pixel_format) << "\n";

    uint64_t frame_count = 0;
    uint64_t first_utc = 0, last_utc = 0;
    uint64_t first_idx = 0, last_idx = 0;
    uint64_t first_camera_frame_number = 0, last_camera_frame_number = 0;
    uint64_t camera_frame_number_gaps = 0, total_camera_frame_number_gap = 0;

    while (true) {
        RawRollingFrameHeader rh{};
        const size_t got = fread(&rh, 1, sizeof(rh), fp);
        if (got == 0) break;
        if (got != sizeof(rh)) {
            std::cerr << "Partial frame header at frame " << frame_count << "\n";
            break;
        }
        if (rh.magic != CBRRAW_FRAME_MAGIC) {
            std::cerr << "Bad frame magic at frame " << frame_count << "\n";
            break;
        }
        if (frame_count == 0) {
            first_utc = rh.pc_utc_ns;
            first_idx = rh.frame_index;
            first_camera_frame_number = rh.camera_frame_number;
        } else if (rh.camera_frame_number > 0 && last_camera_frame_number > 0) {
            if (rh.camera_frame_number > last_camera_frame_number + 1) {
                ++camera_frame_number_gaps;
                total_camera_frame_number_gap += rh.camera_frame_number - last_camera_frame_number - 1;
            }
        }
        last_utc = rh.pc_utc_ns;
        last_idx = rh.frame_index;
        last_camera_frame_number = rh.camera_frame_number;
        ++frame_count;
        if (fseek(fp, static_cast<long>(rh.payload_bytes), SEEK_CUR) != 0) {
            std::cerr << "Could not seek past payload at frame " << frame_count << "\n";
            break;
        }
    }

    std::cout << "frames: " << frame_count << "\n";
    if (frame_count > 0) {
        std::cout << "first_frame_index: " << first_idx << "\n";
        std::cout << "last_frame_index: " << last_idx << "\n";
        std::cout << "first_pc_utc_ns: " << first_utc << "\n";
        std::cout << "last_pc_utc_ns: " << last_utc << "\n";
        std::cout << "first_camera_frame_number: " << first_camera_frame_number << "\n";
        std::cout << "last_camera_frame_number: " << last_camera_frame_number << "\n";
        std::cout << "camera_frame_number_gaps: " << camera_frame_number_gaps << "\n";
        std::cout << "total_camera_frame_number_gap: " << total_camera_frame_number_gap << "\n";
        if (last_utc > first_utc && frame_count > 1) {
            const double elapsed_s = static_cast<double>(last_utc - first_utc) / 1e9;
            std::cout << "duration_s: " << elapsed_s << "\n";
            std::cout << "mean_fps_by_utc: " << static_cast<double>(frame_count - 1) / elapsed_s << "\n";
        }
    }

    fclose(fp);
    return 0;
}
