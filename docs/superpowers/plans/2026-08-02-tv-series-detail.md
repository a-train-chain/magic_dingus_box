# TV Series Detail (Phase 2c-2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Selecting a TV poster in Browse opens a new SeriesDetailScreen — poster/overview + a season list with per-season state — from which the user can add Season 1, download the next season, add the whole series behind the codebase's first blocking disk preflight, and remove the series orphan-proof.

**Architecture:** A pure, Renderer-free `series_detail_logic.h` (browse_logic.h house style) owns every decision — season-row merge (TMDB base + Sonarr statistics overlay, immune to the `settled==false ⇒ empty seasons` contract), season state, screen state resolution, disk estimate, and the block/warn/allow verdict — all Mac-tested. A new GLES `SeriesDetailScreen` copies DetailScreen's proven thread idioms (FetchWorker+DoneFlag for loads, single member thread + atomics for mutations, `catch (const std::system_error&)` on every spawn, gen-bump + join in the dtor) and the 2c-1 render discipline (no early return that skips last-drawn chrome). Registration is the known 4-part addition; the dispatch switch has no `default:`, so the compiler enforces completeness.

**Tech Stack:** C++17, Catch2 v3 (Mac `test_media_browser_unit`), TMDB v3 `/tv/{id}`, Sonarr v4 API via the Phase-2b `SonarrClient`, OpenGL ES via the existing `Renderer` (kiosk-only).

## Global Constraints

- **Repo root (quote every path — spaces AND emoji):** `/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box `
- **Work tree for this phase:** `<repo-root>/.worktrees/tvdetail` on branch `feat/tv-series-detail`, starting from `d096f43` (v1.8.0). Every path below is relative to `<worktree>/magic_dingus_box_cpp/` unless written absolute.
- **ONE implementer per worktree; reviewers NEVER change git state** (no commit/stash/checkout/reset — `git stash` once destroyed in-progress work here). Serialize all agents.
- **DO NOT TOUCH:** `magic_dingus_box_cpp/scripts/setup_services.sh` and the repo-root `scripts/golden_image/` (other sessions own them), and the repo-root main checkout (it sits on another session's branch).
- **Commit style:** `feat(mb): …` / `fix(mb): …` / `test(mb): …`, each ending with a blank line then `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- **Anchor every edit on quoted surrounding text, never line numbers.**
- **Mac test loop.** Configure once:
  ```bash
  cmake -S "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvdetail/magic_dingus_box_cpp" \
        -B "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvdetail/magic_dingus_box_cpp/build-mb" \
        -DBUILD_KIOSK=OFF -DBUILD_TESTS=ON -DENABLE_MEDIA_BROWSER=ON
  ```
  Then per test step:
  ```bash
  cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvdetail/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
    && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvdetail/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
  ```
  Re-run the configure line whenever a task adds a file to `CMakeLists.txt`.
- **Green baseline that must not regress: 281 cases / 5692 assertions.** Per-task gates are stated as **"+N cases from this task's listed TEST_CASE blocks"** relative to the count the task starts from, NOT absolute totals — 2c-1's absolute targets drifted when review fixes legitimately added cases.
- **Pi kiosk compile-verify only via an isolated scratch build in `~/mdb-2c2` on `magic@magicpi5.local`. NEVER `deploy_cpp.sh`** (its DEFAULT host is `magic@magicpi.local` — a different box — and it restarts the live kiosk). Never restart `magic-dingus-box-cpp.service`. Launch long builds with `setsid nohup sh -c "make -C build -j2 … > build.log 2>&1; echo EXIT=\$? >> build.log" >/dev/null 2>&1 </dev/null &` — a bare `nohup make &` died silently at 68% when the launching SSH connection dropped, and it looks identical to a stalled build. Poll the log from a FRESH connection; **`pgrep -f` self-matches its own SSH command line.** `assets/` must be in the rsync or CMake fails late.
- **Kiosk compiles `-Wall -Wextra -Wpedantic`; test target `-Wall -Wextra`. No warnings in new code.** (Known pre-existing: `main.cpp:505 -Wrange-loop-construct` and 5 others — not yours.)
- **`WatchdogSec=10`: every Sonarr mutation runs on a worker thread.** `add_series` alone can take ≈13.5 s (`add_settle_timeout_ms + add_settle_poll_ms + timeout_secs`). Copy DetailScreen's spawn idiom including `catch (const std::system_error&)` — an uncaught throw from a thread ctor is `std::terminate`, a hard kiosk crash.
- **`settled == false ⇒ `AddSeriesResult::series.seasons` is ALWAYS EMPTY** — never render that as "0 seasons". Two meanings: poll timeout (transient) or never-refreshed existing record (PERMANENT for announced series). The season list therefore ALWAYS builds from TMDB's `TmdbTvDetail::seasons` as the base, with Sonarr statistics overlaid when present — the UI never depends on Sonarr seasons existing.
- **`Series : SeriesSearchHit` SLICES** — assigning a `Series` into a `SeriesSearchHit` drops `sonarr_id`/`path`/`monitored`. Never pass a `Series` by value as its base.
- **`SeriesSearchHit::runtime_minutes` is PER-EPISODE** (the disk-estimate multiplicand).
- **Render discipline (2c-1 lesson, structural):** a screen's render() must not gain an exit path that skips its last-drawn chrome. SeriesDetailScreen has no input-swallowing modal (its confirms are button-label swaps), so the binding form here is: the transient banner/Toast region is the last draw, and every state path falls through to it — no `return` after the header except the whole-body early states, which draw no interactive affordance.
- **Mock-honesty rule (final-review carry-forward, verbatim intent):** `SonarrMockClient` overrides every public virtual by contract. Whichever task adds the FIRST kiosk caller of a Sonarr method must make the corresponding mock override honest in the SAME change — the standing precedent is `get_library_checked() → std::nullopt` ("engaged = the service answered"; commit `63f9046`). This plan deliberately consumes NO newly-trapped method: in-library detection uses `get_library_checked` (mock already honest), so `find_series_by_tvdb` AND `is_reachable` keep zero kiosk callers and remain documented traps — do not add a caller to either. The one new virtual (Task 2's `get_quality_definitions`) gets its mock override in the same task.
- **`sonarr_configured` gates everything Sonarr-shaped.** BrowseScreen already receives `/*sonarr_configured=*/!sonarr_key.empty()` (main.cpp:885). SeriesDetailScreen receives the same flag; when false it never calls Sonarr, offers no mutation, and shows the established copy family ("TV library not set up on this box").
- **Out of scope — do not implement:** queue-screen grouping by downloadId, Library mixed-kind listing, Search TV mode, `TmdbClient::search_tv`, episode picker / TV playback (Phase 3), any Sonarr health gate, per-episode monitoring UI.

---

## File Structure

**Created**

| File | Responsibility |
|---|---|
| `src/media_browser/ui/series_detail_logic.h` | ALL decisions, pure + header-only: `SeasonRow`/`SeasonState`, `merge_season_rows`, `decide_season_state`, `SeriesDetailState` + `SeriesDetailInputs` + `decide_series_detail_state` + `series_detail_state_message`, `next_unmonitored_season`, `estimate_remaining_bytes`, `pick_preferred_mb_per_min`, `whole_series_verdict`. |
| `src/media_browser/ui/series_detail_screen.{h,cpp}` | The GLES screen: load workers, action row, season list render, mutation workers, re-poll. Kiosk-only (`KIOSK_MEDIA_BROWSER_SOURCES`). |
| `tests/media_browser/test_series_detail_logic.cpp` | The pure core's suite. |

**Modified**

| File | Change |
|---|---|
| `src/media_browser/sonarr/sonarr_types.h` | `QualityDefinition{quality_id, title, preferred_mb_per_min, max_mb_per_min}`. |
| `src/media_browser/sonarr/sonarr_client.{h,cpp}` | `virtual std::vector<QualityDefinition> get_quality_definitions();` + `parse_quality_definitions` in `sonarr_parsers.{h,cpp}`. |
| `src/media_browser/sonarr/sonarr_mock.{h,cpp}` | Override `get_quality_definitions` (fixture-shaped values — config data, fabricates nothing). |
| `src/media_browser/ui/mb_screen.h` | `Screen::SeriesDetail` enum value. |
| `src/media_browser/ui/browse_screen.{h,cpp}` | `MediaKind selected_kind()` accessor; TV SELECT routes to `Screen::SeriesDetail` instead of the Toast; footer hint "Coming soon" → "Detail". |
| `src/main.cpp` | SeriesDetailScreen instance (radarr-free: `sonarr, *tmdb, qbit_owned.get(), state, sonarr_configured`), dispatch case, `set_tmdb_id`/`set_origin` forwarding block. |
| `CMakeLists.txt` | `series_detail_screen.cpp` → `KIOSK_MEDIA_BROWSER_SOURCES`; `test_series_detail_logic.cpp` → `MEDIA_BROWSER_TEST_SOURCES`. |
| `tests/media_browser/test_sonarr_client.cpp` | Mock contract updates (Tasks 2 and 4). |

**Task map (9 tasks).** 1 pure logic core · 2 quality definitions endpoint · 3 screen read-only core · 4 registration + Browse routing + mock honesty · 5 add flows (Season 1 / next season) · 6 whole-series confirm + blocking preflight · 7 orphan-proof remove · 8 downloading badges + quiet re-poll · 9 Pi verify (both flags) + acceptance checklist. Tasks 1–2 are strictly test-first; 3–8 each open with any new pure helper + its failing test, then the Renderer-bound half; the kiosk binary compiles at the END of every task (no multi-task broken windows this phase — Task 3's screen is registered nowhere but compiles standalone).

---
### Task 1: series_detail_logic.h — the pure decision core

**Files:**
- Create: `src/media_browser/ui/series_detail_logic.h`
- Create: `tests/media_browser/test_series_detail_logic.cpp`
- Modify: `CMakeLists.txt` (test file into `MEDIA_BROWSER_TEST_SOURCES`)

**Interfaces:**
- Consumes: `TmdbTvSeason` (tmdb_client.h:84), `Series`/`Season` (sonarr_types.h), `QualityDefinition` (Task 2 adds it — THIS task forward-declares nothing; `pick_preferred_mb_per_min` is written against a minimal struct THIS task defines, which Task 2 then moves — see Step 3 note).
- Produces (Tasks 3–8 rely on these exact names): `SeasonState`, `SeasonRow`, `decide_season_state(int,int,bool)`, `merge_season_rows(const std::vector<TmdbTvSeason>&, const Series*, const std::unordered_set<int>&)`, `next_unmonitored_season(const std::vector<SeasonRow>&)`, `estimate_remaining_bytes(const std::vector<SeasonRow>&, int, double)`, `pick_preferred_mb_per_min(const std::vector<QualityDefinition>&)`, `DiskVerdict`, `whole_series_verdict(int64_t, std::optional<int64_t>)`, `kDiskFloorBytes`, `SeriesDetailState`, `SeriesDetailInputs`, `decide_series_detail_state(...)`, `series_detail_state_message(SeriesDetailState)`.

- [ ] **Step 1: Write the failing test file**

Create `tests/media_browser/test_series_detail_logic.cpp` exactly:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/series_detail_logic.h"

using namespace media_browser;
using namespace media_browser::ui;

namespace {
TmdbTvSeason tv_season(int n, int eps) {
    TmdbTvSeason s;
    s.season_number = n;
    s.episode_count = eps;
    return s;
}
Season sonarr_season(int n, bool mon, int eps, int files, int64_t bytes) {
    return Season{n, mon, eps, files, bytes};
}
}  // namespace

TEST_CASE("season rows: TMDB-only base, specials excluded, sorted", "[series_detail]") {
    std::vector<TmdbTvSeason> tmdb = {tv_season(2, 13), tv_season(0, 5), tv_season(1, 7)};
    auto rows = merge_season_rows(tmdb, nullptr, {});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].season_number == 1);
    CHECK(rows[0].episode_count == 7);
    CHECK_FALSE(rows[0].monitored);
    CHECK(rows[0].episode_file_count == 0);
    CHECK(rows[0].state == SeasonState::None);
    CHECK(rows[1].season_number == 2);
}

TEST_CASE("season rows: Sonarr overlay wins on stats; unsettled keeps TMDB base",
          "[series_detail]") {
    std::vector<TmdbTvSeason> tmdb = {tv_season(1, 7), tv_season(2, 13)};

    SECTION("settled series overlays counts, files, monitored; extra season appended") {
        Series s;
        s.seasons = {sonarr_season(0, false, 5, 0, 0),
                     sonarr_season(1, true, 7, 7, 1),
                     sonarr_season(3, false, 10, 0, 0)};
        auto rows = merge_season_rows(tmdb, &s, {});
        REQUIRE(rows.size() == 3);  // 1, 2 (tmdb-only), 3 (sonarr-only); specials dropped
        CHECK(rows[0].monitored);
        CHECK(rows[0].episode_file_count == 7);
        CHECK(rows[0].state == SeasonState::Complete);
        CHECK(rows[1].season_number == 2);
        CHECK_FALSE(rows[1].monitored);
        CHECK(rows[2].season_number == 3);
        CHECK(rows[2].episode_count == 10);
    }

    SECTION("settled==false contract: empty sonarr seasons ⇒ rows are the TMDB base, never empty") {
        Series s;  // add_series with settled==false: seasons ALWAYS empty
        REQUIRE(s.seasons.empty());
        auto rows = merge_season_rows(tmdb, &s, {});
        REQUIRE(rows.size() == 2);  // the "0 seasons" hazard, pinned
        CHECK(rows[0].episode_count == 7);
    }
}

TEST_CASE("season state precedence: downloading > complete > partial > none",
          "[series_detail]") {
    CHECK(decide_season_state(10, 10, true) == SeasonState::Downloading);
    CHECK(decide_season_state(10, 10, false) == SeasonState::Complete);
    CHECK(decide_season_state(10, 3, false) == SeasonState::Partial);
    CHECK(decide_season_state(10, 0, false) == SeasonState::None);
    // Unknown episode_count with files present must not claim Complete.
    CHECK(decide_season_state(0, 3, false) == SeasonState::Partial);
    CHECK(decide_season_state(0, 0, false) == SeasonState::None);
}

TEST_CASE("next_unmonitored_season: first ascending unmonitored, nullopt when all monitored",
          "[series_detail]") {
    std::vector<SeasonRow> rows(3);
    rows[0].season_number = 1; rows[0].monitored = true;
    rows[1].season_number = 2; rows[1].monitored = false;
    rows[2].season_number = 3; rows[2].monitored = false;
    auto next = next_unmonitored_season(rows);
    REQUIRE(next.has_value());
    CHECK(*next == 2);
    rows[1].monitored = rows[2].monitored = true;
    CHECK_FALSE(next_unmonitored_season(rows).has_value());
    CHECK_FALSE(next_unmonitored_season({}).has_value());
}

TEST_CASE("estimate_remaining_bytes: missing episodes only, with defensive fallbacks",
          "[series_detail]") {
    std::vector<SeasonRow> rows(2);
    rows[0].episode_count = 10; rows[0].episode_file_count = 7;   // 3 missing
    rows[1].episode_count = 13; rows[1].episode_file_count = 0;   // 13 missing
    // 16 eps × 45 min × 70 MB/min × 1 MiB = 16*45*70 MiB
    const int64_t expected = 16LL * 45 * 70 * 1024 * 1024;
    CHECK(estimate_remaining_bytes(rows, 45, 70.0) == expected);
    // runtime<=0 falls back to 45; rate<=0 falls back to 70 — a zero estimate
    // would silently ALLOW a whole-series add past the blocking preflight.
    CHECK(estimate_remaining_bytes(rows, 0, 70.0) == expected);
    CHECK(estimate_remaining_bytes(rows, 45, 0.0) == expected);
    // files >= count clamps to zero missing, never negative
    rows[0].episode_file_count = 12; rows[1].episode_file_count = 13;
    CHECK(estimate_remaining_bytes(rows, 45, 70.0) == 0);
}

TEST_CASE("pick_preferred_mb_per_min: 1080p family max, 720p fallback, 70 default",
          "[series_detail]") {
    std::vector<QualityDefinition> defs = {
        {4, "HDTV-720p", 40.0, 60.0},
        {9, "HDTV-1080p", 70.0, 100.0},
        {3, "WEBDL-1080p", 65.0, 100.0},
    };
    CHECK(pick_preferred_mb_per_min(defs) == 70.0);
    defs.erase(defs.begin() + 1);  // drop HDTV-1080p → WEBDL-1080p (65) wins
    CHECK(pick_preferred_mb_per_min(defs) == 65.0);
    defs.erase(defs.begin() + 1);  // only 720p left
    CHECK(pick_preferred_mb_per_min(defs) == 40.0);
    CHECK(pick_preferred_mb_per_min({}) == 70.0);
    // preferred<=0 rows (unlimited/null upstream) are skipped, not returned
    CHECK(pick_preferred_mb_per_min({{9, "HDTV-1080p", 0.0, 0.0}}) == 70.0);
}

TEST_CASE("whole_series_verdict: block at free minus 20 GiB floor; stat failure warns",
          "[series_detail]") {
    const int64_t GiB = 1024LL * 1024 * 1024;
    // free 100 GiB, floor 20 → budget 80
    CHECK(whole_series_verdict(80 * GiB, 100 * GiB) == DiskVerdict::Allow);
    CHECK(whole_series_verdict(80 * GiB + 1, 100 * GiB) == DiskVerdict::Block);
    CHECK(whole_series_verdict(1, std::nullopt) == DiskVerdict::WarnOnly);
    CHECK(whole_series_verdict(1, int64_t{0}) == DiskVerdict::WarnOnly);
}

TEST_CASE("series detail state resolver: precedence and copy", "[series_detail]") {
    SeriesDetailInputs in;
    in.tmdb_done = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::Loading);
    in.tmdb_done = true; in.tmdb_ok = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::TmdbError);
    in.tmdb_ok = true; in.sonarr_configured = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::NotConfigured);
    in.sonarr_configured = true; in.sonarr_done = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::Loading);
    in.sonarr_done = true; in.sonarr_ok = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::SonarrUnreachable);
    in.sonarr_ok = true; in.in_library = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::NotInLibrary);
    in.in_library = true;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::InLibrary);

    // Copy: the NotConfigured string is the SAME literal browse ships —
    // "TV library not set up on this box" — so the two screens never disagree.
    CHECK(std::string(series_detail_state_message(SeriesDetailState::NotConfigured))
          == "TV library not set up on this box");
    CHECK(std::string(series_detail_state_message(SeriesDetailState::SonarrUnreachable))
          == "Sonarr service offline");
    CHECK(series_detail_state_message(SeriesDetailState::NotInLibrary) == nullptr);
    CHECK(series_detail_state_message(SeriesDetailState::InLibrary) == nullptr);
}
```

- [ ] **Step 2: Register the test file and verify it fails to compile**

In `CMakeLists.txt`, inside `MEDIA_BROWSER_TEST_SOURCES`, add after the line `tests/media_browser/test_browse_logic.cpp`:

```cmake
        tests/media_browser/test_series_detail_logic.cpp
