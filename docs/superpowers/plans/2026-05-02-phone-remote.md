# Phone Remote Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a first-class phone remote delivered as a tab inside Content Manager — kiosk-initiated QR pairing, sub-80 ms input-to-display latency, full mobile-native UX.

**Architecture:** Phone (browser) → WebSocket → Flask → `/dev/uinput` virtual gamepad → kiosk `InputManager` (unchanged). C++ owns pairing-code generation and status-file writes only — no network code. Tap-to-seek uses a separate file-queue (`seek_request.json`). Spec at [docs/superpowers/specs/2026-05-02-phone-remote-design.md](docs/superpowers/specs/2026-05-02-phone-remote-design.md).

**Tech Stack:** C++17 + Catch2 (kiosk side), Python 3 + Flask + Flask-Sock + python-evdev/uinput + pytest (Flask side), vanilla HTML/CSS/JS (phone side, no framework).

---

## File Structure

**C++ (`magic_dingus_box_cpp/src/`):**
- 🆕 `ui/pairing_screen.{h,cpp}` — pairing-code RNG, JSON write, screen state
- 🆕 `ui/pairing_screen_renderer.{h,cpp}` — QR + code + countdown + paired-device list rendering
- ✏️ `ui/settings_menu.{h,cpp}` — `PHONE_REMOTE` menu section + top-level entry
- ✏️ `ui/renderer.cpp` — wire pairing-screen rendering into `render_settings()` flow
- ✏️ `app/controller.{h,cpp}` — `kiosk_status.json` writer @ 5 Hz; `seek_request.json` poller each tick
- ✏️ `app/app_state.h` — `ScreenMode` enum + accessor

**C++ tests (`magic_dingus_box_cpp/tests/phone_remote/`):**
- 🆕 `test_pairing_code.cpp` — code RNG entropy, file write atomicity
- 🆕 `test_status_writer.cpp` — kiosk_status.json schema fidelity

**Flask (`magic_dingus_box/web/`):**
- 🆕 `remote/__init__.py` — package marker
- 🆕 `remote/uinput_writer.py` — virtual device + key emit
- 🆕 `remote/auth.py` — `/pair` endpoint, HMAC cookie issue/verify
- 🆕 `remote/ws_handler.py` — WebSocket protocol & multi-client broadcast
- 🆕 `remote/status_broadcaster.py` — `kiosk_status.json` watcher
- 🆕 `remote/devices.py` — `paired_remotes.json` read/write helpers
- ✏️ `admin.py` — register the `remote` blueprint, add `/pair` and `/admin/remote/ws` routes, add Remote tab to admin shell
- 🆕 `static/remote/remote.html`, `remote.css`, `remote.js` — phone UI
- 🆕 `static/remote/manifest.json` — PWA manifest

**Flask tests (`magic_dingus_box/web/tests/`):**
- 🆕 `test_pairing.py` — HMAC roundtrip, brute-force lockout, code expiry
- 🆕 `test_uinput_writer.py` — event-encoding contract (uses fake device)
- 🆕 `test_ws_protocol.py` — message format, multi-client broadcast
- 🆕 `test_status_broadcast.py` — mtime-watch + diff suppression
- 🆕 `test_remote_e2e.py` — full pair → WS → press → uinput-event flow

**System:**
- 🆕 `magic_dingus_box_cpp/scripts/data/90-magicdingus-uinput.rules` — udev rule
- ✏️ `magic_dingus_box_cpp/scripts/install_deps.sh` — apt install python3-evdev
- ✏️ `magic_dingus_box_cpp/scripts/setup_services.sh` — install udev rule

---

## Phase A — C++ status writer

Goal: kiosk writes `kiosk_status.json` at 5 Hz. After this phase, you can `tail -f data/kiosk_status.json` and watch state change as you navigate.

### Task A.1: Add `ScreenMode` enum to AppState

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h`

- [ ] **Step 1: Read the file**

```bash
head -50 magic_dingus_box_cpp/src/app/app_state.h
```

- [ ] **Step 2: Add `ScreenMode` enum and field**

Insert after the existing struct includes, before the `AppState` struct definition:

```cpp
enum class ScreenMode {
    Playlist,
    Playback,
    Settings,
    RetroArch,
    MediaBrowser
};

inline const char* screen_mode_to_string(ScreenMode m) {
    switch (m) {
        case ScreenMode::Playlist:     return "playlist";
        case ScreenMode::Playback:     return "playback";
        case ScreenMode::Settings:     return "settings";
        case ScreenMode::RetroArch:    return "retroarch";
        case ScreenMode::MediaBrowser: return "media_browser";
    }
    return "unknown";
}
```

Inside the `AppState` struct, add:

```cpp
// Phone remote status sync — derived from current UI mode.
// Initialized to Playlist; updated by main loop / settings_menu / controller.
std::atomic<ScreenMode> screen_mode{ScreenMode::Playlist};
```

- [ ] **Step 3: Verify build still passes**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Expected: build succeeds (no callers yet, just a new field).

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/app/app_state.h
git commit -m "feat(remote): add ScreenMode enum to AppState"
```

### Task A.2: Status writer module

**Files:**
- Create: `magic_dingus_box_cpp/src/app/status_writer.h`
- Create: `magic_dingus_box_cpp/src/app/status_writer.cpp`
- Test: `magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp`

- [ ] **Step 1: Write the failing test**

