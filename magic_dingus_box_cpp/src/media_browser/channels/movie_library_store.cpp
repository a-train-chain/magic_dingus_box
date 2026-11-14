#include "movie_library_store.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

#include "app/media_info.h"

namespace app {
namespace {
bool same_movie(const LibraryMovie& a, const LibraryMovie& b) {
    return a.tmdb_id == b.tmdb_id && a.title == b.title && a.year == b.year &&
           a.genres == b.genres && a.certification == b.certification &&
           a.path == b.path && a.length_s == b.length_s && a.codec == b.codec;
}
} // namespace

MovieLibraryStore::MovieLibraryStore(std::string snapshot_path, Fetch fetch, Exists exists,
                                   FileSink sink, Clock clock, DriveReady drive_ready)
    : path_(std::move(snapshot_path)), fetch_(std::move(fetch)), exists_(std::move(exists)),
      sink_(sink ? std::move(sink) : FileSink(&MediaInfoStore::write_file_durably)),
      clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })),
      drive_ready_(drive_ready ? std::move(drive_ready) : DriveReady([] { return true; })),
      owner_thread_(std::this_thread::get_id()) {}

void MovieLibraryStore::submit() {
    worker_.submit([this] { return run_job(); });
}

void MovieLibraryStore::start() {
    if (started_) return;
    started_ = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        read_requested_ = true;
    }
    submit();
}

void MovieLibraryStore::request_drive_check() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        drive_check_requested_ = true;
    }
    submit();
}

bool MovieLibraryStore::poll(bool tunnel_healthy) {
    std::optional<std::vector<LibraryMovie>> next;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (read_finished_) first_load_finished_ = true;
        next.swap(result_);
        if (attempt_finished_) {
            last_attempt_finished_ = attempt_finished_;
            attempt_finished_.reset();
            fetch_outstanding_ = false;
        }
    }
    if (tunnel_healthy && !fetch_outstanding_ &&
        (!last_attempt_finished_ || clock_() - *last_attempt_finished_ >= kRefreshInterval)) {
        fetch_outstanding_ = true;
        {
            std::lock_guard<std::mutex> lk(mu_);
            fetch_requested_ = true;
        }
        submit();
    }
    if (!next || (next->size() == movies_.size() &&
                  std::equal(next->begin(), next->end(), movies_.begin(), same_movie))) return false;
    movies_ = std::move(*next);
    return true;
}

bool MovieLibraryStore::flush_until(std::chrono::steady_clock::time_point deadline) {
    if (!worker_.flush_until(deadline)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    return !write_failed_;
}

void MovieLibraryStore::publish_existing() {
    std::vector<LibraryMovie> available;
    // A mount can disappear while stale files remain at the mountpoint.
    // Never publish those files unless the drive itself is ready.
    if (drive_ready_()) {
        for (const auto& movie : full_list_) {
            if (exists_(movie.path)) available.push_back(movie);
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    result_ = std::move(available);
}

bool MovieLibraryStore::run_job() {
    bool read = false;
    bool fetch = false;
    bool drive_check = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        read = std::exchange(read_requested_, false);
        fetch = std::exchange(fetch_requested_, false);
        drive_check = std::exchange(drive_check_requested_, false);
    }
    if (!read && !fetch && !drive_check) return true;

    // LatestWinsWorker falls back inline if thread creation fails. This
    // store must NEVER run its I/O there: treat the requests as failed
    // attempts, lifting boot's gate and rate-limiting another fetch. No
    // per-frame thread-creation retry storm on an exhausted system.
    if (std::this_thread::get_id() == owner_thread_) {
        std::lock_guard<std::mutex> lk(mu_);
        if (read) read_finished_ = true;
        if (fetch) attempt_finished_ = clock_();
        return false;
    }

    bool ok = true;
    if (read) {
        try {
            std::ifstream file(path_, std::ios::binary);
            if (file) {
                std::ostringstream body;
                body << file.rdbuf();
                if (!file.bad()) {
                    if (auto loaded = parse_movie_library(body.str())) full_list_ = std::move(*loaded);
                }
            }
            publish_existing();
        } catch (...) {
            ok = false;
        }
        std::lock_guard<std::mutex> lk(mu_);
        read_finished_ = true; // missing/corrupt/throwing reads also finish boot
    }

    bool fetched_list = false;
    if (fetch) {
        try {
            if (auto fetched = fetch_()) {
                full_list_ = std::move(*fetched);
                fetched_list = true;
                bool written = false;
                try {
                    written = sink_(path_, serialize_movie_library(full_list_));
                } catch (...) {
                    // A failed sink must not cost the in-memory library.
                }
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    write_failed_ = !written;
                }
                ok = written && ok;
            }
        } catch (...) {
            ok = false; // nullopt/exception retain the last full list and snapshot
        }
    }
    if (fetched_list || drive_check) {
        try {
            publish_existing();
        } catch (...) {
            ok = false; // I/O failure is not evidence that the drive is absent
        }
    }
    if (fetch) {
        const auto finished = clock_();
        std::lock_guard<std::mutex> lk(mu_);
        attempt_finished_ = finished;
    }
    return ok;
}

} // namespace app
