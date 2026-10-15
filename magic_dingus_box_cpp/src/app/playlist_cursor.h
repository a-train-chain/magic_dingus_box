#pragma once

// Sequential NEXT / PREV over one playlist: where the cursor goes, and
// when the end of the playlist means "back to the menu". Pure, tested on
// the Mac (tests/app/test_playlist_cursor.cpp); Controller::load_next_item
// and load_previous_item execute the decision.

#include <vector>

#include "app_state.h"

namespace app::cursor {

// The loop setting that applies to `pl`: its own `loop:` key for a VIDEO
// playlist; the global playlist_loop for a game playlist, where `loop:` is
// meaningless (selecting a game hands off to RetroArch).
bool effective_loop(const Playlist& pl, bool global_playlist_loop);

struct NextStep {
    bool stop_to_menu = false;   // end reached with looping off (or nothing to play)
    int index = -1;              // valid when !stop_to_menu
};

// Sequential next from `current` (-1 = nothing played yet). At the end:
// wrap to 0 when looping, else stop_to_menu — NOT "cursor to item 0":
// indexes left >= 0 with no video read as a between-items transition and
// the renderer drew nothing until reboot.
NextStep sequential_next(int current, int playlist_size, bool loop);

// Previous item, always wrapping (PREV at item 0 goes to the last item).
// -1 for an empty playlist.
int previous_index(int current, int playlist_size);

// Order in which load_next_item retries after `failed` fails to load: the
// following items, wrapping, stopping BEFORE coming back round to
// `origin` (the item that was playing). Empty when there is nothing else.
std::vector<int> retry_order(int failed, int origin, int playlist_size);

}  // namespace app::cursor
