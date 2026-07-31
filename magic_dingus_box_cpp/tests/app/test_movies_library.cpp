// Tests for the synthetic "Movies" playlist scanner (load_movies_library).
//
// The scanner shipped defaulting to /mnt/ssd/library/Movies — a layout Radarr
// never wrote to (setup_services.sh: movies land directly in
// ${STORAGE_ROOT}/library/<Title (Year)>/, "the earlier setup created
// library/Movies/ which then sat empty forever — drop it"). The result: the
// main-menu Movies row silently never appeared on fielded boxes. These tests
// pin the default root to Radarr's real root and the scan behavior against
// that layout, including the leftovers it must ignore.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/playlist_loader.h"

#ifdef MEDIA_BROWSER_ENABLED

namespace fs = std::filesystem;

namespace {

// Builds a scratch library root; caller adds movie dirs then scans it.
struct TempLibrary {
    fs::path root;

    TempLibrary() {
        static int counter = 0;
        root = fs::temp_directory_path() /
               ("mdb_movies_lib_test_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempLibrary() { fs::remove_all(root); }

    // "The Matrix (1999)" + "the.matrix.1999.mkv" -> full file path
    void add_movie(const std::string& dir_name, const std::string& file_name) {
        fs::create_directories(root / dir_name);
        std::ofstream(root / dir_name / file_name) << "x";
    }
};

} // namespace

TEST_CASE("default movies root matches Radarr's real library root") {
    // Must agree with RadarrClient::Config::host_library_prefix
    // ("/mnt/ssd/library/") and setup_services.sh's storage layout. The old
    // "/mnt/ssd/library/Movies" default pointed at a subdirectory Radarr
    // never populated.
    CHECK(std::string(app::PlaylistLoader::kMoviesLibraryRoot) ==
          "/mnt/ssd/library");
}

TEST_CASE("scans one-subdir-per-movie Radarr layout") {
    TempLibrary lib;
    lib.add_movie("The Matrix (1999)", "the.matrix.1999.mkv");
    lib.add_movie("Alien (1979)", "alien.1979.remastered.mp4");

    app::Playlist pl =
        app::PlaylistLoader::load_movies_library(lib.root.string());

    REQUIRE(pl.items.size() == 2);
    // Alphabetical, case-insensitive; titles come from the directory names.
    CHECK(pl.items[0].title == "Alien (1979)");
    CHECK(pl.items[1].title == "The Matrix (1999)");
    CHECK(pl.items[0].source_type == "local");
    CHECK(pl.items[1].path ==
          (lib.root / "The Matrix (1999)" / "the.matrix.1999.mkv").string());
}

TEST_CASE("ignores the legacy empty Movies/ leftover and non-movie clutter") {
    TempLibrary lib;
    lib.add_movie("Heat (1995)", "heat.1995.m4v");
    // Legacy pre-created subdir: no video directly inside it (old layout put
    // movies another level down, and on real boxes it was simply empty).
    fs::create_directories(lib.root / "Movies");
    // A dir with no video file at all must be skipped, not crash the scan.
    fs::create_directories(lib.root / "backups");
    std::ofstream(lib.root / "backups" / "notes.txt") << "not a movie";
    // Loose files at the root are not movie dirs.
    std::ofstream(lib.root / "stray.mkv") << "x";

    app::Playlist pl =
        app::PlaylistLoader::load_movies_library(lib.root.string());

    REQUIRE(pl.items.size() == 1);
    CHECK(pl.items[0].title == "Heat (1995)");
}

TEST_CASE("missing library root yields an empty playlist, no throw") {
    app::Playlist pl = app::PlaylistLoader::load_movies_library(
        (fs::temp_directory_path() / "mdb_movies_does_not_exist").string());
    CHECK(pl.items.empty());
    CHECK(pl.title == "Movies");
}

TEST_CASE("video extension match is case-insensitive") {
    TempLibrary lib;
    lib.add_movie("Tron (1982)", "TRON.1982.MKV");

    app::Playlist pl =
        app::PlaylistLoader::load_movies_library(lib.root.string());

    REQUIRE(pl.items.size() == 1);
    CHECK(pl.items[0].title == "Tron (1982)");
}

#endif // MEDIA_BROWSER_ENABLED
