#include "media_browser/ui/search_screen.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "media_browser/radarr/radarr_client.h"
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
constexpr float kPaddingX            = 32.0f;     // matches DetailScreen

// Top header strip — same dimensions as DetailScreen so the rule sits
// at exactly the same Y across screens (visual continuity when
// transitioning).
constexpr float kHeaderBaselineY     = 38.0f;
constexpr float kHeaderRuleY         = 58.0f;

// Query box just below the header strip. Outline-only (no fill), tall
// enough to comfortably hold font_large_size (24px) text + a caret.
constexpr float kQueryBoxMarginTop   = 18.0f;     // gap below header rule
constexpr float kQueryBoxHeight      = 48.0f;
constexpr float kQueryBoxBorderW     = 2.0f;
constexpr float kQueryBoxPadX        = 16.0f;     // inner left/right padding

// Keyboard sits between the query box and the section divider that
// separates it from the results grid. The keyboard widget keeps its
// existing self-rendered look — we just allocate the region.
constexpr float kKbMarginTop         = 14.0f;     // gap below query box
constexpr float kKbMarginBottom      = 14.0f;     // gap above section rule
constexpr float kKeyGap              = 6.0f;
constexpr float kTopFrac             = 0.40f;     // keyboard region ends here

// Results grid (smaller than BrowseScreen's 4-col layout — 3 columns
// because the keyboard eats vertical real estate).
constexpr float kGridPaddingTop      = 18.0f;
constexpr float kCellPadding         = 16.0f;
constexpr float kPosterW             = 180.0f;
constexpr float kPosterH             = 270.0f;
constexpr float kLabelAreaH          = 48.0f;
constexpr float kCellW               = kPosterW;
constexpr float kCellH               = kPosterH + kLabelAreaH;
constexpr float kFocusOutlineW       = 3.0f;      // matches DetailScreen poster
constexpr float kPosterBorderW       = 2.0f;      // un-focused poster frame

// Library chip (drawn over the focused-or-not poster's bottom-left).
constexpr float kChipPadX            = 8.0f;
constexpr float kChipH               = 20.0f;
constexpr float kChipBorderW         = 2.0f;
constexpr float kChipMargin          = 8.0f;      // inset from poster edges

// Bottom hint sits this far above the screen bottom (no filled bar).
constexpr float kHintMarginBottom    = 12.0f;

// Backdrop poster tint — same deterministic Knuth hash used by Browse
// and Detail, so a movie's placeholder color stays consistent across
// every screen it appears on.
::ui::Color poster_tint_for_tmdb(int tmdb_id) {
    uint32_t h = static_cast<uint32_t>(tmdb_id) * 2654435761u;
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
}

// Truncate `text` with a trailing ellipsis if it exceeds max_w at
// font_size. Same helper DetailScreen uses; duplicated here rather
// than shared because the file-pair convention in this directory is
// for each screen to own its anonymous-namespace helpers.
std::string truncate_to_width(::ui::Renderer& r, const std::string& text,
                              int font_size, float max_w) {
    if (r.mb_text_width(text, font_size) <= max_w) return text;
    const std::string ellipsis = "...";
    for (size_t n = text.size(); n > 0; --n) {
        std::string candidate = text.substr(0, n) + ellipsis;
        if (r.mb_text_width(candidate, font_size) <= max_w) return candidate;
    }
    return ellipsis;
}

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

void SearchScreen::enter() {
    // Start fresh each time the user opens Search. If they navigated in
    // from Browse, they want an empty query.
    query_.clear();
    last_queried_.clear();
    results_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    selected_tmdb_id_ = 0;
    searching_ = false;
    focus_ = Focus::Keyboard;
    last_input_time_ = std::chrono::steady_clock::time_point::min();

    // Open the virtual keyboard with empty text. We don't use the
    // on_enter/on_cancel callbacks — this screen handles its own flow —
    // but the widget requires the open() call to become active and
    // initialize its selected_row/col.
    keyboard_.open("", "Search Movies",
                   /*on_enter=*/nullptr, /*on_cancel=*/nullptr);

    // Always re-fetch the library on (re-)entry — same reasoning as
    // BrowseScreen::enter(). Without this, a movie removed via Detail
    // still shows up as "in library" in search results and BTN2 add
    // gets blocked with "Already in library." Quality profiles are
    // cached separately because they don't change on adds/removes.
    auto lib = radarr_.get_library();
    library_tmdb_ids_.clear();
    for (const auto& m : lib) {
        if (m.tmdb_id > 0) library_tmdb_ids_.insert(m.tmdb_id);
    }
    if (!library_cached_) {
        quality_profiles_ = radarr_.get_quality_profiles();
        library_cached_ = true;
    }
}

