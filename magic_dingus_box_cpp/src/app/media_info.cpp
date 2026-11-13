#include "media_info.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

#include <json/json.h>

#include "../utils/fsync_util.h"
#include "../utils/logger.h"

namespace fs = std::filesystem;

namespace app {

namespace {

constexpr int kVersion = 1;

Json::Value fresh_root() {
    Json::Value root(Json::objectValue);
    root["version"] = kVersion;
    root["videos"] = Json::Value(Json::objectValue);
    return root;
}

// Text -> a root object whose "videos" is an object (added when absent).
// False when the text is not that. Blank text is a fresh, empty root: a
// zero-length file is what a power cut before the first write leaves.
bool parse_root(const std::string& text, Json::Value& root) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        root = fresh_root();
        return true;
    }
    Json::CharReaderBuilder builder;
    // Trailing garbage is corrupt, as it is to the web admin's json.loads.
    builder["failIfExtra"] = true;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value parsed;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &parsed, &errors)) {
        return false;
    }
    if (!parsed.isObject()) return false;
    if (!parsed.isMember("videos")) {
        parsed["videos"] = Json::Value(Json::objectValue);
    } else if (!parsed["videos"].isObject()) {
        return false;
    }
    root = std::move(parsed);
    return true;
}

// A length or aspect: a positive finite number (an integer counts).
std::optional<double> positive_number(const Json::Value& v) {
    if (!v.isDouble()) return std::nullopt;  // int, uint or real; never bool
    const double d = v.asDouble();
    if (!std::isfinite(d) || d <= 0.0) return std::nullopt;
    return d;
}

// What the kiosk reads out of a parsed root. An entry that is not an
// object, or that holds nothing the kiosk uses, is left out.
MediaInfoMap map_from_root(const Json::Value& root) {
    MediaInfoMap out;
    const Json::Value& videos = root["videos"];
    for (auto it = videos.begin(); it != videos.end(); ++it) {
        const Json::Value& entry = *it;
        if (!entry.isObject()) continue;
        MediaInfo info;
        info.duration_s = positive_number(entry["duration_s"]);
        info.display_aspect = positive_number(entry["display_aspect"]);
        if (entry["crop"].isBool()) info.crop = entry["crop"].asBool();
        if (!info.duration_s && !info.display_aspect && !info.crop) continue;
        out[it.name()] = info;
    }
    return out;
}

// The settings.json writer's settings (settings_persistence.cpp).
std::string to_text(const Json::Value& root) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return Json::writeString(builder, root) + "\n";
}

// One crop edit, on the file's JSON: false is written, true (the default)
// removes the field, and an entry left with nothing in it goes too. Fields
// the kiosk does not know are never touched.
void apply_edit(Json::Value& videos, const std::string& key, bool crop) {
    if (!crop) {
        Json::Value& entry = videos[key];
        if (!entry.isObject()) entry = Json::Value(Json::objectValue);
        entry["crop"] = false;
        return;
    }
    if (!videos.isMember(key)) return;
    Json::Value& entry = videos[key];
    if (!entry.isObject()) return;
    entry.removeMember("crop");
    if (entry.empty()) videos.removeMember(key);
}

// The same edit on the kiosk's map.
void apply_edit(MediaInfoMap& map, const std::string& key, bool crop) {
    if (!crop) {
        map[key].crop = false;
        return;
    }
    const auto it = map.find(key);
    if (it == map.end()) return;
    it->second.crop.reset();
    if (!it->second.duration_s && !it->second.display_aspect) map.erase(it);
}

// The whole file; "" when it does not exist. False on any other error.
bool read_text_file(const std::string& path, std::string& out) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            ::close(fd);
            errno = err;  // for the caller's log line
            return false;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return true;
}

// flock(LOCK_EX) on the sidecar lock file for the object's lifetime.
// Blocking on purpose: it runs on the worker thread, where waiting for the
// web admin's write is free and means the two simply take turns.
class SidecarLock {
public:
    explicit SidecarLock(const std::string& path) {
        // O_CLOEXEC: the main thread fork/execs RetroArch and helpers, and
        // none of them may inherit a descriptor holding the lock.
        // O_RDONLY is enough for flock, and needs no write permission on
        // a lock file the web admin created.
        fd_ = ::open(path.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0) {
            error_ = errno;
            return;
        }
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR) continue;
            error_ = errno;
            ::close(fd_);
            fd_ = -1;
            return;
        }
    }
    ~SidecarLock() {
        if (fd_ < 0) return;
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
    SidecarLock(const SidecarLock&) = delete;
    SidecarLock& operator=(const SidecarLock&) = delete;

    bool held() const { return fd_ >= 0; }
    int error() const { return error_; }  // errno when not held

private:
    int fd_ = -1;
    int error_ = 0;
};

}  // namespace

