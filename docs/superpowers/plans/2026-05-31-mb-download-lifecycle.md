# Media Browser — Download Lifecycle Polish (Handoff)

**Created:** 2026-05-31, end of a long session. Condensed handoff so a FRESH
session can finish the remaining work with clean tooling.

**Context for the fresh session:** The user asked to audit the entire
"pick a movie → download → it finishes → watch it" flow and make every
transition feel professional, with special attention to **ambiguity when a
download finishes**. Three subagent audits (download-lifecycle explorer,
screen-transition explorer, UI code-reviewer) ran and converged. Their full
findings are summarized below — you do NOT need to re-run them.

Repo root: `magic_dingus_box_cpp/` (NOTE the trailing space in the parent dir
`magic_dingus_box /` — Glob fails on it; use `grep -rn` via Bash or the Read
tool with full quoted paths).

Build/deploy: `PI_HOST=magic@magicpi.local ./scripts/deploy_cpp.sh --build`
(the script restarts the kiosk automatically). Unit tests on the Pi:
`cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && ./test_media_browser_unit`.
MEDIA_BROWSER build is ON by default (production target).

---

## ALREADY DONE THIS SESSION (committed separately — verify with `git log`)

**Finding D — artwork cache stuck paused after non-normal Playback exit.**
Three `ui_renderer.artwork_cache().resume()` calls added in `src/main.cpp` on
the exit paths that bypass the normal screen-transition resume:
  1. exit-modal commit (`ExitModal::Result::Exit` branch)
  2. `btn4_long_press_exit` branch
  3. `Screen::Exit` return branch
`resume()` is idempotent (no-op when not paused). This was a real bug: after
exiting Playback via any of those three paths the artwork worker stayed paused
for the rest of the process, so NO posters loaded on later Browse/Library
visits until reboot. If `git log` shows this committed, it's handled. If the
working tree still has uncommitted main.cpp edits, build+test+commit them.

---

## REMAINING WORK (do these in a fresh session, in this order)

### A — DetailScreen does not self-refresh while a download completes  [HIGH]

**Symptom:** User opens a movie's Detail while it's downloading
(`Mode::InLibraryNoFile`, shows "Search Again / monitored" banner). Download
finishes, Radarr imports, `hasFile` flips true — but Detail keeps showing the
old buttons + "MONITORED" banner forever. Re-entering the SAME movie's Detail
short-circuits the refresh (`enter()` lines ~253-260: `!needs_refresh_ &&
have_loaded_data` → returns early). The ONLY way to see the Play button appear
is to visit a different movie and come back.

**Why the obvious fix is wrong:** `fetch()` (detail_screen.cpp ~267) sets
`mode_ = Mode::Loading` and resets `movie_` + `tmdb_detail_`. Calling it for a
background refresh flashes "Loading…" over content the user is reading. Bad.

**Recommended design — a QUIET library-only re-poll:**
- Add `poll_library_state()` that re-fetches ONLY Radarr library (not TMDB,
  which is the slow 6s VPN call) on a timer from `update()`, e.g. every 10s,
  ONLY when `mode_ == InLibraryNoFile` (the one mode that can transition on
  completion). Use the existing async worker pattern (generation counter,
  `tmdb_result_mtx_`, drained in `apply_pending_detail`) BUT add a `quiet`
  flag to the fetch path so it does NOT set `mode_ = Loading` or reset
  `movie_`/`tmdb_detail_` — it only swaps in the new library record + flips
  `mode_` to `InLibraryWithFile` when `hasFile` becomes true.
- Simplest concrete shape: add `bool quiet` param to `run_fetch`/a new
  `run_library_poll(gen)` that reuses `DetailFetchResult` but leaves `detail`
  empty; `apply_pending_detail` already tolerates `!detail_ok` only as an
  error — so give the quiet result a `quiet` marker so apply merges library
  without entering Mode::Error. When `mode_` flips NotInLibrary/InLibraryNoFile
  → InLibraryWithFile, `::ui::Toast::show("Ready to play")` for delight.
- Reset the poll timer in `enter()`.

