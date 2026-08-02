# Phase 3 TV Playback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Downloaded TV becomes watchable: series in the Library, an episode picker with watch state, resume, next-episode countdown, season-end offer; movies gain resume/watched too.

**Architecture:** Live Sonarr for episode metadata (fetched at screen-open), a new SQLite `watch_state` table (LibraryDb migration v3) for positions/watched flags, and pure decision logic (`episode_logic.h`) between them. PlaybackScreen stays dumb — SeriesDetail hands it everything (paths, episode vector, season rows, resume point) at the dispatcher handoff.

**Tech Stack:** C++17, sqlite3 (the TEST targets link it today; the kiosk binary does NOT — Task 3 wires the kiosk sources + sqlite3 link), jsoncpp, GStreamer playbin (no new decode paths), Catch2-style house test harness (`tests/media_browser/`).

**Spec:** `docs/superpowers/specs/2026-08-02-tv-playback-design.md` (repo root docs/). Read it once before Task 1.

## Global Constraints

- All new code inside `ENABLE_MEDIA_BROWSER`; the `ENABLE_MEDIA_BROWSER=OFF` binary must stay bit-identical (never touch the OFF path of `settings_persistence.cpp`; no new settings keys needed by this plan).
- Dual-board contract: runtime branching only, never `#ifdef` a board; no new decode paths; the Pi 4B envelope rules (`make -j2` on-Pi).
- Every worker publish gen-checks UNDER its pending mutex (the b70710d invariant). WatchStore is main/render-thread-only by construction — workers never touch it.
- TMDB movie/TV id spaces overlap completely: any persistent or in-memory key that can hold both kinds uses `(kind, tmdb_id)` / `MediaRef`, never a bare int.
- Season 0 (specials) is excluded everywhere, matching `merge_season_rows`.
- Never decide success by reading `last_error()` after a call (cross-thread split read); checked-variant pattern: `nullopt` = didn't answer, engaged-empty = genuinely empty.
- Mock-honesty rule: `SonarrMockClient` overrides every new virtual IN THE SAME TASK; keyless boxes must not fabricate data. The mock lives in `src/media_browser/sonarr/sonarr_mock.{h,cpp}` — the ONLY mock files; grep for `class SonarrMockClient` rather than guessing a filename, and never create a second mock header.
- `sonarr/` types live in the flat `namespace media_browser` — there is NO nested `sonarr` namespace. Write `EpisodeInfo` / `Series` unqualified from `media_browser::ui` code (`media_browser::EpisodeInfo` from outside the namespace).
- Watched threshold: `position/duration >= 0.92`. Resumable: `position >= 60.0` and not past watched threshold. Countdown: 8 seconds. Checkpoint cadence: 30 s.
- Mac test loop — configure FIRST; no `build-mac` exists in a fresh worktree. The exact configure line (the executed 2026-08-01-tv-browse convention): `cmake -S "<worktree>/magic_dingus_box_cpp" -B "<worktree>/magic_dingus_box_cpp/build-mac" -DBUILD_KIOSK=OFF -DBUILD_TESTS=ON -DENABLE_MEDIA_BROWSER=ON`. Re-run it whenever a task adds files to CMakeLists.txt. A plain default configure gets `ENABLE_MEDIA_BROWSER=OFF` (option default OFF, CMakeLists.txt:58) and the ENTIRE MB suite silently drops out — ctest goes vacuously green at 8 targets while none of this plan's code compiles.
- Mac suite must stay green after every task: `cmake --build build-mac -j8 && ctest --test-dir build-mac` → exactly 9 ctest targets, and a green run MUST show the `MediaBrowserUnit` target executing. All MB tests compile into the single `test_media_browser_unit` binary (sources appended to `MEDIA_BROWSER_TEST_SOURCES`, one aggregated `MediaBrowserUnit` ctest target) — the target count stays 9 no matter how many test .cpp files this plan adds. Focused runs use Catch2 tags: `./build-mac/test_media_browser_unit "[episode_logic]"`. Pi compile only in the final task.
- Copy strings verbatim where given (toasts, card labels, hints) — several tests pin them.
- Working directory for all commands: the repo's `magic_dingus_box_cpp/` directory. The repo root path contains a trailing space and emoji — always quote paths.

## File Structure

```
src/media_browser/ui/episode_logic.h            NEW  pure decisions: thresholds, next_up, season-end card, WatchIdentity, format_position_hms
src/media_browser/sonarr/sonarr_types.h         MOD  EpisodeInfo struct
src/media_browser/sonarr/sonarr_client.{h,cpp}  MOD  get_episodes_checked + parser
src/media_browser/sonarr/sonarr_mock.{h,cpp}    MOD  mock override (class SonarrMockClient)
src/media_browser/library/library_db.cpp        MOD  migration v3 (watch_state)
src/media_browser/library/watch_store.{h,cpp}   NEW  prepared-statement wrapper, main-thread-only
src/utils/config.{h,cpp}                        MOD  get_media_db_file()
src/media_browser/ui/playback_screen.{h,cpp}    MOD  origin, start position, watch identity, NextUp/SeasonEnd overlays
src/media_browser/ui/series_detail_screen.{h,cpp} MOD episode view, PlayNextUp, get_play_target, pending intent
src/media_browser/ui/series_detail_logic.h      MOD  Action::PlayNextUp in decide_action_row
src/media_browser/ui/library_screen.{h,cpp}     MOD  mixed entries, MediaRef keys, kind-aware select
src/media_browser/ui/library_view.{h,cpp}       MOD  LibraryEntry-based view, real Unwatched
src/main.cpp                                    MOD  handoffs, checkpoint tick, WatchStore ownership
scripts/verify_services.sh                      MOD  extend check_sonarr_root_folder with accessible==true
scripts/storage_attach.sh (or services unit)    MOD  idempotent TV-root ensure (pre-docker-check placement)
scripts/setup_services.sh                       MOD  TV-dir mkdir alignment + keep-file (${TARGET_USER})
scripts/deploy_cpp.sh                           MOD  rsync --exclude 'data/media_browser.db*'
scripts/install_deps.sh                         MOD  sqlite3 CLI into the apt list
tests/media_browser/test_episode_logic.cpp      NEW
tests/media_browser/test_watch_store.cpp        NEW
tests/media_browser/test_sonarr_client.cpp      MOD  episode parser + mock honesty
tests/media_browser/test_library_view.cpp       MOD  entry-based filters
tests/media_browser/test_series_detail_logic.cpp MOD PlayNextUp rows
CMakeLists.txt                                  MOD  sources into MEDIA_BROWSER_SOURCES + KIOSK_MEDIA_BROWSER_SOURCES, tests into MEDIA_BROWSER_TEST_SOURCES, kiosk sqlite3 link
```

---

### Task 1: `episode_logic.h` pure core

**Files:**
- Create: `src/media_browser/ui/episode_logic.h`
- Test: `tests/media_browser/test_episode_logic.cpp`
- Modify: `CMakeLists.txt` (append `tests/media_browser/test_episode_logic.cpp` to `MEDIA_BROWSER_TEST_SOURCES` at :457-493, where `test_series_detail_logic.cpp` is just a source line — all MB tests compile into the single `test_media_browser_unit` binary; there are NO per-test targets and no block to copy)

