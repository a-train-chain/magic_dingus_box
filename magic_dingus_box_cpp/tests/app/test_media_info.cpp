// The per-video media info store (app/media_info.h): <data>/media_info.json,
// shared by the kiosk and the web admin — each video's crop override plus
// its length and display aspect.
//
// tests/app/fixtures/media_info.json is the schema of record: the web
// admin's Python tests read the same file and assert the same values.
//
// What the store tests pin is the render-thread contract: set_crop() changes
// the in-memory answer at once and never waits for the card; every write is
// lock -> read -> merge -> write, so a web-admin write that landed between
// two kiosk toggles is kept; an edit the disk does not have yet survives a
// reload, a failed write and a replaced (dropped) worker job.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include <json/json.h>

#include "app/media_info.h"

namespace fs = std::filesystem;
using app::MediaInfo;
using app::MediaInfoMap;
using app::MediaInfoStore;
using namespace std::chrono_literals;

namespace {

const fs::path kFixture =
    fs::path(__FILE__).parent_path() / "fixtures" / "media_info.json";

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, const std::string& body) {
    std::ofstream(p, std::ios::binary | std::ios::trunc) << body;
}

// The file as the web admin would see it (raw JSON, unknown fields too).
Json::Value read_json(const fs::path& p) {
    Json::Value root;
    std::istringstream in(read_file(p));
    in >> root;
    return root;
}

// A fresh data directory holding media_info.json.
struct Sandbox {
    fs::path dir;
    fs::path file;
    Sandbox() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("mdb_media_info_" + std::to_string(rd()));
        fs::create_directories(dir);
        file = dir / "media_info.json";
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// A write hook that can hold writes mid-way (an SD card stuck behind a
// writeback queue) or make them fail (a full card). Otherwise it does the
// real durable write, so the file on disk is what the store produced.
struct HeldSink {
    std::mutex mu;
    std::condition_variable cv;
    bool gated = false;  // while true, each write waits for a permit
    int permits = 0;
    bool fail = false;
    int calls = 0;

    MediaInfoStore::FileSink sink() {
        return [this](const std::string& path, const std::string& body) {
            {
                std::unique_lock<std::mutex> lk(mu);
                ++calls;
                cv.notify_all();
                cv.wait(lk, [this] { return !gated || permits > 0; });
                if (gated) --permits;
                if (fail) return false;
            }
            return MediaInfoStore::write_file_durably(path, body);
        };
    }
    // Until the n-th write has started (and is waiting, when gated).
    void wait_calls(int n) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this, n] { return calls >= n; });
    }
    void hold() {
        std::lock_guard<std::mutex> lk(mu);
        gated = true;
        permits = 0;
    }
    // Lets exactly one held write through.
    void release_one() {
        std::lock_guard<std::mutex> lk(mu);
        ++permits;
        cv.notify_all();
    }
    void release() {
        std::lock_guard<std::mutex> lk(mu);
        gated = false;
        cv.notify_all();
    }
    void set_fail(bool f) {
        std::lock_guard<std::mutex> lk(mu);
        fail = f;
    }
    int call_count() {
        std::lock_guard<std::mutex> lk(mu);
        return calls;
    }
};

bool flushed(MediaInfoStore& store) {
    return store.flush_until(std::chrono::steady_clock::now() + 10s);
}

// flush, then adopt what the worker read.
void settle(MediaInfoStore& store) {
    REQUIRE(flushed(store));
    (void)store.poll();
}

}  // namespace

// ── The file format ──────────────────────────────────────────────────────

