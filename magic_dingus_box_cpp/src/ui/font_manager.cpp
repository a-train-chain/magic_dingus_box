#include "font_manager.h"
#include "text_utf8.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <fstream>
#include <iostream>
#include <vector>
#include <GLES3/gl3.h>
#include <cstring>

namespace ui {

FontManager::FontManager()
    : font_size_(0)
    , line_height_(0)
    , baseline_(0)
    , ascent_(0)
{
}

FontManager::~FontManager() {
    cleanup();
}

bool FontManager::load_font(const std::string& path, int size) {
    if (!load_ttf_file(path)) {
        return false;
    }

    font_path_ = path;
    font_size_ = size;
    baseline_cache_.clear();  // font_data_ just changed; memos are stale
    width_cache_.clear();
    
    // Calculate proper line height and baseline from font metrics
    if (!font_data_.empty()) {
        stbtt_fontinfo font;
        if (stbtt_InitFont(&font, font_data_.data(), stbtt_GetFontOffsetForIndex(font_data_.data(), 0))) {
            float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(font_size_));
            int ascent, descent, line_gap;
            stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
            
            // Store ascent for glyph positioning
            ascent_ = static_cast<int>(ascent * scale);
            // Baseline is the distance from top of line to baseline (ascent * scale)
            baseline_ = ascent_;
            // Line height is ascent + descent + line_gap
            line_height_ = static_cast<int>((ascent - descent + line_gap) * scale);
        } else {
            // Fallback
            baseline_ = static_cast<int>(font_size_ * 0.8);
            line_height_ = static_cast<int>(font_size_ * 1.2);
        }
    } else {
        baseline_ = static_cast<int>(font_size_ * 0.8);
        line_height_ = static_cast<int>(font_size_ * 1.2);
    }
    
    return true;
}

bool FontManager::load_ttf_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        // Don't print error for every failed path - only if all fail
        return false;
    }
    
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    font_data_.resize(size);
    file.read(reinterpret_cast<char*>(font_data_.data()), size);
    
    return true;
}

Glyph FontManager::get_glyph(char32_t codepoint) {
    // Check cache
    auto it = glyph_cache_.find(codepoint);
    if (it != glyph_cache_.end()) {
        return it->second;
    }
    
    // Rasterize and cache
    Glyph glyph = rasterize_glyph(codepoint);
    glyph_cache_[codepoint] = glyph;
    
    return glyph;
}

Glyph FontManager::get_glyph_at_size(char32_t codepoint, int size) {
    // If size matches base size, use cached glyph
    if (size == font_size_) {
        return get_glyph(codepoint);
    }
    
    // Check per-size cache
    GlyphCacheKey key{codepoint, size};
    auto it = size_glyph_cache_.find(key);
    if (it != size_glyph_cache_.end()) {
        return it->second;
    }
    
    // Rasterize at the requested size and cache it
    Glyph glyph = rasterize_glyph_at_size(codepoint, size);
    size_glyph_cache_[key] = glyph;
    
    return glyph;
}

Glyph FontManager::rasterize_glyph(char32_t codepoint) {
    return rasterize_glyph_at_size(codepoint, font_size_);
}

bool FontManager::atlas_alloc(int w, int h, int& page_idx, int& x, int& y) {
    const int pw = w + 2 * kGlyphPad;
    const int ph = h + 2 * kGlyphPad;
    if (pw > kAtlasSize || ph > kAtlasSize) {
        // A glyph bigger than a whole page can't exist at kiosk font
        // sizes; refuse rather than corrupt the shelf state.
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!atlas_pages_.empty()) {
            AtlasPage& p = atlas_pages_.back();
            if (p.shelf_x + pw > kAtlasSize) {
                // Shelf full — open a new one below.
                p.shelf_y += p.shelf_h;
                p.shelf_x = 0;
                p.shelf_h = 0;
            }
            if (p.shelf_y + ph <= kAtlasSize) {
                x = p.shelf_x + kGlyphPad;
                y = p.shelf_y + kGlyphPad;
                p.shelf_x += pw;
                if (ph > p.shelf_h) p.shelf_h = ph;
                page_idx = static_cast<int>(atlas_pages_.size()) - 1;
                return true;
            }
        }
        // No page yet, or the last page is out of vertical space: append
        // a fresh page (zero-initialized so glyph padding samples
        // transparent) and retry once.
        AtlasPage page;
        glGenTextures(1, &page.texture);
        glBindTexture(GL_TEXTURE_2D, page.texture);
        std::vector<uint8_t> zeros(
            static_cast<size_t>(kAtlasSize) * kAtlasSize * 4, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAtlasSize, kAtlasSize, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        atlas_pages_.push_back(page);
    }
    return false;  // unreachable in practice
}

