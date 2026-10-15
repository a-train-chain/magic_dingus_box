#pragma once

#include <algorithm>
#include <vector>

namespace media_browser::ui {

// Pure helpers behind the Movie Settings "Sources" panel. Templated on the
// row type so the screen's nested IndexerRow (id / enabled / has_stats /
// result_count) is used directly and the Mac tests can drive them with a
// plain struct — no GL, no network.

// Canonical panel order: enabled before disabled, then rows with search
// stats, then by result count (desc). stable_sort so equal rows keep their
// relative order across repeated toggles (the cursor must not jump).
template <typename Row>
void sort_indexer_rows(std::vector<Row>& rows) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) {
                         if (a.enabled != b.enabled) return a.enabled > b.enabled;
                         if (a.has_stats != b.has_stats)
                             return a.has_stats > b.has_stats;
                         return a.result_count > b.result_count;
                     });
}

// Apply a COMPLETED async enable/disable to the rows, keyed by indexer id
// (never by index: a full reload may have replaced/reordered the list while
// the PUT was in flight). Re-sorts so the row migrates to its section and
// returns the cursor index that follows it, clamped to the visible window.
// Returns -1 when the id is no longer present (the reload dropped it) — the
// caller keeps its cursor unchanged.
template <typename Row>
int apply_indexer_toggle(std::vector<Row>& rows, int id, bool enabled,
                         int max_visible) {
    bool found = false;
    for (auto& r : rows) {
        if (r.id == id) {
            r.enabled = enabled;
            found = true;
            break;
        }
    }
    if (!found) return -1;
    sort_indexer_rows(rows);
    for (size_t k = 0; k < rows.size(); ++k) {
        if (rows[k].id == id) {
            return std::min<int>(static_cast<int>(k),
                                 std::max(0, max_visible - 1));
        }
    }
    return -1;
}

}  // namespace media_browser::ui
