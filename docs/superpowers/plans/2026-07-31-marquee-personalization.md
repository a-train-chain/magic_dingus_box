# Marquee Personalization (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Media Browser's content tabs fresh (6-hour TTL), shuffleable (filter-overlay SHUFFLE row), and personal (a "For You" tab seeded from the Radarr library via TMDB recommendations).

**Architecture:** Three layers, landed bottom-up: (1) the TMDB client's list calls gain a `TmdbList {ok, total_pages, hits}` result shape so callers can distinguish failure from empty; (2) BrowseScreen's pagination becomes window-relative (base page N loads pages N…N+4) with a background revalidate path; (3) a new `Category::ForYou` renders a merged, scored recommendation list computed by a pure, unit-tested function. All UI work stays inside the `ENABLE_MEDIA_BROWSER` compilation gate.

**Tech Stack:** C++17, libcurl + jsoncpp (TmdbClient), Catch2 v3 (tests, FetchContent), CMake. Spec: `docs/superpowers/specs/2026-07-31-marquee-personalization-and-tv-design.md`.

## Global Constraints

- **Never edit `src/main.cpp`** — a parallel session owns an uncommitted change there (the BTN1/BTN3 double-fire fix). Phase 1 needs no dispatcher changes.
- **Bit-identical invariant:** every file touched is inside the Media Browser source lists; the `ENABLE_MEDIA_BROWSER=OFF` binary must not change.
- **All commands run from** `/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp` unless stated. The repo path contains spaces and emoji — always quote it.
- **Mac unit tests:** `cmake --build build-mb --target test_media_browser_unit -j8 && ./build-mb/test_media_browser_unit` (build-mb is already configured with `ENABLE_MEDIA_BROWSER=ON BUILD_TESTS=ON BUILD_KIOSK=OFF`). New test files must be added to `MEDIA_BROWSER_TEST_SOURCES` in `CMakeLists.txt`; new pure (Renderer-free) sources to `MEDIA_BROWSER_SOURCES`.
- **Kiosk-only files** (`browse_screen.cpp`, `mb_filter_overlay.cpp`, `mb_chrome.cpp`, `playback_overlay.cpp`) do NOT compile on the Mac (they need GLES). Compile-verify them on the Pi: `cd .. && MEDIA_BROWSER=true ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build` (expects the Pi at `magic@magicpi.local`; override with `PI_HOST`).
- **Spec values (verbatim):** TTL = 6 hours; shuffle base ranges Popular 1–26, Top Rated 1–21; `kMaxLoadedPages = 5`; For You seeds = min(8, library size); merge cap = exactly 100; strip spacing mitigation `kTabHorizPad` 16→10, `kTabGap` 24→16.
- Commit style: conventional prefixes (`feat:`, `test:`, `refactor:`) matching repo history.

---

### Task 1: `TmdbList` result shape (spec 1a′)

**Files:**
- Modify: `src/media_browser/tmdb_client.h` (add struct + change 7 signatures + new parser decl)
- Modify: `src/media_browser/tmdb_client.cpp` (endpoints + `parse_list`)
- Modify: `src/media_browser/ui/browse_screen.cpp:526-537, 602-617` (call sites)
- Modify: `src/media_browser/ui/playback_overlay.cpp:163-178` (call sites)
- Create: `tests/media_browser/fixtures/tmdb/recommendations.json`
- Modify: `tests/media_browser/test_tmdb_client.cpp` (new tests)

**Interfaces:**
- Produces: `struct TmdbList { bool ok = false; int total_pages = 0; std::vector<TmdbSearchHit> hits; };` and `static TmdbList TmdbClient::parse_list(const std::string& json)`. The seven list endpoints (`get_popular`, `get_now_playing`, `get_top_rated`, `get_upcoming`, `discover`, `get_similar`, `get_recommendations`) now return `TmdbList`. `ok` is true iff the JSON parsed AND `results` was an array (a TMDB error payload parses but has no `results` → `!ok`). `search_movie`, `get_movie`, `get_genres`, and the existing static parsers are untouched.
- Consumed by: Tasks 3, 4, 6 (`.ok`, `.total_pages`, `.hits`).

- [ ] **Step 1: Write the failing tests**

Append to `tests/media_browser/test_tmdb_client.cpp`:

```cpp
TEST_CASE("TmdbClient::parse_list carries ok + total_pages + hits", "[tmdb][list]") {
    const std::string json = R"({
        "page": 1,
        "total_pages": 42,
        "results": [
            {"id": 11, "title": "Star Wars", "release_date": "1977-05-25",
             "vote_average": 8.2, "poster_path": "/sw.jpg"},
            {"id": 12, "title": "Adult Junk", "adult": true,
             "release_date": "2001-01-01", "vote_average": 1.0, "poster_path": "/x.jpg"}
        ]
    })";
    auto list = media_browser::TmdbClient::parse_list(json);
    REQUIRE(list.ok);
    REQUIRE(list.total_pages == 42);
    REQUIRE(list.hits.size() == 1);  // adult row dropped, same as parse_list_response
    CHECK(list.hits[0].tmdb_id == 11);
    CHECK(list.hits[0].poster_path == "https://image.tmdb.org/t/p/w500/sw.jpg");
}

TEST_CASE("TmdbClient::parse_list flags malformed and error payloads", "[tmdb][list]") {
    CHECK_FALSE(media_browser::TmdbClient::parse_list("not json {{{").ok);
    // TMDB error payload: valid JSON, no results array → NOT ok.
    CHECK_FALSE(media_browser::TmdbClient::parse_list(
        R"({"status_code": 34, "status_message": "not found"})").ok);
    // Empty results array is still ok (a real, empty page).
    auto empty = media_browser::TmdbClient::parse_list(
        R"({"page": 1, "total_pages": 1, "results": []})");
    CHECK(empty.ok);
    CHECK(empty.hits.empty());
}

TEST_CASE("TmdbClient::parse_list parses the recommendations fixture", "[tmdb][list]") {
    const std::string json = load_fixture("recommendations.json");
    REQUIRE_FALSE(json.empty());
    auto list = media_browser::TmdbClient::parse_list(json);
    REQUIRE(list.ok);
    CHECK(list.total_pages == 2);
    REQUIRE(list.hits.size() == 3);   // 4 entries in fixture, 1 adult dropped
    CHECK(list.hits[0].tmdb_id == 27205);
}
```

Create `tests/media_browser/fixtures/tmdb/recommendations.json`:

