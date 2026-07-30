# Universal N64 Controller RetroArch Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make physical Z+Start open the RetroArch menu for an N64-style
controller on every core without changing either button's normal game mapping.

**Architecture:** Define Z+Start once in the N64-style semantic preamble so
all current and future core branches inherit it. Keep each core's game slots
unchanged, including PS1 Z→L2 and Start→Start. Extract the launcher's existing
hotkey text emission into a pure tested writer so exact serialization and the
absence of an exit bind are mechanically verified.

**Tech Stack:** C++17, Catch2, CMake/CTest, RetroArch 1.20, libretro cores,
systemd, Raspberry Pi OS.

## Global Constraints

- Every N64-style controller mapping uses physical Z as hotkey enable and
  physical Start as menu toggle.
- Z alone and Start alone retain their existing per-core game functions.
- PS1 keeps physical Z mapped to L2 and physical Start mapped to Start while
  also using those same physical tokens for the menu chord.
- Do not change any face, shoulder, trigger, D-pad, analog-stick, Select,
  Coin, turbo, or native N64 game mapping.
- Do not add an exit-emulator bind, change
  `input_menu_toggle_gamepad_combo = "3"`, create per-ROM remaps, or consume
  another controller button.
- PS-style controller hotkeys remain Select+Start.
- Existing N64 and Dreamcast mappings remain byte-for-byte identical.
- Live profile `controller_profiles.json` must remain byte-for-byte unchanged.
- Deployment must restart the managed kiosk only after the corrected binary
  finishes linking, and `/proc/<MainPID>/exe` must hash-match the disk binary.
- Live completion requires PS1 L2-alone plus Z+Start validation and Dreamcast
  controls plus Z+Start validation.

---

### Task 1: Make Z+Start an N64-style invariant

**Files:**

- Modify: `magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_mapping.h`
- Modify: `magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp`
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_controller_mapping.cpp`
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_port_resolution.cpp`
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_mapping_kinds.cpp`
- Modify: `magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h`

**Interfaces:**

- Consumes: `SemanticMapping`, `LogicalControl::N64_Z`,
  `LogicalControl::N64_START`, `ControllerMapping`, and physical-profile
  resolution.
- Produces: `void write_hotkey_binds(std::ostream&,
  const ControllerMapping&)`, used by the launcher and focused unit tests.
- Preserves: all existing public mapping and launcher behavior outside
  N64-style hotkey metadata.

- [ ] **Step 1: Write the universal-hotkey and PS1-conflict regressions**

In `tests/retroarch/test_controller_mapping.cpp`, change the three PS1 core
expectations from empty hotkeys to:

```cpp
CHECK(map.enable_hotkey_btn == "6");
CHECK(map.menu_toggle_btn == "12");
```

Keep every existing PS1 game-slot assertion, especially:

```cpp
CHECK(map.l2_btn == "6");
CHECK(map.start_btn == "12");
```

Add a test tagged `[retroarch][mapping][hotkeys][n64_style]`:

```cpp
TEST_CASE("N64-style controllers use physical Z+Start on every core",
          "[retroarch][mapping][hotkeys][n64_style]") {
    const std::vector<std::string> cores = {
        "nestopia_libretro",
        "fceumm_libretro",
        "snes9x2010_libretro",
        "genesis_plus_gx_libretro",
        "pcsx_rearmed_libretro",
        "beetle_psx_libretro",
        "swanstation_libretro",
        "mednafen_pce_fast_libretro",
        "prosystem_libretro",
        "fbneo_libretro",
        "mupen64plus_next_libretro",
        "parallel_n64_libretro",
        "flycast_libretro",
        "totally_unknown_core",
    };

    for (const auto& core : cores) {
        INFO("core=" << core);
        const auto map =
            get_mapping(ControllerType::N64_ADAPTER, core);
        CHECK(map.enable_hotkey_btn == "6");
        CHECK(map.menu_toggle_btn == "12");
        CHECK(map.exit_emulator_btn.empty());
    }
}
```

In `tests/retroarch/test_port_resolution.cpp`, add these assertions to the
persisted `2563:0575` PS1 mapping without changing its game controls:

```cpp
REQUIRE(ps1.l2_btn == "6");
REQUIRE(ps1.start_btn == "12");
REQUIRE(ps1.enable_hotkey_btn == "6");
REQUIRE(ps1.menu_toggle_btn == "12");
REQUIRE(ps1.exit_emulator_btn.empty());
```

In `tests/retroarch/test_mapping_kinds.cpp`, change the N64-style PS1 mapping
expectations from empty hotkeys to tokens `6` and `12`. Leave the PS-style
hotkey assertions at Select+Start.

- [ ] **Step 2: Run focused tests and verify RED**

From `magic_dingus_box_cpp`:

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][hotkeys][n64_style]"
./build/test_retroarch_unit "[retroarch][mapping][ps1][n64_style]"
./build/test_retroarch_unit "[retroarch][wizard_profile][cross_layer]"
./build/test_retroarch_unit "[build_mapping][kinds]"
```

