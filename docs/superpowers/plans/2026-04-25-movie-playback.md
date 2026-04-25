# Movie Playback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire up the "Play" button on the Media Browser DetailScreen so a downloaded movie file actually plays through the existing GStreamer pipeline, with the same input vocabulary the main UI uses (PLAY_PAUSE, ±10s with PREV/NEXT, ±5s with C-stick, velocity-curve rotary seek, BTN4 to stop).

**Architecture:** New `PlaybackScreen` MbScreen implementation borrows the existing `Controller` (single GStreamer pipeline). Path translation `/library/...` → `/mnt/ssd/library/...` lives in `RadarrClient::resolve_host_path()` with env-var overrides (`MDB_HOST_LIBRARY_PREFIX`, `MDB_CONTAINER_LIBRARY_PREFIX`). DetailScreen's `do_play()` runs a `std::filesystem::exists` pre-flight, then transitions to `Screen::Playback`. Video frame rendering uses the existing `state.video_active`-gated path — PlaybackScreen never draws video itself, only HUD.

**Tech Stack:** C++17, GStreamer (existing kiosk pipeline), Catch2 (existing tests), spdlog. Renderer primitives (`mb_draw_*`) already shipped from the Media Browser UI work.

**Spec:** `docs/superpowers/specs/2026-04-25-movie-playback-design.md`

---

## File Structure

**New files:**
- `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.h` — class declaration.
- `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp` — lifecycle + input + HUD render.
- `magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp` — new file for `resolve_host_path` unit tests.

**Modified files:**
- `magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h` — add `file_container_path` to `Movie`.
- `magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.cpp` — populate `file_container_path` from `movieFile.path`.
- `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h` — `Config` gets two prefix fields; declare `resolve_host_path`.
- `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp` — constructor normalizes prefixes; implement `resolve_host_path`.
- `magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h` — add `Screen::Playback` enum value.
- `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.h` — add `PlayTarget` struct + `get_play_target()`.
- `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp` — real `do_play()` + `get_play_target()`.
- `magic_dingus_box_cpp/src/main.cpp` — env-var ingestion, construct `PlaybackScreen`, dispatcher case + Detail→Playback handoff.
- `magic_dingus_box_cpp/CMakeLists.txt` — add `playback_screen.cpp` to `KIOSK_MEDIA_BROWSER_SOURCES`; add `test_radarr_client.cpp` to test target.
- `magic_dingus_box_cpp/tests/media_browser/test_radarr_parsers.cpp` — extend movie-list test to assert `file_container_path` is parsed.

---

## Task 1: Parse the absolute container path from `movieFile.path`

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h:23-31`
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.cpp:68-73`
- Modify: `magic_dingus_box_cpp/tests/media_browser/test_radarr_parsers.cpp` (extend existing test or add new)
- Test: same file

The existing parser only reads `relativePath` (just the filename). We need the absolute container path (the `path` field) for playback. Add a new field; don't change the existing one.

- [ ] **Step 1.1: Locate or build a fixture with `movieFile.path`**

Run: `ls magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/`
Expected: a `movie_list.json` (or similar) fixture exists. If a fixture with a `movieFile` block doesn't exist, create `magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/movie_list_with_file.json`:

```json
[
  {
    "id": 1,
    "tmdbId": 45745,
    "imdbId": "tt1727587",
    "title": "Sintel",
    "year": 2010,
    "monitored": true,
    "hasFile": true,
    "added": "2026-04-25T02:10:33Z",
    "movieFile": {
      "path": "/library/Sintel (2010)/Sintel (2010) [720p] [BluRay] [YTS.MX].mp4",
      "relativePath": "Sintel (2010) [720p] [BluRay] [YTS.MX].mp4",
      "size": 142606336,
      "quality": {"quality": {"name": "Bluray-720p"}}
    }
  }
]
```

- [ ] **Step 1.2: Write the failing test**

Add to `magic_dingus_box_cpp/tests/media_browser/test_radarr_parsers.cpp` (after the existing `parse_movie_list` test or wherever movie-list tests live):

```cpp
TEST_CASE("parse_movie_list: extracts movieFile.path as file_container_path",
          "[radarr][parsers]") {
    auto json = read_fixture("movie_list_with_file.json");
    auto movies = media_browser::RadarrParsers::parse_movie_list(json);
    REQUIRE(movies.size() == 1);
    REQUIRE(movies[0].has_file == true);
    REQUIRE(movies[0].file_path ==
            "Sintel (2010) [720p] [BluRay] [YTS.MX].mp4");
    REQUIRE(movies[0].file_container_path ==
            "/library/Sintel (2010)/Sintel (2010) [720p] [BluRay] [YTS.MX].mp4");
}
```

