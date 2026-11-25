// app::SettingsPersistence against real files (MAGIC_SETTINGS_FILE points
// at a temp path). What is pinned:
//   - every persisted field round-trips save -> load;
//   - a missing file is not an error and leaves the defaults alone;
//   - a WRONG-TYPED file (7413d96: "bezel_index":"2" escaped main() and
//     crash-looped the box on every systemd restart) is caught, moved
//     aside to settings.json.corrupt, and the next save writes a clean file
//     without touching the quarantined copy;
//   - peek_is_crt_native() answers CRT (the safe 720p path) for every
//     unreadable shape;
//   - loading applies the audio output (through the audio router's runner,
//     faked here so no real pactl ever runs);
//   - saving never does file I/O on the caller (the render loop, which
//     pings the systemd watchdog): the bytes go to a background writer,
//     the newest snapshot wins, shutdown's flush is bounded and retries a
//     failed write once, and a reload that overtakes an unfinished save
//     writes the reloaded state back.
//   - the CRT engine choice and the tube setting round-trip, an older file
//     loads its engine from enhanced_crt_enabled, and a bad tube value is
//     dropped with a warning — never a quarantine.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <json/json.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "app/app_state.h"
#include "app/audio_router.h"
#include "app/settings_persistence.h"
#include "platform/platform_profile.h"
#include "tube/tube_setting.h"

namespace fs = std::filesystem;
using app::AppState;
using app::AudioOutput;
using app::DisplayMode;
using app::SettingsPersistence;
using namespace std::chrono_literals;

namespace {

// save_settings() only queues; the file is written by a background thread.
bool saved_to_disk() {
    return SettingsPersistence::flush_pending_saves(10s);
}

// Points MAGIC_SETTINGS_FILE at a fresh temp file and fakes pactl for the
// test's lifetime.
struct SettingsSandbox {
    fs::path dir;
    fs::path file;
    std::vector<std::vector<std::string>> pactl_calls;

