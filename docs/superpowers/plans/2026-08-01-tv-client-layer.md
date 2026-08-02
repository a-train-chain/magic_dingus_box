# TV Client Layer (Phase 2b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the TV data layer — TMDB TV endpoints on the existing `TmdbClient` plus a new `SonarrClient` mirroring `RadarrClient` — fully unit-tested, wired into `main.cpp`, with zero user-visible behavior change.

**Architecture:** `TmdbClient` grows TV list/detail/genre endpoints reusing its existing `TmdbList {ok,total_pages,hits}` shape and HTTP retry path; `TmdbSearchHit` gains a `kind` field defaulting to `Movie` so every existing movie caller is untouched. A new `src/media_browser/sonarr/` directory mirrors `src/media_browser/radarr/` file-for-file (types / parsers / client / mock), reusing the Servarr-identical `QualityProfile` and `RootFolder` structs rather than redefining them. Everything in this phase is renderer-free and therefore Mac-unit-testable; the kiosk binary only gains a constructed-but-unconsumed client.

**Tech Stack:** C++17, libcurl, jsoncpp, spdlog, Catch2 v3.5.2, CMake. Sonarr 4.0.19.2979-ls320 serving `/api/v3` with `X-Api-Key`. TMDB v3.

## Global Constraints

- **Repo root (quote every path — it contains spaces AND emoji):** `/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box `
- **Work tree for this phase:** `/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient` on branch `feat/tv-client`. Every path in this plan is relative to `<worktree>/magic_dingus_box_cpp/` unless written absolute.
- **ONE implementer per worktree.** Do not run two agents in this worktree concurrently.
- **DO NOT TOUCH these files — two other sessions are editing them on separate branches:** `magic_dingus_box_cpp/scripts/setup_services.sh` and everything under the **repo-root** `scripts/golden_image/` (note: NOT `magic_dingus_box_cpp/scripts/golden_image/` — that path does not exist; the golden-image scripts live one level up, beside `magic_dingus_box_cpp/`). Phase 2b needs no change to either (Sonarr provisioning already landed via the merged `feat/sonarr-stack` branch: `services/docker-compose.yml` has the `sonarr` service and `setup_services.sh` already writes `SONARR_API_KEY=` into `services/.env`).
- **Commit style:** `feat(mb): …` for implementation, `test(mb): …` for test-only commits. Every commit message ends with:
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  ```
- **Mac test loop.** `ctest -R MediaBrowserUnit` does work (`CMakeLists.txt` registers it via `add_test(NAME MediaBrowserUnit ...)`), but invoke the binary directly — Catch2's own output names the failing case and it accepts tag filters like `[sonarr]`:
  ```bash
  cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
    && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
  ```
  First time only, configure it:
  ```bash
  cmake -S "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp" \
        -B "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" \
        -DBUILD_KIOSK=OFF -DBUILD_TESTS=ON -DENABLE_MEDIA_BROWSER=ON
  ```
- **Green baseline that must not regress: 167 test cases / 5039 assertions.** Any task that ends with fewer than 167 cases passing has broken something.
- **Do not regress movies.** `TmdbSearchHit::kind` defaults to `MediaKind::Movie`; `parse_list_response`, `parse_search_response`, `parse_movie_detail`, `build_discover_url`, `get_popular/get_top_rated/get_now_playing/get_upcoming/get_similar/get_recommendations/discover/get_genres` keep their exact current signatures and behavior.
- **Test-first is mandatory.** Nearly every unit here is pure. Each parser and URL builder gets its failing test written and *observed failing* before the implementation exists.
- **No screens, no UI.** Phase 2b touches no file under `src/media_browser/ui/`, no `mb_chrome`, no overlay. The Movies/TV toggle, series detail screen, season UI, queue grouping UI and search-follows-mode are Phase 2c.
- **The client must NOT pre-group the queue.** Sonarr's `/api/v3/queue` is per-episode; a season pack is N records sharing one `downloadId`. The client returns the raw per-episode records *including* `download_id`; grouping is Phase 2c's UI job.
- **Sonarr applies `addOptions` ASYNCHRONOUSLY — this is a correctness requirement, not a note.** `POST /api/v3/series` returns the STORED resource (`SeriesController.AddSeries` → `Created(series.Id)` → `RestController.Created` serializes `GetResourceById(id)`), but at that instant the stored record still has every season `monitored:true` — Sonarr has only inserted the row and published `SeriesAddedEvent`, which queues a `RefreshSeriesCommand`. **`addOptions` itself is never observable on read, at any point.** A controller live-probe (2026-08-01: added Breaking Bad, polled `GET /series/{id}` 40×, plus the POST response) found `addOptions` absent from the POST response and from every GET — it is write-only on Sonarr's `SeriesResource`, not "populated, then nulled once the refresh lands" as originally assumed. Only once `RefreshSeriesService` has pulled the episode list from SkyHook does `EpisodeMonitoredService` apply the `monitor` enum — and the same probe showed `statistics.totalEpisodeCount` can populate *before* that enum is applied, so "episodes exist" alone is not a safe settle signal either. The predicate that cannot false-positive during that race is checking the **REQUESTED OUTCOME** directly: episodes exist AND the monitored-season set matches what was asked for — see `add_settled()` / `record_refreshed()` in `sonarr_client.cpp`. **An immediate single re-GET is therefore just as wrong as trusting the POST body** — it races the refresh and returns the same all-monitored view. `add_series` must BOUNDED-POLL until that outcome settles, and must report `settled=false` when it times out — and when it does, `series.seasons` is ALWAYS EMPTY (both unsettled paths clear it) rather than a pending snapshot, so the caller re-fetches instead of caching a wrong season list. **`settled == false` has two different causes 2c must not conflate:** on a just-added series it means "the poll timed out" (TRANSIENT — re-fetch shortly); on an *already-in-library* series (the idempotent add path) it means "Sonarr has never refreshed this record at all" (e.g. an announced/upcoming series with no episodes yet) — which is PERMANENT, and a UI that spins waiting for it to flip will spin forever. That poll sleeps, so it is **worker-thread only** — `cfg_.timeout_secs=5` and `WatchdogSec=10` mean it must never run on the render thread.
- **`minimumAvailability` does not exist in Sonarr** (Radarr-only). Never send it.
- **`addOptions.monitor` is derived from the caller's `monitor` flag**, never hardcoded: `monitor ? "firstSeason" : "none"`. Sonarr honours `addOptions.monitor` independently of `series.monitored`, so a "don't monitor" add that still sent `firstSeason` would leave a fully monitored season 1 underneath — and the moment anything flips `series.monitored` true, Sonarr grabs the whole season with no user action naming it.
- **Any probe whose answer drives a mutation needs a checked shape.** `nullopt`/empty must not conflate "the server said no" with "the request failed" — that conflation is the shipped Radarr bug `get_library_checked` exists to fix, and Sonarr shares Gluetun's netns so transport blips are routine.
- **TV's family-safe posture is WEAKER than the movie path — do not imply parity.** TMDB's `adult` flag is a **pornography-only** gate (TMDB staff: it does not mean "suggestive" or "mature"). `/discover/tv` has **no `certification.*` params at all** (movie-only), so there is no server-side rating pre-filter for TV and **TV-MA content will surface in the grids**. The spec's decision is to accept that: no certification gate, unrated shows allowed. Say this plainly wherever it comes up; never write copy suggesting TV matches the movie filter.
- **Parse `sizeleft` tolerantly** — accept both `sizeleft` (what serializes today) and `sizeLeft` (staged upstream rename, currently commented out).
- **Anchor every edit on quoted surrounding text, never on line numbers** — the files in this plan are actively edited by other work.
- **`pgrep -f <pattern>` self-matches over SSH** (the ssh command line contains the pattern). Use `pgrep -x <exact-name>` or poll a file instead.
- **Kiosk compile-verify only via an isolated Pi scratch build. NEVER `deploy_cpp.sh`** (it targets `magic@magicpi.local:/opt/magic_dingus_box`, the live install). Never restart `magic-dingus-box-cpp.service`.
- **Kiosk target compiles with `-Wall -Wextra -Wpedantic`**; the test target with `-Wall -Wextra`. No warnings in new code.
- **Live box facts (ground truth, proven in Phase 2a's acceptance test):** Sonarr at `http://localhost:8989`, API under `/api/v3`, `X-Api-Key` header. Root folder `/data/library/tv` ↔ host `/mnt/ssd/library/tv`. Quality profile named `"Any"` (id 1 on this box — **always resolve by name, never hardcode the id**). `GET /api/v3/series/lookup?term=tmdb:1396` → Breaking Bad with `tvdbId: 81189`. `DELETE /api/v3/queue/{id}?removeFromClient=true` removes the WHOLE download; sibling episode rows then 404.

---

## File Structure

**Created**

| File | Responsibility |
|---|---|
| `src/media_browser/sonarr/sonarr_types.h` | `Season`, `Episode`, `SeriesSearchHit`, `Series`, `SonarrQueueItem`. Includes `radarr_types.h` to reuse `QualityProfile` / `RootFolder` (identical Servarr shapes; redefining them in `namespace media_browser` is a redefinition error). |
| `src/media_browser/sonarr/sonarr_parsers.{h,cpp}` | Static stateless jsoncpp parsers over Sonarr payloads. |
| `src/media_browser/sonarr/sonarr_client.{h,cpp}` | curl-based HTTP client; all public methods `virtual` for mocking; `http_get/http_post/http_put/http_delete` `protected virtual` for stubbing. |
| `src/media_browser/sonarr/sonarr_mock.{h,cpp}` | `SonarrMockClient` — in-memory seeded subclass; the kiosk's no-API-key fallback and a test double. |
| `tests/media_browser/test_tmdb_tv.cpp` | TV row/detail/genre parsing + TV URL builders. |
| `tests/media_browser/test_sonarr_parsers.cpp` | Fixture-driven parser tests. |
| `tests/media_browser/test_sonarr_client.cpp` | Stub-driven client tests (paths, bodies, async-settle polling, path translation). |
| `tests/media_browser/fixtures/tmdb/tv_popular.json` | `/tv/popular` shape — rows WITHOUT an `adult` key, one WITH `adult:true`, one with `poster_path: null`. |
| `tests/media_browser/fixtures/tmdb/tv_detail.json` | `/tv/1396?append_to_response=credits` shape with `seasons[]` including Specials. |
| `tests/media_browser/fixtures/tmdb/tv_genres.json` | All 16 `/genre/tv/list` entries. |
| `tests/media_browser/fixtures/sonarr/series_lookup.json` | `/series/lookup?term=tmdb:1396` — `id:0`, all seasons monitored. |
| `tests/media_browser/fixtures/sonarr/series_added_pending.json` | `/series/7` **immediately** after the POST — `addOptions` still populated, every season still `monitored:true`, `statistics.totalEpisodeCount: 0`. The pre-refresh state the poll must reject. |
| `tests/media_browser/fixtures/sonarr/series_added.json` | `/series/7` once the refresh has settled — `addOptions` gone, S1 monitored, S0 + S2..S5 not. |
| `tests/media_browser/fixtures/sonarr/series_list.json` | `/series` — array containing the above object. |
| `tests/media_browser/fixtures/sonarr/queue.json` | 3 per-episode records of one season pack sharing one `downloadId`. |
| `tests/media_browser/fixtures/sonarr/history_series.json` | `/history/series` records with mixed-case + duplicate + empty `downloadId`. |
| `tests/media_browser/fixtures/sonarr/root_folders.json` | One root folder, `/data/library/tv`. |
| `tests/media_browser/fixtures/sonarr/quality_profiles.json` | The box's `"Any"` profile (id 1). |

**Modified**

| File | Change |
|---|---|
| `src/media_browser/tmdb_client.h` | `MediaKind` enum + `TmdbSearchHit::kind`; `TvDiscoverFilter`, `TmdbTvSeason`, `TmdbTvDetail`; TV endpoint + parser + URL-builder declarations; `extract_year` removed from the private section. |
| `src/media_browser/tmdb_client.cpp` | `extract_year` and a shared `fill_list_row` helper move to the anonymous namespace; `parse_list` rewritten on top of it; TV parsers + endpoints added. |
| `src/media_browser/radarr/radarr_parsers.h` | `normalize_tmdb_poster_url` promoted from the .cpp anonymous namespace to a public static so `SonarrParsers` can reuse it. **Task 3b only** — split out so a reviewer can reject the visibility change to shipped Radarr code without also rejecting the new Sonarr parsers. |
| `src/media_browser/radarr/radarr_parsers.cpp` | Same promotion; its **single** internal call site (inside `pick_image`, the only caller — the definition, that call, and a TEST_CASE *name* are the only three occurrences in the tree) updated. |
| `src/main.cpp` | `SonarrClient` construction inside the existing `#ifdef MEDIA_BROWSER_ENABLED` block, mirroring the Radarr key chain. |
| `src/media_browser/test_cli/main.cpp` | `sonarr-status` / `sonarr-lookup` / `sonarr-library` / `sonarr-queue` subcommands. |
| `CMakeLists.txt` | Sonarr sources into `MEDIA_BROWSER_SOURCES` (tests + CLI) and `KIOSK_MEDIA_BROWSER_SOURCES` (kiosk); test files into `MEDIA_BROWSER_TEST_SOURCES`. |

---

### Task 1: `TmdbSearchHit::kind` + TV row parsing

**Files:**
- Modify: `src/media_browser/tmdb_client.h` (add `MediaKind`, `kind`, `parse_tv_list`; remove the private `extract_year` declaration)
- Modify: `src/media_browser/tmdb_client.cpp` (move `extract_year` to the anonymous namespace, add `fill_list_row`, rewrite `parse_list`, add `parse_tv_list`)
- Create: `tests/media_browser/fixtures/tmdb/tv_popular.json`
- Create: `tests/media_browser/test_tmdb_tv.cpp`
- Modify: `CMakeLists.txt` (add `tests/media_browser/test_tmdb_tv.cpp` to `MEDIA_BROWSER_TEST_SOURCES`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `enum class media_browser::MediaKind { Movie, Tv };`
  - `media_browser::TmdbSearchHit::kind` (type `MediaKind`, default `MediaKind::Movie`)
  - `static TmdbList media_browser::TmdbClient::parse_tv_list(const std::string& json);`
  - Anonymous-namespace helpers in `tmdb_client.cpp`: `int extract_year(const std::string&)` and `bool fill_list_row(const Json::Value&, TmdbSearchHit&, const char* title_key, const char* original_key, const char* date_key, MediaKind)`.

---

- [ ] **Step 1: Create the TV list fixture**

Create `tests/media_browser/fixtures/tmdb/tv_popular.json`. Note deliberately: rows 1 and 2 carry **no `adult` key at all** (that is what `/tv/popular` really returns), row 3 carries `adult: true`, row 2 has `poster_path: null` and a float `vote_average`.

```json
{
  "page": 1,
  "results": [
    {
      "backdrop_path": "/84XPpjGvxNyExjSuLQe0SzioErt.jpg",
      "first_air_date": "2008-01-20",
      "genre_ids": [18, 80],
      "id": 1396,
      "name": "Breaking Bad",
      "origin_country": ["US"],
      "original_language": "en",
      "original_name": "Breaking Bad",
      "overview": "Walter White, a New Mexico chemistry teacher, is diagnosed with cancer.",
      "popularity": 289.1,
      "poster_path": "/ggFHVNu6YYI5L9pCfOacjizRGt.jpg",
      "vote_average": 8.9,
      "vote_count": 13000
    },
    {
      "backdrop_path": null,
      "first_air_date": "2011-04-17",
      "genre_ids": [10765, 18, 10759],
      "id": 1399,
      "name": "Game of Thrones",
      "origin_country": ["US"],
      "original_language": "en",
      "original_name": "Game of Thrones",
      "overview": "Seven noble families fight for control of the mythical land of Westeros.",
      "popularity": 201.4,
      "poster_path": null,
      "vote_average": 8.456,
      "vote_count": 22000
    },
    {
      "adult": true,
      "first_air_date": "2015-01-01",
      "genre_ids": [],
      "id": 999999,
      "name": "Should Be Dropped",
      "origin_country": ["US"],
      "original_language": "en",
      "original_name": "Should Be Dropped",
      "overview": "adult content",
      "popularity": 1.0,
      "poster_path": "/drop.jpg",
      "vote_average": 1.0,
      "vote_count": 3
    }
  ],
  "total_pages": 500,
  "total_results": 10000
}
```

- [ ] **Step 2: Write the failing tests**

Create `tests/media_browser/test_tmdb_tv.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "media_browser/tmdb_client.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
// Same convention as test_tmdb_client.cpp: __FILE__ resolves the fixture dir
// regardless of the runner's CWD.
std::string load_fixture(const std::string& name) {
    std::filesystem::path here = std::filesystem::path(__FILE__).parent_path();
    std::ifstream f(here / "fixtures" / "tmdb" / name);
    if (!f) return {};
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}
}  // namespace

namespace mb = media_browser;

TEST_CASE("parse_tv_list maps name/first_air_date and tags kind=Tv", "[tmdb][tv]") {
    const std::string json = load_fixture("tv_popular.json");
    REQUIRE_FALSE(json.empty());
    auto list = mb::TmdbClient::parse_tv_list(json);

    REQUIRE(list.ok);
    CHECK(list.total_pages == 500);
    // 3 rows in the fixture, 1 dropped for adult:true.
    REQUIRE(list.hits.size() == 2);

    CHECK(list.hits[0].tmdb_id == 1396);
    CHECK(list.hits[0].title == "Breaking Bad");           // from "name"
    CHECK(list.hits[0].original_title == "Breaking Bad");  // from "original_name"
    CHECK(list.hits[0].year == 2008);                      // from "first_air_date"
    CHECK(list.hits[0].rating == Catch::Approx(8.9));
    CHECK(list.hits[0].poster_path ==
          "https://image.tmdb.org/t/p/w500/ggFHVNu6YYI5L9pCfOacjizRGt.jpg");
    CHECK(list.hits[0].kind == mb::MediaKind::Tv);
    CHECK(list.hits[1].kind == mb::MediaKind::Tv);
}

TEST_CASE("parse_tv_list treats a missing adult key as false", "[tmdb][tv]") {
    // /tv/popular and /tv/top_rated rows carry NO adult field at all. If the
    // parser required the key it would drop every popular show on the box.
    const std::string json = R"({
        "page": 1, "total_pages": 1,
        "results": [
            {"id": 1396, "name": "Breaking Bad", "original_name": "Breaking Bad",
             "first_air_date": "2008-01-20", "vote_average": 8.9,
             "poster_path": "/bb.jpg", "overview": "x"}
        ]
    })";
    auto list = mb::TmdbClient::parse_tv_list(json);
    REQUIRE(list.ok);
    REQUIRE(list.hits.size() == 1);
    CHECK(list.hits[0].tmdb_id == 1396);
}

TEST_CASE("parse_tv_list drops adult rows when the flag IS present", "[tmdb][tv]") {
    // /search/tv, /tv/{id}/similar and /tv/{id}/recommendations DO carry adult.
    const std::string json = R"({
        "page": 1, "total_pages": 1,
        "results": [
            {"id": 1, "name": "Keep", "first_air_date": "2020-01-01", "adult": false},
            {"id": 2, "name": "Drop", "first_air_date": "2020-01-01", "adult": true}
        ]
    })";
    auto list = mb::TmdbClient::parse_tv_list(json);
    REQUIRE(list.ok);
    REQUIRE(list.hits.size() == 1);
    CHECK(list.hits[0].title == "Keep");
}

TEST_CASE("parse_tv_list tolerates null poster_path and float vote_average",
          "[tmdb][tv]") {
    const std::string json = load_fixture("tv_popular.json");
    auto list = mb::TmdbClient::parse_tv_list(json);
    REQUIRE(list.hits.size() == 2);
    CHECK(list.hits[1].tmdb_id == 1399);
    CHECK(list.hits[1].poster_path.empty());          // null → "no art"
    CHECK(list.hits[1].rating == Catch::Approx(8.456));  // NOT truncated to 8
}

TEST_CASE("parse_tv_list flags malformed and error payloads", "[tmdb][tv]") {
    CHECK_FALSE(mb::TmdbClient::parse_tv_list("not json {{{").ok);
    CHECK_FALSE(mb::TmdbClient::parse_tv_list(
        R"({"status_code": 34, "status_message": "not found"})").ok);
    auto empty = mb::TmdbClient::parse_tv_list(
        R"({"page": 1, "total_pages": 1, "results": []})");
    CHECK(empty.ok);
    CHECK(empty.hits.empty());
}

TEST_CASE("movie parse_list still defaults kind to Movie", "[tmdb][tv]") {
    // Regression guard for every existing movie caller: nothing in the movie
    // path sets kind, so the default must be Movie.
    const std::string json = R"({
        "page": 1, "total_pages": 3,
        "results": [{"id": 603, "title": "The Matrix", "release_date": "1999-03-30",
                     "vote_average": 8.2, "poster_path": "/m.jpg"}]
    })";
    auto list = mb::TmdbClient::parse_list(json);
    REQUIRE(list.hits.size() == 1);
    CHECK(list.hits[0].kind == mb::MediaKind::Movie);
    CHECK(list.hits[0].title == "The Matrix");
    CHECK(list.hits[0].year == 1999);
}
```

Add the file to the test target in `CMakeLists.txt` — anchor on the existing line and insert after it:

```cmake
        tests/media_browser/test_tmdb_client.cpp
        tests/media_browser/test_tmdb_tv.cpp
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8
```
Expected: **compile FAILS** with `no member named 'parse_tv_list' in 'media_browser::TmdbClient'` and `no member named 'kind' in 'media_browser::TmdbSearchHit'` and `no type named 'MediaKind'`.

- [ ] **Step 4: Add `MediaKind` + `kind` + `parse_tv_list` to the header**

In `src/media_browser/tmdb_client.h`, insert immediately above `struct TmdbSearchHit`:

```cpp
// Which TMDB namespace a row came from. Defaults to Movie on every hit so
// the entire pre-TV movie path (Browse, Search, For You, playback overlay)
// is untouched by the TV work — a TV row is only ever produced by the
// parse_tv_* family, which sets this explicitly.
enum class MediaKind { Movie, Tv };
```

Inside `struct TmdbSearchHit`, add the `kind` member. **Anchor on the full line INCLUDING its trailing comment** — the bare string `    double rating = 0.0;` occurs twice in this header (once in `TmdbSearchHit`, once in `TmdbMovieDetail`, where it has no comment), so a bare-substring edit is ambiguous and a `replace_all` would silently add `kind` to `TmdbMovieDetail` too. The unique anchor is:

```cpp
    double rating = 0.0;         // vote_average
```

which becomes:

```cpp
    double rating = 0.0;         // vote_average
    MediaKind kind = MediaKind::Movie;  // see MediaKind — movie rows never set this
```

In the "Pure parsers" block, after the `parse_list` declaration, add:

```cpp
    // TV-shaped variant of parse_list. TMDB's TV rows name their fields
    // differently (name / original_name / first_air_date instead of
    // title / original_title / release_date) and OMIT `adult` entirely on
    // /tv/popular and /tv/top_rated — so `adult` is read as
    // optional-default-false and only true rows are dropped. Every hit
    // comes back tagged kind == MediaKind::Tv.
    static TmdbList parse_tv_list(const std::string& json);
```

In the private section, **delete** this line (it moves to the .cpp anonymous namespace so the new file-local helper can call it):

```cpp
    static int extract_year(const std::string& date_yyyy_mm_dd);
```

- [ ] **Step 5: Move `extract_year` and add the shared row filler**

In `src/media_browser/tmdb_client.cpp`, delete the out-of-line definition:

```cpp
int TmdbClient::extract_year(const std::string& date) {
    if (date.size() < 4) return 0;
    try { return std::stoi(date.substr(0, 4)); }
    catch (...) { return 0; }
}
```

and add these two helpers inside the existing anonymous namespace, immediately after `resolve_poster_url`:

```cpp
// Year from an ISO "YYYY-MM-DD". 0 when absent or unparseable. Moved out of
// TmdbClient's private section so fill_list_row (below) can use it — it had
// no callers outside this translation unit.
static int extract_year(const std::string& date) {
    if (date.size() < 4) return 0;
    try { return std::stoi(date.substr(0, 4)); }
    catch (...) { return 0; }
}

// One row of any TMDB "results[]" payload. Movie and TV rows are the same
// shape except for three key names, so the parsers differ only in what they
// pass here. Returns false when the row must be dropped (family-safe gate):
// `adult` is read as optional-default-false because /tv/popular and
// /tv/top_rated omit the field entirely, and include_adult does not exist on
// those endpoints — the parser is the only gate there.
static bool fill_list_row(const Json::Value& r, TmdbSearchHit& h,
                          const char* title_key, const char* original_key,
                          const char* date_key, MediaKind kind) {
    if (r.get("adult", false).asBool()) return false;
    h.tmdb_id        = r.get("id", 0).asInt();
    h.title          = r.get(title_key, "").asString();
    h.original_title = r.get(original_key, "").asString();
    h.overview       = r.get("overview", "").asString();
    h.poster_path    = resolve_poster_url(r.get("poster_path", "").asString());
    h.year           = extract_year(r.get(date_key, "").asString());
    h.rating         = r.get("vote_average", 0.0).asDouble();
    h.kind           = kind;
    return true;
}
```

Now replace the body of the existing `TmdbClient::parse_list` row loop. The whole function becomes:

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
        TmdbSearchHit h;
        if (!fill_list_row(r, h, "title", "original_title", "release_date",
                           MediaKind::Movie)) {
            continue;  // family-safe drop
        }
        list.hits.push_back(std::move(h));
    }
    return list;
}
```

And add `parse_tv_list` directly beneath it:

```cpp
TmdbList TmdbClient::parse_tv_list(const std::string& json) {
    TmdbList list;
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(json);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        spdlog::error("[media_browser] TMDB TV list parse error: {}", err);
        return list;  // ok=false
    }
    const auto& results = root["results"];
    if (!results.isArray()) return list;  // TMDB error payload — ok=false
    list.ok = true;
    list.total_pages = root.get("total_pages", 0).asInt();
    for (const auto& r : results) {
        TmdbSearchHit h;
        if (!fill_list_row(r, h, "name", "original_name", "first_air_date",
                           MediaKind::Tv)) {
            continue;  // family-safe drop
        }
        list.hits.push_back(std::move(h));
    }
    return list;
}
```

Finally, fix the three remaining `extract_year(...)` call sites inside `parse_search_response`, `parse_list_response` and `parse_movie_detail` — they were `extract_year(...)` unqualified inside member functions and resolve to the new file-local function unchanged, so **no edit is needed**. Verify by compiling.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: `All tests passed (… assertions in 173 test cases)` — 167 baseline + 6 new. Zero failures.

- [ ] **Step 7: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/tmdb_client.h \
  magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp \
  magic_dingus_box_cpp/tests/media_browser/test_tmdb_tv.cpp \
  magic_dingus_box_cpp/tests/media_browser/fixtures/tmdb/tv_popular.json \
  magic_dingus_box_cpp/CMakeLists.txt
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): TmdbSearchHit::kind + TV row parser

TV rows use name/original_name/first_air_date and omit `adult` entirely on
/tv/popular and /tv/top_rated, so the parser reads it optional-default-false
and is the only family-safe gate there. kind defaults to Movie so every
existing movie caller is untouched.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: TMDB TV list endpoints + URL builders

**Files:**
- Modify: `src/media_browser/tmdb_client.h` (add `TvDiscoverFilter`, the four TV list endpoints, `discover_tv`, and two static URL builders)
- Modify: `src/media_browser/tmdb_client.cpp` (implement them)
- Modify: `tests/media_browser/test_tmdb_tv.cpp` (append URL-builder tests)

**Interfaces:**
- Consumes: `MediaKind`, `TmdbClient::parse_tv_list` (Task 1); the pre-existing `TmdbList { bool ok; int total_pages; std::vector<TmdbSearchHit> hits; }`.
- Produces:
  - `struct media_browser::TvDiscoverFilter` with members `std::vector<int> genre_ids; std::optional<int> first_air_date_year_gte; std::optional<int> first_air_date_year_lte; std::optional<float> vote_average_gte; std::optional<int> vote_count_gte; std::optional<int> with_runtime_gte; std::optional<int> with_runtime_lte; std::optional<std::string> with_original_language; std::string sort_by = "popularity.desc";`
  - `TmdbList TmdbClient::get_tv_popular(int page = 1);`
  - `TmdbList TmdbClient::get_tv_top_rated(int page = 1);`
  - `TmdbList TmdbClient::get_tv_recommendations(int tmdb_id, int page = 1);`
  - `TmdbList TmdbClient::get_tv_similar(int tmdb_id, int page = 1);`
  - `TmdbList TmdbClient::discover_tv(const TvDiscoverFilter& filter, int page = 1);`
  - `static std::string TmdbClient::build_tv_list_url(const std::string& api_key, const std::string& endpoint_path, int page);`
  - `static std::string TmdbClient::build_tv_discover_url(const std::string& api_key, const TvDiscoverFilter& filter, int page);`

---

- [ ] **Step 1: Write the failing URL-builder tests**

Append to `tests/media_browser/test_tmdb_tv.cpp`:

```cpp
// --- TV list URL builder -------------------------------------------------

