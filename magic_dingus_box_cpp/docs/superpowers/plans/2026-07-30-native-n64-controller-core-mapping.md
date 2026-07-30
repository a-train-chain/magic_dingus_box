# Native N64 Controller Core Mapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make an N64-style controller drive every N64 core function according
to the label printed on the physical controller, with Z+Start opening the
RetroArch menu.

**Architecture:** Keep the saved physical controller profile as the source of
device-specific tokens. Change only the N64-style semantic table for N64 cores
so it targets Mupen64Plus-Next's independent C-button RetroPad contract
directly, then let the existing `build_mapping()` and launcher serialization
compose the final configuration. Preserve every non-N64 semantic branch and
all launch/core options.

**Tech Stack:** C++17, Catch2, CMake/CTest, RetroArch 1.20,
Mupen64Plus-Next/ParaLLEl N64 libretro cores, systemd, Raspberry Pi OS.

## Global Constraints

- Physical N64 A/B/C-Up/C-Down/C-Left/C-Right/L/R/Z/Start must perform the
  matching native N64 function.
- Physical analog stick and D-pad remain independent and native.
- Physical Z+Start opens the RetroArch menu in N64 games; Z and Start retain
  their native single-button functions.
- `mupen64plus-alt-map = "True"` remains enabled for both supported N64 cores.
- Right-stick C-button bindings must be absent for an N64-style controller;
  the four C buttons use Mupen's direct digital RetroPad slots.
- The saved controller profile, wizard format, kiosk UI actions, PS1 mapping,
  Dreamcast mapping, DualShock overrides, PS-style N64 mapping, rendering,
  saves, and performance options must not change.
- The mapping applies to both `mupen64plus_next_libretro` and
  `parallel_n64_libretro`, for every ROM.

---

### Task 1: Correct the N64-style semantic mapping

**Files:**

