// xi_raw_rolling.cpp
// Stream RAW8 Bayer from XIMEA as fast as possible into rolling ~2 GiB files.
// Each file has a small file header; each frame has a record header followed by packed payload (width*height bytes).
// Padding bytes (padding_x) are NOT written to disk (we pack active width per row).
//
// Build:  g++ xi_raw_rolling.cpp -O2 -o xi_raw_rolling -I/opt/XIMEA/include -lm3api -pthread
//
// Usage:  ./xi_raw_rolling [width height exposure_us frames rollGiB prefix]
//   width/height default: 2048 x 700
//   exposure_us default: 2000
//   frames default: 10000
//   rollGiB default: 2
//   prefix default: "xi_raw"
//
// Notes:
// - Timestamps use std::chrono::steady_clock (monotonic) in nanoseconds
// - Data format is XI_RAW8 (5). We also record stride (width + padding_x) for reference.
// - File size limit is approximate: we roll as soon as adding next frame would exceed the limit.
//
// File header (at start of each file):
//   magic      = 'XRAW' (0x58524157)
//   version    = 1
//   header_sz  = sizeof(FileHeader)
//   file_index = 0,1,2...
//   start_ns   = monotonic ns when file created
//   w,h        = ROI width/height
//   stride     = width + padding_x (bytes/line in source)
//   data_fmt   = XI_RAW8 (5)
//
// Frame record header (before each frame payload):
//   magic       = 'XBIN' (0x5842494E)
//   version     = 1
//   header_sz   = sizeof(FrameHeader)
//   frame_index = 0..N
//   ts_mono_ns  = per-frame monotonic ns timestamp (when xiGetImage returned)
//   width       = ROI width
//   height      = ROI height
//   stride      = ROI width + padding_x (source stride)
//   payload     = width * height (packed contiguous, no padding)
//   data_fmt    = XI_RAW8 (5)

#include <m3api/xiApi.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>

#ifndef XI_PRM_PADDING_X
#define XI_PRM_PADDING_X "padding_x"
#endif

using namespace std;
using namespace std::chrono;

static atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

// ---------- headers written to disk ----------
#pragma pack(push,1)
struct FileHeader {
    uint32_t magic;       // 'X','R','A','W' = 0x58524157
    uint16_t version;     // 1
    uint16_t header_size; // sizeof(FileHeader)
    uint32_t file_index;  // rolling file index
    uint64_t start_mono_ns;

    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;  // width + padding_x
    uint32_t data_format;   // XI_RAW8 = 5
};

struct FrameHeader {
    uint32_t magic;        // 'X','B','I','N' = 0x5842494E
    uint16_t version;      // 1
    uint16_t header_size;  // sizeof(FrameHeader)
    uint64_t frame_index;
    uint64_t ts_mono_ns;   // steady_clock time when received
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;   // source stride (width + padding_x)
    uint32_t payload_bytes;  // width * height
    uint32_t data_format;    // XI_RAW8 = 5
    uint32_t reserved;       // 0
};
#pragma pack(pop)

static constexpr uint32_t MAGIC_FILE  = 0x58524157; // 'XRAW'
static constexpr uint32_t MAGIC_FRAME = 0x5842494E; // 'XBIN'
static constexpr uint16_t VER_FILE    = 1;
static constexpr uint16_t VER_FRAME   = 1;

// ---------- simple rolling writer ----------
struct RollingWriter {
    string     prefix = "xi_raw";
    uint64_t   roll_bytes = (uint64_t)2 * 1024ULL * 1024ULL * 1024ULL; // ~2 GiB
    uint32_t   width = 0, height = 0, stride = 0, data_fmt = 5;
    FILE*      fp = nullptr;
    uint32_t   file_index = 0;
    uint64_t   bytes_in_file = 0;

