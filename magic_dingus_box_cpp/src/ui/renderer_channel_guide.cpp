// ui::Renderer — the Channel Guide: paints a ui::GuideView
// (ui/channel_guide_view.h), which main.cpp lays out when its inputs change
// and hands over with set_channel_guide(); render(state) calls
// render_channel_guide in place of the playlist list (renderer_main_menu.cpp).
//
// Painting only. Every rectangle, label, tone and flag comes from the view;
// this file adds what the view leaves to the painter: the type sizes,
// paddings and vertical rhythm of the design's mockup (layout A), and the
// fitting of the panel's free-text strings to their width with the real
// font measurement (ui/text_fit.h — Zen Dots is proportional). What it
// measures is kept per view revision (Renderer::GuideFitCache), so a frame
// that repaints the same layout measures and builds no strings. Spec:
// docs/superpowers/specs/2026-11-12-channel-guide-design.md, "Screen".
//
// One of several translation units implementing ui::Renderer (split by
// responsibility; renderer.h is the API). Everything here is a batched
// primitive — draw_quad / draw_text / draw_line / draw_textured_quad — with
// no raw GL, so nothing here needs a flush_ui_batch() first.

#include "renderer.h"

#include "channel_guide_view.h"
#include "font_manager.h"
#include "text_fit.h"
#include "text_utf8.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace ui {

namespace guide_paint {

// Per-canvas sizes, from the mockup's layout A with its type moved onto the
// theme's sizes (14 / 18 / 22 / 32; 13 only for the hint row and small
// labels). Font sizes are FontManager pixel heights.
struct Metrics {
    int bug;        // the window's channel bug, Zen Dots
    int pad;        // panel padding (the 3 px gold rule sits inside it)
    int gap;        // between panel rows
    int clock;      // panel clock, Zen Dots
    int num;        // CH box number, Zen Dots
    int box_min_w;  // CH box minimum width
    bool ch_label;  // "CH" caption above the number
    int ptitle;     // playlist title, Zen Dots, two lines at most
    int ptitle_lh;  // its line height
    int ttl;        // item title, mono (faux bold)
    int row_lh;     // a 13 px text row: progress, NEXT
    int bottom_h;   // the panel's bottom row (Picture / PRESS TO WATCH)
    int chan_pad;   // channel cell: left edge of the number / shuffle icon
    int chan_num;   // channel number, Zen Dots
    int cell_pad;   // programme cell text inset
    int hint_top;   // the hints row's top
};

// Modern TV 960x720.
constexpr Metrics kTv{18, 18, 10, 22, 32, 64, true, 18, 23, 18, 16, 30, 14, 18, 10, 660};
// CRT Native 640x480: no CH caption, artist, NEXT row or source label.
constexpr Metrics kCrt{14, 12, 7, 14, 22, 44, false, 14, 17, 14, 14, 24, 10, 14, 6, 446};

// 13 px is for the hint row and the panel's small labels only (eyebrow,
// AM/PM, CH, progress times, NEXT row, PICTURE / what crop does, the source
// label, PRESS TO WATCH, joins at m:ss). Everything in the GRID is at least
// kBody — the spec's anti-goal: no cell text under 14 px on the 720 canvas —
// except the CRT canvas's channel names: 12 px (GuideGrid::name_px, which the
// view truncates for), so ~12 letters fit its 132 px column.
constexpr int kLabel = 13;
constexpr int kBody = 14;  // grid: cell titles and artists, ticks, the date; TV names
constexpr float kHintIcon = 15.0f;

// Fixed labels as strings once, not a temporary per frame.
const std::string kNoSchedule = "NO SCHEDULE YET";
const std::string kCh = "CH";
const std::string kPicture = "PICTURE";
const std::string kCrop = "CROP";
const std::string kFillsScreen = "fills the screen";
const std::string kWholeFrame = "whole frame";
const std::string kPressToWatch = "PRESS TO WATCH";

}  // namespace guide_paint

