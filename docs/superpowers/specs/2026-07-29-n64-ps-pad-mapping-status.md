# N64 games on the PS-style pad — status and open decisions

**Date:** 2026-07-29
**Status:** core bug fixed and deployed; target layout designed but not implemented; one related bug found and blocking separate work
**Author's note:** this document exists because the investigation took most of a day, went through five wrong hypotheses before the real one, and none of it is written down anywhere else. Read this before touching N64 pad mapping again.

## Executive summary

A PlayStation-style USB pad (SHANWAN "Android Gamepad", `2563:0526`) could not jump in Super Mario 64. Five hypotheses were tried and eliminated — pad mislabeling, evdev index drift, a mapping-table gap, hardware cross-talk, a duplicate RetroArch bind — before the real cause surfaced: **`mupen64plus-next` ships a core option, `alt-map`, off by default, and with it off the emulator overlays its four C-buttons onto the A/B face buttons.** One RetroPad slot did double duty, and the C-button behavior always won, so no physical button could produce a clean jump.

**Fixed and deployed** (`main` @ `48a5376`): `write_n64_core_options()` now sets `mupen64plus-alt-map = "True"` for both N64 cores. Confirmed on hardware: jump works.

**Not yet done:** the pad's full N64 layout was designed in conversation (geometry-based face-button assignment, L/Z adjacency, C-buttons on the analog stick) but only partially applied, and only as a live core-option read — nothing has been baked into the shipped mapping table or saved as a portable remap file.

**Found along the way, not yet fixed:** `ControllerMapping`'s C++ struct ships non-empty hardcoded defaults for every button field. When a semantic table leaves a slot unset, the default silently ships as a real binding instead of "unbound." This is why punch worked at all before any of today's fixes — by accident, not by design — and it is currently blocking the controller-coverage-invariant plan (`feat/coverage-invariant`, Task 1, status: **BLOCKED**).

## Timeline (why this took so long)

Recording the wrong turns because each one consumed real time and each is a plausible-sounding trap for the next person too.

1. **"The wizard's PlayStation button names don't match this pad's ABXY labels."** True, and worth fixing in the wizard UI, but not the jump bug — position-based capture (bottom/right/left/top) already made the *capture* correct regardless of labels.
2. **"joydev button indices might not be contiguous with evdev codes."** Checked with `EVIOCGBIT` against `/dev/input/event5`: the pad declares codes 304–318 with zero gaps, so every joydev index equals `code − 304` exactly. Not the bug.
3. **"The mapping table has a gap — a slot never bound."** It doesn't, for the RetroPad slots the semantic table actually sets. This hypothesis was reasonable given the *separate*, real ghost-default bug (see below), but the specific slot driving jump (`s.b = CROSS`) was correctly set.
4. **"The pad has hardware cross-talk — pressing a face button also perturbs the right stick."** Tested directly: a raw joydev event logger recorded 10+ presses of the jump button live, *while the collision was still happening in-game*, and captured only clean `BUTTON 0` events — zero axis noise. Ruled out with data, not inference.
5. **"RetroArch has a duplicate live binding."** This one was real, but not a shipped bug: while jointly navigating RetroArch's `Port 1 Controls` menu earlier in the same debugging session, R2 was accidentally bound to the same physical button as jump (almost certainly a stray `Start` press on the R2 row entering rebind mode, followed by the next real button press). It lived only in that RetroArch process's memory — confirmed zero `.rmp` remap files exist on disk, and confirmed the generated launch config always wrote `input_player1_r2_btn = ""`. Unbinding it live fixed the symptom immediately. **This will not recur**: nothing MDB generates ever binds R2 for this table, and no file persisted the accidental bind.
6. **The real bug**: `mupen64plus-alt-map`. Found by reading the core's own input descriptor strings out of the `.so` (`strings -t d`), which showed labels like `"A Button (C3)"` and `"B Button (C2)"` instead of plain `"A Button"`/`"B Button"` — the `(Cn)` suffix being the tell that a C-button was riding on top of the real function. The option's own description in the binary: *"Use an alternate control scheme, useful for some 3rdparty controllers."*

## Confirmed working state (with evidence)

All of the below was checked directly against the live box, not assumed.

