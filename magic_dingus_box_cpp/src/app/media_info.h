#pragma once

// Per-video media info: <data>/media_info.json, shared by the kiosk and the
// web admin. Design: docs/superpowers/specs/2026-11-12-video-crop-toggle-
// design.md, "Where the setting is stored".
//
//   {"version": 1,
//    "videos": {"media/x.mp4": {"duration_s": 212.4,
//                               "display_aspect": 1.7778,
//                               "crop": false}}}
//
// Keys are paths relative to the data directory, with `/` separators. Every
// field is optional: the web admin writes duration_s and display_aspect when
// it saves a video, and `crop` exists only as `false` — absent means the
// default, cropped. A missing, empty or corrupt file means "every video is
// cropped"; it never blocks playback or the menu. Fields and top-level keys
// the kiosk does not know are kept when it writes (it merges into the JSON it
// read, it never rebuilds the file from its own struct).
//
// The schema of record is tests/app/fixtures/media_info.json, read by this
// side's test (tests/app/test_media_info.cpp) and the web admin's.
//
// Cross-process protocol, the same in both processes: every write is
//   flock(LOCK_EX) on the SIDECAR <data>/media_info.json.lock -> read ->
//   merge -> write tmp + fsync -> rename -> fsync dir -> unlock.
// The lock is never on the data file itself: rename replaces that inode, so
// a lock on it would protect nothing. Reads take no lock (a rename is
// atomic: a reader sees the old file or the new one, never a torn one).

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "../utils/latest_wins_worker.h"

namespace app {

struct MediaInfo {
    std::optional<double> duration_s;
    std::optional<double> display_aspect;
    std::optional<bool> crop;
};

inline bool operator==(const MediaInfo& a, const MediaInfo& b) {
    return a.duration_s == b.duration_s && a.display_aspect == b.display_aspect &&
           a.crop == b.crop;
}
inline bool operator!=(const MediaInfo& a, const MediaInfo& b) { return !(a == b); }

using MediaInfoMap = std::map<std::string, MediaInfo>;

// nullopt = corrupt (not JSON, or the root / "videos" is not an object).
// Empty or whitespace-only text = an empty map (a file cut short before its
// first write). Within a valid file a field of the wrong type, or a length /
// aspect that is not a positive finite number, reads as absent, and an
// entry that is not an object is skipped — the rest still loads.
std::optional<MediaInfoMap> parse_media_info(const std::string& json);

// Version 1, two-space indented, trailing newline.
std::string serialize_media_info(const MediaInfoMap& map);

// `resolved_path` relative to `data_dir`, with `/` separators; "" when the
// path is not inside `data_dir` (or is the directory itself). Lexical only —
// no filesystem access; both paths are normalized first ("a/../b", a
// trailing slash). Absolute paths on both sides: a relative path against an
// absolute directory is "" rather than a guess.
std::string media_key_for(const std::string& resolved_path,
                          const std::string& data_dir);

// The video's crop setting: true (cropped) unless its entry says false.
bool crop_for(const MediaInfoMap& map, const std::string& key);

// The kiosk's copy of media_info.json.
//
// THREADING: every public method belongs to ONE thread (the render loop),
// and none of request_reload / poll / map / crop_for / set_crop touches the
// filesystem, takes the file lock or waits for the writer: the loop pings
// the systemd watchdog (WatchdogSec=10), and with the SD card saturated by a
// Content Manager upload a single create/rename/fsync has been sampled at
// ~6 s (see status_writer.h). The reads and writes run on a worker thread
// (utils::LatestWinsWorker); the loop only swaps in what it produced.
//
// Edits are never lost: set_crop records the edit in a pending map that
// EVERY worker job drains when it runs (the worker replaces a job it has not
// started with a newer one, so a job must never carry an edit only in its
// own captures). Until an edit is known to be on disk, it is laid back on
// top of whatever the worker read, so a reload — or a write's own result —
// can never undo it on screen. A write (or read) that fails keeps its work
// waiting: it goes out with the next set_crop / request_reload, poll()
// retries it every kRetryInterval, and flush_until retries once.
class MediaInfoStore {
public:
    // Writes `body` to `path` durably (tmp + fsync + rename + fsync dir).
    // Returns false on failure. Runs on the worker thread only, with the
    // sidecar lock held.
    using FileSink = std::function<bool(const std::string& path,
                                        const std::string& body)>;
    // poll()'s clock (render thread).
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // How often poll() retries work a failed job left waiting (a full or
    // read-only card): often enough that a toggle reaches the disk soon
    // after the card recovers, rarely enough not to hammer one that hasn't.
    static constexpr std::chrono::seconds kRetryInterval{30};

