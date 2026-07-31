#include "media_browser/artwork/artwork_cache.h"

// stb_image implementation lives in utils/stb_image_impl.cpp — here we
// include the header in "decoder only" mode (no STB_IMAGE_IMPLEMENTATION
// define) so the symbols resolve against that TU's copy.
#include "../../utils/stb_image.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

// libcurl: only pulled in when we're actually going to run the real
// fetcher thread. Tests compile with ARTWORK_CACHE_TEST_MODE and do not
// need libcurl.
#ifndef ARTWORK_CACHE_TEST_MODE
#include <curl/curl.h>
#endif

// GL calls live only on the production (kiosk) build path. In test
// mode we substitute no-op stubs so the cache's bookkeeping can be
// exercised without an EGL context.
#ifndef ARTWORK_CACHE_TEST_MODE
#include <GLES2/gl2.h>
#endif

namespace media_browser {

namespace {

#ifndef ARTWORK_CACHE_TEST_MODE
// libcurl write callback: append incoming bytes to a std::string buffer.
std::size_t curl_write_to_string(void* contents, std::size_t size,
                                 std::size_t nmemb, void* userp) {
    std::size_t total = size * nmemb;
    auto* buf = static_cast<std::string*>(userp);
    buf->append(static_cast<char*>(contents), total);
    return total;
}
#endif

}  // namespace

ArtworkCache::ArtworkCache(std::size_t max_bytes, std::string disk_cache_dir)
    : max_bytes_(max_bytes), disk_cache_dir_(std::move(disk_cache_dir)) {
    spdlog::info("[artwork] cache ctor, max_bytes={}, disk_cache_dir='{}'",
                 max_bytes, disk_cache_dir_);

    // Ensure the disk cache directory exists. On any failure, log and
    // disable disk caching — the network-only path stays correct.
    if (!disk_cache_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(disk_cache_dir_, ec);
        if (ec) {
            spdlog::warn("[artwork] disk cache dir creation failed ('{}'): {} — "
                         "disk caching disabled, network-only mode",
                         disk_cache_dir_, ec.message());
            disk_cache_dir_.clear();
        } else {
            // Tally existing files so disk-LRU starts from a correct
            // baseline (not just bytes written this session). Walks the
            // directory once at boot — fast even with thousands of
            // entries.
            disk_recount_bytes();
            // Apply the LRU cap immediately in case a previous run
            // crashed before its own eviction sweep ran.
            disk_evict_lru_if_needed();
            spdlog::info("[artwork] disk cache ready at '{}' ({} bytes used / {} cap)",
                         disk_cache_dir_, disk_bytes_in_use_.load(), max_disk_bytes_);
        }
    }

#ifndef ARTWORK_CACHE_TEST_MODE
    fetcher_thread_ = std::thread(&ArtworkCache::fetcher_thread_main, this);
    spdlog::info("[artwork] fetcher thread spawned");
#endif
}

ArtworkCache::~ArtworkCache() {
    // Signal shutdown and wake the fetcher so it can exit cleanly. The
    // worker may be parked on either work_cv_ (waiting for a URL) or
    // pause_cv_ (waiting for resume), so notify both.
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        work_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        pause_cv_.notify_all();
    }
    if (fetcher_thread_.joinable()) {
        fetcher_thread_.join();
    }

#ifndef ARTWORK_CACHE_TEST_MODE
    // Release any GL textures we still own.
    std::lock_guard<std::mutex> lock(entries_mutex_);
    for (auto& [url, entry] : entries_) {
        if (entry.texture_id != 0) {
            glDeleteTextures(1, &entry.texture_id);
            entry.texture_id = 0;
        }
    }
#endif
}

