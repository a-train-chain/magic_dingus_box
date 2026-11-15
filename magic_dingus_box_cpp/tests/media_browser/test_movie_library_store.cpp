#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "app/media_info.h"
#include "media_browser/channels/movie_library_store.h"

using app::LibraryMovie;
using app::MovieLibraryStore;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
LibraryMovie film(int id, const std::string& path) {
    return {id, "Movie " + std::to_string(id), 2001, {"Drama"}, "PG", path, 120.5, "h264"};
}
struct Sandbox {
    fs::path dir = fs::temp_directory_path() / ("mdb_movie_store_" + std::to_string(std::random_device{}()));
    fs::path file = dir / "movie_library.json";
    Sandbox() { fs::create_directories(dir); }
    ~Sandbox() { std::error_code ec; fs::remove_all(dir, ec); }
    void write(const std::string& body) { std::ofstream(file) << body; }
    std::string read() { std::ifstream in(file); std::ostringstream out; out << in.rdbuf(); return out.str(); }
};
struct FakeClock {
    std::atomic<int64_t> seconds{0};
    auto now() const { return std::chrono::steady_clock::time_point{} + std::chrono::seconds(seconds.load()); }
};
// External I/O can stall indefinitely. The store must leave its owner free
// and drain requests submitted while this operation is held.
struct Gate {
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    void hold() {
        std::unique_lock<std::mutex> lk(mu);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [this] { return released; });
    }
    bool wait_entered() {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, 2s, [this] { return entered; });
    }
    void release() { std::lock_guard<std::mutex> lk(mu); released = true; cv.notify_all(); }
    ~Gate() { release(); }
};
}

TEST_CASE("movie snapshot boot publishes only existing files after polling", "[movie_library_store]") {
    Sandbox box;
    box.write(app::serialize_movie_library({film(1, "/present.mp4"), film(2, "/absent.mp4")}));
    const auto owner = std::this_thread::get_id();
    bool exists_on_worker = true;
    MovieLibraryStore store(box.file.string(), [] { return std::nullopt; }, [&](const std::string& path) {
        exists_on_worker &= std::this_thread::get_id() != owner;
        return path == "/present.mp4";
    });
    store.start();
    CHECK_FALSE(store.first_load_finished());
    store.wait_idle_for_tests();
    CHECK(store.poll(false));
    REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 1);
    CHECK(store.first_load_finished());
    CHECK(exists_on_worker);
    CHECK_FALSE(store.poll(false));
    store.start(); // boot is idempotent; never replace a newer full list
    store.wait_idle_for_tests();
    CHECK_FALSE(store.poll(false));
}

TEST_CASE("missing or corrupt snapshots finish boot without movies", "[movie_library_store]") {
    Sandbox box;
    SECTION("missing") {}
    SECTION("corrupt") { box.write("not json"); }
    MovieLibraryStore store(box.file.string(), [] { return std::nullopt; }, [](const std::string&) { return true; });
    store.start();
    store.wait_idle_for_tests();
    CHECK_FALSE(store.poll(false));
    CHECK(store.first_load_finished());
    CHECK(store.movies().empty());
}

TEST_CASE("movie refresh waits for healthy tunnel and fifteen minutes after attempt completion", "[movie_library_store]") {
    Sandbox box;
    FakeClock clock;
    int fetches = 0;
    MovieLibraryStore store(box.file.string(), [&]() -> std::optional<std::vector<LibraryMovie>> {
        ++fetches;
        clock.seconds = 600; // the request took ten minutes
        return std::nullopt;
    }, [](const std::string&) { return true; }, nullptr, [&] { return clock.now(); });
    store.start();
    store.wait_idle_for_tests();
    store.poll(false);
    store.wait_idle_for_tests();
    CHECK(fetches == 0);
    store.poll(true);
    store.wait_idle_for_tests();
    CHECK(fetches == 1);
    clock.seconds = 1499;
    store.poll(true);
    store.wait_idle_for_tests();
    CHECK(fetches == 1);
    clock.seconds = 1500;
    store.poll(false);
    store.wait_idle_for_tests();
    CHECK(fetches == 1);
    store.poll(true);
    store.wait_idle_for_tests();
    CHECK(fetches == 2);
}

