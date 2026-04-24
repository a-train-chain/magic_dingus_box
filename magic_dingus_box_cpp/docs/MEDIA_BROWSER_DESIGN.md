# Media Browser - Design Document

> ⚠️ **SUPERSEDED (2026-04-23) by `MEDIA_BROWSER_V2_DESIGN.md`.**
> This document captured the original "build a Radarr-equivalent release picker in C++" vision.
> After a scope reassessment, the project pivoted to using Radarr + Prowlarr + qBittorrent as
> companion Docker services on the Pi, with the kiosk as a custom UI client. Phase 1 work
> (LibraryDb, TmdbClient, TorrentSession, CMake isolation) still shipped and remains in the tree,
> but Phase 2+ follows the V2 design. This doc is preserved for historical context only —
> do NOT use it to guide implementation work.

**Status:** Research / Pre-implementation (superseded)
**Target release:** Future update (post-1.x, not yet scheduled)
**Author:** Design captured from architecture discussion, 2026-04-21

A long-form movie & TV browsing experience for the kiosk: search a metadata database (TMDB), pick a title, and have the device autonomously find, download, and import the content — all without leaving the kiosk UI or touching a torrent site manually.

This document captures the full design so implementation can begin fresh in a future session with all decisions already pinned.

---

## 1. Goals

- **Kiosk-native browsing.** Poster grid, detail pages, search, controller-navigable — matches the existing UI style (immediate-mode renderer, controller-first input).
- **Feature parity with Radarr/Sonarr** for release selection, download orchestration, quality profiles, upgrades, stuck-download handling, blocklisting.
- **Single-binary kiosk model preserved.** No companion services, no Docker, no external daemons. Everything ships inside `magic_dingus_box_cpp`.
- **Full control of the experience.** Custom scoring, custom UI, custom retry policy — tailored to small-screen kiosk use, not adapted from a desktop tool.
- **Isolation during development.** Feature can be built, tested, and merged without touching the existing playlist/RetroArch experience until explicitly enabled.

## 2. Non-goals

- Streaming from external services (Netflix, Prime, etc.). Out of scope.
- Full "media server" features (multi-user accounts, remote clients, transcode-on-demand). This is a kiosk, not a Jellyfin replacement.
- Live TV, PVR, DVR.
- Music, audiobooks, photos.
- Supporting every indexer under the sun in v1 — we target the Torznab standard and add specific trackers as needed.

## 3. Legal / operational framing

The underlying technology (BitTorrent, indexer search, metadata APIs) is legal and has many legitimate uses. Downloading copyrighted material without authorization is illegal in most jurisdictions. The design assumes the operator is responsible for content legality and focuses on legitimate sources:

- **Internet Archive** — large public-domain film library, free API.
- **Creative Commons / public-domain torrents** — well-seeded, legal.
- **Personal rips of media you own** — legal in many jurisdictions.
- **Linux distro ISOs and other legal torrents** — useful for testing the full pipeline without any legal ambiguity.

The feature ships **disabled by default** behind both a CMake flag and a runtime settings toggle (see §13). Documentation will emphasize legitimate-source use.

## 4. High-level architecture

```
                   +-------------------------------+
                   |         Kiosk UI              |
                   |   (MediaBrowser screens)      |
                   +---------------+---------------+
                                   |
                                   | in-process API
                                   v
+-----------+      +-----------------------------------+      +-----------+
|   TMDB    |<-----+         Media Browser Core        +----->|  SQLite   |
| (metadata)|      |                                   |      |  (state)  |
+-----------+      |  +-----------+   +-------------+  |      +-----------+
                   |  | Indexers  |   |Orchestrator |  |
+-----------+      |  | (Torznab) |   |(state mach.)|  |
| Indexers  |<-----+  +-----------+   +-------------+  |
| (trackers)|      |                                   |
+-----------+      |  +--------------+   +----------+  |
                   |  | Release      |   |libtorrent|  |
                   |  | Parser+Score |   | Session  |  |
                   |  +--------------+   +----------+  |
                   +-----------------------------------+
                                   |
                                   v
                   +-------------------------------+
                   |  Library directory (on disk)  |
                   |  data/library/Movies/...      |
                   |  data/library/TV/...          |
                   +-------------------------------+
```