Expected: failures show PS1 and `totally_unknown_core` have empty hotkeys.
All pre-existing game-slot assertions continue to pass.

- [ ] **Step 3: Implement the minimal semantic invariant**

At the beginning of `semantic_n64_style()` in
`src/retroarch/controller_mapping.cpp`, immediately after
`SemanticMapping s;`, add:

```cpp
s.hotkey_enable = L::N64_Z;
s.menu_toggle = L::N64_START;
```

Delete the local `hotkeys` lambda. Remove every branch-local `hotkeys()` call
without changing any neighboring game mapping, stick, D-pad, or core-option
statement.

Update the function's leading comment to say Z+Start is universal for all
N64-style core mappings, including PS1.

- [ ] **Step 4: Run focused tests and verify GREEN**

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][hotkeys][n64_style]"
./build/test_retroarch_unit "[retroarch][mapping][ps1][n64_style]"
./build/test_retroarch_unit "[retroarch][wizard_profile][cross_layer]"
./build/test_retroarch_unit "[build_mapping][kinds]"
```

Expected: all pass. PS1's `l2_btn` and `enable_hotkey_btn` both resolve to
physical Z, and `start_btn` and `menu_toggle_btn` both resolve to physical
Start.

- [ ] **Step 5: Write the hotkey-serialization regression**

Declare this interface in `src/retroarch/controller_mapping.h`:

```cpp
void write_hotkey_binds(std::ostream& out,
                        const ControllerMapping& map);
```

Before implementing it, add this test to
`tests/retroarch/test_controller_mapping.cpp`:

```cpp
TEST_CASE("N64-style hotkeys serialize once as a menu chord, never exit",
          "[retroarch][mapping][hotkeys][config]") {
    const auto map = get_mapping(
        ControllerType::N64_ADAPTER, "pcsx_rearmed_libretro");
    std::ostringstream out;
    retroarch::write_hotkey_binds(out, map);
    CHECK(out.str() ==
          "input_enable_hotkey_btn = \"6\"\n"
          "input_menu_toggle_btn = \"12\"\n");
    CHECK(out.str().find("input_exit_emulator_btn") ==
          std::string::npos);
}
```

- [ ] **Step 6: Run the serialization test and verify RED**

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][hotkeys][config]"
```

Expected: the test cannot link until `write_hotkey_binds()` is implemented.
The missing symbol is the intended RED for this extracted interface.

- [ ] **Step 7: Implement and use the pure hotkey writer**

In `src/retroarch/controller_mapping.cpp`, add:

```cpp
void write_hotkey_binds(std::ostream& out,
                        const ControllerMapping& map) {
    if (map.enable_hotkey_btn.empty()) return;

    out << "input_enable_hotkey_btn = \""
        << map.enable_hotkey_btn << "\"\n";
    if (!map.menu_toggle_btn.empty()) {
        out << "input_menu_toggle_btn = \""
            << map.menu_toggle_btn << "\"\n";
    }
    if (!map.exit_emulator_btn.empty()) {
        out << "input_exit_emulator_btn = \""
            << map.exit_emulator_btn << "\"\n";
    }
}
```

In `src/retroarch/retroarch_launcher.cpp`, replace only the existing
player-one hotkey-emission `if` block with:

```cpp
write_hotkey_binds(script_file, map);
```

Do not call it for `map_p2`. This preserves player-one-only menu ownership
and writes the two global hotkey lines exactly once.

- [ ] **Step 8: Run serialization and isolation tests**

