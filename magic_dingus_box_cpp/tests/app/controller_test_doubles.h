#pragma once

// Test doubles for linking app::Controller's portable half
// (src/app/controller_playback.cpp) into test_app_unit without
// controller.cpp (GStreamer/DRM/RetroArch, kiosk-only).
//
// FakeVideoPlayer stands in for GstPlayer behind video::VideoPlayer.
// controller_test_doubles.cpp defines Controller::load_item_media — the
// media half of load_playlist_item — as the kiosk's call sequence minus the
// sleeps: stop(), the 200 ms settle (recorded as "settle", not slept),
// load_file_with_resolution(), play(). Every call lands in one ordered
// event log so a test can assert what a step cost.

#include <set>
#include <string>
#include <vector>

#include "video/video_player.h"

namespace test_doubles {

// The ordered log of player calls and load_item_media steps.
std::vector<std::string>& events();
// Result of the next emulated_game load_item_media (default: success).
bool& game_launch_ok();
// Count of `name` entries in events().
int count(const std::string& name);

class FakeVideoPlayer : public video::VideoPlayer {
public:
    // Scripted state the getters report.
    bool playing = false;
    bool paused = false;
    bool eos = false;
    bool error = false;
    double position = 0.0;
    double duration = 0.0;
    double volume = 100.0;
    // load_file() returns false for these resolved paths.
    std::set<std::string> fail_paths;

    bool initialize(const std::string&) override { return true; }
    bool load_file(const std::string& path, double, double, bool) override;
    void play() override { events().push_back("play"); playing = true; }
    void pause() override { paused = true; }
    void toggle_pause() override { paused = !paused; }
    void seek(double) override {}
    void seek_absolute(double) override {}
    void stop() override;
    bool is_playing() const override { return playing; }
    bool is_paused() const override { return paused; }
    bool at_eos() const override { return eos; }
    bool has_error() const override { return error; }
    double get_position() const override { return position; }
    double get_duration() const override { return duration; }
    void set_volume(double v) override { volume = v; }
    double get_volume() const override { return volume; }
    void cleanup() override {}
};

}  // namespace test_doubles
