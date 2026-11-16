# Tube CRT engine (Sound Factory port) — design

Status: APPROVED by the owner 2026-11-15. **Amended the same day** after
Phase 0 (two timing rounds on the owner's Pi 5, raw logs in
`docs/engineering/measurements/2026-11-15-tube-phase0*-journal.txt`) and the
owner's frame-rate decision. §1, §2 and §5–§7 are built (plans 1 and 2).
§3, §4, §8, §9, Approaches, Acceptance, Build order and Risks now describe
what plan 3 (`docs/superpowers/plans/2026-11-15-tube-crt-renderer.md`)
builds; every number in them is measured unless it says otherwise.

## Intent

The owner built a CRT effect engine for the Sound Factory panel
(`../mdb-sound-factory/engine/src/panel/crt.metal` and its CPU side) and
likes how it looks: 44 numeric settings plus a mask choice, six preset
looks, and effects the Magic Dingus Box does not have (real mask patterns,
brightness given back after the mask, curvature and glass, hum bar, noise,
chroma bleed, afterglow, a true line raster, worn-tube character).

He wants that engine on the Magic Dingus Box so its CRT look is more
realistic, with every parameter adjustable, without bogging down a 2 GB
Pi 5.

## Goals

- A third CRT engine, **Tube**, next to Classic and Enhanced, that draws
  the Sound Factory tube over the main menu and playlist video.
- Every Sound Factory row exists on the box with the same key, the same
  0–100 range and the same meaning, so a look tuned on one product carries
  to the other (two rows excepted: see "Sizes").
- Six preset looks (PVM, Monitor, Trinitron, TV, Arcade, Everything) and a
  derived CUSTOM state, exactly Sound Factory's model: a look is a starting
  point, each moved row is stored on top of it.
- All 45 rows are editable **on the TV** in the Settings menu and **in the
  Content Manager** on a phone or laptop, and a change from either shows on
  the TV within a fraction of a second.
- Smooth playback on a 2 GB Pi 5 at 1080p60 with a playlist video playing,
  for every shipped look: a **lean** look holds 60 fps, a **rich** look an
  even 30 fps (§4, acceptance numbers below).
- A still look on a still menu still lets the box rest (the redraw gate
  keeps working).
- Pi 4B units behave exactly as today.

## Non-goals

- Games. RetroArch owns the screen during a game; the Sound Factory look
  as a RetroArch shader is a later, separate project.
- The Media Browser. Its menus keep their own `mb_*` CRT values on the
  Classic overlay path; movies stay clean.
- The Tube engine on the Pi 4B, even a reduced version. Possible later.
- Removing or changing Classic or Enhanced. Both stay, pixel for pixel.
- Sharing one shader source between the two products. The box gets a
  translation; the two files are kept alike by hand and by tests.
- Mouse/pointer mapping through the bend (Sound Factory's `pictureCoord`
  C++ twin). The box has no pointer.
- Power-off collapse on game launch or on a channel change. The shader
  inputs are ported; only the two uses under "Transitions" are wired.

## Anti-goals

- A look that makes playlist video judder. A realistic tube that drops
  frames is a failure.
- A menu that keeps the GPU busy forever because a look "moves" when
  nothing on it is moving.
- Anything that flashes. Sound Factory's flash-safety caps come across
  with the effects, each on the line that draws it, and the 30 Hz
  interlace alternation is deliberately not a Tube row.
- A slider that means something different on the two products.
- A customer landing in 45 sliders by accident with no way back to a
  known-good look.

## Constraints

- **Dual-board contract** (CLAUDE.md). One image, one release. Board
  differences resolve at runtime through `platform::PlatformProfile`; no
  `#ifdef`. The Tube engine is a Pi 5 feature and needs a profile gate.
- **GLES 3.0 on V3D**, drawn into the kiosk's existing context. No new
  system dependency in the kiosk binary.
- **The render thread never blocks**: no synchronous `glReadPixels`, no
  recurring file write on the main loop (WatchdogSec=10; see the
  `StatusWriter` rule in CLAUDE.md).
- **The kiosk is the only writer of `config/settings.json`.**
- **The real budget is GPU time and memory bandwidth, not RAM.** The extra
  textures total a few MB. What is scarce: fragment time per frame at
  1920×1080 (2.25× the pixels of 720p) while the Pi 5 software-decodes
  video on the CPU and shares memory bandwidth with the GPU.
- **The kiosk runs for weeks.** A float seconds-since-boot loses precision
  (the reason `ui/crt_time.h` wraps at 60 s). Sound Factory passes raw
  `time` to its shader; the box cannot.

## Approaches

