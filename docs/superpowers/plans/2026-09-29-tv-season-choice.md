# TV Season Choice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep TV shows in the Library after all their seasons are deleted, and let the viewer choose which season to download (smart default + adjustable), both for shows in the library and new ones.

**Architecture:** All decisions are pure, header-only logic under Mac unit tests (`library_view`, new `season_choice.h`, `series_detail_logic.h`'s `decide_action_row`). The two screens (`library_screen.cpp`, `series_detail_screen.cpp`) only marshal state in and draw/dispatch the answer, following the existing pattern. Downloads reuse the existing `start_season_download` (monitor season + re-monitor episodes + search) and `SonarrClient::add_series`.

**Tech Stack:** C++17, Catch2 v3 (Mac `build-mb`), Sonarr v3 API, kiosk build via `magic_dingus_box_cpp/dev/pisim/pisim.sh` (arm64 Trixie container).

**Spec:** `docs/superpowers/specs/2026-09-29-tv-season-choice-design.md`

## Global Constraints

- Both boards (Pi 5, Pi 4B) — no board branch; no new dependencies.
- Render thread never blocks: no network call outside `spawn_mutation` workers (systemd `WatchdogSec=10`).
- Season 0 (specials) is never counted, suggested or chosen.
- Never fall back to monitoring Season 1 when the user chose another season.
- Mac test build: `cmake -S magic_dingus_box_cpp -B magic_dingus_box_cpp/build-mb -DENABLE_MEDIA_BROWSER=ON && cmake --build magic_dingus_box_cpp/build-mb -j8 --target test_media_browser_unit && ./magic_dingus_box_cpp/build-mb/test_media_browser_unit "[tag]"` (run from repo root; the repo dir name has a trailing space — quote it).
- Screen files compile only in the kiosk build: `magic_dingus_box_cpp/dev/pisim/pisim.sh build` (Docker Desktop running).
- New MB test files must be added to `MEDIA_BROWSER_TEST_SOURCES` in `magic_dingus_box_cpp/CMakeLists.txt` (explicit list, not a glob).
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

All paths below are relative to `magic_dingus_box_cpp/`.

---

### Task 1: Library keeps emptied shows

**Files:**
- Modify: `src/media_browser/ui/library_view.cpp:52-60`
- Modify: `src/media_browser/ui/library_view.h` (inclusion-rule comment near line 86)
- Modify: `src/media_browser/ui/library_screen.cpp` (tile render, after the TV chip block ~line 880)
- Test: `tests/media_browser/test_library_view.cpp:222-237`

**Interfaces:**
- Consumes: `LibraryEntry{ref, file_count, downloading, ...}` (unchanged).
- Produces: every Sonarr series yields an entry. "Empty" = `ref.kind == MediaKind::Tv && file_count == 0 && !downloading` (derived, no new field).

- [ ] **Step 1: Replace the exclusion test with the new rule**

In `tests/media_browser/test_library_view.cpp`, replace the whole `TEST_CASE("A 0-file, non-downloading series produces no entry", ...)` with:

```cpp
TEST_CASE("A 0-file, non-downloading series stays in the Library, empty",
          "[library_view][entries][tv][inclusion]") {
    // Deleting every season of a show (per-season delete) must not look like
    // deleting the show: Sonarr still holds the record, so the Library keeps
    // it — exactly as a file-less movie is kept. Only Remove takes it out.
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(100, "Emptied Show", 2026, kCutoff,
                    {make_season(1, 10, 0), make_season(2, 10, 0)},
                    /*series_level_file_count=*/0),
    };

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads, kNoStarted);

    REQUIRE(entries.size() == 1);
    CHECK(entries[0].ref == tv_ref(100));
    CHECK(entries[0].file_count == 0);
    CHECK(entries[0].total_count == 20);
    CHECK_FALSE(entries[0].downloading);
    CHECK_FALSE(entries[0].watched);  // 0 files can never read as watched
}

TEST_CASE("An emptied show with watch history still reads unwatched",
          "[library_view][entries][tv][inclusion]") {
    // Watched through S1 before the files were deleted: the counts stay
    // season-0-excluded, and the file_count > 0 guard keeps it unwatched,
    // so the Unwatched filter still offers it.
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(100, "Emptied Show", 2026, kCutoff,
                    {make_season(1, 10, 0)}, /*series_level_file_count=*/0),
    };
    const std::unordered_map<int, int> tv_counts{{100, 10}};

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, tv_counts, kNoDownloads, kNoStarted);

    REQUIRE(entries.size() == 1);
    CHECK_FALSE(entries[0].watched);
}
```

(If `tv_counts`' type in `build_library_entries` differs from `std::unordered_map<int,int>`, match the type used by `kNoTvCounts` at the top of the file.)

- [ ] **Step 2: Run to verify failure**

Run: `./magic_dingus_box_cpp/build-mb/test_media_browser_unit "[inclusion]"` after building.
Expected: FAIL — `entries.size() == 1` is 0.

- [ ] **Step 3: Implement**

In `src/media_browser/ui/library_view.cpp`, replace

```cpp
        const bool is_downloading = downloading_refs.count(ref) > 0;
        if (s.episode_file_count <= 0 && !is_downloading) continue;
```

with

```cpp
        const bool is_downloading = downloading_refs.count(ref) > 0;
        // Every series Sonarr holds is an entry, files or not — the same rule
        // movies have always had. A show emptied by per-season deletes used to
        // vanish here and read as removed (Game of Thrones, 2026-08-22); Remove
        // is the only thing that takes a show out of the Library.
```

Update the block comment above it (`---- TV: included when ...`) to say "TV: every series is included; counts below use the season-0-excluded sums". In `library_view.h` update the inclusion-rule comment (near line 86, `series.episode_file_count > 0 || downloading_refs...`) to the new rule.

- [ ] **Step 4: Run tests**

Run the full `test_media_browser_unit`. Expected: PASS. If an older test asserted exclusion elsewhere (e.g. the "Inclusion reads the SERIES-LEVEL stat" case), update its expectation to "included" and keep what it proves about counts.

- [ ] **Step 5: Draw the empty tile**

In `src/media_browser/ui/library_screen.cpp`, directly after the `if (en->ref.kind == MediaKind::Tv) { ... TV chip ... }` block, add:

```cpp
            // Emptied show (every season deleted, nothing in flight): still in
            // the Library by design, so say why it cannot play. Dim the poster
            // and tag its bottom edge; the top-left slot belongs to the TV chip.
            if (en->ref.kind == MediaKind::Tv && en->file_count == 0 &&
                !en->downloading && !state_badge_shown) {
                r.mb_fill_rect(static_cast<float>(x), static_cast<float>(y),
                               static_cast<float>(cell_w),
                               static_cast<float>(poster_h),
                               ::ui::Color(0, 0, 0, 150));
                constexpr int kEmptyFontPx = 12;
                constexpr int kEmptyPadX = 6;
                constexpr int kEmptyPadY = 2;
                const std::string empty_label = "NOTHING DOWNLOADED";
                const int tw = r.mb_text_width(empty_label, kEmptyFontPx);
                const int bw = tw + 2 * kEmptyPadX;
                const int bh = kEmptyFontPx + 2 * kEmptyPadY;
                const int bx = x + (cell_w - bw) / 2;
                const int by = y + poster_h - bh - chrome::kPad1;
                r.mb_fill_rect(static_cast<float>(bx), static_cast<float>(by),
                               static_cast<float>(bw), static_cast<float>(bh),
                               th.bg);
                r.mb_stroke_rect(static_cast<float>(bx), static_cast<float>(by),
                                 static_cast<float>(bw), static_cast<float>(bh),
                                 2.0f, th.dim);
                r.mb_draw_text(empty_label,
                               static_cast<float>(bx + kEmptyPadX),
                               static_cast<float>(by + kEmptyPadY +
                                                  kEmptyFontPx - 2),
                               kEmptyFontPx, th.dim);
            }
```

If the year pill drawn by `draw_poster_card` overlaps the bottom edge, move the tag up by `chrome::kBadgeBoxH + chrome::kPad1` (check `mb_chrome.cpp` `draw_poster_card` for where the year pill sits).

- [ ] **Step 6: Kiosk build compiles**

Run: `magic_dingus_box_cpp/dev/pisim/pisim.sh build`
Expected: `pisim: kiosk binary OK`.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/library_view.cpp magic_dingus_box_cpp/src/media_browser/ui/library_view.h magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp magic_dingus_box_cpp/tests/media_browser/test_library_view.cpp
git commit -m "feat(tv): shows stay in the Library after every season is deleted"
```

---

### Task 2: Eligible and suggested season (pure logic)

**Files:**
- Create: `src/media_browser/ui/season_choice.h`
- Create: `tests/media_browser/test_season_choice.cpp`
- Modify: `CMakeLists.txt` (`MEDIA_BROWSER_TEST_SOURCES`: add `tests/media_browser/test_season_choice.cpp`)

**Interfaces:**
- Consumes: `SeasonRow`, `SeasonState` (`series_detail_logic.h`); `watch_map`, `WatchKey`, `WatchRowLite`, `is_resumable_position` (`episode_logic.h`).
- Produces:
  - `std::vector<int> media_browser::ui::eligible_seasons(const std::vector<SeasonRow>& rows);`
  - `std::optional<int> media_browser::ui::suggested_season(const std::vector<SeasonRow>& rows, const watch_map& watch);`

- [ ] **Step 1: Write the failing tests**

`tests/media_browser/test_season_choice.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/season_choice.h"

using namespace media_browser::ui;

namespace {
SeasonRow row(int n, SeasonState st, int eps = 10, int files = 0) {
    SeasonRow r;
    r.season_number = n;
    r.episode_count = eps;
    r.episode_file_count = files;
    r.state = st;
    return r;
}
watch_map watched_through(int last_season, int eps_per_season = 10) {
    watch_map w;
    for (int s = 1; s <= last_season; ++s)
        for (int e = 1; e <= eps_per_season; ++e)
            w[WatchKey{s, e}] = WatchRowLite{2700, 2700, true};
    return w;
}
}  // namespace

TEST_CASE("eligible: only seasons with nothing on disk and nothing in flight",
          "[season_choice]") {
    const std::vector<SeasonRow> rows = {
        row(1, SeasonState::Complete, 10, 10), row(2, SeasonState::Partial, 10, 3),
        row(3, SeasonState::Downloading), row(4, SeasonState::None),
        row(5, SeasonState::None)};
    CHECK(eligible_seasons(rows) == std::vector<int>{4, 5});
}

TEST_CASE("eligible: specials never appear", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(0, SeasonState::None), row(1, SeasonState::None)};
    CHECK(eligible_seasons(rows) == std::vector<int>{1});
}

TEST_CASE("suggest: emptied Game of Thrones watched through S4 -> 5", "[season_choice]") {
    std::vector<SeasonRow> rows;
    for (int s = 1; s <= 8; ++s) rows.push_back(row(s, SeasonState::None));
    CHECK(suggested_season(rows, watched_through(4)) == 5);
}

TEST_CASE("suggest: new show with no history -> 1", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(1, SeasonState::None), row(2, SeasonState::None),
                                         row(3, SeasonState::None)};
    CHECK(suggested_season(rows, {}) == 1);
}

