# RetroArch Graceful Exit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Exiting any RetroArch game returns to the kiosk menu as one deliberate transition — single HDMI mode change, launch plate dissolves to black, menu fades up — instead of a stale gold frame, two TV resyncs, and a green flash.

**Architecture:** Three components in the shared exit path of `Controller::load_playlist_item` (covers all five launch routes and all ten cores): (1) restore the kiosk's real boot mode instead of a hardcoded 640x480, with a consume-once flag so the main loop's `reset_display` handler doesn't repeat the mode set; (2) dissolve the frozen launch plate via a new `loading_alpha` driven through the existing `progress_callback`; (3) a fullscreen black quad fading out under the bezel after the main loop resumes. Pure logic (alpha ramp, state resets) lives in `src/app/game_launch_recovery.{h,cpp}` so the Mac Catch2 suites cover it.

**Tech Stack:** C++17, DRM/KMS + EGL/GLES3, Catch2 v3 (Mac suites via `build-test/`), deploy to Pi via `scripts/deploy_cpp.sh`.

**Spec:** `docs/superpowers/specs/2026-08-02-retroarch-graceful-exit-design.md`

## Global Constraints

- **The launch-side look is unchanged.** Gold plate, `NOW LOADING`, phase-stepped bar, wording, colours — untouched. Only the return changes.
- **The fixed 1000ms settle sleep before `acquire_master()` is NOT replaced** — only lifted into a named constant (`kRetroArchSettleDelay`). Hardware-tuned; a bounded poll is a separate follow-on.
- **No `PlatformProfile` branch, no `#ifdef` per board.** All changes are board-identical DRM/render logic (dual-board contract, CLAUDE.md rule 1).
- The dissolve must run **after** `set_mode` — painting before the mode restore blocks on a page-flip that never arrives (measured 4.3s → 15.8s launches; comment at `controller.cpp:837`).
- Working directory is the repo root `magic_dingus_box ` (trailing space in the directory name — always quote paths). All `src/` paths below are relative to `magic_dingus_box_cpp/`.
- Mac test gate for every task: `cmake --build build-test --target test_retroarch_unit -j8 && ./build-test/test_retroarch_unit` from `magic_dingus_box_cpp/`.
- `main.cpp` line numbers below are as of commit `c9797e9` and drift as tasks land — each task quotes the surrounding code, anchor on that, not the number.

---

### Task 1: AppState fields + pure recovery helpers (Mac TDD)

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (~line 400, the loading-state block)
- Modify: `magic_dingus_box_cpp/src/app/game_launch_recovery.h`
- Modify: `magic_dingus_box_cpp/src/app/game_launch_recovery.cpp`
- Test: `magic_dingus_box_cpp/tests/retroarch/test_game_launch_recovery.cpp`

**Interfaces:**
- Consumes: existing `app::AppState`, `app::prepare_kiosk_state_after_game(AppState&)`.
- Produces (later tasks rely on these exact names):
  - `AppState` fields: `std::atomic<float> loading_alpha{1.0f}`, `std::atomic<bool> display_mode_restored{false}`, `std::atomic<int64_t> post_game_fade_start_ms{0}`
  - `void app::prepare_loading_state_for_launch(AppState& state)`
  - `float app::return_dissolve_alpha(float elapsed_ms, float duration_ms)`

- [ ] **Step 1: Write the failing tests**

Append to `tests/retroarch/test_game_launch_recovery.cpp` (add `#include <catch2/catch_approx.hpp>` at the top, below the existing include):

