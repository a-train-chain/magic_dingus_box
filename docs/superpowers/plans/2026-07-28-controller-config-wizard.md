# Controller Setup Wizard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Settings-menu wizard that captures any USB gamepad's physical layout by prompting for each button, stores it per controller model, and drives both RetroArch per-core binds (per port) and kiosk menu navigation.

**Architecture:** Three layers replace today's fused per-core tables in `controller_mapping.cpp`: semantic tables (RetroPad slot ← LogicalControl, per core per style), physical profiles (LogicalControl → evdev binding + RetroArch udev token; built-ins for the two shipped pads, captured for everything else), and a combiner `build_mapping()` producing the existing `ControllerMapping` struct so the launcher contract is unchanged. A snapshot regression test written FIRST locks today's exact output for all 10 cores × both pads.

**Tech Stack:** C++17, Catch2 v3 (FetchContent), libevdev, jsoncpp, CMake. Mac-testable modules in `src/retroarch/`; Pi-only I/O stays in the launcher / InputManager.

**Spec:** `docs/superpowers/specs/2026-07-28-controller-config-wizard-design.md` (approved 2026-07-28).

## Global Constraints

- Repo root: the git worktree at `.claude/worktrees/controller-config-wizard` of `magic_dingus_box ` (parent dir name has a trailing space — always quote paths).
- All paths below are relative to repo root; C++ paths relative to `magic_dingus_box_cpp/` where obvious from context.
- **Zero behavior change for the two shipped pads** until the wizard exists: the snapshot suite (Task 1) must pass unmodified after every task.
- Mac test loop (from `magic_dingus_box_cpp/`): `mkdir -p build-mac && cd build-mac && cmake .. && make test_retroarch_unit -j8 && ./test_retroarch_unit` (add `"[tag]"` to filter). The main kiosk binary does NOT build on Mac; only test targets do. Compile-verify UI/platform changes with `deploy_cpp.sh --build` against the Pi (`PI_HOST=magic@magicpi5.local` or `magic@10.55.0.1`).
- New kiosk sources must be added BOTH to the relevant `set(..._SOURCES ...)` list in `CMakeLists.txt` (RETROARCH_SOURCES line ~144, UI_SOURCES line ~110, PLATFORM_SOURCES line ~89) AND, for Mac-testable modules, to the `test_retroarch_unit` source list (line ~462).
- Commit style: conventional commits (`feat(retroarch): ...`, `feat(ui): ...`, `test(retroarch): ...`, `docs: ...`). Commit after every green test cycle. End commit messages with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- `ControllerMapping` struct fields and defaults (b_btn="1", y_btn="3", select_btn="10", start_btn="2", a_btn="0", x_btn="4", l_btn="5", r_btn="6", up_btn="h0up"…, l_x_plus="+0"…, analog_dpad_mode="1") are LOAD-BEARING: legacy per-core branches rely on unset fields keeping these defaults, and emitted configs include them. Never change them.
- New persisted file: `config/controller_profiles.json` (under `config/*`, already OTA-preserved). Env override `MAGIC_CONTROLLER_PROFILES_FILE` for tests.
- Existing tests that must stay green: `test_retroarch_unit` (incl. `tests/retroarch/test_controller_mapping.cpp`), `test_launch_contract.cpp` suite.

## File Structure

| File | Responsibility | Status |
|---|---|---|
| `src/retroarch/logical_controls.{h,cpp}` | `ControllerStyle`, `LogicalControl` enums, string keys, wizard step order | Create |
| `src/retroarch/controller_profile.{h,cpp}` | `PhysicalBinding`, `PhysicalProfile`, built-in profiles, JSON (de)serialize, store load/save, per-port resolution, menu-overlay derivation | Create |
| `src/retroarch/joydev_index.{h,cpp}` | evdev code → RetroArch udev bind token (pure) | Create |
| `src/retroarch/capture_session.{h,cpp}` | Wizard capture state machine (pure, zero I/O) | Create |
| `src/retroarch/controller_mapping.{h,cpp}` | + `SemanticMapping`, `get_semantic_mapping()`, `build_mapping()`, `write_player_binds()`; legacy tables become semantic tables + builtin profiles | Modify |
| `src/retroarch/controller_detector.{h,cpp}` | + expose `match_vid_pid()`, add `detect_connected_controllers()` | Modify |
| `src/retroarch/retroarch_launcher.cpp` | Per-port resolution; P1/P2 block replaced by `write_player_binds()` calls | Modify |
| `src/platform/input_manager.{h,cpp}` | Menu-nav overlays, raw-capture mode, `DeviceCaps` | Modify |
| `src/ui/controller_wizard.{h,cpp}` | Wizard UI state (phases, prompts, save) | Create |
| `src/ui/controller_wizard_renderer.cpp` | `Renderer::render_controller_wizard()` overlay | Create |
| `src/ui/settings_menu.{h,cpp}` | `CONTROLLER_SETUP` row + wizard open/close (PairingScreen pattern) | Modify |
| `src/ui/renderer.h` + `src/ui/renderer.cpp` | render dispatch for wizard overlay | Modify |
| `src/main.cpp` | SELECT dispatch + wizard input interception + overlay reload | Modify |
| `src/utils/config.{h,cpp}` | `get_controller_profiles_file()` | Modify |
| `src/tools/controller_probe.cpp` | On-Pi token cross-check binary (Linux-only target) | Create |
| `tests/retroarch/mapping_snapshot_util.h`, `test_mapping_snapshot.cpp`, `mapping_snapshot_golden.h` | Snapshot gate | Create |
| `tests/retroarch/test_logical_controls.cpp`, `test_controller_profile.cpp`, `test_joydev_index.cpp`, `test_capture_session.cpp`, `test_player_binds.cpp` | Unit tests (auto-globbed) | Create |

---

### Task 1: Snapshot regression gate (written against CURRENT code)

Locks today's exact `get_mapping()` output for every shipped core × {N64_ADAPTER, PS_STYLE_DRAGONRISE, UNKNOWN} plus an unknown core, BEFORE any refactor. This suite is the license for every later task.

**Files:**
- Create: `magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_util.h`
- Create: `magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h`
- Create: `magic_dingus_box_cpp/tests/retroarch/test_mapping_snapshot.cpp`

**Interfaces:**
- Produces: `serialize_mapping(const retroarch::ControllerMapping&) -> std::string` (test-only helper); golden map `kMappingGolden: std::map<std::string, std::string>` keyed `"<PAD>|<core>"` where PAD ∈ `N64`, `PS`, `UNKNOWN`.

- [ ] **Step 1: Write the serializer helper**

`tests/retroarch/mapping_snapshot_util.h`:

```cpp
#pragma once
#include <sstream>
#include <string>
#include "retroarch/controller_mapping.h"

// Canonical one-string form of every ControllerMapping field. Field order
// is frozen — the golden strings in mapping_snapshot_golden.h depend on it.
inline std::string serialize_mapping(const retroarch::ControllerMapping& m) {
    std::ostringstream o;
    o << "name=" << m.name << "\n"
      << "adm=" << m.analog_dpad_mode << "|drv=" << m.input_driver
      << "|pad=" << m.core_option_pad_type << "\n"
      << "btn=" << m.b_btn << "," << m.y_btn << "," << m.select_btn << ","
      << m.start_btn << "," << m.a_btn << "," << m.x_btn << "," << m.l_btn
      << "," << m.r_btn << "," << m.l2_btn << "," << m.r2_btn << "\n"
      << "dpad=" << m.up_btn << "," << m.down_btn << "," << m.left_btn << ","
      << m.right_btn << "\n"
      << "ls=" << m.l_x_plus << "," << m.l_x_minus << "," << m.l_y_plus << ","
      << m.l_y_minus << "\n"
      << "rs_axis=" << m.r_x_plus << "," << m.r_x_minus << "," << m.r_y_plus
      << "," << m.r_y_minus << "\n"
      << "rs_btn=" << m.r_x_plus_btn << "," << m.r_x_minus_btn << ","
      << m.r_y_plus_btn << "," << m.r_y_minus_btn << "\n"
      << "dpad_axis=" << m.up_axis << "," << m.down_axis << "," << m.left_axis
      << "," << m.right_axis << "\n"
      << "hotkeys=" << m.enable_hotkey_btn << "," << m.menu_toggle_btn << ","
      << m.exit_emulator_btn << "\n"
      << "extra=" << m.extra_config;
    return o.str();
}
```

- [ ] **Step 2: Write a temporary golden-generator test**

`tests/retroarch/test_mapping_snapshot.cpp` (first version — printer):

```cpp
#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <vector>
#include "mapping_snapshot_util.h"
#include "retroarch/controller_detector.h"

using retroarch::ControllerType;
using retroarch::get_mapping;

static const std::vector<std::string> kSnapshotCores = {
    "nestopia_libretro",          "snes9x2010_libretro",
    "genesis_plus_gx_libretro",   "pcsx_rearmed_libretro",
    "mednafen_pce_fast_libretro", "prosystem_libretro",
    "fbneo_libretro",             "mupen64plus_next_libretro",
    "parallel_n64_libretro",      "flycast_libretro",
    "totally_unknown_core",  // guards the default-construct fallthrough
};

TEST_CASE("GENERATOR - print golden entries", "[mapping_snapshot_gen]") {
    auto dump = [](const char* pad, ControllerType t) {
        for (const auto& core : kSnapshotCores) {
            std::cout << "{\"" << pad << "|" << core << "\", R\"GOLD("
                      << serialize_mapping(get_mapping(t, core))
                      << ")GOLD\"},\n";
        }
    };
    dump("N64", ControllerType::N64_ADAPTER);
    dump("PS", ControllerType::PS_STYLE_DRAGONRISE);
    dump("UNKNOWN", ControllerType::UNKNOWN);
    SUCCEED();
}
```

- [ ] **Step 3: Build and capture the golden output**

```bash
cd magic_dingus_box_cpp && mkdir -p build-mac && cd build-mac && cmake .. >/dev/null && make test_retroarch_unit -j8 && ./test_retroarch_unit "[mapping_snapshot_gen]" > /tmp/golden_raw.txt; grep -A100000 '^{' /tmp/golden_raw.txt | grep -B100000 'GOLD"},$' > /tmp/golden_entries.txt && wc -l /tmp/golden_entries.txt
```

Expected: build succeeds; `/tmp/golden_entries.txt` contains 33 `{...}` entries' worth of lines (11 cores × 3 pads, multi-line raw strings).

- [ ] **Step 4: Create the golden header from the captured output**

`tests/retroarch/mapping_snapshot_golden.h`:

```cpp
#pragma once
#include <map>
#include <string>

// GENERATED from the pre-refactor get_mapping() (Task 1, 2026-07-28).
// DO NOT EDIT BY HAND unless a mapping change is deliberate and
// hardware-verified. Regenerate with the [mapping_snapshot_gen] test.
inline const std::map<std::string, std::string>& mapping_golden() {
    static const std::map<std::string, std::string> kMappingGolden = {
        // <PASTE the contents of /tmp/golden_entries.txt here verbatim>
    };
    return kMappingGolden;
}
```

- [ ] **Step 5: Replace the generator with the comparing test**

