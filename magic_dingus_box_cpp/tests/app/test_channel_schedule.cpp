// The Channel Guide schedule (app/channel_schedule.h): a pure function of the
// wall clock, anchored at 2026-01-01T00:00:00Z.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <set>
#include <string>
#include <vector>

#include "app/channel_schedule.h"

using Catch::Approx;

namespace {

constexpr double E = app::kScheduleEpochUtc;

app::PlaylistItem item(const std::string& path, const std::string& type = "") {
    app::PlaylistItem it;
    it.path = path;
    it.source_type = type;
    return it;
}

app::Channel channel_of(std::vector<double> lengths) {
    app::Channel c;
    int i = 0;
    for (double l : lengths) {
        c.items.push_back({1, i++, l});
        c.total_s += l;
    }
    return c;
}

app::MediaInfoMap durations(std::initializer_list<std::pair<const char*, double>> l) {
    app::MediaInfoMap m;
    for (auto& [k, d] : l) m[k].duration_s = d;
    return m;
}

// The UI list: row 0 is the virtual Master Shuffle row exactly as
// split_for_ui_with_master_shuffle makes it (one dummy item), then the rows.
// Built directly so game items stay in the rows and the indices are the
// authored ones.
std::vector<app::Playlist> ui_rows(std::vector<std::vector<app::PlaylistItem>> rows) {
    std::vector<app::Playlist> ui;
    app::Playlist master;
    master.title = "Master Shuffle";
    master.items.push_back({});
    ui.push_back(master);
    for (auto& r : rows) {
        app::Playlist p;
        p.title = "P";
        p.items = r;
        ui.push_back(p);
    }
    return ui;
}

}  // namespace

TEST_CASE("on_air walks the loop from the epoch", "[channel_schedule]") {
    auto c = channel_of({100, 200, 300});
    CHECK(app::on_air(c, E + 50)->index == 0);
    CHECK(app::on_air(c, E + 50)->offset_s == Approx(50));
    CHECK(app::on_air(c, E + 350)->index == 2);
    CHECK(app::on_air(c, E + 350)->offset_s == Approx(50));
    CHECK(app::on_air(c, E + 650)->starts_at == Approx(E + 600));
    CHECK(app::on_air(c, E + 650)->ends_at == Approx(E + 700));
}

TEST_CASE("on_air wraps backwards before the epoch", "[channel_schedule]") {
    auto c = channel_of({100, 200, 300});
    CHECK(app::on_air(c, E - 50)->index == 2);
    CHECK(app::on_air(c, E - 50)->offset_s == Approx(250));
    CHECK(app::on_air(c, E - 50)->starts_at == Approx(E - 300));
}

TEST_CASE("lineup lists items overlapping the window", "[channel_schedule]") {
    auto c = channel_of({100, 200, 300});
    auto L = app::lineup(c, E + 50, E + 400);
    REQUIRE(L.size() == 3);
    CHECK(L[0].index == 0);
    CHECK(L[1].starts_at == Approx(E + 100));
    CHECK(L[2].index == 2);
    CHECK(app::lineup(c, E + 50, E + 50).empty());
    // wraps past the end of the loop
    auto W = app::lineup(c, E + 550, E + 650);
    REQUIRE(W.size() == 2);
    CHECK(W[1].index == 0);
    CHECK(W[1].starts_at == Approx(E + 600));
}

TEST_CASE("empty channel has nothing on air", "[channel_schedule]") {
    CHECK_FALSE(app::on_air(app::Channel{}, E).has_value());
    CHECK(app::lineup(app::Channel{}, E, E + 60).empty());
}

TEST_CASE("media_key_of_item strips ./ and data/", "[channel_schedule]") {
    CHECK(app::media_key_of_item(item("data/media/a.mp4")) == "media/a.mp4");
    CHECK(app::media_key_of_item(item("./media/a.mp4")) == "media/a.mp4");
    CHECK(app::media_key_of_item(item("./data/media/a.mp4")) == "media/a.mp4");
    CHECK(app::media_key_of_item(item("media/a.mp4")) == "media/a.mp4");
}

TEST_CASE("build_channels decides which items are scheduled", "[channel_schedule]") {
    auto trimmed = item("media/a.mp4");
    trimmed.start = 10;
    trimmed.end = 70;
    auto no_length = item("media/none.mp4");
    auto backwards = item("media/b.mp4");
    backwards.start = 50;
    backwards.end = 20;
    auto rows = ui_rows({{trimmed, no_length, backwards, item("media/yt", "youtube"),
                          item("rom.nes", "emulated_game"), item("data/media/c.mp4", "video"),
                          item("./media/d.mp4", "local")}});
    auto ch = app::build_channels(
        rows, durations({{"media/a.mp4", 999}, {"media/c.mp4", 30}, {"media/d.mp4", 45}}));
    REQUIRE(ch.size() == 2);
    CHECK(ch[1].number == 1);
    REQUIRE(ch[1].items.size() == 3);
    CHECK(ch[1].items[0].length_s == Approx(60));  // trim wins over duration
    CHECK(ch[1].items[0].playlist == 1);
    CHECK(ch[1].items[0].item == 0);
    CHECK(ch[1].items[1].item == 5);
    CHECK(ch[1].items[1].length_s == Approx(30));
    CHECK(ch[1].items[2].length_s == Approx(45));
    CHECK(ch[1].total_s == Approx(135));
}

