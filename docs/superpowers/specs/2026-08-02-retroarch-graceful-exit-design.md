# Graceful return from RetroArch — design

**Date:** 2026-08-02
**Status:** approved (design), not yet implemented
**Scope:** the shared exit path in `Controller::load_playlist_item`, covering every launch route and every emulated console

## Problem

Exiting a game does not return to the kiosk UI gracefully. What the operator sees:

1. The **gold launch plate reappears** — bar full, phase reading `STARTING` — as if
   the box were launching the game a second time.
2. The screen goes black and the TV resyncs, **twice**.
3. A **green plate flashes** too briefly to read as anything but a glitch.
4. The menu **pops in** at full brightness with no transition.

Three independent defects are stacked here.

### Defect 1 — the frozen frame lies

The kiosk can only draw until it hands DRM master to RetroArch. The last frame it
presents stays on the panel until RetroArch takes over, and — because
`release_master(disable_crtc = false)` deliberately leaves the CRTC enabled for
Vulkan-core compatibility — the scanout **reverts to that same kiosk framebuffer**
when RetroArch dies.

So one framebuffer serves two jobs: "RetroArch is starting" (held ~2.5s going in)
and "RetroArch just quit" (held ~2s coming out). It is authored only for the
first, which is why it lies on the way back. `app_state.h` documents the symptom;
the existing `loading_is_exit` green plate is an attempt to overwrite it after the
fact rather than to make the held frame correct.

### Defect 2 — a spurious mode round-trip (the real bug)

`controller.cpp:830` hardcodes:

```cpp
display_->set_mode(640, 480);
```

**No unit ever boots at 640x480.** `config::display::target_drm_mode()` returns
1280x720 for `CRT_NATIVE` and 1920x1080 for `MODERN_TV`; 640x480 is only the
last-resort fallback in `main.cpp`'s boot cascade. So every game exit forces the
panel down to 640x480, and then `prepare_kiosk_state_after_game` sets
`reset_display`, and `main.cpp:1640` sets the mode straight back to the boot mode.

Two HDMI mode changes per exit. On a TV that is two black-screen resyncs with the
set's own OSD popping up over them.

The green plate is painted in between — at 640x480, through a GBM/EGL surface
still sized for 720p/1080p, and before the main loop has run `frame_ctx.reset()`
and `egl.make_current()`. That mismatch is what makes the green flash look wrong
as well as brief.

### Defect 3 — no transition at either end

The dissolve out and the fade in do not exist. The green plate is a status
readout, not a transition, and the menu appears by simply being drawn.

### Measured timeline (Modern TV unit, before)

| when | panel shows |
|---|---|
| RetroArch quits | CRTC reverts to the kiosk's stale FB → **gold plate, bar full, `STARTING`** |
| +0–1000ms | fixed `sleep(1000)` settle — still gold |
| +1000ms | `acquire_master()` (usually first try) |
| +1000ms | `set_mode(640,480)` → **mode change #1**, black + TV OSD |
| +~1500ms | green `RESTORING DISPLAY` plate, wrong mode, stale EGL/frame ctx |
| +~1500–2500ms | input re-init blocks — green frozen |
| +~2500ms | green `RESTORING CONTROLS` |
| +~2600ms | `reset_display` → `set_mode(1920,1080)` → **mode change #2**, black + OSD |
| then | menu pops in at full brightness |

## Goals

- The return reads as one deliberate transition rather than three glitches.
- **The launch experience is unchanged.** The gold phase-stepped plate, its
  wording, its colour, and its behaviour while frozen all stay exactly as they
  are. This was an explicit constraint from the operator.
- One HDMI mode change per exit, to the correct resolution.
- Works for every console and every launch route without per-core special-casing.

## Non-goals

- **Not touching the fixed `sleep(1000)` settle delay.** It is load-bearing for
  DRM release and was tuned on hardware; replacing it with a bounded poll is a
  worthwhile follow-on (see *Follow-on*) but must be verified separately so a
  timing regression can never be confused with a rendering one. This spec only
  lifts it into a named constant so the swap is a one-place change later.
- Not changing where the operator lands after a game. `settings_menu.close()`
  returning to the playlist UI is existing, intended behaviour.
- Not eliminating the stale gold frame entirely. It is a hardware consequence of
  the DRM handover and cannot be drawn over while the kiosk has no master. The
  design makes it *mean something* instead of making it disappear.
- Not reusing the existing `is_fading` / `ui_overlay_alpha` machinery — see
  *Component 3* for why.
- No `PlatformProfile` branch. Every change here is DRM/render logic identical on
  Pi 4B and Pi 5, so the dual-board contract needs nothing beyond both boards
  being exercised at verification time.

## Design

Everything lands in the single shared exit path inside
`Controller::load_playlist_item`. Per the comment at `main.cpp:1030`, that path is
common to all five launch routes — main-UI `SELECT` on a mixed playlist,
`NEXT`/`PREV`, auto-advance at video end, Master Shuffle, and the Settings game
browser. The exit sequence itself (`pkill -9 retroarch` plus the DRM dance) is
core-agnostic, so NES through Dreamcast are all covered by one fix.