TEST_CASE("suggest: past what is downloading -> next after the in-flight season",
          "[season_choice]") {
    const std::vector<SeasonRow> rows = {
        row(1, SeasonState::Complete, 10, 10), row(2, SeasonState::Downloading),
        row(3, SeasonState::None), row(4, SeasonState::None), row(5, SeasonState::None)};
    CHECK(suggested_season(rows, watched_through(1)) == 3);
}

TEST_CASE("suggest: nothing eligible after the frontier -> lowest eligible (gap)",
          "[season_choice]") {
    const std::vector<SeasonRow> rows = {
        row(1, SeasonState::None), row(2, SeasonState::Complete, 10, 10),
        row(3, SeasonState::Complete, 10, 10)};
    watch_map w = watched_through(3);
    CHECK(suggested_season(rows, w) == 1);
}

TEST_CASE("suggest: a resume position counts as watched progress", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(1, SeasonState::None), row(2, SeasonState::None),
                                         row(3, SeasonState::None)};
    watch_map w;
    w[WatchKey{2, 4}] = WatchRowLite{900, 2700, false};  // mid-episode in S2
    CHECK(suggested_season(rows, w) == 3);
}

TEST_CASE("suggest: everything on disk -> nullopt", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(1, SeasonState::Complete, 10, 10),
                                         row(2, SeasonState::Complete, 10, 10)};
    CHECK_FALSE(suggested_season(rows, {}).has_value());
}

