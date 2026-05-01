# Media Browser — Source Selection & Indexer Pool Redesign

**Date:** 2026-05-01
**Status:** Design approved, awaiting implementation plan

## Background & Motivation

The kiosk's Media Browser currently has three problems with how it finds and selects torrents for movie playback:

1. **The Settings → Sources panel is a lie.** Toggling indexer checkboxes on `mb_settings_screen.cpp` only flips an in-memory `enabled` flag and shows a banner saying "local-only in MVP — use Prowlarr web UI to persist." On app restart the toggle is lost. The widget also only shows the first five indexers from the JSON fixture (Demonoid, EZTV, Internet Archive, LimeTorrents, Magnetz), which excludes the actually-active TPB, YTS, and TorrentDownload entries.
2. **The active indexer pool is thin and weak in places.** Only LimeTorrents, The Pirate Bay, YTS, and TorrentDownload are enabled. The five "preconfigured but disabled" indexers in the fixture (Demonoid, EZTV, Internet Archive, Magnetz, Torrent Downloads) are poor fits for movies. Higher-yield public sources (1337x, TorrentGalaxy, Solid Torrents, Knaben, BitSearch, TheRARBG) aren't represented at all.
3. **There is no manual override for Radarr's auto-pick.** When the user presses "Add to Library," Radarr selects the highest-scoring release that passes its custom-format filters and hands it off to qBittorrent. If the chosen release is dead, slow, or the wrong cut, the user has no way to see other candidates or pick one. They sit watching a stalled download with no diagnostic and no escape.

The goal is to expand the source pool, surface real diagnostic data on the kiosk's settings panel, and add a manual release-picker UI so the user can override Radarr's auto-pick when it goes wrong — while keeping the "press Add and walk away" auto-flow as the default for the 90% case where it works.

## Goals

- Auto-flow remains the default. "Add to Library" still grabs Radarr's pick without user interaction.
- Provide a manual escape hatch. The user can open a release picker proactively (always-visible button on Detail screen) or reactively (toast prompt when a download stalls).
- Settings panel reflects reality. Toggling actually persists to Prowlarr; per-indexer health and last-search results are visible on-screen.
- Expand high-yield indexer coverage from 4 to 10 active sources.
- Tighten Radarr's auto-pick scoring without restructuring the existing custom-format pipeline.

## Non-Goals

- No changes to the playback pipeline (GStreamer, hardware decode, codec gating).
- No changes to the family-safe content filter (TMDB `include_adult`, parser-side `adult` check, Prowlarr title regex).
- No changes to the Confirm Remove flow or Radarr/qBit cleanup logic.
- No private-tracker support. All new indexers are public.
- No changes to the Prowlarr → Radarr Apps integration sync.

---

## Design

### 1. Indexer Pool Changes

**File:** `magic_dingus_box_cpp/scripts/data/prowlarr_indexers.json`

Add six new Cardigann indexer entries, all `enable: true`, priority 25, default categories 2000–2080 (Movies):

| Indexer | Cardigann definition | FlareSolverr tag |
|---|---|---|
| 1337x | `1337x` | `cloudflare` |
| TorrentGalaxy | `torrentgalaxy` | none |
| Solid Torrents | `solidtorrents` | none |
| BitSearch | `bitsearch` | none |
| Knaben | `knaben` | none |
| TheRARBG | `therarbg` | `cloudflare` |

No fixture changes for the disabled-by-default set. Demonoid Clone, EZTV, Internet Archive, Magnetz, and Torrent Downloads remain in the JSON with `enable: false` exactly as today. They appear in the new Sources panel (see §5) as a "disabled" group at the bottom — operators can flip them on at runtime via the panel toggle (which calls Prowlarr live), or by editing the fixture and re-running `setup_services.sh` for a permanent change that survives a service rebuild.

**Net active pool:** 4 → 10. LimeTorrents, The Pirate Bay, YTS, TorrentDownload remain unchanged; the six new indexers join them as enabled.

`scripts/setup_services.sh` Step 13 already handles indexer reconciliation by name and pushes JSON-fixture state to Prowlarr, so no script changes are needed for the pool expansion itself.

### 2. Scoring Tuning

**Files:** `magic_dingus_box_cpp/scripts/data/radarr_custom_formats.json`, `magic_dingus_box_cpp/scripts/setup_services.sh`

Two tuning changes, no structural changes:

- **`minimumSeeders`: 5 → 10.** Edit `setup_services.sh` Step 14b (the Python inline that PUTs `minimumSeeders` to every Radarr indexer). Films with <10 seeders typically take so long to start that the auto-flow's "press Add and walk away" UX breaks anyway; the manual picker is the escape hatch for genuinely rare titles.
- **Expand the trusted-group regex** in the "Trusted small-release groups" custom format. New regex:
  ```
  \b(yify|yts\.(mx|am|ag)|galaxyrg|rarbg|fgt|surge|piratess?|tgx|evo|aoc|ion10|qxr|successfulcrab)\b
  ```
  Score remains +30. The new groups (TGx, EVO, AOC, ION10, QxR, SuccessfulCrab) are recognized active high-quality x264 release groups that show up frequently in 1337x and TorrentGalaxy results.

No new custom format is needed for "high seeder bonus." Radarr already tiebreaks by seeder count within an equal-score tier, so when two releases have the same custom-format score, the more-seeded one wins automatically.

### 3. Release Picker Screen (new UI)

**New files:**
- `magic_dingus_box_cpp/src/media_browser/ui/release_picker_screen.h`
- `magic_dingus_box_cpp/src/media_browser/ui/release_picker_screen.cpp`

A new MbScreen mode that displays a sorted list of releases for a single movie and lets the user grab any one of them.

**Layout:** Six rows per screen, scrollable for additional releases. Each row shows:

```
[gold-border = Radarr's auto-pick]
Movie.Title.2024.1080p.WEB-DL.x264-GROUP   [200 ↑ / 12 ↓]   2.1 GB   x264 1080p WEB
Movie.Title.2024.720p.BluRay.x264-YIFY     [180 ↑ / 8 ↓]    900 MB   x264 720p BluRay
Movie.Title.2024.1080p.WEB.x265-RARE       [12 ↑ / 2 ↓]     1.5 GB   x265 1080p WEB    [dimmed red]
...
```

Columns: title (truncated to fit), seeders/leechers, size, codec badge, resolution badge, source badge.

**Sort order:** seeders descending, then Radarr custom-format score descending.

**Visual cues:**
- The release Radarr would have auto-picked has a gold border.
- Releases below `minFormatScore = -200` are dimmed red but still selectable (user can override the filter).

**Controls:**
- DPad up/down: navigate
- SELECT (A): grab the highlighted release → calls `RadarrClient::grab_release(guid)` → POST `/api/v3/release` with the release GUID
- BACK (B): cancel, return to caller (Detail screen or stall toast)

**Data source:** The picker is constructed with a list of `ReleaseCandidate` structs prepared by either Detail (which already has Prowlarr search results cached) or the watchdog (which re-runs the Prowlarr search). The picker itself does not initiate a Prowlarr search.

**Two entry points:**
1. **Detail screen "Pick a source" button.** Visible once Prowlarr's availability check completes. Opens the picker for the displayed movie. Available whether or not the movie has been added to the library — picking from the picker triggers the same `grab_release` flow as Add-to-Library would.
2. **Stall prompt** (see §4). When the watchdog detects a stalled download, the toast's "Pick" action opens the picker for the stalled movie.

### 4. Download Watchdog (new module)

**New files:**
- `magic_dingus_box_cpp/src/media_browser/qbittorrent/download_watchdog.h`
- `magic_dingus_box_cpp/src/media_browser/qbittorrent/download_watchdog.cpp`
- `magic_dingus_box_cpp/src/media_browser/qbittorrent/qbit_client.h` *(new if not already present)*
- `magic_dingus_box_cpp/src/media_browser/qbittorrent/qbit_client.cpp` *(new if not already present)*

Background module that watches active downloads and triggers a stall prompt when something goes wrong.

**Lifecycle:**
- When the user presses "Add to Library" or grabs a release from the picker, the watchdog records a `WatchedDownload { tmdb_id, movie_title, started_at }`.
- A background poller wakes every 10 seconds and queries:
  - qBit `/api/v2/torrents/info?category=radarr` for current download state.
  - Radarr `/api/v3/queue` for tracked-download status (failed / warning).
  - Radarr `/api/v3/history?eventType=grabbed,downloadFailed,downloadFolderImported` for blacklist/failure events.

**Stall conditions** (any one triggers the prompt):
- qBit progress is `0.0` and uploaded peers count is `0` for 60 seconds past `started_at` (no peer connections at all).
- Radarr queue entry shows `status == failed` or `trackedDownloadStatus in {warning, error}`.
- Radarr history shows a `downloadFailed` event for this movie.

