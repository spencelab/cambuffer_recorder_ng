#include "cambuffer_recorder_ng/CamBufferRecorderNode.hpp"

#include <rclcpp/rclcpp.hpp>

#include <chrono>

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);

  auto node = std::make_shared<cambuffer_recorder_ng::CamBufferRecorderNode>();

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node->get_node_base_interface());

  while (rclcpp::ok()) {
    // This blocks while idle, but wakes immediately for incoming ROS work:
    // services, lifecycle transitions, future dump_trigger topic callbacks, etc.
    executor.spin_once(std::chrono::milliseconds(100));

    // Lifecycle shutdown moves the node to finalized, but would not otherwise
    // make the process exit. Check after each executor wakeup.
    const auto state = node->get_current_state();
    if (state.label() == "finalized") {
      RCLCPP_INFO(node->get_logger(), "Lifecycle state is finalized; exiting process.");
      break;
    }
  }

  executor.remove_node(node->get_node_base_interface());

  if (rclcpp::ok()) {
    rclcpp::shutdown();
  }

  return 0;
}
