# TV Series Detail (Phase 2c-2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Revision note (2026-08-02).** This plan was verified against the real tree by two adversarial reviewers (API/concurrency + spec/failure-modes) and rewritten: 11 Critical and 15 Important findings are folded in. Every API named below has been read in the worktree. The code blocks are transcribable AS WRITTEN — do not "adapt as needed".

**Goal:** Selecting a TV poster in Browse opens a new SeriesDetailScreen — poster/overview + a season list with per-season state — from which the user can add Season 1, download the next season, add the whole series behind the codebase's first blocking disk preflight, and remove the series orphan-proof.

**Architecture:** A pure, Renderer-free `series_detail_logic.h` (browse_logic.h house style) owns every decision — season-row merge (TMDB base + Sonarr statistics overlay, immune to the `settled==false ⇒ empty seasons` contract), season state, screen state resolution, disk estimate, and the block/warn/allow verdict — all Mac-tested. A new GLES `SeriesDetailScreen` copies DetailScreen's proven thread idioms (FetchWorker+DoneFlag for loads, single member thread + atomics for mutations, `catch (const std::system_error&)` on every spawn, gen-bump + join in the dtor) and DetailScreen's exact input idiom (rotary gated on `e.delta != 0`, buttons on `e.pressed`, drain-relay at the top of `handle_input`). Registration is the known 4-part addition; the dispatch switch has no `default:`, so the compiler enforces completeness.

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
- **`WatchdogSec=10`: every Sonarr mutation runs on a worker thread.** `add_series` alone can take ≈13.5 s (`add_settle_timeout_ms + add_settle_poll_ms + timeout_secs`). Copy DetailScreen's spawn idiom including `catch (const std::system_error&)` — an uncaught throw from a thread ctor is `std::terminate`, a hard kiosk crash. **A worker must never hold `mut_mtx_` across a network call**: take the lock only to publish the result.
- **The whole-series flow issues N sequential GET+PUT round trips** (one `set_season_monitored` per unmonitored season, each a GET of the full series plus a PUT), then a search command, then a final GET. On a 21-season show that is ~44 round trips under one user gesture. Accepted for this phase because it all runs on the mutation worker and the render thread never blocks — but it is the reason the action row stays interactive-but-dimmed rather than pretending the operation is instant, and it is the first thing to batch if the wait ever reads as a hang.
- **`settled == false ⇒ `AddSeriesResult::series.seasons` is ALWAYS EMPTY** — never render that as "0 seasons". Two meanings: poll timeout (transient) or never-refreshed existing record (PERMANENT for announced series). The season list therefore ALWAYS builds from TMDB's `TmdbTvDetail::seasons` as the base, with Sonarr statistics overlaid when present — the UI never depends on Sonarr seasons existing. An unsettled record also reads as **every season unmonitored**, which is why the action row hides the add controls until it settles (Task 5).
- **`Series : SeriesSearchHit` SLICES** — assigning a `Series` into a `SeriesSearchHit` drops `sonarr_id`/`path`/`monitored`. Never pass a `Series` by value as its base.
- **`SeriesSearchHit::runtime_minutes` is PER-EPISODE** (the disk-estimate multiplicand). It exists only on a record Sonarr has, so PRE-ADD the estimate necessarily falls back to `estimate_remaining_bytes`' 45-minute assumption. That is why every estimate the UI shows is labelled `(est)`.
- **Render discipline (2c-1 lesson, structural):** `render()` must not gain an early `return` — every state path falls through, and **the footer hint row is the last draw on every path**. The screen has no input-swallowing modal (its confirms are button-label swaps), and it does **not** draw a Toast itself: `main.cpp` (the `ui::Toast::render` call beside the `glViewport` reset) owns the single toast draw for the whole app, in the correct projection. A screen-level `::ui::Toast::render` would double-draw and mis-place the panel — the comment at that main.cpp call site cites the operator-reported CRT bug that produced this rule.
- **Mock-honesty rule — what actually protects this screen.** `SonarrMockClient` overrides every public virtual by contract, and several of those overrides are deliberately dishonest traps. This plan does **not** claim "no newly-trapped method is consumed": `add_series` transitively calls `find_series_by_tvdb` inside the real client, so an add exercises that family whether the screen names it or not. **The load-bearing invariant is `sonarr_configured_` gating.** The mock is constructed if and only if `sonarr_key` is empty (main.cpp's `sonarr_owned` branch), the screen receives that same flag as `sonarr_configured`, and when it is false the screen never calls Sonarr at all — it resolves to `NotConfigured`, renders read-only, and offers no action row. So on a box where the mock exists, zero Sonarr calls are made from this screen; on a box where calls are made, the client is real.
  **Binding warning for future work:** any change that renders Sonarr actions in a degraded or unconfigured state — a "retry" button on the `NotConfigured` page, a mock-mode dev affordance, an action row drawn before `sonarr_ok_` is known — puts the whole dishonest-mock family live at once. Whoever makes that change must first make the relevant overrides honest, in the same commit, to the standing precedent (`get_library_checked() → std::nullopt`, "engaged = the service answered", commit `63f9046`).
  In-library detection here uses `get_library_checked()` (mock already honest) rather than `lookup_by_tmdb`→`find_series_by_tvdb`: one call, no TMDB→TVDB hop, transport failure distinguishable. The one NEW virtual (Task 2's `get_quality_definitions`) gets its mock override in the same task.
- **`sonarr_configured` gates everything Sonarr-shaped.** BrowseScreen already receives `/*sonarr_configured=*/!sonarr_key.empty()` (main.cpp:885). SeriesDetailScreen receives the same flag; when false it never calls Sonarr, offers no mutation, and shows the established copy family ("TV library not set up on this box").
- **Out of scope — do not implement:** queue-screen grouping by downloadId, Library mixed-kind listing, Search TV mode, `TmdbClient::search_tv`, episode picker / TV playback (Phase 3), any Sonarr health gate, per-episode monitoring UI.

---

## File Structure

**Created**

| File | Responsibility |
|---|---|
| `src/media_browser/ui/series_detail_logic.h` | ALL decisions, pure + header-only: `SeasonRow`/`SeasonState`, `merge_season_rows`, `decide_season_state`, `SeriesDetailState` + `SeriesDetailInputs` + `decide_series_detail_state` + `series_detail_state_message`, `next_unmonitored_season`, `estimate_remaining_bytes`, `pick_preferred_mb_per_min`, `whole_series_verdict`, `cancel_ids_for_series` (Task 7). |
| `src/media_browser/ui/series_detail_screen.{h,cpp}` | The GLES screen: load workers, action row, paged season list, mutation workers, re-poll. Kiosk-only (`KIOSK_MEDIA_BROWSER_SOURCES`). |
| `tests/media_browser/test_series_detail_logic.cpp` | The pure core's suite. |

**Modified**

| File | Change |
|---|---|
| `src/media_browser/sonarr/sonarr_types.h` | `QualityDefinition{quality_id, title, preferred_mb_per_min, max_mb_per_min}`. |
| `src/media_browser/sonarr/sonarr_parsers.{h,cpp}` | `static std::vector<QualityDefinition> parse_quality_definitions(const std::string&);` on `class SonarrParsers`. |
| `src/media_browser/sonarr/sonarr_client.{h,cpp}` | `virtual std::vector<QualityDefinition> get_quality_definitions();` (body mirrors `get_root_folders` exactly). Task 7 also adds the `set_error({})` entry clear to `get_series_download_hashes` — `.cpp` only, the declaration is unchanged. |
| `src/media_browser/sonarr/sonarr_mock.{h,cpp}` | Override `get_quality_definitions` (fixture-shaped values — config data, fabricates nothing). |
| `src/media_browser/ui/mb_chrome.{h,cpp}` | `wrap_text` PROMOTED out of detail_screen.cpp's anonymous namespace (same precedent as `truncate_to_width`, mb_chrome.h:42). |
| `src/media_browser/ui/detail_screen.cpp` | Local `wrap_text` deleted; its one call site becomes `chrome::wrap_text(...)`. |
| `src/media_browser/ui/mb_screen.h` | `Screen::SeriesDetail` enum value. |
| `src/media_browser/ui/browse_screen.cpp` | TV SELECT routes to `Screen::SeriesDetail` instead of the Toast; footer hint "Coming soon" → "Detail". **No new accessor** — the kind is encoded in the returned `Screen`. |
| `src/main.cpp` | SeriesDetailScreen instance (4-arg ctor: `sonarr, *tmdb, qbit_owned.get(), /*sonarr_configured=*/!sonarr_key.empty()`), dispatch case, `set_tmdb_id`/`set_origin` forwarding block. |
| `CMakeLists.txt` | `series_detail_screen.cpp` → `KIOSK_MEDIA_BROWSER_SOURCES`; `test_series_detail_logic.cpp` → `MEDIA_BROWSER_TEST_SOURCES`. |
| `tests/media_browser/test_sonarr_parsers.cpp`, `tests/media_browser/test_sonarr_client.cpp` | Parser + mock contract cases (Task 2). |

**Task map (9 tasks).** 1 pure logic core · 2 quality definitions endpoint · 3 screen read-only core (+ `wrap_text` promotion) · 4 registration + Browse routing · 5 action row + add flows · 6 whole-series confirm + blocking preflight · 7 orphan-proof remove · 8 downloading badges + quiet re-poll · 9 Pi verify (both flags) + acceptance checklist. Tasks 1–2 are strictly test-first; 3–8 each open with any new pure helper + its failing test, then the Renderer-bound half; the kiosk binary compiles at the END of every task (no multi-task broken windows this phase — Task 3's screen is registered nowhere but compiles standalone).

---
### Task 1: series_detail_logic.h — the pure decision core

**Files:**
- Create: `src/media_browser/ui/series_detail_logic.h`
- Create: `tests/media_browser/test_series_detail_logic.cpp`
- Modify: `CMakeLists.txt` (test file into `MEDIA_BROWSER_TEST_SOURCES`)

**Interfaces:**
- Consumes: `TmdbTvSeason` (tmdb_client.h:84), `Series`/`Season` (sonarr_types.h), `QualityDefinition` (added to `sonarr_types.h` by THIS task — see the Step 3 note).
- Produces (Tasks 3–8 rely on these exact names): `SeasonState`, `SeasonRow`, `decide_season_state(int,int,bool)`, `merge_season_rows(const std::vector<TmdbTvSeason>&, const Series*, const std::unordered_set<int>&)`, `next_unmonitored_season(const std::vector<SeasonRow>&)`, `estimate_remaining_bytes(const std::vector<SeasonRow>&, int, double)`, `pick_preferred_mb_per_min(const std::vector<QualityDefinition>&)`, `DiskVerdict`, `whole_series_verdict(int64_t, std::optional<int64_t>)`, `kDiskFloorBytes`, `SeriesDetailState`, `SeriesDetailInputs`, `decide_series_detail_state(...)`, `series_detail_state_message(SeriesDetailState)`.
- **Task 5 APPENDS to both files created here** — the action row's pure algebra (`Action`, `ActionButton`, `canonical_action`, `whole_series_label`, `ActionRowInputs`, `ActionRow`, `decide_action_row`) plus its 8 table cases. See Task 5 Step 0; nothing in THIS task depends on them, so Task 1 stands alone as written below.

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
    // The runtime<=0 path is also the PRE-ADD path: Sonarr has no record yet,
    // so 45 is an assumption and every label built from it says "(est)".
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

TEST_CASE("whole_series_verdict: block at free minus 20 GiB floor; only a FAILED stat warns",
          "[series_detail]") {
    const int64_t GiB = 1024LL * 1024 * 1024;
    // free 100 GiB, floor 20 → budget 80
    CHECK(whole_series_verdict(80 * GiB, 100 * GiB) == DiskVerdict::Allow);
    CHECK(whole_series_verdict(80 * GiB + 1, 100 * GiB) == DiskVerdict::Block);
    // nullopt == the stat FAILED (nothing answered) — warn, never wedge.
    CHECK(whole_series_verdict(1, std::nullopt) == DiskVerdict::WarnOnly);
    // A READING of zero is the full-disk case, which is exactly what the
    // blocking preflight exists for. Failing open here would let the one
    // situation the feature was built to catch sail straight through.
    CHECK(whole_series_verdict(1, int64_t{0}) == DiskVerdict::Block);
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
#include <string>
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
//
// runtime<=0 is ALSO the ordinary pre-add case (Series::runtime_minutes
// only exists once Sonarr has the record), so a pre-add estimate is an
// ASSUMPTION, not a measurement. Every label built from this value must
// say so — see the "(est)" suffix in the whole-series confirm.
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
// 20 GiB floor.
//
// WarnOnly is for a FAILED reading only (nullopt — nothing answered), per
// the movie flow's philosophy: warn, never wedge. A reading OF ZERO is not
// a failure, it is the full disk — the exact case this preflight exists to
// stop — so it Blocks. With estimate > 0 and a 20 GiB floor the general
// comparison already yields Block for free==0; the explicit branch below
// makes that intent unmistakable rather than incidental.
enum class DiskVerdict { Allow, Block, WarnOnly };

inline constexpr int64_t kDiskFloorBytes = 20LL * 1024 * 1024 * 1024;

inline DiskVerdict whole_series_verdict(int64_t estimate_bytes,
                                        std::optional<int64_t> free_bytes) {
    if (!free_bytes.has_value()) return DiskVerdict::WarnOnly;
    if (*free_bytes <= 0) return DiskVerdict::Block;
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

**Step 3 note (build order):** this header references `QualityDefinition`, which does not exist until Task 2. To keep THIS task green standalone, add the struct to `src/media_browser/sonarr/sonarr_types.h` NOW, in this task, placed after the `SonarrQueueItem` struct with this exact text (Task 2 then only adds the parser + client method + mock override, not the type):

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
- Modify: `src/media_browser/sonarr/sonarr_parsers.h` (declaration on `class SonarrParsers`), `src/media_browser/sonarr/sonarr_parsers.cpp` (parser)
- Modify: `src/media_browser/sonarr/sonarr_client.h` / `.cpp` (virtual + fetch)
- Modify: `src/media_browser/sonarr/sonarr_mock.h` / `.cpp` (override — the mock overrides EVERY public virtual by contract, sonarr_mock.h:11-14)
- Test: `tests/media_browser/test_sonarr_parsers.cpp` (append), `tests/media_browser/test_sonarr_client.cpp` (append)

**Interfaces:**
- Consumes: `QualityDefinition` (added to `sonarr_types.h` by Task 1).
- Produces: `virtual std::vector<QualityDefinition> get_quality_definitions();` on `SonarrClient` (empty vector = fetch/parse failed — consumers fall back to the 70 default via `pick_preferred_mb_per_min`); `static std::vector<QualityDefinition> parse_quality_definitions(const std::string& json);` on `class SonarrParsers`.

**House style (verified in the tree — do not deviate):** every Sonarr parser is a **static member of `class SonarrParsers`**, not a free function. Tests call it as `mb::SonarrParsers::parse_quality_definitions(...)` (the test files already do `namespace mb = media_browser;`). The client's fetch body takes a **PATH** — `http_get` prepends `base_url` and the api key itself (`SonarrClient::http_get`, sonarr_client.cpp:95); passing `cfg_.base_url + "/api/v3/..."` would produce a doubled URL and a SILENT empty body, which `pick_preferred_mb_per_min` would quietly turn into the 70 fallback with no error anywhere.

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
    auto defs = mb::SonarrParsers::parse_quality_definitions(body);
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
    CHECK(mb::SonarrParsers::parse_quality_definitions("").empty());
    CHECK(mb::SonarrParsers::parse_quality_definitions("not json").empty());
    CHECK(mb::SonarrParsers::parse_quality_definitions(R"({"error":"x"})").empty());
    // Rows missing the quality object are skipped, not fatal.
    CHECK(mb::SonarrParsers::parse_quality_definitions(R"([{"minSize": 1.0}])").empty());
}
```

- [ ] **Step 2: Run to verify failure**

Mac test loop. Expected: FAIL — `parse_quality_definitions` is not a member of `SonarrParsers`.

- [ ] **Step 3: Implement parser + client + mock**

In `src/media_browser/sonarr/sonarr_parsers.h`, inside `class SonarrParsers`, directly after the line `static std::vector<RootFolder> parse_root_folders(const std::string& json);`:

```cpp
    // GET /api/v3/qualitydefinition rows. Tolerant: non-array bodies and rows
    // missing the quality object yield/skip empty — the consumer treats an
    // empty vector as "use the fallback rate", never as an error state.
    static std::vector<QualityDefinition> parse_quality_definitions(
        const std::string& json);
```

In `src/media_browser/sonarr/sonarr_parsers.cpp`, after `SonarrParsers::parse_root_folders`, using the file's own anonymous-namespace `parse_json` helper (the idiom every sibling parser in that file uses):

```cpp
std::vector<QualityDefinition> SonarrParsers::parse_quality_definitions(
        const std::string& json) {
    std::vector<QualityDefinition> out;
    Json::Value root;
    if (!parse_json(json, root)) return out;
    if (!root.isArray()) return out;
    for (const auto& row : root) {
        if (!row.isObject() || !row["quality"].isObject()) continue;
        QualityDefinition d;
        d.quality_id = row["quality"].get("id", 0).asInt();
        d.title      = row["quality"].get("name", "").asString();
        // preferredSize/maxSize are null upstream for "unlimited", and
        // asDouble() on a null throws in jsoncpp — gate on isNumeric().
        if (row["preferredSize"].isNumeric())
            d.preferred_mb_per_min = row["preferredSize"].asDouble();
        if (row["maxSize"].isNumeric())
            d.max_mb_per_min = row["maxSize"].asDouble();
        out.push_back(std::move(d));
    }
    return out;
}
```

In `src/media_browser/sonarr/sonarr_client.h`, after `virtual std::vector<RootFolder> get_root_folders();`:

```cpp
    // Quality definitions — the MB/min table behind the TV disk estimate.
    // Empty on any failure; pick_preferred_mb_per_min falls back to 70.
    virtual std::vector<QualityDefinition> get_quality_definitions();
```

In `src/media_browser/sonarr/sonarr_client.cpp`, directly after `SonarrClient::get_root_folders` — the body is that method's shape line for line, including the PATH-only argument:

```cpp
std::vector<QualityDefinition> SonarrClient::get_quality_definitions() {
    auto resp = http_get("/api/v3/qualitydefinition");
    if (resp.empty()) return {};
    return SonarrParsers::parse_quality_definitions(resp);
}
```

In `src/media_browser/sonarr/sonarr_mock.h`, beside the other overrides (after `std::vector<RootFolder> get_root_folders() override;`):

```cpp
    std::vector<QualityDefinition> get_quality_definitions() override;
```

In `src/media_browser/sonarr/sonarr_mock.cpp`:

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

The screen exists, loads, and renders a paged season list; nothing routes to it yet (Task 4) and it offers no actions yet (Tasks 5–7). It must compile into the kiosk binary at the end of this task.

**Files:**
- Modify: `src/media_browser/ui/mb_chrome.h` / `.cpp` (promote `wrap_text`)
- Modify: `src/media_browser/ui/detail_screen.cpp` (delete the local copy, retarget its one call site)
- Create: `src/media_browser/ui/series_detail_screen.h`
- Create: `src/media_browser/ui/series_detail_screen.cpp`
- Modify: `src/media_browser/ui/mb_screen.h` (`Screen::SeriesDetail`), `src/main.cpp` (stub dispatch case)
- Modify: `CMakeLists.txt` (`series_detail_screen.cpp` into `KIOSK_MEDIA_BROWSER_SOURCES`, after the line `src/media_browser/ui/detail_screen.cpp`)