All components run in-process. No IPC, no daemons, no Docker.

## 5. Module layout

Following the existing `src/<domain>/` pattern:

```
magic_dingus_box_cpp/src/media_browser/
├── tmdb_client.{h,cpp}          # TMDB API client (metadata, search, artwork)
├── artwork_cache.{h,cpp}        # Downloads + resizes posters/fanart to local disk
├── indexer/
│   ├── indexer_interface.h      # Abstract: search(query) -> vector<Release>
│   ├── torznab_client.{h,cpp}   # Torznab protocol impl (covers most trackers)
│   └── indexer_registry.{h,cpp} # Per-indexer config + rate limiting
├── release/
│   ├── release.h                # Release struct (all parsed metadata)
│   ├── release_parser.{h,cpp}   # Filename → structured metadata
│   └── release_scorer.{h,cpp}   # Quality profile scoring
├── library/
│   ├── library_db.{h,cpp}       # SQLite wrapper (schema in §6)
│   ├── library_entry.h          # Movie / Episode / Season types
│   └── library_importer.{h,cpp} # Rename + move to library after download
├── torrent/
│   ├── torrent_session.{h,cpp}  # libtorrent session wrapper
│   └── torrent_handle.{h,cpp}   # Per-download state + progress
├── orchestrator/
│   ├── orchestrator.{h,cpp}     # State machine: search → download → import
│   ├── quality_profile.{h,cpp}  # User-configurable profile definitions
│   └── scheduler.{h,cpp}        # Periodic: RSS poll, metadata refresh, disk check
└── ui/
    ├── browse_screen.{h,cpp}    # TMDB-powered poster grid (discovery + search)
    ├── detail_screen.{h,cpp}    # Title detail + "Download" action
    ├── queue_screen.{h,cpp}     # Active downloads + progress
    ├── library_screen.{h,cpp}   # What we already have, monitored, upgrades
    └── mb_settings_screen.{h,cpp} # Quality profiles, indexers, storage caps
```

Why this shape:
- Mirrors existing top-level `platform/`, `video/`, `ui/`, `app/`, `retroarch/` separation.
- Clean subdirectories inside `media_browser/` for the larger internal domains (indexer, release, library, torrent, orchestrator, ui).
- UI screens live under `media_browser/ui/` rather than the shared `src/ui/` tree — keeps the feature self-contained and easy to exclude via CMake.

## 6. Data model (SQLite)

Single SQLite DB at `data/media_browser.db`. WAL mode for crash safety.

### Core tables

