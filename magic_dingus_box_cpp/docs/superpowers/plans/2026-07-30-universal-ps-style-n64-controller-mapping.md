# Universal PS-style N64 Controller Mapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the approved position-based DualShock layout for every N64 game using a captured PS-style controller.

**Architecture:** Keep controller-specific tokens in `PhysicalProfile` and express the universal layout in `semantic_ps_style()` using logical controls. Add a PS-style-N64-only clear-before-fill switch so an intentionally unused RetroPad slot becomes truly unbound without changing any other core or controller style. Continue generating ordinary RetroArch player binds and relying on the already-pinned Mupen alternate map for the final RetroPad-to-N64 translation.

**Tech Stack:** C++17, Catch2, CMake, RetroArch 1.20.0, Mupen64Plus-Next/Parallel N64 libretro cores, SSH/rsync deployment to Raspberry Pi host `magicpi5.local`.

## Global Constraints

- Apply the layout to both supported N64 cores, both players, and every controller profile classified as `PS_STYLE`.
- Do not change mappings for PS1, SNES, any other non-N64 system, or N64-shaped controllers.
- Do not create `.rmp` files or per-game remaps.
- Resolve physical button numbers and axis tokens from the captured profile; do not hardcode SHANWAN tokens in production code.
- Keep `mupen64plus-alt-map = "True"` for both N64 cores.
- Keep the physical Select button as the hotkey modifier, Start as N64 Start, and Select+Start as the RetroArch menu chord.
- Keep L3 and R3 unbound for N64.
- Serialize every empty `*_btn` field as `nul`, never `""`.
- Preserve unrelated user changes already present in the worktree.

---

## File structure

- `src/retroarch/controller_mapping.h`
  - Owns `SemanticMapping`; add the N64-only clear-before-fill policy flag.
- `src/retroarch/controller_mapping.cpp`
  - Owns the PS-style N64 semantic table, profile resolution, and `nul` button serialization.
- `tests/retroarch/test_controller_mapping.cpp`
  - Pins the universal layout against the built-in PS-style profile for both N64 cores.
- `tests/retroarch/test_ps1_analog_binds.cpp`
  - Contains the full captured SHANWAN profile; pin the resolved physical tokens, both players, D-pad, sticks, and hotkeys here.
- `tests/retroarch/test_player_binds.cpp`
  - Keeps the empty-button-to-`nul` regression independent of whether N64 intentionally binds RetroPad R2.
- `tests/retroarch/mapping_snapshot_golden.h`
  - Updates only the two deliberate `PS_STYLE + N64` snapshots.
- `docs/superpowers/specs/2026-07-29-n64-ps-pad-mapping-status.md`
  - Add a correction for the disproven “in-memory remap” diagnosis and point to the approved replacement design.

---

### Task 1: Preserve the deployed `nul` serialization fix

The worktree already contains the red-green implementation that fixed the live
Smash collision. Do not recreate or revert it. This task verifies and commits
that existing focused change before layering the universal mapping on top.

**Files:**
- Modify: `src/retroarch/controller_mapping.cpp:555-584`
- Modify: `src/retroarch/controller_mapping.h:249-258`
- Test: `tests/retroarch/test_player_binds.cpp:14-36`
- Test: `tests/retroarch/test_hardware_capture_regression.cpp:70-134`
- Test: `tests/retroarch/test_player_binds_legacy_equivalence.cpp:23-113`
- Test: `tests/retroarch/test_ps1_analog_binds.cpp:29-30,195-272`

**Interfaces:**
- Consumes: `void write_player_binds(std::ostream&, const ControllerMapping&, int)`
- Produces: the invariant that an empty button token serializes as RetroArch's `nul` sentinel.

- [ ] **Step 1: Inspect the existing focused diff**

Run:

```bash
git diff -- \
  src/retroarch/controller_mapping.cpp \
  src/retroarch/controller_mapping.h \
  tests/retroarch/test_player_binds.cpp \
  tests/retroarch/test_hardware_capture_regression.cpp \
  tests/retroarch/test_player_binds_legacy_equivalence.cpp \
  tests/retroarch/test_ps1_analog_binds.cpp
```

Confirm that the production change is the `write_btn()` helper and that all
test changes replace expected empty button strings with `"nul"`. Do not include
the universal layout in this commit.

- [ ] **Step 2: Run the complete RetroArch unit suite**

