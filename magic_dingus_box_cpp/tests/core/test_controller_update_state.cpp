#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "app/app_state.h"
#include "app/controller.h"
#include "video/video_player.h"

namespace fs = std::filesystem;

namespace {
struct CountingPlayer : video::VideoPlayer {
    int update_calls = 0;
    bool initialize(const std::string&) override { return true; }
    bool load_file(const std::string&, double, double, bool) override { return true; }
    void play() override {}
    void pause() override {}
    void toggle_pause() override {}
    void seek(double) override {}
    void seek_absolute(double) override {}
    void stop() override {}
    // True once update_state() has ticked at least once. This is what lets
    // a test that drives playback through the public API (load_playlist_item
    // -> wait_for_playback_start) observe the pump without a real backend:
    // wait_for_playback_start's loop calls update_state() and then checks
    // is_playing(), so this is exactly what ends that loop.
    bool is_playing() const override { return update_calls >= 1; }
    bool is_paused() const override { return false; }
    double get_position() const override { return 0.0; }
    double get_duration() const override { return 0.0; }
    void set_volume(double) override {}
    double get_volume() const override { return 1.0; }
    void cleanup() override {}
    void update_state() override { ++update_calls; }
};
}  // namespace

// NOTE on this test vs. the task-3 brief: the brief proposed asserting the
// seam purely by calling Controller::update_state(AppState&) and checking
// player.update_calls == 1. That does not hold, and it is not a bug in the
// seam: Controller::update_state(AppState&) reads player_->get_position()/
// get_duration() (GstPlayer queries the live pipeline for both, not a
// cached value) and player_->is_playing()/is_paused(), but it has never
// been the caller of VideoPlayer::update_state() -- before this refactor
// or after it. Only the private Controller::wait_for_playback_start()
// ticks it, via the exact seam this task adds (see controller.cpp: the
// `dynamic_cast<video::GstPlayer*>(player_)` block becomes
// `player_->update_state();`), and that path is reachable only through
// load_playlist_item's file-load branches -- not from a fresh Controller
// with no player state to load. Making the literal brief assertion pass
// would mean adding a NEW call to player_->update_state() inside the
// public per-frame update_state(AppState&), which fires at additional
// call sites in main.cpp (e.g. the intro-video wait loop and the
// post-intro stop-confirmation loop) whose consequences can't be verified
// here (main.cpp/BUILD_KIOSK is not buildable on this Mac) -- exactly the
// kind of kiosk-behavior change Phase 0 rules out. So this test instead
// pins the actual (narrower) property Controller::update_state(AppState&)
// has: it can hold a VideoPlayer* that is not a GstPlayer and never touches
// it via a dynamic_cast (update_calls stays 0 -- the old dynamic_cast block
// would have silently no-op'd here too, for the same reason), while
// VideoPlayer::update_state() itself still dispatches polymorphically
// through the base interface. The test below this one is the one that
// actually reaches the changed line in wait_for_playback_start() through
// the public API.
TEST_CASE("Controller::update_state(AppState&) does not itself pump VideoPlayer::update_state(), but the interface still dispatches virtually", "[core][controller]") {
    CountingPlayer player;
    app::Controller controller(&player);
    app::AppState state;
    controller.update_state(state);  // must not require player to be a GstPlayer
    video::VideoPlayer& iface = player;
    REQUIRE(player.update_calls == 0);
    iface.update_state();
    REQUIRE(player.update_calls == 1);
}

// This is the test that reaches the changed line in wait_for_playback_start()
// (controller.cpp: `player_->update_state();`, replacing the old
// `dynamic_cast<video::GstPlayer*>(player_)` block) through the public API.
// load_playlist_item's "local" branch calls stop(), then
// load_file_with_resolution() -- gated on fs::exists(), hence the real temp
// file below -- then play(), then wait_for_playback_start(1000, ...), whose
// loop calls player_->update_state() and checks is_playing() every ~16ms.
// CountingPlayer::is_playing() flips true on the FIRST such tick, so this
// both exits the loop immediately (no 1000ms budget burned) and proves the
// pump ran through the VideoPlayer interface. Restoring the old
// dynamic_cast<video::GstPlayer*>(player_) block would leave update_calls at
// 0 here (CountingPlayer is not a GstPlayer) and player.is_playing() would
// never flip true, so this test would fail -- see the fix-round report for
// RED/GREEN evidence of exactly that.
TEST_CASE("Controller::load_playlist_item pumps the player's update_state() through wait_for_playback_start", "[core][controller]") {
    const fs::path tmp_file = fs::temp_directory_path() / "mdb_test_update_state_seam.mp4";
    {
        std::ofstream out(tmp_file);
        out << "not a real video, just needs to exist for the fs::exists gate";
    }

    CountingPlayer player;
    app::Controller controller(&player);
    app::AppState state;

    app::Playlist playlist;
    playlist.title = "Test Playlist";
    app::PlaylistItem item;
    item.title = "Test Item";
    item.path = tmp_file.string();
    item.source_type = "local";
    playlist.items.push_back(item);

    auto result = controller.load_playlist_item(state, playlist, 0, "");

    std::error_code ec;
    fs::remove(tmp_file, ec);

    REQUIRE(result.success);
    REQUIRE(player.update_calls >= 1);
}
