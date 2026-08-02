cmake_minimum_required(VERSION 3.16)
project(magic_dingus_box_cpp)

include(CTest)

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

# Unit-test targets (all Catch2 suites + the MB test CLI). Default ON so
# dev builds and CI test jobs are unchanged. Production build paths pass
# -DBUILD_TESTS=OFF — update.sh's on-Pi OTA rebuild and the release
# workflow's binary build were compiling the ENTIRE test suite they never
# run (and fetching Catch2 from GitHub to do it — a needless network
# dependency in the OTA path, on a Pi where every compiled TU costs real
# minutes).
option(BUILD_TESTS "Build unit test targets" ON)

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

# yaml-cpp again, this time off the Linux-only path and NOT required.
#
# test_app_unit compiles playlist_loader.cpp for the trim/loop parsing tests,
# so it needs yaml-cpp headers on whatever machine is running unit tests —
# but the probe above lives inside the Linux-only block, so on a Mac
# YAMLCPP_INCLUDE_DIRS came back empty and the target failed to compile with
# "'yaml-cpp/yaml.h' file not found". The suite silently stopped being part
# of a Mac test run.
#
# Not REQUIRED: a dev box without yaml-cpp should still configure and build
# every other suite, it just skips this one (see the guard on test_app_unit).
if(NOT YAMLCPP_FOUND)
    pkg_check_modules(YAMLCPP yaml-cpp)
endif()

# Filesystem (C++17 std::filesystem)

# GStreamer is now the only video backend - MPV removed

# Source files
set(PLATFORM_SOURCES
    src/platform/drm_display.cpp
    src/platform/gbm_context.cpp
    src/platform/egl_context.cpp
    src/platform/input_manager.cpp
    src/platform/gpio_manager.cpp
    src/platform/platform_profile.cpp
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
    src/ui/controller_wizard.cpp
    src/ui/controller_wizard_renderer.cpp
    src/ui/pairing_screen.cpp
    src/ui/pairing_screen_renderer.cpp
    src/ui/theme.cpp
    src/ui/font_manager.cpp
    src/ui/settings_menu.cpp
    src/ui/virtual_keyboard.cpp
    src/ui/qrcodegen.cpp
)

# toast.cpp is referenced by settings_menu.cpp (Wi-Fi connect toasts) which
# is always compiled into the kiosk binary, so toast.cpp must also always be
# included regardless of ENABLE_MEDIA_BROWSER.
list(APPEND UI_SOURCES src/ui/toast.cpp)

set(APP_SOURCES
    src/app/game_launch_recovery.cpp
    src/app/game_quiet_mode.cpp
    src/app/playlist_loader.cpp
    src/app/rom_title.cpp
    src/app/controller.cpp
    src/app/controller_text_input.cpp
    src/app/sample_mode.cpp
    src/app/settings_persistence.cpp
    src/app/mb_filter_persistence.cpp
    src/app/status_writer.cpp
    src/app/playback_stall_watchdog.cpp
)

set(UTILS_SOURCES
    src/utils/config.cpp
    src/utils/path_resolver.cpp
    src/utils/stb_image_impl.cpp
    src/utils/time_format.cpp
    src/utils/wifi_manager.cpp
)

set(RETROARCH_SOURCES
    src/retroarch/launch_contract.cpp
    src/retroarch/retroarch_launcher.cpp
    src/retroarch/controller_detector.cpp
    src/retroarch/controller_mapping.cpp
    src/retroarch/logical_controls.cpp
    src/retroarch/controller_profile.cpp
    src/retroarch/joydev_index.cpp
    src/retroarch/capture_session.cpp
)

