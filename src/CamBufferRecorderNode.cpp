#include "cambuffer_recorder_ng/CamBufferRecorderNode.hpp"

#include "cambuffer_recorder_ng/FakeCamera.hpp"
#include "cambuffer_recorder_ng/settings/SettingsSerialization.hpp"

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

int settingIntOr(const CameraSettings& settings, const std::string& name, int fallback)
{
    return static_cast<int>(settings.getOr<int64_t>(name, static_cast<int64_t>(fallback)));
}

}  // namespace

CamBufferRecorderNode::CamBufferRecorderNode()
    : rclcpp_lifecycle::LifecycleNode(
          "cambuffer_recorder_ng",
          rclcpp::NodeOptions()
              .allow_undeclared_parameters(true)
              .automatically_declare_parameters_from_overrides(true))
{
    settings_manager_ = std::make_unique<SettingsManager>(*this);

    RCLCPP_INFO(get_logger(), "Camera backends built into this binary: %s",
                builtBackendSummary().c_str());
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_configure(const rclcpp_lifecycle::State &)
{
    backend_ = normalizeBackendName(get_parameter("backend").as_string());
    requested_settings_ = settings_manager_->buildRequestedSettings(backend_);

    width_ = settingIntOr(requested_settings_, "width", width_);
    height_ = settingIntOr(requested_settings_, "height", height_);
    fps_ = settingIntOr(requested_settings_, "fps", fps_);

    const std::string cti_path = get_parameter("cti_path").as_string();
    const int device_index = static_cast<int>(get_parameter("device_index").as_int());

    try {
        if (backend_ == "fake") {
            camera_ = std::make_shared<FakeCamera>(width_, height_, fps_);
            camera_->configure(requested_settings_);
            camera_->open(device_index);
        }
        else if (backend_ == "xiapi") {
#ifdef HAVE_XIMEA
            camera_ = std::make_shared<XiCamera>();
            camera_->configure(requested_settings_);
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
            gentl_camera->configure(requested_settings_);
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

        effective_settings_ = camera_->effectiveSettings();
        width_ = settingIntOr(effective_settings_, "width", width_);
        height_ = settingIntOr(effective_settings_, "height", height_);
        fps_ = settingIntOr(effective_settings_, "fps", fps_);

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
    const std::string metadata_path = metadataPathForOutput(output_path);

    try {
        effective_settings_ = camera_->effectiveSettings();
        writeMetadataYaml(metadata_path,
                          backend_,
                          output_path,
                          requested_settings_,
                          effective_settings_);
        RCLCPP_INFO(get_logger(), "Wrote metadata to %s", metadata_path.c_str());

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
        camera_->close();
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

std::string CamBufferRecorderNode::metadataPathForOutput(const std::string& output_path) const
{
    const std::string explicit_path = get_parameter("metadata_path").as_string();
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    return output_path + ".metadata.yaml";
}

}  // namespace cambuffer_recorder_ng
