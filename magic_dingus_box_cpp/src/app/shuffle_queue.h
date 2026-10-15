#pragma once

// Shuffle and Master Shuffle ordering, and the Master Shuffle "back"
// history — the decisions only, on plain vectors with an injected RNG, so
// they are unit-tested on the Mac (tests/app/test_shuffle_queue.cpp).
// Controller and main.cpp own the side effects (loading, UI state).
//
// WHY extracted: this lived inline in controller.cpp/main.cpp, in no test
// target, and had a crash nobody could see: a box whose only content
// playlists were all EMPTY generated an empty Master Shuffle queue and
// then indexed element 0 of it.

#include <cstddef>
#include <deque>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace app::shuffle {

using Entry = std::pair<int, int>;   // (playlist index, item index)

// Fisher-Yates permutation of 0..n-1. Empty for n <= 0.
std::vector<int> make_index_queue(int n, std::mt19937& rng);

// Per-playlist shuffle: every item plays once before any repeats. Takes
// the next index from `queue` (cursor `position`), regenerating when the
// queue is exhausted, sized for a different playlist length, or built for
// a different playlist (`queue_playlist_id` vs `playlist_id`). Avoids
// replaying `current` back-to-back when the playlist has another item.
// Returns -1 for an empty playlist; 0 for a one-item playlist.
int next_shuffled_index(std::vector<int>& queue, int& position,
                        int& queue_playlist_id, int playlist_id,
                        int playlist_size, int current, std::mt19937& rng);

// Every (playlist, item) of playlists 1..N, shuffled. Playlist 0 is the
// VIRTUAL Master Shuffle row (one dummy item, no file) and never a source.
std::vector<Entry> make_master_queue(const std::vector<int>& playlist_sizes,
                                     std::mt19937& rng);

// Next Master Shuffle pick, regenerating the queue when exhausted.
// nullopt when there is nothing to play at all (no source playlist has an
// item) — the caller must not index into the queue then.
std::optional<Entry> next_master_item(std::vector<Entry>& queue, int& position,
                                      const std::vector<int>& playlist_sizes,
                                      std::mt19937& rng);

// Does `e` still name a real item of a real SOURCE playlist? A queue or
// history entry can go stale when playlists are reloaded under it.
bool is_valid_source_entry(const Entry& e, const std::vector<int>& playlist_sizes);

// ── Master Shuffle history ("PREV" walks back through what played) ────
constexpr std::size_t kMaxHistory = 10;

// Record the item being left. Ignores the virtual row (index 0) and
// unset indices — a later PREV would otherwise try to load the dummy item,
// fail after playback_started_ was already cleared, and stall
// auto-advance. Keeps at most kMaxHistory entries (oldest dropped).
void record_history(std::deque<Entry>& history, int playlist_index, int item_index);

// Pop back to the most recent entry that is still valid, discarding stale
// ones on the way. nullopt when nothing valid remains.
std::optional<Entry> pop_valid_history(std::deque<Entry>& history,
                                       const std::vector<int>& playlist_sizes);

}  // namespace app::shuffle
