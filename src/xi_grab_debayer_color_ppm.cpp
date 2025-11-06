//g++ xi_grab_debayer_color_ppm.cpp -o xi_grab_debayer_color_ppm -I/opt/XIMEA/include -lm3api -pthread
//./xi_grab_debayer_color_ppm --bayer=BGGR
#include "m3api/xiApi.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <cstring>

enum class BayerPattern { RGGB, GRBG, GBRG, BGGR };


// ----------------------------------------------------
// 2×2 DebayerHalfColor: RGGB → RGB packed (8-bit)
// ----------------------------------------------------
inline void debayer_half_color_rggb(const uint8_t* src, int src_w, int src_h, int src_stride,
                               std::vector<uint8_t>& dst)
{
    int dst_w = src_w / 2;
    int dst_h = src_h / 2;
    dst.resize(dst_w * dst_h * 3);

    for (int y = 0; y < dst_h; ++y) {
        const uint8_t* row0 = src + (2 * y) * src_stride;
        const uint8_t* row1 = row0 + src_stride;
        uint8_t* out = dst.data() + y * dst_w * 3;

        for (int x = 0; x < dst_w; ++x) {
            uint8_t r = row0[2 * x];           // R
            uint8_t g1 = row0[2 * x + 1];      // G top
            uint8_t g2 = row1[2 * x];          // G bottom
            uint8_t b = row1[2 * x + 1];       // B
            uint8_t g = static_cast<uint8_t>((g1 + g2) / 2);

            out[3 * x + 0] = r;
            out[3 * x + 1] = g;
            out[3 * x + 2] = b;
        }
    }
}

// ----------------------------------------------------
// 2×2 DebayerHalfColor: GRBG → RGB packed (8-bit)
// ----------------------------------------------------
inline void debayer_half_color_grbg(const uint8_t* src, int src_w, int src_h, int src_stride,
                               std::vector<uint8_t>& dst)
{
    // GRBG pattern:
    // G R
    // B G

    int dst_w = src_w / 2;
    int dst_h = src_h / 2;
    dst.resize(dst_w * dst_h * 3);

    for (int y = 0; y < dst_h; ++y) {
        const uint8_t* row0 = src + (2 * y) * src_stride;
        const uint8_t* row1 = row0 + src_stride;
        uint8_t* out = dst.data() + y * dst_w * 3;

        for (int x = 0; x < dst_w; ++x) {
            uint8_t g1 = row0[2 * x];          // G (top-left)
            uint8_t r  = row0[2 * x + 1];      // R (top-right)
            uint8_t b  = row1[2 * x];          // B (bottom-left)
            uint8_t g2 = row1[2 * x + 1];      // G (bottom-right)
            uint8_t g  = static_cast<uint8_t>((g1 + g2) / 2);

            out[3 * x + 0] = r;  // R
            out[3 * x + 1] = g;  // G
            out[3 * x + 2] = b;  // B
        }
    }
}

// ----------------------------------------------------
// GENERAL
// ----------------------------------------------------
inline void debayer_half_color(const uint8_t* src, int src_w, int src_h, int src_stride,
                               std::vector<uint8_t>& dst, BayerPattern pattern)
{
    int dst_w = src_w / 2;
    int dst_h = src_h / 2;
    dst.resize(dst_w * dst_h * 3);

    for (int y = 0; y < dst_h; ++y) {
        const uint8_t* row0 = src + (2 * y) * src_stride;
        const uint8_t* row1 = row0 + src_stride;
        uint8_t* out = dst.data() + y * dst_w * 3;

        for (int x = 0; x < dst_w; ++x) {
            uint8_t r, g, b;

            switch (pattern) {
            case BayerPattern::RGGB: {
                r  = row0[2*x];
                uint8_t g1 = row0[2*x+1];
                uint8_t g2 = row1[2*x];
                b  = row1[2*x+1];
                g  = static_cast<uint8_t>((g1 + g2) / 2);
                break;
            }
            case BayerPattern::GRBG: {
                uint8_t g1 = row0[2*x];
                r  = row0[2*x+1];
                b  = row1[2*x];
                uint8_t g2 = row1[2*x+1];
                g  = static_cast<uint8_t>((g1 + g2) / 2);
                break;
            }
            case BayerPattern::GBRG: {
                uint8_t g1 = row0[2*x];
                b  = row0[2*x+1];
                r  = row1[2*x];
                uint8_t g2 = row1[2*x+1];
                g  = static_cast<uint8_t>((g1 + g2) / 2);
                break;
            }
            case BayerPattern::BGGR: {
                b  = row0[2*x];
                uint8_t g1 = row0[2*x+1];
                uint8_t g2 = row1[2*x];
                r  = row1[2*x+1];
                g  = static_cast<uint8_t>((g1 + g2) / 2);
                break;
            }
            }

            out[3*x + 0] = r;
            out[3*x + 1] = g;
            out[3*x + 2] = b;
        }
    }
}



// ----------------------------------------------------
// Save RGB image as binary PPM
// ----------------------------------------------------
void save_ppm(const std::string& fname, const std::vector<uint8_t>& rgb,
              int w, int h)
{
    std::ofstream f(fname, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), w * h * 3);
    f.close();
}

// ----------------------------------------------------
// Main: grab a few frames and dump as PPM
// ----------------------------------------------------
int main(int argc, char** argv)
{
    BayerPattern pattern = BayerPattern::GBRG; // default

    // Parse from command line
    if (argc > 1) {
        std::string arg = argv[1];
        if      (arg == "--bayer=GRBG") pattern = BayerPattern::GRBG;
        else if (arg == "--bayer=GBRG") pattern = BayerPattern::GBRG;
        else if (arg == "--bayer=BGGR") pattern = BayerPattern::BGGR;
        else if (arg == "--bayer=RGGB") pattern = BayerPattern::RGGB;
        std::cout << "Using Bayer pattern: " << arg.substr(8) << std::endl;
    }

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

    const int N = 10;  // dump 5 frames
    std::vector<uint8_t> rgb;
    for (int i = 0; i < N; ++i) {
        if (xiGetImage(h, 200, &img) == XI_OK) {
            int stride = img.width + img.padding_x;
            auto t0 = std::chrono::high_resolution_clock::now();
            debayer_half_color(static_cast<uint8_t*>(img.bp),
                               img.width, img.height, stride, rgb, pattern);
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::ostringstream name;
            name << "frame_" << std::setw(3) << std::setfill('0') << i << ".ppm";
            save_ppm(name.str(), rgb, img.width / 2, img.height / 2);
            std::cout << "Saved " << name.str()
                      << " (" << img.width/2 << "x" << img.height/2
                      << ")  debayer: " << ms << " ms\n";
        } else {
            std::cerr << "Timeout getting image\n";
        }
    }

    xiStopAcquisition(h);
    xiCloseDevice(h);
    std::cout << "Done.\n";
    return 0;
}

