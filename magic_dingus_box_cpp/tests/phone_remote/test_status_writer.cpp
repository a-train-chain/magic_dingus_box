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

namespace {
void tube_state(app::AppState& state) {
    state.platform_profile.tube_crt_engine = true;
    state.tube_renderer_ready = true;
    state.display_settings.mode = app::DisplayMode::MODERN_TV;
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    tube::select_look(state.display_settings.tube, tube::Look::tv);
    tube::set_row(state.display_settings.tube, tube::Effect::hum, 20);
    tube::set_mask(state.display_settings.tube, tube::Mask::dots);
}

Json::Value written_tube(const app::AppState& state, const fs::path& tmp) {
    fs::remove(tmp);
    app::StatusWriter w(tmp.string());
    w.write_now(state);
    w.flush();
    Json::Value root;
    std::ifstream(tmp) >> root;
    fs::remove(tmp);
    return root["tube"];
}

Json::Value tube_of(const std::string& body) {
    std::istringstream in(body);
    Json::Value root;
    in >> root;
    return root["tube"];
}
} // namespace

TEST_CASE("status_writer publishes the tube state", "[remote][status][tube]") {
    app::AppState state;
    tube_state(state);
    state.tube_live.handled_seq = 12;
    state.tube_live.degauss_requests = 3;
    const Json::Value t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube.json");
    REQUIRE(t["gate"].asBool());
    REQUIRE(t["engine"].asString() == "tube");
    REQUIRE(t["effective_engine"].asString() == "tube");
    REQUIRE_FALSE(t["crt_native"].asBool());
    REQUIRE(t["look"].asString() == "tv");
    REQUIRE(t["mask"].asString() == "dots");
    REQUIRE(t["custom"].asBool());
    REQUIRE(t["moved"].size() == 2);
    REQUIRE(t["moved"][0].asString() == "hum");
    REQUIRE(t["moved"][1].asString() == "mask");
    REQUIRE(t["values"].size() == 44);
    REQUIRE(t["values"]["hum"].asInt() == 20);
    REQUIRE(t["values"]["brightness"].asInt() == 58);
    REQUIRE(t["handled_seq"].asUInt64() == 12);
    REQUIRE(t["degauss_requests"].asUInt64() == 3);
}

TEST_CASE("status_writer: the tube block carries the frame rate, too_heavy and unavailable",
          "[remote][status][tube]") {
    app::AppState state;
    tube_state(state);
    // Not drawing yet: the rate the setting will draw at (look tv is rich).
    Json::Value t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_rate.json");
    REQUIRE(t["frame_rate"].asInt() == 30);
    REQUIRE_FALSE(t["too_heavy"].asBool());
    REQUIRE(t["unavailable"].isNull());
    // Drawing: the renderer's own numbers win.
    state.tube_render.drawing = true;
    state.tube_render.frame_rate = 60;
    state.tube_render.too_heavy = true;
    state.tube_render.unavailable = "build_hung";
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_rate.json");
    REQUIRE(t["frame_rate"].asInt() == 60);
    REQUIRE(t["too_heavy"].asBool());
    REQUIRE(t["unavailable"].asString() == "build_hung");
}

TEST_CASE("status_writer: the tube block lists the effects the box is not drawing",
          "[remote][status][tube]") {
    app::AppState state;
    tube_state(state);   // look tv
    // Not drawing (before the first frame, the Media Browser, a film it does not draw): none.
    Json::Value t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_nd.json");
    REQUIRE(t["not_drawn"].isArray());
    REQUIRE(t["not_drawn"].empty());
    // Drawing the 4:3 picture of a 1080p Modern TV: TV cannot afford its afterglow.
    state.tube_render.drawing = true;
    state.tube_render.picture = tube::Picture{1440.0, 1080.0, false};
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_nd.json");
    REQUIRE(t["not_drawn"].size() == 1u);
    REQUIRE(t["not_drawn"][0].asString() == "afterglow");
    REQUIRE(t["frame_rate"].asInt() == 60);   // the renderer's own rate, unchanged by this
    // A larger picture where TV fits only without both: afterglow, then glow, in drop order.
    state.tube_render.picture = tube::Picture{1440.0, 1110.0, false};
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_nd.json");
    // Every row a dropped effect silences: GLOW SIZE with GLOW.
    REQUIRE(t["not_drawn"].size() == 3u);
    REQUIRE(t["not_drawn"][0].asString() == "afterglow");
    REQUIRE(t["not_drawn"][1].asString() == "glow");
    REQUIRE(t["not_drawn"][2].asString() == "glowSize");
    // A look that affords everything at the reference size.
    state.tube_render.picture = tube::Picture{1440.0, 1080.0, false};
    tube::select_look(state.display_settings.tube, tube::Look::trinitron);
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_nd.json");
    REQUIRE(t["not_drawn"].empty());
}

