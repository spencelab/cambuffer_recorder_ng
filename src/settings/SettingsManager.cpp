#include "cambuffer_recorder_ng/settings/SettingsManager.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace cambuffer_recorder_ng
{

static std::string lowerNoSpaces(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        if (c == '-' || c == ' ') return '_';
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string normalizeBackendName(std::string backend)
{
    backend = lowerNoSpaces(std::move(backend));
    if (backend == "fakecamera" || backend == "fake_cam") return "fake";
    if (backend == "ximea" || backend == "xi" || backend == "xi_api") return "xiapi";
    return backend;
}

std::string normalizeModeName(std::string mode)
{
    mode = lowerNoSpaces(std::move(mode));

    if (mode.empty() || mode == "video" || mode == "mp4") {
        return "video_rgb24";
    }

    // Accept the human-readable/camel config spelling plus common variants,
    // but return one canonical lowercase key so mode comparisons are stable.
    if (mode == "raw8bayergbrg_rolling" ||
        mode == "raw8bayergbrgrolling" ||
        mode == "raw8_bayer_gbrg_rolling" ||
        mode == "raw8_bayer_gbrgrolling") {
        return "raw8bayergbrg_rolling";
    }

    if (mode == "raw8bayergbrg_ram_buffer" ||
        mode == "raw8bayergbrgrambuffer" ||
        mode == "raw8_bayer_gbrg_ram_buffer" ||
        mode == "raw8_bayer_gbrg_ram" ||
        mode == "raw8bayergbrg_ram" ||
        mode == "bayer_ram_buffer") {
        return "raw8bayergbrg_ram_buffer";
    }

    if (mode == "raw8mono_rolling" ||
        mode == "raw8monorolling" ||
        mode == "raw8_mono_rolling" ||
        mode == "raw8_mono_rolling" ||
        mode == "mono8_rolling" ||
        mode == "mono8rolling" ||
        mode == "mono_rolling") {
        return "raw8mono_rolling";
    }

    if (mode == "raw8mono_ram_buffer" ||
        mode == "raw8monorambuffer" ||
        mode == "raw8_mono_ram_buffer" ||
        mode == "raw8_mono_ram" ||
        mode == "raw8mono_ram" ||
        mode == "mono8_ram_buffer" ||
        mode == "mono_ram_buffer") {
        return "raw8mono_ram_buffer";
    }

    return mode;
}

static bool parseBoolString(const std::string& value, const std::string& parameter_name)
{
    const std::string v = lowerNoSpaces(value);
    if (v == "true" || v == "1" || v == "yes" || v == "on" || v == "enabled") {
        return true;
    }
    if (v == "false" || v == "0" || v == "no" || v == "off" || v == "disabled") {
        return false;
    }
    throw std::runtime_error("Invalid boolean string for " + parameter_name + ": " + value +
                             " (use true/false or auto)");
}

static bool isAutoString(const std::string& value)
{
    const std::string v = lowerNoSpaces(value);
    return v.empty() || v == "auto" || v == "default" || v == "mode_default";
}

SettingsManager::SettingsManager(rclcpp_lifecycle::LifecycleNode& node)
: node_(node)
{
    declareParameters();
}

void SettingsManager::declareParameters()
{
    auto declare_string = [&](const std::string& name, const std::string& value) {
        if (!node_.has_parameter(name)) node_.declare_parameter<std::string>(name, value);
    };
    auto declare_int = [&](const std::string& name, int value) {
        if (!node_.has_parameter(name)) node_.declare_parameter<int>(name, value);
    };
    auto declare_int64 = [&](const std::string& name, int64_t value) {
        if (!node_.has_parameter(name)) node_.declare_parameter<int64_t>(name, value);
    };
    auto declare_double = [&](const std::string& name, double value) {
        if (!node_.has_parameter(name)) node_.declare_parameter<double>(name, value);
    };
    auto declare_bool = [&](const std::string& name, bool value) {
        if (!node_.has_parameter(name)) node_.declare_parameter<bool>(name, value);
    };

    declare_string("backend", "fake");
    declare_string("mode", "video_rgb24");

    // Compatibility aliases. Non-positive values mean "use mode/backend default".
    declare_int("width", 0);
    declare_int("height", 0);
    declare_double("fps", 0.0);
    declare_string("output_path", "");

    // Explicit namespaced settings.
    declare_int("camera.width", 0);
    declare_int("camera.height", 0);
    // ROI origin in sensor pixels. Non-negative values are valid; XIMEA honors
    // these as XI_PRM_OFFSET_X/Y after width/height are applied.
    declare_int("camera.offset_x", 0);
    declare_int("camera.offset_y", 0);
    declare_double("camera.fps", 0.0);
    declare_double("camera.exposure_us", 0.0);
    declare_double("camera.gain_db", 0.0);
    declare_bool("camera.hardware_trigger", false);
    declare_double("camera.expected_hardware_fps", 0.0);
    declare_int64("camera.grab_timeout_ms", 0);
    declare_double("camera.timeout_warn_interval_s", 0.0);
    declare_double("camera.fps_report_interval_s", 0.0);
    declare_string("camera.pixel_format", "");
    declare_string("camera.bayer_pattern", "");

    // Fake backend pacing is a tri-state string: "auto", "true", or "false".
    // Mode/backend defaults decide auto. Quote this in YAML, e.g. "false".
    declare_string("fake.realtime_pacing", "auto");

    declare_int("device_index", 0);
    declare_string("cti_path", "/opt/XIMEA/lib/ximea.gentl2.cti");
    // XiAPI usually uses 1 for the first physical GPI. XiCamTool may label this as GPI 0.
    // Override ximea.gpi_selector if your camera/API reports a different selector.
    declare_int("ximea.gpi_selector", 1);
    declare_string("ximea.trigger_edge", "rising");
    // xiAPI internal frame-buffer queue. Default xiAPI queue is small; 16 gives
    // roughly 150 ms of cushion at 100 Hz for externally triggered capture.
    // Set <=0 to skip setting it explicitly.
    declare_int("ximea.buffers_queue_size", 16);

    declare_string("pipeline.debayer.enabled", "false");
    declare_double("pipeline.resize.scale", 1.0);
    // Non-positive gains mean "use mode default". This lets modes provide sensible defaults
    // while YAML/CLI can override any individual channel.
    declare_bool("pipeline.white_balance.enabled", false);
    declare_double("pipeline.white_balance.r_gain", 0.0);
    declare_double("pipeline.white_balance.g_gain", 0.0);
    declare_double("pipeline.white_balance.b_gain", 0.0);

    declare_string("output.dir", "/tmp");
    declare_string("output.prefix", "");
    declare_string("output.path", "");
    declare_string("output.kind", "");
    declare_string("output.codec", "libx264");
    declare_string("output.pixel_format", "");
    declare_string("metadata_path", "");

    declare_double("rolling.max_file_gib", 2.0);
    declare_int64("rolling.max_file_bytes", 0);
    declare_int64("rolling.max_frames", 0);
    declare_bool("rolling.pack_rows", true);

    declare_int64("ram_buffer.capacity_frames", 1100);
    declare_string("ram_buffer.dump_policy", "pause_acquisition");
    declare_string("ram_buffer.default_trigger_position", "post_trigger");
    declare_double("ram_buffer.default_window_s", 4.0);
    declare_int64("ram_buffer.default_window_frames", 0);
    // Legacy/expert custom-window defaults retained for compatibility.
    declare_double("ram_buffer.default_pre_s", 4.0);
    declare_double("ram_buffer.default_post_s", 0.0);
    declare_bool("ram_buffer.allow_partial_default", false);
    declare_double("ram_buffer.dump_wait_timeout_s", 30.0);
}

CameraSettings SettingsManager::defaultsForBackend(const std::string& backend_name)
{
    CameraSettings s;
    s.set("backend", backend_name);
    s.set("device_index", int64_t{0});
    s.set("camera.width", int64_t{640});
    s.set("camera.height", int64_t{480});
    s.set("camera.offset_x", int64_t{0});
    s.set("camera.offset_y", int64_t{0});
    s.set("camera.fps", 30.0);
    s.set("camera.exposure_us", 5000.0);
    s.set("camera.gain_db", 0.0);
    s.set("camera.hardware_trigger", false);
    s.set("camera.expected_hardware_fps", 0.0);
    s.set("camera.grab_timeout_ms", int64_t{1000});
    s.set("camera.timeout_warn_interval_s", 5.0);
    s.set("camera.fps_report_interval_s", 5.0);
    s.set("camera.pixel_format", std::string{"rgb24"});
    s.set("camera.bayer_pattern", std::string{""});

    if (backend_name == "fake") {
        s.set("camera.width", int64_t{640});
        s.set("camera.height", int64_t{480});
        s.set("camera.pixel_format", std::string{"rgb24"});
        // Default fakecam behavior emulates a camera that blocks until the next frame.
        // High-level modes can override this when the recorder loop owns pacing.
        s.set("fake.realtime_pacing", true);
    } else if (backend_name == "xiapi") {
        s.set("camera.width", int64_t{2048});
        s.set("camera.height", int64_t{700});
        s.set("camera.fps", 5.0);
        s.set("camera.exposure_us", 2000.0);
        s.set("camera.pixel_format", std::string{"bayer_gbrg8"});
        s.set("camera.bayer_pattern", std::string{"GBRG"});
        s.set("ximea.gpi_selector", int64_t{1});
        s.set("ximea.trigger_edge", std::string{"rising"});
        s.set("ximea.buffers_queue_size", int64_t{16});
    } else if (backend_name == "gentl") {
        s.set("camera.width", int64_t{1280});
        s.set("camera.height", int64_t{1024});
        s.set("camera.pixel_format", std::string{"raw8"});
        s.set("cti_path", std::string{"/opt/XIMEA/lib/ximea.gentl2.cti"});
    }

    return s;
}

CameraSettings SettingsManager::defaultsForMode(const std::string& mode_name)
{
    CameraSettings s;
    s.set("mode", mode_name);
    s.set("output.dir", std::string{"/tmp"});
    s.set("output.prefix", std::string{"cbrng"});
    s.set("metadata_path", std::string{""});

    const std::string normalized_mode = normalizeModeName(mode_name);

    if (normalized_mode == "raw8bayergbrg_rolling") {
        s.set("output.kind", std::string{"rolling_raw_binary"});
        s.set("output.prefix", std::string{"raw8bayerGBRG_rolling"});
        s.set("camera.width", int64_t{2048});
        s.set("camera.height", int64_t{700});
        s.set("camera.fps", 5.0);
        s.set("camera.exposure_us", 2000.0);
        s.set("camera.hardware_trigger", false);
        s.set("camera.expected_hardware_fps", 5.0);
        s.set("camera.grab_timeout_ms", int64_t{1000});
        s.set("camera.timeout_warn_interval_s", 5.0);
        s.set("camera.fps_report_interval_s", 5.0);
        // Mode chooses the acquisition format; cameras simply obey camera.*.
        // For the rolling raw mode, fakecam and XIMEA should both produce one-byte
        // GBRG Bayer frames.
        s.set("camera.pixel_format", std::string{"bayer_gbrg8"});
        s.set("camera.bayer_pattern", std::string{"GBRG"});
        s.set("pipeline.debayer.enabled", false);
        s.set("pipeline.resize.scale", 1.0);
        s.set("pipeline.white_balance.enabled", true);
        s.set("pipeline.white_balance.r_gain", 1.23);
        s.set("pipeline.white_balance.g_gain", 1.00);
        s.set("pipeline.white_balance.b_gain", 1.60);
        s.set("output.pixel_format", std::string{"bayer_gbrg8"});
        s.set("rolling.max_file_gib", 2.0);
        s.set("rolling.max_file_bytes", int64_t{0});
        s.set("rolling.max_frames", int64_t{0});
        s.set("rolling.pack_rows", true);
        s.set("ximea.buffers_queue_size", int64_t{16});
        // RollingRawRecorder already does software pacing. Leave fakecam unpaced
        // by default to avoid two 100 Hz limiters becoming an accidental 50 Hz.
        s.set("fake.realtime_pacing", false);
    } else if (normalized_mode == "raw8mono_rolling") {
        s.set("output.kind", std::string{"rolling_raw_binary"});
        s.set("output.prefix", std::string{"raw8mono_rolling"});
        s.set("camera.width", int64_t{2048});
        s.set("camera.height", int64_t{700});
        s.set("camera.fps", 5.0);
        s.set("camera.exposure_us", 2000.0);
        s.set("camera.hardware_trigger", false);
        s.set("camera.expected_hardware_fps", 5.0);
        s.set("camera.grab_timeout_ms", int64_t{1000});
        s.set("camera.timeout_warn_interval_s", 5.0);
        s.set("camera.fps_report_interval_s", 5.0);
        // One-byte monochrome frames from monochrome XIMEA cameras. Same raw
        // rolling container as Bayer, but tagged RAW8_MONO so audit/info/tools
        // do not assume a Bayer mosaic.
        s.set("camera.pixel_format", std::string{"mono8"});
        s.set("camera.bayer_pattern", std::string{""});
        s.set("pipeline.debayer.enabled", false);
        s.set("pipeline.resize.scale", 1.0);
        s.set("pipeline.white_balance.enabled", false);
        s.set("pipeline.white_balance.r_gain", 1.0);
        s.set("pipeline.white_balance.g_gain", 1.0);
        s.set("pipeline.white_balance.b_gain", 1.0);
        s.set("output.pixel_format", std::string{"mono8"});
        s.set("rolling.max_file_gib", 2.0);
        s.set("rolling.max_file_bytes", int64_t{0});
        s.set("rolling.max_frames", int64_t{0});
        s.set("rolling.pack_rows", true);
        s.set("ximea.buffers_queue_size", int64_t{16});
        // RollingRawRecorder owns software pacing; leave fakecam unpaced when
        // using this high-level rolling mode.
        s.set("fake.realtime_pacing", false);
    } else if (normalized_mode == "raw8bayergbrg_ram_buffer") {
        s.set("output.kind", std::string{"ram_raw_circular"});
        s.set("output.prefix", std::string{"raw8bayerGBRG_ram_buffer"});
        s.set("camera.width", int64_t{2048});
        s.set("camera.height", int64_t{700});
        s.set("camera.fps", 250.0);
        s.set("camera.exposure_us", 2000.0);
        s.set("camera.hardware_trigger", false);
        s.set("camera.expected_hardware_fps", 250.0);
        s.set("camera.grab_timeout_ms", int64_t{1000});
        s.set("camera.timeout_warn_interval_s", 5.0);
        s.set("camera.fps_report_interval_s", 5.0);
        s.set("camera.pixel_format", std::string{"bayer_gbrg8"});
        s.set("camera.bayer_pattern", std::string{"GBRG"});
        s.set("pipeline.debayer.enabled", false);
        s.set("pipeline.resize.scale", 1.0);
        s.set("pipeline.white_balance.enabled", true);
        s.set("pipeline.white_balance.r_gain", 1.23);
        s.set("pipeline.white_balance.g_gain", 1.00);
        s.set("pipeline.white_balance.b_gain", 1.60);
        s.set("output.pixel_format", std::string{"bayer_gbrg8"});
        s.set("rolling.max_file_gib", 2.0);
        s.set("rolling.max_file_bytes", int64_t{0});
        s.set("rolling.pack_rows", true);
        s.set("ram_buffer.capacity_frames", int64_t{1100});
        s.set("ram_buffer.dump_policy", std::string{"pause_acquisition"});
        s.set("ram_buffer.default_trigger_position", std::string{"post_trigger"});
        s.set("ram_buffer.default_window_s", 4.0);
        s.set("ram_buffer.default_window_frames", int64_t{0});
        s.set("ram_buffer.default_pre_s", 4.0);
        s.set("ram_buffer.default_post_s", 0.0);
        s.set("ram_buffer.allow_partial_default", false);
        s.set("ram_buffer.dump_wait_timeout_s", 30.0);
        s.set("ximea.buffers_queue_size", int64_t{16});
        s.set("fake.realtime_pacing", false);
    } else if (normalized_mode == "raw8mono_ram_buffer") {
        s.set("output.kind", std::string{"ram_raw_circular"});
        s.set("output.prefix", std::string{"raw8mono_ram_buffer"});
        s.set("camera.width", int64_t{2048});
        s.set("camera.height", int64_t{700});
        s.set("camera.fps", 250.0);
        s.set("camera.exposure_us", 2000.0);
        s.set("camera.hardware_trigger", false);
        s.set("camera.expected_hardware_fps", 250.0);
        s.set("camera.grab_timeout_ms", int64_t{1000});
        s.set("camera.timeout_warn_interval_s", 5.0);
        s.set("camera.fps_report_interval_s", 5.0);
        s.set("camera.pixel_format", std::string{"mono8"});
        s.set("camera.bayer_pattern", std::string{""});
        s.set("pipeline.debayer.enabled", false);
        s.set("pipeline.resize.scale", 1.0);
        s.set("pipeline.white_balance.enabled", false);
        s.set("pipeline.white_balance.r_gain", 1.0);
        s.set("pipeline.white_balance.g_gain", 1.0);
        s.set("pipeline.white_balance.b_gain", 1.0);
        s.set("output.pixel_format", std::string{"mono8"});
        s.set("rolling.max_file_gib", 2.0);
        s.set("rolling.max_file_bytes", int64_t{0});
        s.set("rolling.pack_rows", true);
        s.set("ram_buffer.capacity_frames", int64_t{1100});
        s.set("ram_buffer.dump_policy", std::string{"pause_acquisition"});
        s.set("ram_buffer.default_trigger_position", std::string{"post_trigger"});
        s.set("ram_buffer.default_window_s", 4.0);
        s.set("ram_buffer.default_window_frames", int64_t{0});
        s.set("ram_buffer.default_pre_s", 4.0);
        s.set("ram_buffer.default_post_s", 0.0);
        s.set("ram_buffer.allow_partial_default", false);
        s.set("ram_buffer.dump_wait_timeout_s", 30.0);
        s.set("ximea.buffers_queue_size", int64_t{16});
        s.set("fake.realtime_pacing", false);
    } else {
        s.set("output.kind", std::string{"video_mp4"});
        s.set("output.prefix", std::string{"video_rgb24"});
        s.set("output.codec", std::string{"libx264"});
        s.set("output.pixel_format", std::string{"rgb24"});
        s.set("pipeline.debayer.enabled", false);
        s.set("pipeline.resize.scale", 1.0);
        s.set("pipeline.white_balance.enabled", false);
        s.set("pipeline.white_balance.r_gain", 1.0);
        s.set("pipeline.white_balance.g_gain", 1.0);
        s.set("pipeline.white_balance.b_gain", 1.0);
        s.set("fake.realtime_pacing", true);
    }

    return s;
}

CameraSettings SettingsManager::readRosOverrides()
{
    CameraSettings s;
    auto set_int_if_positive = [&](const std::string& ros_name, const std::string& key) {
        const int value = node_.get_parameter(ros_name).as_int();
        if (value > 0) s.set(key, int64_t{value});
    };
    auto set_double_if_positive = [&](const std::string& ros_name, const std::string& key) {
        const double value = node_.get_parameter(ros_name).as_double();
        if (value > 0.0) s.set(key, value);
    };
    auto set_int_if_nonnegative = [&](const std::string& ros_name, const std::string& key) {
        const int value = node_.get_parameter(ros_name).as_int();
        if (value >= 0) s.set(key, int64_t{value});
    };
    auto set_string_if_nonempty = [&](const std::string& ros_name, const std::string& key) {
        const std::string value = node_.get_parameter(ros_name).as_string();
        if (!value.empty()) s.set(key, value);
    };

    s.set("backend", normalizeBackendName(node_.get_parameter("backend").as_string()));
    s.set("mode", normalizeModeName(node_.get_parameter("mode").as_string()));

    set_int_if_positive("width", "camera.width");
    set_int_if_positive("height", "camera.height");
    set_double_if_positive("fps", "camera.fps");
    set_string_if_nonempty("output_path", "output.path");

    set_int_if_positive("camera.width", "camera.width");
    set_int_if_positive("camera.height", "camera.height");
    set_int_if_nonnegative("camera.offset_x", "camera.offset_x");
    set_int_if_nonnegative("camera.offset_y", "camera.offset_y");
    set_double_if_positive("camera.fps", "camera.fps");
    set_double_if_positive("camera.exposure_us", "camera.exposure_us");
    // Gain can legitimately be zero, but zero is also our default/no-op. Set it if YAML uses camera.gain_db.
    s.set("camera.gain_db", node_.get_parameter("camera.gain_db").as_double());
    if (node_.get_parameter("camera.hardware_trigger").as_bool()) {
        s.set("camera.hardware_trigger", true);
    }
    set_double_if_positive("camera.expected_hardware_fps", "camera.expected_hardware_fps");
    set_int_if_positive("camera.grab_timeout_ms", "camera.grab_timeout_ms");
    set_double_if_positive("camera.timeout_warn_interval_s", "camera.timeout_warn_interval_s");
    set_double_if_positive("camera.fps_report_interval_s", "camera.fps_report_interval_s");
    set_string_if_nonempty("camera.pixel_format", "camera.pixel_format");
    set_string_if_nonempty("camera.bayer_pattern", "camera.bayer_pattern");

    const std::string fake_realtime_pacing = node_.get_parameter("fake.realtime_pacing").as_string();
    if (!isAutoString(fake_realtime_pacing)) {
        s.set("fake.realtime_pacing",
              parseBoolString(fake_realtime_pacing, "fake.realtime_pacing"));
    }

    s.set("device_index", int64_t{node_.get_parameter("device_index").as_int()});
    set_string_if_nonempty("cti_path", "cti_path");
    s.set("ximea.gpi_selector", int64_t{node_.get_parameter("ximea.gpi_selector").as_int()});
    set_string_if_nonempty("ximea.trigger_edge", "ximea.trigger_edge");
    set_int_if_positive("ximea.buffers_queue_size", "ximea.buffers_queue_size");

    set_string_if_nonempty("output.dir", "output.dir");
    set_string_if_nonempty("output.prefix", "output.prefix");
    set_string_if_nonempty("output.path", "output.path");
    set_string_if_nonempty("output.kind", "output.kind");
    set_string_if_nonempty("output.codec", "output.codec");
    set_string_if_nonempty("output.pixel_format", "output.pixel_format");
    set_string_if_nonempty("metadata_path", "metadata_path");

    if (node_.get_parameter("pipeline.white_balance.enabled").as_bool()) {
        s.set("pipeline.white_balance.enabled", true);
    }
    const double wb_r = node_.get_parameter("pipeline.white_balance.r_gain").as_double();
    const double wb_g = node_.get_parameter("pipeline.white_balance.g_gain").as_double();
    const double wb_b = node_.get_parameter("pipeline.white_balance.b_gain").as_double();
    if (wb_r > 0.0) s.set("pipeline.white_balance.r_gain", wb_r);
    if (wb_g > 0.0) s.set("pipeline.white_balance.g_gain", wb_g);
    if (wb_b > 0.0) s.set("pipeline.white_balance.b_gain", wb_b);

    const double roll_gib = node_.get_parameter("rolling.max_file_gib").as_double();
    if (roll_gib > 0.0) s.set("rolling.max_file_gib", roll_gib);
    const int64_t roll_bytes = node_.get_parameter("rolling.max_file_bytes").as_int();
    if (roll_bytes > 0) s.set("rolling.max_file_bytes", roll_bytes);
    const int64_t max_frames = node_.get_parameter("rolling.max_frames").as_int();
    if (max_frames > 0) s.set("rolling.max_frames", max_frames);
    s.set("rolling.pack_rows", node_.get_parameter("rolling.pack_rows").as_bool());

    const int64_t ram_capacity_frames = node_.get_parameter("ram_buffer.capacity_frames").as_int();
    if (ram_capacity_frames > 0) s.set("ram_buffer.capacity_frames", ram_capacity_frames);
    set_string_if_nonempty("ram_buffer.dump_policy", "ram_buffer.dump_policy");
    set_string_if_nonempty("ram_buffer.default_trigger_position", "ram_buffer.default_trigger_position");
    const double ram_default_window_s = node_.get_parameter("ram_buffer.default_window_s").as_double();
    if (ram_default_window_s > 0.0) s.set("ram_buffer.default_window_s", ram_default_window_s);
    const int64_t ram_default_window_frames = node_.get_parameter("ram_buffer.default_window_frames").as_int();
    if (ram_default_window_frames > 0) s.set("ram_buffer.default_window_frames", ram_default_window_frames);
    const double ram_default_pre_s = node_.get_parameter("ram_buffer.default_pre_s").as_double();
    if (ram_default_pre_s >= 0.0) s.set("ram_buffer.default_pre_s", ram_default_pre_s);
    const double ram_default_post_s = node_.get_parameter("ram_buffer.default_post_s").as_double();
    if (ram_default_post_s >= 0.0) s.set("ram_buffer.default_post_s", ram_default_post_s);
    if (node_.get_parameter("ram_buffer.allow_partial_default").as_bool()) {
        s.set("ram_buffer.allow_partial_default", true);
    }
    const double ram_dump_wait_timeout_s = node_.get_parameter("ram_buffer.dump_wait_timeout_s").as_double();
    if (ram_dump_wait_timeout_s > 0.0) s.set("ram_buffer.dump_wait_timeout_s", ram_dump_wait_timeout_s);

    return s;
}

CameraSettings SettingsManager::buildRequestedSettings()
{
    CameraSettings ros = readRosOverrides();
    const std::string backend = ros.get<std::string>("backend");
    const std::string mode = ros.get<std::string>("mode");

    CameraSettings settings = defaultsForBackend(backend);
    settings.mergeFrom(defaultsForMode(mode));
    settings.mergeFrom(ros);

    // Compatibility aliases for older code/metadata readers.
    settings.set("width", settings.get<int64_t>("camera.width"));
    settings.set("height", settings.get<int64_t>("camera.height"));
    settings.set("offset_x", settings.get<int64_t>("camera.offset_x"));
    settings.set("offset_y", settings.get<int64_t>("camera.offset_y"));
    settings.set("fps", settings.get<double>("camera.fps"));

    return settings;
}

}  // namespace cambuffer_recorder_ng
