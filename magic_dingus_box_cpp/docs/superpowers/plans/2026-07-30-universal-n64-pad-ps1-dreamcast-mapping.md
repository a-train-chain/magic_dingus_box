# Universal N64-style controller mapping for PS1 and Dreamcast implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the existing N64-style-controller PS1 and Dreamcast mappings with the approved universal, layer-free layouts while preserving every unrelated controller and core mapping.

**Architecture:** Keep the existing logical-control pipeline. Change only the PS1 and Flycast branches in `semantic_n64_style()`, resolve those logical assignments through the existing physical profile, and pin the emitted `ControllerMapping` and RetroArch configuration with Catch2 tests and golden snapshots.

**Tech Stack:** C++17, Catch2 v3, CMake/CTest, RetroArch 1.20 configuration, Bash deployment to Raspberry Pi 5.

## Global Constraints

- The design specification is `docs/superpowers/specs/2026-07-30-universal-n64-pad-ps1-dreamcast-mapping-design.md`.
- The change is limited to the PS1 and Dreamcast branches of `semantic_n64_style()`.
- PS1 cores are PCSX-ReARMed, Beetle PSX, and SwanStation; Dreamcast uses Flycast.
- The layouts apply to every game and both controller ports with no per-title remaps.
- Physical button and axis tokens must come from the N64-style `PhysicalProfile`; do not hard-code USB tokens in semantic production logic.
- The D-pad and analog stick remain independent on both systems.
- PS1 has no modifier layer, right-stick mapping, L3, or R3.
- Dreamcast C-Up and C-Right remain unbound.
- PS1 uses RetroArch's `L1 + R1 + Start + Select` menu combination; it has no explicit `enable_hotkey_btn`.
- Dreamcast uses Z + Start for the RetroArch menu.
- Dreamcast L/R triggers are digital full presses; do not claim or emulate pressure sensitivity.
- Every intentionally unbound button serializes as `"nul"`, never `""`.
- No PS-style controller mapping or unrelated N64-style system mapping may change.

## File structure

- Modify `src/retroarch/controller_mapping.cpp`: own the two approved semantic mappings.
- Modify `tests/retroarch/test_controller_mapping.cpp`: pin resolved physical tokens, independent D-pad/stick behavior, hotkeys, and both-player config output.
- Modify `tests/retroarch/mapping_snapshot_golden.h`: update only the four deliberate N64/unknown fallback PS1 and Dreamcast entries.
- Do not create a new mapping layer, remap file, or per-title configuration.

---

### Task 1: Implement the universal layer-free PS1 mapping

**Files:**
- Modify: `src/retroarch/controller_mapping.cpp:49-60`
- Modify: `tests/retroarch/test_controller_mapping.cpp:71-86`
- Modify: `tests/retroarch/mapping_snapshot_golden.h`

**Interfaces:**
- Consumes: `SemanticMapping`, `LogicalControl`, `build_mapping()`, `get_mapping()`, and `write_player_binds()` from the existing mapping pipeline.
- Produces: the resolved `ControllerMapping` for every `N64_STYLE + PS1` core, with fields `b/y/select/start/a/x/l/r/l2/r2`, native D-pad tokens, left-stick axes, empty right-stick/click fields, and no explicit hotkeys.

- [ ] **Step 1: Add a failing resolved-mapping and emitted-config test**

Add this test to `tests/retroarch/test_controller_mapping.cpp` before the Dreamcast test:

