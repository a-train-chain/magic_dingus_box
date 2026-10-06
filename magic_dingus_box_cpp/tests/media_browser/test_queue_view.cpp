// Table tests for queue_view.h — QueueScreen's refresh-worker passes,
// cancel table and composed text, moved out of queue_screen.cpp so they run
// on the Mac.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "media_browser/ui/queue_view.h"

namespace mb = media_browser;
using namespace media_browser::ui;

namespace {

const std::string kDash = "\xE2\x80\x94";
const std::string kBullet = "  \xE2\x80\xA2  ";

mb::QueueItem qitem(int id, int movie_id, const std::string& download_id = "",
                    const std::string& state = "downloading") {
    mb::QueueItem q;
    q.id = id;
    q.movie_id = movie_id;
    q.download_id = download_id;
    q.state = state;
    return q;
}

mb::Movie movie(int radarr_id, bool monitored, bool has_file,
                const std::string& poster = "") {
    mb::Movie m;
    m.radarr_id = radarr_id;
    m.monitored = monitored;
    m.has_file = has_file;
    m.poster_url = poster;
    return m;
}

TvQueueRow tvrow(int first_id, const std::string& hash = "", int64_t size = 0,
                 int64_t left = 0) {
    TvQueueRow t;
    t.group.first_queue_id = first_id;
    t.group.download_id = hash;
    t.group.size_bytes = size;
    t.group.sizeleft_bytes = left;
    t.group.status = "downloading";
    return t;
}

}  // namespace

TEST_CASE("queue view: formatters", "[queue_view]") {
    CHECK(queue_format_rate(0) == "0 B/s");
    CHECK(queue_format_rate(512) == "512 B/s");
    CHECK(queue_format_rate(1536) == "1.5 KB/s");
    CHECK(queue_format_rate(150 * 1024) == "150 KB/s");
    CHECK(queue_format_rate(3 * 1024 * 1024) == "3.0 MB/s");

    CHECK(queue_format_eta(0) == "--");
    CHECK(queue_format_eta(45) == "45s");
    CHECK(queue_format_eta(725) == "12m 05s");
    CHECK(queue_format_eta(4980) == "1h 23m");

    CHECK(queue_titlecase_state("") == "Unknown");
    CHECK(queue_titlecase_state("downloading") == "Downloading");

    CHECK(queue_progress_tone("failed") == MbTone::Highlight2);
    CHECK(queue_progress_tone("warning") == MbTone::Highlight2);
    CHECK(queue_progress_tone("stalled") == MbTone::Highlight2);
    CHECK(queue_progress_tone("downloading") == MbTone::Highlight1);
    CHECK(queue_progress_tone("completed") == MbTone::Highlight1);
    CHECK(queue_progress_tone("importing") == MbTone::Accent);
    CHECK(queue_progress_tone("queued") == MbTone::Accent);
    CHECK(queue_progress_tone("") == MbTone::Accent);

    TvQueueGroup g;
    g.series_title = "Breaking Bad " + kDash + " Season 1";
    g.episode_count = 10;
    CHECK(tv_row_title(g) == "Breaking Bad " + kDash + " Season 1 (10 eps)");
    g.series_title.clear();
    g.episode_count = 1;
    CHECK(tv_row_title(g) == "Untitled (1 ep)");
}

TEST_CASE("queue view: qBit state vocabulary", "[queue_view]") {
    CHECK(arr_state_from_qbit("downloading", "x") == "downloading");
    CHECK(arr_state_from_qbit("stalledDL", "x") == "stalled");
    for (const char* s : {"metaDL", "queuedDL", "checkingDL", "allocating"})
        CHECK(arr_state_from_qbit(s, "x") == "queued");
    for (const char* s : {"uploading", "pausedUP", "stalledUP", "queuedUP",
                          "checkingUP", "forcedUP"})
        CHECK(arr_state_from_qbit(s, "x") == "completed");
    CHECK(arr_state_from_qbit("error", "x") == "failed");
    CHECK(arr_state_from_qbit("missingFiles", "x") == "failed");
    CHECK(arr_state_from_qbit("pausedDL", "x") == "paused");
    CHECK(arr_state_from_qbit("moving", "importing") == "importing");  // unmapped
    CHECK(lowercase_hash("ABCdef12") == "abcdef12");
}