TEST_CASE("movie fetch writes the full snapshot on worker while publishing existing files", "[movie_library_store]") {
    Sandbox box;
    const auto owner = std::this_thread::get_id();
    bool worker_only = true;
    std::string written_path;
    std::string written_body;
    MovieLibraryStore store(box.file.string(), [&]() -> std::optional<std::vector<LibraryMovie>> {
        worker_only &= std::this_thread::get_id() != owner;
        return std::vector<LibraryMovie>{film(7, "/present.mp4"), film(8, "/missing.mp4")};
    }, [&](const std::string& path) {
        worker_only &= std::this_thread::get_id() != owner;
        return path == "/present.mp4";
    }, [&](const std::string& path, const std::string& body) {
        worker_only &= std::this_thread::get_id() != owner;
        written_path = path;
        written_body = body;
        return app::MediaInfoStore::write_file_durably(path, body);
    });
    store.start(); store.wait_idle_for_tests(); store.poll(true); store.wait_idle_for_tests();
    CHECK(store.poll(false));
    REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 7);
    CHECK(written_path == box.file.string());
    const auto saved = app::parse_movie_library(box.read());
    REQUIRE(saved.has_value());
    REQUIRE(saved->size() == 2);
    CHECK(saved->at(1).path == "/missing.mp4");
    CHECK(saved->at(0).length_s == 120.5);
    CHECK(written_body == box.read());
    CHECK(worker_only);
}

TEST_CASE("failed movie fetch preserves the current library and snapshot", "[movie_library_store]") {
    Sandbox box;
    const auto original = app::serialize_movie_library({film(1, "/old.mp4")});
    box.write(original);
    MovieLibraryStore store(box.file.string(), [] { return std::nullopt; }, [](const std::string&) { return true; });
    store.start(); store.wait_idle_for_tests(); store.poll(true); store.wait_idle_for_tests();
    CHECK_FALSE(store.poll(false));
    REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 1);
    CHECK(box.read() == original);
}

TEST_CASE("movie drive checks hide missing files immediately and recover the full list", "[movie_library_store]") {
    Sandbox box;
    box.write(app::serialize_movie_library({film(1, "/movie.mp4")}));
    bool present = true;
    bool mounted = true;
    bool drive_on_worker = true;
    int exists_calls = 0;
    const auto owner = std::this_thread::get_id();
    MovieLibraryStore store(box.file.string(), [] { return std::nullopt; }, [&](const std::string&) {
        ++exists_calls;
        return present;
    }, nullptr, nullptr, [&] {
        drive_on_worker &= std::this_thread::get_id() != owner;
        return mounted;
    });
    store.start(); store.wait_idle_for_tests(); store.poll(false);
    REQUIRE(store.movies().size() == 1);
    present = false;
    store.request_drive_check(); store.wait_idle_for_tests();
    CHECK(store.poll(false));
    CHECK(store.movies().empty());
    present = true;
    mounted = false;
    const auto calls_before_unmount = exists_calls;
    store.request_drive_check(); store.wait_idle_for_tests();
    CHECK_FALSE(store.poll(false));
    CHECK(exists_calls == calls_before_unmount);
    mounted = true;
    store.request_drive_check(); store.wait_idle_for_tests();
    CHECK(store.poll(false));
    REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 1);
    CHECK(drive_on_worker);
    REQUIRE(app::parse_movie_library(box.read())->size() == 1);
}

TEST_CASE("a failed movie snapshot write keeps the new list and retries on next fetch", "[movie_library_store]") {
    Sandbox box;
    box.write(app::serialize_movie_library({film(1, "/old.mp4")}));
    FakeClock clock;
    bool fail = true;
    MovieLibraryStore store(box.file.string(), []() -> std::optional<std::vector<LibraryMovie>> {
        return std::vector<LibraryMovie>{film(2, "/new.mp4")};
    }, [](const std::string&) { return true; }, [&](const std::string& path, const std::string& body) {
        return !fail && app::MediaInfoStore::write_file_durably(path, body);
    }, [&] { return clock.now(); });
    store.start(); store.wait_idle_for_tests(); store.poll(true); store.wait_idle_for_tests();
    CHECK(store.poll(false));
    REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 2);
    CHECK(app::parse_movie_library(box.read())->at(0).tmdb_id == 1);
    CHECK_FALSE(store.flush_until(std::chrono::steady_clock::now() + 1s));
    fail = false;
    clock.seconds = 900;
    store.poll(true); store.wait_idle_for_tests(); store.poll(false);
    CHECK(store.flush_until(std::chrono::steady_clock::now() + 1s));
    CHECK(app::parse_movie_library(box.read())->at(0).tmdb_id == 2);
}

