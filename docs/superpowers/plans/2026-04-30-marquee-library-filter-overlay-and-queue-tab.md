# Marquee Library filter overlay + Queue tab + input grammar redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the design from `docs/superpowers/specs/2026-04-30-marquee-library-filter-overlay-and-queue-tab-design.md` — a 6-tab Marquee strip with a peer-level Queue destination, a slide-in Library overlay (sort + filter + stats) on BTN4, and a full input-grammar remap (BTN2 = back globally in MB; Playback preserved).

**Architecture:** Two new persisted display settings (`mb_library_sort` / `mb_library_filter`), tab-strip reordering across all 6 menu screens with cascading nav-handler updates, a new overlay state machine inside `LibraryScreen` with its own input-capture / animation / render, and removal of legacy BTN2 quick-add / BTN4 back call sites in 5 screen files. The work is structured so the kiosk stays buildable + smoke-testable after every task — early tasks land the data layer and tab reorder (low-risk, no behavior change for the operator until they cycle to Queue), the input-grammar remap is one larger task that touches all menu screens together (so BTN2 = back lands consistently), and the slide-in overlay is the final big feature drop.

**Tech Stack:** C++17, immediate-mode OpenGL ES 3.0, evdev input, `chrono::steady_clock` for animation timing, `JsonCpp` for `config/settings.json` persistence. Build: `cmake .. && make -j2` on Pi via `magic_dingus_box_cpp/scripts/deploy_cpp.sh --build` (output buffered to a logfile; monitor for `✓ Build complete`). Deploy / smoke loop: `ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service'` after each build (`deploy_cpp.sh` restarts BEFORE building, so a manual restart is required to load the new binary).

---

## File map

| File | Responsibility |
|---|---|
| `magic_dingus_box_cpp/src/app/app_state.h` | Add `MbLibrarySort` + `MbLibraryFilter` enums, plus the two field declarations in `DisplaySettings`. Doc-comment the dual-store relationship + persistence path. |
| `magic_dingus_box_cpp/src/app/settings_persistence.cpp` | Save (enum→string) + load (string→enum, default-on-miss) for the two new fields. Falls under the existing `display.*` JSON sub-object. |
| `magic_dingus_box_cpp/src/media_browser/ui/browse_screen.h` | No new enum values needed (`Category::Queue` already exists). May add a comment update. |
| `magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp` | Tab strip → 6 tabs with Queue between Library and Settings; `Queue` returns `Screen::Queue` from the strip-walk handler. Retire BTN2 quick-add. BTN4 (was back to MainMenu) → no-op on short press; long-press preserved by dispatcher. |
| `magic_dingus_box_cpp/src/media_browser/ui/library_screen.h` | Add `LibraryOverlayState` enum (`Closed / SlidingIn / Open / SlidingOut`), `overlay_state_`, `overlay_focus_row_`, `overlay_anim_started_at_` members. Add `apply_sort_filter_to_view_` private helper. |
| `magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp` | Tab strip → 6 tabs (Library active at index 3). Wire BTN4 to open overlay; BTN2 returns `Screen::Browse`. Render overlay panel (slides in from right, stats + sort + filter contents). Wire input capture during open state. Update `rebuild_view()` to apply persisted sort + filter from `state.display_settings`. Retire visual sort sub-tab strip on screen body (lives in overlay now). |
| `magic_dingus_box_cpp/src/media_browser/ui/search_screen.cpp` | Tab strip → 6 tabs with Search at index 2. PREV → Browse (with last-active content category), NEXT → Library. Retire BTN2 quick-add. BTN4 → no-op. |
| `magic_dingus_box_cpp/src/media_browser/ui/queue_screen.cpp` | Tab strip → 6 tabs with Queue active at index 4. PREV → Library, NEXT → Settings. Retire BTN2 cancel handler. BTN4 → no-op. |
| `magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp` | Tab strip → 6 tabs with Settings active at index 5. PREV → Queue (was Search). Retire BTN2 refresh-services shortcut. BTN4 → no-op. |
| `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp` | Retire BTN2 primary-button shortcut. Swap BTN4 (was back to origin) to BTN2 (back to origin). BTN4 → no-op. |
| `magic_dingus_box_cpp/src/media_browser/ui/mb_chrome.{h,cpp}` | (Optional, only if the gold left-edge rule on the panel becomes reusable.) Add `draw_panel_left_edge()` helper. |
| `magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp` | **No changes.** Playback is special — BTN2 stays play-pause, BTN4 stays exit-to-Detail. Preserved verbatim. |

**Smoke-test cadence**: After Tasks 1, 2, 3, 5, 7, 8 each, the change is buildable + smoke-testable on the Pi. The plan ends with a final pass running the spec's 11-step manual smoke test before commit-and-tag.

---

### Task 1: Persisted sort + filter fields

**Goal:** Add the data layer for the new selections. Kiosk compiles + boots unchanged (no UI touches `DisplaySettings::mb_library_sort` / `mb_library_filter` yet — they default to `Recent` / `All`).

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (within the existing `DisplaySettings` struct, alongside the v1.6.2 `mb_*_intensity` block)
- Modify: `magic_dingus_box_cpp/src/app/settings_persistence.cpp` (save block ~line 49 area; load block ~line 152 area)

- [ ] **Step 1: Add the two enums + fields to `DisplaySettings`**

In `app_state.h`, after the existing `mb_flicker_intensity` field and before the helper methods, add:

```cpp
        // Media Browser Library overlay (v1.6.x). Two enums + two
        // fields capture the operator's chosen sort + filter for the
        // Library grid. The slide-in overlay (BTN4 on LibraryScreen)
        // writes these; LibraryScreen::rebuild_view() reads them on
        // every entry to apply the chosen ordering / subset.
        // Persisted to config/settings.json as
        //   display.mb_library_sort   (string: "recent"/"title"/"year"/"size")
        //   display.mb_library_filter (string: "all"/"unwatched"/"missing_files"/"recently_added")
        // Defaults match what an operator sees on a fresh kiosk.
        enum class MbLibrarySort {
            Recent = 0,
            Title  = 1,
            Year   = 2,
            Size   = 3,
        };
        enum class MbLibraryFilter {
            All            = 0,
            Unwatched      = 1,  // Placeholder until watched-history lands.
            MissingFiles   = 2,
            RecentlyAdded  = 3,
        };
        MbLibrarySort   mb_library_sort   = MbLibrarySort::Recent;
        MbLibraryFilter mb_library_filter = MbLibraryFilter::All;
```

- [ ] **Step 2: Add string ↔ enum helpers in settings_persistence.cpp**

At the top of the anonymous namespace in `settings_persistence.cpp` (or just after existing helpers), add:

```cpp
namespace {
const char* mb_library_sort_to_string(app::AppState::DisplaySettings::MbLibrarySort s) {
    using S = app::AppState::DisplaySettings::MbLibrarySort;
    switch (s) {
        case S::Recent: return "recent";
        case S::Title:  return "title";
        case S::Year:   return "year";
        case S::Size:   return "size";
    }
    return "recent";
}
app::AppState::DisplaySettings::MbLibrarySort mb_library_sort_from_string(const std::string& s) {
    using R = app::AppState::DisplaySettings::MbLibrarySort;
    if (s == "title") return R::Title;
    if (s == "year")  return R::Year;
    if (s == "size")  return R::Size;
    return R::Recent;  // Default for "recent" or any unknown value.
}
const char* mb_library_filter_to_string(app::AppState::DisplaySettings::MbLibraryFilter f) {
    using F = app::AppState::DisplaySettings::MbLibraryFilter;
    switch (f) {
        case F::All:            return "all";
        case F::Unwatched:      return "unwatched";
        case F::MissingFiles:   return "missing_files";
        case F::RecentlyAdded:  return "recently_added";
    }
    return "all";
}
app::AppState::DisplaySettings::MbLibraryFilter mb_library_filter_from_string(const std::string& s) {
    using R = app::AppState::DisplaySettings::MbLibraryFilter;
    if (s == "unwatched")      return R::Unwatched;
    if (s == "missing_files")  return R::MissingFiles;
    if (s == "recently_added") return R::RecentlyAdded;
    return R::All;
}
}  // namespace
```