**Interfaces:**
- Consumes: `media_browser::EpisodeInfo` (flat `media_browser` namespace — see Global Constraints) — **forward-declared contract; Task 2 defines it with exactly these fields** (`int id; int season_number; int episode_number; std::string title; int runtime_minutes; std::string air_date; bool has_file; int episode_file_id; bool monitored; std::string file_container_path; long long file_size_bytes; std::string file_quality;`). To keep Task 1 independently compilable, `episode_logic.h` templates on the episode type where it only needs `.season_number/.episode_number/.has_file` and takes plain scalars elsewhere.
- Produces (namespace `media_browser::ui`, all header-only, Renderer-free):
  - `constexpr double kWatchedFraction = 0.92;`
  - `constexpr double kResumableMinSeconds = 60.0;`
  - `constexpr int kNextUpCountdownSeconds = 8;`
  - `constexpr int kCheckpointIntervalMs = 30000;`
  - `bool is_watched_position(double position_s, double duration_s)` — false when `duration_s <= 0`.
  - `bool is_resumable_position(double position_s, double duration_s)` — `position >= 60 && !is_watched_position(...)`.
  - `struct WatchKey { int season; int episode; bool operator==(...) };` + hash — the map key SeriesDetail/Playback share.
  - `struct WatchRowLite { double position_s = 0; double duration_s = 0; bool watched = false; };`
  - `using watch_map = std::unordered_map<WatchKey, WatchRowLite, WatchKeyHash>;` — the alias every later task uses.
  - `struct WatchIdentity { MediaRef ref; int season = 0; int episode = 0; };` — declared HERE, once (pure data; include `media_ref.h`, which is GL-free). Tasks 4/5/6 all reference `media_browser::ui::WatchIdentity` from this header — no per-screen redefinition (PlaybackScreen, DetailScreen's PlayTarget, and SeriesDetail's SeriesPlayTarget all carry this one type).
  - `std::string format_position_hms(double position_s)` — pure time formatter, test-pinned: `H:MM:SS` when >= 1 hour, else `M:SS` (e.g. `2:07:03`, `23:14`, `2:05`). Task 4's resume toast and Task 6's `▶` episode glyph both use it (there is no suitable existing helper — `Renderer::format_time` is MM:SS-only with unbounded minutes and needs a `Renderer&`).
  - `template <class Ep> const Ep* next_up(const std::vector<Ep>& episodes, const std::unordered_map<WatchKey, WatchRowLite, WatchKeyHash>& watch, const Ep* current)` — episodes assumed sorted (season asc, episode asc), season 0 entries ignored; when `current != nullptr`, returns the first episode strictly after `current` in that order with `has_file == true` (crossing seasons) — **the watch map is IGNORED in this form**: natural sequential auto-play, a mid-binge rewatch never skips ahead over previously-watched episodes; when `current == nullptr` ("what should I watch next" from the series screen), returns the first episode with `has_file` that is not watched in `watch`, else `nullptr` — only this form consults the watch map.
  - `enum class SeasonEndKind { NextEpisode, OfferNextSeason, Downloading, SeriesDone };`
  - `struct SeasonEndCard { SeasonEndKind kind; int finished_season = 0; int next_season = 0; };` — BOTH season fields are set in every non-NextEpisode outcome (`next_season` also for Downloading and the SeriesDone-with-next-row case; 0 when no next row exists). The pinned titles need both: "Season N finished" uses the FINISHED season, "Season N is on its way" uses the NEXT (downloading) season — deriving one from the other breaks on non-contiguous season numbering.
  - `template <class Ep> SeasonEndCard season_end_card(const std::vector<SeasonRow>& rows, const std::vector<Ep>& episodes, const std::unordered_map<WatchKey, WatchRowLite, WatchKeyHash>& watch, const Ep& finished)` — precedence: if `next_up(episodes, watch, &finished)` exists → `NextEpisode`; else find the smallest `rows[i].season_number > finished.season_number`: none → `SeriesDone`; else exactly: that row `state == SeasonState::Downloading` → `Downloading`; not monitored → `OfferNextSeason`; monitored with no files and not downloading → `Downloading` (search in flight / pending, honest copy "on its way"); `episode_file_count > 0` (files exist but next_up said no ⇒ rows and episodes disagree — stale stats) → `SeriesDone` is the honest fallback. Every non-NextEpisode outcome fills both `finished_season` and `next_season` per the struct note above.
  - `std::string season_end_title(const SeasonEndCard&, const std::string& series_title)` — pinned copy per kind: OfferNextSeason → `"Season <finished_season> finished"`; Downloading → `"Season <next_season> is on its way"` (the NEXT — downloading — season, not the finished one); SeriesDone → `"That's everything!"`.
  - `std::string season_end_button_label(const SeasonEndCard&)` — OfferNextSeason → `"Start Season <next_season>"`; else `"Done"`.

**Steps:**

- [ ] **Step 1: Write the failing tests** — `tests/media_browser/test_episode_logic.cpp` with a local `struct FakeEp { int season_number; int episode_number; bool has_file; };` covering, at minimum:
  - thresholds: `is_watched_position(55, 60)` true (0.9167 < 0.92? NO — 55/60 = 0.9166 → **false**; use `56, 60` → 0.933 true); `(0, 0)` false; `(120, 0)` false; `is_resumable_position(59, 3600)` false, `(61, 3600)` true, `(3550, 3600)` false (watched).
  - `next_up` from current: gap in files (E4 finished, E5 no file, E6 has file → E6); season crossing (S1E10 → S2E1 has_file); nothing after → nullptr; season-0 rows ignored.
  - **watched-overlap pin:** finished S1E10 with S2E1 has_file AND watched in the map → `next_up` returns S2E1 and `season_end_card` → NextEpisode — pins that the `current != nullptr` form IGNORES the watch map (natural sequential auto-play).
  - `next_up` with `current == nullptr`: skips watched-with-file, returns first unwatched-with-file; all watched → nullptr.
  - `season_end_card` matrix: next file exists → NextEpisode; next season unmonitored → OfferNextSeason{finished_season=1, next_season=2}; next season monitored+0 files → Downloading; next season Downloading state → Downloading; no next season → SeriesDone; label pins for all three strings — including the Downloading title using `next_season` (finished S1 with S2 downloading → `"Season 2 is on its way"`, NOT "Season 1").
  - `format_position_hms` pins: `7623` → `"2:07:03"`, `1394` → `"23:14"`, `125` → `"2:05"`.
- [ ] **Step 2: Register the test source FIRST** — append `tests/media_browser/test_episode_logic.cpp` to `MEDIA_BROWSER_TEST_SOURCES` (CMakeLists.txt:457-493) and re-run the Global Constraints configure line. Registration must precede the verify-failure build: an unregistered .cpp compiles into nothing and the failure below would never appear.
- [ ] **Step 3: Run to verify failure** — `cmake --build build-mac -j8 2>&1 | head` fails: `episode_logic.h` not found.
- [ ] **Step 4: Implement `episode_logic.h`** exactly per the Produces block. `SeasonRow`/`SeasonState` come from `#include "series_detail_logic.h"`.
- [ ] **Step 5: Build + run** — `./build-mac/test_media_browser_unit "[episode_logic]"` (Catch2 tag, matching the existing `[sonarr]`-style tags) → PASS; full `ctest --test-dir build-mac` → 9 targets green with `MediaBrowserUnit` executing.
- [ ] **Step 6: Commit** — `git add -A && git commit -m "feat(mb): episode_logic pure core — thresholds, next_up, season-end card"`

