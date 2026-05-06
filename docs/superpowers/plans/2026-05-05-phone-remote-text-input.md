# Phone Remote Text Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the operator type into the kiosk's MB Search and Wi-Fi password text fields using their phone's native OS keyboard, instead of navigating the kiosk's on-screen keyboard with a D-pad.

**Architecture:** Phone receives a kiosk status flag indicating any `VirtualKeyboard` is active; phone exposes a native `<input>` whose events stream as new WebSocket messages (`type_char`, `key_special`, `clear`); Flask appends them to a JSON-Lines queue file; the kiosk's main loop drains the queue each frame and dispatches to a per-frame-resolved active keyboard pointer. Three small kiosk additions, one new Python module, one new HTML section.

**Tech Stack:** C++17 (Catch2 tests, `std::filesystem`, `flock`), Python 3 (Flask + flask-sock + pytest), HTML/CSS/JS (vanilla, no build step), JSON-Lines for IPC.

**Spec:** `docs/superpowers/specs/2026-05-05-phone-remote-text-input-design.md`

---

## File Structure

| File | Purpose |
|---|---|
| `magic_dingus_box_cpp/src/ui/virtual_keyboard.{h,cpp}` | Adds `type_char(c)`, `clear_buffer()`, `commit()` methods |
| `magic_dingus_box_cpp/src/app/app_state.h` | Adds `active_text_keyboard` pointer + `active_text_title` |
| `magic_dingus_box_cpp/src/media_browser/ui/search_screen.h` | Adds public `is_keyboard_active()`, `keyboard()` accessors |
| `magic_dingus_box_cpp/src/app/status_writer.cpp` | Emits `text_input` block in JSON snapshot |
| `magic_dingus_box_cpp/src/app/controller.{h,cpp}` | Adds `poll_text_input_queue(state)` method |
| `magic_dingus_box_cpp/src/main.cpp` | Per-frame: populate `active_text_keyboard`, call `poll_text_input_queue` |
| `magic_dingus_box_cpp/CMakeLists.txt` | Adds `virtual_keyboard.cpp` + `controller.cpp` to test executable |
| `magic_dingus_box_cpp/tests/phone_remote/test_virtual_keyboard_text.cpp` | New: tests for the 3 new VirtualKeyboard methods |
| `magic_dingus_box_cpp/tests/phone_remote/test_text_input_queue.cpp` | New: tests for `poll_text_input_queue` |
| `magic_dingus_box/web/remote/text_input_writer.py` | New: `TextInputWriter` class — appends events to queue file under flock |
| `magic_dingus_box/web/remote/ws_handler.py` | Adds `type_char` / `key_special` / `clear` message dispatch |
| `magic_dingus_box/web/admin.py` | Constructs `TextInputWriter` and passes it into `handle_connection` |
| `magic_dingus_box/web/tests/test_text_input_writer.py` | New: pytest for `TextInputWriter` (queue format, locking, rate limit) |
| `magic_dingus_box/web/tests/test_ws_protocol.py` | Extend: new tests for the three new WS message types |
| `magic_dingus_box/web/static/remote/remote.html` | Adds `<section id="text-section">` markup |
| `magic_dingus_box/web/static/remote/remote.css` | Adds `.text-mode #text-section` visibility styles |
| `magic_dingus_box/web/static/remote/remote.js` | Adds input listeners, `syncToKiosk` diff, `applyStatus` text_input handling |
| `magic_dingus_box_cpp/scripts/deploy_cpp.sh` | Adds `text_input_queue.jsonl` to runtime-state rsync exclude list |

---

## Task 1: VirtualKeyboard — `type_char`, `clear_buffer`, `commit`

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/virtual_keyboard.h`
- Modify: `magic_dingus_box_cpp/src/ui/virtual_keyboard.cpp`
- Create: `magic_dingus_box_cpp/tests/phone_remote/test_virtual_keyboard_text.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt:454-458` (add `src/ui/virtual_keyboard.cpp` to test executable sources)

- [ ] **Step 1: Write the failing test**

Create `magic_dingus_box_cpp/tests/phone_remote/test_virtual_keyboard_text.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "ui/virtual_keyboard.h"

TEST_CASE("VirtualKeyboard::type_char appends to buffer", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    kb.open("", "Test", nullptr, nullptr);
    REQUIRE(kb.get_text() == "");

    kb.type_char('a');
    kb.type_char('b');
    kb.type_char('c');
    REQUIRE(kb.get_text() == "abc");
}

TEST_CASE("VirtualKeyboard::type_char ignored when not active", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    // Not opened — should be inactive
    REQUIRE_FALSE(kb.is_active());
    kb.type_char('x');
    REQUIRE(kb.get_text() == "");
}

TEST_CASE("VirtualKeyboard::clear_buffer wipes text", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    kb.open("hello", "Test", nullptr, nullptr);
    REQUIRE(kb.get_text() == "hello");

    kb.clear_buffer();
    REQUIRE(kb.get_text() == "");
}

TEST_CASE("VirtualKeyboard::commit fires on_enter when set", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    std::string captured;
    kb.open("password123", "Wi-Fi",
            [&](const std::string& t) { captured = t; },
            nullptr);

    kb.commit();
    REQUIRE(captured == "password123");
}

