// Strip/title fit harness for the Marquee header (Phase 2c-1).
//
// WHY THIS EXISTS: draw_screen_header right-aligns the 7-chip tab strip with
// NO overflow guard — if the strip grows past the title it silently draws over
// it. Re-run this before changing any strip label in
// chrome::marquee_tab_labels(), or the screen title in
// marquee_title_for_mode(). It reproduces FontManager::get_text_width exactly
// (per-glyph (int)(advance * stbtt_ScaleForPixelHeight(size))) and
// chrome::tab_strip_total_width, against the shipped ZenDots face and the
// shipped mb_chrome.cpp constants.
//
// BUILD + RUN (not a CMake target on purpose — zero build-time cost):
//   cp ../magic_dingus_box_cpp/src/ui/stb_truetype.h /tmp/ && \
//   clang++ -std=c++17 -O1 -I/tmp -o /tmp/measure_strip_fit measure_strip_fit.cpp && \
//   /tmp/measure_strip_fit ../magic_dingus_box_cpp/assets/fonts/ZenDots-Regular.ttf
//
// EXPECTED OUTPUT as of 2026-08-01 (both gaps MUST stay positive):
//   title=Marquee      strip_left=305 title_right=203 gap=+102
//   title=Marquee TV   strip_left=305 title_right=260 gap=+45
//
// For the record, the variants that do NOT fit and are therefore banned:
//   all three content labels suffixed " · TV" -> strip 1080, gap -63
//   all three content labels suffixed " TV"   -> strip 1044, gap -27
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static stbtt_fontinfo font;
static char32_t decode_utf8(const std::string& s, std::size_t& pos) {
    if (pos >= s.size()) return 0;
    unsigned char c = s[pos];
    if (c < 0x80) { pos += 1; return c; }
    if ((c & 0xE0) == 0xC0) { char32_t r = ((c & 0x1F) << 6) | (s[pos+1] & 0x3F); pos += 2; return r; }
    if ((c & 0xF0) == 0xE0) { char32_t r = ((c & 0x0F) << 12) | ((s[pos+1] & 0x3F) << 6) | (s[pos+2] & 0x3F); pos += 3; return r; }
    pos += 1; return 0;
}
// Mirrors FontManager::get_text_width: per-glyph (int)(advance * scale).
static int text_w(const std::string& t, int size) {
    const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size));
    int width = 0; std::size_t pos = 0;
    while (pos < t.size()) {
        char32_t c = decode_utf8(t, pos);
        if (!c) break;
        int aw, lsb;
        stbtt_GetCodepointHMetrics(&font, static_cast<int>(c), &aw, &lsb);
        width += static_cast<int>(aw * scale);
    }
    return width;
}
int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    stbtt_InitFont(&font, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0));
    const int kTabFontPx = 24, kTabHorizPad = 10, kTabGap = 16;
    const int kTitleFontPx = 32, kSafeInset = 60, kScreenW = 1280;
    const std::vector<std::string> labels = {"Popular", "Top Rated", "For You",
                                             "Search", "Library", "Queue", "Settings"};
    int strip_w = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        strip_w += text_w(labels[i], kTabFontPx) + 2 * kTabHorizPad;
        if (i + 1 < labels.size()) strip_w += kTabGap;
    }
    const int strip_left = kScreenW - kSafeInset - strip_w;
    for (const char* title : {"Marquee", "Marquee TV"}) {
        const int title_right = kSafeInset + text_w(title, kTitleFontPx);
        printf("title=%-12s strip_left=%d title_right=%d gap=%+d\n",
               title, strip_left, title_right, strip_left - title_right);
    }
    return 0;
}
