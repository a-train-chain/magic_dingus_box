#pragma once

// GBM buffer handle -> DRM framebuffer id cache used by FramePresenter.
// Pure bookkeeping, header-only, unit-tested on the Mac
// (tests/platform/test_fb_cache.cpp). The actual drmModeRmFB is injected
// so the eviction policy can be asserted without a DRM device.
//
// Two rules every removal path obeys, both learned from the cache this
// replaced (an unordered_map trimmed from begin()):
//
//   1. An fb that leaves the cache is RmFB'd. The old trim erased the
//      on-screen entry WITHOUT RmFB — the kernel object outlived its only
//      reference, one leak per such eviction until the fd closed.
//   2. The fb currently scanned out is never RmFB'd by a trim/recovery.
//      Removing a framebuffer that is on screen makes the kernel disable
//      the CRTC: a black TV, not a leak. So a trim evicts the oldest
//      entry that is NOT on screen (one always exists when size > max),
//      and the exhaustion recovery keeps the current entry.
//
// Eviction order is insertion order — "oldest" now means oldest, rather
// than whatever bucket the hash map iterated first.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace platform {

class FbCache {
public:
    using RemoveFn = std::function<void(uint32_t fb_id)>;

    std::optional<uint32_t> find(uint32_t bo_handle) const {
        for (const auto& e : entries_) {
            if (e.first == bo_handle) return e.second;
        }
        return std::nullopt;
    }

    // Adds bo_handle -> fb_id as the newest entry. A stale entry for the
    // same handle is replaced, and its fb RmFB'd when `rm` is given.
    void insert(uint32_t bo_handle, uint32_t fb_id, const RemoveFn& rm = {}) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->first == bo_handle) {
                if (rm && it->second != fb_id) rm(it->second);
                entries_.erase(it);
                break;
            }
        }
        entries_.emplace_back(bo_handle, fb_id);
    }

    // Evicts oldest-first down to max_entries, skipping current_fb_id.
    void trim(std::size_t max_entries, uint32_t current_fb_id,
              const RemoveFn& rm) {
        auto it = entries_.begin();
        while (entries_.size() > max_entries && it != entries_.end()) {
            if (it->second == current_fb_id) {
                ++it;
                continue;
            }
            if (rm) rm(it->second);
            it = entries_.erase(it);
        }
    }

    // Buffer-exhaustion recovery: drop everything but the on-screen fb.
    // current_fb_id == 0 (nothing presented yet) drops everything.
    void drop_all_except(uint32_t current_fb_id, const RemoveFn& rm) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (current_fb_id != 0 && it->second == current_fb_id) {
                ++it;
                continue;
            }
            if (rm) rm(it->second);
            it = entries_.erase(it);
        }
    }

    // Display reset / shutdown: none of our fbs is (or will stay) on
    // screen, so every one goes.
    void clear(const RemoveFn& rm) {
        if (rm) {
            for (const auto& e : entries_) rm(e.second);
        }
        entries_.clear();
    }

    std::size_t size() const { return entries_.size(); }

private:
    // A GBM surface cycles 2-4 buffers, so a linear scan beats hashing and
    // keeps insertion order for free.
    std::vector<std::pair<uint32_t, uint32_t>> entries_;
};

}  // namespace platform
