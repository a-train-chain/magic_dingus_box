#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <string>
#include <utility>
#include "app/app_state.h"
#include "app/controller.h"
#include "ui/virtual_keyboard.h"

namespace fs = std::filesystem;

namespace {
// Helper: write JSON-Lines events into the queue file.
void write_queue(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::trunc);
    f << content;
}

// What three consecutive frames do. The lock/read/truncate runs on a worker
// thread (it can block on the SD card and on the web side's lock, and the
// render loop pings the systemd watchdog), so: frame 1 notices the file and
// asks for a drain, the worker drains, frame 2 dispatches what it read.
void drain_and_dispatch(app::Controller& c, app::AppState& state) {
    c.poll_text_input_queue(state);
    c.wait_for_text_input_drain();
    c.poll_text_input_queue(state);
}
}

TEST_CASE("poll_text_input_queue idle fast path", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_idle.jsonl";
    fs::remove(tmp);

    ui::VirtualKeyboard kb;
    kb.open("hello", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    // No file → no-op, no exception, buffer unchanged.
    drain_and_dispatch(c, state);
    REQUIRE(kb.get_text() == "hello");
}

TEST_CASE("poll_text_input_queue type_char appends", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_typechar.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"s\",\"seq\":1}\n"
        "{\"t\":\"type_char\",\"c\":\"h\",\"seq\":2}\n"
        "{\"t\":\"type_char\",\"c\":\"a\",\"seq\":3}\n");

    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    drain_and_dispatch(c, state);
    REQUIRE(kb.get_text() == "sha");
    // File should be truncated after drain.
    REQUIRE(fs::file_size(tmp) == 0);
}

TEST_CASE("poll_text_input_queue dispatches backspace and enter", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_special.jsonl";
    write_queue(tmp,
        "{\"t\":\"key_special\",\"k\":\"backspace\",\"seq\":1}\n"
        "{\"t\":\"key_special\",\"k\":\"enter\",\"seq\":2}\n");

    std::string captured;
    ui::VirtualKeyboard kb;
    kb.open("hello", "T",
            [&](const std::string& s){ captured = s; },
            nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    drain_and_dispatch(c, state);
    REQUIRE(kb.get_text() == "hell");          // backspace ate the 'o'
    REQUIRE(captured == "hell");                // commit fired on_enter
}

TEST_CASE("poll_text_input_queue clear wipes buffer", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_clear.jsonl";
    write_queue(tmp, "{\"t\":\"clear\",\"seq\":1}\n");

    ui::VirtualKeyboard kb;
    kb.open("password", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    drain_and_dispatch(c, state);
    REQUIRE(kb.get_text() == "");
}

TEST_CASE("poll_text_input_queue drops events when no receiver", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_dropped.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"x\",\"seq\":1}\n");

    app::AppState state;
    state.active_text_keyboard = nullptr;       // no receiver
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    REQUIRE_NOTHROW(drain_and_dispatch(c, state));
    REQUIRE(fs::file_size(tmp) == 0);            // still truncated
}

TEST_CASE("poll_text_input_queue skips malformed lines", "[remote][text_queue]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_malformed.jsonl";
    write_queue(tmp,
        "{\"t\":\"type_char\",\"c\":\"a\",\"seq\":1}\n"
        "this is not json\n"
        "{\"t\":\"type_char\",\"c\":\"b\",\"seq\":2}\n");

    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);

    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    drain_and_dispatch(c, state);
    REQUIRE(kb.get_text() == "ab");
    REQUIRE(fs::file_size(tmp) == 0);
}

// ── Off-thread file work ────────────────────────────────────────────────
// The drain used to run on the render loop: a BLOCKING flock (the loop
// waited for as long as the web admin's writer held the lock) and an
// ftruncate per drain (a journaled file write — the class of call that
// stalled kiosk_status.json for ~6 s and got the kiosk killed by the
// systemd watchdog). These pin the contract that replaced it.

namespace {

const char* const kTypeA = "{\"t\":\"type_char\",\"c\":\"a\",\"seq\":1}\n";
const char* const kTypeB = "{\"t\":\"type_char\",\"c\":\"b\",\"seq\":2}\n";
const char* const kTypeC = "{\"t\":\"type_char\",\"c\":\"c\",\"seq\":3}\n";

// A drain that can be parked (the SD card, or the web side's lock) and
// counts how often it was entered.
struct StalledDrain {
    std::mutex mu;
    std::condition_variable cv;
    bool released = false;
    int entered = 0;
    std::string next;   // what the next drain "reads"

