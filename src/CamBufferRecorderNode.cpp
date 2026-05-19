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

uint64_t utcNowNs()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::string quoteYaml(const std::string& s)
{
    std::ostringstream oss;
    oss << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') oss << '\\';
        if (c == '\n') oss << "\\n";
        else oss << c;
    }
    oss << '"';
    return oss.str();
}

}  // namespace

CamBufferRecorderNode::CamBufferRecorderNode()
    : rclcpp_lifecycle::LifecycleNode("cambuffer_recorder_ng")
{
    SettingsManager settings_manager(*this);

    auto event_qos = rclcpp::QoS(rclcpp::KeepLast(50)).reliable().transient_local();
    settings_event_pub_ = this->create_publisher<std_msgs::msg::String>("~/settings_event", event_qos);
    recording_event_pub_ = this->create_publisher<std_msgs::msg::String>("~/recording_event", event_qos);
    // Keep these event publishers active even when the lifecycle node is inactive so
    // configuration/start/stop service calls are always visible to rosbag and GUIs.
    settings_event_pub_->on_activate();
    recording_event_pub_->on_activate();

    apply_settings_srv_ = this->create_service<cambuffer_recorder_ng::srv::ApplySettings>(
        "~/apply_settings",
        [this](const std::shared_ptr<cambuffer_recorder_ng::srv::ApplySettings::Request> request,
               std::shared_ptr<cambuffer_recorder_ng::srv::ApplySettings::Response> response) {
            handleApplySettings(request, response);
        });

    start_recording_srv_ = this->create_service<std_srvs::srv::Trigger>(
        "~/start_recording",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
               std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
            handleStartRecording(request, response);
        });

    stop_recording_srv_ = this->create_service<std_srvs::srv::Trigger>(
        "~/stop_recording",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
               std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
            handleStopRecording(request, response);
        });

    get_status_srv_ = this->create_service<cambuffer_recorder_ng::srv::GetStatus>(
        "~/get_status",
        [this](const std::shared_ptr<cambuffer_recorder_ng::srv::GetStatus::Request> request,
               std::shared_ptr<cambuffer_recorder_ng::srv::GetStatus::Response> response) {
            handleGetStatus(request, response);
        });

    RCLCPP_INFO(get_logger(), "Camera backends built into this binary: %s",
                builtBackendSummary().c_str());
    RCLCPP_INFO(get_logger(),
                "Control services ready: ~/apply_settings, ~/start_recording, ~/stop_recording, ~/get_status. Event topics: ~/settings_event, ~/recording_event.");
}

CameraSettings CamBufferRecorderNode::resolveSettingsFromOverrides(const CameraSettings& overrides,
                                                                   bool merge_with_current) const
{
    std::string backend = "fake";
    std::string mode = "video_rgb24";

    if (merge_with_current && requested_settings_.has("backend")) {
        backend = requested_settings_.get<std::string>("backend");
    }
    if (merge_with_current && requested_settings_.has("mode")) {
        mode = requested_settings_.get<std::string>("mode");
    }
    if (overrides.has("backend")) backend = normalizeBackendName(overrides.get<std::string>("backend"));
    if (overrides.has("mode")) mode = normalizeModeName(overrides.get<std::string>("mode"));

    CameraSettings settings = SettingsManager::defaultsForBackend(backend);
    settings.mergeFrom(SettingsManager::defaultsForMode(mode));
    if (merge_with_current) settings.mergeFrom(requested_settings_);
    settings.mergeFrom(overrides);

    settings.set("backend", backend);
    settings.set("mode", mode);

    // Compatibility aliases for older code/metadata readers.
    settings.set("width", settings.get<int64_t>("camera.width"));
    settings.set("height", settings.get<int64_t>("camera.height"));
    settings.set("fps", settings.get<double>("camera.fps"));

    return settings;
}