- [ ] **Step 3: Save the two fields** (in the save_settings function, in the same block where the `mb_*_intensity` keys are written)

Find this block in `settings_persistence.cpp`:

```cpp
    display["mb_flicker_intensity"]     = state.display_settings.mb_flicker_intensity;
    root["display"] = display;
```

Insert just before the `root["display"] = display;` line:

```cpp
    // Media Browser Library overlay sort + filter (v1.6.x). String
    // enums for human-readable JSON.
    display["mb_library_sort"]   = mb_library_sort_to_string(
        state.display_settings.mb_library_sort);
    display["mb_library_filter"] = mb_library_filter_to_string(
        state.display_settings.mb_library_filter);
```

- [ ] **Step 4: Load the two fields** (in the load_settings function, in the same block where the `mb_*_intensity` keys are read)

Find this block:

```cpp
        state.display_settings.mb_flicker_intensity =
            display.get("mb_flicker_intensity",
                        state.display_settings.flicker_intensity).asFloat();
    }
```

Insert just before the closing `}`:

```cpp
        // Library overlay sort + filter — defaults are the same as the
        // struct defaults (Recent / All) so an operator upgrading from
        // pre-v1.6.x sees no visual change to their library on first
        // launch.
        state.display_settings.mb_library_sort = mb_library_sort_from_string(
            display.get("mb_library_sort", "recent").asString());
        state.display_settings.mb_library_filter = mb_library_filter_from_string(
            display.get("mb_library_filter", "all").asString());
```

- [ ] **Step 5: Build + smoke-test (no UI yet — verify compile + boot)**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/magic_dingus_box_suite/magic_dingus_box "
PI_HOST=magic@10.55.0.1 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build > /tmp/task1.log 2>&1
grep -E "Build complete|error:" /tmp/task1.log | tail -3
```

Expected: `✓ Build complete`. Then:

```bash
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service && sleep 3 && systemctl is-active magic-dingus-box-cpp.service'
```

Expected: `active`. Then verify the new keys land in settings.json after a save (e.g. cycle a CRT setting to force a save):

```bash
ssh magic@10.55.0.1 'cat /opt/magic_dingus_box/config/settings.json | jq .display.mb_library_sort, .display.mb_library_filter'
```

Expected: `"recent"` and `"all"`.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/app/app_state.h magic_dingus_box_cpp/src/app/settings_persistence.cpp
git commit -m "feat(mb): add mb_library_sort + mb_library_filter persisted settings"
```

---

### Task 2: 6-tab strip across all menu screens

**Goal:** Add the Queue tab between Library and Settings on every Marquee menu screen's tab strip. Wire BTN1/BTN3 routing accordingly. Operator can now reach the Queue via BTN3 right-walk from Library, and tab nav stays consistent across all 6 screens.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp` (two `kVisibleTabs` arrays — one in `handle_input`, one in `render`)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp` (`kTabLabels` + `kLibraryStripPos`)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/search_screen.cpp` (chrome header tabs vector + handle_input PREV/NEXT)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/queue_screen.cpp` (chrome header tabs vector + handle_input PREV/NEXT)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp` (chrome header tabs vector + handle_input PREV/NEXT)

The 6-tab order is: `Popular · Top Rated · Search · Library · Queue · Settings`

- [ ] **Step 1: Update BrowseScreen's `kVisibleTabs` arrays + nav routing**

In `browse_screen.cpp`, find both `kVisibleTabs[]` definitions (one in `handle_input`, one in `render` — same array, two locations). Replace:

```cpp
    static constexpr Category kVisibleTabs[] = {
        Category::Popular,
        Category::TopRated,
        Category::Library,
        Category::Search,
        Category::Settings,
    };
```

With:

```cpp
    static constexpr Category kVisibleTabs[] = {
        Category::Popular,
        Category::TopRated,
        Category::Search,
        Category::Library,
        Category::Queue,
        Category::Settings,
    };
```

In the same file, in `handle_input`, find the BTN3/NEXT branch and the BTN1/PREV branch — both already have lines like:

```cpp
            if (new_cat == Category::Library)  return Screen::Library;
            if (new_cat == Category::Search)   return Screen::Search;
            if (new_cat == Category::Settings) return Screen::MovieSettings;
```

Add a `Queue` route right alongside (in BOTH branches):

```cpp
            if (new_cat == Category::Queue)    return Screen::Queue;
```

- [ ] **Step 2: Update LibraryScreen's tab labels + active position**

In `library_screen.cpp`'s render function, find:

```cpp
    static constexpr const char* kTabLabels[] = {
        "Popular", "Top Rated", "Library", "Search", "Settings",
    };
    constexpr int kNumVisibleTabs = 5;
    constexpr int kLibraryStripPos = 2;
```

Replace with:

```cpp
    static constexpr const char* kTabLabels[] = {
        "Popular", "Top Rated", "Search", "Library", "Queue", "Settings",
    };
    constexpr int kNumVisibleTabs = 6;
    constexpr int kLibraryStripPos = 3;
```

In the same file, in `handle_input`, find the PREV/NEXT branches:

```cpp
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Browse;
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            return Screen::Search;
        }
```

Update to (Library is now at index 3, so PREV → Search at index 2, NEXT → Queue at index 4):

```cpp
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Search;
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            return Screen::Queue;
        }
```

- [ ] **Step 3: Update SearchScreen's tabs + nav**

In `search_screen.cpp`'s render function, find the tabs vector built for `chrome::draw_screen_header`. Replace it with:

```cpp
        const std::vector<chrome::TabSpec> tabs = {
            {"Popular",   chrome::TabState::Inactive},
            {"Top Rated", chrome::TabState::Inactive},
            {"Search",    chrome::TabState::Active},
            {"Library",   chrome::TabState::Inactive},
            {"Queue",     chrome::TabState::Inactive},
            {"Settings",  chrome::TabState::Inactive},
        };
```

In `handle_input`, find PREV/NEXT branches and update:

```cpp
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Browse;  // Browse remembers last-active content category.
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            return Screen::Library;
        }
```

- [ ] **Step 4: Update QueueScreen's tabs + nav**

In `queue_screen.cpp`'s render function, find the tabs vector and replace:

```cpp
        const std::vector<chrome::TabSpec> tabs = {
            {"Popular",   chrome::TabState::Inactive},
            {"Top Rated", chrome::TabState::Inactive},
            {"Search",    chrome::TabState::Inactive},
            {"Library",   chrome::TabState::Inactive},
            {"Queue",     chrome::TabState::Active},
            {"Settings",  chrome::TabState::Inactive},
        };
```

In `handle_input`, find / add PREV/NEXT branches:

```cpp
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Library;
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            return Screen::MovieSettings;
        }
```

