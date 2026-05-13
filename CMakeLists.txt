cmake_minimum_required(VERSION 3.16)
project(cambuffer_recorder_ng)

# =========================
#  C++ standard
# =========================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
#  FFmpeg via pkg-config
# =========================
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavcodec
  libavformat
  libavutil
  libswscale
)

# =========================
#  Backend source lists
# =========================
set(CAMERA_SOURCES
  src/FakeCamera.cpp
)

set(CAMERA_INCLUDE_DIRS "")
set(CAMERA_LIBS "")
set(CAMERA_COMPILE_DEFS "")

# =========================
#  XIMEA Camera SDK detection
# =========================
find_path(XIMEA_INCLUDE_DIR
  NAMES xiApi.h
  PATHS
    /opt/XIMEA/include
    /usr/include
    /usr/local/include
)

find_library(XIMEA_LIB
  NAMES m3api
  PATHS
    /opt/XIMEA/lib
    /opt/XIMEA/lib64
    /usr/lib
    /usr/lib64
    /usr/local/lib
)

if(XIMEA_INCLUDE_DIR AND XIMEA_LIB)
  message(STATUS "XIMEA SDK found")
  message(STATUS "  XIMEA include: ${XIMEA_INCLUDE_DIR}")
  message(STATUS "  XIMEA library: ${XIMEA_LIB}")

  list(APPEND CAMERA_SOURCES
    src/XiCamera.cpp
  )

  list(APPEND CAMERA_INCLUDE_DIRS
    ${XIMEA_INCLUDE_DIR}
  )

  list(APPEND CAMERA_LIBS
    ${XIMEA_LIB}
  )

  list(APPEND CAMERA_COMPILE_DEFS
    HAVE_XIMEA=1
  )
else()
  message(STATUS "XIMEA SDK not found; XIMEA backend will be unavailable at runtime.")
endif()

# =========================
#  GenTL detection
# =========================
find_path(GENTL_INCLUDE_DIR
  NAMES GenTL_v1_5.h GenTL.h
  PATHS
    /opt/XIMEA/samples/_libs/GenTL
    /opt/XIMEA/API/GenTL
    /opt/gentl/include
    /usr/include
    /usr/local/include
)

if(GENTL_INCLUDE_DIR)
  message(STATUS "GenTL headers found")
  message(STATUS "  GenTL include: ${GENTL_INCLUDE_DIR}")

  list(APPEND CAMERA_SOURCES
    src/GenTLCamera.cpp
  )

  list(APPEND CAMERA_INCLUDE_DIRS
    ${GENTL_INCLUDE_DIR}
  )

  list(APPEND CAMERA_LIBS
    dl
  )

  list(APPEND CAMERA_COMPILE_DEFS
    HAVE_GENTL=1
  )
else()
  message(STATUS "GenTL headers not found; GenTL backend will be unavailable at runtime.")
endif()

# =========================
#  Main library
# =========================
add_library(${PROJECT_NAME}_lib
  src/BufferPool.cpp
  src/CamBufferRecorderNode.cpp
  src/Recorder.cpp
  src/FfmpegWriter.cpp
  src/settings/CameraSettings.cpp
  src/settings/SettingsManager.cpp
  src/settings/SettingsSerialization.cpp
  src/raw/RawRollingWriter.cpp
  src/raw/RollingRawRecorder.cpp
  ${CAMERA_SOURCES}
)

target_include_directories(${PROJECT_NAME}_lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${OpenCV_INCLUDE_DIRS}
  PRIVATE
    ${CAMERA_INCLUDE_DIRS}
)

target_compile_definitions(${PROJECT_NAME}_lib
  PRIVATE
    ${CAMERA_COMPILE_DEFS}
)

ament_target_dependencies(${PROJECT_NAME}_lib
  rclcpp
  rclcpp_lifecycle
  sensor_msgs
  std_msgs
  diagnostic_msgs
  OpenCV
)

target_link_libraries(${PROJECT_NAME}_lib
  PkgConfig::FFMPEG
  Threads::Threads
  ${CAMERA_LIBS}
)

# =========================
#  Executable
# =========================
add_executable(${PROJECT_NAME}
  src/main.cpp
)

target_link_libraries(${PROJECT_NAME}
  ${PROJECT_NAME}_lib
)

ament_target_dependencies(${PROJECT_NAME}
  rclcpp
  rclcpp_lifecycle
)

# =========================
#  Raw rolling inspection/conversion tools
# =========================
add_executable(raw_rolling_info src/tools/raw_rolling_info.cpp)
target_link_libraries(raw_rolling_info ${PROJECT_NAME}_lib)
ament_target_dependencies(raw_rolling_info rclcpp OpenCV)

add_executable(raw_rolling_to_mp4 src/tools/raw_rolling_to_mp4.cpp)
target_link_libraries(raw_rolling_to_mp4 ${PROJECT_NAME}_lib)
ament_target_dependencies(raw_rolling_to_mp4 rclcpp OpenCV)

# =========================
#  Install
# =========================
install(TARGETS
  ${PROJECT_NAME}
  ${PROJECT_NAME}_lib
  raw_rolling_info
  raw_rolling_to_mp4
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include/
)

install(DIRECTORY launch config msg action
  DESTINATION share/${PROJECT_NAME}/
  OPTIONAL
)

ament_package()