TEST_CASE("a stalled movie fetch keeps poll nonblocking and retains queued drive checks", "[movie_library_store]") {
    Sandbox box;
    box.write(app::serialize_movie_library({film(1, "/old.mp4")}));
    Gate gate;
    std::atomic<bool> present{true};
    std::atomic<int> checks{0};
    int fetches = 0;
    MovieLibraryStore store(box.file.string(), [&]() -> std::optional<std::vector<LibraryMovie>> {
        ++fetches;
        gate.hold();
        return std::nullopt;
    }, [&](const std::string&) { ++checks; return present.load(); });
    store.start(); store.wait_idle_for_tests(); store.poll(true);
    const bool entered = gate.wait_entered();
    if (!entered) { gate.release(); FAIL("fetch never started"); }
    present = false;
    const auto before = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) { store.request_drive_check(); store.poll(true); }
    CHECK(std::chrono::steady_clock::now() - before < 100ms);
    CHECK_FALSE(store.flush_until(std::chrono::steady_clock::now() + 10ms));
    gate.release();
    store.wait_idle_for_tests();
    CHECK(store.poll(false));
    CHECK(store.movies().empty());
    CHECK(fetches == 1);
    CHECK(checks >= 2);
}

TEST_CASE("a queued fetch survives drive-check job replacement behind the boot read", "[movie_library_store]") {
    Sandbox box;
    box.write(app::serialize_movie_library({film(1, "/old.mp4")}));
    Gate gate;
    int fetches = 0;
    int checks = 0;
    MovieLibraryStore store(box.file.string(), [&]() -> std::optional<std::vector<LibraryMovie>> {
        ++fetches;
        return std::vector<LibraryMovie>{film(2, "/new.mp4")};
    }, [&](const std::string&) { if (++checks == 1) gate.hold(); return true; });
    store.start();
    const bool entered = gate.wait_entered();
    if (!entered) { gate.release(); FAIL("boot filter never started"); }
    store.poll(true); // fetch is pending behind the held read
    for (int i = 0; i < 20; ++i) store.request_drive_check(); // replaces its job
    gate.release();
    store.wait_idle_for_tests(); store.poll(false);
    REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 2);
    CHECK(fetches == 1);
}

TEST_CASE("movie store destruction drains pending snapshot write while members are alive", "[movie_library_store]") {
    Sandbox box;
    Gate gate;
    auto finished = std::async(std::launch::async, [&] {
        MovieLibraryStore store(box.file.string(), [&]() -> std::optional<std::vector<LibraryMovie>> {
            gate.hold();
            return std::vector<LibraryMovie>{film(3, "/movie.mp4")};
        }, [](const std::string&) { return true; });
        store.start(); store.wait_idle_for_tests(); store.poll(true);
        // Normal scope exit must join before the fields jobs use die.
    });
    const bool entered = gate.wait_entered();
    if (!entered) { gate.release(); FAIL("fetch never started"); }
    CHECK(finished.wait_for(10ms) == std::future_status::timeout);
    gate.release();
    REQUIRE(finished.wait_for(2s) == std::future_status::ready);
    finished.get();
    const auto saved = app::parse_movie_library(box.read());
    REQUIRE(saved.has_value());
    REQUIRE(saved->size() == 1);
    CHECK(saved->at(0).tmdb_id == 3);
}

TEST_CASE("snapshot reads never block the movie store owner", "[movie_library_store][fifo]") {
    Sandbox box;
    REQUIRE(::mkfifo(box.file.c_str(), 0600) == 0);
    MovieLibraryStore store(box.file.string(), [] { return std::nullopt; }, [](const std::string&) { return true; });
    const auto before = std::chrono::steady_clock::now();
    store.start();
    store.poll(false);
    CHECK(std::chrono::steady_clock::now() - before < 100ms);
    CHECK_FALSE(store.first_load_finished());
    // The FIFO reader and this writer meet only if start queued real I/O.
    int fd = -1;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
        fd = ::open(box.file.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd < 0) std::this_thread::yield();
    }
    REQUIRE(fd >= 0);
    const auto bytes = app::serialize_movie_library({film(4, "/movie.mp4")});
    const auto count = ::write(fd, bytes.data(), bytes.size());
    ::close(fd);
    REQUIRE(count == static_cast<ssize_t>(bytes.size()));
    store.wait_idle_for_tests();
    CHECK(store.poll(false));
    CHECK(store.first_load_finished());
    REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 4);
}