- [ ] **Step 1.3: Run the test, confirm it fails to compile**

Run:
```
cd magic_dingus_box_cpp/build-mac && cmake --build . --target test_media_browser_unit -j4
```
Expected: compile error like `'struct media_browser::Movie' has no member named 'file_container_path'`.

- [ ] **Step 1.4: Add the field to the `Movie` struct**

In `magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h:23-31`, add `file_container_path` after `file_path`:

```cpp
struct Movie : MovieSearchHit {
    int radarr_id = 0;
    bool monitored = false;
    bool has_file = false;
    std::string file_path;            // relative to root folder (e.g. "Sintel.mp4")
    std::string file_container_path;  // absolute path inside Radarr container (e.g. "/library/Sintel (2010)/Sintel.mp4")
    std::string file_quality;         // e.g. "Bluray-1080p"
    int64_t file_size_bytes = 0;
    std::string added_at;             // ISO 8601
};
```

- [ ] **Step 1.5: Populate the field in the parser**

In `magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.cpp:68-73`:

```cpp
        if (r.isMember("movieFile")) {
            const auto& f = r["movieFile"];
            m.file_path = f.get("relativePath", "").asString();
            m.file_container_path = f.get("path", "").asString();
            m.file_quality = f["quality"]["quality"].get("name", "").asString();
            m.file_size_bytes = f.get("size", 0).asInt64();
        }
```

- [ ] **Step 1.6: Re-run the test**

Run:
```
cd magic_dingus_box_cpp/build-mac && cmake --build . --target test_media_browser_unit -j4 && ./test_media_browser_unit "[radarr][parsers]"
```
Expected: all parser tests pass, the new one too.

- [ ] **Step 1.7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h \
        magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_radarr_parsers.cpp \
        magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/movie_list_with_file.json
git commit -m "feat(radarr): parse movieFile.path into Movie.file_container_path"
```

---

## Task 2: `RadarrClient::resolve_host_path` with TDD

**Files:**
- Create: `magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (add new test source to test target)
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h:12-19,52-60`
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp` (top-of-file anonymous namespace + constructor + new method)

- [ ] **Step 2.1: Create the test file with all 5 cases**

Create `magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "media_browser/radarr/radarr_client.h"

namespace mb = media_browser;

TEST_CASE("resolve_host_path: translates exact container prefix to host",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    cfg.container_library_prefix = "/library/";
    cfg.host_library_prefix = "/mnt/ssd/library/";
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("/library/Sintel (2010)/Sintel.mp4") ==
            "/mnt/ssd/library/Sintel (2010)/Sintel.mp4");
}

TEST_CASE("resolve_host_path: rejects /library2 false-prefix",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    cfg.container_library_prefix = "/library/";
    cfg.host_library_prefix = "/mnt/ssd/library/";
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("/library2/foo.mp4") == "/library2/foo.mp4");
}

TEST_CASE("resolve_host_path: passes through unrecognized paths",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    cfg.container_library_prefix = "/library/";
    cfg.host_library_prefix = "/mnt/ssd/library/";
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("/data/foo.mp4") == "/data/foo.mp4");
}

TEST_CASE("resolve_host_path: empty path returns empty",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("").empty());
}

TEST_CASE("resolve_host_path: normalizes prefix without trailing slash",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    cfg.container_library_prefix = "/library";       // no trailing slash
    cfg.host_library_prefix      = "/mnt/ssd/library"; // no trailing slash
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("/library/Sintel.mp4") ==
            "/mnt/ssd/library/Sintel.mp4");
    // Critical: /library2 must still NOT match.
    REQUIRE(c.resolve_host_path("/library2/foo.mp4") == "/library2/foo.mp4");
}
```

- [ ] **Step 2.2: Wire the new test source into CMake**

In `magic_dingus_box_cpp/CMakeLists.txt`, find the `test_media_browser_unit` target's source list (look for `test_radarr_parsers.cpp` and add nearby):

```cmake
# Find the line that adds test_radarr_parsers.cpp:
add_executable(test_media_browser_unit
    tests/media_browser/test_radarr_parsers.cpp
    tests/media_browser/test_radarr_client.cpp   # <-- ADD THIS LINE
    tests/media_browser/test_tmdb_client.cpp
    # ... etc
)
```

Run `grep -n test_radarr_parsers magic_dingus_box_cpp/CMakeLists.txt` first to find the exact line, then add `test_radarr_client.cpp` immediately below it.

- [ ] **Step 2.3: Run tests, confirm compile failure**