**Interfaces:**
- Consumes: everything Task 1 produced; `TmdbClient::get_tv_detail(int)`; `SonarrClient::get_library_checked()`; `SonarrClient::get_quality_definitions()` (Task 2); `record_refreshed(const Series&)` (free function, sonarr_client.h); chrome helpers (`draw_screen_header`, `draw_poster_card`, `draw_footer_hints`, `wrap_text`) and `truncate_to_width` (mb_ui_utils pure core).
- Produces (Tasks 4–8 rely on): class `SeriesDetailScreen` with ctor `SeriesDetailScreen(SonarrClient& sonarr, TmdbClient& tmdb, QbittorrentClient* qbit, bool sonarr_configured)`; `void set_tmdb_id(int)`; `int tmdb_id() const`; `void set_origin(Screen)`; `Screen origin() const`; the four `MbScreen` overrides; private `fetch()`, `apply_pending()`, `rebuild_rows()`, and the members named in the header below.

**Design notes the code below embodies (do not "simplify" them away):**
- **In-library detection is `get_library_checked()` + scan by `tmdb_id`** — one call, no TMDB→TVDB hop, transport failure distinguishable (`nullopt`), and the mock is already honest on it.
- Thread idiom is DetailScreen's `FetchWorker` + `DoneFlag` + generation counter, verbatim in shape.
- **`fetch()` bumps BOTH generations before it clears anything.** `poll_gen_` is declared in this task even though Task 8 is what spawns the poll: the poll publishes into the same `pending_`, so if `fetch()` did not invalidate it, a poll for series A could land in series B's `pending_` and Remove would then target A's `sonarr_id` under B's header.
- `render()` has **no early return**; the footer hint row is the last draw on every path. The screen draws **no Toast** — main.cpp owns the single toast draw.

- [ ] **Step 1: Promote `wrap_text` into mb_chrome**

`wrap_text` currently lives in `detail_screen.cpp`'s anonymous namespace (its greedy pixel-width word-wrap, with a hard character split for over-wide words). Two screens now need it, and internal linkage means the second one silently gets a second copy — the exact drift class that produced the "recently added" filter bug. The precedent for this move is `truncate_to_width` (see mb_chrome.h:42 and the comment above it: Renderer-shaped helpers live in mb_chrome because naming `::ui::Renderer` drags in GLES, which mb_ui_utils must stay free of).

**1a.** In `src/media_browser/ui/mb_chrome.h`, inside `namespace media_browser::ui::chrome` (put it directly above the `// ---------- Layout constants ----------` banner):

```cpp
// Greedy word-wrap by pixel width, one line per vector entry. Words that
// individually exceed max_w are hard-split by character at whatever point
// fits — no hyphenation, no shaping.
//
// Promoted here out of detail_screen.cpp's anonymous namespace when the
// series detail screen became its second caller, for the same reason
// truncate_to_width was promoted (see the comment above that declaration):
// internal linkage means every extra caller silently gets its own copy, and
// copies drift. It lives in mb_chrome rather than mb_ui_utils because it
// names ::ui::Renderer, which drags in GLES and cannot appear in a macOS
// test target.
//
// PlaybackOverlay's `wrap_text_overlay` is deliberately NOT folded in: it is
// a different function with different behavior, and re-pointing it is a
// behavior change with no caller asking for one.
std::vector<std::string> wrap_text(::ui::Renderer& r, const std::string& text,
                                   int font_size, float max_w);
```

**1b.** In `src/media_browser/ui/mb_chrome.cpp`, add `#include <sstream>` to the standard-header block (beside `<algorithm>` / `<cmath>` / `<string>`), and add `#include <vector>` if not already pulled in by mb_chrome.h. Then, inside `namespace media_browser::ui::chrome` (after the anonymous-namespace constants block, before `draw_focus_ring`), paste the function body **exactly as it stands today in detail_screen.cpp** — that file's `wrap_text` from its `std::vector<std::string> wrap_text(` line through its closing brace, unchanged:

```cpp
std::vector<std::string> wrap_text(::ui::Renderer& r, const std::string& text,
                                   int font_size, float max_w) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;

    std::istringstream iss(text);
    std::string word;
    std::string current;

    auto width_of = [&](const std::string& s) {
        return static_cast<float>(r.mb_text_width(s, font_size));
    };

    while (iss >> word) {
        std::string candidate = current.empty() ? word : current + " " + word;
        if (width_of(candidate) <= max_w) {
            current = candidate;
            continue;
        }
        // Candidate is too wide. Flush current (if any) and start new line.
        if (!current.empty()) {
            lines.push_back(current);
            current.clear();
        }
        // If the word by itself fits on a line, start the line with it.
        if (width_of(word) <= max_w) {
            current = word;
            continue;
        }
        // Word is wider than max_w — hard-split by characters.
        std::string fragment;
        for (char c : word) {
            std::string next = fragment + c;
            if (width_of(next) > max_w) {
                lines.push_back(fragment);
                fragment = std::string(1, c);
            } else {
                fragment = next;
            }
        }
        current = fragment;
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}
```

**1c.** In `src/media_browser/ui/detail_screen.cpp`, DELETE the local definition — the comment block beginning `// Greedy word-wrap by pixel width. Breaks words that individually exceed` through the closing brace of `wrap_text` — and change its single call site (inside the `lay_out_block` lambda) from

```cpp
        auto lines = wrap_text(r, body, body_font_size, col_w);
```

to

```cpp
        auto lines = chrome::wrap_text(r, body, body_font_size, col_w);
```

`chrome::` qualification is required: after the deletion, unqualified lookup from inside `media_browser::ui` finds nothing, and ADL on a `::ui::Renderer&` argument searches namespace `::ui`, not `media_browser::ui::chrome`. detail_screen.cpp already includes mb_chrome.h (line 2). Leave `truncate_wrapped` where it is — it is a different helper with one caller.

- [ ] **Step 2: Write the header**

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

// TV series detail (Phase 2c-2): poster/overview + a paged season list with
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
    // Everything a worker may publish. Declared complete in this task even
    // though Task 8's re-poll is what fills the last three fields — the
    // struct is the contract between every worker and apply_pending(), and
    // growing it later would mean editing the drain in two places.
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
        // Has Sonarr ever actually refreshed this record? (record_refreshed)
        bool has_settled = false;
        bool settled = true;
        // Seasons with live queue activity (Task 8's poll).
        std::unordered_set<int> downloading;
        bool has_downloading = false;
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

    // Authoritative render-thread state (apply_pending / drain_mutation only).
    std::optional<TmdbTvDetail> detail_;
    std::optional<Series> series_;
    std::vector<SeasonRow> rows_;
    std::unordered_set<int> downloading_seasons_;  // fed by Task 8
    double mb_per_min_ = 70.0;
    bool tmdb_done_ = false, tmdb_ok_ = false;
    bool sonarr_done_ = false, sonarr_ok_ = false, in_library_ = false;
    // False for the window where Sonarr holds the record but has never
    // refreshed it: seasons[] is empty and EVERY row reads unmonitored, so
    // the add controls must not be offered (they would say "Download
    // Season 1" one second after adding Season 1).
    bool series_settled_ = true;

    // Season-list paging. BTN1/BTN3 move pages; render() recomputes the page
    // count each frame from the space actually left after the reserved
    // action row + indicator row, and clamps season_page_ into range.
    int season_page_ = 0;
    int season_page_count_ = 1;

    std::atomic<uint64_t> fetch_gen_{0};
    // Task 8's quiet re-poll publishes into pending_ as well, so its
    // generation must be invalidated by fetch() and the dtor. Declared here
    // so that discipline is in place from the file's first version.
    std::atomic<uint64_t> poll_gen_{0};
    std::chrono::steady_clock::time_point last_poll_at_{};
    std::mutex pending_mtx_;
    PendingLoad pending_;
    std::atomic<bool> pending_ready_{false};
    std::vector<FetchWorker> workers_;
};

}  // namespace media_browser::ui
```

- [ ] **Step 3: Write the implementation**

Create `src/media_browser/ui/series_detail_screen.cpp` exactly:

```cpp
#include "media_browser/ui/series_detail_screen.h"

#include <algorithm>
#include <system_error>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "platform/input_manager.h"
#include "spdlog/spdlog.h"
#include "ui/renderer.h"
#include "ui/theme.h"

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
// Mirrors mb_chrome.cpp's anonymous-namespace kTitleFontPx (32): the header
// title is drawn with mb_draw_title_text in the Zen Dots face, which is a
// DIFFERENT metric from mb_text_width. Measuring the title with the body
// font would under-cut it and let a long series name run under the frame.
constexpr int kHeaderTitlePx = 32;
// Vertical budget reserved out of the season list, once, in render():
// the action-row buttons (chrome::draw_button is 18 px label + 10 px
// vertical padding + 2 px border, so 52) and the paging indicator line.
constexpr int kButtonRowH = 52;
constexpr int kIndicatorRowH = 24;
}  // namespace

SeriesDetailScreen::SeriesDetailScreen(SonarrClient& sonarr, TmdbClient& tmdb,
                                       QbittorrentClient* qbit,
                                       bool sonarr_configured)
    : sonarr_(sonarr), tmdb_(tmdb), qbit_(qbit),
      sonarr_configured_(sonarr_configured) {}

SeriesDetailScreen::~SeriesDetailScreen() {
    // Invalidate every in-flight publish BEFORE joining. Task 8 states this
    // destructor's final form; until then there is no poll worker to join.
    fetch_gen_.fetch_add(1);
    poll_gen_.fetch_add(1);
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
    // Bump BOTH generations FIRST, then clear. A load worker or a re-poll
    // that finishes between here and the clear must find a stale generation
    // and discard: otherwise it publishes series A's Series / in_library into
    // the pending_ that series B is about to drain, and Remove would target
    // A's sonarr_id under B's header.
    const uint64_t gen = fetch_gen_.fetch_add(1) + 1;
    poll_gen_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_ = PendingLoad{};
        pending_ready_.store(false, std::memory_order_release);
    }
    // Reset render-thread state to a clean Loading page.
    detail_.reset();
    series_.reset();
    rows_.clear();
    downloading_seasons_.clear();
    tmdb_done_ = tmdb_ok_ = false;
    sonarr_done_ = sonarr_ok_ = in_library_ = false;
    series_settled_ = true;
    season_page_ = 0;
    season_page_count_ = 1;
    // Task 8's poll gate must not inherit series A's timestamp — it would
    // delay series B's first poll by a full interval.
    last_poll_at_ = {};
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
    std::lock_guard<std::mutex> lk(pending_mtx_);
    // Recheck under the lock — a worker that passed a pre-lock check could
    // be descheduled across fetch()'s bump-and-clear and publish stale data
    // into the new series' pending_.
    if (gen != fetch_gen_.load()) return;  // preempted — discard
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
    // round-trip, and this shape needs no TMDB->TVDB hop at all.
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
    std::lock_guard<std::mutex> lk(pending_mtx_);
    // Recheck under the lock — a worker that passed a pre-lock check could
    // be descheduled across fetch()'s bump-and-clear and publish stale data
    // into the new series' pending_.
    if (gen != fetch_gen_.load()) return;  // preempted — discard
    pending_.sonarr_done = true;
    pending_.sonarr_ok = lib.has_value();
    pending_.in_library = match.has_value();
    if (match.has_value()) {
        pending_.settled = record_refreshed(*match);
        pending_.has_settled = true;
    }
    pending_.series = std::move(match);
    pending_.quality_defs = std::move(defs);
    pending_ready_.store(true, std::memory_order_release);
}

void SeriesDetailScreen::apply_pending() {
    if (!pending_ready_.load(std::memory_order_acquire)) return;
    PendingLoad p;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        // Move-and-reset, not copy: each half publishes once. tmdb_done_ /
        // detail_ / sonarr_done_ / series_ etc. are accumulated on `this`
        // (not re-derived from pending_ each frame), so a half applied on
        // an earlier drain is never lost when the other half's later
        // publish resets pending_ to fresh defaults.
        p = std::move(pending_);
        pending_ = PendingLoad{};
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
        if (p.has_settled) series_settled_ = p.settled;
        if (!p.quality_defs.empty())
            mb_per_min_ = pick_preferred_mb_per_min(p.quality_defs);
    }
    if (p.has_downloading) downloading_seasons_ = std::move(p.downloading);
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
        // BTN4 (SETTINGS_MENU, black) — back to whoever opened us. Gated on
        // e.pressed, exactly like DetailScreen's.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return origin_;
        }
        // BTN1 / BTN3 page the season list. Task 5 replaces this whole
        // function with its final form (rotary + SELECT + the back relay).
        if (e.action == platform::InputAction::PREV && e.pressed) {
            if (season_page_ > 0) --season_page_;
            continue;
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            if (season_page_ + 1 < season_page_count_) ++season_page_;
            continue;
        }
    }
    return Screen::SeriesDetail;
}

void SeriesDetailScreen::update() { apply_pending(); }

void SeriesDetailScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const auto& th = r.mb_theme();
    r.mb_fill_rect(0, 0, static_cast<float>(screen_w),
                   static_cast<float>(screen_h), th.bg);
    // Recomputed by the body path below; body-less states have one page.
    season_page_count_ = 1;
    bool overflow = false;

    // Header: EMPTY tab strip, exactly like DetailScreen (detail_screen.cpp
    // passes /*tabs=*/{}). This screen is a drill-down, not a strip member,
    // and the 7-chip strip drawn under an arbitrary series title overlaps it.
    // The title is measured with the TITLE font's own measurer — the
    // Renderer overload of truncate_to_width measures the body font, which
    // would under-cut a Zen Dots title.
    const std::string raw_title =
        detail_.has_value() ? detail_->title : std::string("Series");
    const std::string title = truncate_to_width(
        raw_title, kHeaderTitlePx,
        static_cast<float>(screen_w - 2 * chrome::kSafeInset_px),
        [&r](const std::string& s, int px) {
            return static_cast<float>(r.mb_title_text_width(s, px));
        });
    const int content_top = chrome::draw_screen_header(
        r, screen_w, title, /*tabs=*/{}, /*focused_tab=*/-1);

    const SeriesDetailInputs in{tmdb_done_, tmdb_ok_, sonarr_configured_,
                                sonarr_done_, sonarr_ok_, in_library_};
    const SeriesDetailState st = decide_series_detail_state(in);
    const char* msg = series_detail_state_message(st);

    const bool body_less =
        (st == SeriesDetailState::Loading || st == SeriesDetailState::TmdbError);
    if (body_less) {
        // Centered message; falls THROUGH to the footer below. No return.
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
        // Meta line: year · seasons · episodes · status [· syncing…].
        {
            std::string meta = std::to_string(detail_->year);
            meta += " \xC2\xB7 " + std::to_string(detail_->number_of_seasons) +
                    " season" + (detail_->number_of_seasons == 1 ? "" : "s");
            meta += " \xC2\xB7 " + std::to_string(detail_->number_of_episodes) +
                    " episodes";
            if (!detail_->status.empty()) meta += " \xC2\xB7 " + detail_->status;
            // Honest label for the window where Sonarr holds the record but
            // has never refreshed it: the rows below are TMDB's, not Sonarr's.
            if (in_library_ && !series_settled_)
                meta += " \xC2\xB7 syncing\xE2\x80\xA6";
            r.mb_draw_text(truncate_to_width(r, meta, kBodyFontPx, text_w),
                           static_cast<float>(text_x),
                           static_cast<float>(y + 16), kBodyFontPx, th.dim);
        }
        // Overview: wrapped by the promoted chrome helper, max 5 lines.
        {
            const auto lines =
                chrome::wrap_text(r, detail_->overview, kBodyFontPx, text_w);
            int line_y = y + 44;
            for (size_t i = 0; i < lines.size() && i < 5; ++i) {
                r.mb_draw_text(lines[i], static_cast<float>(text_x),
                               static_cast<float>(line_y), kBodyFontPx, th.fg);
                line_y += kBodyFontPx + 6;
            }
        }
        // ---- season list (paged) ----
        // The action row AND the paging indicator are reserved out of the
        // list's budget HERE, once. Nothing below may draw past list_top +
        // per_page*kRowH, so the "+N more" overlap of the button row that
        // the previous revision had is structurally impossible.
        const int list_top = y + poster_h + chrome::kPad3;
        const int list_bottom = screen_h - chrome::kFooterHeight_px -
                                chrome::kPad3;
        const int list_avail =
            list_bottom - kButtonRowH - kIndicatorRowH - list_top;
        // CRT_NATIVE (640x480 logical) leaves no room below the poster —
        // list_avail goes NEGATIVE there, and a forced one-row minimum drew
        // into the reserved button band. Clamp to zero rows: the totals
        // line, action row, and footer still render; per-season detail is a
        // 720p+ affordance.
        const int per_page = std::max(0, list_avail / kRowH);
        const int total_rows = static_cast<int>(rows_.size());
        // per_page can be 0 on the 640x480 canvas (the clamp above) —
        // dividing by it is UB, and the indicator would land back in the
        // poster region. Zero rows means one page and no paging affordance.
        season_page_count_ = per_page > 0
            ? std::max(1, (total_rows + per_page - 1) / per_page) : 1;
        if (season_page_ >= season_page_count_)
            season_page_ = season_page_count_ - 1;
        if (season_page_ < 0) season_page_ = 0;
        overflow = per_page > 0 && total_rows > per_page;
        const int first = season_page_ * per_page;
        const int last = std::min(total_rows, first + per_page);

        int list_y = list_top;
        for (int i = first; i < last; ++i) {
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
        // Paging indicator — inside the reserved band, only when it earns
        // its line. A 21-season show is the headline case for this screen;
        // without paging its later seasons were simply unreachable.
        if (overflow) {
            const std::string ind =
                "Seasons " + std::to_string(first + 1) + "\xE2\x80\x93" +
                std::to_string(last) + " of " + std::to_string(total_rows) +
                " \xC2\xB7 [BTN1/BTN3]";
            r.mb_draw_text(ind, static_cast<float>(body_x),
                           static_cast<float>(list_bottom - kButtonRowH - 8),
                           kBodyFontPx, th.dim);
        }
        // Task 5 draws the action row here, inside this scope (body_x and
        // list_bottom are in scope only here).
    }

    // ALWAYS LAST on every path. The Toast is NOT drawn here: main.cpp owns
    // the single app-wide Toast::render in the correct projection.
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::Btn1Yellow,
         overflow ? "Seasons \xE2\x86\x90" : "\xE2\x80\x94"},
        {chrome::HintIcon::Btn2Red, "Exit"},
        {chrome::HintIcon::Btn3Green,
         overflow ? "Seasons \xE2\x86\x92" : "\xE2\x80\x94"},
        {chrome::HintIcon::Btn4Black, "Back"},
        {chrome::HintIcon::RotaryNav, "\xE2\x80\x94"},
        {chrome::HintIcon::RotaryPress, "\xE2\x80\x94"},
    });
}

}  // namespace media_browser::ui
```

- [ ] **Step 4: Add the enum value now so the fall-through compiles**

In `src/media_browser/ui/mb_screen.h`, in the `Screen` enum after the line `Detail,`:

```cpp
    SeriesDetail,    // TV series detail (Phase 2c-2). Radarr-free mirror
                     // of Detail; reached only from Browse in TV mode.
