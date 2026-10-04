// utils::subprocess::run against REAL processes (true/false/echo/sleep/sh).
// The helper replaced seven hand-rolled fork/exec/wait copies, one of which
// read pactl's stdout with no deadline on the render thread; these cases
// pin the properties every caller now relies on: a hard deadline that
// always reaps, exact exit codes, complete output however large, and no
// kiosk fd leaking into the child.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "utils/subprocess.h"

namespace sp = utils::subprocess;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

namespace {
long elapsed_ms(Clock::time_point t0) {
    return static_cast<long>(std::chrono::duration_cast<milliseconds>(
        Clock::now() - t0).count());
}
}  // namespace

TEST_CASE("subprocess: exit codes", "[subprocess]") {
    CHECK(sp::run({"true"}, milliseconds(5000)).exit_code == 0);
    CHECK(sp::run({"true"}, milliseconds(5000)).ok());
    auto f = sp::run({"false"}, milliseconds(5000));
    CHECK(f.exit_code == 1);
    CHECK_FALSE(f.ok());
    CHECK_FALSE(f.timed_out);
    CHECK(sp::run({"sh", "-c", "exit 42"}, milliseconds(5000)).exit_code == 42);
}

TEST_CASE("subprocess: a signal death reports 128+signal", "[subprocess]") {
    auto r = sp::run({"sh", "-c", "kill -9 $$"}, milliseconds(5000));
    CHECK(r.exit_code == 128 + 9);
    CHECK_FALSE(r.timed_out);
}

TEST_CASE("subprocess: missing program is 127, empty argv is -1", "[subprocess]") {
    CHECK(sp::run({"/nonexistent/mdb-no-such-program"}, milliseconds(5000)).exit_code == 127);
    CHECK(sp::run({"mdb-no-such-program-on-path"}, milliseconds(5000)).exit_code == 127);
    CHECK(sp::run({}, milliseconds(5000)).exit_code == -1);
}

TEST_CASE("subprocess: stdout capture is opt-in", "[subprocess]") {
    auto captured = sp::run({"echo", "hello", "world"}, milliseconds(5000), true);
    CHECK(captured.ok());
    CHECK(captured.out == "hello world\n");

    auto discarded = sp::run({"echo", "hello"}, milliseconds(5000), false);
    CHECK(discarded.ok());
    CHECK(discarded.out.empty());
}

TEST_CASE("subprocess: argv is never shell-interpreted", "[subprocess]") {
    // A sink or SSID name containing shell syntax must arrive verbatim.
    auto r = sp::run({"echo", "$(touch /tmp/mdb-pwned); `id` | ; &&"},
                     milliseconds(5000), true);
    CHECK(r.out == "$(touch /tmp/mdb-pwned); `id` | ; &&\n");
}

TEST_CASE("subprocess: stderr is discarded or merged on request", "[subprocess]") {
    const std::vector<std::string> both = {"sh", "-c", "echo out; echo err 1>&2"};
    sp::Options o;
    o.timeout = milliseconds(5000);
    o.capture_stdout = true;

    o.stderr_mode = sp::Stderr::Discard;
    CHECK(sp::run(both, o).out == "out\n");

    o.stderr_mode = sp::Stderr::MergeIntoStdout;
    auto merged = sp::run(both, o).out;
    CHECK(merged.find("out\n") != std::string::npos);
    CHECK(merged.find("err\n") != std::string::npos);
}

TEST_CASE("subprocess: large output arrives complete", "[subprocess]") {
    // Far beyond a pipe buffer (64 KiB): a reader that stopped early would
    // deadlock the child on a full pipe.
    auto r = sp::run({"head", "-c", "1000000", "/dev/zero"}, milliseconds(10000), true);
    CHECK(r.ok());
    CHECK(r.out.size() == 1000000u);
}

TEST_CASE("subprocess: output past max_output is drained, not kept", "[subprocess]") {
    sp::Options o;
    o.timeout = milliseconds(10000);
    o.capture_stdout = true;
    o.max_output = 1000;
    auto r = sp::run({"head", "-c", "300000", "/dev/zero"}, o);
    CHECK(r.ok());              // the child was never blocked on the pipe
    CHECK(r.out.size() == 1000u);
}

TEST_CASE("subprocess: timeout terminates and reaps the child", "[subprocess]") {
    const auto t0 = Clock::now();
    auto r = sp::run({"sleep", "10"}, milliseconds(200));
    CHECK(r.timed_out);
    CHECK_FALSE(r.ok());
    CHECK(r.exit_code == 128 + 15);   // SIGTERM was enough
    CHECK(elapsed_ms(t0) < 3000);
}

TEST_CASE("subprocess: timeout while capturing returns what was read", "[subprocess]") {
    const auto t0 = Clock::now();
    auto r = sp::run({"sh", "-c", "echo partial; exec sleep 10"}, milliseconds(300), true);
    CHECK(r.timed_out);
    CHECK(r.out == "partial\n");
    CHECK(elapsed_ms(t0) < 3000);
}

TEST_CASE("subprocess: a child that ignores SIGTERM is SIGKILLed after the grace",
          "[subprocess]") {
    sp::Options o;
    o.timeout = milliseconds(200);
    o.kill_grace = milliseconds(200);
    const auto t0 = Clock::now();
    // `exec sleep` keeps the ignored disposition (exec preserves SIG_IGN).
    auto r = sp::run({"sh", "-c", "trap '' TERM; exec sleep 10"}, o);
    CHECK(r.timed_out);
    CHECK(r.exit_code == 128 + 9);
    CHECK(elapsed_ms(t0) < 3000);
}

TEST_CASE("subprocess: the deadline covers the whole process group", "[subprocess]") {
    // The shell waits on a backgrounded grandchild that also holds the
    // pipe. Killing only the direct child would leave the grandchild
    // running; group signalling ends both.
    const auto t0 = Clock::now();
    auto r = sp::run({"sh", "-c", "sleep 10 & wait"}, milliseconds(200), true);
    CHECK(r.timed_out);
    CHECK(elapsed_ms(t0) < 3000);
}

TEST_CASE("subprocess: a grandchild holding stdout does not stall a finished child",
          "[subprocess]") {
    // The child exits at once but leaves a background process with the
    // pipe's write end. Waiting for EOF would burn the whole deadline.
    const auto t0 = Clock::now();
    auto r = sp::run({"sh", "-c", "sleep 3 & echo done"}, milliseconds(5000), true);
    CHECK_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(r.out == "done\n");
    CHECK(elapsed_ms(t0) < 2500);
}

TEST_CASE("subprocess: stdin is /dev/null", "[subprocess]") {
    // `cat` would block forever on an inherited terminal or pipe.
    const auto t0 = Clock::now();
    auto r = sp::run({"cat"}, milliseconds(3000), true);
    CHECK(r.ok());
    CHECK(r.out.empty());
    CHECK(elapsed_ms(t0) < 2000);
}

TEST_CASE("subprocess: parent fds do not leak into the child", "[subprocess]") {
    // A plain open() — no O_CLOEXEC — the way a DRM or evdev fd is opened.
    const int fd = ::open("/dev/null", O_RDONLY);
    REQUIRE(fd >= 0);
    const int high = ::dup2(fd, 200);
    ::close(fd);
    REQUIRE(high == 200);
    auto r = sp::run({"sh", "-c", "if [ -e /dev/fd/200 ]; then echo leaked; else echo clean; fi"},
                     milliseconds(5000), true);
    ::close(high);
    CHECK(r.out == "clean\n");
}