Create `magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <jsoncpp/json/json.h>
#include "app/app_state.h"
#include "app/status_writer.h"

namespace fs = std::filesystem;

TEST_CASE("status_writer emits required schema fields", "[remote][status]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_test.json";
    fs::remove(tmp);

    app::AppState state;
    state.screen_mode = app::ScreenMode::Playback;
    state.position = 42.5;
    state.duration = 100.0;
    state.is_paused = false;

    app::StatusWriter w(tmp.string());
    w.write_now(state);

    REQUIRE(fs::exists(tmp));

    std::ifstream f(tmp);
    Json::Value root;
    f >> root;

    REQUIRE(root["schema"].asInt() == 1);
    REQUIRE(root["screen"].asString() == "playback");
    REQUIRE(root["playback"]["position_sec"].asDouble() == 42.5);
    REQUIRE(root["playback"]["duration_sec"].asDouble() == 100.0);
    REQUIRE(root["playback"]["is_paused"].asBool() == false);
    REQUIRE(root["ts"].asDouble() > 0.0);
}

TEST_CASE("status_writer atomic write — never leaves partial files", "[remote][status]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_atomic.json";
    fs::remove(tmp);

    app::AppState state;
    state.screen_mode = app::ScreenMode::Playlist;

    app::StatusWriter w(tmp.string());
    w.write_now(state);

    // Verify no leftover .tmp file in the directory
    fs::path tmp_dir = tmp.parent_path();
    bool has_partial = false;
    for (auto& entry : fs::directory_iterator(tmp_dir)) {
        if (entry.path().filename().string().find("mdb_status_atomic") != std::string::npos
            && entry.path().extension() == ".tmp") {
            has_partial = true;
        }
    }
    REQUIRE(has_partial == false);
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

In `magic_dingus_box_cpp/CMakeLists.txt`, find the existing media-browser test block and add a parallel block for phone-remote tests. Search for `add_executable(test_media_browser_unit` and add after that block:

```cmake
# Phone remote tests
file(GLOB PHONE_REMOTE_TEST_SOURCES "tests/phone_remote/*.cpp")
if(PHONE_REMOTE_TEST_SOURCES)
    add_executable(test_phone_remote_unit
        ${PHONE_REMOTE_TEST_SOURCES}
        src/app/status_writer.cpp
        src/ui/pairing_screen.cpp
    )
    target_link_libraries(test_phone_remote_unit PRIVATE Catch2::Catch2WithMain ${JSONCPP_LIBRARIES})
    target_include_directories(test_phone_remote_unit PRIVATE src ${JSONCPP_INCLUDE_DIRS})
    add_test(NAME PhoneRemoteUnit COMMAND test_phone_remote_unit)
endif()
```

(`pairing_screen.cpp` doesn't exist yet but will by the time this target is fully populated; we'll create it as an empty stub now to keep the target buildable.)

- [ ] **Step 3: Create empty stubs so the target builds**

Create `magic_dingus_box_cpp/src/ui/pairing_screen.h`:

```cpp
#pragma once
namespace ui { class PairingScreen { public: PairingScreen() = default; }; }
```

Create `magic_dingus_box_cpp/src/ui/pairing_screen.cpp`:

```cpp
#include "pairing_screen.h"
```

- [ ] **Step 4: Run test to verify it fails (compile error — StatusWriter doesn't exist)**

```bash
cd magic_dingus_box_cpp && mkdir -p build && cd build && cmake .. && make test_phone_remote_unit 2>&1 | tail -10
```

Expected: compile FAIL with `'StatusWriter' is not a member of 'app'` or `status_writer.h: No such file`.

- [ ] **Step 5: Implement the writer**

Create `magic_dingus_box_cpp/src/app/status_writer.h`:

```cpp
#pragma once
#include <string>
#include "app_state.h"

namespace app {

class StatusWriter {
public:
    explicit StatusWriter(std::string path);
    // Writes the current state to disk atomically (temp + rename).
    void write_now(const AppState& state);

private:
    std::string path_;
    std::string tmp_path_;
};

} // namespace app
```

Create `magic_dingus_box_cpp/src/app/status_writer.cpp`:

```cpp
#include "status_writer.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <jsoncpp/json/json.h>

namespace app {

StatusWriter::StatusWriter(std::string path)
    : path_(std::move(path)), tmp_path_(path_ + ".tmp") {}

void StatusWriter::write_now(const AppState& state) {
    Json::Value root;
    root["schema"] = 1;

    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    root["ts"] = duration_cast<duration<double>>(now).count();
    root["screen"] = screen_mode_to_string(state.screen_mode.load());

    // Playlist context — best-effort; readers tolerate nulls.
    Json::Value playlist;
    playlist["name"]       = state.current_playlist_name;
    playlist["item_index"] = state.current_item_index;
    playlist["item_count"] = state.current_item_count;
    root["playlist"] = playlist;

    Json::Value np;
    np["title"]    = state.now_playing_title;
    np["subtitle"] = state.now_playing_subtitle;
    np["kind"]     = state.now_playing_kind;
    root["now_playing"] = np;

    Json::Value playback;
    playback["position_sec"] = state.position.load();
    playback["duration_sec"] = state.duration.load();
    playback["is_paused"]    = state.is_paused.load();
    root["playback"] = playback;

    if (state.screen_mode.load() == ScreenMode::RetroArch) {
        Json::Value ra;
        ra["rom_name"] = state.retroarch_rom_name;
        ra["core"]     = state.retroarch_core;
        root["retroarch"] = ra;
    } else {
        root["retroarch"] = Json::Value::null;
    }

    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    std::string out = Json::writeString(b, root);

    {
        std::ofstream f(tmp_path_, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
    }
    std::rename(tmp_path_.c_str(), path_.c_str());
}

} // namespace app
```

Add the supporting fields to `app_state.h` (next to the `screen_mode` field):

```cpp
// Phone remote status sync — populated by the main loop / controller.
std::string current_playlist_name;
int current_item_index = 0;
int current_item_count = 0;
std::string now_playing_title;
std::string now_playing_subtitle;
std::string now_playing_kind;          // "video" | "game" | "media_browser_movie" | ""
std::string retroarch_rom_name;
std::string retroarch_core;
std::atomic<bool> is_paused{false};
```

(Many of these fields may already exist under different names — if so, *do not duplicate*; instead reuse existing fields and adjust `status_writer.cpp` to read them. Read `app_state.h` carefully before adding.)

- [ ] **Step 6: Run test to verify pass**

```bash
cd magic_dingus_box_cpp/build && make test_phone_remote_unit && ./test_phone_remote_unit -t "[status]" 2>&1 | tail -20
```

Expected: 2 test cases pass.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/app/status_writer.{h,cpp} magic_dingus_box_cpp/src/app/app_state.h magic_dingus_box_cpp/tests/phone_remote/ magic_dingus_box_cpp/CMakeLists.txt magic_dingus_box_cpp/src/ui/pairing_screen.{h,cpp}
git commit -m "feat(remote): kiosk_status.json writer with atomic write + tests"
```

### Task A.3: Wire status writer into main loop

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp`
- Modify: `magic_dingus_box_cpp/src/app/controller.{h,cpp}` (only if controller is the right place — main.cpp may suffice)

- [ ] **Step 1: Read main.cpp around the main loop**

```bash
grep -n "auto input_events\|input.poll()\|gpio.poll()\|auto loop_start" magic_dingus_box_cpp/src/main.cpp | head
```

- [ ] **Step 2: Add the writer instance and 5 Hz tick**

Near the top of `main()` after `AppState` is constructed:

```cpp
#include "app/status_writer.h"
// ...
app::StatusWriter status_writer(utils::Config::data_dir() + "/kiosk_status.json");
auto last_status_write = std::chrono::steady_clock::now();
constexpr auto STATUS_PERIOD = std::chrono::milliseconds(200);  // 5 Hz
```

Inside the main loop, after input/state updates and before render, add:

```cpp
auto now = std::chrono::steady_clock::now();
if (now - last_status_write >= STATUS_PERIOD) {
    status_writer.write_now(state);
    last_status_write = now;
}
```

- [ ] **Step 3: Update `state.screen_mode` from the relevant places**

Find where the kiosk transitions between modes and update `state.screen_mode`:

- When `settings_menu.is_active()` → `state.screen_mode = ScreenMode::Settings;`
- When playback is active (`controller.is_playing()`) → `ScreenMode::Playback`
- Default browsing → `ScreenMode::Playlist`
- During RetroArch fork → `ScreenMode::RetroArch` (set before fork, restore after)
- During Media Browser → `ScreenMode::MediaBrowser` (where existing MB enter/exit lives)

Each transition is a single-line assignment. Use `git grep` to find the 5 transition points.

- [ ] **Step 4: Deploy + verify on Pi**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build --test
ssh magic@magicpi.local "tail -f /opt/magic_dingus_box/data/kiosk_status.json"  # in another shell
```

Expected: file content updates ~5×/s; navigating menus changes `screen` field.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(remote): wire status_writer into main loop @ 5 Hz"
```

---

## Phase B — Flask uinput pipeline

Goal: Flask owns a uinput virtual gamepad. After this phase, `curl -X POST localhost:5000/admin/remote/_debug/press?btn=OK` drives the kiosk. No phone UI yet.

### Task B.1: Install dependencies

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/install_deps.sh`
- Modify: `magic_dingus_box/web/wsgi.py` or `requirements.txt`

- [ ] **Step 1: Locate apt install line in install_deps.sh**

```bash
grep -n "apt install\|apt-get install" magic_dingus_box_cpp/scripts/install_deps.sh
```

- [ ] **Step 2: Add `python3-evdev` to the list**

```bash
# Edit the apt install line to include python3-evdev
```

Add the package name. The Debian-packaged version handles both reading evdev and writing uinput.

- [ ] **Step 3: Add `flask-sock` to Python deps**

Find the existing requirements file (`magic_dingus_box/web/requirements.txt` or similar) and add:

```
flask-sock>=0.7.0
```

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/install_deps.sh magic_dingus_box/web/requirements.txt
git commit -m "feat(remote): add python3-evdev and flask-sock dependencies"
```

### Task B.2: udev rule for `/dev/uinput`

**Files:**
- Create: `magic_dingus_box_cpp/scripts/data/90-magicdingus-uinput.rules`
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh`

- [ ] **Step 1: Create the rule**

`magic_dingus_box_cpp/scripts/data/90-magicdingus-uinput.rules`:

```
# Allow the magic-dingus-web service user to write to /dev/uinput
# for the phone remote virtual gamepad.
KERNEL=="uinput", GROUP="input", MODE="0660"
```

- [ ] **Step 2: Add idempotent install step in setup_services.sh**

Find a good place (near the existing service unit install) and add:

```bash
# Phone remote: udev rule for /dev/uinput access
sudo install -m 0644 "${SCRIPT_DIR}/data/90-magicdingus-uinput.rules" \
    /etc/udev/rules.d/90-magicdingus-uinput.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=uinput
sudo usermod -a -G input "$(systemctl show -p User --value magic-dingus-web.service)"
```

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box_cpp/scripts/data/90-magicdingus-uinput.rules magic_dingus_box_cpp/scripts/setup_services.sh
git commit -m "feat(remote): udev rule + setup wiring for /dev/uinput access"
```

### Task B.3: uinput writer module — failing test

**Files:**
- Create: `magic_dingus_box/web/remote/__init__.py` (empty)
- Test: `magic_dingus_box/web/tests/test_uinput_writer.py`

- [ ] **Step 1: Write the failing test**

Create `magic_dingus_box/web/tests/test_uinput_writer.py`:

```python
"""Verifies that pressing each named button emits the correct evdev event sequence.
Uses a fake uinput device that captures writes instead of opening /dev/uinput."""

import pytest
from magic_dingus_box.web.remote.uinput_writer import UinputWriter, ButtonName


class FakeDevice:
    def __init__(self):
        self.events = []  # list of (type, code, value) tuples

    def write(self, type_, code, value):
        self.events.append((type_, code, value))

    def syn(self):
        self.events.append(("SYN",))


@pytest.fixture
def writer():
    fake = FakeDevice()
    w = UinputWriter(device=fake)
    return w, fake


def test_ok_press_emits_btn_south_down_up(writer):
    w, fake = writer
    w.press(ButtonName.OK, phase="tap")
    # tap = down + up
    assert fake.events[0][:2] == (1, 0x130)  # EV_KEY, BTN_SOUTH
    assert fake.events[0][2] == 1            # press
    assert fake.events[1] == ("SYN",)
    assert fake.events[2][:2] == (1, 0x130)
    assert fake.events[2][2] == 0            # release
    assert fake.events[3] == ("SYN",)


def test_dpad_up_emits_hat_y_down_then_zero(writer):
    w, fake = writer
    w.press(ButtonName.UP, phase="tap")
    # ABS_HAT0Y = 0x11
    assert fake.events[0] == (3, 0x11, -1)   # EV_ABS, ABS_HAT0Y, -1
    assert fake.events[2] == (3, 0x11, 0)


def test_phase_down_does_not_release(writer):
    w, fake = writer
    w.press(ButtonName.OK, phase="down")
    # Only down event; no auto-release
    keydowns = [e for e in fake.events if e[:2] == (1, 0x130) and e[2] == 1]
    keyups = [e for e in fake.events if e[:2] == (1, 0x130) and e[2] == 0]
    assert len(keydowns) == 1
    assert len(keyups) == 0


def test_unknown_button_raises(writer):
    w, _ = writer
    with pytest.raises(ValueError):
        w.press("BOGUS", phase="tap")  # type: ignore
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_uinput_writer.py -v
```

Expected: ImportError or ModuleNotFoundError on `magic_dingus_box.web.remote.uinput_writer`.

- [ ] **Step 3: Implement the writer**

Create `magic_dingus_box/web/remote/__init__.py` (empty file).

Create `magic_dingus_box/web/remote/uinput_writer.py`:

```python
"""Writes evdev events to a /dev/uinput-backed virtual gamepad.

The kiosk's InputManager reads evdev devices and maps button codes to
high-level InputAction values. This writer emits the codes that
mapping table already understands — see input_manager.cpp."""
from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import Optional, Protocol

# evdev type constants
EV_KEY = 1
EV_ABS = 3
EV_SYN = 0
SYN_REPORT = 0

# Button codes (matches Linux input-event-codes.h)
BTN_SOUTH = 0x130
BTN_EAST  = 0x131
BTN_NORTH = 0x133
BTN_WEST  = 0x134
BTN_TL    = 0x136
BTN_TR    = 0x137
BTN_START = 0x13B
KEY_Z     = 44       # KEY_Z for RetroArch hotkey

# Axis codes
ABS_HAT0X = 0x10
ABS_HAT0Y = 0x11


class ButtonName(str, enum.Enum):
    OK         = "OK"
    UP         = "UP"
    DOWN       = "DOWN"
    LEFT       = "LEFT"
    RIGHT      = "RIGHT"
    YELLOW     = "YELLOW"
    RED        = "RED"
    GREEN      = "GREEN"
    BLACK      = "BLACK"
    QUIT_GAME  = "QUIT_GAME"


@dataclass(frozen=True)
class _AxisEvent:
    code: int   # ABS_HAT0X / ABS_HAT0Y
    value: int  # -1, 0, +1


@dataclass(frozen=True)
class _KeyEvent:
    code: int


# Mapping: ButtonName → either a single key code (for buttons) or an axis event (for D-pad).
_MAP: dict[ButtonName, _KeyEvent | _AxisEvent] = {
    ButtonName.OK:        _KeyEvent(BTN_SOUTH),
    ButtonName.UP:        _AxisEvent(ABS_HAT0Y, -1),
    ButtonName.DOWN:      _AxisEvent(ABS_HAT0Y,  1),
    ButtonName.LEFT:      _AxisEvent(ABS_HAT0X, -1),
    ButtonName.RIGHT:     _AxisEvent(ABS_HAT0X,  1),
    ButtonName.YELLOW:    _KeyEvent(BTN_TL),
    ButtonName.RED:       _KeyEvent(BTN_EAST),
    ButtonName.GREEN:     _KeyEvent(BTN_TR),
    ButtonName.BLACK:     _KeyEvent(BTN_NORTH),
    # QUIT_GAME emits Z + Start as the kiosk's existing hotkey for "quit RetroArch".
    # Implemented specially in press() below.
}


class _DeviceProto(Protocol):
    def write(self, type_: int, code: int, value: int) -> None: ...
    def syn(self) -> None: ...


class UinputWriter:
    """Writes events to a uinput-backed device. Pass `device=None` to open
    the real /dev/uinput; pass a fake for tests."""

    def __init__(self, device: Optional[_DeviceProto] = None):
        if device is None:
            device = self._open_real_device()
        self._dev = device

    @staticmethod
    def _open_real_device() -> _DeviceProto:
        # Imported lazily so unit tests don't need uinput available.
        from evdev import UInput, ecodes as e

        capabilities = {
            e.EV_KEY: [BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
                       BTN_TL, BTN_TR, BTN_START, KEY_Z],
            e.EV_ABS: [
                (ABS_HAT0X, e.AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)),
                (ABS_HAT0Y, e.AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)),
            ],
        }
        return UInput(capabilities, name="MagicDingus Phone Remote", phys="flask-remote/0")

    def press(self, btn, phase: str = "tap") -> None:
        """Phase: 'down', 'up', or 'tap' (down+up)."""
        if isinstance(btn, str) and not isinstance(btn, ButtonName):
            try:
                btn = ButtonName(btn)
            except ValueError:
                raise ValueError(f"unknown button: {btn!r}")

        if btn == ButtonName.QUIT_GAME:
            # Z + Start chord — RetroArch's exit-core hotkey.
            self._emit_key_phase(KEY_Z, phase)
            self._emit_key_phase(BTN_START, phase)
            return

        ev = _MAP[btn]
        if isinstance(ev, _KeyEvent):
            self._emit_key_phase(ev.code, phase)
        else:  # _AxisEvent
            self._emit_axis_phase(ev.code, ev.value, phase)

    def _emit_key_phase(self, code: int, phase: str) -> None:
        if phase in ("down", "tap"):
            self._dev.write(EV_KEY, code, 1); self._dev.syn()
        if phase in ("up", "tap"):
            self._dev.write(EV_KEY, code, 0); self._dev.syn()

    def _emit_axis_phase(self, code: int, value: int, phase: str) -> None:
        if phase in ("down", "tap"):
            self._dev.write(EV_ABS, code, value); self._dev.syn()
        if phase in ("up", "tap"):
            self._dev.write(EV_ABS, code, 0); self._dev.syn()
```

- [ ] **Step 4: Run test to verify pass**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_uinput_writer.py -v
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/remote/__init__.py magic_dingus_box/web/remote/uinput_writer.py magic_dingus_box/web/tests/test_uinput_writer.py
git commit -m "feat(remote): uinput writer module with mapping + tests"
```