TEST_CASE("throwing movie fetches are rate limited and leave boot and cached movies usable", "[movie_library_store]") {
    Sandbox box;
    box.write(app::serialize_movie_library({film(1, "/movie.mp4")}));
    FakeClock clock;
    int attempts = 0;
    MovieLibraryStore store(box.file.string(), [&]() -> std::optional<std::vector<LibraryMovie>> {
        ++attempts;
        throw std::runtime_error("network failure");
    }, [](const std::string&) { return true; }, nullptr, [&] { return clock.now(); });
    store.start(); store.wait_idle_for_tests(); store.poll(true); store.wait_idle_for_tests();
    for (int i = 0; i < 20; ++i) store.poll(true);
    store.wait_idle_for_tests();
    CHECK(store.first_load_finished());
    REQUIRE(store.movies().size() == 1);
    CHECK(attempts == 1);
    clock.seconds = 900;
    store.poll(true); store.wait_idle_for_tests();
    CHECK(attempts == 2);
}

TEST_CASE("movie drive predicate failures keep the last published list", "[movie_library_store]") {
    Sandbox box;
    box.write(app::serialize_movie_library({film(1, "/movie.mp4")}));
    bool fail = false;
    MovieLibraryStore store(box.file.string(), [] { return std::nullopt; }, [&](const std::string&) {
        if (fail) throw std::runtime_error("drive I/O failure");
        return true;
    });
    store.start(); store.wait_idle_for_tests(); store.poll(false);
    fail = true;
    store.request_drive_check(); store.wait_idle_for_tests();
    CHECK_FALSE(store.poll(false));
    REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 1);
}

TEST_CASE("failed scheduled fetches recheck drive and recover cached films after remount", "[movie_library_store][refresh_drive]") {
    Sandbox box;
    const auto snapshot = app::serialize_movie_library({film(1, "/movie.mp4")});
    box.write(snapshot);
    FakeClock clock;
    bool mounted = true;
    int checks = 0;
    MovieLibraryStore store(box.file.string(), [] { return std::nullopt; },
        [](const std::string&) { return true; }, nullptr, [&] { return clock.now(); },
        [&] { ++checks; return mounted; });
    store.start(); store.wait_idle_for_tests(); store.poll(false);
    REQUIRE(store.movies().size() == 1);
    mounted = false;
    store.poll(true); store.wait_idle_for_tests();
    CHECK(store.poll(false)); CHECK(store.movies().empty());
    const int checks_after_failure = checks;
    mounted = true;
    for (int i = 0; i < 100; ++i) store.poll(true);
    store.wait_idle_for_tests();
    CHECK(checks == checks_after_failure); // no checks/retries between scheduled attempts
    clock.seconds = 900;
    store.poll(true); store.wait_idle_for_tests();
    CHECK(store.poll(false)); REQUIRE(store.movies().size() == 1);
    CHECK(store.movies()[0].tmdb_id == 1); CHECK(box.read() == snapshot);
}

TEST_CASE("offline periodic drive checks hide unmounted films and restore them without HTTP", "[movie_library_store][offline_drive]") {
    Sandbox box;
    box.write(app::serialize_movie_library({film(1, "/movie.mp4")}));
    FakeClock clock;
    bool mounted = true;
    int fetches = 0;
    int checks = 0;
    const auto owner = std::this_thread::get_id();
    bool worker_only = true;
    MovieLibraryStore store(box.file.string(), [&]() -> std::optional<std::vector<LibraryMovie>> {
        ++fetches; return std::nullopt;
    }, [](const std::string&) { return true; }, nullptr, [&] { return clock.now(); }, [&] {
        worker_only &= std::this_thread::get_id() != owner;
        ++checks; return mounted;
    });
    store.start(); store.wait_idle_for_tests(); store.poll(false);
    REQUIRE(store.movies().size() == 1);
    mounted = false; clock.seconds = 899;
    for (int i = 0; i < 100; ++i) store.poll(false);
    store.wait_idle_for_tests(); CHECK(checks == 1);
    clock.seconds = 900; store.poll(false); store.wait_idle_for_tests();
    CHECK(store.poll(false)); CHECK(store.movies().empty());
    mounted = true; clock.seconds = 1800;
    store.poll(false); store.wait_idle_for_tests();
    CHECK(store.poll(false)); REQUIRE(store.movies().size() == 1);
    CHECK(fetches == 0); CHECK(checks == 3); CHECK(worker_only);
}