```

Re-run the configure line from Global Constraints, then the build. Expected: FAIL — `series_detail_logic.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `src/media_browser/ui/series_detail_logic.h` exactly:

```cpp
#pragma once

// Pure decision helpers for the TV series detail screen (spec:
// 2026-07-31-marquee-personalization-and-tv-design.md, Phase 2 "Series
// detail screen" + "Download granularity" + "Disk safety"). Header-only
// and Renderer-free so test_media_browser_unit can assert on them — same
// rationale as browse_logic.h / mb_ui_utils / library_view.
//
// The one structural rule everything here serves: the season list ALWAYS
// builds from TMDB's seasons as the base, with Sonarr statistics overlaid
// when present. AddSeriesResult with settled==false carries an EMPTY
// seasons vector by contract (sonarr_client.h) — a UI that keyed its rows
// on Sonarr seasons would render "0 seasons" for every unsettled add.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/tmdb_client.h"

namespace media_browser::ui {

// ---------- Season rows ----------

enum class SeasonState { None, Downloading, Partial, Complete };

struct SeasonRow {
    int season_number = 0;
    int episode_count = 0;       // Sonarr statistics when overlaid, else TMDB
    int episode_file_count = 0;  // Sonarr only; 0 pre-add
    bool monitored = false;      // Sonarr only; false pre-add
    SeasonState state = SeasonState::None;
};

// Downloading wins over everything (live queue activity is the freshest
// signal). Complete requires a KNOWN episode count — files present against
// an unknown total is Partial, never Complete.
inline SeasonState decide_season_state(int episode_count, int episode_file_count,
                                       bool downloading) {
    if (downloading) return SeasonState::Downloading;
    if (episode_count > 0 && episode_file_count >= episode_count)
        return SeasonState::Complete;
    if (episode_file_count > 0) return SeasonState::Partial;
    return SeasonState::None;
}

// TMDB seasons are the base; a Sonarr Series (nullable — pre-add, or a
// failed resolve) overlays monitored + statistics by season_number, and
// contributes seasons TMDB lacks. Season 0 (Specials) is excluded from
// BOTH sources: it is not part of "whole series" intent and would skew
// the disk estimate. Output is sorted by season_number.
inline std::vector<SeasonRow> merge_season_rows(
        const std::vector<TmdbTvSeason>& tmdb_seasons,
        const Series* sonarr,
        const std::unordered_set<int>& downloading_seasons) {
    std::vector<SeasonRow> rows;
    rows.reserve(tmdb_seasons.size());
    for (const auto& ts : tmdb_seasons) {
        if (ts.season_number == 0) continue;
        SeasonRow r;
        r.season_number = ts.season_number;
        r.episode_count = ts.episode_count;
        rows.push_back(r);
    }
    if (sonarr != nullptr) {
        for (const auto& ss : sonarr->seasons) {
            if (ss.season_number == 0) continue;
            auto it = std::find_if(rows.begin(), rows.end(),
                                   [&ss](const SeasonRow& r) {
                                       return r.season_number == ss.season_number;
                                   });
            if (it == rows.end()) {
                SeasonRow r;
                r.season_number = ss.season_number;
                rows.push_back(r);
                it = rows.end() - 1;
            }
            it->monitored = ss.monitored;
            it->episode_file_count = ss.episode_file_count;
            if (ss.episode_count > 0) it->episode_count = ss.episode_count;
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const SeasonRow& a, const SeasonRow& b) {
                  return a.season_number < b.season_number;
              });
    for (auto& r : rows) {
        r.state = decide_season_state(
            r.episode_count, r.episode_file_count,
            downloading_seasons.count(r.season_number) > 0);
    }
    return rows;
}

// "Download next season" target: the first unmonitored season in ascending
// order (specials never appear in rows). nullopt = everything monitored,
// the action hides.
inline std::optional<int> next_unmonitored_season(const std::vector<SeasonRow>& rows) {
    for (const auto& r : rows) {
        if (!r.monitored) return r.season_number;
    }
    return std::nullopt;
}

// ---------- Disk safety ----------

// Estimate covers episodes NOT yet on disk: sum(max(0, count - files)) ×
// per-episode runtime × preferred MB/min. Defensive fallbacks are load-
// bearing: runtime<=0 or rate<=0 would make the estimate 0 and silently
// ALLOW a whole-series add straight past the blocking preflight — 45 min
// and 70 MB/min are the fixture-typical values.
inline int64_t estimate_remaining_bytes(const std::vector<SeasonRow>& rows,
                                        int runtime_minutes, double mb_per_min) {
    const int rt = runtime_minutes > 0 ? runtime_minutes : 45;
    const double rate = mb_per_min > 0.0 ? mb_per_min : 70.0;
    int64_t missing = 0;
    for (const auto& r : rows) {
        missing += std::max(0, r.episode_count - r.episode_file_count);
    }
    return static_cast<int64_t>(
        std::llround(static_cast<double>(missing) * rt * rate * 1024.0 * 1024.0));
}

// Highest preferred rate in the 1080p family (what season packs actually
// land as under the retuned profiles), falling back to 720p, then to the
// fixture's 1080p preferred (70). Rows with preferred<=0 (Sonarr encodes
// "unlimited" as null) are skipped. Values come from Sonarr's live
// quality definitions — NOT hardcoded, because the operator retuned them
// once already (25/40 → 40/70, 2026-07-26).
inline double pick_preferred_mb_per_min(const std::vector<QualityDefinition>& defs) {
    double best_1080 = 0.0, best_720 = 0.0;
    for (const auto& d : defs) {
        if (d.preferred_mb_per_min <= 0.0) continue;
        if (d.title.find("1080p") != std::string::npos)
            best_1080 = std::max(best_1080, d.preferred_mb_per_min);
        else if (d.title.find("720p") != std::string::npos)
            best_720 = std::max(best_720, d.preferred_mb_per_min);
    }
    if (best_1080 > 0.0) return best_1080;
    if (best_720 > 0.0) return best_720;
    return 70.0;
}

// The codebase's FIRST blocking preflight (spec, Phase 2 "Disk safety"):
// whole-series adds Block when the estimate exceeds free space minus a
// 20 GiB floor. A missing/zero free-space reading fails OPEN to WarnOnly
// — matching the movie flow's philosophy (warn, never wedge).
enum class DiskVerdict { Allow, Block, WarnOnly };

inline constexpr int64_t kDiskFloorBytes = 20LL * 1024 * 1024 * 1024;

inline DiskVerdict whole_series_verdict(int64_t estimate_bytes,
                                        std::optional<int64_t> free_bytes) {
    if (!free_bytes.has_value() || *free_bytes <= 0) return DiskVerdict::WarnOnly;
    if (estimate_bytes > *free_bytes - kDiskFloorBytes) return DiskVerdict::Block;
    return DiskVerdict::Allow;
}

// ---------- Screen state ----------

enum class SeriesDetailState {
    Loading,            // either fetch still in flight
    TmdbError,          // TMDB detail failed — nothing to render
    NotConfigured,      // no SONARR_API_KEY: read-only page, no actions
    SonarrUnreachable,  // configured but not answering: read-only + banner
    NotInLibrary,       // actions: Add Season 1 / Whole series…
    InLibrary,          // actions: Download next season / Whole series… / Remove
};

struct SeriesDetailInputs {
    bool tmdb_done = false;
    bool tmdb_ok = false;
    bool sonarr_configured = false;
    bool sonarr_done = false;
    bool sonarr_ok = false;
    bool in_library = false;
};

// Precedence: TMDB first (without it there is no page at all), then the
// configured gate (an unconfigured box must read as "not set up", never
// as an outage — see 63f9046's rationale), then Sonarr's answer.
inline SeriesDetailState decide_series_detail_state(const SeriesDetailInputs& in) {
    if (!in.tmdb_done) return SeriesDetailState::Loading;
    if (!in.tmdb_ok) return SeriesDetailState::TmdbError;
    if (!in.sonarr_configured) return SeriesDetailState::NotConfigured;
    if (!in.sonarr_done) return SeriesDetailState::Loading;
    if (!in.sonarr_ok) return SeriesDetailState::SonarrUnreachable;
    return in.in_library ? SeriesDetailState::InLibrary
                         : SeriesDetailState::NotInLibrary;
}

// Copy for the non-interactive states; nullptr = render the page body.
// NotConfigured is the SAME literal BrowseScreen ships so the two screens
// never disagree about the same box. No default: — -Wswitch catches a new
// enumerator here.
inline const char* series_detail_state_message(SeriesDetailState s) {
    switch (s) {
        case SeriesDetailState::Loading:           return "Loading...";
        case SeriesDetailState::TmdbError:
            return "Couldn't load series info \xE2\x80\x94 check network";
        case SeriesDetailState::NotConfigured:
            return "TV library not set up on this box";
        case SeriesDetailState::SonarrUnreachable: return "Sonarr service offline";
        case SeriesDetailState::NotInLibrary:      return nullptr;
        case SeriesDetailState::InLibrary:         return nullptr;
    }
    return nullptr;  // unreachable; keeps -Wreturn-type quiet without a default:
}

}  // namespace media_browser::ui
```

