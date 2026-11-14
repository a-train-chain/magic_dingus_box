#include <catch2/catch_test_macros.hpp>
#include <limits>
#include "app/channel_schedule.h"
#include "media_browser/channels/movie_channels.h"

namespace {
app::LibraryMovie film(int id, const std::string& title = "Film",
                       std::vector<std::string> genres = {},
                       const std::string& cert = "", int year = 2000) {
    return {id, title, year, std::move(genres), cert, "/movies/" + std::to_string(id) + ".mp4", 90.5, "x264"};
}
const app::Playlist* channel(const std::vector<app::Playlist>& channels, const std::string& path) {
    for (const auto& c : channels) if (c.path == path) return &c;
    return nullptr;
}
std::vector<int> ids(const app::Playlist& playlist) {
    std::vector<int> result;
    for (const auto& item : playlist.items) result.push_back(item.movie_tmdb_id);
    return result;
}
}

TEST_CASE("movie channels require at least three eligible films", "[movie_channels]") {
    CHECK(app::build_movie_playlists({}, {}).empty());
    CHECK(app::build_movie_playlists({film(1), film(2)}, {}).empty());
    auto channels = app::build_movie_playlists({film(1, "B", {"Comedy"}), film(2, "C", {"comedy"}), film(3)}, {});
    REQUIRE(channels.size() == 1);
    CHECK(channels[0].title == "MOVIE MIX");
    CHECK(channels[0].path == "movies:mix");
    channels = app::build_movie_playlists({film(1, "B", {"Comedy"}), film(2, "C", {"comedy"}), film(3, "D", {"COMEDY"})}, {});
    REQUIRE(channels.size() == 2);
    CHECK(channels[1].path == "movies:comedy");
}

TEST_CASE("Kids requires safe certification and a family genre", "[movie_channels]") {
    const auto channels = app::build_movie_playlists({
        film(1, "A", {"Family"}, "G"), film(2, "B", {"animation"}, "PG"), film(3, "C", {"FAMILY"}, "pg"),
        film(4, "D", {"Family"}, "PG-13"), film(5, "E", {"Animation"}, "NR"),
        film(6, "F", {"Animation"}, ""), film(7, "G", {"Family"}, "R"), film(8, "H", {"Drama"}, "G")}, {});
    const auto* kids = channel(channels, "movies:kids");
    REQUIRE(kids);
    CHECK(kids->title == "KIDS");
    CHECK(ids(*kids) == std::vector<int>{1, 2, 3});
}

TEST_CASE("Classics excludes unknown years and includes only years before 1980", "[movie_channels]") {
    const auto channels = app::build_movie_playlists({film(1, "A", {}, "", 1979), film(2, "B", {}, "", 1920),
        film(3, "C", {}, "", 1970), film(4, "D", {}, "", 1980), film(5, "E", {}, "", 0)}, {});
    const auto* classics = channel(channels, "movies:classics");
    REQUIRE(classics);
    CHECK(ids(*classics) == std::vector<int>{1, 2, 3});
}

TEST_CASE("genre rules accept every specified genre case insensitively", "[movie_channels]") {
    struct Rule { const char* genre; const char* path; const char* title; };
    for (const auto& rule : std::vector<Rule>{
        {"cOmEdY", "movies:comedy", "COMEDY"}, {"drama", "movies:drama", "DRAMA"},
        {"Action", "movies:action", "ACTION & ADVENTURE"}, {"Adventure", "movies:action", "ACTION & ADVENTURE"},
        {"Science Fiction", "movies:scifi", "SCI-FI"}, {"Fantasy", "movies:scifi", "SCI-FI"},
        {"Thriller", "movies:thrillers", "THRILLERS & CRIME"}, {"Crime", "movies:thrillers", "THRILLERS & CRIME"},
        {"Mystery", "movies:thrillers", "THRILLERS & CRIME"}, {"horror", "movies:horror", "HORROR"},
        {"Romance", "movies:romance", "ROMANCE"}, {"War", "movies:war", "WAR & HISTORY"},
        {"History", "movies:war", "WAR & HISTORY"}, {"Western", "movies:war", "WAR & HISTORY"}}) {
        INFO(rule.genre);
        const auto channels = app::build_movie_playlists({film(1, "A", {rule.genre}), film(2, "B", {rule.genre}), film(3, "C", {rule.genre})}, {});
        REQUIRE(channels.size() == 2);
        CHECK(channels[1].path == rule.path);
        CHECK(channels[1].title == rule.title);
    }
}