Rewrite `test_mapping_snapshot.cpp` (keep `kSnapshotCores`; the generator TEST_CASE stays but hidden behind Catch2's `[.]` tag so it can regenerate goldens on demand):

```cpp
#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <vector>
#include "mapping_snapshot_util.h"
#include "mapping_snapshot_golden.h"
#include "retroarch/controller_detector.h"

using retroarch::ControllerType;
using retroarch::get_mapping;

static const std::vector<std::string> kSnapshotCores = { /* same 11 as above */ };

static ControllerType pad_of(const std::string& tag) {
    if (tag == "N64") return ControllerType::N64_ADAPTER;
    if (tag == "PS") return ControllerType::PS_STYLE_DRAGONRISE;
    return ControllerType::UNKNOWN;
}

TEST_CASE("mapping output is bit-identical to the pre-refactor snapshot",
          "[mapping_snapshot]") {
    REQUIRE(mapping_golden().size() == kSnapshotCores.size() * 3);
    for (const auto& [key, golden] : mapping_golden()) {
        const auto bar = key.find('|');
        REQUIRE(bar != std::string::npos);
        INFO("snapshot key: " << key);
        REQUIRE(serialize_mapping(get_mapping(pad_of(key.substr(0, bar)),
                                              key.substr(bar + 1))) == golden);
    }
}

TEST_CASE("GENERATOR - print golden entries", "[.][mapping_snapshot_gen]") {
    /* unchanged generator body from Step 2 */
}
```

- [ ] **Step 6: Run and verify it passes against current code**

```bash
cd magic_dingus_box_cpp/build-mac && make test_retroarch_unit -j8 && ./test_retroarch_unit "[mapping_snapshot]"
```

Expected: `All tests passed` (33 comparisons).

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_util.h magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h magic_dingus_box_cpp/tests/retroarch/test_mapping_snapshot.cpp
git commit -m "test(retroarch): snapshot-lock get_mapping() output before refactor"
```

---

### Task 2: `logical_controls` module + built-in physical profiles

**Files:**
- Create: `magic_dingus_box_cpp/src/retroarch/logical_controls.h`, `.cpp`
- Create: `magic_dingus_box_cpp/src/retroarch/controller_profile.h`, `.cpp` (structs + builtins only; JSON/store come in Task 5)
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (add both `.cpp` to `RETROARCH_SOURCES` and to `test_retroarch_unit` sources)
- Test: `magic_dingus_box_cpp/tests/retroarch/test_logical_controls.cpp`, `tests/retroarch/test_controller_profile.cpp`

**Interfaces:**
- Produces:
  - `enum class ControllerStyle { PS_STYLE, N64_STYLE }`
  - `enum class LogicalControl { ... }` (exact list below)
  - `const char* logical_control_key(LogicalControl)`; `std::optional<LogicalControl> logical_control_from_key(const std::string&)`; `ControllerStyle style_of(LogicalControl)`; `std::vector<LogicalControl> capture_steps(ControllerStyle)`; `std::string control_prompt(LogicalControl)` (wizard display text)
  - `struct PhysicalBinding { enum class Kind { BUTTON, HAT, AXIS }; Kind kind; uint16_t code; int direction; std::string token; }`
  - `struct PhysicalProfile { std::string name; ControllerStyle style; uint16_t vid, pid; std::string captured_at; std::map<LogicalControl, PhysicalBinding> controls; bool has(LogicalControl) const; std::string token(LogicalControl) const; const PhysicalBinding* binding(LogicalControl) const; }`
  - `const PhysicalProfile& builtin_n64_adapter_profile()`; `const PhysicalProfile& builtin_dragonrise_profile()`
  - `std::string vidpid_key(uint16_t vid, uint16_t pid)` → `"0079:0006"` (4-hex lowercase, colon)

- [ ] **Step 1: Write failing tests**

`tests/retroarch/test_logical_controls.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "retroarch/logical_controls.h"

using namespace retroarch;

TEST_CASE("logical control keys round-trip", "[logical_controls]") {
    REQUIRE(std::string(logical_control_key(LogicalControl::CROSS)) == "cross");
    REQUIRE(std::string(logical_control_key(LogicalControl::N64_C_UP)) == "n64_c_up");
    REQUIRE(logical_control_from_key("cross") == LogicalControl::CROSS);
    REQUIRE(logical_control_from_key("n64_stick_left") == LogicalControl::N64_STICK_LEFT);
    REQUIRE(!logical_control_from_key("bogus").has_value());
}

TEST_CASE("style_of splits the vocabularies", "[logical_controls]") {
    REQUIRE(style_of(LogicalControl::TRIANGLE) == ControllerStyle::PS_STYLE);
    REQUIRE(style_of(LogicalControl::N64_Z) == ControllerStyle::N64_STYLE);
}

TEST_CASE("capture step lists are complete and style-pure", "[logical_controls]") {
    const auto ps = capture_steps(ControllerStyle::PS_STYLE);
    const auto n64 = capture_steps(ControllerStyle::N64_STYLE);
    REQUIRE(ps.size() == 24);
    REQUIRE(n64.size() == 18);
    for (auto c : ps) REQUIRE(style_of(c) == ControllerStyle::PS_STYLE);
    for (auto c : n64) REQUIRE(style_of(c) == ControllerStyle::N64_STYLE);
    // D-pad first: the most universal control anchors the flow.
    REQUIRE(ps.front() == LogicalControl::DPAD_UP);
    REQUIRE(n64.front() == LogicalControl::N64_DPAD_UP);
}

TEST_CASE("every control has a human prompt", "[logical_controls]") {
    for (auto style : {ControllerStyle::PS_STYLE, ControllerStyle::N64_STYLE})
        for (auto c : capture_steps(style))
            REQUIRE(!control_prompt(c).empty());
}
```

`tests/retroarch/test_controller_profile.cpp` (builtin-profile portion):

```cpp
#include <catch2/catch_test_macros.hpp>
#include "retroarch/controller_profile.h"

using namespace retroarch;

TEST_CASE("builtin N64 adapter profile reproduces the legacy tokens",
          "[controller_profile]") {
    const auto& p = builtin_n64_adapter_profile();
    REQUIRE(p.style == ControllerStyle::N64_STYLE);
    REQUIRE(p.vid == 0x0e6d); REQUIRE(p.pid == 0x111d);
    // Tokens transcribed from controller_mapping.cpp's physical table:
    // 0=C-Left, 1=B, 2=A, 3=C-Down, 4=L, 5=R, 6=Z, 8=C-Right, 9=C-Up, 12=Start
    REQUIRE(p.token(LogicalControl::N64_A) == "2");
    REQUIRE(p.token(LogicalControl::N64_B) == "1");
    REQUIRE(p.token(LogicalControl::N64_Z) == "6");
    REQUIRE(p.token(LogicalControl::N64_L) == "4");
    REQUIRE(p.token(LogicalControl::N64_R) == "5");
    REQUIRE(p.token(LogicalControl::N64_START) == "12");
    REQUIRE(p.token(LogicalControl::N64_C_LEFT) == "0");
    REQUIRE(p.token(LogicalControl::N64_C_DOWN) == "3");
    REQUIRE(p.token(LogicalControl::N64_C_RIGHT) == "8");
    REQUIRE(p.token(LogicalControl::N64_C_UP) == "9");
    REQUIRE(p.token(LogicalControl::N64_DPAD_UP) == "h0up");
    REQUIRE(p.token(LogicalControl::N64_STICK_UP) == "-1");
    REQUIRE(p.token(LogicalControl::N64_STICK_RIGHT) == "+0");
    // evdev codes for the kiosk-menu overlay (code = 304 + index on this pad)
    REQUIRE(p.binding(LogicalControl::N64_A)->code == 306);
    REQUIRE(p.binding(LogicalControl::N64_B)->code == 305);
    REQUIRE(p.binding(LogicalControl::N64_START)->code == 316);
    // Missing control -> empty token, null binding
    REQUIRE(p.token(LogicalControl::CROSS) == "");
    REQUIRE(p.binding(LogicalControl::CROSS) == nullptr);
}

TEST_CASE("builtin DragonRise profile reproduces the legacy tokens",
          "[controller_profile]") {
    const auto& p = builtin_dragonrise_profile();
    REQUIRE(p.style == ControllerStyle::PS_STYLE);
    // 0=Triangle 1=Circle 2=Cross 3=Square 4=L1 5=R1 6=L2 7=R2 8=Select 9=Start
    REQUIRE(p.token(LogicalControl::TRIANGLE) == "0");
    REQUIRE(p.token(LogicalControl::CIRCLE) == "1");
    REQUIRE(p.token(LogicalControl::CROSS) == "2");
    REQUIRE(p.token(LogicalControl::SQUARE) == "3");
    REQUIRE(p.token(LogicalControl::L1) == "4");
    REQUIRE(p.token(LogicalControl::R2) == "7");
    REQUIRE(p.token(LogicalControl::SELECT) == "8");
    REQUIRE(p.token(LogicalControl::START) == "9");
    REQUIRE(p.token(LogicalControl::DPAD_LEFT) == "h0left");
    REQUIRE(p.token(LogicalControl::LSTICK_DOWN) == "+1");
    // Legacy right-stick tokens are +2/+3 (see get_mapping_ps_style N64 branch)
    REQUIRE(p.token(LogicalControl::RSTICK_RIGHT) == "+2");
    REQUIRE(p.token(LogicalControl::RSTICK_DOWN) == "+3");
    // evdev codes: code = 288 + index (BTN_TRIGGER range, per input_manager.cpp)
    REQUIRE(p.binding(LogicalControl::CROSS)->code == 290);
    REQUIRE(p.binding(LogicalControl::START)->code == 297);
}

TEST_CASE("vidpid_key formats 4-hex lowercase", "[controller_profile]") {
    REQUIRE(vidpid_key(0x0079, 0x0006) == "0079:0006");
    REQUIRE(vidpid_key(0x0e6d, 0x111d) == "0e6d:111d");
}
```

- [ ] **Step 2: Run tests to verify they fail to compile**

```bash
cd magic_dingus_box_cpp/build-mac && cmake .. >/dev/null && make test_retroarch_unit -j8
```

Expected: FAIL — `retroarch/logical_controls.h: No such file or directory`.

- [ ] **Step 3: Implement `logical_controls.h`**

```cpp
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace retroarch {

enum class ControllerStyle { PS_STYLE, N64_STYLE };

// One value per physical control the wizard can ask about. The PS and N64
// vocabularies are deliberately distinct (no punning): an N64 pad's A is not
// "the same control" as a PS pad's Cross even if a core treats them alike.
enum class LogicalControl {
    // PS-style vocabulary
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    CROSS, CIRCLE, SQUARE, TRIANGLE,
    L1, R1, L2, R2, L3, R3,
    SELECT, START,
    LSTICK_UP, LSTICK_DOWN, LSTICK_LEFT, LSTICK_RIGHT,
    RSTICK_UP, RSTICK_DOWN, RSTICK_LEFT, RSTICK_RIGHT,
    // N64 vocabulary
    N64_A, N64_B, N64_START, N64_Z, N64_L, N64_R,
    N64_C_UP, N64_C_DOWN, N64_C_LEFT, N64_C_RIGHT,
    N64_DPAD_UP, N64_DPAD_DOWN, N64_DPAD_LEFT, N64_DPAD_RIGHT,
    N64_STICK_UP, N64_STICK_DOWN, N64_STICK_LEFT, N64_STICK_RIGHT,
};

// Stable snake_case key used in controller_profiles.json ("cross", "n64_a").
const char* logical_control_key(LogicalControl c);
std::optional<LogicalControl> logical_control_from_key(const std::string& key);

ControllerStyle style_of(LogicalControl c);

// The wizard's prompt order for a style. D-pad first, sticks last, so a
// minimal pad front-loads the controls it actually has.
std::vector<LogicalControl> capture_steps(ControllerStyle style);

// On-screen prompt, e.g. "Press CROSS (bottom face button)" /
// "Move the analog stick UP".
std::string control_prompt(LogicalControl c);

const char* controller_style_key(ControllerStyle s);  // "ps_style" / "n64_style"
std::optional<ControllerStyle> controller_style_from_key(const std::string& key);

}  // namespace retroarch
```

- [ ] **Step 4: Implement `logical_controls.cpp`**

```cpp
#include "logical_controls.h"

namespace retroarch {

namespace {
struct Entry { LogicalControl c; const char* key; const char* prompt; };
const Entry kEntries[] = {
    {LogicalControl::DPAD_UP, "dpad_up", "Press D-Pad UP"},
    {LogicalControl::DPAD_DOWN, "dpad_down", "Press D-Pad DOWN"},
    {LogicalControl::DPAD_LEFT, "dpad_left", "Press D-Pad LEFT"},
    {LogicalControl::DPAD_RIGHT, "dpad_right", "Press D-Pad RIGHT"},
    {LogicalControl::CROSS, "cross", "Press CROSS (bottom face button)"},
    {LogicalControl::CIRCLE, "circle", "Press CIRCLE (right face button)"},
    {LogicalControl::SQUARE, "square", "Press SQUARE (left face button)"},
    {LogicalControl::TRIANGLE, "triangle", "Press TRIANGLE (top face button)"},
    {LogicalControl::L1, "l1", "Press L1 (left shoulder)"},
    {LogicalControl::R1, "r1", "Press R1 (right shoulder)"},
    {LogicalControl::L2, "l2", "Press L2 (left trigger)"},
    {LogicalControl::R2, "r2", "Press R2 (right trigger)"},
    {LogicalControl::L3, "l3", "Click the LEFT stick (L3)"},
    {LogicalControl::R3, "r3", "Click the RIGHT stick (R3)"},
    {LogicalControl::SELECT, "select", "Press SELECT"},
    {LogicalControl::START, "start", "Press START"},
    {LogicalControl::LSTICK_UP, "lstick_up", "Move the LEFT stick UP"},
    {LogicalControl::LSTICK_DOWN, "lstick_down", "Move the LEFT stick DOWN"},
    {LogicalControl::LSTICK_LEFT, "lstick_left", "Move the LEFT stick LEFT"},
    {LogicalControl::LSTICK_RIGHT, "lstick_right", "Move the LEFT stick RIGHT"},
    {LogicalControl::RSTICK_UP, "rstick_up", "Move the RIGHT stick UP"},
    {LogicalControl::RSTICK_DOWN, "rstick_down", "Move the RIGHT stick DOWN"},
    {LogicalControl::RSTICK_LEFT, "rstick_left", "Move the RIGHT stick LEFT"},
    {LogicalControl::RSTICK_RIGHT, "rstick_right", "Move the RIGHT stick RIGHT"},
    {LogicalControl::N64_A, "n64_a", "Press A (big blue button)"},
    {LogicalControl::N64_B, "n64_b", "Press B (green button)"},
    {LogicalControl::N64_START, "n64_start", "Press START (center)"},
    {LogicalControl::N64_Z, "n64_z", "Press Z (underside trigger)"},
    {LogicalControl::N64_L, "n64_l", "Press L (left shoulder)"},
    {LogicalControl::N64_R, "n64_r", "Press R (right shoulder)"},
    {LogicalControl::N64_C_UP, "n64_c_up", "Press C-UP (yellow)"},
    {LogicalControl::N64_C_DOWN, "n64_c_down", "Press C-DOWN (yellow)"},
    {LogicalControl::N64_C_LEFT, "n64_c_left", "Press C-LEFT (yellow)"},
    {LogicalControl::N64_C_RIGHT, "n64_c_right", "Press C-RIGHT (yellow)"},
    {LogicalControl::N64_DPAD_UP, "n64_dpad_up", "Press D-Pad UP"},
    {LogicalControl::N64_DPAD_DOWN, "n64_dpad_down", "Press D-Pad DOWN"},
    {LogicalControl::N64_DPAD_LEFT, "n64_dpad_left", "Press D-Pad LEFT"},
    {LogicalControl::N64_DPAD_RIGHT, "n64_dpad_right", "Press D-Pad RIGHT"},
    {LogicalControl::N64_STICK_UP, "n64_stick_up", "Move the analog stick UP"},
    {LogicalControl::N64_STICK_DOWN, "n64_stick_down", "Move the analog stick DOWN"},
    {LogicalControl::N64_STICK_LEFT, "n64_stick_left", "Move the analog stick LEFT"},
    {LogicalControl::N64_STICK_RIGHT, "n64_stick_right", "Move the analog stick RIGHT"},
};
}  // namespace

const char* logical_control_key(LogicalControl c) {
    for (const auto& e : kEntries) if (e.c == c) return e.key;
    return "";
}

std::optional<LogicalControl> logical_control_from_key(const std::string& key) {
    for (const auto& e : kEntries) if (key == e.key) return e.c;
    return std::nullopt;
}

std::string control_prompt(LogicalControl c) {
    for (const auto& e : kEntries) if (e.c == c) return e.prompt;
    return "";
}

ControllerStyle style_of(LogicalControl c) {
    return c >= LogicalControl::N64_A ? ControllerStyle::N64_STYLE
                                      : ControllerStyle::PS_STYLE;
}

std::vector<LogicalControl> capture_steps(ControllerStyle style) {
    using L = LogicalControl;
    if (style == ControllerStyle::N64_STYLE) {
        return {L::N64_DPAD_UP, L::N64_DPAD_DOWN, L::N64_DPAD_LEFT,
                L::N64_DPAD_RIGHT, L::N64_A, L::N64_B, L::N64_START, L::N64_Z,
                L::N64_L, L::N64_R, L::N64_C_UP, L::N64_C_DOWN, L::N64_C_LEFT,
                L::N64_C_RIGHT, L::N64_STICK_UP, L::N64_STICK_DOWN,
                L::N64_STICK_LEFT, L::N64_STICK_RIGHT};
    }
    return {L::DPAD_UP, L::DPAD_DOWN, L::DPAD_LEFT, L::DPAD_RIGHT, L::CROSS,
            L::CIRCLE, L::SQUARE, L::TRIANGLE, L::L1, L::R1, L::L2, L::R2,
            L::SELECT, L::START, L::LSTICK_UP, L::LSTICK_DOWN, L::LSTICK_LEFT,
            L::LSTICK_RIGHT, L::RSTICK_UP, L::RSTICK_DOWN, L::RSTICK_LEFT,
            L::RSTICK_RIGHT, L::L3, L::R3};
}

const char* controller_style_key(ControllerStyle s) {
    return s == ControllerStyle::N64_STYLE ? "n64_style" : "ps_style";
}

std::optional<ControllerStyle> controller_style_from_key(const std::string& key) {
    if (key == "n64_style") return ControllerStyle::N64_STYLE;
    if (key == "ps_style") return ControllerStyle::PS_STYLE;
    return std::nullopt;
}

}  // namespace retroarch
```

NOTE: `style_of` relies on `N64_A` being the first N64 enumerator — keep the enum order.

- [ ] **Step 5: Implement `controller_profile.h` (structs + builtins only)**

```cpp
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include "logical_controls.h"

namespace retroarch {

struct PhysicalBinding {
    enum class Kind { BUTTON, HAT, AXIS };
    Kind kind = Kind::BUTTON;
    uint16_t code = 0;   // EV_KEY code for BUTTON; ABS_* code for HAT/AXIS
    int direction = 0;   // -1/+1 for HAT and AXIS; 0 for BUTTON
    std::string token;   // RetroArch udev bind token: "5", "h0up", "+2", "-3"
};

struct PhysicalProfile {
    std::string name;
    ControllerStyle style = ControllerStyle::PS_STYLE;
    uint16_t vid = 0, pid = 0;
    std::string captured_at;  // ISO-8601, informational only
    std::map<LogicalControl, PhysicalBinding> controls;

    bool has(LogicalControl c) const { return controls.count(c) != 0; }
    std::string token(LogicalControl c) const {
        auto it = controls.find(c);
        return it == controls.end() ? std::string() : it->second.token;
    }
    const PhysicalBinding* binding(LogicalControl c) const {
        auto it = controls.find(c);
        return it == controls.end() ? nullptr : &it->second;
    }
};

// Built-in profiles for the two shipped pads. Token values transcribed 1:1
// from the legacy physical tables in controller_mapping.cpp; evdev codes
// from input_manager.cpp's map_button_to_action comments.
const PhysicalProfile& builtin_n64_adapter_profile();
const PhysicalProfile& builtin_dragonrise_profile();

std::string vidpid_key(uint16_t vid, uint16_t pid);  // "0079:0006"

}  // namespace retroarch
```

- [ ] **Step 6: Implement `controller_profile.cpp` (builtins)**

```cpp
#include "controller_profile.h"

#include <cstdio>
#include <linux/input-event-codes.h>  // only for the code constants; see below

namespace retroarch {

// NOTE ON PORTABILITY: this file must build on macOS for the unit tests.
// linux/input-event-codes.h does not exist there, so define the handful of
// codes we need when the header is absent.
#ifndef BTN_SOUTH
#define BTN_SOUTH 0x130
#endif
#ifndef ABS_X
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02
#define ABS_RZ 0x05
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11
#endif

namespace {
using L = LogicalControl;
using K = PhysicalBinding::Kind;

PhysicalBinding btn(uint16_t code, const char* tok) { return {K::BUTTON, code, 0, tok}; }
PhysicalBinding hat(uint16_t code, int dir, const char* tok) { return {K::HAT, code, dir, tok}; }
PhysicalBinding axis(uint16_t code, int dir, const char* tok) { return {K::AXIS, code, dir, tok}; }
}  // namespace

const PhysicalProfile& builtin_n64_adapter_profile() {
    static const PhysicalProfile p = [] {
        PhysicalProfile p;
        p.name = "SWITCH CO.,LTD. Controller (N64 adapter)";
        p.style = ControllerStyle::N64_STYLE;
        p.vid = 0x0e6d; p.pid = 0x111d;
        // Joystick index i lives at evdev code 304+i on this adapter
        // (contiguous BTN_GAMEPAD range; indices verified via evtest, see
        // controller_mapping.cpp's physical table).
        p.controls = {
            {L::N64_C_LEFT, btn(304, "0")},  {L::N64_B, btn(305, "1")},
            {L::N64_A, btn(306, "2")},       {L::N64_C_DOWN, btn(307, "3")},
            {L::N64_L, btn(308, "4")},       {L::N64_R, btn(309, "5")},
            {L::N64_Z, btn(310, "6")},       {L::N64_C_RIGHT, btn(312, "8")},
            {L::N64_C_UP, btn(313, "9")},    {L::N64_START, btn(316, "12")},
            {L::N64_DPAD_UP, hat(ABS_HAT0Y, -1, "h0up")},
            {L::N64_DPAD_DOWN, hat(ABS_HAT0Y, +1, "h0down")},
            {L::N64_DPAD_LEFT, hat(ABS_HAT0X, -1, "h0left")},
            {L::N64_DPAD_RIGHT, hat(ABS_HAT0X, +1, "h0right")},
            {L::N64_STICK_UP, axis(ABS_Y, -1, "-1")},
            {L::N64_STICK_DOWN, axis(ABS_Y, +1, "+1")},
            {L::N64_STICK_LEFT, axis(ABS_X, -1, "-0")},
            {L::N64_STICK_RIGHT, axis(ABS_X, +1, "+0")},
        };
        return p;
    }();
    return p;
}

const PhysicalProfile& builtin_dragonrise_profile() {
    static const PhysicalProfile p = [] {
        PhysicalProfile p;
        p.name = "DragonRise Generic USB Joystick";
        p.style = ControllerStyle::PS_STYLE;
        p.vid = 0x0079; p.pid = 0x0006;
        // Joystick index i lives at evdev code 288+i (BTN_TRIGGER range,
        // see input_manager.cpp map_button_to_action comment block).
        // Right-stick axis tokens are the LEGACY +2/+3 (what shipped code
        // emits); the on-Pi probe (Task 12) re-verifies against hardware.
        p.controls = {
            {L::TRIANGLE, btn(288, "0")}, {L::CIRCLE, btn(289, "1")},
            {L::CROSS, btn(290, "2")},    {L::SQUARE, btn(291, "3")},
            {L::L1, btn(292, "4")},       {L::R1, btn(293, "5")},
            {L::L2, btn(294, "6")},       {L::R2, btn(295, "7")},
            {L::SELECT, btn(296, "8")},   {L::START, btn(297, "9")},
            {L::DPAD_UP, hat(ABS_HAT0Y, -1, "h0up")},
            {L::DPAD_DOWN, hat(ABS_HAT0Y, +1, "h0down")},
            {L::DPAD_LEFT, hat(ABS_HAT0X, -1, "h0left")},
            {L::DPAD_RIGHT, hat(ABS_HAT0X, +1, "h0right")},
            {L::LSTICK_UP, axis(ABS_Y, -1, "-1")},
            {L::LSTICK_DOWN, axis(ABS_Y, +1, "+1")},
            {L::LSTICK_LEFT, axis(ABS_X, -1, "-0")},
            {L::LSTICK_RIGHT, axis(ABS_X, +1, "+0")},
            {L::RSTICK_UP, axis(ABS_RZ, -1, "-3")},
            {L::RSTICK_DOWN, axis(ABS_RZ, +1, "+3")},
            {L::RSTICK_LEFT, axis(ABS_Z, -1, "-2")},
            {L::RSTICK_RIGHT, axis(ABS_Z, +1, "+2")},
        };
        return p;
    }();
    return p;
}

std::string vidpid_key(uint16_t vid, uint16_t pid) {
    char buf[10];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", vid, pid);
    return buf;
}

}  // namespace retroarch
```

IMPORTANT: the `#include <linux/input-event-codes.h>` must be wrapped for Mac:

```cpp
#ifdef __linux__
#include <linux/input-event-codes.h>
#endif
```

(keep the `#ifndef` fallback defines either way).

- [ ] **Step 7: Wire into CMake**

In `CMakeLists.txt`:
- `RETROARCH_SOURCES` (~line 144): add `src/retroarch/logical_controls.cpp` and `src/retroarch/controller_profile.cpp`.
- `test_retroarch_unit` sources (~line 462): add the same two files.

- [ ] **Step 8: Build, run, verify pass**

```bash
cd magic_dingus_box_cpp/build-mac && cmake .. >/dev/null && make test_retroarch_unit -j8 && ./test_retroarch_unit "[logical_controls],[controller_profile]"
```

Expected: PASS. Also run the full suite (`./test_retroarch_unit`) — snapshot still green.

- [ ] **Step 9: Commit**

```bash
git add -A magic_dingus_box_cpp/src/retroarch magic_dingus_box_cpp/tests/retroarch magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(retroarch): logical control vocabulary + builtin physical profiles"
```

---

### Task 3: Semantic tables + `build_mapping()` + `get_mapping()` refactor

The core refactor. `controller_mapping.cpp`'s two 250-line index-literal helpers become semantic tables; `get_mapping()` becomes a wrapper over `build_mapping(semantic, builtin_profile)`. The Task 1 snapshot is the acceptance test — no golden value may change.

**Files:**
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_mapping.h`
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp`
- Test: existing `tests/retroarch/test_mapping_snapshot.cpp` + `test_controller_mapping.cpp` (unchanged, must stay green); new cases in `tests/retroarch/test_controller_profile.cpp`

**Interfaces:**
- Consumes: `LogicalControl`, `ControllerStyle`, `PhysicalProfile`, `builtin_*_profile()` (Task 2).
- Produces:
  - `struct SemanticMapping` (fields below)
  - `SemanticMapping get_semantic_mapping(ControllerStyle style, const std::string& core_name)`
  - `ControllerMapping build_mapping(const SemanticMapping& sem, const PhysicalProfile& profile)`
  - `get_mapping(ControllerType, core)` signature UNCHANGED (now a wrapper).

- [ ] **Step 1: Add the new declarations to `controller_mapping.h`**

After the `ControllerMapping` struct, before `get_mapping`:

```cpp
#include <optional>
#include "controller_profile.h"

// A per-core mapping expressed in LOGICAL controls instead of physical
// button numbers. get_semantic_mapping() owns the per-core decisions
// (which control drives which RetroPad slot); build_mapping() marries a
// SemanticMapping to a PhysicalProfile to produce the ControllerMapping
// the launcher emits. Slots left nullopt keep ControllerMapping's struct
// defaults — several legacy branches rely on exactly that.
struct SemanticMapping {
    std::string name = "Default";
    std::string analog_dpad_mode = "1";
    std::string core_option_pad_type = "";
    std::string extra_config = "";

    // RetroPad button slots
    std::optional<LogicalControl> b, y, select, start, a, x, l, r, l2, r2;
    // RetroPad digital d-pad slots
    std::optional<LogicalControl> up, down, left, right;
    // Main analog stick controls (LSTICK_* or N64_STICK_*)
    std::optional<LogicalControl> stick_up, stick_down, stick_left, stick_right;
    bool left_stick = false;    // emit l_x/l_y axis binds from stick_*
    bool stick_to_dpad = false; // emit up/down/left/right_axis from stick_*
    // RetroPad right-stick slots (RSTICK_* or N64_C_*); presence gates emission
    std::optional<LogicalControl> r_up, r_down, r_left, r_right;
    // Hotkeys
    std::optional<LogicalControl> hotkey_enable, menu_toggle, exit_emulator;
};

SemanticMapping get_semantic_mapping(ControllerStyle style,
                                     const std::string& core_name);
ControllerMapping build_mapping(const SemanticMapping& sem,
                                const PhysicalProfile& profile);
```

- [ ] **Step 2: Write failing build_mapping unit tests** (append to `test_controller_profile.cpp`)

```cpp
#include "retroarch/controller_mapping.h"

TEST_CASE("build_mapping resolves slots through the profile", "[build_mapping]") {
    SemanticMapping sem;
    sem.name = "T"; sem.analog_dpad_mode = "0";
    sem.b = LogicalControl::CROSS;
    sem.r_up = LogicalControl::RSTICK_UP; sem.r_down = LogicalControl::RSTICK_DOWN;
    sem.r_left = LogicalControl::RSTICK_LEFT; sem.r_right = LogicalControl::RSTICK_RIGHT;
    const auto m = build_mapping(sem, builtin_dragonrise_profile());
    REQUIRE(m.b_btn == "2");            // CROSS token
    REQUIRE(m.y_btn == "3");            // slot absent -> struct default kept
    REQUIRE(m.r_x_plus == "+2");        // AXIS kind -> axis form
    REQUIRE(m.r_x_plus_btn.empty());
}

TEST_CASE("build_mapping uses button form for a digital C cluster", "[build_mapping]") {
    SemanticMapping sem;
    sem.r_up = LogicalControl::N64_C_UP; sem.r_down = LogicalControl::N64_C_DOWN;
    sem.r_left = LogicalControl::N64_C_LEFT; sem.r_right = LogicalControl::N64_C_RIGHT;
    const auto m = build_mapping(sem, builtin_n64_adapter_profile());
    REQUIRE(m.r_y_minus_btn == "9");    // C-Up, BUTTON kind -> _btn form
    REQUIRE(m.r_x_plus.empty());
}

TEST_CASE("build_mapping unbinds slots the profile lacks", "[build_mapping]") {
    SemanticMapping sem;
    sem.l2 = LogicalControl::L2;        // DragonRise has it; a stickless capture may not
    PhysicalProfile p = builtin_dragonrise_profile();
    p.controls.erase(LogicalControl::L2);
    REQUIRE(build_mapping(sem, p).l2_btn.empty());   // "" = unbound, not default
}
```

Run: `make test_retroarch_unit -j8` → FAIL (no `build_mapping` symbol).

- [ ] **Step 3: Implement `build_mapping()` in `controller_mapping.cpp`**

```cpp
ControllerMapping build_mapping(const SemanticMapping& sem,
                                const PhysicalProfile& profile) {
    ControllerMapping m;  // struct defaults are load-bearing — see header
    m.name = sem.name;
    m.analog_dpad_mode = sem.analog_dpad_mode;
    m.core_option_pad_type = sem.core_option_pad_type;
    m.extra_config = sem.extra_config;

    auto put = [&](std::string ControllerMapping::*field,
                   const std::optional<LogicalControl>& slot) {
        if (slot) m.*field = profile.token(*slot);
    };
    put(&ControllerMapping::b_btn, sem.b);
    put(&ControllerMapping::y_btn, sem.y);
    put(&ControllerMapping::select_btn, sem.select);
    put(&ControllerMapping::start_btn, sem.start);
    put(&ControllerMapping::a_btn, sem.a);
    put(&ControllerMapping::x_btn, sem.x);
    put(&ControllerMapping::l_btn, sem.l);
    put(&ControllerMapping::r_btn, sem.r);
    put(&ControllerMapping::l2_btn, sem.l2);
    put(&ControllerMapping::r2_btn, sem.r2);
    put(&ControllerMapping::up_btn, sem.up);
    put(&ControllerMapping::down_btn, sem.down);
    put(&ControllerMapping::left_btn, sem.left);
    put(&ControllerMapping::right_btn, sem.right);

    if (sem.left_stick) {
        m.l_x_plus  = sem.stick_right ? profile.token(*sem.stick_right) : "";
        m.l_x_minus = sem.stick_left  ? profile.token(*sem.stick_left)  : "";
        m.l_y_plus  = sem.stick_down  ? profile.token(*sem.stick_down)  : "";
        m.l_y_minus = sem.stick_up    ? profile.token(*sem.stick_up)    : "";
    }
    if (sem.stick_to_dpad) {
        m.right_axis = sem.stick_right ? profile.token(*sem.stick_right) : "";
        m.left_axis  = sem.stick_left  ? profile.token(*sem.stick_left)  : "";
        m.down_axis  = sem.stick_down  ? profile.token(*sem.stick_down)  : "";
        m.up_axis    = sem.stick_up    ? profile.token(*sem.stick_up)    : "";
    }

    // Right stick / C cluster: axis vs button form follows the PROFILE's
    // binding kind (a real stick binds axes; a digital C cluster binds
    // buttons). Mirrors the legacy write_right_stick_binds contract.
    if (sem.r_up && sem.r_down && sem.r_left && sem.r_right) {
        const auto* up = profile.binding(*sem.r_up);
        if (up && up->kind == PhysicalBinding::Kind::AXIS) {
            m.r_x_plus  = profile.token(*sem.r_right);
            m.r_x_minus = profile.token(*sem.r_left);
            m.r_y_plus  = profile.token(*sem.r_down);
            m.r_y_minus = profile.token(*sem.r_up);
        } else if (up) {
            m.r_x_plus_btn  = profile.token(*sem.r_right);
            m.r_x_minus_btn = profile.token(*sem.r_left);
            m.r_y_plus_btn  = profile.token(*sem.r_down);
            m.r_y_minus_btn = profile.token(*sem.r_up);
        }
        // up == nullptr (skipped in wizard): emit neither form.
    }

    if (sem.hotkey_enable) m.enable_hotkey_btn = profile.token(*sem.hotkey_enable);
    if (sem.menu_toggle)   m.menu_toggle_btn   = profile.token(*sem.menu_toggle);
    if (sem.exit_emulator) m.exit_emulator_btn = profile.token(*sem.exit_emulator);
    return m;
}
```

- [ ] **Step 4: Run build_mapping tests** → PASS. Commit `feat(retroarch): build_mapping combiner`.

- [ ] **Step 5: Transcribe the N64-style semantic tables**

In `controller_mapping.cpp`, add (anon namespace) `semantic_n64_style(const std::string& core_name)`. Transcription is 1:1 from `get_mapping_n64_adapter` — each physical index literal becomes the logical control that OWNS that index in the builtin profile. Full function:

```cpp
SemanticMapping semantic_n64_style(const std::string& core) {
    using L = LogicalControl;
    SemanticMapping s;
    auto stick = [&] {
        s.stick_up = L::N64_STICK_UP; s.stick_down = L::N64_STICK_DOWN;
        s.stick_left = L::N64_STICK_LEFT; s.stick_right = L::N64_STICK_RIGHT;
    };
    auto hotkeys = [&] { s.hotkey_enable = L::N64_Z; s.menu_toggle = L::N64_START; };
    auto dpad = [&] {
        s.up = L::N64_DPAD_UP; s.down = L::N64_DPAD_DOWN;
        s.left = L::N64_DPAD_LEFT; s.right = L::N64_DPAD_RIGHT;
    };
    if (core.find("nestopia") != std::string::npos || core.find("fceumm") != std::string::npos) {
        s.name = "NES (N64 Controller)"; s.analog_dpad_mode = "0";
        s.b = L::N64_B; s.a = L::N64_A;
        s.select = L::N64_C_UP; s.start = L::N64_START;
        s.x = L::N64_C_DOWN; s.y = L::N64_C_LEFT;
        stick(); s.stick_to_dpad = true; hotkeys();
        s.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                         "nestopia_audio_vol_sq2 = \"100\"\n"
                         "nestopia_audio_vol_tri = \"100\"\n"
                         "nestopia_audio_vol_noise = \"100\"\n"
                         "nestopia_audio_vol_dpcm = \"100\"\n";
    } else if (core.find("pcsx") != std::string::npos || core.find("beetle_psx") != std::string::npos || core.find("swanstation") != std::string::npos) {
        s.name = "PS1 (N64 Controller)"; s.core_option_pad_type = "analog"; s.analog_dpad_mode = "0";
        s.b = L::N64_A; s.a = L::N64_B; s.y = L::N64_C_DOWN; s.x = L::N64_C_LEFT;
        s.start = L::N64_START; s.select = L::N64_C_UP;
        s.l = L::N64_L; s.r = L::N64_R; s.r2 = L::N64_C_RIGHT;
        stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();
    } else if (core.find("prosystem") != std::string::npos) {
        s.name = "Atari 7800"; s.analog_dpad_mode = "0";
        s.b = L::N64_B; s.a = L::N64_A; s.start = L::N64_START;
        // legacy sets select_btn="10" explicitly — that IS the struct
        // default, and physical button 10 is unused on this pad, so the
        // slot stays nullopt and the default carries it. Same for the
        // other branches below that "set" a field to its default.
        dpad(); stick(); s.stick_to_dpad = true; hotkeys();
    } else if (core.find("genesis_plus_gx") != std::string::npos) {
        s.name = "Sega Genesis"; s.analog_dpad_mode = "0";
        s.a = L::N64_A; s.b = L::N64_B; s.y = L::N64_C_DOWN; s.start = L::N64_START;
        dpad(); stick(); s.stick_to_dpad = true; hotkeys();
    } else if (core.find("snes9x") != std::string::npos) {
        s.name = "Super Nintendo"; s.analog_dpad_mode = "0";
        s.b = L::N64_B; s.a = L::N64_A; s.y = L::N64_C_DOWN; s.x = L::N64_C_LEFT;
        s.l = L::N64_L; s.r = L::N64_R; s.start = L::N64_START;
        dpad(); stick(); s.stick_to_dpad = true; hotkeys();
    } else if (core.find("mednafen_pce_fast") != std::string::npos) {
        s.name = "PC Engine / TurboGrafx-16"; s.analog_dpad_mode = "0";
        s.b = L::N64_B; s.a = L::N64_A; s.start = L::N64_START;
        s.y = L::N64_C_LEFT; s.x = L::N64_C_DOWN;
        stick(); s.stick_to_dpad = true; hotkeys();
    } else if (core.find("fbneo") != std::string::npos) {
        s.name = "Arcade (FinalBurn Neo)"; s.analog_dpad_mode = "0";
        s.y = L::N64_C_LEFT; s.x = L::N64_C_DOWN; s.l = L::N64_L;
        s.b = L::N64_B; s.a = L::N64_A; s.r = L::N64_R;
        s.select = L::N64_C_UP; s.start = L::N64_START;
        stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();
    } else if (core.find("mupen64plus") != std::string::npos || core.find("parallel_n64") != std::string::npos) {
        s.name = "Nintendo 64 (N64 pad)"; s.analog_dpad_mode = "0";
        s.b = L::N64_A; s.a = L::N64_B; s.l = L::N64_L; s.r = L::N64_R;
        s.l2 = L::N64_Z; s.start = L::N64_START;
        stick(); s.left_stick = true;   // NO stick_to_dpad — d-pad must not double
        s.r_up = L::N64_C_UP; s.r_down = L::N64_C_DOWN;
        s.r_left = L::N64_C_LEFT; s.r_right = L::N64_C_RIGHT;
        dpad(); hotkeys();
    } else if (core.find("flycast") != std::string::npos) {
        s.name = "Dreamcast (N64 pad)"; s.analog_dpad_mode = "0";
        s.b = L::N64_A; s.a = L::N64_B; s.y = L::N64_C_LEFT; s.x = L::N64_C_DOWN;
        s.l2 = L::N64_L; s.r2 = L::N64_R; s.start = L::N64_START;
        stick(); s.left_stick = true; dpad(); hotkeys();
    }
    return s;
}
```

CAREFUL: the mupen/flycast branches must NOT set `stick_to_dpad` and must leave `up_axis` etc. at the ControllerMapping default `""` — the legacy code explicitly clears them; the semantic version achieves the same by never setting the flag. `build_mapping` only touches those fields when the flag is true, and the struct default is already `""`. ✓

- [ ] **Step 6: Transcribe the PS-style semantic tables**

```cpp
SemanticMapping semantic_ps_style(const std::string& core) {
    using L = LogicalControl;
    SemanticMapping s;
    s.analog_dpad_mode = "0";
    s.hotkey_enable = L::SELECT; s.menu_toggle = L::START;
    s.stick_up = L::LSTICK_UP; s.stick_down = L::LSTICK_DOWN;
    s.stick_left = L::LSTICK_LEFT; s.stick_right = L::LSTICK_RIGHT;
    s.left_stick = true; s.stick_to_dpad = true;   // preamble defaults

    if (core.find("nestopia") != std::string::npos || core.find("fceumm") != std::string::npos) {
        s.name = "NES (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.select = L::SELECT; s.start = L::START;
        s.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                         "nestopia_audio_vol_sq2 = \"100\"\n"
                         "nestopia_audio_vol_tri = \"100\"\n"
                         "nestopia_audio_vol_noise = \"100\"\n"
                         "nestopia_audio_vol_dpcm = \"100\"\n";
    } else if (core.find("snes9x") != std::string::npos) {
        s.name = "Super Nintendo (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.l = L::L1; s.r = L::R1; s.select = L::SELECT; s.start = L::START;
    } else if (core.find("genesis_plus_gx") != std::string::npos) {
        s.name = "Sega Genesis (PS-style)";
        s.y = L::SQUARE; s.b = L::CROSS; s.a = L::CIRCLE; s.x = L::TRIANGLE;
        s.l = L::L1; s.r = L::R1; s.start = L::START;
    } else if (core.find("pcsx") != std::string::npos || core.find("beetle_psx") != std::string::npos || core.find("swanstation") != std::string::npos) {
        s.name = "PS1 (PS-style, 1:1)"; s.core_option_pad_type = "analog";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.l = L::L1; s.r = L::R1; s.l2 = L::L2; s.r2 = L::R2;
        s.select = L::SELECT; s.start = L::START;
    } else if (core.find("prosystem") != std::string::npos) {
        s.name = "Atari 7800 (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.select = L::SELECT; s.start = L::START;
    } else if (core.find("mednafen_pce_fast") != std::string::npos) {
        s.name = "PC Engine (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.select = L::SELECT; s.start = L::START;
    } else if (core.find("fbneo") != std::string::npos) {
        s.name = "Arcade / FBNeo (PS-style)";
        s.y = L::SQUARE; s.x = L::TRIANGLE; s.l = L::L1;
        s.b = L::CROSS; s.a = L::CIRCLE; s.r = L::R1;
        s.select = L::SELECT; s.start = L::START;
    } else if (core.find("mupen64plus") != std::string::npos || core.find("parallel_n64") != std::string::npos) {
        s.name = "Nintendo 64 (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.l = L::L1; s.r = L::R1;
        s.l2 = L::L2; s.start = L::START;
        s.stick_to_dpad = false;   // legacy clears the *_axis dpad binds
        s.r_up = L::RSTICK_UP; s.r_down = L::RSTICK_DOWN;
        s.r_left = L::RSTICK_LEFT; s.r_right = L::RSTICK_RIGHT;
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;
    } else if (core.find("flycast") != std::string::npos) {
        s.name = "Dreamcast (PS-style)";
        s.b = L::CROSS; s.a = L::CIRCLE; s.y = L::SQUARE; s.x = L::TRIANGLE;
        s.l2 = L::L2; s.r2 = L::R2; s.start = L::START;
        // NOTE: legacy flycast PS branch KEEPS the preamble's
        // stick-to-dpad binds (it never clears up_axis) — do not clear
        // stick_to_dpad here. Quirk preserved by the snapshot.
        s.up = L::DPAD_UP; s.down = L::DPAD_DOWN;
        s.left = L::DPAD_LEFT; s.right = L::DPAD_RIGHT;
    } else {
        // Unknown core: legacy returns the preamble-only mapping (name
        // "Default" with PS hotkeys/sticks applied). Keep identical.
    }
    return s;
}

SemanticMapping get_semantic_mapping(ControllerStyle style, const std::string& core_name) {
    return style == ControllerStyle::N64_STYLE ? semantic_n64_style(core_name)
                                               : semantic_ps_style(core_name);
}
```

TRAP — the unknown-core PS case: legacy `get_mapping_ps_style` falls through with the preamble applied, so `name=="Default"` but hotkeys/sticks are set. The N64 unknown-core case returns pure struct defaults. The snapshot's `totally_unknown_core` entries encode both exactly — trust them.

- [ ] **Step 7: Rewrite `get_mapping()` as the wrapper; delete the legacy helpers**

```cpp
ControllerMapping get_mapping(ControllerType type, const std::string& core_name) {
    switch (type) {
        case ControllerType::PS_STYLE_DRAGONRISE:
            return build_mapping(get_semantic_mapping(ControllerStyle::PS_STYLE, core_name),
                                 builtin_dragonrise_profile());
        case ControllerType::N64_ADAPTER:
        case ControllerType::UNKNOWN:
        default:
            return build_mapping(get_semantic_mapping(ControllerStyle::N64_STYLE, core_name),
                                 builtin_n64_adapter_profile());
    }
}
```

Delete `get_mapping_n64_adapter` and `get_mapping_ps_style` entirely. Keep `write_right_stick_binds` untouched.

- [ ] **Step 8: Run the FULL suite — the snapshot is the verdict**

```bash
cd magic_dingus_box_cpp/build-mac && make test_retroarch_unit -j8 && ./test_retroarch_unit
```

Expected: ALL PASS, especially `[mapping_snapshot]` (33/33) and the legacy `test_controller_mapping.cpp` cases. If a snapshot entry fails, the diff in the assertion message tells you exactly which field of which core×pad diverged — fix the semantic table, never the golden.

- [ ] **Step 9: Commit**

```bash
git add magic_dingus_box_cpp/src/retroarch/controller_mapping.h magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp magic_dingus_box_cpp/tests/retroarch/test_controller_profile.cpp
git commit -m "refactor(retroarch): controller tables split into semantic + physical layers"
```

---

### Task 4: `joydev_index` — evdev code → RetroArch udev bind token

**Files:**
- Create: `magic_dingus_box_cpp/src/retroarch/joydev_index.h`, `.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (RETROARCH_SOURCES + test_retroarch_unit)
- Test: `magic_dingus_box_cpp/tests/retroarch/test_joydev_index.cpp`

**Interfaces:**
- Produces:
  - `int button_index(const std::vector<uint16_t>& key_codes, uint16_t code)` — joydev ordering: codes in `[0x120, KEY_MAX]` ascending first, then `[0x100, 0x120)` ascending; -1 if absent.
  - `int axis_index(const std::vector<uint16_t>& abs_codes, uint16_t code)` — ascending ABS order EXCLUDING the hat range `0x10..0x17`; -1 if absent or a hat.
  - `int hat_number(uint16_t abs_code)` — `ABS_HAT0X/Y`→0 … `ABS_HAT3X/Y`→3; -1 otherwise.
  - `std::string bind_token(const std::vector<uint16_t>& key_codes, const std::vector<uint16_t>& abs_codes, PhysicalBinding::Kind kind, uint16_t code, int direction)` — "" when unresolvable.

- [ ] **Step 1: Write failing tests**

`tests/retroarch/test_joydev_index.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "retroarch/joydev_index.h"
#include "retroarch/controller_profile.h"

using namespace retroarch;
using K = PhysicalBinding::Kind;

TEST_CASE("button_index follows joydev two-range ordering", "[joydev_index]") {
    // A pad with codes both above and below BTN_JOYSTICK (0x120=288):
    // 0x120-range first (ascending), then the 0x100 BTN_MISC range wraps.
    const std::vector<uint16_t> keys = {0x100, 0x101, 0x130, 0x131, 0x13b};
    REQUIRE(button_index(keys, 0x130) == 0);
    REQUIRE(button_index(keys, 0x131) == 1);
    REQUIRE(button_index(keys, 0x13b) == 2);
    REQUIRE(button_index(keys, 0x100) == 3);   // BTN_MISC wraps AFTER
    REQUIRE(button_index(keys, 0x101) == 4);
    REQUIRE(button_index(keys, 0x999) == -1);
}

TEST_CASE("DragonRise-shaped pad gets contiguous indices", "[joydev_index]") {
    std::vector<uint16_t> keys;
    for (uint16_t c = 288; c <= 299; ++c) keys.push_back(c);
    REQUIRE(button_index(keys, 288) == 0);   // Triangle
    REQUIRE(button_index(keys, 290) == 2);   // Cross
    REQUIRE(button_index(keys, 297) == 9);   // Start
}

TEST_CASE("axis_index skips hats", "[joydev_index]") {
    // ABS_X(0), ABS_Y(1), ABS_Z(2), ABS_RZ(5), ABS_HAT0X(16), ABS_HAT0Y(17)
    const std::vector<uint16_t> abs = {0, 1, 2, 5, 16, 17};
    REQUIRE(axis_index(abs, 0) == 0);
    REQUIRE(axis_index(abs, 2) == 2);
    REQUIRE(axis_index(abs, 5) == 3);
    REQUIRE(axis_index(abs, 16) == -1);      // hat is not an axis
    REQUIRE(hat_number(16) == 0);
    REQUIRE(hat_number(17) == 0);
    REQUIRE(hat_number(0x14) == 2);          // ABS_HAT2X
    REQUIRE(hat_number(1) == -1);
}

TEST_CASE("bind_token formats all three kinds", "[joydev_index]") {
    std::vector<uint16_t> keys; for (uint16_t c = 288; c <= 299; ++c) keys.push_back(c);
    const std::vector<uint16_t> abs = {0, 1, 2, 5, 16, 17};
    REQUIRE(bind_token(keys, abs, K::BUTTON, 290, 0) == "2");
    REQUIRE(bind_token(keys, abs, K::HAT, 17, -1) == "h0up");
    REQUIRE(bind_token(keys, abs, K::HAT, 16, +1) == "h0right");
    REQUIRE(bind_token(keys, abs, K::AXIS, 5, +1) == "+3");
    REQUIRE(bind_token(keys, abs, K::AXIS, 1, -1) == "-1");
    REQUIRE(bind_token(keys, abs, K::BUTTON, 999, 0) == "");
}
```

- [ ] **Step 2: Run to verify FAIL** (missing header).

- [ ] **Step 3: Implement**

`joydev_index.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "controller_profile.h"

namespace retroarch {

// RetroArch's udev joypad driver numbers buttons the same way the kernel
// joystick API does: EV_KEY codes in [BTN_JOYSTICK(0x120), KEY_MAX] get
// indices first in ascending code order, then codes in
// [BTN_MISC(0x100), BTN_JOYSTICK) wrap after them. Axes are the device's
// non-hat ABS codes in ascending order; ABS_HAT0X..ABS_HAT3Y become hats.
// VERIFY ON HARDWARE before trusting (Task 12's controller_probe): the
// builtin-profile tokens are the ground truth this must reproduce.
int button_index(const std::vector<uint16_t>& key_codes, uint16_t code);
int axis_index(const std::vector<uint16_t>& abs_codes, uint16_t code);
int hat_number(uint16_t abs_code);

std::string bind_token(const std::vector<uint16_t>& key_codes,
                       const std::vector<uint16_t>& abs_codes,
                       PhysicalBinding::Kind kind, uint16_t code,
                       int direction);

}  // namespace retroarch
```

`joydev_index.cpp`:

```cpp
#include "joydev_index.h"
#include <algorithm>

namespace retroarch {

namespace {
constexpr uint16_t kBtnMisc = 0x100;
constexpr uint16_t kBtnJoystick = 0x120;
constexpr uint16_t kHatFirst = 0x10;  // ABS_HAT0X
constexpr uint16_t kHatLast = 0x17;   // ABS_HAT3Y
}  // namespace

int button_index(const std::vector<uint16_t>& key_codes, uint16_t code) {
    std::vector<uint16_t> sorted = key_codes;
    std::sort(sorted.begin(), sorted.end());
    int idx = 0;
    for (uint16_t c : sorted)                       // high range first
        if (c >= kBtnJoystick) { if (c == code) return idx; ++idx; }
    for (uint16_t c : sorted)                       // BTN_MISC wraps after
        if (c >= kBtnMisc && c < kBtnJoystick) { if (c == code) return idx; ++idx; }
    return -1;
}

int hat_number(uint16_t abs_code) {
    if (abs_code < kHatFirst || abs_code > kHatLast) return -1;
    return (abs_code - kHatFirst) / 2;
}

int axis_index(const std::vector<uint16_t>& abs_codes, uint16_t code) {
    if (hat_number(code) >= 0) return -1;
    std::vector<uint16_t> sorted = abs_codes;
    std::sort(sorted.begin(), sorted.end());
    int idx = 0;
    for (uint16_t c : sorted) {
        if (hat_number(c) >= 0) continue;
        if (c == code) return idx;
        ++idx;
    }
    return -1;
}

std::string bind_token(const std::vector<uint16_t>& key_codes,
                       const std::vector<uint16_t>& abs_codes,
                       PhysicalBinding::Kind kind, uint16_t code,
                       int direction) {
    switch (kind) {
        case PhysicalBinding::Kind::BUTTON: {
            int i = button_index(key_codes, code);
            return i < 0 ? "" : std::to_string(i);
        }
        case PhysicalBinding::Kind::HAT: {
            int h = hat_number(code);
            if (h < 0) return "";
            const bool is_y = (code - kHatFirst) % 2 == 1;
            const char* dir = is_y ? (direction < 0 ? "up" : "down")
                                   : (direction < 0 ? "left" : "right");
            return "h" + std::to_string(h) + dir;
        }
        case PhysicalBinding::Kind::AXIS: {
            int i = axis_index(abs_codes, code);
            if (i < 0) return "";
            return (direction < 0 ? "-" : "+") + std::to_string(i);
        }
    }
    return "";
}

}  // namespace retroarch
```

- [ ] **Step 4: Wire into CMake (both lists), build, run** → `./test_retroarch_unit "[joydev_index]"` PASS; full suite PASS.

- [ ] **Step 5: Commit** — `feat(retroarch): joydev/udev bind-token conversion`.

---

### Task 5: Profile JSON serialization + store I/O + per-port resolution

**Files:**
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_profile.h`, `.cpp`
- Modify: `magic_dingus_box_cpp/src/utils/config.h`, `.cpp` (add `get_controller_profiles_file()`)
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` — `test_retroarch_unit` needs jsoncpp: add `${JSONCPP_INCLUDE_DIRS}` to its `target_include_directories`, and `target_link_directories(... ${JSONCPP_LIBRARY_DIRS})` + `${JSONCPP_LIBRARIES}` to its link libraries (copy the pattern from `test_phone_remote_unit`, CMakeLists ~line 543-545).
- Test: append to `tests/retroarch/test_controller_profile.cpp`

**Interfaces:**
- Consumes: `PhysicalProfile`, `vidpid_key`, `logical_control_key/from_key`, `controller_style_key/from_key`, `builtin_*_profile()`, `get_semantic_mapping`, `build_mapping`, `ControllerType`, `match_vid_pid` (exposed in Task 6... NOTE: expose it HERE, it's 3 lines — see Step 5).
- Produces:
  - `std::string profiles_to_json(const std::map<std::string, PhysicalProfile>&)`
  - `std::map<std::string, PhysicalProfile> profiles_from_json(const std::string&)` — malformed/unknown input degrades to empty/partial, never throws.
  - `std::map<std::string, PhysicalProfile> load_profile_store()` / `bool save_profile_store(const std::map<std::string, PhysicalProfile>&)` — path from `config::get_controller_profiles_file()`; save is atomic tmp+rename.
  - `ControllerMapping resolve_mapping_for_pad(uint16_t vid, uint16_t pid, const std::map<std::string, PhysicalProfile>& store, const std::string& core_name)` — captured profile → builtin (by VID/PID) → legacy N64 fallback.
  - In `controller_detector.h`: `ControllerType match_vid_pid(uint16_t vid, uint16_t pid);` (moved out of the anon namespace).

- [ ] **Step 1: Write failing tests** (append to `test_controller_profile.cpp`)

```cpp
TEST_CASE("profiles round-trip through JSON", "[controller_profile][json]") {
    std::map<std::string, PhysicalProfile> in;
    PhysicalProfile p = builtin_dragonrise_profile();
    p.vid = 0x0810; p.pid = 0xe501; p.name = "Twin USB"; p.captured_at = "2026-07-28T21:00:00Z";
    in[vidpid_key(p.vid, p.pid)] = p;
    const auto out = profiles_from_json(profiles_to_json(in));
    REQUIRE(out.size() == 1);
    const auto& q = out.at("0810:e501");
    REQUIRE(q.name == "Twin USB");
    REQUIRE(q.style == ControllerStyle::PS_STYLE);
    REQUIRE(q.vid == 0x0810); REQUIRE(q.pid == 0xe501);
    REQUIRE(q.token(LogicalControl::CROSS) == "2");
    REQUIRE(q.binding(LogicalControl::DPAD_UP)->kind == PhysicalBinding::Kind::HAT);
    REQUIRE(q.binding(LogicalControl::DPAD_UP)->direction == -1);
}

TEST_CASE("malformed JSON degrades to an empty store", "[controller_profile][json]") {
    REQUIRE(profiles_from_json("").empty());
    REQUIRE(profiles_from_json("not json at all").empty());
    REQUIRE(profiles_from_json("{\"version\":1,\"profiles\":{\"x\":42}}").empty());
}

TEST_CASE("unknown keys are ignored, known ones survive", "[controller_profile][json]") {
    const char* j = R"({"version":1,"future_field":true,"profiles":{
      "0810:e501":{"name":"T","style":"ps_style","captured_at":"",
        "controls":{"cross":{"kind":"button","code":289,"token":"1"},
                    "warp_drive":{"kind":"button","code":300,"token":"9"}}}}})";
    const auto out = profiles_from_json(j);
    REQUIRE(out.size() == 1);
    REQUIRE(out.at("0810:e501").token(LogicalControl::CROSS) == "1");
    REQUIRE(out.at("0810:e501").controls.size() == 1);  // warp_drive dropped
}