TEST_CASE("build_tv_list_url builds a paged TV list URL", "[tmdb][tv][url]") {
    const std::string url = mb::TmdbClient::build_tv_list_url("KEY", "/tv/popular", 3);
    CHECK(url.find("https://api.themoviedb.org/3/tv/popular") == 0);
    CHECK(url.find("api_key=KEY") != std::string::npos);
    CHECK(url.find("language=en-US") != std::string::npos);
    CHECK(url.find("page=3") != std::string::npos);
    // include_adult does NOT exist on /tv/popular, /tv/top_rated,
    // /tv/{id}/similar or /tv/{id}/recommendations. Sending it would be a
    // lie about where the family-safe gate lives (it lives in parse_tv_list).
    CHECK(url.find("include_adult") == std::string::npos);
}

TEST_CASE("build_tv_list_url works for the per-series endpoints",
          "[tmdb][tv][url]") {
    const std::string rec =
        mb::TmdbClient::build_tv_list_url("KEY", "/tv/1396/recommendations", 1);
    CHECK(rec.find("/tv/1396/recommendations") != std::string::npos);
    const std::string sim =
        mb::TmdbClient::build_tv_list_url("KEY", "/tv/1396/similar", 2);
    CHECK(sim.find("/tv/1396/similar") != std::string::npos);
    CHECK(sim.find("page=2") != std::string::npos);
}

// --- TV discover URL builder ---------------------------------------------

TEST_CASE("build_tv_discover_url uses first_air_date, never air_date",
          "[tmdb][tv][url]") {
    mb::TvDiscoverFilter f;
    f.first_air_date_year_gte = 2015;
    f.first_air_date_year_lte = 2020;

    const std::string url = mb::TmdbClient::build_tv_discover_url("KEY", f, 1);
    CHECK(url.find("/discover/tv") != std::string::npos);
    CHECK(url.find("first_air_date.gte=2015-01-01") != std::string::npos);
    CHECK(url.find("first_air_date.lte=2020-12-31") != std::string::npos);
    // air_date.* matches ANY episode's air date, which is not what the
    // filter means. The '&' prefix is load-bearing: without it the needle
    // would match inside "first_air_date.gte".
    CHECK(url.find("&air_date.gte=") == std::string::npos);
    CHECK(url.find("&air_date.lte=") == std::string::npos);
}

TEST_CASE("build_tv_discover_url joins multi-genre with pipe (OR semantics)",
          "[tmdb][tv][url]") {
    mb::TvDiscoverFilter f;
    // TV genre id space: 10759 Action & Adventure, 10765 Sci-Fi & Fantasy,
    // 18 Drama. Movie ids (28, 878) are INVALID here.
    f.genre_ids = {10759, 10765, 18};
    const std::string url = mb::TmdbClient::build_tv_discover_url("KEY", f, 1);
    CHECK(url.find("with_genres=10759%7C10765%7C18") != std::string::npos);
}

TEST_CASE("build_tv_discover_url emits include_adult=false and the rest of the "
          "filter", "[tmdb][tv][url]") {
    mb::TvDiscoverFilter f;
    f.vote_average_gte = 7.5f;
    f.vote_count_gte = 200;
    f.with_runtime_gte = 20;
    f.with_runtime_lte = 70;
    f.with_original_language = "en";
    f.sort_by = "vote_average.desc";
    const std::string url = mb::TmdbClient::build_tv_discover_url("KEY", f, 4);
    // include_adult DOES exist on /discover/tv (unlike the list endpoints).
    CHECK(url.find("include_adult=false") != std::string::npos);
    CHECK(url.find("vote_average.gte=7.5") != std::string::npos);
    CHECK(url.find("vote_count.gte=200") != std::string::npos);
    CHECK(url.find("with_runtime.gte=20") != std::string::npos);
    CHECK(url.find("with_runtime.lte=70") != std::string::npos);
    CHECK(url.find("with_original_language=en") != std::string::npos);
    CHECK(url.find("sort_by=vote_average.desc") != std::string::npos);
    CHECK(url.find("page=4") != std::string::npos);
    // /discover/tv has NO certification params (movie-only) — never emit them.
    CHECK(url.find("certification") == std::string::npos);
}