```sql
-- Canonical catalog entry (movie or TV series)
CREATE TABLE titles (
    id              INTEGER PRIMARY KEY,
    tmdb_id         INTEGER NOT NULL UNIQUE,
    kind            TEXT NOT NULL CHECK(kind IN ('movie','tv')),
    title           TEXT NOT NULL,
    original_title  TEXT,
    year            INTEGER,
    overview        TEXT,
    poster_path     TEXT,           -- local path in artwork cache
    fanart_path     TEXT,
    runtime_minutes INTEGER,
    tmdb_rating     REAL,
    added_at        INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL
);

-- User's "do I want this?" intent
CREATE TABLE monitored (
    title_id        INTEGER PRIMARY KEY REFERENCES titles(id),
    quality_profile_id INTEGER NOT NULL REFERENCES quality_profiles(id),
    monitor         INTEGER NOT NULL DEFAULT 1,  -- 0 = ignored
    min_seeders     INTEGER NOT NULL DEFAULT 1,
    monitor_added_at INTEGER NOT NULL
);

-- TV: one row per season
CREATE TABLE seasons (
    id              INTEGER PRIMARY KEY,
    title_id        INTEGER NOT NULL REFERENCES titles(id),
    season_number   INTEGER NOT NULL,
    episode_count   INTEGER,
    UNIQUE(title_id, season_number)
);

-- TV: one row per episode
CREATE TABLE episodes (
    id              INTEGER PRIMARY KEY,
    season_id       INTEGER NOT NULL REFERENCES seasons(id),
    episode_number  INTEGER NOT NULL,
    name            TEXT,
    air_date        TEXT,   -- ISO 8601
    overview        TEXT,
    have_file       INTEGER NOT NULL DEFAULT 0,
    file_path       TEXT,
    file_quality    TEXT,   -- e.g. "1080p WEB-DL"
    UNIQUE(season_id, episode_number)
);

-- Movies: one file per title (for now; 4K + 1080p dual-store is a later concern)
CREATE TABLE movie_files (
    title_id        INTEGER PRIMARY KEY REFERENCES titles(id),
    file_path       TEXT NOT NULL,
    quality         TEXT NOT NULL,
    size_bytes      INTEGER NOT NULL,
    imported_at     INTEGER NOT NULL
);
```

### Download lifecycle

```sql
CREATE TABLE quality_profiles (
    id              INTEGER PRIMARY KEY,
    name            TEXT NOT NULL UNIQUE,
    spec_json       TEXT NOT NULL   -- see §8
);

CREATE TABLE queue (
    id              INTEGER PRIMARY KEY,
    title_id        INTEGER NOT NULL REFERENCES titles(id),
    episode_id      INTEGER REFERENCES episodes(id),  -- null for movies
    state           TEXT NOT NULL,
    -- 'searching','queued','downloading','verifying','importing','completed','failed'
    release_json    TEXT,           -- chosen Release, serialized
    torrent_hash    TEXT,
    progress        REAL DEFAULT 0, -- 0.0 - 1.0
    last_error      TEXT,
    started_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL
);

CREATE TABLE history (
    id              INTEGER PRIMARY KEY,
    title_id        INTEGER REFERENCES titles(id),
    event           TEXT NOT NULL, -- 'grabbed','imported','failed','blocklisted','upgraded'
    release_name    TEXT,
    quality         TEXT,
    indexer         TEXT,
    detail          TEXT,           -- JSON blob
    occurred_at     INTEGER NOT NULL
);

CREATE TABLE blocklist (
    id              INTEGER PRIMARY KEY,
    title_id        INTEGER NOT NULL REFERENCES titles(id),
    release_hash    TEXT,           -- infohash if known
    release_name    TEXT NOT NULL,
    reason          TEXT,
    added_at        INTEGER NOT NULL
);
```

### Indexer config

```sql
CREATE TABLE indexers (
    id              INTEGER PRIMARY KEY,
    name            TEXT NOT NULL UNIQUE,
    kind            TEXT NOT NULL,        -- 'torznab'
    base_url        TEXT NOT NULL,
    api_key         TEXT,
    enabled         INTEGER NOT NULL DEFAULT 1,
    priority        INTEGER NOT NULL DEFAULT 50,
    rate_limit_ms   INTEGER NOT NULL DEFAULT 2000,
    last_request_at INTEGER
);
```

## 7. Release parser

**The hard part.** Input is a torrent name like:

```
The.Matrix.1999.REMASTERED.2160p.UHD.BluRay.x265.10bit.HDR.DTS-HD.MA.7.1-FGT
```

Output is a structured `Release`:

```cpp
struct Release {
    std::string raw_name;
    std::string parsed_title;     // "The Matrix"
    int year;                     // 1999
    Resolution resolution;        // R_2160P
    Source source;                // BLURAY
    std::string video_codec;      // "x265"
    bool hdr;                     // true
    std::string audio_codec;      // "DTS-HD MA"
    std::string audio_channels;   // "7.1"
    std::string release_group;    // "FGT"
    std::string language;         // "en" (default)
    bool proper;                  // false
    bool repack;                  // false
    // TV-specific
    std::optional<int> season;
    std::optional<int> episode;
    bool is_season_pack;
};
```

