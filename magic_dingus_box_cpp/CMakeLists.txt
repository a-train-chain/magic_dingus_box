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

# Detect whether to build the kiosk binary. Requires Linux-only packages
# (DRM/KMS/GBM/EGL/evdev). On non-Linux platforms (e.g. macOS dev machines)
# we skip these so developers can still build ENABLE_MEDIA_BROWSER targets.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BUILD_KIOSK_DEFAULT ON)
else()
    set(BUILD_KIOSK_DEFAULT OFF)
endif()
option(BUILD_KIOSK "Build the kiosk executable (requires Linux kiosk deps)" ${BUILD_KIOSK_DEFAULT})

# Declare ENABLE_MEDIA_BROWSER up here, BEFORE any source-list block uses it.
# Previously this option() was declared near line 268 (next to the
# MEDIA_BROWSER_SOURCES block) but the if(ENABLE_MEDIA_BROWSER) gates at
# lines ~88, ~109, and ~146 fired earlier in CMake's parse-time evaluation.
# Behavior was masked by CMake's cache (a -DENABLE_MEDIA_BROWSER=ON command
# line stored a cache entry that survived re-runs), but a fresh cmake-gui
# session or a clean cache silently dropped sequence_detector.cpp and
# toast.cpp from the build even when the flag was set.
option(ENABLE_MEDIA_BROWSER "Build experimental Media Browser feature" OFF)

if(BUILD_KIOSK)
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

    # Systemd notification (optional - for watchdog support)
    pkg_check_modules(SYSTEMD libsystemd)
    if(SYSTEMD_FOUND)
        add_compile_definitions(HAVE_SYSTEMD)
    endif()

    # GStreamer (video backend, Linux-only for kiosk)
    pkg_check_modules(GST REQUIRED gstreamer-1.0)
    pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
    pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0)
    pkg_check_modules(GST_GL REQUIRED gstreamer-gl-1.0)

    # YAML config parsing (kiosk-only — media_browser uses JSON)
    pkg_check_modules(YAMLCPP REQUIRED yaml-cpp)
endif()

# JSON parsing (cross-platform — used by both kiosk settings and media_browser)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

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

# Media Browser unlock sequence detector — compiled into the kiosk binary
# when ENABLE_MEDIA_BROWSER is on (also linked into test_media_browser_unit
# via MEDIA_BROWSER_SOURCES further down).
if(ENABLE_MEDIA_BROWSER)
    list(APPEND PLATFORM_SOURCES src/platform/sequence_detector.cpp)
endif()

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

# toast.cpp is Media Browser V2 chrome — only compile it when the feature
# flag is on so the ENABLE_MEDIA_BROWSER=OFF build stays bit-identical to
# pre-V2 baseline (invariant enforced by CI golden_image preflight).
if(ENABLE_MEDIA_BROWSER)
    list(APPEND UI_SOURCES src/ui/toast.cpp)
endif()

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
    src/retroarch/controller_detector.cpp
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

