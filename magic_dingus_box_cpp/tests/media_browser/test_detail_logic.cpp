// Table tests for detail_logic.h — the movie Detail screen's decisions
// (modes and button rows, profile pick, disk warning, fetch / poll / worker
// drain verdicts, and the page's composed text), moved out of
// detail_screen.cpp so they run on the Mac.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "media_browser/ui/detail_logic.h"

namespace mb = media_browser;
using namespace media_browser::ui;

namespace {

const std::string kDash = "\xE2\x80\x94";
const std::string kBullet = "  \xE2\x80\xA2  ";

std::vector<DetailAction> actions(const std::vector<DetailButton>& b) {
    std::vector<DetailAction> out;
    for (const auto& x : b) out.push_back(x.action);
    return out;
}

mb::Movie movie(int tmdb_id, int radarr_id, bool has_file) {
    mb::Movie m;
    m.tmdb_id = tmdb_id;
    m.radarr_id = radarr_id;
    m.has_file = has_file;
    return m;
}

mb::QueueItem qitem(int movie_id, const std::string& tracked) {
    mb::QueueItem q;
    q.movie_id = movie_id;
    q.tracked_download_state = tracked;
    return q;
}

}  // namespace

TEST_CASE("detail: loaded-data cache test", "[detail_logic]") {
    CHECK_FALSE(detail_has_loaded_data(DetailMode::Loading));
    CHECK_FALSE(detail_has_loaded_data(DetailMode::Error));
    CHECK_FALSE(detail_has_loaded_data(DetailMode::NoTmdb));
    CHECK(detail_has_loaded_data(DetailMode::NotInLibrary));
    CHECK(detail_has_loaded_data(DetailMode::InLibraryNoFile));
    CHECK(detail_has_loaded_data(DetailMode::InLibraryWithFile));
}

TEST_CASE("detail: button row per mode", "[detail_logic]") {
    using A = DetailAction;
    CHECK(decide_detail_buttons(DetailMode::Loading, false, true).empty());
    CHECK(decide_detail_buttons(DetailMode::NoTmdb, true, true).empty());
    CHECK(actions(decide_detail_buttons(DetailMode::Error, false, false)) ==
          std::vector<A>{A::Retry});
    CHECK(actions(decide_detail_buttons(DetailMode::NotInLibrary, true, true)) ==
          std::vector<A>{A::AddToLibrary, A::MoreInfo});
    CHECK(actions(decide_detail_buttons(DetailMode::InLibraryNoFile, false, false)) ==
          std::vector<A>{A::SearchAgain, A::PickSource, A::Remove});
    CHECK(actions(decide_detail_buttons(DetailMode::InLibraryNoFile, true, false)) ==
          std::vector<A>{A::SearchAgain, A::PickSource, A::ConfirmRemove});
    // Play only when the file is truly ready.
    CHECK(actions(decide_detail_buttons(DetailMode::InLibraryWithFile, false, true)) ==
          std::vector<A>{A::Play, A::PickSource, A::Remove});
    CHECK(actions(decide_detail_buttons(DetailMode::InLibraryWithFile, true, false)) ==
          std::vector<A>{A::PickSource, A::ConfirmRemove});

    const auto row = decide_detail_buttons(DetailMode::InLibraryWithFile, true, true);
    CHECK(row[0].label == "Play");
    CHECK(row[1].label == "Pick a source");
    CHECK(row[2].label == "Confirm Remove");
    CHECK(decide_detail_buttons(DetailMode::NotInLibrary, false, false)[0].label ==
          "Add to Library");
}

TEST_CASE("detail: focus clamp", "[detail_logic]") {
    CHECK(clamp_detail_focus(-2, 3) == 0);
    CHECK(clamp_detail_focus(5, 3) == 2);
    CHECK(clamp_detail_focus(1, 3) == 1);
    CHECK(clamp_detail_focus(4, 0) == 4);  // empty row leaves it alone
    CHECK(clamp_detail_focus(-1, 0) == 0);
}

TEST_CASE("detail: button kinds", "[detail_logic]") {
    using K = chrome::ButtonKind;
    CHECK(detail_button_kind(DetailAction::Play) == K::Ok);
    CHECK(detail_button_kind(DetailAction::AddToLibrary) == K::Ok);
    CHECK(detail_button_kind(DetailAction::SearchAgain) == K::Action);
    CHECK(detail_button_kind(DetailAction::Retry) == K::Action);
    CHECK(detail_button_kind(DetailAction::PickSource) == K::Action);
    CHECK(detail_button_kind(DetailAction::Remove) == K::Warn);
    CHECK(detail_button_kind(DetailAction::ConfirmRemove) == K::Warn);
    CHECK(detail_button_kind(DetailAction::MoreInfo) == K::Neutral);
}

