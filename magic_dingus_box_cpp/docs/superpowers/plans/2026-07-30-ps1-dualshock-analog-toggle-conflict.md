# PS1 DualShock Analog Toggle Conflict Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent PCSX-ReARMed from disabling the emulated DualShock analog mode through the N64 controller's L+R+C-Up inputs, then recover Metal Gear Solid from its persisted analog-off auto-state without touching memory-card data.

**Architecture:** Extend the existing pure PCSX core-options writer with one explicit `pcsx_rearmed_analog_combo = "disabled"` line and pin it in the existing launch-contract test. After review, deploy in the established build/test/restart order, move exactly one Metal Gear Solid auto-state to an explicit recoverable backup, and validate the fresh live PS1 configuration and controller behavior before resuming Dreamcast acceptance.

**Tech Stack:** C++17, Catch2 v3, CMake/CTest, RetroArch 1.20.0, PCSX-ReARMed, Bash, systemd, SSH, Raspberry Pi 5

## Global Constraints

- Apply the new option only to PCSX-ReARMed core-option output.
- Keep `--device 1:517 --device 2:517` unchanged.
- Keep every PS1 face, shoulder, D-pad, analog-axis, and Z+Start mapping unchanged.
- Do not change N64, Dreamcast, or any other console mapping.
- Preserve `/opt/magic_dingus_box/config/controller_profiles.json` byte-for-byte.
- Preserve the Metal Gear Solid memory-card save byte-for-byte.
- Move the single Metal Gear Solid `.state.auto` to its explicit backup path; do not delete or overwrite it.
- Do not use recursive commands, broad globs, or touch any other game's state.

---

### Task 1: Disable the PCSX DualShock analog toggle

**Files:**

- Modify: `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp:217-250`
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp:641-685`

**Interfaces:**

- Consumes: `retroarch::write_core_options(std::ostream&, const std::string&, const std::string&)`
- Produces: PCSX-ReARMed core-option output containing exactly `pcsx_rearmed_analog_combo = "disabled"`

- [ ] **Step 1: Add the failing regression assertion**

In `TEST_CASE("PS1 core disables frame skipping and preserves native performance options", ...)`, add this assertion after the two absent `pad1type`/`pad2type` checks:

```cpp
require_line(config, "pcsx_rearmed_analog_combo = \"disabled\"");
```

The production change that will make this test pass is adding the same exact
line to the PCSX branch of `write_core_options()`.

- [ ] **Step 2: Build the focused target and verify RED**

From `magic_dingus_box_cpp`, run:

```bash
cmake --build build -j4 --target test_retroarch_unit
./build/test_retroarch_unit \
  "PS1 core disables frame skipping and preserves native performance options"
```

Expected: the test fails because the generated config does not contain
`pcsx_rearmed_analog_combo = "disabled"`. It must not fail from a compile
error or an unrelated assertion.

- [ ] **Step 3: Add the minimal production option**

In the PCSX branch of `write_core_options()`, immediately after resolving the
title override and before the performance options, add:

```cpp
// This controller has no dedicated DualShock Analog button. PCSX's default
// L1+R1+Select toggle overlaps the former menu chord and can persist ANALOG
// OFF in auto-states, so keep DualShock mode under the core/game's control.
out << "pcsx_rearmed_analog_combo = \"disabled\"\n";
```

Do not alter `core_input_device_args()`, controller mappings, or any other
core option.

- [ ] **Step 4: Verify GREEN and regression scope**

Run:

```bash
cmake --build build -j4 --target test_retroarch_unit
./build/test_retroarch_unit \
  "PS1 core disables frame skipping and preserves native performance options"
./build/test_retroarch_unit "[retroarch]"
cmake --build build -j4
ctest --test-dir build --output-on-failure
git diff --check
```

Expected:

- focused PS1 contract test passes;
- full RetroArch suite passes with 2,024 assertions or more;
- full build passes;
- local CTest passes 8/8; and
- the diff contains only the one test assertion and one PCSX option/comment.

- [ ] **Step 5: Commit the reviewed implementation**

```bash
git add \
  magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp \
  magic_dingus_box_cpp/src/retroarch/launch_contract.cpp
git commit -m "fix(ps1): prevent accidental analog mode toggle"
```

Record the commit SHA and independent review result before deployment.

---

### Task 2: Deploy, recover the auto-state, and validate PS1

**Files:**

- Read: `/opt/magic_dingus_box/config/controller_profiles.json`
- Read: `/tmp/retroarch_mdb.cfg`
- Read: `/tmp/retroarch_core_options.cfg`
- Move: `/opt/magic_dingus_box/magic_dingus_box_cpp/data/states/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).state.auto`
- Create by move: `/opt/magic_dingus_box/magic_dingus_box_cpp/data/states/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).state.auto.backup-20260730T115845-analog-off`
- Preserve: `/opt/magic_dingus_box/magic_dingus_box_cpp/data/saves/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).srm`
- Report: `.superpowers/sdd/2026-07-30-ps1-dualshock-analog-toggle-conflict/task-2-report.md`

**Interfaces:**

- Consumes: reviewed Task 1 commit, saved controller profile, Metal Gear Solid auto-state and memory-card save
- Produces: active kiosk running the post-link binary, recoverable analog-off state backup, byte-identical memory-card save, fresh PCSX options with the toggle disabled, and hands-on PS1 acceptance

- [ ] **Step 1: Record local and Pi invariants**

From `magic_dingus_box_cpp`, run:

```bash
git status --short --branch
git rev-parse HEAD
cmake --build build -j4
ctest --test-dir build --output-on-failure
ssh -o BatchMode=yes magic@magicpi5.local '
set -eu
test -z "$(pgrep -x retroarch || true)"
sha256sum /opt/magic_dingus_box/config/controller_profiles.json
stat -c "%U:%G %a" /opt/magic_dingus_box/config/controller_profiles.json
sha256sum "/opt/magic_dingus_box/magic_dingus_box_cpp/data/states/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).state.auto"
sha256sum "/opt/magic_dingus_box/magic_dingus_box_cpp/data/saves/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).srm"
systemctl is-active magic-dingus-box-cpp.service
'
```

Expected pre-state:

```text
controller profile:
12a40ed090e3ebd900cded14a6be324beb9cd7136bfd678f756c24f5d8399ddd
magic:magic 0644