```json
{
  "page": 1,
  "results": [
    {"id": 27205, "title": "Inception", "original_title": "Inception",
     "overview": "A thief who steals corporate secrets.",
     "poster_path": "/inception.jpg", "release_date": "2010-07-15",
     "vote_average": 8.4, "adult": false},
    {"id": 155, "title": "The Dark Knight", "original_title": "The Dark Knight",
     "overview": "Batman raises the stakes.",
     "poster_path": "/tdk.jpg", "release_date": "2008-07-16",
     "vote_average": 8.5, "adult": false},
    {"id": 999999, "title": "Should Be Dropped", "original_title": "Should Be Dropped",
     "overview": "adult content", "poster_path": "/drop.jpg",
     "release_date": "2015-01-01", "vote_average": 5.0, "adult": true},
    {"id": 603, "title": "The Matrix", "original_title": "The Matrix",
     "overview": "A hacker learns the truth.",
     "poster_path": "/matrix.jpg", "release_date": "1999-03-30",
     "vote_average": 8.2, "adult": false}
  ],
  "total_pages": 2,
  "total_results": 40
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build build-mb --target test_media_browser_unit -j8
```
Expected: **compile error** — `parse_list` is not a member of `TmdbClient`. (A compile failure is this step's "failing test".)

- [ ] **Step 3: Implement `TmdbList` in the client**

In `src/media_browser/tmdb_client.h`, insert after the `TmdbSearchHit` struct (line 21):

```cpp
// Result of any TMDB "results[]" list endpoint. `ok` distinguishes a fetch/
// parse failure from a genuinely empty page — the bare-vector endpoints could
// not, which made stale-while-revalidate inexpressible (spec 1a′). `ok` is
// true iff the JSON parsed AND carried a results array; TMDB error payloads
// (valid JSON, no results) are NOT ok.
struct TmdbList {
    bool ok = false;
    int total_pages = 0;   // TMDB total_pages; 0 when absent.
    std::vector<TmdbSearchHit> hits;
};
```

Change the seven signatures in the class (lines 77-91):

```cpp
    // Category endpoints — TMDB's canonical discovery surfaces.
    TmdbList get_popular(int page = 1);
    TmdbList get_now_playing(int page = 1);
    TmdbList get_top_rated(int page = 1);
    TmdbList get_upcoming(int page = 1);

    // Discover (free-form filter).
    TmdbList discover(const DiscoverFilter& filter, int page = 1);

    // Similar movies (by id) — used by Marquee playback overlay.
    TmdbList get_similar(int tmdb_id, int page = 1);

    // Recommendations for a movie (by id) — algorithmic mix of similar + trending.
    // Generally gives better suggestions than get_similar; callers should fall
    // back to get_similar when this returns empty hits.
    TmdbList get_recommendations(int tmdb_id, int page = 1);
```

Add beside the other static parsers (after line 103):

```cpp
    // TmdbList-shaped variant of parse_list_response — same row handling
    // (family-safe drop, w500 poster prefix) plus ok/total_pages. The old
    // vector-shaped parser stays for its existing tests and callers.
    static TmdbList parse_list(const std::string& json);
```

In `src/media_browser/tmdb_client.cpp`, add after `parse_list_response` (line 397):

```cpp
TmdbList TmdbClient::parse_list(const std::string& json) {
    TmdbList list;
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(json);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        spdlog::error("[media_browser] TMDB list parse error: {}", err);
        return list;  // ok=false
    }
    const auto& results = root["results"];
    if (!results.isArray()) return list;  // TMDB error payload — ok=false
    list.ok = true;
    list.total_pages = root.get("total_pages", 0).asInt();
    for (const auto& r : results) {
        if (r.get("adult", false).asBool()) continue;  // family-safe drop
        TmdbSearchHit h;
        h.tmdb_id = r.get("id", 0).asInt();
        h.title = r.get("title", "").asString();
        h.original_title = r.get("original_title", "").asString();
        h.overview = r.get("overview", "").asString();
        h.poster_path = resolve_poster_url(r.get("poster_path", "").asString());
        h.year = extract_year(r.get("release_date", "").asString());
        h.rating = r.get("vote_average", 0.0).asDouble();
        list.hits.push_back(std::move(h));
    }
    return list;
}
```

Convert the seven endpoint bodies (each currently `if (body.empty()) return {}; return parse_list_response(body);`) to the same two-line tail — e.g. `get_popular` (line 207) becomes:

```cpp
TmdbList TmdbClient::get_popular(int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/popular?api_key=" << api_key_
        << "&include_adult=false"
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};  // ok=false — network/HTTP failure
    return parse_list(body);
}
```

Apply the identical tail change (return type `TmdbList`, `return parse_list(body);`) to `get_now_playing`, `get_top_rated`, `get_upcoming`, `get_similar`, `get_recommendations`, and `discover`.

- [ ] **Step 4: Update the two call sites**

`src/media_browser/ui/browse_screen.cpp` — `run_load_page` (line 530): change the local and the switch to:

```cpp
    TmdbList list;
    switch (cat) {
        case Category::Popular:    list = tmdb_.get_popular(page);     break;
        case Category::NowPlaying: list = tmdb_.get_now_playing(page); break;
        case Category::TopRated:   list = tmdb_.get_top_rated(page);   break;
        case Category::Upcoming:   list = tmdb_.get_upcoming(page);    break;
        default: break;
    }
    std::vector<TmdbSearchHit> result = std::move(list.hits);
```

`run_reload_filter_page` (line 604): `auto result = tmdb_.discover(filter, page);` becomes:

```cpp
    auto list = tmdb_.discover(filter, page);
    auto result = std::move(list.hits);
```

(`no_more` and the publish block below each are unchanged in this task; Task 3 rewrites them.)

`src/media_browser/ui/playback_overlay.cpp` (line 163): the worker body becomes:

```cpp
        auto results = tmdb.get_recommendations(id, /*page=*/1).hits;
        if (cancel_requested_.load()) {
            spdlog::debug("[playback_overlay] prefetch cancelled for tmdb_id={}", id);
            return;
        }
        spdlog::info("[playback_overlay] get_recommendations returned {} films for tmdb_id={}",
                     results.size(), id);
        if (results.empty()) {
            results = tmdb.get_similar(id, /*page=*/1).hits;
```

(The lines after the fallback call — cancel check, log, `kMaxSimilar` resize — are unchanged.)

- [ ] **Step 5: Run Mac tests to verify they pass**

```bash
cmake --build build-mb --target test_media_browser_unit -j8 && ./build-mb/test_media_browser_unit "[list]"
```
Expected: `All tests passed` (3 new test cases). Then run the full suite once: `./build-mb/test_media_browser_unit` — expected all green (no existing test names an endpoint return type).

- [ ] **Step 6: Compile-verify kiosk-only callers on the Pi**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box " && MEDIA_BROWSER=true ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```
Expected: build completes with no errors (browse_screen.cpp + playback_overlay.cpp compile against the new signatures).

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/tmdb_client.h magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp magic_dingus_box_cpp/src/media_browser/ui/playback_overlay.cpp magic_dingus_box_cpp/tests/media_browser/test_tmdb_client.cpp magic_dingus_box_cpp/tests/media_browser/fixtures/tmdb/recommendations.json
git commit -m "feat(mb): TMDB list endpoints return TmdbList (ok + total_pages + hits)"
```

---

### Task 2: Pure browse-logic helpers

**Files:**
- Create: `src/media_browser/ui/browse_logic.h` (header-only, Renderer-free)
- Create: `tests/media_browser/test_browse_logic.cpp`
- Modify: `CMakeLists.txt` (add the test file to `MEDIA_BROWSER_TEST_SOURCES`)

**Interfaces:**
- Produces (all in `namespace media_browser::ui`):
  - `bool tmdb_grid_stale(std::chrono::steady_clock::time_point last, std::chrono::steady_clock::time_point now)` — true when `last` is default-constructed or `now - last > 6h`.
  - `int window_last_page(int base, int max_loaded_pages = 5)` → `base + max_loaded_pages - 1`.
  - `int discover_max_base(int total_pages, int max_loaded_pages = 5)` → `min(26, max(1, total_pages − max_loaded_pages + 1))`; `26` when `total_pages <= 0` (unknown → optimistic).
  - `int pick_shuffle_base(int current_base, int max_base, uint32_t rand_value)` — uniform over `1..max_base` excluding `current_base` when ≥2 candidates exist; returns `1` when `max_base <= 1` (collapsed range → plain refetch, spec 1b).
  - `enum class ForYouEntry { UseCache, Sample, WaitForLibrary, ServiceUnavailable, EmptyLibrary };` and `ForYouEntry decide_foryou_entry(bool has_cached_list, bool refresh_done_once, bool library_fetch_ok, bool library_empty)` (spec 1c three-way entry rule; the in-flight case is `WaitForLibrary`).
  - Constants: `kShuffleMaxBasePopular = 26`, `kShuffleMaxBaseTopRated = 21`, `kBrowseTtlHours = 6`.
- Consumed by: Tasks 3, 4, 6 (BrowseScreen) and this task's tests.

- [ ] **Step 1: Write the failing tests**

Create `tests/media_browser/test_browse_logic.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "media_browser/ui/browse_logic.h"

using namespace media_browser::ui;
using clock_tp = std::chrono::steady_clock::time_point;

TEST_CASE("tmdb_grid_stale: default timestamp is stale; 6h TTL", "[browse_logic]") {
    const clock_tp t0{std::chrono::hours(1000)};
    CHECK(tmdb_grid_stale(clock_tp{}, t0));                          // never loaded
    CHECK_FALSE(tmdb_grid_stale(t0, t0 + std::chrono::hours(5)));    // fresh
    CHECK_FALSE(tmdb_grid_stale(t0, t0 + std::chrono::hours(6)));    // boundary: exactly 6h is fresh
    CHECK(tmdb_grid_stale(t0, t0 + std::chrono::hours(7)));          // stale
}

TEST_CASE("window_last_page is base-relative", "[browse_logic]") {
    CHECK(window_last_page(1) == 5);
    CHECK(window_last_page(12) == 16);
    CHECK(window_last_page(26) == 30);
}

TEST_CASE("discover_max_base clamps to known total_pages", "[browse_logic]") {
    CHECK(discover_max_base(0) == 26);     // unknown → optimistic ceiling
    CHECK(discover_max_base(-1) == 26);
    CHECK(discover_max_base(500) == 26);   // huge → ceiling
    CHECK(discover_max_base(8) == 4);      // 8 - 5 + 1
    CHECK(discover_max_base(5) == 1);
    CHECK(discover_max_base(2) == 1);      // fewer pages than the window → base 1 only
}

TEST_CASE("pick_shuffle_base excludes current base when possible", "[browse_logic]") {
    // Collapsed range → plain refetch of page 1.
    CHECK(pick_shuffle_base(1, 1, 0u) == 1);
    CHECK(pick_shuffle_base(1, 0, 7u) == 1);
    // Current base outside range → plain uniform draw over 1..max.
    CHECK(pick_shuffle_base(0, 4, 0u) == 1);
    CHECK(pick_shuffle_base(0, 4, 3u) == 4);
    CHECK(pick_shuffle_base(99, 4, 5u) == 2);  // 5 % 4 = 1 → base 2
    // Exclusion: current=3, max=5 → candidates {1,2,4,5} in rand order.
    CHECK(pick_shuffle_base(3, 5, 0u) == 1);
    CHECK(pick_shuffle_base(3, 5, 1u) == 2);
    CHECK(pick_shuffle_base(3, 5, 2u) == 4);
    CHECK(pick_shuffle_base(3, 5, 3u) == 5);
    CHECK(pick_shuffle_base(3, 5, 4u) == 1);   // wraps
    // Exclusion with exactly 2 candidates always picks the other one.
    CHECK(pick_shuffle_base(1, 2, 0u) == 2);
    CHECK(pick_shuffle_base(2, 2, 0u) == 1);
    CHECK(pick_shuffle_base(1, 2, 41u) == 2);
}

TEST_CASE("decide_foryou_entry three-way rule", "[browse_logic]") {
    // Cached list always wins — activation never refetches (spec 1c).
    CHECK(decide_foryou_entry(true, true, true, false) == ForYouEntry::UseCache);
    CHECK(decide_foryou_entry(true, false, false, true) == ForYouEntry::UseCache);
    // No refresh completed yet → wait (caller shows Loading + kicks a refresh).
    CHECK(decide_foryou_entry(false, false, false, false) == ForYouEntry::WaitForLibrary);
    // Refresh done but library GET failed → service state, NOT the teach message.
    CHECK(decide_foryou_entry(false, true, false, true) == ForYouEntry::ServiceUnavailable);
    // Refresh done, fetch ok, genuinely empty → teach message.
    CHECK(decide_foryou_entry(false, true, true, true) == ForYouEntry::EmptyLibrary);
    // Refresh done, fetch ok, library populated → sample.
    CHECK(decide_foryou_entry(false, true, true, false) == ForYouEntry::Sample);
}
```

Add to `CMakeLists.txt` in `MEDIA_BROWSER_TEST_SOURCES` (after `tests/media_browser/test_tmdb_client.cpp`):

```cmake
        tests/media_browser/test_browse_logic.cpp
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build-mb --target test_media_browser_unit -j8
```
Expected: compile error — `browse_logic.h` not found.

- [ ] **Step 3: Implement**

Create `src/media_browser/ui/browse_logic.h`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>

// Pure decision helpers for BrowseScreen's TTL / shuffle / For You entry
// logic (spec: 2026-07-31-marquee-personalization-and-tv-design.md, Phase 1).
// Header-only and Renderer-free so test_media_browser_unit can assert on
// them — same rationale as mb_ui_utils / library_view.

namespace media_browser::ui {

inline constexpr int kBrowseTtlHours       = 6;
inline constexpr int kShuffleMaxBasePopular  = 26;  // window base+4 never passes page 30
inline constexpr int kShuffleMaxBaseTopRated = 21;  // window base+4 never passes page 25

// True when the grid loaded at `last` should be refreshed at `now`.
// A default-constructed time_point means "never loaded" → stale.
inline bool tmdb_grid_stale(std::chrono::steady_clock::time_point last,
                            std::chrono::steady_clock::time_point now) {
    if (last == std::chrono::steady_clock::time_point{}) return true;
    return (now - last) > std::chrono::hours(kBrowseTtlHours);
}

// Last page of the base-relative pagination window.
inline int window_last_page(int base, int max_loaded_pages = 5) {
    return base + max_loaded_pages - 1;
}

// Highest legal shuffle base for a /discover result set. total_pages <= 0
// means no cached value for this filter signature — clamp optimistically to
// the Popular ceiling and rely on the empty-page → page-1 fallback.
inline int discover_max_base(int total_pages, int max_loaded_pages = 5) {
    if (total_pages <= 0) return kShuffleMaxBasePopular;
    int mb = total_pages - max_loaded_pages + 1;
    if (mb < 1) mb = 1;
    if (mb > kShuffleMaxBasePopular) mb = kShuffleMaxBasePopular;
    return mb;
}

// Uniform draw over 1..max_base excluding current_base whenever at least two
// candidates exist; a collapsed range degrades to a plain page-1 refetch
// (spec 1b: the shuffle draw must never deadlock re-rolling).
inline int pick_shuffle_base(int current_base, int max_base, uint32_t rand_value) {
    if (max_base <= 1) return 1;
    const bool exclude = (current_base >= 1 && current_base <= max_base);
    if (!exclude) {
        return 1 + static_cast<int>(rand_value % static_cast<uint32_t>(max_base));
    }
    // Draw index over the max_base-1 non-current candidates, then skip past
    // current_base so the mapping stays uniform.
    int base = 1 + static_cast<int>(rand_value % static_cast<uint32_t>(max_base - 1));
    if (base >= current_base) ++base;
    return base;
}

// Spec 1c "entry rule (three-way)" — what to do when the For You tab
// activates. WaitForLibrary covers both "refresh in flight" and "no refresh
// ever ran" (the caller kicks one off if none is in flight).
enum class ForYouEntry { UseCache, Sample, WaitForLibrary, ServiceUnavailable, EmptyLibrary };

inline ForYouEntry decide_foryou_entry(bool has_cached_list,
                                       bool refresh_done_once,
                                       bool library_fetch_ok,
                                       bool library_empty) {
    if (has_cached_list) return ForYouEntry::UseCache;
    if (!refresh_done_once) return ForYouEntry::WaitForLibrary;
    if (!library_fetch_ok) return ForYouEntry::ServiceUnavailable;
    if (library_empty) return ForYouEntry::EmptyLibrary;
    return ForYouEntry::Sample;
}

}  // namespace media_browser::ui
```

- [ ] **Step 4: Run to verify pass**

```bash
cmake --build build-mb --target test_media_browser_unit -j8 && ./build-mb/test_media_browser_unit "[browse_logic]"
```
Expected: `All tests passed` (5 test cases).

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/browse_logic.h magic_dingus_box_cpp/tests/media_browser/test_browse_logic.cpp magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): pure browse-logic helpers (TTL, shuffle base, For You entry rule)"
```

---

### Task 3: Window-relative pagination, `load_shuffle`, and TTL revalidate (spec 1a + 1b mechanics)

Kiosk-only code — verified by Pi compile + the pure helpers from Task 2. No Mac-testable units here by design; every decision with branching logic was extracted to `browse_logic.h`.

**Files:**
- Modify: `src/media_browser/ui/browse_screen.h`
- Modify: `src/media_browser/ui/browse_screen.cpp`

**Interfaces:**
- Consumes: `TmdbList` (Task 1); `tmdb_grid_stale`, `window_last_page`, `pick_shuffle_base`, `discover_max_base` (Task 2).
- Produces (private members/methods later tasks rely on):
  - `int page_window_base_ = 1;` — first page of the active window.
  - `std::chrono::steady_clock::time_point chart_loaded_at_{};` — active chart grid age.
  - `std::unordered_map<std::string, int> discover_total_pages_;` — per-filter-signature cache, key = `TmdbClient::build_discover_url("", filter, 1)`.
  - `void load_shuffle(Category cat, int base_page);` and `void load_shuffle_discover(int base_page);`
  - `void revalidate_active_chart();`
  - `PendingPage` gains `bool ok`, `int total_pages`, `bool is_revalidate`, `std::string discover_sig`.
  - `run_load_page` / `run_reload_filter_page` gain a trailing `bool is_revalidate` parameter.

- [ ] **Step 1: Extend the header**

In `src/media_browser/ui/browse_screen.h`:

Add to the includes: `#include <unordered_map>` and `#include "media_browser/ui/browse_logic.h"`.

