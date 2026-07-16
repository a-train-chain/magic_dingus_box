# RetroArch KMS Launch Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make all seven shipped RetroArch cores take over the Pi's DRM/KMS display reliably, cancel startup after 15 seconds when takeover fails, and return to a usable kiosk without changing the established Modern TV or CRT rendering contracts.

**Architecture:** Extract the generated RetroArch video and startup contracts into a small cross-platform module covered by Catch2. The shell side detects the real RetroArch PID opening a KMS card; the C++ parent enforces a 15-second deadline over the entire launcher preamble, terminates the launch process group on failure, and then uses the existing DRM/input/audio recovery path. The Pi smoke harness counts a launch only after the current RetroArch process publishes the KMS-ready marker.

**Tech Stack:** C++17, Catch2 v3, Bash, Python 3, DRM/KMS, Vulkan, RetroArch 1.20.0, CMake/CTest, Bats, systemd journal.

## Global Constraints

- Keep the kiosk UI output at its current 1280x720 mode.
- Keep Modern TV RetroArch output at 1920x1080.
- Keep the Modern TV custom viewport exactly `(251, 10, 1415, 1059)` with aspect-ratio index 22.
- Keep CRT Native at 640x480 with custom viewport disabled and aspect-ratio index 23.
- Keep `video_driver="vulkan"`, `video_threaded="false"`, `video_max_swapchain_images="2"`, `video_vsync="true"`, `video_frame_delay="4"`, shaders off, and smoothing off.
- Add only the explicit `video_context_driver="khr_display"` selection to the Vulkan video contract.
- Keep PS1 native 1x rendering, per-core options, controller mappings, rotation, saves, audio routing, and volume calculations unchanged.
- Production startup timeout is exactly 15 seconds; shorter values are injectable only through the launch-supervisor options used by tests.
- Once KMS readiness is observed, startup supervision stops polling and adds no gameplay overhead.
- Do not change emulator cores, ROMs, BIOS files, bezel assets, movie playback, system clocks, governor, or overclock settings.

---

### Task 1: Freeze the RetroArch video contract and select KMS explicitly

**Files:**
- Create: `magic_dingus_box_cpp/src/retroarch/launch_contract.h`
- Create: `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp`
- Create: `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/retroarch_launcher.h`
- Modify: `magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp:1590`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt:138`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt:439`

**Interfaces:**
- Consumes: `app::DisplayMode`, `config::get_bezels_dir()`.
- Produces: `retroarch::LaunchOptions` and `void retroarch::write_video_config(std::ostream&, const LaunchOptions&)`.

- [ ] **Step 1: Write the failing Modern TV and CRT contract tests**

Create `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp` with:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <string>

#include "retroarch/launch_contract.h"

namespace {
void require_line(const std::string& cfg, const std::string& line) {
    REQUIRE(cfg.find(line + "\n") != std::string::npos);
}
}

TEST_CASE("Modern TV keeps its native-core 4:3 bezel contract", "[retroarch][video]") {
    retroarch::LaunchOptions opts;
    opts.display_mode = app::DisplayMode::MODERN_TV;
    opts.bezel_file = "mdb_kv19.png";
    std::ostringstream out;
    retroarch::write_video_config(out, opts);
    const std::string cfg = out.str();

    require_line(cfg, "video_driver = \"vulkan\"");
    require_line(cfg, "video_context_driver = \"khr_display\"");
    require_line(cfg, "video_threaded = \"false\"");
    require_line(cfg, "video_max_swapchain_images = \"2\"");
    require_line(cfg, "video_vsync = \"true\"");
    require_line(cfg, "video_frame_delay = \"4\"");
    require_line(cfg, "video_shader_enable = \"false\"");
    require_line(cfg, "video_smooth = \"false\"");
    require_line(cfg, "video_fullscreen_x = \"1920\"");
    require_line(cfg, "video_fullscreen_y = \"1080\"");
    require_line(cfg, "video_custom_viewport_enable = \"true\"");
    require_line(cfg, "video_custom_viewport_x = \"251\"");
    require_line(cfg, "video_custom_viewport_y = \"10\"");
    require_line(cfg, "video_custom_viewport_width = \"1415\"");
    require_line(cfg, "video_custom_viewport_height = \"1059\"");
    require_line(cfg, "aspect_ratio_index = \"22\"");
    require_line(cfg, "input_overlay_enable = \"true\"");
    require_line(cfg, "input_overlay_opacity = \"1.0\"");
    require_line(cfg, "input_overlay_hide_in_menu = \"true\"");
}

