// app::shuffle — Shuffle / Master Shuffle ordering and the "PREV" history.
// Seeded RNG throughout; every property below must hold for EVERY seed,
// so the cases loop over many.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <random>
#include <set>
#include <vector>

#include "app/shuffle_queue.h"

namespace sh = app::shuffle;
using sh::Entry;

TEST_CASE("shuffle: index queue is a permutation", "[shuffle]") {
    std::mt19937 rng(1);
    CHECK(sh::make_index_queue(0, rng).empty());
    CHECK(sh::make_index_queue(-3, rng).empty());
    auto q = sh::make_index_queue(25, rng);
    std::sort(q.begin(), q.end());
    std::vector<int> want(25);
    for (int i = 0; i < 25; ++i) want[static_cast<size_t>(i)] = i;
    CHECK(q == want);
}

TEST_CASE("shuffle: every item plays once per cycle", "[shuffle]") {
    for (unsigned seed = 0; seed < 50; ++seed) {
        std::mt19937 rng(seed);
        std::vector<int> queue;
        int pos = 0, qid = -1, current = -1;
        std::multiset<int> seen;
        for (int k = 0; k < 7; ++k) {
            current = sh::next_shuffled_index(queue, pos, qid, 3, 7, current, rng);
            seen.insert(current);
        }
        for (int i = 0; i < 7; ++i) CHECK(seen.count(i) == 1);
        CHECK(qid == 3);
    }
}

TEST_CASE("shuffle: never the same item twice in a row across reshuffles", "[shuffle]") {
    for (unsigned seed = 0; seed < 200; ++seed) {
        std::mt19937 rng(seed);
        std::vector<int> queue;
        int pos = 0, qid = -1, current = -1;
        for (int k = 0; k < 40; ++k) {
            const int next = sh::next_shuffled_index(queue, pos, qid, 1, 4, current, rng);
            REQUIRE(next != current);
            REQUIRE(next >= 0);
            REQUIRE(next < 4);
            current = next;
        }
    }
}

TEST_CASE("shuffle: degenerate playlist sizes", "[shuffle]") {
    std::mt19937 rng(7);
    std::vector<int> queue;
    int pos = 0, qid = -1;
    CHECK(sh::next_shuffled_index(queue, pos, qid, 1, 0, -1, rng) == -1);
    CHECK(sh::next_shuffled_index(queue, pos, qid, 1, 1, 0, rng) == 0);
    CHECK(sh::next_shuffled_index(queue, pos, qid, 1, 1, 0, rng) == 0);
}

TEST_CASE("shuffle: switching playlist or size regenerates the queue", "[shuffle]") {
    std::mt19937 rng(3);
    std::vector<int> queue;
    int pos = 0, qid = -1;
    (void)sh::next_shuffled_index(queue, pos, qid, 1, 5, -1, rng);
    CHECK(queue.size() == 5u);
    // Different playlist, same size: a queue half-consumed for playlist 1
    // must not leak its remaining order into playlist 2.
    (void)sh::next_shuffled_index(queue, pos, qid, 2, 5, -1, rng);
    CHECK(qid == 2);
    CHECK(pos == 1);
    // The playlist shrank under the queue (reload): indices must stay in range.
    for (int k = 0; k < 10; ++k) {
        const int i = sh::next_shuffled_index(queue, pos, qid, 2, 3, -1, rng);
        CHECK(i >= 0);
        CHECK(i < 3);
    }
}

TEST_CASE("master shuffle: queue covers every source item, never the virtual row",
          "[shuffle]") {
    std::mt19937 rng(11);
    // Index 0 = the virtual Master Shuffle row (one dummy item).
    const std::vector<int> sizes = {1, 3, 0, 2};
    auto q = sh::make_master_queue(sizes, rng);
    std::sort(q.begin(), q.end());
    CHECK(q == std::vector<Entry>{{1, 0}, {1, 1}, {1, 2}, {3, 0}, {3, 1}});
}

