# Tube CRT Engine — Core (Plan 1 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Sound Factory tube's settings model on the box — rows, looks, the resolver, the row-to-shader-input arithmetic, persistence and the Pi 5 gate — as pure, unit-tested C++ with no change to what any box draws.

**Architecture:** A new header-only directory `magic_dingus_box_cpp/src/tube/` (namespace `tube`), Renderer-free and I/O-free, ported from Sound Factory's `data/DisplayCrt.h`, `data/CrtLooks.h` and `panel/TubeInputs.h`. `AppState::DisplaySettings` gains an engine enum and a `tube::TubeSetting`; `settings_persistence` reads and writes them; `PlatformProfile` gains the gate. Until Plan 3 lands, an engine saved as Tube draws as Enhanced.

**Tech Stack:** C++17, Catch2 (existing `test_app_unit` and `test_platform_unit` targets), jsoncpp, Python 3 stdlib (one dev script).

**Spec:** `docs/superpowers/specs/2026-11-15-tube-crt-engine-design.md` (§1 data model, §2 board gate, §5 sizes, Testing). Read it before starting.

**The three plans:** this one (core). Plan 2: controls (TV menu, Content Manager tab, live tuning) — needs only this plan. Plan 3: the renderer — written after the Phase 0 measurements on the owner's Pi 5, which run as a separate throwaway spike and are not part of this plan.

**Source to port from** (read-only, sibling checkout at commit `4ef74a6`): `../mdb-sound-factory/engine/src/data/DisplayCrt.h`, `../mdb-sound-factory/engine/src/data/CrtLooks.h`, `../mdb-sound-factory/engine/src/panel/TubeInputs.h`, `../mdb-sound-factory/engine/src/panel/CrtGeometry.h`, `../mdb-sound-factory/ui/src/screens/settings/crt-settings.ts` (labels, hints, groups), and its tests `../mdb-sound-factory/engine/tests/test_crt_looks.cpp`, `test_crt_geometry.cpp`, `test_crt_wire.cpp`.

## Global Constraints

