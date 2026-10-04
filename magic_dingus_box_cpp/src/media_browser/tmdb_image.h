#pragma once

// TMDB image-size helpers. Header-only and dependency-free so the data
// layer (Radarr/Sonarr parsers), the kiosk-only screens and the Mac unit
// tests can all share one definition without a CMake source-list entry.
//
// Why sizes matter: every poster the artwork cache holds is decoded to
// RGBA8 and uploaded with a full mipmap chain (ArtworkCache::texture_bytes
// — base level x ~4/3). On the Pi that is unswappable system RAM.
//
//   size   pixels     GPU bytes (RGBA8 + mips)
//   w185   185x278    ~274 KB
//   w342   342x513    ~936 KB
//   w500   500x750    ~2.0 MB
//
// The Media Browser lays out on a 1280x720 LOGICAL canvas at every output
// mode (main.cpp: the MB canvas is the renderer's logical size, not the
// HDMI mode), and the largest output is 1920x1080 — a 1.5x upscale. The
// 9-column grids (Browse / Search / Library / playback-overlay rail) are
// (1280 - 2*60 - 8*8) / 9 = 121 logical px wide -> 182 physical px at
// 1080p. w185 covers that 1:1, so grid cards fetch w185 and only the
// hero posters (Detail 280 px, SeriesDetail 160 px, overlay 200 px) keep
// the w500 the data layer emits. ~7x less texture memory per grid card.
//
// The artwork cache (GPU and on-disk) is keyed by the full URL, so the
// w185 and w500 variants of one poster are independent entries that
// coexist; nothing else has to know about sizes.

#include <string>
#include <string_view>

namespace media_browser {

// The size the data layer emits (tmdb_client kImageBase, Radarr/Sonarr
// normalization) and hero posters draw at.
inline constexpr std::string_view kTmdbHeroPosterSize = "w500";
// Grid / rail / queue-row thumbnail size. See the table above.
inline constexpr std::string_view kTmdbGridPosterSize = "w185";
// Widest LOGICAL card that w185 still covers 1:1 at the 1.5x (1080p)
// output scale: floor(185 / 1.5). Wider cards keep the URL as given.
inline constexpr int kTmdbGridPosterMaxLogicalW = 123;

// True for http(s)://image.tmdb.org/t/p/<size>/<file> — the ONLY URLs
// whose size segment we may rewrite. Radarr/Sonarr also serve TVDB and
// fanart.tv artwork; a "/t/p/" substring on another host means nothing.
inline bool is_tmdb_image_url(std::string_view url) {
    constexpr std::string_view kHttps = "https://image.tmdb.org/t/p/";
    constexpr std::string_view kHttp  = "http://image.tmdb.org/t/p/";
    std::string_view prefix;
    if (url.substr(0, kHttps.size()) == kHttps) {
        prefix = kHttps;
    } else if (url.substr(0, kHttp.size()) == kHttp) {
        prefix = kHttp;
    } else {
        return false;
    }
    // Need a non-empty size segment followed by '/' and a non-empty file.
    const auto size_end = url.find('/', prefix.size());
    return size_end != std::string_view::npos && size_end > prefix.size() &&
           size_end + 1 < url.size();
}

// Rewrite the size segment of a TMDB image URL ("original", "w500",
// "w780", ...) to `size`. Anything that is not an image.tmdb.org URL —
// TVDB, fanart.tv, empty, malformed — is returned unchanged.
inline std::string tmdb_image_url_with_size(const std::string& url,
                                            std::string_view size) {
    if (!is_tmdb_image_url(url)) return url;
    const std::size_t size_start = url.find("/t/p/") + 5;
    const std::size_t size_end = url.find('/', size_start);
    if (std::string_view(url).substr(size_start, size_end - size_start) == size) {
        return url;  // already that size — no rebuild
    }
    std::string out;
    out.reserve(url.size() - (size_end - size_start) + size.size());
    out.append(url, 0, size_start);
    out.append(size);
    out.append(url, size_end, std::string::npos);
    return out;
}

// The URL a poster card `logical_w` logical px wide should fetch: the
// grid size when the card is grid-sized, otherwise the URL as given
// (hero posters keep their w500).
inline std::string tmdb_poster_url_for_card(const std::string& url,
                                            int logical_w) {
    if (logical_w > kTmdbGridPosterMaxLogicalW) return url;
    return tmdb_image_url_with_size(url, kTmdbGridPosterSize);
}

}  // namespace media_browser
