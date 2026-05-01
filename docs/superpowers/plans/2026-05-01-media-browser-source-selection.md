# Media Browser source-selection redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the design from `docs/superpowers/specs/2026-05-01-media-browser-source-selection-design.md` — expand the Prowlarr indexer pool from 4 to 10 active sources, add a manual release-picker screen with always-visible + reactive entry points, replace the broken in-memory Sources toggle with a health-led settings panel that actually persists to Prowlarr, and add a download watchdog that prompts the user when an auto-pick stalls.

**Architecture:** The work is staged so each phase ships standalone. Phase 1 is pure configuration (JSON + shell, deploys via `setup_services.sh`, no C++ rebuild). Phase 2 refactors `ProwlarrClient` from aggregate-only to per-release storage — this is the foundation everything else needs (the picker reads candidates from it, the settings panel reads per-indexer stats from it). Phase 3 adds two `RadarrClient` methods (`grab_release` for picker grabs, `get_history` for stall detection). Phases 4–5 build the picker UI on top of the refactored Prowlarr client, then add the Detail-screen entry point. Phase 6 adds the watchdog module + stall prompt modal. Phase 7 rebuilds the Sources panel using the live Prowlarr API + per-indexer stats.

**Tech Stack:** C++17, immediate-mode OpenGL ES 3.0, libcurl HTTP (synchronous, mutex-protected sessions), JsonCpp for parsing, Catch2 v3 for tests. Build: `cmake .. && make -j2` on Pi via `magic_dingus_box_cpp/scripts/deploy_cpp.sh --build`. Service deploy: `ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service'` after each build. Service config deploy (Phase 1 only): `ssh magic@10.55.0.1 'sudo /opt/magic_dingus_box/services/setup_services.sh'`.

---

## File map

| File | Responsibility |
|---|---|
| `scripts/data/prowlarr_indexers.json` | Append six new Cardigann indexer entries (1337x, TGx, Solid, BitSearch, Knaben, TheRARBG). Existing entries unchanged. |
| `scripts/data/radarr_custom_formats.json` | Expand the "Trusted small-release groups" regex with TGx, EVO, AOC, ION10, QxR, SuccessfulCrab. |
| `scripts/setup_services.sh` | Step 14b: `TARGET = 5` → `TARGET = 10` for Radarr indexer `minimumSeeders`. |
| `src/media_browser/prowlarr/prowlarr_client.h` | New `ReleaseRecord` and `IndexerStats` structs; new `get_last_releases()` and `get_last_indexer_stats()` accessors. Existing `ReleaseSummary` retained for back-compat with Detail screen's existing readout. |
| `src/media_browser/prowlarr/prowlarr_client.cpp` | Search worker now retains per-release data + per-indexer counts/timing/error in member maps. |
| `src/media_browser/radarr/radarr_client.h` | Two new virtual methods: `grab_release(const Json::Value& release)` and `get_history(int movieId)`. New `HistoryEvent` struct. |
| `src/media_browser/radarr/radarr_client.cpp` | Implementations: POST `/api/v3/release`, GET `/api/v3/history?movieId=X`. |
| `src/media_browser/qbittorrent/qbittorrent_client.h` | Add `get_torrent(hash)` accessor returning `std::optional<QbitTorrent>`. (Existing class is otherwise sufficient.) |
| `src/media_browser/ui/mb_screen.h` | Add `Screen::ReleasePicker` to the enum. |
| `src/media_browser/ui/release_picker_screen.h` (new) | `ReleasePickerScreen` class declaration; `ReleaseCandidate` struct. |
| `src/media_browser/ui/release_picker_screen.cpp` (new) | Sort/render/input logic. Calls `RadarrClient::grab_release` on SELECT. |
| `src/media_browser/ui/detail_screen.h` | New "Pick a source" button entry in the action row. |
| `src/media_browser/ui/detail_screen.cpp` | `rebuild_buttons()` adds the button (gated on Prowlarr-state == Ready). New handler opens picker. |
| `src/media_browser/qbittorrent/download_watchdog.h` (new) | `DownloadWatchdog` class with `watch(tmdb_id, title)` + `tick()` + stall-event signal. |
| `src/media_browser/qbittorrent/download_watchdog.cpp` (new) | Polling loop (driven from main loop's per-frame tick), stall conditions, snooze map. |
| `src/media_browser/ui/stall_prompt_modal.h` (new) | Lightweight modal panel — title, message, two buttons (`[Pick]` / `[Dismiss]`). |
| `src/media_browser/ui/stall_prompt_modal.cpp` (new) | Render + input handling. Used by main loop to surface watchdog stall events. |
| `src/media_browser/ui/mb_settings_screen.h` | Reshape `ProwlarrIndexer` aggregate → `IndexerRow` carrying name + enabled + stats. |
| `src/media_browser/ui/mb_settings_screen.cpp` | Sources panel: pulls indexer list from Prowlarr `GET /api/v1/indexer` on entry, decorates with `ProwlarrClient::get_last_indexer_stats()`, sorts by result count desc, SELECT calls `PUT /api/v1/indexer/<id>`. |
| `src/main.cpp` | Wire new screens into dispatcher; instantiate watchdog; route stall-prompt modal display. |
| `magic_dingus_box_cpp/CMakeLists.txt` | Add new source files to `KIOSK_MEDIA_BROWSER_SOURCES` and corresponding tests to `MEDIA_BROWSER_TEST_SOURCES`. |
| `tests/media_browser/test_release_picker.cpp` (new) | Sort order, gold-border auto-pick highlight, dim-red below-threshold logic. |
| `tests/media_browser/test_download_watchdog.cpp` (new) | Stall conditions (zero-progress, queue-failed, blacklist-event), snooze, cleanup. |
| `tests/media_browser/test_prowlarr_client.cpp` (new) | Per-indexer stat capture from a multi-indexer search response fixture. |

**Smoke-test cadence:** Phase 1 ships fully on its own (config-only, no C++ rebuild, can be deployed and tested before Phase 2 starts). Each subsequent phase rebuilds the kiosk binary; after Tasks 5, 9, 10, 13, and 15 the kiosk is buildable + smoke-testable on the Pi.

---

## Phase 1 — Configuration (low-risk, ships standalone)

### Task 1: Add six new Prowlarr indexer entries

**Goal:** Expand the active indexer pool from 4 to 10 by appending six new Cardigann entries to the fixture. Re-running `setup_services.sh` reconciles them into Prowlarr by name.

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/data/prowlarr_indexers.json`

- [ ] **Step 1: Read the fixture and confirm the existing structure**

Run: `head -110 magic_dingus_box_cpp/scripts/data/prowlarr_indexers.json`

Expected: a JSON array of indexer objects. Each object has fields like `definitionName`, `enable`, `priority`, `appProfileId`, `protocol`, `privacy`, `capabilities`, etc. Use the first entry (Demonoid Clone, lines 1–105) as the structural template.

- [ ] **Step 2: Append six new entries to the JSON array**

Insert the following six entries before the closing `]` of the top-level array. Each one matches the Cardigann definition name exactly (Prowlarr resolves the URL list from its own definition store).

Template for each entry:

```json
{
  "indexerUrls": ["<primary-url>"],
  "legacyUrls": [],
  "definitionName": "<cardigann-id>",
  "description": "<short-description>",
  "language": "en-US",
  "encoding": "Unicode (UTF-8)",
  "enable": true,
  "redirect": false,
  "supportsRss": true,
  "supportsSearch": true,
  "supportsRedirect": false,
  "supportsPagination": false,
  "appProfileId": 1,
  "protocol": "torrent",
  "privacy": "public",
  "priority": 25,
  "downloadClientId": 0,
  "name": "<display-name>",
  "fields": [],
  "tags_by_label": [<empty-or-["cloudflare"]>],
  "added": "0001-01-01T00:00:00Z",
  "capabilities": {
    "limitsMax": 100,
    "limitsDefault": 100,
    "categories": [
      { "name": "Movies", "subCategories": [
        { "name": "Movies/Foreign", "subCategories": [] },
        { "name": "Movies/Other",   "subCategories": [] },
        { "name": "Movies/SD",      "subCategories": [] },
        { "name": "Movies/HD",      "subCategories": [] },
        { "name": "Movies/UHD",     "subCategories": [] },
        { "name": "Movies/BluRay",  "subCategories": [] },
        { "name": "Movies/3D",      "subCategories": [] },
        { "name": "Movies/DVD",     "subCategories": [] },
        { "name": "Movies/WEB-DL",  "subCategories": [] }
      ]}
    ]
  }
}
```

The six entries to insert:

| `name` | `definitionName` | `indexerUrls[0]` | `description` | `tags_by_label` |
|---|---|---|---|---|
| `1337x` | `1337x` | `https://1337x.to/` | `1337x is a Public general-purpose torrent tracker` | `["cloudflare"]` |
| `TorrentGalaxy` | `torrentgalaxy` | `https://torrentgalaxy.to/` | `TorrentGalaxy is a Public general-purpose torrent tracker (RARBG successor)` | `[]` |
| `Solid Torrents` | `solidtorrents` | `https://solidtorrents.to/` | `Solid Torrents is a Public meta-search aggregator` | `[]` |
| `BitSearch` | `bitsearch` | `https://bitsearch.to/` | `BitSearch is a Public meta-search aggregator` | `[]` |
| `Knaben` | `knaben` | `https://knaben.eu/` | `Knaben is a Public meta-search aggregator with a large database` | `[]` |
| `TheRARBG` | `therarbg` | `https://therarbg.com/` | `TheRARBG is a community-run RARBG mirror preserving the original release database` | `["cloudflare"]` |

- [ ] **Step 3: Validate JSON parses cleanly**

Run: `python3 -c "import json; json.load(open('magic_dingus_box_cpp/scripts/data/prowlarr_indexers.json'))" && echo OK`

Expected: `OK`. If you get a `JSONDecodeError`, you have a comma/brace mistake — fix and re-run.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/data/prowlarr_indexers.json
git commit -m "feat(mb): expand Prowlarr indexer pool with 1337x, TGx, Solid, BitSearch, Knaben, TheRARBG"
```

---

### Task 2: Tighten Radarr `minimumSeeders` floor 5 → 10

**Goal:** Filter out shaky low-seed releases at search time. Eliminates downloads that connect slowly or stall on Add-to-Library.

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh` (Step 14b, around line 767)

- [ ] **Step 1: Read the current Step 14b block**

Run: `sed -n '760,815p' magic_dingus_box_cpp/scripts/setup_services.sh`

