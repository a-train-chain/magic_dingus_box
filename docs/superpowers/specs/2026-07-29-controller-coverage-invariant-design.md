# Controller coverage invariant — design

**Date:** 2026-07-29
**Status:** approved (design), not yet implemented
**Scope:** guarantee both shipped pads can reach every control on all ten emulated consoles

## Problem

The kiosk ships two physical pads:

- a **PlayStation-style** pad (`2563:0526`, SHANWAN "Android Gamepad") — 12 buttons, 2 analog sticks, real hat
- an **N64-style** adapter (`2563:0575`) — 9 buttons + Start, 1 analog stick, real hat

`semantic_ps_style()` and `semantic_n64_style()` in `retroarch/controller_mapping.cpp`
hand-write a mapping for each pad × core pair. Ten cores × two pads is **twenty
hand-maintained tables, with nothing checking them for completeness.**

Auditing all twenty against what each core actually reads found five places where
a console control is unreachable while the pad has unused inputs sitting free:

| pad | core | unreachable | pad had free |
|---|---|---|---|
| PS-style | PS1 | right stick, L3/R3 | both sticks captured; L3/R3 captured |
| N64 | Genesis | 3 of 6 buttons (X/Y/Z) | Z, L, R, C_LEFT, C_UP, C_RIGHT |
| N64 | SNES | Select | Z, C_UP, C_RIGHT |
| N64 | PC Engine | Select | four inputs |
| N64 | PS1 | L2 | Z |

These are not design compromises. They are omissions — the wizard captured the
controls, the pad reports them, and the mapping table simply never referenced
them. The PS1 right-stick case was found only because a game was launched and the
emitted binds were inspected by hand.

**The fix is not to patch five holes. It is to make holes fail a test.**

## Non-goals

- Not replacing the twenty hand-written tables with auto-assignment. They encode
  real ergonomic judgment (see *Ergonomic principles*) that a capability-matching
  algorithm would discard, and doing so would rewrite all 33 golden-snapshot
  strings at once.