std::uint32_t ArtworkCache::get_or_fetch(const std::string& url) {
    if (url.empty()) return 0;

    // Hot path: URL already cached as a GL texture — bump LRU, return id.
    {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        auto it = entries_.find(url);
        if (it != entries_.end()) {
            it->second.last_access = std::chrono::steady_clock::now();
            return it->second.texture_id;
        }
        // Poison-pill: URL has failed to decode 3+ times this session
        // (typically a TMDB JPEG that stb_image can't handle). Stop
        // re-enqueueing it — every UI frame would otherwise schedule a
        // fresh network round-trip. Counter incremented for diagnostics
        // (perf_report.sh surfaces it).
        if (dead_urls_.find(url) != dead_urls_.end()) {
            dead_url_skips_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }

    // Cold path: enqueue a fetch if we haven't already. In-flight dedup
    // handles the common case where the UI calls get_or_fetch() every
    // frame for a URL that's still downloading.
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        if (in_flight_.insert(url).second) {
            work_queue_.push_back(url);
            work_cv_.notify_one();
            spdlog::info("[artwork] enqueued url='{}' (queue_size={})",
                         url.substr(0, 80), work_queue_.size());
        }
    }
    return 0;
}

std::size_t ArtworkCache::pump() {
    // Drain the ready queue. Swap-then-process to minimize lock hold time.
    std::vector<PendingUpload> to_upload;
    {
        std::lock_guard<std::mutex> lock(ready_mutex_);
        to_upload.swap(ready_uploads_);
    }
    for (auto& p : to_upload) {
        bytes_waiting_upload_.fetch_sub(p.pixels_rgba.size(),
                                        std::memory_order_relaxed);
        upload_one(std::move(p));
    }
    return to_upload.size();
}

void ArtworkCache::upload_one(PendingUpload&& p) {
    if (p.pixels_rgba.empty() || p.width <= 0 || p.height <= 0) {
        // Decode failure — make sure we drop the in-flight marker so a
        // future retry is possible, but don't insert an entry.
        std::lock_guard<std::mutex> lock(work_mutex_);
        in_flight_.erase(p.url);
        return;
    }

    Entry entry;
    entry.bytes = static_cast<std::size_t>(p.width) *
                  static_cast<std::size_t>(p.height) * 4u;
    entry.width = p.width;
    entry.height = p.height;
    entry.last_access = std::chrono::steady_clock::now();

#ifndef ARTWORK_CACHE_TEST_MODE
    glGenTextures(1, &entry.texture_id);
    glBindTexture(GL_TEXTURE_2D, entry.texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, p.width, p.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, p.pixels_rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    spdlog::info("[artwork] uploaded poster: url='{}' {}x{} tex_id={}",
                 p.url, p.width, p.height, entry.texture_id);
#else
    // TEST_MODE: use a non-zero sentinel so callers can distinguish
    // "uploaded" entries from "not yet uploaded" ones without touching GL.
    static std::uint32_t test_next_id = 1;
    entry.texture_id = test_next_id++;
#endif

    {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        entries_[p.url] = entry;
        bytes_in_use_ += entry.bytes;
        evict_lru_locked();
    }

    // Drop the in-flight marker. (Not strictly needed — the entries_
    // map lookup in get_or_fetch would hit — but keeps the set from
    // growing unboundedly.)
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        in_flight_.erase(p.url);
    }
}

void ArtworkCache::evict_lru_locked() {
    // Called with entries_mutex_ held. Walk entries, find the oldest
    // last_access, drop it, repeat until under budget. Simple O(n*k)
    // algorithm — fine for n in the low thousands (our budget size).
    while (bytes_in_use_ > max_bytes_ && !entries_.empty()) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.last_access < oldest->second.last_access) {
                oldest = it;
            }
        }
#ifndef ARTWORK_CACHE_TEST_MODE
        if (oldest->second.texture_id != 0) {
            glDeleteTextures(1, &oldest->second.texture_id);
        }
#endif
        bytes_in_use_ -= oldest->second.bytes;
        spdlog::debug("[artwork] evicted LRU entry url='{}' ({} bytes)",
                      oldest->first, oldest->second.bytes);
        entries_.erase(oldest);
    }
}