TEST_CASE("detail: SELECT gate", "[detail_logic]") {
    CHECK(decide_detail_activate(true, true, 3, 0) == DetailActivateGate::Removing);
    CHECK(decide_detail_activate(false, true, 3, 0) == DetailActivateGate::Adding);
    CHECK(decide_detail_activate(false, false, 0, 0) == DetailActivateGate::Nothing);
    CHECK(decide_detail_activate(false, false, 3, 3) == DetailActivateGate::Nothing);
    CHECK(decide_detail_activate(false, false, 3, -1) == DetailActivateGate::Nothing);
    CHECK(decide_detail_activate(false, false, 3, 2) == DetailActivateGate::Dispatch);
}

TEST_CASE("detail: quality profile pick order", "[detail_logic]") {
    std::vector<mb::QualityProfile> ps;
    CHECK(pick_movie_quality_profile_id(ps) == 0);
    ps.push_back({1, "SD", 0, {}});
    CHECK(pick_movie_quality_profile_id(ps) == 1);  // first as last resort
    ps.push_back({2, "Ultra 1080p+", 0, {}});
    CHECK(pick_movie_quality_profile_id(ps) == 2);  // contains "1080p"
    ps.push_back({3, "HD-1080p", 0, {}});
    CHECK(pick_movie_quality_profile_id(ps) == 3);
    ps.push_back({4, "HD - 720p/1080p", 0, {}});
    CHECK(pick_movie_quality_profile_id(ps) == 4);
    ps.push_back({5, "Any", 0, {}});
    CHECK(pick_movie_quality_profile_id(ps) == 5);
}

TEST_CASE("detail: low-space warning", "[detail_logic]") {
    constexpr std::uintmax_t kGiB = 1024ULL * 1024 * 1024;
    CHECK_FALSE(low_space_warning(std::nullopt).has_value());
    CHECK_FALSE(low_space_warning(0).has_value());
    CHECK_FALSE(low_space_warning(15 * kGiB).has_value());
    CHECK_FALSE(low_space_warning(400 * kGiB).has_value());
    const auto w = low_space_warning(15 * kGiB - 1);
    REQUIRE(w.has_value());
    CHECK(*w == "Warning: only 14 GB free " + kDash +
                    " large releases may fail to import");
    CHECK(*low_space_warning(kGiB / 2) == "Warning: only 0 GB free " + kDash +
                                              " large releases may fail to import");
}

TEST_CASE("detail: library match and mode", "[detail_logic]") {
    const std::vector<mb::Movie> lib{movie(10, 1, false), movie(20, 2, true)};
    CHECK(find_movie_by_tmdb(lib, 20) == &lib[1]);
    CHECK(find_movie_by_tmdb(lib, 30) == nullptr);
    CHECK(mode_for_library_match(nullptr) == DetailMode::NotInLibrary);
    CHECK(mode_for_library_match(&lib[0]) == DetailMode::InLibraryNoFile);
    CHECK(mode_for_library_match(&lib[1]) == DetailMode::InLibraryWithFile);
}

TEST_CASE("detail: queue import probe — the first row for the movie decides",
          "[detail_logic]") {
    CHECK_FALSE(queue_shows_import({}, 7));
    CHECK(queue_shows_import({qitem(3, "downloading"), qitem(7, "importing")}, 7));
    CHECK(queue_shows_import({qitem(7, "importPending")}, 7));
    CHECK_FALSE(queue_shows_import({qitem(7, "downloading"), qitem(7, "importing")}, 7));
    CHECK_FALSE(queue_shows_import({qitem(3, "importing")}, 7));
}

TEST_CASE("detail: library poll drain", "[detail_logic]") {
    using S = LibraryPollStep;
    CHECK(decide_library_poll(DetailMode::Loading, true, true, true) == S::Ignore);
    CHECK(decide_library_poll(DetailMode::InLibraryWithFile, true, true, true) ==
          S::Ignore);
    CHECK(decide_library_poll(DetailMode::InLibraryNoFile, false, true, true) ==
          S::Ignore);
    CHECK(decide_library_poll(DetailMode::InLibraryNoFile, true, false, false) ==
          S::Ignore);
    CHECK(decide_library_poll(DetailMode::InLibraryNoFile, true, true, true) ==
          S::FileLanded);
    CHECK(decide_library_poll(DetailMode::InLibraryNoFile, true, true, false) ==
          S::StillWaiting);
}

