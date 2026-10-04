#pragma once

#include <chrono>
#include <functional>

namespace media_browser {

// "Services ready" gate — BLOCKS, so call it on a WORKER thread only, never
// the render thread (WatchdogSec=10; the default deadline is 90 s).
//
// Why it exists: a FullPause playback session (every Pi 4B, and every 2 GB
// Pi 5 — CLAUDE.md "Playback contention guard") stops mdb_radarr /
// mdb_sonarr / mdb_prowlarr / mdb_byparr for the whole movie. leave() only
// QUEUES the resume on the MovieQuietMode worker, and the containers then
// take ~20-40 s after `docker start` to answer again. Anything the user
// asked for at the end of — or during — playback that talks to those
// services ("Start Season N" on the season-end card, a similar-film
// quick-add) used to fire immediately into a stopped container and fail
// with a misleading "couldn't" toast. Such an action instead runs this
// gate first, then its real requests.
//
// Sequence: (1) wait for the MovieQuietMode worker to go idle, in bounded
// slices, so a ping can never land in the window where a PAUSE is still
// executing and see the service up a moment before docker stop takes it
// down; (2) poll the service's cheap status endpoint until it answers.
// Both phases share ONE wall-clock deadline (a refused request that burns
// its own 5 s curl timeout counts against it), and `cancelled` is checked
// at least every `slice` so a screen destructor never sits out a deadline.
struct ServiceGateHooks {
    // Optional. Bounded wait for the movie quiet-mode executor to be idle
    // (app::MovieQuietMode::wait_until_idle_for); true = idle. Unset = no
    // executor reachable from the caller — the ping phase alone is still
    // correct after a COMPLETED pause, because a stopped container cannot
    // answer.
    std::function<bool(std::chrono::milliseconds)> wait_quiet_idle;
    // Required. One cheap request; true = the service answered.
    std::function<bool()> ping;
    // Optional. True = give up now (screen shutting down / intent dropped).
    std::function<bool()> cancelled;
    // Test seams; default to steady_clock::now / this_thread::sleep_for.
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
};

struct ServiceGateTiming {
    // Container restart is ~20-40 s measured; 90 s leaves room for a slow
    // first answer on a loaded Pi 4B without leaving a user waiting on a
    // toast that never resolves.
    std::chrono::milliseconds deadline{90000};
    std::chrono::milliseconds poll_interval{2000};
    // Cancellation granularity for every wait the gate itself performs.
    std::chrono::milliseconds slice{250};
};

enum class GateResult { Ready, TimedOut, Cancelled };

GateResult wait_for_service(const ServiceGateHooks& hooks,
                            ServiceGateTiming timing = {});

}  // namespace media_browser
