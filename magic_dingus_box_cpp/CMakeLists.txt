cmake_minimum_required(VERSION 3.16)
project(magic_dingus_box_cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Fetch spdlog for structured logging
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)
# Use header-only mode for simplicity
set(SPDLOG_HEADER_ONLY ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# Find required libraries using pkg-config
find_package(PkgConfig REQUIRED)

# DRM/KMS, GBM, EGL, OpenGL ES
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)

# Input handling
pkg_check_modules(EVDEV REQUIRED libevdev)

# GPIO (optional - only available on Raspberry Pi)
pkg_check_modules(GPIOD libgpiod)
if(GPIOD_FOUND)
    add_compile_definitions(HAVE_GPIOD)
endif()

# Data parsing
pkg_check_modules(YAMLCPP REQUIRED yaml-cpp)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# GStreamer
pkg_check_modules(GST REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0)
pkg_check_modules(GST_GL REQUIRED gstreamer-gl-1.0)

# Filesystem (C++17 std::filesystem)

# GStreamer is now the only video backend - MPV removed

# Source files
set(PLATFORM_SOURCES
    src/platform/drm_display.cpp
    src/platform/gbm_context.cpp
    src/platform/egl_context.cpp
    src/platform/input_manager.cpp
    src/platform/gpio_manager.cpp
)

set(VIDEO_SOURCES
    src/video/gst_player.cpp
    src/video/gst_renderer.cpp
)

set(UI_SOURCES
    src/ui/renderer.cpp
    src/ui/theme.cpp
    src/ui/font_manager.cpp
    src/ui/settings_menu.cpp
    src/ui/virtual_keyboard.cpp
    src/ui/qrcodegen.cpp
)

set(APP_SOURCES
    src/app/playlist_loader.cpp
    src/app/controller.cpp
    src/app/sample_mode.cpp
    src/app/settings_persistence.cpp
)

set(UTILS_SOURCES
    src/utils/config.cpp
    src/utils/path_resolver.cpp
    src/utils/wifi_manager.cpp
)

set(RETROARCH_SOURCES
    src/retroarch/retroarch_launcher.cpp
)

set(ALL_SOURCES
    src/main.cpp
    ${PLATFORM_SOURCES}
    ${VIDEO_SOURCES}
    ${UI_SOURCES}
    ${APP_SOURCES}
    ${UTILS_SOURCES}
    ${RETROARCH_SOURCES}
)

# Executable
add_executable(magic_dingus_box_cpp ${ALL_SOURCES})

# Include directories
target_include_directories(magic_dingus_box_cpp PRIVATE
    ${DRM_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    ${EVDEV_INCLUDE_DIRS}
    ${GPIOD_INCLUDE_DIRS}
    ${YAMLCPP_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${GST_GL_INCLUDE_DIRS}
    src/
)

# Link libraries
target_link_libraries(magic_dingus_box_cpp
    ${DRM_LIBRARIES}
    ${GBM_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
    ${EVDEV_LIBRARIES}
    ${GPIOD_LIBRARIES}
    ${YAMLCPP_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    ${GST_GL_LIBRARIES}
    spdlog::spdlog_header_only
    pthread
    m
    dl
)

# Compiler flags
target_compile_options(magic_dingus_box_cpp PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Link filesystem library for GCC < 9 (std::filesystem is built-in for GCC 9+)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(magic_dingus_box_cpp stdc++fs)
endif()

# Debug flags
target_compile_definitions(magic_dingus_box_cpp PRIVATE
    $<$<CONFIG:Debug>:DEBUG=1>
)

# Install target (optional)
install(TARGETS magic_dingus_box_cpp
    RUNTIME DESTINATION bin
)

# Copy assets to build directory for development
file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR})

# Copy data directory structure but EXCLUDE large user data (roms, media)
# These are user files that shouldn't be copied during build
file(COPY ${CMAKE_SOURCE_DIR}/data DESTINATION ${CMAKE_BINARY_DIR}
    PATTERN "roms" EXCLUDE
    PATTERN "media" EXCLUDE
    PATTERN "*.chd" EXCLUDE
    PATTERN "*.iso" EXCLUDE
    PATTERN "*.bin" EXCLUDE
    PATTERN "*.cue" EXCLUDE
    PATTERN "*.mp4" EXCLUDE
    PATTERN "*.mkv" EXCLUDE
    PATTERN "*.avi" EXCLUDE
)

# dev_data only needed for local development, skip in production builds
if(EXISTS ${CMAKE_SOURCE_DIR}/dev_data)
    file(COPY ${CMAKE_SOURCE_DIR}/dev_data DESTINATION ${CMAKE_BINARY_DIR}
        PATTERN "roms" EXCLUDE
        PATTERN "media" EXCLUDE
        PATTERN "*.chd" EXCLUDE
        PATTERN "*.iso" EXCLUDE
        PATTERN "*.mp4" EXCLUDE
        PATTERN "*.mkv" EXCLUDE
    )
endif()