std::optional<MediaInfoMap> parse_media_info(const std::string& json) {
    Json::Value root;
    if (!parse_root(json, root)) return std::nullopt;
    return map_from_root(root);
}

std::string serialize_media_info(const MediaInfoMap& map) {
    Json::Value root = fresh_root();
    Json::Value& videos = root["videos"];
    for (const auto& [key, info] : map) {
        Json::Value entry(Json::objectValue);
        if (info.duration_s) entry["duration_s"] = *info.duration_s;
        if (info.display_aspect) entry["display_aspect"] = *info.display_aspect;
        // Cropped is the default, so only `false` is ever written.
        if (info.crop && !*info.crop) entry["crop"] = false;
        videos[key] = entry;
    }
    return to_text(root);
}

std::string media_key_for(const std::string& resolved_path, const std::string& data_dir) {
    if (resolved_path.empty() || data_dir.empty()) return "";
    const fs::path file = fs::path(resolved_path).lexically_normal();
    fs::path dir = fs::path(data_dir).lexically_normal();
    // "/opt/x/data/" -> "/opt/x/data": a trailing separator is an empty
    // last element, which would otherwise take part in the comparison.
    if (!dir.has_filename() && dir.has_parent_path() && dir != dir.root_path()) {
        dir = dir.parent_path();
    }
    if (file.is_absolute() != dir.is_absolute()) return "";
    const fs::path rel = file.lexically_relative(dir);
    if (rel.empty() || rel == ".") return "";
    if (*rel.begin() == "..") return "";
    return rel.generic_string();
}

bool crop_for(const MediaInfoMap& map, const std::string& key) {
    const auto it = map.find(key);
    if (it == map.end() || !it->second.crop) return true;
    return *it->second.crop;
}

MediaInfoStore::MediaInfoStore(std::string path)
    : MediaInfoStore(std::move(path), &MediaInfoStore::write_file_durably) {}

MediaInfoStore::MediaInfoStore(std::string path, FileSink sink, Clock clock)
    : path_(std::move(path)),
      lock_path_(path_ + ".lock"),
      sink_(std::move(sink)),
      clock_(clock ? std::move(clock) : Clock(&std::chrono::steady_clock::now)) {}

bool MediaInfoStore::write_file_durably(const std::string& path, const std::string& body) {
    // The kiosk's staging name. The web admin stages through its own
    // mkstemp name, and the sidecar lock keeps the two writers apart.
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            LOG_WARN("media_info: cannot open {} for writing", tmp_path);
            return false;
        }
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
        file.close();
        if (file.fail()) {
            LOG_WARN("media_info: writing {} failed", tmp_path);
            std::error_code ec;
            fs::remove(tmp_path, ec);
            return false;
        }
    }
    // Data before the rename, or a power cut can leave a zero-length file
    // under the final name (fsync_util.h). Best-effort, like settings.json.
    if (!utils::fsync_file(tmp_path)) {
        LOG_WARN("media_info: fsync of {} failed — writing on without the "
                 "durability guarantee", tmp_path);
    }
    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        LOG_WARN("media_info: rename {} -> {} failed: {}", tmp_path, path, ec.message());
        std::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        return false;
    }
    (void)utils::fsync_parent_dir(path);
    return true;
}

void MediaInfoStore::submit() {
    // Never waits for a job in flight. A job the worker has not started is
    // replaced by this one, which does the same thing: take everything.
    worker_.submit([this] { return run_job(); });
}

bool MediaInfoStore::request_reload(const std::string& consume_marker) {
    bool queued_new = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!reload_requested_) queued_new = true;
        reload_requested_ = true;
        if (!consume_marker.empty() && reload_marker_ != consume_marker) {
            reload_marker_ = consume_marker;
            queued_new = true;
        }
    }
    // Submitted either way, as before: when a failed read left the request
    // waiting with no job queued, this is what retries it at once.
    submit();
    return queued_new;
}

void MediaInfoStore::set_crop(const std::string& key, bool crop) {
    if (key.empty()) return;
    apply_edit(map_, key, crop);
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_[key] = crop;
    }
    submit();
}

bool MediaInfoStore::poll() {
    std::optional<MediaInfoMap> next;
    Edits older;
    Edits newer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (result_) {
            next = std::move(result_);
            result_.reset();
            older = inflight_;
            newer = pending_;
        }
    }
    retry_stranded_work();
    if (!next) return false;
    // What the worker read predates every edit still in flight or queued:
    // lay them back on top, oldest first, or a reload (or the result of an
    // earlier write) would undo them on screen.
    for (const auto& [key, crop] : older) apply_edit(*next, key, crop);
    for (const auto& [key, crop] : newer) apply_edit(*next, key, crop);
    if (*next == map_) return false;
    map_ = std::move(*next);
    return true;
}