- Modify: `magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp:128-151`
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_mapping.h:72-99`
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_controller_mapping.cpp:19-69,255-330`
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_port_resolution.cpp:162-269`
- Modify: `magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h`

**Interfaces:**

- Consumes: `SemanticMapping semantic_n64_style(const std::string& core)`,
  `ControllerMapping build_mapping(const SemanticMapping&,
  const PhysicalProfile&)`, and saved `PhysicalProfile` tokens.
- Produces: the existing `ControllerMapping` interface with direct native N64
  button fields and empty right-stick fields. No signature or file-format
  change.

- [ ] **Step 1: Rewrite the built-in N64 regression before production code**

In `tests/retroarch/test_controller_mapping.cpp`, replace the old N64
right-stick expectations with one table-driven check for both N64 cores:

```cpp
for (const auto& core : {"mupen64plus_next_libretro",
                         "parallel_n64_libretro"}) {
    INFO("core=" << core);
    const auto map = get_mapping(ControllerType::N64_ADAPTER, core);

    CHECK(map.b_btn == "2");       // physical A -> native A
    CHECK(map.y_btn == "1");       // physical B -> native B
    CHECK(map.a_btn == "3");       // C-Down
    CHECK(map.x_btn == "9");       // C-Up
    CHECK(map.l_btn == "0");       // C-Left
    CHECK(map.r_btn == "8");       // C-Right
    CHECK(map.l2_btn == "6");      // Z
    CHECK(map.r2_btn == "5");      // R shoulder
    CHECK(map.select_btn == "4");  // L shoulder
    CHECK(map.start_btn == "12");

    CHECK(map.r_x_plus.empty());
    CHECK(map.r_x_minus.empty());
    CHECK(map.r_y_plus.empty());
    CHECK(map.r_y_minus.empty());
    CHECK(map.r_x_plus_btn.empty());
    CHECK(map.r_x_minus_btn.empty());
    CHECK(map.r_y_plus_btn.empty());
    CHECK(map.r_y_minus_btn.empty());

    CHECK(map.enable_hotkey_btn == "6");
    CHECK(map.menu_toggle_btn == "12");
}
```

Update the config-emission tests so an N64-style pad emits direct P1 and P2
button fields and `write_right_stick_binds()` emits nothing. Keep the
PS-style-pad right-stick-axis section unchanged. The serialized N64 checks
must include:

```cpp
CHECK(cfg.find("input_player2_b_btn = \"2\"\n") != std::string::npos);
CHECK(cfg.find("input_player2_y_btn = \"1\"\n") != std::string::npos);
CHECK(cfg.find("input_player2_a_btn = \"3\"\n") != std::string::npos);
CHECK(cfg.find("input_player2_x_btn = \"9\"\n") != std::string::npos);
CHECK(cfg.find("input_player2_l_btn = \"0\"\n") != std::string::npos);
CHECK(cfg.find("input_player2_r_btn = \"8\"\n") != std::string::npos);
CHECK(cfg.find("input_player2_select_btn = \"4\"\n") != std::string::npos);
CHECK(cfg.find("input_player2_r2_btn = \"5\"\n") != std::string::npos);
CHECK(cfg.find("input_player2_r_") == std::string::npos);
```

Update the unknown-controller comparison to compare direct native fields
(`y_btn`, `select_btn`, and `r2_btn`) rather than an empty right-stick field.

- [ ] **Step 2: Update the persisted-profile regression before production code**

In
`tests/retroarch/test_port_resolution.cpp`, retain all kiosk, PS1, and
Dreamcast assertions verbatim. Replace only the N64 subsection with a loop
over both N64 cores:

```cpp
for (const auto& core : {"mupen64plus_next_libretro",
                         "parallel_n64_libretro"}) {
    INFO("core=" << core);
    const auto n64 =
        resolve_mapping_for_pad(kUnknownVid, kUnknownPid, store, core);
    CHECK(n64.b_btn == "1");       // saved physical A
    CHECK(n64.y_btn == "2");       // saved physical B
    CHECK(n64.a_btn == "0");       // saved C-Down
    CHECK(n64.x_btn == "9");       // saved C-Up
    CHECK(n64.l_btn == "3");       // saved C-Left
    CHECK(n64.r_btn == "8");       // saved C-Right
    CHECK(n64.l2_btn == "6");      // saved Z
    CHECK(n64.r2_btn == "5");      // saved R
    CHECK(n64.select_btn == "4");  // saved L
    CHECK(n64.start_btn == "12");
    CHECK(n64.r_x_plus.empty());
    CHECK(n64.r_x_minus.empty());
    CHECK(n64.r_y_plus.empty());
    CHECK(n64.r_y_minus.empty());
    CHECK(n64.r_x_plus_btn.empty());
    CHECK(n64.r_x_minus_btn.empty());
    CHECK(n64.r_y_plus_btn.empty());
    CHECK(n64.r_y_minus_btn.empty());
    CHECK(n64.enable_hotkey_btn == "6");
    CHECK(n64.menu_toggle_btn == "12");
}
```

Keep native D-pad and left-stick assertions for the resolved N64 mapping as
well as PS1 and Dreamcast.

- [ ] **Step 3: Run the focused tests and verify RED**

Run from `magic_dingus_box_cpp`:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][n64]"
./build/test_retroarch_unit "[retroarch][wizard_profile][cross_layer]"
```

Expected: both focused runs fail on the old N64 semantics. Representative
old values are `a_btn == "1"` instead of native `y_btn == "1"` for the
built-in profile, and non-empty `r_*_btn` C bindings.

- [ ] **Step 4: Implement the minimal N64-only semantic correction**

Replace only the N64-style/N64-core branch in
`src/retroarch/controller_mapping.cpp` with:

```cpp
} else if (core.find("mupen64plus") != std::string::npos ||
           core.find("parallel_n64") != std::string::npos) {
    // ---- Nintendo 64 on a real N64 pad -------------------------
    // Mupen's independent C-button mode gives every native N64 control a
    // dedicated digital RetroPad slot. Route the physical labels directly
    // to those slots; no right-stick bridge or duplicate defaults.
    s.name = "Nintendo 64 (N64 pad)";
    s.analog_dpad_mode = "0";
    s.clear_unassigned_buttons = true;

    s.b = L::N64_A;          // native A
    s.y = L::N64_B;          // native B
    s.a = L::N64_C_DOWN;
    s.x = L::N64_C_UP;
    s.l = L::N64_C_LEFT;
    s.r = L::N64_C_RIGHT;
    s.l2 = L::N64_Z;
    s.r2 = L::N64_R;
    s.select = L::N64_L;
    s.start = L::N64_START;

    stick();
    s.left_stick = true;
    dpad();
    hotkeys();               // physical Z + Start
```