TEST_CASE("VirtualKeyboard::commit no-op when on_enter unset", "[remote][keyboard]") {
    ui::VirtualKeyboard kb;
    kb.open("hello", "Search", nullptr, nullptr);
    // Should not crash; nothing to capture.
    REQUIRE_NOTHROW(kb.commit());
    REQUIRE(kb.get_text() == "hello");
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . --target test_phone_remote_unit -j4 2>&1 | tail -10'
```
Expected: compile error: `'type_char' is not a member of 'ui::VirtualKeyboard'`

- [ ] **Step 3: Add method declarations to header**

Modify `magic_dingus_box_cpp/src/ui/virtual_keyboard.h` — insert after line 38 (`void space();`):

```cpp
    // Append a literal character to the buffer. Bypasses the on-screen
    // layout's focus-driven select() so external sources (Phone Remote)
    // can inject any character directly. No-op when not active.
    void type_char(char c);

    // Wipe text_buffer_ in one shot. Used by Phone Remote's "×" clear
    // affordance and paste handling. No-op when not active.
    void clear_buffer();

    // Fire on_enter_(text_buffer_) for the current buffer. No-op when
    // not active or when no on_enter callback was registered (e.g. MB
    // Search uses live debounce and ignores submit).
    void commit();
```

- [ ] **Step 4: Implement methods**

Modify `magic_dingus_box_cpp/src/ui/virtual_keyboard.cpp` — append at end of file:

```cpp
void VirtualKeyboard::type_char(char c) {
    if (!active_) return;
    text_buffer_ += c;
}

void VirtualKeyboard::clear_buffer() {
    if (!active_) return;
    text_buffer_.clear();
}

void VirtualKeyboard::commit() {
    if (!active_) return;
    if (on_enter_) on_enter_(text_buffer_);
}
```

- [ ] **Step 5: Wire test into build**

Modify `magic_dingus_box_cpp/CMakeLists.txt` lines 454-458 — add `src/ui/virtual_keyboard.cpp` to the test executable's sources:

```cmake
    add_executable(test_phone_remote_unit
        ${PHONE_REMOTE_TEST_SOURCES}
        src/app/status_writer.cpp
        src/ui/pairing_screen.cpp
        src/ui/virtual_keyboard.cpp
    )
```

- [ ] **Step 6: Run tests to verify they pass**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . --target test_phone_remote_unit -j4 && ./test_phone_remote_unit "[remote][keyboard]"' 2>&1 | tail -15
```
Expected: `All tests passed (X assertions in Y test cases)` — five new tests passing.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/virtual_keyboard.h \
        magic_dingus_box_cpp/src/ui/virtual_keyboard.cpp \
        magic_dingus_box_cpp/tests/phone_remote/test_virtual_keyboard_text.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(keyboard): add type_char, clear_buffer, commit for Phone Remote"
```

---

## Task 2: AppState fields + SearchScreen accessors

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/search_screen.h`

(No tests — these are pure structural additions used by later tasks. Task 3's StatusWriter test exercises the new AppState fields.)

- [ ] **Step 1: Add AppState fields**

In `magic_dingus_box_cpp/src/app/app_state.h`, find the `AppState` struct. Near where other phone-remote fields live (`screen_mode`, `paused`, etc.), add:

```cpp
    // Phone Remote — text input.
    // Pointer to the VirtualKeyboard currently accepting characters
    // from the phone, or nullptr when no text-entry context is active.
    // Updated once per frame at the top of main.cpp's render loop.
    // Lifetime: the pointed-to keyboard outlives the state pointer
    // (both are stack-owned in main.cpp).
    ui::VirtualKeyboard* active_text_keyboard = nullptr;

    // Human-readable label for the current text-input context, e.g.
    // "Search movies" or "Wi-Fi password". Set alongside
    // active_text_keyboard. Empty when no context.
    std::string active_text_title;
```

If `app_state.h` doesn't already include the keyboard header, add a forward declaration before `AppState`:

```cpp
namespace ui { class VirtualKeyboard; }
```

(Use a forward declaration rather than including `virtual_keyboard.h` to avoid pulling render-related headers into every translation unit that touches `AppState`.)

- [ ] **Step 2: Add SearchScreen accessors**

In `magic_dingus_box_cpp/src/media_browser/ui/search_screen.h`, find the public section. The header already has `keyboard_` as a private member (line 133). Add after the existing public accessors:

```cpp
    // Phone Remote integration: expose the search field's keyboard so
    // main.cpp can route phone-typed characters into it via the
    // text-input queue drainer.
    bool is_keyboard_active() const { return keyboard_.is_active(); }
    ::ui::VirtualKeyboard& keyboard() { return keyboard_; }
```

- [ ] **Step 3: Build verifies (no test added)**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . --target magic_dingus_box_cpp -j4 2>&1 | tail -5'
```
Expected: `[100%] Built target magic_dingus_box_cpp` — clean build.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/app/app_state.h \
        magic_dingus_box_cpp/src/media_browser/ui/search_screen.h
git commit -m "feat(state): active_text_keyboard pointer + SearchScreen kbd accessors"
```

---

## Task 3: StatusWriter — `text_input` block

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/status_writer.cpp`
- Modify: `magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp` (extend existing tests)

- [ ] **Step 1: Write the failing test**

Append to `magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp`:

```cpp
TEST_CASE("status_writer emits text_input.active=false when no keyboard", "[remote][status][text_input]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_text_inactive.json";
    fs::remove(tmp);

    app::AppState state;
    state.active_text_keyboard = nullptr;

    app::StatusWriter w(tmp.string());
    w.write_now(state);

    std::ifstream f(tmp);
    Json::Value root;
    f >> root;

    REQUIRE(root.isMember("text_input"));
    REQUIRE(root["text_input"]["active"].asBool() == false);
}

TEST_CASE("status_writer emits text_input block when keyboard active", "[remote][status][text_input]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_text_active.json";
    fs::remove(tmp);

    ui::VirtualKeyboard kb;
    kb.open("shawsh", "Search movies", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    state.active_text_title    = "Search movies";

    app::StatusWriter w(tmp.string());
    w.write_now(state);

    std::ifstream f(tmp);
    Json::Value root;
    f >> root;

    REQUIRE(root["text_input"]["active"].asBool() == true);
    REQUIRE(root["text_input"]["title"].asString() == "Search movies");
    REQUIRE(root["text_input"]["buffer"].asString() == "shawsh");
}
```

Add the include at the top of the file if not already present:

```cpp
#include "ui/virtual_keyboard.h"
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . --target test_phone_remote_unit -j4 && ./test_phone_remote_unit "[text_input]"' 2>&1 | tail -10
```
Expected: assertion failure on `root.isMember("text_input")` — field not yet emitted.

- [ ] **Step 3: Implement in StatusWriter**

In `magic_dingus_box_cpp/src/app/status_writer.cpp`, find `write_now()`. Inside the JSON-build block (where `screen`, `playback`, etc. are populated), add the text_input block. Add the include at top of file if not already present:

```cpp
#include "ui/virtual_keyboard.h"
```

Then in `write_now()` after the existing schema fields:

```cpp
    Json::Value text_input(Json::objectValue);
    if (state.active_text_keyboard != nullptr) {
        text_input["active"] = true;
        text_input["title"]  = state.active_text_title;
        text_input["buffer"] = state.active_text_keyboard->get_text();
    } else {
        text_input["active"] = false;
    }
    root["text_input"] = text_input;
```

(Place this alongside the existing `root["screen"]`, `root["playback"]` assignments — order doesn't matter for JSON.)

- [ ] **Step 4: Run tests to verify they pass**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . --target test_phone_remote_unit -j4 && ./test_phone_remote_unit "[text_input]"' 2>&1 | tail -10
```
Expected: `All tests passed` — both new test cases passing, plus the existing status_writer tests.

- [ ] **Step 5: Run full test suite to catch regressions**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && ./test_phone_remote_unit' 2>&1 | tail -5
```
Expected: all tests pass (existing + new).

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/app/status_writer.cpp \
        magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp
git commit -m "feat(status): emit text_input block from AppState.active_text_keyboard"
```

---

## Task 4: Controller — `poll_text_input_queue`

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/controller.h`
- Modify: `magic_dingus_box_cpp/src/app/controller.cpp`
- Create: `magic_dingus_box_cpp/tests/phone_remote/test_text_input_queue.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt:454-459` — add `src/app/controller.cpp` if not already in the test exe list

- [ ] **Step 1: Write the failing test**

Create `magic_dingus_box_cpp/tests/phone_remote/test_text_input_queue.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "app/app_state.h"
#include "app/controller.h"
#include "ui/virtual_keyboard.h"

namespace fs = std::filesystem;

namespace {
// Helper: write JSON-Lines events into the queue file.
void write_queue(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::trunc);
    f << content;
}
}

TEST_CASE("poll_text_input_queue idle fast path", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_idle.jsonl";
    fs::remove(tmp);

    ui::VirtualKeyboard kb;
    kb.open("hello", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    // No file → no-op, no exception, buffer unchanged.
    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "hello");
}

TEST_CASE("poll_text_input_queue type_char appends", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_typechar.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"s\",\"seq\":1}\n"
        "{\"t\":\"type_char\",\"c\":\"h\",\"seq\":2}\n"
        "{\"t\":\"type_char\",\"c\":\"a\",\"seq\":3}\n");

    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "sha");
    // File should be truncated after drain.
    REQUIRE(fs::file_size(tmp) == 0);
}