Run:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit
```

Expected:

```text
All tests passed (1412 assertions in 141 test cases)
```

- [ ] **Step 3: Confirm the deployed source matches the local fix**

Run:

```bash
sha256sum src/retroarch/controller_mapping.cpp src/retroarch/controller_mapping.h
ssh magic@magicpi5.local \
  'cd /opt/magic_dingus_box/magic_dingus_box_cpp && sha256sum src/retroarch/controller_mapping.cpp src/retroarch/controller_mapping.h'
```

Expected: local and remote hashes match for both files.

- [ ] **Step 4: Commit only the serialization fix**

```bash
git add \
  src/retroarch/controller_mapping.cpp \
  src/retroarch/controller_mapping.h \
  tests/retroarch/test_player_binds.cpp \
  tests/retroarch/test_hardware_capture_regression.cpp \
  tests/retroarch/test_player_binds_legacy_equivalence.cpp \
  tests/retroarch/test_ps1_analog_binds.cpp
git commit -m "fix(retroarch): serialize unbound buttons as nul"
```

Expected: the design and plan commits remain separate, and no unrelated file
is included.

---

### Task 2: Implement the universal PS-style N64 layout with TDD

**Files:**
- Modify: `src/retroarch/controller_mapping.h:120-168`
- Modify: `src/retroarch/controller_mapping.cpp:271-291,330-383`
- Test: `tests/retroarch/test_controller_mapping.cpp:87-99`
- Test: `tests/retroarch/test_ps1_analog_binds.cpp:224-247`
- Test: `tests/retroarch/test_player_binds.cpp:24-36`
- Test: `tests/retroarch/mapping_snapshot_golden.h:218-239`
- Modify: `docs/superpowers/specs/2026-07-29-n64-ps-pad-mapping-status.md:1-15`

**Interfaces:**
- Consumes: `SemanticMapping`, `PhysicalProfile`, `build_mapping()`, `resolve_mapping_for_pad()`, and `write_player_binds()`.
- Produces: `SemanticMapping::clear_unassigned_buttons` and the approved universal PS-style N64 mapping.

- [ ] **Step 1: Decouple the `nul` regression from the N64 layout**

In `tests/retroarch/test_player_binds.cpp`, replace the assertion that assumes
the PS-style N64 mapping leaves RetroPad R2 empty:

```cpp
    // Regression for the live SHANWAN/N64 failure: an empty R2 used to become
    // physical button 0. Keep this serializer test independent of whether a
    // particular console intentionally assigns RetroPad R2.
    ControllerMapping unbound;
    unbound.r2_btn.clear();
    std::ostringstream unbound_out;
    write_player_binds(unbound_out, unbound, 1);
    REQUIRE(unbound_out.str().find("input_player1_r2_btn = \"nul\"\n") !=
            std::string::npos);
