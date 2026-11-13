// Tests for ui::layout_channel_guide (ui/channel_guide_view.h): every
// rectangle, label, tone and flag the Channel Guide painter draws, at both
// logical canvases. TV 960x720, CRT 640x480. Design:
// docs/superpowers/specs/2026-11-12-channel-guide-design.md.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "app/channel_schedule.h"
#include "ui/channel_guide_view.h"

namespace {

using app::Channel;
using app::Playlist;
using app::PlaylistItem;
using app::ScheduledItem;

constexpr double E = app::kScheduleEpochUtc;
constexpr double kNow = E + 8 * 3600 + 14 * 60 + 30;  // local 8:14:30 AM

struct Fixture {
    std::vector<Playlist> playlists;
    std::vector<Channel> channels;
    Fixture() {
        for (int n = 0; n < 8; ++n) {
            Playlist p;
            p.title = n == 0 ? "Master Shuffle" : "Chan " + std::to_string(n);
            channels.push_back(Channel{});
            channels.back().number = n;
            if (n == 7) {  // empty channel
                playlists.push_back(p);
                continue;
            }
            for (int i = 0; i < 4; ++i) {
                PlaylistItem it;
                it.title = "Show " + std::to_string(n) + "-" + std::to_string(i);
                it.artist = "Artist " + std::to_string(i);
                p.items.push_back(it);
                channels.back().items.push_back(ScheduledItem{n, i, 600.0});
                channels.back().total_s += 600.0;
            }
            playlists.push_back(p);
        }
    }
    ui::GuideViewInput in(bool crt = false) const {
        ui::GuideViewInput i;
        i.crt = crt;
        i.channels = &channels;
        i.playlists = &playlists;
        i.tuned = 0;
        i.cursor = app::GuideCursor{};
        i.picture_available = false;
        i.picture_crop = false;
        i.now_utc = kNow;
        i.utc_offset_s = 0;
        return i;
    }
};

}  // namespace

TEST_CASE("TV geometry, ticks, now line and cells", "[guide_view]") {
    Fixture f;
    auto v = ui::layout_channel_guide(f.in());
    REQUIRE(v.grid.ticks.size() == 4);
    CHECK(v.grid.ticks[0].label == "8:10 AM");
    CHECK(v.grid.ticks[1].label == "8:15");
    CHECK(v.grid.ticks[0].x == 199);
    CHECK(v.grid.ticks[1].x == 369);
    CHECK(v.grid.ticks[3].x == 710);
    CHECK(v.grid.now_x == 351);
    REQUIRE(v.grid.rows.size() == 5);
    auto& cells = v.grid.rows[1].cells;  // 8:10-8:20, 8:20-8:30
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].x == 199);
    CHECK(cells[0].w == 338);
    CHECK(cells[1].x == 540);
    CHECK(cells[1].w == 337);
    CHECK(v.window.x == 40);
    CHECK(v.window.w == 384);
    CHECK(v.logo.x == 739);
    CHECK(v.logo.y == 654);
    CHECK(v.logo.w == 181);
    CHECK(v.logo.h == 26);
    CHECK(v.grid.date_label == "THU JAN 1");
}

TEST_CASE("a programme that began before the window starts with an arrow", "[guide_view]") {
    Fixture f;
    f.channels[1].items.clear();
    f.channels[1].items.push_back(ScheduledItem{1, 0, 8 * 3600.0 + 5 * 60});  // ends 8:05
    f.channels[1].items.push_back(ScheduledItem{1, 1, 1200});                  // 8:05-8:25
    f.channels[1].total_s = 8 * 3600.0 + 5 * 60 + 1200;
    auto v = ui::layout_channel_guide(f.in());
    auto& c = v.grid.rows[1].cells;
    REQUIRE(!c.empty());
    CHECK(c[0].label.rfind("\xE2\x97\x82 ", 0) == 0);
    CHECK(c[0].x == 199);
}

TEST_CASE("narrow cells drop text then artist", "[guide_view]") {
    Fixture f;
    f.channels[1].items.clear();
    const double base = 8 * 3600.0 + 10 * 60;  // the window starts here
    f.channels[1].items.push_back(ScheduledItem{1, 0, base});
    f.channels[1].items.push_back(ScheduledItem{1, 1, 49});   // 28 px slot -> w 25
    f.channels[1].items.push_back(ScheduledItem{1, 2, 93});   // 53 px slot -> w 50
    f.channels[1].items.push_back(ScheduledItem{1, 3, 600});
    f.channels[1].total_s = base + 49 + 93 + 600;
    auto v = ui::layout_channel_guide(f.in());
    auto& c = v.grid.rows[1].cells;
    REQUIRE(c.size() >= 3);
    CHECK(c[0].w == 25);
    CHECK_FALSE(c[0].show_text);
    CHECK_FALSE(c[0].show_artist);
    CHECK(c[1].w == 50);
    CHECK(c[1].show_text);
    CHECK_FALSE(c[1].show_artist);
    CHECK(c[2].show_artist);
}