- The repository folder's name ends in a space: `.../📺 MDB/magic_dingus_box `. Use paths relative to the repo root; never retype the absolute path.
- Work on a new branch `tube-crt-core` cut from `main`, not on the current branch.
- Dual-board contract (CLAUDE.md): no `#ifdef` on a board; board differences only through `platform::PlatformProfile`.
- No new system dependency. Everything in `src/tube/` is header-only, includes only the C++ standard library, and does no I/O.
- No visible change on any box: Classic and Enhanced draw exactly as before; nothing reads the new fields at draw time in this plan.
- Row keys, their order, the 0–100 range, the six look names and the five mask names are Sound Factory's, character for character. The order is append-only.
- Every curve and constant in the row-to-input arithmetic is Sound Factory's, except where the spec changes it: sizes (§5), time (§3 "Precision and time"), the opening's geometry (§3 "The picture rectangle"), CRT Native neutral rows (§5).
- `config/settings.json` is written only by the kiosk; a bad tube value is dropped with a WARN and is never fatal.
- Tests are Catch2, tagged `[tube]`, in `magic_dingus_box_cpp/tests/app/test_tube_*.cpp` (the existing `tests/app/*.cpp` glob builds them into `test_app_unit`; no CMake change).
- Build and run (the Mac's disk is nearly full: use this one build directory and delete it when the plan is finished):
  ```bash
  cmake -S magic_dingus_box_cpp -B build-tube -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF
  cmake --build build-tube --target test_app_unit test_platform_unit -j8
  ./build-tube/test_app_unit "[tube]"
  ```
- Commit messages end with the attribution line the session gives; one commit per task.

## Review Focus

1. **A `settings.json` with a wrong-typed tube value** (`"hum": "20"`, `"tube": 5`, `"rows": []`): the box must boot with every other setting intact and that value dropped. Today a wrong-typed field anywhere quarantines the whole file. (Task 7)
2. **A Pi 5 backup restored on a Pi 4B** (`crt_engine: "tube"`): draws as Enhanced, and saving on the Pi 4B must not erase the tube settings or the saved engine. (Task 7)
3. **A rollback to an older kiosk** after Tube was selected: the old kiosk reads only `enhanced_crt_enabled` and must land on Enhanced. (Task 7)
4. **A kiosk up for 30 days:** every moving phase must still advance smoothly; none may freeze or jump. (Task 5)
5. **A row at exactly its neutral value or out of range** (0, 50, 100, -1, 101, mask word misspelled): neutral means the shader input is exactly neutral (so the renderer can skip the effect); out of range is refused at the door and clamped in the size arithmetic. (Tasks 1, 3, 4)

---

### Task 1: Rows, masks and looks — the words

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_rows.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_rows.cpp`

**Interfaces:**
- Produces (namespace `tube`):
  - `enum class Effect` — 44 enumerators in Sound Factory's order: `brightness, contrast, saturation, white, sharpness, scanlines, lineSize, beam, raster, maskStrength, maskSize, glow, glowSize, afterglow, curvature, vignette, rim, glassBlack, reflection, convergence, chromaBleed, noise, jitter, flicker, hum, abl, breathing, rollBand, rollSpeed, rollWidth, rollTint, tracking, trackingRate, colorDrift, driftSpeed, trackingWidth, trackingRecovery, flagging, flagHeight, flagSpeed, purity, puritySize, purityHue, purityPosition`
  - `inline constexpr std::size_t kEffectCount = 44;`
  - `inline constexpr std::array<Effect, kEffectCount> kEffects;`
  - `constexpr std::size_t index_of(Effect) noexcept;`
  - `constexpr std::string_view effect_key(Effect) noexcept;` / `constexpr std::optional<Effect> parse_effect(std::string_view) noexcept;`
  - `enum class Mask { grille, magentaGreen, dots, slot, none };` `kMasks`, `mask_name`, `parse_mask`
  - `enum class Look { pvm, monitor, trinitron, tv, arcade, everything };` `kLooks`, `look_name`, `parse_look`, `inline constexpr Look kLookDefault = Look::pvm;`
  - `inline constexpr int kRowMin = 0, kRowMax = 100;` `constexpr bool is_row_value(long long) noexcept;`

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "tube/tube_rows.h"
using namespace tube;

TEST_CASE("tube rows: 44 effects in Sound Factory's order", "[tube]") {
    REQUIRE(kEffects.size() == 44);
    REQUIRE(effect_key(kEffects.front()) == "brightness");
    REQUIRE(index_of(Effect::breathing) == 26);
    REQUIRE(index_of(Effect::rollBand) == 27);
    REQUIRE(effect_key(kEffects.back()) == "purityPosition");
    for (std::size_t i = 0; i < kEffects.size(); ++i) {
        REQUIRE(index_of(kEffects[i]) == i);
        REQUIRE(parse_effect(effect_key(kEffects[i])) == kEffects[i]);
    }
}
TEST_CASE("tube rows: words are exact", "[tube]") {
    REQUIRE(parse_effect("Brightness") == std::nullopt);
    REQUIRE(parse_effect("") == std::nullopt);
    REQUIRE(mask_name(Mask::magentaGreen) == "magentaGreen");
    REQUIRE(parse_mask("slot") == Mask::slot);
    REQUIRE(parse_mask("Slot") == std::nullopt);
    REQUIRE(kLooks.size() == 6);
    REQUIRE(look_name(Look::everything) == "everything");
    REQUIRE(parse_look("pvm") == Look::pvm);
    REQUIRE(kLookDefault == Look::pvm);
}
TEST_CASE("tube rows: the range", "[tube]") {
    REQUIRE(is_row_value(0));
    REQUIRE(is_row_value(100));
    REQUIRE_FALSE(is_row_value(-1));
    REQUIRE_FALSE(is_row_value(101));
}
```

- [ ] **Step 2: Run it and see it fail** — `cmake --build build-tube --target test_app_unit -j8` fails: `tube/tube_rows.h` not found.
- [ ] **Step 3: Implement `tube_rows.h`** as a port of `DisplayCrt.h`'s enums, tables and parse/name functions (tables and loops, no `switch`). Leave out `CrtFlicker` and the JUCE-era migration words.
- [ ] **Step 4: Run** `./build-tube/test_app_unit "[tube]"` — all pass.
- [ ] **Step 5: Commit** — `tube: the rows, masks and looks (Sound Factory's words)`

---

### Task 2: The look table and the two whole-pixel sizes

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_looks.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_looks.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces:
  - `struct LookValues { std::array<int, kEffectCount> effects; Mask mask; };`
  - `constexpr const LookValues& look_values(Look) noexcept;` (an out-of-enum value reads as `kLookDefault`)
  - `inline constexpr int kSizeRowStep = 10;`
  - `constexpr int mask_pitch_row_px(int row) noexcept;` — `2 + row / 10` (2..12), pixels at the 1080-line reference
  - `constexpr int line_pitch_row_px(int row) noexcept;` — `3 + row / 10` (3..13), same reference
  - `int whole_px(int reference_px, double picture_h) noexcept;` — `max(2, lround(reference_px * picture_h / 1080.0))`

The table is Sound Factory's `kCrtLookTable` copied number for number, keeping its one-row-per-look shape (Task 8's script reads it with a regex), with exactly two columns changed for a TV (spec §5):

| Look | lineSize (SF → box) | maskSize (SF → box) |
|---|---|---|
| pvm | 30 → 10 | 40 → 10 |
| monitor | 10 → 10 | 10 → 10 |
| trinitron | 30 → 10 | 40 → 10 |
| tv | 30 → 10 | 40 → 10 |
| arcade | 30 → 10 | 70 → 40 |
| everything | 30 → 10 | 40 → 10 |

(lineSize 10 is 4 px a line at 1080p = 270 lines, 3 px at 720p = 240 lines; maskSize 10 is a 3 px triad, 40 is 6 px.)