if(BUILD_KIOSK)
    # Media Browser UI + Radarr client sources are linked into the kiosk
    # binary when the feature flag is on. Torrent/library-db stay test-only
    # for now (Tasks 24+ will wire them in).
    set(KIOSK_MEDIA_BROWSER_SOURCES "")
    if(ENABLE_MEDIA_BROWSER)
        list(APPEND KIOSK_MEDIA_BROWSER_SOURCES
            src/media_browser/radarr/radarr_parsers.cpp
            src/media_browser/radarr/radarr_client.cpp
            src/media_browser/radarr/radarr_mock.cpp
            src/media_browser/prowlarr/prowlarr_client.cpp
            src/media_browser/qbittorrent/qbittorrent_client.cpp
            src/media_browser/qbittorrent/download_watchdog.cpp
            src/media_browser/tmdb_client.cpp
            src/media_browser/artwork/artwork_cache.cpp
            src/media_browser/ui/browse_screen.cpp
            src/media_browser/ui/search_screen.cpp
            src/media_browser/ui/detail_screen.cpp
            src/media_browser/ui/playback_screen.cpp
            src/media_browser/ui/queue_screen.cpp
            src/media_browser/ui/release_picker_screen.cpp
            src/media_browser/ui/library_screen.cpp
            src/media_browser/ui/mb_settings_screen.cpp
            src/media_browser/ui/mb_chrome.cpp
            src/media_browser/ui/mb_filter_overlay.cpp
            src/media_browser/ui/mb_exit_modal.cpp
            src/media_browser/ui/stall_prompt_modal.cpp
            src/media_browser/ui/playback_overlay.cpp
        )
        # libcurl is required by radarr_client.cpp.
        pkg_check_modules(CURL REQUIRED libcurl)
    endif()

    # Executable
    add_executable(magic_dingus_box_cpp ${ALL_SOURCES} ${KIOSK_MEDIA_BROWSER_SOURCES})

    # Include directories
    target_include_directories(magic_dingus_box_cpp PRIVATE
        ${DRM_INCLUDE_DIRS}
        ${GBM_INCLUDE_DIRS}
        ${EGL_INCLUDE_DIRS}
        ${GLES2_INCLUDE_DIRS}
        ${EVDEV_INCLUDE_DIRS}
        ${GPIOD_INCLUDE_DIRS}
        ${SYSTEMD_INCLUDE_DIRS}
        ${YAMLCPP_INCLUDE_DIRS}
        ${JSONCPP_INCLUDE_DIRS}
        ${GST_INCLUDE_DIRS}
        ${GST_APP_INCLUDE_DIRS}
        ${GST_VIDEO_INCLUDE_DIRS}
        ${GST_GL_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
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
        ${SYSTEMD_LIBRARIES}
        ${YAMLCPP_LIBRARIES}
        ${JSONCPP_LIBRARIES}
        ${GST_LIBRARIES}
        ${GST_APP_LIBRARIES}
        ${GST_VIDEO_LIBRARIES}
        ${GST_GL_LIBRARIES}
        ${CURL_LIBRARIES}
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

    # Suppress -Wdangling-reference noise from spdlog/fmt (GCC 13+ flags
    # internal arg_mapper template instantiations as having dangling refs
    # — false positive in upstream code, drowns out 70+ lines per build
    # and hides real warnings). This is GCC-only; Clang doesn't emit the
    # warning. Apply at the kiosk-target level so it propagates to all
    # spdlog usage without affecting our own code's warning visibility.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "13.0")
        target_compile_options(magic_dingus_box_cpp PRIVATE -Wno-dangling-reference)
    endif()

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
endif()

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

# ============================================================
# Media Browser feature (Phase 1+) — disabled by default.
# When ON, builds libtorrent-backed movie/TV browser scaffolding
# and a standalone test_media_browser executable.
# Note: option() is declared at the top of the file (next to
# BUILD_KIOSK) so the if(ENABLE_MEDIA_BROWSER) gates earlier in
# the file see the flag. Don't move it back here.
# ============================================================

if(ENABLE_MEDIA_BROWSER)
    message(STATUS "Media Browser: ENABLED")
    add_compile_definitions(MEDIA_BROWSER_ENABLED)

    # --- Dependencies (system packages) ---
    pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    pkg_check_modules(CURL REQUIRED libcurl)
    find_package(Boost REQUIRED)  # libtorrent header dependency (headers only)

    # --- Catch2 v3 via FetchContent (header-only test framework) ---
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)

    # --- Media browser library sources (expanded in later tasks) ---
    set(MEDIA_BROWSER_SOURCES
        src/media_browser/library/library_db.cpp
        src/media_browser/tmdb_client.cpp
        src/media_browser/torrent/torrent_session.cpp
        src/media_browser/radarr/radarr_parsers.cpp
        src/media_browser/radarr/radarr_client.cpp
        src/media_browser/radarr/radarr_mock.cpp
        src/media_browser/prowlarr/prowlarr_client.cpp
        src/media_browser/qbittorrent/qbittorrent_client.cpp
        src/media_browser/qbittorrent/download_watchdog.cpp
        src/platform/sequence_detector.cpp
    )

    # Task 17: screen dispatcher + stub screens. Header-only base class
    # (mb_screen.h) + 6 concrete .cpp files. Kept in a separate list so
    # the test binary (which has no Renderer) doesn't need to link them.
    set(MEDIA_BROWSER_UI_SOURCES
        src/media_browser/ui/browse_screen.cpp
        src/media_browser/ui/search_screen.cpp
        src/media_browser/ui/detail_screen.cpp
        src/media_browser/ui/playback_screen.cpp
        src/media_browser/ui/queue_screen.cpp
        src/media_browser/ui/library_screen.cpp
        src/media_browser/ui/mb_settings_screen.cpp
        src/media_browser/ui/mb_chrome.cpp
    )

    # --- Unit test runner ---
    set(MEDIA_BROWSER_TEST_SOURCES
        tests/media_browser/test_smoke.cpp
        tests/media_browser/test_library_db.cpp
        tests/media_browser/test_tmdb_client.cpp
        tests/media_browser/test_sequence_detector.cpp
        tests/media_browser/test_radarr_parsers.cpp
        tests/media_browser/test_radarr_client.cpp
        tests/media_browser/test_artwork_cache.cpp
        tests/media_browser/test_prowlarr_client.cpp
        tests/media_browser/test_release_picker.cpp
        tests/media_browser/test_download_watchdog.cpp
        src/media_browser/artwork/artwork_cache.cpp
        ${MEDIA_BROWSER_SOURCES}
    )

    add_executable(test_media_browser_unit ${MEDIA_BROWSER_TEST_SOURCES})
    # The artwork cache compiles into tests in "test mode" — no GL, no
    # libcurl, so the unit test binary stays portable (runs on mac dev
    # box). The production kiosk build compiles artwork_cache.cpp
    # without this define, enabling the real libcurl + GLES upload path.
    #
    # NOTE: `set_source_files_properties(... COMPILE_DEFINITIONS ...)`
    # applies globally to every target that compiles those files — it
    # would leak the TEST_MODE flag into the kiosk binary too. Use
    # target_compile_definitions on the test-only target so the flag
    # is scoped correctly.
    target_compile_definitions(test_media_browser_unit PRIVATE
        ARTWORK_CACHE_TEST_MODE
    )
    target_include_directories(test_media_browser_unit PRIVATE
        src/
        ${LIBTORRENT_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
        ${JSONCPP_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}
    )
    target_link_directories(test_media_browser_unit PRIVATE
        ${LIBTORRENT_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS}
        ${CURL_LIBRARY_DIRS}
        ${JSONCPP_LIBRARY_DIRS}
    )
    target_link_libraries(test_media_browser_unit PRIVATE
        Catch2::Catch2WithMain
        ${LIBTORRENT_LIBRARIES}
        ${SQLITE3_LIBRARIES}
        ${CURL_LIBRARIES}
        ${JSONCPP_LIBRARIES}
        spdlog::spdlog_header_only
        pthread
    )
    target_compile_options(test_media_browser_unit PRIVATE -Wall -Wextra)

    # --- test_media_browser CLI ---
    add_executable(test_media_browser
        src/media_browser/test_cli/main.cpp
        ${MEDIA_BROWSER_SOURCES}
    )
    target_include_directories(test_media_browser PRIVATE
        src/
        ${LIBTORRENT_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
        ${JSONCPP_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}
    )
    target_link_directories(test_media_browser PRIVATE
        ${LIBTORRENT_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS}
        ${CURL_LIBRARY_DIRS}
        ${JSONCPP_LIBRARY_DIRS}
    )
    target_link_libraries(test_media_browser PRIVATE
        ${LIBTORRENT_LIBRARIES}
        ${SQLITE3_LIBRARIES}
        ${CURL_LIBRARIES}
        ${JSONCPP_LIBRARIES}
        spdlog::spdlog_header_only
        pthread
    )
    target_compile_options(test_media_browser PRIVATE -Wall -Wextra)
endif()