```cpp
TEST_CASE("prepare_loading_state_for_launch resets the launch plate to opaque",
          "[retroarch][recovery]") {
    app::AppState state{};
    state.is_loading_game = false;
    state.loading_alpha.store(0.0f);            // as left by a completed dissolve
    state.post_game_fade_start_ms.store(12345); // in-flight fade from a prior exit

    app::prepare_loading_state_for_launch(state);

    REQUIRE(state.is_loading_game.load());
    REQUIRE(state.loading_alpha.load() == 1.0f);
    REQUIRE(state.post_game_fade_start_ms.load() == 0);
}

TEST_CASE("return_dissolve_alpha ramps 1 to 0, clamped and monotone",
          "[retroarch][recovery]") {
    REQUIRE(app::return_dissolve_alpha(0.0f, 250.0f) == 1.0f);
    REQUIRE(app::return_dissolve_alpha(125.0f, 250.0f) == Catch::Approx(0.5f));
    REQUIRE(app::return_dissolve_alpha(250.0f, 250.0f) == 0.0f);
    REQUIRE(app::return_dissolve_alpha(500.0f, 250.0f) == 0.0f);  // past the end
    REQUIRE(app::return_dissolve_alpha(-50.0f, 250.0f) == 1.0f);  // before the start
    REQUIRE(app::return_dissolve_alpha(10.0f, 0.0f) == 0.0f);     // degenerate duration

    float prev = 1.0f;
    for (float t = 0.0f; t <= 500.0f; t += 10.0f) {
        const float a = app::return_dissolve_alpha(t, 250.0f);
        REQUIRE(a <= prev);
        REQUIRE(a >= 0.0f);
        REQUIRE(a <= 1.0f);
        prev = a;
    }
}

TEST_CASE("prepare_kiosk_state_after_game leaves display_mode_restored alone",
          "[retroarch][recovery]") {
    // The exit path sets this flag BEFORE prepare_kiosk_state_after_game runs;
    // the main loop's reset_display handler consumes it AFTER. If this helper
    // ever cleared it, the main loop would re-set_mode and the TV would
    // resync twice again.
    app::AppState state{};
    state.display_mode_restored.store(true);

    app::prepare_kiosk_state_after_game(state);

    REQUIRE(state.display_mode_restored.load());
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd "magic_dingus_box_cpp" && cmake --build build-test --target test_retroarch_unit -j8
```

Expected: **compile failure** — `loading_alpha` / `display_mode_restored` / `post_game_fade_start_ms` / `prepare_loading_state_for_launch` / `return_dissolve_alpha` not declared.

- [ ] **Step 3: Add the AppState fields**

In `src/app/app_state.h`, directly **below** the `loading_is_exit` declaration (line ~400 — do NOT delete `loading_is_exit` yet; Task 3 does that when its references go), add:

```cpp
    // Global alpha for the launch plate, 1.0 = fully drawn.
    //
    // The plate is dissolved to 0 on the way back from a game. The frame the
    // dissolve STARTS from is pixel-identical to the one already frozen on
    // the panel (the kiosk's own stale framebuffer, which the scanout picks
    // back up when RetroArch dies) — so the stale frame stops reading as a
    // hang and becomes frame 1 of a deliberate fade. Reset to 1.0 by
    // prepare_loading_state_for_launch at the NEXT launch, or that launch
    // opens invisible.
    std::atomic<float> loading_alpha{1.0f};

    // Set true by the game-exit path after it successfully restores the
    // kiosk's display mode; consumed (exchange(false)) by the main loop's
    // reset_display handler so it can SKIP its own set_mode. Without this,
    // every game exit did two mode changes and the TV resynced twice.
    // An explicit flag, not a get_current_mode() comparison: that getter
    // returns a cached mode which RetroArch's own mode changes never update,
    // so a comparison would wrongly skip the restore exactly when the exit
    // path's set_mode had failed.
    std::atomic<bool> display_mode_restored{false};

    // steady_clock time_since_epoch in ms when the post-game menu fade-up
    // began; 0 = no fade active. Set by the game-session exit hook, rendered
    // and eventually cleared by main.cpp's render loop.
    std::atomic<int64_t> post_game_fade_start_ms{0};
```

- [ ] **Step 4: Add the helpers**

`src/app/game_launch_recovery.h` — append below the existing declaration:

```cpp
// Reset the launch-plate state for a NEW game launch: plate fully opaque,
// any in-flight post-game fade cancelled, is_loading_game raised. Called
// from the game-session BEGIN hook so every launch route gets it.
void prepare_loading_state_for_launch(AppState& state);

// Pure ramp for the return dissolve: 1.0 at elapsed<=0 down to 0.0 at
// elapsed>=duration, clamped. duration<=0 returns 0 (treat a degenerate
// dissolve as already finished).
float return_dissolve_alpha(float elapsed_ms, float duration_ms);
```

`src/app/game_launch_recovery.cpp` — append inside `namespace app`:

```cpp
void prepare_loading_state_for_launch(AppState& state) {
    state.is_loading_game = true;
    state.loading_alpha.store(1.0f);
    state.post_game_fade_start_ms.store(0);
}

float return_dissolve_alpha(float elapsed_ms, float duration_ms) {
    if (duration_ms <= 0.0f) return 0.0f;
    const float a = 1.0f - elapsed_ms / duration_ms;
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd "magic_dingus_box_cpp" && cmake --build build-test --target test_retroarch_unit -j8 && ./build-test/test_retroarch_unit "[recovery]"
```

Expected: PASS (all `[recovery]` cases, including the pre-existing one).

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/app/app_state.h magic_dingus_box_cpp/src/app/game_launch_recovery.h magic_dingus_box_cpp/src/app/game_launch_recovery.cpp magic_dingus_box_cpp/tests/retroarch/test_game_launch_recovery.cpp
git commit -m "feat: state fields + pure helpers for graceful RetroArch return"
```

---

### Task 2: Restore the real kiosk mode on game exit (Component 1)

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/controller.h` (public section near `initialize_retroarch_launcher`, private members near `kiosk_mode_*`)
- Modify: `magic_dingus_box_cpp/src/app/controller.cpp` (settle sleep ~line 807, mode restore ~line 830)
- Modify: `magic_dingus_box_cpp/src/main.cpp` (boot-mode site ~1495, display-toggle site ~1761, `reset_display` handler ~1640)

**Interfaces:**
- Consumes: `AppState::display_mode_restored` (Task 1), `DrmDisplay::set_mode(w, h)`.
- Produces: `void Controller::set_kiosk_display_mode(uint32_t w, uint32_t h)` — called by `main.cpp` at boot and on display-mode toggle.

- [ ] **Step 1: Add the setter and members to `controller.h`**

In the public section (next to `initialize_retroarch_launcher()`):

```cpp
    // The display mode the kiosk runs at, for restoring after RetroArch.
    // Set at boot once the final mode is known, and again whenever the
    // CRT_NATIVE <-> MODERN_TV toggle changes it at runtime — otherwise the
    // stored value goes stale the first time an operator flips display mode.
    void set_kiosk_display_mode(uint32_t w, uint32_t h) {
        kiosk_mode_w_ = w;
        kiosk_mode_h_ = h;
    }
```

In the private section (next to `retroarch_launcher_`):

```cpp
    // Mode to restore after RetroArch exits. 0 = never set (fall back to
    // the legacy 640x480 floor).
    uint32_t kiosk_mode_w_ = 0;
    uint32_t kiosk_mode_h_ = 0;
```

- [ ] **Step 2: Name the settle sleep**

In `controller.cpp`, replace:

```cpp
        // Add delay here to ensure RetroArch has fully released DRM master and kernel resources
        std::cout << "Waiting for system to settle..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
```

with:

```cpp
        // Fixed settle before re-acquiring DRM master, so RetroArch has fully
        // released DRM and kernel resources. Load-bearing and hardware-tuned;
        // see the follow-on in the 2026-08-02 graceful-exit spec before
        // replacing this with a bounded poll.
        constexpr std::chrono::milliseconds kRetroArchSettleDelay{1000};
        std::cout << "Waiting for system to settle..." << std::endl;
        std::this_thread::sleep_for(kRetroArchSettleDelay);
```

- [ ] **Step 3: Replace the hardcoded 640x480 restore**

In `controller.cpp`, replace:

```cpp
            // Force restore video mode to ensure UI is visible
            std::cout << "Restoring display mode to 640x480..." << std::endl;
            display_->set_mode(640, 480);
```

with:

```cpp
            // Restore the mode the kiosk actually booted with — NOT 640x480.
            // No unit boots at 640x480 (target_drm_mode gives 1280x720 /
            // 1920x1080; 640x480 is only the boot cascade's last resort), so
            // hardcoding it made every game exit do TWO mode changes: down to
            // 640x480 here, then back up in main.cpp's reset_display handler
            // — two black-screen TV resyncs per exit. On success the
            // display_mode_restored flag tells that handler to skip its own
            // set_mode, making this the ONLY mode change on the way back.
            bool mode_restored = false;
            if (kiosk_mode_w_ > 0 && kiosk_mode_h_ > 0) {
                std::cout << "Restoring kiosk display mode "
                          << kiosk_mode_w_ << "x" << kiosk_mode_h_ << "..." << std::endl;
                mode_restored = display_->set_mode(kiosk_mode_w_, kiosk_mode_h_);
            }
            if (!mode_restored) {
                // Legacy floor, preserved for the never-configured case and
                // for a failed restore.
                std::cout << "Falling back to 640x480..." << std::endl;
                mode_restored = display_->set_mode(640, 480);
            }
            state.display_mode_restored.store(mode_restored);
```