```

The dispatch switch in main.cpp has no `default:`, so `-Wswitch` fires the moment the enum grows. That is the desired forcing function — **this task therefore ALSO adds the dispatch case as a pure no-crash stub** in `src/main.cpp`, in the screen-transition switch after the `case media_browser::ui::Screen::Detail:` line:

```cpp
                    case media_browser::ui::Screen::SeriesDetail:
                        // Instance + forwarding land with the Browse routing
                        // (2c-2 Task 4); unreachable until then.
                        active_mb_screen = &mb_browse;
                        break;
```

- [ ] **Step 5: CMake + Mac suite**

Add to `KIOSK_MEDIA_BROWSER_SOURCES` after `src/media_browser/ui/detail_screen.cpp`:

```cmake
            src/media_browser/ui/series_detail_screen.cpp
```

Run the Mac test loop. Expected: green, **+0 cases** (screen files are not in the Mac target — this proves nothing about the screen; the Pi gate below is the real check).

- [ ] **Step 6: Pi kiosk compile-verify**

Per Global Constraints: rsync the worktree's `magic_dingus_box_cpp/` to `magic@magicpi5.local:mdb-2c2/src/`, configure `-DBUILD_KIOSK=ON -DBUILD_TESTS=OFF -DENABLE_MEDIA_BROWSER=ON`, build with the `setsid` launch line, poll the log. Expected: `Built target magic_dingus_box_cpp`, 0 errors, no warnings naming `series_detail_*`, `mb_chrome*`, `detail_screen.cpp`, `mb_screen.h` or `main.cpp` beyond the known pre-existing set. **The `wrap_text` promotion is the specific thing to check for redefinition/ambiguity errors** — if detail_screen.cpp still compiles after Step 1c, the local copy really is gone.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/mb_chrome.h \
        magic_dingus_box_cpp/src/media_browser/ui/mb_chrome.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h \
        magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): SeriesDetailScreen read-only core + promote wrap_text to mb_chrome

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Registration + Browse routing

TV posters stop toasting "coming soon" and open the real screen.

**Files:**
- Modify: `src/media_browser/ui/browse_screen.cpp` (SELECT routing + footer hint)
- Modify: `src/main.cpp` (instance, real dispatch case, forwarding block)

**Interfaces:**
- Consumes: Task 3's class and `Screen::SeriesDetail`.
- Produces: nothing new on BrowseScreen. **There is no `selected_kind()` accessor** — the kind is already encoded in the `Screen` value the SELECT site returns, and a second, redundant source of truth for the same decision is one more thing to keep in sync.

- [ ] **Step 1: Browse routing**

In `src/media_browser/ui/browse_screen.cpp`, replace the 2c-1 TV SELECT guard — the block beginning `// 2c-1 ships discovery only. DetailScreen is Movie/Radarr-typed` through `return Screen::Detail;` inclusive — with:

```cpp
                // Movie and TV TMDB id spaces OVERLAP COMPLETELY (1396 is
                // Breaking Bad AND an unrelated film), so the id alone must
                // never choose the destination. The KIND picks it, and the
                // choice is carried in the returned Screen value — the
                // dispatcher needs no second accessor to re-derive it.
                selected_tmdb_id_ = hit.tmdb_id;
                return hit.kind == MediaKind::Tv ? Screen::SeriesDetail
                                                 : Screen::Detail;
```

And change the footer hint line `{chrome::HintIcon::RotaryPress, tv_mode() ? "Coming soon" : "Detail"},` to:

```cpp
        {chrome::HintIcon::RotaryPress, "Detail"},
```

(the three-line comment above it about "Coming soon" goes with it).

- [ ] **Step 2: main.cpp instance + forwarding**

After the `mb_detail` construction line (`media_browser::ui::DetailScreen     mb_detail(radarr, *tmdb, ...)`), add:

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
                    // Only Browse can produce this transition, and it only
                    // produces it for a TV hit — the kind lives in the
                    // transition itself, not in a BrowseScreen accessor.
                    if (current_mb_screen == media_browser::ui::Screen::Browse) {
                        mb_series_detail.set_tmdb_id(mb_browse.selected_tmdb_id());
                        mb_series_detail.set_origin(current_mb_screen);
                    }
                }