### Task B.4: Bare WS endpoint (no auth) for end-to-end smoke

**Files:**
- Modify: `magic_dingus_box/web/admin.py`
- Test: `magic_dingus_box/web/tests/test_ws_protocol.py`

- [ ] **Step 1: Write the failing test**

Create `magic_dingus_box/web/tests/test_ws_protocol.py`:

```python
"""Smoke test that the bare WS endpoint accepts press messages and routes
them to the uinput writer. Auth is added in Phase C."""
import json
import pytest
from magic_dingus_box.web.admin import create_app
from magic_dingus_box.web.remote.uinput_writer import UinputWriter, EV_KEY, BTN_SOUTH


@pytest.fixture
def app(tmp_path):
    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    return app


def test_press_via_debug_endpoint_writes_to_uinput(app, monkeypatch):
    captured = []
    class FakeDev:
        def write(self, t, c, v): captured.append((t, c, v))
        def syn(self): captured.append(("SYN",))

    fake_writer = UinputWriter(device=FakeDev())
    app.config["UINPUT_WRITER"] = fake_writer

    client = app.test_client()
    rv = client.post("/admin/remote/_debug/press?btn=OK&phase=tap")
    assert rv.status_code == 200
    # OK = BTN_SOUTH; tap = down then up
    assert (EV_KEY, BTN_SOUTH, 1) in captured
    assert (EV_KEY, BTN_SOUTH, 0) in captured
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_ws_protocol.py -v
```