**Prompt:** Non-blocking toast rendered by the existing MbChrome notification layer. Text: `[Movie name] download stalled. Pick a different source?` with two actions: `[Pick]` and `[Dismiss]`.
- `Pick` → re-runs Prowlarr search for that movie, opens the release picker.
- `Dismiss` → snoozes the watchdog for that movie for 10 minutes. Subsequent stall conditions during the snooze are ignored.

**Cleanup:** Watched downloads are removed from the watchdog when their corresponding qBit torrent reaches 100% or is deleted (e.g., by the Confirm Remove flow).

**`qbit_client`:** Thin wrapper around qBittorrent WebUI v2 API. Required endpoints:
- `POST /api/v2/auth/login` (cookie-based session)
- `GET /api/v2/torrents/info?category=radarr`
- `GET /api/v2/torrents/properties?hash=<hash>` (for upload/download peer counts)

If a qBit client wrapper already exists in the codebase, extend it; otherwise add the new files. The implementer should verify before writing.

### 5. Settings → Sources Panel Redesign

**Modified files:**
- `magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.h`
- `magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp`
- `magic_dingus_box_cpp/src/media_browser/prowlarr/prowlarr_client.h`
- `magic_dingus_box_cpp/src/media_browser/prowlarr/prowlarr_client.cpp`

Replace the existing checkbox-only Sources panel with a health-led row layout. One row per active indexer (sorted by last-search result count descending), with disabled indexers grouped at the bottom.

**Row content:**

```
● 1337x              47 results  ·  18 with ≥10 seeds   [last 1.8s]    ☑ enabled
● The Pirate Bay     32 results  ·  9 with ≥10 seeds    [last 0.9s]    ☑ enabled
● TorrentGalaxy      24 results  ·  11 with ≥10 seeds   [last 1.2s]    ☑ enabled
● YTS                 8 results  ·  8 with ≥10 seeds    [last 0.4s]    ☑ enabled
● Solid Torrents      0 results                          [TIMEOUT]      ☑ enabled
○ Demonoid Clone      —                                                 ☐ disabled
```

**Health dot color:** green if last search returned ≥1 result with no error, yellow if 0 results but no error, red if last search errored or timed out, gray if no search has been performed this session.

**Controls:**
- DPad: navigate rows
- SELECT (A): toggle enable/disable. Calls Prowlarr `PUT /api/v1/indexer/<id>` with the updated entity. UI updates optimistically; reverts and shows a banner if the PUT fails.
- INFO (Y or equivalent): expand row to show last error message (if any), full URL, category list, and last-success timestamp.

**Data source — `ProwlarrClient` extension:**
- Add a per-indexer stat record: `IndexerStats { name, last_search_at, last_response_ms, last_result_count, last_seeded_count, last_error }` (where `last_seeded_count` counts results with `seeders >= minimumSeeders`).
- `ProwlarrClient::search_async` already aggregates results across indexers; extend the worker to record stats per indexer from the response (Prowlarr returns each result with an `indexer` field) and store them in a member map.
- Expose `get_last_search_stats() -> std::vector<IndexerStats>` for the settings screen.
- If no search has been performed this session, the panel shows the indexer list with name + enabled state + gray dots.

**Persistence semantics:** Toggles call Prowlarr live and Prowlarr persists immediately. The kiosk's view is regenerated from `GET /api/v1/indexer` on panel open. The JSON fixture (`prowlarr_indexers.json`) remains the source of truth for fresh deploys but is not the live source — operators who want a runtime change to survive a service rebuild should also update the fixture.

### 6. Detail Screen Changes

**Modified files:**
- `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.h`
- `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp`

Add a new "Pick a source" button next to the existing "Add to Library" button. The button is disabled until Prowlarr's `search_async` for the displayed movie completes; once results are available, it becomes selectable and shows the result count (e.g., `Pick a source (12)`).

Pressing the button opens `ReleasePickerScreen` with the cached Prowlarr results converted to `ReleaseCandidate` structs. The picker calls back into Detail (or directly to `RadarrClient::grab_release`) when the user selects a release.

The "Add to Library" button retains its current behavior: triggers Radarr's auto-pick flow, no picker shown.

### 7. Component Boundaries & Data Flow