```

- [ ] **Step 3: Mac suite + Pi compile**

Mac loop (expect **+0 cases**, still green). Pi compile via `mdb-2c2` (incremental — main.cpp + browse_screen.cpp + the new screen). Expected: links clean, no new warnings.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp
git commit -m "feat(mb): route TV posters to SeriesDetailScreen

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---
### Task 5: Action row + add flows — "Add Season 1" and "Download next season"

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.h` / `.cpp`
- Modify: `src/media_browser/ui/series_detail_logic.h` (the action row's pure algebra — Step 0)
- Modify: `tests/media_browser/test_series_detail_logic.cpp` (its table tests — Step 0)
- Modify: `src/media_browser/sonarr/sonarr_client.cpp` (one entry-clear — Step 0)

**Interfaces:**
- Consumes: `SonarrClient::get_quality_profiles()`, `add_series(tmdb_id, quality_profile_id, monitor, title_fallback)`, `set_season_monitored`, `trigger_season_search`, `get_series`, `last_error()`; `record_refreshed`; Task 1's `next_unmonitored_season`.
- Produces in `series_detail_logic.h` (pure, Mac-tested; Tasks 6-7 name these enumerators): `enum class Action`, `struct ActionButton`, `canonical_action()`, `whole_series_label(bool, int64_t)`, `ActionRowInputs`, `ActionRow`, `decide_action_row()`.
- Produces in the screen (Tasks 6-7 extend these): `rebuild_buttons()` (a thin caller of `decide_action_row`), `dispatch_action()`, `spawn_mutation()`, `drain_mutation()`, `expire_confirms()`, and the members below.

**Verified API note.** `add_series(id, qp, /*monitor=*/true, title)` sends `addOptions.monitor = "firstSeason"` and `searchForMissingEpisodes = true` (sonarr_client.cpp, the `addOptions` block). `monitor=false` sends `"none"` with no search. The spec's wording is "Add Season 1"; `"firstSeason"` monitors the first AIRED season, which is not always numbered 1. Keep the spec's label — it is what the user means — but do not assume the flipped season is literally S1 anywhere in the logic.

**Written-once rules for this task.** `handle_input`, `rebuild_buttons`, `drain_mutation`, `spawn_mutation` and `expire_confirms` are written HERE in their **final** form, and Tasks 6–7 only add `dispatch_action` cases. That is why this task declares the confirm members (`whole_armed_`, `remove_pending_`, `navigate_back_`, …) even though Tasks 6–7 are what arm them: these five functions are the subtle ones, and editing them three times is how the focus/identity bugs got in.

**Known limitation (accepted).** `drain_mutation` runs from `update()`, and `main.cpp` calls `update()` only on the ACTIVE MB screen — so if the user leaves the Media Browser entirely while a mutation is finishing, the outcome toast is deferred until they next open SeriesDetail rather than lost. Two caveats make that acceptable rather than silent: the toast is title-prefixed (it still reads correctly whenever it lands), and `drain_mutation` logs an `spdlog::info` naming the dropped application when the identity gate fails, so a "my add didn't stick" report has something to read. The one case with no user-visible trace at all is quitting the kiosk mid-mutation: the destructor JOINS `mut_worker_` (up to ~14 s of shutdown stall) and the pending toast dies with the process. Fixing that properly means an app-level outcome queue, which is out of this phase's scope.

- [ ] **Step 0: The pure action-row core, its tests, and one client entry-clear**

The row's algebra — which buttons exist, their labels, and where focus lands — is pure decision logic and belongs in Task 1's header, not inside the kiosk-only screen TU. It is also the exact function whose focus bug was a plan-time Critical, so it gets table tests.

Append to `src/media_browser/ui/series_detail_logic.h`, at the end of `namespace media_browser::ui` (after `series_detail_state_message`):

```cpp
// ---------- Action row ----------

// The action row's vocabulary. It lives HERE rather than nested inside
// SeriesDetailScreen because the decision that BUILDS the row is pure (see
// decide_action_row below) and its table tests are Mac-side: the enum has to
// be reachable from a TU that cannot include a Renderer-bound header. Tasks
// 6-7's dispatch cases name these same enumerators.
enum class Action { AddSeason1, NextSeason, WholeSeries, Remove, ConfirmRemove };

struct ActionButton {
    Action action;
    std::string label;
};

// Remove and ConfirmRemove are ONE button in two states: every focus
// comparison canonicalizes, so arming the confirm cannot move focus off it.
inline Action canonical_action(Action a) {
    return a == Action::ConfirmRemove ? Action::Remove : a;
}

// The whole-series button's label, armed and unarmed.
//
// Binary GB (GiB) on purpose — it is the same unit the free-space readings
// arrive in, so the two numbers in the Block toast compare like with like.
// "(est)" is the honesty: pre-add the per-episode runtime is
// estimate_remaining_bytes' 45-minute ASSUMPTION, because Sonarr has no
// record to read runtime_minutes from yet.
inline std::string whole_series_label(bool armed, int64_t estimate_bytes) {
    if (!armed) return "Whole series\xE2\x80\xA6";
    return "Confirm ~" +
           std::to_string(estimate_bytes / (1024LL * 1024 * 1024)) +
           " GB (est)";
}

// Everything the action row is decided from. All of it is render-thread
// state on the screen; none of it is Renderer-shaped, which is the point.
struct ActionRowInputs {
    SeriesDetailState state = SeriesDetailState::Loading;
    // False for the window where Sonarr holds the record but has never
    // refreshed it — EVERY season reads unmonitored there, so the add
    // controls must not be offered.
    bool series_settled = true;
    // next_unmonitored_season(rows) — nullopt when everything is monitored.
    std::optional<int> next_unmonitored;
    bool remove_pending = false;
    bool whole_armed = false;
    int64_t whole_estimate_bytes = 0;
    // The action under the focus ring BEFORE this rebuild, if any. Focus is
    // preserved by ACTION IDENTITY, never by index.
    std::optional<Action> prev_focus_action;
};

struct ActionRow {
    std::vector<ActionButton> buttons;
    int focus = 0;
};

// The whole of the action row's algebra: which buttons exist, what they are
// labelled, and where focus lands.
//
// Focus is preserved by ACTION IDENTITY, never by index. A rebuild can
// insert, drop or relabel rows (an add turns "Add Season 1" into "Download
// Season 2" + "Remove"), and forcing focus to 0 meant the whole-series
// confirm was never the focused button — pressing SELECT inside the 4 s
// window fired "Add Season 1" instead. Deterministically.
//
// The no-match fallback biases AWAY from destructive actions: after an
// unsettled add the row is [Remove] alone for ~9 s, and a satisfied user
// tapping again should not find the delete button pre-focused unless it is
// genuinely the only thing on offer.
inline ActionRow decide_action_row(const ActionRowInputs& in) {
    ActionRow out;
    if (in.state == SeriesDetailState::NotInLibrary) {
        out.buttons.push_back({Action::AddSeason1, "Add Season 1"});
        out.buttons.push_back(
            {Action::WholeSeries,
             whole_series_label(in.whole_armed, in.whole_estimate_bytes)});
    } else if (in.state == SeriesDetailState::InLibrary) {
        // While the record is unsettled EVERY season reads unmonitored, so
        // next_unmonitored would answer "1" one second after we added
        // season 1 and the primary button would read "Download Season 1".
        // Offer Remove only until the poll settles it; the meta line says
        // "syncing…" so the missing controls read as pending, not broken.
        if (in.series_settled && in.next_unmonitored.has_value()) {
            out.buttons.push_back(
                {Action::NextSeason,
                 "Download Season " + std::to_string(*in.next_unmonitored)});
            out.buttons.push_back(
                {Action::WholeSeries,
                 whole_series_label(in.whole_armed, in.whole_estimate_bytes)});
        }
        out.buttons.push_back(
            in.remove_pending
                ? ActionButton{Action::ConfirmRemove, "Confirm Remove"}
                : ActionButton{Action::Remove, "Remove"});
    }
    // NOTE: there is deliberately NO "Working…" swap while a mutation is in
    // flight. Replacing the row wholesale destroyed focus identity and was
    // the root of the wrong-mutation bug; the screen dims the real row
    // instead, and SELECT is already gated on the in-flight flag.
    out.focus = 0;
    for (size_t i = 0; i < out.buttons.size(); ++i) {
        if (canonical_action(out.buttons[i].action) != Action::Remove) {
            out.focus = static_cast<int>(i);
            break;
        }
    }
    if (in.prev_focus_action.has_value()) {
        for (size_t i = 0; i < out.buttons.size(); ++i) {
            if (canonical_action(out.buttons[i].action) ==
                canonical_action(*in.prev_focus_action)) {
                out.focus = static_cast<int>(i);
                break;
            }
        }
    }
    if (out.focus >= static_cast<int>(out.buttons.size())) out.focus = 0;
    return out;
}
```

Append to `tests/media_browser/test_series_detail_logic.cpp` — **+8 TEST_CASE blocks** (settled add row, unsettled ⇒ [Remove] only, the settled in-library triple, arm/disarm keeping focus on the same canonical button, bias-away-from-Remove on a keep miss, the [Remove]-only fallback, the empty row in every non-actionable state, and the armed label carrying the estimate):

```cpp
// ---------- Action row ----------
//
// decide_action_row is the function whose focus algebra was a plan-time
// Critical (the whole-series confirm was never the focused button, so SELECT
// inside the 4 s window fired "Add Season 1"). It lived untested inside the
// kiosk-only screen TU until the Task-5 review; these cases pin the four
// rules it encodes — WHICH buttons, their LABELS, keep-focus-by-identity,
// and the bias away from Remove.

namespace {
ActionRowInputs row_inputs(SeriesDetailState st) {
    ActionRowInputs in;
    in.state = st;
    return in;
}
}  // namespace

TEST_CASE("action row: not-in-library offers the add pair, focused on add",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::NotInLibrary);
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 2);
    CHECK(row.buttons[0].action == Action::AddSeason1);
    CHECK(row.buttons[0].label == "Add Season 1");
    CHECK(row.buttons[1].action == Action::WholeSeries);
    CHECK(row.buttons[1].label == "Whole series\xE2\x80\xA6");
    CHECK(row.focus == 0);
}

TEST_CASE("action row: an UNSETTLED in-library record is [Remove] only",
          "[series_detail]") {
    // The window where Sonarr holds the record but has never refreshed it:
    // every season reads unmonitored, so next_unmonitored answers the first
    // season we JUST added. Offering "Download Season 1" one second after
    // adding season 1 is the bug this rule exists for.
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = false;
    in.next_unmonitored = 1;
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 1);
    CHECK(row.buttons[0].action == Action::Remove);
    CHECK(row.focus == 0);
}

TEST_CASE("action row: a settled in-library record offers next season + whole + remove",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = true;
    in.next_unmonitored = 2;
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 3);
    CHECK(row.buttons[0].action == Action::NextSeason);
    CHECK(row.buttons[0].label == "Download Season 2");
    CHECK(row.buttons[1].action == Action::WholeSeries);
    CHECK(row.buttons[2].action == Action::Remove);
    CHECK(row.focus == 0);  // never the destructive one

    // Everything monitored: the add controls hide, Remove stays.
    in.next_unmonitored = std::nullopt;
    const auto only_remove = decide_action_row(in);
    REQUIRE(only_remove.buttons.size() == 1);
    CHECK(only_remove.buttons[0].action == Action::Remove);
}

TEST_CASE("action row: arming a confirm keeps focus on the SAME canonical button",
          "[series_detail]") {
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = true;
    in.next_unmonitored = 2;  // row is [NextSeason, WholeSeries, Remove]

    // Remove -> ConfirmRemove: one button in two states, focus must not move.
    in.prev_focus_action = Action::Remove;
    in.remove_pending = true;
    const auto armed = decide_action_row(in);
    REQUIRE(armed.buttons.size() == 3);
    CHECK(armed.buttons[2].action == Action::ConfirmRemove);
    CHECK(armed.buttons[2].label == "Confirm Remove");
    CHECK(armed.focus == 2);

    // ...and back again when the confirm expires.
    in.prev_focus_action = Action::ConfirmRemove;
    in.remove_pending = false;
    const auto disarmed = decide_action_row(in);
    CHECK(disarmed.buttons[2].action == Action::Remove);
    CHECK(disarmed.focus == 2);

    // The whole-series arm is a relabel of the button the user pressed; the
    // 4 s confirm is unreachable if focus slides off it.
    in.prev_focus_action = Action::WholeSeries;
    in.whole_armed = true;
    in.whole_estimate_bytes = 12LL * 1024 * 1024 * 1024;
    const auto whole = decide_action_row(in);
    CHECK(whole.buttons[1].action == Action::WholeSeries);
    CHECK(whole.focus == 1);
}

TEST_CASE("action row: focus biases AWAY from Remove when the kept action is gone",
          "[series_detail]") {
    // The user pressed Add on a not-in-library page; the add landed and the
    // row became the in-library set. AddSeason1 no longer exists.
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = true;
    in.next_unmonitored = 2;
    in.prev_focus_action = Action::AddSeason1;
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 3);
    CHECK(row.focus == 0);
    CHECK(row.buttons[row.focus].action != Action::Remove);
}

TEST_CASE("action row: [Remove]-only falls back onto Remove itself",
          "[series_detail]") {
    // The bias has to yield when the destructive button is genuinely the
    // only thing on offer — otherwise focus points at nothing.
    auto in = row_inputs(SeriesDetailState::InLibrary);
    in.series_settled = false;
    in.next_unmonitored = 1;
    in.prev_focus_action = Action::AddSeason1;  // no match in the new row
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 1);
    CHECK(row.buttons[0].action == Action::Remove);
    CHECK(row.focus == 0);
}

TEST_CASE("action row: the non-actionable states have an EMPTY row",
          "[series_detail]") {
    // Loading / TmdbError / NotConfigured / SonarrUnreachable: no buttons at
    // all, so a SELECT is a structural no-op rather than a silent add.
    for (auto st : {SeriesDetailState::Loading, SeriesDetailState::TmdbError,
                    SeriesDetailState::NotConfigured,
                    SeriesDetailState::SonarrUnreachable}) {
        auto in = row_inputs(st);
        in.next_unmonitored = 1;
        in.prev_focus_action = Action::Remove;
        const auto row = decide_action_row(in);
        CHECK(row.buttons.empty());
        CHECK(row.focus == 0);
    }
}

TEST_CASE("action row: the armed whole-series label carries the GiB estimate",
          "[series_detail]") {
    CHECK(whole_series_label(false, 0) == "Whole series\xE2\x80\xA6");
    // Binary GB, and "(est)" because pre-add the runtime is an assumption.
    CHECK(whole_series_label(true, 42LL * 1024 * 1024 * 1024) ==
          "Confirm ~42 GB (est)");
    // Truncation, not rounding: 41.9 GiB must never read as "42 GB".
    CHECK(whole_series_label(true, 42LL * 1024 * 1024 * 1024 - 1) ==
          "Confirm ~41 GB (est)");
    // The label the row actually carries when armed.
    auto in = row_inputs(SeriesDetailState::NotInLibrary);
    in.whole_armed = true;
    in.whole_estimate_bytes = 7LL * 1024 * 1024 * 1024;
    const auto row = decide_action_row(in);
    REQUIRE(row.buttons.size() == 2);
    CHECK(row.buttons[1].label == "Confirm ~7 GB (est)");
}
```

**And one client fix, in the same step.** `SonarrClient::get_quality_profiles` is the only member of its family that never cleared `last_error_` on entry, which defeats the transport-vs-empty distinction this task's `AddSeason1` body depends on: a stale error from ANY earlier call would be echoed as "couldn't reach Sonarr — <unrelated>". In `src/media_browser/sonarr/sonarr_client.cpp`, make it match `add_series` / `set_season_monitored` / `trigger_season_search`:

```cpp
std::vector<QualityProfile> SonarrClient::get_quality_profiles() {
    // Without clearing first, an empty result here is ambiguous to callers
    // that read last_error() to tell "Sonarr answered, no profiles" from "we
    // never reached Sonarr" — a PRIOR call's error would be surfaced as if
    // it were this one's. Same entry clear as add_series /
    // set_season_monitored / trigger_season_search.
    set_error({});
    auto resp = http_get("/api/v3/qualityprofile");
    if (resp.empty()) return {};
    return SonarrParsers::parse_quality_profiles(resp);
}
```

- [ ] **Step 1: Header additions**

In `series_detail_screen.h`, in the private section directly after `void rebuild_rows();`:

```cpp
    // ---- action row (Tasks 5-7) ----
    // `Action` / `ActionButton` / `whole_series_label` and the row's whole
    // decision (decide_action_row) live in series_detail_logic.h: that
    // algebra is pure, it was the source of a wrong-button bug, and Mac
    // table tests cannot include this Renderer-bound header.
    void rebuild_buttons();             // thin caller of decide_action_row
    void dispatch_action(Action a);
    void expire_confirms();

    // ONE mutation at a time, on ONE reused worker thread (WatchdogSec=10:
    // add_series alone can take ~13.5 s — never on the render thread).
    // spawn_mutation joins the previous worker, wraps the body so mut_done_
    // flips on EVERY exit path including a throw, and catches
    // std::system_error from the thread ctor (a raw throw there is
    // std::terminate).
    void spawn_mutation(std::function<void()> body);
    void drain_mutation();

    std::vector<ActionButton> buttons_;
    int focus_ = 0;

    // Confirm state — render-thread ONLY. No worker ever writes these: the
    // press-1 worker publishes a verdict and drain_mutation arms the button,
    // so the countdown starts when the LABEL appears, and there is no
    // unsynchronized cross-thread write to a member render() reads.
    bool whole_armed_ = false;
    std::chrono::steady_clock::time_point whole_armed_at_{};
    int64_t whole_estimate_bytes_ = 0;
    static constexpr int kWholeConfirmMs = 4000;
    bool remove_pending_ = false;
    std::chrono::steady_clock::time_point remove_pending_at_{};
    static constexpr int kRemovePendingMs = 2000;
    // Drain-set, consumed by handle_input's relay at the top (DetailScreen's
    // drain_remove_result idiom). CLEARED when consumed and in fetch() —
    // a latched flag would return origin_ on every frame forever.
    bool navigate_back_ = false;

    std::thread mut_worker_;
    std::atomic<bool> mut_in_flight_{false};
    std::atomic<bool> mut_done_{false};
    // Which series this mutation was started for. Render-thread only
    // (written in spawn_mutation, read in drain_mutation): the user can back
    // out mid-add and open a different show, and an outcome must never
    // rewrite THAT page's state.
    int mut_tmdb_id_ = 0;
    // Which LOAD this mutation was started against. Same thread discipline
    // as mut_tmdb_id_, and it closes the A→B→A hole that the id alone
    // cannot see: leaving series A, opening B, then coming BACK to A passes
    // an id-only gate, and the refetch's pre-mutation library snapshot then
    // clobbers the drain's result (the page shows "Add Season 1" for a
    // series that is now in the library, sticky until you leave again).
    uint64_t mut_fetch_gen_ = 0;
    std::mutex mut_mtx_;
    std::string mut_toast_;                          // guarded by mut_mtx_
    std::optional<Series> mut_series_;               // guarded
    bool mut_settled_ = true;                        // guarded
    bool mut_removed_ = false;                       // guarded
    bool mut_have_verdict_ = false;                  // guarded
    DiskVerdict mut_verdict_ = DiskVerdict::Block;   // guarded
    int64_t mut_estimate_ = 0;                       // guarded
```

Add `#include <functional>` to the header's standard includes.

- [ ] **Step 2: fetch() clears the action row too**

In `series_detail_screen.cpp`'s `fetch()`, immediately after the line `season_page_count_ = 1;` and BEFORE Task 3's `last_poll_at_ = {};` line, add:

```cpp
    // With buttons_ cleared, a SELECT during Loading is a STRUCTURAL no-op —
    // there is nothing to dispatch. Before this, the previous series' stale
    // (and invisible) buttons still accepted SELECT and fired a silent add
    // with an empty title fallback.
    buttons_.clear();
    focus_ = 0;
    whole_armed_ = false;
    remove_pending_ = false;
    navigate_back_ = false;
```

Task 3's line stays exactly where it was, comment intact — the insertion goes ABOVE it, not through it:

```cpp
    // Task 8's poll gate must not inherit series A's timestamp — it would
    // delay series B's first poll by a full interval.
    last_poll_at_ = {};
```

- [ ] **Step 3: apply_pending rebuilds the row**

In `apply_pending()`, change the trailing `rebuild_rows();` to:

```cpp
    rebuild_rows();
    rebuild_buttons();
```

- [ ] **Step 4: Join the mutation worker in the dtor**

In `~SeriesDetailScreen()`, immediately after the two `fetch_add(1)` lines, add:

```cpp
    if (mut_worker_.joinable()) mut_worker_.join();
```

- [ ] **Step 5: Implement the machinery**

Add `#include "ui/toast.h"` to `series_detail_screen.cpp`'s includes — the screen now SHOWS toasts via `::ui::Toast::show`; it still never RENDERS them, because main.cpp owns the single app-wide draw. Then append to `series_detail_screen.cpp`, inside `namespace media_browser::ui`:

```cpp
void SeriesDetailScreen::rebuild_buttons() {
    // Thin caller: every decision (which buttons, their labels, and where
    // focus lands) is decide_action_row's, in series_detail_logic.h, under
    // Mac table tests. This function only marshals render-thread state into
    // ActionRowInputs and copies the answer back — the focus algebra it used
    // to inline was the exact thing that once fired the wrong mutation.
    ActionRowInputs in;
    in.state = decide_series_detail_state(
        SeriesDetailInputs{tmdb_done_, tmdb_ok_, sonarr_configured_,
                           sonarr_done_, sonarr_ok_, in_library_});
    in.series_settled = series_settled_;
    in.next_unmonitored = next_unmonitored_season(rows_);
    in.remove_pending = remove_pending_;
    in.whole_armed = whole_armed_;
    in.whole_estimate_bytes = whole_estimate_bytes_;
    if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size()))
        in.prev_focus_action = buttons_[static_cast<size_t>(focus_)].action;

    ActionRow row = decide_action_row(in);
    buttons_ = std::move(row.buttons);
    focus_ = row.focus;
}

void SeriesDetailScreen::spawn_mutation(std::function<void()> body) {
    if (mut_in_flight_.load()) return;               // one at a time
    if (mut_worker_.joinable()) mut_worker_.join();  // reap the finished one
    // A poll already in flight is stale by definition once a mutation
    // starts: if it published between this mutation's drain and the next
    // apply_pending, its PRE-mutation snapshot would overwrite the
    // mutation's result — for remove, permanently (the page would show a
    // removed series as in-library and never recover). One bump closes it.
    poll_gen_.fetch_add(1);
    mut_tmdb_id_ = tmdb_id_;
    // Stamp the LOAD as well as the id. The id alone cannot see A→B→A: back
    // on series A, a refetch has already republished A's PRE-mutation
    // library snapshot, and an id-only gate lets the drain apply the
    // mutation's result on top of — or be overwritten by — that stale load.
    // The observed symptom was "Add Season 1" on a series that had just been
    // added, sticky until the user visited a different series.
    mut_fetch_gen_ = fetch_gen_.load();
    mut_in_flight_.store(true);
    mut_done_.store(false);
    const std::string title =
        detail_.has_value() ? detail_->title : std::string("This series");
    try {
        mut_worker_ = std::thread([this, body = std::move(body), title]() {
            // Flips mut_done_ on EVERY exit path, exception included. Without
            // it a throw out of body() leaves mut_in_flight_ stuck true and
            // the action row inert for the rest of the session — and an
            // uncaught throw out of the thread is std::terminate.
            struct DoneGuard {
                std::atomic<bool>& flag;
                ~DoneGuard() { flag.store(true, std::memory_order_release); }
            } guard{mut_done_};
            try {
                body();
            } catch (const std::exception& e) {
                spdlog::warn("[SeriesDetail] mutation threw: {}", e.what());
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_toast_ = title +
                             ": something went wrong \xE2\x80\x94 try again";
            }
        });
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] mutation spawn failed: {}", e.what());
        mut_in_flight_.store(false);
        ::ui::Toast::show("Couldn't start the operation \xE2\x80\x94 try again");
        return;
    }
    rebuild_buttons();  // row persists; render() dims it while in flight
}

void SeriesDetailScreen::drain_mutation() {
    if (!mut_done_.load(std::memory_order_acquire)) return;
    // ALWAYS clear the in-flight state, whatever the outcome and whichever
    // series is on screen now. A drain that bailed early on an identity
    // mismatch used to leave mut_in_flight_ latched forever.
    mut_done_.store(false);
    mut_in_flight_.store(false);

    std::string toast;
    std::optional<Series> fresh;
    bool fresh_settled = true;
    bool removed = false;
    bool have_verdict = false;
    DiskVerdict verdict = DiskVerdict::Block;
    int64_t estimate = 0;
    {
        std::lock_guard<std::mutex> lk(mut_mtx_);
        toast = std::move(mut_toast_);
        mut_toast_.clear();
        fresh = std::move(mut_series_);
        mut_series_.reset();
        fresh_settled = mut_settled_;
        mut_settled_ = true;
        removed = mut_removed_;
        mut_removed_ = false;
        have_verdict = mut_have_verdict_;
        mut_have_verdict_ = false;
        verdict = mut_verdict_;
        estimate = mut_estimate_;
        mut_estimate_ = 0;
    }

    // The toast shows REGARDLESS of which page we are on: every worker
    // composes it title-prefixed ("Breaking Bad: Season 1 search started"),
    // so an outcome that lands after the user moved on is still meaningful
    // instead of silently lost.
    if (!toast.empty()) ::ui::Toast::show(toast);

    // Everything that MUTATES THIS PAGE is gated on identity: the SAME
    // series AND the same load of it (A→B→A refetches A, so the id matches
    // again while the page state underneath is a fresh pre-mutation
    // snapshot).
    if (mut_tmdb_id_ != tmdb_id_ || mut_fetch_gen_ != fetch_gen_.load()) {
        // The toast above already told the user what happened; the page
        // state deliberately does not move. Leave a trace so a "my add
        // didn't stick" report has something to read: the alternative is a
        // silently dropped application with no record anywhere.
        spdlog::info("[SeriesDetail] dropping mutation result for tmdb:{} "
                     "(gen {}) — page now tmdb:{} (gen {})",
                     mut_tmdb_id_, mut_fetch_gen_, tmdb_id_,
                     fetch_gen_.load());
        rebuild_buttons();
        return;
    }
    if (have_verdict && verdict != DiskVerdict::Block) {
        // Armed HERE, on the render thread — the 4 s window starts when the
        // label appears, not when a worker finished computing free space.
        whole_estimate_bytes_ = estimate;
        whole_armed_ = true;
        whole_armed_at_ = std::chrono::steady_clock::now();
    }
    if (fresh.has_value()) {
        series_ = std::move(fresh);
        series_settled_ = fresh_settled;
        in_library_ = true;
        sonarr_done_ = sonarr_ok_ = true;
        rebuild_rows();
    }
    if (removed) {
        series_.reset();
        in_library_ = false;
        series_settled_ = true;
        rebuild_rows();
        navigate_back_ = true;
    }
    last_poll_at_ = {};  // Task 8: refresh badges next frame, not in 9 s
    rebuild_buttons();
    // Deliberately NO focus_on(WholeSeries) here: the identity-preserving
    // rebuild already keeps focus on the button the user pressed, and if
    // they rotated away during the free-space fetch, yanking focus back
    // would be the one place it moves without being asked. The armed label
    // + Warn color are the signal.
}

void SeriesDetailScreen::expire_confirms() {
    const auto now = std::chrono::steady_clock::now();
    bool changed = false;
    if (whole_armed_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - whole_armed_at_).count() > kWholeConfirmMs) {
        whole_armed_ = false;
        changed = true;
    }
    if (remove_pending_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - remove_pending_at_).count() > kRemovePendingMs) {
        remove_pending_ = false;
        changed = true;
    }
    if (changed) rebuild_buttons();
}

void SeriesDetailScreen::dispatch_action(Action a) {
    // Pressing anything OTHER than the armed control disarms it first.
    if (a != Action::WholeSeries) whole_armed_ = false;
    if (a != Action::Remove && a != Action::ConfirmRemove) remove_pending_ = false;
    switch (a) {
        case Action::AddSeason1: {
            const int id = tmdb_id_;
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            spawn_mutation([this, id, title]() {
                // Quality profile BY NAME ("Any" is this box's profile; the
                // id is not portable) — DetailScreen::pick_quality_profile_id's
                // policy minus its movie-only fallbacks.
                const auto profiles = sonarr_.get_quality_profiles();
                int qp_id = 0;
                for (const auto& qp : profiles) {
                    if (qp_id == 0) qp_id = qp.id;
                    if (qp.name == "Any") { qp_id = qp.id; break; }
                }
                if (qp_id == 0) {
                    // Distinguish "Sonarr answered, no profiles" from "we
                    // never reached Sonarr" — last_error() is set on every
                    // transport failure, so an empty vector alone is
                    // ambiguous and blaming the config would be wrong.
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = err.empty()
                        ? title + ": Sonarr has no quality profile \xE2\x80\x94 not added"
                        : title + ": couldn't reach Sonarr \xE2\x80\x94 " + err;
                    return;
                }
                // monitor=true => addOptions.monitor="firstSeason" +
                // searchForMissingEpisodes=true: exactly the spec's
                // season-at-a-time default, applied by Sonarr itself.
                auto res = sonarr_.add_series(id, qp_id, /*monitor=*/true, title);
                if (!res.ok) {
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = title + ": add failed \xE2\x80\x94 " + err;
                    return;
                }
                if (!res.settled) {
                    // settled==false: seasons[] is EMPTY by contract. The
                    // page keeps rendering TMDB rows and hides the add
                    // controls until the Task-8 poll settles the record.
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_series_ = res.series;
                    mut_settled_ = false;
                    mut_toast_ =
                        title + ": added \xE2\x80\x94 syncing seasons\xE2\x80\xA6";
                    return;
                }
                // ---- settled: did the add actually DO anything? ----
                // add_series returns ok=true from its find-existing branch
                // without applying addOptions, monitoring anything or
                // searching. That branch dedupes by tvdbId while this screen
                // detects in-library by tmdbId, so a library record with
                // tmdb_id == 0 still offers "Add Season 1" — and the press
                // would toast "Season 1 search started" having done nothing
                // at all. Read the outcome off the returned record instead
                // of trusting ok=true.
                //
                // The season we check is the LOWEST NON-SPECIAL season
                // NUMBER, never the literal 1: addOptions.monitor =
                // "firstSeason" monitors the first AIRED season, which is
                // not always numbered 1 (and season 0 is specials).
                const int sid = res.series.sonarr_id;
                int first_season = 0;
                bool first_monitored = false;
                int first_files = 0;
                for (const auto& s : res.series.seasons) {
                    if (s.season_number <= 0) continue;
                    if (first_season != 0 && s.season_number >= first_season)
                        continue;
                    first_season = s.season_number;
                    first_monitored = s.monitored;
                    first_files = s.episode_file_count;
                }
                std::string toast;
                std::optional<Series> fresh;
                if (first_season == 0 || sid <= 0) {
                    // A settled record with no ordinary season, or with no
                    // id to act on: nothing to verify, so claim nothing.
                    toast = title + ": added to your TV library";
                } else if (!first_monitored) {
                    // The idempotent branch (or any drift): the add did NOT
                    // apply monitoring, so do it explicitly and report THOSE
                    // outcomes — same shape as the NextSeason flow.
                    if (!sonarr_.set_season_monitored(sid, first_season, true)) {
                        const std::string err = sonarr_.last_error();
                        std::lock_guard<std::mutex> lk(mut_mtx_);
                        mut_toast_ = title + ": couldn't monitor season " +
                                     std::to_string(first_season) +
                                     " \xE2\x80\x94 " + err;
                        return;
                    }
                    const bool searched =
                        sonarr_.trigger_season_search(sid, first_season);
                    fresh = sonarr_.get_series(sid);
                    toast = searched
                        ? title + ": Season " + std::to_string(first_season) +
                              " search started"
                        : title + ": Season " + std::to_string(first_season) +
                              " monitored, but the search didn't start "
                              "\xE2\x80\x94 Sonarr will pick it up on RSS";
                } else if (first_files > 0) {
                    // Monitored AND already has files: the box already had
                    // this series and nothing was started, so say that
                    // rather than promising a search.
                    toast = title + ": already in your TV library";
                } else {
                    // Monitored with nothing on disk yet — the add path
                    // genuinely acted (or a prior add did) and
                    // searchForMissingEpisodes rode in with monitor=true.
                    toast = title + ": Season 1 search started";
                }
                std::lock_guard<std::mutex> lk(mut_mtx_);
                if (fresh.has_value()) {
                    mut_settled_ = record_refreshed(*fresh);
                    mut_series_ = std::move(fresh);
                } else {
                    mut_series_ = res.series;
                    mut_settled_ = true;
                }
                mut_toast_ = std::move(toast);
            });
            break;
        }
        case Action::NextSeason: {
            if (!series_.has_value() || series_->sonarr_id <= 0) break;
            const auto next = next_unmonitored_season(rows_);
            if (!next.has_value()) break;
            const int sid = series_->sonarr_id;
            const int season = *next;
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            spawn_mutation([this, sid, season, title]() {
                if (!sonarr_.set_season_monitored(sid, season, true)) {
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = title + ": couldn't monitor season " +
                                 std::to_string(season) + " \xE2\x80\x94 " + err;
                    return;
                }
                const bool searched = sonarr_.trigger_season_search(sid, season);
                auto fresh = sonarr_.get_series(sid);
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_toast_ = searched
                    ? title + ": Season " + std::to_string(season) +
                          " search started"
                    : title + ": Season " + std::to_string(season) +
                          " monitored, but the search didn't start "
                          "\xE2\x80\x94 Sonarr will pick it up on RSS";
                if (fresh.has_value()) {
                    mut_settled_ = record_refreshed(*fresh);
                    mut_series_ = std::move(fresh);
                }
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

- [ ] **Step 6: Input — replace `handle_input` wholesale, in final form**

Replace the whole of `SeriesDetailScreen::handle_input` (Task 3's version) with this. It is DetailScreen's structure line for line: the drain relay at the top, `SETTINGS_MENU`/`SELECT` gated on `e.pressed`, and rotary gated on **`e.delta != 0` — never `e.pressed`**, because `platform::InputEvent` (input_manager.h) carries `pressed=false` for the encoder's EV_REL events, so an `if (!e.pressed) continue;` filter kills rotary navigation outright.

```cpp
Screen SeriesDetailScreen::handle_input(
        const std::vector<platform::InputEvent>& events) {
    // Async completion relay, FIRST — DetailScreen's drain_remove_result
    // shape. Consuming (and clearing) the flag here is what keeps one remove
    // from bricking the screen: a latched flag would return origin_ forever.
    if (navigate_back_) {
        navigate_back_ = false;
        return origin_;
    }
    for (const auto& e : events) {
        // BTN4 (SETTINGS_MENU, black) — back to whoever opened us.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return origin_;
        }
        // Rotary twist. platform::InputEvent has no `value` field — the
        // direction/magnitude is `delta`, and rotary events carry
        // pressed=false, so delta is the ONLY correct gate.
        if ((e.action == platform::InputAction::ROTATE ||
             e.action == platform::InputAction::ROTATE_VERTICAL) &&
            e.delta != 0) {
            // Focus is FROZEN while THIS series' mutation runs: render()
            // hides the focus ring for exactly that window, so any movement
            // would be invisible, and the post-drain rebuild would then land
            // focus on a button the user never saw themselves select.
            const bool busy = mut_in_flight_.load() && mut_tmdb_id_ == tmdb_id_;
            if (buttons_.empty() || busy) continue;
            const int n = static_cast<int>(buttons_.size());
            // Clamp, do not wrap — DetailScreen's exact idiom, so the ends
            // of the row feel like ends rather than teleporting focus.
            focus_ = std::clamp(focus_ + e.delta, 0, n - 1);
            // Any navigation cancels BOTH pending confirms, so the user can
            // never press-move-press their way into a mutation they were not
            // looking at.
            if (whole_armed_ || remove_pending_) {
                whole_armed_ = false;
                remove_pending_ = false;
                rebuild_buttons();
            }
            continue;
        }
        // BTN1 / BTN3 page the season list when it overflows.
        if (e.action == platform::InputAction::PREV && e.pressed) {
            if (season_page_ > 0) --season_page_;
            continue;
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            if (season_page_ + 1 < season_page_count_) ++season_page_;
            continue;
        }
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (buttons_.empty()) continue;
            // The gate is GLOBAL (one mutation worker) but the dim is
            // series-scoped, so series B's row looks perfectly live while
            // series A's mutation finishes — up to ~14 s of presses landing
            // on nothing. A silent no-op reads as a dead button; say it.
            if (mut_in_flight_.load()) {
                ::ui::Toast::show("Still finishing the last action\xE2\x80\xA6");
                continue;
            }
            if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size()))
                dispatch_action(buttons_[static_cast<size_t>(focus_)].action);
            continue;
        }
        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal in
        // main.cpp. It never reaches here; no per-screen handler needed.
    }
    return Screen::SeriesDetail;
}
```

And replace `SeriesDetailScreen::update` with:

```cpp
void SeriesDetailScreen::update() {
    drain_mutation();
    expire_confirms();
    apply_pending();
}
```

(The `#include "ui/toast.h"` this task's code needs was added in Step 5.)