TEST_CASE("queue view: library-cache staleness", "[queue_view]") {
    const std::vector<mb::Movie> cache{movie(1, true, false), movie(2, true, true)};
    CHECK(movie_lib_cache_stale(true, cache, {}));
    CHECK_FALSE(movie_lib_cache_stale(false, cache, {qitem(10, 1), qitem(11, 2)}));
    CHECK(movie_lib_cache_stale(false, cache, {qitem(10, 3)}));  // just added

    std::vector<mb::Series> tv_cache(1);
    tv_cache[0].sonarr_id = 5;
    TvQueueGroup known, unknown, zero;
    known.series_id = 5;
    unknown.series_id = 6;
    zero.series_id = 0;
    CHECK_FALSE(tv_lib_cache_stale(false, tv_cache, {known, zero}));
    CHECK(tv_lib_cache_stale(false, tv_cache, {known, unknown}));
    CHECK(tv_lib_cache_stale(true, tv_cache, {}));

    tv_cache[0].title = "Show";
    tv_cache[0].poster_url = "p.jpg";
    const auto refs = series_refs_by_id(tv_cache);
    REQUIRE(refs.count(5) == 1);
    CHECK(refs.at(5).title == "Show");
    CHECK(refs.at(5).poster_url == "p.jpg");
}

TEST_CASE("queue view: TV rows carry Sonarr's ETA for the kept row id",
          "[queue_view]") {
    std::vector<mb::SonarrQueueItem> rows(2);
    rows[0].id = 100; rows[0].eta_seconds = 300;
    rows[1].id = 101; rows[1].eta_seconds = 999;
    TvQueueGroup a, b;
    a.first_queue_id = 100;
    b.first_queue_id = 555;
    const auto tv = tv_rows_from_groups({a, b}, rows);
    REQUIRE(tv.size() == 2);
    CHECK(tv[0].eta_seconds == 300);
    CHECK(tv[0].group.first_queue_id == 100);
    CHECK(tv[1].eta_seconds == 0);
}

TEST_CASE("queue view: poster patching and the awaiting list", "[queue_view]") {
    std::vector<mb::QueueItem> queue{qitem(10, 1), qitem(11, 2), qitem(12, 9)};
    queue[1].poster_url = "own.jpg";
    const std::vector<mb::Movie> lib{
        movie(1, true, false, "lib1.jpg"), movie(2, true, false, "lib2.jpg"),
        movie(3, true, false, "lib3.jpg"), movie(4, false, false),
        movie(5, true, true)};
    const auto active = patch_queue_posters(queue, lib);
    CHECK(active == std::unordered_set<int>{1, 2, 9});
    CHECK(queue[0].poster_url == "lib1.jpg");
    CHECK(queue[1].poster_url == "own.jpg");  // never overwritten
    CHECK(queue[2].poster_url.empty());       // not in the library

    const auto awaiting = awaiting_release_movies(lib, active);
    REQUIRE(awaiting.size() == 1);
    CHECK(awaiting[0].radarr_id == 3);  // monitored, no file, not queued
}

TEST_CASE("queue view: qBit live overlay", "[queue_view]") {
    CHECK(qbit_overlay_failed(true, false, true));
    CHECK(qbit_overlay_failed(true, true, false));
    CHECK_FALSE(qbit_overlay_failed(true, true, true));
    CHECK_FALSE(qbit_overlay_failed(false, false, false));

    std::vector<mb::QueueItem> queue{qitem(10, 1, "AAAA", "queued"),
                                     qitem(11, 2, "", "queued"),
                                     qitem(12, 3, "CCCC", "queued")};
    std::vector<TvQueueRow> tv{tvrow(100, "BBBB")};
    mb::QbitTorrent a;
    a.progress = 0.5; a.dlspeed = 2048; a.upspeed = 10; a.num_leechs = 4;
    a.num_seeds = 2; a.size = 1000; a.downloaded = 400; a.eta_seconds = 60;
    a.state = "stalledDL";
    mb::QbitTorrent b = a;
    b.state = "uploading";
    b.size = 5000;
    b.downloaded = 5000;
    const std::unordered_map<std::string, mb::QbitTorrent> map{{"aaaa", a},
                                                               {"bbbb", b}};
    apply_qbit_overlay(queue, tv, map);
    CHECK(queue[0].progress == 0.5);
    CHECK(queue[0].download_rate_bps == 2048);
    CHECK(queue[0].upload_rate_bps == 10);
    CHECK(queue[0].peers == 4);
    CHECK(queue[0].seeds == 2);
    CHECK(queue[0].size_bytes == 1000);
    CHECK(queue[0].sizeleft_bytes == 600);
    CHECK(queue[0].eta_seconds == 60);
    CHECK(queue[0].state == "stalled");
    CHECK(queue[1].state == "queued");  // no hash: untouched
    CHECK(queue[2].state == "queued");  // not in qBit: untouched
    CHECK(tv[0].group.status == "completed");
    CHECK(tv[0].group.size_bytes == 5000);
    CHECK(tv[0].group.sizeleft_bytes == 0);
    CHECK(tv[0].peers == 4);
}

