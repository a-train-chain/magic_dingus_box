# Tube CRT Engine — Renderer (Plan 3 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Tasks marked **HARDWARE** are runbooks for the controller (workers cannot reach a Pi); a worker does only their steps marked *(worker)*.

**Goal:** Draw the Tube engine on a Pi 5 — a lean look at 60 fps in one pass, a rich look at an even 30 fps through pipeline 2 — from a fixed, pre-tested table of shader programs, with the lean class and both budgets fixed by measurement, and no change to what any box shows until the last task switches it on after hardware acceptance.

**Architecture:** Decisions are pure and Mac-tested: `src/tube/` gains the measured budget and lean set (`tube_budget.h`, filled from Task 1b's table), the frame class (`tube_frame_class.h`), the program table, pass plan and cost prediction (`tube_programs.h`), the shader assembly (`tube_shader_assembly.h`, over GLSL files in `src/tube/shaders/` embedded at build time) and the per-frame driver (`tube_driver.h`). `src/app/` gains the cadence decision (`tube_redraw.h`), the pacing monitor and the build guard; `src/video/content_area.h` becomes the one source of the content rectangle; `utils/frame_pacing.h` gains the Rich30 present hold. The GL side is one kiosk-only class, `ui::TubeRenderer`, owned by `ui::Renderer` and reached through `begin_scene_fbo` / `end_scene_fbo_and_composite`. Everything is decided by `app::effective_crt_engine(crt_engine, app::tube_offered(state))`, which stays Enhanced on every box while `app::kTubeRendererReady` is false; hardware runs use a test-only `MDB_TUBE_PREVIEW` build that is never deployed to `/opt`.

**Tech Stack:** C++17, OpenGL ES 3.0 (+ `GL_EXT_sRGB_write_control`, `GL_EXT_texture_sRGB_decode`, `GL_EXT_color_buffer_half_float`, `GL_EXT_disjoint_timer_query`, all optional), GLSL ES 3.00, Catch2 (`test_app_unit`, `test_utils_unit`, `test_video_unit`, `test_ui_unit`, `test_phone_remote_unit`), CMake, `glslangValidator` (CI only), Python 3 + pytest, node (the web helper test), bash.

**Spec:** `docs/superpowers/specs/2026-11-15-tube-crt-engine-design.md` as amended after Phase 0 and review — §3 Rendering, §4 Resting and the two frame rates, §8, §9, Acceptance, Testing. Measurements: `docs/engineering/measurements/2026-11-15-tube-phase0*-journal.txt`. Plans 1 (`2026-11-15-tube-crt-core.md`, built) and 2 (`2026-11-15-tube-crt-controls.md`) are what this plan consumes.

**Reference, never merged:** the spike worktree `.worktrees/tube-spike` (branch `spike/tube-phase0`): `magic_dingus_box_cpp/src/ui/tube_spike.cpp` (pipeline 2's passes, `begin_scene`/`scene_decode` for the sRGB write-control trick, `ensure_tex`, the timer ring `tbegin`/`tend`/`read_set`, `build`/`first_use`, `mask_at`, `upload_scene`) and `magic_dingus_box_cpp/spike/tube/` (`v2_*.frag`, `common.glsl`, `downsample.frag`, `blur.frag`, `copy.frag`, `SPIKE.md`). Port from them; do not copy bench code, file loading, the control file or per-frame read-back. **Every coefficient and flash-safety cap comes from `../mdb-sound-factory/engine/src/panel/crt.metal` at `4ef74a6`**; the renderer's per-frame bookkeeping from `TubeRenderer.mm` (`stepBrightnessLimitTo`, `readMean`, `kDegaussSeconds`, the block at lines 937–958). Tasks 1 and 1b add measurement code to the spike branch only.

**Depends on Plan 2:** Tasks 2–12 need only Plan 2's Tasks 1–7 (in this tree or landing). Task 13 needs Plan 2 complete (Tasks 3, 5, 6b, 9 are the files it edits). Rebase this branch onto Plan 2's tip before Task 13. **Tasks 3, 4 and 5 consume Task 1b's table**: where a value is needed before it exists, it is written as the provisional value below and marked `// from Task 1b's table (provisional)`; when the table lands, those constants and their pinned tests are updated in one commit.

**Provisional values (the round-2 predictions; replaced by Task 1b):** lean budget **8.0 ms**, rich budget **24.0 ms**; lean set = bend, beam (cheap), tone, displacements, motion (noise/flicker/hum/band/purity), probe — i.e. every row except the seven always-rich ones may stay lean, with the cheap beam; PVM and Monitor **not** lean until Task 1b says a lean PVM-like pass holds 60 fps (predicted ~9.3 ms exact beam, ~8.3–8.6 cheap: at or over the edge); per-pass costs at 1440×1080: quarter 0.7, glow blur 4.0, signal full `sig_sharp` 4.4 / `sig_sharp_chroma` 7.1 / `sig_guns` 7.1 / `sig_guns_chroma` 10.0 (half width ×0.56), lines 1.2 half / 2.0 full, output lean 9.3 / rich scene or signal 9.1 / rich raster 11.6 / rich raster only 10.1, history with copy 4.5, probe 0.3.

## Global Constraints

- The repository folder's name ends in a space: `.../📺 MDB/magic_dingus_box `. Work in the worktree `.worktrees/tube-crt-renderer` (branch `tube-crt-renderer`); use paths relative to its root. In bash, `cd "$HOME/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tube-crt-renderer"` first.
- **No task before Task 17 changes what any box shows, and none deploys to `/opt`.** Every Tube code path is reached only when `app::effective_crt_engine(display_settings.crt_engine, app::tube_offered(state)) == CrtEngine::Tube`, never true while `kTubeRendererReady` is false. Code that runs regardless (the content-rect extraction, the gate's new field, the pacing monitor) must be behaviour-neutral when Tube is not drawing, and each such task names the test that pins it.
- Dual-board contract: board differences only through `app::tube_offered(state)`. No `#ifdef` on a board. The one `#ifdef` this plan adds (`MDB_TUBE_PREVIEW`, Task 12) is a test-build switch, defined for every target by `add_compile_definitions`, removed in Task 17.
- No new kiosk dependency. GL extensions are probed at runtime; without them the same programs decode in the shader. `glslang-tools` is CI-only tooling (not a `-dev` package; `tests/local/system_deps_parity.bats` does not see it).
- `src/tube/` stays header-only, standard library only, no I/O; the generated shader header is standard library only.
- The render thread never blocks: no `glFinish`, no synchronous `glReadPixels`, no `glGetQueryObject` without `GL_QUERY_RESULT_AVAILABLE` first. No recurring file write on the main loop; the build guard writes `/dev/shm` only when a program is built.
- **No program is built before READY=1** (the watchdog starts there). Frames whose programs are not all built draw through Enhanced.
- The renderer's rules: `flush_ui_batch()` before every non-batched GL call in the `Renderer` files; every GL object the tube owns is released by `Renderer::reset_gl()` and `cleanup()` and rebuilt on its next frame; the Media Browser never draws through the tube; the bezel and toast are drawn after the composite; the scene FBO is sized to the framebuffer.
- Shaders: only programs from `tube::kPrograms` are ever compiled; never per frame.
- Exact values: reference picture 1440×1080 px; phase step cap 0.1 s; mean probe 64×64 at 5 Hz; degauss 1.4 s, moving 1.5 s; warm-up 1.5 s, moving 1.6 s; glass light ×32; build guard `/dev/shm/mdb-tube-build`; too heavy: > 5% of presents late over 30 s with a video playing; GPU timing log every 10 s; pacing report every 60 s; budget margins 0.5 ms (lean), 1.0 ms (rich).
- Mac build (one directory; the Mac's disk is nearly full — delete it when the plan is finished). `test_app_unit` is skipped without yaml-cpp (`brew install yaml-cpp`):
  ```bash
  cmake -S magic_dingus_box_cpp -B build-tube3 -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF
  cmake --build build-tube3 --target test_app_unit test_utils_unit test_video_unit test_ui_unit test_phone_remote_unit -j8
  ```
  Kiosk-only files (`main.cpp`, `renderer*.cpp`, `tube_renderer*.cpp`) are verified by `magic_dingus_box_cpp/dev/pisim/pisim.sh check` (the real kiosk binary in the Pi-userspace container, plus every suite on aarch64) and CI's `kiosk-build` job. Shaders by `scripts/dev/check_tube_shaders.sh` (Task 6).
- One commit per task; messages end with the session's attribution line.

## The hardware runbook (Tasks 1, 1b, 14, 15, 16, 17 use it)

The box (the owner's Pi 5, or a Pi 4B for Task 16) is **shared with another session that runs VPN tests**.

1. **Ask for a window.** Through the coordinator, ask the other session for a window on the box and wait until it is granted. Record the time.
2. **Before:** `ssh $PI 'systemctl show -p MainPID -p NRestarts magic-dingus-box-cpp; sha256sum /opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp'` (the "before" line of the report).
3. **What a run touches under `/opt` — truthfully.** The test binary and its scripts live in `~/tube-preview` (or `~/tube-spike`), never under `/opt`. But the test kiosk runs on the production data (`MAGIC_BASE_PATH`): it writes `magic_dingus_box_cpp/config/settings.json` (a look or engine chosen during the run is saved), `data/kiosk_status.json`, `data/tube_request.json` (created by `tube_op` below), `data/screenshot_request` and `data/screenshots/*.bmp` (from captures), and whatever the kiosk writes in normal running (logs, watch checkpoints, the phone-remote queues).
4. **Save:** `ssh $PI 'sudo systemctl stop magic-dingus-box-cpp && sleep 2 && cp -a /opt/magic_dingus_box/magic_dingus_box_cpp/config/settings.json ~/tube-preview/settings.json.bak && ls /opt/magic_dingus_box/magic_dingus_box_cpp/data/screenshots > ~/tube-preview/screenshots.before 2>/dev/null; rm -f /dev/shm/mdb-tube-build'`.
5. **Start:** SPIKE.md §2's `systemd-run` with `--unit=mdb-tube-preview`, `--working-directory=/home/magic/tube-preview`, the binary `/home/magic/tube-preview/magic_dingus_box_cpp`, without `MDB_TUBE_SPIKE_DIR` and `MDB_REDRAW_GATE`, plus the task's `--setenv=`/`--property=` lines. Follow `journalctl -u mdb-tube-preview -f -o cat`.
6. **Drive looks** by writing the op log the kiosk polls (Plan 2), as `magic`, atomically, `seq` = the status's `handled_seq` + 1:
   ```bash
   tube_op() { # tube_op '{"op":"look","look":"tv"}'
     d=/opt/magic_dingus_box/magic_dingus_box_cpp/data
     s=$(jq '.tube.handled_seq' $d/kiosk_status.json); s=$((s+1))
     jq -n --argjson op "$1" --argjson s $s '{version:1, ops:[$op + {seq:$s}]}' > $d/tube_request.json.tmp
     mv $d/tube_request.json.tmp $d/tube_request.json; }
   ```
   Engine: `tube_op '{"op":"engine","engine":"tube"}'`; a row: `tube_op '{"op":"row","key":"glow","value":0}'`. Video: start a 960×720 H.264 playlist item with the phone remote or a pad.
7. **Restore (always, also after a failure):** `sudo systemctl stop mdb-tube-preview; sudo systemctl reset-failed mdb-tube-preview 2>/dev/null; sudo rm -f /dev/shm/mdb-tube-build; rm -f /opt/magic_dingus_box/magic_dingus_box_cpp/data/tube_request.json /opt/magic_dingus_box/magic_dingus_box_cpp/data/screenshot_request`; copy off, then delete, every screenshot not in `screenshots.before`; `cp -a ~/tube-preview/settings.json.bak /opt/magic_dingus_box/magic_dingus_box_cpp/config/settings.json`; `sleep 2; sudo systemctl start magic-dingus-box-cpp; systemctl is-active magic-dingus-box-cpp` → `active`.
8. **Report:** `systemctl show -p MainPID -p NRestarts magic-dingus-box-cpp`, the binary's `sha256sum` (must equal step 2's), and `diff ~/tube-preview/settings.json.bak /opt/.../config/settings.json` (must be empty), to the coordinator; then tell the other session the window is over. Delete `~/tube-preview` at the end of the task.

The preview build (Tasks 14–16), on the Mac: `docker run --rm --platform linux/arm64 -v "$PWD:/repo" -w /repo mdb-pisim:trixie bash -c 'cmake -S magic_dingus_box_cpp -B magic_dingus_box_cpp/build-pisim-preview -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_KIOSK=ON -DENABLE_MEDIA_BROWSER=ON -DBUILD_TESTS=OFF -DMDB_TUBE_PREVIEW=ON && cmake --build magic_dingus_box_cpp/build-pisim-preview -j6'`; copy with `ssh $PI 'mkdir -p ~/tube-preview'; scp magic_dingus_box_cpp/build-pisim-preview/magic_dingus_box_cpp $PI:tube-preview/`; `ssh $PI 'ldd ~/tube-preview/magic_dingus_box_cpp | grep "not found" || echo libs OK'`.

## Review Focus

1. **A shader compile that hangs.** One Mesa variant hung the compiler for nine minutes. Nothing is built before READY=1; the guard covers each program until its first draw in its real pass; every program is drawn through its real pass on a Pi 5; a forced hang under the watchdog leaves the next start on Enhanced. (Task 1; Task 5 `"tube programs: the validation plans draw every program"`; Task 9 `"tube build guard: a marker left by a dead kiosk trips the guard"`; Task 14 Steps 3–4.)
2. **The lean class holds 60 fps or does not exist.** Its contents and both budgets come from a measurement of the lean pass as specified, with pacing measured with the timers off; if nothing fits, every look ships Rich30. (Task 1b; Task 3 `"tube frame class: a row whose ingredient is not lean forces 30 fps"`, `"tube frame class: no usable lean set makes everything rich"`; Task 15 Step 2.)
3. **A look change while a video plays.** Moving a row across neutral swaps programs: at most the plan's new programs are built (3–35 ms each), never per frame; a glass rebake only when a glass row changes; the class switch resets the pacing monitor. (Task 5 `"tube programs: a cheap row change keeps the programs, a crossing changes them"`; Task 7 `"tube driver: the plan changes only when a row crosses neutral"`, `"tube driver: the glass rebakes only for its own rows"`; Task 15 Step 4.)
4. **A box up for weeks.** No float grows with uptime; the speed rows do not jump when tuned. (Task 2 `"tube frame: a speed step does not move the band"`, `"tube frame: accumulated phases stay in one cycle after a month of frames"`.)
5. **The Channel Guide's window and full-screen films.** One content rectangle for drawing and tube; a film is Rich30 and drops afterglow then glow when its predicted cost exceeds the rich budget. (Task 8 `"content rects: the kiosk's three layouts"`; Task 5 `"tube programs: a full-screen film drops afterglow, then glow, to fit"`; Task 15 film step.)
6. **A display-mode change (720p ↔ 1080p, Modern TV ↔ CRT Native, 50/60/120 Hz).** Targets resize, the glass rebakes, pitches recompute, CRT Native draws no inset, the hold keeps `round(hz/30)` vblanks. (Task 2 `"tube inputs: CRT Native has no inset and no corners"`; Task 7 `"tube driver: a new picture size rebuilds the still"`; Task 8 `"present hold: round(refresh / 30) vblanks after the last flip"`; Task 15 Step 4.)
7. **A GL context rebuild after a game.** `reset_gl()` releases every tube object; the scene texture is re-specified once; programs rebuild inside the guard; no warm-up replays. (Task 10 `release_gl`; Task 15 game step with a screenshot.)
8. **The Media Browser taking the screen.** The tube never draws there; phases pause; the first step back is capped at 0.1 s. (Task 2 `"tube frame: a long gap advances by at most 0.1 s"`; Task 12 gate; Task 15 MB step.)
9. **Rich30 cadence.** Every rich frame lands `round(hz/30)` vblanks after the last whether the GPU took 5 or 30 ms; the hold sits between the swap and the flip on a fresh clock; the watchdog is pinged every iteration. (Task 8 present-hold cases and `"redraw gate: rich tube motion is continuous, lean tube motion is field rate"`; Task 15 pacing runs.)
10. **A Pi 4B, and a Pi 5 under `MDB_PLATFORM_POLICY_OVERRIDE=pi4`.** Tube never offered, never built; Classic/Enhanced within the noise floor of the previous release. (Plan 2's gate tests; Task 16.)
11. **The hardware runs leave the shared box as found.** Window asked, settings backed up and diffed, created files removed, the production kiosk's binary hash unchanged. (The runbook, steps 1–8.)

---

### Task 1 (HARDWARE): Isolate the compiler hang

**Files:** Create `docs/engineering/measurements/2026-11-15-tube-hang-isolation.txt` (the journal excerpts and the finding).

**Interfaces:** Produces the finding Tasks 5, 6 and 14 consume: the define set (and, if it can be narrowed, the construct) that hangs Mesa 25.0.7's V3D compiler, or "not reproduced".

Suspect, from the round-2 journal: the hang began right after bench row 39 (`P2 everything`), when the bench built row 40, `P2 everything sharpness=50` (`tube_spike.cpp` `kBench`). With sharpness at 50 only the signal program changes, so the suspect is `v2_signal.frag` with `SIG_TONE SIG_CHROMA SIG_CONV SIG_JITTER SCENE_HW_DECODE` (no `SIG_SHARP`).

- [ ] **Step 1:** Runbook 1–2. Build and copy the spike exactly as `.worktrees/tube-spike/magic_dingus_box_cpp/spike/tube/SPIKE.md` §1 (to `~/tube-spike/`), and runbook 4 with `~/tube-spike` in place of `~/tube-preview`.
- [ ] **Step 2: Run it bounded.** SPIKE.md §2's `systemd-run`, adding `--property=RuntimeMaxSec=180 --setenv=V3D_DEBUG=shaderdb`. Write `control.json` as `{"look": "everything", "rows": {"sharpness": 50}, "pipeline": 2, "hw_srgb": true}` (one write, whole file).
- [ ] **Step 3: Read the result.** A hang shows a `program signal [...]` line with no `first draw with signal took` line after it, the process at ~100% of a core (`top -H -p $(systemctl show -p MainPID --value mdb-tube-spike)`), and `sudo gdb -p <pid> -batch -ex 'thread apply all bt 8'` (gdb is installed on the box) inside `libgallium`/`v3d`. Record the define set.
- [ ] **Step 4: Narrow it.** If it hung, re-run Step 2 once per variant, removing one define at a time through `rows` (`chromaBleed: 0`, `convergence: 0`, `jitter: 0`, `contrast: 50, saturation: 50` for tone), then with `"hw_srgb": false`. If it did not hang, run the bench's next rows the same way (`kBench` rows 41–51), one per run. Stop at the first hang or after the list.
- [ ] **Step 5:** SPIKE.md §3 restore, then runbook 7–8 (with `mdb-tube-spike`); keep `~/tube-spike` for Task 1b.
- [ ] **Step 6:** Write the finding and commit — `docs: Tube hang isolation on the Pi 5`. If not reproduced: record "not reproduced in N runs"; Task 14 is then the gate.

---

### Task 1b (HARDWARE, with a worker step): Price the lean pass and measure both budgets

**Files:** Spike branch only: `.worktrees/tube-spike/magic_dingus_box_cpp/spike/tube/v2_output.frag`, `.../src/ui/tube_spike.cpp`, `.../src/ui/renderer_crt.cpp` (committed on `spike/tube-phase0`, never merged). This branch: create `docs/engineering/measurements/2026-11-xx-tube-lean-pricing.txt` (the table and the DECISION block).

**Interfaces:** Produces the DECISION block Tasks 3, 4, 5 and 15 consume:
```
DECISION (Task 1b)
lean.usable: yes|no
lean.bend: yes|no   lean.beam: yes|no   lean.cheap_beam: yes|no
lean.tone: yes|no   lean.displace: yes|no   lean.motion: yes|no   lean.probe: yes|no
lean_budget_ms: <x.x>   rich_budget_ms: <y.y>
pvm_lean: yes|no
pass_costs_ms: quarter <> glow_blur <> sig_sharp <> sig_sharp_chroma <> sig_guns <> sig_guns_chroma <> lines_half <> lines_full <> out_lean <> out_rich <> out_rich_raster <> out_rich_raster_only <> history <> probe
enhanced_composite_ms: <z.z>
```

- [ ] **Step 1 (worker, on the Mac, spike branch):** add to the spike, measurement only:
  - `v2_output.frag`: an `OUT_SCENE` source — read the scene with `uiTex` (hardware decode), apply tone (`OUT_TONE`: contrast's `pow`, saturation) and the three displacements (`OUT_DISPLACE`: jitter, tracking, flagging, the signal shader's code moved here) before the beam, and write encoded colour to the screen; `OUT_MOTION` gating noise, flicker, hum, the band and purity together.
  - `control.json` key `"lean": {"bend": bool, "beam": "exact"|"cheap"|"off", "tone": bool, "displace": bool, "motion": bool, "probe": bool}` — when present, pipeline 2 draws only this lean output (mask texture, baked glass, the chosen ingredients' defines) and nothing else.
  - The mean probe: every 12th frame the scene into a 64×64 `SRGB8_ALPHA8`, `glGenerateMipmap`, `glReadPixels` of the 1×1 level into a `GL_PIXEL_PACK_BUFFER`, `glFenceSync`, polled with `glClientWaitSync(…, 0, 0)`; timed as its own pass `probe`.
  - `"gpu_timers": false` creates no query objects at all (pacing runs).
  - `"engine": "enhanced"`: the spike's `composite` returns false so the renderer's own Enhanced composite draws, wrapped (timers on) in one `GL_TIME_ELAPSED` query reported as `enhanced`.
  - Verify: `bash check_shaders.sh` (add the new define combinations) and the SPIKE.md §1 pisim build. Commit on the spike branch — `spike: lean-pass pricing (OUT_SCENE, probe, timers off, Enhanced baseline)`.
- [ ] **Step 2: GPU prices (timers on).** Runbook 1–2, 4; copy the spike (SPIKE.md §1); SPIKE.md §2 with `--property=RuntimeMaxSec=14400`. Video playing, Modern TV 1080p, picture 1440×1080. For each configuration, 60 s, the mean of the six `tube spike gpu` lines' totals and per pass:
  - Base `B` = `{"lean": {"bend": false, "beam": "off", "tone": false, "displace": false, "motion": false, "probe": false}}` with PVM's mask and glass rows.
  - `B` + one ingredient each: `bend`; `beam exact`; `beam cheap`; `tone`; `displace` (jitter 30, tracking 30, flagging 30); `motion` (noise 30, hum 30, flicker 30); `probe` (abl 20).
  - `FULL exact` and `FULL cheap` (every ingredient); `PVM-lean` (PVM's rows with glow 0, afterglow 0, sharpness 50: bend, beam, tone, probe) with each beam; `Monitor-lean` likewise.
  - Rich references (pipeline 2 as round 2): `pvm`, `monitor`, `tv` half + cheap without afterglow and glow, `tv` half + cheap, `trinitron` half — their per-pass costs fill `pass_costs_ms`.
  - `{"engine": "enhanced"}` with the box's current Enhanced sliders — the baseline.
- [ ] **Step 3: Pacing (timers off, a separate run).** Restart the spike with `"gpu_timers": false`. 10 minutes each, video playing, from the `tube spike pacing` lines (`late` = interval over 1.5 refreshes): `FULL cheap`, `PVM-lean` (the cheaper beam that its price allows), the candidate lean set (Step 4), and for the rich budget the rich references from cheapest to dearest at the 30 fps cadence (late = an interval over 41.7 ms).
- [ ] **Step 4: Decide, by these rules, and write the DECISION block.**
  - `lean_budget_ms` = the largest timers-on total among the lean configurations whose timers-off run held ≥ 99% on their vblank, minus 0.5. `rich_budget_ms` = the largest timers-on total among the rich references that held ≥ 99% at 33.3 ms, minus 1.0. If no lean configuration held, `lean.usable: no`.
  - The lean set: start from every ingredient; `cheap_beam` yes unless the exact beam's `FULL` total already fits; while `B` + the chosen ingredients' measured increments exceed `lean_budget_ms`, drop the ingredient with the largest increment (never the base). The resulting set must itself have held ≥ 99% in Step 3 (re-run it if it was not among them); if even `B` misses, `lean.usable: no`.
  - `pvm_lean: yes` iff PVM-lean's ingredients are all in the lean set and its own pacing run held ≥ 99%.
- [ ] **Step 5:** SPIKE.md §3 restore; runbook 7–8; `rm -rf ~/tube-spike`. Commit the pricing file — `docs: Tube lean pass priced on the Pi 5; the lean set and both budgets`.

**If `lean.usable: no`:** Tasks 3–5 encode it (every look Rich30, the Lean60 path kept for any custom mix that would qualify — none can then), Task 4 changes no look, Task 13's tags still mark every row, and the coordinator tells the owner that Tube ships at 30 fps for every look. Nothing else stalls.

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
  - `inline void advance_character_phases(CharacterPhases&, const Resolved&, double dt) noexcept;` — `dt` clamped to `[0, kMaxPhaseStepSeconds]`; each phase `+= dt / its period`, wrapped with `x - floor(x)`.
  - `tube_character_frame(TubeUniforms&, const Resolved&, const CharacterPhases&, double tracking_elapsed, bool enabled)` replaces the `double seconds` form: `rollPosition = phases.roll`; drift `sin(kTwoPi * phases.drift)`; flagging's phase `kTwoPi * phases.flag`. Every other line unchanged.
  - `FrameState` gains `CharacterPhases phases{};`; `tube_frame` passes `state.phases`. `seconds` still drives hum, flicker, degauss and signal motion.
  - The header's "What differs" comment gains: the three speed rows' phases are accumulated, so a speed step changes only the rate.

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
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — all pass; then `python3 magic_dingus_box_cpp/scripts/dev/check_tube_parity.py` when `../mdb-sound-factory` is present.
- [ ] **Step 5: Commit** — `tube: CRT Native draws no inset; the speed rows' phases are accumulated`

---

### Task 3: The lean set, the budgets and the frame class

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_budget.h`, `magic_dingus_box_cpp/src/tube/tube_frame_class.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_frame_class.cpp`

**Interfaces:**
- Consumes: Task 1b's DECISION block (or the provisional values); `tube::Effect`, `kEffects`, `index_of`, `Resolved`, `Picture`, `neutral_in_crt_native`.
- Produces in `tube_budget.h` (namespace `tube`; every constant carries `// from Task 1b's table` — `(provisional)` until it lands):
  - `struct LeanSet { bool usable, bend, beam, tone, displace, motion, probe, cheap_beam; };` `inline constexpr LeanSet kLeanSet{true, true, true, true, true, true, true, true};`
  - `inline constexpr double kLeanBudgetMs = 8.0, kRichBudgetMs = 24.0;` `inline constexpr bool kRichBeamCheap = false;` (the acceptance ladder may flip it)
  - `struct PassCosts { double quarter, glow_blur, sig_sharp, sig_sharp_chroma, sig_guns, sig_guns_chroma, half_width_factor, lines_half, lines_full, out_lean, out_rich, out_rich_raster, out_rich_raster_only, history, probe; };` `inline constexpr PassCosts kPassCosts{0.7, 4.0, 4.4, 7.1, 7.1, 10.0, 0.56, 1.2, 2.0, 9.3, 9.1, 11.6, 10.1, 4.5, 0.3};` — ms at 1440×1080.
  - `inline constexpr std::array<Effect, 7> kAlwaysRich{Effect::glow, Effect::afterglow, Effect::sharpness, Effect::convergence, Effect::chromaBleed, Effect::raster, Effect::colorDrift};` (one per line; Task 13's Python check reads it as text).
- Produces in `tube_frame_class.h`:
  - `enum class FrameClass { Lean60, Rich30 };` `constexpr int frames_per_second(FrameClass) noexcept;`
  - `inline constexpr double kReferencePicturePixels = 1440.0 * 1080.0;`
  - `constexpr int rate_neutral(Effect) noexcept;` — 50 for `sharpness`, `contrast`, `saturation`; else 0.
  - `constexpr bool forces_30fps(Effect, const LeanSet& lean = kLeanSet) noexcept;` — always-rich rows; `curvature` unless `lean.bend`; `scanlines` unless `lean.beam`; `contrast`, `saturation` unless `lean.tone`; `jitter`, `tracking`, `flagging` unless `lean.displace`; `noise`, `flicker`, `hum`, `rollBand`, `purity` unless `lean.motion`; `abl`, `breathing` unless `lean.probe`; every row when `!lean.usable`.
  - `FrameClass setting_frame_class(const Resolved&, bool crt_native, const LeanSet& lean = kLeanSet) noexcept;` — Rich30 when `!lean.usable` or any `forces_30fps` row differs from `rate_neutral` (a row `neutral_in_crt_native` counts as neutral when `crt_native`).
  - `FrameClass frame_class(const Resolved&, const Picture&, const LeanSet& lean = kLeanSet) noexcept;` — also Rich30 when `picture.w * picture.h > kReferencePicturePixels`.

- [ ] **Step 1: Write the failing test**

```cpp
using namespace tube;
Resolved neutral_rates() {                          // every row that can force 30 fps at neutral
    Resolved r = resolve(TubeSetting{});
    for (auto e : kEffects) if (forces_30fps(e, LeanSet{true, false, false, false, false, false, false, false}))
        r.effects[index_of(e)] = rate_neutral(e);
    return r;
}
constexpr LeanSet kAllLean{true, true, true, true, true, true, true, true};
TEST_CASE("tube frame class: with every ingredient lean, the seven always-rich rows force 30 fps", "[tube]") {
    int n = 0; for (auto e : kEffects) n += forces_30fps(e, kAllLean);
    REQUIRE(n == 7);
    for (auto e : kAlwaysRich) REQUIRE(forces_30fps(e, kAllLean));
    REQUIRE_FALSE(forces_30fps(Effect::glowSize, kAllLean)); REQUIRE_FALSE(forces_30fps(Effect::brightness, kAllLean));
    for (auto e : kAlwaysRich) {
        Resolved r = neutral_rates(); r.effects[index_of(e)] = (e == Effect::sharpness) ? 55 : 5;
        INFO(effect_key(e)); REQUIRE(setting_frame_class(r, false, kAllLean) == FrameClass::Rich30);
    }
    REQUIRE(setting_frame_class(neutral_rates(), false, kAllLean) == FrameClass::Lean60);
}
TEST_CASE("tube frame class: a row whose ingredient is not lean forces 30 fps", "[tube]") {
    LeanSet no_bend = kAllLean; no_bend.bend = false;
    REQUIRE(forces_30fps(Effect::curvature, no_bend)); REQUIRE_FALSE(forces_30fps(Effect::curvature, kAllLean));
    Resolved r = neutral_rates(); r.effects[index_of(Effect::curvature)] = 70;
    REQUIRE(setting_frame_class(r, false, kAllLean) == FrameClass::Lean60);
    REQUIRE(setting_frame_class(r, false, no_bend) == FrameClass::Rich30);
    LeanSet no_motion = kAllLean; no_motion.motion = false;
    for (auto e : {Effect::noise, Effect::flicker, Effect::hum, Effect::rollBand, Effect::purity})
        REQUIRE(forces_30fps(e, no_motion));
}
TEST_CASE("tube frame class: no usable lean set makes everything rich", "[tube]") {
    LeanSet none = kAllLean; none.usable = false;
    REQUIRE(setting_frame_class(neutral_rates(), false, none) == FrameClass::Rich30);
}
TEST_CASE("tube frame class: CRT Native's neutral rows do not force 30 fps", "[tube]") {
    Resolved r = neutral_rates(); r.effects[index_of(Effect::raster)] = 60;
    REQUIRE(setting_frame_class(r, false, kAllLean) == FrameClass::Rich30);
    REQUIRE(setting_frame_class(r, true, kAllLean) == FrameClass::Lean60);
}
TEST_CASE("tube frame class: a picture larger than the reference is rich", "[tube]") {
    const Resolved r = neutral_rates();
    REQUIRE(frame_class(r, Picture{1440.0, 1080.0, false}, kAllLean) == FrameClass::Lean60);
    REQUIRE(frame_class(r, Picture{960.0, 720.0, false}, kAllLean) == FrameClass::Lean60);
    REQUIRE(frame_class(r, Picture{1920.0, 1080.0, false}, kAllLean) == FrameClass::Rich30);
    REQUIRE(frames_per_second(FrameClass::Rich30) == 30);
}
TEST_CASE("tube budget: the constants are Task 1b's", "[tube]") {
    // Pinned to the DECISION block; update with it, in the same commit.
    REQUIRE(kLeanBudgetMs == 8.0); REQUIRE(kRichBudgetMs == 24.0);   // provisional
    REQUIRE(kLeanSet.usable); REQUIRE(kLeanSet.cheap_beam);          // provisional
}
```
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** both headers with the DECISION block's values if Task 1b has landed, else the provisional ones (header comment: design §3 "Frame class", the rule that filled them).
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: the lean set, the budgets and the frame class (from the lean pricing)`

---

### Task 4: The shipped looks, and what differs from Sound Factory

**Files:**
- Modify: `magic_dingus_box_cpp/src/tube/tube_looks.h` (header comment always; the pvm and monitor rows only on the "lean" branch), `magic_dingus_box_cpp/scripts/dev/check_tube_parity.py` (`BOX_RETUNED`), `magic_dingus_box/web/static/tube_rows.json` (regenerated when a look changes), `docs/superpowers/specs/2026-11-15-tube-crt-engine-design.md` (the "Rows that differ from Sound Factory's" table: drop the "*if lean*" marks for the branch taken)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_looks.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_inputs.cpp`

**Interfaces:**
- Consumes: Task 1b's `pvm_lean` (provisional: **no**), Task 3.
- Produces, by branch:
  - **`pvm_lean: yes`:** in PVM and Monitor every row `forces_30fps(e)` goes to `rate_neutral(e)` — at least PVM `sharpness 63 → 50, glow 8 → 0, afterglow 10 → 0`, Monitor `sharpness 60 → 50, glow 5 → 0`, plus any row Task 1b's lean set excluded (e.g. curvature if `lean.bend: no`). `BOX_RETUNED` lists every changed `(look, key)` cell; the Sound-Factory-ported cases in `test_tube_inputs.cpp` that read PVM's sharpness 1.26, glow 0.08, persistence 0.10 set those rows to 63, 8, 10 first.
  - **`pvm_lean: no`** (also when `lean.usable: no`): no look changes; `BOX_RETUNED` stays empty; the header comment says why.
  - Either way the header comment and the design table list exactly the cells that differ from Sound Factory's.

- [ ] **Step 1: Write the failing test** (the case for the branch taken; `kPinned` updated on the lean branch)

```cpp
TEST_CASE("tube looks: the classes Task 1b decided", "[tube]") {
    constexpr bool kPvmLean = false;   // from Task 1b's table (provisional): pvm_lean
    for (auto look : kLooks) {
        TubeSetting s; select_look(s, look);
        const bool lean = kPvmLean && (look == Look::pvm || look == Look::monitor);
        INFO(look_name(look));
        REQUIRE((setting_frame_class(resolve(s), false) == FrameClass::Lean60) == lean);
    }
    const auto& pvm = look_values(Look::pvm).effects;
    REQUIRE(pvm[index_of(Effect::sharpness)] == (kPvmLean ? 50 : 63));
    REQUIRE(pvm[index_of(Effect::glow)] == (kPvmLean ? 0 : 8));
    REQUIRE(pvm[index_of(Effect::abl)] == 20);
}
```
- [ ] **Step 2: Run** — on the lean branch it fails (PVM still rich); on the "no" branch it passes at once and pins the decision.
- [ ] **Step 3: Implement** the branch; regenerate the row file on the lean branch (`MDB_WRITE_TUBE_ROWS_JSON=1 ./build-tube3/test_app_unit "[tube_rows_json]"`).
- [ ] **Step 4: Run** `./build-tube3/test_app_unit` and `test_ui_unit` in full, `python -m pytest magic_dingus_box/web/tests/test_tube_rows_json.py -q`, and `check_tube_parity.py` when the sibling checkout exists (each changed cell reported as deliberate).
- [ ] **Step 5: Commit** — lean branch `tube: PVM and Monitor ship lean (60 fps), as measured`; other branch `tube: the six looks stay Sound Factory's (30 fps), as measured`.

---

### Task 5: The program table, the pass plan and the cost prediction

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_programs.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_programs.cpp`

**Interfaces:**
- Consumes: Tasks 1, 1b, 3; `TubeUniforms`, `tube_uniforms_for`, `glow_sigma_px`.
- Produces (namespace `tube`):
  - `enum class Program : std::uint8_t { glass, quarter, blur, copy_encode, sig_sharp, sig_sharp_chroma, sig_guns, sig_guns_chroma, lines, out_lean_still, out_lean_moving, out_rich_scene, out_rich_scene_ag, out_rich_signal, out_rich_signal_ag, out_rich_raster, out_rich_raster_ag, out_rich_raster_only, out_rich_raster_only_ag };` `inline constexpr std::size_t kProgramCount = 19;`
  - `enum class ShaderFile : std::uint8_t { glass, quarter, blur, copy_encode, signal, lines, output };`
  - Capability bits `kCapTone = 1u << 0, kCapDisplace = 1u << 1, kCapSharp = 1u << 2, kCapGuns = 1u << 3, kCapChroma = 1u << 4, kCapSceneSource = 1u << 5, kCapSignalSource = 1u << 6, kCapRaster = 1u << 7, kCapRasterOnly = 1u << 8, kCapGlow = 1u << 9, kCapHistory = 1u << 10, kCapMotion = 1u << 11, kCapAbl = 1u << 12, kCapBend = 1u << 13, kCapBeam = 1u << 14`.
  - `struct ProgramInfo { Program id; std::string_view name; ShaderFile file; std::string_view defines; bool reads_scene; std::uint32_t caps; };` `inline constexpr std::array<ProgramInfo, kProgramCount> kPrograms` — the table below; for the two lean outputs `defines`/`caps` hold only the fixed part.
  - `std::string program_defines(Program);` and `std::uint32_t program_caps(Program);` — the table's, plus for `out_lean_*` the lean set's ingredients: `OUT_BEND`/Bend if `kLeanSet.bend`, `OUT_SCANLINES`/Beam if `.beam`, `OUT_TONE`/Tone if `.tone`, `OUT_ABL`/Abl if `.probe`, and for `out_lean_moving` `OUT_DISPLACE`/Displace if `.displace`, `OUT_MOTION`/Motion if `.motion`; `OUT_BEAM_CHEAP` when `.cheap_beam` (lean) or `kRichBeamCheap` (rich outputs).
  - `struct Needs { bool tone, displace, sharp, guns, chroma, raster, raster_only, glow, afterglow, motion, abl, bend, beam; };` (all default false) `Needs needs_of(const TubeUniforms& still, const Resolved&) noexcept;` — as before, plus `bend` = curvature > 0, `beam` = scanlines > 0. Read from the STILL, never per frame.
  - `struct PassPlan { FrameClass cls = FrameClass::Lean60; bool mean_probe = false, quarter = false, glow = false; std::optional<Program> signal; bool signal_half = false; bool lines = false; Program output = Program::out_lean_still; bool afterglow = false; bool operator==(const PassPlan&) const; };`
  - `PassPlan plan_for(const Needs&, bool larger_than_reference, const LeanSet& lean = kLeanSet) noexcept;` — Lean60 iff `lean.usable`, no rich need (`sharp guns chroma raster glow afterglow`), not larger, and every need's ingredient is in `lean` (`bend`, `beam`, `tone`, `displace`, `motion`, `abl`↔`probe`); the rest as before (signal iff `sharp || guns || chroma || raster`; program by `guns` × `chroma`; `signal_half = chroma || raster`; `lines = raster`; `quarter = glow || chroma`; output by source, `_ag` iff afterglow; lean: `out_lean_moving` when `motion || displace`; `mean_probe = abl`).
  - `struct TargetSizes { int pic_w, pic_h, quarter_w, quarter_h, signal_w, signal_h, lines_w, lines_h; static constexpr int kMeanSize = 64; };` `TargetSizes target_sizes(const PassPlan&, const Picture&, const TubeUniforms& still) noexcept;`
  - `std::size_t target_bytes(const PassPlan&, const TargetSizes&, bool half_float) noexcept;` — glass `w·h·4`; history `2·w·h·4` (a ping-pong pair) when `afterglow`; signal and lines `·8` (`half_float`) or `·4`; quarter `·4` and two glow targets `·4` when `glow`; mean `64·64·4·4/3`.
  - `double predicted_ms(const PassPlan&, const TargetSizes&, const PassCosts& c = kPassCosts) noexcept;` — the passes' `kPassCosts` (signal × `half_width_factor` when half; lines half/full) scaled by `pic_w·pic_h / kReferencePicturePixels`.
  - `PassPlan plan_passes(const Resolved&, const Picture&) noexcept;` — `plan_for(needs_of(still, r), larger)`; **when larger than the reference** and `predicted_ms > kRichBudgetMs`, re-plan without afterglow, then (if still over) without glow; stop there (design §3).
  - `std::vector<Program> programs_of(const PassPlan&);` — `glass` first, no duplicates.
  - `std::vector<PassPlan> validation_plans();` — a fixed list of plans that together draw every program at least once (Task 14 draws each through its real passes).
  - `std::vector<std::string_view> uniforms_of(Program);` — the `TubeUniforms` fields and own uniforms the program declares (Task 6 tests it against the sources).

| Program | File | Fixed defines | Reads scene | Fixed caps |
|---|---|---|---|---|
| `glass` | glass | — | no | — |
| `quarter` | quarter | — | yes | — |
| `blur` | blur | — | no | — |
| `copy_encode` | copy_encode | — | no | — |
| `sig_sharp` | signal | `SIG_SHARP` | yes | Tone Displace Sharp |
| `sig_sharp_chroma` | signal | `SIG_SHARP SIG_CHROMA` | yes | + Chroma |
| `sig_guns` | signal | `SIG_SHARP SIG_GUNS` | yes | Tone Displace Sharp Guns |
| `sig_guns_chroma` | signal | `SIG_SHARP SIG_GUNS SIG_CHROMA` | yes | + Chroma |
| `lines` | lines | — | no | Raster |
| `out_lean_still` | output | `OUT_SCENE` (+ lean ingredients) | yes | SceneSource (+ …) |
| `out_lean_moving` | output | `OUT_SCENE` (+ lean ingredients, displace, motion) | yes | SceneSource (+ …) |
| `out_rich_scene` | output | `OUT_SCENE OUT_BEND OUT_SCANLINES OUT_TONE OUT_DISPLACE OUT_MOTION OUT_ABL OUT_GLOW` | yes | SceneSource Bend Beam Tone Displace Motion Abl Glow |
| `out_rich_scene_ag` | output | `… OUT_HISTORY` | yes | + History |
| `out_rich_signal` | output | `OUT_SIGNAL OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW` | no | SignalSource Bend Beam Motion Abl Glow |
| `out_rich_signal_ag` | output | `… OUT_HISTORY` | no | + History |
| `out_rich_raster` | output | `OUT_SIGNAL OUT_RASTER OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW` | no | + Raster |
| `out_rich_raster_ag` | output | `… OUT_HISTORY` | no | + History |
| `out_rich_raster_only` | output | `OUT_RASTER OUT_RASTER_ONLY OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW` | no | Raster RasterOnly Bend Beam Motion Abl Glow |
| `out_rich_raster_only_ag` | output | `… OUT_HISTORY` | no | + History |

If Task 1 named a construct, a comment at the table points at the finding and the program that avoids it.

- [ ] **Step 1: Write the failing test**

```cpp
using namespace tube;
constexpr unsigned kNeedBits = 13;
Needs needs_from_bits(unsigned b) {
    Needs n; bool* f[] = {&n.tone, &n.displace, &n.sharp, &n.guns, &n.chroma, &n.raster, &n.raster_only,
                          &n.glow, &n.afterglow, &n.motion, &n.abl, &n.bend, &n.beam};
    for (unsigned i = 0; i < kNeedBits; ++i) *f[i] = (b >> i) & 1u;
    if (n.raster_only) n.raster = true;
    return n;
}
bool lean_has(const Needs& n, const LeanSet& l) {
    return (!n.bend || l.bend) && (!n.beam || l.beam) && (!n.tone || l.tone) && (!n.displace || l.displace) &&
           (!n.motion || l.motion) && (!n.abl || l.probe);
}
TEST_CASE("tube programs: nineteen, named once, in enum order", "[tube]") {
    REQUIRE(kPrograms.size() == 19);
    for (std::size_t i = 0; i < kPrograms.size(); ++i) REQUIRE(static_cast<std::size_t>(kPrograms[i].id) == i);
    std::set<std::string_view> names; for (auto& p : kPrograms) names.insert(p.name);
    REQUIRE(names.size() == 19);
}
TEST_CASE("tube programs: every reachable setting maps to a program with what it needs", "[tube]") {
    for (const LeanSet lean : {kLeanSet, LeanSet{true, false, true, true, false, true, false, true}, LeanSet{false, false, false, false, false, false, false, false}})
    for (unsigned b = 0; b < (1u << kNeedBits); ++b) for (bool big : {false, true}) {
        const Needs n = needs_from_bits(b); const PassPlan p = plan_for(n, big, lean); INFO(b << " big " << big);
        const bool rich_need = n.sharp || n.guns || n.chroma || n.raster || n.glow || n.afterglow;
        const bool rich = rich_need || big || !lean.usable || !lean_has(n, lean);
        REQUIRE((p.cls == FrameClass::Rich30) == rich);
        const std::uint32_t oc = program_caps(p.output);
        if (!rich) {
            REQUIRE(p.output == ((n.motion || n.displace) ? Program::out_lean_moving : Program::out_lean_still));
            REQUIRE_FALSE(p.signal.has_value()); REQUIRE_FALSE(p.glow); REQUIRE_FALSE(p.afterglow);
            REQUIRE(p.mean_probe == n.abl);
            continue;
        }
        REQUIRE(p.signal.has_value() == (n.sharp || n.guns || n.chroma || n.raster));
        if (p.signal) {
            const std::uint32_t sc = program_caps(*p.signal);
            REQUIRE((sc & kCapTone)); REQUIRE((sc & kCapDisplace));
            if (n.guns) REQUIRE((sc & kCapGuns)); if (n.chroma) REQUIRE((sc & kCapChroma));
            REQUIRE((oc & kCapSceneSource) == 0); REQUIRE(p.signal_half == (n.chroma || n.raster));
        } else {
            REQUIRE((oc & kCapSceneSource)); REQUIRE((oc & kCapTone)); REQUIRE((oc & kCapDisplace));
        }
        REQUIRE((oc & kCapBend)); REQUIRE((oc & kCapBeam)); REQUIRE((oc & kCapMotion)); REQUIRE((oc & kCapAbl));
        REQUIRE(p.lines == n.raster); REQUIRE(((oc & kCapRasterOnly) != 0) == n.raster_only);
        if (!big) { REQUIRE(p.glow == n.glow); REQUIRE(p.afterglow == n.afterglow); }
        if (p.glow) REQUIRE((oc & kCapGlow));
        REQUIRE(((oc & kCapHistory) != 0) == p.afterglow);
        REQUIRE(p.mean_probe == n.abl);
    }
}
TEST_CASE("tube programs: a lean output compiles exactly the lean set", "[tube]") {
    const std::uint32_t still = program_caps(Program::out_lean_still);
    REQUIRE(((still & kCapBend) != 0) == kLeanSet.bend); REQUIRE(((still & kCapTone) != 0) == kLeanSet.tone);
    REQUIRE((still & kCapMotion) == 0);
    REQUIRE((program_defines(Program::out_lean_still).find("OUT_BEAM_CHEAP") != std::string::npos) == kLeanSet.cheap_beam);
}
TEST_CASE("tube programs: the shipped looks", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    auto plan = [&](Look l) { TubeSetting s; select_look(s, l); return plan_passes(resolve(s), pic); };
    for (auto look : kLooks) { TubeSetting s; select_look(s, look);   // the two pure deciders agree
        REQUIRE(plan(look).cls == frame_class(resolve(s), pic)); }
    const PassPlan tv = plan(Look::tv);
    REQUIRE(tv.signal == Program::sig_guns_chroma); REQUIRE(tv.signal_half); REQUIRE(tv.lines);
    REQUIRE(tv.output == Program::out_rich_raster_ag);
    REQUIRE(plan(Look::arcade).output == Program::out_rich_raster_only_ag);   // raster 100
}
TEST_CASE("tube programs: a cheap row change keeps the programs, a crossing changes them", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    TubeSetting s; for (auto e : kAlwaysRich) set_row(s, e, rate_neutral(e));   // a lean-able PVM
    const PassPlan a = plan_passes(resolve(s), pic);
    set_row(s, Effect::brightness, 80); set_row(s, Effect::maskStrength, 90);
    REQUIRE(plan_passes(resolve(s), pic) == a);
    set_row(s, Effect::glow, 5);
    REQUIRE(plan_passes(resolve(s), pic).cls == FrameClass::Rich30);
}
TEST_CASE("tube programs: a full-screen film drops afterglow, then glow, to fit", "[tube]") {
    const Picture film{1920.0, 1080.0, false};
    for (Look look : kLooks) {
        TubeSetting s; select_look(s, look); const Resolved r = resolve(s);
        const PassPlan full = plan_for(needs_of(tube_uniforms_for(r, film), r), true);
        const PassPlan p = plan_passes(r, film);
        const TargetSizes t = target_sizes(p, film, tube_uniforms_for(r, film));
        INFO(look_name(look));
        REQUIRE(p.cls == FrameClass::Rich30);
        if (predicted_ms(full, target_sizes(full, film, tube_uniforms_for(r, film))) <= kRichBudgetMs) REQUIRE(p == full);
        if (full.glow && !p.glow) REQUIRE_FALSE(p.afterglow);                      // glow goes only after afterglow
        if (full.afterglow && !p.afterglow && p.glow) REQUIRE(predicted_ms(p, t) <= kRichBudgetMs);
    }
    TubeSetting s; select_look(s, Look::tv);
    REQUIRE(plan_passes(resolve(s), Picture{1440.0, 1080.0, false}).afterglow);     // the 4:3 picture keeps it
}
TEST_CASE("tube programs: the validation plans draw every program", "[tube]") {
    std::set<Program> drawn;
    for (const PassPlan& p : validation_plans()) for (Program g : programs_of(p)) drawn.insert(g);
    if (!(kLeanSet.motion || kLeanSet.displace)) drawn.insert(Program::out_lean_moving);  // unreachable, still validated by name
    REQUIRE(drawn.size() == kProgramCount);
}
TEST_CASE("tube programs: target sizes and memory", "[tube]") {
    TubeSetting s; select_look(s, Look::tv);
    const Picture pic{1440.0, 1080.0, false}; const Resolved r = resolve(s);
    const PassPlan p = plan_passes(r, pic); const TargetSizes t = target_sizes(p, pic, tube_uniforms_for(r, pic));
    REQUIRE(t.quarter_w == 360); REQUIRE(t.quarter_h == 270);
    REQUIRE(t.signal_w == 720);  REQUIRE(t.signal_h == 1080);
    REQUIRE(t.lines_w == 720);   REQUIRE(t.lines_h == 270);
    for (unsigned b = 0; b < (1u << kNeedBits); ++b) {
        const PassPlan q = plan_for(needs_from_bits(b), false);
        const TargetSizes ts{1440, 1080, 360, 270, q.signal_half ? 720 : 1440, 1080, q.signal_half ? 720 : 1440, 270};
        REQUIRE(target_bytes(q, ts, true) < 32u * 1024u * 1024u);
    }
}
```
(The validation case's `out_lean_moving` line keeps the program in Task 14's list even if the lean set makes it unreachable: `validation_plans()` then includes a plan naming it explicitly.)
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** `tube_programs.h`.
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: the fixed program table, the pass plan and the cost prediction`

---

### Task 6: The shaders — files, embedding, assembly and CI validation

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/shaders/common.glsl`, `fullscreen.vert`, `glass.frag`, `quarter.frag`, `blur.frag`, `copy_encode.frag`, `signal.frag`, `lines.frag`, `output.frag`
- Create: `magic_dingus_box_cpp/cmake/EmbedTubeShaders.cmake`; `magic_dingus_box_cpp/src/tube/tube_shader_assembly.h`; `magic_dingus_box_cpp/scripts/dev/check_tube_shaders.sh`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (the embed at configure time, each `.glsl` a `CMAKE_CONFIGURE_DEPENDS`; `${CMAKE_BINARY_DIR}/generated` on the kiosk's and `test_app_unit`'s include paths), `.github/workflows/test-local.yml` (job `tube-shaders`; a `kiosk-build` assertion), `.github/workflows/release.yml` (the same assertion next to the existing ones, in their form: `strings magic_dingus_box_cpp | grep -q …`)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_shaders.cpp`

**Interfaces:**
- Consumes: Task 5 (`kPrograms`, `program_defines`, `uniforms_of`), Task 1's finding.
- Produces:
  - The generated header `tube/tube_shader_sources.h` (namespace `tube::glsl`): one `inline constexpr std::string_view` per file (`common`, `fullscreen_vert`, `glass`, `quarter`, `blur`, `copy_encode`, `signal`, `lines`, `output`) as raw strings with the delimiter `tube_glsl` (the embed fails the configure if a file contains `)tube_glsl"`). Why generated, not hand-written `R"(...)"` as `renderer_shaders.cpp` does: CI must compile the very sources the kiosk embeds, as files.
  - **The binary's marker** is a comment in `common.glsl` — `// TUBE_GLSL_TABLE_V1` — so it is in every embedded string the kiosk links, with nothing else referencing it.
  - `tube_shader_assembly.h` (namespace `tube`): `enum class SceneDecode { Hardware, Shader };` `std::string assemble_fragment(Program, SceneDecode);` — `"#version 300 es\n"`, `program_defines(p)` (`#define NAME 1` each), `#define SCENE_HW_DECODE 1` when `Hardware` and the program reads the scene, `"#line 1 1\n"`, `common`, `"\n#line 1 2\n"`, the file. `std::string assemble_vertex();`
  - The GLSL, ported from the spike files with `crt.metal` the authority for every coefficient and cap:
    - `common.glsl` ← spike `common.glsl`: exact sRGB `decodeGamma`/`encodeGamma` only, `uiTex` with `SCENE_HW_DECODE`, `sceneUv`, `flipV`, and the marker comment.
    - `signal.frag` ← `v2_signal.frag`: tone and the three displacements always compiled and switched by uniform tests; `SIG_SHARP` (switched inside by `sharpness != 1.0`), `SIG_GUNS` (convergence + drift, three reads), `SIG_CHROMA` (six quarter taps + YIQ).
    - `lines.frag` ← `v2_lines.frag`.
    - `output.frag` ← `v2_output.frag` and Task 1b's spike `OUT_SCENE`: `OUT_SCENE` reads the scene, `OUT_TONE` and `OUT_DISPLACE` apply tone and displacements before the beam; `OUT_SIGNAL` reads one signal tap; `OUT_RASTER`/`OUT_RASTER_ONLY` as the spike; `OUT_BEND` the per-pixel bend (else the flat fit); `OUT_SCANLINES` the beam (`OUT_BEAM_CHEAP` its cheap form); the mask always from the texture; rolloff, tint and the power block (warm-up, degauss, spot) switched by uniform tests in every output; `OUT_ABL` ABL/breathing; `OUT_MOTION` noise, flicker, hum, the band, purity, each behind its uniform test; `OUT_GLOW` the glow tap; `OUT_HISTORY` writes linear light into the history and reads the previous one (else encodes); the glass texel's RGB divided by 32.
    - `glass.frag` ← `v2_glass.frag`, RGB written ×32 into `RGBA8`; `quarter.frag` ← `downsample.frag`; `blur.frag` ← `blur.frag`; `copy_encode.frag` ← `copy.frag` with `ENCODE_OUTPUT` always.
    - Nothing may reproduce the construct Task 1 named; a comment says so where the nearest code is.
  - `scripts/dev/check_tube_shaders.sh <dir>`: `glslangValidator -S frag` on every `*.frag`, `-S vert` on `fullscreen.vert`; prints `N checked, M failed`; exits 1 on any failure or when `N != 29` (19 hardware-decode programs + the 9 that read the scene with shader decode + the vertex shader).
  - `test-local.yml` job `tube-shaders` (ubuntu-latest): `apt-get install -y build-essential cmake pkg-config libjsoncpp-dev libyaml-cpp-dev glslang-tools`; configure `-DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF`; build `test_app_unit`; `MDB_WRITE_TUBE_GLSL=$RUNNER_TEMP/glsl ./build/test_app_unit "[tube_shaders]"`; `bash magic_dingus_box_cpp/scripts/dev/check_tube_shaders.sh $RUNNER_TEMP/glsl`. `kiosk-build`: `strings "$B" | grep -q "TUBE_GLSL_TABLE_V1" || { echo "FATAL: tube shaders not embedded"; exit 1; }`; `release.yml`: `strings magic_dingus_box_cpp | grep -q "TUBE_GLSL_TABLE_V1" || { echo "FATAL: tube shaders not embedded"; exit 1; }`.

- [ ] **Step 1: Write the failing test** (`test_tube_shaders.cpp`, tags `[tube][tube_shaders]`)

```cpp
using namespace tube;
TEST_CASE("tube shaders: every program assembles with its defines first", "[tube][tube_shaders]") {
    for (const auto& info : kPrograms) for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
        const std::string src = assemble_fragment(info.id, decode); INFO(info.name);
        REQUIRE(src.rfind("#version 300 es\n", 0) == 0);
        REQUIRE(src.find("#version", 1) == std::string::npos);
        std::istringstream words{program_defines(info.id)}; std::string d;
        while (words >> d) REQUIRE(src.find("#define " + d + " 1\n") != std::string::npos);
        const bool hw = decode == SceneDecode::Hardware && info.reads_scene;
        REQUIRE((src.find("#define SCENE_HW_DECODE 1\n") != std::string::npos) == hw);
        REQUIRE(src.find("TUBE_MASK_TYPE") == std::string::npos);      // the mask is data
        REQUIRE(src.find("OUT_MESH") == std::string::npos);            // the bend is per pixel
        REQUIRE(src.find("TUBE_GLSL_TABLE_V1") != std::string::npos);  // the binary's marker
    }
    REQUIRE(assemble_vertex().rfind("#version 300 es\n", 0) == 0);
}
TEST_CASE("tube shaders: crt.metal's flash-safety caps are in the GLSL", "[tube][tube_shaders]") {
    const std::string out = assemble_fragment(Program::out_rich_scene, SceneDecode::Hardware);
    REQUIRE(out.find("min(noise, 0.2)") != std::string::npos);                 // NOISE: +-1 % + grain
    REQUIRE(out.find("min(flicker, 1.0) * 0.025") != std::string::npos);       // FLICKER: +-2.5 %
    REQUIRE(out.find("min(hum, 1.0) * 0.25") != std::string::npos);            // HUM: 25 % darker at most
    REQUIRE(out.find("min(jitter, 1.0) * 0.50 * mm") != std::string::npos);    // JITTER: +-0.25 mm a line
    REQUIRE(assemble_fragment(Program::sig_sharp, SceneDecode::Hardware).find("min(jitter, 1.0) * 0.50 * mm") != std::string::npos);
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
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass. (The kiosk binary first links the header in Task 10; the marker assertion is checked there.)
- [ ] **Step 5: Commit** — `tube: the shaders as files, embedded, assembled, and validated in CI`

---

### Task 7: The per-frame driver

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_driver.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_driver.cpp`

**Interfaces:**
- Consumes: Tasks 2, 3, 5; `tube_frame`, `FrameState`, `settle_frames`, `moves_on_its_own`, `tracking_moving`, `warmup_gain`, `kWarmupSeconds`, `kAblSeconds`, `kSettledWithin`, `kSettleFramesPerSecond`, `glow_sigma_px`, `mask_gain`.
- Produces (namespace `tube`):
  - `struct MaskTile { int w = 1, h = 1; std::vector<std::uint8_t> rgb; };` `MaskTile mask_tile(Mask, int pitch, float strength);` — one period, linear 0..255, port of the spike's `mask_at` (crt.metal `maskAt`); `none` → 1×1 white.
  - `inline constexpr float kGlassLightScale = 32.0f;` `inline constexpr double kDegaussSeconds = 1.4, kDegaussMovesSeconds = 1.5, kWarmupMovesSeconds = kWarmupSeconds + 0.1, kMeanPeriodSeconds = 0.2;`
  - `class AblLimiter { public: void on_mean(double light); void step(double now); double level() const; double target() const; double distance() const; bool measured() const; };` — Sound Factory's `readMean` + `stepBrightnessLimitTo`.
  - `struct DriverPeek { FrameClass cls; bool moving; };`
  - `struct DriverFrame { TubeUniforms u; Resolved resolved; PassPlan plan; TargetSizes sizes; float glow_sigma = 0.0f; MaskTile mask; bool still_changed = false; bool glass_changed = false; bool mask_changed = false; bool moving = false; };`
  - `class FrameDriver { public: void start(double now, std::uint64_t degauss_requests); DriverPeek peek(const TubeSetting&, const Picture&, double now, std::uint64_t degauss_requests) const; DriverFrame frame(const TubeSetting&, const Picture&, double now, std::uint64_t degauss_requests); void on_mean(double light); bool mean_due(double now) const; void note_mean_requested(double now); };`
  - `frame()`'s order: rebuild the still (`tube_uniforms_for`, `plan_passes`, `target_sizes`, `glow_sigma_px`) only when the setting or the picture changed (`still_changed`); `glass_changed` only when a glass input changed — `cornerPx`, `marginPx`, `rim`, `vignette`, `glassBlack`, `reflection`, or the picture size (the bake is ~4 ms; a brightness step must not rebake); the mask tile only when mask, pitch or strength changed; restart the tracking clock when any of `tracking`, `trackingRate`, `trackingWidth`, `trackingRecovery` changed; `dt = now - last` → `advance_character_phases`; the degauss start when `degauss_requests` exceeds the baseline; `AblLimiter::step(now)`; `FrameState` as `TubeRenderer.mm` lines 937–958; `tube_frame(still, resolved, fs)`; the settle debt (`settle_frames(u, distance)`) re-armed on `still_changed` or a new mean target, minus one each frame. `moving` = `moves_on_its_own(resolved) || tracking_moving(resolved, tracking_elapsed) || now - start < kWarmupMovesSeconds || now - degauss_start < kDegaussMovesSeconds || settle_debt > 0`.

- [ ] **Step 1: Write the failing test**

```cpp
using namespace tube;
const Picture k1080{1440.0, 1080.0, false};
TEST_CASE("tube driver: warm-up from start, then still", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); d.start(100.0, 0);   // no settle debt
    REQUIRE(d.frame(s, k1080, 100.1, 0).u.warmupGain == 0.0f);
    REQUIRE(d.peek(s, k1080, 101.0, 0).moving);
    REQUIRE(d.frame(s, k1080, 102.0, 0).u.warmupGain == 1.0f);
    REQUIRE_FALSE(d.peek(s, k1080, 102.1, 0).moving);           // nothing moves, nothing owed
}
TEST_CASE("tube driver: degauss when the counter rises, never at start", "[tube]") {
    FrameDriver d; TubeSetting s; set_row(s, Effect::afterglow, 0); d.start(0.0, 7);
    REQUIRE(d.frame(s, k1080, 3.0, 7).u.degauss == 0.0f);
    REQUIRE(d.frame(s, k1080, 4.0, 8).u.degauss == Approx(1.0f));
    REQUIRE(d.frame(s, k1080, 4.7, 8).u.degauss == Approx(0.5f));
    REQUIRE(d.peek(s, k1080, 5.45, 8).moving);
    REQUIRE(d.frame(s, k1080, 5.6, 8).u.degauss == 0.0f);
    REQUIRE_FALSE(d.peek(s, k1080, 5.6, 8).moving);
}
TEST_CASE("tube driver: the plan changes only when a row crosses neutral", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 0); const PassPlan p0 = d.frame(s, k1080, 2.0, 0).plan;
    set_row(s, Effect::brightness, 70);
    auto f = d.frame(s, k1080, 2.1, 0); REQUIRE(f.still_changed); REQUIRE(f.plan == p0);
    REQUIRE_FALSE(d.frame(s, k1080, 2.2, 0).still_changed);
    set_row(s, Effect::afterglow, 0); set_row(s, Effect::glow, 0); set_row(s, Effect::sharpness, 50);
    set_row(s, Effect::noise, 10);
    REQUIRE_FALSE(d.frame(s, k1080, 2.3, 0).plan == p0);
}
TEST_CASE("tube driver: the glass rebakes only for its own rows", "[tube]") {
    FrameDriver d; TubeSetting s; d.start(0.0, 0);
    REQUIRE(d.frame(s, k1080, 2.0, 0).glass_changed);          // the first frame bakes
    set_row(s, Effect::brightness, 70); REQUIRE_FALSE(d.frame(s, k1080, 2.1, 0).glass_changed);
    set_row(s, Effect::vignette, 60);   REQUIRE(d.frame(s, k1080, 2.2, 0).glass_changed);
    REQUIRE(d.frame(s, Picture{960.0, 720.0, false}, 2.3, 0).glass_changed);
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
- [ ] **Step 5: Commit** — `tube: the per-frame driver — still cache, glass key, phases, limiter, warm-up, degauss`

---

### Task 8: One content rectangle, the present hold and the redraw gate

**Files:**
- Create: `magic_dingus_box_cpp/src/video/content_area.h`, `magic_dingus_box_cpp/src/app/tube_redraw.h`
- Modify: `magic_dingus_box_cpp/src/utils/frame_pacing.h`, `magic_dingus_box_cpp/src/app/redraw_gate.h`, `magic_dingus_box_cpp/src/app/redraw_gate.cpp`
- Test: `magic_dingus_box_cpp/tests/video/test_content_area.cpp`, `magic_dingus_box_cpp/tests/utils/test_frame_pacing.cpp`, `magic_dingus_box_cpp/tests/app/test_redraw_gate.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_redraw.cpp`

**Interfaces:**
- Consumes: `video::IRect`, `video::four_by_three_rect`; `tube::FrameClass`.
- Produces:
  - `video::ContentRects { IRect content; IRect logical_content; bool letterbox_active; };` `ContentRects content_rects(bool modern_tv, bool full_screen_film, bool media_browser, int fb_w, int fb_h, int canvas_w, int canvas_h);` — exactly main.cpp's computation today, moved, not changed.
  - `int utils::held_vblanks(int refresh_hz) noexcept;` — `max(1, lround(hz / 30.0))`, `hz <= 0` treated as 60.
  - `std::chrono::microseconds utils::present_hold(int refresh_hz, std::chrono::microseconds since_last_present);` — `held_vblanks == 1` → 0; else `max(0, period × (held_vblanks − 1) + period / 8 − since_last_present)`, with `frame_pacing_for`'s period and margin. The caller passes a FRESH `steady_clock::now() - last_present_done`, read after `eglSwapBuffers` and before the page flip.
  - `std::chrono::microseconds utils::held_frame_sleep(int refresh_hz, std::chrono::microseconds iteration_elapsed);` — the bottom-of-loop sleep for a held frame: only the floor `period × held_vblanks − period / 8 − iteration_elapsed` (≥ 0), never a post-present gap, so the next frame starts rendering right after the flip.
  - `app::MainMenuActivity::tube_rich_motion` (default false): non-static, not a field-rate reason.
  - `app::TubeRedraw { bool field_rate_motion = false; bool continuous_motion = false; bool hold_present = false; };` `TubeRedraw app::tube_redraw(bool tube_drawing, tube::FrameClass, bool tube_moving, int vrefresh_hz) noexcept;` — not drawing: all false. Lean60: `field_rate_motion = tube_moving`. Rich30: `continuous_motion = tube_moving`, `hold_present = vrefresh_hz >= 48`.

The cadence cases (60 Hz unless named; the hold after the swap): a 10 or 20 ms frame waits to 18.75 ms and flips at 33.3; a 26 ms frame is at the GPU by the swap and flips at 33.3; a 40 ms frame flips at 50 (20 fps, counted late, toast after 30 s); 50 Hz: hold to 22.5 ms, 25 fps; 120 Hz: four vblanks, hold to 26.0 ms, 30 fps; 59.94 Hz reads 60, 2 ms margin; after a rest (input after idle) the gap is long, no hold; a class change mid-video costs its builds on the change frame and resets the pacing monitor; the Media Browser and games draw no tube, so no hold.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_content_area.cpp
TEST_CASE("content rects: the kiosk's three layouts", "[video]") {
    auto m = video::content_rects(true, false, false, 1920, 1080, 1280, 720);
    REQUIRE(m.content == video::IRect{240, 0, 1440, 1080});
    REQUIRE(m.logical_content == video::IRect{160, 0, 960, 720}); REQUIRE(m.letterbox_active);
    auto f = video::content_rects(true, true, false, 1920, 1080, 1280, 720);   // full-screen film
    REQUIRE(f.content == video::IRect{0, 0, 1920, 1080}); REQUIRE_FALSE(f.letterbox_active);
    REQUIRE(f.logical_content == video::IRect{0, 0, 1280, 720});
    REQUIRE(video::content_rects(true, false, true, 1920, 1080, 1280, 720).content == video::IRect{0, 0, 1920, 1080});
    auto c = video::content_rects(false, false, false, 1280, 720, 640, 480);   // CRT Native
    REQUIRE(c.content == video::IRect{0, 0, 1280, 720}); REQUIRE(c.logical_content == video::IRect{0, 0, 640, 480});
    REQUIRE(video::content_rects(true, false, false, 1280, 720, 1280, 720).content == video::IRect{160, 0, 960, 720});
}
// test_frame_pacing.cpp
TEST_CASE("present hold: round(refresh / 30) vblanks after the last flip", "[pacing]") {
    using us = std::chrono::microseconds;
    REQUIRE(utils::held_vblanks(60) == 2); REQUIRE(utils::held_vblanks(120) == 4);
    REQUIRE(utils::held_vblanks(50) == 2); REQUIRE(utils::held_vblanks(30) == 1); REQUIRE(utils::held_vblanks(0) == 2);
    REQUIRE(utils::present_hold(60, us{5000}) == us{16666 + 2083 - 5000});    // fast frame: wait
    REQUIRE(utils::present_hold(60, us{18749}) == us{0});
    REQUIRE(utils::present_hold(60, us{30000}) == us{0});                     // slow frame: flip now
    REQUIRE(utils::present_hold(120, us{0}) == us{3 * 8333 + 1041});
    REQUIRE(utils::present_hold(50, us{0}) == us{20000 + 2500});
    REQUIRE(utils::present_hold(30, us{0}) == us{0});
    REQUIRE(utils::held_frame_sleep(60, us{40000}) == us{0});                 // a held frame never adds a gap
    REQUIRE(utils::held_frame_sleep(60, us{1000}) == us{2 * 16666 - 2083 - 1000});  // the floor (flip did not block)
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
    REQUIRE_FALSE(rich.continuous_motion); REQUIRE(rich.hold_present);
    REQUIRE(app::tube_redraw(true, FrameClass::Rich30, true, 120).continuous_motion);
    REQUIRE_FALSE(app::tube_redraw(true, FrameClass::Rich30, true, 30).hold_present);
}
```
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement.** Comments: `content_area.h` says main.cpp and the tube must both use it; `present_hold` says where it is called (after the swap, before the flip, fresh clock) and why Rich30 holds the flip instead of skipping an iteration (design §4).
- [ ] **Step 4: Run** `./build-tube3/test_video_unit`, `test_utils_unit`, `test_app_unit` in full — pass (existing gate and pacing cases unchanged).
- [ ] **Step 5: Commit** — `tube: one content rectangle, the Rich30 present hold, and the gate's tube motion`

---

### Task 9: The pacing monitor, the GPU timing averager and the build guard

**Files:**
- Create: `magic_dingus_box_cpp/src/app/tube_pacing.h`, `magic_dingus_box_cpp/src/tube/tube_gpu_timing.h`, `magic_dingus_box_cpp/src/app/tube_build_guard.h`, `magic_dingus_box_cpp/src/app/tube_build_guard.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (`APP_SOURCES` and `test_app_unit`: `src/app/tube_build_guard.cpp`)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_pacing.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_build_guard.cpp`

**Interfaces:**
- Produces:
  - `app::TubePacingMonitor` — `using Clock = std::chrono::steady_clock;` `void on_present(Clock::time_point done, tube::FrameClass, int vrefresh_hz, bool video_playing);` — late when the interval exceeds `period × (Rich30 ? held_vblanks(hz) : 1) + period / 2`; the first present after a gap over 250 ms counts nothing. `struct Report { std::uint64_t presents = 0, late = 0, video_presents = 0, video_late = 0; tube::FrameClass cls; };` `std::optional<Report> take_report(Clock::time_point now);` (every 60 s; the first call starts the window). `bool too_heavy() const;` (last 30 s of video presents, window ≥ 30 s, late > 5%). `bool take_toast();` (true once). `void reset();` (a setting or frame-class change).
  - `tube::PassTimingAverager` — `enum class Pass { quarter, glow, signal, lines, output, history, probe };` `void add(Pass, double ms);` `void end_frame();` `std::optional<std::string> take_line(double now_seconds);` — every 10 s: `"tube gpu: <n> frames: quarter <a> glow <b> signal <c> lines <d> output <e> history <f> probe <g> = <total> ms"`, two decimals, `-` for a pass that ran in no frame.
  - `app::TubeBuildGuard` — `static constexpr const char* kDefaultPath = "/dev/shm/mdb-tube-build";` `explicit TubeBuildGuard(std::string marker_path);` (reads it once: present → `tripped()`; its content is the programs' names) `bool tripped() const;` `const std::string& tripped_program() const;` `void begin(std::string_view programs);` (writes the names of every program not yet drawn in its real pass; a write error logs WARN and continues) `void end();` (removes it). Memory-backed `/dev/shm`: survives a kiosk restart, not a reboot.

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
    app::TubePacingMonitor hz120; auto u = t + 1h;                            // 120 Hz: the cadence is four vblanks
    for (int i = 0; i < 5; ++i) { u += 33333us; hz120.on_present(u, FrameClass::Rich30, 120, true); }
    REQUIRE(hz120.take_report(u) == std::nullopt);
    REQUIRE(hz120.take_report(u + 61s)->late == 0);
}
TEST_CASE("tube pacing: too heavy after 30 s of video with more than 5% late, toast once", "[tube]") {
    app::TubePacingMonitor m; auto t = app::TubePacingMonitor::Clock::time_point{} + 1h;
    for (int i = 0; i < 1900; ++i) { t += (i % 10 == 0) ? 33333us : 16667us; m.on_present(t, FrameClass::Lean60, 60, true); }
    REQUIRE(m.too_heavy()); REQUIRE(m.take_toast()); REQUIRE_FALSE(m.take_toast());
    m.reset(); REQUIRE_FALSE(m.too_heavy());
    app::TubePacingMonitor menu; t += 1s;
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
    { app::TubeBuildGuard g(path); REQUIRE_FALSE(g.tripped()); g.begin("sig_guns_chroma out_rich_raster_ag"); }  // died here
    app::TubeBuildGuard after(path);
    REQUIRE(after.tripped()); REQUIRE(after.tripped_program() == "sig_guns_chroma out_rich_raster_ag");
    std::filesystem::remove(path);
    app::TubeBuildGuard clean(path); clean.begin("glass"); clean.end();
    REQUIRE_FALSE(app::TubeBuildGuard(path).tripped());
}
```
(The 120 Hz case's first `take_report` starts the window.)
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `./build-tube3/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: pacing monitor, GPU timing averager and the build guard`

---

### Task 10: The GL side, part 1 — capabilities, programs under the guard, targets, mask and glass

**Files:**
- Create: `magic_dingus_box_cpp/src/ui/tube_renderer.h`, `magic_dingus_box_cpp/src/ui/tube_renderer.cpp`, `magic_dingus_box_cpp/src/ui/tube_renderer_passes.cpp` (an empty TU with the header include until Task 11)
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (`UI_SOURCES`)

**Interfaces:**
- Consumes: Tasks 5, 6, 7, 9; `ui::Renderer`'s VAO/VBO and 2D program (passed in).
- Produces (namespace `ui`, kiosk-only; nothing calls it until Task 12):
  ```cpp
  class TubeRenderer {
  public:
      struct Caps { bool srgb_write_control = false, srgb_decode = false, half_float_targets = false, timer_query = false; };
      struct GlEnv { GLuint scene_tex = 0; int scene_w = 0, scene_h = 0, fb_w = 0, fb_h = 0; GLuint vao = 0, vbo = 0, ui_program = 0; };
      explicit TubeRenderer(std::string guard_path = app::TubeBuildGuard::kDefaultPath);
      ~TubeRenderer();                                   // no GL: release_gl() runs from Renderer::cleanup()
      bool available() const;                            // not hang-tripped, and no failure for the current plan
      std::string_view unavailable_reason() const;       // "" | "build_hung" | "build_failed"
      const Caps& caps();                                // probed once per context
      void allow_builds();                               // main calls it right after READY=1; prepare() does nothing before
      void begin_scene(GLuint scene_tex, int w, int h);  // SRGB8 storage + FRAMEBUFFER_SRGB off; re-specifies only a new or resized texture
      void end_tube_scene(GLuint scene_tex, int w, int h);  // back to RGBA8: the engine left Tube
      bool prepare(const tube::PassPlan&, tube::SceneDecode);  // compiles and links what the plan lacks, marker written first
      bool composite(const GlEnv&, video::IRect picture, const tube::DriverFrame&);  // Task 11
      bool validate_all_programs(const GlEnv&, video::IRect picture, tube::SceneDecode);  // Task 14
      void release_gl();
  private:
      struct Impl; std::unique_ptr<Impl> impl_;
  };
  ```
- Behaviour decided here (reference: spike `log_gl_info`, `build`, `first_use`, `ensure_tex`, `free_tex`, `begin_scene`, `scene_decode`, the glass bake):
  - **Caps** from `glGetString(GL_EXTENSIONS)`; `SceneDecode::Hardware` iff `srgb_write_control && srgb_decode`.
  - **Programs:** one cache slot per `tube::Program`. `prepare` (a no-op before `allow_builds()`) writes the guard marker naming every program of `programs_of(plan)` not yet *drawn*, then compiles `assemble_vertex()` + `assemble_fragment(p, decode)`, links, caches uniform locations for `uniforms_of(p)`, and logs `tube: program <name> compiled+linked in <ms> ms`. It does **not** draw: Mesa compiles its real variant at the first draw with the real target and texture formats, so the marker stays until Task 11's composite has drawn them all through their real passes and returned. A compile/link failure logs the info log once, records the failed plan, removes the marker and returns false; it is retried when the plan changes or `FrameDriver::start` runs again (an engine change).
  - **Forced hang (preview builds only, `#ifdef MDB_TUBE_PREVIEW`):** `MDB_TUBE_FORCE_HANG=<program name>` (read once) makes `prepare` sleep forever, after writing the marker, before compiling that program — Task 14 Step 4.
  - **Targets** by `TargetSizes`, re-made only when a size or the plan's need changes: quarter, glow tmp, glow out, history (×2, ping-pong) `SRGB8_ALPHA8`; signal and lines `RGBA16F` when `half_float_targets`, else `SRGB8_ALPHA8`; glass `RGBA8`; mean 64×64 `SRGB8_ALPHA8` with mipmaps; mask `RGBA8` from `MaskTile`, NEAREST + REPEAT. An incomplete FBO counts as a build failure (retried as above).
  - **Scene storage:** `begin_scene` remembers (texture, w, h, format); it re-specifies only when `ensure_scene_fbo` produced a new texture or size, or after `release_gl` — never per frame.
  - **Glass** baked when `DriverFrame::glass_changed`, logged with its time (`tube: glass baked in <ms> ms`); mask uploaded when `mask_changed`.
  - The TubeRenderer is called with the UI batch already flushed (its caller's job, `flush_ui_batch()` in the Renderer files); its header says so.
  - `release_gl()` deletes every program, texture, FBO, query and PBO, zeroes the ids and forgets the remembered scene storage; the next frame rebuilds.

- [ ] **Step 1: Write the class declaration and `caps`/`allow_builds`/`release_gl`.**
- [ ] **Step 2: Write the program cache and `prepare` (marker, compile, link, uniform locations, failure record, the preview forced hang).**
- [ ] **Step 3: Write targets, scene storage, mask upload and the glass bake.**
- [ ] **Step 4: Verify in the Pi-userspace container:** `bash magic_dingus_box_cpp/dev/pisim/pisim.sh check` — the kiosk builds, every suite passes on aarch64, `strings magic_dingus_box_cpp/build-pisim/magic_dingus_box_cpp | grep -c TUBE_GLSL_TABLE_V1` ≥ 1. `grep -n "TubeRenderer" magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/src/ui/renderer*.cpp` → no hits (production unchanged).
- [ ] **Step 5: Commit** — `tube: GL resources and the program cache under the build guard (not wired)`

---

### Task 11: The GL side, part 2 — the passes, the mean probe and the debug timers

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/tube_renderer_passes.cpp`, `magic_dingus_box_cpp/src/ui/tube_renderer.cpp`

**Interfaces:**
- Consumes: Task 10; `tube::DriverFrame`, `PassPlan`, `TargetSizes`, `uniforms_of`, `validation_plans`, `PassTimingAverager`.
- Produces: `TubeRenderer::composite(env, picture, frame)`; `void set_mean_sink(std::function<void(double)>)`; `validate_all_programs`.
- `composite` order (reference: spike `TubeSpike::composite` and `Impl::draw_v2`):
  1. `prepare(frame.plan, decode)`; false → return false (the caller draws Enhanced).
  2. A scoped GL-state object restores, on **every** return path: VAO 0, FBO 0, viewport whole framebuffer, units 7..0 unbound, blend on with `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, the 2D program, `GL_FRAMEBUFFER_SRGB_EXT` off, scene decode `GL_SKIP_DECODE_EXT`. In: clip-space quad in the renderer's VBO; blend off; `GL_FRAMEBUFFER_SRGB_EXT` on; scene decode `GL_DECODE_EXT` (hardware path).
  3. Mean probe when the driver said it is due: `quarter` into the 64×64 target, `glGenerateMipmap`, `glReadPixels` of level 6 (1×1) into a `GL_PIXEL_PACK_BUFFER`, `glFenceSync`; on later frames `glClientWaitSync(fence, 0, 0)` — when signalled, `glMapBufferRange` 4 bytes, decode sRGB on the CPU, luminance `0.2126 r + 0.7152 g + 0.0722 b`, to the sink. Never waits.
  4. Rich passes the plan asks for: quarter → blur H → blur V; signal (`signal_w × signal_h`, `uSigSize`); lines.
  5. Clear the bars black; output into the picture rectangle on the default framebuffer (GL's bottom-left y `fb_h - y - h`) or into the history (`_ag`), then `copy_encode` to the picture rectangle; swap the history pair.
  6. When every program of the plan has now been drawn in its real pass: `guard.end()`.
  - Uniform upload: for each name in `uniforms_of(program)`, the cached location and the `DriverFrame::u` field (a name → member table like the spike's `kFields`, with a `static_assert` that it covers `TubeUniforms`).
  - `MDB_TUBE_GPU_TIMING=1` (read once, logged at INFO when on): each pass in a `GL_TIME_ELAPSED_EXT` query from a ring of 4 sets, read only when `GL_QUERY_RESULT_AVAILABLE`, else dropped; results to `PassTimingAverager`, its line at INFO. Off: no query objects exist.
  - Any GL error during the frame (`glGetError` once at the end) → log once, record a failure for this plan (retried as Task 10), return false.
- `validate_all_programs(env, picture, decode)`: for each plan of `validation_plans()`, run `composite` with a synthetic `DriverFrame` for that plan (neutral uniforms, `still_changed`, `glass_changed`, `mask_changed` true) into the real default framebuffer and real targets — so each program's first draw is its real variant — logging `tube validate: <program> first real draw <ms> ms` per program (CPU time across its draw call) and `tube validate: N of N programs OK`.

- [ ] **Step 1: Write the scoped state object and steps 1–2 and 5–6 (a plan with only the output pass draws end to end).**
- [ ] **Step 2: Write the rich passes (step 4) and the history pair.**
- [ ] **Step 3: Write the mean probe and its sink.**
- [ ] **Step 4: Write the debug timers and `validate_all_programs`.**
- [ ] **Step 5: Verify:** `bash magic_dingus_box_cpp/dev/pisim/pisim.sh check`; still no caller (`grep` as Task 10).
- [ ] **Step 6: Commit** — `tube: the passes, the async mean probe and the debug GPU timers (not wired)`

---

### Task 12: Wiring — the renderer, the main loop and the preview build

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/renderer.h`, `magic_dingus_box_cpp/src/ui/renderer.cpp` (construct, `reset_gl`, `cleanup`), `magic_dingus_box_cpp/src/ui/renderer_crt.cpp` (`begin_scene_fbo`, `end_scene_fbo_and_composite`), `magic_dingus_box_cpp/src/main.cpp`, `magic_dingus_box_cpp/src/app/app_state.h`, `magic_dingus_box_cpp/CMakeLists.txt` (option `MDB_TUBE_PREVIEW`, default OFF; when ON, `add_compile_definitions(MDB_TUBE_PREVIEW=1)` at the top level, before any target, so every translation unit sees the same constant), `.github/workflows/test-local.yml` (`kiosk-build`) and `.github/workflows/release.yml` (one assertion each)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_menu_logic.cpp` (one case)

**Interfaces:**
- Consumes: Tasks 7–11; `app::effective_crt_engine`, `app::tube_offered`, `TubeLiveState::degauss_requests`.
- Produces:
  - `app_state.h`: `kTubeRendererReady` is `true` only under `#ifdef MDB_TUBE_PREVIEW` (TEST-ONLY; removed in Task 17). `struct TubeRenderStatus { bool drawing = false; int frame_rate = 60; bool too_heavy = false; std::string unavailable; };` and `AppState::tube_render`.
  - `Renderer`: `void set_tube_picture(video::IRect content, bool crt_native);` `struct TubeStatus { bool drawing = false; tube::FrameClass cls = tube::FrameClass::Lean60; bool moving = false; };` `TubeStatus tube_status(const app::AppState&, double now) const;` (drawing iff the effective engine is Tube, not the Media Browser, and `TubeRenderer::available()`; class and moving from `FrameDriver::peek`) `std::string_view tube_unavailable_reason() const;` `void allow_tube_builds();`
  - `begin_scene_fbo`: engine = `effective_crt_engine(crt_engine, tube_offered(state))`; for Tube with an available TubeRenderer, gates 1 and 4 do not apply; `FrameDriver::start` on the first Tube frame after any other engine; `TubeRenderer::begin_scene` before the clear. Leaving Tube calls `end_tube_scene`.
  - `end_scene_fbo_and_composite`: Tube → `composite(env, tube_picture_, driver.frame(...))`; false → this frame's Enhanced composite.
  - `reset_gl()` / `cleanup()`: `tube_->release_gl()`.
  - `main.cpp`:
    - `const video::ContentRects rects = video::content_rects(...)` once per iteration before the gate; the draw block uses `rects` instead of its inline computation; `ui_renderer.set_tube_picture(rects.content, crt_native)`.
    - Right after `sd_notify(0, "READY=1")`: `ui_renderer.allow_tube_builds()`. No program is built before it; until the plan's programs are built and drawn, frames draw Enhanced.
    - Gate block: `ui_renderer.tube_status(state, now)` → `app::tube_redraw(...)`; when Tube draws, `act.crt_time_effects = r.field_rate_motion` and `act.tube_rich_motion = r.continuous_motion`.
    - **Between `egl.swap_buffers()` and `present_frame()`:** when `r.hold_present`, `sleep_for(utils::present_hold(vrefresh, std::chrono::steady_clock::now() - last_present_done))` — a fresh clock read here, never the iteration's `now`. At the bottom of the loop, when the frame was held, `utils::held_frame_sleep(vrefresh, iteration_elapsed)` replaces `frame_cap_sleep`.
    - After `present_frame()`: `tube_pacing.on_present(...)` while Tube draws; its per-minute report at INFO (`"Tube pacing: class <30|60>, <n> presents (<v> with video), <l> late"`); `take_toast()` → `ui::Toast::show("This CRT look is too heavy for smooth video")`; `tube_pacing.reset()` when the tube setting or the frame class changes; `state.tube_render` filled each iteration.
    - Preview builds only (`#ifdef MDB_TUBE_PREVIEW`): `MDB_TUBE_VALIDATE=1` (read once) makes the first Tube frame after `allow_tube_builds()` call `validate_all_programs` for both decode paths (Task 14).
    - A tripped guard logs `LOG_ERROR("tube: the last kiosk died building {} — Tube draws as Enhanced until reboot", names)` once.
  - Preview builds log `LOG_WARN("TUBE PREVIEW BUILD — test only, never ship")` at start. `kiosk-build`: `if strings "$B" | grep -q "TUBE PREVIEW BUILD"; then echo "FATAL: a Tube preview build"; exit 1; fi`; `release.yml` the same with `strings magic_dingus_box_cpp`.

- [ ] **Step 1: Write the test** (pins that production still offers nothing)

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
- [ ] **Step 2: Run** `./build-tube3/test_app_unit "[tube]"` — passes already (the constant is false): it guards the steps below.
- [ ] **Step 3: Renderer wiring** (`renderer.h`, `renderer.cpp`, `renderer_crt.cpp`).
- [ ] **Step 4: main.cpp wiring** (content rects, `allow_tube_builds`, gate inputs, the hold after the swap, the bottom sleep, pacing, toast, status).
- [ ] **Step 5: The preview option and the two workflow assertions.**
- [ ] **Step 6: Verify:** `./build-tube3/test_app_unit`, `test_video_unit`, `test_utils_unit` in full; `bash magic_dingus_box_cpp/dev/pisim/pisim.sh check`; the preview build (runbook) compiles and `strings` finds `TUBE PREVIEW BUILD` in it and not in `build-pisim/`'s binary. Read the `main.cpp` diff for anything that runs when Tube is not drawing: only `content_rects` (pinned to today's rectangles by Task 8) and `set_tube_picture` (stores two values).
- [ ] **Step 7: Commit** — `tube: wired into the renderer and the main loop, inert until the renderer is switched on`

---

### Task 13: The controls show which rows force 30 fps

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/tube_rows_json.h`, `magic_dingus_box/web/static/tube_rows.json` (regenerated), `magic_dingus_box_cpp/src/app/tube_menu_logic.h`, `magic_dingus_box_cpp/src/ui/settings_menu.cpp` (the `TUBE_VALUE` rows' sublabel), `magic_dingus_box_cpp/src/app/status_writer.cpp` (`tube_status_json`), `magic_dingus_box/web/static/manager_display.js`, `magic_dingus_box/web/static/index.html` (`manager_display.js?v=` +1), `magic_dingus_box_cpp/scripts/dev/check_tube_parity.py` (`parse_always_rich`)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_rows_json.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_menu_logic.cpp`, `magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp`, `magic_dingus_box/web/tests/test_tube_rows_json.py`, `magic_dingus_box/web/tests/test_manager_display_js.py`

**Interfaces:**
- Consumes: Task 3 (`forces_30fps`, `kAlwaysRich`, `setting_frame_class`, `frames_per_second`); Task 12 (`AppState::tube_render`); Plan 2's `tube_rows_json()`, `tube_look_label`, `tube_status_json`.
- Produces:
  - Row file: each row gains `"forces_30fps": bool` (after `crt_native_neutral`); each look gains `"fps": 60|30` (`setting_frame_class(resolve(look), false)`) after `"mask"`.
  - `tube_menu_logic.h`: `std::string tube_look_label(const tube::TubeSetting&, bool crt_native);` appends `" · 60 fps"` / `" · 30 fps"` (Plan 2's callers pass `mode == CRT_NATIVE`); `std::string_view tube_rate_tag(tube::Effect) noexcept;` (`"30 fps"` or `""`), appended as `" · 30 fps"` to a forcing row's sublabel by `settings_menu.cpp`.
  - Status `tube` block (offered): `"frame_rate"`, `"too_heavy"`, `"unavailable"` (string or null).
  - `manager_display.js`: `function tubeRateTag(row)` → `"30 fps"` when `row.forces_30fps`, else `""`; each look's `fps` in the look picker; the header shows `"<frame_rate> fps"` and, when `too_heavy`, "This look is too heavy for smooth video on this TV".
  - `check_tube_parity.py`: `parse_always_rich(text)` reads `kAlwaysRich`'s names from `tube_budget.h`.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_tube_rows_json.cpp (append)
TEST_CASE("tube rows json: the 30 fps rows and each look's rate", "[tube][tube_rows_json]") {
    Json::Value root; std::istringstream in(app::tube_rows_json()); in >> root;
    for (const auto& r : root["rows"])
        REQUIRE(r["forces_30fps"].asBool() == tube::forces_30fps(*tube::parse_effect(r["key"].asString())));
    for (const auto& l : root["looks"]) {
        tube::TubeSetting s; tube::select_look(s, *tube::parse_look(l["key"].asString()));
        REQUIRE(l["fps"].asInt() == tube::frames_per_second(tube::setting_frame_class(tube::resolve(s), false)));
    }
    REQUIRE(root["looks"][3]["fps"].asInt() == 30);                  // tv is rich whatever Task 1b decided
}
// test_tube_menu_logic.cpp (update Plan 2's label cases to the new signature, and append)
TEST_CASE("tube menu logic: the Look row says its rate; forcing rows are tagged", "[tube]") {
    tube::TubeSetting s; for (auto e : tube::kAlwaysRich) tube::set_row(s, e, tube::rate_neutral(e));
    const bool lean = tube::setting_frame_class(tube::resolve(s), false) == tube::FrameClass::Lean60;
    REQUIRE(app::tube_look_label(s, false).find(lean ? " · 60 fps" : " · 30 fps") != std::string::npos);
    tube::set_row(s, tube::Effect::glow, 10);
    REQUIRE(app::tube_look_label(s, false).find(" · 30 fps") != std::string::npos);
    REQUIRE(app::tube_rate_tag(tube::Effect::glow) == "30 fps");
    REQUIRE(app::tube_rate_tag(tube::Effect::brightness).empty());
}
// test_status_writer.cpp (append)
//   offered state, look tv → t["frame_rate"].asInt() == 30, t["too_heavy"].asBool() == false, t["unavailable"].isNull();
//   state.tube_render.unavailable = "build_hung" → t["unavailable"].asString() == "build_hung".
```
```python
# test_tube_rows_json.py
def test_forces_30fps_includes_the_always_rich_rows():
    always = parity.parse_always_rich((CPP / "src/tube/tube_budget.h").read_text())
    assert always == {"glow", "afterglow", "sharpness", "convergence", "chromaBleed", "raster", "colorDrift"}
    assert always <= {r["key"] for r in ROWS["rows"] if r["forces_30fps"]}
# test_manager_display_js.py
def test_rate_tag_marks_only_forcing_rows():
    # node: tubeRateTag({forces_30fps: true}) == "30 fps"; tubeRateTag({forces_30fps: false}) == ""
```
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement**; regenerate the row file.
- [ ] **Step 4: Run** `./build-tube3/test_app_unit`, `test_ui_unit`, `test_phone_remote_unit` in full; `python -m pytest magic_dingus_box/web/tests/test_tube_rows_json.py magic_dingus_box/web/tests/test_manager_display_js.py -q`; `pisim.sh check`.
- [ ] **Step 5: Commit** — `tube: the controls tag the rows that force 30 fps and show each look's rate`

---

### Task 14 (HARDWARE): Smoke test, every program through its real pass, and a forced hang

**Files:** Create `docs/engineering/measurements/2026-11-xx-tube-programs-pi5.txt` (journal excerpts).

**Interfaces:** Consumes Tasks 10–12: in a preview build, `MDB_TUBE_VALIDATE=1` (read once) makes the first frame after `allow_tube_builds()` call `validate_all_programs` for both decode paths and log the summary; `MDB_TUBE_FORCE_HANG=<name>` (Task 10).

- [ ] **Step 1: Smoke.** Runbook 1–5 with the preview build and `--property=RuntimeMaxSec=600`. `tube_op '{"op":"engine","engine":"tube"}'`, then each look for 20 s on the static menu and with a video: no GL error, no `build_failed`, a screenshot per look (bent glass, scanlines, mask inside the cabinet art; the bars black); the intro on a restart of the unit draws Enhanced for its first frames, then the tube with the warm-up. Runbook 7.
- [ ] **Step 2: Validate.** Runbook 4–5 with `--setenv=MDB_TUBE_VALIDATE=1 --setenv=V3D_DEBUG=shaderdb --property=RuntimeMaxSec=180` and the engine set to Tube. Every one of the 28 fragment programs logs `first real draw … ms`; the summary says `28 of 28`; the shaderdb lines give each program's instruction and thread counts.
- [ ] **Step 3: Pass/fail.** PASS when all 28 draw, every first real draw ≤ 50 ms, `/dev/shm/mdb-tube-build` is gone after the summary, and `systemctl show -p Result mdb-tube-preview` ≠ `timeout`. A hang (a program's line missing, then the timeout): the marker names it; bisect its blocks the way Task 1 did, change that program's source (never add a program), re-run Task 6's validator and this step. A lean output in 2-thread mode is recorded and handed to Task 15: if its look then misses the lean budget, the costliest motion ingredient is removed from the lean set (`tube_budget.h`, the rows it carries then force 30 fps, the lean program stops compiling it) and Steps 2–3 run again.
- [ ] **Step 4: Forced hang under the watchdog.** Runbook 4, then a notify-type unit with the production watchdog: `sudo systemd-run --unit=mdb-tube-hang --uid=magic --gid=magic -p Type=notify -p NotifyAccess=main -p WatchdogSec=10 -p Restart=on-failure -p RestartSec=2 --working-directory=/home/magic/tube-preview --setenv=HOME=/home/magic --setenv=XDG_RUNTIME_DIR=/run/user/$(id -u magic) --setenv=MDB_TUBE_FORCE_HANG=out_lean_still /home/magic/tube-preview/magic_dingus_box_cpp`; once it is up (READY), `tube_op '{"op":"engine","engine":"tube"}'` with a look whose plan uses `out_lean_still` (or `MDB_TUBE_FORCE_HANG` set to a program the current look uses). Expect: the hang inside `prepare`, a watchdog `SIGABRT` within ~10 s, a restart, then on the second start `tube: the last kiosk died building …out_lean_still… — Tube draws as Enhanced until reboot`, the TV drawing (the op may not have been saved before the hang, so the saved engine shows; if it is Tube, it draws as Enhanced), `jq .tube.unavailable kiosk_status.json` → `"build_hung"`, and no further restarts in 60 s (`NRestarts` 1). Stop it, `sudo rm -f /dev/shm/mdb-tube-build`, runbook 7–8.
- [ ] **Step 5:** Write the journal excerpts and commit — `docs: Tube programs drawn through their real passes on the Pi 5; the hang guard holds`.

---

### Task 15 (HARDWARE): Budgets per look, and the cases that bite

**Files:** Modify `magic_dingus_box_cpp/src/tube/tube_looks.h`, `tests/app/test_tube_looks.cpp`, `magic_dingus_box/web/static/tube_rows.json` (when a look changes), `magic_dingus_box_cpp/src/tube/tube_budget.h` (`kLeanSet.cheap_beam` / `kRichBeamCheap` only, if the ladder flips them), the design's "Rows that differ from Sound Factory's" table; create `docs/engineering/measurements/2026-11-xx-tube-acceptance-pi5.txt`.

**Interfaces:** Consumes Task 1b's budgets (`kLeanBudgetMs`, `kRichBudgetMs`), Task 12's journal lines, the runbook.

- [ ] **Step 1: GPU run (timers on).** Runbook 1–5, preview build, `--setenv=MDB_TUBE_GPU_TIMING=1 --property=RuntimeMaxSec=14400`. Video playing (960×720 H.264, Modern TV 1080p). Each look 5 minutes: the mean of its `tube gpu` totals and per pass; one screenshot. Runbook 7.
- [ ] **Step 2: Pacing run (timers off, a separate run).** Runbook 4–5 without `MDB_TUBE_GPU_TIMING`, `RuntimeMaxSec=14400`. Each look 10 minutes with the video: the ten `Tube pacing` lines. PASS per look: GPU total ≤ its class's budget **and** `late / presents` ≤ 1% (every vblank for Lean60, 33.3 ms for Rich30). Runbook 7.
- [ ] **Step 3: On a fail, the ladder — each step takes rows to neutral and removes a pass or selects a smaller program, in the order of measured saving; one step at a time, re-running Steps 1–2 for that look only:**
  - Rich30: AFTERGLOW → 0 (removes the history pass and copy, 2.1–4.7 ms) → GLOW → 0 (the glow chain, ~3–4.7 ms) → the signal rows to neutral: SHARPNESS 50, CONVERGENCE 0, COLOUR DRIFT 0, CHROMA BLEED 0 (the signal pass, 2.2–10 ms; with TRUE RASTER left the pass stays at half width as `sig_sharp`) → `kRichBeamCheap = true` (0.7–1.7 ms; re-run Task 6's validator and Task 14 Step 2 first).
  - Lean60: `kLeanSet.cheap_beam = true` if not already (re-run Task 6 and Task 14 Step 2) → its moving rows to neutral, selecting `out_lean_still`, only if Task 1b priced that saving above 0.3 ms → **the look becomes Rich30**: its Sound Factory values back (Task 4's other branch for that look), the owner told.
  - Never "lower a value": values cost nothing measurable; structure does. Each change is an edit to `tube_looks.h`, its pinned test, the regenerated row file and the design's difference table, committed with the before/after numbers in the message.
- [ ] **Step 4: The cases** (pacing runs timers off unless a cost is asked; each recorded):
  - *Look changes during video* (PVM → TV → PVM → Everything, 10 s apart): no frame longer than 100 ms except the change frame; its `compiled+linked` lines ≤ 50 ms each; no glass bake on a non-glass row change (`tube: glass baked` absent while stepping brightness).
  - *Static menu*: a still look → the gate's first-window line shows skipping; Everything with every wear row 100 (`rollBand tracking colorDrift flagging purity`) → ≤ 30 presents a second and the Settings menu readable in a screenshot.
  - *Channel Guide* (Guide style, a channel with video in its window): screenshot — guide and window through one tube; pacing as the look's class.
  - *Full-screen film*: each look — late ≤ 1% at 33.3 ms; the journal shows which looks dropped afterglow/glow; a GPU run confirms the drop matches `predicted_ms` within 15% (else update `kPassCosts`).
  - *Display-mode change*: Modern TV 1080p → 720p → CRT Native → back, with a lean look and TV: no GL error, a screenshot after each, CRT Native without inset or rounded corners; if the TV offers 50 Hz or 120 Hz, TV's pacing at that rate (25 / 30 fps).
  - *A game and back* (any PS1 game, quit with Select+Start): the tube returns, `compiled+linked` lines after the return, no warm-up replay, a screenshot.
  - *Media Browser* (if present): enter and leave; the MB draws clean; the tube resumes without a jump.
  - *Degauss* (`tube_op '{"op":"degauss"}'` on a static menu): ~1.5 s of drawing, then rest.
  - *Heat*: 30 minutes of TV with video in the enclosure: `vcgencmd get_throttled` → `throttled=0x0`.
  - *Memory*: `MemAvailable` on the static menu with Classic, then with TV: falls by < 32 MB.
  - *Baseline*: the Enhanced composite's cost from Task 1b beside each look in the acceptance file.
- [ ] **Step 5:** Runbook 7–8. Write the acceptance file (every number above, per look) and commit with the look changes — `tube: looks taken to their class budgets on the Pi 5, and the acceptance numbers`.

---

### Task 16 (HARDWARE): The Pi 4B, and the policy override

**Files:** Create `magic_dingus_box_cpp/scripts/dev/bmp_diff.py` (stdlib only: two BMPs → the count of differing pixels and the largest channel difference); append to Task 15's acceptance file.

- [ ] **Step 1 (worker): `bmp_diff.py`** with a pytest in `magic_dingus_box_cpp/scripts/tests/test_bmp_diff.py` (two generated 4×4 BMPs differing in one pixel → `1`).
- [ ] **Step 2: The noise floor on a Pi 4B** (never a customer's; runbook 1–2): with the production kiosk (the previous release), Enhanced at the box's sliders, two captures of the static main menu 5 s apart (`touch data/screenshot_request` twice); `bmp_diff.py` → the floor (blink phase, Enhanced flicker). Then Classic, two captures, the same.
- [ ] **Step 3: Pi 4B, preview build** (runbook 4–5): `jq .tube kiosk_status.json` → `"gate": false, "reason": "needs_pi5"`; Settings → Display offers "Classic / Enhanced" only; no CRT Look tab; `journalctl -u mdb-tube-preview | grep -c "tube: program"` → `0`; `tube_op '{"op":"engine","engine":"tube"}'` changes nothing (status still Enhanced). Captures of the static main menu with Classic and with Enhanced; `bmp_diff.py` against Step 2's → within the floor. Runbook 7–8.
- [ ] **Step 4: Pi 5 under the override.** The runbook on the Pi 5 with `--setenv=MDB_PLATFORM_POLICY_OVERRIDE=pi4`: the four checks of Step 3 (`reason` `needs_pi5`). Runbook 7–8; `verify_box.sh` passes on both boxes afterwards.
- [ ] **Step 5: Commit** — `docs: Tube stays off on the Pi 4B and under the Pi 4 policy rehearsal`.

---

### Task 17: Switch it on

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (`kTubeRendererReady = true`, the `MDB_TUBE_PREVIEW` block removed), `magic_dingus_box_cpp/CMakeLists.txt` (the option removed), `magic_dingus_box_cpp/src/main.cpp` (the preview log line removed), `magic_dingus_box_cpp/src/ui/tube_renderer.cpp` (the forced-hang hook removed), `magic_dingus_box_cpp/tests/app/test_tube_menu_logic.cpp` (Task 12's case and Plan 2's `"Tube is offered only on a Pi 5 with the renderer ready"`), `CLAUDE.md` (Architecture: `src/tube/`, `ui/tube_renderer`, the lean set by measurement, the two frame classes and the present hold, the program table and the validator job, the build guard, `MDB_TUBE_GPU_TIMING`), `CHANGELOG.md` (`## [Unreleased]`), `magic_dingus_box_cpp/docs/OWNER_GUIDE.md` only if it lists the CRT engines (customer words: "Tube — a Sound Factory picture tube; some looks run at 30 frames a second")
- Keep: both workflows' `TUBE PREVIEW BUILD` assertions.

**Interfaces:** Produces the only visible change of the three plans: on a Pi 5, Settings → Display offers Tube and the Content Manager shows the CRT Look tab. The first deploy of any of this plan's code to a box's `/opt`.

- [ ] **Step 1: Update the tests** — `REQUIRE(app::kTubeRendererReady);`; Plan 2's gate case: a default `AppState` on a Pi 5 profile is offered; `tube_not_offered_reason` is `"needs_pi5"` on a Pi 4B profile and `""` on a Pi 5.
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Flip the constant; remove the preview switch and the forced-hang hook.**
- [ ] **Step 4: Run** all Mac suites (9 with Media Browser ON, 8 OFF: `cmake -S magic_dingus_box_cpp -B build-tube3-mb -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=ON` then `ctest --test-dir build-tube3-mb`, and the same OFF in `build-tube3`), the web pytest, `pisim.sh check`.
- [ ] **Step 5: On the owner's Pi 5** (runbook 1–2 for the window and the before line): `PI_HOST=magic@<the Pi 5> magic_dingus_box_cpp/dev/pisim/pisim.sh push` (it keeps `.prev` and restores it if the kiosk does not come up). Select Tube on the TV: a lean look (if any) at 60 fps, TV at 30 fps; `verify_box.sh` passes. Report as runbook 8 (the binary's hash is now the new one; the settings diff shows only the engine and look chosen). Then delete `build-tube3*` and `build-pisim-preview` (the Mac's disk).
- [ ] **Step 6: Commit** — `tube: switch the Tube engine on (Pi 5)`

---

## Requirement map (design → task)

| Design requirement | Task |
|---|---|
| §3 Frame class; the lean set and budgets by measurement; "forces 30 fps" flag | 1b, 3, 13 |
| §3 Shipped looks in two classes only if measured; rows that differ from Sound Factory | 1b, 4, 15 |
| §3 Pipeline 2 passes, hardware sRGB decode + fallback, half width, half float, glass ×32 | 5, 6, 10, 11 |
| §3 Owner calls: half-width softening, no warm-up softening on lean | 6, 15 (screenshots) |
| §3 Fixed program table, CI validator, drawn through real passes on a Pi 5 | 5, 6, 14 |
| §3 Programs built after READY=1, on engine start / plan change, never per frame | 7, 10, 12, 14 |
| §3 Compile hang isolated; guard covers the real first draw; forced hang under the watchdog; failures retried | 1, 9, 10, 11, 14 |
| §3 Picture rectangle from the content area; guide window; bezel on top | 8, 12, 15 |
| §3 Full-screen film Rich30 with predicted-cost drops | 3, 5, 15 |
| §3 CRT Native neutral fit | 2, 15 |
| §3 Accumulated phases | 2, 7 |
| §3 ABL mean probe 64×64, async; lean only if priced | 1b, 5, 7, 11 |
| §3 GPU timing only under the debug variable; pacing with it off | 1b, 9, 11, 15 |
| §3 Glass rebaked only for glass inputs | 7, 10, 15 |
| §4 Lean60 field rate; Rich30 hold after the swap, `round(hz/30)`; video/input/watchdog | 8, 12, 15 |
| §4 Resting and settle debt | 7, 8, 15 |
| §6 30 fps tags on the TV and the web | 13 |
| §8 Warm-up from the first tube frame; degauss from the request counter | 7, 12, 14, 15 |
| §9 Too heavy: per-minute log, toast, status flag; reset on class change | 9, 12, 13 |
| Acceptance: measured budgets, the ladder, films, heat, memory, Enhanced baseline, Pi 4B within the noise floor | 1b, 15, 16 |
| Build order 4: switch-on last, first deploy to `/opt` | 17 |