**Step 3 note (build order):** this header references `QualityDefinition`, which does not exist until Task 2. To keep THIS task green standalone, add the struct to `src/media_browser/sonarr/sonarr_types.h` NOW, in this task, placed after the `SonarrQueueItem` struct with this exact text (Task 2 then only adds the client method + parser, not the type):

```cpp
// One row of GET /api/v3/qualitydefinition — the source of the TV disk
// estimate's MB/min multiplier. Sizes are megabytes-per-minute doubles in
// Sonarr's API (minSize/maxSize/preferredSize); preferred may be null
// upstream ("unlimited"), which parses to 0 and is skipped by consumers.
struct QualityDefinition {
    int quality_id = 0;             // quality.id
    std::string title;              // quality.name, e.g. "HDTV-1080p"
    double preferred_mb_per_min = 0.0;
    double max_mb_per_min = 0.0;
};
```

- [ ] **Step 4: Run the suite to verify it passes**

Run the Mac test loop. Expected: all green, **+8 cases from this task's TEST_CASE blocks** (assertion count grows accordingly), baseline count otherwise unchanged.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_logic.h \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_types.h \
        magic_dingus_box_cpp/tests/media_browser/test_series_detail_logic.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): series_detail_logic — pure season/estimate/state core

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---
### Task 2: SonarrClient::get_quality_definitions

**Files:**
- Modify: `src/media_browser/sonarr/sonarr_parsers.h` (declaration), `src/media_browser/sonarr/sonarr_parsers.cpp` (parser)
- Modify: `src/media_browser/sonarr/sonarr_client.h` / `.cpp` (virtual + fetch)
- Modify: `src/media_browser/sonarr/sonarr_mock.h` / `.cpp` (override — the mock overrides EVERY public virtual by contract, sonarr_mock.h:11-14)
- Test: `tests/media_browser/test_sonarr_parsers.cpp` (append), `tests/media_browser/test_sonarr_client.cpp` (append)

**Interfaces:**
- Consumes: `QualityDefinition` (added to `sonarr_types.h` by Task 1).
- Produces: `virtual std::vector<QualityDefinition> get_quality_definitions();` on `SonarrClient` (empty vector = fetch/parse failed — consumers fall back to the 70 default via `pick_preferred_mb_per_min`); `std::vector<QualityDefinition> parse_quality_definitions(const std::string& json);` in `sonarr_parsers.h`.

- [ ] **Step 1: Write the failing parser tests**

Append to `tests/media_browser/test_sonarr_parsers.cpp`:

```cpp
TEST_CASE("parse_quality_definitions: fixture-shaped rows, null preferred tolerated",
          "[sonarr][parsers]") {
    // Shape per GET /api/v3/qualitydefinition (Sonarr 4): sizes are
    // MB/min doubles; preferredSize may be null ("unlimited").
    const std::string body = R"([
        {"quality": {"id": 4, "name": "HDTV-720p"},
         "minSize": 17.1, "maxSize": 60.0, "preferredSize": 40.0},
        {"quality": {"id": 9, "name": "HDTV-1080p"},
         "minSize": 33.3, "maxSize": 100.0, "preferredSize": 70.0},
        {"quality": {"id": 3, "name": "Bluray-1080p"},
         "minSize": 50.4, "maxSize": 100.0, "preferredSize": null}
    ])";
    auto defs = media_browser::parse_quality_definitions(body);
    REQUIRE(defs.size() == 3);
    CHECK(defs[0].quality_id == 4);
    CHECK(defs[0].title == "HDTV-720p");
    CHECK(defs[0].preferred_mb_per_min == 40.0);
    CHECK(defs[0].max_mb_per_min == 60.0);
    CHECK(defs[1].preferred_mb_per_min == 70.0);
    CHECK(defs[2].preferred_mb_per_min == 0.0);  // null → 0, skipped downstream
}

TEST_CASE("parse_quality_definitions: non-array and garbage bodies parse to empty",
          "[sonarr][parsers]") {
    CHECK(media_browser::parse_quality_definitions("").empty());
    CHECK(media_browser::parse_quality_definitions("not json").empty());
    CHECK(media_browser::parse_quality_definitions(R"({"error":"x"})").empty());
    // Rows missing the quality object are skipped, not fatal.
    CHECK(media_browser::parse_quality_definitions(R"([{"minSize": 1.0}])").empty());
}
```

- [ ] **Step 2: Run to verify failure**

Mac test loop. Expected: FAIL — `parse_quality_definitions` is not a member of `media_browser`.

- [ ] **Step 3: Implement parser + client + mock**

In `src/media_browser/sonarr/sonarr_parsers.h`, next to the other `parse_*` declarations, add:

```cpp
// GET /api/v3/qualitydefinition rows. Tolerant: non-array bodies and rows
// missing the quality object yield/skip empty — the consumer treats an
// empty vector as "use the fallback rate", never as an error state.
std::vector<QualityDefinition> parse_quality_definitions(const std::string& json);
```

In `src/media_browser/sonarr/sonarr_parsers.cpp`, following the established tolerant-parse idiom of its siblings (Json::Reader guarded, isArray checked):

```cpp
std::vector<QualityDefinition> parse_quality_definitions(const std::string& json) {
    std::vector<QualityDefinition> out;
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(json);
    if (!Json::parseFromStream(builder, stream, &root, &errs)) return out;
    if (!root.isArray()) return out;
    for (const auto& row : root) {
        if (!row.isObject() || !row.isMember("quality") ||
            !row["quality"].isObject()) {
            continue;
        }
        QualityDefinition d;
        d.quality_id = row["quality"].get("id", 0).asInt();
        d.title      = row["quality"].get("name", "").asString();
        // preferredSize/maxSize may be null ("unlimited") — asDouble() on
        // null throws in jsoncpp, so gate on isNumeric().
        if (row.isMember("preferredSize") && row["preferredSize"].isNumeric())
            d.preferred_mb_per_min = row["preferredSize"].asDouble();
        if (row.isMember("maxSize") && row["maxSize"].isNumeric())
            d.max_mb_per_min = row["maxSize"].asDouble();
        out.push_back(std::move(d));
    }
    return out;
}
```

