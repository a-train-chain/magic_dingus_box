#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include "app_state.h"
#include "../utils/latest_wins_worker.h"

namespace app {

// Publishes kiosk_status.json for the phone remote, verify_box.sh and the
// smoke-test harness.
//
// The FILE I/O runs on a dedicated writer thread, never on the caller's.
// Origin (2026-10-26, 1 GB Pi 4B, 11 GB playlist-package import): with the
// SD card saturated by the web admin's upload/extract writes (~7 MB/s of
// card bandwidth, 50-75 MB of dirty+writeback queued ahead), the tmp
// file's openat(O_CREAT) waited on the ext4 journal and the rename waited
// in ext4_evict_inode for the REPLACED file's page to finish writeback
// behind that queue — sampled at up to ~6 s per call on the render thread,
// which pings the systemd watchdog (WatchdogSec=10). The kiosk was
// SIGABRTed five times in fifteen minutes. A status file is advisory; the
// picture and the watchdog ping must never wait for one.
class StatusWriter {
public:
    // Writes `body` to `path` atomically (via `tmp_path` + rename).
    // Returns false on failure. Runs on the writer thread only.
    using FileSink = std::function<bool(const std::string& tmp_path,
                                        const std::string& path,
                                        const std::string& body)>;

    explicit StatusWriter(std::string path);
    // Test seam: replaces the real tmp+rename with `sink`.
    StatusWriter(std::string path, FileSink sink);
    // Stops the writer thread after it finishes the write in hand and the
    // latest pending one (so a shutdown still publishes its final state).
    ~StatusWriter();

    StatusWriter(const StatusWriter&) = delete;
    StatusWriter& operator=(const StatusWriter&) = delete;

    /// Snapshots the current state and queues it for the writer thread.
    /// Never touches the filesystem and never waits for a write: a newer
    /// snapshot simply replaces one the writer has not picked up yet
    /// (latest wins — readers only ever want the newest state).
    /// THREAD-SAFETY: must be called from the same thread that constructed
    /// the StatusWriter (typically the main loop). Reads several
    /// non-atomic AppState fields without a lock; calling from a second
    /// thread is a data race.
    void write_now(const AppState& state);

    /// Blocks until every snapshot queued so far is on disk (or failed).
    /// For tests and shutdown paths — NEVER call it from the render loop.
    void flush();

private:
    std::string path_;
    std::string tmp_path_;
    FileSink sink_;
    std::thread::id owning_thread_;
    // Change-detection + heartbeat (see write_now): the serialized body
    // (EXCLUDING the ts stamp) of the last snapshot handed to the writer,
    // and when. Both touched only on the owning thread.
    std::string last_body_;
    std::chrono::steady_clock::time_point last_write_{};

    // The writer thread (latest snapshot wins; utils/latest_wins_worker.h).
    // Declared LAST: it is destroyed — drained and joined — first, while
    // sink_ and the paths its jobs read are still alive.
    utils::LatestWinsWorker worker_;
};

} // namespace app