```

This remains green before the mapping change and continues guarding the
original root cause afterward.

- [ ] **Step 2: Replace the incomplete built-in N64 mapping test**

Replace `TEST_CASE("PS-style pad keeps its N64 mapping", ...)` in
`tests/retroarch/test_controller_mapping.cpp` with:

```cpp
TEST_CASE("PS-style pads use the universal N64 layout",
          "[retroarch][mapping][n64][ps_style]") {
    for (const auto& core : {"mupen64plus_next_libretro",
                             "parallel_n64_libretro"}) {
        INFO("core=" << core);
        const auto map =
            get_mapping(ControllerType::PS_STYLE_DRAGONRISE, core);

        CHECK(map.b_btn == "2");       // Cross    -> N64 A
        CHECK(map.y_btn == "3");       // Square   -> N64 B
        CHECK(map.x_btn == "0");       // Triangle -> C-Up
        CHECK(map.r_btn == "1");       // Circle   -> C-Right
        CHECK(map.select_btn == "4");  // L1       -> N64 L
        CHECK(map.l2_btn == "6");      // L2       -> N64 Z
        CHECK(map.r2_btn == "5");      // R1       -> N64 R
        CHECK(map.a_btn == "7");       // R2       -> C-Down
        CHECK(map.l_btn.empty());      // C-Left stays on the right stick
        CHECK(map.start_btn == "9");
        CHECK(map.enable_hotkey_btn == "8");
        CHECK(map.menu_toggle_btn == "9");
        CHECK(map.l3_btn.empty());
        CHECK(map.r3_btn.empty());

        CHECK(map.up_btn == "h0up");
        CHECK(map.down_btn == "h0down");
        CHECK(map.left_btn == "h0left");
        CHECK(map.right_btn == "h0right");
        CHECK(map.l_x_plus == "+0");
        CHECK(map.l_x_minus == "-0");
        CHECK(map.l_y_plus == "+1");
        CHECK(map.l_y_minus == "-1");
        CHECK(map.r_x_plus == "+2");
        CHECK(map.r_x_minus == "-2");
        CHECK(map.r_y_plus == "+3");
        CHECK(map.r_y_minus == "-3");
    }
}
```

- [ ] **Step 3: Add a captured-SHANWAN config test for both cores and players**

Append this test after the existing N64 right-stick test in
`tests/retroarch/test_ps1_analog_binds.cpp`:

```cpp
TEST_CASE("captured PS-style pad resolves the universal N64 layout",
          "[ps1_analog][player_binds][n64][ps_style]") {
    const PhysicalProfile profile = shanwan_profile();
    const auto store = store_of(profile);

    for (const auto& core : {"mupen64plus_next_libretro",
                             "parallel_n64_libretro"}) {
        INFO("core=" << core);
        const auto map = retroarch::resolve_mapping_for_pad(
            profile.vid, profile.pid, store, core);

        CHECK(map.b_btn == "0");       // bottom / printed A -> N64 A
        CHECK(map.y_btn == "3");       // left / printed X   -> N64 B
        CHECK(map.x_btn == "4");       // top / printed Y    -> C-Up
        CHECK(map.r_btn == "1");       // right / printed B  -> C-Right
        CHECK(map.select_btn == "6");  // L1                 -> N64 L
        CHECK(map.l2_btn == "8");      // L2                 -> N64 Z
        CHECK(map.r2_btn == "7");      // R1                 -> N64 R
        CHECK(map.a_btn == "9");       // R2                 -> C-Down
        CHECK(map.l_btn.empty());
        CHECK(map.start_btn == "11");
        CHECK(map.enable_hotkey_btn == "10");
        CHECK(map.menu_toggle_btn == "11");
        CHECK(map.l3_btn.empty());
        CHECK(map.r3_btn.empty());

        CHECK(map.up_btn == "h0up");
        CHECK(map.down_btn == "h0down");
        CHECK(map.left_btn == "h0left");
        CHECK(map.right_btn == "h0right");
        CHECK(map.l_x_plus == "+0");
        CHECK(map.l_x_minus == "-0");
        CHECK(map.l_y_plus == "+1");
        CHECK(map.l_y_minus == "-1");
        CHECK(map.r_x_plus == "+2");
        CHECK(map.r_x_minus == "-2");
        CHECK(map.r_y_plus == "+3");
        CHECK(map.r_y_minus == "-3");

        for (int player : {1, 2}) {
            INFO("player=" << player);
            const std::string cfg = emit_for(profile, core, player);
            const std::string p =
                "input_player" + std::to_string(player) + "_";
            CHECK(has_line(cfg, p + "b_btn = \"0\"\n"));
            CHECK(has_line(cfg, p + "y_btn = \"3\"\n"));
            CHECK(has_line(cfg, p + "select_btn = \"6\"\n"));
            CHECK(has_line(cfg, p + "start_btn = \"11\"\n"));
            CHECK(has_line(cfg, p + "a_btn = \"9\"\n"));
            CHECK(has_line(cfg, p + "x_btn = \"4\"\n"));
            CHECK(has_line(cfg, p + "l_btn = \"nul\"\n"));
            CHECK(has_line(cfg, p + "r_btn = \"1\"\n"));
            CHECK(has_line(cfg, p + "l2_btn = \"8\"\n"));
            CHECK(has_line(cfg, p + "r2_btn = \"7\"\n"));
            CHECK(has_line(cfg, p + "l3_btn = \"nul\"\n"));
            CHECK(has_line(cfg, p + "r3_btn = \"nul\"\n"));
            CHECK(cfg.find("_btn = \"\"\n") == std::string::npos);
        }
    }
}
```

- [ ] **Step 4: Run the new tests and verify the expected failure**

Run:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[mapping][n64][ps_style]"
./build/test_retroarch_unit "[player_binds][n64][ps_style]"
```

Expected: failures show the old mapping, including Circle in RetroPad A,
L1/R1 in RetroPad L/R, no physical R1 in RetroPad R2, and no explicit
clear-before-fill behavior for RetroPad L.

- [ ] **Step 5: Add an explicit clear-before-fill policy**

Add this field and comment to `SemanticMapping` in
`src/retroarch/controller_mapping.h`:

```cpp
    // When true, build_mapping() clears every RetroPad button slot before
    // applying the semantic assignments below. This lets a table express
    // "intentionally unbound" despite ControllerMapping's legacy non-empty
    // defaults, without changing any other core or controller style.
    bool clear_unassigned_buttons = false;
```