Replace the `PendingPage` struct (lines 198-202) with:

```cpp
    struct PendingPage {
        std::vector<TmdbSearchHit> movies;
        int  page;         // absolute TMDB page number (window base .. base+4)
        bool no_more;      // true if this fetch indicates we hit the end of the list
        bool ok = true;    // TmdbList.ok — false = fetch/parse failure
        int  total_pages = 0;      // TmdbList.total_pages (0 when unknown)
        bool is_revalidate = false;  // background TTL refresh — skip swap on failure/empty
        std::string discover_sig;    // non-empty for discover fetches → total_pages cache key
    };
```

Add to the pagination-state block (after `loaded_tmdb_ids_`, line 233):

```cpp
    // First page of the active pagination window. 1 for normal loads; the
    // random base after a shuffle. maybe_load_more_pages() loads
    // [page_window_base_, window_last_page(page_window_base_)] — the old code
    // treated kMaxLoadedPages as an ABSOLUTE page cap, which would have made
    // any shuffled base >= 6 load a single page and stop (spec 1b).
    int page_window_base_ = 1;
    // When a shuffled base page comes back genuinely empty (ok but 0 hits —
    // possible on the /discover path), fall back to a plain page-1 load.
    bool shuffle_retry_base1_ = false;
    // Age of the active chart grid (Popular/TopRated, curated or discover).
    // Default-constructed = never loaded. Drives the 6h TTL (spec 1a).
    std::chrono::steady_clock::time_point chart_loaded_at_{};
    // Last-seen total_pages per discover filter signature (spec 1b) —
    // key = TmdbClient::build_discover_url("", filter, 1).
    std::unordered_map<std::string, int> discover_total_pages_;
```

Change the two worker declarations (lines 124-125) and add the new entry points (after `reload_for_category()`, line 117):

```cpp
    void run_load_page(uint64_t gen, Category cat, int page, bool is_revalidate = false);
    void run_reload_filter_page(uint64_t gen, DiscoverFilter filter, int page,
                                bool is_revalidate = false);
    // Shuffle entry points (spec 1b): mirror load_category's synchronous
    // reset, then spawn the base page of a fresh window.
    void load_shuffle(Category cat, int base_page);
    void load_shuffle_discover(int base_page);
    // Background TTL refresh for the active chart tab (spec 1a): no clear,
    // no Loading state; swap happens in apply_pending only when the result
    // is ok and non-empty.
    void revalidate_active_chart();
    // True when the committed filter state for the active chart tab routes
    // it through /discover (extracted from reload_for_category).
    bool active_chart_filters_active() const;
```

- [ ] **Step 2: Rewrite the load/publish/apply path in `browse_screen.cpp`**

**`load_category`** (line 469): add window reset. After `loaded_tmdb_ids_.clear();` (line 486) insert:

```cpp
    page_window_base_ = 1;
    shuffle_retry_base1_ = false;
```

**New entry points** — add after `load_category`:

```cpp
void BrowseScreen::load_shuffle(Category cat, int base_page) {
    // Mirror load_category's synchronous reset (spec 1b: the reset cannot be
    // left to apply_pending — its replace branch only fires for the window
    // base page, and spawn_page_worker alone does not bump the generation).
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    more_available_ = true;
    fetching_more_ = false;
    loaded_tmdb_ids_.clear();
    shuffle_retry_base1_ = false;
    page_window_base_ = base_page;
    loading_ = true;
    tmdb_current_gen_.fetch_add(1);
    spdlog::info("[BrowseScreen] load_shuffle: {} base={} (gen={})",
                 label_for_category(cat), base_page, tmdb_current_gen_.load());
    spawn_page_worker(cat, base_page);
}

void BrowseScreen::load_shuffle_discover(int base_page) {
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    more_available_ = true;
    fetching_more_ = false;
    loaded_tmdb_ids_.clear();
    shuffle_retry_base1_ = false;
    page_window_base_ = base_page;
    loading_ = true;
    tmdb_current_gen_.fetch_add(1);
    spdlog::info("[BrowseScreen] load_shuffle_discover: base={} (gen={})",
                 base_page, tmdb_current_gen_.load());
    spawn_page_worker(Category::Filter, base_page);
}

bool BrowseScreen::active_chart_filters_active() const {
    if (category_ != Category::Popular && category_ != Category::TopRated) return false;
    FilterTabKind tab = (category_ == Category::Popular)
                        ? FilterTabKind::Popular : FilterTabKind::TopRated;
    return any_filter_active(read_filter_state(state_.display_settings, tab), tab);
}

void BrowseScreen::revalidate_active_chart() {
    // Spec 1a stale-while-revalidate: bump the generation NOW so in-flight
    // pages from the old (possibly shuffled) window are dropped rather than
    // appended after the swap, reset the window base to 1, but defer every
    // visible reset (grid, cursor, Loading) to the swap in apply_pending.
    tmdb_current_gen_.fetch_add(1);
    page_window_base_ = 1;
    shuffle_retry_base1_ = false;
    fetching_more_ = true;
    next_page_to_fetch_ = 2;
    const uint64_t gen = tmdb_current_gen_.load();
    spdlog::info("[BrowseScreen] TTL revalidate: {} (gen={})",
                 label_for_category(category_), gen);
    if (active_chart_filters_active()) {
        FilterTabKind tab = (category_ == Category::Popular)
                            ? FilterTabKind::Popular : FilterTabKind::TopRated;
        current_filter_ = build_discover_filter(
            read_filter_state(state_.display_settings, tab), tab);
        tmdb_workers_.spawn([this, gen, filter = current_filter_]() {
            run_reload_filter_page(gen, filter, /*page=*/1, /*is_revalidate=*/true);
        });
    } else {
        tmdb_workers_.spawn([this, gen, cat = category_]() {
            run_load_page(gen, cat, /*page=*/1, /*is_revalidate=*/true);
        });
    }
}
```

**`run_load_page`** (line 526) — full replacement:

```cpp
void BrowseScreen::run_load_page(uint64_t gen, Category cat, int page,
                                 bool is_revalidate) {
    TmdbList list;
    switch (cat) {
        case Category::Popular:    list = tmdb_.get_popular(page);     break;
        case Category::NowPlaying: list = tmdb_.get_now_playing(page); break;
        case Category::TopRated:   list = tmdb_.get_top_rated(page);   break;
        case Category::Upcoming:   list = tmdb_.get_upcoming(page);    break;
        default: break;
    }
    const bool no_more = list.hits.size() < 5;
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] page={} gen={} stale at publish (current={}); discarding",
                     page, gen, tmdb_current_gen_.load());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;
        PendingPage pp;
        pp.movies = std::move(list.hits);
        pp.page = page;
        pp.no_more = no_more;
        pp.ok = list.ok;
        pp.total_pages = list.total_pages;
        pp.is_revalidate = is_revalidate;
        tmdb_pending_pages_.push_back(std::move(pp));
    }
    tmdb_result_ready_.store(true);
}
```

**`run_reload_filter_page`** (line 602) — full replacement:

```cpp
void BrowseScreen::run_reload_filter_page(uint64_t gen, DiscoverFilter filter,
                                          int page, bool is_revalidate) {
    auto list = tmdb_.discover(filter, page);
    const bool no_more = list.hits.size() < 5;
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] discover page={} gen={} stale; discarding",
                     page, gen);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;
        PendingPage pp;
        pp.movies = std::move(list.hits);
        pp.page = page;
        pp.no_more = no_more;
        pp.ok = list.ok;
        pp.total_pages = list.total_pages;
        pp.is_revalidate = is_revalidate;
        pp.discover_sig = TmdbClient::build_discover_url("", filter, 1);
        tmdb_pending_pages_.push_back(std::move(pp));
    }
    tmdb_result_ready_.store(true);
}
```

**`apply_pending`** (line 619) — replace the drain loop body (keep the drain/sort preamble):

```cpp
    for (auto& pp : drained) {
        // total_pages cache for the discover shuffle clamp (spec 1b) — keyed
        // by filter signature, learned from any discover page that reports it.
        if (!pp.discover_sig.empty() && pp.total_pages > 0) {
            discover_total_pages_[pp.discover_sig] = pp.total_pages;
        }
        // Background revalidate that failed or came back empty: skip the swap
        // entirely — the old grid survives (spec 1a). Freeze pagination for
        // the stale grid: its window state no longer matches its content.
        if (pp.is_revalidate && (!pp.ok || pp.movies.empty())) {
            spdlog::warn("[BrowseScreen] TTL revalidate failed (ok={}, hits={}); keeping stale grid",
                         pp.ok, pp.movies.size());
            fetching_more_ = false;
            more_available_ = false;
            continue;
        }
        // A shuffled base page that is genuinely empty (ok, 0 hits — possible
        // on narrow /discover filters): fall back to page 1 (spec 1b).
        if (!pp.is_revalidate && pp.page == page_window_base_ &&
            page_window_base_ != 1 && pp.ok && pp.movies.empty()) {
            spdlog::info("[BrowseScreen] shuffled base {} empty; falling back to page 1",
                         page_window_base_);
            shuffle_retry_base1_ = true;
            continue;
        }
        size_t added = 0, dups = 0;
        if (pp.page == page_window_base_) {
            // Window-base page — canonical replacement (was hardcoded page 1).
            movies_.clear();
            loaded_tmdb_ids_.clear();
            movies_.reserve(pp.movies.size());
            for (auto& m : pp.movies) {
                if (loaded_tmdb_ids_.insert(m.tmdb_id).second) {
                    movies_.push_back(std::move(m));
                    ++added;
                } else {
                    ++dups;
                }
            }
            grid_cursor_ = 0;
            scroll_row_ = 0;
            // Timestamp rule (spec 1a/1b): the grid is fresh whenever its
            // base page lands ok and non-empty — normal load, shuffle, or
            // revalidate alike.
            if (pp.ok && !movies_.empty()) {
                chart_loaded_at_ = std::chrono::steady_clock::now();
            }
        } else {
            movies_.reserve(movies_.size() + pp.movies.size());
            for (auto& m : pp.movies) {
                if (loaded_tmdb_ids_.insert(m.tmdb_id).second) {
                    movies_.push_back(std::move(m));
                    ++added;
                } else {
                    ++dups;
                }
            }
        }
        if (pp.no_more) more_available_ = false;
        spdlog::info("[BrowseScreen] applied page {}: +{} movies, {} dup(s) "
                     "skipped (total {})",
                     pp.page, added, dups, movies_.size());
    }
    if (!drained.empty()) {
        loading_ = false;
        fetching_more_ = false;
    }
    // Deferred outside the drain loop so we don't mutate pagination state
    // mid-iteration: rerun the shuffle as a plain page-1 load.
    if (shuffle_retry_base1_) {
        shuffle_retry_base1_ = false;
        if (active_chart_filters_active()) load_shuffle_discover(1);
        else load_shuffle(category_, 1);
    }
```

**`maybe_load_more_pages`** (line 676) — replace the cap check and prefetch condition:

```cpp
    if (fetching_more_ || loading_) return;
    if (!more_available_) return;
    if (is_nav_chip(category_)) return;
    // Base-relative window (spec 1b): load [base, base+kMaxLoadedPages-1].
    if (next_page_to_fetch_ > window_last_page(page_window_base_, kMaxLoadedPages)) return;

    const int rows_loaded = movies_.empty()
        ? 0
        : (static_cast<int>(movies_.size()) + kGridCols - 1) / kGridCols;
    const int cursor_row = grid_cursor_ / kGridCols;
    const bool prefetch_second = (next_page_to_fetch_ == page_window_base_ + 1);
    const bool near_end = (rows_loaded > 0) && (cursor_row >= rows_loaded - 1);
    if (!prefetch_second && !near_end) return;

    spdlog::info("[BrowseScreen] auto-fetching page {} ({})",
                 next_page_to_fetch_,
                 prefetch_second ? "second-page prefetch" : "scroll-driven");
    spawn_page_worker(category_, next_page_to_fetch_);
```

**`enter`** (line 275) — replace with:

```cpp
void BrowseScreen::enter() {
    want_search_screen_ = false;
    if (!loaded_) {
        load_category(category_);
        loaded_ = true;
    } else if (!is_nav_chip(category_) && category_ != Category::Filter &&
               tmdb_grid_stale(chart_loaded_at_, std::chrono::steady_clock::now())) {
        // Spec 1a: 6h TTL, evaluated on enter() only (tab switches already
        // refetch unconditionally). Stale-while-revalidate — the old grid
        // stays on screen until a fresh page 1 lands.
        revalidate_active_chart();
    }
    refresh_library_async();
}
```

(Task 6 extends this `else if` chain with the For You branch.)