(If the file's siblings use a different jsoncpp entry — e.g. a shared `parse_json_body` helper — match THAT idiom instead; the behavior contract is the tests.)

In `src/media_browser/sonarr/sonarr_client.h`, after `virtual std::vector<RootFolder> get_root_folders();`:

```cpp
    // Quality definitions — the MB/min table behind the TV disk estimate.
    // Empty on any failure; pick_preferred_mb_per_min falls back to 70.
    virtual std::vector<QualityDefinition> get_quality_definitions();
```

In `src/media_browser/sonarr/sonarr_client.cpp`, mirroring `get_root_folders`' body shape (http_get → parse):

```cpp
std::vector<QualityDefinition> SonarrClient::get_quality_definitions() {
    const std::string body = http_get(config_.base_url + "/api/v3/qualitydefinition");
    if (body.empty()) return {};
    return parse_quality_definitions(body);
}
```

(Anchor on the real `http_get` call convention in that file — if it takes a path-only argument or returns an optional, match it exactly.)

In `src/media_browser/sonarr/sonarr_mock.h` add the override declaration beside the other overrides; in `sonarr_mock.cpp`:

```cpp
std::vector<QualityDefinition> SonarrMockClient::get_quality_definitions() {
    // Fixture-shaped (scripts/data/sonarr_qualitydefinitions.json): enough
    // for the estimate math to be realistic in dev. Unlike the library,
    // this is CONFIG data — serving it fabricates nothing about the box.
    return {
        {4, "HDTV-720p", 40.0, 60.0},
        {9, "HDTV-1080p", 70.0, 100.0},
    };
}
```

- [ ] **Step 4: Add the mock-contract test**

Append to `tests/media_browser/test_sonarr_client.cpp` (near the existing mock cases):

```cpp
TEST_CASE("SonarrMockClient serves fixture-shaped quality definitions",
          "[sonarr][mock]") {
    mb::SonarrMockClient m;
    auto defs = m.get_quality_definitions();
    REQUIRE(defs.size() == 2);
    CHECK(media_browser::ui::pick_preferred_mb_per_min(defs) == 70.0);
}
```

Add `#include "media_browser/ui/series_detail_logic.h"` to that file's includes if not present.

- [ ] **Step 5: Run the suite**

Mac test loop. Expected: green, **+3 cases from this task's TEST_CASE blocks**.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/sonarr/ \
        magic_dingus_box_cpp/tests/media_browser/test_sonarr_parsers.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_sonarr_client.cpp
git commit -m "feat(mb): SonarrClient::get_quality_definitions for the TV disk estimate

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---
### Task 3: SeriesDetailScreen — read-only core

The screen exists, loads, and renders; nothing routes to it yet (Task 4) and it offers no actions yet (Tasks 5–7). It must compile into the kiosk binary at the end of this task.

**Files:**
- Create: `src/media_browser/ui/series_detail_screen.h`
- Create: `src/media_browser/ui/series_detail_screen.cpp`
- Modify: `CMakeLists.txt` (`series_detail_screen.cpp` into `KIOSK_MEDIA_BROWSER_SOURCES`, after the line `src/media_browser/ui/detail_screen.cpp`)

**Interfaces:**
- Consumes: everything Task 1 produced; `TmdbClient::get_tv_detail(int)`; `SonarrClient::get_library_checked()` (reachability-honest: `nullopt` = did not answer — the mock is ALREADY honest here, commit `63f9046`); `SonarrClient::get_quality_definitions()` (Task 2); chrome helpers (`draw_screen_header`, `draw_poster_card`, `draw_footer_hints`, `marquee_tabs`); `::ui::Toast`.
- Produces (Tasks 4–8 rely on): class `SeriesDetailScreen` with ctor `SeriesDetailScreen(SonarrClient& sonarr, TmdbClient& tmdb, QbittorrentClient* qbit, bool sonarr_configured)`; `void set_tmdb_id(int)`; `int tmdb_id() const`; `void set_origin(Screen)`; `Screen origin() const`; the four `MbScreen` overrides; private `void fetch()`, `void apply_pending()`, members named exactly as in the header below.

**Design notes the code below embodies (do not "simplify" them away):**
- **In-library detection is `get_library_checked()` + scan by `tmdb_id`** — NOT `lookup_by_tmdb`→`find_series_by_tvdb`. One call, no TMDB→TVDB hop, transport failure is distinguishable (`nullopt`), and the mock is already honest on it. `find_series_by_tvdb` keeps zero kiosk callers and remains a documented trap.
- Thread idiom is DetailScreen's `FetchWorker` + `DoneFlag` + generation counter, verbatim in shape. The dtor bumps the gen then joins everything.
- render() draws the transient banner region LAST on every path (2c-1 discipline). The two body-less states (Loading, TmdbError) draw a centered message; NotConfigured and SonarrUnreachable draw the FULL read-only body plus a warning line — the TMDB half of the page is real regardless of Sonarr.

- [ ] **Step 1: Write the header**

Create `src/media_browser/ui/series_detail_screen.h` exactly:

```cpp
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/series_detail_logic.h"

namespace media_browser {
class SonarrClient;
class QbittorrentClient;
}

namespace media_browser::ui {

// TV series detail (Phase 2c-2): poster/overview + a season list with
// per-season state, and (Tasks 5-7) the season-at-a-time add flow, the
// whole-series blocking preflight, and orphan-proof remove.
//
// Deliberately Radarr-free: everything mutating is SonarrClient-shaped,
// the mirror image of DetailScreen being Radarr-shaped — the two screens
// share chrome helpers and idioms, never clients. All decisions live in
// series_detail_logic.h (pure, Mac-tested); this class is transport +
// paint.
class SeriesDetailScreen : public MbScreen {
public:
    SeriesDetailScreen(SonarrClient& sonarr, TmdbClient& tmdb,
                       QbittorrentClient* qbit, bool sonarr_configured);
    ~SeriesDetailScreen();

    // Same contract as DetailScreen::set_tmdb_id: no-op on the same id
    // (preserves loaded state on back-and-forth), refetch on a new one.
    void set_tmdb_id(int tmdb_id);
    int tmdb_id() const { return tmdb_id_; }

    // Where BTN4 returns to. Set by the dispatcher at transition time.
    void set_origin(Screen origin) { origin_ = origin; }
    Screen origin() const { return origin_; }

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    // ---- load pipeline (DetailScreen's FetchWorker idiom) ----
    struct FetchWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    struct PendingLoad {
        // TMDB half
        bool tmdb_done = false;
        bool tmdb_ok = false;
        std::optional<TmdbTvDetail> detail;
        // Sonarr half
        bool sonarr_done = false;
        bool sonarr_ok = false;
        bool in_library = false;
        std::optional<Series> series;
        std::vector<QualityDefinition> quality_defs;
    };

    void fetch();                       // spawns both workers under gen
    void run_tmdb_fetch(uint64_t gen, int tmdb_id,
                        std::shared_ptr<std::atomic<bool>> done);
    void run_sonarr_fetch(uint64_t gen, int tmdb_id,
                          std::shared_ptr<std::atomic<bool>> done);
    void reap_finished_workers();
    void apply_pending();               // render-thread drain
    void rebuild_rows();                // rows_ = merge_season_rows(...)

    SonarrClient& sonarr_;
    TmdbClient& tmdb_;
    QbittorrentClient* qbit_;           // Task 7 (remove); may be null
    const bool sonarr_configured_;

    int tmdb_id_ = 0;
    bool needs_refresh_ = true;
    Screen origin_ = Screen::Browse;

    // Authoritative render-thread state (only apply_pending writes these).
    std::optional<TmdbTvDetail> detail_;
    std::optional<Series> series_;
    std::vector<SeasonRow> rows_;
    std::unordered_set<int> downloading_seasons_;  // fed by Task 8
    double mb_per_min_ = 70.0;
    bool tmdb_done_ = false, tmdb_ok_ = false;
    bool sonarr_done_ = false, sonarr_ok_ = false, in_library_ = false;

    // Season-list viewport: rows beyond the fit render as "+N more".
    int visible_season_rows_ = 0;       // computed in render()

    std::atomic<uint64_t> fetch_gen_{0};
    std::mutex pending_mtx_;
    PendingLoad pending_;
    std::atomic<bool> pending_ready_{false};
    std::vector<FetchWorker> workers_;
};

}  // namespace media_browser::ui
```

- [ ] **Step 2: Write the implementation**

Create `src/media_browser/ui/series_detail_screen.cpp` exactly:

```cpp
#include "media_browser/ui/series_detail_screen.h"

#include <algorithm>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "platform/input_manager.h"
#include "spdlog/spdlog.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

namespace media_browser::ui {

namespace {
// Sets the done flag on every exit path of a worker (DetailScreen idiom).
struct DoneFlag {
    std::shared_ptr<std::atomic<bool>> flag;
    ~DoneFlag() {
        if (flag) flag->store(true, std::memory_order_release);
    }
};
constexpr int kBodyFontPx = 16;
constexpr int kRowFontPx = 18;
constexpr int kRowH = 34;
}  // namespace

SeriesDetailScreen::SeriesDetailScreen(SonarrClient& sonarr, TmdbClient& tmdb,
                                       QbittorrentClient* qbit,
                                       bool sonarr_configured)
    : sonarr_(sonarr), tmdb_(tmdb), qbit_(qbit),
      sonarr_configured_(sonarr_configured) {}

SeriesDetailScreen::~SeriesDetailScreen() {
    // Invalidate every in-flight worker's publish, then join.
    fetch_gen_.fetch_add(1);
    for (auto& w : workers_) {
        if (w.thread.joinable()) w.thread.join();
    }
}

void SeriesDetailScreen::set_tmdb_id(int tmdb_id) {
    if (tmdb_id == tmdb_id_) return;
    tmdb_id_ = tmdb_id;
    needs_refresh_ = true;
}

void SeriesDetailScreen::enter() {
    if (needs_refresh_) {
        needs_refresh_ = false;
        fetch();
    }
}

void SeriesDetailScreen::reap_finished_workers() {
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->done->load(std::memory_order_acquire) && it->thread.joinable()) {
            it->thread.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void SeriesDetailScreen::fetch() {
    reap_finished_workers();
    // Reset render-thread state to a clean Loading page.
    detail_.reset();
    series_.reset();
    rows_.clear();
    downloading_seasons_.clear();
    tmdb_done_ = tmdb_ok_ = false;
    sonarr_done_ = sonarr_ok_ = in_library_ = false;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_ = PendingLoad{};
        pending_ready_.store(false, std::memory_order_release);
    }
    const uint64_t gen = fetch_gen_.fetch_add(1) + 1;
    const int id = tmdb_id_;
    if (id <= 0) {
        tmdb_done_ = true;  // resolver -> TmdbError; nothing to fetch
        sonarr_done_ = true;
        return;
    }
    // Two independent workers; each publishes into pending_ under the
    // mutex and flips pending_ready_. An unconfigured box spawns only
    // the TMDB half — the Sonarr flags stay at their "never asked"
    // defaults and the resolver routes to NotConfigured.
    try {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread t(&SeriesDetailScreen::run_tmdb_fetch, this, gen, id, done);
        workers_.push_back(FetchWorker{std::move(t), std::move(done)});
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] tmdb worker spawn failed: {}", e.what());
        tmdb_done_ = true;  // TmdbError; user can back out and retry
    }
    if (sonarr_configured_) {
        try {
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::thread t(&SeriesDetailScreen::run_sonarr_fetch, this, gen, id,
                          done);
            workers_.push_back(FetchWorker{std::move(t), std::move(done)});
        } catch (const std::system_error& e) {
            spdlog::warn("[SeriesDetail] sonarr worker spawn failed: {}",
                         e.what());
            sonarr_done_ = true;  // SonarrUnreachable; page stays read-only
        }
    } else {
        sonarr_done_ = true;  // resolver: NotConfigured before this is read
    }
}

void SeriesDetailScreen::run_tmdb_fetch(uint64_t gen, int tmdb_id,
                                        std::shared_ptr<std::atomic<bool>> done) {
    DoneFlag df{done};
    auto detail = tmdb_.get_tv_detail(tmdb_id);
    if (gen != fetch_gen_.load()) return;  // preempted — discard
    std::lock_guard<std::mutex> lk(pending_mtx_);
    pending_.tmdb_done = true;
    pending_.tmdb_ok = detail.has_value();
    pending_.detail = std::move(detail);
    pending_ready_.store(true, std::memory_order_release);
}

void SeriesDetailScreen::run_sonarr_fetch(uint64_t gen, int tmdb_id,
                                          std::shared_ptr<std::atomic<bool>> done) {
    DoneFlag df{done};
    // In-library detection: the reachability-honest checked variant plus a
    // tmdb_id scan. NOT lookup_by_tmdb -> find_series_by_tvdb — that pair
    // cannot distinguish "not mapped" from "not answering", costs an extra
    // round-trip, and find_series_by_tvdb's mock is a documented trap with
    // deliberately zero kiosk callers.
    auto lib = sonarr_.get_library_checked();
    std::optional<Series> match;
    if (lib.has_value()) {
        for (const auto& s : *lib) {
            if (s.tmdb_id == tmdb_id) {
                match = s;
                break;
            }
        }
    }
    // Quality definitions ride along on the same worker: one Sonarr
    // round-trip's latency, and the estimate needs them before any add
    // affordance renders. Empty on failure -> pick_preferred falls back.
    std::vector<QualityDefinition> defs;
    if (lib.has_value()) defs = sonarr_.get_quality_definitions();
    if (gen != fetch_gen_.load()) return;  // preempted — discard
    std::lock_guard<std::mutex> lk(pending_mtx_);
    pending_.sonarr_done = true;
    pending_.sonarr_ok = lib.has_value();
    pending_.in_library = match.has_value();
    pending_.series = std::move(match);
    pending_.quality_defs = std::move(defs);
    pending_ready_.store(true, std::memory_order_release);
}

void SeriesDetailScreen::apply_pending() {
    if (!pending_ready_.load(std::memory_order_acquire)) return;
    PendingLoad p;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        p = pending_;
        pending_ready_.store(false, std::memory_order_release);
    }
    if (p.tmdb_done) {
        tmdb_done_ = true;
        tmdb_ok_ = p.tmdb_ok;
        if (p.detail.has_value()) detail_ = std::move(p.detail);
    }
    if (p.sonarr_done) {
        sonarr_done_ = true;
        sonarr_ok_ = p.sonarr_ok;
        in_library_ = p.in_library;
        if (p.series.has_value()) series_ = std::move(p.series);
        if (!p.quality_defs.empty())
            mb_per_min_ = pick_preferred_mb_per_min(p.quality_defs);
    }
    rebuild_rows();
}

void SeriesDetailScreen::rebuild_rows() {
    if (!detail_.has_value()) {
        rows_.clear();
        return;
    }
    rows_ = merge_season_rows(detail_->seasons,
                              series_.has_value() ? &*series_ : nullptr,
                              downloading_seasons_);
}

Screen SeriesDetailScreen::handle_input(
        const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        if (!e.pressed) continue;
        // BTN4 = back to wherever we came from. Actions land in Tasks 5-7.
        if (e.action == platform::InputAction::SETTINGS_MENU) {
            return origin_;
        }
    }
    return Screen::SeriesDetail;
}

void SeriesDetailScreen::update() { apply_pending(); }

void SeriesDetailScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const auto& th = r.mb_theme();
    r.mb_fill_rect(0, 0, static_cast<float>(screen_w),
                   static_cast<float>(screen_h), th.bg);

    const std::string title =
        detail_.has_value() ? detail_->title : std::string("Series");
    const int content_top = chrome::draw_screen_header(
        r, screen_w, title, chrome::marquee_tabs(""), /*focused_tab=*/-1);

    const SeriesDetailInputs in{tmdb_done_, tmdb_ok_, sonarr_configured_,
                                sonarr_done_, sonarr_ok_, in_library_};
    const SeriesDetailState st = decide_series_detail_state(in);
    const char* msg = series_detail_state_message(st);

    const bool body_less =
        (st == SeriesDetailState::Loading || st == SeriesDetailState::TmdbError);
    if (body_less) {
        // Centered message; falls THROUGH to footer + banner below.
        if (msg != nullptr) {
            const std::string m(msg);
            const int tw = r.mb_text_width(m, kRowFontPx);
            r.mb_draw_text(m, static_cast<float>((screen_w - tw) / 2),
                           static_cast<float>(screen_h / 2), kRowFontPx,
                           st == SeriesDetailState::TmdbError ? th.highlight2
                                                              : th.dim);
        }
    } else {
        // ---- read-only body: poster left, overview right, seasons below ----
        const int body_x = chrome::kSafeInset_px;
        int y = content_top + chrome::kPad3;
        // NotConfigured / SonarrUnreachable warning line above the body.
        if (msg != nullptr) {
            r.mb_draw_text(msg, static_cast<float>(body_x),
                           static_cast<float>(y + 14), 14, th.highlight2);
            y += 22;
        }
        const int poster_w = 160, poster_h = 240;
        chrome::draw_poster_card(r, body_x, y, poster_w, poster_h,
                                 detail_->title, detail_->year,
                                 th.dim, in_library_, /*download_pct=*/-1,
                                 detail_->poster_path);
        const int text_x = body_x + poster_w + chrome::kPad4;
        const float text_w = static_cast<float>(screen_w - text_x -
                                                chrome::kSafeInset_px);
        // Meta line: year · seasons · episodes · status.
        {
            std::string meta = std::to_string(detail_->year);
            meta += " \xC2\xB7 " + std::to_string(detail_->number_of_seasons) +
                    " season" + (detail_->number_of_seasons == 1 ? "" : "s");
            meta += " \xC2\xB7 " + std::to_string(detail_->number_of_episodes) +
                    " episodes";
            if (!detail_->status.empty()) meta += " \xC2\xB7 " + detail_->status;
            r.mb_draw_text(meta, static_cast<float>(text_x),
                           static_cast<float>(y + 16), kBodyFontPx, th.dim);
        }
        // Overview: wrapped, max 5 lines (mb_ui_utils wrap helper).
        {
            const auto lines = wrap_text_to_width(
                detail_->overview, kBodyFontPx, text_w,
                [&r](const std::string& s, int px) {
                    return static_cast<float>(r.mb_text_width(s, px));
                });
            int line_y = y + 44;
            int shown = 0;
            for (const auto& ln : lines) {
                if (shown++ == 5) break;
                r.mb_draw_text(ln, static_cast<float>(text_x),
                               static_cast<float>(line_y), kBodyFontPx, th.fg);
                line_y += kBodyFontPx + 6;
            }
        }
        // ---- season list ----
        int list_y = y + poster_h + chrome::kPad3;
        const int list_bottom = screen_h - chrome::kFooterHeight_px -
                                chrome::kPad3;
        visible_season_rows_ =
            std::max(0, (list_bottom - list_y) / kRowH);
        const int shown_rows = std::min<int>(
            static_cast<int>(rows_.size()), visible_season_rows_);
        for (int i = 0; i < shown_rows; ++i) {
            const auto& row = rows_[static_cast<size_t>(i)];
            std::string label = "Season " + std::to_string(row.season_number);
            std::string counts =
                std::to_string(row.episode_file_count) + "/" +
                std::to_string(row.episode_count) + " eps";
            const char* state_txt = nullptr;
            ::ui::Color state_col = th.dim;
            switch (row.state) {
                case SeasonState::None:
                    state_txt = row.monitored ? "monitored" : "\xE2\x80\x94";
                    break;
                case SeasonState::Downloading:
                    state_txt = "downloading";
                    state_col = th.highlight2;
                    break;
                case SeasonState::Partial:
                    state_txt = "partial";
                    state_col = th.accent;
                    break;
                case SeasonState::Complete:
                    state_txt = "complete";
                    state_col = th.highlight1;
                    break;
            }
            r.mb_draw_text(label, static_cast<float>(body_x),
                           static_cast<float>(list_y + 22), kRowFontPx, th.fg);
            r.mb_draw_text(counts, static_cast<float>(body_x + 220),
                           static_cast<float>(list_y + 22), kBodyFontPx,
                           th.dim);
            r.mb_draw_text(state_txt, static_cast<float>(body_x + 360),
                           static_cast<float>(list_y + 22), kBodyFontPx,
                           state_col);
            list_y += kRowH;
        }
        if (static_cast<int>(rows_.size()) > shown_rows) {
            const int hidden = static_cast<int>(rows_.size()) - shown_rows;
            r.mb_draw_text("\xE2\x80\xA6and " + std::to_string(hidden) +
                               " more season" + (hidden == 1 ? "" : "s"),
                           static_cast<float>(body_x),
                           static_cast<float>(list_y + 20), kBodyFontPx,
                           th.dim);
        }
    }

    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::Btn1Yellow, "\xE2\x80\x94"},
        {chrome::HintIcon::Btn2Red, "\xE2\x80\x94"},
        {chrome::HintIcon::Btn3Green, "\xE2\x80\x94"},
        {chrome::HintIcon::Btn4Black, "Back"},
        {chrome::HintIcon::RotaryNav, "\xE2\x80\x94"},
        {chrome::HintIcon::RotaryPress, "\xE2\x80\x94"},
    });

    // ALWAYS LAST on every path (2c-1 discipline) — the transient region.
    ::ui::Toast::render(r, screen_w, screen_h);
}

}  // namespace media_browser::ui
```

**Anchoring notes for the implementer:** `wrap_text_to_width` — verify the exact name and signature in `mb_ui_utils.h` before using; if the house helper differs (e.g. it returns a different container or takes the measure lambda differently), adapt the call site, not the helper. Same for `::ui::Toast::render` — DetailScreen's render() tail shows the house way to draw the transient region; match it. `Screen::SeriesDetail` does not exist until Task 4 — `handle_input` returning it will not compile yet, so UNTIL Task 4 return `origin_`'s type-mate `Screen::Browse` on the fall-through line with a `// Task 4 replaces with Screen::SeriesDetail` comment, OR (preferred) add the enum value in THIS task as part of Step 3 — see below.

- [ ] **Step 3: Add the enum value now so the fall-through compiles**

In `src/media_browser/ui/mb_screen.h`, in the `Screen` enum after the line `Detail,`:

```cpp
    SeriesDetail,    // TV series detail (Phase 2c-2). Radarr-free mirror
                     // of Detail; reached only from Browse in TV mode.
```

**This will NOT break main.cpp's dispatch switch yet** — the switch only needs a case when it can receive the value, and nothing returns `Screen::SeriesDetail` until Task 4 wires Browse. BUT the switch has no `default:`, so `-Wswitch` WILL fire on it the moment the enum grows. That is the desired forcing function — **this task therefore ALSO adds the dispatch case as a pure no-crash stub** in `src/main.cpp`, in the screen-transition switch after the `case media_browser::ui::Screen::Detail:` line:

```cpp
                    case media_browser::ui::Screen::SeriesDetail:
                        // Instance + forwarding land with the Browse routing
                        // (2c-2 Task 4); unreachable until then.
                        active_mb_screen = &mb_browse;
                        break;
```

- [ ] **Step 4: CMake + Mac suite**

Add to `KIOSK_MEDIA_BROWSER_SOURCES` after `src/media_browser/ui/detail_screen.cpp`:

```cmake
            src/media_browser/ui/series_detail_screen.cpp
```

Run the Mac test loop. Expected: green, **+0 cases** (screen files are not in the Mac target — this proves nothing about the screen; the Pi gate below is the real check).

- [ ] **Step 5: Pi kiosk compile-verify**

Per Global Constraints: rsync the worktree's `magic_dingus_box_cpp/` to `magic@magicpi5.local:mdb-2c2/src/`, configure `-DBUILD_KIOSK=ON -DBUILD_TESTS=OFF -DENABLE_MEDIA_BROWSER=ON`, build with the `setsid` launch line, poll the log. Expected: `Built target magic_dingus_box_cpp`, 0 errors, no warnings naming `series_detail_*` or `mb_screen.h` or `main.cpp` beyond the known pre-existing set.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h \
        magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): SeriesDetailScreen read-only core + Screen::SeriesDetail

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Registration + Browse routing

TV posters stop toasting "coming soon" and open the real screen.

**Files:**
- Modify: `src/main.cpp` (instance, real dispatch case, forwarding block)
- Modify: `src/media_browser/ui/browse_screen.h` (kind accessor), `src/media_browser/ui/browse_screen.cpp` (SELECT routing + footer hint)

**Interfaces:**
- Consumes: Task 3's class and `Screen::SeriesDetail`.
- Produces: `MediaKind BrowseScreen::selected_kind() const` — the dispatcher's routing signal, alongside the existing `selected_tmdb_id()`.

- [ ] **Step 1: Browse accessor + routing**

In `src/media_browser/ui/browse_screen.h`, directly below the line `int selected_tmdb_id() const { return selected_tmdb_id_; }`:

```cpp
    // Kind of the poster most recently selected — the dispatcher routes
    // Movie -> DetailScreen, Tv -> SeriesDetailScreen. Movie/TV TMDB id
    // spaces OVERLAP COMPLETELY (id 1396 is Breaking Bad AND an unrelated
    // film), so the id alone MUST NOT choose the screen.
    MediaKind selected_kind() const { return selected_kind_; }
```

and below the member `int selected_tmdb_id_ = 0;`:

```cpp
    MediaKind selected_kind_ = MediaKind::Movie;
```

In `src/media_browser/ui/browse_screen.cpp`, replace the 2c-1 TV SELECT guard — the block beginning `// 2c-1 ships discovery only. DetailScreen is Movie/Radarr-typed` through `return Screen::Detail;` inclusive — with:

```cpp
                // Movie and TV ids overlap completely, so the KIND — not
                // the id — picks the destination screen. The dispatcher
                // reads selected_kind() to forward to the right detail.
                selected_tmdb_id_ = hit.tmdb_id;
                selected_kind_ = hit.kind;
                return hit.kind == MediaKind::Tv ? Screen::SeriesDetail
                                                 : Screen::Detail;
```

And change the footer hint line `{chrome::HintIcon::RotaryPress, tv_mode() ? "Coming soon" : "Detail"}` to:

```cpp
        {chrome::HintIcon::RotaryPress, "Detail"},
```

- [ ] **Step 2: main.cpp instance + forwarding**

After the `mb_detail` construction line, add:

```cpp
    media_browser::ui::SeriesDetailScreen mb_series_detail(
        sonarr, *tmdb, qbit_owned.get(),
        /*sonarr_configured=*/!sonarr_key.empty());
```

(add `#include "media_browser/ui/series_detail_screen.h"` beside the other mb screen includes). Replace Task 3's stub dispatch case body with:

```cpp
                    case media_browser::ui::Screen::SeriesDetail:
                        active_mb_screen = &mb_series_detail;
                        break;
```

And next to the existing `if (next == media_browser::ui::Screen::Detail) {` forwarding block, add a sibling:

```cpp
                if (next == media_browser::ui::Screen::SeriesDetail) {
                    if (current_mb_screen == media_browser::ui::Screen::Browse) {
                        mb_series_detail.set_tmdb_id(mb_browse.selected_tmdb_id());
                        mb_series_detail.set_origin(current_mb_screen);
                    }
                }
```

Also fix Task 3's temporary fall-through in `SeriesDetailScreen::handle_input` if it returned `Screen::Browse`: the steady-state return is `Screen::SeriesDetail`.

- [ ] **Step 3: Mac suite + Pi compile + hardware smoke**

Mac loop (expect **+0 cases**, still green). Pi compile via `mdb-2c2` (incremental — main.cpp + browse_screen.cpp + the new screen). Expected: links clean, no new warnings.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/src/media_browser/ui/browse_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp
git commit -m "feat(mb): route TV posters to SeriesDetailScreen

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---
### Task 5: Add flows — "Add Season 1" and "Download next season"

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.h` / `.cpp`

**Interfaces:**
- Consumes: `SonarrClient::get_quality_profiles()`, `get_root_folders()`, `add_series(tmdb_id, qp_id, monitor, title_fallback)` (monitor=true ⇒ `addOptions.monitor="firstSeason"` — **verify that mapping in sonarr_client.cpp's add body and state it in your report**), `set_season_monitored`, `trigger_season_search`; Task 1's `next_unmonitored_season`, `estimate_remaining_bytes`.
- Produces (Tasks 6-7 extend these): `enum class Action { AddSeason1, NextSeason, WholeSeries, Remove, ConfirmRemove };`, `struct ActionButton { Action action; std::string label; };`, members `buttons_`, `focus_`, `rebuild_buttons()`, the mutation-worker members `mut_worker_`, `mut_in_flight_`, `mut_done_`, `mut_ok_`, `mut_error_`, `mut_toast_`, and `spawn_mutation(std::function<void()>)`.

- [ ] **Step 1: Header additions**

In `series_detail_screen.h`, after the `origin()` accessor add the action machinery (private section, after `rebuild_rows();`):

```cpp
    // ---- action row (Tasks 5-7) ----
    enum class Action { AddSeason1, NextSeason, WholeSeries, Remove, ConfirmRemove };
    struct ActionButton {
        Action action;
        std::string label;
    };
    void rebuild_buttons();
    void dispatch_action(Action a);

    // ONE mutation at a time, on ONE reused worker thread (WatchdogSec=10:
    // add_series alone can take ~13.5 s — never on the render thread).
    // spawn_mutation joins the previous worker, wraps the body so
    // mut_done_ flips on every exit path, and catches std::system_error
    // from the thread ctor (a raw throw there is std::terminate).
    void spawn_mutation(std::function<void()> body);
    void drain_mutation();

    std::vector<ActionButton> buttons_;
    int focus_ = 0;

    std::thread mut_worker_;
    std::atomic<bool> mut_in_flight_{false};
    std::atomic<bool> mut_done_{false};
    bool mut_ok_ = false;                // worker-written, drained under done flag
    std::string mut_toast_;              // message to show on drain
    std::optional<Series> mut_series_;   // fresh series state when the op returns one
    std::mutex mut_mtx_;
```

And in the dtor (`series_detail_screen.cpp`), before the workers_ loop, add:

```cpp
    if (mut_worker_.joinable()) mut_worker_.join();
```

- [ ] **Step 2: Implement the machinery**

Append to `series_detail_screen.cpp` (and call `rebuild_buttons()` at the end of `apply_pending()`; call `drain_mutation()` at the top of `update()`):

```cpp
void SeriesDetailScreen::rebuild_buttons() {
    buttons_.clear();
    const SeriesDetailInputs in{tmdb_done_, tmdb_ok_, sonarr_configured_,
                                sonarr_done_, sonarr_ok_, in_library_};
    const SeriesDetailState st = decide_series_detail_state(in);
    if (mut_in_flight_.load()) {
        buttons_.push_back({Action::AddSeason1, "Working\xE2\x80\xA6"});
        focus_ = 0;
        return;
    }
    if (st == SeriesDetailState::NotInLibrary) {
        buttons_.push_back({Action::AddSeason1, "Add Season 1"});
        buttons_.push_back({Action::WholeSeries, "Whole series\xE2\x80\xA6"});
    } else if (st == SeriesDetailState::InLibrary) {
        const auto next = next_unmonitored_season(rows_);
        if (next.has_value()) {
            buttons_.push_back({Action::NextSeason,
                                "Download Season " + std::to_string(*next)});
            buttons_.push_back({Action::WholeSeries, "Whole series\xE2\x80\xA6"});
        }
        buttons_.push_back({Action::Remove, "Remove"});
    }
    if (focus_ >= static_cast<int>(buttons_.size())) focus_ = 0;
}

void SeriesDetailScreen::spawn_mutation(std::function<void()> body) {
    if (mut_in_flight_.load()) return;   // one at a time
    if (mut_worker_.joinable()) mut_worker_.join();
    mut_in_flight_.store(true);
    mut_done_.store(false);
    try {
        mut_worker_ = std::thread([this, body = std::move(body)]() {
            body();
            mut_done_.store(true, std::memory_order_release);
        });
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] mutation spawn failed: {}", e.what());
        mut_in_flight_.store(false);
        ::ui::Toast::show("Couldn't start the operation \xE2\x80\x94 try again");
    }
    rebuild_buttons();   // show "Working…" immediately
}