- [ ] **Step 1: Write the failing test.** Pin all six rows in full (a `kPinned` array beside the test, as Sound Factory's `test_crt_looks.cpp` does) and assert `look_values(look).effects == kPinned[i].effects` and the mask for each. Plus:

```cpp
TEST_CASE("tube looks: pvm as shipped", "[tube]") {
    const auto& v = look_values(Look::pvm);
    REQUIRE(v.mask == Mask::grille);
    REQUIRE(v.effects[index_of(Effect::brightness)] == 55);
    REQUIRE(v.effects[index_of(Effect::lineSize)] == 10);
    REQUIRE(v.effects[index_of(Effect::maskSize)] == 10);
    REQUIRE(v.effects[index_of(Effect::hum)] == 0);
    REQUIRE(v.effects[index_of(Effect::purityPosition)] == 0);
    REQUIRE(look_values(Look::arcade).effects[index_of(Effect::maskSize)] == 40);
    REQUIRE(look_values(Look::tv).mask == Mask::slot);
}
TEST_CASE("tube looks: every value is a legal row", "[tube]") {
    for (auto look : kLooks)
        for (int v : look_values(look).effects) REQUIRE(is_row_value(v));
}
TEST_CASE("tube looks: whole-pixel sizes", "[tube]") {
    REQUIRE(mask_pitch_row_px(0) == 2);
    REQUIRE(mask_pitch_row_px(10) == 3);
    REQUIRE(mask_pitch_row_px(100) == 12);
    REQUIRE(line_pitch_row_px(10) == 4);
    REQUIRE(whole_px(4, 1080.0) == 4);
    REQUIRE(whole_px(4, 720.0) == 3);
    REQUIRE(whole_px(3, 720.0) == 2);
    REQUIRE(whole_px(2, 480.0) == 2);   // never under two
}
```

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run, see it pass.**
- [ ] **Step 5: Commit** — `tube: the look table, with the two whole-pixel sizes chosen for a TV`

---

### Task 3: The setting and its resolver

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_setting.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_setting.cpp`

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces:
  - `struct TubeSetting { Look look = kLookDefault; std::optional<Mask> mask; std::array<std::optional<int>, kEffectCount> rows{}; bool operator==(const TubeSetting&) const; };` — `rows`/`mask` hold only what was moved.
  - `struct Resolved { Look look; Mask mask; std::array<int, kEffectCount> effects; bool custom; };`
  - `Resolved resolve(const TubeSetting&) noexcept;`
  - `std::vector<std::string_view> moved_keys(const TubeSetting&);` — effect keys in canonical order, then `"mask"`; a stated value equal to the look's is not moved.
  - `bool set_row(TubeSetting&, Effect, int value) noexcept;` — false and no change when `!is_row_value(value)`; a value equal to the look's clears the override.
  - `void set_mask(TubeSetting&, Mask) noexcept;` — equal to the look's clears it.
  - `void select_look(TubeSetting&, Look) noexcept;` — sets the look and clears every moved row and the mask (spec §1).
  - `void reset_to_look(TubeSetting&) noexcept;`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("tube setting: a look untouched is not custom", "[tube]") {
    TubeSetting s; select_look(s, Look::tv);
    auto r = resolve(s);
    REQUIRE_FALSE(r.custom);
    REQUIRE(r.mask == Mask::slot);
    REQUIRE(r.effects == look_values(Look::tv).effects);
    REQUIRE(moved_keys(s).empty());
}
TEST_CASE("tube setting: a moved row sits on top of the look", "[tube]") {
    TubeSetting s;                                   // pvm
    REQUIRE(set_row(s, Effect::hum, 20));
    set_mask(s, Mask::dots);
    auto r = resolve(s);
    REQUIRE(r.custom);
    REQUIRE(r.effects[index_of(Effect::hum)] == 20);
    REQUIRE(r.effects[index_of(Effect::brightness)] == 55);
    REQUIRE(r.mask == Mask::dots);
    REQUIRE(moved_keys(s) == std::vector<std::string_view>{"hum", "mask"});
}
TEST_CASE("tube setting: moving a row back to the look's value un-moves it", "[tube]") {
    TubeSetting s; set_row(s, Effect::hum, 20); set_row(s, Effect::hum, 0);
    REQUIRE_FALSE(resolve(s).custom);
    REQUIRE(s == TubeSetting{});
}
TEST_CASE("tube setting: out of range is refused", "[tube]") {
    TubeSetting s;
    REQUIRE_FALSE(set_row(s, Effect::hum, 101));
    REQUIRE_FALSE(set_row(s, Effect::hum, -1));
    REQUIRE(s == TubeSetting{});
}
TEST_CASE("tube setting: picking a look clears what was moved", "[tube]") {
    TubeSetting s; set_row(s, Effect::hum, 20); set_mask(s, Mask::dots);
    select_look(s, Look::arcade);
    REQUIRE(moved_keys(s).empty());
    REQUIRE(resolve(s).mask == Mask::dots);          // arcade's own
    set_row(s, Effect::noise, 40); reset_to_look(s);
    REQUIRE(s.look == Look::arcade);
    REQUIRE_FALSE(resolve(s).custom);
}
```

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run, see it pass.**
- [ ] **Step 5: Commit** — `tube: a look plus the rows moved on top of it`

---

### Task 4: Row values into shader inputs (the still part)

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_inputs.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_inputs.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  - `inline constexpr double kTubeMmAcross = 196.608;` (Sound Factory's panel width; the 4:3 picture is treated as that tube)
  - `struct Picture { double w, h; bool crt_native; };` — the picture rectangle in output pixels (1440×1080 at 1080p Modern TV).
  - `struct TubeUniforms` — plain `float` fields, Sound Factory's names: `outW, outH, pxPerMm, curvature, cornerPx, marginPx, flatFit, scanlines, lines, raster, beamMin, beamMax, maskType, maskStrength, maskGain, maskPitch, glow, sharpness, brightness, contrast, saturation, convergence, persistence, flicker, hum, vignette, rim, noise, jitter, glassBlack, reflection, abl, breathe, chromaBleed, tint[3]`, plus the per-frame block Task 5 fills. No `time`, `frame`, `humPeriod`, `arcX`, `arcY`, `srcSize`, `bypass` (source equals output; the opening has straight edges).
  - `TubeUniforms tube_uniforms_for(const Resolved&, const Picture&) noexcept;`
  - `float mask_gain(Mask, float strength) noexcept;`
  - `float hum_strength(double row) noexcept;`
  - `float glow_sigma_px(const Resolved&, const Picture&) noexcept;` — sigma in quarter-resolution pixels: `(0.5 + 5.5 * row / 100) mm × pxPerMm / 4`
  - `constexpr bool neutral_in_crt_native(Effect) noexcept;` — true for `scanlines, raster, maskStrength, maskSize, curvature, rim, glassBlack, reflection` (and the mask reads as `none`).

Port `tubeUniformsFor`, `tubeMaskGain`, `tubeHumStrength`, `tubeWhite` line for line, with these changes: `pxPerMm = picture.w / kTubeMmAcross`; lengths Sound Factory converts with `uiPx(mm)` use `mm * pxPerMm`; `cornerPx = 5.0 mm`, `marginPx = 1.5 mm` in pixels; `flatFit` is `flatFitFor` evaluated for straight edges (arc ends at 1.0), computed by the same bisection; `maskPitch = whole_px(mask_pitch_row_px(row), picture.h)`; `lines = picture.h / whole_px(line_pitch_row_px(row), picture.h)`; there is no `gated` parameter. When `picture.crt_native`, the rows `neutral_in_crt_native` names are drawn at 0 and `maskType = 0`, `maskGain = 1`.

- [ ] **Step 1: Write the failing test** (values from Sound Factory's curves; use `Catch::Approx` with `.margin(1e-4)`):

```cpp
const Picture k1080{1440.0, 1080.0, false};
TEST_CASE("tube inputs: pvm at 1080p", "[tube]") {
    TubeSetting s; auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.pxPerMm     == Approx(1440.0 / 196.608));
    REQUIRE(u.brightness  == Approx(1.05));
    REQUIRE(u.contrast    == Approx(1.08));
    REQUIRE(u.saturation  == Approx(1.16));
    REQUIRE(u.sharpness   == Approx(1.26));
    REQUIRE(u.scanlines   == Approx(0.35));
    REQUIRE(u.beamMin     == Approx(0.16));
    REQUIRE(u.beamMax     == Approx(0.40));
    REQUIRE(u.raster      == 0.0f);
    REQUIRE(u.maskType    == 1.0f);
    REQUIRE(u.maskStrength== Approx(0.48));
    REQUIRE(u.maskGain    == Approx(3.0 / 2.04));
    REQUIRE(u.maskPitch   == 3.0f);
    REQUIRE(u.lines       == Approx(270.0));
    REQUIRE(u.glow        == Approx(0.08));
    REQUIRE(u.persistence == Approx(0.10));
    REQUIRE(u.curvature   == Approx(0.70));
    REQUIRE(u.abl         == Approx(0.20));
    REQUIRE(u.breathe     == Approx(0.15));
    REQUIRE(u.hum == 0.0f); REQUIRE(u.noise == 0.0f); REQUIRE(u.jitter == 0.0f); REQUIRE(u.flicker == 0.0f);
    REQUIRE(u.tint[0] == 1.0f); REQUIRE(u.tint[1] == 1.0f); REQUIRE(u.tint[2] == 1.0f);
}
TEST_CASE("tube inputs: 720p keeps whole pixels", "[tube]") {
    TubeSetting s; auto u = tube_uniforms_for(resolve(s), Picture{960.0, 720.0, false});
    REQUIRE(u.lines == Approx(240.0));
    REQUIRE(u.maskPitch == 2.0f);
}
TEST_CASE("tube inputs: neutral rows give exactly neutral inputs", "[tube]") {
    TubeSetting s;
    for (auto e : {Effect::brightness, Effect::contrast, Effect::saturation, Effect::white, Effect::sharpness}) set_row(s, e, 50);
    auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.brightness == 1.0f); REQUIRE(u.contrast == 1.0f); REQUIRE(u.saturation == 1.0f); REQUIRE(u.sharpness == 1.0f);
    REQUIRE(u.tint[0] == 1.0f); REQUIRE(u.tint[2] == 1.0f);
}
TEST_CASE("tube inputs: a moving row's 100 is what the shader's cap expects", "[tube]") {
    TubeSetting s;
    for (auto e : {Effect::noise, Effect::jitter, Effect::flicker, Effect::hum}) set_row(s, e, 100);
    auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.noise == Approx(0.2)); REQUIRE(u.jitter == Approx(1.0));
    REQUIRE(u.flicker == Approx(1.0)); REQUIRE(u.hum == Approx(1.0));
}
TEST_CASE("tube inputs: hum's curve", "[tube]") {
    REQUIRE(hum_strength(0) == 0.0f);
    REQUIRE(hum_strength(30) == Approx(0.06));
    REQUIRE(hum_strength(65) == Approx(0.53));
    REQUIRE(hum_strength(100) == Approx(1.0));
}
TEST_CASE("tube inputs: the mask gives back what it takes", "[tube]") {
    REQUIRE(mask_gain(Mask::none, 0.8f) == 1.0f);
    REQUIRE(mask_gain(Mask::grille, 0.48f) == Approx(1.4705882));
    REQUIRE(mask_gain(Mask::magentaGreen, 0.304f) == Approx(2.0 / 1.696));
    REQUIRE(mask_gain(Mask::dots, 0.0f) == 1.0f);
    REQUIRE(mask_gain(Mask::slot, 0.0f) == 1.0f);
}
TEST_CASE("tube inputs: colour temp and millimetre sizes", "[tube]") {
    TubeSetting s; set_row(s, Effect::white, 0); set_row(s, Effect::chromaBleed, 50); set_row(s, Effect::convergence, 17);
    auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.tint[0] == Approx(1.0)); REQUIRE(u.tint[1] == Approx(0.95)); REQUIRE(u.tint[2] == Approx(0.86));
    REQUIRE(u.chromaBleed == Approx(1.5 * 1440.0 / 196.608));
    REQUIRE(u.convergence == Approx(0.102 * 1440.0 / 196.608));
}
TEST_CASE("tube inputs: a real CRT keeps its own scanlines, mask and glass", "[tube]") {
    TubeSetting s; select_look(s, Look::tv);
    auto u = tube_uniforms_for(resolve(s), Picture{960.0, 720.0, true});
    REQUIRE(u.scanlines == 0.0f); REQUIRE(u.raster == 0.0f);
    REQUIRE(u.maskType == 0.0f);  REQUIRE(u.maskGain == 1.0f);
    REQUIRE(u.curvature == 0.0f); REQUIRE(u.rim == 0.0f);
    REQUIRE(u.glassBlack == 0.0f); REQUIRE(u.reflection == 0.0f);
    REQUIRE(u.chromaBleed > 0.0f);                    // signal rows still work
}
TEST_CASE("tube inputs: the flat picture fits inside its margin", "[tube]") {
    TubeSetting s; auto u = tube_uniforms_for(resolve(s), k1080);
    REQUIRE(u.flatFit > 0.9f); REQUIRE(u.flatFit < 1.0f);
}
```

Also copy into this file every case of Sound Factory's `test_crt_looks.cpp` / `test_crt_wire.cpp` that exercises `tubeUniformsFor`, `tubeMaskGain` or `tubeHumStrength` and still applies after the changes above; record `4ef74a6` in a comment at the top of the file.

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run, see it pass.**
- [ ] **Step 5: Commit** — `tube: a setting turned into what the shader reads`

---

### Task 5: What one frame owns — phases, character, warm-up, settling

**Files:**
- Modify: `magic_dingus_box_cpp/src/tube/tube_inputs.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_frame.cpp`

**Interfaces:**
- Consumes: Task 4.
- Produces (per-frame fields added to `TubeUniforms`): `humPhase` (0..1), `flickerWave` (the whole `0.6 sin(37.7 t) + 0.4 sin(61.3 t + 1.7)` term), `noiseSeed` (`fract(frame × 0.618) × 913`), `jitterSeed` (`frame mod 1024`), `jitterBias`, `degaussPhaseA`, `degaussPhaseB` (radians, wrapped to 2π: `31 t` and `27 t`), `ablLevel`, `power`, `degauss`, `spot`, `warmupGain`, and Sound Factory's character block `rollAmount, rollPosition, rollHalfWidth, rollTint, trackingOffset, trackingPosition, trackingHalfWidth, driftOffset, flagOffset, flagExtent, purityAmount, purityX, purityY, purityRadius, purityR, purityG, purityB`.
  - `inline constexpr double kHumPeriodSeconds = 16.7;`
  - `void tube_frame_phases(TubeUniforms&, double seconds, std::uint64_t frame) noexcept;` — `seconds` is a `double` since kiosk start; all wrapping is done here in double precision.
  - `void tube_character_frame(TubeUniforms&, const Resolved&, double seconds, double tracking_elapsed, bool enabled) noexcept;` — port of `tubeCharacterFrame`.
  - `void tube_signal_motion(TubeUniforms&, double seconds, double amount, double rising_light) noexcept;` — port of `tubeSignalMotion`.
  - `double tracking_period(const Resolved&) noexcept;` `double tracking_duration(const Resolved&) noexcept;` `bool tracking_moving(const Resolved&, double elapsed) noexcept;`
  - `inline constexpr double kWarmupSeconds = 1.5;` `double warmup_gain(double seconds) noexcept;`
  - `int settle_frames(const TubeUniforms&, double abl_distance) noexcept;` — port of `tubeSettleFrames`.
  - `bool moves_on_its_own(const Resolved&) noexcept;` — true when any of `noise, jitter, flicker, hum, rollBand, colorDrift, flagging` is above 0 (tracking is asked separately through `tracking_moving`). This is what Plan 3's redraw gate asks.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("tube frame: warm-up", "[tube]") {
    REQUIRE(warmup_gain(0.0) == 0.0);  REQUIRE(warmup_gain(0.2) == 0.0);
    REQUIRE(warmup_gain(0.5) == Approx(0.4));
    REQUIRE(warmup_gain(0.8) == Approx(0.8));
    REQUIRE(warmup_gain(1.15) == Approx(0.9));
    REQUIRE(warmup_gain(1.5) == Approx(1.0));  REQUIRE(warmup_gain(99.0) == 1.0);
}
TEST_CASE("tube frame: settling debt", "[tube]") {
    TubeUniforms u{};
    REQUIRE(settle_frames(u, 0.0) == 0);
    u.persistence = 0.10f; REQUIRE(settle_frames(u, 0.0) == 4);
    u.persistence = 0.30f; REQUIRE(settle_frames(u, 0.0) == 6);
    u.persistence = 1.00f; REQUIRE(settle_frames(u, 0.0) == 30);
    u.persistence = 0.0f; u.abl = 0.2f; REQUIRE(settle_frames(u, 1.0) == 26);
    u.abl = 0.0f; REQUIRE(settle_frames(u, 1.0) == 0);
}
TEST_CASE("tube frame: phases stay smooth after 30 days", "[tube]") {
    const double t0 = 30.0 * 86400.0, dt = 1.0 / 60.0;
    TubeUniforms a{}, b{};
    tube_frame_phases(a, t0, 155'520'000ull);
    tube_frame_phases(b, t0 + dt, 155'520'001ull);
    double d = b.humPhase - a.humPhase; if (d < 0) d += 1.0;
    REQUIRE(d == Approx(dt / 16.7).margin(1e-6));           // advanced by exactly one frame's worth
    REQUIRE(a.humPhase >= 0.0f); REQUIRE(a.humPhase < 1.0f);
    REQUIRE(std::abs(a.flickerWave) <= 1.0f);
    REQUIRE(b.flickerWave != a.flickerWave);                // not frozen
    REQUIRE(a.degaussPhaseA >= 0.0f); REQUIRE(a.degaussPhaseA < 6.2832f);
    REQUIRE(a.jitterSeed < 1024.0f);
    REQUIRE(b.noiseSeed != a.noiseSeed);
}
TEST_CASE("tube frame: the rolling band", "[tube]") {
    TubeSetting s; set_row(s, Effect::rollBand, 100);        // pvm: rollSpeed 35, rollWidth 35, rollTint 50
    auto r = resolve(s); auto u = tube_uniforms_for(r, Picture{1440.0, 1080.0, false});
    tube_character_frame(u, r, 10.8, 0.0, true);
    REQUIRE(u.rollAmount == Approx(2.0));
    REQUIRE(u.rollPosition == Approx(0.5));                  // 10.8 s of a 21.6 s pass
    REQUIRE(u.rollHalfWidth == Approx(13.1 * (1440.0 / 196.608) / 1080.0));
    REQUIRE(u.rollTint == Approx(0.0));
    tube_character_frame(u, r, 10.8, 0.0, false);
    REQUIRE(u.rollAmount == 0.0f);                           // disabled zeroes everything
}
TEST_CASE("tube frame: tracking sleeps between slips", "[tube]") {
    TubeSetting s; auto off = resolve(s);                    // pvm: tracking 0
    REQUIRE_FALSE(tracking_moving(off, 0.1));
    set_row(s, Effect::tracking, 50); auto r = resolve(s);   // rate 15, recovery 43
    REQUIRE(tracking_period(r) == Approx(51.6));
    REQUIRE(tracking_duration(r) == Approx(0.55));
    REQUIRE(tracking_moving(r, 0.1));
    REQUIRE_FALSE(tracking_moving(r, 5.0));
}
TEST_CASE("tube frame: the colour patch starts top left", "[tube]") {
    TubeSetting s; set_row(s, Effect::purity, 100); set_row(s, Effect::purityPosition, 0);
    auto r = resolve(s); auto u = tube_uniforms_for(r, Picture{1440.0, 1080.0, false});
    tube_character_frame(u, r, 0.0, 0.0, true);
    REQUIRE(u.purityX == Approx(0.14645)); REQUIRE(u.purityY == Approx(0.14645));
}
TEST_CASE("tube frame: what moves on its own", "[tube]") {
    TubeSetting s; REQUIRE_FALSE(moves_on_its_own(resolve(s)));      // pvm is still
    set_row(s, Effect::hum, 1); REQUIRE(moves_on_its_own(resolve(s)));
    TubeSetting e; select_look(e, Look::everything); REQUIRE(moves_on_its_own(resolve(e)));
}
```

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement.** In the ports, replace Sound Factory's `u.pxPerMm * u.srcSize.x / u.outSize.x` with `u.pxPerMm` (source equals output).
- [ ] **Step 4: Run, see it pass.**
- [ ] **Step 5: Commit** — `tube: what one frame owns, with every phase wrapped on the CPU`

---

### Task 6: Groups, labels, hints and steps

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_groups.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_groups.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces:
  - `struct RowInfo { std::string_view label, hint1, hint2; };` `constexpr const RowInfo& row_info(Effect) noexcept;`
  - `inline constexpr RowInfo kMaskRowInfo;` — label `MASK`.
  - `struct Group { std::string_view key, label; bool has_mask_row; std::span<const Effect> rows; };` (or an equivalent fixed-capacity type; the builder's choice)
  - `inline constexpr std::array<Group, 10> kGroups;` in this order, with these rows:
    `picture`/PICTURE: brightness contrast saturation white sharpness · `scanlines`/SCANLINES: scanlines lineSize beam raster · `mask`/MASK: (mask) maskStrength maskSize · `light`/LIGHT: glow glowSize afterglow · `glass`/GLASS: curvature vignette rim glassBlack reflection · `signal`/SIGNAL: convergence chromaBleed noise jitter · `supply`/SUPPLY: flicker hum abl breathing · `character`/CHARACTER: rollBand rollSpeed rollWidth rollTint colorDrift driftSpeed · `sync`/SYNC: tracking trackingRate trackingWidth trackingRecovery flagging flagHeight flagSpeed · `purity`/COLOUR PURITY: purity puritySize purityHue purityPosition
  - `constexpr int row_step(Effect) noexcept;` — 10 for `lineSize` and `maskSize`, 5 for every other row.
  - `constexpr int step_row(Effect, int value, int direction) noexcept;` — one step up or down, clamped to 0..100, landing on a multiple of the step.

Labels and the two hint lines are `TUBE_EFFECT` from Sound Factory's `crt-settings.ts`, copied exactly, with three edits for a TV: `maskSize` hints become `"how wide one colour triad is"`, `"in the TV's own pixels"`; `lineSize` hints become `"how tall one beam line is"`, `"in the TV's own pixels"`; `rim` second hint becomes `"where the tube meets its surround"`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("tube groups: every row is in exactly one group", "[tube]") {
    std::array<int, kEffectCount> seen{};
    int masks = 0;
    for (const auto& g : kGroups) { masks += g.has_mask_row; for (auto e : g.rows) ++seen[index_of(e)]; }
    for (int n : seen) REQUIRE(n == 1);
    REQUIRE(masks == 1);
}
TEST_CASE("tube groups: Sound Factory's ten, in its order", "[tube]") {
    const std::array<std::pair<std::string_view, std::size_t>, 10> want{{
        {"PICTURE", 5}, {"SCANLINES", 4}, {"MASK", 2}, {"LIGHT", 3}, {"GLASS", 5},
        {"SIGNAL", 4}, {"SUPPLY", 4}, {"CHARACTER", 6}, {"SYNC", 7}, {"COLOUR PURITY", 4}}};
    for (std::size_t i = 0; i < 10; ++i) {
        REQUIRE(kGroups[i].label == want[i].first);
        REQUIRE(kGroups[i].rows.size() == want[i].second);
    }
    REQUIRE(kGroups[2].has_mask_row);
}
TEST_CASE("tube groups: labels and steps", "[tube]") {
    REQUIRE(row_info(Effect::hum).label == "HUM BAR");
    REQUIRE(row_info(Effect::white).label == "COLOUR TEMP");
    REQUIRE(row_info(Effect::flagging).label == "TOP EDGE BEND");
    for (auto e : kEffects) REQUIRE_FALSE(row_info(e).label.empty());
    REQUIRE(row_step(Effect::maskSize) == 10);
    REQUIRE(row_step(Effect::hum) == 5);
    REQUIRE(step_row(Effect::hum, 20, +1) == 25);
    REQUIRE(step_row(Effect::hum, 22, +1) == 25);   // off-grid value (set from the web) snaps
    REQUIRE(step_row(Effect::hum, 22, -1) == 20);
    REQUIRE(step_row(Effect::hum, 100, +1) == 100);
    REQUIRE(step_row(Effect::hum, 0, -1) == 0);
    REQUIRE(step_row(Effect::lineSize, 10, +1) == 20);
}
```

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run, see it pass.**
- [ ] **Step 5: Commit** — `tube: the ten groups, their labels, and a row's step`

---

### Task 7: The board gate, the engine choice and persistence

**Files:**
- Modify: `magic_dingus_box_cpp/src/platform/platform_profile.h` (classification table comment near line 42, and the struct), `magic_dingus_box_cpp/src/platform/platform_profile.cpp` (`profile_for`, `apply_policy_override`)
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (`DisplaySettings`, near line 419)
- Modify: `magic_dingus_box_cpp/src/app/settings_persistence.cpp` (the `display` block of save near line 88 and of load near line 283)
- Test: `magic_dingus_box_cpp/tests/platform/test_platform_profile.cpp`, `magic_dingus_box_cpp/tests/app/test_settings_persistence.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_engine.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  - `PlatformProfile::tube_crt_engine` (`bool`, default `false`): Pi 5 `true`, Pi 4B `false`, Unknown `true`. A POLICY row: listed in the classification table and copied in `apply_policy_override()`.
  - In `app_state.h`: `enum class CrtEngine { Classic, Enhanced, Tube };`
    `DisplaySettings::crt_engine` (`CrtEngine`, default `Classic`), `DisplaySettings::tube` (`tube::TubeSetting`),
    `void DisplaySettings::set_crt_engine(CrtEngine)` — sets `crt_engine` and keeps `enhanced_crt_enabled == (engine != Classic)`.
  - `constexpr CrtEngine app::effective_crt_engine(CrtEngine saved, bool tube_gate) noexcept;` — `Tube` with a closed gate is `Enhanced`; everything else is itself. The saved value is never rewritten because of the gate.
  - `settings.json` `display` keys: `"crt_engine"` (`"classic"|"enhanced"|"tube"`), `"tube": {"look": "<name>", "mask": "<name>", "rows": {"<key>": <int>}}` — `mask` and each row present only when moved; `"enhanced_crt_enabled"` keeps being written, `true` for Enhanced and Tube.

The existing "CRT Engine" menu toggle (`settings_menu.cpp` near line 222) flips `enhanced_crt_enabled` directly; change it to call `set_crt_engine(Classic or Enhanced)` so the two stay in step. It still offers only Classic/Enhanced (Tube joins it in Plan 2).

Loading rules: `crt_engine` absent or not one of the three words → derived from `enhanced_crt_enabled`. Every value under `tube` is type-checked before it is converted: a `tube` that is not an object, a `rows` that is not an object, an unknown key, a non-integer, an out-of-range integer or an unknown word is dropped with one `LOG_WARN` naming it, and loading continues. None of these may reach the wrong-typed-file quarantine.

- [ ] **Step 1: Write the failing tests**

`test_platform_profile.cpp`:
```cpp
TEST_CASE("tube engine gate per board", "[tube]") {
    REQUIRE(profile_for(PiModel::Pi5).tube_crt_engine);
    REQUIRE_FALSE(profile_for(PiModel::Pi4).tube_crt_engine);
    REQUIRE(profile_for(PiModel::Unknown).tube_crt_engine);
}
TEST_CASE("policy override pi4 closes the tube gate on a Pi 5", "[tube]") {
    auto p = apply_policy_override(profile_for(PiModel::Pi5), "pi4");
    REQUIRE_FALSE(p.tube_crt_engine);
    REQUIRE(p.model == PiModel::Pi5);
}
```
(Match the existing override tests in that file for the exact call shape of `apply_policy_override`.)

`test_tube_engine.cpp`:
```cpp
TEST_CASE("effective engine honours the gate without changing the choice", "[tube]") {
    using app::CrtEngine; using app::effective_crt_engine;
    REQUIRE(effective_crt_engine(CrtEngine::Tube, true) == CrtEngine::Tube);
    REQUIRE(effective_crt_engine(CrtEngine::Tube, false) == CrtEngine::Enhanced);
    REQUIRE(effective_crt_engine(CrtEngine::Enhanced, false) == CrtEngine::Enhanced);
    REQUIRE(effective_crt_engine(CrtEngine::Classic, true) == CrtEngine::Classic);
}
TEST_CASE("set_crt_engine keeps the old flag in step", "[tube]") {
    app::AppState::DisplaySettings d;
    d.set_crt_engine(app::CrtEngine::Tube);     REQUIRE(d.enhanced_crt_enabled);
    d.set_crt_engine(app::CrtEngine::Classic);  REQUIRE_FALSE(d.enhanced_crt_enabled);
}
```
(Use whatever qualified name `DisplaySettings` has in `app_state.h`.)

`test_settings_persistence.cpp`, using the file's existing `SettingsSandbox`:
- **round trip:** engine Tube, look `tv`, `hum` 20, `afterglow` 35, mask `dots` → save → load into a fresh state → equal `crt_engine` and `tube`; the saved JSON has `"enhanced_crt_enabled": true`, `"crt_engine": "tube"`, `rows` with exactly `hum` and `afterglow`.
- **untouched look writes no rows:** look `arcade`, nothing moved → saved `tube` has `look` and neither `mask` nor a non-empty `rows`.
- **old file:** a file with only `"enhanced_crt_enabled": true` loads as `CrtEngine::Enhanced` with a default `TubeSetting`; with `false`, `Classic`.
- **older kiosk after a rollback:** after saving engine Tube, the JSON's `enhanced_crt_enabled` is `true`.
- **bad values are dropped, nothing else is lost:** a file with `"bezel_index": 2`, `"crt_engine": "plasma"`, `"enhanced_crt_enabled": true`, `"tube": {"look": "vhs", "mask": "Slot", "rows": {"hum": "20", "noise": 101, "wobble": 5, "afterglow": 35}}` loads with `bezel_index == 2`, engine `Enhanced`, look `pvm`, no mask override, exactly one moved row (`afterglow` 35), and `settings.json.corrupt` does not exist.
- **wrong shapes:** `"tube": 5` and `"tube": {"rows": []}` each load with a default `TubeSetting`, `bezel_index` kept, no quarantine file.
- **a Pi 4B save keeps a Pi 5's choice:** load a file with `crt_engine: "tube"` and a moved row, save without changing anything, reload → still Tube with that row (persistence never consults the gate).

- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement.** Update the classification table comment and the `MDB_PLATFORM_POLICY_OVERRIDE` paragraph's field list in `platform_profile.h`.
- [ ] **Step 4: Run** `./build-tube/test_app_unit` and `./build-tube/test_platform_unit` in full (not only `[tube]`) — all pass.
- [ ] **Step 5: Commit** — `tube: the Pi 5 gate, the engine choice and its place in settings.json`

---

### Task 8: Parity with Sound Factory, and the notes

**Files:**
- Create: `magic_dingus_box_cpp/scripts/dev/check_tube_parity.py`
- Create: `magic_dingus_box_cpp/scripts/tests/test_check_tube_parity.py`
- Modify: `CLAUDE.md` (Architecture → C++ Source Structure: a `tube/` entry; "Platform policy override" → "What switches": add the tube gate)
- Modify: `CHANGELOG.md` (`## [Unreleased]`)

**Interfaces:**
- Consumes: Tasks 1–2 (the two headers' text).
- Produces: `check_tube_parity.py [--sf PATH]` — default `../mdb-sound-factory` relative to the repo root. Exit 0 with `parity ok (Sound Factory <short commit>)` when the two agree; exit 1 listing each difference; exit 2 with `no Sound Factory checkout at <path>` when the sibling is absent. Python 3 stdlib only. Functions: `parse_keys(text) -> list[str]`, `parse_looks(text) -> dict[str, tuple[list[int], str]]`, `compare(box, sf) -> list[str]`.

It compares, by reading the headers' text: the effect keys and their order; the mask and look names; and every look's values and mask, **except** the `lineSize` and `maskSize` columns, which differ by design.

- [ ] **Step 1: Write the failing test** (`pytest magic_dingus_box_cpp/scripts/tests/test_check_tube_parity.py`): `parse_keys` and `parse_looks` on small inline header snippets in both repos' shapes; `compare` returns `[]` for equal tables, names the look and key for a changed value (`"tv.hum: box 30, Sound Factory 40"`), ignores a `lineSize`/`maskSize` difference, and reports a missing or reordered key; `main` returns 2 for a path that does not exist.
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the test, then the script for real:** `python3 magic_dingus_box_cpp/scripts/dev/check_tube_parity.py` → `parity ok (Sound Factory 4ef74a6)` (or whatever the sibling's HEAD is; a real difference here means a port error in Task 1 or 2 — fix that, not the script).
- [ ] **Step 5: Write the notes.** CLAUDE.md `tube/` entry: what the directory is, that it is header-only and unit-tested in `test_app_unit` (`[tube]`), that row keys/order/looks are Sound Factory's and append-only, the two size rows that differ, that a new Sound Factory row arrives by manual port plus `check_tube_parity.py`, and that until the renderer lands an engine saved as Tube draws as Enhanced. CHANGELOG: one line under Unreleased saying the tube's settings model landed with no visible change.
- [ ] **Step 6: Full verification.** Both Mac configurations, all suites:
  ```bash
  ctest --test-dir build-tube --output-on-failure
  cmake -S magic_dingus_box_cpp -B build-tube -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=ON && cmake --build build-tube -j8 && ctest --test-dir build-tube --output-on-failure
  ```
  Expected: every suite passes in both. Then `rm -rf build-tube`.
- [ ] **Step 7: Commit** — `tube: a parity check against Sound Factory, and the notes`
