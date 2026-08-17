#include "cambuffer_recorder_ng/CamBufferRecorderNode.hpp"

#include "cambuffer_recorder_ng/FakeCamera.hpp"
#include "cambuffer_recorder_ng/settings/SettingsManager.hpp"
#include "cambuffer_recorder_ng/settings/SettingsSerialization.hpp"
#include "cambuffer_recorder_ng/StorageGuard.hpp"

#ifdef HAVE_XIMEA
#include "cambuffer_recorder_ng/XiCamera.hpp"
#endif

#ifdef HAVE_GENTL
#include "cambuffer_recorder_ng/GenTLCamera.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <sched.h>
#include <stdexcept>
#include <system_error>
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

std::string schedulerPolicyName(int policy)
{
    switch (policy) {
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_RR: return "SCHED_RR";
        case SCHED_OTHER: return "SCHED_OTHER";
#ifdef SCHED_BATCH
        case SCHED_BATCH: return "SCHED_BATCH";
#endif
#ifdef SCHED_IDLE
        case SCHED_IDLE: return "SCHED_IDLE";
#endif
        default: return "unknown";
    }
}

std::string joinPath(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string extensionForOutputKind(const std::string& output_kind)
{
    if (output_kind == "rolling_raw_binary" || output_kind == "ram_raw_circular") return ".cbrraw";
    return ".mp4";
}

bool ensureDirectoryExists(const std::string& dir_path, std::string& error)
{
    if (dir_path.empty()) return true;

    std::error_code ec;
    const std::filesystem::path dir(dir_path);

    if (std::filesystem::exists(dir, ec)) {
        if (ec) {
            error = "Could not inspect directory '" + dir.string() + "': " + ec.message();
            return false;
        }
        if (!std::filesystem::is_directory(dir, ec)) {
            error = "Path exists but is not a directory: '" + dir.string() + "'";
            return false;
        }
        if (ec) {
            error = "Could not inspect directory '" + dir.string() + "': " + ec.message();
            return false;
        }
        return true;
    }

    if (!std::filesystem::create_directories(dir, ec) && ec) {
        error = "Could not create directory '" + dir.string() + "': " + ec.message();
        return false;
    }

    return true;
}

bool ensureParentDirectoryExists(const std::string& target_path, std::string& error)
{
    if (target_path.empty()) return true;

    const std::filesystem::path p(target_path);
    if (!p.has_parent_path()) return true;

    return ensureDirectoryExists(p.parent_path().string(), error);
}

bool isRollingRawOutput(const std::string& output_kind, const std::string& mode)
{
    return output_kind == "rolling_raw_binary" ||
           mode == "raw8bayergbrg_rolling" ||
           mode == "raw8mono_rolling";
}

bool isRamRawOutput(const std::string& output_kind, const std::string& mode)
{
    return output_kind == "ram_raw_circular" ||
           mode == "raw8bayergbrg_ram_buffer" ||
           mode == "raw8mono_ram_buffer";
}

bool isRawOutput(const std::string& output_kind, const std::string& mode)
{
    return isRollingRawOutput(output_kind, mode) || isRamRawOutput(output_kind, mode);
}

std::string sanitizeFilenameToken(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_') out.push_back(c);
        else if (c == ' ' || c == '.') out.push_back('_');
    }
    if (out.size() > 48) out.resize(48);
    return out;
}

