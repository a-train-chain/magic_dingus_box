# Marquee Personalization + TV — Design

**Date:** 2026-07-31
**Status:** Approved direction; Phase 1 ready for implementation planning. Phases 2–3 are design-level and get their own plan cycles when Phase 1 ships.
**Repo:** `magic_dingus_box_cpp` (Media Browser, `ENABLE_MEDIA_BROWSER=ON` builds)

## Problem

The Media Browser's Popular and Top Rated tabs show near-identical grids every visit:

1. **Deterministic fetches (the dominant cause).** Every load requests page 1 of TMDB's global `/movie/popular` or `/movie/top_rated` (`src/media_browser/ui/browse_screen.cpp:504`). Popular drifts slowly; Top Rated is effectively static. Pagination tops out at `kMaxLoadedPages=5` (~100 titles) of TMDB's 500+ pages.
2. **Same-tab re-entry replays memory.** `BrowseScreen` is a process-lifetime object; `enter()` fetches only when `!loaded_`, and `loaded_` is never reset (`browse_screen.cpp:275-288`), so re-entering the Media Browser replays the landing tab's cached grid. (Tab switches *do* refetch — every BTN1/BTN3 activation calls `load_category()` unconditionally (`browse_screen.cpp:894,910`) — but the refetch is deterministic, so it looks identical anyway.)