void SeriesDetailScreen::drain_mutation() {
    if (!mut_done_.load(std::memory_order_acquire)) return;
    mut_done_.store(false);
    mut_in_flight_.store(false);
    std::string toast;
    std::optional<Series> fresh;
    {
        std::lock_guard<std::mutex> lk(mut_mtx_);
        toast = std::move(mut_toast_);
        mut_toast_.clear();
        fresh = std::move(mut_series_);
        mut_series_.reset();
    }
    if (fresh.has_value()) {
        series_ = std::move(fresh);
        in_library_ = true;
        sonarr_done_ = sonarr_ok_ = true;
        rebuild_rows();
    }
    if (!toast.empty()) ::ui::Toast::show(toast);
    rebuild_buttons();
}

void SeriesDetailScreen::dispatch_action(Action a) {
    switch (a) {
        case Action::AddSeason1: {
            const int id = tmdb_id_;
            const std::string title = detail_.has_value() ? detail_->title : "";
            spawn_mutation([this, id, title]() {
                // Quality profile: "Any" (the provisioned profile) by name,
                // else the first one — same policy as the movie add.
                int qp_id = 0;
                for (const auto& qp : sonarr_.get_quality_profiles()) {
                    if (qp_id == 0) qp_id = qp.id;
                    if (qp.name == "Any") { qp_id = qp.id; break; }
                }
                std::lock_guard<std::mutex> lk(mut_mtx_);
                if (qp_id == 0) {
                    mut_ok_ = false;
                    mut_toast_ = "Sonarr has no quality profile \xE2\x80\x94 not added";
                    return;
                }
                auto res = sonarr_.add_series(id, qp_id, /*monitor=*/true, title);
                mut_ok_ = res.ok;
                if (!res.ok) {
                    mut_toast_ = "Add failed \xE2\x80\x94 " + sonarr_.last_error();
                } else if (!res.settled) {
                    // settled==false: seasons[] is EMPTY by contract. The
                    // page keeps rendering TMDB rows; the Task-8 re-poll
                    // (or the next entry) picks up real statistics. Two
                    // meanings (poll timeout / never-refreshed record) —
                    // both read correctly as "added, syncing".
                    mut_series_ = res.series;
                    mut_toast_ = "Added \xE2\x80\x94 syncing seasons\xE2\x80\xA6";
                } else {
                    mut_series_ = res.series;
                    mut_toast_ = "Season 1 search started";
                }
            });
            break;
        }
        case Action::NextSeason: {
            if (!series_.has_value() || series_->sonarr_id <= 0) break;
            const auto next = next_unmonitored_season(rows_);
            if (!next.has_value()) break;
            const int sid = series_->sonarr_id;
            const int season = *next;
            spawn_mutation([this, sid, season]() {
                std::lock_guard<std::mutex> lk(mut_mtx_);
                if (!sonarr_.set_season_monitored(sid, season, true)) {
                    mut_ok_ = false;
                    mut_toast_ = "Couldn't monitor season " +
                                 std::to_string(season) + " \xE2\x80\x94 " +
                                 sonarr_.last_error();
                    return;
                }
                if (!sonarr_.trigger_season_search(sid, season)) {
                    mut_ok_ = false;
                    mut_toast_ = "Monitored, but the search didn't start \xE2\x80\x94 "
                                 "Sonarr will pick it up on RSS";
                    // fall through: refresh state anyway
                } else {
                    mut_ok_ = true;
                    mut_toast_ = "Season " + std::to_string(season) +
                                 " search started";
                }
                mut_series_ = sonarr_.get_series(sid);  // fresh monitored flags
            });
            break;
        }
        case Action::WholeSeries:
        case Action::Remove:
        case Action::ConfirmRemove:
            break;  // Tasks 6 and 7
    }
}
```

- [ ] **Step 3: Input + render wiring**

In `handle_input`, before the BTN4 branch:

```cpp
        if (e.action == platform::InputAction::ROTATE ||
            e.action == platform::InputAction::ROTATE_VERTICAL) {
            if (!buttons_.empty()) {
                focus_ = (focus_ + (e.value > 0 ? 1 : -1) +
                          static_cast<int>(buttons_.size())) %
                         static_cast<int>(buttons_.size());
            }
            continue;
        }
        if (e.action == platform::InputAction::SELECT) {
            if (!buttons_.empty() && !mut_in_flight_.load()) {
                dispatch_action(buttons_[static_cast<size_t>(focus_)].action);
            }
            continue;
        }
