#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace app {

// Serializes the "quiet the background media stack for gameplay" side
// effects on one worker thread. request_pause()/request_resume() record
// the latest desired state and return immediately — the game-launch path
// must never block on docker/qBit round-trips (2-8s). The worker applies
// state changes strictly in order, so a game that exits while the pause
// is still applying always gets a matching resume, and a pause+resume
// requested before the worker wakes coalesce into doing nothing.
class GameQuietMode {
public:
    struct Actions {
        std::function<void()> pause;
        std::function<void()> resume;
    };

    explicit GameQuietMode(Actions actions);
    // Applies any still-pending request, then joins the worker.
    ~GameQuietMode();

    GameQuietMode(const GameQuietMode&) = delete;
    GameQuietMode& operator=(const GameQuietMode&) = delete;

    void request_pause();
    void request_resume();

    // Test seam: blocks until the worker has applied the latest request.
    void wait_until_idle();

private:
    void run();
    void set_desired(bool paused);

    Actions actions_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool desired_paused_ = false;   // guarded by mutex_
    bool applied_paused_ = false;   // guarded by mutex_
    bool stop_ = false;             // guarded by mutex_
    std::thread worker_;
};

}  // namespace app