### Approach

- Port the regex set from **Radarr's `Parser.cs`** (MIT-compatible open source). ~400 patterns built up over a decade.
- Write a C++ equivalent using `std::regex` (with PCRE2 as a fallback if std::regex perf is inadequate on Pi).
- Keep a test corpus (`tests/release_parser_corpus.txt`) of 1000+ real release names with expected parses. New edge cases add rows, never delete. Regression-testable forever.

### Known edge cases

- Titles containing years (`2001: A Space Odyssey`) — disambiguate by context.
- Titles with dots in name (`Dr. Strangelove`) — dots are also the scene-naming separator.
- Multi-language releases.
- `S01E01-E02` vs `S01E01E02` vs `1x01-02`.
- Season packs without episode numbers.
- Anime numbering (absolute episode numbers, no seasons).

**Budget a full phase on the parser alone.** Underestimating this is how projects slip.

## 8. Quality profiles & scoring

A quality profile is JSON:

```json
{
  "name": "1080p Standard",
  "allowed": ["1080p WEB-DL", "1080p BluRay", "720p BluRay", "720p WEB-DL"],
  "preferred_order": [
    "1080p BluRay",
    "1080p WEB-DL",
    "720p BluRay",
    "720p WEB-DL"
  ],
  "min_size_mb": 500,
  "max_size_mb": 8000,
  "upgrade_until": "1080p BluRay",
  "reject_cam": true,
  "reject_ts": true,
  "reject_dvdscr": false,
  "group_blocklist": ["YTS", "RARBG-bot"],
  "group_boost": ["SPARKS", "FGT"],
  "min_seeders": 3
}
```

### Scoring

```
score = quality_rank_points       (e.g. index in preferred_order, inverted)
      + seeder_score              (logarithmic: log10(seeders) * 50)
      + size_fit_score            (gaussian-ish around (min+max)/2)
      + recency_score             (newer releases score slightly higher)
      + group_boost_score         (+50 for boosted, -inf for blocklisted)
      - freeleech_penalty         (no penalty, but configurable)
```

Deterministic and unit-testable. Given the same candidate list and profile, always picks the same release.

### Upgrade logic

If a better-quality release appears for a monitored title already in the library, the orchestrator re-queues automatically. After successful import, the old file is replaced (or kept, per setting). This is what makes Radarr/Sonarr feel "smart."

## 9. Orchestrator state machine

Per queue entry:

```
        +-----------+
        | searching |<---------------------------+
        +-----+-----+                            |
              | release found                    | stuck / failed / no peers
              v                                  | (retry with next-best)
        +-----------+                            |
        |  queued   |                            |
        +-----+-----+                            |
              | libtorrent add_torrent ok        |
              v                                  |
        +-------------+                          |
        | downloading +--------------------------+
        +------+------+
               | 100% + hash ok
               v
        +------------+
        | verifying  |
        +------+-----+
               | ok
               v
        +------------+
        | importing  |
        +------+-----+
               | rename + move + DB update ok
               v
        +------------+
        | completed  |
        +------------+
```

### Retry policy

- **Stuck download**: no piece progress for N minutes (configurable, default 15). Cancel, blocklist the release, return to `searching` with "exclude this infohash" filter.
- **No peers**: no connected peers after M minutes (default 5). Same as stuck.
- **Import failure** (e.g. disk full): move to `failed`, surface in UI, retry on user action only — never silent retry on import failures.
- **All releases exhausted**: back off. Try again in 6 hours (configurable).

### Concurrency

- Max N concurrent downloads per profile (default 3 on Pi, configurable).
- Indexer requests rate-limited per-indexer.
- Single orchestrator thread; uses a priority queue. libtorrent runs its own threads internally.