void FontManager::destroy_atlas_pages() {
    for (auto& p : atlas_pages_) {
        if (p.texture != 0) {
            glDeleteTextures(1, &p.texture);
        }
    }
    atlas_pages_.clear();
}

Glyph FontManager::rasterize_glyph_at_size(char32_t codepoint, int size) {
    Glyph glyph = {};
    
    if (font_data_.empty()) {
        return glyph;
    }
    
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, font_data_.data(), stbtt_GetFontOffsetForIndex(font_data_.data(), 0))) {
        std::cerr << "Failed to initialize font" << std::endl;
        return glyph;
    }
    
    float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size));
    
    // Get horizontal metrics first (needed for both visible and invisible glyphs)
    int advance_width, left_side_bearing;
    stbtt_GetCodepointHMetrics(&font, codepoint, &advance_width, &left_side_bearing);
    
    // Get bounding box relative to baseline (more accurate than bitmap yoff)
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font, codepoint, scale, scale, &x0, &y0, &x1, &y1);
    
    // Calculate bitmap dimensions
    int width = x1 - x0;
    int height = y1 - y0;
    
    // For space characters or invisible glyphs, we still need the advance width
    if (width == 0 || height == 0) {
        glyph.advance = static_cast<int>(advance_width * scale);
        glyph.bearing_x = 0;
        glyph.bearing_y = 0;
        glyph.yoff = 0;
        glyph.width = 0;
        glyph.height = 0;
        glyph.texture_id = 0;  // No texture for spaces
        
        return glyph;
    }
    
    // Get the bitmap (xoff and yoff are offsets from origin to bitmap top-left)
    int xoff, yoff;
    int bitmap_width, bitmap_height;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&font, scale, scale, codepoint, &bitmap_width, &bitmap_height, &xoff, &yoff);
    
    if (!bitmap) {
        glyph.advance = static_cast<int>(advance_width * scale);
        glyph.bearing_x = 0;
        glyph.bearing_y = 0;
        glyph.yoff = 0;
        glyph.width = 0;
        glyph.height = 0;
        glyph.texture_id = 0;
        return glyph;
    }
    
    // Get vertical metrics for reference
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
    
    // Upload into the shared glyph atlas. Same rasterization, same RGBA
    // conversion (grayscale alpha → white RGB + alpha, tinted by the
    // color uniform at draw time) — only the destination changed from a
    // per-glyph texture to a region of a shared page.
    int page_idx = 0, ax = 0, ay = 0;
    if (!atlas_alloc(bitmap_width, bitmap_height, page_idx, ax, ay)) {
        std::cerr << "FontManager: glyph " << static_cast<uint32_t>(codepoint)
                  << " (" << bitmap_width << "x" << bitmap_height
                  << ") exceeds atlas page — rendered as blank" << std::endl;
        stbtt_FreeBitmap(bitmap, nullptr);
        glyph.advance = static_cast<int>(advance_width * scale);
        return glyph;
    }

    // Padded upload: the glyph sits inset by kGlyphPad inside a
    // zero-initialized border so LINEAR sampling at sub-pixel positions
    // blends toward transparent, never toward a neighboring glyph.
    const int pw = bitmap_width + 2 * kGlyphPad;
    const int ph = bitmap_height + 2 * kGlyphPad;
    std::vector<uint8_t> rgba_data(static_cast<size_t>(pw) * ph * 4, 0);
    for (int row = 0; row < bitmap_height; row++) {
        for (int col = 0; col < bitmap_width; col++) {
            uint8_t* px = &rgba_data[
                ((static_cast<size_t>(row) + kGlyphPad) * pw +
                 (col + kGlyphPad)) * 4];
            px[0] = 255;  // R (white, tinted by color uniform)
            px[1] = 255;  // G
            px[2] = 255;  // B
            px[3] = bitmap[row * bitmap_width + col];  // A
        }
    }
    const AtlasPage& page = atlas_pages_[static_cast<size_t>(page_idx)];
    glBindTexture(GL_TEXTURE_2D, page.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, ax - kGlyphPad, ay - kGlyphPad,
                    pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, rgba_data.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbtt_FreeBitmap(bitmap, nullptr);

    glyph.texture_id = page.texture;
    glyph.u0 = static_cast<float>(ax) / kAtlasSize;
    glyph.v0 = static_cast<float>(ay) / kAtlasSize;
    glyph.u1 = static_cast<float>(ax + bitmap_width) / kAtlasSize;
    glyph.v1 = static_cast<float>(ay + bitmap_height) / kAtlasSize;
    glyph.width = bitmap_width;
    glyph.height = bitmap_height;
    glyph.bearing_x = xoff;
    // y0 from GetCodepointBitmapBox is the top of the bounding box relative to baseline
    // In stb_truetype coordinates (Y-up), y0 is negative (above baseline)
    // bearing_y is the distance from baseline to top of bitmap (positive = above baseline)
    glyph.bearing_y = -y0;  // Convert to positive distance
    // yoff from GetCodepointBitmap is the offset from bitmap origin to baseline
    // We'll use y0 from the bounding box instead for more accurate positioning
    glyph.yoff = -y0;  // Distance from baseline to top of bitmap
    glyph.advance = static_cast<int>(advance_width * scale);
    
    return glyph;
}

