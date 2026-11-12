# Builds the WPILib Robot Simulation GUI (halsim_gui + glass + wpigui +
# Dear ImGui + ntcore + the sim HAL) as a single-threaded WebAssembly module.
#
# Inputs:
#   ALLWPILIB_DIR  allwpilib checkout at the season's tag, with the season's
#                  patch set already applied (build.sh does both)
#   WPILIB_VERSION version string baked into the module
#
# Use through build.sh; it prepares the checkout and runs this under emcmake.

cmake_minimum_required(VERSION 3.22)
project(wpilib_websim CXX C)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure with emcmake (see build.sh)")
endif()
if(NOT ALLWPILIB_DIR OR NOT EXISTS "${ALLWPILIB_DIR}/simulation/halsim_gui")
  message(FATAL_ERROR "ALLWPILIB_DIR must point at a patched allwpilib checkout")
endif()
if(NOT WPILIB_VERSION)
  message(FATAL_ERROR "WPILIB_VERSION is required")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)

set(R "${ALLWPILIB_DIR}")
set(IM "${R}/thirdparty/imgui_suite")

# ---------------------------------------------------------------- sources ---
function(websim_glob out)
  set(result)
  foreach(dir IN LISTS ARGN)
    if(NOT EXISTS "${dir}")
      message(FATAL_ERROR "Expected upstream directory is missing: ${dir}")
    endif()
    file(GLOB_RECURSE found CONFIGURE_DEPENDS "${dir}/*.cpp" "${dir}/*.c")
    list(APPEND result ${found})
  endforeach()
  list(FILTER result EXCLUDE REGEX "/jni/")
  set(${out} ${result} PARENT_SCOPE)
endfunction()

# Dear ImGui, ImPlot, fonts, stb_image and the GLFW + OpenGL3 backends
file(GLOB IMGUI_SRC CONFIGURE_DEPENDS
  "${IM}/imgui/cpp/*.cpp"
  "${IM}/imgui/cpp/misc/cpp/*.cpp"
  "${IM}/implot/cpp/*.cpp"
  "${IM}/generated/fonts/src/*.cpp")
list(FILTER IMGUI_SRC EXCLUDE REGEX "_demo\\.cpp$")
list(APPEND IMGUI_SRC
  "${IM}/imgui/cpp/backends/imgui_impl_glfw.cpp"
  "${IM}/imgui/cpp/backends/imgui_impl_opengl3.cpp"
  "${IM}/stb/cpp/stb_image.cpp")

file(GLOB WPIUTIL_TP_DIRS "${R}/wpiutil/src/main/native/thirdparty/*/src"
                          "${R}/wpiutil/src/main/native/thirdparty/*/cpp")
websim_glob(WPIUTIL_SRC
  "${R}/wpiutil/src/main/native/cpp"
  "${R}/wpiutil/src/main/native/unix"
  ${WPIUTIL_TP_DIRS})

# Only what Field2d / Mechanism2d and the struct/protobuf decoders need
websim_glob(WPIMATH_SRC
  "${R}/wpimath/src/main/native/cpp/geometry"
  "${R}/wpimath/src/generated/main/native/cpp/wpimath/protobuf")
# Ellipse2d::Nearest() pulls in the Sleipnir optimizer; nothing in the GUI
# calls it, so that one file is left out rather than building a solver.
list(FILTER WPIMATH_SRC EXCLUDE REGEX "/geometry/Ellipse2d\\.cpp$")
list(APPEND WPIMATH_SRC "${R}/wpimath/src/main/native/cpp/MathShared.cpp")

websim_glob(NTCORE_SRC
  "${R}/ntcore/src/main/native/cpp"
  "${R}/ntcore/src/generated/main/native/cpp")

websim_glob(HAL_SRC
  "${R}/hal/src/main/native/sim"
  "${R}/hal/src/main/native/cpp")

set(WPIGUI_SRC
  "${R}/wpigui/src/main/native/cpp/wpigui.cpp"
  "${R}/wpigui/src/main/native/cpp/wpigui_openurl.cpp"
  "${R}/wpigui/src/main/native/cpp/portable-file-dialogs.cpp"
  "${R}/wpigui/src/main/native/webgl/wpigui_webgl.cpp")

websim_glob(GLASS_SRC
  "${R}/glass/src/lib/native/cpp"
  "${R}/glass/src/libnt/native/cpp")

websim_glob(SIMGUI_SRC "${R}/simulation/halsim_gui/src/main/native/cpp")