std::size_t ArtworkCache::entries_count() const {
    std::lock_guard<std::mutex> lock(entries_mutex_);
    return entries_.size();
}

std::size_t ArtworkCache::bytes_in_use() const {
    std::lock_guard<std::mutex> lock(entries_mutex_);
    return bytes_in_use_;
}

std::size_t ArtworkCache::bytes_waiting_upload() const {
    return bytes_waiting_upload_.load(std::memory_order_relaxed);
}

std::optional<ArtworkCache::TextureDims>
ArtworkCache::get_dims(const std::string& url) const {
    std::lock_guard<std::mutex> lock(entries_mutex_);
    auto it = entries_.find(url);
    if (it == entries_.end()) return std::nullopt;
    if (it->second.width <= 0 || it->second.height <= 0) return std::nullopt;
    return TextureDims{it->second.width, it->second.height};
}

// ---------------------------------------------------------------------------
// Pause / resume — gates the fetcher thread during movie playback so the
// artwork worker doesn't compete with GStreamer for I/O bandwidth on the
// USB SSD that holds the library files.
// ---------------------------------------------------------------------------

void ArtworkCache::pause() {
    if (paused_.exchange(true, std::memory_order_acq_rel)) return;  // already paused
    spdlog::info("[artwork] paused (playback contention guard)");
}

void ArtworkCache::resume() {
    if (!paused_.exchange(false, std::memory_order_acq_rel)) return;  // already running
    spdlog::info("[artwork] resumed");
    // Wake the fetcher in case it's idle on pause_cv_.
    {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        pause_cv_.notify_all();
    }
    // Also kick the work_cv in case there's queued work and the fetcher
    // is sleeping on it (it would wake on its own when pause clears, but
    // notifying here makes the resume snappier in tests).
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        work_cv_.notify_all();
    }
}

void ArtworkCache::clear_textures() {
    // Discard queued-but-not-yet-uploaded pixel buffers first so pump()
    // after a game doesn't re-upload stale posters. Their in-flight
    // markers are normally erased at upload time (upload_one), so erase
    // them here too — otherwise get_or_fetch() would treat these URLs
    // as forever-pending and never re-enqueue them.
    std::vector<PendingUpload> discarded;
    {
        std::lock_guard<std::mutex> ready_lock(ready_mutex_);
        discarded.swap(ready_uploads_);
        bytes_waiting_upload_.store(0, std::memory_order_relaxed);
    }
    if (!discarded.empty()) {
        std::lock_guard<std::mutex> work_lock(work_mutex_);
        for (const auto& upload : discarded) {
            in_flight_.erase(upload.url);
        }
    }

    std::size_t dropped = 0;
    {
        std::lock_guard<std::mutex> entries_lock(entries_mutex_);
        dropped = entries_.size();
#ifndef ARTWORK_CACHE_TEST_MODE
        for (auto& [url, entry] : entries_) {
            if (entry.texture_id != 0) {
                glDeleteTextures(1, &entry.texture_id);
            }
        }
#endif
        entries_.clear();
        bytes_in_use_ = 0;
    }
    spdlog::info("[artwork] cleared {} textures + {} queued uploads "
                 "for game session", dropped, discarded.size());
}

// ---------------------------------------------------------------------------
// Disk cache helpers
// ---------------------------------------------------------------------------

std::string ArtworkCache::disk_cache_path(const std::string& url) const {
    if (disk_cache_dir_.empty()) return {};
    // std::hash isn't cryptographic but it's plenty for cache keys —
    // two distinct URLs hashing to the same file just means a cache miss
    // (the loaded JPEG would decode to the wrong image, so we ALWAYS
    // re-decode the body, never trust the filename alone). Hex-encode
    // the 64-bit hash for a stable filename.
    auto h = std::hash<std::string>{}(url);
    std::ostringstream ss;
    ss << std::hex << h << ".jpg";
    return disk_cache_dir_ + "/" + ss.str();
}

