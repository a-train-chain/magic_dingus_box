# Tube CRT Engine — Renderer (Plan 3 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Tasks marked **HARDWARE** are runbooks for the controller (workers cannot reach a Pi); a worker skips them.

**Goal:** Draw the Tube engine on a Pi 5 — a lean look at 60 fps in one pass, a rich look at an even 30 fps through pipeline 2 — from a fixed, pre-tested table of shader programs, with no change to what any box shows until the last task switches it on after hardware acceptance.

**Architecture:** Decisions are pure and Mac-tested: `src/tube/` gains the frame class (`tube_frame_class.h`), the program table and pass plan (`tube_programs.h`), the shader assembly (`tube_shader_assembly.h`, over GLSL files in `src/tube/shaders/` embedded at build time) and the per-frame driver (`tube_driver.h`: still-uniform cache, accumulated phases, limiter, warm-up, degauss, settle debt). `src/app/` gains the cadence decision (`tube_redraw.h`), the pacing monitor and the build guard; `src/video/content_area.h` becomes the one source of the content rectangle; `utils/frame_pacing.h` gains the Rich30 present hold. The GL side is one kiosk-only class, `ui::TubeRenderer` (`tube_renderer.cpp` resources and programs, `tube_renderer_passes.cpp` the passes), owned by `ui::Renderer` and reached through `begin_scene_fbo` / `end_scene_fbo_and_composite`. Everything is decided by `app::effective_crt_engine(crt_engine, app::tube_offered(state))`, which stays Enhanced on every box while `app::kTubeRendererReady` is false; hardware runs use a test-only `MDB_TUBE_PREVIEW` build.

**Tech Stack:** C++17, OpenGL ES 3.0 (+ `GL_EXT_sRGB_write_control`, `GL_EXT_texture_sRGB_decode`, `GL_EXT_color_buffer_half_float`, `GL_EXT_disjoint_timer_query`, all optional), GLSL ES 3.00, Catch2 (`test_app_unit`, `test_utils_unit`, `test_video_unit`, `test_ui_unit`, `test_phone_remote_unit`), CMake, `glslangValidator` (CI only), Python 3 + pytest, node (the web helper test), bash.

**Spec:** `docs/superpowers/specs/2026-11-15-tube-crt-engine-design.md` as amended after Phase 0 — §3 Rendering, §4 Resting and the two frame rates, §8 Transitions, §9, Acceptance, Testing. Measurements: `docs/engineering/measurements/2026-11-15-tube-phase0*-journal.txt`. Plans 1 (`2026-11-15-tube-crt-core.md`, built) and 2 (`2026-11-15-tube-crt-controls.md`) are what this plan consumes.