There is also no personalization (nothing uses the owner's library as a taste signal) and no TV support anywhere in the stack.

## Approved decisions

1. **For You** is a new third content tab. Popular and Top Rated stay honest global charts (freshened + shuffleable).
2. **Shuffle** lives in the existing BTN4 filter overlay as a new action row. (A rotary long-press binding may be added later; not in scope.)
3. **TV** is a Movies/TV mode toggle inside the filter overlay, switching the content tabs' data source — not extra tabs.
4. **TV downloads default to season-at-a-time** (Season 1 on first add, one-press "next season" afterwards). Whole-series is available behind an explicit confirm with a disk estimate.

---

## Phase 1 — Fresh, personalized, refreshable movie rows

Phase 1 lands as three separately shippable milestones in order: **1a → 1b → 1c**. 1a and 1b are small; 1c is the bulk of the work.

### 1a′. Prerequisite: list results carry a success signal and `total_pages`

`TmdbClient`'s list calls currently return a bare `std::vector<TmdbSearchHit>` that is empty on **both** HTTP failure and a genuinely empty page — no caller can tell the difference — and `parse_list_response` discards TMDB's `total_pages` (`tmdb_client.cpp:368-397`). Both 1a and 1b need those signals, so the list-call return shape changes once, up front: a small `TmdbList { bool ok; int total_pages; std::vector<TmdbSearchHit> hits; }` result for `get_popular`/`get_top_rated`/`discover` (and the recommendation calls, for 1c's benefit). This is the only `tmdb_client` API change in Phase 1; existing non-list callers are untouched.

### 1a. Staleness TTL

- BrowseScreen keeps an in-memory `last_loaded_at` (`std::chrono::steady_clock`) per **chart grid** (the active Popular/Top Rated grid; For You keeps its own — see 1c), updated whenever that grid's base-page result lands. On `enter()`, if the landing grid is older than **6 hours**, refresh it. Search, Library, and Queue are out of scope.
- **Stale-while-revalidate (chart tabs).** The TTL refresh does *not* clear the grid or show the Loading state. At issue time it bumps `tmdb_current_gen_` and resets the tracked base page to 1 — so in-flight pages from the old (possibly shuffled) window are discarded rather than appended after the swap — but defers all visible resets. It then fetches page 1 of the canonical chart in the background via a revalidate-tagged pending-page. When the result lands **ok and non-empty**, the existing page-1-replace branch in `apply_pending` (`browse_screen.cpp:640-653`) swaps the grid and resets the cursor to top-left, and `last_loaded_at` updates. When the result is `!ok` **or empty** (indistinguishable network failure/empty page — the revalidate path treats both as failure), the swap is skipped: the old grid stays, `last_loaded_at` stays, and the next `enter()` retries. The error state appears only when there is no prior data.
- **Stale-while-revalidate (For You).** TTL expiry on For You draws a fresh seed sample and reruns the merge *in the background*: the old grid stays visible (no clear, no Loading) and swaps only when the new merge completes with at least one successful seed; on total failure the old grid stays. (Explicit SHUFFLE, by contrast, clears and shows Loading — the user asked for a reload and should see one.)
- Evaluated on `enter()` only, by design: chart tab switches already refetch unconditionally, and a session parked on one tab inside MB may exceed the TTL until the next entry. No persistence; a reboot naturally refetches.

### 1b. Shuffle (filter overlay action row)

**Overlay changes.** `FilterOverlay` gains a **SHUFFLE** action row, rendered like RESET ALL (`render_reset_row`, `mb_filter_overlay.cpp:314-330`, invoked at `:456-458`). The overlay's row model is currently hardcoded (`kFocusableRowCount=7`, with row 6 special-cased as RESET — `mb_filter_overlay.h:100`, no readers outside the overlay); it becomes a **per-tab row configuration keyed by `FilterTabKind`** so Popular/Top Rated get their 6 filter rows + RESET + SHUFFLE, while For You opens with SHUFFLE only in Phase 1 (Phase 2's Movies/TV mode row will join it — do not hard-code a single-row shape).

**Semantics.** `FilterOverlay` gains a second callback (`set_on_shuffle`) beside the existing commit callback. Selecting SHUFFLE closes the overlay via the commit-free `close()` path and fires `on_shuffle`, whose BrowseScreen handler first **persists any staged `working_` edits** and then performs one shuffle load under the resulting filters — the persist logic is extracted from the existing commit lambda (`browse_screen.cpp:873-878`) into a shared helper so the two paths cannot drift, and no separate commit reload runs. Additionally, the plain BTN4-close commit is skipped when `working_` is unchanged since the overlay opened — today it unconditionally commits + reloads (`mb_filter_overlay.cpp:243-262`), which would otherwise silently replace a shuffled grid with the canonical chart the moment the user peeks at the overlay.

**Shuffle load.** A new entry point `load_shuffle(category, base_page)` mirrors `load_category` (`browse_screen.cpp:480-504`): synchronously clear `movies_`/`loaded_tmdb_ids_`, reset cursor and scroll, bump `tmdb_current_gen_`, set the Loading state, then spawn the worker at the random base page. (The reset cannot be left to `apply_pending` — its reset branch only fires for page 1 — and `spawn_page_worker` alone does not bump the generation counter.)

**Pagination window.** `maybe_load_more_pages` (`browse_screen.cpp:676-700`) currently treats `kMaxLoadedPages=5` as an **absolute page-number cap** and only prefetches page 2 when the base is 1. Both must be generalized: track the shuffle base page, load the window `[base, base + kMaxLoadedPages − 1]`, and prefetch `base + 1` after the base page lands. Without this, any base ≥ 6 would load a single ~18-title page.

**Base-page ranges** (chosen so `base + 4` stays inside the page-quality ceilings):

- **Popular:** uniform random base in **1–26** (window never passes page 30; deeper `/movie/popular` pages degrade fast).
- **Top Rated:** uniform random base in **1–21** (window never passes page 25, ~the all-time top 500).
- **Filters active:** shuffle randomizes the `/discover/movie` page instead, preserving the (just-committed) filters, drawing from `1 … min(26, max(1, total_pages − kMaxLoadedPages + 1))`. `total_pages` comes from a per-filter-signature cache of the last-seen `TmdbList.total_pages` (populated by any prior discover fetch under those filters); when the just-committed filters have no cached value, clamp optimistically to 26 and rely on the fallback below (accepting one extra ~6 s round-trip in the worst case).
- **Current-base exclusion (both paths):** the draw excludes the current base (re-roll on collision) whenever the range holds at least two candidates; if it collapses to a single page (possible on the filters path), keep the current base and let the shuffle be a plain refetch.
- **Empty-page fallback:** if the fetched base page comes back empty, fall back to page 1.

A shuffle whose base page lands updates that grid's `last_loaded_at`. The overlay is reachable from the phone remote today (BLACK short-press = BTN4), so shuffle needs zero remote-side work.

### 1c. For You tab

**Tab strip.** New `Category::ForYou` between Top Rated and Search: `Popular | Top Rated | For You | Search | Library | Queue | Settings`. As a targeted cleanup, the two manually-duplicated `kVisibleTabs` arrays (`browse_screen.cpp:810` input path, `:976` render path, with a "keep in sync" comment) are consolidated into a single shared constant so the new tab is added exactly once.

**Header fit.** The header right-aligns the chip strip with no overflow handling (`mb_chrome.cpp:553-568`), and a 7th chip grows the strip past the "Marquee" title — measured with the real ZenDots metrics, the 7-chip strip overlaps the title by ~58 px on the 1280-wide logical canvas. Mitigation: tighten the strip constants (`kTabHorizPad` 16→10, `kTabGap` 24→16, `mb_chrome.cpp:40-43`), which recovers ~130 px; verify no overlap in the hardware pass.

**Library signal.** For You samples from the Radarr library id cache (`library_tmdb_ids_`), which is populated **asynchronously** by `refresh_library_async` (kicked only from `enter()`, `browse_screen.cpp:287`) and is empty on first entry (`browse_screen.h:283-286`). Two additions make its state legible:

- `RadarrClient::get_library()` gains a success signal (it currently returns an empty vector on both HTTP failure and a genuinely empty library, `radarr_client.cpp:150-154`) — this adds `radarr_client.{h,cpp}` to the Phase 1 surface.
- BrowseScreen records whether a library refresh has completed at least once and whether the most recent one's library fetch succeeded (complementing the existing `services_ok_`/`library_cached_` flags).

**Entry rule (three-way).** On For You activation:

- **(a) Most recent refresh completed successfully** → sample immediately from the cached ids. For You is **exempt from the unconditional tab-activation refetch**: activating the tab re-renders the cached merged list when one exists; a new sample runs only on first entry, TTL expiry, or SHUFFLE. (For You keeps its own `last_loaded_at`; 1a's chart timestamps don't cover it.)
- **(b) Refresh in flight** (entering triggers one if none is) → Loading state; sampling kicks off from `apply_library_pending()` when the result lands (gated on `category_ == ForYou`).
- **(c) Most recent refresh completed as failure** → the existing service-unavailable state. (An in-flight refresh shows Loading, never this state.)

**Algorithm (seed-sampled recommendations):**

1. Sample **min(8, library size)** seed titles uniformly without replacement from `library_tmdb_ids_`.
2. For each seed, one `WorkerPool` worker fetches `TmdbClient::get_recommendations(seed, page 1)` and, on an empty result, falls back to `get_similar(seed, page 1)` in the same worker — the documented contract already proven in the playback overlay (`playback_overlay.cpp:161-176`). The pool spawns a thread per task, so all 8 fetches run concurrently; the serial fallback hop makes the worst case ~12 s over the ~6 s/call VPN egress, covered by the existing static "Loading..." text.
3. **Join:** a per-generation completion counter triggers the merge when every seed worker has completed or failed; a stale generation abandons the merge. Partial failures merge whatever returned.
4. **Merge** — implemented as a pure free function `merge_recommendations(vector<vector<TmdbSearchHit>>, exclude_set) -> vector<TmdbSearchHit>` so it is unit-testable without network: de-duplicate by tmdb id; **score = number of distinct seeds that recommended the title**; ties broken by the minimum index the title holds across all seed lists, remaining ties by ascending tmdb id; drop anything in `exclude_set` (the library ids); cap at exactly **100**. Results arrive already family-safe-trimmed by the shared `parse_list_response` parser (`TmdbSearchHit` carries no adult field — there is nothing further to filter here).
5. Render into the normal poster grid. **For You never issues scroll-driven page loads** — the merged list is the complete data set and scrolling stops at its end.

**Timestamp rule.** A completed merge with at least one successful seed updates For You's `last_loaded_at` (a partially-degraded grid is accepted until the next TTL window). If every seed fails: on an explicit load (first entry, SHUFFLE) show the existing error state; on a TTL refresh keep the old grid. Either way the timestamp is left untouched so the next `enter()` retries.

**Shuffle on For You** = draw a fresh random seed sample and rerun with clear + Loading (see 1a for the background TTL variant).

**Edge cases:**

- **Empty cache, refresh succeeded** (library genuinely empty): centered empty-state message "Add movies to your library to get recommendations" (same pattern as the existing no-API-key message lines). No silent fallback to Popular — the tab should teach what feeds it.
- **Library ≤ 8 titles:** every sample is the full library and the merge is deterministic, so SHUFFLE and TTL re-draws regenerate an identical grid — accepted for Phase 1 (the lever, if it ever bothers, is randomizing the recommendations page per seed). With 1–3 titles the results feel like "similar to X" — also acceptable.
- **Partial fetch failures:** merge whatever seeds returned; only if *all* seeds fail apply the timestamp rule above.

**Filter overlay on For You:** SHUFFLE only in Phase 1 (recommendations are already taste-scoped; post-filters are future work). `FilterTabKind` (`mb_filter_overlay.h:14-17`) gains a `ForYou` value; no persisted filter state for the tab, so no `settings.json`/`app_state.h` additions in Phase 1.

**Persistence:** none. Seeds resample on first entry, TTL expiry, and shuffle — never on mere tab activation.

### Phase 1 testing

- Unit: `merge_recommendations` scoring/tie-break/dedup/exclusion/cap; shuffle base-page selection (bounds, current-base exclusion incl. the collapsed-range case, filters-active clamp with and without a cached `total_pages`); TTL decision logic and the For You three-way entry rule (extract as small pure helpers); `TmdbList` parsing (`ok`, `total_pages`, hits).
- No recommendations fixture exists (`tests/media_browser/fixtures/tmdb/` holds only `popular.json` and `genres.json`): add a `recommendations.json` fixture and a parse test alongside the existing list-parse tests.
- Hardware pass: tab strip walk (now 7 chips — confirm no title overlap after the spacing change), For You on the real library, shuffle from both enclosure and phone remote, overlay peek after a shuffle (grid must survive), TTL behavior across an MB exit/re-enter, stale-while-revalidate with the network cable pulled (old grid must survive).

### Phase 1 interactions / risks

- The BTN1/BTN3 double-fire dispatcher fix **has already landed in the working tree** (uncommitted `main.cpp` change from the parallel session; the dispatcher now carries a "ONE SEMANTIC PER PHYSICAL INPUT" invariant at `main.cpp:1954-1967`). Phase 1 adds **no dispatcher bindings** and should avoid `main.cpp` edits until that change is committed; expected Phase 1 surface is `browse_screen.{h,cpp}`, `mb_filter_overlay.{h,cpp}`, `mb_chrome.{h,cpp}` (strip spacing), `tmdb_client.{h,cpp}` (TmdbList), `radarr_client.{h,cpp}` (get_library success signal), and tests.
- Memory: no new caches beyond the small per-filter-signature `total_pages` map; the grid window stays ~100 titles; artwork continues through the existing LRU `ArtworkCache`. Safe on the 2GB Pi.
- TMDB rate limits: worst case per For You load is 8–16 list calls (recommendations + fallbacks) — well under TMDB's ~50 req/s cap.

---

## Phase 2 — TV via Sonarr (design level; own plan when Phase 1 ships)

Nothing TV-shaped exists today: Radarr's model is one-video-per-title, `TmdbClient` is `/movie/*`-only, Prowlarr availability search hardcodes movie categories 2000-2080 (`prowlarr_client.cpp:209-214`), and playback accepts exactly one file path. The V2 design already names the path: **add Sonarr** ("a natural V3 extension", `MEDIA_BROWSER_V2_DESIGN.md:463`). The superseded V1 doc's seasons/episodes model (`MEDIA_BROWSER_DESIGN.md:157-186`) is the in-house design reference.

**Stack.** Pin a **Sonarr 4.x** image using the full linuxserver `<version>-lsNN` tag (e.g. `lscr.io/linuxserver/sonarr:4.0.19.2979-ls320` — verified current stable, 2026-07; custom formats were introduced in Sonarr v4; "v3" below refers only to the API route namespace, which Sonarr 4 still serves, with `X-Api-Key` auth). The container joins the existing Gluetun network namespace beside Radarr in `services/docker-compose.yml`. The integration surface is wider than the compose file — all of these hardcode the netns-dependent set and must gain sonarr or it silently drifts:

- Gluetun's `ports:` section gains **8989** (behind-netns services expose ports only there; the kiosk can't reach Sonarr otherwise).
- `scripts/gluetun_cascade_restart.sh` `DEPENDENTS` and `PAUSED_CONTAINERS`; `scripts/playback_services_pause.sh` `CONTAINERS` (Sonarr pauses during playback like Radarr on the 2GB boxes).
- The VPN health monitor's port list (`vpn_health_monitor.cpp:22`).
- Quality fixtures mirror the movie set in `scripts/data/` (profile limited to 720p/1080p with the same AV1/HEVC/HDR penalty custom formats; per-episode caps via MB/min definitions), reconciled by a parallel block in `setup_services.sh`, with matching `verify_services.sh` smoke coverage.

