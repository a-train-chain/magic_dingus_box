#include "media_browser/ui/mb_ui_utils.h"

#include <cstdio>

namespace media_browser::ui {

std::string truncate_to_width(const std::string& text, int font_size,
                              float max_w, const TextMeasureFn& measure) {
    // Reproduced verbatim from the six screen-local copies. The `<=` (not `<`)
    // and the bare-ellipsis fallthrough are both load-bearing -- see the
    // contract in the header before touching either.
    if (measure(text, font_size) <= max_w) return text;
    const std::string ellipsis = "...";
    for (size_t n = text.size(); n > 0; --n) {
        std::string candidate = text.substr(0, n) + ellipsis;
        if (measure(candidate, font_size) <= max_w) return candidate;
    }
    return ellipsis;
}

::ui::Color stable_tint_for_id(int id) {
    // Knuth multiplicative hash (2^32 / phi), then three overlapping byte
    // windows of the product biased up into the mid-dark range.
    uint32_t h = static_cast<uint32_t>(id) * 2654435761u;
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
}

float ease_in_cubic(float t) {
    return t * t * t;
}

float ease_out_cubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

std::string format_bytes(int64_t bytes) {
    // See the header: `<= 0` is the CALLER's decision, because "0 B" and "?"
    // mean different things on different screens. Returning "0 B" here is a
    // defined fallback so a missed guard is a wrong label, never UB.
    if (bytes <= 0) return "0 B";
    double v = static_cast<double>(bytes);
    const char* unit = "B";
    if (v >= 1024.0 * 1024.0 * 1024.0) {
        v /= (1024.0 * 1024.0 * 1024.0); unit = "GB";
    } else if (v >= 1024.0 * 1024.0) {
        v /= (1024.0 * 1024.0); unit = "MB";
    } else if (v >= 1024.0) {
        v /= 1024.0; unit = "KB";
    }
    char buf[32];
    // One decimal below 100 of the unit, none at or above it. LibraryScreen's
    // copy always printed a decimal; Queue's and ReleasePicker's dropped it at
    // 100, and the 2-of-3 majority wins -- so Library's storage readout now
    // says "500 MB" where it said "500.0 MB". Intended and user-visible.
    if (v >= 100.0) std::snprintf(buf, sizeof(buf), "%.0f %s", v, unit);
    else            std::snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
    return buf;
}

}  // namespace media_browser::ui