```cpp
TEST_CASE("N64 adapter uses the universal layer-free PS1 layout",
          "[retroarch][mapping][ps1][n64_style]") {
    for (const auto& core : {"pcsx_rearmed_libretro",
                             "beetle_psx_libretro",
                             "swanstation_libretro"}) {
        INFO("core=" << core);
        const auto map = get_mapping(ControllerType::N64_ADAPTER, core);

        CHECK(map.name == "PS1 (N64 Controller)");
        CHECK(map.core_option_pad_type == "analog");
        CHECK(map.analog_dpad_mode == "0");

        CHECK(map.b_btn == "2");       // N64 A      -> Cross
        CHECK(map.y_btn == "1");       // N64 B      -> Square
        CHECK(map.x_btn == "0");       // C-Left     -> Triangle
        CHECK(map.a_btn == "3");       // C-Down     -> Circle
        CHECK(map.l_btn == "4");       // L           -> L1
        CHECK(map.r_btn == "5");       // R           -> R1
        CHECK(map.l2_btn == "6");      // Z           -> L2
        CHECK(map.r2_btn == "8");      // C-Right     -> R2
        CHECK(map.select_btn == "9");  // C-Up        -> Select
        CHECK(map.start_btn == "12");

        CHECK(map.up_btn == "h0up");
        CHECK(map.down_btn == "h0down");
        CHECK(map.left_btn == "h0left");
        CHECK(map.right_btn == "h0right");
        CHECK(map.l_x_plus == "+0");
        CHECK(map.l_x_minus == "-0");
        CHECK(map.l_y_plus == "+1");
        CHECK(map.l_y_minus == "-1");

        CHECK(map.up_axis.empty());
        CHECK(map.down_axis.empty());
        CHECK(map.left_axis.empty());
        CHECK(map.right_axis.empty());
        CHECK(map.r_x_plus.empty());
        CHECK(map.r_x_minus.empty());
        CHECK(map.r_y_plus.empty());
        CHECK(map.r_y_minus.empty());
        CHECK(map.r_x_plus_btn.empty());
        CHECK(map.r_x_minus_btn.empty());
        CHECK(map.r_y_plus_btn.empty());
        CHECK(map.r_y_minus_btn.empty());
        CHECK(map.l3_btn.empty());
        CHECK(map.r3_btn.empty());
        CHECK(map.enable_hotkey_btn.empty());
        CHECK(map.menu_toggle_btn.empty());

        for (int player : {1, 2}) {
            INFO("player=" << player);
            std::ostringstream out;
            retroarch::write_player_binds(out, map, player);
            const std::string cfg = out.str();
            const std::string p =
                "input_player" + std::to_string(player) + "_";

            CHECK(cfg.find(p + "b_btn = \"2\"\n") != std::string::npos);
            CHECK(cfg.find(p + "y_btn = \"1\"\n") != std::string::npos);
            CHECK(cfg.find(p + "select_btn = \"9\"\n") != std::string::npos);
            CHECK(cfg.find(p + "start_btn = \"12\"\n") != std::string::npos);
            CHECK(cfg.find(p + "a_btn = \"3\"\n") != std::string::npos);
            CHECK(cfg.find(p + "x_btn = \"0\"\n") != std::string::npos);
            CHECK(cfg.find(p + "l_btn = \"4\"\n") != std::string::npos);
            CHECK(cfg.find(p + "r_btn = \"5\"\n") != std::string::npos);
            CHECK(cfg.find(p + "l2_btn = \"6\"\n") != std::string::npos);
            CHECK(cfg.find(p + "r2_btn = \"8\"\n") != std::string::npos);
            CHECK(cfg.find(p + "l3_btn = \"nul\"\n") != std::string::npos);
            CHECK(cfg.find(p + "r3_btn = \"nul\"\n") != std::string::npos);
            CHECK(cfg.find(p + "up_axis = \"\"\n") != std::string::npos);
            CHECK(cfg.find(p + "down_axis = \"\"\n") != std::string::npos);
            CHECK(cfg.find(p + "r_x_") == std::string::npos);
            CHECK(cfg.find("_btn = \"\"\n") == std::string::npos);
        }
    }
}
```

- [ ] **Step 2: Run the focused test and verify it fails for the old layout**