TEST_CASE("a cell whose label would be only an ellipsis shows no text", "[guide_view]") {
    // Just over the 30 px text threshold a label truncates to "…" alone,
    // which reads as a stray dash on the TV (Pi 5 test, 2026-11-13); with the
    // began-before arrow, "◂…" / "◂ …". A label keeps its text only when at
    // least one character of the title fits.
    Fixture f;
    const double base = 8 * 3600.0 + 10 * 60;  // the window starts here
    // 01: cells from the window's start, 32 px (1 char) then 40 px (2 chars).
    f.channels[1].items = {ScheduledItem{1, 0, base}, ScheduledItem{1, 1, 62},
                           ScheduledItem{1, 2, 76}, ScheduledItem{1, 3, 600}};
    f.channels[1].total_s = base + 62 + 76 + 600;
    // 02-04: a programme that began 30 s before the window, its cell 40, 48
    // and 56 px wide (2, 3, 4 chars with the arrow).
    const double lens[] = {76, 90, 104};
    for (int n = 2; n <= 4; ++n) {
        const double l = lens[n - 2];
        f.channels[n].items = {ScheduledItem{n, 0, base - 30}, ScheduledItem{n, 1, 30 + l},
                               ScheduledItem{n, 2, 600}};
        f.channels[n].total_s = base + l + 600;
    }
    auto v = ui::layout_channel_guide(f.in());
    const auto& r1 = v.grid.rows[1].cells;
    REQUIRE(r1.size() >= 2);
    CHECK(r1[0].w == 32);
    CHECK(r1[0].label == "\xE2\x80\xA6");  // "…"
    CHECK_FALSE(r1[0].show_text);
    CHECK(r1[1].w == 40);
    CHECK(r1[1].label == "S\xE2\x80\xA6");  // "S…": a letter of the title
    CHECK(r1[1].show_text);
    const char* arrow_labels[] = {"\xE2\x97\x82\xE2\x80\xA6", "\xE2\x97\x82 \xE2\x80\xA6",
                                  "\xE2\x97\x82 S\xE2\x80\xA6"};
    const int widths[] = {40, 48, 56};
    for (int n = 2; n <= 4; ++n) {
        const auto& c = v.grid.rows[n].cells;
        REQUIRE_FALSE(c.empty());
        INFO("channel " << n);
        CHECK(c[0].w == widths[n - 2]);
        CHECK(c[0].label == arrow_labels[n - 2]);
        CHECK(c[0].show_text == (n == 4));
    }
}

TEST_CASE("a label or artist line with no character of its text is not shown",
          "[guide_view]") {
    namespace d = ui::guide_detail;
    CHECK_FALSE(d::has_text_chars(""));
    CHECK_FALSE(d::has_text_chars("\xE2\x80\xA6"));                 // …
    CHECK_FALSE(d::has_text_chars("\xE2\x97\x82\xE2\x80\xA6"));     // ◂…
    CHECK_FALSE(d::has_text_chars("\xE2\x97\x82 \xE2\x80\xA6"));    // ◂ …
    CHECK_FALSE(d::has_text_chars("\xE2\x97\x82 "));                // ◂ (empty title)
    CHECK(d::has_text_chars("S\xE2\x80\xA6"));
    CHECK(d::has_text_chars("\xE2\x97\x82 S\xE2\x80\xA6"));
    CHECK(d::has_text_chars("A"));
    // The artist line: an artist fitted to one character is not drawn.
    CHECK_FALSE(d::has_text_chars(d::fit("Artist 1", 1)));
    CHECK(d::has_text_chars(d::fit("Artist 1", 5)));
}

TEST_CASE("long titles truncate by character with an ellipsis", "[guide_view]") {
    Fixture f;
    // A 338 px cell -> floor((338 - 20) / 8.428) = 37 characters.
    f.channels[1].items.clear();
    const double base = 8 * 3600.0 + 10 * 60;
    f.channels[1].items.push_back(ScheduledItem{1, 0, base});
    f.channels[1].items.push_back(ScheduledItem{1, 1, 600});
    f.channels[1].total_s = base + 600;
    f.playlists[1].items[1].title = std::string(60, 'x');
    auto v = ui::layout_channel_guide(f.in());
    auto& c = v.grid.rows[1].cells[0];
    REQUIRE(c.w == 338);
    CHECK(c.label == std::string(36, 'x') + "\xE2\x80\xA6");
    // Multi-byte characters are never cut in half.
    std::string uni;
    for (int i = 0; i < 60; ++i) uni += "\xC3\xA9";  // é
    f.playlists[1].items[1].title = uni;
    v = ui::layout_channel_guide(f.in());
    CHECK(v.grid.rows[1].cells[0].label == [] {
        std::string r;
        for (int i = 0; i < 36; ++i) r += "\xC3\xA9";
        return r + "\xE2\x80\xA6";
    }());
}