TEST_CASE("media_info: the shared fixture parses to its values", "[media_info]") {
    REQUIRE(fs::exists(kFixture));
    const auto parsed = app::parse_media_info(read_file(kFixture));
    REQUIRE(parsed.has_value());
    const MediaInfoMap& m = *parsed;
    REQUIRE(m.size() == 4);

    const MediaInfo& classic = m.at("media/classic.mp4");
    REQUIRE(classic.duration_s.has_value());
    CHECK(*classic.duration_s == Catch::Approx(212.4));
    REQUIRE(classic.display_aspect.has_value());
    CHECK(*classic.display_aspect == Catch::Approx(1.3333));
    CHECK_FALSE(classic.crop.has_value());

    const MediaInfo& wide = m.at("media/widescreen.mp4");
    CHECK(*wide.duration_s == Catch::Approx(95.5));
    CHECK(*wide.display_aspect == Catch::Approx(1.7778));
    REQUIRE(wide.crop.has_value());
    CHECK(*wide.crop == false);

    const MediaInfo& unprobed = m.at("media/not_probed_yet.mp4");
    CHECK_FALSE(unprobed.duration_s.has_value());
    CHECK_FALSE(unprobed.display_aspect.has_value());
    CHECK(unprobed.crop == std::optional<bool>(false));

    // An integer duration is a duration; keys keep spaces and subdirectories.
    const MediaInfo& longer = m.at("media/music videos/long one.mp4");
    CHECK(*longer.duration_s == Catch::Approx(3600.0));
    CHECK_FALSE(longer.crop.has_value());

    CHECK(app::crop_for(m, "media/classic.mp4"));
    CHECK_FALSE(app::crop_for(m, "media/widescreen.mp4"));
    CHECK_FALSE(app::crop_for(m, "media/not_probed_yet.mp4"));
}

TEST_CASE("media_info: serialize then parse gives the same map", "[media_info]") {
    MediaInfoMap m;
    m["media/a.mp4"].duration_s = 212.4;
    m["media/a.mp4"].display_aspect = 16.0 / 9.0;
    m["media/a.mp4"].crop = false;
    m["media/b c/d.mp4"].duration_s = 0.1;
    m["media/e.mp4"].display_aspect = 4.0 / 3.0;
    m["media/f.mp4"].crop = false;

    const std::string text = app::serialize_media_info(m);
    const auto back = app::parse_media_info(text);
    REQUIRE(back.has_value());
    CHECK(*back == m);

    // The documented shape: a version and a "videos" object.
    Json::Value root;
    std::istringstream in(text);
    in >> root;
    CHECK(root["version"].asInt() == 1);
    CHECK(root["videos"].isObject());
    CHECK(root["videos"]["media/a.mp4"]["crop"].asBool() == false);
    CHECK_FALSE(root["videos"]["media/e.mp4"].isMember("crop"));

    CHECK(app::parse_media_info(app::serialize_media_info({})) == MediaInfoMap{});
}

TEST_CASE("media_info: corrupt text is nullopt; empty text is an empty map",
          "[media_info]") {
    CHECK_FALSE(app::parse_media_info("{oops").has_value());
    CHECK_FALSE(app::parse_media_info("[]").has_value());
    CHECK_FALSE(app::parse_media_info(R"({"version": 1, "videos": []})").has_value());

    // A zero-length file (a power cut before the first write) is no data.
    REQUIRE(app::parse_media_info("").has_value());
    CHECK(app::parse_media_info("")->empty());
    REQUIRE(app::parse_media_info(" \n").has_value());
    CHECK(app::parse_media_info(" \n")->empty());

    // A field of the wrong type (or a nonsense value) reads as absent; the
    // rest of the file still loads.
    const auto lenient = app::parse_media_info(R"({"version": 1, "videos": {
        "media/a.mp4": {"crop": "no", "duration_s": -3, "display_aspect": 1.5},
        "media/b.mp4": 7,
        "media/c.mp4": {"crop": false}
    }})");
    REQUIRE(lenient.has_value());
    REQUIRE(lenient->count("media/a.mp4") == 1);
    CHECK_FALSE(lenient->at("media/a.mp4").crop.has_value());
    CHECK_FALSE(lenient->at("media/a.mp4").duration_s.has_value());
    CHECK(*lenient->at("media/a.mp4").display_aspect == Catch::Approx(1.5));
    CHECK(lenient->count("media/b.mp4") == 0);
    CHECK(lenient->at("media/c.mp4").crop == std::optional<bool>(false));
}

TEST_CASE("media_info: a video without a setting is cropped", "[media_info]") {
    CHECK(app::crop_for(MediaInfoMap{}, "media/a.mp4"));
    CHECK(app::crop_for(MediaInfoMap{}, ""));

    MediaInfoMap m;
    m["media/a.mp4"].duration_s = 10.0;  // an entry, but no crop field
    CHECK(app::crop_for(m, "media/a.mp4"));
    m["media/a.mp4"].crop = false;
    CHECK_FALSE(app::crop_for(m, "media/a.mp4"));
}