bool CamBufferRecorderNode::configureFromSettings(const CameraSettings& settings, std::string& message)
{
    if (recording_) {
        message = "Cannot configure while recording; stop recording first.";
        return false;
    }

    cleanupCameraAndRecorders();

    requested_settings_ = settings;
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
            message = "backend:=xiapi requested, but XIMEA support was not built. Built backends: " + builtBackendSummary();
            RCLCPP_ERROR(get_logger(), "%s", message.c_str());
            publishSettingsEvent("configure", false, message);
            return false;
#endif
        }
        else if (backend_ == "gentl") {
#ifdef HAVE_GENTL
            auto gentl_camera = std::make_shared<GenTLCamera>();
            gentl_camera->configure(requested_settings_);
            gentl_camera->open(cti_path, device_index);
            camera_ = gentl_camera;
#else
            message = "backend:=gentl requested, but GenTL support was not built. Built backends: " + builtBackendSummary();
            RCLCPP_ERROR(get_logger(), "%s", message.c_str());
            publishSettingsEvent("configure", false, message);
            return false;
#endif
        }
        else {
            message = "Unknown backend '" + backend_ + "'. Built backends: " + builtBackendSummary();
            RCLCPP_ERROR(get_logger(), "%s", message.c_str());
            publishSettingsEvent("configure", false, message);
            return false;
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

        configured_ = true;
        message = "Configured backend '" + backend_ + "', mode '" + mode_ + "', output kind '" + output_kind_ + "'.";

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

        publishSettingsEvent("configure", true, message);
        return true;
    } catch (const std::exception& e) {
        configured_ = false;
        message = std::string("Camera configure/open failed for backend '") + backend_ + "': " + e.what();
        RCLCPP_ERROR(get_logger(), "%s", message.c_str());
        publishSettingsEvent("configure", false, message);
        cleanupCameraAndRecorders();
        return false;
    }
}

bool CamBufferRecorderNode::startRecording(std::string& message)
{
    if (recording_) {
        message = "Already recording.";
        return true;
    }
    if (!configured_ || !camera_) {
        message = "Camera not configured.";
        RCLCPP_ERROR(get_logger(), "%s", message.c_str());
        publishRecordingEvent("start_recording", false, message);
        return false;
    }

    try {
        camera_->start();

        // Write metadata at recording start so it shares the timestamped run id with the output files.
        effective_settings_.set("run_id", run_id_);
        effective_settings_.set("output.path", output_path_);
        effective_settings_.set("metadata_path", metadata_path_);
        effective_settings_.set("rolling.path_prefix", rolling_path_prefix_);
        writeTextFile(metadata_path_, buildMetadataYaml(run_id_, backend_, mode_, output_kind_, requested_settings_, effective_settings_));

        if (output_kind_ == "rolling_raw_binary" || mode_ == "raw8bayergbrg_rolling") {
            rolling_raw_recorder_ = std::make_shared<RollingRawRecorder>();
            rolling_raw_recorder_->setEventCallback(
                [this](const std::string& event_type, bool success, const std::string& event_message) {
                    this->publishRecordingEvent(event_type, success, event_message);
                });
            if (!rolling_raw_recorder_->start(camera_, effective_settings_, rolling_path_prefix_)) {
                message = "Rolling raw recorder failed to start for prefix '" + rolling_path_prefix_ + "'.";
                RCLCPP_ERROR(get_logger(), "%s", message.c_str());
                camera_->stop();
                publishRecordingEvent("start_recording", false, message);
                return false;
            }
            message = "Backend '" + backend_ + "' active, rolling RAW binary capture to " + rolling_path_prefix_ + "_####.cbrraw";
            RCLCPP_INFO(get_logger(), "%s", message.c_str());
        } else {
            recorder_ = std::make_shared<Recorder>();
            const bool recorder_started = recorder_->start(
                [this](uint8_t*& d, size_t& sz, uint64_t& t, int& w, int& h, int& s) {
                    return camera_->grab(d, sz, t, w, h, s, 100);
                },
                output_path_, width_, height_, fps_);

            if (!recorder_started) {
                message = "Recorder failed to start for output '" + output_path_ + "'.";
                RCLCPP_ERROR(get_logger(), "%s", message.c_str());
                camera_->stop();
                publishRecordingEvent("start_recording", false, message);
                return false;
            }
            message = "Backend '" + backend_ + "' active and recording video to " + output_path_;
            RCLCPP_INFO(get_logger(), "%s", message.c_str());
        }
    } catch (const std::exception& e) {
        message = std::string("Activation failed: ") + e.what();
        RCLCPP_ERROR(get_logger(), "%s", message.c_str());
        if (camera_) camera_->stop();
        publishRecordingEvent("start_recording", false, message);
        return false;
    }

    recording_ = true;
    running_ = false;
    publishRecordingEvent("start_recording", true, message);
    return true;
}