Run:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][ps1][n64_style]"
```

Expected: FAIL because the old branch maps N64 B to Circle, omits L2, duplicates the analog stick onto D-pad axes, and emits Z + Start as explicit hotkeys.

- [ ] **Step 3: Implement the minimal PS1 semantic mapping**

First replace the function-level hotkey comment above
`semantic_n64_style()` with:

```cpp
// Default N64-style hotkey combo: Z trigger + Start.
// PS1 is the exception: Z is L2 there, so that branch relies on RetroArch's
// global L1 + R1 + Start + Select gamepad combination instead.
```

Replace the existing PS1 branch in `semantic_n64_style()` with:

```cpp
    } else if (core.find("pcsx") != std::string::npos ||
               core.find("beetle_psx") != std::string::npos ||
               core.find("swanstation") != std::string::npos) {
        // ---- Sony PlayStation on an N64 pad ------------------------
        // Layer-free original PlayStation layout. The physical D-pad and
        // analog stick stay independent; DualShock right-stick/L3/R3
        // functions are intentionally outside this controller's scope.
        s.name = "PS1 (N64 Controller)";
        s.core_option_pad_type = "analog";
        s.analog_dpad_mode = "0";
        s.clear_unassigned_buttons = true;

        s.b = L::N64_A;        // Cross
        s.y = L::N64_B;        // Square
        s.x = L::N64_C_LEFT;   // Triangle
        s.a = L::N64_C_DOWN;   // Circle
        s.l = L::N64_L;        // L1
        s.r = L::N64_R;        // R1
        s.l2 = L::N64_Z;       // L2
        s.r2 = L::N64_C_RIGHT; // R2
        s.select = L::N64_C_UP;
        s.start = L::N64_START;

        dpad();
        stick();
        s.left_stick = true;
        // No stick_to_dpad and no explicit hotkeys. RetroArch's global
        // L1+R1+Start+Select combo opens the menu.
```

- [ ] **Step 4: Run the focused test and verify it passes**

Run:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][ps1][n64_style]"
```

Expected: PASS for all three PS1 core names and both players.

- [ ] **Step 5: Run the snapshot test and verify only the intentional PS1 entries fail**

Run:

```bash
./build/test_retroarch_unit "[mapping_snapshot]"
```

Expected: FAIL only for:

```text
N64|pcsx_rearmed_libretro
UNKNOWN|pcsx_rearmed_libretro
```

- [ ] **Step 6: Update the two PS1 golden entries**

In `tests/retroarch/mapping_snapshot_golden.h`, update the `N64|pcsx_rearmed_libretro` and `UNKNOWN|pcsx_rearmed_libretro` entries so each contains:

```text
name=PS1 (N64 Controller)
adm=0|drv=udev|pad=analog
btn=2,1,9,12,3,0,4,5,6,8
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=,,
extra=
```

Do not regenerate or edit any other golden entry.

- [ ] **Step 7: Run the PS1, snapshot, and full RetroArch suites**

Run:

```bash
./build/test_retroarch_unit "[retroarch][mapping][ps1][n64_style]"
./build/test_retroarch_unit "[mapping_snapshot]"
./build/test_retroarch_unit
```

Expected: all commands exit 0; the full RetroArch suite reports zero failed assertions.

- [ ] **Step 8: Confirm scope and commit Task 1**

Run:

```bash
git diff --check
git diff --stat
git diff -- tests/retroarch/mapping_snapshot_golden.h
```

Expected: production changes are confined to the PS1 branch, tests cover the approved layout, and only the two PS1 golden entries changed.

Commit:

```bash
git add src/retroarch/controller_mapping.cpp \
        tests/retroarch/test_controller_mapping.cpp \
        tests/retroarch/mapping_snapshot_golden.h
git commit -m "feat(ps1): map N64 controller layer-free"
```

---

### Task 2: Implement the role-consistent Dreamcast mapping

**Files:**
- Modify: `src/retroarch/controller_mapping.cpp:135-154`
- Modify: `tests/retroarch/test_controller_mapping.cpp:71-85`
- Modify: `tests/retroarch/mapping_snapshot_golden.h`

**Interfaces:**
- Consumes: the same `SemanticMapping -> build_mapping() -> ControllerMapping` pipeline pinned in Task 1.
- Produces: the Flycast mapping `A/X/Y/B` on physical `A/B/C-Left/C-Down`, digital triggers on L/R, native D-pad and analog stick, Z + Start hotkeys, and explicit unbound values for every unused slot.

- [ ] **Step 1: Replace the old Dreamcast test with the approved failing expectations**

Replace `TEST_CASE("Dreamcast is playable on the N64 adapter", ...)` in `tests/retroarch/test_controller_mapping.cpp` with:

```cpp
TEST_CASE("N64 adapter uses the role-consistent Dreamcast layout",
          "[retroarch][mapping][dreamcast][n64_style]") {
    const auto map =
        get_mapping(ControllerType::N64_ADAPTER, "flycast_libretro");

    CHECK(map.name == "Dreamcast (N64 pad)");
    CHECK(map.analog_dpad_mode == "0");
    CHECK(map.b_btn == "2");       // N64 A      -> DC A
    CHECK(map.y_btn == "1");       // N64 B      -> DC X
    CHECK(map.x_btn == "0");       // C-Left     -> DC Y
    CHECK(map.a_btn == "3");       // C-Down     -> DC B
    CHECK(map.l2_btn == "4");      // L          -> left trigger
    CHECK(map.r2_btn == "5");      // R          -> right trigger
    CHECK(map.start_btn == "12");

    CHECK(map.select_btn.empty());
    CHECK(map.l_btn.empty());
    CHECK(map.r_btn.empty());
    CHECK(map.l3_btn.empty());
    CHECK(map.r3_btn.empty());
    CHECK(map.r_x_plus.empty());
    CHECK(map.r_x_plus_btn.empty());
    CHECK(map.up_axis.empty());
    CHECK(map.down_axis.empty());
    CHECK(map.left_axis.empty());
    CHECK(map.right_axis.empty());

    CHECK(map.up_btn == "h0up");
    CHECK(map.down_btn == "h0down");
    CHECK(map.left_btn == "h0left");
    CHECK(map.right_btn == "h0right");
    CHECK(map.l_x_plus == "+0");
    CHECK(map.l_x_minus == "-0");
    CHECK(map.l_y_plus == "+1");
    CHECK(map.l_y_minus == "-1");

    CHECK(map.enable_hotkey_btn == "6");
    CHECK(map.menu_toggle_btn == "12");

    for (int player : {1, 2}) {
        INFO("player=" << player);
        std::ostringstream out;
        retroarch::write_player_binds(out, map, player);
        const std::string cfg = out.str();
        const std::string p =
            "input_player" + std::to_string(player) + "_";

        CHECK(cfg.find(p + "b_btn = \"2\"\n") != std::string::npos);
        CHECK(cfg.find(p + "y_btn = \"1\"\n") != std::string::npos);
        CHECK(cfg.find(p + "select_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "start_btn = \"12\"\n") != std::string::npos);
        CHECK(cfg.find(p + "a_btn = \"3\"\n") != std::string::npos);
        CHECK(cfg.find(p + "x_btn = \"0\"\n") != std::string::npos);
        CHECK(cfg.find(p + "l_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "l2_btn = \"4\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r2_btn = \"5\"\n") != std::string::npos);
        CHECK(cfg.find(p + "l3_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r3_btn = \"nul\"\n") != std::string::npos);
        CHECK(cfg.find(p + "up_axis = \"\"\n") != std::string::npos);
        CHECK(cfg.find(p + "down_axis = \"\"\n") != std::string::npos);
        CHECK(cfg.find(p + "r_x_") == std::string::npos);
        CHECK(cfg.find("_btn = \"\"\n") == std::string::npos);
    }
}
```

- [ ] **Step 2: Run the focused Dreamcast test and verify it fails**

Run:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][dreamcast][n64_style]"
```

Expected: FAIL because the old mapping sends N64 B to Dreamcast B, uses C-Left/C-Down for X/Y in the opposite approved roles, and leaves legacy Select/L1/R1 defaults active.

- [ ] **Step 3: Implement the minimal Dreamcast semantic mapping**

Replace the Flycast branch in `semantic_n64_style()` with:

```cpp
    } else if (core.find("flycast") != std::string::npos) {
        // ---- Sega Dreamcast on an N64 pad --------------------------
        // Match the approved PS1 muscle memory: primary on A,
        // attack/secondary on B, upper action on C-Left, and
        // back/alternate on C-Down.
        s.name = "Dreamcast (N64 pad)";
        s.analog_dpad_mode = "0";
        s.clear_unassigned_buttons = true;

        s.b = L::N64_A;        // DC A
        s.y = L::N64_B;        // DC X
        s.x = L::N64_C_LEFT;   // DC Y
        s.a = L::N64_C_DOWN;   // DC B
        s.l2 = L::N64_L;       // DC left trigger, digital full press
        s.r2 = L::N64_R;       // DC right trigger, digital full press
        s.start = L::N64_START;

        stick();
        s.left_stick = true;
        dpad();
        hotkeys();             // Z + Start
```

Do not assign C-Up, C-Right, Select, RetroPad L/R, right-stick directions, L3, or R3.

- [ ] **Step 4: Run the focused test and verify it passes**

Run:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][dreamcast][n64_style]"
```

Expected: PASS for the approved game-facing fields, both players, independent D-pad/stick, and Z + Start mapping.

- [ ] **Step 5: Run the snapshot test and verify only the intentional Dreamcast entries fail**

Run:

```bash
./build/test_retroarch_unit "[mapping_snapshot]"
```

Expected: FAIL only for:

```text
N64|flycast_libretro
UNKNOWN|flycast_libretro
```

- [ ] **Step 6: Update the two Dreamcast golden entries**

In `tests/retroarch/mapping_snapshot_golden.h`, update the `N64|flycast_libretro` and `UNKNOWN|flycast_libretro` entries so each contains:

```text
name=Dreamcast (N64 pad)
adm=0|drv=udev|pad=
btn=2,1,,12,3,0,,,4,5
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=6,12,
extra=
```

Do not edit the PS1 entries from Task 1 or any unrelated golden entry.

- [ ] **Step 7: Run the Dreamcast, snapshot, and complete local suites**

Run:

```bash
./build/test_retroarch_unit "[retroarch][mapping][dreamcast][n64_style]"
./build/test_retroarch_unit "[mapping_snapshot]"
./build/test_retroarch_unit
ctest --test-dir build --output-on-failure
```

Expected: all commands exit 0; CTest reports all targets passed and zero failures.

- [ ] **Step 8: Confirm scope and commit Task 2**

Run:

```bash
git diff --check
git diff --stat
git diff HEAD -- tests/retroarch/mapping_snapshot_golden.h
```

Expected: Task 2 changes only the Flycast branch, its focused test, and the two Dreamcast golden entries.

Commit:

```bash
git add src/retroarch/controller_mapping.cpp \
        tests/retroarch/test_controller_mapping.cpp \
        tests/retroarch/mapping_snapshot_golden.h
git commit -m "feat(dreamcast): align N64 controller actions"
```

---

### Task 3: Deploy and verify the live PS1 and Dreamcast mappings

**Files:**
- No repository files should change.
- Inspect live generated files: `/tmp/retroarch_mdb.cfg` and `/tmp/retroarch_core_options.cfg`.

**Interfaces:**
- Consumes: the green, reviewed Task 1 and Task 2 commits.
- Produces: a clean Raspberry Pi deployment, exact live configuration evidence, and hands-on acceptance for representative installed games.

- [ ] **Step 1: Run the final local pre-deployment gate**

Run from `magic_dingus_box_cpp`:

```bash
git status --short
git diff --check
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Expected: clean worktree, no whitespace errors, successful build, and 100% of CTest targets passing.

- [ ] **Step 2: Deploy from the implementation worktree and run Pi tests**

Run:

```bash
PI_HOST=magic@magicpi5.local ./scripts/deploy_cpp.sh --test
```

Expected: rsync succeeds, the Pi build and tests pass, the kiosk service restarts, and `magic-dingus-box-cpp.service` becomes active.

- [ ] **Step 3: Launch PS1 Metal Gear Solid and inspect the fresh config**

Launch `Metal Gear Solid (USA) (v1.1).m3u` from the kiosk with the N64-style controller as player 1. Then run:

```bash
ssh magic@magicpi5.local '
  systemctl is-active magic-dingus-box-cpp.service
  pgrep -a retroarch
  grep -E "^input_player1_(b|y|select|start|a|x|l|r|l2|r2|l3|r3|up|down|left|right)_btn" /tmp/retroarch_mdb.cfg
  grep -E "^input_player1_l_[xy]_(plus|minus)_axis" /tmp/retroarch_mdb.cfg
  grep -E "^input_player1_(up|down|left|right)_axis" /tmp/retroarch_mdb.cfg
  grep "^input_menu_toggle_gamepad_combo" /tmp/retroarch_mdb.cfg
  ! grep -E "^input_player[12]_.*_btn = \"\"$" /tmp/retroarch_mdb.cfg
  ! grep -E "^input_player1_r_[xy]_(plus|minus)_(axis|btn)" /tmp/retroarch_mdb.cfg
'
```

Expected game-facing lines:

```text
input_player1_b_btn = "2"
input_player1_y_btn = "1"
input_player1_select_btn = "9"
input_player1_start_btn = "12"
input_player1_a_btn = "3"
input_player1_x_btn = "0"
input_player1_l_btn = "4"
input_player1_r_btn = "5"
input_player1_l2_btn = "6"
input_player1_r2_btn = "8"
input_player1_l3_btn = "nul"
input_player1_r3_btn = "nul"
input_player1_up_btn = "h0up"
input_player1_down_btn = "h0down"
input_player1_left_btn = "h0left"
input_player1_right_btn = "h0right"
input_player1_l_x_plus_axis = "+0"
input_player1_l_x_minus_axis = "-0"
input_player1_l_y_plus_axis = "+1"
input_player1_l_y_minus_axis = "-1"
input_player1_up_axis = ""
input_player1_down_axis = ""
input_player1_left_axis = ""
input_player1_right_axis = ""
input_menu_toggle_gamepad_combo = "1"
```

Also confirm there is no `input_enable_hotkey_btn` or `input_menu_toggle_btn` line for the PS1 launch.

- [ ] **Step 4: Perform PS1 hands-on acceptance**

In Metal Gear Solid, confirm:

- N64 A, B, C-Left, and C-Down reach Cross, Square, Triangle, and Circle;
- L, R, Z, and C-Right reach L1, R1, L2, and R2;
- C-Up reaches Select and Start reaches Start;
- the D-pad and analog stick work independently; and
- L + R + C-Up + Start opens the RetroArch Quick Menu.

Then launch `Gran Turismo (USA).chd` and confirm the D-pad/analog movement behavior remains independent and all digital shoulder inputs respond. Do not expect gradual Dreamcast-style pressure from this PS1 check.

- [ ] **Step 5: Launch Dreamcast Sonic Adventure and inspect the fresh config**

Launch `Sonic Adventure (USA) (En,Ja,Fr,De,Es) (Rev A).chd`. Then rerun the live inspection with:

```bash
ssh magic@magicpi5.local '
  systemctl is-active magic-dingus-box-cpp.service
  pgrep -a retroarch
  grep -E "^input_player1_(b|y|select|start|a|x|l|r|l2|r2|l3|r3|up|down|left|right)_btn" /tmp/retroarch_mdb.cfg
  grep -E "^input_player1_l_[xy]_(plus|minus)_axis" /tmp/retroarch_mdb.cfg
  grep -E "^input_player1_(up|down|left|right)_axis" /tmp/retroarch_mdb.cfg
  grep -E "^(input_enable_hotkey_btn|input_menu_toggle_btn)" /tmp/retroarch_mdb.cfg
  ! grep -E "^input_player[12]_.*_btn = \"\"$" /tmp/retroarch_mdb.cfg
  ! grep -E "^input_player1_r_[xy]_(plus|minus)_(axis|btn)" /tmp/retroarch_mdb.cfg
'
```

Expected game-facing and hotkey lines:

```text
input_player1_b_btn = "2"
input_player1_y_btn = "1"
input_player1_select_btn = "nul"
input_player1_start_btn = "12"
input_player1_a_btn = "3"
input_player1_x_btn = "0"
input_player1_l_btn = "nul"
input_player1_r_btn = "nul"
input_player1_l2_btn = "4"
input_player1_r2_btn = "5"
input_player1_l3_btn = "nul"
input_player1_r3_btn = "nul"
input_player1_up_btn = "h0up"
input_player1_down_btn = "h0down"
input_player1_left_btn = "h0left"
input_player1_right_btn = "h0right"
input_player1_l_x_plus_axis = "+0"
input_player1_l_x_minus_axis = "-0"
input_player1_l_y_plus_axis = "+1"
input_player1_l_y_minus_axis = "-1"
input_player1_up_axis = ""
input_player1_down_axis = ""
input_player1_left_axis = ""
input_player1_right_axis = ""
input_enable_hotkey_btn = "6"
input_menu_toggle_btn = "12"
```

- [ ] **Step 6: Perform Dreamcast hands-on acceptance**

In Sonic Adventure, confirm:

- N64 A, B, C-Left, and C-Down reach Dreamcast A, X, Y, and B;
- Start, D-pad, and analog movement work;
- C-Up and C-Right produce no game action; and
- Z + Start opens the RetroArch Quick Menu.

Then launch `Crazy Taxi (USA).chd` and confirm L and R produce full left-trigger and right-trigger presses. Record that throttle/brake are digital on/off inputs; gradual pressure is physically unavailable on this controller.

- [ ] **Step 7: Run the final evidence gate**

Run:

```bash
git status --short
ctest --test-dir build --output-on-failure
ssh magic@magicpi5.local '
  systemctl is-active magic-dingus-box-cpp.service
  cd /opt/magic_dingus_box/magic_dingus_box_cpp/build
  ctest --output-on-failure
'
```

Expected: clean worktree, all local and Pi tests pass, and the kiosk service is active. No code commit is created by Task 3.