In `build_mapping()` in `src/retroarch/controller_mapping.cpp`, immediately
after copying the mapping metadata and before `put_btn()` is called, add:

```cpp
    if (sem.clear_unassigned_buttons) {
        m.b_btn.clear();
        m.y_btn.clear();
        m.select_btn.clear();
        m.start_btn.clear();
        m.a_btn.clear();
        m.x_btn.clear();
        m.l_btn.clear();
        m.r_btn.clear();
        m.l2_btn.clear();
        m.r2_btn.clear();
        m.l3_btn.clear();
        m.r3_btn.clear();
    }
```

Do not clear D-pad or axis fields here. Those have separate kind-aware
resolution paths and are explicitly assigned by the N64 branch.

- [ ] **Step 6: Implement the approved semantic table**

Replace the PS-style N64 button assignments in
`src/retroarch/controller_mapping.cpp` with:

```cpp
        s.name = "Nintendo 64 (PS-style)";
        s.clear_unassigned_buttons = true;

        // Mupen's alternate map translates these RetroPad slots to the
        // final N64 functions shown at right.
        s.b = L::CROSS;       // bottom -> N64 A
        s.y = L::SQUARE;      // left   -> N64 B
        s.x = L::TRIANGLE;    // top    -> C-Up
        s.r = L::CIRCLE;      // right  -> C-Right
        s.select = L::L1;     // L1     -> N64 L
        s.l2 = L::L2;         // L2     -> N64 Z
        s.r2 = L::R1;         // R1     -> N64 R
        s.a = L::R2;          // R2     -> C-Down
        s.start = L::START;
```

Leave `s.l` unset so RetroPad L/C-Left remains available only on the right
stick. Keep the existing right-stick, native D-pad, left-stick, and hotkey
assignments unchanged.

- [ ] **Step 7: Run focused tests**

Run:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[mapping][n64][ps_style]"
./build/test_retroarch_unit "[player_binds][n64][ps_style]"
./build/test_retroarch_unit "[player_binds]"
```

Expected: the new layout tests pass. The snapshot test still fails until its
two deliberate golden entries are updated.

- [ ] **Step 8: Update only the two PS-style N64 golden entries**

Run the existing generator:

```bash
./build/test_retroarch_unit "[mapping_snapshot_gen]" \
  > /tmp/mapping_snapshot_gen.txt
```

Copy only the generated `PS|mupen64plus_next_libretro` and
`PS|parallel_n64_libretro` entries into
`tests/retroarch/mapping_snapshot_golden.h`. Their button rows must be:

```text
btn=2,3,4,9,7,0,,1,6,5
```

Do not change any other golden entry. Verify the exact scope:

```bash
git diff -- tests/retroarch/mapping_snapshot_golden.h
```

- [ ] **Step 9: Correct the superseded investigation status**

At the top of
`docs/superpowers/specs/2026-07-29-n64-ps-pad-mapping-status.md`, change the
status and add this correction:

```markdown
**Status:** superseded by the approved universal mapping design

> **2026-07-30 correction:** The apparent duplicate live R2 binding was not
> merely an in-memory RetroArch remap. RetroArch 1.20.0 parses
> `input_playerN_*_btn = ""` as physical button `0`; the shipped serializer
> therefore aliased every empty button slot to the bottom face button. The
> serializer now emits RetroArch's `nul` sentinel. The finished target layout
> is specified in
> `2026-07-30-universal-ps-style-n64-controller-mapping-design.md`.
```

Keep the historical investigation below the correction; it remains useful
context, but the correction must be the first thing future readers see.

- [ ] **Step 10: Run the complete regression suite**

Run:

```bash
git diff --check
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit
```

Expected: all RetroArch unit tests pass, including snapshots and both N64
cores. Review the diff and confirm that non-N64 golden entries are unchanged.

- [ ] **Step 11: Commit the universal layout**

```bash
git add \
  src/retroarch/controller_mapping.cpp \
  src/retroarch/controller_mapping.h \
  tests/retroarch/test_controller_mapping.cpp \
  tests/retroarch/test_ps1_analog_binds.cpp \
  tests/retroarch/test_player_binds.cpp \
  tests/retroarch/mapping_snapshot_golden.h \
  docs/superpowers/specs/2026-07-29-n64-ps-pad-mapping-status.md
