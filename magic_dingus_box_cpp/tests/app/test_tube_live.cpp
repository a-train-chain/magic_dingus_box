// app/tube_live.h - the kiosk's poller for the web's op log (data/tube_request.json):
// a stat every 100 ms, a read only when the file's version changes, ops applied in
// order, settings saved once a second after the last change, nothing ever written.
#include <catch2/catch_test_macros.hpp>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

#include "app/app_state.h"
#include "app/redraw_gate.h"
#include "app/settings_persistence.h"
#include "app/tube_live.h"
#include "app/tube_ops.h"
#include "tube/tube_setting.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const app::TubeRequestPoller::Clock::time_point T0 = app::TubeRequestPoller::Clock::time_point{} + 1h;

// A fresh, empty directory per test; `path` is the op log inside it.
struct LiveDir {
    fs::path dir;
    fs::path path;
    LiveDir() {
        static int n = 0;
        dir = fs::temp_directory_path() /
              ("mdb-tube-live-" + std::to_string(::getpid()) + "-" + std::to_string(++n));
        fs::remove_all(dir);
        fs::create_directories(dir);
        path = dir / "tube_request.json";
    }
    ~LiveDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// Written the way the web admin does it: a temp file, then a rename over the log.
void write_text(const fs::path& path, const std::string& text) {
    const fs::path tmp = path.string() + ".tmp";
    { std::ofstream(tmp, std::ios::binary) << text; }
    fs::rename(tmp, path);
}
void write_log(const fs::path& path, const std::string& ops) {
    write_text(path, R"({"version":1,"ops":[)" + ops + "]}");
}
std::string row_op(int seq, int value) {
    return R"({"seq":)" + std::to_string(seq) + R"(,"op":"row","key":"hum","value":)" +
           std::to_string(value) + "}";
}
std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void offered(app::AppState& state) {
    state.platform_profile.tube_crt_engine = true;
    state.tube_renderer_ready = true;
}
int hum(const app::AppState& state) {
    return tube::resolve(state.display_settings.tube).effects[tube::index_of(tube::Effect::hum)];
}

// Counts settings saves instead of writing settings.json.
struct SaveCounter {
    std::shared_ptr<std::atomic<int>> saves = std::make_shared<std::atomic<int>>(0);
    SaveCounter() {
        auto counter = saves;
        app::SettingsPersistence::set_file_sink_for_tests(
            [counter](const std::string&, const std::string&) {
                ++*counter;
                return true;
            });
    }
    ~SaveCounter() {
        (void)app::SettingsPersistence::flush_pending_saves(1s);
        app::SettingsPersistence::set_file_sink_for_tests({});
    }
    int count() const {
        (void)app::SettingsPersistence::flush_pending_saves(1s);
        return saves->load();
    }
};

// Captures what the LOG_* macros write for the test's lifetime, so the
// warning can be counted and nothing reaches stdout.
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
        for (auto at = text.find("[warning]"); at != std::string::npos; at = text.find("[warning]", at + 1)) ++n;
        return n;
    }
};

}  // namespace

TEST_CASE("tube live: no file at start, then an op applies", "[tube]") {
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0)); REQUIRE(state.tube_live.adopted);
    write_log(path, row_op(1, 20));
    REQUIRE_FALSE(poller.poll(state, T0 + 50ms));          // inside the 100 ms period
    REQUIRE(poller.poll(state, T0 + 100ms));
    REQUIRE(hum(state) == 20); REQUIRE(state.tube_live.handled_seq == 1);
}

TEST_CASE("tube live: a file left from before a restart is adopted, not applied", "[tube]") {
    LiveDir d; const fs::path& path = d.path;
    write_log(path, row_op(9, 20));
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    REQUIRE(state.display_settings.tube == tube::TubeSetting{});
    REQUIRE(state.tube_live.handled_seq == 9);
    REQUIRE_FALSE(poller.poll(state, T0 + 1s));            // same version: not read again
}

TEST_CASE("tube live: two writes inside one mtime tick are both seen", "[tube]") {
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    write_log(path, row_op(1, 10));
    const auto t1 = fs::last_write_time(path);
    REQUIRE(poller.poll(state, T0 + 100ms));
    REQUIRE(hum(state) == 10);
    const auto size1 = fs::file_size(path);
    write_log(path, row_op(2, 15));                        // the same length
    fs::last_write_time(path, t1);                         // only the inode differs
    REQUIRE(fs::file_size(path) == size1);
    REQUIRE(poller.poll(state, T0 + 200ms));
    REQUIRE(hum(state) == 15); REQUIRE(state.tube_live.handled_seq == 2);
}

TEST_CASE("tube live: ops written between polls all land, in order", "[tube]") {
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    write_log(path, row_op(1, 10));
    write_log(path, row_op(1, 10) + R"(,{"seq":2,"op":"mask","mask":"dots"},)" + row_op(3, 20));
    REQUIRE(poller.poll(state, T0 + 100ms));
    REQUIRE(hum(state) == 20);
    REQUIRE(tube::resolve(state.display_settings.tube).mask == tube::Mask::dots);
    REQUIRE(state.tube_live.handled_seq == 3);
}

TEST_CASE("tube live: saves once, a second after the last change", "[tube]") {
    SaveCounter saves;
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    write_log(path, row_op(1, 10));
    REQUIRE(poller.poll(state, T0 + 100ms));
    write_log(path, row_op(1, 10) + "," + row_op(2, 15));
    REQUIRE(poller.poll(state, T0 + 600ms));
    poller.poll(state, T0 + 1500ms);
    REQUIRE(saves.count() == 0);
    poller.poll(state, T0 + 1600ms);
    REQUIRE(saves.count() == 1);
    poller.poll(state, T0 + 3s);
    REQUIRE(saves.count() == 1);
}

