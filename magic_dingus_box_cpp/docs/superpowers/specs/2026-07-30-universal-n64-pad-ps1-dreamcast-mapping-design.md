# Universal N64-style controller mapping for PS1 and Dreamcast

**Date:** 2026-07-30
**Status:** approved in conversation; awaiting written-spec review

## Goal

Ship one layer-free PlayStation 1 mapping and one layer-free Dreamcast mapping
for physical N64-style controllers. Each mapping must apply automatically to
every game for its system, every supported core for that system, and both
controller ports.

The layouts prioritize consistent physical roles across systems. The large N64
`A` button remains the primary action. The physical `B`, `C-Left`, and
`C-Down` buttons keep the same secondary, upper, and alternate-action roles
across PS1 and Dreamcast even when the destination console's printed labels
differ.

## Scope

The change is limited to the PS1 and Dreamcast branches of
`semantic_n64_style()`.

- PS1 includes PCSX-ReARMed, Beetle PSX, and SwanStation.
- Dreamcast includes Flycast.
- Player 1 and player 2 receive the same game-facing mapping.
- The mapping is universal within each system. There are no per-title remaps.
- Physical tokens continue to come from the captured or built-in N64-style
  controller profile.

All other controller styles, emulator cores, and system mappings remain
unchanged.

## Non-goals

- Do not emulate a second PS1 analog stick.
- Do not add PS1 L3 or R3 through a modifier layer.
- Do not add per-game exceptions.
- Do not map the N64 analog stick to the D-pad.
- Do not hard-code the USB adapter's physical button numbers in the semantic
  tables.
- Do not attempt analog Dreamcast trigger pressure on digital N64 shoulder
  buttons.

## Approaches considered

### Role-consistent, layer-free mapping — selected

Keep comparable gameplay roles on the same physical buttons across PS1 and
Dreamcast. This is easy to learn, uses every standard console function
directly, and avoids hidden modes.

### Console-label mapping

Match the destination console's printed labels more closely. This makes some
individual labels easier to explain but moves common action roles between
physical buttons when changing systems.

### Maximum-compatibility modifier layer

Use a held button to emulate PS1's second stick and L3/R3. This reaches a small
number of DualShock-only functions but makes normal play and RetroArch hotkeys
more complicated. It conflicts with the approved layer-free requirement.

## Approved PS1 layout

The PS1 mapping exposes the complete original PlayStation control set plus the
left analog stick. The D-pad and analog stick remain independent.

| Physical N64 control | RetroPad slot | PS1 result |
|---|---|---|
| A | B | Cross |
| B | Y | Square |
| C-Left | X | Triangle |
| C-Down | A | Circle |
| L shoulder | L | L1 |
| R shoulder | R | R1 |
| Z trigger | L2 | L2 |
| C-Right | R2 | R2 |
| C-Up | Select | Select |
| Start | Start | Start |
| D-pad | D-pad | PS1 D-pad |
| Analog stick | Left analog | PS1 left analog |
| C-Up + L + R + Start | RetroArch menu combination | Open Quick Menu |

The PS1 pad type remains `analog`, with `analog_dpad_mode = "0"`. The semantic
mapping binds the physical analog stick only to the RetroPad left stick. It
does not also emit D-pad axis bindings.

The mapping does not assign RetroPad right-stick directions, L3, or R3.

### PS1 menu access

PS1 uses RetroArch's configured `L1 + R1 + Start + Select` gamepad combination.
On the physical N64 controller this is:

```text
L + R + Start + C-Up
```

The PS1 semantic branch does not assign a separate hotkey-enable button. This
preserves Z as L2 and keeps every original PlayStation button available to the
game.

## Approved Dreamcast layout

The Dreamcast mapping follows the same physical action roles as PS1.

| Physical N64 control | RetroPad slot | Dreamcast result |
|---|---|---|
| A | B | A |
| B | Y | X |
| C-Left | X | Y |
| C-Down | A | B |
| L shoulder | L2 | Left trigger |
| R shoulder | R2 | Right trigger |
| Start | Start | Start |
| D-pad | D-pad | Dreamcast D-pad |
| Analog stick | Left analog | Dreamcast analog stick |
| Z trigger | Hotkey enable | No Dreamcast action |
| Z + Start | RetroArch hotkey chord | Open Quick Menu |
| C-Up | Unbound | No action |
| C-Right | Unbound | No action |

