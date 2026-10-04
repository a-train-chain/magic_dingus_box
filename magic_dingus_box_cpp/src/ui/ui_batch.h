#pragma once

// CPU-side vertex batch for the UI renderer (MDB_BATCH_UI). Pure logic, no
// GL — unit-tested on the Mac (tests/ui/test_ui_batch.cpp); the GL half
// (shader, VBO orphaning, flush) is Renderer::flush_ui_batch in
// renderer_batch.cpp.
//
// WHY: the immediate path issues one glBufferData + uniforms + draw call
// per quad (four per stroked rect), so a Media Browser grid costs hundreds
// of driver round trips a frame. The batch accumulates triangles with a
// PER-VERTEX color, all sampling one texture, and the renderer submits
// them in one upload + one draw when something forces a flush. Solids
// sample a white texel (a reserved white block in every glyph-atlas page,
// or a 1x1 white texture), so solids and text share one shader state and
// one batch whenever they share a texture.
//
// ORDER: geometry is only ever appended, and the renderer flushes before
// anything that is not batched (texture change, blend/viewport/framebuffer
// /program change, a direct GL draw, end of the drawing scope) — so the
// GPU sees exactly the immediate path's draw order, just in fewer calls.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct BatchColor {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

class UiBatch {
public:
    // x, y, u, v, r, g, b, a
    static constexpr int kFloatsPerVertex = 8;

    bool empty() const { return verts_.empty(); }
    uint32_t texture() const { return tex_; }
    std::size_t vertex_count() const { return verts_.size() / kFloatsPerVertex; }
    std::size_t byte_size() const { return verts_.size() * sizeof(float); }
    const float* data() const { return verts_.data(); }

    // True when geometry sampling `tex` cannot join the pending vertices —
    // the caller must flush first. An empty batch accepts any texture.
    bool needs_flush_for(uint32_t tex) const { return !verts_.empty() && tex != tex_; }

    // Discards pending geometry (after a flush, or when the GL context the
    // texture ids belong to is gone). Keeps capacity: allocation-free
    // after warm-up.
    void clear() {
        verts_.clear();
        tex_ = 0;
    }

    // Axis-aligned quad as two triangles, in the same vertex order the
    // immediate path's GL_TRIANGLE_STRIP (TL, TR, BL, BR) rasterizes:
    // (TL, TR, BL), (TR, BR, BL). UVs per corner: u0/v0 at x0/y0.
    void quad(uint32_t tex, float x0, float y0, float x1, float y1,
              float u0, float v0, float u1, float v1, const BatchColor& c) {
        begin(tex);
        vertex(x0, y0, u0, v0, c);
        vertex(x1, y0, u1, v0, c);
        vertex(x0, y1, u0, v1, c);
        vertex(x1, y0, u1, v0, c);
        vertex(x1, y1, u1, v1, c);
        vertex(x0, y1, u0, v1, c);
    }

    // A 4-vertex triangle strip (x, y, u, v per vertex — the immediate
    // path's vertex layout) as its two triangles.
    void strip4(uint32_t tex, const float* xyuv, const BatchColor& c) {
        begin(tex);
        const int order[6] = {0, 1, 2, 1, 3, 2};
        for (int i : order) vertex(xyuv[i * 4], xyuv[i * 4 + 1], xyuv[i * 4 + 2], xyuv[i * 4 + 3], c);
    }

    // `count` vertices of independent triangles (count % 3 == 0), x, y, u, v
    // each — a GL_TRIANGLES upload of the immediate path.
    void triangles(uint32_t tex, const float* xyuv, int count, const BatchColor& c) {
        begin(tex);
        for (int i = 0; i + 2 < count; i += 3) {
            for (int k = 0; k < 3; ++k) {
                const float* p = xyuv + (i + k) * 4;
                vertex(p[0], p[1], p[2], p[3], c);
            }
        }
    }

    // A GL_TRIANGLE_FAN of `count` vertices (x, y, u, v each) as
    // (v0, v[i], v[i+1]) triangles.
    void fan(uint32_t tex, const float* xyuv, int count, const BatchColor& c) {
        begin(tex);
        for (int i = 1; i + 1 < count; ++i) {
            for (int k : {0, i, i + 1}) {
                const float* p = xyuv + k * 4;
                vertex(p[0], p[1], p[2], p[3], c);
            }
        }
    }

private:
    void begin(uint32_t tex) {
        // Callers flush first (needs_flush_for); joining a batch with a
        // different texture would draw this geometry with the wrong one.
        if (verts_.empty()) tex_ = tex;
    }
    void vertex(float x, float y, float u, float v, const BatchColor& c) {
        verts_.insert(verts_.end(), {x, y, u, v, c.r, c.g, c.b, c.a});
    }

    std::vector<float> verts_;
    uint32_t tex_ = 0;
};

// MDB_BATCH_UI value -> enabled. Unset/empty/anything else = ON;
// "0", "off", "false", "no" (any case) = OFF (the immediate path).
inline bool ui_batch_enabled_from_env(const char* value) {
    if (value == nullptr) return true;
    std::string v(value);
    for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return !(v == "0" || v == "off" || v == "false" || v == "no");
}

}  // namespace ui