## 10. Indexer layer

### Interface

```cpp
class Indexer {
public:
    virtual ~Indexer() = default;
    virtual std::string name() const = 0;
    virtual std::vector<Release> search(const SearchQuery& q) = 0;
    virtual std::vector<Release> rss_feed() = 0;  // for scheduled polling
};

struct SearchQuery {
    std::string imdb_id;        // most specific
    std::string tmdb_id;
    std::string title;          // fallback
    int year;
    MediaKind kind;             // movie | tv
    std::optional<int> season;
    std::optional<int> episode;
    int min_seeders = 1;
};
```

### Torznab

Torznab is a standardized XML-over-HTTP indexer protocol. Many public and private trackers expose it natively. One `TorznabClient` implementation covers most of them — configuration is just URL + API key per instance.

For non-Torznab sources we care about, write purpose-built clients implementing `Indexer`. Each has its own file under `indexer/`.

### Rate limiting

Per-indexer token bucket. Global circuit breaker: if an indexer fails 5× in a row, disable it for 30 minutes and surface in UI.

## 11. libtorrent integration

### Session configuration

```cpp
namespace lt = libtorrent;

lt::settings_pack pack;
pack.set_str(lt::settings_pack::user_agent, "MagicDingusBox/1.0");
pack.set_int(lt::settings_pack::alert_mask,
    lt::alert::error_notification |
    lt::alert::storage_notification |
    lt::alert::status_notification);
pack.set_bool(lt::settings_pack::enable_dht, true);
pack.set_bool(lt::settings_pack::enable_lsd, true);
pack.set_bool(lt::settings_pack::enable_upnp, true);
pack.set_bool(lt::settings_pack::enable_natpmp, true);
pack.set_int(lt::settings_pack::download_rate_limit, 0);  // configurable
pack.set_int(lt::settings_pack::upload_rate_limit, 0);    // configurable
pack.set_int(lt::settings_pack::active_downloads, 3);
pack.set_int(lt::settings_pack::active_seeds, 10);

lt::session ses(pack);
```

### Key design points

- **Session state persistence**: save `.fastresume` per torrent so restarts don't redownload.
- **Storage location**: `data/downloads/incomplete/` while active, moved to `data/library/...` on import.
- **Piece verification**: libtorrent does this; orchestrator waits for the `torrent_finished_alert` before advancing to `verifying`.
- **Bandwidth caps**: configurable per time-of-day (e.g. unthrottled overnight). Nice-to-have in v1, deferred otherwise.
- **Seeding policy**: keep seeding after completion per user setting (legal defaults only). Seed ratio and time limits configurable.

### Pi 4 performance notes

- Pi 4 CPU handles libtorrent fine — the bottleneck is disk I/O and network.
- **USB3 SSD required** for the download directory. SD card write amplification will kill the card in weeks under heavy torrent load.
- Memory: libtorrent default cache is fine on 2GB Pi; cap at 128MB to leave room for GStreamer/UI.

## 12. UI screens

All screens use the existing immediate-mode renderer. Controller navigation matches the current kiosk idiom (DPad/analog, A to select, B to back).

### Browse screen (`browse_screen.cpp`)

- Default: poster grid of "Popular now" from TMDB (cached locally).
- Category strip at top: Popular, Top Rated, Now Playing, Upcoming, Search.
- Search triggers the virtual keyboard (reuse `ui/virtual_keyboard`).
- Scroll with DPad; A opens detail.

### Detail screen (`detail_screen.cpp`)

- Fanart as background with dim overlay.
- Poster, title, year, runtime, rating, overview.
- "Download" button → kicks off orchestrator, transitions to queue screen.
- For TV: season/episode list, per-episode state (have / missing / downloading).
- "Back" returns to browse.

### Queue screen (`queue_screen.cpp`)

