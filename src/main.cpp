#include "cambuffer_recorder_ng/CamBufferRecorderNode.hpp"
#include <rclcpp/rclcpp.hpp>

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<cambuffer_recorder_ng::CamBufferRecorderNode>();
  rclcpp::spin(node->get_node_base_interface());
  rclcpp::shutdown();
  return 0;
}

