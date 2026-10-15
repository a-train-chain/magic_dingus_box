#include "shuffle_queue.h"

#include <algorithm>

namespace app::shuffle {

std::vector<int> make_index_queue(int n, std::mt19937& rng) {
    std::vector<int> q;
    if (n <= 0) return q;
    q.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) q.push_back(i);
    std::shuffle(q.begin(), q.end(), rng);
    return q;
}

int next_shuffled_index(std::vector<int>& queue, int& position,
                        int& queue_playlist_id, int playlist_id,
                        int playlist_size, int current, std::mt19937& rng) {
    if (playlist_size <= 0) return -1;
    if (playlist_size == 1) return 0;

    if (queue_playlist_id != playlist_id) {
        queue.clear();   // built for another playlist: force regeneration
        queue_playlist_id = playlist_id;
    }
    auto take = [&]() {
        if (queue.empty() || position < 0 ||
            position >= static_cast<int>(queue.size()) ||
            static_cast<int>(queue.size()) != playlist_size) {
            queue = make_index_queue(playlist_size, rng);
            position = 0;
        }
        return queue[static_cast<std::size_t>(position++)];
    };
    int next = take();
    // A fresh permutation can start with the item that just played; take
    // the following one instead (it exists: playlist_size >= 2).
    if (next == current && position < playlist_size) next = take();
    return next;
}

std::vector<Entry> make_master_queue(const std::vector<int>& playlist_sizes,
                                     std::mt19937& rng) {
    std::vector<Entry> q;
    for (std::size_t p = 1; p < playlist_sizes.size(); ++p) {
        for (int i = 0; i < playlist_sizes[p]; ++i) {
            q.emplace_back(static_cast<int>(p), i);
        }
    }
    std::shuffle(q.begin(), q.end(), rng);
    return q;
}

std::optional<Entry> next_master_item(std::vector<Entry>& queue, int& position,
                                      const std::vector<int>& playlist_sizes,
                                      std::mt19937& rng) {
    if (queue.empty() || position < 0 || position >= static_cast<int>(queue.size())) {
        queue = make_master_queue(playlist_sizes, rng);
        position = 0;
    }
    if (queue.empty()) return std::nullopt;
    return queue[static_cast<std::size_t>(position++)];
}

bool is_valid_source_entry(const Entry& e, const std::vector<int>& playlist_sizes) {
    return e.first > 0 && e.first < static_cast<int>(playlist_sizes.size()) &&
           e.second >= 0 && e.second < playlist_sizes[static_cast<std::size_t>(e.first)];
}

void record_history(std::deque<Entry>& history, int playlist_index, int item_index) {
    if (playlist_index <= 0 || item_index < 0) return;
    history.emplace_back(playlist_index, item_index);
    while (history.size() > kMaxHistory) history.pop_front();
}

std::optional<Entry> pop_valid_history(std::deque<Entry>& history,
                                       const std::vector<int>& playlist_sizes) {
    while (!history.empty()) {
        const Entry e = history.back();
        history.pop_back();
        if (is_valid_source_entry(e, playlist_sizes)) return e;
    }
    return std::nullopt;
}

}  // namespace app::shuffle