Note: `mode_restored` intentionally goes true on the 640x480 fallback too — the flag means "a set_mode already happened, don't do another", not "the preferred mode is up". If the boot-mode set failed (cable/EDID trouble), the main loop's repeat attempt would fail for the same reason, so skipping it loses nothing; a box left at 640x480 is caught by `verify_box.sh`'s display-mode-vs-persisted-setting check. When even the fallback fails the flag stays false and the main loop's handler makes its own attempt, same as today.

- [ ] **Step 4: Feed the setter from `main.cpp` (two sites)**

Site 1 — boot, right after the final mode is read (anchor: `"Final Display Mode: "`):

```cpp
    mode = display.get_current_mode();
    std::cout << "Final Display Mode: " << mode.width << "x" << mode.height << " @ " << (mode.refresh/1000.0) << "Hz" << std::endl;
    gst_renderer.set_screen_size(mode.width, mode.height);
    // Remember the real kiosk mode so the game-exit path can restore it
    // directly (one mode change, not a 640x480 round-trip).
    controller.set_kiosk_display_mode(mode.width, mode.height);
```

Site 2 — the CRT_NATIVE ↔ MODERN_TV toggle (anchor: `"Switched to: "`):

```cpp
            if (ok) {
                // Update mode info and notify renderers
                mode = display.get_current_mode();
                std::cout << "Switched to: " << mode.name << " (" << mode.width << "x" << mode.height << ")" << std::endl;
                // Keep the game-exit restore target in sync with the new mode.
                controller.set_kiosk_display_mode(mode.width, mode.height);
```

(the existing `gst_renderer.set_screen_size` / `ui_renderer.set_framebuffer_size` lines below stay unchanged).

- [ ] **Step 5: Consume the flag in the `reset_display` handler**

In `main.cpp` (anchor: `"Resetting display state after external application..."`), replace:

```cpp
            // Force mode restoration (RetroArch might have changed resolution)
            if (!display.set_mode(mode.width, mode.height)) {
                std::cerr << "Warning: Failed to restore display mode: " << mode.width << "x" << mode.height << std::endl;
            } else {
                std::cout << "Restored display mode: " << mode.width << "x" << mode.height << std::endl;
            }
```

with:

```cpp
            // Force mode restoration (RetroArch might have changed resolution)
            // — unless the game-exit path already did it, in which case a
            // second set_mode here would make the TV resync twice.
            if (state.display_mode_restored.exchange(false)) {
                std::cout << "Display mode already restored by game-exit path; skipping set_mode" << std::endl;
            } else if (!display.set_mode(mode.width, mode.height)) {
                std::cerr << "Warning: Failed to restore display mode: " << mode.width << "x" << mode.height << std::endl;
            } else {
                std::cout << "Restored display mode: " << mode.width << "x" << mode.height << std::endl;
            }
```

Everything else in the handler (`frame_ctx.reset()`, `egl.make_current()`, `gst_renderer.reset_gl()`) stays unconditional and untouched.

- [ ] **Step 6: Run the Mac suite (compile + regression gate)**

```bash
cd "magic_dingus_box_cpp" && cmake --build build-test --target test_retroarch_unit -j8 && ./build-test/test_retroarch_unit
```