```

(Anchor `e.value` on how BrowseScreen reads rotary direction — if the house convention is a different field or separate PREV/NEXT actions, match it exactly.)

In `render()`, between the season list and `draw_footer_hints`, draw the action row (skip when `buttons_.empty()`):

```cpp
        int bx = body_x;
        const int brow_y = list_bottom - 44;
        for (size_t i = 0; i < buttons_.size(); ++i) {
            chrome::ButtonKind kind = chrome::ButtonKind::Ok;
            if (buttons_[i].action == Action::Remove ||
                buttons_[i].action == Action::ConfirmRemove) {
                kind = chrome::ButtonKind::Warn;
            } else if (buttons_[i].action == Action::WholeSeries) {
                kind = chrome::ButtonKind::Action;
            }
            const auto rect = chrome::draw_button(
                r, bx, brow_y, buttons_[i].label, kind,
                static_cast<int>(i) == focus_);
            bx = rect.x + rect.w + chrome::kPad3;
        }
```

Reserve the row's height by subtracting `52` from `list_bottom` when computing `visible_season_rows_` (buttons must never overlap the list). Update the footer hints: RotaryNav `"Choose"`, RotaryPress `"Select"`, Btn4 `"Back"` — dim the rotary pair when `buttons_.empty()`.

- [ ] **Step 4: Mac suite + Pi compile**

Mac loop: **+0 cases**, still green. Pi incremental build in `mdb-2c2`: links clean, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp
git commit -m "feat(mb): SeriesDetail add flows — Season 1 and next season

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: "Whole series…" — armed confirm + the blocking preflight

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.h` / `.cpp`