- [ ] **Step 3: Compile-verify on the Pi**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box " && MEDIA_BROWSER=true ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```
Expected: clean build. Quick behavioral sanity on the box (service restarts with deploy): enter the marquee twice within a minute — second entry must NOT refetch (no `TTL revalidate` log line); `journalctl -u magic-dingus-box-cpp -n 50` shows normal `load_category` lines only.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/browse_screen.h magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp
git commit -m "feat(mb): window-relative pagination, shuffle load path, 6h stale-while-revalidate TTL"
```

---

### Task 4: Filter-overlay SHUFFLE row + commit-skip (spec 1b UI)

**Files:**
- Modify: `src/media_browser/ui/mb_filter_overlay.h`
- Modify: `src/media_browser/ui/mb_filter_overlay.cpp`
- Modify: `src/media_browser/ui/browse_screen.h` (persist helper + `do_shuffle` decls)
- Modify: `src/media_browser/ui/browse_screen.cpp` (BTN4 handler wiring)

**Interfaces:**
- Consumes: `load_shuffle`/`load_shuffle_discover`, `discover_total_pages_`, `page_window_base_` (Task 3); `pick_shuffle_base`, `discover_max_base`, `kShuffleMaxBasePopular/TopRated` (Task 2).
- Produces:
  - `FilterTabKind::ForYou` enum value (wired to Browse in Task 6).
  - `FilterState` equality: `bool operator==(const FilterState&, const FilterState&)` (+ `!=`).
  - `FilterOverlay::set_on_shuffle(ShuffleCallback)` with `using ShuffleCallback = std::function<void(const FilterState&, FilterTabKind)>;`
  - Per-tab row model: `int row_count() const`, `enum class RowRole { Value, Reset, Shuffle }`, `RowRole role_for_row(int) const`. Popular/TopRated = 8 rows (0-5 values, 6 Reset, 7 Shuffle); ForYou = 1 row (Shuffle).
  - Commit-skip: BTN4 close fires `on_commit_` only when `working_ != opened_` (the open()-time snapshot).
  - `BrowseScreen::persist_filter_state(FilterTabKind, const FilterState&)` and `BrowseScreen::do_shuffle()`.

- [ ] **Step 1: Overlay header changes**

In `src/media_browser/ui/mb_filter_overlay.h`:

Extend the tab enum (line 14):

```cpp
enum class FilterTabKind {
    Popular,
    TopRated,
    ForYou,   // SHUFFLE-only overlay in Phase 1 (spec 1c)
};
```

Add equality after the `FilterState` struct (line 29):

```cpp
inline bool operator==(const FilterState& a, const FilterState& b) {
    return a.genre_mask == b.genre_mask && a.decade == b.decade &&
           a.min_rating == b.min_rating && a.runtime == b.runtime &&
           a.language == b.language && a.sort == b.sort;
}
inline bool operator!=(const FilterState& a, const FilterState& b) { return !(a == b); }
```

In the class: add beside `set_on_commit` (line 78):

```cpp
    // SHUFFLE row callback (spec 1b). The overlay closes itself via the
    // commit-free close() before firing; the handler persists any staged
    // edits and performs exactly one shuffle load.
    using ShuffleCallback = std::function<void(const FilterState&, FilterTabKind)>;
    void set_on_shuffle(ShuffleCallback cb) { on_shuffle_ = std::move(cb); }
```

In the private section: delete `static constexpr int kFocusableRowCount = 7;` (line 100) and add:

```cpp
    // Per-tab row model (spec 1b): the row count and roles vary by tab kind.
    // Popular/TopRated: rows 0-5 = filter/sort values, 6 = RESET ALL,
    // 7 = SHUFFLE. ForYou: single SHUFFLE row.
    enum class RowRole { Value, Reset, Shuffle };
    bool has_filter_rows() const { return tab_ != FilterTabKind::ForYou; }
    int  row_count() const { return has_filter_rows() ? 8 : 1; }
    RowRole role_for_row(int row) const {
        if (!has_filter_rows()) return RowRole::Shuffle;
        if (row == 6) return RowRole::Reset;
        if (row == 7) return RowRole::Shuffle;
        return RowRole::Value;
    }
    FilterState opened_;          // snapshot at open() — commit skipped when unchanged
    ShuffleCallback on_shuffle_;
    void render_shuffle_row(::ui::Renderer& r, int panel_x, int x, int y,
                            bool focused);
```

- [ ] **Step 2: Overlay implementation changes**

In `src/media_browser/ui/mb_filter_overlay.cpp`:

`open()` (line 115): add `opened_ = current;` immediately after `working_ = current;`.

`on_rotate()` (line 210-213): replace the two `kFocusableRowCount` uses with `row_count()`:

```cpp
        int new_focus = focus_row_ + (delta > 0 ? 1 : -1);
        if (new_focus < 0)               new_focus = 0;
        if (new_focus >= row_count())    new_focus = row_count() - 1;
        focus_row_ = new_focus;
```

`on_select()` (line 224): replace the RowSelect branch:

```cpp
    } else {
        // RowSelect
        switch (role_for_row(focus_row_)) {
            case RowRole::Reset:
                // RESET ALL — clear staging area, stay in RowSelect, no commit.
                working_ = FilterState{};
                break;
            case RowRole::Shuffle:
                // SHUFFLE — close (commit-free path) and hand staged state to
                // the handler, which persists it and performs one shuffle load.
                if (on_shuffle_) on_shuffle_(working_, tab_);
                close();
                break;
            case RowRole::Value:
                mode_ = Mode::ValueSelect;
                break;
        }
    }
    return true;
```

`on_btn4_close()` (line 258-260): replace the commit lines:

```cpp
    // RowSelect: apply staged changes in one shot — but only when something
    // actually changed since open(). Unconditional commit used to refetch the
    // tab on every overlay peek, which would silently replace a shuffled grid
    // with the canonical chart (spec 1b).
    if (on_commit_ && working_ != opened_) on_commit_(working_, tab_);
    close();
    return true;
```

`render()` — two changes. First, at the top after the panel chrome (`content_x` line), short-circuit the ForYou variant:

```cpp
    // ForYou variant (spec 1c): title + single SHUFFLE row, nothing else.
    if (!has_filter_rows()) {
        const int title_baseline2 = kOverlayPanelTopY + kOverlayPanelInnerPadY +
                                    kPanelTitleFontPx - 2;
        r.mb_draw_title_text("FOR YOU",
                             static_cast<float>(content_x),
                             static_cast<float>(title_baseline2),
                             kPanelTitleFontPx, th.accent);
        const int row_y = title_baseline2 + 20 + kOverlaySectionGap;
        render_shuffle_row(r, panel_x, content_x, row_y, focus_row_ == 0);
        const int hint_y2 = kOverlayPanelBottomY - kOverlayPanelInnerPadY;
        chrome::draw_hint_row(r, content_x, hint_y2, {
            {chrome::HintIcon::Btn4Black,   "Close"},
            {chrome::HintIcon::RotaryPress, "Shuffle"},
        });
        return;
    }
```

Second, insert the SHUFFLE row after the RESET ALL row (after `render_reset_row(...)`, line ~458 — the row fits: reset row bottom is ~513, panel bottom 634, footer hint 618):

```cpp
    // ---- SHUFFLE row (row 7) ----
    const int shuffle_row_y = reset_row_y + kOverlayRowHeight;
    render_shuffle_row(r, panel_x, content_x, shuffle_row_y, (focus_row_ == 7));
```

Add the row renderer next to `render_reset_row` (line 314):

```cpp
void FilterOverlay::render_shuffle_row(::ui::Renderer& r, int panel_x, int x, int y,
                                        bool focused) {
    const auto& th = r.mb_theme();
    if (focused) draw_cursor_marker(r, panel_x, y, th.accent);
    const float fy = static_cast<float>(y + kOverlayRowHeight - 10);
    r.mb_draw_text("SHUFFLE", static_cast<float>(x + 18), fy,
                   kRowFontPx, focused ? th.accent : th.dim);
    const char* hint = "press for new results";
    const int right_x = x + kOverlayPanelW - 2 * kOverlayPanelInnerPadX;
    const int tw = r.mb_text_width(hint, kSectionHeadingFontPx);
    r.mb_draw_text(hint, static_cast<float>(right_x - tw), fy,
                   kSectionHeadingFontPx, th.fg);
}
```

- [ ] **Step 3: BrowseScreen wiring**

In `src/media_browser/ui/browse_screen.h`, declare next to `reload_for_category()`:

```cpp
    // Shared persist half of the overlay commit (spec 1b): write per-tab
    // filter state + save settings.json, WITHOUT the reload — the commit
    // path adds reload_for_category(), the shuffle path adds do_shuffle().
    void persist_filter_state(FilterTabKind tab, const FilterState& fs);
    // Spec 1b shuffle dispatch for the active tab.
    void do_shuffle();
```

In `src/media_browser/ui/browse_screen.cpp`, add `#include <random>` to the includes, then add the two methods (near `reload_for_category`):

```cpp
void BrowseScreen::persist_filter_state(FilterTabKind tab, const FilterState& fs) {
    // ForYou keeps no persisted filter state in Phase 1 (spec 1c).
    if (tab == FilterTabKind::ForYou) return;
    write_filter_state(state_.display_settings, tab, fs);
    ::app::SettingsPersistence::save_settings(state_);
}

void BrowseScreen::do_shuffle() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    if (category_ == Category::Popular || category_ == Category::TopRated) {
        if (active_chart_filters_active()) {
            FilterTabKind tab = (category_ == Category::Popular)
                                ? FilterTabKind::Popular : FilterTabKind::TopRated;
            current_filter_ = build_discover_filter(
                read_filter_state(state_.display_settings, tab), tab);
            const std::string sig =
                TmdbClient::build_discover_url("", current_filter_, 1);
            const auto it = discover_total_pages_.find(sig);
            const int max_base =
                discover_max_base(it == discover_total_pages_.end() ? 0 : it->second,
                                  kMaxLoadedPages);
            load_shuffle_discover(
                pick_shuffle_base(page_window_base_, max_base, rng()));
        } else {
            const int max_base = (category_ == Category::Popular)
                                 ? kShuffleMaxBasePopular : kShuffleMaxBaseTopRated;
            load_shuffle(category_,
                         pick_shuffle_base(page_window_base_, max_base, rng()));
        }
    }
    // Category::ForYou is wired in the For You task — resample_foryou(false).
}
```

In `handle_input`'s BTN4 handler (line 864-879), replace the open/commit block:

```cpp
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            if (filter_overlay_.is_visible()) {
                filter_overlay_.on_btn4_close();
            } else if (category_ == Category::Popular ||
                       category_ == Category::TopRated) {
                FilterTabKind tk = (category_ == Category::Popular)
                                   ? FilterTabKind::Popular
                                   : FilterTabKind::TopRated;
                filter_overlay_.open(tk, read_filter_state(state_.display_settings, tk));
                filter_overlay_.set_on_commit(
                    [this](const FilterState& fs, FilterTabKind tk2) {
                        this->persist_filter_state(tk2, fs);
                        this->reload_for_category();
                    });
                filter_overlay_.set_on_shuffle(
                    [this](const FilterState& fs, FilterTabKind tk2) {
                        this->persist_filter_state(tk2, fs);
                        this->do_shuffle();
                    });
            }
            continue;
        }
```

