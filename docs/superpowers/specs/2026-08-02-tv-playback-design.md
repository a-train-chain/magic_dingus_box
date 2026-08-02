# Phase 3 — TV Playback Design

**Goal:** Downloaded TV becomes watchable from the couch: series appear in the Library, a series opens to an episode picker with watch state, episodes play with resume, the next episode auto-plays with a countdown, and finishing a season offers to start the next one. Movies gain the same resume/watched tracking.

**Approach (approved):** Live Sonarr + minimal watch store. Episode lists and file paths come from Sonarr at screen-open (it is on-box; milliseconds away). The dormant `LibraryDb` SQLite store wakes up to hold only what Sonarr cannot know: watch positions and watched flags. No catalog mirroring, no sync pipeline.

**Product decisions (approved by Alex, 2026-08-02):**
1. **Mixed Library** — TV series appear as posters alongside movies in the Library tab; selecting one opens the existing series detail screen.
2. **Auto-play + countdown** — episode end starts the next downloaded episode after an ~8s on-screen countdown with a "red = stop" hint.
3. **Season-end offer** — finishing the last downloaded episode of a season shows a card offering to start the next season's download via the existing Start-Season-N flow.
4. **Movies too** — the same store records movie position/watched; Play resumes; the Library "Unwatched" chip becomes real for everything.

---

## Architecture overview

```
Sonarr (live, on-box)          LibraryDb (SQLite, new v3 migration)
  /episode?seriesId&               watch_state(kind, tmdb_id, season,
  includeEpisodeFile=true           episode, position_s, duration_s,
        │                           watched, updated_at)
        ▼                                   ▲ render/main thread only
SeriesDetailScreen ──────────► episode rows + watch overlay + next-up
        │  SELECT episode                   │
        ▼                                   │ checkpoints (30s), EOS,
PlaybackScreen (resume via existing         │ exit
  load_file start param) ───────────────────┘
        │ EOS
        ▼
next_up() → countdown → in-place next episode
        └ no next file → season-end card → SeriesDetail intent
```

Everything decision-shaped is pure and Mac-tested (`episode_logic.h`, extensions to `series_detail_logic.h`, `library_view.h`); screens and the store stay thin.

## Components

### 1. SonarrClient episode endpoint (`src/media_browser/sonarr/`)

New method pair following the checked-variant house pattern:

- `std::optional<std::vector<EpisodeInfo>> get_episodes_checked(int sonarr_id)` — GET `/api/v3/episode?seriesId=N&includeEpisodeFile=true`. `nullopt` = Sonarr didn't answer; engaged-empty = genuinely no episodes.
- `EpisodeInfo` (new, `sonarr_types.h`; the existing minimal `Episode` stays for queue embeds): `id, season_number, episode_number, title, runtime_minutes, air_date, has_file, episode_file_id, monitored, file_container_path, file_size_bytes, file_quality`.

Live-verified parsing contract (captured 2026-08-02 against Sonarr 4.0.19, series 6):
- `hasFile=false` records carry **no `episodeFile` key at all** (not null) and `episodeFileId` is **`0`** (not absent). Parser must treat missing key + id 0 as "no file".
- `runtime` is populated per-episode on regular seasons; zero on most specials → fall back to `series.runtime`. Season 0 (specials) is **excluded everywhere**, matching `merge_season_rows`.
- `episodeFile.path` is container-absolute (`/data/library/tv/...`); `SonarrClient::resolve_host_path` already maps it to `/mnt/ssd/library/tv/...`.
- The embedded `episodeFile` shape must be re-verified against a real import during implementation (zero files existed at design time); a fixture from the first real import becomes the parser test.

`SonarrMockClient` overrides the new method in the same change (mock-honesty rule): returns `nullopt` so keyless boxes cannot fabricate episodes.

### 2. WatchStore (`src/media_browser/library/watch_store.{h,cpp}`, new)

Thin wrapper over the existing `LibraryDb` (WAL already on) owning prepared statements. **Main/render thread only** — LibraryDb is documented non-thread-safe, and every caller below already lives on the main thread. Workers never touch it; they publish, and the drain joins watch data.

