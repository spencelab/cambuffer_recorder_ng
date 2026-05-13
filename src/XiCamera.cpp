#include "cambuffer_recorder_ng/XiCamera.hpp"

#ifdef HAVE_XIMEA

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef XI_PRM_PADDING_X
#define XI_PRM_PADDING_X "padding_x"
#endif

namespace cambuffer_recorder_ng {

static void xiCheck(XI_RETURN stat, const std::string& what)
{
    if (stat != XI_OK) {
        throw std::runtime_error(what + " failed: " + std::to_string(stat));
    }
}

void XiCamera::configure(const CameraSettings& requested_settings)
{
    requested_settings_ = requested_settings;

    width_ = static_cast<int>(requested_settings.getOr<int64_t>("camera.width", int64_t{2048}));
    height_ = static_cast<int>(requested_settings.getOr<int64_t>("camera.height", int64_t{700}));
    exposure_us_ = requested_settings.getOr<double>("camera.exposure_us", 2000.0);
    gain_db_ = requested_settings.getOr<double>("camera.gain_db", 0.0);

    effective_settings_ = requested_settings_;
    effective_settings_.set("backend", std::string{"xiapi"});
}

void XiCamera::open(int device_index)
{
    XI_RETURN stat = xiOpenDevice(device_index, &handle_);
    xiCheck(stat, "xiOpenDevice");

    std::memset(&image_, 0, sizeof(image_));
    image_.size = sizeof(XI_IMG);

    const std::string pixel_format = requested_settings_.getOr<std::string>("camera.pixel_format", "raw8");
    if (pixel_format == "raw8" || pixel_format == "XI_RAW8" || pixel_format == "bayer8") {
        xiCheck(xiSetParamInt(handle_, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8), "xiSetParamInt IMAGE_DATA_FORMAT XI_RAW8");
    }

    xiCheck(xiSetParamInt(handle_, XI_PRM_WIDTH, width_), "xiSetParamInt WIDTH");
    xiCheck(xiSetParamInt(handle_, XI_PRM_HEIGHT, height_), "xiSetParamInt HEIGHT");
    xiCheck(xiSetParamInt(handle_, XI_PRM_EXPOSURE, static_cast<int>(exposure_us_)), "xiSetParamInt EXPOSURE");
    xiSetParamFloat(handle_, XI_PRM_GAIN, static_cast<float>(gain_db_));

    xiGetParamInt(handle_, XI_PRM_WIDTH, &width_);
    xiGetParamInt(handle_, XI_PRM_HEIGHT, &height_);
    xiGetParamInt(handle_, XI_PRM_IMAGE_DATA_FORMAT, &image_data_format_);
    if (xiGetParamInt(handle_, XI_PRM_PADDING_X, &padding_x_) != XI_OK) padding_x_ = 0;
    stride_bytes_ = width_ + padding_x_;

    int exposure_readback = 0;
    if (xiGetParamInt(handle_, XI_PRM_EXPOSURE, &exposure_readback) == XI_OK) {
        exposure_us_ = static_cast<double>(exposure_readback);
    }

    effective_settings_ = requested_settings_;
    effective_settings_.set("backend", std::string{"xiapi"});
    effective_settings_.set("camera.width", int64_t{width_});
    effective_settings_.set("camera.height", int64_t{height_});
    effective_settings_.set("camera.exposure_us", exposure_us_);
    effective_settings_.set("camera.gain_db", gain_db_);
    effective_settings_.set("camera.pixel_format", std::string{"raw8"});
    effective_settings_.set("camera.bayer_pattern", requested_settings_.getOr<std::string>("camera.bayer_pattern", "GBRG"));
    effective_settings_.set("camera.padding_x", int64_t{padding_x_});
    effective_settings_.set("camera.stride_bytes", int64_t{stride_bytes_});
    effective_settings_.set("ximea.image_data_format", int64_t{image_data_format_});
}

void XiCamera::start()
{
    if (!handle_) throw std::runtime_error("XiCamera not opened");
    xiCheck(xiStartAcquisition(handle_), "xiStartAcquisition");
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
    if (stat != XI_OK || image_.bp == nullptr) return false;

    const int img_width = static_cast<int>(image_.width);
    const int img_height = static_cast<int>(image_.height);
    const int img_stride = img_width + padding_x_;

    data = static_cast<uint8_t*>(image_.bp);
    size = static_cast<size_t>(img_stride) * static_cast<size_t>(img_height);
    width = img_width;
    height = img_height;
    stride = img_stride;

    // Camera timestamp when available. RollingRawRecorder stores PC UTC independently.
    ts = static_cast<uint64_t>(image_.tsSec) * 1'000'000'000ULL +
         static_cast<uint64_t>(image_.tsUSec) * 1000ULL;
    return true;
}

} // namespace cambuffer_recorder_ng

#endif  // HAVE_XIMEA
