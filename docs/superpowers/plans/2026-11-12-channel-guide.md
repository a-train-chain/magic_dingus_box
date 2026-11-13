# Channel Guide Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A second overlay style, Channel Guide, in which every video playlist is a live channel on a wall-clock schedule, shown as a cable-TV grid under a picture window, chosen in Settings → Display.

**Architecture:** Pure, Mac-tested units carry every decision — the schedule (`what is on channel C at time t`), the auto-tune rule, the guide cursor, and a view model that lays out the whole screen at both canvas sizes. `PlaylistPlayback` gains live tuning through its existing non-blocking switch; the renderer only paints the view model; main.cpp wires inputs, the picture window and the time-zone marker. The web admin back-fills video lengths and sets the box's time zone from the browser.

**Tech Stack:** C++17 kiosk (GLES, GStreamer, jsoncpp, Catch2), Python 3 Flask (pytest), classic browser JS (node-tested), bash.

**Spec:** `docs/superpowers/specs/2026-11-12-channel-guide-design.md` (and its dependency `2026-11-12-video-crop-toggle-design.md`). Mockups: the "Channel Guide Overlay" canvas, layout A.

**Branch:** `channel-guide`, cut from `crop-toggle` once `docs/superpowers/plans/2026-11-12-video-crop-toggle.md` is complete. This plan consumes from it: `app::MediaInfoMap` / `MediaInfoStore` (`cpp/src/app/media_info.h`), `web/media_info.py`, `web/media_probe.py` (`probe_source`), `GstRenderer::set_picture(IRect target_fb, bool crop, IRect opening_fb)`, `video::IRect`, `AppState::picture_control_available / picture_crop`, and `PlaylistTransport::toggle_picture_crop()`. Paths: `cpp/` = `magic_dingus_box_cpp/`, `web/` = `magic_dingus_box/web/`.

## Global Constraints

- Menu Style defaults to **List**; with List selected nothing changes.
- Schedule epoch: **2026-01-01T00:00:00Z = 1767225600 s**; never changes. Schedule maths in UTC seconds; display in local time.
- Channels = the List's rows after `split_for_ui_with_master_shuffle`; **00 = Master Shuffle**. Only `local` / `video` items are scheduled (`youtube` cannot load today).
- An item's scheduled length = `(end > 0 ? end : duration_s) − start`; unknown or ≤ 0 → left out.
- Every channel loops; the playlist's `loop:` key is ignored in Channel Guide style.
- Tuning (a press) closes the guide; channel up/down keeps it open with the cursor following; channel up/down wraps.
- The guide never closes by itself.
- Screen geometry (logical px, top-left origin) from the spec's table — TV 960×720: window x40 y40 384×288, panel x448 y40 472×288, grid x40 y352 w880 (header 30, rows 52, channel column 196), 20-min window, 5-min ticks; CRT 640×480: window x24 y24 224×168, panel x260 y24 356×168, grid x24 y206 w592 (header 24, rows 40, column 132), 15-min window, 5-min ticks; 5 rows visible on both; 3 px gaps between cells; text only in cells ≥ 30 px wide, artist only ≥ 64 px (TV); `◂ ` before a programme that began before the window.
- Colours are theme tokens only: `bg`, `bg_lift`, `fg`, `dim`, gold `accent` (cursor), green `highlight1` (tuned), steel `accent2` (time axis, NEXT), red `highlight2` (now line only). Zen Dots for clock, channel numbers, playlist titles; DejaVu Sans Mono elsewhere (advance width 0.602 em).
- No file I/O on the render loop (WatchdogSec=10). New `src/app`/`src/ui` `.cpp` files go in both the kiosk and the test source lists (`cpp/CMakeLists.txt:137-202` and `:767-863`). C++ suites with Media Browser ON and OFF; one build directory at a time, deleted after (~4 GB free). Kiosk-only files are syntax-checked on the Mac (stub `xf86drmMode.h`); CI's `kiosk-build` compiles them.
- Web: route modules never import `admin`; bump `?v=` for changed JS.
- Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

