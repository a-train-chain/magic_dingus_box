# Controller Setup Wizard — Design

**Date:** 2026-07-28
**Status:** Approved (design review with Alexander, 2026-07-28)
**Branch:** `feat/controller-config-wizard`

## Problem

Third-party USB controllers number their buttons arbitrarily. The box's controller
detection is a VID/PID whitelist of exactly two devices — the N64-style adapter
(`0e6d:111d`) and the DragonRise PS-style pad (`0079:0006`) in
`src/retroarch/controller_detector.cpp` — and any other pad silently falls through to
the N64 adapter mapping, producing scrambled buttons in games. The kiosk menu's own
button handling (`InputManager::map_button_to_action`) is likewise hardcoded to those
two pads, so an unrecognized pad often cannot navigate the menus either.

## Solution overview

A **Controller Setup** wizard in the Settings menu. The user picks the controller
style they own (PlayStation-style or N64-style), then presses each control as
prompted. The result is a **physical profile** stored per controller model (USB
VID/PID) that:

1. drives RetroArch button binds for every core at every game launch, per port
   (player 1 and player 2 resolved independently by each port's connected model), and
2. makes the pad navigate the kiosk menus.

A controller model needs the wizard exactly once; the profile auto-applies whenever
that model is plugged in, and survives OTA updates.

## Goals

- Any USB gamepad can be made fully functional (games + menus) via a guided,
  TV-friendly wizard with no keyboard or SSH.
- Zero behavior change for the two shipped pads out of the box — enforced by a
  snapshot regression gate (see Testing).
- All new decision logic unit-testable on the Mac.

## Non-goals (deliberate scope exclusions)

- Per-game mapping overrides.
- Analog deadzone / sensitivity tuning UI.
- Web-admin profile management or editing.
- Hotkey rebinding UI (hotkeys derive from semantic tables, as today).
- More than 2 players.
- A third "simple pad" wizard type — odd pads (e.g. stickless SNES-style) are handled
  by skippable steps within the PS-style flow.

## Architecture: three layers

Today each per-core table in `controller_mapping.cpp` fuses two concerns:
*which physical control drives which RetroPad slot for this core* (validated,
hand-tuned semantics) and *that control's button/axis number on one specific pad*.
The refactor separates them:

### Layer 1 — Semantic tables (per core, per style)

Maps RetroPad slots to `LogicalControl` values: "RetroPad B ← CROSS",
"RetroPad R2 ← N64_Z". One table set per `ControllerStyle` (`PS_STYLE`, `N64_STYLE`),
extracted 1:1 from the existing per-core branches of `get_mapping_ps_style()` and
`get_mapping_n64_adapter()`, **including** core options metadata
(`analog_dpad_mode`, `core_option_pad_type`, `extra_config`) and hotkey assignments
(enable-hotkey, menu-toggle, exit-emulator), which become `LogicalControl`-valued.

### Layer 2 — Physical profiles (per controller model)

`PhysicalProfile`: for each `LogicalControl`, the physical binding on a concrete pad —
its evdev identity (for kiosk menu use) and its RetroArch udev bind token (for config
emission). The two shipped pads become **built-in profiles** (their tokens transcribed
from the current tables' physical comments). The wizard produces **captured profiles**
keyed by `"vvvv:pppp"` VID/PID.

Precedence at resolution time: **captured profile → built-in profile → legacy
fallback** (today's UNKNOWN→N64-table behavior, preserved unchanged). Captured wins
over built-in so a clone pad that reuses a known VID/PID with different wiring can be
corrected by the operator.

### Layer 3 — Combiner and emission

`build_mapping(semantic_table, physical_profile) → ControllerMapping`. The existing
`ControllerMapping` struct remains the emission contract; `get_mapping(ControllerType,
core_name)` survives as a thin wrapper (builtin profile + semantic table → combiner)
so existing call sites and tests keep working. Controls absent from a profile
(skipped in the wizard) produce empty tokens, and emission skips empty binds.

The player-bind emission block currently hand-duplicated for P1/P2 in
`retroarch_launcher.cpp` (~lines 560–637) moves into a Mac-testable ostream emitter
`write_player_binds(std::ostream&, const ControllerMapping&, int player)` in
`controller_mapping.cpp`, alongside the existing `write_right_stick_binds()`. The
launcher calls it once per player with that port's mapping. This removes the
duplicated mirror block that previously caused a real P2 bug.

## New components

| Component | Contents |
|---|---|
| `src/retroarch/logical_controls.h` | `ControllerStyle` enum; `LogicalControl` enum with distinct PS vocabulary (DPAD_UP/DOWN/LEFT/RIGHT, CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1, L2, R2, L3, R3, SELECT, START, LSTICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT) and N64 vocabulary (N64_A, N64_B, N64_START, N64_Z, N64_L, N64_R, N64_C_UP/DOWN/LEFT/RIGHT, N64_DPAD_UP/DOWN/LEFT/RIGHT, N64_STICK_UP/DOWN/LEFT/RIGHT). No punning between vocabularies. |
| `src/retroarch/controller_profile.{h,cpp}` | `PhysicalProfile`; JSON (de)serialization as pure string-in/string-out; built-in profiles for the two shipped pads; profile-store load/save with atomic temp+rename write. |
| `src/retroarch/joydev_index.{h,cpp}` | Pure conversion: device's ordered EV_KEY capability list + ABS axis list → RetroArch udev bind token for a given evdev code/axis direction (`"5"`, `"h0up"`, `"+2"`, `"-3"`). Implements the joystick-API button ordering (`BTN_JOYSTICK`..`KEY_MAX` first, then `BTN_MISC`..`BTN_JOYSTICK−1`); hats tokenized as `hN…`, non-hat ABS codes indexed in ascending order. **Must be verified against RetroArch's udev driver — see Validation.** |
| `src/retroarch/capture_session.{h,cpp}` | Wizard state machine, zero I/O (see Capture rules). |
| `src/ui/controller_wizard.{h,cpp}` + `src/ui/controller_wizard_renderer.cpp` | Wizard UI state + full-screen overlay renderer, following the PairingScreen pattern (private `Renderer` method in its own .cpp). |
| `config/controller_profiles.json` | Captured profile store. Under `config/*`, so preserved across OTA with no `update.sh` changes. |

### Profile store schema

```json
{
  "version": 1,
  "profiles": {
    "0810:e501": {
      "name": "Twin USB Joystick",
      "style": "ps_style",
      "captured_at": "2026-07-28T21:00:00Z",
      "controls": {
        "cross":    { "kind": "button", "code": 289, "token": "1" },
        "dpad_up":  { "kind": "hat",    "code": 17, "direction": -1, "token": "h0up" },
        "lstick_up":{ "kind": "axis",   "code": 1,  "direction": -1, "token": "-1" }
      }
    }
  }
}
```

`kind` ∈ `button|hat|axis`; `code` is the evdev code (`EV_KEY` code for buttons,
`ABS_*` code for hats/axes); `direction` ∈ `{-1, +1}` for hats/axes; `token` is the
RetroArch udev bind token computed at capture time by `joydev_index`. Unknown keys are
ignored on load; a malformed file is logged and treated as empty (built-ins unaffected).

## Wizard UX flow

Entry: new top-level Settings row **"Controller Setup"** (`MenuSection::CONTROLLER_SETUP`),
dispatched from main.cpp's SELECT handler like Phone Remote; full-screen overlay
replaces the settings panel; a main.cpp interception block consumes all other input
while active.

1. **Pick target** — "Press any button on the controller you want to set up." The
   first button press from any eligible joystick selects the target device and shows
   its name. Phone-remote virtual pad, rotary encoder, and keyboards are never
   eligible targets. No joystick connected → instructional text, not an error.
2. **Pick style** — PlayStation-style vs N64-style. Navigable via box hardware
   buttons, rotary, phone remote, or any *other* already-working pad — never assumes
   the target pad can navigate.
3. **Capture steps** — one control at a time ("Press **Cross** (bottom face
   button)", "Move the **analog stick UP**"), with a progress list of all steps.
   Step order: d-pad, face buttons, shoulders/triggers, Start/Select (PS), sticks /
   stick clicks (PS) / C-buttons and Z (N64). During a capture step the target pad's
   input is capture data; box buttons + rotary drive the wizard chrome: **Skip**,
   **Redo last**, **Cancel** (footer hints rendered on screen).
4. **Test screen** — all captured controls listed; live input from the target pad
   lights up the matching label. Save / Redo / Cancel.
5. **Save** — atomic write to the profile store, toast confirmation, return to
   Settings. Effects are immediate for menu navigation and apply at the next game
   launch. Re-running the wizard for a model overwrites its profile.

**Inactivity:** 2 minutes with no input anywhere auto-cancels the wizard (nothing
saved), so an abandoned wizard cannot trap the kiosk.

### Capture rules (capture_session state machine)

- A button registers on **press + release** of the same code.
- An axis registers when deflection from the step-start rest position exceeds 50% of
  the axis range and then returns toward center. Rest position is sampled at step
  start (handles off-center resting and inverted axes; the observed sign becomes the
  token sign).
- Hat (`ABS_HAT0X/Y`) events register as hat directions.
- **Duplicate rejection:** an input already assigned in this session is rejected with
  "already used for ⟨control⟩" and the step re-prompts.
- **Skip** leaves the control unbound (empty token; no bind emitted; menu-nav entry
  omitted).
- Target device disappearing (unplug, read error) aborts the wizard cleanly with a
  toast.

## Kiosk menu navigation integration

main.cpp loads the profile store at startup (and after each wizard save) and hands
`InputManager` a per-VID/PID overlay mapping derived from each profile's semantics:

| LogicalControl | InputAction |
|---|---|
| CROSS / N64_A / START / N64_START | SELECT |
| CIRCLE / N64_B | SETTINGS_MENU |
| TRIANGLE / N64_Z | PLAY_PAUSE |
| R1 / N64_R | NEXT |
| L1 / N64_L | PREV |
| DPAD_LEFT/RIGHT, LSTICK_LEFT/RIGHT, N64_DPAD_*, N64_STICK_LEFT/RIGHT | ROTATE (navigate) |
| RSTICK_LEFT/RIGHT / N64_C_LEFT/RIGHT | SEEK_LEFT / SEEK_RIGHT |

(Mirrors the semantics currently hardcoded in `map_button_to_action` /
`map_axis_to_action`.)

- Devices are matched to overlays at open time via `libevdev` vendor/product IDs.
- **Overlays are additive**: an event code the overlay doesn't claim falls through to
  the existing hardcoded switch. A bad capture can never make the menu less usable
  than today, and box hardware buttons always still reach Settings for a re-run.
- `InputManager` gains a raw-capture mode for the wizard: while active, the target
  device's events are delivered raw (device id, VID/PID, name, kind, code, value)
  instead of being action-mapped; other devices behave normally. It also exposes
  per-device capability info (ordered button codes; axes with ranges) for
  `joydev_index` and for rest-position calibration.

## Launch-time resolution (per port)

`controller_detector` is generalized from "first recognized device wins" to
enumerate connected joysticks in port order, returning each port's
`{vid, pid, name}`. At launch, for each of port 0 (player 1) and port 1 (player 2):

    captured profile for vid:pid → built-in profile → legacy fallback table

then `build_mapping()` per port and `write_player_binds()` per player with that
port's mapping. Core options that encode pad type stay per-port capable
(`pcsx_rearmed_pad1type` / `pad2type` already exist). Existing
`input_player{1,2}_joypad_index` behavior (0 and 1) is unchanged.

Mixed-model two-player works when each model has a profile (built-in or captured);
the profile follows the controller model, not the port.

## Testing

### Snapshot regression gate — written FIRST, against current code

Before any refactoring: a test that records the exact `ControllerMapping` produced by
today's `get_mapping()` for **all 10 shipped cores × both pads** (every field,
including hotkeys, core options, and extra_config), committed as literal expected
values. The refactored path must reproduce every value identically. This is the
license to restructure the hand-tuned tables (whose N64/DC entries are still marked
unvalidated-on-hardware — the snapshot preserves them bit-for-bit rather than
re-deriving them).

### Mac unit tests (existing `test_retroarch_unit` target; new sources added to its CMake list)

- `capture_session`: PS happy path; N64 happy path; skips (incl. fully stickless
  pad); duplicate rejection; hat vs button vs axis d-pads; inverted axis; off-center
  rest position; target-unplugged abort.
- `joydev_index`: synthetic capability lists, including button codes below
  `BTN_JOYSTICK` (`BTN_MISC` range), hats mixed with axes, missing ABS_X.
- `controller_profile`: JSON round-trip; malformed-file tolerance; unknown-key
  tolerance; built-in profiles produce the legacy tokens.
- Emission: `write_player_binds` output for P1 vs P2; empty-token binds omitted;
  two different profiles → distinct P1/P2 blocks.

### On-Pi validation

- **Numbering cross-check (decisive):** run `joydev_index` against the real N64
  adapter and DragonRise pad; computed tokens must equal the legacy hand-tuned
  tokens. Also verify ordering assumptions against RetroArch's `udev_joypad.c` source
  and a verbose-log run before trusting the converter.
- Wizard end-to-end with at least one third-party USB pad: capture → menu nav →
  game launch across systems (incl. an N64 and a PS1 title) → P2 mixed-model launch.
- Existing suites stay green: `ui_launch_test.py` 14/14, `emulator_smoke_test.py`,
  retroarch emission tests, `verify_box.sh`.

## Error handling summary

- Wizard state is in-memory until the single atomic save — no partial profiles on disk.
- Malformed/missing profile store → log + empty store; built-ins unaffected.
- Unplug mid-wizard → clean abort + toast.
- Menu-nav overlays additive-only → worst case equals today's behavior.
- Launch with no profile and unknown pad → today's exact fallback path.

## Open questions to resolve during implementation

1. **RetroArch udev button ordering** — confirm `udev_joypad.c` orders buttons
   identically to the joystick API (`BTN_JOYSTICK` first, then `BTN_MISC` wrap) and
   how it indexes non-hat axes. Resolved by source reading + the on-Pi cross-check
   above before the converter is trusted.
2. **Exact wizard-chrome routing** for box GPIO buttons and rotary (Skip/Redo/Cancel)
   — follows the existing GPIO/rotary handling in main.cpp; pick the precise buttons
   during implementation.
3. Whether the style-pick step can be skipped when the target pad is a known VID/PID
   (offer "re-map this known pad" flow) — nice-to-have, decide during implementation
   if free; otherwise the standard flow already covers it.

## Coordination notes

- The uncommitted `fix/content-manager-playlist-editor` branch touches
  `launch_contract.cpp` / `retroarch_launcher.cpp`. This design mostly *removes*
  launcher code, so overlap is limited; whichever branch lands second has a small,
  known merge (the P1/P2 emission block region).
- Golden images: captured profiles ride along in clones under `config/` (harmless,
  and helpful when boxes ship with identical pads). No `first_boot.sh` change.
- OTA: `config/*` is already excluded in all four `update.sh` rsync lists; no
  `OTA_UPDATE_GUARANTEES.md` table change strictly required, but add a line item for
  `config/controller_profiles.json` for documentation completeness.