**Reference, never merged:** the spike worktree `.worktrees/tube-spike` (branch `spike/tube-phase0`): `magic_dingus_box_cpp/src/ui/tube_spike.cpp` (pipeline 2's passes, `begin_scene`/`scene_decode` for the sRGB write-control trick, `ensure_tex`, the timer ring `tbegin`/`tend`/`read_set`, `build`/`first_use`, `mask_at`, `upload_scene`) and `magic_dingus_box_cpp/spike/tube/` (`v2_signal.frag`, `v2_lines.frag`, `v2_output.frag`, `v2_glass.frag`, `common.glsl`, `downsample.frag`, `blur.frag`, `copy.frag`, `SPIKE.md`). Port from them; do not copy bench code, file loading, the control file or per-frame read-back. **Every coefficient and flash-safety cap comes from `../mdb-sound-factory/engine/src/panel/crt.metal` at `4ef74a6`**; the renderer's per-frame bookkeeping from `TubeRenderer.mm` (`stepBrightnessLimitTo`, `readMean`, `kDegaussSeconds`, the block at lines 937–958).

**Depends on Plan 2:** Tasks 1–12 need only Plan 2's Tasks 1–7 (in this tree or landing). Task 13 needs Plan 2 complete (Tasks 3, 5, 6b, 9 are the files it edits). Rebase this branch onto Plan 2's tip before Task 13.

## Global Constraints

- The repository folder's name ends in a space: `.../📺 MDB/magic_dingus_box `. Work in the worktree `.worktrees/tube-crt-renderer` (branch `tube-crt-renderer`); use paths relative to its root. In bash, `cd "$HOME/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tube-crt-renderer"` first.
- **No task before Task 17 changes what any box shows.** Every Tube code path is reached only when `app::effective_crt_engine(display_settings.crt_engine, app::tube_offered(state)) == CrtEngine::Tube`, which is never true while `kTubeRendererReady` is false. Code that runs regardless (the content-rect extraction, the gate's new field, the present hold, the pacing monitor) must be byte-for-byte behaviour-neutral when Tube is not drawing, and each such task names the test that pins it.
- Dual-board contract: board differences only through `app::tube_offered(state)` (which reads `platform_profile.tube_crt_engine`). No `#ifdef` on a board. The one `#ifdef` this plan adds (`MDB_TUBE_PREVIEW`, Task 12) is a test-build switch removed in Task 17.
- No new kiosk dependency. GL extensions are probed at runtime; without them the same programs decode in the shader. `glslang-tools` is CI-only tooling (not a `-dev` package; `tests/local/system_deps_parity.bats` does not see it).
- `src/tube/` stays header-only, standard library only, no I/O; the generated shader header is standard library only.
- The render thread never blocks: no `glFinish`, no synchronous `glReadPixels`, no `glGetQueryObject` without `GL_QUERY_RESULT_AVAILABLE` first. No recurring file write on the main loop; the build guard writes `/dev/shm` only when a program is built.
- The renderer's rules: `flush_ui_batch()` before every non-batched GL call in the `Renderer` files; every GL object the tube owns is released by `Renderer::reset_gl()` and `cleanup()` and rebuilt on its next frame; the Media Browser never draws through the tube; the bezel and toast are drawn after the composite; the scene FBO is sized to the framebuffer.
- Shaders: only programs from `tube::kPrograms` are ever compiled. Programs are built when Tube starts drawing or when the plan needs one not yet built — never per frame.
- Exact values: Lean60 budget 8 ms, Rich30 24 ms (GPU, all tube passes, 1440×1080); reference picture 1440×1080 px; phase step cap 0.1 s; mean probe 64×48 at 5 Hz; degauss 1.4 s, moving 1.5 s; warm-up 1.5 s, moving 1.6 s; glass light scale ×32; build guard `/dev/shm/mdb-tube-build`; too-heavy: > 5% of presents late over 30 s with a video playing; GPU timing log every 10 s; pacing report every 60 s.
- Mac build (one directory; the Mac's disk is nearly full — delete it when the plan is finished). `test_app_unit` is skipped without yaml-cpp (`brew install yaml-cpp`):
  ```bash
  cmake -S magic_dingus_box_cpp -B build-tube3 -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF
  cmake --build build-tube3 --target test_app_unit test_utils_unit test_video_unit test_ui_unit test_phone_remote_unit -j8
  ```
  Kiosk-only files (`main.cpp`, `renderer*.cpp`, `tube_renderer*.cpp`) are verified by `magic_dingus_box_cpp/dev/pisim/pisim.sh check` (the real kiosk binary in the Pi-userspace container, plus every suite on aarch64) and by CI's `kiosk-build` job. Shaders by `scripts/dev/check_tube_shaders.sh` (Task 6).
- One commit per task; messages end with the session's attribution line.

## Review Focus

1. **A shader compile that hangs.** One Mesa variant hung the compiler for nine minutes. The table must not contain it, every program must be compiled and drawn on a Pi 5 before switch-on, and a hang that still happens on a box must cost one boot's Tube, not a restart loop. (Task 1 isolation; Task 5 `"tube programs: every reachable setting maps to a program with what it needs"`; Task 9 `"tube build guard: a marker left by a dead kiosk trips the guard"`; Task 14 every program under `RuntimeMaxSec`.)
2. **A look change while a video plays.** Moving a row across neutral swaps programs: at most the plan's new programs are built (each ≤ 50 ms, measured), never per frame, and the frame class switches cleanly between 60 and 30 fps. (Task 5 `"tube programs: a cheap row change keeps the programs, a crossing changes them"`; Task 7 `"tube driver: the plan changes only when a row crosses neutral"`; Task 15 step "look changes during video".)
3. **A box up for weeks.** No float grows with uptime: phases are accumulated and wrapped in double; the speed rows do not jump when tuned. (Task 2 `"tube frame: a speed step does not move the band"`, `"tube frame: accumulated phases stay in one cycle after a month of frames"`.)
4. **The Channel Guide's picture window and full-screen films.** The tube draws over the same content rectangle the kiosk uses for everything else; a full-screen film is Rich30. (Task 8 `"content rects: the kiosk's three layouts"`; Task 3 `"tube frame class: a picture larger than the reference is rich"`; Task 15 guide and film checks.)
5. **A display-mode change (720p ↔ 1080p, Modern TV ↔ CRT Native).** Targets resize, the glass rebakes, line and mask pitches recompute, CRT Native draws no inset. (Task 2 `"tube inputs: CRT Native has no inset and no corners"`; Task 7 `"tube driver: a new picture size rebuilds the still"`; Task 15 mode-switch step.)
6. **A GL context rebuild after a game.** `reset_gl()` releases every tube object; the next frame rebuilds programs (inside the guard), targets, mask and glass; no warm-up replays. (Task 10 `release_gl`; Task 15 game-return step with a screenshot.)
7. **The Media Browser taking the screen.** The tube never draws there; the phases pause; on return the first step is capped at 0.1 s. (Task 2 `"tube frame: a long gap advances by at most 0.1 s"`; Task 12 `begin_scene_fbo` gate; Task 15 MB step.)
8. **Rich30 cadence.** Every rich frame lands exactly two vblanks after the last, whether the GPU took 5 ms or 30 ms; the watchdog is pinged every iteration; input is never lost. (Task 8 `"present hold"` cases and `"redraw gate: rich tube motion is continuous, lean tube motion is field rate"`; Task 15 pacing numbers.)
9. **A Pi 4B, and a Pi 5 under `MDB_PLATFORM_POLICY_OVERRIDE=pi4`.** Tube is never offered, never built, and Classic/Enhanced are pixel-identical to the previous release. (Plan 2's gate tests; Task 16.)
10. **The hardware runs leave the box as found.** The preview binary runs beside `/opt`, the build-guard marker and `settings.json` are restored, the production kiosk is restarted. (Tasks 14–16 "Restore" steps.)

---

### Task 1 (HARDWARE): Isolate the compiler hang

**Files:** Create `docs/engineering/measurements/2026-11-15-tube-hang-isolation.txt` (the journal excerpts and the finding).

**Interfaces:** Produces the finding Tasks 5 and 6 consume: the define set (and, if it can be narrowed, the construct) that hangs Mesa 25.0.7's V3D compiler, or "not reproduced".

Suspect, from the round-2 journal: the hang began right after bench row 39 (`P2 everything`), when the bench built row 40, `P2 everything sharpness=50` (`tube_spike.cpp` `kBench`). With sharpness at 50 only the signal program changes, so the suspect is `v2_signal.frag` with `SIG_TONE SIG_CHROMA SIG_CONV SIG_JITTER SCENE_HW_DECODE` (no `SIG_SHARP`).

- [ ] **Step 1: Build and copy the spike** exactly as `.worktrees/tube-spike/magic_dingus_box_cpp/spike/tube/SPIKE.md` §1 (pisim container build, `scp` binary and shaders to `~/tube-spike/`). Target: the owner's Pi 5 (find it as the memory note "Owner's live box magicpi5" says; never `magicpi.local`).
- [ ] **Step 2: Run it bounded.** SPIKE.md §2's `systemd-run`, adding `--property=RuntimeMaxSec=180 --setenv=V3D_DEBUG=shaderdb`. Write `control.json` as `{"look": "everything", "rows": {"sharpness": 50}, "pipeline": 2, "hw_srgb": true}` (one write, whole file). Follow `journalctl -u mdb-tube-spike -f -o cat`.
- [ ] **Step 3: Read the result.** A hang shows a `program signal [...]` line with no `first draw with signal took` line after it, the process at ~100% of a core (`top -H -p $(systemctl show -p MainPID --value mdb-tube-spike)`), and `/proc/<pid>/task/*/stack` or `sudo gdb -p <pid> -batch -ex 'thread apply all bt 8'` inside `libgallium`/`v3d`. Record the define set.
- [ ] **Step 4: Narrow it.** If it hung, re-run Step 2 once per variant, removing one define at a time through `rows` (`chromaBleed: 0`, `convergence: 0`, `jitter: 0`, `contrast: 50, saturation: 50` for tone), then with `"hw_srgb": false`. If it did not hang, run the bench's next rows the same way (`everything` with convergence 0, chroma 0, raster 0, … — `kBench` rows 41–51), one per run. Stop at the first hang or after the list.
- [ ] **Step 5: Restore** (SPIKE.md §3): `sudo systemctl stop mdb-tube-spike; sudo systemctl reset-failed mdb-tube-spike; sleep 2; sudo systemctl start magic-dingus-box-cpp; systemctl is-active magic-dingus-box-cpp` → `active`. `rm -rf ~/tube-spike` at the end of the task.
- [ ] **Step 6: Write the finding** in the measurements file (define set, whether it reproduces, the narrowed construct if any — e.g. "the 6-tap chroma loop with per-gun reads and no sharp block") and commit — `docs: Tube hang isolation on the Pi 5`.

**On a fail to reproduce:** record "not reproduced in N runs"; Task 14 is then the only gate, and Review Focus 1 rests on the build guard.

---

### Task 2: CRT Native's neutral fit, and accumulated phases

**Files:**
- Modify: `magic_dingus_box_cpp/src/tube/tube_inputs.h`
- Modify: `magic_dingus_box_cpp/tests/app/test_tube_frame.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_inputs.cpp`

**Interfaces:**
- Consumes: `tube::Resolved`, `Picture`, `TubeUniforms`, `FrameState`, `tube_frame`, `tube_character_frame`, `detail::row_fraction`, `detail::kTwoPi`.
- Produces (namespace `tube`):
  - `tube_uniforms_for`: when `picture.crt_native`, `cornerPx = 0`, `marginPx = 0`, `flatFit = 1` (design §3, owner question 1).
  - `struct CharacterPhases { double roll = 0.0, drift = 0.0, flag = 0.0; };` — cycles, each in `[0, 1)`.
  - `inline constexpr double kMaxPhaseStepSeconds = 0.1;`
  - `inline double roll_period(const Resolved&) noexcept;` (`30 - 24 * rollSpeed`), `inline double drift_period(const Resolved&) noexcept;` (`24 - 18 * driftSpeed`), `inline double flag_period(const Resolved&) noexcept;` (`12 - 10 * flagSpeed`) — row fractions as today.
  - `inline void advance_character_phases(CharacterPhases&, const Resolved&, double dt) noexcept;` — `dt` clamped to `[0, kMaxPhaseStepSeconds]`; each phase `+= dt / its period`, then wrapped with `x - floor(x)`.
  - `tube_character_frame(TubeUniforms&, const Resolved&, const CharacterPhases&, double tracking_elapsed, bool enabled)` replaces the `double seconds` form: `rollPosition = phases.roll`; drift `sin(kTwoPi * phases.drift)`; flagging's phase `kTwoPi * phases.flag`. Every other line unchanged.
  - `FrameState` gains `CharacterPhases phases{};`; `tube_frame` passes `state.phases`. `seconds` still drives hum, flicker, degauss and signal motion.
  - The header's "What differs" comment gains: the three speed rows' phases are accumulated, so a speed step changes only the rate (Sound Factory derives them from absolute time).

- [ ] **Step 1: Write the failing tests** (in `test_tube_frame.cpp`; rewrite every existing case that calls `tube_character_frame` or builds a `FrameState` for the character — "the rolling band", "optional character is bounded…", "the character keeps moving…", the `tube_frame` step-for-step cases — onto phases: `CharacterPhases p; p.roll = 0.5;` gives `rollPosition == Approx(0.5)`; the bounded loop advances `p` by 0.1 s per step)

```cpp
TEST_CASE("tube frame: a speed step does not move the band", "[tube]") {
    TubeSetting s; set_row(s, Effect::rollBand, 100);          // pvm: rollSpeed 35 -> 21.6 s
    auto r = resolve(s); auto u = tube_uniforms_for(r, Picture{1440.0, 1080.0, false});
    CharacterPhases p;
    for (int i = 0; i < 108; ++i) advance_character_phases(p, r, 0.1);
    REQUIRE(p.roll == Approx(0.5).margin(1e-9));
    tube_character_frame(u, r, p, 0.0, true); const float before = u.rollPosition;
    set_row(s, Effect::rollSpeed, 100); r = resolve(s);         // 6 s a pass
    tube_character_frame(u, r, p, 0.0, true);
    REQUIRE(u.rollPosition == before);                          // the step moved nothing
    advance_character_phases(p, r, 0.1);
    REQUIRE(p.roll == Approx(0.5 + 0.1 / 6.0).margin(1e-9));    // only the rate changed
}
TEST_CASE("tube frame: drift and flagging follow their accumulated phase", "[tube]") {
    auto r = resolve(TubeSetting{});
    r.effects[index_of(Effect::colorDrift)] = 100; r.effects[index_of(Effect::flagging)] = 100;
    auto u = tube_uniforms_for(r, Picture{1440.0, 1080.0, false});
    CharacterPhases p; p.drift = 0.25; p.flag = 0.0;
    tube_character_frame(u, r, p, 0.0, true);
    REQUIRE(u.driftOffset == Approx(0.25 * u.pxPerMm * 3.0));   // sin(pi/2) at character_strength(1, 3) = 3
    REQUIRE(u.flagOffset == Approx(0.0).margin(1e-6));
}
TEST_CASE("tube frame: a long gap advances by at most 0.1 s", "[tube]") {
    auto r = resolve(TubeSetting{}); CharacterPhases p;
    advance_character_phases(p, r, 5.0);                          // a game, the Media Browser
    REQUIRE(p.roll == Approx(0.1 / roll_period(r)));
    advance_character_phases(p, r, -1.0);                         // a clock that went backwards
    REQUIRE(p.roll == Approx(0.1 / roll_period(r)));
}
TEST_CASE("tube frame: accumulated phases stay in one cycle after a month of frames", "[tube]") {
    auto r = resolve(TubeSetting{}); CharacterPhases p;
    p.roll = 0.999999; p.drift = 0.999999; p.flag = 0.999999;
    for (int i = 0; i < 1000; ++i) advance_character_phases(p, r, 1.0 / 60.0);
    for (double x : {p.roll, p.drift, p.flag}) { REQUIRE(x >= 0.0); REQUIRE(x < 1.0); }
}
```

In `test_tube_inputs.cpp`:
```cpp
TEST_CASE("tube inputs: CRT Native has no inset and no corners", "[tube]") {
    auto u = tube_uniforms_for(resolve(TubeSetting{}), Picture{1280.0, 720.0, true});
    REQUIRE(u.cornerPx == 0.0f); REQUIRE(u.marginPx == 0.0f); REQUIRE(u.flatFit == 1.0f);
    auto m = tube_uniforms_for(resolve(TubeSetting{}), Picture{1440.0, 1080.0, false});
    REQUIRE(m.cornerPx > 0.0f); REQUIRE(m.flatFit < 1.0f);         // Modern TV keeps the tube's opening
}
```
- [ ] **Step 2: Run, see them fail** — `cmake --build build-tube3 --target test_app_unit -j8` fails: `CharacterPhases` undeclared.
- [ ] **Step 3: Implement** in `tube_inputs.h`.
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — all pass; then `python3 magic_dingus_box_cpp/scripts/dev/check_tube_parity.py` when `../mdb-sound-factory` is present (it compares constants as text; the three period expressions keep Sound Factory's numbers).
- [ ] **Step 5: Commit** — `tube: CRT Native draws no inset; the speed rows' phases are accumulated`

---

### Task 3: The frame class and the rows that force 30 fps

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_frame_class.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_frame_class.cpp`

**Interfaces:**
- Consumes: `tube::Effect`, `kEffects`, `index_of`, `Resolved`, `Picture`, `neutral_in_crt_native`.
- Produces (namespace `tube`):
  - `enum class FrameClass { Lean60, Rich30 };` `constexpr int frames_per_second(FrameClass) noexcept;` (60 / 30)
  - `inline constexpr double kReferencePicturePixels = 1440.0 * 1080.0;`
  - `inline constexpr std::array<Effect, 7> kForces30{Effect::glow, Effect::afterglow, Effect::sharpness, Effect::convergence, Effect::chromaBleed, Effect::raster, Effect::colorDrift};` — one line per row, so the web check (Task 13) reads it as text.
  - `constexpr bool forces_30fps(Effect) noexcept;`
  - `constexpr int rate_neutral(Effect) noexcept;` — 50 for `sharpness`, else 0.
  - `FrameClass setting_frame_class(const Resolved&, bool crt_native) noexcept;` — Rich30 when any `kForces30` row differs from `rate_neutral`, a row `neutral_in_crt_native` counting as neutral when `crt_native`.
  - `FrameClass frame_class(const Resolved&, const Picture&) noexcept;` — `setting_frame_class(r, picture.crt_native)`, or Rich30 when `picture.w * picture.h > kReferencePicturePixels`.

- [ ] **Step 1: Write the failing test**

```cpp
using namespace tube;
Resolved neutral_rates() {                         // every 30 fps row at its neutral value
    Resolved r = resolve(TubeSetting{});
    for (auto e : kForces30) r.effects[index_of(e)] = rate_neutral(e);
    return r;
}
TEST_CASE("tube frame class: the seven rows, and only they, force 30 fps", "[tube]") {
    int n = 0; for (auto e : kEffects) n += forces_30fps(e);
    REQUIRE(n == 7);
    REQUIRE(forces_30fps(Effect::sharpness)); REQUIRE_FALSE(forces_30fps(Effect::glowSize));
    REQUIRE_FALSE(forces_30fps(Effect::noise)); REQUIRE_FALSE(forces_30fps(Effect::abl));
    REQUIRE(rate_neutral(Effect::sharpness) == 50); REQUIRE(rate_neutral(Effect::glow) == 0);
}
TEST_CASE("tube frame class: any one forcing row makes a setting rich", "[tube]") {
    REQUIRE(setting_frame_class(neutral_rates(), false) == FrameClass::Lean60);
    for (auto e : kForces30) {
        Resolved r = neutral_rates(); r.effects[index_of(e)] = (e == Effect::sharpness) ? 55 : 5;
        INFO(effect_key(e)); REQUIRE(setting_frame_class(r, false) == FrameClass::Rich30);
    }
    Resolved lean = neutral_rates();
    for (auto e : {Effect::noise, Effect::hum, Effect::curvature, Effect::maskStrength, Effect::abl, Effect::glowSize})
        lean.effects[index_of(e)] = 100;
    REQUIRE(setting_frame_class(lean, false) == FrameClass::Lean60);
}
TEST_CASE("tube frame class: CRT Native's neutral rows do not force 30 fps", "[tube]") {
    Resolved r = neutral_rates(); r.effects[index_of(Effect::raster)] = 60;
    REQUIRE(setting_frame_class(r, false) == FrameClass::Rich30);
    REQUIRE(setting_frame_class(r, true) == FrameClass::Lean60);
}
TEST_CASE("tube frame class: a picture larger than the reference is rich", "[tube]") {
    const Resolved r = neutral_rates();
    REQUIRE(frame_class(r, Picture{1440.0, 1080.0, false}) == FrameClass::Lean60);
    REQUIRE(frame_class(r, Picture{960.0, 720.0, false}) == FrameClass::Lean60);
    REQUIRE(frame_class(r, Picture{1920.0, 1080.0, false}) == FrameClass::Rich30);   // full-screen film
    REQUIRE(frames_per_second(FrameClass::Rich30) == 30);
}
```
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** `tube_frame_class.h` (header comment: design §3 "Frame class", the measurements behind 8 ms).
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: the frame class, and the seven rows that force 30 fps`

---

### Task 4: The shipped looks in two classes

**Files:**
- Modify: `magic_dingus_box_cpp/src/tube/tube_looks.h` (pvm and monitor rows, header comment), `magic_dingus_box_cpp/scripts/dev/check_tube_parity.py` (`BOX_RETUNED`), `magic_dingus_box/web/static/tube_rows.json` (regenerated)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_looks.cpp` (`kPinned`, a new case), `magic_dingus_box_cpp/tests/app/test_tube_inputs.cpp` (the Sound-Factory-ported cases that read PVM)

**Interfaces:**
- Consumes: Task 3.
- Produces: PVM `sharpness 63 → 50, glow 8 → 0, afterglow 10 → 0`; Monitor `sharpness 60 → 50, glow 5 → 0`. Nothing else in the table changes. `check_tube_parity.py` gains `BOX_RETUNED = {("pvm", "sharpness"), ("pvm", "glow"), ("pvm", "afterglow"), ("monitor", "sharpness"), ("monitor", "glow")}` reported as deliberate, like `IGNORED_KEYS`.

- [ ] **Step 1: Write the failing test** (and update `kPinned`'s two rows to the new values)

```cpp
TEST_CASE("tube looks: PVM and Monitor are lean, the other four rich", "[tube]") {
    for (auto look : kLooks) {
        TubeSetting s; select_look(s, look);
        const bool lean = look == Look::pvm || look == Look::monitor;
        INFO(look_name(look));
        REQUIRE((setting_frame_class(resolve(s), false) == FrameClass::Lean60) == lean);
    }
    const auto& pvm = look_values(Look::pvm).effects;
    REQUIRE(pvm[index_of(Effect::sharpness)] == 50); REQUIRE(pvm[index_of(Effect::glow)] == 0);
    REQUIRE(pvm[index_of(Effect::afterglow)] == 0);  REQUIRE(pvm[index_of(Effect::abl)] == 20);
}
```
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement.** Edit the two rows; say in the header comment which five cells are box-only and why (design §3 "The shipped looks in two classes"). In `test_tube_inputs.cpp`, the cases ported from Sound Factory that read PVM's sharpness 1.26, glow 0.08 and persistence 0.10 set those three rows to 63, 8, 10 first, so they stay Sound Factory's cases. Regenerate the row file: `MDB_WRITE_TUBE_ROWS_JSON=1 ./build-tube3/test_app_unit "[tube_rows_json]"`.
- [ ] **Step 4: Run** `./build-tube3/test_app_unit` and `./build-tube3/test_ui_unit` in full (fix any case that read PVM's old three values by setting them explicitly), `python -m pytest magic_dingus_box/web/tests/test_tube_rows_json.py -q`, and `check_tube_parity.py` when the sibling checkout exists (it must report the five cells as deliberate, not fail).
- [ ] **Step 5: Commit** — `tube: PVM and Monitor ship lean (60 fps); the other four looks are rich`

---

### Task 5: The program table and the pass plan

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_programs.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_programs.cpp`

**Interfaces:**
- Consumes: Tasks 1, 3; `TubeUniforms`, `tube_uniforms_for`, `glow_sigma_px`, `Resolved`, `Picture`.
- Produces (namespace `tube`):
  - `enum class Program : std::uint8_t { glass, quarter, blur, copy_encode, sig_sharp, sig_sharp_chroma, sig_guns, sig_guns_chroma, lines, out_lean_still, out_lean_moving, out_rich_scene, out_rich_scene_ag, out_rich_signal, out_rich_signal_ag, out_rich_raster, out_rich_raster_ag, out_rich_raster_only, out_rich_raster_only_ag };` `inline constexpr std::size_t kProgramCount = 19;`
  - `enum class ShaderFile : std::uint8_t { glass, quarter, blur, copy_encode, signal, lines, output };`
  - Capability bits `inline constexpr std::uint32_t kCapTone = 1u << 0, kCapDisplace = 1u << 1, kCapSharp = 1u << 2, kCapGuns = 1u << 3, kCapChroma = 1u << 4, kCapSceneSource = 1u << 5, kCapSignalSource = 1u << 6, kCapRaster = 1u << 7, kCapRasterOnly = 1u << 8, kCapGlow = 1u << 9, kCapHistory = 1u << 10, kCapMotion = 1u << 11, kCapAbl = 1u << 12;`
  - `struct ProgramInfo { Program id; std::string_view name; ShaderFile file; std::string_view defines; bool reads_scene; std::uint32_t caps; };` and `inline constexpr std::array<ProgramInfo, kProgramCount> kPrograms` — the table below, in enum order; `constexpr const ProgramInfo& program_info(Program) noexcept;`
  - `inline constexpr bool kLeanBeamCheap = false; inline constexpr bool kRichBeamCheap = false;` — the per-class beam (design §3); the only switches the hardware acceptance may flip.
  - `struct Needs { bool tone = false, displace = false, sharp = false, guns = false, chroma = false, raster = false, raster_only = false, glow = false, afterglow = false, motion = false, abl = false; };`
  - `Needs needs_of(const TubeUniforms& still, const Resolved&) noexcept;` — `tone`: contrast or saturation ≠ 1; `displace`: jitter > 0 or the tracking or flagging row > 0; `sharp`: sharpness ≠ 1; `guns`: convergence > 0 or the colorDrift row > 0; `chroma`: chromaBleed > 0; `raster`: raster > 0; `raster_only`: raster ≥ 1; `glow`: glow > 0; `afterglow`: persistence > 0; `motion`: noise, flicker or hum > 0, or the rollBand or purity row > 0, or `displace`; `abl`: abl or breathe > 0. Read from the STILL (CRT Native already neutral there), never a per-frame value.
  - `struct PassPlan { FrameClass cls = FrameClass::Lean60; bool mean_probe = false, quarter = false, glow = false; std::optional<Program> signal; bool signal_half = false; bool lines = false; Program output = Program::out_lean_still; bool afterglow = false; bool operator==(const PassPlan&) const; };`
  - `PassPlan plan_for(const Needs&, bool larger_than_reference) noexcept;` and `PassPlan plan_passes(const Resolved&, const Picture&) noexcept;` (= `plan_for(needs_of(tube_uniforms_for(r, p), r), p.w * p.h > kReferencePicturePixels)`).
  - `std::vector<Program> programs_of(const PassPlan&);` — every program the plan draws with, `glass` first, no duplicates.
  - `struct TargetSizes { int pic_w, pic_h, quarter_w, quarter_h, signal_w, signal_h, lines_w, lines_h; static constexpr int kMeanW = 64, kMeanH = 48; };` `TargetSizes target_sizes(const PassPlan&, const Picture&, const TubeUniforms& still) noexcept;` — quarter `ceil(w/4) × ceil(h/4)`; signal `w` or `w/2` (`signal_half`) × `h`; lines `signal_w × ceil(still.lines)`.
  - `std::size_t target_bytes(const PassPlan&, const TargetSizes&, bool half_float) noexcept;` — glass `w·h·4`; history `2·w·h·4` (a ping-pong pair) when `afterglow`; signal and lines `·8` (`half_float`) or `·4`; quarter `·4` and two glow targets `·4` when `glow`; mean `64·48·4·4/3`.
  - `std::vector<std::string_view> uniforms_of(Program);` — the `TubeUniforms` fields and the program's own uniforms (`uSceneMap`, `uSceneClamp`, `uSigSize`, `uOrigin`, `uMaskSize`, `uPicSize`, `uDstSize`, `uDir`, `uSigma`, `uRadius`, the samplers) the program declares. Task 6 tests the list against the sources.

The table (`defines` are space-separated names, each emitted as `#define NAME 1`):

| Program | File | Defines | Reads scene | Caps |
|---|---|---|---|---|
| `glass` | glass | — | no | — |
| `quarter` | quarter | — | yes | — |
| `blur` | blur | — | no | — |
| `copy_encode` | copy_encode | — | no | — |
| `sig_sharp` | signal | `SIG_SHARP` | yes | Tone Displace Sharp |
| `sig_sharp_chroma` | signal | `SIG_SHARP SIG_CHROMA` | yes | Tone Displace Sharp Chroma |
| `sig_guns` | signal | `SIG_SHARP SIG_GUNS` | yes | Tone Displace Sharp Guns |
| `sig_guns_chroma` | signal | `SIG_SHARP SIG_GUNS SIG_CHROMA` | yes | Tone Displace Sharp Guns Chroma |
| `lines` | lines | — | no | Raster |
| `out_lean_still` | output | `OUT_SCENE` | yes | SceneSource Tone Abl |
| `out_lean_moving` | output | `OUT_SCENE OUT_MOTION` | yes | SceneSource Tone Displace Motion Abl |
| `out_rich_scene` | output | `OUT_SCENE OUT_MOTION OUT_GLOW` | yes | SceneSource Tone Displace Motion Abl Glow |
| `out_rich_scene_ag` | output | `… OUT_HISTORY` | yes | + History |
| `out_rich_signal` | output | `OUT_SIGNAL OUT_MOTION OUT_GLOW` | no | SignalSource Motion Abl Glow |
| `out_rich_signal_ag` | output | `… OUT_HISTORY` | no | + History |
| `out_rich_raster` | output | `OUT_SIGNAL OUT_RASTER OUT_MOTION OUT_GLOW` | no | SignalSource Raster Motion Abl Glow |
| `out_rich_raster_ag` | output | `… OUT_HISTORY` | no | + History |
| `out_rich_raster_only` | output | `OUT_RASTER OUT_RASTER_ONLY OUT_MOTION OUT_GLOW` | no | Raster RasterOnly Motion Abl Glow |
| `out_rich_raster_only_ag` | output | `… OUT_HISTORY` | no | + History |

`plan_for` rules: a rich need is `sharp`, `guns`, `chroma`, `raster`, `glow`, `afterglow`, or `larger_than_reference` → Rich30, else Lean60. Lean: `out_lean_moving` when `motion`, else `out_lean_still`; no other pass but the mean probe. Rich: signal exists iff `sharp || guns || chroma || raster` (then tone and displacements are done there); signal program by `guns` × `chroma` (`sig_sharp` when neither); `signal_half = chroma || raster`; `lines = raster`; `quarter = glow || chroma`; `glow = glow`; output by source (no signal → `out_rich_scene*`; signal and `raster_only` → `out_rich_raster_only*`; signal and `raster` → `out_rich_raster*`; signal → `out_rich_signal*`), `_ag` iff `afterglow`. Both classes: `mean_probe = abl`. If Task 1 named a construct, add a comment at the table pointing at the finding and the program that avoids it.

- [ ] **Step 1: Write the failing test**

```cpp
using namespace tube;
Needs needs_from_bits(unsigned b) {
    Needs n; bool* f[] = {&n.tone, &n.displace, &n.sharp, &n.guns, &n.chroma, &n.raster,
                          &n.raster_only, &n.glow, &n.afterglow, &n.motion, &n.abl};
    for (unsigned i = 0; i < 11; ++i) *f[i] = (b >> i) & 1u;
    if (n.raster_only) n.raster = true;                 // raster_only implies raster
    if (n.displace) n.motion = true;                    // as needs_of
    return n;
}
TEST_CASE("tube programs: nineteen, named once, in enum order", "[tube]") {
    REQUIRE(kPrograms.size() == 19);
    for (std::size_t i = 0; i < kPrograms.size(); ++i) REQUIRE(static_cast<std::size_t>(kPrograms[i].id) == i);
    std::set<std::string_view> names; for (auto& p : kPrograms) names.insert(p.name);
    REQUIRE(names.size() == 19);
}
TEST_CASE("tube programs: every reachable setting maps to a program with what it needs", "[tube]") {
    for (unsigned b = 0; b < (1u << 11); ++b) for (bool big : {false, true}) {
        const Needs n = needs_from_bits(b); const PassPlan p = plan_for(n, big); INFO(b << " big " << big);
        const bool rich = n.sharp || n.guns || n.chroma || n.raster || n.glow || n.afterglow || big;
        REQUIRE((p.cls == FrameClass::Rich30) == rich);
        const std::uint32_t oc = program_info(p.output).caps;
        REQUIRE(p.signal.has_value() == (rich && (n.sharp || n.guns || n.chroma || n.raster)));
        if (p.signal) {
            const std::uint32_t sc = program_info(*p.signal).caps;
            REQUIRE((sc & kCapTone)); REQUIRE((sc & kCapDisplace));
            if (n.guns) REQUIRE((sc & kCapGuns)); if (n.chroma) REQUIRE((sc & kCapChroma));
            REQUIRE((oc & kCapSceneSource) == 0);
            REQUIRE(p.signal_half == (n.chroma || n.raster));
        } else {
            REQUIRE((oc & kCapSceneSource));
            if (n.tone) REQUIRE((oc & kCapTone)); if (n.displace) REQUIRE((oc & kCapDisplace));
        }
        if (n.motion) REQUIRE((oc & kCapMotion)); if (n.abl) REQUIRE((oc & kCapAbl));
        REQUIRE(p.lines == (rich && n.raster)); if (n.raster && rich) REQUIRE((oc & kCapRaster));
        REQUIRE(((oc & kCapRasterOnly) != 0) == (rich && n.raster_only));
        REQUIRE(p.glow == (rich && n.glow)); if (p.glow) REQUIRE((oc & kCapGlow));
        REQUIRE(p.quarter == (rich && (n.glow || n.chroma)));
        REQUIRE(p.afterglow == (rich && n.afterglow)); REQUIRE(((oc & kCapHistory) != 0) == p.afterglow);
        REQUIRE(p.mean_probe == n.abl);
        if (!rich) REQUIRE((p.output == Program::out_lean_still || p.output == Program::out_lean_moving));
    }
}
TEST_CASE("tube programs: the shipped looks", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    auto plan = [&](Look l) { TubeSetting s; select_look(s, l); return plan_passes(resolve(s), pic); };
    REQUIRE(plan(Look::pvm).cls == FrameClass::Lean60);
    REQUIRE(plan(Look::pvm).output == Program::out_lean_still);       // nothing moves; tone, ABL
    REQUIRE(plan(Look::pvm).mean_probe);
    REQUIRE(programs_of(plan(Look::pvm)) == std::vector<Program>{Program::glass, Program::quarter, Program::out_lean_still});
    const PassPlan tv = plan(Look::tv);
    REQUIRE(tv.signal == Program::sig_guns_chroma); REQUIRE(tv.signal_half); REQUIRE(tv.lines);
    REQUIRE(tv.output == Program::out_rich_raster_ag);
    REQUIRE(plan(Look::arcade).output == Program::out_rich_raster_only_ag);   // raster 100
}
TEST_CASE("tube programs: a cheap row change keeps the programs, a crossing changes them", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    TubeSetting s; const PassPlan a = plan_passes(resolve(s), pic);
    set_row(s, Effect::brightness, 80); set_row(s, Effect::maskStrength, 90);
    REQUIRE(plan_passes(resolve(s), pic) == a);
    set_row(s, Effect::hum, 5);
    REQUIRE(plan_passes(resolve(s), pic).output == Program::out_lean_moving);
    set_row(s, Effect::glow, 5);
    REQUIRE(plan_passes(resolve(s), pic).cls == FrameClass::Rich30);
}
TEST_CASE("tube programs: target sizes and memory", "[tube]") {
    TubeSetting s; select_look(s, Look::tv);
    const Picture pic{1440.0, 1080.0, false}; const Resolved r = resolve(s);
    const PassPlan p = plan_passes(r, pic); const TargetSizes t = target_sizes(p, pic, tube_uniforms_for(r, pic));
    REQUIRE(t.quarter_w == 360); REQUIRE(t.quarter_h == 270);
    REQUIRE(t.signal_w == 720);  REQUIRE(t.signal_h == 1080);
    REQUIRE(t.lines_w == 720);   REQUIRE(t.lines_h == 270);
    for (unsigned b = 0; b < (1u << 11); ++b) {                     // every plan at 1440x1080: under 32 MiB
        const PassPlan q = plan_for(needs_from_bits(b), false);
        const TargetSizes ts{1440, 1080, 360, 270, q.signal_half ? 720 : 1440, 1080, q.signal_half ? 720 : 1440, 270};
        REQUIRE(target_bytes(q, ts, true) < 32u * 1024u * 1024u);
    }
}
```
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** `tube_programs.h`.
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: the fixed program table and the pass plan`

---

### Task 6: The shaders — files, embedding, assembly and CI validation

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/shaders/common.glsl`, `fullscreen.vert`, `glass.frag`, `quarter.frag`, `blur.frag`, `copy_encode.frag`, `signal.frag`, `lines.frag`, `output.frag`
- Create: `magic_dingus_box_cpp/cmake/EmbedTubeShaders.cmake`; `magic_dingus_box_cpp/src/tube/tube_shader_assembly.h`; `magic_dingus_box_cpp/scripts/dev/check_tube_shaders.sh`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (run the embed at configure time with each `.glsl` as a `CMAKE_CONFIGURE_DEPENDS`; add `${CMAKE_BINARY_DIR}/generated` to the kiosk's and `test_app_unit`'s include directories), `.github/workflows/test-local.yml` (new job `tube-shaders`; `kiosk-build`'s assertions), `.github/workflows/release.yml` (the same marker assertion next to the existing `strings` checks)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_shaders.cpp`

**Interfaces:**
- Consumes: Task 5 (`kPrograms`, `program_info`, `uniforms_of`, `kLeanBeamCheap`, `kRichBeamCheap`), Task 1's finding.
- Produces:
  - The generated header `tube/tube_shader_sources.h` (namespace `tube::glsl`): one `inline constexpr std::string_view` per file (`common`, `fullscreen_vert`, `glass`, `quarter`, `blur`, `copy_encode`, `signal`, `lines`, `output`) as raw strings with the delimiter `tube_glsl` (the embed fails the configure if a file contains `)tube_glsl"`), and `inline constexpr std::string_view kTableMarker = "TUBE_GLSL_TABLE_V1";`. Why generated, not hand-written `R"(...)"` as `renderer_shaders.cpp` does: CI must compile the very sources the kiosk embeds, as files, with a validator.
  - `tube_shader_assembly.h` (namespace `tube`): `enum class SceneDecode { Hardware, Shader };` `std::string assemble_fragment(Program, SceneDecode);` — `"#version 300 es\n"`, the program's defines (`#define NAME 1` each), `#define SCENE_HW_DECODE 1` when `Hardware` and the program reads the scene, `#define OUT_BEAM_CHEAP 1` for output programs whose class's beam constant is true (`out_lean_*` → `kLeanBeamCheap`, `out_rich_*` → `kRichBeamCheap`), `"#line 1 1\n"`, `common`, `"\n#line 1 2\n"`, the file. `std::string assemble_vertex();` — `"#version 300 es\n"` + `fullscreen_vert`.
  - The GLSL, ported from the spike files with `crt.metal` the authority for every coefficient and cap:
    - `common.glsl` ← spike `common.glsl`: exact sRGB `decodeGamma`/`encodeGamma` only (no `TUBE_GAMMA` choice), `uiTex` with `SCENE_HW_DECODE`, `sceneUv`, `flipV`.
    - `signal.frag` ← `v2_signal.frag`: tone and the three displacements always compiled and switched by uniform tests (`if (jitter > 0.0)`, …); `SIG_SHARP` (switched inside by `sharpness != 1.0`, so a raster-only look with sharpness 50 pays one tap), `SIG_GUNS` (convergence + drift offsets, three reads), `SIG_CHROMA` (six quarter taps + YIQ).
    - `lines.frag` ← `v2_lines.frag` unchanged in arithmetic.
    - `output.frag` ← `v2_output.frag`, restructured: `OUT_SCENE` reads the scene directly and applies tone and (with `OUT_MOTION`) the three displacements before the beam; `OUT_SIGNAL` reads one signal tap; `OUT_RASTER`/`OUT_RASTER_ONLY` as the spike; the mask always from the texture (the arithmetic variant is gone; `uMaskSize` 1×1 white for none); the bend always per pixel (no mesh); rolloff, tint, the power block (warm-up, degauss, spot) and ABL/breathing switched by uniform tests in every output; `OUT_MOTION` adds noise, flicker, hum, the band and purity, each behind its uniform test; `OUT_GLOW` adds the glow tap behind `glow > 0.0`; `OUT_HISTORY` writes linear light into the history and reads the previous one (else encodes for the screen); the glass texel's RGB divided by 32 (`kGlassLightScale`).
    - `glass.frag` ← `v2_glass.frag`, RGB written ×32 into an `RGBA8` target.
    - `quarter.frag` ← `downsample.frag`; `blur.frag` ← `blur.frag`; `copy_encode.frag` ← `copy.frag` with `ENCODE_OUTPUT` always.
    - Nothing in any file may reproduce the construct Task 1 named; say so in a comment where the nearest code is.
  - `scripts/dev/check_tube_shaders.sh <dir>`: runs `glslangValidator -S frag <f>` on every `*.frag` and `-S vert` on `fullscreen.vert` in `<dir>`; prints `N checked, M failed`; exits 1 on any failure or when `N != 29` (19 hardware-decode programs + the 9 that read the scene again with shader decode + the vertex shader).
  - `test-local.yml` job `tube-shaders` (ubuntu-latest): `apt-get install -y build-essential cmake pkg-config libjsoncpp-dev libyaml-cpp-dev glslang-tools`; configure `-DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF`; build `test_app_unit`; `MDB_WRITE_TUBE_GLSL=$RUNNER_TEMP/glsl ./build/test_app_unit "[tube_shaders]"`; `bash magic_dingus_box_cpp/scripts/dev/check_tube_shaders.sh $RUNNER_TEMP/glsl`. `kiosk-build` and `release.yml`: `strings "$B" | grep -q "TUBE_GLSL_TABLE_V1" || { echo "FATAL: tube shaders not embedded"; exit 1; }`.

- [ ] **Step 1: Write the failing test** (`test_tube_shaders.cpp`, tags `[tube][tube_shaders]`)

```cpp
using namespace tube;
TEST_CASE("tube shaders: every program assembles with its defines first", "[tube][tube_shaders]") {
    for (const auto& info : kPrograms) for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
        const std::string src = assemble_fragment(info.id, decode); INFO(info.name);
        REQUIRE(src.rfind("#version 300 es\n", 0) == 0);
        REQUIRE(src.find("#version", 1) == std::string::npos);
        std::istringstream words{std::string(info.defines)}; std::string d;
        while (words >> d) REQUIRE(src.find("#define " + d + " 1\n") != std::string::npos);
        const bool hw = decode == SceneDecode::Hardware && info.reads_scene;
        REQUIRE((src.find("#define SCENE_HW_DECODE 1\n") != std::string::npos) == hw);
        REQUIRE(src.find("TUBE_MASK_TYPE") == std::string::npos);      // the mask is data
        REQUIRE(src.find("OUT_MESH") == std::string::npos);            // the bend is per pixel
    }
    REQUIRE(assemble_vertex().rfind("#version 300 es\n", 0) == 0);
    REQUIRE(glsl::kTableMarker == "TUBE_GLSL_TABLE_V1");
}
TEST_CASE("tube shaders: each program declares the uniforms the renderer uploads", "[tube][tube_shaders]") {
    for (const auto& info : kPrograms) {
        const std::string src = assemble_fragment(info.id, SceneDecode::Hardware);
        for (std::string_view u : uniforms_of(info.id)) {
            INFO(info.name << " " << u);
            const std::regex decl("uniform\\s+\\w+\\s+[^;]*\\b" + std::string(u) + "\\b");
            REQUIRE(std::regex_search(src, decl));
        }
    }
}
TEST_CASE("tube shaders: written out for the validator when asked", "[tube][tube_shaders]") {
    const char* dir = std::getenv("MDB_WRITE_TUBE_GLSL");
    if (dir == nullptr || *dir == '\0') return;
    std::filesystem::create_directories(dir); int n = 0;
    for (const auto& info : kPrograms) for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
        if (decode == SceneDecode::Shader && !info.reads_scene) continue;
        std::ofstream(std::string(dir) + "/" + std::string(info.name) + (decode == SceneDecode::Hardware ? ".hw" : ".sw") + ".frag")
            << assemble_fragment(info.id, decode);
        ++n;
    }
    std::ofstream(std::string(dir) + "/fullscreen.vert") << assemble_vertex();
    REQUIRE(n == 28);
}
```
- [ ] **Step 2: Run, see it fail** — `tube/tube_shader_sources.h` not found.
- [ ] **Step 3: Implement** the embed, the assembly and the nine files. Locally: `brew install glslang`, then `MDB_WRITE_TUBE_GLSL=/tmp/tube-glsl ./build-tube3/test_app_unit "[tube_shaders]" && bash magic_dingus_box_cpp/scripts/dev/check_tube_shaders.sh /tmp/tube-glsl` → `29 checked, 0 failed`.
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass; `bash magic_dingus_box_cpp/dev/pisim/pisim.sh build` — the kiosk links and `strings magic_dingus_box_cpp/build-pisim/magic_dingus_box_cpp | grep TUBE_GLSL_TABLE_V1` prints it (the kiosk includes the header in Task 10; until then add a one-line `#include` in `renderer_crt.cpp` referencing `tube::glsl::kTableMarker` in a log line at init, which Task 10 replaces).
- [ ] **Step 5: Commit** — `tube: the shaders as files, embedded, assembled, and validated in CI`

---

### Task 7: The per-frame driver

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_driver.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_driver.cpp`

**Interfaces:**
- Consumes: Tasks 2, 3, 5; `tube_frame`, `FrameState`, `settle_frames`, `moves_on_its_own`, `tracking_moving`, `warmup_gain`, `kWarmupSeconds`, `kAblSeconds`, `kSettledWithin`, `kSettleFramesPerSecond`, `glow_sigma_px`, `mask_gain`.
- Produces (namespace `tube`):
  - `struct MaskTile { int w = 1, h = 1; std::vector<std::uint8_t> rgb; };` `MaskTile mask_tile(Mask, int pitch, float strength);` — one period of the mask, linear 0..255, port of the spike's `mask_at` (crt.metal `maskAt`); `none` → 1×1 white.
  - `inline constexpr float kGlassLightScale = 32.0f;` `inline constexpr double kDegaussSeconds = 1.4, kDegaussMovesSeconds = 1.5, kWarmupMovesSeconds = kWarmupSeconds + 0.1, kMeanPeriodSeconds = 0.2;`
  - `class AblLimiter { public: void on_mean(double light); void step(double now); double level() const; double target() const; double distance() const; bool measured() const; };` — Sound Factory's `readMean` + `stepBrightnessLimitTo`: the first mean is adopted at once; a step from rest counts `1/kSettleFramesPerSecond`; otherwise `elapsed` clamped to `[0, 1]`; `level += (target - level) * (1 - exp(-elapsed / kAblSeconds))`.
  - `struct DriverPeek { FrameClass cls; bool moving; };`
  - `struct DriverFrame { TubeUniforms u; Resolved resolved; PassPlan plan; TargetSizes sizes; float glow_sigma = 0.0f; MaskTile mask; bool still_changed = false; bool mask_changed = false; bool moving = false; };`
  - `class FrameDriver { public:`
    `void start(double now, std::uint64_t degauss_requests);` — the engine began drawing (boot or a switch to Tube): warm-up from `now`; the degauss baseline is `degauss_requests`.
    `DriverPeek peek(const TubeSetting&, const Picture&, double now, std::uint64_t degauss_requests) const;` — for the redraw gate, before drawing; changes nothing.
    `DriverFrame frame(const TubeSetting&, const Picture&, double now, std::uint64_t degauss_requests);` — called once per DRAWN tube frame.
    `void on_mean(double light);` `bool mean_due(double now) const;` (5 Hz while the plan has `mean_probe`) `void note_mean_requested(double now);`
    `};`
  - `frame()`'s order: rebuild the still (`tube_uniforms_for`, `plan_passes`, `target_sizes`, `glow_sigma_px`) only when the setting or the picture changed (`still_changed`), and the mask tile only when mask, pitch or strength changed (`mask_changed`); restart the tracking clock when any of `tracking`, `trackingRate`, `trackingWidth`, `trackingRecovery` changed (Sound Factory: a tracking control auditions at once); `dt = now - last` → `advance_character_phases`; the degauss start when `degauss_requests` exceeds the baseline (baseline then follows); `AblLimiter::step(now)`; `FrameState` as `TubeRenderer.mm` lines 937–958 (`warmupGain`, `degauss = max(0, 1 - (now - degauss_start) / 1.4)`, `motion_amount = (now - start - kWarmupMovesSeconds) / 0.5`, `rising_light = max(0, target - level)`, `ablLevel`); `tube_frame(still, resolved, fs)`; the settle debt (`settle_frames(u, distance)`) re-armed on `still_changed` or a new mean target, minus one each frame. `moving` = `moves_on_its_own(resolved) || tracking_moving(resolved, tracking_elapsed) || now - start < kWarmupMovesSeconds || now - degauss_start < kDegaussMovesSeconds || settle_debt > 0`.

- [ ] **Step 1: Write the failing test**

```cpp
using namespace tube;
const Picture k1080{1440.0, 1080.0, false};
TEST_CASE("tube driver: warm-up from start, then still", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(100.0, 0);
    REQUIRE(d.frame(s, k1080, 100.1, 0).u.warmupGain == 0.0f);
    REQUIRE(d.peek(s, k1080, 101.0, 0).moving);
    auto f = d.frame(s, k1080, 102.0, 0);
    REQUIRE(f.u.warmupGain == 1.0f);
    REQUIRE_FALSE(d.peek(s, k1080, 102.1, 0).moving);           // pvm moves nothing, settled
}
TEST_CASE("tube driver: degauss when the counter rises, never at start", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 7);              // presses made before the engine drew
    REQUIRE(d.frame(s, k1080, 3.0, 7).u.degauss == 0.0f);
    REQUIRE(d.frame(s, k1080, 4.0, 8).u.degauss == Approx(1.0f));
    REQUIRE(d.frame(s, k1080, 4.7, 8).u.degauss == Approx(0.5f));
    REQUIRE(d.peek(s, k1080, 5.45, 8).moving);
    REQUIRE(d.frame(s, k1080, 5.6, 8).u.degauss == 0.0f);
    REQUIRE_FALSE(d.peek(s, k1080, 5.6, 8).moving);
}
TEST_CASE("tube driver: the plan changes only when a row crosses neutral", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 0); d.frame(s, k1080, 2.0, 0);
    set_row(s, Effect::brightness, 70);
    auto f = d.frame(s, k1080, 2.1, 0); REQUIRE(f.still_changed); REQUIRE(f.plan.output == Program::out_lean_still);
    REQUIRE_FALSE(d.frame(s, k1080, 2.2, 0).still_changed);
    set_row(s, Effect::noise, 10);
    REQUIRE(d.frame(s, k1080, 2.3, 0).plan.output == Program::out_lean_moving);
}
TEST_CASE("tube driver: a new picture size rebuilds the still", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 0);
    auto a = d.frame(s, k1080, 2.0, 0);
    auto b = d.frame(s, Picture{960.0, 720.0, false}, 2.1, 0);
    REQUIRE(b.still_changed); REQUIRE(a.u.lines == Approx(270.0)); REQUIRE(b.u.lines == Approx(240.0));
    auto c = d.frame(s, Picture{1280.0, 720.0, true}, 2.2, 0);     // CRT Native
    REQUIRE(c.u.maskType == 0.0f); REQUIRE(c.u.scanlines == 0.0f); REQUIRE(c.u.cornerPx == 0.0f);
}
TEST_CASE("tube driver: the band moves by dt, and not at all while not drawn", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::rollBand, 50); d.start(0.0, 0);
    const float p0 = d.frame(s, k1080, 2.0, 0).u.rollPosition;
    const float p1 = d.frame(s, k1080, 2.1, 0).u.rollPosition;
    REQUIRE(p1 - p0 == Approx(0.1 / 21.6).margin(1e-6));
    const float p2 = d.frame(s, k1080, 600.0, 0).u.rollPosition;  // ten minutes in the Media Browser
    REQUIRE(p2 - p1 == Approx(0.1 / 21.6).margin(1e-6));
}
TEST_CASE("tube driver: the limiter follows the mean on the frames' clock", "[tube]") {
    AblLimiter a; a.on_mean(0.8); REQUIRE(a.level() == Approx(0.8));  // the first picture is not news
    a.on_mean(0.2); a.step(10.0);                                      // from rest: one 30 Hz frame
    REQUIRE(a.level() == Approx(0.8 - 0.6 * (1.0 - std::exp(-(1.0 / 30.0) / 0.1))));
}
TEST_CASE("tube driver: the mask tile passes what its gain gives back", "[tube]") {
    for (auto m : {Mask::grille, Mask::magentaGreen, Mask::dots, Mask::slot}) for (int pitch : {3, 4, 6}) {
        const MaskTile t = mask_tile(m, pitch, 0.48f); double sum = 0;
        for (std::size_t i = 0; i < t.rgb.size(); ++i) sum += t.rgb[i] / 255.0;
        INFO(mask_name(m) << " " << pitch);
        REQUIRE(sum / t.rgb.size() * mask_gain(m, 0.48f) == Approx(1.0).margin(0.01));
    }
    REQUIRE(mask_tile(Mask::none, 3, 0.5f).w == 1);
}
```
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** `tube_driver.h`.
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: the per-frame driver — still cache, phases, limiter, warm-up, degauss`

---

### Task 8: One content rectangle, the cadence and the redraw gate

**Files:**
- Create: `magic_dingus_box_cpp/src/video/content_area.h`, `magic_dingus_box_cpp/src/app/tube_redraw.h`
- Modify: `magic_dingus_box_cpp/src/utils/frame_pacing.h`, `magic_dingus_box_cpp/src/app/redraw_gate.h`, `magic_dingus_box_cpp/src/app/redraw_gate.cpp`
- Test: `magic_dingus_box_cpp/tests/video/test_content_area.cpp`, `magic_dingus_box_cpp/tests/utils/test_frame_pacing.cpp`, `magic_dingus_box_cpp/tests/app/test_redraw_gate.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_redraw.cpp`

**Interfaces:**
- Consumes: `video::IRect`, `video::four_by_three_rect`; `tube::FrameClass`.
- Produces:
  - `video::ContentRects { IRect content; IRect logical_content; bool letterbox_active; };` `ContentRects content_rects(bool modern_tv, bool full_screen_film, bool media_browser, int fb_w, int fb_h, int canvas_w, int canvas_h);` — exactly main.cpp's computation today (`letterbox_active = modern_tv && !full_screen_film && !media_browser`; then `four_by_three_rect` of each), moved, not changed.
  - `utils::present_hold(int refresh_hz, std::chrono::microseconds since_last_present) -> std::chrono::microseconds` — `max(0, period + period / 8 - since_last_present)`, `refresh_hz <= 0` treated as 60 (the same period and margin as `frame_pacing_for`).
  - `app::MainMenuActivity::tube_rich_motion` (new, default false): a Rich30 look with something moving; makes the menu non-static, and is NOT a field-rate reason.
  - `app::TubeRedraw { bool field_rate_motion = false; bool continuous_motion = false; bool hold_present = false; };` `TubeRedraw app::tube_redraw(bool tube_drawing, tube::FrameClass, bool tube_moving, int vrefresh_hz) noexcept;` — not drawing: all false. Lean60: `field_rate_motion = tube_moving`. Rich30: `continuous_motion = tube_moving`, `hold_present = vrefresh_hz >= 48`.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_content_area.cpp
TEST_CASE("content rects: the kiosk's three layouts", "[video]") {
    auto m = video::content_rects(true, false, false, 1920, 1080, 960, 720);
    REQUIRE(m.content == video::IRect{240, 0, 1440, 1080});
    REQUIRE(m.logical_content == video::IRect{0, 0, 960, 720}); REQUIRE(m.letterbox_active);
    auto f = video::content_rects(true, true, false, 1920, 1080, 960, 720);   // full-screen film
    REQUIRE(f.content == video::IRect{0, 0, 1920, 1080}); REQUIRE_FALSE(f.letterbox_active);
    REQUIRE(video::content_rects(true, false, true, 1920, 1080, 960, 720).content == video::IRect{0, 0, 1920, 1080});
    auto c = video::content_rects(false, false, false, 1280, 720, 640, 480);  // CRT Native
    REQUIRE(c.content == video::IRect{0, 0, 1280, 720}); REQUIRE(c.logical_content == video::IRect{0, 0, 640, 480});
    REQUIRE(video::content_rects(true, false, false, 1280, 720, 960, 720).content == video::IRect{160, 0, 960, 720});
}
// test_frame_pacing.cpp
TEST_CASE("present hold: the flip lands two vblanks after the last", "[pacing]") {
    using us = std::chrono::microseconds;
    REQUIRE(utils::present_hold(60, us{5000}) == us{16666 + 2083 - 5000});   // fast frame: wait
    REQUIRE(utils::present_hold(60, us{18749}) == us{0});
    REQUIRE(utils::present_hold(60, us{30000}) == us{0});                    // slow frame: present now
    REQUIRE(utils::present_hold(0, us{0}) == utils::present_hold(60, us{0}));
    REQUIRE(utils::present_hold(50, us{0}) == us{20000 + 2500});
}
// test_redraw_gate.cpp
TEST_CASE("redraw gate: rich tube motion is continuous, lean tube motion is field rate", "[redraw]") {
    app::MainMenuActivity a; REQUIRE(app::is_static_main_menu(a));
    a.tube_rich_motion = true;
    REQUIRE_FALSE(app::is_static_main_menu(a)); REQUIRE_FALSE(app::is_crt_field_rate_main_menu(a));
    app::MainMenuActivity b; b.crt_time_effects = true;                       // lean tube motion rides this
    REQUIRE(app::is_crt_field_rate_main_menu(b));
}
// test_tube_redraw.cpp
TEST_CASE("tube redraw: what each class asks of the loop", "[tube]") {
    using tube::FrameClass;
    auto none = app::tube_redraw(false, FrameClass::Rich30, true, 60);
    REQUIRE_FALSE(none.field_rate_motion); REQUIRE_FALSE(none.continuous_motion); REQUIRE_FALSE(none.hold_present);
    auto lean = app::tube_redraw(true, FrameClass::Lean60, true, 60);
    REQUIRE(lean.field_rate_motion); REQUIRE_FALSE(lean.hold_present);
    auto rich = app::tube_redraw(true, FrameClass::Rich30, false, 60);
    REQUIRE_FALSE(rich.continuous_motion); REQUIRE(rich.hold_present);       // held even when still
    REQUIRE(app::tube_redraw(true, FrameClass::Rich30, true, 60).continuous_motion);
    REQUIRE_FALSE(app::tube_redraw(true, FrameClass::Rich30, true, 30).hold_present);  // a 30 Hz mode already is
}
```
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement.** Comments: `content_area.h` says main.cpp and the tube must both use it; `present_hold` says why Rich30 holds the present instead of skipping an iteration (design §4: the skip-then-render field rate leaves 14.6 ms, a rich frame needs up to 24).
- [ ] **Step 4: Run** `./build-tube3/test_video_unit`, `test_utils_unit`, `test_app_unit` in full — pass (every existing redraw-gate and pacing case unchanged).
- [ ] **Step 5: Commit** — `tube: one content rectangle, the Rich30 present hold, and the gate's tube motion`

---

### Task 9: The pacing monitor, the GPU timing averager and the build guard

**Files:**
- Create: `magic_dingus_box_cpp/src/app/tube_pacing.h`, `magic_dingus_box_cpp/src/tube/tube_gpu_timing.h`, `magic_dingus_box_cpp/src/app/tube_build_guard.h`, `magic_dingus_box_cpp/src/app/tube_build_guard.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (`APP_SOURCES` and `test_app_unit`: `src/app/tube_build_guard.cpp`)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_pacing.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_build_guard.cpp`

**Interfaces:**
- Produces:
  - `app::TubePacingMonitor` — `using Clock = std::chrono::steady_clock;` `void on_present(Clock::time_point done, tube::FrameClass, int vrefresh_hz, bool video_playing);` — an interval is late when it exceeds `period × (Rich30 ? 2 : 1) + period / 2` (the first present after a gap of more than 250 ms counts nothing). `struct Report { std::uint64_t presents = 0, late = 0, video_presents = 0, video_late = 0; tube::FrameClass cls; };` `std::optional<Report> take_report(Clock::time_point now);` — every 60 s (first call starts the window). `bool too_heavy() const;` — over the last 30 s of video presents (window must span ≥ 30 s), `late > 5%`. `bool take_toast();` — true once when `too_heavy` first holds; `void reset();` (a setting change) clears the window and re-arms the toast.
  - `tube::PassTimingAverager` — `enum class Pass { quarter, glow, signal, lines, output, history, probe };` `void add(Pass, double ms);` `void end_frame();` `std::optional<std::string> take_line(double now_seconds);` — every 10 s: `"tube gpu: <n> frames: quarter <a> glow <b> signal <c> lines <d> output <e> history <f> probe <g> = <total> ms"`, two decimals, `-` for a pass that ran in no frame, averages over the frames each pass ran in, `total` = sum of those averages.
  - `app::TubeBuildGuard` — `static constexpr const char* kDefaultPath = "/dev/shm/mdb-tube-build";` `explicit TubeBuildGuard(std::string marker_path);` (reads it once: present → `tripped()`; its content is the program name) `bool tripped() const;` `const std::string& tripped_program() const;` `void begin(std::string_view program);` (writes the name; a write error logs WARN and continues) `void end();` (removes it). The marker is in memory-backed `/dev/shm`: it survives a kiosk restart, not a reboot.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_tube_pacing.cpp
using namespace std::chrono_literals; using tube::FrameClass;
TEST_CASE("tube pacing: late is half a refresh past the class's cadence", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    m.take_report(t);
    for (int i = 0; i < 10; ++i) { t += 33333us; m.on_present(t, FrameClass::Rich30, 60, true); }
    t += 50000us; m.on_present(t, FrameClass::Rich30, 60, true);              // missed: 50 ms
    t += 41000us; m.on_present(t, FrameClass::Rich30, 60, true);              // 41 ms < 33.3 + 8.3: on time
    auto r = m.take_report(t + 60s); REQUIRE(r); REQUIRE(r->presents == 12); REQUIRE(r->late == 1);
}
TEST_CASE("tube pacing: too heavy after 30 s of video with more than 5% late, toast once", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    for (int i = 0; i < 1900; ++i) { t += (i % 10 == 0) ? 33333us : 16667us; m.on_present(t, FrameClass::Lean60, 60, true); }
    REQUIRE(m.too_heavy()); REQUIRE(m.take_toast()); REQUIRE_FALSE(m.take_toast());
    m.reset(); REQUIRE_FALSE(m.too_heavy());
    app::TubePacingMonitor menu; t += 1s;                                      // no video: never too heavy
    for (int i = 0; i < 1900; ++i) { t += 33333us; menu.on_present(t, FrameClass::Lean60, 60, false); }
    REQUIRE_FALSE(menu.too_heavy());
}
TEST_CASE("tube gpu timing: one line every ten seconds", "[tube]") {
    tube::PassTimingAverager a; using P = tube::PassTimingAverager::Pass;
    REQUIRE_FALSE(a.take_line(0.0));
    for (int i = 0; i < 2; ++i) { a.add(P::output, 7.0 + i); a.add(P::probe, 0.2); a.end_frame(); }
    REQUIRE_FALSE(a.take_line(5.0));
    REQUIRE(*a.take_line(10.0) ==
            "tube gpu: 2 frames: quarter - glow - signal - lines - output 7.50 history - probe 0.20 = 7.70 ms");
}
// test_tube_build_guard.cpp
TEST_CASE("tube build guard: a marker left by a dead kiosk trips the guard", "[tube]") {
    const auto path = (std::filesystem::temp_directory_path() / "mdb-tube-build-test").string();
    std::filesystem::remove(path);
    { app::TubeBuildGuard g(path); REQUIRE_FALSE(g.tripped()); g.begin("sig_guns_chroma"); }   // died here
    app::TubeBuildGuard after(path);
    REQUIRE(after.tripped()); REQUIRE(after.tripped_program() == "sig_guns_chroma");
    std::filesystem::remove(path);
    app::TubeBuildGuard clean(path); clean.begin("glass"); clean.end();
    REQUIRE_FALSE(app::TubeBuildGuard(path).tripped());
}
```
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: pacing monitor, GPU timing averager and the build guard`

---

### Task 10: The GL side, part 1 — capabilities, programs, targets, mask and glass

**Files:**
- Create: `magic_dingus_box_cpp/src/ui/tube_renderer.h`, `magic_dingus_box_cpp/src/ui/tube_renderer.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (`UI_SOURCES`: `src/ui/tube_renderer.cpp`, `src/ui/tube_renderer_passes.cpp` — the second file is an empty TU with the header include until Task 11), `magic_dingus_box_cpp/src/ui/renderer_crt.cpp` (drop Task 6's temporary marker line)

**Interfaces:**
- Consumes: Tasks 5, 6, 7, 9 (`TubeBuildGuard`, `PassTimingAverager`); `ui::Renderer`'s VAO/VBO and 2D program (passed in, not reached into).
- Produces (namespace `ui`, kiosk-only; nothing calls it until Task 12):
  ```cpp
  class TubeRenderer {
  public:
      struct Caps { bool srgb_write_control = false, srgb_decode = false, half_float_targets = false, timer_query = false; };
      struct GlEnv { GLuint scene_tex = 0; int scene_w = 0, scene_h = 0, fb_w = 0, fb_h = 0; GLuint vao = 0, vbo = 0, ui_program = 0; };
      explicit TubeRenderer(std::string guard_path = app::TubeBuildGuard::kDefaultPath);
      ~TubeRenderer();                                   // no GL: release_gl() runs from Renderer::cleanup()
      bool available() const;                            // guard not tripped and no sticky failure
      std::string_view unavailable_reason() const;       // "" | "build_hung" | "build_failed"
      const Caps& caps();                                // probed once per context
      void begin_scene(GLuint scene_tex, int w, int h);  // SRGB8_ALPHA8 storage (re-specified, same name) + FRAMEBUFFER_SRGB off
      void end_tube_scene(GLuint scene_tex, int w, int h);  // back to RGBA8: the engine left Tube
      bool prepare(const tube::PassPlan&, tube::SceneDecode);  // builds what the plan lacks, inside the guard
      bool composite(const GlEnv&, video::IRect picture, const tube::DriverFrame&);  // Task 11
      bool validate_all_programs(tube::SceneDecode);     // Task 14: build + first-draw every table program
      void release_gl();
  private:
      struct Impl; std::unique_ptr<Impl> impl_;
  };
  ```
- Behaviour decided here (reference: spike `tube_spike.cpp` `log_gl_info`, `build`, `first_use`, `ensure_tex`, `free_tex`, `begin_scene`, `scene_decode`, the glass bake):
  - **Caps** from `glGetString(GL_EXTENSIONS)`: the four extensions in the Tech Stack line; `SceneDecode::Hardware` iff `srgb_write_control && srgb_decode`.
  - **Programs:** one cache slot per `tube::Program`; `prepare` builds each program of `programs_of(plan)` not yet built: `guard.begin(name)` → compile `assemble_vertex()` + `assemble_fragment(p, decode)` → link → cache uniform locations for `uniforms_of(p)` → one first draw of that program into a 16×16 scratch target (Mesa compiles its hardware variant at first draw) → `guard.end()`. Each build logs `tube: program <name> built in <compile+link> ms, first draw <ms> ms`. A compile/link failure logs the info log once, sets the sticky `build_failed`, returns false. Never builds a program outside `kPrograms`.
  - **Targets** by `TargetSizes` (re-made only when a size or the plan's need changes): quarter, glow tmp, glow out, history (×2, ping-pong) `SRGB8_ALPHA8`; signal and lines `RGBA16F` when `half_float_targets`, else `SRGB8_ALPHA8`; glass `RGBA8`; mean `SRGB8_ALPHA8` 64×48 with mipmaps; mask `RGBA8` from `MaskTile`, NEAREST + REPEAT. An incomplete FBO is a sticky `build_failed`.
  - **Glass:** baked with the `glass` program when `DriverFrame::still_changed`; mask uploaded when `mask_changed`.
  - **Every** non-batched GL call follows `Renderer`'s `flush_ui_batch()` convention where it touches shared state (the TubeRenderer is called with the batch already flushed by its caller; say so in the header and assert nothing is pending with a debug log).
  - `release_gl()` deletes every program, texture, FBO, query and PBO and zeroes the ids; the next `prepare` rebuilds.

- [ ] **Step 1: Write the code** (`tube_renderer.{h,cpp}`) — the GL body follows the spike functions named above; no bench, no file loading, no control file, no per-frame timer read-back.
- [ ] **Step 2: Verify in the Pi-userspace container:** `bash magic_dingus_box_cpp/dev/pisim/pisim.sh check` — the kiosk binary builds, every suite passes on aarch64. `grep -n "TubeRenderer" magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/src/ui/renderer*.cpp` → no hits (nothing constructs it yet: production unchanged).
- [ ] **Step 3: Commit** — `tube: GL resources and the program cache (not wired)`

---

### Task 11: The GL side, part 2 — the passes, the mean probe and the debug timers

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/tube_renderer_passes.cpp`, `magic_dingus_box_cpp/src/ui/tube_renderer.cpp`

**Interfaces:**
- Consumes: Task 10; `tube::DriverFrame`, `PassPlan`, `TargetSizes`, `uniforms_of`, `FrameDriver::on_mean` (through a callback), `PassTimingAverager`.
- Produces: `TubeRenderer::composite(env, picture, frame)` and `void set_mean_sink(std::function<void(double)>)`; `bool TubeRenderer::mean_requested_this_frame() const`.
- `composite` order (reference: spike `TubeSpike::composite` and `Impl::draw_v2`):
  1. `prepare(frame.plan, decode)` (no-op when built); false → return false.
  2. GL in: the renderer's VAO/VBO with a clip-space quad; blend off; `GL_FRAMEBUFFER_SRGB_EXT` **on** (the tube's sRGB targets encode); scene texture `GL_TEXTURE_SRGB_DECODE_EXT = GL_DECODE_EXT` (hardware path).
  3. Mean probe when the driver said it is due: `quarter` into the 64×48 target, `glGenerateMipmap`, `glReadPixels` of level 6 (1×1) into a `GL_PIXEL_PACK_BUFFER`, `glFenceSync`; each later frame `glClientWaitSync(fence, 0, 0)` — when signalled, `glMapBufferRange` 4 bytes, decode sRGB on the CPU, luminance `0.2126 r + 0.7152 g + 0.0722 b`, hand it to the sink. Never waits.
  4. Rich passes the plan asks for: quarter → blur H → blur V; signal (viewport `signal_w × signal_h`, `uSigSize`); lines.
  5. Clear the bars black (viewport whole framebuffer); output into the picture rectangle on the default framebuffer — `picture` from the caller, GL's bottom-left y `fb_h - y - h` — or into the history texture (`_ag`), then `copy_encode` from it to the picture rectangle; swap the history pair.
  6. GL out exactly as `end_scene_fbo_and_composite` leaves it: VAO 0, FBO 0, viewport whole framebuffer, units 7..0 unbound, blend on with `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, the 2D program bound, `GL_FRAMEBUFFER_SRGB_EXT` **off**, scene decode back to `GL_SKIP_DECODE_EXT`.
  - Uniform upload: for each name in `uniforms_of(program)`, the cached location and the `DriverFrame::u` field (a name → member table like the spike's `kFields`, with a `static_assert` that it covers `TubeUniforms`).
  - `MDB_TUBE_GPU_TIMING=1` (read once at construction, logged at INFO when on): each pass inside a `GL_TIME_ELAPSED_EXT` query from a ring of 4 sets; a set is read only when `GL_QUERY_RESULT_AVAILABLE` says so, else dropped; results go to `PassTimingAverager`, whose line is logged at INFO. Off: no query objects exist.
  - Any GL error during the frame (`glGetError` once at the end) → log once, sticky failure, return false (the caller draws Enhanced).

- [ ] **Step 1: Write the code.**
- [ ] **Step 2: Verify:** `bash magic_dingus_box_cpp/dev/pisim/pisim.sh check`; still no caller (`grep` as Task 10).
- [ ] **Step 3: Commit** — `tube: the passes, the async mean probe and the debug GPU timers (not wired)`

---

### Task 12: Wiring — the renderer, the main loop and the preview build

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/renderer.h`, `magic_dingus_box_cpp/src/ui/renderer.cpp` (construct, `reset_gl`, `cleanup`), `magic_dingus_box_cpp/src/ui/renderer_crt.cpp` (`begin_scene_fbo`, `end_scene_fbo_and_composite`), `magic_dingus_box_cpp/src/main.cpp`, `magic_dingus_box_cpp/src/app/app_state.h`, `magic_dingus_box_cpp/CMakeLists.txt` (option `MDB_TUBE_PREVIEW`, default OFF), `.github/workflows/test-local.yml` (`kiosk-build`) and `.github/workflows/release.yml` (one assertion each)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_menu_logic.cpp` (one case)

**Interfaces:**
- Consumes: Tasks 7–11; `app::effective_crt_engine`, `app::tube_offered`, `TubeLiveState::degauss_requests`.
- Produces:
  - `app_state.h`: `kTubeRendererReady` becomes `true` only under `#ifdef MDB_TUBE_PREVIEW` (a TEST-ONLY build switch, never set by any install path; removed in Task 17). `struct TubeRenderStatus { bool drawing = false; int frame_rate = 60; bool too_heavy = false; std::string unavailable; };` and `AppState::tube_render`.
  - `Renderer`: `void set_tube_picture(video::IRect content, bool crt_native);` `struct TubeStatus { bool drawing = false; tube::FrameClass cls = tube::FrameClass::Lean60; bool moving = false; };` `TubeStatus tube_status(const app::AppState&, double now) const;` (drawing iff the effective engine is Tube, not the Media Browser, and `TubeRenderer::available()`; class and moving from `FrameDriver::peek`) `std::string_view tube_unavailable_reason() const;` `bool prepare_tube(const app::AppState&, video::IRect picture, bool crt_native);` (boot: build the startup plan's programs).
  - `begin_scene_fbo`: the engine is `effective_crt_engine(crt_engine, tube_offered(state))`; for Tube with an available TubeRenderer, gates 1 and 4 (the Enhanced flag and "any Enhanced effect") do not apply; `FrameDriver::start` on the first Tube frame after any other engine; `TubeRenderer::begin_scene` before the clear. Leaving Tube calls `end_tube_scene`.
  - `end_scene_fbo_and_composite`: Tube → `composite(env, tube_picture_, driver.frame(...))`; false → this frame's Enhanced composite (its program exists; the scene bytes are today's).
  - `reset_gl()` / `cleanup()`: `tube_->release_gl()`.
  - `main.cpp`: `const video::ContentRects rects = video::content_rects(...)` once per iteration before the gate; the draw block uses `rects` instead of its inline computation; `ui_renderer.set_tube_picture(rects.content, crt_native)`; the gate block gets `ui_renderer.tube_status(state, now)` → `app::tube_redraw(...)`: when Tube draws, `act.crt_time_effects = r.field_rate_motion` (the Classic/Enhanced values are not drawn) and `act.tube_rich_motion = r.continuous_motion`; before `present_frame()`, when `r.hold_present`, sleep `utils::present_hold(vrefresh, now - last_present_done)`; after it, `tube_pacing.on_present(...)` while Tube draws, its per-minute report at INFO (`"Tube pacing: class <30|60>, <n> presents (<v> with video), <l> late"`), `take_toast()` → `ui::Toast::show("This CRT look is too heavy for smooth video")`, and `tube_pacing.reset()` when the tube setting changes; `state.tube_render` filled each iteration. At boot, before `app::start_intro`: `ui_renderer.prepare_tube(state, rects.content, crt_native)` when the effective engine is Tube. A tripped guard logs `LOG_ERROR("tube: the last kiosk died building program {} — Tube draws as Enhanced until reboot", name)` once.
  - Preview builds log `LOG_WARN("TUBE PREVIEW BUILD — test only, never ship")` at start. `kiosk-build` and `release.yml`: `if strings "$B" | grep -q "TUBE PREVIEW BUILD"; then echo "FATAL: a Tube preview build"; exit 1; fi`.

- [ ] **Step 1: Write the failing test** (pins that production still offers nothing)

```cpp
TEST_CASE("tube menu logic: a production build is not renderer-ready", "[tube]") {
#ifndef MDB_TUBE_PREVIEW
    REQUIRE_FALSE(app::kTubeRendererReady);
    app::AppState s; s.platform_profile.tube_crt_engine = true;
    s.display_settings.set_crt_engine(app::CrtEngine::Tube);
    REQUIRE(app::effective_crt_engine(s.display_settings.crt_engine, app::tube_offered(s)) == app::CrtEngine::Enhanced);
#endif
}
```
- [ ] **Step 2: Run** `./build-tube3/test_app_unit "[tube]"` — passes already (the constant is false): it is the guard for Step 3.
- [ ] **Step 3: Implement** the wiring.
- [ ] **Step 4: Verify:** `./build-tube3/test_app_unit`, `test_video_unit`, `test_utils_unit` in full; `bash magic_dingus_box_cpp/dev/pisim/pisim.sh check`; and a preview build compiles: `docker run --rm --platform linux/arm64 -v "$PWD:/repo" -w /repo mdb-pisim:trixie bash -c 'cmake -S magic_dingus_box_cpp -B magic_dingus_box_cpp/build-pisim-preview -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_KIOSK=ON -DENABLE_MEDIA_BROWSER=ON -DBUILD_TESTS=OFF -DMDB_TUBE_PREVIEW=ON && cmake --build magic_dingus_box_cpp/build-pisim-preview -j6'` and `strings` finds `TUBE PREVIEW BUILD` in it and not in `build-pisim/`'s binary. Read the diff of `main.cpp` once more for anything that runs when Tube is not drawing: only `content_rects` (pinned by Task 8's test to today's rectangles) and `set_tube_picture` (stores two values).
- [ ] **Step 5: Commit** — `tube: wired into the renderer and the main loop, inert until the renderer is switched on`

---

### Task 13: The controls show which rows force 30 fps

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/tube_rows_json.h`, `magic_dingus_box/web/static/tube_rows.json` (regenerated), `magic_dingus_box_cpp/src/app/tube_menu_logic.h`, `magic_dingus_box_cpp/src/ui/settings_menu.cpp` (the `TUBE_VALUE` rows' sublabel), `magic_dingus_box_cpp/src/app/status_writer.cpp` (`tube_status_json`), `magic_dingus_box/web/static/manager_display.js`, `magic_dingus_box/web/static/index.html` (`manager_display.js?v=` +1), `magic_dingus_box_cpp/scripts/dev/check_tube_parity.py` (`parse_forces_30`)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_rows_json.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_menu_logic.cpp`, `magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp`, `magic_dingus_box/web/tests/test_tube_rows_json.py`, `magic_dingus_box/web/tests/test_manager_display_js.py`

**Interfaces:**
- Consumes: Task 3 (`forces_30fps`, `kForces30`, `setting_frame_class`, `frames_per_second`); Task 12 (`AppState::tube_render`); Plan 2's `tube_rows_json()`, `tube_look_label`, `tube_status_json`, `tubeShown`.
- Produces:
  - Row file: each row gains `"forces_30fps": bool` (after `crt_native_neutral`); each look gains `"fps": 60|30` (`setting_frame_class(resolve(look), false)`) after `"mask"`. Regenerate with `MDB_WRITE_TUBE_ROWS_JSON=1`.
  - `tube_menu_logic.h`: `tube_look_label` appends `" · 60 fps"` / `" · 30 fps"` from `setting_frame_class(resolve(s), crt_native)` — signature becomes `std::string tube_look_label(const tube::TubeSetting&, bool crt_native);` (Plan 2's callers pass `mode == CRT_NATIVE`); `std::string_view tube_rate_tag(tube::Effect) noexcept;` (`"30 fps"` or `""`), appended as `" · 30 fps"` to a forcing row's sublabel by `settings_menu.cpp`.
  - Status `tube` block (offered): `"frame_rate"` (`frames_per_second(setting_frame_class(resolved, crt_native))`), `"too_heavy"` (`tube_render.too_heavy`), `"unavailable"` (`tube_render.unavailable` or null).
  - `manager_display.js`: `function tubeRateTag(row)` → `"30 fps"` when `row.forces_30fps`, else `""`, shown beside the row's label; each look's `fps` in the look picker; the header shows `"<frame_rate> fps"` and, when `too_heavy`, "This look is too heavy for smooth video on this TV".
  - `check_tube_parity.py`: `parse_forces_30(text)` reads `kForces30`'s `Effect::` names from `tube_frame_class.h`; `test_tube_rows_json.py` asserts the JSON's `forces_30fps` set equals it.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_tube_rows_json.cpp (append)
TEST_CASE("tube rows json: the 30 fps rows and each look's rate", "[tube][tube_rows_json]") {
    Json::Value root; std::istringstream in(app::tube_rows_json()); in >> root;
    int forcing = 0; for (const auto& r : root["rows"]) forcing += r["forces_30fps"].asBool();
    REQUIRE(forcing == 7);
    REQUIRE(root["looks"][0]["key"].asString() == "pvm"); REQUIRE(root["looks"][0]["fps"].asInt() == 60);
    REQUIRE(root["looks"][3]["key"].asString() == "tv");  REQUIRE(root["looks"][3]["fps"].asInt() == 30);
}
// test_tube_menu_logic.cpp (update Plan 2's label cases to the new signature, and append)
TEST_CASE("tube menu logic: the Look row says its rate; forcing rows are tagged", "[tube]") {
    tube::TubeSetting s;                                           // pvm: lean
    REQUIRE(app::tube_look_label(s, false) == "Look: PVM · 60 fps");
    tube::set_row(s, tube::Effect::glow, 10);
    REQUIRE(app::tube_look_label(s, false) == "Look: PVM (1 moved) · 30 fps");
    REQUIRE(app::tube_rate_tag(tube::Effect::glow) == "30 fps");
    REQUIRE(app::tube_rate_tag(tube::Effect::hum).empty());
}
// test_status_writer.cpp (append)
//   offered state, look tv → t["frame_rate"].asInt() == 30, t["too_heavy"].asBool() == false, t["unavailable"].isNull();
//   state.tube_render.unavailable = "build_hung" → t["unavailable"].asString() == "build_hung".
```
```python
# test_tube_rows_json.py
def test_forces_30fps_matches_the_header():
    want = parity.parse_forces_30((CPP / "src/tube/tube_frame_class.h").read_text())
    assert {r["key"] for r in ROWS["rows"] if r["forces_30fps"]} == want
    assert want == {"glow", "afterglow", "sharpness", "convergence", "chromaBleed", "raster", "colorDrift"}
# test_manager_display_js.py
def test_rate_tag_marks_only_forcing_rows():
    # node: tubeRateTag({forces_30fps: true}) == "30 fps"; tubeRateTag({forces_30fps: false}) == ""
```
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement**; regenerate the row file.
- [ ] **Step 4: Run** `./build-tube3/test_app_unit`, `test_ui_unit`, `test_phone_remote_unit` in full; `python -m pytest magic_dingus_box/web/tests/test_tube_rows_json.py magic_dingus_box/web/tests/test_manager_display_js.py -q`; `pisim.sh check`.
- [ ] **Step 5: Commit** — `tube: the controls tag the rows that force 30 fps and show each look's rate`

---

### Task 14 (HARDWARE): Every program compiled and drawn on the Pi 5

**Files:** Create `docs/engineering/measurements/2026-11-xx-tube-programs-pi5.txt` (journal excerpt).

**Interfaces:** Consumes Tasks 10–12 (`TubeRenderer::validate_all_programs`, triggered at startup in a preview build by `MDB_TUBE_VALIDATE=1`: both decode paths, each program built and first-drawn into a 64×64 scratch target inside the guard, one line each, then `"tube validate: N of N programs OK"`; the env is read only in a preview build).

**The preview runbook (Tasks 14–16 use it).** Never touches `/opt` or `/etc`.
1. Build on the Mac: the `docker run … -DMDB_TUBE_PREVIEW=ON` command of Task 12 Step 4.
2. Copy: `PI=magic@<the Pi 5>`; `ssh $PI 'mkdir -p ~/tube-preview'`; `scp magic_dingus_box_cpp/build-pisim-preview/magic_dingus_box_cpp $PI:tube-preview/`; `ssh $PI 'ldd ~/tube-preview/magic_dingus_box_cpp | grep "not found" || echo libs OK'`.
3. Save state: `ssh $PI 'sudo systemctl stop magic-dingus-box-cpp && sleep 2 && cp -a /opt/magic_dingus_box/magic_dingus_box_cpp/config/settings.json ~/tube-preview/settings.json.bak && rm -f /dev/shm/mdb-tube-build'`.
4. Start: SPIKE.md §2's `systemd-run` with `--unit=mdb-tube-preview`, `--working-directory=/home/magic/tube-preview`, the binary `/home/magic/tube-preview/magic_dingus_box_cpp`, without `MDB_TUBE_SPIKE_DIR` and `MDB_REDRAW_GATE`, plus the task's `--setenv=` lines and `--property=RuntimeMaxSec=<bound>`. Follow `journalctl -u mdb-tube-preview -f -o cat`.
5. Drive looks without the web page: write the op log the kiosk polls, as `magic`, atomically — `seq` = the status's `handled_seq` + 1:
   ```bash
   tube_op() { # tube_op '{"op":"look","look":"tv"}'
     d=/opt/magic_dingus_box/magic_dingus_box_cpp/data
     s=$(jq '.tube.handled_seq' $d/kiosk_status.json); s=$((s+1))
     jq -n --argjson op "$1" --argjson s $s '{version:1, ops:[$op + {seq:$s}]}' > $d/tube_request.json.tmp
     mv $d/tube_request.json.tmp $d/tube_request.json; }
   ```
   The engine: `tube_op '{"op":"engine","engine":"tube"}'`. Rows: `tube_op '{"op":"row","key":"glow","value":0}'`.
6. **Restore (always, also after a failure):** `sudo systemctl stop mdb-tube-preview; sudo systemctl reset-failed mdb-tube-preview 2>/dev/null; sudo rm -f /dev/shm/mdb-tube-build; cp -a ~/tube-preview/settings.json.bak /opt/magic_dingus_box/magic_dingus_box_cpp/config/settings.json; sleep 2; sudo systemctl start magic-dingus-box-cpp; systemctl is-active magic-dingus-box-cpp` → `active`. Delete `~/tube-preview` at the end of the task. A marker left in `/dev/shm` would make the production kiosk draw Enhanced for Tube until reboot — which production never draws yet, but the restore removes it anyway.

- [ ] **Step 1:** Runbook 1–4 with `--setenv=MDB_TUBE_VALIDATE=1 --setenv=V3D_DEBUG=shaderdb --property=RuntimeMaxSec=120`.
- [ ] **Step 2: Measure:** every one of the 28 fragment programs logs `built in … first draw …`; the summary line says `28 of 28`. From the shaderdb lines: each program's instruction count and thread count.
- [ ] **Step 3: Pass/fail:** PASS when all 28 build, every first draw ≤ 50 ms, `out_lean_still` and `out_lean_moving` run in 4-thread mode, and the unit did not hit `RuntimeMaxSec` (`systemctl show -p Result mdb-tube-preview` ≠ `timeout`). A hang (no line for a program, then the timeout): the marker names it; bisect its blocks the way Task 1 did, change that program's source (never add a program), re-run Task 6's validator and this task. A lean output in 2-thread mode: move its rarest uniform-gated block (purity, then the band) out of `out_lean_moving` into `OUT_MOTION`-only code paths, re-measure; if it stays, record it — Task 15's budget decides.
- [ ] **Step 4:** Runbook 6. Write the journal excerpt and commit — `docs: Tube program table compiled and drawn on the Pi 5`.

---

### Task 15 (HARDWARE): Budgets per look, and the cases that bite

**Files:** Modify `magic_dingus_box_cpp/src/tube/tube_looks.h`, `tests/app/test_tube_looks.cpp`, `magic_dingus_box/web/static/tube_rows.json` (when a look is lowered), `magic_dingus_box_cpp/src/tube/tube_programs.h` (only `kLeanBeamCheap` / `kRichBeamCheap`, if flipped); create `docs/engineering/measurements/2026-11-xx-tube-acceptance-pi5.txt`.

**Interfaces:** Consumes Task 12's journal lines (`tube gpu: …`, `Tube pacing: …`), Task 14's runbook.

- [ ] **Step 1: Per look, video playing.** Runbook 1–4 with `--setenv=MDB_TUBE_GPU_TIMING=1 --property=RuntimeMaxSec=7200`. Start a 960×720 H.264 playlist video (phone remote or pad, Modern TV 1080p). For each look (`tube_op '{"op":"look","look":"<pvm|monitor|trinitron|tv|arcade|everything>"}'`): 10 minutes; record the ten `Tube pacing` lines and the sixty `tube gpu` lines; `touch /opt/magic_dingus_box/magic_dingus_box_cpp/data/screenshot_request` once and keep the newest `.bmp`.
- [ ] **Step 2: Pass/fail per look.** Lean60 (PVM, Monitor): mean `tube gpu` total ≤ 8.00 ms and `late / presents` ≤ 1% over the ten minutes. Rich30 (the other four): total ≤ 24.00 ms and late ≤ 1% at the 33.3 ms cadence. Screenshot: bent glass, scanlines and mask inside the cabinet art; nothing flashing.
- [ ] **Step 3: On a fail, in this order, one change at a time, re-running only that look:** a lean look → `kLeanBeamCheap = true` (re-run Task 14 Step 1 for the two lean outputs and Task 6's validator) → lower its `scanlines` toward 0 → report to the owner that the look should go back to Rich30 (its Sound Factory sharpness) and stop. A rich look → `kRichBeamCheap = true` → `afterglow` 0 → `glow` halved, then 0 → `raster` lowered. Each lowering is an edit to `tube_looks.h`, its pinned test and the regenerated row file, committed with the measured before/after numbers in the message.
- [ ] **Step 4: The cases.** Each under the PASS look named, each recorded:
  - *Look changes during video* (PVM → TV → PVM → Everything, 10 s apart): no frame longer than 100 ms in the pacing lines except the change frame; the change frame's `tube: program … built` lines ≤ 50 ms each.
  - *Static menu* (stop the video, open nothing): PVM → `Redraw gate` first-window line shows skipping; Everything with every wear row 100 (`rollBand tracking colorDrift flagging purity` via `tube_op`) → ≤ 30 presents a second and the Settings menu readable in a screenshot.
  - *Channel Guide* (menu style Guide, a channel with video in its window): screenshot — guide and window through one tube; pacing as the look's class.
  - *Full-screen film* (a channel film that fills the screen): each rich look and PVM — late ≤ 1% at 33.3 ms (Rich30 forced).
  - *Display-mode change*: Modern TV 1080p → 720p → CRT Native → back, with PVM and TV: no GL error, screenshot after each, CRT Native shows no inset and no rounded corners.
  - *A game and back* (launch any PS1 game, quit with Select+Start): the tube returns on the first frame, `tube: program … built` lines after the return, no warm-up replay, screenshot.
  - *Media Browser* (if the box has it): enter and leave; the MB draws clean; the tube resumes without a jump.
  - *Degauss* (`tube_op '{"op":"degauss"}'` on a static menu): ~1.5 s of drawing, then rest.
  - *Heat:* 30 minutes of TV with video in the enclosure: `vcgencmd get_throttled` → `throttled=0x0`.
  - *Memory:* `grep MemAvailable /proc/meminfo` on the static menu with Classic, then with TV: falls by < 32 MB.
- [ ] **Step 5:** Runbook 6. Write the acceptance file (every number above, per look) and commit with the look changes — `tube: looks lowered to their class budgets on the Pi 5, and the acceptance numbers`.

---

### Task 16 (HARDWARE): The Pi 4B, and the policy override

**Files:** Append to Task 15's acceptance file.

- [ ] **Step 1: Pi 4B, preview build** (runbook against a Pi 4B; never a customer's): `jq .tube kiosk_status.json` → `"gate": false, "reason": "needs_pi5"`; Settings → Display offers "Classic / Enhanced" only; the Content Manager shows no CRT Look tab; `journalctl -u mdb-tube-preview | grep -c "tube: program"` → `0`; `tube_op '{"op":"engine","engine":"tube"}'` changes nothing (status still Enhanced). Screenshots of the static main menu with Classic and with Enhanced (default sliders); restore; the same two screenshots from the production kiosk (the previous release); `cmp` each pair → identical.
- [ ] **Step 2: Pi 5 under the override.** On the Pi 5, the runbook with `--setenv=MDB_PLATFORM_POLICY_OVERRIDE=pi4`: the same four checks as Step 1 (`reason` `needs_pi5`). Restore; confirm `verify_box.sh` passes on both boxes after restore (it FAILs while the override is in any unit — the transient unit is gone by then).
- [ ] **Step 3: Commit** the appended file — `docs: Tube stays off on the Pi 4B and under the Pi 4 policy rehearsal`.

---

### Task 17: Switch it on

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (`kTubeRendererReady = true`, the `MDB_TUBE_PREVIEW` block removed), `magic_dingus_box_cpp/CMakeLists.txt` (the option removed), `magic_dingus_box_cpp/src/main.cpp` (the preview log line removed), `magic_dingus_box_cpp/tests/app/test_tube_menu_logic.cpp` (Task 12's case and Plan 2's `"Tube is offered only on a Pi 5 with the renderer ready"` updated to the new default), `CLAUDE.md` (Architecture: `src/tube/`, `ui/tube_renderer`, the two frame classes and the present hold, the program table and the validator job, the build guard, `MDB_TUBE_GPU_TIMING`), `CHANGELOG.md` (`## [Unreleased]`), `magic_dingus_box_cpp/docs/OWNER_GUIDE.md` only if it lists the CRT engines (customer words: "Tube — a Sound Factory picture tube; some looks run at 30 frames a second")
- Keep: `release.yml`'s `TUBE PREVIEW BUILD` assertion (it now guards nothing and costs nothing; it stops the switch from coming back).

**Interfaces:** Produces the only visible change of the three plans: on a Pi 5, Settings → Display offers Tube and the Content Manager shows the CRT Look tab.

- [ ] **Step 1: Update the tests** — `REQUIRE(app::kTubeRendererReady);` and Plan 2's gate case: a default `AppState` on a Pi 5 profile is offered; `tube_not_offered_reason` is `"needs_pi5"` on a Pi 4B profile and `""` on a Pi 5.
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Flip the constant; remove the preview switch.**
- [ ] **Step 4: Run** all Mac suites (9 with Media Browser ON, 8 OFF: `cmake -S magic_dingus_box_cpp -B build-tube3-mb -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=ON` then `ctest --test-dir build-tube3-mb`, and the same OFF in `build-tube3`), the web pytest, `pisim.sh check`. On the Pi 5 with `dev/pisim/pisim.sh push`: select Tube on the TV, PVM — 60 fps, then TV — 30 fps; `verify_box.sh` passes. Then delete `build-tube3*` and `build-pisim-preview` (the Mac's disk).
- [ ] **Step 5: Commit** — `tube: switch the Tube engine on (Pi 5)`

---

## Requirement map (design → task)

| Design requirement | Task |
|---|---|
| §3 Frame class, "forces 30 fps" flag | 3, 13 |
| §3 Shipped looks in two classes | 4, 15 |
| §3 Pipeline 2 passes, hardware sRGB decode + fallback, half width, half float, glass ×32 | 5, 6, 10, 11 |
| §3 Fixed program table, CI validator, compiled-and-drawn on a Pi 5 | 5, 6, 14 |
| §3 Programs built on engine start / need change, never per frame | 7, 10, 12 |
| §3 Compile hang isolated, build guard | 1, 9, 10, 12, 14 |
| §3 Picture rectangle from the content area; guide window; full-screen film Rich30; bezel on top | 3, 8, 12, 15 |
| §3 CRT Native neutral fit (call) | 2, 15 |
| §3 Accumulated phases (call) | 2, 7 |
| §3 ABL mean probe, async, lean-allowed | 5, 7, 11, 15 |
| §3 GPU timing only under the debug variable | 9, 11 |
| §3 Failure → Enhanced | 10, 11, 12 |
| §4 Lean60 field rate, Rich30 present hold, video/input/watchdog | 8, 12, 15 |
| §4 Resting and settle debt | 7, 8, 15 |
| §6 30 fps tags on the TV and the web | 13 |
| §8 Warm-up at engine start; degauss from the request counter | 7, 12, 15 |
| §9 Too heavy: per-minute log, toast, status flag | 9, 12, 13 |
| Acceptance: budgets, pacing, heat, memory, Pi 4B | 15, 16 |
| Build order 4: switch-on last | 17 |
