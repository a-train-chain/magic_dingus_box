#include "media_browser/ui/search_screen.h"
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

#include <spdlog/spdlog.h>

#include "media_browser/radarr/radarr_client.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

namespace media_browser::ui {

namespace {

// Retro home-menu-inspired layout (target 1280x720). The chrome here is
// intentionally identical in vocabulary to DetailScreen and BrowseScreen
// so all three screens read as one app:
//   - Top "SEARCH" header strip in the Zen Dots title font, steel-blue
//     (accent2), underlined with a full-width 2px rule. Same idiom as
//     DetailScreen's "FEATURE PRESENTATION" header.
//   - Query box: 2px gold outline, no fill, ▶ prefix in steel-blue,
//     blinking caret rendered as a 2px-wide gold rect (consistent with
//     the playlist-cursor's 500ms blink, sourced from epoch_ms).
//   - Section divider (steel-blue 2px @ alpha 0.85) between the
//     keyboard region and the results grid — same divider DetailScreen
//     uses above its action row.
//   - Result cells: poster + body-font title/year. Focused cell gets a
//     3px gold outline, plus a blinking ◂ in steel-blue at the
//     bottom-right (matches the home-menu/DetailScreen cursor idiom).
//   - "IN LIBRARY" chip in the focused-or-not cell uses the same
//     gold-outline / accent-text pattern as DetailScreen's genre chips.
//   - Footer hint: centered, dim, font_small. No filled bar.
//
// The virtual keyboard between the query box and the divider keeps its
// existing visual style — that widget is shared with the kiosk main UI
// and we deliberately avoid forking it here. The chrome around it, not
// the keyboard itself, is what carries the retro look.
// Horizontal safe-area inset. Matches mb_chrome::kSafeInset_px so the
// query box, keyboard, divider rule, and results grid all line up under
// the same vertical margin the chrome header uses — and, critically,
// stay clear of the 40px wood-frame overlay drawn around every Marquee
// screen. With kFrameInset_px=40 from chrome.h, 60px here gives a 20px
// breathing gap between content and the inner edge of the frame.
constexpr float kPaddingX            = 60.0f;     // matches chrome::kSafeInset_px

// Top header strip is now drawn by chrome::draw_screen_header (Marquee
// 5-tab strip + "Search" title). The screen anchors all body content
// off the y-coord that helper returns. The legacy kHeaderBaselineY /
// kHeaderRuleY constants were retired here as part of the v1.6.x
// chrome unification — see browse_screen.cpp / library_screen.cpp for
// the same migration pattern.

// Query box just below the chrome header. Outline-only (no fill), tall
// enough to comfortably hold font_large_size (24px) text + a caret.
constexpr float kQueryBoxMarginTop   = 18.0f;     // gap below chrome header
constexpr float kQueryBoxHeight      = 48.0f;
constexpr float kQueryBoxBorderW     = 2.0f;
constexpr float kQueryBoxPadX        = 16.0f;     // inner left/right padding

// Keyboard sits between the query box and the section divider that
// separates it from the results grid. The keyboard widget keeps its
// existing self-rendered look — we just allocate the region.
constexpr float kKbMarginTop         = 14.0f;     // gap below query box
constexpr float kKbMarginBottom      = 14.0f;     // gap above section rule
constexpr float kKeyGap              = 6.0f;
// Keyboard region ends here. Bumped from 0.40 → 0.50 in v1.6.x: the
// Marquee chrome header pushes the query box ~60px lower, so the kb
// region needs a corresponding shift down to keep ≥130px of vertical
// space for the keys (4 rows). Keeps the results-grid footprint
// unchanged for 720p — the empty-state message and 1-row poster strip
// still fit cleanly above the footer.
constexpr float kTopFrac             = 0.50f;     // keyboard region ends here

// Results grid — 9 columns, matching BrowseScreen / LibraryScreen.
// Cell width, poster height, and meta-area height are all computed at
// render time from the safe-area width and chrome::draw_poster_card's
// shared idiom; the only constant retained here is the gap above the
// grid (between the steel-blue divider and the first row of cells).
constexpr float kGridPaddingTop      = 18.0f;

// Friendlier on-key labels for the special keys that the widget stores
// as all-caps sentinels. Keeps the rendered text short enough to fit
// inside the small cells.
const char* display_label(const std::string& key) {
    if (key == "SPACE")  return "SPACE";
    if (key == "BACK")   return "BKSP";
    if (key == "ENTER")  return "OK";
    if (key == "CANCEL") return "X";
    return key.c_str();
}

}  // namespace

SearchScreen::SearchScreen(RadarrClient& radarr) : radarr_(radarr) {}

SearchScreen::~SearchScreen() {
    // Bump both gens so any in-flight worker sees its result is stale
    // and bails before publishing. Then join all tracked workers so we
    // don't have a thread holding references to *this after the screen
    // is destroyed. Workers call RadarrClient which has a 5s curl
    // timeout per request (single-attempt — Radarr is on-Pi-localhost,
    // no retry needed), so worst-case shutdown wait is ~5s. In
    // practice the lookup workers complete in 1-3s and the lib worker
    // in ~200ms.
    lib_current_gen_.fetch_add(1);
    lookup_current_gen_.fetch_add(1);
    lib_workers_.join_all();
    lookup_workers_.join_all();
}

void SearchScreen::enter() {
    // Start fresh each time the user opens Search. If they navigated in
    // from Browse, they want an empty query.
    query_.clear();
    last_queried_.clear();
    results_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    selected_tmdb_id_ = 0;
    focus_ = Focus::Keyboard;
    last_input_time_ = std::chrono::steady_clock::time_point::min();

    // Open the virtual keyboard with empty text. We don't use the
    // on_enter/on_cancel callbacks — this screen handles its own flow —
    // but the widget requires the open() call to become active and
    // initialize its selected_row/col.
    keyboard_.open("", "Search Movies",
                   /*on_enter=*/nullptr, /*on_cancel=*/nullptr);

    // Library cache used to be fetched synchronously here. Run sync it
    // blocked render for ~1s on every entry to the screen — visible as
    // a stutter on Browse->Search transitions. Now dispatched async; a
    // future update() tick drains it via apply_pending_lib(). Until
    // then, library_tmdb_ids_ may be empty, the IN LIBRARY chips
    // simply don't render, and BTN2 quick-add is gated on lib_loaded_
    // (with a "Loading library — please wait" hint) so the user can't
    // accidentally re-add a movie that's already in their library.
    start_lib_fetch();
}

void SearchScreen::start_lib_fetch() {
    lib_loading_ = true;
    const uint64_t my_gen = lib_current_gen_.fetch_add(1) + 1;
    spdlog::info("[SearchScreen] start_lib_fetch (gen={})", my_gen);
    lib_workers_.spawn([this, my_gen]() { run_lib_fetch(my_gen); });
}

void SearchScreen::run_lib_fetch(uint64_t gen) {
    LibFetchResult r;
    r.library = radarr_.get_library();
    if (gen != lib_current_gen_.load()) {
        spdlog::info("[SearchScreen] lib gen={} stale after get_library; discarding",
                     gen);
        return;
    }
    // Profiles are only fetched on the FIRST library load. Subsequent
    // refreshes (post-add) just need to refresh the in-library set.
    if (!library_cached_) {
        r.profiles = radarr_.get_quality_profiles();
        r.profiles_valid = true;
        if (gen != lib_current_gen_.load()) {
            spdlog::info("[SearchScreen] lib gen={} stale after get_quality_profiles; discarding",
                         gen);
            return;
        }
    }
    // Fetch queue alongside the library so apply_pending_lib() can populate
    // downloading_tmdb_ids_ without an extra HTTP round-trip.
    r.queue = radarr_.get_queue();
    if (gen != lib_current_gen_.load()) {
        spdlog::info("[SearchScreen] lib gen={} stale after get_queue; discarding",
                     gen);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(lib_result_mtx_);
        if (gen != lib_current_gen_.load()) return;  // re-check under lock
        lib_pending_ = std::move(r);
    }
    lib_result_ready_.store(true);
}

void SearchScreen::apply_pending_lib() {
    if (!lib_result_ready_.exchange(false)) return;
    LibFetchResult incoming;
    {
        std::lock_guard<std::mutex> lk(lib_result_mtx_);
        incoming = std::move(lib_pending_);
    }
    // Build radarr_id → tmdb_id map for queue cross-reference.
    std::unordered_map<int, int> radarr_to_tmdb;
    library_tmdb_ids_.clear();
    for (const auto& m : incoming.library) {
        if (m.tmdb_id > 0) {
            library_tmdb_ids_.insert(m.tmdb_id);
            radarr_to_tmdb[m.radarr_id] = m.tmdb_id;
        }
    }
    // Populate the downloading set.
    downloading_tmdb_ids_.clear();
    for (const auto& qi : incoming.queue) {
        auto it = radarr_to_tmdb.find(qi.movie_id);
        if (it != radarr_to_tmdb.end()) {
            downloading_tmdb_ids_.insert(it->second);
        }
    }
    if (incoming.profiles_valid) {
        quality_profiles_ = std::move(incoming.profiles);
        library_cached_ = true;
    }
    lib_loaded_ = true;
    lib_loading_ = false;
    spdlog::info("[SearchScreen] applied lib: {} ids, {} profiles, {} downloading",
                 library_tmdb_ids_.size(), quality_profiles_.size(),
                 downloading_tmdb_ids_.size());
}

void SearchScreen::quick_add_focused() {
    // Only fires from the results grid; no-op on the keyboard.
    if (focus_ != Focus::Results) return;
    if (results_.empty()) return;
    if (grid_cursor_ < 0 ||
        grid_cursor_ >= static_cast<int>(results_.size())) return;
    const auto& hit = results_[grid_cursor_];
    if (hit.tmdb_id <= 0) return;

    // Block the add until the in-library cache has populated at least
    // once. Without this, the user could fire a quick-add before the
    // async lib_ fetch returns, and we'd happily re-add a movie that's
    // already in their library (Radarr would then create a duplicate).
    // A toast keeps the user oriented; the lib fetch is fast enough
    // that they can retry within a second.
    if (!lib_loaded_) {
        ::ui::Toast::show("Loading library — please wait");
        return;
    }

    if (library_tmdb_ids_.count(hit.tmdb_id) > 0) {
        ::ui::Toast::show("Already in library");
        return;
    }

    int qp = 0;
    for (const auto& p : quality_profiles_) {
        if (p.name == "HD-1080p") { qp = p.id; break; }
    }
    if (qp == 0 && !quality_profiles_.empty()) qp = quality_profiles_.front().id;
    if (qp == 0) {
        ::ui::Toast::show("No quality profile — check Radarr");
        return;
    }

    // add_movie() stays synchronous — it's a user-initiated mutation,
    // expected to block briefly, and serializing it with the lookup
    // pipeline would add significant complexity for no real win.
    bool ok = radarr_.add_movie(hit.tmdb_id, qp, /*monitor=*/true);
    if (!ok) {
        ::ui::Toast::show("Add failed — see Radarr logs");
        return;
    }
    // Insert into the local cache immediately so the IN LIBRARY chip
    // appears without waiting for the async refresh below.
    library_tmdb_ids_.insert(hit.tmdb_id);
    std::string msg = "Added: ";
    msg += (hit.title.empty() ? "movie" : hit.title);
    ::ui::Toast::show(msg);

    // Re-pull the library async so any other state Radarr gained from
    // the add (radarr_id, etc.) gets picked up next tick. Using the
    // async path here (instead of a direct radarr_.get_library() call)
    // matches the Task 6 invariant that all library refreshes go
    // through the same pipeline.
    start_lib_fetch();
}

void SearchScreen::run_lookup_if_due() {
    if (query_ == last_queried_) return;
    if (last_input_time_ == std::chrono::steady_clock::time_point::min()) return;
    auto now = std::chrono::steady_clock::now();
    auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_input_time_).count();
    if (idle_ms < kDebounceMs) return;

