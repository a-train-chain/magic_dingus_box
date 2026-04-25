# Movie Playback in the Media Browser

**Status:** approved design — ready for implementation plan.
**Date:** 2026-04-25.
**Owner:** Alex.

## Problem

The Media Browser's `DetailScreen` shows a "Play" button when a movie's
been downloaded (`hasFile == true`). Today that button is a stub — it
returns `Screen::Library` and nothing else happens. Users who have
successfully added and downloaded a movie can't actually watch it from
the kiosk.

The fix needs to:

- Play the downloaded movie file using the same GStreamer pipeline the
  main kiosk UI already uses for playlist videos.
- Reuse the main UI's input vocabulary so muscle memory carries over —
  PLAY/PAUSE, ±10s with PREV/NEXT, ±5s with the C-stick, velocity-based
  rotary-encoder seek, BTN4 to stop.
- Return the user to the same `DetailScreen` they came from when
  playback ends or the user stops it.
- Translate Radarr's container-internal paths (`/library/...`) into the
  Pi host paths GStreamer expects (`/mnt/ssd/library/...`).

## Approach: dedicated `PlaybackScreen` inside the Media Browser

A new `MbScreen` implementation, `PlaybackScreen`, takes the existing
slot in the Media Browser screen state machine alongside Browse / Search
/ Detail / Queue / Library / MovieSettings. It borrows references to the
existing `Controller` and `AppState` rather than owning a player — there
is exactly one GStreamer pipeline in the kiosk and PlaybackScreen drives
it the same way the main UI does.

This was chosen over a "phantom playlist" approach (which would have
mutated `state.playlists` to inject the movie as a synthetic playlist
item) because it keeps lifecycle clean: the screen-level state machine
already owns enter/leave semantics, error recovery happens in one place,
and there's no risk of leaving stale data in `state.playlists` if the
kiosk crashes mid-playback.

## Architecture

### New files

- `src/media_browser/ui/playback_screen.h`
- `src/media_browser/ui/playback_screen.cpp`

### Modified files

- `src/media_browser/ui/mb_screen.h` — add `Screen::Playback` enum value.
- `src/media_browser/ui/detail_screen.{h,cpp}`:
  - `do_play()` becomes real: pre-flight `std::filesystem::exists`
    check, on failure show banner and stay; on success return
    `Screen::Playback`.
  - New method `PlayTarget get_play_target() const` returns
    `{host_path, title}` for the currently-loaded movie. Used by the
    main.cpp dispatcher during the Detail→Playback transition (same
    handoff pattern Browse/Search/Library use to forward
    `selected_tmdb_id` into Detail). `host_path` is computed by calling
    `radarr_.resolve_host_path(...)` on the Radarr movie file path.
  - No constructor change.
- `src/media_browser/radarr/radarr_client.{h,cpp}` — add
  `resolve_host_path(...)` member function, plus two new fields on
  `Config` (`container_library_prefix`, `host_library_prefix`).
- `src/main.cpp`:
  - Construct `PlaybackScreen mb_playback(controller, state);`
  - Add to dispatcher's screen-transition switch.
  - Set the two new RadarrClient `Config` fields from env vars
    (`MDB_HOST_LIBRARY_PREFIX`, `MDB_CONTAINER_LIBRARY_PREFIX`) with
    sensible defaults.
- `tests/media_browser/test_radarr_client.cpp` — add
  `resolve_host_path` test cases.
- `magic_dingus_box_cpp/CMakeLists.txt` — add the new source file to
  `KIOSK_MEDIA_BROWSER_SOURCES`.

### `PlaybackScreen` interface

```cpp
class PlaybackScreen : public MbScreen {
public:
    PlaybackScreen(app::Controller& controller, app::AppState& state);

    // Caller (DetailScreen via main.cpp dispatcher) sets these BEFORE
    // returning Screen::Playback, the same handoff pattern the
    // dispatcher uses for set_tmdb_id / set_origin on DetailScreen.
    void set_movie(std::string host_path, std::string title);

    void enter() override;
    void leave() override;
    Screen handle_input(const std::vector<platform::InputEvent>&) override;
    void update() override;
    void render(::ui::Renderer& r, int w, int h) override;

private:
    app::Controller& controller_;
    app::AppState&   state_;

    std::string movie_title_;
    std::string movie_path_;          // host-side path

    bool was_video_active_ = false;   // edge-detect playback-ended
    bool exit_pending_ = false;       // arm exit on next handle_input
    std::string deferred_toast_;      // surfaced on Detail after exit

    std::chrono::steady_clock::time_point title_marquee_until_{};
};
```

### Lifecycle