- [ ] **Step 5: Update MbSettingsScreen's tabs + nav**

In `mb_settings_screen.cpp`'s render function, find the tabs vector and replace:

```cpp
        const std::vector<chrome::TabSpec> tabs = {
            {"Popular",   chrome::TabState::Inactive},
            {"Top Rated", chrome::TabState::Inactive},
            {"Search",    chrome::TabState::Inactive},
            {"Library",   chrome::TabState::Inactive},
            {"Queue",     chrome::TabState::Inactive},
            {"Settings",  chrome::TabState::Active},
        };
```

In `handle_input`, find the PREV branch and update from `Screen::Search` to `Screen::Queue`:

```cpp
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Queue;
        }
```

(NEXT remains a dead-end no-op since Settings is rightmost.)

- [ ] **Step 6: Build + smoke-test**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/magic_dingus_box_suite/magic_dingus_box "
PI_HOST=magic@10.55.0.1 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build > /tmp/task2.log 2>&1
grep -E "Build complete|error:" /tmp/task2.log | tail -3
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service && sleep 3 && systemctl is-active magic-dingus-box-cpp.service'
```

Expected: `✓ Build complete` then `active`.

**Manual smoke test on Pi:** Walk left-to-right through the strip with BTN3 starting from Browse. Verify the order is `Popular → Top Rated → Search → Library → Queue → Settings` and the active-tab gold border tracks correctly. Walk back with BTN1. Confirm no crash, no garbled labels.

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/search_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/queue_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp
git commit -m "feat(mb): 6-tab Marquee strip with Queue between Library and Settings"
```

---

### Task 3: Input grammar remap — BTN2 = back, BTN4 retired (cascading conflict cleanup)

**Goal:** Land the input-grammar remap. After this task, BTN2 is back globally on every Marquee menu screen (Playback preserved as-is); BTN4 short-press is a no-op on every menu screen except Library (overlay wired in Task 4); BTN4 long-press still exits MB → MainMenu (handled by the dispatcher, not these screens).

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp` (BTN2 quick-add → retire; BTN4 → no-op)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/search_screen.cpp` (BTN2 quick-add → retire; BTN4 stays unchanged — was already no-op-ish via SETTINGS_MENU; verify)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp` (BTN2 → back to Browse; BTN4 → no-op for now, overlay wired in Task 4)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/queue_screen.cpp` (BTN2 cancel → retire; BTN4 → no-op or Detail-back depending on existing nav)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp` (BTN2 refresh → retire; BTN4 → no-op)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp` (BTN2 primary-button shortcut → retire; back moves from BTN4 to BTN2)

**Important: Playback grammar is preserved verbatim** — `playback_screen.cpp` keeps its existing BTN2 = play-pause, BTN4 = exit-to-Detail. Don't touch it.

- [ ] **Step 1: BrowseScreen — retire BTN2 quick-add, gate BTN4 short-press**

In `browse_screen.cpp::handle_input`, find:

```cpp
        // BTN2 (PLAY_PAUSE, red) — quick-add focused poster to library.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            quick_add_focused();
            continue;
        }
```

Delete this whole block (BTN2 is now back; on BrowseScreen "back" → exit MB to MainMenu, which is what `SETTINGS_MENU` already does — so we route BTN2 to the same exit). Right above the existing `SETTINGS_MENU` handler, add:

```cpp
        // BTN2 (PLAY_PAUSE, red) — back. Browse is the top of the
        // Marquee section, so back-from-Browse exits the Media Browser
        // entirely. Same destination as the existing BTN4 long-press
        // handled by the input dispatcher.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            return Screen::Exit;
        }
```

The existing `SETTINGS_MENU` (BTN4 short-press) handler currently `return Screen::Exit;` — change it to a no-op-pass:

```cpp
        // BTN4 (SETTINGS_MENU, black) — short-press is a no-op in v1.6.x.
        // The slide-in overlay is Library-specific; on Browse there's
        // nothing for BTN4 to do. Long-press still exits MB → MainMenu
        // via the input dispatcher's long-press branch.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            continue;
        }
```

- [ ] **Step 2: SearchScreen — retire BTN2 quick-add, gate BTN4**

In `search_screen.cpp::handle_input`, find the BTN2 (PLAY_PAUSE) handler and replace its body:

```cpp
        // BTN2 (PLAY_PAUSE, red) — back. Returns to whatever Marquee
        // tab the operator was on before navigating to Search (handled
        // via Screen::Browse which retains its category_).
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            return Screen::Browse;
        }
```

Find the BTN4 (SETTINGS_MENU) handler. Replace its `return Screen::Browse;` with `continue;`:

```cpp
        // BTN4 (SETTINGS_MENU, black) — short-press is a no-op in v1.6.x.
        // No slide-in overlay on Search; long-press to exit MB still
        // works via the input dispatcher's long-press branch.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            continue;
        }
```

- [ ] **Step 3: LibraryScreen — wire BTN2 → back, leave BTN4 stub for Task 4**

In `library_screen.cpp::handle_input`, find the existing BTN4 (SETTINGS_MENU) handler that exits / goes back. Replace its body to be a no-op short-press (the overlay-open behavior gets wired in Task 4):

```cpp
        // BTN4 (SETTINGS_MENU, black) — opens the slide-in overlay.
        // Wired in Task 4. For now: no-op short-press; long-press
        // still exits MB via the input dispatcher.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            continue;
        }
```

Add a new BTN2 handler near the top of the loop (after the BTN1/BTN3 PREV/NEXT handlers from Task 2):

```cpp
        // BTN2 (PLAY_PAUSE, red) — back. Library's parent in the back
        // stack is Browse; Browse retains its last-active content
        // category so the operator returns to wherever they were
        // (Popular / Top Rated) before opening Library.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            return Screen::Browse;
        }
```

- [ ] **Step 4: QueueScreen — retire BTN2 cancel, gate BTN4**

In `queue_screen.cpp::handle_input`, find the BTN2 (PLAY_PAUSE) cancel handler and replace its body:

```cpp
        // BTN2 (PLAY_PAUSE, red) — back. Returns to Library (Queue's
        // tab-strip neighbour and the most-likely "where I came from"
        // for an operator inspecting an active download).
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            return Screen::Library;
        }
```

Find the BTN4 (SETTINGS_MENU) handler and replace with no-op:

```cpp
        // BTN4 (SETTINGS_MENU, black) — short-press is a no-op in v1.6.x.
        // No slide-in overlay on Queue.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            continue;
        }
```

- [ ] **Step 5: MbSettingsScreen — retire BTN2 refresh shortcut, gate BTN4**

In `mb_settings_screen.cpp::handle_input`, find the BTN2 (PLAY_PAUSE) handler that calls `refresh_service_health()`. Replace its body:

```cpp
        // BTN2 (PLAY_PAUSE, red) — back. Settings is at the right end
        // of the Marquee strip; back returns to Queue (the immediate
        // PREV-tab neighbour). The Services row's SELECT (A) handler
        // already does what the old BTN2 shortcut did (re-pings the
        // 3 services).
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            return Screen::Queue;
        }
```

Find the BTN4 (SETTINGS_MENU) handler and replace with no-op:

```cpp
        // BTN4 (SETTINGS_MENU, black) — short-press is a no-op in v1.6.x.
        // No slide-in overlay on Settings.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            continue;
        }
```

- [ ] **Step 6: DetailScreen — back moves from BTN4 to BTN2, retire BTN2 primary shortcut**

