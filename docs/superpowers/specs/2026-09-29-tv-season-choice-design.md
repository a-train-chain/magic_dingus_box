# TV: keep emptied shows in Library, choose the season to download

**Date:** 2026-09-29
**Status:** Approved design (owner, in chat)
**Boards:** Pi 5 and Pi 4B — pure UI/Sonarr-API change, no board branch.

## Problem

Observed with Game of Thrones on the Pi 5 bench box (2026-08-22):

1. The owner deleted seasons one by one (per-season delete) to make room.
   After the last one, the show **disappeared from Library**. It had not
   been removed — Sonarr still held series 7, monitored, 9 seasons all
   unmonitored, 0 files — but `LibraryView` only lists a series with
   `episode_file_count > 0` or an active download
   (`src/media_browser/ui/library_view.cpp:60`). Movies without a file,
   by contrast, stay listed. The per-season delete looked like a
   whole-series delete.
2. Re-finding the show and pressing the primary button started **Season 1**:
   `decide_action_row`'s "Download Season N" always targets
   `next_unmonitored_season(rows)`, the LOWEST unmonitored season, and every
   season was unmonitored. The owner had watched through Season 4. Picking
   a specific season was possible (SELECT on a season row), but nothing on
   screen said so.

## Goals

- A show stays in Library while Sonarr has it, whatever is on disk.
  Only **Remove** takes it out.
- The primary download action proposes the season the viewer most likely
  wants next, and lets them change it before anything starts.
- Same for a show not yet in the library ("Add Season N").

Non-goals: a "free up space / delete watched seasons" helper (separate
feature); any change to per-season delete, whole-series add, Remove, or
the season-row SELECT shortcut.

## Design

### 1. Library inclusion

`LibraryView` includes every Sonarr series the Library receives (the same
set movies already get: everything the *arr holds). A series with no
episode file and no active download renders as **empty**:

- poster dimmed (same treatment as a movie `MissingFile` tile),
- a "Nothing downloaded" tag in the tile's status slot,
- normal sort position — no reordering.

Counts and watched math are unchanged (season-0-excluded sums; the
Unwatched filter keeps using `tv_watched_counts`). An empty series with
watch history is "partially watched" exactly as it would be with files.

### 2. Suggested season (pure logic)

New pure functions in `series_detail_logic.h`:

```cpp
// Seasons a download can target: not on disk (state None — no files,
// not downloading), season_number >= 1. Ascending.
std::vector<int> eligible_seasons(const std::vector<SeasonRow>& rows);

// The season the primary button proposes, or nullopt when nothing is
// eligible (the button hides, as today when all are monitored).
//   frontier = max(highest season with ANY watched episode or any
//                  resume position in `watch`,
//                  highest season with episode_file_count > 0 or
//                  state Downloading)
//   -> the first eligible season > frontier;
//   -> else the lowest eligible season (viewer is re-watching / filling
//      a gap).
std::optional<int> suggested_season(const std::vector<SeasonRow>& rows,
                                    const watch_map& watch);
```

Eligibility is by what is ON DISK / IN FLIGHT, not by the monitored flag:
a monitored season whose search found nothing is still a legitimate
target (re-issuing its download re-runs the search). This replaces
`next_unmonitored_season` as the primary-button source; that function
stays for any other caller.

Worked examples (unit-tested):

| Case | Rows | Watch | Suggest |
|---|---|---|---|
| GoT after deletes | S1-S8 all None | watched through S4E10 | 5 |
| New show, no history | S1-S3 None | — | 1 |
| Mid-download | S1 Complete, S2 Downloading, S3-S5 None | S1 watched | 3 |
| Gap | S1 None, S2 Complete, S3 Complete | S2-S3 watched | 1 (nothing after S3 eligible → lowest) |
| All on disk | all Complete | — | nullopt |
| Specials | S0 present | S0 watched | S0 never counts, never suggested |

### 3. The season chooser (primary button)

`decide_action_row` labels the primary button from `suggested_season`:
"Download Season N" (in library) / "Add Season N" (not in library),
N defaulting to the suggestion. The button gains a two-step flow,
mirroring the existing arm/confirm idiom ("Whole series… → Confirm"):

1. **Idle** — "Download Season 5". SELECT → **Choosing**.
2. **Choosing** — label `‹ Season 5 · ~22 GB ›`. Rotate / D-pad
   Left-Right steps through `eligible_seasons` (clamped at the ends, no
   wrap). The size is `estimate_remaining_bytes` for that one season,
   with the same "(est)" honesty rule as the whole-series label when the
   runtime is assumed (called with a one-row vector holding that season). SELECT → start the download for the chosen
   season, return to Idle. BTN4 (back), or any focus move off the
   button, cancels to Idle without side effects.

While Choosing, rotate events adjust the season instead of moving focus
(the one navigation chain is otherwise unchanged). Focus identity rules
in `decide_action_row` are unchanged; the chooser state lives beside
`whole_armed_`/`remove_pending_` and is cleared by the same
"any navigation cancels pending confirms" rule — except the rotate that
Choosing itself consumes.

Chooser state is a small pure struct with a transition function
(`SeasonChooser{state, candidates, index}` + `on_select/on_rotate/on_cancel`)
so its behaviour is Mac-testable without the screen.

### 4. Starting the chosen season

- **In library:** `start_season_download(N)` (existing — monitors the
  season, re-monitors its episodes, searches). No change.
- **Not in library, N == 1:** existing `AddSeason1` path unchanged
  (`add_series(monitor=true)` → Sonarr's own `firstSeason` + search).
- **Not in library, N > 1:** `add_series(monitor=false)` (addOptions
  `monitor="none"`, no search; its settle predicate already exists),
  then, once settled, `start_season_download(N)` on the new record. If
  the add does not settle within its budget, toast
  "added — choose the season again in a moment" and leave the record
  (the page's existing settle poll then offers the controls). Never
  fall back to monitoring Season 1.

### 5. Where the code goes

| Unit | Change |
|---|---|
| `ui/library_view.cpp/.h` | inclusion rule only — "empty" is derived from the existing `file_count == 0 && !downloading` |
| `ui/library_screen.cpp` | render empty TV tile (dim + tag) |
| `ui/series_detail_logic.h` | `eligible_seasons`, `suggested_season`, `SeasonChooser`, action-row label source |
| `ui/series_detail_screen.cpp/.h` | wire chooser input + render; not-in-library N>1 add path |
| tests: `test_library_view.cpp`, `test_series_detail_logic.cpp` | table tests above + chooser transitions |

## Error handling

- Sonarr unreachable while choosing: the choice itself needs no network;
  the start path's existing toasts apply.
- Candidate list changes under the chooser (a poll flips a season to
  Downloading): the chooser re-validates its index against fresh
  `eligible_seasons` on each rebuild; if the chosen season is no longer
  eligible it snaps to the nearest eligible one, or cancels to Idle when
  none remain.

## Testing

- Mac unit tests for every table row and chooser transition (TDD).
- Container build (`pisim.sh check`) — the screen compiles only in the
  kiosk build.
- Hardware, Pi 5 bench box (real GoT record): Library shows GoT dimmed
  "Nothing downloaded"; its page proposes "Download Season 5"; choose
  Season 6 → Sonarr shows S6 monitored + a SeasonSearch command
  (verified over SSH via the Sonarr API); cancel path leaves Sonarr
  untouched.
