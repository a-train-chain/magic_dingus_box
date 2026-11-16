# Tube CRT engine (Sound Factory port) — design

Status: APPROVED by the owner 2026-11-15. Nothing here is
built. Nothing has been measured on a Pi yet; every cost in this document
is an expectation until Phase 0 replaces it with a number.

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
  for every shipped look (acceptance numbers below).
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

Three were considered.

**A. Faithful port as a new engine — chosen.** Translate `crt.metal`
effect for effect into one GLSL ES shader. Copy the pure-C++ row table,
look table and row-to-shader mapping (`data/DisplayCrt.h`,
`data/CrtLooks.h`, `panel/TubeInputs.h` minus `simd` and the pointer
twin). One shader serves every look; each effect is skipped by a uniform
test when its row is at its neutral value, as in Sound Factory. Trade-off:
the Pi's driver may not fully skip a switched-off effect, so a light look
could cost more than it should. Only measurement tells.

**B. Faithful port, compiled per look — held in reserve.** Same as A, but
the shader is assembled from per-effect blocks and a lean variant is
compiled for the set of effects that are on. Off costs exactly nothing.
Trade-off: a hitch when a slider crosses zero during tuning, and more
combinations to test. If Phase 0 shows switched-off effects still costing
time under A, the build moves to B. Nothing the owner sees changes.

**C. Grow the Enhanced shader — rejected.** Add effects one at a time
under the existing seven sliders. Small steps, but the sliders would not
match Sound Factory's (so looks would not transfer), and Enhanced works on
gamma-encoded colour, where giving back the mask's brightness does not
look right.

The owner chose A, with B as the fallback.

## What the owner decided

- Scope: main menu and playlist videos first; games later.
- Controls: preset looks **and all 45 rows on the TV menu**, and the same
  45 rows in the Content Manager with live update.
- Approach A.

Everything below this line that is not one of those three is the
author's call, marked **(call)** where it is a choice the owner may want
to change.

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

The Tube engine reuses Enhanced's substrate: `begin_scene_fbo()` redirects
video + UI into the scene FBO; `end_scene_fbo_and_composite()` draws the
result to the screen; bezel and toast are drawn afterwards, unaffected, as
today. The Media Browser gate stays.

**The Settings menu is drawn through the tube (call).** It is drawn
inside the scene today, so Enhanced already affects it, and Sound
Factory's own settings screen is tuned through its tube the same way: the
menu's text is where a change to the mask, scanlines or sharpness is
easiest to judge. Draw order does not change. The moving and bending rows
are bounded by the ported caps (jitter ±0.25 mm of a 197 mm picture, and
so on), so the menu is expected to stay readable at every row's 100;
Phase 0 checks this on the TV with the Everything look and every wear row
at 100. If it does not hold, the fallback is to draw the Settings menu
after the composite, like the toast.