Run:
```
cd magic_dingus_box_cpp/build-mac && cmake .. && cmake --build . --target test_media_browser_unit -j4
```
Expected: compile error — `'class media_browser::RadarrClient::Config' has no member named 'container_library_prefix'`.

- [ ] **Step 2.4: Add Config fields and method declaration**

In `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h`, modify the `Config` struct:

```cpp
    struct Config {
        std::string base_url = "http://localhost:7878";
        std::string api_key;
        // HTTP timeout for all Radarr requests. Kept short so a stalled
        // Radarr instance doesn't freeze the kiosk main render thread while
        // get_queue() / is_reachable() are in-flight.
        int timeout_secs = 5;
        // Path translation for movie files: Radarr returns container-internal
        // paths like /library/foo.mp4. The kiosk runs on the host and needs
        // /mnt/ssd/library/foo.mp4. Both prefixes are normalized to end in
        // '/' by the constructor (defense against /library2 false-matches).
        std::string container_library_prefix = "/library/";
        std::string host_library_prefix      = "/mnt/ssd/library/";
    };
```

And in the public method block (after `get_root_folders()`):

```cpp
    // Path translation for the movie file Radarr reports. Returns the host
    // path that GStreamer can open, given a container-internal path.
    // Unrecognized paths pass through unchanged with a warn-level log.
    std::string resolve_host_path(const std::string& container_path) const;
```

- [ ] **Step 2.5: Implement the method**

In `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp`, near the top of the file (after existing includes, inside an anonymous namespace if there is one or alongside any helpers), add:

```cpp
namespace {
std::string ensure_trailing_slash(const std::string& s) {
    if (s.empty()) return s;
    return s.back() == '/' ? s : s + "/";
}
}  // namespace
```

Then update the constructor to normalize the prefixes (find `RadarrClient::RadarrClient` and modify):

```cpp
RadarrClient::RadarrClient(Config config)
    : cfg_(std::move(config)) {
    // Normalize prefixes so prefix matching can't fall for /library2/foo.
    cfg_.container_library_prefix =
        ensure_trailing_slash(cfg_.container_library_prefix);
    cfg_.host_library_prefix =
        ensure_trailing_slash(cfg_.host_library_prefix);
    curl_global_init(CURL_GLOBAL_DEFAULT);   // existing line — keep
}
```

(If the existing constructor doesn't have `curl_global_init`, leave whatever's there — just add the two normalization lines at the top of the body.)

Add the method implementation anywhere in the file:

```cpp
std::string RadarrClient::resolve_host_path(const std::string& container_path) const {
    if (container_path.empty()) return container_path;
    if (container_path.rfind(cfg_.container_library_prefix, 0) == 0) {
        return cfg_.host_library_prefix +
               container_path.substr(cfg_.container_library_prefix.size());
    }
    spdlog::warn("[radarr] resolve_host_path: '{}' does not match prefix "
                 "'{}'; passing through unchanged",
                 container_path, cfg_.container_library_prefix);
    return container_path;
}
```

If `spdlog` isn't already included in `radarr_client.cpp`, add `#include <spdlog/spdlog.h>` near the top.

- [ ] **Step 2.6: Run the tests, confirm all pass**

Run:
```
cd magic_dingus_box_cpp/build-mac && cmake --build . --target test_media_browser_unit -j4 && ./test_media_browser_unit "[radarr][paths]"
```
Expected: 5 test cases pass.

- [ ] **Step 2.7: Run the full test suite to check nothing regressed**

Run:
```
./test_media_browser_unit
```
Expected: all assertions pass (199 + new ones).

- [ ] **Step 2.8: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h \
        magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(radarr): add resolve_host_path with prefix normalization"
```

---

## Task 3: Add `Screen::Playback` enum value

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h:22-30`

This is a 1-line enum addition. We don't wire dispatcher cases yet — that's Task 8. Adding the value first lets later tasks reference `Screen::Playback` symbolically.

- [ ] **Step 3.1: Add the enum value**

In `magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h`, modify the `Screen` enum:

```cpp
enum class Screen {
    Browse,
    Search,
    Detail,
    Queue,
    Library,
    Playback,        // <-- NEW: ad-hoc movie playback inside the Media Browser
    MovieSettings,
    Exit             // Return to kiosk main menu (AppScreen::MainMenu).
};
```

- [ ] **Step 3.2: Build to confirm no breakage**