TEST_CASE("cursor row: bar, ring on the on-now cell", "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.cursor.row = 2;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.grid.rows[2].cursor_bar);
    CHECK_FALSE(v.grid.rows[1].cursor_bar);
    bool ring = false;
    for (auto& c : v.grid.rows[2].cells) ring = ring || c.ring;
    CHECK(ring);
    CHECK(v.grid.rows[2].cells[0].ring);  // the 8:10 programme holds 8:14:30
    for (auto& c : v.grid.rows[1].cells) CHECK_FALSE(c.ring);
    CHECK(v.panel.eyebrow == "ON CH 02 NOW");
    CHECK(v.panel.eyebrow_tone == ui::GuideTone::Gold);
    CHECK(v.panel.bottom == ui::GuideBottom::PressToWatch);
    CHECK(v.panel.join_note == "joins at 4:30");
}

TEST_CASE("shuffle tuned row is green", "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.tuned = 0;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.grid.rows[0].shuffle);
    CHECK(v.grid.rows[0].num_tone == ui::GuideTone::Green);
    CHECK(v.panel.ch_text == "MIX");
    CHECK(v.window_bug == "MIX");
    CHECK(v.panel.eyebrow == "NOW PLAYING");
    CHECK(v.panel.eyebrow_tone == ui::GuideTone::Green);
    CHECK(v.grid.rows[1].num_tone != ui::GuideTone::Green);
}

TEST_CASE("the picture control shows on the tuned row when available", "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.tuned = 1;
    in.cursor.row = 1;
    in.picture_available = true;
    in.source_aspect = 16.0 / 9.0;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.bottom == ui::GuideBottom::Picture);
    CHECK(v.panel.show_source);
    CHECK(v.panel.source_label == "16:9 SOURCE");
    CHECK(v.window_bug == "01");
    in.cursor = app::GuideCursor{0, 0, true};
    v = ui::layout_channel_guide(in);
    CHECK(v.panel.bottom == ui::GuideBottom::Picture);
    CHECK(v.panel.ch_text == "01");
    CHECK_FALSE(v.grid.rows[0].cursor_bar);
}

TEST_CASE("an empty channel has no schedule and no cells", "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.cursor.scroll = 3;
    in.cursor.row = 7;
    auto v = ui::layout_channel_guide(in);
    REQUIRE(v.grid.rows.size() == 5);
    auto& r = v.grid.rows[4];
    CHECK(r.no_schedule);
    CHECK(r.cells.empty());
    CHECK(r.name_tone == ui::GuideTone::Dim);
    CHECK(v.grid.up_arrow);
    CHECK_FALSE(v.grid.down_arrow);
}

TEST_CASE("nothing tuned: no green, no bug, press to watch", "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.tuned = -1;
    auto v = ui::layout_channel_guide(in);
    for (auto& r : v.grid.rows) {
        CHECK(r.num_tone != ui::GuideTone::Green);
        CHECK(r.name_tone != ui::GuideTone::Green);
        for (auto& c : r.cells) CHECK(c.tone != ui::GuideTone::Green);
    }
    CHECK(v.window_bug == "");
    CHECK(v.panel.bottom == ui::GuideBottom::PressToWatch);
    CHECK(v.panel.eyebrow_tone == ui::GuideTone::Gold);
    CHECK(v.panel.eyebrow == "ON CH 00 NOW");
}

TEST_CASE("panel: clock, item, progress, next", "[guide_view]") {
    Fixture f;
    auto v = ui::layout_channel_guide(f.in());
    CHECK(v.panel.clock_hm == "8:14");
    CHECK(v.panel.clock_ap == "AM");
    CHECK(v.panel.item_title.rfind("Show 0-", 0) == 0);
    CHECK(v.panel.start_hm == "8:10");
    CHECK(v.panel.end_hm == "8:20");
    CHECK(v.panel.progress > 0.449);
    CHECK(v.panel.progress < 0.451);
    CHECK(v.panel.show_next);
    CHECK(v.panel.next_hm == "8:20");
    CHECK(v.panel.show_artist);
}

TEST_CASE("local time follows the utc offset", "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.utc_offset_s = -5 * 3600;  // 8:14 UTC is 3:14 local
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.clock_hm == "3:14");
    CHECK(v.grid.ticks[0].label == "3:10 AM");
}

