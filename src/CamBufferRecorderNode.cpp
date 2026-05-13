#include "cambuffer_recorder_ng/CamBufferRecorderNode.hpp"

#include "cambuffer_recorder_ng/FakeCamera.hpp"

#ifdef HAVE_XIMEA
#include "cambuffer_recorder_ng/XiCamera.hpp"
#endif

#ifdef HAVE_GENTL
#include "cambuffer_recorder_ng/GenTLCamera.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace cambuffer_recorder_ng
{
namespace
{

std::string normalizeBackendName(std::string backend)
{
    std::transform(backend.begin(), backend.end(), backend.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (backend == "fakecamera" || backend == "fake_cam" || backend == "fake-cam") {
        return "fake";
    }
    if (backend == "ximea" || backend == "xi" || backend == "xi_api") {
        return "xiapi";
    }
    return backend;
}

std::string joinStrings(const std::vector<std::string>& items, const std::string& sep)
{
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) oss << sep;
        oss << items[i];
    }
    return oss.str();
}

std::string builtBackendSummary()
{
    std::vector<std::string> backends;
    backends.emplace_back("fake");

#ifdef HAVE_XIMEA
    backends.emplace_back("xiapi");
#endif

#ifdef HAVE_GENTL
    backends.emplace_back("gentl");
#endif

    return joinStrings(backends, ", ");
}

}  // namespace

