#include "media_browser/ui/mb_recs.h"

#include <algorithm>
#include <unordered_map>

namespace media_browser::ui {

std::vector<TmdbSearchHit> merge_recommendations(
    const std::vector<std::vector<TmdbSearchHit>>& per_seed,
    const std::unordered_set<int>& exclude,
    int cap) {
    struct Entry {
        TmdbSearchHit hit;
        int seed_count = 0;
        int min_index = 0;
    };
    std::unordered_map<int, Entry> by_id;
    for (const auto& seed_list : per_seed) {
        std::unordered_set<int> seen_this_seed;  // same seed repeating a title counts once
        for (int idx = 0; idx < static_cast<int>(seed_list.size()); ++idx) {
            const auto& hit = seed_list[idx];
            if (hit.tmdb_id <= 0) continue;
            if (exclude.count(hit.tmdb_id) > 0) continue;
            if (!seen_this_seed.insert(hit.tmdb_id).second) continue;
            auto [it, inserted] = by_id.try_emplace(hit.tmdb_id);
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
    entries.reserve(by_id.size());
    for (auto& [id, e] : by_id) entries.push_back(std::move(e));
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.seed_count != b.seed_count) return a.seed_count > b.seed_count;
        if (a.min_index != b.min_index) return a.min_index < b.min_index;
        return a.hit.tmdb_id < b.hit.tmdb_id;
    });
    if (static_cast<int>(entries.size()) > cap) entries.resize(cap);
    std::vector<TmdbSearchHit> out;
    out.reserve(entries.size());
    for (auto& e : entries) out.push_back(std::move(e.hit));
    return out;
}

}  // namespace media_browser::ui