TEST_CASE("resolution order: captured > builtin > N64 fallback", "[controller_profile]") {
    std::map<std::string, PhysicalProfile> store;
    // 1. Unknown pad, empty store -> N64 fallback (legacy behavior)
    auto m = resolve_mapping_for_pad(0x1234, 0x5678, store, "snes9x2010_libretro");
    REQUIRE(m.name == "Super Nintendo");
    // 2. Known builtin -> its style
    m = resolve_mapping_for_pad(0x0079, 0x0006, store, "snes9x2010_libretro");
    REQUIRE(m.name == "Super Nintendo (PS-style)");
    // 3. Captured profile for the SAME vid/pid wins over the builtin
    PhysicalProfile clone = builtin_dragonrise_profile();
    clone.controls[LogicalControl::CROSS].token = "7";   // rewired clone pad
    store[vidpid_key(0x0079, 0x0006)] = clone;
    m = resolve_mapping_for_pad(0x0079, 0x0006, store, "snes9x2010_libretro");
    REQUIRE(m.b_btn == "7");
    // 4. Captured profile for an unknown pad
    PhysicalProfile cap = builtin_dragonrise_profile();
    cap.vid = 0x1234; cap.pid = 0x5678;
    store[vidpid_key(0x1234, 0x5678)] = cap;
    REQUIRE(resolve_mapping_for_pad(0x1234, 0x5678, store, "snes9x2010_libretro").name
            == "Super Nintendo (PS-style)");
}

