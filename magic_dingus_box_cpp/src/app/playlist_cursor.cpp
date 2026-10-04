#include "playlist_cursor.h"

namespace app::cursor {

bool effective_loop(const Playlist& pl, bool global_playlist_loop) {
    return pl.is_game_playlist() ? global_playlist_loop : pl.loop;
}

NextStep sequential_next(int current, int playlist_size, bool loop) {
    NextStep s;
    if (playlist_size <= 0) {
        s.stop_to_menu = true;
        return s;
    }
    const int next = (current < 0) ? 0 : current + 1;
    if (next < playlist_size) {
        s.index = next;
    } else if (loop) {
        s.index = 0;
    } else {
        s.stop_to_menu = true;
    }
    return s;
}

int previous_index(int current, int playlist_size) {
    if (playlist_size <= 0) return -1;
    if (current <= 0 || current >= playlist_size) return playlist_size - 1;
    return current - 1;
}

std::vector<int> retry_order(int failed, int origin, int playlist_size) {
    std::vector<int> order;
    if (playlist_size <= 1) return order;
    int idx = failed;
    // At most playlist_size - 1 candidates: every item but the failed one.
    for (int n = 0; n < playlist_size - 1; ++n) {
        idx = (idx + 1) % playlist_size;
        if (idx == origin) break;   // wrapped back to what was playing
        order.push_back(idx);
    }
    return order;
}

}  // namespace app::cursor