TEST_CASE("CRT Native remains 640x480 without a custom viewport", "[retroarch][video]") {
    retroarch::LaunchOptions opts;
    opts.display_mode = app::DisplayMode::CRT_NATIVE;
    opts.bezel_file = "mdb_kv19.png";
    std::ostringstream out;
    retroarch::write_video_config(out, opts);
    const std::string cfg = out.str();

    require_line(cfg, "video_context_driver = \"khr_display\"");
    require_line(cfg, "video_fullscreen_x = \"640\"");
    require_line(cfg, "video_fullscreen_y = \"480\"");
    require_line(cfg, "video_custom_viewport_enable = \"false\"");
    require_line(cfg, "aspect_ratio_index = \"23\"");
    REQUIRE(cfg.find("input_overlay =") == std::string::npos);
}
```

Add a `test_retroarch_unit` target after Catch2 is available:

```cmake
file(GLOB RETROARCH_TEST_SOURCES "tests/retroarch/*.cpp")
if(RETROARCH_TEST_SOURCES)
    add_executable(test_retroarch_unit
        ${RETROARCH_TEST_SOURCES}
        src/utils/config.cpp
    )
    target_include_directories(test_retroarch_unit PRIVATE src)
    target_link_libraries(test_retroarch_unit PRIVATE Catch2::Catch2WithMain pthread)
    target_compile_options(test_retroarch_unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME RetroArchUnit COMMAND test_retroarch_unit)
endif()
```

- [ ] **Step 2: Run the target and verify RED**

Run:

```bash
cmake -S magic_dingus_box_cpp -B magic_dingus_box_cpp/build -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4
```

Expected: compilation fails because `retroarch/launch_contract.h` and `write_video_config` do not exist as a public production contract.

- [ ] **Step 3: Extract the production video writer and add KMS**

Create `launch_contract.h`:

```cpp
#pragma once

#include <chrono>
#include <iosfwd>
#include <string>
#include <sys/types.h>

#include "app/app_state.h"

namespace retroarch {

struct LaunchOptions {
    app::DisplayMode display_mode = app::DisplayMode::CRT_NATIVE;
    std::string bezel_file;
};

void write_video_config(std::ostream& out, const LaunchOptions& opts);

}  // namespace retroarch
```

Move the existing `RetroArchLauncher::write_video_config` implementation byte-for-byte into `launch_contract.cpp`, change it to the free function above, and insert this one new line immediately after the Vulkan driver:

```cpp
out << "video_driver = \"vulkan\"\n";
out << "video_context_driver = \"khr_display\"\n";
```

Move `LaunchOptions` out of `retroarch_launcher.h`, include `launch_contract.h` there, remove the private writer declaration, add `launch_contract.cpp` to `RETROARCH_SOURCES`, and add it to `test_retroarch_unit`.

- [ ] **Step 4: Run the focused tests and verify GREEN**

Run:

```bash
cmake -S magic_dingus_box_cpp -B magic_dingus_box_cpp/build -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4
ctest --test-dir magic_dingus_box_cpp/build -R RetroArchUnit --output-on-failure
```

Expected: `RetroArchUnit` passes both cases with zero failures.

- [ ] **Step 5: Commit the video contract**

```bash
git add magic_dingus_box_cpp/CMakeLists.txt \
  magic_dingus_box_cpp/src/retroarch/launch_contract.h \
  magic_dingus_box_cpp/src/retroarch/launch_contract.cpp \
  magic_dingus_box_cpp/src/retroarch/retroarch_launcher.h \
  magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp
git commit -m "fix(retroarch): pin Vulkan launches to KMS"
```

---

### Task 2: Add a real KMS-readiness detector and bounded process supervisor

**Files:**
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.h`
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp`
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp`

**Interfaces:**
- Consumes: a shell-safe RetroArch command, launcher PID, ready-marker path, DRM card pattern, and timeout.
- Produces: `build_kms_ready_watch_block`, `wait_for_startup`, `terminate_process_group`, and `StartupStatus`.

- [ ] **Step 1: Write failing tests for readiness, early exit, and timeout**

Extend the test file with real subprocess tests, not mocks:

```cpp
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
std::string temp_path(const char* leaf) {
    return (fs::temp_directory_path() /
            (std::string("mdb-ra-") + std::to_string(getpid()) + "-" + leaf)).string();
}

pid_t spawn_group(const std::string& command) {
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        setpgid(0, 0);
        execl("/bin/bash", "bash", "-c", command.c_str(), nullptr);
        _exit(127);
    }
    setpgid(pid, pid);
    return pid;
}
}

TEST_CASE("KMS marker makes startup ready without waiting for game exit", "[retroarch][startup]") {
    const std::string marker = temp_path("ready");
    fs::remove(marker);
    pid_t pid = spawn_group("sleep 0.10; printf '1234\\n' > '" + marker + "'; sleep 5");
    REQUIRE(retroarch::wait_for_startup(pid, marker, 2s, 20ms) ==
            retroarch::StartupStatus::Ready);
    REQUIRE(retroarch::terminate_process_group(pid, 500ms));
    fs::remove(marker);
}

TEST_CASE("child exit before KMS marker is a startup failure", "[retroarch][startup]") {
    const std::string marker = temp_path("early-ready");
    fs::remove(marker);
    pid_t pid = spawn_group("exit 7");
    REQUIRE(retroarch::wait_for_startup(pid, marker, 2s, 20ms) ==
            retroarch::StartupStatus::Exited);
    fs::remove(marker);
}

TEST_CASE("startup timeout terminates the entire launch group", "[retroarch][startup]") {
    const std::string marker = temp_path("timeout-ready");
    fs::remove(marker);
    pid_t pid = spawn_group("sleep 10");
    REQUIRE(retroarch::wait_for_startup(pid, marker, 200ms, 20ms) ==
            retroarch::StartupStatus::TimedOut);
    REQUIRE(retroarch::terminate_process_group(pid, 200ms));
    int status = 0;
    REQUIRE(waitpid(pid, &status, WNOHANG) == -1);
    fs::remove(marker);
}

TEST_CASE("generated watcher removes compositor hints and publishes real PID", "[retroarch][startup]") {
    retroarch::ReadyWatchOptions opts;
    opts.ready_file = "/tmp/mdb-ready";
    opts.drm_card_pattern = "/dev/dri/card*";
    const std::string block = retroarch::build_kms_ready_watch_block("/usr/bin/retroarch --verbose", opts);
    REQUIRE(block.find("unset DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE SDL_VIDEODRIVER") != std::string::npos);
    REQUIRE(block.find("/proc/$RETROARCH_PID/fd/*") != std::string::npos);
    REQUIRE(block.find("printf '%s\\n' \"$RETROARCH_PID\"") != std::string::npos);
    REQUIRE(block.find("/dev/dri/card*") != std::string::npos);
}
```

- [ ] **Step 2: Run the focused target and verify RED**

Run:

```bash
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4
```

Expected: compile failure for the missing readiness/supervisor interfaces.

- [ ] **Step 3: Implement the minimal production supervisor**

Add these declarations to `launch_contract.h`:

```cpp
enum class StartupStatus { Ready, Exited, TimedOut, WaitError };

struct ReadyWatchOptions {
    std::string ready_file = "/tmp/retroarch_mdb.ready";
    std::string drm_card_pattern = "/dev/dri/card*";
};

std::string build_kms_ready_watch_block(const std::string& command,
                                        const ReadyWatchOptions& opts);
StartupStatus wait_for_startup(pid_t launcher_pid,
                               const std::string& ready_file,
                               std::chrono::milliseconds timeout,
                               std::chrono::milliseconds poll_interval =
                                   std::chrono::milliseconds(50));
bool terminate_process_group(pid_t launcher_pid,
                             std::chrono::milliseconds grace);
```

Implement the watcher so it:

```bash
unset DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE SDL_VIDEODRIVER
rm -f "$RETROARCH_READY_FILE"
retroarch --config "$CONFIG_FILE" -L "$CORE_PATH" "$ROM_PATH" &
RETROARCH_PID=$!
while kill -0 "$RETROARCH_PID" 2>/dev/null; do
    for fd in /proc/$RETROARCH_PID/fd/*; do
        target=$(readlink "$fd" 2>/dev/null || true)
        case "$target" in
            /dev/dri/card*)
                printf '%s\n' "$RETROARCH_PID" > "$RETROARCH_READY_FILE"
                break 2
                ;;
        esac
    done
    sleep 0.05
done
wait "$RETROARCH_PID"
RETROARCH_EXIT=$?
```