In `detail_screen.cpp::handle_input`, find the BTN4 (SETTINGS_MENU) handler that does `return origin_;`. Replace with a no-op:

```cpp
        // BTN4 (SETTINGS_MENU, black) — short-press is a no-op in v1.6.x.
        // Back moved to BTN2.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            continue;
        }
```

Find the BTN2 (PLAY_PAUSE) handler that activates the focused button (`focus_ = 0; on_activate();`). Replace its body with the back routing:

```cpp
        // BTN2 (PLAY_PAUSE, red) — back. Returns to whichever screen
        // opened this Detail (Browse / Library / Search / Queue) via
        // origin_, which the dispatcher sets on every transition into
        // Detail.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            return origin_;
        }
```

- [ ] **Step 7: Build + smoke-test**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/magic_dingus_box_suite/magic_dingus_box "
PI_HOST=magic@10.55.0.1 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build > /tmp/task3.log 2>&1
grep -E "Build complete|error:" /tmp/task3.log | tail -3
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service && sleep 3 && systemctl is-active magic-dingus-box-cpp.service'
```

**Manual smoke test on Pi:**
1. From Browse: BTN2 → exits MB to MainMenu. (Reach Browse again via Settings → Movies.)
2. From Library: BTN2 → back to Browse.
3. From Search: BTN2 → back to Browse.
4. From Queue: BTN2 → back to Library.
5. From Settings: BTN2 → back to Queue.
6. From Detail (entered from Library): BTN2 → back to Library.
7. From Detail (entered from Browse): BTN2 → back to Browse.
8. From any menu screen: BTN4 short-press → nothing happens (no-op).
9. From any menu screen: BTN4 long-press (>500 ms hold) → exits MB to kiosk MainMenu.
10. From Playback: BTN2 still toggles play/pause; BTN4 still exits to Detail.

- [ ] **Step 8: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/search_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/queue_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp
git commit -m "feat(mb): BTN2=back, BTN4=no-op (overlay wired separately for Library)"
```

---

### Task 4: Library overlay — state machine + members

**Goal:** Add the overlay state members + helper methods to LibraryScreen, but don't render or wire input yet. After this task, LibraryScreen still behaves identically; the overlay infrastructure is in place to be used by Tasks 5 + 6.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/library_screen.h` (add enum + members + private method declarations)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp` (constructor init for new members; no render or input handling yet)

- [ ] **Step 1: Add the LibraryOverlayState enum + members in library_screen.h**

Find the `private:` section of `LibraryScreen`. Inside it, add (alongside the existing `grid_cursor_` / `scroll_row_` members):

```cpp
    // Slide-in overlay state machine (v1.6.x). The overlay is a 480 px
    // panel that slides in from the right edge on BTN4 press, holding
    // stats + sort + filter controls. Closed = no overlay rendered;
    // SlidingIn = animating from x=1280 → x=760 (200 ms ease-out);
    // Open = stationary; SlidingOut = animating from x=760 → x=1280
    // (150 ms ease-in). Input is captured by the panel during
    // SlidingIn / Open / SlidingOut.
    enum class OverlayState {
        Closed     = 0,
        SlidingIn  = 1,
        Open       = 2,
        SlidingOut = 3,
    };
    OverlayState overlay_state_ = OverlayState::Closed;

    // Animation start time for the current slide. Used to compute the
    // panel's current x-position via ease curves in render().
    std::chrono::steady_clock::time_point overlay_anim_started_at_{};

    // Cursor position inside the panel. The 8 focusable rows are
    // indexed 0-7: Sort (Recent, Title, Year, Size) at 0-3, Filter
    // (All, Unwatched, MissingFiles, RecentlyAdded) at 4-7.
    int overlay_focus_row_ = 0;

    // Number of focusable rows in the panel — kept as a constant so
    // render and input-handling stay in lockstep.
    static constexpr int kOverlayFocusableRows = 8;

    // Open / close transitions. start_open_overlay() snaps to
    // SlidingIn, sets the cursor to whichever row matches the
    // currently-active sort, and timestamps the animation start.
    // start_close_overlay() snaps to SlidingOut and timestamps;
    // tick_overlay_animation() promotes SlidingIn → Open and
    // SlidingOut → Closed when the animation duration has elapsed.
    void start_open_overlay();
    void start_close_overlay();
    void tick_overlay_animation();
```

Make sure `<chrono>` is included at the top of `library_screen.h`. If not, add `#include <chrono>` to the existing include block.

- [ ] **Step 2: Implement the three transition helpers + tick in library_screen.cpp**

Add these three methods anywhere in the LibraryScreen impl block (next to other private methods is fine):

```cpp
void LibraryScreen::start_open_overlay() {
    overlay_state_ = OverlayState::SlidingIn;
    overlay_anim_started_at_ = std::chrono::steady_clock::now();
    // Cursor lands on the currently-active sort row so a single A
    // re-confirms the existing choice (zero-effort cancel).
    using S = ::app::AppState::DisplaySettings::MbLibrarySort;
    switch (state_.display_settings.mb_library_sort) {
        case S::Recent: overlay_focus_row_ = 0; break;
        case S::Title:  overlay_focus_row_ = 1; break;
        case S::Year:   overlay_focus_row_ = 2; break;
        case S::Size:   overlay_focus_row_ = 3; break;
    }
}

void LibraryScreen::start_close_overlay() {
    overlay_state_ = OverlayState::SlidingOut;
    overlay_anim_started_at_ = std::chrono::steady_clock::now();
}

void LibraryScreen::tick_overlay_animation() {
    if (overlay_state_ == OverlayState::Closed ||
        overlay_state_ == OverlayState::Open) return;
    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - overlay_anim_started_at_)
        .count();
    constexpr int kSlideInMs  = 200;
    constexpr int kSlideOutMs = 150;
    if (overlay_state_ == OverlayState::SlidingIn && elapsed_ms >= kSlideInMs) {
        overlay_state_ = OverlayState::Open;
    } else if (overlay_state_ == OverlayState::SlidingOut && elapsed_ms >= kSlideOutMs) {
        overlay_state_ = OverlayState::Closed;
    }
}
```