**Interfaces:**
- Consumes: Task 1's `estimate_remaining_bytes`, `whole_series_verdict`, `DiskVerdict`, `kDiskFloorBytes`; `SonarrClient::get_root_folders()` (RootFolder carries `free_space_bytes`); `std::filesystem::space` fallback on `/mnt/ssd/library/tv`.
- Produces: the armed-confirm members (`whole_armed_`, `whole_armed_at_`, `whole_estimate_bytes_`, `kWholeConfirmMs = 4000`).

**Why armed-button, not a modal:** the codebase's confirm idiom is a button-label swap with expiry (DetailScreen's Remove → "Confirm Remove", 2 s; QueueScreen's cancel, 2 s). The spec asks for "a confirm modal with disk estimate"; the estimate goes IN the armed label, which satisfies the requirement inside the established idiom — no screen in the codebase owns a modal, and 2c-1 documented why input-swallowing overlays are dangerous to bolt on. 4 s (not 2) because the user is reading a number.

- [ ] **Step 1: Header additions**

```cpp
    // Whole-series confirm: press 1 fetches free space off-thread and arms
    // (or Blocks); press 2 within kWholeConfirmMs executes.
    bool whole_armed_ = false;
    std::chrono::steady_clock::time_point whole_armed_at_{};
    int64_t whole_estimate_bytes_ = 0;
    static constexpr int kWholeConfirmMs = 4000;
```

- [ ] **Step 2: Implement**

Add `#include <filesystem>` to `series_detail_screen.cpp`'s includes. Replace `case Action::WholeSeries: ... break;` in `dispatch_action` with:

```cpp
        case Action::WholeSeries: {
            if (whole_armed_) {
                // ---- press 2: execute ----
                whole_armed_ = false;
                const bool pre_add = !in_library_;
                const int id = tmdb_id_;
                const std::string title =
                    detail_.has_value() ? detail_->title : "";
                const int sid =
                    series_.has_value() ? series_->sonarr_id : 0;
                std::vector<int> to_monitor;
                for (const auto& row : rows_) {
                    if (!row.monitored) to_monitor.push_back(row.season_number);
                }
                spawn_mutation([this, pre_add, id, title, sid, to_monitor]() {
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    int series_id = sid;
                    if (pre_add) {
                        int qp_id = 0;
                        for (const auto& qp : sonarr_.get_quality_profiles()) {
                            if (qp_id == 0) qp_id = qp.id;
                            if (qp.name == "Any") { qp_id = qp.id; break; }
                        }
                        if (qp_id == 0) {
                            mut_ok_ = false;
                            mut_toast_ =
                                "Sonarr has no quality profile \xE2\x80\x94 not added";
                            return;
                        }
                        auto res =
                            sonarr_.add_series(id, qp_id, /*monitor=*/true, title);
                        if (!res.ok) {
                            mut_ok_ = false;
                            mut_toast_ =
                                "Add failed \xE2\x80\x94 " + sonarr_.last_error();
                            return;
                        }
                        series_id = res.series.sonarr_id;
                        mut_series_ = res.series;
                    }
                    if (series_id <= 0) {
                        mut_ok_ = false;
                        mut_toast_ = "Series id unknown \xE2\x80\x94 try again "
                                     "once syncing finishes";
                        return;
                    }
                    int failed = 0;
                    for (int season : to_monitor) {
                        if (!sonarr_.set_season_monitored(series_id, season, true))
                            ++failed;
                    }
                    if (!sonarr_.trigger_series_search(series_id)) {
                        mut_toast_ = "Monitored, but the search didn't start "
                                     "\xE2\x80\x94 Sonarr will pick it up on RSS";
                    } else {
                        mut_toast_ = failed == 0
                            ? "Whole-series search started"
                            : "Search started (" + std::to_string(failed) +
                              " season(s) couldn't be monitored)";
                    }
                    mut_ok_ = (failed == 0);
                    mut_series_ = sonarr_.get_series(series_id);
                });
                break;
            }
            // ---- press 1: estimate + free space + verdict, off-thread ----
            const int runtime =
                series_.has_value() ? series_->runtime_minutes : 0;
            const int64_t estimate =
                estimate_remaining_bytes(rows_, runtime, mb_per_min_);
            spawn_mutation([this, estimate]() {
                // Free space: Sonarr's TV root folder first (the value the
                // import actually depends on), std::filesystem fallback,
                // and fail-OPEN to WarnOnly per the movie flow's philosophy.
                std::optional<int64_t> free_bytes;
                for (const auto& rf : sonarr_.get_root_folders()) {
                    if (rf.path.find("/tv") != std::string::npos &&
                        rf.free_space_bytes > 0) {
                        free_bytes = rf.free_space_bytes;
                        break;
                    }
                }
                if (!free_bytes.has_value()) {
                    std::error_code ec;
                    auto info =
                        std::filesystem::space("/mnt/ssd/library/tv", ec);
                    if (!ec && info.available > 0)
                        free_bytes = static_cast<int64_t>(info.available);
                }
                const DiskVerdict v = whole_series_verdict(estimate, free_bytes);
                const auto gb = [](int64_t b) {
                    return std::to_string(b / (1024 * 1024 * 1024)) + " GB";
                };
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_ok_ = true;
                switch (v) {
                    case DiskVerdict::Block:
                        // The codebase's FIRST blocking preflight: do not
                        // arm; show both numbers and the floor.
                        mut_toast_ = "Not enough space: needs ~" + gb(estimate) +
                                     ", " + gb(*free_bytes) +
                                     " free (20 GB floor)";
                        break;
                    case DiskVerdict::WarnOnly:
                        mut_toast_ = "Couldn't check free space \xE2\x80\x94 "
                                     "confirm to proceed anyway";
                        whole_estimate_bytes_ = estimate;
                        whole_armed_ = true;
                        whole_armed_at_ = std::chrono::steady_clock::now();
                        break;
                    case DiskVerdict::Allow:
                        whole_estimate_bytes_ = estimate;
                        whole_armed_ = true;
                        whole_armed_at_ = std::chrono::steady_clock::now();
                        break;
                }
            });
            break;
        }
```

In `rebuild_buttons()`, the WholeSeries label becomes armed-aware — replace both `buttons_.push_back({Action::WholeSeries, "Whole series\xE2\x80\xA6"});` lines with:

```cpp
        buttons_.push_back({Action::WholeSeries, whole_series_label()});
```

and add the helper + expiry:

```cpp
std::string SeriesDetailScreen::whole_series_label() const {
    if (!whole_armed_) return "Whole series\xE2\x80\xA6";
    return "Confirm ~" +
           std::to_string(whole_estimate_bytes_ / (1024 * 1024 * 1024)) +
           " GB";
}
```

(declare `std::string whole_series_label() const;` in the header). In `update()`, after `drain_mutation()`:

```cpp
    if (whole_armed_) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - whole_armed_at_)
                      .count();
        if (ms > kWholeConfirmMs) {
            whole_armed_ = false;
            rebuild_buttons();
        }
    }
```

The armed button renders `ButtonKind::Warn` — in the render loop's kind selection, add `|| (buttons_[i].action == Action::WholeSeries && whole_armed_)` to the Warn condition. Any OTHER action press while armed disarms first (`whole_armed_ = false;` at the top of `dispatch_action`, before the switch, except for the WholeSeries case itself — structure it as: `const bool was_armed = whole_armed_; if (a != Action::WholeSeries) whole_armed_ = false;`).

- [ ] **Step 3: Mac suite + Pi compile + commit**

Mac loop **+0 cases** green; Pi incremental clean. Then:

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp
git commit -m "feat(mb): whole-series add behind the blocking disk preflight

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Orphan-proof remove

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.h` / `.cpp`

**Interfaces:**
- Consumes: `SonarrClient::get_queue()`, `cancel_queue_item(queue_id)`, `get_series_download_hashes(sonarr_id)`, `remove_series(sonarr_id, delete_files)`; `QbittorrentClient`'s torrent-delete call — **anchor its exact name and signature on DetailScreen's `run_remove` Step 2 body and use precisely that**.
- Produces: `remove_pending_` + `remove_pending_at_` + `kRemovePendingMs = 2000` (DetailScreen's exact confirm idiom), and a return-to-origin on success.

- [ ] **Step 1: The confirm swap**

Header:

```cpp
    // Remove confirm: DetailScreen's exact idiom — label swap + 2s expiry.
    bool remove_pending_ = false;
    std::chrono::steady_clock::time_point remove_pending_at_{};
    static constexpr int kRemovePendingMs = 2000;
    bool remove_succeeded_ = false;   // worker-written under mut_mtx_
    bool navigate_back_ = false;      // drain-set; handle_input returns origin_
```

`rebuild_buttons()`: the Remove entry becomes

```cpp
        buttons_.push_back(remove_pending_
                               ? ActionButton{Action::ConfirmRemove, "Confirm Remove"}
                               : ActionButton{Action::Remove, "Remove"});
