#include "cambuffer_recorder_ng/XiCamera.hpp"

#include <rclcpp/rclcpp.hpp>

#ifdef HAVE_XIMEA

#include <chrono>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef XI_PRM_PADDING_X
#define XI_PRM_PADDING_X "padding_x"
#endif

#ifndef XI_PRM_BUFFERS_QUEUE_SIZE
#define XI_PRM_BUFFERS_QUEUE_SIZE "buffers_queue_size"
#endif

#ifndef XI_PRM_OFFSET_X
#define XI_PRM_OFFSET_X "offsetX"
#endif

#ifndef XI_PRM_OFFSET_Y
#define XI_PRM_OFFSET_Y "offsetY"
#endif

#ifndef XI_PRM_BUFFER_POLICY
#define XI_PRM_BUFFER_POLICY "buffer_policy"
#endif

#ifndef XI_PRM_ACQ_BUFFER_SIZE
#define XI_PRM_ACQ_BUFFER_SIZE "acq_buffer_size"
#endif

#ifndef XI_PRM_ACQ_BUFFER_SIZE_UNIT
#define XI_PRM_ACQ_BUFFER_SIZE_UNIT "acq_buffer_size_unit"
#endif

#ifndef XI_PRM_COUNTER_SELECTOR
#define XI_PRM_COUNTER_SELECTOR "counter_selector"
#endif

#ifndef XI_PRM_COUNTER_VALUE
#define XI_PRM_COUNTER_VALUE "counter_value"
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
    offset_x_ = static_cast<int>(requested_settings.getOr<int64_t>("camera.offset_x", int64_t{0}));
    offset_y_ = static_cast<int>(requested_settings.getOr<int64_t>("camera.offset_y", int64_t{0}));
    if (offset_x_ < 0 || offset_y_ < 0) {
        throw std::runtime_error("XIMEA ROI offsets must be non-negative");
    }
    exposure_us_ = requested_settings.getOr<double>("camera.exposure_us", 2000.0);
    gain_db_ = requested_settings.getOr<double>("camera.gain_db", 0.0);
    hardware_trigger_ = requested_settings.getOr<bool>("camera.hardware_trigger", false);
    gpi_selector_ = static_cast<int>(requested_settings.getOr<int64_t>("ximea.gpi_selector", int64_t{1}));
    trigger_edge_ = requested_settings.getOr<std::string>("ximea.trigger_edge", "rising");
    buffers_queue_size_ = static_cast<int>(requested_settings.getOr<int64_t>("ximea.buffers_queue_size", int64_t{16}));
    requested_acq_buffer_size_bytes_ =
        requested_settings.getOr<int64_t>("ximea.acq_buffer_size_bytes", int64_t{0});
    if (requested_acq_buffer_size_bytes_ < 0 ||
        requested_acq_buffer_size_bytes_ > static_cast<int64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("ximea.acq_buffer_size_bytes must be between 0 and INT_MAX");
    }
    direct_grab_into_enabled_ = requested_settings.getOr<bool>("ximea.direct_grab_into_enabled", false);
    std::transform(trigger_edge_.begin(), trigger_edge_.end(), trigger_edge_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    effective_settings_ = requested_settings_;
    effective_settings_.set("backend", std::string{"xiapi"});
}