Run:
```
cd magic_dingus_box_cpp/build-mac && cmake --build . -j4
```
Expected: builds clean (might warn about non-exhaustive switch in main.cpp dispatcher — that's fine, we add the case in Task 8).

- [ ] **Step 3.3: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h
git commit -m "feat(mb): add Screen::Playback enum value"
```

---

## Task 4: PlaybackScreen header + skeleton class

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.h`
- Create: `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp` (stub)
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (add to `KIOSK_MEDIA_BROWSER_SOURCES`)

Build a compiling skeleton first; flesh out behavior in Tasks 5 and 6.

- [ ] **Step 4.1: Create the header**

Create `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.h`:

```cpp
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "media_browser/ui/mb_screen.h"

// Forward declarations to keep this header light.
namespace app {
class Controller;
struct AppState;
}
namespace ui { class Renderer; }

namespace media_browser::ui {

// Plays an ad-hoc movie file through the existing kiosk GStreamer pipeline.
// Constructed once in main.cpp; DetailScreen sets the movie via set_movie()
// before transitioning into Screen::Playback.
//
// Lifecycle:
//   set_movie(host_path, title)      <- caller sets target before transition
//   enter()                          <- load + play, arm title marquee
//   handle_input(events) -> Screen   <- maps inputs to Controller methods,
//                                       returns Screen::Detail on BTN4 or
//                                       on natural end-of-stream.
//   update()                         <- edge-detects end-of-stream,
//                                       decays title marquee.
//   render(r, w, h)                  <- draws HUD only (video frame is
//                                       drawn by the main render loop's
//                                       state.video_active path).
//   leave()                          <- idempotent stop(); surfaces any
//                                       deferred toast.
class PlaybackScreen : public MbScreen {
public:
    PlaybackScreen(app::Controller& controller, app::AppState& state);

    // Caller (main.cpp dispatcher, on Detail->Playback) sets these BEFORE
    // returning Screen::Playback. Last setter wins.
    void set_movie(std::string host_path, std::string title);

    void enter() override;
    void leave() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    app::Controller& controller_;
    app::AppState&   state_;

    std::string movie_title_;
    std::string movie_path_;       // host-side path

    bool was_video_active_ = false;
    bool exit_pending_ = false;
    std::string deferred_toast_;

    std::chrono::steady_clock::time_point title_marquee_until_{};
};

}  // namespace media_browser::ui
```

- [ ] **Step 4.2: Create the .cpp stub (just enough to compile)**

Create `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp`:

```cpp
#include "media_browser/ui/playback_screen.h"

#include "app/app_state.h"
#include "app/controller.h"
#include "platform/input_manager.h"

namespace media_browser::ui {

PlaybackScreen::PlaybackScreen(app::Controller& controller, app::AppState& state)
    : controller_(controller), state_(state) {}

void PlaybackScreen::set_movie(std::string host_path, std::string title) {
    movie_path_ = std::move(host_path);
    movie_title_ = std::move(title);
}

void PlaybackScreen::enter() {
    // Implemented in Task 5.
}

void PlaybackScreen::leave() {
    // Implemented in Task 5.
}

Screen PlaybackScreen::handle_input(
        const std::vector<platform::InputEvent>& /*events*/) {
    // Implemented in Task 5.
    return Screen::Playback;
}

void PlaybackScreen::update() {
    // Implemented in Task 5.
}

void PlaybackScreen::render(::ui::Renderer& /*r*/,
                            int /*screen_w*/, int /*screen_h*/) {
    // Implemented in Task 6.
}

}  // namespace media_browser::ui
```

- [ ] **Step 4.3: Add to CMakeLists.txt**

In `magic_dingus_box_cpp/CMakeLists.txt`, find `KIOSK_MEDIA_BROWSER_SOURCES` (run `grep -n KIOSK_MEDIA_BROWSER_SOURCES magic_dingus_box_cpp/CMakeLists.txt` to locate). Add `playback_screen.cpp` near the other UI screens:

```cmake
set(KIOSK_MEDIA_BROWSER_SOURCES
    # ... existing entries ...
    src/media_browser/ui/detail_screen.cpp
    src/media_browser/ui/playback_screen.cpp     # <-- ADD THIS LINE
    src/media_browser/ui/library_screen.cpp
    # ... etc
)
```

- [ ] **Step 4.4: Re-run CMake configure + build to confirm skeleton compiles on Pi**

Run:
```
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build 2>&1 | tail -10
```
Expected: build succeeds (only the pre-existing spdlog dangling-reference warning).

- [ ] **Step 4.5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/playback_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): add PlaybackScreen skeleton (compiles, no behavior)"
```

---

## Task 5: PlaybackScreen lifecycle (enter/leave/update/handle_input)

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp`

Implement everything except `render()`. After this task, you can technically play a movie if you wired the dispatcher — Task 6 just adds polish.

- [ ] **Step 5.1: Add includes for spdlog and Controller methods**