- A box whose clock reads earlier than the epoch (offline, before NTP) must still schedule sensibly — pinned in Task 2 (`on_air before the epoch wraps backwards`).
- A playlist edited while its channel is tuned must not leave stale indices in play — pinned in Task 4 (`a rebuilt channel list is used at the next boundary`).
- A channel whose items all lack a length (fresh upload before the back-fill reaches it) must show "No schedule yet", be skipped by channel up/down and by auto-tune — pinned in Task 2 (`empty channel`), Task 5 and Task 7.
- Every channel failing must not loop tunes — pinned in Task 5 (`all gave up → nothing`).
- During Master Shuffle the tuned channel is 00 even though the playing playlist is a source playlist — pinned in Task 4 and Task 7 (`shuffle tuned row is green`).

---

### Task 1: Back-fill video lengths and aspects

**Files:**
- Create: `web/media_backfill.py`
- Modify: `web/admin.py` (`create_app`: start the job once per process, beside the other start-up work)
- Test: `web/tests/test_media_backfill.py`

**Interfaces:**
- Consumes: `media_info.load/update/media_key`, `media_probe.probe_source`.
- Produces: `run_backfill(data_dir: Path, probe=probe_source, paused=<fn: bool>, wait=time.sleep, poke=<fn>) -> int` — returns how many entries it filled; `start_backfill_thread(data_dir: Path) -> threading.Thread` (daemon, `os.nice(10)` inside the thread). `paused` defaults to "the playback-pause marker `/tmp/mdb_playback_services_paused` exists" (games and movies).

- [ ] **Step 1: Failing tests:** two videos in `media/`, one already complete in the store → only the other is probed and filled (`duration_s`, `display_aspect`); a probe returning `None` is skipped and the run continues; while `paused()` is true the job waits (fake `wait` records calls) and does not probe; `poke` is called once at the end only when something was filled; a second run probes nothing.
- [ ] **Step 2: Run** `python3 -m pytest web/tests/test_media_backfill.py -q` → fail.
- [ ] **Step 3: Implement**, one file at a time.
- [ ] **Step 4: Run** → pass; the web suite shows only the known 16 failures.
- [ ] **Step 5: Commit** `web: back-fill every video's length and aspect into the media info store`.

---

### Task 2: The channel schedule

**Files:**
- Create: `cpp/src/app/channel_schedule.{h,cpp}` (both CMake lists)
- Test: `cpp/tests/app/test_channel_schedule.cpp` (tag `[channel_schedule]`)

**Interfaces:**
- Produces (namespace `app`):
  - `constexpr double kScheduleEpochUtc = 1767225600.0;`
  - `struct ScheduledItem { int playlist = 0; int item = 0; double length_s = 0; };` (`playlist` indexes `state.playlists`)
  - `struct Channel { int number = 0; std::vector<ScheduledItem> items; double total_s = 0; };`
  - `std::string media_key_of_item(const PlaylistItem&);` — lexical only: strip a leading `./` and `data/`, giving `media/x.mp4`
  - `std::vector<Channel> build_channels(const std::vector<Playlist>& ui_playlists, const MediaInfoMap& info);` — one channel per row; row 0 is the Master Shuffle mix
  - `struct OnAir { int index = 0; double offset_s = 0; double starts_at = 0; double ends_at = 0; };`
  - `std::optional<OnAir> on_air(const Channel&, double now_utc);` (`nullopt` for an empty channel)
  - `std::vector<OnAir> lineup(const Channel&, double from_utc, double to_utc);`
- Master Shuffle: every scheduled item of channels 1…N exactly once, ordered by a hand-written Fisher–Yates over `std::mt19937(0x4D4442)` (`j = rng() % (i + 1)`), NOT `std::shuffle` (its algorithm differs between libc++ and libstdc++, so the Mac and the Pi would disagree).