The class needs access to `::app::AppState&`. If `state_` isn't already a member, find how the screen accesses `state.display_settings.mb_*_intensity` for the existing CRT row rendering — it'll either be a constructor param (matching MbSettingsScreen's pattern) or accessed via the renderer. **If LibraryScreen doesn't already have `state_` access**, this step requires a constructor signature update — match the pattern from `mb_settings_screen.cpp` exactly:

```cpp
LibraryScreen::LibraryScreen(RadarrClient& radarr, ::app::AppState& state)
    : radarr_(radarr), state_(state) {}
```

And in `library_screen.h`, add `::app::AppState& state_;` member + update the include block to include `app/app_state.h`. Also update the call site in `main.cpp` where `mb_library` is constructed — match how `mb_mb_settings` was given access to `state` in the v1.6.2 work.

- [ ] **Step 3: Call tick_overlay_animation() once per render**

In `library_screen.cpp::render`, at the very top (right after the function signature opens), add:

```cpp
    tick_overlay_animation();
```

This guarantees the state machine progresses on every frame regardless of input — pure time-driven advancement.

- [ ] **Step 4: Build + smoke-test (no visible behavior change yet)**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/magic_dingus_box_suite/magic_dingus_box "
PI_HOST=magic@10.55.0.1 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build > /tmp/task4.log 2>&1
grep -E "Build complete|error:" /tmp/task4.log | tail -3
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service && sleep 3 && systemctl is-active magic-dingus-box-cpp.service'
```

**Manual smoke test:** Walk into Library, navigate the grid normally — confirm nothing is broken. The overlay state machine is now compiled and ticking but not rendering yet, so the operator sees no change.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/library_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp \
        magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(mb): LibraryScreen overlay state machine scaffolding"
```

---

### Task 5: Library overlay — render the panel

**Goal:** Render the slide-in panel (background, header, sort + filter rows, animated x-position based on overlay_state_). Wire BTN4 to actually open / close the overlay. Selections inside the overlay still need wiring (Task 6) — for this task A on a row is a no-op; the overlay just renders correctly and animates open/close.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp` (render block additions + BTN4 short-press handler)

- [ ] **Step 1: Add panel-layout constants near the top of the anonymous namespace**

In the `namespace { ... }` block at the top of `library_screen.cpp`, alongside other layout constants, add:

```cpp
// Slide-in overlay (v1.6.x).
//   Panel sits flush with the wood-frame's right inner edge (x=1240).
//   Width 480 px → left edge at x=760 when fully open.
//   Vertical extent: y=120 (chrome header bottom) → y=634 (chrome
//   footer hint band top), 514 px tall.
constexpr int kOverlayPanelW         = 480;
constexpr int kOverlayPanelOpenX     = 760;     // Left edge when fully open
constexpr int kOverlayPanelClosedX   = 1280;    // Left edge when fully closed (off-screen right)
constexpr int kOverlayPanelTopY      = 120;
constexpr int kOverlayPanelBottomY   = 634;
constexpr int kOverlayPanelH         = kOverlayPanelBottomY - kOverlayPanelTopY;
constexpr int kOverlayPanelInnerPadX = 24;
constexpr int kOverlayPanelInnerPadY = 16;
constexpr int kOverlaySlideInMs      = 200;
constexpr int kOverlaySlideOutMs     = 150;

// Block layout inside the panel — three logical sections, top to bottom.
constexpr int kOverlayStatsBlockH    = 96;   // Title + 3 stat lines
constexpr int kOverlaySectionGap     = 18;   // Vertical gap between section blocks
constexpr int kOverlayRowHeight      = 32;   // Per row in Sort + Filter sections
constexpr int kOverlaySectionHeaderH = 28;   // Gold ZenDots heading + gap

// Ease curve for the slide-in animation. Cubic ease-out: 1 - (1-t)^3.
inline float ease_out_cubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
// Ease-in for slide-out: t^3.
inline float ease_in_cubic(float t) {
    return t * t * t;
}
```

- [ ] **Step 2: Add a `compute_overlay_left_x()` helper at the end of the anonymous namespace**

```cpp
// Compute the panel's current left x-coord based on overlay state +
// elapsed animation time. Returns kOverlayPanelClosedX when the
// panel is in Closed state (rendered off-screen).
int compute_overlay_left_x(LibraryScreen::OverlayState state,
                           std::chrono::steady_clock::time_point anim_started) {
    using S = LibraryScreen::OverlayState;
    if (state == S::Closed)  return kOverlayPanelClosedX;
    if (state == S::Open)    return kOverlayPanelOpenX;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(now - anim_started).count();
    if (state == S::SlidingIn) {
        const float t = std::min(1.0f,
            static_cast<float>(elapsed_ms) / static_cast<float>(kOverlaySlideInMs));
        const float eased = ease_out_cubic(t);
        return static_cast<int>(
            kOverlayPanelClosedX + eased *
                (kOverlayPanelOpenX - kOverlayPanelClosedX));
    }
    // SlidingOut.
    const float t = std::min(1.0f,
        static_cast<float>(elapsed_ms) / static_cast<float>(kOverlaySlideOutMs));
    const float eased = ease_in_cubic(t);
    return static_cast<int>(
        kOverlayPanelOpenX + eased *
            (kOverlayPanelClosedX - kOverlayPanelOpenX));
}
```

This signature requires `LibraryScreen::OverlayState` to be public in the header, OR the helper has to be a private static method on the class. Move it inside the class as a private static method to avoid leaking the enum:

```cpp
// In library_screen.h's private: section, alongside tick_overlay_animation:
static int compute_overlay_left_x(OverlayState state,
                                  std::chrono::steady_clock::time_point anim_started);
```

And implement in the .cpp as `int LibraryScreen::compute_overlay_left_x(...) { ... }`.

- [ ] **Step 3: Render the overlay panel at the end of LibraryScreen::render**

At the very END of `library_screen.cpp::render` (after the existing footer hint draw), add:

```cpp
    // ============================================================
    // Slide-in overlay (v1.6.x)
    // ============================================================
    if (overlay_state_ != OverlayState::Closed) {
        namespace chrome = ::media_browser::ui::chrome;
        const auto& th = r.mb_theme();

        const int panel_x = compute_overlay_left_x(
            overlay_state_, overlay_anim_started_at_);
        const float fpanel_x = static_cast<float>(panel_x);
        const float fpanel_y = static_cast<float>(kOverlayPanelTopY);
        const float fpanel_w = static_cast<float>(kOverlayPanelW);
        const float fpanel_h = static_cast<float>(kOverlayPanelH);

        // Panel background — bg_lift, fully opaque.
        r.mb_fill_rect(fpanel_x, fpanel_y, fpanel_w, fpanel_h, th.bg_lift);
        // Left edge: 2 px gold rule.
        r.mb_fill_rect(fpanel_x, fpanel_y, 2.0f, fpanel_h, th.accent);
        // Top edge: 1 px dim rule for closure with chrome header band.
        r.mb_fill_rect(fpanel_x, fpanel_y, fpanel_w, 1.0f, th.dim);

        // Panel content x — inset from left rule by panel inner pad.
        const int content_x = panel_x + kOverlayPanelInnerPadX;

        // ---- Stats block ----
        // Gold ZenDots heading: "LIBRARY" + active-sort subtitle.
        constexpr int kPanelTitleFontPx = 22;
        const int title_baseline = kOverlayPanelTopY + kOverlayPanelInnerPadY +
                                   kPanelTitleFontPx - 2;
        r.mb_draw_title_text("LIBRARY",
                             static_cast<float>(content_x),
                             static_cast<float>(title_baseline),
                             kPanelTitleFontPx, th.accent);

        // Three stat lines in cream body font, ~14 px each.
        constexpr int kStatFontPx = 14;
        const int stat_y0 = title_baseline + 24;
        char buf_titles[64], buf_used[64], buf_free[64];
        std::snprintf(buf_titles, sizeof(buf_titles), "%zu titles",
                      library_.size());
        // Reuse the format_bytes helper already in library_screen.cpp.
        int64_t used_bytes = 0;
        for (const Movie& m : library_) {
            if (m.has_file) used_bytes += m.file_size_bytes;
        }
        std::snprintf(buf_used, sizeof(buf_used), "%s used",
                      format_bytes(used_bytes).c_str());
        int64_t free_bytes = 0;
        {
            std::error_code ec;
            auto info = std::filesystem::space("/mnt/ssd/library", ec);
            if (!ec) free_bytes = static_cast<int64_t>(info.available);
        }
        std::snprintf(buf_free, sizeof(buf_free), "%s free",
                      free_bytes > 0 ? format_bytes(free_bytes).c_str() : "—");

        r.mb_draw_text(buf_titles,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0),
                       kStatFontPx, th.fg);
        r.mb_draw_text(buf_used,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0 + 18),
                       kStatFontPx, th.fg);
        r.mb_draw_text(buf_free,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0 + 36),
                       kStatFontPx, th.fg);

        // 1 px dim divider under stats.
        const int divider1_y = stat_y0 + 56;
        r.mb_draw_line(static_cast<float>(content_x),
                       static_cast<float>(divider1_y),
                       static_cast<float>(panel_x + kOverlayPanelW - kOverlayPanelInnerPadX),
                       static_cast<float>(divider1_y),
                       1.0f, th.dim, 0.5f);

        // ---- Sort by section ----
        constexpr int kSectionHeadingFontPx = 14;
        constexpr int kRowFontPx = 16;
        const int sort_heading_y = divider1_y + kOverlaySectionGap;
        r.mb_draw_title_text("SORT BY",
                             static_cast<float>(content_x),
                             static_cast<float>(sort_heading_y),
                             kSectionHeadingFontPx, th.accent);

        struct Row { const char* label; bool is_active; bool is_focused; };
        using S = ::app::AppState::DisplaySettings::MbLibrarySort;
        const S active_sort = state_.display_settings.mb_library_sort;
        const Row sort_rows[4] = {
            {"Recent", active_sort == S::Recent, overlay_focus_row_ == 0},
            {"Title",  active_sort == S::Title,  overlay_focus_row_ == 1},
            {"Year",   active_sort == S::Year,   overlay_focus_row_ == 2},
            {"Size",   active_sort == S::Size,   overlay_focus_row_ == 3},
        };
        // Subtitle: active-sort name in gold next to the heading
        // (right-aligned to the panel's content area).
        for (int i = 0; i < 4; ++i) {
            if (!sort_rows[i].is_active) continue;
            const std::string subtitle = std::string(" · ") + sort_rows[i].label;
            const int subtitle_w = r.mb_text_width(subtitle, kSectionHeadingFontPx);
            const int heading_w = r.mb_title_text_width("SORT BY", kSectionHeadingFontPx);
            r.mb_draw_text(subtitle,
                           static_cast<float>(content_x + heading_w),
                           static_cast<float>(sort_heading_y),
                           kSectionHeadingFontPx, th.accent);
        }

        const int sort_rows_y0 = sort_heading_y + kOverlaySectionHeaderH;
        for (int i = 0; i < 4; ++i) {
            const int row_y = sort_rows_y0 + i * kOverlayRowHeight;
            const ::ui::Color label_color = (sort_rows[i].is_active || sort_rows[i].is_focused)
                ? th.accent : th.dim;
            // Focused row gets a blinking gold ▶ marker 18 px to the
            // left of the label x. Reuses the same draw-cursor-marker
            // pattern from MbSettingsScreen (right-pointing variant).
            if (sort_rows[i].is_focused) {
                const float marker_x = static_cast<float>(content_x);
                const float marker_cy = static_cast<float>(row_y + kOverlayRowHeight / 2);
                constexpr float kMarkerHalfH = 6.0f;
                constexpr float kMarkerW = 7.2f;
                r.mb_fill_triangle(
                    marker_x,           marker_cy - kMarkerHalfH,
                    marker_x,           marker_cy + kMarkerHalfH,
                    marker_x + kMarkerW, marker_cy,
                    th.accent, 1.0f);
            }
            r.mb_draw_text(sort_rows[i].label,
                           static_cast<float>(content_x + 18),
                           static_cast<float>(row_y + kOverlayRowHeight - 10),
                           kRowFontPx, label_color);
        }

        // 1 px divider under sort.
        const int divider2_y = sort_rows_y0 + 4 * kOverlayRowHeight + 6;
        r.mb_draw_line(static_cast<float>(content_x),
                       static_cast<float>(divider2_y),
                       static_cast<float>(panel_x + kOverlayPanelW - kOverlayPanelInnerPadX),
                       static_cast<float>(divider2_y),
                       1.0f, th.dim, 0.5f);

        // ---- Filter section ----
        const int filter_heading_y = divider2_y + kOverlaySectionGap;
        r.mb_draw_title_text("FILTER",
                             static_cast<float>(content_x),
                             static_cast<float>(filter_heading_y),
                             kSectionHeadingFontPx, th.accent);

        using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
        const F active_filter = state_.display_settings.mb_library_filter;
        struct FilterRow { const char* label; bool is_active; bool is_focused; bool is_soon; };
        const FilterRow filter_rows[4] = {
            {"All",            active_filter == F::All,            overlay_focus_row_ == 4, false},
            {"Unwatched",      active_filter == F::Unwatched,      overlay_focus_row_ == 5, true},
            {"Missing files",  active_filter == F::MissingFiles,   overlay_focus_row_ == 6, false},
            {"Recently added", active_filter == F::RecentlyAdded,  overlay_focus_row_ == 7, false},
        };
        const int filter_rows_y0 = filter_heading_y + kOverlaySectionHeaderH;
        for (int i = 0; i < 4; ++i) {
            const int row_y = filter_rows_y0 + i * kOverlayRowHeight;
            const ::ui::Color label_color =
                filter_rows[i].is_soon ? th.dim
              : (filter_rows[i].is_active || filter_rows[i].is_focused) ? th.accent
              : th.dim;
            if (filter_rows[i].is_focused) {
                const float marker_x = static_cast<float>(content_x);
                const float marker_cy = static_cast<float>(row_y + kOverlayRowHeight / 2);
                constexpr float kMarkerHalfH = 6.0f;
                constexpr float kMarkerW = 7.2f;
                r.mb_fill_triangle(
                    marker_x,           marker_cy - kMarkerHalfH,
                    marker_x,           marker_cy + kMarkerHalfH,
                    marker_x + kMarkerW, marker_cy,
                    th.accent, 1.0f);
            }
            std::string label = filter_rows[i].label;
            if (filter_rows[i].is_soon) label += "  (soon)";
            r.mb_draw_text(label,
                           static_cast<float>(content_x + 18),
                           static_cast<float>(row_y + kOverlayRowHeight - 10),
                           kRowFontPx, label_color);
        }

        // ---- Footer hint inside the panel ----
        const int hint_y = kOverlayPanelBottomY - kOverlayPanelInnerPadY;
        r.mb_draw_text("BTN4 close   A select   Rotary nav",
                       static_cast<float>(content_x),
                       static_cast<float>(hint_y),
                       12, th.dim);
    }
