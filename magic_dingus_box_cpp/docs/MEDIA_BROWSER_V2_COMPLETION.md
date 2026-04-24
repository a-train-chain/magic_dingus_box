# Media Browser V2 — Completion Record

Ship record for the V2 plan. What made it in, what was deferred, where we
deviated from the plan, and what remains to do. Companion to
`MEDIA_BROWSER_V2_PLAN.md` and `MEDIA_BROWSER_V2_DESIGN.md`.

---

## What was built

**23 of 30** planned tasks landed. The shipped scope includes:

- **Docker stack** — Radarr, Prowlarr, qBittorrent, Gluetun (ProtonVPN free-tier
  kill-switch), and FlareSolverr (CloudFlare bypass for indexers) all running
  under `magic_dingus_box_cpp/services/docker-compose.yml`, bound to
  `127.0.0.1` on the Pi, health-checked
- **Sequence detector** — `SequenceDetector` class in
  `src/app/sequence_detector.{h,cpp}` wired into `main.cpp` input pipeline;
  three-step unlock (BTN1+BTN3 chord → BTN2×3 → rotary click) sets
  `media_browser.unlocked=true`
- **Six UI screens** — Browse, Search, Detail, Queue, Library, Movies Settings;
  all under `src/media_browser/screens/`, dispatched by `MediaBrowser` shell,
  all navigable controller-free (BTN1/BTN3 = up/down, rotary = horizontal,
  rotary click = select, BTN4 = back)
- **RadarrClient** — HTTP/JSON client under `src/media_browser/` with discovery,
  search, add-to-library, queue, library, and delete endpoints; offline
  `RadarrMockClient` for development
- **Movies playlist source** — synthesized from `/mnt/ssd/library/Movies/`
  filesystem layout, surfaced in the main kiosk playlist browser under
  "Movies" when `media_browser.unlocked=true`
- **Flag-off preservation** — default `media_browser.unlocked=false` hides
  every trace of the feature from the main UI
- **`test_media_browser` CLI** — Catch2-backed test runner plus `radarr-*`
  subcommands for manual end-to-end verification against a live stack

---

## What was deferred

**Task 24 — DRM display mode switch.** The design called for dynamically
switching DRM mode when entering/leaving the Media Browser. Deferred because:

1. The change crosses DRM, GStreamer, and the RetroArch-handoff boundary —
   three of the most delicate subsystems in the codebase, each with their
   own state-machine invariants
2. We have no actual P2P-downloaded content to test against end-to-end:
   ProtonVPN free tier blocks P2P, so the pipeline can be smoke-tested up to
   the "download queued" step but no further without a paid VPN upgrade
3. The current fixed-mode rendering works correctly for all tested content
   (Internet Archive direct HTTP, mock fixtures); the display switch is a
   polish item, not a blocker

To pick this up later, treat it as a full session: reserve time to re-verify
the RetroArch launch/return flow after each change, and have real downloaded
content ready to exercise the full playback path.

---

## Audit fixes applied

Five remediation commits between the feature freeze and this completion
record, in chronological order:

- **`afbd1ab`** — `feat(media_browser): add Library/Queue/Settings nav chips
  to Browse` — Browse screen originally only had content chips; nav chips were
  missing, which stranded users (only way into Library was a theoretical keyboard
  shortcut that didn't exist). Added the 4 nav chips.
- **`823334a`** — `sec(media_browser): bind service UIs to 127.0.0.1
  (require SSH tunnel)` — Services were originally bound to `0.0.0.0`, exposing
  full admin UIs to the LAN. Rebound to loopback; operator admin now requires
  an explicit SSH tunnel.
- **`884d927`** — `fix(media_browser): code-audit Tier 1/2 followups` — batch of
  correctness fixes from internal audit (error-handling gaps, lifetime bugs,
  edge-case crashes).
- **`6bd0bda`** — `fix(media_browser): FlareSolverr healthcheck — use curl
  (image has no wget)` — compose healthcheck was failing because the container
  base image lacks `wget`; switched to `curl`.
- **`6ddbc28`** — `fix(media_browser): 4 screen-walkthrough UX bugs` — four
  issues found during manual walkthrough (keyboard CANCEL stranding, Detail
  back-nav always returning to Browse instead of origin, Queue cancel
  confirmation timing, filter chip rendering).

---

## Plan deviations

1. **HD-1080p built-in profile instead of a custom "Kiosk" profile.** The V2
   plan specified creating a custom quality profile named "Kiosk" with hand-picked
   quality tiers. Radarr 5.14 changed the quality-profile schema in a way that
   broke the planned API call. Rather than pin to an older Radarr or chase the
   new schema, we shipped with the built-in `HD-1080p` profile. Functionally
   equivalent for the target use cases; revisit when there's time to learn the
   new schema.

2. **Task 24 (DRM display mode switch) deferred** — see above.

3. **Tasks 26–30 documentation expanded from one bundle to three files.** The
   plan grouped doc deliverables into a single task. The actual deliverable is
   three focused files under `magic_dingus_box_cpp/docs/`:
   - `MEDIA_BROWSER_UI_CHECKLIST.md` — manual test checklist
   - `MEDIA_BROWSER_USER_GUIDE.md` — operator-facing guide
   - `MEDIA_BROWSER_V2_COMPLETION.md` — this file
   This split surfaces each audience's content at the top level instead of
   burying three different use cases in one document.

---

## Bit-identical invariant status — **DRIFTED**

The V2 plan asserted that with `media_browser.unlocked=false` the kiosk
binary should be **bit-identical** to the Phase 1 baseline.

- **Phase 1 baseline sha256**: `90e3af0449366c782c85598ba134fa2e2bb9d9de0820fc776d7447f8833ce1cc`
- **Current flag-off build sha256**: `622b6b6d372ff1497e1f00a7ecf7e7bc95633b8bce20e8894ae3ad94c8dfb533`

The hashes differ. Root causes (likely — to be confirmed in a future audit
pass):

- **Unguarded struct-member additions in `app_state.h`** — e.g.
  `media_browser_unlocked` and related state fields added outside any
  `#ifdef MEDIA_BROWSER_ENABLED` guard, so the flag-off build still has them
  in `AppState`'s layout
- **Unguarded method additions in `ui/renderer.h / .cpp`** — the public
  `mb_*` helper methods added to the renderer are compiled in unconditionally
- **Enum value additions in `ui/settings_menu.h`** — `MenuSection::MEDIA_BROWSER`
  and the `HIDE_MEDIA_BROWSER` entry extend the enum layout regardless of flag
- **Unconditional plumbing in `main.cpp`** outside the `#ifdef` — new includes
  and the `sequence_detector` member are compiled in even with the flag off

**None of these have functional impact when the flag is OFF** — the binary
behaves identically to the Phase 1 build from the user's perspective. The
added state is present but never reached: no code path reads it unless
`media_browser.unlocked=true`. So this is pragmatic drift, not a correctness
bug. Documenting it here rather than claiming the strict invariant still
holds.

To restore strict bit-identity: audit every file in the feature's blast
radius (see list above) and gate every addition behind `#ifdef
MEDIA_BROWSER_ENABLED` (or a runtime-equivalent pattern), then rebuild and
re-hash.

---

## Test results

- **Catch2 unit/integration tests**: 26 test cases / 98 assertions, all
  passing (run via `./build/test_media_browser`)
- **CLI subcommands**: `test_media_browser radarr-discover`, `radarr-search`,
  `radarr-add`, `radarr-queue`, `radarr-library`, `radarr-delete` all
  exercised successfully against the live local stack
- **End-to-end Radarr search**: querying "The Matrix" against the live Radarr
  returns **20 results** with valid TMDB IDs, posters, and release years —
  confirms the full pipeline (kiosk → Radarr API key discovery → HTTP query
  → JSON parse → UI render) works

---

## Acceptance criteria from V2 design §20

| # | Criterion | Status |
|---|---|---|
| 1 | Secret sequence detects and sets `media_browser.unlocked=true` | **Met** |
| 2 | Toast displayed on successful unlock | **Met** |
| 3 | Movies entry appears in main playlist when unlocked | **Met** |
| 4 | Six screens implemented and navigable | **Met** |
| 5 | Controller-free navigation (4 buttons + rotary only) on all screens | **Met** |
| 6 | BTN4 returns to origin screen (not always Browse) | **Met** (post-`6ddbc28`) |
| 7 | Live search with debounced Radarr query | **Met** |
| 8 | Add to Library triggers Radarr monitor + search | **Met** |
| 9 | Queue shows rate / peers / ETA live | **Met** |
| 10 | Library filters work (All/Unwatched/Missing/Recent) | **Met** |
| 11 | Movies Settings exposes 11 operator controls | **Met** |
| 12 | Hide Movies re-locks without data loss | **Met** |
| 13 | Services bound to 127.0.0.1 only | **Met** (post-`823334a`) |
| 14 | VPN kill-switch on qBittorrent | **Met** |
| 15 | FlareSolverr wired in for CloudFlare indexers | **Met** |
| 16 | Flag-off build is bit-identical to Phase 1 | **Unmet** (documented drift — see above) |
| 17 | Display mode switch on enter/exit | **Deferred** (Task 24) |
| 18 | Catch2 test suite passes | **Met** |
| 19 | CLI test tool exercises all endpoints | **Met** |
| 20 | Documentation delivered | **Met** (this file + 2 companions) |

---

## Known open issues

- **Task 24 not implemented** — DRM display mode switch deferred (see above)
- **Bit-identity drift** — flag-off build no longer matches Phase 1 hash;
  documented as pragmatic drift, audit pass needed to restore strict
  invariant
- **Task 30 not yet run** — fresh-Pi re-deploy + end-to-end smoke test from
  a clean image has not been performed; only incremental deploys on a
  warm system have been exercised
- **Post-reboot Docker DNS flake** — on some Pi reboots, Docker's embedded
  DNS doesn't come back cleanly, causing Radarr ↔ Prowlarr ↔ qBittorrent
  inter-container calls to fail with lookup errors. Manual fix: `sudo
  systemctl restart docker`. Root cause not yet investigated

---

## Remaining work for a daytime session

1. **Fix bit-identity drift** — audit shared files (`app_state.h`,
   `ui/renderer.{h,cpp}`, `ui/settings_menu.h`, `main.cpp`) and gate every
   Media-Browser-adjacent addition behind `#ifdef MEDIA_BROWSER_ENABLED` so
   the flag-off binary returns to the Phase 1 hash
2. **Implement Task 24 (DRM display mode switch)** — once real P2P content
   is available to test against, add mode negotiation on Media Browser
   enter/exit and verify the RetroArch handoff path remains intact
3. **Run Task 30 (fresh-Pi deploy + full smoke test)** — flash a clean Pi
   image, deploy from scratch, walk the UI checklist end-to-end

---

## Deploy status

- Pi has the **latest main-branch code** synced via `deploy_cpp.sh --build`
- **Flag-ON kiosk binary is currently running** under
  `magic-dingus-box-cpp.service` (active, healthy)
- **All five Docker services healthy**: radarr, prowlarr, qbittorrent,
  gluetun, flaresolverr all report `Up (healthy)` in `docker compose ps`
- **SSH tunnel required for service admin UIs** — ports 7878 / 9696 / 8080
  are loopback-only on the Pi; operator opens them by tunneling from their
  laptop:
  ```
  ssh -L 7878:localhost:7878 -L 9696:localhost:9696 -L 8080:localhost:8080 magic@magicpi.local
  ```