Expected: 404 (route doesn't exist) or attribute error.

- [ ] **Step 3: Add the debug endpoint to admin.py**

Find the bottom of `create_app()` in `admin.py`, before `return app`. Insert:

```python
    # ============= Phone Remote =============
    from .remote.uinput_writer import UinputWriter

    @app.route("/admin/remote/_debug/press", methods=["POST"])
    def remote_debug_press():
        btn = request.args.get("btn", "")
        phase = request.args.get("phase", "tap")
        writer = app.config.get("UINPUT_WRITER")
        if writer is None:
            try:
                writer = UinputWriter()  # opens real /dev/uinput
                app.config["UINPUT_WRITER"] = writer
            except Exception as e:
                return error_response("uinput_unavailable", str(e), status=503)
        try:
            writer.press(btn, phase=phase)
        except ValueError as e:
            return error_response("bad_button", str(e))
        return success_response({"sent": btn})
```

- [ ] **Step 4: Run test to verify pass**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_ws_protocol.py -v
```

Expected: 1 test passes.

- [ ] **Step 5: End-to-end smoke on Pi**

Deploy and verify:

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh
ssh magic@magicpi.local "sudo systemctl restart magic-dingus-web"
# In another shell:
curl -X POST "http://magicpi.local:5000/admin/remote/_debug/press?btn=OK&phase=tap"
# Watch the kiosk — should react as if SELECT was pressed.
curl -X POST "http://magicpi.local:5000/admin/remote/_debug/press?btn=DOWN&phase=tap"
# Kiosk should move selection down.
```

Expected: kiosk responds to curl-driven presses.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box/web/admin.py magic_dingus_box/web/tests/test_ws_protocol.py
git commit -m "feat(remote): bare /admin/remote/_debug/press endpoint for E2E smoke"
```

### Task B.5: Filter uinput device in InputManager (RetroArch handoff mitigation)

When the kiosk releases evdev devices for RetroArch handoff and re-grabs them on return, it must **not** grab the Flask-owned uinput device — otherwise Flask's writes get queued behind `EVIOCGRAB` and the phone goes dead until next boot.

**Files:**
- Modify: `magic_dingus_box_cpp/src/platform/input_manager.cpp` — skip devices whose `phys` starts with `flask-remote/`

- [ ] **Step 1: Locate the device-open path**

```bash
grep -n "open_joystick_devices\|EVIOCGRAB\|libevdev_new_from_fd\|libevdev_get_phys" magic_dingus_box_cpp/src/platform/input_manager.cpp | head
```

- [ ] **Step 2: Add the filter**

In the device-discovery loop (where each candidate `/dev/input/event*` is opened and `libevdev_new_from_fd` is called), after retrieving the device's `phys` string, skip if it matches our virtual remote:

```cpp
const char* phys = libevdev_get_phys(dev);
if (phys && std::string(phys).rfind("flask-remote/", 0) == 0) {
    spdlog::info("input: skipping flask-remote/* virtual device (owned by Flask)");
    libevdev_free(dev);
    close(fd);
    continue;
}
```

- [ ] **Step 3: Verify on Pi**

Deploy. Pair phone. Launch a RetroArch game and exit it. After exit, verify the phone remote still works (kiosk did not grab the uinput device).

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build --test
# On phone: pair, navigate, launch a game, exit it, navigate again
```

Expected: phone remote responsive both before and after the RetroArch round-trip.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/platform/input_manager.cpp
git commit -m "fix(remote): InputManager skips flask-remote/* uinput device on re-grab"
```

---

## Phase C — Pairing flow

Goal: kiosk Settings → Phone Remote shows a QR. Visiting `…?pair=CODE` sets a signed cookie. The cookie gates a real WS endpoint.

### Task C.1: Pairing-code generation in C++

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/pairing_screen.{h,cpp}` (replace stubs)
- Test: `magic_dingus_box_cpp/tests/phone_remote/test_pairing_code.cpp`

- [ ] **Step 1: Write the failing test**

`magic_dingus_box_cpp/tests/phone_remote/test_pairing_code.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <jsoncpp/json/json.h>
#include "ui/pairing_screen.h"

namespace fs = std::filesystem;

TEST_CASE("pairing code is 6-digit decimal", "[remote][pairing]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_pairing_test.json";
    fs::remove(tmp);

    ui::PairingScreen p(tmp.string());
    p.regenerate();

    REQUIRE(fs::exists(tmp));
    std::ifstream f(tmp);
    Json::Value root;
    f >> root;

    std::string code = root["code"].asString();
    REQUIRE(code.size() == 6);
    for (char c : code) REQUIRE(c >= '0');
    for (char c : code) REQUIRE(c <= '9');
    REQUIRE(root["attempts_remaining"].asInt() == 5);
    REQUIRE(root["expires_at"].asInt64() > root["issued_at"].asInt64());
    REQUIRE(root["nonce"].asString().size() >= 32);
}

TEST_CASE("two regenerations produce different codes", "[remote][pairing]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_pairing_test2.json";
    fs::remove(tmp);

    ui::PairingScreen p(tmp.string());
    p.regenerate();
    auto code1 = p.current_code();
    p.regenerate();
    auto code2 = p.current_code();
    REQUIRE(code1 != code2);  // entropy sanity check
}

TEST_CASE("close() deletes the session file", "[remote][pairing]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_pairing_test3.json";
    fs::remove(tmp);

    ui::PairingScreen p(tmp.string());
    p.regenerate();
    REQUIRE(fs::exists(tmp));
    p.close();
    REQUIRE_FALSE(fs::exists(tmp));
}
```

- [ ] **Step 2: Run test to verify it fails (compile error)**

```bash
cd magic_dingus_box/build && make test_phone_remote_unit 2>&1 | tail
```

Expected: undefined reference to `regenerate`, `current_code`, `close`.

- [ ] **Step 3: Implement pairing screen**

Replace `magic_dingus_box_cpp/src/ui/pairing_screen.h`:

```cpp
#pragma once
#include <chrono>
#include <string>

namespace ui {

class PairingScreen {
public:
    explicit PairingScreen(std::string session_path);

    // Generates a fresh 6-digit code, writes pairing_session.json atomically.
    void regenerate();

    // Polls the file: if Flask deleted it (5 wrong attempts), regenerate.
    // Should be called at ~1 Hz while the screen is open.
    void tick();

    // Removes the session file. Call when leaving the pairing screen.
    void close();

    const std::string& current_code() const { return code_; }
    std::chrono::system_clock::time_point expires_at() const { return expires_at_; }

private:
    std::string session_path_;
    std::string tmp_path_;
    std::string code_;
    std::string nonce_;
    std::chrono::system_clock::time_point issued_at_;
    std::chrono::system_clock::time_point expires_at_;

    void write_atomic_();
    static std::string generate_code_();
    static std::string generate_nonce_();
};

} // namespace ui
```

Replace `magic_dingus_box_cpp/src/ui/pairing_screen.cpp`:

```cpp
#include "pairing_screen.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <jsoncpp/json/json.h>

namespace ui {

namespace {
constexpr auto kCodeWindow = std::chrono::seconds(120);
constexpr int kInitialAttempts = 5;

std::random_device& rng() {
    static std::random_device r;
    return r;
}
}  // namespace

PairingScreen::PairingScreen(std::string session_path)
    : session_path_(std::move(session_path)),
      tmp_path_(session_path_ + ".tmp") {}

std::string PairingScreen::generate_code_() {
    std::uniform_int_distribution<int> d(0, 999999);
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%06d", d(rng()));
    return std::string(buf);
}

std::string PairingScreen::generate_nonce_() {
    std::uniform_int_distribution<unsigned> d(0, 0xFFFFFFFFu);
    std::ostringstream s;
    s << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) s << std::setw(8) << d(rng());
    return s.str();  // 32 hex chars = 128 bits
}

void PairingScreen::regenerate() {
    code_ = generate_code_();
    nonce_ = generate_nonce_();
    issued_at_ = std::chrono::system_clock::now();
    expires_at_ = issued_at_ + kCodeWindow;
    write_atomic_();
}

void PairingScreen::tick() {
    namespace fs = std::filesystem;
    if (code_.empty()) {
        regenerate();
        return;
    }
    if (std::chrono::system_clock::now() >= expires_at_) {
        regenerate();
        return;
    }
    if (!fs::exists(session_path_)) {
        // Flask deleted it (5 wrong attempts); roll a fresh code.
        regenerate();
    }
}

void PairingScreen::close() {
    std::filesystem::remove(session_path_);
    code_.clear();
    nonce_.clear();
}

void PairingScreen::write_atomic_() {
    Json::Value root;
    root["schema"] = 1;
    root["code"] = code_;
    root["issued_at"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            issued_at_.time_since_epoch()).count());
    root["expires_at"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            expires_at_.time_since_epoch()).count());
    root["attempts_remaining"] = kInitialAttempts;
    root["nonce"] = nonce_;

    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    std::string out = Json::writeString(b, root);

    {
        std::ofstream f(tmp_path_, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
    }
    std::rename(tmp_path_.c_str(), session_path_.c_str());
}

}  // namespace ui
```

- [ ] **Step 4: Run test to verify pass**

```bash
cd magic_dingus_box_cpp/build && make test_phone_remote_unit && ./test_phone_remote_unit -t "[pairing]"
```

Expected: 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/pairing_screen.{h,cpp} magic_dingus_box_cpp/tests/phone_remote/test_pairing_code.cpp
git commit -m "feat(remote): C++ pairing-screen code generation + atomic write"
```

### Task C.2: Settings menu wiring

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.h` (add `PHONE_REMOTE` enum value)
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.cpp` (add menu entry + sub-screen state)
- Modify: `magic_dingus_box_cpp/src/main.cpp` (route the menu selection)

- [ ] **Step 1: Add `PHONE_REMOTE` to `MenuSection` enum**

In `settings_menu.h`, add to the enum (next to `INFO`):

```cpp
PHONE_REMOTE,
```

- [ ] **Step 2: Add the top-level menu item**

In `settings_menu.cpp`, find where `Content Manager` is added (around line 376) and add the parallel entry immediately after:

```cpp
menu_items_.emplace_back("Phone Remote", MenuSection::PHONE_REMOTE, "Pair phone");
```

Same for the other place where `Content Manager` is added (around line 49).

- [ ] **Step 3: Add a `PairingScreen` member**

In `settings_menu.h`:

```cpp
#include "pairing_screen.h"

// inside SettingsMenuManager class:
private:
    std::unique_ptr<PairingScreen> pairing_screen_;
public:
    PairingScreen* pairing_screen();
    void open_pairing_screen();
    void close_pairing_screen();
    bool is_pairing_screen_active() const { return pairing_active_; }
private:
    bool pairing_active_ = false;
```

In `settings_menu.cpp`:

```cpp
PairingScreen* SettingsMenuManager::pairing_screen() {
    if (!pairing_screen_) {
        pairing_screen_ = std::make_unique<PairingScreen>(
            utils::Config::data_dir() + "/pairing_session.json");
    }
    return pairing_screen_.get();
}

void SettingsMenuManager::open_pairing_screen() {
    pairing_active_ = true;
    pairing_screen()->regenerate();
}

void SettingsMenuManager::close_pairing_screen() {
    if (pairing_active_) {
        pairing_screen()->close();
        pairing_active_ = false;
    }
}
```

- [ ] **Step 4: Wire the routing in main.cpp**

Find where the existing menu sections are dispatched (look for `case MenuSection::INFO:` or similar) and add:

```cpp
case ui::MenuSection::PHONE_REMOTE:
    settings_menu.open_pairing_screen();
    break;
```

Add a 1 Hz tick for the active pairing screen in the main loop:

```cpp
if (settings_menu.is_pairing_screen_active()) {
    settings_menu.pairing_screen()->tick();
}
```

Add an exit hook (when user backs out of settings or pairing screen):

```cpp
// Wherever settings menu close is handled
settings_menu.close_pairing_screen();
```

- [ ] **Step 5: Verify build**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Expected: build passes (no rendering yet, just state).

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/settings_menu.{h,cpp} magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(remote): wire Phone Remote menu entry to PairingScreen state"
```

### Task C.3: Pairing-screen renderer

**Files:**
- Create: `magic_dingus_box_cpp/src/ui/pairing_screen_renderer.{h,cpp}`
- Modify: `magic_dingus_box_cpp/src/ui/renderer.cpp` — call into the new renderer when pairing screen is active

- [ ] **Step 1: Create the renderer**

`magic_dingus_box_cpp/src/ui/pairing_screen_renderer.h`:

```cpp
#pragma once
#include <vector>
#include <string>

namespace ui {
class Renderer;
class PairingScreen;
struct PairedDevice {
    std::string nickname;
    int64_t last_seen;  // Unix seconds
};

void render_pairing_screen(Renderer& r, const PairingScreen& p,
                           const std::vector<PairedDevice>& paired_devices,
                           int viewport_w, int viewport_h);
}
```

`magic_dingus_box_cpp/src/ui/pairing_screen_renderer.cpp`:

```cpp
#include "pairing_screen_renderer.h"
#include "renderer.h"
#include "pairing_screen.h"
#include "theme.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace ui {

void render_pairing_screen(Renderer& r, const PairingScreen& p,
                           const std::vector<PairedDevice>& paired_devices,
                           int viewport_w, int viewport_h) {
    Theme theme;
    // Title
    r.draw_text("Phone Remote", viewport_w / 2.0f, 60.0f,
                theme.font_title_size, theme.fg, /*center=*/true);

    // QR code — center of the screen
    float qr_size = std::min(viewport_w, viewport_h) * 0.45f;
    float qr_x = (viewport_w - qr_size) / 2.0f;
    float qr_y = 120.0f;

    // URL embedded in QR — admin home with pair param
    std::string url = "http://magicpi.local:5000/?pair=" + p.current_code() + "&tab=remote";
    r.render_qr_code(url, qr_x, qr_y, qr_size, /*alpha=*/1.0f);

    // 6-digit code below QR
    float code_y = qr_y + qr_size + 30.0f;
    r.draw_text(p.current_code(), viewport_w / 2.0f, code_y,
                theme.font_heading_size, theme.accent, /*center=*/true);

    // Countdown
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        p.expires_at() - std::chrono::system_clock::now()).count();
    if (remaining < 0) remaining = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Code refreshes in %lld:%02lld",
                  static_cast<long long>(remaining / 60),
                  static_cast<long long>(remaining % 60));
    r.draw_text(buf, viewport_w / 2.0f, code_y + 36.0f,
                theme.font_small_size, theme.dim, /*center=*/true);

    // Paired devices list at bottom
    float list_y = viewport_h - 200.0f;
    r.draw_text("Paired devices", viewport_w / 2.0f, list_y,
                theme.font_medium_size, theme.fg, /*center=*/true);

    if (paired_devices.empty()) {
        r.draw_text("(none yet — scan the QR code with your phone)",
                    viewport_w / 2.0f, list_y + 30.0f,
                    theme.font_small_size, theme.dim, /*center=*/true);
    } else {
        for (size_t i = 0; i < paired_devices.size(); ++i) {
            const auto& d = paired_devices[i];
            float y = list_y + 30.0f + static_cast<float>(i) * 22.0f;
            r.draw_text(d.nickname, viewport_w / 2.0f, y,
                        theme.font_small_size, theme.fg, /*center=*/true);
        }
    }
}

}  // namespace ui
```

- [ ] **Step 2: Wire into the main render path**

In `renderer.cpp`, find where the Settings menu is rendered (search for `render_settings` or the INFO submenu render). Add a check before that:

```cpp
if (settings_menu.is_pairing_screen_active()) {
    render_pairing_screen(*this, *settings_menu.pairing_screen(),
                          paired_devices_, viewport_w_, viewport_h_);
    return;
}
```

(`paired_devices_` will be populated by reading `paired_remotes.json` on settings entry — that's Task C.5; for now, pass an empty vector.)

- [ ] **Step 3: Update CMakeLists.txt**

Add `src/ui/pairing_screen_renderer.cpp` to the kiosk's source list (search for `pairing_screen.cpp` and add the new file alongside).

- [ ] **Step 4: Deploy + verify**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build --test
```

Navigate Settings → Phone Remote on the Pi. Expected: QR code + 6-digit code + countdown visible.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/pairing_screen_renderer.{h,cpp} magic_dingus_box_cpp/src/ui/renderer.cpp magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(remote): render pairing screen (QR + code + countdown + device list)"
```

### Task C.4: Flask /pair handler — failing test

**Files:**
- Test: `magic_dingus_box/web/tests/test_pairing.py`
- Create: `magic_dingus_box/web/remote/auth.py`
- Create: `magic_dingus_box/web/remote/devices.py`

- [ ] **Step 1: Write the failing test**

Create `magic_dingus_box/web/tests/test_pairing.py`:

```python
"""End-to-end pairing tests: code validation, brute-force lockout, cookie issue, revoke."""
import json
import time
from pathlib import Path

import pytest

from magic_dingus_box.web.admin import create_app


@pytest.fixture
def app(tmp_path):
    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    app.config["SECRET_KEY"] = "test-secret-key"
    return app


@pytest.fixture
def client(app):
    return app.test_client()


def write_session(tmp_path: Path, code="847291", attempts=5, expires_in=120):
    payload = {
        "schema": 1,
        "code": code,
        "issued_at": int(time.time()),
        "expires_at": int(time.time()) + expires_in,
        "attempts_remaining": attempts,
        "nonce": "abc123" * 8,
    }
    p = tmp_path / "pairing_session.json"
    p.write_text(json.dumps(payload))
    return p


def test_pair_with_correct_code_issues_cookie(client, app, tmp_path):
    write_session(tmp_path)
    rv = client.get("/?pair=847291&tab=remote", follow_redirects=False)
    # Redirects to /?tab=remote with a Set-Cookie
    assert rv.status_code in (302, 303)
    assert "mdb_remote" in rv.headers.get("Set-Cookie", "")


def test_pair_with_wrong_code_decrements_attempts(client, app, tmp_path):
    p = write_session(tmp_path, attempts=5)
    rv = client.get("/?pair=000000")
    assert rv.status_code == 401
    data = json.loads(p.read_text())
    assert data["attempts_remaining"] == 4
    # Cookie not set
    assert "mdb_remote" not in rv.headers.get("Set-Cookie", "")


def test_five_wrong_attempts_deletes_session(client, tmp_path):
    p = write_session(tmp_path, attempts=1)
    rv = client.get("/?pair=000000")
    assert rv.status_code == 401
    assert not p.exists()


def test_expired_code_returns_410(client, tmp_path):
    write_session(tmp_path, expires_in=-10)
    rv = client.get("/?pair=847291")
    assert rv.status_code == 410


def test_no_session_file_returns_410(client):
    rv = client.get("/?pair=847291")
    assert rv.status_code == 410


def test_revoked_device_rejects_cookie(client, app, tmp_path):
    write_session(tmp_path)
    rv = client.get("/?pair=847291&tab=remote", follow_redirects=False)
    assert rv.status_code in (302, 303)

    # Find the device id and remove from paired_remotes.json
    paired_path = tmp_path / "paired_remotes.json"
    data = json.loads(paired_path.read_text())
    assert len(data["devices"]) == 1
    device_id = data["devices"][0]["id"]
    data["devices"] = []
    paired_path.write_text(json.dumps(data))

    # The cookie still HMAC-verifies but the device lookup misses → 401
    cookie = rv.headers["Set-Cookie"].split(";")[0]
    name, value = cookie.split("=", 1)
    client.set_cookie(domain="localhost", key=name, value=value)
    rv2 = client.get("/admin/remote/protected_check")
    assert rv2.status_code == 401
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_pairing.py -v
```

Expected: failures (no /pair handler, no auth module).

- [ ] **Step 3: Implement auth + devices**

Create `magic_dingus_box/web/remote/devices.py`:

```python
"""Read/write paired_remotes.json (the device list)."""
from __future__ import annotations

import json
import os
import time
import uuid
from pathlib import Path
from typing import Optional


def _load(path: Path) -> dict:
    if not path.exists():
        return {"schema": 1, "devices": []}
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError:
        return {"schema": 1, "devices": []}


def _save_atomic(path: Path, data: dict) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(tmp, path)


def add_device(path: Path, nickname: str, user_agent_hint: str = "") -> str:
    data = _load(path)
    device_id = uuid.uuid4().hex
    data["devices"].append({
        "id": device_id,
        "nickname": nickname or "Phone",
        "user_agent_hint": user_agent_hint,
        "paired_at": int(time.time()),
        "last_seen": int(time.time()),
    })
    _save_atomic(path, data)
    return device_id


def find_device(path: Path, device_id: str) -> Optional[dict]:
    data = _load(path)
    for d in data["devices"]:
        if d["id"] == device_id:
            return d
    return None


def touch_last_seen(path: Path, device_id: str) -> None:
    data = _load(path)
    for d in data["devices"]:
        if d["id"] == device_id:
            d["last_seen"] = int(time.time())
            break
    _save_atomic(path, data)


def revoke_device(path: Path, device_id: str) -> bool:
    data = _load(path)
    before = len(data["devices"])
    data["devices"] = [d for d in data["devices"] if d["id"] != device_id]
    if len(data["devices"]) != before:
        _save_atomic(path, data)
        return True
    return False
```

Create `magic_dingus_box/web/remote/auth.py`:

```python
"""Pairing endpoint + HMAC cookie issue/verify."""
from __future__ import annotations

import hmac
import hashlib
import json
import os
import time
from pathlib import Path
from typing import Optional

from flask import current_app, request, redirect, make_response

from . import devices as devices_mod

COOKIE_NAME = "mdb_remote"
COOKIE_MAX_AGE = 60 * 60 * 24 * 365  # 1 year


def _data_dir() -> Path:
    return Path(current_app.config["DATA_DIR"])


def _session_path() -> Path:
    return _data_dir() / "pairing_session.json"


def _devices_path() -> Path:
    return _data_dir() / "paired_remotes.json"


def _audit_path() -> Path:
    return _data_dir() / "pairing_audit.log"


def _hmac(device_id: str, issued_at: int) -> str:
    secret = current_app.config["SECRET_KEY"].encode("utf-8")
    msg = f"{device_id}|{issued_at}".encode("utf-8")
    return hmac.new(secret, msg, hashlib.sha256).hexdigest()


def issue_cookie(response, device_id: str) -> None:
    issued_at = int(time.time())
    sig = _hmac(device_id, issued_at)
    value = f"{device_id}.{issued_at}.{sig}"
    response.set_cookie(
        COOKIE_NAME, value,
        max_age=COOKIE_MAX_AGE, httponly=True, samesite="Strict", path="/",
    )


def verify_cookie(cookie_value: str) -> Optional[str]:
    """Returns the device_id if valid AND not revoked, else None."""
    if not cookie_value:
        return None
    parts = cookie_value.split(".")
    if len(parts) != 3:
        return None
    device_id, issued_str, sig = parts
    try:
        issued_at = int(issued_str)
    except ValueError:
        return None
    expected = _hmac(device_id, issued_at)
    if not hmac.compare_digest(expected, sig):
        return None
    if devices_mod.find_device(_devices_path(), device_id) is None:
        return None
    return device_id


def _audit(outcome: str, code_attempt: str, ip: str) -> None:
    line = json.dumps({
        "ts": int(time.time()),
        "ip": ip,
        "outcome": outcome,
        "code": code_attempt[:2] + "****",  # don't log the full code
    })
    with _audit_path().open("a") as f:
        f.write(line + "\n")


def handle_pair_param(submitted_code: str):
    """Called from the admin index handler when ?pair= is present.
    Returns a Flask response, or None to indicate 'not pairing — pass through'."""
    session_path = _session_path()
    ip = request.remote_addr or "?"

    if not session_path.exists():
        _audit("no_session", submitted_code, ip)
        from flask import abort
        return abort(410, "Pairing screen not open on the kiosk.")

    try:
        session = json.loads(session_path.read_text())
    except json.JSONDecodeError:
        _audit("session_corrupt", submitted_code, ip)
        from flask import abort
        return abort(410)

    if int(time.time()) > session["expires_at"]:
        _audit("expired", submitted_code, ip)
        session_path.unlink(missing_ok=True)
        from flask import abort
        return abort(410, "Code expired. Open Settings → Phone Remote on the kiosk.")

    if not hmac.compare_digest(session["code"], submitted_code):
        # Decrement attempts atomically; delete if 0.
        session["attempts_remaining"] -= 1
        if session["attempts_remaining"] <= 0:
            session_path.unlink(missing_ok=True)
            _audit("locked_out", submitted_code, ip)
        else:
            tmp = session_path.with_suffix(".json.tmp")
            tmp.write_text(json.dumps(session))
            os.replace(tmp, session_path)
            _audit("wrong_code", submitted_code, ip)
        from flask import abort
        return abort(401, "Wrong code.")

    # Success — issue cookie, register device, consume session.
    user_agent = request.headers.get("User-Agent", "")
    nickname = "Phone"  # Will be replaced by Step C.6 nickname-prompt.
    device_id = devices_mod.add_device(_devices_path(), nickname, user_agent)
    session_path.unlink(missing_ok=True)
    _audit("paired", submitted_code, ip)

    target = request.args.get("tab", "remote")
    resp = redirect(f"/?tab={target}", code=303)
    issue_cookie(resp, device_id)
    return resp
```

- [ ] **Step 4: Wire into admin.py index route**

In `admin.py`, find the index route handler (or the `create_app` setup of routes for `/`). Add at the top of that handler:

```python
from .remote import auth as remote_auth

@app.route("/")
def index():
    pair_code = request.args.get("pair")
    if pair_code:
        resp = remote_auth.handle_pair_param(pair_code)
        if resp is not None:
            return resp
    # ... existing index handling ...
```

Also add a debug "protected_check" route the test uses:

```python
@app.route("/admin/remote/protected_check")
def remote_protected_check():
    cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
    device_id = remote_auth.verify_cookie(cookie)
    if device_id is None:
        return error_response("unpaired", "Not paired", status=401)
    return success_response({"device_id": device_id})
```

Make sure `app.config["DATA_DIR"]` is set in `create_app`:

```python
app.config["DATA_DIR"] = str(data_dir)
```

- [ ] **Step 5: Run test to verify pass**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_pairing.py -v
```

Expected: 6 tests pass.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box/web/remote/auth.py magic_dingus_box/web/remote/devices.py magic_dingus_box/web/admin.py magic_dingus_box/web/tests/test_pairing.py
git commit -m "feat(remote): /pair flow with HMAC cookie + brute-force lockout"
```

### Task C.5: Read paired devices into kiosk

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.cpp` — load `paired_remotes.json` on entry
- Modify: `magic_dingus_box_cpp/src/ui/renderer.cpp` — pass list to `render_pairing_screen`

- [ ] **Step 1: Add a paired-devices load helper**

In `pairing_screen_renderer.h` (already created in Task C.3), add:

```cpp
std::vector<PairedDevice> load_paired_devices(const std::string& path);
```

In `pairing_screen_renderer.cpp`:

```cpp
#include <fstream>
#include <jsoncpp/json/json.h>

std::vector<PairedDevice> load_paired_devices(const std::string& path) {
    std::vector<PairedDevice> out;
    std::ifstream f(path);
    if (!f) return out;
    Json::Value root;
    f >> root;
    for (const auto& d : root["devices"]) {
        PairedDevice pd;
        pd.nickname = d["nickname"].asString();
        pd.last_seen = d["last_seen"].asInt64();
        out.push_back(pd);
    }
    return out;
}
```

- [ ] **Step 2: Refresh the list at 1 Hz while pairing screen is open**

In `renderer.cpp` near the pairing-screen render call:

```cpp
static auto last_devices_load = std::chrono::steady_clock::time_point{};
static std::vector<PairedDevice> cached_devices;
auto now = std::chrono::steady_clock::now();
if (now - last_devices_load > std::chrono::seconds(1)) {
    cached_devices = load_paired_devices(utils::Config::data_dir() + "/paired_remotes.json");
    last_devices_load = now;
}
render_pairing_screen(*this, *settings_menu.pairing_screen(), cached_devices,
                      viewport_w_, viewport_h_);
```

- [ ] **Step 3: Deploy + verify**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build --test
# On the Pi: open Settings → Phone Remote.
# In a second shell, simulate a pairing:
ssh magic@magicpi.local 'echo {\"schema\":1,\"devices\":[{\"id\":\"test\",\"nickname\":\"iPhone test\",\"last_seen\":1000,\"paired_at\":1000}]} > /opt/magic_dingus_box/data/paired_remotes.json'
```

Expected: kiosk pairing screen shows "iPhone test" within ~1s.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/pairing_screen_renderer.{h,cpp} magic_dingus_box_cpp/src/ui/renderer.cpp
git commit -m "feat(remote): kiosk reads paired_remotes.json + renders device list"
```

### Task C.6: Nickname prompt on first pair

**Files:**
- Modify: `magic_dingus_box/web/remote/auth.py` — split pair into "submit code" and "set nickname"
- Modify: `magic_dingus_box/web/static/remote/` — small landing page for nickname

- [ ] **Step 1: Adjust the pair flow**

Update `handle_pair_param` to redirect to a nickname prompt instead of straight to `/?tab=remote`:

```python
# Replace the "Success" block at the end of handle_pair_param:
device_id = devices_mod.add_device(_devices_path(), "Phone", user_agent)  # placeholder name
session_path.unlink(missing_ok=True)
_audit("paired", submitted_code, ip)
resp = redirect(f"/admin/remote/name?d={device_id}", code=303)
issue_cookie(resp, device_id)
return resp
```

Add a new route:

```python
@app.route("/admin/remote/name", methods=["GET", "POST"])
def remote_name():
    cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
    device_id = remote_auth.verify_cookie(cookie)
    if device_id is None:
        return redirect("/", code=303)
    if request.method == "POST":
        nickname = request.form.get("nickname", "").strip()[:40] or "Phone"
        # Update the entry in paired_remotes.json
        from .remote import devices as devices_mod
        path = Path(app.config["DATA_DIR"]) / "paired_remotes.json"
        data = json.loads(path.read_text())
        for d in data["devices"]:
            if d["id"] == device_id:
                d["nickname"] = nickname
        path.write_text(json.dumps(data, indent=2))
        return redirect("/?tab=remote", code=303)
    return render_template_string("""
<!doctype html>
<html><head><title>Name your remote</title></head>
<body style="font-family:system-ui;background:#1F191F;color:#F2E4D9;text-align:center;padding:60px">
<h1>Name this remote</h1>
<form method="post" style="display:flex;flex-direction:column;align-items:center;gap:12px">
<input name="nickname" placeholder="iPhone (Alex)" autofocus
       style="padding:12px;background:#2A232A;color:#F2E4D9;border:1px solid #968B85;border-radius:8px;font-size:16px" />
<button type="submit"
       style="padding:12px 24px;background:#F5BF42;color:#1F191F;border:none;border-radius:8px;font-weight:600;font-size:16px">
Pair</button>
</form>
</body></html>
""")
```

- [ ] **Step 2: Update the test for the new redirect target**

Update `test_pair_with_correct_code_issues_cookie` in `test_pairing.py`:

```python
assert rv.location.endswith("/admin/remote/name?d=" + ANY_DEVICE_ID)
```

Or just assert it redirects somewhere AND a cookie is set — drop the path assertion.

- [ ] **Step 3: Run tests**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_pairing.py -v
```

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box/web/remote/auth.py magic_dingus_box/web/admin.py magic_dingus_box/web/tests/test_pairing.py
git commit -m "feat(remote): nickname prompt on first pair"
```

---

## Phase D — Phone UI

Goal: render the locked Style A · Variant 1 layout on a phone, connect to the WS, send press events.

### Task D.1: Static HTML/CSS skeleton

**Files:**
- Create: `magic_dingus_box/web/static/remote/remote.html`
- Create: `magic_dingus_box/web/static/remote/remote.css`

- [ ] **Step 1: Create the HTML structure**

`magic_dingus_box/web/static/remote/remote.html`:

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#1F191F">
<link rel="manifest" href="/static/remote/manifest.json">
<link rel="stylesheet" href="/static/remote/remote.css">
<title>Magic Dingus Remote</title>
</head>
<body class="remote">
<div id="app" class="screen">
  <div class="now-playing" data-mode="playlist">
    <div class="dot" id="conn-dot"></div>
    <div class="text">
      <div class="label" id="np-label">Now Playing</div>
      <div class="title" id="np-title">—</div>
    </div>
  </div>

  <div class="scrub" id="scrub">
    <div class="scrub-fill" id="scrub-fill" style="width:0%"></div>
  </div>
  <div class="scrub-time">
    <span id="time-now">0:00</span><span id="time-total">0:00</span>
  </div>

  <div class="dpad-wrap">
    <div class="dpad">
      <button class="arm up"    data-btn="UP"    aria-label="Up"></button>
      <button class="arm down"  data-btn="DOWN"  aria-label="Down"></button>
      <button class="arm left"  data-btn="LEFT"  aria-label="Left"></button>
      <button class="arm right" data-btn="RIGHT" aria-label="Right"></button>
      <button class="center"    data-btn="OK"    aria-label="Select">OK</button>
    </div>
  </div>

  <div class="actions">
    <button class="btn yellow" data-btn="YELLOW">PREV</button>
    <button class="btn red"    data-btn="RED" id="btn-red">PAUSE</button>
    <button class="btn green"  data-btn="GREEN">NEXT</button>
    <button class="btn black"  data-btn="BLACK">MENU</button>
  </div>
</div>
<script src="/static/remote/remote.js"></script>
</body></html>
```

- [ ] **Step 2: Write the CSS — locked Style A Variant 1 tokens**

`magic_dingus_box/web/static/remote/remote.css`:

```css
:root {
  --bg: #1F191F;
  --bg-lift: #2A232A;
  --fg: #F2E4D9;
  --dim: #968B85;
  --accent: #F5BF42;     /* gold — focus */
  --hot: #EA3A27;        /* red */
  --success: #66DD7A;    /* green */
  --action: #5884B1;     /* steel blue */
}
* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
html, body {
  background: var(--bg); color: var(--fg);
  font-family: -apple-system, BlinkMacSystemFont, "Inter", system-ui, sans-serif;
  height: 100%;
  overscroll-behavior: none;
  -webkit-touch-callout: none;
}
body.remote { user-select: none; }

.screen {
  display: flex; flex-direction: column;
  width: 100%; min-height: 100dvh;
  padding:
    calc(env(safe-area-inset-top) + 14px)
    calc(env(safe-area-inset-right) + 14px)
    calc(env(safe-area-inset-bottom) + 14px)
    calc(env(safe-area-inset-left) + 14px);
}

.now-playing {
  background: var(--bg-lift); border-radius: 10px;
  padding: 10px 12px; display: flex; align-items: center; gap: 10px;
}
.now-playing .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--success); flex: none; }
.now-playing .dot[data-state="amber"] { background: var(--accent); }
.now-playing .dot[data-state="red"]   { background: var(--hot); animation: pulse 1.2s infinite; }
.now-playing .label { color: var(--dim); font-size: 9px; text-transform: uppercase; letter-spacing: 1px; }
.now-playing .title { font-size: 14px; font-weight: 500; margin-top: 2px; }
@keyframes pulse { 0%,100% { opacity: 1 } 50% { opacity: 0.4 } }

.scrub { height: 4px; background: var(--bg-lift); border-radius: 2px; margin-top: 12px;
         touch-action: manipulation; cursor: pointer; }
.scrub-fill { background: var(--accent); height: 100%; border-radius: 2px; transition: width 0.1s linear; }
.scrub-time { display: flex; justify-content: space-between;
              font-size: 9px; color: var(--dim); margin-top: 4px;
              font-variant-numeric: tabular-nums; }

.dpad-wrap { flex: 1; display: flex; align-items: center; justify-content: center; padding: 14px 0; }
.dpad { position: relative; width: 220px; height: 220px; }
.dpad .arm {
  position: absolute; background: var(--bg-lift); border: none; border-radius: 10px;
  color: var(--fg);
  touch-action: manipulation;
  transition: transform 70ms ease-out, background 70ms ease-out;
}
.dpad .arm:active { transform: scale(0.96); background: #3a323a; }
.dpad .arm.up    { top: 0;    left: 75px;  width: 70px; height: 75px; }
.dpad .arm.down  { bottom: 0; left: 75px;  width: 70px; height: 75px; }
.dpad .arm.left  { left: 0;   top: 75px;   width: 75px; height: 70px; }
.dpad .arm.right { right: 0;  top: 75px;   width: 75px; height: 70px; }
.dpad .arm::before {
  content: ""; position: absolute; top: 50%; left: 50%;
  transform: translate(-50%, -50%); border: 11px solid transparent;
}
.dpad .arm.up::before    { border-bottom-color: var(--fg); border-top: 0; }
.dpad .arm.down::before  { border-top-color: var(--fg); border-bottom: 0; }
.dpad .arm.left::before  { border-right-color: var(--fg); border-left: 0; }
.dpad .arm.right::before { border-left-color: var(--fg); border-right: 0; }

.dpad .center {
  position: absolute; top: 82px; left: 82px; width: 56px; height: 56px;
  background: var(--accent); color: var(--bg);
  border: 4px solid var(--bg);  /* outer ring */
  border-radius: 50%;
  font-weight: 700; font-size: 12px; letter-spacing: 1px;
  touch-action: manipulation;
  transition: transform 70ms ease-out, filter 70ms ease-out;
}
.dpad .center:active { transform: scale(0.94); filter: brightness(0.9); }

.actions { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; margin-top: 12px; }
.actions .btn {
  height: 48px; border: none; border-radius: 10px;
  font-size: 11px; font-weight: 700; letter-spacing: 0.5px;
  touch-action: manipulation;
  transition: transform 70ms ease-out, filter 70ms ease-out;
}
.actions .btn:active { transform: scale(0.96); filter: brightness(0.85); }
.actions .btn.yellow { background: var(--accent); color: var(--bg); }
.actions .btn.red    { background: var(--hot);    color: var(--fg); }
.actions .btn.green  { background: var(--success);color: var(--bg); }
.actions .btn.black  { background: var(--bg-lift);color: var(--fg); border: 1px solid var(--dim); }

/* Mode-specific reveal/hide */
.screen[data-mode="playlist"] .scrub,
.screen[data-mode="playlist"] .scrub-time,
.screen[data-mode="settings"] .scrub,
.screen[data-mode="settings"] .scrub-time { visibility: hidden; }
```

- [ ] **Step 3: Add a Flask route to serve the page**

In `admin.py`, add:

```python
@app.route("/admin/remote", methods=["GET"])
def remote_page():
    cookie = request.cookies.get(remote_auth.COOKIE_NAME, "")
    device_id = remote_auth.verify_cookie(cookie)
    if device_id is None:
        return render_template_string("""
<!doctype html><html><body style="font-family:system-ui;background:#1F191F;color:#F2E4D9;text-align:center;padding:60px">
<h1>Remote not paired</h1><p>On the kiosk, open Settings → Phone Remote and scan the QR code.</p>
</body></html>
""")
    return send_from_directory("static/remote", "remote.html")
```

- [ ] **Step 4: Manual smoke**

Deploy + visit `http://magicpi.local:5000/admin/remote` after pairing. Expected: D-pad UI renders correctly. Buttons don't do anything yet (Task D.2).

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/static/remote/remote.{html,css} magic_dingus_box/web/admin.py
git commit -m "feat(remote): phone UI HTML/CSS skeleton (Style A variant 1)"
```

### Task D.2: WS handler + multi-client broadcast

**Files:**
- Create: `magic_dingus_box/web/remote/ws_handler.py`
- Modify: `magic_dingus_box/web/admin.py` — register flask-sock, mount the WS

- [ ] **Step 1: Write the handler**

`magic_dingus_box/web/remote/ws_handler.py`:

```python
"""WebSocket protocol for the phone remote.

Each connected phone gets one Connection object. The handler:
  - Verifies the auth cookie on connect (else closes 4401).
  - Reads incoming JSON messages: press, seek, hello.
  - Forwards press → uinput. Forwards seek → seek_request.json.
  - Listens for status broadcasts (status_broadcaster will register a queue per conn).
"""
from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from pathlib import Path
from queue import Queue, Empty
from threading import Lock
from typing import List

from flask import current_app, request


@dataclass
class Connection:
    sock: object              # flask-sock WebSocket
    device_id: str
    queue: "Queue[dict]"


_lock = Lock()
_connections: List[Connection] = []


def all_connections() -> List[Connection]:
    with _lock:
        return list(_connections)


def _add(conn: Connection) -> None:
    with _lock:
        _connections.append(conn)


def _remove(conn: Connection) -> None:
    with _lock:
        try:
            _connections.remove(conn)
        except ValueError:
            pass


def handle_connection(ws, *, uinput_writer, data_dir: Path, verify_cookie):
    """Drive a single connection until the socket closes."""
    cookie = request.cookies.get("mdb_remote", "")
    device_id = verify_cookie(cookie)
    if device_id is None:
        ws.close(reason="unpaired")
        return

    conn = Connection(sock=ws, device_id=device_id, queue=Queue(maxsize=64))
    _add(conn)
    try:
        # Send hello
        ws.send(json.dumps({"t": "hello_ack", "schema": 1}))
        while True:
            # Drain outgoing queue first (status pushes from broadcaster)
            try:
                while True:
                    msg = conn.queue.get_nowait()
                    ws.send(json.dumps(msg))
            except Empty:
                pass

            raw = ws.receive(timeout=0.05)
            if raw is None:
                continue
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue

            t = msg.get("t")
            if t == "press":
                btn = msg.get("btn", "")
                phase = msg.get("phase", "tap")
                try:
                    uinput_writer.press(btn, phase=phase)
                except ValueError:
                    ws.send(json.dumps({"t": "error", "code": "bad_button", "msg": btn}))
            elif t == "seek":
                pos = float(msg.get("pos", 0.0))
                pos = max(0.0, min(1.0, pos))
                req = data_dir / "seek_request.json"
                tmp = data_dir / "seek_request.json.tmp"
                tmp.write_text(json.dumps({
                    "schema": 1, "pos": pos, "ts": time.time(),
                }))
                os.replace(tmp, req)
                ws.send(json.dumps({"t": "ack", "of": "seek", "ok": True}))
            elif t == "hello":
                pass  # already handshook
    finally:
        _remove(conn)
```

- [ ] **Step 2: Wire into admin.py**

```python
from flask_sock import Sock
sock = Sock(app)

@sock.route("/admin/remote/ws")
def remote_ws(ws):
    from .remote import ws_handler, auth as remote_auth
    writer = app.config.get("UINPUT_WRITER")
    if writer is None:
        from .remote.uinput_writer import UinputWriter
        writer = UinputWriter()
        app.config["UINPUT_WRITER"] = writer
    ws_handler.handle_connection(
        ws,
        uinput_writer=writer,
        data_dir=Path(app.config["DATA_DIR"]),
        verify_cookie=remote_auth.verify_cookie,
    )
```

- [ ] **Step 3: Phone-side JS — connect & emit presses**

Create `magic_dingus_box/web/static/remote/remote.js`:

```js
(function () {
  'use strict';

  const screen = document.getElementById('app');
  const dot = document.getElementById('conn-dot');

  let ws = null;
  let backoff = 250;
  let lastStatusTs = 0;

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${proto}//${location.host}/admin/remote/ws`);
    ws.onopen = () => {
      backoff = 250;
      ws.send(JSON.stringify({ t: 'hello', client: 'remote-v1', schema: 1 }));
      dot.dataset.state = 'green';
    };
    ws.onmessage = (e) => {
      const msg = JSON.parse(e.data);
      if (msg.t === 'status') applyStatus(msg.data);
    };
    ws.onclose = () => {
      dot.dataset.state = 'red';
      setTimeout(connect, backoff);
      backoff = Math.min(backoff * 2, 5000);
    };
    ws.onerror = () => {};
  }

  function send(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
  }

  function bindPress(el, btn) {
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      send({ t: 'press', btn, phase: 'down' });
      if ('vibrate' in navigator) navigator.vibrate(8);
    });
    el.addEventListener('pointerup',    () => send({ t: 'press', btn, phase: 'up' }));
    el.addEventListener('pointercancel', () => send({ t: 'press', btn, phase: 'up' }));
    el.addEventListener('pointerleave',  () => send({ t: 'press', btn, phase: 'up' }));
  }

  document.querySelectorAll('[data-btn]').forEach((el) => {
    bindPress(el, el.dataset.btn);
  });

  function applyStatus(s) {
    lastStatusTs = Date.now();
    screen.dataset.mode = s.screen || 'playlist';
    const np = s.now_playing || {};
    document.getElementById('np-label').textContent =
      (s.screen === 'playback') ? 'Now Playing' :
      (s.screen === 'settings') ? 'Settings' :
      (s.playlist && s.playlist.name) ? s.playlist.name : 'Playlist';
    document.getElementById('np-title').textContent = np.title || '—';
    if (s.playback) {
      const pct = s.playback.duration_sec
        ? (100 * s.playback.position_sec / s.playback.duration_sec)
        : 0;
      document.getElementById('scrub-fill').style.width = `${pct}%`;
      document.getElementById('time-now').textContent  = fmt(s.playback.position_sec);
      document.getElementById('time-total').textContent = fmt(s.playback.duration_sec);
      document.getElementById('btn-red').textContent = s.playback.is_paused ? 'PLAY' : 'PAUSE';
    }
  }

  function fmt(sec) {
    sec = Math.max(0, Math.floor(sec || 0));
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
    return h ? `${h}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`
             : `${m}:${String(s).padStart(2,'0')}`;
  }

  // Connection-state amber after 1s without status
  setInterval(() => {
    if (ws && ws.readyState === 1) {
      const age = Date.now() - lastStatusTs;
      dot.dataset.state = age > 5000 ? 'red' : age > 1000 ? 'amber' : 'green';
    }
  }, 500);

  // Tap-to-seek
  document.getElementById('scrub').addEventListener('click', (e) => {
    if (screen.dataset.mode !== 'playback') return;
    const rect = e.currentTarget.getBoundingClientRect();
    const pos = (e.clientX - rect.left) / rect.width;
    send({ t: 'seek', pos: Math.max(0, Math.min(1, pos)) });
  });

  connect();
})();
```

- [ ] **Step 4: Manual smoke on Pi**

Deploy. Pair a phone. Open the remote tab. Buttons should drive the kiosk.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/remote/ws_handler.py magic_dingus_box/web/static/remote/remote.js magic_dingus_box/web/admin.py
git commit -m "feat(remote): WS handler + phone JS — D-pad presses drive kiosk"
```

