# Per-video Crop Toggle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store uploads at their own aspect ratio and let each video be shown cropped to fill the 4:3 picture (default) or whole inside the bezel, toggled per video from the kiosk overlay and the Content Manager.

**Architecture:** One pure placement function decides where the video is drawn and which part of the frame is sampled; the GL renderer only applies it. A per-video `media_info.json` store, shared by the kiosk (C++, written on a background thread) and the web admin (Python), holds each video's crop override plus its length and display aspect. The list overlay gains a cursor state "on the Picture control", reached by turning past the last playlist.

**Tech Stack:** C++17 kiosk (GLES, GStreamer, jsoncpp, Catch2), Python 3 Flask web admin (pytest), classic browser JS (tested under node), bash + bats.

**Spec:** `docs/superpowers/specs/2026-11-12-video-crop-toggle-design.md` (read it first; this plan argues from it).

**Branch:** `crop-toggle`, cut from `architecture-fixes` (draft PR a-train-chain/magic_dingus_box#14, which this work depends on: background settings writer, `LatestWinsWorker`, the `controller_playback.cpp` split). All paths below are relative to the repo root; `cpp/` = `magic_dingus_box_cpp/`, `web/` = `magic_dingus_box/web/`.

## Global Constraints

- Crop is the default for every video; the store records only `crop: false` (an absent key means cropped).
- "4:3" means display aspect within 2 % of 4/3: `|aspect / (4/3) − 1| ≤ 0.02`.
- Stored masters: aspect-preserving, never upscaled, scaled to *cover* the preset's 4:3 box, and never outside a 1920-wide × 1080-tall box (a 9:16 clip → about 608×1080).
- `modern` stays accepted by the API as an alias of `crt_hd`.
- Board differences resolve at runtime only; no `#ifdef` per board; the Pi 4B (1.5 GB, hardware H.264 only) is the envelope.
- The render loop pings a 10 s systemd watchdog: no file read or write on it. The store's reads and writes run on its own worker thread.
- A new `cpp/src/app/*.cpp` goes in BOTH `APP_SOURCES` (`cpp/CMakeLists.txt:165-202`) and the `test_app_unit` source list (`:795-863`); a new `cpp/src/ui/*.cpp` in `UI_SOURCES` (`:137-163`) and `test_ui_unit` (`:767-792`).
- C++ suites run with Media Browser ON and OFF. The Mac has ~4 GB free: one build directory at a time, deleted afterwards.
- Kiosk-only files (`main.cpp`, `gst_renderer.cpp`, `renderer_*.cpp`) do not compile on the Mac: syntax-check them (`clang++ -fsyntax-only` with the test target's flags plus a stub `xf86drmMode.h` defining `struct drmModeModeInfo`); CI's `kiosk-build` job is their real compile.
- Web: route modules never import `admin`; bump a JS file's `?v=` in `web/static/index.html` whenever it changes.
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

- An anamorphic file (720×480 with pixel aspect 32:27) must show as 16:9, not 3:2 — pinned in Task 1 (`place_picture` takes display aspect) and Task 2 (`source_display_aspect` multiplies in PAR).
- A phone clip with a 90° rotation tag must be stored upright and sized by its displayed dimensions — pinned in Task 7 (`test_rotated_source_is_sized_upright`).
- A missing, empty or corrupt `media_info.json` means "everything cropped" and never blocks playback or the menu — pinned in Task 4 (`parse_media_info` returns `nullopt`; store map stays empty) and Task 5 (`load` returns `{}`).
- A kiosk toggle and a Content Manager edit at the same moment must both survive — pinned in Task 4 (`a web write between two kiosk toggles is kept`) and Task 5 (`update` merges under the lock).
- The video changing while the cursor is on the Picture control (next item is 4:3) must put the cursor back on the last playlist, never leave it on an invisible control — pinned in Task 6 (`picture focus falls back when the control disappears`).

---

### Task 1: Pure picture placement

**Files:**
- Create: `cpp/src/video/picture_placement.h` (header-only; `test_video_unit` links no `src/*.cpp`)
- Test: `cpp/tests/video/test_picture_placement.cpp` (picked up by the target's glob)

**Interfaces:**
- Produces (namespace `video`):
  - `struct IRect { int x, y, w, h; };` with `operator==`
  - `struct TexRect { float u0, v0, u1, v1; };`
  - `struct Placement { IRect viewport; TexRect tex; };`
  - `bool is_four_by_three(double display_aspect);`
  - `IRect four_by_three_rect(int w, int h);` — the centred 4:3 rect inside w×h (pillarbox or letterbox), integer-exact like main.cpp's current two copies
  - `IRect scale_image_rect(IRect r, int img_w, int img_h, int fb_w, int fb_h);` — a rect in bezel-image px (1920×1080) to framebuffer px, rounding to nearest
  - `Placement place_picture(double src_aspect, IRect target, double target_aspect, IRect opening, bool crop);` — `src_aspect` and `target_aspect` are DISPLAY aspects (CRT Native's 1280×720 framebuffer is shown as 4:3, so its pixels are not square); `opening` lies inside `target` (pass `target` when there is no bezel)

- [ ] **Step 1: Write the failing tests** (tag `[picture_placement]`)

```cpp
const video::IRect T{0, 0, 960, 720};
// crop fills the 4:3 target and samples the centre of the frame
auto c = video::place_picture(16.0/9, T, 4.0/3, T, true);
CHECK(c.viewport == T);
CHECK(c.tex.u0 == Approx(0.125)); CHECK(c.tex.u1 == Approx(0.875));
CHECK(c.tex.v0 == Approx(0.0));   CHECK(c.tex.v1 == Approx(1.0));
// fit shows the whole frame with bars
CHECK(video::place_picture(16.0/9, T, 4.0/3, T, false).viewport == video::IRect{0, 90, 960, 540});
// fit inside the MDB bezel opening at 1080p
const video::IRect T1080{240, 0, 1440, 1080}, MDB{284, 48, 1348, 984};
CHECK(video::place_picture(16.0/9, T1080, 4.0/3, MDB, false).viewport == video::IRect{284, 161, 1348, 758});
// crop ignores the opening (the bezel covers the edges, as today)
CHECK(video::place_picture(16.0/9, T1080, 4.0/3, MDB, true).viewport == T1080);
// CRT Native: non-square framebuffer shown as 4:3
const video::IRect CRT{0, 0, 1280, 720};
CHECK(video::place_picture(16.0/9, CRT, 4.0/3, CRT, false).viewport == video::IRect{0, 90, 1280, 540});
CHECK(video::place_picture(16.0/9, CRT, 4.0/3, CRT, true).tex.u0 == Approx(0.125));
// portrait
auto p = video::place_picture(9.0/16, T, 4.0/3, T, true);
CHECK(p.tex.v0 == Approx(0.2890625)); CHECK(p.tex.v1 == Approx(0.7109375)); CHECK(p.viewport == T);
CHECK(video::place_picture(9.0/16, T, 4.0/3, T, false).viewport == video::IRect{277, 0, 405, 720});
// 4:3 source: full target, full frame
auto q = video::place_picture(4.0/3, T, 4.0/3, T, true);
CHECK(q.viewport == T); CHECK(q.tex.u0 == Approx(0.0)); CHECK(q.tex.u1 == Approx(1.0));
// helpers
CHECK(video::is_four_by_three(640.0/480)); CHECK(video::is_four_by_three(720.0*8/(480*9))); // 720x480, PAR 8:9
CHECK_FALSE(video::is_four_by_three(1.5)); CHECK_FALSE(video::is_four_by_three(16.0/9));
CHECK(video::four_by_three_rect(1280, 720) == video::IRect{160, 0, 960, 720});
CHECK(video::four_by_three_rect(1920, 1080) == video::IRect{240, 0, 1440, 1080});
CHECK(video::four_by_three_rect(640, 480) == video::IRect{0, 0, 640, 480});
CHECK(video::scale_image_rect(MDB, 1920, 1080, 1280, 720) == video::IRect{189, 32, 899, 656});
```

- [ ] **Step 2: Run them, expect a compile failure** — `cmake --build build-crop --target test_video_unit` → `picture_placement.h` not found.
- [ ] **Step 3: Implement `cpp/src/video/picture_placement.h`.** Work in display-aspect space: the target's pixel aspect is `(target.w / target.h) / target_aspect`. Crop: viewport = target; the frame fraction kept on the long axis is `target_aspect / src_aspect` (or its inverse), centred. Fit: the largest rect of `src_aspect` inside `opening`, centred; full tex. Round sizes to nearest; centre with `(space − size) / 2` integer division.
- [ ] **Step 4: Run** `build-crop/test_video_unit "[picture_placement]"` → all pass.
- [ ] **Step 5: Commit** `video: pure picture placement (crop / fit, display-aspect aware)`.

---

### Task 2: Draw playlist video through the placement

**Files:**
- Modify: `cpp/src/video/gst_renderer.h:25-131`, `cpp/src/video/gst_renderer.cpp:176-198` (VBO), `:503-645` (`render_quad`)
- Modify: `cpp/src/main.cpp:1740-1798` (two 4:3 rect copies), `:1817-1902` (video block)
- Test: `cpp/tests/video/test_picture_placement.cpp` (PAR helper)

**Interfaces:**
- Consumes: Task 1.
- Produces:
  - `double video::display_aspect(int w, int h, int par_n, int par_d);` (add to `picture_placement.h`)
  - `void GstRenderer::set_picture(video::IRect target_fb, bool crop, video::IRect opening_fb);` — playlist/intro mode: `render_quad` draws via `place_picture` (target aspect 4/3)
  - `void GstRenderer::clear_picture();` — back to today's paths (Media Browser aspect-preserve and its render inset), untouched
  - `std::optional<double> GstRenderer::source_display_aspect() const;` — from the last uploaded frame, `nullopt` before the first

- [ ] **Step 1: Failing test** — `CHECK(video::display_aspect(720, 480, 32, 27) == Approx(16.0/9).epsilon(0.001)); CHECK(video::display_aspect(1920, 1080, 1, 1) == Approx(16.0/9));`
- [ ] **Step 2: Run, expect failure** (undefined `display_aspect`).
- [ ] **Step 3: Implement.** Convention: every `IRect` in this plan uses a TOP-LEFT origin in framebuffer px; convert to GL's bottom-left origin only at the `glViewport` call (`y_gl = fb_h − y − h`). `display_aspect` in the header. In the renderer: make the quad VBO `GL_DYNAMIC_DRAW` and rewrite only its texture coordinates (`glBufferSubData`) when the tex rect changes; in picture mode set `glViewport` to the placement viewport offset by nothing else (the target is already in framebuffer px) and remove the old `letterbox_mode_` branch (`set_letterbox_mode` is deleted; its only caller is main.cpp). Nothing is drawn outside the viewport, so the pillarbox bars stay black.
- [ ] **Step 4: main.cpp.** Replace the physical and logical 4:3 copies with `video::four_by_three_rect` (logical: on `ui_canvas`). Each frame, outside the Media Browser: target = `four_by_three_rect(mode)` in Modern TV, `{0,0,mode.width,mode.height}` in CRT Native; opening = the active bezel's opening via `scale_image_rect` when the bezel is drawn (Task 3), else target; crop = `media_info.crop_for(controller.current_media_key())` (Task 4; until Task 4 lands, `true`). In the Media Browser call `clear_picture()`.
- [ ] **Step 5: Verify** `test_video_unit` passes; syntax-check `gst_renderer.cpp` and `main.cpp`.
- [ ] **Step 6: Commit** `video: playlist and intro video drawn through place_picture; non-4:3 no longer stretched`.

---

### Task 3: Bezel catalog with visible openings

**Files:**
- Create: `cpp/src/app/bezel_catalog.{h,cpp}` (both CMake lists; `test_app_unit` also gains `src/utils/stb_image_impl.cpp`)
- Modify: `cpp/src/app/app_state.h:69-74` (`BezelInfo`), `cpp/src/main.cpp:548-599` (inline loader → `parse_bezel_catalog`), `cpp/assets/bezels/bezels.json`, `tests/local/bezels_json_schema.bats`
- Test: `cpp/tests/app/test_bezel_catalog.cpp`

**Interfaces:**
- Produces: `BezelInfo` gains `bool has_opening = false; int opening_x = 0, opening_y = 0, opening_w = 0, opening_h = 0;` (1920×1080 image px); `std::vector<app::BezelInfo> app::parse_bezel_catalog(const std::string& json_text);` (`file` stays the bare name; main.cpp keeps joining it to the found directory)
- JSON: every entry with a `file` carries `"opening": {"x":…, "y":…, "w":…, "h":…}`; `procedural` has none (it draws nothing; its opening is the whole 4:3 area).

- [ ] **Step 1: Failing tests** — parse a two-entry sample (`opening` present → fields set; absent → `has_opening == false`; malformed JSON → empty vector). Then the real catalog: for every entry with a file, load `fs::path(__FILE__).parent_path() / "../../assets/bezels" / file` with `stbi_load`, compute the bounding box of pixels with alpha < 16, and `CHECK` each declared edge is within 2 px of it (print the measured box on failure).
- [ ] **Step 2: Run, expect failures** (no openings declared yet).
- [ ] **Step 3: Implement** the parser, then fill `bezels.json` from the measured boxes (the three `mdb_*` bezels measure about `x284 y48 w1348 h984`; the other seven file bezels share one opening near `1403×1045`). Make the bats schema require `opening` with four positive integers on every entry that has a `file`.
- [ ] **Step 4: Run** `test_app_unit "[bezel_catalog]"` and `./tests/run_all.sh --local-only --filter bezels_json_schema` → pass.
- [ ] **Step 5: Commit** `bezels: declare each frame's visible opening; parse the catalog outside main.cpp`.

---

### Task 4: Kiosk side of the media info store

**Files:**
- Create: `cpp/src/app/media_info.{h,cpp}` (both CMake lists), fixture `cpp/tests/app/fixtures/media_info.json`
- Modify: `cpp/src/app/controller.h`, `cpp/src/app/controller_playback.cpp:88-110` (record the key), `cpp/src/main.cpp` (store construction, per-frame `poll`, the reload marker beside `:2092-2125`, shutdown flush beside `:2460-2476`)
- Test: `cpp/tests/app/test_media_info.cpp`

**Interfaces:**
- File `<data>/media_info.json`, version 1: `{"version": 1, "videos": {"media/x.mp4": {"duration_s": 212.4, "display_aspect": 1.7778, "crop": false}}}`. Keys are paths relative to the data directory with `/` separators. Every field is optional; `crop` is written only as `false`. Cross-process lock: `flock(LOCK_EX)` on the sidecar `<data>/media_info.json.lock` (never on the data file: it is replaced by rename). Writes: lock → read → merge → tmp + fsync → rename → unlock.
- Produces (namespace `app`):
  - `struct MediaInfo { std::optional<double> duration_s, display_aspect; std::optional<bool> crop; };`
  - `using MediaInfoMap = std::map<std::string, MediaInfo>;`
  - `std::optional<MediaInfoMap> parse_media_info(const std::string& json);` (`nullopt` = corrupt; empty text = empty map)
  - `std::string serialize_media_info(const MediaInfoMap&);`
  - `std::string media_key_for(const std::string& resolved_path, const std::string& data_dir);` (`""` outside the data dir)
  - `bool crop_for(const MediaInfoMap&, const std::string& key);` (default `true`)
  - `class MediaInfoStore` — ctor `(std::string path)`; `void request_reload();` `bool poll();` (render thread: adopt a finished read, re-applying pending kiosk edits on top; true when the map changed) `const MediaInfoMap& map() const;` `bool crop_for(const std::string& key) const;` `void set_crop(const std::string& key, bool crop);` (map updated now; the write is queued) `bool flush_until(std::chrono::steady_clock::time_point);` plus a test seam ctor taking a write hook (pattern: `StatusWriter`'s `FileSink`). Backed by `utils::LatestWinsWorker`; because it drops unstarted jobs, each job drains the store's pending-edit map at run time (the `WatchStore` pattern), so no edit is lost.
  - `const std::string& Controller::current_media_key() const;` — set in `load_file_with_resolution` from the resolved path; `""` for the intro and files outside the data dir.

- [ ] **Step 1: Failing tests** (tag `[media_info]`): the fixture parses to the expected values; `parse(serialize(m)) == m`; `parse_media_info("{oops")` is `nullopt`; `crop_for({}, "media/a.mp4")` is true; `media_key_for("/opt/x/data/media/a.mp4", "/opt/x/data") == "media/a.mp4"` and `""` for `/tmp/a.mp4`; `set_crop` returns immediately while the write hook is blocked and `crop_for` already reflects it; **a web write between two kiosk toggles is kept** (write the file directly with a new `duration_s` for another key between two `set_crop` calls; after `flush_until`, both the web's field and both toggles are on disk); `set_crop(k, true)` removes the `crop` field; a reload while an edit is pending keeps the edit.
- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement** with jsoncpp (same writer settings as `settings_persistence.cpp:70-145`) and the durable-write pattern of `write_file_durably` (`:147-203`).
- [ ] **Step 4: Wire main.cpp:** construct with `config::get_data_path() + "/media_info.json"`, `request_reload()` at start, `poll()` once per frame, marker `media_info_reload_request` → `request_reload()` (same handling as the settings marker), and `flush_until` within the existing 3 s shutdown deadline. Replace Task 2's temporary `true` with the store's answer.
- [ ] **Step 5: Run** `test_app_unit "[media_info]"` (MB ON and OFF) → pass; syntax-check `main.cpp`.
- [ ] **Step 6: Commit** `kiosk: per-video media info store (background, locked, merged writes)`.

---

### Task 5: Web side of the media info store

**Files:**
- Create: `web/media_info.py`
- Modify: `web/admin.py:371-389` (`RELOAD_MARKERS` gains `"media_info": "media_info_reload_request"`)
- Test: `web/tests/test_media_info.py` (reads the same `cpp/tests/app/fixtures/media_info.json`, path pattern of `test_playlist_yaml_fixture.py:23-26`)

**Interfaces:**
- Produces: `media_key(path: Path, data_dir: Path) -> str | None`; `load(data_dir: Path) -> dict[str, dict]` (missing/empty/corrupt → `{}`); `update(data_dir: Path, key: str, **fields) -> None` (a field set to `None` is removed; an entry left empty is removed); `remove(data_dir: Path, key: str) -> None`. Same file, lock file and write order as Task 4; atomic write via `admin_common._atomic_write_text`.

- [ ] **Step 1: Failing tests:** the fixture loads to the same values the C++ test asserts; `load` of a missing, empty and `{oops` file is `{}`; `update` keeps other keys and other fields; `update(..., crop=None)` removes `crop`; `media_key(data_dir/"media"/"a.mp4", data_dir) == "media/a.mp4"`, `None` outside; `update` blocks while another process holds the lock (`fcntl.flock` from a second fd in a thread, released after 200 ms; `update` returns after it).
- [ ] **Step 2: Run** `python3 -m pytest web/tests/test_media_info.py -q` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → pass.
- [ ] **Step 5: Commit** `web: media info store module (same file, lock and fixture as the kiosk)`.

---

### Task 6: The Picture control in the list overlay (logic)

**Files:**
- Create: `cpp/src/app/overlay_nav.{h,cpp}` (both CMake lists)
- Modify: `cpp/src/main.cpp:1352-1395` (ROTATE handler), `cpp/src/app/playlist_reload.cpp:155-184` (clamp), `cpp/src/app/playlist_playback.{h,cpp}` (`on_select`, `PlaylistTransport`), `cpp/src/app/controller_transport.h`, `cpp/src/app/app_state.h`
- Test: `cpp/tests/app/test_overlay_nav.cpp`, `cpp/tests/app/test_playlist_playback.cpp`

**Interfaces:**
- Produces:
  - `struct app::OverlayCursor { int selected = 0; int scroll = 0; bool on_picture = false; };`
  - `OverlayCursor app::rotate_overlay(OverlayCursor c, int delta, int rows, int max_visible, bool picture_available);`
  - `OverlayCursor app::clamp_overlay(OverlayCursor c, int rows, int max_visible, bool picture_available);`
  - `AppState` gains `bool picture_focus = false; bool picture_control_available = false; bool picture_crop = true;`
  - `virtual void PlaylistTransport::toggle_picture_crop() = 0;` — `ControllerTransport` forwards to a `std::function<void()>` supplied by main.cpp (which calls `media_info.set_crop(key, !current)`).

- [ ] **Step 1: Failing tests** (`[overlay_nav]`): moving down/up clamps at both ends with `picture_available == false` (today's behaviour, including scroll keeping the cursor visible and `scroll ∈ [0, rows − max_visible]`); down from the last row with `picture_available` sets `on_picture`, a further down is a no-op; up from `on_picture` returns to the last row; **picture focus falls back when the control disappears** (`clamp_overlay({rows-1, s, true}, rows, mv, false)` → `on_picture == false`, `selected == rows − 1`). In `test_playlist_playback.cpp`: with `picture_focus` true and the overlay visible, `on_select()` records exactly `"toggle_picture"` and leaves `ui_visible_when_playing` true and the indices unchanged; with the overlay hidden it reveals the overlay and does not toggle.
- [ ] **Step 2: Run, expect failure.**
- [ ] **Step 3: Implement** `overlay_nav`; route main.cpp's ROTATE block and the reload clamp through it (state fields ↔ `OverlayCursor`); add the `on_select` branch after the "reveal hidden UI" check; add the transport method (the fake records `"toggle_picture"`). main.cpp computes each frame: `picture_control_available = playlist_playback.owns_pipeline() && state.video_active && !key.empty() && aspect && !video::is_four_by_three(*aspect)`, where `aspect` is the store's `display_aspect` for the key, else `gst_renderer.source_display_aspect()`; then `clamp_overlay` it. Add `picture_focus` and `picture_crop` to the redraw signature beside `main.cpp:1625`.
- [ ] **Step 4: Run** `test_app_unit "[overlay_nav],[playlist_playback]"` (MB ON and OFF) → pass; syntax-check main.cpp.
- [ ] **Step 5: Commit** `overlay: turn past the last playlist to reach the Picture control`.

---

### Task 7: Draw the Picture control

**Files:**
- Create: `cpp/src/ui/picture_control_layout.h` (header-only)
- Modify: `cpp/src/ui/renderer.h:563-564`, `cpp/src/ui/renderer_main_menu.cpp:248` (call), `:497-785` (`render_playlist_list`), `:955-1019` (`render_footer`)
- Test: `cpp/tests/ui/test_picture_control_layout.cpp`

**Interfaces:**
- Produces: `struct ui::PictureControlPos { float x, y; bool own_line; };` `ui::PictureControlPos ui::place_picture_control(float canvas_w, float title_x, float title_baseline, float control_w, float line_h);` — left of the title (`x = title_x − 16 − control_w`) unless that is left of `canvas_w / 2`, then on its own line above the title, right-aligned at `canvas_w − 80`, at `title_baseline − line_h − 6`.
- `render_playlist_list` gains a trailing `bool picture_focus` (no row highlight or blinking indicator while true).

- [ ] **Step 1: Failing tests:** short title at 960 → same line, `x == title_x − 16 − control_w`; long title at 960 → `own_line`, `x == 960 − 80 − control_w`; long title at 640 → `own_line`.
- [ ] **Step 2: Run** `test_ui_unit "[picture_control]"` → fail.
- [ ] **Step 3: Implement** the helper; in `render_footer`, when `state.picture_control_available`, draw a 14 px box (2 px `fg` outline; filled `fg` with a `bg` tick when cropped) and the label `CROP` in `font_small_size`, at the helper's position; when `state.picture_focus`, outline and label in `accent` (gold). Pass `state.picture_focus` to `render_playlist_list`.
- [ ] **Step 4: Run** the UI test → pass; syntax-check `renderer_main_menu.cpp`.
- [ ] **Step 5: Commit** `overlay: draw the Picture control beside the now-playing title`.

---

### Task 8: Uploads keep their aspect ratio

**Files:**
- Create: `web/media_probe.py` (module level, so the Channel Guide's backfill job can import it; it imports nothing from `admin`)
- Modify: `web/admin_media.py:241-264` (presets), `:373-390` (probe → `media_probe`), `:392-436` (`run_transcode_job` filter), `:590-594` (publish), `:746-851` (`probe_video` direct-save rule), `:853-969` (smart-upload), `:156-194` (raw upload publish)
- Modify: `web/static/index.html:241-263` (Framing help text) and its `?v=`
- Test: `web/tests/test_master_size.py`, extend `web/tests/test_transcode_staging.py`

**Interfaces:**
- Consumes: Task 5 (`media_info.update`, `media_key`), `_poke_kiosk_reload("media_info")`.
- Produces in `web/media_probe.py`: `probe_source(path) -> dict | None` with `width, height` (as displayed: swapped for a ±90° rotation), `sar` (float), `rotation` (int), `duration_s` (float), `display_aspect` (float); `master_size(disp_w: float, disp_h: float, preset: str) -> tuple[int, int]` (cover the preset box, never upscale, cap at 1920×1080, round each side down to even). `TRANSCODE_RESOLUTIONS["modern"]` equals `crt_hd`. The upload's `fit_mode` no longer changes the file: `fit` writes `crop: False` for the new video, `crop` writes nothing.

- [ ] **Step 1: Failing tests:** `master_size(1920, 1080, "crt_hd") == (1280, 720)`; `master_size(1080, 1920, "crt_hd") == (608, 1080)`; `master_size(640, 480, "crt_hd") == (640, 480)`; `master_size(3840, 2160, "crt_fhd") == (1920, 1080)`; `master_size(853.33, 480, "crt_hd") == (852, 480)`; `master_size(1920, 1080, "modern") == master_size(1920, 1080, "crt_hd")`. With the ffmpeg stub: a 1920×1080 upload logs `-vf scale=1280:720,setsar=1` and no `crop=` / `pad=`; `test_rotated_source_is_sized_upright` (stub ffprobe reports 1920×1080 with rotation 90) logs `scale=608:1080`; after publish, `media_info.load` has the video's `duration_s` and `display_aspect`, and `crop` is `False` only for `fit_mode=fit`; a source already at its master size with H.264 and no rotation is saved directly (no ffmpeg run).
- [ ] **Step 2: Run** `python3 -m pytest web/tests/test_master_size.py web/tests/test_transcode_staging.py -q` → fail.
- [ ] **Step 3: Implement.** `-vf` becomes `scale={w}:{h},setsar=1` (ffmpeg auto-rotates its input). Direct save requires `master_size(src) == (src_w, src_h)`, rotation 0 and SAR 1, plus today's codec checks (drop the `same_aspect` rule). After every publish (transcode `:590`, direct `:924`, raw upload `:193`), probe the published file and `media_info.update(...)`, then poke. Help text: Framing now sets how the video starts out (cropped or whole) and can be changed later per video; widescreen uploads are now stored at their full width (a 16:9 video at the default quality is 1280×720, about a third more than before).
- [ ] **Step 4: Run** the web suite → only the known 16 Werkzeug failures on this Mac.
- [ ] **Step 5: Commit** `uploads: store at the source's aspect; framing becomes the video's starting crop setting`.

---

### Task 9: Crop per video in the Content Manager

**Files:**
- Modify: `web/admin_media.py:119-152` (list), `:196-217` (delete); add `POST /admin/media/picture`
- Modify: `web/static/manager_media.js:94-115` (row Actions cell) and its `?v=` in `index.html`
- Test: `web/tests/test_media_picture_route.py`, `web/tests/test_media_row_crop.py` (node harness of `test_upload_form_fields.py`)

**Interfaces:**
- Produces: each `GET /admin/media` entry gains `display_aspect: float | null` and `crop: bool`; `POST /admin/media/picture` with JSON `{"path": "data/media/x.mp4", "crop": false}` (CSRF; path must resolve inside `data/media`) → `{"ok": true, "data": {"crop": false}}` and pokes `media_info`; `DELETE /admin/media/<path>` also removes the store entry.

- [ ] **Step 1: Failing tests:** route sets and clears `crop` (store reflects it, marker written); a path outside `data/media` → 400 `VALIDATION_ERROR`; delete removes the entry; the list reports `crop` true by default; the JS row shows a Crop checkbox for `display_aspect` 1.7778 and none for 1.3333 or `null`, and clicking it calls `apiRequest` with `{path, crop}`.
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement** (`fetchWithCsrfRetry` via `apiRequest`; label `Crop`, ticked = cropped).
- [ ] **Step 4: Run** → pass.
- [ ] **Step 5: Commit** `content manager: crop on/off per video`.

---

### Task 10: The store travels with the box's data

**Files:**
- Modify: `web/admin_system.py:284-491` (backup + restore), `web/admin_playlist_import.py:562-658` (import), `cpp/scripts/deploy_cpp.sh:165-182`, `cpp/scripts/update.sh:16-35, 1861, 2043, 2508, 2669`, `tests/local/update_rsync_excludes.bats`, `OTA_UPDATE_GUARANTEES.md:87-96`, `tests/ota_rehearsal/box/manifest.py:34-41`, `.github/workflows/release.yml:180-200`
- Test: extend `web/tests/test_backup_restore.py`; a new case in the import tests

**Interfaces:**
- Consumes: Task 5.
- Behaviour: backup includes `data/media_info.json` when present; restore writes it atomically and pokes `media_info`. `media_info.json` and `media_info.json.lock` are per-box data on every deploy/OTA/release exclude list. Package import probes each extracted video into the store and, when the zip has a `media_info.json` at its root, merges its entries by basename onto the imported `media/<basename>` keys (no exporter exists in this repo; this lets one write the file).

- [ ] **Step 1: Failing tests:** backup zip contains `data/media_info.json`; restore writes it and creates the marker; the rsync-excludes bats lists both names in all four `update.sh` blocks; an import with a root `media_info.json` carrying `{"videos": {"clip.mp4": {"crop": false}}}` leaves `media/clip.mp4` with `crop: False` and a `duration_s`.
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement.** Entries for files a clone's curation stashes stay as harmless orphans (nothing reads a key with no file).
- [ ] **Step 4: Run** the web suite and `./tests/run_offline.sh --suite local-bats` → pass.
- [ ] **Step 5: Commit** `media info: backup, restore, import and the per-box exclude lists`.

---

### Task 11: Docs and whole-branch verification

**Files:**
- Modify: `CLAUDE.md` (the "Transcode presets are MASTERS" table and its 4:3-viewport paragraph — non-4:3 masters are no longer letterboxed or stretched; a `media_info.json` line in the kiosk/web file channels; the bezel `opening` field)

- [ ] **Step 1:** Update `CLAUDE.md` where a stated fact changed.
- [ ] **Step 2:** `./tests/run_offline.sh --with-cpp` with Media Browser ON, then OFF (delete each build directory after) → every suite passes except the known 16 Werkzeug failures in web-pytest on this Mac.
- [ ] **Step 3:** Syntax-check every kiosk-only file this plan touched.
- [ ] **Step 4: Commit** `docs: crop toggle`.
- [ ] **Step 5: Hardware checklist for the PR description** (both boards, Modern TV and CRT Native, with and without an MDB bezel): a 16:9 upload plays cropped full-screen; turning past the last playlist reaches CROP; pressing shows the whole frame inside the bezel opening with no stretch; the choice survives a restart and shows in the Content Manager; toggling there updates the TV within a second or two; a 4:3 video shows no control; a 9:16 phone clip plays upright; the Media Browser is unchanged; `journalctl -u magic-dingus-box-cpp` shows no watchdog kills while toggling during a large upload.