int FontManager::get_baseline_at_size(int size) const {
    if (font_data_.empty()) {
        return static_cast<int>(size * 0.8);
    }

    // Memoized: this is a pure function of the immutable font_data_ and
    // size, but stbtt_InitFont re-parses the whole TrueType table
    // directory on every call — and renderer.cpp calls this from
    // per-item render loops every frame (~600 full font re-parses/sec on
    // the playlist menu, for a value that only ever takes a couple of
    // distinct sizes).
    auto it = baseline_cache_.find(size);
    if (it != baseline_cache_.end()) {
        return it->second;
    }

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, const_cast<unsigned char*>(font_data_.data()),
                        stbtt_GetFontOffsetForIndex(font_data_.data(), 0))) {
        return static_cast<int>(size * 0.8);
    }

    float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size));
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);

    const int baseline = static_cast<int>(ascent * scale);
    baseline_cache_.emplace(size, baseline);
    return baseline;
}

int FontManager::get_text_width(const std::string& text) {
    int width = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        char32_t c = ::ui::decode_utf8(text, pos);
        if (c == 0) break;
        Glyph g = get_glyph(c);
        width += g.advance;
    }
    return width;
}

int FontManager::get_text_width(const std::string& text, int font_size) {
    // Memoized: hot render paths measure the same strings every frame —
    // MB header tabs (~18 measures/frame), footer hints, poster-card
    // years, and the grid title fit/wrap checks. Widths are a pure
    // function of font_data_ + size (glyph ADVANCES survive
    // reset_textures(); only the GL textures are recreated), so the memo
    // is invalidated only where font_data_ changes (load_font/cleanup).
    // Size-capped: grid wrap loops measure substring candidates, which
    // would otherwise grow the map without bound — on overflow just
    // clear; the hot entries repopulate within a frame.
    std::string key;
    key.reserve(text.size() + 8);
    key.append(std::to_string(font_size)).push_back('\x1F');
    key.append(text);
    auto it = width_cache_.find(key);
    if (it != width_cache_.end()) return it->second;

    int width = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        char32_t c = ::ui::decode_utf8(text, pos);
        if (c == 0) break;
        Glyph g = get_glyph_at_size(c, font_size);
        width += g.advance;
    }

    if (width_cache_.size() >= kWidthCacheMaxEntries) width_cache_.clear();
    width_cache_.emplace(std::move(key), width);
    return width;
}

void FontManager::cleanup() {
    // Glyphs no longer own textures — the atlas pages are the only glyph
    // GL objects.
    destroy_atlas_pages();
    glyph_cache_.clear();
    size_glyph_cache_.clear();

    font_data_.clear();
    baseline_cache_.clear();
    width_cache_.clear();
}

void FontManager::reset_textures() {
    // Clear GL textures but KEEP font_data_ so glyphs can be re-rasterized
    // This is used when EGL context is restored after external app (RetroArch)
    destroy_atlas_pages();
    glyph_cache_.clear();
    size_glyph_cache_.clear();

    // NOTE: font_data_ is NOT cleared - glyphs will be re-rasterized on demand
}
} // namespace ui