std::string normalizeLooseToken(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        if (c == '-' || c == ' ' || c == '.') return '_';
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string canonicalTriggerPosition(std::string position)
{
    position = normalizeLooseToken(std::move(position));
    if (position.empty()) return "";

    // These aliases deliberately use the lab's historical names:
    // post-trigger means the event happened before the software trigger, so the
    // output window should end at the trigger/request frame.
    if (position == "end" || position == "post" || position == "post_trigger" ||
        position == "history" || position == "previous" || position == "prior") {
        return "end";
    }
    if (position == "center" || position == "centre" || position == "mid" ||
        position == "mid_trigger" || position == "middle") {
        return "center";
    }
    if (position == "start" || position == "pre" || position == "pre_trigger" ||
        position == "future" || position == "next") {
        return "start";
    }
    if (position == "custom") return "custom";
    return position;
}

uint64_t framesFromSeconds(double seconds, double fps)
{
    if (seconds <= 0.0 || fps <= 0.0) return 0;
    return static_cast<uint64_t>(std::llround(seconds * fps));
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
    storage_free_bytes_pub_ = this->create_publisher<std_msgs::msg::UInt64>("~/storage/free_bytes", rclcpp::QoS(1).reliable().transient_local());
    storage_free_gib_pub_ = this->create_publisher<std_msgs::msg::Float64>("~/storage/free_gib", rclcpp::QoS(1).reliable().transient_local());
    // Keep these event publishers active even when the lifecycle node is inactive so
    // configuration/start/stop service calls are always visible to rosbag and GUIs.
    settings_event_pub_->on_activate();
    recording_event_pub_->on_activate();
    storage_free_bytes_pub_->on_activate();
    storage_free_gib_pub_->on_activate();

    min_free_space_gib_ = this->declare_parameter<double>("storage.min_free_space_gib", 8.0);
    if (min_free_space_gib_ < 0.0) min_free_space_gib_ = 0.0;
    min_free_space_bytes_ = static_cast<uint64_t>(min_free_space_gib_ * 1024.0 * 1024.0 * 1024.0);

    const double storage_status_period_s = this->declare_parameter<double>("storage.status_period_s", 10.0);
    if (storage_status_period_s > 0.0) {
        storage_status_timer_ = this->create_wall_timer(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(storage_status_period_s)),
            [this]() { this->publishStorageStatus(); });
    }

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

    dump_buffer_srv_ = this->create_service<cambuffer_recorder_ng::srv::DumpBuffer>(
        "~/dump_buffer",
        [this](const std::shared_ptr<cambuffer_recorder_ng::srv::DumpBuffer::Request> request,
               std::shared_ptr<cambuffer_recorder_ng::srv::DumpBuffer::Response> response) {
            handleDumpBuffer(request, response);
        });

    RCLCPP_INFO(get_logger(), "Camera backends built into this binary: %s",
                builtBackendSummary().c_str());
    RCLCPP_INFO(get_logger(),
                "Control services ready: ~/apply_settings, ~/start_recording, ~/stop_recording, ~/dump_buffer, ~/get_status. Event topics: ~/settings_event, ~/recording_event. Storage topics: ~/storage/free_bytes, ~/storage/free_gib.");
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
    settings.set("offset_x", settings.get<int64_t>("camera.offset_x"));
    settings.set("offset_y", settings.get<int64_t>("camera.offset_y"));
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
            metadata_path_ = isRawOutput(output_kind_, mode_)
                ? rolling_path_prefix_ + ".metadata.yaml"
                : output_path_ + ".metadata.yaml";
        }

        configured_ = true;
        message = "Configured backend '" + backend_ + "', mode '" + mode_ + "', output kind '" + output_kind_ + "'.";

        RCLCPP_INFO(get_logger(),
                    "Configured backend '%s', mode '%s', output kind '%s' (%dx%d @ offset %ld,%ld @ %.3g fps). Built backends: %s",
                    backend_.c_str(), mode_.c_str(), output_kind_.c_str(), width_, height_,
                    static_cast<long>(effective_settings_.getOr<int64_t>("camera.offset_x", int64_t{0})),
                    static_cast<long>(effective_settings_.getOr<int64_t>("camera.offset_y", int64_t{0})),
                    requested_settings_.getOr<double>("camera.fps", static_cast<double>(fps_)),
                    builtBackendSummary().c_str());
        RCLCPP_INFO(get_logger(), "Run id: %s", run_id_.c_str());
        RCLCPP_INFO(get_logger(), "Metadata path: %s", metadata_path_.c_str());
        publishStorageStatus();
        if (isRollingRawOutput(output_kind_, mode_)) {
            RCLCPP_INFO(get_logger(), "Rolling raw path prefix: %s", rolling_path_prefix_.c_str());
        } else if (isRamRawOutput(output_kind_, mode_)) {
            RCLCPP_INFO(get_logger(), "RAM circular raw session/dump path prefix: %s", rolling_path_prefix_.c_str());
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
        std::string dir_error;
        const std::string output_dir = requested_settings_.getOr<std::string>("output.dir", "/tmp");
        if (!ensureDirectoryExists(output_dir, dir_error) ||
            !ensureParentDirectoryExists(output_path_, dir_error) ||
            !ensureParentDirectoryExists(rolling_path_prefix_, dir_error) ||
            !ensureParentDirectoryExists(metadata_path_, dir_error)) {
            message = "Could not prepare output directories before recording start: " + dir_error;
            RCLCPP_ERROR(get_logger(), "%s", message.c_str());
            publishRecordingEvent("start_recording", false, message);
            return false;
        }

        if (!checkStorageOrLog("recording start", message)) {
            publishRecordingEvent("start_recording", false, message);
            return false;
        }

        camera_->start();

        // Write metadata at recording start so it shares the timestamped run id with the output files.
        effective_settings_.set("run_id", run_id_);
        effective_settings_.set("output.path", output_path_);
        effective_settings_.set("metadata_path", metadata_path_);
        effective_settings_.set("rolling.path_prefix", rolling_path_prefix_);
        writeTextFile(metadata_path_, buildMetadataYaml(run_id_, backend_, mode_, output_kind_, requested_settings_, effective_settings_));

        if (isRollingRawOutput(output_kind_, mode_)) {
            rolling_raw_recorder_ = std::make_shared<RollingRawRecorder>();
            rolling_raw_recorder_->setEventCallback(
                [this](const std::string& event_type, bool success, const std::string& event_message) {
                    this->publishRecordingEvent(event_type, success, event_message);
                });
            rolling_raw_recorder_->setRolloverCallback(
                [this](uint32_t next_file_index) {
                    std::string storage_message;
                    const bool ok = this->checkStorageOrLog(
                        "rolling raw rollover before file index " + std::to_string(next_file_index),
                        storage_message);
                    if (!ok) {
                        this->publishRecordingEvent("storage_low_stop", false, storage_message);
                    }
                    return ok;
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
        } else if (isRamRawOutput(output_kind_, mode_)) {
            ram_raw_recorder_ = std::make_shared<RamCircularRawRecorder>();
            ram_raw_recorder_->setEventCallback(
                [this](const std::string& event_type, bool success, const std::string& event_message) {
                    if (success) {
                        RCLCPP_INFO(this->get_logger(), "%s: %s", event_type.c_str(), event_message.c_str());
                    } else {
                        RCLCPP_WARN(this->get_logger(), "%s: %s", event_type.c_str(), event_message.c_str());
                    }
                    this->publishRecordingEvent(event_type, success, event_message);
                });
            ram_raw_recorder_->setRolloverCallback(
                [this](uint32_t next_file_index) {
                    std::string storage_message;
                    const bool ok = this->checkStorageOrLog(
                        "RAM dump raw rollover before file index " + std::to_string(next_file_index),
                        storage_message);
                    if (!ok) {
                        this->publishRecordingEvent("storage_low_stop", false, storage_message);
                    }
                    return ok;
                });
            if (!ram_raw_recorder_->start(camera_, effective_settings_, rolling_path_prefix_)) {
                message = "RAM circular raw recorder failed to start for prefix '" + rolling_path_prefix_ + "'.";
                RCLCPP_ERROR(get_logger(), "%s", message.c_str());
                camera_->stop();
                publishRecordingEvent("start_recording", false, message);
                return false;
            }
            message = "Backend '" + backend_ + "' active, buffering RAW frames in RAM. Use ~/dump_buffer to write CBRRAW dumps under " + rolling_path_prefix_ + "_dumpNNNNNN_####.cbrraw";
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

    RollingAcquisitionDiagnostics rolling_diagnostics;
    bool have_rolling_diagnostics = false;

    if (rolling_raw_recorder_) {
        rolling_raw_recorder_->stop();
        rolling_diagnostics = rolling_raw_recorder_->diagnostics();
        have_rolling_diagnostics = true;
    }
    if (ram_raw_recorder_) {
        ram_raw_recorder_->stop();
    }
    if (recorder_) {
        recorder_->stop();
    }

    // xiAPI's documented performance counters are intended to be read after the
    // acquisition has stopped. Keep the camera handle open until after readback.
    if (camera_) camera_->stop();
    const CameraAcquisitionDiagnostics camera_diagnostics =
        camera_ ? camera_->acquisitionDiagnostics() : CameraAcquisitionDiagnostics{};

    if (have_rolling_diagnostics) {
        effective_settings_.set("acquisition_diagnostics.frames_written",
                                int64_t{rolling_diagnostics.frames_written});
        effective_settings_.set("acquisition_diagnostics.camera_frame_gaps",
                                int64_t{rolling_diagnostics.camera_frame_gaps});
        effective_settings_.set("acquisition_diagnostics.camera_frame_nonmonotonic",
                                int64_t{rolling_diagnostics.camera_frame_nonmonotonic});
        effective_settings_.set("acquisition_diagnostics.max_write_frame_ms",
                                rolling_diagnostics.max_write_frame_ms);
        effective_settings_.set("acquisition_diagnostics.writes_over_10ms",
                                int64_t{rolling_diagnostics.writes_over_10ms});
        effective_settings_.set("acquisition_diagnostics.writes_over_50ms",
                                int64_t{rolling_diagnostics.writes_over_50ms});
        effective_settings_.set("acquisition_diagnostics.writes_over_100ms",
                                int64_t{rolling_diagnostics.writes_over_100ms});
        effective_settings_.set("acquisition_diagnostics.writes_over_500ms",
                                int64_t{rolling_diagnostics.writes_over_500ms});
        effective_settings_.set("acquisition_diagnostics.worker_sched_policy",
                                int64_t{rolling_diagnostics.worker_sched_policy});
        effective_settings_.set("acquisition_diagnostics.worker_sched_policy_name",
                                schedulerPolicyName(rolling_diagnostics.worker_sched_policy));
        effective_settings_.set("acquisition_diagnostics.worker_sched_priority",
                                int64_t{rolling_diagnostics.worker_sched_priority});
        effective_settings_.set("acquisition_diagnostics.rlimit_rtprio_soft",
                                int64_t{rolling_diagnostics.rlimit_rtprio_soft});
        effective_settings_.set("acquisition_diagnostics.rlimit_rtprio_hard",
                                int64_t{rolling_diagnostics.rlimit_rtprio_hard});
    }

    if (camera_diagnostics.available) {
        effective_settings_.set("acquisition_diagnostics.ximea_acq_buffer_size_value",
                                int64_t{camera_diagnostics.acq_buffer_size_value});
        effective_settings_.set("acquisition_diagnostics.ximea_acq_buffer_size_unit",
                                int64_t{camera_diagnostics.acq_buffer_size_unit});
        effective_settings_.set("acquisition_diagnostics.ximea_acq_buffer_size_bytes",
                                int64_t{camera_diagnostics.acq_buffer_size_bytes});
        effective_settings_.set("acquisition_diagnostics.ximea_buffers_queue_size",
                                int64_t{camera_diagnostics.buffers_queue_size});
        effective_settings_.set("acquisition_diagnostics.ximea_buffer_policy",
                                int64_t{camera_diagnostics.buffer_policy});
        effective_settings_.set("acquisition_diagnostics.ximea_api_skipped_frames",
                                int64_t{camera_diagnostics.api_skipped_frames});
        effective_settings_.set("acquisition_diagnostics.ximea_transport_skipped_frames",
                                int64_t{camera_diagnostics.transport_skipped_frames});
        effective_settings_.set("acquisition_diagnostics.ximea_transport_transferred_frames",
                                int64_t{camera_diagnostics.transport_transferred_frames});
    }

    if (have_rolling_diagnostics || camera_diagnostics.available) {
        RCLCPP_INFO(
            get_logger(),
            "ACQUISITION_DIAGNOSTICS frames_written=%ld camera_frame_gaps=%ld "
            "max_write_frame_ms=%.3f writes_gt_10ms=%ld writes_gt_50ms=%ld "
            "writes_gt_100ms=%ld writes_gt_500ms=%ld worker_policy=%s worker_priority=%d "
            "rtprio_soft=%ld rtprio_hard=%ld ximea_api_skipped=%ld "
            "ximea_transport_skipped=%ld ximea_transport_transferred=%ld",
            static_cast<long>(rolling_diagnostics.frames_written),
            static_cast<long>(rolling_diagnostics.camera_frame_gaps),
            rolling_diagnostics.max_write_frame_ms,
            static_cast<long>(rolling_diagnostics.writes_over_10ms),
            static_cast<long>(rolling_diagnostics.writes_over_50ms),
            static_cast<long>(rolling_diagnostics.writes_over_100ms),
            static_cast<long>(rolling_diagnostics.writes_over_500ms),
            schedulerPolicyName(rolling_diagnostics.worker_sched_policy).c_str(),
            rolling_diagnostics.worker_sched_priority,
            static_cast<long>(rolling_diagnostics.rlimit_rtprio_soft),
            static_cast<long>(rolling_diagnostics.rlimit_rtprio_hard),
            static_cast<long>(camera_diagnostics.api_skipped_frames),
            static_cast<long>(camera_diagnostics.transport_skipped_frames),
            static_cast<long>(camera_diagnostics.transport_transferred_frames));

        // Metadata is initially written at start so the run always has a sidecar.
        // Rewrite it after a clean stop to include end-of-acquisition diagnostics.
        try {
            writeTextFile(metadata_path_, buildMetadataYaml(
                run_id_, backend_, mode_, output_kind_, requested_settings_, effective_settings_));
        } catch (const std::exception& e) {
            RCLCPP_WARN(get_logger(),
                        "Could not update metadata with acquisition diagnostics at stop: %s",
                        e.what());
        }
    }

    rolling_raw_recorder_.reset();
    ram_raw_recorder_.reset();
    recorder_.reset();

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
    if (recording_ || rolling_raw_recorder_ || ram_raw_recorder_ || recorder_) {
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

void CamBufferRecorderNode::handleDumpBuffer(
    const std::shared_ptr<cambuffer_recorder_ng::srv::DumpBuffer::Request> request,
    std::shared_ptr<cambuffer_recorder_ng::srv::DumpBuffer::Response> response)
{
    // Capture local receive time immediately. If the service request does not
    // carry an explicit synchronized trigger_utc_ns, this becomes the dump
    // anchor. Do this before storage checks or other work so multi-camera
    // software-trigger alignment is not affected by local bookkeeping latency.
    const uint64_t local_receive_utc_ns = systemUtcNowNs();

    if (!recording_ || !ram_raw_recorder_) {
        response->success = false;
        response->message = "RAM circular raw recorder is not active. Use a *_ram_buffer mode/output.kind:=ram_raw_circular and start recording first.";
        RCLCPP_WARN(get_logger(), "%s", response->message.c_str());
        publishRecordingEvent("dump_buffer", false, response->message);
        return;
    }

    std::string storage_message;
    if (!checkStorageOrLog("RAM buffer dump", storage_message)) {
        response->success = false;
        response->message = storage_message;
        publishRecordingEvent("dump_buffer", false, response->message);
        return;
    }

    const double fps_for_windows = std::max(
        0.0, effective_settings_.getOr<double>(
            "camera.expected_hardware_fps",
            effective_settings_.getOr<double>("camera.fps", static_cast<double>(fps_))));

    RamBufferDumpRequest dump_request;
    dump_request.label = request->label;
    dump_request.allow_partial = request->allow_partial ||
        effective_settings_.getOr<bool>("ram_buffer.allow_partial_default", false);
    dump_request.anchor_pc_utc_ns = request->trigger_utc_ns != 0
        ? request->trigger_utc_ns
        : local_receive_utc_ns;

    const bool use_legacy_custom_seconds = (request->pre_s > 0.0 || request->post_s > 0.0);
    if (use_legacy_custom_seconds) {
        dump_request.trigger_position = "custom";
        dump_request.window_s = std::max(0.0, request->pre_s) + std::max(0.0, request->post_s);
        dump_request.window_frames = 0;
        dump_request.frames_before_trigger = framesFromSeconds(std::max(0.0, request->pre_s), fps_for_windows);
        dump_request.frames_after_trigger = framesFromSeconds(std::max(0.0, request->post_s), fps_for_windows);
    } else {
        std::string trigger_position = canonicalTriggerPosition(request->trigger_position);
        if (trigger_position.empty()) {
            trigger_position = canonicalTriggerPosition(
                effective_settings_.getOr<std::string>("ram_buffer.default_trigger_position", "end"));
        }
        if (trigger_position.empty()) trigger_position = "end";

        dump_request.trigger_position = trigger_position;
        dump_request.window_s = request->window_s > 0.0
            ? request->window_s
            : effective_settings_.getOr<double>("ram_buffer.default_window_s", 4.0);

        const int64_t configured_window_frames = effective_settings_.getOr<int64_t>(
            "ram_buffer.default_window_frames", int64_t{0});
        dump_request.window_frames = request->window_frames > 0
            ? request->window_frames
            : static_cast<uint32_t>(std::max<int64_t>(0, configured_window_frames));

        const uint64_t total_window_frames = dump_request.window_frames > 0
            ? static_cast<uint64_t>(dump_request.window_frames)
            : framesFromSeconds(dump_request.window_s, fps_for_windows);

        if (total_window_frames == 0) {
            response->success = false;
            response->message = "RAM dump resolved to zero frames. Set window_frames or a positive window_s and expected FPS.";
            RCLCPP_WARN(get_logger(), "%s", response->message.c_str());
            publishRecordingEvent("dump_buffer", false, response->message);
            return;
        }

        if (trigger_position == "end") {
            dump_request.frames_before_trigger = total_window_frames;
            dump_request.frames_after_trigger = 0;
        } else if (trigger_position == "center") {
            dump_request.frames_before_trigger = (total_window_frames + 1) / 2;
            dump_request.frames_after_trigger = total_window_frames - dump_request.frames_before_trigger;
        } else if (trigger_position == "start") {
            dump_request.frames_before_trigger = 0;
            dump_request.frames_after_trigger = total_window_frames;
        } else {
            response->success = false;
            response->message = "Unknown ram_buffer trigger_position '" + trigger_position +
                "'. Use post_trigger/end, mid_trigger/center, pre_trigger/start, or legacy pre_s/post_s custom values.";
            RCLCPP_WARN(get_logger(), "%s", response->message.c_str());
            publishRecordingEvent("dump_buffer", false, response->message);
            return;
        }
    }

    if (!request->output_prefix.empty()) {
        dump_request.output_prefix = request->output_prefix;
    } else {
        const uint64_t dump_number = ++dump_counter_;
        std::ostringstream prefix;
        prefix << rolling_path_prefix_ << "_dump"
               << std::setw(6) << std::setfill('0') << dump_number;
        const std::string safe_label = sanitizeFilenameToken(request->label);
        if (!safe_label.empty()) prefix << "_" << safe_label;
        dump_request.output_prefix = prefix.str();
    }

    const uint64_t total_resolved_frames =
        dump_request.frames_before_trigger + dump_request.frames_after_trigger;
    const double resolved_window_s = fps_for_windows > 0.0
        ? static_cast<double>(total_resolved_frames) / fps_for_windows
        : 0.0;

    RCLCPP_INFO(get_logger(),
                "Dumping RAM buffer: trigger_position=%s, window=%lu frames (%.3f s at %.3f fps), "
                "before_or_at_trigger=%lu frames, after_trigger=%lu frames, allow_partial=%s, "
                "anchor_pc_utc_ns=%lu%s, prefix=%s",
                dump_request.trigger_position.c_str(),
                static_cast<unsigned long>(total_resolved_frames),
                resolved_window_s, fps_for_windows,
                static_cast<unsigned long>(dump_request.frames_before_trigger),
                static_cast<unsigned long>(dump_request.frames_after_trigger),
                dump_request.allow_partial ? "true" : "false",
                static_cast<unsigned long>(dump_request.anchor_pc_utc_ns),
                request->trigger_utc_ns != 0 ? " (request trigger_utc_ns)" : " (local receive time)",
                dump_request.output_prefix.c_str());

    const RamBufferDumpResult result = ram_raw_recorder_->dump(dump_request);

    response->success = result.success;
    response->message = result.message;
    response->dump_prefix = result.dump_prefix;
    response->first_file = result.first_file;
    response->metadata_path = result.success ? result.dump_prefix + ".metadata.yaml" : "";
    response->trigger_position = dump_request.trigger_position;
    response->frames_before_trigger = dump_request.frames_before_trigger;
    response->frames_after_trigger = dump_request.frames_after_trigger;
    response->first_frame_index = result.first_frame_index;
    response->last_frame_index = result.last_frame_index;
    response->frames_written = result.frames_written;
    response->dropped_frames = result.dropped_frames;
    response->anchor_pc_utc_ns = result.anchor_pc_utc_ns;
    response->trigger_frame_index = result.trigger_frame_index;
    response->trigger_frame_pc_utc_ns = result.trigger_frame_pc_utc_ns;
    response->trigger_frame_camera_timestamp_ns = result.trigger_frame_camera_timestamp_ns;
    response->trigger_frame_camera_frame_number = result.trigger_frame_camera_frame_number;

    if (result.success) {
        CameraSettings dump_effective_settings = effective_settings_;
        dump_effective_settings.set("ram_buffer.dump.label", request->label);
        dump_effective_settings.set("ram_buffer.dump.trigger_position", dump_request.trigger_position);
        dump_effective_settings.set("ram_buffer.dump.window_s", dump_request.window_s);
        dump_effective_settings.set("ram_buffer.dump.window_frames", static_cast<int64_t>(dump_request.window_frames));
        dump_effective_settings.set("ram_buffer.dump.frames_before_trigger", static_cast<int64_t>(dump_request.frames_before_trigger));
        dump_effective_settings.set("ram_buffer.dump.frames_after_trigger", static_cast<int64_t>(dump_request.frames_after_trigger));
        dump_effective_settings.set("ram_buffer.dump.resolved_window_frames", static_cast<int64_t>(total_resolved_frames));
        dump_effective_settings.set("ram_buffer.dump.resolved_window_s", resolved_window_s);
        dump_effective_settings.set("ram_buffer.dump.fps_for_window_conversion", fps_for_windows);
        dump_effective_settings.set("ram_buffer.dump.pre_s", fps_for_windows > 0.0 ? static_cast<double>(dump_request.frames_before_trigger) / fps_for_windows : 0.0);
        dump_effective_settings.set("ram_buffer.dump.post_s", fps_for_windows > 0.0 ? static_cast<double>(dump_request.frames_after_trigger) / fps_for_windows : 0.0);
        dump_effective_settings.set("ram_buffer.dump.allow_partial", dump_request.allow_partial);
        dump_effective_settings.set("ram_buffer.dump.local_receive_utc_ns", static_cast<int64_t>(local_receive_utc_ns));
        dump_effective_settings.set("ram_buffer.dump.request_trigger_utc_ns", static_cast<int64_t>(request->trigger_utc_ns));
        dump_effective_settings.set("ram_buffer.dump.anchor_pc_utc_ns", static_cast<int64_t>(result.anchor_pc_utc_ns));
        dump_effective_settings.set("ram_buffer.dump.trigger_frame_index", static_cast<int64_t>(result.trigger_frame_index));
        dump_effective_settings.set("ram_buffer.dump.trigger_frame_pc_utc_ns", static_cast<int64_t>(result.trigger_frame_pc_utc_ns));
        dump_effective_settings.set("ram_buffer.dump.trigger_frame_camera_timestamp_ns", static_cast<int64_t>(result.trigger_frame_camera_timestamp_ns));
        dump_effective_settings.set("ram_buffer.dump.trigger_frame_camera_frame_number", static_cast<int64_t>(result.trigger_frame_camera_frame_number));
        dump_effective_settings.set("ram_buffer.dump.prefix", result.dump_prefix);
        dump_effective_settings.set("ram_buffer.dump.first_file", result.first_file);
        dump_effective_settings.set("ram_buffer.dump.first_frame_index", static_cast<int64_t>(result.first_frame_index));
        dump_effective_settings.set("ram_buffer.dump.last_frame_index", static_cast<int64_t>(result.last_frame_index));
        dump_effective_settings.set("ram_buffer.dump.frames_written", static_cast<int64_t>(result.frames_written));
        dump_effective_settings.set("ram_buffer.dump.dropped_frames", static_cast<int64_t>(result.dropped_frames));
        dump_effective_settings.set("rolling.path_prefix", result.dump_prefix);
        dump_effective_settings.set("metadata_path", response->metadata_path);

        try {
            writeTextFile(response->metadata_path,
                          buildMetadataYaml(run_id_, backend_, mode_, output_kind_, requested_settings_, dump_effective_settings));
        } catch (const std::exception& e) {
            response->success = false;
            response->message = std::string("RAM dump succeeded but metadata write failed: ") + e.what();
            RCLCPP_ERROR(get_logger(), "%s", response->message.c_str());
            publishRecordingEvent("dump_buffer", false, response->message);
            return;
        }
        RCLCPP_INFO(get_logger(), "%s Metadata: %s", response->message.c_str(), response->metadata_path.c_str());
    } else {
        RCLCPP_WARN(get_logger(), "%s", response->message.c_str());
    }

    publishRecordingEvent("dump_buffer", response->success, response->message);
}

std::string CamBufferRecorderNode::storageTargetPath() const
{
    if (isRawOutput(output_kind_, mode_)) {
        return rolling_path_prefix_.empty() ? metadata_path_ : rolling_path_prefix_;
    }
    return output_path_.empty() ? metadata_path_ : output_path_;
}

bool CamBufferRecorderNode::checkStorageOrLog(const std::string& context, std::string& message)
{
    const std::string target_path = storageTargetPath();
    const auto status = getStorageStatusForPath(target_path);

    if (!status.ok) {
        message = "Storage check failed during " + context + " for target '" + target_path +
                  "': " + status.error;
        RCLCPP_ERROR(get_logger(), "%s", message.c_str());
        return false;
    }

    publishStorageStatus();

    if (status.available_bytes < min_free_space_bytes_) {
        std::ostringstream oss;
        oss << "Not enough free storage during " << context << ": "
            << bytesToGib(status.available_bytes) << " GiB available at '" << status.checked_path
            << "', minimum is " << min_free_space_gib_ << " GiB. Stopping/refusing recording.";
        message = oss.str();
        RCLCPP_ERROR(get_logger(), "%s", message.c_str());
        return false;
    }

    std::ostringstream oss;
    oss << "Storage OK during " << context << ": "
        << bytesToGib(status.available_bytes) << " GiB available at '" << status.checked_path
        << "', minimum is " << min_free_space_gib_ << " GiB.";
    message = oss.str();
    RCLCPP_INFO(get_logger(), "%s", message.c_str());
    return true;
}

void CamBufferRecorderNode::publishStorageStatus()
{
    const std::string target_path = storageTargetPath();
    if (target_path.empty()) return;

    const auto status = getStorageStatusForPath(target_path);
    if (!status.ok) return;

    if (storage_free_bytes_pub_) {
        std_msgs::msg::UInt64 msg;
        msg.data = status.available_bytes;
        storage_free_bytes_pub_->publish(msg);
    }
    if (storage_free_gib_pub_) {
        std_msgs::msg::Float64 msg;
        msg.data = bytesToGib(status.available_bytes);
        storage_free_gib_pub_->publish(msg);
    }
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