`wait_for_startup` must poll `waitpid(..., WNOHANG)` and marker existence until the configured deadline. On timeout it returns `TimedOut`; on an already-reaped child it returns `Exited`. `terminate_process_group` sends `SIGTERM` to `-launcher_pid`, polls `waitpid`, escalates to `SIGKILL`, and always reaps the launcher child. Handle `EINTR` in every wait loop.

- [ ] **Step 4: Run RED-GREEN verification**

Run:

```bash
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4
ctest --test-dir magic_dingus_box_cpp/build -R RetroArchUnit --output-on-failure
```

Expected: readiness returns without waiting five seconds; timeout completes in under one second; all `RetroArchUnit` cases pass.

- [ ] **Step 5: Commit the supervisor**

```bash
git add magic_dingus_box_cpp/src/retroarch/launch_contract.h \
  magic_dingus_box_cpp/src/retroarch/launch_contract.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp
git commit -m "feat(retroarch): supervise KMS display takeover"
```

---

### Task 3: Integrate supervision and make every launch exit path recover the kiosk

**Files:**
- Create: `magic_dingus_box_cpp/src/app/game_launch_recovery.h`
- Create: `magic_dingus_box_cpp/src/app/game_launch_recovery.cpp`
- Create: `magic_dingus_box_cpp/tests/retroarch/test_game_launch_recovery.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp:1042-1220`
- Modify: `magic_dingus_box_cpp/src/app/controller.cpp:558-739`
- Modify: `magic_dingus_box_cpp/src/main.cpp:2189-2214`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 readiness block and supervisor; existing DRM, input, audio, and display-reset paths.
- Produces: truthful `RetroArchLauncher::launch_game()` success and a shared `prepare_kiosk_state_after_game(AppState&)` recovery normalization.

- [ ] **Step 1: Write the failing state-recovery test**

Create `test_game_launch_recovery.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "app/game_launch_recovery.h"

TEST_CASE("all RetroArch exits request display reset and clear playback state",
          "[retroarch][recovery]") {
    app::AppState state{};
    state.reset_display = false;
    state.current_item_index = 4;
    state.current_playlist_index = 2;
    state.video_active = true;
    state.is_loading_game = true;

    app::prepare_kiosk_state_after_game(state);

    REQUIRE(state.reset_display.load());
    REQUIRE(state.current_item_index == -1);
    REQUIRE(state.current_playlist_index == -1);
    REQUIRE_FALSE(state.video_active.load());
    REQUIRE_FALSE(state.is_loading_game.load());
}
```

Add `game_launch_recovery.cpp` to `APP_SOURCES` and to `test_retroarch_unit`, then build. Expected RED: the recovery module does not exist.

- [ ] **Step 2: Implement shared state normalization and verify GREEN**

Create:

```cpp
// game_launch_recovery.h
#pragma once
#include "app_state.h"
namespace app {
void prepare_kiosk_state_after_game(AppState& state);
}

// game_launch_recovery.cpp
#include "game_launch_recovery.h"
namespace app {
void prepare_kiosk_state_after_game(AppState& state) {
    state.reset_display = true;
    state.current_item_index = -1;
    state.current_playlist_index = -1;
    state.video_active = false;
    state.is_loading_game = false;
}
}
```

Run `test_retroarch_unit` and require GREEN.

- [ ] **Step 3: Integrate the tested launch contract**

In `launch_drm`:

1. Remove `/tmp/retroarch_mdb.ready` synchronously before `fork()`.
2. Replace `export DISPLAY=:0` and the direct foreground RetroArch line with `build_kms_ready_watch_block(retroarch_cmd, opts)`.
3. End the generated script with `exit "$RETROARCH_EXIT"` after cleanup.
4. Put the launcher child in a new process group with `setpgid(0, 0)` and repeat `setpgid(child, child)` in the parent to close the race.
5. Do not mutate the kiosk parent's display environment; unset compositor variables only in the child/generated script.
6. Call `wait_for_startup(child, ready_file, 15s)`. On timeout or early exit, terminate/reap the process group and return false.
7. On readiness, block in the existing `waitpid` game loop, then remove the marker and return true regardless of a later user-driven RetroArch exit status.