TEST_CASE("movies appear in every qualifying channel with stable title and ID order", "[movie_channels]") {
    const auto channels = app::build_movie_playlists({film(9, "Z", {"Comedy", "Drama"}),
        film(7, "A", {"Comedy", "Drama"}), film(3, "A", {"Comedy", "Drama"}, "", 0)}, {});
    REQUIRE(channels.size() == 3);
    for (const auto& c : channels) CHECK(ids(c) == std::vector<int>{3, 7, 9});
    const auto& item = channels[0].items[0];
    CHECK(item.path == "/movies/3.mp4");
    CHECK(item.source_type == "local");
    CHECK(item.title == "A");
    CHECK(item.artist.empty());
    CHECK(item.start == 0.0);
    CHECK(item.end == 90.5);
    CHECK(channels[0].items[1].artist == "2000");
}

TEST_CASE("eight channel limit keeps mix and the fullest seven then restores table order", "[movie_channels]") {
    // Ten qualify (all except War): four children + four horror films make
    // those full; the other seven candidates tie at three, table order wins.
    const std::vector<std::string> genres{"Family", "Comedy", "Drama", "Action", "Fantasy", "Thriller", "Horror", "Romance"};
    const auto channels = app::build_movie_playlists({film(1, "A", genres, "G", 1979),
        film(2, "B", genres, "G", 1979), film(3, "C", genres, "G", 1979),
        film(4, "D", {"Family"}, "PG"), film(5, "E", {"Horror"})}, {});
    REQUIRE(channels.size() == 8);
    std::vector<std::string> paths;
    for (const auto& c : channels) paths.push_back(c.path);
    CHECK(paths == std::vector<std::string>{"movies:mix", "movies:kids", "movies:classics", "movies:comedy", "movies:drama", "movies:action", "movies:scifi", "movies:horror"});
    CHECK(channels[1].items.size() == 4);
    CHECK(channels[7].items.size() == 4);
}

TEST_CASE("codec policy permits unknown codecs and compares known codecs without case", "[movie_channels]") {
    const std::vector<std::string> allowed{"x264", "h264", "avc"};
    for (const auto* codec : {"X264", "H264", "AvC", ""}) CHECK(app::codec_allowed(allowed, codec));
    for (const auto* codec : {"x265", "HEVC", "AV1"}) CHECK_FALSE(app::codec_allowed(allowed, codec));
    CHECK(app::codec_allowed({}, "AV1"));
    CHECK(app::codec_allowed({"X264"}, "x264"));
    auto a = film(1), b = film(2), c = film(3), d = film(4), e = film(5), f = film(6);
    a.codec = "X264"; b.codec = ""; c.codec = "avc"; d.codec = "x265"; e.codec = "HEVC"; f.codec = "AV1";
    const auto channels = app::build_movie_playlists({a, b, c, d, e, f}, allowed);
    REQUIRE(channels.size() == 1);
    CHECK(ids(channels[0]) == std::vector<int>{1, 2, 3});
}

TEST_CASE("invalid library entries cannot make a channel qualify", "[movie_channels]") {
    auto short_film = film(4), empty_path = film(5), zero_id = film(0), negative_id = film(-1), nan = film(6), infinite = film(7);
    short_film.length_s = 2.99;
    empty_path.path.clear();
    nan.length_s = std::numeric_limits<double>::quiet_NaN();
    infinite.length_s = std::numeric_limits<double>::infinity();
    CHECK(app::build_movie_playlists({film(1), film(2), short_film, empty_path, zero_id, negative_id, nan, infinite}, {}).empty());
    auto boundary = film(3);
    boundary.length_s = 3.0;
    const auto channels = app::build_movie_playlists({film(1), film(2), boundary}, {});
    REQUIRE(channels.size() == 1);
    CHECK(channels[0].items.size() == 3);
    CHECK(channels[0].items[2].end == 3.0);
}
