# Movie Channels, Daily Lineups and Pick-Ahead Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Downloaded Media Browser movies become live Channel Guide channels sorted by type, every channel gets a new lineup each day, and a Pick Ahead mode lets the viewer start a later programme now.

**Architecture:** Movie channels are in-memory *virtual playlists* (path `movies:<type>`) appended after the real playlist rows of `state.playlists` only in Channel Guide style with the Media Browser unlocked, so the guide's existing rule "channel number = UI row" still holds. A worker-thread `MovieLibraryStore` keeps a private snapshot of Radarr's library; pure units decide channel membership, the daily lineup (a function of channel, local date and pass), the off-air window, the quiet-mode grace and the pick-ahead cursor; kiosk-only files gather facts and paint.

**Tech Stack:** C++17 kiosk (GLES, GStreamer, jsoncpp, Catch2), bash + bats for delivery scripts.

**Spec:** `docs/superpowers/specs/2026-11-13-movie-channels-design.md` (APPROVED 2026-11-13). Builds on `2026-11-12-channel-guide-design.md`.

**Branch:** `movie-channels`, cut from `channel-guide` (worktree `.worktrees/movie-channels`).

## Global Constraints

- Movie channels appear "only when the Media Browser is compiled in and unlocked, and only in Channel Guide style" (`MEDIA_BROWSER_ENABLED` + `state.media_browser_unlocked` + `MenuStyle::ChannelGuide`). Daily lineups and Pick Ahead exist in both builds.
- A channel needs **at least 3 movies**; **at most 8 movie channels**, keeping the fullest, **MOVIE MIX always kept**.
- Fixed channel order and rules (genre names are TMDB's, as Radarr reports them; match case-insensitively):
  MOVIE MIX (every movie) · KIDS (certification G or PG **and** a genre of Family or Animation) · CLASSICS (year before 1980) · COMEDY (Comedy) · DRAMA (Drama) · ACTION & ADVENTURE (Action or Adventure) · SCI-FI (Science Fiction or Fantasy) · THRILLERS & CRIME (Thriller, Crime or Mystery) · HORROR (Horror) · ROMANCE (Romance) · WAR & HISTORY (War, History or Western).
- The Kids channel never shows a PG-13, R, NR or unrated film.
- `last_channel` identity of a movie channel is `movies:<type>` (e.g. `movies:kids`).
- Library refresh: background worker at start-up once the tunnel is healthy, then every **15 minutes**; snapshot `data/movie_library.json`, atomic write on a worker, loaded at start-up.
- The snapshot is private: excluded from deploy/OTA rsyncs, deleted by golden-image prep and on first boot (the handling `media_info.json` gets).
- Daily lineups change at **4:00 AM local**; movie channels' day runs **6:00 AM → 1:00 AM**; **1:00–6:00 AM** movie channels are off the air, rows read `OFF THE AIR · BACK AT 6:00 AM`.
- Playlist channels: written order, rotated to a date-seeded start. Movie channels, Master Shuffle and Movie Mix: fully shuffled, reshuffled every pass (seed = channel identity + date + pass). Hand-written Fisher–Yates over `std::mt19937` (identical on Mac and Pi). An item running at a boundary is cut there.
- Downloads quiet mode while a movie-channel movie plays (`platform::service_quiet_mode`), released **120 s** after the last movie stops; never lift a pause the Media Browser or the owner made.
- Framing: a full-screen movie is edge to edge (no 4:3 pillarbox, no bezel), whole picture by default; per-film fill stored under key `movie:<tmdb_id>` in `media_info.json`, default whole. In the guide window a film shows whole.
- Settings → Display **Guide Mode: Live / Pick Ahead** (default Live), shown only in Channel Guide style; pick-ahead reaches **24 hours** ahead; eyebrow **ON DEMAND**.
- Channel viewing never writes `WatchStore`.
- Pi 4B: movies it cannot hardware-decode are left out, via a `PlatformProfile` field.
- No file I/O, `stat` of the movie drive or Radarr call on the render loop (WatchdogSec=10). Decisions in pure, Mac-tested units; kiosk-only files (`main.cpp`, `controller*.cpp`, `gst_renderer.cpp`, `renderer_*.cpp`, `settings_input.cpp`, `settings_menu.cpp`, `playback_screen.cpp`) only gather facts and paint; syntax-check them with the stub script `/private/tmp/claude-501/-Users-alexanderchaney-Documents----Projects----MDB-magic-dingus-box-/9afc55bb-ec80-43a5-ad2f-1c63b16e6337/scratchpad/stubs8/check_main.sh` (CI `kiosk-build` is the real compile).
- New `.cpp` files go in BOTH the kiosk and the test source lists of `magic_dingus_box_cpp/CMakeLists.txt`; Media Browser files only in the `MEDIA_BROWSER_ENABLED` lists and `test_media_browser_unit`.
- C++ suites with Media Browser ON and OFF. The Mac has ~3 GB free: one build directory at a time, under the scratchpad, deleted when the task ends.
- Commit with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; never push.

## Review Focus

1. **The library changes mid-movie** (a download lands, a movie is removed, the snapshot reloads): the tuned movie channel stays tuned by identity even if its row number moved, the playing film keeps playing, a channel that drops out ends at the film's end like a schedule loss. Test: Task 5.
2. **Drive unplugged mid-movie**: each failed movie starts the next programme at once, the give-up budget stops the loop within its count, the drive check removes the channels — never a dark window, never a reload storm. Test: Task 7.
3. **Time-zone or DST change across 4:00 / 1:00 / 6:00** while tuned: the schedule re-phases once, playback rejoins live rather than replaying or holding at EOS. Test: Task 4.
4. **The Media Browser is locked while a movie channel plays**: movie channels vanish at the rebuild, playback leaves the film for the last playlist channel or 00, and the download pause lifts after the grace. Test: Tasks 5 and 7.
5. **Pick-ahead across a boundary**: picking a programme after 4:00 AM, or the last film before 1:00 AM, plays it whole, then rejoins live — or parks on 00 when the channel is off air — and never replays what was just watched. Test: Task 9.

---

### Task 1: Radarr movie facts and the Pi 4B codec gate

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h` (`Movie`), `radarr_parsers.cpp` (the `movieFile` block near line 87)
- Create: `magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/movie_library.json`
- Test: `tests/media_browser/test_radarr_parsers.cpp`
- Modify: `src/platform/platform_profile.{h,cpp}`; Test: `tests/platform/` (the profile test file)

**Interfaces:**
- Produces: `Movie` fields `std::vector<std::string> genres;` `std::string certification;` `double file_runtime_s = 0;` (exact seconds of `movieFile.mediaInfo.runTime`) `std::string file_video_codec;` (`movieFile.mediaInfo.videoCodec`, verbatim).
- Produces: `PlatformProfile::movie_channel_codecs` (`std::vector<std::string>`; empty = any codec). Pi4 `{"x264", "h264", "avc"}`; Pi5 and Unknown `{}`. A POLICY row (it switches under `MDB_PLATFORM_POLICY_OVERRIDE=pi4`).

- [ ] **Step 1: Write the failing tests.** Fixture `movie_library.json` = a `/api/v3/movie` array of five records: (a) "Toy Tale" 1995, certification `G`, genres `["Animation","Family"]`, runTime `"1:21:03"`, videoCodec `"x264"`; (b) "Night Things" 1982, `R`, `["Horror"]`, `"1:41:00"`, `"x264"`; (c) "Big Laugh" 2004, `PG-13`, `["Comedy"]`, `"1:38:30"`, `"x265"`; (d) "Old Story" 1955, `NR`, `["Drama","Romance"]`, `"2:01:15"`, `"h264"`; (e) "Not Yet" 2025, `hasFile: false`, no `movieFile`. Test `[radarr_parsers]` "library facts": (a) `genres == {"Animation","Family"}`, `certification == "G"`, `file_runtime_s == 4863.0`, `file_video_codec == "x264"`, and `file_runtime_minutes == 81` still; (e) `genres` parsed, `file_runtime_s == 0`, `file_video_codec.empty()`; a record whose `genres` is not an array yields empty genres. Profile test: Pi4's `movie_channel_codecs` equals `{"x264","h264","avc"}`, Pi5 and Unknown are empty, and the pi4 override on a Pi 5 gives Pi4's list.
- [ ] **Step 2: Run** `test_media_browser_unit "[radarr_parsers]"` and `test_platform_unit` → fail.
- [ ] **Step 3: Implement** the four fields in the existing `movieFile` block (reuse `parse_timeleft_to_seconds`; `genres` from the record's `genres` array, non-string entries skipped) and the profile field + its row in the classification table comment + the override copy.
- [ ] **Step 4: Run** both suites (MB ON; `test_platform_unit` also MB OFF) → pass.
- [ ] **Step 5: Commit** `feat(mb): Radarr library facts for movie channels; Pi 4B movie codec list`.

### Task 2: Movie channel rules and the snapshot format

**Files:**
- Create: `src/media_browser/channels/movie_library.{h,cpp}`, `src/media_browser/channels/movie_channels.{h,cpp}`
- Modify: `src/app/app_state.h` (`PlaylistItem`), `src/app/channel_schedule.h` (prefix helpers)
- Test: `tests/media_browser/test_movie_library.cpp`, `tests/media_browser/test_movie_channels.cpp` (in `test_media_browser_unit`)

**Interfaces:**
- Consumes: Task 1 `Movie` fields; `RadarrClient::resolve_host_path`.
- Produces (`app` namespace, MB-only files):
  ```cpp
  struct LibraryMovie {
      int tmdb_id = 0; std::string title; int year = 0;
      std::vector<std::string> genres; std::string certification;
      std::string path;     // absolute host path of the file
      double length_s = 0;  // exact file length
      std::string codec;
  };
  // {"version":1,"movies":[{tmdb_id,title,year,genres,certification,path,length_s,codec}]}
  std::string serialize_movie_library(const std::vector<LibraryMovie>&);
  std::optional<std::vector<LibraryMovie>> parse_movie_library(const std::string& json);  // nullopt = corrupt
  // has_file movies only; path = resolve(file_container_path); length_s = file_runtime_s
  std::vector<LibraryMovie> to_library_movies(const std::vector<Movie>&,
      const std::function<std::string(const std::string&)>& resolve);
  bool codec_allowed(const std::vector<std::string>& allowed, const std::string& codec);  // case-insensitive; "" allowed
  std::vector<Playlist> build_movie_playlists(const std::vector<LibraryMovie>& movies,
                                              const std::vector<std::string>& allowed_codecs);
  ```
- Produces (`channel_schedule.h`, both builds): `inline constexpr const char* kMovieChannelPrefix = "movies:";` `bool is_movie_playlist(const Playlist&)` (path starts with the prefix).
- Produces: `PlaylistItem::movie_tmdb_id` (`int`, 0 = not a movie). A movie playlist: `title` = the channel name ("MOVIE MIX"), `path` = `movies:mix|kids|classics|comedy|drama|action|scifi|thrillers|horror|romance|war`; each item `{path = movie.path, source_type = "local", title = movie.title, artist = year as text ("" when 0), start = 0, end = length_s, movie_tmdb_id}` — `end` makes it a trimmed item, so `build_channels` schedules it without `media_info.json`.

- [ ] **Step 1: Write the failing tests** (`[movie_channels]`, `[movie_library]`): snapshot round-trip; corrupt text → nullopt; a wrong-typed field drops only that movie. Rules: Kids takes G/PG + Family/Animation and never PG-13, R, NR or "" (a PG-13 Family film and an NR Animation film are both out); Classics is year < 1980 (1979 in, 1980 out, year 0 out); a 2-movie genre makes no channel, a 3-movie one does; fewer than 3 movies in all → no playlists at all; with 10 candidate channels qualifying, exactly 8 come back, MOVIE MIX first, the 7 fullest of the rest (ties broken by table order), emitted in table order; a movie lands on every channel it qualifies for; items sorted by title then tmdb_id; codec gate (`{"x264","h264","avc"}` drops `x265`/`HEVC`/`AV1`, keeps `X264` and `""`); `length_s < kMinScheduledItemS`, empty path or `tmdb_id <= 0` → left out; `to_library_movies` skips `has_file == false` and uses the resolver.
- [ ] **Step 2: Run** `test_media_browser_unit "[movie_channels],[movie_library]"` → fail.
- [ ] **Step 3: Implement.** The rule table is one static array of `{key, title, predicate}` in table order.
- [ ] **Step 4: Run** → pass (MB ON); MB OFF builds (`is_movie_playlist` compiles in both).
- [ ] **Step 5: Commit** `feat(mb): movie channel rules and library snapshot format`.

### Task 3: The movie library store (worker)

**Files:**
- Create: `src/media_browser/channels/movie_library_store.{h,cpp}`
- Test: `tests/media_browser/test_movie_library_store.cpp`

**Interfaces:**
- Consumes: Task 2 `LibraryMovie`, `serialize/parse_movie_library`; `utils::LatestWinsWorker`; `app::MediaInfoStore::write_file_durably` (the durable-write sink).
- Produces:
  ```cpp
  class MovieLibraryStore {
  public:
      using Fetch  = std::function<std::optional<std::vector<LibraryMovie>>()>;  // worker; nullopt = Radarr unreachable
      using Exists = std::function<bool(const std::string& path)>;               // worker
      using FileSink = std::function<bool(const std::string& path, const std::string& body)>;
      using Clock = std::function<std::chrono::steady_clock::time_point()>;
      static constexpr std::chrono::minutes kRefreshInterval{15};
      MovieLibraryStore(std::string snapshot_path, Fetch fetch, Exists exists,
                        FileSink sink = nullptr, Clock clock = nullptr);
      void start();                  // queue the snapshot read (boot)
      void request_drive_check();    // re-check every movie's file now
      bool poll(bool tunnel_healthy);  // render thread; true when movies() changed
      const std::vector<LibraryMovie>& movies() const;  // only movies whose file exists
      bool first_load_finished() const;  // the snapshot read ran, any outcome
      void wait_idle_for_tests();
  };
  ```
- Rules: `poll` adopts worker results and, when `tunnel_healthy`, queues a fetch the first time and then `kRefreshInterval` after the last fetch ATTEMPT finished. A fetch that returns a list writes the snapshot (all movies, before the existence filter) and publishes the existence-filtered list; nullopt keeps everything as it is. A failed write keeps the list and is retried with the next fetch. `request_drive_check` re-filters the last full list on the worker. Every read, write and `Exists` call runs on the worker; `poll` never blocks.

- [ ] **Step 1: Write the failing tests** (`[movie_library_store]`, fake clock/fetch/exists/sink): boot with a snapshot → movies after the read, `first_load_finished`; no file or corrupt file → empty, `first_load_finished` true; no fetch while the tunnel is down; first healthy poll fetches, the next fetch only after 15 min; a fetch writes the snapshot through the sink and publishes; a fetch returning nullopt keeps the old list; a movie whose file is missing is not published but stays in the snapshot; `request_drive_check` after files vanish publishes an empty list at once (no 15-min wait); a failing sink keeps the published list.
- [ ] **Step 2: Run** `test_media_browser_unit "[movie_library_store]"` → fail.
- [ ] **Step 3: Implement** on one `LatestWinsWorker`, with the drain-every-job pattern `MediaInfoStore` uses so a replaced job never loses a queued drive check or fetch.
- [ ] **Step 4: Run** → pass.
- [ ] **Step 5: Commit** `feat(mb): movie library store with private snapshot`.

### Task 4: Daily lineups in the schedule

The hardest task: dispatch on the most capable model.

**Files:**
- Modify: `src/app/channel_schedule.{h,cpp}`, `src/app/playlist_playback.{h,cpp}`, `src/app/controller_transport.h`, `src/ui/channel_guide_view.h`, every other caller `git grep` finds
- Test: `tests/app/test_channel_schedule.cpp` (existing expectations are rewritten for daily lineups), `tests/app/test_playlist_playback.cpp`, `tests/ui/test_channel_guide_view.cpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class LineupOrder { Rotate, Shuffle };
  inline constexpr int kLineupDayStartH = 4;
  inline constexpr int kMovieDayStartH = 6;
  inline constexpr int kMovieOnAirH = 19;   // 6:00 AM -> 1:00 AM
  struct Channel {           // gains:
      std::string identity;  // "shuffle" / playlist_identity(...) / "movies:<type>"
      LineupOrder order = LineupOrder::Rotate;
      int day_start_h = kLineupDayStartH;
      int on_air_h = 24;
      // number, items (written order), total_s as before
  };
  std::vector<int> day_order(const Channel&, int64_t day, int pass);  // a permutation of item indexes
  std::optional<OnAir> on_air(const Channel&, double now_utc, int utc_offset_s);      // nullopt: empty or off air
  std::vector<OnAir> lineup(const Channel&, double from_utc, double to_utc, int utc_offset_s);
  std::optional<OnAir> airing_after(const Channel&, const OnAir&, int utc_offset_s);  // nullopt: off air follows
  std::optional<OnAir> join_point(const Channel&, double now_utc, int utc_offset_s);
  std::optional<OnAir> boundary_join(const Channel&, double now_utc,
                                     const std::optional<LoadedAiring>& last, int utc_offset_s);
  std::optional<double> back_on_air_at(const Channel&, double now_utc, int utc_offset_s);  // nullopt: on air now
  ```
  `kScheduleEpochUtc` is deleted. `OnAir::index` stays an index into `Channel::items`.
- Produces: `PlaylistTransport::utc_offset_s(double utc) const` (pure virtual; the kiosk adapter returns `localtime_r`'s `tm_gmtoff`, the test fake a settable value, default 0).
- `build_channels`: row 0 `identity = kShuffleChannelId`, `order = Shuffle`, items = every playlist row's items in row order **except movie playlists** (no pre-shuffle — `day_order` shuffles); playlist rows `Rotate` with `playlist_identity`; movie playlists `Shuffle`, `day_start_h = kMovieDayStartH`, `on_air_h = kMovieOnAirH`, identity = their path.
- The day (the algorithm the tests pin):
  ```
  local = now + off;  day = floor((local - day_start_h*3600) / 86400)
  day_start_utc = day*86400 + day_start_h*3600 - off;  t = now - day_start_utc
  t >= on_air_h*3600  -> off air, back at day_start_utc + 86400
  pass = floor(t / total_s); walk day_order(ch, day, pass) to t - pass*total_s
  ends_at = min(natural end, day_start_utc + on_air_h*3600)
  ```
  Seed = 32-bit FNV-1a of `identity + "|" + day + "|" + pass` (decimal). Rotate: start = seed(identity, day, 0) % n, order = rotation of 0..n-1 (every pass alike). Shuffle: the hand-written Fisher–Yates `build_channels` uses today (`j = rng() % (k + 1)`, k from n-1 down), with `std::mt19937 rng(seed)`.
- Cut airings: `PlaylistPlayback::load_on_air` records the file position where the airing ends (`item.start + (oa.ends_at - oa.starts_at)`); in guide style `tick_auto_advance` uses the smaller of that and the item's own `end`, so a cut airing advances at the cut.

- [ ] **Step 1: Write the failing tests** (`[channel_schedule]`): a Rotate channel airs its items in written order from a start that is the same all day and differs on most of 10 consecutive days; the lineup changes between 03:59:59 and 04:00:00 local and not between 23:59:59 and 00:00:01 (offset −8 h and +5:30); the same instant gives the same airing in separate calls; a Shuffle channel's passes are permutations and not all equal over 5 passes of a 60 s channel; an airing crossing the day boundary has `ends_at` = the boundary and the next day starts with that day's `day_order[0]` at offset 0; a movie channel is off air at 01:00:00 and 05:59:59 local, `back_on_air_at` = 06:00 local, `join_point` and `lineup` give nothing in [1:00, 6:00), an airing crossing 1:00 ends at 1:00, `airing_after` the last airing of the movie day is nullopt; the same UTC instant at offsets 0 and +3600 lands in different days near the boundary; `boundary_join` across 4:00 never returns the loaded airing and yields the new day's first airing; Master Shuffle contains no movie-playlist items; **golden**: `day_order` for identity `"path:/golden.yaml"`, 7 items, day 20405, passes 0 and 1, both orders — computed once on the Mac and pasted as literals (fails wherever libc++/libstdc++ differ). `[playlist_playback]`: a cut airing advances at the cut position; a time-zone change of +1 h between two boundaries rejoins live, never the loaded airing. `[channel_guide_view]` existing tests updated for offset-aware calls.
- [ ] **Step 2: Run** `test_app_unit "[channel_schedule],[playlist_playback]"`, `test_ui_unit "[channel_guide_view]"` → fail.
- [ ] **Step 3: Implement**; update every caller to pass the offset (`controller_transport.h` uses `localtime_r` — no file I/O after `tzset`).
- [ ] **Step 4: Run** `test_app_unit`, `test_ui_unit` in full, MB ON and OFF → pass; syntax-check `main.cpp`.
- [ ] **Step 5: Commit** `feat(guide): daily lineups — rotated playlists, reshuffled mixes, cut at the day boundary`.

### Task 5: Movie channels in the channel list

**Files:**
- Modify: `src/app/playlist_reload.{h,cpp}`, `src/app/auto_tune.{h,cpp}`, `src/main.cpp`, `src/media_browser/mb_services.{h,cpp}` (the store's Fetch)
- Test: `tests/app/test_playlist_reload.cpp`, `tests/app/test_auto_tune.cpp`

**Interfaces:**
- Consumes: Task 2 `build_movie_playlists`, `is_movie_playlist`; Task 3 `MovieLibraryStore`; Task 4 `build_channels`.
- Produces: `void merge_movie_playlists(std::vector<Playlist>& playlists, std::vector<Playlist> movie_playlists)` — removes every movie playlist, then appends the given ones after the last real row. `playlist_identity` returns a movie playlist's path verbatim (`movies:kids`). `AutoTuneInput::movie_library_read` (`bool`, default `true`): the power-on tune waits for it like `media_info_read`.
- Wiring (`main.cpp`, under `MEDIA_BROWSER_ENABLED`): the store is built with the snapshot `<data>/movie_library.json`, a Fetch that calls `RadarrClient::get_library_checked()` and `to_library_movies` with `resolve_host_path`, and `Exists` = `std::filesystem::exists` with an error code. `movie_channels_on = menu_style == ChannelGuide && state.media_browser_unlocked`. `rebuild_channels()` merges `build_movie_playlists(store.movies(), profile.movie_channel_codecs)` (or nothing when off) **through the playlist-reload re-anchoring** (`snapshot_for_reload` → merge → the same re-anchor `apply_reloaded_playlists` does), never by assigning `state.playlists` bare. The web-admin playlist reload merges the current movie playlists into its fresh list before re-anchoring. Rebuild triggers: `store.poll(tunnel_healthy)` true, the unlock flag changing, the Menu Style changing. `tunnel_healthy` comes from the existing `VpnHealthMonitor` flag.

- [ ] **Step 1: Write the failing tests:** `[playlist_reload]` merge is idempotent, keeps real rows and their order, strips all movie rows when given none, and a reload snapshot taken on a movie channel re-anchors to the same identity after the movie rows reorder (Review Focus 1), and to "nothing tuned" when that channel is gone (Review Focus 4); `playlist_identity` of `movies:kids` is `movies:kids`. `[auto_tune]` gates stay closed while `movie_library_read` is false and open when it turns true; a `last_channel` of `movies:comedy` resolves once the movie rows exist.
- [ ] **Step 2: Run** `test_app_unit "[playlist_reload],[auto_tune]"` → fail.
- [ ] **Step 3: Implement** the pure parts, then the wiring.
- [ ] **Step 4: Run** `test_app_unit` (MB ON and OFF) → pass; syntax-check `main.cpp`, `mb_services.cpp`.
- [ ] **Step 5: Commit** `feat(guide): movie channels join the guide when the Media Browser is unlocked`.

### Task 6: Edge-to-edge framing and the per-film Picture control

**Files:**
- Modify: `src/app/media_info.{h,cpp}`, `src/video/picture_placement.h`, `src/video/gst_renderer.{h,cpp}`, `src/app/controller_playback.cpp` (+ `controller.h`), `src/app/guide_nav.{h,cpp}` (beside `guide_painted`), `src/main.cpp`
- Test: `tests/app/test_media_info.cpp`, `tests/video/test_picture_placement.cpp`, `tests/app/test_guide_nav.cpp`

**Interfaces:**
- Produces: `std::string movie_media_key(int tmdb_id)` → `"movie:<id>"`; `bool crop_default_for(const std::string& key)` (false for `movie:` keys, true otherwise). `crop_for` falls back to that default; `set_crop` removes the field when the value equals the key's default and otherwise writes it (so a film set to fill stores `"crop": true`). Parse and the web admin are unchanged (the web admin's `load` ignores `crop: true`; golden-image pruning drops `movie:` keys because no such file exists).
- Produces: `bool video::picture_crop_for(bool is_movie, bool in_guide_window, bool stored, double display_aspect)` — a movie: `!in_guide_window && stored`; otherwise `picture_crop(stored, display_aspect)` (unchanged). `bool video::movie_fill_offered(double film_aspect, double output_aspect)` — the aspects differ by more than 2%.
- Produces: `bool app::movie_full_screen(const AppState&)` — Channel Guide style, the playing item (`current_playlist_index`/`current_item_index`) has `movie_tmdb_id > 0`, `!guide_painted(s)`, and the Media Browser does not own the screen.
- Produces: `GstRenderer::set_picture(IRect target_fb, bool crop, IRect opening_fb, double target_aspect = 4.0 / 3.0)`.
- Kiosk wiring: `Controller::current_media_key()` is `movie_media_key(item.movie_tmdb_id)` for a movie item. In `main.cpp`, when `movie_full_screen(state)`: `letterbox_active` is false (so no bezel), the target is the whole content rect and `target_aspect = content.w / double(content.h)`; the crop is `picture_crop_for(...)`. `picture_control_available` for a movie uses `movie_fill_offered`. In CRT Native nothing changes (the output is already 4:3).

- [ ] **Step 1: Write the failing tests:** `[media_info]` a `movie:` key with no entry is whole (`crop_for == false`); `set_crop("movie:603", true)` stores `crop: true` and `set_crop(..., false)` removes the field (and the entry); playlist keys unchanged. `[picture_placement]` a movie is whole in the guide window even when stored fill; full screen it follows the stored value, and a 4:3 film is NOT forced to crop; `movie_fill_offered(2.39, 16/9.)` true, `(1.78, 16/9.)` false. `[guide_nav]` `movie_full_screen` false in List style, with the guide painted, on a playlist item, and true for a playing film full screen.
- [ ] **Step 2: Run** `test_app_unit "[media_info],[guide_nav]"`, `test_video_unit "[picture_placement]"` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the three suites (MB ON and OFF) → pass; syntax-check `main.cpp`, `gst_renderer.cpp`, `controller_playback.cpp`.
- [ ] **Step 5: Commit** `feat(guide): movies play edge to edge; per-film fill under movie:<tmdb_id>`.

### Task 7: Download pause and the failed-movie rule

**Files:**
- Modify: `src/app/movie_quiet_mode.{h,cpp}`, `src/app/playlist_playback.{h,cpp}`, `src/main.cpp`
- Create: `src/app/movie_channel_quiet.h`
- Test: `tests/retroarch/test_movie_quiet_mode.cpp`, `tests/app/test_movie_channel_quiet.cpp`, `tests/app/test_playlist_playback.cpp`

**Interfaces:**
- Produces: `enum class MovieQuietMode::Claimant { MediaBrowser, MovieChannel };` `request_pause(Mode, Claimant = Claimant::MediaBrowser)`, `request_resume(Claimant = Claimant::MediaBrowser)` (PlaybackScreen's calls are unchanged). Claims are kept on the calling (render) thread side: a pause is queued only when the first claim arrives or a claim asks for a stronger mode than any held (FullPause > Trickle); a resume is queued only when the last claim is released; a resume from a claimant holding nothing does nothing. The worker's consent accumulation and the cancel-an-unstarted-pause rule are unchanged.
- Produces: `class MovieChannelQuiet { public: static constexpr double kReleaseGraceS = 120.0; enum class Cmd { None, Claim, Release }; Cmd tick(bool movie_playing, double now_s); bool claimed() const; };` — Claim on the first frame a film plays; Release once no film has played for 120 s straight.
- Produces: `PlaylistPlayback::set_movie_failure_hook(std::function<void()>)`. A failed item (`Decision::Advance`) whose loaded item is a movie starts the channel's next programme now, from its beginning (`airing_after` the loaded airing, offset 0) instead of waiting for the slot; with nothing after it (off air) it stops as today. Every failed movie calls the hook once. A playlist channel's failed item still waits for its slot. `GiveUp` is unchanged.
- Wiring (`main.cpp`): `movie_playing` = guide style, `video_active`, the current item is a movie. On Claim: `platform::service_quiet_mode(profile, platform::read_mem_available_kib())` → `movie_quiet_mode.request_pause(mode, Claimant::MovieChannel)`; on Release: `request_resume(Claimant::MovieChannel)`. The hook calls `movie_library_store.request_drive_check()`.

- [ ] **Step 1: Write the failing tests:** `[movie_quiet_mode]` two claimants — the channel's claim then the MB's: one pause; MB resume → no resume; channel release → one resume; a Trickle claim then a FullPause claim → a second pause, one resume undoes both; resume from a claimant without a claim → nothing. `[movie_channel_quiet]` claim on first play; channel surfing (film, 30 s playlist, film) never releases; release at exactly 120 s without a film; a new film after release claims again. `[playlist_playback]` a failed movie loads the next programme at offset 0 at once; the hook runs once per failure; three failing films in a row give up within the existing budget and leave nothing tuned (Review Focus 2); a failed playlist item still waits for `guide_wait_until_utc`.
- [ ] **Step 2: Run** `test_retroarch_unit "[movie_quiet_mode]"`, `test_app_unit "[movie_channel_quiet],[playlist_playback]"` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the suites (MB ON and OFF) → pass; syntax-check `main.cpp`, `playback_screen.cpp`.
- [ ] **Step 5: Commit** `feat(guide): movie channels pause downloads with a 2-minute grace; a failed film moves on`.

### Task 8: The overnight off-air window

**Files:**
- Modify: `src/app/app_state.h`, `src/app/playlist_playback.{h,cpp}`, `src/app/auto_tune.{h,cpp}`, `src/app/guide_nav.{h,cpp}`, `src/ui/channel_guide_view.h`, `src/ui/renderer_channel_guide.cpp`, `src/main.cpp`
- Create: `src/app/off_air.h`
- Test: `tests/app/test_off_air.cpp`, `tests/app/test_playlist_playback.cpp`, `tests/app/test_auto_tune.cpp`, `tests/app/test_guide_nav.cpp`, `tests/ui/test_channel_guide_view.cpp`

**Interfaces:**
- Produces: `AppState::off_air_return_id` (`std::string`, "" = none; not persisted).
- `PlaylistPlayback::rejoin_schedule`: when the tuned channel is off air (`back_on_air_at` has a value), set `off_air_return_id` to its identity (if empty) and load channel 0's live airing in place, keeping the overlay. `land_on_channel(0)` leaves `last_channel_id` alone while `off_air_return_id` is set.
- Produces: `std::optional<int> off_air_return_row(const AppState&, int return_row, bool return_on_air, bool switch_pending)` (in `off_air.h`) — the row to tune when `off_air_return_id` is set, the box is still on 00, the return channel is on air again and no switch is pending.
- `guide_input`: a viewer's Tune or ChannelStep clears `off_air_return_id`.
- `AutoTuneInput` gains `bool last_channel_off_air = false;` — `decide_auto_tune` returns 0 when the last channel is off air (main.cpp then sets `off_air_return_id = last_channel_id`). `tunable[c]` is false for an off-air channel.
- View: `GuideRow::off_air` (`bool`) and `off_air_text` = `"OFF THE AIR · BACK AT " + clock(back_on_air_at)` in the guide's 12-hour form (`6:00 AM`), no cells, Dim; the row cannot be tuned (`cursor_row_tunable` false); fitted on the CRT canvas.

- [ ] **Step 1: Write the failing tests:** `[off_air]` return row given only on 00, only when back on air, never during a switch. `[playlist_playback]` a film cut at 1:00 parks on 00 at once, `off_air_return_id == "movies:comedy"`, `last_channel_id` still `movies:comedy`. `[guide_nav]` a viewer Tune or channel step clears the return id; a Close on 00 does not. `[auto_tune]` power-on at 3:00 AM with last channel `movies:kids` → 00. `[channel_guide_view]` an off-air movie row reads `OFF THE AIR · BACK AT 6:00 AM` at offsets −8 h and +1 h and has no cells.
- [ ] **Step 2: Run** `test_app_unit "[off_air],[playlist_playback],[guide_nav],[auto_tune]"`, `test_ui_unit "[channel_guide_view]"` → fail.
- [ ] **Step 3: Implement**; `main.cpp` tunes `off_air_return_row`'s row with `keep_overlay=true` each frame it has one; the renderer paints the off-air row.
- [ ] **Step 4: Run** → pass (MB ON and OFF); syntax-check `main.cpp`, `renderer_channel_guide.cpp`.
- [ ] **Step 5: Commit** `feat(guide): movie channels go off the air 1-6 AM; the box parks on 00 and comes back`.

### Task 9: Guide Mode setting and on-demand playback

**Files:**
- Modify: `src/app/app_state.h`, `src/app/settings_persistence.cpp`, `src/ui/settings_menu.cpp`, `src/app/playlist_playback.{h,cpp}`, `src/ui/channel_guide_view.h`
- Test: `tests/app/test_settings_persistence.cpp`, `tests/app/test_playlist_playback.cpp`, `tests/ui/test_channel_guide_view.cpp`

**Interfaces:**
- Produces: `enum class GuideMode { Live, PickAhead };` `DisplaySettings::guide_mode = GuideMode::Live`, persisted as `display.guide_mode` = `"live"` / `"pick_ahead"` (anything else reads Live). Settings → Display row `Guide Mode: Live` / `Guide Mode: Pick Ahead`, present only while Menu Style is Channel Guide, toggled and saved like Menu Style.
- Produces: `AppState::on_demand` (`bool`, render thread, not persisted).
- Produces: `void PlaylistPlayback::play_on_demand(int channel, const OnAir& airing)` — rides the same switch as `tune` (stop, settle, re-entrancy rules); when the load runs it loads `airing`'s item from its beginning (offset 0, no cut) and sets `AppState::on_demand = true`. Any live load (`tune`, `on_channel_step`, a boundary on a live airing, the post-game and Media Browser re-tunes, a restart) clears it.
- End of an on-demand programme: if the channel's on-air airing now holds the same `ScheduledItem` (same playlist and item) → the airing after that on-air airing, from its beginning; else `join_point` (live); off air → the Task 8 park. Never the programme just watched.
- View: `GuideViewInput::on_demand` → the tuned panel's eyebrow reads `ON DEMAND`.

- [ ] **Step 1: Write the failing tests:** `[settings]` round-trip of `guide_mode`, unknown → Live. `[playlist_playback]` an on-demand load plays from 0 and sets `on_demand`; at its natural end with live airing another item → live join; with live airing the same item → the next item from 0; a channel step during on-demand → live, `on_demand` false; on-demand of the last film before 1:00 AM plays whole, then parks on 00 (Review Focus 5); on-demand of a programme after 4:00 AM tomorrow (the next day's lineup) plays it and then rejoins today's live schedule. `[channel_guide_view]` eyebrow `ON DEMAND`.
- [ ] **Step 2: Run** `test_app_unit "[settings],[playlist_playback]"`, `test_ui_unit "[channel_guide_view]"` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → pass (MB ON and OFF); syntax-check `settings_menu.cpp`.
- [ ] **Step 5: Commit** `feat(guide): Guide Mode setting; on-demand playback with snap-back to live`.

### Task 10: Pick-ahead navigation and view

**Files:**
- Modify: `src/app/guide_nav.{h,cpp}`, `src/ui/channel_guide_view.h`, `src/ui/renderer_channel_guide.cpp`, `src/app/settings_input.cpp` (menu button), `src/main.cpp`
- Test: `tests/app/test_guide_nav.cpp`, `tests/ui/test_channel_guide_view.cpp`

**Interfaces:**
- Consumes: Task 9 `play_on_demand`, `GuideMode`; Task 4 `lineup`.
- Produces: `GuideCursor::pick_at` (`std::optional<double>`: the UTC start of the highlighted programme on `row`; nullopt = not in the row). `GuideEvent::Horizontal` (LEFT/RIGHT, delta ±1) and `GuideEvent::Back` (the menu button). `GuideFacts` gains `bool pick_ahead`, `double now_utc`, `std::vector<OnAir> row_airings` (= `lineup(cursor row, now, now + 24 h)`). `GuideCommand` gains `std::optional<OnAir> on_demand` (main.cpp calls `play_on_demand(cursor.row, *on_demand)`).
- Rules in Pick Ahead (Live mode is exactly today's behaviour): Select on a tunable row outside it enters the row on the on-now airing; Rotate/Horizontal in the row moves one programme per step, clamped at the last airing within 24 h, and a step back before the on-now airing leaves the row; Select on the on-now airing tunes live (today's Tune/Close rules); Select on a later one issues `on_demand`; Back leaves the row and is consumed (Settings does not open); Horizontal outside a row enters it (right) or does nothing (left). `pick_at` is cleared when the guide closes, the mode changes or the row stops being tunable; once the picked airing starts it reads as on now.
- View: the picked airing's cell gets the ring; the time window starts at the guide's usual start, moved later just far enough to show the picked cell whole; the panel names the picked programme and its start time. Same at 960x720 and 640x480.

- [ ] **Step 1: Write the failing tests:** `[guide_nav]` the rules above, one case each, plus: Live mode ignores Horizontal and Back exactly as today; the 24 h clamp; leaving the row by turning back. `[channel_guide_view]` the ring on a programme 6 h ahead with the window moved to show it, at both canvas sizes.
- [ ] **Step 2: Run** `test_app_unit "[guide_nav]"`, `test_ui_unit "[channel_guide_view]"` → fail.
- [ ] **Step 3: Implement**; `main.cpp` builds `row_airings` only for guide input events (never per frame), routes phone/keyboard LEFT/RIGHT and the menu button to `guide_input` while the guide is visible in Pick Ahead.
- [ ] **Step 4: Run** → pass (MB ON and OFF); syntax-check `main.cpp`, `settings_input.cpp`, `renderer_channel_guide.cpp`.
- [ ] **Step 5: Commit** `feat(guide): Pick Ahead — move along a channel's row and start a later programme`.

### Task 11: Keep the movie snapshot private

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/deploy_cpp.sh` (excludes near line 182), `magic_dingus_box_cpp/scripts/update.sh` (the four rsyncs that exclude `media_info.json`, and the comment near line 1963), `scripts/golden_image/prepare_for_cloning.sh` (beside the `media_info.json` step), `scripts/golden_image/first_boot.sh`
- Test: `tests/local/update_rsync_excludes.bats`, the clone bats file that checks `media_info.json`

**Interfaces:**
- Consumes: Task 3's paths `data/movie_library.json` and its temp `data/movie_library.json.tmp`.

- [ ] **Step 1: Write the failing tests:** every rsync in `update.sh` that excludes `data/media_info.json` also excludes `data/movie_library.json` and `.tmp`; `deploy_cpp.sh` likewise; `prepare_for_cloning.sh` and `first_boot.sh` delete both.
- [ ] **Step 2: Run** `bats tests/local/update_rsync_excludes.bats` (and the clone file) → fail.
- [ ] **Step 3: Implement**, each with a one-line comment naming why (the owner's titles).
- [ ] **Step 4: Run** → pass.
- [ ] **Step 5: Commit** `fix(privacy): movie library snapshot never ships, syncs or survives cloning`.

### Task 12: Docs and full verification

**Files:**
- Modify: `CLAUDE.md` (Channel Guide / Media Browser sections: movie channels, daily lineups, off-air window, Pick Ahead, the quiet claim), `CHANGELOG.md` `## [Unreleased]`, `magic_dingus_box_cpp/docs/OWNER_GUIDE.md` if it describes the guide (no betas)
- Create: hardware checklist in the plan workspace (`hardware-checklist.md`)

- [ ] **Step 1:** Document the behaviour as built (rules, times, files, the claim/grace, privacy).
- [ ] **Step 2: Run** every C++ suite MB ON (9) and MB OFF (8), the bats files touched, and the stub syntax check of every kiosk-only file changed on the branch; `git grep -n WatchStore -- src/app` shows no new use.
- [ ] **Step 3:** Hardware checklist for the Pi 5 run: movie channels appear after unlock and disappear on lock; Kids content; a film full screen edge to edge, fill toggle, guide window whole; downloads paused during a film and resumed ~2 min after leaving; drive unplugged mid-film; 1:00 AM park and 6:00 AM return (time-zone trick); Pick Ahead on knob and phone; Pi 4B rehearsal with the policy override (codec gate, FullPause).
- [ ] **Step 4: Commit** `docs: movie channels, daily lineups and Pick Ahead`.