    SettingsSandbox() {
        dir = fs::temp_directory_path() /
              ("mdb-settings-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        file = dir / "settings.json";
        ::setenv("MAGIC_SETTINGS_FILE", file.c_str(), 1);
        app::audio::set_default_runner_for_tests(
            [this](const std::vector<std::string>& argv) {
                pactl_calls.push_back(argv);
                utils::subprocess::Result r;
                r.exit_code = 0;
                if (argv.size() >= 4 && argv[3] == "sinks") {
                    r.out = "1\talsa_output.platform-fef00700.hdmi.hdmi-stereo\tm\ts\tRUNNING\n";
                }
                return r;
            });
    }
    ~SettingsSandbox() {
        (void)SettingsPersistence::flush_pending_saves(10s);
        SettingsPersistence::set_file_sink_for_tests({});
        app::audio::set_default_runner_for_tests({});
        ::unsetenv("MAGIC_SETTINGS_FILE");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& text) const {
        std::ofstream(file) << text;
    }
    std::string read(const fs::path& p) const {
        std::ifstream in(p);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    fs::path corrupt() const { return fs::path(file.string() + ".corrupt"); }
    Json::Value json() const {
        std::ifstream in(file);
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errors;
        REQUIRE(Json::parseFromStream(builder, in, &root, &errors));
        return root;
    }
};

// Captures what the LOG_* macros write (spdlog's default logger) for the
// test's lifetime, so a warning can be asserted and nothing reaches stdout.
struct LogCapture {
    std::ostringstream out;
    std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();

    LogCapture() {
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
        auto logger = std::make_shared<spdlog::logger>("capture", sink);
        logger->set_level(spdlog::level::trace);
        spdlog::set_default_logger(logger);
    }
    ~LogCapture() { spdlog::set_default_logger(previous); }

    std::size_t warnings() const {
        const std::string text = out.str();
        std::size_t n = 0;
        for (std::size_t at = text.find("[warning]"); at != std::string::npos;
             at = text.find("[warning]", at + 1)) {
            ++n;
        }
        return n;
    }
    bool mentions(const std::string& word) const {
        return out.str().find(word) != std::string::npos;
    }
};

std::optional<int> row(const tube::TubeSetting& t, tube::Effect e) {
    return t.rows[tube::index_of(e)];
}

std::size_t moved_row_count(const tube::TubeSetting& t) {
    std::size_t n = 0;
    for (const auto& r : t.rows) n += r.has_value() ? 1 : 0;
    return n;
}

}  // namespace

TEST_CASE("settings: every persisted field round-trips", "[settings]") {
    SettingsSandbox sb;
    AppState a;
    a.display_settings.mode = DisplayMode::MODERN_TV;
    a.display_settings.bezel_index = 3;
    a.display_settings.scanline_intensity = 0.25f;
    a.display_settings.warmth_intensity = 0.5f;
    a.display_settings.glow_intensity = 0.75f;
    a.display_settings.rgb_mask_intensity = 0.25f;
    a.display_settings.bloom_intensity = 0.5f;
    a.display_settings.interlacing_intensity = 0.75f;
    a.display_settings.flicker_intensity = 0.25f;
    a.display_settings.set_crt_engine(app::CrtEngine::Enhanced);
    a.display_settings.mb_playback_show_frame = false;
    a.display_settings.mb_scanline_intensity = 0.75f;
    a.display_settings.mb_flicker_intensity = 0.5f;
    a.display_settings.mb_library_sort = AppState::DisplaySettings::MbLibrarySort::Year;
    a.display_settings.mb_library_filter = AppState::DisplaySettings::MbLibraryFilter::MissingFiles;
    a.display_settings.menu_style = app::MenuStyle::ChannelGuide;
    a.last_channel_id = "playlist:Saturday Cartoons";
    a.playlist_loop = false;
    a.shuffle = true;
    a.master_volume = 37;
    a.audio_settings.output = AudioOutput::HEADPHONE;
    a.audio_settings.retroarch_volume_offset_db = -6.0f;
    REQUIRE(SettingsPersistence::save_settings(a));
    REQUIRE(saved_to_disk());

    AppState b;
    REQUIRE(SettingsPersistence::load_settings(b));
    CHECK(b.display_settings.mode == DisplayMode::MODERN_TV);
    CHECK(b.display_settings.bezel_index == 3);
    CHECK(b.display_settings.scanline_intensity == Catch::Approx(0.25f));
    CHECK(b.display_settings.warmth_intensity == Catch::Approx(0.5f));
    CHECK(b.display_settings.glow_intensity == Catch::Approx(0.75f));
    CHECK(b.display_settings.rgb_mask_intensity == Catch::Approx(0.25f));
    CHECK(b.display_settings.bloom_intensity == Catch::Approx(0.5f));
    CHECK(b.display_settings.interlacing_intensity == Catch::Approx(0.75f));
    CHECK(b.display_settings.flicker_intensity == Catch::Approx(0.25f));
    CHECK(b.display_settings.enhanced_crt_enabled);
    CHECK(b.display_settings.crt_engine == app::CrtEngine::Enhanced);
    CHECK_FALSE(b.display_settings.mb_playback_show_frame);
    CHECK(b.display_settings.mb_scanline_intensity == Catch::Approx(0.75f));
    CHECK(b.display_settings.mb_flicker_intensity == Catch::Approx(0.5f));
    CHECK(b.display_settings.mb_library_sort == AppState::DisplaySettings::MbLibrarySort::Year);
    CHECK(b.display_settings.mb_library_filter ==
          AppState::DisplaySettings::MbLibraryFilter::MissingFiles);
    CHECK(b.display_settings.menu_style == app::MenuStyle::ChannelGuide);
    CHECK(b.last_channel_id == "playlist:Saturday Cartoons");
    CHECK_FALSE(b.playlist_loop);
    CHECK(b.shuffle);
    CHECK(b.master_volume == 37);
    CHECK(b.audio_settings.output == AudioOutput::HEADPHONE);
    CHECK(b.audio_settings.retroarch_volume_offset_db == Catch::Approx(-6.0f));

    CHECK_FALSE(SettingsPersistence::peek_is_crt_native());
    // The atomic save leaves no temp file behind.
    CHECK_FALSE(fs::exists(sb.file.string() + ".tmp"));
}

TEST_CASE("settings: the checked-in fixture is what the kiosk writes", "[settings]") {
    // tests/app/fixtures/settings.json is ALSO the reference file
    // tests/local/settings_json_roundtrip.bats validates (it used to read a
    // build/dev_data copy that no checkout has, and skipped everywhere).
    // Loading it must succeed, and in the Media Browser build — the one
    // every shipped unit runs — saving it back must reproduce it byte for
    // byte, so the fixture cannot drift from the real on-disk format.
    const fs::path fixture = fs::path(__FILE__).parent_path() / "fixtures" / "settings.json";
    SettingsSandbox sb;
    const std::string original = sb.read(fixture);
    REQUIRE_FALSE(original.empty());
    sb.write(original);

    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.display_settings.mode == DisplayMode::MODERN_TV);
    CHECK(s.display_settings.menu_style == app::MenuStyle::ChannelGuide);
    CHECK(s.last_channel_id == "shuffle");
    CHECK(s.master_volume == 80);
    CHECK(s.audio_settings.output == AudioOutput::HDMI);
    CHECK_FALSE(fs::exists(sb.corrupt()));