    bool open_new_file() {
        if (fp) { fflush(fp); fclose(fp); fp = nullptr; }
        char name[512];
        snprintf(name, sizeof(name), "%s_%04u.xraw", prefix.c_str(), file_index++);

        fp = fopen(name, "wb");
        if (!fp) { perror("fopen"); return false; }

        FileHeader fh{};
        fh.magic       = MAGIC_FILE;
        fh.version     = VER_FILE;
        fh.header_size = (uint16_t)sizeof(FileHeader);
        fh.file_index  = file_index - 1;
        fh.start_mono_ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        fh.width       = width;
        fh.height      = height;
        fh.stride_bytes= stride;
        fh.data_format = data_fmt;

        if (fwrite(&fh, 1, sizeof(fh), fp) != sizeof(fh)) {
            perror("fwrite file header");
            fclose(fp); fp = nullptr;
            return false;
        }
        bytes_in_file = sizeof(fh);
        cout << "[roll] opened " << name << " (w="<<width<<",h="<<height<<",stride="<<stride<<")\n";
        return true;
    }

    bool maybe_roll(uint64_t next_record_bytes) {
        if (!fp) return false;
        if (bytes_in_file + next_record_bytes > roll_bytes) {
            return open_new_file();
        }
        return true;
    }

    bool write_frame(uint64_t frame_index,
                     uint64_t ts_ns,
                     const uint8_t* packed, uint32_t payload_bytes)
    {
        if (!fp) return false;
        FrameHeader rh{};
        rh.magic         = MAGIC_FRAME;
        rh.version       = VER_FRAME;
        rh.header_size   = (uint16_t)sizeof(FrameHeader);
        rh.frame_index   = frame_index;
        rh.ts_mono_ns    = ts_ns;
        rh.width         = width;
        rh.height        = height;
        rh.stride_bytes  = stride;
        rh.payload_bytes = payload_bytes;
        rh.data_format   = data_fmt;
        rh.reserved      = 0;

        const uint64_t total = sizeof(rh) + payload_bytes;
        if (!maybe_roll(total)) return false;

        if (fwrite(&rh, 1, sizeof(rh), fp) != sizeof(rh)) { perror("fwrite rec header"); return false; }
        if (fwrite(packed, 1, payload_bytes, fp) != payload_bytes) { perror("fwrite payload"); return false; }
        bytes_in_file += total;
        return true;
    }

    void close() {
        if (fp) { fflush(fp); fclose(fp); fp = nullptr; }
    }
};

