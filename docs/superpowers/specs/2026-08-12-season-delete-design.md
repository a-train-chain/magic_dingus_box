# Per-Season Delete — Design

Date: 2026-08-12
Status: approved by Alex (kiosk UX + Approach 1)
Origin: Game of Thrones season 3 downloaded in a non-English language;
the kiosk offered only whole-series Remove, so a single bad season could
not be deleted, blocklisted, or replaced without destroying seasons 1–2.

## Decisions (made with Alex)

1. **Delete + blocklist, re-download manually.** Deleting a season
   removes its files, blocklists the release(s) that produced them, and
   leaves the season unmonitored. Re-download is the EXISTING
   "Start Season N" button — season delete never searches.
2. **UI lives inside the episode picker.** The season being deleted is
   whichever season's episode list is open — unambiguous, and the user
   is looking at the episodes they are about to remove.
3. **Approach 1: full orphan-proof season remove** — the season-scoped
   mirror of the whole-series Remove worker (Task 7), reusing the
   stall-reaper's blocklist semantics.

## UX (kiosk, SeriesDetailScreen Episodes region)

- Eligibility: the open season has `episode_file_count > 0` OR live
  queue rows. Otherwise the delete row does not exist.
- Mechanism: a trailing focusable ACTION ROW at the end of the episode
  list, labeled `Delete Season N…`. No new button needed — the
  Episodes region has no reliably free button (BTN1/BTN3 page when the
  list overflows, BTN2 = Exit, BTN4 = Seasons). The rotary reaches the
  row like any episode; it pages with the list; the RotaryPress footer
  hint reads `Select` (not `Play`) while it is focused.
- First SELECT on the row ARMS it: label becomes
  `Confirm delete Season N`. Mirrors the Remove/ConfirmRemove idiom:
  arming does not move focus, timed disarm (same duration as
  `remove_pending_at_`), disarm when focus leaves the row.
- Second SELECT STARTS the worker. Picker shows a "Removing season…"
  status line; the delete row goes inert while in flight (BTN4/back
  remains allowed — the worker is background and gen-checked, leaving
  the screen does not corrupt it).
- Completion: return region to Seasons, `fetch()` refresh, toast
  `Season N removed — Start Season N re-downloads it.`
- Failure: stage-named toast (e.g. `Season delete aborted — Sonarr
  history unavailable`); nothing destructive has run before the abort
  point; pressing delete again retries safely.
- Mutual exclusion: arming season-delete is blocked while whole-series
  Remove is pending/in-flight, and vice versa.

## Worker stages (destructive steps LAST)

Season-scoped background worker, same publish pattern as the Task 7
remove worker (generation-checked, render thread applies results).

| # | Stage | On failure |
|---|-------|-----------|
| a | Unmonitor the season AND its episodes: `set_season_monitored(id, N, false)` + bulk `set_episodes_monitored(season's episode ids, false)`. PROBED 2026-08-13: `season.monitored` and `episode.monitored` are INDEPENDENT, and Sonarr's auto-redownload-on-failed keys off the EPISODE flag — season-only unmonitoring lets stage (d) fire searches behind our back. Corollary: the Start-Season-N flow must explicitly re-monitor the season's episodes before its SeasonSearch (idempotent; removes all dependence on cascade semantics) | abort (nothing changed) |
| b | `get_season_history_checked(id, N)` — grab records, import records, download hashes (lowercased) | nullopt → abort |
| c | Cancel this season's queue rows with `blocklist=true, removeFromClient=true` (stall-reaper semantics) | abort |
| d | `mark_history_failed(history_id)` for the season's imported grab(s) — Sonarr's supported blocklist for an already-imported release | abort (blocklist incomplete = the bad release could return; retry is safe) |
| e | qBit: delete the season's torrents by hash, with data. Skipped when `qbit_` is null (Task 7 contract). Library files of OTHER seasons survive deletion of a multi-season pack (imports are separate copies/hardlinks); such a pack merely stops seeding | warn-and-continue (torrent may already be gone) |
| f | `get_episode_files_checked(id)` → filter `season_number == N` → `delete_episode_files(bulk ids)` — the destructive step, last | abort with stage toast |
| g | Publish → refresh rows, toast | — |

