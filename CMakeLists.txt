cmake_minimum_required(VERSION 3.16)
project(cambuffer_recorder_ng)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(OpenCV REQUIRED)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale)

include_directories(
  include
  ${OpenCV_INCLUDE_DIRS}
)

add_library(${PROJECT_NAME}_lib
  src/FakeCamera.cpp
  src/BufferPool.cpp
  src/FfmpegWriter.cpp
  src/Recorder.cpp
  src/CamBufferRecorderNode.cpp
)

ament_target_dependencies(${PROJECT_NAME}_lib
  rclcpp rclcpp_lifecycle sensor_msgs diagnostic_msgs
  OpenCV
)
target_link_libraries(${PROJECT_NAME}_lib
  PkgConfig::FFMPEG Threads::Threads
)

add_executable(cambuffer_recorder_ng src/main.cpp)
target_link_libraries(cambuffer_recorder_ng ${PROJECT_NAME}_lib)
ament_target_dependencies(cambuffer_recorder_ng rclcpp rclcpp_lifecycle)

install(TARGETS cambuffer_recorder_ng ${PROJECT_NAME}_lib
  DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include/ DESTINATION include/)
install(DIRECTORY launch config msg action DESTINATION share/${PROJECT_NAME}/)

ament_package()