---

## Phase E — Status broadcast

Goal: phone shows live Now Playing / playback position / mode-aware UI driven by `kiosk_status.json`.

### Task E.1: Status broadcaster thread

**Files:**
- Create: `magic_dingus_box/web/remote/status_broadcaster.py`
- Test: `magic_dingus_box/web/tests/test_status_broadcast.py`

- [ ] **Step 1: Write the failing test**

`magic_dingus_box/web/tests/test_status_broadcast.py`:

```python
"""Verifies the broadcaster fans out status changes to all queues, and
suppresses re-sends when mtime hasn't changed."""
import json
import time
from pathlib import Path
from queue import Queue

import pytest

from magic_dingus_box.web.remote.status_broadcaster import StatusBroadcaster


def write_status(path: Path, screen="playlist"):
    path.write_text(json.dumps({"schema": 1, "ts": time.time(), "screen": screen}))


def test_broadcaster_fans_out_to_all_queues(tmp_path):
    p = tmp_path / "kiosk_status.json"
    write_status(p, screen="playlist")
    q1, q2 = Queue(), Queue()
    b = StatusBroadcaster(p, [q1, q2], interval_s=0.05)
    b.start()
    time.sleep(0.1)
    write_status(p, screen="playback")
    time.sleep(0.2)
    b.stop()
    msgs1 = list(q1.queue)
    msgs2 = list(q2.queue)
    # Each queue should have received at least one message with screen=playback
    assert any(m["data"]["screen"] == "playback" for m in msgs1)
    assert any(m["data"]["screen"] == "playback" for m in msgs2)


def test_broadcaster_suppresses_unchanged_mtime(tmp_path):
    p = tmp_path / "kiosk_status.json"
    write_status(p)
    q = Queue()
    b = StatusBroadcaster(p, [q], interval_s=0.05)
    b.start()
    time.sleep(0.3)  # multiple ticks, but file unchanged
    b.stop()
    msgs = list(q.queue)
    # Initial read produces 1 message; subsequent ticks should not re-send
    assert len(msgs) == 1
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_status_broadcast.py -v
```