    REQUIRE(SettingsPersistence::save_settings(s));
    REQUIRE(saved_to_disk());
#ifdef MEDIA_BROWSER_ENABLED
    CHECK(sb.read(sb.file) == original);
#endif
}

TEST_CASE("settings: a missing file is not an error and keeps the defaults", "[settings]") {
    SettingsSandbox sb;
    AppState s;
    s.master_volume = 64;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.master_volume == 64);
    CHECK(s.display_settings.mode == DisplayMode::CRT_NATIVE);
    CHECK(SettingsPersistence::peek_is_crt_native());   // missing -> safe CRT
    CHECK(sb.pactl_calls.empty());                      // nothing to apply
}

TEST_CASE("settings: missing sections and keys fall back to defaults", "[settings]") {
    SettingsSandbox sb;
    sb.write(R"({"display": {"mode": "modern_tv"}})");
    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.display_settings.mode == DisplayMode::MODERN_TV);
    CHECK(s.display_settings.bezel_index == 0);
    CHECK(s.playlist_loop);       // playback section absent: struct default
    CHECK(s.master_volume == 100);
}

TEST_CASE("settings: menu style and last channel default to List and none",
          "[settings]") {
    SettingsSandbox sb;
    sb.write(R"({"display": {"mode": "modern_tv"}, "playback": {"shuffle": true}})");
    AppState s;
    s.display_settings.menu_style = app::MenuStyle::ChannelGuide;  // must be reset
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.display_settings.menu_style == app::MenuStyle::List);
    CHECK(s.last_channel_id.empty());
}

TEST_CASE("settings: an unknown menu_style loads as List", "[settings]") {
    SettingsSandbox sb;
    sb.write(R"({"display": {"menu_style": "hologram"}})");
    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.display_settings.menu_style == app::MenuStyle::List);
}

TEST_CASE("settings: out-of-range volume is clamped", "[settings]") {
    SettingsSandbox sb;
    sb.write(R"({"playback": {"master_volume": 250}})");
    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.master_volume == 100);
    sb.write(R"({"playback": {"master_volume": -5}})");
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.master_volume == 0);
}

TEST_CASE("settings: wrong-typed fields are quarantined, never thrown", "[settings]") {
    // Each of these made jsoncpp throw out of load_settings (7413d96).
    const std::vector<std::string> bad = {
        R"({"display": {"bezel_index": "2"}})",
        R"({"display": [1, 2, 3]})",
        R"([1, 2, 3])",
        R"({"playback": {"shuffle": "yes"}})",
        R"({"playback": {"master_volume": 1e30}})",
        R"({"audio": {"retroarch_volume_offset_db": "loud"}})",
        R"({"display": {"scanline_intensity": {"nested": true}}})",
    };
    for (const auto& text : bad) {
        INFO(text);
        SettingsSandbox sb;
        sb.write(text);
        AppState s;
        bool ok = true;
        REQUIRE_NOTHROW(ok = static_cast<bool>(SettingsPersistence::load_settings(s)));
        CHECK_FALSE(ok);
        // Moved aside for diagnosis; the live path is free for a clean save.
        CHECK(fs::exists(sb.corrupt()));
        CHECK_FALSE(fs::exists(sb.file));
        CHECK(sb.read(sb.corrupt()) == text);

        REQUIRE(SettingsPersistence::save_settings(s));
        REQUIRE(saved_to_disk());
        CHECK(fs::exists(sb.file));
        CHECK(sb.read(sb.corrupt()) == text);   // never overwritten
        AppState again;
        CHECK(SettingsPersistence::load_settings(again));
    }
}

TEST_CASE("settings: convertible type mismatches load leniently", "[settings]") {
    // jsoncpp converts these instead of throwing; they must not quarantine
    // a file the operator only slightly mistyped.
    SettingsSandbox sb;
    sb.write(R"({"audio": {"output": 7}, "playback": {"playlist_loop": 0, "master_volume": 55.0}})");
    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.audio_settings.output == AudioOutput::AUTO);   // "7" is no known output
    CHECK_FALSE(s.playlist_loop);
    CHECK(s.master_volume == 55);
    CHECK_FALSE(fs::exists(sb.corrupt()));
}

