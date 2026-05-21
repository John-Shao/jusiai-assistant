# ---------------------------------------------------------------------------
# JuSi AI Assistant — Linux audio/video AI assistant client.
#
# Built on the LiveKit C++ SDK (../livekit-sdk-cpp) and the JuSi Meet device
# API backend (https://meet.jusiai.com). GUI is LVGL v8.4 rendered through
# SDL3; SDL3 also drives microphone / camera capture and speaker playback.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)

project(jusiai-assistant VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Single output tree so the executable, the LiveKit FFI library and SDL3
# end up side by side (the executable's RPATH is $ORIGIN).
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(UNIX AND NOT APPLE)
  set(CMAKE_BUILD_RPATH "$ORIGIN")
  set(CMAKE_INSTALL_RPATH "$ORIGIN")
  set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# LiveKit C++ SDK
#
# LIVEKIT_SDK_DIR must point at a checkout of livekit-sdk-cpp. It is added as a
# sub-project so the `livekit` target (and its transitive protobuf / abseil /
# OpenSSL usage requirements) are available with a guaranteed ABI match.
# ---------------------------------------------------------------------------
set(LIVEKIT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../livekit-sdk-cpp"
    CACHE PATH "Path to the livekit-sdk-cpp source tree")
get_filename_component(LIVEKIT_SDK_DIR "${LIVEKIT_SDK_DIR}" ABSOLUTE)

if(NOT EXISTS "${LIVEKIT_SDK_DIR}/include/livekit/livekit.h")
  message(FATAL_ERROR
    "LiveKit SDK not found at: ${LIVEKIT_SDK_DIR}\n"
    "Pass -DLIVEKIT_SDK_DIR=/path/to/livekit-sdk-cpp when configuring.")
endif()

set(LIVEKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIVEKIT_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIVEKIT_SDK_DIR}" "${CMAKE_BINARY_DIR}/livekit-sdk")

# ---------------------------------------------------------------------------
# Third-party dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)        # HTTPS for the device API (cpp-httplib)

include(sdl3)                         # SDL3 — window, camera, mic, speaker
include(lvgl)                         # LVGL v8.4 — GUI toolkit
include(json)                         # nlohmann/json — device API payloads

# ---------------------------------------------------------------------------
# jusiai-assistant executable
# ---------------------------------------------------------------------------
add_executable(jusiai-assistant
  src/main.cpp
  src/app_config.cpp
  src/api/device_api_client.cpp
  src/rtc/livekit_session.cpp
  src/media/audio_io.cpp
  src/media/camera_io.cpp
  src/core/assistant_controller.cpp
  src/ui/lv_sdl_disp.cpp
  src/ui/ui_app.cpp
  # Vendored SDL3 capture/playback helpers (originally from the SDK examples).
  src/media/sdl_media.cpp
)

target_include_directories(jusiai-assistant PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"  # vendored httplib.h
  "${LIVEKIT_SDK_DIR}/include"
)

target_compile_definitions(jusiai-assistant PRIVATE
  CPPHTTPLIB_OPENSSL_SUPPORT           # enable HTTPS in cpp-httplib
  JUSIAI_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(jusiai-assistant PRIVATE
  livekit
  SDL3::SDL3
  lvgl
  nlohmann_json::nlohmann_json
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(jusiai-assistant PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -Wno-deprecated-declarations)      # WebRTC legacy headers on newer GCC
endif()

# ---------------------------------------------------------------------------
# Stage runtime shared libraries next to the executable ($ORIGIN RPATH).
# ---------------------------------------------------------------------------
add_custom_command(TARGET jusiai-assistant POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:livekit>" "$<TARGET_FILE_DIR:jusiai-assistant>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:livekit_ffi>" "$<TARGET_FILE_DIR:jusiai-assistant>"
  COMMENT "Staging LiveKit shared libraries")

if(UNIX AND NOT APPLE)
  add_custom_command(TARGET jusiai-assistant POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:jusiai-assistant>"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:SDL3::SDL3>"
            "$<TARGET_FILE_DIR:jusiai-assistant>/$<TARGET_SONAME_FILE_NAME:SDL3::SDL3>"
    COMMENT "Staging SDL3 shared library")
else()
  add_custom_command(TARGET jusiai-assistant POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:jusiai-assistant>"
    COMMENT "Staging SDL3 shared library")
endif()

# Ship the sample config alongside the binary.
add_custom_command(TARGET jusiai-assistant POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/config/jusiai-assistant.conf"
          "$<TARGET_FILE_DIR:jusiai-assistant>/jusiai-assistant.conf.sample"
  COMMENT "Staging sample configuration")

install(TARGETS jusiai-assistant RUNTIME DESTINATION bin)