**Indexers.** Prowlarr syncs the **TV-capable subset** of the existing indexers to the Sonarr app (YTS is movies-only and will not sync). Decision item for the Phase 2 plan: enable **EZTV**, already present-but-disabled in `scripts/data/prowlarr_indexers.json`, so season-pack coverage doesn't ride on TPB/LimeTorrents/TorrentDownload/Knaben alone.

**Storage.** Sonarr root folder **`/data/library/tv`** ↔ host **`/mnt/ssd/library/tv`**, under the existing single `${STORAGE_ROOT}:/data` mount — no new bind mounts, preserving the hardlink-import property the repo just migrated to (`docker-compose.yml:30-45`, `migrate_hardlink_layout.sh`). `SonarrClient::Config` carries the matching container/host prefix pair, mirroring `RadarrClient` (`radarr_client.h:29-31`).

**Clients.**

- `TmdbClient` grows TV endpoints — `get_tv_popular`, `get_tv_top_rated`, `discover_tv` (`first_air_date.gte/.lte` — verified; NOT `air_date.*`, which matches any episode's date), `get_tv_detail` (with seasons array), `get_tv_recommendations`/`get_tv_similar`, `get_tv_genres`. Parsers read `name`/`first_air_date` alongside `title`/`release_date`; `TmdbSearchHit` gains a `kind` field (`movie|tv`, defaulting to movie so existing callers are untouched). **TV family-safe mechanics (verified against TMDB docs):** the `adult` field is ABSENT from `/tv/popular` and `/tv/top_rated` rows and `include_adult` exists only on `/search/tv` and `/discover/tv` — parsers treat `adult` as optional-default-false on every TV row and drop `true` rows; TMDB's adult flag means pornography only (largely absent from TV), so the movie policy (R-rated kept, porn blocked) carries over with **no TV-MA certification gate** and unrated shows allowed. TV genre ids are a different space (16 genres; 10759 Action & Adventure, 10765 Sci-Fi & Fantasy replace several movie ids) — the per-mode genre tables must not share ids. **Extend the existing class** — the win is reusing `http_get`'s VPN-egress retry/backoff, API-key handling, error mutex, and the shared parsers. (A sibling client would not be unsafe — libcurl ref-counts `curl_global_init`/`cleanup` pairs and `RadarrClient` already runs its own matched pair — it would just duplicate all of the above.)
- New `SonarrClient` mirroring the `RadarrClient` subset actually used (Sonarr 4's API is deliberately similar). Verified integration shapes: series resolve via `GET /api/v3/series/lookup?term=tmdb:<id>` (Sonarr's own TMDB delegation path; fall back to a title search when the TMDB→TVDB mapping is missing, and handle already-added via `GET /api/v3/series?tvdbId=`); add = `POST /api/v3/series` from the lookup resource + `qualityProfileId`/`rootFolderPath`/`addOptions.monitor="firstSeason"` + `searchForMissingEpisodes` (camelCase enums); next-season = `PUT /api/v3/series/{id}` with the full resource (season monitored flags edited) + `SeasonSearch {seriesId, seasonNumber}` command; library list, per-episode queue (parse `sizeleft` tolerantly — an upstream rename to `sizeLeft` is staged), root folders, quality profiles, and `GET /api/v3/history/series` for the orphan-proof remove's download hashes. Note `minimumAvailability` does NOT exist for series (Radarr-only) — the nearest analog, `monitorNewItems`, defaults appropriately and needs no kiosk surface.

**UI.**

- **Movies/TV toggle** = a mode row in the filter overlay applying to the three content tabs; strip labels re-render per mode ("Popular · TV"). Mode persists in `settings.json`. **Popular and Top Rated each persist two independent filter sets keyed by mode**; toggling swaps sets without clearing either, and genre ids never cross modes (movie ids from `/genre/movie/list`, TV ids from `/genre/tv/list`). **For You persists no filter state in either mode** (its overlay stays mode row + SHUFFLE until post-filters land). **Search follows the active mode** (`/search/tv` in TV mode); **Library and Queue always show both kinds** with a type badge and ignore the toggle.
- **Series detail screen:** poster/overview plus a season list with per-season state (none / downloading / complete, episode counts from Sonarr). Actions: "Add Season 1" (first add, default), "Download next season", "Whole series…" (confirm modal with disk estimate), Remove.
- **Queue:** Sonarr's `/api/v3/queue` returns **one record per episode**; a season pack appears as N rows sharing a single `downloadId`. The kiosk groups by `downloadId` — one row per download with series/season label and episode count — so the existing qBit hash overlay maps 1:1 to grouped rows.
- **For You (TV mode):** same seed-sampled algorithm over the Sonarr library with `get_tv_recommendations`.

**Download granularity.** Sonarr monitors per-season/per-episode natively, so season-at-a-time is configuration, not new engineering: first add = Season 1 monitored + a season search; "next season" flips monitoring on the next unmonitored season and triggers its search; whole-series sets all seasons monitored after the confirm. **Season searches admit season packs, which typically win for finished seasons** — there is no "prefer packs" toggle in Sonarr, and currently-airing seasons arrive per-episode via RSS.

**Disk safety.** Estimate = **episode count × series runtime (Sonarr's `series.runtime`) × preferred MB/min** from the Sonarr quality definitions (~1–2 GB per 42–60-min episode at the retuned rates; a GoT-scale series ≈ 50–100 GB, one season 5–15 GB). Note this introduces the codebase's **first blocking preflight**: the movie flow only warns via Toast below 15 GB and never blocks (`detail_screen.cpp:809-829`). Whole-series adds block when the estimate exceeds free space minus a 20 GB floor, showing the estimate in the confirm modal; if the free-space stat fails, fall back to warn-only (fail-open, matching the movie flow's philosophy).

## Phase 3 — TV playback (later)

Playback currently takes one path (`PlaybackScreen::set_movie`). Series need an episode picker on the series detail screen (per-episode file paths from Sonarr), "next episode" on end-of-stream, and — ideally — watch-position tracking. The dormant `LibraryDb` SQLite store is the natural home, but note it supplies the **migration framework and a `kind IN ('movie','tv')` column, not a TV-ready schema**: its shipped migrations end at v2 with no seasons/episodes tables, and its `history` table is a download-event log. Phase 3 therefore adds new migrations (seasons/episodes per the V1 model, plus a watched/watch-position store), which is also the moment the Library "Unwatched" filter becomes real (`library_view.h:41-44` anticipates `Movie.watched`).

## Out of scope

- Watch-history capture for movies (valuable, but For You works from the library signal alone; revisit with Phase 3).
- Rotary long-press shuffle binding; post-filters on For You; Trakt/Letterboxd sync; multi-user profiles.
- Any change to the locked-off (`ENABLE_MEDIA_BROWSER=OFF`) binary — the bit-identical invariant holds.
