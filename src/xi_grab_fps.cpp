#include "m3api/xiApi.h"
#include <iostream>
#include <chrono>
#include <cstring>

void print_camera_info(HANDLE h) {
    char model[256] = {0}, serial[256] = {0};
    xiGetParamString(h, XI_PRM_DEVICE_NAME, model, sizeof(model));
    xiGetParamString(h, XI_PRM_DEVICE_SN, serial, sizeof(serial));

    int width = 0, height = 0, offset_x = 0, offset_y = 0, gain = 0, exposure = 0;
    float fps = 0.0f;
    int data_format = 0;

    xiGetParamInt(h, XI_PRM_WIDTH, &width);
    xiGetParamInt(h, XI_PRM_HEIGHT, &height);
    xiGetParamInt(h, XI_PRM_OFFSET_X, &offset_x);
    xiGetParamInt(h, XI_PRM_OFFSET_Y, &offset_y);
    xiGetParamInt(h, XI_PRM_GAIN, &gain);
    xiGetParamInt(h, XI_PRM_EXPOSURE, &exposure);
    xiGetParamFloat(h, XI_PRM_FRAMERATE, &fps);
    xiGetParamInt(h, XI_PRM_IMAGE_DATA_FORMAT, &data_format);

    std::cout << "\n=== XIMEA Camera Info ===\n";
    std::cout << "Model:     " << model << "\n";
    std::cout << "Serial:    " << serial << "\n";
    std::cout << "ROI:       " << width << "x" << height
              << " @ (" << offset_x << "," << offset_y << ")\n";
    std::cout << "Exposure:  " << exposure << " us\n";
    std::cout << "Gain:      " << gain << "\n";
    std::cout << "Framerate: " << fps << " fps\n";
    std::cout << "Format:    " << data_format << " (XI_RAW8=8, XI_MONO8=1, etc.)\n";
    std::cout << "==========================\n\n";
}

int main() {
    HANDLE h = NULL;
    XI_RETURN stat = xiOpenDevice(0, &h);
    if (stat != XI_OK) {
        std::cerr << "Failed to open camera, code: " << stat << std::endl;
        return -1;
    }

    // Optional: ensure known baseline settings
    xiSetParamInt(h, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(h, XI_PRM_EXPOSURE, 10000);
    xiSetParamFloat(h, XI_PRM_FRAMERATE, 300.0f);

    print_camera_info(h);

    xiStartAcquisition(h);

    XI_IMG img = {sizeof(XI_IMG)};
    auto start = std::chrono::steady_clock::now();
    int frames = 0;
    const int test_frames = 500;

    for (int i = 0; i < test_frames; ++i) {
        if (xiGetImage(h, 100, &img) == XI_OK)
            frames++;
    }

    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();
    double fps = frames / elapsed;

    std::cout << "Captured " << frames << " frames in " << elapsed << " s\n";
    std::cout << "Measured: " << fps << " fps\n";

    xiStopAcquisition(h);
    xiCloseDevice(h);
    return 0;
}

