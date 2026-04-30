# Marquee Library filter overlay + Queue tab + input grammar redesign

**Status**: Design approved 2026-04-30. Ready for implementation plan.

## Problem statement

Three current gaps in the Marquee Media Browser:

1. **Library has no way to access its sort/filter dimensions.** `LibraryScreen` already renders `Recent / Title / Year / Size` sub-tabs visually, but the cycling input was never wired up. The operator can see the labels but can't switch between them.
2. **Queue is unreachable from top-level navigation.** `Screen::Queue` is only entered from `DetailScreen` ("View Queue" button). An operator who wants to see "what's downloading right now" has to open a movie's Detail page first — there's no peer-level destination for the queue.
3. **BTN2 is overloaded with screen-specific functions** that conflict with operator muscle memory. `BTN4 = back` works in the Marquee, but the operator wants `BTN4` for a richer purpose (slide-in overlay) and `BTN2` for back — matching the home-menu UI's input grammar where the menu button on the playlist UI opens a slide-in.

This design closes all three gaps with one cohesive change: a Queue tab in the Marquee strip, a slide-in overlay accessible via `BTN4` on Library, and a global Marquee input-grammar remap that puts back on `BTN2` and frees `BTN4` for the overlay.

## Solution overview

### 6-tab Marquee strip (reordered)

```
Popular · Top Rated · Search · Library · Queue · Settings
```

Reordering rationale:
- **Popular / Top Rated** (left) — discovery: "what's out there"
- **Search** — bridge: "find a specific thing out there"
- **Library / Queue** (middle) — owned content: "your stuff" (in library + downloading)
- **Settings** (right) — config

Queue gets a peer-level tab. The DetailScreen → Queue transition stays as a secondary entry point.

### Marquee input grammar (post-redesign)

The button vocabulary is unified across all Marquee menu screens (Browse / TopRated / Search / Library / Queue / Detail / Settings). Playback is special-cased — pause is sacred for media watching, so its grammar is preserved.

| Button | All Marquee menu screens | Playback |
|---|---|---|
| **BTN1** (yellow) | Tab PREV ← | Seek −10 s |
| **BTN2** (red) | **Back** (out of sub-screen → previous; out of overlay → close it) | Play-pause |
| **BTN3** (green) | Tab NEXT → | Seek +10 s |
| **BTN4** (black) | **Slide-in overlay** — Library only; no-op on every other menu screen | Exit playback → Detail |
| **BTN4 long-press** | Exit MB → kiosk MainMenu | Exit MB → kiosk MainMenu |
| **Rotary CW/CCW** | Cell / row nav | Seek scrub |
| **A / Rotary click** | Select / activate | (no-op) |

Cascading conflict resolutions (current `BTN2` functions retire — operators reach the same actions through other paths):

- **BrowseScreen quick-add** (currently BTN2) → retired. Operator opens Detail and clicks Add.
- **DetailScreen primary-button shortcut** (currently BTN2) → retired. A on the focused button is the same thing.
- **MbSettingsScreen refresh services** (currently BTN2) → retired. The Services row already re-pings on A.
- **SearchScreen quick-add** (currently BTN2) → retired. Same as Browse — go through Detail.
- **QueueScreen cancel** (currently BTN2) → retired. Detail → Confirm Remove handles cancel for in-library items; queue-only items can be cancelled via the Detail screen of the corresponding movie.

### Slide-in Library overlay

A 480 px panel that slides in from the right edge of the screen on `BTN4` press while `LibraryScreen` is active. Houses the library's stats, sort, and filter controls.

#### Geometry