### Component 1 — restore the mode the kiosk actually booted with

`Controller` gains:

```cpp
void set_kiosk_display_mode(uint32_t w, uint32_t h);
```

storing `kiosk_mode_w_` / `kiosk_mode_h_`. It is called from `main.cpp` in **two**
places:

- `main.cpp:1495`, right after `mode = display.get_current_mode()` finalises the
  boot mode;
- `main.cpp:1761`, in the `CRT_NATIVE` ↔ `MODERN_TV` toggle, right after the same
  assignment — otherwise the stored value goes stale the first time an operator
  changes display mode at runtime.

`controller.cpp:830` then becomes:

```cpp
// RetroArch leaves the panel on its own mode, so ONE set_mode is required
// here. Restore the mode the kiosk actually booted with — NOT 640x480. No
// unit boots at 640x480 (target_drm_mode gives 1280x720 / 1920x1080; 640x480
// is only the boot cascade's last resort), so hardcoding it made every game
// exit do TWO mode changes and the TV resync twice.
bool restored = false;
if (kiosk_mode_w_ > 0 && kiosk_mode_h_ > 0) {
    restored = display_->set_mode(kiosk_mode_w_, kiosk_mode_h_);
}
if (!restored) {
    restored = display_->set_mode(640, 480);   // preserve the old floor
}
state.display_mode_restored.store(restored);
```

The `reset_display` handler at `main.cpp:1631` consumes the flag:

```cpp
if (state.display_mode_restored.exchange(false)) {
    // The game-exit path already put the panel on mode.width x mode.height.
    // Re-setting it here would make the TV resync a second time.
} else if (!display.set_mode(mode.width, mode.height)) {
    // unchanged warning path
}
```

**Why an explicit flag rather than comparing `get_current_mode()`:**
`DrmDisplay::get_current_mode()` returns the cached `current_mode_`
(`drm_display.h:30`), which is only written by `set_connector_mode()`. RetroArch
changing the mode behind the kiosk's back never updates it, so the cache claims
the boot mode is still set when it is not. A comparison against it would wrongly
skip the restore in exactly the failure case where it matters most.

Everything else in the `reset_display` handler — `frame_ctx.reset()`,
`egl.make_current()`, `gst_renderer.reset_gl()` — is unconditional and unchanged.

### Component 2 — dissolve the frozen plate instead of replacing it

Delete `loading_is_exit` entirely: the field in `app_state.h:400`, the store at
`controller.cpp:788`, the reset at `main.cpp:1086`, and the `exiting` branch in
`render_loading_overlay` (`renderer.cpp:2191`, the `RETURNING` label at 2212, and
the green `frame_color` / `bar_color`). It exists only to draw the plate being
removed.

Add to `AppState`:

```cpp
// Global alpha for the launch plate, 1.0 = fully drawn.
//
// The plate is dissolved to 0 on the way back from a game. The frame the
// dissolve STARTS from is pixel-identical to the one already frozen on the
// panel (the kiosk's own stale framebuffer, which the scanout picks back up
// when RetroArch dies) — so the stale frame stops reading as a hang and
// becomes frame 1 of a deliberate fade. Must be reset to 1.0 before the next
// launch or that launch opens invisible.
std::atomic<float> loading_alpha{1.0f};
```

`render_loading_overlay` reads it once at the top and applies it uniformly:

```cpp
const float a = std::min(1.0f, std::max(0.0f, state.loading_alpha.load()));
auto fade = [a](ui::Color c) {
    c.a = static_cast<uint8_t>(c.a * a);
    return c;
};
```

Every `draw_quad` colour goes through `fade()`; every `draw_text` call passes `a`
as its trailing alpha argument (the parameter already exists — see the QR label
draws around `renderer.cpp:1415`). No geometry or layout changes.

The exit path, immediately after Component 1's `set_mode` succeeds, runs the
dissolve through the **existing** `progress_callback`:

```cpp
// First paint since the handover. Start at alpha 1.0 — identical to what is
// already on the panel — then fade to black over kReturnDissolve.
constexpr auto kReturnDissolve = std::chrono::milliseconds(250);
const auto t0 = std::chrono::steady_clock::now();
for (;;) {
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    if (elapsed >= kReturnDissolve) break;
    const float t = std::chrono::duration<float>(elapsed) /
                    std::chrono::duration<float>(kReturnDissolve);
    state.loading_alpha.store(1.0f - t);
    if (progress_callback) progress_callback();
}
state.loading_alpha.store(0.0f);
if (progress_callback) progress_callback();   // solid black
```

`progress_callback` (defined at `main.cpp:2538` and installed for every route)
already does `glClearColor(0,0,0,1)` + `glClear` before rendering the overlay, so
alpha 0 yields pure black. It also renders the bezel **after** the overlay at full
alpha, so the bezel stays solid through the whole dissolve.

