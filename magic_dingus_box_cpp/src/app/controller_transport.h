#pragma once

// The kiosk's PlaylistTransport: forwards every call 1:1 to the real
// Controller and GstPlayer. Kiosk-only (needs GStreamer); the Mac tests use
// a recording fake instead. See app/playlist_playback.h.

#include <thread>

#include "controller.h"
#include "playlist_playback.h"
#include "../video/gst_player.h"

namespace app {

class ControllerTransport final : public PlaylistTransport {
public:
    ControllerTransport(Controller& controller, video::GstPlayer& player)
        : controller_(controller), player_(player) {}

    void stop() override { controller_.stop(); }
    void play() override { controller_.play(); }
    void toggle_pause() override { controller_.toggle_pause(); }
    void seek(double seconds) override { controller_.seek(seconds); }
    bool is_playing() const override { return controller_.is_playing(); }
    bool is_paused() const override { return controller_.is_paused(); }
    utils::Result<> load_playlist_item(AppState& state, const Playlist& playlist,
                                       int item_index,
                                       const std::string& playlist_directory) override {
        return controller_.load_playlist_item(state, playlist, item_index,
                                              playlist_directory);
    }
    void load_next_item(AppState& state, const std::string& dir) override {
        controller_.load_next_item(state, dir);
    }
    void load_previous_item(AppState& state, const std::string& dir) override {
        controller_.load_previous_item(state, dir);
    }
    void play_random_global_video(AppState& state, const std::string& dir) override {
        controller_.play_random_global_video(state, dir);
    }
    void master_shuffle_advance(AppState& state, const std::string& dir) override {
        controller_.master_shuffle_advance(state, dir);
    }
    void master_shuffle_back(AppState& state, const std::string& dir) override {
        controller_.master_shuffle_back(state, dir);
    }

    uint64_t stream_generation() const override { return player_.stream_generation(); }
    bool has_error() const override { return player_.has_error(); }
    double player_position() const override { return player_.get_position(); }

    void sleep_for(std::chrono::milliseconds d) override {
        std::this_thread::sleep_for(d);
    }

private:
    Controller& controller_;
    video::GstPlayer& player_;
};

}  // namespace app