At the top of `playback_screen.cpp`, ensure these includes are present:

```cpp
#include "media_browser/ui/playback_screen.h"

#include <spdlog/spdlog.h>

#include "app/app_state.h"
#include "app/controller.h"
#include "platform/input_manager.h"
#include "ui/toast.h"
#include "utils/result.h"
```

- [ ] **Step 5.2: Implement `enter()`**

Replace the stub `enter()` body:

```cpp
void PlaybackScreen::enter() {
    exit_pending_ = false;
    deferred_toast_.clear();
    was_video_active_ = false;

    if (movie_path_.empty()) {
        deferred_toast_ = "No movie file path";
        exit_pending_ = true;
        return;
    }

    // Empty playlist_dir: load_file_with_resolution treats this as "use the
    // path as-is" (no relative resolution). The path is already host-absolute
    // since DetailScreen ran it through RadarrClient::resolve_host_path.
    auto load_result = controller_.load_file_with_resolution(
        movie_path_, /*playlist_dir=*/"", /*start=*/0.0, /*end=*/0.0,
        /*loop=*/false);

    // Result<> exposes operator bool — false on failure. .error() carries
    // the message.
    if (!load_result) {
        deferred_toast_ = "Playback failed: " + load_result.error();
        spdlog::error("[playback] load failed for '{}': {}",
                      movie_path_, load_result.error());
        exit_pending_ = true;
        return;
    }

    controller_.play();

    // Initial false — update() needs to see video_active go false→true
    // (playback actually started) before it can detect a true→false
    // edge as end-of-stream. Otherwise we'd false-trigger end-of-stream
    // on the first frame because state.video_active hasn't been updated
    // by player.update_state() yet.
    was_video_active_ = false;

    // Title marquee for 3 seconds on entry.
    title_marquee_until_ = std::chrono::steady_clock::now()
                          + std::chrono::seconds(3);

    spdlog::info("[playback] playing '{}' (path='{}')",
                 movie_title_, movie_path_);
}
```

- [ ] **Step 5.3: Implement `leave()`**

```cpp
void PlaybackScreen::leave() {
    // Idempotent — safe to call from any exit path. Catches the case where
    // the user long-presses BTN4 and the dispatcher hard-exits to MainMenu.
    controller_.stop();
    state_.show_seek_bar = false;

    if (!deferred_toast_.empty()) {
        ::ui::Toast::show(deferred_toast_);
        deferred_toast_.clear();
    }

    spdlog::info("[playback] left playback screen");
}
```

- [ ] **Step 5.4: Implement `update()`**

```cpp
void PlaybackScreen::update() {
    // Edge-detect natural end-of-stream: state.video_active flips true→false
    // when the GStreamer pipeline reaches EOS. We only treat it as
    // end-of-stream if WE didn't trigger the stop (exit_pending stays false
    // until this branch fires).
    bool video_active_now = state_.video_active;
    if (was_video_active_ && !video_active_now && !exit_pending_) {
        exit_pending_ = true;
        spdlog::info("[playback] natural end-of-stream detected");
    }
    was_video_active_ = video_active_now;
}
```

- [ ] **Step 5.5: Implement `handle_input()`**

Replace the stub:

```cpp
Screen PlaybackScreen::handle_input(
        const std::vector<platform::InputEvent>& events) {
    // First: if natural end-of-stream or load failure already armed an exit,
    // honor it. This fires every frame even with empty events because the
    // dispatcher always calls handle_input.
    if (exit_pending_) {
        return Screen::Detail;
    }

    for (const auto& e : events) {
        // BTN4 short-press → stop and return to Detail. The dispatcher's
        // long-press handler (held >500ms) intercepts before we see it,
        // so reaching here means it's a short press.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            controller_.stop();
            return Screen::Detail;
        }

        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            controller_.toggle_pause();
            continue;
        }

        // ±10s with PREV/NEXT — same as main UI when video is playing.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            controller_.seek(10.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }
        if (e.action == platform::InputAction::PREV && e.pressed) {
            controller_.seek(-10.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }

        // ±5s with C-stick.
        if (e.action == platform::InputAction::SEEK_RIGHT) {
            controller_.seek(5.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }
        if (e.action == platform::InputAction::SEEK_LEFT) {
            controller_.seek(-5.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }

        // Velocity-curve rotary seek — exact same formula as main.cpp:1758.
        if (e.action == platform::InputAction::ROTATE && e.delta != 0) {
            double velocity = static_cast<double>(e.velocity);
            double seek_seconds = 5.0 + 25.0 * (velocity * velocity);
            controller_.seek(seek_seconds * e.delta);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }
    }

    return Screen::Playback;
}
```