TEST_CASE("CRT drops ticks, artists, next and the source label", "[guide_view]") {
    Fixture f;
    auto in = f.in(true);
    in.picture_available = true;
    in.cursor.row = 0;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.grid.ticks.size() == 3);
    CHECK(v.window.x == 24);
    CHECK(v.window.w == 224);
    CHECK(v.logo.x == 477);
    CHECK(v.logo.y == 443);
    CHECK(v.logo.w == 139);
    CHECK(v.logo.h == 20);
    CHECK_FALSE(v.panel.show_next);
    CHECK_FALSE(v.panel.show_artist);
    CHECK_FALSE(v.panel.show_source);
    CHECK(v.panel.artist.empty());
    for (auto& r : v.grid.rows)
        for (auto& c : r.cells) {
            CHECK_FALSE(c.show_artist);
            CHECK(c.artist.empty());
        }
    CHECK(v.hints.size() == 2);
}

TEST_CASE("TV hints", "[guide_view]") {
    Fixture f;
    auto v = ui::layout_channel_guide(f.in());
    REQUIRE(v.hints.size() == 3);
    CHECK(v.hints[0].key == "TURN");
    CHECK(v.hints[0].label == "channels");
    CHECK(v.hints[2].label == "on green to hide guide");
}

TEST_CASE("the bottom row: None on a tuned 4:3 row and on a no-schedule row", "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.tuned = 1;
    in.cursor.row = 1;
    in.picture_available = false;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.bottom == ui::GuideBottom::None);
    CHECK(v.panel.join_note.empty());
    in.cursor.row = 7;  // no schedule
    in.cursor.scroll = 3;
    v = ui::layout_channel_guide(in);
    CHECK(v.panel.bottom == ui::GuideBottom::None);
    in.tuned = -1;
    v = ui::layout_channel_guide(in);
    CHECK(v.panel.bottom == ui::GuideBottom::None);
}

TEST_CASE("the view carries the canvas and the Picture control's state", "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.tuned = 1;
    in.cursor = app::GuideCursor{0, 0, true};
    in.picture_available = true;
    in.picture_crop = false;
    auto v = ui::layout_channel_guide(in);
    CHECK_FALSE(v.crt);
    CHECK(v.panel.bottom == ui::GuideBottom::Picture);
    CHECK_FALSE(v.panel.picture_crop);
    CHECK(v.panel.picture_focus);
    in.picture_crop = true;
    in.cursor = app::GuideCursor{1, 0, false};
    v = ui::layout_channel_guide(in);
    CHECK(v.panel.picture_crop);
    CHECK_FALSE(v.panel.picture_focus);
    in.crt = true;
    CHECK(ui::layout_channel_guide(in).crt);
}

TEST_CASE("the source label names the playing aspect only when the name is true",
          "[guide_view]") {
    using ui::guide_source_label;
    SECTION("common aspects, within 2 %") {
        CHECK(guide_source_label(16.0 / 9.0) == "16:9 SOURCE");
        CHECK(guide_source_label(1.78) == "16:9 SOURCE");
        CHECK(guide_source_label(16.0 / 9.0 * 1.019) == "16:9 SOURCE");
        CHECK(guide_source_label(1.85) == "1.85:1 SOURCE");
        CHECK(guide_source_label(2.39) == "2.39:1 SOURCE");
        CHECK(guide_source_label(2.35) == "2.39:1 SOURCE");
        CHECK(guide_source_label(2.40) == "2.39:1 SOURCE");
        CHECK(guide_source_label(2.43) == "2.39:1 SOURCE");
        CHECK(guide_source_label(21.0 / 9.0) == "21:9 SOURCE");
    }
    SECTION("two names in reach: the nearer one") {
        // 2.34 is 0.29 % from 21:9 and 0.43 % from 2.35.
        CHECK(guide_source_label(2.34) == "21:9 SOURCE");
        CHECK(guide_source_label(2.345) == "2.39:1 SOURCE");
    }
    SECTION("taller than wide: portrait") {
        CHECK(guide_source_label(9.0 / 16.0) == "PORTRAIT SOURCE");
        CHECK(guide_source_label(0.99) == "PORTRAIT SOURCE");
    }
    SECTION("wider than 4:3 with no common name: wide") {
        CHECK(guide_source_label(1.6) == "WIDE SOURCE");   // 16:10, 10 % off 16:9
        CHECK(guide_source_label(2.0) == "WIDE SOURCE");   // 8 % off 1.85
        CHECK(guide_source_label(1.40) == "WIDE SOURCE");  // past 4:3's 2 %
        CHECK(guide_source_label(2.76) == "WIDE SOURCE");  // Ultra Panavision
    }
    SECTION("nothing to name truthfully: no label") {
        CHECK(guide_source_label(0.0) == "");
        CHECK(guide_source_label(-1.0) == "");
        CHECK(guide_source_label(std::nan("")) == "");
        CHECK(guide_source_label(std::numeric_limits<double>::infinity()) == "");
        CHECK(guide_source_label(4.0 / 3.0) == "");
        CHECK(guide_source_label(4.0 / 3.0 * 1.019) == "");  // 4:3 by is_four_by_three
        CHECK(guide_source_label(1.0) == "");
        CHECK(guide_source_label(1.25) == "");  // 5:4: between square and 4:3
    }
}