TEST_CASE("poll_text_input_queue dispatches backspace and enter", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_special.jsonl";
    write_queue(tmp,
        "{\"t\":\"key_special\",\"k\":\"backspace\",\"seq\":1}\n"
        "{\"t\":\"key_special\",\"k\":\"enter\",\"seq\":2}\n");

    std::string captured;
    ui::VirtualKeyboard kb;
    kb.open("hello", "T",
            [&](const std::string& s){ captured = s; },
            nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "hell");          // backspace ate the 'o'
    REQUIRE(captured == "hell");                // commit fired on_enter
}

TEST_CASE("poll_text_input_queue clear wipes buffer", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_clear.jsonl";
    write_queue(tmp, "{\"t\":\"clear\",\"seq\":1}\n");

    ui::VirtualKeyboard kb;
    kb.open("password", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "");
}

TEST_CASE("poll_text_input_queue drops events when no receiver", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_dropped.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"x\",\"seq\":1}\n");

    app::AppState state;
    state.active_text_keyboard = nullptr;       // no receiver
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    REQUIRE_NOTHROW(c.poll_text_input_queue(state));
    REQUIRE(fs::file_size(tmp) == 0);            // still truncated
}

TEST_CASE("poll_text_input_queue skips malformed lines", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_malformed.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"a\",\"seq\":1}\n"
        "this is not json\n"
        "{\"t\":\"type_char\",\"c\":\"b\",\"seq\":2}\n");

    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    c.poll_text_input_queue(state);
    REQUIRE(kb.get_text() == "ab");
    REQUIRE(fs::file_size(tmp) == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . --target test_phone_remote_unit -j4 2>&1 | tail -10'
```
Expected: compile error: `'set_text_input_queue_path' is not a member of 'app::Controller'`.

- [ ] **Step 3: Add method declarations to Controller header**

In `magic_dingus_box_cpp/src/app/controller.h`, add to the public section of `Controller`:

```cpp
    // Phone Remote: drain the JSON-Lines queue at the configured path
    // and dispatch each event to state.active_text_keyboard. No-op when
    // file is missing/empty (idle fast path) or pointer is null. The
    // file is truncated after parsing under the same flock.
    void poll_text_input_queue(AppState& state);

    // Set the queue file path. Default is data/text_input_queue.jsonl
    // resolved via config::get_data_path(). Tests inject a temp path.
    void set_text_input_queue_path(std::string path);
```

Add a private member to hold the path:

```cpp
private:
    std::string text_input_queue_path_;
```

- [ ] **Step 4: Implement in Controller**

In `magic_dingus_box_cpp/src/app/controller.cpp`, add includes if not already present:

```cpp
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <json/json.h>
#include "ui/virtual_keyboard.h"
```

Implement:

```cpp
void Controller::set_text_input_queue_path(std::string path) {
    text_input_queue_path_ = std::move(path);
}

void Controller::poll_text_input_queue(AppState& state) {
    namespace fs = std::filesystem;

    if (text_input_queue_path_.empty()) return;

    std::error_code ec;
    auto sz = fs::file_size(text_input_queue_path_, ec);
    if (ec || sz == 0) return;  // idle fast path

    // Acquire exclusive lock for the read+truncate window.
    int fd = ::open(text_input_queue_path_.c_str(), O_RDWR);
    if (fd < 0) return;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return;
    }

    // Read entire file into memory under lock.
    std::string contents;
    {
        std::ifstream f(text_input_queue_path_);
        std::stringstream ss;
        ss << f.rdbuf();
        contents = ss.str();
    }

    // Truncate file (we own the lock).
    if (::ftruncate(fd, 0) != 0) {
        // best-effort; events still parsed below.
    }
    ::flock(fd, LOCK_UN);
    ::close(fd);

    // Parse and dispatch each JSON-line.
    auto* kb = state.active_text_keyboard;
    std::stringstream ss(contents);
    std::string line;
    Json::CharReaderBuilder reader_builder;
    auto reader = std::unique_ptr<Json::CharReader>(reader_builder.newCharReader());

    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        Json::Value ev;
        std::string err;
        if (!reader->parse(line.data(), line.data() + line.size(), &ev, &err)) {
            continue;  // malformed line — skip
        }
        if (kb == nullptr) continue;  // no receiver, drop silently

        const std::string t = ev.get("t", "").asString();
        if (t == "type_char") {
            const std::string c = ev.get("c", "").asString();
            if (c.size() == 1) kb->type_char(c[0]);
        } else if (t == "key_special") {
            const std::string k = ev.get("k", "").asString();
            if (k == "backspace") kb->backspace();
            else if (k == "enter") kb->commit();
            else if (k == "cancel") kb->close();
        } else if (t == "clear") {
            kb->clear_buffer();
        }
    }
}
```

- [ ] **Step 5: Wire test into build (add `controller.cpp` to test exe sources)**

Modify `magic_dingus_box_cpp/CMakeLists.txt` lines 454-458 — add `src/app/controller.cpp`:

```cmake
    add_executable(test_phone_remote_unit
        ${PHONE_REMOTE_TEST_SOURCES}
        src/app/status_writer.cpp
        src/ui/pairing_screen.cpp
        src/ui/virtual_keyboard.cpp
        src/app/controller.cpp
    )