TEST_CASE("detail: worker drains", "[detail_logic]") {
    CHECK(decide_add_drain(false, 5, 5) == AddDrain::Failed);
    CHECK(decide_add_drain(true, 6, 5) == AddDrain::OtherMovie);
    CHECK(decide_add_drain(true, 5, 5) == AddDrain::Success);

    CHECK(decide_remove_drain(false, true, 1, 1) == RemoveDrain::Failed);
    CHECK(decide_remove_drain(true, true, 2, 1) == RemoveDrain::OtherMovie);
    CHECK(decide_remove_drain(true, true, 1, 1) == RemoveDrain::Removed);
    // No record on screen any more: still the removed path.
    CHECK(decide_remove_drain(true, false, 0, 1) == RemoveDrain::Removed);

    auto v = decide_search_drain(true, true);
    CHECK(v.banner == std::optional<std::string>("Search triggered"));
    CHECK_FALSE(v.toast);
    v = decide_search_drain(true, false);
    CHECK_FALSE(v.banner.has_value());
    CHECK_FALSE(v.toast);
    v = decide_search_drain(false, true);
    CHECK(v.banner == std::optional<std::string>("Search failed"));
    CHECK(v.toast);
    v = decide_search_drain(false, false);
    CHECK_FALSE(v.banner.has_value());
    CHECK(v.toast);
    CHECK(std::string(search_failed_toast()) ==
          "Search didn't start " + kDash + " Radarr didn't answer; try again");
}

TEST_CASE("detail: formatters", "[detail_logic]") {
    CHECK(format_runtime(0) == "N/A");
    CHECK(format_runtime(-5) == "N/A");
    CHECK(format_runtime(45) == "45m");
    CHECK(format_runtime(120) == "2h");
    CHECK(format_runtime(136) == "2h 16m");
    CHECK(format_rating(0.0) == "");
    CHECK(format_rating(7.44) == "7.4");
    CHECK(format_rating(10.0) == "10.0");
    CHECK(format_vote_count(120) == "120");
    CHECK(format_vote_count(1234) == "1.2k");
    CHECK(format_vote_count(9999) == "9.9k");
    CHECK(format_vote_count(15000) == "15k");
    CHECK(format_vote_count(1500000) == "1M");
    CHECK(join_with_bullet({}) == "");
    CHECK(join_with_bullet({"A"}) == "A");
    CHECK(join_with_bullet({"A", "B", "C"}) == "A" + kBullet + "B" + kBullet + "C");
}

TEST_CASE("detail: display metadata fallback chain", "[detail_logic]") {
    mb::TmdbMovieDetail t;
    t.title = "Heat";
    t.year = 1995;
    t.runtime_minutes = 170;
    t.poster_path = "tmdb.jpg";
    t.genres = {"Crime"};
    mb::Movie m = movie(949, 3, true);
    m.title = "Heat (Radarr)";
    m.year = 1994;
    m.poster_url = "radarr.jpg";
    m.overview = "radarr overview";

    auto d = resolve_detail_display(t, m);
    CHECK(d.title == "Heat");
    CHECK(d.year == 1995);
    CHECK(d.poster_url == "tmdb.jpg");
    CHECK(d.overview.empty());  // TMDB wins wholesale when it has a title

    t.title.clear();
    d = resolve_detail_display(t, m);
    CHECK(d.title == "Heat (Radarr)");
    CHECK(d.year == 1994);
    CHECK(d.overview == "radarr overview");
    CHECK(d.poster_url == "tmdb.jpg");  // TMDB's poster kept when it had one
    CHECK(d.genres == std::vector<std::string>{"Crime"});

    t.poster_path.clear();
    CHECK(resolve_detail_display(t, m).poster_url == "radarr.jpg");
    CHECK(resolve_detail_display(std::nullopt, std::nullopt).title == "Untitled");

    CHECK(detail_play_title(std::nullopt, m) == "Heat (Radarr)");
    t.title = "Heat";
    CHECK(detail_play_title(t, m) == "Heat");
}

TEST_CASE("detail: header, meta and small copy", "[detail_logic]") {
    CHECK(detail_header_sub_info(1999) == "1999  \xC2\xB7  BTN4 back");
    CHECK(detail_header_sub_info(0) == "BTN4 back");

    CHECK(detail_meta_line(1999, 136, "en") ==
          "1999" + kBullet + "2h 16m" + kBullet + "EN");
    CHECK(detail_meta_line(0, 0, "") == "");
    CHECK(detail_meta_line(0, 95, "") == "1h 35m");
    CHECK(detail_meta_line(0, 45, "") == "45m");
    CHECK(detail_meta_line(0, 0, "fr") == "FR");
    CHECK(detail_meta_line(2001, 0, "") == "2001");
    CHECK(detail_votes_text(25000) == "(25k votes)");

    CHECK(std::string(directors_label(1)) == "DIRECTED BY");
    CHECK(std::string(directors_label(2)) == "DIRECTORS");
    CHECK(quoted_tagline("Free your mind.") ==
          "\xE2\x80\x9C" "Free your mind.\xE2\x80\x9D");
    CHECK(std::string(detail_error_message(true)) ==
          "Couldn't fetch movie info from TMDB.");
    CHECK(std::string(detail_error_message(false)) ==
          "No TMDB key " + kDash + " add one in the Content Manager, Media Browser tab.");

    const auto hints = detail_footer_hints();
    REQUIRE(hints.size() == 6);
    CHECK(hints[3].action == "Back");
    CHECK(hints[4].action == "Action");
    CHECK(hints[5].action == "Confirm");
}