bool ArtworkCache::try_load_from_disk(const std::string& url,
                                      int& w, int& h,
                                      std::vector<std::uint8_t>& pixels_rgba) {
#ifdef ARTWORK_CACHE_TEST_MODE
    // stb_image's symbols come from utils/stb_image_impl.cpp's
    // STB_IMAGE_IMPLEMENTATION define — not linked into the test binary. The disk
    // read+decode path is exercised live on the Pi (perf_report.sh
    // captures hit/miss counters); tests just verify the wiring around
    // it (dir creation, stats accessors, pause/resume).
    (void)url; (void)w; (void)h; (void)pixels_rgba;
    return false;
#else
    if (disk_cache_dir_.empty()) return false;
    const std::string path = disk_cache_path(url);
    if (path.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        disk_misses_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Slurp the file
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        disk_misses_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::vector<unsigned char> body((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    if (body.empty()) {
        disk_misses_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(
        body.data(), static_cast<int>(body.size()), &w, &h, &channels, 4);
    if (!decoded) {
        // Corrupted cache entry — drop it so future attempts will refetch.
        spdlog::warn("[artwork] disk cache entry corrupt, removing: '{}'", path);
        std::filesystem::remove(path, ec);
        disk_misses_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(w) *
                          static_cast<std::size_t>(h) * 4u;
    pixels_rgba.assign(decoded, decoded + n);
    stbi_image_free(decoded);
    disk_hits_.fetch_add(1, std::memory_order_relaxed);
    return true;
#endif
}

void ArtworkCache::write_to_disk(const std::string& url,
                                 const std::string& body) {
    if (disk_cache_dir_.empty()) return;
    if (body.empty()) return;
    const std::string path = disk_cache_path(url);
    if (path.empty()) return;

    // Atomic-ish: write to a temp then rename. Avoids a half-written
    // file being left behind if the process is killed mid-write.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            spdlog::warn("[artwork] disk cache write failed (open): '{}'", tmp);
            return;
        }
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f) {
            spdlog::warn("[artwork] disk cache write failed (write): '{}'", tmp);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        spdlog::warn("[artwork] disk cache rename failed '{}' -> '{}': {}",
                     tmp, path, ec.message());
        std::filesystem::remove(tmp, ec);
        return;
    }
    disk_writes_.fetch_add(1, std::memory_order_relaxed);
    disk_bytes_in_use_.fetch_add(body.size(), std::memory_order_relaxed);
    // Trigger an eviction sweep when this write pushes us over the cap.
    // Cheap when the cache is well below cap (single load + compare).
    if (disk_bytes_in_use_.load(std::memory_order_relaxed) > max_disk_bytes_) {
        disk_evict_lru_if_needed();
    }
}

void ArtworkCache::disk_recount_bytes() {
    if (disk_cache_dir_.empty()) return;
    std::error_code ec;
    std::size_t total = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(disk_cache_dir_, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::error_code sec;
        const auto sz = entry.file_size(sec);
        if (!sec) total += sz;
    }
    disk_bytes_in_use_.store(total, std::memory_order_relaxed);
}

void ArtworkCache::disk_evict_lru_if_needed() {
    if (disk_cache_dir_.empty()) return;
    if (disk_bytes_in_use_.load(std::memory_order_relaxed) <= max_disk_bytes_) {
        return;
    }

    // Gather (mtime, size, path) for all regular files in the cache dir
    // and sort ascending by mtime so we delete the oldest first.
    struct CacheFile {
        std::filesystem::file_time_type mtime;
        std::uintmax_t                  size;
        std::filesystem::path           path;
    };
    std::vector<CacheFile> files;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(disk_cache_dir_, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::error_code mec, sec;
        const auto mt = entry.last_write_time(mec);
        const auto sz = entry.file_size(sec);
        if (mec || sec) continue;
        files.push_back({mt, sz, entry.path()});
    }
    std::sort(files.begin(), files.end(),
              [](const CacheFile& a, const CacheFile& b) {
                  return a.mtime < b.mtime;
              });

    std::size_t evicted = 0;
    std::size_t bytes_freed = 0;
    std::size_t cur = disk_bytes_in_use_.load(std::memory_order_relaxed);
    for (const auto& f : files) {
        if (cur <= max_disk_bytes_) break;
        std::error_code rec;
        std::filesystem::remove(f.path, rec);
        if (rec) {
            spdlog::warn("[artwork] disk cache evict failed '{}': {}",
                         f.path.string(), rec.message());
            continue;
        }
        cur -= std::min<std::uintmax_t>(f.size, cur);
        bytes_freed += f.size;
        ++evicted;
    }
    disk_bytes_in_use_.store(cur, std::memory_order_relaxed);
    if (evicted > 0) {
        disk_evictions_.fetch_add(evicted, std::memory_order_relaxed);
        spdlog::info("[artwork] disk LRU evicted {} files ({} bytes freed, {} bytes left / {} cap)",
                     evicted, bytes_freed, cur, max_disk_bytes_);
    }
}

// ---------------------------------------------------------------------------
// Background fetcher thread
// ---------------------------------------------------------------------------

void ArtworkCache::fetcher_thread_main() {
#ifndef ARTWORK_CACHE_TEST_MODE
    spdlog::info("[artwork] fetcher thread entered, initializing curl");
    // Initialize libcurl once per thread. curl_global_init is called at
    // process start by RadarrClient already, so we don't need it here;
    // per-handle init is the per-thread part.
    CURL* curl = curl_easy_init();
    if (!curl) {
        spdlog::error("[artwork] curl_easy_init failed — fetcher thread exiting");
        return;
    }
    spdlog::info("[artwork] fetcher thread ready, entering work loop");
    // Sensible defaults for TMDB image CDN. 15s connect, 30s transfer.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "magic-dingus-box/1.0");

    while (!stop_.load(std::memory_order_acquire)) {
        // Pause gate: when paused (movie playback), block here until
        // resumed. Stop signal still wakes us so shutdown is clean.
        {
            std::unique_lock<std::mutex> lock(pause_mutex_);
            pause_cv_.wait(lock, [&] {
                return stop_.load(std::memory_order_acquire) ||
                       !paused_.load(std::memory_order_acquire);
            });
            if (stop_.load(std::memory_order_acquire)) break;
        }

        std::string url;
        {
            std::unique_lock<std::mutex> lock(work_mutex_);
            work_cv_.wait(lock, [&] {
                return stop_.load(std::memory_order_acquire) ||
                       !work_queue_.empty();
            });
            if (stop_.load(std::memory_order_acquire)) break;
            url = std::move(work_queue_.front());
            work_queue_.pop_front();
        }
        spdlog::info("[artwork] fetcher picked up url='{}'", url.substr(0, 80));

        // 1) Disk-cache hit path: skip network entirely. Median lookup
        //    on the USB SSD is sub-millisecond vs 7-15s from TMDB.
        {
            int dw = 0, dh = 0;
            std::vector<std::uint8_t> dpixels;
            if (try_load_from_disk(url, dw, dh, dpixels) && !dpixels.empty()) {
                PendingUpload pu;
                pu.url = url;
                pu.width = dw;
                pu.height = dh;
                pu.pixels_rgba = std::move(dpixels);
                spdlog::info("[artwork] disk-cache hit url='{}' {}x{}",
                             url.substr(0, 80), dw, dh);
                bytes_waiting_upload_.fetch_add(pu.pixels_rgba.size(),
                                                std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(ready_mutex_);
                ready_uploads_.push_back(std::move(pu));
                continue;
            }
        }

        // 2) Network path — only when disk cache missed (or is disabled).
        std::string body;
        body.reserve(256 * 1024);  // typical poster JPEG is 30-200KB
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_string);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

        CURLcode cc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (cc != CURLE_OK || http_code >= 400 || body.empty()) {
            spdlog::warn("[artwork] fetch failed url='{}' curl={} http={} bytes={}",
                         url, static_cast<int>(cc), http_code, body.size());
            // Drop the in-flight marker so a future get_or_fetch() can retry.
            std::lock_guard<std::mutex> lock(work_mutex_);
            in_flight_.erase(url);
            continue;
        }

        int w = 0, h = 0, channels = 0;
        unsigned char* decoded = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(body.data()),
            static_cast<int>(body.size()), &w, &h, &channels, 4);
        if (!decoded) {
            spdlog::warn("[artwork] decode failed url='{}' bytes={}",
                         url, body.size());
            // Track the failure. After kMaxDecodeFailures, mark the URL
            // dead so get_or_fetch stops re-enqueueing it on every UI
            // frame. Some TMDB JPEGs trip stb_image deterministically;
            // without this guard one bad poster generates ~10 net
            // requests/sec for the kiosk's lifetime.
            {
                std::lock_guard<std::mutex> elock(entries_mutex_);
                int& count = failure_counts_[url];
                if (++count >= kMaxDecodeFailures) {
                    dead_urls_.insert(url);
                    failure_counts_.erase(url);
                    spdlog::warn("[artwork] url marked DEAD after {} decode failures: '{}'",
                                 kMaxDecodeFailures, url);
                }
            }
            std::lock_guard<std::mutex> lock(work_mutex_);
            in_flight_.erase(url);
            continue;
        }

        // 3) Persist the raw JPEG bytes to disk so subsequent fetches
        //    of this URL (this session OR after reboot) hit the
        //    sub-millisecond disk path instead of TMDB.
        write_to_disk(url, body);

        PendingUpload pu;
        pu.url = url;
        pu.width = w;
        pu.height = h;
        const std::size_t n = static_cast<std::size_t>(w) *
                              static_cast<std::size_t>(h) * 4u;
        pu.pixels_rgba.assign(decoded, decoded + n);
        stbi_image_free(decoded);

        spdlog::info("[artwork] fetched poster url='{}' {}x{} bytes={}",
                     url, w, h, body.size());

        bytes_waiting_upload_.fetch_add(n, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            ready_uploads_.push_back(std::move(pu));
        }
    }

    curl_easy_cleanup(curl);
#endif
}

// ---------------------------------------------------------------------------
// Test-only hooks
// ---------------------------------------------------------------------------

#ifdef ARTWORK_CACHE_TEST_MODE
void ArtworkCache::test_inject_ready_upload(TestPendingUpload upload) {
    PendingUpload pu;
    pu.url = std::move(upload.url);
    pu.width = upload.width;
    pu.height = upload.height;
    pu.pixels_rgba = std::move(upload.pixels_rgba);

    bytes_waiting_upload_.fetch_add(pu.pixels_rgba.size(),
                                    std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(ready_mutex_);
    ready_uploads_.push_back(std::move(pu));
}

std::size_t ArtworkCache::pump_for_tests() {
    // Same shape as pump() but skips GL (upload_one already branches on
    // ARTWORK_CACHE_TEST_MODE internally).
    return pump();
}

void ArtworkCache::test_touch(const std::string& url) {
    std::lock_guard<std::mutex> lock(entries_mutex_);
    auto it = entries_.find(url);
    if (it != entries_.end()) {
        it->second.last_access = std::chrono::steady_clock::now();
    }
}

bool ArtworkCache::test_is_in_flight(const std::string& url) {
    std::lock_guard<std::mutex> lock(work_mutex_);
    return in_flight_.count(url) > 0;
}
#endif

}  // namespace media_browser