- [ ] **Step 7: Render the action row**

In `render()`, **inside the `else` block** — this is the only scope where `body_x` and `list_bottom` exist — immediately after the `if (overflow) { … }` paging-indicator block, add:

```cpp
        // Action row. Its height was already reserved out of the season
        // list's budget above (kButtonRowH), so it can never overlap a row.
        if (!buttons_.empty()) {
            // SERIES-SCOPED: another series' still-finishing mutation must
            // not gray THIS page's row. SELECT stays globally gated (one
            // worker), so a press in that window is ignored briefly (≤14 s
            // worst case) — never misapplied.
            const bool busy = mut_in_flight_.load() && mut_tmdb_id_ == tmdb_id_;
            int bx = body_x;
            const int brow_y = list_bottom - kButtonRowH;
            const int row_right = screen_w - chrome::kSafeInset_px;
            int drawn = 0;
            for (size_t i = 0; i < buttons_.size(); ++i) {
                // Defensive clamp: stop before a button would cross the safe
                // inset rather than running it under the cabinet art. The
                // motivating canvas is 640x480 (CRT_NATIVE), where three
                // buttons plus a long "Confirm ~N GB (est)" label are not
                // guaranteed to fit; Task 9's acceptance eyeballs the row on
                // the real box. Width is predicted with draw_button's own
                // geometry (kBtnFontPx 18 + kBtnPadX 18 a side, mb_chrome.cpp)
                // — worst case a drift there clamps one button early, which
                // is still strictly better than drawing it where nobody can
                // see it.
                const int bw = r.mb_text_width(buttons_[i].label,
                                               kButtonFontPx) +
                               2 * kButtonPadX_px;
                if (drawn > 0 && bx + bw > row_right) break;
                chrome::ButtonKind kind = chrome::ButtonKind::Ok;
                if (buttons_[i].action == Action::Remove ||
                    buttons_[i].action == Action::ConfirmRemove ||
                    (buttons_[i].action == Action::WholeSeries && whole_armed_)) {
                    kind = chrome::ButtonKind::Warn;
                } else if (buttons_[i].action == Action::WholeSeries) {
                    kind = chrome::ButtonKind::Action;
                }
                // While a mutation runs the row stays put with its labels
                // unchanged and simply loses its focus ring — it reads as
                // "busy" without destroying focus identity. SELECT is
                // already gated on !mut_in_flight_ in handle_input.
                const auto rect = chrome::draw_button(
                    r, bx, brow_y, buttons_[i].label, kind,
                    /*focused=*/!busy && static_cast<int>(i) == focus_);
                bx = rect.x + rect.w + chrome::kPad3;
                ++drawn;
            }
            // A focused button that was never drawn is an invisible
            // affordance: SELECT would fire something the user cannot see.
            // Clamp onto the last button that actually made it onto the
            // screen (render() already clamps season_page_ the same way).
            if (focus_ >= drawn) focus_ = drawn > 0 ? drawn - 1 : 0;
        }
```

The clamp needs draw_button's metrics, which live in mb_chrome.cpp's anonymous namespace. Mirror them into `series_detail_screen.cpp`'s own anonymous namespace beside `kButtonRowH`:

```cpp
// chrome::draw_button's OWN geometry (mb_chrome.cpp's kBtnFontPx /
// kBtnPadX), mirrored here so the action row can predict a button's width
// BEFORE drawing it and stop at the safe inset. draw_button reports its rect
// only after it has already painted, which is too late to decline.
constexpr int kButtonFontPx = 18;
constexpr int kButtonPadX_px = 18;
```

And update the footer hints' last two entries so the rotary pair dims when there is no row:

```cpp
        {chrome::HintIcon::RotaryNav,
         buttons_.empty() ? "\xE2\x80\x94" : "Choose"},
        {chrome::HintIcon::RotaryPress,
         buttons_.empty() ? "\xE2\x80\x94" : "Select"},
```

- [ ] **Step 8: Mac suite + Pi compile**

Mac loop: **+8 cases** from Step 0's TEST_CASE blocks (the screen itself is kiosk-only and contributes none), still green. Pi incremental build in `mdb-2c2`: links clean, no new warnings.

- [ ] **Step 9: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_logic.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_client.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_series_detail_logic.cpp
git commit -m "feat(mb): SeriesDetail action row + add flows (Season 1, next season)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: "Whole series…" — armed confirm + the blocking preflight

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.cpp` (dispatch case only — the members, the label helper, the arming and the expiry all landed in Task 5)

**Interfaces:**
- Consumes: Task 1's `estimate_remaining_bytes`, `whole_series_verdict`, `DiskVerdict`; `SonarrClient::get_root_folders()` (`RootFolder::free_space_bytes`, radarr_types.h:104), `get_quality_profiles`, `add_series`, `set_season_monitored`, `trigger_series_search`, `get_series`, `last_error`; `std::filesystem::space` fallback on `/mnt/ssd/library/tv`.

**Why armed-button, not a modal:** the codebase's confirm idiom is a button-label swap with expiry (DetailScreen's Remove → "Confirm Remove", 2 s; QueueScreen's cancel, 2 s). The spec asks for "a confirm modal with disk estimate"; the estimate goes IN the armed label, which satisfies the requirement inside the established idiom — no screen in the codebase owns a modal, and 2c-1 documented why input-swallowing overlays are dangerous to bolt on. 4 s (not 2) because the user is reading a number.

- [ ] **Step 1: Implement the dispatch case**

Add `#include <filesystem>` to `series_detail_screen.cpp`'s includes. Replace the `case Action::WholeSeries:` line in `dispatch_action`'s trailing `case Action::WholeSeries: case Action::Remove: case Action::ConfirmRemove: break;` group with the full case below (leaving `case Action::Remove:` / `case Action::ConfirmRemove:` sharing the `break;` for Task 7):

```cpp
        case Action::WholeSeries: {
            if (whole_armed_) {
                // ---- press 2: execute ----
                whole_armed_ = false;
                const bool pre_add = !in_library_;
                const int id = tmdb_id_;
                const std::string title =
                    detail_.has_value() ? detail_->title
                                        : std::string("This series");
                const int sid = series_.has_value() ? series_->sonarr_id : 0;
                std::vector<int> to_monitor;
                for (const auto& row : rows_) {
                    if (!row.monitored) to_monitor.push_back(row.season_number);
                }
                spawn_mutation([this, pre_add, id, title, sid, to_monitor]() {
                    int series_id = sid;
                    std::optional<Series> added;
                    if (pre_add) {
                        const auto profiles = sonarr_.get_quality_profiles();
                        int qp_id = 0;
                        for (const auto& qp : profiles) {
                            if (qp_id == 0) qp_id = qp.id;
                            if (qp.name == "Any") { qp_id = qp.id; break; }
                        }
                        if (qp_id == 0) {
                            const std::string err = sonarr_.last_error();
                            std::lock_guard<std::mutex> lk(mut_mtx_);
                            mut_toast_ = err.empty()
                                ? title + ": Sonarr has no quality profile "
                                          "\xE2\x80\x94 not added"
                                : title + ": couldn't reach Sonarr \xE2\x80\x94 " + err;
                            return;
                        }
                        // *** monitor=true is REQUIRED here. *** add_series
                        // writes the SERIES-LEVEL monitored flag from this
                        // same parameter (series["monitored"] = monitor) and
                        // no client method exists to flip it afterwards.
                        // "none" would leave the series permanently
                        // unmonitored, and Sonarr's
                        // MonitoredEpisodeSpecification rejects every release
                        // for an unmonitored series: seasons monitored,
                        // search runs, NOTHING ever downloads — invisibly.
                        //
                        // The firstSeason-vs-our-PUTs race this used to fear
                        // is provably over when settled==true: add_settled
                        // (monitor=true) requires the applied monitoring
                        // state to have been OBSERVED. The race exists only
                        // on the timeout path — which is why the season PUTs
                        // below are gated on res.settled.
                        auto res = sonarr_.add_series(id, qp_id,
                                                      /*monitor=*/true, title);
                        if (!res.ok) {
                            const std::string err = sonarr_.last_error();
                            std::lock_guard<std::mutex> lk(mut_mtx_);
                            mut_toast_ = title + ": add failed \xE2\x80\x94 " + err;
                            return;
                        }
                        if (!res.settled) {
                            // firstSeason has NOT been applied yet; seasons
                            // PUT now would be unmonitored behind us when it
                            // lands. Stop honestly: the row shows [Remove]
                            // only until the poll settles, then "Whole
                            // series…" reappears and the retry takes the
                            // in-library path — idempotent by design.
                            std::lock_guard<std::mutex> lk(mut_mtx_);
                            mut_series_ = res.series;
                            mut_settled_ = false;
                            mut_toast_ = title + ": added \xE2\x80\x94 syncing seasons; "
                                         "the whole-series option returns when "
                                         "Sonarr finishes";
                            return;
                        }
                        // Settled: S1 is already monitored and the add-time
                        // search already covers it (searchForMissingEpisodes
                        // rode in with monitor=true); S1's entry in
                        // to_monitor is a harmless idempotent PUT, and the
                        // series search below may re-query S1 — redundant,
                        // not harmful.
                        series_id = res.series.sonarr_id;
                        added = res.series;
                    }
                    if (series_id <= 0) {
                        std::lock_guard<std::mutex> lk(mut_mtx_);
                        if (added.has_value()) {
                            mut_series_ = std::move(added);
                            mut_settled_ = false;
                        }
                        mut_toast_ = title + ": added, but Sonarr hasn't assigned "
                                     "an id yet \xE2\x80\x94 try Whole series again "
                                     "in a moment";
                        return;
                    }
                    // For an ANNOUNCED series Sonarr may not know every
                    // season yet, so some of these PUTs legitimately fail.
                    // They are counted, not hidden, and the toast below is
                    // composed from the real count.
                    int failed = 0;
                    for (int season : to_monitor) {
                        if (!sonarr_.set_season_monitored(series_id, season, true))
                            ++failed;
                    }
                    const bool searched = sonarr_.trigger_series_search(series_id);
                    auto fresh = sonarr_.get_series(series_id);
                    const int total = static_cast<int>(to_monitor.size());
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    if (fresh.has_value()) {
                        mut_settled_ = record_refreshed(*fresh);
                        mut_series_ = std::move(fresh);
                    } else if (added.has_value()) {
                        mut_series_ = std::move(added);
                        mut_settled_ = false;
                    }
                    // Composed from BOTH signals — the failure count AND the
                    // search outcome. Never claim "Monitored" or promise RSS
                    // when every PUT failed (Sonarr stopped mid-flow): the
                    // series is in the library with nothing monitored and
                    // nothing will ever arrive.
                    if (total > 0 && failed == total) {
                        mut_toast_ = title + ": couldn't monitor seasons "
                                     "\xE2\x80\x94 is Sonarr running?";
                    } else if (!searched) {
                        mut_toast_ = title + ": seasons monitored, but the search "
                                     "didn't start \xE2\x80\x94 Sonarr will pick "
                                     "them up on RSS";
                    } else if (failed > 0) {
                        mut_toast_ = title + ": search started (" +
                                     std::to_string(failed) + " of " +
                                     std::to_string(total) +
                                     " seasons couldn't be monitored)";
                    } else {
                        mut_toast_ = title + ": whole-series search started";
                    }
                });
                break;
            }
            // ---- press 1: estimate + free space + verdict, off-thread ----
            // The multiplicand: in-library uses Sonarr's real per-episode
            // runtime; PRE-ADD there is no record, so estimate_remaining_bytes
            // falls back to 45 minutes. We deliberately do NOT call
            // lookup_by_tmdb to fetch the real runtime first: that would give
            // find_series_by_tvdb's mock family its first indirect kiosk
            // surface AND add a round-trip to a gesture that already waits.
            // The armed label says "(est)" precisely because of this.
            const int runtime = (in_library_ && series_.has_value())
                                    ? series_->runtime_minutes : 0;
            const int64_t estimate =
                estimate_remaining_bytes(rows_, runtime, mb_per_min_);
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            // Immediate feedback: the free-space fetch can take the full 5 s
            // HTTP timeout, and the Allow path's only signal is the armed
            // label appearing — a user who glances away would otherwise read
            // press-1 as "the button did nothing".
            ::ui::Toast::show(title + ": checking free space\xE2\x80\xA6");
            spawn_mutation([this, estimate, title]() {
                // Free space, two sources with DIFFERENT zero semantics:
                //
                //  - Sonarr's root folder: freeSpace is parsed with a 0
                //    default, so an ABSENT/null field (Sonarr can't stat the
                //    folder — the stale-container-bind state that
                //    magic-dingus-storage-attach.service exists for) is
                //    indistinguishable from a genuinely full disk. A 0 here
                //    is therefore AMBIGUOUS and must fall through, or a
                //    healthy box with 400 GB free gets a false "0 GB free"
                //    Block with a wrong diagnosis.
                //  - std::filesystem::space on the host path: failure is a
                //    distinct error code, so a returned 0 is a REAL full
                //    disk and whole_series_verdict correctly Blocks on it.
                //
                // Do not "simplify" the two guards into one.
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
                    auto info = std::filesystem::space("/mnt/ssd/library/tv", ec);
                    if (!ec) free_bytes = static_cast<int64_t>(info.available);
                }
                const DiskVerdict v = whole_series_verdict(estimate, free_bytes);
                const auto gb = [](int64_t b) {
                    return std::to_string(b / (1024LL * 1024 * 1024)) + " GB";
                };
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_have_verdict_ = true;
                mut_verdict_ = v;
                mut_estimate_ = estimate;
                switch (v) {
                    case DiskVerdict::Block:
                        // The codebase's FIRST blocking preflight: publish a
                        // Block verdict so drain_mutation does NOT arm, and
                        // show both numbers plus the floor.
                        mut_toast_ = title + ": not enough space \xE2\x80\x94 needs ~" +
                                     gb(estimate) + " (est), " +
                                     gb(free_bytes.value_or(0)) +
                                     " free (20 GB floor)";
                        break;
                    case DiskVerdict::WarnOnly:
                        mut_toast_ = title + ": couldn't check free space "
                                     "\xE2\x80\x94 confirm to proceed anyway";
                        break;
                    case DiskVerdict::Allow:
                        break;  // the armed label IS the feedback
                }
            });
            break;
        }
```

Nothing else changes: `whole_series_label(armed, estimate)` (series_detail_logic.h, called by `decide_action_row`) already renders the armed text from `ActionRowInputs::whole_armed` / `whole_estimate_bytes`, `drain_mutation` already arms the button from the published verdict, `expire_confirms` already reverts it after 4 s, and the render loop already picks `ButtonKind::Warn` for an armed WholeSeries.

- [ ] **Step 2: Mac suite + Pi compile + commit**

Mac loop **+0 cases** green; Pi incremental clean. Then:

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp
git commit -m "feat(mb): whole-series add behind the blocking disk preflight

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Orphan-proof remove

> **Revised 2026-08-01 (review fixes, commit on top of `5453391`).** The first
> implementation removed the series even when the seeding-torrent lookup had
> FAILED — `get_series_download_hashes` returns `{}` for both "no history"
> and "the GET died", and the flow could not tell them apart, so a Sonarr blip
> orphaned every seeding torrent forever under a toast that said "removed".
> That, the untested dedupe, and five smaller honesty/state defects are folded
> in below. Steps 1-2 are the new prerequisites; step 3 is the rewritten worker.

**Files:**
- Modify: `src/media_browser/ui/series_detail_logic.h` (the cancel-id decision, pure)
- Modify: `tests/media_browser/test_series_detail_logic.cpp` (**+4 cases**)
- Modify: `src/media_browser/sonarr/sonarr_client.cpp` (entry clear — one line + rationale)
- Modify: `src/media_browser/ui/series_detail_screen.cpp` (dispatch cases + `drain_mutation`)

**Interfaces:**
- Consumes: `SonarrClient::get_queue()` (per-EPISODE rows; `SonarrQueueItem::id`, `series_id`, `download_id`, `title`), `cancel_queue_item(queue_id)`, `get_series_download_hashes(sonarr_id)`, `remove_series(sonarr_id, delete_files)`, `last_error()`; `QbittorrentClient::delete_torrent(const std::string& hash, bool delete_files)` — the exact call DetailScreen's `run_remove` step 2 makes (detail_screen.cpp:1060).
- Produces: `cancel_ids_for_series` — the first piece of this screen's remove path that is testable off-Pi.

- [ ] **Step 1: The cancel-id decision moves into the pure core, with tests**

The dedupe was written inline in the worker, where its most important property
— a 13-row season pack yields **one** cancel, not 13 — could not be asserted at
all. Add to `series_detail_logic.h`, immediately above `// ---------- Screen state ----------`:

```cpp
// ---------- Remove ----------

// Queue rows -> the exact ids to cancel for one series, deduped by
// download_id (a season pack's siblings 404 by design after the first
// cancel). Rows with an empty download_id fall back to q.title as the
// dedupe key (documented identical across a pack's rows) rather than
// fanning out per-row.
//
// The dedupe is what makes the remove flow's abort guard trustworthy.
// Sonarr's queue is per EPISODE while cancel_queue_item acts on the WHOLE
// download, so cancelling row 1 of a 13-episode pack makes rows 2-13 404 BY
// DESIGN (sonarr_client.h). Counting those 404s as failures aborted the
// remove AFTER the torrent was already gone, leaving the series record and
// its files behind under a "NOT removed" toast that was itself a lie.
//
// A row with NEITHER a download_id nor a title has no key at all, so it is
// taken as-is — the committed loop's behaviour for an unkeyed row. Collapsing
// several of those onto one empty key would silently skip real cancels.
inline std::vector<int> cancel_ids_for_series(
        const std::vector<SonarrQueueItem>& queue, int sonarr_series_id) {
    std::vector<int> ids;
    std::unordered_set<std::string> seen;
    for (const auto& q : queue) {
        if (q.series_id != sonarr_series_id) continue;
        const std::string& key = q.download_id.empty() ? q.title : q.download_id;
        if (key.empty()) {
            ids.push_back(q.id);  // nothing to dedupe on
            continue;
        }
        if (seen.insert(key).second) ids.push_back(q.id);
    }
    return ids;
}
```

`SonarrQueueItem` already arrives via the file's existing
`#include "media_browser/sonarr/sonarr_types.h"`, and `<unordered_set>` /
`<vector>` are already included — no new includes.

Then in `tests/media_browser/test_series_detail_logic.cpp`, above
`TEST_CASE("series detail state resolver: precedence and copy", ...)`:

```cpp
// ---------- Remove: cancel-id selection ----------
//
// The decision that decides how many cancels the orphan-proof remove issues.
// It lived inline in the kiosk-only screen TU, where its most important
// property — a 13-row season pack yields ONE cancel, not 13 — could not be
// asserted at all. Over-cancelling is not cosmetic: sibling rows 404 by
// design after the first cancel, and counting those 404s as failures aborted
// the remove AFTER the torrent was gone.

namespace {
SonarrQueueItem q_row(int id, int series_id, std::string download_id,
                      std::string title = {}) {
    SonarrQueueItem q;
    q.id = id;
    q.series_id = series_id;
    q.download_id = std::move(download_id);
    q.title = std::move(title);
    return q;
}
}  // namespace

TEST_CASE("cancel ids: a 13-row season pack yields exactly ONE cancel",
          "[series_detail]") {
    std::vector<SonarrQueueItem> queue;
    for (int i = 0; i < 13; ++i) {
        queue.push_back(q_row(100 + i, 7, "ABCDEF", "Breaking.Bad.S02.1080p"));
    }
    const auto ids = cancel_ids_for_series(queue, 7);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 100);  // the FIRST row of the pack, in queue order
}

TEST_CASE("cancel ids: an empty download_id dedupes on the release title",
          "[series_detail]") {
    // Mixed rows: a populated-id pack, plus rows whose download_id never made
    // it into the queue payload but which share one release title (the field
    // is documented identical across a pack's rows). Fanning those out
    // per-row would fire N cancels at ONE download and count N-1 by-design
    // 404s as failures.
    std::vector<SonarrQueueItem> queue = {
        q_row(1, 7, "HASH1", "Show.S01.1080p"),
        q_row(2, 7, "HASH1", "Show.S01.1080p"),
        q_row(3, 7, "", "Show.S03.720p"),
        q_row(4, 7, "", "Show.S03.720p"),
        q_row(5, 7, "", "Show.S03.720p"),
    };
    const auto ids = cancel_ids_for_series(queue, 7);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 3);
}

TEST_CASE("cancel ids: rows for OTHER series are excluded", "[series_detail]") {
    // The queue is global. Cancelling another series' download while removing
    // this one is destructive and silent — the user asked to delete one show.
    std::vector<SonarrQueueItem> queue = {
        q_row(1, 7, "MINE", "Mine.S01"),
        q_row(2, 9, "THEIRS", "Theirs.S01"),
        q_row(3, 9, "", "Theirs.S02"),
        q_row(4, 7, "MINE", "Mine.S01"),
    };
    const auto ids = cancel_ids_for_series(queue, 7);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 1);
    CHECK(cancel_ids_for_series(queue, 9).size() == 2);
    CHECK(cancel_ids_for_series(queue, 404).empty());
    CHECK(cancel_ids_for_series({}, 7).empty());
}

TEST_CASE("cancel ids: empty download_ids with DISTINCT titles stay separate",
          "[series_detail]") {
    // The title fallback must not over-collapse: these are three genuinely
    // different downloads, and skipping two of them would leave live torrents
    // running for a series the user just deleted.
    std::vector<SonarrQueueItem> queue = {
        q_row(1, 7, "", "Show.S01E01.720p"),
        q_row(2, 7, "", "Show.S01E02.720p"),
        q_row(3, 7, "", "Show.S01E03.720p"),
    };
    const auto ids = cancel_ids_for_series(queue, 7);
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == 1);
    CHECK(ids[2] == 3);
    // Degenerate: no download_id AND no title is unkeyed, so each row is
    // taken as-is rather than collapsing onto one empty key.
    std::vector<SonarrQueueItem> unkeyed = {q_row(8, 7, "", ""),
                                            q_row(9, 7, "", "")};
    CHECK(cancel_ids_for_series(unkeyed, 7).size() == 2);
}
```

- [ ] **Step 2: `get_series_download_hashes` must clear `last_error()` on entry**

Without it the remove flow below cannot distinguish its two empty returns, and
the abort in step 3 is either spurious (a stale error from an earlier call) or
missing entirely (a fresh failure that reads clean). This task originally
replaced the function with an entry-cleared `set_error({}); ... if
(resp.empty()) return {};` body — the sibling idiom already used by
`cancel_queue_item`, `get_quality_profiles`, `add_series`,
`set_season_monitored` and `trigger_season_search`.

