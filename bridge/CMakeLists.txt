# Builds halsim_webbridge, the HAL simulation extension that feeds the web
# Sim GUI from inside the robot process.
#
# It links against WPILib's published desktop libraries (hal, wpinet,
# wpiutil); WPILIB_SDK must contain their unpacked headers and shared
# libraries. Use build.sh, which downloads them and packages the result.

cmake_minimum_required(VERSION 3.22)
project(halsim_webbridge CXX)

if(NOT WPILIB_SDK OR NOT EXISTS "${WPILIB_SDK}/include/hal/HAL.h")
  message(FATAL_ERROR "WPILIB_SDK must point at unpacked WPILib headers and libraries (see build.sh)")
endif()
if(NOT WPILIB_VERSION)
  message(FATAL_ERROR "WPILIB_VERSION is required")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

add_library(halsim_webbridge SHARED src/bridge.cpp)
target_include_directories(halsim_webbridge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../protocol")
target_include_directories(halsim_webbridge SYSTEM PRIVATE "${WPILIB_SDK}/include")
target_compile_definitions(halsim_webbridge PRIVATE WEBSIM_WPILIB_VERSION="${WPILIB_VERSION}")
target_compile_options(halsim_webbridge PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_directories(halsim_webbridge PRIVATE "${WPILIB_SDK}/lib")
# The robot process has already loaded these; the extension only refers to them.
target_link_libraries(halsim_webbridge PRIVATE wpiHal wpinet wpiutil)
target_link_options(halsim_webbridge PRIVATE -Wl,--no-undefined -Wl,--as-needed)
# GradleRIO extracts every native library into one directory; find WPILib's
# next to this one, as WPILib's own extensions do.
set_target_properties(halsim_webbridge PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
