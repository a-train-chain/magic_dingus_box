# RetroArch Performance Headroom Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Maximize CPU/RAM/IO/thermal headroom during RetroArch gameplay on the Pi 4B 2GB — quiet the media stack for games, reclaim PS1 audio latency, add heavy-scene core options, and cool the boot config — without touching aspect ratio, viewport, bezels, or the launch/return architecture.

**Architecture:** Track 1 adds a small serialized side-effect worker (`GameQuietMode`) wired into main.cpp's game-launch block, reusing the movie path's `playback_services_pause.sh` + qBittorrent `pause_all` machinery; extends `VpnHealthMonitor` with a game-session skip + post-session grace; adds `ArtworkCache::clear_textures()`. Tracks 2–3 are one-line changes to the portable `launch_contract.cpp` config emitters, each gated by Pi-side measurement. Track 4 is two `config.txt` edits on the Pi validated by a thermal soak.

**Tech Stack:** C++17, Catch2 (targets `test_retroarch_unit`, `test_media_browser_unit` — both run on the Mac dev box), CMake/CTest, Bash, ALSA `/proc/asound` sampling, `vcgencmd`, deploy via `./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build`.

## Global Constraints

- Repo root path ends with a space (`magic_dingus_box `): quote all shell paths; the Glob tool fails on it.
- Preserve the exact video contract: `video_driver="vulkan"`, `video_context_driver="khr_display"`, `video_threaded="false"`, `video_max_swapchain_images="2"`, `video_vsync="true"`, `video_frame_delay="4"`; Modern TV 1920x1080 with viewport `(251, 10, 1415, 1059)` + `aspect_ratio_index="22"` + bezel keys; CRT Native 640x480 + `aspect_ratio_index="23"`.
- Preserve PS1: `neon_enhancement_enable="disabled"`, `drc="enabled"`, `psxclock="57"`, `icache_emulation="enabled"`, frameskip disabled, dithering enabled, CD/XA audio on; non-PS1 cores and core downloader stay at 48 ms / downloader stays synchronous `alsa`.
- Preserve all controller mappings, hotkeys, save paths, auto-save/load, audio routing, volume math, the 15 s KMS-takeover supervision, and every recovery path.
- Movie playback's existing pause/resume behavior must not regress.
- Work on branch `feature/retroarch-headroom` off `main` (post-merge `f1643fb`). Each track is its own commit series; a track that fails its Pi gate is reverted, not left half-on.
- Before any deploy/kiosk restart/smoke run on the Pi: check the box is idle — `ssh magic@magicpi.local 'cat /opt/magic_dingus_box/magic_dingus_box_cpp/data/kiosk_status.json'` must show `"screen":"playlist"` and `"retroarch":null`. If someone is playing, wait.
- Local test command (build dir already configured on the Mac): `cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit test_media_browser_unit -j4 && ctest --test-dir magic_dingus_box_cpp/build --output-on-failure`.

---

### Task 1: `GameQuietMode` — serialized pause/resume worker

**Files:**
- Create: `magic_dingus_box_cpp/src/app/game_quiet_mode.h`
- Create: `magic_dingus_box_cpp/src/app/game_quiet_mode.cpp`
- Create: `magic_dingus_box_cpp/tests/retroarch/test_game_quiet_mode.cpp` (picked up by the `tests/retroarch/*.cpp` glob)
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` — add `src/app/game_quiet_mode.cpp` to the `test_retroarch_unit` sources (the block at ~line 458) AND to the kiosk executable's source list (next to `src/app/game_launch_recovery.cpp`, ~line 142 region).

**Interfaces:**
- Produces: `app::GameQuietMode` with `struct Actions { std::function<void()> pause; std::function<void()> resume; }`, `explicit GameQuietMode(Actions)`, `void request_pause()`, `void request_resume()`, `void wait_until_idle()` (test seam), destructor that applies the last pending request then joins. Task 4 consumes exactly these names.

**Why a worker:** `docker stop -t2` of three containers takes 2–8 s and qBit's HTTP timeout is 5 s — the launch path must not block on that (spec: fire-and-forget). A single worker with a desired-state model also makes a fast pause→resume flip safe: if the game exits while pause is still applying, the worker finishes pause then applies resume; if pause never started, both coalesce to nothing.

- [ ] **Step 1: Write the failing test**

```cpp
// magic_dingus_box_cpp/tests/retroarch/test_game_quiet_mode.cpp
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "app/game_quiet_mode.h"

