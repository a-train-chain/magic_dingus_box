#include "font_manager.h"

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
    
    // Create texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    // Convert to RGBA (use actual bitmap dimensions)
    // The bitmap from stb_truetype is grayscale alpha, so we use it as the alpha channel
    std::vector<uint8_t> rgba_data(bitmap_width * bitmap_height * 4);
    for (int i = 0; i < bitmap_width * bitmap_height; i++) {
        rgba_data[i * 4 + 0] = 255;  // R (white, will be tinted by color uniform)
        rgba_data[i * 4 + 1] = 255;  // G
        rgba_data[i * 4 + 2] = 255;  // B
        rgba_data[i * 4 + 3] = bitmap[i];  // A (from stb_truetype bitmap)
    }
    
    // Use GL_RGBA as internal format (OpenGL ES 3.0 compatible)
    // Note: GL_RGBA8 may not be available in all OpenGL ES implementations
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap_width, bitmap_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_data.data());
    
    // Use linear filtering for smooth text (not nearest which causes blockiness)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    stbtt_FreeBitmap(bitmap, nullptr);
    
    glyph.texture_id = texture;
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
    
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, const_cast<unsigned char*>(font_data_.data()), 
                        stbtt_GetFontOffsetForIndex(font_data_.data(), 0))) {
        return static_cast<int>(size * 0.8);
    }
    
    float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size));
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
    
    return static_cast<int>(ascent * scale);
}

int FontManager::get_text_width(const std::string& text) {
    int width = 0;
    for (char c : text) {
        Glyph g = get_glyph(static_cast<char32_t>(c));
        width += g.advance;
    }
    return width;
}

int FontManager::get_text_width(const std::string& text, int font_size) {
    int width = 0;
    for (char c : text) {
        Glyph g = get_glyph_at_size(static_cast<char32_t>(c), font_size);
        width += g.advance;
    }
    return width;
}

void FontManager::cleanup() {
    // Free all glyph textures from base cache
    for (auto& pair : glyph_cache_) {
        if (pair.second.texture_id != 0) {
            glDeleteTextures(1, &pair.second.texture_id);
        }
    }
    glyph_cache_.clear();
    
    // Free all glyph textures from per-size cache
    for (auto& pair : size_glyph_cache_) {
        if (pair.second.texture_id != 0) {
            glDeleteTextures(1, &pair.second.texture_id);
        }
    }
    size_glyph_cache_.clear();
    
    font_data_.clear();
}

} // namespace ui

