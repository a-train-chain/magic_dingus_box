#include <catch2/catch_test_macros.hpp>
#include "media_browser/qbittorrent/download_watchdog.h"

namespace mb = media_browser;
using Clock = std::chrono::steady_clock;

// WatchedDownload aggregate initialization order:
//   { tmdb_id, radarr_movie_id, title, started_at, hash }
// (radarr_movie_id field was added in the v1.7.x async picker refactor;
// pre-refactor tests omitted it implicitly. New WatchedDownload literals
// pass it explicitly.)

TEST_CASE("watchdog emits stall when zero progress for 60s", "[watchdog][zero-progress]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({/*tmdb_id=*/1, /*radarr_movie_id=*/0,
                          /*title=*/"Inception",
                          /*started_at=*/in.now - std::chrono::seconds(70),
                          /*hash=*/"abc"});
    mb::QbitTorrent t; t.hash = "abc"; t.progress = 0.0; t.num_seeds = 0;
    in.qbit_torrents.push_back(t);
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].tmdb_id == 1);
    REQUIRE(events[0].reason == mb::DownloadWatchdog::Reason::ZeroProgress);
}

TEST_CASE("watchdog does NOT emit stall when within 60s grace", "[watchdog][grace]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({1, 0, "Inception",
                          in.now - std::chrono::seconds(30), "abc"});
    mb::QbitTorrent t; t.hash = "abc"; t.progress = 0.0; t.num_seeds = 0;
    in.qbit_torrents.push_back(t);
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.empty());
}

TEST_CASE("watchdog does NOT emit stall when peers are connected (even at 0%)",
          "[watchdog][peers-connected]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({1, 0, "Inception",
                          in.now - std::chrono::seconds(70), "abc"});
    mb::QbitTorrent t; t.hash = "abc"; t.progress = 0.0; t.num_seeds = 5;  // peers found
    in.qbit_torrents.push_back(t);
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.empty());
}

TEST_CASE("watchdog emits stall on Radarr downloadFailed history event",
          "[watchdog][radarr-failed]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({1, 0, "Inception",
                          in.now - std::chrono::seconds(10), "abc"});
    mb::RadarrClient::HistoryEvent ev;
    ev.movie_id = 1; ev.event_type = "downloadFailed"; ev.id = 42;
    in.recent_history.push_back(ev);
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].reason == mb::DownloadWatchdog::Reason::RadarrFailed);
}

TEST_CASE("watchdog suppresses stall during snooze window",
          "[watchdog][snooze]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({1, 0, "Inception",
                          in.now - std::chrono::seconds(70), "abc"});
    mb::QbitTorrent t; t.hash = "abc"; t.progress = 0.0; t.num_seeds = 0;
    in.qbit_torrents.push_back(t);
    in.snoozed_until[1] = in.now + std::chrono::seconds(60);  // snoozed
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.empty());
}

TEST_CASE("watchdog emits stall when no qBit torrent at all (Radarr never handed off)",
          "[watchdog][no-handoff]") {
    mb::DownloadWatchdog::Inputs in;
    in.now = Clock::now();
    in.watched.push_back({1, 0, "Inception",
                          in.now - std::chrono::seconds(70), "abc"});
    // No qBit torrents at all.
    auto events = mb::DownloadWatchdog::evaluate(in);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].reason == mb::DownloadWatchdog::Reason::ZeroProgress);
}

TEST_CASE("watchdog StallEvent carries radarr_movie_id from WatchedDownload",
          "[watchdog][movie-id]") {
    // Round-trip: when watched_.radarr_movie_id is set, evaluate() must
    // copy it through into both the ZeroProgress and RadarrFailed event
    // shapes. This is what powers the stall-modal "Pick another"
    // deep-link into ReleasePickerScreen — the modal needs the
    // radarr_movie_id to call load_async() (Radarr's release endpoint
    // is keyed on the Radarr id, not the TMDB id).
    SECTION("ZeroProgress reason copies radarr_movie_id through") {
        mb::DownloadWatchdog::Inputs in;
        in.now = Clock::now();
        in.watched.push_back({/*tmdb_id=*/1, /*radarr_movie_id=*/42,
                              "Inception",
                              in.now - std::chrono::seconds(70), "abc"});
        auto events = mb::DownloadWatchdog::evaluate(in);
        REQUIRE(events.size() == 1);
        REQUIRE(events[0].radarr_movie_id == 42);
        REQUIRE(events[0].tmdb_id == 1);
        REQUIRE(events[0].reason == mb::DownloadWatchdog::Reason::ZeroProgress);
    }
    SECTION("RadarrFailed reason copies radarr_movie_id through") {
        mb::DownloadWatchdog::Inputs in;
        in.now = Clock::now();
        in.watched.push_back({1, 99, "Inception",
                              in.now - std::chrono::seconds(10), "abc"});
        mb::RadarrClient::HistoryEvent ev;
        ev.movie_id = 1; ev.event_type = "downloadFailed"; ev.id = 7;
        in.recent_history.push_back(ev);
        auto events = mb::DownloadWatchdog::evaluate(in);
        REQUIRE(events.size() == 1);
        REQUIRE(events[0].radarr_movie_id == 99);
        REQUIRE(events[0].reason == mb::DownloadWatchdog::Reason::RadarrFailed);
    }
}
