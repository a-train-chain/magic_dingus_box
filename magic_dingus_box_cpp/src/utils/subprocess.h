#pragma once

// The kiosk's ONE way to run a helper program (amixer, pactl, udevadm,
// nmcli, pkill) and wait for it. Unit-tested on the Mac against real
// processes (tests/utils/test_subprocess.cpp).
//
// WHY: there were seven hand-rolled fork/exec/wait copies, each with its
// own subset of the failure modes — no deadline at all (pactl's capture
// ran on the RENDER thread with a blocking read and a blocking waitpid, so
// a wedged PulseAudio froze the picture until the systemd watchdog shot
// the kiosk), waitpid not retried on EINTR, every kiosk fd (DRM master,
// evdev grabs) leaked into the child, SIGPIPE inherited as SIG_IGN. One
// implementation means one place to get those right.
//
// Guarantees of run():
//   - argv is exec'd directly (execvp, PATH lookup) — never through a
//     shell, so nothing in argv is ever interpreted;
//   - EVERY run has a deadline. On expiry the child's whole process group
//     gets SIGTERM, then SIGKILL after kill_grace. TERM first because the
//     usual long-runner is `sudo nmcli ...`: sudo relays SIGTERM to its
//     root child, but SIGKILL to sudo would orphan that child — and the
//     unprivileged kiosk cannot signal a root process itself;
//   - the child is always reaped (waitpid retried on EINTR), so no zombie
//     survives a timeout;
//   - the child gets stdin from /dev/null (a sudo that wants a password
//     fails fast instead of waiting forever), default signal dispositions,
//     an empty signal mask, and no inherited fds above 2;
//   - only async-signal-safe calls run between fork and exec (the kiosk is
//     multithreaded).

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace utils::subprocess {

enum class Stderr {
    Discard,           // -> /dev/null
    MergeIntoStdout,   // 2>&1 — callers that parse nmcli's "Error: ..." lines
};

struct Options {
    // Hard deadline for the whole run, output reading included. Values
    // <= 0 are clamped to 1 ms: an unbounded wait is not on offer.
    std::chrono::milliseconds timeout{5000};
    // false: the child's stdout goes to /dev/null and Result::out is empty.
    bool capture_stdout = false;
    Stderr stderr_mode = Stderr::Discard;
    // SIGTERM -> SIGKILL grace on timeout.
    std::chrono::milliseconds kill_grace{200};
    // Captured bytes beyond this are read (so the child never blocks on a
    // full pipe) but dropped.
    std::size_t max_output = 4u << 20;
};

struct Result {
    // WEXITSTATUS when the child exited; 128+signal when a signal killed
    // it; 127 when the program could not be exec'd (not installed);
    // -1 when it could not be started at all (empty argv, fork/pipe failure)
    // or its exit status was lost (lost_child).
    int exit_code = -1;
    std::string out;
    bool timed_out = false;
    // waitpid failed with ECHILD: the child was reaped by someone else
    // (SIGCHLD ignored, or a stray waitpid(-1) elsewhere in the process).
    // The command's outcome is UNKNOWN, so exit_code is -1 and ok() is
    // false — it used to read as status 0, i.e. success.
    bool lost_child = false;

    bool ok() const { return !timed_out && exit_code == 0; }
};

Result run(const std::vector<std::string>& argv, const Options& opts);

// The common shapes.
inline Result run(const std::vector<std::string>& argv,
                  std::chrono::milliseconds timeout,
                  bool capture_stdout = false) {
    Options o;
    o.timeout = timeout;
    o.capture_stdout = capture_stdout;
    return run(argv, o);
}

}  // namespace utils::subprocess