```

Note: `controller.cpp` likely depends on more sources (e.g., `gst_player.cpp`). If linking fails with undefined symbols, add the minimum additional sources (or extract `poll_text_input_queue` + `set_text_input_queue_path` into a small standalone implementation file like `controller_text_input.cpp` to keep the test exe link surface minimal). Decide based on the actual link errors in step 6.

- [ ] **Step 6: Run tests to verify they pass**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . --target test_phone_remote_unit -j4 && ./test_phone_remote_unit "[text_queue]"' 2>&1 | tail -15
```
Expected: all six new test cases pass. If linker errors mention `controller.cpp` symbols, follow the note in Step 5: split `poll_text_input_queue` into its own translation unit.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/app/controller.h \
        magic_dingus_box_cpp/src/app/controller.cpp \
        magic_dingus_box_cpp/tests/phone_remote/test_text_input_queue.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(controller): poll_text_input_queue drains JSONL queue → keyboard"
```

---

## Task 5: Main loop wiring

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp`

(No tests — main loop is integration-tested manually on the Pi in Task 12.)

- [ ] **Step 1: Set the queue file path at startup**

In `magic_dingus_box_cpp/src/main.cpp`, find where `controller` is constructed (search for `app::Controller controller`). Immediately after construction, set the queue path:

```cpp
    controller.set_text_input_queue_path(
        config::get_data_path() + "/text_input_queue.jsonl");
```

- [ ] **Step 2: Populate `active_text_keyboard` per frame**

In `magic_dingus_box_cpp/src/main.cpp`, find the main render loop (`while (running) {` around line 1184). Near the top of the loop body, alongside the existing per-frame state setup (and before the input poll), add:

```cpp
        // Phone Remote — refresh the active text-input pointer each frame.
        // Whichever VirtualKeyboard is currently is_active() becomes the
        // destination for phone-typed characters via poll_text_input_queue
        // below. This is the only place the pointer is set/cleared, so
        // the spec's "single source of truth" invariant holds.
        state.active_text_keyboard = nullptr;
        state.active_text_title    = "";
#ifdef MEDIA_BROWSER_ENABLED
        if (state.current_screen == app::AppScreen::MediaBrowser
            && current_mb_screen == media_browser::ui::Screen::Search
            && mb_search.is_keyboard_active()) {
            state.active_text_keyboard = &mb_search.keyboard();
            state.active_text_title    = "Search movies";
        } else
#endif
        if (keyboard.is_active()) {
            // The kiosk's main VirtualKeyboard (Wi-Fi password etc.)
            // is named `keyboard` in main.cpp.
            state.active_text_keyboard = &keyboard;
            state.active_text_title    = keyboard.get_title();
        }
```

(If the existing variable for the kiosk's main keyboard isn't `keyboard`, use whatever name `main.cpp` uses — same identifier already passed to `keyboard.is_active()` elsewhere in the file. Search for `keyboard.is_active()` to confirm.)

- [ ] **Step 3: Drain the queue per frame**

Find where `controller.poll_seek_request()` is called in the main loop. Add the new drain call immediately after it:

```cpp
        controller.poll_seek_request();
        controller.poll_text_input_queue(state);
```

- [ ] **Step 4: Build and verify the kiosk binary compiles**

```bash
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake --build . --target magic_dingus_box_cpp -j4 2>&1 | tail -5'
```
Expected: `[100%] Built target magic_dingus_box_cpp` — clean build.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(main): wire active_text_keyboard + poll_text_input_queue per frame"
```

---

## Task 6: Flask `TextInputWriter`

**Files:**
- Create: `magic_dingus_box/web/remote/text_input_writer.py`
- Create: `magic_dingus_box/web/tests/test_text_input_writer.py`

- [ ] **Step 1: Write the failing tests**

Create `magic_dingus_box/web/tests/test_text_input_writer.py`:

```python
"""Unit tests for TextInputWriter — appends JSONL events to a queue file
under flock, with a per-process monotonic seq counter and a per-connection
rate limit."""
from __future__ import annotations

import json
import threading
import time
from pathlib import Path

import pytest

from remote.text_input_writer import TextInputWriter


@pytest.fixture
def writer(tmp_path: Path) -> TextInputWriter:
    return TextInputWriter(queue_path=tmp_path / "queue.jsonl")


def _read_lines(path: Path) -> list[dict]:
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        if line.strip():
            out.append(json.loads(line))
    return out


def test_type_char_appends_one_event(writer, tmp_path):
    writer.type_char("a", device_id="dev1")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert len(events) == 1
    assert events[0]["t"] == "type_char"
    assert events[0]["c"] == "a"
    assert events[0]["device"] == "dev1"
    assert events[0]["seq"] >= 1
    assert events[0]["ts"] > 0


def test_seq_is_monotonic(writer, tmp_path):
    writer.type_char("a", device_id="d")
    writer.type_char("b", device_id="d")
    writer.key_special("backspace", device_id="d")
    events = _read_lines(tmp_path / "queue.jsonl")
    seqs = [e["seq"] for e in events]
    assert seqs == sorted(seqs)
    assert len(set(seqs)) == 3  # all distinct


def test_key_special_writes_correct_shape(writer, tmp_path):
    writer.key_special("enter", device_id="d")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert events[0]["t"] == "key_special"
    assert events[0]["k"] == "enter"


def test_clear_writes_correct_shape(writer, tmp_path):
    writer.clear(device_id="d")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert events[0]["t"] == "clear"


def test_concurrent_writes_no_torn_lines(tmp_path):
    """Two threads writing 50 events each should produce 100 well-formed
    JSON lines with no interleaved bytes (flock works)."""
    path = tmp_path / "queue.jsonl"
    w = TextInputWriter(queue_path=path)

    def hammer(tag: str):
        for i in range(50):
            w.type_char(chr(97 + (i % 26)), device_id=tag)

    t1 = threading.Thread(target=hammer, args=("A",))
    t2 = threading.Thread(target=hammer, args=("B",))
    t1.start(); t2.start()
    t1.join(); t2.join()

    events = _read_lines(path)
    assert len(events) == 100
    # Every line parsed cleanly (no torn writes).


def test_rate_limit_drops_excess(writer, tmp_path):
    """50 events/sec/device. 60 fired in <1s — last 10 should drop silently."""
    for _ in range(60):
        writer.type_char("x", device_id="dev1")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert len(events) == 50  # capped


def test_rate_limit_per_device(writer, tmp_path):
    """Per-device buckets — dev1 hitting limit doesn't affect dev2."""
    for _ in range(60):
        writer.type_char("a", device_id="dev1")
    for _ in range(10):
        writer.type_char("b", device_id="dev2")
    events = _read_lines(tmp_path / "queue.jsonl")
    dev1 = [e for e in events if e["device"] == "dev1"]
    dev2 = [e for e in events if e["device"] == "dev2"]
    assert len(dev1) == 50
    assert len(dev2) == 10


