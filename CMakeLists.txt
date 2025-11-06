cmake_minimum_required(VERSION 3.16)
project(cambuffer_recorder_ng)

# =========================
#  ROS2 and dependencies
# =========================
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# =========================
#  FFmpeg (via pkg-config)
# =========================
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale)

# =========================
#  XIMEA Camera SDK (Linux)
# =========================
find_path(XIMEA_INCLUDE_DIR
  NAMES xiApi.h
  PATHS /opt/XIMEA/include /usr/include
)

find_library(XIMEA_LIB
  NAMES m3api
  PATHS /usr/lib /usr/lib64 /opt/XIMEA/lib /opt/XIMEA/lib64
)

if (XIMEA_INCLUDE_DIR AND XIMEA_LIB)
  message(STATUS "Found XIMEA SDK: ${XIMEA_LIB}")
  set(HAVE_XIMEA TRUE)
else()
  message(WARNING "XIMEA SDK not found or incomplete; XiCamera will not link.")
  set(HAVE_XIMEA FALSE)
endif()

if(HAVE_XIMEA)
  include_directories(${XIMEA_INCLUDE_DIR})
  link_directories(/usr/lib /usr/lib64 /opt/XIMEA/lib /opt/XIMEA/lib64)
endif()



# =========================
#  Include directories
# =========================
# Fallback include path if find_path() fails
if (NOT EXISTS ${XIMEA_INCLUDE_DIR}/xiApi.h)
  set(XIMEA_INCLUDE_DIR /opt/XIMEA/include)
endif()

include_directories(
  include
  ${OpenCV_INCLUDE_DIRS}
)

if(HAVE_XIMEA)
  include_directories(${XIMEA_INCLUDE_DIR})
  link_directories(/opt/XIMEA/lib /opt/XIMEA/lib64)
endif()

# =========================
#  Library sources
# =========================
add_library(${PROJECT_NAME}_lib
  src/CamBufferRecorderNode.cpp
  src/Recorder.cpp
  src/FfmpegWriter.cpp
  src/GenTLCamera.cpp
  src/XiCamera.cpp       # XiCamera only compiled; linking optional
)

# =========================
#  Target dependencies
# =========================
ament_target_dependencies(${PROJECT_NAME}_lib
  rclcpp
  rclcpp_lifecycle
  sensor_msgs
  diagnostic_msgs
  OpenCV
)

target_link_libraries(${PROJECT_NAME}_lib
  PkgConfig::FFMPEG
  Threads::Threads
)

if(HAVE_XIMEA)
  target_link_libraries(${PROJECT_NAME}_lib ${XIMEA_LIB})
endif()

# =========================
#  Executable
# =========================
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} ${PROJECT_NAME}_lib)
ament_target_dependencies(${PROJECT_NAME} rclcpp rclcpp_lifecycle)

# =========================
#  Installation
# =========================
install(TARGETS
  ${PROJECT_NAME}
  ${PROJECT_NAME}_lib
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include/
)

install(DIRECTORY launch config msg action
  DESTINATION share/${PROJECT_NAME}/
  OPTIONAL
)

ament_package()