TEST_CASE("media_info: keys are paths relative to the data directory",
          "[media_info]") {
    CHECK(app::media_key_for("/opt/x/data/media/a.mp4", "/opt/x/data") == "media/a.mp4");
    CHECK(app::media_key_for("/tmp/a.mp4", "/opt/x/data") == "");

    // Same answer however the two paths are spelled.
    CHECK(app::media_key_for("/opt/x/data/media/a.mp4", "/opt/x/data/") == "media/a.mp4");
    CHECK(app::media_key_for("/opt/x/data/playlists/../media/a.mp4", "/opt/x/data") ==
          "media/a.mp4");
    CHECK(app::media_key_for("/opt/x/data/media/sub dir/b.mp4", "/opt/x/data") ==
          "media/sub dir/b.mp4");

    // Never a key for the directory itself, a sibling that shares its
    // prefix, or a relative path against an absolute directory.
    CHECK(app::media_key_for("/opt/x/data", "/opt/x/data") == "");
    CHECK(app::media_key_for("/opt/x/data2/a.mp4", "/opt/x/data") == "");
    CHECK(app::media_key_for("media/a.mp4", "/opt/x/data") == "");
    CHECK(app::media_key_for("/opt/x/data/media/a.mp4", "") == "");
}

// ── The store ────────────────────────────────────────────────────────────

TEST_CASE("media_info store: set_crop answers at once while the write is held",
          "[media_info]") {
    Sandbox sb;
    HeldSink held;
    held.hold();
    MediaInfoStore store(sb.file.string(), held.sink());

    const auto t0 = std::chrono::steady_clock::now();
    store.set_crop("media/a.mp4", false);
    held.wait_calls(1);  // the writer is now stuck "on the card"
    // More toggles while it is stuck. Each queues a job that REPLACES the
    // one not yet started — none of these edits may be lost with it.
    store.set_crop("media/b.mp4", false);
    store.set_crop("media/c.mp4", false);
    store.set_crop("media/c.mp4", true);
    store.set_crop("media/d.mp4", false);
    CHECK(std::chrono::steady_clock::now() - t0 < 500ms);

    CHECK_FALSE(store.crop_for("media/a.mp4"));
    CHECK_FALSE(store.crop_for("media/b.mp4"));
    CHECK(store.crop_for("media/c.mp4"));
    CHECK_FALSE(store.crop_for("media/d.mp4"));
    CHECK(store.map().count("media/c.mp4") == 0);
    CHECK_FALSE(store.flush_until(std::chrono::steady_clock::now() + 50ms));

    held.release();
    REQUIRE(flushed(store));
    // In flight, then ONE more write carrying every later edit.
    CHECK(held.call_count() == 2);
    const auto disk = app::parse_media_info(read_file(sb.file));
    REQUIRE(disk.has_value());
    CHECK_FALSE(app::crop_for(*disk, "media/a.mp4"));
    CHECK_FALSE(app::crop_for(*disk, "media/b.mp4"));
    CHECK(disk->count("media/c.mp4") == 0);
    CHECK_FALSE(app::crop_for(*disk, "media/d.mp4"));

    (void)store.poll();
    CHECK_FALSE(store.crop_for("media/a.mp4"));
    CHECK_FALSE(store.crop_for("media/d.mp4"));
}

TEST_CASE("media_info store: a web write between two kiosk toggles is kept",
          "[media_info]") {
    Sandbox sb;
    MediaInfoStore store(sb.file.string());
    store.request_reload();
    settle(store);

    store.set_crop("media/a.mp4", false);
    REQUIRE(flushed(store));

    // The web admin saves a new video's probe (it merges, like the kiosk).
    // The kiosk is not poked, so its map does not have this entry.
    Json::Value web = read_json(sb.file);
    web["videos"]["media/new.mp4"]["duration_s"] = 61.5;
    Json::StreamWriterBuilder b;
    write_file(sb.file, Json::writeString(b, web));

    store.set_crop("media/b.mp4", false);
    REQUIRE(flushed(store));

    const auto disk = app::parse_media_info(read_file(sb.file));
    REQUIRE(disk.has_value());
    CHECK_FALSE(app::crop_for(*disk, "media/a.mp4"));
    CHECK_FALSE(app::crop_for(*disk, "media/b.mp4"));
    REQUIRE(disk->count("media/new.mp4") == 1);
    CHECK(*disk->at("media/new.mp4").duration_s == Catch::Approx(61.5));

    // And the kiosk picked the web's entry up from its own write's read.
    CHECK(store.poll());
    REQUIRE(store.map().count("media/new.mp4") == 1);
    CHECK_FALSE(store.crop_for("media/a.mp4"));
}