```
DetailScreen
   ├── Prowlarr search (existing, async) ─────────┐
   ├── Add to Library → RadarrClient::add_movie   │
   └── Pick a source → ReleasePickerScreen ◄──────┘ (uses cached Prowlarr results)
                          │
                          └── SELECT → RadarrClient::grab_release(guid)
                                          │
                                          └── DownloadWatchdog::watch(tmdb_id, title)

DownloadWatchdog (background thread)
   ├── poll qBit /torrents/info every 10s
   ├── poll Radarr /queue + /history every 10s
   └── on stall → MbChrome::toast("stalled — Pick?") → ReleasePickerScreen

MbSettingsScreen → Sources panel
   ├── reads ProwlarrClient::get_last_search_stats()
   ├── reads Prowlarr /indexer for live enable/disable state
   └── SELECT → Prowlarr PUT /indexer/<id>
```

### 8. Files Touched

**New (3 modules, 5–7 files):**
- `src/media_browser/ui/release_picker_screen.{h,cpp}`
- `src/media_browser/qbittorrent/download_watchdog.{h,cpp}`
- `src/media_browser/qbittorrent/qbit_client.{h,cpp}` *(if not already present)*

**Modified:**
- `src/media_browser/prowlarr/prowlarr_client.{h,cpp}` — per-indexer stat tracking, `get_last_search_stats()`
- `src/media_browser/radarr/radarr_client.{h,cpp}` — `grab_release(guid)`, `get_queue()`, `get_history()` methods
- `src/media_browser/ui/detail_screen.{h,cpp}` — "Pick a source" button + handler
- `src/media_browser/ui/mb_settings_screen.{h,cpp}` — health-led Sources panel
- `src/media_browser/ui/mb_screen.h` (or wherever mode routing lives) — register `ReleasePicker` mode
- `scripts/data/prowlarr_indexers.json` — six new indexer entries appended (`enable: true`); existing disabled entries unchanged
- `scripts/data/radarr_custom_formats.json` — expanded trusted-group regex
- `scripts/setup_services.sh` — `minimumSeeders` 5 → 10 in Step 14b

**Tests (new):**
- `tests/media_browser/test_release_picker.cpp` — sort order, gold-border highlight on auto-pick, dim-red on below-threshold
- `tests/media_browser/test_download_watchdog.cpp` — stall conditions (zero-progress, queue-failed, blacklist-event), snooze behavior, cleanup on completion
- `tests/media_browser/test_qbit_client.cpp` — auth, torrents/info parsing
- Extend `test_prowlarr_client.cpp` (or add it) — per-indexer stat capture from search response

## Error Handling

- **Prowlarr unreachable** → settings panel shows existing indexer list with gray dots + a banner "Prowlarr offline." Toggle attempts show "Save failed."
- **Radarr `grab_release` returns non-2xx** → release picker shows an inline error and stays open. User can retry or pick a different release.
- **qBit auth fails** → watchdog logs the error and falls back to Radarr-only stall detection (queue + history). Settings panel does not depend on qBit.
- **Indexer search timeout** → recorded as `last_error = "TIMEOUT"`, displayed on the settings panel. Does not block other indexers' results.
- **Stall prompt while picker is already open** → suppress the toast (don't stack picker prompts).

## Testing Strategy

- **Unit tests** for the release picker's sort logic, gold-border selection, dim-rejected logic — no UI rendering, just data transformations.
- **Unit tests** for the watchdog's stall conditions using mocked qBit and Radarr responses (fixtures: zero-progress mid-download, queue-failed entry, blacklist event).
- **Unit tests** for `ProwlarrClient::get_last_search_stats()` populating correctly from a multi-indexer search response fixture.
- **Integration check** (manual on Pi): with all 10 indexers enabled, run an availability check on a popular film and verify each indexer reports a result count on the settings panel.
- **Integration check** (manual on Pi): grab a deliberately-low-seeded release through the picker, verify download starts, watch the stall prompt appear after 60s if it doesn't.

## Open Questions for Implementation

- Does a qBittorrent client wrapper already exist in the codebase? If yes, extend it instead of adding `qbit_client.{h,cpp}` from scratch. The implementer should grep for `qbittorrent` or `qbit` before writing.
- Where exactly does the MbChrome toast layer live? The watchdog needs a reference to it — either passed in via constructor or accessed through a global/AppState pointer. Match whatever pattern already exists for other toasts.
- Are there existing notification/toast tests that the watchdog tests should mirror? Reuse the existing harness if so.
