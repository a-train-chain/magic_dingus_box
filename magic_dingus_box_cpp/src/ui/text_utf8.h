#pragma once
#include <cstdint>
#include <string>

namespace ui {

// True iff `b` is a UTF-8 continuation byte (10xxxxxx), i.e. the second or
// later byte of a multi-byte sequence. Lead bytes and ASCII are false, so a
// byte offset is a codepoint boundary exactly when this is false.
inline bool utf8_is_continuation(unsigned char b) {
    return (b & 0xC0) == 0x80;
}

// Snap a prefix LENGTH down to the nearest UTF-8 codepoint boundary.
//
// `n` is a count of bytes to keep (as in `s.substr(0, n)`), not an index of a
// byte to look at. The return value is the largest m <= n such that
// `s.substr(0, m)` ends on a codepoint boundary -- so it never splits a
// multi-byte sequence and leaves orphaned bytes behind.
//
// Cutting a sequence in half is not a cosmetic problem: decode_utf8 above
// returns U+FFFD for the orphan, so the kiosk draws a replacement box. Any
// code that shortens a string to fit a pixel budget must route its cut
// through here.
//
// n >= s.size() returns s.size() (the whole string is trivially a boundary).
// Malformed input cannot hang or over-read: the walk is bounded below by 0.
inline std::size_t utf8_floor_boundary(const std::string& s, std::size_t n) {
    if (n >= s.size()) return s.size();
    while (n > 0 && utf8_is_continuation(static_cast<unsigned char>(s[n]))) {
        --n;
    }
    return n;
}

// Drop the last codepoint from `s`, in place. The codepoint-wise replacement
// for `s.pop_back()` in a "shrink until it fits" loop -- popping single bytes
// there is what produced replacement boxes on accented and CJK titles.
// A no-op on an empty string, and always makes progress otherwise, so it is
// safe as a loop body.
inline void utf8_pop_back(std::string& s) {
    if (s.empty()) return;
    s.resize(utf8_floor_boundary(s, s.size() - 1));
}

// Decode one UTF-8 codepoint from `s` starting at `pos`. On return,
// `pos` is advanced past the consumed bytes. On invalid sequences,
// returns 0xFFFD (replacement character) and advances one byte so
// callers always make progress.
inline char32_t decode_utf8(const std::string& s, std::size_t& pos) {
    if (pos >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) {  // 1-byte ASCII
        ++pos;
        return c;
    }
    auto need_continuation = [&](int need) -> char32_t {
        if (pos + need >= s.size()) { pos = s.size(); return 0xFFFD; }
        for (int i = 1; i <= need; ++i) {
            if ((static_cast<unsigned char>(s[pos + i]) & 0xC0) != 0x80) {
                ++pos;  // resync one byte
                return 0xFFFD;
            }
        }
        return 0;  // signal OK
    };
    char32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {  // 2-byte: 110xxxxx 10xxxxxx
        if (char32_t err = need_continuation(1); err) return err;
        cp = (c & 0x1F) << 6;
        cp |= (static_cast<unsigned char>(s[pos + 1]) & 0x3F);
        pos += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0) {  // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
        if (char32_t err = need_continuation(2); err) return err;
        cp = (c & 0x0F) << 12;
        cp |= (static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6;
        cp |= (static_cast<unsigned char>(s[pos + 2]) & 0x3F);
        pos += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0) {  // 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        if (char32_t err = need_continuation(3); err) return err;
        cp = (c & 0x07) << 18;
        cp |= (static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12;
        cp |= (static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6;
        cp |= (static_cast<unsigned char>(s[pos + 3]) & 0x3F);
        pos += 4;
        return cp;
    }
    // Invalid lead byte — emit replacement, resync.
    ++pos;
    return 0xFFFD;
}

}  // namespace ui