TEST_CASE("the panel carries the source label beside the Picture control only",
          "[guide_view]") {
    Fixture f;
    auto in = f.in();
    in.tuned = 1;
    in.cursor.row = 1;
    in.picture_available = true;
    in.source_aspect = 2.39;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.source_label == "2.39:1 SOURCE");
    CHECK(v.panel.show_source);
    SECTION("unknown aspect: the control, no label") {
        in.source_aspect = 0.0;
        v = ui::layout_channel_guide(in);
        CHECK(v.panel.bottom == ui::GuideBottom::Picture);
        CHECK(v.panel.source_label.empty());
        CHECK_FALSE(v.panel.show_source);
    }
    SECTION("CRT drops it") {
        in.crt = true;
        v = ui::layout_channel_guide(in);
        CHECK(v.panel.bottom == ui::GuideBottom::Picture);
        CHECK(v.panel.source_label.empty());
        CHECK_FALSE(v.panel.show_source);
    }
    SECTION("no Picture control (another channel): no label") {
        in.cursor.row = 2;
        v = ui::layout_channel_guide(in);
        CHECK(v.panel.bottom == ui::GuideBottom::PressToWatch);
        CHECK(v.panel.source_label.empty());
        CHECK_FALSE(v.panel.show_source);
    }
}

TEST_CASE("the video window in framebuffer px", "[guide_view]") {
    const ui::GuideRect tv{40, 40, 384, 288};
    const ui::GuideRect crt{24, 24, 224, 168};
    // Modern TV at 1920x1080: the 960x720 canvas is the 1440x1080 4:3 area
    // at x 240 — x_fb = 240 + x * 1080 / 720, y_fb = y * 1080 / 720.
    auto w = ui::guide_rect_to_fb(tv, 960, 720, ui::GuideRect{240, 0, 1440, 1080});
    CHECK(w.x == 300);
    CHECK(w.y == 60);
    CHECK(w.w == 576);
    CHECK(w.h == 432);
    // Modern TV at 1280x720: identity inside the 960x720 area at x 160.
    w = ui::guide_rect_to_fb(tv, 960, 720, ui::GuideRect{160, 0, 960, 720});
    CHECK(w.x == 200);
    CHECK(w.y == 40);
    CHECK(w.w == 384);
    CHECK(w.h == 288);
    // CRT Native: 640x480 stretched over the whole 1280x720 framebuffer —
    // x * 1280 / 640, y * 720 / 480 (not uniform).
    w = ui::guide_rect_to_fb(crt, 640, 480, ui::GuideRect{0, 0, 1280, 720});
    CHECK(w.x == 48);
    CHECK(w.y == 36);
    CHECK(w.w == 448);
    CHECK(w.h == 252);
    // A 4:3 area that does not fill the height (a 5:4 mode letterboxes it):
    // the area's own offset and scale, edges rounded.
    w = ui::guide_rect_to_fb(tv, 960, 720, ui::GuideRect{0, 32, 1280, 960});
    CHECK(w.x == 53);   // round(40 * 4/3 = 53.33)
    CHECK(w.y == 85);   // 32 + round(53.33)
    CHECK(w.w == 512);  // round(424 * 4/3 = 565.33) - 53
    CHECK(w.h == 384);  // 32 + round(328 * 4/3 = 437.33) - 85
    // No canvas: nothing.
    w = ui::guide_rect_to_fb(tv, 0, 720, ui::GuideRect{0, 0, 1280, 720});
    CHECK(w.w == 0);
    CHECK(w.h == 0);
}

TEST_CASE("channel names truncate to the column's real width", "[guide_view]") {
    Fixture f;
    f.playlists[1].title = std::string(40, 'a');
    auto v = ui::layout_channel_guide(f.in());
    CHECK(v.grid.name_x == 56);
    CHECK(v.grid.name_w == 132);
    CHECK(v.grid.name_px == 14);  // TV: the body size (the spec's 14 px floor)
    CHECK(v.grid.rows[1].name == std::string(14, 'A') + "\xE2\x80\xA6");  // 15 chars
    // CRT Native: the widest channel number ends at x 33 (measured on the
    // box), so the name starts at 36 with a 4 px right margin, in 12 px mono
    // (advance 0.602 em = 7.22 px): 12 characters, not 9.
    v = ui::layout_channel_guide(f.in(true));
    CHECK(v.grid.name_x == 36);
    CHECK(v.grid.name_w == 92);  // 132 - 36 - 4
    CHECK(v.grid.name_px == 12);
    CHECK(v.grid.rows[1].name == std::string(11, 'A') + "\xE2\x80\xA6");  // 12 chars
    CHECK(v.grid.rows[0].name == "MASTER SHUF\xE2\x80\xA6");  // was "MASTER S…"
    f.playlists[1].title = "Saturday AMs";  // exactly 12: whole
    v = ui::layout_channel_guide(f.in(true));
    CHECK(v.grid.rows[1].name == "SATURDAY AMS");
}