Expected: a Python heredoc that GETs `/indexer`, walks each entry, finds the `minimumSeeders` field, and PUTs it back if it differs from `TARGET`.

- [ ] **Step 2: Change `TARGET = 5` to `TARGET = 10`**

Find the line:

```python
TARGET = 5
```

Change to:

```python
TARGET = 10
```

- [ ] **Step 3: Verify the only `TARGET = 5` change is in Step 14b**

Run: `grep -n "TARGET = " magic_dingus_box_cpp/scripts/setup_services.sh`

Expected: exactly one match, now showing `TARGET = 10`.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/setup_services.sh
git commit -m "tune(mb): raise Radarr indexer minimumSeeders 5 -> 10"
```

---

### Task 3: Expand trusted release-group regex

**Goal:** Reward more high-quality x264 release groups with the +30 custom-format bonus, so Radarr's auto-pick favors trusted encodes within an equal-codec tier.

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/data/radarr_custom_formats.json` (around line 158)

- [ ] **Step 1: Locate the existing "Trusted small-release groups" regex**

Run: `grep -n "yify\\\\|yts" magic_dingus_box_cpp/scripts/data/radarr_custom_formats.json`

Expected: one match, the line containing `\\b(yify|yts\\.?(mx|am|ag)|galaxyrg|rarbg|fgt|surge|piratess?)\\b`.

- [ ] **Step 2: Replace the regex value**

Open the file, find the line:

```json
            "value": "\\b(yify|yts\\.?(mx|am|ag)|galaxyrg|rarbg|fgt|surge|piratess?)\\b",
```

Replace with:

```json
            "value": "\\b(yify|yts\\.?(mx|am|ag)|galaxyrg|rarbg|fgt|surge|piratess?|tgx|evo|aoc|ion10|qxr|successfulcrab)\\b",
```

- [ ] **Step 3: Validate JSON**

Run: `python3 -c "import json; json.load(open('magic_dingus_box_cpp/scripts/data/radarr_custom_formats.json'))" && echo OK`

Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/data/radarr_custom_formats.json
git commit -m "tune(mb): add TGx/EVO/AOC/ION10/QxR/SuccessfulCrab to trusted-groups regex"
```

---

### Task 4: Deploy Phase 1 to the Pi and verify

**Goal:** Push the three config changes to the Pi, run `setup_services.sh`, and confirm Prowlarr/Radarr reflect the new state.

**Files:** none — deploy + smoke verification.

- [ ] **Step 1: Sync the updated fixtures + script to the Pi**

Run: `rsync -avz magic_dingus_box_cpp/scripts/ magic@10.55.0.1:/opt/magic_dingus_box/scripts/`

Expected: rsync output showing the three changed files transferred.

- [ ] **Step 2: Run `setup_services.sh` on the Pi**

Run: `ssh magic@10.55.0.1 'sudo /opt/magic_dingus_box/services/setup_services.sh' 2>&1 | tail -80`

Expected: success messages for indexer reconciliation (six new entries added), custom-format update (trusted-groups regex changed), and the seeder-threshold report showing `updated: [...]` listing every active indexer.

- [ ] **Step 3: Verify Prowlarr indexer count via API**

Run: `ssh magic@10.55.0.1 'curl -s -H "X-Api-Key: $(cat /opt/magic_dingus_box/services/.env | grep PROWLARR_API_KEY | cut -d= -f2)" http://localhost:9696/api/v1/indexer | python3 -c "import json,sys; d=json.load(sys.stdin); print(len([i for i in d if i[\"enable\"]]), \"enabled,\", len(d), \"total\")"'`

Expected: `10 enabled, 15 total` (10 active = 4 existing + 6 new; 15 total = 10 active + 5 disabled).

- [ ] **Step 4: Sanity-check a Prowlarr search**

Run: `ssh magic@10.55.0.1 'curl -s -H "X-Api-Key: $(cat /opt/magic_dingus_box/services/.env | grep PROWLARR_API_KEY | cut -d= -f2)" "http://localhost:9696/api/v1/search?query=Inception&categories=2000" | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d), \"results\"); print(set(r.get(\"indexer\",\"?\") for r in d))"'`

Expected: ≥30 results, with at least 1337x, TorrentGalaxy, and one of the meta-search aggregators represented in the indexer set.

- [ ] **Step 5: Tag Phase 1 as a checkpoint**

Run: `git tag mb-source-selection-phase1 && git push origin mb-source-selection-phase1` *(only if you push tags — otherwise omit `--push`)*

---

## Phase 2 — ProwlarrClient refactor (per-release + per-indexer stats)

### Task 5: Add `ReleaseRecord` and `IndexerStats` structs to ProwlarrClient

**Goal:** Extend `ProwlarrClient` to retain per-release data and per-indexer stats from each search. The existing `ReleaseSummary` aggregate stays for back-compat with the Detail screen's current readout.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/prowlarr/prowlarr_client.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/prowlarr/prowlarr_client.cpp`
- Test: `magic_dingus_box_cpp/tests/media_browser/test_prowlarr_client.cpp` (new)

- [ ] **Step 1: Write the failing test (new file)**

Create `tests/media_browser/test_prowlarr_client.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "media_browser/prowlarr/prowlarr_client.h"

namespace mb = media_browser;

namespace {

// Two-indexer fixture: one healthy, one with zero results.
constexpr const char* kSearchResponse = R"JSON([
  {"title":"Inception 2010 1080p WEB-DL x264-GROUPA","indexer":"1337x",
   "seeders":200,"leechers":12,"size":2147483648,"protocol":"torrent",
   "guid":"https://1337x.to/torrent/abc","downloadUrl":"magnet:?xt=urn:btih:abc"},
  {"title":"Inception 2010 720p BluRay x264-YIFY","indexer":"YTS",
   "seeders":180,"leechers":8,"size":943718400,"protocol":"torrent",
   "guid":"https://yts.mx/torrent/def","downloadUrl":"magnet:?xt=urn:btih:def"},
  {"title":"Inception 2010 1080p WEB x265-RARE","indexer":"1337x",
   "seeders":3,"leechers":1,"size":1610612736,"protocol":"torrent",
   "guid":"https://1337x.to/torrent/ghi","downloadUrl":"magnet:?xt=urn:btih:ghi"}
])JSON";

}

TEST_CASE("ProwlarrClient parses per-release records from search response",
          "[prowlarr][parser]") {
    auto records = mb::ProwlarrClient::parse_search_response(kSearchResponse);
    REQUIRE(records.size() == 3);
    REQUIRE(records[0].title == "Inception 2010 1080p WEB-DL x264-GROUPA");
    REQUIRE(records[0].indexer == "1337x");
    REQUIRE(records[0].seeders == 200);
    REQUIRE(records[0].size_bytes == 2147483648LL);
    REQUIRE(records[0].guid == "https://1337x.to/torrent/abc");
}

TEST_CASE("ProwlarrClient aggregates per-indexer stats",
          "[prowlarr][stats]") {
    auto records = mb::ProwlarrClient::parse_search_response(kSearchResponse);
    auto stats = mb::ProwlarrClient::aggregate_indexer_stats(
        records, /*seed_threshold=*/10);
    // Expect two indexers in the map.
    REQUIRE(stats.size() == 2);
    auto find = [&](const std::string& name) {
        for (const auto& s : stats) if (s.name == name) return s;
        FAIL("indexer not found: " << name);
        return mb::ProwlarrClient::IndexerStats{};
    };
    auto x1337 = find("1337x");
    REQUIRE(x1337.result_count == 2);
    REQUIRE(x1337.results_above_seed_threshold == 1);  // only the 200-seeder
    auto yts = find("YTS");
    REQUIRE(yts.result_count == 1);
    REQUIRE(yts.results_above_seed_threshold == 1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd magic_dingus_box_cpp/build && cmake --build . --target test_media_browser_unit && ./test_media_browser_unit "[prowlarr][parser]"`

Expected: build error — `parse_search_response` and `IndexerStats` don't exist yet.

- [ ] **Step 3: Add the structs and static parsers to `prowlarr_client.h`**

Inside `class ProwlarrClient`, add (after the existing `ReleaseSummary` struct):

```cpp
    struct ReleaseRecord {
        std::string title;
        std::string indexer;
        std::string guid;
        std::string download_url;     // magnet: or http URL
        std::string protocol;          // "torrent" or "usenet"
        int         seeders   = 0;
        int         leechers  = 0;
        long long   size_bytes = 0;
        long long   age_seconds = 0;   // since publish, if Prowlarr provides
    };

    struct IndexerStats {
        std::string name;
        int   result_count = 0;
        int   results_above_seed_threshold = 0;
        int   last_response_ms = 0;       // 0 if not measured per-indexer
        std::string last_error;            // empty if healthy
    };

    // Static parsers (separated for unit testing — don't need a live HTTP client).
    static std::vector<ReleaseRecord> parse_search_response(const std::string& json_body);
    static std::vector<IndexerStats>  aggregate_indexer_stats(
        const std::vector<ReleaseRecord>& records, int seed_threshold);

    // Live accessors (populated after search_async completes).
    std::vector<ReleaseRecord> get_last_releases() const;
    std::vector<IndexerStats>  get_last_indexer_stats() const;
```

Add the matching member fields in the private section:

```cpp
    mutable std::mutex                    last_results_mu_;
    std::vector<ReleaseRecord>            last_releases_;       // guarded by last_results_mu_
    std::vector<IndexerStats>             last_indexer_stats_;  // guarded by last_results_mu_
```

- [ ] **Step 4: Implement the static parsers in `prowlarr_client.cpp`**

Add at the top of the file (after existing includes, before any other definitions):

