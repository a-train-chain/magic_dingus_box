#include <catch2/catch_test_macros.hpp>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <sys/resource.h>
#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
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

namespace {
std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
void write_all(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}
struct LogSilence {  // begin() on a tripped guard warns; keep the test output clean
    std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
    std::ostringstream out;
    LogSilence() {
        auto l = std::make_shared<spdlog::logger>("q", std::make_shared<spdlog::sinks::ostream_sink_mt>(out));
        spdlog::set_default_logger(l);
    }
    ~LogSilence() { spdlog::set_default_logger(previous); }
};
}  // namespace

TEST_CASE("tube build guard: a tripped guard never writes or removes the marker", "[tube]") {
    LogSilence quiet;
    const auto path = temp_marker("mdb-tube-build-test-stay");
    write_all(path, "sig_guns_chroma");
    app::TubeBuildGuard a(path);
    REQUIRE(a.tripped());
    a.begin("glass"); a.end();
    REQUIRE(read_all(path) == "sig_guns_chroma");
    REQUIRE(a.tripped()); REQUIRE(a.tripped_program() == "sig_guns_chroma");
    app::TubeBuildGuard fresh(path);
    REQUIRE(fresh.tripped()); REQUIRE(fresh.tripped_program() == "sig_guns_chroma");
    std::filesystem::remove(path);                                            // someone else removes it
    REQUIRE(a.tripped());
    REQUIRE_NOTHROW(a.end());
    std::filesystem::remove(path);
}