- [ ] **Step 1: Failing tests** (E = `kScheduleEpochUtc`; a channel of lengths 100, 200, 300 s):

```cpp
CHECK(on_air(c, E + 50)->index == 0);   CHECK(on_air(c, E + 50)->offset_s == Approx(50));
CHECK(on_air(c, E + 350)->index == 2);  CHECK(on_air(c, E + 350)->offset_s == Approx(50));
CHECK(on_air(c, E + 650)->starts_at == Approx(E + 600));            // loops
// on_air before the epoch wraps backwards
CHECK(on_air(c, E - 50)->index == 2);   CHECK(on_air(c, E - 50)->offset_s == Approx(250));
CHECK(on_air(c, E - 50)->starts_at == Approx(E - 300));
auto L = lineup(c, E + 50, E + 400);
REQUIRE(L.size() == 3); CHECK(L[1].starts_at == Approx(E + 100)); CHECK(L[2].index == 2);
// empty channel
CHECK_FALSE(on_air(Channel{}, E).has_value()); CHECK(lineup(Channel{}, E, E + 60).empty());
CHECK(media_key_of_item(item("data/media/a.mp4")) == "media/a.mp4");
CHECK(media_key_of_item(item("./media/a.mp4")) == "media/a.mp4");
```

  `build_channels`: an item with `start 10, end 70` is 60 s whatever its duration; no duration and `end == 0` → left out; `start >= end` → left out; `youtube` and `emulated_game` items → left out; channel numbers equal row indices; channel 0 holds every scheduled item of 1…N exactly once, the same order on two builds, and a different order after an item is added.
- [ ] **Step 2: Run** `test_app_unit "[channel_schedule]"` → fail.
- [ ] **Step 3: Implement** (`fmod` with a positive-result fix for negative offsets).
- [ ] **Step 4: Run** → pass (MB ON and OFF).
- [ ] **Step 5: Commit** `channels: live schedule anchored to 2026-01-01 UTC`.

---

### Task 3: Menu Style and last channel settings

**Files:**
- Modify: `cpp/src/app/app_state.h:425-659` (`DisplaySettings`), `cpp/src/app/settings_persistence.cpp:70-145, 368-459`, `cpp/tests/app/fixtures/settings.json`, `cpp/tests/app/test_settings_persistence.cpp:99-179`, `tests/local/settings_json_roundtrip.bats`, `cpp/src/ui/settings_menu.cpp:179-217`
- Test: those two test files plus `cpp/tests/ui/test_settings_menu_redraw.cpp` (or a sibling)

**Interfaces:**
- Produces: `enum class app::MenuStyle { List, ChannelGuide };` `DisplaySettings::menu_style = MenuStyle::List` (JSON `display.menu_style`: `"list"` | `"channel_guide"`, unknown → list); `AppState::last_channel_id` (JSON `playback.last_channel`, a `playlist_identity()` string, `"shuffle"` for channel 00, `""` = none).
- Settings → Display gains, right after the Mode row, `Menu Style: List` / `Menu Style: Channel Guide` with sublabel `List / Channel guide`; selecting it flips the value and calls `save_settings`.

- [ ] **Step 1: Failing tests:** round trip of both new fields; the fixture (with `"menu_style": "channel_guide"` and a `last_channel`) loads and re-saves byte-identical (MB build) — keep MB-OFF output deterministic too; a missing key loads as List / `""`; the bats type check accepts the new keys; the Display submenu has the row after Mode and selecting it flips the style and records one save.
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `test_app_unit "[settings]"`, `test_ui_unit`, the bats file → pass.
- [ ] **Step 5: Commit** `settings: Menu Style (List / Channel Guide) and the last channel watched`.

---

### Task 4: Live tuning in PlaylistPlayback