- [ ] **Step 3: Implement the broadcaster**

`magic_dingus_box/web/remote/status_broadcaster.py`:

```python
"""Watches kiosk_status.json mtime; fans out parsed snapshots to all queues."""
from __future__ import annotations

import json
import threading
import time
from pathlib import Path
from queue import Queue, Full
from typing import Iterable, List


class StatusBroadcaster:
    def __init__(self, path: Path, queues: Iterable[Queue], interval_s: float = 0.2):
        self._path = Path(path)
        self._queues = list(queues)  # caller may also mutate via add_queue/remove_queue
        self._interval = interval_s
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_mtime = 0.0
        self._lock = threading.Lock()

    def add_queue(self, q: Queue) -> None:
        with self._lock:
            self._queues.append(q)

    def remove_queue(self, q: Queue) -> None:
        with self._lock:
            try:
                self._queues.remove(q)
            except ValueError:
                pass

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)

    def _run(self) -> None:
        while not self._stop.wait(self._interval):
            try:
                mtime = self._path.stat().st_mtime
            except FileNotFoundError:
                continue
            if mtime <= self._last_mtime:
                continue
            self._last_mtime = mtime
            try:
                data = json.loads(self._path.read_text())
            except json.JSONDecodeError:
                continue
            msg = {"t": "status", "data": data}
            with self._lock:
                snapshot = list(self._queues)
            for q in snapshot:
                try:
                    q.put_nowait(msg)
                except Full:
                    pass  # drop frame if a slow client backs up
```