int main(int argc, char** argv)
{
    signal(SIGINT, on_sigint);

    // ---------- CLI ----------
    int width  = (argc > 1) ? atoi(argv[1]) : 2048;
    int height = (argc > 2) ? atoi(argv[2]) : 700;
    int exposure_us = (argc > 3) ? atoi(argv[3]) : 2000;
    uint64_t max_frames = (argc > 4) ? strtoull(argv[4], nullptr, 10) : 10000;
    double rollGiB = (argc > 5) ? atof(argv[5]) : 2.0;
    string prefix = (argc > 6) ? string(argv[6]) : string("xi_raw");

    cout << "RAW8 rolling capture: " << width << "x" << height
         << " exp=" << exposure_us << "us, frames=" << max_frames
         << ", roll≈" << rollGiB << " GiB, prefix=" << prefix << "\n";

    // ---------- Open camera ----------
    HANDLE cam = nullptr;
    XI_RETURN stat = xiOpenDevice(0, &cam);
    if (stat != XI_OK) { cerr << "xiOpenDevice failed: " << stat << "\n"; return 1; }

    // Force RAW8 first, then ROI & exposure
    xiSetParamInt(cam, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(cam, XI_PRM_WIDTH,  width);
    xiSetParamInt(cam, XI_PRM_HEIGHT, height);
    xiSetParamInt(cam, XI_PRM_EXPOSURE, exposure_us);

    // Read back cfg
    int actual_w=0, actual_h=0, fmt=0, padx=0;
    xiGetParamInt(cam, XI_PRM_WIDTH,  &actual_w);
    xiGetParamInt(cam, XI_PRM_HEIGHT, &actual_h);
    xiGetParamInt(cam, XI_PRM_IMAGE_DATA_FORMAT, &fmt);
    if (xiGetParamInt(cam, XI_PRM_PADDING_X, &padx) != XI_OK) padx = 0;
    if (fmt != XI_RAW8) {
        cerr << "Warning: data format not RAW8 (" << fmt << ")\n";
    }
    const int stride = actual_w + padx;

    cout << "XI cfg: width="<<actual_w<<" height="<<actual_h
         <<" exposure(us)="<<exposure_us
         <<" padding_x="<<padx
         <<" data_format="<<fmt<<" (RAW8=5)\n";

    // ---------- Prepare rolling writer ----------
    RollingWriter writer;
    writer.prefix     = prefix;
    writer.roll_bytes = (uint64_t)(rollGiB * 1024.0 * 1024.0 * 1024.0);
    writer.width      = (uint32_t)actual_w;
    writer.height     = (uint32_t)actual_h;
    writer.stride     = (uint32_t)stride;
    writer.data_fmt   = 5; // RAW8

    if (!writer.open_new_file()) {
        xiCloseDevice(cam);
        return 1;
    }

    // ---------- Acquisition ----------
    XI_IMG img{};
    img.size = sizeof(XI_IMG);
    stat = xiStartAcquisition(cam);
    if (stat != XI_OK) { cerr << "xiStartAcquisition failed: " << stat << "\n"; xiCloseDevice(cam); return 1; }

    // packed payload buffer (width * height)
    const size_t payload_bytes = (size_t)actual_w * (size_t)actual_h;
    vector<uint8_t> packed(payload_bytes);

    // timing accumulators
    double sum_grab_ms = 0, sum_write_ms = 0;
    uint64_t frames = 0;

    cout << "==== streaming... (Ctrl+C to stop) ====\n";

    while (!g_stop && frames < max_frames) {
        auto t0 = high_resolution_clock::now();
        stat = xiGetImage(cam, 100, &img);
        if (stat != XI_OK || img.bp == nullptr) {
            cerr << "xiGetImage failed ("<<stat<<")\n";
            break;
        }
        auto t1 = high_resolution_clock::now();

        // pack active width per row (ignore padding)
        const uint8_t* src = static_cast<const uint8_t*>(img.bp);
        uint8_t* dst = packed.data();
        const int w = img.width;
        const int h = img.height;
        const int src_stride = stride; // guaranteed width+padx

        for (int y=0; y<h; ++y) {
            const uint8_t* srow = src + y * src_stride;
            memcpy(dst + (size_t)y * w, srow, (size_t)w);
        }

        auto t2 = high_resolution_clock::now();

        const uint64_t ts_ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();

        // write record header + payload
        if (!writer.write_frame(frames, ts_ns, packed.data(), (uint32_t)payload_bytes)) {
            cerr << "write_frame failed\n";
            break;
        }

        auto t3 = high_resolution_clock::now();

        const double grab_ms  = duration<double, milli>(t1 - t0).count();
        const double write_ms = duration<double, milli>(t3 - t2).count();

        sum_grab_ms  += grab_ms;
        sum_write_ms += write_ms;

        if ((frames % 100) == 0) {
            cout << "Frame " << frames
                 << " grab:" << grab_ms << "ms"
                 << " pack+write:" << write_ms << "ms\n";
        }

        ++frames;
    }

    xiStopAcquisition(cam);
    xiCloseDevice(cam);
    writer.close();

    if (frames > 0) {
        const double avg_g = sum_grab_ms / frames;
        const double avg_w = sum_write_ms / frames;
        const double tot   = avg_g + avg_w;
        cout << "\nFrames: " << frames
             << "  Averages -> grab " << avg_g << " ms, write " << avg_w
             << " ms, total " << tot
             << " ms  => " << (1000.0 / tot) << " FPS\n";
    }

    cout << "done.\n";
    return 0;
}