> **Superseded by Task 8 (carry-forward from this task's re-review).** Entry
> clear alone was the right instinct but the wrong tool: "empty +
> `last_error()` clean" is a decision split across TWO calls, and Task 8 adds
> a background thread (the ~9 s re-poll) that shares this client and its ONE
> `last_error_` member. A poll finishing in the gap between step 3's
> `get_series_download_hashes` call and its `last_error()` read can set (or
> clear) that member from an UNRELATED request, turning the remove worker's
> abort decision into a race — the exact class of bug this step exists to
> prevent, just moved one layer up. Task 8 therefore replaces the function
> above with a CHECKED variant, contract-identical to `get_library_checked` /
> `find_series_by_tvdb`: nullopt vs. an engaged-but-empty vector, decided
> entirely within ONE call so there is nothing left to race. This is the
> code now in the tree, in both files:

`src/media_browser/sonarr/sonarr_client.h`, replacing the single
`get_series_download_hashes` declaration:

```cpp
    // Distinct downloadIds from this series' history, lowercased for direct
    // comparison with QbittorrentClient (which stores hashes lowercase).
    // Feeds the orphan-proof remove: the queue only knows in-progress
    // downloads, so finished-and-seeding torrents would otherwise survive a
    // series deletion.
    //
    // CHECKED shape on purpose, like get_library_checked / find_series_by_tvdb:
    // nullopt means the service did not answer (transport/HTTP failure), an
    // engaged-but-empty vector means Sonarr answered and this series
    // genuinely has no download history. The orphan-proof remove worker
    // branches on exactly that distinction — engaged proceeds, nullopt
    // aborts before anything is deleted — and it must read the answer from
    // THIS single return rather than a follow-up last_error() call: Task 8
    // adds a ~9 s background re-poll that shares this client and its one
    // last_error_ member, so a decision split across two calls could
    // observe an error the POLL thread set (or cleared) in between, not
    // this call's own outcome.
    virtual std::optional<std::vector<std::string>>
    get_series_download_hashes_checked(int sonarr_id);

    // Bare wrapper — nullopt collapses to {}, same relationship as
    // get_library() has to get_library_checked(). Do NOT use this to decide
    // whether a failure occurred; see get_series_download_hashes_checked.
    virtual std::vector<std::string> get_series_download_hashes(int sonarr_id);
```

`src/media_browser/sonarr/sonarr_client.cpp`:

```cpp
std::optional<std::vector<std::string>>
SonarrClient::get_series_download_hashes_checked(int sonarr_id) {
    // Entry clear, same shape as cancel_queue_item / get_quality_profiles.
    // Load-bearing here, not merely tidy: http_get returns "" both on a
    // transport failure (curl error, HTTP >= 400 — set_error was called)
    // and would return "" on nothing else, since /api/v3/history/series
    // answers a real 200 with at least "[]". Without the clear, a PRIOR
    // call's stale error state would linger uninspected; callers of THIS
    // method never read last_error() at all — the nullopt/engaged split
    // below is the whole answer, by design (see the header's doc comment
    // for why: Task 8's background re-poll shares this client's one
    // last_error_ member with the orphan-proof remove worker).
    set_error({});
    // /api/v3/history/series is UNPAGINATED (a bare array) — no pageSize
    // parameter, unlike Radarr's /api/v3/history.
    auto resp = http_get("/api/v3/history/series?seriesId="
                         + std::to_string(sonarr_id));
    if (resp.empty()) return std::nullopt;  // transport/HTTP failure — NOT "no history"
    return SonarrParsers::parse_history_download_ids(resp);
}

std::vector<std::string> SonarrClient::get_series_download_hashes(int sonarr_id) {
    return get_series_download_hashes_checked(sonarr_id)
        .value_or(std::vector<std::string>{});
}
```

`SonarrMockClient` overrides `get_series_download_hashes_checked` too (the
mock overrides EVERY public virtual, by contract): unlike
`get_library_checked`, this mock has no field-observed reachability lie to
defend against here, so the override is ENGAGED and delegates to the same
fixture-consistent computation its raw override already had — see Task 8's
carry-forward step for the exact body and its Mac test.

> **Same ruling, applied to the two DELETEs (Task 8 review fix).** `remove_series`
> and `cancel_queue_item` decided the same way — `set_error({}); http_delete(...);
> return last_error().empty();` — and are on the same shared `last_error_`. Both
> directions of that race are live once the re-poll runs Sonarr HTTP
> concurrently: a poll failure mid-window false-FAILS a cancel that succeeded
> (aborting the remove with a lying toast), and a poll's entry-clear landing
> before `remove_series`' read false-SUCCEEDS a failed DELETE — "removed" toast,
> series record still in Sonarr, torrents already purged by step 2. Fix is the
> same shape: the private `http_delete` returns the HTTP STATUS CODE (0 =
> transport failure), `last_error` side effects untouched, and both callers
> branch in-band on `code > 0 && code < 400`. No public signature changes, so
> `SonarrMockClient` is unaffected. This is the code now in the tree:

`src/media_browser/sonarr/sonarr_client.h`:

```cpp
    // Returns the HTTP STATUS CODE, not the body — 0 means the request never
    // got an answer (transport failure). DELETE endpoints answer with an empty
    // body on success, so the body cannot distinguish success from failure and
    // the old "did last_error() stay empty?" read is a cross-thread split read
    // now that Task 8's background re-poll shares this client's one
    // last_error_ member. Callers branch on `code > 0 && code < 400`.
    // last_error side effects are unchanged: still set on transport failure
    // and on HTTP >= 400, so the UI keeps its message.
    virtual long http_delete(const std::string& path);
```

`src/media_browser/sonarr/sonarr_client.cpp`:

```cpp
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    // 0 is the ONE reserved "no answer" value: a transport failure never
    // produced a status line, so callers can treat it as "this DELETE did not
    // happen" without consulting last_error() across a thread boundary.
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return 0; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        set_error(os.str());
    }
    return http_code;
}
```

```cpp
    // The verdict is IN-BAND. Reading last_error() back would be a split read
    // across threads: Task 8's background re-poll drives this same client and
    // clears last_error_ on entry, so a poll landing between the DELETE and
    // the read reports a FAILED remove as a success — "removed" toast, series
    // record still in Sonarr, and the torrents already purged by step 2. The
    // status code belongs to this call alone.
    const long code = http_delete(path);
    return code > 0 && code < 400;
}
```

```cpp
bool SonarrClient::cancel_queue_item(int queue_id) {
    set_error({});
    // In-band verdict, same reason as remove_series: last_error() is shared
    // with the background re-poll, and the other direction of that race is
    // just as live — a poll's Sonarr failure landing mid-window would fail a
    // cancel that actually succeeded, aborting the remove with a lying toast.
    const long code = http_delete("/api/v3/queue/" + std::to_string(queue_id)
                                  + "?removeFromClient=true&blocklist=false");
    return code > 0 && code < 400;
}
```

The seven `StubSonarr`-family overrides in
`tests/media_browser/test_sonarr_client.cpp` move to
`long http_delete(...) { return 200; }` (200 preserves the old "no error ⇒
success" reading); the two that record `delete_path` keep recording it, and
the `remove_series` / `cancel_queue_item` cases still assert the same paths.
`RadarrClient::http_delete` is deliberately left on the old shape — NOT
because no concurrent poll exists (DetailScreen::run_library_poll DOES
overlap run_remove), but because Radarr's poll path never entry-clears
`last_error_` (`get_movie`/`get_queue` carry no `set_error({})`), so only
the safe false-FAILURE direction is reachable there; the destructive
false-SUCCESS cannot occur. Recorded as a pre-existing issue, out of this
plan's scope.

- [ ] **Step 3: The confirm swap + the remove worker**

Replace `dispatch_action`'s trailing `case Action::Remove: case Action::ConfirmRemove: break;` with:

```cpp
        case Action::Remove:
            remove_pending_ = true;
            remove_pending_at_ = std::chrono::steady_clock::now();
            rebuild_buttons();
            break;
        case Action::ConfirmRemove: {
            remove_pending_ = false;
            if (!series_.has_value() || series_->sonarr_id <= 0) {
                // Without this the label stayed on "Confirm Remove" with
                // nothing behind it: every further press silently fell out of
                // the switch, which reads as a dead button. Repaint the row
                // (remove_pending_ is already cleared, so it reverts to
                // "Remove") and say why, in the whole-series sibling's copy.
                rebuild_buttons();
                ::ui::Toast::show("series id unknown \xE2\x80\x94 try again "
                                  "once syncing finishes");
                break;
            }
            const int sid = series_->sonarr_id;
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            spawn_mutation([this, sid, title]() {
                // Sonarr-shaped mirror of DetailScreen::run_remove:
                //  1. Cancel in-flight downloads — ONCE PER DOWNLOAD, not
                //     once per queue row (cancel_ids_for_series does that
                //     dedupe; it is pure and Mac-tested).
                //  2. Purge every torrent the series' history knows about
                //     (catches finished+seeding ones step 1 misses).
                //  3. remove_series(delete_files=true).
                //  4. Back to origin (drained on the render thread).
                const std::vector<int> cancel_ids =
                    cancel_ids_for_series(sonarr_.get_queue(), sid);
                int cancel_failed = 0;
                int cancel_ok = 0;
                std::string cancel_err;
                for (int qid : cancel_ids) {
                    if (sonarr_.cancel_queue_item(qid)) {
                        ++cancel_ok;
                        continue;
                    }
                    ++cancel_failed;
                    // Captured AT the failing iteration. last_error() is
                    // cleared on entry to every client call, so a LATER
                    // success wipes the diagnosis and the abort toast below
                    // degrades to "NOT removed" with no reason attached.
                    if (cancel_err.empty()) cancel_err = sonarr_.last_error();
                }
                if (cancel_failed > 0) {
                    // A genuinely failed cancel still aborts before anything
                    // is deleted — the house rule that keeps a half-removed
                    // series from orphaning a torrent. But cancels use
                    // removeFromClient=true, so any that SUCCEEDED before the
                    // failure have already taken their downloads with them:
                    // saying "couldn't cancel" flat would imply nothing
                    // happened, and the user would not know data is gone.
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ =
                        (cancel_ok > 0
                             ? title + ": cancelled " + std::to_string(cancel_ok) +
                                   " download(s), then failed \xE2\x80\x94 series "
                                   "NOT removed; retry is safe"
                             : title + ": couldn't cancel " +
                                   std::to_string(cancel_failed) +
                                   " download(s) \xE2\x80\x94 NOT removed") +
                        (cancel_err.empty() ? std::string()
                                            : " (" + cancel_err + ")");
                    return;
                }
                if (qbit_ != nullptr) {
                    // The history walk runs BEFORE the decision to proceed,
                    // because a FAILED walk is indistinguishable from "no
                    // history" by its return value alone: both are {}. Removing
                    // on a failed walk is the exact production failure
                    // DetailScreen documents — every seeding torrent orphaned
                    // forever, under a toast that said "removed".
                    // get_series_download_hashes clears last_error() on entry
                    // (sonarr_client.cpp), which is what makes empty + clean
                    // mean "genuinely nothing to purge" and empty + error mean
                    // "we never reached Sonarr".
                    //
                    // Aborting here is retry-safe: the cancel stage above is
                    // idempotent — an already-cancelled download leaves no
                    // queue row, so its ids simply dedupe to nothing next time.
                    //
                    // Still gated on qbit_ as before: a box with no qBittorrent
                    // client cannot purge anything at all, so it keeps the old
                    // semantics and accepts the orphan risk BY CONSTRUCTION.
                    const std::vector<std::string> hashes =
                        sonarr_.get_series_download_hashes(sid);
                    if (hashes.empty() && !sonarr_.last_error().empty()) {
                        std::lock_guard<std::mutex> lk(mut_mtx_);
                        mut_toast_ = title + ": couldn't check for seeding "
                                     "torrents \xE2\x80\x94 series NOT removed";
                        return;
                    }
                    for (const auto& h : hashes) {
                        if (!qbit_->delete_torrent(h, /*delete_files=*/true)) {
                            spdlog::warn("[SeriesDetail] qbit delete failed for {}",
                                         h);
                        }
                    }
                }
                if (!sonarr_.remove_series(sid, /*delete_files=*/true)) {
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = title + ": remove failed \xE2\x80\x94 " + err;
                    return;
                }
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_removed_ = true;
                mut_toast_ = title + ": removed from the TV library";
            });
            break;
        }
```

> **Superseded by Task 8 (carry-forward from this task's re-review).** Two
> defects survived review: (a) the "series id unknown" toast above is the
> ONLY toast on this screen with no title prefix — every sibling toast on
> this row (add, next season, whole series, and every OTHER branch of this
> same remove worker) is `title + ": " + ...`; (b) the hashes stage's
> `hashes.empty() && !sonarr_.last_error().empty()` check reads `last_error()`
> in a SEPARATE call from the one that produced it, which Task 8's
> background re-poll (sharing this client's one `last_error_` member) turns
> into a race — see Step 2's note above for the mechanism. Task 8 fixes both;
> this is the code now in the tree:

```cpp
        case Action::ConfirmRemove: {
            remove_pending_ = false;
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            if (!series_.has_value() || series_->sonarr_id <= 0) {
                // Without this the label stayed on "Confirm Remove" with
                // nothing behind it: every further press silently fell out of
                // the switch, which reads as a dead button. Repaint the row
                // (remove_pending_ is already cleared, so it reverts to
                // "Remove") and say why, title-prefixed like every sibling
                // toast on this screen.
                rebuild_buttons();
                ::ui::Toast::show(title + ": series id unknown \xE2\x80\x94 "
                                  "try again once syncing finishes");
                break;
            }
            const int sid = series_->sonarr_id;
            spawn_mutation([this, sid, title]() {
                // Sonarr-shaped mirror of DetailScreen::run_remove:
                //  1. Cancel in-flight downloads — ONCE PER DOWNLOAD, not
                //     once per queue row (cancel_ids_for_series does that
                //     dedupe; it is pure and Mac-tested).
                //  2. Purge every torrent the series' history knows about
                //     (catches finished+seeding ones step 1 misses).
                //  3. remove_series(delete_files=true).
                //  4. Back to origin (drained on the render thread).
                const std::vector<int> cancel_ids =
                    cancel_ids_for_series(sonarr_.get_queue(), sid);
                int cancel_failed = 0;
                int cancel_ok = 0;
                std::string cancel_err;
                for (int qid : cancel_ids) {
                    if (sonarr_.cancel_queue_item(qid)) {
                        ++cancel_ok;
                        continue;
                    }
                    ++cancel_failed;
                    // Captured AT the failing iteration. last_error() is
                    // cleared on entry to every client call, so a LATER
                    // success wipes the diagnosis and the abort toast below
                    // degrades to "NOT removed" with no reason attached.
                    if (cancel_err.empty()) cancel_err = sonarr_.last_error();
                }
                if (cancel_failed > 0) {
                    // A genuinely failed cancel still aborts before anything
                    // is deleted — the house rule that keeps a half-removed
                    // series from orphaning a torrent. But cancels use
                    // removeFromClient=true, so any that SUCCEEDED before the
                    // failure have already taken their downloads with them:
                    // saying "couldn't cancel" flat would imply nothing
                    // happened, and the user would not know data is gone.
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ =
                        (cancel_ok > 0
                             ? title + ": cancelled " + std::to_string(cancel_ok) +
                                   " download(s), then failed \xE2\x80\x94 series "
                                   "NOT removed; retry is safe"
                             : title + ": couldn't cancel " +
                                   std::to_string(cancel_failed) +
                                   " download(s) \xE2\x80\x94 NOT removed") +
                        (cancel_err.empty() ? std::string()
                                            : " (" + cancel_err + ")");
                    return;
                }
                if (qbit_ != nullptr) {
                    // The history walk runs BEFORE the decision to proceed,
                    // because a FAILED walk is indistinguishable from "no
                    // history" by an unchecked return alone. Removing on a
                    // failed walk is the exact production failure
                    // DetailScreen documents — every seeding torrent orphaned
                    // forever, under a toast that said "removed".
                    //
                    // CHECKED variant, deliberately not the raw one + a
                    // follow-up last_error() read: Task 8 adds a ~9 s
                    // background re-poll on its own thread that shares this
                    // SonarrClient (and its one last_error_ member). A
                    // decision split across two calls — get the hashes, THEN
                    // separately ask last_error() — could read whatever the
                    // poll thread set or cleared in the gap, not this call's
                    // own outcome. nullopt IS the whole answer here; nothing
                    // else needs to be read to make the call.
                    //
                    // Aborting here is retry-safe: the cancel stage above is
                    // idempotent — an already-cancelled download leaves no
                    // queue row, so its ids simply dedupe to nothing next time.
                    //
                    // Still gated on qbit_ as before: a box with no qBittorrent
                    // client cannot purge anything at all, so it keeps the old
                    // semantics and accepts the orphan risk BY CONSTRUCTION.
                    const std::optional<std::vector<std::string>> hashes =
                        sonarr_.get_series_download_hashes_checked(sid);
                    if (!hashes.has_value()) {
                        std::lock_guard<std::mutex> lk(mut_mtx_);
                        // Cancel-stage context: any downloads cancelled
                        // before this abort ARE gone (removeFromClient=true
                        // in the cancel loop above), so a flat "couldn't
                        // check, NOT removed" would understate what already
                        // happened — same honesty rule as the cancel-failure
                        // toast just above.
                        mut_toast_ =
                            (cancel_ok > 0
                                 ? title + ": cancelled " +
                                       std::to_string(cancel_ok) +
                                       " download(s), then couldn't check "
                                       "for seeding torrents \xE2\x80\x94 "
                                       "series NOT removed"
                                 : title + ": couldn't check for seeding "
                                           "torrents \xE2\x80\x94 series NOT "
                                           "removed") +
                            " \xE2\x80\x94 Sonarr didn't answer";
                        return;
                    }
                    for (const auto& h : *hashes) {
                        if (!qbit_->delete_torrent(h, /*delete_files=*/true)) {
                            spdlog::warn("[SeriesDetail] qbit delete failed for {}",
                                         h);
                        }
                    }
                }
                if (!sonarr_.remove_series(sid, /*delete_files=*/true)) {
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = title + ": remove failed \xE2\x80\x94 " + err;
                    return;
                }
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_removed_ = true;
                mut_toast_ = title + ": removed from the TV library";
            });
            break;
        }
```

**Binding failure semantics (embodied above):**

| stage | outcome | what the user is told | what actually happened |
|---|---|---|---|
| cancel | all succeed (incl. a pack deduped to 1) | — | proceed |
| cancel | first N succeed, then one fails | `"cancelled N download(s), then failed — series NOT removed; retry is safe"` | those N downloads ARE gone (`removeFromClient=true`); the series is not |
| cancel | the first one fails | `"couldn't cancel N download(s) — NOT removed"` | nothing deleted |
| history | engaged (even empty) | — | the series genuinely has no history; proceed |
| history | nullopt, no prior cancels | `"<title>: couldn't check for seeding torrents — series NOT removed — Sonarr didn't answer"` | **abort**; nothing deleted, retry is safe |
| history | nullopt, N cancels already succeeded | `"<title>: cancelled N download(s), then couldn't check for seeding torrents — series NOT removed — Sonarr didn't answer"` | **abort**; those N downloads ARE gone, the series record is not |
| history | (no qBit client) | — | stage skipped entirely; a box with no qBittorrent accepts the orphan risk BY CONSTRUCTION |
| remove | `remove_series` fails | `"remove failed — <err>"` | torrents purged, series record survives |

Retry-safety is what licenses the abort: the cancel stage is idempotent, because
an already-cancelled download leaves no queue row and therefore contributes no
id on the next pass.

*(Table updated by Task 8's carry-forward: the history row now reads
`get_series_download_hashes_checked`'s nullopt/engaged split instead of
`last_error()` — see Step 2/3's superseding notes above and Task 8's own
carry-forward step below for why.)*

- [ ] **Step 4: `drain_mutation`'s removed-path hardening**

Two additions, both in `drain_mutation`. First, inside the identity-gate-FAILED
branch, after the `spdlog::info` and before `rebuild_buttons()`:

```cpp
        // A dropped `removed` is the ONE outcome that outlives the page it
        // was started from: the Sonarr record is gone for good. Without this
        // flag, re-entering that same tmdb_id hits set_tmdb_id's same-id
        // no-op and enter()'s short-circuit, and the page repaints its cached
        // pre-remove snapshot — a "Remove" button aimed at a record Sonarr no
        // longer knows about. DetailScreen's drain_remove_result sets exactly
        // this for exactly this reason.
        if (removed) needs_refresh_ = true;
```

Second, in the `removed` branch, beside `series_.reset()`:

```cpp
    if (removed) {
        series_.reset();
        // Empty today; Task 8's queue poll makes it load-bearing — a stale
        // downloading set would paint Downloading badges on the rows of a
        // series that no longer exists during the frame before navigate_back_
        // is consumed.
        downloading_seasons_.clear();
        in_library_ = false;
        series_settled_ = true;
        rebuild_rows();
```

Nothing else changes: `decide_action_row` (fed `remove_pending_` by `rebuild_buttons`) already swaps Remove → "Confirm Remove" and canonicalizes the two into one button for focus purposes, `expire_confirms` already reverts it after 2 s, `drain_mutation` already clears state and sets `navigate_back_` off `mut_removed_`, and `handle_input`'s top-of-function relay already consumes that flag and returns `origin_`.

- [ ] **Step 5: Mac suite + Pi compile + commit**

Mac loop **+4 cases** green (Step 1's four `TEST_CASE` blocks); Pi incremental clean.

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_logic.h \
        magic_dingus_box_cpp/tests/media_browser/test_series_detail_logic.cpp \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_client.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp
git commit -m "feat(mb): orphan-proof series remove

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: Downloading badges + quiet re-poll

> **Carries forward three fixes from Task 7's re-review, all caused by THIS
> task's own poll.** Running Sonarr HTTP on a ~9 s timer, on its own thread,
> sharing the client's ONE `last_error_` member with every other caller, makes
> `last_error()` decision-critical across threads for the first time in this
> screen — Task 7's orphan-proof remove worker read it in a call split from
> the one that produced it, which is exactly the shape that now races. Step 0
> below lands the fix (a checked variant, contract-identical to
> `get_library_checked`) before the poll that creates the race is even wired
> up, plus two smaller copy fixes the same review found.

**Files:**
- Modify: `src/media_browser/ui/series_detail_screen.h` / `.cpp`
- Modify: `src/media_browser/sonarr/sonarr_client.h` / `.cpp` (Step 0 — the checked hashes variant)
- Modify: `src/media_browser/sonarr/sonarr_mock.h` / `.cpp` (Step 0 — matching mock override)
- Modify: `tests/media_browser/test_sonarr_client.cpp` (Step 0 — **+2 cases**, pinning the checked variant's contract on both the real client and the mock)

**Interfaces:**
- Consumes: `SonarrClient::get_series(sonarr_id)`, `get_queue()`, `get_series_download_hashes_checked(sonarr_id)`, `record_refreshed`; DetailScreen's `maybe_repoll_library` idiom (single reused thread, gen counter, inflight flag, 9 s cadence).
- Produces: live `downloading_seasons_` driving `SeasonState::Downloading`, post-add settle — an unsettled add's statistics arrive, `series_settled_` flips true, and the add controls appear without any user action — and (Step 0) `SonarrClient::get_series_download_hashes_checked`, the reachability-honest shape Task 7's remove worker now consumes instead of a raced `last_error()` read.

- [ ] **Step 0: The checked hashes variant + two copy fixes (carried forward from Task 7's re-review)**

`src/media_browser/sonarr/sonarr_client.h` — replaces the single
`get_series_download_hashes` declaration Task 7 added (Task 7 Step 2 above
now carries the full before/after and rationale; this is the declaration
landing in the header):

```cpp
    // Distinct downloadIds from this series' history, lowercased for direct
    // comparison with QbittorrentClient (which stores hashes lowercase).
    // Feeds the orphan-proof remove: the queue only knows in-progress
    // downloads, so finished-and-seeding torrents would otherwise survive a
    // series deletion.
    //
    // CHECKED shape on purpose, like get_library_checked / find_series_by_tvdb:
    // nullopt means the service did not answer (transport/HTTP failure), an
    // engaged-but-empty vector means Sonarr answered and this series
    // genuinely has no download history. The orphan-proof remove worker
    // branches on exactly that distinction — engaged proceeds, nullopt
    // aborts before anything is deleted — and it must read the answer from
    // THIS single return rather than a follow-up last_error() call: Task 8
    // adds a ~9 s background re-poll that shares this client and its one
    // last_error_ member, so a decision split across two calls could
    // observe an error the POLL thread set (or cleared) in between, not
    // this call's own outcome.
    virtual std::optional<std::vector<std::string>>
    get_series_download_hashes_checked(int sonarr_id);

    // Bare wrapper — nullopt collapses to {}, same relationship as
    // get_library() has to get_library_checked(). Do NOT use this to decide
    // whether a failure occurred; see get_series_download_hashes_checked.
    virtual std::vector<std::string> get_series_download_hashes(int sonarr_id);
```

`src/media_browser/sonarr/sonarr_client.cpp` — the raw function now
delegates:

```cpp
std::optional<std::vector<std::string>>
SonarrClient::get_series_download_hashes_checked(int sonarr_id) {
    // Entry clear, same shape as cancel_queue_item / get_quality_profiles.
    // Load-bearing here, not merely tidy: http_get returns "" both on a
    // transport failure (curl error, HTTP >= 400 — set_error was called)
    // and would return "" on nothing else, since /api/v3/history/series
    // answers a real 200 with at least "[]". Without the clear, a PRIOR
    // call's stale error state would linger uninspected; callers of THIS
    // method never read last_error() at all — the nullopt/engaged split
    // below is the whole answer, by design (see the header's doc comment
    // for why: Task 8's background re-poll shares this client's one
    // last_error_ member with the orphan-proof remove worker).
    set_error({});
    // /api/v3/history/series is UNPAGINATED (a bare array) — no pageSize
    // parameter, unlike Radarr's /api/v3/history.
    auto resp = http_get("/api/v3/history/series?seriesId="
                         + std::to_string(sonarr_id));
    if (resp.empty()) return std::nullopt;  // transport/HTTP failure — NOT "no history"
    return SonarrParsers::parse_history_download_ids(resp);
}

std::vector<std::string> SonarrClient::get_series_download_hashes(int sonarr_id) {
    return get_series_download_hashes_checked(sonarr_id)
        .value_or(std::vector<std::string>{});
}
```

`src/media_browser/sonarr/sonarr_mock.h` — the mock overrides EVERY public
virtual by contract, so it gains the matching override:

```cpp
    // Engaged, not nullopt: unlike get_library_checked, this mock has no
    // specific reason to answer "unreachable" here, and it delegates to the
    // same fixture-consistent computation as the raw variant just below.
    std::optional<std::vector<std::string>>
    get_series_download_hashes_checked(int sonarr_id) override;
    std::vector<std::string> get_series_download_hashes(int sonarr_id) override;
```

`src/media_browser/sonarr/sonarr_mock.cpp`:

```cpp
std::optional<std::vector<std::string>>
SonarrMockClient::get_series_download_hashes_checked(int sonarr_id) {
    // Engaged, always: the mock has no transport to fail, and (unlike
    // get_library_checked) there is no field-observed bug this method needs
    // to defend against by claiming unreachable. Same fixture-consistent
    // computation as the raw variant below, so both stay coherent.
    std::vector<std::string> out;
    for (const auto& q : queue_) {
        if (q.series_id != sonarr_id || q.download_id.empty()) continue;
        if (std::find(out.begin(), out.end(), q.download_id) == out.end()) {
            out.push_back(q.download_id);
        }
    }
    return out;
}

std::vector<std::string>
SonarrMockClient::get_series_download_hashes(int sonarr_id) {
    return get_series_download_hashes_checked(sonarr_id)
        .value_or(std::vector<std::string>{});
}
```

`src/media_browser/ui/series_detail_screen.cpp` — Task 7 Step 3's block
above already carries the remove worker's final form (checked variant +
the cancel_ok-aware abort toast + the title-prefixed "series id unknown"
toast); nothing further to change there.

`tests/media_browser/test_sonarr_client.cpp` — two new `TEST_CASE`s, right
after "get_series_download_hashes walks the series history" and after
"SonarrMockClient seeds a coherent season pack" respectively:

```cpp
TEST_CASE("get_series_download_hashes_checked distinguishes empty from failed",
          "[sonarr][history]") {
    // Task 8's orphan-proof remove worker reads THIS return alone to decide
    // whether to abort — never a follow-up last_error() call, because Task
    // 8's background re-poll runs on its own thread and shares this
    // client's one last_error_ member. nullopt must mean "Sonarr did not
    // answer"; an engaged vector, even empty, must mean it genuinely did
    // (same contract as get_library_checked, pinned the same way).
    SECTION("HTTP failure -> nullopt") {
        StubSonarr s;  // no replies configured -> http_get returns ""
        CHECK_FALSE(s.get_series_download_hashes_checked(7).has_value());
        // The raw wrapper collapses nullopt to {}, same as get_library().
        CHECK(s.get_series_download_hashes(7).empty());
    }
    SECTION("genuinely no history -> engaged optional, empty vector") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/history/series?seriesId=7", "[]"}};
        auto hashes = s.get_series_download_hashes_checked(7);
        REQUIRE(hashes.has_value());
        CHECK(hashes->empty());
    }
    SECTION("populated history") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/history/series?seriesId=7",
                          read_fixture("history_series.json")}};
        auto hashes = s.get_series_download_hashes_checked(7);
        REQUIRE(hashes.has_value());
        REQUIRE(hashes->size() == 2);
        CHECK((*hashes)[0] == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
        CHECK((*hashes)[1] == "ffeeddccbbaa99887766554433221100aabbccdd");
    }
}
```

```cpp
TEST_CASE("SonarrMockClient's get_series_download_hashes_checked stays "
          "coherent with the raw variant", "[sonarr][mock]") {
    // Unlike get_library_checked (deliberately always nullopt — see the
    // dedicated test above for why), this mock has no field-observed
    // reachability lie to defend against here, so the checked override is
    // engaged and must agree exactly with the raw wrapper it backs.
    mb::SonarrMockClient m;
    auto checked = m.get_series_download_hashes_checked(1);
    REQUIRE(checked.has_value());
    REQUIRE(checked->size() == 1);
    CHECK((*checked)[0] == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(*checked == m.get_series_download_hashes(1));
}
```

- [ ] **Step 1: Header additions**

```cpp
    // ~9s quiet re-poll while InLibrary: fresh per-season statistics +
    // queue-derived downloading set. Single reused worker; never flashes
    // Loading (writes land via pending_/apply_pending like the fetch).
    // BOTH priors are passed BY VALUE — reading in_library_ / sonarr_ok_
    // from the worker thread was an unsynchronized read of render-thread
    // state. poll_gen_ and last_poll_at_ already live in the Task-3 block.
    void maybe_repoll_series();
    void run_series_poll(uint64_t gen, int sonarr_id, bool prev_sonarr_ok,
                         bool prev_in_library);
    std::atomic<bool> poll_inflight_{false};
    static constexpr int kSeriesPollMs = 9000;
    std::thread poll_worker_;
```

- [ ] **Step 2: The destructor's final form**

Replace `~SeriesDetailScreen()` with exactly this. It is the last word on shutdown ordering:

```cpp
SeriesDetailScreen::~SeriesDetailScreen() {
    // Invalidate every in-flight publish BEFORE joining anything: a worker
    // that finishes between the bump and its join sees a stale generation
    // and drops its result instead of writing into a half-destroyed object.
    // BOTH generations must move — fetch_gen_ gates the two load workers,
    // poll_gen_ gates the re-poll, and they publish into the SAME pending_.
    fetch_gen_.fetch_add(1);
    poll_gen_.fetch_add(1);
    if (mut_worker_.joinable()) mut_worker_.join();
    if (poll_worker_.joinable()) poll_worker_.join();
    for (auto& w : workers_) {
        if (w.thread.joinable()) w.thread.join();
    }
    // Worst-case shutdown latency is the mutation worker's: a whole-series
    // add is add_series (~13.5 s ceiling: add_settle_timeout_ms +
    // add_settle_poll_ms + timeout_secs) plus N season PUTs, a search and a
    // GET, each bounded by cfg_.timeout_secs (5 s). This screen is destroyed
    // only at kiosk shutdown, where nothing is watchdogged. Detaching
    // instead would be strictly worse — a detached worker writes into freed
    // members.
}
```

- [ ] **Step 3: Implement the poll**

```cpp
void SeriesDetailScreen::maybe_repoll_series() {
    if (!in_library_ || !sonarr_ok_) return;
    if (!series_.has_value() || series_->sonarr_id <= 0) return;
    if (mut_in_flight_.load() || poll_inflight_.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_poll_at_).count() < kSeriesPollMs) {
        return;
    }
    last_poll_at_ = now;
    if (poll_worker_.joinable()) poll_worker_.join();
    poll_inflight_.store(true);
    const uint64_t gen = poll_gen_.fetch_add(1) + 1;
    try {
        poll_worker_ = std::thread(&SeriesDetailScreen::run_series_poll, this,
                                   gen, series_->sonarr_id, sonarr_ok_,
                                   in_library_);
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] poll spawn failed: {}", e.what());
        poll_inflight_.store(false);
    }
}

void SeriesDetailScreen::run_series_poll(uint64_t gen, int sonarr_id,
                                         bool prev_sonarr_ok,
                                         bool prev_in_library) {
    // Clears the in-flight flag on EVERY exit path, the gen-mismatch discard
    // included. Without it, one future early return leaves poll_inflight_
    // stuck true and maybe_repoll_series() never polls again for the rest of
    // the session — silently, since the page still renders its last snapshot.
    struct InflightGuard {
        std::atomic<bool>& flag;
        ~InflightGuard() { flag.store(false, std::memory_order_release); }
    } inflight_guard{poll_inflight_};
    auto fresh = sonarr_.get_series(sonarr_id);
    std::unordered_set<int> downloading;
    for (const auto& q : sonarr_.get_queue()) {
        if (q.series_id == sonarr_id) downloading.insert(q.season_number);
    }
    std::lock_guard<std::mutex> lk(pending_mtx_);
    // Recheck under the lock — a worker that passed a pre-lock check could
    // be descheduled across fetch()'s bump-and-clear and publish stale data
    // into the new series' pending_.
    if (gen != poll_gen_.load()) return;  // preempted — discard
    pending_.sonarr_done = true;
    // A poll is ADVISORY: a transient blip must not flip the page to
    // SonarrUnreachable under the user. Success proves reachability;
    // failure leaves the original fetch's verdict standing. Both priors
    // arrive by value so nothing here reads render-thread state.
    pending_.sonarr_ok = fresh.has_value() ? true : prev_sonarr_ok;
    if (fresh.has_value()) {
        pending_.in_library = true;
        // The settle signal for an existing record: has Sonarr ever
        // actually refreshed it? This is what un-hides the add controls
        // ~9 s after an unsettled add, with no user action.
        pending_.settled = record_refreshed(*fresh);
        pending_.has_settled = true;
        pending_.series = std::move(fresh);
        // The queue snapshot rides on get_series' verdict. get_queue()
        // returns an EMPTY vector both for "nothing is downloading" and for
        // "Sonarr did not answer", and only get_series can tell those apart:
        // it succeeded, so Sonarr is answering and an empty set is the truth.
        // Publishing it unconditionally would let one transport blip blank
        // every real per-season badge until the next poll ~9 s later.
        pending_.downloading = std::move(downloading);
        pending_.has_downloading = true;
    } else {
        pending_.in_library = prev_in_library;
        // has_downloading stays false — apply_pending() leaves the existing
        // badges standing rather than clearing them on no evidence.
    }
    pending_ready_.store(true, std::memory_order_release);
}
```

`PendingLoad` already carries `downloading` / `has_downloading` / `settled` / `has_settled`, and `apply_pending()` already consumes all four (Task 3) — nothing to add there.

- [ ] **Step 4: Drive it from update()**

Change `update()` to:

```cpp
void SeriesDetailScreen::update() {
    drain_mutation();
    expire_confirms();
    apply_pending();
    maybe_repoll_series();
}
```

`drain_mutation` already resets `last_poll_at_ = {}` after every mutation it APPLIES (i.e. one whose series and load both still match the page — the identity gate drops the rest before this line), so a successful add's badges and statistics arrive on the next frame rather than up to 9 s later.

**Failure semantics (binding, embodied in the code above):** a poll is advisory — `sonarr_ok` only ever ratchets TO true from a successful poll; a failed poll leaves the original fetch's verdict standing, so a transient blip cannot flip the page to SonarrUnreachable under the user.

- [ ] **Step 5: Mac suite + Pi compile + commit**

Mac loop **+2 cases** green (Step 0's two `TEST_CASE` blocks — the poll
itself is kiosk-only code and adds none); Pi incremental clean.

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_client.h \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_client.cpp \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_mock.h \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_mock.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_sonarr_client.cpp
git commit -m "feat(mb): per-season downloading badges + quiet series re-poll

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: Pi verify (both flags) + acceptance

**Files:** none modified. This task produces evidence.

- [ ] **Step 1: Mac suites** — full `ctest` run (all 9 targets) + `test_media_browser_unit` at its final count (281 + this plan's additions; assert "no fewer than 5692 assertions").

- [ ] **Step 2: Pi build, `ENABLE_MEDIA_BROWSER=ON`** — clean scratch configure + build in `~/mdb-2c2` per Global Constraints. `EXIT=0`, no warnings naming any file this plan touched.

- [ ] **Step 3: Pi build, `ENABLE_MEDIA_BROWSER=OFF`** — the OFF invariant: `series_detail_*` is MB-gated via `KIOSK_MEDIA_BROWSER_SOURCES`, `mb_screen.h`'s new enum value is harmless, and `main.cpp`'s new instance/case must sit inside the existing `#ifdef MEDIA_BROWSER_ENABLED` region (they do if placed beside `mb_detail`'s — verify). `mb_chrome.{h,cpp}` is itself MB-only, so the promoted `wrap_text` needs no separate gate. `EXIT=0`.

- [ ] **Step 4: Clean up** `ssh magic@magicpi5.local 'rm -rf ~/mdb-2c2'`.

- [ ] **Step 5: Hardware acceptance checklist (Alex, on the box).** Deploy after merge through the same direct-deploy flow as 2c-1 (`PI_HOST=magic@magicpi5.local ./scripts/deploy_cpp.sh --build` from the MERGED main — never from this worktree).

1. **TV poster opens the screen.** Marquee → TV mode → select any poster → SeriesDetail: title, poster, year/seasons/episodes meta, wrapped overview, season list with counts. BTN4 returns to Browse with grid position intact.
2. **Movie poster still opens the movie Detail** — unchanged in every respect. (The `wrap_text` promotion touched Detail's synopsis: check the synopsis still wraps and ellipsizes exactly as before.)
3. **Season list truth.** Pick a show you know (e.g. Breaking Bad): season numbers and episode counts match TMDB; specials (S0) absent.
4. **A 20+ season show pages.** Open something long (The Simpsons, Grey's Anatomy): the indicator row reads `Seasons 1–N of M · [BTN1/BTN3]`, BTN3 advances, BTN1 goes back, the last page stops at the last season, and the season rows NEVER overlap the button row or the footer.
5. **Add Season 1.** On a show NOT in Sonarr: press Add Season 1 → the row dims (labels unchanged) → toast prefixed with the show's title. In Sonarr's web UI (port 8989): the series exists, ONLY Season 1 monitored, a season search ran (History). On the kiosk: row S1 flips to monitored; within ~18 s a grabbed release shows S1 `downloading`.
6. **settled==false path.** Immediately after the add toast, the season list must still show ALL seasons with TMDB counts — never "0 seasons" or an empty list — the meta line reads `… · syncing…`, and the action row shows ONLY `Remove` (never "Download Season 1" right after adding season 1). Within ~9 s the counts refresh and the add controls appear.
7. **Next season.** Press "Download Season 2" → S2 monitored + search started (verify in Sonarr UI). Button relabels to "Download Season 3".
8. **Whole series — allow path.** On a small show with plenty of disk: "Whole series…" → button arms to `Confirm ~N GB (est)` (sanity-check N ≈ episodes × 45 min × 70 MB/min) → confirm within 4 s → all seasons monitored + series search (Sonarr UI). Letting the arm EXPIRE (wait 5 s) reverts the label harmlessly.
9. **Arm, then confirm WITHOUT rotating.** Press "Whole series…", wait ~1 s, press SELECT again with no rotary movement in between. The confirm must FIRE (this is the focus-preservation case: focus must still be on the armed button, not reset to the first one). Then repeat but ROTATE once before the second press — the label must revert and nothing must be monitored.
10. **Whole series — first add monitors EVERY season.** On a show not in Sonarr, whole-series it. After the toast, wait ~30 s and reload the series in Sonarr's UI: seasons 2..N must still be monitored. (This is the `addOptions.monitor="none"` case — with "firstSeason" Sonarr's async apply lands after our PUTs and silently unmonitors everything past season 1.)
11. **Whole series — BLOCK path.** Temporarily fill the SSD or pick something enormous (e.g. a 30-season show): pressing "Whole series…" toasts `<title>: not enough space — needs ~X GB (est), Y GB free (20 GB floor)` and does NOT arm. Nothing was monitored (Sonarr UI unchanged).
12. **Stop Sonarr, THEN mutate.** From a loaded in-library page, `docker stop mdb_sonarr`, then press "Whole series…" and confirm. The toast must be honest — `<title>: couldn't monitor seasons — is Sonarr running?` — and must NOT claim anything was monitored or promise RSS pickup. `docker start mdb_sonarr`.
13. **Remove, orphan-proof.** On a test series with an active download: Remove → Confirm Remove within 2 s → toast, return to Browse. Verify: Sonarr no longer lists the series, qBit no longer lists its torrent, `/mnt/ssd/library/tv/<series>` gone. **Then open another series and remove it too** — the second remove must work (proves `navigate_back_` is cleared, not latched).
14. **Remove with a SEASON PACK in flight.** Specifically: a series whose queue shows MANY episode rows sharing one download (a season pack). Remove → Confirm Remove. It must succeed — not abort with "couldn't cancel N download(s)". Verify the torrent is gone from qBit and the series from Sonarr.
15. **Leave mid-add.** Start an add on show A, immediately BTN4 out and open a DIFFERENT show B. Expect: A's title-prefixed outcome toast appears over B's page; B's page shows B's own state (never A's seasons, never A's buttons); B's action row is undimmed; a SELECT during that window toasts `Still finishing the last action…` rather than doing nothing at all, and everything is live within ~14 s — never permanently stuck; going back to A shows A correctly added.
15b. **A→B→A.** Start an add on show A, BTN4 out, open B, then go straight BACK to A while the add is still running. A re-fetches, so its page must settle into the CORRECT post-add state (in-library, `· syncing…` then the season controls) — not "Add Season 1" stuck on an added series. The dropped application is logged: `journalctl -u magic-dingus-box-cpp | grep "dropping mutation result"` should show one line naming A's tmdb id and both generations.
15c. **Add something the box already has, the hard way.** If a library record ever presents with `tmdb_id == 0` (Sonarr's own tmdbId field empty), the page offers "Add Season 1" even though the series is present. Pressing it must NOT toast "Season 1 search started" — expect `<title>: already in your TV library` when the first season already has files, or a real monitor+search (and a season-numbered toast) when it does not.
16. **Downloading badge.** While something is downloading: the season row reads `downloading`; when the import completes the row flips to `complete` within ~9 s without leaving the screen.
17. **Unconfigured box.** Blank `SONARR_API_KEY` in `services/.env`, restart kiosk: SeriesDetail shows the full read-only page with the line `TV library not set up on this box` and NO action row. Restore the key.
18. **Sonarr stopped at load.** `docker stop mdb_sonarr` → open a TV poster: read-only page with `Sonarr service offline`, no action row, no crash, BTN4 works. `docker start mdb_sonarr`.
19. **`scripts/verify_box.sh`** exits 0 (SHIPPABLE).

---

## Self-Review (already applied to the text above)

**Spec coverage.** Series detail screen (poster/overview + per-season state + counts): Tasks 3+8. "Add Season 1" default: Task 5. "Download next season": Task 5. "Whole series…" with confirm + estimate: Task 6. Remove: Task 7. Blocking preflight per spec (block over free−20 GiB, estimate shown, warn only when the reading FAILS): Tasks 1+6. Season-at-a-time = configuration: Tasks 5-6. Season packs: nothing to add — season searches admit them server-side, and the remove flow now dedupes their queue rows. Queue grouping / Search TV / Library mixed / episode picker: out of scope by the exclusion list.

**What this revision changed, and why (adversarial verification, 2 reviewers).**

1. **Input.** `platform::InputEvent` has `{action, delta, pressed, velocity, is_from_rotary}` — no `value`. The previous draft read `e.value` (does not compile) behind an `if (!e.pressed) continue;` filter that would have killed rotary entirely, since EV_REL events carry `pressed=false`. `handle_input` is now DetailScreen's structure verbatim: `ROTATE`/`ROTATE_VERTICAL` gated on `e.delta != 0` with `std::clamp` (no wrap), `SELECT`/`SETTINGS_MENU` on `e.pressed`, and any rotation disarms BOTH confirms.
2. **Focus.** Every mutation used to force `focus_ = 0` and swap the row for a single "Working…" button, so the whole-series confirm was never focused and SELECT inside the 4 s window fired *Add Season 1*. The "Working…" swap is gone (the row persists and renders dimmed), and focus is preserved by ACTION IDENTITY across rebuilds, with Remove/ConfirmRemove treated as one button.
3. **Thread safety.** Worker threads no longer write `whole_armed_`/`whole_armed_at_`/`whole_estimate_bytes_`: the press-1 worker publishes a verdict under `mut_mtx_` and `drain_mutation` (render thread) arms the button, so the countdown starts when the label appears. `run_series_poll` takes both priors by value instead of reading `in_library_`/`sonarr_ok_` off-thread. No worker holds `mut_mtx_` across a network call.
4. **Generation discipline.** `fetch()` and the destructor bump `poll_gen_` as well as `fetch_gen_`, and both bumps happen BEFORE `pending_` is cleared — otherwise a stale poll published series A's record into series B's `pending_` and Remove targeted A under B's header. The destructor's final form is stated verbatim in Task 8, with its shutdown-latency note.
5. **Mutation identity.** `mut_tmdb_id_` **and `mut_fetch_gen_`** record which series a mutation belongs to AND which load of it. `drain_mutation` ALWAYS clears the in-flight state (a stuck flag froze the row for the session) but applies series/library/navigation only when both match, and logs the drop otherwise. The generation half closes A→B→A: coming back to series A re-fetches it, so an id-only gate would let the drain fight the refetch's pre-mutation snapshot. Every worker-composed toast is title-prefixed, so an outcome that lands after the user moved on is still meaningful instead of silently dropped.
6. **Robustness.** `spawn_mutation`'s thread body now matches its comment: an RAII guard flips `mut_done_` on every exit path and a `catch` turns a throw out of `body()` into a failure toast instead of `std::terminate`.
7. **Honesty.** `whole_series_verdict` Blocks on a free-space reading of ZERO (the full-disk case the preflight exists for) and warns only on a FAILED reading; Task 1's test pins it. The armed label says `Confirm ~N GB (est)` because pre-add the runtime multiplicand is a 45-minute assumption. The whole-series toast is composed from BOTH the failure count and the search outcome, so it never claims "monitored" or promises RSS when every PUT failed. The quality-profile-missing toast distinguishes transport failure via `last_error()`.
8. **Correctness against Sonarr's async addOptions.** The whole-series pre-add uses `monitor=true` — REQUIRED, because `add_series` writes the SERIES-LEVEL monitored flag from the same parameter and no client method can flip it afterwards; an unmonitored series has every release rejected by `MonitoredEpisodeSpecification` (seasons monitored, search runs, nothing downloads, invisibly). The `"firstSeason"`-vs-our-PUTs race is provably over when `settled==true` (`add_settled` requires the applied state to have been OBSERVED), so the season PUTs gate on `res.settled` and the timeout path stops honestly for a later retry via the in-library path. Announced series may have seasons Sonarr does not know yet; those PUT failures are counted and reported, not hidden.
9. **Remove.** Cancels once per unique `download_id` (per-row fallback when empty). A season pack's sibling rows 404 by documented design; counting them as failures aborted the remove *after* the torrent was gone. The abort-before-delete rule still applies to genuinely failed cancels.
10. **Unsettled records.** `series_settled_` (from `record_refreshed`) hides NextSeason/WholeSeries and appends `· syncing…` to the meta line for the window where Sonarr holds the record but has never refreshed it — otherwise the primary button read "Download Season 1" one second after adding Season 1.
11. **Layout.** The header passes an EMPTY tab strip like DetailScreen (a 7-chip strip under an arbitrary series title overlaps it) and truncates the title with the title font's own measurer. The season list is PAGED with BTN1/BTN3 — a 21-season show was previously unreachable past ~8 rows, and that is the screen's headline case. The action row's height and the indicator row are reserved out of the list budget in ONE place, inside the `else` scope where `body_x`/`list_bottom` exist, so the old "+N more overlaps the buttons" geometry is structurally impossible.
12. **Render discipline.** The screen no longer calls `::ui::Toast::render` — main.cpp owns the single app-wide draw in the correct projection, and a screen-level call double-drew in the wrong one. The binding rule is restated as: no early `return` in `render()`, and the footer is the last draw on every path.
13. **House style.** Task 2's parser is a `static` member of `class SonarrParsers` (not a free function), tests call it as `mb::SonarrParsers::parse_quality_definitions`, and the client body mirrors `get_root_folders` exactly — `http_get` takes a PATH and prepends the base URL itself, so the old `cfg_.base_url + ...` would have 404'd SILENTLY into the 70 fallback.
14. **Deletions.** `BrowseScreen::selected_kind()` is gone (the kind rides the returned `Screen`), as are the dead `mut_ok_`, the dead `was_armed`, and the write-only `visible_season_rows_` (replaced by `season_page_count_`, which the paging math genuinely consumes).
15. **Mock honesty, restated correctly.** The previous claim — "this plan consumes no newly-trapped method" — was false: `add_series` calls `find_series_by_tvdb` inside the client. The real invariant is `sonarr_configured_` gating (the mock exists iff the key is empty, and this screen makes zero Sonarr calls in that state), plus a binding warning that any future degraded-state Sonarr affordance puts the whole dishonest-mock family live at once and must fix those overrides first.
16. **Task 5 review fixes (post-`45e17d0`, folded into the text above).** Four Important + four minor findings from the Task-5 review, all mirrored into Task 5's blocks: (a) `add_series` returns `ok=true` from its find-existing branch having applied nothing, so the settled path now READS the outcome off the returned record — lowest non-special season monitored? has files? — and either monitors+searches explicitly, says "already in your TV library", or keeps the search toast; (b) `spawn_mutation` stamps `mut_fetch_gen_` and the drain gates on it (A→B→A); (c) a SELECT gated by the global in-flight flag now toasts instead of dying silently, because the dim is series-scoped; (d) `SonarrClient::get_quality_profiles` gained the entry `set_error({})` its siblings all have, without which the "couldn't reach Sonarr" branch could echo an unrelated stale error — the one out-of-screen change; (e) the row's focus algebra moved into `decide_action_row` in series_detail_logic.h with 8 table cases, since it is pure decision logic and its bug was a plan-time Critical; (f) rotary focus freezes while the same series is mutating (the ring is hidden, so movement would be invisible); (g) the action row stops drawing before a button would cross the safe inset and clamps focus to what was actually drawn (640x480 is the motivating canvas); (h) the identity-gate failure logs the dropped application.
17. **Shared helper.** `wrap_text` is promoted from detail_screen.cpp's anonymous namespace into `mb_chrome.{h,cpp}` — the `truncate_to_width` precedent — rather than copied a second time. PlaybackOverlay's separate `wrap_text_overlay` is deliberately left alone.
