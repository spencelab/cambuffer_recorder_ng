#include "cambuffer_recorder_ng/CamBufferRecorderNode.hpp"

#include "cambuffer_recorder_ng/FakeCamera.hpp"
#include "cambuffer_recorder_ng/settings/SettingsManager.hpp"
#include "cambuffer_recorder_ng/settings/SettingsSerialization.hpp"

#ifdef HAVE_XIMEA
#include "cambuffer_recorder_ng/XiCamera.hpp"
#endif

#ifdef HAVE_GENTL
#include "cambuffer_recorder_ng/GenTLCamera.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace cambuffer_recorder_ng
{
namespace
{

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

std::string joinPath(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string extensionForOutputKind(const std::string& output_kind)
{
    if (output_kind == "rolling_raw_binary") return ".cbrraw";
    return ".mp4";
}

}  // namespace

CamBufferRecorderNode::CamBufferRecorderNode()
    : rclcpp_lifecycle::LifecycleNode("cambuffer_recorder_ng")
{
    SettingsManager settings_manager(*this);
    RCLCPP_INFO(get_logger(), "Camera backends built into this binary: %s",
                builtBackendSummary().c_str());
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_configure(const rclcpp_lifecycle::State &)
{
    SettingsManager settings_manager(*this);
    requested_settings_ = settings_manager.buildRequestedSettings();

    backend_ = normalizeBackendName(requested_settings_.get<std::string>("backend"));
    mode_ = normalizeModeName(requested_settings_.get<std::string>("mode"));
    output_kind_ = requested_settings_.getOr<std::string>("output.kind", "video_mp4");

    const int device_index = static_cast<int>(requested_settings_.getOr<int64_t>("device_index", int64_t{0}));
    const std::string cti_path = requested_settings_.getOr<std::string>("cti_path", "/opt/XIMEA/lib/ximea.gentl2.cti");

    try {
        if (backend_ == "fake") {
            camera_ = std::make_shared<FakeCamera>();
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
                         "backend:=xiapi requested, but XIMEA support was not built. Built backends: %s",
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
                         "backend:=gentl requested, but GenTL support was not built. Built backends: %s",
                         builtBackendSummary().c_str());
            return CallbackReturn::FAILURE;
#endif
        }
        else {
            RCLCPP_ERROR(get_logger(), "Unknown backend '%s'. Built backends: %s",
                         backend_.c_str(), builtBackendSummary().c_str());
            return CallbackReturn::FAILURE;
        }

        effective_settings_ = camera_->getEffectiveSettings();
        if (effective_settings_.values().empty()) effective_settings_ = requested_settings_;

        width_  = static_cast<int>(effective_settings_.getOr<int64_t>("camera.width", requested_settings_.getOr<int64_t>("camera.width", int64_t{640})));
        height_ = static_cast<int>(effective_settings_.getOr<int64_t>("camera.height", requested_settings_.getOr<int64_t>("camera.height", int64_t{480})));
        fps_    = static_cast<int>(effective_settings_.getOr<double>("camera.fps", requested_settings_.getOr<double>("camera.fps", 30.0)));

        run_id_ = utcTimestampForFilename();
        const std::string output_dir = requested_settings_.getOr<std::string>("output.dir", "/tmp");
        std::string prefix = requested_settings_.getOr<std::string>("output.prefix", "cbrng");
        if (prefix.empty()) prefix = backend_ + "_" + mode_;
        prefix += "_" + run_id_;

        output_path_ = requested_settings_.getOr<std::string>("output.path", "");
        if (output_path_.empty() && output_kind_ == "video_mp4") {
            output_path_ = joinPath(output_dir, prefix + extensionForOutputKind(output_kind_));
        }
        rolling_path_prefix_ = joinPath(output_dir, prefix);

        metadata_path_ = requested_settings_.getOr<std::string>("metadata_path", "");
        if (metadata_path_.empty()) {
            metadata_path_ = (output_kind_ == "rolling_raw_binary")
                ? rolling_path_prefix_ + ".metadata.yaml"
                : output_path_ + ".metadata.yaml";
        }

        RCLCPP_INFO(get_logger(),
                    "Configured backend '%s', mode '%s', output kind '%s' (%dx%d @ %.3g fps). Built backends: %s",
                    backend_.c_str(), mode_.c_str(), output_kind_.c_str(), width_, height_,
                    requested_settings_.getOr<double>("camera.fps", static_cast<double>(fps_)),
                    builtBackendSummary().c_str());
        RCLCPP_INFO(get_logger(), "Run id: %s", run_id_.c_str());
        RCLCPP_INFO(get_logger(), "Metadata path: %s", metadata_path_.c_str());
        if (output_kind_ == "rolling_raw_binary") {
            RCLCPP_INFO(get_logger(), "Rolling raw path prefix: %s", rolling_path_prefix_.c_str());
        } else {
            RCLCPP_INFO(get_logger(), "Video output path: %s", output_path_.c_str());
        }

        return CallbackReturn::SUCCESS;
    } catch (const std::exception& e) {
        RCLCPP_ERROR(get_logger(), "Camera configure/open failed for backend '%s': %s", backend_.c_str(), e.what());
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

    try {
        camera_->start();

        // Write metadata at recording start so it shares the timestamped run id with the output files.
        effective_settings_.set("run_id", run_id_);
        effective_settings_.set("output.path", output_path_);
        effective_settings_.set("metadata_path", metadata_path_);
        effective_settings_.set("rolling.path_prefix", rolling_path_prefix_);
        writeTextFile(metadata_path_, buildMetadataYaml(run_id_, backend_, mode_, output_kind_, requested_settings_, effective_settings_));

        if (output_kind_ == "rolling_raw_binary" || mode_ == "raw8bayerGBRG_rolling") {
            rolling_raw_recorder_ = std::make_shared<RollingRawRecorder>();
            if (!rolling_raw_recorder_->start(camera_, effective_settings_, rolling_path_prefix_)) {
                RCLCPP_ERROR(get_logger(), "Rolling raw recorder failed to start for prefix '%s'", rolling_path_prefix_.c_str());
                camera_->stop();
                return CallbackReturn::FAILURE;
            }
            RCLCPP_INFO(get_logger(), "Backend '%s' active, rolling RAW binary capture to %s_####.cbrraw",
                        backend_.c_str(), rolling_path_prefix_.c_str());
        } else {
            recorder_ = std::make_shared<Recorder>();
            const bool recorder_started = recorder_->start(
                [this](uint8_t*& d, size_t& sz, uint64_t& t, int& w, int& h, int& s) {
                    return camera_->grab(d, sz, t, w, h, s, 100);
                },
                output_path_, width_, height_, fps_);

            if (!recorder_started) {
                RCLCPP_ERROR(get_logger(), "Recorder failed to start for output '%s'", output_path_.c_str());
                camera_->stop();
                return CallbackReturn::FAILURE;
            }
            RCLCPP_INFO(get_logger(), "Backend '%s' active and recording video to %s",
                        backend_.c_str(), output_path_.c_str());
        }
    } catch (const std::exception& e) {
        RCLCPP_ERROR(get_logger(), "Activation failed: %s", e.what());
        if (camera_) camera_->stop();
        return CallbackReturn::FAILURE;
    }

    running_ = false;
    return CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_deactivate(const rclcpp_lifecycle::State &)
{
    running_ = false;
    if (worker_.joinable()) worker_.join();

    if (rolling_raw_recorder_) rolling_raw_recorder_->stop();
    if (recorder_) recorder_->stop();
    if (camera_) camera_->stop();

    RCLCPP_INFO(get_logger(), "Camera deactivated and recording stopped.");
    return CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_shutdown(const rclcpp_lifecycle::State & previous_state)
{
    RCLCPP_INFO(get_logger(), "Shutdown requested from lifecycle state '%s'. Backend: '%s'.",
                previous_state.label().c_str(), backend_.c_str());

    running_ = false;
    if (worker_.joinable()) worker_.join();

    if (rolling_raw_recorder_) {
        RCLCPP_INFO(get_logger(), "Stopping rolling raw recorder during shutdown.");
        rolling_raw_recorder_->stop();
        rolling_raw_recorder_.reset();
    }
    if (recorder_) {
        RCLCPP_INFO(get_logger(), "Stopping recorder during shutdown.");
        recorder_->stop();
        recorder_.reset();
    }
    if (camera_) {
        RCLCPP_INFO(get_logger(), "Stopping camera backend '%s' during shutdown.", backend_.c_str());
        camera_->stop();
        camera_->close();
        camera_.reset();
    }

    RCLCPP_INFO(get_logger(), "cambuffer_recorder_ng shutdown complete.");
    return CallbackReturn::SUCCESS;
}

void CamBufferRecorderNode::run_loop()
{
    // Kept for future diagnostics. The Recorder/RollingRawRecorder owns the grab loop.
}

}  // namespace cambuffer_recorder_ng