- [ ] **Step 5.6: Build to confirm**

Run:
```
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build 2>&1 | tail -10
```
Expected: clean build (pre-existing spdlog warning only).

- [ ] **Step 5.7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp
git commit -m "feat(mb): wire PlaybackScreen lifecycle + input controls"
```

---

## Task 6: PlaybackScreen HUD render

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp`

Title marquee + pause indicator + persistent BTN4 hint. The seek bar is reused from the main UI render path — no work here.

- [ ] **Step 6.1: Add the renderer include and implement `render()`**

At the top of `playback_screen.cpp`, ensure this include is present:

```cpp
#include "ui/renderer.h"
#include "ui/theme.h"
```

Replace the stub `render()` with:

```cpp
void PlaybackScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // Title marquee — top-left, 3 seconds with linear fade-out over the
    // last 500 ms. Matches Detail's "FEATURE PRESENTATION" header style.
    auto now = std::chrono::steady_clock::now();
    if (now < title_marquee_until_) {
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            title_marquee_until_ - now).count();
        float alpha = 1.0f;
        if (remaining_ms < 500) {
            alpha = static_cast<float>(remaining_ms) / 500.0f;
        }
        std::string heading = "NOW PLAYING";
        if (!movie_title_.empty()) {
            heading += " — " + movie_title_;
        }
        const float kPaddingX = 32.0f;
        const float kBaselineY = 38.0f;
        r.mb_draw_title_text(heading, kPaddingX, kBaselineY,
                             th.font_heading_size, th.accent2, alpha);
        // Underline beneath, just like Detail's header rule.
        const float rule_y = 58.0f;
        r.mb_draw_line(kPaddingX, rule_y, w - kPaddingX, rule_y,
                       2.0f, th.accent2, 0.95f * alpha);
    }

    // Pause indicator — bottom-center, persistent until unpause.
    if (controller_.is_paused()) {
        std::string label = "PAUSED";
        int sz = th.font_medium_size;
        int baseline = r.mb_text_baseline(sz);
        int tw = r.mb_text_width(label, sz);
        float tx = (w - static_cast<float>(tw)) / 2.0f;
        float ty = h - 60.0f - static_cast<float>(sz)
                 + static_cast<float>(baseline);
        r.mb_draw_text(label, tx, ty, sz, th.dim, 0.85f);
    }

    // Persistent control hint — bottom-right, mirrors Media Browser footer.
    {
        std::string hint = "BTN4: stop   PLAY: pause   ROTATE: seek";
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        int tw = r.mb_text_width(hint, sz);
        float tx = w - 32.0f - static_cast<float>(tw);
        float ty = h - 12.0f - static_cast<float>(sz)
                 + static_cast<float>(baseline);
        r.mb_draw_text(hint, tx, ty, sz, th.dim, 0.7f);
    }
}
```

- [ ] **Step 6.2: Build to confirm**

Run:
```
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build 2>&1 | tail -10
```
Expected: clean build.

- [ ] **Step 6.3: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp
git commit -m "feat(mb): PlaybackScreen HUD — title marquee, pause indicator, hint"
```

---

## Task 7: DetailScreen `do_play()` + `get_play_target()`

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.h` (add `PlayTarget` struct + method)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp` (real `do_play`, new `get_play_target`)

- [ ] **Step 7.1: Add `PlayTarget` and `get_play_target()` to the header**

In `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.h`, in the public block of `DetailScreen` (near `set_tmdb_id`):

```cpp
    // Carrier struct for the Detail->Playback handoff. Populated by
    // get_play_target() and consumed by main.cpp dispatcher to call
    // PlaybackScreen::set_movie before the transition lands.
    struct PlayTarget {
        std::string host_path;  // empty if no playable file
        std::string title;
    };

    // Returns the host-resolved file path + display title for the currently
    // loaded movie, or {empty, empty} if no playable file exists.
    PlayTarget get_play_target() const;