- [ ] **Step 4: Compile-verify on the Pi + hardware sanity**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box " && MEDIA_BROWSER=true ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```
Expected: clean build. On the TV: (1) BTN4 on Popular → overlay shows RESET ALL + SHUFFLE; (2) select SHUFFLE → overlay closes, grid reloads with different movies; (3) BTN4 open → BTN4 close with no edits → grid does NOT reload (log shows no `load_category`); (4) set a genre, select SHUFFLE → results respect the genre and settings.json records it.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/mb_filter_overlay.h magic_dingus_box_cpp/src/media_browser/ui/mb_filter_overlay.cpp magic_dingus_box_cpp/src/media_browser/ui/browse_screen.h magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp
git commit -m "feat(mb): SHUFFLE overlay row with per-tab row model and unchanged-commit skip"
```

---

### Task 5: `merge_recommendations` + library success signal (spec 1c foundations)

**Files:**
- Create: `src/media_browser/ui/mb_recs.h`, `src/media_browser/ui/mb_recs.cpp`
- Create: `tests/media_browser/test_mb_recs.cpp`
- Modify: `CMakeLists.txt` (`mb_recs.cpp` → `MEDIA_BROWSER_SOURCES`; test file → `MEDIA_BROWSER_TEST_SOURCES`)
- Modify: `src/media_browser/radarr/radarr_client.h`, `radarr_client.cpp` (checked library fetch)
- Modify: `src/media_browser/radarr/radarr_mock.h`, `radarr_mock.cpp` (override)
- Modify: `tests/media_browser/test_radarr_client.cpp` (mock coverage)

**Interfaces:**
- Produces:
  - `std::vector<TmdbSearchHit> media_browser::ui::merge_recommendations(const std::vector<std::vector<TmdbSearchHit>>& per_seed, const std::unordered_set<int>& exclude, int cap = 100);` — dedup by tmdb_id; score = distinct seed count; ties by min index across seed lists, then ascending tmdb_id; drops `exclude` members and `tmdb_id <= 0`; caps at exactly `cap`.
  - `virtual std::optional<std::vector<Movie>> RadarrClient::get_library_checked();` — `std::nullopt` on HTTP failure, a (possibly empty) vector on success. `get_library()` becomes a thin wrapper: `return get_library_checked().value_or(std::vector<Movie>{});`. `RadarrMockClient` overrides `get_library_checked` to return its canned `library_`.
- Consumed by: Task 6.

- [ ] **Step 1: Write the failing merge tests**

Create `tests/media_browser/test_mb_recs.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "media_browser/ui/mb_recs.h"

using media_browser::TmdbSearchHit;
using media_browser::ui::merge_recommendations;

namespace {
TmdbSearchHit mk(int id) {
    TmdbSearchHit h;
    h.tmdb_id = id;
    h.title = "Movie " + std::to_string(id);
    return h;
}
}  // namespace

TEST_CASE("merge: seed-count score dominates", "[mb_recs]") {
    // id 7 recommended by 2 seeds; id 9 by 1 seed at index 0.
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(9), mk(7)},
        {mk(7)},
    };
    auto out = merge_recommendations(per_seed, {});
    REQUIRE(out.size() == 2);
    CHECK(out[0].tmdb_id == 7);
    CHECK(out[1].tmdb_id == 9);
}

TEST_CASE("merge: min-index breaks equal seed counts", "[mb_recs]") {
    // Both ids appear in exactly one seed list; 21 at index 0 beats 20 at index 2.
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(5), mk(6), mk(20)},
        {mk(21)},
    };
    auto out = merge_recommendations(per_seed, {});
    REQUIRE(out.size() == 4);
    CHECK(out[0].tmdb_id == 5);    // count 1, min-index 0, id 5
    CHECK(out[1].tmdb_id == 21);   // count 1, min-index 0, id 21
    CHECK(out[2].tmdb_id == 6);    // count 1, min-index 1
    CHECK(out[3].tmdb_id == 20);   // count 1, min-index 2
}

TEST_CASE("merge: ascending tmdb_id breaks full ties", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk(300), mk(100)}, {mk(100), mk(300)}};
    auto out = merge_recommendations(per_seed, {});
    REQUIRE(out.size() == 2);
    // Both: count 2, min-index 0 → lower id first.
    CHECK(out[0].tmdb_id == 100);
    CHECK(out[1].tmdb_id == 300);
}

TEST_CASE("merge: excludes library ids and non-positive ids", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {{mk(1), mk(2), mk(0), mk(-3)}};
    auto out = merge_recommendations(per_seed, {2});
    REQUIRE(out.size() == 1);
    CHECK(out[0].tmdb_id == 1);
}

TEST_CASE("merge: duplicate within one seed list counts once", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed = {
        {mk(4), mk(4), mk(4)},   // one seed, repeated — count must stay 1
        {mk(8)}, {mk(8)},        // two distinct seeds — count 2
    };
    auto out = merge_recommendations(per_seed, {});
    REQUIRE(out.size() == 2);
    CHECK(out[0].tmdb_id == 8);
    CHECK(out[1].tmdb_id == 4);
}

TEST_CASE("merge: caps at exactly cap", "[mb_recs]") {
    std::vector<std::vector<TmdbSearchHit>> per_seed(1);
    for (int i = 1; i <= 150; ++i) per_seed[0].push_back(mk(i));
    auto out = merge_recommendations(per_seed, {});
    CHECK(out.size() == 100);          // default cap (spec: exactly 100)
    auto out3 = merge_recommendations(per_seed, {}, 3);
    REQUIRE(out3.size() == 3);
    CHECK(out3[0].tmdb_id == 1);       // count 1, min-index 0
}
```

Add to `CMakeLists.txt`: `src/media_browser/ui/mb_recs.cpp` at the end of `MEDIA_BROWSER_SOURCES` (after `library_view.cpp`), and `tests/media_browser/test_mb_recs.cpp` after `test_browse_logic.cpp` in `MEDIA_BROWSER_TEST_SOURCES`.

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build-mb --target test_media_browser_unit -j8
```
Expected: compile error — `mb_recs.h` not found.

- [ ] **Step 3: Implement merge**

Create `src/media_browser/ui/mb_recs.h`:

```cpp
#pragma once

#include <unordered_set>
#include <vector>

#include "media_browser/tmdb_client.h"

// Pure merge/rank for the For You tab (spec 1c step 4). Renderer-free so
// test_media_browser_unit can assert on it.

namespace media_browser::ui {

inline constexpr int kForYouCap = 100;

// Merge per-seed TMDB recommendation lists into one ranked grid:
//   score  = number of DISTINCT seeds recommending the title
//   tie 1  = minimum index the title holds across all seed lists
//   tie 2  = ascending tmdb_id
// Drops exclude-set members (the library) and non-positive ids; duplicate
// rows within one seed list count once. Result capped at exactly `cap`.
// Inputs arrive already family-safe-trimmed by TmdbClient's list parser.
std::vector<TmdbSearchHit> merge_recommendations(
    const std::vector<std::vector<TmdbSearchHit>>& per_seed,
    const std::unordered_set<int>& exclude,
    int cap = kForYouCap);

}  // namespace media_browser::ui
```

Create `src/media_browser/ui/mb_recs.cpp`:

```cpp
#include "media_browser/ui/mb_recs.h"

#include <algorithm>
#include <unordered_map>

namespace media_browser::ui {

std::vector<TmdbSearchHit> merge_recommendations(
    const std::vector<std::vector<TmdbSearchHit>>& per_seed,
    const std::unordered_set<int>& exclude,
    int cap) {
    struct Entry {
        TmdbSearchHit hit;
        int seed_count = 0;
        int min_index = 0;
    };
    std::unordered_map<int, Entry> by_id;
    for (const auto& seed_list : per_seed) {
        std::unordered_set<int> seen_this_seed;  // same seed repeating a title counts once
        for (int idx = 0; idx < static_cast<int>(seed_list.size()); ++idx) {
            const auto& hit = seed_list[idx];
            if (hit.tmdb_id <= 0) continue;
            if (exclude.count(hit.tmdb_id) > 0) continue;
            if (!seen_this_seed.insert(hit.tmdb_id).second) continue;
            auto [it, inserted] = by_id.try_emplace(hit.tmdb_id);
            Entry& e = it->second;
            if (inserted) {
                e.hit = hit;
                e.min_index = idx;
            } else if (idx < e.min_index) {
                e.min_index = idx;
            }
            ++e.seed_count;
        }
    }
    std::vector<Entry> entries;
    entries.reserve(by_id.size());
    for (auto& [id, e] : by_id) entries.push_back(std::move(e));
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.seed_count != b.seed_count) return a.seed_count > b.seed_count;
        if (a.min_index != b.min_index) return a.min_index < b.min_index;
        return a.hit.tmdb_id < b.hit.tmdb_id;
    });
    if (static_cast<int>(entries.size()) > cap) entries.resize(cap);
    std::vector<TmdbSearchHit> out;
    out.reserve(entries.size());
    for (auto& e : entries) out.push_back(std::move(e.hit));
    return out;
}

}  // namespace media_browser::ui
```

- [ ] **Step 4: Run merge tests**

```bash
cmake --build build-mb --target test_media_browser_unit -j8 && ./build-mb/test_media_browser_unit "[mb_recs]"
```
Expected: `All tests passed` (6 test cases).

- [ ] **Step 5: Library success signal (test-first)**

Append to `tests/media_browser/test_radarr_client.cpp`:

```cpp
TEST_CASE("RadarrMockClient::get_library_checked reports success with canned data",
          "[radarr][mock]") {
    media_browser::RadarrMockClient mock;
    auto checked = mock.get_library_checked();
    REQUIRE(checked.has_value());
    CHECK(checked->size() == mock.get_library().size());
}
```

