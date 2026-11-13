// ui::Renderer — the Channel Guide: paints a ui::GuideView
// (ui/channel_guide_view.h), which main.cpp lays out for the frame and hands
// over with set_channel_guide(); render(state) calls render_channel_guide in
// place of the playlist list (renderer_main_menu.cpp).
//
// Painting only. Every rectangle, label, tone and flag comes from the view;
// this file adds what the view leaves to the painter: the type sizes,
// paddings and vertical rhythm of the design's mockup (layout A), and the
// fitting of the panel's free-text strings to their width with the real
// font measurement (ui/text_fit.h — Zen Dots is proportional). Spec:
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

namespace {

// Per-canvas sizes, from the mockup's layout A with its type moved onto the
// theme's sizes (14 / 18 / 22 / 32; 13 only for the hint row and small
// labels). Font sizes are FontManager pixel heights.
struct GuideMetrics {
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
constexpr GuideMetrics kTv{18, 18, 10, 22, 32, 64, true, 18, 23, 18, 16, 30, 14, 18, 10, 660};
// CRT Native 640x480: no CH caption, artist, NEXT row or source label.
constexpr GuideMetrics kCrt{14, 12, 7, 14, 22, 44, false, 14, 17, 14, 14, 24, 10, 14, 6, 446};

constexpr int kLabel = 13;  // hint row and small labels
constexpr int kBody = 14;   // cell titles, channel names, ticks, artist

const char* const kSourceLabel = "16:9 SOURCE";
const char* const kNoSchedule = "NO SCHEDULE YET";

}  // namespace

void Renderer::render_channel_guide(const GuideView& view, float alpha, bool window_dark) {
    const GuideMetrics& m = view.crt ? kCrt : kTv;
    const Theme& t = *theme_;
    FontManager& body = *body_font_manager_;
    FontManager& title = *title_font_manager_;

    // ── Small drawing helpers (batched primitives, the overlay's alpha) ──
    auto tone = [&t](GuideTone g) -> const Color& {
        switch (g) {
            case GuideTone::Dim: return t.dim;
            case GuideTone::Gold: return t.accent;
            case GuideTone::Green: return t.highlight1;
            case GuideTone::Steel: return t.accent2;
            case GuideTone::Red: return t.highlight2;
            case GuideTone::Fg: break;
        }
        return t.fg;
    };
    auto with_alpha = [](const Color& c, int a) { return Color(c.r, c.g, c.b, static_cast<uint8_t>(a)); };
    // Baseline of `size` text centred in a `lh`-tall line starting at `top`.
    auto baseline_in = [&](float top, float lh, int size, bool zen) {
        const FontManager& f = zen ? title : body;
        return top + (lh - static_cast<float>(size)) / 2.0f +
               static_cast<float>(f.get_baseline_at_size(size));
    };
    auto body_w = [&body](const std::string& s, int size) {
        return static_cast<float>(body.get_text_width(s, size));
    };
    auto zen_w = [&title](const std::string& s, int size) {
        return static_cast<float>(title.get_text_width(s, size));
    };
    // Returns the right edge.
    auto text = [&](const std::string& s, float x, float b, int size, const Color& c) {
        draw_text(s, x, b, size, c, false, alpha);
        return x + body_w(s, size);
    };
    auto zen = [&](const std::string& s, float x, float b, int size, const Color& c) {
        draw_text(s, x, b, size, c, true, alpha);
        return x + zen_w(s, size);
    };
    // DejaVu Sans Mono has no bold face loaded: the same text again 1 px right.
    auto bold = [&](const std::string& s, float x, float b, int size, const Color& c) {
        draw_text(s, x, b, size, c, false, alpha);
        draw_text(s, x + 1.0f, b, size, c, false, alpha);
    };
    // `spacing` px between code points (the mockup's letter-spaced labels).
    auto spaced_w = [&](const std::string& s, int size, float spacing) {
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
    };
    auto spaced = [&](const std::string& s, float x, float b, int size, const Color& c,
                      float spacing) {
        bool first = true;
        std::size_t pos = 0;
        while (pos < s.size()) {
            const std::size_t start = pos;
            if (decode_utf8(s, pos) == 0) break;
            const std::string cp = s.substr(start, pos - start);
            if (!first) x += spacing;
            first = false;
            draw_text(cp, x, b, size, c, false, alpha);
            x += body_w(cp, size);
        }
        return x;
    };
    auto fill = [&](float x, float y, float w, float h, const Color& c) {
        if (w > 0.0f && h > 0.0f) draw_quad(x, y, w, h, c, alpha);
    };
    // A `th` px border drawn INSIDE x, y, w, h.
    auto frame = [&](float x, float y, float w, float h, float th, const Color& c) {
        fill(x, y, w, th, c);
        fill(x, y + h - th, w, th, c);
        fill(x, y + th, th, h - 2.0f * th, c);
        fill(x + w - th, y + th, th, h - 2.0f * th, c);
    };
    // A filled triangle `w` x `h`, centred on cx, flat side up when `down`
    // (pointing down), as 1 px rows.
    auto triangle = [&](float cx, float top, float w, float h, bool down, const Color& c) {
        const int rows = std::max(1, static_cast<int>(std::lround(h)));
        for (int i = 0; i < rows; ++i) {
            const float k = down ? static_cast<float>(rows - i) / rows
                                 : static_cast<float>(i + 1) / rows;
            fill(cx - w * k / 2.0f, top + i, w * k, 1.0f, c);
        }
    };

    const float W = static_cast<float>(width_);
    const float H = static_cast<float>(height_);

    // ── Background: bg over the whole canvas except the video window ─────
    // (no 50 % overlay: the video lives in the window, not under the guide).
    const GuideRect& win = view.window;
    const float wx = static_cast<float>(win.x), wy = static_cast<float>(win.y);
    const float ww = static_cast<float>(win.w), wh = static_cast<float>(win.h);
    fill(0.0f, 0.0f, W, wy, t.bg);
    fill(0.0f, wy + wh, W, H - (wy + wh), t.bg);
    fill(0.0f, wy, wx, wh, t.bg);
    fill(wx + ww, wy, W - (wx + ww), wh, t.bg);

    // ── Video window: dark with no video, a 1 px fg hairline at 18 %, the
    // channel bug (Zen Dots, 70 % fg) top-left inside it ────────────────────
    if (window_dark) fill(wx, wy, ww, wh, Color(0, 0, 0));
    frame(wx, wy, ww, wh, 1.0f, with_alpha(t.fg, 46));
    if (!view.window_bug.empty()) {
        zen(view.window_bug, wx + 12.0f,
            wy + 9.0f + static_cast<float>(title.get_baseline_at_size(m.bug)), m.bug,
            with_alpha(t.fg, 178));
    }

    // ── Now-playing panel: bg_lift, 3 px gold rule on top ────────────────
    const GuidePanel& p = view.panel;
    const GuideRect& pr = view.panel_rect;
    const float px = static_cast<float>(pr.x), py = static_cast<float>(pr.y);
    const float pw = static_cast<float>(pr.w), ph = static_cast<float>(pr.h);
    fill(px, py, pw, ph, t.bg_lift);
    fill(px, py, pw, 3.0f, t.accent);
    const float ix = px + m.pad;
    const float ir = px + pw - m.pad;
    const float iw = ir - ix;
    float y = py + m.pad;

    // Eyebrow (left) and clock with AM/PM (right), one baseline.
    {
        const float lh = std::round(m.clock * 1.2f);
        const float b = baseline_in(y, lh, m.clock, true);
        spaced(p.eyebrow, ix, b, kLabel, tone(p.eyebrow_tone), 2.0f);
        const float ap_w = body_w(p.clock_ap, kLabel);
        text(p.clock_ap, ir - ap_w, b, kLabel, t.dim);
        zen(p.clock_hm, ir - ap_w - 5.0f - zen_w(p.clock_hm, m.clock), b, m.clock, t.fg);
        y += lh + m.gap;
    }

    // CH box (gold Zen Dots number in a gold hairline box, "CH" caption on
    // TV) and the playlist title beside it, two lines at most, both centred
    // on the row.
    {
        const float cap_lh = m.ch_label ? 14.0f : 0.0f;
        const float num_lh = std::round(m.num * 1.05f);
        const float box_h = 3.0f + cap_lh + num_lh + 5.0f;
        const float num_w = zen_w(p.ch_text, m.num);
        const float box_w = std::max(static_cast<float>(m.box_min_w), num_w + 16.0f);
        const float tx = ix + box_w + 14.0f;
        const auto lines = wrap_to_lines(
            p.playlist_title, static_cast<int>(ir - tx), 2,
            [&](const std::string& s) { return static_cast<int>(zen_w(s, m.ptitle)); });
        const float block_h = static_cast<float>(lines.size()) * m.ptitle_lh;
        const float row_h = std::max(box_h, block_h);
        const float box_y = y + (row_h - box_h) / 2.0f;
        fill(ix, box_y, box_w, box_h, t.bg);
        frame(ix, box_y, box_w, box_h, 1.0f, with_alpha(t.accent, 115));
        if (m.ch_label) {
            const float cw = spaced_w("CH", kLabel, 3.0f);
            spaced("CH", ix + (box_w - cw) / 2.0f, baseline_in(box_y + 3.0f, cap_lh, kLabel, false),
                   kLabel, t.dim, 3.0f);
        }
        zen(p.ch_text, ix + (box_w - num_w) / 2.0f,
            baseline_in(box_y + 3.0f + cap_lh, num_lh, m.num, true), m.num, t.accent);
        float ly = y + (row_h - block_h) / 2.0f;
        for (const std::string& line : lines) {
            zen(line, tx, baseline_in(ly, static_cast<float>(m.ptitle_lh), m.ptitle, true),
                m.ptitle, t.fg);
            ly += m.ptitle_lh;
        }
        y += row_h + m.gap;
    }

    // Rule.
    fill(ix, y, iw, 1.0f, with_alpha(t.fg, 31));
    y += 1.0f + m.gap;

    const bool on_air = !p.start_hm.empty();
    auto fit_body = [&](const std::string& s, float max_w, int size) {
        return fit_to_width(s, static_cast<int>(max_w),
                            [&](const std::string& c) { return static_cast<int>(body_w(c, size)); });
    };

    // Item title (bold) and artist (dim, TV) — or the channel has nothing on.
    {
        const float lh = std::round(m.ttl * 1.2f);
        const float b = baseline_in(y, lh, m.ttl, false);
        if (on_air) {
            bold(fit_body(p.item_title, iw - 1.0f, m.ttl), ix, b, m.ttl, t.fg);
        } else {
            text(kNoSchedule, ix, b, m.ttl, t.dim);
        }
        y += lh;
        if (on_air && p.show_artist) {
            y += 4.0f;
            const float alh = std::round(kBody * 1.2f);
            text(fit_body(p.artist, iw, kBody), ix, baseline_in(y, alh, kBody, false), kBody,
                 t.dim);
            y += alh;
        }
        y += m.gap;
    }

    if (on_air) {
        // Start time, gold progress bar on a bg trough, end time.
        const float b = baseline_in(y, static_cast<float>(m.row_lh), kLabel, false);
        const float sx = text(p.start_hm, ix, b, kLabel, t.dim);
        const float ew = body_w(p.end_hm, kLabel);
        text(p.end_hm, ir - ew, b, kLabel, t.dim);
        const float bar_x = sx + 10.0f;
        const float bar_w = (ir - ew - 10.0f) - bar_x;
        const float bar_y = y + (m.row_lh - 6.0f) / 2.0f;
        fill(bar_x, bar_y, bar_w, 6.0f, t.bg);
        fill(bar_x, bar_y,
             bar_w * static_cast<float>(std::clamp(p.progress, 0.0, 1.0)), 6.0f, t.accent);
        y += m.row_lh + m.gap;

        // NEXT (steel) + time, then the next title (TV).
        if (p.show_next) {
            const float nb = baseline_in(y, static_cast<float>(m.row_lh), kLabel, false);
            const float nx =
                spaced("NEXT " + p.next_hm, ix, nb, kLabel, t.accent2, 2.0f) + 10.0f;
            text(fit_body(p.next_title, ir - nx, kLabel), nx, nb, kLabel, t.fg);
            y += m.row_lh + m.gap;
        }
    }

    // Bottom row, pinned to the panel's bottom: the Picture control (+ the
    // source label on TV) on the tuned channel, PRESS TO WATCH + the join
    // note on another, nothing otherwise (GuideBottom::None).
    {
        const float top = std::max(y, py + ph - m.pad - m.bottom_h);
        const float cy = top + m.bottom_h / 2.0f;
        auto b_at = [&](int size) {
            return cy - size / 2.0f + static_cast<float>(body.get_baseline_at_size(size));
        };
        if (p.bottom == GuideBottom::Picture) {
            const Color& ink = p.picture_focus ? t.accent : t.fg;
            float x = spaced("PICTURE", ix, b_at(kLabel), kLabel, t.dim, 2.0f) + 10.0f;
            const float box = 16.0f;
            const float by = cy - box / 2.0f;
            if (p.picture_crop) {
                fill(x, by, box, box, ink);
                // The tick, knocked out in bg (the mockup's 3.5,8.4 → 6.6,11.4 → 12.6,4.8).
                draw_line(x + 3.5f, by + 8.4f, x + 6.6f, by + 11.4f, 2.2f, t.bg, alpha);
                draw_line(x + 6.6f, by + 11.4f, x + 12.6f, by + 4.8f, 2.2f, t.bg, alpha);
            } else {
                frame(x, by, box, box, 2.0f, ink);
            }
            x += box + 10.0f;
            x = spaced("CROP", x, b_at(kBody), kBody, ink, 1.0f) + 10.0f;
            x = text(p.picture_crop ? "fills the screen" : "whole frame", x, b_at(kLabel),
                     kLabel, t.dim);
            if (p.picture_focus) {
                frame(ix - 6.0f, cy - 12.0f, (x + 8.0f) - (ix - 6.0f), 24.0f, 2.0f, t.accent);
            }
            if (p.show_source) {
                const float sw = spaced_w(kSourceLabel, kLabel, 1.0f);
                spaced(kSourceLabel, ir - sw, b_at(kLabel), kLabel, t.dim, 1.0f);
            }
        } else if (p.bottom == GuideBottom::PressToWatch) {
            spaced("PRESS TO WATCH", ix, b_at(kLabel), kLabel, t.accent, 2.0f);
            if (!p.join_note.empty()) {
                text(p.join_note, ir - body_w(p.join_note, kLabel), b_at(kLabel), kLabel, t.dim);
            }
        }
    }

    // ── Grid ─────────────────────────────────────────────────────────────
    // view.grid's x / y are relative to grid.rect's top-left; row y counts
    // the header. Rows and cells are row_h - 3 tall: the 3 px gaps show the
    // background as grid lines.
    const GuideGrid& g = view.grid;
    const float gx = static_cast<float>(g.rect.x), gy = static_cast<float>(g.rect.y);
    const float rh = static_cast<float>(g.row_h - 3);
    const float col_w = static_cast<float>(g.column_w);

    // Header: the date (dim, in the channel column), the time ticks (steel,
    // a 1 x 12 px mark then the label; the first carries AM/PM).
    {
        const float hb = baseline_in(gy, static_cast<float>(g.header_h), kLabel, false);
        const float date_max = col_w - m.chan_pad - 4.0f;
        const std::string date = fit_to_width(
            g.date_label, static_cast<int>(date_max),
            [&](const std::string& s) { return static_cast<int>(spaced_w(s, kLabel, 2.0f)); });
        spaced(date, gx + m.chan_pad, hb, kLabel, t.dim, 2.0f);
        const float tb = baseline_in(gy, static_cast<float>(g.header_h), kBody, false);
        for (const GuideTick& tick : g.ticks) {
            const float tx = gx + tick.x;
            fill(tx, gy + (g.header_h - 12.0f) / 2.0f, 1.0f, 12.0f, t.accent2);
            text(tick.label, tx + 7.0f, tb, kBody, t.accent2);
        }
    }

    for (const GuideRow& row : g.rows) {
        const float ry = gy + row.y;
        // Channel cell: number (Zen Dots) or the shuffle icon for 00, then
        // the short name at name_x; a 4 px gold bar on the cursor's row.
        fill(gx, ry, col_w, rh, t.bg_lift);
        if (row.cursor_bar) fill(gx, ry, 4.0f, rh, t.accent);
        const Color& num_c = tone(row.num_tone);
        if (row.shuffle) {
            // The mockup's 18 x 12 icon: two crossing strokes, two arrowheads.
            const float ix0 = gx + m.chan_pad;
            const float top = ry + (rh - 12.0f) / 2.0f;
            draw_line(ix0 + 1.0f, top + 10.0f, ix0 + 11.0f, top + 2.0f, 1.8f, num_c, alpha);
            draw_line(ix0 + 1.0f, top + 2.0f, ix0 + 11.0f, top + 10.0f, 1.8f, num_c, alpha);
            for (int i = 0; i < 4; ++i) {  // 4 px wide, 4 px tall at the base
                const float hh = 4.0f * static_cast<float>(4 - i) / 4.0f;
                fill(ix0 + 11.0f + i, top + 2.0f - hh / 2.0f, 1.0f, hh, num_c);
                fill(ix0 + 11.0f + i, top + 10.0f - hh / 2.0f, 1.0f, hh, num_c);
            }
        } else {
            zen(row.number_text, gx + m.chan_pad, baseline_in(ry, rh, m.chan_num, true), m.chan_num,
                num_c);
        }
        text(row.name, gx + g.name_x, baseline_in(ry, rh, kBody, false), kBody,
             tone(row.name_tone));

        const float cells_x = gx + col_w + 3.0f;
        if (row.no_schedule) {
            fill(cells_x, ry, gx + g.rect.w - cells_x, rh, t.bg_lift);
            text(kNoSchedule, cells_x + m.cell_pad, baseline_in(ry, rh, kBody, false), kBody,
                 t.dim);
            continue;
        }
        for (const GuideCell& c : row.cells) {
            const float cx = gx + c.x;
            const float cw = static_cast<float>(c.w);
            fill(cx, ry, cw, rh, t.bg_lift);
            if (c.ring) frame(cx, ry, cw, rh, 2.0f, t.accent);
            if (!c.show_text) continue;
            const Color& ink = tone(c.tone);
            const float tx = cx + m.cell_pad;
            if (c.show_artist && !c.artist.empty()) {
                // Title over artist, the pair centred in the cell.
                const float lh1 = std::round(kBody * 1.2f);
                const float lh2 = std::round(kLabel * 1.2f);
                const float top = ry + (rh - (lh1 + 3.0f + lh2)) / 2.0f;
                const float b1 = baseline_in(top, lh1, kBody, false);
                if (c.bold) bold(c.label, tx, b1, kBody, ink);
                else text(c.label, tx, b1, kBody, ink);
                text(c.artist, tx, baseline_in(top + lh1 + 3.0f, lh2, kLabel, false), kLabel,
                     t.dim);
            } else {
                const float b1 = baseline_in(ry, rh, kBody, false);
                if (c.bold) bold(c.label, tx, b1, kBody, ink);
                else text(c.label, tx, b1, kBody, ink);
            }
        }
    }

    // Now line: 2 px red down every row, a 10 x 6 marker in the header.
    if (!g.rows.empty()) {
        const float nx = gx + g.now_x;
        const float rows_h = static_cast<float>(g.rows.size()) * g.row_h - 3.0f;
        fill(nx, gy + g.header_h, 2.0f, rows_h, t.highlight2);
        triangle(nx + 1.0f, gy + g.header_h - 7.0f, 10.0f, 6.0f, true, t.highlight2);
    }

    // Scroll arrows, as the list draws them (steel, 12 x 7): above and below
    // the grid, centred on the channel column.
    {
        const float acx = gx + col_w / 2.0f;
        if (g.up_arrow) triangle(acx, gy - 10.0f, 12.0f, 7.0f, false, t.accent2);
        if (g.down_arrow) {
            triangle(acx, gy + static_cast<float>(g.rect.h) + 1.0f, 12.0f, 7.0f, true, t.accent2);
        }
    }

    // ── Hints row (knob icon, key, label) and the logo at its right end ──
    {
        const float top = static_cast<float>(m.hint_top);
        const float icon = 15.0f;
        const float cy = top + icon / 2.0f;
        const float b = cy - kLabel / 2.0f + static_cast<float>(body.get_baseline_at_size(kLabel));
        float x = gx;
        for (const GuideHint& h : view.hints) {
            // The knob: a 6 px ring (1.4 px stroke) with its pointer at 12.
            const float kx = x + icon / 2.0f;
            constexpr float kR = 6.0f;
            constexpr int kSegs = 20;
            for (int i = 0; i < kSegs; ++i) {
                const float a0 = 6.2831853f * i / kSegs;
                const float a1 = 6.2831853f * (i + 1) / kSegs;
                draw_line(kx + kR * std::cos(a0), cy + kR * std::sin(a0), kx + kR * std::cos(a1),
                          cy + kR * std::sin(a1), 1.4f, t.dim, alpha);
            }
            fill(kx - 0.7f, cy - kR + 1.0f, 1.4f, 3.75f, t.dim);
            x += icon + 7.0f;
            x = spaced(h.key, x, b, kLabel, t.fg, 1.0f) + 7.0f;
            x = text(h.label, x, b, kLabel, t.dim) + 24.0f;
        }
    }
    if (logo_texture_id_ != 0 && logo_width_ > 0 && logo_height_ > 0) {
        // Fitted inside the view's logo rect, right-aligned, its aspect kept.
        const GuideRect& lr = view.logo;
        const float s = std::min(static_cast<float>(lr.w) / logo_width_,
                                 static_cast<float>(lr.h) / logo_height_);
        const float lw = logo_width_ * s;
        const float lh = logo_height_ * s;
        draw_textured_quad(logo_texture_id_, lr.x + lr.w - lw, lr.y + (lr.h - lh) / 2.0f, lw, lh,
                           alpha);
    }
}

}  // namespace ui