TEST_CASE("store save/load round-trips through a temp file", "[controller_profile][store]") {
    ::setenv("MAGIC_CONTROLLER_PROFILES_FILE", "/tmp/mdb_test_profiles.json", 1);
    std::remove("/tmp/mdb_test_profiles.json");
    REQUIRE(load_profile_store().empty());          // missing file -> empty, no error
    std::map<std::string, PhysicalProfile> in;
    in["0810:e501"] = builtin_dragonrise_profile();
    REQUIRE(save_profile_store(in));
    REQUIRE(load_profile_store().size() == 1);
    ::unsetenv("MAGIC_CONTROLLER_PROFILES_FILE");
}
```

(add `#include <cstdio>` and `#include <cstdlib>` at the top of the test file.)

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Add the config path** — `src/utils/config.h` (next to `get_settings_file()` declaration) and `src/utils/config.cpp`:

```cpp
std::string get_controller_profiles_file() {
    if (const char* env = std::getenv("MAGIC_CONTROLLER_PROFILES_FILE")) {
        return env;
    }
    return get_config_path() + "/controller_profiles.json";
}
```

- [ ] **Step 4: Implement JSON + store + resolution in `controller_profile.cpp`**

Declarations to append in `controller_profile.h`:

```cpp
#include "controller_detector.h"   // ControllerType (for resolve)
struct ControllerMapping;          // fwd-declared; defined in controller_mapping.h

std::string profiles_to_json(const std::map<std::string, PhysicalProfile>& profiles);
std::map<std::string, PhysicalProfile> profiles_from_json(const std::string& text);
std::map<std::string, PhysicalProfile> load_profile_store();
bool save_profile_store(const std::map<std::string, PhysicalProfile>& profiles);
ControllerMapping resolve_mapping_for_pad(
    uint16_t vid, uint16_t pid,
    const std::map<std::string, PhysicalProfile>& store,
    const std::string& core_name);
```

