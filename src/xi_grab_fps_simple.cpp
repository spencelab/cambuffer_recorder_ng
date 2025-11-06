// g++ xi_grab_fps.cpp -o xi_grab_fps -I/opt/XIMEA/include -lm3api -pthread

#include "m3api/xiApi.h"
#include <iostream>
#include <chrono>

int main() {
    HANDLE h = NULL;
    xiOpenDevice(0, &h);
    xiSetParamInt(h, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiStartAcquisition(h);

    XI_IMG img = {sizeof(XI_IMG)};
    auto start = std::chrono::steady_clock::now();
    int frames = 0;

    while (frames < 300) {
        xiGetImage(h, 100, &img);
        frames++;
    }

    auto end = std::chrono::steady_clock::now();
    double fps = frames / std::chrono::duration<double>(end - start).count();
    std::cout << "Measured: " << fps << " fps" << std::endl;

    xiStopAcquisition(h);
    xiCloseDevice(h);
}

