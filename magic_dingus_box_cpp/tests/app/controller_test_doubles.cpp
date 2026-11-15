// Link-level doubles for the parts of app::Controller that live in the
// kiosk-only controller.cpp. See controller_test_doubles.h.

#include "controller_test_doubles.h"

#include <algorithm>
#include <filesystem>

#include "app/controller.h"
#include "retroarch/retroarch_launcher.h"

namespace test_doubles {

std::vector<std::string>& events() {
    static std::vector<std::string> log;
    return log;
}

bool& game_launch_ok() {
    static bool ok = true;
    return ok;
}

int count(const std::string& name) {
    const auto& e = events();
    return static_cast<int>(std::count(e.begin(), e.end(), name));
}

bool FakeVideoPlayer::load_file(const std::string& path, double start, double, bool) {
    events().push_back("load_file " + std::filesystem::path(path).filename().string());
    last_start = start;
    ++generation;
    if (fail_paths.count(path)) return false;
    playing = false;
    eos = false;
    error = false;
    position = 0.0;
    return true;
}

void FakeVideoPlayer::stop() {
    events().push_back("stop");
    playing = false;
    eos = false;
    error = false;
    position = 0.0;
    duration = 0.0;
}

}  // namespace test_doubles

// Controller is a member of no test target except through this file: its
// RetroArchLauncher member needs a constructor, and nothing here launches.
namespace retroarch {
RetroArchLauncher::RetroArchLauncher() : retroarch_available_(false) {}
}  // namespace retroarch

namespace app {

// controller.cpp's load_item_media, minus the waits: the same calls in the
// same order for a video item (stop, 200 ms settle, load from
// item.start + start_offset_s, play, the up-to-1000 ms wait for playback —
// which exits at once here because play() sets playing), a scripted result
// for a game.
utils::Result<> Controller::load_item_media(AppState&, const app::PlaylistItem& item,
                                            const std::string& playlist_directory,
                                            std::function<void()>,
                                            double start_offset_s) {
    using test_doubles::events;
    if (item.source_type == "local" || item.source_type == "video") {
        stop();
        events().push_back("settle");
        auto load_result = load_file_with_resolution(item.path, playlist_directory,
                                                     item.start + start_offset_s,
                                                     item.end, false);
        if (!load_result) {
            return utils::Result<>::fail("Failed to load playlist item: " + item.path +
                                         " (" + load_result.error() + ")");
        }
        play();
        return utils::Result<>::ok();
    }
    if (item.source_type == "emulated_game") {
        events().push_back("game " + item.title);
        if (test_doubles::game_launch_ok()) return utils::Result<>::ok();
        return utils::Result<>::fail("Failed to launch game: " + item.title);
    }
    return utils::Result<>::fail("Unsupported source type: " + item.source_type);
}

}  // namespace app