git commit -m "feat(n64): add universal PS-style controller layout"
```

---

### Task 3: Deploy and verify on `magicpi5`

**Files:**
- Deploy: the committed C++ source and tests through `scripts/deploy_cpp.sh`
- Inspect: `/tmp/retroarch_mdb.cfg`
- Inspect: `/tmp/retroarch_core_options.cfg`

**Interfaces:**
- Consumes: the committed universal semantic mapping and the captured
  `2563:0526` profile on `magicpi5`.
- Produces: a fresh kiosk binary and live RetroArch config whose physical
  tokens match the approved N64 actions.

- [ ] **Step 1: Verify the local branch immediately before deployment**

Run:

```bash
git status --short
git log -3 --oneline
git diff --check
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit
```

Expected: no uncommitted implementation files, the serializer and universal
layout commits are visible, and the complete suite passes.

- [ ] **Step 2: Deploy and rebuild with the repository's safe restart path**

Run from `magic_dingus_box_cpp/`:

```bash
PI_HOST=magic@magicpi5.local ./scripts/deploy_cpp.sh --build
```

The script must detect the `controller_mapping.h` fingerprint change, perform
a clean Pi build, stop the kiosk and RetroArch with SIGTERM, wait for DRM and
audio teardown, and restart the kiosk only after the new binary is built.

- [ ] **Step 3: Run the complete RetroArch suite on the Pi**

Run:

```bash
ssh magic@magicpi5.local \
  'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && ./test_retroarch_unit'
```

Expected: all RetroArch tests pass on the deployment target.

- [ ] **Step 4: Launch any N64 game and inspect the generated config**

After the user launches an N64 game with the SHANWAN pad as player 1, run:

```bash
ssh magic@magicpi5.local \
  'grep -E "^(input_(enable_hotkey|menu_toggle)_btn|input_player1_(b|y|select|start|a|x|l|r|l2|r2|l3|r3)_btn|input_player1_r_[xy]_(plus|minus)_axis)" /tmp/retroarch_mdb.cfg; grep "^mupen64plus-alt-map" /tmp/retroarch_core_options.cfg'
```

Expected:

```text
input_player1_b_btn = "0"
input_player1_y_btn = "3"
input_player1_select_btn = "6"
input_player1_start_btn = "11"
input_player1_a_btn = "9"
input_player1_x_btn = "4"
input_player1_l_btn = "nul"
input_player1_r_btn = "1"
input_player1_l2_btn = "8"
input_player1_r2_btn = "7"
input_player1_l3_btn = "nul"
input_player1_r3_btn = "nul"
input_player1_r_x_plus_axis = "+2"
input_player1_r_x_minus_axis = "-2"
input_player1_r_y_plus_axis = "+3"
input_player1_r_y_minus_axis = "-3"
input_enable_hotkey_btn = "10"
input_menu_toggle_btn = "11"
mupen64plus-alt-map = "True"
```

Also prove no empty button values remain:

```bash
ssh magic@magicpi5.local \
  '! grep -E "^input_player[12]_.*_btn = \"\"$" /tmp/retroarch_mdb.cfg'
```

Expected: exit code `0` and no output.

- [ ] **Step 5: Perform hands-on acceptance checks**

Use the real controller and record each result:

```text
Super Smash Bros.
[ ] Bottom face button performs N64 A attack without grab/throw.
[ ] R1 performs N64 R grab/throw.
[ ] R2 performs C-Down and does not grab.

Super Mario 64
[ ] Bottom face button jumps.
[ ] Left face button attacks.
[ ] R1 performs the N64 R camera action.
[ ] Right stick reaches all four C directions.

Zelda: Ocarina of Time or Majora's Mask
[ ] L2 performs Z-targeting.
[ ] Top/right/R2 reach C-Up/C-Right/C-Down.
[ ] Right stick reaches all four C-item directions.
[ ] Native D-pad remains available.

RetroArch controls
[ ] Select alone produces no N64 action.
[ ] Start alone reaches N64 Start.
[ ] Select+Start opens the RetroArch menu.
[ ] L3 and R3 produce no N64 action.
```

- [ ] **Step 6: Final evidence check**

Before claiming completion, use `superpowers:verification-before-completion`
and freshly rerun:

```bash
git status --short
./build/test_retroarch_unit
ssh magic@magicpi5.local \
  'systemctl is-active magic-dingus-box-cpp.service; pgrep -a retroarch || true; grep -E "^input_player1_(b|y|select|start|a|x|l|r|l2|r2|l3|r3)_btn" /tmp/retroarch_mdb.cfg'
```

Completion requires all automated checks, the live config inspection, and the
hands-on acceptance checklist—not merely a successful build.
