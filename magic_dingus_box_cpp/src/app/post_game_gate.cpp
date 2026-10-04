#include "post_game_gate.h"

namespace app {

bool PostGameGate::take_ready(bool display_reset_pending) {
    if (!settling_ || display_reset_pending) return false;
    settling_ = false;
    return true;
}

}  // namespace app