TEST_CASE("suggest: watched specials never move the frontier", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(1, SeasonState::None), row(2, SeasonState::None)};
    watch_map w;
    w[WatchKey{0, 1}] = WatchRowLite{100, 100, true};
    CHECK(suggested_season(rows, w) == 1);
}
```

Add `tests/media_browser/test_season_choice.cpp` to `MEDIA_BROWSER_TEST_SOURCES` in `CMakeLists.txt`, next to `tests/media_browser/test_series_detail_logic.cpp`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake -S magic_dingus_box_cpp -B magic_dingus_box_cpp/build-mb -DENABLE_MEDIA_BROWSER=ON && cmake --build magic_dingus_box_cpp/build-mb -j8 --target test_media_browser_unit`
Expected: compile error — `season_choice.h` not found.

- [ ] **Step 3: Implement `season_choice.h`**

```cpp
// Which season a TV download should target, and the chooser that lets the
// viewer change it. Pure, header-only, Mac-tested (test_season_choice.cpp).
//
// Separate from series_detail_logic.h because it needs watch_map, and
// episode_logic.h (which defines watch_map) already includes
// series_detail_logic.h — putting this there would be an include cycle.
#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "media_browser/ui/episode_logic.h"
#include "media_browser/ui/series_detail_logic.h"

namespace media_browser::ui {

// Seasons a download can target: season >= 1 with nothing on disk and
// nothing in flight (SeasonState::None). Eligibility is deliberately NOT the
// monitored flag: a monitored season whose search found nothing is still a
// legitimate target — re-issuing it re-runs the search. Ascending.
inline std::vector<int> eligible_seasons(const std::vector<SeasonRow>& rows) {
    std::vector<int> out;
    for (const auto& r : rows) {
        if (r.season_number >= 1 && r.state == SeasonState::None)
            out.push_back(r.season_number);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The season the primary button proposes. frontier = the highest season the
// viewer has touched: any watched episode or resumable position, any file on
// disk, or a download in flight. Suggest the first eligible season past it;
// when none is past it (re-watching, or filling a gap) the lowest eligible.
inline std::optional<int> suggested_season(const std::vector<SeasonRow>& rows,
                                           const watch_map& watch) {
    const std::vector<int> eligible = eligible_seasons(rows);
    if (eligible.empty()) return std::nullopt;
    int frontier = 0;
    for (const auto& kv : watch) {
        if (kv.first.season < 1) continue;
        if (kv.second.watched ||
            is_resumable_position(kv.second.position_s, kv.second.duration_s))
            frontier = std::max(frontier, kv.first.season);
    }
    for (const auto& r : rows) {
        if (r.season_number < 1) continue;
        if (r.episode_file_count > 0 || r.state == SeasonState::Downloading)
            frontier = std::max(frontier, r.season_number);
    }
    for (int s : eligible) {
        if (s > frontier) return s;
    }
    return eligible.front();
}

}  // namespace media_browser::ui
```