TEST_CASE("master shuffle: cycles through everything, then reshuffles", "[shuffle]") {
    std::mt19937 rng(5);
    const std::vector<int> sizes = {1, 2, 2};
    std::vector<Entry> queue;
    int pos = 0;
    std::set<Entry> first_cycle;
    for (int k = 0; k < 4; ++k) {
        auto e = sh::next_master_item(queue, pos, sizes, rng);
        REQUIRE(e.has_value());
        first_cycle.insert(*e);
    }
    CHECK(first_cycle.size() == 4u);
    auto again = sh::next_master_item(queue, pos, sizes, rng);   // regenerated
    REQUIRE(again.has_value());
    CHECK(pos == 1);
}

TEST_CASE("master shuffle: no playable items yields nothing (was an out-of-bounds read)",
          "[shuffle]") {
    std::mt19937 rng(9);
    std::vector<Entry> queue;
    int pos = 0;
    // Only the virtual row, or source playlists that are all empty.
    CHECK_FALSE(sh::next_master_item(queue, pos, {1}, rng).has_value());
    CHECK_FALSE(sh::next_master_item(queue, pos, {1, 0, 0}, rng).has_value());
    CHECK_FALSE(sh::next_master_item(queue, pos, {}, rng).has_value());
}

TEST_CASE("master shuffle: entry validity", "[shuffle]") {
    const std::vector<int> sizes = {1, 3, 0};
    CHECK(sh::is_valid_source_entry({1, 0}, sizes));
    CHECK(sh::is_valid_source_entry({1, 2}, sizes));
    CHECK_FALSE(sh::is_valid_source_entry({0, 0}, sizes));    // virtual row
    CHECK_FALSE(sh::is_valid_source_entry({1, 3}, sizes));    // past the end
    CHECK_FALSE(sh::is_valid_source_entry({2, 0}, sizes));    // emptied playlist
    CHECK_FALSE(sh::is_valid_source_entry({3, 0}, sizes));    // deleted playlist
    CHECK_FALSE(sh::is_valid_source_entry({1, -1}, sizes));
}

TEST_CASE("master shuffle history: back walks what played, newest first", "[shuffle]") {
    std::deque<Entry> h;
    const std::vector<int> sizes = {1, 5, 5};
    sh::record_history(h, 1, 0);
    sh::record_history(h, 2, 3);
    sh::record_history(h, 1, 4);
    CHECK(sh::pop_valid_history(h, sizes) == Entry{1, 4});
    CHECK(sh::pop_valid_history(h, sizes) == Entry{2, 3});
    CHECK(sh::pop_valid_history(h, sizes) == Entry{1, 0});
    CHECK_FALSE(sh::pop_valid_history(h, sizes).has_value());
}

TEST_CASE("master shuffle history: virtual row and unset indices are never recorded",
          "[shuffle]") {
    std::deque<Entry> h;
    sh::record_history(h, 0, 0);    // the virtual Master Shuffle row
    sh::record_history(h, -1, 2);
    sh::record_history(h, 2, -1);
    CHECK(h.empty());
}

TEST_CASE("master shuffle history: capped at kMaxHistory, oldest dropped", "[shuffle]") {
    std::deque<Entry> h;
    for (int i = 0; i < 25; ++i) sh::record_history(h, 1, i);
    CHECK(h.size() == sh::kMaxHistory);
    CHECK(h.front() == Entry{1, 25 - static_cast<int>(sh::kMaxHistory)});
    CHECK(h.back() == Entry{1, 24});
}

TEST_CASE("master shuffle history: stale entries are skipped, not dead-ends", "[shuffle]") {
    // Playlist 2 was emptied and item 4 of playlist 1 removed by a reload:
    // PREV must step past them to the last entry that still exists rather
    // than silently doing nothing.
    std::deque<Entry> h;
    sh::record_history(h, 1, 1);
    sh::record_history(h, 2, 0);
    sh::record_history(h, 1, 4);
    const std::vector<int> sizes = {1, 3, 0};
    CHECK(sh::pop_valid_history(h, sizes) == Entry{1, 1});
    CHECK(h.empty());
}