### Task 2: SonarrClient episode endpoint + mock honesty

**Files:**
- Modify: `src/media_browser/sonarr/sonarr_types.h` (EpisodeInfo, after the existing minimal `Episode` at :29-36 — keep `Episode` untouched, queue embeds still use it)
- Modify: `src/media_browser/sonarr/sonarr_client.h` (~:235, next to the other checked variants), `sonarr_client.cpp`
- Modify: `src/media_browser/sonarr/sonarr_mock.{h,cpp}` (class `SonarrMockClient` — declarations in the .h, method bodies conventionally in the .cpp; the inline `{ return std::nullopt; }` body in the header is acceptable here, matching the Step 4 snippet)
- Test: `tests/media_browser/test_sonarr_client.cpp` (append)

**Interfaces:**
- Produces: `struct EpisodeInfo` with exactly the fields listed in Task 1's Consumes block; `virtual std::optional<std::vector<EpisodeInfo>> get_episodes_checked(int sonarr_id)`; static parser `static std::vector<EpisodeInfo> parse_episode_list(const std::string& json)` (public static for tests, house pattern).
- Endpoint: `GET /api/v3/episode?seriesId=<id>&includeEpisodeFile=true` via the existing `http_get`. Error contract: `nullopt` ONLY when transport fails (`http_get` returns the empty string); any non-empty body goes through the parser, so a malformed/unparseable body returns **engaged-empty, exactly like `get_library_checked`** (the misclassification is accepted); valid-empty `[]` is also engaged-empty.

**Parsing contract (live-verified 2026-08-02, Sonarr 4.0.19):**
- `hasFile == false` records carry NO `episodeFile` key and `episodeFileId == 0`. `has_file` = json `hasFile` bool; treat missing-key/0 defensively.
- When present: `episodeFile.path` (container-absolute `/data/library/tv/...`) → `file_container_path`; `episodeFile.size` → `file_size_bytes`; `episodeFile.quality.quality.name` → `file_quality`. Each read guarded with `isMember`; absent → empty/0. **NOTE:** the embedded shape is captured from docs, not a live import (zero files existed at design time). Task 9 re-verifies against the first real import and updates the fixture if reality differs.
- `runtime` may be 0 (specials) — store as-is; callers fall back to series runtime.
- Sort the returned vector by (season_number, episode_number) before returning — `next_up` assumes it. Do NOT filter season 0 here (client stays policy-free); UI filters.

**Steps:**

- [ ] **Step 1: Failing parser tests** — append to `test_sonarr_client.cpp`: fixture string with 3 records: S1E1 hasFile=false (no episodeFile key, episodeFileId 0), S1E2 hasFile=true with full episodeFile embed, S0E1 special (runtime 0). Assert field extraction, sort order, no-file defaults. Plus the error contract, exercised via a subclass overriding `http_get` (the `StubSonarr` pattern in this test file): transport failure (`http_get` returns empty string) → `get_episodes_checked` returns nullopt; valid-empty `[]` → engaged-empty; **malformed JSON → `parse_episode_list` returns empty and `get_episodes_checked` returns engaged-empty — NOT nullopt** (exactly like `get_library_checked`; the misclassification is accepted, and the test names that outcome). Mirror `get_library_checked`'s implementation shape verbatim (read it first in `sonarr_client.cpp`).
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** EpisodeInfo + parser + `get_episodes_checked` mirroring `get_library_checked`'s error discipline.
- [ ] **Step 4: Mock honesty** — in `sonarr_mock.h`, `SonarrMockClient` overrides: `std::optional<std::vector<EpisodeInfo>> get_episodes_checked(int) override { return std::nullopt; }` with the standard "keyless boxes must not fabricate" comment. Add a mock-honesty pin test asserting nullopt.
- [ ] **Step 5: Build + full ctest green.**
- [ ] **Step 6: Commit** — `feat(mb): Sonarr episode endpoint (checked) + honest mock`

### Task 3: LibraryDb v3 + WatchStore + config path

