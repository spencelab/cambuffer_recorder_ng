#include "cambuffer_recorder_ng/CamBufferRecorderNode.hpp"

namespace cambuffer_recorder_ng
{

CamBufferRecorderNode::CamBufferRecorderNode()
    : rclcpp_lifecycle::LifecycleNode("cambuffer_recorder_ng")
{
    declare_parameter<int>("width", 640);
    declare_parameter<int>("height", 480);
    declare_parameter<int>("fps", 30);
    declare_parameter<std::string>("output_path", "/home/spencelab/fakecam_test.mp4");
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_configure(const rclcpp_lifecycle::State &)
{
    width_ = get_parameter("width").as_int();
    height_ = get_parameter("height").as_int();
    fps_ = get_parameter("fps").as_int();

    camera_ = std::make_shared<FakeCamera>(width_, height_, fps_);
    camera_->open();

    RCLCPP_INFO(get_logger(), "Configured FakeCamera %dx%d @ %d fps",
                width_, height_, fps_);
    return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
CamBufferRecorderNode::on_activate(const rclcpp_lifecycle::State &)
{
    camera_->start();

    // start recorder thread writing frames to file
    recorder_ = std::make_shared<Recorder>();
    std::string output_path = get_parameter("output_path").as_string();
    recorder_->start(camera_, output_path, width_, height_, fps_);

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
    int w, h, stride;
    uint64_t ts;

    while (running_ && rclcpp::ok()) {
        if (camera_->grab(data, w, h, stride, ts)) {
            // Throttled log every 2s to avoid spamming console
            RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 2000,
                                 "Frame ts: %lu  (%dx%d)", ts, w, h);
        }
    }
}

}  // namespace cambuffer_recorder_ng