**The boot intro (call).** The intro video is drawn through the tube, as
it is through Enhanced today. When the saved engine is Tube and the board
gate is open, the Tube shader is compiled at startup with the renderer's
other shaders (before the intro's first frame and before READY=1), and
the warm-up plays on the first Tube frame, which is the start of the
intro. Otherwise the shader is compiled the first time the engine is
switched to Tube in the menu or from the web page; that hitch must stay
far inside the 10 s watchdog (measured in Phase 0).

**The picture rectangle.** The tube is drawn over the 4:3 content area
only (1440×1080 at 1080p, 960×720 at 720p), not the whole framebuffer;
the pillarbox bars are cleared to black. That is 25% fewer pixels than
full-screen. Source and output are 1:1 in that rectangle. The tube's
"opening" is that rectangle with Sound Factory's corner radius and margin
scaled to it and straight edges (`arcX = arcY = 1`); the walnut-specific
arcs do not come across. Bezel art draws on top as today; how each bezel's
opening sits against the curved picture is for the owner's eye and may
need a per-bezel follow-up.

**Passes**, each allocated and run only while a row needs it:

| Pass | Size | When |
|---|---|---|
| Scene FBO (exists) | framebuffer | Tube on |
| Quarter picture | 1/4 × 1/4 | GLOW > 0 or CHROMA BLEED > 0 |
| Glow blur (separable, H then V) | 1/4 × 1/4 | GLOW > 0; sigma from GLOW SIZE |
| Line raster (5-tap tent per line) | width × lines | TRUE RASTER > 0 |
| Tube pass | picture rectangle | always |
| History (afterglow) | picture rectangle | AFTERGLOW > 0 |

The existing bloom FBOs are Enhanced's and are not shared; the Tube engine
owns its textures and frees them when the engine is switched away.

**Afterglow costs more here than on the Mac.** Sound Factory writes the
screen and the history in one pass (two render targets). The Pi's screen
is the default framebuffer, which cannot be paired with a texture target,
so with AFTERGLOW on the tube draws into the history texture (reading the
previous one) and is then copied to the screen: one extra full-rectangle
copy per frame. With AFTERGLOW at 0 none of this exists, as in Sound
Factory (`crtFragmentNoAfterglow`).

**Linear light.** The tube's arithmetic is in linear light, as in Sound
Factory. The scene FBO stays as it is (gamma-encoded RGBA8; UI and video
drawing are untouched), so the shader decodes on sampling and encodes on
output; the intermediate textures (quarter, lines, history) use an sRGB
format so dark tones keep their precision. Exact method is the builder's.

**Precision and time.** `precision highp float`. No raw time uniform: the
CPU computes every moving phase in double precision once per frame (hum
roll, flicker, the rolling band, drift, flagging, the noise seed, the
jitter frame) and hands the shader wrapped phases. No effect may jump when
a clock wraps; a test holds each phase continuous across its wrap.

**Brightness limit and breathing** need the picture's average light. It is
taken from the quarter picture (mip to 1×1) and read back
**asynchronously** (pixel buffer object + fence) about five times a
second, smoothed with Sound Factory's 0.1 s constant. Never a blocking
read. While the read is unavailable the last value holds.

**Shader failure.** If the Tube shader fails to compile or a pass's FBO is
incomplete, the kiosk logs it once and draws the Enhanced path (and
Classic below that), as Enhanced already falls back today.

### 4. Resting, and moving at field rate

The redraw gate (`app/redraw_gate.h`) must keep working:

- A look with no moving row on a static menu draws nothing once it has
  settled. "Settled" is Sound Factory's `tubeSettleFrames`: each drawn
  change owes the frames the afterglow needs to decay to 0.02% and the
  brightness limiter needs to travel, whichever is longer.
- A look with a moving row (NOISE, JITTER, FLICKER, HUM BAR, ROLLING BAND,
  COLOUR DRIFT, TOP EDGE BEND, and TRACKING SLIP while a slip is in
  flight) draws at 30 fps on a static menu, through the existing
  `crt_time_effects` field-rate path. Tracking sleeps between slips, as in
  Sound Factory.
- During video the loop draws every frame regardless.

The gate's decision stays pure and unit-tested: a function of the resolved
setting and the settle debt.

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
REFLECTION — and shown as not applicable. The signal, supply and wear rows
still work.

### 6. Controls

**One row table, two front ends.** `tube_groups.h` is the source. The web
page needs the same keys, labels, hints, groups and look values; it gets
them from a JSON file generated from the table and checked in, with a test
on each side that fails when the two differ. No hand-kept second list.

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

- **Warm-up** (Sound Factory's 1.5 s) when the Tube engine starts drawing:
  at kiosk start (over the start of the intro, §3), and when the engine is
  switched to Tube.
- **Degauss** as a menu action and a button on the web page.

Not wired now: the power-off collapse to a dot.

### 9. When a look is too heavy

Every shipped look is tuned to pass the acceptance numbers. A custom mix
can still exceed the GPU. The kiosk counts frames that miss their vblank
while the Tube engine draws and logs the count per minute in the journal
next to the existing UI draw-call line. If video is playing and misses are
sustained, it shows one toast ("This CRT look is too heavy for smooth
video") and flags it in the status the web page reads. It does not change
the owner's settings **(call)**.

## Acceptance

On a 2 GB Pi 5, Modern TV at 1920×1080@60, a 960×720 H.264 playlist video
playing, for **each of the six looks**:

- At least 99% of frames are presented on their vblank over a 10-minute
  run. A look that fails has its costliest rows lowered in the box's look
  table until it passes.
- 30 minutes in the enclosure with no throttling (`vcgencmd
  get_throttled` stays `0x0`).
- `MemAvailable` falls by less than 32 MB with the engine on.
- On the static main menu: a look with no moving row reaches zero draws
  per second after settling; a look with moving rows draws at no more than
  30 fps.

On a Pi 4B: with the same release, screenshots of Classic and Enhanced
match the previous release, and no Tube row or tab is offered.

Both boards: the Mac suites pass (all 9 with Media Browser ON, 8 OFF),
`verify_box.sh` passes.

## Build order

0. **Measure first (throwaway spike allowed).** The shader ported and
   wired behind an environment variable on the owner's Pi 5 at 1080p with
   video playing; each effect group timed on and off. Its results decide:
   A or B; which rows the shipped looks can afford; shader compile time.
   Results are written down in `docs/engineering/`. Everything after this
   step is built to the normal standard; the spike's code is not kept
   without going back through review.
1. The pure core: rows, looks, resolver, mapping, persistence, the board
   gate — with tests.
2. The renderer: shader, passes, resting and field rate, fallback.
3. The TV menu.
4. The Content Manager tab and live tuning.
5. Hardware validation on both boards; the owner retunes the six looks on
   his TV.

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
  the request parser.
- **Shader:** the GLSL is compiled in CI on every push (test-only tooling,
  builder's choice; a new package must follow CLAUDE.md's dependency
  rule). Rendered-pixel checks like Sound Factory's WASH/TONE/PITCH/FLASH
  legs are wanted where they can run without a Pi; what cannot run off the
  box is checked on hardware with `screenshot_request` and listed as such.
- **Web (pytest):** validation, CSRF, the gate-closed answer, the request
  file's atomicity and sequence, the generated row file matching the C++
  table; `manager_display.js` follows the classic-script load-order rules.
- **Hardware:** the acceptance section, on both boards.

## Left to the builder

- The file protocol's exact shape (§7), within its stated requirements.
- How the shader decodes and encodes sRGB, the blur's tap count, texture
  formats, how the mean is mipped.
- How the shader source is organised so that a move to approach B is a
  change of assembly, not a rewrite.
- CI shader-compile tooling.
- The web tab's layout and styling, within the Content Manager's existing
  look.
- File and symbol names.

## Risks

- **The Pi 5 cannot afford the heavier rows at 1080p.** Mitigations in
  order: approach B; lower those rows in the shipped looks; the too-heavy
  warning for custom mixes. Phase 0 exists to find this out before
  anything else is built.
- **Afterglow's extra copy** and the history's read/write are the most
  memory traffic this adds, during software video decode.
- **Looks tuned for a 7.9-inch panel at arm's length will not look right
  on a living-room TV as copied.** Expected; step 5 is the retune.
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
  `crt_time.h`, `redraw_gate.h`. The rendering analysis in §3 and §4 stands.
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
- Rows are written only through `tube::set_row` / `tube::set_mask` (or
  validated with `tube::is_row_value` first): `tube::resolve()` does not
  range-check. This binds Plan 2's request parser.
- `tube::tube_frame(still, resolved, state)` is the per-frame entry point.
  `tube_uniforms_for` gives the still uniforms and may be cached;
  `tube_signal_motion` must never run twice on the same uniforms. A
  renderer with no quarter-resolution pass zeroes glow, chroma bleed, ABL
  and breathing on the still uniforms, as Sound Factory's does.
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

### Open, for the owner, before the renderer plan

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