# Retroarch sources shared by BOTH test_retroarch_unit and
# test_phone_remote_unit, below. Those two targets used to maintain fully
# independent source lists even though both compile controller_mapping.cpp,
# which is not self-contained (it delegates to controller_profile.cpp /
# controller_detector.cpp / logical_controls.cpp / launch_contract.cpp) --
# so a new dependency added to any of those files had to be added to BOTH
# targets by hand, and it silently didn't happen once (test_phone_remote_unit
# failed to LINK, not compile, with undefined symbols, while
# test_retroarch_unit kept passing). Defining the shared subset once and
# having both targets consume this variable makes that class of drift
# structurally impossible instead of relying on someone reading a comment.
# (retroarch_launcher.cpp is deliberately NOT in this list -- it's needed by
# only one of the two targets. joydev_index.cpp USED to be excluded on the
# same reasoning, but capture_session.cpp -- itself shared here -- calls
# bind_token(), so joydev_index.cpp must ship to both targets too now.)
set(RETROARCH_TESTABLE_SOURCES
    src/retroarch/launch_contract.cpp
    src/retroarch/controller_detector.cpp
    src/retroarch/controller_mapping.cpp
    src/retroarch/logical_controls.cpp
    src/retroarch/controller_profile.cpp
    src/retroarch/joydev_index.cpp
    src/retroarch/capture_session.cpp
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
            src/media_browser/sonarr/sonarr_parsers.cpp
            src/media_browser/sonarr/sonarr_client.cpp
            src/media_browser/sonarr/sonarr_mock.cpp
            src/media_browser/radarr/radarr_parsers.cpp
            src/media_browser/radarr/radarr_client.cpp
            src/media_browser/radarr/radarr_mock.cpp
            src/media_browser/prowlarr/prowlarr_client.cpp
            src/media_browser/qbittorrent/qbittorrent_client.cpp
            src/media_browser/qbittorrent/download_watchdog.cpp
            src/media_browser/health/vpn_health_monitor.cpp
            src/media_browser/movie_drive.cpp
            src/media_browser/tmdb_client.cpp
            src/media_browser/artwork/artwork_cache.cpp
            src/media_browser/ui/mb_ui_utils.cpp
            # Renderer-free, so it is ALSO in MEDIA_BROWSER_SOURCES and gets
            # unit-tested — but library_screen.cpp calls into it, so the kiosk
            # binary needs its own copy here or the link fails on an undefined
            # build_library_view. Same arrangement as mb_ui_utils.cpp above: the
            # two lists feed different targets and neither includes the other.
            src/media_browser/ui/library_view.cpp
            # Renderer-free For You merge/rank helper (spec 1c step 4). Same
            # dual-list arrangement as mb_ui_utils/library_view above: this
            # file also lives in MEDIA_BROWSER_SOURCES for unit testing, and
            # browse_screen.cpp's For You state machine calls into it, so the
            # kiosk binary needs its own copy here or the link fails on an
            # undefined merge_recommendations.
            src/media_browser/ui/mb_recs.cpp
            src/media_browser/ui/mb_filter_state.cpp
            src/media_browser/ui/browse_screen.cpp
            src/media_browser/ui/search_screen.cpp
            src/media_browser/ui/detail_screen.cpp
            src/media_browser/ui/series_detail_screen.cpp
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
    if(BUILD_TESTS)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.5.2
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    # --- Media browser library sources (expanded in later tasks) ---
    set(MEDIA_BROWSER_SOURCES
        src/media_browser/sonarr/sonarr_parsers.cpp
        src/media_browser/sonarr/sonarr_client.cpp
        src/media_browser/sonarr/sonarr_mock.cpp
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
        # Pure UI helpers shared by every Marquee screen. Deliberately has no
        # ::ui::Renderer dependency (Renderer pulls in GLES, which does not
        # exist in the test binaries) so these four functions are testable
        # here rather than only compilable into the kiosk. The
        # Renderer-shaped truncate_to_width convenience overload lives in
        # mb_chrome.cpp, which stays kiosk-only.
        src/media_browser/ui/mb_ui_utils.cpp
        # The Library grid's filter + sort decision logic, split out of
        # library_screen.cpp for the same reason: it names no ::ui::Renderer, so
        # unlike the screen it lives in it can be compiled and asserted on here.
        # That coverage gap is what let the "recently added" cutoff bug ship.
        src/media_browser/ui/library_view.cpp
        src/media_browser/ui/mb_recs.cpp
        # Renderer-free filter logic (per-mode filter storage, the two genre
        # catalogs, the mode-toggle transaction, both discover-filter
        # builders). Same dual-list arrangement as mb_recs/library_view above:
        # also listed in KIOSK_MEDIA_BROWSER_SOURCES because browse_screen and
        # the filter overlay call into it.
        src/media_browser/ui/mb_filter_state.cpp
    )

    # NOTE: the Media Browser screen .cpp files (browse/search/detail/playback/
    # queue/library/mb_settings/mb_chrome) are NOT listed here. They need a GL
    # Renderer, so they go only into the kiosk binary, via
    # KIOSK_MEDIA_BROWSER_SOURCES above — that list is the single place they are
    # named. A second list of exactly those eight files used to sit here
    # (MEDIA_BROWSER_UI_SOURCES), consumed by no target, with a comment implying
    # it existed to keep them OUT of the test binary. It did nothing, and it sent
    # anyone tracing where those files get compiled down a dead end.

    # --- Unit test runner (and the MB diagnostic CLI) — test-only targets,
    # skipped entirely when BUILD_TESTS=OFF (production OTA/CI builds).
    if(BUILD_TESTS)
    set(MEDIA_BROWSER_TEST_SOURCES
        tests/media_browser/test_smoke.cpp
        tests/media_browser/test_movie_drive.cpp
        tests/media_browser/test_library_db.cpp
        tests/media_browser/test_tmdb_client.cpp
        tests/media_browser/test_tmdb_tv.cpp
        tests/media_browser/test_media_ref.cpp
        tests/media_browser/test_mb_filter_persistence.cpp
        tests/media_browser/test_mb_filter_state.cpp
        tests/media_browser/test_filter_rows.cpp
        tests/media_browser/test_browse_logic.cpp
        tests/media_browser/test_series_detail_logic.cpp
        tests/media_browser/test_episode_logic.cpp
        tests/media_browser/test_queue_groups.cpp
        tests/media_browser/test_mb_recs.cpp
        tests/media_browser/test_sequence_detector.cpp
        tests/media_browser/test_radarr_parsers.cpp
        tests/media_browser/test_radarr_client.cpp
        tests/media_browser/test_sonarr_parsers.cpp
        tests/media_browser/test_sonarr_client.cpp
        tests/media_browser/test_artwork_cache.cpp
        tests/media_browser/test_prowlarr_client.cpp
        tests/media_browser/test_release_picker.cpp
        tests/media_browser/test_download_watchdog.cpp
        tests/media_browser/test_vpn_health_monitor.cpp
        tests/media_browser/test_mb_ui_utils.cpp
        tests/media_browser/test_library_view.cpp
        tests/media_browser/test_queue_screen.cpp
        src/media_browser/health/vpn_health_monitor.cpp
        src/media_browser/movie_drive.cpp
        src/media_browser/artwork/artwork_cache.cpp
        # Renderer-free, jsoncpp-only mapping of the four per-mode filter sets
        # to/from settings.json. Lives in APP_SOURCES for the kiosk; listed
        # here so the legacy-key migration is unit-tested (BUILD_KIOSK=OFF
        # does not compile APP_SOURCES).
        src/app/mb_filter_persistence.cpp
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
    add_test(NAME MediaBrowserUnit COMMAND test_media_browser_unit)

    # --- test_media_browser CLI ---
    #
    # NOT registered with add_test, and must not be: despite the name this is
    # not a test binary. It is a subcommand-driven CLI for exercising the
    # subsystems by hand on real hardware ("test_media_browser tmdb-search
    # alien"). Invoked bare it prints help and returns 2, so a naive
    # add_test() would add a permanently failing test; every real subcommand
    # needs API keys from the environment plus a live TMDB / Radarr /
    # libtorrent stack, which no test run has. Live scaffolding, not dead
    # code — keep building it, just never through ctest.
    #
    # Its name is also why the missing MediaBrowserUnit registration above
    # went unnoticed: two similarly-named targets, one of which genuinely
    # should not be a ctest entry.
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
    endif()  # BUILD_TESTS
endif()

# ============================================================
# Phone Remote feature tests — no Linux-only deps, runs on Mac.
# ============================================================

# Every standalone unit suite below (retroarch/ui/app/platform/display/
# utils/video/phone-remote) is test-only — one gate covers them all.
if(BUILD_TESTS)

# Ensure Catch2 is available (may already be fetched if ENABLE_MEDIA_BROWSER
# is ON; FetchContent_Declare is idempotent with a matching name).
if(NOT TARGET Catch2::Catch2WithMain)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# RetroArch launch-contract tests are deliberately portable so display
# handoff behavior can be verified on a development machine without DRM.
file(GLOB RETROARCH_TEST_SOURCES "tests/retroarch/*.cpp")
if(RETROARCH_TEST_SOURCES)
    add_executable(test_retroarch_unit
        ${RETROARCH_TEST_SOURCES}
        src/app/game_launch_recovery.cpp
        src/app/game_quiet_mode.cpp
        ${RETROARCH_TESTABLE_SOURCES}
        src/utils/config.cpp
    )
    target_include_directories(test_retroarch_unit PRIVATE src ${JSONCPP_INCLUDE_DIRS})
    target_link_directories(test_retroarch_unit PRIVATE ${JSONCPP_LIBRARY_DIRS})
    target_link_libraries(test_retroarch_unit PRIVATE Catch2::Catch2WithMain ${JSONCPP_LIBRARIES} pthread)
    target_compile_options(test_retroarch_unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME RetroArchUnit COMMAND test_retroarch_unit)
endif()

# UI-layer tests that don't need GL. Today: the Controller Setup wizard's
# phase machine, its save/merge into the profile store, and the SettingsMenu
# side of the wizard's lifecycle.
#
# settings_menu.cpp is in here for one reason: toggle()'s wizard_active_ branch
# is the cancel path that ACTUALLY fires on hardware. main.cpp consumes every
# SETTINGS_MENU event in its BTN4 hold/volume block and `continue`s, so that
# event never reaches ControllerWizard::on_action(); the operator's press
# arrives as settings_menu.toggle() instead. That path had zero automated
# coverage while settings_menu.cpp was in no Mac target, on the one screen
# whose failure mode is a bricked appliance with no keyboard attached.
#
# What the Mac build cannot link, tests/ui/ui_test_doubles.cpp defines at the
# linker level (InputManager/libevdev, Toast/GLES, PairingScreen +
# SettingsPersistence/spdlog), so the corresponding .cpp files are absent here.
# wifi_manager / virtual_keyboard / platform_profile compile clean on macOS
# with no project-level link tail, so they are linked for real.
#
# Deliberately does NOT include controller_wizard_renderer.cpp: that one is a
# Renderer method and is covered by the Pi build only.
file(GLOB UI_TEST_SOURCES "tests/ui/*.cpp")
if(UI_TEST_SOURCES)
    add_executable(test_ui_unit
        ${UI_TEST_SOURCES}
        src/ui/controller_wizard.cpp
        src/ui/settings_menu.cpp
        src/ui/virtual_keyboard.cpp
        src/utils/wifi_manager.cpp
        src/platform/platform_profile.cpp
        ${RETROARCH_TESTABLE_SOURCES}
        src/utils/config.cpp
        # controller_wizard.cpp stamps captured_at through this.
        src/utils/time_format.cpp
    )
    target_include_directories(test_ui_unit PRIVATE src tests/ui ${JSONCPP_INCLUDE_DIRS})
    target_link_directories(test_ui_unit PRIVATE ${JSONCPP_LIBRARY_DIRS})
    target_link_libraries(test_ui_unit PRIVATE Catch2::Catch2WithMain ${JSONCPP_LIBRARIES} pthread)
    target_compile_options(test_ui_unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME UiUnit COMMAND test_ui_unit)
endif()

# App-layer pure-logic tests (ROM display titles, etc). No Linux-only deps.
file(GLOB APP_TEST_SOURCES "tests/app/*.cpp")
if(APP_TEST_SOURCES AND NOT YAMLCPP_FOUND)
    message(WARNING
        "yaml-cpp not found — skipping test_app_unit (playlist trim/loop "
        "parsing tests). Install it: brew install yaml-cpp / apt install "
        "libyaml-cpp-dev")
endif()
if(APP_TEST_SOURCES AND YAMLCPP_FOUND)
    add_executable(test_app_unit
        ${APP_TEST_SOURCES}
        src/app/rom_title.cpp
        src/app/playback_stall_watchdog.cpp
        # playlist_loader is here for the trim/loop parsing tests; it pulls in
        # yaml-cpp, hence the extra include/link directories below.
        src/app/playlist_loader.cpp
        # platform_profile backs filter_for_platform (the Pi 4 game-system
        # gate) — pure logic, no Linux-only deps.
        src/platform/platform_profile.cpp
    )
    target_include_directories(test_app_unit PRIVATE src ${YAMLCPP_INCLUDE_DIRS})
    target_link_directories(test_app_unit PRIVATE ${YAMLCPP_LIBRARY_DIRS})
    target_link_libraries(test_app_unit PRIVATE Catch2::Catch2WithMain ${YAMLCPP_LIBRARIES})
    target_compile_options(test_app_unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME AppUnit COMMAND test_app_unit)
endif()

# Platform profile tests — Pi model detection + sink/gpiochip resolution.
# Pure logic, no Linux-only deps, runs on Mac.
file(GLOB PLATFORM_TEST_SOURCES "tests/platform/*.cpp")
if(PLATFORM_TEST_SOURCES)
    add_executable(test_platform_unit
        ${PLATFORM_TEST_SOURCES}
        src/platform/platform_profile.cpp
    )
    target_include_directories(test_platform_unit PRIVATE src)
    target_link_libraries(test_platform_unit PRIVATE Catch2::Catch2WithMain)
    target_compile_options(test_platform_unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME PlatformUnit COMMAND test_platform_unit)
endif()

# Display policy tests — resolution/logical-canvas policy. Pure logic.
file(GLOB DISPLAY_TEST_SOURCES "tests/display/*.cpp")
if(DISPLAY_TEST_SOURCES)
    add_executable(test_display_unit ${DISPLAY_TEST_SOURCES} src/utils/config.cpp)
    target_include_directories(test_display_unit PRIVATE src)
    target_link_libraries(test_display_unit PRIVATE Catch2::Catch2WithMain)
    target_compile_options(test_display_unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME DisplayUnit COMMAND test_display_unit)
endif()

# utils/ tests — leaf helpers with no project dependencies. Today: the shared
# ISO-8601 UTC formatter, which lives in utils/ precisely because its two
# callers are in retroarch/ and media_browser/ and neither should depend on the
# other.
file(GLOB UTILS_TEST_SOURCES "tests/utils/*.cpp")
if(UTILS_TEST_SOURCES)
    add_executable(test_utils_unit ${UTILS_TEST_SOURCES} src/utils/time_format.cpp)
    target_include_directories(test_utils_unit PRIVATE src)
    target_link_libraries(test_utils_unit PRIVATE Catch2::Catch2WithMain)
    target_compile_options(test_utils_unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME UtilsUnit COMMAND test_utils_unit)
endif()

# Stream-gate tests — stale-frame suppression between video streams.
# Pure logic, no GL/GStreamer deps, runs on Mac.
file(GLOB VIDEO_TEST_SOURCES "tests/video/*.cpp")
if(VIDEO_TEST_SOURCES)
    add_executable(test_video_unit ${VIDEO_TEST_SOURCES})
    target_include_directories(test_video_unit PRIVATE src)
    target_link_libraries(test_video_unit PRIVATE Catch2::Catch2WithMain)
    target_compile_options(test_video_unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME VideoUnit COMMAND test_video_unit)
endif()

# Phone remote tests
#
# This target compiles src/retroarch/controller_mapping.cpp, which is NOT
# self-contained -- get_mapping()/resolve_mapping_for_pad() delegate to the
# built-in physical controller profiles and other retroarch/ sources. It
# shares those sources with test_retroarch_unit via RETROARCH_TESTABLE_SOURCES
# (defined above, near RETROARCH_SOURCES) instead of listing them
# independently, so a new dependency added to any of those files reaches both
# test targets automatically -- this target used to list its own copy of the
# source list and had silently drifted out of sync with test_retroarch_unit
# before (a LINK failure, not a compile failure, since the missing piece is
# an undefined symbol).
file(GLOB PHONE_REMOTE_TEST_SOURCES "tests/phone_remote/*.cpp")
if(PHONE_REMOTE_TEST_SOURCES)
    add_executable(test_phone_remote_unit
        ${PHONE_REMOTE_TEST_SOURCES}
        src/app/status_writer.cpp
        src/ui/pairing_screen.cpp
        src/ui/virtual_keyboard.cpp
        src/app/controller_text_input.cpp
        ${RETROARCH_TESTABLE_SOURCES}
        src/retroarch/retroarch_launcher.cpp
        src/utils/config.cpp
    )
    target_include_directories(test_phone_remote_unit PRIVATE src ${JSONCPP_INCLUDE_DIRS})
    target_link_directories(test_phone_remote_unit PRIVATE ${JSONCPP_LIBRARY_DIRS})
    target_link_libraries(test_phone_remote_unit PRIVATE Catch2::Catch2WithMain ${JSONCPP_LIBRARIES} spdlog::spdlog_header_only pthread)
    # std::filesystem needs explicit linking on GCC < 9 (Bookworm ships
    # GCC 12, so this is a no-op there — but matters for older toolchains
    # used in CI / cross-compile.)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
        target_link_libraries(test_phone_remote_unit PRIVATE stdc++fs)
    endif()
    add_test(NAME PhoneRemoteUnit COMMAND test_phone_remote_unit)
    target_compile_options(test_phone_remote_unit PRIVATE -Wall -Wextra)
endif()

endif()  # BUILD_TESTS — standalone unit suites

# ---------------------------------------------------------------------------
# controller_probe — on-Pi diagnostic tool, NOT part of the kiosk binary.
#
# Prints each joystick's live evdev capability lists and the joydev_index
# tokens computed from them, then diffs those tokens against every applicable
# builtin PhysicalProfile. It is the hardware half of the ground truth that
# tests/retroarch/ asserts from transcribed capability lists.
#
# Linux-only: it links libevdev and reads /dev/input/event*, neither of which
# exists on macOS. Gated on EVDEV_FOUND as well as the platform because the
# pkg_check_modules(EVDEV ...) above only runs under BUILD_KIOSK — a Linux
# configure with -DBUILD_KIOSK=OFF must skip this target, not fail on an
# empty ${EVDEV_LIBRARIES}.
#
# Built on demand (`make controller_probe`); EXCLUDE_FROM_ALL keeps it out of
# the default build so the kiosk's build time is unchanged.
if(UNIX AND NOT APPLE AND EVDEV_FOUND)
    add_executable(controller_probe EXCLUDE_FROM_ALL
        src/tools/controller_probe.cpp
        src/retroarch/joydev_index.cpp
        src/retroarch/logical_controls.cpp
        src/retroarch/controller_profile.cpp
        src/utils/config.cpp
    )
    target_include_directories(controller_probe PRIVATE src ${JSONCPP_INCLUDE_DIRS} ${EVDEV_INCLUDE_DIRS})
    target_link_directories(controller_probe PRIVATE ${JSONCPP_LIBRARY_DIRS} ${EVDEV_LIBRARY_DIRS})
    target_link_libraries(controller_probe PRIVATE ${EVDEV_LIBRARIES} ${JSONCPP_LIBRARIES})
    target_compile_options(controller_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()