TEST_CASE("GameQuietMode applies pause then resume in order",
          "[quiet_mode]") {
    std::vector<int> order;   // 1 = pause ran, 2 = resume ran
    std::mutex order_mutex;

    app::GameQuietMode quiet({
        [&] { std::lock_guard<std::mutex> l(order_mutex); order.push_back(1); },
        [&] { std::lock_guard<std::mutex> l(order_mutex); order.push_back(2); },
    });

    quiet.request_pause();
    quiet.wait_until_idle();
    quiet.request_resume();
    quiet.wait_until_idle();

    REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE("GameQuietMode never leaves services paused after a fast "
          "pause->resume flip", "[quiet_mode]") {
    std::atomic<int> pauses{0};
    std::atomic<int> resumes{0};

    app::GameQuietMode quiet({
        [&] {
            // Simulate the slow docker-stop path.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            pauses.fetch_add(1);
        },
        [&] { resumes.fetch_add(1); },
    });

    quiet.request_pause();
    // Flip back before the 50ms pause action can possibly finish.
    quiet.request_resume();
    quiet.wait_until_idle();

    // Either both ran (pause was in flight, resume corrected it) or
    // neither ran (coalesced before the worker picked it up). What must
    // NEVER happen is pause-without-resume.
    REQUIRE(pauses.load() == resumes.load());
}

TEST_CASE("GameQuietMode destructor applies the last pending request",
          "[quiet_mode]") {
    std::atomic<int> resumes{0};
    {
        app::GameQuietMode quiet({
            [] {},
            [&] { resumes.fetch_add(1); },
        });
        quiet.request_pause();
        quiet.wait_until_idle();
        quiet.request_resume();
        // No wait_until_idle — destructor must flush it.
    }
    REQUIRE(resumes.load() == 1);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4`
Expected: FAIL — `app/game_quiet_mode.h: No such file or directory`.

- [ ] **Step 3: Implement**

```cpp
// magic_dingus_box_cpp/src/app/game_quiet_mode.h
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace app {

// Serializes the "quiet the background media stack for gameplay" side
// effects on one worker thread. request_pause()/request_resume() record
// the latest desired state and return immediately — the game-launch path
// must never block on docker/qBit round-trips (2-8s). The worker applies
// state changes strictly in order, so a game that exits while the pause
// is still applying always gets a matching resume, and a pause+resume
// requested before the worker wakes coalesce into doing nothing.
class GameQuietMode {
public:
    struct Actions {
        std::function<void()> pause;
        std::function<void()> resume;
    };

    explicit GameQuietMode(Actions actions);
    // Applies any still-pending request, then joins the worker.
    ~GameQuietMode();

    GameQuietMode(const GameQuietMode&) = delete;
    GameQuietMode& operator=(const GameQuietMode&) = delete;

    void request_pause();
    void request_resume();

    // Test seam: blocks until the worker has applied the latest request.
    void wait_until_idle();

private:
    void run();
    void set_desired(bool paused);

    Actions actions_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool desired_paused_ = false;   // guarded by mutex_
    bool applied_paused_ = false;   // guarded by mutex_
    bool stop_ = false;             // guarded by mutex_
    std::thread worker_;
};

}  // namespace app
```

```cpp
// magic_dingus_box_cpp/src/app/game_quiet_mode.cpp
#include "app/game_quiet_mode.h"

namespace app {

GameQuietMode::GameQuietMode(Actions actions)
    : actions_(std::move(actions)), worker_([this] { run(); }) {}

GameQuietMode::~GameQuietMode() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void GameQuietMode::request_pause() { set_desired(true); }
void GameQuietMode::request_resume() { set_desired(false); }

void GameQuietMode::set_desired(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desired_paused_ = paused;
    }
    cv_.notify_all();
}

void GameQuietMode::wait_until_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return desired_paused_ == applied_paused_; });
}

void GameQuietMode::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        cv_.wait(lock, [this] {
            return stop_ || desired_paused_ != applied_paused_;
        });
        if (desired_paused_ == applied_paused_) {
            break;  // stop_ set and nothing pending
        }
        const bool target = desired_paused_;
        lock.unlock();
        if (target) {
            if (actions_.pause) actions_.pause();
        } else {
            if (actions_.resume) actions_.resume();
        }
        lock.lock();
        applied_paused_ = target;
        cv_.notify_all();
    }
}

}  // namespace app
```

CMakeLists.txt: add `src/app/game_quiet_mode.cpp` to the `test_retroarch_unit` `add_executable` list (after `src/app/game_launch_recovery.cpp`) and to the kiosk executable's source list next to where `src/app/game_launch_recovery.cpp` appears.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4 && ctest --test-dir magic_dingus_box_cpp/build -R RetroArchUnit --output-on-failure`
Expected: PASS (all cases, including pre-existing ones).

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/app/game_quiet_mode.h \
        magic_dingus_box_cpp/src/app/game_quiet_mode.cpp \
        magic_dingus_box_cpp/tests/retroarch/test_game_quiet_mode.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(app): serialized quiet-mode worker for game sessions"
```

---

### Task 2: `ArtworkCache::clear_textures()`

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/artwork/artwork_cache.h` (public API, next to `pause()`/`resume()` at ~line 90)
- Modify: `magic_dingus_box_cpp/src/media_browser/artwork/artwork_cache.cpp`
- Modify: `magic_dingus_box_cpp/tests/media_browser/test_artwork_cache.cpp`

**Interfaces:**
- Produces: `void ArtworkCache::clear_textures();` — main-thread only (GL). Task 4 consumes it.
- Consumes: existing `entries_`, `bytes_in_use_`, `ready_uploads_`, `bytes_waiting_upload_` members and the `ARTWORK_CACHE_TEST_MODE` GL-skip convention used by `upload_one`.