TEST_CASE("detail: availability readout", "[detail_logic]") {
    using S = mb::ProwlarrClient::State;
    auto v = availability_view(S::Searching, "", std::nullopt);
    CHECK(v.body == availability_searching_message());
    CHECK(v.tone == MbTone::Dim);
    CHECK(v.alpha == 0.85f);
    CHECK(availability_view(S::Idle, "", std::nullopt).body ==
          availability_searching_message());

    v = availability_view(S::Failed, "timeout", std::nullopt);
    CHECK(v.body == "Sources unavailable: timeout");
    CHECK(v.tone == MbTone::Highlight2);
    CHECK(v.alpha == 0.95f);

    v = availability_view(S::Ready, "", std::nullopt);
    CHECK(v.body == "No sources found" + kBullet +
                        "Add anyway and Radarr will keep watching");
    CHECK(v.tone == MbTone::Highlight2);
    mb::ReleaseSummary sum;
    v = availability_view(S::Ready, "", sum);  // zero releases
    CHECK(v.tone == MbTone::Highlight2);

    sum.total_releases = 4;
    sum.best_seeders = 30;
    sum.total_seeders = 51;
    v = availability_view(S::Ready, "", sum);
    CHECK(v.body == "30 seeders (best)" + kBullet + "4 releases" + kBullet +
                        "51 total seeders");
    CHECK(v.tone == MbTone::Highlight1);
    CHECK(v.alpha == 0.95f);
}

TEST_CASE("detail: banners", "[detail_logic]") {
    CHECK(vpn_banner_applies(true, false, DetailMode::NotInLibrary));
    CHECK(vpn_banner_applies(true, false, DetailMode::InLibraryNoFile));
    CHECK_FALSE(vpn_banner_applies(true, false, DetailMode::InLibraryWithFile));
    CHECK_FALSE(vpn_banner_applies(true, true, DetailMode::NotInLibrary));
    CHECK_FALSE(vpn_banner_applies(false, false, DetailMode::NotInLibrary));
    CHECK(std::string(vpn_down_banner_text()).rfind("VPN TUNNEL DOWN", 0) == 0);

    auto b = awaiting_file_banner(true, true);  // importing wins
    CHECK(b.tone == MbTone::Highlight1);
    CHECK(b.text.rfind("DOWNLOADED", 0) == 0);
    b = awaiting_file_banner(false, true);
    CHECK(b.tone == MbTone::Highlight2);
    CHECK(b.text.rfind("IN THEATERS", 0) == 0);
    b = awaiting_file_banner(false, false);
    CHECK(b.tone == MbTone::Dim);
    CHECK(b.text.rfind("MONITORED", 0) == 0);

    CHECK(runtime_mismatch_text(3, 120) ==
          "FILE LOOKS WRONG" + kBullet + "3 min file vs 120 min expected " + kDash +
              " probably not the real movie (use Remove, then re-add)");
}

TEST_CASE("detail: flex column line budgets", "[detail_logic]") {
    // line_h 28, min section 70, top pad 14, label 22.
    CHECK(synopsis_line_budget(300.0f, false, false, 70.0f, 14.0f, 28.0f) == 10);
    CHECK(synopsis_line_budget(300.0f, true, true, 70.0f, 14.0f, 28.0f) == 5);
    CHECK(synopsis_line_budget(50.0f, true, true, 70.0f, 14.0f, 28.0f) == 1);  // floor 1
    CHECK(cast_line_budget(200.0f, true, 70.0f, 14.0f, 22.0f, 28.0f) == 3);
    CHECK(cast_line_budget(200.0f, false, 70.0f, 14.0f, 22.0f, 28.0f) == 5);
    CHECK(directors_line_budget(100.0f, 14.0f, 22.0f, 28.0f) == 2);
    CHECK(directors_line_budget(-40.0f, 14.0f, 22.0f, 28.0f) == 1);
}