**Migration v3** (`library_db.cpp` MIGRATIONS array, same mechanism):
```sql
CREATE TABLE watch_state(
  id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL CHECK(kind IN ('movie','tv')),
  tmdb_id INTEGER NOT NULL,
  season INTEGER NOT NULL DEFAULT 0,     -- movies: 0
  episode INTEGER NOT NULL DEFAULT 0,    -- movies: 0
  position_s REAL NOT NULL DEFAULT 0,
  duration_s REAL NOT NULL DEFAULT 0,
  watched INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL,
  UNIQUE(kind, tmdb_id, season, episode)
);
CREATE INDEX idx_watch_lookup ON watch_state(kind, tmdb_id);
```
Keyed by `(kind, tmdb_id)` because TMDB movie/TV id spaces overlap completely — the MediaRef doctrine applied to storage.

**API:** `open()` (db path: new `config::get_media_db_file()` → data dir + `/media_browser.db`, matching the test CLI's default; deploy rsync runs with `--delete` and only per-file `data/` excludes, so the plan adds an explicit `--exclude 'data/media_browser.db*'` to `deploy_cpp.sh`'s rsync — the glob covers the `-wal`/`-shm` sidecars — following the `text_input_queue.jsonl` precedent, so the DB survives deploys), `upsert_position(ref, season, ep, pos, dur)`, `mark_watched(ref, season, ep)`, `series_watch(tmdb_id) → map<(season,ep), WatchRow>`, `movie_watch(tmdb_id) → optional<WatchRow>`, `watched_movie_ids() / tv_watch_counts()` for the Library filter. All best-effort: an open/exec failure logs once and degrades to "no watch data" — playback never blocks on the store.

**Thresholds (pure, in `episode_logic.h`):** watched when `position/duration ≥ 0.92` (applied at checkpoint/exit/EOS); resumable when `position ≥ 60s` and below the watched threshold. A watched row keeps its position but resume ignores it (plays from start).

**Resume UX (deliberate v1 choice):** Play/SELECT always auto-resumes when a resumable position exists — least presses for family viewing. A toast on resumed start ("Resuming from 23:14") makes it visible, and starting over is one rotary seek-to-start away in the player. No resume-vs-restart modal in v1; revisit only if it confuses in practice.

### 3. Playback layer (`playback_screen.{h,cpp}` + main.cpp dispatcher)