Run `cmake --build build-mb --target test_media_browser_unit -j8` — expected compile error (`get_library_checked` undeclared).

Implement — `src/media_browser/radarr/radarr_client.h`, next to `get_library()` (line 45):

```cpp
    // Library fetch with an explicit success signal (spec 1c): nullopt on
    // HTTP failure, a possibly-empty vector on success. get_library() keeps
    // its old bare-vector shape as a wrapper — an empty library and a failed
    // fetch were previously indistinguishable, which would have made For You
    // show "add movies to your library" on a full box whenever the GET
    // failed after a successful ping.
    virtual std::optional<std::vector<Movie>> get_library_checked();
```

`src/media_browser/radarr/radarr_client.cpp` — replace `get_library()` (line 150):

```cpp
std::optional<std::vector<Movie>> RadarrClient::get_library_checked() {
    auto resp = http_get("/api/v3/movie");
    if (resp.empty()) return std::nullopt;
    return RadarrParsers::parse_movie_list(resp);
}

std::vector<Movie> RadarrClient::get_library() {
    return get_library_checked().value_or(std::vector<Movie>{});
}
```

`src/media_browser/radarr/radarr_mock.h` — add beside the `get_library` override (line 16):

```cpp
    std::optional<std::vector<Movie>> get_library_checked() override;
```

`src/media_browser/radarr/radarr_mock.cpp` — add beside the `get_library` definition (line 60):

```cpp
std::optional<std::vector<Movie>> RadarrMockClient::get_library_checked() {
    return library_;
}
```

(Note: the mock must override `get_library_checked` — the base `get_library()` wrapper routes through it virtually, so the mock's canned library now serves both shapes.)

- [ ] **Step 6: Run full Mac suite**

```bash
cmake --build build-mb --target test_media_browser_unit -j8 && ./build-mb/test_media_browser_unit
```
Expected: all green, including the new `[radarr][mock]` case.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/mb_recs.h magic_dingus_box_cpp/src/media_browser/ui/mb_recs.cpp magic_dingus_box_cpp/tests/media_browser/test_mb_recs.cpp magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp magic_dingus_box_cpp/src/media_browser/radarr/radarr_mock.h magic_dingus_box_cpp/src/media_browser/radarr/radarr_mock.cpp magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): merge_recommendations pure ranker + get_library_checked success signal"
```

---

### Task 6: The For You tab (spec 1c integration)

Kiosk-only integration of Tasks 2+5's tested pieces.

**Files:**
- Modify: `src/media_browser/ui/browse_screen.h`
- Modify: `src/media_browser/ui/browse_screen.cpp`
- Modify: `src/media_browser/ui/mb_chrome.cpp:41,43` (strip spacing)

**Interfaces:**
- Consumes: `merge_recommendations` (Task 5), `decide_foryou_entry`/`ForYouEntry` (Task 2), `get_library_checked` (Task 5), `TmdbList` (Task 1), `do_shuffle` (Task 4).
- Produces: `Category::ForYou`; the shared `kVisibleTabs` member array; For You state machine (`activate_foryou`, `start_foryou_sample`, `apply_foryou_pending`).

- [ ] **Step 1: Header changes (`browse_screen.h`)**

Renumber the `Category` enum (line 72) — ForYou is a CONTENT category, so it must sit below the `is_nav_chip` boundary:

```cpp
    enum class Category {
        Popular = 0,
        NowPlaying = 1,
        TopRated = 2,
        Upcoming = 3,
        Filter = 4,
        ForYou = 5,
        Search = 6,
        Library = 7,
        Queue = 8,
        Settings = 9,
    };
```

Update the counts (lines 92-93): `kNumContentCategories = 6;  // Popular..ForYou` and `kNumCategories = 10;` (Category values are never persisted — `settings.json` stores only the per-tab filter fields — so renumbering is safe.)

Add the shared strip array (C++17 inline static member — replaces the two duplicated function-local arrays; spec 1c targeted cleanup):

```cpp
    // Single source of truth for the Marquee strip — consumed by BOTH
    // handle_input() and render(). Was duplicated in the two functions
    // with a "keep in sync" comment.
    static constexpr Category kVisibleTabs[] = {
        Category::Popular,
        Category::TopRated,
        Category::ForYou,
        Category::Search,
        Category::Library,
        Category::Queue,
        Category::Settings,
    };
    static constexpr int kNumVisibleTabs =
        static_cast<int>(sizeof(kVisibleTabs) / sizeof(kVisibleTabs[0]));
```

Add the For You state block (after the quick-add caches):

```cpp
    // --- For You state (spec 1c) -----------------------------------
    // Cached merged list — activation re-renders this without refetching;
    // a new sample runs only on first entry, TTL expiry, or SHUFFLE.
    std::vector<TmdbSearchHit> foryou_movies_;
    std::chrono::steady_clock::time_point foryou_loaded_at_{};
    // One in-flight sample job. Workers capture the shared_ptr; a stale job
    // (gen mismatch) is simply never consumed. remaining==0 → ready to merge.
    struct SeedResult {
        bool ok = false;                    // TmdbList.ok of whichever call served it
        std::vector<TmdbSearchHit> hits;
    };
    struct ForYouJob {
        uint64_t gen = 0;
        bool background = false;            // TTL refresh — keep old grid on total failure
        std::atomic<int> remaining{0};
        std::mutex mtx;
        std::vector<SeedResult> results;
    };
    std::shared_ptr<ForYouJob> foryou_job_;
    bool foryou_waiting_for_library_ = false;  // sample deferred until refresh lands
    bool foryou_failed_ = false;               // all seeds failed on an explicit load
    // Library-refresh outcome flags (spec 1c): set by apply_library_pending.
    bool lib_refresh_done_once_ = false;
    bool lib_fetch_ok_ = false;
    void activate_foryou();
    void start_foryou_sample(bool background);
    void apply_foryou_pending();
```

Add `#include <memory>` and `#include "media_browser/ui/mb_recs.h"` to the header's includes. Extend `PendingLibrary` (line 287) with `bool library_fetch_ok = false;`.

- [ ] **Step 2: Implementation (`browse_screen.cpp`)**

`label_for_category` (line 430): add `case Category::ForYou: return "For You";` after TopRated.

**Delete** the two function-local `kVisibleTabs`/`kNumVisibleTabs` definitions in `handle_input` (lines 810-819) and `render` (lines 976-985) — both functions now use the class members directly (no other edits needed there; same names).

`run_library_refresh` (line 304): replace the `get_library()` call block:

```cpp
    if (r.services_ok) {
        auto lib_checked = radarr_.get_library_checked();
        r.library_fetch_ok = lib_checked.has_value();
        const auto& lib = lib_checked ? *lib_checked : std::vector<Movie>{};
```

(The loop over `lib` below is unchanged.)

`apply_library_pending` (line 345): after `services_ok_ = r.services_ok;` add:

```cpp
    lib_refresh_done_once_ = true;
    lib_fetch_ok_ = r.services_ok && r.library_fetch_ok;
```

And at the end of the function (after the `if (r.services_ok)` block):

```cpp
    // For You deferred-sample hook (spec 1c entry rule case b).
    if (foryou_waiting_for_library_ && category_ == Category::ForYou) {
        foryou_waiting_for_library_ = false;
        start_foryou_sample(/*background=*/false);
    }
```

`load_category` (line 469): route ForYou to its own state machine — insert right after the `is_nav_chip` early-return (line 490):

```cpp
    if (cat == Category::ForYou) {
        // For You is exempt from the unconditional tab-activation refetch
        // (spec 1c): activation re-renders the cached merged list when one
        // exists. load_category was already called with cleared grid state.
        activate_foryou();
        return;
    }
```

Add the state machine (after `apply_library_pending`):

