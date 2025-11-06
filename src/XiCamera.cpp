#include <m3api/xiApi.h> 
#include "cambuffer_recorder_ng/XiCamera.hpp"
#include <chrono>
#include <cstring>
#include <iostream>

namespace cambuffer_recorder_ng {

void XiCamera::open(int device_index)
{
    XI_RETURN stat = xiOpenDevice(device_index, &handle_);
    if (stat != XI_OK)
        throw std::runtime_error("xiOpenDevice failed: " + std::to_string(stat));

    std::memset(&image_, 0, sizeof(image_));
    image_.size = sizeof(XI_IMG);

    xiGetParamInt(handle_, XI_PRM_WIDTH, &width_);
    xiGetParamInt(handle_, XI_PRM_HEIGHT, &height_);

    // Configure RAW8 format
    xiSetParamInt(handle_, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8);
    xiSetParamInt(handle_, XI_PRM_EXPOSURE, 10000);
    xiSetParamInt(handle_, XI_PRM_GAIN, 0);
}

void XiCamera::start()
{
    if (!handle_) throw std::runtime_error("XiCamera not opened");
    xiStartAcquisition(handle_);
    running_ = true;
}

void XiCamera::stop()
{
    if (handle_) xiStopAcquisition(handle_);
    running_ = false;
}

void XiCamera::close()
{
    if (handle_) {
        xiCloseDevice(handle_);
        handle_ = nullptr;
    }
    running_ = false;
}

bool XiCamera::grab(uint8_t*& data, size_t& size, uint64_t& ts,
                    int& width, int& height, int& stride, int timeout_ms)
{
    if (!running_) return false;

    XI_RETURN stat = xiGetImage(handle_, timeout_ms, &image_);
    if (stat != XI_OK) return false;

    data = static_cast<uint8_t*>(image_.bp);
    size = image_.width * image_.height;
    width = image_.width;
    height = image_.height;
    stride = image_.width;  // assume 1 byte per pixel for RAW8
    ts = static_cast<uint64_t>(image_.tsSec) * 1'000'000'000ULL +
         static_cast<uint64_t>(image_.tsUSec) * 1000ULL;
    return true;
}

} // namespace cambuffer_recorder_ng

