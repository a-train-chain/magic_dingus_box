# Native N64 Controller Core Mapping Design

## Goal

When an N64-style controller launches an N64 game, every physical control
must perform the function printed on the controller. The fix must be isolated
to N64 cores and must not change the approved PS1 or Dreamcast layouts.

The RetroArch menu must remain reachable through a deliberate combination
that makes sense on a native N64 controller.

## Root Cause

The saved `2563:0575` wizard profile correctly records the real controller's
physical controls. The generated Super Mario 64 configuration also selects
that profile.

The defect is in the N64 semantic mapping. Mupen64Plus-Next's independent
C-button mode (`mupen64plus-alt-map = "True"`) assigns native N64 functions
to these RetroPad slots:

| RetroPad slot | Native N64 function |
| --- | --- |
| B | A |
| Y | B |
| A | C-Down |
| X | C-Up |
| L | C-Left |
| R | C-Right |
| L2 | Z |
| R2 | R shoulder |
| Select | L shoulder |
| Start | Start |

The existing N64-style mapping correctly assigns physical A and Z, but it
assigns physical B, L, and R to the wrong RetroPad slots and also routes the
C cluster through the right-stick bridge. Default button values then create
additional duplicate functions.

## Approved Mapping

For `mupen64plus_next` and `parallel_n64`, resolve the saved physical profile
through this direct semantic mapping:

| Physical N64 control | RetroPad slot | Emulated N64 function |
| --- | --- | --- |
| A | B | A |
| B | Y | B |
| C-Down | A | C-Down |
| C-Up | X | C-Up |
| C-Left | L | C-Left |
| C-Right | R | C-Right |
| Z | L2 | Z |
| R shoulder | R2 | R shoulder |
| L shoulder | Select | L shoulder |
| Start | Start | Start |
| Analog stick | Left analog stick | Control stick |
| D-pad | D-pad | D-pad |

The mapping clears unassigned button defaults before filling these slots.
It does not emit right-stick C-button bindings, because independent C-button
mode already provides direct digital slots for all four physical C buttons.
This prevents duplicate or overlapping N64 functions.

`mupen64plus-alt-map = "True"` remains enabled for both supported N64 cores.

## RetroArch Menu

Physical **Z+Start** opens the RetroArch menu in N64 games:

- Z is the hotkey-enable button.
- Start is the menu-toggle button.
- Start alone remains the native N64 Start button.
- Z alone remains the native N64 Z trigger.

This combination is easy to remember, requires a deliberate chord, and avoids
consuming or layering any native game function during ordinary single-button
input.

The PS1 global **L+R+C-Up+Start** menu combination and all existing non-N64
hotkey behavior remain unchanged.

## Scope and Isolation

Only the N64-style semantic branch for N64 cores changes. Do not alter:

- the saved controller profile or wizard capture format;
- kiosk A/B menu behavior;
- PS1 or Dreamcast semantic mappings;
- DualShock device overrides;
- controller VID/PID classification;
- N64 core rendering, save, or performance options; or
- mappings for PS-style controllers.

The fix applies to every N64 game because mappings are generated per core,
not per ROM.

## Verification

Automated tests must independently derive and assert the native mapping for:

1. the built-in legacy N64 adapter profile;
2. the persisted live `2563:0575` wizard profile;
3. both `mupen64plus_next_libretro` and `parallel_n64_libretro`;
4. serialized RetroArch player-one binds;
5. absence of right-stick C-button bindings and stale default duplicates;
6. physical Z+Start hotkeys; and
7. unchanged PS1 and Dreamcast mappings.

After review, deploy the fix to `magicpi5.local`, rebuild, run the full Pi
test suite, restart only the managed kiosk service, and launch Super Mario 64.
The fresh generated configuration must match the saved profile:

```text
b/y/a/x = A/B/C-Down/C-Up
l/r/l2/r2/select/start = C-Left/C-Right/Z/R/L/Start
right-stick C-button binds = unbound
hotkeys = Z/Start
```

Hands-on acceptance requires:

- physical A jumps;
- physical B attacks or cancels where the game uses N64 B;
- each physical C button controls only its matching camera direction;
- L, R, Z, Start, D-pad, and analog stick retain their native functions; and
- Z+Start opens the RetroArch menu.