**Files:**
- Modify: `src/media_browser/library/library_db.cpp` (MIGRATIONS array)
- Create: `src/media_browser/library/watch_store.{h,cpp}`
- Modify: `src/utils/config.{h,cpp}` (`get_media_db_file()` next to `get_settings_file()` at config.cpp:59-64 — declared in plain `namespace config` (the file lives under src/utils/ but the namespace carries NO `utils` prefix — config.h:6); returns `get_data_path() + "/media_browser.db"`; read config.cpp first and mirror `get_settings_file`'s style)
- Modify: `CMakeLists.txt` (see the CMake wiring bullet below — test lists AND the kiosk target)
- Modify: `scripts/deploy_cpp.sh` (rsync exclude for the DB — see below)
- Test: `tests/media_browser/test_watch_store.cpp` (temp-file DB per test, `test_library_db.cpp` style)

**Interfaces (Produces):**
```cpp
// watch_store.h — namespace media_browser::library
// Main/render-thread-only (LibraryDb is documented non-thread-safe).
// Every method is best-effort: failures log once (spdlog warn) and
// return empty/false — playback must never block on the store.
class WatchStore {
 public:
  struct Row { int season = 0; int episode = 0; double position_s = 0;
               double duration_s = 0; bool watched = false; };
  bool open(const std::string& db_path);   // opens + run_migrations(); false on failure → degraded mode
  bool ok() const;                          // open succeeded
  void upsert_position(const MediaRef& ref, int season, int episode,
                       double position_s, double duration_s);   // also sets watched=1 when is_watched_position()
  void mark_watched(const MediaRef& ref, int season, int episode);
  std::unordered_map<ui::WatchKey, ui::WatchRowLite, ui::WatchKeyHash>
      series_watch(int tmdb_id);            // kind='tv', all rows for series
  std::optional<Row> movie_watch(int tmdb_id);                 // kind='movie', season=episode=0
  std::unordered_set<int> watched_movie_ids();                 // for library filter
  std::unordered_map<int,int> tv_watched_counts();             // tmdb_id -> COUNT(watched=1)
 private:
  LibraryDb db_; bool ok_ = false; bool warned_ = false;
};
```
- Migration v3 SQL exactly as in the spec (`watch_state` + `idx_watch_lookup`), appended to `MIGRATIONS[]` as `{3, "watch_state", ...}`. Bare CREATE TABLE is safe (version-gated, matching v2's convention).
- Upserts via `INSERT INTO watch_state(...) VALUES(...) ON CONFLICT(kind,tmdb_id,season,episode) DO UPDATE SET position_s=excluded.position_s, duration_s=excluded.duration_s, watched=MAX(watched,excluded.watched), updated_at=excluded.updated_at` — watched is a ratchet; a rewatch never un-watches.
- `updated_at` = `time(nullptr)`.
- **CMake wiring (all of it lands in this task):**
  1. `src/media_browser/library/watch_store.cpp` into `MEDIA_BROWSER_SOURCES` near `library_db.cpp` at :414, and `tests/media_browser/test_watch_store.cpp` into `MEDIA_BROWSER_TEST_SOURCES` (no new test target — the aggregated `test_media_browser_unit` binary picks it up).
  2. Append `src/media_browser/library/library_db.cpp` AND `src/media_browser/library/watch_store.cpp` to `KIOSK_MEDIA_BROWSER_SOURCES` (:226-268). The kiosk binary is `add_executable(magic_dingus_box_cpp ${ALL_SOURCES} ${KIOSK_MEDIA_BROWSER_SOURCES})` (:275) — `MEDIA_BROWSER_SOURCES` feeds ONLY the two test targets, so without this Task 4's `WatchStore watch_store;` in main.cpp link-fails with undefined `LibraryDb::*`/`WatchStore::*` at Task 9's Pi build. Precedent: `library_view.cpp`/`mb_recs.cpp` appear in BOTH lists with a dual-list comment. Update the stale ":221-223 'Torrent/library-db stay test-only for now'" comment.
  3. Inside the same `BUILD_KIOSK`+`ENABLE_MEDIA_BROWSER` block, add `pkg_check_modules(SQLITE3 REQUIRED sqlite3)` next to the existing `pkg_check_modules(CURL REQUIRED libcurl)` (:270-272) and add `${SQLITE3_INCLUDE_DIRS}`/`${SQLITE3_LIBRARY_DIRS}`/`${SQLITE3_LIBRARIES}` to the kiosk target — today only the test targets consume the :395 `pkg_check_modules(SQLITE3)`; the kiosk links no sqlite3 at all.
- **`scripts/deploy_cpp.sh`:** insert `--exclude 'data/media_browser.db*'` (the glob covers the `-wal`/`-shm` sidecars) into the rsync exclude block next to the other `data/` persistence excludes — the `text_input_queue.jsonl` precedent. Without it, `rsync --checksum --delete` wipes the Pi's watch DB on every deploy: all resume positions and watched flags silently destroyed by the very deploys Task 9 runs.
- **Accepted v1 behavior (record, don't fix):** watch rows are never garbage-collected, so `tv_watched_counts` may overcount vs the CURRENT files after episodes are deleted/re-sourced in Sonarr — a series can read fully-watched while its current files are unwatched. Unwatched can under-show a re-sourced series; it self-corrects as new files are watched. Deliberate tradeoff; Task 7 inherits it.

**Steps:**

- [ ] **Step 1: Failing tests** — fresh temp DB: open → schema_version()==3; v2→v3 upgrade path (open a db, run only through v2 by… simplest: open once with today's code creates v3 — for the upgrade test, hand-create a v2 DB via `LibraryDb::exec` of the v2 SQL + version rows, then WatchStore::open upgrades to 3). Upsert then read back; watched ratchet (position back to 0 doesn't clear watched); threshold auto-set at 0.93; movie_watch keying; watched_movie_ids/tv_watched_counts aggregation with mixed kinds sharing a tmdb_id (the collision pin: movie 1396 watched must NOT count toward tv 1396); degraded mode: open("/nonexistent-dir/x.db") → ok()==false, all reads empty, no crash.
- [ ] **Step 2: Verify failure** (register `test_watch_store.cpp` in `MEDIA_BROWSER_TEST_SOURCES` first, then build). **Step 3: Implement (migration + store + config helper + all three CMake wiring items + the deploy_cpp.sh exclude; `bash -n scripts/deploy_cpp.sh`). Step 4: full ctest green (target count stays 9). Step 5: Commit** — `feat(mb): LibraryDb v3 watch_state + WatchStore`

### Task 4: Playback resume + watch checkpoints (movies complete here)

**Files:**
- Modify: `src/media_browser/ui/playback_screen.{h,cpp}`
- Modify: `src/main.cpp` (WatchStore ownership, Detail→Playback handoff at :2306-2321, checkpoint tick near the status writer at :3821-3826, exit-path writes via `flush_watch_state` at ALL THREE Playback exit sites — see below)
- Modify: `src/media_browser/ui/detail_screen.cpp` (`get_play_target()` at :1037-1078 — resume position + identity)
- Test: `tests/media_browser/test_episode_logic.cpp` (checkpoint-decision helpers if any new pure logic emerges; otherwise no new Mac test — this task's verification is compilation + the Task 9 hardware loop). Keep any new decision (e.g. "should this tick write?") pure and tested.

**Interfaces:**
- Consumes: `WatchStore` (Task 3), `is_watched_position/is_resumable_position` (Task 1).
- Produces on `PlaybackScreen`:
  - Uses `media_browser::ui::WatchIdentity` from `episode_logic.h` (declared once in Task 1 — do NOT redefine it here or on any screen).
  - `void set_origin(Screen s)` — replaces both hardcoded `return Screen::Detail;` sites (`playback_screen.cpp:222-225` exit_pending path, `:240` BTN4 path) with `return origin_;`. Default `Screen::Detail` (belt-and-braces).
  - `void set_start_position(double s)` — one-shot; `enter()` passes it as the start parameter of `controller_.load_file_with_resolution(movie_path_, "", start_position_, 0.0, false)` (the param exists, currently hardcoded 0.0 at playback_screen.cpp:~120); cleared in `leave()`.
  - `void set_watch_identity(std::optional<WatchIdentity> id)`.
  - `std::optional<WatchIdentity> watch_identity() const` + `std::optional<WatchIdentity> take_eos_watched()` — the EOS accessor is **consume-once**: it returns the engaged identity exactly once per EOS latch (internal `eos_reported_` flag, reset together with `eos_latched_` on `enter()`/in-place reload). main.cpp calls it each frame and writes `mark_watched` only when it returns engaged — never a per-frame SQLite write while a countdown or season-end card idles on screen. EOS detection stays inside PlaybackScreen (the `video_active` edge at :439-442); it additionally latches `eos_latched_` this task adds, cleared on `enter()`/in-place reload (Task 5 rewrites this edge for TV — exit_pending_ suppression).
- Produces in `main.cpp`:
  - `media_browser::library::WatchStore watch_store;` constructed with the MB screens (main.cpp:902-928 region), `watch_store.open(config::get_media_db_file())` (plain `config::`, matching main.cpp's existing `config::get_data_path()` call at :504) — inside `#ifdef MEDIA_BROWSER_ENABLED`. Failure tolerated (`ok()==false` → everything degrades).
  - Checkpoint tick: in the per-frame MB block, when `current_mb_screen == Screen::Playback` and identity engaged: every `kCheckpointIntervalMs` write `watch_store.upsert_position(id.ref, id.season, id.episode, state.get_position(), state.get_duration())`. Static `last_checkpoint` time_point, same idiom as `last_status_write` (:505-508).
  - **Exit write — one helper, three sites, always PRE-`leave()`:** a single `flush_watch_state(mb_playback, watch_store, state)` helper reads `mb_playback.watch_identity()` + `state.get_position()`/`state.get_duration()` and upserts, called at ALL THREE Playback exit paths in main.cpp: (a) the BTN4 long-press hard-exit (:2240-2251), (b) the `Screen::Exit` branch (:2254-2262), and (c) the normal transition — in the pre-leave handoff region (same region as :2306-2321), i.e. when `next != Playback && current_mb_screen == Playback`, BEFORE `active_mb_screen->leave()` at :2333. **Never write after `leave()` and never a frame later:** `stop()` zeroes position/duration and `Controller::update_state` copies the zeros into AppState, so a post-leave or next-frame write calls `upsert_position(..., 0, 0)` and clobbers the resume point (the MAX() ratchet protects only `watched`, not position). The write executes in the SAME frame as the transition (`update_state` ran at frame top, so the read is valid). Skip the write when identity is disengaged, or when position reads 0 while a previous checkpoint holds a nonzero position.
  - Movie EOS: when `take_eos_watched()` returns an engaged identity with kind==Movie → `mark_watched` (upsert with duration as position is fine too — use `mark_watched`).
- `DetailScreen::get_play_target()` additions: `pt.resume_position` (from `watch_store.movie_watch(tmdb_id)` → `is_resumable_position` ? position : 0.0) and `pt.watch_identity = WatchIdentity{MediaRef{MediaKind::Movie, tmdb_id}, 0, 0}`. DetailScreen gains a `WatchStore* watch_ = nullptr` ctor param (main.cpp passes `&watch_store`; null-safe: null → no resume). The dispatcher handoff (:2306-2321) forwards both: `mb_playback.set_start_position(pt.resume_position); mb_playback.set_watch_identity(pt.watch_identity); mb_playback.set_origin(Screen::Detail);`
- Resume toast (pinned copy): when `enter()` starts with `start_position_ > 0`, `::ui::Toast::show("Resuming from " + format_position_hms(start_position_) + " — seek back to restart")` — `format_position_hms` is the pure Task 1 helper (`H:MM:SS` when >= 1 h, else `M:SS`); Task 6's `▶` episode glyph uses the same helper, so the two surfaces cannot drift.

**Steps:**

- [ ] **Step 1:** Read `playback_screen.cpp` enter/leave/handle_input and main.cpp:2306-2360 + :3821-3850 in full before editing.
- [ ] **Step 2:** Implement PlaybackScreen members (origin_, start_position_, watch_identity_, eos_latched_, eos_reported_) + the two return-site swaps + enter/leave wiring + resume toast.
- [ ] **Step 3:** Implement main.cpp ownership + handoff + checkpoint tick + `flush_watch_state` at all three exit sites (pre-`leave()`, same-frame) + movie-EOS watched via `take_eos_watched()`.
- [ ] **Step 4:** DetailScreen ctor param + get_play_target additions (read the PlayTarget struct at detail_screen.h:113-127 and extend it).
- [ ] **Step 5:** Full Mac build + ctest green (playback_screen compiles on Mac? **It does NOT** — kiosk-only TU. Mac gate = the MB test suite + everything that includes the headers; the kiosk TUs' syntax gate is the Task 9 Pi build. Keep header changes self-contained so test TUs including `playback_screen.h` still compile — check whether any test does; if none, note it.)
- [ ] **Step 6: Commit** — `feat(mb): playback resume + watch checkpoints; movies track watched`

### Task 5: Episode EOS machinery — NextUp countdown + season-end card

**Files:**
- Modify: `src/media_browser/ui/playback_screen.{h,cpp}`
- Test: `tests/media_browser/test_episode_logic.cpp` (countdown/card decisions are already pure from Task 1; add the `EndOverlayModel` resolver tests below)

**Interfaces:**
- Produces (pure, added to `episode_logic.h`):
  - `enum class EndOverlayKind { None, Countdown, Card };`
  - `struct EndOverlayModel { EndOverlayKind kind; std::string title_line; std::string body_line; std::string primary_label; bool has_primary; SeasonEndCard card; int next_index = -1; };` — `next_index` is the position of the next episode in the episodes vector (`-1` when none); typed, testable, no pointers.
  - `template <class Ep> EndOverlayModel decide_end_overlay(const std::vector<SeasonRow>& rows, const std::vector<Ep>& episodes, const watch_map&, const Ep& finished, const std::string& series_title)` — NextEpisode → Countdown with `title_line = "Next: S<em>E<n> · <title>"`, `primary_label = "Play now"`; otherwise Card with the Task 1 title/button strings and `body_line` copy: OfferNextSeason → `"Start the Season <next_season> download?"`, Downloading → `"Check the Queue for progress."`, SeriesDone → `""`.
- Produces on `PlaybackScreen`:
  - `void set_episode_context(std::vector<EpisodeInfo> episodes, std::vector<std::string> host_paths, std::vector<SeasonRow> rows, watch_map watch, std::string series_title)` — handed by the dispatcher (Task 6); `host_paths` is index-aligned with `episodes` (empty string when no file), pre-resolved by SeriesDetail so playback never touches SonarrClient. Cleared on `leave()`.
  - **EOS-edge rewrite (the load-bearing change):** at the `video_active` EOS edge in `update()` (playback_screen.cpp:439-442), when the watch identity is engaged and kind==Tv, latch `eos_latched_` ONLY — do **NOT** set `exit_pending_`; the movie/no-identity path keeps `exit_pending_ = true` exactly as today. The end-overlay input handling in `handle_input` runs while `eos_latched_ && !exit_pending_` — the existing `exit_pending_` fast-return at :223-225 stays first and now fires only for movies/load-failures. For TV, `exit_pending_` is set only by the overlay's own outcomes (RED, Done, missing-file). Without this rewrite the countdown is unreachable: the edge would arm both flags and `handle_input` would return `origin_` on the next frame. A load failure during in-place advance falls back to `exit_pending_ = true` + deferred toast (the `enter()` precedent).
  - **Locating the finished episode:** PlaybackScreen resolves `finished` by searching `episodes_` for `(identity.season, identity.episode)`; on no match (stale vector, identity advanced, S/E absent) it skips the overlay entirely and takes the movie-style exit (`exit_pending_` → `origin_`). The in-place advance updates a cached `current_index_` and re-validates it the same way at the next EOS — never index blindly.
  - End-overlay state machine inside update/render/handle_input, active only when identity kind==Tv and `eos_latched_`:
    - Enter overlay: mark current watched via a new main-thread callback? **No** — keep the store write in main.cpp (it owns WatchStore): main.cpp's `take_eos_watched()` returns the Tv identity (exactly once per latch) → `mark_watched(...)`. The map lives inside PlaybackScreen's context. Simplest correct split: main.cpp writes the store; PlaybackScreen updates its own in-memory `watch_` copy (`watch_[key].watched = true`) before deciding the overlay — in-memory and persistent stay consistent without playback touching SQLite.
    - Countdown: 8s (frame-timer, same clock idiom as confirm timers in series_detail_screen.h:135-138). Render: dim scrim + title_line + `"Starting in N…"` + hint row {RotaryPress "Play now", Btn2Red "Stop"}. SELECT/expiry → **in-place advance**: `controller_.stop()`, set `movie_path_` = resolve of next ep (path handed pre-resolved — see Task 6: `EpisodeInfo.file_container_path` is resolved to host path by SeriesDetail before handoff? NO — playback receives the episode vector with container paths; it must not call SonarrClient. **Decision: the dispatcher hands a parallel `std::vector<std::string> host_paths` aligned with the episode vector**, resolved by SeriesDetail's get_play_target via `sonarr_.resolve_host_path` for every has_file episode at handoff time), update identity (season/episode) + the cached `current_index_`, `set_start_position(0)`, reset `eos_latched_`/`eos_reported_` + `eos_suppress_frames_ = 60`, `load_file_with_resolution(...)`, and update the overlay title WITHOUT wiping meta — **`set_movie()` RESETS `overlay_meta_` (playback_screen.cpp:44-46); never call it bare mid-session.** The advance sequence is: `auto meta = overlay_meta_; set_movie(next_host_path, new_title); meta.title = new_title; set_movie_meta(std::move(meta));` (new_title = `"<series> — S<em>E<n> · <ep title>"`; poster/synopsis/cast survive). `enter()` is NOT re-run on in-place advance, so re-arm its side effects explicitly: `title_marquee_until_`, `eos_suppress_frames_ = 60`, `was_video_active_`. RED → return `origin_` (SeriesDetail).
    - Card: render title/body/buttons; primary (when has_primary) fires: OfferNextSeason → set `pending_intent_out_ = next_season` and return `Screen::SeriesDetail`; Done/RED → return `Screen::SeriesDetail`.
  - `std::optional<int> take_pending_next_season()` — consumed by the dispatcher on the Playback→SeriesDetail transition (Task 6 wires the receiving side).
  - File-existence guard on advance: `std::filesystem::exists(next_host_path)`; missing → toast `"File missing on disk"` and fall through to card/exit.
- **Movie behavior untouched** — every new branch gates on identity kind==Tv.

**Steps:**

- [ ] **Step 1:** Failing tests for `decide_end_overlay` (Countdown line format pin `"Next: S1E5 · The Wolf and the Lion"`, card bodies, has_primary matrix). First extend the shared `FakeEp` in `test_episode_logic.cpp` with a trailing `std::string title;` member — append-only, so Task 1's brace-inits stay valid (`title_line` makes the template reference `ep.title`; `runtime` is not needed).
- [ ] **Step 2:** Implement resolver in episode_logic.h; ctest green.
- [ ] **Step 3:** Implement PlaybackScreen state machine (incl. the EOS-edge rewrite / exit_pending_ suppression for TV) + rendering + in-place advance (kiosk TU — Pi-gated).
- [ ] **Step 4:** Full Mac ctest green. **Step 5: Commit** — `feat(mb): next-episode countdown + season-end card in playback`

### Task 6: SeriesDetail episode picker + PlayNextUp + handoffs

**Files:**
- Modify: `src/media_browser/ui/series_detail_logic.h` (Action::PlayNextUp in `decide_action_row`)
- Modify: `src/media_browser/ui/series_detail_screen.{h,cpp}`
- Modify: `src/main.cpp` (SeriesDetail→Playback handoff; Playback→SeriesDetail intent consumption; Library-producer guard comes in Task 7)
- Test: `tests/media_browser/test_series_detail_logic.cpp` (action-row), `tests/media_browser/test_episode_logic.cpp` (row-format helper if extracted)

**Interfaces:**
- `series_detail_logic.h`: `Action::PlayNextUp` added BEFORE AddSeason1 in the enum; `ActionRowInputs` gains `bool has_next_up; int next_up_season; int next_up_episode; bool next_up_is_first;` — `decide_action_row` emits PlayNextUp as the FIRST button when `state==InLibrary && has_next_up`, label `"Continue S<em>E<n>"` or `"Start watching S<em>E<n>"` (when `next_up_is_first`, i.e. nothing watched yet). Focus preservation by action identity extends naturally; forced-focus chain tests updated (the `prev_row_remove_only` machinery must survive a row that now starts with PlayNextUp).
- `SeriesDetailScreen`:
  - Episode view state: `enum class DetailRegion { Seasons, Episodes };` + `episodes_season_` (which season is open), `episodes_` (fetched vector), `episode_watch_` (map from WatchStore — see the watch-state join rule below), `episode_focus_`, `episode_page_` (reuse the season paging idiom with per_page>0 guards). SELECT on a season row with `episode_file_count > 0` → Episodes region for that season; BTN4 in Episodes → back to Seasons (does NOT leave the screen); BTN4 in Seasons → existing back behavior.
  - Episode fetch: new FetchWorker lane via `get_episodes_checked(sonarr_id)` following the run_sonarr_fetch pattern — the episode worker **CAPTURES `gen = fetch_gen_.load()` at spawn WITHOUT bumping** (`fetch()` remains the only bumper of `fetch_gen_`; a bump here would invalidate any in-flight tmdb/sonarr worker for the SAME series, whose publish then gen-mismatches and the screen wedges in Loading) and gen-checks under `pending_mtx_` before publishing, exactly like `run_sonarr_fetch`; guard double-spawns with an `episodes_inflight_` CAS flag (the `poll_inflight_` idiom). Fetched once per series load (not per season — the endpoint returns all seasons); refetched when file counts change (compare `episode_file_count` totals; cheap trigger, avoids hammering) — **the file-count comparison AND the refetch spawn run on the render thread inside `apply_pending()` after the 9s poll's `series_` lands, never inside `run_series_poll`** (spawning from the worker thread races the render-thread-only `workers_` vector).
  - Watch-state join: join `watch_->series_watch(tmdb_id_)` into `episode_watch_` (and rebuild episode rows + the action row) on EVERY `apply_pending()` drain AND unconditionally in `enter()` when `tmdb_id_` is unchanged (cheap indexed read; WatchStore is main-thread-only and `enter()` runs on the render thread, so the direct read is legal). Re-entry from Playback must show fresh `✓`/`▶` glyphs and the updated PlayNextUp label immediately — the Playback→SeriesDetail return never calls `set_tmdb_id`, so `needs_refresh_` stays false and no fetch fires; without this join the screen shows pre-playback state until the next 9s poll drains, and hardware acceptance step 3 fails. The same re-join recomputes `next_up` for the PlayNextUp label.
  - Episode-region outage rendering: track `episodes_done_`/`episodes_ok_` from the drain — `nullopt` → the region renders the existing Sonarr outage line (reuse the `series_detail_state_message(SonarrUnreachable)` "Sonarr service offline" idiom); engaged-empty → `"No episodes"`. SELECT on a season row still opens the region in both cases; season rows (TMDB-based) render regardless.
  - Episode row render: `E<n> · <title> · <runtime>m` + state glyph: `✓` watched, `▶ <format_position_hms(position)>` resumable (the Task 1 helper — same formatter as the resume toast), `·` unwatched-with-file, dimmed no-file (+ `"downloading"` suffix when that season's state==Downloading). SELECT on fileless row → toast `"Not downloaded yet"`. SELECT on a has_file row guards `std::filesystem::exists(host_path)` before returning Playback (the do_play precedent, detail_screen.cpp:1078-1094) → toast `"File missing on disk"` on failure.
  - `struct SeriesPlayTarget { std::string host_path; std::string display_title; double resume_position; int year; int runtime_min; std::string synopsis; std::string poster_url; std::string genres; WatchIdentity identity; std::vector<EpisodeInfo> episodes; std::vector<std::string> host_paths; std::vector<SeasonRow> rows; watch_map watch; std::string series_title; }` (`WatchIdentity` is the Task 1 `episode_logic.h` type; meta fields mirror `PlaybackOverlayMovieMeta` at playback_overlay.h:20-31, filled from the TMDB TV detail) — `get_play_target()` builds it for the focused episode (or next_up for PlayNextUp), resolving `host_paths[i] = sonarr_.resolve_host_path(episodes[i].file_container_path)` for has_file episodes (empty string otherwise), display_title `"<series> — S<em>E<n> · <ep title>"`, resume via `episode_watch_` + `is_resumable_position`.
  - `void set_pending_intent_next_season(int season)` + consumption in `enter()`: runs the EXISTING NextSeason path — set the season via the same `dispatch_action(Action::NextSeason)` flow (read dispatch_action first; it derives the target season from `next_unmonitored_season` — assert the intent season matches, else toast and no-op; drift means the world changed while playing, safest is no-op with `"Season already started"` toast when already monitored).
  - `WatchStore* watch_ = nullptr` ctor param (null-safe), used at drain to join `series_watch(tmdb_id)`.
- `main.cpp` dispatcher:
  - `next == Screen::Playback && current_mb_screen == Screen::SeriesDetail` branch mirroring :2306-2321: pull `SeriesPlayTarget`, call `set_movie(host_path, display_title)`, `set_movie_meta(...)` (poster from TMDB detail), `set_start_position`, `set_watch_identity`, `set_origin(Screen::SeriesDetail)`, `set_episode_context(episodes→with host_paths, rows, watch, series_title)`.
  - `next == Screen::SeriesDetail && current_mb_screen == Screen::Playback`: `if (auto s = mb_playback.take_pending_next_season()) mb_series_detail.set_pending_intent_next_season(*s);` — place this branch in the **pre-leave handoff region** alongside the existing `next == SeriesDetail` block at :2293-2301, i.e. BEFORE `active_mb_screen->leave()` at :2333: Task 5's `leave()` may clear `pending_intent_out_`, so the take must precede `leave()` (order: take → leave() clears → enter() consumes the intent already stored on mb_series_detail). SeriesDetail's tmdb identity is already set (playback never changes it).

**Steps:**

- [ ] **Step 1:** Failing action-row tests (PlayNextUp first, labels pinned, focus chains incl. remove-only escape).
- [ ] **Step 2:** Implement logic-header changes; ctest green.
- [ ] **Step 3:** Implement screen: region state, fetch lane, render, get_play_target, intent.
- [ ] **Step 4:** Dispatcher branches in main.cpp.
- [ ] **Step 5:** Full Mac ctest green. **Step 6: Commit** — `feat(mb): episode picker + PlayNextUp + playback handoffs`

### Task 7: Mixed Library — entries, MediaRef keys, real Unwatched

**Files:**
- Modify: `src/media_browser/ui/library_view.{h,cpp}` + Test: `tests/media_browser/test_library_view.cpp`
- Modify: `src/media_browser/ui/library_screen.{h,cpp}`
- Modify: `src/main.cpp` (LibraryScreen ctor gains `SonarrClient*` + `WatchStore*`; SeriesDetail-producer guard extends to Library)

**Interfaces:**
- `library_view.h` (pure layer first — this is where the tests live):
  - `struct LibraryEntry { MediaRef ref; std::string title; int year; std::string poster_url; std::string added_at; int file_count; int total_count; bool downloading; bool watched;  // movies: watched flag; tv: watched == (watched_count >= file_count && file_count > 0)
      const Movie* movie = nullptr; const Series* series = nullptr; };`
  - `std::vector<LibraryEntry> build_library_entries(const std::vector<Movie>&, const std::vector<Series>&, const std::unordered_set<int>& watched_movie_ids, const std::unordered_map<int,int>& tv_watched_counts, const std::unordered_set<MediaRef>& downloading_refs)` — movie entry per Movie (file_count = has_file?1:0, total 1, watched from set); series entry ONLY when `episode_file_count > 0 || downloading_refs.count(tv ref)` (spec inclusion rule); sorted stays caller's job.
  - `build_library_view` and `library_row_kept` re-typed over `LibraryEntry` (keep names; update the Movie-specific recent-cutoff logic to use `entry.added_at`). Filter semantics over entries, all pinned by tests: **Unwatched** = `keep = !e.watched;` — one line for BOTH kinds via the precomputed `entry.watched`; a 0-file downloading series has `watched == false` so it IS KEPT (deliberately superseding the spec's `watched_episode_count < episodeFileCount` formula, which would drop it); movies with no file: watched=false, kept. **MissingFiles** for TV = `file_count < total_count` (movies keep today's `!has_file` meaning via file_count). **Sort::Size** reads `movie->file_size_bytes` / `series->size_on_disk_bytes` through the entry pointers. Remember Task 3's accepted v1 note: watch rows never GC, so a re-sourced series can under-show in Unwatched until rewatched.
- `library_screen`: `run_refresh()` adds `sonarr_ ? sonarr_->get_library_checked() : nullopt` (worker thread — allowed, it's the client not the store) AND, when `sonarr_` is non-null, `sonarr_->get_queue_checked()` for the TV downloading refs: map each queue item's series id to tmdb via the fetched series vector (`Series` carries both sonarr_id and tmdb_id) and insert `MediaRef{Tv, tmdb}` into the downloading set (`nullopt` → treat as empty, same as the movie queue today) — without this the inclusion rule has no source for a freshly-started 0-file season download, so it never appears in the Library and no TV tile ever shows DOWNLOADING. PendingResult carries the series vector + the TV downloading refs + a sonarr_ok flag (outage → TV entries absent + the existing warning-line idiom from queue_screen as precedent). Watch data (`watched_movie_ids`, `tv_watched_counts`) is read from WatchStore ON THE RENDER THREAD at drain time (`apply_pending`), then `build_library_entries` + `rebuild_view()`. The three id-sets (`downloading_tmdb_ids_`/`stuck_tmdb_ids_`/`importing_tmdb_ids_`) PLUS their PendingResult mirrors (library_screen.h:134-149 and :158-163 — six declarations to re-key, not four) re-key to `std::unordered_set<MediaRef>`; `selected_tmdb_id_` becomes `selected_ref_` (`MediaRef selected_ref() const`); SELECT returns `ref.kind == MediaKind::Tv ? Screen::SeriesDetail : Screen::Detail` (browse_screen.cpp:1577-1589 pattern). TV poster cells: reuse `draw_poster_card` with series poster_url + a small `TV` chip glyph (draw after the card, top-left, `th.accent` on dim quad — match the Browse badge idiom, grep `IN LIBRARY` badge in browse_screen for the chip drawing pattern).
- `main.cpp`: LibraryScreen ctor passes `sonarr_key.empty() ? nullptr : &sonarr` (mock-phantom guard, QueueScreen precedent at its construction site) + `&watch_store`; the SeriesDetail handoff guard at :2296-2301 extends: `current_mb_screen == Screen::Browse || current_mb_screen == Screen::Library`, pulling the tmdb id from the respective screen's selected accessor. ALSO update the Library branch of the existing Detail-producer guard at main.cpp:2273-2275 — `mb_detail.set_tmdb_id(mb_library.selected_tmdb_id())` becomes `mb_library.selected_ref().tmdb_id` (guard `kind == MediaKind::Movie`; only movies reach Detail) — the rename breaks this kiosk-only call site and the miss would surface only at Task 9's Pi build.

**Steps:**

- [ ] **Step 1:** Failing `test_library_view.cpp` rewrites: entry building (inclusion rule: 0-file non-downloading series absent; downloading 0-file present with downloading=true), Unwatched matrix (movie watched/un, tv fully/partially watched, tv no-file-downloading → KEPT — the `!watched` rule), MissingFiles-for-TV pin (`file_count < total_count`), Size-sort via the entry pointers, movie/tv same-tmdb-id collision pin (movie 1396 watched must not mark tv 1396), recent-cutoff still works on entries, empty-poster fallback.
- [ ] **Step 2:** Verify failure. **Step 3:** Implement view layer; ctest green.
- [ ] **Step 4:** Implement screen + main.cpp wiring (kiosk TU parts Pi-gated).
- [ ] **Step 5:** Full Mac ctest green. **Step 6: Commit** — `feat(mb): mixed movie+TV Library with real Unwatched`

### Task 8: Ops hardening — TV root ensure + verify assertion

**Files:**
- Modify: `scripts/storage_attach.sh` (the mount-activation unit's script — insert the ensure immediately after the `mountpoint -q "$STORAGE_ROOT"` guard at :45-48 and BEFORE the docker-readiness checks at :53-61, unconditional and idempotent; the script early-exits when docker isn't ready or mdb_radarr isn't running, which is exactly the early-boot mount-activation state this ensure exists for — placed after those guards it would miss every boot that matters. The ensure needs only the mount, not docker)
- Modify: `scripts/setup_services.sh` (locate the existing TV-dir mkdir from Phase 2a at :173-178; align it to also drop the keep-file, using `${TARGET_USER}` for ownership as that script already does)
- Modify: `scripts/verify_services.sh` (EXTEND the existing `check_sonarr_root_folder()` — no new check)
- Modify: `scripts/install_deps.sh` (add `sqlite3` — the CLI — to the apt list: `libsqlite3-dev` at :101 ships only headers, and Task 9's acceptance steps shell out to the `sqlite3` binary on the box)

**Interfaces / exact changes:**
- Ensure block (both scripts, idempotent):
```bash
# Sonarr's root folder must exist before any import fires. It has been
# destroyed twice by an unidentified empty-dir sweep (2026-08-02); the
# keep-file makes it permanently non-empty so sweeps cannot match it.
install -d -o magic -g magic /mnt/ssd/library/tv
[ -f /mnt/ssd/library/tv/.mdb-keep ] || { touch /mnt/ssd/library/tv/.mdb-keep && chown magic:magic /mnt/ssd/library/tv/.mdb-keep; }
```
(In storage_attach.sh derive the path as `${STORAGE_ROOT}/library/tv` from its existing variables — read the script first and use its conventions; hardcode nothing it already parameterizes. storage_attach.sh has no user variable, so `magic:magic` stays hardcoded THERE ONLY; in setup_services.sh use `${TARGET_USER}` for ownership, matching :173-178.)
- verify_services.sh: **EXTEND the existing `check_sonarr_root_folder()` at verify_services.sh:339-356 — do NOT add a parallel check.** It already curls `/api/v3/rootfolder` with the house `curl -fsS` + python `sys.exit` idiom and asserts the `/data/library/tv` record EXISTS (presence only); add the missing half: additionally fail unless every returned root folder reports `accessible == true`, keeping its curl/python/skip idiom unchanged. Failure message: `"Sonarr root folder missing or inaccessible — run storage_attach or check /mnt/ssd/library/tv"`. Its existing SONARR_API_KEY soft-skip (pre-2a boxes) already covers the skip case.

**Steps:**

- [ ] **Step 1:** Read all four scripts' relevant sections. **Step 2:** Implement (incl. the `sqlite3` apt-list addition in install_deps.sh). `bash -n` each script (syntax gate).
- [ ] **Step 3:** Commit — `fix(ops): TV root folder ensure + verify assertion`

### Task 9: Pi build, deploy, hardware acceptance

**Files:** none new — build + verify. (Any fixture drift found here patches Task 2's parser + fixtures in a follow-up commit.)

**Steps:**

- [ ] **Step 1: Pi scratch compile** BOTH flag states — rsync worktree to `magic@magicpi5.local:/home/magic/scratch-builds/tvplay/` (assets/ must sync or cmake fails late), `cmake -DENABLE_MEDIA_BROWSER=ON` then `OFF`, `setsid nohup sh -c "make -C build -j2 ...; echo EXIT=\$?" > build.log 2>&1 < /dev/null &` and poll the log (bare nohup dies with the SSH session; never `pgrep -f`). Expect EXIT=0 both.
- [ ] **Step 2: Real-import fixture verification** — GoT S1 should be imported by now. `curl` the live `/api/v3/episode?seriesId=6&includeEpisodeFile=true`, extract one hasFile=true record, diff its `episodeFile` shape against Task 2's fixture; if fields differ (path/size/quality nesting), fix parser + fixture, re-run Mac suite, commit `fix(mb): episode parser vs real import shape`.
- [ ] **Step 3: Deploy** — merge to main happens via the finishing skill AFTER review; deploy with `PI_HOST=magic@magicpi5.local ./scripts/deploy_cpp.sh --build` (NEVER default PI_HOST — it targets a different box). Do not restart the kiosk while a family session is active (check `now_playing` first).
- [ ] **Step 4: Hardware acceptance loop** (kmsgrab + phone-remote uinput rig; stop magic-dingus-web first, kill injector by PID):
  1. Library shows GoT tile with TV chip alongside movies; Unwatched filter keeps it.
  2. Open series → PlayNextUp says "Start watching S1E1"; episode view shows 10 rows with file glyphs.
  3. Play E1 ~2 min → BTN4 out → row shows `▶ 2:xx`; reopen → PlayNextUp "Continue S1E1"; play resumes with the pinned toast.
  4. Seek near end → EOS → countdown overlay `"Next: S1E2 · The Kingsroad"` → auto-advance in place; RED during a later countdown returns to series screen.
  5. `watch_state` rows visible via `sqlite3 data/media_browser.db 'SELECT * FROM watch_state'` — kind='tv' rows with sane positions; watched=1 on E1. (The `sqlite3` CLI comes from Task 8's install_deps.sh addition; on an image that predates it, `sudo apt-get install -y sqlite3` first.) Then re-run `PI_HOST=magic@magicpi5.local ./scripts/deploy_cpp.sh` and confirm the rows SURVIVE the deploy — this exercises Task 3's `data/media_browser.db*` rsync exclude; before it, `rsync --delete` wiped the DB on every deploy.
  6. Season-end card: temporarily mark E2..E10 watched via sqlite UPDATE, seek E1 to end… (or watch the real last file) → card offers "Start Season 2" → fires the real preflight/toast path → Sonarr shows S2 monitored+searching → cancel the S2 download afterward via the kiosk Queue (blocklist not needed) and un-monitor S2 via the API to restore state; revert the sqlite UPDATE.
  7. Movie resume: play any library movie 2 min, exit, replay → resumes; movie EOS (seek to end) → Library Unwatched hides it.
  8. Outage honesty: `docker stop mdb_sonarr` → Library still shows movies + warning line, series tiles absent, episode view shows outage message; `docker start mdb_sonarr` → recovers. (Do this only if nothing is downloading — GoT complete by then.)
  9. `verify_box.sh` 20/20 + `verify_services.sh` all checks incl. the new root-folder assertion.
- [ ] **Step 5:** Update CLAUDE.md (Media Browser section: playback flow + watch store paragraph; the auto-blocklist/missing-search bullets already updated). Commit — `docs: Phase 3 playback + watch store`
- [ ] **Step 6:** Ledger complete; hand to superpowers:finishing-a-development-branch.