TEST_CASE("settings: syntactically broken JSON fails without changing state", "[settings]") {
    SettingsSandbox sb;
    sb.write(R"({"display": {"mode": "modern_tv", )");
    AppState s;
    s.master_volume = 42;
    CHECK_FALSE(SettingsPersistence::load_settings(s));
    CHECK(s.master_volume == 42);
    CHECK(s.display_settings.mode == DisplayMode::CRT_NATIVE);
}

TEST_CASE("settings: peek_is_crt_native answers CRT for every unreadable shape",
          "[settings]") {
    SettingsSandbox sb;
    for (const char* text : {
             "",
             "{ not json",
             "[]",
             R"({"display": []})",
             R"({"display": {}})",
             R"({"display": {"mode": 5}})",
             R"({"display": {"mode": "crt_native"}})",
         }) {
        INFO(text);
        sb.write(text);
        CHECK(SettingsPersistence::peek_is_crt_native());
    }
    sb.write(R"({"display": {"mode": "modern_tv"}})");
    CHECK_FALSE(SettingsPersistence::peek_is_crt_native());
}

TEST_CASE("settings: loading an audio section applies the output", "[settings]") {
    SettingsSandbox sb;
    sb.write(R"({"audio": {"output": "hdmi", "retroarch_volume_offset_db": -3.0}})");
    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.audio_settings.output == AudioOutput::HDMI);
    REQUIRE_FALSE(sb.pactl_calls.empty());
    CHECK(sb.pactl_calls[1] == std::vector<std::string>{
              "pactl", "set-default-sink", "alsa_output.platform-fef00700.hdmi.hdmi-stereo"});
}

// ── Background writer ───────────────────────────────────────────────────
// Every save_settings() call site is on the render loop, which pings the
// systemd watchdog (WatchdogSec=10). The old save did tmp-create + fsync +
// rename + directory fsync right there; with the SD card saturated by a
// Content Manager upload, kiosk_status.json's plain create+rename was
// sampled at ~6 s per call (2026-10-26, five watchdog SIGABRTs in fifteen
// minutes). These pin what replaced it.

namespace {

// A FileSink that can be parked mid-write (a card stuck behind a writeback
// queue) and told to fail. Records everything it is handed.
struct StalledSettingsSink {
    std::mutex mu;
    std::condition_variable cv;
    bool released = false;
    bool entered = false;
    int fail_next = 0;
    std::vector<std::string> paths;
    std::vector<std::string> bodies;

    void install() {
        SettingsPersistence::set_file_sink_for_tests(
            [this](const std::string& path, const std::string& body) {
                std::unique_lock<std::mutex> lk(mu);
                entered = true;
                cv.notify_all();
                cv.wait(lk, [this] { return released; });
                paths.push_back(path);
                bodies.push_back(body);
                if (fail_next > 0) { --fail_next; return false; }
                return true;
            });
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
    std::vector<std::string> seen() {
        std::lock_guard<std::mutex> lk(mu);
        return bodies;
    }
};

std::string body_with_volume(int volume) {
    AppState s;
    s.master_volume = volume;
    return SettingsPersistence::serialize(s);
}

}  // namespace

TEST_CASE("settings: save_settings never waits for the disk, and the newest "
          "snapshot wins",
          "[settings][io]") {
    SettingsSandbox sb;
    StalledSettingsSink sink;
    sink.install();

    AppState s;
    s.master_volume = 1;
    REQUIRE(SettingsPersistence::save_settings(s));
    sink.wait_entered();  // the writer is now stuck "in fsync"
    CHECK(SettingsPersistence::save_pending());

    // An operator leaning on a toggle while the card is stuck: with the
    // old synchronous save every press blocked the render loop.
    const auto t0 = std::chrono::steady_clock::now();
    for (int v = 2; v <= 40; ++v) {
        s.master_volume = v;
        REQUIRE(SettingsPersistence::save_settings(s));
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 500ms);

    // Shutdown's flush is bounded: a wedged card cannot hold it.
    CHECK_FALSE(SettingsPersistence::flush_pending_saves(20ms));

    sink.release();
    REQUIRE(SettingsPersistence::flush_pending_saves(10s));
    CHECK_FALSE(SettingsPersistence::save_pending());

    // The write in flight, then ONLY the newest state — never a backlog,
    // and never an older snapshot after a newer one.
    const auto bodies = sink.seen();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0] == body_with_volume(1));
    CHECK(bodies[1] == body_with_volume(40));
    CHECK(sink.paths[1] == sb.file.string());
}