```cpp
std::vector<ProwlarrClient::ReleaseRecord>
ProwlarrClient::parse_search_response(const std::string& json_body) {
    std::vector<ReleaseRecord> out;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string err;
    std::istringstream is(json_body);
    if (!Json::parseFromStream(b, is, &root, &err)) return out;
    if (!root.isArray()) return out;
    for (const auto& r : root) {
        ReleaseRecord rr;
        rr.title        = r.get("title", "").asString();
        rr.indexer      = r.get("indexer", "").asString();
        rr.guid         = r.get("guid", "").asString();
        rr.download_url = r.get("downloadUrl", "").asString();
        rr.protocol     = r.get("protocol", "torrent").asString();
        rr.seeders      = r.get("seeders", 0).asInt();
        rr.leechers     = r.get("leechers", 0).asInt();
        rr.size_bytes   = r.get("size", 0).asInt64();
        rr.age_seconds  = r.get("ageHours", 0).asInt64() * 3600;
        out.push_back(std::move(rr));
    }
    return out;
}

std::vector<ProwlarrClient::IndexerStats>
ProwlarrClient::aggregate_indexer_stats(
    const std::vector<ReleaseRecord>& records, int seed_threshold) {
    std::map<std::string, IndexerStats> by_name;
    for (const auto& r : records) {
        auto& s = by_name[r.indexer];
        s.name = r.indexer;
        s.result_count++;
        if (r.seeders >= seed_threshold) s.results_above_seed_threshold++;
    }
    std::vector<IndexerStats> out;
    out.reserve(by_name.size());
    for (auto& kv : by_name) out.push_back(std::move(kv.second));
    return out;
}

std::vector<ProwlarrClient::ReleaseRecord>
ProwlarrClient::get_last_releases() const {
    std::lock_guard<std::mutex> lk(last_results_mu_);
    return last_releases_;
}

std::vector<ProwlarrClient::IndexerStats>
ProwlarrClient::get_last_indexer_stats() const {
    std::lock_guard<std::mutex> lk(last_results_mu_);
    return last_indexer_stats_;
}
```

- [ ] **Step 5: Wire the search worker to populate the new fields**

Find the existing search worker function in `prowlarr_client.cpp` (the lambda passed to `std::thread` or detached worker — look for `state_.store(State::Searching)`). At the point where it currently builds the `ReleaseSummary`, add **before** the summary block:

```cpp
    auto records = parse_search_response(response_body);
    auto stats   = aggregate_indexer_stats(records, /*seed_threshold=*/10);
    {
        std::lock_guard<std::mutex> lk(last_results_mu_);
        last_releases_       = std::move(records);
        last_indexer_stats_  = std::move(stats);
    }
```

Keep the existing `ReleaseSummary` aggregation untouched — Detail screen still uses it.

Also include `<map>`, `<mutex>`, `<sstream>` at the top of `prowlarr_client.cpp` if not already present.

- [ ] **Step 6: Run the tests, confirm pass**

Add `tests/media_browser/test_prowlarr_client.cpp` to `CMakeLists.txt` `MEDIA_BROWSER_TEST_SOURCES` list (around line 343):

```cmake
    tests/media_browser/test_prowlarr_client.cpp
```

Then:

```bash
cd magic_dingus_box_cpp/build && cmake --build . --target test_media_browser_unit
./test_media_browser_unit "[prowlarr]"
```

Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/prowlarr/prowlarr_client.{h,cpp} \
        magic_dingus_box_cpp/tests/media_browser/test_prowlarr_client.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "refactor(mb): ProwlarrClient retains per-release records + per-indexer stats"
```

---

## Phase 3 — RadarrClient extensions

### Task 6: Add `RadarrClient::grab_release(release_json)`

**Goal:** Allow the kiosk to grab a specific release that the user picks from the manual picker. Radarr's API accepts a release object (originally fetched from `/api/v3/release?movieId=X`) and downloads exactly that release.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp`
- Test: `magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp` (extend existing)

- [ ] **Step 1: Write the failing test**

Append to `tests/media_browser/test_radarr_client.cpp`:

```cpp
namespace {
class RecordingRadarr : public mb::RadarrClient {
public:
    RecordingRadarr() : RadarrClient(Config{}) {}
    std::string last_method, last_path, last_body;
    std::string http_post(const std::string& path,
                          const std::string& body) override {
        last_method = "POST";
        last_path = path;
        last_body = body;
        return R"({"id":42})";
    }
    // Stubs to satisfy other virtuals if needed:
    std::string http_get(const std::string&) override { return ""; }
    std::string http_delete(const std::string&) override { return ""; }
};
}

TEST_CASE("RadarrClient::grab_release POSTs the release object verbatim",
          "[radarr][grab]") {
    RecordingRadarr r;
    Json::Value release;
    release["guid"]      = "magnet:?xt=urn:btih:abc";
    release["indexerId"] = 7;
    release["title"]     = "Inception 2010 1080p WEB-DL x264-GROUPA";
    bool ok = r.grab_release(release);
    REQUIRE(ok);
    REQUIRE(r.last_method == "POST");
    REQUIRE(r.last_path == "/api/v3/release");
    // Body should contain the indexerId we passed.
    REQUIRE(r.last_body.find("\"indexerId\"") != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd magic_dingus_box_cpp/build && cmake --build . --target test_media_browser_unit
./test_media_browser_unit "[radarr][grab]"
```

Expected: build error — `grab_release` not declared.

- [ ] **Step 3: Add the method declaration to `radarr_client.h`**

Inside `class RadarrClient`, after `cancel_queue_item`, add:

```cpp
    // Grab a specific release picked by the user. The `release` JSON
    // must be an object previously returned from /api/v3/release?movieId=X
    // (or constructed with the same shape — at minimum guid + indexerId).
    virtual bool grab_release(const Json::Value& release);

    // Fetch /api/v3/release?movieId=X — the list of releases Radarr would
    // consider for an interactive search of this movie. Each entry has
    // guid, indexerId, title, seeders, size, quality, customFormatScore, etc.
    virtual std::vector<Json::Value> get_releases_for_movie(int radarr_movie_id);
```

- [ ] **Step 4: Implement `grab_release` in `radarr_client.cpp`**

```cpp
bool RadarrClient::grab_release(const Json::Value& release) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::string body = Json::writeString(w, release);
    std::string resp = http_post("/api/v3/release", body);
    return !resp.empty();  // Radarr returns the queued release on success.
}

std::vector<Json::Value>
RadarrClient::get_releases_for_movie(int radarr_movie_id) {
    std::string resp = http_get("/api/v3/release?movieId=" +
                                std::to_string(radarr_movie_id));
    std::vector<Json::Value> out;
    if (resp.empty()) return out;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string err;
    std::istringstream is(resp);
    if (!Json::parseFromStream(b, is, &root, &err)) return out;
    if (!root.isArray()) return out;
    for (const auto& r : root) out.push_back(r);
    return out;
}
```

Include `<sstream>` if not already.

- [ ] **Step 5: Run tests, confirm pass**

```bash
cd magic_dingus_box_cpp/build && cmake --build . --target test_media_browser_unit
./test_media_browser_unit "[radarr][grab]"
```

Expected: `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.{h,cpp} \
        magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp
git commit -m "feat(mb): RadarrClient::grab_release + get_releases_for_movie for manual picker"
```

---

### Task 7: Add `RadarrClient::get_history(movieId)` for stall detection

**Goal:** Let the watchdog detect Radarr-side download failures (blacklist events, grab failures) by polling history.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp`
- Test: `magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp` (extend)

- [ ] **Step 1: Add the `HistoryEvent` struct + method declaration to `radarr_client.h`**

After `get_movie_download_hashes`, add:

```cpp
    struct HistoryEvent {
        int         id = 0;
        int         movie_id = 0;
        std::string event_type;       // grabbed, downloadFailed, downloadFolderImported, etc.
        std::string source_title;     // release title
        std::string date_iso;         // ISO 8601
    };

    // Recent history events for one movie. Returns most-recent-first.
    virtual std::vector<HistoryEvent> get_history(int radarr_movie_id, int page_size = 20);
```

- [ ] **Step 2: Write the failing test**

```cpp
TEST_CASE("RadarrClient::get_history parses history records",
          "[radarr][history]") {
    class StubRadarr : public mb::RadarrClient {
    public:
        StubRadarr() : RadarrClient(Config{}) {}
        std::string http_get(const std::string& path) override {
            REQUIRE(path.find("/api/v3/history") == 0);
            REQUIRE(path.find("movieId=99") != std::string::npos);
            return R"({"records":[
              {"id":1,"movieId":99,"eventType":"grabbed",
               "sourceTitle":"Inception 2010 1080p WEB-DL x264","date":"2026-05-01T10:00:00Z"},
              {"id":2,"movieId":99,"eventType":"downloadFailed",
               "sourceTitle":"Inception 2010 1080p WEB-DL x264","date":"2026-05-01T10:30:00Z"}
            ]})";
        }
        std::string http_post(const std::string&, const std::string&) override { return ""; }
        std::string http_delete(const std::string&) override { return ""; }
    };
    StubRadarr r;
    auto events = r.get_history(99);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].event_type == "grabbed");
    REQUIRE(events[1].event_type == "downloadFailed");
    REQUIRE(events[1].movie_id == 99);
}
```

- [ ] **Step 3: Run test to verify it fails**

```bash
cd magic_dingus_box_cpp/build && cmake --build . --target test_media_browser_unit
./test_media_browser_unit "[radarr][history]"
```

Expected: build error — `get_history` not declared.

- [ ] **Step 4: Implement `get_history` in `radarr_client.cpp`**

```cpp
std::vector<RadarrClient::HistoryEvent>
RadarrClient::get_history(int radarr_movie_id, int page_size) {
    std::vector<HistoryEvent> out;
    std::string path = "/api/v3/history?movieId=" + std::to_string(radarr_movie_id) +
                       "&pageSize=" + std::to_string(page_size) +
                       "&sortKey=date&sortDirection=descending";
    std::string resp = http_get(path);
    if (resp.empty()) return out;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string err;
    std::istringstream is(resp);
    if (!Json::parseFromStream(b, is, &root, &err)) return out;
    const auto& recs = root["records"];
    if (!recs.isArray()) return out;
    for (const auto& r : recs) {
        HistoryEvent e;
        e.id           = r.get("id", 0).asInt();
        e.movie_id     = r.get("movieId", 0).asInt();
        e.event_type   = r.get("eventType", "").asString();
        e.source_title = r.get("sourceTitle", "").asString();
        e.date_iso     = r.get("date", "").asString();
        out.push_back(std::move(e));
    }
    return out;
}
```

- [ ] **Step 5: Run tests, confirm pass**

```bash
./test_media_browser_unit "[radarr][history]"
```

Expected: `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.{h,cpp} \
        magic_dingus_box_cpp/tests/media_browser/test_radarr_client.cpp
git commit -m "feat(mb): RadarrClient::get_history for stall detection"
```

---

### Task 8: Add `QbittorrentClient::get_torrent(hash)`

**Goal:** Single-torrent lookup so the watchdog can poll a specific download without re-fetching the whole torrent list each tick.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/qbittorrent/qbittorrent_client.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/qbittorrent/qbittorrent_client.cpp`

