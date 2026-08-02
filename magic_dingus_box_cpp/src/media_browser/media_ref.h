#pragma once

#include <cstddef>
#include <functional>

namespace media_browser {

// Which TMDB namespace a row came from. Defaults to Movie on every hit so
// the entire pre-TV movie path (Browse, Search, For You, playback overlay)
// is untouched by the TV work — a TV row is only ever produced by the
// parse_tv_* family, which sets this explicitly.
enum class MediaKind { Movie, Tv };

// *** TMDB's movie and TV id spaces OVERLAP COMPLETELY. *** tmdb_id 1396 is
// Breaking Bad (TV) AND an unrelated movie — the two id spaces are
// independently assigned, so a bare `int tmdb_id` is NOT a unique key across
// kinds. MediaRef is the safe key: every set/map that can ever see both
// kinds (in-library "owned" checks, page-dedupe, recommendation merge and
// exclude lists) is keyed on this, not on a bare id.
struct MediaRef {
    MediaKind kind = MediaKind::Movie;
    int id = 0;
};

inline bool operator==(const MediaRef& a, const MediaRef& b) {
    return a.kind == b.kind && a.id == b.id;
}
inline bool operator!=(const MediaRef& a, const MediaRef& b) { return !(a == b); }

}  // namespace media_browser

namespace std {

// Injective for every real TMDB id: the id occupies bits 1..32 and the kind
// bit 0, so (Movie, n) and (Tv, n) can never share a bucket-and-equality
// outcome, and no (kind, id) pair aliases another.
template <>
struct hash<::media_browser::MediaRef> {
    size_t operator()(const ::media_browser::MediaRef& r) const noexcept {
        return static_cast<size_t>(static_cast<unsigned>(r.id)) * 2u
             + (r.kind == ::media_browser::MediaKind::Tv ? 1u : 0u);
    }
};

}  // namespace std