- [ ] **Step 4: Run tests**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_status_broadcast.py -v
```

Expected: 2 tests pass.

- [ ] **Step 5: Wire into admin.py — start broadcaster on app create**

```python
# In create_app, near the bottom:
from .remote.status_broadcaster import StatusBroadcaster
status_path = Path(app.config["DATA_DIR"]) / "kiosk_status.json"
app.config["STATUS_BROADCASTER"] = StatusBroadcaster(status_path, queues=[], interval_s=0.2)
app.config["STATUS_BROADCASTER"].start()
```

In `ws_handler.handle_connection`, after `_add(conn)`:

```python
broadcaster = current_app.config.get("STATUS_BROADCASTER")
if broadcaster:
    broadcaster.add_queue(conn.queue)
try:
    # ... existing loop ...
finally:
    if broadcaster:
        broadcaster.remove_queue(conn.queue)
    _remove(conn)
```

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box/web/remote/status_broadcaster.py magic_dingus_box/web/admin.py magic_dingus_box/web/remote/ws_handler.py magic_dingus_box/web/tests/test_status_broadcast.py
git commit -m "feat(remote): status broadcaster fans kiosk_status.json to all phones"
```

### Task E.2: Mode-aware UI on the phone

The JS in Task D.2 already routes `screen` to `data-mode`. Verify on Pi:

- [ ] **Step 1: Manual verification**

Deploy. Open the remote on a phone. Navigate the kiosk through each mode:

- Playlist: title shows playlist name, scrub bar hidden
- Playback: scrub bar visible with live progress, PAUSE/PLAY label flips
- Settings: title shows "Settings", scrub hidden
- RetroArch: (see Task E.3)
- Media Browser: behaves like Playlist

- [ ] **Step 2: Commit any tweaks**

If the JS needs adjustment to handle a mode correctly, edit `remote.js` and commit:

```bash
git add magic_dingus_box/web/static/remote/remote.js
git commit -m "fix(remote): tighten mode-aware UI for <mode>"
```

### Task E.3: RetroArch takeover screen

**Files:**
- Modify: `magic_dingus_box/web/static/remote/remote.{html,css,js}`

- [ ] **Step 1: Add a hidden "Quit Game" overlay**

In `remote.html`, before the closing `</div>` of `#app`:

```html
<div id="ra-overlay" class="ra-overlay" hidden>
  <div class="ra-title" id="ra-title">Game in progress</div>
  <button class="ra-quit" data-btn="QUIT_GAME">Quit Game</button>
</div>
```

- [ ] **Step 2: CSS**

Append to `remote.css`:

```css
.ra-overlay {
  position: fixed; inset: 0; background: rgba(31,25,31,0.95);
  display: flex; flex-direction: column; align-items: center; justify-content: center;
  gap: 32px; padding: 32px;
  padding-top: calc(env(safe-area-inset-top) + 32px);
  padding-bottom: calc(env(safe-area-inset-bottom) + 32px);
  z-index: 100;
}
.ra-overlay[hidden] { display: none; }
.ra-title { font-size: 18px; color: var(--dim); text-align: center; }
.ra-quit {
  background: var(--hot); color: var(--fg); border: none; border-radius: 16px;
  padding: 24px 40px; font-size: 18px; font-weight: 700;
  box-shadow: 0 6px 16px rgba(234, 58, 39, 0.4);
}
.ra-quit:active { transform: scale(0.96); }
```

- [ ] **Step 3: JS — show overlay when screen=retroarch**

In `applyStatus()`, after setting `data-mode`:

```js
const ra = document.getElementById('ra-overlay');
if (s.screen === 'retroarch') {
  ra.hidden = false;
  document.getElementById('ra-title').textContent =
    s.retroarch && s.retroarch.rom_name ? s.retroarch.rom_name : 'Game in progress';
} else {
  ra.hidden = true;
}
```

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box/web/static/remote/remote.{html,css,js}
git commit -m "feat(remote): RetroArch takeover screen with Quit Game button"
```

---

## Phase F — Tap-to-seek round-trip

Goal: tapping the scrub bar on the phone seeks the kiosk's video.

### Task F.1: C++ seek-request poller

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/controller.{h,cpp}`

- [ ] **Step 1: Add a poller method**

In `controller.h` declare:

```cpp
public:
    // Polled each tick; reads data/seek_request.json if present and seeks.
    void poll_seek_request();

private:
    std::string seek_request_path_;
```

In `controller.cpp` constructor:

```cpp
seek_request_path_ = utils::Config::data_dir() + "/seek_request.json";
```

Implementation:

```cpp
void Controller::poll_seek_request() {
    namespace fs = std::filesystem;
    if (!fs::exists(seek_request_path_)) return;

    Json::Value root;
    {
        std::ifstream f(seek_request_path_);
        if (!f) return;
        try { f >> root; } catch (...) { fs::remove(seek_request_path_); return; }
    }
    fs::remove(seek_request_path_);  // consume

    if (!root.isMember("pos")) return;
    double frac = root["pos"].asDouble();
    if (frac < 0.0 || frac > 1.0) return;
    double dur = state_->duration.load();
    if (dur <= 0.0) return;
    double target = frac * dur;
    seek_to(target);  // existing method
}
```

- [ ] **Step 2: Call from main loop**

In `main.cpp`, in the main loop body:

```cpp
controller.poll_seek_request();
```

- [ ] **Step 3: Deploy + manually test**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
# Start playback, then on phone tap a position on the scrub bar.
```

Expected: video jumps to tapped position.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/app/controller.{h,cpp} magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(remote): kiosk polls seek_request.json each tick"
```

---

## Phase G — Mobile-native shell

Goal: Remote tab is full-screen and PWA-installable on iOS/Android. Other admin tabs get a responsive baseline pass.

### Task G.1: PWA manifest + install hint

**Files:**
- Create: `magic_dingus_box/web/static/remote/manifest.json`
- Create: `magic_dingus_box/web/static/remote/icon-192.png`, `icon-512.png` (use the kiosk app icon)
- Modify: `magic_dingus_box/web/static/remote/remote.{html,js}`

- [ ] **Step 1: Manifest**

`magic_dingus_box/web/static/remote/manifest.json`:

```json
{
  "name": "Magic Dingus Remote",
  "short_name": "Dingus",
  "start_url": "/admin/remote",
  "display": "standalone",
  "background_color": "#1F191F",
  "theme_color": "#1F191F",
  "orientation": "portrait",
  "icons": [
    {"src": "/static/remote/icon-192.png", "sizes": "192x192", "type": "image/png"},
    {"src": "/static/remote/icon-512.png", "sizes": "512x512", "type": "image/png"}
  ]
}
```

- [ ] **Step 2: iOS-specific meta tags**

In `remote.html` `<head>`:

```html
<link rel="apple-touch-icon" href="/static/remote/icon-192.png">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Dingus">
```

- [ ] **Step 3: One-time "Add to Home Screen" toast on iOS**

In `remote.js`, on first load (use `localStorage` to track):

```js
function maybeShowInstallHint() {
  const isStandalone = window.navigator.standalone === true ||
                       window.matchMedia('(display-mode: standalone)').matches;
  const isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent);
  if (isStandalone) return;
  if (localStorage.getItem('mdb_remote_install_hint_shown')) return;
  if (!isIOS) return;
  const t = document.createElement('div');
  t.className = 'install-toast';
  t.innerHTML = '<strong>Add to Home Screen</strong> for full-screen remote.<br>' +
                'Tap Share → Add to Home Screen.';
  t.onclick = () => { t.remove(); localStorage.setItem('mdb_remote_install_hint_shown', '1'); };
  document.body.appendChild(t);
}
maybeShowInstallHint();
```

CSS:

```css
.install-toast {
  position: fixed; bottom: 16px; left: 16px; right: 16px;
  background: var(--bg-lift); color: var(--fg);
  padding: 14px 16px; border-radius: 12px; border: 1px solid var(--accent);
  font-size: 13px; line-height: 1.4; z-index: 200;
  padding-bottom: calc(14px + env(safe-area-inset-bottom));
}
```

