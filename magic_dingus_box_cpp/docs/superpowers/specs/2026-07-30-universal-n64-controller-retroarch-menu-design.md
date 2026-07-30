# Universal N64 Controller RetroArch Menu Design

## Goal

Every game launched with an N64-style controller must use the same physical
**Z+Start** chord to open the RetroArch menu, regardless of core.

The chord must not replace, remove, or remap either button's normal game
function:

- Z alone continues to perform the per-console action assigned to physical Z.
- Start alone continues to perform the console's Start action.
- Holding Z and pressing Start opens the RetroArch menu.

This is a menu toggle, not an emulator-exit command.

## Current State

The N64-controller mappings for NES, SNES, Genesis, Atari 7800, PC Engine,
FinalBurn Neo, N64, and Dreamcast already emit:

```text
input_enable_hotkey_btn = <physical Z token>
input_menu_toggle_btn = <physical Start token>
```

PS1 is the only shipped exception. Its N64-controller semantic branch omits
explicit hotkeys and relies on RetroArch's global
L1+R1+Start+Select combination because physical Z is also PS1 L2.

The exact live `2563:0575` profile records physical Z as token `6` and Start
as token `12`.

## Approved Architecture

Make Z+Start an unconditional property of
`semantic_n64_style(const std::string&)`, before core-specific game mappings
are selected:

```cpp
s.hotkey_enable = LogicalControl::N64_Z;
s.menu_toggle = LogicalControl::N64_START;
```

Remove the branch-local helper calls that redundantly assign the same chord.
This makes the rule apply to:

- every currently shipped core;
- all PS1 core variants (`pcsx_rearmed`, Beetle PSX, and SwanStation);
- the legacy UNKNOWN-controller fallback when it resolves as N64-style; and
- future cores that initially fall through the N64-style semantic function.

The existing physical-profile composition remains authoritative. Different
N64-style USB models may use different numeric tokens, but the wizard's saved
`N64_Z` and `N64_START` controls always produce the chord for that device.

## Conflict Prevention

Do not change any per-core game slot. The hotkey fields are additional
RetroArch metadata derived from the same physical controls.

For PS1 with the live controller, the generated configuration must contain
both:

```text
input_player1_l2_btn = "6"
input_enable_hotkey_btn = "6"
```

and:

```text
input_player1_start_btn = "12"
input_menu_toggle_btn = "12"
```

This preserves the existing game mapping while defining the chord. Automated
configuration evidence alone is not enough: live acceptance must prove that
physical Z/L2 still works when pressed alone in a PS1 game and that Z+Start
opens the menu.

For consoles where physical Z is not otherwise assigned, it acts only as the
hotkey modifier. For consoles where it is assigned, ordinary Z input remains
part of the game mapping.

Do not add an exit-emulator bind, change the global combo preset, create
per-ROM remaps, or consume any additional controller button.

## Existing Console Coverage

The N64-style controller has semantic mappings for:

| Console/core family | Controller coverage |
| --- | --- |
| NES | A/B, turbo buttons, Select, Start, D-pad; stick duplicates D-pad |
| SNES | B/A/Y/X, L/R, Start, D-pad; stick duplicates D-pad |
| Genesis | A/B/C, Start, D-pad; stick duplicates D-pad |
| Atari 7800 | Two action buttons, Start, D-pad; stick duplicates D-pad |
| PC Engine / TurboGrafx-16 | I/II, turbo buttons, Start, D-pad |
| FinalBurn Neo | Six arcade actions, Coin, Start, D-pad |
| PS1 | Four face buttons, L1/R1/L2/R2, Select, Start, D-pad, left stick |
| N64 | Complete native controls |
| Dreamcast | A/B/X/Y, digital full-pull triggers, Start, D-pad, stick |

Physical limitations remain honest:

- the N64-style controller cannot provide PS1 right-stick, L3, or R3 input;
- Dreamcast triggers are digital full presses, not gradual analog pressure;
- consoles with more physical inputs than the N64 controller cannot gain
  controls that the hardware does not have.

All mappings have automated coverage. PS1 and N64 are hands-on validated.
Dreamcast still requires hands-on validation.

## Automated Verification

Tests must prove:

1. every named N64-style core resolves hotkeys to logical Z+Start;
2. all three PS1 cores retain the approved face, shoulder, D-pad, and
   left-stick mappings while gaining Z+Start;
3. persisted `2563:0575` resolution produces PS1 game L2=`6`,
   game Start=`12`, hotkey enable=`6`, and menu toggle=`12`;
4. Dreamcast and native N64 mappings remain byte-for-byte unchanged except
   for no-op refactoring of where their already-identical hotkeys originate;
5. PS-style controller hotkeys remain Select+Start;
6. P1 hotkeys serialize exactly once, with no exit-emulator binding; and
7. mapping snapshots change only for N64-style PS1 and unknown-core entries
   that previously lacked explicit hotkeys.

Run the focused mapping suites, snapshot suite, full RetroArch unit suite,
full build, and CTest.

## Live Verification

After independent review:

1. deploy and rebuild on `magicpi5.local`;
2. ensure the managed service starts after the corrected binary finishes
   linking by comparing `/proc/<MainPID>/exe` with the on-disk binary hash;
3. preserve `controller_profiles.json` byte-for-byte;
4. launch Metal Gear Solid and verify:
   - physical Z works as PS1 L2 when pressed alone;
   - Start works normally;
   - Z+Start opens the RetroArch menu;
   - the approved PS1 face, shoulder, D-pad, and analog mappings remain;
5. launch Sonic Adventure and verify:
   - physical A/B/C-Left/C-Down perform Dreamcast A/X/Y/B;
   - L/R perform digital left/right trigger presses;
   - Start, D-pad, and analog stick work;
   - Z+Start opens the RetroArch menu; and
6. launch Crazy Taxi if needed to confirm both digital trigger mappings in a
   trigger-heavy game.

Do not claim universal live completion until PS1 L2 and Dreamcast hands-on
checks pass.