TEST_CASE("status_writer: the tube block says which engine draws the screen and a film",
          "[remote][status][tube]") {
    app::AppState state;
    tube_state(state);   // look tv
    Json::Value t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_film.json");
    REQUIRE(t["film_engine"].isNull());   // the main loop has not reported the output yet
    REQUIRE(t["yielding"] == false);
    REQUIRE(t["effective_engine"].asString() == "tube");
    state.tube_render.film_picture = tube::Picture{1920.0, 1080.0, false};
    // The List style shows no film full-screen: not applicable.
    state.display_settings.menu_style = app::MenuStyle::List;
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_film.json");
    REQUIRE(t["film_engine"].isNull());
    state.display_settings.menu_style = app::MenuStyle::ChannelGuide;
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_film.json");
    REQUIRE(t["film_engine"].asString() == "enhanced");
    // A film on screen, yielded: Enhanced draws it, and nothing is "not drawn".
    state.tube_render.yielding = true;
    state.tube_render.picture = tube::Picture{1920.0, 1080.0, false};
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_film.json");
    REQUIRE(t["yielding"] == true);
    REQUIRE(t["effective_engine"].asString() == "enhanced");
    REQUIRE(t["not_drawn"].empty());
    // The guard tripped: Enhanced draws, and the field says so.
    state.tube_render.yielding = false;
    state.tube_render.unavailable = "build_hung";
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_film.json");
    REQUIRE(t["effective_engine"].asString() == "enhanced");
    REQUIRE(t["engine"].asString() == "tube");
    // A look that fits a film.
    state.tube_render.unavailable.clear();
    tube::select_look(state.display_settings.tube, tube::Look::monitor);
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube_film.json");
    REQUIRE(t["film_engine"].asString() == "tube");
    REQUIRE(t["effective_engine"].asString() == "tube");
}

TEST_CASE("status_writer: Tube not offered publishes the engine and the reason only",
          "[remote][status][tube]") {
    app::AppState state;
    tube_state(state);
    state.tube_renderer_ready = false;   // the seam: every build sets it true since the switch-on
    Json::Value t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube2.json");
    REQUIRE(t.getMemberNames() ==
            std::vector<std::string>{"effective_engine", "engine", "gate", "reason"});
    REQUIRE(t["reason"].asString() == "not_ready");
    REQUIRE(t["effective_engine"].asString() == "enhanced");
    REQUIRE(t["engine"].asString() == "tube");
    state.platform_profile.tube_crt_engine = false;
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube2.json");
    REQUIRE(t["reason"].asString() == "needs_pi5");
}

TEST_CASE("status_writer: switched on, the board gate alone decides the web tab",
          "[remote][status][tube]") {
    // A default AppState (the renderer flag as every build sets it): a Pi 5 board
    // gate publishes gate true (the Content Manager shows the CRT Look tab); a
    // closed board gate (a Pi 4B, or a Pi 5 under MDB_PLATFORM_POLICY_OVERRIDE=pi4)
    // publishes needs_pi5, and the web hides the tab.
    app::AppState state;
    state.platform_profile.tube_crt_engine = true;
    Json::Value t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube3.json");
    REQUIRE(t["gate"].asBool());
    REQUIRE(t["engine"].asString() == "classic");
    REQUIRE(t["effective_engine"].asString() == "classic");
    state.platform_profile.tube_crt_engine = false;   // a real Pi 4B: the flag stays open
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube3.json");
    REQUIRE(t.getMemberNames() ==
            std::vector<std::string>{"effective_engine", "engine", "gate", "reason"});
    REQUIRE_FALSE(t["gate"].asBool());
    REQUIRE(t["reason"].asString() == "needs_pi5");
    REQUIRE(t["engine"].asString() == "classic");
    REQUIRE(t["effective_engine"].asString() == "classic");
}

TEST_CASE("status_writer: a tube change is written at once", "[remote][status][tube]") {
    // The writer is latest-wins by design: a snapshot the writer thread has not picked up
    // yet is replaced by a newer one. So the test must not depend on whether the thread
    // got to the first body before the second arrived. It holds the writer inside the first
    // write (waiting on a condition, no sleeps), submits the tube change behind it, then
    // lets go: the first body is in hand and the change is the pending one, so exactly
    // those two are written, in order. A change swallowed by the change-detection (the
    // heartbeat's 2 s window) would leave the first body as the last one.
    StalledSink rec;
    app::AppState state;
    tube_state(state);
    app::StatusWriter w("/nonexistent/kiosk_status.json", rec.sink());
    w.write_now(state);
    rec.wait_entered();
    tube::set_row(state.display_settings.tube, tube::Effect::hum, 25);
    w.write_now(state);  // well inside the 2 s heartbeat: only the change can cause a write
    rec.release();
    w.flush();
    REQUIRE(rec.bodies.size() == 2);
    REQUIRE(tube_of(rec.bodies[0])["values"]["hum"].asInt() == 20);
    REQUIRE(tube_of(rec.bodies.back())["values"]["hum"].asInt() == 25);
}