| RetroPad slot | physical button (pad's own ABXY label) | evdev code → token | N64 function | status |
|---|---|---|---|---|
| B | bottom (labeled **A**) | 304 → `"0"` | **A — jump** | ✅ confirmed by direct play-test after the `alt-map` fix |
| Y | left (labeled **X**) | 307 → `"3"` | B — punch | ✅ confirmed working, **but via the ghost-default bug, not a deliberate binding** — see below |
| A | right (labeled **B**) | 305 → `"1"` | unknown | ⚠️ not verified since the `alt-map` fix — see Open Item 1 |
| X | top (labeled **Y**) | 308 → `"4"` | unknown | ⚠️ not verified since the `alt-map` fix — see Open Item 1 |
| — | right stick, all 4 directions | axes 2/3 | C-up/down/left/right | ✅ confirmed live via RetroArch's own `Options` menu: all four `*-cbutton` core options read "Right Analog [X/Y] [positive/negative]" |
| L | left shoulder | 310 → `"6"` | L | designed, not re-verified post-fix |
| L2 | left trigger | 312 → `"8"` | Z | designed, not re-verified post-fix |
| R | right shoulder | 311 → `"7"` | R (camera cycle) | confirmed pre-fix, not re-verified post-fix |

Source of the RetroPad-B→jump and RetroPad-Y→punch facts: the user read the core's own `Port 1 Controls` screen aloud, control by control, which is authoritative — it comes from the core's `SET_INPUT_DESCRIPTORS` call, not from any guess on this side.

## The ghost-default bug (separate from the jump bug, found while investigating it)

`magic_dingus_box_cpp/src/retroarch/controller_mapping.h`, `struct ControllerMapping`, ships non-empty hardcoded defaults:

```
b_btn = "1"        y_btn = "3"        select_btn = "10"    start_btn = "2"
a_btn = "0"        x_btn = "4"        l_btn = "5"           r_btn = "6"
```

`build_mapping()` only overwrites a field when the semantic table explicitly assigns that `LogicalControl`. A slot the table never mentions keeps the struct default — which is a real, non-empty RetroArch bind token — and **ships to the game indistinguishable from a deliberate binding.**

Concretely, in the very table this document is about (`semantic_ps_style()`'s `"Nintendo 64 (PS-style)"` branch), `s.x` and `s.y` are never assigned:

```cpp
s.b = L::CROSS; s.a = L::CIRCLE; s.l = L::L1; s.r = L::R1;
s.l2 = L::L2; s.start = L::START;
```

So `y_btn` silently kept its default `"3"`, which happens to be the same evdev token as physical **Square** (labeled X on this pad) — which is why punch worked at all. It is not a coincidence that will hold forever: it is a coincidence that holds *today*, on *this* struct's default, for *this* pad's captured tokens. Change any one of those three things and punch moves or disappears with no code change to explain why.

**Confirmed worse elsewhere.** In `semantic_n64_style()`'s Genesis branch (verified directly against the source on 2026-07-29, not taken on a subagent's word):

```cpp
s.a = L::N64_A; s.b = L::N64_B; s.y = L::N64_C_DOWN; s.start = L::N64_START;
dpad(); stick(); s.left_stick = true; s.stick_to_dpad = true; hotkeys();
```

`s.x`, `s.l`, `s.r` are never set, so `x_btn`/`l_btn`/`r_btn` keep their defaults `"4"`/`"5"`/`"6"` — and those tokens are the **N64 pad's real L, R and Z-trigger buttons.** `Z` is the box-wide RetroArch menu-toggle modifier (`Z + Start` opens the menu on every core). A ghost bind on that token is a live risk, not a cosmetic one.

**Why this blocks the coverage-invariant work:** Task 1 of `docs/superpowers/plans/2026-07-29-controller-coverage-invariant.md` (branch `feat/coverage-invariant`, worktree `.worktrees/coverage`) built a test asserting "every slot a core reads resolves to a non-empty token," using `""` as the definition of "unreachable." That assumption is wrong in the presence of ghost defaults: an *actually* unbound slot and a *ghost-bound* slot are indistinguishable by that test, because both need to be "non-empty" — one legitimately, one by accident. The implementer's run found exactly this (3 failures instead of the expected 7, in the wrong places) and correctly stopped rather than patch the test to pass. **Uncommitted evidence sits in that worktree right now**: `magic_dingus_box_cpp/tests/retroarch/core_slot_coverage.h` and `test_core_slot_coverage.cpp` (untracked, not committed — left as-is deliberately, do not commit them without redesigning `slot_value()` first).

This needs a decision before that plan can resume — see Open Item 3.

## The designed target layout (discussed, reasoned through, not implemented)

Established in conversation with the user, including the geometric reasoning, before the `alt-map` bug was found — so it was designed against a controller that, at the time, still had two functions overlaid on one button. It should still be correct now that A and B are separated, but **has not been re-validated against the fixed core.**

| physical button (this pad's own label) | N64 function | reasoning |
|---|---|---|
| **A** (bottom) | **A — jump** | already correct; primary action on the most reachable button |
| **X** (left) | **B — punch/grab** | the N64 pad's B sits up-and-left of A — X occupies that same relative position on this pad's diamond. Matching the *label* (B→B) would put B on the wrong side and invert every A+B combo (dive, long-jump, grab-throw). Also: this is what already works today, by the ghost-default accident above — formalizing it changes nothing the user has learned. |
| **Y** (top) | C-up | top of diamond → top of C-cluster |
| **B** (right) | C-right | right of diamond → right of cluster |
| **L1** | L | shoulder → shoulder |
| **L2** | **Z** | trigger → trigger; real N64 hardware puts L and Z close enough for one finger to reach either depending on grip, and most games lean on Z (crouch/dive) far more than L |
| **R1** | R | shoulder → shoulder; also cycles SM64's camera, matching real N64 behavior |
| **R2** | C-down | remaining C direction |
| right stick | all four C's, analog | **already true today**, confirmed live in RetroArch's own Options menu — no MDB change needed for this part |
| D-pad | *(open — see Open Item 2)* | |

**Consequence not yet decided:** binding Y/B/R2 to individual C-buttons, on top of the right stick already covering all four, means N64 games that use C as discrete item buttons (Zelda OoT/MM) get real presses, while games that use C as a camera stick (Mario) get analog motion — RetroArch allows both an axis and a button bound to the same function simultaneously, so this is not a conflict, just an additive design choice not yet built.

## Open items — decisions needed before implementing

**1. Verify RetroPad A and RetroPad X's N64 function under `alt-map = True`.**
The pre-fix `Port 1 Controls` readout showed these two slots' *only* descriptor as their C-button overlay identity (`"(C1)"`, `"(C4)"`) — neither had a plain `"A Button"`/`"B Button"` label the way RetroPad B and RetroPad Y did. That means it is not yet confirmed whether RetroPad A (physical right/labeled-B) and RetroPad X (physical top/labeled-Y) do anything at all now that the overlay is gone. Needs: read `Port 1 Controls` again, now, post-fix, for these two rows specifically.

**2. D-pad: keep it as the real N64 d-pad, or repurpose it as the C-button cluster?**
Raised and reasoned through with the user but never decided:
- *Keep it* (default/no change): a handful of N64 titles use the real d-pad for menus; SM64 and Zelda are not among them.
- *Repurpose it*: geometrically ideal for C-buttons (a discrete four-way diamond, same shape as the C-cluster), and N64's d-pad is the least-used control on the real hardware. Cost: any of those few d-pad-dependent titles lose it entirely.
This is a real trade with no clearly-correct default — needs the user's call.

**3. Fix the ghost-default bug before or alongside the coverage-invariant plan?**
Two shapes, not evaluated against each other yet:
- Make `build_mapping()` construct every `ControllerMapping` pre-cleared (empty strings) and have each semantic table's `s.name = "..."` branch be the *only* source of a non-empty field — closest to "unassigned actually means unassigned," but touches the one function every core's binds flow through and requires the golden snapshot (`tests/retroarch/mapping_snapshot_golden.h`) to be regenerated and audited line-by-line (same discipline already used for the PS1 right-stick fix in `64823cb`).
- Change only the defaults for fields known to collide with something live (at minimum `l_btn`/`r_btn` on the N64-pad table, since those ghost onto real N64 L/R/Z) and leave the rest — smaller, faster, but leaves the general footgun in place for the next core or pad.
Either way, `core_slot_coverage.h`'s `slot_value()` (in the blocked worktree) needs to distinguish "field left at struct default" from "field explicitly set to that value" — which the current `std::string` fields cannot do at all. That likely means the fix has to happen in `ControllerMapping`/`build_mapping()` first; the coverage test cannot be fixed around it.

**4. Bake the finished layout into the shipped C++ table, or ship it as a RetroArch core remap file?**
- **Shipped table** (`semantic_ps_style()`'s N64 branch): applies to every box automatically, is unit-tested, shows up in the golden snapshot, requires a deploy to change.
- **`.rmp` core remap file**, saved once via `Quick Menu → Controls → Save Core Remap File`: applies to every N64 game immediately with no code change, but lives only on the box it was saved on, is not in git, is not tested, and would not exist on a freshly cloned or golden-imaged unit unless separately captured into the clone process.
No decision made. Note `auto_remaps_enable = "true"` is already set box-wide, so the remap-file path is live and available today if that is the preferred route for personal tuning, independent of whatever ships in the table by default.

## Concrete next steps, in a sensible order

1. Re-open `Port 1 Controls` in a running N64 game and read out RetroPad A and RetroPad X's rows (Open Item 1) — five minutes, unblocks confirming the rest of the designed layout is still accurate post-`alt-map`-fix.
2. Decide Open Item 2 (d-pad vs. C-cluster) — needs the user, ideally with a controller in hand and a Zelda or Mario session to feel out.
3. Decide Open Item 3's scope (ghost-default fix: full clear-before-fill, or targeted) — this gates resuming `feat/coverage-invariant` Task 1.
4. Decide Open Item 4 (shipped table vs. personal `.rmp`) — could reasonably be "both": ship a sane default in the table, let personal tuning live in a remap on top.
5. Once 1–4 are settled: implement the layout in `semantic_ps_style()`'s N64 branch, TDD as usual, regenerate and audit the golden snapshot, deploy, and re-verify hands-on exactly the way `alt-map` was verified today — a real play session, not just a config read.

## Related artifacts

- Fix already shipped: `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp` (`mupen64plus-alt-map = "True"`), commit `48a5376` on `main`.
- Blocked plan: `docs/superpowers/plans/2026-07-29-controller-coverage-invariant.md`, branch `feat/coverage-invariant`, worktree `.worktrees/coverage`. Task 1 report: `.worktrees/coverage/.superpowers/sdd/task-1-report.md`.
- Design principles for cross-pad ergonomics generally (frequency-over-position, triggers-to-triggers, etc.): `docs/superpowers/specs/2026-07-29-controller-coverage-invariant-design.md`.