// One frame's painting of a GuideView: the helpers the sections share and
// the sections themselves, in render_channel_guide's order. A nested class
// of Renderer, so it reaches the private draw_* helpers, fonts and theme.
class Renderer::GuidePainter {
public:
    GuidePainter(Renderer& r, const GuideView& v, float alpha)
        : r_(r),
          v_(v),
          m_(v.crt ? guide_paint::kCrt : guide_paint::kTv),
          t_(*r.theme_),
          body_(*r.body_font_manager_),
          zen_(*r.title_font_manager_),
          alpha_(alpha),
          ix_(static_cast<float>(v.panel_rect.x + m_.pad)),
          ir_(static_cast<float>(v.panel_rect.x + v.panel_rect.w - m_.pad)) {}

    void background_and_window(bool window_dark);
    void panel();
    void grid();
    void hints_and_logo();

private:
    Renderer& r_;
    const GuideView& v_;
    const guide_paint::Metrics& m_;
    const Theme& t_;
    FontManager& body_;
    FontManager& zen_;
    const float alpha_;
    const float ix_, ir_;  // the panel's inner left / right edges

    const GuideFitCache& fitted();

    const Color& tone(GuideTone g) const;
    static Color with_alpha(const Color& c, int a) {
        return Color(c.r, c.g, c.b, static_cast<uint8_t>(a));
    }
    // Baseline of `size` text centred in a `lh`-tall line starting at `top`.
    float baseline_in(float top, float lh, int size, bool zen) const {
        const FontManager& f = zen ? zen_ : body_;
        return top + (lh - static_cast<float>(size)) / 2.0f +
               static_cast<float>(f.get_baseline_at_size(size));
    }
    float body_w(const std::string& s, int size) {
        return static_cast<float>(body_.get_text_width(s, size));
    }
    float zen_w(const std::string& s, int size) {
        return static_cast<float>(zen_.get_text_width(s, size));
    }
    void text(const std::string& s, float x, float b, int size, const Color& c) {
        r_.draw_text(s, x, b, size, c, false, alpha_);
    }
    void zen_text(const std::string& s, float x, float b, int size, const Color& c) {
        r_.draw_text(s, x, b, size, c, true, alpha_);
    }
    // DejaVu Sans Mono has no bold face loaded: the same text again 1 px right.
    void bold(const std::string& s, float x, float b, int size, const Color& c) {
        text(s, x, b, size, c);
        text(s, x + 1.0f, b, size, c);
    }
    // `spacing` px between code points (the mockup's letter-spaced labels):
    // the width, and the label measured once (fitted() only) ...
    float spaced_w(const std::string& s, int size, float spacing);
    GuideSpacedLabel measure_spaced(const std::string& s, int size, float spacing);
    // ... then painted every frame from what was measured: no measuring,
    // no string built. `size` is the size it was measured at.
    void spaced(const GuideSpacedLabel& l, float x, float b, int size, const Color& c) {
        for (std::size_t i = 0; i < l.glyphs.size() && i < l.x.size(); ++i) {
            text(l.glyphs[i], x + l.x[i], b, size, c);
        }
    }
    void fill(float x, float y, float w, float h, const Color& c) {
        if (w > 0.0f && h > 0.0f) r_.draw_quad(x, y, w, h, c, alpha_);
    }
    // A `th` px border drawn INSIDE x, y, w, h.
    void frame(float x, float y, float w, float h, float th, const Color& c) {
        fill(x, y, w, th, c);
        fill(x, y + h - th, w, th, c);
        fill(x, y + th, th, h - 2.0f * th, c);
        fill(x + w - th, y + th, th, h - 2.0f * th, c);
    }
    // A filled triangle `w` x `h`, centred on cx, flat side up when `down`
    // (pointing down), as 1 px rows.
    void triangle(float cx, float top, float w, float h, bool down, const Color& c) {
        const int rows = std::max(1, static_cast<int>(std::lround(h)));
        for (int i = 0; i < rows; ++i) {
            const float k = down ? static_cast<float>(rows - i) / rows
                                 : static_cast<float>(i + 1) / rows;
            fill(cx - w * k / 2.0f, top + i, w * k, 1.0f, c);
        }
    }
};

