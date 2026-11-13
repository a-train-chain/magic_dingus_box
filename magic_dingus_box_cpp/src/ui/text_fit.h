#pragma once

// Fitting a string to a pixel width with the font's REAL measurement — Zen
// Dots is proportional, so a character count cannot do it (the Channel
// Guide's now-playing panel: playlist title, item title, artist, next
// title). Pure and header-only: the measure is passed in — FontManager::
// get_text_width in the renderer, a fake in tests/ui/test_text_fit.cpp.
// Every cut falls on a code-point boundary, never inside a UTF-8 sequence
// (a split sequence draws a replacement box — ui/text_utf8.h).
//
// `measure(s)` must return the width of `s` in px and grow (weakly) as `s`
// grows by whole code points; FontManager's sum of advances does.

#include <cstddef>
#include <string>
#include <vector>

#include "text_utf8.h"

namespace ui {

inline constexpr const char* kFitEllipsis = "\xE2\x80\xA6";  // "…"

namespace text_fit_detail {

inline std::size_t code_points(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if (!utf8_is_continuation(c)) ++n;
    }
    return n;
}

// Bytes in the first `n` code points of `s` (all of it when shorter).
inline std::size_t prefix_bytes(const std::string& s, std::size_t n) {
    std::size_t cp = 0, i = 0;
    while (i < s.size()) {
        if (!utf8_is_continuation(static_cast<unsigned char>(s[i]))) {
            if (cp == n) break;
            ++cp;
        }
        ++i;
    }
    return i;
}

inline void trim_trailing_spaces(std::string& s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
}

inline std::size_t skip_spaces(const std::string& s, std::size_t pos) {
    while (pos < s.size() && s[pos] == ' ') ++pos;
    return pos;
}

}  // namespace text_fit_detail

// `text` when it measures <= max_w. Otherwise the longest code-point prefix
// that still fits with "…" after it (spaces before the "…" dropped), and ""
// when not even "…" fits.
template <class Measure>
std::string fit_to_width(const std::string& text, int max_w, Measure&& measure) {
    namespace d = text_fit_detail;
    if (text.empty() || measure(text) <= max_w) return text;
    const std::string ellipsis = kFitEllipsis;
    if (measure(ellipsis) > max_w) return "";
    // Largest k in [0, n) with prefix(k) + "…" <= max_w; k = 0 fits (above).
    const std::size_t n = d::code_points(text);
    std::size_t lo = 0, hi = n - 1;
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        if (measure(text.substr(0, d::prefix_bytes(text, mid)) + ellipsis) <= max_w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    std::string out = text.substr(0, d::prefix_bytes(text, lo));
    d::trim_trailing_spaces(out);
    return out + ellipsis;
}

// Word-wraps `text` into at most `max_lines` lines of max_w: lines break at
// spaces; a single word wider than a line is cut between code points; the
// last line takes whatever is left, fitted with "…" (fit_to_width).
template <class Measure>
std::vector<std::string> wrap_to_lines(const std::string& text, int max_w, int max_lines,
                                       Measure&& measure) {
    namespace d = text_fit_detail;
    std::vector<std::string> lines;
    if (max_lines <= 0) return lines;
    std::size_t pos = d::skip_spaces(text, 0);
    while (pos < text.size() && static_cast<int>(lines.size()) + 1 < max_lines) {
        const std::string rest = text.substr(pos);
        if (measure(rest) <= max_w) {
            lines.push_back(rest);
            return lines;
        }
        // The longest run of whole words that fits.
        std::size_t take = 0;  // bytes of `rest` on this line
        std::size_t space = rest.find(' ');
        while (space != std::string::npos) {
            if (measure(rest.substr(0, space)) > max_w) break;
            take = space;
            space = rest.find(' ', d::skip_spaces(rest, space));
        }
        if (take == 0) {
            // The first word alone is too wide: as many of its code points
            // as fit (at least one, so the wrap always moves on).
            const std::size_t n = d::code_points(rest);
            std::size_t k = 1;
            while (k < n && measure(rest.substr(0, d::prefix_bytes(rest, k + 1))) <= max_w) ++k;
            take = d::prefix_bytes(rest, k);
        }
        std::string line = rest.substr(0, take);
        d::trim_trailing_spaces(line);
        lines.push_back(line);
        pos = d::skip_spaces(text, pos + take);
    }
    if (pos < text.size()) {
        std::string last = fit_to_width(text.substr(pos), max_w, measure);
        if (!last.empty()) lines.push_back(last);
    }
    return lines;
}

}  // namespace ui