- [ ] **Step 1: Write the failing test** (append to `test_artwork_cache.cpp`, following its existing `test_inject_ready_upload`/`pump_for_tests` style — read the top of the file first and reuse its helper for building a `TestPendingUpload`)

```cpp
TEST_CASE("clear_textures drops all entries and queued uploads",
          "[artwork_cache]") {
    media_browser::ArtworkCache cache(/*max_bytes=*/1024 * 1024);

    media_browser::ArtworkCache::TestPendingUpload up;
    up.url = "http://example.test/poster1.jpg";
    up.width = 8;
    up.height = 8;
    up.pixels_rgba.assign(8 * 8 * 4, 0xAB);
    cache.test_inject_ready_upload(up);
    REQUIRE(cache.pump_for_tests() == 1);
    REQUIRE(cache.entries_count() == 1);
    REQUIRE(cache.bytes_in_use() > 0);

    // A second upload left waiting (not pumped) must also be discarded.
    up.url = "http://example.test/poster2.jpg";
    cache.test_inject_ready_upload(up);
    REQUIRE(cache.bytes_waiting_upload() > 0);

    cache.clear_textures();

    REQUIRE(cache.entries_count() == 0);
    REQUIRE(cache.bytes_in_use() == 0);
    REQUIRE(cache.bytes_waiting_upload() == 0);
    REQUIRE(cache.pump_for_tests() == 0);

    // The cache must keep working after a clear.
    up.url = "http://example.test/poster3.jpg";
    cache.test_inject_ready_upload(up);
    REQUIRE(cache.pump_for_tests() == 1);
    REQUIRE(cache.entries_count() == 1);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build magic_dingus_box_cpp/build --target test_media_browser_unit -j4`
Expected: FAIL — `clear_textures` is not a member of `ArtworkCache`.

- [ ] **Step 3: Implement**

Header (after `resume()`):

```cpp
    // Main thread only (owns the GL context). Deletes every uploaded
    // texture and discards queued-but-not-yet-uploaded pixel buffers.
    // Called before the DRM handoff to RetroArch so up to max_bytes_ of
    // poster textures don't sit in RAM/GPU memory for the whole game
    // session. Entries rebuild lazily via get_or_fetch() on return —
    // the disk cache makes that a local re-read, not a TMDB round-trip.
    void clear_textures();
```