const Color& Renderer::GuidePainter::tone(GuideTone g) const {
    switch (g) {
        case GuideTone::Dim: return t_.dim;
        case GuideTone::Gold: return t_.accent;
        case GuideTone::Green: return t_.highlight1;
        case GuideTone::Steel: return t_.accent2;
        case GuideTone::Red: return t_.highlight2;
        case GuideTone::Fg: break;
    }
    return t_.fg;
}

float Renderer::GuidePainter::spaced_w(const std::string& s, int size, float spacing) {
    float w = 0.0f;
    bool first = true;
    std::size_t pos = 0;
    while (pos < s.size()) {
        const std::size_t start = pos;
        if (decode_utf8(s, pos) == 0) break;
        if (!first) w += spacing;
        first = false;
        w += body_w(s.substr(start, pos - start), size);
    }
    return w;
}

Renderer::GuideSpacedLabel Renderer::GuidePainter::measure_spaced(const std::string& s,
                                                                  int size, float spacing) {
    GuideSpacedLabel l;
    float x = 0.0f;
    bool first = true;
    std::size_t pos = 0;
    while (pos < s.size()) {
        const std::size_t start = pos;
        if (decode_utf8(s, pos) == 0) break;
        if (!first) x += spacing;
        first = false;
        l.glyphs.push_back(s.substr(start, pos - start));
        l.x.push_back(x);
        x += body_w(l.glyphs.back(), size);
    }
    l.w = x;
    return l;
}

// Everything the sections measure with the fonts, once per layout.
const Renderer::GuideFitCache& Renderer::GuidePainter::fitted() {
    namespace gp = guide_paint;
    GuideFitCache& f = r_.guide_fit_;
    if (f.valid && f.revision == v_.revision) return f;
    f.valid = true;
    f.revision = v_.revision;
    const GuidePanel& p = v_.panel;
    auto body_measure = [this](int size) {
        return [this, size](const std::string& s) { return static_cast<int>(body_w(s, size)); };
    };

    f.eyebrow = measure_spaced(p.eyebrow, gp::kLabel, 2.0f);
    f.ch_caption = measure_spaced(gp::kCh, gp::kLabel, 3.0f);
    f.ap_w = body_w(p.clock_ap, gp::kLabel);
    f.hm_w = zen_w(p.clock_hm, m_.clock);
    f.num_w = zen_w(p.ch_text, m_.num);
    f.box_w = std::max(static_cast<float>(m_.box_min_w), f.num_w + 16.0f);
    f.title_x = ix_ + f.box_w + 14.0f;
    f.title_lines = wrap_to_lines(
        p.playlist_title, static_cast<int>(ir_ - f.title_x), 2,
        [this](const std::string& s) { return static_cast<int>(zen_w(s, m_.ptitle)); });
    f.item_title = fit_to_width(p.item_title, static_cast<int>(ir_ - ix_ - 1.0f),
                                body_measure(m_.ttl));  // -1: the faux bold
    f.artist = fit_to_width(p.artist, static_cast<int>(ir_ - ix_), body_measure(gp::kBody));
    f.start_w = body_w(p.start_hm, gp::kLabel);
    f.end_w = body_w(p.end_hm, gp::kLabel);
    f.next_label = "NEXT " + p.next_hm;
    f.next_spaced = measure_spaced(f.next_label, gp::kLabel, 2.0f);
    f.next_title_x = ix_ + f.next_spaced.w + 10.0f;
    f.next_title = fit_to_width(p.next_title, static_cast<int>(ir_ - f.next_title_x),
                                body_measure(gp::kLabel));
    // The Picture control: PICTURE · box · CROP · what crop does.
    f.picture = measure_spaced(gp::kPicture, gp::kLabel, 2.0f);
    f.crop = measure_spaced(gp::kCrop, gp::kBody, 1.0f);
    f.pic_box_x = ix_ + f.picture.w + 10.0f;
    f.pic_crop_x = f.pic_box_x + 16.0f + 10.0f;
    f.pic_desc_x = f.pic_crop_x + f.crop.w + 10.0f;
    f.pic_end = f.pic_desc_x +
                body_w(p.picture_crop ? gp::kFillsScreen : gp::kWholeFrame, gp::kLabel);
    f.source = measure_spaced(p.source_label, gp::kLabel, 1.0f);
    f.source_w = f.source.w;
    f.press_to_watch = measure_spaced(gp::kPressToWatch, gp::kLabel, 2.0f);
    f.join_w = body_w(p.join_note, gp::kLabel);

    // The date: grid text, so the body size (no grid text under 14 px on
    // the 720 canvas — the spec's anti-goal).
    const GuideGrid& g = v_.grid;
    f.date = fit_to_width(
        g.date_label, static_cast<int>(g.column_w - m_.chan_pad - 4),
        [this](const std::string& s) { return static_cast<int>(spaced_w(s, gp::kBody, 2.0f)); });
    f.date_spaced = measure_spaced(f.date, gp::kBody, 2.0f);

    f.hint_key_x.clear();
    f.hint_label_x.clear();
    f.hint_keys.clear();
    float x = static_cast<float>(g.rect.x);
    for (const GuideHint& h : v_.hints) {
        x += gp::kHintIcon + 7.0f;
        f.hint_key_x.push_back(x);
        f.hint_keys.push_back(measure_spaced(h.key, gp::kLabel, 1.0f));
        x += f.hint_keys.back().w + 7.0f;
        f.hint_label_x.push_back(x);
        x += body_w(h.label, gp::kLabel) + 24.0f;
    }
    return f;
}

