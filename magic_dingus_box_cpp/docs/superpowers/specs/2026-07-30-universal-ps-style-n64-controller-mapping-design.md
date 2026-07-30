# Universal PS-style controller mapping for N64

**Date:** 2026-07-30
**Status:** approved in conversation; awaiting written-spec review

## Goal

Ship one ergonomic N64 layout for captured PlayStation-style controllers. The
layout must apply automatically to every N64 game, both supported N64 cores,
both players, and every controller profile classified as `PS_STYLE`.

The mapping is position-based. Letters printed on a third-party pad do not
need to match RetroArch's virtual RetroPad labels. For example, the SHANWAN
Android Gamepad prints `A/X/Y/B` at the bottom/left/top/right positions, while
RetroArch names those same face positions `B/Y/X/A`.

## Non-goals

- Do not change mappings for PS1, SNES, or any non-N64 system.
- Do not change the mapping for N64-shaped controllers.
- Do not create a machine-local `.rmp` remap or per-game overrides.
- Do not rename RetroArch's virtual control labels.
- Do not assign N64 actions to Select, L3, or R3.

## Delivery approach

Encode the layout in the shared `PS_STYLE + N64` semantic mapping.

This is preferred over a RetroArch core-remap file because the shipped table
is portable, version-controlled, unit-tested, and regenerated for every
launch. It is preferred over per-game remaps because the requested behavior
is intentionally universal.

The implementation must continue to use logical controls such as `CROSS`,
`L1`, and `R2`. The captured physical profile supplies controller-specific
button numbers and axis tokens. No SHANWAN-specific token may be hardcoded in
the semantic table.

## Approved control layout

| Physical PS-style control | RetroPad slot | N64 result |
|---|---|---|
| Cross / bottom | B | A |
| Square / left | Y | B |
| Triangle / top | X | C-Up |
| Circle / right | R | C-Right |
| L1 | Select | L |
| L2 | L2 | Z |
| R1 | R2 | R |
| R2 | A | C-Down |
| Left stick | Left analog | N64 analog stick |
| Right stick | Right analog | C-Up, C-Down, C-Left, and C-Right |
| D-pad | D-pad | N64 D-pad |
| Start | Start | N64 Start |
| Select | Hotkey modifier only | No N64 action |
| Select + Start | RetroArch hotkey chord | Open RetroArch menu |
| L3 / R3 | Unbound | No action |

The apparently unusual RetroPad slots are required by
`mupen64plus-alt-map = "True"`:

- RetroPad B becomes N64 A.
- RetroPad Y becomes N64 B.
- RetroPad X becomes C-Up.
- RetroPad A becomes C-Down.
- RetroPad L becomes C-Left.
- RetroPad R becomes C-Right.
- RetroPad L2 becomes N64 Z.
- RetroPad R2 becomes N64 R.
- RetroPad Select becomes N64 L.

The generated config therefore prioritizes the final N64 function, not a
superficial match between the controller's printed letter and RetroArch's
virtual label.

## Data flow and scope

The mapping path is:

```text
physical input
  -> captured logical control
  -> shared PS-style N64 semantic table
  -> generated RetroArch player binding
  -> RetroPad slot
  -> N64 core action
```

Only the N64 branch of `semantic_ps_style()` changes. Both
`mupen64plus_next_libretro` and `parallel_n64_libretro` already select that
branch. The same semantic table is resolved independently for player 1 and
player 2, so both players receive the layout without duplicated mapping
logic.

The existing N64 core option that enables the alternate map remains pinned
for both cores. The right stick remains bound to the RetroPad right-stick
axes so it continues to provide the complete C-button cluster. The face
buttons and R2 provide convenient digital duplicates of C-Up, C-Right, and
C-Down; C-Left remains available on the right stick.

## Safety behavior

- A missing or incompatible physical control must serialize as RetroArch's
  `nul` sentinel, never as an empty string. RetroArch parses an empty button
  value as physical button `0`, which previously made the bottom face button
  trigger N64 A and N64 R simultaneously.
- The physical Select button remains the hotkey-enable input. It must not also
  occupy a player-facing RetroPad slot.
- Start remains both N64 Start and the menu-toggle half of the
  Select+Start chord. Start alone must continue to reach the game.
- L3 and R3 remain explicitly unbound for N64.
- No non-N64 semantic mapping or N64-style-controller mapping may change.

## Testing

Automated coverage must include:

1. Semantic mapping assertions for every approved face button, shoulder,
   trigger, stick, D-pad direction, Start, Select, L3, and R3.
2. Physical-profile resolution using the full SHANWAN profile, including
   physical R2 token `9`.
3. Generated config assertions for player 1 and player 2.
4. Equivalent expectations for Mupen64Plus-Next and Parallel N64.
5. Regression assertions that physical R2's captured token lands in the
   RetroPad A field (N64 C-Down), physical R1's token lands in the RetroPad R2
   field (N64 R), and no unbound button field serializes as `""`.
6. Snapshot updates limited to the two `PS_STYLE + N64` entries, plus any
   expected updates caused by the already-approved `nul` serialization fix.
7. The complete RetroArch unit suite.

After deployment to `magicpi5`, inspect the newly generated live config before
manual play testing. Confirm that its physical tokens resolve to the approved
N64 actions and that no unintended token aliases exist.

Manual acceptance checks:

- **Super Smash Bros.:** bottom face button performs N64 A attacks without
  grabbing; R1 performs N64 R grab/throw.
- **Super Mario 64:** bottom face button jumps, left face button attacks,
  R1 performs the N64 R camera action, and the right stick controls all four
  C directions.
- **Zelda (Ocarina of Time or Majora's Mask):** L2 performs Z-targeting,
  digital C buttons and the right stick reach the intended C-item directions,
  and the native D-pad remains available.
- Select alone produces no N64 action; Select+Start opens RetroArch.

## Success criteria

The work is complete when the automated suite passes, the live generated
configuration matches this specification, and the hands-on checks confirm the
mapping in representative action, camera, and C-item use cases.
