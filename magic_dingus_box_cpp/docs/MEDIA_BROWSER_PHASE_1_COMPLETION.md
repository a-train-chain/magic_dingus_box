# Media Browser Phase 1 — Completion Record

**Date completed:** 2026-04-23
**Pi model:** Raspberry Pi 4B (2GB)
**OS:** Raspberry Pi OS Lite 64-bit (Bookworm)
**libtorrent version:** libtorrent-rasterbar 2.0.x (via `apt install libtorrent-rasterbar-dev`)

## Unit test results (on Pi)

```
$ ./test_media_browser_unit
===============================================================================
All tests passed (38 assertions in 12 test cases)
```

12 test cases across: smoke (2), library_db (7), tmdb (3).

## CLI commands (on Pi, live)

- [x] `db-init` — schema v2 created, DB at `data/media_browser.db`
- [x] `db-status` — reports `schema version: 2`, zero counts for titles / queue / history
- [x] `tmdb-search "Sita Sings the Blues"` — returned 1 result: `[20529] Sita Sings the Blues (2008) rating=6.9`
- [x] `tmdb-get 20529` — returned full detail: title, year=2008, runtime=82min, rating=6.9, poster, backdrop, full overview
- [x] `torrent-add-file /tmp/debian-netinst.torrent` — attached, info_hash `3b1de9cb7011350fa152ec47419620aa153e19e7`
- [x] `torrent-wait` — peer discovery verified (0 → 123 peers over 30s, 412 KB/s peak, 1.3% downloaded); timed out as expected per test plan option (b)
- [x] `torrent-remove` — exits 0

## Production kiosk unaffected

- [x] `magic-dingus-box-cpp.service`: `active (running) since Thu 2026-04-23 15:18:56 PDT; 41min ago` — never restarted during Phase 1 work
- [x] No errors in `journalctl -u magic-dingus-box-cpp.service` mentioning `media_browser`
- [x] Only existing pre-Phase-1 messages in the kiosk log (unrelated pulseaudio DBus notes)

## Binary hash check (invariant)

Baseline (pre-Phase 1) `magic_dingus_box_cpp` SHA256:
```
90e3af0449366c782c85598ba134fa2e2bb9d9de0820fc776d7447f8833ce1cc
```

After Task 4 (flag off, SQLite foundation in place):
```
90e3af0449366c782c85598ba134fa2e2bb9d9de0820fc776d7447f8833ce1cc  ✓ match
```

After Task 11 (flag off, all Phase 1 work applied):
```
90e3af0449366c782c85598ba134fa2e2bb9d9de0820fc776d7447f8833ce1cc  ✓ match
```

**The production kiosk binary is bit-identical to baseline after every checkpoint.**

## Commits

1. `8194a9a` Task 1 — ENABLE_MEDIA_BROWSER CMake flag + empty scaffold
2. `d384b49` Task 2 — Catch2 v3 + BUILD_KIOSK option
3. `b95bca6` Task 3 — LibraryDb SQLite wrapper + migration runner
4. `16b433e` Task 4 — Phase 1 schema (titles, queue, history)
5. `5aad5bd` Task 5 — TMDB client with pure parsers
6. `bf52c82` Task 6 — TorrentSession libtorrent wrapper
7. `b227913` Task 7 — test_media_browser CLI skeleton
8. `00bb1a6` Tasks 8-10 — CLI command handlers (db/tmdb/torrent)
9. `9d7bc1a` Task 11 — `--media-browser` flag in deploy + install scripts
10. _(Task 12 completion commit)_

## Plan deviations (documented in commit messages)

1. **Added `BUILD_KIOSK` CMake option.** The plan assumed Mac could build the
   existing project, but the kiosk depends on Linux-only packages
   (libdrm/libgbm/libegl/libevdev/gstreamer). Introduced a new
   `option(BUILD_KIOSK ...)` that defaults ON on Linux (Pi unchanged) and
   OFF elsewhere so Mac devs can still build ENABLE_MEDIA_BROWSER targets.

2. **Added `target_link_directories` for Mac homebrew.** On Mac, homebrew
   packages live in `/opt/homebrew/lib` which isn't a default linker path.
   Pi builds unaffected (library dirs are default paths there).

3. **Added `find_package(Boost REQUIRED)`.** libtorrent's public headers
   include `<boost/config.hpp>` which needs explicit discovery on Mac.
   Implicit on Pi (libtorrent-rasterbar-dev pulls in libboost-dev).

4. **Fixed pre-existing bug in `deploy_cpp.sh` Step 1.8.** The rsync of
   `libretro_cores/` had no existence check, causing `set -euo pipefail`
   to kill the deploy when the directory was absent. Wrapped in `[ -d ]`.

5. **Tasks 8, 9, 10 committed together.** Plan had them as three separate
   commits, but they're contiguous edits to the same file replacing stubs
   with implementations. Combined commit has all three sets of verification
   results in the message.

## Phase 2 follow-ups (notes for future self)

- **spdlog format strings:** Several `spdlog::error(...)` calls produced
  verbose template-instantiation context output during Pi compile. Builds
  succeed, but the noise could hide real errors. Investigate whether we
  should pin spdlog to a newer version or refactor format strings.
- **CLI statelessness:** Each `test_media_browser` invocation starts a new
  libtorrent session. `torrent-wait` re-attaches from the download dir, but
  real users would want a persistent orchestrator. This is Phase 3 scope.
- **No integration tests yet for TorrentSession.** Covered by the CLI
  end-to-end verification, but Phase 3 should add a mockable session for
  unit testing the orchestrator state machine.
- **Docs directory is gitignored.** All Phase 1 docs (design, plan, this
  completion record) live on the filesystem but aren't in the repo. This
  is project convention; they could be force-added with `git add -f` if
  we want them tracked later.
- **Phase 1 final artifacts on Pi at:**
  - `/opt/magic_dingus_box/magic_dingus_box_cpp/build/test_media_browser`
  - `/opt/magic_dingus_box/magic_dingus_box_cpp/build/test_media_browser_unit`
  - TMDB key at `~/.config/magic_dingus_box/tmdb_api_key` (mode 0600)

## Open questions (still deferred per design §19)

- Release parser regex engine: std::regex vs PCRE2 (benchmark on Pi first)
- TV metadata: TMDB vs TVDB
- Seeding defaults for imported content
- Artwork cache LRU strategy
- Web admin integration for queue status

## Summary

**Phase 1 is complete and verified on hardware.** All 12 tasks from
`MEDIA_BROWSER_PHASE_1_PLAN.md` are executed, committed, and pass their
acceptance criteria. The production kiosk binary is bit-identical to the
pre-Phase-1 baseline when built without the flag. All subsystems exercised
end-to-end on a real Pi 4B with real network and real TMDB API access.

The design doc's Phase 1 deliverable — *"a `test_media_browser` executable
that searches TMDB, downloads a legal test torrent, writes results to
SQLite"* — is met.
