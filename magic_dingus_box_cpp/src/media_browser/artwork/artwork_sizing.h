#pragma once

// Host-independent sizing policy for artwork textures. Header-only and
// dependency-free (no GL, no stb) so the Mac unit tests exercise exactly
// what the kiosk runs.
//
// Why this exists: tmdb_poster_url_for_card() shrinks only image.tmdb.org
// URLs (it rewrites the size segment). Sonarr's posters are usually TVDB
// (artworks.thetvdb.com, ~680x1000) and pass through at full size —
// ~3.6 MB of RGBA8 + mipmaps EACH. On a Pi 4B's 64 MB budget that is ~17
// posters, fewer than one 18-card Library page, so a TV-heavy page
// uploaded and evicted posters every frame (flickering back to tint).
//
// The fix belongs to the artwork pipeline, not to any one host: a texture
// requested for a GRID/CARD slot is downscaled after DECODE (on the
// fetcher thread) to at most kCardMaxPixelW x kCardMaxPixelH, keeping
// aspect. Hero / Detail requests keep the decoded size. The two variants
// are separate cache entries (artwork_cache_key) sharing one on-disk JPEG.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "media_browser/tmdb_image.h"

namespace media_browser {

enum class ArtworkVariant {
    Full,  // hero / Detail / overlay: decoded size, untouched
    Card,  // grid card / rail / queue thumbnail: capped at the card box
};

// The card box: 2x the widest grid card at the largest (1080p, 1.5x)
// output scale. kTmdbGridPosterMaxLogicalW (123 logical) * 1.5 = 185 px,
// x2 = 370; height allows a 2:3 poster at that width. A 680x1000 TVDB
// poster lands at 370x544: ~1.07 MB with mips instead of ~3.6 MB, so an
// 18-card page of them is ~19 MB against the Pi 4B's 64 MB budget.
inline constexpr int kCardMaxPixelW = 370;
inline constexpr int kCardMaxPixelH = 555;

// Which variant a poster slot `logical_w` logical px wide should request.
// Same threshold as the TMDB w185 rewrite: anything a w185 covers 1:1 at
// 1080p is a card; wider slots (Detail 280, overlay 200, SeriesDetail
// 160) are heroes.
inline ArtworkVariant artwork_variant_for_slot(float logical_w) {
    return logical_w <= static_cast<float>(kTmdbGridPosterMaxLogicalW)
               ? ArtworkVariant::Card
               : ArtworkVariant::Full;
}

// Cache key for (url, variant). Full keeps the bare URL (so existing
// entries and every Full caller are unchanged); Card appends a suffix
// containing a space, which cannot occur in a valid URL, so the two
// variants never collide.
inline std::string artwork_cache_key(const std::string& url,
                                     ArtworkVariant variant) {
    if (variant == ArtworkVariant::Full) return url;
    return url + " #card";
}

struct PixelSize {
    int w = 0;
    int h = 0;
};

// Largest size <= (max_w, max_h) with the source's aspect ratio. Never
// upscales; a non-positive max means "no limit" on that axis; each output
// axis is at least 1 px for a non-empty source.
inline PixelSize fit_within(int src_w, int src_h, int max_w, int max_h) {
    if (src_w <= 0 || src_h <= 0) return {0, 0};
    double scale = 1.0;
    if (max_w > 0 && src_w > max_w) {
        scale = std::min(scale, static_cast<double>(max_w) / src_w);
    }
    if (max_h > 0 && src_h > max_h) {
        scale = std::min(scale, static_cast<double>(max_h) / src_h);
    }
    if (scale >= 1.0) return {src_w, src_h};
    const int w = std::max(1, static_cast<int>(src_w * scale + 0.5));
    const int h = std::max(1, static_cast<int>(src_h * scale + 0.5));
    // Rounding must never push an axis past its cap.
    return {max_w > 0 ? std::min(w, max_w) : w,
            max_h > 0 ? std::min(h, max_h) : h};
}

// The decoded size a texture for `variant` should be uploaded at.
inline PixelSize target_size_for_variant(int src_w, int src_h,
                                         ArtworkVariant variant) {
    if (variant == ArtworkVariant::Full) {
        return src_w > 0 && src_h > 0 ? PixelSize{src_w, src_h}
                                      : PixelSize{0, 0};
    }
    return fit_within(src_w, src_h, kCardMaxPixelW, kCardMaxPixelH);
}

// Area-average (box-filter) downscale of tightly packed RGBA8 pixels.
// Each destination pixel is the coverage-weighted mean of the source
// pixels under it, so stripes/text alias far less than nearest-neighbour.
// Downscale only: a destination axis larger than the source's is clamped
// to the source. Returns an empty vector on invalid input.
inline std::vector<std::uint8_t> downscale_rgba_area(
        const std::uint8_t* src, int sw, int sh, int dw, int dh) {
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return {};
    dw = std::min(dw, sw);
    dh = std::min(dh, sh);
    const std::size_t sws = static_cast<std::size_t>(sw);
    if (dw == sw && dh == sh) {
        return std::vector<std::uint8_t>(src, src + sws * sh * 4u);
    }

    // Per-axis contribution lists: for destination index d, the source
    // indices it covers and each one's coverage weight (sum = 1).
    struct Tap { int index; float weight; };
    auto build_taps = [](int src_n, int dst_n) {
        std::vector<std::vector<Tap>> taps(static_cast<std::size_t>(dst_n));
        const double ratio = static_cast<double>(src_n) / dst_n;
        for (int d = 0; d < dst_n; ++d) {
            const double begin = d * ratio;
            const double end = std::min<double>(src_n, (d + 1) * ratio);
            const int first = static_cast<int>(begin);
            const int last = std::min(src_n - 1, static_cast<int>(end - 1e-9));
            for (int s = first; s <= last; ++s) {
                const double lo = std::max<double>(begin, s);
                const double hi = std::min<double>(end, s + 1.0);
                if (hi > lo) {
                    taps[d].push_back(
                        {s, static_cast<float>((hi - lo) / ratio)});
                }
            }
        }
        return taps;
    };
    const auto xtaps = build_taps(sw, dw);
    const auto ytaps = build_taps(sh, dh);

    // Horizontal pass: sw x sh -> dw x sh (float accumulators).
    const std::size_t dws = static_cast<std::size_t>(dw);
    std::vector<float> tmp(dws * static_cast<std::size_t>(sh) * 4u);
    for (int y = 0; y < sh; ++y) {
        const std::uint8_t* row = src + static_cast<std::size_t>(y) * sws * 4u;
        float* out = tmp.data() + static_cast<std::size_t>(y) * dws * 4u;
        for (int dx = 0; dx < dw; ++dx) {
            float acc[4] = {0, 0, 0, 0};
            for (const Tap& t : xtaps[dx]) {
                const std::uint8_t* p = row + static_cast<std::size_t>(t.index) * 4u;
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * t.weight;
            }
            for (int c = 0; c < 4; ++c) out[dx * 4 + c] = acc[c];
        }
    }

    // Vertical pass: dw x sh -> dw x dh, rounded back to bytes.
    std::vector<std::uint8_t> dst(dws * static_cast<std::size_t>(dh) * 4u);
    for (int dy = 0; dy < dh; ++dy) {
        std::uint8_t* out = dst.data() + static_cast<std::size_t>(dy) * dws * 4u;
        for (int dx = 0; dx < dw; ++dx) {
            float acc[4] = {0, 0, 0, 0};
            for (const Tap& t : ytaps[dy]) {
                const float* p = tmp.data() +
                    (static_cast<std::size_t>(t.index) * dws + dx) * 4u;
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * t.weight;
            }
            for (int c = 0; c < 4; ++c) {
                const float v = std::min(255.0f, std::max(0.0f, acc[c] + 0.5f));
                out[dx * 4 + c] = static_cast<std::uint8_t>(v);
            }
        }
    }
    return dst;
}

// Eviction protection window: an entry last drawn in drawn-frame
// `last_drawn` is protected while `current_frame` is that frame or the
// next one. Frame tracking is unarmed until the first begin_frame()
// (current_frame == 0), and 0 is also "never drawn", so neither state
// protects anything.
inline bool artwork_drawn_recently(std::uint64_t last_drawn,
                                   std::uint64_t current_frame) {
    if (current_frame == 0 || last_drawn == 0) return false;
    return last_drawn + 1 >= current_frame;
}

}  // namespace media_browser