    last_queried_ = query_;
    if (query_.empty()) {
        // Clearing the query: drop visible results immediately and
        // bump the gen so any in-flight lookup is discarded — we don't
        // want a stale "wo" lookup to land after the user cleared back
        // to empty.
        lookup_current_gen_.fetch_add(1);
        results_.clear();
        grid_cursor_ = 0;
        scroll_row_ = 0;
        lookup_loading_ = false;
        return;
    }
    start_lookup(query_);
}

void SearchScreen::start_lookup(std::string query) {
    lookup_loading_ = true;
    const uint64_t my_gen = lookup_current_gen_.fetch_add(1) + 1;
    spdlog::info("[SearchScreen] lookup gen={} query='{}'",
                 my_gen, query);
    // Pass query by value into the worker — the user keeps typing, so
    // query_ might mutate in flight. Each worker captures its own copy.
    lookup_workers_.spawn([this, my_gen, q = std::move(query)]() mutable {
        run_lookup(my_gen, std::move(q));
    });
}

void SearchScreen::run_lookup(uint64_t gen, std::string query) {
    auto result = radarr_.lookup(query);
    if (gen != lookup_current_gen_.load()) {
        spdlog::info("[SearchScreen] lookup gen={} stale at publish; discarding",
                     gen);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(lookup_result_mtx_);
        if (gen != lookup_current_gen_.load()) return;  // re-check under lock
        lookup_pending_ = std::move(result);
    }
    lookup_result_ready_.store(true);
}

