#include <catch2/catch_test_macros.hpp>

#include "app/app_state.h"
#include "app/controller.h"
#include "video/video_player.h"

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
    bool is_playing() const override { return false; }
    bool is_paused() const override { return false; }
    double get_position() const override { return 0.0; }
    double get_duration() const override { return 0.0; }
    void set_volume(double) override {}
    double get_volume() const override { return 1.0; }
    void cleanup() override {}
    void update_state() override { ++update_calls; }
};
}  // namespace

TEST_CASE("Controller::update_state pumps the player through the VideoPlayer interface", "[core][controller]") {
    CountingPlayer player;
    app::Controller controller(&player);
    app::AppState state;
    controller.update_state(state);
    REQUIRE(player.update_calls == 1);
}
