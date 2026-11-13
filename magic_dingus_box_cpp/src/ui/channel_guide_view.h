#pragma once

// The Channel Guide's view model: every rectangle, label, tone and flag the
// painter draws, computed from plain inputs at either logical canvas (TV
// 960x720, CRT 640x480). Pure and header-only: no GL, no fonts, no I/O, no
// AppState, and no C time-zone calls (local time is now_utc + utc_offset_s,
// formatted here), so it is unit-tested on the Mac
// (tests/ui/test_channel_guide_view.cpp). Design:
// docs/superpowers/specs/2026-11-12-channel-guide-design.md.
//
// Coordinates: `window`, `panel`, `logo` and `grid.rect` are ABSOLUTE canvas
// px. Everything inside `grid` is relative to the grid's top-left: x from the
// grid's left edge, y from the grid's top (header first, then rows).
// `now_x` is the LEFT edge of the 2 px now line.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/app_state.h"
#include "app/channel_schedule.h"
#include "app/guide_nav.h"

namespace ui {

enum class GuideTone { Fg, Dim, Gold, Green, Steel, Red };
enum class GuideBottom { Picture, PressToWatch, None };

struct GuideRect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct GuideViewInput {
    bool crt = false;
    const std::vector<app::Channel>* channels = nullptr;
    const std::vector<app::Playlist>* playlists = nullptr;
    int tuned = -1;  // the channel to show as tuned (pending tune first); -1 none
    app::GuideCursor cursor;
    bool picture_available = false;
    bool picture_crop = false;
    double now_utc = 0;
    int utc_offset_s = 0;
};

struct GuidePanel {
    std::string eyebrow;
    GuideTone eyebrow_tone = GuideTone::Gold;
    std::string clock_hm, clock_ap;
    std::string ch_text;  // "04", or "MIX" for 00
    std::string playlist_title;
    std::string item_title, artist;
    std::string start_hm, end_hm;
    double progress = 0;
    std::string next_hm, next_title;
    GuideBottom bottom = GuideBottom::PressToWatch;
    std::string join_note;  // "joins at m:ss" (another channel)
    bool show_artist = false, show_next = false, show_source = false;
    // The Picture control's checkbox (ticked = cropped) and its gold focus
    // (the cursor is on it); drawn only when bottom == Picture.
    bool picture_crop = true;
    bool picture_focus = false;
};

struct GuideTick {
    int x = 0;
    std::string label;
};

struct GuideCell {
    int x = 0, w = 0;
    std::string label, artist;
    bool show_text = false, show_artist = false;
    bool ring = false;  // the cursor row's on-now cell
    GuideTone tone = GuideTone::Fg;
    bool bold = false;  // the on-now cell
};

struct GuideRow {
    int y = 0;
    bool shuffle = false;
    std::string number_text;  // "00" too: the painter draws the icon for `shuffle`
    std::string name;         // upper-cased, truncated to the channel column
    GuideTone num_tone = GuideTone::Fg, name_tone = GuideTone::Fg;
    bool cursor_bar = false;
    bool no_schedule = false;  // "NO SCHEDULE YET" (Dim), no cells
    std::vector<GuideCell> cells;
};

struct GuideGrid {
    GuideRect rect;       // absolute
    int header_h = 0, row_h = 0, column_w = 0;
    // Channel name: left edge (grid-relative, after the bar and number) and the
    // width available to it (right margin already kept clear).
    int name_x = 0, name_w = 0;
    std::string date_label;
    std::vector<GuideTick> ticks;
    int now_x = 0;
    bool up_arrow = false, down_arrow = false;
    std::vector<GuideRow> rows;
};

struct GuideHint {
    std::string key, label;
};

struct GuideView {
    bool crt = false;  // laid out for the CRT Native canvas (640x480)
    GuideRect window;
    std::string window_bug;  // "04" / "MIX" / "" (nothing tuned)
    GuideRect panel_rect;
    GuidePanel panel;
    GuideGrid grid;
    std::vector<GuideHint> hints;
    GuideRect logo;
};

namespace guide_detail {

constexpr double kCharAdvance = 0.602 * 14.0;  // DejaVu Sans Mono at 14 px
constexpr int kCellPadX = 10;                  // per side
constexpr int kMinTextW = 30, kMinArtistW = 64;
constexpr const char* kArrow = "\xE2\x97\x82 ";    // "◂ "
constexpr const char* kEllipsis = "\xE2\x80\xA6";  // "…"

inline std::size_t utf8_len(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// First `n` code points of `s`; never cuts inside a multi-byte sequence.
inline std::string utf8_prefix(const std::string& s, std::size_t n) {
    std::size_t cp = 0, i = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (cp == n) break;
            ++cp;
        }
        ++i;
    }
    return s.substr(0, i);
}

inline std::string fit(const std::string& s, int chars) {
    if (chars <= 0) return "";
    if (utf8_len(s) <= static_cast<std::size_t>(chars)) return s;
    return utf8_prefix(s, static_cast<std::size_t>(chars - 1)) + kEllipsis;
}

inline int chars_for_width(int w) {
    return std::max(0, static_cast<int>(std::floor((w - 2 * kCellPadX) / kCharAdvance)));
}

inline std::string upper_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

inline std::string two_digits(int n) {
    char b[16];
    std::snprintf(b, sizeof b, "%02d", n);
    return b;
}

inline double floor_mod(double a, double m) { return a - std::floor(a / m) * m; }

struct Clock {
    int hour12, minute;
    const char* ap;
};

inline Clock clock_of(double local_s) {
    const int sod = static_cast<int>(floor_mod(local_s, 86400.0));
    const int h = sod / 3600;
    Clock c;
    c.minute = (sod / 60) % 60;
    c.hour12 = h % 12 == 0 ? 12 : h % 12;
    c.ap = h < 12 ? "AM" : "PM";
    return c;
}

inline std::string hm(double local_s) {
    const Clock c = clock_of(local_s);
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", c.hour12, c.minute);
    return b;
}

inline std::string date_label(double local_s) {
    static const char* kDays[] = {"THU", "FRI", "SAT", "SUN", "MON", "TUE", "WED"};
    static const char* kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    const long long z0 = static_cast<long long>(std::floor(local_s / 86400.0));
    const int wd = static_cast<int>(((z0 % 7) + 7) % 7);  // 1970-01-01 = Thursday
    // civil_from_days (Howard Hinnant)
    const long long z = z0 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    char b[32];
    std::snprintf(b, sizeof b, "%s %s %d", kDays[wd], kMonths[m - 1], d);
    return b;
}

inline const app::PlaylistItem* item_of(const GuideViewInput& in, const app::ScheduledItem& s) {
    if (!in.playlists || s.playlist < 0 || s.playlist >= static_cast<int>(in.playlists->size()))
        return nullptr;
    const auto& items = (*in.playlists)[s.playlist].items;
    if (s.item < 0 || s.item >= static_cast<int>(items.size())) return nullptr;
    return &items[s.item];
}

inline std::string title_of(const GuideViewInput& in, const app::ScheduledItem& s) {
    const auto* it = item_of(in, s);
    return it ? it->title : "";
}

inline std::string playlist_title(const GuideViewInput& in, int n) {
    if (!in.playlists || n < 0 || n >= static_cast<int>(in.playlists->size())) return "";
    return (*in.playlists)[n].title;
}

}  // namespace guide_detail