    app::TextQueueDrainer::DrainFn fn() {
        return [this](const std::string&) {
            std::unique_lock<std::mutex> lk(mu);
            ++entered;
            cv.notify_all();
            cv.wait(lk, [this] { return released; });
            return std::exchange(next, std::string());
        };
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return entered > 0; });
    }
    void release() {
        std::lock_guard<std::mutex> lk(mu);
        released = true;
        cv.notify_all();
    }
    int times_entered() {
        std::lock_guard<std::mutex> lk(mu);
        return entered;
    }
};

}  // namespace

TEST_CASE("poll_text_input_queue never waits for a stalled drain",
          "[remote][text_queue][io]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_stalled.jsonl";
    write_queue(tmp, kTypeA);   // non-empty: every frame sees work to do

    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);
    app::AppState state;
    state.active_text_keyboard = &kb;

    StalledDrain stalled;
    stalled.next = kTypeA;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());
    c.set_text_input_drain_for_tests(stalled.fn());

    c.poll_text_input_queue(state);
    stalled.wait_entered();   // the worker is now stuck in the drain

    // Several seconds' worth of frames while it is stuck. The old code
    // blocked the render loop on each one.
    const auto t0 = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 300; ++frame) c.poll_text_input_queue(state);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    CHECK(kb.get_text() == "");            // nothing delivered yet
    CHECK(stalled.times_entered() == 1);   // and no pile of queued drains

    stalled.release();
    c.wait_for_text_input_drain();
    c.poll_text_input_queue(state);
    CHECK(kb.get_text() == "a");           // delivered once, not 300 times
    c.wait_for_text_input_drain();
    fs::remove(tmp);
}

TEST_CASE("poll_text_input_queue takes its turn behind another lock holder "
          "without blocking the caller",
          "[remote][text_queue][io]") {
    // The web admin's writer holds flock(LOCK_EX) across its write + flush.
    // Stand in for it with a second descriptor holding the lock.
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_locked.jsonl";
    write_queue(tmp, kTypeA);
    const int holder = ::open(tmp.c_str(), O_RDWR);
    REQUIRE(holder >= 0);
    REQUIRE(::flock(holder, LOCK_EX) == 0);

    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);
    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    const auto t0 = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 120; ++frame) {
        c.poll_text_input_queue(state);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // 120 frames at 1 ms each; the old blocking flock would still be
    // sitting in the first one.
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    CHECK(kb.get_text() == "");
    CHECK(fs::file_size(tmp) > 0);   // untouched while the other side holds it

    // The writer appends under its lock, then lets go: nothing it wrote may
    // be lost or delivered twice.
    {
        std::ofstream f(tmp, std::ios::app);
        f << kTypeB;
    }
    REQUIRE(::flock(holder, LOCK_UN) == 0);
    ::close(holder);

    c.wait_for_text_input_drain();
    drain_and_dispatch(c, state);
    CHECK(kb.get_text() == "ab");
    CHECK(fs::file_size(tmp) == 0);
    fs::remove(tmp);
}

TEST_CASE("poll_text_input_queue keeps keystroke order across drains",
          "[remote][text_queue][io]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_order.jsonl";
    ui::VirtualKeyboard kb;
    kb.open("", "T", nullptr, nullptr);
    app::AppState state;
    state.active_text_keyboard = &kb;
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());

    write_queue(tmp, kTypeA);
    c.poll_text_input_queue(state);          // frame 1: asks for a drain
    c.wait_for_text_input_drain();
    write_queue(tmp, std::string(kTypeB) + kTypeC);   // typed meanwhile
    c.poll_text_input_queue(state);          // frame 2: delivers "a", asks again
    CHECK(kb.get_text() == "a");
    c.wait_for_text_input_drain();
    c.poll_text_input_queue(state);          // frame 3: delivers "bc"
    CHECK(kb.get_text() == "abc");
    CHECK(fs::file_size(tmp) == 0);
    fs::remove(tmp);
}

TEST_CASE("poll_text_input_queue: an empty queue costs no drain at all",
          "[remote][text_queue][io]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_text_q_empty.jsonl";
    write_queue(tmp, "");
    app::AppState state;
    StalledDrain drain;
    drain.release();
    app::Controller c;
    c.set_text_input_queue_path(tmp.string());
    c.set_text_input_drain_for_tests(drain.fn());

    for (int frame = 0; frame < 100; ++frame) c.poll_text_input_queue(state);
    c.wait_for_text_input_drain();
    // No lock, no read, no truncate — the idle box never touches the file.
    CHECK(drain.times_entered() == 0);
    fs::remove(tmp);
}