TEST_CASE("settings: the flush that runs before exit lands the last change "
          "on disk",
          "[settings][io]") {
    SettingsSandbox sb;
    AppState s;
    s.display_settings.mode = DisplayMode::MODERN_TV;
    s.master_volume = 12;
    REQUIRE(SettingsPersistence::save_settings(s));
    s.master_volume = 73;  // the LAST change before the stop
    REQUIRE(SettingsPersistence::save_settings(s));

    // What main() does before _exit().
    REQUIRE(SettingsPersistence::flush_pending_saves(10s));

    CHECK(sb.read(sb.file) == SettingsPersistence::serialize(s));
    CHECK_FALSE(fs::exists(sb.file.string() + ".tmp"));   // atomic: no debris
    CHECK_FALSE(SettingsPersistence::peek_is_crt_native());
    AppState back;
    REQUIRE(SettingsPersistence::load_settings(back));
    CHECK(back.master_volume == 73);
}

TEST_CASE("settings: a failed save is retried once by the flush",
          "[settings][io]") {
    SettingsSandbox sb;
    StalledSettingsSink sink;
    sink.release();          // never stalls in this test
    sink.install();
    AppState s;
    s.master_volume = 55;

    SECTION("the retry succeeds") {
        sink.fail_next = 1;  // e.g. the card was full at that moment
        REQUIRE(SettingsPersistence::save_settings(s));
        CHECK(SettingsPersistence::flush_pending_saves(10s));
        const auto bodies = sink.seen();
        REQUIRE(bodies.size() == 2);          // the failure, then the retry
        CHECK(bodies[1] == body_with_volume(55));
    }
    SECTION("the retry fails too: reported, not looped") {
        sink.fail_next = 2;
        REQUIRE(SettingsPersistence::save_settings(s));
        CHECK_FALSE(SettingsPersistence::flush_pending_saves(10s));
        CHECK(sink.seen().size() == 2);
        // Nothing is left spinning, and a later save is a fresh attempt.
        CHECK_FALSE(SettingsPersistence::save_pending());
        REQUIRE(SettingsPersistence::save_settings(s));
        CHECK(SettingsPersistence::flush_pending_saves(10s));
        CHECK(sink.seen().size() == 3);
    }
    SECTION("a newer successful save makes the older failure moot") {
        sink.fail_next = 1;
        REQUIRE(SettingsPersistence::save_settings(s));
        while (SettingsPersistence::save_pending()) std::this_thread::yield();
        s.master_volume = 56;
        REQUIRE(SettingsPersistence::save_settings(s));
        CHECK(SettingsPersistence::flush_pending_saves(10s));
        CHECK(sink.seen().size() == 2);       // no third, redundant write
    }
}

TEST_CASE("settings: a real write failure leaves the existing file alone",
          "[settings][io]") {
    SettingsSandbox sb;
    sb.write(R"({"playback": {"master_volume": 31}})");
    // The tmp path is occupied by a directory, so the tmp file cannot be
    // created — the durable writer's own failure path, not a fake.
    fs::create_directories(sb.file.string() + ".tmp");

    AppState s;
    s.master_volume = 99;
    REQUIRE(SettingsPersistence::save_settings(s));   // queued fine
    CHECK_FALSE(SettingsPersistence::flush_pending_saves(10s));

    AppState back;
    REQUIRE(SettingsPersistence::load_settings(back));
    CHECK(back.master_volume == 31);                  // old file intact
}

TEST_CASE("settings: a reload that overtakes an unfinished save writes the "
          "reloaded settings back",
          "[settings][io]") {
    // The web admin's backup restore replaces settings.json and pokes the
    // kiosk. A save of ours still in the air predates the restore; landing
    // after the reload it would leave the disk on settings the kiosk no
    // longer has in memory.
    SettingsSandbox sb;
    StalledSettingsSink sink;
    sink.install();

    AppState s;
    s.master_volume = 10;
    REQUIRE(SettingsPersistence::save_settings(s));   // in flight, stuck
    sink.wait_entered();
    s.master_volume = 11;
    REQUIRE(SettingsPersistence::save_settings(s));   // queued behind it

    sb.write(R"({"playback": {"master_volume": 88}})");   // the restore
    REQUIRE(SettingsPersistence::reload_from_disk(s));
    CHECK(s.master_volume == 88);

    sink.release();
    REQUIRE(SettingsPersistence::flush_pending_saves(10s));
    const auto bodies = sink.seen();
    // The stuck pre-restore save, then the RELOADED state — the queued
    // pre-restore snapshot (volume 11) was replaced, never written.
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0] == body_with_volume(10));
    CHECK(bodies[1] == SettingsPersistence::serialize(s));
    CHECK(bodies[1] == body_with_volume(88));
}

