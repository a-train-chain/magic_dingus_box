#pragma once
#include <cstdint>
#include <string>

namespace ui {

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
