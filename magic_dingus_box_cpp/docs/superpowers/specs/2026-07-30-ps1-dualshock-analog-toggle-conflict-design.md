# PS1 DualShock Analog Toggle Conflict Design

## Goal

Keep the N64-style controller's physical analog stick active in PS1 games
while preserving the approved direct game mapping and universal **Z+Start**
RetroArch menu chord.

## Confirmed Root Cause

The generated PCSX-ReARMed options do not set
`pcsx_rearmed_analog_combo`, so the core uses its default
`l1+r1+select` toggle. On the live N64-style controller that means:

```text
L + R + C-Up
```

That combination is the first three buttons of the former RetroArch menu
chord. If those three inputs register before Start, PCSX toggles the emulated
DualShock to digital mode. The runtime log recorded `ANALOG OFF`, and Metal
Gear Solid's automatic resume state preserved that controller state across
later launches.

The live launch otherwise remained correct:

- PCSX selected DualShock for both ports with `--device 1:517` and
  `--device 2:517`;
- the generated player-one left-stick axes remained `+0/-0/+1/-1`;
- the PS1 button mapping remained unchanged; and
- the universal menu chord remained Z token `6` plus Start token `12`.

## Approved Fix

Add this line to the generated PCSX-ReARMed core-options file:

```text
pcsx_rearmed_analog_combo = "disabled"
```

This prevents the core from changing DualShock analog mode in response to a
game-button combination. It applies only to PCSX-ReARMed launches and does
not alter RetroArch's Z+Start menu handling.

Do not change:

- the explicit PCSX DualShock device arguments;
- any PS1 face, shoulder, D-pad, or analog-axis mapping;
- any N64, Dreamcast, or other-console mapping;
- the saved `2563:0575` controller profile; or
- PS1 memory-card save data.

## Existing Auto-State Recovery

Before the first post-fix Metal Gear Solid launch, move its current
`.state.auto` file to a timestamped backup beside the original. Do not delete
or overwrite it.

This forces one normal game boot, allowing the emulated DualShock to enter
analog mode without inheriting the saved `ANALOG OFF` state. The memory-card
save remains in place, and the old resume state remains recoverable by moving
the backup back if needed.

The operation must resolve and print the exact source and backup paths before
moving the single Metal Gear Solid auto-state. It must not use a recursive
command, a broad glob, or touch any other game's state.

## Automated Verification

Add a regression assertion to the existing PCSX core-options contract test.
The test must fail before the production change because the generated options
omit `pcsx_rearmed_analog_combo`, then pass when they contain exactly:

```text
pcsx_rearmed_analog_combo = "disabled"
```

Run the focused launch-contract test, full RetroArch unit suite, complete
build, and CTest.

## Live Verification

After review and deployment:

1. preserve the saved controller profile byte-for-byte;
2. back up the one Metal Gear Solid auto-state;
3. launch Metal Gear Solid and confirm the fresh config contains the disabled
   PCSX analog combo and DualShock device `517`;
4. confirm the physical analog stick works;
5. confirm Z alone remains PS1 L2 and Start remains Start;
6. confirm Z+Start opens the RetroArch menu; and
7. confirm the existing face, shoulder, and D-pad controls still work.

Only after PS1 passes should the existing Dreamcast hands-on validation plan
resume.