**Files:**
- Modify: `cpp/src/app/playlist_playback.{h,cpp}` (`PlaylistTransport` `:70-102`, `finish_switch` `:306-364`, `on_next`/`on_prev` `:204-250`, `tick_auto_advance` `:441-523`, `act_on_failed_item` `:41-63`), `cpp/src/app/controller_transport.h`, `cpp/src/app/controller.h:84`, `cpp/src/app/controller_playback.cpp:422-466`, `cpp/src/app/controller.cpp:113-150` (`load_item_media` offset), `cpp/src/app/app_state.h`
- Test: `cpp/tests/app/test_playlist_playback.cpp`, `cpp/tests/app/test_controller_playback.cpp`, `cpp/tests/app/controller_test_doubles.{h,cpp}` (`FakeVideoPlayer::load_file` records `start`)

**Interfaces:**
- Consumes: Task 2.
- Produces:
  - `Controller::load_playlist_item(..., std::function<void()> progress = nullptr, double start_offset_s = 0.0)`; `load_item_media` passes `item.start + start_offset_s` as the start.
  - `PlaylistTransport`: `virtual utils::Result<> load_playlist_item_at(AppState&, const Playlist&, int item, const std::string& dir, double offset_s) = 0;` and `virtual double wall_now_utc() const = 0;` (`ControllerTransport`: `std::chrono::system_clock`).
  - `PlaylistPlayback`: `void set_channels(const std::vector<Channel>* channels);` (`nullptr` = List style); `bool guide_style() const;` `void tune(int channel, bool keep_overlay);` `void on_channel_step(int delta);`
  - `AppState`: `int tuned_channel = -1;` `std::vector<int> guide_gave_up;` `double guide_wait_until_utc = 0;`