TEST_CASE("queue view: TV progress fallback and import reclassification",
          "[queue_view]") {
    std::vector<TvQueueRow> tv{tvrow(1, "", 1000, 250), tvrow(2, "", 0, 0),
                               tvrow(3, "", 1000, -5), tvrow(4, "", 1000, 900)};
    tv[3].progress = 0.7;  // overlay already reached it
    derive_tv_progress(tv);
    CHECK(tv[0].progress == 0.75);
    CHECK(tv[1].progress == 0.0);
    CHECK(tv[2].progress == 1.0);  // negative sizeleft clamps
    CHECK(tv[3].progress == 0.7);

    std::string st = "completed";
    reclassify_import_state(st, "importing");
    CHECK(st == "importing");
    st = "completed";
    reclassify_import_state(st, "importPending");
    CHECK(st == "importing");
    st = "completed";
    reclassify_import_state(st, "importBlocked");
    CHECK(st == "warning");
    st = "completed";
    reclassify_import_state(st, "importFailed");
    CHECK(st == "warning");
    st = "completed";
    reclassify_import_state(st, "imported");
    CHECK(st == "completed");
    st = "downloading";
    reclassify_import_state(st, "importing");
    CHECK(st == "downloading");
}

TEST_CASE("queue view: cursor, cancel table and scroll", "[queue_view]") {
    CHECK(clamp_queue_cursor(7, 3) == 2);
    CHECK(clamp_queue_cursor(-1, 3) == 0);
    CHECK(clamp_queue_cursor(2, 0) == 0);

    const std::vector<mb::QueueItem> queue{qitem(10, 1)};
    const std::vector<TvQueueRow> tv{tvrow(10)};
    CHECK(cancel_target_present(false, 10, queue, tv));
    CHECK(cancel_target_present(true, 10, queue, tv));
    CHECK_FALSE(cancel_target_present(false, 11, queue, tv));
    CHECK_FALSE(cancel_target_present(true, 11, queue, {}));

    // Section-qualified: id 10 in the TV section is not movie row 10.
    CHECK(cancel_armed_for(true, false, 10, false, 10));
    CHECK_FALSE(cancel_armed_for(true, true, 10, false, 10));
    CHECK_FALSE(cancel_armed_for(false, false, 10, false, 10));
    CHECK(decide_queue_select(true, false, 10, false, 10) == QueueSelect::Confirm);
    CHECK(decide_queue_select(true, true, 10, false, 10) == QueueSelect::Arm);
    CHECK(decide_queue_select(false, false, 0, false, 0) == QueueSelect::Arm);

    CHECK(cancel_failed_toast("") == "Couldn't cancel the download " + kDash + " try again");
    CHECK(cancel_failed_toast("Heat") == "Couldn't cancel Heat " + kDash + " try again");

    CHECK(queue_scroll_row(2, 5, 4) == 2);
    CHECK(queue_scroll_row(9, 0, 4) == 6);
    CHECK(queue_scroll_row(3, 1, 4) == 1);
}

TEST_CASE("queue view: row projection, sub-line and percentage", "[queue_view]") {
    std::vector<mb::QueueItem> queue{qitem(10, 1, "", "downloading")};
    queue[0].title = "Heat";
    queue[0].download_rate_bps = 1536;
    TvQueueRow t = tvrow(100);
    t.group.series_title = "Show";
    t.group.episode_count = 2;
    t.group.status = "importing";
    const auto rows = build_queue_rows(queue, {t});
    REQUIRE(rows.size() == 2);
    CHECK_FALSE(rows[0].is_tv);
    CHECK(rows[0].id == 10);
    CHECK(queue_row_active(rows[0]));
    CHECK(rows[1].is_tv);
    CHECK(rows[1].id == 100);
    CHECK(rows[1].title == "Show (2 eps)");
    CHECK_FALSE(queue_row_active(rows[1]));

    CHECK(queue_row_sub_line(rows[0]) == "Downloading" + kBullet + "1.5 KB/s");
    CHECK(queue_row_sub_line(rows[1]) == "TV" + kBullet + "Importing\xE2\x80\xA6");

    QueueRowView v;
    v.state = "downloading";
    v.size_bytes = 4LL * 1024 * 1024 * 1024;
    v.sizeleft_bytes = v.size_bytes;  // nothing yet
    v.eta_seconds = 725;
    v.peers = 1;
    v.seeds = 2;
    CHECK(queue_row_sub_line(v) == "Downloading" + kBullet + "0 B / 4.0 GB" + kBullet +
                                       "ETA 12m 05s" + kBullet + "1 peer / 2 seeds");

    CHECK(queue_percent_text(1.0) == "100%");
    CHECK(queue_percent_text(0.9995) == "100%");
    CHECK(queue_percent_text(0.0) == "0%");
    CHECK(queue_percent_text(0.0004) == "0%");
    CHECK(queue_percent_text(0.038) == "3.8%");
}