// ── Background and the video window ──────────────────────────────────────
void Renderer::GuidePainter::background_and_window(bool window_dark) {
    // bg over the whole canvas except the video window (no 50 % overlay:
    // the video lives in the window, not under the guide).
    const float W = static_cast<float>(r_.width_);
    const float H = static_cast<float>(r_.height_);
    const GuideRect& win = v_.window;
    const float wx = static_cast<float>(win.x), wy = static_cast<float>(win.y);
    const float ww = static_cast<float>(win.w), wh = static_cast<float>(win.h);
    fill(0.0f, 0.0f, W, wy, t_.bg);
    fill(0.0f, wy + wh, W, H - (wy + wh), t_.bg);
    fill(0.0f, wy, wx, wh, t_.bg);
    fill(wx + ww, wy, W - (wx + ww), wh, t_.bg);

    // No video in it: video black — deliberately not a theme token. The
    // window stands in for the picture, and an empty picture is the
    // framebuffer's black clear (main.cpp), not the guide's bg.
    if (window_dark) fill(wx, wy, ww, wh, Color(0, 0, 0));
    // A 1 px fg hairline at 18 % inside it, the channel bug (Zen Dots, 70 %
    // fg) top-left.
    frame(wx, wy, ww, wh, 1.0f, with_alpha(t_.fg, 46));
    if (!v_.window_bug.empty()) {
        zen_text(v_.window_bug, wx + 12.0f,
                 wy + 9.0f + static_cast<float>(zen_.get_baseline_at_size(m_.bug)), m_.bug,
                 with_alpha(t_.fg, 178));
    }
}