TEST_CASE("media_info store: set_crop(true) removes the crop field",
          "[media_info]") {
    Sandbox sb;
    write_file(sb.file, R"({"version": 1, "videos": {
        "media/a.mp4": {"duration_s": 30.0, "crop": false},
        "media/b.mp4": {"crop": false}
    }})");
    MediaInfoStore store(sb.file.string());
    store.request_reload();
    settle(store);
    REQUIRE_FALSE(store.crop_for("media/a.mp4"));

    store.set_crop("media/a.mp4", true);
    store.set_crop("media/b.mp4", true);
    CHECK(store.crop_for("media/a.mp4"));
    CHECK(store.crop_for("media/b.mp4"));
    REQUIRE(flushed(store));

    const Json::Value root = read_json(sb.file);
    // The entry keeps what the web admin wrote; only crop goes.
    REQUIRE(root["videos"].isMember("media/a.mp4"));
    CHECK_FALSE(root["videos"]["media/a.mp4"].isMember("crop"));
    CHECK(root["videos"]["media/a.mp4"]["duration_s"].asDouble() == Catch::Approx(30.0));
    // An entry left with nothing in it is removed.
    CHECK_FALSE(root["videos"].isMember("media/b.mp4"));
}

TEST_CASE("media_info store: a reload while an edit is pending keeps the edit",
          "[media_info]") {
    Sandbox sb;
    write_file(sb.file, R"({"version": 1, "videos": {"media/b.mp4": {"duration_s": 10.0}}})");
    HeldSink sink;
    sink.set_fail(true);  // the card refuses the write (full, read-only)
    MediaInfoStore store(sb.file.string(), sink.sink());

    store.set_crop("media/a.mp4", false);
    // The write failed; the shutdown-style flush retries once, then says so.
    CHECK_FALSE(flushed(store));
    CHECK(sink.call_count() == 2);

    // What the worker read is adopted — and the edit the disk does not have
    // is laid back on top of it.
    CHECK(store.poll());
    CHECK(store.map().count("media/b.mp4") == 1);
    CHECK_FALSE(store.crop_for("media/a.mp4"));

    // The web admin changes the file and pokes the kiosk; the card is back.
    write_file(sb.file, R"({"version": 1, "videos": {"media/b.mp4": {"duration_s": 20.0}}})");
    sink.set_fail(false);
    store.request_reload();
    settle(store);

    CHECK(*store.map().at("media/b.mp4").duration_s == Catch::Approx(20.0));
    CHECK_FALSE(store.crop_for("media/a.mp4"));
    // The reload carried the edit to disk too.
    const auto disk = app::parse_media_info(read_file(sb.file));
    REQUIRE(disk.has_value());
    CHECK_FALSE(app::crop_for(*disk, "media/a.mp4"));
    CHECK(*disk->at("media/b.mp4").duration_s == Catch::Approx(20.0));
}

TEST_CASE("media_info store: an edit made while a write is in flight survives "
          "that write's result", "[media_info]") {
    Sandbox sb;
    HeldSink sink;
    sink.hold();
    MediaInfoStore store(sb.file.string(), sink.sink());

    store.set_crop("media/a.mp4", false);
    sink.wait_calls(1);              // job 1 carries only media/a.mp4
    store.set_crop("media/b.mp4", false);
    sink.release_one();
    sink.wait_calls(2);              // job 1 is done and posted; job 2 is held

    // Job 1's result (a only) is adopted; b is still on its way and must
    // be laid back on top of it.
    (void)store.poll();
    CHECK_FALSE(store.crop_for("media/a.mp4"));
    CHECK_FALSE(store.crop_for("media/b.mp4"));

    sink.release();
    REQUIRE(flushed(store));
    (void)store.poll();
    CHECK_FALSE(store.crop_for("media/b.mp4"));
}