Expected: PASS. (`controller.cpp`/`main.cpp` aren't in the Mac target — the Pi compile gate is Task 5 — but `app_state.h` changes flow through here.)

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/app/controller.h magic_dingus_box_cpp/src/app/controller.cpp magic_dingus_box_cpp/src/main.cpp
git commit -m "fix: restore the kiosk's real boot mode after RetroArch, once"
```

---

### Task 3: Dissolve the frozen launch plate (Component 2)

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (delete `loading_is_exit`, ~line 400)
- Modify: `magic_dingus_box_cpp/src/app/controller.cpp` (exit sequence, ~lines 786–871)
- Modify: `magic_dingus_box_cpp/src/main.cpp` (game-session hooks, ~lines 1040–1090)
- Modify: `magic_dingus_box_cpp/src/ui/renderer.cpp` (`render_loading_overlay`, ~lines 2157–2260)

**Interfaces:**
- Consumes: `AppState::loading_alpha`, `app::prepare_loading_state_for_launch`, `app::return_dissolve_alpha` (Task 1).
- Produces: nothing new — deletes `AppState::loading_is_exit` and the green RETURNING plate.

- [ ] **Step 1: Delete `loading_is_exit`**

Remove from `src/app/app_state.h` the field AND its comment block (the comment beginning `// The same plate serves the RETURN from a game…` through `std::atomic<bool> loading_is_exit{false};`). The three fields added in Task 1 stay.

- [ ] **Step 2: Stop authoring the RETURN plate in `controller.cpp`**

Replace (immediately after `launch_game` returns, anchor: `"Flip the launch plate to its RETURN wording"`):

```cpp
        // Game has exited. Restore system.
        //
        // Flip the launch plate to its RETURN wording immediately — before the
        // display comes back — so that whatever gets scanned out during the
        // handover already says the right thing. The kiosk's own framebuffer
        // still holds the last frame it drew (a full bar reading "STARTING"),
        // and re-acquiring DRM master puts that straight back on screen.
        state.loading_is_exit.store(true);
        state.loading_progress.store(0.0f);
        state.loading_phase = "CLOSING GAME";
```

with:

```cpp
        // Game has exited. Restore system.
        //
        // Deliberately do NOT touch loading_progress / loading_phase here.
        // The kiosk's own framebuffer still holds the last frame it drew — a
        // full gold bar reading "STARTING" — and re-acquiring DRM master puts
        // that exact frame straight back on screen. The dissolve below fades
        // out THAT frame, so the state it renders from must stay identical to
        // the state it was drawn from: the first dissolve frame is then
        // pixel-identical to what is already on the panel, and the stale
        // frame reads as frame 1 of a deliberate fade instead of a hang.
```

- [ ] **Step 3: Replace the green progress plates with the dissolve loop**

In `controller.cpp`, inside the `if (display_)` re-acquire block, replace:

```cpp
            state.loading_progress.store(0.45f);
            state.loading_phase = "RESTORING DISPLAY";
            if (progress_callback) progress_callback();
```

with:

```cpp
            // First frames we may draw since the handover. MUST stay after
            // set_mode() — painting before it blocks on a page-flip event
            // that never arrives (measured: launches 4.3s -> 15.8s).
            //
            // Dissolve the launch plate to black. Frame 1 is drawn at alpha
            // 1.0 — pixel-identical to the stale frame the scanout already
            // picked up — so the transition starts from what is on the panel.
            // progress_callback ends in present_frame(), which blocks on the
            // page flip, so the loop is naturally paced at the refresh rate.
            if (progress_callback) {
                constexpr std::chrono::milliseconds kReturnDissolve{250};
                const auto dissolve_t0 = std::chrono::steady_clock::now();
                for (;;) {
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - dissolve_t0);
                    if (elapsed >= kReturnDissolve) break;
                    state.loading_alpha.store(app::return_dissolve_alpha(
                        static_cast<float>(elapsed.count()),
                        static_cast<float>(kReturnDissolve.count())));
                    progress_callback();
                }
                state.loading_alpha.store(0.0f);
                progress_callback();  // hold solid black for the restore work
            }
```

And **delete** the later block (anchor: after input init success):

```cpp
            if (input_initialized) {
                state.loading_progress.store(0.8f);
                state.loading_phase = "RESTORING CONTROLS";
                if (progress_callback) progress_callback();
            }
```

(keep the surrounding retry/error handling; only this progress-plate block goes).

Add `#include "game_launch_recovery.h"` to `controller.cpp`'s includes if not already present (it defines `return_dissolve_alpha`; `prepare_kiosk_state_after_game` is already called at the bottom of the exit path, so the include likely exists — verify).

- [ ] **Step 4: Update the game-session hooks in `main.cpp`**

Entry hook (anchor: `controller.set_game_session_hooks(`): replace

```cpp
        [&](const app::PlaylistItem& item) {
            state.is_loading_game = true;
```

with

```cpp
        [&](const app::PlaylistItem& item) {
            // Raises is_loading_game, resets loading_alpha to opaque, and
            // cancels any in-flight post-game fade — a stale alpha of 0 from
            // the previous exit would make this launch's plate invisible.
            app::prepare_loading_state_for_launch(state);
```

Exit hook: replace

```cpp
            // Reset loading state. loading_is_exit must clear too or the
            // NEXT launch would open on the return wording and the green
            // frame.
            state.is_loading_game = false;
            state.loading_is_exit.store(false);
            state.loading_progress.store(0.0f);
            state.loading_phase.clear();
```

with

```cpp
            // Reset loading state. loading_alpha is deliberately NOT reset
            // here — it is 0.0 (dissolved) and stays 0.0 until the next
            // launch's prepare_loading_state_for_launch, so nothing can
            // flash the plate between now and the menu fade-in.
            state.is_loading_game = false;
            state.loading_progress.store(0.0f);
            state.loading_phase.clear();
```

Add `#include "app/game_launch_recovery.h"` to `main.cpp`'s app includes.

- [ ] **Step 5: Apply `loading_alpha` in `render_loading_overlay`**

In `src/ui/renderer.cpp`:

1. At the top of the function, after `if (!state.is_loading_game) return;`, add:

```cpp
    // Return-dissolve alpha. 1.0 for the whole launch side; ramped to 0 by
    // the game-exit path. The caller clears to black first, so alpha 0 is
    // pure black — skip the draws entirely.
    const float a = std::min(1.0f, std::max(0.0f, state.loading_alpha.load()));
    if (a <= 0.004f) return;
```

2. Delete the two `exiting` lines:

```cpp
    const bool exiting = state.loading_is_exit.load();
    // Gold going in, green coming back — see the bar below.
    const ui::Color frame_color = exiting ? theme_->highlight1 : theme_->accent;
```

replacing with:

```cpp
    const ui::Color frame_color = theme_->accent;
```

3. Replace the eyebrow label selection:

```cpp
    const std::string label = exiting ? "RETURNING" : "NOW LOADING";
```

with:

```cpp
    const std::string label = "NOW LOADING";
```

4. Pass `a` as the trailing `alpha_multiplier` argument to **every** draw call in the function — both `draw_quad(..., a)` and `draw_text(..., a)` overloads already take it (`renderer.h:460-461`). That is: the fullscreen dim quad, the panel quad, the four hairline edge quads, the eyebrow text, the title text, the system text, the bar background quad, the bar segment quads in the loop, and the phase text. Example for the first two:

```cpp
    draw_quad(0.0f, 0.0f, w, h, ui::Color(0, 0, 0, 220), a);
    ...
    draw_quad(panel_x, panel_y, panel_w, panel_h, theme_->bg_lift, a);
```

and for text (the existing calls pass `false` for `use_title_font` — append `a`):

```cpp
    draw_text(label, inner_x, panel_y + panel_h * 0.20f, label_size,
              theme_->dim, false, a);
```

5. Update the bar-colour comment (it references green): replace

```cpp
    // Gold going in, green coming back. The colour alone tells you which
    // direction the box is moving without reading a word of it, which matters
    // because both plates can be on screen only briefly.
    const ui::Color bar_color = frame_color;
```

with

```cpp
    const ui::Color bar_color = frame_color;
```

- [ ] **Step 6: Verify no reference to `loading_is_exit` survives**

```bash
cd "magic_dingus_box_cpp" && grep -rn "loading_is_exit" src tests
```

Expected: no output.

- [ ] **Step 7: Run the Mac suite**

```bash
cd "magic_dingus_box_cpp" && cmake --build build-test --target test_retroarch_unit -j8 && ./build-test/test_retroarch_unit
```

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add magic_dingus_box_cpp/src/app/app_state.h magic_dingus_box_cpp/src/app/controller.cpp magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/src/ui/renderer.cpp
git commit -m "feat: dissolve the launch plate to black on RetroArch exit"
```

---

### Task 4: Fade the menu up from black (Component 3)

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/renderer.h` (declaration next to `render_bezel()`, ~line 241)
- Modify: `magic_dingus_box_cpp/src/ui/renderer.cpp` (implementation next to `render_bezel()`, ~line 875)
- Modify: `magic_dingus_box_cpp/src/main.cpp` (exit hook ~line 1085; render loop after `end_scene_fbo_and_composite`, ~line 3600)

**Interfaces:**
- Consumes: `AppState::post_game_fade_start_ms` (Task 1).
- Produces: `void Renderer::render_post_game_fade(float alpha)`.

- [ ] **Step 1: Declare in `renderer.h`**

Below `void render_bezel();`:

```cpp
    // Fullscreen black quad at `alpha` — the menu fade-up after a game.
    // Fullscreen like render_bezel (original_width_/original_height_, not the
    // possibly-letterboxed content viewport); caller sets glViewport first.
    void render_post_game_fade(float alpha);
```

- [ ] **Step 2: Implement in `renderer.cpp`**

Directly after `render_bezel()`'s closing brace:

```cpp
void Renderer::render_post_game_fade(float alpha) {
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    // Same fullscreen idiom as render_bezel: bind the UI shader and use the
    // ORIGINAL screen dimensions, not the content-viewport ones.
    glUseProgram(shader_program_);
    const float w = static_cast<float>(original_width_);
    const float h = static_cast<float>(original_height_);
    glUniform2f(u_screen_size_loc_, w, h);

    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        w,    0.0f, 1.0f, 0.0f,
        0.0f, h,    0.0f, 1.0f,
        w,    h,    1.0f, 1.0f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glUniform4f(u_color_loc_, 0.0f, 0.0f, 0.0f, alpha);
    glUniform1i(u_use_texture_loc_, 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}
```

- [ ] **Step 3: Start the fade clock in the exit hook**

In `main.cpp`'s game-session exit hook, directly after the loading-state reset from Task 3 Step 4, add:

```cpp
            // Start the menu fade-up from black. The dissolve left the panel
            // solid black; the render loop draws a black quad over the UI at
            // decreasing alpha (under the bezel — the bezel stays solid
            // through the whole round trip) until the fade window elapses.
            state.post_game_fade_start_ms.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
```

- [ ] **Step 4: Render the fade in the main loop**

In `main.cpp`, immediately **after** the `end_scene_fbo_and_composite` block:

```cpp
        if (scene_fbo_used) {
            ui_renderer.end_scene_fbo_and_composite(state);
        }
```

and **before** the bezel block (anchor: `// Render bezel overlay LAST in Modern TV mode`), insert:

```cpp
        // Post-game fade-up: black quad over the freshly rebuilt menu,
        // decreasing alpha over kPostGameFadeMs. Drawn UNDER the bezel so
        // the bezel stays solid across the dissolve AND this fade — one
        // continuous element bridging the whole game round trip. A separate
        // mechanism from is_fading/ui_overlay_alpha on purpose: that path is
        // entangled with video-active and UI-visibility semantics and early-
        // returns on is_transitioning, which prepare_kiosk_state_after_game
        // only incidentally avoids.
        {
            const int64_t fade_start = state.post_game_fade_start_ms.load();
            if (fade_start != 0) {
                constexpr int64_t kPostGameFadeMs = 250;
                const int64_t now_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                const int64_t elapsed = now_ms - fade_start;
                if (elapsed >= kPostGameFadeMs) {
                    state.post_game_fade_start_ms.store(0);
                } else {
                    glViewport(0, 0, mode.width, mode.height);
                    ui_renderer.render_post_game_fade(
                        1.0f - static_cast<float>(elapsed) /
                                   static_cast<float>(kPostGameFadeMs));
                }
            }
        }
```

- [ ] **Step 5: Run the Mac suite**

```bash
cd "magic_dingus_box_cpp" && cmake --build build-test --target test_retroarch_unit -j8 && ./build-test/test_retroarch_unit
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/renderer.h magic_dingus_box_cpp/src/ui/renderer.cpp magic_dingus_box_cpp/src/main.cpp
git commit -m "feat: fade the menu up from black after a game exits"
```

---

### Task 5: Full test sweep, CHANGELOG, Pi build + hardware verification

**Files:**
- Modify: `CHANGELOG.md` (repo root, `[Unreleased]` section)
- No source changes expected; fixes discovered on hardware get their own commits.

**Interfaces:**
- Consumes: everything above, deployed to the bench Pi 5 (`magic@magicpi.local`, service `magic-dingus-box-cpp.service`).

- [ ] **Step 1: Run ALL Mac suites**

```bash
cd "magic_dingus_box_cpp" && cmake --build build-test -j8 && ctest --test-dir build-test --output-on-failure
```

Expected: all suites PASS.

- [ ] **Step 2: CHANGELOG entry**

Under `## [Unreleased]`, add (create a `### Fixed` heading if one doesn't exist yet):

```markdown
### Fixed
- **Graceful return from RetroArch.** Exiting any game no longer replays the
  gold "NOW LOADING" frame, black-screens the TV twice, or flashes a green
  progress plate. The exit path now restores the kiosk's real boot mode
  directly (the old code forced 640x480 and the main loop immediately set it
  back — two HDMI resyncs per exit), dissolves the frozen launch plate to
  black over 250ms, does the input/audio restore work under black, and fades
  the menu up from black. The bezel stays solid through the whole round trip.
  Launch-side visuals are unchanged.
```

- [ ] **Step 3: Deploy + Pi compile gate**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Expected: rsync + clean Pi-side compile. This is the first time `controller.cpp` / `main.cpp` / `renderer.cpp` changes compile (they're not in the Mac targets) — fix any errors here and amend the responsible task's commit.

```bash
ssh magic@magicpi.local "sudo systemctl restart magic-dingus-box-cpp.service"
```

- [ ] **Step 4: Hardware verification — Modern TV mode (1920x1080)**

One launch-and-exit per core family: **PS1** (`pcsx_rearmed` — the Vulkan case `disable_crtc=false` exists for), **N64** (`mupen64plus_next`), **Dreamcast** (`flycast` — 4KB-page kernel, drives its own internal resolution). For each, from Settings → Video Games: launch, play ~30s, exit via Z+Start → Quit. Confirm:

- [ ] Launch side unchanged: gold plate, phase-stepped bar, `STARTING` at full bar.
- [ ] On exit: stale gold plate holds (expected, ~1s), then **one** TV resync, then the gold plate visibly dissolves to black — **no green at any point**.
- [ ] Black holds while controls restore; menu then fades up rather than popping.
- [ ] Bezel never flickers through the whole round trip.
- [ ] Controller works in the menu afterwards (input re-init unharmed).
- [ ] Exit wall-clock not worse than baseline (~3.4s from game quit to menu).

Log assertions:

```bash
ssh magic@magicpi.local "journalctl -u magic-dingus-box-cpp.service --since '10 min ago' | grep -E 'Restoring kiosk display mode|skipping set_mode|Falling back to 640x480'"
```

Expected: one `Restoring kiosk display mode 1920x1080` + one `skipping set_mode` per exit; **zero** `Falling back to 640x480`.

- [ ] **Step 5: Hardware verification — CRT_NATIVE mode (1280x720)**

Settings → Display → CRT Native (restart if the resolution change is deferred), then repeat one launch/exit (PS1 is enough). Expected: `Restoring kiosk display mode 1280x720` in the journal, same visual sequence. Then toggle back to Modern TV, **without restarting**, and do one more exit — this validates the runtime re-sync of `set_kiosk_display_mode` at the display-toggle site.

- [ ] **Step 6: Acceptance script**

```bash
ssh magic@magicpi.local "sudo /opt/magic_dingus_box/scripts/verify_box.sh"
```

Expected: exit 0; in particular the display-mode-vs-persisted-setting check passes after a game exit.

- [ ] **Step 7: Commit + wrap up**

```bash
git add CHANGELOG.md
git commit -m "docs: changelog entry for graceful RetroArch exit"
```

Then use superpowers:finishing-a-development-branch (Pi 4B spot-check before any release per the dual-board contract — no board-specific code here, but both boards ship the binary).

---

## Self-review notes

- **Spec coverage:** Component 1 → Task 2; Component 2 → Tasks 1+3; Component 3 → Tasks 1+4; off-Pi tests → Task 1; hardware matrix (3 cores × 2 display modes, runtime toggle) → Task 5; settle-sleep named constant → Task 2 Step 2; `loading_is_exit` deletion → Task 3.
- **Deviation from spec, deliberate:** the spec's illustrative `fade()` colour-helper is replaced by the existing `alpha_multiplier` trailing parameters on `draw_quad`/`draw_text` — same effect, no new helper (DRY).
- **Type consistency:** `loading_alpha`/`display_mode_restored`/`post_game_fade_start_ms` names identical across Tasks 1–4; `return_dissolve_alpha(float, float)` and `prepare_loading_state_for_launch(AppState&)` match between Task 1 definitions and Task 3 call sites; `set_kiosk_display_mode(uint32_t, uint32_t)` matches between Task 2 header and both main.cpp call sites.