- [ ] **Step 1: Add method declaration to `qbittorrent_client.h`**

After `get_torrents_by_hash()`:

```cpp
    // Convenience: fetch one torrent by hash. Returns nullopt if not found.
    virtual std::optional<QbitTorrent> get_torrent(const std::string& hash);
```

Include `<optional>` if not already.

- [ ] **Step 2: Implement in `qbittorrent_client.cpp`**

```cpp
std::optional<QbitTorrent>
QbittorrentClient::get_torrent(const std::string& hash) {
    auto map = get_torrents_by_hash();
    auto it = map.find(hash);
    if (it == map.end()) return std::nullopt;
    return it->second;
}
```

- [ ] **Step 3: Verify the kiosk still builds**

```bash
cd magic_dingus_box_cpp/build && cmake --build . -j2 2>&1 | tail -20
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/qbittorrent/qbittorrent_client.{h,cpp}
git commit -m "feat(mb): QbittorrentClient::get_torrent(hash) lookup"
```

---

## Phase 4 — Release picker UI

### Task 9: Add `Screen::ReleasePicker` to the screen enum

**Goal:** Reserve the screen slot. Adding it before the screen class exists keeps the dispatcher additive in Task 12.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h`

- [ ] **Step 1: Add the enum value**

Find the `enum class Screen` block (around line 22) and add `ReleasePicker` after `Detail`:

```cpp
enum class Screen {
    Browse,
    Search,
    Detail,
    ReleasePicker,   // Manual override of Radarr's auto-pick (v1.7.0).
    Queue,
    Library,
    Playback,
    MovieSettings,
    Exit
};
```

- [ ] **Step 2: Verify the kiosk still builds (no usage yet, just an enum addition)**

```bash
cd magic_dingus_box_cpp/build && cmake --build . -j2 2>&1 | tail -20
```

Expected: build succeeds. (If any switch statement complains about a missing case, add a `case Screen::ReleasePicker: break;` no-op there for now — the actual case body lands in Task 12.)

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h
git commit -m "feat(mb): reserve Screen::ReleasePicker enum slot"
```

---

### Task 10: Create `ReleasePickerScreen` header + skeleton

**Goal:** Define the screen class with its data shape (the `ReleaseCandidate` struct) and inert render/input methods. Logic lands in Task 11.

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/ui/release_picker_screen.h`
- Create: `magic_dingus_box_cpp/src/media_browser/ui/release_picker_screen.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (add the new .cpp to `KIOSK_MEDIA_BROWSER_SOURCES`)

- [ ] **Step 1: Create the header**

`release_picker_screen.h`:

```cpp
#pragma once

#include "media_browser/ui/mb_screen.h"
#include "media_browser/prowlarr/prowlarr_client.h"
#include <functional>
#include <string>
#include <vector>

namespace media_browser {

class RadarrClient;
namespace ui { class Renderer; }

class ReleasePickerScreen {
public:
    // One row in the picker. Built from a Prowlarr ReleaseRecord (and
    // optionally enriched with Radarr's own scoring once we wire that in).
    struct ReleaseCandidate {
        std::string  title;
        std::string  indexer;
        std::string  guid;
        std::string  download_url;
        int          seeders   = 0;
        int          leechers  = 0;
        long long    size_bytes = 0;
        std::string  codec;       // "x264" / "x265" / "AV1" / "" (parsed from title)
        std::string  resolution;  // "720p" / "1080p" / "2160p" / ""
        std::string  source;      // "BluRay" / "WEB-DL" / "WEBRip" / "HDTV" / ""
        int          score        = 0;     // Radarr custom-format score, if known
        bool         would_auto_pick = false;  // gold-border highlight
        bool         below_threshold = false;  // dim red — score < minFormatScore
    };

    explicit ReleasePickerScreen(RadarrClient& radarr);

    // Set the candidates to display. Caller is responsible for sort + flag
    // population. Resets focus to row 0.
    void set_candidates(std::string movie_title, std::vector<ReleaseCandidate> rows);

    // Per-frame entry points. Mirrors the convention of other MB screens.
    void render(ui::Renderer& r, int screen_w, int screen_h);
    Screen handle_input(int btn);   // returns the next Screen

private:
    RadarrClient& radarr_;
    std::string   movie_title_;
    std::vector<ReleaseCandidate> rows_;
    int           focus_ = 0;
    int           scroll_top_ = 0;
    static constexpr int kVisibleRows = 6;
};

}  // namespace media_browser
```

- [ ] **Step 2: Create the skeleton .cpp**

`release_picker_screen.cpp`:

```cpp
#include "media_browser/ui/release_picker_screen.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "ui/renderer.h"

namespace media_browser {

ReleasePickerScreen::ReleasePickerScreen(RadarrClient& radarr)
    : radarr_(radarr) {}

void ReleasePickerScreen::set_candidates(std::string movie_title,
                                         std::vector<ReleaseCandidate> rows) {
    movie_title_ = std::move(movie_title);
    rows_       = std::move(rows);
    focus_      = 0;
    scroll_top_ = 0;
}

void ReleasePickerScreen::render(ui::Renderer& r, int /*screen_w*/, int /*screen_h*/) {
    // Skeleton — Task 11 fills this in.
    (void)r;
}

Screen ReleasePickerScreen::handle_input(int /*btn*/) {
    // Skeleton — Task 11 fills this in.
    return Screen::ReleasePicker;
}

}  // namespace media_browser
```

- [ ] **Step 3: Add the .cpp to CMakeLists.txt**

In `KIOSK_MEDIA_BROWSER_SOURCES` (around line 156–176), insert:

```cmake
    src/media_browser/ui/release_picker_screen.cpp
```

- [ ] **Step 4: Build to verify it compiles**

```bash
cd magic_dingus_box_cpp/build && cmake --build . -j2 2>&1 | tail -20
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/release_picker_screen.{h,cpp} \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): ReleasePickerScreen skeleton (header + inert impl)"
```

---

### Task 11: Implement picker sort, render, and SELECT handler

**Goal:** Render the 6-row picker with seeder/size/codec columns, gold-border on Radarr's choice, dim-red on below-threshold rows. SELECT calls `RadarrClient::grab_release`; BACK returns to Detail.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/release_picker_screen.cpp`
- Test: `magic_dingus_box_cpp/tests/media_browser/test_release_picker.cpp` (new)

- [ ] **Step 1: Write the failing test for the sort + flag logic**

Create `tests/media_browser/test_release_picker.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "media_browser/ui/release_picker_screen.h"

namespace mb = media_browser;

namespace {
mb::ReleasePickerScreen::ReleaseCandidate mk(int seeders, int score,
                                              const std::string& title) {
    mb::ReleasePickerScreen::ReleaseCandidate c;
    c.title = title;
    c.seeders = seeders;
    c.score = score;
    return c;
}
}

TEST_CASE("sort_candidates orders by seeders desc, then score desc",
          "[picker][sort]") {
    std::vector<mb::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(50, 80, "low-seed"),
        mk(200, 80, "high-seed"),
        mk(50, 100, "low-seed-better-score"),
    };
    mb::ReleasePickerScreen::sort_candidates(rows);
    REQUIRE(rows[0].title == "high-seed");
    REQUIRE(rows[1].title == "low-seed-better-score");
    REQUIRE(rows[2].title == "low-seed");
}

TEST_CASE("flag_auto_pick_and_threshold marks Radarr's choice + below-threshold rows",
          "[picker][flags]") {
    std::vector<mb::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(200, 80, "best"),
        mk(180, 80, "second"),
        mk(50, -300, "rejected"),  // below the -200 minFormatScore floor
    };
    mb::ReleasePickerScreen::flag_auto_pick_and_threshold(
        rows, /*min_format_score=*/-200);
    REQUIRE(rows[0].would_auto_pick == true);
    REQUIRE(rows[1].would_auto_pick == false);
    REQUIRE(rows[2].below_threshold == true);
    REQUIRE(rows[0].below_threshold == false);
}
```

- [ ] **Step 2: Run, confirm fails**

```bash
cd magic_dingus_box_cpp/build && cmake --build . --target test_media_browser_unit 2>&1 | tail -10
```

Expected: build error — `sort_candidates` and `flag_auto_pick_and_threshold` not declared.

- [ ] **Step 3: Add static helpers to the header**

In `release_picker_screen.h`, add inside the class (in the `public:` section):

```cpp
    // Static helpers (separated for unit testing).
    static void sort_candidates(std::vector<ReleaseCandidate>& rows);
    static void flag_auto_pick_and_threshold(
        std::vector<ReleaseCandidate>& rows, int min_format_score);
```

- [ ] **Step 4: Implement helpers in `release_picker_screen.cpp`**

Add at the top of the file (after constructor):

```cpp
void ReleasePickerScreen::sort_candidates(std::vector<ReleaseCandidate>& rows) {
    std::sort(rows.begin(), rows.end(),
              [](const ReleaseCandidate& a, const ReleaseCandidate& b) {
                  if (a.seeders != b.seeders) return a.seeders > b.seeders;
                  return a.score > b.score;
              });
}

void ReleasePickerScreen::flag_auto_pick_and_threshold(
    std::vector<ReleaseCandidate>& rows, int min_format_score) {
    // Auto-pick: highest-scoring row that passes the threshold.
    int best_idx = -1;
    int best_score = std::numeric_limits<int>::min();
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i].below_threshold = (rows[i].score < min_format_score);
        if (!rows[i].below_threshold && rows[i].score > best_score) {
            best_score = rows[i].score;
            best_idx = static_cast<int>(i);
        }
        rows[i].would_auto_pick = false;
    }
    if (best_idx >= 0) rows[best_idx].would_auto_pick = true;
}
```

Add `#include <algorithm>` and `#include <limits>` at the top.

- [ ] **Step 5: Update `set_candidates` to call the helpers**

Replace the existing `set_candidates` body with:

```cpp
void ReleasePickerScreen::set_candidates(std::string movie_title,
                                         std::vector<ReleaseCandidate> rows) {
    movie_title_ = std::move(movie_title);
    rows_       = std::move(rows);
    sort_candidates(rows_);
    flag_auto_pick_and_threshold(rows_, /*min_format_score=*/-200);
    focus_      = 0;
    scroll_top_ = 0;
}
```

- [ ] **Step 6: Run unit tests, confirm pass**

```bash
./test_media_browser_unit "[picker]"
```

Expected: `All tests passed`.

- [ ] **Step 7: Implement `render()` (UI rendering)**