// ── Now-playing panel: bg_lift, 3 px gold rule on top ───────────────────
void Renderer::GuidePainter::panel() {
    namespace gp = guide_paint;
    const GuidePanel& p = v_.panel;
    const GuideFitCache& f = fitted();
    const GuideRect& pr = v_.panel_rect;
    const float px = static_cast<float>(pr.x), py = static_cast<float>(pr.y);
    const float pw = static_cast<float>(pr.w), ph = static_cast<float>(pr.h);
    fill(px, py, pw, ph, t_.bg_lift);
    fill(px, py, pw, 3.0f, t_.accent);
    const float iw = ir_ - ix_;
    float y = py + m_.pad;

    // Eyebrow (left) and clock with AM/PM (right), one baseline.
    {
        const float lh = std::round(m_.clock * 1.2f);
        const float b = baseline_in(y, lh, m_.clock, true);
        spaced(f.eyebrow, ix_, b, gp::kLabel, tone(p.eyebrow_tone));
        text(p.clock_ap, ir_ - f.ap_w, b, gp::kLabel, t_.dim);
        zen_text(p.clock_hm, ir_ - f.ap_w - 5.0f - f.hm_w, b, m_.clock, t_.fg);
        y += lh + m_.gap;
    }

    // CH box (gold Zen Dots number in a gold hairline box, "CH" caption on
    // TV) and the playlist title beside it, two lines at most, both centred
    // on the row.
    {
        const float cap_lh = m_.ch_label ? 14.0f : 0.0f;
        const float num_lh = std::round(m_.num * 1.05f);
        const float box_h = 3.0f + cap_lh + num_lh + 5.0f;
        const float block_h = static_cast<float>(f.title_lines.size()) * m_.ptitle_lh;
        const float row_h = std::max(box_h, block_h);
        const float box_y = y + (row_h - box_h) / 2.0f;
        fill(ix_, box_y, f.box_w, box_h, t_.bg);
        frame(ix_, box_y, f.box_w, box_h, 1.0f, with_alpha(t_.accent, 115));
        if (m_.ch_label) {
            spaced(f.ch_caption, ix_ + (f.box_w - f.ch_caption.w) / 2.0f,
                   baseline_in(box_y + 3.0f, cap_lh, gp::kLabel, false), gp::kLabel, t_.dim);
        }
        zen_text(p.ch_text, ix_ + (f.box_w - f.num_w) / 2.0f,
                 baseline_in(box_y + 3.0f + cap_lh, num_lh, m_.num, true), m_.num, t_.accent);
        float ly = y + (row_h - block_h) / 2.0f;
        for (const std::string& line : f.title_lines) {
            zen_text(line, f.title_x,
                     baseline_in(ly, static_cast<float>(m_.ptitle_lh), m_.ptitle, true),
                     m_.ptitle, t_.fg);
            ly += m_.ptitle_lh;
        }
        y += row_h + m_.gap;
    }

    // Rule.
    fill(ix_, y, iw, 1.0f, with_alpha(t_.fg, 31));
    y += 1.0f + m_.gap;

    // Item title (bold) and artist (dim, TV) — or the channel has nothing on.
    const bool on_air = !p.start_hm.empty();
    {
        const float lh = std::round(m_.ttl * 1.2f);
        const float b = baseline_in(y, lh, m_.ttl, false);
        if (on_air) {
            bold(f.item_title, ix_, b, m_.ttl, t_.fg);
        } else {
            text(gp::kNoSchedule, ix_, b, m_.ttl, t_.dim);
        }
        y += lh;
        if (on_air && p.show_artist) {
            y += 4.0f;
            const float alh = std::round(gp::kBody * 1.2f);
            text(f.artist, ix_, baseline_in(y, alh, gp::kBody, false), gp::kBody, t_.dim);
            y += alh;
        }
        y += m_.gap;
    }

    if (on_air) {
        // Start time, gold progress bar on a bg trough, end time.
        const float b = baseline_in(y, static_cast<float>(m_.row_lh), gp::kLabel, false);
        text(p.start_hm, ix_, b, gp::kLabel, t_.dim);
        text(p.end_hm, ir_ - f.end_w, b, gp::kLabel, t_.dim);
        const float bar_x = ix_ + f.start_w + 10.0f;
        const float bar_w = (ir_ - f.end_w - 10.0f) - bar_x;
        const float bar_y = y + (m_.row_lh - 6.0f) / 2.0f;
        fill(bar_x, bar_y, bar_w, 6.0f, t_.bg);
        fill(bar_x, bar_y, bar_w * static_cast<float>(std::clamp(p.progress, 0.0, 1.0)), 6.0f,
             t_.accent);
        y += m_.row_lh + m_.gap;

        // NEXT (steel) + time, then the next title (TV).
        if (p.show_next) {
            const float nb = baseline_in(y, static_cast<float>(m_.row_lh), gp::kLabel, false);
            spaced(f.next_spaced, ix_, nb, gp::kLabel, t_.accent2);
            text(f.next_title, f.next_title_x, nb, gp::kLabel, t_.fg);
            y += m_.row_lh + m_.gap;
        }
    }

    // Bottom row, pinned to the panel's bottom: the Picture control (+ the
    // source label on TV) on the tuned channel, PRESS TO WATCH + the join
    // note on another, nothing otherwise (GuideBottom::None).
    const float top = std::max(y, py + ph - m_.pad - m_.bottom_h);
    const float cy = top + m_.bottom_h / 2.0f;
    auto b_at = [&](int size) {
        return cy - size / 2.0f + static_cast<float>(body_.get_baseline_at_size(size));
    };
    if (p.bottom == GuideBottom::Picture) {
        const Color& ink = p.picture_focus ? t_.accent : t_.fg;
        spaced(f.picture, ix_, b_at(gp::kLabel), gp::kLabel, t_.dim);
        const float box = 16.0f;
        const float bx = f.pic_box_x;
        const float by = cy - box / 2.0f;
        if (p.picture_crop) {
            fill(bx, by, box, box, ink);
            // The tick, knocked out in bg (the mockup's 3.5,8.4 → 6.6,11.4 → 12.6,4.8).
            r_.draw_line(bx + 3.5f, by + 8.4f, bx + 6.6f, by + 11.4f, 2.2f, t_.bg, alpha_);
            r_.draw_line(bx + 6.6f, by + 11.4f, bx + 12.6f, by + 4.8f, 2.2f, t_.bg, alpha_);
        } else {
            frame(bx, by, box, box, 2.0f, ink);
        }
        spaced(f.crop, f.pic_crop_x, b_at(gp::kBody), gp::kBody, ink);
        text(p.picture_crop ? gp::kFillsScreen : gp::kWholeFrame, f.pic_desc_x, b_at(gp::kLabel),
             gp::kLabel, t_.dim);
        if (p.picture_focus) {
            frame(ix_ - 6.0f, cy - 12.0f, (f.pic_end + 8.0f) - (ix_ - 6.0f), 24.0f, 2.0f,
                  t_.accent);
        }
        if (p.show_source) {
            spaced(f.source, ir_ - f.source_w, b_at(gp::kLabel), gp::kLabel, t_.dim);
        }
    } else if (p.bottom == GuideBottom::PressToWatch) {
        spaced(f.press_to_watch, ix_, b_at(gp::kLabel), gp::kLabel, t_.accent);
        if (!p.join_note.empty()) {
            text(p.join_note, ir_ - f.join_w, b_at(gp::kLabel), gp::kLabel, t_.dim);
        }
    }
}