TEST_CASE("media_info store: poll reports a change only when there is one",
          "[media_info]") {
    Sandbox sb;
    MediaInfoStore store(sb.file.string());
    CHECK_FALSE(store.poll());  // nothing read yet

    store.request_reload();     // no file: an empty map, same as before
    REQUIRE(flushed(store));
    CHECK_FALSE(store.poll());

    write_file(sb.file, R"({"version": 1, "videos": {"media/a.mp4": {"crop": false}}})");
    store.request_reload();
    REQUIRE(flushed(store));
    CHECK(store.poll());
    CHECK_FALSE(store.crop_for("media/a.mp4"));
    CHECK_FALSE(store.poll());  // adopted once

    // A corrupt file reads as "every video cropped".
    write_file(sb.file, "{oops");
    store.request_reload();
    REQUIRE(flushed(store));
    CHECK(store.poll());
    CHECK(store.crop_for("media/a.mp4"));
}

TEST_CASE("media_info store: a kiosk write keeps fields it does not know",
          "[media_info]") {
    Sandbox sb;
    write_file(sb.file, R"({"version": 1, "added_later": {"x": 1}, "videos": {
        "media/a.mp4": {"duration_s": 30.0, "thumbnail": "thumbs/a.jpg"}
    }})");
    MediaInfoStore store(sb.file.string());
    store.set_crop("media/a.mp4", false);
    REQUIRE(flushed(store));

    const Json::Value root = read_json(sb.file);
    CHECK(root["added_later"]["x"].asInt() == 1);
    CHECK(root["videos"]["media/a.mp4"]["thumbnail"].asString() == "thumbs/a.jpg");
    CHECK(root["videos"]["media/a.mp4"]["crop"].asBool() == false);
    CHECK(root["videos"]["media/a.mp4"]["duration_s"].asDouble() == Catch::Approx(30.0));
}

TEST_CASE("media_info store: writes wait for the sidecar lock", "[media_info]") {
    Sandbox sb;
    const std::string lock_path = sb.file.string() + ".lock";
    // The web admin holding the lock (its own open file description).
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::flock(fd, LOCK_EX) == 0);

    MediaInfoStore store(sb.file.string());
    store.set_crop("media/a.mp4", false);
    CHECK_FALSE(store.crop_for("media/a.mp4"));  // answered without the lock
    CHECK_FALSE(store.flush_until(std::chrono::steady_clock::now() + 200ms));
    CHECK_FALSE(fs::exists(sb.file));

    ::flock(fd, LOCK_UN);
    ::close(fd);
    REQUIRE(flushed(store));
    const auto disk = app::parse_media_info(read_file(sb.file));
    REQUIRE(disk.has_value());
    CHECK_FALSE(app::crop_for(*disk, "media/a.mp4"));
    // The lock lives beside the data file, never on it (the data file is
    // replaced by rename on every write).
    CHECK(fs::exists(lock_path));
}

TEST_CASE("media_info store: the file is read under the lock, not before it",
          "[media_info]") {
    Sandbox sb;
    write_file(sb.file, R"({"version": 1, "videos": {"media/b.mp4": {"duration_s": 10.0}}})");
    const std::string lock_path = sb.file.string() + ".lock";
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::flock(fd, LOCK_EX) == 0);

    MediaInfoStore store(sb.file.string());
    store.set_crop("media/a.mp4", false);
    // Long enough for the worker to be waiting on the lock (and, if it read
    // first, to have read the file as it was).
    CHECK_FALSE(store.flush_until(std::chrono::steady_clock::now() + 200ms));

    // The web admin, holding the lock, saves a new video's probe.
    write_file(sb.file, R"({"version": 1, "videos": {
        "media/b.mp4": {"duration_s": 10.0},
        "media/c.mp4": {"duration_s": 42.0, "display_aspect": 1.7778}
    }})");
    ::flock(fd, LOCK_UN);
    ::close(fd);
    REQUIRE(flushed(store));

    // The kiosk's write was merged into the web's file, not the one it would
    // have seen before taking the lock.
    const auto disk = app::parse_media_info(read_file(sb.file));
    REQUIRE(disk.has_value());
    CHECK_FALSE(app::crop_for(*disk, "media/a.mp4"));
    REQUIRE(disk->count("media/c.mp4") == 1);
    CHECK(*disk->at("media/c.mp4").duration_s == Catch::Approx(42.0));
    CHECK(disk->count("media/b.mp4") == 1);
}

