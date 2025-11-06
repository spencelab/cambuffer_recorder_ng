#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <thread>
#include <atomic>
#include <memory>

#include "cambuffer_recorder_ng/XiCamera.hpp"
#include "cambuffer_recorder_ng/FakeCamera.hpp"
#include "cambuffer_recorder_ng/GenTLCamera.hpp"
#include "cambuffer_recorder_ng/Recorder.hpp"

namespace cambuffer_recorder_ng
{

/**
 * @brief Lifecycle node that wraps a camera and Recorder.
 *
 * When configured, it initializes the camera.
 * When activated, it starts streaming and recording to disk.
 * When deactivated, it stops and joins all threads.
 */
class CamBufferRecorderNode : public rclcpp_lifecycle::LifecycleNode
{
public:
    CamBufferRecorderNode();

    using CallbackReturn =
        rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

protected:
    CallbackReturn on_configure(const rclcpp_lifecycle::State &) override;
    CallbackReturn on_activate(const rclcpp_lifecycle::State &) override;
    CallbackReturn on_deactivate(const rclcpp_lifecycle::State &) override;

private:
    void run_loop();

    std::shared_ptr<ICamera> camera_;
    //std::shared_ptr<XiCamera> camera_;
    //std::shared_ptr<GenTLCamera> camera_;
    //std::shared_ptr<FakeCamera> camera_;
    std::shared_ptr<Recorder> recorder_;

    std::thread worker_;
    std::atomic<bool> running_{false};

    int width_{640};
    int height_{480};
    int fps_{30};
};

}  // namespace cambuffer_recorder_ng