// ── The tuned channel's panel describes the picture (final review I3) ────
// Channel 01's on-air airing at 8:14:30 is Show 1-1 (8:10-8:20, 4:30 in).
// After a pause or a seek back that carried the playing item past its slot,
// or under a clock that NTP moved after the power-on tune joined, the window
// shows another item: on the tuned row the panel names THAT one, with times
// from its position; the grid stays the schedule.

namespace {

ui::GuideViewInput tuned_one(const Fixture& f, int item, double position_s,
                             double duration_s = 600.0) {
    auto in = f.in();
    in.tuned = 1;
    in.cursor.row = 1;
    in.playing.playlist = 1;
    in.playing.item = item;
    in.playing.position_s = position_s;
    in.playing.duration_s = duration_s;
    return in;
}

}  // namespace

TEST_CASE("a tuned item paused past its slot: the panel describes what plays",
          "[guide_view]") {
    Fixture f;
    // Show 1-0 (scheduled 8:00-8:10) is still playing, 9:50 in.
    auto v = ui::layout_channel_guide(tuned_one(f, 0, 590.0));
    CHECK(v.panel.eyebrow == "NOW PLAYING");
    CHECK(v.panel.item_title == "Show 1-0");
    CHECK(v.panel.artist == "Artist 0");
    CHECK(v.panel.start_hm == "8:04");  // 8:14:30 - 9:50 = 8:04:40
    CHECK(v.panel.end_hm == "8:14");    // + 10:00 = 8:14:40
    CHECK(v.panel.progress > 0.983);
    CHECK(v.panel.progress < 0.984);
    // Next: what the channel joins when it ends (8:14:40): Show 1-1, live.
    CHECK(v.panel.next_hm == "8:14");
    CHECK(v.panel.next_title == "Show 1-1");
    CHECK(v.panel.join_note.empty());
    // The grid is still the schedule.
    REQUIRE_FALSE(v.grid.rows[1].cells.empty());
    CHECK(v.grid.rows[1].cells[0].label == "Show 1-1");
    CHECK(v.grid.rows[1].cells[0].tone == ui::GuideTone::Green);
}

TEST_CASE("a tuned item ahead of its slot: the panel describes what plays",
          "[guide_view]") {
    Fixture f;
    // Show 1-2 (scheduled 8:20-8:30) started 5 s ago, ahead of schedule.
    auto v = ui::layout_channel_guide(tuned_one(f, 2, 5.0));
    CHECK(v.panel.item_title == "Show 1-2");
    CHECK(v.panel.start_hm == "8:14");  // 8:14:25
    CHECK(v.panel.end_hm == "8:24");    // 8:24:25
    CHECK(v.panel.progress > 0.0083);
    CHECK(v.panel.progress < 0.0084);
    // At 8:24:25 Show 1-2's own slot is on air: the boundary never rejoins
    // the item that just ended, so the next is Show 1-3.
    CHECK(v.panel.next_hm == "8:24");
    CHECK(v.panel.next_title == "Show 1-3");
}