(`resolve_mapping_for_pad` returns by value → needs the full type; simplest is `#include "controller_mapping.h"` in controller_profile.cpp only, keeping the fwd-decl + a **free function declared in controller_mapping.h instead** if a cycle bites. RESOLUTION: `controller_mapping.h` already includes `controller_profile.h` (Task 3) — so declare `resolve_mapping_for_pad` in **`controller_mapping.h`** (below `build_mapping`) and implement it in `controller_mapping.cpp`. Keep JSON/store in controller_profile.)

Implementation (jsoncpp, in `controller_profile.cpp`):

```cpp
#include <json/json.h>
#include <fstream>
#include "../utils/config.h"

namespace retroarch {

namespace {
const char* kind_key(PhysicalBinding::Kind k) {
    switch (k) {
        case PhysicalBinding::Kind::BUTTON: return "button";
        case PhysicalBinding::Kind::HAT: return "hat";
        case PhysicalBinding::Kind::AXIS: return "axis";
    }
    return "button";
}
std::optional<PhysicalBinding::Kind> kind_from_key(const std::string& s) {
    if (s == "button") return PhysicalBinding::Kind::BUTTON;
    if (s == "hat") return PhysicalBinding::Kind::HAT;
    if (s == "axis") return PhysicalBinding::Kind::AXIS;
    return std::nullopt;
}
}  // namespace

std::string profiles_to_json(const std::map<std::string, PhysicalProfile>& profiles) {
    Json::Value root;
    root["version"] = 1;
    Json::Value& out = root["profiles"] = Json::Value(Json::objectValue);
    for (const auto& [key, p] : profiles) {
        Json::Value jp;
        jp["name"] = p.name;
        jp["style"] = controller_style_key(p.style);
        jp["captured_at"] = p.captured_at;
        Json::Value& jc = jp["controls"] = Json::Value(Json::objectValue);
        for (const auto& [control, b] : p.controls) {
            Json::Value jb;
            jb["kind"] = kind_key(b.kind);
            jb["code"] = b.code;
            if (b.direction != 0) jb["direction"] = b.direction;
            jb["token"] = b.token;
            jc[logical_control_key(control)] = jb;
        }
        out[key] = jp;
    }
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    return Json::writeString(w, root);
}

std::map<std::string, PhysicalProfile> profiles_from_json(const std::string& text) {
    std::map<std::string, PhysicalProfile> result;
    Json::Value root;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream in(text);
    if (text.empty() || !Json::parseFromStream(rb, in, &root, &errs)) return result;
    const Json::Value& profiles = root["profiles"];
    if (!profiles.isObject()) return result;
    for (const auto& key : profiles.getMemberNames()) {
        const Json::Value& jp = profiles[key];
        if (!jp.isObject()) continue;
        PhysicalProfile p;
        p.name = jp.get("name", "").asString();
        auto style = controller_style_from_key(jp.get("style", "").asString());
        if (!style) continue;
        p.style = *style;
        p.captured_at = jp.get("captured_at", "").asString();
        // vid/pid parsed back from the "vvvv:pppp" map key
        if (key.size() != 9 || key[4] != ':') continue;
        p.vid = static_cast<uint16_t>(std::stoul(key.substr(0, 4), nullptr, 16));
        p.pid = static_cast<uint16_t>(std::stoul(key.substr(5, 4), nullptr, 16));
        const Json::Value& jc = jp["controls"];
        if (jc.isObject()) {
            for (const auto& ck : jc.getMemberNames()) {
                auto control = logical_control_from_key(ck);
                if (!control) continue;              // unknown key: skip
                const Json::Value& jb = jc[ck];
                auto kind = kind_from_key(jb.get("kind", "").asString());
                if (!kind) continue;
                PhysicalBinding b;
                b.kind = *kind;
                b.code = static_cast<uint16_t>(jb.get("code", 0).asUInt());
                b.direction = jb.get("direction", 0).asInt();
                b.token = jb.get("token", "").asString();
                p.controls[*control] = b;
            }
        }
        result[key] = p;
    }
    return result;
}

std::map<std::string, PhysicalProfile> load_profile_store() {
    std::ifstream f(config::get_controller_profiles_file());
    if (!f.good()) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return profiles_from_json(ss.str());
}

bool save_profile_store(const std::map<std::string, PhysicalProfile>& profiles) {
    const std::string path = config::get_controller_profiles_file();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.good()) return false;
        f << profiles_to_json(profiles);
        if (!f.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace retroarch
```

`std::stoul` on a non-hex key would throw — guard with a try/catch around the two parses (`catch (...) { continue; }`).

- [ ] **Step 5: Expose `match_vid_pid` + implement `resolve_mapping_for_pad`**

`controller_detector.h` — add above `detect_primary_controller()`:

```cpp
// Match a (vendor, product) pair to a known controller type. Exposed for
// per-port mapping resolution.
ControllerType match_vid_pid(uint16_t vid, uint16_t pid);
```

(and remove it from the anon namespace in the .cpp.)

`controller_mapping.cpp`:

```cpp
ControllerMapping resolve_mapping_for_pad(
    uint16_t vid, uint16_t pid,
    const std::map<std::string, PhysicalProfile>& store,
    const std::string& core_name) {
    // 1. Captured profile wins (covers rewired clones of known pads too).
    auto it = store.find(vidpid_key(vid, pid));
    if (it != store.end()) {
        return build_mapping(get_semantic_mapping(it->second.style, core_name),
                             it->second);
    }
    // 2. Builtin by VID/PID; 3. legacy N64 fallback for everything else.
    return get_mapping(match_vid_pid(vid, pid), core_name);
}
```

- [ ] **Step 6: Build, run all tests** → PASS (snapshot included). **Step 7: Commit** — `feat(retroarch): controller profile store + per-pad mapping resolution`.

---

### Task 6: `write_player_binds()` emitter + launcher integration

Moves the hand-duplicated P1/P2 bind block out of `retroarch_launcher.cpp` (Pi-only) into `controller_mapping.cpp` (Mac-testable). Output must be byte-identical to the current block.

**Files:**
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_mapping.h`, `.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp` (lines ~533-609: the P1 block from `input_player1_analog_dpad_mode` through the P2 `_axis` lines)
- Test: `magic_dingus_box_cpp/tests/retroarch/test_player_binds.cpp`

**Interfaces:**
- Produces: `void write_player_binds(std::ostream& out, const ControllerMapping& map, int player)` — emits, in this exact order: `analog_dpad_mode`, `b/y/select/start_btn`, `up/down/left/right_btn`, `a/x_btn`, `l/r_btn`, `l2/r2_btn`, `l_x/l_y plus/minus _axis`, `write_right_stick_binds`, `up/down/left/right_axis`. All lines UNCONDITIONAL (empty values emitted as `""`) except the right-stick block — exactly what the launcher does today. Hotkeys and pcsx pad-type stay in the launcher (player-1-only policy).

- [ ] **Step 1: Write failing test**

`tests/retroarch/test_player_binds.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"

using namespace retroarch;