// ── Grid ─────────────────────────────────────────────────────────────────
// view.grid's x / y are relative to grid.rect's top-left; row y counts the
// header. Rows and cells are row_h - 3 tall: the 3 px gaps show the
// background as grid lines.
void Renderer::GuidePainter::grid() {
    namespace gp = guide_paint;
    const GuideFitCache& f = fitted();
    const GuideGrid& g = v_.grid;
    const float gx = static_cast<float>(g.rect.x), gy = static_cast<float>(g.rect.y);
    const float rh = static_cast<float>(g.row_h - 3);
    const float col_w = static_cast<float>(g.column_w);

    // Header: the date (dim, in the channel column), the time ticks (steel,
    // a 1 x 12 px mark then the label; the first carries AM/PM).
    spaced(f.date_spaced, gx + m_.chan_pad,
           baseline_in(gy, static_cast<float>(g.header_h), gp::kBody, false), gp::kBody, t_.dim);
    const float tb = baseline_in(gy, static_cast<float>(g.header_h), gp::kBody, false);
    for (const GuideTick& tick : g.ticks) {
        const float tx = gx + tick.x;
        fill(tx, gy + (g.header_h - 12.0f) / 2.0f, 1.0f, 12.0f, t_.accent2);
        text(tick.label, tx + 7.0f, tb, gp::kBody, t_.accent2);
    }

    for (const GuideRow& row : g.rows) {
        const float ry = gy + row.y;
        // Channel cell: number (Zen Dots) or the shuffle icon for 00, then
        // the short name at name_x; a 4 px gold bar on the cursor's row.
        fill(gx, ry, col_w, rh, t_.bg_lift);
        if (row.cursor_bar) fill(gx, ry, 4.0f, rh, t_.accent);
        const Color& num_c = tone(row.num_tone);
        if (row.shuffle) {
            // The mockup's 18 x 12 icon: two crossing strokes, two arrowheads.
            const float x0 = gx + m_.chan_pad;
            const float top = ry + (rh - 12.0f) / 2.0f;
            r_.draw_line(x0 + 1.0f, top + 10.0f, x0 + 11.0f, top + 2.0f, 1.8f, num_c, alpha_);
            r_.draw_line(x0 + 1.0f, top + 2.0f, x0 + 11.0f, top + 10.0f, 1.8f, num_c, alpha_);
            for (int i = 0; i < 4; ++i) {  // 4 px wide, 4 px tall at the base
                const float hh = 4.0f * static_cast<float>(4 - i) / 4.0f;
                fill(x0 + 11.0f + i, top + 2.0f - hh / 2.0f, 1.0f, hh, num_c);
                fill(x0 + 11.0f + i, top + 10.0f - hh / 2.0f, 1.0f, hh, num_c);
            }
        } else {
            zen_text(row.number_text, gx + m_.chan_pad, baseline_in(ry, rh, m_.chan_num, true),
                     m_.chan_num, num_c);
        }
        // At the view's name size (14 TV, 12 CRT): the size its truncation assumed.
        text(row.name, gx + g.name_x, baseline_in(ry, rh, g.name_px, false), g.name_px,
             tone(row.name_tone));

        const float cells_x = gx + col_w + 3.0f;
        if (row.no_schedule) {
            fill(cells_x, ry, gx + g.rect.w - cells_x, rh, t_.bg_lift);
            text(gp::kNoSchedule, cells_x + m_.cell_pad, baseline_in(ry, rh, gp::kBody, false),
                 gp::kBody, t_.dim);
            continue;
        }
        for (const GuideCell& c : row.cells) {
            const float cx = gx + c.x;
            const float cw = static_cast<float>(c.w);
            fill(cx, ry, cw, rh, t_.bg_lift);
            if (c.ring) frame(cx, ry, cw, rh, 2.0f, t_.accent);
            if (!c.show_text) continue;
            const Color& ink = tone(c.tone);
            const float tx = cx + m_.cell_pad;
            float b1 = baseline_in(ry, rh, gp::kBody, false);
            if (c.show_artist && !c.artist.empty()) {
                // Title over artist, the pair centred in the cell. Both at the
                // body size: no cell text under 14 px on the 720 canvas (the
                // spec's anti-goal) — 17 + 3 + 17 fits the 49 px row.
                const float lh = std::round(gp::kBody * 1.2f);
                const float top = ry + (rh - (lh + 3.0f + lh)) / 2.0f;
                b1 = baseline_in(top, lh, gp::kBody, false);
                text(c.artist, tx, baseline_in(top + lh + 3.0f, lh, gp::kBody, false),
                     gp::kBody, t_.dim);
            }
            if (c.bold) bold(c.label, tx, b1, gp::kBody, ink);
            else text(c.label, tx, b1, gp::kBody, ink);
        }
    }

    // Now line: 2 px red down every row, a 10 x 6 marker in the header.
    if (!g.rows.empty()) {
        const float nx = gx + g.now_x;
        const float rows_h = static_cast<float>(g.rows.size()) * g.row_h - 3.0f;
        fill(nx, gy + g.header_h, 2.0f, rows_h, t_.highlight2);
        triangle(nx + 1.0f, gy + g.header_h - 7.0f, 10.0f, 6.0f, true, t_.highlight2);
    }

    // Scroll arrows, as the list draws them (steel, 12 x 7): above and below
    // the grid, centred on the channel column.
    const float acx = gx + col_w / 2.0f;
    if (g.up_arrow) triangle(acx, gy - 10.0f, 12.0f, 7.0f, false, t_.accent2);
    if (g.down_arrow) {
        triangle(acx, gy + static_cast<float>(g.rect.h) + 1.0f, 12.0f, 7.0f, true, t_.accent2);
    }
}