TEST_CASE("tube live: polling never writes", "[tube]") {
    SaveCounter saves;
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    write_log(path, row_op(1, 10));
    const std::string bytes = read_bytes(path);
    const auto mtime = fs::last_write_time(path);
    for (auto t = T0; t <= T0 + 900ms; t += 10ms) poller.poll(state, t);
    REQUIRE(hum(state) == 10);
    REQUIRE(saves.count() == 0);
    REQUIRE(read_bytes(path) == bytes);
    REQUIRE((fs::last_write_time(path) == mtime));  // file_time_type does not print
    poller.poll(state, T0 + 1100ms);                       // the only write: the save
    REQUIRE(saves.count() == 1);
    REQUIRE(read_bytes(path) == bytes);
}

TEST_CASE("tube live: flush saves a pending change at once", "[tube]") {
    SaveCounter saves;
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    write_log(path, row_op(1, 10));
    REQUIRE(poller.poll(state, T0 + 100ms));
    poller.flush(state);
    REQUIRE(saves.count() == 1);
    poller.poll(state, T0 + 5s);
    REQUIRE(saves.count() == 1);
    poller.flush(state);                                   // nothing armed: no save
    REQUIRE(saves.count() == 1);
}

TEST_CASE("tube live: a settings reload drops the pending save", "[tube]") {
    SaveCounter saves;
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    write_log(path, row_op(1, 10));
    REQUIRE(poller.poll(state, T0 + 100ms));
    poller.forget_pending_save();                          // settings.json was just restored
    poller.poll(state, T0 + 5s);
    poller.flush(state);
    REQUIRE(saves.count() == 0);
}

TEST_CASE("tube live: an unreadable file is skipped and the next good one applies", "[tube]") {
    LogCapture log;
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    write_text(path, "not json");
    REQUIRE_FALSE(poller.poll(state, T0 + 100ms));
    REQUIRE(state.display_settings.tube == tube::TubeSetting{});
    REQUIRE_FALSE(poller.poll(state, T0 + 200ms));         // the same bad version: one warning
    REQUIRE(log.warnings() == 1);
    write_log(path, row_op(1, 20));
    REQUIRE(poller.poll(state, T0 + 300ms));
    REQUIRE(hum(state) == 20);
}

TEST_CASE("tube live: a file over the size cap is refused unread", "[tube]") {
    LogCapture log;
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    std::string big = R"({"version":1,"ops":[)" + row_op(1, 20) + "]}";
    big.append(app::kTubeOpsMaxBytes, ' ');
    write_text(path, big);
    REQUIRE_FALSE(poller.poll(state, T0 + 100ms));
    REQUIRE(state.tube_live.handled_seq == 0);
    REQUIRE(log.warnings() == 1);
}

TEST_CASE("tube live: the file going away and coming back", "[tube]") {
    LiveDir d; const fs::path& path = d.path;
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    write_log(path, row_op(1, 10));
    REQUIRE_FALSE(poller.poll(state, T0));                 // adopted
    fs::remove(path);
    REQUIRE_FALSE(poller.poll(state, T0 + 100ms));
    write_log(path, row_op(1, 10) + "," + row_op(2, 30));
    REQUIRE(poller.poll(state, T0 + 200ms));
    REQUIRE(hum(state) == 30); REQUIRE(state.tube_live.handled_seq == 2);
}

TEST_CASE("tube live: not offered, ops are consumed and nothing changes", "[tube]") {
    SaveCounter saves;
    LiveDir d; const fs::path& path = d.path;
    app::AppState state;
    state.platform_profile.tube_crt_engine = true;
    state.tube_renderer_ready = false;
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    write_log(path, R"({"seq":1,"op":"engine","engine":"tube"})");
    REQUIRE_FALSE(poller.poll(state, T0 + 100ms));
    REQUIRE(state.display_settings.crt_engine == app::CrtEngine::Classic);
    REQUIRE(state.tube_live.handled_seq == 1);
    poller.poll(state, T0 + 5s);
    poller.flush(state);
    REQUIRE(saves.count() == 0);
}

TEST_CASE("tube live: any tube change moves the redraw signature", "[tube]") {
    app::AppState state; app::ContentSignature a, b, c, e, m, l;
    app::add_tube_redraw_signature(a, state);
    tube::set_row(state.display_settings.tube, tube::Effect::hum, 20); app::add_tube_redraw_signature(b, state);
    ++state.tube_live.degauss_requests;                                  app::add_tube_redraw_signature(c, state);
    REQUIRE(a.value() != b.value()); REQUIRE(b.value() != c.value());
    tube::set_mask(state.display_settings.tube, tube::Mask::dots);       app::add_tube_redraw_signature(m, state);
    tube::select_look(state.display_settings.tube, tube::Look::tv);      app::add_tube_redraw_signature(l, state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);         app::add_tube_redraw_signature(e, state);
    REQUIRE(c.value() != m.value()); REQUIRE(m.value() != l.value()); REQUIRE(l.value() != e.value());
}

TEST_CASE("tube live: a file there but unreadable at boot is adopted once readable, never applied", "[tube]") {
    LiveDir d; const fs::path& path = d.path;
    write_log(path, row_op(4, 20));
    fs::permissions(path, fs::perms::none);
    if (::access(path.c_str(), R_OK) == 0) SKIP("running as root: the file stays readable");
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    REQUIRE_FALSE(state.tube_live.adopted);                // cannot tell what it holds yet
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
    REQUIRE_FALSE(poller.poll(state, T0 + 100ms));         // same version, read now: adopted
    REQUIRE(state.tube_live.adopted);
    REQUIRE(state.tube_live.handled_seq == 4);
    REQUIRE(state.display_settings.tube == tube::TubeSetting{});
}