```

`update()` gains the expiry (same shape as the whole-series one, clearing `remove_pending_`). `dispatch_action`:

```cpp
        case Action::Remove:
            remove_pending_ = true;
            remove_pending_at_ = std::chrono::steady_clock::now();
            rebuild_buttons();
            break;
        case Action::ConfirmRemove: {
            remove_pending_ = false;
            if (!series_.has_value() || series_->sonarr_id <= 0) break;
            const int sid = series_->sonarr_id;
            spawn_mutation([this, sid]() {
                // Mirror of DetailScreen::run_remove, Sonarr-shaped:
                //  1. Cancel in-flight queue rows for this series
                //     (removeFromClient semantics live server-side).
                //  2. Purge every torrent Sonarr's history associates with
                //     the series — catches finished+seeding torrents step 1
                //     misses. Gated on qbit_ being wired.
                //  3. remove_series(delete_files=true).
                //  4. Back to Browse (drained on the render thread).
                std::lock_guard<std::mutex> lk(mut_mtx_);
                int cancel_failed = 0;
                for (const auto& q : sonarr_.get_queue()) {
                    if (q.series_id == sid && !sonarr_.cancel_queue_item(q.id))
                        ++cancel_failed;
                }
                if (cancel_failed > 0) {
                    mut_ok_ = false;
                    mut_toast_ = "Couldn't cancel " +
                                 std::to_string(cancel_failed) +
                                 " download(s) \xE2\x80\x94 series NOT removed";
                    return;   // abort before deleting anything (house rule)
                }
                if (qbit_ != nullptr) {
                    const auto hashes = sonarr_.get_series_download_hashes(sid);
                    // EXACT qBit call: copy DetailScreen run_remove step 2.
                    for (const auto& h : hashes) {
                        qbit_->delete_torrent(h, /*delete_files=*/true);
                    }
                }
                if (!sonarr_.remove_series(sid, /*delete_files=*/true)) {
                    mut_ok_ = false;
                    mut_toast_ =
                        "Remove failed \xE2\x80\x94 " + sonarr_.last_error();
                    return;
                }
                mut_ok_ = true;
                mut_toast_ = "Removed from TV library";
                remove_succeeded_ = true;
            });
            break;
        }
```

In `drain_mutation()`, after the toast: if `remove_succeeded_` — reset it, clear `series_`, `in_library_ = false`, `rebuild_rows()`, and set a member `navigate_back_ = true`; `handle_input` checks `navigate_back_` FIRST and returns `origin_` (screen transitions must come from handle_input — match how the codebase returns screens, and verify DetailScreen's remove uses the same drain→handle_input relay; if it instead returns from a drain called in update(), copy THAT).

- [ ] **Step 2: Mac suite + Pi compile + commit**

Mac loop **+0 cases** green; Pi incremental clean.

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp
git commit -m "feat(mb): orphan-proof series remove

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: Downloading badges + quiet re-poll

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.h` / `.cpp`

**Interfaces:**
- Consumes: `SonarrClient::get_series(sonarr_id)`, `get_queue()`; DetailScreen's `maybe_repoll_library` idiom (single reused thread, gen counter, inflight flag, 9 s cadence).
- Produces: live `downloading_seasons_` driving `SeasonState::Downloading`, and post-add settle (an unsettled add's statistics arrive without user action).

- [ ] **Step 1: Implement the poll (DetailScreen idiom, renamed)**

Header:

```cpp
    // ~9s quiet re-poll while InLibrary: fresh per-season statistics +
    // queue-derived downloading set. Single reused worker; never flashes
    // Loading (writes land via pending_/apply_pending like the fetch).
    void maybe_repoll_series();
    void run_series_poll(uint64_t gen, int sonarr_id, bool prev_sonarr_ok);
    std::atomic<uint64_t> poll_gen_{0};
    std::atomic<bool> poll_inflight_{false};
    std::chrono::steady_clock::time_point last_poll_at_{};
    static constexpr int kSeriesPollMs = 9000;
    std::thread poll_worker_;
```

Implementation (join `poll_worker_` in the dtor beside `mut_worker_`):

```cpp
void SeriesDetailScreen::maybe_repoll_series() {
    if (!in_library_ || !sonarr_ok_) return;
    if (!series_.has_value() || series_->sonarr_id <= 0) return;
    if (mut_in_flight_.load() || poll_inflight_.load()) return;
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_poll_at_)
                        .count();
    if (ms < kSeriesPollMs) return;
    last_poll_at_ = now;
    poll_inflight_.store(true);
    const uint64_t gen = poll_gen_.fetch_add(1) + 1;
    if (poll_worker_.joinable()) poll_worker_.join();
    try {
        poll_worker_ = std::thread(&SeriesDetailScreen::run_series_poll, this,
                                   gen, series_->sonarr_id, sonarr_ok_);
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] poll spawn failed: {}", e.what());
        poll_inflight_.store(false);
    }
}

void SeriesDetailScreen::run_series_poll(uint64_t gen, int sonarr_id,
                                         bool prev_sonarr_ok) {
    auto fresh = sonarr_.get_series(sonarr_id);
    std::unordered_set<int> downloading;
    for (const auto& q : sonarr_.get_queue()) {
        if (q.series_id == sonarr_id) downloading.insert(q.season_number);
    }
    if (gen == poll_gen_.load()) {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_.sonarr_done = true;
        // A poll is ADVISORY: a transient blip must not flip the page to
        // SonarrUnreachable under the user. Success proves reachability;
        // failure leaves the original fetch's verdict standing.
        pending_.sonarr_ok = fresh.has_value() ? true : prev_sonarr_ok;
        if (fresh.has_value()) {
            pending_.in_library = true;
            pending_.series = std::move(fresh);
        } else {
            pending_.in_library = in_library_;
        }
        pending_.downloading = std::move(downloading);
        pending_.has_downloading = true;
        pending_ready_.store(true, std::memory_order_release);
    }
    poll_inflight_.store(false, std::memory_order_release);
}
```

Extend `PendingLoad` with `std::unordered_set<int> downloading; bool has_downloading = false;` and in `apply_pending()` add, before `rebuild_rows()`:

```cpp
    if (p.has_downloading) downloading_seasons_ = std::move(p.downloading);
```

Call `maybe_repoll_series()` from `update()` after `drain_mutation()`. In `drain_mutation()`, when a mutation succeeded, force an early poll with `last_poll_at_ = {};` so badges appear within one frame of the next update rather than 9 s later.

**Failure semantics (binding, embodied in the code above):** a poll is advisory — `sonarr_ok` only ever ratchets TO true from a successful poll; a failed poll leaves the original fetch's verdict standing, so a transient blip cannot flip the page to SonarrUnreachable under the user.

- [ ] **Step 2: Mac suite + Pi compile + commit**

Mac loop **+0 cases** green; Pi incremental clean.

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp
git commit -m "feat(mb): per-season downloading badges + quiet series re-poll

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: Pi verify (both flags) + acceptance

**Files:** none modified. This task produces evidence.

- [ ] **Step 1: Mac suites** — full `ctest` run (all 9 targets) + `test_media_browser_unit` at its final count (281 + this plan's additions; assert "no fewer than 5692 assertions").

- [ ] **Step 2: Pi build, `ENABLE_MEDIA_BROWSER=ON`** — clean scratch configure + build in `~/mdb-2c2` per Global Constraints. `EXIT=0`, no warnings naming any file this plan touched.

- [ ] **Step 3: Pi build, `ENABLE_MEDIA_BROWSER=OFF`** — the OFF invariant: `series_detail_*` is MB-gated via `KIOSK_MEDIA_BROWSER_SOURCES`, `mb_screen.h`'s new enum value is harmless, and `main.cpp`'s new instance/case must sit inside the existing `#ifdef MEDIA_BROWSER_ENABLED` region (they do if placed beside `mb_detail`'s — verify). `EXIT=0`.

- [ ] **Step 4: Clean up** `ssh magic@magicpi5.local 'rm -rf ~/mdb-2c2'`.

- [ ] **Step 5: Hardware acceptance checklist (Alex, on the box).** Deploy after merge through the same direct-deploy flow as 2c-1 (`PI_HOST=magic@magicpi5.local ./scripts/deploy_cpp.sh --build` from the MERGED main — never from this worktree).

1. **TV poster opens the screen.** Marquee → TV mode → select any poster → SeriesDetail: title, poster, year/seasons/episodes meta, wrapped overview, season list with counts. BTN4 returns to Browse with grid position intact.
2. **Movie poster still opens the movie Detail** — unchanged in every respect.
3. **Season list truth.** Pick a show you know (e.g. Breaking Bad): season numbers and episode counts match TMDB; specials (S0) absent.
4. **Add Season 1.** On a show NOT in Sonarr: press Add Season 1 → "Working…" → toast. In Sonarr's web UI (port 8989): the series exists, ONLY Season 1 monitored, a season search ran (History). On the kiosk: row S1 flips to monitored; within ~18 s a grabbed release shows S1 `downloading`.
5. **settled==false path.** Immediately after the add toast, the season list must still show ALL seasons with TMDB counts — never "0 seasons" or an empty list. Within ~9 s the counts/monitored flags refresh from Sonarr.
6. **Next season.** Press "Download Season 2" → S2 monitored + search started (verify in Sonarr UI). Button relabels to "Download Season 3".
7. **Whole series — allow path.** On a small show with plenty of disk: "Whole series…" → button arms to "Confirm ~N GB" (sanity-check N ≈ episodes × 45 min × 70 MB/min) → confirm within 4 s → all seasons monitored + series search (Sonarr UI). Letting the arm EXPIRE (wait 5 s) reverts the label harmlessly.
8. **Whole series — BLOCK path.** Temporarily fill the SSD or pick something enormous (e.g. a 30-season show): pressing "Whole series…" toasts `Not enough space: needs ~X GB, Y GB free (20 GB floor)` and does NOT arm. Nothing was monitored (Sonarr UI unchanged).
9. **Remove, orphan-proof.** On the Task-4 test series with an active download: Remove → Confirm Remove within 2 s → toast, return to Browse. Verify: Sonarr no longer lists the series, qBit no longer lists its torrent, `/mnt/ssd/library/tv/<series>` gone.
10. **Downloading badge.** While something is downloading: the season row reads `downloading`; when the import completes the row flips to `complete` within ~9 s without leaving the screen.
11. **Unconfigured box.** Blank `SONARR_API_KEY` in `services/.env`, restart kiosk: SeriesDetail shows the full read-only page with the line `TV library not set up on this box` and NO action row. Restore the key.
12. **Sonarr stopped.** `docker stop mdb_sonarr` → SeriesDetail shows the read-only page with `Sonarr service offline`, no action row crash, BTN4 works. `docker start mdb_sonarr`.
13. **`scripts/verify_box.sh`** exits 0 (SHIPPABLE).

---

## Self-Review (already applied to the text above)

**Spec coverage.** Series detail screen (poster/overview + per-season state + counts): Tasks 3+8. "Add Season 1" default: Task 5. "Download next season": Task 5. "Whole series…" with confirm + estimate: Task 6. Remove: Task 7. Blocking preflight exactly per spec (block over free−20 GB, estimate shown, fail-open to warn): Tasks 1+6. Season-at-a-time = configuration (`monitor="firstSeason"`, flip + season search): Tasks 5-6. Season packs: nothing to do — season searches admit them server-side. Queue grouping / Search TV / Library mixed / episode picker: out of scope by the exclusion list.

**Consistency notes.** (1) In-library detection deliberately uses `get_library_checked()` + tmdb_id scan, NOT `find_series_by_tvdb` — so the Global Constraints' mock-honesty rule fires for NO new method this plan: `get_library_checked`'s mock is already honest, and `find_series_by_tvdb`/`is_reachable` keep zero kiosk callers and remain documented traps. (2) `Screen::SeriesDetail` lands in Task 3 (with a stub dispatch case) so every task ends with a compiling kiosk binary — there is NO multi-task broken window in this plan. (3) All mutation bodies run under `mut_mtx_` and only `drain_mutation` (render thread) touches `series_`/`rows_`/toasts. (4) `run_series_poll`'s failure semantics are advisory-not-authoritative — Task 8's caution paragraph is binding on the implementer and the reviewer.