```cpp
void BrowseScreen::activate_foryou() {
    foryou_failed_ = false;
    switch (decide_foryou_entry(!foryou_movies_.empty(), lib_refresh_done_once_,
                                lib_fetch_ok_, library_tmdb_ids_.empty())) {
        case ForYouEntry::UseCache:
            movies_ = foryou_movies_;
            loading_ = false;
            more_available_ = false;   // no scroll-driven pages on For You
            fetching_more_ = false;
            break;
        case ForYouEntry::WaitForLibrary:
            loading_ = true;
            foryou_waiting_for_library_ = true;
            refresh_library_async();   // CAS-guarded no-op when already in flight
            break;
        case ForYouEntry::ServiceUnavailable:
        case ForYouEntry::EmptyLibrary:
            loading_ = false;          // render() branches on the flags below
            break;
        case ForYouEntry::Sample:
            start_foryou_sample(/*background=*/false);
            break;
    }
}

void BrowseScreen::start_foryou_sample(bool background) {
    // Sample min(8, library size) seeds uniformly without replacement.
    std::vector<int> pool(library_tmdb_ids_.begin(), library_tmdb_ids_.end());
    if (pool.empty()) { loading_ = false; return; }
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::vector<int> seeds;
    const int want = std::min<int>(8, static_cast<int>(pool.size()));
    std::sample(pool.begin(), pool.end(), std::back_inserter(seeds), want, rng);

    if (!background) {
        movies_.clear();
        grid_cursor_ = 0;
        scroll_row_ = 0;
        loading_ = true;
    }
    more_available_ = false;
    fetching_more_ = false;
    foryou_failed_ = false;
    tmdb_current_gen_.fetch_add(1);
    auto job = std::make_shared<ForYouJob>();
    job->gen = tmdb_current_gen_.load();
    job->background = background;
    job->remaining.store(static_cast<int>(seeds.size()));
    foryou_job_ = job;
    spdlog::info("[BrowseScreen] For You sample: {} seeds (gen={}, background={})",
                 seeds.size(), job->gen, background);
    for (int seed : seeds) {
        tmdb_workers_.spawn([this, job, seed]() {
            // Recommendations with documented get_similar fallback — the same
            // contract the playback overlay uses (spec 1c step 2).
            auto list = tmdb_.get_recommendations(seed, /*page=*/1);
            if (list.hits.empty()) {
                auto fb = tmdb_.get_similar(seed, /*page=*/1);
                if (fb.ok || !list.ok) list = std::move(fb);
            }
            {
                std::lock_guard<std::mutex> lk(job->mtx);
                job->results.push_back({list.ok, std::move(list.hits)});
            }
            job->remaining.fetch_sub(1, std::memory_order_acq_rel);
        });
    }
}

void BrowseScreen::apply_foryou_pending() {
    if (!foryou_job_) return;
    if (foryou_job_->remaining.load(std::memory_order_acquire) != 0) return;
    auto job = std::move(foryou_job_);
    if (job->gen != tmdb_current_gen_.load()) return;  // preempted — discard
    std::vector<SeedResult> results;
    {
        std::lock_guard<std::mutex> lk(job->mtx);
        results = std::move(job->results);
    }
    int ok_seeds = 0;
    std::vector<std::vector<TmdbSearchHit>> per_seed;
    per_seed.reserve(results.size());
    for (auto& sr : results) {
        if (sr.ok) ++ok_seeds;
        per_seed.push_back(std::move(sr.hits));
    }
    if (ok_seeds == 0) {
        // Timestamp rule (spec 1c): all seeds failed → timestamp untouched so
        // the next enter() retries. Explicit load shows the error state;
        // background TTL keeps the old grid.
        spdlog::warn("[BrowseScreen] For You: all {} seeds failed", results.size());
        if (!job->background) {
            loading_ = false;
            foryou_failed_ = true;
        }
        return;
    }
    foryou_movies_ = merge_recommendations(per_seed, library_tmdb_ids_);
    foryou_loaded_at_ = std::chrono::steady_clock::now();
    spdlog::info("[BrowseScreen] For You merged: {} titles from {} ok seed(s)",
                 foryou_movies_.size(), ok_seeds);
    if (category_ == Category::ForYou) {
        movies_ = foryou_movies_;
        grid_cursor_ = 0;
        scroll_row_ = 0;
        loading_ = false;
        more_available_ = false;
        fetching_more_ = false;
    }
}
```

`update()` (line 702): add `apply_foryou_pending();` on the line after `apply_pending();`.

`maybe_load_more_pages` (top of function): add `if (category_ == Category::ForYou) return;` beside the nav-chip guard.

`enter()` — extend Task 3's `else if` chain (ForYou branch FIRST, since ForYou is not a nav chip):

```cpp
    } else if (category_ == Category::ForYou) {
        if (tmdb_grid_stale(foryou_loaded_at_, std::chrono::steady_clock::now()) &&
            !foryou_movies_.empty()) {
            start_foryou_sample(/*background=*/true);   // spec 1a SWR for For You
        } else if (foryou_movies_.empty()) {
            activate_foryou();                          // never loaded — entry rule
        }
    } else if (!is_nav_chip(category_) && category_ != Category::Filter &&
```

`do_shuffle()` (Task 4): replace the trailing comment with:

```cpp
    if (category_ == Category::ForYou) {
        start_foryou_sample(/*background=*/false);  // fresh seed draw, clear + Loading
    }
```

BTN4 handler: extend the open condition to include ForYou:

```cpp
            } else if (category_ == Category::Popular ||
                       category_ == Category::TopRated ||
                       category_ == Category::ForYou) {
                FilterTabKind tk =
                    (category_ == Category::Popular)  ? FilterTabKind::Popular :
                    (category_ == Category::TopRated) ? FilterTabKind::TopRated :
                                                        FilterTabKind::ForYou;
```

(`read_filter_state` is only defined for Popular/TopRated — pass a default state for ForYou: change the `open` call to `filter_overlay_.open(tk, tk == FilterTabKind::ForYou ? FilterState{} : read_filter_state(state_.display_settings, tk));`.)

`render()` — two changes. The footer `filter_available` (line 1016) becomes:

```cpp
    const bool filter_available = (category_ == Category::Popular ||
                                   category_ == Category::TopRated);
    const bool shuffle_only = (category_ == Category::ForYou);
```

and the BTN4 hint line becomes:

```cpp
            {chrome::HintIcon::Btn4Black,
             filter_available ? "Filters" : (shuffle_only ? "Shuffle" : "\xE2\x80\x94")},
```

Insert the For You state branches after the `!services_ok_` branch (line 1033) and before the generic Loading branch:

```cpp
    if (category_ == Category::ForYou) {
        if (lib_refresh_done_once_ && !lib_fetch_ok_) {
            draw_centered_msg("Radarr service offline", th.highlight2);
            draw_baseline_footer();
            return;
        }
        if (foryou_failed_) {
            draw_centered_msg("Couldn't load recommendations \xE2\x80\x94 try again later",
                              th.highlight2);
            draw_baseline_footer();
            return;
        }
        if (!loading_ && movies_.empty() && lib_refresh_done_once_ &&
            library_tmdb_ids_.empty()) {
            draw_centered_msg("Add movies to your library to get recommendations",
                              th.dim);
            draw_baseline_footer();
            return;
        }
    }
```

- [ ] **Step 3: Header fit (`mb_chrome.cpp`)**

The 7-chip strip overlaps the "Marquee" title by ~58 px at the current spacing (spec 1c "Header fit"). Change lines 41 and 43:

```cpp
constexpr int kTabHorizPad       = 10;          // was kPad3 (16) — 7-chip strip fit
constexpr int kTabVertPad        = kPad2;       // Inside-tab vertical padding
constexpr int kTabGap            = 16;          // was kPad4 (24) — 7-chip strip fit
```

- [ ] **Step 4: Compile-verify on the Pi + hardware sanity**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box " && MEDIA_BROWSER=true ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```
Expected: clean build. On the TV: (1) strip shows 7 chips with no title overlap; (2) For You loads a grid of titles NOT in the library; (3) tab away and back — grid identical, no refetch in the journal; (4) BTN4 on For You → SHUFFLE-only overlay; shuffle produces a different grid; (5) with Radarr stopped (`docker stop mdb_radarr`), For You shows "Radarr service offline", not the teach message — restart Radarr after.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/browse_screen.h magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp magic_dingus_box_cpp/src/media_browser/ui/mb_chrome.cpp
git commit -m "feat(mb): For You tab — library-seeded TMDB recommendations with shuffle + TTL"
```

---

### Task 7: Full verification pass

**Files:**
- Modify: `CHANGELOG.md` (feature entry)

- [ ] **Step 1: Full Mac suite**

```bash
cmake --build build-mb --target test_media_browser_unit -j8 && ./build-mb/test_media_browser_unit
```
Expected: all tests pass (existing suites + `[list]`, `[browse_logic]`, `[mb_recs]`, mock case).

- [ ] **Step 2: Deploy + hardware checklist (spec §Phase 1 testing)**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box " && MEDIA_BROWSER=true ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Then on the TV, in order:
1. Walk all 7 chips with BTN1/BTN3 — no title overlap, every tab lands.
2. Shuffle Popular and Top Rated from the enclosure AND from the phone remote (BLACK short-press → SHUFFLE) — different movies each press; scroll after a shuffle keeps loading contiguous pages (up to ~100 titles).
3. Overlay peek after a shuffle (BTN4 open → close, no edits) — shuffled grid survives.
4. For You on the real library; tab away/back (cached); shuffle (new results).
5. TTL: exit to the main kiosk, re-enter MB within minutes — no refetch of the landing tab.
6. Stale-while-revalidate failure path: pull the Pi's network (or `sudo ip link set eth0 down`... use the Wi-Fi toggle if eth is the SSH path — pulling the TMDB egress is enough), force a revalidate by waiting out the TTL or temporarily lowering `kBrowseTtlHours` to 0 in a scratch build — the old grid must survive the failed refresh. Restore network after.
7. `journalctl -u magic-dingus-box-cpp --since "-10 min" | grep -i "for you\|shuffle\|revalidate"` — sample counts, merge sizes, and no stale-gen warnings.

- [ ] **Step 3: CHANGELOG + final commit**

Add under the Unreleased/next-version heading in `CHANGELOG.md` (match the file's existing entry style):

```markdown
- Media Browser: "For You" tab — personalized recommendations seeded from the
  Radarr library (TMDB recommendations, merged + ranked, shuffleable).
- Media Browser: SHUFFLE action in the filter overlay for Popular / Top Rated /
  For You; browse grids now refresh automatically after 6 hours
  (stale-while-revalidate — a failed refresh keeps the old grid).
```

```bash
git add CHANGELOG.md
git commit -m "docs: changelog for marquee personalization (For You, shuffle, TTL)"
```

---

## Self-review notes (already applied)

- **Spec coverage:** 1a′ → Task 1; 1a → Tasks 2+3 (TTL, SWR, timestamp rules); 1b → Tasks 2+3+4 (window, ranges, exclusion collapse, discover clamp + cache, empty-page fallback, commit-skip, per-tab rows); 1c → Tasks 2+5+6 (entry rule, seeds, join, merge, edge cases, header fit, footer hint); testing section → Tasks 1/2/5/7. Out-of-scope items (rotary long-press, post-filters, watch history) appear in no task.
- **Type consistency:** `TmdbList` (Task 1) is consumed by name in Tasks 3 and 6; `pick_shuffle_base(current, max, rand)` signature matches between Tasks 2 and 4; `SeedResult.ok` ↔ `TmdbList.ok`; `get_library_checked` optional shape matches Task 6's `lib_checked` usage; `FilterTabKind::ForYou` introduced in Task 4, first wired in Task 6.
- **Known simplification:** For You reuses `tmdb_current_gen_` for preemption rather than a separate counter — a chart load bumping the gen while a For You sample is in flight correctly discards the sample (the user left the tab; re-entry resamples).