| Event | Behavior |
|---|---|
| `set_movie(path, title)` | Stash for `enter()`. Idempotent — last setter wins. |
| `enter()` | `controller.load_file_with_resolution(movie_path_, "", 0, 0, false)`. On error, arm `exit_pending_` and set `deferred_toast_`. On success, `controller.play()`, set `was_video_active_ = true`, arm 3-second title marquee. |
| `update()` | Edge-detect `was_video_active_ && !state.video_active` → set `exit_pending_` (playback ended naturally). |
| `handle_input(events)` | If `exit_pending_`, return `Screen::Detail`. Otherwise route inputs to controller (table below). BTN4 → `controller.stop()` → return `Screen::Detail`. |
| `leave()` | `controller.stop()` (idempotent — protects against any exit path). If `deferred_toast_` is non-empty, push it via `::ui::Toast::show(...)` so it surfaces on Detail. |
| `render(r, w, h)` | Title marquee (3s with fade), pause indicator if paused, persistent BTN4 hint bottom-right. The video frame draws via the existing `state.video_active`-gated path in the main render loop — PlaybackScreen never draws video itself. |

### Dispatcher wiring (main.cpp)

```cpp
// near line 522 where other Mb screens are constructed:
media_browser::ui::PlaybackScreen mb_playback(controller, state);

// In the screen-transition switch (~line 1366):
case media_browser::ui::Screen::Playback: active_mb_screen = &mb_playback; break;

// In the special-handoff block (~line 1350) where the dispatcher
// forwards selected_tmdb_id from source screen to Detail, add a parallel
// handoff that sets movie path/title on Playback before transition:
if (next == media_browser::ui::Screen::Playback &&
    current_mb_screen == media_browser::ui::Screen::Detail) {
    auto path_title = mb_detail.get_play_target();
    mb_playback.set_movie(path_title.host_path, path_title.title);
}
```

DetailScreen exposes a `get_play_target()` method that returns
`{host_path, title}` for the currently-loaded movie (using
`radarr_.resolve_host_path(...)` internally on the movie file path).

## Path translation

Radarr returns container-internal paths like
`/library/Sintel (2010)/Sintel (2010) [720p] [BluRay] [YTS.MX].mp4`
because that's what its bind-mounted volume looks like from inside the
container. The kiosk runs on the host, where the same file lives at
`/mnt/ssd/library/Sintel (2010)/...`. Translation belongs at the
RadarrClient boundary — it's the only thing producing container paths.

### Algorithm

```cpp
std::string RadarrClient::resolve_host_path(const std::string& container_path) const {
    // Both prefixes normalized to end in '/' in Config setter, so the
    // string comparison is unambiguous (won't match /library2/...).
    if (container_path.rfind(cfg_.container_library_prefix, 0) == 0) {
        return cfg_.host_library_prefix +
               container_path.substr(cfg_.container_library_prefix.size());
    }
    spdlog::warn("[radarr] resolve_host_path: '{}' does not match prefix '{}'; "
                 "passing through unchanged",
                 container_path, cfg_.container_library_prefix);
    return container_path;
}
```

### Config defaults and env overrides

```cpp
struct RadarrClient::Config {
    // ... existing fields ...
    std::string container_library_prefix = "/library/";
    std::string host_library_prefix      = "/mnt/ssd/library/";
};
```

main.cpp populates from env if set:

```cpp
if (const char* p = std::getenv("MDB_CONTAINER_LIBRARY_PREFIX")) {
    radarr_cfg.container_library_prefix = ensure_trailing_slash(p);
}
if (const char* p = std::getenv("MDB_HOST_LIBRARY_PREFIX")) {
    radarr_cfg.host_library_prefix = ensure_trailing_slash(p);
}
```

`ensure_trailing_slash` is a tiny anonymous-namespace helper in main.cpp.
The `Config` *constructor* (or a setter) ALSO normalizes — defense in
depth — so callers that build a Config struct programmatically don't
have to remember.

## Controls

Same vocabulary as main UI. Source: `main.cpp` lines 1937–2008.

| Input | Action |
|---|---|
| `PLAY_PAUSE` | `controller.toggle_pause()` |
| `NEXT` | `controller.seek(+10)` |
| `PREV` | `controller.seek(-10)` |
| `SEEK_RIGHT` | `controller.seek(+5)`; `state.show_seek_bar = true; state.seek_bar_timer = 1.5;` |
| `SEEK_LEFT`  | `controller.seek(-5)`; same seek-bar trigger |
| `ROTATE` (delta + velocity) | `controller.seek((5 + 25*v*v) * delta);` same seek-bar trigger. Exact formula reused from main.cpp:1758. |
| `SETTINGS_MENU` | `controller.stop()` → return `Screen::Detail` |
| `QUIT` | `running = false` at dispatcher level — unchanged |