```bash
cmake --build build --target test_retroarch_unit -j4
./build/test_retroarch_unit "[retroarch][mapping][hotkeys]"
./build/test_retroarch_unit "[retroarch][mapping][dreamcast][n64_style]"
./build/test_retroarch_unit "[retroarch][mapping][n64]"
./build/test_retroarch_unit "[retroarch][mapping][n64][ps_style]"
./build/test_retroarch_unit "[ps1_analog][player_binds][n64][ps_style]"
```

Expected: all pass. Dreamcast and native N64 retain their exact existing
game mappings and Z+Start chord; PS-style N64 remains Select+Start.

- [ ] **Step 9: Update only four intentional snapshots**

Run:

```bash
./build/test_retroarch_unit "[mapping_snapshot_gen]" \
  > /tmp/universal-z-start-golden.txt
```

In `tests/retroarch/mapping_snapshot_golden.h`, change only:

- `N64|pcsx_rearmed_libretro`
- `N64|totally_unknown_core`
- `UNKNOWN|pcsx_rearmed_libretro`
- `UNKNOWN|totally_unknown_core`

For each, replace:

```text
hotkeys=,,
```

with:

```text
hotkeys=6,12,
```

Compare all four with the generator output. Do not change any other line or
golden entry.

- [ ] **Step 10: Run the complete verification gate**

```bash
./build/test_retroarch_unit "[mapping_snapshot]"
./build/test_retroarch_unit
cmake --build build -j4
ctest --test-dir build --output-on-failure
git diff --check
git diff --stat
```

Expected: all 33 snapshots pass, the full RetroArch suite passes, full build
succeeds, CTest reports 8/8, and only the seven listed files changed.

- [ ] **Step 11: Self-review and commit**

Confirm from the diff:

- no per-core game slot changed;
- the N64/Dreamcast snapshot entries are unchanged;
- the PS-style snapshot entries are unchanged;
- the launcher calls `write_hotkey_binds()` exactly once;
- no `input_exit_emulator_btn` is emitted for any N64-style mapping; and
- only the four approved snapshot keys changed.

Commit:

```bash
git add magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp \
  magic_dingus_box_cpp/src/retroarch/controller_mapping.h \
  magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_controller_mapping.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_port_resolution.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_mapping_kinds.cpp \
  magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h
git commit -m "feat(retroarch): make Z Start the universal N64-pad menu chord"
```

### Task 2: Deploy and validate PS1 and Dreamcast

**Files:**

- Read: `/opt/magic_dingus_box/config/controller_profiles.json`
- Read: `/tmp/retroarch_mdb.cfg`
- Read: `/tmp/retroarch_core_options.cfg`
- Write through deployment: `/opt/magic_dingus_box/magic_dingus_box_cpp`
- Report: SDD workspace `task-2-report.md`

**Interfaces:**

- Consumes: reviewed Task 1 commit and live `2563:0575` saved profile.
- Produces: active kiosk running the post-link binary, PS1 configuration
  evidence, Dreamcast configuration evidence, and hands-on acceptance.

- [ ] **Step 1: Record source, profile, and service pre-state**

From `magic_dingus_box_cpp`:

```bash
git status --short --branch
git rev-parse HEAD
cmake --build build -j4
ctest --test-dir build --output-on-failure
ssh -o BatchMode=yes magic@magicpi5.local '
sha256sum /opt/magic_dingus_box/config/controller_profiles.json
stat -c "%U:%G %a" /opt/magic_dingus_box/config/controller_profiles.json
systemctl is-active magic-dingus-box-cpp.service
'
```

Expected: reviewed Task 1 HEAD, clean worktree, local CTest 8/8, profile
`magic:magic 0644`, and active service. Record the live profile checksum as
the post-deployment byte-for-byte invariant.

- [ ] **Step 2: Sync, build, test, and restart in a guaranteed order**

Do not use `deploy_cpp.sh --test`. Run:

```bash
PI_HOST=magic@magicpi5.local ./scripts/deploy_cpp.sh
ssh -o BatchMode=yes magic@magicpi5.local '
set -eu
cd /opt/magic_dingus_box/magic_dingus_box_cpp/build
cmake --build . -j4
ctest --output-on-failure
sudo systemctl restart magic-dingus-box-cpp.service
systemctl is-active magic-dingus-box-cpp.service
pid=$(systemctl show -p MainPID --value magic-dingus-box-cpp.service)
sha256sum "/proc/$pid/exe" \
  /opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp
'
```