```

- [ ] **Step 4: Wire BTN4 to open/close the overlay**

Replace the no-op BTN4 short-press handler from Task 3 in `library_screen.cpp::handle_input` with:

```cpp
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            // BTN4 toggles the slide-in overlay. Re-pressing closes it.
            if (overlay_state_ == OverlayState::Closed) {
                start_open_overlay();
            } else if (overlay_state_ == OverlayState::Open) {
                start_close_overlay();
            }
            // While SlidingIn or SlidingOut, ignore — operator will
            // wait ~200 ms for the animation to settle. Prevents
            // mid-animation state thrash.
            continue;
        }
```

- [ ] **Step 5: Capture rotary nav inside the overlay**

In `library_screen.cpp::handle_input`, BEFORE the existing PREV/NEXT/ROTATE handlers (so the overlay handlers run first when active), add:

```cpp
        // ============================================================
        // Overlay input capture: when the overlay is open or animating,
        // it consumes ROTATE / SELECT / PLAY_PAUSE / SETTINGS_MENU
        // inputs. The underlying grid does NOT see these events while
        // the panel is visible.
        // ============================================================
        const bool overlay_active = (overlay_state_ != OverlayState::Closed);
        if (overlay_active) {
            // BTN2 (PLAY_PAUSE, red) — back closes the overlay.
            if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
                if (overlay_state_ == OverlayState::Open ||
                    overlay_state_ == OverlayState::SlidingIn) {
                    start_close_overlay();
                }
                continue;
            }
            // Rotary CW/CCW — walk through the 8 focusable rows.
            if (e.action == platform::InputAction::ROTATE) {
                if (e.delta == 0) continue;
                overlay_focus_row_ = std::clamp(
                    overlay_focus_row_ + e.delta,
                    0, kOverlayFocusableRows - 1);
                continue;
            }
            // SELECT — wired in Task 6. For now, no-op.
            if (e.action == platform::InputAction::SELECT && e.pressed) {
                continue;
            }
            // BTN1 / BTN3 are no-ops while overlay is open (prevents
            // tab-jumping the underlying grid).
            if (e.action == platform::InputAction::PREV ||
                e.action == platform::InputAction::NEXT) {
                continue;
            }
            // SETTINGS_MENU is handled by the toggle branch above; let
            // it through.
        }