void XiCamera::open(int device_index)
{
    XI_RETURN stat = xiOpenDevice(device_index, &handle_);
    xiCheck(stat, "xiOpenDevice");

    std::memset(&image_, 0, sizeof(image_));
    image_.size = sizeof(XI_IMG);

    // This must be enabled only for the experimental direct grab-into-RAM path.
    // In XI_BP_SAFE mode xiGetImage() copies into the application-provided XI_IMG.bp.
    // In the normal/unsafe policy xiAPI may replace XI_IMG.bp with an SDK-owned buffer,
    // which is exactly how we ended up with valid headers but all-zero RAM payloads.
    if (direct_grab_into_enabled_) {
        XI_RETURN bpstat = xiSetParamInt(handle_, XI_PRM_BUFFER_POLICY, XI_BP_SAFE);
        if (bpstat != XI_OK) {
            std::cout << "[xiapi] warning: could not set buffer_policy=XI_BP_SAFE "
                      << "for direct_grab_into_enabled (xiAPI status " << bpstat << ")"
                      << std::endl;
        } else {
            std::cout << "[xiapi] direct_grab_into_enabled=true; requested buffer_policy=XI_BP_SAFE"
                      << std::endl;
        }
    }

    const std::string pixel_format = requested_settings_.getOr<std::string>("camera.pixel_format", "bayer_gbrg8");
    if (pixel_format == "raw8" || pixel_format == "XI_RAW8" || pixel_format == "bayer8" ||
        pixel_format == "bayer_gbrg8" || pixel_format == "raw8_bayer_gbrg") {
        xiCheck(xiSetParamInt(handle_, XI_PRM_IMAGE_DATA_FORMAT, XI_RAW8), "xiSetParamInt IMAGE_DATA_FORMAT XI_RAW8");
    }

    // XIMEA requires ROI width/height and offsets to remain within sensor bounds.
    // Reset offsets first so reducing/changing an ROI cannot fail because of a
    // previously persisted non-zero camera-side offset. Then apply the requested
    // ROI size and finally move the ROI origin.
    xiCheck(xiSetParamInt(handle_, XI_PRM_OFFSET_X, 0), "xiSetParamInt OFFSET_X reset");
    xiCheck(xiSetParamInt(handle_, XI_PRM_OFFSET_Y, 0), "xiSetParamInt OFFSET_Y reset");
    xiCheck(xiSetParamInt(handle_, XI_PRM_WIDTH, width_), "xiSetParamInt WIDTH");
    xiCheck(xiSetParamInt(handle_, XI_PRM_HEIGHT, height_), "xiSetParamInt HEIGHT");
    xiCheck(xiSetParamInt(handle_, XI_PRM_OFFSET_X, offset_x_), "xiSetParamInt OFFSET_X");
    xiCheck(xiSetParamInt(handle_, XI_PRM_OFFSET_Y, offset_y_), "xiSetParamInt OFFSET_Y");
    xiCheck(xiSetParamInt(handle_, XI_PRM_EXPOSURE, static_cast<int>(exposure_us_)), "xiSetParamInt EXPOSURE");
    xiSetParamFloat(handle_, XI_PRM_GAIN, static_cast<float>(gain_db_));

    if (hardware_trigger_) {
        const int trigger_source =
            (trigger_edge_ == "falling" || trigger_edge_ == "edge_falling")
                ? XI_TRG_EDGE_FALLING
                : XI_TRG_EDGE_RISING;

        xiCheck(xiSetParamInt(handle_, XI_PRM_TRG_SELECTOR, XI_TRG_SEL_FRAME_START),
                "xiSetParamInt TRG_SELECTOR FRAME_START");
        xiCheck(xiSetParamInt(handle_, XI_PRM_GPI_SELECTOR, gpi_selector_),
                "xiSetParamInt GPI_SELECTOR");
        xiCheck(xiSetParamInt(handle_, XI_PRM_GPI_MODE, XI_GPI_TRIGGER),
                "xiSetParamInt GPI_MODE TRIGGER");
        xiCheck(xiSetParamInt(handle_, XI_PRM_TRG_SOURCE, trigger_source),
                "xiSetParamInt TRG_SOURCE external edge");

        std::cout << "[xiapi] hardware trigger enabled: gpi_selector=" << gpi_selector_
                  << ", edge=" << trigger_edge_ << std::endl;
    } else {
        // Explicitly leave the camera in free-run mode when hardware triggering is disabled.
        // Some cameras retain trigger settings across sessions/context unless reset.
        xiSetParamInt(handle_, XI_PRM_TRG_SOURCE, XI_TRG_OFF);
    }

    // XI_PRM_ACQ_BUFFER_SIZE invalidates XI_PRM_BUFFERS_QUEUE_SIZE according to
    // xiAPI. Therefore configure the byte-addressed circular acquisition buffer
    // first, then the image-count FIFO queue, while acquisition is still stopped.
    if (requested_acq_buffer_size_bytes_ > 0) {
        // Make the configuration key's "_bytes" contract explicit instead of
        // relying on xiAPI's documented default unit of 1 byte.
        xiCheck(
            xiSetParamInt(handle_, XI_PRM_ACQ_BUFFER_SIZE_UNIT, 1),
            "xiSetParamInt ACQ_BUFFER_SIZE_UNIT");
        xiCheck(
            xiSetParamInt(handle_, XI_PRM_ACQ_BUFFER_SIZE,
                          static_cast<int>(requested_acq_buffer_size_bytes_)),
            "xiSetParamInt ACQ_BUFFER_SIZE");
        std::cout << "[xiapi] acq_buffer_size requested: "
                  << requested_acq_buffer_size_bytes_ << " bytes (unit=1)" << std::endl;
    }

    // Ask xiAPI for the queue limits in the fully configured camera state.
    // Parameter modifiers are queried by concatenating them to the base parameter
    // name, e.g. XI_PRM_BUFFERS_QUEUE_SIZE XI_PRM_INFO_MAX.  Keep this
    // diagnostic non-fatal because older cameras/API versions may not expose every
    // modifier even when the base parameter itself is available.
    int queue_current = 0;
    int queue_min = 0;
    int queue_max = 0;
    int queue_increment = 0;
    const XI_RETURN qcur_stat =
        xiGetParamInt(handle_, XI_PRM_BUFFERS_QUEUE_SIZE, &queue_current);
    const XI_RETURN qmin_stat =
        xiGetParamInt(handle_, XI_PRM_BUFFERS_QUEUE_SIZE XI_PRM_INFO_MIN, &queue_min);
    const XI_RETURN qmax_stat =
        xiGetParamInt(handle_, XI_PRM_BUFFERS_QUEUE_SIZE XI_PRM_INFO_MAX, &queue_max);
    const XI_RETURN qinc_stat =
        xiGetParamInt(handle_, XI_PRM_BUFFERS_QUEUE_SIZE XI_PRM_INFO_INCREMENT, &queue_increment);

    if (qcur_stat == XI_OK && qmin_stat == XI_OK && qmax_stat == XI_OK && qinc_stat == XI_OK) {
        std::cout << "[xiapi] buffers_queue_size range before requested set:"
                  << " current=" << queue_current
                  << " min=" << queue_min
                  << " max=" << queue_max
                  << " increment=" << queue_increment
                  << std::endl;
    } else {
        std::cout << "[xiapi] warning: buffers_queue_size range query incomplete:"
                  << " current=" << queue_current << "(status=" << qcur_stat << ")"
                  << " min=" << queue_min << "(status=" << qmin_stat << ")"
                  << " max=" << queue_max << "(status=" << qmax_stat << ")"
                  << " increment=" << queue_increment << "(status=" << qinc_stat << ")"
                  << std::endl;
    }

    if (buffers_queue_size_ > 0) {
        XI_RETURN qstat = xiSetParamInt(handle_, XI_PRM_BUFFERS_QUEUE_SIZE, buffers_queue_size_);
        if (qstat != XI_OK) {
            std::cout << "[xiapi] warning: could not set buffers_queue_size="
                      << buffers_queue_size_ << " (xiAPI status " << qstat << ")"
                      << std::endl;
        } else {
            std::cout << "[xiapi] buffers_queue_size requested: "
                      << buffers_queue_size_ << std::endl;
        }
    }

    xiGetParamInt(handle_, XI_PRM_WIDTH, &width_);
    xiGetParamInt(handle_, XI_PRM_HEIGHT, &height_);
    xiGetParamInt(handle_, XI_PRM_OFFSET_X, &offset_x_);
    xiGetParamInt(handle_, XI_PRM_OFFSET_Y, &offset_y_);
    xiGetParamInt(handle_, XI_PRM_IMAGE_DATA_FORMAT, &image_data_format_);
    int actual_buffers_queue_size = buffers_queue_size_;
    if (xiGetParamInt(handle_, XI_PRM_BUFFERS_QUEUE_SIZE, &actual_buffers_queue_size) == XI_OK) {
        buffers_queue_size_ = actual_buffers_queue_size;
    }

    int acq_buffer_size_value = 0;
    int acq_buffer_size_unit = 1;
    const XI_RETURN acq_size_stat = xiGetParamInt(handle_, XI_PRM_ACQ_BUFFER_SIZE, &acq_buffer_size_value);
    const XI_RETURN acq_unit_stat = xiGetParamInt(handle_, XI_PRM_ACQ_BUFFER_SIZE_UNIT, &acq_buffer_size_unit);
    if (acq_size_stat == XI_OK) {
        acq_buffer_size_value_ = acq_buffer_size_value;
        acq_buffer_size_unit_ = (acq_unit_stat == XI_OK && acq_buffer_size_unit > 0) ? acq_buffer_size_unit : 1;
        acq_buffer_size_bytes_ = acq_buffer_size_value_ * acq_buffer_size_unit_;
    }

    if (xiGetParamInt(handle_, XI_PRM_BUFFER_POLICY, &buffer_policy_) != XI_OK) {
        buffer_policy_ = -1;
    }

    if (xiGetParamInt(handle_, XI_PRM_PADDING_X, &padding_x_) != XI_OK) padding_x_ = 0;
    stride_bytes_ = width_ + padding_x_;

    const double configured_fps = requested_settings_.getOr<double>(
        "camera.expected_hardware_fps", requested_settings_.getOr<double>("camera.fps", 0.0));
    const int64_t frame_payload_bytes = static_cast<int64_t>(width_) * static_cast<int64_t>(height_);
    const double acq_buffer_seconds =
        (acq_buffer_size_bytes_ > 0 && frame_payload_bytes > 0 && configured_fps > 0.0)
            ? static_cast<double>(acq_buffer_size_bytes_) /
                  (static_cast<double>(frame_payload_bytes) * configured_fps)
            : -1.0;
    const double queue_seconds =
        (buffers_queue_size_ > 1 && configured_fps > 0.0)
            ? static_cast<double>(buffers_queue_size_ - 1) / configured_fps
            : -1.0;

    RCLCPP_INFO(rclcpp::get_logger("cambuffer_recorder_ng.xiapi"),
                "XIMEA buffer readback: acq_buffer_size_value=%ld unit=%ld bytes=%ld; "
                "buffers_queue_size=%d; buffer_policy=%d; frame_payload=%ld bytes; "
                "estimated acq-buffer depth=%.3f s; queue depth=%.3f s",
                static_cast<long>(acq_buffer_size_value_),
                static_cast<long>(acq_buffer_size_unit_),
                static_cast<long>(acq_buffer_size_bytes_),
                buffers_queue_size_, buffer_policy_,
                static_cast<long>(frame_payload_bytes),
                acq_buffer_seconds, queue_seconds);

    int exposure_readback = 0;
    if (xiGetParamInt(handle_, XI_PRM_EXPOSURE, &exposure_readback) == XI_OK) {
        exposure_us_ = static_cast<double>(exposure_readback);
    }

    effective_settings_ = requested_settings_;
    effective_settings_.set("backend", std::string{"xiapi"});
    effective_settings_.set("camera.width", int64_t{width_});
    effective_settings_.set("camera.height", int64_t{height_});
    effective_settings_.set("camera.offset_x", int64_t{offset_x_});
    effective_settings_.set("camera.offset_y", int64_t{offset_y_});
    effective_settings_.set("camera.exposure_us", exposure_us_);
    effective_settings_.set("camera.gain_db", gain_db_);
    effective_settings_.set("camera.hardware_trigger", hardware_trigger_);
    effective_settings_.set("ximea.gpi_selector", int64_t{gpi_selector_});
    effective_settings_.set("ximea.trigger_edge", trigger_edge_);
    effective_settings_.set("ximea.acq_buffer_size_bytes_requested",
                            int64_t{requested_acq_buffer_size_bytes_});
    effective_settings_.set("ximea.buffers_queue_size", int64_t{buffers_queue_size_});
    effective_settings_.set("ximea.acq_buffer_size_value_actual", int64_t{acq_buffer_size_value_});
    effective_settings_.set("ximea.acq_buffer_size_unit_actual", int64_t{acq_buffer_size_unit_});
    effective_settings_.set("ximea.acq_buffer_size_bytes_actual", int64_t{acq_buffer_size_bytes_});
    effective_settings_.set("ximea.buffer_policy_actual", int64_t{buffer_policy_});
    effective_settings_.set("ximea.acq_buffer_depth_seconds_estimated", acq_buffer_seconds);
    effective_settings_.set("ximea.queue_depth_seconds_estimated", queue_seconds);
    effective_settings_.set("ximea.direct_grab_into_enabled", direct_grab_into_enabled_);
    effective_settings_.set("camera.expected_hardware_fps",
                            requested_settings_.getOr<double>("camera.expected_hardware_fps",
                                                              requested_settings_.getOr<double>("camera.fps", 0.0)));
    effective_settings_.set("camera.grab_timeout_ms",
                            int64_t{requested_settings_.getOr<int64_t>("camera.grab_timeout_ms", int64_t{1000})});
    effective_settings_.set("camera.pixel_format", requested_settings_.getOr<std::string>("camera.pixel_format", "bayer_gbrg8"));
    effective_settings_.set("camera.bayer_pattern", requested_settings_.getOr<std::string>("camera.bayer_pattern", "GBRG"));
    effective_settings_.set("camera.bytes_per_pixel", int64_t{1});
    effective_settings_.set("camera.padding_x", int64_t{padding_x_});
    effective_settings_.set("camera.stride_bytes", int64_t{stride_bytes_});
    effective_settings_.set("ximea.image_data_format", int64_t{image_data_format_});
}