void SearchScreen::apply_pending_lookup() {
    if (!lookup_result_ready_.exchange(false)) return;
    std::vector<MovieSearchHit> incoming;
    {
        std::lock_guard<std::mutex> lk(lookup_result_mtx_);
        incoming = std::move(lookup_pending_);
    }
    results_ = std::move(incoming);
    grid_cursor_ = 0;
    scroll_row_ = 0;
    lookup_loading_ = false;
    spdlog::info("[SearchScreen] applied lookup: {} results", results_.size());
}

void SearchScreen::update() {
    // Join+drop finished workers (instant — see worker_pool.h). Without
    // this, every lookup left its thread object pinned until shutdown.
    lib_workers_.reap();
    lookup_workers_.reap();

    // Phone Remote text input: the on-screen-keyboard SELECT path syncs
    // keyboard_.get_text() into query_ inside handle_input() — but when
    // the Phone Remote calls keyboard_.type_char() / backspace() /
    // clear_buffer() directly via the queue drainer, that path is
    // bypassed. Poll the buffer here so external buffer changes get
    // picked up by the debouncer below. Cheap (string compare on a
    // short buffer); only triggers a query when content actually
    // differs.
    if (keyboard_.get_text() != query_) {
        query_ = keyboard_.get_text();
        last_input_time_ = std::chrono::steady_clock::now();
    }

    // Drain finished async results first so the rest of update() (and
    // the imminent render() call) sees the latest state.
    apply_pending_lib();
    apply_pending_lookup();
    run_lookup_if_due();
}