TEST_CASE("Master Shuffle holds every item once, in a stable order", "[channel_schedule]") {
    std::vector<app::PlaylistItem> a, b;
    app::MediaInfoMap info;
    for (int i = 0; i < 6; ++i) {
        a.push_back(item("media/a" + std::to_string(i) + ".mp4"));
        info["media/a" + std::to_string(i) + ".mp4"].duration_s = 10 + i;
    }
    for (int i = 0; i < 5; ++i) {
        b.push_back(item("media/b" + std::to_string(i) + ".mp4"));
        info["media/b" + std::to_string(i) + ".mp4"].duration_s = 20 + i;
    }
    auto rows = ui_rows({a, b});
    auto one = app::build_channels(rows, info);
    auto two = app::build_channels(rows, info);
    REQUIRE(one.size() == 3);
    CHECK(one[0].number == 0);
    REQUIRE(one[0].items.size() == 11);
    CHECK(one[0].total_s == Approx(one[1].total_s + one[2].total_s));

    std::set<std::pair<int, int>> seen;
    for (auto& s : one[0].items) seen.insert({s.playlist, s.item});
    CHECK(seen.size() == 11);

    for (size_t i = 0; i < 11; ++i) {
        CHECK(one[0].items[i].playlist == two[0].items[i].playlist);
        CHECK(one[0].items[i].item == two[0].items[i].item);
    }

    // actually shuffled, not just concatenated
    bool in_order = true;
    for (size_t i = 1; i < 11; ++i) {
        auto prev = std::make_pair(one[0].items[i - 1].playlist, one[0].items[i - 1].item);
        auto cur = std::make_pair(one[0].items[i].playlist, one[0].items[i].item);
        if (cur < prev) in_order = false;
    }
    CHECK_FALSE(in_order);

    // adding an item changes the order
    b.push_back(item("media/b9.mp4"));
    info["media/b9.mp4"].duration_s = 99;
    auto three = app::build_channels(ui_rows({a, b}), info);
    REQUIRE(three[0].items.size() == 12);
    bool same = true;
    for (size_t i = 0; i < 11; ++i) {
        if (three[0].items[i].playlist != one[0].items[i].playlist ||
            three[0].items[i].item != one[0].items[i].item)
            same = false;
    }
    CHECK_FALSE(same);
}

TEST_CASE("non-finite times never schedule or loop", "[channel_schedule]") {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    auto c = channel_of({100, 200, 300});
    CHECK_FALSE(app::on_air(c, nan).has_value());
    CHECK_FALSE(app::on_air(c, inf).has_value());
    CHECK(app::lineup(c, E, inf).empty());
    CHECK(app::lineup(c, nan, E + 60).empty());
    CHECK(app::lineup(c, E, nan).empty());
    CHECK(app::lineup(c, -inf, E).empty());
}

TEST_CASE("a channel with no usable lengths is empty", "[channel_schedule]") {
    auto rows = ui_rows({{item("media/x.mp4"), item("media/y.mp4", "youtube")}});
    auto ch = app::build_channels(rows, {});
    REQUIRE(ch.size() == 2);
    CHECK(ch[1].items.empty());
    CHECK_FALSE(app::on_air(ch[1], E + 10).has_value());
    CHECK(app::lineup(ch[1], E, E + 60).empty());
}

TEST_CASE("a mix with nothing to schedule is empty", "[channel_schedule]") {
    auto only_master = app::build_channels(ui_rows({}), {});
    REQUIRE(only_master.size() == 1);
    CHECK(only_master[0].items.empty());
    CHECK(only_master[0].total_s == Approx(0));

    auto none = app::build_channels(ui_rows({{item("media/x.mp4")}, {item("media/z.mp4")}}), {});
    REQUIRE(none.size() == 3);
    CHECK(none[0].items.empty());
    CHECK(none[0].total_s == Approx(0));
}

TEST_CASE("Master Shuffle order is a golden permutation", "[channel_schedule]") {
    // std::mt19937 is fully specified by the standard and the Fisher-Yates is
    // hand-written, so this exact order must hold on the Pi's libstdc++ too.
    app::MediaInfoMap info;
    std::vector<app::PlaylistItem> a, b;
    for (int i = 0; i < 3; ++i) {
        a.push_back(item("media/a" + std::to_string(i) + ".mp4"));
        b.push_back(item("media/b" + std::to_string(i) + ".mp4"));
        info["media/a" + std::to_string(i) + ".mp4"].duration_s = 10;
        info["media/b" + std::to_string(i) + ".mp4"].duration_s = 10;
    }
    auto ch = app::build_channels(ui_rows({a, b}), info);
    std::vector<std::pair<int, int>> got;
    for (auto& s : ch[0].items) got.push_back({s.playlist, s.item});
    const std::vector<std::pair<int, int>> golden = {{2, 1}, {2, 0}, {1, 2}, {1, 1}, {1, 0}, {2, 2}};
    CHECK(got == golden);
}