void SearchScreen::quick_add_focused() {
    // Only fires from the results grid; no-op on the keyboard.
    if (focus_ != Focus::Results) return;
    if (results_.empty()) return;
    if (grid_cursor_ < 0 ||
        grid_cursor_ >= static_cast<int>(results_.size())) return;
    const auto& hit = results_[grid_cursor_];
    if (hit.tmdb_id <= 0) return;

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

    bool ok = radarr_.add_movie(hit.tmdb_id, qp, /*monitor=*/true);
    if (!ok) {
        ::ui::Toast::show("Add failed — see Radarr logs");
        return;
    }
    library_tmdb_ids_.insert(hit.tmdb_id);
    std::string msg = "Added: ";
    msg += (hit.title.empty() ? "movie" : hit.title);
    ::ui::Toast::show(msg);
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
        results_.clear();
        grid_cursor_ = 0;
        scroll_row_ = 0;
        searching_ = false;
        return;
    }
    searching_ = true;
    // RadarrClient::lookup is synchronous. At single-user scale this is
    // fine: it blocks the UI for the duration of one Radarr HTTP round
    // trip, which is typically under a second.
    results_ = radarr_.lookup(query_);
    searching_ = false;
    grid_cursor_ = 0;
    scroll_row_ = 0;
}

void SearchScreen::update() {
    run_lookup_if_due();
}