```

- [ ] **Step 6: Build + smoke-test (overlay should slide in / out, but selections do nothing yet)**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/magic_dingus_box_suite/magic_dingus_box "
PI_HOST=magic@10.55.0.1 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build > /tmp/task5.log 2>&1
grep -E "Build complete|error:" /tmp/task5.log | tail -3
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service && sleep 3 && systemctl is-active magic-dingus-box-cpp.service'
```

**Manual smoke test:**
1. Walk into Library. Press BTN4 — confirm panel slides in from the right (~200 ms).
2. Confirm three blocks visible: LIBRARY heading + 3 stat lines, SORT BY (Recent active in gold, others dim), FILTER (All active in gold, Unwatched dim with "(soon)" suffix, others dim).
3. Rotate rotary CW — cursor walks down through the 8 rows (Recent → Title → Year → Size → All → Unwatched → Missing files → Recently added). Cursor stays clamped at row 7 even when rotated past it.
4. Press BTN2 — panel slides out (~150 ms) and the underlying Library grid is interactable again.
5. Press BTN4 to re-open. Press BTN4 again — panel slides out (toggle behavior).
6. Press A on a row — nothing happens (Task 6 wires the selection).

- [ ] **Step 7: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/library_screen.h
git commit -m "feat(mb): Library slide-in overlay panel render + open/close"
```

---

### Task 6: Library overlay — wire selections + apply sort/filter to view

**Goal:** A on a row applies the corresponding sort or filter, persists the choice, and closes the overlay. The Library grid re-sorts / re-filters on the operator's next view rebuild.

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp` (overlay SELECT handler + `rebuild_view` updates)

- [ ] **Step 1: Wire the overlay SELECT handler**

Replace the no-op overlay SELECT branch from Task 5 with:

```cpp
            if (e.action == platform::InputAction::SELECT && e.pressed) {
                if (overlay_state_ != OverlayState::Open) continue;
                // Map focused row → sort or filter mutation.
                using S = ::app::AppState::DisplaySettings::MbLibrarySort;
                using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
                bool changed = false;
                switch (overlay_focus_row_) {
                    case 0: state_.display_settings.mb_library_sort = S::Recent;        changed = true; break;
                    case 1: state_.display_settings.mb_library_sort = S::Title;         changed = true; break;
                    case 2: state_.display_settings.mb_library_sort = S::Year;          changed = true; break;
                    case 3: state_.display_settings.mb_library_sort = S::Size;          changed = true; break;
                    case 4: state_.display_settings.mb_library_filter = F::All;           changed = true; break;
                    case 5: /* Unwatched is a placeholder — accept the click but render the row as "(soon)"; no real filter wired yet. */
                            state_.display_settings.mb_library_filter = F::Unwatched;    changed = true; break;
                    case 6: state_.display_settings.mb_library_filter = F::MissingFiles;  changed = true; break;
                    case 7: state_.display_settings.mb_library_filter = F::RecentlyAdded; changed = true; break;
                }
                if (changed) {
                    ::app::SettingsPersistence::save_settings(state_);
                    rebuild_view();
                    start_close_overlay();
                }
                continue;
            }
```

Make sure `#include "app/settings_persistence.h"` is at the top of `library_screen.cpp`. If not, add it alongside other app/ includes.

- [ ] **Step 2: Apply sort + filter in `rebuild_view`**

Find the existing `LibraryScreen::rebuild_view()` method. Currently it likely just copies `library_` pointers into `view_` (or applies the legacy All / Unwatched / MissingUpgrades / Recent filter). Replace the body with:

```cpp
void LibraryScreen::rebuild_view() {
    view_.clear();
    view_.reserve(library_.size());

    // ---- Filter pass ----
    using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
    const F filter = state_.display_settings.mb_library_filter;
    const auto now = std::chrono::system_clock::now();
    const auto thirty_days_ago = now - std::chrono::hours(24 * 30);
    for (const Movie& m : library_) {
        bool keep = true;
        switch (filter) {
            case F::All:
                keep = true;
                break;
            case F::Unwatched:
                // Placeholder: kiosk doesn't track watched-history yet.
                // Accept all rows so the operator sees something while
                // the (soon) feature is in development. Will switch to
                // `keep = !m.watched;` once Movie.watched lands.
                keep = true;
                break;
            case F::MissingFiles:
                keep = !m.has_file;
                break;
            case F::RecentlyAdded:
                // Movie.added is std::chrono::system_clock::time_point.
                keep = (m.added >= thirty_days_ago);
                break;
        }
        if (keep) view_.push_back(&m);
    }

    // ---- Sort pass ----
    using S = ::app::AppState::DisplaySettings::MbLibrarySort;
    const S sort = state_.display_settings.mb_library_sort;
    auto cmp_recent = [](const Movie* a, const Movie* b) {
        return a->added > b->added;  // newest first
    };
    auto cmp_title = [](const Movie* a, const Movie* b) {
        return ::strcasecmp(a->title.c_str(), b->title.c_str()) < 0;
    };
    auto cmp_year = [](const Movie* a, const Movie* b) {
        if (a->year != b->year) return a->year > b->year;  // newest first
        return ::strcasecmp(a->title.c_str(), b->title.c_str()) < 0;
    };
    auto cmp_size = [](const Movie* a, const Movie* b) {
        return a->file_size_bytes > b->file_size_bytes;  // largest first
    };
    switch (sort) {
        case S::Recent: std::sort(view_.begin(), view_.end(), cmp_recent); break;
        case S::Title:  std::sort(view_.begin(), view_.end(), cmp_title);  break;
        case S::Year:   std::sort(view_.begin(), view_.end(), cmp_year);   break;
        case S::Size:   std::sort(view_.begin(), view_.end(), cmp_size);   break;
    }

    // Clamp the grid cursor + scroll row to the new view's bounds.
    const int n = static_cast<int>(view_.size());
    if (grid_cursor_ >= n) grid_cursor_ = std::max(0, n - 1);
    const int max_scroll_row = n / kGridCols;
    if (scroll_row_ > max_scroll_row) scroll_row_ = max_scroll_row;
}
```

