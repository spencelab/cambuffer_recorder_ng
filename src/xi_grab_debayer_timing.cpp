#include "m3api/xiApi.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <cstring>

// ----------------------------------------------------
// Half-resolution debayer (RGGB → grayscale demo)
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
// Query and print basic camera parameters
// ----------------------------------------------------
void print_camera_info(HANDLE h)
{
    char model[256] = {0}, serial[256] = {0};
    xiGetParamString(h, XI_PRM_DEVICE_NAME, model, sizeof(model));
    xiGetParamString(h, XI_PRM_DEVICE_SN, serial, sizeof(serial));

    int width = 0, height = 0, offset_x = 0, offset_y = 0;
    int exposure = 0;
    float fps = 0;
    xiGetParamInt(h, XI_PRM_WIDTH, &width);
    xiGetParamInt(h, XI_PRM_HEIGHT, &height);
    xiGetParamInt(h, XI_PRM_OFFSET_X, &offset_x);
    xiGetParamInt(h, XI_PRM_OFFSET_Y, &offset_y);
    xiGetParamInt(h, XI_PRM_EXPOSURE, &exposure);
    xiGetParamFloat(h, XI_PRM_FRAMERATE, &fps);

    std::cout << "\n=== XIMEA Camera Info ===\n";
    std::cout << "Model: " << model << "  SN: " << serial << "\n";
    std::cout << "ROI: " << width << "x" << height << " @ (" << offset_x << "," << offset_y << ")\n";
    std::cout << "Exposure: " << exposure << " us   FPS: " << fps << "\n";
    std::cout << "==========================\n\n";
}

// ----------------------------------------------------
// Main
// ----------------------------------------------------
int main()
{
    HANDLE h = nullptr;
    XI_RETURN stat = xiOpenDevice(0, &h);
    if (stat != XI_OK) {
        std::cerr << "Failed to open camera, code: " << stat << std::endl;
        return -1;
    }

    // --- Force ROI and settings ---
    xiSetParamInt(h, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(h, XI_PRM_WIDTH, 2048);
    xiSetParamInt(h, XI_PRM_HEIGHT, 700);
    xiSetParamInt(h, XI_PRM_EXPOSURE, 2000);
    xiSetParamFloat(h, XI_PRM_FRAMERATE, 250.0f);

    print_camera_info(h);

    xiStartAcquisition(h);

    XI_IMG img = {sizeof(XI_IMG)};
    std::vector<uint8_t> debayered;
    const int N = 200;
    int grabbed = 0;
    double t_total_grab = 0.0, t_total_proc = 0.0;

    for (int i = 0; i < N; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        if (xiGetImage(h, 100, &img) == XI_OK) {
            auto t1 = std::chrono::high_resolution_clock::now();

            // Compute stride (bp_line_step replaced by padding_x)
            int stride = img.width + img.padding_x;

            debayer_half(static_cast<uint8_t*>(img.bp),
                         img.width, img.height, stride, debayered);

            auto t2 = std::chrono::high_resolution_clock::now();

            t_total_grab += std::chrono::duration<double, std::milli>(t1 - t0).count();
            t_total_proc += std::chrono::duration<double, std::milli>(t2 - t1).count();
            grabbed++;
        }
    }

    xiStopAcquisition(h);
    xiCloseDevice(h);

    std::cout << "\nCaptured " << grabbed << " frames\n";
    std::cout << "Avg grab time:   " << (t_total_grab / grabbed) << " ms\n";
    std::cout << "Avg process time:" << (t_total_proc / grabbed) << " ms\n";
    std::cout << "Approx total FPS: " << (1000.0 / (t_total_grab / grabbed + t_total_proc / grabbed)) << "\n";

    return 0;
}