TEST_CASE("settings: a reload with nothing outstanding leaves the restored "
          "file byte for byte",
          "[settings][io]") {
    SettingsSandbox sb;
    const std::string restored =
        R"({"playback": {"master_volume": 88}, "future_key": 1})";
    sb.write(restored);
    AppState s;
    REQUIRE(SettingsPersistence::reload_from_disk(s));
    CHECK(s.master_volume == 88);
    CHECK_FALSE(SettingsPersistence::save_pending());
    CHECK(sb.read(sb.file) == restored);
}

TEST_CASE("settings: Guide Mode survives saves and unknown values return Live", "[settings]") {
    SettingsSandbox sb;
    AppState s;
    REQUIRE(s.display_settings.guide_mode == app::GuideMode::Live);
    s.display_settings.guide_mode = app::GuideMode::PickAhead;
    SettingsPersistence::save_settings(s); REQUIRE(saved_to_disk());
    AppState loaded; SettingsPersistence::load_settings(loaded);
    CHECK(sb.read(sb.file).find("pick_ahead") != std::string::npos);
    CHECK(loaded.display_settings.guide_mode == app::GuideMode::PickAhead);
    sb.write(R"({"display":{"guide_mode":"unrecognized"}})");
    SettingsPersistence::load_settings(loaded);
    CHECK(loaded.display_settings.guide_mode == app::GuideMode::Live);
    s.display_settings.guide_mode = app::GuideMode::Live;
    SettingsPersistence::save_settings(s); REQUIRE(saved_to_disk());
    SettingsPersistence::load_settings(loaded);
    CHECK(loaded.display_settings.guide_mode == app::GuideMode::Live);
}

TEST_CASE("settings: non-string Guide Modes use Live without rejecting other settings", "[settings]") {
    SettingsSandbox sb;
    for (const std::string raw : {"null", "true", "42", "[]", "{}"}) {
        sb.write("{\"display\":{\"guide_mode\":" + raw + ",\"menu_style\":\"channel_guide\"}}");
        AppState s; s.display_settings.guide_mode = app::GuideMode::PickAhead;
        REQUIRE(SettingsPersistence::load_settings(s));
        CHECK(s.display_settings.guide_mode == app::GuideMode::Live);
        CHECK(s.display_settings.menu_style == app::MenuStyle::ChannelGuide);
    }
}

// ---------------------------------------------------------------
// The CRT engine choice and the tube setting (display.crt_engine,
// display.tube). Persistence saves and loads the user's choice as it is:
// the board gate is applied at draw time, never here.
// ---------------------------------------------------------------

TEST_CASE("settings: the engine and a moved tube round-trip", "[settings][tube]") {
    SettingsSandbox sb;
    AppState a;
    a.display_settings.set_crt_engine(app::CrtEngine::Tube);
    tube::select_look(a.display_settings.tube, tube::Look::tv);
    REQUIRE(tube::set_row(a.display_settings.tube, tube::Effect::hum, 20));
    REQUIRE(tube::set_row(a.display_settings.tube, tube::Effect::afterglow, 35));
    tube::set_mask(a.display_settings.tube, tube::Mask::dots);
    REQUIRE(SettingsPersistence::save_settings(a));
    REQUIRE(saved_to_disk());

    AppState b;
    REQUIRE(SettingsPersistence::load_settings(b));
    CHECK(b.display_settings.crt_engine == app::CrtEngine::Tube);
    CHECK(b.display_settings.enhanced_crt_enabled);
    CHECK(b.display_settings.tube == a.display_settings.tube);

    const Json::Value display = sb.json()["display"];
    CHECK(display["enhanced_crt_enabled"].asBool());
    CHECK(display["crt_engine"].asString() == "tube");
    CHECK(display["tube"]["look"].asString() == "tv");
    CHECK(display["tube"]["mask"].asString() == "dots");
    const Json::Value& rows = display["tube"]["rows"];
    REQUIRE(rows.isObject());
    CHECK(rows.getMemberNames() == std::vector<std::string>{"afterglow", "hum"});
    CHECK(rows["hum"].asInt() == 20);
    CHECK(rows["afterglow"].asInt() == 35);
}

