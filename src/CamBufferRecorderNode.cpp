#include "cambuffer_recorder_ng/CamBufferRecorderNode.hpp"

// --- Choose camera backend ---
#include "cambuffer_recorder_ng/XiCamera.hpp"       // legacy XIMEA SDK
#include "cambuffer_recorder_ng/GenTLCamera.hpp"      // new generic GenTL backend
#include "cambuffer_recorder_ng/FakeCamera.hpp"       // for testing without hardware
#include "cambuffer_recorder_ng/Recorder.hpp"

namespace cambuffer_recorder_ng
{

CamBufferRecorderNode::CamBufferRecorderNode()
    : rclcpp_lifecycle::LifecycleNode("cambuffer_recorder_ng")
{
    declare_parameter<int>("width", 320);
    declare_parameter<int>("height", 240);
    declare_parameter<int>("fps", 30);
    declare_parameter<std::string>("output_path", "/home/spencelab/fakecam_test.mp4");

    // GenTL path — defaults to XIMEA, but can be overridden at launch
    declare_parameter<std::string>("cti_path", "/opt/XIMEA/lib/ximea.gentl2.cti");
    declare_parameter<int>("device_index", 0);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_configure(const rclcpp_lifecycle::State &)
{
    width_  = get_parameter("width").as_int();
    height_ = get_parameter("height").as_int();
    fps_    = get_parameter("fps").as_int();
    std::string cti_path = get_parameter("cti_path").as_string();
    std::string backend;
    get_parameter_or("backend", backend, std::string("xiapi"));
    int device_index;
    get_parameter_or("device_index", device_index, 0);

    try {
        if (backend == "xiapi") {
            camera_ = std::make_shared<XiCamera>();
        } else if (backend == "gentl") {
            camera_ = std::make_shared<GenTLCamera>();
        } else {
            camera_ = std::make_shared<FakeCamera>(width_, height_, fps_);
        }

        camera_->open(device_index);
        RCLCPP_INFO(get_logger(), "Configured %s backend", backend.c_str());
        return CallbackReturn::SUCCESS;
    } catch (const std::exception& e) {
        RCLCPP_ERROR(get_logger(), "Camera open failed: %s", e.what());
        return CallbackReturn::FAILURE;
    }


    // --- Keep FakeCamera for testing ---
    /*
    camera_ = std::make_shared<FakeCamera>(width_, height_, fps_);
    camera_->open();
    RCLCPP_INFO(get_logger(), "Configured FakeCamera %dx%d @ %d fps", width_, height_, fps_);
    */

    return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_activate(const rclcpp_lifecycle::State &)
{
    if (!camera_) {
        RCLCPP_ERROR(get_logger(), "Camera not configured.");
        return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::FAILURE;
    }

    recorder_ = std::make_shared<Recorder>();
    std::string output_path = get_parameter("output_path").as_string();

    std::string backend;
    get_parameter_or("backend", backend, std::string("xiapi"));
   
    camera_->start();
        
    recorder_->start(
        [this](uint8_t*& d, size_t& sz, uint64_t& t, int& w, int& h, int& s) {
            return camera_->grab(d, sz, t, w, h, s, 100);
        },
        output_path, width_, height_, fps_);

    running_ = true;
    worker_ = std::thread(&CamBufferRecorderNode::run_loop, this);

    RCLCPP_INFO(get_logger(), "Camera active and recording to %s", output_path.c_str());
    return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_deactivate(const rclcpp_lifecycle::State &)
{
    running_ = false;
    if (worker_.joinable()) worker_.join();

    if (recorder_) recorder_->stop();
    if (camera_)  camera_->stop();

    RCLCPP_INFO(get_logger(), "Camera deactivated and recording stopped.");
    return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

void CamBufferRecorderNode::run_loop()
{
    uint8_t* data;
    size_t size;
    int w, h, stride;
    uint64_t ts;

    size_t frame_count = 0;
    auto last_heartbeat = std::chrono::steady_clock::now();

    while (running_ && rclcpp::ok()) {
        if (camera_->grab(data, size, ts, w, h, stride, 100)) {
            frame_count++;

            // Write frame to recorder if active
            // (you can skip this if already handled inside Recorder)
            // recorder_->push_frame(data, size, w, h, ts);

            // Log one frame timestamp every 2s (already there)
            RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 2000,
                                 "Frame ts: %lu (%dx%d, %zu bytes)",
                                 ts, w, h, size);
        }

        // --- Heartbeat: print FPS every second ---
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_heartbeat).count();
        if (elapsed >= 1.0) {
            double fps_est = frame_count / elapsed;
            RCLCPP_INFO(get_logger(), "Heartbeat: %.2f fps (%.0f frames in %.2fs)",
                        fps_est, (double)frame_count, elapsed);
            frame_count = 0;
            last_heartbeat = now;
        }
    }
}


}  // namespace cambuffer_recorder_ng