static std::string emit(ControllerType t, const std::string& core, int player) {
    std::ostringstream o;
    write_player_binds(o, get_mapping(t, core), player);
    return o.str();
}

TEST_CASE("player binds match the legacy launcher block line-for-line",
          "[player_binds]") {
    const std::string cfg = emit(ControllerType::PS_STYLE_DRAGONRISE,
                                 "pcsx_rearmed_libretro", 1);
    // Exact lines the launcher used to emit (spot-check the shape + a few values)
    REQUIRE(cfg.find("input_player1_analog_dpad_mode = \"0\"\n") != std::string::npos);
    REQUIRE(cfg.find("input_player1_b_btn = \"2\"\n") != std::string::npos);
    REQUIRE(cfg.find("input_player1_l2_btn = \"6\"\n") != std::string::npos);
    REQUIRE(cfg.find("input_player1_l_x_plus_axis = \"+0\"\n") != std::string::npos);
    REQUIRE(cfg.find("input_player1_up_axis = \"-1\"\n") != std::string::npos);
    // Unconditional emission: an empty value still writes the line
    const std::string nes = emit(ControllerType::N64_ADAPTER, "nestopia_libretro", 1);
    REQUIRE(nes.find("input_player1_l2_btn = \"\"\n") != std::string::npos);
}

TEST_CASE("player 2 mirrors with the player2 prefix and no player1 lines",
          "[player_binds]") {
    const std::string cfg = emit(ControllerType::N64_ADAPTER,
                                 "mupen64plus_next_libretro", 2);
    REQUIRE(cfg.find("input_player2_r_x_plus_btn = \"8\"") != std::string::npos);
    REQUIRE(cfg.find("player1") == std::string::npos);
}

TEST_CASE("two different mappings produce genuinely different P1/P2 blocks",
          "[player_binds]") {
    std::ostringstream o;
    write_player_binds(o, get_mapping(ControllerType::PS_STYLE_DRAGONRISE,
                                      "snes9x2010_libretro"), 1);
    write_player_binds(o, get_mapping(ControllerType::N64_ADAPTER,
                                      "snes9x2010_libretro"), 2);
    const std::string cfg = o.str();
    REQUIRE(cfg.find("input_player1_b_btn = \"2\"") != std::string::npos);  // Cross
    REQUIRE(cfg.find("input_player2_b_btn = \"1\"") != std::string::npos);  // N64 B
}
```

- [ ] **Step 2: FAIL run. Step 3: Implement** in `controller_mapping.cpp` (declaration in the header next to `write_right_stick_binds`):

```cpp
void write_player_binds(std::ostream& out, const ControllerMapping& map,
                        int player) {
    const std::string p = "input_player" + std::to_string(player) + "_";
    out << p << "analog_dpad_mode = \"" << map.analog_dpad_mode << "\"\n";
    out << p << "b_btn = \"" << map.b_btn << "\"\n";
    out << p << "y_btn = \"" << map.y_btn << "\"\n";
    out << p << "select_btn = \"" << map.select_btn << "\"\n";
    out << p << "start_btn = \"" << map.start_btn << "\"\n";
    out << p << "up_btn = \"" << map.up_btn << "\"\n";
    out << p << "down_btn = \"" << map.down_btn << "\"\n";
    out << p << "left_btn = \"" << map.left_btn << "\"\n";
    out << p << "right_btn = \"" << map.right_btn << "\"\n";
    out << p << "a_btn = \"" << map.a_btn << "\"\n";
    out << p << "x_btn = \"" << map.x_btn << "\"\n";
    out << p << "l_btn = \"" << map.l_btn << "\"\n";
    out << p << "r_btn = \"" << map.r_btn << "\"\n";
    out << p << "l2_btn = \"" << map.l2_btn << "\"\n";
    out << p << "r2_btn = \"" << map.r2_btn << "\"\n";
    out << p << "l_x_plus_axis = \"" << map.l_x_plus << "\"\n";
    out << p << "l_x_minus_axis = \"" << map.l_x_minus << "\"\n";
    out << p << "l_y_plus_axis = \"" << map.l_y_plus << "\"\n";
    out << p << "l_y_minus_axis = \"" << map.l_y_minus << "\"\n";
    write_right_stick_binds(out, map, player);
    out << p << "up_axis = \"" << map.up_axis << "\"\n";
    out << p << "down_axis = \"" << map.down_axis << "\"\n";
    out << p << "left_axis = \"" << map.left_axis << "\"\n";
    out << p << "right_axis = \"" << map.right_axis << "\"\n";
}
```

ORDER TRAP: the legacy P1 block emits d-pad `_btn` lines BETWEEN select/start and a/x, and the `_axis` d-pad lines AFTER the right-stick block. The function above preserves that. (RetroArch's parser is order-insensitive, but keep the diff reviewable.)

- [ ] **Step 4: PASS run, commit** — `feat(retroarch): mac-testable player bind emitter`.

- [ ] **Step 5: Replace the launcher block**

In `retroarch_launcher.cpp` `launch_drm()`: delete the block from `script_file << "input_player1_analog_dpad_mode ...` (line ~533) through the last `input_player2_right_axis` line (~609), INCLUDING both `write_right_stick_binds` calls, and replace with:

```cpp
            write_player_binds(script_file, map, 1);
            // P2 mirrors P1 for now; Task 7 resolves each port separately.
            write_player_binds(script_file, map, 2);
```

Keep everything after (`pcsx_rearmed_pad2type`, hotkeys, extra_config) untouched.

- [ ] **Step 6: Compile-verify on the Pi** (launcher doesn't build on Mac):

```bash
PI_HOST=magic@magicpi5.local ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Expected: clean build. **Step 7: Commit** — `refactor(retroarch): launcher uses write_player_binds`.

---

### Task 7: Per-port detection + launch-time resolution

**Files:**
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_detector.h`, `.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp`

**Interfaces:**
- Produces: `struct DetectedPad { int port; uint16_t vid, pid; std::string name; };` and `std::vector<DetectedPad> detect_connected_controllers();` in `controller_detector.h` — walks `/dev/input/js*` lexicographically (same enumeration as `detect_primary_controller`), one entry per node with parsed VID/PID (0000 on parse failure) and the sysfs `device/name` line.
- Consumes: `resolve_mapping_for_pad` + `load_profile_store` (Task 5).

- [ ] **Step 1: Implement `detect_connected_controllers()`**

In `controller_detector.cpp`, factor the existing js-walk into the new function (reuse `read_sysfs_line`/`parse_hex4`):

```cpp
std::vector<DetectedPad> detect_connected_controllers() {
    namespace fs = std::filesystem;
    std::vector<DetectedPad> pads;
    std::vector<fs::path> js_nodes;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
        if (ec) break;
        const auto& name = entry.path().filename().string();
        if (name.rfind("js", 0) == 0 && name.size() > 2) js_nodes.push_back(entry.path());
    }
    std::sort(js_nodes.begin(), js_nodes.end());
    int port = 0;
    for (const auto& node : js_nodes) {
        const std::string basename = node.filename().string();
        const fs::path id_dir = fs::path("/sys/class/input") / basename / "device" / "id";
        DetectedPad pad;
        pad.port = port++;
        pad.vid = parse_hex4(read_sysfs_line(id_dir / "vendor"));
        pad.pid = parse_hex4(read_sysfs_line(id_dir / "product"));
        pad.name = read_sysfs_line(fs::path("/sys/class/input") / basename / "device" / "name");
        std::cout << "controller_detector: " << node.string() << " vid="
                  << std::hex << pad.vid << " pid=" << pad.pid << std::dec
                  << " name=" << pad.name << std::endl;
        pads.push_back(pad);
    }
    return pads;
}
```

(`detect_primary_controller()` stays — the autoconfig-emission branch still uses it.)

- [ ] **Step 2: Wire per-port resolution into `launch_drm()`**

Around line ~524 (`ControllerMapping map = get_mapping(controller_type, core_name);`) replace with:

```cpp
            // 2. Resolve each port's mapping independently: captured profile
            // -> builtin -> legacy N64 fallback. A missing P2 pad mirrors P1
            // (legacy behavior for the shipped identical-pads case).
            const auto pads = detect_connected_controllers();
            const auto profile_store = load_profile_store();
            ControllerMapping map =
                pads.empty() ? get_mapping(controller_type, core_name)
                             : resolve_mapping_for_pad(pads[0].vid, pads[0].pid,
                                                       profile_store, core_name);
            ControllerMapping map_p2 =
                pads.size() > 1 ? resolve_mapping_for_pad(pads[1].vid, pads[1].pid,
                                                          profile_store, core_name)
                                : map;
```

and the Task 6 emission becomes:

```cpp
            write_player_binds(script_file, map, 1);
            write_player_binds(script_file, map_p2, 2);
```

Add the includes at the top of the file: `#include "controller_profile.h"` (for `load_profile_store`). Hotkeys, `pcsx_rearmed_pad1type`, `extra_config` continue to come from `map` (player 1) — unchanged policy; `pcsx_rearmed_pad2type` switches to `map_p2.core_option_pad_type` (guard on `!map_p2.core_option_pad_type.empty()`).

- [ ] **Step 3: Pi compile + smoke** — `deploy_cpp.sh --build`, then on the Pi run one game launch (`python3 magic_dingus_box_cpp/scripts/emulator_smoke_test.py --playlist "PS1"` or a manual UI launch) and confirm `/home/magic/retroarch_launcher.sh` contains identical P1 binds to a pre-change capture (diff against a saved copy).

- [ ] **Step 4: Run full Mac suite** (still green) **and commit** — `feat(retroarch): per-port controller mapping resolution at launch`.

---

### Task 8: `capture_session` — the wizard's state machine (pure)

**Files:**
- Create: `magic_dingus_box_cpp/src/retroarch/capture_session.h`, `.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (RETROARCH_SOURCES + test_retroarch_unit)
- Test: `magic_dingus_box_cpp/tests/retroarch/test_capture_session.cpp`

**Interfaces:**
- Consumes: `capture_steps()`, `bind_token()`, `PhysicalBinding`, `LogicalControl`.
- Produces:

```cpp
namespace retroarch {

// Everything the session needs to know about the target device, gathered
// once by InputManager when the wizard picks it (Task 9).
struct CaptureDeviceCaps {
    uint16_t vid = 0, pid = 0;
    std::string name;
    std::vector<uint16_t> key_codes;                  // ascending EV_KEY codes
    struct AxisRange { uint16_t code; int min, max, rest; };
    std::vector<AxisRange> axes;                      // ascending ABS codes (incl. hats)
};

class CaptureSession {
public:
    CaptureSession(ControllerStyle style, CaptureDeviceCaps caps);

    enum class FeedResult { NONE, CAPTURED, DUPLICATE, DONE };
    // ev_type is EV_KEY (0x01) or EV_ABS (0x03); anything else -> NONE.
    FeedResult feed(uint16_t ev_type, uint16_t code, int32_t value);

    void skip();            // current control stays unbound, advance
    bool redo_last();       // step back one (false at the first step)
    bool done() const;
    LogicalControl current_control() const;   // undefined when done()
    size_t step_index() const;
    size_t step_count() const;
    LogicalControl last_duplicate_of() const; // valid after DUPLICATE
    // Only meaningful when done(): captured bindings with tokens filled in.
    std::map<LogicalControl, PhysicalBinding> results() const;

private:
    ControllerStyle style_;
    CaptureDeviceCaps caps_;
    std::vector<LogicalControl> steps_;
    size_t index_ = 0;
    std::map<LogicalControl, PhysicalBinding> captured_;
    LogicalControl duplicate_of_{};
    // per-step transient state
    int pressed_code_ = -1;          // button awaiting release
    int armed_abs_code_ = -1;        // axis/hat deflected, awaiting return
    int armed_direction_ = 0;
};

}  // namespace retroarch
```

**Capture rules (from the spec):**
- BUTTON: `EV_KEY value==1` records the code; `value==0` on the same code captures. A different button's press replaces the pending one.
- HAT (`ABS_HAT*`): value ±1 arms (code+sign); value 0 captures.
- AXIS (non-hat ABS): deflection beyond 50% of half-range from `rest` arms (with observed sign); return within 25% of half-range of `rest` captures. `rest` comes from caps (sampled at wizard start).
- DUPLICATE: the armed/released (kind, code, direction) triple already exists in `captured_` → return `DUPLICATE`, don't advance, expose `last_duplicate_of()`.
- Tokens are computed at capture time via `bind_token(caps.key_codes, <abs codes from caps.axes>, kind, code, direction)`.
- `feed` returns `DONE` on the capture that fills the final step; `done()` true thereafter.

- [ ] **Step 1: Write failing tests**

`tests/retroarch/test_capture_session.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "retroarch/capture_session.h"

using namespace retroarch;
namespace {
constexpr uint16_t EV_KEY_T = 0x01, EV_ABS_T = 0x03;

CaptureDeviceCaps dragonrise_like() {
    CaptureDeviceCaps c;
    c.vid = 0x0810; c.pid = 0xe501; c.name = "Twin USB";
    for (uint16_t k = 288; k <= 299; ++k) c.key_codes.push_back(k);
    c.axes = {{0, -32768, 32767, 0}, {1, -32768, 32767, 0},
              {2, -32768, 32767, 0}, {5, -32768, 32767, 0},
              {16, -1, 1, 0}, {17, -1, 1, 0}};
    return c;
}
}  // namespace

TEST_CASE("happy path captures a button on press+release", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    REQUIRE(s.current_control() == LogicalControl::DPAD_UP);
    // D-pad up on the hat: arm with -1, capture on 0
    REQUIRE(s.feed(EV_ABS_T, 17, -1) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 17, 0) == CaptureSession::FeedResult::CAPTURED);
    REQUIRE(s.current_control() == LogicalControl::DPAD_DOWN);
    REQUIRE(s.feed(EV_ABS_T, 17, +1) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 17, 0) == CaptureSession::FeedResult::CAPTURED);
    // skip left/right, then CROSS as a button
    s.skip(); s.skip();
    REQUIRE(s.current_control() == LogicalControl::CROSS);
    REQUIRE(s.feed(EV_KEY_T, 290, 1) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_KEY_T, 290, 0) == CaptureSession::FeedResult::CAPTURED);
}

TEST_CASE("duplicates are rejected and reported", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    s.feed(EV_KEY_T, 290, 1); s.feed(EV_KEY_T, 290, 0);   // DPAD_UP <- btn 290
    REQUIRE(s.current_control() == LogicalControl::DPAD_DOWN);
    s.feed(EV_KEY_T, 290, 1);
    REQUIRE(s.feed(EV_KEY_T, 290, 0) == CaptureSession::FeedResult::DUPLICATE);
    REQUIRE(s.last_duplicate_of() == LogicalControl::DPAD_UP);
    REQUIRE(s.current_control() == LogicalControl::DPAD_DOWN);  // did not advance
}

TEST_CASE("axis capture honors rest position and sign", "[capture_session]") {
    auto caps = dragonrise_like();
    caps.axes[1].rest = 1000;                       // slightly off-center stick
    CaptureSession s(ControllerStyle::PS_STYLE, caps);
    while (s.current_control() != LogicalControl::LSTICK_UP) s.skip();
    // Inverted-feeling axis: up produces POSITIVE values on this pad
    REQUIRE(s.feed(EV_ABS_T, 1, 30000) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 1, 1500) == CaptureSession::FeedResult::CAPTURED);
    const auto r = s.results();                     // partial results OK for assert
    REQUIRE(r.at(LogicalControl::LSTICK_UP).direction == +1);
    REQUIRE(r.at(LogicalControl::LSTICK_UP).token == "+1");
}

TEST_CASE("small wiggles below 50% never arm", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    while (s.current_control() != LogicalControl::LSTICK_UP) s.skip();
    REQUIRE(s.feed(EV_ABS_T, 1, -8000) == CaptureSession::FeedResult::NONE);
    REQUIRE(s.feed(EV_ABS_T, 1, 0) == CaptureSession::FeedResult::NONE);   // never armed
}

