#pragma once

// Private to the ui::Renderer implementation files (renderer*.cpp) — not
// part of the Renderer API. Anything shared by more than one of those
// translation units lives here instead of being duplicated.

#include <GLES3/gl3.h>

// Every UI renderer draw goes through this, so the per-frame count the
// journal reports covers both the immediate and the batched path.
#define UI_DRAW_ARRAYS(...)      \
    do {                         \
        ++ui_draw_calls_;        \
        glDrawArrays(__VA_ARGS__); \
    } while (0)