Check `is_resumable_position`'s declaration in `episode_logic.h` (it is used in `series_detail_screen.cpp:496`); if it lives elsewhere, include that header instead.

- [ ] **Step 4: Run tests**

Run: `./magic_dingus_box_cpp/build-mb/test_media_browser_unit "[season_choice]"`
Expected: all PASS. (If "a resume position counts" fails because 900/2700 is not "resumable" by `is_resumable_position`'s thresholds, pick a position that is — read the function — rather than changing the rule.)

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/season_choice.h magic_dingus_box_cpp/tests/media_browser/test_season_choice.cpp magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(tv): suggest the season after what the viewer has watched or has"
```

---

### Task 3: SeasonChooser state machine (pure logic)

**Files:**
- Modify: `src/media_browser/ui/season_choice.h`
- Test: `tests/media_browser/test_season_choice.cpp`

**Interfaces:**
- Consumes: `eligible_seasons`, `suggested_season` (Task 2); `estimate_remaining_bytes(rows, runtime_minutes, mb_per_min)` (`series_detail_logic.h:122`).
- Produces:

```cpp
struct SeasonChooser {
    bool choosing = false;
    std::vector<int> candidates;   // eligible seasons, ascending
    int index = 0;                 // into candidates while choosing
    // Idle -> Choosing, starting on `start` (snaps to nearest candidate).
    void open(std::vector<int> eligible, int start);
    // Clamped step; no wrap. No-op when idle.
    void step(int delta);
    // Choosing -> Idle; returns the chosen season (nullopt when idle/empty).
    std::optional<int> confirm();
    void cancel();
    // Re-validate after a rebuild: snap to nearest still-eligible season,
    // cancel when none remain. No-op when idle.
    void revalidate(const std::vector<int>& eligible);
    std::optional<int> current() const;
};
std::string chooser_label(int season, int64_t estimate_bytes, bool estimated);
// -> "\xE2\x80\xB9 Season 5 \xC2\xB7 ~22 GB (est) \xE2\x80\xBA"  (‹ Season 5 · ~22 GB (est) ›)
```

- [ ] **Step 1: Write the failing tests** (append to `test_season_choice.cpp`)

```cpp
TEST_CASE("chooser: opens on the suggested season and steps through candidates, clamped",
          "[season_choice][chooser]") {
    SeasonChooser c;
    CHECK_FALSE(c.current().has_value());
    c.open({3, 5, 6, 8}, 5);
    REQUIRE(c.choosing);
    CHECK(c.current() == 5);
    c.step(+1); CHECK(c.current() == 6);
    c.step(+5); CHECK(c.current() == 8);   // clamped, no wrap
    c.step(-9); CHECK(c.current() == 3);
}

TEST_CASE("chooser: open snaps a non-candidate start to the nearest candidate",
          "[season_choice][chooser]") {
    SeasonChooser c;
    c.open({2, 7}, 5);
    CHECK(c.current() == 7);  // nearest at or above wins a tie-break upward
    SeasonChooser d;
    d.open({2, 7}, 9);
    CHECK(d.current() == 7);
}

TEST_CASE("chooser: confirm returns the season and goes idle; cancel returns nothing",
          "[season_choice][chooser]") {
    SeasonChooser c;
    c.open({4, 5}, 4);
    c.step(+1);
    CHECK(c.confirm() == 5);
    CHECK_FALSE(c.choosing);
    CHECK_FALSE(c.confirm().has_value());
    c.open({4, 5}, 4);
    c.cancel();
    CHECK_FALSE(c.choosing);
}

TEST_CASE("chooser: revalidate snaps or cancels when candidates change",
          "[season_choice][chooser]") {
    SeasonChooser c;
    c.open({4, 5, 6}, 5);
    c.revalidate({4, 6});          // S5 started downloading elsewhere
    CHECK(c.current() == 6);
    c.revalidate({});              // everything now on disk / in flight
    CHECK_FALSE(c.choosing);
}

TEST_CASE("chooser: an empty candidate list never opens", "[season_choice][chooser]") {
    SeasonChooser c;
    c.open({}, 1);
    CHECK_FALSE(c.choosing);
}

TEST_CASE("chooser label: arrows, season, GiB, (est) only when estimated",
          "[season_choice][chooser]") {
    const int64_t gib = 1024LL * 1024 * 1024;
    CHECK(chooser_label(5, 22 * gib, true) ==
          "\xE2\x80\xB9 Season 5 \xC2\xB7 ~22 GB (est) \xE2\x80\xBA");
    CHECK(chooser_label(12, 3 * gib, false) ==
          "\xE2\x80\xB9 Season 12 \xC2\xB7 ~3 GB \xE2\x80\xBA");
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build magic_dingus_box_cpp/build-mb -j8 --target test_media_browser_unit`
Expected: compile error — `SeasonChooser` undeclared.

- [ ] **Step 3: Implement** (append inside the namespace in `season_choice.h`; add `#include <cstdint>` and `#include <string>`)

```cpp
// The primary button's two-step flow: SELECT opens it on the suggested
// season, rotate steps through candidates, SELECT confirms, BTN4 or any focus
// move cancels. Render-thread only, like whole_armed_/remove_pending_.
struct SeasonChooser {
    bool choosing = false;
    std::vector<int> candidates;
    int index = 0;

    void open(std::vector<int> eligible, int start) {
        candidates = std::move(eligible);
        choosing = !candidates.empty();
        index = 0;
        if (!choosing) return;
        snap_to(start);
    }
    void step(int delta) {
        if (!choosing) return;
        index = std::clamp(index + delta, 0,
                           static_cast<int>(candidates.size()) - 1);
    }
    std::optional<int> confirm() {
        if (!choosing) return std::nullopt;
        const int season = candidates[static_cast<size_t>(index)];
        cancel();
        return season;
    }
    void cancel() {
        choosing = false;
        candidates.clear();
        index = 0;
    }
    void revalidate(const std::vector<int>& eligible) {
        if (!choosing) return;
        const int was = candidates[static_cast<size_t>(index)];
        candidates = eligible;
        if (candidates.empty()) { cancel(); return; }
        snap_to(was);
    }
    std::optional<int> current() const {
        if (!choosing) return std::nullopt;
        return candidates[static_cast<size_t>(index)];
    }

private:
    // First candidate >= season, else the last one.
    void snap_to(int season) {
        index = static_cast<int>(candidates.size()) - 1;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i] >= season) { index = static_cast<int>(i); break; }
        }
    }
};

// "‹ Season 5 · ~22 GB (est) ›". GiB, same unit as whole_series_label; the
// "(est)" suffix whenever the runtime behind the estimate was assumed.
inline std::string chooser_label(int season, int64_t estimate_bytes,
                                 bool estimated) {
    return std::string("\xE2\x80\xB9 Season ") + std::to_string(season) +
           " \xC2\xB7 ~" +
           std::to_string(estimate_bytes / (1024LL * 1024 * 1024)) + " GB" +
           (estimated ? " (est)" : "") + " \xE2\x80\xBA";
}
```

- [ ] **Step 4: Run tests**

Run: `./magic_dingus_box_cpp/build-mb/test_media_browser_unit "[season_choice]"`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/season_choice.h magic_dingus_box_cpp/tests/media_browser/test_season_choice.cpp
git commit -m "feat(tv): season chooser state machine for the primary download button"
```

---

### Task 4: Action row driven by the suggested season

**Files:**
- Modify: `src/media_browser/ui/series_detail_logic.h:328-440` (`Action`, `ActionRowInputs`, `decide_action_row`)
- Modify: `src/media_browser/ui/series_detail_screen.cpp:469-480` (`rebuild_buttons`) and `:900` (`case Action::AddSeason1`)
- Test: `tests/media_browser/test_series_detail_logic.cpp:270-330`

**Interfaces:**
- Consumes: `suggested_season` (Task 2).
- Produces:
  - `enum class Action { PlayNextUp, AddSeason, NextSeason, WholeSeries, Remove, ConfirmRemove };` (`AddSeason1` renamed)
  - `ActionRowInputs::primary_season` (`std::optional<int>`, replaces `next_unmonitored`) and `ActionRowInputs::primary_label_override` (`std::optional<std::string>`).
  - Labels: not in library → `"Add Season " + N` (N = `primary_season.value_or(1)`); in library → `"Download Season " + N`; either replaced by `primary_label_override` when set.

- [ ] **Step 1: Update and add tests**

In `test_series_detail_logic.cpp`: replace every `Action::AddSeason1` with `Action::AddSeason` and every `in.next_unmonitored = ...` with `in.primary_season = ...`. Then add:

```cpp
TEST_CASE("action row: not-in-library add targets the suggested season",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::NotInLibrary);
    in.primary_season = 3;
    const auto row = decide_action_row(in);
    REQUIRE_FALSE(row.buttons.empty());
    CHECK(row.buttons[0].action == Action::AddSeason);
    CHECK(row.buttons[0].label == "Add Season 3");
}

TEST_CASE("action row: not-in-library with no suggestion still offers Season 1",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::NotInLibrary);
    const auto row = decide_action_row(in);
    CHECK(row.buttons[0].label == "Add Season 1");
}