- Not building a per-game override layer. Deferred until real play identifies
  specific titles that fight the console-level default; building the machinery
  first would be speculative. Note for later: MDB generates its RetroArch config
  at launch and already knows the ROM path, so per-game can live in the layer MDB
  already owns — RetroArch `.rmp` remap files are **not** required, and would
  split the source of truth across two systems whose semantics differ (`.rmp`
  permutes RetroPad IDs; MDB's layer maps physical → RetroPad).
- Not touching `input_remap_sort_by_controller_enable`, `.rmp` files, or any
  RetroArch-side config. Everything here is MDB-side.

## Background: the two mapping layers

Understanding which layer this design operates in matters, because RetroArch has
two and they are easy to conflate.

1. **Device → RetroPad.** `input_playerN_x_btn = "4"` means "physical button 4 is
   RetroPad X". This is what MDB generates, per launch, and the only layer this
   design touches.
2. **RetroPad → core.** `.rmp` remap files permute RetroPad IDs before the core
   sees them. Fully enabled on the box (`input_remap_binds_enable=true`,
   `auto_remaps_enable=true`, remap directory configured) and **entirely unused —
   zero `.rmp` files exist.**

Each core then applies its own fixed internal RetroPad → emulated-console
mapping. That last part is why the invariant below is declared **per core, not
per console**: `mupen64plus` reads RetroPad `L2` to mean N64's Z button, so "which
slots matter" is a property of the core.

## Design

### 1. Declare what each core reads

A table mapping each core to the set of RetroPad slots that core resolves to a
real emulated control. Sketch (exact set to be confirmed against each core's
source during implementation — an incorrect declaration makes the invariant
either toothless or a source of false failures):

| core | slots read |
|---|---|
| Nestopia (NES) | b, a, select, start, dpad |
| Snes9x (SNES) | b, a, y, x, l, r, select, start, dpad |
| Genesis Plus GX | b, a, y, x, l, r, start, dpad |
| PCSX-ReARMed (PS1) | b, a, y, x, l, r, l2, r2, l3, r3, select, start, dpad, left stick, right stick |
| Beetle PCE Fast | b, a, select, start, dpad |
| ProSystem (Atari 7800) | b, a, select, start, dpad |
| FinalBurn Neo | b, a, y, x, l, r, select (coin), start, dpad |
| Mupen64Plus-Next / ParaLLEl N64 | b, a, l, r, l2 (Z), start, dpad, left stick, right stick (C) |
| Flycast (Dreamcast) | b, a, y, x, l2, r2, start, dpad, left stick |

Genesis's **Mode** button is deliberately excluded: it toggles 3/6-button
compatibility and no game requires pressing it during play.

### 2. Enforce it in both directions

A table-driven test over every (core, pad) pair:

- **No dead control.** Every declared slot must resolve to a non-empty bind
  token. A slot the core reads but the pad cannot reach fails.
- **No stale exception.** Exceptions are declared explicitly with a reason. If a
  declared exception turns out to be *bound*, that also fails — otherwise the
  allow-list rots into a list of things that used to be broken.

The second direction is the part that keeps this honest over time.

### 3. The five fills

Four of the five follow conventions already present in the file rather than
inventing new ones — which is the strongest available evidence that the existing
tables are worth keeping.

| pad → core | fill | reasoning |
|---|---|---|
| N64 → SNES | `select = C_UP` | C_UP is **already** Select for this pad on NES, PS1 and FBNeo. SNES simply missed the convention. |
| N64 → PC Engine | `select = C_UP` | same convention |
| N64 → PS1 | `l2 = N64_Z` | Z is a trigger, L2 is a trigger. Z was unused. |
| N64 → Genesis | `x = C_LEFT`, `l = C_UP`, `r = C_RIGHT` | see *Ergonomic principles* — keeps all six Genesis buttons under the right thumb |
| PS → PS1 | right stick → `RSTICK_*`, `l3`/`r3` → `L3`/`R3` | pad captured all of them; L3/R3 need new plumbing (below) |

Existing bindings are **not** changed. Every fill is additive, which keeps the
golden-snapshot diff small and cannot regress a mapping that already works.

### 4. New plumbing for L3/R3

No pad on any core has ever had stick clicks: `ControllerMapping` has no
`l3_btn`/`r3_btn` fields and `write_player_binds()` emits zero such lines.
Required:

- `SemanticMapping`: `std::optional<LogicalControl> l3, r3`
- `ControllerMapping`: `std::string l3_btn, r3_btn`
- `build_mapping()`: resolve them alongside the other optionals
- `write_player_binds()`: emit `input_playerN_l3_btn` / `r3_btn` for **both**
  players, following the existing unconditional-emission rule (an empty value
  still writes `= ""`; RetroArch treats that differently from an absent line)

RetroArch supports these — a stock `retroarch.cfg` contains
`input_playerN_l3_btn` and `input_playerN_r3_btn`.

### 5. The one accepted exception

**The N64 pad cannot have a right stick on PS1.** After `l2 = Z`, all ten of its
buttons are consumed: `A B C_DOWN C_LEFT L R Z C_RIGHT C_UP Start` cover PS1's
four face buttons, L1/R1/L2/R2, Select and Start. Nothing remains.

The C-cluster *could* drive a digital right stick — `ControllerMapping` already
has `r_x_plus_btn`-style fields, which is how N64's C-buttons already ride the PS
pad's right stick. But freeing the cluster means dropping `r2 = C_RIGHT`, trading
**R2, which most PS1 games use**, for a **right stick only dual-analog titles
use**. Keeping R2 is the better trade.

Declared as an exception with this reasoning attached.

## Ergonomic principles

Coverage is testable; feel is not. These are the principles behind the
assignments, recorded so future changes can argue with them rather than guess:

1. **Frequency beats position.** The most-used console button goes on the most
   reachable physical input. On the N64 pad the big A and B buttons are most
   reachable, so Genesis's B and C (jump/attack in most games) sit there and
   Genesis A — typically secondary — sits on C_DOWN, even though that inverts
   left-to-right order.
2. **Rarely-pressed controls go on the least reachable input.** C_UP is the
   furthest of the C-cluster, which makes it a *good* home for Select — pressed
   between rounds, never mid-action.
3. **Triggers map to triggers.** Z → L2 rather than to a face button.
4. **Keep a functional group physically grouped.** A real 6-button Genesis pad has
   all six under the right thumb. Hence Genesis X/Y/Z on the C-cluster arc
   (C_LEFT / C_UP / C_RIGHT) rather than split onto the shoulders.

**Open ergonomic question, to settle by playing rather than reasoning:**
principle 4 puts Genesis Y on C_UP, and principle 2 says C_UP is the least
reachable input — a tension. In Street Fighter II Genesis Y is medium punch,
pressed constantly, so C_UP may feel like a stretch. The alternative is
`x = N64_L`, `l = C_UP`, `r = N64_R`, moving two of the three onto the very
comfortable shoulders at the cost of breaking thumb-locality. **Try a 6-button
fighting game and switch if the cluster feels cramped.** This is the one decision
in the document that hands-on testing should override.

## Verification

- Unit tests for the invariant (both directions) and for each fill.
- A test that every other core still emits **empty** `l3_btn`/`r3_btn`, so the new
  plumbing cannot leak stick clicks into consoles that never had them.
- A test that a partially-captured profile (e.g. the builtin DragonRise, which has
  no L3/R3) degrades to empty rather than a garbage token.
- **Golden snapshot regeneration is required** and must be audited line by line.
  `tests/retroarch/mapping_snapshot_golden.h` locks `get_mapping()` output for 11
  cores × 3 pad types and has been byte-identical through all recent work. Expected
  changes: two new `l3_btn`/`r3_btn` lines per player per mapping (empty except
  PS1 + PS pad), right-stick values for PS1 + PS pad, and the four N64-pad fills.
  **Anything else changing is a regression — stop rather than accept it.**
- macOS both `ENABLE_MEDIA_BROWSER` configs, zero warnings.
- Pi aarch64 kiosk build. Build with `nice -n 19 … -j2`: earlier `-j3` builds on
  this 4-core Pi starved the live kiosk until it missed its 10-second systemd
  watchdog and restarted twice. Never touch `/opt` or restart a service.
- Hands-on: a 6-button Genesis game, a SNES game (Select), a dual-analog PS1 game
  on the PS pad, and PS1 on the N64 pad (L2). Confirm with the bind checker that
  emitted tokens all come from the pad's captured profile.

## Sequencing

A separate in-flight change already implements the PS → PS1 right stick and L3/R3
fill (branch `fix/ps1-right-stick-l3r3`). Let it land and be reviewed first; this
work then builds the invariant on top, which retroactively protects it.
