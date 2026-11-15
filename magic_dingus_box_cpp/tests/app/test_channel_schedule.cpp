// The Channel Guide schedule (app/channel_schedule.h): a pure function of the
// local broadcast date, with elapsed programme durations between civil boundaries.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <set>
#include <string>
#include <vector>

#include "app/channel_schedule.h"

using Catch::Approx;

namespace {

constexpr double E = 1767240000.0;  // 2026-01-01 04:00 UTC, broadcast day 20454

app::PlaylistItem item(const std::string& path, const std::string& type = "") {
    app::PlaylistItem it;
    it.path = path;
    it.source_type = type;
    return it;
}

app::Channel channel_of(std::vector<double> lengths) {
    app::Channel c;
    c.identity = "fixture3";  // day 20454 rotates 3- and 4-item fixtures from index 0
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

TEST_CASE("on_air walks the written order within a broadcast day", "[channel_schedule]") {
    auto c = channel_of({100, 200, 300});
    CHECK(app::on_air(c, E + 50, 0)->index == 0);
    CHECK(app::on_air(c, E + 50, 0)->offset_s == Approx(50));
    CHECK(app::on_air(c, E + 350, 0)->index == 2);
    CHECK(app::on_air(c, E + 350, 0)->offset_s == Approx(50));
    CHECK(app::on_air(c, E + 650, 0)->starts_at == Approx(E + 600));
    CHECK(app::on_air(c, E + 650, 0)->ends_at == Approx(E + 700));
}

TEST_CASE("lineup lists items overlapping the window", "[channel_schedule]") {
    auto c = channel_of({100, 200, 300});
    auto L = app::lineup(c, E + 50, E + 400, 0);
    REQUIRE(L.size() == 3);
    CHECK(L[0].index == 0);
    CHECK(L[1].starts_at == Approx(E + 100));
    CHECK(L[2].index == 2);
    CHECK(app::lineup(c, E + 50, E + 50, 0).empty());
    // wraps past the end of the loop
    auto W = app::lineup(c, E + 550, E + 650, 0);
    REQUIRE(W.size() == 2);
    CHECK(W[1].index == 0);
    CHECK(W[1].starts_at == Approx(E + 600));
}

TEST_CASE("empty channel has nothing on air", "[channel_schedule]") {
    CHECK_FALSE(app::on_air(app::Channel{}, E, 0).has_value());
    CHECK(app::lineup(app::Channel{}, E, E + 60, 0).empty());
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

TEST_CASE("items shorter than 3 s are left out of the schedule", "[channel_schedule]") {
    // A sub-second item would inflate the guide's lineup (thousands of
    // slots per layout) and could never confirm its own playback.
    auto short_trim = item("media/t.mp4");
    short_trim.start = 10;
    short_trim.end = 12.9;  // 2.9 s
    auto exact = item("media/e.mp4");
    exact.start = 1;
    exact.end = 4;  // 3 s: kept
    auto rows = ui_rows({{short_trim, item("media/blip.mp4"), exact, item("media/ok.mp4"),
                          item("media/tail.mp4")}});
    // tail.mp4 is 20 s long but starts 18 s in: 2 s playable.
    rows[1].items[4].start = 18;
    auto ch = app::build_channels(rows, durations({{"media/t.mp4", 99},
                                                   {"media/blip.mp4", 0.4},
                                                   {"media/ok.mp4", 30},
                                                   {"media/tail.mp4", 20}}));
    REQUIRE(ch[1].items.size() == 2);
    CHECK(ch[1].items[0].item == 2);
    CHECK(ch[1].items[0].length_s == Approx(3));
    CHECK(ch[1].items[1].item == 3);
    CHECK(ch[1].total_s == Approx(33));
    CHECK(ch[0].items.size() == 2);  // the mix takes only what the rows scheduled
    CHECK(app::kMinScheduledItemS == 3.0);
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

    // Written row order is retained; the day's order is applied at scheduling time.
    for (int i = 0; i < 6; ++i) {
        CHECK(one[0].items[i].playlist == 1);
        CHECK(one[0].items[i].item == i);
    }
    CHECK(one[0].items[6].playlist == 2);
    CHECK(one[0].items[6].item == 0);
}

TEST_CASE("non-finite times never schedule or loop", "[channel_schedule]") {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    auto c = channel_of({100, 200, 300});
    CHECK_FALSE(app::on_air(c, nan, 0).has_value());
    CHECK_FALSE(app::on_air(c, inf, 0).has_value());
    CHECK(app::lineup(c, E, inf, 0).empty());
    CHECK(app::lineup(c, nan, E + 60, 0).empty());
    CHECK(app::lineup(c, E, nan, 0).empty());
    CHECK(app::lineup(c, -inf, E, 0).empty());
}

TEST_CASE("a channel with no usable lengths is empty", "[channel_schedule]") {
    auto rows = ui_rows({{item("media/x.mp4"), item("media/y.mp4", "youtube")}});
    auto ch = app::build_channels(rows, {});
    REQUIRE(ch.size() == 2);
    CHECK(ch[1].items.empty());
    CHECK_FALSE(app::on_air(ch[1], E + 10, 0).has_value());
    CHECK(app::lineup(ch[1], E, E + 60, 0).empty());
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

TEST_CASE("join_point: the on-air item, or the next from its start near the end",
          "[channel_schedule]") {
    auto c = channel_of({100, 200, 300});
    auto j = app::join_point(c, E + 130, 0);
    REQUIRE(j);
    CHECK(j->index == 1);
    CHECK(j->offset_s == Approx(30));
    // 2 s left is enough; under it, the next item from its start.
    j = app::join_point(c, E + 298, 0);
    CHECK(j->index == 1);
    CHECK(j->offset_s == Approx(198));
    j = app::join_point(c, E + 298.5, 0);
    CHECK(j->index == 2);
    CHECK(j->offset_s == 0.0);
    CHECK(j->starts_at == Approx(E + 300));
    CHECK(j->ends_at == Approx(E + 600));
    // The last item's tail wraps to the first.
    j = app::join_point(c, E + 599, 0);
    CHECK(j->index == 0);
    CHECK(j->starts_at == Approx(E + 600));
    CHECK_FALSE(app::join_point(app::Channel{}, E, 0).has_value());
}

TEST_CASE("airing_after: the next item from its start, wrapping", "[channel_schedule]") {
    auto c = channel_of({100, 200, 300});
    auto a = *app::on_air(c, E + 450, 0);
    auto b = app::airing_after(c, a, 0);
    REQUIRE(b);
    CHECK(b->index == 0);
    CHECK(b->offset_s == 0.0);
    CHECK(b->starts_at == Approx(E + 600));
    CHECK(b->ends_at == Approx(E + 700));
}

TEST_CASE("boundary_join never goes back to the airing last loaded", "[channel_schedule]") {
    auto c = channel_of({100, 200, 300});
    // b's airing, 100-300 s.
    app::LoadedAiring b;
    b.index = 1;
    b.item = c.items[1];
    b.starts_at = E + 100;
    b.ends_at = E + 300;

    SECTION("no airing loaded: join_point") {
        auto j = app::boundary_join(c, E + 290, std::nullopt, 0);
        CHECK(j->index == 1);
        CHECK(j->offset_s == Approx(190));
    }
    SECTION("ahead, inside the loaded airing: the one after it, from its start") {
        auto j = app::boundary_join(c, E + 290, b, 0);
        CHECK(j->index == 2);
        CHECK(j->offset_s == 0.0);
        CHECK(j->starts_at == Approx(E + 300));
    }
    SECTION("further ahead, inside an EARLIER airing: still the one after the loaded") {
        auto j = app::boundary_join(c, E + 50, b, 0);  // a is on
        CHECK(j->index == 2);
        CHECK(j->offset_s == 0.0);
        CHECK(j->starts_at == Approx(E + 300));
    }
    SECTION("on time (the boundary fires just before the end): the next, from its start") {
        auto j = app::boundary_join(c, E + 299.6, b, 0);
        CHECK(j->index == 2);
        CHECK(j->offset_s == 0.0);
    }
    SECTION("behind: whatever is on now, at its offset") {
        auto j = app::boundary_join(c, E + 300.4, b, 0);
        CHECK(j->index == 2);
        CHECK(j->offset_s == Approx(0.4));
        j = app::boundary_join(c, E + 750, b, 0);  // the next cycle's b, a new airing
        CHECK(j->index == 1);
        CHECK(j->offset_s == Approx(50));
    }
    SECTION("an identical rebuild (a crop-only media info change) keeps the guard") {
        const app::Channel same = c;  // a new list, the same schedule
        auto j = app::boundary_join(same, E + 290, b, 0);
        CHECK(j->index == 2);
        CHECK(j->offset_s == 0.0);
    }
    SECTION("a rebuilt channel that moved the loaded item: plain join_point") {
        auto rebuilt = channel_of({100, 200, 300});
        rebuilt.items[1].item = 7;  // index 1 is another item now
        auto j = app::boundary_join(rebuilt, E + 290, b, 0);
        CHECK(j->index == 1);
        CHECK(j->offset_s == Approx(190));
        app::LoadedAiring out_of_range = b;
        out_of_range.index = 9;
        j = app::boundary_join(c, E + 290, out_of_range, 0);
        CHECK(j->index == 1);
    }
}

TEST_CASE("boundary_join resyncs after a rebuild that re-phased the channel",
          "[channel_schedule]") {
    // Within a broadcast day a rebuild that changes
    // the channel's cycle (an upload to its playlist, a trim edit, the
    // duration back-fill) moves every airing — the loaded item keeps its
    // index, but the live airing now usually started before the loaded one
    // ended. That is NOT playback running ahead: the boundary must join the
    // live airing, or the channel stays out of phase until it is re-tuned.
    // Times are deliberately NOT multiples of both cycles (1800 s, 2220 s).
    auto old_ch = channel_of({600, 600, 600});
    const auto loaded = app::on_air(old_ch, E + 12345, 0);  // item 2, 12000-12600
    REQUIRE(loaded);
    REQUIRE(loaded->index == 2);
    app::LoadedAiring last;
    last.index = loaded->index;
    last.item = old_ch.items[2];
    last.starts_at = loaded->starts_at;
    last.ends_at = loaded->ends_at;

    auto grown = channel_of({600, 600, 600, 420});  // a 4th item, 420 s
    REQUIRE(grown.items[2].item == last.item.item);  // the same item, same index
    // The loaded item's natural end, as the boundary fires.
    const double end = last.ends_at - 0.4;
    const auto live = app::join_point(grown, end, 0);
    REQUIRE(live);
    CHECK(live->index == 2);
    CHECK(live->offset_s == Approx(299.6));  // the re-phased item 2, 299.6 s in
    auto j = app::boundary_join(grown, end, last, 0);
    REQUIRE(j);
    CHECK(j->index == live->index);
    CHECK(j->offset_s == Approx(live->offset_s));
    CHECK(j->starts_at == Approx(live->starts_at));

    // And every later boundary stays in phase: each load is recorded from
    // what it joined, which is now the live schedule's own airing.
    app::OnAir cur = *j;
    for (int k = 0; k < 8; ++k) {
        last.index = cur.index;
        last.item = grown.items[cur.index];
        last.starts_at = cur.starts_at;
        last.ends_at = cur.ends_at;
        const double t = cur.ends_at - 0.4;
        const auto next = app::boundary_join(grown, t, last, 0);
        const auto want = app::join_point(grown, t, 0);
        REQUIRE(next);
        CHECK(next->index == want->index);
        CHECK(next->starts_at == Approx(want->starts_at));
        cur = *next;
    }
}

TEST_CASE("movie playlist identities are lexical and their exact trims schedule without media info", "[channel_schedule]") {
    app::Playlist movies;
    movies.path = "movies:mix";
    app::PlaylistItem film;
    film.path = "/host/movies/film.mp4";
    film.source_type = "local";
    film.movie_tmdb_id = 42;
    film.end = 4863.25;
    movies.items.push_back(film);
    CHECK(app::is_movie_playlist(movies));
    app::Playlist ordinary;
    ordinary.path = "data/playlists/movies:mix.yaml";
    CHECK_FALSE(app::is_movie_playlist(ordinary));
    ordinary.path = "movies";
    CHECK_FALSE(app::is_movie_playlist(ordinary));
    ordinary.path.clear();
    CHECK_FALSE(app::is_movie_playlist(ordinary));
    const auto channels = app::build_channels({{}, movies}, {});
    REQUIRE(channels.size() == 2);
    REQUIRE(channels[1].items.size() == 1);
    CHECK(channels[1].items[0].length_s == 4863.25);
}

TEST_CASE("daily programmes are cut at 4 AM instead of continuing an epoch loop", "[channel_schedule][daily_red]") {
    auto ch = channel_of({100000});
    // 2026-01-02 03:59 UTC belongs to the Jan 1 broadcast day.
    const auto a = app::on_air(ch, 1767326340.0, 0);
    REQUIRE(a);
    CHECK(a->ends_at == 1767326400.0);
}

TEST_CASE("Master Shuffle excludes movie playlist programmes", "[channel_schedule][daily_red]") {
    auto rows = ui_rows({{item("clip.mp4")}, {item("film.mp4")}});
    rows[2].path = "movies:mix";
    const auto channels = app::build_channels(rows, durations({{"clip.mp4", 60}, {"film.mp4", 7200}}));
    REQUIRE(channels[0].items.size() == 1);
    CHECK(channels[0].items[0].playlist == 1);
}

TEST_CASE("daily Rotate preserves written succession and varies its first programme", "[channel_schedule]") {
    auto ch = channel_of({10, 10, 10, 10, 10, 10, 10});
    std::set<int> firsts;
    int changed = 0;
    int previous = -1;
    for (int day = 20454; day < 20464; ++day) {
        const auto order = app::day_order(ch, day, 0);
        REQUIRE(order.size() == 7);
        firsts.insert(order.front());
        if (previous >= 0 && previous != order.front()) ++changed;
        previous = order.front();
        for (int i = 1; i < 7; ++i) CHECK(order[i] == (order[i - 1] + 1) % 7);
        CHECK(app::day_order(ch, day, 8) == order);
    }
    CHECK(firsts.size() > 3);
    CHECK(changed >= 6);
}

TEST_CASE("Shuffle reshuffles complete permutations each pass", "[channel_schedule]") {
    auto ch = channel_of({10, 10, 10, 10, 10, 10});
    ch.order = app::LineupOrder::Shuffle;
    std::set<std::vector<int>> passes;
    for (int pass = 0; pass < 5; ++pass) {
        auto order = app::day_order(ch, 20454, pass);
        passes.insert(order);
        std::set<int> indices(order.begin(), order.end());
        CHECK(indices == std::set<int>{0, 1, 2, 3, 4, 5});
        const auto first = app::on_air(ch, E + pass * 60, 0);
        REQUIRE(first);
        CHECK(first->index == order.front());
        CHECK(first->offset_s == 0);
    }
    CHECK(passes.size() > 1);
}

TEST_CASE("day orders are portable golden permutations", "[channel_schedule]") {
    auto ch = channel_of({10, 10, 10, 10, 10, 10, 10});
    ch.identity = "path:/golden.yaml";
    CHECK(app::day_order(ch, 20405, 0) == std::vector<int>{5, 6, 0, 1, 2, 3, 4});
    CHECK(app::day_order(ch, 20405, 1) == std::vector<int>{5, 6, 0, 1, 2, 3, 4});
    ch.order = app::LineupOrder::Shuffle;
    CHECK(app::day_order(ch, 20405, 0) == std::vector<int>{2, 0, 4, 3, 5, 1, 6});
    CHECK(app::day_order(ch, 20405, 1) == std::vector<int>{3, 5, 2, 0, 6, 1, 4});
}

TEST_CASE("the broadcast date changes at local 4 AM and not midnight", "[channel_schedule]") {
    auto ch = channel_of({100000, 100000, 100000});
    for (int offset : {-8 * 3600, 19800}) {
        CAPTURE(offset);
        const double start = E - offset;
        const auto before = app::on_air(ch, start + 86400 - 1, offset);
        const auto after = app::on_air(ch, start + 86400, offset);
        REQUIRE(before); REQUIRE(after);
        CHECK(before->ends_at == start + 86400);
        CHECK(after->starts_at == start + 86400);
        CHECK(after->offset_s == 0);
        CHECK(after->index == app::day_order(ch, 20455, 0).front());
        const auto midnight_before = app::on_air(ch, start + 20 * 3600 - 1, offset);
        const auto midnight_after = app::on_air(ch, start + 20 * 3600 + 1, offset);
        REQUIRE(midnight_before); REQUIRE(midnight_after);
        CHECK(midnight_before->starts_at == midnight_after->starts_at);
        const auto repeated = app::on_air(ch, start + 123, offset);
        REQUIRE(repeated);
        CHECK(repeated->starts_at == start);
        CHECK(repeated->offset_s == 123);
        CHECK(app::on_air(ch, start + 123, offset)->index == repeated->index);
        app::LoadedAiring loaded{before->index, ch.items[before->index], before->starts_at, before->ends_at, offset};
        const auto joined = app::boundary_join(ch, start + 86400, loaded, offset);
        REQUIRE(joined);
        CHECK(joined->starts_at == start + 86400);
    }
    CHECK(app::on_air(ch, E - 1, 0)->starts_at == E - 86400);
    CHECK(app::on_air(ch, E - 1, 3600)->starts_at == E - 3600);
    // floor, not truncation, for dates before Unix epoch.
    CHECK(app::on_air(ch, -1, 0)->starts_at == -72000);
}

TEST_CASE("movie channels stop at 1 AM and resume at 6 AM", "[channel_schedule]") {
    auto ch = channel_of({100000});
    ch.day_start_h = app::kMovieDayStartH;
    ch.on_air_h = app::kMovieOnAirH;
    const double start = E + 2 * 3600;  // Jan 1 06:00
    const double close = start + 19 * 3600;  // Jan 2 01:00
    const double reopen = start + 86400;
    const auto last = app::on_air(ch, close - 1, 0);
    REQUIRE(last);
    CHECK(last->ends_at == close);
    CHECK_FALSE(app::airing_after(ch, *last, 0));
    CHECK_FALSE(app::join_point(ch, close - 1, 0));  // no new load in its final second
    for (double now : {close, reopen - 1}) {
        CHECK_FALSE(app::on_air(ch, now, 0));
        CHECK_FALSE(app::join_point(ch, now, 0));
        REQUIRE(app::back_on_air_at(ch, now, 0));
        CHECK(*app::back_on_air_at(ch, now, 0) == reopen);
    }
    CHECK(app::lineup(ch, close, reopen, 0).empty());
    const auto window = app::lineup(ch, close - 1, reopen + 1, 0);
    REQUIRE(window.size() == 2);
    CHECK(window[0].ends_at == close);
    CHECK(window[1].starts_at == reopen);
    CHECK_FALSE(app::back_on_air_at(ch, reopen, 0));
    CHECK_FALSE(app::back_on_air_at(app::Channel{}, close, 0));
}

TEST_CASE("channel construction assigns identities and daily rules", "[channel_schedule]") {
    auto rows = ui_rows({{item("clip.mp4")}, {item("film.mp4")}});
    rows[1].path = "/golden.yaml";
    rows[2].path = "movies:kids";
    const auto ch = app::build_channels(rows, durations({{"clip.mp4", 60}, {"film.mp4", 7200}}));
    CHECK(ch[0].identity == "shuffle");
    CHECK(ch[0].order == app::LineupOrder::Shuffle);
    CHECK(ch[1].identity == "path:/golden.yaml");
    CHECK(ch[1].order == app::LineupOrder::Rotate);
    CHECK(ch[1].day_start_h == 4);
    CHECK(ch[1].on_air_h == 24);
    CHECK(ch[2].identity == "movies:kids");
    CHECK(ch[2].order == app::LineupOrder::Shuffle);
    CHECK(ch[2].day_start_h == 6);
    CHECK(ch[2].on_air_h == 19);
}

TEST_CASE("DST uses elapsed durations between real civil day boundaries", "[channel_schedule][dst]") {
    auto ch = channel_of({200000});
    // Synthetic zone: spring forward Jan 2 at 02:00 UTC, 02:00 -> 03:00 local.
    const double jump = E + 22 * 3600;
    const app::UtcOffsetAt spring = [=](double utc) { return utc < jump ? 0 : 3600; };
    auto before = app::on_air(ch, jump - 1, 0, spring);
    auto after = app::on_air(ch, jump, 3600, spring);
    REQUIRE(before); REQUIRE(after);
    CHECK(before->starts_at == E);
    CHECK(after->starts_at == E);  // the current offset must not move the day's start
    CHECK(after->offset_s == 22 * 3600);
    CHECK(after->ends_at == E + 23 * 3600);
    const auto next = app::airing_after(ch, *after, 3600, spring);
    REQUIRE(next);
    CHECK(next->starts_at == E + 23 * 3600);
    CHECK(next->offset_s == 0);
    // Real 24-hour lookahead reaches beyond the next local 4 AM on a short day.
    const auto ahead = app::lineup(ch, E, E + 86400, 0, spring);
    REQUIRE(ahead.size() == 2);
    CHECK(ahead[0].ends_at == E + 23 * 3600);
    CHECK(ahead[1].starts_at == E + 23 * 3600);
    const app::UtcOffsetAt fall = [=](double utc) { return utc < jump ? 3600 : 0; };
    const auto long_day = app::on_air(ch, jump + 1, 0, fall);
    REQUIRE(long_day);
    CHECK(long_day->starts_at == E - 3600);
    CHECK(long_day->ends_at == E + 86400);
    CHECK(long_day->ends_at - long_day->starts_at == 25 * 3600);
    CHECK(app::lineup(ch, E - 3600, E + 23 * 3600, 3600, fall).size() == 1);
}

TEST_CASE("movie fall-back closes at the first 1 AM and stays closed", "[channel_schedule][dst]") {
    auto ch = channel_of({200000});
    ch.day_start_h = 6;
    ch.on_air_h = 19;
    const double midnight = E + 20 * 3600;  // Jan 2 00:00 UTC = first 01:00 local
    const double rollback = midnight + 3600;  // 02:00 -> 01:00
    const app::UtcOffsetAt fall = [=](double utc) { return utc < rollback ? 3600 : 0; };
    const auto last = app::on_air(ch, midnight - 1, 3600, fall);
    REQUIRE(last);
    CHECK(last->ends_at == midnight);
    for (double now : {midnight, rollback, rollback + 1800, midnight + 5 * 3600}) {
        CHECK_FALSE(app::on_air(ch, now, fall(now), fall));
        CHECK(app::back_on_air_at(ch, now, fall(now), fall) == midnight + 6 * 3600);
    }
    CHECK(app::lineup(ch, midnight, midnight + 6 * 3600, 3600, fall).empty());
}

TEST_CASE("a skipped civil day boundary takes effect at the first valid instant", "[channel_schedule][dst]") {
    auto ch = channel_of({200000});
    // At local 03:00 skip directly to 05:00: there is no 04:00 that day.
    const double jump = E + 23 * 3600;
    const app::UtcOffsetAt zone = [=](double utc) { return utc < jump ? 0 : 7200; };
    const auto old_day = app::on_air(ch, jump - 1, 0, zone);
    const auto new_day = app::on_air(ch, jump, 7200, zone);
    REQUIRE(old_day); REQUIRE(new_day);
    CHECK(old_day->ends_at == jump);
    CHECK(new_day->starts_at == jump);
    CHECK(new_day->offset_s == 0);
}

TEST_CASE("fractional durations keep the lineup contiguous and every successor advances", "[channel_schedule][fractional]") {
    auto ch = channel_of({3.1, 4.2, 5.3});
    const auto programmes = app::lineup(ch, E, E + 120, 0);
    REQUIRE(programmes.size() > 20);
    double covered = E;
    for (const auto& a : programmes) {
        CHECK(a.starts_at == Approx(covered));
        REQUIRE(a.ends_at > covered);
        const auto next = app::airing_after(ch, a, 0);
        REQUIRE(next);
        CHECK(next->starts_at == Approx(a.ends_at));
        CHECK(next->offset_s == Approx(0).margin(1e-6));
        CHECK(next->index == (a.index + 1) % 3);
        covered = a.ends_at;
    }
    CHECK(covered >= E + 120);
}