In `Controller::load_playlist_item`:

1. Invoke the loading callback once immediately before `release_master(false)`.
2. Replace the post-release `wait_with_callback(200, progress_callback)` with a plain 200 ms sleep so the kiosk does not page-flip after surrendering DRM master.
3. Run existing DRM/input/audio cleanup for both successful and failed launch results.
4. Call `prepare_kiosk_state_after_game(state)` before branching on the result.
5. Return the existing error result when readiness failed.

In `main.cpp`, close `settings_menu` on both success and failure. On failure set the existing error banner to exactly `Unable to start game`.

- [ ] **Step 4: Add Pi regression assertions before deploying**

Extend `tests/pi/retroarch_config_emission.bats`:

```bash
@test "launcher pins Vulkan to KMS without changing stable video tuning" {
    config=$(extract_config)
    for line in \
        'video_driver = "vulkan"' \
        'video_context_driver = "khr_display"' \
        'video_threaded = "false"' \
        'video_max_swapchain_images = "2"' \
        'video_vsync = "true"' \
        'video_shader_enable = "false"' \
        'video_smooth = "false"'; do
        echo "$config" | grep -Fqx "$line" || { echo "missing: $line"; false; }
    done
}
```

Extend `tests/pi/service_logs_clean.bats` with a dedicated test that fails on post-release CRTC attempts:

```bash
@test "latest RetroArch handoff has no page flips after DRM release" {
    run pi_ssh 'sudo journalctl -u magic-dingus-box-cpp.service --since "5 minutes ago" --no-pager'
    [ "$status" -eq 0 ]
    ! echo "$output" | grep -F 'Failed to set CRTC: Permission denied'
}
```

Run both against the current Pi before deployment and record the expected RED failures.

- [ ] **Step 5: Build and run the full local regression suite**

```bash
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit test_phone_remote_unit -j4
ctest --test-dir magic_dingus_box_cpp/build --output-on-failure
./tests/run_local_tests.sh
git diff --check
```

Expected: all local CTest and Bats cases pass with no diff errors.

- [ ] **Step 6: Commit the integrated recovery**

```bash
git add magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp \
  magic_dingus_box_cpp/src/app/game_launch_recovery.h \
  magic_dingus_box_cpp/src/app/game_launch_recovery.cpp \
  magic_dingus_box_cpp/src/app/controller.cpp \
  magic_dingus_box_cpp/src/main.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_game_launch_recovery.cpp \
  magic_dingus_box_cpp/CMakeLists.txt \
  tests/pi/retroarch_config_emission.bats \
  tests/pi/service_logs_clean.bats
git commit -m "fix(retroarch): recover cleanly from launch failures"
```

---

### Task 4: Make all-core smoke testing prove real display takeover

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/emulator_smoke_test.py`
- Create: `magic_dingus_box_cpp/scripts/tests/test_emulator_smoke.py`

**Interfaces:**
- Consumes: `/tmp/retroarch_mdb.ready`, `/proc/<pid>`, `/home/magic/retroarch_launcher.log`, kiosk status JSON, and the existing remote-control endpoint.
- Produces: a launch result that is true only after fresh KMS readiness and sustained RetroArch runtime.

- [ ] **Step 1: Write failing Python tests for marker freshness and fatal logs**

Create `test_emulator_smoke.py` using `unittest` and temporary files:

```python
import os
import tempfile
import time
import unittest
from unittest import mock

import emulator_smoke_test as smoke


class ReadinessTests(unittest.TestCase):
    def test_stale_marker_is_not_ready(self):
        with tempfile.NamedTemporaryFile(mode="w") as f:
            f.write(str(os.getpid()))
            f.flush()
            old = time.time() - 60
            os.utime(f.name, (old, old))
            self.assertFalse(smoke.read_ready_pid(f.name, time.time()))

    @mock.patch.object(smoke, "retroarch_pid_is_live", return_value=True)
    def test_fresh_marker_with_live_pid_is_ready(self, _live):
        with tempfile.NamedTemporaryFile(mode="w") as f:
            f.write("1234\n")
            f.flush()
            self.assertEqual(smoke.read_ready_pid(f.name, time.time() - 1), 1234)

    def test_wayland_and_swapchain_errors_are_fatal(self):
        self.assertIn("Wayland", smoke.launch_log_failure(
            "[ERROR] [Wayland]: Failed to connect to Wayland server."))
        self.assertIn("QueuePresent", smoke.launch_log_failure(
            "[Vulkan]: QueuePresent failed, destroying swapchain"))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests and verify RED**