file(GLOB APP_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# --------------------------------------------------------------- includes ---
file(GLOB WPIUTIL_TP_INC "${R}/wpiutil/src/main/native/thirdparty/*/include")
set(WEBSIM_INCLUDES
  "${R}/wpiutil/src/main/native/include"
  ${WPIUTIL_TP_INC}
  "${R}/wpinet/src/main/native/include"
  "${R}/wpimath/src/main/native/include"
  "${R}/wpimath/src/main/native/thirdparty/eigen/include"
  "${R}/wpimath/src/main/native/thirdparty/gcem/include"
  "${R}/wpimath/src/generated/main/native/include"
  "${R}/wpimath/src/generated/main/native/cpp"
  "${R}/ntcore/src/main/native/include"
  "${R}/ntcore/src/generated/main/native/include"
  "${R}/ntcore/src/main/native/cpp"
  "${R}/hal/src/main/native/include"
  "${R}/hal/src/generated/main/native/include"
  "${R}/hal/src/main/native/sim"
  "${R}/wpigui/src/main/native/include"
  "${R}/glass/src/lib/native/include"
  "${R}/glass/src/libnt/native/include"
  "${R}/simulation/halsim_gui/src/main/native/include"
  "${R}/simulation/halsim_gui/src/main/native/cpp"
  "${R}/fieldImages/src/main/native/include"
  "${IM}/imgui/include"
  "${IM}/imgui/include/misc/cpp"
  "${IM}/imgui/include/backends"
  "${IM}/implot/include"
  "${IM}/generated/fonts/include"
  "${IM}/stb/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../protocol")

# ----------------------------------------------------------------- target ---
set(WEBSIM_PORTS "--use-port=contrib.glfw3")

# Upstream code, compiled as-is: its warnings are not ours to fix.
add_library(upstream OBJECT
  ${IMGUI_SRC} ${WPIUTIL_SRC} ${WPIMATH_SRC} ${NTCORE_SRC} ${HAL_SRC}
  ${WPIGUI_SRC} ${GLASS_SRC} ${SIMGUI_SRC})
target_include_directories(upstream PRIVATE ${WEBSIM_INCLUDES})
target_compile_definitions(upstream PRIVATE IMGUI_IMPL_OPENGL_ES3 NDEBUG)
target_compile_options(upstream PRIVATE -w ${WEBSIM_PORTS})

add_executable(simgui ${APP_SRC} $<TARGET_OBJECTS:upstream>)
# Upstream headers are SYSTEM here so -Wall applies to our code only.
target_include_directories(simgui SYSTEM PRIVATE ${WEBSIM_INCLUDES})
target_include_directories(simgui PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../protocol")
target_compile_definitions(simgui PRIVATE
  IMGUI_IMPL_OPENGL_ES3 NDEBUG WEBSIM_WPILIB_VERSION="${WPILIB_VERSION}")
target_compile_options(simgui PRIVATE -Wall -Wextra -Wno-unused-parameter
  ${WEBSIM_PORTS})

# The Timing window and FPGA clock are redirected to the robot process when
# the native bridge is connected. These are link-time wrappers around the
# local sim HAL's functions, so no upstream source is edited.
set(WEBSIM_WRAPPED
  HAL_GetFPGATime
  HALSIM_PauseTiming HALSIM_ResumeTiming HALSIM_IsTimingPaused
  HALSIM_StepTiming HALSIM_StepTimingAsync
  HALSIM_GetNextNotifierTimeout HALSIM_GetNumNotifiers HALSIM_GetNotifierInfo)
set(WEBSIM_WRAP_FLAGS)
foreach(sym IN LISTS WEBSIM_WRAPPED)
  list(APPEND WEBSIM_WRAP_FLAGS "-Wl,--wrap=${sym}")
endforeach()

target_link_options(simgui PRIVATE
  ${WEBSIM_PORTS}
  ${WEBSIM_WRAP_FLAGS}
  -lwebsocket.js
  -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=1048576
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createSimGui
  -sENVIRONMENT=web
  -sEXPORTED_RUNTIME_METHODS=FS,ccall,stringToNewUTF8,UTF8ToString
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free
  -sERROR_ON_UNDEFINED_SYMBOLS=1
  -sEXIT_RUNTIME=0
  -sFILESYSTEM=1
  --closure=0)
set_target_properties(simgui PROPERTIES SUFFIX ".js")
