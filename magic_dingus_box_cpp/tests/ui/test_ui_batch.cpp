// ui::UiBatch — the CPU half of UI draw-call batching (MDB_BATCH_UI).
//
// What must hold for the batched picture to equal the immediate one:
//   - every primitive becomes the same triangles the immediate path's
//     strip / fan / triangle list rasterizes, with its color on every
//     vertex;
//   - geometry is appended in call order (draw order IS the picture with
//     alpha blending);
//   - a batch never mixes textures — needs_flush_for() says when the
//     renderer must submit before switching.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "ui/ui_batch.h"

using ui::BatchColor;
using ui::UiBatch;

namespace {

struct V {
    float x, y, u, v, r, g, b, a;
};

std::vector<V> verts(const UiBatch& b) {
    std::vector<V> out;
    const float* d = b.data();
    for (std::size_t i = 0; i < b.vertex_count(); ++i) {
        const float* p = d + i * UiBatch::kFloatsPerVertex;
        out.push_back({p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]});
    }
    return out;
}

const BatchColor kRed{1.0f, 0.0f, 0.0f, 0.5f};
const BatchColor kBlue{0.0f, 0.0f, 1.0f, 1.0f};

}  // namespace

TEST_CASE("UiBatch: quad is the strip's two triangles with per-vertex color",
          "[ui_batch]") {
    UiBatch b;
    b.quad(7, 10, 20, 30, 60, 0.1f, 0.2f, 0.3f, 0.4f, kRed);
    REQUIRE(b.vertex_count() == 6);
    CHECK(b.texture() == 7u);
    CHECK(b.byte_size() == 6 * 8 * sizeof(float));
    const auto v = verts(b);
    // (TL, TR, BL), (TR, BR, BL) — what GL_TRIANGLE_STRIP TL,TR,BL,BR covers.
    const float expect[6][4] = {
        {10, 20, 0.1f, 0.2f}, {30, 20, 0.3f, 0.2f}, {10, 60, 0.1f, 0.4f},
        {30, 20, 0.3f, 0.2f}, {30, 60, 0.3f, 0.4f}, {10, 60, 0.1f, 0.4f},
    };
    for (int i = 0; i < 6; ++i) {
        CHECK(v[i].x == expect[i][0]);
        CHECK(v[i].y == expect[i][1]);
        CHECK(v[i].u == expect[i][2]);
        CHECK(v[i].v == expect[i][3]);
        CHECK(v[i].r == 1.0f);
        CHECK(v[i].g == 0.0f);
        CHECK(v[i].b == 0.0f);
        CHECK(v[i].a == 0.5f);
    }
}

TEST_CASE("UiBatch: strip4 / fan / triangles match their GL primitives",
          "[ui_batch]") {
    UiBatch b;
    const float strip[16] = {0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1};
    b.strip4(1, strip, kBlue);
    REQUIRE(b.vertex_count() == 6);
    auto v = verts(b);
    // Strip triangles (0,1,2) and (1,3,2).
    CHECK((v[0].x == 0 && v[0].y == 0));
    CHECK((v[1].x == 1 && v[1].y == 0));
    CHECK((v[2].x == 0 && v[2].y == 1));
    CHECK((v[3].x == 1 && v[3].y == 0));
    CHECK((v[4].x == 1 && v[4].y == 1));
    CHECK((v[5].x == 0 && v[5].y == 1));

    b.clear();
    // Fan of 5 vertices = 3 triangles, each (center, i, i+1).
    const float fan[20] = {5, 5, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 10, 10, 0, 0, 0, 10, 0, 0};
    b.fan(1, fan, 5, kBlue);
    REQUIRE(b.vertex_count() == 9);
    v = verts(b);
    for (int t = 0; t < 3; ++t) {
        CHECK((v[t * 3].x == 5 && v[t * 3].y == 5));
        CHECK(v[t * 3 + 1].x == fan[(t + 1) * 4]);
        CHECK(v[t * 3 + 2].x == fan[(t + 2) * 4]);
    }

    b.clear();
    const float tris[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0,
                            2, 2, 0, 0, 3, 2, 0, 0, 2, 3, 0, 0};
    b.triangles(1, tris, 6, kBlue);
    REQUIRE(b.vertex_count() == 6);
    v = verts(b);
    for (int i = 0; i < 6; ++i) {
        CHECK(v[i].x == tris[i * 4]);
        CHECK(v[i].y == tris[i * 4 + 1]);
    }
}

TEST_CASE("UiBatch: geometry is appended in call order", "[ui_batch]") {
    UiBatch b;
    b.quad(3, 0, 0, 1, 1, 0, 0, 0, 0, kRed);
    b.quad(3, 5, 5, 6, 6, 0, 0, 0, 0, kBlue);
    const auto v = verts(b);
    REQUIRE(v.size() == 12);
    CHECK(v[0].r == 1.0f);   // first quad first
    CHECK(v[0].x == 0.0f);
    CHECK(v[6].b == 1.0f);   // second quad after it
    CHECK(v[6].x == 5.0f);
}

TEST_CASE("UiBatch: a batch never mixes textures", "[ui_batch]") {
    UiBatch b;
    CHECK_FALSE(b.needs_flush_for(9));  // empty accepts anything
    b.quad(9, 0, 0, 1, 1, 0, 0, 1, 1, kRed);
    CHECK_FALSE(b.needs_flush_for(9));
    CHECK(b.needs_flush_for(4));
    b.clear();
    CHECK(b.empty());
    CHECK(b.texture() == 0u);
    CHECK_FALSE(b.needs_flush_for(4));
    b.quad(4, 0, 0, 1, 1, 0, 0, 1, 1, kRed);
    CHECK(b.texture() == 4u);
}

TEST_CASE("ui_batch_enabled_from_env: default ON, explicit off values",
          "[ui_batch]") {
    CHECK(ui::ui_batch_enabled_from_env(nullptr));
    CHECK(ui::ui_batch_enabled_from_env(""));
    CHECK(ui::ui_batch_enabled_from_env("1"));
    CHECK_FALSE(ui::ui_batch_enabled_from_env("0"));
    CHECK_FALSE(ui::ui_batch_enabled_from_env("OFF"));
    CHECK_FALSE(ui::ui_batch_enabled_from_env("false"));
    CHECK_FALSE(ui::ui_batch_enabled_from_env("no"));
}