def test_rate_limit_window_resets(tmp_path):
    """After the 1s window, full quota available again."""
    w = TextInputWriter(queue_path=tmp_path / "queue.jsonl")
    for _ in range(50):
        w.type_char("x", device_id="d")
    time.sleep(1.05)
    w.type_char("y", device_id="d")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert len(events) == 51
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_text_input_writer.py -v 2>&1 | tail -10
```
Expected: `ModuleNotFoundError: No module named 'remote.text_input_writer'`.

- [ ] **Step 3: Implement TextInputWriter**

Create `magic_dingus_box/web/remote/text_input_writer.py`:

```python
"""Appends Phone Remote text-input events to a JSON-Lines queue file
that the kiosk's main loop drains each frame.

Mirrors the role of UinputWriter (button presses → evdev events) for
text input: each phone keystroke arrives as a WS message and is
serialized into one JSONL line under flock. Concurrent connections
from multiple phones are safe — flock(LOCK_EX) serializes the writes.

Per-device rate limit (50 events/sec) prevents a malicious or
misbehaving paired phone from filling the queue file."""
from __future__ import annotations

import fcntl
import json
import threading
import time
from pathlib import Path
from typing import Dict


# Per-device rate limit. Sustained human typing tops out at ~5-10
# char/sec; 50 is a comfortable ceiling that catches paste flurries
# without leaving room for abuse.
_MAX_EVENTS_PER_SECOND = 50
_RATE_WINDOW_S = 1.0


class _Bucket:
    """Token bucket per device. Reset on the next event after the window."""
    __slots__ = ("count", "window_start")

    def __init__(self) -> None:
        self.count = 0
        self.window_start = 0.0


class TextInputWriter:
    def __init__(self, queue_path: Path) -> None:
        self._path = Path(queue_path)
        # Ensure parent dir exists; touch the file so flock has a target.
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._path.touch(exist_ok=True)
        self._seq = 0
        self._seq_lock = threading.Lock()
        self._buckets: Dict[str, _Bucket] = {}
        self._buckets_lock = threading.Lock()

    # ── public API ────────────────────────────────────────────────────

    def type_char(self, c: str, *, device_id: str) -> None:
        """Append a `type_char` event. `c` must be a single ASCII
        printable character; non-conforming inputs are dropped silently
        (the WS handler is the primary filter; this is defense in depth).
        """
        if len(c) != 1 or not c.isprintable() or ord(c) >= 0x80:
            return
        self._append({"t": "type_char", "c": c}, device_id)

    def key_special(self, k: str, *, device_id: str) -> None:
        """Append a `key_special` event. `k` must be one of
        ('backspace', 'enter', 'cancel'); others dropped."""
        if k not in ("backspace", "enter", "cancel"):
            return
        self._append({"t": "key_special", "k": k}, device_id)

    def clear(self, *, device_id: str) -> None:
        """Append a `clear` event."""
        self._append({"t": "clear"}, device_id)

    # ── internals ─────────────────────────────────────────────────────

    def _next_seq(self) -> int:
        with self._seq_lock:
            self._seq += 1
            return self._seq

    def _check_rate(self, device_id: str) -> bool:
        """Return True if event allowed; False if it should be dropped."""
        now = time.time()
        with self._buckets_lock:
            b = self._buckets.setdefault(device_id, _Bucket())
            if now - b.window_start >= _RATE_WINDOW_S:
                b.window_start = now
                b.count = 0
            if b.count >= _MAX_EVENTS_PER_SECOND:
                return False
            b.count += 1
            return True

    def _append(self, event: dict, device_id: str) -> None:
        if not self._check_rate(device_id):
            return
        event["seq"]    = self._next_seq()
        event["ts"]     = time.time()
        event["device"] = device_id
        line = json.dumps(event) + "\n"
        with open(self._path, "ab") as f:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX)
            try:
                f.write(line.encode("utf-8"))
            finally:
                fcntl.flock(f.fileno(), fcntl.LOCK_UN)
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_text_input_writer.py -v 2>&1 | tail -15
```
Expected: all 8 tests pass.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/remote/text_input_writer.py \
        magic_dingus_box/web/tests/test_text_input_writer.py
git commit -m "feat(web): TextInputWriter — JSONL queue writer with per-device rate limit"
```

---

## Task 7: Flask WS handler dispatch

**Files:**
- Modify: `magic_dingus_box/web/remote/ws_handler.py`
- Modify: `magic_dingus_box/web/admin.py` (construct & inject `TextInputWriter`)
- Modify: `magic_dingus_box/web/tests/test_ws_protocol.py` (extend)

- [ ] **Step 1: Write the failing tests**

Append to `magic_dingus_box/web/tests/test_ws_protocol.py`:

```python
def test_ws_type_char_routes_to_text_input_writer(ws_test_app):
    """type_char message → TextInputWriter.type_char(c, device_id)"""
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "type_char", "c": "s"})
    assert fake_text.calls == [("type_char", "s", "dev1")]


def test_ws_key_special_routes(ws_test_app):
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "key_special", "k": "backspace"})
    ws_test_app.send_recv({"t": "key_special", "k": "enter"})
    ws_test_app.send_recv({"t": "key_special", "k": "cancel"})
    assert fake_text.calls == [
        ("key_special", "backspace", "dev1"),
        ("key_special", "enter", "dev1"),
        ("key_special", "cancel", "dev1"),
    ]


def test_ws_clear_routes(ws_test_app):
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "clear"})
    assert fake_text.calls == [("clear", None, "dev1")]


def test_ws_type_char_filters_non_ascii(ws_test_app):
    """Emoji / multi-byte chars dropped at WS edge — never reach writer."""
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "type_char", "c": "🍕"})
    ws_test_app.send_recv({"t": "type_char", "c": ""})
    ws_test_app.send_recv({"t": "type_char", "c": "ab"})
    assert fake_text.calls == []  # all three dropped


def test_ws_key_special_filters_unknown_key(ws_test_app):
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "key_special", "k": "nonsense"})
    assert fake_text.calls == []
```

You'll need to extend the existing `ws_test_app` fixture so it carries a `text_input_writer` attribute. Locate the fixture (top of `test_ws_protocol.py` or in `conftest.py`); add a `FakeTextInputWriter` analogous to the existing `FakeUinputWriter`:

```python
class FakeTextInputWriter:
    """Captures TextInputWriter calls for verification.
    Each entry: (method, payload_or_None, device_id)."""
    def __init__(self):
        self.calls = []

    def type_char(self, c, *, device_id):
        self.calls.append(("type_char", c, device_id))

    def key_special(self, k, *, device_id):
        self.calls.append(("key_special", k, device_id))

    def clear(self, *, device_id):
        self.calls.append(("clear", None, device_id))
```