TEST_CASE("build_tv_discover_url omits unset filters", "[tmdb][tv][url]") {
    mb::TvDiscoverFilter f;  // all defaults
    const std::string url = mb::TmdbClient::build_tv_discover_url("KEY", f, 1);
    CHECK(url.find("with_genres") == std::string::npos);
    CHECK(url.find("first_air_date.gte") == std::string::npos);
    CHECK(url.find("first_air_date.lte") == std::string::npos);
    CHECK(url.find("vote_average.gte") == std::string::npos);
    CHECK(url.find("with_runtime") == std::string::npos);
    CHECK(url.find("with_original_language") == std::string::npos);
    CHECK(url.find("sort_by=popularity.desc") != std::string::npos);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8
```
Expected: **compile FAILS** with `no member named 'build_tv_list_url'`, `no member named 'build_tv_discover_url'`, `no type named 'TvDiscoverFilter'`.

- [ ] **Step 3: Declare the filter, endpoints and builders**

In `src/media_browser/tmdb_client.h`, insert immediately after the closing brace of `struct DiscoverFilter`:

```cpp
// Inline filter used for /discover/tv queries. Deliberately a separate type
// from DiscoverFilter, not a shared one: the date params differ
// (first_air_date.* vs primary_release_date.*) and — the sharp edge — the
// genre id spaces are DIFFERENT. TV has 16 genres; 10759 "Action & Adventure"
// and 10765 "Sci-Fi & Fantasy" replace the movie ids 28/12 and 878/14, and
// 11 movie ids (28, 12, 14, 27, 36, 53, 878, 10402, 10749, 10752, 10770) are
// invalid for TV. A shared struct would invite a caller to carry movie ids
// into a TV query and silently get an empty grid.
struct TvDiscoverFilter {
    std::vector<int> genre_ids;                     // TV ids only (see /genre/tv/list); URL-emitted as with_genres=18%7C80 → OR
    std::optional<int> first_air_date_year_gte;     // formatted "YYYY-01-01" in URL
    std::optional<int> first_air_date_year_lte;     // formatted "YYYY-12-31" in URL
    std::optional<float> vote_average_gte;
    std::optional<int> vote_count_gte;
    std::optional<int> with_runtime_gte;            // per-episode minutes
    std::optional<int> with_runtime_lte;
    std::optional<std::string> with_original_language;  // ISO 639-1
    std::string sort_by = "popularity.desc";        // popularity|vote_average|vote_count|first_air_date|name .asc/.desc
};
```

In the public method block, after `TmdbList get_recommendations(int tmdb_id, int page = 1);`, add:

```cpp
    // --- TV -------------------------------------------------------------
    // Same TmdbList shape as the movie endpoints; hits come back tagged
    // kind == MediaKind::Tv. None of these four accepts include_adult (it
    // exists only on /search/tv and /discover/tv), so parse_tv_list is the
    // family-safe gate — see its comment.
    TmdbList get_tv_popular(int page = 1);
    TmdbList get_tv_top_rated(int page = 1);
    TmdbList get_tv_recommendations(int tmdb_id, int page = 1);
    TmdbList get_tv_similar(int tmdb_id, int page = 1);

    // /discover/tv. Note there are NO certification params for TV (movie-only),
    // so a rating-based pre-filter is impossible server-side — the spec's
    // decision is no TV certification gate at all.
    TmdbList discover_tv(const TvDiscoverFilter& filter, int page = 1);
```

In the "URL builders" block, after `build_discover_url`, add:

```cpp
    // Shared builder for the four paged TV list endpoints. `endpoint_path`
    // is the API-relative path with a leading slash, e.g. "/tv/popular" or
    // "/tv/1396/recommendations".
    static std::string build_tv_list_url(const std::string& api_key,
                                         const std::string& endpoint_path,
                                         int page);

    static std::string build_tv_discover_url(const std::string& api_key,
                                             const TvDiscoverFilter& filter,
                                             int page);
```

- [ ] **Step 4: Implement the builders and endpoints**

In `src/media_browser/tmdb_client.cpp`, add after `TmdbClient::get_recommendations`:

```cpp
std::string TmdbClient::build_tv_list_url(const std::string& api_key,
                                          const std::string& endpoint_path,
                                          int page) {
    std::ostringstream url;
    url << kApiBase << endpoint_path
        << "?api_key=" << url_encode(api_key)
        << "&language=en-US"
        << "&page=" << page;
    // No include_adult: /tv/popular, /tv/top_rated, /tv/{id}/similar and
    // /tv/{id}/recommendations do not accept it. parse_tv_list drops
    // adult==true rows client-side instead.
    return url.str();
}

TmdbList TmdbClient::get_tv_popular(int page) {
    auto body = http_get(build_tv_list_url(api_key_, "/tv/popular", page));
    if (body.empty()) return {};  // ok=false — network/HTTP failure
    return parse_tv_list(body);
}

TmdbList TmdbClient::get_tv_top_rated(int page) {
    auto body = http_get(build_tv_list_url(api_key_, "/tv/top_rated", page));
    if (body.empty()) return {};  // ok=false — network/HTTP failure
    return parse_tv_list(body);
}

TmdbList TmdbClient::get_tv_recommendations(int tmdb_id, int page) {
    // Algorithmic mix; callers fall back to get_tv_similar when hits is empty
    // — the same documented contract the movie For You path already uses.
    const std::string path =
        "/tv/" + std::to_string(tmdb_id) + "/recommendations";
    auto body = http_get(build_tv_list_url(api_key_, path, page));
    if (body.empty()) return {};  // ok=false — network/HTTP failure
    return parse_tv_list(body);
}

TmdbList TmdbClient::get_tv_similar(int tmdb_id, int page) {
    const std::string path = "/tv/" + std::to_string(tmdb_id) + "/similar";
    auto body = http_get(build_tv_list_url(api_key_, path, page));
    if (body.empty()) return {};  // ok=false — network/HTTP failure
    return parse_tv_list(body);
}

std::string TmdbClient::build_tv_discover_url(const std::string& api_key,
                                              const TvDiscoverFilter& filter,
                                              int page) {
    std::ostringstream url;
    url << kApiBase << "/discover/tv"
        << "?api_key=" << url_encode(api_key)
        << "&include_adult=false"   // exists on /discover/tv (unlike the list endpoints)
        << "&language=en-US"
        << "&page=" << page;
    if (!filter.sort_by.empty()) {
        url << "&sort_by=" << url_encode(filter.sort_by);
    }
    if (!filter.genre_ids.empty()) {
        url << "&with_genres=";
        for (size_t i = 0; i < filter.genre_ids.size(); ++i) {
            if (i > 0) url << "%7C";  // URL-encoded pipe → OR (comma would mean AND)
            url << filter.genre_ids[i];
        }
    }
    // first_air_date.* = the SERIES premiere. air_date.* (which also exists)
    // matches any episode's air date and would let a 1960s show through a
    // "2020s" filter — never use it here.
    if (filter.first_air_date_year_gte) {
        url << "&first_air_date.gte=" << *filter.first_air_date_year_gte << "-01-01";
    }
    if (filter.first_air_date_year_lte) {
        url << "&first_air_date.lte=" << *filter.first_air_date_year_lte << "-12-31";
    }
    if (filter.vote_average_gte) {
        url << "&vote_average.gte=" << *filter.vote_average_gte;
    }
    if (filter.vote_count_gte) {
        url << "&vote_count.gte=" << *filter.vote_count_gte;
    }
    if (filter.with_runtime_gte) {
        url << "&with_runtime.gte=" << *filter.with_runtime_gte;
    }
    if (filter.with_runtime_lte) {
        url << "&with_runtime.lte=" << *filter.with_runtime_lte;
    }
    if (filter.with_original_language) {
        url << "&with_original_language=" << url_encode(*filter.with_original_language);
    }
    return url.str();
}

TmdbList TmdbClient::discover_tv(const TvDiscoverFilter& filter, int page) {
    auto body = http_get(build_tv_discover_url(api_key_, filter, page));
    if (body.empty()) return {};  // ok=false — network/HTTP failure
    return parse_tv_list(body);
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: `All tests passed (… assertions in 179 test cases)` — 173 + 6 new.

- [ ] **Step 6: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/tmdb_client.h \
  magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp \
  magic_dingus_box_cpp/tests/media_browser/test_tmdb_tv.cpp
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): TMDB TV list endpoints + discover_tv

get_tv_popular/top_rated/recommendations/similar share one URL builder and
send no include_adult (the param does not exist on those endpoints).
discover_tv uses first_air_date.* — air_date.* matches any episode's date —
and carries its own filter type so movie genre ids can never leak into a TV
query.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: TMDB TV detail (with `seasons[]`) + TV genres

**Files:**
- Create: `tests/media_browser/fixtures/tmdb/tv_detail.json`
- Create: `tests/media_browser/fixtures/tmdb/tv_genres.json`
- Modify: `src/media_browser/tmdb_client.h` (`TmdbTvSeason`, `TmdbTvDetail`, `get_tv_detail`, `parse_tv_detail`, `get_tv_genres`)
- Modify: `src/media_browser/tmdb_client.cpp`
- Modify: `tests/media_browser/test_tmdb_tv.cpp`

**Interfaces:**
- Consumes: `MediaKind` (Task 1); the pre-existing `struct Genre { int id; std::string name; }` and `static std::vector<Genre> parse_genres_response(const std::string&)`.
- Produces:
  - `struct media_browser::TmdbTvSeason { int season_number; std::string name; std::string overview; std::string air_date; int episode_count; std::string poster_path; };`
  - `struct media_browser::TmdbTvDetail` (full member list in Step 3)
  - `std::optional<TmdbTvDetail> TmdbClient::get_tv_detail(int tmdb_id);`
  - `static std::optional<TmdbTvDetail> TmdbClient::parse_tv_detail(const std::string& json);`
  - `std::vector<Genre> TmdbClient::get_tv_genres();`

---

- [ ] **Step 1: Create the two fixtures**

`tests/media_browser/fixtures/tmdb/tv_detail.json`:

```json
{
  "adult": false,
  "backdrop_path": "/tsRy63Mu5cu8etL1X7ZLyf7UP1M.jpg",
  "created_by": [
    {"id": 66633, "credit_id": "52542286760ee31328001a7b", "name": "Vince Gilligan"}
  ],
  "episode_run_time": [45],
  "first_air_date": "2008-01-20",
  "genres": [
    {"id": 18, "name": "Drama"},
    {"id": 80, "name": "Crime"}
  ],
  "id": 1396,
  "in_production": false,
  "last_air_date": "2013-09-29",
  "name": "Breaking Bad",
  "number_of_episodes": 62,
  "number_of_seasons": 5,
  "original_language": "en",
  "original_name": "Breaking Bad",
  "overview": "Walter White, a New Mexico chemistry teacher, is diagnosed with cancer.",
  "popularity": 289.1,
  "poster_path": "/ggFHVNu6YYI5L9pCfOacjizRGt.jpg",
  "seasons": [
    {"air_date": "2009-02-17", "episode_count": 5, "id": 3577, "name": "Specials",
     "overview": "", "poster_path": "/40dT79mDEZwXkQiZNBgSaydQFDP.jpg",
     "season_number": 0, "vote_average": 0},
    {"air_date": "2008-01-20", "episode_count": 7, "id": 3572, "name": "Season 1",
     "overview": "High school chemistry teacher Walter White.",
     "poster_path": "/1BP4xYv9ZG4ZVHkL7ocOziBbSYH.jpg",
     "season_number": 1, "vote_average": 8.2},
    {"air_date": "2009-03-08", "episode_count": 13, "id": 3573, "name": "Season 2",
     "overview": "", "poster_path": null,
     "season_number": 2, "vote_average": 8.3}
  ],
  "status": "Ended",
  "tagline": "Change the equation.",
  "type": "Scripted",
  "vote_average": 8.906,
  "vote_count": 13500,
  "credits": {
    "cast": [
      {"order": 0, "name": "Bryan Cranston", "character": "Walter White"},
      {"order": 1, "name": "Aaron Paul", "character": "Jesse Pinkman"},
      {"order": 2, "name": "Anna Gunn", "character": "Skyler White"},
      {"order": 3, "name": "Dean Norris", "character": "Hank Schrader"},
      {"order": 4, "name": "Betsy Brandt", "character": "Marie Schrader"},
      {"order": 5, "name": "RJ Mitte", "character": "Walter White Jr."},
      {"order": 6, "name": "Bob Odenkirk", "character": "Saul Goodman"}
    ],
    "crew": []
  }
}
```

`tests/media_browser/fixtures/tmdb/tv_genres.json` — all 16 TV genres:

```json
{
  "genres": [
    {"id": 10759, "name": "Action & Adventure"},
    {"id": 16, "name": "Animation"},
    {"id": 35, "name": "Comedy"},
    {"id": 80, "name": "Crime"},
    {"id": 99, "name": "Documentary"},
    {"id": 18, "name": "Drama"},
    {"id": 10751, "name": "Family"},
    {"id": 10762, "name": "Kids"},
    {"id": 9648, "name": "Mystery"},
    {"id": 10763, "name": "News"},
    {"id": 10764, "name": "Reality"},
    {"id": 10765, "name": "Sci-Fi & Fantasy"},
    {"id": 10766, "name": "Soap"},
    {"id": 10767, "name": "Talk"},
    {"id": 10768, "name": "War & Politics"},
    {"id": 37, "name": "Western"}
  ]
}
```

- [ ] **Step 2: Write the failing tests**

Append to `tests/media_browser/test_tmdb_tv.cpp`:

```cpp
// --- TV detail -----------------------------------------------------------

TEST_CASE("parse_tv_detail extracts the series record", "[tmdb][tv][detail]") {
    const std::string json = load_fixture("tv_detail.json");
    REQUIRE_FALSE(json.empty());
    auto d = mb::TmdbClient::parse_tv_detail(json);
    REQUIRE(d.has_value());

    CHECK(d->tmdb_id == 1396);
    CHECK(d->title == "Breaking Bad");            // from "name"
    CHECK(d->original_title == "Breaking Bad");   // from "original_name"
    CHECK(d->tagline == "Change the equation.");
    CHECK(d->first_air_date == "2008-01-20");
    CHECK(d->last_air_date == "2013-09-29");
    CHECK(d->year == 2008);
    CHECK(d->rating == Catch::Approx(8.906));
    CHECK(d->vote_count == 13500);
    CHECK(d->original_language == "en");
    CHECK(d->status == "Ended");
    CHECK_FALSE(d->in_production);
    CHECK(d->number_of_seasons == 5);
    CHECK(d->number_of_episodes == 62);
    CHECK(d->poster_path ==
          "https://image.tmdb.org/t/p/w500/ggFHVNu6YYI5L9pCfOacjizRGt.jpg");
    CHECK(d->backdrop_path ==
          "https://image.tmdb.org/t/p/w500/tsRy63Mu5cu8etL1X7ZLyf7UP1M.jpg");

    REQUIRE(d->genres.size() == 2);
    CHECK(d->genres[0] == "Drama");
    CHECK(d->genres[1] == "Crime");

    // Cast capped at 6 like the movie detail parser.
    REQUIRE(d->cast_top.size() == 6);
    CHECK(d->cast_top[0] == "Bryan Cranston");
    CHECK(d->cast_top[5] == "RJ Mitte");

    // TV's analog of "directors" is created_by.
    REQUIRE(d->creators.size() == 1);
    CHECK(d->creators[0] == "Vince Gilligan");
}

TEST_CASE("parse_tv_detail keeps Specials (season 0) in seasons[]",
          "[tmdb][tv][detail]") {
    // Sonarr's addOptions.monitor=firstSeason leaves specials UNmonitored, so
    // the UI needs to know season 0 exists to render it correctly. The parser
    // preserves TMDB's order and does not filter it out.
    auto d = mb::TmdbClient::parse_tv_detail(load_fixture("tv_detail.json"));
    REQUIRE(d.has_value());
    REQUIRE(d->seasons.size() == 3);
    CHECK(d->seasons[0].season_number == 0);
    CHECK(d->seasons[0].name == "Specials");
    CHECK(d->seasons[0].episode_count == 5);
    CHECK(d->seasons[1].season_number == 1);
    CHECK(d->seasons[1].episode_count == 7);
    CHECK(d->seasons[1].air_date == "2008-01-20");
    CHECK(d->seasons[1].poster_path ==
          "https://image.tmdb.org/t/p/w500/1BP4xYv9ZG4ZVHkL7ocOziBbSYH.jpg");
    CHECK(d->seasons[2].season_number == 2);
    CHECK(d->seasons[2].poster_path.empty());  // null poster_path
}

TEST_CASE("parse_tv_detail drops adult entries", "[tmdb][tv][detail]") {
    // /tv/{id} DOES document `adult`. Same defence-in-depth as
    // parse_movie_detail: a caller with a raw id must not land on a porn entry.
    const std::string json =
        R"({"id": 5, "name": "XXX", "first_air_date": "2020-01-01", "adult": true})";
    CHECK_FALSE(mb::TmdbClient::parse_tv_detail(json).has_value());
}

TEST_CASE("parse_tv_detail handles a minimal payload and bad JSON",
          "[tmdb][tv][detail]") {
    auto d = mb::TmdbClient::parse_tv_detail(
        R"({"id": 1, "name": "Minimal", "first_air_date": "2020-01-01"})");
    REQUIRE(d.has_value());
    CHECK(d->tagline.empty());
    CHECK(d->genres.empty());
    CHECK(d->cast_top.empty());
    CHECK(d->creators.empty());
    CHECK(d->seasons.empty());
    CHECK(d->number_of_seasons == 0);

    CHECK_FALSE(mb::TmdbClient::parse_tv_detail("not json {{{").has_value());
    // No "id" member → not a series payload.
    CHECK_FALSE(mb::TmdbClient::parse_tv_detail(R"({"success": false})").has_value());
}

// --- TV detail / genre URL builders --------------------------------------

TEST_CASE("build_tv_genres_url hits /genre/tv/list, never /genre/movie/list",
          "[tmdb][tv][url]") {
    // The entire reason get_tv_genres() exists as a separate call. A
    // one-character typo here ships a movie genre table into TV mode, where
    // ids 28/878 do not exist and 10759/10765 are missing — silently, because
    // both URLs return a well-formed {genres:[...]} the parser accepts.
    const std::string url = mb::TmdbClient::build_tv_genres_url("KEY");
    CHECK(url.find("/genre/tv/list") != std::string::npos);
    CHECK(url.find("/genre/movie/list") == std::string::npos);
    CHECK(url.find("api_key=KEY") != std::string::npos);
}

TEST_CASE("build_tv_detail_url appends credits and not content_ratings",
          "[tmdb][tv][url]") {
    const std::string url = mb::TmdbClient::build_tv_detail_url("KEY", 1396);
    CHECK(url.find("/tv/1396") != std::string::npos);
    CHECK(url.find("append_to_response=credits") != std::string::npos);
    CHECK(url.find("api_key=KEY") != std::string::npos);
    CHECK(url.find("language=en-US") != std::string::npos);
    // No certification gate is applied for TV (spec decision), so paying for
    // content_ratings would buy nothing.
    CHECK(url.find("content_ratings") == std::string::npos);
}

// --- TV genres -----------------------------------------------------------

TEST_CASE("TV genre list is a different id space from movies",
          "[tmdb][tv][genres]") {
    const std::string json = load_fixture("tv_genres.json");
    REQUIRE_FALSE(json.empty());
    // The genre payload shape is identical for movies and TV, so the existing
    // parser is reused; what must NOT be shared is the resulting table.
    auto genres = mb::TmdbClient::parse_genres_response(json);
    REQUIRE(genres.size() == 16);

    auto has_id = [&genres](int id) {
        for (const auto& g : genres) if (g.id == id) return true;
        return false;
    };
    auto name_of = [&genres](int id) -> std::string {
        for (const auto& g : genres) if (g.id == id) return g.name;
        return {};
    };

    // TV-only ids.
    CHECK(has_id(10759));
    CHECK(name_of(10759) == "Action & Adventure");
    CHECK(has_id(10765));
    CHECK(name_of(10765) == "Sci-Fi & Fantasy");
    CHECK(has_id(10768));  // War & Politics

    // Movie-only ids must be ABSENT — a shared table keyed by movie ids would
    // silently mislabel or drop TV rows.
    CHECK_FALSE(has_id(28));    // Action (movie)
    CHECK_FALSE(has_id(878));   // Science Fiction (movie)
    CHECK_FALSE(has_id(10749)); // Romance (movie)
    CHECK_FALSE(has_id(10770)); // TV Movie (movie)

    // Overlapping ids keep their names.
    CHECK(name_of(18) == "Drama");
    CHECK(name_of(16) == "Animation");
}
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8
```
Expected: **compile FAILS** with `no member named 'parse_tv_detail' in 'media_browser::TmdbClient'`, plus `no member named 'build_tv_detail_url'` and `no member named 'build_tv_genres_url'`. Those last two are the drivers for this task's real hazard — the `/genre/tv/list` vs `/genre/movie/list` typo. (The `TV genre list is a different id space` case is a fixture guard rather than a driver: it exercises the pre-existing `parse_genres_response` and will pass as soon as the fixture exists. That is why the two builder tests above it are the ones that must go red here.)

- [ ] **Step 4: Declare the TV detail types and methods**

In `src/media_browser/tmdb_client.h`, insert after the closing brace of `struct TmdbMovieDetail`:

```cpp
// One row of /tv/{id}'s seasons[]. Includes season 0 ("Specials") when TMDB
// returns it — the caller decides whether to show it. Sonarr's
// addOptions.monitor="firstSeason" leaves specials unmonitored, so the UI
// needs to know the season exists to render its state honestly.
struct TmdbTvSeason {
    int season_number = 0;
    std::string name;          // "Season 1" / "Specials"
    std::string overview;
    std::string air_date;      // ISO yyyy-mm-dd; frequently empty
    int episode_count = 0;
    std::string poster_path;   // full w500 URL; empty when TMDB has none
};

// /tv/{id}?append_to_response=credits. Mirrors TmdbMovieDetail's conventions
// (full w500 image URLs, display-name genre strings, cast capped at 6) so the
// series detail screen can reuse the movie detail layout.
//
// Deliberately carries NO runtime field: the disk estimate uses Sonarr's
// series.runtime, and TMDB's episode_run_time is an array that is frequently
// empty on modern entries.
struct TmdbTvDetail {
    int tmdb_id = 0;
    std::string title;           // TMDB "name"
    std::string original_title;  // TMDB "original_name"
    std::string overview;
    std::string tagline;
    std::string poster_path;     // full w500 URL
    std::string backdrop_path;   // full w500 URL
    int year = 0;                // from first_air_date
    double rating = 0.0;         // vote_average
    int vote_count = 0;
    std::string first_air_date;  // ISO yyyy-mm-dd
    std::string last_air_date;   // ISO yyyy-mm-dd; empty while airing
    std::string original_language;
    std::string status;          // "Ended" / "Returning Series" / "Canceled" / ...
    bool in_production = false;
    int number_of_seasons = 0;
    int number_of_episodes = 0;
    std::vector<std::string> genres;    // display names, in TMDB order
    std::vector<std::string> cast_top;  // up to 6 from credits.cast
    std::vector<std::string> creators;  // created_by[].name — TV's "directors"
    std::vector<TmdbTvSeason> seasons;  // TMDB order; includes season 0
};
```

In the public method block, after `TmdbList discover_tv(...)`, add:

```cpp
    // Series detail (by id), with seasons[] and credits in one round-trip.
    std::optional<TmdbTvDetail> get_tv_detail(int tmdb_id);

    // TV genre list. A SEPARATE call from get_genres() on purpose — the id
    // spaces differ (see TvDiscoverFilter). Cache client-side; changes rarely.
    std::vector<Genre> get_tv_genres();
```

In the "Pure parsers" block, after `parse_tv_list`, add:

```cpp
    static std::optional<TmdbTvDetail> parse_tv_detail(const std::string& json);
```

In the "URL builders" block, after `build_tv_discover_url`, add — same reason Task 2 extracted its builders: `TmdbClient::http_get` is private and non-virtual, so a URL built inline inside a member function is unreachable by any stub and ships untested.

```cpp
    static std::string build_tv_detail_url(const std::string& api_key, int tmdb_id);
    static std::string build_tv_genres_url(const std::string& api_key);
```

- [ ] **Step 5: Implement the parser and the two endpoints**

In `src/media_browser/tmdb_client.cpp`, add after `TmdbClient::discover_tv`:

```cpp
std::string TmdbClient::build_tv_detail_url(const std::string& api_key,
                                            int tmdb_id) {
    // append_to_response=credits gets cast in the same HTTP round-trip, the
    // same way get_movie does.
    //
    // Deliberately NOT appending content_ratings. The kiosk applies no TV
    // certification gate at all (spec decision), so the US rating would be
    // fetched and discarded. Be clear about what that means: parse_tv_detail's
    // `adult` check is a PORNOGRAPHY-only filter — TMDB staff are explicit
    // that the flag does not mean "mature" or "suggestive" — and /discover/tv
    // has no certification.* params, so there is no server-side rating filter
    // for TV either. TV-MA series will appear in the grids. This is a weaker
    // posture than the movie path, accepted knowingly; it is not parity.
    std::ostringstream url;
    url << kApiBase << "/tv/" << tmdb_id
        << "?api_key=" << url_encode(api_key)
        << "&language=en-US"
        << "&append_to_response=credits";
    return url.str();
}

std::string TmdbClient::build_tv_genres_url(const std::string& api_key) {
    return std::string(kApiBase) + "/genre/tv/list?api_key=" + url_encode(api_key);
}

std::optional<TmdbTvDetail> TmdbClient::get_tv_detail(int tmdb_id) {
    auto body = http_get(build_tv_detail_url(api_key_, tmdb_id));
    if (body.empty()) return std::nullopt;
    return parse_tv_detail(body);
}

std::vector<Genre> TmdbClient::get_tv_genres() {
    auto body = http_get(build_tv_genres_url(api_key_));
    if (body.empty()) return {};
    // Response shape is {genres:[{id,name}]} — identical to /genre/movie/list,
    // so the parser is shared. The RESULT must not be: TV and movie genre ids
    // are different spaces (see TvDiscoverFilter).
    return parse_genres_response(body);
}

std::optional<TmdbTvDetail> TmdbClient::parse_tv_detail(const std::string& json) {
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(json);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        spdlog::error("[media_browser] TMDB TV detail parse error: {}", err);
        return std::nullopt;
    }
    if (!root.isObject() || !root.isMember("id")) return std::nullopt;

    // Family-safe gate, mirroring parse_movie_detail: /tv/{id} does document
    // `adult`, so a caller reaching Detail with a raw id can't land on porn.
    if (root.get("adult", false).asBool()) {
        spdlog::warn("[media_browser] TMDB TV detail: dropping adult entry id={}",
                     root.get("id", 0).asInt());
        return std::nullopt;
    }

    TmdbTvDetail d;
    d.tmdb_id            = root.get("id", 0).asInt();
    d.title              = root.get("name", "").asString();
    d.original_title     = root.get("original_name", "").asString();
    d.overview           = root.get("overview", "").asString();
    d.tagline            = root.get("tagline", "").asString();
    d.poster_path        = resolve_poster_url(root.get("poster_path", "").asString());
    d.backdrop_path      = resolve_poster_url(root.get("backdrop_path", "").asString());
    d.rating             = root.get("vote_average", 0.0).asDouble();
    d.vote_count         = root.get("vote_count", 0).asInt();
    d.first_air_date     = root.get("first_air_date", "").asString();
    d.last_air_date      = root.get("last_air_date", "").asString();
    d.year               = extract_year(d.first_air_date);
    d.original_language  = root.get("original_language", "").asString();
    d.status             = root.get("status", "").asString();
    d.in_production      = root.get("in_production", false).asBool();
    d.number_of_seasons  = root.get("number_of_seasons", 0).asInt();
    d.number_of_episodes = root.get("number_of_episodes", 0).asInt();

    const auto& genres = root["genres"];
    if (genres.isArray()) {
        for (const auto& g : genres) {
            std::string name = g.get("name", "").asString();
            if (!name.empty()) d.genres.push_back(std::move(name));
        }
    }

    const auto& created_by = root["created_by"];
    if (created_by.isArray()) {
        for (const auto& c : created_by) {
            std::string name = c.get("name", "").asString();
            if (!name.empty()) d.creators.push_back(std::move(name));
        }
    }

    const auto& seasons = root["seasons"];
    if (seasons.isArray()) {
        for (const auto& s : seasons) {
            TmdbTvSeason ts;
            ts.season_number = s.get("season_number", 0).asInt();
            ts.name          = s.get("name", "").asString();
            ts.overview      = s.get("overview", "").asString();
            ts.air_date      = s.get("air_date", "").asString();
            ts.episode_count = s.get("episode_count", 0).asInt();
            ts.poster_path   = resolve_poster_url(s.get("poster_path", "").asString());
            d.seasons.push_back(std::move(ts));
        }
    }

    // credits.cast is pre-sorted by "order" (lowest = top-billed). Cap at 6,
    // matching parse_movie_detail so the two detail layouts stay identical.
    //
    // NOTE: /tv/{id}/credits (and this appended form) is LATEST-SEASON cast.
    // /tv/{id}/aggregate_credits is series-wide but has a different row shape
    // (roles[] instead of character) — out of scope for Phase 2b.
    const auto& credits = root["credits"];
    if (credits.isObject()) {
        const auto& cast = credits["cast"];
        if (cast.isArray()) {
            constexpr int kMaxCast = 6;
            int taken = 0;
            for (const auto& c : cast) {
                if (taken >= kMaxCast) break;
                std::string name = c.get("name", "").asString();
                if (!name.empty()) {
                    d.cast_top.push_back(std::move(name));
                    ++taken;
                }
            }
        }
    }

    return d;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: `All tests passed (… assertions in 186 test cases)` — 179 + 7 new.

- [ ] **Step 7: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/tmdb_client.h \
  magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp \
  magic_dingus_box_cpp/tests/media_browser/test_tmdb_tv.cpp \
  magic_dingus_box_cpp/tests/media_browser/fixtures/tmdb/tv_detail.json \
  magic_dingus_box_cpp/tests/media_browser/fixtures/tmdb/tv_genres.json
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): TMDB TV detail with seasons[] + TV genre list

get_tv_detail appends credits in one round-trip and keeps season 0
(Specials) so the season UI can render Sonarr's unmonitored-specials state
honestly. get_tv_genres is a separate call because TV and movie genre ids
are different spaces.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3b: Promote `normalize_tmdb_poster_url` to a `RadarrParsers` static (no behavior change)

Split out of Task 4 deliberately. Task 4 is new, isolated code in a brand-new directory; this is a visibility refactor of **shipped** Radarr code that makes a formerly file-local helper part of `RadarrParsers`' public surface and establishes a permanent build dependency of `sonarr_parsers.cpp` on `radarr_parsers.h`. A reviewer might reasonably reject this in favour of a neutral `servarr/servarr_image.h` while still approving the Sonarr parsers — and could not do so if the two were one commit.

**Files:**
- Modify: `src/media_browser/radarr/radarr_parsers.h`
- Modify: `src/media_browser/radarr/radarr_parsers.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `static std::string media_browser::RadarrParsers::normalize_tmdb_poster_url(const std::string& url);` — rewrites the `/t/p/<size>/` segment of a TMDB image URL to `w500`; non-TMDB URLs pass through unchanged. Task 4's `SonarrParsers` calls it.

---

- [ ] **Step 1: Declare it public**

In `src/media_browser/radarr/radarr_parsers.h`, add to the `RadarrParsers` public block, after `parse_active_searches`:

```cpp
    // Rewrites the "/t/p/<size>/" segment of a TMDB image URL to w500;
    // non-TMDB URLs (TVDB, fanart.tv) pass through unchanged. Public because
    // SonarrParsers needs the identical behaviour — Sonarr serves a mix of
    // TMDB and TVDB artwork and the 256MB artwork-cache budget applies to
    // both libraries. Was a file-local helper until Phase 2b.
    static std::string normalize_tmdb_poster_url(const std::string& url);
```

- [ ] **Step 2: Move the definition out of the anonymous namespace**

In `src/media_browser/radarr/radarr_parsers.cpp`, cut the existing definition — the function plus the long comment block above it that explains the w500 rationale — out of the anonymous namespace, and paste it back **after** the anonymous namespace's closing `}  // namespace`, qualified as a member:

```cpp
std::string RadarrParsers::normalize_tmdb_poster_url(const std::string& url) {
    // Match ".../t/p/<size>/..." and replace <size> with w500. <size> is
    // "original" or a "w###"/"h###" token; we only touch that one segment.
    static const std::string kMarker = "/t/p/";
    auto marker_pos = url.find(kMarker);
    if (marker_pos == std::string::npos) return url;  // not a TMDB image URL
    const std::size_t size_start = marker_pos + kMarker.size();
    const auto size_end = url.find('/', size_start);
    if (size_end == std::string::npos) return url;    // malformed — leave it
    return url.substr(0, size_start) + "w500" + url.substr(size_end);
}
```

There is exactly **one** internal call site — inside `pick_image`, which is the function's only caller anywhere in the tree. Update it:

```cpp
            return RadarrParsers::normalize_tmdb_poster_url(img["remoteUrl"].asString());
```

`pick_image` sits in the anonymous namespace *above* the new member definition, but no forward declaration is needed: `radarr_parsers.h` is included at the top of the file and now declares the member.

- [ ] **Step 3: Run the suite — this refactor's only proof is that nothing moved**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit" "[radarr][parsers]"
```
Expected: every `[radarr][parsers]` case passes, **including** the existing `parse_movie_lookup` case in `test_radarr_parsers.cpp` that asserts `poster_url` contains `/t/p/w500/` and no `/original/` segment. That case exercises the moved function through `parse_movie_lookup` and is the whole behavior-preservation proof — this task adds no test of its own because there is no new behavior to test.

Then the full suite:
```bash
"/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: `All tests passed (… assertions in 186 test cases)` — unchanged from Task 3. A refactor that changes the count has changed behavior.

- [ ] **Step 4: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.h \
  magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.cpp
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
refactor(mb): promote normalize_tmdb_poster_url to a RadarrParsers static

Sonarr serves a mix of TMDB and TVDB artwork and needs identical w500
normalization; a second copy would fork. No behavior change — the existing
w500 assertions in test_radarr_parsers.cpp are the proof.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Sonarr types + parsers (fixture-driven)

**Files:**
- Create: `src/media_browser/sonarr/sonarr_types.h`
- Create: `src/media_browser/sonarr/sonarr_parsers.h`
- Create: `src/media_browser/sonarr/sonarr_parsers.cpp`
- Create: `tests/media_browser/fixtures/sonarr/series_lookup.json`, `series_added_pending.json`, `series_added.json`, `series_list.json`, `queue.json`, `history_series.json`, `root_folders.json`, `quality_profiles.json`
- Create: `tests/media_browser/test_sonarr_parsers.cpp`
- Modify: `CMakeLists.txt` (`MEDIA_BROWSER_SOURCES` += `sonarr_parsers.cpp`; `MEDIA_BROWSER_TEST_SOURCES` += `test_sonarr_parsers.cpp`)

**Interfaces:**
- Consumes: `media_browser::QualityProfile`, `media_browser::RootFolder` from `radarr_types.h`; `RadarrParsers::parse_quality_profiles` / `parse_root_folders`; and `RadarrParsers::normalize_tmdb_poster_url` **(made public by Task 3b — this task does not touch `radarr_parsers.*`)**.
- Produces (all in `namespace media_browser`):
  - `struct Season { int season_number; bool monitored; int episode_count; int episode_file_count; int64_t size_on_disk_bytes; };`
  - `struct Episode { int id; int series_id; int season_number; int episode_number; std::string title; std::string air_date; };`
  - `struct SeriesSearchHit { int tvdb_id; int tmdb_id; std::string imdb_id; std::string title; std::string overview; int year; int runtime_minutes; std::string status; std::string poster_url; std::string fanart_url; std::vector<Season> seasons; };`
  - `struct Series : SeriesSearchHit { int sonarr_id; bool monitored; std::string path; std::string added_at; int episode_file_count; int64_t size_on_disk_bytes; };`
  - `struct SonarrQueueItem { int id; int series_id; int episode_id; int season_number; std::string title; int64_t size_bytes; int64_t sizeleft_bytes; double progress; int eta_seconds; std::string state; std::string tracked_download_state; std::string download_id; Episode episode; };`
  - `class SonarrParsers` with statics: `parse_series_lookup`, `parse_series_list`, `parse_series`, `parse_queue`, `parse_queue_total`, `parse_history_download_ids`, `parse_quality_profiles`, `parse_root_folders` (exact signatures in Step 4).

---

- [ ] **Step 1: Create the eight Sonarr fixtures**

`tests/media_browser/fixtures/sonarr/series_lookup.json` — what `/api/v3/series/lookup?term=tmdb:1396` returns for a series NOT yet in the library (`id: 0`, no `path`, every season `monitored: true`):

```json
[
  {
    "title": "Breaking Bad",
    "sortTitle": "breaking bad",
    "status": "ended",
    "overview": "Walter White, a New Mexico chemistry teacher, is diagnosed with cancer.",
    "network": "AMC",
    "year": 2008,
    "runtime": 47,
    "tvdbId": 81189,
    "tmdbId": 1396,
    "imdbId": "tt0903747",
    "titleSlug": "breaking-bad",
    "seriesType": "standard",
    "seasonFolder": true,
    "monitored": true,
    "id": 0,
    "images": [
      {"coverType": "poster", "remoteUrl": "https://image.tmdb.org/t/p/original/ggFHVNu6YYI5L9pCfOacjizRGt.jpg"},
      {"coverType": "fanart", "remoteUrl": "https://artworks.thetvdb.com/banners/fanart/original/81189-3.jpg"}
    ],
    "seasons": [
      {"seasonNumber": 0, "monitored": true},
      {"seasonNumber": 1, "monitored": true},
      {"seasonNumber": 2, "monitored": true},
      {"seasonNumber": 3, "monitored": true},
      {"seasonNumber": 4, "monitored": true},
      {"seasonNumber": 5, "monitored": true}
    ]
  }
]
```

`tests/media_browser/fixtures/sonarr/series_added_pending.json` — what `POST /api/v3/series` returns, and what `GET /api/v3/series/7` keeps returning, **until Sonarr's async refresh runs**. `addOptions` is still populated, every season is still `monitored: true` (that is what the lookup resource carried and what was submitted), and `statistics.totalEpisodeCount` is 0 because SkyHook's episode list has not been fetched yet. This is the state the poll must reject:

```json
{
  "title": "Breaking Bad",
  "status": "ended",
  "overview": "Walter White, a New Mexico chemistry teacher, is diagnosed with cancer.",
  "year": 2008,
  "runtime": 47,
  "tvdbId": 81189,
  "tmdbId": 1396,
  "imdbId": "tt0903747",
  "path": "/data/library/tv/Breaking Bad",
  "rootFolderPath": "/data/library/tv",
  "qualityProfileId": 1,
  "seasonFolder": true,
  "monitored": true,
  "added": "2026-08-01T09:00:00Z",
  "id": 7,
  "addOptions": {
    "monitor": "firstSeason",
    "searchForMissingEpisodes": true,
    "searchForCutoffUnmetEpisodes": false
  },
  "images": [
    {"coverType": "poster", "remoteUrl": "https://image.tmdb.org/t/p/original/ggFHVNu6YYI5L9pCfOacjizRGt.jpg"},
    {"coverType": "fanart", "remoteUrl": "https://artworks.thetvdb.com/banners/fanart/original/81189-3.jpg"}
  ],
  "seasons": [
    {"seasonNumber": 0, "monitored": true},
    {"seasonNumber": 1, "monitored": true},
    {"seasonNumber": 2, "monitored": true},
    {"seasonNumber": 3, "monitored": true},
    {"seasonNumber": 4, "monitored": true},
    {"seasonNumber": 5, "monitored": true}
  ],
  "statistics": {"seasonCount": 5, "episodeFileCount": 0, "totalEpisodeCount": 0, "sizeOnDisk": 0}
}
```

`tests/media_browser/fixtures/sonarr/series_added.json` — what `GET /api/v3/series/7` returns **once the refresh has settled**: `addOptions` gone (`RefreshSeriesService` nulls it after applying the monitor enum), episode statistics populated, season 1 monitored and everything else **not**:

```json
{
  "title": "Breaking Bad",
  "status": "ended",
  "overview": "Walter White, a New Mexico chemistry teacher, is diagnosed with cancer.",
  "year": 2008,
  "runtime": 47,
  "tvdbId": 81189,
  "tmdbId": 1396,
  "imdbId": "tt0903747",
  "path": "/data/library/tv/Breaking Bad",
  "rootFolderPath": "/data/library/tv",
  "qualityProfileId": 1,
  "seasonFolder": true,
  "monitored": true,
  "added": "2026-08-01T09:00:00Z",
  "id": 7,
  "images": [
    {"coverType": "poster", "remoteUrl": "https://image.tmdb.org/t/p/original/ggFHVNu6YYI5L9pCfOacjizRGt.jpg"},
    {"coverType": "fanart", "remoteUrl": "https://artworks.thetvdb.com/banners/fanart/original/81189-3.jpg"}
  ],
  "seasons": [
    {"seasonNumber": 0, "monitored": false,
     "statistics": {"episodeFileCount": 0, "totalEpisodeCount": 5, "sizeOnDisk": 0}},
    {"seasonNumber": 1, "monitored": true,
     "statistics": {"episodeFileCount": 7, "totalEpisodeCount": 7, "sizeOnDisk": 8589934592}},
    {"seasonNumber": 2, "monitored": false,
     "statistics": {"episodeFileCount": 0, "totalEpisodeCount": 13, "sizeOnDisk": 0}},
    {"seasonNumber": 3, "monitored": false,
     "statistics": {"episodeFileCount": 0, "totalEpisodeCount": 13, "sizeOnDisk": 0}},
    {"seasonNumber": 4, "monitored": false,
     "statistics": {"episodeFileCount": 0, "totalEpisodeCount": 13, "sizeOnDisk": 0}},
    {"seasonNumber": 5, "monitored": false,
     "statistics": {"episodeFileCount": 0, "totalEpisodeCount": 16, "sizeOnDisk": 0}}
  ],
  "statistics": {"seasonCount": 5, "episodeFileCount": 7, "totalEpisodeCount": 62, "sizeOnDisk": 8589934592}
}
```

`tests/media_browser/fixtures/sonarr/series_list.json` — `GET /api/v3/series` returns an array; reuse the same object:

```json
[
  {
    "title": "Breaking Bad",
    "status": "ended",
    "overview": "Walter White, a New Mexico chemistry teacher, is diagnosed with cancer.",
    "year": 2008,
    "runtime": 47,
    "tvdbId": 81189,
    "tmdbId": 1396,
    "imdbId": "tt0903747",
    "path": "/data/library/tv/Breaking Bad",
    "qualityProfileId": 1,
    "monitored": true,
    "added": "2026-08-01T09:00:00Z",
    "id": 7,
    "images": [
      {"coverType": "poster", "remoteUrl": "https://image.tmdb.org/t/p/original/ggFHVNu6YYI5L9pCfOacjizRGt.jpg"},
      {"coverType": "fanart", "remoteUrl": "https://artworks.thetvdb.com/banners/fanart/original/81189-3.jpg"}
    ],
    "seasons": [
      {"seasonNumber": 0, "monitored": false,
       "statistics": {"episodeFileCount": 0, "totalEpisodeCount": 5, "sizeOnDisk": 0}},
      {"seasonNumber": 1, "monitored": true,
       "statistics": {"episodeFileCount": 7, "totalEpisodeCount": 7, "sizeOnDisk": 8589934592}}
    ],
    "statistics": {"seasonCount": 5, "episodeFileCount": 7, "totalEpisodeCount": 62, "sizeOnDisk": 8589934592}
  }
]
```

`tests/media_browser/fixtures/sonarr/queue.json` — one season-2 pack, three episode records, **one shared `downloadId`**:

```json
{
  "page": 1,
  "pageSize": 100,
  "totalRecords": 3,
  "records": [
    {
      "id": 101,
      "seriesId": 7,
      "episodeId": 5001,
      "seasonNumber": 2,
      "title": "Breaking.Bad.S02.1080p.WEB-DL.x264-GROUPA",
      "size": 12884901888,
      "sizeleft": 6442450944,
      "status": "downloading",
      "trackedDownloadState": "downloading",
      "trackedDownloadStatus": "ok",
      "downloadId": "A1B2C3D4E5F60718293A4B5C6D7E8F9012345678",
      "protocol": "torrent",
      "downloadClient": "qBittorrent",
      "timeleft": "01:20:00",
      "episode": {"id": 5001, "seriesId": 7, "seasonNumber": 2,
                  "episodeNumber": 1, "title": "Seven Thirty-Seven",
                  "airDate": "2009-03-08"}
    },
    {
      "id": 102,
      "seriesId": 7,
      "episodeId": 5002,
      "seasonNumber": 2,
      "title": "Breaking.Bad.S02.1080p.WEB-DL.x264-GROUPA",
      "size": 12884901888,
      "sizeleft": 6442450944,
      "status": "downloading",
      "trackedDownloadState": "downloading",
      "trackedDownloadStatus": "ok",
      "downloadId": "A1B2C3D4E5F60718293A4B5C6D7E8F9012345678",
      "protocol": "torrent",
      "downloadClient": "qBittorrent",
      "timeleft": "01:20:00",
      "episode": {"id": 5002, "seriesId": 7, "seasonNumber": 2,
                  "episodeNumber": 2, "title": "Grilled",
                  "airDate": "2009-03-15"}
    },
    {
      "id": 103,
      "seriesId": 7,
      "episodeId": 5003,
      "seasonNumber": 2,
      "title": "Breaking.Bad.S02.1080p.WEB-DL.x264-GROUPA",
      "size": 12884901888,
      "sizeleft": 6442450944,
      "status": "downloading",
      "trackedDownloadState": "downloading",
      "trackedDownloadStatus": "ok",
      "downloadId": "A1B2C3D4E5F60718293A4B5C6D7E8F9012345678",
      "protocol": "torrent",
      "downloadClient": "qBittorrent",
      "timeleft": "01:20:00",
      "episode": {"id": 5003, "seriesId": 7, "seasonNumber": 2,
                  "episodeNumber": 3, "title": "Bit by a Dead Bee",
                  "airDate": "2009-03-22"}
    }
  ]
}
```

`tests/media_browser/fixtures/sonarr/history_series.json` — `GET /api/v3/history/series?seriesId=7` is UNPAGINATED (a bare array), and carries duplicates, mixed case and an empty id:

```json
[
  {"id": 900, "seriesId": 7, "episodeId": 5001, "eventType": "grabbed",
   "sourceTitle": "Breaking.Bad.S02.1080p.WEB-DL.x264-GROUPA",
   "date": "2026-08-01T10:00:00Z",
   "downloadId": "A1B2C3D4E5F60718293A4B5C6D7E8F9012345678"},
  {"id": 901, "seriesId": 7, "episodeId": 5002, "eventType": "grabbed",
   "sourceTitle": "Breaking.Bad.S02.1080p.WEB-DL.x264-GROUPA",
   "date": "2026-08-01T10:00:01Z",
   "downloadId": "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678"},
  {"id": 902, "seriesId": 7, "episodeId": 4001, "eventType": "downloadFolderImported",
   "sourceTitle": "Breaking.Bad.S01.1080p.WEB-DL.x264-GROUPB",
   "date": "2026-07-30T08:00:00Z",
   "downloadId": "FFEEDDCCBBAA99887766554433221100AABBCCDD"},
  {"id": 903, "seriesId": 7, "episodeId": 4002, "eventType": "episodeFileDeleted",
   "sourceTitle": "old file", "date": "2026-07-29T08:00:00Z",
   "downloadId": ""}
]
```

`tests/media_browser/fixtures/sonarr/root_folders.json`:

```json
[
  {"id": 1, "path": "/data/library/tv", "accessible": true,
   "freeSpace": 187904819200, "unmappedFolders": []}
]
```

`tests/media_browser/fixtures/sonarr/quality_profiles.json`:

```json
[
  {"id": 1, "name": "Any", "upgradeAllowed": true, "cutoff": 1,
   "minFormatScore": -200, "cutoffFormatScore": 0}
]
```

- [ ] **Step 2: Write the failing parser tests**

Create `tests/media_browser/test_sonarr_parsers.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "media_browser/sonarr/sonarr_parsers.h"

namespace fs = std::filesystem;
namespace mb = media_browser;

static std::string read_fixture(const std::string& name) {
    fs::path p = fs::path(__FILE__).parent_path() / "fixtures" / "sonarr" / name;
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_CASE("parse_series_lookup extracts the not-yet-added series", "[sonarr][parsers]") {
    auto hits = mb::SonarrParsers::parse_series_lookup(read_fixture("series_lookup.json"));
    REQUIRE(hits.size() == 1);
    const auto& h = hits[0];
    CHECK(h.title == "Breaking Bad");
    CHECK(h.tvdb_id == 81189);
    CHECK(h.tmdb_id == 1396);
    CHECK(h.imdb_id == "tt0903747");
    CHECK(h.year == 2008);
    // Sonarr's series.runtime is PER EPISODE — it is what the whole-series
    // disk estimate multiplies by, so it must survive the parse.
    CHECK(h.runtime_minutes == 47);
    CHECK(h.status == "ended");
    // Lookup results carry every season monitored:true — that is Sonarr's
    // pre-add default, NOT what the add will persist.
    REQUIRE(h.seasons.size() == 6);
    CHECK(h.seasons[0].season_number == 0);
    for (const auto& s : h.seasons) CHECK(s.monitored);
}

TEST_CASE("parse_series_lookup normalizes TMDB artwork to w500 and passes "
          "TVDB URLs through", "[sonarr][parsers]") {
    auto hits = mb::SonarrParsers::parse_series_lookup(read_fixture("series_lookup.json"));
    REQUIRE(hits.size() == 1);
    // TMDB poster: rewritten so it shares the artwork-cache key TmdbClient
    // already emits, and so a 2000x3000 original never reaches the 256MB cache.
    CHECK(hits[0].poster_url.find("/t/p/w500/") != std::string::npos);
    CHECK(hits[0].poster_url.find("/original/") == std::string::npos);
    // TVDB fanart: not a TMDB URL, so it passes through untouched.
    CHECK(hits[0].fanart_url ==
          "https://artworks.thetvdb.com/banners/fanart/original/81189-3.jpg");
}

TEST_CASE("parse_series reads per-season monitored flags and statistics",
          "[sonarr][parsers]") {
    auto s = mb::SonarrParsers::parse_series(read_fixture("series_added.json"));
    REQUIRE(s.has_value());
    CHECK(s->sonarr_id == 7);
    CHECK(s->tvdb_id == 81189);
    CHECK(s->tmdb_id == 1396);
    CHECK(s->monitored);
    CHECK(s->path == "/data/library/tv/Breaking Bad");
    CHECK(s->added_at == "2026-08-01T09:00:00Z");
    CHECK(s->episode_file_count == 7);
    CHECK(s->size_on_disk_bytes == 8589934592LL);

    REQUIRE(s->seasons.size() == 6);
    CHECK(s->seasons[0].season_number == 0);
    CHECK_FALSE(s->seasons[0].monitored);          // Specials
    CHECK(s->seasons[0].episode_count == 5);
    CHECK(s->seasons[1].season_number == 1);
    CHECK(s->seasons[1].monitored);                // the ONLY monitored season
    CHECK(s->seasons[1].episode_count == 7);
    CHECK(s->seasons[1].episode_file_count == 7);
    CHECK(s->seasons[1].size_on_disk_bytes == 8589934592LL);
    for (size_t i = 2; i < s->seasons.size(); ++i) {
        CHECK_FALSE(s->seasons[i].monitored);
    }
}

TEST_CASE("parse_series_list parses the library array", "[sonarr][parsers]") {
    auto list = mb::SonarrParsers::parse_series_list(read_fixture("series_list.json"));
    REQUIRE(list.size() == 1);
    CHECK(list[0].sonarr_id == 7);
    CHECK(list[0].title == "Breaking Bad");
    CHECK(list[0].seasons.size() == 2);
}

TEST_CASE("parse_series_list and parse_series reject wrong shapes",
          "[sonarr][parsers]") {
    CHECK(mb::SonarrParsers::parse_series_list("not json {{{").empty());
    CHECK(mb::SonarrParsers::parse_series_list(R"({"error":"x"})").empty());
    CHECK_FALSE(mb::SonarrParsers::parse_series("not json {{{").has_value());
    CHECK_FALSE(mb::SonarrParsers::parse_series("[]").has_value());
}

TEST_CASE("parse_queue keeps one record per EPISODE and never groups",
          "[sonarr][parsers][queue]") {
    auto q = mb::SonarrParsers::parse_queue(read_fixture("queue.json"));
    // A season pack is N episode rows sharing ONE downloadId. The client
    // deliberately does not collapse them — grouping is Phase 2c's UI job,
    // and it needs the raw rows plus the shared id to do it.
    REQUIRE(q.size() == 3);
    CHECK(q[0].id == 101);
    CHECK(q[1].id == 102);
    CHECK(q[2].id == 103);
    CHECK(q[0].download_id == q[1].download_id);
    CHECK(q[1].download_id == q[2].download_id);
    // Raw casing preserved (uppercase hex), matching RadarrParsers::parse_queue
    // — QueueScreen lowercases at comparison time against qBit.
    CHECK(q[0].download_id == "A1B2C3D4E5F60718293A4B5C6D7E8F9012345678");

    CHECK(q[0].series_id == 7);
    CHECK(q[0].episode_id == 5001);
    CHECK(q[0].season_number == 2);
    CHECK(q[0].size_bytes == 12884901888LL);
    CHECK(q[0].sizeleft_bytes == 6442450944LL);
    CHECK(q[0].progress > 0.49);
    CHECK(q[0].progress < 0.51);
    CHECK(q[0].eta_seconds == 4800);            // "01:20:00"
    CHECK(q[0].state == "downloading");
    CHECK(q[0].tracked_download_state == "downloading");
    // Embedded episode (requested via includeEpisode=true) gives 2c the
    // "S02E01 — Seven Thirty-Seven" label without a second round-trip.
    CHECK(q[0].episode.id == 5001);
    CHECK(q[0].episode.episode_number == 1);
    CHECK(q[0].episode.title == "Seven Thirty-Seven");
    CHECK(q[0].episode.air_date == "2009-03-08");
    CHECK(q[2].episode.episode_number == 3);
}

TEST_CASE("parse_queue accepts the staged sizeLeft rename", "[sonarr][parsers][queue]") {
    // 'sizeleft' is marked [Obsolete] upstream with 'SizeLeft' staged but
    // commented out. Parse both so a Sonarr upgrade cannot silently zero
    // every progress bar.
    const std::string json = R"({"records":[
      {"id": 1, "seriesId": 7, "episodeId": 2, "seasonNumber": 1,
       "title": "T", "size": 1000, "sizeLeft": 250, "status": "downloading"}
    ]})";
    auto q = mb::SonarrParsers::parse_queue(json);
    REQUIRE(q.size() == 1);
    CHECK(q[0].sizeleft_bytes == 250);
    CHECK(q[0].progress > 0.74);
    CHECK(q[0].progress < 0.76);
}

TEST_CASE("parse_queue survives an empty/absent records array",
          "[sonarr][parsers][queue]") {
    CHECK(mb::SonarrParsers::parse_queue(R"({"records":[]})").empty());
    CHECK(mb::SonarrParsers::parse_queue(R"({"page":1})").empty());
    CHECK(mb::SonarrParsers::parse_queue("not json {{{").empty());
}

TEST_CASE("parse_queue_total reads totalRecords for the pagination loop",
          "[sonarr][parsers][queue]") {
    // get_queue pages through the queue; without totalRecords it cannot tell
    // "that was the last page" from "the page happened to be full".
    CHECK(mb::SonarrParsers::parse_queue_total(read_fixture("queue.json")) == 3);
    CHECK(mb::SonarrParsers::parse_queue_total(R"({"totalRecords":250,"records":[]})") == 250);
    // Absent/malformed → 0, which the loop treats as "no total available"
    // and falls back to the short-page test.
    CHECK(mb::SonarrParsers::parse_queue_total(R"({"records":[]})") == 0);
    CHECK(mb::SonarrParsers::parse_queue_total("not json {{{") == 0);
}

TEST_CASE("parse_history_download_ids dedupes case-insensitively and lowercases",
          "[sonarr][parsers][history]") {
    auto ids = mb::SonarrParsers::parse_history_download_ids(
        read_fixture("history_series.json"));
    // 4 records: two are the same hash in different case, one distinct, one
    // empty. qBittorrent stores hashes lowercase, so the orphan-proof remove
    // must hand it lowercase and must not ask twice for the same torrent.
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(ids[1] == "ffeeddccbbaa99887766554433221100aabbccdd");
}

TEST_CASE("parse_history_download_ids accepts a paged body too",
          "[sonarr][parsers][history]") {
    // /history/series is unpaginated, but /history is paged — tolerate both
    // so a caller switching endpoints does not silently get nothing.
    const std::string paged =
        R"({"records":[{"id":1,"downloadId":"ABC"},{"id":2,"downloadId":"abc"}]})";
    auto ids = mb::SonarrParsers::parse_history_download_ids(paged);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "abc");
}

TEST_CASE("parse_quality_profiles and parse_root_folders reuse the Radarr shapes",
          "[sonarr][parsers]") {
    auto profiles = mb::SonarrParsers::parse_quality_profiles(
        read_fixture("quality_profiles.json"));
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].id == 1);
    // Resolve BY NAME at every call site — the id is 1 on this box only.
    CHECK(profiles[0].name == "Any");

    auto roots = mb::SonarrParsers::parse_root_folders(read_fixture("root_folders.json"));
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].id == 1);
    CHECK(roots[0].path == "/data/library/tv");
    CHECK(roots[0].free_space_bytes == 187904819200LL);
}
```

Add both new source files to `CMakeLists.txt`.

**Anchor warning — `CMakeLists.txt` contains TWO source lists whose entries differ only by indentation.** `radarr_mock.cpp`, `prowlarr_client.cpp`, `qbittorrent_client.cpp` and several others appear in BOTH: once inside `list(APPEND KIOSK_MEDIA_BROWSER_SOURCES` at indent 12 (the kiosk binary, Task 8's target) and once inside `set(MEDIA_BROWSER_SOURCES` at indent 8 (the test + CLI targets, this task's target). A path that appears in both is never a safe anchor, and neither is a path that also appears inside some other `list(APPEND ...)` call on a single line (`src/platform/sequence_detector.cpp` is in `PLATFORM_SOURCES` that way).

The only anchors guaranteed unique are the list-opening lines themselves. Verify before editing:

```bash
grep -c "set(MEDIA_BROWSER_SOURCES" "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/CMakeLists.txt"
```
Expected: `1`.

Insert directly beneath the opening line of `set(MEDIA_BROWSER_SOURCES` (its first entry today is `src/media_browser/library/library_db.cpp`):

```cmake
    set(MEDIA_BROWSER_SOURCES
        src/media_browser/sonarr/sonarr_parsers.cpp
        src/media_browser/library/library_db.cpp
```

For the test list, anchor on `tests/media_browser/test_radarr_client.cpp`, which occurs exactly once in the whole file:

```cmake
        tests/media_browser/test_radarr_client.cpp
        tests/media_browser/test_sonarr_parsers.cpp
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
cmake -S "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp" \
      -B "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" \
      -DBUILD_KIOSK=OFF -DBUILD_TESTS=ON -DENABLE_MEDIA_BROWSER=ON \
  && cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8
```
(The re-configure is needed because the source lists changed.)
Expected: **configure or compile FAILS** — CMake cannot find `src/media_browser/sonarr/sonarr_parsers.cpp`, and/or the test cannot find `media_browser/sonarr/sonarr_parsers.h`.

- [ ] **Step 4: Create the types header**

`src/media_browser/sonarr/sonarr_types.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// QualityProfile and RootFolder are Servarr-generic: Sonarr's
// /api/v3/qualityprofile and /api/v3/rootfolder payloads are the same shape
// Radarr serves, and both live in namespace media_browser — redefining them
// here would be a redefinition error, and duplicating them under new names
// would fork two identical structs. Reuse them.
#include "media_browser/radarr/radarr_types.h"

namespace media_browser {

// One season of a series. Sonarr models monitoring per season, which is what
// makes "season at a time" configuration rather than engineering.
struct Season {
    int season_number = 0;          // 0 == Specials
    bool monitored = false;
    int episode_count = 0;          // statistics.totalEpisodeCount
    int episode_file_count = 0;     // statistics.episodeFileCount
    int64_t size_on_disk_bytes = 0; // statistics.sizeOnDisk
};

// A single episode. Phase 2b only ever populates this from a queue record's
// embedded `episode` object (requested via includeEpisode=true) — there is no
// /api/v3/episode fetch until Phase 3's episode picker needs one.
struct Episode {
    int id = 0;
    int series_id = 0;
    int season_number = 0;
    int episode_number = 0;
    std::string title;
    std::string air_date;   // ISO yyyy-mm-dd; empty for unaired/unknown
};

// A series as returned by /api/v3/series/lookup — not yet in the library, so
// no Sonarr id and no path.
struct SeriesSearchHit {
    int tvdb_id = 0;         // Sonarr's primary key for adds; POST validates > 0
    int tmdb_id = 0;
    std::string imdb_id;
    std::string title;
    std::string overview;
    int year = 0;
    // PER-EPISODE runtime in minutes. This is the multiplicand in the
    // whole-series disk estimate (episodes x runtime x preferred MB/min).
    int runtime_minutes = 0;
    std::string status;      // "continuing" / "ended" / "upcoming"
    std::string poster_url;  // w500-normalized when TMDB-sourced
    std::string fanart_url;
    std::vector<Season> seasons;
};

// A series in the library.
struct Series : SeriesSearchHit {
    int sonarr_id = 0;
    bool monitored = false;
    // Container-internal path, e.g. "/data/library/tv/Breaking Bad".
    // Run it through SonarrClient::resolve_host_path before handing it to
    // anything on the host (GStreamer, stat()).
    std::string path;
    std::string added_at;            // ISO 8601
    int episode_file_count = 0;      // statistics.episodeFileCount
    int64_t size_on_disk_bytes = 0;  // statistics.sizeOnDisk
};

// One /api/v3/queue record. Sonarr's queue is per EPISODE: a season pack
// yields N of these sharing ONE download_id. This type is deliberately NOT
// pre-grouped — Phase 2c groups by download_id for display, and
// DELETE /api/v3/queue/{id}?removeFromClient=true acts on the whole download
// (every sibling row 404s afterwards), so the UI needs the raw rows to know
// which ids belong together.
struct SonarrQueueItem {
    int id = 0;              // queue row id — the delete key
    int series_id = 0;
    int episode_id = 0;
    int season_number = 0;
    std::string title;       // release title (identical across a pack's rows)
    int64_t size_bytes = 0;
    int64_t sizeleft_bytes = 0;
    double progress = 0.0;   // 0.0 - 1.0, derived from size/sizeleft
    int eta_seconds = 0;     // from "timeleft"
    std::string state;                   // status: queued/downloading/completed/failed
    std::string tracked_download_state;  // importBlocked / importPending / imported / ...
    // Raw casing as Sonarr emits it (uppercase hex in practice, == the qBit
    // info_hash). Consumers lowercase at comparison time, exactly as
    // QueueScreen already does for Radarr's queue.
    std::string download_id;
    Episode episode;         // populated when the request set includeEpisode=true
};

}  // namespace media_browser
```

- [ ] **Step 5: Create the parsers**

`src/media_browser/sonarr/sonarr_parsers.h`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>
#include "media_browser/sonarr/sonarr_types.h"

namespace media_browser {

// Stateless parsers over Sonarr v4's /api/v3 payloads. Same arrangement as
// RadarrParsers: pure statics so every shape is unit-testable from a fixture
// with no network and no client instance.
class SonarrParsers {
public:
    // GET /api/v3/series/lookup?term=... — array of not-yet-added series.
    static std::vector<SeriesSearchHit> parse_series_lookup(const std::string& json);
    // GET /api/v3/series (optionally ?tvdbId=) — array of library series.
    static std::vector<Series> parse_series_list(const std::string& json);
    // GET /api/v3/series/{id} — a single library series object.
    static std::optional<Series> parse_series(const std::string& json);
    // GET /api/v3/queue — paged {records:[...]}, one record per EPISODE.
    static std::vector<SonarrQueueItem> parse_queue(const std::string& json);
    // The paged envelope's totalRecords. 0 when absent or unparseable.
    // SonarrClient::get_queue uses it to know when it has read every page.
    static int parse_queue_total(const std::string& json);
    // GET /api/v3/history/series?seriesId= — bare array (the paged
    // {records:[...]} form is also accepted). Returns distinct downloadIds,
    // lowercased for qBittorrent comparison, in first-seen order.
    static std::vector<std::string> parse_history_download_ids(const std::string& json);
    // Servarr-identical shapes — these delegate to RadarrParsers so there is
    // exactly one implementation of each.
    static std::vector<QualityProfile> parse_quality_profiles(const std::string& json);
    static std::vector<RootFolder> parse_root_folders(const std::string& json);
};

}  // namespace media_browser
```

`src/media_browser/sonarr/sonarr_parsers.cpp`:

```cpp
#include "media_browser/sonarr/sonarr_parsers.h"
#include "media_browser/radarr/radarr_parsers.h"

#include <json/json.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>

namespace media_browser {

namespace {

bool parse_json(const std::string& text, Json::Value& out) {
    Json::CharReaderBuilder rb;
    std::string err;
    std::istringstream is(text);
    return Json::parseFromStream(rb, is, &out, &err);
}

std::string pick_image(const Json::Value& images, const std::string& coverType) {
    if (!images.isArray()) return "";
    for (const auto& img : images) {
        if (img["coverType"].asString() == coverType) {
            // Sonarr mixes TMDB and TVDB/fanart.tv artwork. The TMDB ones get
            // downsized to w500 (shared artwork-cache key + the 256MB budget);
            // the rest pass through untouched.
            return RadarrParsers::normalize_tmdb_poster_url(
                img["remoteUrl"].asString());
        }
    }
    return "";
}

// "HH:MM:SS" and "D.HH:MM:SS" → total seconds; 0 on failure. Same format
// Sonarr uses for queue timeleft as Radarr does.
int parse_timeleft_to_seconds(const std::string& s) {
    if (s.empty()) return 0;
    int days = 0;
    size_t time_start = 0;
    size_t dot = s.find('.');
    size_t first_colon = s.find(':');
    if (dot != std::string::npos && first_colon != std::string::npos &&
        dot < first_colon) {
        try {
            days = std::stoi(s.substr(0, dot));
        } catch (...) {
            return 0;
        }
        time_start = dot + 1;
    }
    const std::string t = s.substr(time_start);
    int hh = 0, mm = 0, ss = 0;
    if (std::sscanf(t.c_str(), "%d:%d:%d", &hh, &mm, &ss) != 3) return 0;
    if (hh < 0 || mm < 0 || ss < 0) return 0;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

void fill_seasons(const Json::Value& r, std::vector<Season>& out) {
    const auto& seasons = r["seasons"];
    if (!seasons.isArray()) return;
    for (const auto& s : seasons) {
        Season season;
        season.season_number = s.get("seasonNumber", 0).asInt();
        season.monitored     = s.get("monitored", false).asBool();
        const auto& st = s["statistics"];
        if (st.isObject()) {
            season.episode_count      = st.get("totalEpisodeCount", 0).asInt();
            season.episode_file_count = st.get("episodeFileCount", 0).asInt();
            season.size_on_disk_bytes = st.get("sizeOnDisk", 0).asInt64();
        }
        out.push_back(std::move(season));
    }
}

void fill_search_hit(const Json::Value& r, SeriesSearchHit& h) {
    h.tvdb_id         = r.get("tvdbId", 0).asInt();
    h.tmdb_id         = r.get("tmdbId", 0).asInt();
    h.imdb_id         = r.get("imdbId", "").asString();
    h.title           = r.get("title", "").asString();
    h.overview        = r.get("overview", "").asString();
    h.year            = r.get("year", 0).asInt();
    h.runtime_minutes = r.get("runtime", 0).asInt();
    h.status          = r.get("status", "").asString();
    h.poster_url      = pick_image(r["images"], "poster");
    h.fanart_url      = pick_image(r["images"], "fanart");
    fill_seasons(r, h.seasons);
}

void fill_library_fields(const Json::Value& r, Series& s) {
    s.sonarr_id = r.get("id", 0).asInt();
    s.monitored = r.get("monitored", false).asBool();
    s.path      = r.get("path", "").asString();
    s.added_at  = r.get("added", "").asString();
    const auto& st = r["statistics"];
    if (st.isObject()) {
        s.episode_file_count = st.get("episodeFileCount", 0).asInt();
        s.size_on_disk_bytes = st.get("sizeOnDisk", 0).asInt64();
    }
}

// Both the bare-array (/history/series) and paged (/history) shapes.
const Json::Value* records_of(const Json::Value& root) {
    if (root.isArray()) return &root;
    if (root.isObject() && root.isMember("records") && root["records"].isArray()) {
        return &root["records"];
    }
    return nullptr;
}

}  // namespace

std::vector<SeriesSearchHit> SonarrParsers::parse_series_lookup(const std::string& json) {
    std::vector<SeriesSearchHit> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        SeriesSearchHit h;
        fill_search_hit(r, h);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<Series> SonarrParsers::parse_series_list(const std::string& json) {
    std::vector<Series> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        Series s;
        fill_search_hit(r, s);
        fill_library_fields(r, s);
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<Series> SonarrParsers::parse_series(const std::string& json) {
    Json::Value root;
    if (!parse_json(json, root) || !root.isObject()) return std::nullopt;
    Series s;
    fill_search_hit(root, s);
    fill_library_fields(root, s);
    return s;
}

std::vector<SonarrQueueItem> SonarrParsers::parse_queue(const std::string& json) {
    std::vector<SonarrQueueItem> out;
    Json::Value root;
    if (!parse_json(json, root)) return out;
    const auto& records = root["records"];
    if (!records.isArray()) return out;
    for (const auto& r : records) {
        SonarrQueueItem q;
        q.id            = r.get("id", 0).asInt();
        q.series_id     = r.get("seriesId", 0).asInt();
        q.episode_id    = r.get("episodeId", 0).asInt();
        q.season_number = r.get("seasonNumber", 0).asInt();
        q.title         = r.get("title", "").asString();
        q.size_bytes    = r.get("size", 0).asInt64();
        // 'sizeleft' is what serializes today; 'sizeLeft' is staged upstream
        // (the replacement property is committed but commented out). Accept
        // both so a Sonarr bump cannot silently zero every progress bar.
        q.sizeleft_bytes = r.isMember("sizeleft")
                             ? r.get("sizeleft", 0).asInt64()
                             : r.get("sizeLeft", 0).asInt64();
        q.state          = r.get("status", "").asString();
        q.tracked_download_state = r.get("trackedDownloadState", "").asString();
        q.download_id    = r.get("downloadId", "").asString();
        if (q.size_bytes > 0) {
            q.progress = static_cast<double>(q.size_bytes - q.sizeleft_bytes)
                         / static_cast<double>(q.size_bytes);
        }
        if (r.isMember("timeleft")) {
            q.eta_seconds = parse_timeleft_to_seconds(r["timeleft"].asString());
        }
        const auto& ep = r["episode"];
        if (ep.isObject()) {
            q.episode.id             = ep.get("id", 0).asInt();
            q.episode.series_id      = ep.get("seriesId", 0).asInt();
            q.episode.season_number  = ep.get("seasonNumber", 0).asInt();
            q.episode.episode_number = ep.get("episodeNumber", 0).asInt();
            q.episode.title          = ep.get("title", "").asString();
            q.episode.air_date       = ep.get("airDate", "").asString();
        }
        out.push_back(std::move(q));
    }
    return out;
}

int SonarrParsers::parse_queue_total(const std::string& json) {
    Json::Value root;
    if (!parse_json(json, root) || !root.isObject()) return 0;
    return root.get("totalRecords", 0).asInt();
}

std::vector<std::string>
SonarrParsers::parse_history_download_ids(const std::string& json) {
    std::vector<std::string> out;
    Json::Value root;
    if (!parse_json(json, root)) return out;
    const Json::Value* records = records_of(root);
    if (!records) return out;
    out.reserve(4);
    for (const auto& r : *records) {
        if (!r.isObject()) continue;
        std::string id = to_lower(r.get("downloadId", "").asString());
        if (id.empty()) continue;
        if (std::find(out.begin(), out.end(), id) == out.end()) {
            out.push_back(std::move(id));
        }
    }
    return out;
}

std::vector<QualityProfile> SonarrParsers::parse_quality_profiles(const std::string& json) {
    return RadarrParsers::parse_quality_profiles(json);
}

std::vector<RootFolder> SonarrParsers::parse_root_folders(const std::string& json) {
    return RadarrParsers::parse_root_folders(json);
}

}  // namespace media_browser
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: `All tests passed (… assertions in 198 test cases)` — 186 + 12 new.

- [ ] **Step 7: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/sonarr/ \
  magic_dingus_box_cpp/tests/media_browser/test_sonarr_parsers.cpp \
  magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr/ \
  magic_dingus_box_cpp/CMakeLists.txt
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): Sonarr types + fixture-driven parsers

Series/Season/Episode mirror the radarr_types conventions and reuse the
Servarr-identical QualityProfile/RootFolder rather than forking them. The
queue parser keeps one record per episode with the shared downloadId intact
— grouping belongs to the UI — and accepts both sizeleft and the staged
sizeLeft rename.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: `SonarrClient` core (HTTP, lookup, library, profiles, paths) + `SonarrMockClient`

**Files:**
- Create: `src/media_browser/sonarr/sonarr_client.h`, `src/media_browser/sonarr/sonarr_client.cpp`
- Create: `src/media_browser/sonarr/sonarr_mock.h`, `src/media_browser/sonarr/sonarr_mock.cpp`
- Create: `tests/media_browser/test_sonarr_client.cpp`
- Modify: `CMakeLists.txt` (`MEDIA_BROWSER_SOURCES` += `sonarr_client.cpp`, `sonarr_mock.cpp`; `MEDIA_BROWSER_TEST_SOURCES` += `test_sonarr_client.cpp`)

**Interfaces:**
- Consumes: everything from Task 4 (`Series`, `SeriesSearchHit`, `Season`, `Episode`, `SonarrQueueItem`, `SonarrParsers::*`); `RadarrClient::normalize_prefix` (delegated to).
- Produces:
  - `class media_browser::SonarrClient` with nested `struct Config { std::string base_url = "http://localhost:8989"; std::string api_key; int timeout_secs = 5; int queue_page_size = 100; int add_settle_timeout_ms = 8000; int add_settle_poll_ms = 500; std::string container_library_prefix = "/data/library/tv/"; std::string host_library_prefix = "/mnt/ssd/library/tv/"; };`
  - Public virtuals implemented **in this task**: `bool is_reachable();`, `std::optional<SystemStatus> get_status();`, `std::vector<SeriesSearchHit> lookup_by_tmdb(int tmdb_id, const std::string& title_fallback = "");`, `std::vector<SeriesSearchHit> lookup(const std::string& query);`, `std::optional<std::vector<Series>> get_library_checked();`, `std::vector<Series> get_library();`, `std::optional<Series> get_series(int sonarr_id);`, `std::optional<std::vector<Series>> find_series_by_tvdb(int tvdb_id);`, `std::vector<QualityProfile> get_quality_profiles();`, `std::vector<RootFolder> get_root_folders();`
  - Non-virtual: `std::string resolve_host_path(const std::string&) const;`, `static std::string normalize_prefix(std::string);`, `std::string last_error() const;`
  - Static URL builders: `static std::string build_lookup_path_tmdb(int tmdb_id);`, `static std::string build_lookup_path_term(const std::string& term);`
  - Protected virtuals for stubbing: `std::string http_get(const std::string& path);`, `std::string http_post(const std::string& path, const std::string& body);`, `std::string http_put(const std::string& path, const std::string& body);`, `std::string http_delete(const std::string& path);`
  - `class media_browser::SonarrMockClient : public SonarrClient` overriding every public virtual declared so far.
- **Later tasks add to this class:** Task 6 adds `add_series`, `set_season_monitored`, `trigger_season_search`, `trigger_series_search`, `remove_series`; Task 7 adds `get_queue`, `cancel_queue_item`, `get_series_download_hashes`. Do **not** declare those virtuals now — an undefined virtual makes the vtable fail to link.

---

- [ ] **Step 1: Write the failing tests AND register them with CMake**

Registration happens in this step, not after the red run. `MEDIA_BROWSER_TEST_SOURCES` is an explicit enumerated list, not a glob — a test `.cpp` that is not named there is never compiled, so writing the test alone produces a build that *succeeds* and silently ignores the new file. That is not a red state, it is an invisible one.

Create `tests/media_browser/test_sonarr_client.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_mock.h"

namespace fs = std::filesystem;
namespace mb = media_browser;

static std::string read_fixture(const std::string& name) {
    fs::path p = fs::path(__FILE__).parent_path() / "fixtures" / "sonarr" / name;
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// --- URL builders --------------------------------------------------------

TEST_CASE("build_lookup_path_tmdb keeps the tmdb: prefix literal",
          "[sonarr][url]") {
    // Live-proven on the box: GET /api/v3/series/lookup?term=tmdb:1396 →
    // Breaking Bad with tvdbId 81189. Sonarr's SkyHook proxy special-cases the
    // literal "tmdb:" prefix, so the colon must NOT be percent-encoded.
    CHECK(mb::SonarrClient::build_lookup_path_tmdb(1396) ==
          "/api/v3/series/lookup?term=tmdb:1396");
}

TEST_CASE("build_lookup_path_term percent-encodes the title", "[sonarr][url]") {
    CHECK(mb::SonarrClient::build_lookup_path_term("Breaking Bad") ==
          "/api/v3/series/lookup?term=Breaking%20Bad");
    // RFC 3986 unreserved characters pass through; everything else is %HH.
    CHECK(mb::SonarrClient::build_lookup_path_term("Marvel's Agents of S.H.I.E.L.D.") ==
          "/api/v3/series/lookup?term=Marvel%27s%20Agents%20of%20S.H.I.E.L.D.");
}

// --- Path translation ----------------------------------------------------

TEST_CASE("resolve_host_path: default config maps Sonarr's /data/library/tv",
          "[sonarr][paths]") {
    mb::SonarrClient c{mb::SonarrClient::Config{}};  // all defaults
    CHECK(c.resolve_host_path("/data/library/tv/Breaking Bad/S01E01.mkv") ==
          "/mnt/ssd/library/tv/Breaking Bad/S01E01.mkv");
}

TEST_CASE("resolve_host_path: rejects a /tv2 false prefix", "[sonarr][paths]") {
    mb::SonarrClient c{mb::SonarrClient::Config{}};
    CHECK(c.resolve_host_path("/data/library/tv2/foo.mkv") ==
          "/data/library/tv2/foo.mkv");
}

TEST_CASE("resolve_host_path: normalizes prefixes without a trailing slash",
          "[sonarr][paths]") {
    mb::SonarrClient::Config cfg;
    cfg.container_library_prefix = "/data/library/tv";      // no trailing slash
    cfg.host_library_prefix      = "/mnt/ssd/library/tv";   // no trailing slash
    mb::SonarrClient c(cfg);
    CHECK(c.resolve_host_path("/data/library/tv/Show/ep.mkv") ==
          "/mnt/ssd/library/tv/Show/ep.mkv");
    CHECK(c.resolve_host_path("/data/library/tv2/foo.mkv") ==
          "/data/library/tv2/foo.mkv");
}

TEST_CASE("resolve_host_path: passes through unrecognized and empty paths",
          "[sonarr][paths]") {
    mb::SonarrClient c{mb::SonarrClient::Config{}};
    CHECK(c.resolve_host_path("/data/library/Movies/x.mkv") ==
          "/data/library/Movies/x.mkv");
    CHECK(c.resolve_host_path("").empty());
}

// --- Stub harness --------------------------------------------------------

namespace {
// Records every path the client hits and replies from a canned table. Reused
// by later tasks' tests.
class StubSonarr : public mb::SonarrClient {
public:
    StubSonarr() : SonarrClient(Config{}) {}
    std::vector<std::string> gets;
    // path prefix -> response body. First matching prefix wins.
    std::vector<std::pair<std::string, std::string>> get_replies;

    std::string http_get(const std::string& path) override {
        gets.push_back(path);
        for (const auto& kv : get_replies) {
            if (path.rfind(kv.first, 0) == 0) return kv.second;
        }
        return "";
    }
    std::string http_post(const std::string&, const std::string&) override { return ""; }
    std::string http_put(const std::string&, const std::string&) override { return ""; }
    std::string http_delete(const std::string&) override { return ""; }
};
}  // namespace

TEST_CASE("lookup_by_tmdb hits the tmdb: path and parses the result",
          "[sonarr][lookup]") {
    StubSonarr s;
    s.get_replies = {{"/api/v3/series/lookup?term=tmdb:1396",
                      read_fixture("series_lookup.json")}};
    auto hits = s.lookup_by_tmdb(1396);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].tvdb_id == 81189);
    REQUIRE(s.gets.size() == 1);
    CHECK(s.gets[0] == "/api/v3/series/lookup?term=tmdb:1396");
}

TEST_CASE("lookup_by_tmdb falls back to a title search on an empty result",
          "[sonarr][lookup]") {
    // Some shows have no TMDB->TVDB mapping in SkyHook; the tmdb: lookup then
    // returns []. Without the fallback the kiosk could never add them.
    StubSonarr s;
    s.get_replies = {
        {"/api/v3/series/lookup?term=tmdb:", "[]"},
        {"/api/v3/series/lookup?term=Breaking%20Bad",
         read_fixture("series_lookup.json")},
    };
    auto hits = s.lookup_by_tmdb(1396, "Breaking Bad");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].title == "Breaking Bad");
    REQUIRE(s.gets.size() == 2);
    CHECK(s.gets[0] == "/api/v3/series/lookup?term=tmdb:1396");
    CHECK(s.gets[1] == "/api/v3/series/lookup?term=Breaking%20Bad");
}

TEST_CASE("lookup_by_tmdb does not fall back without a title",
          "[sonarr][lookup]") {
    StubSonarr s;
    s.get_replies = {{"/api/v3/series/lookup?term=tmdb:", "[]"}};
    CHECK(s.lookup_by_tmdb(1396).empty());
    CHECK(s.gets.size() == 1);
}

TEST_CASE("get_library_checked distinguishes empty from failed",
          "[sonarr][library]") {
    // The Radarr equivalent shipped as a bug fix: a bare vector made "empty
    // library" and "GET failed" indistinguishable, which broke For You.
    // Start with the checked shape rather than retrofit it.
    SECTION("HTTP failure → nullopt") {
        StubSonarr s;  // no replies configured → http_get returns ""
        CHECK_FALSE(s.get_library_checked().has_value());
        CHECK(s.get_library().empty());
    }
    SECTION("genuinely empty library → engaged optional, empty vector") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/series", "[]"}};
        auto lib = s.get_library_checked();
        REQUIRE(lib.has_value());
        CHECK(lib->empty());
    }
    SECTION("populated library") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/series", read_fixture("series_list.json")}};
        auto lib = s.get_library_checked();
        REQUIRE(lib.has_value());
        REQUIRE(lib->size() == 1);
        CHECK((*lib)[0].sonarr_id == 7);
    }
}

TEST_CASE("get_series and find_series_by_tvdb use the right paths",
          "[sonarr][library]") {
    StubSonarr s;
    s.get_replies = {
        {"/api/v3/series?tvdbId=81189", read_fixture("series_list.json")},
        {"/api/v3/series/7", read_fixture("series_added.json")},
    };
    auto by_tvdb = s.find_series_by_tvdb(81189);
    REQUIRE(by_tvdb.has_value());
    REQUIRE(by_tvdb->size() == 1);
    CHECK((*by_tvdb)[0].sonarr_id == 7);

    auto by_id = s.get_series(7);
    REQUIRE(by_id.has_value());
    CHECK(by_id->seasons.size() == 6);

    REQUIRE(s.gets.size() == 2);
    CHECK(s.gets[0] == "/api/v3/series?tvdbId=81189");
    CHECK(s.gets[1] == "/api/v3/series/7");
}

TEST_CASE("find_series_by_tvdb separates 'not in library' from 'request failed'",
          "[sonarr][library]") {
    // This probe gates a MUTATION (add_series decides whether to POST), so
    // collapsing the two outcomes is the same class of bug get_library_checked
    // exists to fix — and Sonarr rides Gluetun's netns, so transport blips are
    // routine rather than theoretical.
    SECTION("server answered: not in the library") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/series?tvdbId=", "[]"}};
        auto r = s.find_series_by_tvdb(81189);
        REQUIRE(r.has_value());   // the request worked...
        CHECK(r->empty());        // ...and the answer is "no"
    }
    SECTION("transport failure") {
        StubSonarr s;  // no replies configured → http_get returns ""
        CHECK_FALSE(s.find_series_by_tvdb(81189).has_value());
    }
    SECTION("already in the library") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/series?tvdbId=", read_fixture("series_list.json")}};
        auto r = s.find_series_by_tvdb(81189);
        REQUIRE(r.has_value());
        REQUIRE(r->size() == 1);
        CHECK((*r)[0].sonarr_id == 7);
    }
}

TEST_CASE("profiles and root folders use the Servarr paths", "[sonarr][config]") {
    StubSonarr s;
    s.get_replies = {
        {"/api/v3/qualityprofile", read_fixture("quality_profiles.json")},
        {"/api/v3/rootfolder", read_fixture("root_folders.json")},
    };
    auto profiles = s.get_quality_profiles();
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Any");
    auto roots = s.get_root_folders();
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].path == "/data/library/tv");
    CHECK(s.gets[0] == "/api/v3/qualityprofile");
    CHECK(s.gets[1] == "/api/v3/rootfolder");
}

// --- Mock ----------------------------------------------------------------

TEST_CASE("SonarrMockClient serves a coherent seeded library", "[sonarr][mock]") {
    mb::SonarrMockClient m;
    CHECK(m.is_reachable());
    auto lib = m.get_library_checked();
    REQUIRE(lib.has_value());
    REQUIRE(lib->size() == 1);
    CHECK((*lib)[0].sonarr_id == 1);
    CHECK((*lib)[0].tmdb_id == 1396);
    // Season 1 monitored, the rest not — the same shape a real firstSeason
    // add produces, so a mock-mode screen renders the real state machine.
    REQUIRE((*lib)[0].seasons.size() >= 2);
    CHECK_FALSE((*lib)[0].seasons[0].monitored);  // Specials
    CHECK((*lib)[0].seasons[1].monitored);        // Season 1

    auto one = m.get_series(1);
    REQUIRE(one.has_value());
    CHECK(one->title == (*lib)[0].title);

    auto profiles = m.get_quality_profiles();
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Any");
    CHECK(m.get_root_folders().front().path == "/data/library/tv");
}
```

Then register all three new files in `CMakeLists.txt` **now**. Extend the Sonarr block Task 4 created — it sits directly under the unique `set(MEDIA_BROWSER_SOURCES` opening line, which is what to anchor on. Do NOT anchor on `radarr_mock.cpp` or `prowlarr_client.cpp`: those appear in both source lists, distinguished only by indent.

```cmake
    set(MEDIA_BROWSER_SOURCES
        src/media_browser/sonarr/sonarr_parsers.cpp
        src/media_browser/sonarr/sonarr_client.cpp
        src/media_browser/sonarr/sonarr_mock.cpp
```

and inside `set(MEDIA_BROWSER_TEST_SOURCES`:

```cmake
        tests/media_browser/test_sonarr_parsers.cpp
        tests/media_browser/test_sonarr_client.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake -S "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp" \
      -B "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" \
      -DBUILD_KIOSK=OFF -DBUILD_TESTS=ON -DENABLE_MEDIA_BROWSER=ON
```
Expected: **CONFIGURE fails** with `Cannot find source file: src/media_browser/sonarr/sonarr_client.cpp` (and `sonarr_mock.cpp`). Create the two `.cpp` files as empty stubs, re-configure, and the *build* then fails with `'media_browser/sonarr/sonarr_client.h' file not found` from `test_sonarr_client.cpp`. That is the genuine red state; do not record a pass here.

- [ ] **Step 3: Write the client header**

`src/media_browser/sonarr/sonarr_client.h`:

```cpp
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <json/json.h>
#include "media_browser/sonarr/sonarr_types.h"

namespace media_browser {

// HTTP client for Sonarr v4 (which still serves its API under /api/v3 — there
// is no /api/v4 namespace). Mirrors RadarrClient: every public method is
// virtual so SonarrMockClient can replace them wholesale, and the four http_*
// helpers are protected virtuals so unit tests can stub transport without a
// network.
class SonarrClient {
public:
    struct Config {
        std::string base_url = "http://localhost:8989";
        std::string api_key;
        // Kept short (like Radarr's) so a stalled Sonarr cannot freeze the
        // render thread. The kiosk systemd unit has WatchdogSec=10; any
        // synchronous call reachable from render must fit inside it.
        int timeout_secs = 5;
        // Queue page size. Sonarr's PagingResource default is far smaller, and
        // its queue is per-EPISODE, so season packs make >100 records genuinely
        // reachable — get_queue() pages until it has them all. Overridable so
        // tests can force the multi-page path without a 100-record fixture.
        int queue_page_size = 100;
        // Budget for add_series' post-POST settle poll (see add_series).
        // 8s comfortably covers a SkyHook episode fetch; the caller gets a
        // provisional result rather than a hang if it does not.
        int add_settle_timeout_ms = 8000;
        int add_settle_poll_ms = 500;
        // Sonarr's root folder is /data/library/tv inside the container; the
        // host sees /mnt/ssd/library/tv. Both normalized to end in '/' by the
        // constructor so "/data/library/tv2/..." cannot false-match.
        std::string container_library_prefix = "/data/library/tv/";
        std::string host_library_prefix      = "/mnt/ssd/library/tv/";
    };

    explicit SonarrClient(Config config);
    virtual ~SonarrClient();

    SonarrClient(const SonarrClient&) = delete;
    SonarrClient& operator=(const SonarrClient&) = delete;

    // Service health
    virtual bool is_reachable();
    virtual std::optional<SystemStatus> get_status();

    // Series discovery.
    //
    // Resolves a TMDB id through Sonarr's own delegation path
    // (term=tmdb:<id>, which SkyHook maps to TVDB server-side) — live-proven
    // against the box, so the kiosk needs no TVDB mapping table. Some shows
    // have no mapping and come back empty; pass the TMDB title as
    // `title_fallback` to retry as a free-text search.
    virtual std::vector<SeriesSearchHit> lookup_by_tmdb(
        int tmdb_id, const std::string& title_fallback = "");
    virtual std::vector<SeriesSearchHit> lookup(const std::string& query);

    // Library. get_library_checked() is the primary shape: nullopt on HTTP
    // failure vs a possibly-empty vector on success. The bare wrapper exists
    // for callers that genuinely do not care — do NOT use it to decide
    // "library is empty", which is the bug the Radarr equivalent had to fix.
    virtual std::optional<std::vector<Series>> get_library_checked();
    virtual std::vector<Series> get_library();
    virtual std::optional<Series> get_series(int sonarr_id);
    // GET /api/v3/series?tvdbId=<id> — Sonarr filters server-side. Used to
    // detect an already-added series before POSTing (which would 400 on
    // seriesExistsValidator).
    //
    // CHECKED shape on purpose, like get_library_checked: nullopt means the
    // REQUEST FAILED, an engaged-but-empty vector means Sonarr answered "not
    // in the library". Callers must not collapse the two — this probe gates a
    // mutation, and Sonarr shares Gluetun's netns, so a tunnel blip that read
    // as "not present" would POST a duplicate and surface Sonarr's 400
    // validation text instead of the real network fault.
    virtual std::optional<std::vector<Series>> find_series_by_tvdb(int tvdb_id);

    // Profiles / storage. Resolve the quality profile BY NAME at the call
    // site ("Any" on this box, id 1 — the id is not portable).
    virtual std::vector<QualityProfile> get_quality_profiles();
    virtual std::vector<RootFolder> get_root_folders();

    // Container path -> host path. Unrecognized paths pass through unchanged
    // with a warn-level log.
    std::string resolve_host_path(const std::string& container_path) const;

    // Trailing-slash normalization, exposed so main.cpp can normalize
    // env-supplied overrides at the same boundary the constructor uses.
    static std::string normalize_prefix(std::string s);

    // URL builders, exposed for unit tests.
    static std::string build_lookup_path_tmdb(int tmdb_id);
    static std::string build_lookup_path_term(const std::string& term);

    // Returns a COPY under the error mutex — screens read this on the render
    // thread while worker threads write it (the exact data race the Radarr
    // client had to fix).
    std::string last_error() const {
        std::lock_guard<std::mutex> lk(err_mtx_);
        return last_error_;
    }

protected:
    void set_error(std::string msg) {
        std::lock_guard<std::mutex> lk(err_mtx_);
        last_error_ = std::move(msg);
    }
    // Virtual for stubbing in tests (see test_sonarr_client.cpp).
    virtual std::string http_get(const std::string& path);
    virtual std::string http_post(const std::string& path, const std::string& body);
    virtual std::string http_put(const std::string& path, const std::string& body);
    virtual std::string http_delete(const std::string& path);

    Config cfg_;

private:
    mutable std::mutex err_mtx_;
    std::string last_error_;  // guarded by err_mtx_
};

}  // namespace media_browser
```

- [ ] **Step 4: Write the client implementation**

`src/media_browser/sonarr/sonarr_client.cpp`:

```cpp
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_parsers.h"
#include "media_browser/radarr/radarr_client.h"   // normalize_prefix (one implementation)
#include "media_browser/radarr/radarr_parsers.h"  // parse_system_status

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <sstream>

namespace media_browser {

namespace {

size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// RFC 3986 percent-encoding: unreserved characters pass through, everything
// else becomes %HH. Copied in spirit from tmdb_client.cpp rather than from
// RadarrClient::lookup's minimal encoder — that one allocates a CURL handle
// per call in ProwlarrClient's variant and percent-encodes '.'/'-' needlessly.
std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3 / 2);
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z')
                              || (c >= 'a' && c <= 'z')
                              || (c >= '0' && c <= '9')
                              || c == '-' || c == '_'
                              || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

SonarrClient::SonarrClient(Config config) : cfg_(std::move(config)) {
    cfg_.container_library_prefix = normalize_prefix(cfg_.container_library_prefix);
    cfg_.host_library_prefix      = normalize_prefix(cfg_.host_library_prefix);
    // libcurl reference-counts init/cleanup pairs, and RadarrClient/TmdbClient
    // already run their own — matching them here keeps the client usable on
    // its own (the test_media_browser CLI constructs one with nothing else
    // alive).
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

SonarrClient::~SonarrClient() {
    curl_global_cleanup();
}

std::string SonarrClient::normalize_prefix(std::string s) {
    // One implementation, shared with the Radarr client.
    return RadarrClient::normalize_prefix(std::move(s));
}

std::string SonarrClient::build_lookup_path_tmdb(int tmdb_id) {
    // The "tmdb:" prefix is matched literally by Sonarr's SkyHook proxy —
    // percent-encoding the colon is unnecessary and obscures the contract.
    return "/api/v3/series/lookup?term=tmdb:" + std::to_string(tmdb_id);
}

std::string SonarrClient::build_lookup_path_term(const std::string& term) {
    return "/api/v3/series/lookup?term=" + url_encode(term);
}

// --- transport -----------------------------------------------------------

namespace {
// Shared curl setup for all four verbs.
struct CurlRequest {
    CURL* curl = nullptr;
    curl_slist* headers = nullptr;
    std::string body;
    ~CurlRequest() {
        if (headers) curl_slist_free_all(headers);
        if (curl) curl_easy_cleanup(curl);
    }
};
}  // namespace

std::string SonarrClient::http_get(const std::string& path) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    // Required off the main thread: without it libcurl's resolver uses
    // SIGALRM and crashes with a SIGSEGV inside the signal handler.
    // ProwlarrClient documents the same; RadarrClient omits it (a latent bug
    // this client deliberately does not copy).
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_post(const std::string& path, const std::string& body) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    req.headers = curl_slist_append(req.headers, "Content-Type: application/json");
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        // Include the body: Sonarr's validation failures (seriesExistsValidator,
        // bad qualityProfileId) explain themselves there and nowhere else.
        std::ostringstream os; os << "HTTP " << http_code << ": " << req.body;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_put(const std::string& path, const std::string& body) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    req.headers = curl_slist_append(req.headers, "Content-Type: application/json");
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code << ": " << req.body;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_delete(const std::string& path) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        set_error(os.str());
        return {};
    }
    return req.body;
}

// --- endpoints -----------------------------------------------------------

bool SonarrClient::is_reachable() {
    return !http_get("/ping").empty();
}

std::optional<SystemStatus> SonarrClient::get_status() {
    auto resp = http_get("/api/v3/system/status");
    if (resp.empty()) return std::nullopt;
    // Same {version, buildTime} shape Radarr serves.
    return RadarrParsers::parse_system_status(resp);
}

std::vector<SeriesSearchHit>
SonarrClient::lookup_by_tmdb(int tmdb_id, const std::string& title_fallback) {
    auto resp = http_get(build_lookup_path_tmdb(tmdb_id));
    auto hits = resp.empty() ? std::vector<SeriesSearchHit>{}
                             : SonarrParsers::parse_series_lookup(resp);
    if (!hits.empty() || title_fallback.empty()) return hits;
    // No TMDB->TVDB mapping in SkyHook for this show. Retry as free text.
    spdlog::info("[sonarr] tmdb:{} had no lookup match; falling back to "
                 "title search '{}'", tmdb_id, title_fallback);
    return lookup(title_fallback);
}

std::vector<SeriesSearchHit> SonarrClient::lookup(const std::string& query) {
    auto resp = http_get(build_lookup_path_term(query));
    if (resp.empty()) return {};
    return SonarrParsers::parse_series_lookup(resp);
}

std::optional<std::vector<Series>> SonarrClient::get_library_checked() {
    auto resp = http_get("/api/v3/series");
    if (resp.empty()) return std::nullopt;
    return SonarrParsers::parse_series_list(resp);
}

std::vector<Series> SonarrClient::get_library() {
    return get_library_checked().value_or(std::vector<Series>{});
}

std::optional<Series> SonarrClient::get_series(int sonarr_id) {
    auto resp = http_get("/api/v3/series/" + std::to_string(sonarr_id));
    if (resp.empty()) return std::nullopt;
    return SonarrParsers::parse_series(resp);
}

std::optional<std::vector<Series>> SonarrClient::find_series_by_tvdb(int tvdb_id) {
    auto resp = http_get("/api/v3/series?tvdbId=" + std::to_string(tvdb_id));
    if (resp.empty()) return std::nullopt;  // transport/HTTP failure — NOT "absent"
    return SonarrParsers::parse_series_list(resp);
}

std::vector<QualityProfile> SonarrClient::get_quality_profiles() {
    auto resp = http_get("/api/v3/qualityprofile");
    if (resp.empty()) return {};
    return SonarrParsers::parse_quality_profiles(resp);
}

std::vector<RootFolder> SonarrClient::get_root_folders() {
    auto resp = http_get("/api/v3/rootfolder");
    if (resp.empty()) return {};
    return SonarrParsers::parse_root_folders(resp);
}

std::string SonarrClient::resolve_host_path(const std::string& container_path) const {
    if (container_path.empty()) return container_path;
    if (container_path.rfind(cfg_.container_library_prefix, 0) == 0) {
        return cfg_.host_library_prefix +
               container_path.substr(cfg_.container_library_prefix.size());
    }
    // No legacy alternates: unlike the movie library, the TV subtree was
    // created after the hardlink migration, so /data/library/tv is the only
    // path Sonarr has ever recorded.
    spdlog::warn("[sonarr] resolve_host_path: '{}' does not match prefix "
                 "'{}'; passing through unchanged",
                 container_path, cfg_.container_library_prefix);
    return container_path;
}

}  // namespace media_browser
```

- [ ] **Step 5: Write the mock**

`src/media_browser/sonarr/sonarr_mock.h`:

```cpp
#pragma once

#include "media_browser/sonarr/sonarr_client.h"

namespace media_browser {

// In-memory Sonarr for dev machines with no API key and for tests. Seeded
// with one series in the exact post-firstSeason-add state (S1 monitored,
// specials and later seasons not) so mock-mode UI exercises the real state
// machine rather than an idealized one.
//
// Unlike RadarrMockClient — which leaves get_history/grab_release falling
// through to the real HTTP path against an empty config — this mock overrides
// EVERY public virtual. A dev machine must never emit a live request.
class SonarrMockClient : public SonarrClient {
public:
    SonarrMockClient();

    bool is_reachable() override;
    std::optional<SystemStatus> get_status() override;
    std::vector<SeriesSearchHit> lookup_by_tmdb(
        int tmdb_id, const std::string& title_fallback) override;
    std::vector<SeriesSearchHit> lookup(const std::string& query) override;
    std::optional<std::vector<Series>> get_library_checked() override;
    std::vector<Series> get_library() override;
    std::optional<Series> get_series(int sonarr_id) override;
    std::optional<std::vector<Series>> find_series_by_tvdb(int tvdb_id) override;
    std::vector<QualityProfile> get_quality_profiles() override;
    std::vector<RootFolder> get_root_folders() override;

protected:
    std::vector<Series> library_;
    std::vector<QualityProfile> profiles_;
    int next_id_ = 1;
};

}  // namespace media_browser
```

`src/media_browser/sonarr/sonarr_mock.cpp`:

```cpp
#include "media_browser/sonarr/sonarr_mock.h"

namespace media_browser {

SonarrMockClient::SonarrMockClient() : SonarrClient({/* empty config */}) {
    profiles_.push_back({1, "Any", 1, {}});

    Series s;
    s.sonarr_id       = next_id_++;
    s.tvdb_id         = 81189;
    s.tmdb_id         = 1396;
    s.imdb_id         = "tt0903747";
    s.title           = "Breaking Bad";
    s.overview        = "Mock series. Use a real SonarrClient for actual data.";
    s.year            = 2008;
    s.runtime_minutes = 47;
    s.status          = "ended";
    s.monitored       = true;
    s.path            = "/data/library/tv/Breaking Bad";
    s.added_at        = "2026-08-01T09:00:00Z";
    s.episode_file_count = 7;
    s.size_on_disk_bytes = 8589934592LL;
    // Specials + 5 seasons; only season 1 monitored and downloaded — the
    // shape addOptions.monitor="firstSeason" actually persists.
    s.seasons.push_back({0, false, 5, 0, 0});
    s.seasons.push_back({1, true, 7, 7, 8589934592LL});
    s.seasons.push_back({2, false, 13, 0, 0});
    s.seasons.push_back({3, false, 13, 0, 0});
    s.seasons.push_back({4, false, 13, 0, 0});
    s.seasons.push_back({5, false, 16, 0, 0});
    library_.push_back(std::move(s));
}

bool SonarrMockClient::is_reachable() { return true; }

std::optional<SystemStatus> SonarrMockClient::get_status() {
    SystemStatus st;
    st.version = "mock-4.0.19.2979";
    st.startup_completed = true;
    return st;
}

std::vector<SeriesSearchHit>
SonarrMockClient::lookup_by_tmdb(int tmdb_id, const std::string& title_fallback) {
    SeriesSearchHit h;
    h.tmdb_id = tmdb_id;
    h.tvdb_id = 81189;
    h.title = title_fallback.empty()
                ? ("Mock Series " + std::to_string(tmdb_id))
                : title_fallback;
    h.year = 2008;
    h.runtime_minutes = 47;
    h.status = "ended";
    h.overview = "Mock result. Use a real SonarrClient for actual data.";
    // Lookup results arrive with every season monitored — mirror that so a
    // caller cannot accidentally depend on the mock being tidier than Sonarr.
    for (int n = 0; n <= 5; ++n) h.seasons.push_back({n, true, 0, 0, 0});
    return {h};
}

std::vector<SeriesSearchHit> SonarrMockClient::lookup(const std::string& query) {
    return lookup_by_tmdb(0, query);
}

std::optional<std::vector<Series>> SonarrMockClient::get_library_checked() {
    return library_;
}

std::vector<Series> SonarrMockClient::get_library() { return library_; }

std::optional<Series> SonarrMockClient::get_series(int sonarr_id) {
    for (const auto& s : library_) if (s.sonarr_id == sonarr_id) return s;
    return std::nullopt;
}

std::optional<std::vector<Series>> SonarrMockClient::find_series_by_tvdb(int tvdb_id) {
    // Always "the request worked" — the mock has no transport to fail.
    std::vector<Series> out;
    for (const auto& s : library_) if (s.tvdb_id == tvdb_id) out.push_back(s);
    return out;
}

std::vector<QualityProfile> SonarrMockClient::get_quality_profiles() {
    return profiles_;
}

std::vector<RootFolder> SonarrMockClient::get_root_folders() {
    RootFolder rf;
    rf.id = 1;
    rf.path = "/data/library/tv";
    rf.free_space_bytes = 500'000'000'000LL;
    return {rf};
}

}  // namespace media_browser
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: `All tests passed (… assertions in 212 test cases)` — 198 + 14 new.

- [ ] **Step 7: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/sonarr/ \
  magic_dingus_box_cpp/tests/media_browser/test_sonarr_client.cpp \
  magic_dingus_box_cpp/CMakeLists.txt
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): SonarrClient core + mock

Lookup resolves TMDB ids through Sonarr's own tmdb: delegation with a
title-search fallback for shows SkyHook cannot map. get_library_checked
ships with the success signal rather than retrofitting it. The http_*
helpers set CURLOPT_NOSIGNAL, which the Radarr helpers omit.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: `add_series` (async-settle poll), season monitoring, search commands, delete

**Files:**
- Modify: `src/media_browser/sonarr/sonarr_client.h` / `.cpp`
- Modify: `src/media_browser/sonarr/sonarr_mock.h` / `.cpp`
- Modify: `tests/media_browser/test_sonarr_client.cpp`

**Interfaces:**
- Consumes: Task 5's `SonarrClient` (`http_get/http_post/http_put/http_delete`, `get_root_folders`, `get_series`, `find_series_by_tvdb`, `build_lookup_path_tmdb/term`, `Config::add_settle_timeout_ms` / `add_settle_poll_ms`).
- Produces (new public virtuals on `SonarrClient`, all overridden in `SonarrMockClient`):
  - `struct media_browser::AddSeriesResult { bool ok = false; bool settled = false; Series series; };`
  - `virtual AddSeriesResult add_series(int tmdb_id, int quality_profile_id, bool monitor = true, const std::string& title_fallback = "");`
  - `virtual bool set_season_monitored(int sonarr_id, int season_number, bool monitored);`
  - `virtual bool trigger_season_search(int sonarr_id, int season_number);`
  - `virtual bool trigger_series_search(int sonarr_id);`
  - `virtual bool remove_series(int sonarr_id, bool delete_files = false);`

---

- [ ] **Step 1: Write the failing tests**

Append to `tests/media_browser/test_sonarr_client.cpp`:

```cpp
// --- add_series ----------------------------------------------------------

namespace {
// Reproduces the live box's ACTUAL behaviour. Sonarr's POST returns the
// STORED resource (RestController.Created serializes GetResourceById), but
// addOptions is applied ASYNCHRONOUSLY — AddSeriesService persists, publishes
// SeriesAddedEvent, which queues a RefreshSeriesCommand; only once
// RefreshSeriesService has pulled episodes from SkyHook does
// EpisodeMonitoredService apply the monitor enum and null addOptions out.
//
// So both the POST response AND any immediate GET show every season
// monitored:true. This stub models exactly that: the POST and the first GET
// return the pending fixture, later GETs return the settled one.
class AddSonarr : public mb::SonarrClient {
public:
    static Config fast_settle() {
        Config c;
        // Tight budget: the settle path needs only two polls, and the
        // never-settles path must not spin for long.
        c.add_settle_timeout_ms = 50;
        c.add_settle_poll_ms = 0;   // never actually sleep in the suite
        return c;
    }
    AddSonarr() : SonarrClient(fast_settle()) {}
    std::vector<std::string> gets;
    std::string post_path, post_body;
    bool already_added = false;
    bool never_settles = false;   // simulate a refresh that never completes
    bool probe_fails = false;     // simulate a Gluetun blip on the tvdbId probe
    int series_gets = 0;

    std::string http_get(const std::string& path) override {
        gets.push_back(path);
        if (path.rfind("/api/v3/series/lookup?term=tmdb:1396", 0) == 0) {
            return read_fixture("series_lookup.json");
        }
        if (path.rfind("/api/v3/series?tvdbId=81189", 0) == 0) {
            if (probe_fails) return "";  // transport failure, NOT "absent"
            return already_added ? read_fixture("series_list.json") : "[]";
        }
        if (path.rfind("/api/v3/rootfolder", 0) == 0) {
            return read_fixture("root_folders.json");
        }
        if (path.rfind("/api/v3/series/7", 0) == 0) {
            // First read races the refresh; later reads see it landed.
            ++series_gets;
            if (never_settles || series_gets == 1) {
                return read_fixture("series_added_pending.json");
            }
            return read_fixture("series_added.json");
        }
        return "";
    }
    std::string http_post(const std::string& path, const std::string& body) override {
        post_path = path;
        post_body = body;
        // The stored resource as it exists the instant after the insert:
        // id assigned, addOptions still populated, seasons untouched.
        return read_fixture("series_added_pending.json");
    }
    std::string http_put(const std::string&, const std::string&) override { return ""; }
    std::string http_delete(const std::string&) override { return ""; }
};
}  // namespace

TEST_CASE("add_series polls until Sonarr's async refresh settles",
          "[sonarr][add]") {
    // THE load-bearing test of this phase. A single read — of the POST body OR
    // of an immediate GET — returns every season monitored:true, because the
    // monitor enum has not been applied yet. A client that trusted it would
    // tell the UI the whole series is monitored, and "download next season"
    // would target the wrong season forever.
    AddSonarr s;
    auto added = s.add_series(1396, /*quality_profile_id=*/1, /*monitor=*/true);
    REQUIRE(added.ok);
    REQUIRE(added.settled);           // the poll saw the refresh land
    CHECK(added.series.sonarr_id == 7);
    REQUIRE(added.series.seasons.size() == 6);
    CHECK_FALSE(added.series.seasons[0].monitored);  // Specials
    CHECK(added.series.seasons[1].monitored);        // Season 1 — the only one
    CHECK_FALSE(added.series.seasons[2].monitored);
    CHECK_FALSE(added.series.seasons[3].monitored);
    CHECK_FALSE(added.series.seasons[4].monitored);
    CHECK_FALSE(added.series.seasons[5].monitored);

    // Proof it POLLED rather than reading once: the first GET returned the
    // pending state and was correctly rejected.
    CHECK(s.series_gets >= 2);
}

TEST_CASE("add_series returns a PROVISIONAL result when the refresh never lands",
          "[sonarr][add]") {
    // Bounded, not unbounded: a wedged SkyHook fetch must not hang the worker.
    // The caller still gets the series (the add DID happen) but settled=false
    // tells Phase 2c to re-fetch instead of caching an all-monitored list.
    AddSonarr s;
    s.never_settles = true;
    auto added = s.add_series(1396, 1, true);
    CHECK(added.ok);                 // the add succeeded
    CHECK_FALSE(added.settled);      // ...but the season flags are not trustworthy
    CHECK(added.series.sonarr_id == 7);
}

TEST_CASE("add_series POSTs a valid Sonarr payload", "[sonarr][add]") {
    AddSonarr s;
    REQUIRE(s.add_series(1396, 1, true).ok);
    CHECK(s.post_path == "/api/v3/series");
    CHECK(s.post_body.find(R"("qualityProfileId":1)") != std::string::npos);
    CHECK(s.post_body.find(R"("rootFolderPath":"/data/library/tv")") != std::string::npos);
    CHECK(s.post_body.find(R"("monitor":"firstSeason")") != std::string::npos);
    CHECK(s.post_body.find(R"("searchForMissingEpisodes":true)") != std::string::npos);
    CHECK(s.post_body.find(R"("seasonFolder":true)") != std::string::npos);
    // minimumAvailability is a Radarr-only concept; sending it to Sonarr is
    // meaningless noise at best.
    CHECK(s.post_body.find("minimumAvailability") == std::string::npos);
}

TEST_CASE("add_series with monitor=false sends monitor:none, not firstSeason",
          "[sonarr][add]") {
    // Sonarr honours addOptions.monitor INDEPENDENTLY of series.monitored. An
    // unmonitored add that still said "firstSeason" would leave a fully
    // monitored season 1 underneath, and the moment anything flips
    // series.monitored true — a user toggle, a 2c "resume", a seasonpass bulk
    // edit — Sonarr grabs the whole season with nobody having asked for it.
    AddSonarr s;
    REQUIRE(s.add_series(1396, 1, /*monitor=*/false).ok);
    CHECK(s.post_body.find(R"("monitor":"none")") != std::string::npos);
    CHECK(s.post_body.find("firstSeason") == std::string::npos);
    CHECK(s.post_body.find(R"("searchForMissingEpisodes":false)") != std::string::npos);
}

TEST_CASE("add_series is idempotent when the series is already in the library",
          "[sonarr][add]") {
    // POSTing an already-added tvdbId 400s on seriesExistsValidator. Detect it
    // first and return the existing record instead of surfacing an error.
    AddSonarr s;
    s.already_added = true;
    auto added = s.add_series(1396, 1, true);
    REQUIRE(added.ok);
    CHECK(added.settled);  // an existing library record is settled by definition
    CHECK(added.series.sonarr_id == 7);
    CHECK(s.post_path.empty());  // nothing was POSTed
}

TEST_CASE("add_series ABORTS when the existence probe cannot reach Sonarr",
          "[sonarr][add]") {
    // Sonarr shares Gluetun's netns, so the probe failing mid-add is routine.
    // Treating that as "not in the library" would POST a duplicate and hand the
    // user Sonarr's 400 validation text instead of the real network fault.
    AddSonarr s;
    s.probe_fails = true;
    auto added = s.add_series(1396, 1, true);
    CHECK_FALSE(added.ok);
    CHECK(s.post_path.empty());  // crucially: no POST was issued
    CHECK_FALSE(s.last_error().empty());
}

TEST_CASE("add_series fails cleanly when the lookup finds nothing",
          "[sonarr][add]") {
    class EmptyLookup : public mb::SonarrClient {
    public:
        EmptyLookup() : SonarrClient(Config{}) {}
        std::string http_get(const std::string& path) override {
            if (path.rfind("/api/v3/series/lookup", 0) == 0) return "[]";
            return "";
        }
        std::string http_post(const std::string&, const std::string&) override {
            FAIL("add_series must not POST without a lookup result");
            return "";
        }
        std::string http_put(const std::string&, const std::string&) override { return ""; }
        std::string http_delete(const std::string&) override { return ""; }
    };
    EmptyLookup s;
    CHECK_FALSE(s.add_series(1396, 1, true).ok);
    CHECK_FALSE(s.last_error().empty());
}

// --- season monitoring ---------------------------------------------------

namespace {
class PutSonarr : public mb::SonarrClient {
public:
    PutSonarr() : SonarrClient(Config{}) {}
    std::string put_path, put_body, post_path, post_body, delete_path;
    std::string http_get(const std::string& path) override {
        if (path.rfind("/api/v3/series/7", 0) == 0) {
            return read_fixture("series_added.json");
        }
        return "";
    }
    std::string http_put(const std::string& path, const std::string& body) override {
        put_path = path;
        put_body = body;
        return body;  // Sonarr returns the updated resource
    }
    std::string http_post(const std::string& path, const std::string& body) override {
        post_path = path;
        post_body = body;
        return R"({"id":1,"name":"SeasonSearch","status":"queued"})";
    }
    std::string http_delete(const std::string& path) override {
        delete_path = path;
        return "{}";
    }
};
}  // namespace

TEST_CASE("set_season_monitored flips exactly one season and PUTs the whole "
          "resource", "[sonarr][seasons]") {
    // Sonarr's PUT /api/v3/series/{id} replaces the resource — sending a
    // partial object silently wipes the fields left out.
    PutSonarr s;
    REQUIRE(s.set_season_monitored(7, 2, true));
    CHECK(s.put_path == "/api/v3/series/7");

    Json::Value sent;
    {
        Json::CharReaderBuilder rb;
        std::string err;
        std::istringstream is(s.put_body);
        REQUIRE(Json::parseFromStream(rb, is, &sent, &err));
    }
    // Non-season fields survive the round-trip.
    CHECK(sent["id"].asInt() == 7);
    CHECK(sent["path"].asString() == "/data/library/tv/Breaking Bad");
    CHECK(sent["qualityProfileId"].asInt() == 1);
    REQUIRE(sent["seasons"].isArray());
    REQUIRE(sent["seasons"].size() == 6);
    for (const auto& season : sent["seasons"]) {
        const int n = season["seasonNumber"].asInt();
        const bool mon = season["monitored"].asBool();
        if (n == 1 || n == 2) CHECK(mon);   // 1 was already on, 2 just flipped
        else                  CHECK_FALSE(mon);
    }
}

TEST_CASE("set_season_monitored reports failure for an unknown season",
          "[sonarr][seasons]") {
    PutSonarr s;
    CHECK_FALSE(s.set_season_monitored(7, 99, true));
    CHECK(s.put_path.empty());  // nothing sent
    CHECK_FALSE(s.last_error().empty());
}

// --- commands + delete ---------------------------------------------------

TEST_CASE("trigger_season_search posts the SeasonSearch command",
          "[sonarr][commands]") {
    PutSonarr s;
    REQUIRE(s.trigger_season_search(7, 2));
    CHECK(s.post_path == "/api/v3/command");
    CHECK(s.post_body == R"({"name":"SeasonSearch","seriesId":7,"seasonNumber":2})");
}

TEST_CASE("trigger_series_search posts the SeriesSearch command",
          "[sonarr][commands]") {
    PutSonarr s;
    REQUIRE(s.trigger_series_search(7));
    CHECK(s.post_path == "/api/v3/command");
    CHECK(s.post_body == R"({"name":"SeriesSearch","seriesId":7})");
}

TEST_CASE("remove_series deletes with files and no import-list exclusion",
          "[sonarr][remove]") {
    PutSonarr s;
    REQUIRE(s.remove_series(7, /*delete_files=*/true));
    // addImportListExclusion=false: excluding it would make Sonarr refuse to
    // ever re-add the show, which is not what "remove from my box" means.
    CHECK(s.delete_path ==
          "/api/v3/series/7?deleteFiles=true&addImportListExclusion=false");
}
```

Add `#include <json/json.h>` to the top of `tests/media_browser/test_sonarr_client.cpp` (the season test parses the PUT body).

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8
```
Expected: **compile FAILS** — `no type named 'AddSeriesResult' in namespace 'media_browser'` and `no member named 'add_series' in 'media_browser::SonarrClient'` (and the same for `set_season_monitored`, `trigger_season_search`, `trigger_series_search`, `remove_series`).

- [ ] **Step 3: Declare the five methods**

In `src/media_browser/sonarr/sonarr_client.h`, add this result type just above the `class SonarrClient` declaration:

```cpp
// Outcome of add_series. `settled` is the part callers must not ignore: see
// SonarrClient::add_series for why a "successful" add can still hand back a
// season list that does not reflect the requested monitoring.
struct AddSeriesResult {
    bool ok = false;       // the series is in Sonarr and `series` identifies it
    bool settled = false;  // Sonarr's async refresh finished; seasons[] are authoritative
    Series series;         // when !settled, every season may still read monitored:true
};
```

and after `find_series_by_tvdb`, add:

```cpp
    // Library management.
    //
    // Adds a series with addOptions.monitor = "firstSeason" (the spec's
    // season-at-a-time default) when `monitor` is true, or "none" when it is
    // false — NEVER "firstSeason" for an unmonitored add, because Sonarr
    // honours addOptions.monitor independently of series.monitored and would
    // leave a fully monitored season 1 armed underneath.
    //
    // *** Sonarr applies addOptions ASYNCHRONOUSLY, AND NEVER SERIALIZES IT
    // BACK ON READ. *** The POST returns the STORED resource
    // (RestController.Created serializes GetResourceById), but at that
    // moment AddSeriesService has only inserted the row and published
    // SeriesAddedEvent, which queues a RefreshSeriesCommand. Not until
    // RefreshSeriesService has fetched the episode list from SkyHook does
    // EpisodeMonitoredService apply the monitor enum. A controller live-probe
    // (2026-08-01: added Breaking Bad, polled GET /series/{id} 40x) found
    // addOptions ABSENT from the POST response and every GET — it is
    // write-only, not "populated then nulled" as originally assumed — and
    // found statistics.totalEpisodeCount can populate BEFORE the monitor
    // enum is applied, so neither "addOptions disappeared" nor "episodes
    // exist" is a safe settle signal on its own. The predicate that cannot
    // false-positive is checking the REQUESTED OUTCOME directly: episodes
    // exist AND the monitored-season set matches what was asked for (see
    // add_settled() / record_refreshed() in sonarr_client.cpp). Until
    // settled, BOTH the POST response and any GET can show every season
    // monitored:true — so a single immediate re-GET is no better than
    // trusting the POST body.
    //
    // This method therefore BOUNDED-POLLS GET /api/v3/series/{id} against
    // that outcome predicate, capped by Config::add_settle_timeout_ms. On
    // timeout it returns ok=true, settled=false with seasons[] CLEARED
    // (never the pending/mid-refresh snapshot) — the add really did happen —
    // and the caller MUST re-fetch rather than treat the empty list as "this
    // show has 0 seasons".
    //
    // *** WORKER THREAD ONLY. *** This sleeps between polls. cfg_.timeout_secs
    // is 5 and the kiosk unit's WatchdogSec is 10; calling it from the render
    // thread risks a watchdog kill.
    //
    // Idempotent: when the tvdbId is already in the library the existing record
    // is returned (settled) instead of POSTing, which would 400 on
    // seriesExistsValidator. ok=false on any failure; see last_error().
    virtual AddSeriesResult add_series(int tmdb_id,
                                       int quality_profile_id,
                                       bool monitor = true,
                                       const std::string& title_fallback = "");

    // Flips one season's monitored flag. Sonarr's PUT replaces the whole
    // resource, so this GETs the current record, edits one season, and PUTs
    // it back untouched otherwise. false when the series or season is not
    // found, or the PUT failed.
    virtual bool set_season_monitored(int sonarr_id, int season_number, bool monitored);

    // POST /api/v3/command. Command names are the C# class name minus
    // "Command". Always pass a seriesId — MissingEpisodeSearch without one
    // sweeps the entire library.
    virtual bool trigger_season_search(int sonarr_id, int season_number);
    virtual bool trigger_series_search(int sonarr_id);

    // DELETE /api/v3/series/{id}. Never sets addImportListExclusion — the
    // user is deleting a download, not blacklisting the show.
    virtual bool remove_series(int sonarr_id, bool delete_files = false);
```

- [ ] **Step 4: Implement them**

In `src/media_browser/sonarr/sonarr_client.cpp`, add `#include <chrono>` and `#include <thread>` at the top (the settle poll needs both; `<json/json.h>` already arrives via `sonarr_client.h`), then add after `find_series_by_tvdb`:

```cpp
AddSeriesResult SonarrClient::add_series(int tmdb_id,
                                         int quality_profile_id,
                                         bool monitor,
                                         const std::string& title_fallback) {
    AddSeriesResult result;
    set_error({});

    // Sonarr requires the full series resource on POST (title, tvdbId, images,
    // seasons, …), so the flow is lookup-then-mutate — same pattern as
    // RadarrClient::add_movie. Parse the RAW lookup body rather than going
    // through SonarrParsers: the POST needs every field, not just the ones
    // SeriesSearchHit models.
    std::string lookup_resp = http_get(build_lookup_path_tmdb(tmdb_id));
    Json::Value root;
    auto parse_into_root = [&root](const std::string& text) {
        Json::CharReaderBuilder rb;
        Json::Value parsed;
        std::string err;
        std::istringstream is(text);
        if (!Json::parseFromStream(rb, is, &parsed, &err)) return false;
        root = std::move(parsed);
        return true;
    };
    bool have = !lookup_resp.empty() && parse_into_root(lookup_resp)
                && ((root.isArray() && root.size() > 0) || root.isObject());
    if (!have && !title_fallback.empty()) {
        // No TMDB->TVDB mapping in SkyHook; retry as free text.
        spdlog::info("[sonarr] add_series: tmdb:{} unmapped, retrying title '{}'",
                     tmdb_id, title_fallback);
        lookup_resp = http_get(build_lookup_path_term(title_fallback));
        have = !lookup_resp.empty() && parse_into_root(lookup_resp)
               && ((root.isArray() && root.size() > 0) || root.isObject());
    }
    if (!have) {
        set_error("Sonarr lookup returned no results for tmdb:"
                  + std::to_string(tmdb_id));
        spdlog::error("[sonarr] add_series: {}", last_error());
        return result;  // ok=false
    }

    Json::Value series = root.isArray() ? root[0u] : root;

    // Already in the library? POSTing would 400 on seriesExistsValidator.
    //
    // The CHECKED probe matters here: nullopt means the request itself failed
    // (Sonarr rides Gluetun's netns, so this is a routine transport blip), and
    // treating that as "not present" would POST a duplicate and replace the
    // real network error with Sonarr's validation text. Abort instead.
    const int tvdb_id = series.get("tvdbId", 0).asInt();
    if (tvdb_id > 0) {
        auto probe = find_series_by_tvdb(tvdb_id);
        if (!probe) {
            set_error("Could not reach Sonarr to check whether tvdb:"
                      + std::to_string(tvdb_id) + " is already in the library");
            spdlog::error("[sonarr] add_series: {}", last_error());
            return result;  // ok=false — deliberately NO POST
        }
        if (!probe->empty()) {
            result.ok = true;
            result.settled = true;  // an existing library record is settled
            result.series = probe->front();
            spdlog::info("[sonarr] add_series: tvdb:{} already in library "
                         "(id={}); returning existing record",
                         tvdb_id, result.series.sonarr_id);
            return result;
        }
    }

    auto roots = get_root_folders();
    if (roots.empty()) {
        set_error("No root folder configured in Sonarr");
        spdlog::error("[sonarr] add_series: {}", last_error());
        return result;  // ok=false
    }

    series["qualityProfileId"] = quality_profile_id;
    series["rootFolderPath"]   = roots.front().path;
    series["monitored"]        = monitor;
    series["seasonFolder"]     = true;
    // NO minimumAvailability — that field does not exist on Sonarr's
    // SeriesResource (it is a Radarr concept).
    Json::Value addOptions;
    // Derive the enum from the caller's intent — never hardcode "firstSeason".
    // "firstSeason" monitors the first regular season and unmonitors everything
    // else INCLUDING specials (the spec's season-at-a-time default). "none"
    // unmonitors everything unconditionally, which is the only correct value
    // for an unmonitored add: Sonarr applies addOptions.monitor independently
    // of series.monitored, so sending "firstSeason" here would leave season 1
    // armed and it would start grabbing the moment anything re-monitors the
    // series. Enum values serialize camelCase.
    addOptions["monitor"] = monitor ? "firstSeason" : "none";
    addOptions["searchForMissingEpisodes"] = monitor;
    addOptions["searchForCutoffUnmetEpisodes"] = false;
    series["addOptions"] = addOptions;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string body = Json::writeString(wb, series);

    const std::string resp = http_post("/api/v3/series", body);
    if (resp.empty()) {
        spdlog::error("[sonarr] add_series POST failed: {}", last_error());
        return result;  // ok=false
    }

    // The POST response IS the stored resource (RestController.Created
    // serializes GetResourceById) — but the row was inserted microseconds ago
    // and addOptions has not been applied yet, so its seasons[] still reads
    // exactly what we submitted: all monitored. Take the id and nothing else.
    int new_id = 0;
    {
        Json::CharReaderBuilder rb;
        Json::Value posted;
        std::string err;
        std::istringstream is(resp);
        if (Json::parseFromStream(rb, is, &posted, &err) && posted.isObject()) {
            new_id = posted.get("id", 0).asInt();
        }
    }
    if (new_id <= 0 && tvdb_id > 0) {
        // POST response carried no usable id — find the row by the key Sonarr
        // indexes on.
        if (auto probe = find_series_by_tvdb(tvdb_id); probe && !probe->empty()) {
            new_id = probe->front().sonarr_id;
        }
    }
    if (new_id <= 0) {
        set_error("Sonarr accepted the add but no series id could be resolved");
        spdlog::error("[sonarr] add_series: {}", last_error());
        return result;  // ok=false
    }

    // *** Bounded settle poll ***
    // AddSeriesService published SeriesAddedEvent, which queued a
    // RefreshSeriesCommand. Only after that refresh pulls the episode list from
    // SkyHook does EpisodeMonitoredService apply addOptions.monitor and null
    // addOptions out. Poll until we can SEE that happen:
    //   - addOptions was populated and is now gone  → the refresh ran, or
    //   - statistics.totalEpisodeCount > 0          → episodes exist, which is
    //     exactly the precondition EpisodeMonitoredService needs.
    // The first clause needs the transition (not merely "absent"), because a
    // Sonarr build that omits addOptions from the GET resource entirely would
    // otherwise look settled on the very first poll. The second clause is the
    // fallback for that case.
    //
    // THIS SLEEPS — worker thread only.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(cfg_.add_settle_timeout_ms);
    const std::string series_path = "/api/v3/series/" + std::to_string(new_id);
    bool saw_add_options = false;
    for (;;) {
        const std::string cur = http_get(series_path);
        if (!cur.empty()) {
            Json::Value obj;
            Json::CharReaderBuilder rb;
            std::string err;
            std::istringstream is(cur);
            if (Json::parseFromStream(rb, is, &obj, &err) && obj.isObject()) {
                const bool has_add_options =
                    obj.isMember("addOptions") && obj["addOptions"].isObject();
                const bool has_episodes =
                    obj["statistics"].get("totalEpisodeCount", 0).asInt() > 0;
                if (has_add_options) saw_add_options = true;

                if (auto parsed = SonarrParsers::parse_series(cur)) {
                    result.ok = true;
                    result.series = *parsed;   // keep the freshest read we have
                }
                if ((saw_add_options && !has_add_options) || has_episodes) {
                    result.settled = true;
                    break;
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg_.add_settle_poll_ms));
    }

    if (!result.ok) {
        set_error("Sonarr accepted the add but its state could not be read back");
        spdlog::error("[sonarr] add_series: {}", last_error());
        return result;
    }
    if (!result.settled) {
        // Not an error — the add succeeded. But the season flags in
        // `result.series` are the submitted view, not the applied one.
        spdlog::warn("[sonarr] add_series: tmdb={} id={} did not settle within "
                     "{}ms; returning provisional state (caller must re-fetch)",
                     tmdb_id, new_id, cfg_.add_settle_timeout_ms);
    } else {
        spdlog::info("[sonarr] add_series ok: tmdb={} tvdb={} id={} '{}'",
                     tmdb_id, tvdb_id, new_id,
                     series.get("title", "?").asString());
    }
    return result;
}

bool SonarrClient::set_season_monitored(int sonarr_id, int season_number,
                                        bool monitored) {
    set_error({});
    const std::string path = "/api/v3/series/" + std::to_string(sonarr_id);
    const std::string current = http_get(path);
    if (current.empty()) {
        set_error("Sonarr series " + std::to_string(sonarr_id) + " not readable");
        return false;
    }
    Json::Value series;
    {
        Json::CharReaderBuilder rb;
        std::string err;
        std::istringstream is(current);
        if (!Json::parseFromStream(rb, is, &series, &err) || !series.isObject()) {
            set_error("Sonarr series parse failed: " + err);
            return false;
        }
    }
    Json::Value& seasons = series["seasons"];
    if (!seasons.isArray()) {
        set_error("Sonarr series " + std::to_string(sonarr_id) + " has no seasons[]");
        return false;
    }
    bool found = false;
    for (auto& s : seasons) {
        if (s.get("seasonNumber", -1).asInt() == season_number) {
            s["monitored"] = monitored;
            found = true;
            break;
        }
    }
    if (!found) {
        set_error("season " + std::to_string(season_number) + " not found on series "
                  + std::to_string(sonarr_id));
        return false;
    }
    // PUT replaces the whole resource — send the object back intact apart from
    // the one flag we changed.
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return !http_put(path, Json::writeString(wb, series)).empty();
}

bool SonarrClient::trigger_season_search(int sonarr_id, int season_number) {
    std::ostringstream body;
    body << R"({"name":"SeasonSearch","seriesId":)" << sonarr_id
         << R"(,"seasonNumber":)" << season_number << R"(})";
    return !http_post("/api/v3/command", body.str()).empty();
}

bool SonarrClient::trigger_series_search(int sonarr_id) {
    std::ostringstream body;
    body << R"({"name":"SeriesSearch","seriesId":)" << sonarr_id << R"(})";
    return !http_post("/api/v3/command", body.str()).empty();
}

bool SonarrClient::remove_series(int sonarr_id, bool delete_files) {
    set_error({});
    const std::string path = "/api/v3/series/" + std::to_string(sonarr_id)
                           + "?deleteFiles=" + (delete_files ? "true" : "false")
                           + "&addImportListExclusion=false";
    http_delete(path);
    return last_error().empty();
}
```

- [ ] **Step 5: Extend the mock**

In `src/media_browser/sonarr/sonarr_mock.h`, add to the public override block:

```cpp
    AddSeriesResult add_series(int tmdb_id, int quality_profile_id,
                               bool monitor,
                               const std::string& title_fallback) override;
    bool set_season_monitored(int sonarr_id, int season_number, bool monitored) override;
    bool trigger_season_search(int sonarr_id, int season_number) override;
    bool trigger_series_search(int sonarr_id) override;
    bool remove_series(int sonarr_id, bool delete_files) override;
```

In `src/media_browser/sonarr/sonarr_mock.cpp`, add (and `#include <algorithm>` at the top):

```cpp
AddSeriesResult SonarrMockClient::add_series(int tmdb_id,
                                             int /*quality_profile_id*/,
                                             bool monitor,
                                             const std::string& title_fallback) {
    AddSeriesResult r;
    // The mock has no async refresh, so everything it returns is settled.
    r.settled = true;
    for (const auto& s : library_) {
        if (s.tmdb_id == tmdb_id) {  // idempotent
            r.ok = true;
            r.series = s;
            return r;
        }
    }
    Series s;
    s.sonarr_id       = next_id_++;
    s.tmdb_id         = tmdb_id;
    s.tvdb_id         = 100000 + tmdb_id;
    s.title           = title_fallback.empty()
                          ? ("Mock Series " + std::to_string(tmdb_id))
                          : title_fallback;
    s.monitored       = monitor;
    s.runtime_minutes = 45;
    s.path            = "/data/library/tv/" + s.title;
    // Mirror a real firstSeason add: specials off, season 1 on, rest off.
    s.seasons.push_back({0, false, 3, 0, 0});
    s.seasons.push_back({1, monitor, 10, 0, 0});   // "none" when monitor==false
    s.seasons.push_back({2, false, 10, 0, 0});
    library_.push_back(s);
    r.ok = true;
    r.series = s;
    return r;
}

bool SonarrMockClient::set_season_monitored(int sonarr_id, int season_number,
                                            bool monitored) {
    for (auto& s : library_) {
        if (s.sonarr_id != sonarr_id) continue;
        for (auto& season : s.seasons) {
            if (season.season_number == season_number) {
                season.monitored = monitored;
                return true;
            }
        }
        return false;
    }
    return false;
}

bool SonarrMockClient::trigger_season_search(int /*id*/, int /*season*/) { return true; }
bool SonarrMockClient::trigger_series_search(int /*id*/) { return true; }

bool SonarrMockClient::remove_series(int sonarr_id, bool /*delete_files*/) {
    auto it = std::remove_if(library_.begin(), library_.end(),
                             [&](const Series& s) { return s.sonarr_id == sonarr_id; });
    const bool removed = (it != library_.end());
    library_.erase(it, library_.end());
    return removed;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit" "[sonarr]"
```
Expected: all `[sonarr]` cases pass — and the run must finish as fast as the others. If it visibly pauses, the stub's `fast_settle()` config is not reaching the base class and the poll is really sleeping. Then run the whole suite:
```bash
"/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: `All tests passed (… assertions in 224 test cases)` — 212 + 12 new.

- [ ] **Step 7: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/sonarr/ \
  magic_dingus_box_cpp/tests/media_browser/test_sonarr_client.cpp
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): Sonarr add-series, season monitoring, search commands, delete

Sonarr applies addOptions asynchronously — AddSeriesService publishes
SeriesAddedEvent, which queues a RefreshSeriesCommand, and only after that
refresh pulls episodes from SkyHook does EpisodeMonitoredService apply the
monitor enum. Until then both the POST response and any GET show every
season monitored, so add_series bounded-polls for the refresh to land and
reports settled=false on timeout rather than handing back a wrong season
list. addOptions.monitor derives from the caller's flag (none, not
firstSeason, for an unmonitored add). set_season_monitored PUTs the whole
resource because Sonarr's PUT replaces rather than patches.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Queue (per-episode, ungrouped) + history download hashes

**Files:**
- Modify: `src/media_browser/sonarr/sonarr_client.h` / `.cpp`
- Modify: `src/media_browser/sonarr/sonarr_mock.h` / `.cpp`
- Modify: `tests/media_browser/test_sonarr_client.cpp`

**Interfaces:**
- Consumes: Task 4's `SonarrQueueItem`, `SonarrParsers::parse_queue`, `SonarrParsers::parse_history_download_ids`; Task 5's transport.
- Produces (new public virtuals on `SonarrClient`, all overridden in `SonarrMockClient`):
  - `virtual std::vector<SonarrQueueItem> get_queue();`
  - `virtual bool cancel_queue_item(int queue_id);`
  - `virtual std::vector<std::string> get_series_download_hashes(int sonarr_id);`

---

- [ ] **Step 1: Write the failing tests**

Append to `tests/media_browser/test_sonarr_client.cpp`:

```cpp
// --- queue ---------------------------------------------------------------

namespace {
class QueueSonarr : public mb::SonarrClient {
public:
    QueueSonarr() : SonarrClient(Config{}) {}
    std::vector<std::string> gets;
    std::string delete_path;
    std::string http_get(const std::string& path) override {
        gets.push_back(path);
        if (path.rfind("/api/v3/queue", 0) == 0) return read_fixture("queue.json");
        if (path.rfind("/api/v3/history/series", 0) == 0) {
            return read_fixture("history_series.json");
        }
        return "";
    }
    std::string http_post(const std::string&, const std::string&) override { return ""; }
    std::string http_put(const std::string&, const std::string&) override { return ""; }
    std::string http_delete(const std::string& path) override {
        delete_path = path;
        return "{}";
    }
};
}  // namespace

TEST_CASE("get_queue requests embedded episodes and returns them ungrouped",
          "[sonarr][queue]") {
    QueueSonarr s;
    auto q = s.get_queue();

    // One page: the fixture's 3 records are a short page against the default
    // pageSize of 100, so the loop stops without a second request.
    REQUIRE(s.gets.size() == 1);
    CHECK(s.gets[0].rfind("/api/v3/queue?", 0) == 0);
    CHECK(s.gets[0].find("page=1") != std::string::npos);
    CHECK(s.gets[0].find("pageSize=100") != std::string::npos);
    // includeEpisode=true gets the S02E01 label in the same round-trip;
    // Phase 2c's grouped row needs it to say "3 episodes" with names.
    CHECK(s.gets[0].find("includeEpisode=true") != std::string::npos);

    // Three episode rows for ONE season pack, deliberately NOT collapsed.
    // Grouping by download_id is Phase 2c's UI concern; the client must not
    // pre-empt it, because DELETE acts on the whole download and the UI needs
    // to know which ids are siblings.
    REQUIRE(q.size() == 3);
    CHECK(q[0].download_id == q[2].download_id);
    CHECK(q[0].season_number == 2);
    CHECK(q[0].episode.episode_number == 1);
    CHECK(q[2].episode.episode_number == 3);
}

TEST_CASE("get_queue pages until the queue is exhausted", "[sonarr][queue]") {
    // Sonarr's queue is per EPISODE, so a single 20-episode season pack is 20
    // records and ~5 concurrent packs saturate a 100-record page. Truncating
    // silently looks exactly like "that download isn't queued".
    //
    // queue_page_size is configurable precisely so this can be proven without
    // a 100-record fixture: set it to 3 and the existing fixture becomes a
    // FULL page, forcing a second request.
    class PagedSonarr : public mb::SonarrClient {
    public:
        static Config small_pages() {
            Config c;
            c.queue_page_size = 3;
            return c;
        }
        PagedSonarr() : SonarrClient(small_pages()) {}
        std::vector<std::string> gets;
        std::string http_get(const std::string& path) override {
            gets.push_back(path);
            if (path.find("page=1") != std::string::npos) {
                return read_fixture("queue.json");   // 3 records == a full page
            }
            if (path.find("page=2") != std::string::npos) {
                return R"({"page":2,"pageSize":3,"totalRecords":4,"records":[
                  {"id":104,"seriesId":7,"episodeId":5004,"seasonNumber":2,
                   "title":"Breaking.Bad.S02.1080p.WEB-DL.x264-GROUPA",
                   "size":12884901888,"sizeleft":6442450944,"status":"downloading",
                   "downloadId":"A1B2C3D4E5F60718293A4B5C6D7E8F9012345678"}
                ]})";
            }
            return "";
        }
        std::string http_post(const std::string&, const std::string&) override { return ""; }
        std::string http_put(const std::string&, const std::string&) override { return ""; }
        std::string http_delete(const std::string&) override { return ""; }
    };
    PagedSonarr s;
    auto q = s.get_queue();
    REQUIRE(s.gets.size() == 2);
    CHECK(s.gets[0].find("page=1") != std::string::npos);
    CHECK(s.gets[0].find("pageSize=3") != std::string::npos);
    CHECK(s.gets[1].find("page=2") != std::string::npos);
    REQUIRE(q.size() == 4);
    CHECK(q[3].id == 104);
}