TEST_CASE("queue view: header lines and empty state", "[queue_view]") {
    CHECK(queue_count_line(1, 0) == "1 monitored");
    CHECK(queue_count_line(0, 1) == "1 monitored");
    CHECK(queue_count_line(2, 3) == "5 monitored " + kDash + " 2 downloading, 3 awaiting");
    CHECK(queue_count_line(0, 0) == "0 monitored " + kDash + " 0 downloading, 0 awaiting");

    auto ind = queue_refresh_indicator(true, 100);
    CHECK(ind.text == "refreshing...");
    CHECK(ind.tone == MbTone::Highlight1);
    ind = queue_refresh_indicator(false, 3);
    CHECK(ind.text == "updated 3s ago");
    CHECK(ind.tone == MbTone::Dim);
    CHECK(ind.alpha == 0.85f);
    ind = queue_refresh_indicator(false, 16);
    CHECK(ind.text == "slow " + kDash + " updated 16s ago");
    CHECK(ind.tone == MbTone::Accent);
    ind = queue_refresh_indicator(false, 46);
    CHECK(ind.text == "STALE " + kDash + " last update 46s ago");
    CHECK(ind.tone == MbTone::Highlight2);
    CHECK(queue_refresh_indicator(false, 45).tone == MbTone::Accent);
    CHECK(queue_refresh_indicator(false, 15).tone == MbTone::Dim);

    CHECK(radarr_offline_line("timeout") == "Radarr offline " + kDash + " timeout");
    CHECK(show_sonarr_offline_line(true, false));
    CHECK_FALSE(show_sonarr_offline_line(true, true));
    CHECK(show_qbit_overlay_line(true, true, false));
    CHECK_FALSE(show_qbit_overlay_line(true, true, true));

    auto hint = queue_empty_hint(false);
    CHECK(hint.text == "Add a movie from Browse or Search to start a download.");
    CHECK(hint.tone == MbTone::Dim);
    hint = queue_empty_hint(true);
    CHECK(hint.text == std::string(sonarr_offline_line()));
    CHECK(hint.tone == MbTone::Accent);
}

TEST_CASE("queue view: awaiting section", "[queue_view]") {
    mb::ActiveSearches s;
    std::vector<mb::Movie> awaiting{movie(1, true, false), movie(2, true, false)};
    awaiting[0].title = "Heat";
    awaiting[0].year = 1995;
    awaiting[1].title = "Untitled Project";
    CHECK(count_searching(awaiting, s) == 0);
    s.movie_ids.insert(2);
    CHECK(count_searching(awaiting, s) == 1);
    CHECK(awaiting_searching(awaiting[1], s));
    CHECK_FALSE(awaiting_searching(awaiting[0], s));
    s.global_search_running = true;
    CHECK(count_searching(awaiting, s) == 2);

    auto sub = awaiting_sub_line(1);
    CHECK(sub.text.rfind("Searching indexers now for 1 title ", 0) == 0);
    CHECK(sub.tone == MbTone::Highlight1);
    CHECK(awaiting_sub_line(3).text.rfind("Searching indexers now for 3 titles ", 0) == 0);
    sub = awaiting_sub_line(0);
    CHECK(sub.text.rfind("Radarr re-checks indexers", 0) == 0);
    CHECK(sub.tone == MbTone::Dim);

    CHECK(awaiting_row_label(awaiting[0], false) ==
          "Heat (1995)" + kBullet + "Monitored, awaiting release");
    CHECK(awaiting_row_label(awaiting[1], true) ==
          "Untitled Project" + kBullet + "Searching indexers now\xE2\x80\xA6");

    const auto hints = queue_footer_hints();
    REQUIRE(hints.size() == 6);
    CHECK(hints[4].action == "Browse");
}
