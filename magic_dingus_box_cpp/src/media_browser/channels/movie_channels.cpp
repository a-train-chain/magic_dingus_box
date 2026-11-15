#include "movie_channels.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numeric>

#include "app/channel_schedule.h"

namespace app {
namespace {

// Genre/certification/codec tokens are ASCII; avoid locale-dependent matching.
std::string lower_ascii(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}

bool has_genre(const LibraryMovie& movie, std::initializer_list<const char*> genres) {
    for (const auto& genre : movie.genres) {
        const auto normalized = lower_ascii(genre);
        for (const auto* candidate : genres) if (normalized == candidate) return true;
    }
    return false;
}

struct Rule {
    const char* key;
    const char* title;
    bool (*predicate)(const LibraryMovie&);
};

const Rule kRules[] = {
    {"mix", "MOVIE MIX", [](const LibraryMovie&) { return true; }},
    {"kids", "KIDS", [](const LibraryMovie& movie) {
        const auto cert = lower_ascii(movie.certification);
        return (cert == "g" || cert == "pg") && has_genre(movie, {"family", "animation"});
    }},
    {"classics", "CLASSICS", [](const LibraryMovie& movie) { return movie.year > 0 && movie.year < 1980; }},
    {"comedy", "COMEDY", [](const LibraryMovie& movie) { return has_genre(movie, {"comedy"}); }},
    {"drama", "DRAMA", [](const LibraryMovie& movie) { return has_genre(movie, {"drama"}); }},
    {"action", "ACTION & ADVENTURE", [](const LibraryMovie& movie) { return has_genre(movie, {"action", "adventure"}); }},
    {"scifi", "SCI-FI", [](const LibraryMovie& movie) { return has_genre(movie, {"science fiction", "fantasy"}); }},
    {"thrillers", "THRILLERS & CRIME", [](const LibraryMovie& movie) { return has_genre(movie, {"thriller", "crime", "mystery"}); }},
    {"horror", "HORROR", [](const LibraryMovie& movie) { return has_genre(movie, {"horror"}); }},
    {"romance", "ROMANCE", [](const LibraryMovie& movie) { return has_genre(movie, {"romance"}); }},
    {"war", "WAR & HISTORY", [](const LibraryMovie& movie) { return has_genre(movie, {"war", "history", "western"}); }},
};

}  // namespace

bool codec_allowed(const std::vector<std::string>& allowed, const std::string& codec) {
    if (allowed.empty() || codec.empty()) return true;
    const auto normalized = lower_ascii(codec);
    return std::any_of(allowed.begin(), allowed.end(), [&](const std::string& candidate) {
        return lower_ascii(candidate) == normalized;
    });
}

std::vector<Playlist> build_movie_playlists(const std::vector<LibraryMovie>& movies,
                                           const std::vector<std::string>& allowed_codecs) {
    std::vector<const LibraryMovie*> eligible;
    for (const auto& movie : movies) {
        if (movie.tmdb_id > 0 && !movie.path.empty() && std::isfinite(movie.length_s) &&
            movie.length_s >= kMinScheduledItemS && codec_allowed(allowed_codecs, movie.codec)) {
            eligible.push_back(&movie);
        }
    }
    if (eligible.size() < 3) return {};
    std::sort(eligible.begin(), eligible.end(), [](const LibraryMovie* a, const LibraryMovie* b) {
        if (a->title != b->title) return a->title < b->title;
        return a->tmdb_id < b->tmdb_id;
    });

    std::vector<Playlist> candidates;
    for (const auto& rule : kRules) {
        Playlist playlist;
        playlist.title = rule.title;
        playlist.path = std::string(kMovieChannelPrefix) + rule.key;
        for (const auto* movie : eligible) {
            if (!rule.predicate(*movie)) continue;
            PlaylistItem item;
            item.path = movie->path;
            item.source_type = "local";
            item.title = movie->title;
            item.artist = movie->year == 0 ? "" : std::to_string(movie->year);
            item.end = movie->length_s;
            item.movie_tmdb_id = movie->tmdb_id;
            playlist.items.push_back(std::move(item));
        }
        if (playlist.items.size() >= 3) candidates.push_back(std::move(playlist));
    }
    if (candidates.size() <= 8) return candidates;

    // Mix is first and always retained. Stable ranking keeps table order on
    // equal populations; restore that order after choosing the fullest seven.
    std::vector<size_t> selected(candidates.size());
    std::iota(selected.begin(), selected.end(), 0);
    std::stable_sort(selected.begin() + 1, selected.end(), [&](size_t a, size_t b) {
        return candidates[a].items.size() > candidates[b].items.size();
    });
    selected.resize(8);
    std::sort(selected.begin(), selected.end());
    std::vector<Playlist> result;
    for (size_t index : selected) result.push_back(std::move(candidates[index]));
    return result;
}

}  // namespace app