- [ ] **Step 4: Source the icon files**

Copy the existing kiosk app icon (PNG) to `static/remote/icon-{192,512}.png`. If no icon exists, generate from the Marquee colors using ImageMagick:

```bash
convert -size 512x512 xc:'#1F191F' \
  -fill '#F5BF42' -draw "circle 256,256 256,180" \
  -font Helvetica-Bold -pointsize 200 -fill '#1F191F' -gravity center -annotate 0 'D' \
  magic_dingus_box/web/static/remote/icon-512.png
convert magic_dingus_box/web/static/remote/icon-512.png -resize 192x192 \
  magic_dingus_box/web/static/remote/icon-192.png
```

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/static/remote/{manifest.json,icon-192.png,icon-512.png,remote.html,remote.css,remote.js}
git commit -m "feat(remote): PWA manifest + iOS install hint"
```

### Task G.2: Embed Remote tab inside Content Manager

**Files:**
- Modify: `magic_dingus_box/web/static/index.html` (or wherever the admin shell lives)
- Modify: `magic_dingus_box/web/static/manager.js` — add Remote tab handler

- [ ] **Step 1: Locate the existing tab strip**

```bash
grep -n "tab\|nav-item\|#tabs" magic_dingus_box/web/static/index.html magic_dingus_box/web/static/manager.js | head
```

- [ ] **Step 2: Add a Remote tab entry**

Add to the tab definitions (mirror existing tabs' structure exactly). The tab content should be an `<iframe src="/admin/remote">` so the existing remote.html is reused as-is. Wrap with viewport-detection:

```js
// In manager.js where tabs are wired up, add:
function activateRemoteTab() {
  const isPhone = window.matchMedia('(max-width: 700px) and (pointer: coarse)').matches;
  if (isPhone) {
    // Full-screen takeover: navigate to /admin/remote outright
    location.assign('/admin/remote');
    return;
  }
  // Desktop: render in iframe inside the tab
  const target = document.getElementById('tab-remote');
  target.innerHTML = '<iframe src="/admin/remote" style="width:100%;height:600px;border:0;border-radius:12px"></iframe>';
}
```

- [ ] **Step 3: Verify on real devices**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh
```

- Open `http://magicpi.local:5000/` on iPhone Safari → Remote tab should redirect to full-screen `/admin/remote`.
- Open on a laptop → Remote tab renders an iframe inside the admin shell.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box/web/static/index.html magic_dingus_box/web/static/manager.js
git commit -m "feat(remote): Remote tab in Content Manager (full-screen on phone, iframe on desktop)"
```

### Task G.3: Other admin tabs — responsive baseline

**Files:**
- Modify: `magic_dingus_box/web/static/style.css` and `table_styles.css`

- [ ] **Step 1: Mobile breakpoint**

Append to `magic_dingus_box/web/static/style.css`:

```css
@media (max-width: 700px) {
  /* Tab strip: scrollable pill bar */
  .tabs, [role="tablist"] {
    display: flex; gap: 8px; overflow-x: auto;
    padding: 8px 12px;
    scroll-snap-type: x mandatory;
    -webkit-overflow-scrolling: touch;
  }
  .tabs button, [role="tab"] {
    flex: none; scroll-snap-align: start;
    min-height: 44px; padding: 0 16px; border-radius: 22px;
  }
  /* Tap targets — all buttons */
  button, .button, a.button { min-height: 44px; }
  /* Tables — horizontal scroll instead of squish */
  table { display: block; overflow-x: auto; }
  /* Hint banner for desktop-leaning flows */
  .desktop-recommended {
    background: var(--bg-lift); padding: 12px; border-radius: 8px;
    margin-bottom: 12px; font-size: 13px; color: var(--dim);
  }
}
```

- [ ] **Step 2: Add hint banners to heavy-flow tabs**

Find the playlists, ROM upload, and media upload sections in `index.html`. Inside each, at the top:

```html
<div class="desktop-recommended">
  Best on a laptop or tablet — bulk uploads and drag-drop work better with a larger screen.
</div>
```

- [ ] **Step 3: Manual smoke on a phone**

Open admin tabs on a phone. Check: tap targets are large, tabs scroll, tables scroll horizontally without breaking layout.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box/web/static/style.css magic_dingus_box/web/static/index.html
git commit -m "feat(remote): mobile baseline pass for non-Remote admin tabs"
```

---

## Phase H — Forget device + audit polish

Goal: kiosk's "Forget device" button on a paired-device row revokes that phone immediately.

### Task H.1: Forget-device flow

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/pairing_screen.{h,cpp}` — selection + "Forget" action
- Modify: `magic_dingus_box_cpp/src/ui/pairing_screen_renderer.cpp` — render selected row highlight + button hint

- [ ] **Step 1: Add row-selection state**

In `pairing_screen.h`, add:

```cpp
public:
    void select_next_device(int n_devices);
    void select_prev_device(int n_devices);
    int selected_device_index() const { return selected_device_; }
    void forget_selected_device(const std::vector<std::string>& device_ids);

private:
    int selected_device_ = 0;
```

In `pairing_screen.cpp`:

```cpp
void PairingScreen::select_next_device(int n) {
    if (n <= 0) { selected_device_ = 0; return; }
    selected_device_ = (selected_device_ + 1) % n;
}
void PairingScreen::select_prev_device(int n) {
    if (n <= 0) { selected_device_ = 0; return; }
    selected_device_ = (selected_device_ - 1 + n) % n;
}
void PairingScreen::forget_selected_device(const std::vector<std::string>& ids) {
    if (selected_device_ < 0 || selected_device_ >= (int)ids.size()) return;
    // Append to revocations queue file; Flask drains it.
    std::filesystem::path data_dir = std::filesystem::path(session_path_).parent_path();
    std::ofstream f((data_dir / "pending_revocations.txt").string(), std::ios::app);
    f << ids[selected_device_] << "\n";
}
```

- [ ] **Step 2: Flask consumes the revocation file**

In `auth.py`:

```python
def reap_revocations(data_dir: Path) -> None:
    """Drain the kiosk-written revoke file and remove devices."""
    rev_path = Path(data_dir) / "pending_revocations.txt"
    if not rev_path.exists():
        return
    text = rev_path.read_text()
    rev_path.unlink(missing_ok=True)
    paired = data_dir / "paired_remotes.json"
    if not paired.exists(): return
    data = json.loads(paired.read_text())
    ids = {ln.strip() for ln in text.splitlines() if ln.strip()}
    data["devices"] = [d for d in data["devices"] if d["id"] not in ids]
    tmp = paired.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(tmp, paired)
```

Call `reap_revocations()` at the start of each `verify_cookie()` call OR on a small timer. Simplest: call it from the broadcaster thread once per second.

In `status_broadcaster.py` `_run()`, before the mtime check:

```python
try:
    auth.reap_revocations(self._path.parent)
except Exception:
    pass
```

(Add `from . import auth` at the top of `status_broadcaster.py`.)

- [ ] **Step 3: Render selection + bind the input**

In `pairing_screen_renderer.cpp`, highlight the selected row by drawing a subtle background rectangle behind it (use `theme.bg_lift` or `theme.accent` at low alpha).

In the kiosk's input handling for the pairing screen, map:
- `ROTATE_VERTICAL` (D-pad up/down) → `select_next_device` / `select_prev_device`
- `BTN2 / PLAY_PAUSE` (red) → `forget_selected_device`

- [ ] **Step 4: Manual test**

Pair a phone. Open Settings → Phone Remote on the kiosk. Use the rotary to highlight the device, press red to forget. Phone should be kicked off within ~1s.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/pairing_screen.{h,cpp} magic_dingus_box_cpp/src/ui/pairing_screen_renderer.cpp magic_dingus_box/web/remote/auth.py magic_dingus_box/web/remote/status_broadcaster.py
git commit -m "feat(remote): forget-device flow from kiosk pairing screen"
```

### Task H.2: End-to-end integration test

**Files:**
- Test: `magic_dingus_box/web/tests/test_remote_e2e.py`

- [ ] **Step 1: Write the test**

```python
"""Full pair → WS → press → uinput-event flow with fakes."""
import json
import time
from pathlib import Path
from queue import Queue

import pytest

from magic_dingus_box.web.admin import create_app
from magic_dingus_box.web.remote.uinput_writer import UinputWriter, EV_KEY, BTN_SOUTH


class FakeDev:
    def __init__(self): self.events = []
    def write(self, t, c, v): self.events.append((t, c, v))
    def syn(self): self.events.append(("SYN",))


def test_full_pair_then_press(tmp_path):
    # 1. Kiosk writes pairing_session.json
    (tmp_path / "pairing_session.json").write_text(json.dumps({
        "schema": 1, "code": "111111",
        "issued_at": int(time.time()), "expires_at": int(time.time()) + 60,
        "attempts_remaining": 5, "nonce": "x" * 32,
    }))

    # 2. App
    app = create_app(data_dir=tmp_path)
    app.config["SECRET_KEY"] = "test-key"
    fake = FakeDev()
    app.config["UINPUT_WRITER"] = UinputWriter(device=fake)

    # 3. Pair
    client = app.test_client()
    rv = client.get("/?pair=111111&tab=remote", follow_redirects=False)
    assert "mdb_remote" in rv.headers.get("Set-Cookie", "")

    # 4. Direct uinput call (skipping WS for unit isolation)
    app.config["UINPUT_WRITER"].press("OK", phase="tap")
    keydowns = [e for e in fake.events if e[:2] == (EV_KEY, BTN_SOUTH) and e[2] == 1]
    keyups   = [e for e in fake.events if e[:2] == (EV_KEY, BTN_SOUTH) and e[2] == 0]
    assert keydowns and keyups
```

- [ ] **Step 2: Run the test**

```bash
cd magic_dingus_box/web && python -m pytest tests/test_remote_e2e.py -v
```

Expected: pass.

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box/web/tests/test_remote_e2e.py
git commit -m "test(remote): end-to-end pair → press integration test"
```

### Task H.3: Latency budget verification on real Pi

- [ ] **Step 1: Add timestamp instrumentation**

In `remote.js`, log press send time. In `controller.cpp`, log when the synthesized input event arrives. Use the system journal:

```cpp
spdlog::debug("phone press received: btn={} delta_ms={}", btn_name, delta_ms);
```

- [ ] **Step 2: Run on a phone over typical home Wi-Fi**

Press OK 50 times. Compute p50/p95 from the logs:

```bash
ssh magic@magicpi.local "journalctl -u magic-dingus-box-cpp --since '5 min ago' | grep 'phone press' | awk '{print $NF}' | sort -n"
```

- [ ] **Step 3: Verify p95 < 80 ms**

If p95 > 80 ms, document fallbacks in the spec (status payload trim, WS compression). If hits target, commit a small note:

```bash
echo "P95 latency: <X> ms (measured 2026-XX-XX)" >> docs/superpowers/specs/2026-05-02-phone-remote-design.md
git add docs/superpowers/specs/2026-05-02-phone-remote-design.md
git commit -m "docs(remote): record measured latency"
```

---

## Self-review checklist

(Engineer should re-run before declaring complete.)

- [ ] All Pairing flow steps from spec section "Pairing & token lifecycle" map to a task ✓ (Phase C)
- [ ] All status-sync schema fields are emitted by the C++ writer ✓ (Task A.2)
- [ ] All buttons in the spec's Button-to-InputAction table appear in `_MAP` in `uinput_writer.py` ✓ (Task B.3)
- [ ] All five `screen` modes have a corresponding UI rule in `applyStatus()` ✓ (Tasks D.2, E.3)
- [ ] Brute-force lockout deletes session file ✓ (Task C.4)
- [ ] PWA manifest + iOS install hint ✓ (Task G.1)
- [ ] Mobile-native shell only Remote tab fullscreen; others responsive baseline only ✓ (Tasks G.2, G.3)
- [ ] Latency p95 measured and documented ✓ (Task H.3)
- [ ] Forget-device round-trip works ✓ (Task H.1)
- [ ] No `TODO`/`TBD`/placeholder text in any committed file