- List of active queue entries.
- Per-entry: title, poster thumb, progress bar, download rate, peers, ETA.
- Actions: Pause, Cancel, Force-next-release, Open detail.
- Auto-refresh from DB every 2s while visible.

### Library screen (`library_screen.cpp`)

- What you already have. Grid view with "Have HD / Have SD / Missing episodes" state indicators.
- Filter: monitored, unwatched, missing upgrades.
- Selecting an item plays it (hand off to existing GStreamer playback path).

### Media Browser settings (`mb_settings_screen.cpp`)

- Quality profile editor (create / edit / delete profiles).
- Indexer management (add Torznab URL + API key, enable/disable, test connection).
- Storage settings (download dir, library dir, size caps).
- Bandwidth limits.
- Reuses the existing settings renderer patterns.

### Entry point

A new top-level menu entry "Movies & TV" on the main kiosk screen transitions to the Browse screen. When the feature is disabled (settings toggle or CMake flag), the entry is not rendered.

## 13. Build & deployment

### CMake flag

```cmake
option(ENABLE_MEDIA_BROWSER "Build movie/TV browser feature" OFF)

if(ENABLE_MEDIA_BROWSER)
    pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)
    pkg_check_modules(SQLITE REQUIRED sqlite3)
    pkg_check_modules(CURL REQUIRED libcurl)

    set(MEDIA_BROWSER_SOURCES
        src/media_browser/tmdb_client.cpp
        src/media_browser/artwork_cache.cpp
        src/media_browser/indexer/torznab_client.cpp
        src/media_browser/indexer/indexer_registry.cpp
        src/media_browser/release/release_parser.cpp
        src/media_browser/release/release_scorer.cpp
        src/media_browser/library/library_db.cpp
        src/media_browser/library/library_importer.cpp
        src/media_browser/torrent/torrent_session.cpp
        src/media_browser/torrent/torrent_handle.cpp
        src/media_browser/orchestrator/orchestrator.cpp
        src/media_browser/orchestrator/quality_profile.cpp
        src/media_browser/orchestrator/scheduler.cpp
        src/media_browser/ui/browse_screen.cpp
        src/media_browser/ui/detail_screen.cpp
        src/media_browser/ui/queue_screen.cpp
        src/media_browser/ui/library_screen.cpp
        src/media_browser/ui/mb_settings_screen.cpp
    )
    list(APPEND ALL_SOURCES ${MEDIA_BROWSER_SOURCES})
    target_compile_definitions(magic_dingus_box_cpp PRIVATE MEDIA_BROWSER_ENABLED)
    target_include_directories(magic_dingus_box_cpp PRIVATE
        ${LIBTORRENT_INCLUDE_DIRS} ${SQLITE_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})
    target_link_libraries(magic_dingus_box_cpp
        ${LIBTORRENT_LIBRARIES} ${SQLITE_LIBRARIES} ${CURL_LIBRARIES})
endif()
```

Production builds ship with `ENABLE_MEDIA_BROWSER=OFF`. Zero binary cost, zero new dependencies. Dev builds use `-DENABLE_MEDIA_BROWSER=ON`.

### Settings gate

Even with the CMake flag on, a runtime settings toggle hides the feature:

```yaml
# data/settings.yaml
media_browser:
  enabled: false                    # top-level kill switch
  download_dir: /mnt/ssd/downloads  # must exist + be writable
  library_dir: /mnt/ssd/library
  max_concurrent_downloads: 3
  tmdb_api_key: ""                  # required when enabled
  indexers: []                      # populated via settings UI
```

Disabled state = menu entry not rendered, orchestrator not started, libtorrent session not initialized.

### Source code guards

All `#include` of media_browser headers and all call sites are wrapped:

```cpp
#ifdef MEDIA_BROWSER_ENABLED
#include "media_browser/orchestrator/orchestrator.h"
#endif
```

Main loop integration point:

```cpp
// in main.cpp
#ifdef MEDIA_BROWSER_ENABLED
if (state.settings.media_browser.enabled) {
    media_browser::tick(state);
}
#endif
```

## 14. Dependencies

New runtime deps (all standard on Pi OS / Debian):

| Package | Purpose | apt name |
|---|---|---|
| libtorrent-rasterbar | BitTorrent session | `libtorrent-rasterbar-dev` |
| sqlite3 | Local DB | `libsqlite3-dev` |
| libcurl | HTTP client (TMDB, Torznab) | `libcurl4-openssl-dev` |
| tinyxml2 or pugixml | Torznab XML parsing | `libpugixml-dev` |

Header-only deps: existing `jsoncpp` (already in the tree).

Apt one-liner for the Pi:

```bash
sudo apt install -y libtorrent-rasterbar-dev libsqlite3-dev \
    libcurl4-openssl-dev libpugixml-dev
```

Added to `deploy_cpp.sh` behind a `--media-browser` flag that also sets the CMake option.

## 15. Build order (phased)

### Phase 0 — Research & setup (this document)

Captured. Next steps start fresh when implementation is scheduled.

### Phase 1 — Proof of life (1–2 weekends)

- SQLite schema + library_db CRUD.
- TMDB client: search + detail + artwork download.
- Embedded libtorrent session: download a hardcoded magnet link, verify completion.
- No UI, no orchestrator — just prove the foundation works on a Pi.
- Deliverable: a `test_media_browser` executable that searches TMDB, downloads a legal test torrent, writes results to SQLite.

### Phase 2 — Indexer + basic selection (2–3 weekends)

- Torznab client.
- Release parser v1 (ported regex set, 90% accuracy on test corpus).
- Simple release scorer: "highest seeders matching resolution."
- Orchestrator v1: single state, happy path only (search → download → import).
- Deliverable: kick off a download by title from a test harness; it completes and lands in the library directory.

### Phase 3 — The brain (3–4 weekends)

- Full quality profiles + JSON config.
- Full scorer (all factors).
- Orchestrator v2: full state machine, retries, stuck detection, blocklist.
- Upgrade logic.
- Scheduler (RSS polling, metadata refresh, disk-space checks).
- History + audit log.
- Deliverable: behaves like Radarr for movies. No UI yet.

### Phase 4 — UI (2–3 weekends)

- Browse screen (TMDB grid, search, categories).
- Detail screen.
- Queue screen.
- Library screen.
- Media Browser settings screen.
- Main-menu entry integration, settings gate wiring.
- Deliverable: end-to-end kiosk UX, feature-flagged.

### Phase 5 — TV support (2–3 weekends)

- Episodes + seasons in data model (already in schema).
- Per-episode and season-pack download logic.
- TV-specific UI: season list, episode list, aired vs not-aired state.
- Deliverable: Sonarr-equivalent behavior.

### Phase 6 — Polish (1–2 weekends)

- Manual release override (let user pick a specific search result).
- Import-error UX (disk full, permissions, bad file).
- Bandwidth scheduling.
- Performance pass on Pi 4 (profile hotspots, cache sizes).
- Documentation: user guide, operator guide.

**Total estimate:** 11–17 weekends for full parity. Realistic elapsed: 3–4 months of dedicated weekend work, more if squeezed between other priorities.

## 16. Testing strategy

- **Release parser:** regression corpus of real release names (`tests/release_parser_corpus.txt`). Target 95%+ parse accuracy. Every new edge case gets a line.
- **Scorer:** pure-function tests. Given candidate list + profile, assert expected winner.
- **Orchestrator:** state machine tests with mocked Indexer and TorrentSession. Exhaustive coverage of transition edges.
- **Library DB:** schema migration tests, round-trip CRUD tests.
- **Integration:** one end-to-end test on CI using a known-legal torrent (Internet Archive public-domain film or Linux ISO). Proves the real libtorrent path works.
- **UI:** manual checklist under `tests/manual/media_browser_checklist.md`, matching the pattern of `pre_image_checklist.md`.