The dissolve is drawn through the same code path that renders today's green plate,
at the same point in the sequence — so it carries the same risk profile, not a new
one. It must stay **after** `set_mode`: `controller.cpp:837` records that painting
before the mode is restored blocks on a page-flip event that never arrives, which
took launches from 4.3s to 15.8s.

The two green progress updates (`RESTORING DISPLAY` at 0.45, `RESTORING CONTROLS`
at 0.8) are removed. The remaining restore work — input re-init, `apply_output()`,
`player_->stop()` — then runs with black on screen.

`loading_alpha` is reset to 1.0 in the game-session **entry** hook
(`main.cpp:1040`), alongside `is_loading_game = true`, so the next launch always
starts opaque regardless of how the previous session ended. Resetting it only in
the exit hook would leave it at 0 after any path that bypasses the exit hook.

### Component 3 — fade the menu up from black

Add to `AppState`:

```cpp
// steady_clock time_since_epoch in ms, 0 = not fading.
std::atomic<int64_t> post_game_fade_start_ms{0};
```

Set in the game-session exit hook at `main.cpp:1080`, where `is_loading_game` is
already cleared.

Add to `Renderer`:

```cpp
// Fullscreen black quad at `alpha`, used to fade the UI up after a game.
// Reuses render_bezel()'s fullscreen viewport handling rather than draw_quad's
// logical-canvas coordinates, because the caller has just been rendering into
// a letterboxed content viewport.
void render_post_game_fade(float alpha);
```

Called in `main.cpp` **after** `end_scene_fbo_and_composite` (`main.cpp:3600`) and
**before** the bezel block, at alpha `1 - progress` over 250ms.

Drawing it under the bezel is deliberate: the bezel is at full alpha during the
dissolve and at full alpha during the fade-in, so it is one continuous element
bridging the entire transition and never flickers.

**Why not reuse `is_fading`:** that machinery (`renderer.cpp:1168-1190`) is
entangled with video-active and UI-visibility semantics, and `render()` early-
returns on `is_transitioning` at `renderer.cpp:1163`. It happens to work here only
because `prepare_kiosk_state_after_game` sets both playlist indices to -1 —
an incidental dependency, not a contract. A separate, narrowly-named mechanism
cannot be broken by future changes to video fading.

### Resulting timeline (Modern TV unit, after)

| when | panel shows |
|---|---|
| RetroArch quits | stale gold plate |
| +0–1000ms | `pkill` + settle sleep — still gold *(unchanged; see Follow-on)* |
| +1000ms | `set_mode` to the real boot mode — **the only** resync |
| +1000–1250ms | gold plate dissolves to black |
| +1250–2300ms | black while input / audio / player restore |
| +2300–2550ms | menu fades up from black, bezel solid throughout |

No green flash, no OSD churn, no frame that claims a game is launching.

## Testing

### Off-Pi (Mac suites, pure logic)

Extend `tests/retroarch/test_game_launch_recovery.cpp`:

- `display_mode_restored` round-trips and `exchange(false)` consumes it exactly
  once — a second read must not skip a genuinely needed `set_mode`.
- `prepare_kiosk_state_after_game` leaves the existing five fields unchanged
  (regression guard on the current test's assertions).

New `tests/ui/test_return_dissolve.cpp`:

- The alpha ramp is monotonically decreasing and clamped to [0, 1] for elapsed
  times from 0 through 2× the dissolve duration.
- `loading_alpha` is 1.0 after the entry hook's reset regardless of its prior
  value — the same class of bug the deleted `loading_is_exit` reset comment at
  `main.cpp:1082` warns about, where a stale flag makes the *next* launch open
  with the wrong visual.

### On hardware (Pi 5)

One launch-and-exit per core family, confirming: a single TV resync, no green
flash, no stale-gold hang, menu fades rather than pops, and exit wall-clock not
worse than the ~3.4s baseline.

- **PS1** (`pcsx_rearmed`) — the Vulkan-sensitive case that `disable_crtc = false`
  exists for.
- **N64** (`mupen64plus_next`).
- **Dreamcast** (`flycast`) — worth watching specifically, given its 4 KB-page
  kernel dependency and that it drives its own internal resolution.

Also verify on a `CRT_NATIVE` unit (1280x720) as well as a `MODERN_TV` unit
(1920x1080), since Component 1's whole point is that the restored mode differs
between them. Both boards should be exercised per the dual-board contract, though
no code path here branches on board.

`scripts/verify_box.sh` already checks display mode against the persisted setting;
running it after a game exit is a cheap confirmation that Component 1 landed.

## Follow-on (not in this spec)

Replace the fixed `sleep(1000)` settle with a bounded poll: `pkill`, wait until no
`retroarch` process remains, then retry `acquire_master()` every 25ms with a
~150ms floor and a 2500ms ceiling so the worst case can never be slower than
today. That would cut roughly 700–800ms of stale gold and is the single largest
remaining improvement to the return. Held back deliberately so this spec's
rendering changes can be verified on hardware in isolation first.