This requires:
- `#include <algorithm>` (for `std::sort`) — likely already present
- `#include <cstring>` (for `strcasecmp`) — add if missing
- `Movie` struct must have a `bool has_file`, `int year`, `int64_t file_size_bytes`, `std::string title`, and a `std::chrono::system_clock::time_point added` field. Verify these exist in `radarr_types.h`. **If `added` doesn't exist**, plumb it through: Radarr's `/api/v3/movie` response includes an `added` ISO-8601 string; parse it in `radarr_parsers.cpp` and store on `Movie`. (Add this prerequisite as Step 2.5 below if needed; otherwise skip.)

- [ ] **Step 2.5 (conditional): Plumb `Movie.added` if it's missing**

Check if `Movie` has the field:

```bash
grep -n "added" /Users/alexanderchaney/Documents/🧠\ Projects/magic_dingus_box_suite/magic_dingus_box\ /magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h
```

If no `added` field exists, add to `radarr_types.h`'s `Movie` struct:

```cpp
    // Date the movie was added to Radarr's library. ISO-8601 from the
    // /api/v3/movie response field "added"; parsed to a chrono
    // time_point in radarr_parsers.cpp. Used by LibraryScreen's
    // overlay sort = Recent and filter = RecentlyAdded.
    std::chrono::system_clock::time_point added{};
```

In `radarr_parsers.cpp`, find the `parse_movie` function and add an extraction:

```cpp
    if (j.isMember("added")) {
        // Radarr emits ISO-8601 strings like "2024-12-31T08:15:42Z".
        // Parse with std::get_time + std::mktime; clamp to epoch on
        // parse failure.
        std::tm tm = {};
        std::istringstream iss(j["added"].asString());
        iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        if (!iss.fail()) {
            const std::time_t tt = ::timegm(&tm);
            m.added = std::chrono::system_clock::from_time_t(tt);
        }
    }
```

This requires `#include <iomanip>`, `#include <sstream>`, `#include <ctime>` if not already present.

- [ ] **Step 3: Build + smoke-test (the full end-to-end overlay flow)**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/magic_dingus_box_suite/magic_dingus_box "
PI_HOST=magic@10.55.0.1 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build > /tmp/task6.log 2>&1
grep -E "Build complete|error:" /tmp/task6.log | tail -3
ssh magic@10.55.0.1 'sudo systemctl restart magic-dingus-box-cpp.service && sleep 3 && systemctl is-active magic-dingus-box-cpp.service'
```

**Manual smoke test:**
1. Walk into Library. Press BTN4 → overlay slides in.
2. Press A on `Title` (rotary down 1, click) — overlay slides out, library re-sorts alphabetically.
3. Press BTN4 → overlay opens with cursor on `Title` (the now-active sort).
4. Rotate to `Missing files`, click A — overlay closes, library shows only items where `has_file == false` (likely empty or near-empty depending on operator's library state).
5. Reboot the Pi: `ssh magic@10.55.0.1 'sudo reboot'`. Wait ~60 s for it to come back. Open Library — confirm sort is still `Title` and filter is still `Missing files`. Confirm `config/settings.json` has `"mb_library_sort": "title"` and `"mb_library_filter": "missing_files"`.
6. Open overlay, click `All` and `Recent` to restore defaults.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp
# Plus radarr_types.h / radarr_parsers.cpp if Step 2.5 was needed.
git commit -m "feat(mb): wire Library overlay SELECT — apply sort + filter, persist, close"
```

---

### Task 7: Final polish + spec smoke-test pass

**Goal:** Run the full 11-step manual smoke test from the spec on a fresh Pi state. Catch anything that's still off.

- [ ] **Step 1: Run the spec's 11-step test list end-to-end**

From the spec's "Testing notes" section:

1. Walk through 6 tabs L→R with BTN3, then R→L with BTN1. ✓
2. From Library, BTN4 → panel slides in from right.
3. Rotate through all 8 panel rows. Cursor lands on `Recent` initially (or whichever sort is active), walks down through Sort then Filter.
4. A on `Title` → overlay closes, library sorts alphabetically. Re-open overlay → `Title` is active.
5. A on `Missing files` → library filters to missing-only.
6. Reboot Pi → confirm sort + filter persist.
7. From Library, BTN2 → back to Browse.
8. From Detail (entered from Library), BTN2 → back to Library.
9. Playback: BTN2 still play-pause; BTN4 still exit to Detail.
10. From Browse / Search / Settings / Queue / Detail: BTN4 short-press is a no-op.
11. From open overlay: BTN1 / BTN3 are no-ops (don't tab-jump underlying grid).

- [ ] **Step 2: Document any operator-visible quirks discovered + fix**

If any test fails or reveals a usability issue, fix in this task before the version bump. Common likely issues:
- Overlay rendering at the wrong z-depth (drawn before something that overlaps it)
- Animation stutter on slow frames (acceptable as long as it doesn't hang)
- Cursor landing on a non-existent row after `library_` is shorter than expected

- [ ] **Step 3: Bump version + write CHANGELOG entry + commit + tag + push + GitHub release**

Bump VERSION to `1.6.3`. Add CHANGELOG entry following the v1.6.2 pattern with sections Added / Changed / Fixed / Notes for operators. Update OTA_UPDATE_GUARANTEES.md's "currently `vX.Y.Z`" line to `v1.6.3`, and consider adding a short v1.6.3 addition note about the new `display.mb_library_*` keys (already covered by the `config/*` exclusion, just worth surfacing). Then:

```bash
git add VERSION CHANGELOG.md OTA_UPDATE_GUARANTEES.md
git commit -m "release: v1.6.3 — Marquee Library filter overlay + Queue tab + input grammar redesign"
git tag -a v1.6.3 -m "v1.6.3 — Marquee Library filter overlay + Queue tab + input grammar redesign"
git push origin main
git push origin v1.6.3
gh release create v1.6.3 --title "v1.6.3 — Marquee Library filter overlay + Queue tab + input grammar redesign" --notes-from-tag
```

- [ ] **Step 4: Smoke-test post-release**

Verify the Pi accepts the OTA cleanly:

```bash
ssh magic@10.55.0.1 '/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh check'
```

Expected output: shows v1.6.3 as available. (Don't actually run the install — the kiosk is already on the latest binary via `deploy_cpp.sh`.)

---

## Self-review

Spec coverage check:
- 6-tab strip reorder → Task 2 ✓
- Queue tab as peer destination → Task 2 ✓
- Input grammar remap (BTN2=back, BTN4=overlay/no-op) → Task 3 ✓
- Slide-in overlay state machine + animation → Tasks 4–5 ✓
- Slide-in panel layout + content blocks → Task 5 ✓
- Sort + filter persistence → Task 1 ✓
- Apply sort + filter to view → Task 6 ✓
- Playback grammar preserved → Task 3 (explicit no-touch) ✓
- 11-step manual smoke test → Task 7 ✓

Placeholder scan: no TODO / TBD / "implement later" / vague-error-handling instructions.

Type consistency:
- `MbLibrarySort` / `MbLibraryFilter` enum class names match between Tasks 1, 5, 6.
- `OverlayState` matches between Tasks 4, 5.
- `kOverlayFocusableRows = 8` referenced consistently in Task 4 (declaration) and Task 5 (clamp).
- `start_open_overlay()` / `start_close_overlay()` / `tick_overlay_animation()` defined Task 4, called Task 5/6 with same names.
- Movie field accesses (`m.has_file`, `m.year`, `m.title`, `m.file_size_bytes`, `m.added`) consistent with Task 6's filter/sort lambdas; `m.added` flagged as conditional in Step 2.5 if missing.

Scope check: single feature, ~10 files touched, clean task boundaries, each task is buildable + smoke-testable on the Pi.

Plan ready.
