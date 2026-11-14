#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "movie_library.h"
#include "utils/latest_wins_worker.h"

namespace app {

// All public methods belong to one owner (the render thread). Only the
// worker reads/writes the snapshot, fetches Radarr, or checks drive/files.
// start/request_drive_check/poll never wait for that I/O. The published
// movies are an owner-thread copy; the full list stays on the worker so a
// temporarily absent drive/file can return without waiting for Radarr.
class MovieLibraryStore {
public:
    using Fetch = std::function<std::optional<std::vector<LibraryMovie>>()>;
    using Exists = std::function<bool(const std::string& path)>;
    using FileSink = std::function<bool(const std::string& path, const std::string& body)>;
    // Used on both owner and worker: injected clocks must support both.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using DriveReady = std::function<bool()>;
    static constexpr std::chrono::minutes kRefreshInterval{15};

    // DriveReady runs before Exists, including when the full list is empty.
    // The kiosk supplies its mount predicate; omitted means no mount gate
    // (useful for non-removable libraries and portable tests).
    MovieLibraryStore(std::string snapshot_path, Fetch fetch, Exists exists,
                      FileSink sink = nullptr, Clock clock = nullptr,
                      DriveReady drive_ready = nullptr);
    ~MovieLibraryStore() = default;
    MovieLibraryStore(const MovieLibraryStore&) = delete;
    MovieLibraryStore& operator=(const MovieLibraryStore&) = delete;

    void start(); // one boot snapshot read, any outcome lifts the boot gate
    void request_drive_check();
    bool poll(bool tunnel_healthy); // true only when movies() changes
    const std::vector<LibraryMovie>& movies() const { return movies_; }
    bool first_load_finished() const { return first_load_finished_; }

    // Shutdown before main's _exit: bounded even when a fetch/disk is stuck.
    // False on timeout or if the latest snapshot write failed; a failed
    // write is retried with fresh data on the next scheduled fetch.
    bool flush_until(std::chrono::steady_clock::time_point deadline);
    void wait_idle_for_tests() { worker_.flush(); }

private:
    void submit();
    bool run_job();
    void publish_existing(); // worker only; exceptions keep the last result

    std::string path_;
    Fetch fetch_;
    Exists exists_;
    FileSink sink_;
    Clock clock_;
    DriveReady drive_ready_;
    const std::thread::id owner_thread_;

    // Owner only.
    std::vector<LibraryMovie> movies_;
    bool started_ = false;
    bool first_load_finished_ = false;
    bool fetch_outstanding_ = false;
    std::optional<std::chrono::steady_clock::time_point> last_attempt_finished_;

    // Worker only; jobs run serially.
    std::vector<LibraryMovie> full_list_;

    // Shared; never held across file/drive/network I/O. Every job drains
    // every request flag, so replacing a pending job cannot lose a fetch
    // or drive check captured only by an obsolete job.
    std::mutex mu_;
    bool read_requested_ = false;
    bool fetch_requested_ = false;
    bool drive_check_requested_ = false;
    bool read_finished_ = false;
    bool write_failed_ = false;
    std::optional<std::vector<LibraryMovie>> result_;
    std::optional<std::chrono::steady_clock::time_point> attempt_finished_;

    // LAST: joins/drains before any data used by the jobs is destroyed.
    utils::LatestWinsWorker worker_;
};

} // namespace app