In the fixture's app construction, pass this instance into `handle_connection` alongside the existing `uinput_writer`. (If the fixture currently builds a real Flask app with the test client, also attach `text_input_writer` as an attribute on the returned object so tests can reach it via `ws_test_app.text_input_writer`.)

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_ws_protocol.py -v -k "type_char or key_special or clear" 2>&1 | tail -15
```
Expected: tests fail with `AttributeError: 'ws_test_app' has no attribute 'text_input_writer'` or assertion failures (no calls recorded).

- [ ] **Step 3: Extend `handle_connection` to accept text_input_writer**

In `magic_dingus_box/web/remote/ws_handler.py`, change the `handle_connection` signature:

```python
def handle_connection(ws, *, uinput_writer, text_input_writer, data_dir: Path,
                      verify_cookie: Callable[[str], Optional[str]]):
```

Then in the message-dispatch chain (after the existing `if t == "press":` / `elif t == "seek":` blocks), add:

```python
            elif t == "type_char":
                c = msg.get("c", "")
                if isinstance(c, str) and len(c) == 1 and c.isprintable() and ord(c) < 0x80:
                    text_input_writer.type_char(c, device_id=device_id)

            elif t == "key_special":
                k = msg.get("k", "")
                if isinstance(k, str) and k in ("backspace", "enter", "cancel"):
                    text_input_writer.key_special(k, device_id=device_id)

            elif t == "clear":
                text_input_writer.clear(device_id=device_id)
```

- [ ] **Step 4: Construct and pass TextInputWriter from admin.py**

In `magic_dingus_box/web/admin.py`, find where `UinputWriter` is constructed (search for `UinputWriter(`). Construct `TextInputWriter` alongside it:

```python
from remote.text_input_writer import TextInputWriter
# ...
text_input_writer = TextInputWriter(queue_path=DATA_DIR / "text_input_queue.jsonl")
```

(Use the same `DATA_DIR` the kiosk reads from — the kiosk side resolves it via `config::get_data_path()` which equals the admin's `DATA_DIR` on a deployed system.)

In the `@sock.route("/admin/remote/ws")` handler, pass it through to `handle_connection`:

```python
        ws_handler.handle_connection(
            ws,
            uinput_writer=uinput_writer,
            text_input_writer=text_input_writer,
            data_dir=DATA_DIR,
            verify_cookie=verify_cookie,
        )
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_ws_protocol.py -v 2>&1 | tail -15
```
Expected: all WS-protocol tests pass (existing + 5 new).

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box/web/remote/ws_handler.py \
        magic_dingus_box/web/admin.py \
        magic_dingus_box/web/tests/test_ws_protocol.py
git commit -m "feat(web): WS handler routes type_char/key_special/clear to TextInputWriter"
```

---

## Task 8: Phone HTML + CSS (text section)

**Files:**
- Modify: `magic_dingus_box/web/static/remote/remote.html`
- Modify: `magic_dingus_box/web/static/remote/remote.css`

(No automated tests — visual smoke test on the phone in Task 12.)

- [ ] **Step 1: Add HTML markup**

In `magic_dingus_box/web/static/remote/remote.html`, find the existing `<main>` or top-level container that holds the D-pad. Insert the text section as a sibling **before** the D-pad section so it renders at the top:

```html
<section id="text-section" hidden>
  <header class="text-section__header">
    <span id="text-title" class="text-section__title"></span>
    <button id="text-clear" class="text-section__clear" type="button" aria-label="Clear">×</button>
  </header>
  <input
    id="text-input"
    class="text-section__input"
    type="text"
    autocomplete="off"
    autocapitalize="none"
    spellcheck="false"
    enterkeyhint="search"
    aria-label="Text input"
  >
</section>
```

The `hidden` attribute keeps it out of the layout when the kiosk isn't in a text context. The `enterkeyhint="search"` makes iOS / Android render the OS keyboard's return key as "Search" rather than "Return" — a small UX touch. The autocomplete / autocapitalize / spellcheck disables interfere with kiosk-side buffer matching.

- [ ] **Step 2: Add CSS rules**

Append to `magic_dingus_box/web/static/remote/remote.css`:

```css
/* ── Phone Remote — text input section ──────────────────────────── */
/* Mirrors the existing D-pad section's spacing/typography so the
   transition between text-mode and D-pad-mode feels native. */

#text-section {
  /* Hidden by default; revealed when body.text-mode is set. */
  display: none;
  margin: 0 1rem 1rem 1rem;
  padding: 0.75rem 0.875rem;
  border-radius: 12px;
  background: rgba(255, 255, 255, 0.06);
  transition: opacity 150ms ease-out;
}

body.text-mode #text-section {
  display: block;
}

.text-section__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 0.5rem;
}

.text-section__title {
  font-size: 0.875rem;
  font-weight: 600;
  color: rgba(255, 255, 255, 0.85);
  letter-spacing: 0.02em;
}

.text-section__clear {
  appearance: none;
  border: none;
  background: rgba(255, 255, 255, 0.1);
  color: rgba(255, 255, 255, 0.85);
  font-size: 1.125rem;
  font-weight: 600;
  width: 28px;
  height: 28px;
  border-radius: 50%;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 0;
}

.text-section__clear:active {
  background: rgba(255, 255, 255, 0.2);
}

.text-section__input {
  width: 100%;
  box-sizing: border-box;
  padding: 0.625rem 0.75rem;
  font-size: 1rem;
  font-family: inherit;
  border: 2px solid rgba(255, 255, 255, 0.15);
  border-radius: 8px;
  background: rgba(0, 0, 0, 0.3);
  color: white;
  outline: none;
  transition: border-color 120ms ease-out;
}

.text-section__input:focus {
  border-color: rgba(255, 255, 255, 0.5);
}

.text-section__input:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
```

- [ ] **Step 3: Verify the page renders without errors**

Reload the phone remote page in a desktop browser (`http://magicpi.local:5000/admin/remote/`) and confirm:
- No HTML parse errors (open dev-tools console).
- The `#text-section` element exists in the DOM but is `display: none`.
- Adding `class="text-mode"` to `<body>` via dev-tools makes the section appear, the title is empty, the input field renders, the clear "×" button renders.

```bash
ssh magic@magicpi.local 'curl -s http://localhost:5000/admin/remote/ | grep -E "text-section|text-input" | head -5'
```
Expected: lines from the new HTML markup appear in the served page.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box/web/static/remote/remote.html \
        magic_dingus_box/web/static/remote/remote.css
git commit -m "feat(remote): text-input HTML + CSS (hidden until kiosk activates)"
```

---

## Task 9: Phone JS — input listeners + diff

**Files:**
- Modify: `magic_dingus_box/web/static/remote/remote.js`

(No JS unit-test infra exists in this codebase; this code is exercised by the manual integration test in Task 12. The `syncToKiosk` diff is small and self-contained — `console.log` instrumentation + manual exercises in Step 3 verify behavior.)

- [ ] **Step 1: Find the existing JS structure**

Open `magic_dingus_box/web/static/remote/remote.js`. Identify:
- Where `send(...)` is defined (already wraps `ws.send(JSON.stringify(...))`).
- Where the existing status-message handler lives (the function that processes `{t: "status", data: ...}` messages).

- [ ] **Step 2: Add element references and state**

Near the top of `remote.js` (alongside existing element queries), add:

```javascript
// ── Phone Remote — text input ──────────────────────────────────────
const textSection = document.getElementById('text-section');
const textInput   = document.getElementById('text-input');
const textTitle   = document.getElementById('text-title');
const clearBtn    = document.getElementById('text-clear');

// The last value we sent to the kiosk. Used to compute per-keystroke
// diffs (single-char append → type_char, single-char delete →
// backspace, anything else → clear+retype). The kiosk's authoritative
// buffer is read back via status; we only override our local copy
// when our <input> is unfocused (otherwise we'd clobber the user's
// cursor mid-typing).
let lastLocalValue = "";
```

- [ ] **Step 3: Add `syncToKiosk` diff function**

Add this function in `remote.js` (near `send`):

```javascript
// Compute the diff between the input's previous and current value;
// emit the WS message(s) that get the kiosk's buffer to match.
//   - Single-char append → {t: "type_char", c}
//   - Single-char delete (from the end) → {t: "key_special", k: "backspace"}
//   - Anything else (paste, multi-delete, IME) → {t: "clear"} + per-char type_chars
function syncToKiosk(newVal, oldVal) {
  // Single-char append at end?
  if (newVal.length === oldVal.length + 1 && newVal.startsWith(oldVal)) {
    send({ t: 'type_char', c: newVal[newVal.length - 1] });
    return;
  }
  // Single-char delete at end?
  if (newVal.length === oldVal.length - 1 && oldVal.startsWith(newVal)) {
    send({ t: 'key_special', k: 'backspace' });
    return;
  }
  // Multi-char change — paste, multi-delete, IME composition commit.
  // Cheapest robust path: clear the kiosk buffer and retype everything.
  send({ t: 'clear' });
  for (const c of newVal) {
    // The WS handler filters non-ASCII anyway, but emit cleanly here too.
    if (c.length === 1 && c.charCodeAt(0) < 0x80) {
      send({ t: 'type_char', c });
    }
  }
}
```

- [ ] **Step 4: Wire input + keydown + clear listeners**

Add (still in `remote.js`, near other event-listener setup):

```javascript
// Every native-keyboard input event (per character, paste, autocomplete-
// commit) fires this. We compute the diff against lastLocalValue and
// emit the matching kiosk message(s).
textInput.addEventListener('input', (e) => {
  const newVal = e.target.value;
  syncToKiosk(newVal, lastLocalValue);
  lastLocalValue = newVal;
});

// The OS keyboard's "Search" / "Return" key. Submit semantics depend
// on the kiosk-side context: search keyboard ignores it (no on_enter
// callback set; search is debounced); WiFi keyboard fires its
// on_enter (commit password attempt).
textInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    e.preventDefault();      // don't submit a form / add a newline
    send({ t: 'key_special', k: 'enter' });
  }
});