auto-state:
acb2d1749f74192106d51560a942d236bddedffd3704c986e0ccaab1a8b02859

memory card:
4849179b5fffcfe4398408f68ce6f500b703cf333c9418ef37d4719ff1680d79
```

Stop if RetroArch is running or any checksum differs; resolve the exact live
state before continuing.

- [ ] **Step 2: Deploy, build, test, and restart in guaranteed order**

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

Expected: Pi CTest passes 9/9, service is active, and the running and on-disk
binary hashes are identical. The final restart must occur only after build
and CTest succeed.

- [ ] **Step 3: Move exactly one auto-state to its recoverable backup**

Run this single explicit move while RetroArch is not running:

```bash
ssh -o BatchMode=yes magic@magicpi5.local '
set -eu
test -z "$(pgrep -x retroarch || true)"
state="/opt/magic_dingus_box/magic_dingus_box_cpp/data/states/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).state.auto"
backup="/opt/magic_dingus_box/magic_dingus_box_cpp/data/states/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).state.auto.backup-20260730T115845-analog-off"
printf "source=%s\nbackup=%s\n" "$state" "$backup"
test -f "$state"
test ! -e "$backup"
test "$(sha256sum "$state" | cut -d " " -f 1)" = \
  "acb2d1749f74192106d51560a942d236bddedffd3704c986e0ccaab1a8b02859"
mv -- "$state" "$backup"
test ! -e "$state"
test -f "$backup"
sha256sum "$backup"
stat -c "%n | %s | %U:%G %a | %y" "$backup"
'
```

Expected: only the exact source auto-state moves, and the backup retains SHA
`acb2d1749f74192106d51560a942d236bddedffd3704c986e0ccaab1a8b02859`.
This is recoverable by moving the backup to the original path.

- [ ] **Step 4: Verify protected files after deployment and move**

Run:

```bash
ssh -o BatchMode=yes magic@magicpi5.local '
set -eu
sha256sum /opt/magic_dingus_box/config/controller_profiles.json
stat -c "%U:%G %a" /opt/magic_dingus_box/config/controller_profiles.json
sha256sum "/opt/magic_dingus_box/magic_dingus_box_cpp/data/saves/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).srm"
'
```

Expected: controller profile SHA, owner, and mode match Step 1 exactly; the
memory-card SHA remains
`4849179b5fffcfe4398408f68ce6f500b703cf333c9418ef37d4719ff1680d79`.

- [ ] **Step 5: Request a fresh Metal Gear Solid launch and inspect it**

Ask the user to launch `Metal Gear Solid (USA) (v1.1).m3u` and leave it
running. Then run:

```bash
ssh -o BatchMode=yes magic@magicpi5.local '
set -eu
pgrep -a retroarch
grep -Fx "pcsx_rearmed_analog_combo = \"disabled\"" \
  /tmp/retroarch_core_options.cfg
grep -E "^input_player1_(b|y|a|x|l|r|l2|r2|select|start)_btn|^input_player1_l_[xy]_(plus|minus)_axis|^input_enable_hotkey_btn|^input_menu_toggle_btn|^input_exit_emulator_btn" \
  /tmp/retroarch_mdb.cfg
'
```

Expected process arguments include:

```text
--device 1:517 --device 2:517
```

Expected live binds:

```text
b/y/a/x = 1/2/0/3
l/r/l2/r2/select/start = 4/5/6/8/9/12
left stick = +0/-0/+1/-1
input_enable_hotkey_btn = "6"
input_menu_toggle_btn = "12"
input_exit_emulator_btn absent
```

- [ ] **Step 6: Complete hands-on PS1 acceptance**

Ask the user to confirm:

- the physical analog stick moves Snake;
- Z alone performs PS1 L2;
- Start works normally;
- Z+Start opens the RetroArch menu; and
- face, shoulder, and D-pad controls remain correct.

Stop on any failure. Do not proceed to Dreamcast until the PS1 regression is
resolved.

- [ ] **Step 7: Run the final automated and live invariant gate**

After the user closes Metal Gear Solid, run:

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
sha256sum /opt/magic_dingus_box/config/controller_profiles.json
sha256sum "/opt/magic_dingus_box/magic_dingus_box_cpp/data/saves/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).srm"
sha256sum "/opt/magic_dingus_box/magic_dingus_box_cpp/data/states/ps1/PCSX-ReARMed/Metal Gear Solid (USA) (v1.1).state.auto.backup-20260730T115845-analog-off"
'
```

Expected: local 8/8 and Pi 9/9 CTest pass, the service is active, running and
disk binary hashes match, and all three protected checksums remain unchanged.

Only after this gate should the universal-controller plan resume at Dreamcast
hands-on validation.
