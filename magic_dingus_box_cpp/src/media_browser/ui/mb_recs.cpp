#include "media_browser/ui/mb_recs.h"

#include <algorithm>
#include <unordered_map>

namespace media_browser::ui {

std::vector<TmdbSearchHit> merge_recommendations(
    const std::vector<std::vector<TmdbSearchHit>>& per_seed,
    const std::unordered_set<MediaRef>& exclude,
    int cap) {
    struct Entry {
        TmdbSearchHit hit;
        int seed_count = 0;
        int min_index = 0;
    };
    std::unordered_map<MediaRef, Entry> by_ref;
    for (const auto& seed_list : per_seed) {
        std::unordered_set<MediaRef> seen_this_seed;  // same seed repeating a title counts once
        for (int idx = 0; idx < static_cast<int>(seed_list.size()); ++idx) {
            const auto& hit = seed_list[idx];
            if (hit.tmdb_id <= 0) continue;
            const MediaRef ref = media_ref_of(hit);
            if (exclude.count(ref) > 0) continue;
            if (!seen_this_seed.insert(ref).second) continue;
            auto [it, inserted] = by_ref.try_emplace(ref);
            Entry& e = it->second;
            if (inserted) {
                e.hit = hit;
                e.min_index = idx;
            } else if (idx < e.min_index) {
                e.min_index = idx;
            }
            ++e.seed_count;
        }
    }
    std::vector<Entry> entries;
    entries.reserve(by_ref.size());
    for (auto& [ref, e] : by_ref) entries.push_back(std::move(e));
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.seed_count != b.seed_count) return a.seed_count > b.seed_count;
        if (a.min_index != b.min_index) return a.min_index < b.min_index;
        if (a.hit.tmdb_id != b.hit.tmdb_id) return a.hit.tmdb_id < b.hit.tmdb_id;
        // Kind is the last discriminator so the ordering is TOTAL: without
        // it, a movie and a show sharing one tmdb id compare equivalent and
        // std::sort may emit them in either order run to run.
        return a.hit.kind < b.hit.kind;
    });
    if (static_cast<int>(entries.size()) > cap) entries.resize(cap);
    std::vector<TmdbSearchHit> out;
    out.reserve(entries.size());
    for (auto& e : entries) out.push_back(std::move(e.hit));
    return out;
}

}  // namespace media_browser::ui