```

- [ ] **Step 7.2: Add `<filesystem>` include in detail_screen.cpp**

At the top of `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp`:

```cpp
#include <filesystem>
```

- [ ] **Step 7.3: Implement `get_play_target()`**

Add anywhere in `detail_screen.cpp` (near other DetailScreen methods):

```cpp
DetailScreen::PlayTarget DetailScreen::get_play_target() const {
    PlayTarget pt;
    if (!movie_.has_value()) return pt;
    if (movie_->file_container_path.empty()) return pt;

    pt.host_path = radarr_.resolve_host_path(movie_->file_container_path);
    // Prefer the rich TMDB title if available; fall back to the Radarr title.
    if (tmdb_detail_.has_value() && !tmdb_detail_->title.empty()) {
        pt.title = tmdb_detail_->title;
    } else {
        pt.title = movie_->title;
    }
    return pt;
}
```

- [ ] **Step 7.4: Replace the `do_play()` stub**

Find the existing `do_play()` in `detail_screen.cpp` (around line 475 — `Screen DetailScreen::do_play() { ... return Screen::Library; }`) and replace with:

```cpp
Screen DetailScreen::do_play() {
    if (!movie_.has_value()) {
        show_banner("No movie record");
        return Screen::Detail;
    }
    auto pt = get_play_target();
    if (pt.host_path.empty()) {
        show_banner("Movie file path unknown");
        return Screen::Detail;
    }
    std::error_code ec;
    if (!std::filesystem::exists(pt.host_path, ec)) {
        show_banner("File missing on disk");
        return Screen::Detail;
    }
    return Screen::Playback;
}
```

- [ ] **Step 7.5: Build to confirm**

Run:
```
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build 2>&1 | tail -10
```
Expected: clean build.

- [ ] **Step 7.6: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp
git commit -m "feat(mb): DetailScreen.do_play wires to PlaybackScreen"
```

---

## Task 8: main.cpp dispatcher wiring + env vars

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp`

Three pieces here: (a) read the env-var overrides into `radarr_cfg` before constructing `RadarrClient`; (b) construct `PlaybackScreen mb_playback`; (c) extend the dispatcher's transition-switch with `Screen::Playback` and add the Detail→Playback handoff.

- [ ] **Step 8.1: Find existing radarr_cfg construction**

Run: `grep -n "radarr_cfg\|RadarrClient::Config" magic_dingus_box_cpp/src/main.cpp | head -10`
Expected: lines around 477-490 where `radarr_cfg` is built.

- [ ] **Step 8.2: Add env-var ingestion above the RadarrClient construction**

In `magic_dingus_box_cpp/src/main.cpp`, above the line that constructs `radarr_cfg` (around line 477), add a small helper in the file's anonymous namespace if one exists, otherwise inline as a static lambda right above the use:

```cpp
        // Defense-in-depth normalization: callers may pass prefixes with
        // or without trailing slashes; we always store with one.
        auto ensure_trailing_slash = [](std::string s) {
            if (!s.empty() && s.back() != '/') s.push_back('/');
            return s;
        };
        if (const char* p = std::getenv("MDB_CONTAINER_LIBRARY_PREFIX")) {
            radarr_cfg.container_library_prefix = ensure_trailing_slash(p);
        }
        if (const char* p = std::getenv("MDB_HOST_LIBRARY_PREFIX")) {
            radarr_cfg.host_library_prefix = ensure_trailing_slash(p);
        }
```

This block goes immediately AFTER `radarr_cfg.api_key = ...` (and any other field assignments) and BEFORE `radarr_owned = std::make_unique<media_browser::RadarrClient>(std::move(radarr_cfg));`.

If `<cstdlib>` isn't already included near the top of main.cpp for `std::getenv`, add it.

- [ ] **Step 8.3: Construct PlaybackScreen alongside the other Mb screens**

In `main.cpp` near line 517-526 where `mb_browse`, `mb_search`, `mb_detail`, etc. are constructed:

```cpp
    media_browser::ui::PlaybackScreen   mb_playback(controller, state);
