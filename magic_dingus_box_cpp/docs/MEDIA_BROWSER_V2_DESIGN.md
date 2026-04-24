# Media Browser V2 — Pivoted Design

**Status:** Design / Pre-implementation
**Supersedes:** `MEDIA_BROWSER_DESIGN.md` (original "build Radarr in C++" vision)
**Last revised:** 2026-04-23

A hidden, secret-sequence-unlocked movie browser for the Magic Dingus Box kiosk. Uses a companion Docker stack (Radarr + Prowlarr + qBittorrent) for release discovery and download orchestration; the kiosk provides a custom CRT-aesthetic UI client. Playback uses the existing GStreamer pipeline at an elevated 1080p widescreen mode scoped only to movie playback.

---

## 1. Why this design exists (the pivot)

The original `MEDIA_BROWSER_DESIGN.md` proposed building a Radarr-equivalent release-picker from scratch in C++. After more honest analysis, that path was reassessed as 10–15 focused weekends (200–350 hours) for feature parity with a mature open-source system that already runs on a Raspberry Pi 4.

This V2 design instead **runs Radarr, Prowlarr, and qBittorrent as companion Docker services on the same Pi** and treats the kiosk as a custom UI client to Radarr's HTTP API. The kiosk's unique contribution is the CRT-aesthetic UI, controller-first navigation, and the secret-sequence unlock easter-egg UX.

Realistic effort drops from **10–15 weekends** (self-build) to **~7 focused weekends sequential, or 4–5 weekends wall-clock with parallel sessions** (UI client + integration). See §15 for phase breakdown.

Phase 1 assets (`LibraryDb`, `TmdbClient`, `TorrentSession`, `test_media_browser` CLI, CMake isolation) remain in place. `LibraryDb` is still useful as a local cache; `TmdbClient` and `TorrentSession` become dormant on the kiosk side but stay in the tree for potential future use or tests.

## 2. Goals

1. **Hidden-until-invited.** The feature is invisible to casual kiosk users. A specific secret button sequence reveals it.
2. **Kiosk-native UI.** Browse, search, and manage movies using the same fonts, colors, rendering, and controller navigation as the rest of the kiosk — no "bolted-on different app" feeling.
3. **Radarr as backend.** Heavy lifting (release picking, download orchestration, quality management) done by the mature open-source stack, not rewritten.
4. **Seamless playback.** Downloaded movies appear in the kiosk's existing playlist system automatically. Playback uses GStreamer at 1080p widescreen for movies only; all other content keeps the current display mode.
5. **Isolation discipline preserved.** `ENABLE_MEDIA_BROWSER` CMake flag stays OFF by default. The Docker stack is optional — if it's not running, the kiosk degrades gracefully (Movies menu shows "service unavailable").
6. **Owner-only admin access.** Full Radarr web UI accessible to the owner via browser on any device on the network, authenticated so casual users can't stumble into it.

## 3. Non-goals

