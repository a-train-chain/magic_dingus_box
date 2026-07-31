#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace ui {

struct Glyph {
    // The ATLAS PAGE texture this glyph lives in — shared by hundreds of
    // glyphs (pre-atlas, every glyph owned its own GL texture and the
    // renderer paid one texture bind + buffer upload + draw call PER
    // GLYPH PER FRAME).
    uint32_t texture_id;
    int width;
    int height;
    int bearing_x;
    int bearing_y;  // Distance from baseline to top of bitmap
    int advance;
    int yoff;  // Raw yoff from stb_truetype (offset from top of bitmap to baseline)
    // This glyph's rect within texture_id, in normalized UVs.
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
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
    
    // Reset textures only (for GL context change) - keeps font data for re-rasterization
    void reset_textures();

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

    // Memo for get_baseline_at_size(): baseline is a pure function of the
    // immutable font_data_ + size, but computing it re-parses the whole
    // font (stbtt_InitFont) — and the renderer asks per item per frame.
    // mutable because the getter is const. Cleared wherever font_data_ is
    // replaced/cleared.
    mutable std::unordered_map<int, int> baseline_cache_;

    // Memo for get_text_width(text, size): key is "<size>\x1F<text>".
    // Valid across reset_textures() (advances don't change); cleared with
    // baseline_cache_ where font_data_ changes. Capped — see the .cpp
    // comment about wrap-loop substring churn.
    static constexpr std::size_t kWidthCacheMaxEntries = 4096;
    std::unordered_map<std::string, int> width_cache_;
    
    // ── Glyph atlas ──────────────────────────────────────────────────
    // Shelf-packed 1024x1024 RGBA pages; new pages are appended when the
    // current one fills. Each glyph gets a 1px transparent border so
    // LINEAR sampling at sub-pixel positions can't bleed a neighbor in.
    // Pages are the ONLY glyph GL objects now — cleanup/reset_textures
    // delete pages, and glyphs re-rasterize into fresh pages on demand.
    struct AtlasPage {
        uint32_t texture = 0;
        int shelf_x = 0;   // next free x on the current shelf
        int shelf_y = 0;   // top of the current shelf
        int shelf_h = 0;   // height of the current shelf
    };
    static constexpr int kAtlasSize = 1024;
    static constexpr int kGlyphPad = 1;
    std::vector<AtlasPage> atlas_pages_;
    // Allocate a w×h glyph rect (padding handled internally). On success
    // returns the page index and the top-left of the USABLE rect.
    bool atlas_alloc(int w, int h, int& page_idx, int& x, int& y);
    void destroy_atlas_pages();

    // Rasterize a glyph using stb_truetype (at base font size)
    Glyph rasterize_glyph(char32_t codepoint);
    
    // Rasterize a glyph at a specific size
    Glyph rasterize_glyph_at_size(char32_t codepoint, int size);
    
    // Load TTF file
    bool load_ttf_file(const std::string& path);
};

} // namespace ui