Do not set `s.r_up`, `s.r_down`, `s.r_left`, or `s.r_right`.

Update only stale comments in `controller_mapping.h` and
`write_right_stick_binds()` so they say the right-stick representation is
for PS-style/modern pads emulating N64 C buttons; native N64-style pads use
the independent digital slots. Do not alter serialization behavior.

- [ ] **Step 5: Run the focused tests and verify GREEN**

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][n64]"
./build/test_retroarch_unit "[retroarch][wizard_profile][cross_layer]"
```

Expected: both commands pass, including both N64 cores, the built-in profile,
the persisted `2563:0575` profile, direct serialization, empty right-stick
fields, and Z+Start.

- [ ] **Step 6: Update only the four intentional mapping snapshots**

Run the hidden generator as an independent check:

```bash
./build/test_retroarch_unit "[mapping_snapshot_gen]" \
  > /tmp/native-n64-mapping-golden.txt
```

In `tests/retroarch/mapping_snapshot_golden.h`, replace only the
`N64|mupen64plus_next_libretro`, `N64|parallel_n64_libretro`,
`UNKNOWN|mupen64plus_next_libretro`, and
`UNKNOWN|parallel_n64_libretro` entries. Each new entry must contain:

```text
name=Nintendo 64 (N64 pad)
adm=0|drv=udev|pad=
btn=2,1,4,12,3,9,0,8,6,5
l3r3=,
dpad=h0up,h0down,h0left,h0right
ls=+0,-0,+1,-1
rs_axis=,,,
rs_btn=,,,
dpad_axis=,,,
hotkeys=6,12,
extra=
```

Compare those four literals with the corresponding generator output. Do not
replace or reformat the other 29 golden entries.

- [ ] **Step 7: Prove PS1, Dreamcast, and PS-style N64 isolation**

```bash
./build/test_retroarch_unit "[retroarch][mapping][ps1][n64_style]"
./build/test_retroarch_unit "[retroarch][mapping][dreamcast][n64_style]"
./build/test_retroarch_unit "[ps1_analog][player_binds][n64][ps_style]"
./build/test_retroarch_unit "[mapping_snapshot]"
./build/test_retroarch_unit
cmake --build build -j4
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: all focused tests pass, all 33 snapshots pass, the full RetroArch
suite passes, the full build succeeds, and CTest reports 8/8 targets passing.

- [ ] **Step 8: Review the diff and commit**

Confirm:

```bash
git diff --stat
git diff -- magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp \
  magic_dingus_box_cpp/src/retroarch/controller_mapping.h \
  magic_dingus_box_cpp/tests/retroarch/test_controller_mapping.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_port_resolution.cpp \
  magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h
```

Only the five listed files may change. Commit:

```bash
git add magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp \
  magic_dingus_box_cpp/src/retroarch/controller_mapping.h \
  magic_dingus_box_cpp/tests/retroarch/test_controller_mapping.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_port_resolution.cpp \
  magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h
git commit -m "fix(n64): restore native controller semantics"
```

### Task 2: Deploy and validate the native N64 mapping

**Files:**

- Read: `/opt/magic_dingus_box/config/controller_profiles.json`
- Read: `/tmp/retroarch_mdb.cfg`
- Read: `/tmp/retroarch_core_options.cfg`
- Write through deployment: `/opt/magic_dingus_box/magic_dingus_box_cpp`
- Report: SDD workspace `task-2-report.md`

**Interfaces:**

- Consumes: the reviewed Task 1 commit and live saved profile `2563:0575`.
- Produces: a rebuilt active kiosk service and fresh Super Mario 64 launch
  evidence. It does not change the profile store.

- [ ] **Step 1: Record pre-deployment state**

```bash
git status --short --branch
git rev-parse HEAD
cmake --build build -j4
ctest --test-dir build --output-on-failure
ssh -o BatchMode=yes magic@magicpi5.local \
  'sha256sum /opt/magic_dingus_box/config/controller_profiles.json;
   stat -c "%U:%G %a" /opt/magic_dingus_box/config/controller_profiles.json;
   systemctl is-active magic-dingus-box-cpp.service'
```