TEST_CASE("redo_last steps back and unbinds", "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    REQUIRE(!s.redo_last());                        // nothing to redo yet
    s.feed(EV_KEY_T, 290, 1); s.feed(EV_KEY_T, 290, 0);
    REQUIRE(s.redo_last());
    REQUIRE(s.current_control() == LogicalControl::DPAD_UP);
    // the freed binding is reusable without DUPLICATE
    s.feed(EV_KEY_T, 290, 1);
    REQUIRE(s.feed(EV_KEY_T, 290, 0) == CaptureSession::FeedResult::CAPTURED);
}

TEST_CASE("a stickless pad finishes by skipping and yields no stick binds",
          "[capture_session]") {
    CaptureSession s(ControllerStyle::PS_STYLE, dragonrise_like());
    size_t guard = 0;
    while (!s.done() && guard++ < 100) {
        if (s.current_control() == LogicalControl::CROSS) {
            s.feed(EV_KEY_T, 290, 1); s.feed(EV_KEY_T, 290, 0);
        } else {
            s.skip();
        }
    }
    REQUIRE(s.done());
    REQUIRE(s.results().size() == 1);
    REQUIRE(s.results().count(LogicalControl::LSTICK_UP) == 0);
}

TEST_CASE("N64 flow reaches DONE on the final capture", "[capture_session]") {
    CaptureSession s(ControllerStyle::N64_STYLE, dragonrise_like());
    while (s.step_index() + 1 < s.step_count()) s.skip();
    s.feed(EV_KEY_T, 291, 1);
    REQUIRE(s.feed(EV_KEY_T, 291, 0) == CaptureSession::FeedResult::DONE);
    REQUIRE(s.done());
}
```

- [ ] **Step 2: FAIL run. Step 3: Implement `capture_session.cpp`**

```cpp
#include "capture_session.h"
#include <cstdlib>
#include "joydev_index.h"

namespace retroarch {

namespace {
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvAbs = 0x03;
constexpr uint16_t kHatFirst = 0x10, kHatLast = 0x17;
}  // namespace

CaptureSession::CaptureSession(ControllerStyle style, CaptureDeviceCaps caps)
    : style_(style), caps_(std::move(caps)), steps_(capture_steps(style)) {}

bool CaptureSession::done() const { return index_ >= steps_.size(); }
LogicalControl CaptureSession::current_control() const { return steps_[index_]; }
size_t CaptureSession::step_index() const { return index_; }
size_t CaptureSession::step_count() const { return steps_.size(); }
LogicalControl CaptureSession::last_duplicate_of() const { return duplicate_of_; }
std::map<LogicalControl, PhysicalBinding> CaptureSession::results() const { return captured_; }

void CaptureSession::skip() {
    if (done()) return;
    pressed_code_ = -1; armed_abs_code_ = -1; armed_direction_ = 0;
    ++index_;
}

bool CaptureSession::redo_last() {
    if (index_ == 0) return false;
    --index_;
    captured_.erase(steps_[index_]);
    pressed_code_ = -1; armed_abs_code_ = -1; armed_direction_ = 0;
    return true;
}

CaptureSession::FeedResult CaptureSession::feed(uint16_t ev_type, uint16_t code,
                                                int32_t value) {
    if (done()) return FeedResult::NONE;

    PhysicalBinding pending;
    bool complete = false;

    if (ev_type == kEvKey) {
        if (value == 1) { pressed_code_ = code; return FeedResult::NONE; }
        if (value == 0 && pressed_code_ == static_cast<int>(code)) {
            pending = {PhysicalBinding::Kind::BUTTON, code, 0, ""};
            complete = true;
            pressed_code_ = -1;
        }
    } else if (ev_type == kEvAbs) {
        if (code >= kHatFirst && code <= kHatLast) {
            if (value != 0) { armed_abs_code_ = code; armed_direction_ = value < 0 ? -1 : +1; return FeedResult::NONE; }
            if (armed_abs_code_ == static_cast<int>(code)) {
                pending = {PhysicalBinding::Kind::HAT, code, armed_direction_, ""};
                complete = true;
                armed_abs_code_ = -1;
            }
        } else {
            const CaptureDeviceCaps::AxisRange* ax = nullptr;
            for (const auto& a : caps_.axes) if (a.code == code) { ax = &a; break; }
            if (!ax) return FeedResult::NONE;
            const int half = std::max(1, (ax->max - ax->min) / 2);
            const int delta = value - ax->rest;
            if (std::abs(delta) > half / 2) {           // >50% deflection arms
                armed_abs_code_ = code;
                armed_direction_ = delta < 0 ? -1 : +1;
                return FeedResult::NONE;
            }
            if (armed_abs_code_ == static_cast<int>(code) &&
                std::abs(delta) < half / 4) {           // <25% of half-range = back at rest
                pending = {PhysicalBinding::Kind::AXIS, code, armed_direction_, ""};
                complete = true;
                armed_abs_code_ = -1;
            }
        }
    }

    if (!complete) return FeedResult::NONE;

    for (const auto& [control, b] : captured_) {
        if (b.kind == pending.kind && b.code == pending.code &&
            b.direction == pending.direction) {
            duplicate_of_ = control;
            return FeedResult::DUPLICATE;
        }
    }

    std::vector<uint16_t> abs_codes;
    for (const auto& a : caps_.axes) abs_codes.push_back(a.code);
    pending.token = bind_token(caps_.key_codes, abs_codes, pending.kind,
                               pending.code, pending.direction);
    captured_[steps_[index_]] = pending;
    ++index_;
    return done() ? FeedResult::DONE : FeedResult::CAPTURED;
}

}  // namespace retroarch
```

- [ ] **Step 4: Wire CMake, build, run** `"[capture_session]"` → PASS; full suite PASS. **Step 5: Commit** — `feat(retroarch): wizard capture state machine`.

---

### Task 9: InputManager — menu-nav overlays + raw-capture mode + device caps

Pi-compiled only (no Mac unit tests for this file); the pure derivation `menu_overlay_from_profile()` lives in `controller_profile.cpp` and IS Mac-tested.

**Files:**
- Modify: `magic_dingus_box_cpp/src/platform/input_manager.h`, `.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_profile.h`, `.cpp` (overlay derivation)
- Test: append to `tests/retroarch/test_controller_profile.cpp` (derivation only)

**Interfaces:**
- Produces in `input_manager.h` (namespace `platform`):

```cpp
// Per-model kiosk-menu mapping learned by the Controller Setup wizard.
// ADDITIVE: codes not listed here fall through to the built-in switch, so a
// bad profile can never make the menu less usable than stock.
struct MenuNavOverlay {
    std::map<uint16_t, InputAction> buttons;  // EV_KEY code -> action
    int nav_x_abs = -1;   // ABS code driving ROTATE (main stick X), -1 = none
    int seek_abs = -1;    // ABS code driving SEEK_LEFT/RIGHT, -1 = none
};

struct RawInputEvent {
    uint16_t vid, pid;
    std::string device_name;
    uint16_t type;   // EV_KEY / EV_ABS
    uint16_t code;
    int32_t value;
};
```

  and on `InputManager`:

```cpp
    // key = (vid << 16) | pid
    void set_menu_overlays(std::map<uint32_t, MenuNavOverlay> overlays);
    // Raw capture (wizard): while enabled, joystick devices' events are
    // delivered via drain_raw_events() INSTEAD of being action-mapped.
    // Keyboards, the rotary encoder, and the phone-remote virtual pad
    // ("MagicDingus Phone Remote") keep producing InputActions so the
    // wizard chrome stays driveable.
    void set_raw_capture(bool enabled);
    std::vector<RawInputEvent> drain_raw_events();
    // Capability snapshot for the wizard's CaptureSession (nullopt if the
    // device isn't currently open). rest = current axis value at call time.
    std::optional<retroarch::CaptureDeviceCaps> device_caps(uint16_t vid, uint16_t pid);
```

  LAYERING NOTE: `platform/` including `retroarch/capture_session.h` for `CaptureDeviceCaps` is acceptable here (header-only struct); if the include direction offends, move `CaptureDeviceCaps` into its own tiny header `src/retroarch/capture_device_caps.h` and include that from both.

- Produces in `controller_profile.h`:

```cpp
platform::MenuNavOverlay menu_overlay_from_profile(const PhysicalProfile& p);
```

- [ ] **Step 1: Write failing derivation tests** (append to `test_controller_profile.cpp`)

```cpp
#include "platform/input_manager.h"

TEST_CASE("menu overlay mirrors the kiosk's hardcoded semantics",
          "[controller_profile][overlay]") {
    using platform::InputAction;
    const auto ps = menu_overlay_from_profile(builtin_dragonrise_profile());
    REQUIRE(ps.buttons.at(290) == InputAction::SELECT);          // Cross
    REQUIRE(ps.buttons.at(297) == InputAction::SELECT);          // Start
    REQUIRE(ps.buttons.at(289) == InputAction::SETTINGS_MENU);   // Circle
    REQUIRE(ps.buttons.at(288) == InputAction::PLAY_PAUSE);      // Triangle
    REQUIRE(ps.buttons.at(293) == InputAction::NEXT);            // R1
    REQUIRE(ps.buttons.at(292) == InputAction::PREV);            // L1
    REQUIRE(ps.nav_x_abs == 0);                                  // ABS_X
    REQUIRE(ps.seek_abs == 2);                                   // ABS_Z (right stick X)

    const auto n64 = menu_overlay_from_profile(builtin_n64_adapter_profile());
    REQUIRE(n64.buttons.at(306) == InputAction::SELECT);         // A
    REQUIRE(n64.buttons.at(305) == InputAction::SETTINGS_MENU);  // B
    REQUIRE(n64.buttons.at(310) == InputAction::PLAY_PAUSE);     // Z
    REQUIRE(n64.buttons.at(309) == InputAction::NEXT);           // R
    REQUIRE(n64.buttons.at(308) == InputAction::PREV);           // L
    REQUIRE(n64.seek_abs == -1);   // C cluster is buttons, not an axis
}
```

- [ ] **Step 2: FAIL. Step 3: Implement `menu_overlay_from_profile`** in `controller_profile.cpp`:

```cpp
platform::MenuNavOverlay menu_overlay_from_profile(const PhysicalProfile& p) {
    using platform::InputAction;
    using L = LogicalControl;
    platform::MenuNavOverlay o;
    auto add_btn = [&](L c, InputAction a) {
        const auto* b = p.binding(c);
        if (b && b->kind == PhysicalBinding::Kind::BUTTON) o.buttons[b->code] = a;
    };
    // Mirrors input_manager.cpp map_button_to_action's operator semantics.
    add_btn(L::CROSS, InputAction::SELECT);
    add_btn(L::START, InputAction::SELECT);
    add_btn(L::N64_A, InputAction::SELECT);
    add_btn(L::N64_START, InputAction::SELECT);
    add_btn(L::CIRCLE, InputAction::SETTINGS_MENU);
    add_btn(L::N64_B, InputAction::SETTINGS_MENU);
    add_btn(L::TRIANGLE, InputAction::PLAY_PAUSE);
    add_btn(L::N64_Z, InputAction::PLAY_PAUSE);
    add_btn(L::R1, InputAction::NEXT);
    add_btn(L::N64_R, InputAction::NEXT);
    add_btn(L::L1, InputAction::PREV);
    add_btn(L::N64_L, InputAction::PREV);
    auto stick_axis = [&](L c) -> int {
        const auto* b = p.binding(c);
        return (b && b->kind == PhysicalBinding::Kind::AXIS) ? b->code : -1;
    };
    o.nav_x_abs = stick_axis(p.style == ControllerStyle::N64_STYLE
                                 ? L::N64_STICK_RIGHT : L::LSTICK_RIGHT);
    o.seek_abs = stick_axis(p.style == ControllerStyle::N64_STYLE
                                ? L::N64_C_RIGHT : L::RSTICK_RIGHT);
    return o;
}
```

Run → PASS. Commit `feat(retroarch): menu-nav overlay derivation`.

- [ ] **Step 4: Implement the InputManager side** (`input_manager.cpp`; compile-verified on Pi):

1. `Device` struct gains: `uint16_t vid = 0, pid = 0; const MenuNavOverlay* overlay = nullptr;`
2. In `open_joystick_devices()` after `device->name = ...`: `device->vid = libevdev_get_id_vendor(dev); device->pid = libevdev_get_id_product(dev);` then `device->overlay = lookup in overlays_ map` (member `std::map<uint32_t, MenuNavOverlay> overlays_;`). `set_menu_overlays()` stores the map AND re-resolves `overlay` pointers for already-open devices.
3. In `poll()`:
   - At the very top of the per-device loop: if `raw_capture_ && device->is_joystick && device->name != "MagicDingus Phone Remote"`, then for each event of type EV_KEY or EV_ABS push `{device->vid, device->pid, device->name, ev.type, ev.code, ev.value}` onto `raw_events_` and `continue` past all action mapping (still draining libevdev).
   - In the joystick EV_KEY branch, BEFORE `map_button_to_action`: `if (device->overlay) { auto it = device->overlay->buttons.find(ev.code); if (it != device->overlay->buttons.end()) { input_ev.action = it->second; goto emitted; } }` (or structure with a small lambda instead of goto — implementer's choice, match surrounding style which uses straight-line ifs).
   - In the EV_ABS branch, BEFORE the `ABS_Y`/default handling: if `device->overlay && ev.code == device->overlay->nav_x_abs` → run the same logic as the existing `axis == 0` ROTATE block (extract that block into a private helper `InputAction rotate_from_axis_value(int16_t value)` so both paths share it); if `ev.code == device->overlay->seek_abs` → SEEK_LEFT/RIGHT with the existing deadzone 5000.
   - NOTE: for overlay devices the hardcoded `map_axis_to_action` fallback must be SKIPPED for the two overlay-claimed codes (avoid double-firing) but still runs for everything else.
4. `set_raw_capture(bool)` clears `raw_events_` on both edges. `drain_raw_events()` moves-and-clears.
   Also in `poll()`: when `libevdev_next_event` returns `-ENODEV` for a joystick device, erase that device from `devices_` (it was unplugged). This is what lets `device_caps()` return `nullopt` for a vanished pad — the wizard's unplug-abort path (Task 10/12) depends on it.
5. `device_caps(vid, pid)`: find the open device; fill `key_codes` by iterating `libevdev_has_event_code(dev, EV_KEY, c)` for `c in [0x100, 0x2ff]`; fill axes by iterating ABS codes `0..0x3f` with `libevdev_get_abs_minimum/maximum` and `libevdev_get_event_value(dev, EV_ABS, code)` as `rest`.

- [ ] **Step 5: Pi compile** — `PI_HOST=magic@magicpi5.local ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build`. Expected: clean; kiosk still boots (`sudo systemctl restart magic-dingus-box-cpp.service` + check `verify_box.sh` kiosk-health lines) and both shipped pads still navigate the menu (overlay absent = pure fallthrough).

- [ ] **Step 6: Commit** — `feat(platform): input overlays, raw capture mode, device caps`.

---

### Task 10: Wizard UI — settings entry, screen flow, renderer, save

**Files:**
- Create: `magic_dingus_box_cpp/src/ui/controller_wizard.h`, `.cpp`
- Create: `magic_dingus_box_cpp/src/ui/controller_wizard_renderer.cpp`
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.h` (enum value + wizard accessors), `.cpp`
- Modify: `magic_dingus_box_cpp/src/ui/renderer.h` (declare `render_controller_wizard`), `src/ui/renderer.cpp` (dispatch)
- Modify: `magic_dingus_box_cpp/src/main.cpp` (SELECT dispatch, interception block, raw-event pump, overlay reload)
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (UI_SOURCES: add `src/ui/controller_wizard.cpp`, `src/ui/controller_wizard_renderer.cpp`)

**Interfaces:**
- Consumes: `CaptureSession`, `CaptureDeviceCaps`, `capture_steps`, `control_prompt`, `PhysicalProfile`, `save_profile_store`/`load_profile_store`, `vidpid_key`, `menu_overlay_from_profile`, `InputManager::{set_raw_capture, drain_raw_events, device_caps, set_menu_overlays}`, `Toast::show`.
- Produces (`src/ui/controller_wizard.h`):