TEST_CASE("settings: an untouched look writes no rows and no mask", "[settings][tube]") {
    SettingsSandbox sb;
    AppState a;
    tube::select_look(a.display_settings.tube, tube::Look::arcade);
    REQUIRE(SettingsPersistence::save_settings(a));
    REQUIRE(saved_to_disk());

    const Json::Value display = sb.json()["display"];
    CHECK(display["crt_engine"].asString() == "classic");
    CHECK_FALSE(display["enhanced_crt_enabled"].asBool());
    const Json::Value& t = display["tube"];
    REQUIRE(t.isObject());
    CHECK(t["look"].asString() == "arcade");
    CHECK_FALSE(t.isMember("mask"));
    CHECK((!t.isMember("rows") || t["rows"].empty()));

    AppState b;
    REQUIRE(SettingsPersistence::load_settings(b));
    CHECK(b.display_settings.tube == a.display_settings.tube);
}

TEST_CASE("settings: a file older than crt_engine takes the engine from the old flag",
          "[settings][tube]") {
    SettingsSandbox sb;
    sb.write(R"({"display": {"enhanced_crt_enabled": true}})");
    AppState on;
    REQUIRE(SettingsPersistence::load_settings(on));
    CHECK(on.display_settings.crt_engine == app::CrtEngine::Enhanced);
    CHECK(on.display_settings.enhanced_crt_enabled);
    CHECK(on.display_settings.tube == tube::TubeSetting{});

    sb.write(R"({"display": {"enhanced_crt_enabled": false}})");
    AppState off;
    off.display_settings.set_crt_engine(app::CrtEngine::Tube);  // overwritten by the file
    REQUIRE(SettingsPersistence::load_settings(off));
    CHECK(off.display_settings.crt_engine == app::CrtEngine::Classic);
    CHECK_FALSE(off.display_settings.enhanced_crt_enabled);
    CHECK(off.display_settings.tube == tube::TubeSetting{});
}

TEST_CASE("settings: a valid crt_engine wins over an old flag that disagrees with it",
          "[settings][tube]") {
    SettingsSandbox sb;
    sb.write(R"({"display": {"crt_engine": "classic", "enhanced_crt_enabled": true}})");
    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.display_settings.crt_engine == app::CrtEngine::Classic);
    CHECK_FALSE(s.display_settings.enhanced_crt_enabled);   // the flag follows the engine
}

TEST_CASE("settings: with Tube offered, every settings file draws the engine it drew before",
          "[settings][tube]") {
    // The switch-on offers Tube on a Pi 5 and never selects it: a box with no
    // saved engine, a saved Classic or a saved Enhanced (in either spelling) loads
    // and draws exactly what it did before. Only an explicit "tube" draws Tube.
    struct Case { const char* body; app::CrtEngine draws; };
    const Case cases[] = {
        {nullptr, app::CrtEngine::Classic},                                   // no settings file
        {R"({"display": {"mode": "modern_tv"}})", app::CrtEngine::Classic},   // no engine key at all
        {R"({"display": {"enhanced_crt_enabled": false}})", app::CrtEngine::Classic},
        {R"({"display": {"enhanced_crt_enabled": true}})", app::CrtEngine::Enhanced},
        {R"({"display": {"crt_engine": "classic", "enhanced_crt_enabled": false}})", app::CrtEngine::Classic},
        {R"({"display": {"crt_engine": "enhanced", "enhanced_crt_enabled": true}})", app::CrtEngine::Enhanced},
        {R"({"display": {"crt_engine": "plasma", "enhanced_crt_enabled": true}})", app::CrtEngine::Enhanced},
        {R"({"display": {"crt_engine": "tube", "enhanced_crt_enabled": true}})", app::CrtEngine::Tube},
    };
    LogCapture log;   // "plasma" is warned about (and dropped)
    for (const Case& c : cases) {
        SettingsSandbox sb;
        if (c.body) sb.write(c.body);
        AppState s;
        s.platform_profile = platform::profile_for(platform::PiModel::Pi5);
        REQUIRE(app::tube_offered(s));
        REQUIRE(SettingsPersistence::load_settings(s));
        CHECK(app::effective_crt_engine(s.display_settings.crt_engine, app::tube_offered(s)) == c.draws);
    }
}

TEST_CASE("settings: an older kiosk after a rollback still sees Tube as enhanced",
          "[settings][tube]") {
    // A rolled-back kiosk knows only enhanced_crt_enabled. Tube must read
    // there as the nearest thing it has: the Enhanced engine.
    SettingsSandbox sb;
    AppState a;
    a.display_settings.set_crt_engine(app::CrtEngine::Tube);
    REQUIRE(SettingsPersistence::save_settings(a));
    REQUIRE(saved_to_disk());
    const Json::Value display = sb.json()["display"];
    REQUIRE(display["enhanced_crt_enabled"].isBool());
    CHECK(display["enhanced_crt_enabled"].asBool());
}