// "×" clear affordance.
clearBtn.addEventListener('click', () => {
  textInput.value = '';
  lastLocalValue = '';
  send({ t: 'clear' });
  textInput.focus();    // keep keyboard up so user can keep typing
});
```

- [ ] **Step 5: Sanity-check via dev-tools console**

In a desktop browser open `http://magicpi.local:5000/admin/remote/`. Open dev-tools network tab → WebSocket frames. Manually flip `<body class="text-mode">` via the Elements pane to reveal the section. Type "hi" into the input.

Expected outbound frames:
```json
{"t":"type_char","c":"h"}
{"t":"type_char","c":"i"}
```

Press backspace. Expected:
```json
{"t":"key_special","k":"backspace"}
```

Paste "abc" (Cmd+V from clipboard). Expected:
```json
{"t":"clear"}
{"t":"type_char","c":"a"}
{"t":"type_char","c":"b"}
{"t":"type_char","c":"c"}
```

If any of these messages don't fire as described, fix the diff function before proceeding.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box/web/static/remote/remote.js
git commit -m "feat(remote): JS input listeners + diff → type_char/backspace/clear"
```

---

## Task 10: Phone JS — `applyStatus` for text mode

**Files:**
- Modify: `magic_dingus_box/web/static/remote/remote.js`

- [ ] **Step 1: Locate the existing status handler**

In `remote.js`, find the function that processes incoming `{t: "status", data: ...}` messages from the WS. (Search for `'status'` or the function that updates the now-playing UI from the broadcast.) Note the variable name of the parsed status object — likely `status` or `data`.

- [ ] **Step 2: Add `applyTextInput` invocation**

Inside the status handler, after the existing UI updates (now-playing, position, etc.), call a new helper:

```javascript
applyTextInput(status);   // or whatever the status variable is named
```

Define `applyTextInput` (alongside the other helpers):

```javascript
// Toggle the phone between D-pad mode and text-input mode based on
// the kiosk's text_input.active flag. When the kiosk leaves text
// mode, we also blur the <input> to dismiss any open OS keyboard.
// When the kiosk has truth that doesn't match the phone's local
// value (e.g., physical-controller typing, reconnect catch-up), we
// override — but only if the user isn't actively focused on the
// field (otherwise their cursor would jump mid-keystroke).
function applyTextInput(status) {
  const ti = status && status.text_input;
  if (!ti) return;

  const wantsTextMode = ti.active === true;
  document.body.classList.toggle('text-mode', wantsTextMode);

  if (!wantsTextMode) {
    // Kiosk left the text context — dismiss any open OS keyboard.
    if (document.activeElement === textInput) {
      textInput.blur();
    }
    // Reset local state so a fresh entry next time starts clean.
    lastLocalValue = '';
    return;
  }

  // Apply server-truth to the field, but only when the user isn't
  // actively typing (focus would mean their cursor would jump).
  textTitle.textContent = ti.title || '';
  if (document.activeElement !== textInput) {
    const buf = ti.buffer || '';
    if (textInput.value !== buf) {
      textInput.value = buf;
      lastLocalValue = buf;
    }
  }
}
```

- [ ] **Step 3: Add disconnect handling for the input**

In `remote.js`, find the WS `onclose` / `onerror` handlers (or the reconnect-banner toggling logic). When the WS goes down, disable the text input so the user can't type into the void:

```javascript
// In onopen:
textInput.disabled = false;