- Behaviour: `tune` uses the existing settle/poll switch (`pending_.target` = channel); when the load actually runs it takes `on_air(channel, wall_now_utc())` and calls `load_playlist_item_at` with that item and offset; success sets `tuned_channel`, `last_channel_id` (the source playlist's identity, or `"shuffle"` for 00) and clears `guide_gave_up`; the overlay is hidden only when `!keep_overlay`. `on_next` / `on_prev` call `on_channel_step(±1)` in guide style: next channel with a non-empty schedule, wrapping, `keep_overlay = ui_visible_when_playing`. At an item's natural end (today's `decide_auto_advance` decision) guide style loads `on_air(tuned, now)` instead of the next index. A failed item in guide style (the policy's `Advance`) stops playback and sets `guide_wait_until_utc` to that item's scheduled end, so the channel resumes with its next scheduled item instead of reloading the failing one (the failure still counts toward the budget). Give-up in guide style: `stop_to_menu` as today, then push `tuned_channel` onto `guide_gave_up` and set `tuned_channel = -1`. List style paths are unchanged.

- [ ] **Step 1: Failing tests** (fake wall clock on `FakeTransport`; channels built from `put_menu()` with known lengths): a tune at a known time records `load_at(<playlist>,<item>,<offset>)` only after the 200 ms settle, with the offset computed at load time (advance the wall clock during the settle and assert the later offset); tune hides the overlay; `on_next` with the overlay visible tunes channel+1 and leaves it visible; stepping past the last channel wraps to 00 and skips an empty channel; at an item's end the next load is the scheduled item at its offset; **a rebuilt channel list is used at the next boundary** (call `set_channels` with a shortened channel mid-item; the boundary loads from the new list); tuning 00 sets `tuned_channel == 0` while `current_playlist_index` is the source playlist; a give-up in guide style leaves `tuned_channel == -1` and `guide_gave_up == {c}`; an item that fails to load in guide style stops and sets `guide_wait_until_utc` to its scheduled end. Controller test: `load_playlist_item(..., 12.5)` on an item with `start 10` makes the player's recorded start `22.5`.
- [ ] **Step 2: Run** `test_app_unit "[playlist_playback],[controller_playback]"` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** (MB ON and OFF) → pass; syntax-check `controller.cpp`.
- [ ] **Step 5: Commit** `playback: tune channels live, step with PREV/NEXT, rejoin the schedule at each boundary`.

---

### Task 5: Re-tune whenever nothing is playing

**Files:**
- Create: `cpp/src/app/auto_tune.{h,cpp}` (both lists)
- Modify: `cpp/src/main.cpp` (evaluate once per frame after input handling), `cpp/src/app/post_game_gate.h` (a read-only `bool pending() const` if none exists)
- Test: `cpp/tests/app/test_auto_tune.cpp`

**Interfaces:**
- Consumes: Tasks 2, 4.
- Produces: `struct app::AutoTuneInput { bool guide_style, intro_complete, on_main_screen, video_active, switch_pending, loading_game, post_game_pending; double now_utc, wait_until_utc; int last_channel; std::vector<bool> tunable; std::vector<int> gave_up; };` and `std::optional<int> app::decide_auto_tune(const AutoTuneInput&);`
- Rule: all of `guide_style && intro_complete && on_main_screen && !video_active && !switch_pending && !loading_game && !post_game_pending && now_utc >= wait_until_utc`, else `nullopt`. Target: `last_channel` if tunable and not given up; else, after a give-up, the next tunable channel after the most recent given-up one (wrapping) that is not given up; else 1 if tunable; else the lowest tunable; else `nullopt`.
- main.cpp: resolve `last_channel_id` to a row with `find_playlist_by_identity` (`"shuffle"` → 0); `tunable[i] = !channels[i].items.empty()`; on a result call `playlist_playback.tune(c, /*keep_overlay=*/false)`. This single rule covers power-on (all four intro exits), the post-game ready edge, leaving the Media Browser and a give-up.

- [ ] **Step 1: Failing tests:** each gating flag alone blocks; a `wait_until_utc` in the future blocks and, once reached, the same channel is chosen; last channel chosen when tunable; no last channel → 1; channel 1 empty → lowest tunable; gave up on 3 with last = 3 → 4; gave up on the last row → wraps to 0 when tunable; **all gave up → nothing**; nothing tunable → nothing.
- [ ] **Step 2: Run** `test_app_unit "[auto_tune]"` → fail.
- [ ] **Step 3: Implement**, then wire main.cpp.
- [ ] **Step 4: Run** → pass; syntax-check main.cpp.
- [ ] **Step 5: Commit** `channels: nothing playing is never a resting state in Channel Guide style`.

---

### Task 6: Guide cursor and input

**Files:**
- Create: `cpp/src/app/guide_nav.{h,cpp}` (both lists)
- Modify: `cpp/src/app/app_state.h` (`GuideCursor guide_cursor;`), `cpp/src/main.cpp:1352-1440` (ROTATE / SELECT / NEXT / PREV dispatch branches on menu style)
- Test: `cpp/tests/app/test_guide_nav.cpp`

**Interfaces:**
- Produces: `struct app::GuideCursor { int row = 0; int scroll = 0; bool on_picture = false; };` `constexpr int kGuideVisibleRows = 5;` `GuideCursor guide_open(int tuned, int rows);` `GuideCursor guide_rotate(GuideCursor, int delta, int rows, bool picture_available);` `GuideCursor guide_clamp(GuideCursor, int rows, bool picture_available);` `enum class GuidePress { Tune, Close, TogglePicture, None };` `GuidePress guide_press(const GuideCursor&, int tuned, bool row_tunable);`
- Picture control sits ABOVE row 0: up from row 0 → `on_picture` (when available); down from it → row 0. Scroll keeps the cursor row visible within 5 rows.
- main.cpp in guide style: overlay hidden → ROTATE seeks (as today), SELECT reveals the overlay and sets `guide_cursor = guide_open(tuned, n)`; overlay shown → ROTATE → `guide_rotate`, SELECT → `guide_press`: `Tune` → `playlist_playback.tune(row, false)`, `Close` → hide the overlay with today's fade, `TogglePicture` → `toggle_picture_crop()`; NEXT/PREV → `playlist_playback.on_channel_step(±1)` and, while shown, the cursor follows the tuned channel. Clamp every frame with `state.picture_control_available`.

- [ ] **Step 1: Failing tests:** open lands on the tuned row with it visible; rotate clamps at the last row; up from row 0 reaches the picture only when available; down from the picture returns to row 0; the picture vanishing puts the cursor on row 0; scrolling with 8 rows keeps the row inside `[scroll, scroll+5)`; `guide_press` returns `Tune` on another tunable row, `Close` on the tuned row, `None` on an untunable row, `TogglePicture` on the picture.
- [ ] **Step 2: Run** `test_app_unit "[guide_nav]"` → fail.
- [ ] **Step 3: Implement**, then main.cpp's branches (List style dispatch unchanged).
- [ ] **Step 4: Run** → pass; syntax-check main.cpp.
- [ ] **Step 5: Commit** `guide: encoder, buttons and phone remote drive the channel guide`.

---

### Task 7: The guide's view model

**Files:**
- Create: `cpp/src/ui/channel_guide_view.h` (header-only)
- Test: `cpp/tests/ui/test_channel_guide_view.cpp` (tag `[guide_view]`)

**Interfaces:**
- Consumes: Tasks 2, 6.
- Produces (namespace `ui`):
  - `enum class GuideTone { Fg, Dim, Gold, Green, Steel, Red };`
  - `struct GuideViewInput { bool crt; const std::vector<app::Channel>* channels; const std::vector<app::Playlist>* playlists; int tuned; app::GuideCursor cursor; bool picture_available; bool picture_crop; double now_utc; int utc_offset_s; };`
  - `struct GuideView` holding: `window` (rect); `window_bug` (the tuned channel's 2-digit number or `MIX`, `""` when nothing is tuned; painted top-left inside the window in Zen Dots at 70 % `fg`); `panel` (`eyebrow`, `eyebrow_tone`, `clock_hm`, `clock_ap`, `ch_text`, `playlist_title`, `item_title`, `artist`, `start_hm`, `end_hm`, `progress` 0…1, `next_hm`, `next_title`, `bottom` ∈ {`Picture`, `PressToWatch`}, `join_note`, `show_artist`, `show_next`, `show_source`); `grid` (`date_label`, `ticks[{x, label}]`, `now_x`, `up_arrow`, `down_arrow`, `rows[{y, shuffle, number_text, name, num_tone, name_tone, cursor_bar, no_schedule, cells[{x, w, label, artist, show_text, show_artist, ring, tone, bold}]}]`); `hints[{key, label}]`; `logo` (rect).
  - `GuideView layout_channel_guide(const GuideViewInput&);`
- Rules beyond the Global Constraints: grid x is relative to the grid's left; cells start at `channel_column + 3`; px per second = `(grid_w − channel_column − 3) / window_s`; the window starts at local time floored to 5 minutes; ticks every 5 minutes, the first labelled with AM/PM; positions with `std::lround`; cell `w = right − left − 3`. Text is truncated by character count with a trailing `…` (monospace: chars = ⌊(w − 2×10) / (0.602 × 14)⌋). Eyebrow `NOW PLAYING` (Green) on the tuned row, else `ON CH 04 NOW` (Gold); `ch_text` = 2-digit number or `MIX` for 00; channel names are the playlist title upper-cased; row 00 has `shuffle = true`; a row with no schedule shows `NO SCHEDULE YET` (Dim) and no cells; `join_note` = `joins at m:ss`. CRT drops `artist`, `next`, the source label and cell artists. Hints: `TURN channels`, `PRESS watch`, `PRESS on green to hide guide` (TV), first two on CRT. Logo: TV `{739, 654, 181, 26}`, CRT `{477, 443, 139, 20}`.

- [ ] **Step 1: Failing tests** (TV, `utc_offset_s = 0`, now = E + 8 h 14 min 30 s, so local 8:14:30 AM; a channel of 600 s items starting at E):

```cpp
REQUIRE(v.grid.ticks.size() == 4);
CHECK(v.grid.ticks[0].label == "8:10 AM"); CHECK(v.grid.ticks[1].label == "8:15");
CHECK(v.grid.ticks[0].x == 199); CHECK(v.grid.ticks[1].x == 369); CHECK(v.grid.ticks[3].x == 710);
CHECK(v.grid.now_x == 351);
auto& cells = v.grid.rows[1].cells;            // 600 s items: 8:10–8:20, 8:20–8:30
CHECK(cells[0].x == 199); CHECK(cells[0].w == 338);
CHECK(cells[1].x == 540); CHECK(cells[1].w == 337);
CHECK(v.window.x == 40); CHECK(v.window.w == 384);
```

  Plus: a cell that began before 8:10 starts with `◂ `; a 25 px cell has `show_text == false`, a 50 px cell `show_artist == false`; a 37-character limit truncates a long title to 37 chars ending in `…`; the cursor row's on-now cell has `ring == true` and its row `cursor_bar == true`; **shuffle tuned row is green** (tuned 0 → row 0 `num_tone == Green`, panel `ch_text == "MIX"`); cursor on another row → eyebrow `ON CH 04 NOW`, `bottom == PressToWatch`; cursor on the tuned row with the picture available → `bottom == Picture`; an empty channel → `no_schedule` and no cells; nothing tuned (`tuned == -1`, every channel failed) → no row is Green, `window_bug == ""`, `bottom == PressToWatch`; CRT: 3 ticks, `show_artist == false`, `show_next == false`, logo `{477, 443, 139, 20}`.
- [ ] **Step 2: Run** `test_ui_unit "[guide_view]"` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** (MB ON and OFF) → pass.
- [ ] **Step 5: Commit** `guide: view model for the channel guide at both canvases`.

---

### Task 8: Paint the guide and put the video in its window

**Files:**
- Create: `cpp/src/ui/renderer_channel_guide.cpp` (in `UI_SOURCES` only: it is GL)
- Modify: `cpp/src/ui/renderer.h` (`void render_channel_guide(const GuideView&);`), `cpp/src/ui/renderer_main_menu.cpp:211-249` (branch on `menu_style`), `cpp/src/main.cpp` (video target, framebuffer clear, redraw signature `:1614-1644`, `unsetenv("TZ")` at start-up, `timezone_reload_request` → `tzset()`, saving settings when `last_channel_id` changes, rebuilding channels at boot / on reload / when the media info store changes)

**Interfaces:**
- Consumes: Tasks 2–7; crop plan's `set_picture`.
- Painting: `bg` over the whole content viewport except the window (no 50 % overlay); panel `bg_lift` with a 3 px gold top rule; the CH box with a 1 px gold hairline; cells `bg_lift`; cursor ring 2 px gold; channel cursor bar 4 px gold; now line 2 px red with a 10×6 marker; scroll arrows as the list draws them; logo via `draw_textured_quad(logo_texture_id_, …)`; tones map to the theme colours in the Global Constraints.
- Video: while the guide is shown, `set_picture(window_fb, crop, window_fb)` where `window_fb` is the view's window converted to framebuffer px, top-left origin — Modern TV: `x_fb = content_x + x × fb_h / 720`, `y_fb = y × fb_h / 720`; CRT Native: `x × fb_w / 640`, `y × fb_h / 480` (non-uniform). Clear the framebuffer before drawing the video while the guide is shown. Otherwise the crop plan's full-picture target.
- Redraw signature adds: guide shown, cursor row / scroll / on_picture, tuned channel, local minute.

- [ ] **Step 1:** Implement the renderer and main.cpp wiring.
- [ ] **Step 2:** Syntax-check `renderer_channel_guide.cpp`, `renderer_main_menu.cpp`, `main.cpp` (MB ON and OFF defines).
- [ ] **Step 3:** Run `test_ui_unit`, `test_app_unit`, `test_display_unit` (MB ON and OFF) → pass.
- [ ] **Step 4: Commit** `guide: paint the channel guide with the tuned video in its window`.

---

### Task 9: Time zone from the owner's browser

**Files:**
- Create: `web/timezone_apply.py`
- Modify: `web/admin_system.py` (route, beside `:152-181`), `web/admin.py:371-389` (`RELOAD_MARKERS` gains `"timezone": "timezone_reload_request"`), `web/static/manager_devices.js:290-306` (`loadAllContentFromDevice`) and its `?v=`
- Test: `web/tests/test_timezone_route.py`, `web/tests/test_timezone_page_load.py` (node harness of `test_upload_form_fields.py`)

**Interfaces:**
- Produces: `valid_zone(name: str, zoneinfo_dir: Path = Path("/usr/share/zoneinfo")) -> bool` (regex `^[A-Za-z_]+(/[A-Za-z0-9_+-]+){0,2}$` and an existing regular file); `rewrite_env_tz(env_path: Path, zone: str) -> bool` (replace the `TZ=` line or append one; atomic; keeps the file's mode; `False` when the file is absent). Route `POST /admin/system/timezone` (CSRF) with `{"zone": "America/Los_Angeles"}`: invalid → 400 `VALIDATION_ERROR`; equal to `timedatectl show -p Timezone --value` → `{"changed": false}` and no further call; otherwise `sudo -n timedatectl set-timezone <zone>` (10 s timeout), `rewrite_env_tz(<install>/services/.env, zone)`, poke `timezone` → `{"changed": true}`. The page posts `Intl.DateTimeFormat().resolvedOptions().timeZone` once per device load, after `fetchCsrfToken()`, ignoring any failure.

- [ ] **Step 1: Failing tests:** `valid_zone` accepts `America/Los_Angeles` (with a temp zoneinfo dir) and rejects `../etc/passwd`, `Foo`, `""`; `rewrite_env_tz` replaces an existing `TZ=` keeping other lines and the mode, appends when missing; the route (subprocess monkeypatched) calls `timedatectl set-timezone` only when the zone differs, writes the marker, and 400s on an invalid zone; the page-load harness posts `{zone}` to `/admin/system/timezone`.
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the web suite → only the known 16 failures.
- [ ] **Step 5: Commit** `web: set the box's time zone from the Content Manager's browser`.

---

### Task 10: Docs and whole-branch verification

**Files:**
- Modify: `CLAUDE.md` (Channel Guide under the kiosk's `app/` and `ui/` entries; the time-zone channel; Controls table gains the Channel Guide behaviour of PREV/NEXT)

- [ ] **Step 1:** Update `CLAUDE.md` where a stated fact changed.
- [ ] **Step 2:** `./tests/run_offline.sh --with-cpp` with Media Browser ON, then OFF (delete each build directory after) → every suite passes except the known 16 Werkzeug failures on this Mac.
- [ ] **Step 3:** Syntax-check every kiosk-only file touched.
- [ ] **Step 4: Commit** `docs: channel guide`.
- [ ] **Step 5: Hardware checklist for the PR description** (Pi 4B and Pi 5; Modern TV with an MDB bezel and CRT Native): Menu Style switches between List and Channel Guide and survives a restart; power-on lands on the last channel mid-video; the guide matches mockup A; the knob moves the cursor, a press tunes and closes, the yellow/green buttons step channels with the guide open and closed; tuning shows no gap longer than today's playlist switch; times match the phone's zone after opening the Content Manager once; after a game and after leaving the Media Browser the last channel resumes live; a channel with an unplayable file skips on; `journalctl -u magic-dingus-box-cpp` shows no watchdog kills over an hour with the guide open.
