#include <catch2/catch_test_macros.hpp>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "app/tube_build_guard.h"

namespace {
std::string temp_marker(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
}  // namespace

TEST_CASE("tube build guard: a marker left by a dead kiosk trips the guard", "[tube]") {
    const auto path = (std::filesystem::temp_directory_path() / "mdb-tube-build-test").string();
    std::filesystem::remove(path);
    { app::TubeBuildGuard g(path); REQUIRE_FALSE(g.tripped()); g.begin("sig_guns_chroma out_rich_raster_ag"); }  // died here
    app::TubeBuildGuard after(path);
    REQUIRE(after.tripped()); REQUIRE(after.tripped_program() == "sig_guns_chroma out_rich_raster_ag");
    std::filesystem::remove(path);
    app::TubeBuildGuard clean(path); clean.begin("glass"); clean.end();
    REQUIRE_FALSE(app::TubeBuildGuard(path).tripped());
}

TEST_CASE("tube build guard: an empty marker still trips, with a placeholder name", "[tube]") {
    const auto path = temp_marker("mdb-tube-build-test-empty");
    { std::ofstream(path, std::ios::trunc); }                                 // died between create and write
    app::TubeBuildGuard g(path);
    REQUIRE(g.tripped()); REQUIRE(g.tripped_program() == "unknown");
    std::filesystem::remove(path);
}

TEST_CASE("tube build guard: a directory at the marker path trips and does not throw", "[tube]") {
    const auto path = temp_marker("mdb-tube-build-test-dir");
    std::filesystem::remove_all(path);
    std::filesystem::create_directory(path);
    REQUIRE_NOTHROW(app::TubeBuildGuard(path));
    app::TubeBuildGuard g(path);
    REQUIRE(g.tripped()); REQUIRE(g.tripped_program() == "unknown");
    std::filesystem::remove_all(path);
}

TEST_CASE("tube build guard: a tripped guard stays tripped after end() or an outside removal", "[tube]") {
    const auto path = temp_marker("mdb-tube-build-test-stay");
    { app::TubeBuildGuard g(path); g.begin("glass"); }
    app::TubeBuildGuard a(path);
    REQUIRE(a.tripped());
    a.end();
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE(a.tripped()); REQUIRE(a.tripped_program() == "glass");
    { app::TubeBuildGuard g(path); g.begin("glass"); }
    app::TubeBuildGuard b(path);
    std::filesystem::remove(path);                                            // someone else removes it
    REQUIRE(b.tripped());
    REQUIRE_NOTHROW(b.end());                                                 // already gone
}

TEST_CASE("tube build guard: begin in a missing directory warns and continues", "[tube]") {
    std::ostringstream out;
    auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("capture", std::make_shared<spdlog::sinks::ostream_sink_mt>(out));
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);
    app::TubeBuildGuard g(temp_marker("mdb-tube-no-such-dir/marker"));
    REQUIRE_FALSE(g.tripped());
    REQUIRE_NOTHROW(g.begin("glass"));
    REQUIRE_NOTHROW(g.end());
    logger->flush();
    spdlog::set_default_logger(previous);
    REQUIRE(out.str().find("tube build guard") != std::string::npos);
}