TEST_CASE("media_info store: a reload poke's marker is deleted by the worker",
          "[media_info]") {
    Sandbox sb;
    write_file(sb.file, R"({"version": 1, "videos": {"media/a.mp4": {"crop": false}}})");
    const fs::path marker = sb.dir / "media_info_reload_request";
    write_file(marker, "");
    MediaInfoStore store(sb.file.string());

    // A plain reload (boot) leaves a marker alone.
    store.request_reload();
    settle(store);
    CHECK(fs::exists(marker));
    CHECK_FALSE(store.crop_for("media/a.mp4"));

    // The poke: the render loop only saw the marker; the job deletes it
    // (before reading, so a poke that lands mid-read is never lost).
    write_file(sb.file, R"({"version": 1, "videos": {}})");
    store.request_reload(marker.string());
    settle(store);
    CHECK_FALSE(fs::exists(marker));
    CHECK(store.crop_for("media/a.mp4"));

    // Already gone (a second poke for the same marker): still just a reload.
    store.request_reload(marker.string());
    REQUIRE(flushed(store));
}

TEST_CASE("media_info store: poll retries stranded work, at most every 30 s",
          "[media_info]") {
    Sandbox sb;
    auto fake_now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    const auto clock = [&fake_now] { return fake_now; };
    HeldSink sink;
    sink.set_fail(true);  // the card refuses writes
    MediaInfoStore store(sb.file.string(), sink.sink(), clock);
    REQUIRE(MediaInfoStore::kRetryInterval == std::chrono::seconds(30));

    // poll() once, then let any job it queued finish.
    auto poll_at = [&](std::chrono::seconds advance) {
        fake_now += advance;
        (void)store.poll();
        store.wait_idle_for_tests();
    };

    SECTION("a failed write") {
        store.set_crop("media/a.mp4", false);
        store.wait_idle_for_tests();
        REQUIRE(sink.call_count() == 1);

        poll_at(0s);    // stranded edit noticed: the clock starts
        CHECK(sink.call_count() == 1);
        poll_at(29s);
        CHECK(sink.call_count() == 1);
        poll_at(1s);    // 30 s after it was noticed
        CHECK(sink.call_count() == 2);
        poll_at(1s);    // failed again: not every frame
        poll_at(28s);
        CHECK(sink.call_count() == 2);
        poll_at(1s);
        CHECK(sink.call_count() == 3);

        // The card recovers: the next retry lands it, then nothing more runs.
        sink.set_fail(false);
        poll_at(30s);
        CHECK(sink.call_count() == 4);
        const auto disk = app::parse_media_info(read_file(sb.file));
        REQUIRE(disk.has_value());
        CHECK_FALSE(app::crop_for(*disk, "media/a.mp4"));
        poll_at(60s);
        poll_at(60s);
        CHECK(sink.call_count() == 4);
        CHECK_FALSE(store.crop_for("media/a.mp4"));
    }

    SECTION("a failed read") {
        // Unreadable: a directory where the file should be.
        fs::create_directories(sb.file);
        store.request_reload();
        store.wait_idle_for_tests();

        // The file becomes readable, but nothing retries before 30 s.
        fs::remove(sb.file);
        write_file(sb.file, R"({"version": 1, "videos": {"media/a.mp4": {"crop": false}}})");
        poll_at(0s);
        poll_at(29s);
        (void)store.poll();
        CHECK(store.crop_for("media/a.mp4"));

        poll_at(1s);    // the retry reads it...
        (void)store.poll();  // ...and the next frame adopts it
        CHECK_FALSE(store.crop_for("media/a.mp4"));
        CHECK(sink.call_count() == 0);  // reads never write
    }
}