Screen SearchScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // BTN4 / Menu: back to Browse (back stack — NOT all the way out
        // to the kiosk main menu; Browse handles that).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Browse;
        }

        // BTN2 (PLAY_PAUSE): quick-add focused result. No-op on keyboard.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            quick_add_focused();
            continue;
        }

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
    // Top header strip: "SEARCH" + status (searching... / N results)
    // Mirrors DetailScreen's "FEATURE PRESENTATION" idiom: Zen Dots
    // heading in steel-blue (accent2), full-width 2px steel-blue rule
    // beneath. The rule sits at exactly the same Y across screens so
    // transitioning between Browse/Search/Detail feels continuous.
    // ---------------------------------------------------------------
    {
        const std::string heading = "SEARCH";
        r.mb_draw_title_text(heading, kPaddingX, kHeaderBaselineY,
                             th.font_heading_size, th.accent2, 1.0f);

        // Right side: live status. While a Radarr lookup is in flight
        // we show a soft "searching..." in accent (gold) — the in-flight
        // signal is loud enough to notice but small enough not to fight
        // the Zen Dots heading. Once results settle we swap to a dim
        // "N result(s)" so the user gets a confirmation of how many
        // posters are about to appear.
        const int   status_size     = th.font_small_size;
        std::string status_text;
        ::ui::Color status_color    = th.dim;
        float       status_alpha    = 0.85f;
        if (searching_) {
            status_text  = "searching...";
            status_color = th.accent;
            status_alpha = 0.95f;
        } else if (!query_.empty() && !results_.empty()) {
            int n = static_cast<int>(results_.size());
            status_text = std::to_string(n)
                        + (n == 1 ? " result" : " results");
        }
        if (!status_text.empty()) {
            int sw = r.mb_text_width(status_text, status_size);
            float sx = w - kPaddingX - static_cast<float>(sw);
            // Align the status baseline a couple px lower than the
            // heading so it reads as a subtitle, same trick used in
            // DetailScreen's back-hint.
            float sy = kHeaderBaselineY + 2.0f;
            r.mb_draw_text(status_text, sx, sy, status_size,
                           status_color, status_alpha);
        }

        // Full-width 2px steel-blue rule — screen frame, not heading
        // underline. Identical to DetailScreen's header rule.
        r.mb_draw_line(kPaddingX, kHeaderRuleY,
                       w - kPaddingX, kHeaderRuleY,
                       2.0f, th.accent2, 0.95f);
    }

    // ---------------------------------------------------------------
    // Query box: 2px gold outline, no fill. ▶ prefix in steel-blue
    // matches the home-menu's playlist-item arrow vocabulary; the
    // query text itself is in fg (warm cream); a 2px-wide gold caret
    // (rendered as a primitive rect, NOT an underscore glyph) blinks
    // at the end of the text on the same 500ms cadence as every other
    // cursor in the app.
    // ---------------------------------------------------------------
    const float qb_x = kPaddingX;
    const float qb_y = kHeaderRuleY + kQueryBoxMarginTop;
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
        r.mb_draw_text(prefix, prefix_x, text_y, qf, th.accent2, 0.95f);
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
                const ::ui::Color& bg = is_selected ? th.accent : th.action;
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
                   2.0f, th.accent2, 0.85f);

    // ---------------------------------------------------------------
    // Results grid (3 columns).
    // ---------------------------------------------------------------
    const float grid_top    = sep_y + kGridPaddingTop;
    const float grid_bottom = h - kHintMarginBottom
                            - static_cast<float>(th.font_small_size) - 8.0f;
    const float grid_h      = grid_bottom - grid_top;

    const int visible_rows = std::max(1,
        static_cast<int>(grid_h / (kCellH + kCellPadding)));

    if (focus_ == Focus::Results && !results_.empty()) {
        const int focused_row = grid_cursor_ / kGridCols;
        if (focused_row < scroll_row_) scroll_row_ = focused_row;
        if (focused_row >= scroll_row_ + visible_rows) {
            scroll_row_ = focused_row - visible_rows + 1;
        }
    }

    const int total_rows = results_.empty() ? 0
                         : (static_cast<int>(results_.size()) - 1) / kGridCols + 1;
    const int end_row = std::min(total_rows, scroll_row_ + visible_rows);

    const float grid_interior_w = w - 2.0f * kPaddingX;
    float col_gap = (grid_interior_w - kGridCols * kCellW)
                  / std::max(1.0f, static_cast<float>(kGridCols - 1));
    if (col_gap < kCellPadding) col_gap = kCellPadding;

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
        } else if (searching_) {
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
    for (int row = scroll_row_; row < end_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            const int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(results_.size())) break;
            const auto& m = results_[idx];

            const float cell_x = kPaddingX + col * (kCellW + col_gap);
            const float cell_y = grid_top
                               + (row - scroll_row_) * (kCellH + kCellPadding);

            // Poster — fit (preserve aspect) with the deterministic
            // tint placeholder if no artwork is available yet. Same
            // helper Browse/Detail use, so a movie's placeholder
            // color is consistent across screens.
            const ::ui::Color tint = poster_tint_for_tmdb(m.tmdb_id);
            r.mb_draw_poster_fit(m.poster_url,
                                 cell_x, cell_y, kPosterW, kPosterH,
                                 tint, 1.0f);

            const bool focused =
                (focus_ == Focus::Results && idx == grid_cursor_);

            // Frame: 2px gold for un-focused, 3px gold for focused.
            // Same gold-frame "TV monitor" idiom Detail uses on its
            // big poster — just thinner here because the cells are
            // smaller and a heavy outline would dominate.
            const float border_w = focused ? kFocusOutlineW : kPosterBorderW;
            r.mb_stroke_rect(cell_x, cell_y, kPosterW, kPosterH,
                             border_w, th.accent,
                             focused ? 1.0f : 0.85f);

            // Blinking ◂ marker at the bottom-right of the focused
            // poster — same triangle primitive + 500ms cadence as
            // DetailScreen's action-button cursor. Steel-blue color
            // so it doesn't compete with the gold focus outline.
            if (focused && blink_on) {
                const float ms = static_cast<float>(th.font_medium_size) * 0.55f;
                const float mcx = cell_x + kPosterW - 14.0f;
                const float mcy = cell_y + kPosterH - 14.0f;
                r.mb_fill_triangle(
                    mcx,             mcy - ms,
                    mcx,             mcy + ms,
                    mcx - ms * 1.2f, mcy,
                    th.accent2, 1.0f);
            }

            // "IN LIBRARY" chip in the bottom-left of the poster —
            // gold outline + accent text, same pattern as
            // DetailScreen's genre chips. We draw a small bg fill
            // here only because the chip overlays a poster image and
            // would be unreadable against bright artwork otherwise;
            // the bg color is th.bg at high alpha so it still reads
            // as part of the same border-and-text vocabulary.
            if (m.tmdb_id > 0 && library_tmdb_ids_.count(m.tmdb_id) > 0) {
                const std::string chip_text = "IN LIBRARY";
                const int   chip_font = th.font_small_size;
                const int   chip_base = r.mb_text_baseline(chip_font);
                const float chip_text_w = static_cast<float>(
                    r.mb_text_width(chip_text, chip_font));
                const float chip_w = chip_text_w + 2.0f * kChipPadX;
                const float chip_x = cell_x + kChipMargin;
                const float chip_y = cell_y + kPosterH
                                   - kChipH - kChipMargin;

                // Background pad — bg at high alpha so the chip is
                // legible even over a bright poster. NOT a flat color
                // block; it's just a readability backstop behind the
                // outline-and-text idiom.
                r.mb_fill_rect(chip_x, chip_y, chip_w, kChipH,
                               th.bg, 0.85f);
                r.mb_stroke_rect(chip_x, chip_y, chip_w, kChipH,
                                 kChipBorderW, th.accent, 1.0f);
                const float ctx = chip_x + kChipPadX;
                const float cty = chip_y
                                + (kChipH - static_cast<float>(chip_font)) / 2.0f
                                + static_cast<float>(chip_base);
                r.mb_draw_text(chip_text, ctx, cty,
                               chip_font, th.accent, 1.0f);
            }

            // Title + year underneath — body font, fg for the title
            // (full opacity when focused, 0.9 otherwise) and dim for
            // the year. Truncated to cell width.
            const int title_size = th.font_medium_size;
            const int title_base = r.mb_text_baseline(title_size);
            const std::string title = truncate_to_width(
                r, m.title.empty() ? "Untitled" : m.title,
                title_size, kCellW);
            const float title_y = cell_y + kPosterH + 8.0f
                                + static_cast<float>(title_base);
            r.mb_draw_text(title, cell_x, title_y, title_size, th.fg,
                           focused ? 1.0f : 0.9f);

            if (m.year > 0) {
                const std::string year = std::to_string(m.year);
                const int year_size = th.font_small_size;
                const int year_base = r.mb_text_baseline(year_size);
                const float year_y = title_y
                                   + static_cast<float>(title_size) * 0.6f
                                   + static_cast<float>(year_base);
                r.mb_draw_text(year, cell_x, year_y,
                               year_size, th.dim, 0.85f);
            }
        }
    }

    // ---------------------------------------------------------------
    // Bottom hint — centered, dim, font_small. No filled bar; the
    // page is bordered by the steel-blue rules at the top and the
    // section divider, and another fill block here would feel heavy.
    // The hint text changes by focus region so the user always sees
    // the inputs that matter for what's currently active.
    // ---------------------------------------------------------------
    {
        const std::string hint =
            (focus_ == Focus::Keyboard)
                ? "Rotate: keyboard   BTN2: type   Down: results   BTN4: back"
                : "Rotate: nav   RCLICK: open   BTN2: quick-add   BTN4: back";
        const int sz       = th.font_small_size;
        const int baseline = r.mb_text_baseline(sz);
        const int hw       = r.mb_text_width(hint, sz);
        const float hx     = (w - static_cast<float>(hw)) / 2.0f;
        const float hy     = h - kHintMarginBottom
                           - static_cast<float>(sz)
                           + static_cast<float>(baseline);
        r.mb_draw_text(hint, hx, hy, sz, th.dim, 0.85f);
    }
}

}  // namespace media_browser::ui