// In onclose / onerror:
textInput.disabled = true;
```

(If those handlers don't exist as discrete blocks, search for where `ws = new WebSocket(...)` is set up and add the listeners there.)

- [ ] **Step 4: Manual smoke test on a desktop browser**

```bash
ssh magic@magicpi.local 'sudo systemctl restart magic-dingus-web.service'
```

Wait 2 seconds. Open `http://magicpi.local:5000/admin/remote/` in a browser. The page should load with `<body>` class NOT containing `text-mode`.

Manually inject a fake status into the dev-tools console:

```javascript
applyTextInput({ text_input: { active: true, title: "Search movies", buffer: "shaw" }});
```

Expected: text section appears, title shows "Search movies", input shows "shaw". Type "s" — input becomes "shaws", WS frame sent.

Then:

```javascript
applyTextInput({ text_input: { active: false }});
```

Expected: text section disappears, body class loses `text-mode`.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/static/remote/remote.js
git commit -m "feat(remote): applyStatus mode-swap + disconnect handling"
```

---

## Task 11: Deploy script — exclude the queue file

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/deploy_cpp.sh`

- [ ] **Step 1: Find the existing rsync exclude list**

In `magic_dingus_box_cpp/scripts/deploy_cpp.sh`, search for the existing exclude lines for runtime state (the kiosk has similar files like `paired_remotes.json`, `flask_secret.key`, `seek_request.json`, `kiosk_status.json`, `pending_revocations.txt`).

```bash
grep -n "paired_remotes.json\|flask_secret.key\|seek_request.json\|kiosk_status.json\|pending_revocations.txt" magic_dingus_box_cpp/scripts/deploy_cpp.sh
```

- [ ] **Step 2: Add `text_input_queue.jsonl` to the exclude list**

Wherever those existing excludes are (typically as `--exclude='data/foo'` arguments to rsync), add a sibling line:

```bash
--exclude='data/text_input_queue.jsonl'
```

Match the surrounding style — quoting, indentation, comma/no-comma. The file is per-Pi runtime state; deploys must not wipe it mid-typing or the kiosk would lose in-flight events.

- [ ] **Step 3: Verify the deploy script still parses**

```bash
bash -n magic_dingus_box_cpp/scripts/deploy_cpp.sh && echo "syntax OK"
```
Expected: `syntax OK`.

- [ ] **Step 4: Dry-run sanity check (optional)**

```bash
DRY_RUN=1 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh 2>&1 | grep -i "exclude" | head -10
```
Expected (if the script supports `DRY_RUN`): `text_input_queue.jsonl` appears in the exclude list. If no `DRY_RUN` support, skip.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/scripts/deploy_cpp.sh
git commit -m "deploy: exclude text_input_queue.jsonl from rsync"
```

---

## Task 12: Manual integration test on Pi

**Files:** none — this is a verification step.

- [ ] **Step 1: Deploy and restart**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build 2>&1 | tail -10
```

Expected: clean build, kiosk restart, `Built target magic_dingus_box_cpp` and a "Restarted kiosk" message. The web admin restarts independently:

```bash
ssh magic@magicpi.local 'sudo systemctl restart magic-dingus-web.service'
```

- [ ] **Step 2: Verify status emits text_input.active=false at idle**

```bash
ssh magic@magicpi.local 'cat /opt/magic_dingus_box/magic_dingus_box_cpp/data/kiosk_status.json | python3 -m json.tool | grep -A2 text_input'
```
Expected:
```
"text_input": {
    "active": false
},
```

- [ ] **Step 3: Test MB Search from a paired phone**

On the phone (already paired from prior session), open the Phone Remote page. Use the D-pad to navigate the kiosk into Movies → Search (or whatever flow opens the search keyboard). When the kiosk shows the search keyboard:

Expected on phone: text section fades in, title shows "Search movies", input has focus or accepts taps.

Tap the input. Native OS keyboard appears. Type "shawshank".

Expected on kiosk: results panel updates live as each character arrives. Search results for Shawshank Redemption appear within ~200ms.

Press Backspace 4 times on phone keyboard.

Expected on kiosk: query becomes "shaws", debouncer re-runs search.

Press the phone's "×" clear button.

Expected on kiosk: query becomes empty, results clear.

Use D-pad on phone (below the text section, not blocked while OS keyboard is dismissed) to navigate into the result list and select Shawshank.

Expected: kiosk transitions to detail screen.

- [ ] **Step 4: Test Wi-Fi password from phone**

Use the kiosk's Settings menu (BTN4 from main playlist) → Wi-Fi → select a network. The kiosk shows the password keyboard.

Expected on phone: text section's title changes to "Wi-Fi password" (or whatever the kiosk's title is). Input refocuses. Type a password. Press the OS keyboard's "Search"/Enter key.

Expected on kiosk: keyboard closes, kiosk attempts to connect to the network. (Connect succeeds or fails on its own; either way the keyboard dismissed and on_enter fired.)

- [ ] **Step 5: Test mode-swap when kiosk leaves text mode externally**

With the phone in text mode and the OS keyboard up, use a physical controller (or the phone's BTN4) to close the kiosk's keyboard.

Expected: phone OS keyboard auto-dismisses (because `applyTextInput` blurs the input when `active=false`), text section fades out, D-pad section returns. Within 200ms of the kiosk's status push.

- [ ] **Step 6: Test rate limit doesn't break normal typing**

Type 30 characters quickly into the phone input. Confirm all 30 reach the kiosk (visible in kiosk's search field). The 50-event/sec limit is well above sustained human typing.

- [ ] **Step 7: Inspect the queue file is being drained**

```bash
ssh magic@magicpi.local 'while true; do
  ls -la /opt/magic_dingus_box/magic_dingus_box_cpp/data/text_input_queue.jsonl 2>/dev/null
  sleep 0.5
done' &
```

Type rapidly on the phone. Expected: file size briefly non-zero, immediately returns to 0 — kiosk drains it within one frame (~16ms typically).

Stop the loop with `Ctrl+C`.

- [ ] **Step 8: Cleanup — no commit needed**

Step 1 already shipped; Steps 2-7 are verification only.

If any step fails: fix the underlying task (1-11) and re-run from Step 1.

---

## Summary

12 tasks, ~80 steps total. Backwards-compatible — old paired phones (cached `remote.js`) ignore the new `text_input` status field and stay in D-pad mode permanently; force-reload or cache expiry lifts them into the new behavior. No re-pairing required.

Each task produces a working, committable change. Tasks 1, 3, 4, 6, 7 have unit tests; Tasks 2, 5, 8, 9, 10, 11 are structural / UI changes verified by build success and the integration test in Task 12.