Files: `src/media_browser/ui/detail_screen.{h,cpp}`. Header has the async
state block (~309-331) and `DetailFetchResult` (~226-237). `update()` is
~485-506. `apply_pending_detail()` is ~358-425.

### B — LibraryScreen does not self-refresh while a download completes  [MOD]

**Symptom:** User on Library watching a poster's "DOWNLOADING" badge. Download
finishes; badge never clears (and `has_file`/poster state stays stale) until
they leave + re-enter. `downloading_tmdb_ids_` / `stuck_tmdb_ids_` are only
rebuilt in `reload()`, which only runs from `enter()`.

**Design:** LibraryScreen currently has NO `update()` override and `reload()`
is a SYNC blocking call (get_library + get_queue on the render thread — also
flagged as a minor stutter-on-entry issue, Finding from audit #2). Two options:
  1. Minimal: add an `update()` that calls `reload()` on a ~5-10s timer. Keeps
     the sync call but bounds its frequency. Accept the brief hitch.
  2. Better: make `reload()` async (worker thread + pending result drained in
     `update()`, mirroring QueueScreen's `refresh_async`/`apply_pending`).
     Eliminates both the staleness AND the entry stutter. More work.
Recommend option 2 if time allows; option 1 is acceptable.

Files: `src/media_browser/ui/library_screen.{h,cpp}`. `enter()`/`reload()` are
~258-300. Note `reload()` already builds both id-sets correctly; you're just
making it run periodically + ideally off-thread.

### C — Queue shows "Completed" during import, then row silently vanishes [MOD]

**Symptom:** qBit hits 100% → row shows green "Completed" (no distinction from
"still importing"), then disappears with no "Imported ✓" terminal state when
Radarr finishes. Ambiguous — the user can't tell if it worked.

**Root:** In `queue_screen.cpp` run_refresh (~300-316) the qBit-overlay
translation maps all qBit seeding states (`uploading`/`pausedUP`/etc.) to the
single string `"completed"`, which discards Radarr's `tracked_download_state`
(`importPending`/`importing`/`importBlocked`) that was parsed into
`QueueItem.tracked_download_state`. `progress_color_for_state` (~167-177) only
knows downloading/completed/failed/stalled/queued/paused.

**Design:**
- When qBit says a torrent is seeding/complete BUT Radarr's
  `tracked_download_state` is `importing`/`importPending`, set the row state to
  a new `"importing"` vocabulary word; render it amber with sub-line
  "Importing…". (Don't overwrite with "completed" when Radarr signals import
  in progress.)
- Optional nicety: when a row that was present last refresh is GONE this
  refresh AND the movie now has a file (cross-ref library), flash a transient
  toast "<title> ready to play" instead of silent vanish. Lower priority.
- Add `"importing"` to `progress_color_for_state` (amber/accent) and
  `titlecase_state` already title-cases it.

Files: `src/media_browser/ui/queue_screen.cpp` overlay block ~300-316, color
fn ~167-177, sub-line render ~802-826.

### C1 — Cancel-download gives no success/fail feedback  [IMPORTANT]

`do_cancel_focused()` (queue_screen.cpp ~396-406) discards the return of
`radarr_.cancel_queue_item()`. Capture it; on success
`::ui::Toast::show("Download cancelled")`, on failure
`::ui::Toast::show("Cancel failed — see Radarr logs")`. Matches the toast
vocabulary `do_add_to_library` uses.

### C2 — All-unplayable picker list traps the user  [IMPORTANT, from my recent change]

In `release_picker_screen.cpp`, the new playability filter blocks SELECT on
`Unplayable` rows with a toast + `continue`. If EVERY release is Unplayable
(e.g. a title that only has HDR/4K/HEVC-10bit), the user is stuck pressing
SELECT and only getting toasts, with no on-screen explanation. The
`rows_.empty()` empty-state (~556) doesn't fire because rows exist.

**Design:** In `set_candidates()` after sort, count playable rows; store a
`bool all_unplayable_`. In render, when `all_unplayable_`, show a dedicated
message in the body: "All available releases are incompatible with this TV
(HDR/4K/HEVC). Try Search Again from the Detail screen." Keep the rows visible
below it for transparency, or replace — your call. At minimum the user must
see WHY nothing is grabbable.

Files: `src/media_browser/ui/release_picker_screen.{h,cpp}` —
`set_candidates()` ~259-270, render empty-state ~536-555.

### C8 — Grab-release navigates to Detail but toast says "see Queue"  [IMPORTANT]

`release_picker_screen.cpp` ~349-367: on successful `grab_release()` it shows
`"Grabbing release — see Queue"` then `return Screen::Detail`. Detail will
still show `InLibraryNoFile` (looks like nothing happened). Change to
`return Screen::Queue` to match the toast and mirror `do_add_to_library()`
(which returns Screen::Queue). Verify the dispatcher handles
ReleasePicker→Queue (it should; Queue is a top-level screen).

---

## LOWER-PRIORITY ITEMS (from audits, optional — mention to user before doing)

- **Playback `std::system()` blocks render thread** (playback_screen.cpp
  ~101-102, ~198-199): the services-pause/unpause shell calls are synchronous
  on the render thread. Move to a detached thread for true fire-and-forget.
  (CLAUDE.md already flags std::system for execve replacement generally.)
- **Playback quick-add calls `get_quality_profiles()` sync on render thread**
  (~254-266, ~5s freeze while video plays). Cache profiles from the Detail
  handoff instead.
- **Radarr stopped during playback can't import** (playback_services_pause.sh
  docker-stops Radarr): a download that completes during a movie won't import
  until playback ends + Radarr cold-starts (~10-15s). User may hit Detail and
  see InLibraryNoFile briefly. Consider a post-playback toast "Services
  restarting — downloads resume shortly," or leave Radarr running (only stop
  Prowlarr/Byparr) — tradeoff: Radarr idle RAM vs import latency.