Expected: Pi CTest 9/9, service active, and the two binary hashes identical.
The final restart occurs in the same `set -e` command only after build and
CTest succeed, preventing the previously observed stale mapped executable.

- [ ] **Step 3: Verify profile preservation**

Repeat the checksum and owner/mode command from Step 1. Expected: exact
checksum, owner, and mode match. Confirm the live `2563:0575` Z/Start tokens
remain `6` and `12`.

- [ ] **Step 4: Request Metal Gear Solid launch and inspect PS1 config**

Ask the user to launch `Metal Gear Solid (USA) (v1.1).m3u` and leave it
running. Then inspect only the fresh config:

```bash
ssh -o BatchMode=yes magic@magicpi5.local '
pgrep -a retroarch
stat -c "%n | %s | %y" /tmp/retroarch_mdb.cfg \
  /tmp/retroarch_core_options.cfg
grep -E "^input_player1_(b|y|a|x|l|r|l2|r2|select|start)_btn|^input_enable_hotkey_btn|^input_menu_toggle_btn|^input_exit_emulator_btn" \
  /tmp/retroarch_mdb.cfg
'
```

Expected live-profile PS1 values:

```text
b/y/a/x = 1/2/0/3
l/r/l2/r2/select/start = 4/5/6/8/9/12
input_enable_hotkey_btn = "6"
input_menu_toggle_btn = "12"
input_exit_emulator_btn absent
```

- [ ] **Step 5: Request PS1 hands-on conflict check**

Ask the user to confirm in Metal Gear Solid:

- physical Z performs PS1 L2 when pressed alone;
- Start works normally;
- Z+Start opens the RetroArch menu; and
- existing face, shoulder, D-pad, and analog controls remain correct.

Stop on any L2 conflict; do not proceed to Dreamcast until resolved.

- [ ] **Step 6: Request Sonic Adventure launch and inspect Dreamcast config**

Ask the user to close PS1, launch `Sonic Adventure`, and leave it running.
Inspect:

```bash
ssh -o BatchMode=yes magic@magicpi5.local '
pgrep -a retroarch
stat -c "%n | %s | %y" /tmp/retroarch_mdb.cfg \
  /tmp/retroarch_core_options.cfg
grep -E "^input_player1_(b|y|a|x|l2|r2|start|up|down|left|right)_btn|^input_player1_l_[xy]_(plus|minus)_axis|^input_enable_hotkey_btn|^input_menu_toggle_btn|^input_exit_emulator_btn" \
  /tmp/retroarch_mdb.cfg
'
```

Expected live-profile Dreamcast values:

```text
b/y/a/x = 1/2/0/3
l2/r2/start = 4/5/12
D-pad = h0up/h0down/h0left/h0right
left stick = +0/-0/+1/-1
input_enable_hotkey_btn = "6"
input_menu_toggle_btn = "12"
input_exit_emulator_btn absent
```

- [ ] **Step 7: Request Dreamcast hands-on acceptance**

Ask the user to confirm:

- physical A/B/C-Left/C-Down perform Dreamcast A/X/Y/B;
- L/R perform full digital trigger presses;
- Start, D-pad, and analog stick work;
- Z+Start opens the RetroArch menu.

If trigger behavior is unclear in Sonic Adventure, launch Crazy Taxi and
confirm L/R act as its trigger inputs.

- [ ] **Step 8: Run the final automated gate**

```bash
cmake --build build -j4
ctest --test-dir build --output-on-failure
ssh -o BatchMode=yes magic@magicpi5.local '
set -eu
systemctl is-active magic-dingus-box-cpp.service
cd /opt/magic_dingus_box/magic_dingus_box_cpp/build
ctest --output-on-failure
pid=$(systemctl show -p MainPID --value magic-dingus-box-cpp.service)
sha256sum "/proc/$pid/exe" \
  /opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp
'
```

Expected: local CTest 8/8, Pi CTest 9/9, active service, matching running/disk
binary hashes, and unchanged profile checksum.

Do not claim universal completion until the user confirms both PS1 and
Dreamcast hands-on checks.
