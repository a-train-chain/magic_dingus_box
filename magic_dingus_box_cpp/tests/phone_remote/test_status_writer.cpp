#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include "app/app_state.h"
#include "app/status_writer.h"
#include "ui/virtual_keyboard.h"

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
    w.flush();

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
    w.flush();

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

TEST_CASE("status_writer emits text_input.active=false when no keyboard", "[remote][status][text_input]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_text_inactive.json";
    fs::remove(tmp);

    app::AppState state;
    state.active_text_keyboard = nullptr;

    app::StatusWriter w(tmp.string());
    w.write_now(state);
    w.flush();

    std::ifstream f(tmp);
    Json::Value root;
    f >> root;

    REQUIRE(root.isMember("text_input"));
    REQUIRE(root["text_input"]["active"].asBool() == false);
}

TEST_CASE("status_writer emits text_input block when keyboard active", "[remote][status][text_input]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_text_active.json";
    fs::remove(tmp);

    ui::VirtualKeyboard kb;
    kb.open("shawsh", "Search movies", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    state.active_text_title    = "Search movies";

    app::StatusWriter w(tmp.string());
    w.write_now(state);
    w.flush();

    std::ifstream f(tmp);
    Json::Value root;
    f >> root;

    REQUIRE(root.isMember("text_input"));
    REQUIRE(root["text_input"]["active"].asBool() == true);
    REQUIRE(root["text_input"]["title"].asString() == "Search movies");
    REQUIRE(root["text_input"]["buffer"].asString() == "shawsh");
}

TEST_CASE("status_writer publishes the TEST-ONLY platform policy override",
          "[remote][status][platform]") {
    // verify_box.sh fails a box whose status carries a non-null
    // platform_policy_override, so the field must be present (null) when
    // the override is off and carry "pi4" when it is on.
    fs::path tmp = fs::temp_directory_path() / "mdb_status_policy_override.json";

    auto read_root = [&]() {
        std::ifstream f(tmp);
        Json::Value root;
        f >> root;
        return root;
    };

    {
        fs::remove(tmp);
        app::AppState state;
        app::StatusWriter w(tmp.string());
        w.write_now(state);
        w.flush();
        Json::Value root = read_root();
        REQUIRE(root.isMember("platform_policy_override"));
        REQUIRE(root["platform_policy_override"].isNull());
    }
    {
        fs::remove(tmp);
        app::AppState state;
        state.platform_profile.policy_override = "pi4";
        app::StatusWriter w(tmp.string());
        w.write_now(state);
        w.flush();
        Json::Value root = read_root();
        REQUIRE(root["platform_policy_override"].asString() == "pi4");
    }
}

// ── Off-thread file I/O ─────────────────────────────────────────────────
// 2026-10-26, 1 GB Pi 4B: an 11 GB package import saturated the SD card
// and the status tmp+rename sat in ext4 for up to ~6 s per call ON THE
// RENDER THREAD, which pings the systemd watchdog — five SIGABRTs in
// fifteen minutes. These pin the contract that fixed it: write_now never
// waits for a write, the newest snapshot wins, and nothing is lost.

namespace {

// A FileSink that can be held mid-write, standing in for an SD card stuck
// behind a writeback queue. Records every body it is handed.
struct StalledSink {
    std::mutex mu;
    std::condition_variable cv;
    bool released = false;
    bool entered = false;
    std::vector<std::string> bodies;
    int fail_next = 0;

    app::StatusWriter::FileSink sink() {
        return [this](const std::string&, const std::string&, const std::string& body) {
            std::unique_lock<std::mutex> lk(mu);
            entered = true;
            cv.notify_all();
            cv.wait(lk, [this] { return released; });
            bodies.push_back(body);
            if (fail_next > 0) { --fail_next; return false; }
            return true;
        };
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lk(mu);
        released = true;
        cv.notify_all();
    }
};

double position_of(const std::string& body) {
    std::istringstream in(body);
    Json::Value root;
    in >> root;
    return root["playback"]["position_sec"].asDouble();
}

} // namespace

TEST_CASE("status_writer: write_now never waits for a stalled write",
          "[remote][status][io]") {
    StalledSink stalled;
    app::StatusWriter w("/nonexistent/kiosk_status.json", stalled.sink());
    app::AppState state;

    state.set_position(1.0);
    w.write_now(state);
    stalled.wait_entered();  // the writer is now stuck "on the card"

    // Many frames' worth of changing state while the write is stuck. With
    // the old synchronous writer each call would block for the stall.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 2; i <= 50; ++i) {
        state.set_position(static_cast<double>(i));
        w.write_now(state);
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(elapsed < std::chrono::milliseconds(500));

    stalled.release();
    w.flush();
    // Latest wins: the in-flight write, then ONLY the newest snapshot — not
    // a 49-write backlog the card would then have to work through.
    REQUIRE(stalled.bodies.size() == 2);
    REQUIRE(position_of(stalled.bodies[0]) == 1.0);
    REQUIRE(position_of(stalled.bodies[1]) == 50.0);
}

TEST_CASE("status_writer: a failed write is resent on the next call",
          "[remote][status][io]") {
    StalledSink sink;
    sink.release();
    sink.fail_next = 1;
    app::StatusWriter w("/nonexistent/kiosk_status.json", sink.sink());
    app::AppState state;
    state.set_position(7.0);

    w.write_now(state);
    w.flush();
    REQUIRE(sink.bodies.size() == 1);  // failed

    // Same content, well inside the 2 s heartbeat: without the failure it
    // would be skipped as unchanged.
    w.write_now(state);
    w.flush();
    REQUIRE(sink.bodies.size() == 2);
    REQUIRE(position_of(sink.bodies[1]) == 7.0);

    // Succeeded — an unchanged state is skipped again.
    w.write_now(state);
    w.flush();
    REQUIRE(sink.bodies.size() == 2);
}

TEST_CASE("status_writer: destruction publishes the final snapshot",
          "[remote][status][io]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_final.json";
    fs::remove(tmp);
    {
        app::StatusWriter w(tmp.string());
        app::AppState state;
        state.screen_mode = app::ScreenMode::RetroArch;
        w.write_now(state);
    }  // no flush(): the destructor must not drop the pending write
    std::ifstream f(tmp);
    Json::Value root;
    f >> root;
    REQUIRE(root["screen"].asString() == "retroarch");
}