```

Place this line right after `mb_library` (mirror the order in the Screen enum).

Also add the include at the top of main.cpp (alongside other `media_browser/ui/*.h` includes near lines 16-20):

```cpp
#include "media_browser/ui/playback_screen.h"
```

- [ ] **Step 8.4: Add Detail→Playback handoff in the special-handoff block**

In `main.cpp` around line 1350 (the `if (next == media_browser::ui::Screen::Detail)` block that copies `selected_tmdb_id`), add a parallel branch for the new transition:

```cpp
                // Detail -> Playback: copy resolved host path + title from
                // Detail to Playback so it knows what to load on enter().
                if (next == media_browser::ui::Screen::Playback &&
                    current_mb_screen == media_browser::ui::Screen::Detail) {
                    auto pt = mb_detail.get_play_target();
                    mb_playback.set_movie(pt.host_path, pt.title);
                }
```

Place this right after the existing Detail handoff `if` block, BEFORE `active_mb_screen->leave();`.

- [ ] **Step 8.5: Add the dispatcher case for Screen::Playback**

In `main.cpp` around line 1366 (the `switch (next)` block that picks `active_mb_screen`):

```cpp
                switch (next) {
                    case media_browser::ui::Screen::Browse:        active_mb_screen = &mb_browse;      break;
                    case media_browser::ui::Screen::Search:        active_mb_screen = &mb_search;      break;
                    case media_browser::ui::Screen::Detail:        active_mb_screen = &mb_detail;      break;
                    case media_browser::ui::Screen::Queue:         active_mb_screen = &mb_queue;       break;
                    case media_browser::ui::Screen::Library:       active_mb_screen = &mb_library;     break;
                    case media_browser::ui::Screen::Playback:      active_mb_screen = &mb_playback;    break;  // <-- NEW
                    case media_browser::ui::Screen::MovieSettings: active_mb_screen = &mb_mb_settings; break;
                    case media_browser::ui::Screen::Exit: break;  // handled above
                }
```

- [ ] **Step 8.6: Build to confirm everything links**

Run:
```
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build 2>&1 | tail -10
```
Expected: clean build (only the pre-existing spdlog dangling-reference warning).

- [ ] **Step 8.7: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(mb): wire PlaybackScreen into dispatcher + env-var path overrides"
```

---

## Task 9: Smoke test on Pi

**Files:** none (manual verification)

The unit tests for `resolve_host_path` already passed (Task 2). Now verify the playback path end-to-end on the Pi.

- [ ] **Step 9.1: Restart the kiosk service**

Run:
```
ssh magic@magicpi.local "sudo systemctl restart magic-dingus-box-cpp.service && sleep 3 && sudo systemctl is-active magic-dingus-box-cpp.service"
```
Expected: `active`.

- [ ] **Step 9.2: On the kiosk: unlock Movies, navigate to Library**

Use the secret unlock sequence to enter Movies, navigate to **Library**.
Expected: Sintel poster shows with a green "has good file" dot.

- [ ] **Step 9.3: Open Sintel's DetailScreen and hit Play**

Select Sintel, opens DetailScreen, focus the **Play** button (it should be the leftmost since `hasFile=true`), press SELECT/BTN2.
Expected:
- Black screen briefly, then video starts within ~1 second.
- Title marquee `"NOW PLAYING — Sintel"` shows top-left for 3 seconds and fades out.
- Persistent footer hint at bottom-right: `"BTN4: stop   PLAY: pause   ROTATE: seek"`.

- [ ] **Step 9.4: Verify all controls**

While playback is active:

| Action | Expected |
|---|---|
| Press PLAY (Z) | Video pauses; "PAUSED" appears bottom-center. |
| Press PLAY again | Video resumes; "PAUSED" disappears. |
| Press NEXT (→) | Video jumps forward ~10s; seek bar flashes. |
| Press PREV (←) | Video jumps back ~10s; seek bar flashes. |
| C-stick right | Video jumps forward ~5s; seek bar flashes. |
| C-stick left | Video jumps back ~5s; seek bar flashes. |
| Rotary encoder slow turn | ~5s seek per detent, seek bar shows. |
| Rotary encoder fast turn | Up to ~30s seek per detent, seek bar shows. |
| BTN4 (short press) | Returns to Sintel's DetailScreen. Player stopped. |

- [ ] **Step 9.5: Re-enter playback to confirm idempotent**

From Sintel's DetailScreen, hit Play again.
Expected: video starts cleanly. Title marquee shows again.

- [ ] **Step 9.6: BTN4 long-press during playback**

Hold BTN4 for >500 ms during playback.
Expected: kiosk returns to MainMenu (existing dispatcher long-press behavior). Re-enter Movies → Library → Sintel still shows correctly. No phantom video frame, no crash.

- [ ] **Step 9.7: Test the missing-file path (optional)**

```
ssh magic@magicpi.local "sudo mv '/mnt/ssd/library/Sintel (2010)/Sintel (2010) [720p] [BluRay] [YTS.MX].mp4' '/tmp/sintel.mp4.bak'"
```
On the kiosk: try Play. Expected: banner `"File missing on disk"`. Stays on Detail.
Restore: `ssh magic@magicpi.local "sudo mv /tmp/sintel.mp4.bak '/mnt/ssd/library/Sintel (2010)/Sintel (2010) [720p] [BluRay] [YTS.MX].mp4'"`.

- [ ] **Step 9.8: Service log spot-check**

Run:
```
ssh magic@magicpi.local "sudo journalctl -u magic-dingus-box-cpp.service --since '5 minutes ago' --no-pager 2>/dev/null | grep -iE 'playback|gst' | tail -20"
```
Expected: `[playback] playing 'Sintel'...`, `[playback] left playback screen`, no error or panic lines.

- [ ] **Step 9.9: Final commit if any tweaks were needed during smoke test**

```bash
git add -A && git commit -m "fix: smoke-test polish for movie playback" || echo "no fixes needed"
```