- **Resume:** new setter `set_start_position(double s)`; `enter()` passes it to `load_file_with_resolution(...)` whose start parameter exists today and is hardcoded `0.0`. One-shot; cleared on `leave()`.
- **Identity:** new setter `set_watch_identity(WatchIdentity{MediaRef ref, int season, int episode, double duration_hint})`. The dispatcher sets it during the handoff (movie: season/episode 0).
- **Checkpoints:** main.cpp, next to the existing 200ms status tick (which already reads `state.get_position()`): every 30s while Playback is active, `watch_store.upsert_position(...)`; crossing the watched threshold sets the flag. Also written on playback exit. Single-row WAL upserts — negligible SD wear.
- **Return origin:** `PlaybackScreen` currently hardcodes `return Screen::Detail`. It gains `set_origin(Screen)` (set by the dispatcher, same pattern as SeriesDetail): BTN4/exit returns to Detail for movies, SeriesDetail for episodes.
- **Episode EOS → Next-Up countdown:** on the existing EOS edge, when identity is an episode: mark watched, then instead of `exit_pending_`, enter a `NextUp` overlay state — "Next: S1E5 'The Wolf and the Lion' — starting in 8…" with hints SELECT = now, RED = stop. On expiry/SELECT: **in-place reload** (`controller_.stop()`, `load_file` with the next episode's path, reset watch identity + `eos_suppress_frames_`) — no screen transition, no flicker. RED: cancel → return to SeriesDetail. Movie EOS is unchanged (mark watched, exit to Detail).
- **Next-episode resolution is pure** (`episode_logic.h`): `next_up(episodes, watch_rows) → optional<EpisodeInfo>`, with two explicit forms. **After finishing an episode, auto-advance is strictly sequential:** the next episode after the current one in airing order that has a file, crossing season boundaries when files exist, **regardless of its watched flag** — a mid-binge rewatch never skips ahead over previously-watched episodes. The watched-skipping form — first unwatched-with-file — applies only to the series screen's PlayNextUp, where there is no current episode. The countdown needs the episode list: SeriesDetail passes the loaded episode vector through the dispatcher handoff (`PlayTarget` analog), so playback never fetches.
- **Season-end card:** when EOS finds no `next_up` with a file: card replaces the countdown — decided purely by `season_end_card(rows, episodes, current) → {Play|StartNextSeason|Downloading|Done}`:
  - next season not monitored → "Season N finished — Start Season N+1?" [Start] [Done]. **Start** returns to SeriesDetail with a pending intent (`set_pending_intent(StartNextSeason)`); SeriesDetail's `enter()` consumes it and runs the **existing** `dispatch_action(NextSeason)` path — disk preflight, toasts, gen-guards all reused, zero Sonarr mutation code in playback.
  - next season downloading → informational "Season N+1 is downloading" [Done].
  - series finished (`finaleType:"series"` reached / no more seasons) → "That's the series!" [Done].

### 4. SeriesDetailScreen episode picker (`series_detail_screen.{h,cpp}`)

- **Episode view:** SELECT on a season row with `episode_file_count > 0` switches the season-list region to that season's episode list (BTN4 = back to seasons; existing paging idiom, per_page guards). Episodes fetch via `get_episodes_checked` on first need using the existing FetchWorker + PendingLoad pattern (gen-checked under `pending_mtx_` — the b70710d invariant). Watch overlay joins on the render thread from WatchStore.
- **Episode rows:** `E4 · You Win or You Die · 57m` + state glyph: ✓ watched, `▶ 23:14` in progress, `·` unwatched, dimmed when no file (with "downloading" when its season is). SELECT on a fileless row: toast, no-op.
- **Next-up button:** when InLibrary with any files, `decide_action_row` gains a leading `Action::PlayNextUp` — label "Continue S1E4" (or "Start watching S1E1"). One press from series screen to playing. Focus rules follow the existing action-identity preservation; forced-focus chain rebuilds get table tests (the `prev_row_remove_only` lesson).
- **Handoff:** SeriesDetail exposes `get_play_target()` (host path via `sonarr_.resolve_host_path`, display title "Game of Thrones — S1E4 · You Win or You Die", overlay meta from TMDB detail, watch identity, resume position from WatchStore, plus the episode vector **and season rows** — everything next-up and the season-end card decide from, so playback never fetches). The dispatcher gains the `SeriesDetail → Playback` branch mirroring the existing `Detail → Playback` one.

### 5. Mixed Library (`library_screen.{h,cpp}`, `library_view.{h,cpp}`)

- **Entries:** new `LibraryEntry { MediaRef ref; title; year; poster_url; added_at; file_count; total_count; downloading; watched_state }` built from Radarr `Movie` or Sonarr `Series` (which carries poster_url, added_at, statistics). `run_refresh()` additionally calls `sonarr_->get_library_checked()`, null-gated on a `SonarrClient*` ctor param that main.cpp passes only when a key exists (QueueScreen precedent — the mock-phantom guard). Sonarr unreachable ⇒ TV entries simply absent that cycle plus the existing outage-warning idiom; movies unaffected.
- **Inclusion rule:** a series appears when `episodeFileCount > 0` **or** it has an active download (badge, matching movie behavior). Never-downloaded monitored series stay Browse-only.
- **Selection:** kind-aware — SELECT returns `Screen::Detail` or `Screen::SeriesDetail` by `ref.kind` (Browse pattern); the dispatcher's SeriesDetail-producer guard extends from `Browse` to `Browse|Library`. Id sets re-key from bare int to `MediaRef` (the 2c-1 conversion, applied here).
- **Unwatched becomes real:** `library_view` filters on watch data — movies: `!watched`; TV: `watched_episode_count < episodeFileCount`. Watch snapshot is read from WatchStore on the render thread at drain time and passed into `build_library_view` (workers never touch the store). TV posters get a small type marker consistent with whatever Queue-row unification lands in 2c-3.

### 6. Ops hardening (in scope — the import path must actually work)

- `/mnt/ssd/library/tv` has now been destroyed twice by an unidentified cleanup (recreated 2026-08-02 with a `.mdb-keep` file inside; Radarr's `deleteEmptyFolders` was already false — culprit unknown). The plan adds: (a) a `verify_services.sh` assertion that Sonarr's root folder reports `accessible: true` — this check would have caught both incidents within a day; (b) an idempotent `install -d` of the TV root (+ keep-file) in the boot path (`magic-dingus-services` ExecStartPre or storage-attach), so the folder can never be missing when an import fires.
- The first implementation task verifies a real import end-to-end (the GoT download completes ~today) and captures the genuine `episodeFile` JSON as the parser fixture.

## Error handling

- **Sonarr down at episode-view open:** FetchWorker publishes failure → episode region shows the existing outage message idiom; season rows (TMDB-based) still render. Never fabricate: keyless boxes get `nullopt` from the mock.
- **File missing on disk at play** (deleted behind Sonarr's back): same guard as movies — `std::filesystem::exists` before transition, toast "File missing on disk".
- **WatchStore failure:** log once, run without watch data (no resume, filter falls back to keep-everything). Playback is never blocked by SQLite.
- **Countdown with stale episode list** (file imported mid-watch): next_up uses the list loaded at handoff; a just-imported episode is missed until the next visit — acceptable, self-corrects.
- **Mid-download watching:** episodes import one-by-one; picker shows per-episode file state and refreshes via the existing 9s poll gen-guarded repoll.

## Invariants & constraints

- All new code inside `ENABLE_MEDIA_BROWSER`; the OFF binary stays bit-identical (settings writer untouched on the OFF path).
- Dual-board: no new decode paths (same playbin pipeline); runtime branching only; sqlite3 links into the TEST targets today — the plan's Task 3 wires `library_db.cpp`/`watch_store.cpp` into `KIOSK_MEDIA_BROWSER_SOURCES` and adds the kiosk's sqlite3 link (`MEDIA_BROWSER_SOURCES` feeds only the test binaries).
- Every worker publish gen-checks under its mutex; WatchStore is main-thread-only by construction.
- Season 0 (specials) excluded everywhere, matching 2c-2.
- No changes to Radarr/Sonarr service config; playback contention guard (torrent pause) applies to episodes exactly as movies — it keys on playback start/end, not media kind.

## Testing

- **Pure logic (Mac, table tests):** `next_up` (gaps, season crossings, all-watched, no-files), season-end card decision matrix, watched/resume thresholds, `LibraryEntry` merge + Unwatched filtering for both kinds, action-row with PlayNextUp (including forced-focus chains), episode JSON parsing (fixtures: live captures incl. the hasFile=false/no-key/id-0 facts + the real import fixture).
- **WatchStore:** temp-DB tests in the `test_library_db.cpp` style — migration v3 on fresh and v2 DBs, upsert/threshold semantics, degraded-mode (unopenable path).
- **Mock honesty:** new SonarrClient virtuals overridden; keyless-box behavior pinned.
- **Hardware acceptance:** full loop on magicpi5 — Library shows GoT → episode picker with real files → play → resume after exit → EOS countdown → auto-advance → cancel → season-end card → Start-Season-2 fires the real preflight. Movie resume verified on an existing library title. Pi ON+OFF builds.

## Out of scope

- "Season ready to watch" completion notification (recommended separately; small follow-up once import events are observable).
- TV search / `search_tv`, per-episode download management, specials (S0) playback, multi-user profiles, Trakt/Letterboxd, watched badges in Browse charts, Continue-Watching rows in Browse/For You.
- Queue-screen TV polish (posters/labels) — stays in 2c-3.
