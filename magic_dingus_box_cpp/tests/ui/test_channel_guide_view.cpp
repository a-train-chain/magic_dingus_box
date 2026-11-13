// Tests for ui::layout_channel_guide (ui/channel_guide_view.h): every
// rectangle, label, tone and flag the Channel Guide painter draws, at both
// logical canvases. TV 960x720, CRT 640x480. Design:
// docs/superpowers/specs/2026-11-12-channel-guide-design.md.

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("long titles truncate by character with an ellipsis", "[guide_view]") {
    Fixture f;
    // 332 px wide cell -> floor((332 - 20) / 8.428) = 37 characters.
    f.channels[1].items.clear();
    const double base = 8 * 3600.0 + 10 * 60;
    f.channels[1].items.push_back(ScheduledItem{1, 0, base});
    f.channels[1].items.push_back(ScheduledItem{1, 1, 600});
    f.channels[1].total_s = base + 600;
    f.playlists[1].items[1].title = std::string(60, 'x');
    auto v = ui::layout_channel_guide(f.in());
    auto& c = v.grid.rows[1].cells[0];
    REQUIRE(c.w == 338);  // 40 chars at this width: (338-20)/8.428 = 37.7 -> 37
    CHECK(c.label == std::string(36, 'x') + "\xE2\x80\xA6");
    // Multi-byte characters are never cut in half.
    f.playlists[1].items[1].title = std::string(60, '\0');
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
    auto v = ui::layout_channel_guide(in);
    CHECK(v.panel.bottom == ui::GuideBottom::Picture);
    CHECK(v.panel.show_source);
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
