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
//     faked here so no real pactl ever runs).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "app/app_state.h"
#include "app/audio_router.h"
#include "app/settings_persistence.h"

namespace fs = std::filesystem;
using app::AppState;
using app::AudioOutput;
using app::DisplayMode;
using app::SettingsPersistence;

namespace {

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
};

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
    a.display_settings.enhanced_crt_enabled = true;
    a.display_settings.mb_playback_show_frame = false;
    a.display_settings.mb_scanline_intensity = 0.75f;
    a.display_settings.mb_flicker_intensity = 0.5f;
    a.display_settings.mb_library_sort = AppState::DisplaySettings::MbLibrarySort::Year;
    a.display_settings.mb_library_filter = AppState::DisplaySettings::MbLibraryFilter::MissingFiles;
    a.playlist_loop = false;
    a.shuffle = true;
    a.master_volume = 37;
    a.audio_settings.output = AudioOutput::HEADPHONE;
    a.audio_settings.retroarch_volume_offset_db = -6.0f;
    REQUIRE(SettingsPersistence::save_settings(a));

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
    CHECK_FALSE(b.display_settings.mb_playback_show_frame);
    CHECK(b.display_settings.mb_scanline_intensity == Catch::Approx(0.75f));
    CHECK(b.display_settings.mb_flicker_intensity == Catch::Approx(0.5f));
    CHECK(b.display_settings.mb_library_sort == AppState::DisplaySettings::MbLibrarySort::Year);
    CHECK(b.display_settings.mb_library_filter ==
          AppState::DisplaySettings::MbLibraryFilter::MissingFiles);
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
    CHECK(s.master_volume == 80);
    CHECK(s.audio_settings.output == AudioOutput::HDMI);
    CHECK_FALSE(fs::exists(sb.corrupt()));

    REQUIRE(SettingsPersistence::save_settings(s));
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
