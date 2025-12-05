#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace ui {

struct Glyph {
    uint32_t texture_id;
    int width;
    int height;
    int bearing_x;
    int bearing_y;  // Distance from baseline to top of bitmap
    int advance;
    int yoff;  // Raw yoff from stb_truetype (offset from top of bitmap to baseline)
};

class FontManager {
public:
    FontManager();
    ~FontManager();

    // Load font from TTF file (base size, can be scaled per glyph)
    bool load_font(const std::string& path, int size);
    
    // Get glyph for a character at a specific size (scales from base font)
    Glyph get_glyph_at_size(char32_t codepoint, int size);
    
    // Get glyph for a character (rasterizes if not cached)
    Glyph get_glyph(char32_t codepoint);
    
    // Get text width in pixels (at base font size)
    int get_text_width(const std::string& text);
    
    // Get text width in pixels at a specific font size
    int get_text_width(const std::string& text, int font_size);
    
    // Get line height
    int get_line_height() const { return line_height_; }
    
    // Get baseline offset (distance from top of line to baseline) for base font size
    int get_baseline() const { return baseline_; }
    
    // Get baseline offset for a specific font size
    int get_baseline_at_size(int size) const;
    
    // Get font ascent (distance from baseline to top of tallest glyph)
    int get_ascent() const { return ascent_; }
    
    // Cleanup
    void cleanup();

private:
    std::string font_path_;
    int font_size_;
    int line_height_;
    int baseline_;  // Distance from top of line to baseline
    int ascent_;    // Font ascent (distance from baseline to top)
    
    // Glyph cache (keyed by codepoint for base font size)
    std::unordered_map<char32_t, Glyph> glyph_cache_;
    
    // Per-size glyph cache (keyed by codepoint + size)
    // This prevents creating duplicate textures for the same glyph at different sizes
    struct GlyphCacheKey {
        char32_t codepoint;
        int size;
        
        bool operator==(const GlyphCacheKey& other) const {
            return codepoint == other.codepoint && size == other.size;
        }
    };
    
    struct GlyphCacheKeyHash {
        std::size_t operator()(const GlyphCacheKey& key) const {
            return std::hash<char32_t>()(key.codepoint) ^ (std::hash<int>()(key.size) << 1);
        }
    };
    
    std::unordered_map<GlyphCacheKey, Glyph, GlyphCacheKeyHash> size_glyph_cache_;
    
    // Font data (loaded from TTF)
    std::vector<uint8_t> font_data_;
    
    // Rasterize a glyph using stb_truetype (at base font size)
    Glyph rasterize_glyph(char32_t codepoint);
    
    // Rasterize a glyph at a specific size
    Glyph rasterize_glyph_at_size(char32_t codepoint, int size);
    
    // Load TTF file
    bool load_ttf_file(const std::string& path);
};

} // namespace ui