void MediaInfoStore::retry_stranded_work() {
    // A queued or running job will take whatever is waiting; leave the
    // schedule as it is until that job is done.
    if (worker_.busy()) return;
    bool waiting = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        waiting = !pending_.empty() || reload_requested_;
    }
    if (!waiting) {
        retry_armed_ = false;
        return;
    }
    // Work waiting with the worker idle means a job failed and left it.
    // The first retry comes kRetryInterval after it was first seen here,
    // then one per kRetryInterval while it keeps failing.
    const auto now = clock_();
    if (!retry_armed_) {
        retry_armed_ = true;
        next_retry_ = now + kRetryInterval;
        return;
    }
    if (now < next_retry_) return;
    next_retry_ = now + kRetryInterval;
    submit();
}

bool MediaInfoStore::edits_waiting() {
    std::lock_guard<std::mutex> lk(mu_);
    return !pending_.empty() || !inflight_.empty();
}

bool MediaInfoStore::flush_until(std::chrono::steady_clock::time_point deadline) {
    for (int attempt = 0;; ++attempt) {
        if (!worker_.flush_until(deadline)) return false;
        // Judged by what is still waiting, not by the flag: every job takes
        // all pending edits, and a failed write puts its edits back.
        (void)worker_.take_failure();
        if (!edits_waiting()) return true;
        if (attempt > 0) return false;
        LOG_WARN("media_info: the last write of {} failed — retrying once", path_);
        submit();
    }
}

bool MediaInfoStore::run_job() {
    Edits batch;
    bool want_read = false;
    std::string marker;
    {
        std::lock_guard<std::mutex> lk(mu_);
        batch.swap(pending_);
        inflight_ = batch;
        want_read = std::exchange(reload_requested_, false);
        marker = std::exchange(reload_marker_, std::string());
    }
    if (batch.empty() && !want_read) return true;  // an earlier job took it all

    std::optional<MediaInfoMap> on_disk;
    bool ok = false;
    try {
        if (!marker.empty()) {
            // The web admin's poke, consumed BEFORE the read: a poke written
            // after this point survives for the next reload. Gone already
            // (a repeat request) is fine.
            std::error_code ec;
            fs::remove(marker, ec);
        }
        ok = read_merge_write(batch, on_disk);
    } catch (const std::exception& e) {
        // jsoncpp throws on a misuse; never let it cost the edits.
        LOG_WARN("media_info: {} — {} change(s) kept for the next write",
                 e.what(), batch.size());
        ok = false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    if (on_disk) result_ = std::move(on_disk);
    if (!ok) {
        // Back in the queue, behind any newer edit for the same video.
        for (const auto& [key, crop] : inflight_) pending_.try_emplace(key, crop);
        if (!on_disk && want_read) reload_requested_ = true;
    }
    inflight_.clear();
    return ok;
}

bool MediaInfoStore::read_merge_write(const Edits& batch,
                                      std::optional<MediaInfoMap>& on_disk) {
    // Only a write takes the lock: a read sees a whole file either way.
    std::optional<SidecarLock> lock;
    if (!batch.empty()) {
        lock.emplace(lock_path_);
        if (!lock->held()) {
            LOG_WARN("media_info: cannot lock {} (errno {}) — {} change(s) kept "
                     "for the next write", lock_path_, lock->error(), batch.size());
            return false;
        }
    }

    std::string text;
    if (!read_text_file(path_, text)) {
        const int err = errno;
        LOG_WARN("media_info: cannot read {} (errno {})", path_, err);
        return false;
    }
    Json::Value root;
    if (!parse_root(text, root)) {
        LOG_WARN("media_info: {} is not valid JSON of the expected shape — every "
                 "video reads as cropped{}", path_,
                 batch.empty() ? "" : "; this write replaces it");
        root = fresh_root();
    }
    on_disk = map_from_root(root);
    if (batch.empty()) return true;

    // Merge into what is on disk NOW (the web admin may have written since
    // this process last read it), never into the kiosk's own map.
    for (const auto& [key, crop] : batch) apply_edit(root["videos"], key, crop);
    if (!root.isMember("version")) root["version"] = kVersion;
    if (!sink_(path_, to_text(root))) {
        LOG_WARN("media_info: writing {} failed — {} change(s) kept for the "
                 "next write", path_, batch.size());
        return false;
    }
    on_disk = map_from_root(root);
    return true;
}

}  // namespace app