Implementation (artwork_cache.cpp — mirror `upload_one`'s `#ifndef ARTWORK_CACHE_TEST_MODE` GL guard and its GL header include):

```cpp
void ArtworkCache::clear_textures() {
    {
        std::lock_guard<std::mutex> ready_lock(ready_mutex_);
        ready_uploads_.clear();
        bytes_waiting_upload_.store(0);
    }
    std::lock_guard<std::mutex> entries_lock(entries_mutex_);
#ifndef ARTWORK_CACHE_TEST_MODE
    for (auto& [url, entry] : entries_) {
        if (entry.texture_id != 0) {
            glDeleteTextures(1, &entry.texture_id);
        }
    }
#endif
    entries_.clear();
    bytes_in_use_ = 0;
}
```

While in the file, verify (read, don't assume) that the fetcher removes a URL from `in_flight_` when its result is pushed to `ready_uploads_` — if removal instead happens at pump time, also erase the discarded uploads' URLs from `in_flight_` inside the first lock block, otherwise those posters can never re-fetch until restart.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build magic_dingus_box_cpp/build --target test_media_browser_unit -j4 && ctest --test-dir magic_dingus_box_cpp/build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/artwork/artwork_cache.h \
        magic_dingus_box_cpp/src/media_browser/artwork/artwork_cache.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_artwork_cache.cpp
git commit -m "feat(mb): ArtworkCache::clear_textures for game-session memory reclaim"
```

---

### Task 3: `VpnHealthMonitor` game-session skip + post-session grace

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.cpp`
- Modify: `magic_dingus_box_cpp/tests/media_browser/test_vpn_health_monitor.cpp`

**Interfaces:**
- Consumes: `state.is_loading_game` (`std::atomic<bool>`, app_state.h:357 — true for the WHOLE game session because the launch call blocks) and `state.video_active` (atomic, movie playback).
- Produces: test constructor gains a 4th parameter `std::chrono::milliseconds post_session_grace` (defaulted so existing call sites compile unchanged): `VpnHealthMonitor(app::AppState&, PingFn, std::chrono::milliseconds poll_interval, std::chrono::milliseconds post_session_grace = std::chrono::seconds(90))`.

**Behavior:** while `video_active || is_loading_game`: no ping, failure counter pinned to 0, grace deadline continuously pushed to `now + grace`. After the session: pings run, but failures inside the grace window don't count (a `docker start`-ing Radarr needs ~10–30 s). Any success clears the grace early and resets counters (existing instant-recovery path).

- [ ] **Step 1: Write the failing tests** (append; match the `ScriptedPinger` style already in the file)

```cpp
TEST_CASE("VpnHealthMonitor skips polling during a game session",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = true;

    ScriptedPinger pinger;
    pinger.next_result = true;

    media_browser::VpnHealthMonitor monitor(
        state, [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5),
        /*post_session_grace=*/std::chrono::milliseconds(0));

    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // arm ever_healthy_

    state.is_loading_game = true;
    pinger.next_result = false;   // Radarr is stopped by quiet mode
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // >> 3 polls
    REQUIRE(state.media_browser_vpn_healthy == true);
    REQUIRE(monitor.consecutive_failures() == 0);

    state.is_loading_game = false;
    monitor.stop();
}

TEST_CASE("VpnHealthMonitor ignores failures during the post-session grace",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = true;

    ScriptedPinger pinger;
    pinger.next_result = true;

    media_browser::VpnHealthMonitor monitor(
        state, [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5),
        /*post_session_grace=*/std::chrono::seconds(10));

    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // arm ever_healthy_

    state.is_loading_game = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    state.is_loading_game = false;

    // Radarr is still restarting: failures land inside the 10s grace.
    pinger.next_result = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(state.media_browser_vpn_healthy == true);
    monitor.stop();
}

TEST_CASE("VpnHealthMonitor resumes real failure detection after the grace",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = true;

    ScriptedPinger pinger;
    pinger.next_result = true;

    media_browser::VpnHealthMonitor monitor(
        state, [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5),
        /*post_session_grace=*/std::chrono::milliseconds(20));

    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // arm ever_healthy_

    state.is_loading_game = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    state.is_loading_game = false;

    pinger.next_result = false;
    // 20ms grace expires, then >3 failing polls at 5ms cadence.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(state.media_browser_vpn_healthy == false);
    monitor.stop();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build magic_dingus_box_cpp/build --target test_media_browser_unit -j4`
Expected: FAIL — constructor takes 3 arguments, not 4.

- [ ] **Step 3: Implement**

Header: add the 4th defaulted param to the test constructor; add members `std::chrono::milliseconds post_session_grace_;` and (worker-thread-only, no atomic needed) `std::chrono::steady_clock::time_point grace_until_{};`. Update the class comment: the skip now covers movie playback AND game sessions, plus a post-session grace.

The default (production) constructor delegates with `std::chrono::seconds(90)`.

`run()` loop becomes:

```cpp
void VpnHealthMonitor::run() {
    while (!stop_flag_.load()) {
        // Skip polling while the kiosk has intentionally quieted the
        // media stack: movie playback (PlaybackScreen pauses Radarr) or
        // a game session (GameQuietMode stops Radarr/Prowlarr/Byparr;
        // is_loading_game stays true for the whole blocked session).
        // Keep pushing the grace deadline so the freshly-restarted
        // containers get time to come up after the session ends.
        const bool session_active =
            state_.video_active.load() || state_.is_loading_game.load();
        if (session_active) {
            consecutive_failures_.store(0);
            grace_until_ = std::chrono::steady_clock::now() + post_session_grace_;
            std::this_thread::sleep_for(poll_interval_);
            continue;
        }

        bool ok = ping_fn_();
        if (ok) {
            consecutive_failures_.store(0);
            state_.media_browser_vpn_healthy = true;
            ever_healthy_.store(true);
            grace_until_ = {};  // healthy again — grace no longer needed
        } else if (std::chrono::steady_clock::now() < grace_until_) {
            // Radarr is still docker-start-ing after a game/movie; a
            // failure here is expected, not a tunnel drop.
            consecutive_failures_.store(0);
        } else {
            int n = consecutive_failures_.fetch_add(1) + 1;
            if (n >= kFailureThreshold && ever_healthy_.load()) {
                state_.media_browser_vpn_healthy = false;
            }
        }
        std::this_thread::sleep_for(poll_interval_);
    }
}
```

(Keep the existing cold-boot-guard comments; fold them into the new structure rather than deleting them.)

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build magic_dingus_box_cpp/build --target test_media_browser_unit -j4 && ctest --test-dir magic_dingus_box_cpp/build --output-on-failure`
Expected: PASS, including the four pre-existing monitor cases.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.h \
        magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_vpn_health_monitor.cpp
git commit -m "feat(mb): VPN monitor skips game sessions + post-session grace"
```

---

### Task 4: Wire quiet mode into the game-launch path + Pi validation of Track 1

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp` — construction near the `qbit_owned` block (~line 644–680) and the game-launch block (`state.is_loading_game = true` at ~line 2116, `= false` at ~line 2204)
- Modify: `magic_dingus_box_cpp/src/ui/renderer.h` (~line 277) — add an `artwork_cache_initialized()` guard so we never lazily construct the cache just to clear it

**Interfaces:**
- Consumes: `app::GameQuietMode` (Task 1), `ArtworkCache::clear_textures()` (Task 2), `qbit_owned` (`std::unique_ptr<media_browser::QbittorrentClient>`), `ui_renderer.artwork_cache()`.
- Reference behavior to mirror: `playback_screen.cpp:79-102` (pause) and `:185-199` (resume).

- [ ] **Step 1: Renderer guard.** In `renderer.h` next to the `artwork_cache()` declaration, add (adjusting the member name to whatever the lazy `unique_ptr` in `renderer.cpp:2418-2437` is actually called — read it first):

```cpp
    // True once the lazy artwork cache exists. Lets the game-launch path
    // clear poster textures without constructing a cache (and its disk
    // recount + fetcher thread) on kiosks that never opened the MB.
    bool artwork_cache_initialized() const { return artwork_cache_ != nullptr; }
```

- [ ] **Step 2: Construct `GameQuietMode` in main().** Immediately after the `qbit_owned` construction block (inside the same `#ifdef MEDIA_BROWSER_ENABLED` region — verify the ifdef spans it; if `qbit_owned` is unconditional, still guard the new code with `#ifdef MEDIA_BROWSER_ENABLED`):

```cpp
    // Track-1 quiet mode: silence the torrent/media stack for the whole
    // game session, mirroring PlaybackScreen's movie behavior. Gated on
    // the provisioning marker so unprovisioned Pis do exactly nothing
    // (no docker errors, no qBit timeouts in the log).
    app::GameQuietMode game_quiet_mode({
        /*pause=*/[qbit = qbit_owned.get()]() {
            if (!std::filesystem::exists(
                    "/opt/magic_dingus_box/services/.env")) {
                return;
            }
            if (qbit != nullptr && !qbit->pause_all()) {
                std::cout << "[quiet-mode] qbit pause_all failed "
                             "(best-effort)" << std::endl;
            }
            (void)std::system(
                "/usr/local/bin/playback_services_pause.sh pause "
                ">/dev/null 2>&1");
        },
        /*resume=*/[qbit = qbit_owned.get()]() {
            if (!std::filesystem::exists(
                    "/opt/magic_dingus_box/services/.env")) {
                return;
            }
            (void)std::system(
                "/usr/local/bin/playback_services_pause.sh unpause "
                ">/dev/null 2>&1");
            if (qbit != nullptr && !qbit->resume_all()) {
                std::cout << "[quiet-mode] qbit resume_all failed; "
                             "resume from web UI if needed" << std::endl;
            }
        }});
```

Add `#include "app/game_quiet_mode.h"` (and `<filesystem>` if not already included) at the top of main.cpp.

- [ ] **Step 3: Hook the launch block.** Right after `state.is_loading_game = true;` (~line 2116):

```cpp
#ifdef MEDIA_BROWSER_ENABLED
                                        // Quiet the media stack for the whole
                                        // session (async — never delays launch)
                                        // and drop poster textures while the GL
                                        // context is still current.
                                        game_quiet_mode.request_pause();
                                        if (ui_renderer.artwork_cache_initialized()) {
                                            ui_renderer.artwork_cache().pause();
                                            ui_renderer.artwork_cache().clear_textures();
                                        }
#endif
```

Right after `state.is_loading_game = false;` (~line 2204):

```cpp
#ifdef MEDIA_BROWSER_ENABLED
                                        if (ui_renderer.artwork_cache_initialized()) {
                                            ui_renderer.artwork_cache().resume();
                                        }
                                        game_quiet_mode.request_resume();
#endif
```

Because `load_playlist_item` is synchronous and returns on every outcome (normal quit, launch failure, KMS timeout), this single resume site covers all exit paths. Kiosk-crash-mid-game recovery stays with the existing startup unpause (main.cpp:406-420). Known accepted gap (pre-existing, same as movies): a crash mid-game leaves qBit torrents paused until the next game/movie cycle resumes them.

- [ ] **Step 4: Local build + full local tests**

Run: `cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit test_media_browser_unit -j4 && ctest --test-dir magic_dingus_box_cpp/build --output-on-failure`
Expected: PASS. (The kiosk binary itself can't link on macOS — Pi build comes next.)

- [ ] **Step 5: Deploy + build on the Pi** (box must be idle — see Global Constraints)

Run: `./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build`
Expected: clean Pi-side build, service restarts, intro plays.

- [ ] **Step 6: Live Track-1 verification**

1. Baseline: `ssh magic@magicpi.local 'docker ps --format "{{.Names}}" | sort; free -h | head -2'` → five `mdb_*` containers running; note free memory.
2. Launch one PS1 game via the smoke harness: `ssh magic@magicpi.local 'cd /opt/magic_dingus_box && MAGIC_DATA_DIR=/opt/magic_dingus_box/magic_dingus_box_cpp/data python3 magic_dingus_box_cpp/scripts/emulator_smoke_test.py --games 1 --no-restart-test'` — or launch manually and let it sit.
3. While the game runs: `ssh magic@magicpi.local 'docker ps --format "{{.Names}}" | sort; free -h | head -2'` → `mdb_radarr`, `mdb_prowlarr`, `mdb_byparr` GONE from the running list (within ~10 s of launch); `mdb_gluetun` + `mdb_qbittorrent` still up; free memory up by roughly 300 MB vs baseline. qBit torrents paused: `ssh magic@magicpi.local 'docker exec mdb_qbittorrent cat /proc/1/status | head -1'` is not sufficient — instead check the web API from the Pi: `curl -s "http://localhost:8080/api/v2/torrents/info" ...` requires auth; acceptable proxy: journal shows no `[quiet-mode] qbit pause_all failed` line.
4. Quit the game (`ssh magic@magicpi.local 'pkill -TERM retroarch'`). Within ~30 s: all five containers running again, no `pause_all/resume_all failed` in `journalctl -u magic-dingus-box-cpp --since "-5 min"`.
5. No tunnel toast: journal contains no VPN-unhealthy flip after the game; `kiosk_status.json` back to `"screen":"playlist"`.
6. Kill-mid-game recovery: launch a game, then `ssh magic@magicpi.local 'sudo systemctl restart magic-dingus-box-cpp.service'`; after the kiosk comes back, confirm all five containers return to running (startup unpause path).

- [ ] **Step 7: Full smoke matrix** (Track 1 regression gate)

Run: `ssh magic@magicpi.local 'cd /opt/magic_dingus_box && MAGIC_DATA_DIR=/opt/magic_dingus_box/magic_dingus_box_cpp/data python3 magic_dingus_box_cpp/scripts/emulator_smoke_test.py'`
Expected: 14/14 PASS including restart path.

- [ ] **Step 8: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/src/ui/renderer.h
git commit -m "feat: quiet the media stack + drop poster textures during game sessions"
```

---

### Task 5: Track 2 — PS1 audio latency 64 → 48 attempt

**Files:**
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp:193` (`audio_latency_ms_for_core`)
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp` (the case asserting PS1 = 64)

- [ ] **Step 1: Flip the test to the new expectation.** Find the existing `audio_latency_ms_for_core` assertions (PS1 cores → 64) and change them to 48; non-PS1 stays 48. Run `cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4 && ctest --test-dir magic_dingus_box_cpp/build -R RetroArchUnit --output-on-failure` → FAIL (production still returns 64).

- [ ] **Step 2: Implement**

```cpp
int audio_latency_ms_for_core(const std::string& core_name) {
    // PS1 was 64 during the alsathread migration; walked back down to 48
    // after a clean zero-retrigger soak (Track 2, 2026-07-16). If PS1
    // crackle ever returns, raise the PS1 branch back to 64 first.
    (void)core_name;
    return 48;
}
```

KEEP the function and its per-core signature even though both branches now agree — Step 5's fallback and future tuning need the seam, and the call sites/tests document the contract. (If the fallback lever ends up shipping, the body stays `is_ps1_core(...) ? 48 : 48`-free anyway since spu_thread lives in `write_core_options`.)

Run the same test command → PASS.

- [ ] **Step 3: Deploy** (box idle first): `./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build`

- [ ] **Step 4: ALSA soak — THPS2, ≥35 s** (same harness as the audio-buffer validation)

1. Launch Tony Hawk's Pro Skater 2: playlist "PlayStation Classics", the smoke harness supports targeting it (playlist 4, game 27 per the prior validation; confirm indices from `kiosk_status.json` game browser fields if the library changed).
2. Confirm buffer size: `ssh magic@magicpi.local 'cat /proc/asound/card1/pcm0p/sub0/hw_params'` → `buffer_size: 2304` (48 ms at 48 kHz).
3. Sample for 30 s: `ssh magic@magicpi.local 'for i in $(seq 300); do cat /proc/asound/card1/pcm0p/sub0/status; echo ---; sleep 0.1; done' > /tmp/alsa_soak_48.txt`
4. Analyze: count distinct `trigger_time` values (must be 1 — zero resets), all samples `state: RUNNING`, minimum `delay` > 0, max `avail_max` ≤ 2304.

- [ ] **Step 5: Decision tree**

- **Clean** → keep 48. Also confirm the generated config on the Pi said `audio_latency = "48"` (`/tmp/retroarch_mdb.cfg` during the run). Proceed to Step 6.
- **Dirty** → A/B the fallback lever: in `write_core_options` change `pcsx_rearmed_spu_thread` to `"disabled"` (update its comment: inline SPU trades a few % of one core for steady delivery), update the core-options test line, redeploy, re-run the soak at 48.
  - Fallback clean → keep 48 + `spu_thread = "disabled"`.
  - Fallback dirty → revert BOTH changes (`git checkout -- magic_dingus_box_cpp/src/retroarch/launch_contract.cpp magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp`), redeploy, re-run soak at 64 to confirm the baseline is still clean, and record the track as closed-no-change.

- [ ] **Step 6: Quit game, run smoke PS1 case once more, commit (only if a 48 variant passed)**

```bash
git add magic_dingus_box_cpp/src/retroarch/launch_contract.cpp \
        magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp
git commit -m "perf(retroarch): PS1 audio latency 64->48 (zero-retrigger validated)"
```

User does the final listening check (audio lag + no crackle) before this track is considered accepted.

---

### Task 6: Track 3 — frame-delay auto + PS1 threaded GPU rendering

**Files:**
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp` (`write_video_config` after the `video_frame_delay` line; `write_core_options`)
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp`

- [ ] **Step 1: Failing tests.** In BOTH the Modern TV and CRT Native video cases add `require_line(config, "video_frame_delay_auto = \"true\"");`. In the PS1 core-options case add `require_line(config, "pcsx_rearmed_gpu_thread_rendering = \"async\"");`. Run RetroArchUnit → FAIL.

- [ ] **Step 2: Implement.** In `write_video_config`, directly under `video_frame_delay = "4"`:

```cpp
    // Auto mode (RetroArch >= 1.9.13): treat 4 as the target and back the
    // effective delay off automatically if a heavy scene makes frames run
    // long — converts a would-be stutter into a 4 ms latency give-back.
    out << "video_frame_delay_auto = \"true\"\n";
```

In `write_core_options`, after the `frameskip_type` line:

```cpp
    // Run the software rasterizer on a second thread (Pi 4 has four
    // cores; the accepted community setting for heavy scenes). "async"
    // is the fast variant; a handful of titles show frame glitches with
    // it — drop to "sync" (or remove) if the visual A/B finds any.
    out << "pcsx_rearmed_gpu_thread_rendering = \"async\"\n";
```

Run RetroArchUnit → PASS.

- [ ] **Step 3: Deploy** (box idle): `./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build`

- [ ] **Step 4: Visual A/B ladder on the Pi.** Test set: THPS2 (3D-heavy) + one 2D PS1 title from the PlayStation Classics playlist. For each: launch, play ≥2 minutes, watch for tearing/flicker/missing geometry; confirm `/tmp/retroarch_core_options.cfg` contains the async line during the run; check `/home/magic/retroarch_launcher.log` for new errors.
  - Glitches with `async` → change emitted value to `"sync"`, update test, redeploy, re-run.
  - Glitches with `sync` too → remove the option + its test line (status quo), redeploy.
  - The user is the final arbiter on "looks right" — flag them before closing the task.

- [ ] **Step 5: Full smoke matrix again** (guards non-PS1 cores against the `video_frame_delay_auto` addition): expect 14/14.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/retroarch/launch_contract.cpp \
        magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp
git commit -m "perf(retroarch): auto frame-delay backoff + PS1 threaded GPU rendering"
```

---

### Task 7: Track 3 — smoke-test dynarec assertion

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/emulator_smoke_test.py` (fatal-signature / per-game health-check area, ~lines 148–230 — it already reads `/home/magic/retroarch_launcher.log` via `launcher_log_since(cursor)` and checks signatures like `("QueuePresent failed", ...)`)
- Modify: `magic_dingus_box_cpp/scripts/tests/test_emulator_smoke.py` (unit-test the new check with canned log text)

- [ ] **Step 1: Discover the exact log line.** With a PS1 game freshly launched: `ssh magic@magicpi.local "grep -i -E 'dynarec|recompiler|lightrec|interpreter|drc' /home/magic/retroarch_launcher.log"`. Record the positive line the ari64 dynarec prints (RetroArch runs with `--verbose`, so core init lines land in this log).

- [ ] **Step 2: Failing unit test.** In `test_emulator_smoke.py`, add a test for a new helper `check_ps1_dynarec(log_text) -> Optional[str]` (returns an error string or None):

```python
def test_ps1_dynarec_check_passes_on_dynarec_log():
    assert smoke.check_ps1_dynarec("blah\n<discovered positive line>\nblah") is None

def test_ps1_dynarec_check_fails_on_lightrec():
    err = smoke.check_ps1_dynarec("Lightrec initialized\n")
    assert err is not None

def test_ps1_dynarec_check_fails_when_no_dynarec_mentioned():
    assert smoke.check_ps1_dynarec("nothing relevant\n") is not None
```

(Substitute the literal discovered line from Step 1 into the first test.) Run: `ssh magic@magicpi.local 'cd /opt/magic_dingus_box && python3 -m pytest magic_dingus_box_cpp/scripts/tests/test_emulator_smoke.py -q'` (pytest is Pi-only per project convention) → FAIL (helper missing).

- [ ] **Step 3: Implement the helper + wire it.**

```python
PS1_DYNAREC_OK = re.compile(r"(?i)dynarec|dynamic recompiler")
PS1_DYNAREC_BAD = re.compile(r"(?i)lightrec|falling back to interpreter")

def check_ps1_dynarec(log_text: str):
    """A future core update that silently drops ari64 for Lightrec or the
    interpreter is a multi-x PS1 slowdown — fail the smoke run loudly."""
    if PS1_DYNAREC_BAD.search(log_text):
        return "PS1 core is not using the ari64 dynarec (lightrec/interpreter found)"
    if not PS1_DYNAREC_OK.search(log_text):
        return "PS1 launch log has no dynarec initialization line"
    return None
```

Wire it into the per-game health check only when the launched core is `pcsx_rearmed` (the harness knows the core via the game-browser status fields / playlist item), using the same `launcher_log_since(cursor)` text the fatal-signature scan uses. Adjust the regexes to the discovered line if needed — the intent (ari64 present, lightrec/interpreter absent) governs.

- [ ] **Step 4: Run pytest on the Pi → PASS; then run the smoke harness PS1 case live → PASS.**

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/scripts/emulator_smoke_test.py \
        magic_dingus_box_cpp/scripts/tests/test_emulator_smoke.py
git commit -m "test(emulators): assert ari64 dynarec active in PS1 smoke runs"
```

---

### Task 8: Track 4 — boot config (thermal + memory) with soak gate

**Files (Pi-side, not repo):** `/boot/firmware/config.txt` on `magicpi.local`
**Files (repo):** `magic_dingus_box_cpp/scripts/golden_image/CLONING.md` (or the golden-image README that travels with the repo — record the settings)

- [ ] **Step 1: Record the before-state.**

```bash
ssh magic@magicpi.local 'vcgencmd measure_temp; vcgencmd get_throttled; grep -E "force_turbo|gpu_mem" /boot/firmware/config.txt'
```

Expected today: ~73 °C idle, `0x80000`, `force_turbo=1`, `gpu_mem=128`. Save the output.

- [ ] **Step 2: Edit config.txt (backup first).**

```bash
ssh magic@magicpi.local 'sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.bak-headroom && sudo sed -i -e "/^force_turbo=1/d" -e "s/^gpu_mem=128/gpu_mem=76/" /boot/firmware/config.txt && grep -E "force_turbo|gpu_mem|arm_freq|over_voltage|core_freq|v3d_freq" /boot/firmware/config.txt'
```

Expected: `force_turbo` line gone; `gpu_mem=76`; `arm_freq=2000`, `over_voltage=6`, `gpu_freq=600`, `v3d_freq=600`, `core_freq=550` all still present.

- [ ] **Step 3: Reboot and verify baseline health.**

```bash
ssh magic@magicpi.local 'sudo reboot'
# wait ~90s
ssh magic@magicpi.local 'vcgencmd get_throttled; vcgencmd measure_temp; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq; vcgencmd get_mem gpu; free -h | head -2'
```

Expected: throttled `0x0` (fresh boot), ARM still pinned at 2000000 by the `performance` governor, `gpu=76M`, idle temp measurably below the 73 °C before-state (give it 10 min to stabilize, then re-read). Confirm the kiosk intro played (journal) and the serial console / rotary encoder still work (rotary: scroll the playlist rail via a quick manual check or the phone remote).

- [ ] **Step 4: 30-minute PS1 soak with temp log.** Launch THPS2 (as in Task 5), then:

```bash
ssh magic@magicpi.local 'for i in $(seq 180); do echo "$(date +%T) $(vcgencmd measure_temp) $(vcgencmd get_throttled) $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)"; sleep 10; done' | tee /tmp/thermal_soak.log
```

Acceptance: every sample < 80 °C; `get_throttled` never shows bits 0–3 or 16–18 (values like `0x0` or at most the pre-existing soft-limit bit 19); ARM frequency stays 2000000; no perceptible new stutter (spot-check the game at start/middle/end — V3D clock ramping is the risk). Then quit the game; confirm clean return.

- **Regression path:** stutter or throttling → `ssh magic@magicpi.local 'sudo cp /boot/firmware/config.txt.bak-headroom /boot/firmware/config.txt && sudo reboot'` and record the track as reverted (gpu_mem=76 may be retried alone — it has no clock interaction).

- [ ] **Step 5: Movie + MB pass.** Play one movie for ≥2 min (hardware H.264 path exercises CMA with the smaller gpu_mem) and one MB browse/search pass. Both must behave normally.

- [ ] **Step 6: Record in the repo + commit.** Add a short "Boot config (2026-07-16)" note to the golden-image doc: `force_turbo removed (idle downclock for thermal headroom; performance governor still pins ARM at 2 GHz), gpu_mem 128→76 (KMS/V3D uses CMA, not firmware memory)` — cloned Pis inherit via the SD image; the `.bak-headroom` backup file is the rollback.

```bash
git add magic_dingus_box_cpp/scripts/golden_image/CLONING.md
git commit -m "docs(golden-image): record force_turbo removal + gpu_mem 76"
```

---

### Task 9: Final regression + measurements + changelog

- [ ] **Step 1: Full seven-core smoke matrix** (idle box): expect 14/14 + restart path PASS.
- [ ] **Step 2: Before/after table** for the PR/changelog: idle temp, in-game temp ceiling, `get_throttled`, free RAM + zram used during a PS1 session (`free -h; cat /proc/swaps` while a game runs — quiet mode should show ~300 MB more available than the Task-4 Step-6 baseline), ALSA soak result at the shipped latency, containers stopped/restored timings.
- [ ] **Step 3: CHANGELOG.md** — add an Unreleased entry (Keep-a-Changelog style) summarizing: game quiet mode, VPN-monitor grace, artwork cache clearing, PS1 audio latency (final value), frame-delay auto, PS1 threaded GPU rendering (final value), dynarec smoke assertion, boot-config change.
- [ ] **Step 4: Commit + hand to finishing-a-development-branch** (merge decision is the user's; they also owe the Track-2 listening check and Track-3 visual verdict).

```bash
git add CHANGELOG.md
git commit -m "docs: changelog for RetroArch performance headroom round"
```