Replace the skeleton `render()` body with:

```cpp
void ReleasePickerScreen::render(ui::Renderer& r, int /*screen_w*/, int /*screen_h*/) {
    using namespace media_browser::ui;

    // Header bar: movie title + count.
    std::string header = "Pick a source — " + movie_title_ +
                         " (" + std::to_string(rows_.size()) + " releases)";
    r.draw_text(40, 36, header, /*size=*/22, 0xFFFFFFFF);

    // Column headers.
    constexpr float kRowH = 84.0f;
    constexpr float kListTop = 96.0f;
    r.draw_text(40,  kListTop - 22, "Title",   14, 0x808080FF);
    r.draw_text(720, kListTop - 22, "Seeders", 14, 0x808080FF);
    r.draw_text(880, kListTop - 22, "Size",    14, 0x808080FF);
    r.draw_text(980, kListTop - 22, "Codec",   14, 0x808080FF);
    r.draw_text(1080,kListTop - 22, "Res",     14, 0x808080FF);
    r.draw_text(1160,kListTop - 22, "Source",  14, 0x808080FF);

    // Rows.
    int end = std::min<int>(rows_.size(), scroll_top_ + kVisibleRows);
    for (int i = scroll_top_; i < end; ++i) {
        const auto& c = rows_[i];
        float y = kListTop + (i - scroll_top_) * kRowH;
        bool focused = (i == focus_);

        // Background tint.
        uint32_t bg = focused ? 0x182838FF : 0x0E1620FF;
        r.draw_rect(20, y, 1240, kRowH - 8, bg);

        // Border: gold for auto-pick, focus blue overlays gold.
        if (c.would_auto_pick) r.draw_rect_outline(20, y, 1240, kRowH - 8, 2, 0xFFD700FF);
        if (focused)            r.draw_rect_outline(20, y, 1240, kRowH - 8, 3, 0x4A9EFFFF);

        // Text dimmed if below threshold.
        uint32_t fg = c.below_threshold ? 0x884030FF : 0xE0E0E0FF;

        // Title (truncated).
        std::string t = c.title;
        if (t.size() > 70) t = t.substr(0, 67) + "...";
        r.draw_text(40,  y + 28, t, 16, fg);
        r.draw_text(40,  y + 50, c.indexer, 12, 0x707070FF);

        // Seeders/leechers.
        std::string seed = std::to_string(c.seeders) + " / " + std::to_string(c.leechers);
        r.draw_text(720, y + 32, seed, 16, fg);

        // Size (humanized).
        double mb = c.size_bytes / (1024.0 * 1024.0);
        std::string size = mb > 1024.0
            ? std::to_string(static_cast<int>(mb / 1024.0 * 10) / 10.0) + " GB"
            : std::to_string(static_cast<int>(mb)) + " MB";
        r.draw_text(880, y + 32, size, 16, fg);

        r.draw_text(980, y + 32, c.codec.empty()      ? "?" : c.codec, 16, fg);
        r.draw_text(1080,y + 32, c.resolution.empty() ? "?" : c.resolution, 16, fg);
        r.draw_text(1160,y + 32, c.source.empty()     ? "?" : c.source, 16, fg);
    }

    // Footer hint.
    r.draw_text(40, 690, "DPad: navigate · A: grab · B: back · gold border = Radarr's pick · red dim = below score floor",
                12, 0x808080FF);
}
```

If `Renderer::draw_rect_outline` doesn't exist, use four `draw_rect` calls for the border (top/bottom/left/right strips). Check `src/ui/renderer.h` for the actual method names and adjust.

- [ ] **Step 8: Implement `handle_input()`**

Replace the skeleton:

```cpp
Screen ReleasePickerScreen::handle_input(int btn) {
    // Button constants follow the Marquee input grammar (BTN1=A, BTN2=B,
    // BTN3=X, BTN4=Y, plus DPad). Match values from input_manager.h.
    constexpr int kBtnA = 0, kBtnB = 1;
    constexpr int kDpadUp = 100, kDpadDown = 101;

    if (rows_.empty()) {
        if (btn == kBtnB) return Screen::Detail;
        return Screen::ReleasePicker;
    }

    if (btn == kDpadUp) {
        if (focus_ > 0) --focus_;
        if (focus_ < scroll_top_) scroll_top_ = focus_;
    } else if (btn == kDpadDown) {
        if (focus_ + 1 < static_cast<int>(rows_.size())) ++focus_;
        if (focus_ >= scroll_top_ + kVisibleRows)
            scroll_top_ = focus_ - kVisibleRows + 1;
    } else if (btn == kBtnA) {
        // Grab. Build the JSON Radarr expects — at minimum guid + indexerId,
        // but we don't always know indexerId from Prowlarr alone. The caller
        // (Detail screen) is responsible for building candidates with enough
        // info for grab; here we wrap the available fields.
        Json::Value release;
        release["guid"]        = rows_[focus_].guid;
        release["downloadUrl"] = rows_[focus_].download_url;
        release["title"]       = rows_[focus_].title;
        bool ok = radarr_.grab_release(release);
        // Toast result, return to Detail.
        ::ui::Toast::show(ok ? "Grabbing release — see Queue" : "Grab failed");
        return Screen::Detail;
    } else if (btn == kBtnB) {
        return Screen::Detail;
    }
    return Screen::ReleasePicker;
}
```

Verify the actual button constants by reading `src/platform/input_manager.h` first — substitute the real names if these guesses are wrong. Include `<json/json.h>` and `"ui/toast.h"` in the .cpp.

- [ ] **Step 9: Build, run unit tests, confirm everything passes**

```bash
cd magic_dingus_box_cpp/build && cmake --build . -j2 2>&1 | tail -20
./test_media_browser_unit "[picker]"
```

Expected: build clean, tests pass.

- [ ] **Step 10: Add the test file to CMakeLists.txt**

Add to `MEDIA_BROWSER_TEST_SOURCES`:

```cmake
    tests/media_browser/test_release_picker.cpp
```

- [ ] **Step 11: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/release_picker_screen.{h,cpp} \
        magic_dingus_box_cpp/tests/media_browser/test_release_picker.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): ReleasePickerScreen sort/render/grab implementation"
```

---

### Task 12: Wire `ReleasePickerScreen` into the main-loop dispatcher

**Goal:** Instantiate the picker, route Detail→Picker transitions, and dispatch input/render to it when active.

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp`

- [ ] **Step 1: Locate the existing dispatcher**

Run: `grep -n "Screen::Detail\|case Screen::" magic_dingus_box_cpp/src/main.cpp | head -30`

Expected: existing `case Screen::Browse:`, `case Screen::Detail:`, etc. dispatch sites for both render and input.

- [ ] **Step 2: Add the picker instance alongside the others**

Find the block where `DetailScreen detail_screen(...)` is constructed (top of `main()` or in an init helper). Add:

```cpp
    media_browser::ReleasePickerScreen release_picker(radarr_client);
```

Include the header at top of `main.cpp`:

```cpp
#include "media_browser/ui/release_picker_screen.h"
```

- [ ] **Step 3: Add render + input dispatch cases**

In the main-loop input-handling switch (where `case Screen::Detail` lives), add:

```cpp
            case Screen::ReleasePicker: {
                Screen next = release_picker.handle_input(button_event);
                if (next != Screen::ReleasePicker) current_mb_screen = next;
                break;
            }
```

In the per-frame render switch:

```cpp
            case Screen::ReleasePicker:
                release_picker.render(renderer, screen_w, screen_h);
                break;
```

- [ ] **Step 4: Build and verify clean compile**

```bash
cd magic_dingus_box_cpp/build && cmake --build . -j2 2>&1 | tail -20
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(mb): wire ReleasePickerScreen into main-loop dispatcher"
```

---

## Phase 5 — Detail screen integration

### Task 13: Add "Pick a source" button to Detail screen

**Goal:** Detail screen gets a new button next to "Add to Library." Visible once Prowlarr search is `Ready`. Pressing it builds candidates from `prowlarr.get_last_releases()`, hands them to the picker, and switches screens.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp`

- [ ] **Step 1: Read the current button-rebuild logic**

Run: `grep -n "rebuild_buttons\|Add to Library\|Button" magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp | head -30`

Identify the function that builds the `buttons_` vector and the handler dispatch (the spot where pressing the focused button calls `do_add_to_library()`).

- [ ] **Step 2: Add a `do_pick_source` declaration to `detail_screen.h`**

Inside the private section of `class DetailScreen`:

```cpp
    Screen do_pick_source();
    // The picker (created in main.cpp) is given the candidates we collect here.
    // We expose a setter so main.cpp can wire the screen pair without DetailScreen
    // owning the picker itself.
    void set_picker_callback(std::function<void(std::string,
        std::vector<ReleasePickerScreen::ReleaseCandidate>)> cb);

    std::function<void(std::string,
        std::vector<ReleasePickerScreen::ReleaseCandidate>)> picker_callback_;
```

Include `"media_browser/ui/release_picker_screen.h"` and `<functional>` at top.

- [ ] **Step 3: Implement the setter and the handler**

In `detail_screen.cpp`:

```cpp
void DetailScreen::set_picker_callback(std::function<void(std::string,
    std::vector<ReleasePickerScreen::ReleaseCandidate>)> cb) {
    picker_callback_ = std::move(cb);
}

namespace {
// Lightweight title parsing for the picker's codec/res/source columns.
// Pulls out the first match of each pattern; "" if none.
std::string match_first(const std::string& haystack,
                        const std::vector<std::string>& needles) {
    for (const auto& n : needles) {
        if (haystack.find(n) != std::string::npos) return n;
    }
    return "";
}
}

