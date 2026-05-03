#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct PairedDevice {
    std::string nickname;
    int64_t last_seen;  // Unix seconds
};

// Convenience: parse paired_remotes.json from the given path.
// Returns empty vector if the file is missing or malformed.
std::vector<PairedDevice> load_paired_devices(const std::string& path);

}  // namespace ui