The 500 ms BTN4 long-press → `Screen::Exit` → MainMenu handler at the
dispatcher level continues to win over our short-press handler;
PlaybackScreen never sees the event when it goes long.

## Error handling

| Failure | Behavior |
|---|---|
| `hasFile == false` on movie record | "Play" button never shown by DetailScreen — same as today. Unreachable. |
| `movie_file.path` missing on host (`std::filesystem::exists` false) | DetailScreen banner: `"File missing on disk"`. No transition. |
| GStreamer load failure (`load_file_with_resolution` returns error) | `enter()` arms `exit_pending_` + sets `deferred_toast_ = "Playback failed: <reason>"`. Next `handle_input` returns `Screen::Detail`. `leave()` surfaces the toast. |
| Playback ends naturally | `update()` edge-detects `state.video_active` true→false → `exit_pending_`. Next `handle_input` returns `Screen::Detail`. |
| User holds BTN4 long | Dispatcher-level long-press wins → `Screen::Exit` → MainMenu. `leave()` calls `controller.stop()`. |
| Any unexpected exit path | `leave()` always calls `controller.stop()` (idempotent). Player ends in a known state. |

## HUD overlay

- **Title marquee** — top-left, Zen Dots `font_heading_size`, `accent2`,
  `"NOW PLAYING — Sintel"` for 3 seconds on entry, linear fade-out over
  the last 500 ms. Matches Detail's "FEATURE PRESENTATION" header
  vocabulary.
- **Seek bar** — reuse the existing main-UI render
  (`state.show_seek_bar` + `state.seek_bar_timer`). PlaybackScreen sets
  these on seek events; the kiosk overlay path renders them. No
  duplication.
- **Pause indicator** — when `controller.is_paused()`, draw `"PAUSED"`
  bottom-center, `font_medium`, `th.dim`, alpha 0.85. Persistent until
  unpause.
- **Persistent control hint** — bottom-right,
  `"BTN4: stop   PLAY: pause   ROTATE: seek"`, `font_small`, `th.dim`
  0.7. Mirrors the rest of the Media Browser footer style.
- **Nothing else.** No play/pause toasts, no buffering spinner (defer
  unless testing surfaces a need).

## Testing

### Unit

`tests/media_browser/test_radarr_client.cpp` adds a `resolve_host_path`
test exercising:

1. Exact prefix match → translated correctly.
2. Edge case: `/library2/foo.mp4` does NOT match `/library/`. Passes
   through unchanged. (Trailing-slash normalization defense.)
3. Path that doesn't start with the prefix at all → passes through.
4. Empty string → empty result.

### Manual smoke (on Pi)

1. Library → Sintel → Detail → confirm "Play" button shows.
2. Hit Play → confirm:
   - Video starts within ~1 second.
   - Title marquee `"NOW PLAYING — Sintel"` shows for 3 seconds.
   - Persistent BTN4 hint at bottom-right.
3. Hit PLAY/PAUSE → "PAUSED" overlay appears. Hit again → resumes.
4. Rotary encoder → seek bar shows, position changes; verify both
   directions, slow and fast.
5. NEXT/PREV → ±10s, seek bar shows.
6. C-stick L/R → ±5s, seek bar shows.
7. BTN4 short-press → returns to Detail screen for Sintel. Player
   stopped.
8. Long-press BTN4 mid-playback → kiosk MainMenu (existing behavior).
   Re-enter Movies → Library still shows Sintel correctly.
9. Repeat steps 1–7 a second time to confirm idempotent.
10. (Optional) Manually corrupt the file path on disk → hit Play →
    confirm `"File missing on disk"` banner without crash.

### Out of scope

- No new GStreamer integration test on dev Mac. The translation logic
  (the genuinely tricky bit) gets unit coverage. The GStreamer bridge
  is the same code path the main UI exercises continuously.
- No multi-movie auto-advance / loop / repeat. Future work.
- No subtitle handling. Future work.
- No "resume from where you left off" position-saving. Future work.

## Build / deploy

`ENABLE_MEDIA_BROWSER=ON` already includes everything in the
`KIOSK_MEDIA_BROWSER_SOURCES` CMake list. Adding `playback_screen.cpp`
to that list is sufficient. No new dependencies, no compose changes, no
runtime config changes (env vars have sensible defaults).

`ENABLE_MEDIA_BROWSER=OFF` continues to produce a kiosk binary
unchanged — PlaybackScreen lives entirely behind the gate.