// ── Hints row (knob icon, key, label) and the logo at its right end ──────
void Renderer::GuidePainter::hints_and_logo() {
    namespace gp = guide_paint;
    const GuideFitCache& f = fitted();
    const float cy = m_.hint_top + gp::kHintIcon / 2.0f;
    const float b =
        cy - gp::kLabel / 2.0f + static_cast<float>(body_.get_baseline_at_size(gp::kLabel));
    for (std::size_t i = 0;
         i < v_.hints.size() && i < f.hint_key_x.size() && i < f.hint_keys.size(); ++i) {
        const GuideHint& h = v_.hints[i];
        // The knob: a 6 px ring (1.4 px stroke) with its pointer at 12.
        const float kx = f.hint_key_x[i] - 7.0f - gp::kHintIcon / 2.0f;
        constexpr float kR = 6.0f;
        constexpr int kSegs = 20;
        for (int s = 0; s < kSegs; ++s) {
            const float a0 = 6.2831853f * s / kSegs;
            const float a1 = 6.2831853f * (s + 1) / kSegs;
            r_.draw_line(kx + kR * std::cos(a0), cy + kR * std::sin(a0), kx + kR * std::cos(a1),
                         cy + kR * std::sin(a1), 1.4f, t_.dim, alpha_);
        }
        fill(kx - 0.7f, cy - kR + 1.0f, 1.4f, 3.75f, t_.dim);
        spaced(f.hint_keys[i], f.hint_key_x[i], b, gp::kLabel, t_.fg);
        text(h.label, f.hint_label_x[i], b, gp::kLabel, t_.dim);
    }
    if (r_.logo_texture_id_ != 0 && r_.logo_width_ > 0 && r_.logo_height_ > 0) {
        // Fitted inside the view's logo rect, right-aligned, its aspect kept.
        const GuideRect& lr = v_.logo;
        const float s = std::min(static_cast<float>(lr.w) / r_.logo_width_,
                                 static_cast<float>(lr.h) / r_.logo_height_);
        const float lw = r_.logo_width_ * s;
        const float lh = r_.logo_height_ * s;
        r_.draw_textured_quad(r_.logo_texture_id_, lr.x + lr.w - lw, lr.y + (lr.h - lh) / 2.0f, lw,
                              lh, alpha_);
    }
}

void Renderer::render_channel_guide(const GuideView& view, float alpha, bool window_dark) {
    GuidePainter painter(*this, view, alpha);
    painter.background_and_window(window_dark);
    painter.panel();
    painter.grid();
    painter.hints_and_logo();
}

}  // namespace ui
