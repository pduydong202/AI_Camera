cmake_minimum_required(VERSION 3.16)
project(motion_filter LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
find_package(OpenCV REQUIRED COMPONENTS core imgproc video videoio imgcodecs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
find_path(RGA_INCLUDE_DIR NAMES rga/im2d.hpp)
find_library(RGA_LIBRARY NAMES rga)
if(NOT RGA_INCLUDE_DIR OR NOT RGA_LIBRARY)
  message(FATAL_ERROR "Rockchip librga development files not found (need rga/im2d.hpp and librga.so)")
endif()
add_executable(motion_filter
  src/main.cpp
  src/config.cpp
  src/gst_mpp_source.cpp
  src/motion_pipeline.cpp
  src/event_manager.cpp
  src/recorder.cpp
  src/validation_recorder.cpp)
target_include_directories(motion_filter PRIVATE include ${OpenCV_INCLUDE_DIRS} ${RGA_INCLUDE_DIR})
target_link_libraries(motion_filter PRIVATE ${OpenCV_LIBS} PkgConfig::GSTREAMER ${RGA_LIBRARY} pthread)
target_compile_options(motion_filter PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
add_executable(l2_capture_inspect tools/l2_capture_inspect.cpp)
target_include_directories(l2_capture_inspect PRIVATE include)
target_compile_options(l2_capture_inspect PRIVATE -Wall -Wextra -Wpedantic)
# Safe CPU optimization for Release builds: interprocedural optimization/LTO.
# This does not change algorithm thresholds or runtime logic.
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
if(IPO_SUPPORTED)
  set_property(TARGET motion_filter PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