bool CamBufferRecorderNode::stopRecording(std::string& message)
{
    running_ = false;
    if (worker_.joinable()) worker_.join();

    if (rolling_raw_recorder_) {
        rolling_raw_recorder_->stop();
        rolling_raw_recorder_.reset();
    }
    if (recorder_) {
        recorder_->stop();
        recorder_.reset();
    }
    if (camera_) camera_->stop();

    if (recording_) {
        message = "Camera deactivated and recording stopped.";
    } else {
        message = "Recording already stopped.";
    }
    recording_ = false;
    RCLCPP_INFO(get_logger(), "%s", message.c_str());
    publishRecordingEvent("stop_recording", true, message);
    return true;
}

void CamBufferRecorderNode::cleanupCameraAndRecorders()
{
    std::string ignored;
    if (recording_ || rolling_raw_recorder_ || recorder_) {
        stopRecording(ignored);
    }
    if (camera_) {
        camera_->stop();
        camera_->close();
        camera_.reset();
    }
    configured_ = false;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_configure(const rclcpp_lifecycle::State &)
{
    SettingsManager settings_manager(*this);
    std::string message;
    if (!configureFromSettings(settings_manager.buildRequestedSettings(), message)) {
        return CallbackReturn::FAILURE;
    }
    return CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_activate(const rclcpp_lifecycle::State &)
{
    std::string message;
    if (!startRecording(message)) return CallbackReturn::FAILURE;
    return CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_deactivate(const rclcpp_lifecycle::State &)
{
    std::string message;
    stopRecording(message);
    return CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_shutdown(const rclcpp_lifecycle::State & previous_state)
{
    RCLCPP_INFO(get_logger(), "Shutdown requested from lifecycle state '%s'. Backend: '%s'.",
                previous_state.label().c_str(), backend_.c_str());

    cleanupCameraAndRecorders();

    RCLCPP_INFO(get_logger(), "cambuffer_recorder_ng shutdown complete.");
    return CallbackReturn::SUCCESS;
}

void CamBufferRecorderNode::handleApplySettings(
    const std::shared_ptr<cambuffer_recorder_ng::srv::ApplySettings::Request> request,
    std::shared_ptr<cambuffer_recorder_ng::srv::ApplySettings::Response> response)
{
    try {
        CameraSettings overrides = parseCameraSettingsYaml(request->settings_yaml);
        CameraSettings new_settings = resolveSettingsFromOverrides(overrides, request->merge_with_current);

        if (recording_ && !request->restart_if_active) {
            response->success = false;
            response->message = "Recorder is active. Set restart_if_active=true to stop, apply settings, and optionally restart.";
            response->requested_settings_yaml = cameraSettingsToYaml(requested_settings_, "  ");
            response->effective_settings_yaml = cameraSettingsToYaml(effective_settings_, "  ");
            publishSettingsEvent("apply_settings", false, response->message);
            return;
        }

        const bool was_recording = recording_;
        std::string message;
        if (was_recording) {
            stopRecording(message);
        }

        if (!configureFromSettings(new_settings, message)) {
            response->success = false;
            response->message = message;
            response->requested_settings_yaml = cameraSettingsToYaml(requested_settings_, "  ");
            response->effective_settings_yaml = cameraSettingsToYaml(effective_settings_, "  ");
            publishSettingsEvent("apply_settings", false, message);
            return;
        }

        if (request->activate_after_apply || (was_recording && request->restart_if_active)) {
            if (!startRecording(message)) {
                response->success = false;
                response->message = message;
                response->requested_settings_yaml = cameraSettingsToYaml(requested_settings_, "  ");
                response->effective_settings_yaml = cameraSettingsToYaml(effective_settings_, "  ");
                publishSettingsEvent("apply_settings", false, message);
                return;
            }
        }

        response->success = true;
        response->message = "Settings applied.";
        response->requested_settings_yaml = cameraSettingsToYaml(requested_settings_, "  ");
        response->effective_settings_yaml = cameraSettingsToYaml(effective_settings_, "  ");
        publishSettingsEvent("apply_settings", true, response->message);
    } catch (const std::exception& e) {
        response->success = false;
        response->message = std::string("apply_settings failed: ") + e.what();
        response->requested_settings_yaml = cameraSettingsToYaml(requested_settings_, "  ");
        response->effective_settings_yaml = cameraSettingsToYaml(effective_settings_, "  ");
        publishSettingsEvent("apply_settings", false, response->message);
    }
}

void CamBufferRecorderNode::handleStartRecording(
    const std::shared_ptr<std_srvs::srv::Trigger::Request>,
    std::shared_ptr<std_srvs::srv::Trigger::Response> response)
{
    std::string message;
    response->success = startRecording(message);
    response->message = message;
}

void CamBufferRecorderNode::handleStopRecording(
    const std::shared_ptr<std_srvs::srv::Trigger::Request>,
    std::shared_ptr<std_srvs::srv::Trigger::Response> response)
{
    std::string message;
    response->success = stopRecording(message);
    response->message = message;
}

void CamBufferRecorderNode::handleGetStatus(
    const std::shared_ptr<cambuffer_recorder_ng::srv::GetStatus::Request>,
    std::shared_ptr<cambuffer_recorder_ng::srv::GetStatus::Response> response)
{
    response->success = true;
    response->state = this->get_current_state().label();
    response->configured = configured_;
    response->recording = recording_;
    response->backend = backend_;
    response->mode = mode_;
    response->output_kind = output_kind_;
    response->run_id = run_id_;
    response->output_path = output_path_;
    response->metadata_path = metadata_path_;
    response->rolling_path_prefix = rolling_path_prefix_;
    response->requested_settings_yaml = cameraSettingsToYaml(requested_settings_, "  ");
    response->effective_settings_yaml = cameraSettingsToYaml(effective_settings_, "  ");
}

void CamBufferRecorderNode::publishSettingsEvent(const std::string& event_type, bool success, const std::string& message)
{
    if (!settings_event_pub_) return;
    std_msgs::msg::String msg;
    std::ostringstream out;
    out << "event_type: " << quoteYaml(event_type) << "\n";
    out << "stamp_utc_ns: " << utcNowNs() << "\n";
    out << "success: " << (success ? "true" : "false") << "\n";
    out << "message: " << quoteYaml(message) << "\n";
    out << "configured: " << (configured_ ? "true" : "false") << "\n";
    out << "recording: " << (recording_ ? "true" : "false") << "\n";
    out << "backend: " << quoteYaml(backend_) << "\n";
    out << "mode: " << quoteYaml(mode_) << "\n";
    out << "output_kind: " << quoteYaml(output_kind_) << "\n";
    out << "run_id: " << quoteYaml(run_id_) << "\n";
    out << "metadata_path: " << quoteYaml(metadata_path_) << "\n";
    out << "rolling_path_prefix: " << quoteYaml(rolling_path_prefix_) << "\n";
    out << "requested_settings:\n";
    out << cameraSettingsToYaml(requested_settings_, "  ");
    out << "effective_settings:\n";
    out << cameraSettingsToYaml(effective_settings_, "  ");
    msg.data = out.str();
    settings_event_pub_->publish(msg);
}

void CamBufferRecorderNode::publishRecordingEvent(const std::string& event_type, bool success, const std::string& message)
{
    if (!recording_event_pub_) return;
    std_msgs::msg::String msg;
    std::ostringstream out;
    out << "event_type: " << quoteYaml(event_type) << "\n";
    out << "stamp_utc_ns: " << utcNowNs() << "\n";
    out << "success: " << (success ? "true" : "false") << "\n";
    out << "message: " << quoteYaml(message) << "\n";
    out << "configured: " << (configured_ ? "true" : "false") << "\n";
    out << "recording: " << (recording_ ? "true" : "false") << "\n";
    out << "backend: " << quoteYaml(backend_) << "\n";
    out << "mode: " << quoteYaml(mode_) << "\n";
    out << "output_kind: " << quoteYaml(output_kind_) << "\n";
    out << "run_id: " << quoteYaml(run_id_) << "\n";
    out << "output_path: " << quoteYaml(output_path_) << "\n";
    out << "metadata_path: " << quoteYaml(metadata_path_) << "\n";
    out << "rolling_path_prefix: " << quoteYaml(rolling_path_prefix_) << "\n";
    msg.data = out.str();
    recording_event_pub_->publish(msg);
}

void CamBufferRecorderNode::run_loop()
{
    // Kept for future diagnostics. The Recorder/RollingRawRecorder owns the grab loop.
}

}  // namespace cambuffer_recorder_ng