**What was chosen before anything was measured.** Three were considered.
**A**, a faithful port of `crt.metal` as ONE GLSL ES shader, each effect
skipped by a uniform test at its neutral value — chosen. **B**, the same
shader compiled per look with only the effects that are on — held in
reserve. **C**, growing the Enhanced shader under its seven sliders —
rejected (the sliders would not match Sound Factory's, and Enhanced works
on gamma-encoded colour, where giving back the mask's light looks wrong).
The data model (copied row table, look table and row-to-input arithmetic)
was common to A and B and is built.

**What the measurements showed** (Pi 5 2 GB, V3D 7.1, Mesa 25.0.7, Modern
TV 1920×1080@60, picture 1440×1080, a playlist video playing, Media
Browser stack up; GPU ms per frame):

- A plain copy of the picture costs **1.8 ms**. The budget for a look to
  hold **60 fps with a video playing is about 8 ms** of tube GPU time
  (5.1 ms held 60 fps with 0.3–0.7% late; 12.5 ms ran 52–60 fps, 3–16%
  late). A look at **15.6 ms held a rock-steady 30 fps** (33.33 ms, max
  33.42).
- **A is dead.** The one shader costs **20 ms with every effect off**
  (1302 instructions, the GPU drops to 2-thread mode); the six looks cost
  27–61 ms.
- **B helps and is not enough.** Compiled per look, PVM is 15.6 ms and TV
  43 ms; a compiled variant pays for its own effects even with their rows
  at neutral; a big program's first draw is a 285–645 ms hitch.
- **"Pipeline 2"** (the spike's restructure: hardware sRGB decode, a
  signal pass, a line pass, a small output pass with the mask from a
  texture and the glass baked, the glow chain, the afterglow history)
  brings the heavy looks from 43–60 ms to **~30 ms** and leaves the light
  ones at **14–19 ms**. Per pass: a single output pass with bend +
  scanlines + mask + roll-off is **7–9 ms** by itself; **every extra
  full-picture pass costs 2–4 ms whatever it computes**; the
  quarter-resolution glow chain ~3 ms; afterglow 2–4.5 ms; a signal pass
  at half width saves 2.2–4.5 ms; **a signal pass with nothing to do
  still costs 3 ms**; the cheap beam saves 0.7–1.7 ms; the bend in the
  vertex stage saves nothing; an 8-bit signal texture saves 0.3 ms.
  Programs are 130–282 instructions, first draw ≤ 34 ms.
- **Mesa's shader compiler hung for nine minutes** on one compile-time
  variant (the bench's "everything, sharpness 50" set, right after the six
  looks): the render thread sat in `libgallium` at 100% of a core until the
  process was killed. On a customer box that is a frozen TV, a watchdog
  restart, and the same hang on the next start.
- Kiosk CPU stayed at 8–17% of one core and ~1 GB stayed available: the
  limit is the GPU.

Conclusion: no arrangement reaches 8 ms for a look with glow or afterglow
at 1440×1080; the Pi 5 holds 60 fps with a video only for a look that is
essentially one output pass.

**What is chosen now: pipeline 2, two frame classes, a fixed program
table.** The owner's decision (below) splits the looks: a **Lean60** look
is one output pass (plus a baked glass texture, a mask texture and, when
ABL is on, a tiny mean probe) and draws at 60 fps; a **Rich30** look adds
whatever passes its rows need (glow chain, signal pass, line pass,
afterglow history) and draws an even 30 fps. The class is derived from the
setting, never chosen, and **what the lean pass may draw is itself
measured** before anything is built on it: the round-2 numbers predict a
lean output pass of ~9.3 ms against an inferred 8 ms budget, so the lean
set and both budgets are fixed by a timing run of the lean pass as
specified (§3, Acceptance). Programs come from a small, closed table that
is compiled in CI and compiled-and-drawn through their real passes on a
Pi 5 before the feature is switched on (§3); no program is ever assembled
from an arbitrary combination of rows. Approach C stays rejected.

## What the owner decided

- Scope: main menu and playlist videos first; games later.
- Controls: preset looks **and all 45 rows on the TV menu**, and the same
  45 rows in the Content Manager with live update.
- Approach A (before the measurements; superseded by them — see
  Approaches).
- **Frame rate, after Phase 0: "Both, per look."** A lean look runs at
  60 fps and a rich look at 30 fps, and the row table marks which rows
  force 30 fps. (Of the four options put to him — all 30 fps, all lean at
  60 fps, both per look, 720p output — this one.)

Everything below this line that is not one of those is the author's or
the controller's call, marked **(call)** where it is a choice the owner
may want to change.

## Design

### 1. The data model (pure C++, unit-tested on the Mac)

New directory `magic_dingus_box_cpp/src/tube/`, Renderer-free and
I/O-free, compiled into a Mac test suite:

- `tube_rows.h` — the 44 effects in Sound Factory's canonical order with
  the same keys (`brightness` … `purityPosition`), the five masks
  (`grille`, `magentaGreen`, `dots`, `slot`, `none`), the six looks.
  Append-only, as in Sound Factory.
- `tube_looks.h` — the look table. Values start as a copy of
  `kCrtLookTable`; the two whole-pixel size rows are re-chosen for a TV
  (see "Sizes"). A pinned test holds every number, as Sound Factory's
  `kPinned` does. **The numbers are the owner's to judge on the TV**; a
  retune is an edit to the table and its pinned test, nowhere else.
- `tube_setting.h` — `look` + sparse moved rows + optional mask; the
  resolver (look values with moved rows on top); `custom` and the list of
  moved keys are derived, never stored.
- `tube_inputs.h` — `TubeUniforms` and `tube_uniforms_for(...)`: the port
  of `tubeUniformsFor`, `tubeMaskGain`, `tubeHumStrength`,
  `tubeCharacterFrame`, `tubeSignalMotion`, `tubeWarmupGain`,
  `tubeSettleFrames`. Same curves, same constants.
- `tube_groups.h` — the ten groups in Sound Factory's order (PICTURE,
  SCANLINES, MASK, LIGHT, GLASS, SIGNAL, SUPPLY, CHARACTER, SYNC, COLOUR
  PURITY), each row's label and two-line hint. One table feeds both the TV
  menu and the web page (see §6), so the two cannot disagree.

Picking a look clears the moved rows **(call)**: the look is then exactly
the look. "Reset to look" does the same without changing the look.

**Persistence** (`config/settings.json`, written by the kiosk only):

```json
"display": {
  "crt_engine": "tube",
  "enhanced_crt_enabled": true,
  "tube": { "look": "tv", "rows": { "hum": 20, "afterglow": 35 }, "mask": "slot" }
}
```

`crt_engine` is `classic`, `enhanced` or `tube`. `enhanced_crt_enabled`
keeps being written (true for `enhanced` and `tube`) so a rollback to an
older kiosk, which ignores the new keys, lands on Enhanced rather than
Classic. An absent `crt_engine` is derived from the old flag. `rows` is
sparse; an unknown key or an out-of-range value is dropped with a WARN,
never fatal.

Nothing changes for a box that never selects Tube **(call)**: the default
engine is whatever the box has today, and the first time Tube is selected
it starts on PVM (Sound Factory's default: authentic, and nothing moves,
so the menu still rests).

### 2. The board gate

New `PlatformProfile` field, e.g. `bool tube_crt_engine`:

| Board | Value | Why |
|---|---|---|
| Pi 5 | true | the target |
| Pi 4B | false | fill rate; fielded behavior preserved |
| Unknown | true | dev machines; hide nothing |

It is a POLICY row in the classification table in `platform_profile.h`, so
`MDB_PLATFORM_POLICY_OVERRIDE=pi4` hides it during a rehearsal. Unit tests
in `tests/platform/`.

When the gate is false: the engine row offers only Classic/Enhanced, the
Tube submenus are absent, the web page's tab is not shown, and a `crt_engine: "tube"` found in settings (a Pi 5 backup
restored onto a Pi 4B) **runs as Enhanced without rewriting the file**
(the same shape as `sanitize_for_platform`).

### 3. Rendering