## 17. Observability

- spdlog (already in the tree) for structured logs, tagged `media_browser`.
- History table is the audit trail — every grab, import, failure, upgrade.
- Queue screen surfaces state directly to user.
- Optional: expose a simple HTTP status endpoint (reuses existing web admin pattern) so the Flask web UI can show queue status remotely. Deferred to post-v1.

## 18. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Release parser edge cases eat months of work | Port from Radarr's Parser.cs rather than writing from scratch; build corpus early |
| libtorrent ABI changes between Pi OS versions | Pin version in CMake; document the required apt package version |
| SD card wears out from download I/O | Hard-require USB SSD in docs; settings screen validates download_dir is not on the boot device |
| Disk fills unexpectedly | Scheduler runs a disk-space check; hard-stops new downloads below a configurable floor |
| Indexers go offline or change APIs | Per-indexer circuit breaker; multi-indexer redundancy; UI flags dead indexers |
| Legal exposure from operator misuse | Feature disabled by default; docs emphasize legal sources; no seed-and-forget of copyrighted defaults |
| UI performance with large libraries | Paginate in DB queries; cache rendered poster textures with LRU eviction |
| libtorrent + GStreamer thread interaction | Keep libtorrent alerts on its own thread; marshal state to main thread via lockfree queue |

## 19. Open questions (decide during implementation kickoff)

1. **Regex engine for release parser** — `std::regex` (portable, slower) vs PCRE2 (fast, extra dep). Probably std::regex unless Pi benchmarks force otherwise.
2. **TMDB vs TVDB for TV metadata** — TMDB is free and simpler; TVDB is the legacy standard. Start with TMDB.
3. **Seeding defaults** — seed to 1.0 ratio? Seed forever? Never seed? Default probably "seed until 1.0 or 24h, whichever first," user-configurable.
4. **Artwork cache eviction** — LRU by access time, capped at 500MB? Reasonable default.
5. **Web admin integration** — expose queue status via existing Flask? Nice-to-have, defer.
6. **Subtitles** — fetch from OpenSubtitles? Scope creep for v1; defer.
7. **Multi-indexer result dedup** — by infohash where available, by release name otherwise. Edge cases need thought.

## 20. Explicitly deferred

These are real features, intentionally out of scope for v1:

- Subtitle management.
- Trailer playback from TMDB.
- Transcoding on import (we store the native file).
- Multi-user profiles / watch history.
- Watchlist sync with Trakt / Letterboxd.
- Parental controls.
- External remote control beyond the existing web admin.

## 21. References

- **Radarr** — https://github.com/Radarr/Radarr (release parser, scoring, state machine reference)
- **Sonarr** — https://github.com/Sonarr/Sonarr (TV-specific logic)
- **Prowlarr** — https://github.com/Prowlarr/Prowlarr (indexer management patterns)
- **libtorrent** — https://www.libtorrent.org/ (session API, alert system)
- **TMDB API** — https://developer.themoviedb.org/docs
- **Torznab spec** — https://torznab.github.io/spec-1.3-draft/

---

## Implementation-kickoff checklist

When this feature is greenlit for a real implementation session, use the following as the starting agenda:

- [ ] Re-read this document end to end; note any sections that have become outdated and revise.
- [ ] Decide open questions in §19 based on current priorities.
- [ ] Create the branch: `feature/media-browser`.
- [ ] Stand up the `src/media_browser/` directory skeleton with empty stubs.
- [ ] Add the CMake option and wire the conditional sources.
- [ ] Add the apt dependencies to `deploy_cpp.sh` under a `--media-browser` flag.
- [ ] Begin Phase 1 work (SQLite + TMDB + proof-of-life torrent).
- [ ] Write Phase 1 implementation plan as a separate doc before touching Phase 2+ scope.
