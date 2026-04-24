#include "media_browser/artwork/artwork_cache.h"

// stb_image implementation is already defined in renderer.cpp — here we
// include the header in "decoder only" mode (no STB_IMAGE_IMPLEMENTATION
// define) so the symbols resolve against renderer.cpp's copy.
#include "../../utils/stb_image.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
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

ArtworkCache::ArtworkCache(std::size_t max_bytes)
    : max_bytes_(max_bytes) {
    spdlog::info("[artwork] cache ctor, max_bytes={}", max_bytes);
#ifndef ARTWORK_CACHE_TEST_MODE
    fetcher_thread_ = std::thread(&ArtworkCache::fetcher_thread_main, this);
    spdlog::info("[artwork] fetcher thread spawned");
#endif
}

ArtworkCache::~ArtworkCache() {
    // Signal shutdown and wake the fetcher so it can exit cleanly.
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        work_cv_.notify_all();
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
            std::lock_guard<std::mutex> lock(work_mutex_);
            in_flight_.erase(url);
            continue;
        }

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
#endif

}  // namespace media_browser