TEST_CASE("tube build guard: end without begin removes nothing; begin twice then end leaves no file", "[tube]") {
    const auto path = temp_marker("mdb-tube-build-test-arm");
    std::filesystem::remove(path);
    app::TubeBuildGuard g(path);
    write_all(path, "not ours");                                              // appears after construction
    g.end();
    REQUIRE(read_all(path) == "not ours");
    std::filesystem::remove(path);
    g.begin("a"); g.begin("b");
    REQUIRE(read_all(path) == "b");
    g.end();
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("tube build guard: the program name is safe to log", "[tube]") {
    const auto path = temp_marker("mdb-tube-build-test-safe");
    write_all(path, std::string("a\nb\x01" "c\xff") + std::string(400, 'z'));
    app::TubeBuildGuard g(path);
    REQUIRE(g.tripped());
    REQUIRE(g.tripped_program().size() == 256);
    REQUIRE(g.tripped_program().substr(0, 8) == "a?b?c?zz");
    for (const char c : g.tripped_program()) REQUIRE((c >= 0x20 && c <= 0x7E));
    std::filesystem::remove(path);
}

TEST_CASE("tube build guard: a path under a regular file is no marker", "[tube]") {
    const auto file = temp_marker("mdb-tube-build-test-notdir");
    write_all(file, "x");
    REQUIRE_FALSE(app::TubeBuildGuard(file + "/marker").tripped());
    std::filesystem::remove(file);
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

TEST_CASE("tube build guard: a marker whose write fails after the open is removed and cannot trip", "[tube]") {
    // The open succeeds and the write fails part-way: a file-size limit (RLIMIT_FSIZE,
    // POSIX, so the same on the Mac and on the Pi) lets the first 8 bytes through and
    // fails the rest with EFBIG. SIGXFSZ, which the kernel sends then, is ignored for
    // the duration. Without the removal the partial file stays - and end() would not
    // remove it (the guard is not armed) - so the next start reads it as a hang and
    // keeps Tube off until a reboot.
    const auto path = temp_marker("mdb-tube-build-test-partial");
    std::filesystem::remove(path);
    std::ostringstream out;
    auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("capture", std::make_shared<spdlog::sinks::ostream_sink_mt>(out));
    spdlog::set_default_logger(logger);
    app::TubeBuildGuard g(path);
    REQUIRE_FALSE(g.tripped());
    rlimit saved{};
    REQUIRE(getrlimit(RLIMIT_FSIZE, &saved) == 0);
    rlimit small = saved;
    small.rlim_cur = 8;
    auto* const old_handler = std::signal(SIGXFSZ, SIG_IGN);
    REQUIRE(setrlimit(RLIMIT_FSIZE, &small) == 0);
    g.begin(std::string(64, 'p'));
    setrlimit(RLIMIT_FSIZE, &saved);
    std::signal(SIGXFSZ, old_handler);
    logger->flush();
    spdlog::set_default_logger(previous);
    REQUIRE(out.str().find("could not write") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE_FALSE(app::TubeBuildGuard(path).tripped());   // the next start is clean
    g.end();                                             // and end() has nothing to do
    REQUIRE_FALSE(std::filesystem::exists(path));
    std::filesystem::remove(path);
}

TEST_CASE("tube build guard: a re-open that fails keeps the marker this instance wrote", "[tube]") {
    // Armed, then begin() is called again (the undrawn programs changed) and the open
    // fails - the process is out of file descriptors. The marker on disk is still the
    // one this instance wrote, so end() must still remove it: otherwise the next start
    // reads it as a shader hang and keeps Tube off until a reboot. RLIMIT_NOFILE set to
    // the lowest free descriptor makes the next open fail with EMFILE, on macOS and
    // Linux alike, and nothing else is touched.
    LogSilence quiet;
    const auto path = temp_marker("mdb-tube-build-test-emfile");
    std::filesystem::remove(path);
    app::TubeBuildGuard g(path);
    REQUIRE_FALSE(g.tripped());
    g.begin("sig_lean out_lean");
    REQUIRE(read_all(path) == "sig_lean out_lean");
    rlimit saved{};
    REQUIRE(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    const int lowest_free = dup(0);
    REQUIRE(lowest_free >= 0);
    close(lowest_free);
    // Puts the limit back on every way out of the test (a failed REQUIRE throws), so a
    // failure here is one failure and not a run of confusing "cannot open" ones after it.
    struct RestoreNofile {
        rlimit saved;
        ~RestoreNofile() { setrlimit(RLIMIT_NOFILE, &saved); }
    } restore{saved};
    rlimit none_left = saved;
    none_left.rlim_cur = static_cast<rlim_t>(lowest_free);
    REQUIRE(setrlimit(RLIMIT_NOFILE, &none_left) == 0);
    REQUIRE_NOTHROW(g.begin("sig_rich out_rich"));
    REQUIRE(setrlimit(RLIMIT_NOFILE, &saved) == 0);   // before the reads below need a descriptor
    REQUIRE(quiet.out.str().find("could not write") != std::string::npos);
    REQUIRE(read_all(path) == "sig_lean out_lean");      // untouched: the open never happened
    g.end();
    REQUIRE_FALSE(std::filesystem::exists(path));        // ours, so removed
    std::filesystem::remove(path);
}

TEST_CASE("tube build guard: a re-open that fails on something not ours removes nothing", "[tube]") {
    // Armed, then someone replaces the marker with a directory, so the re-open fails.
    // What is at the path now was not written by this instance: end() leaves it (it
    // removes only a file holding the text this instance wrote, never a look-alike).
    LogSilence quiet;
    const auto path = temp_marker("mdb-tube-build-test-replaced");
    std::filesystem::remove_all(path);
    app::TubeBuildGuard g(path);
    g.begin("glass");
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    REQUIRE_NOTHROW(g.begin("glass sig_lean"));
    REQUIRE(quiet.out.str().find("could not write") != std::string::npos);
    REQUIRE_NOTHROW(g.end());
    REQUIRE(std::filesystem::is_directory(path));
    std::filesystem::remove_all(path);
    // The same with a regular file someone else wrote in place of ours.
    app::TubeBuildGuard h(path);
    h.begin("glass");
    std::filesystem::remove(path);
    write_all(path, "someone else");
    h.end();
    REQUIRE(read_all(path) == "someone else");
    std::filesystem::remove(path);
}
