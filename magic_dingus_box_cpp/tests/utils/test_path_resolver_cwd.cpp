// resolve_video_path must never throw when the process's working directory
// has been deleted out from under it. Incident 2026-11-13: a deploy removed
// build/ (the running kiosk's WorkingDirectory) and the next relative-path
// resolve hit the throwing fs::current_path(), which aborted the kiosk.

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "utils/path_resolver.h"

namespace fs = std::filesystem;

namespace {
// chdir into a fresh temp dir and delete it; restore the old cwd on exit.
struct DeletedCwd {
    fs::path old_cwd;
    fs::path doomed;
    DeletedCwd() {
        old_cwd = fs::current_path();
        doomed = fs::temp_directory_path() / ("mdb_cwd_gone_" + std::to_string(::getpid()));
        fs::create_directories(doomed);
        REQUIRE(::chdir(doomed.c_str()) == 0);
        std::error_code ec;
        fs::remove_all(doomed, ec);
    }
    ~DeletedCwd() {
        (void)!::chdir(old_cwd.c_str());
    }
};
}  // namespace

TEST_CASE("resolve_video_path survives a deleted working directory") {
    fs::path root = fs::temp_directory_path() / ("mdb_resolve_" + std::to_string(::getpid()));
    fs::create_directories(root / "playlists");
    fs::create_directories(root / "media");
    { std::ofstream(root / "playlists" / "clip.mp4") << "x"; }

    DeletedCwd gone;
    std::error_code ec;
    REQUIRE(fs::current_path(ec).empty());  // cwd really is gone

    SECTION("relative path found via the playlist dir") {
        std::string out;
        REQUIRE_NOTHROW(out = utils::resolve_video_path("clip.mp4", (root / "playlists").string()));
        REQUIRE(out == fs::canonical(root / "playlists" / "clip.mp4").string());
    }

    SECTION("relative path that exists nowhere falls back to the input") {
        std::string out;
        REQUIRE_NOTHROW(out = utils::resolve_video_path("nope-xyz.mp4", (root / "playlists").string()));
        REQUIRE(out == "nope-xyz.mp4");
    }

    SECTION("no playlist dir and nothing found falls back to the input") {
        std::string out;
        REQUIRE_NOTHROW(out = utils::resolve_video_path("nope-xyz.mp4", ""));
        REQUIRE(out == "nope-xyz.mp4");
    }

    fs::remove_all(root, ec);
}