Idempotency: every stage tolerates a rerun after partial completion
(unmonitor is a no-op, queue rows already gone, mark-failed on an
already-failed record is tolerated, missing qBit hashes skip, the
episode-file list is re-fetched and re-filtered).

## New SonarrClient surface

All `virtual`, mirrored in `SonarrMockClient`, checked shapes
(`nullopt` = transport/HTTP failure; engaged-but-empty = authoritative
"none") per the existing `get_series_download_hashes_checked` doctrine:

- `get_season_history_checked(int sonarr_id, int season)` →
  `optional<SeasonHistory>` where SeasonHistory carries
  `imported_history_ids`, `grabbed_history_ids`, `download_hashes`.
  Endpoint: `GET /api/v3/history/series?seriesId=X&seasonNumber=N`.
  PROBED 2026-08-13: the server-side seasonNumber filter WORKS and is
  REQUIRED — history records carry no per-record season field, so
  client-side filtering is impossible. The parser takes the response
  as already season-scoped. (Probe also confirmed: `grabbed` records
  embed a live Prowlarr API key in `data.downloadUrl` — fixtures must
  redact it, and no code may log raw history JSON.)
- `set_episodes_monitored(const std::vector<int>& ids, bool monitored)` →
  bool. Endpoint: `PUT /api/v3/episode/monitor`. Used by stage (a) and
  by the Start-Season-N re-monitor.
- `mark_history_failed(int history_id)` → bool.
  Endpoint: `POST /api/v3/history/failed/{id}`.
- `get_episode_files_checked(int sonarr_id)` →
  `optional<vector<EpisodeFileInfo{id, season_number}>>`.
  Endpoint: `GET /api/v3/episodefile?seriesId=X`.
- `delete_episode_files(const vector<int>& ids)` → bool.
  Endpoint: `DELETE /api/v3/episodefile/bulk`.
- Queue cancel: reuse/extend the existing cancel so the season path can
  pass `blocklist=true&removeFromClient=true` (whatever the current
  `cancel_queue_item` sends, the season path must get stall-reaper
  semantics).

**Probe before build (hard prerequisite):** the TV phases repeatedly
proved Sonarr API assumptions wrong. Before any implementation, probe
on magicpi5's live Sonarr: (1) `/history/series?seriesId&seasonNumber`
exists and filters as expected; (2) `POST /history/failed/{id}` shape
and its blocklist side effect; (3) whether mark-failed triggers a
search when the season is unmonitored (expected: no). Capture the real
JSON as fixtures for the mock tests.

## Untouched on purpose

- Watch history/positions (next-up is file-evidence-based, so deleted
  episodes drop out of Continue naturally; positions survive a future
  re-download).
- No web Content Manager UI, no auto re-search, no per-episode delete.
- Language custom-format scoring — separate follow-up: a wrong-language
  release grabbing at all means the CF layer under-penalized it.

## Testing

- **Pure logic (Mac):** hint eligibility (files/queue → hint; neither →
  none), arm/disarm/timeout, mutual exclusion with whole-series Remove
  — table tests beside the existing series_detail_logic tests.
- **Mock client (Mac):** the four new calls against captured fixtures;
  worker stage-ordering tests asserting no destructive call precedes an
  abort (mock records call order); idempotent-retry test.
- **Hardware acceptance:** on magicpi5, against a DISPOSABLE test
  series (add one, grab one episode) — the original GoT case was
  resolved manually on 2026-08-13 (wrong-language pack blocklisted, a
  "Non-English release markers" CF added at -10000, English replacement
  re-grabbed), so GoT's season 3 is now good data and must NOT be the
  test subject. Delete the test season via the UI; verify blocklist
  entry, files gone, season+episodes unmonitored; press Start Season N
  and verify episodes re-monitor and the search fires (skipping the
  blocklisted release); then whole-series-remove the test series.
