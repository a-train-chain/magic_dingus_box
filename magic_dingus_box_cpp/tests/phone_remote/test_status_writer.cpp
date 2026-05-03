#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include "app/app_state.h"
#include "app/status_writer.h"

namespace fs = std::filesystem;

TEST_CASE("status_writer emits required schema fields", "[remote][status]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_test.json";
    fs::remove(tmp);

    app::AppState state;
    state.screen_mode = app::ScreenMode::Playback;
    state.set_position(42.5);
    state.set_duration(100.0);
    state.paused.store(false);

    app::StatusWriter w(tmp.string());
    w.write_now(state);

    REQUIRE(fs::exists(tmp));

    std::ifstream f(tmp);
    Json::Value root;
    f >> root;

    REQUIRE(root["schema"].asInt() == 1);
    REQUIRE(root["screen"].asString() == "playback");
    REQUIRE(root["playback"]["position_sec"].asDouble() == 42.5);
    REQUIRE(root["playback"]["duration_sec"].asDouble() == 100.0);
    REQUIRE(root["playback"]["is_paused"].asBool() == false);
    REQUIRE(root["ts"].asDouble() > 0.0);
}

TEST_CASE("status_writer atomic write — never leaves partial files", "[remote][status]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_atomic.json";
    fs::remove(tmp);

    app::AppState state;
    state.screen_mode = app::ScreenMode::Playlist;

    app::StatusWriter w(tmp.string());
    w.write_now(state);

    // Verify no leftover .tmp file in the directory
    fs::path tmp_dir = tmp.parent_path();
    bool has_partial = false;
    for (auto& entry : fs::directory_iterator(tmp_dir)) {
        if (entry.path().filename().string().find("mdb_status_atomic") != std::string::npos
            && entry.path().extension() == ".tmp") {
            has_partial = true;
        }
    }
    REQUIRE(has_partial == false);
}