TEST_CASE("get_queue returns empty on transport failure", "[sonarr][queue]") {
    class Dead : public mb::SonarrClient {
    public:
        Dead() : SonarrClient(Config{}) {}
        std::string http_get(const std::string&) override { return ""; }
        std::string http_post(const std::string&, const std::string&) override { return ""; }
        std::string http_put(const std::string&, const std::string&) override { return ""; }
        std::string http_delete(const std::string&) override { return ""; }
    };
    Dead d;
    CHECK(d.get_queue().empty());
}

TEST_CASE("cancel_queue_item removes the whole download from the client",
          "[sonarr][queue]") {
    // Live-proven: DELETE /api/v3/queue/{id}?removeFromClient=true kills the
    // entire download; the sibling episode rows then 404. blocklist=false so a
    // user-initiated cancel does not poison the release for a later retry.
    QueueSonarr s;
    REQUIRE(s.cancel_queue_item(101));
    CHECK(s.delete_path == "/api/v3/queue/101?removeFromClient=true&blocklist=false");
}

// --- history -------------------------------------------------------------

TEST_CASE("get_series_download_hashes walks the series history",
          "[sonarr][history]") {
    // Powers the orphan-proof remove: the active queue only knows in-progress
    // downloads, so finished-and-seeding torrents would be left behind in
    // qBittorrent without this.
    QueueSonarr s;
    auto hashes = s.get_series_download_hashes(7);
    REQUIRE(s.gets.size() == 1);
    CHECK(s.gets[0] == "/api/v3/history/series?seriesId=7");
    REQUIRE(hashes.size() == 2);
    CHECK(hashes[0] == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(hashes[1] == "ffeeddccbbaa99887766554433221100aabbccdd");
}

// --- mock ----------------------------------------------------------------

TEST_CASE("SonarrMockClient seeds a coherent season pack", "[sonarr][mock]") {
    mb::SonarrMockClient m;
    auto q = m.get_queue();
    // Three episode rows sharing one downloadId — the shape 2c's grouping has
    // to handle, available on a dev machine with no services running.
    REQUIRE(q.size() == 3);
    CHECK(q[0].download_id == q[1].download_id);
    CHECK(q[1].download_id == q[2].download_id);
    CHECK(q[0].series_id == 1);

    auto hashes = m.get_series_download_hashes(1);
    REQUIRE(hashes.size() == 1);
    // Mock hashes come back lowercase, like the real client's.
    CHECK(hashes[0] == q[0].download_id);

    REQUIRE(m.cancel_queue_item(q[0].id));
    // Cancelling one row removes the whole download, matching live behaviour.
    CHECK(m.get_queue().empty());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8
```
Expected: **compile FAILS** — `no member named 'get_queue' in 'media_browser::SonarrClient'` (and `cancel_queue_item`, `get_series_download_hashes`).

- [ ] **Step 3: Declare the three methods**

In `src/media_browser/sonarr/sonarr_client.h`, after `remove_series`, add:

```cpp
    // Queue / downloads.
    //
    // Sonarr's queue is per EPISODE: a season pack yields N records sharing a
    // single downloadId. These are returned RAW AND UNGROUPED on purpose —
    // Phase 2c groups by download_id for display, and it needs to see the
    // sibling rows to do that (and to know that cancelling any one of them
    // takes the whole download with it).
    //
    // Pages internally (Config::queue_page_size per request) until the queue is
    // exhausted — per-episode records make >100 genuinely reachable.
    virtual std::vector<SonarrQueueItem> get_queue();

    // Removes the download from the client. NOTE: this acts on the WHOLE
    // download, not one episode — every sibling queue id 404s afterwards.
    virtual bool cancel_queue_item(int queue_id);

    // Distinct downloadIds from this series' history, lowercased for direct
    // comparison with QbittorrentClient (which stores hashes lowercase).
    // Feeds the orphan-proof remove: the queue only knows in-progress
    // downloads, so finished-and-seeding torrents would otherwise survive a
    // series deletion.
    virtual std::vector<std::string> get_series_download_hashes(int sonarr_id);
```

- [ ] **Step 4: Implement them**

In `src/media_browser/sonarr/sonarr_client.cpp`, add after `remove_series`:

```cpp
std::vector<SonarrQueueItem> SonarrClient::get_queue() {
    // Sonarr's queue is per EPISODE, so a season pack contributes one record
    // per episode and the queue genuinely outgrows a single page — page
    // through it rather than silently truncating (a missing row is
    // indistinguishable from "that download isn't queued").
    //
    // includeEpisode=true embeds each record's episode object so the UI can
    // label "S02E01 — Seven Thirty-Seven" without a second round-trip.
    // includeSeries stays false: the series is already in the library cache
    // and the extra payload is pure weight on the 2 GB board.
    constexpr int kMaxPages = 20;  // 2000 records at the default page size
    const int page_size = cfg_.queue_page_size > 0 ? cfg_.queue_page_size : 100;
    std::vector<SonarrQueueItem> out;
    int total = 0;
    int page = 1;
    for (; page <= kMaxPages; ++page) {
        const std::string resp = http_get(
            "/api/v3/queue?page=" + std::to_string(page)
            + "&pageSize=" + std::to_string(page_size)
            + "&includeEpisode=true&includeSeries=false");
        if (resp.empty()) break;  // transport failure — keep what we have
        auto batch = SonarrParsers::parse_queue(resp);
        const int batch_total = SonarrParsers::parse_queue_total(resp);
        if (batch_total > 0) total = batch_total;
        const bool short_page = static_cast<int>(batch.size()) < page_size;
        out.insert(out.end(), std::make_move_iterator(batch.begin()),
                   std::make_move_iterator(batch.end()));
        if (short_page) break;
        if (total > 0 && static_cast<int>(out.size()) >= total) break;
    }
    if (page > kMaxPages) {
        spdlog::warn("[sonarr] get_queue hit the {}-page cap with {} records "
                     "(totalRecords={}); the queue view is truncated",
                     kMaxPages, out.size(), total);
    }
    return out;
}

bool SonarrClient::cancel_queue_item(int queue_id) {
    set_error({});
    http_delete("/api/v3/queue/" + std::to_string(queue_id)
                + "?removeFromClient=true&blocklist=false");
    return last_error().empty();
}

std::vector<std::string> SonarrClient::get_series_download_hashes(int sonarr_id) {
    // /api/v3/history/series is UNPAGINATED (a bare array) — no pageSize
    // parameter, unlike Radarr's /api/v3/history.
    auto resp = http_get("/api/v3/history/series?seriesId="
                         + std::to_string(sonarr_id));
    if (resp.empty()) return {};
    return SonarrParsers::parse_history_download_ids(resp);
}
```

- [ ] **Step 5: Extend the mock**

In `src/media_browser/sonarr/sonarr_mock.h`, add to the public override block:

```cpp
    std::vector<SonarrQueueItem> get_queue() override;
    bool cancel_queue_item(int queue_id) override;
    std::vector<std::string> get_series_download_hashes(int sonarr_id) override;
```

and to the protected member block:

```cpp
    std::vector<SonarrQueueItem> queue_;
```

In `src/media_browser/sonarr/sonarr_mock.cpp`, seed the queue at the end of the constructor:

```cpp
    // A season-2 pack in flight: three episode rows sharing one downloadId,
    // the exact shape Phase 2c's grouping must handle. Lowercase hash — the
    // real client lowercases history hashes and QueueScreen lowercases the
    // queue's before comparing, so the mock never hands out a casing the UI
    // would not see.
    const std::string kMockHash = "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678";
    for (int ep = 1; ep <= 3; ++ep) {
        SonarrQueueItem q;
        q.id             = 100 + ep;
        q.series_id      = library_.front().sonarr_id;
        q.episode_id     = 5000 + ep;
        q.season_number  = 2;
        q.title          = "Breaking.Bad.S02.1080p.WEB-DL.x264-MOCK";
        q.size_bytes     = 12'884'901'888LL;
        q.sizeleft_bytes = 6'442'450'944LL;
        q.progress       = 0.5;
        q.eta_seconds    = 4800;
        q.state          = "downloading";
        q.tracked_download_state = "downloading";
        q.download_id    = kMockHash;
        q.episode.id             = q.episode_id;
        q.episode.series_id      = q.series_id;
        q.episode.season_number  = 2;
        q.episode.episode_number = ep;
        q.episode.title          = "Mock Episode " + std::to_string(ep);
        queue_.push_back(std::move(q));
    }
```

and the three overrides:

```cpp
std::vector<SonarrQueueItem> SonarrMockClient::get_queue() { return queue_; }

bool SonarrMockClient::cancel_queue_item(int queue_id) {
    // Cancelling one row removes the WHOLE download, exactly like the live
    // DELETE — a mock that removed a single episode row would teach the UI
    // the wrong lesson.
    std::string hash;
    for (const auto& q : queue_) if (q.id == queue_id) hash = q.download_id;
    if (hash.empty()) return false;
    auto it = std::remove_if(queue_.begin(), queue_.end(),
                             [&](const SonarrQueueItem& q) {
                                 return q.download_id == hash;
                             });
    queue_.erase(it, queue_.end());
    return true;
}

std::vector<std::string>
SonarrMockClient::get_series_download_hashes(int sonarr_id) {
    std::vector<std::string> out;
    for (const auto& q : queue_) {
        if (q.series_id != sonarr_id || q.download_id.empty()) continue;
        if (std::find(out.begin(), out.end(), q.download_id) == out.end()) {
            out.push_back(q.download_id);
        }
    }
    return out;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: `All tests passed (… assertions in 230 test cases)` — 224 + 6 new.

- [ ] **Step 7: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/sonarr/ \
  magic_dingus_box_cpp/tests/media_browser/test_sonarr_client.cpp
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): Sonarr queue + history download hashes

The queue is returned raw and per-episode with the shared downloadId
intact; grouping is the UI's job in Phase 2c. Cancel removes the whole
download (sibling rows 404 afterwards) and history hashes come back
lowercased for the qBittorrent comparison.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Kiosk wiring — `main.cpp` construction, `KIOSK_MEDIA_BROWSER_SOURCES`, Pi compile-verify

**Files:**
- Modify: `src/main.cpp` (inside the existing `#ifdef MEDIA_BROWSER_ENABLED` block)
- Modify: `CMakeLists.txt` (`KIOSK_MEDIA_BROWSER_SOURCES`)

**Interfaces:**
- Consumes: `SonarrClient` (Tasks 5–7), `SonarrMockClient`, `SonarrClient::Config`, `SonarrClient::normalize_prefix`, and the existing `read_env_file_key` lambda already defined in `main.cpp`.
- Produces: a constructed `std::unique_ptr<media_browser::SonarrClient> sonarr_owned` and a bound `media_browser::SonarrClient& sonarr` in `main()`, unconsumed until Phase 2c. No behavior change.

---

- [ ] **Step 1: Confirm the tree is clean before touching `main.cpp`**

`main.cpp` is the file most likely to collide with parallel work. Verify nothing is staged or modified:

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" status --short
```
Expected: no output (clean tree). If `src/main.cpp` shows as modified by someone else, STOP and report — do not merge on top of an uncommitted dispatcher change.

- [ ] **Step 2: Add the Sonarr sources to the kiosk source list**

**Anchor warning, again — and it is worse here than in Task 4.** After Task 5 both source lists contain the identical four-line sequence `radarr_mock.cpp / sonarr_parsers.cpp / sonarr_client.cpp / sonarr_mock.cpp`, distinguished ONLY by leading whitespace: indent 12 inside `list(APPEND KIOSK_MEDIA_BROWSER_SOURCES` (this task's target) versus indent 8 inside `set(MEDIA_BROWSER_SOURCES` (Task 4/5's target). Quoting that block would match both, and an indent-normalizing paste lands in the wrong list — where it would compile fine and silently fail to link the kiosk.

`prowlarr_client.cpp` is no better an anchor — it too appears in both lists (indent 12 and indent 8). The one string guaranteed unique is the list's own opening line. Verify, then insert directly beneath it:

```bash
grep -c "list(APPEND KIOSK_MEDIA_BROWSER_SOURCES" "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/CMakeLists.txt"
```
Expected: `1`.

```cmake
        list(APPEND KIOSK_MEDIA_BROWSER_SOURCES
            src/media_browser/sonarr/sonarr_parsers.cpp
            src/media_browser/sonarr/sonarr_client.cpp
            src/media_browser/sonarr/sonarr_mock.cpp
            src/media_browser/radarr/radarr_parsers.cpp
```

Verify placement before moving on:
```bash
awk '/KIOSK_MEDIA_BROWSER_SOURCES/{k=1} /^    endif/{k=0} k && /sonarr/{print NR": "$0}' \
  "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/CMakeLists.txt"
```
Expected: three lines, all indented 12 spaces.

These three files are renderer-free, so they belong in BOTH lists — the same dual-list arrangement `mb_ui_utils.cpp`, `library_view.cpp` and `mb_recs.cpp` already use. The two lists feed different targets and neither includes the other; a file missing from one link-fails only that target.

- [ ] **Step 3: Construct the client in `main.cpp`**

Add the include next to the existing Radarr includes near the top of `src/main.cpp`:

```cpp
#include "media_browser/radarr/radarr_mock.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_mock.h"
```

Then, immediately **after** the line `media_browser::RadarrClient& radarr = *radarr_owned;` and **before** the `// TMDB client — Phase A` comment, insert:

```cpp
    // Sonarr client (Phase 2b). Same three-stage key chain as Radarr above:
    //   1. MDB_SONARR_API_KEY env var (explicit kiosk config)
    //   2. SONARR_API_KEY env var (systemd EnvironmentFile of services/.env)
    //   3. Parse /opt/magic_dingus_box/services/.env directly
    // setup_services.sh writes SONARR_API_KEY into that .env after Sonarr's
    // first container start; a box provisioned before the Sonarr stack landed
    // simply has no line and falls through to the mock.
    std::unique_ptr<media_browser::SonarrClient> sonarr_owned;
    std::string sonarr_key;
    if (const char* sk = std::getenv("MDB_SONARR_API_KEY"); sk && *sk) sonarr_key = sk;
    else if (const char* sk2 = std::getenv("SONARR_API_KEY"); sk2 && *sk2) sonarr_key = sk2;
    else sonarr_key = read_env_file_key("/opt/magic_dingus_box/services/.env", "SONARR_API_KEY");

    if (!sonarr_key.empty()) {
        media_browser::SonarrClient::Config sonarr_cfg;
        if (const char* base = std::getenv("MDB_SONARR_BASE_URL"); base && *base) {
            sonarr_cfg.base_url = base;
        }
        sonarr_cfg.api_key = sonarr_key;
        // TV path prefixes, resolved in three tiers.
        //
        // The TV subtree is /data/library/tv ↔ /mnt/ssd/library/tv — one level
        // below the movie library root — so it cannot simply reuse the Radarr
        // vars (every TV path would translate one directory too high). But it
        // must not ignore them either: MDB_HOST_LIBRARY_PREFIX exists so
        // STORAGE_ROOT can move without a recompile, and a box where the
        // operator points movies at /mnt/nvme/library/ while Sonarr keeps a
        // compiled-in /mnt/ssd/library/tv/ would hand GStreamer an
        // unresolvable container path — with nothing but a spdlog::warn to say
        // so, and none of the legacy-alternate fallbacks the Radarr resolver
        // has. Nothing in provisioning writes MDB_*_TV_PREFIX, so deriving
        // from the parent is what actually fires in the field.
        //
        // Order: explicit TV var → parent movie var + "tv" → compiled default.
        auto tv_prefix = [](const char* tv_var, const char* parent_var,
                            const std::string& compiled_default) -> std::string {
            if (const char* p = std::getenv(tv_var); p && *p) {
                return media_browser::SonarrClient::normalize_prefix(p);
            }
            if (const char* p = std::getenv(parent_var); p && *p) {
                return media_browser::SonarrClient::normalize_prefix(
                    media_browser::SonarrClient::normalize_prefix(p) + "tv");
            }
            return compiled_default;
        };
        sonarr_cfg.container_library_prefix =
            tv_prefix("MDB_CONTAINER_TV_PREFIX", "MDB_CONTAINER_LIBRARY_PREFIX",
                      sonarr_cfg.container_library_prefix);
        sonarr_cfg.host_library_prefix =
            tv_prefix("MDB_HOST_TV_PREFIX", "MDB_HOST_LIBRARY_PREFIX",
                      sonarr_cfg.host_library_prefix);
        std::cout << "[media_browser] sonarr tv prefixes: "
                  << sonarr_cfg.container_library_prefix << " -> "
                  << sonarr_cfg.host_library_prefix << std::endl;
        std::string sonarr_url_for_log = sonarr_cfg.base_url;
        sonarr_owned = std::make_unique<media_browser::SonarrClient>(std::move(sonarr_cfg));
        std::cout << "[media_browser] Using real SonarrClient (base_url="
                  << sonarr_url_for_log << ")" << std::endl;
    } else {
        sonarr_owned = std::make_unique<media_browser::SonarrMockClient>();
        std::cout << "[media_browser] No Sonarr API key found — using SonarrMockClient"
                  << std::endl;
    }
    media_browser::SonarrClient& sonarr = *sonarr_owned;
    // Phase 2b wires construction only. No screen consumes `sonarr` yet — the
    // Movies/TV toggle, series detail and grouped queue land in Phase 2c. The
    // cast keeps -Wunused-variable quiet without leaving a dangling TODO.
    (void)sonarr;
```

- [ ] **Step 4: Prove the `ENABLE_MEDIA_BROWSER=OFF` binary is untouched**

The locked-off build must stay bit-identical. Rather than spending a second 20-minute Pi build, prove it by inspection — every edit must sit inside a gate:

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" diff -- magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/CMakeLists.txt
```
Confirm by reading the diff that:
- every `main.cpp` hunk is between `#ifdef MEDIA_BROWSER_ENABLED` and its `#endif`, and
- every `CMakeLists.txt` hunk is inside `if(ENABLE_MEDIA_BROWSER)`.

If either is false, move the code — do not proceed.

- [ ] **Step 5: Compile-verify the kiosk binary on the Pi (isolated scratch build)**

Never `deploy_cpp.sh`; never restart the live unit. `assets/` MUST be synced — CMake `file COPY`s it unguarded and configure fails late without it.

```bash
rsync -a --checksum --delete \
  --exclude 'build*' --exclude '.git' --exclude 'data/text_input_queue.jsonl' \
  "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/" \
  magic@magicpi.local:/home/magic/tvclient_build/
```

```bash
ssh magic@magicpi.local 'cmake -S /home/magic/tvclient_build -B /home/magic/tvclient_build/build-kiosk \
  -DBUILD_KIOSK=ON -DBUILD_TESTS=OFF -DENABLE_MEDIA_BROWSER=ON'
```
Expected: configure completes with `-- Configuring done` / `-- Generating done` (watch the LAST line — a missing `assets/` fails after every `pkg_check_modules` succeeds, so the log reads healthy until the end).

```bash
ssh magic@magicpi.local 'cmake --build /home/magic/tvclient_build/build-kiosk --target magic_dingus_box_cpp -j3'
```
`-j3`, not `-j4` — `-j4` starves the running kiosk. Roughly 20 minutes from cold on a Pi 5.
Expected: `[100%] Built target magic_dingus_box_cpp`, zero warnings from any `sonarr_*.cpp` or `tmdb_client.cpp` (the kiosk target uses `-Wall -Wextra -Wpedantic`).

- [ ] **Step 6: Confirm the live kiosk was not disturbed**

```bash
ssh magic@magicpi.local 'systemctl is-active magic-dingus-box-cpp.service'
```
Expected: `active`. (The unit is `magic-dingus-box-cpp.service` — querying `magic-dingus-box` returns `inactive` and reads as "nothing is running" when the kiosk is in fact live.)

- [ ] **Step 7: Re-run the Mac suite and commit**

```bash
cmake --build "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb" --target test_media_browser_unit -j8 \
  && "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/build-mb/test_media_browser_unit"
```
Expected: still `230 test cases`, all passing.

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/main.cpp magic_dingus_box_cpp/CMakeLists.txt
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): construct SonarrClient in main.cpp

Mirrors the Radarr three-stage key chain (MDB_SONARR_API_KEY ->
SONARR_API_KEY -> services/.env) with a mock fallback, plus TV-specific
path prefixes that derive from the movie prefix when only that one is
overridden — reusing it directly would translate every TV path one
directory too high, while ignoring it would strand Sonarr on a compiled-in
path whenever STORAGE_ROOT moves. No screen consumes the client yet.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: `sonarr-*` subcommands in the diagnostic CLI + live smoke on the box

**Files:**
- Modify: `src/media_browser/test_cli/main.cpp`

**Interfaces:**
- Consumes: `SonarrClient` (Tasks 5–7) — `get_status`, `get_quality_profiles`, `get_root_folders` (its output is one of the four things Step 7's live smoke checks), `lookup_by_tmdb`, `get_library_checked` (**not** the bare `get_library` — the CLI must print "fetch failed" distinctly from "empty library"), `get_queue`, `last_error`.
- Produces: four subcommands — `sonarr-status`, `sonarr-lookup <tmdb_id>`, `sonarr-library`, `sonarr-queue` — plus `Config::sonarr_api_key` / `Config::sonarr_base_url` fed by `MDB_SONARR_API_KEY` / `MDB_SONARR_BASE_URL`.

This target already links `MEDIA_BROWSER_SOURCES`, which gained the Sonarr sources in Tasks 4–5, so **no CMake change is needed here**.

---

- [ ] **Step 1: Extend the CLI config**

In `src/media_browser/test_cli/main.cpp`, add to `struct Config` after `radarr_base_url`:

```cpp
    std::string sonarr_api_key;
    std::string sonarr_base_url = "http://localhost:8989";
```

and in `load_config()`, after the two Radarr lines:

```cpp
    if (const char* k = std::getenv("MDB_SONARR_API_KEY")) c.sonarr_api_key = k;
    if (const char* u = std::getenv("MDB_SONARR_BASE_URL")) c.sonarr_base_url = u;
```

- [ ] **Step 2: Add the help text and forward declarations**

In `print_help()`, after the `radarr-profiles` line:

```cpp
        "  sonarr-status                Ping Sonarr, show version + reachability.\n"
        "  sonarr-lookup <tmdb_id>      Sonarr /series/lookup?term=tmdb:<id>.\n"
        "  sonarr-library               Show all series in library (per-season state).\n"
        "  sonarr-queue                 Show the per-episode download queue.\n"
```

and in the `Environment:` block:

```cpp
        "  MDB_SONARR_API_KEY Sonarr v4 API key (required for sonarr-* commands).\n"
        "  MDB_SONARR_BASE_URL Sonarr base URL (default: http://localhost:8989).\n"
```

After the `cmd_radarr_profiles` forward declaration:

```cpp
int cmd_sonarr_status(const Config& c);
int cmd_sonarr_lookup(const Config& c, int tmdb_id);
int cmd_sonarr_library(const Config& c);
int cmd_sonarr_queue(const Config& c);
```

Add the include next to the Radarr one at the top of the file:

```cpp
#include "media_browser/sonarr/sonarr_client.h"
```

- [ ] **Step 3: Add the dispatch entries**

In `main()`, after `if (cmd == "radarr-profiles") return cmd_radarr_profiles(cfg);`:

```cpp
    if (cmd == "sonarr-status") return cmd_sonarr_status(cfg);
    if (cmd == "sonarr-lookup") {
        if (argc < 3) { print_help(); return 2; }
        return cmd_sonarr_lookup(cfg, std::atoi(argv[2]));
    }
    if (cmd == "sonarr-library") return cmd_sonarr_library(cfg);
    if (cmd == "sonarr-queue") return cmd_sonarr_queue(cfg);
```

- [ ] **Step 4: Implement the four commands**

Add after `cmd_radarr_profiles`'s definition, inside the same anonymous namespace:

```cpp
media_browser::SonarrClient::Config make_sonarr_config(const Config& c) {
    media_browser::SonarrClient::Config sc;
    sc.base_url = c.sonarr_base_url;
    sc.api_key = c.sonarr_api_key;
    return sc;
}

int cmd_sonarr_status(const Config& c) {
    if (c.sonarr_api_key.empty()) {
        spdlog::error("no Sonarr API key - set MDB_SONARR_API_KEY");
        return 1;
    }
    media_browser::SonarrClient s(make_sonarr_config(c));
    auto status = s.get_status();
    if (!status) {
        spdlog::error("fetch failed: {}", s.last_error());
        return 1;
    }
    spdlog::info("Sonarr: {} (reachable: true)", status->version);
    auto profiles = s.get_quality_profiles();
    for (const auto& p : profiles) {
        spdlog::info("  profile [{:>3}] {}", p.id, p.name);
    }
    for (const auto& r : s.get_root_folders()) {
        spdlog::info("  root    [{:>3}] {}  free={} GB",
                     r.id, r.path, r.free_space_bytes / 1'000'000'000);
    }
    return 0;
}

int cmd_sonarr_lookup(const Config& c, int tmdb_id) {
    if (c.sonarr_api_key.empty()) {
        spdlog::error("no Sonarr API key - set MDB_SONARR_API_KEY");
        return 1;
    }
    media_browser::SonarrClient s(make_sonarr_config(c));
    auto hits = s.lookup_by_tmdb(tmdb_id);
    if (hits.empty()) {
        spdlog::error("no results for tmdb:{} ({})", tmdb_id, s.last_error());
        return 1;
    }
    spdlog::info("{} result(s) for tmdb:{}:", hits.size(), tmdb_id);
    for (const auto& h : hits) {
        spdlog::info("  {} ({})  tvdb={} tmdb={} runtime={}min status={} seasons={}",
                     h.title, h.year, h.tvdb_id, h.tmdb_id,
                     h.runtime_minutes, h.status, h.seasons.size());
        for (const auto& season : h.seasons) {
            spdlog::info("     S{:02d} monitored={} episodes={}",
                         season.season_number, season.monitored,
                         season.episode_count);
        }
    }
    return 0;
}

int cmd_sonarr_library(const Config& c) {
    if (c.sonarr_api_key.empty()) {
        spdlog::error("no Sonarr API key - set MDB_SONARR_API_KEY");
        return 1;
    }
    media_browser::SonarrClient s(make_sonarr_config(c));
    auto checked = s.get_library_checked();
    if (!checked) {
        spdlog::error("library fetch failed: {}", s.last_error());
        return 1;
    }
    spdlog::info("Library: {} series", checked->size());
    for (const auto& series : *checked) {
        spdlog::info("  [{:>5}] {} ({})  files={} size={} GB  path={}",
                     series.sonarr_id, series.title, series.year,
                     series.episode_file_count,
                     series.size_on_disk_bytes / 1'000'000'000, series.path);
        for (const auto& season : series.seasons) {
            spdlog::info("     S{:02d} monitored={} files={}/{}",
                         season.season_number, season.monitored,
                         season.episode_file_count, season.episode_count);
        }
    }
    return 0;
}

int cmd_sonarr_queue(const Config& c) {
    if (c.sonarr_api_key.empty()) {
        spdlog::error("no Sonarr API key - set MDB_SONARR_API_KEY");
        return 1;
    }
    media_browser::SonarrClient s(make_sonarr_config(c));
    auto q = s.get_queue();
    // Per-episode records, deliberately ungrouped — a season pack shows N
    // rows sharing one downloadId. Phase 2c collapses them for display.
    spdlog::info("Queue: {} episode record(s)", q.size());
    for (const auto& it : q) {
        spdlog::info("  [{:>5}] S{:02d}E{:02d} {} {:.1f}% state={} dl={}",
                     it.id, it.season_number, it.episode.episode_number,
                     it.episode.title.empty() ? it.title : it.episode.title,
                     it.progress * 100.0, it.state, it.download_id);
    }
    return 0;
}
```

- [ ] **Step 5: Build the CLI on the Pi scratch tree**

```bash
rsync -a --checksum --delete \
  --exclude 'build*' --exclude '.git' --exclude 'data/text_input_queue.jsonl' \
  "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient/magic_dingus_box_cpp/" \
  magic@magicpi.local:/home/magic/tvclient_build/
```

```bash
ssh magic@magicpi.local 'cmake -S /home/magic/tvclient_build -B /home/magic/tvclient_build/build-cli \
  -DBUILD_KIOSK=OFF -DBUILD_TESTS=ON -DENABLE_MEDIA_BROWSER=ON \
  && cmake --build /home/magic/tvclient_build/build-cli --target test_media_browser -j3'
```
Expected: `[100%] Built target test_media_browser`.

- [ ] **Step 6: Run the unit suite on the Pi too**

The Mac loop covers the logic, but aarch64 catches size/alignment surprises the x86 Mac hides:

```bash
ssh magic@magicpi.local 'cmake --build /home/magic/tvclient_build/build-cli --target test_media_browser_unit -j3 \
  && /home/magic/tvclient_build/build-cli/test_media_browser_unit'
```
Expected: `All tests passed (… assertions in 230 test cases)`.

- [ ] **Step 7: Live smoke against the real Sonarr**

The key is read straight out of the box's `.env` and never printed:

```bash
ssh magic@magicpi.local 'export MDB_SONARR_API_KEY=$(grep "^SONARR_API_KEY=" /opt/magic_dingus_box/services/.env | cut -d= -f2); \
  CLI=/home/magic/tvclient_build/build-cli/test_media_browser; \
  echo "=== status ==="  && $CLI sonarr-status  && \
  echo "=== lookup ==="  && $CLI sonarr-lookup 1396 && \
  echo "=== library ===" && $CLI sonarr-library && \
  echo "=== queue ==="   && $CLI sonarr-queue'
```

Expected output, checked item by item:
- **status** — a real version string (`4.0.19.2979` family), a profile line reading `Any`, and a root line `/data/library/tv` with non-zero free space.
- **lookup** — `Breaking Bad (2008)  tvdb=81189 tmdb=1396` with a non-zero `runtime` and a seasons list. This is the whole TMDB-ids-resolve-directly claim, re-proven through the C++ client.
- **library** — whatever is on the box; per-season `monitored=` flags must be a MIX (not all true) for any series added with `firstSeason`. All-true means either the settle poll gave up too early (check for the `did not settle` warning in the output) or the parser is reading the wrong field.
- **queue** — either `0 episode record(s)` (fine — nothing downloading) or N rows. If a season pack is in flight, confirm several rows share one `dl=` value; that is the shape Phase 2c groups.

If `sonarr-status` fails with a transport error, check Gluetun before suspecting the client — Sonarr shares its netns and is unreachable from the host whenever the tunnel is down:
```bash
ssh magic@magicpi.local 'docker ps --filter name=mdb_sonarr --format "{{.Names}} {{.Status}}"; \
  docker ps --filter name=mdb_gluetun --format "{{.Names}} {{.Status}}"'
```

- [ ] **Step 8: Commit**

```bash
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" add \
  magic_dingus_box_cpp/src/media_browser/test_cli/main.cpp
git -C "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /.worktrees/tvclient" commit -m "$(cat <<'EOF'
feat(mb): sonarr-* subcommands in the diagnostic CLI

sonarr-status / sonarr-lookup / sonarr-library / sonarr-queue mirror the
radarr-* set so the client can be exercised against the real box by hand.
The library and lookup output prints per-season monitored flags, which is
the fastest way to spot a settle-poll regression.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
)"
```

---

## Definition of Done

- [ ] `test_media_browser_unit` passes on the Mac at **230 test cases** (167 baseline + 63 new), zero failures.
- [ ] The same binary passes on the Pi (aarch64).
- [ ] The kiosk binary compiles and links with `ENABLE_MEDIA_BROWSER=ON` in the isolated Pi scratch build, zero new warnings under `-Wall -Wextra -Wpedantic`.
- [ ] Every `main.cpp` edit sits inside `#ifdef MEDIA_BROWSER_ENABLED`; every `CMakeLists.txt` edit inside `if(ENABLE_MEDIA_BROWSER)` — the locked-off binary is untouched.
- [ ] `test_media_browser sonarr-lookup 1396` prints Breaking Bad with `tvdb=81189` against the live box; `sonarr-queue` prints the per-episode queue.
- [ ] No user-visible behavior change: no screen consumes `SonarrClient`, no TV row reaches any grid.
- [ ] `magic_dingus_box_cpp/scripts/setup_services.sh` and the repo-root `scripts/golden_image/*` are untouched (`git diff --stat` shows neither).
- [ ] `magic-dingus-box-cpp.service` is still `active` on the box.

## Handoff to Phase 2c

Phase 2c owns everything this plan deliberately left out. The seams it will build on:

- **Grouping** — `get_queue()` returns per-episode `SonarrQueueItem`s; group by `download_id` for one row per download, and remember that cancelling any member id kills the whole download.
- **Mode toggle** — `TmdbSearchHit::kind` already distinguishes rows; Library and Queue are supposed to show both kinds with a type badge and ignore the toggle.
- **TMDB movie/TV id spaces OVERLAP COMPLETELY** — TV id 1396 is Breaking Bad; movie id 1396 is an unrelated film. Any set or map keyed on a bare `tmdb_id` (not a `{kind, id}` pair) MUST hold only one kind, or it will silently collapse an unrelated movie and show into one entry. Nothing breaks in 2b (no UI ships), but the moment 2c extends Library/Queue to show both kinds, the existing movie-only int-keyed collections become live hazards: `browse_screen.cpp`'s owned/hide filter (`library_tmdb_ids_.count(m.tmdb_id)`, two call sites), `browse_screen.h`'s `loaded_tmdb_ids_` (append-page dedupe), and `mb_recs.cpp`'s `by_id` / `exclude` maps. See the doc comment next to `MediaKind` in `tmdb_client.h` for the full rule.
- **Genre tables** — call `get_tv_genres()` for TV mode and keep the result in a table separate from `get_genres()`. The id spaces overlap on 8 values and diverge on 19; one shared table silently mislabels.
- **Season UI** — `Series::seasons` carries `monitored`, `episode_count` and `episode_file_count`, which is exactly the none/downloading/complete tri-state. "Download next season" = `set_season_monitored(id, n, true)` then `trigger_season_search(id, n)`.
- **Adds are asynchronous — respect `AddSeriesResult::settled`.** `add_series` bounded-polls until Sonarr's async refresh visibly lands the REQUESTED monitoring outcome (see `add_settled()` / `record_refreshed()` in `sonarr_client.cpp` — a live probe found `addOptions` is never observable on read at all, so settle detection cannot use it as a signal). When `settled == false`, `series.seasons` is ALWAYS EMPTY — `sonarr_client.cpp:393` and `:509` both clear it rather than leave a pending/mid-refresh snapshot for a naive caller to render as fact — so re-fetch with `get_series(id)` and never treat the empty list as "this show has 0 seasons". **`settled == false` has two different meanings 2c must distinguish:** (1) on a just-added series, the poll simply timed out — TRANSIENT, re-fetch shortly and it will likely resolve; (2) on an already-in-library series (the idempotent add path), the record has never been refreshed by Sonarr at all — e.g. an announced/upcoming series with no episodes yet — which is PERMANENT; no amount of re-polling flips it on its own. A spinner keyed on `settled` alone would spin forever for case (2); it needs a different treatment (e.g. "no season data yet — try Trigger Search") rather than a retry loop. And call it **off the render thread** — it sleeps.
- **Disk estimate** — `SeriesSearchHit::runtime_minutes` is per-episode; multiply by the season's `episode_count` and Sonarr's preferred MB/min.
- **TV search is not built.** There is no `search_tv()` / `build_tv_search_url` in `TmdbClient` — Phase 2b ships the six discovery endpoints the spec's Clients bullet names, and search-follows-mode is 2c's. When 2c adds it: send `include_adult=false`, because `/search/tv` and `/discover/tv` are the **only** two TV endpoints that accept the parameter (popular, top_rated, similar and recommendations do not). Note also that `/search/tv` takes two different year params — `first_air_date_year` matches the series premiere only, while `year` matches the premiere **and** every episode air date, so a show that ran for a decade matches ten different `year` values.
- **Open product decision, unrated shows.** TV has no certification gate at all in this design, and many niche/foreign series return an empty `content_ratings.results[]` anyway. Phase 2b allows everything; whoever revisits family-safe posture for TV must decide block-by-default vs allow for unrated shows, and accept that TV-MA content surfaces in the grids either way. Recording it here so the tradeoff is visible rather than buried in a code comment.
- **Not built yet** — no `get_episodes()` (Phase 3's episode picker needs it), no TV availability search (Prowlarr's category block is still movie-only 2000–2080; TV needs the 5000-series), and no `content_ratings` fetch (see above).