Screen SearchScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // BTN4 (SETTINGS_MENU, black) — toggles focus between the virtual
        // keyboard (top region) and the results grid (bottom region).
        // No-op when there are no results so the user isn't confused by
        // BTN4 appearing to do nothing meaningful.
        // Long-press to exit MB still works via the input dispatcher's
        // long-press branch (handled before handle_input is called).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            if (!results_.empty()) {
                focus_ = (focus_ == Focus::Keyboard)
                             ? Focus::Results
                             : Focus::Keyboard;
                if (focus_ == Focus::Results && grid_cursor_ < 0) {
                    grid_cursor_ = 0;
                    scroll_row_  = 0;
                }
            }
            continue;
        }

        // BTN1 / PREV (yellow): walk one tab left in the Marquee strip.
        // v1.6.x 6-tab strip: Popular | Top Rated | Search | Library |
        // Queue | Settings. Search is at index 2, so PREV walks back to
        // Browse (which retains its category_ across transitions, so the
        // user resumes on whichever content tab they were last on).
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Browse;
        }

        // BTN3 / NEXT (green): walk one tab right. Search is at index 2,
        // so NEXT goes to Library at index 3.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            return Screen::Library;
        }

        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal
        // in main.cpp. It never reaches here; no per-screen handler needed.

        // Horizontal nav: always to the keyboard regardless of focus —
        // the results grid navigates with left/right too, but we model
        // the grid as only reachable via DOWN from the keyboard's
        // bottom row, and LEFT/RIGHT there still makes sense as grid
        // step. Implementation: if Focus::Results, use results
        // navigation; otherwise delegate to keyboard.
        if (e.action == platform::InputAction::ROTATE) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::Keyboard) {
                if (delta > 0) keyboard_.navigate_right();
                else           keyboard_.navigate_left();
            } else {
                // Results grid: horizontal step.
                if (results_.empty()) continue;
                int n = static_cast<int>(results_.size());
                grid_cursor_ = std::clamp(grid_cursor_ + delta, 0, n - 1);
            }
            continue;
        }

        // Vertical nav: special edge handling to hop between regions.
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::Keyboard) {
                if (delta > 0) {
                    // DOWN. If we're on the keyboard's last row AND we
                    // have results, hop to the results grid. Otherwise
                    // navigate down within the keyboard.
                    int last_row = static_cast<int>(keyboard_.get_layout().size()) - 1;
                    if (keyboard_.get_selected_row() == last_row &&
                        !results_.empty()) {
                        focus_ = Focus::Results;
                        grid_cursor_ = 0;
                        scroll_row_ = 0;
                    } else {
                        keyboard_.navigate_down();
                    }
                } else {
                    keyboard_.navigate_up();
                }
            } else {
                // In results grid.
                int row = grid_cursor_ / kGridCols;
                int col = grid_cursor_ % kGridCols;
                if (delta < 0 && row == 0) {
                    // UP from top row: focus back to keyboard.
                    focus_ = Focus::Keyboard;
                } else {
                    int new_row = row + (delta > 0 ? 1 : -1);
                    int max_row = results_.empty()
                                  ? 0
                                  : (static_cast<int>(results_.size()) - 1) / kGridCols;
                    new_row = std::clamp(new_row, 0, max_row);
                    int new_idx = new_row * kGridCols + col;
                    if (new_idx < static_cast<int>(results_.size())) {
                        grid_cursor_ = new_idx;
                    } else if (!results_.empty()) {
                        grid_cursor_ = static_cast<int>(results_.size()) - 1;
                    }
                }
            }
            continue;
        }

        // Select: keyboard keystroke or poster activation.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (focus_ == Focus::Keyboard) {
                // Snapshot the text buffer; call select() (which may
                // append a char, backspace, toggle caps/symbols, or
                // fire the ENTER callback); if the buffer changed,
                // mark now as the last-input time for the debounce.
                std::string before = keyboard_.get_text();
                keyboard_.select();
                std::string after = keyboard_.get_text();
                if (after != before) {
                    query_ = after;
                    last_input_time_ = std::chrono::steady_clock::now();
                }
                // ENTER and CANCEL both call close() internally, which
                // sets active_=false. If we don't handle that here, the
                // user is left staring at a dead keyboard with no
                // escape but BTN4. Treat it as "done entering text":
                // if we have results, move focus to the grid; if not,
                // silently re-open the keyboard with the current query
                // so typing can continue (no-op escape).
                if (!keyboard_.is_active()) {
                    if (!results_.empty()) {
                        focus_ = Focus::Results;
                        grid_cursor_ = 0;
                        scroll_row_ = 0;
                    } else {
                        keyboard_.open(query_, "Search Movies",
                                       /*on_enter=*/nullptr,
                                       /*on_cancel=*/nullptr);
                    }
                }
                continue;
            }
            // Results grid: pick this poster and hand off to Detail.
            if (!results_.empty() &&
                grid_cursor_ >= 0 &&
                grid_cursor_ < static_cast<int>(results_.size())) {
                selected_tmdb_id_ = results_[grid_cursor_].tmdb_id;
                return Screen::Detail;
            }
            continue;
        }
    }

    return Screen::Search;
}

void SearchScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    r.mb_fill_background();

    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // 500ms blink cycle, sourced from epoch time so it stays in
    // lockstep with the home-menu's playlist cursor and DetailScreen's
    // action-button marker — every blink across the app breathes
    // together when transitioning between screens.
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const bool blink_on = (epoch_ms / 500) % 2 == 0;

    // ---------------------------------------------------------------
    // Marquee header — "Search" title (left) + the same 5-tab strip
    // BrowseScreen and LibraryScreen render, with "Search" marked active
    // (gold border + gold label). Keeps visual continuity across the
    // whole Marquee section so the user sees one consistent UI when
    // walking BTN1/BTN3 across the strip.
    //
    // The previous "searching..." / "N results" status indicator that
    // used to sit on the right is dropped — the keyboard + results
    // body provide the same feedback (caret blink while typing, the
    // result strip visibly populating below the keyboard once a query
    // resolves). If we want to surface load state explicitly later we
    // can put it as a small sub-line below the input box rather than
    // fight the tab strip for header real estate.
    // ---------------------------------------------------------------
    int body_top_y = 0;
    {
        namespace chrome = ::media_browser::ui::chrome;
        // Shared strip (chrome::marquee_tabs) — this screen used to carry
        // its own six-label copy, which silently lost the "For You" chip
        // when that tab was added to Browse.
        const std::vector<chrome::TabSpec> tabs = chrome::marquee_tabs("Search");
        body_top_y = chrome::draw_screen_header(
            r, screen_w, "Search", tabs, /*focused_tab=*/-1);
    }
    // blink_on may temporarily be unused if the body branches drop it
    // (e.g., empty state without caret). Suppress -Wunused-variable.
    (void)blink_on;

    // ---------------------------------------------------------------
    // Query box: 2px gold outline, no fill. ▶ prefix in steel-blue
    // matches the home-menu's playlist-item arrow vocabulary; the
    // query text itself is in fg (warm cream); a 2px-wide gold caret
    // (rendered as a primitive rect, NOT an underscore glyph) blinks
    // at the end of the text on the same 500ms cadence as every other
    // cursor in the app.
    // ---------------------------------------------------------------
    const float qb_x = kPaddingX;
    // Anchor below the chrome header (which already includes the tab strip
    // and a section rule). body_top_y is the Y at which the chrome ends; a
    // small inset breathes the query box off that rule.
    const float qb_y = static_cast<float>(body_top_y) + kQueryBoxMarginTop;
    const float qb_w = w - 2.0f * kPaddingX;
    const float qb_h = kQueryBoxHeight;

    {
        r.mb_stroke_rect(qb_x, qb_y, qb_w, qb_h,
                         kQueryBoxBorderW, th.accent, 0.95f);

        const int qf       = th.font_large_size;
        const int qf_base  = r.mb_text_baseline(qf);
        // Vertically center the text glyph baseline inside the box.
        const float text_y = qb_y + (qb_h / 2.0f)
                           - static_cast<float>(qf) / 2.0f
                           + static_cast<float>(qf_base);

        // ▶ prefix in steel-blue — accent2 against gold border keeps
        // the "active text field" cue visible without over-using gold.
        const std::string prefix = "\xE2\x96\xB6 ";  // U+25B6 + space
        const float prefix_x = qb_x + kQueryBoxPadX;
        r.mb_draw_text(prefix, prefix_x, text_y, qf, th.dim, 0.95f);
        const float prefix_w = static_cast<float>(
            r.mb_text_width(prefix, qf));

        const float text_x = prefix_x + prefix_w;
        if (query_.empty()) {
            // Placeholder is dim, smaller than the query font would
            // be — uses the same font_large_size so the ▶ + caret
            // alignment doesn't jump when the user starts typing.
            r.mb_draw_text("Type a movie title...",
                           text_x, text_y, qf, th.dim, 0.7f);
        } else {
            // Truncate at the visible width of the box (leave room
            // for the caret + right padding) so long queries never
            // bleed across the gold border.
            const float text_max_w = qb_w - (text_x - qb_x)
                                   - kQueryBoxPadX - 12.0f;
            std::string drawn = truncate_to_width(r, query_, qf,
                                                  text_max_w);
            r.mb_draw_text(drawn, text_x, text_y, qf, th.fg, 1.0f);

            // Blinking text caret — 2px-wide gold rect, height ≈ glyph
            // height. We anchor it just past the rendered text width
            // so it visually "sits at" the insertion point.
            if (blink_on) {
                const float caret_w = 2.0f;
                const float caret_h = static_cast<float>(qf);
                const float caret_x = text_x
                    + static_cast<float>(r.mb_text_width(drawn, qf))
                    + 2.0f;
                const float caret_y = qb_y + (qb_h - caret_h) / 2.0f;
                r.mb_fill_rect(caret_x, caret_y, caret_w, caret_h,
                               th.accent, 1.0f);
            }
        }
    }

    // ---------------------------------------------------------------
    // Virtual keyboard region.
    //
    // The keyboard widget (ui::VirtualKeyboard) does not own its own
    // render() method — by convention in this codebase, the screen
    // that hosts the widget draws it from the widget's exposed state
    // (get_layout / get_selected_row / get_selected_col). We
    // deliberately keep the existing key-cell visuals (filled cells
    // with a thin dim border, accent fill on the focused key) so the
    // keyboard reads identically to its other appearances in the
    // kiosk main UI. Only the chrome around it gets the retro
    // makeover; this preserves muscle memory for users who already
    // know the keyboard from the home menu.
    // ---------------------------------------------------------------
    const float kb_top = qb_y + qb_h + kKbMarginTop;
    const float kb_bot = h * kTopFrac - kKbMarginBottom;
    const float kb_h_  = std::max(120.0f, kb_bot - kb_top);
    const float kb_x   = kPaddingX;
    const float kb_w   = w - 2.0f * kPaddingX;

    {
        const auto& layout = keyboard_.get_layout();
        const int nrows = static_cast<int>(layout.size());
        const float row_h = (kb_h_ - kKeyGap * std::max(0, nrows - 1))
                          / static_cast<float>(std::max(1, nrows));

        const int kb_font     = th.font_medium_size;
        const int kb_baseline = r.mb_text_baseline(kb_font);
        const bool kb_focused = (focus_ == Focus::Keyboard);

        for (int row = 0; row < nrows; ++row) {
            const auto& keys = layout[row];
            const int ncols = static_cast<int>(keys.size());
            const float col_w = (kb_w - kKeyGap * std::max(0, ncols - 1))
                              / static_cast<float>(std::max(1, ncols));
            const float ky = kb_top + row * (row_h + kKeyGap);

            for (int col = 0; col < ncols; ++col) {
                const float kx = kb_x + col * (col_w + kKeyGap);
                const bool is_selected =
                    kb_focused &&
                    row == keyboard_.get_selected_row() &&
                    col == keyboard_.get_selected_col();

                // Preserve the keyboard's existing visual style: gold
                // fill on the focused key, dim "action" fill on the
                // rest, thin dim hairline border. Don't redesign.
                const ::ui::Color& bg = is_selected ? th.accent : th.dim;
                r.mb_fill_rect(kx, ky, col_w, row_h, bg,
                               kb_focused ? 0.95f : 0.45f);
                r.mb_stroke_rect(kx, ky, col_w, row_h, 1.0f, th.dim, 0.5f);

                const char* lbl = display_label(keys[col]);
                int label_size = kb_font;
                if (std::string(lbl).length() > 1)
                    label_size = th.font_small_size;
                const int label_baseline = (label_size == kb_font)
                                           ? kb_baseline
                                           : r.mb_text_baseline(label_size);
                const float tw = static_cast<float>(
                    r.mb_text_width(lbl, label_size));
                const float tx = kx + (col_w - tw) / 2.0f;
                const float ty = ky + (row_h / 2.0f)
                               - static_cast<float>(label_size) / 2.0f
                               + static_cast<float>(label_baseline);

                const ::ui::Color& fg = is_selected ? th.bg : th.fg;
                r.mb_draw_text(lbl, tx, ty, label_size, fg,
                               kb_focused ? 1.0f : 0.7f);
            }
        }
    }

    // ---------------------------------------------------------------
    // Section divider between keyboard region and results grid.
    // Steel-blue 2px rule at alpha 0.85 — same divider DetailScreen
    // uses above its action row. Visually anchors the screen into
    // two clear bands (entry / output).
    // ---------------------------------------------------------------
    const float sep_y = h * kTopFrac;
    r.mb_draw_line(kPaddingX, sep_y,
                   w - kPaddingX, sep_y,
                   2.0f, th.dim, 0.85f);

    // ---------------------------------------------------------------
    // Results grid — 9-column poster-card layout matching BrowseScreen
    // and LibraryScreen exactly. Cell width is computed dynamically
    // from the safe-area width and gap, so all three Marquee grids
    // produce the same poster size at the same render resolution.
    //
    //   cell_w  = (content_w - (kGridCols-1) * kCellGap) / kGridCols
    //   poster_h = cell_w * 1.5   (2:3 movie-poster aspect)
    //   cell_h  = poster_h + kMetaGap + kMetaTotalH
    //
    // The keyboard takes the upper ~50% of the screen so typically only
    // ONE row of posters is visible at a time. When more results exist,
    // ROTATE_VERTICAL scrolls through them. The bottom of the grid is
    // anchored above the chrome footer-hint band (h - 86 px) so the
    // last row's title meta line never gets covered by the BTN1/A/etc
    // hint chips.
    // ---------------------------------------------------------------
    constexpr int kCellGap     = 8;
    constexpr int kRowGap      = 22;   // matches BrowseScreen
    constexpr int kMetaFontPx  = 14;   // 14 px legibility floor under CRT
    constexpr int kMetaLineGap = 2;
    constexpr int kMetaTotalH  = kMetaFontPx + kMetaLineGap + kMetaFontPx; // 30
    constexpr int kMetaGap     = 4;    // poster-bottom → meta-top

    const int content_w = static_cast<int>(w) - 2 * static_cast<int>(kPaddingX);
    const int cell_w    = (content_w - (kGridCols - 1) * kCellGap) / kGridCols;
    const int poster_h  = static_cast<int>(static_cast<float>(cell_w) * 1.5f);
    const int cell_h    = poster_h + kMetaGap + kMetaTotalH;

    const float grid_top    = sep_y + kGridPaddingTop;
    // Clears the chrome footer-hint band: footer baseline = h - 56,
    // band height ≈ 30 — so 86 px from the bottom keeps the last
    // visible meta line legibly above the hint chips (matches
    // mb_settings_screen's list_bottom math).
    const float grid_bottom = h - 86.0f;
    const float grid_h      = grid_bottom - grid_top;

    const int visible_rows = std::max(1,
        static_cast<int>(grid_h / (cell_h + kRowGap)));

    // Page-based scroll: snap scroll_row_ to the page that contains the cursor.
    // Each page is visible_rows tall (typically 1 for Search since the keyboard
    // occupies the upper half — but we keep the formula generic so a larger
    // visible_rows also snaps correctly).
    if (focus_ == Focus::Results && !results_.empty()) {
        const int focused_row = grid_cursor_ / kGridCols;
        scroll_row_ = (focused_row / visible_rows) * visible_rows;
    }

    const int total_rows = results_.empty() ? 0
                         : (static_cast<int>(results_.size()) - 1) / kGridCols + 1;
    const int end_row = std::min(total_rows, scroll_row_ + visible_rows);

    // ---- Empty / loading / no-match states --------------------------
    // Drop-fills are out — these are dim-text-only messages centered
    // in the grid region, so the steel-blue divider above stays the
    // visual anchor instead of a competing block of color.
    if (results_.empty()) {
        std::string msg;
        ::ui::Color msg_color = th.dim;
        float       msg_alpha = 0.85f;
        int         msg_size  = th.font_large_size;
        if (query_.empty()) {
            // Placeholder for the "haven't typed anything yet" state.
            msg = "Type a movie title...";
        } else if (lookup_loading_) {
            // Header already shows "searching..." in accent — repeat
            // it here large-and-centered as the primary affordance.
            msg = "Searching...";
            msg_color = th.accent;
            msg_alpha = 0.95f;
        } else if (query_ != last_queried_) {
            // User typed but the 400ms debounce hasn't fired yet;
            // keep this minimal so it doesn't flash on every keystroke.
            msg = "...";
            msg_size = th.font_medium_size;
        } else {
            // Smart-quoted "No results for «query»" — uses the same
            // U+00AB / U+00BB guillemets as the retro vocabulary. If
            // the query is long, truncate it inside the message.
            const std::string open_q  = "\xC2\xAB";   // U+00AB
            const std::string close_q = "\xC2\xBB";   // U+00BB
            std::string q_drawn = truncate_to_width(
                r, query_, msg_size, w - 2.0f * kPaddingX - 240.0f);
            msg = "No results for " + open_q + q_drawn + close_q;
        }
        const int   mw       = r.mb_text_width(msg, msg_size);
        const float msg_x    = (w - static_cast<float>(mw)) / 2.0f;
        const float msg_y    = grid_top + grid_h / 2.0f
                             + static_cast<float>(r.mb_text_baseline(msg_size)) / 2.0f;
        r.mb_draw_text(msg, msg_x, msg_y, msg_size, msg_color, msg_alpha);
    }

    // ---- Cells ------------------------------------------------------
    // Same idiom as BrowseScreen: chrome::draw_poster_card draws the
    // tinted background, the optional IN LIBRARY badge, and the year
    // pill (semi-transparent dark backing on the bottom-right corner).
    // Title text wraps to 2 lines below the poster — line 1 is the
    // longest leading word-prefix that fits, line 2 is the truncated
    // remainder. Year never appears on the meta line; it lives inside
    // the card as a pill, so the meta region only shows the title.
    namespace chrome = ::media_browser::ui::chrome;
    const float grid_left = kPaddingX;
    for (int row = scroll_row_; row < end_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            const int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(results_.size())) break;
            const auto& m = results_[idx];

            const int x = static_cast<int>(grid_left)
                        + col * (cell_w + kCellGap);
            const int y = static_cast<int>(grid_top)
                        + (row - scroll_row_) * (cell_h + kRowGap);

            // Poster CARD: tint fill + IN LIBRARY / DOWNLOADING badge +
            // year pill in one shared helper. Real artwork can later
            // overlay this (mb_draw_poster_or_tint) but the styled card
            // alone reads as a designed slot even before TMDB images load.
            const ::ui::Color tint = stable_tint_for_id(m.tmdb_id);
            const bool in_library =
                (m.tmdb_id > 0 &&
                 library_tmdb_ids_.count(m.tmdb_id) > 0);
            const bool is_downloading =
                (m.tmdb_id > 0 &&
                 downloading_tmdb_ids_.count(m.tmdb_id) > 0);
            chrome::draw_poster_card(
                r, x, y, cell_w, poster_h,
                m.title, m.year,
                tint, in_library,
                /*download_pct=*/is_downloading ? 0 : -1,
                /*poster_url=*/m.poster_url);

            // Meta line below poster: title only, wrapped to 2 lines
            // when needed. Same word-prefix-walking algorithm Browse
            // uses, so titles render identically across both screens.
            const std::string& title =
                m.title.empty() ? std::string("Untitled") : m.title;
            const float max_w_f = static_cast<float>(cell_w);
            std::string line1, line2;
            if (r.mb_text_width(title, kMetaFontPx) <= max_w_f) {
                line1 = title;
            } else {
                size_t split = std::string::npos;
                size_t pos = 0;
                while (true) {
                    size_t next = title.find(' ', pos + 1);
                    if (next == std::string::npos) break;
                    if (r.mb_text_width(title.substr(0, next), kMetaFontPx)
                            > max_w_f) break;
                    split = next;
                    pos = next;
                }
                if (split == std::string::npos) {
                    line1 = truncate_to_width(r, title, kMetaFontPx, max_w_f);
                } else {
                    line1 = title.substr(0, split);
                    std::string remainder = title.substr(split + 1);
                    line2 = truncate_to_width(r, remainder, kMetaFontPx, max_w_f);
                }
            }
            const int meta_top = y + poster_h + kMetaGap;
            r.mb_draw_text(line1,
                           static_cast<float>(x),
                           static_cast<float>(meta_top + kMetaFontPx),
                           kMetaFontPx, th.dim);
            if (!line2.empty()) {
                r.mb_draw_text(line2,
                               static_cast<float>(x),
                               static_cast<float>(meta_top + kMetaFontPx
                                                  + kMetaLineGap + kMetaFontPx),
                               kMetaFontPx, th.dim);
            }

            // Focus ring on the cursor cell — 2 px gold, 2 px outside.
            // Only when the user has navigated INTO the results region;
            // while typing on the keyboard the highlight stays muted.
            const bool focused =
                (focus_ == Focus::Results && idx == grid_cursor_);
            if (focused) {
                chrome::draw_focus_ring(r, x, y, cell_w, poster_h);
            }
            (void)blink_on;  // Caret blink is the only blink Search uses now.
        }
    }

    // ---------------------------------------------------------------
    // Bottom hint — centered, dim, font_small. No filled bar; the
    // page is bordered by the steel-blue rules at the top and the
    // section divider, and another fill block here would feel heavy.
    // The hint text changes by focus region so the user always sees
    // the inputs that matter for what's currently active.
    // ---------------------------------------------------------------
    // Marquee design: bordered-key glyphs via mb_chrome::draw_footer_hints,
    // shared across all Marquee screens.
    // Local namespace alias for brevity in the two-branch hint dispatch.
    namespace mc = ::media_browser::ui::chrome;
    if (focus_ == Focus::Keyboard) {
        mc::draw_footer_hints(r, screen_w, screen_h, {
            {mc::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
            {mc::HintIcon::Btn2Red,     "Exit"},
            {mc::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
            {mc::HintIcon::Btn4Black,   results_.empty() ? "\xE2\x80\x94" : "Grid"},
            {mc::HintIcon::RotaryNav,   "Type"},
            {mc::HintIcon::RotaryPress, "Type"},
        });
    } else {
        mc::draw_footer_hints(r, screen_w, screen_h, {
            {mc::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
            {mc::HintIcon::Btn2Red,     "Exit"},
            {mc::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
            {mc::HintIcon::Btn4Black,   "Keyboard"},
            {mc::HintIcon::RotaryNav,   "Browse"},
            {mc::HintIcon::RotaryPress, "Detail"},
        });
    }
}

}  // namespace media_browser::ui