Dreamcast's original triggers are pressure-sensitive. The N64 controller's
shoulders are digital, so each trigger is either released or fully pressed.
This provides complete trigger access for ordinary play, but racing games
cannot receive gradual throttle or brake pressure from this controller.

The Dreamcast branch intentionally clears unused RetroPad button slots before
applying the assignments. This prevents legacy default indices from creating
duplicate or accidental actions on Select, L1, R1, or other unassigned slots.

## Architecture and data flow

The existing mapping pipeline remains intact:

```text
physical N64-style input
  -> captured logical control
  -> per-core semantic_n64_style() table
  -> build_mapping()
  -> generated RetroArch player binding
  -> emulated console action
```

The semantic tables refer only to logical controls such as `N64_A`, `N64_Z`,
and `N64_C_LEFT`. `build_mapping()` resolves those controls through the
physical profile for the connected adapter. A captured profile can therefore
correct a rewired or clone controller without changing the console mapping.

Both player mappings are resolved independently through the existing port
pipeline. Only player 1 owns RetroArch menu hotkeys, matching current launcher
behavior.

## Safety behavior

- Intentionally unassigned button fields serialize as RetroArch's `"nul"`
  sentinel, never as an empty string.
- PS1 right-stick, L3, and R3 fields remain unassigned.
- Dreamcast C-Up, C-Right, Select, and unused RetroPad shoulder slots remain
  unassigned.
- The PS1 analog stick must not emit D-pad axis bindings.
- The Dreamcast analog stick must not emit D-pad axis bindings.
- The PS1 branch relies on the standard four-button menu combination and must
  not also consume a gameplay button as an explicit hotkey modifier.
- Dreamcast retains the dedicated Z + Start menu chord because Z has no
  Dreamcast game function.
- No PS-style controller mapping or unrelated N64-style system branch may
  change.

## Testing

### Automated coverage

1. Assert every approved PS1 assignment for PCSX-ReARMed, Beetle PSX, and
   SwanStation.
2. Assert every approved Dreamcast assignment for Flycast.
3. Resolve the mappings through the full N64-adapter physical profile rather
   than testing only semantic objects.
4. Verify player 1 and player 2 receive the same game-facing buttons, D-pad,
   and analog stick.
5. Verify only player 1 receives explicit per-player hotkeys where applicable.
6. Verify PS1 emits no right-stick, L3, R3, or stick-to-D-pad bindings.
7. Verify Dreamcast emits no bindings for C-Up, C-Right, or unused RetroPad
   slots.
8. Verify all intentionally unbound button values serialize as `"nul"` and no
   generated player button value is `""`.
9. Update mapping snapshots only for N64-style and existing unknown-controller
   fallback entries whose PS1 or Dreamcast mappings intentionally changed.
10. Run the complete CTest suite.

### Live configuration inspection

After deployment to `magicpi5`, launch a fresh PS1 game and a fresh Dreamcast
game. Inspect the generated RetroArch configuration for:

- exact player button tokens;
- native D-pad hat bindings;
- independent left-stick axis bindings;
- absence of PS1 right-stick and L3/R3 bindings;
- absence of unintended Dreamcast button bindings;
- correct menu behavior; and
- no empty button values.

### Hands-on acceptance

For PS1, test at least one action game and one menu-heavy or racing game.
Confirm:

- A, B, C-Left, and C-Down reach Cross, Square, Triangle, and Circle;
- L, R, Z, and C-Right reach L1, R1, L2, and R2;
- C-Up reaches Select and Start reaches Start;
- the D-pad and analog stick work independently; and
- L + R + C-Up + Start opens the RetroArch Quick Menu.

For Dreamcast, test at least one action game and one racing or trigger-heavy
game. Confirm:

- A, B, C-Left, and C-Down reach A, X, Y, and B;
- L and R produce full left-trigger and right-trigger presses;
- Start, D-pad, and analog movement work;
- C-Up and C-Right produce no game action; and
- Z + Start opens the RetroArch Quick Menu.

## Success criteria

The work is complete when:

- the complete automated suite passes;
- fresh live PS1 and Dreamcast configurations match this specification;
- representative games confirm every mapped function;
- menu access works without stealing a game button;
- unrelated controller and core snapshots remain unchanged; and
- the limitations around PS1 DualShock-only inputs and Dreamcast analog trigger
  pressure are documented rather than hidden.