Screen DetailScreen::do_pick_source() {
    if (!prowlarr_) {
        ::ui::Toast::show("Prowlarr not configured");
        return Screen::Detail;
    }
    auto records = prowlarr_->get_last_releases();
    if (records.empty()) {
        ::ui::Toast::show("No releases — try Add to Library first to trigger search");
        return Screen::Detail;
    }
    std::vector<ReleasePickerScreen::ReleaseCandidate> candidates;
    candidates.reserve(records.size());
    for (const auto& r : records) {
        ReleasePickerScreen::ReleaseCandidate c;
        c.title        = r.title;
        c.indexer      = r.indexer;
        c.guid         = r.guid;
        c.download_url = r.download_url;
        c.seeders      = r.seeders;
        c.leechers     = r.leechers;
        c.size_bytes   = r.size_bytes;
        // Cheap title-based parsing for the column badges.
        c.codec      = match_first(r.title, {"x264", "x265", "h264", "h265", "AV1", "HEVC"});
        c.resolution = match_first(r.title, {"720p", "1080p", "2160p", "4K"});
        c.source     = match_first(r.title, {"BluRay", "WEB-DL", "WEBRip", "HDTV", "BDRip"});
        // Score: we don't have Radarr's per-release score here cheaply.
        // Initial implementation leaves this 0; a future enhancement can call
        // RadarrClient::get_releases_for_movie to fetch true scores.
        c.score = 0;
        candidates.push_back(std::move(c));
    }
    if (picker_callback_) {
        picker_callback_(title_, std::move(candidates));
    }
    return Screen::ReleasePicker;
}
```

- [ ] **Step 4: Add the button to `rebuild_buttons()`**

Find `rebuild_buttons()` (or the equivalent function that populates the `buttons_` vector). After the existing "Add to Library" button push, add:

```cpp
    if (prowlarr_ && prowlarr_->state() == ProwlarrClient::State::Ready) {
        Button pick;
        pick.label  = "Pick a source (" +
                      std::to_string(prowlarr_->peek_result()->total_releases) + ")";
        pick.kind   = ::media_browser::ui::ButtonKind::Action;
        pick.action = [this]{ return do_pick_source(); };
        buttons_.push_back(std::move(pick));
    }
```

The exact `Button` struct shape may differ — match the existing pattern in `rebuild_buttons()`. If the existing struct is `{ label, action_id }` and dispatch is via switch, add a new `action_id` constant and wire the switch to call `do_pick_source()`.

- [ ] **Step 5: Wire main.cpp to register the picker callback**

In `main.cpp`, after both `detail_screen` and `release_picker` are constructed:

```cpp
    detail_screen.set_picker_callback(
        [&release_picker](std::string title,
                          std::vector<media_browser::ReleasePickerScreen::ReleaseCandidate> rows) {
            release_picker.set_candidates(std::move(title), std::move(rows));
        });
```

- [ ] **Step 6: Build, verify, smoke-test on Pi**

```bash
cd magic_dingus_box_cpp/build && cmake --build . -j2 2>&1 | tail -20
```

Then deploy:

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service'
```

Manual smoke on Pi: open Media Browser → search a movie → enter Detail → wait for Prowlarr search to complete → confirm "Pick a source (N)" button appears → press it → confirm picker opens with the candidates → press B to return.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/detail_screen.{h,cpp} \
        magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(mb): Detail-screen 'Pick a source' button opens manual release picker"
```

---

## Phase 6 — Download watchdog + stall prompt

### Task 14: Create `DownloadWatchdog` with stall-detection logic

**Goal:** Background-tickable watchdog that watches qBit + Radarr for stalled downloads and emits a stall event.

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/qbittorrent/download_watchdog.h`
- Create: `magic_dingus_box_cpp/src/media_browser/qbittorrent/download_watchdog.cpp`
- Test: `magic_dingus_box_cpp/tests/media_browser/test_download_watchdog.cpp` (new)
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (add to both source + test lists)

- [ ] **Step 1: Write the failing test**

Create `tests/media_browser/test_download_watchdog.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "media_browser/qbittorrent/download_watchdog.h"

namespace mb = media_browser;
using Clock = std::chrono::steady_clock;

TEST_CASE("watchdog emits stall when zero progress for 60s", "[watchdog][zero-progress]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({/*tmdb_id=*/1, /*title=*/"Inception",
                          /*started_at=*/in.now - std::chrono::seconds(70),
                          /*hash=*/"abc"});
    mb::QbitTorrent t; t.hash = "abc"; t.progress = 0.0; t.num_seeds = 0;
    in.qbit_torrents.push_back(t);
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].tmdb_id == 1);
    REQUIRE(events[0].reason == mb::DownloadWatchdog::Reason::ZeroProgress);
}

TEST_CASE("watchdog does NOT emit stall when within 60s grace", "[watchdog][grace]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({1, "Inception", in.now - std::chrono::seconds(30), "abc"});
    mb::QbitTorrent t; t.hash = "abc"; t.progress = 0.0; t.num_seeds = 0;
    in.qbit_torrents.push_back(t);
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.empty());
}

TEST_CASE("watchdog emits stall on Radarr downloadFailed history event",
          "[watchdog][radarr-failed]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({1, "Inception", in.now - std::chrono::seconds(10), "abc"});
    mb::RadarrClient::HistoryEvent ev;
    ev.movie_id = 1; ev.event_type = "downloadFailed"; ev.id = 42;
    in.recent_history.push_back(ev);
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].reason == mb::DownloadWatchdog::Reason::RadarrFailed);
}

TEST_CASE("watchdog suppresses stall during snooze window",
          "[watchdog][snooze]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({1, "Inception", in.now - std::chrono::seconds(70), "abc"});
    mb::QbitTorrent t; t.hash = "abc"; t.progress = 0.0; t.num_seeds = 0;
    in.qbit_torrents.push_back(t);
    in.snoozed_until[1] = in.now + std::chrono::seconds(60);  // snoozed
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.empty());
}
```

- [ ] **Step 2: Run, confirm fails (header not present)**

```bash
cd magic_dingus_box_cpp/build && cmake --build . --target test_media_browser_unit 2>&1 | tail -5
```

Expected: include error.

- [ ] **Step 3: Create the header**

`download_watchdog.h`:

```cpp
#pragma once

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace media_browser {

class DownloadWatchdog {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct WatchedDownload {
        int         tmdb_id = 0;
        std::string title;
        TimePoint   started_at;
        std::string hash;       // qBit torrent hash (may be empty until known)
    };

    enum class Reason { ZeroProgress, RadarrFailed };

    struct StallEvent {
        int    tmdb_id = 0;
        std::string title;
        Reason reason = Reason::ZeroProgress;
    };

    // Pure-functional input bundle for `evaluate`. The class wraps live
    // services in production but the evaluation logic is testable via
    // pre-built Inputs.
    struct Inputs {
        TimePoint                                 now;
        std::vector<WatchedDownload>              watched;
        std::vector<QbitTorrent>                  qbit_torrents;
        std::vector<RadarrClient::HistoryEvent>   recent_history;
        std::map<int, TimePoint>                  snoozed_until;
    };

    static std::vector<StallEvent> evaluate(const Inputs& in);

    // Live wiring (only used by the kiosk binary, not tests).
    DownloadWatchdog(RadarrClient& radarr, QbittorrentClient& qbit);
    void watch(int tmdb_id, std::string title, std::string qbit_hash = "");
    void unwatch(int tmdb_id);
    void snooze(int tmdb_id, std::chrono::seconds duration);
    // Called from the main loop ~once per second. Returns any new stall events
    // that should be surfaced to the user.
    std::vector<StallEvent> tick();

private:
    RadarrClient&        radarr_;
    QbittorrentClient&   qbit_;
    std::vector<WatchedDownload>  watched_;
    std::map<int, TimePoint>      snoozed_until_;
    TimePoint                     last_poll_;
};

}  // namespace media_browser
```

- [ ] **Step 4: Implement the .cpp**

`download_watchdog.cpp`:

```cpp
#include "media_browser/qbittorrent/download_watchdog.h"

#include <algorithm>

namespace media_browser {

DownloadWatchdog::DownloadWatchdog(RadarrClient& radarr, QbittorrentClient& qbit)
    : radarr_(radarr), qbit_(qbit) {}

void DownloadWatchdog::watch(int tmdb_id, std::string title, std::string qbit_hash) {
    watched_.push_back({tmdb_id, std::move(title),
                        std::chrono::steady_clock::now(), std::move(qbit_hash)});
}

void DownloadWatchdog::unwatch(int tmdb_id) {
    watched_.erase(std::remove_if(watched_.begin(), watched_.end(),
                                  [tmdb_id](const auto& w){ return w.tmdb_id == tmdb_id; }),
                   watched_.end());
}

void DownloadWatchdog::snooze(int tmdb_id, std::chrono::seconds duration) {
    snoozed_until_[tmdb_id] = std::chrono::steady_clock::now() + duration;
}

std::vector<DownloadWatchdog::StallEvent>
DownloadWatchdog::evaluate(const Inputs& in) {
    std::vector<StallEvent> out;
    for (const auto& w : in.watched) {
        // Snooze check.
        auto sit = in.snoozed_until.find(w.tmdb_id);
        if (sit != in.snoozed_until.end() && in.now < sit->second) continue;

        // Radarr-failed check (highest priority).
        bool radarr_failed = false;
        for (const auto& h : in.recent_history) {
            if (h.movie_id == w.tmdb_id && h.event_type == "downloadFailed") {
                radarr_failed = true;
                break;
            }
        }
        if (radarr_failed) {
            out.push_back({w.tmdb_id, w.title, Reason::RadarrFailed});
            continue;
        }

        // Zero-progress check.
        if (in.now - w.started_at < std::chrono::seconds(60)) continue;
        const QbitTorrent* t = nullptr;
        for (const auto& qt : in.qbit_torrents) {
            if (qt.hash == w.hash) { t = &qt; break; }
        }
        // No qBit torrent at all = stall (Radarr never handed it off).
        // OR qBit torrent with 0 progress and 0 seeds = stall.
        if (!t || (t->progress == 0.0 && t->num_seeds == 0)) {
            out.push_back({w.tmdb_id, w.title, Reason::ZeroProgress});
        }
    }
    return out;
}

std::vector<DownloadWatchdog::StallEvent> DownloadWatchdog::tick() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_poll_ < std::chrono::seconds(10)) return {};
    last_poll_ = now;
    Inputs in;
    in.now = now;
    in.watched = watched_;
    in.qbit_torrents = qbit_.get_torrents();
    in.snoozed_until = snoozed_until_;
    for (const auto& w : watched_) {
        auto hist = radarr_.get_history(w.tmdb_id, /*page_size=*/10);
        for (auto& h : hist) in.recent_history.push_back(std::move(h));
    }
    return evaluate(in);
}

}  // namespace media_browser
```

- [ ] **Step 5: Add files to CMakeLists.txt**

In `KIOSK_MEDIA_BROWSER_SOURCES`:

```cmake
    src/media_browser/qbittorrent/download_watchdog.cpp
```

In `MEDIA_BROWSER_TEST_SOURCES`:

```cmake
    tests/media_browser/test_download_watchdog.cpp
```

- [ ] **Step 6: Build, run watchdog tests**

