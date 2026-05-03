#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct PairedDevice {
    std::string id;       // device_id; needed to forget the right entry
    std::string nickname;
    int64_t last_seen;  // Unix seconds
};

// Convenience: parse paired_remotes.json from the given path.
// Returns empty vector if the file is missing or malformed.
std::vector<PairedDevice> load_paired_devices(const std::string& path);

// mtime-based shared cache. Both the renderer (for display) and main.cpp
// (for input handling — selecting/forgetting devices) must read the same
// list, otherwise after a forget the renderer can show a stale device for
// up to 1 s. The cache reloads when the file's mtime changes.
//
// THREAD-SAFETY: must be called from the main thread only (single shared
// static, no locking).
const std::vector<PairedDevice>& paired_devices_cached(const std::string& path);

}  // namespace ui