- Supporting users other than the kiosk owner (single-user system)
- Streaming from external commercial services (Netflix etc.)
- Transcoding on import (we store native files)
- 4K / HDR support (1080p is the ceiling for Pi 4 hardware decode)
- Subtitles (Phase 2+ — Radarr's Bazarr companion can handle later)
- Live TV, DVR, PVR
- TV series support (deferred; Radarr does movies, Sonarr does TV — adding Sonarr is a future extension)
- Mobile app / second-screen control (Radarr's web UI already serves this need)

## 4. Legal and operational framing

The architecture is legal. Radarr, Prowlarr, qBittorrent, libtorrent, GStreamer, and TMDB API are all legitimate software. Using them to download copyrighted content without permission is illegal in most jurisdictions.

The design assumes the operator (you) is responsible for content legality and focuses on legitimate sources: Internet Archive public-domain films, Creative Commons works, Linux distro ISOs (for testing), and personal rips of media owned on disc. The feature ships disabled by default behind three gates (CMake flag, runtime settings toggle, secret sequence) to make sure only a deliberate operator ever enables it.

## 5. High-level architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Raspberry Pi 4B (the kiosk)                  │
│                                                                 │
│  ┌──────────────────────────┐    ┌──────────────────────────┐   │
│  │   Kiosk C++ binary       │    │  Docker Compose stack    │   │
│  │   (magic_dingus_box_cpp) │    │                          │   │
│  │                          │    │  ┌────────────────────┐  │   │
│  │  ┌────────────────────┐  │    │  │  Radarr :7878      │  │   │
│  │  │ Main UI loop       │  │ HTTP ──► Movie catalog,    │  │   │
│  │  │ - Playlist browser │  │    │  │  quality, imports  │  │   │
│  │  │ - Settings menu    │  │    │  └────────────────────┘  │   │
│  │  │ - [hidden] Movies  │◄─┼──HTTP──┤                      │   │
│  │  └────────────────────┘  │    │  ┌────────────────────┐  │   │
│  │                          │    │  │  Prowlarr :9696    │  │   │
│  │  ┌────────────────────┐  │    │  │  Indexer manager   │  │   │
│  │  │ Secret sequence    │  │    │  └────────────────────┘  │   │
│  │  │ detector           │  │    │         ▲                │   │
│  │  │ (GPIO + rotary)    │  │    │         │                │   │
│  │  └────────────────────┘  │    │  ┌──────┴─────────────┐  │   │
│  │                          │    │  │  qBittorrent :8080 │  │   │
│  │  ┌────────────────────┐  │    │  │  Torrent client    │  │   │
│  │  │ RadarrClient       │  │    │  └────────────────────┘  │   │
│  │  │ (libcurl HTTP)     │  │    │                          │   │
│  │  └────────────────────┘  │    └──────────────────────────┘   │
│  │                          │                                   │
│  │  ┌────────────────────┐  │    Shared storage bind-mount:     │
│  │  │ GStreamer playback │◄─┼─── /mnt/ssd/library/Movies/ ──┐   │
│  │  │ + DRM mode switch  │  │                               │   │
│  │  └────────────────────┘  │    Services write completed   │   │
│  │                          │    downloads here; kiosk      │   │
│  └──────────────────────────┘    playlist system reads.     │   │
│                                                                 │
│  Network: HDMI out to modern TV (1080p-capable)                 │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
                        ▲
                        │  (network — owner's phone/laptop)
                        │
               Radarr/Prowlarr/qBittorrent
               web UIs at magicpi.local:{7878,9696,8080}
               (all authenticated)
```

## 6. Docker stack

### 6.1 Services

| Service | Image | Port | Purpose |
|---|---|---|---|
| **Radarr** | `lscr.io/linuxserver/radarr:latest` | 7878 | Movie catalog, TMDB integration, release selection, download orchestration, library import |
| **Prowlarr** | `lscr.io/linuxserver/prowlarr:latest` | 9696 | Torznab indexer aggregation; Radarr talks to Prowlarr to search many indexers at once |
| **qBittorrent** | `lscr.io/linuxserver/qbittorrent:latest` | 8080 | Torrent client with HTTP API; Radarr hands it magnet URIs to download |

All images are LinuxServer.io builds — well-maintained, Pi-aarch64 compatible, rolling updates.

### 6.2 Deployment

**Location:** `/opt/magic_dingus_box/services/` on the Pi. Contains:
- `docker-compose.yml` — the stack definition
- `.env` — secrets (Radarr/Prowlarr/qBittorrent API keys + auth passwords; not committed)
- `config/` — per-service persistent state (bind-mounted into containers)

**Systemd integration:** A new systemd unit `magic-dingus-services.service` runs `docker compose up -d` at boot and `docker compose down` at shutdown. Enables auto-start after power cycle. Ordered after Docker's daemon in the systemd dependency tree.

### 6.3 Storage layout

USB3 SSD is required (already documented in existing deployment guide — Phase 1 also required this). Layout:

```
/mnt/ssd/
├── downloads/
│   ├── incomplete/      (qBittorrent in-progress)
│   └── complete/        (qBittorrent final destination before Radarr imports)
├── library/
│   └── Movies/          (Radarr imports here with renaming scheme)
│       └── <Movie Title> (<Year>)/
│           ├── <Movie Title> (<Year>) - 1080p BluRay.mkv
│           └── ...
└── config/              (bind-mounted as Docker volumes for each service)
    ├── radarr/
    ├── prowlarr/
    └── qbittorrent/
```

**Playlist integration:** The kiosk's existing `playlist_loader` gains a new built-in "Movies" playlist source that enumerates `/mnt/ssd/library/Movies/*/` on startup and on filesystem change (inotify watch). Each subdirectory becomes a playlist entry with its .mkv/.mp4 file, poster image (from Radarr's download), and metadata. No manual playlist authoring required.

### 6.4 Security (authentication)

All three services run with authentication enabled from first boot. Random strong passwords are generated by the setup script and stored in `/opt/magic_dingus_box/services/.env` (mode 0600, root:magic ownership). Setup script also prints them once for the operator to save in a password manager.

Services bind to `0.0.0.0` (accessible from network) so the operator can admin from phone/laptop, but authentication prevents casual access. No SSH tunnel required.

**Future hardening option:** Bind to `127.0.0.1` and require SSH tunnel for access. Easy to toggle via docker-compose networks. Deferred.

### 6.5 Backup and recovery

Each service's `config/` directory holds everything (DB, settings, indexer API keys). A nightly cron job on the Pi `tar`s `/mnt/ssd/config/` → `/mnt/ssd/backups/services-<YYYYMMDD>.tar.gz` and keeps the last 7 days. Restoration is `tar -xf` over a fresh install.

## 7. Secret sequence unlock

### 7.1 The sequence

**`BTN1+BTN3 (chord), BTN2, BTN2, BTN2, RCLICK`**

| Step | Input | Description |
|---|---|---|
| 1 | `BTN1 + BTN3` pressed simultaneously | Hardware button chord (yellow + green together) |
| 2 | `BTN2` | Red button, tap |
| 3 | `BTN2` | Red button, tap |
| 4 | `BTN2` | Red button, tap |
| 5 | `RCLICK` | Rotary encoder click (commit) |

**Total:** 5 events, ~2-3 seconds to enter once memorized.

### 7.2 Detection rules

- **Timeout:** 2 seconds maximum between events. Longer pause resets sequence state silently.
- **Wrong input resets silently.** Any input that doesn't match the next expected event clears the sequence state. No feedback.
- **No feedback during entry.** Completely silent. LEDs don't flash, screen shows nothing. The only signal of success is the unlock toast appearing.
- **Detection scope:** Always-on while the kiosk process is running. Exception: RetroArch. While a game is running, kiosk input is paused (we're `waitpid`-ing on RA), so the detector naturally can't fire.
- **Already-unlocked state:** If unlocked, re-entering the sequence is a no-op. Re-locking happens only via the settings checkbox (see §7.4).

### 7.3 Chord detection (implementation note)

GPIO polling reads all 4 button states on each poll tick. A "chord" is detected when two or more buttons are simultaneously pressed within a single polling window (~10ms). The sequence detector consumes a snapshot of current press state per tick, so `BTN1+BTN3` appears as a single atomic event.

Edge case: if a user presses BTN1 and BTN3 with a small time gap (one press slightly before the other), the detector's chord window forgives up to ~50ms of skew before treating it as two sequential presses.

### 7.4 Unlock state and persistence

- **First unlock:** sequence completion sets `media_browser.unlocked = true` in `settings.yaml`, persisted via the existing `settings_persistence` module. Persists across reboots.
- **Toast:** transient overlay on current screen — "Movie section unlocked" — fades over 3 seconds.
- **Settings row:** the existing settings menu (accessed via BTN4) gains a new row: `Movies` (only rendered when `media_browser.unlocked == true`). Selecting this row enters the Media Browser.
- **Re-locking:** within the Movies settings screen, a checkbox `Hide Movies feature` when toggled clears `media_browser.unlocked` back to false, exits the Movies UI, and removes the settings row. The next time the user wants access, they must re-enter the secret sequence.

## 8. Display mode switching (1080p for movies only)

### 8.1 Current state

The kiosk's DRM/KMS initialization sets a single display mode at startup (CRT 640x480 or Modern 720p per existing `drm_display` logic). All rendering, video playback, and UI happens at this resolution.

### 8.2 New behavior

The `drm_display` module gains a `request_mode(width, height, refresh)` method that triggers a DRM mode change mid-session. Usage:

- **Enter movie playback:** `drm_display.request_mode(1920, 1080, 60)`. GStreamer pipeline reinitializes for the new viewport; movie plays at native 1080p. Widescreen aspect ratios (16:9, 2.35:1) fill or letterbox naturally via GStreamer's existing `videobox`/`videoconvert` elements.
- **Exit movie playback (return to Media Browser UI):** `drm_display.request_mode(<previous_mode>)` restores the original display mode; kiosk UI continues at its original resolution.

### 8.3 Transition UX

- Brief screen flicker during mode change is expected (~200-500ms black). Acceptable for this rare action.
- During the black interval, briefly display "Switching display mode..." text if practical, or accept silent flicker.
- If the requested mode is unavailable (e.g., display doesn't support 1080p@60), fall back to the kiosk's default mode with a log warning.

### 8.4 Scope

Mode switch is invoked **only** when entering/exiting Media Browser movie playback. The rest of the kiosk (playlists, RetroArch handoff, retro videos) is unchanged.

## 9. Kiosk UI — Media Browser screens

All screens use the existing immediate-mode renderer, font system, color theme, and controller idioms.

### 9.1 Browse screen (`browse_screen.cpp`)

- Grid of movie posters, 4–5 columns, infinite scroll
- Top category strip: **Popular** / **Now Playing** / **Top Rated** / **Discover** / **Search**
- Posters rendered from Radarr's cached thumbnail URLs (Radarr downloads artwork on its end)
- DPad or rotary to navigate; A or RCLICK to open detail
- B to exit back to main menu

### 9.2 Search screen (`search_screen.cpp`)

- Reuse existing `virtual_keyboard` widget
- Live results as you type (debounced ~400ms)
- Radarr's `GET /api/v3/movie/lookup?term=<query>` returns results; poster grid renders below the keyboard
- Highlighted result = currently focused by controller nav
- A/RCLICK opens detail

### 9.3 Detail screen (`detail_screen.cpp`)

- Fanart background (dim + blur shader) if available
- Poster + title + year + runtime + TMDB rating + certification
- Overview / synopsis text
- Actions (bottom of screen):
  - **Add to Library** (if not present) — calls Radarr `POST /api/v3/movie` to monitor + trigger search
  - **Download Now** (if already monitored) — triggers immediate search and download
  - **Remove** (if present) — unmonitor + optionally delete files
  - **Play** (if file present) — enter playback mode (DRM mode switch + GStreamer)
- On "Download Now" the UI transitions to the Queue screen so user sees progress starting.

### 9.4 Queue screen (`queue_screen.cpp`)

- List of active + queued downloads from Radarr's `GET /api/v3/queue`
- Per-entry: poster thumb, title, progress bar, download rate (KB/s), peers, ETA
- Auto-refresh every 2 seconds while visible
- Actions per entry: Pause / Resume / Cancel / Retry (with different releases)
- Global actions at bottom: **Pause all** / **Resume all** / **Retry all failed**

### 9.5 Library screen (`library_screen.cpp`)

- What's downloaded and present on disk
- Grid view like Browse, with indicators: ✅ Have (1080p) / 🟡 Have (720p / SD — upgrade available) / ❌ Missing (monitored but not downloaded yet)
- Filter chip strip: **All** / **Unwatched** / **Missing upgrades** / **Recently added**
- Selecting an entry: Play (if have file) or opens Detail (if missing)
- Controller flow matches existing playlist browser

### 9.6 Movies Settings screen (`mb_settings_screen.cpp`)

This is the kiosk-exposed subset of Radarr config. 11 items, single screen, scrollable:

| Section | Control | Radarr API |
|---|---|---|
| **Service status** | 3 colored indicators: Radarr / Prowlarr / qBittorrent | GET /api/v3/system/status (Radarr), similar for others |
| **Quality profile** | Dropdown list of existing profiles | GET /api/v3/qualityprofile |
| **Minimum seeders** | Integer slider (0–20) | Per-search query parameter |
| **Storage path** | Read-only label + free space (GB) | GET /api/v3/rootfolder |
| **Low-space pause threshold** | Slider (10–200 GB) | Enforced by kiosk-side logic (Radarr doesn't have an exact equivalent) |
| **Max concurrent downloads** | Slider (1–5) | qBittorrent API: set max active downloads |
| **Indexer toggles** | List with enable/disable switches | GET/PUT /api/v1/indexer (Prowlarr) |
| **Retry all failed** | Button | POST to Radarr with command "FailedHistoryMass" |
| **Pause all downloads** | Toggle | qBittorrent API |
| **Resume all downloads** | Toggle | qBittorrent API |
| **Hide Movies feature** | Checkbox — re-locks (requires sequence re-entry to unlock) | Kiosk-local only |
| *(fine print, bottom)* | `Advanced: magicpi.local:7878` | Informational |

## 10. RadarrClient module (kiosk-side)

### 10.1 Architecture

New C++ module under `src/media_browser/radarr/`:
```
src/media_browser/radarr/
├── radarr_client.{h,cpp}      # HTTP client, auth, request/response shape
├── radarr_types.h             # Struct definitions for Movie, QueueItem, Profile, etc.
├── radarr_parsers.{h,cpp}     # JSON → struct parsers (pure, unit-testable)
└── radarr_mock.{h,cpp}        # Mock for offline UI development + tests
```

### 10.2 Interface (key methods)

```cpp
class RadarrClient {
public:
    struct Config {
        std::string base_url = "http://localhost:7878";
        std::string api_key;   // from Radarr; stored in kiosk settings.yaml
        int timeout_secs = 10;
    };

    explicit RadarrClient(Config config);
    ~RadarrClient();

    // Service health
    bool is_reachable();
    std::optional<SystemStatus> get_status();

    // Movie discovery
    std::vector<MovieSearchHit> lookup(const std::string& query);
    std::vector<Movie> get_library();
    std::optional<Movie> get_movie(int tmdb_id);

    // Library management
    bool add_movie(int tmdb_id, int quality_profile_id, bool monitor = true);
    bool remove_movie(int radarr_id, bool delete_files = false);
    bool trigger_search(int radarr_id);

    // Queue / downloads
    std::vector<QueueItem> get_queue();
    bool cancel_queue_item(int queue_id);

    // Profiles
    std::vector<QualityProfile> get_quality_profiles();

    // Diagnostics
    const std::string& last_error() const { return last_error_; }
};
```

### 10.3 Testing

- Pure parsers (`radarr_parsers.cpp`) tested with recorded JSON fixtures in `tests/media_browser/fixtures/radarr/*.json`
- `RadarrMockClient` implements the same interface for UI-layer tests (no network required)
- `test_media_browser` CLI gains subcommands: `radarr-status`, `radarr-search`, `radarr-add`, `radarr-queue` for manual end-to-end testing on the Pi

### 10.4 Error handling

- Network failures (Radarr unreachable): UI shows the Movies screen in a "service offline" state — all screens still render but show a single "Movies service unavailable — check Docker stack" message. No crashes, no white-screen, no inaccessible UI.
- Auth failures (wrong API key): Setup script must capture Radarr's API key from Radarr's first boot and write it to kiosk settings. If wrong, same graceful degradation.
- Radarr returning unexpected shapes (API version change): parsers log an error, return empty/nullopt; UI shows "couldn't parse response."

## 11. Graceful degradation when Docker stack is down

The kiosk must behave sensibly even if the companion services aren't running. Scenarios:

| State | Kiosk behavior |
|---|---|
| All services up, healthy | Normal operation |
| Radarr down | Movies settings shows red Radarr dot. Browse/Search/Queue screens show "service unavailable"; Library screen still works (reads filesystem directly, no Radarr required). |
| qBittorrent down | Browse/Search work, but Add/Download buttons grey out; Queue shows empty with warning |
| Prowlarr down | Add-to-library still works (Radarr keeps retrying indexer search in background); Queue shows "no indexers available" if user triggers immediate search |
| All services down | Movies settings shows all red dots. Only the Library screen (filesystem read) is functional. Kiosk remains stable. |

**Key principle:** never block the user from navigating away. The kiosk must remain responsive; at worst, an information panel explains what's wrong.

## 12. Build isolation and feature gating

Inherited from Phase 1's discipline:

1. **CMake flag:** `ENABLE_MEDIA_BROWSER=OFF` by default. Production kiosk binary is bit-identical to baseline without it.
2. **Settings gate:** `media_browser.enabled` in `settings.yaml`. If false, even with the CMake flag on, no Media Browser code runs.
3. **Unlock sequence:** even with CMake flag + settings gate passed, the Movies menu item is invisible until the sequence is entered.
4. **Service dependency:** without the Docker stack running, the feature is functionally inert (graceful degradation per §11).

Four independent gates means a casual user who somehow ends up with a media-browser-enabled binary still sees nothing until three more deliberate actions (settings toggle, sequence, Docker start).

## 13. Privacy and bandwidth notes

- **Bandwidth:** qBittorrent config will set conservative defaults (max upload 1 MB/s, max download unlimited, max seeds 3). Owner can tune in qBittorrent's web UI.
- **Seeding:** qBittorrent defaults to seed-until-ratio-1.0 or 24h, whichever first. Respects the community but doesn't seed-forever.
- **VPN consideration:** Noted but not implemented. If the owner wants VPN-routed torrent traffic, a future extension could add Gluetun container to route qBittorrent's traffic through a VPN. Recommended docs pointer: LinuxServer.io's Gluetun guide.
- **Logging:** kiosk logs don't include Radarr API keys or user credentials. Secrets live in `/opt/magic_dingus_box/services/.env` and `settings.yaml` (both mode 0600).

## 14. Existing Phase 1 assets — fate

| Phase 1 module | Fate in V2 |
|---|---|
| CMake `ENABLE_MEDIA_BROWSER` flag | Reused — same gating |
| `BUILD_KIOSK` option (Mac-friendly) | Reused — still needed for Mac dev |
| Catch2 test infrastructure | Reused — covers V2 tests too |
| `LibraryDb` | Kept; used as local cache for Radarr responses (faster UI, offline resilience) |
| `TmdbClient` | Dormant (Radarr does TMDB lookups internally). Kept in tree for possible future direct use. |
| `TorrentSession` | Dormant (qBittorrent replaces it). Kept in tree; may be removed in a cleanup pass. |
| `test_media_browser` CLI | Extended with new `radarr-*` subcommands |
| Docker stack infrastructure (deploy_cpp.sh --media-browser flag) | Extended to also set up the Docker stack |

No Phase 1 code is deleted in V2. The goal is strict superset behavior.

## 15. Build order (realistic, parallelizable where possible)

### Sub-project 1 — Docker stack + systemd (Task B1, ~1 weekend, one-time)
- Write `/opt/magic_dingus_box/services/docker-compose.yml`
- Write setup script (`scripts/setup_services.sh`) — pulls images, initializes configs, captures API keys to `.env`
- Write `magic-dingus-services.service` systemd unit
- Document first-time setup in a new `docs/MEDIA_BROWSER_SERVICE_SETUP.md`

### Sub-project 2 — Sequence detector + unlock flow (Task B2, ~1 weekend)
- `sequence_detector.{h,cpp}` in `platform/` — chord + sequence state machine
- Toast renderer (`ui/toast.{h,cpp}`) — reusable transient-overlay primitive
- Wire unlock persistence to `settings_persistence`
- Add `Movies` row to settings menu (conditional on `media_browser.unlocked`)
- Unit tests for the detector (event sequences → state transitions, timeouts, reset conditions)

### Sub-project 3 — RadarrClient + mock (Task B3, ~1 weekend)
- `radarr_types.h`
- `radarr_parsers.cpp` with JSON fixture-based unit tests
- `radarr_client.cpp` — libcurl HTTP methods
- `radarr_mock.cpp` — in-memory mock for UI dev
- Extend `test_media_browser` CLI with `radarr-status`, `radarr-search`, `radarr-add`, `radarr-queue`

### Sub-project 4 — Media Browser UI screens (Task B4, ~2 weekends)
- Can begin with `RadarrMockClient` while B3 progresses in parallel
- 6 screens in order: Browse, Search, Detail, Queue, Library, Settings
- Integrates with real `RadarrClient` at end of B3

### Sub-project 5 — Display mode switching (Task B5, ~0.5 weekend)
- `drm_display.request_mode(w, h, r)` method
- GStreamer pipeline reinit on mode change
- Invoked from the Play action in Detail/Library screens

### Sub-project 6 — Playlist integration (Task B6, ~0.5 weekend)
- `playlist_loader` gains `MoviesPlaylistSource` that reads `/mnt/ssd/library/Movies/`
- Inotify watcher for live refresh
- Each movie becomes a playlist row alongside existing playlists

### Sub-project 7 — Integration, polish, Pi debugging (Task B7, ~1 weekend)
- End-to-end test scripts
- Failure-mode manual testing (service down, disk full, network offline)
- UI polish: animations, transitions, error states
- Documentation: user guide, operator setup guide

**Realistic total:** 7 weekends. Parallelizable as:
- B1 can run in isolation (mostly shell/Docker work, no C++)
- B2, B3 can run in parallel (different codebases, no interface dependencies)
- B4 depends on B3's mock (not real client); real integration at the end
- B5 and B6 are small, can fit between others
- B7 is sequential at the end

With 2 parallel Claude sessions (one on B1+B2, one on B3+B4), wall-clock drops to ~4–5 weekends.

## 16. Testing strategy

- **Unit tests (Catch2):** all parsers, sequence detector state machine, settings persistence round-trip. Target ≥95% coverage of the parser/detector layers.
- **Integration tests:** `test_media_browser` CLI against a real Radarr instance (Docker stack up) in CI or manually on the Pi.
- **UI tests:** manual checklist under `tests/manual/media_browser_v2_checklist.md`, mirroring the Phase 1 completion checklist pattern.
- **Graceful-degradation tests:** manual — start kiosk with services down, verify each screen shows sensible fallbacks.
- **Invariant tests:** production binary (flag off) remains bit-identical to pre-V2 baseline, verified at milestone commits.

## 17. Documentation deliverables

All in `magic_dingus_box_cpp/docs/`:

- `MEDIA_BROWSER_V2_DESIGN.md` — this document
- `MEDIA_BROWSER_V2_PHASE_1_PLAN.md` — implementation plan (written next)
- `MEDIA_BROWSER_SERVICE_SETUP.md` — operator guide: first-time Docker stack install, capturing API keys, troubleshooting
- `MEDIA_BROWSER_USER_GUIDE.md` — end-user guide: the sequence (secret!), how to use each screen, how to hide the feature, basic troubleshooting
- `MEDIA_BROWSER_V2_COMPLETION.md` — written at project completion, records verification results

## 18. Open questions (resolve during implementation kickoff)

1. **Docker image tags:** use `:latest` for rolling updates, or pin to specific versions (e.g., `:4.7.5`)? `:latest` is easier but can break; pinned is safer. Default: pinned, with documented upgrade path. **Decision needed at Task B1 kickoff.**
2. **Quality profile seeding:** should the setup script create a default "1080p Standard" profile automatically, or leave the user to configure via Radarr web UI? Default: seed a sensible profile ("1080p BluRay preferred, 720p acceptable, reject CAM/TS, 1080p upgrade target"). **Decision needed at Task B1 kickoff.**
3. **Artwork caching:** should the kiosk pre-cache posters for the library and popular results, or fetch on demand? Default: on-demand with simple LRU cache (200MB cap). **Decision needed at Task B3 kickoff.**
4. **Movie watched state:** does the kiosk track which movies you've already watched, or is that irrelevant for now? Default: no — that's a Phase 2 nice-to-have. **Non-blocking.**
5. **TV support:** do we add Sonarr to the Docker stack as well, or movies-only for V2? Default: movies-only. Sonarr is a natural V3 extension. **Non-blocking.**
6. **Remote control app:** Radarr has a mobile app (LunaSea). Do we mention/document this for the operator? Default: yes, mention in operator guide. **Non-blocking.**

## 19. Risks and mitigations

| Risk | Mitigation |
|---|---|
| libtorrent / qBittorrent ABI changes between Pi OS versions | Pin Docker image versions; LinuxServer.io images are stable |
| Radarr API changes between versions | Pinned versions + integration test in CI catches API drift |
| SD card wear from Docker writes | Bind-mount all persistent data to USB3 SSD; Docker daemon data dir on SSD too |
| Disk fills unexpectedly | Low-space threshold in kiosk + qBittorrent's own "max disk space" setting |
| Indexer API keys leaked in logs/docs | `.env` file mode 0600; kiosk logs never print API keys; `.env` gitignored |
| Kiosk UI out of sync with Radarr (movie removed via web UI but still showing in kiosk) | Kiosk refreshes library from Radarr on entering Browse/Library screens; not just locally cached |
| Owner forgets the secret sequence | Documented in `MEDIA_BROWSER_USER_GUIDE.md` (not a secret from the owner themselves) |
| Network outage during service setup | Setup script is idempotent; can re-run safely |
| Service container updates break things | LinuxServer.io tags + watchtower auto-update disabled; operator decides when to upgrade |

## 20. Acceptance criteria

V2 is complete when:

1. `cmake .. && make` (no flag) produces a kiosk binary **bit-identical** to the current Phase 1 baseline.
2. `cmake .. -DENABLE_MEDIA_BROWSER=ON && make` produces the kiosk binary + `test_media_browser` CLI that can talk to Radarr.
3. The Docker stack (Radarr, Prowlarr, qBittorrent) starts reliably on boot and survives reboots with state intact.
4. Entering the secret sequence from any kiosk screen produces the toast within 500ms of the final RCLICK.
5. The `Movies` row appears in settings menu **only** when unlocked.
6. The Media Browser's 6 screens render correctly and respond to controller navigation.
7. Search → Add → Download → Import → Play works end-to-end with a legal test torrent.
8. Playback is at 1920x1080 widescreen. Mode reverts on exit.
9. All graceful-degradation scenarios (§11) show sensible UI; no crashes.
10. Re-locking via settings checkbox removes the Movies row and requires sequence re-entry.
11. Operator can access Radarr's web UI from a phone on the network; casual user cannot (auth required).
12. Production kiosk functions identically to pre-V2 behavior with the flag off or services down.

## 21. References

- **Radarr** — https://wiki.servarr.com/radarr (docs), https://github.com/Radarr/Radarr (source)
- **Prowlarr** — https://wiki.servarr.com/prowlarr
- **qBittorrent** — https://github.com/qbittorrent/qBittorrent/wiki/WebUI-API
- **LinuxServer.io images** — https://docs.linuxserver.io/images/docker-radarr/ etc.
- **TMDB API** — https://developer.themoviedb.org/docs (Radarr uses this; we don't directly)
- **Phase 1 design** — `MEDIA_BROWSER_DESIGN.md` (historical, superseded by this doc)
- **Phase 1 completion record** — `MEDIA_BROWSER_PHASE_1_COMPLETION.md`

---

## Implementation-kickoff checklist

When execution begins:

- [ ] Re-read this document end to end; note anything that's become outdated
- [ ] Decide the three "decision needed at Task B1 kickoff" items in §18
- [ ] Create the branch: `feature/media-browser-v2` (branched off current `claude/determined-mclean-e4c3cd` to inherit Phase 1 work)
- [ ] Set up worktree(s) for parallel execution
- [ ] Begin with Task B1 (Docker stack — mostly shell work, unblocks testing for later tasks)
- [ ] Write `MEDIA_BROWSER_V2_PHASE_1_PLAN.md` with bite-sized task steps before any C++ work