TEST_CASE("the on-air item playing: the panel follows its real position", "[guide_view]") {
    // Pi 5 test, 2026-11-13: a join's KEY_UNIT seek lands on the keyframe
    // BEFORE its target, so the picture runs up to a GOP behind the schedule
    // while the panel's bar and times ran on the schedule's. When the playing
    // item IS the on-air airing's, they now come from the player's position
    // and the item's length too; NEXT stays the schedule's next airing.
    Fixture f;
    const auto sched = ui::layout_channel_guide([&] {
        auto in = f.in();
        in.tuned = 1;
        in.cursor.row = 1;
        return in;
    }());
    REQUIRE(sched.panel.start_hm == "8:10");  // Show 1-1 airs 8:10-8:20, 4:30 in
    REQUIRE(sched.panel.progress == 0.45);

    SECTION("a keyframe behind: the bar is the picture's") {
        auto v = ui::layout_channel_guide(tuned_one(f, 1, 262.0));  // 8 s behind
        CHECK(v.panel.eyebrow == "NOW PLAYING");
        CHECK(v.panel.item_title == "Show 1-1");
        CHECK(v.panel.progress > 0.4366);  // 262 / 600
        CHECK(v.panel.progress < 0.4367);
        CHECK(v.panel.start_hm == "8:10");  // 8:10:08
        CHECK(v.panel.end_hm == "8:20");    // 8:20:08
        CHECK(v.panel.next_title == "Show 1-2");
        CHECK(v.panel.next_hm == "8:20");
    }
    SECTION("a minute behind (a pause inside its slot): its own times") {
        auto v = ui::layout_channel_guide(tuned_one(f, 1, 210.0));
        CHECK(v.panel.item_title == "Show 1-1");
        CHECK(v.panel.start_hm == "8:11");  // 8:14:30 - 3:30
        CHECK(v.panel.end_hm == "8:21");
        CHECK(v.panel.progress > 0.3499);
        CHECK(v.panel.progress < 0.3501);
        // NEXT is the schedule's next airing, Show 1-2, at the end the panel
        // shows — never "8:11 - 8:21 · NEXT 8:20" (review M2).
        CHECK(v.panel.next_title == "Show 1-2");
        CHECK(v.panel.next_hm == "8:21");
        CHECK(v.panel.next_hm == v.panel.end_hm);
        CHECK(v.panel.join_note.empty());
        // The grid is still the schedule.
        CHECK(v.grid.rows[1].cells[0].label == "Show 1-1");
        CHECK(v.grid.rows[1].cells[0].x == sched.grid.rows[1].cells[0].x);
    }
    SECTION("on time, a frame either side: the schedule's minutes exactly") {
        // 8:10:00 is a minute edge: a position sampled 30 ms ahead of the
        // clock must not flip the start to 8:09 (nor the end to 8:19).
        for (double pos : {269.97, 270.0, 270.03}) {
            INFO("position " << pos);
            auto v = ui::layout_channel_guide(tuned_one(f, 1, pos));
            CHECK(v.panel.start_hm == sched.panel.start_hm);
            CHECK(v.panel.end_hm == sched.panel.end_hm);
            CHECK(v.panel.next_hm == sched.panel.next_hm);
            CHECK(v.panel.next_title == sched.panel.next_title);
            CHECK(std::fabs(v.panel.progress - pos / 600.0) < 1e-9);
        }
    }
    SECTION("its start trim: the position counts from the trim") {
        f.playlists[1].items[1].start = 30.0;  // the file's 0:30 is the item's 0:00
        auto v = ui::layout_channel_guide(tuned_one(f, 1, 30.0 + 210.0));
        CHECK(v.panel.start_hm == "8:11");
        CHECK(v.panel.progress > 0.3499);
        CHECK(v.panel.progress < 0.3501);
    }
    SECTION("CRT: the same position, the CRT's drops") {
        auto in = tuned_one(f, 1, 210.0);
        in.crt = true;
        auto v = ui::layout_channel_guide(in);
        CHECK(v.panel.start_hm == "8:11");
        CHECK(v.panel.artist.empty());
        CHECK_FALSE(v.panel.show_next);
    }
    SECTION("another channel's row: the schedule") {
        auto in = tuned_one(f, 1, 210.0);
        in.cursor.row = 2;
        auto v = ui::layout_channel_guide(in);
        CHECK(v.panel.start_hm == "8:10");
        CHECK(v.panel.progress == 0.45);
    }
}

TEST_CASE("another channel's panel, or nothing known playing: the schedule",
          "[guide_view]") {
    Fixture f;
    SECTION("the cursor on another channel") {
        auto in = tuned_one(f, 0, 590.0);
        in.cursor.row = 2;
        auto v = ui::layout_channel_guide(in);
        CHECK(v.panel.eyebrow == "ON CH 02 NOW");
        CHECK(v.panel.item_title == "Show 2-1");
        CHECK(v.panel.start_hm == "8:10");
    }
    SECTION("nothing known playing (a switch pending, no video)") {
        auto in = tuned_one(f, 0, 590.0);
        in.playing = ui::GuidePlaying{};
        auto v = ui::layout_channel_guide(in);
        CHECK(v.panel.item_title == "Show 1-1");
        CHECK(v.panel.start_hm == "8:10");
    }
    SECTION("indexes past the playlists") {
        auto in = tuned_one(f, 0, 590.0);
        in.playing.item = 99;
        auto v = ui::layout_channel_guide(in);
        CHECK(v.panel.item_title == "Show 1-1");
    }
}

TEST_CASE("on the Picture control the tuned panel describes what plays too",
          "[guide_view]") {
    Fixture f;
    auto in = tuned_one(f, 0, 590.0);
    in.cursor = app::GuideCursor{0, 0, true};
    in.picture_available = true;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.item_title == "Show 1-0");
    CHECK(v.panel.bottom == ui::GuideBottom::Picture);
}