**Substrate.** The Tube engine reuses Enhanced's: `begin_scene_fbo()`
redirects video + UI into the scene FBO (sized to the framebuffer);
`end_scene_fbo_and_composite()` draws the result to the screen; bezel and
toast are drawn afterwards, unaffected, as today. The Media Browser gate
stays: the MB never draws through any CRT engine. Every non-batched GL call
the tube makes is preceded by `flush_ui_batch()`, and every GL object it
owns is released by `reset_gl()` (a game's return) and `cleanup()` and
rebuilt on its next frame. `renderer_crt.cpp` and `main.cpp` decide the
engine with `app::effective_crt_engine(crt_engine, app::tube_offered(state))`.

**The Settings menu is drawn through the tube (call).** Unchanged: it is
drawn inside the scene, Sound Factory tunes its tube the same way, and the
menu's text is where a change to the mask, scanlines or sharpness is
easiest to judge. Under a Rich30 look the menu moves at 30 fps. The moving
rows are bounded by the ported caps; the hardware acceptance checks the
menu is readable with Everything and every wear row at 100. If it is not,
the fallback is to draw the Settings menu after the composite, like the
toast.

**The boot intro (call).** Drawn through the tube, as through Enhanced
today — but **no program is built before READY=1**. The systemd watchdog
only starts at READY=1, and the unit has no start timeout of its own, so a
compiler hang before it would hold a blank TV for about two minutes. The
first frames of the intro therefore draw through Enhanced; the tube's
programs are built in the frame loop (§ "A compile that hangs"), and the
warm-up plays from the first frame the tube actually draws.

**The picture rectangle (call).** The tube draws over the **content
area main.cpp already computes** — the 4:3 rectangle in Modern TV
(1440×1080 at 1080p, 960×720 at 720p), the whole output in CRT Native, and
the whole output for a full-screen channel film. That computation moves
into one pure function (`video::content_rects`) that both the drawing and
the tube use; the tube never recomputes it. The pillarbox bars are cleared
to black. Consequences:

- *The Channel Guide's picture window* is part of the scene, so the guide,
  its window and the video in it are drawn through the tube together.
- *A full-screen channel film* (no cabinet art) is a 1920×1080 picture, a
  third more pixels than the budgets were measured at. **It always draws as
  Rich30.** Like every picture (below, "When a look cannot afford an
  effect") it drops the afterglow pass, then the glow passes, when that
  gets the look's predicted cost under the rich budget. **When even that
  does not fit, the film is drawn by the Enhanced engine (owner's decision,
  2026-11-15)**: "On a full-screen film, when the chosen Tube look is too
  heavy for it, the box draws the film with the old Enhanced engine, and
  says so in the controls." Measured on a real 1920×1080 film (window 5,
  `docs/engineering/measurements/2026-11-15-tube-budgets-pi5.txt` section
  8): PVM without afterglow 23.22 ms and Monitor 22.76 ms held 30 fps;
  Trinitron 30.72, Arcade 30.70, TV 32.73 and Everything 32.85 ms drew at
  20 fps — so those four yield the film to Enhanced. He declined the
  alternative (the film at 20 fps with the "too heavy" notice): a smooth
  film matters more than the exact tube look, and Enhanced is what films
  look like today. The decision is `tube_yields_to_enhanced()`: an ACTUAL
  full-screen film (`movie_full_screen`, only in the Channel Guide style)
  whose picture is larger than the reference and whose plan is over the
  budget even after the ladder (`plan_passes_and_drops()`'s `over_budget`).
  A large picture that is not a film (CRT Native into a 1080p framebuffer
  until the restart that applies the mode) never yields: it keeps the
  ladder and, if nothing fits, draws as asked, late, with the notice. The
  renderer hands a yielded film over like a Media Browser frame (the tube
  stops drawing, its targets go, its programs stay; no warm-up and no
  rebuild on the way back), NOT as an engine change, because an owner
  flipping between a film channel and a normal one would otherwise replay
  the warm-up and rebuild the programs on every flip (one exception: a box
  that boots straight into a film channel with a heavy look draws its first
  Tube frame, warm-up and first program builds included, at the first flip
  to a normal channel). NOT yet run on hardware. The status says it
  (`effective_engine` "enhanced" while it happens, `yielding`, and
  `film_engine`: what a film would get with the current setting), the TV
  menu's Look row adds " · films: Enhanced" (shortened by a fixed ladder
  so the label always fits its row), and the web page warns before a film
  plays; none of the notes appears where no film can be full-screen (the
  List style) or when Tube is not the engine in effect. Degauss counts only
  while the tube draws the screen: the TV greys its row and the web page
  its button while Enhanced draws a film. A setting that does not fit a film after the ladder now
  yields: PVM with TRUE RASTER raised draws a film with Tube without
  afterglow at 0, and with Enhanced once it no longer fits. Per-film picture fill
  (`video/picture_placement`) only changes where the video sits inside the
  picture; the tube does not see it.
- *Bezels with a recorded opening*: the tube's opening is the content
  area, not the bezel's window; the bezel art is drawn on top as today and
  covers the tube's rounded corners and rim where its window is smaller.
  Lining the bend up with each bezel's window is a follow-up for the
  owner's eye; the opening data exists for it.
- *CRT Native* (owner question 1 in the Addendum, answered **(call)**):
  the picture's fit, corner radius and margin are neutral too — no inset,
  no rounded corners, flat fit 1 — on top of §5's neutral rows. A real tube
  draws its own corners.

**Frame class (call).** A pure function of the resolved setting and the
picture: **Rich30** if any row the lean pass cannot draw is away from
neutral, or the picture is larger than 1440×1080 (a full-screen film);
otherwise **Lean60**. Rows a CRT Native picture draws at neutral count as
neutral there. Seven rows are always rich — GLOW, AFTERGLOW, SHARPNESS
(neutral 50), CONVERGENCE, CHROMA BLEED, TRUE RASTER, COLOUR DRIFT — because
each needs a pass of its own. **Which of the rest the lean pass may draw is
decided by measurement, not assumed:** plan 3's Task 1b prices the lean
pass on the Pi 5 ingredient by ingredient (the bend, the beam exact or
cheap, tone, the three line displacements, noise/hum/flicker and the band,
the brightness probe) and records the richest set that fits the lean budget
with at least 99% of frames on their vblank. A row whose ingredient is not
in that set forces 30 fps too. Measured and decided (Task 1b): the
lean set is the bend, the beam (cheap form), tone and the line displacements;
the motion block (noise, hum, flicker, rolling band, purity) and the probe
(ABL, breathing) are not lean, so those rows draw at 30 fps too. GLOW SIZE
alone forces nothing (it sizes a glow that is off at GLOW 0). The rows
that force 30 fps carry a flag from the same source, which the TV menu and
the web page show beside them, and the Look row shows "60 fps" or
"30 fps". **If no useful lean set fits** (the measured base pass alone
misses the lean budget): every shipped look is Rich30, the Lean60 path stays
for any custom mix that qualifies, and the owner is told.

**When a look cannot afford an effect (owner's decision, 2026-11-15).** The
box draws afterglow whenever the look can afford it at the current picture
size; when it cannot, afterglow is dropped first, then glow, automatically,
with no look's or row's value changed, and the controls say when an effect
is not being drawn. This applies at EVERY picture size, not only above the
1440×1080 reference (the rule before this decision). The owner chose it
from three ways measured on the Pi 5 to bring TV and Everything under the
ceiling (`docs/engineering/measurements/2026-11-15-tube-budgets-pi5.txt`,
RESULT and section 7): afterglow off (TV 22.6, Everything 22.5 ms; pacing
5-9 and 2 late of ~1800 a minute), the cheap beam on the rich outputs (22.6
and 22.7 ms, but up to 9-33/255 different pixels) and chroma bleed off
(23.0 and 22.8 ms). The decision is one pure function,
`tube::plan_passes_and_drops()`: the plan for the setting and the picture
and, while its predicted cost (window 4's kiosk-measured per-pass prices,
scaled by pixel count) exceeds the rich budget (23.1 ms), the same plan
without afterglow if that fits, else without afterglow and glow if that
fits, else the plan as asked with nothing dropped (dropping what does not
get the look under the budget buys nothing visible). It reads the setting
and the picture only, so it cannot flap; the class is decided first and
kept (a look that drops afterglow is still Rich30). Its result is published
as the status block's `not_drawn` list (every row it silences: AFTERGLOW;
GLOW and GLOW SIZE) and shown on those TV-menu rows (" · not drawn") and on
the web page ("Not drawn at this picture size", the slider still editable).
As predicted: at a 1440×1080 picture TV and Everything drop afterglow and
keep glow, the other four looks drop nothing; at 960×720 (720p Modern TV)
and the 1280×720 CRT Native picture nothing is dropped; a 1920×1080 film is
in the film bullet above (films a look cannot afford yield to Enhanced).
Window 5 ran the rule built in: TV and Everything measured 22.59 ms with
`not_drawn` ["afterglow"] and held 30 fps; Trinitron 22.87 and Arcade
22.93 ms kept everything.

**The shipped looks in two classes (call).** As copied from Sound Factory
every look is rich (each has GLOW > 0 and SHARPNESS ≠ 50). Task 1b
decided pvm_lean: no. PVM's ABL 20 and breathing 15 need the brightness probe;
the probe stalls a frame at every run (91.5 % on time with it), so it is out of
the lean set. PVM-lean without the probe was not established: its one run was a
disturbed outlier (78.8 % on time, max interval 283 ms), and Task 15's
ten-minute runs decide it. Until then PVM and Monitor stay exactly as Sound
Factory has them, at 30 fps, and no look value changes. The 60 fps class is
for a custom mix that leaves every 30 fps row neutral. Trinitron, TV, Arcade
and Everything stay Rich30. No look's values were changed to meet the rich
budget: measured with both optimisations (window 4), PVM, Monitor, Trinitron
and Arcade hold 30 fps as shipped, and TV and Everything give up their
afterglow automatically (the owner's decision above) instead of the
Acceptance ladder taking rows to neutral.

*Rows that differ from Sound Factory's, for the owner* (plan 3 keeps this
table current; the parity script lists each cell as deliberate):

| Look | Row | Sound Factory | Box | Why |
|---|---|---|---|---|
| all six | lineSize, maskSize | see plan 1 Task 2 | see plan 1 Task 2 | whole TV pixels (§5) |
| pvm, monitor | (none) | Sound Factory's | the same | Task 1b: PVM-lean needs the probe (ABL 20, breathing 15), which is not lean; PVM-lean without it was not established (one disturbed run) and Task 15 decides, so no row differs for now |
| rich looks | (none) | Sound Factory's | the same | no value changed (owner, 2026-11-15). What is not DRAWN depends on the picture: at 1440×1080 TV and Everything leave out afterglow; on a 1920×1080 film PVM leaves out afterglow, and Trinitron, Arcade, TV and Everything yield the film to the Enhanced engine (owner, 2026-11-15). The setting keeps every value, and the controls say what is not drawn |

**The pipeline (call).** The spike's pipeline 2, with these fixed choices:

| Pass | Program | Size | Exists when | Class |
|---|---|---|---|---|
| Glass bake | `glass` | picture | rebaked only when a glass input (corner, margin, vignette, rim, glass black, reflection) or the picture size changes — ~4 ms, measured | both |
| Mask tile | — (CPU) | one period, NEAREST + REPEAT | rebuilt on mask, pitch or strength change | both |
| Mean probe | `quarter` | 64×64 → mip to 1×1 | ABL or BREATHING > 0; 5 times a second | rich only: the probe stalls a frame at every run (91.5 % on time with it), so it is not in the lean set |
| Quarter picture | `quarter` | ¼ × ¼ | GLOW > 0 or CHROMA BLEED > 0 | rich |
| Glow blur H, then V | `blur` | ¼ × ¼ | GLOW > 0; sigma from GLOW SIZE | rich |
| Signal | one of four `sig_*` | picture height × (picture width, or half when CHROMA BLEED or TRUE RASTER > 0) | SHARPNESS ≠ 50, CONVERGENCE, CHROMA BLEED, COLOUR DRIFT or TRUE RASTER | rich |
| Lines (5-tap tent per line) | `lines` | signal width × lines | TRUE RASTER > 0 | rich |
| Output | one of ten `out_*` | picture | always | both |
| History → screen | `copy_encode` | picture | AFTERGLOW > 0 (the output draws into the history) | rich |

- **Hardware sRGB decode.** While Tube draws, the scene texture is stored
  as `SRGB8_ALPHA8` and written with `GL_FRAMEBUFFER_SRGB_EXT` off
  (`GL_EXT_sRGB_write_control`), so the UI and video bytes are exactly
  today's, and the tube samples it with `GL_DECODE_EXT`
  (`GL_EXT_texture_sRGB_decode`): the texture unit decodes before it
  filters, in linear light as Metal does, and no `pow` runs. The texture is
  re-specified only when it is new or resized, never per frame; when Tube
  stops drawing it goes back to `RGBA8`. Without the extensions (a dev
  machine) the same programs decode in the shader (exact sRGB).
- **A lean look is ONE pass.** Its tone and the three line displacements
  (when Task 1b's set includes them) are folded into the output pass, which
  reads the scene directly; a signal pass that would do only that costs
  3 ms and is never run. A rich look with a signal pass does tone and
  displacements there instead, once per signal texel.
- The bend stays per pixel (the vertex mesh bought nothing). The signal
  and line textures are `RGBA16F` where the driver renders half floats
  (sharpness overshoots above 1), else `SRGB8_ALPHA8`. Quarter, glow and
  history are `SRGB8_ALPHA8`. The glass is `RGBA8` with its light stored
  ×32 (it is never above 0.02), so a 1440×1080 picture's textures stay
  under 32 MiB in every class (worst case: a full-width signal, the
  afterglow pair and the glass, ~32.3 MB).
- **The beam.** The lean programs use the beam form Task 1b chose
  (the cheap beam: one sigma from luma; round 3 measured it 0.7 ms cheaper
  than the exact beam inside PVM-lean, Monitor-lean and the full set —
  the 0.7–1.7 ms range is round 2's earlier estimate);
  the rich programs use Sound Factory's per-channel beam unless the
  acceptance ladder switches them to the cheap one.
- **The owner must accept two softenings (calls).** (1) With CHROMA BLEED
  or TRUE RASTER on — all four rich looks as shipped — the signal pass runs
  at half width, which halves horizontal detail, menu text included
  (measured saving 2.2–4.5 ms; full width does not fit the rich budget).
  (2) Warm-up's softening (Sound Factory lowers sharpness for 1.5 s) is
  absent on a lean look, which has no sharpness stage; the warm-up's light
  ramp is still drawn.

**A fixed program table (call).** Nineteen programs, listed in plan 3,
and nothing else is ever compiled: `glass`, `quarter`, `blur`,
`copy_encode`; four signal programs (`sig_sharp`, `sig_sharp_chroma`,
`sig_guns`, `sig_guns_chroma`); `lines`; two lean outputs
(`out_lean_still`, `out_lean_moving`, which compile exactly the
ingredients of Task 1b's lean set); eight rich outputs (source: scene,
signal, signal + raster, raster only; each to the screen or into the
afterglow history). The mask type is data (the tile), not a variant. Inside
a program the cheap blocks (tone, tint, noise, flicker, hum, the band,
purity, power, ABL) are switched by uniform tests; the variants split only
where the measurements found a structural cost — an extra pass, multi-tap
reads, a different target. Every reachable setting maps to exactly one row
per pass (a pure function with an exhaustive test), every program is
compiled by a GLSL ES validator in CI on every push, and every program is
compiled and drawn **through its real pass, into its real target format**,
on a Pi 5 before the feature is switched on. Programs are built when Tube
starts drawing or when the plan needs one not yet built — never per frame;
a program build is 3–35 ms of compile, link and first draw (measured,
pipeline 2), and frames whose programs are not all built yet draw through
Enhanced.

**A compile that hangs (call).** Mesa compiles its real variant of a
program at the first draw with the real target and texture formats, so the
guard covers that draw, not a scratch one. Before a new program is
compiled the kiosk writes a marker file in `/dev/shm` (memory, not the SD
card; it survives a kiosk restart and not a reboot) naming every program
not yet drawn; it removes the marker once the first composite that draws
them all through their real passes has returned. A kiosk that finds the
marker at start died inside a build — the watchdog killed a hang — so it
draws Enhanced instead of Tube until the next reboot, logs the programs'
names as an ERROR and says so in the status the web page reads. The owner's
settings are not changed. A build that merely **fails** (compile, link or
an incomplete target) is not sticky: that frame draws Enhanced and the
build is retried when the setting or the engine changes.

**Linear light.** Unchanged in intent: the arithmetic is in linear light;
UI and video drawing are untouched (the scene's bytes are today's).

**Precision and time.** `precision highp float`; no raw time uniform. The
CPU computes each moving phase in double once a frame and hands the shader
wrapped values. **Speed rows no longer jump while tuned (call; owner
question 2 in the Addendum):** the rolling band's, colour drift's and
flagging's phases are *accumulated* by the renderer — each frame adds
`dt / period` (dt capped at 0.1 s), wrapped to one cycle — instead of
`seconds / period`, so a step of BAND SPEED, DRIFT SPEED or BEND SPEED
changes only how fast the effect moves from where it is. Tracking keeps
Sound Factory's behaviour: changing any tracking row restarts its slip
clock, so the new setting auditions at once. Hum, flicker, noise, jitter
and degauss have fixed periods and stay derived from the clock.

**Brightness limit and breathing.** The picture's average light comes from
a mean probe: five times a second the scene is drawn into a 64×64 target
(`quarter`, 4,096 pixels; square so the mip chain halves evenly), mipped to
1×1 and read back **asynchronously** (a pixel-pack buffer and a fence,
polled without waiting). The limiter steps toward it on the frames' own
clock with Sound Factory's 0.1 s constant (`stepBrightnessLimitTo`,
including its "from rest" rule). Never a blocking read; while no read has
landed the last value holds. The probe is expected to cost well under
0.3 ms on the frames it runs (measured 0.02 ms), but it stalls a frame each
run, so Task 1b left it out of the lean set: ABL and BREATHING force 30 fps
(the same table).

**GPU timing.** No GPU state is read back in production frames, except
the mean probe. A debug variable, `MDB_TUBE_GPU_TIMING=1`, wraps each pass
in a `GL_TIME_ELAPSED` query, reads each result four frames later without
waiting, and logs per-pass averages once every 10 s. It is how the
hardware tasks price passes; frame pacing is always measured in a separate
run with it off, because the read-back itself can cost pacing.

### 4. Resting, and the two frame rates **(call)**

**Lean60** draws on every vblank whenever the kiosk draws — video, input,
animation — exactly as Classic and Enhanced do. **Rich30 is a true, even
30 fps:** while a rich look draws, every frame is presented exactly
`round(refresh / 30)` vblanks after the previous one — two at 60 Hz, four
at 120 Hz, two at 50 Hz (25 fps); a mode under 48 Hz already presents at
most 30 times a second and is left alone. Video, the UI, the guide and the
toast all move at that rate then.

How (extending the existing 30 fps machinery in `utils/frame_pacing.h` and
the redraw gate, not a second mechanism):

- The CRT field-rate path (`crt_field_rate`) skips the iteration after a
  drawn one and sleeps past the skipped vblank, leaving the next frame
  ~14.6 ms to render. That is enough for a lean look and is what a lean
  look with a moving row uses on a static menu — 30 fps there, as Sound
  Factory renders moving effects.
- A rich frame needs up to the rich budget of GPU, which cannot start
  18.75 ms after the previous flip and still make the second vblank. So
  Rich30 renders **immediately after the previous flip** and **holds the
  page flip**: after `eglSwapBuffers` (which is when the last pass is
  handed to the GPU) and before the flip is queued, the loop reads the
  clock afresh and sleeps until `round(refresh/30) − 1` refreshes plus an
  eighth have passed since the previous flip completed
  (`utils::present_hold`). The flip is then queued after the intermediate
  vblank(s) and lands on the next one. The bottom-of-loop frame cap does
  not add its post-present gap under the hold, so the next frame's
  rendering starts at once.
- Cases, at 60 Hz: a 10 ms or 20 ms frame waits to 18.75 ms and flips at
  33.3 ms; a 26 ms frame (work already at the GPU) flips at 33.3 ms; a
  40 ms frame flips at 50 ms — a steady 20 fps, counted late, and the
  too-heavy toast follows (§9). A frame drawn after the gate rested (input,
  a signature change) sees a long gap since the last flip and is not held.
  A frame class change mid-video costs its program builds on the change
  frame and resets the pacing monitor.
- **Video frames:** the appsink (5 buffers, `drop`, `sync`) keeps decoding
  on the clock; each drawn frame shows the newest decoded frame. A 30 fps
  video shows every frame; 24 and 25 fps videos show each frame for one or
  two 33 ms frames (at 60 fps today: two or three 16.7 ms vblanks). Audio
  is unaffected.
- **Input:** the loop polls input once per iteration, so under Rich30 once
  per 33 ms instead of once per 16.7 ms; the worst added latency is one
  30 fps frame. Nothing is dropped (evdev queues).
- **Watchdog:** pinged once per iteration — at most every 33 ms under
  Rich30, far inside `WatchdogSec=10`. The hold is a bounded sleep, never a
  wait on the GPU, and nothing holds a lock across it.
- **The redraw gate** keeps its skip logic: a static menu under a still
  look rests once settled, in either class; an input or a signature change
  draws on the iteration it happens, and under Rich30 that frame is held to
  the cadence. Under Tube the Classic/Enhanced flicker and interlacing
  values are not drawn and do not keep the menu awake.

Resting rules (pure, unit-tested, a function of the resolved setting, the
transitions and the settle debt):

- A still look on a static menu rests once settled (only the gate's
  250 ms safety net draws, as for every engine today). "Settled" is
  Sound Factory's `tubeSettleFrames`: the frames the afterglow needs to
  decay to 0.02% and the limiter needs to travel, whichever is longer,
  drawn at 30 fps.
- A look with a moving row (NOISE, JITTER, FLICKER, HUM BAR, ROLLING BAND,
  COLOUR DRIFT, TOP EDGE BEND, and TRACKING while a slip is in flight), a
  warm-up or a degauss in progress, or settle debt left, draws at 30 fps on
  a static menu: lean through the field-rate path, rich as continuous
  drawing under the present hold.
- During video the loop draws every frame of its class.
- The Channel Guide (which uses a guide-inputs signature, not
  `wants_continuous_redraw`) is a static menu like any other: it redraws on
  its own signature (at most once a second) and otherwise follows the rules
  above. A live-tuning change from the web page changes the redraw
  signature (plan 2), so it draws within the 100 ms op-log poll.

### 5. Sizes

Sound Factory sizes effects in millimetres of its 197 mm panel. On a TV:

- Everything sized in millimetres (hum band, rolling band, tracking strip,
  glow radius, convergence, chroma reach, jitter, noise grain, colour
  patch) **scales with the picture**: the 4:3 picture is treated as the
  same 197 mm tube, so each effect keeps its proportion of the picture at
  720p and 1080p.
- The two sizes that must be whole pixels, MASK SIZE and SCANLINE SIZE,
  are counted **in the TV's own pixels** (a fractional pitch beats against
  the panel's grid into moiré, the reason Sound Factory snaps them). The
  row keeps its 0–100 range and one detent (10) is still one pixel, but a
  given row value is not the same physical size as on the Sound Factory
  panel. These are the two rows whose look values are re-chosen for the
  box. Starting values **(call)**: mask pitch 3 px; lines 4 px at 1080p
  (270 lines) and 3 px at 720p (240 lines).

**CRT Native mode** (a real tube through the HDMI converter) **(call)**:
the rows that imitate what a real tube already does are drawn at neutral
there — SCANLINES, TRUE RASTER, the mask, CURVATURE, RIM, GLASS BLACK,
REFLECTION — and shown as not applicable; the picture's fit, corner
radius and margin are neutral there too (§3). The signal, supply and wear rows
still work.

### 6. Controls

**One row table, two front ends.** `tube_groups.h` is the source. The web
page needs the same keys, labels, hints, groups and look values; it gets
them from a JSON file generated from the table and checked in, with a test
on each side that fails when the two differ. No hand-kept second list.
*Added after Phase 0:* the same file carries each row's "forces 30 fps"
flag and each look's frame rate (§3 "Frame class"); the TV tags those rows
"30 fps" in their sublabel and the Look row reads "Look: TV · 30 fps"; the
web page tags the same rows and shows the rate the kiosk reports.

**On the TV** (Settings → Display):

```
CRT Engine: Tube                 Classic / Enhanced / Tube
Look: TV   (3 moved)             PVM · Monitor · Trinitron · TV · Arcade · Everything
Reset to look
Picture        ▸   5 rows
Scanlines      ▸   4 rows
Mask           ▸   3 rows
Light          ▸   3 rows
Glass          ▸   5 rows
Signal         ▸   4 rows
Supply         ▸   4 rows
Character      ▸   6 rows
Sync           ▸   7 rows
Colour purity  ▸   4 rows
Degauss
```

The seven Classic/Enhanced rows show only for those engines; the Tube rows
only for Tube. Inside a group each row shows its value and Sound Factory's
hint as the sublabel:

```
HUM BAR                      20
  soft at low · pronounced at 100
```

Adjusting **(call)**: SELECT on a row enters edit mode (the row is
marked); the knob or D-pad then changes the value instead of moving the
cursor; SELECT or back leaves edit mode. Steps of 5, except the two size
rows (10: one pixel) and MASK, which cycles its five words. The whole
screen, the menu included, shows the change on every step. Settings are saved when edit mode
is left, not per step. This is a new kind of row for the Settings menu,
whose rows today only cycle on SELECT; the decision logic (step, clamp,
enter/leave) goes in `settings_input_logic` with tests, per the project's
rule that screens only spawn, drain and paint.

A customer who wanders in can always get back: Look restores a known look
in one press.

**In the Content Manager.** A new **CRT Look** tab: the engine, the look
picker, Reset, and the ten groups as sliders (0–100, the same steps
available but any whole number allowed), each with its hint and a mark
when it differs from the look. On a box where the gate is false the tab
is not shown.

New route module `admin_display.py`, registered through
`register(app, ctx)` like the others; new script
`static/manager_display.js` loaded after the existing ones, with `?v=`
bumps. Mutating routes are CSRF-protected like every other.

### 7. Live tuning

The kiosk owns the setting; the web admin only asks.

- **Read:** the kiosk publishes the resolved tube state in
  `kiosk_status.json` through `StatusWriter` (already off the render
  thread): engine, look, mask, all 44 values, the moved keys, whether the
  gate is open, and the sequence number of the last web request it
  applied. The page reads it through a `GET` route and polls it about once
  a second, refreshing any slider the user is not holding.
- **Write:** a `POST` validates against the row table and writes one small
  request file in the data directory, atomically. The file is an **op
  log**: each op (one row, the mask, a look, Reset, the engine, Degauss)
  carries an increasing sequence number; the web side appends, drops ops
  the kiosk has already handled and keeps only the newest op per row, so
  no change is lost while dragging and no older change can undo another
  row.
- **Apply:** the kiosk checks for the request about ten times a second (a
  stat, not a write), applies it in memory at once, and saves
  `settings.json` once changes have stopped for about a second.

Requirements the builder must meet, however the file protocol is shaped:
no slider movement is lost while dragging; a request that was already
applied is never applied again after a restart (it would undo later edits
made on the TV); an edit on the TV while the page is open shows up on the
page; a kiosk older than this feature ignores the file. The request file
holds no secrets, is excluded from deploy/OTA rsyncs like the other queue
files, and is deleted on clones by `first_boot.sh`.

### 8. Transitions **(call)**

The shader's power, warm-up, spot and degauss inputs come across. Two uses
are wired in this project:

- **Warm-up** (Sound Factory's 1.5 s) when the Tube engine starts
  drawing: at kiosk start (from the first frame the tube draws, §3), and
  when the effective engine becomes Tube (from the menu or the web page).
  Not after a game returns, not after the Media Browser closes: the tube
  was never switched off.
- **Degauss** on request: the TV menu and the web page each add one to
  `tube_live.degauss_requests` (plan 2); the renderer starts Sound
  Factory's 1.4 s decaying wobble whenever that counter has risen since the
  last frame it drew, and counts it "moving" for 1.5 s so a static menu
  shows it whole.

Not wired now: the power-off collapse to a dot.

### 9. When a look is too heavy **(call)**

Every shipped look is tuned to pass its class's budget (Acceptance). A
custom mix can still exceed it — a Lean60 mix at the edge of the lean set,
or a Rich30 mix of every costly row. While the Tube engine draws, the kiosk
counts presents that miss their class's cadence (an interval over half a
refresh past one refresh for Lean60, past `round(refresh/30)` refreshes for
Rich30) and logs the count per minute in the journal next to the UI
draw-call line. If a video is playing and more than 5% of frames over 30 s
miss, it shows one toast ("This CRT look is too heavy for smooth video")
and sets `too_heavy` in the status the web page reads, until the setting or
the frame class changes. It does not change the owner's settings.

## Acceptance

**The budgets are measured, not inferred (call).** Phase 0 bracketed them —
60 fps held at 5.1 ms and missed at 12.5 ms; 30 fps held at 15.6–18.8 ms
and fell to 20 fps at 25.7 ms — so the starting values were 8 ms
(Lean60) and 24 ms (Rich30) of tube GPU per frame at 1440×1080. Plan 3's
Task 1b replaced them by rule, giving **11.8 ms** (Lean60) and **23.1 ms**
(Rich30): the largest measured GPU cost (timers on)
whose separate pacing run (timers off) still held ≥ 99% of frames at the
class's cadence over 10 minutes with a video playing, **minus 0.5 ms**
(Lean60) or **1.0 ms** (Rich30). The numbers below are those budgets.

On a 2 GB Pi 5, Modern TV at 1920×1080@60, a 960×720 H.264 playlist video
playing, for **each of the six looks**:

- Its passes total at most its class's budget (GPU timing run), and
  ≥ **99%** of frames land on their cadence over a 10-minute pacing run
  with the timers off: every vblank for Lean60, every 33.3 ms for Rich30.
- **A look that misses takes its costly rows to neutral, one at a time, in
  the order of their measured saving** (superseded for afterglow and glow
  by the owner's decision of 2026-11-15, §3: those two are dropped from the
  DRAWING automatically, never from the look's values) — each step removes a whole pass or
  selects a smaller program, never "a lower value" (the measurements show
  values cost nothing; structure does). Rich30: AFTERGLOW 0 (the history
  pass and copy, 2.1–4.7 ms), then GLOW 0 (the glow chain, ~3–4.7 ms), then
  the signal rows to neutral (SHARPNESS 50, CONVERGENCE, COLOUR DRIFT,
  CHROMA BLEED 0: the signal pass, 2.2–10 ms; TRUE RASTER keeps a half-width
  one), then the cheap beam (0.7 ms in round 3's pricing). Lean60: the cheap beam if not
  already, then the moving rows to neutral (the smaller `out_lean_still`)
  if Task 1b priced that saving, then **the look becomes Rich30** (its
  Sound Factory values back). The owner retunes by eye afterwards.
- On a full-screen channel film (1920×1080 picture): PVM (without
  afterglow) and Monitor drawn by Tube at ≥ 99% of the 33.3 ms cadence
  (window 5 measured 23.22 and 22.76 ms, 6-25 late of ~1795 a minute);
  Trinitron, Arcade, TV and Everything drawn by the Enhanced engine
  (status `yielding` true, `effective_engine` "enhanced") at the rate
  Enhanced draws films today, and a flip to a normal channel and back
  shows no warm-up and logs no program build.
- 30 minutes in the enclosure with no throttling (`vcgencmd
  get_throttled` stays `0x0`).
- `MemAvailable` falls by less than 32 MB with the engine on.
- On the static main menu: a still look falls to the redraw gate's idle
  rate after settling (its 250 ms safety net: 4 draws a second, as Classic
  and Enhanced today — the original "zero" was never what the gate does);
  a look with moving rows draws at no more than 30 fps.
- Every program in the table compiles, links and draws once on the Pi 5
  through its real pass, each first draw ≤ 50 ms; and a forced hang in a
  preview build is caught by the watchdog and leaves the next start on
  Enhanced.
- The baseline the owner has today, the Enhanced composite on the same
  picture, is measured and recorded beside the looks.

**If no useful lean set fits:** every look is Rich30, PVM and Monitor keep
Sound Factory's values, the Lean60 path remains for qualifying custom mixes,
and the owner is told — the feature still ships.

On a Pi 4B: with the same release, screenshots of Classic and Enhanced
match the previous release (within the noise floor of two captures of the
previous release), and no Tube row or tab is offered.

Both boards: the Mac suites pass (all 9 with Media Browser ON, 8 OFF),
`verify_box.sh` passes.

## Build order

0. **Measure first** — done (Phase 0, two rounds, 2026-11-15; the spike on
   branch `spike/tube-phase0` is a reference only and is never merged).
1. The pure core: rows, looks, resolver, mapping, persistence, the board
   gate — built (plan 1).
2. The controls: TV menu, Content Manager tab, live tuning — plan 2.
3. The renderer (plan 3), in this order: two hardware tasks with the spike
   — isolate the compiler hang, then price the lean pass and measure both
   budgets; the pure parts (CRT Native fit and accumulated phases, the lean
   set and frame class, the looks, the program table, the shaders and their
   CI validation, the per-frame driver, the cadence, the monitors and the
   build guard); the GL parts, inert until switched on; the 30 fps tags in
   the controls; then hardware: a smoke test, every program through its
   real pass and a forced hang, the per-look budgets (taking rows to
   neutral where needed), the Pi 4B and the policy override.
4. **Switching it on** is plan 3's last task, after the hardware
   acceptance: it flips `app::kTubeRendererReady`, and it is the first time
   anything is deployed to a box's `/opt`. Until then no task changes what
   any box shows. The owner retunes the six looks on his TV after it.

## Testing

- **Pure C++ (Mac):** pinned look table; row-to-input curves against
  Sound Factory's own expected values. The box's CI cannot see the Sound
  Factory repository, so the cases are **copied into the box's tests**
  from Sound Factory's `test_crt_looks.cpp`, `test_crt_geometry.cpp`,
  `test_crt_wire.cpp` and (the arithmetic's cases) `test_tube_inputs.cpp`, with the Sound Factory commit they were taken from
  recorded beside them (`4ef74a6` as of this design). A dev-only script
  compares the box's row keys, look values and curve constants against a
  sibling `../mdb-sound-factory` checkout when one is present; it is run
  by hand before a release that touches the tube and is not part of CI. A
  new Sound Factory row reaches the box by a manual port: append the row,
  re-copy the cases, update the recorded commit; mask gain equals the average the mask passes, for every mask and
  pitch; the flash-safety caps (each moving row at 100 and at five times
  100); phase continuity across clock wraps; settle debt; resolver and
  moved keys; persistence round-trip including old files and bad values;
  the board gate; the redraw-gate decision; the menu's edit-mode logic;
  the request parser. Added for the renderer: the frame class and the
  "forces 30 fps" flag; every reachable setting maps to one program-table
  row per pass, with exactly the capabilities it needs; target sizes and
  bytes; the uniform list each program declares; the accumulated phases
  (no jump on a speed step, wrap, dt cap); CRT Native's neutral fit; the
  mask tile's average; the limiter's step; warm-up and degauss from the
  request counter; the lean set's rows; the full-screen film's predicted
  cost and drops; the present hold at 50, 60 and 120 Hz and the gate under
  both classes; the pacing monitor; the build guard; the flash-safety caps
  present in the assembled GLSL.
- **Shader:** every program in the table, assembled by the same pure
  function the kiosk uses (both decode paths), is written out by a test and
  compiled by `glslangValidator` as GLSL ES 3.00 in a CI job of its own on
  every push. `glslang-tools` is CI-only tooling, not a kiosk dependency
  (it is no `-dev` package, so `system_deps_parity.bats` does not list it);
  on a Mac `brew install glslang`. The kiosk binary carries a marker string
  from the embedded sources, asserted by the `kiosk-build` job and
  `release.yml`. Rendered-pixel checks are done on hardware with
  `screenshot_request` and listed as such; nothing renders off the box.
- **Web (pytest):** validation, CSRF, the gate-closed answer, the request
  file's atomicity and sequence, the generated row file matching the C++
  table; `manager_display.js` follows the classic-script load-order rules.
- **Hardware:** the acceptance section, on both boards.

## Left to the builder

- The file protocol's exact shape (§7), within its stated requirements.
- The blur's tap count and the embedding of the shader files (both
  decided in plan 3; the sRGB method, texture formats, the mean's mip and
  the shader organisation are now decided in §3).
- The web tab's layout and styling, within the Content Manager's existing
  look.
- File and symbol names.

## Risks

- **A lean look cannot hold 60 fps at 1080p.** The round-2 output pass
  written to the screen cost 8.9–9.1 ms; a lean pass adds tone, the scene
  read and the probe (~9.3 ms predicted) against ~8 ms. Plan 3 measures the
  lean pass first (Task 1b) and builds on its table; if no useful set fits,
  every look ships Rich30 and the owner is told.
- **The tv-class looks cannot hold the rich budget as shipped** (25–28 ms
  predicted from the per-pass costs). Expected: half-width signal, then the
  ladder in Acceptance (afterglow, glow) — ~21.5 ms predicted after it.
- **A full-screen film costs a third more**: measured 30.7-32.9 ms for
  Trinitron, Arcade, TV and Everything, over the budget even with afterglow
  and glow dropped; those looks yield the film to the Enhanced engine
  (§3, film bullet; owner's decision 2026-11-15).
- **The compiler hang.** Isolated first (plan 3's first task); every
  program is then compiled and drawn through its real pass on the Pi 5
  before the switch-on; nothing is built before READY=1; and a hang that
  still happens on a box costs one boot's worth of Tube, not a restart loop
  (§3) — checked on hardware with a forced hang under the watchdog. A Mesa update can bring a new one: re-run the
  program-table hardware check when the OS image's Mesa changes.
- **Rich30 doubles input latency** on the screens it covers (16.7 → 33 ms
  worst case). Accepted for rich looks.
- **Afterglow's history** read/write and copy is still the most memory
  traffic this adds, during software video decode.
- **Looks tuned for a 7.9-inch panel at arm's length will not look right
  on a living-room TV as copied.** Expected; the owner's retune follows
  the switch-on.
- **Two shaders drift apart.** Mitigated by shared keys, a pinned table,
  and porting Sound Factory's tests; not eliminated.
- **Two pending designs touch the same screen** (`2026-11-12` crop toggle
  and channel guide). Both draw into the scene the tube reads, so they
  should compose; whoever lands second re-checks. (Both have since landed: see the
  Addendum.)

## Addendum — after the core landed (2026-11-15)

Plan 1 (`docs/superpowers/plans/2026-11-15-tube-crt-core.md`) is built: the
data model, persistence and the board gate. This section records what the
later plans must know that the design above does not say.

### The design was written against a stale tree

The checkout this design was written in was 238 commits behind `main`. The
core was rebased onto current `main` before it was proposed. Re-checked
against current `main`:

- **Unchanged since:** `renderer_crt.cpp`, `renderer_shaders.cpp`,
  `crt_time.h`, `redraw_gate.h`. The rendering analysis in §3 and §4 stood
  then; *superseded:* §3 and §4 were rewritten after Phase 0.
- **The two "pending" designs in Risks have landed** (crop toggle, channel
  guide), along with movie channels. The "whoever lands second re-checks"
  duty is now the tube's. The renderer plan must compose with the guide's
  picture window, full-screen channel films that skip the cabinet art, and
  per-film picture fill (`video/picture_placement`).
- **The Channel Guide does not use `wants_continuous_redraw`**: `main.cpp`
  adds a guide-inputs signature to the redraw gate. §4's resting rules need
  re-checking against that.
- **Bezels now record their opening** (`has_opening`, `opening_x/y/w/h` in
  the app state). §3 said how each bezel's opening sits against the curved
  picture "may need a per-bezel follow-up"; the data for it now exists.
- **Saving is asynchronous** (`settings_persistence` serializes on the
  caller and writes on a worker). Code that reads `settings.json` right
  after a save must call `flush_pending_saves()`; code that reloads a file
  something else replaced uses `reload_from_disk()`. §7's "saves once
  changes have stopped" should use this path, not its own.
- **The web admin's restore replaces `settings.json` whole.** A web page
  that edits the tube must leave `crt_engine` and the old flag to the
  kiosk, as §7 already requires.

### What the core leaves for the later plans

- The "CRT Engine" menu toggle still offers only Classic/Enhanced and reads
  the old flag, so a saved Tube shows as "Enhanced" and one press drops it
  to Classic. Plan 2 replaces the toggle before Tube can be chosen.
- `renderer_crt.cpp` and `main.cpp`'s `begin_scene_fbo` path still gate on
  `enhanced_crt_enabled` alone. Plan 3 switches them to
  `app::effective_crt_engine(crt_engine, profile.tube_crt_engine)`.
  *Updated:* plan 2 introduced `app::tube_offered(state)` (the board gate
  AND the renderer-ready constant); plan 3 uses
  `effective_crt_engine(crt_engine, tube_offered(state))`.
- Rows are written only through `tube::set_row` / `tube::set_mask` (or
  validated with `tube::is_row_value` first): `tube::resolve()` does not
  range-check. This binds Plan 2's request parser.
- `tube::tube_frame(still, resolved, state)` is the per-frame entry point.
  `tube_uniforms_for` gives the still uniforms and may be cached;
  `tube_signal_motion` must never run twice on the same uniforms. A
  renderer with no quarter-resolution pass zeroes glow, chroma bleed, ABL
  and breathing on the still uniforms, as Sound Factory's does.
  *Updated:* plan 3 changes the per-frame API for accumulated phases (§3
  "Precision and time"), and its ABL mean comes from a separate probe, so
  ABL and breathing are zeroed only when that probe cannot run.
- `TubeUniforms` is not laid out for a uniform block; upload it field by
  field.
- The MASK row has a label but no hint lines: Sound Factory's hint depends
  on the chosen mask. Plan 2 supplies per-mask hints.
- Hints copied from Sound Factory quote millimetres of its panel ("0: 8 mm
  · 100: 60 mm"). On a TV those sizes scale with the picture; Plan 2
  decides whether to reword them.
- The Tube option stays hidden behind a "renderer ready" constant
  (Plan 2) until the renderer plan flips it, so a release between the two
  plans is safe: it offers nothing new.

### Open, for the owner, before the renderer plan — answered (calls)

Both now have working answers, flagged **(call)** in §3 for the owner:
(1) CRT Native's fit, corner radius and margin are neutral; (2) the three
speed rows' phases are accumulated by the renderer. The questions as they
were put:

1. **CRT Native still insets the picture.** §5 draws scanlines, raster,
   mask, curvature, rim, glass black and reflection at neutral on a real
   tube, but the picture's fit, corner radius and margin are not on that
   list, so a real tube would show the picture about 3% smaller with
   rounded black corners. Proposed: neutral there too.
2. **Speed rows jump while being tuned.** BAND SPEED, DRIFT SPEED and BEND
   SPEED compute their phase as `seconds / period` from absolute time, so
   each step of the slider moves the effect to an unrelated position
   before it carries on. Sound Factory behaves the same. Fixing it means
   accumulating each phase, which changes the per-frame API, so it is
   cheaper to decide before the renderer is built.