- **DownloadWatchdog `unwatch()` never called on completion** (watchdog is
  currently DISABLED in main.cpp per user request — the stall modal was pesky.
  If ever re-enabled, completed-download watches linger and fire false stalls;
  needs an unwatch when library shows hasFile=true. Leave disabled for now.)
- **BrowseScreen/LibraryScreen sync Radarr calls in enter()** cause a
  150-1500ms stutter on every entry. Make async (SearchScreen pattern).
- **SearchScreen stale-result-into-fresh-session** (search_screen.cpp enter()
  doesn't bump `lookup_current_gen_`): navigate away mid-lookup, return → old
  results flash. Bump the gen + clear `lookup_result_ready_` in enter().
- **LibraryScreen overlay + MbSettingsScreen `editing_` not reset on
  re-entry**: re-entering can land in an open sort-overlay / edit-mode. Reset
  `overlay_state_` / `editing_` in their `enter()`.
- **Dead constants in library_screen.cpp top-of-file** (kPaddingX=32 etc. from
  pre-v1.6.x layout) — cleanup only.
- **`truncate_to_width` + `poster_tint_for_tmdb` duplicated 4×** — extract to
  mb_chrome or a util header.
- **stall_prompt_modal title/reason not truncated** — long titles overflow the
  card. (Modal is disabled but still rendered if re-enabled.)
- **Library "Unwatched" filter is a persisted no-op** — block its SELECT or
  toast "coming soon" so it doesn't persist a confusing no-op filter state.

---

## A NOTE ON THIS SESSION'S TOOLING

Earlier in this session I twice misread ordinary tooling flakiness (cancelled
parallel Bash calls, `cat -A` not existing on macOS, SSH timeouts, an awk
display quirk) as "prompt injection / file tampering." It was NOT real —
subagent shasum checks confirmed all files clean and untouched. The fresh
session should TRUST normal Read/Bash output and verify changes the normal way
(build + tests). Do not run forensic integrity subagents; they were wasted
effort.

---

## DONE CRITERIA for the fresh session

1. A, B, C implemented (the three that directly answer "ambiguity when a
   download finishes").
2. C1, C2, C8 fixed (small, high-value correctness/UX).
3. Build clean, `test_media_browser_unit` green on the Pi, kiosk restarted.
4. One or a few focused commits pushed to origin/main.
5. Lower-priority list: surface to the user, implement only what they pick.