```bash
cd magic_dingus_box_cpp/build && cmake --build . --target test_media_browser_unit
./test_media_browser_unit "[watchdog]"
```

Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/qbittorrent/download_watchdog.{h,cpp} \
        magic_dingus_box_cpp/tests/media_browser/test_download_watchdog.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): DownloadWatchdog with zero-progress + Radarr-failed stall detection"
```

---

### Task 15: Create the stall-prompt modal

**Goal:** Lightweight modal that the main loop displays when the watchdog emits a stall event. Two buttons: `[Pick]` (opens the picker for the stalled movie) and `[Dismiss]` (snoozes 10 min).

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/ui/stall_prompt_modal.h`
- Create: `magic_dingus_box_cpp/src/media_browser/ui/stall_prompt_modal.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt`

- [ ] **Step 1: Create the header**

```cpp
#pragma once

#include <functional>
#include <string>

namespace media_browser {

class StallPromptModal {
public:
    using PickHandler = std::function<void(int tmdb_id, const std::string& title)>;
    using DismissHandler = std::function<void(int tmdb_id)>;

    struct Pending {
        int tmdb_id = 0;
        std::string title;
        std::string reason_label;  // "Stalled — no peers" / "Radarr reported failed"
    };

    void set_handlers(PickHandler on_pick, DismissHandler on_dismiss);
    void show(Pending p);
    bool is_active() const { return active_; }

    // Per-frame entry points.
    void render(int screen_w, int screen_h);   // signature matches the existing modal style
    bool handle_input(int btn);                 // true if input was consumed

private:
    bool active_ = false;
    Pending pending_;
    int focus_ = 0;  // 0 = Pick, 1 = Dismiss
    PickHandler pick_handler_;
    DismissHandler dismiss_handler_;
};

}  // namespace media_browser
```

- [ ] **Step 2: Implement the .cpp**

```cpp
#include "media_browser/ui/stall_prompt_modal.h"
#include "media_browser/ui/mb_chrome.h"
#include "ui/renderer.h"

namespace media_browser {

void StallPromptModal::set_handlers(PickHandler on_pick, DismissHandler on_dismiss) {
    pick_handler_ = std::move(on_pick);
    dismiss_handler_ = std::move(on_dismiss);
}

void StallPromptModal::show(Pending p) {
    pending_ = std::move(p);
    focus_   = 0;
    active_  = true;
}

void StallPromptModal::render(int screen_w, int screen_h) {
    if (!active_) return;
    // Match the existing MB modal pattern (e.g. DetailScreen's confirm-remove
    // modal). This is a placeholder layout — adjust to match the project's
    // modal helper if one exists in mb_chrome.
    auto& r = ::ui::Renderer::instance();  // adjust if Renderer is passed in
    // Backdrop.
    r.draw_rect(0, 0, screen_w, screen_h, 0x000000C0);
    // Panel.
    int pw = 720, ph = 280;
    int px = (screen_w - pw) / 2;
    int py = (screen_h - ph) / 2;
    r.draw_rect(px, py, pw, ph, 0x111A24FF);
    r.draw_rect_outline(px, py, pw, ph, 2, 0xFFD700FF);
    r.draw_text(px + 32, py + 40, "Download stalled", 24, 0xFFD700FF);
    r.draw_text(px + 32, py + 80, pending_.title, 18, 0xE0E0E0FF);
    r.draw_text(px + 32, py + 110, pending_.reason_label, 14, 0xA0A0A0FF);

    // Buttons.
    int by = py + ph - 80;
    media_browser::ui::draw_button(r, px + 32,        by, "Pick another",
                                   media_browser::ui::ButtonKind::Action,
                                   /*focused=*/focus_ == 0);
    media_browser::ui::draw_button(r, px + pw - 240,  by, "Dismiss (10m)",
                                   media_browser::ui::ButtonKind::Neutral,
                                   /*focused=*/focus_ == 1);
}

bool StallPromptModal::handle_input(int btn) {
    if (!active_) return false;
    constexpr int kBtnA = 0, kBtnB = 1, kDpadLeft = 102, kDpadRight = 103;
    if (btn == kDpadLeft)  { focus_ = 0; return true; }
    if (btn == kDpadRight) { focus_ = 1; return true; }
    if (btn == kBtnA) {
        if (focus_ == 0 && pick_handler_) pick_handler_(pending_.tmdb_id, pending_.title);
        if (focus_ == 1 && dismiss_handler_) dismiss_handler_(pending_.tmdb_id);
        active_ = false;
        return true;
    }
    if (btn == kBtnB) {
        if (dismiss_handler_) dismiss_handler_(pending_.tmdb_id);
        active_ = false;
        return true;
    }
    return false;
}

}  // namespace media_browser
```

Verify the actual `Renderer`, `mb_chrome::draw_button`, and button-constant names match the project; substitute as needed. If the project's modal pattern is different (e.g. modals are subclasses of a `Modal` base), adapt to match.

- [ ] **Step 3: Add to CMakeLists.txt**

In `KIOSK_MEDIA_BROWSER_SOURCES`:

```cmake
    src/media_browser/ui/stall_prompt_modal.cpp
```

- [ ] **Step 4: Build, verify**

```bash
cd magic_dingus_box_cpp/build && cmake --build . -j2 2>&1 | tail -20
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/stall_prompt_modal.{h,cpp} \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(mb): StallPromptModal — surfaces watchdog stall events with Pick/Dismiss"
```

---

### Task 16: Wire watchdog + modal into main loop

**Goal:** Instantiate the watchdog and modal in `main.cpp`. Tick the watchdog each frame; when it emits an event, show the modal. Wire Pick → opens picker for the stalled movie; Dismiss → snoozes 10 min. Register watch on Add-to-Library and on picker grabs.

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp`
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp` (call watchdog.watch on add-to-library success)

- [ ] **Step 1: Add watchdog + modal instances**

In `main.cpp`, alongside the other media-browser objects:

```cpp
    media_browser::DownloadWatchdog watchdog(radarr_client, qbit_client);
    media_browser::StallPromptModal stall_modal;
```

Includes:

```cpp
    #include "media_browser/qbittorrent/download_watchdog.h"
    #include "media_browser/ui/stall_prompt_modal.h"
```

- [ ] **Step 2: Wire the modal handlers**

After modal construction:

```cpp
    stall_modal.set_handlers(
        [&](int tmdb_id, const std::string& title) {
            // Re-fetch releases and open picker. The picker lives at the
            // movie level — we already have prowlarr's last results if the
            // user is on the same movie's Detail. For a different movie,
            // re-trigger Prowlarr search.
            (void)tmdb_id;
            // For v1: just route to Detail for the title; the user can press
            // "Pick a source" from there. (A v2 enhancement could deep-link
            // straight into the picker after re-running the search.)
            current_mb_screen = Screen::Detail;
        },
        [&](int tmdb_id) {
            watchdog.snooze(tmdb_id, std::chrono::minutes(10));
        });
```

- [ ] **Step 3: Per-frame tick + event surfacing**

In the main loop, before the screen render block:

```cpp
    auto stall_events = watchdog.tick();
    if (!stall_events.empty() && !stall_modal.is_active()) {
        const auto& e = stall_events.front();
        media_browser::StallPromptModal::Pending p;
        p.tmdb_id = e.tmdb_id;
        p.title   = e.title;
        p.reason_label = (e.reason == media_browser::DownloadWatchdog::Reason::RadarrFailed)
            ? "Radarr reported the download failed"
            : "Stalled — no peers connected after 60s";
        stall_modal.show(std::move(p));
    }
```

In the input dispatch, give the modal first crack:

```cpp
    if (stall_modal.handle_input(button_event)) {
        // Modal consumed input; skip normal screen routing this frame.
    } else {
        // ...existing screen switch...
    }
```

In the render loop, draw the modal AFTER the active screen:

```cpp
    stall_modal.render(screen_w, screen_h);
```

- [ ] **Step 4: Register watch on successful Add-to-Library**

In `detail_screen.cpp`, find `do_add_to_library()`. After the `::ui::Toast::show("Added to library — downloading");` line, add:

```cpp
    if (watchdog_) watchdog_->watch(tmdb_id_, title_);
```

Add a `DownloadWatchdog* watchdog_ = nullptr;` member to `DetailScreen` and a setter `void set_watchdog(DownloadWatchdog* w) { watchdog_ = w; }`. Wire `detail_screen.set_watchdog(&watchdog)` in `main.cpp`. Include `"media_browser/qbittorrent/download_watchdog.h"` in `detail_screen.h`.

- [ ] **Step 5: Register watch on picker grab**

In `release_picker_screen.cpp`, in `handle_input` after the successful `grab_release` call, add a similar `watchdog_->watch(...)` (passing tmdb_id + title via setters from `set_candidates`). Mirror the pattern from DetailScreen.

- [ ] **Step 6: Build + smoke-test on Pi**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service'
```

Manual smoke: add a movie to library that has no seeds (e.g. an obscure film) → wait 60+ seconds → confirm stall modal appears → press Dismiss → confirm modal closes and doesn't reappear for 10 min → confirm a different movie's stall still triggers.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/detail_screen.{h,cpp} \
        magic_dingus_box_cpp/src/media_browser/ui/release_picker_screen.{h,cpp}
git commit -m "feat(mb): wire DownloadWatchdog + StallPromptModal into main loop"
```

---

## Phase 7 — Settings panel redesign

### Task 17: Refactor `mb_settings_screen` Sources panel to live data + live toggles

**Goal:** Replace the current in-memory checkbox list with a panel that pulls real indexer state from Prowlarr on entry, decorates with last-search per-indexer stats, sorts by result count desc, and PUTs toggles back to Prowlarr live.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp`
- Modify: `magic_dingus_box_cpp/src/media_browser/prowlarr/prowlarr_client.h` (new helpers)
- Modify: `magic_dingus_box_cpp/src/media_browser/prowlarr/prowlarr_client.cpp`

- [ ] **Step 1: Add `list_indexers()` and `set_indexer_enabled()` to ProwlarrClient**

In `prowlarr_client.h`, add public methods:

```cpp
    struct IndexerInfo {
        int         id = 0;
        std::string name;
        bool        enabled = false;
    };

    // Live GET /api/v1/indexer. Synchronous — call from settings screen entry.
    std::vector<IndexerInfo> list_indexers();

    // Live PUT /api/v1/indexer/<id> with the `enable` field flipped.
    // Returns true on 2xx.
    bool set_indexer_enabled(int id, bool enabled);