TEST_CASE("settings: bad tube values are dropped and nothing else is lost",
          "[settings][tube]") {
    SettingsSandbox sb;
    sb.write(R"({"display": {"bezel_index": 2, "crt_engine": "plasma",
        "enhanced_crt_enabled": true,
        "tube": {"look": "vhs", "mask": "Slot",
                 "rows": {"hum": "20", "noise": 101, "wobble": 5, "afterglow": 35}}}})");
    LogCapture log;
    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    CHECK(s.display_settings.bezel_index == 2);
    CHECK(s.display_settings.crt_engine == app::CrtEngine::Enhanced);
    CHECK(s.display_settings.enhanced_crt_enabled);
    CHECK(s.display_settings.tube.look == tube::Look::pvm);
    CHECK_FALSE(s.display_settings.tube.mask.has_value());
    CHECK(moved_row_count(s.display_settings.tube) == 1);
    CHECK(row(s.display_settings.tube, tube::Effect::afterglow) == 35);
    CHECK_FALSE(fs::exists(sb.corrupt()));

    // One warning per dropped value, each naming what it dropped.
    CHECK(log.warnings() == 6);
    for (const char* word : {"crt_engine", "vhs", "Slot", "hum", "noise", "wobble"}) {
        INFO(word);
        CHECK(log.mentions(word));
    }
}

TEST_CASE("settings: a tube of the wrong shape loads as the default", "[settings][tube]") {
    for (const char* text : {
             R"({"display": {"bezel_index": 2, "tube": 5}})",
             R"({"display": {"bezel_index": 2, "tube": {"rows": []}}})",
             R"({"display": {"bezel_index": 2, "tube": {"look": 3, "mask": [], "rows": {"hum": 2.5}}}})",
         }) {
        INFO(text);
        SettingsSandbox sb;
        sb.write(text);
        LogCapture log;
        AppState s;
        tube::select_look(s.display_settings.tube, tube::Look::tv);  // overwritten by the file
        REQUIRE(SettingsPersistence::load_settings(s));
        CHECK(s.display_settings.bezel_index == 2);
        CHECK(s.display_settings.tube == tube::TubeSetting{});
        CHECK_FALSE(fs::exists(sb.corrupt()));
        CHECK(log.warnings() >= 1);
    }
}

TEST_CASE("settings: a save on a closed-gate board keeps the Tube choice",
          "[settings][tube]") {
    // A card moved from a Pi 5 to a Pi 4B: the Pi 4B draws Enhanced, but its
    // saves must hand the choice back intact if the card goes back.
    SettingsSandbox sb;
    sb.write(R"({"display": {"crt_engine": "tube", "enhanced_crt_enabled": true,
        "tube": {"look": "trinitron", "rows": {"noise": 70}}}})");
    AppState s;
    REQUIRE(SettingsPersistence::load_settings(s));
    REQUIRE(SettingsPersistence::save_settings(s));
    REQUIRE(saved_to_disk());

    AppState again;
    REQUIRE(SettingsPersistence::load_settings(again));
    CHECK(again.display_settings.crt_engine == app::CrtEngine::Tube);
    CHECK(again.display_settings.tube.look == tube::Look::trinitron);
    CHECK(moved_row_count(again.display_settings.tube) == 1);
    CHECK(row(again.display_settings.tube, tube::Effect::noise) == 70);
}

TEST_CASE("Marquee border cycles through the four installed styles and survives reload", "[settings]") {
    SettingsSandbox sb;
    AppState state;
    for (const std::string id : {"simple_walnut", "shaker_cherry", "pegged_oak", "walnut_brass"}) {
        const auto& frame = state.display_settings.marquee_frame();
        REQUIRE(frame.id == id);
        const auto asset = fs::path(__FILE__).parent_path().parent_path().parent_path()
            / "assets" / "marquee" / frame.file;
        REQUIRE(fs::is_regular_file(asset));
        REQUIRE(SettingsPersistence::save_settings(state));
        REQUIRE(saved_to_disk());
        AppState restored;
        REQUIRE(SettingsPersistence::load_settings(restored));
        REQUIRE(restored.display_settings.marquee_frame().id == id);
        state.display_settings.next_marquee_frame();
    }
    CHECK(state.display_settings.marquee_frame_index == 0);
    for (const std::string value : {"null", "42", "\"retired_frame\""}) {
        sb.write("{\"display\":{\"marquee_frame\":" + value + ",\"mb_playback_show_frame\":false}}");
        REQUIRE(SettingsPersistence::load_settings(state));
        CHECK(state.display_settings.marquee_frame_index == 0);
        CHECK_FALSE(state.display_settings.mb_playback_show_frame);
    }
}