```cpp
#pragma once
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include "platform/input_manager.h"
#include "retroarch/capture_session.h"

namespace ui {

class ControllerWizard {
public:
    enum class Phase { PICK_DEVICE, PICK_STYLE, CAPTURE, TEST, DONE };

    void open(platform::InputManager* input);   // enables raw capture
    void close();                               // disables raw capture
    bool is_active() const { return active_; }

    // Called every frame from main.cpp with the drained raw events.
    void on_raw_event(const platform::RawInputEvent& ev);
    // InputActions from OTHER surfaces (box buttons, rotary, phone remote,
    // keyboard). Returns true when consumed.
    bool on_action(const platform::InputEvent& ev);
    // Inactivity timeout; returns false when the wizard closed itself.
    bool tick();

    // Renderer readouts
    Phase phase() const { return phase_; }
    const std::string& device_name() const { return device_name_; }
    int style_cursor() const { return style_cursor_; }       // 0=PS, 1=N64
    std::string prompt() const;                              // current step text
    size_t step_index() const; size_t step_count() const;
    std::string status_line() const;                         // duplicate/skip feedback
    // TEST phase: which captured controls are currently "lit"
    const std::map<retroarch::LogicalControl, bool>& test_lit() const;
    const std::map<retroarch::LogicalControl, retroarch::PhysicalBinding>& captured() const;

private:
    bool active_ = false;
    Phase phase_ = Phase::PICK_DEVICE;
    platform::InputManager* input_ = nullptr;
    uint16_t vid_ = 0, pid_ = 0;
    std::string device_name_;
    int style_cursor_ = 0;
    std::optional<retroarch::CaptureDeviceCaps> caps_;
    std::unique_ptr<retroarch::CaptureSession> session_;
    std::map<retroarch::LogicalControl, retroarch::PhysicalBinding> captured_;
    std::map<retroarch::LogicalControl, bool> test_lit_;
    std::string status_;
    std::chrono::steady_clock::time_point last_input_;
    void save_profile_();
};

}  // namespace ui
```

**Behavior contract (implement exactly):**
- `open()`: `input->set_raw_capture(true)`, phase PICK_DEVICE, `last_input_ = now`.
- PICK_DEVICE: first raw EV_KEY press (`value==1`) from any device sets `vid_/pid_/device_name_`, fetches `caps_ = input_->device_caps(vid_, pid_)`; if caps missing → status "couldn't read controller"; else phase PICK_STYLE.
- PICK_STYLE: `on_action` ROTATE/ROTATE_VERTICAL toggles `style_cursor_`; SELECT confirms → construct `session_` with the chosen `ControllerStyle` and `*caps_`; phase CAPTURE. SETTINGS_MENU/QUIT cancels (close()).
- CAPTURE: raw events from the TARGET device only (`ev.vid==vid_ && ev.pid==pid_`) go to `session_->feed()`; feed results update `status_` ("already used for X" on DUPLICATE). `on_action`: PLAY_PAUSE = skip, PREV = redo_last, SETTINGS_MENU/QUIT = cancel. On DONE: `captured_ = session_->results()`, build `test_lit_` (all false), phase TEST.
- TEST: raw events from the target set `test_lit_[control]=true` while the matching binding's button is down / axis deflected, false on release (match on kind+code+direction, direction-insensitive for buttons). `on_action`: SELECT = save (`save_profile_()` then phase DONE + `Toast::show("Controller saved: " + device_name_)`), PREV = restart capture (new session, phase CAPTURE), SETTINGS_MENU/QUIT = cancel.
- `save_profile_()`: build `PhysicalProfile{name=device_name_, style, vid_, pid_, captured_at="" , controls=captured_}`, `auto store = retroarch::load_profile_store(); store[vidpid_key(vid_,pid_)] = profile; retroarch::save_profile_store(store);`
- DONE: any SELECT/SETTINGS_MENU/QUIT closes.
- `tick()`: if `now - last_input_ > 120s` → close, return false. Every `on_raw_event`/`on_action` refreshes `last_input_`. In phases CAPTURE/TEST, `tick()` also polls `input_->device_caps(vid_, pid_)` at most once per second; `nullopt` means the target pad was unplugged (InputManager drops -ENODEV devices, Task 9) → `Toast::show("Controller disconnected — setup cancelled")`, close, return false.
- `close()`: `input_->set_raw_capture(false)`, `active_ = false`, reset all state.

- [ ] **Step 1: Settings menu plumbing** — `settings_menu.h`: add `CONTROLLER_SETUP` to `MenuSection` (before `BACK`); add to the class:

```cpp
    ControllerWizard* controller_wizard();
    void open_controller_wizard(platform::InputManager* input);
    void close_controller_wizard();
    bool is_controller_wizard_active() const { return wizard_active_; }
private:
    std::unique_ptr<ControllerWizard> controller_wizard_;
    bool wizard_active_ = false;
```

`settings_menu.cpp` `open()` (~line 411, after the Phone Remote row): `menu_items_.emplace_back("Controller Setup", MenuSection::CONTROLLER_SETUP, "Map any USB gamepad");` — add the row in BOTH `open()` and the ctor list if the ctor builds one. Implement open/close following `open_pairing_screen()`/`close_pairing_screen()` (~line 650) with `wizard_active_` and `controller_wizard()->open(input)` / `->close()`.

- [ ] **Step 2: main.cpp dispatch + interception** — in the SELECT dispatch (~line 2462, next to PHONE_REMOTE):

```cpp
                        } else if (section == ui::MenuSection::CONTROLLER_SETUP) {
                            settings_menu.open_controller_wizard(&input_manager);
```

Interception block, placed IMMEDIATELY BEFORE the pairing-screen block (~line 2398), same shape:

```cpp
                // ── Controller Setup wizard: intercept everything ──
                if (settings_menu.is_controller_wizard_active()) {
                    auto* wiz = settings_menu.controller_wizard();
                    if (wiz && wiz->on_action(ev)) continue;
                    if (wiz && !wiz->is_active())
                        settings_menu.close_controller_wizard();
                    continue;  // eat all other inputs while the wizard is up
                }
```

And once per frame (near the `reprobe_phone_remote` cadence code or just before the input loop), pump raw events + tick + reload overlays after save:

```cpp
        if (settings_menu.is_controller_wizard_active()) {
            auto* wiz = settings_menu.controller_wizard();
            for (const auto& raw : input_manager.drain_raw_events())
                wiz->on_raw_event(raw);
            if (!wiz->tick() || !wiz->is_active()) {
                settings_menu.close_controller_wizard();
                // Profiles may have changed — refresh menu-nav overlays.
                std::map<uint32_t, platform::MenuNavOverlay> overlays;
                for (const auto& [key, prof] : retroarch::load_profile_store())
                    overlays[(uint32_t(prof.vid) << 16) | prof.pid] =
                        retroarch::menu_overlay_from_profile(prof);
                input_manager.set_menu_overlays(overlays);
            }
        }
```

Also add the same overlay-building loop ONCE at startup (right after `input_manager.initialize()` in main), factored into a small static helper in main.cpp: `static void reload_menu_overlays(platform::InputManager&);`.

- [ ] **Step 3: Renderer** — `renderer.h`: declare `void render_controller_wizard(const ControllerWizard& wiz);` next to `render_pairing_screen` (~line 430). `renderer.cpp` dispatch (~line 1302): before the pairing branch:

```cpp
        if (state.settings_menu->is_controller_wizard_active()) {
            ui::ControllerWizard* wiz = state.settings_menu->controller_wizard();
            if (wiz) render_controller_wizard(*wiz);
        } else if (state.settings_menu->is_pairing_screen_active()) {
```

`controller_wizard_renderer.cpp` — copy `pairing_screen_renderer.cpp`'s structure (private Renderer method in its own .cpp, uses `draw_quad`/`draw_text`, `theme_` colors, 1280x720 logical coordinates via `width_`/`height_`). Layout per phase:
- Full-screen dark panel (`draw_quad(0, 0, width_, height_, bg color)`), heading "Controller Setup" at top in `font_heading_size`, accent color.
- PICK_DEVICE: centered "Press any button on the controller you want to set up"; sub-line "Phone remote and box buttons keep working for everything else".
- PICK_STYLE: two rows "PlayStation-style controller" / "Nintendo 64-style controller", highlight `style_cursor()`, footer "Rotate/D-pad: choose   A/Select: confirm   B: cancel".
- CAPTURE: big `prompt()` centered; `step_index()+1` of `step_count()` progress line; `status_line()` under it in warn color; left column lists all steps with ✓ for captured / "skipped"; footer "BTN2/Play: skip   BTN1/L: redo   BTN3/B: cancel".
- TEST: two-column list of captured controls; lit ones drawn in accent; footer "A: save   L: redo all   B: cancel".
- DONE: "Saved!" + device name.

- [ ] **Step 4: Pi compile + manual smoke** — `deploy_cpp.sh --build`, restart kiosk, then on the TV: Settings → Controller Setup appears; open with only shipped pads present; run a full capture on the DragonRise (its captured profile should override the builtin harmlessly — same layout); confirm toast + `config/controller_profiles.json` exists and parses (`python3 -c "import json;print(len(json.load(open('/opt/magic_dingus_box/config/controller_profiles.json'))['profiles']))"`); relaunch a game and diff `~/retroarch_launcher.sh` binds (should be unchanged values for this pad); menu nav still works.

- [ ] **Step 5: Commit** — `feat(ui): controller setup wizard`.

---

### Task 11: Docs — OTA guarantees + CHANGELOG

**Files:**
- Modify: `OTA_UPDATE_GUARANTEES.md` (preserved table)
- Modify: `CHANGELOG.md` (new unreleased entry)

- [ ] **Step 1:** Add to the preserved table in `OTA_UPDATE_GUARANTEES.md` (config row already covers it; add an explicit line item for discoverability):

```markdown
| `config/controller_profiles.json` | Captured controller mappings from the Controller Setup wizard. Covered by the existing `config/*` exclude; listed here so nobody "cleans it up". | Per-model button/axis profiles keyed by USB VID/PID. |
```

- [ ] **Step 2:** CHANGELOG entry under a new unreleased heading:

```markdown
### Added
- **Controller Setup wizard** (Settings → Controller Setup): press-each-button
  mapping for any USB gamepad. Profiles are stored per controller model
  (USB VID/PID) in `config/controller_profiles.json`, survive OTA updates,
  drive per-core RetroArch binds for both players independently, and make
  third-party pads navigate the kiosk menus.

### Changed
- Controller mapping internals refactored into semantic tables + physical
  profiles (`build_mapping()`); output for the two shipped pads is
  snapshot-locked and unchanged. Player-bind emission moved to the
  Mac-testable `write_player_binds()`.
```

- [ ] **Step 3: Commit** — `docs: controller wizard OTA + changelog entries`.

---

### Task 12: On-Pi validation — token probe + end-to-end

**Files:**
- Create: `magic_dingus_box_cpp/src/tools/controller_probe.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (Linux-only tool target)

**Interfaces:**
- Consumes: `joydev_index`, `builtin profiles`; libevdev directly.

- [ ] **Step 1: Implement the probe**

```cpp
// controller_probe: prints, for every /dev/input/event* joystick, the
// device identity, its ordered key/abs capability lists, and the
// joydev_index-computed token for every button and axis direction.
// Ground truth check: run with the two shipped pads connected and compare
// against the legacy tokens in the builtin profiles — every one must match.
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "retroarch/joydev_index.h"

int main() {
    DIR* dir = opendir("/dev/input");
    if (!dir) return 1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct libevdev* dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); continue; }
        if (!libevdev_has_event_type(dev, EV_ABS)) { libevdev_free(dev); close(fd); continue; }
        printf("%s: %s vid=%04x pid=%04x\n", path, libevdev_get_name(dev),
               libevdev_get_id_vendor(dev), libevdev_get_id_product(dev));
        std::vector<uint16_t> keys, abses;
        for (unsigned c = 0x100; c <= 0x2ff; ++c)
            if (libevdev_has_event_code(dev, EV_KEY, c)) keys.push_back(c);
        for (unsigned c = 0; c <= 0x3f; ++c)
            if (libevdev_has_event_code(dev, EV_ABS, c)) abses.push_back(c);
        for (uint16_t k : keys)
            printf("  key 0x%03x -> btn token \"%s\"\n", k,
                   retroarch::bind_token(keys, abses,
                       retroarch::PhysicalBinding::Kind::BUTTON, k, 0).c_str());
        for (uint16_t a : abses) {
            const bool hat = retroarch::hat_number(a) >= 0;
            printf("  abs 0x%02x -> -:\"%s\" +:\"%s\"\n", a,
                   retroarch::bind_token(keys, abses,
                       hat ? retroarch::PhysicalBinding::Kind::HAT
                           : retroarch::PhysicalBinding::Kind::AXIS, a, -1).c_str(),
                   retroarch::bind_token(keys, abses,
                       hat ? retroarch::PhysicalBinding::Kind::HAT
                           : retroarch::PhysicalBinding::Kind::AXIS, a, +1).c_str());
        }
        libevdev_free(dev);
        close(fd);
    }
    closedir(dir);
    return 0;
}
```

CMake (near the other Linux-gated targets):

```cmake
if(UNIX AND NOT APPLE)
    add_executable(controller_probe
        src/tools/controller_probe.cpp
        src/retroarch/joydev_index.cpp
        src/retroarch/logical_controls.cpp
        src/retroarch/controller_profile.cpp)
    target_include_directories(controller_probe PRIVATE src ${JSONCPP_INCLUDE_DIRS} ${EVDEV_INCLUDE_DIRS})
    target_link_directories(controller_probe PRIVATE ${JSONCPP_LIBRARY_DIRS})
    target_link_libraries(controller_probe PRIVATE ${EVDEV_LIBRARIES} ${JSONCPP_LIBRARIES})
endif()
```

(check the actual pkg-config variable name for libevdev at the top of CMakeLists — the main target's list shows it.)

- [ ] **Step 2: The verification checklist (run on the Pi, record results in the PR/commit message)**

```bash
PI_HOST=magic@magicpi5.local ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

then on the Pi:

1. **Token ground truth:** `./build/controller_probe` with the N64 adapter and DragonRise connected. Every computed token MUST equal the builtin-profile token (N64: A→"2", Z→"6", Start→"12", hat d-pad, stick ±0/±1; DragonRise: Cross→"2", Start→"9", right stick ±2/±3 — if the probe says the real Ry axis index is 5 not 3, STOP: the legacy +3 token and the builtin profile both need a hardware decision; consult RetroArch's verbose log (`retroarch --verbose` shows udev axis counts) before changing anything).
2. **Also read RetroArch's udev driver source** (`input/drivers_joypad/udev_joypad.c` of the installed version) and confirm the two-range button ordering + non-hat axis ordering assumption in `joydev_index.h`'s header comment. Record the confirmation in the commit.
3. **Wizard end-to-end with a third-party pad:** full PS-style capture; then menu nav (Cross=select, Circle=settings, stick scrolls); then launch NES + PS1 + N64 titles and verify buttons land where the wizard said; then `verify_box.sh` SHIPPABLE.
4. **Two-player mixed:** third-party pad in port 0 + DragonRise in port 1 → PS1 two-player title (Twisted Metal); P1 and P2 both respond with correct layouts; inspect `~/retroarch_launcher.sh` — P1/P2 blocks differ.
5. **Regression sweeps:** `python3 magic_dingus_box_cpp/tests/manual/ui_launch_test.py` (expect 14/14) and `python3 magic_dingus_box_cpp/scripts/emulator_smoke_test.py` (expect clean pass), shipped pads only, no profiles for them in the store.
6. **Abort paths:** unplug the target mid-capture (clean toast, settings back); walk away 2+ minutes (auto-close); malformed `controller_profiles.json` (kiosk boots, wizard still opens).

- [ ] **Step 3: Commit** — `feat(tools): on-Pi controller token probe` + a `docs:` commit recording the validation results.

---

## Execution notes

- Task order is the dependency order; Tasks 1–8 are Mac-only TDD, Tasks 9–10 need the Pi for compile/manual verification, Task 12 needs physical pads.
- After every task: full `./test_retroarch_unit` — the `[mapping_snapshot]` suite failing means STOP and fix before proceeding.
- The uncommitted `fix/content-manager-playlist-editor` branch also edits `retroarch_launcher.cpp`; whichever lands second merges the small P1/P2-block region by keeping BOTH changes (theirs: core-options; ours: `write_player_binds` calls).