```bash
cd magic_dingus_box_cpp/scripts
python3 -m unittest discover -s tests -p 'test_emulator_smoke.py' -v
```

Expected: failures for missing `read_ready_pid`, `retroarch_pid_is_live`, and `launch_log_failure`.

- [ ] **Step 3: Implement truthful readiness in the smoke harness**

Add production helpers that:

- Require marker `mtime >= launch_started_at`.
- Parse one positive PID.
- Verify `/proc/<pid>/comm` equals `retroarch` and `os.kill(pid, 0)` succeeds.
- Read RetroArch output from `/home/magic/retroarch_launcher.log` rather than only the preamble log in `/tmp`.
- Return a fatal message for Wayland failure, `QueuePresent failed`, startup timeout, or early RetroArch death.

Change `test_one_game` and `test_restart_path` so `screen == "retroarch"` is only the first phase. They must then wait for a fresh, live KMS marker before setting `result["launched"] = True`. After `PLAY_SECONDS`, require that PID still be live before sending `SIGTERM`.

- [ ] **Step 4: Verify the Python tests and syntax**

```bash
cd magic_dingus_box_cpp/scripts
python3 -m unittest discover -s tests -p 'test_emulator_smoke.py' -v
python3 -m py_compile emulator_smoke_test.py
```

Expected: all readiness tests pass and compilation emits no errors.

- [ ] **Step 5: Commit the smoke-test correction**

```bash
git add magic_dingus_box_cpp/scripts/emulator_smoke_test.py \
  magic_dingus_box_cpp/scripts/tests/test_emulator_smoke.py
git commit -m "test(retroarch): require real KMS takeover in smoke runs"
```

- [ ] **Step 6: Deploy, rebuild, and regenerate a launcher config on the Pi**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Expected: rsync and the Pi build complete successfully; the kiosk service is active after deployment.

- [ ] **Step 7: Verify the forced failure path before the all-core run**

Run the focused supervisor unit test on the Pi (its fake child never opens KMS), then confirm the real kiosk remains active:

```bash
ssh magic@magicpi.local '/opt/magic_dingus_box/magic_dingus_box_cpp/build/test_retroarch_unit "[retroarch][startup]"'
ssh magic@magicpi.local 'systemctl is-active magic-dingus-box-cpp.service'
```

Expected: the timeout case passes in under one second and the kiosk service prints `active`.

- [ ] **Step 8: Launch one real game on all seven cores**

```bash
ssh -t magic@magicpi.local \
  'cd /opt/magic_dingus_box/magic_dingus_box_cpp && python3 scripts/emulator_smoke_test.py --games 1 --no-restart-test'
```

Expected: seven clean passes covering Nestopia, Snes9x 2010, Genesis Plus GX, PCSX-ReARMed, Mednafen PCE Fast, ProSystem, and FBNeo. Each reaches KMS in under 15 seconds, stays alive through `PLAY_SECONDS`, quits, and returns to the menu.

- [ ] **Step 9: Run Pi configuration and journal verification**

```bash
PI_HOST=magic@magicpi.local ./tests/run_pi_tests.sh --filter retroarch_config_emission
PI_HOST=magic@magicpi.local ./tests/run_pi_tests.sh --filter service_logs_clean
ssh magic@magicpi.local 'python3 - <<"PY"
from pathlib import Path
log = Path("/home/magic/retroarch_launcher.log").read_text(errors="replace")
latest = log.rsplit("Launcher: Preparing to launch RetroArch...", 1)[-1]
bad = [s for s in ("Failed to connect to Wayland server", "QueuePresent failed") if s in latest]
raise SystemExit("fatal video signatures: " + ", ".join(bad) if bad else 0)
PY'
```

Expected: config and journal tests pass; the final grep returns no fatal video signatures.

- [ ] **Step 10: Run final local and Pi build verification**

```bash
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit test_phone_remote_unit -j4
ctest --test-dir magic_dingus_box_cpp/build --output-on-failure
./tests/run_local_tests.sh
ssh magic@magicpi.local \
  'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . -j4'
git diff --check
git status --short
```

Expected: all tests and both builds pass; status contains only the intended plan/implementation changes.