CamBufferRecorderNode::CamBufferRecorderNode()
    : rclcpp_lifecycle::LifecycleNode("cambuffer_recorder_ng")
{
    declare_parameter<std::string>("backend", "fake");
    declare_parameter<int>("width", 320);
    declare_parameter<int>("height", 240);
    declare_parameter<int>("fps", 30);
    declare_parameter<std::string>("output_path", "/home/spencelab/fakecam_test.mp4");

    // GenTL path. This is only used when backend:=gentl and HAVE_GENTL was built.
    declare_parameter<std::string>("cti_path", "/opt/XIMEA/lib/ximea.gentl2.cti");
    declare_parameter<int>("device_index", 0);

    RCLCPP_INFO(get_logger(), "Camera backends built into this binary: %s",
                builtBackendSummary().c_str());
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_configure(const rclcpp_lifecycle::State &)
{
    width_  = get_parameter("width").as_int();
    height_ = get_parameter("height").as_int();
    fps_    = get_parameter("fps").as_int();

    backend_ = normalizeBackendName(get_parameter("backend").as_string());
    const std::string cti_path = get_parameter("cti_path").as_string();
    const int device_index = get_parameter("device_index").as_int();

    try {
        if (backend_ == "fake") {
            camera_ = std::make_shared<FakeCamera>(width_, height_, fps_);
            camera_->open(device_index);
        }
        else if (backend_ == "xiapi") {
#ifdef HAVE_XIMEA
            camera_ = std::make_shared<XiCamera>();
            camera_->open(device_index);
#else
            RCLCPP_ERROR(get_logger(),
                         "backend:=xiapi requested, but XIMEA support was not built. "
                         "Built backends: %s",
                         builtBackendSummary().c_str());
            return CallbackReturn::FAILURE;
#endif
        }
        else if (backend_ == "gentl") {
#ifdef HAVE_GENTL
            auto gentl_camera = std::make_shared<GenTLCamera>();
            gentl_camera->open(cti_path, device_index);
            camera_ = gentl_camera;
#else
            RCLCPP_ERROR(get_logger(),
                         "backend:=gentl requested, but GenTL support was not built. "
                         "Built backends: %s",
                         builtBackendSummary().c_str());
            return CallbackReturn::FAILURE;
#endif
        }
        else {
            RCLCPP_ERROR(get_logger(),
                         "Unknown backend '%s'. Built backends: %s",
                         backend_.c_str(), builtBackendSummary().c_str());
            return CallbackReturn::FAILURE;
        }

        RCLCPP_INFO(get_logger(),
                    "Configured backend '%s' (%dx%d @ %d fps). Built backends: %s",
                    backend_.c_str(), width_, height_, fps_, builtBackendSummary().c_str());
        return CallbackReturn::SUCCESS;
    } catch (const std::exception& e) {
        RCLCPP_ERROR(get_logger(), "Camera open failed for backend '%s': %s",
                     backend_.c_str(), e.what());
        return CallbackReturn::FAILURE;
    }
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_activate(const rclcpp_lifecycle::State &)
{
    if (!camera_) {
        RCLCPP_ERROR(get_logger(), "Camera not configured.");
        return CallbackReturn::FAILURE;
    }

    recorder_ = std::make_shared<Recorder>();
    const std::string output_path = get_parameter("output_path").as_string();

    try {
        camera_->start();

        const bool recorder_started = recorder_->start(
            [this](uint8_t*& d, size_t& sz, uint64_t& t, int& w, int& h, int& s) {
                return camera_->grab(d, sz, t, w, h, s, 100);
            },
            output_path, width_, height_, fps_);

        if (!recorder_started) {
            RCLCPP_ERROR(get_logger(), "Recorder failed to start for output '%s'",
                         output_path.c_str());
            camera_->stop();
            return CallbackReturn::FAILURE;
        }
    } catch (const std::exception& e) {
        RCLCPP_ERROR(get_logger(), "Activation failed: %s", e.what());
        if (camera_) camera_->stop();
        return CallbackReturn::FAILURE;
    }

    // Recorder owns the grab loop. Do not also run this node's diagnostic grab
    // loop, or two threads will pull frames from the same camera at once.
    running_ = false;

    RCLCPP_INFO(get_logger(), "Backend '%s' active and recording to %s",
                backend_.c_str(), output_path.c_str());
    return CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_deactivate(const rclcpp_lifecycle::State &)
{
    running_ = false;
    if (worker_.joinable()) worker_.join();

    if (recorder_) recorder_->stop();
    if (camera_)  camera_->stop();

    RCLCPP_INFO(get_logger(), "Camera deactivated and recording stopped.");
    return CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_shutdown(const rclcpp_lifecycle::State & previous_state)
{
    RCLCPP_INFO(get_logger(),
                "Shutdown requested from lifecycle state '%s'. Backend: '%s'.",
                previous_state.label().c_str(),
                backend_.c_str());

    running_ = false;

    if (worker_.joinable()) {
        worker_.join();
    }

    if (recorder_) {
        RCLCPP_INFO(get_logger(), "Stopping recorder during shutdown.");
        recorder_->stop();
        recorder_.reset();
    }

    if (camera_) {
        RCLCPP_INFO(get_logger(), "Stopping camera backend '%s' during shutdown.",
                    backend_.c_str());
        camera_->stop();
        camera_.reset();
    }

    RCLCPP_INFO(get_logger(), "cambuffer_recorder_ng shutdown complete.");

    return CallbackReturn::SUCCESS;
}

void CamBufferRecorderNode::run_loop()
{
    uint8_t* data = nullptr;
    size_t size = 0;
    int w = 0, h = 0, stride = 0;
    uint64_t ts = 0;

    size_t frame_count = 0;
    auto last_heartbeat = std::chrono::steady_clock::now();

    while (running_ && rclcpp::ok()) {
        if (camera_ && camera_->grab(data, size, ts, w, h, stride, 100)) {
            frame_count++;

            RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 2000,
                                 "Frame ts: %lu (%dx%d, %zu bytes)",
                                 ts, w, h, size);
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_heartbeat).count();
        if (elapsed >= 1.0) {
            double fps_est = frame_count / elapsed;
            RCLCPP_INFO(get_logger(), "Heartbeat: %.2f fps (%.0f frames in %.2fs)",
                        fps_est, static_cast<double>(frame_count), elapsed);
            frame_count = 0;
            last_heartbeat = now;
        }
    }
}

}  // namespace cambuffer_recorder_ng