Expected: clean branch at the reviewed Task 1 commit, local CTest 8/8,
profile owner/mode `magic:magic 644`, and active service. Record the live
profile checksum; the exact value, not a hard-coded historic checksum, is the
post-deployment invariant.

- [ ] **Step 2: Deploy, build, and run Pi tests safely**

From `magic_dingus_box_cpp`:

```bash
PI_HOST=magic@magicpi5.local ./scripts/deploy_cpp.sh
ssh -o BatchMode=yes magic@magicpi5.local \
  'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build &&
   cmake --build . -j4 &&
   ctest --output-on-failure'
```

Do not use `deploy_cpp.sh --test`; that path starts a second foreground DRM
process while the managed kiosk owns the display. Expected: deployment exits
zero, Pi build exits zero, and Pi CTest reports 9/9 targets passing.

- [ ] **Step 3: Restart only the managed kiosk service**

```bash
ssh -o BatchMode=yes magic@magicpi5.local \
  'sudo systemctl restart magic-dingus-box-cpp.service &&
   systemctl is-active magic-dingus-box-cpp.service'
```

Expected: `active`. Do not launch a second kiosk process or edit the profile
store.

- [ ] **Step 4: Verify preserved profile state**

Repeat the profile checksum and owner/mode command from Step 1. Expected:
checksum, owner, and mode exactly match the recorded pre-deployment values.
Parse the JSON and confirm `profiles["2563:0575"]` still contains
`n64_a=1`, `n64_b=2`, `n64_c_down=0`, `n64_c_left=3`, `n64_c_right=8`,
`n64_c_up=9`, `n64_l=4`, `n64_r=5`, `n64_z=6`, and `n64_start=12`.

- [ ] **Step 5: Request a fresh Super Mario 64 launch**

Ask the user to launch `Super Mario 64 (USA).z64` from the restarted kiosk
and leave it running. Do not inspect stale `/tmp` files before the relaunch.

- [ ] **Step 6: Inspect the fresh live configuration**

```bash
ssh -o BatchMode=yes magic@magicpi5.local '
pgrep -a retroarch
stat -c "%n | %s | %y" /tmp/retroarch_mdb.cfg \
  /tmp/retroarch_core_options.cfg
grep -E "^input_player1_(b|y|a|x|l|r|l2|r2|select|start)_btn|^input_player1_r_[xy]_(plus|minus)_(axis|btn)|^input_enable_hotkey_btn|^input_menu_toggle_btn" \
  /tmp/retroarch_mdb.cfg
grep -E "^mupen64plus-alt-map" /tmp/retroarch_core_options.cfg
'
```

Expected player-one values for the live saved profile:

```text
input_player1_b_btn = "1"
input_player1_y_btn = "2"
input_player1_a_btn = "0"
input_player1_x_btn = "9"
input_player1_l_btn = "3"
input_player1_r_btn = "8"
input_player1_l2_btn = "6"
input_player1_r2_btn = "5"
input_player1_select_btn = "4"
input_player1_start_btn = "12"
input_enable_hotkey_btn = "6"
input_menu_toggle_btn = "12"
mupen64plus-alt-map = "True"
```

No `input_player1_r_x_*` or `input_player1_r_y_*` line may be present.
The process must show `mupen64plus_next_libretro` and Super Mario 64, and the
config mtimes must be later than the service restart.

- [ ] **Step 7: Run the final automated gate**

```bash
cmake --build build -j4
ctest --test-dir build --output-on-failure
ssh -o BatchMode=yes magic@magicpi5.local \
  'systemctl is-active magic-dingus-box-cpp.service &&
   cd /opt/magic_dingus_box/magic_dingus_box_cpp/build &&
   ctest --output-on-failure'
```

Expected: local CTest 8/8, active service, and Pi CTest 9/9.

- [ ] **Step 8: Request hands-on acceptance**

Ask the user to confirm:

- A jumps;
- B performs the game's N64 B action;
- all four C buttons move the camera only in their matching direction;
- L, R, Z, Start, D-pad, and analog stick behave natively; and
- Z+Start opens the RetroArch menu.

Do not claim live completion until the user confirms these behaviors.