- **Width**: 480 px (37.5% of 1280 px). 800 px of the underlying Library grid stays visible on the left.
- **Vertical extent**: full body area between chrome header bottom (y=120) and chrome footer-hint band (y=634). Sits flush with the wood frame's right inner edge (at x=1240) — i.e. the panel right edge is at x=1240, panel left edge is at x=760.
- **Background**: `bg_lift` (#2A232A) — same warm tone as MB pages.
- **Left edge**: 2 px gold rule (`accent`) — reads as a separate panel, matches active-tab vocabulary.
- **Top edge**: 1 px steel-blue dim rule for visual closure against the chrome header band.

#### Animation

- **Slide-in**: 200 ms ease-out from x=1280 (off-screen right) to x=760.
- **Slide-out**: 150 ms ease-in back to x=1280, then panel hides.
- Underlying Library grid keeps rendering during animation (panel slides over it).

#### Input behavior while open

- **Rotary CW/CCW**: walks rows within the panel.
- **A / rotary click**: activates the focused row (apply sort, apply filter). After activation, the chosen row gets the gold `▶` cue for ~150 ms before the slide-out animation begins, so the operator sees their selection register before the panel disappears.
- **BTN4**: toggles close (re-press).
- **BTN2**: also closes (matches "back" grammar).
- **BTN1 / BTN3**: no-ops while open (prevents a stray tap from tab-jumping the underlying grid out from under the panel).
- **Library grid rotary nav freezes** — input is fully captured by the panel until it closes.

#### Focus on open

Cursor lands on the currently-active sort row (`Recent` if untouched). One A immediately confirms and closes; one BTN2/BTN4 cancels and closes without applying.

### Panel contents

Three blocks, top to bottom:

#### Stats block (top, ~80 px tall)

- Gold ZenDots heading: `LIBRARY`
- Three lines below (cream `fg`, body font):
  - `<N> titles` *(formatted from `library_.size()`)*
  - `<X.X> <unit> used` *(sum of `Movie.file_size_bytes`, formatted via existing helper)*
  - `<Y.Y> <unit> free` *(`std::filesystem::space("/mnt/ssd/library").available`)*
- 1 px dim divider underneath separates the block from the controls.

#### Sort by section (middle, ~140 px tall)

Gold ZenDots section heading: `SORT BY`. Four rows, mutually exclusive (single-active radio):

| Row | Behavior |
|---|---|
| `Recent` | Default. Sort by `Movie.added` timestamp, newest first. |
| `Title` | A → Z, case-insensitive lexicographic on `Movie.title`. |
| `Year` | Newest → oldest by `Movie.year`; ties broken by Title. |
| `Size` | Largest → smallest by `Movie.file_size_bytes`. |

**Two visual states per row, independent**:
- **Active** *(persistent state — this is the currently-applied sort)*: gold label, no marker on the row itself; the active row's name shows in the panel header subtitle (`SORT BY · Recent`) so the operator can see at-a-glance what's currently in effect even when the cursor is somewhere else.
- **Focused** *(transient state — this is where the rotary cursor is right now)*: blinking gold `▶` left marker (same right-pointing arrow used on Settings rows), gold label.

A row that is BOTH active AND focused: gold marker + gold label (focus + active visually merge — that's the "you're hovering on the active choice" state).
A row that is active but not focused: gold label, no marker.
A row that is focused but not active: gold marker + gold label (briefly, while cursor is there).
A row that is neither: dim label, no marker.

A on a row applies the sort + closes panel.
Dim divider at the bottom.

#### Filter section (bottom, ~140 px tall)

Gold ZenDots section heading: `FILTER`. Four rows, mutually exclusive (single-active radio):

| Row | Behavior |
|---|---|
| `All` | Default. Show every movie in library. |
| `Unwatched` | **Placeholder for now** — wired to "always show" since the kiosk doesn't yet track watched history. Row renders dim with a small `(soon)` suffix. Real filter wires up when watched-history lands. |
| `Missing files` | `Movie.has_file == false`. |
| `Recently added` | Last 30 days by `Movie.added` timestamp. |

Same active/focused visual semantics as the Sort section: active filter shown in the panel header subtitle (`FILTER · All`); focused row gets the blinking gold `▶`.

A on a row applies the filter + closes panel.

#### Footer hint inside panel

Bottom edge of the panel, rendered with the same `chrome::draw_footer_hints` vocabulary as everywhere else but scoped to the panel's x-range:

```
BTN4 close   A select   Rotary nav
```

## Persistence

Two new fields in `AppState::DisplaySettings`:

```cpp
enum class MbLibrarySort {
    Recent = 0, Title = 1, Year = 2, Size = 3
};
enum class MbLibraryFilter {
    All = 0, Unwatched = 1, MissingFiles = 2, RecentlyAdded = 3
};

MbLibrarySort   mb_library_sort   = MbLibrarySort::Recent;
MbLibraryFilter mb_library_filter = MbLibraryFilter::All;
```

Persisted to `config/settings.json` as:
```json
{
  "display": {
    "mb_library_sort": "recent",
    "mb_library_filter": "all"
  }
}
```

String values (not ints) so the JSON stays human-readable. Default values match what the operator sees on a fresh kiosk. Falls under the existing `config/*` OTA exclude — survives upgrades cleanly.

## Architecture / file layout

This is a contained feature touching ~5 source files plus header changes:

| File | Change |
|---|---|
| `src/app/app_state.h` | Add `MbLibrarySort` / `MbLibraryFilter` enums + 2 fields in `DisplaySettings`. |
| `src/app/settings_persistence.cpp` | Save / load the two new fields with string ↔ enum conversion. |
| `src/media_browser/ui/library_screen.{h,cpp}` | Owns the slide-in overlay state machine + render. New `LibraryOverlayState` member (`Closed / SlidingIn / Open / SlidingOut`), `overlay_focus_` row index, animation timestamp. Render order: chrome header → grid → overlay (if visible). Input handling routes through the overlay first when open. |
| `src/media_browser/ui/browse_screen.{cpp,h}` | Add `Category::Queue` to `kVisibleTabs` between `Library` and `Settings`. Update strip-position math accordingly. |
| `src/media_browser/ui/queue_screen.cpp` | Update tabs list to render the 6-tab strip with Queue active. Wire BTN1 → Library / BTN3 → Settings. Remove BTN2 cancel handler (per grammar redesign). |
| `src/media_browser/ui/library_screen.cpp` | Wire BTN4 to open overlay. Apply current `mb_library_sort` + `mb_library_filter` to `view_` in `rebuild_view()`. Retire the visual sort sub-tab strip on the screen body (it lives in the overlay now). |
| `src/media_browser/ui/browse_screen.cpp` | Update tabs vector + nav. Retire BTN2 quick-add handler. |
| `src/media_browser/ui/search_screen.{h,cpp}` | Update tabs vector to the 6-tab order. Search sits at index 2 (between Top Rated and Library), so BTN1 → Browse (with last-active content category), BTN3 → Library. Retire BTN2 quick-add handler. |
| `src/media_browser/ui/detail_screen.cpp` | Retire BTN2 primary-button shortcut handler. |
| `src/media_browser/ui/mb_settings_screen.cpp` | Update tabs vector + nav. Retire BTN2 refresh-services shortcut. Add Queue tab to the strip. |
| `src/media_browser/ui/mb_chrome.{h,cpp}` | (Optional) Add a `draw_panel_left_edge()` helper if the gold-rule panel separator becomes a reusable primitive. |

**Tab nav routing on the new 6-tab strip** (`Popular · Top Rated · Search · Library · Queue · Settings`):

| From | BTN1 (←) | BTN3 (→) | BTN4 |
|---|---|---|---|
| Browse (Popular/TopRated) | Walk left in strip | Walk right; Search/Library/Queue/Settings each return their `Screen` value | (no-op) |
| Search | → Top Rated *(content tab — Browse with TopRated active)* | → Library | (no-op) |
| Library | → Search | → Queue | **Open overlay** |
| Queue | → Library | → Settings | (no-op) |
| Settings | → Queue | (dead-end — rightmost) | (no-op) |
| Detail | (no-op) | (no-op) | (no-op) |
| Playback | Seek −10 s | Seek +10 s | Exit playback → Detail |

## Out of scope

- **Watched history** — `Unwatched` filter is a placeholder until the kiosk tracks per-movie watched state. Not part of this design.
- **Queue bulk actions overlay** — Pause All / Resume All / Retry All Failed could live in a Queue-specific BTN4 overlay, but per the brainstorm we're keeping the overlay surface limited to Library only for now. Bulk actions stay accessible via MovieSettings.
- **Universal contextual menu** (option A from brainstorming) — explicitly rejected. BTN4 has only one purpose in the Marquee: open the Library overlay. Everywhere else it's a no-op (or the special Playback exit).
- **Search-history overlay** — interesting future direction (BTN4 on Search shows recent queries) but explicitly out of scope here.
- **Animation refinement** — the 200 ms / 150 ms timings are starting points; we may tune them once we see them on hardware.

## Testing notes

Manual smoke-test checklist on the Pi after implementation:

1. Walk through the 6 tabs left → right with BTN3, then right → left with BTN1. Confirm tab strip renders correctly with Queue tab present and active state cycling cleanly.
2. From Library, hit BTN4 — confirm panel slides in from the right over the grid.
3. Walk rotary through all 8 rows (4 sort + 4 filter). Confirm cursor lands on `Recent` initially, walks down through Sort then continues into Filter (skipping section headers).
4. A on `Title` — confirm panel closes, library re-sorts alphabetically. Re-open panel, confirm `Title` is now the active sort row.
5. A on `Missing files` — confirm panel closes, library shows only items where `has_file == false`.
6. Reboot the Pi — confirm sort + filter selections persist across the reboot (new keys are loaded from `config/settings.json`).
7. From Library, hit BTN2 — confirm it goes back to Browse (not closing the overlay if it's already closed).
8. From Detail (sub-screen of Library), hit BTN2 — confirm it goes back to Library, NOT to Browse.
9. From PlaybackScreen, confirm BTN2 still toggles play/pause (not back). Confirm BTN4 still exits to Detail.
10. From Browse / Search / Settings / Queue / Detail, confirm BTN4 is a no-op (no overlay, no crash).
11. From within the open overlay, hit BTN1 / BTN3 — confirm no-op (underlying grid does not change tabs).