void XiCamera::start()
{
    if (!handle_) throw std::runtime_error("XiCamera not opened");
    const auto t0 = std::chrono::steady_clock::now();
    xiCheck(xiStartAcquisition(handle_), "xiStartAcquisition");
    const auto t1 = std::chrono::steady_clock::now();
    running_ = true;

    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    RCLCPP_INFO(rclcpp::get_logger("cambuffer_recorder_ng.xiapi"),
                "xiStartAcquisition completed in %.3f ms", ms);
}

void XiCamera::stop()
{
    if (!handle_) {
        running_ = false;
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const XI_RETURN stat = xiStopAcquisition(handle_);
    const auto t1 = std::chrono::steady_clock::now();
    running_ = false;

    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (stat == XI_OK) {
        RCLCPP_INFO(rclcpp::get_logger("cambuffer_recorder_ng.xiapi"),
                    "xiStopAcquisition completed in %.3f ms", ms);
    } else {
        RCLCPP_WARN(rclcpp::get_logger("cambuffer_recorder_ng.xiapi"),
                    "xiStopAcquisition returned %d after %.3f ms", stat, ms);
    }
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

    // Ensure ordinary grab() uses xiAPI-owned buffers even if a prior
    // grabPackedInto() call supplied an application-owned destination.
    image_.bp = nullptr;
    image_.bp_size = 0;

    XI_RETURN stat = xiGetImage(handle_, timeout_ms, &image_);
    if (stat != XI_OK || image_.bp == nullptr) {
        return false;
    }

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

    // XIMEA frame sequence number. This is invaluable for distinguishing true
    // camera/API skips from host-side scheduling jitter. Stored in the raw
    // rolling frame header as camera_frame_number.
    last_frame_number_ = static_cast<uint64_t>(image_.nframe);

    return true;
}

CameraAcquisitionDiagnostics XiCamera::acquisitionDiagnostics()
{
    CameraAcquisitionDiagnostics diagnostics;
    diagnostics.available = handle_ != nullptr;
    diagnostics.acq_buffer_size_value = acq_buffer_size_value_;
    diagnostics.acq_buffer_size_unit = acq_buffer_size_unit_;
    diagnostics.acq_buffer_size_bytes = acq_buffer_size_bytes_;
    diagnostics.buffers_queue_size = buffers_queue_size_;
    diagnostics.buffer_policy = buffer_policy_;

    if (!handle_) return diagnostics;

    auto read_counter = [this](int selector, const char* label) -> int64_t {
        const XI_RETURN select_stat = xiSetParamInt(handle_, XI_PRM_COUNTER_SELECTOR, selector);
        if (select_stat != XI_OK) {
            RCLCPP_WARN(rclcpp::get_logger("cambuffer_recorder_ng.xiapi"),
                        "XIMEA counter '%s' selector unavailable (xiAPI status %d)",
                        label, select_stat);
            return -1;
        }
        int value = 0;
        const XI_RETURN value_stat = xiGetParamInt(handle_, XI_PRM_COUNTER_VALUE, &value);
        if (value_stat != XI_OK) {
            RCLCPP_WARN(rclcpp::get_logger("cambuffer_recorder_ng.xiapi"),
                        "XIMEA counter '%s' read unavailable (xiAPI status %d)",
                        label, value_stat);
            return -1;
        }
        return static_cast<int64_t>(value);
    };

    diagnostics.api_skipped_frames =
        read_counter(XI_CNT_SEL_API_SKIPPED_FRAMES, "api_skipped_frames");
    diagnostics.transport_skipped_frames =
        read_counter(XI_CNT_SEL_TRANSPORT_SKIPPED_FRAMES, "transport_skipped_frames");
    diagnostics.transport_transferred_frames =
        read_counter(XI_CNT_SEL_TRANSPORT_TRANSFERRED_FRAMES, "transport_transferred_frames");
    return diagnostics;
}

bool XiCamera::grabPackedInto(uint8_t* dst,
                              size_t dst_capacity,
                              size_t& payload_bytes,
                              uint64_t& ts,
                              int& width,
                              int& height,
                              int& stride,
                              int timeout_ms)
{
    if (!running_ || !dst) return false;

    // Production-safe default: use the ordinary xiAPI-owned buffer grab and copy
    // into the caller's RAM slot. The experimental application-provided XI_IMG.bp
    // path is opt-in because it depends on XI_BP_SAFE and is easy to get subtly
    // wrong: metadata can look perfect while the caller buffer remains all zeroes.
    if (!direct_grab_into_enabled_) {
        return ICamera::grabPackedInto(dst, dst_capacity, payload_bytes, ts, width, height, stride, timeout_ms);
    }

    // The direct xiAPI path is only safe for packed RAW8 frames. If XIMEA ever
    // reports row padding, fall back to the interface default, which grabs into
    // an xiAPI buffer and packs/copies rows.
    if (padding_x_ != 0) {
        return ICamera::grabPackedInto(dst, dst_capacity, payload_bytes, ts, width, height, stride, timeout_ms);
    }

    const size_t wanted = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    if (wanted == 0 || dst_capacity < wanted) return false;

    // Use a fresh XI_IMG for every direct grab, matching the old recorder code.
    // Reusing the member XI_IMG after xiAPI has filled internal fields makes the
    // pointer ownership story murkier than it needs to be.
    XI_IMG img;
    std::memset(&img, 0, sizeof(img));
    img.size = sizeof(XI_IMG);
    img.bp = dst;
    img.bp_size = static_cast<unsigned int>(dst_capacity);

    XI_RETURN stat = xiGetImage(handle_, timeout_ms, &img);
    if (stat != XI_OK || img.bp == nullptr) {
        return false;
    }

    const int img_width = static_cast<int>(img.width);
    const int img_height = static_cast<int>(img.height);
    const int img_stride = img_width + padding_x_;
    const size_t img_payload = static_cast<size_t>(img_width) * static_cast<size_t>(img_height);

    if (img_width <= 0 || img_height <= 0 || img_stride != img_width || img_payload > dst_capacity) {
        return false;
    }

    if (!direct_grab_info_logged_) {
        direct_grab_info_logged_ = true;
        const uint8_t first_byte = dst_capacity > 0 ? dst[0] : 0;
        RCLCPP_INFO(rclcpp::get_logger("cambuffer_recorder_ng.xiapi"),
                    "XIMEA direct grab first frame: dst=%p returned_bp=%p bp_size=%u image=%dx%d first_dst_byte=%u",
                    static_cast<void*>(dst), img.bp, static_cast<unsigned int>(img.bp_size),
                    img_width, img_height, static_cast<unsigned int>(first_byte));
    }

    // Belt and suspenders: if xiAPI did not return the caller-provided pointer,
    // copy from the SDK-owned buffer rather than silently accepting an all-zero
    // destination slot. This makes the failure mode obvious and protects data.
    if (img.bp != dst) {
        if (!direct_grab_pointer_warned_) {
            direct_grab_pointer_warned_ = true;
            RCLCPP_WARN(rclcpp::get_logger("cambuffer_recorder_ng.xiapi"),
                        "XIMEA direct grab did not return the caller-provided buffer: dst=%p returned_bp=%p. "
                        "Copying returned pixels into the RAM slot for this and subsequent frames.",
                        static_cast<void*>(dst), img.bp);
        }
        std::memcpy(dst, static_cast<const uint8_t*>(img.bp), img_payload);
    }

    payload_bytes = img_payload;
    width = img_width;
    height = img_height;
    stride = img_stride;

    ts = static_cast<uint64_t>(img.tsSec) * 1'000'000'000ULL +
         static_cast<uint64_t>(img.tsUSec) * 1000ULL;
    last_frame_number_ = static_cast<uint64_t>(img.nframe);

    return true;
}

} // namespace cambuffer_recorder_ng

#endif  // HAVE_XIMEA