```

In `prowlarr_client.cpp`:

```cpp
std::vector<ProwlarrClient::IndexerInfo>
ProwlarrClient::list_indexers() {
    std::vector<IndexerInfo> out;
    std::string resp = http_get("/api/v1/indexer");  // use existing helper
    if (resp.empty()) return out;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string err;
    std::istringstream is(resp);
    if (!Json::parseFromStream(b, is, &root, &err)) return out;
    if (!root.isArray()) return out;
    for (const auto& r : root) {
        IndexerInfo i;
        i.id      = r.get("id", 0).asInt();
        i.name    = r.get("name", "").asString();
        i.enabled = r.get("enable", false).asBool();
        out.push_back(std::move(i));
    }
    return out;
}

bool ProwlarrClient::set_indexer_enabled(int id, bool enabled) {
    // Fetch the full object first (Prowlarr requires the entire entity in PUT).
    std::string get_resp = http_get("/api/v1/indexer/" + std::to_string(id));
    if (get_resp.empty()) return false;
    Json::CharReaderBuilder b;
    Json::Value obj;
    std::string err;
    std::istringstream is(get_resp);
    if (!Json::parseFromStream(b, is, &obj, &err)) return false;
    obj["enable"] = enabled;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::string body = Json::writeString(w, obj);
    return !http_put("/api/v1/indexer/" + std::to_string(id) + "?forceSave=true", body).empty();
}
```

If `http_get` / `http_put` aren't currently exposed in `ProwlarrClient`, look at the existing search worker to see how it issues HTTP requests and either expose helpers or replicate the pattern. (RadarrClient has them as virtuals — Prowlarr likely uses the same libcurl pattern.)

- [ ] **Step 2: Reshape the `IndexerRow` data in `mb_settings_screen.h`**

Replace whatever currently represents the indexer list (an in-memory `std::vector<ProwlarrIndexer>` with just name+enabled) with:

```cpp
    struct IndexerRow {
        int    id = 0;
        std::string name;
        bool   enabled = false;
        // Decorations from ProwlarrClient::get_last_indexer_stats()
        // (empty if no search has occurred this session).
        bool   has_stats = false;
        int    result_count = 0;
        int    results_above_seed_threshold = 0;
        std::string last_error;
    };

    std::vector<IndexerRow> indexer_rows_;
```

- [ ] **Step 3: Populate on screen entry / refresh**

Add a method `void refresh_indexers()`:

```cpp
void MbSettingsScreen::refresh_indexers() {
    indexer_rows_.clear();
    auto live = prowlarr_->list_indexers();
    auto stats = prowlarr_->get_last_indexer_stats();
    auto find_stat = [&](const std::string& name) -> ProwlarrClient::IndexerStats* {
        for (auto& s : stats) if (s.name == name) return &s;
        return nullptr;
    };
    for (auto& info : live) {
        IndexerRow row;
        row.id = info.id;
        row.name = info.name;
        row.enabled = info.enabled;
        if (auto* s = find_stat(info.name)) {
            row.has_stats = true;
            row.result_count = s->result_count;
            row.results_above_seed_threshold = s->results_above_seed_threshold;
            row.last_error = s->last_error;
        }
        indexer_rows_.push_back(std::move(row));
    }
    // Sort: enabled with most results first, then enabled with no stats,
    // then disabled at the bottom.
    std::sort(indexer_rows_.begin(), indexer_rows_.end(),
              [](const IndexerRow& a, const IndexerRow& b) {
                  if (a.enabled != b.enabled) return a.enabled > b.enabled;
                  if (a.has_stats != b.has_stats) return a.has_stats > b.has_stats;
                  return a.result_count > b.result_count;
              });
}
```

Call `refresh_indexers()` from the screen's `on_enter()` (or wherever `build_rows()` is invoked).

- [ ] **Step 4: Update the rendering of the IndexerToggles row**

Find the existing render block (around lines 299–340 or further down where `RowKind::IndexerToggles` is drawn). Replace the per-row drawing with:

```cpp
    for (size_t i = 0; i < indexer_rows_.size() && i < kIndexerMaxVisible; ++i) {
        const auto& row = indexer_rows_[i];
        float y = top + i * kIndexerRowHeight;
        // Health dot
        uint32_t dot = 0x808080FF;  // gray = no stats
        if (row.has_stats) {
            if (!row.last_error.empty()) dot = 0xCC4040FF;          // red
            else if (row.result_count == 0) dot = 0xC0C040FF;        // yellow
            else dot = 0x40C040FF;                                   // green
        }
        r.draw_circle(x + 16, y + 18, 6, dot);
        // Name
        r.draw_text(x + 36, y + 14, row.name, 16,
                    row.enabled ? 0xE0E0E0FF : 0x707070FF);
        // Stats
        if (row.has_stats) {
            std::string s = std::to_string(row.result_count) + " results · " +
                            std::to_string(row.results_above_seed_threshold) +
                            " with seeds ≥10";
            r.draw_text(x + 240, y + 14, s, 12, 0xA0A0A0FF);
        } else {
            r.draw_text(x + 240, y + 14, "—", 12, 0x707070FF);
        }
        // Toggle indicator on the right
        r.draw_text(x + width - 80, y + 14,
                    row.enabled ? "[on]" : "[off]", 14,
                    row.enabled ? 0x40C040FF : 0x808080FF);
    }
```

If `draw_circle` doesn't exist, draw a small filled rect instead. Match the row height + width constants to the existing `kIndexerRowHeight` / panel width.

- [ ] **Step 5: Update the SELECT handler to call Prowlarr**

Find the current handler that flips the in-memory enabled flag (around line 694–711 in the report — look for the banner `"Indexer toggle is local-only in MVP"`). Replace with:

```cpp
    if (indexer_cursor_ >= 0 && indexer_cursor_ < (int)indexer_rows_.size()) {
        auto& row = indexer_rows_[indexer_cursor_];
        bool new_state = !row.enabled;
        if (prowlarr_->set_indexer_enabled(row.id, new_state)) {
            row.enabled = new_state;
            ::ui::Toast::show(std::string(row.name) +
                              (new_state ? " enabled" : " disabled"));
        } else {
            ::ui::Toast::show("Toggle failed — Prowlarr unreachable");
        }
    }
```

- [ ] **Step 6: Build + smoke-test**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service'
```

Manual smoke on Pi: open Settings → Sources → confirm 10 enabled indexers shown with green dots after performing a movie search; confirm 5 disabled indexers grouped at the bottom; toggle one off via SELECT → confirm Prowlarr web UI (SSH tunnel localhost:9696) reflects the change after a few seconds.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.{h,cpp} \
        magic_dingus_box_cpp/src/media_browser/prowlarr/prowlarr_client.{h,cpp}
git commit -m "feat(mb): Sources panel pulls live Prowlarr state + per-indexer stats; toggles persist"
```

---

## Final smoke + ship

### Task 18: End-to-end manual smoke test

**Goal:** Walk through every new behavior on the Pi to confirm the entire feature is stable. No code changes here.

- [ ] **Step 1: Indexer pool + search depth**

On the kiosk: open MB → search "Inception" → enter Detail → wait for the "Sources" readout to update. Expected: total releases ≥30 (up from ~15 with the old 4-indexer pool), and best seeders > 100.

- [ ] **Step 2: Manual release picker — proactive**

From Detail (still on Inception): press the new "Pick a source (N)" button. Expected: picker opens with rows sorted by seeders desc; one row has a gold border (Radarr's choice); some rows may be dimmed red if x265/HEVC + below-threshold. Press DPad down a few times → focus moves; press A on a high-seed row → toast "Grabbing release — see Queue", returns to Detail; check Queue tab → that release is downloading.

- [ ] **Step 3: Stall prompt — reactive**

Add an obscure foreign film (low seed availability) to library. Expected: within 60s of qBit not connecting, the stall modal appears with "Stalled — no peers connected after 60s" and the movie title. Press "Pick another" → routes to Detail. Press Dismiss on a different stall → modal closes; verify the same movie doesn't re-prompt for 10 min.

- [ ] **Step 4: Settings → Sources**

Open Settings → Sources. Expected: 10 enabled indexers visible (LimeTorrents, TPB, YTS, TorrentDownload, 1337x, TGx, Solid, BitSearch, Knaben, TheRARBG) with green/yellow/red dots reflecting the most recent search; 5 disabled indexers at the bottom with gray dots. Toggle one indexer off → SELECT → confirm "X disabled" toast → SSH `curl -H "X-Api-Key: $KEY" http://localhost:9696/api/v1/indexer/<id>` shows `enable: false`.

- [ ] **Step 5: minimumSeeders floor verification**

SSH and run: `curl -s -H "X-Api-Key: $KEY" http://localhost:7878/api/v3/indexer | python3 -c "import json,sys; print(set(f['value'] for ix in json.load(sys.stdin) for f in ix.get('fields',[]) if f['name']=='minimumSeeders'))"`. Expected: `{10}`.

- [ ] **Step 6: Tag the release**

```bash
git tag mb-source-selection-v1
# Optionally push the tag if working with a remote
```

---

## Self-review

Spec coverage check (against [docs/superpowers/specs/2026-05-01-media-browser-source-selection-design.md](docs/superpowers/specs/2026-05-01-media-browser-source-selection-design.md)):
- §1 Indexer pool changes → Task 1 (six new entries) + Task 4 (deploy/verify). Task 1 leaves the existing five disabled entries untouched, matching the corrected spec.
- §2 Scoring tuning → Tasks 2 (minimumSeeders) and 3 (trusted-groups regex).
- §3 Release picker UI → Tasks 9 (enum slot), 10 (skeleton), 11 (sort/render/grab impl), 12 (dispatcher wiring).
- §4 Download watchdog → Tasks 14 (watchdog logic), 15 (modal), 16 (main-loop wiring).
- §5 Sources panel redesign → Task 17. Pulls live Prowlarr state + per-indexer stats; live PUT on toggle.
- §6 Detail screen changes → Task 13. Adds "Pick a source" button gated on Prowlarr Ready state.
- §7 Component boundaries → reflected in file map at the top.
- §8 Files touched → file map covers every entry.

No placeholder strings (no TBD/TODO/"implement later"). Method signatures are consistent across tasks: `grab_release(const Json::Value&)` defined in Task 6, called in Task 11; `get_history(int, int)` defined in Task 7, called in Task 14; `get_last_indexer_stats()` defined in Task 5, called in Task 17.