TEST_CASE("action row: the chooser label replaces the primary label, same action",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = true;
    in.primary_season = 5;
    in.primary_label_override = "\xE2\x80\xB9 Season 6 \xC2\xB7 ~20 GB \xE2\x80\xBA";
    in.prev_focus_action = Action::NextSeason;
    const auto row = decide_action_row(in);
    REQUIRE_FALSE(row.buttons.empty());
    const auto& b = row.buttons[static_cast<size_t>(row.focus)];
    CHECK(b.action == Action::NextSeason);
    CHECK(b.label == "\xE2\x80\xB9 Season 6 \xC2\xB7 ~20 GB \xE2\x80\xBA");
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build magic_dingus_box_cpp/build-mb -j8 --target test_media_browser_unit`
Expected: compile errors (`AddSeason`, `primary_season` undeclared).

- [ ] **Step 3: Implement in `series_detail_logic.h`**

- Rename `AddSeason1` → `AddSeason` in the `Action` enum.
- In `ActionRowInputs`, replace `std::optional<int> next_unmonitored;` (and its comment) with:

```cpp
    // season_choice.h's suggested_season(rows, watch): the season the primary
    // button targets. nullopt when nothing is eligible — the in-library
    // button hides (as it did when every season was monitored); the
    // not-in-library add falls back to Season 1.
    std::optional<int> primary_season;
    // The SeasonChooser's label while it is open ("‹ Season 6 · ~20 GB ›").
    // Same button, same Action — only the text changes, so focus identity
    // holds while choosing.
    std::optional<std::string> primary_label_override;
```

- NotInLibrary branch: replace `out.buttons.push_back({Action::AddSeason1, "Add Season 1"});` with

```cpp
        out.buttons.push_back(
            {Action::AddSeason,
             in.primary_label_override.value_or(
                 "Add Season " + std::to_string(in.primary_season.value_or(1)))});
```

- InLibrary branch: replace the `if (in.series_settled && in.next_unmonitored.has_value())` block's primary push with

```cpp
        if (in.series_settled && in.primary_season.has_value()) {
            out.buttons.push_back(
                {Action::NextSeason,
                 in.primary_label_override.value_or(
                     "Download Season " + std::to_string(*in.primary_season))});
```

(keep the WholeSeries push that follows it unchanged).

- [ ] **Step 4: Adapt the screen so the kiosk still compiles**

In `series_detail_screen.cpp`:
- add `#include "media_browser/ui/season_choice.h"`;
- in `rebuild_buttons()`, replace `in.next_unmonitored = next_unmonitored_season(rows_);` with `in.primary_season = suggested_season(rows_, episode_watch_);`;
- rename `case Action::AddSeason1:` to `case Action::AddSeason:` and any other `AddSeason1` reference (`grep -n AddSeason1 src/media_browser/ui/*.cpp src/media_browser/ui/*.h`);
- in `case Action::NextSeason:` replace `const auto next = next_unmonitored_season(rows_);` with `const auto next = suggested_season(rows_, episode_watch_);` (Task 5 replaces this with the chooser).

- [ ] **Step 5: Run tests + kiosk build**

Run: `./magic_dingus_box_cpp/build-mb/test_media_browser_unit` → all PASS.
Run: `magic_dingus_box_cpp/dev/pisim/pisim.sh build` → `kiosk binary OK`.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_logic.h magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp magic_dingus_box_cpp/tests/media_browser/test_series_detail_logic.cpp
git commit -m "feat(tv): primary download button targets the suggested season"
```

---

### Task 5: Wire the chooser into SeriesDetail

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.h` (member near `whole_armed_`, line ~233)
- Modify: `src/media_browser/ui/series_detail_screen.cpp` — `rebuild_buttons()` (~469), rotate handler (~2086), BTN4 handler (~2080), PREV/NEXT paging (~2141), SELECT on the action row (~2213), `case Action::AddSeason` (~900), `case Action::NextSeason` (~1040).

**Interfaces:**
- Consumes: `SeasonChooser`, `chooser_label`, `eligible_seasons`, `suggested_season` (Tasks 2-3); `ActionRowInputs::primary_label_override`, `Action::AddSeason` (Task 4); existing `start_season_download(int)`, `spawn_mutation`, `sonarr_.add_series(int tmdb_id, int qp_id, bool monitor, const std::string& title)`, `estimate_remaining_bytes`, `mb_per_min_`, `series_`, `in_library_`.
- Produces: `void SeriesDetailScreen::start_add_at_season(int season);` (private).

- [ ] **Step 1: State**

In `series_detail_screen.h`, next to `whole_armed_`, add:

```cpp
    // Primary-button season chooser (season_choice.h). Render-thread only,
    // same discipline as whole_armed_ / remove_pending_.
    SeasonChooser season_chooser_;
```

and declare `void start_add_at_season(int season);` beside `start_season_download`. Include `media_browser/ui/season_choice.h` in the header.

- [ ] **Step 2: Label while choosing**

In `rebuild_buttons()`, after `in.primary_season = ...`:

```cpp
    season_chooser_.revalidate(eligible_seasons(rows_));
    if (const auto cur = season_chooser_.current()) {
        std::vector<SeasonRow> one;
        for (const auto& r : rows_)
            if (r.season_number == *cur) one.push_back(r);
        const int runtime = (in_library_ && series_.has_value())
                                ? series_->runtime_minutes : 0;
        in.primary_label_override = chooser_label(
            *cur, estimate_remaining_bytes(one, runtime, mb_per_min_),
            /*estimated=*/runtime <= 0);
    }
```

- [ ] **Step 3: Input while choosing**

At the top of the rotate branch (`if ((e.action == ROTATE || ROTATE_VERTICAL) && e.delta != 0) {`, after the `busy` check), add:

```cpp
            if (season_chooser_.choosing) {
                season_chooser_.step(e.delta);
                rebuild_buttons();
                continue;  // the chooser owns rotation while open
            }
```

In the BTN4 (`SETTINGS_MENU`) branch, before `return origin_;`:

```cpp
            if (season_chooser_.choosing) {
                season_chooser_.cancel();
                rebuild_buttons();
                continue;  // back closes the chooser, not the screen
            }
```

In both PREV and NEXT paging branches, and wherever the existing code does "Any navigation cancels BOTH pending confirms" (`whole_armed_ = false; remove_pending_ = false;`), also call `season_chooser_.cancel();` so a page flip closes it.

- [ ] **Step 4: SELECT opens / confirms**

In the action-row SELECT path, replace

```cpp
            if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size()))
                dispatch_action(buttons_[static_cast<size_t>(focus_)].action);
```

with

```cpp
            if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size())) {
                const Action a = buttons_[static_cast<size_t>(focus_)].action;
                if (a == Action::AddSeason || a == Action::NextSeason) {
                    if (!season_chooser_.choosing) {
                        // Press 1: open on the suggestion (Season 1 for a
                        // brand-new show with no rows yet eligible).
                        const auto elig = eligible_seasons(rows_);
                        season_chooser_.open(
                            elig, suggested_season(rows_, episode_watch_)
                                      .value_or(elig.empty() ? 1 : elig.front()));
                        rebuild_buttons();
                        continue;
                    }
                    // Press 2: start the chosen season.
                    const auto season = season_chooser_.confirm();
                    rebuild_buttons();
                    if (!season.has_value()) continue;
                    if (a == Action::NextSeason) start_season_download(*season);
                    else start_add_at_season(*season);
                    continue;
                }
                dispatch_action(a);
            }
```

`dispatch_action(Action::NextSeason)` is still used by the season-end intent until Task 6; leave its case as Task 4 left it.

- [ ] **Step 5: `start_add_at_season`**

Move the body of `case Action::AddSeason:` into `void SeriesDetailScreen::start_add_at_season(int season)` and make `case Action::AddSeason:` call `start_add_at_season(suggested_season(rows_, episode_watch_).value_or(1)); break;`. Inside the worker:

- Season 1: unchanged — `sonarr_.add_series(id, qp_id, /*monitor=*/true, title)` (Sonarr's `firstSeason` + search).
- Season > 1: `sonarr_.add_series(id, qp_id, /*monitor=*/false, title)` (`monitor="none"`, no search). On `res.ok && res.settled`, publish the record the same way the Season-1 path does, and set a new render-thread-consumed intent `mut_start_season_ = season;` (add `std::optional<int> mut_start_season_;` beside `mut_series_`, guarded by `mut_mtx_`). On `!res.settled`: toast `title + ": added \xE2\x80\x94 choose the season again in a moment"` and do NOT set the intent. Never monitor Season 1 here.

Where the screen drains `mut_series_`/`mut_toast_` on the render thread (search `mut_toast_` consumption in `update()`), also drain `mut_start_season_`: after the record is applied and `rebuild_rows()` has run, call `start_season_download(*intent)` if the season is in `eligible_seasons(rows_)`, else toast `"Season N isn't available to download"`.

Toast for the chosen-season add (press feedback, before the worker): `title + ": adding Season " + std::to_string(season) + "\xE2\x80\xA6"`.

- [ ] **Step 6: Kiosk build + full Mac suite**

Run: `magic_dingus_box_cpp/dev/pisim/pisim.sh check`
Expected: `kiosk binary OK`, 9/9 suites pass.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp
git commit -m "feat(tv): choose the season before downloading — suggested season, adjustable"
```

---

### Task 6: Season-end card starts the season it offered

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.cpp:163-180`

**Interfaces:**
- Consumes: `eligible_seasons` (Task 2), `start_season_download(int)`.

- [ ] **Step 1: Replace the re-validation**

Replace

```cpp
        const auto target = next_unmonitored_season(rows_);
        if (series_.has_value() && series_->sonarr_id > 0 && series_settled_ &&
            target.has_value() && *target == want) {
            dispatch_action(Action::NextSeason);
        } else {
```

with

```cpp
        // The card offered the season after the one just finished. Honour
        // exactly that season while it is still downloadable; comparing it to
        // next_unmonitored_season refused it on any show with a deleted
        // earlier season (emptied GoT: finish S5, offered S6, refused).
        const auto elig = eligible_seasons(rows_);
        if (series_.has_value() && series_->sonarr_id > 0 && series_settled_ &&
            std::find(elig.begin(), elig.end(), want) != elig.end()) {
            start_season_download(want);
        } else {
```

- [ ] **Step 2: Build**

Run: `magic_dingus_box_cpp/dev/pisim/pisim.sh build` → `kiosk binary OK`.

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp
git commit -m "fix(tv): season-end card starts the season it offered, even after deletes"
```

---

### Task 7: Hardware verification on the Pi 5 + changelog

**Files:**
- Modify: `CHANGELOG.md` (`[Unreleased]` → `### Added` / `### Fixed`)

- [ ] **Step 1: Deploy**

```bash
magic_dingus_box_cpp/dev/pisim/pisim.sh check
PI_HOST=magic@magicpi5.local magic_dingus_box_cpp/dev/pisim/pisim.sh push
```

Expected: `✓ kiosk active on new binary`.

- [ ] **Step 2: Record Sonarr state before (GoT = series id 7 on this box)**

```bash
ssh magic@magicpi5.local 'set -a; . /opt/magic_dingus_box/services/.env; set +a; curl -s -H "X-Api-Key: $SONARR_API_KEY" http://127.0.0.1:8989/api/v3/series/7 | python3 -c "import json,sys; s=json.load(sys.stdin); print(\"series monitored:\", s[\"monitored\"], [(x[\"seasonNumber\"],x[\"monitored\"]) for x in s[\"seasons\"]])"'
```

Expected: every season `False` (the series-level `monitored` may be either).

- [ ] **Step 3: Owner at the TV**

1. Media Browser → Library: Game of Thrones is present, dimmed, "NOTHING DOWNLOADED".
2. Open it: primary button reads "Download Season 5".
3. Press SELECT: label becomes `‹ Season 5 · ~NN GB ›`; turn the knob → Season 6; press BTN4 → label returns to "Download Season 5", nothing started (re-run Step 2: still all `False`).
4. SELECT, step to Season 6, SELECT: toast "…Season 6…".

- [ ] **Step 4: Verify Sonarr received exactly Season 6**

Re-run Step 2's command. Expected: `series monitored: True`, `(6, True)`, all others `False`. Then:

```bash
ssh magic@magicpi5.local 'set -a; . /opt/magic_dingus_box/services/.env; set +a; curl -s -H "X-Api-Key: $SONARR_API_KEY" "http://127.0.0.1:8989/api/v3/command" | python3 -c "import json,sys; [print(c[\"name\"], c.get(\"body\",{}).get(\"seasonNumber\"), c[\"status\"]) for c in json.load(sys.stdin)[:5]]"'
```

Expected: a `SeasonSearch 6` entry.

- [ ] **Step 4b: New show at Season 3**

Pick a show NOT in the Library, open it from Browse/Search, press the primary button ("Add Season 1"), SELECT, step to Season 3, SELECT. Then verify over the Sonarr API (substitute the new series id from `/api/v3/series`):

```bash
ssh magic@magicpi5.local 'set -a; . /opt/magic_dingus_box/services/.env; set +a; ID=<new series id>; K="X-Api-Key: $SONARR_API_KEY"; curl -s -H "$K" http://127.0.0.1:8989/api/v3/series/$ID | python3 -c "import json,sys; s=json.load(sys.stdin); print(\"series monitored:\", s[\"monitored\"], [(x[\"seasonNumber\"],x[\"monitored\"]) for x in s[\"seasons\"]])"; curl -s -H "$K" "http://127.0.0.1:8989/api/v3/episode?seriesId=$ID" | python3 -c "import json,sys; from collections import defaultdict; d=defaultdict(set); [d[e[\"seasonNumber\"]].add(e[\"monitored\"]) for e in json.load(sys.stdin)]; print(dict(d))"; curl -s -H "$K" http://127.0.0.1:8989/api/v3/command | python3 -c "import json,sys; [print(c[\"name\"], c.get(\"body\",{}).get(\"seasonNumber\"), c[\"status\"]) for c in json.load(sys.stdin)[:5]]"'
```

Expected: `series monitored: True`; only `(3, True)` among the seasons (Season 1 NOT monitored); Season 3's episodes all `{True}`; a `SeasonSearch 3` command. Then Remove the show from the kiosk (arm + confirm).

- [ ] **Step 5: Changelog**

Under `[Unreleased]`, add to `### Added`:

```markdown
- **Choose which season to download.** A show's page now suggests the
  season after the last one you watched or have — "Download Season 5"
  if you've seen the first four — and pressing it lets you turn the knob
  (or press Left/Right) to pick any other season before it starts. New
  shows work the same way ("Add Season 3"). Press back to change your
  mind; nothing downloads until you confirm.
```

and to `### Fixed`:

```markdown
- **Deleting every season no longer makes a show disappear.** Clearing
  old seasons to make room used to remove the show from your Library
  entirely once the last one went, and downloading it again started
  from Season 1. The show now stays in your Library, marked "Nothing
  downloaded", ready for you to pick the next season.
- **"Start Season N" after a season finale now works on shows where
  earlier seasons were deleted** — it used to say the update didn't
  apply.
```

- [ ] **Step 6: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs(changelog): TV season choice + emptied shows stay in Library"
```
