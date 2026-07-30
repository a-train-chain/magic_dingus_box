#include "media_browser/ui/mb_ui_utils.h"

#include <cstdio>
#include <vector>

#include "ui/text_utf8.h"

namespace media_browser::ui {

std::string truncate_to_width(const std::string& text, int font_size,
                              float max_w, const TextMeasureFn& measure) {
    // The `<=` (not `<`) and the bare-ellipsis fallthrough are both
    // load-bearing -- see the contract in the header before touching either.
    if (measure(text, font_size) <= max_w) return text;

    const std::string ellipsis = "...";

    // Candidate cut lengths: every codepoint boundary in [0, size). Cutting
    // anywhere else splits a multi-byte sequence and leaves an orphaned lead
    // byte that decodes to U+FFFD -- a replacement box on screen.
    //
    // text.size() itself is deliberately NOT a candidate: the full string just
    // failed to fit, and width is non-decreasing in prefix length (see the
    // header), so full + ellipsis cannot fit either. The old scan probed it
    // anyway and always wasted that call.
    std::vector<std::size_t> cuts;
    cuts.reserve(text.size() + 1);
    cuts.push_back(0);
    for (std::size_t i = 1; i < text.size(); ++i) {
        if (!::ui::utf8_is_continuation(static_cast<unsigned char>(text[i]))) {
            cuts.push_back(i);
        }
    }

    // Binary search for the LARGEST candidate that fits. Valid because "fits"
    // is monotone over the candidates -- again, the non-decreasing-width
    // assumption stated in the header.
    std::size_t lo = 0;
    std::size_t hi = cuts.size();
    std::size_t best = cuts.size();  // sentinel: nothing fits
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (measure(text.substr(0, cuts[mid]) + ellipsis, font_size) <= max_w) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    // Nothing fits -> the bare ellipsis, which does NOT itself fit and gets
    // drawn anyway. Note cuts[0] == 0 makes the found-nothing and found-empty
    // cases produce the same string, so this stays one observable behavior.
    if (best == cuts.size()) return ellipsis;
    return text.substr(0, cuts[best]) + ellipsis;
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
