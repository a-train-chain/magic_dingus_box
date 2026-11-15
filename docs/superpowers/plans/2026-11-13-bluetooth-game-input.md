# Bluetooth game input implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Work in the existing pi5-test worktree; do not spawn further agents.

**Goal:** Make event-only Bluetooth controllers work in games and give menu sticks deliberate, controlled repetition.

**Architecture:** Reuse sysfs input capabilities for event-device discovery and existing mapping/reservation emission. Add a portable menu-repeat helper integrated into the Linux InputManager, with per-device stick state.

**Tech Stack:** C++17, Catch2, libevdev, existing sysfs helpers, CMake; native Mac unit tests and Linux/Pi integration.

**Spec:** docs/superpowers/specs/2026-11-13-bluetooth-game-input-design.md

## Global Constraints

- One runtime implementation and one binary support Pi 4B and Pi 5 on Debian Trixie; no board compile switches or new system dependencies.
- Keep N64/Dreamcast gated off on Pi 4; exercise a supported core there if hardware is reachable.
- Preserve captured/builtin/automatic mapping precedence and physical controller reservations.
- Do not change RetroArch gameplay axes or the established D-pad behavior.
- Preserve unrelated changes, content, saves and settings. Deployment is authorized. Do not publish a release or push unrelated work.
- Sol 6.1 implements and tests; Astra is reserved for difficult integration review.

## Review Focus

- Devices with no js alias must get the same mapping and reservation as their event-capable wired equivalents.
- IMUs, touchpads and the phone remote cannot consume player slots.
- Same-model controllers must remain distinct and ordering must be deterministic.
- Analog report frequency and jitter must not multiply menu steps; holding a perfectly still stick must still repeat.
- Disconnect, input capture and neutral transitions must stop repeating without interfering with another pad or the D-pad.

### Task 1: Detect gamepads from event devices

**Files:** src/retroarch/controller_detector.{h,cpp}, src/platform/input_device_info.{h,cpp} if needed, tests/retroarch/test_controller_detector.cpp, affected CMake targets/documentation under magic_dingus_box_cpp.

**Interfaces:** Preserve no-argument detect_connected_controllers() and detect_primary_controller(). Permit an internal or explicit filesystem-root seam to test the actual scanner against fixture trees. Existing DetectedPad metadata feeds resolve_port_mappings and write_port_reservations unchanged.

- [ ] Read the AGENTS instructions and relevant existing tests. Establish a baseline for affected suites.
- [ ] Write a failing scanner regression with only event nodes: phone remote, 057e:2009 Pro Controller and matching IMU; expect exactly one detected pad with nintendo driver and real capabilities. Also cover touch/digitizer exclusion, no alias duplication, two identical pads, mixed USB/Bluetooth, event2/event10 ordering, empty/missing/unreadable nodes.
- [ ] Implement event enumeration and capability classification without opening/grabbing devices. Reuse the classification shared with the kiosk; avoid new board-specific branches. Ensure primary detection uses the same inventory. Update misleading js-node comments.
- [ ] Prove the detected pad resolves to the automatic N64 mapping with reservation 057e:2009; preserve known wired mapping behavior and two-pad semantics.
- [ ] Run affected suites, record red/green evidence and commit only Task 1 plus the approved plan/spec.

### Task 2: Control menu-stick repetition

**Files:** src/platform/input_manager.{h,cpp}, a focused portable menu-stick helper, tests/platform/test_menu_stick_navigation.cpp, CMake source lists as needed.

**Interfaces:** Integrate the helper into real InputManager polling. Keep analog state owned by each Device, separate from the existing D-pad state. Helper receives axis min/max, raw value and a monotonic timestamp, and supports poll/reset without fresh reports.

- [ ] Write failing behavior tests for one initial step from many reports, no repeat before 400 ms, the existing slow/fast D-pad cadence after the delay, neutral/reversal, threshold jitter, and signed/unsigned ranges. Exercise independent devices and reset.
- [ ] Use normalized engage/release thresholds with hysteresis (roughly 50% engage and 35% release). Apply consistent repetition to horizontal and vertical menu stick movement including overlays; retain the legacy 8-bit D-pad-on-axis behavior where required. Avoid generating duplicate events through both overlay and fallback.
- [ ] Reset stick state at capture transitions, removal, and teardown. Integrate scheduled repeats into each poll so unchanged held sticks repeat. Keep physical D-pad navigation and game axes unchanged.
- [ ] Run affected suites, compile-check the real Linux path, record red/green evidence and commit Task 2.

### Task 3: Verify, deploy and test

**Files:** Only focused hardware regression tools/tests if needed; no production changes unless a diagnosed regression requires them.

- [ ] Run all Mac suites; build the aarch64 Linux kiosk using the existing pisim image if available or a safe on-Pi build at -j2.
- [ ] Review Tasks 1 and 2 and resolve actionable findings before deployment. Confirm final git diff.
- [ ] Discover reachable Pi 4 hardware using known project/SSH/mDNS inventory. Do not claim a Pi 5 policy override is a physical Pi 4 test.
- [ ] Deploy the reviewed binary to magic@10.0.0.227, retaining the prior binary. Preserve unrelated live source changes and settings. Observe service health.
- [ ] Verify live controller enumeration, generated controller mapping/reservation, N64 launch/return on Pi 5, supported-core behavior and synthetic menu repetition through the actual input pipeline where feasible. Coordinate real stick/button acceptance with the owner if necessary.
- [ ] Validate the same artifact on Pi 4 if reachable and authorized by the discovered identity; otherwise document the hardware limitation and dual-board test evidence clearly. Save concise results and commit any focused test tools.