TEST_CASE("a playing item the channel does not schedule: its own trimmed length",
          "[guide_view]") {
    Fixture f;
    // Chan 2's first item, trimmed 1:40-6:40 (300 s), 2:30 into the trim.
    f.playlists[2].items[0].start = 100.0;
    f.playlists[2].items[0].end = 400.0;
    auto in = tuned_one(f, 0, 250.0, 900.0);
    in.playing.playlist = 2;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.item_title == "Show 2-0");
    CHECK(v.panel.start_hm == "8:12");  // 8:14:30 - 2:30
    CHECK(v.panel.end_hm == "8:17");    // + 5:00
    CHECK(v.panel.progress > 0.499);
    CHECK(v.panel.progress < 0.501);
    // Untrimmed: the file's duration from the start.
    f.playlists[2].items[0].start = 0.0;
    f.playlists[2].items[0].end = 0.0;
    in.playing.position_s = 450.0;
    v = ui::layout_channel_guide(in);
    CHECK(v.panel.progress > 0.499);
    CHECK(v.panel.progress < 0.501);
    // No length known at all: the schedule.
    in.playing.duration_s = 0.0;
    v = ui::layout_channel_guide(in);
    CHECK(v.panel.item_title == "Show 1-1");
}

TEST_CASE("CRT: the playing item's panel keeps the CRT's drops", "[guide_view]") {
    Fixture f;
    auto in = tuned_one(f, 0, 590.0);
    in.crt = true;
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.item_title == "Show 1-0");
    CHECK(v.panel.artist.empty());
    CHECK_FALSE(v.panel.show_artist);
    CHECK_FALSE(v.panel.show_next);
}

TEST_CASE("joins at m:ss is the tune's own join point", "[guide_view]") {
    // With under 2 s of the on-air item left a tune starts the next item at
    // 0:00 (app::join_point); the note must say so, not 9:59.
    Fixture f;
    auto in = f.in();
    in.cursor.row = 2;
    in.now_utc = E + 8 * 3600 + 20 * 60 - 1.0;  // 8:19:59: Show 2-1 has 1 s left
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.join_note == "joins at 0:00");
    in.now_utc = E + 8 * 3600 + 20 * 60 - 2.0;  // 2 s left: still joined
    v = ui::layout_channel_guide(in);
    CHECK(v.panel.join_note == "joins at 9:58");
}

TEST_CASE("CRT geometry: ticks, now line and cells in exact px", "[guide_view]") {
    // 640x480: grid x24 w592, channel column 132, a 15-minute window.
    // px per second = (592 - 132 - 3) / 900 = 0.50778; cells start at 135.
    Fixture f;
    auto v = ui::layout_channel_guide(f.in(true));
    CHECK(v.grid.rect.x == 24);
    CHECK(v.grid.rect.y == 206);
    CHECK(v.grid.rect.w == 592);
    CHECK(v.grid.header_h == 24);
    CHECK(v.grid.row_h == 40);
    CHECK(v.grid.column_w == 132);
    REQUIRE(v.grid.ticks.size() == 3);
    CHECK(v.grid.ticks[0].label == "8:10 AM");
    CHECK(v.grid.ticks[1].label == "8:15");
    CHECK(v.grid.ticks[2].label == "8:20");
    CHECK(v.grid.ticks[0].x == 135);  // 135
    CHECK(v.grid.ticks[1].x == 287);  // 135 + 152.33
    CHECK(v.grid.ticks[2].x == 440);  // 135 + 304.67
    CHECK(v.grid.now_x == 271);       // 135 + 270 s * 0.50778 - 1 = 271.1
    REQUIRE(v.grid.rows.size() == 5);
    CHECK(v.grid.rows[1].y == 24 + 40);
    auto& cells = v.grid.rows[1].cells;  // 8:10-8:20, then 8:20 to the window's end
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].x == 135);
    CHECK(cells[0].w == 302);  // 440 - 135 - 3
    CHECK(cells[1].x == 440);
    CHECK(cells[1].w == 149);  // 592 - 440 - 3
    CHECK(cells[0].show_text);
    CHECK_FALSE(cells[0].show_artist);
}

TEST_CASE("the next item loaded in the on-air one's last 2 s keeps the schedule's times",
          "[guide_view]") {
    // A boundary (or a tune) in the on-air item's last kMinJoinLeftS loads
    // the next item from its start: that IS the schedule's next airing, so
    // the panel keeps its exact times rather than ones derived from a
    // position that has not moved yet (8:19:59 -> "8:19 - 8:29").
    Fixture f;
    auto in = tuned_one(f, 2, 0.0);
    in.now_utc = E + 8 * 3600 + 20 * 60 - 1.0;  // 8:19:59: Show 1-1 has 1 s left
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.item_title == "Show 1-2");
    CHECK(v.panel.start_hm == "8:20");
    CHECK(v.panel.end_hm == "8:30");
    CHECK(v.panel.progress == 0.0);
    CHECK(v.panel.next_hm == "8:30");
    CHECK(v.panel.next_title == "Show 1-3");
    // With 1 s left and the ending item still playing: the schedule, as ever.
    in.playing.item = 1;
    in.playing.position_s = 599.0;
    v = ui::layout_channel_guide(in);
    CHECK(v.panel.item_title == "Show 1-1");
    CHECK(v.panel.start_hm == "8:10");
}
