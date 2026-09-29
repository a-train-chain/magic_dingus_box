#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace app {

// Off-render-thread executor for the Media Browser movie playback
// contention guard (CLAUDE.md "Playback contention guard"). The sibling of
// GameQuietMode, with two differences that movies need:
//
//   * The pause carries a MODE (trickle vs full pause), decided per session
//     from MemAvailable on the render thread — so this is an ordered command
//     queue rather than a single desired-state bit.
//   * The pause action REPORTS what it actually changed (Consent), and the
//     worker hands exactly that record to the matching resume. The consent
//     records live on the worker, so leave() can never undo something enter()
//     did not do, and can never race the pause that sets them.
//
// PlaybackScreen::enter()/leave() used to run all of this synchronously on
// the render thread: several sequential 5 s qBit requests (plus a 403
// re-login) and a blocking `playback_services_pause.sh` (docker stop/start,
// compose-up fallback) — far past WatchdogSec=10 with a wedged service.
//
// Ordering: commands run strictly FIFO on one worker, so a resume can never
// overtake a pending pause. A resume requested while the pause it would undo
// has not STARTED yet cancels that pause instead (nothing to undo).
class MovieQuietMode {
public:
    enum class Mode { Trickle, FullPause };

    // What a pause actually changed — the consent record for its resume.
    struct Consent {
        bool alt_limited = false;      // qBit alternative speed limits engaged by us
        bool qbit_paused = false;      // qBit pause_all() succeeded
        bool services_paused = false;  // playback_services_pause.sh pause ran
        bool any() const { return alt_limited || qbit_paused || services_paused; }
    };

    struct Actions {
        std::function<Consent(Mode)> pause;
        // Called only with a Consent where any() is true.
        std::function<void(const Consent&)> resume;
    };

    explicit MovieQuietMode(Actions actions);
    // Drains every queued command (so a pending resume is applied), then
    // joins the worker.
    ~MovieQuietMode();

    MovieQuietMode(const MovieQuietMode&) = delete;
    MovieQuietMode& operator=(const MovieQuietMode&) = delete;

    // Render-thread safe: record the request and return immediately.
    void request_pause(Mode mode);
    void request_resume();

    // Blocks until every queued command has been applied.
    void wait_until_idle();
    // Bounded variant for the shutdown path / cross-worker ordering.
    // Returns true if idle was reached within `timeout`.
    bool wait_until_idle_for(std::chrono::milliseconds timeout);

private:
    struct Cmd {
        bool pause = false;
        Mode mode = Mode::FullPause;
    };

    void run();
    bool idle_locked() const { return queue_.empty() && !busy_; }

    Actions actions_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Cmd> queue_;   // guarded by mutex_
    bool busy_ = false;       // guarded by mutex_ — a command is executing
    bool stop_ = false;       // guarded by mutex_
    Consent held_;            // worker-thread only
    std::thread worker_;
};

}  // namespace app