    explicit MediaInfoStore(std::string path);
    // Test seam: replaces the real durable write with `sink` (which may
    // stall or fail, then call write_file_durably itself) and, when given,
    // poll()'s clock (default std::chrono::steady_clock::now).
    MediaInfoStore(std::string path, FileSink sink, Clock clock = nullptr);
    // Finishes the job in hand and the latest queued one, then joins.
    // Unbounded — main()'s _exit path calls flush_until first instead.
    ~MediaInfoStore() = default;

    MediaInfoStore(const MediaInfoStore&) = delete;
    MediaInfoStore& operator=(const MediaInfoStore&) = delete;

    // The real FileSink.
    static bool write_file_durably(const std::string& path, const std::string& body);

    // Queues a read of the file; poll() adopts the result. At boot with no
    // argument. For the web admin's media_info_reload_request poke, pass
    // the marker's path: the worker deletes it just BEFORE it reads (an
    // unlink is a journaled write, and the poke comes after uploads, when
    // the card is busiest — never on the render loop). Deleting first means
    // a poke that lands during the read is kept for the next one; seeing
    // the marker again before the worker got to it costs only a second,
    // harmless reload.
    //
    // True when this call queued something new: no reload was waiting, or
    // this one brings a marker the waiting one would not delete. False when
    // the same request is already waiting for the worker — main.cpp logs
    // the poke only on true, so a worker stuck on the card (marker still in
    // place, seen again every second) does not log every second.
    bool request_reload(const std::string& consume_marker = "");

    // Once per frame: adopts the newest map the worker produced, with every
    // edit not yet known to be on disk applied on top. True when the map
    // changed. Also resubmits work a failed job left waiting, at most once
    // per kRetryInterval (none while a job is queued or running). Never
    // blocks (the worker holds the shared mutex only to swap state, never
    // across file I/O).
    bool poll();

    const MediaInfoMap& map() const { return map_; }
    bool crop_for(const std::string& key) const { return app::crop_for(map_, key); }

    // True once the first read ATTEMPT has finished, in any outcome — the
    // file read (poll() adopting its map in the same call), no file, a
    // corrupt file, an I/O error such as EACCES or EIO — and poll() has
    // published it. False while that read is still under way (a slow or
    // stalled card). The Channel Guide's power-on tune waits for it
    // (app/auto_tune.h): until then only trimmed items have a length. A
    // failed read lifts it too, so a box that can never read the file
    // behaves as before the gate rather than never tuning on its own.
    bool first_read_finished() const { return first_read_finished_; }

    // The operator's choice for one video. The map changes now; the write
    // is queued. crop == true removes the entry's crop field (and the entry
    // when nothing else is left in it). An empty key is ignored.
    void set_crop(const std::string& key, bool crop);

    // Blocks until every edit made so far is on disk (true), or until
    // `deadline` (false — also when a write failed twice: one retry). For
    // shutdown and tests — NEVER the render loop.
    bool flush_until(std::chrono::steady_clock::time_point deadline);

    // Blocks until no job is queued or running, without flush_until's retry.
    // Tests only.
    void wait_idle_for_tests() { worker_.flush(); }

private:
    // key -> crop. The latest edit per key wins.
    using Edits = std::map<std::string, bool>;

    void submit();
    // The worker's job: drain pending_ and the reload request, run
    // read_merge_write, and post what is on disk (or put the edits back).
    bool run_job();
    // Worker thread. With edits: lock -> read -> merge -> write -> unlock;
    // without (or when the lock cannot be taken: the edits then wait): read.
    // Sets `on_disk` to the file's contents after the job
    // (left empty when the file could not be read). False when the edits
    // did not reach the disk.
    bool read_merge_write(const Edits& batch, std::optional<MediaInfoMap>& on_disk);
    bool edits_waiting();
    // poll()'s half: resubmit what a failed job left waiting, rate-limited.
    void retry_stranded_work();

    std::string path_;
    std::string lock_path_;
    FileSink sink_;
    Clock clock_;

    // Render thread only: its map, and the stranded-work retry schedule
    // (armed when poll() first sees work waiting with the worker idle).
    MediaInfoMap map_;
    bool first_read_finished_ = false;
    bool retry_armed_ = false;
    std::chrono::steady_clock::time_point next_retry_{};

    // Between the render thread and the worker. mu_ is held only to swap
    // these, never across file I/O.
    std::mutex mu_;
    Edits pending_;        // edits no job has taken yet
    Edits inflight_;       // edits the running job is writing
    bool reload_requested_ = false;
    bool read_finished_ = false;          // a job's read has run (any outcome)
    std::string reload_marker_;           // deleted by the next reload job
    std::optional<MediaInfoMap> result_;  // the newest map read, not yet adopted

    // LAST member: destroyed (drained and joined) first, while everything
    // its jobs use is still alive.
    utils::LatestWinsWorker worker_;
};

}  // namespace app