inline GuideView layout_channel_guide(const GuideViewInput& in) {
    namespace d = guide_detail;
    GuideView v;
    static const std::vector<app::Channel> kNoChannels;
    const auto& channels = in.channels ? *in.channels : kNoChannels;
    const int n_channels = static_cast<int>(channels.size());

    // Geometry (spec table).
    const bool crt = in.crt;
    v.crt = crt;
    v.window = crt ? GuideRect{24, 24, 224, 168} : GuideRect{40, 40, 384, 288};
    v.panel_rect = crt ? GuideRect{260, 24, 356, 168} : GuideRect{448, 40, 472, 288};
    v.logo = crt ? GuideRect{477, 443, 139, 20} : GuideRect{739, 654, 181, 26};
    const int grid_w = crt ? 592 : 880;
    v.grid.header_h = crt ? 24 : 30;
    v.grid.row_h = crt ? 40 : 52;
    v.grid.column_w = crt ? 132 : 196;
    v.grid.rect = GuideRect{crt ? 24 : 40, crt ? 206 : 352, grid_w,
                            v.grid.header_h + app::kGuideVisibleRows * v.grid.row_h};
    const double window_s = crt ? 900.0 : 1200.0;
    const int col3 = v.grid.column_w + 3;
    const double pps = (grid_w - col3) / window_s;

    // Time: local = utc + offset; the window starts on a 5-minute floor.
    const double local = in.now_utc + in.utc_offset_s;
    const double win_local = std::floor(local / 300.0) * 300.0;
    const double win_start = win_local - in.utc_offset_s;  // UTC
    const double win_end = win_start + window_s;
    auto xpos = [&](double utc) { return col3 + (utc - win_start) * pps; };

    v.grid.date_label = d::date_label(local);
    for (int k = 0; k * 300.0 < window_s; ++k) {
        GuideTick t;
        t.x = static_cast<int>(std::lround(xpos(win_start + k * 300.0)));
        t.label = d::hm(win_local + k * 300.0);
        if (k == 0) t.label += std::string(" ") + d::clock_of(win_local).ap;
        v.grid.ticks.push_back(t);
    }
    v.grid.now_x = static_cast<int>(std::lround(xpos(in.now_utc) - 1));

    const bool tuned_valid = in.tuned >= 0;
    v.window_bug = tuned_valid ? (in.tuned == 0 ? "MIX" : d::two_digits(in.tuned)) : "";

    // Rows.
    const int first = std::max(0, in.cursor.scroll);
    const int last = std::min(n_channels, first + app::kGuideVisibleRows);
    v.grid.up_arrow = first > 0;
    v.grid.down_arrow = first + app::kGuideVisibleRows < n_channels;
    v.grid.name_x = crt ? 40 : 56;
    v.grid.name_w = v.grid.column_w - v.grid.name_x - 8;
    const int name_chars = std::max(0, static_cast<int>(std::floor(v.grid.name_w / d::kCharAdvance)));
    for (int r = first; r < last; ++r) {
        const app::Channel& ch = channels[r];
        GuideRow row;
        row.y = v.grid.header_h + (r - first) * v.grid.row_h;
        row.shuffle = ch.number == 0;
        row.number_text = d::two_digits(ch.number);
        row.name = d::fit(d::upper_ascii(d::playlist_title(in, ch.number)), name_chars);
        const bool is_tuned = tuned_valid && ch.number == in.tuned;
        row.cursor_bar = r == in.cursor.row && !in.cursor.on_picture;
        row.num_tone = is_tuned ? GuideTone::Green : row.cursor_bar ? GuideTone::Gold : GuideTone::Fg;
        row.name_tone = is_tuned ? GuideTone::Green : GuideTone::Fg;
        row.no_schedule = ch.items.empty();
        if (row.no_schedule) {
            row.name_tone = GuideTone::Dim;
        } else {
            for (const app::OnAir& a : app::lineup(ch, win_start, win_end)) {
                if (a.index < 0 || a.index >= static_cast<int>(ch.items.size())) continue;
                const app::ScheduledItem& si = ch.items[a.index];
                const double s = std::max(a.starts_at, win_start);
                const double e = std::min(a.ends_at, win_end);
                const int left = static_cast<int>(std::lround(xpos(s)));
                const int right = static_cast<int>(std::lround(xpos(e)));
                GuideCell c;
                c.x = left;
                c.w = right - left - 3;
                if (c.w < 1) continue;
                const bool began_before = a.starts_at < win_start;
                const bool now_cell = in.now_utc >= a.starts_at && in.now_utc < a.ends_at;
                c.show_text = c.w >= d::kMinTextW;
                c.show_artist = !crt && c.w >= d::kMinArtistW;
                const int chars = d::chars_for_width(c.w);
                c.label = d::fit((began_before ? std::string(d::kArrow) : std::string()) +
                                     d::title_of(in, si),
                                 chars);
                if (!crt) {
                    const auto* it = d::item_of(in, si);
                    c.artist = d::fit(it ? it->artist : "", chars);
                }
                c.bold = now_cell;
                c.ring = now_cell && row.cursor_bar;
                c.tone = now_cell && is_tuned ? GuideTone::Green : GuideTone::Fg;
                row.cells.push_back(std::move(c));
            }
        }
        v.grid.rows.push_back(std::move(row));
    }

    // Panel: the cursor's channel, or the tuned one while on the Picture control.
    const int pc = in.cursor.on_picture && tuned_valid ? in.tuned : in.cursor.row;
    const bool on_tuned = tuned_valid && pc == in.tuned;
    GuidePanel& p = v.panel;
    p.ch_text = pc == 0 ? "MIX" : d::two_digits(pc);
    p.eyebrow = on_tuned ? "NOW PLAYING" : "ON CH " + d::two_digits(pc) + " NOW";
    p.eyebrow_tone = on_tuned ? GuideTone::Green : GuideTone::Gold;
    p.clock_hm = d::hm(local);
    p.clock_ap = d::clock_of(local).ap;
    p.playlist_title = d::playlist_title(in, pc);
    if (pc >= 0 && pc < n_channels && !channels[pc].items.empty()) {
        const app::Channel& ch = channels[pc];
        if (auto a = app::on_air(ch, in.now_utc)) {
            const int ai = std::clamp(a->index, 0, static_cast<int>(ch.items.size()) - 1);
            const app::ScheduledItem& si = ch.items[ai];
            const auto* it = d::item_of(in, si);
            p.item_title = it ? it->title : "";
            p.artist = (!crt && it) ? it->artist : "";
            p.start_hm = d::hm(a->starts_at + in.utc_offset_s);
            p.end_hm = d::hm(a->ends_at + in.utc_offset_s);
            const double len = a->ends_at - a->starts_at;
            p.progress = len > 0 ? std::clamp(a->offset_s / len, 0.0, 1.0) : 0.0;
            const app::ScheduledItem& nx = ch.items[(ai + 1) % ch.items.size()];
            p.next_hm = d::hm(a->ends_at + in.utc_offset_s);
            p.next_title = d::title_of(in, nx);
            if (!on_tuned) {
                const int off = static_cast<int>(std::floor(std::max(0.0, a->offset_s)));
                char b[32];
                std::snprintf(b, sizeof b, "joins at %d:%02d", off / 60, off % 60);
                p.join_note = b;
            }
        }
    }
    p.show_artist = !crt && !p.artist.empty();
    p.show_next = !crt && !p.next_title.empty();
    // Picture control on the tuned row (only when the video is not 4:3);
    // nothing on the tuned row otherwise, nothing on a row with no schedule
    // (a press does nothing); PRESS TO WATCH on another tunable channel.
    const bool pc_tunable = pc >= 0 && pc < n_channels && !channels[pc].items.empty();
    if (on_tuned)
        p.bottom = in.picture_available ? GuideBottom::Picture : GuideBottom::None;
    else
        p.bottom = pc_tunable ? GuideBottom::PressToWatch : GuideBottom::None;
    p.show_source = !crt && p.bottom == GuideBottom::Picture;
    p.picture_crop = in.picture_crop;
    p.picture_focus = in.cursor.on_picture;

    v.hints.push_back({"TURN", "channels"});
    v.hints.push_back({"PRESS", "watch"});
    if (!crt) v.hints.push_back({"PRESS", "on green to hide guide"});
    return v;
}

// A rect on the logical canvas (canvas_w x canvas_h, top-left origin) in
// framebuffer px, top-left origin: where it lands once the canvas is drawn
// into `content_fb` — the 4:3 area in Modern TV (960x720 into 1440x1080 at
// x 240 on a 1920x1080 mode), the whole framebuffer in CRT Native (640x480
// stretched over 1280x720: NOT uniform). Edges are rounded, so neighbouring
// rects stay neighbours. With the content as tall as the framebuffer (every
// 16:9 mode) this is the guide's video window formula: Modern TV
// x_fb = content_x + x * fb_h / 720, y_fb = y * fb_h / 720; CRT Native
// x * fb_w / 640, y * fb_h / 480. An empty canvas maps to an empty rect.
inline GuideRect guide_rect_to_fb(const GuideRect& r, int canvas_w, int canvas_h,
                                  const GuideRect& content_fb) {
    if (canvas_w <= 0 || canvas_h <= 0) return GuideRect{};
    const double sx = static_cast<double>(content_fb.w) / canvas_w;
    const double sy = static_cast<double>(content_fb.h) / canvas_h;
    const int x0 = content_fb.x + static_cast<int>(std::lround(r.x * sx));
    const int y0 = content_fb.y + static_cast<int>(std::lround(r.y * sy));
    const int x1 = content_fb.x + static_cast<int>(std::lround((r.x + r.w) * sx));
    const int y1 = content_fb.y + static_cast<int>(std::lround((r.y + r.h) * sy));
    return GuideRect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace ui
