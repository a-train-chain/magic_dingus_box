// tube_edit.h - the TV menu's EDIT MODE on one tube row.
//
// SELECT on a row opens an edit; each knob click or D-pad press is one turn; SELECT
// or BACK ends it. A value row steps from the RESOLVED value (the look's own when
// nothing is moved) by row_step; the MASK row cycles the five mask words. Every
// change goes through set_row / set_mask, so a row turned back onto the look's own
// value is not moved. end_edit() says whether anything changed, so the caller saves
// once per edit, never per turn.
//
// Pure: standard library only, no I/O.
#pragma once

#include <cstddef>
#include <optional>

#include "tube/tube_groups.h"
#include "tube/tube_rows.h"
#include "tube/tube_setting.h"

namespace tube {

struct Edit {
    bool active = false;
    bool mask = false;                   // the MASK row (a word), not a value row
    Effect effect = Effect::brightness;  // the value row; read only when !mask
    bool changed = false;                // a turn moved something since begin_edit
};

// Open an edit on a value row, or on the MASK row when `effect` is nullopt.
inline void begin_edit(Edit& edit, std::optional<Effect> effect) noexcept {
    edit.active = true;
    edit.mask = !effect.has_value();
    edit.effect = effect.value_or(Effect::brightness);
    edit.changed = false;
}

// Which way a turn goes. The knob and left/right: the sign of `delta`. Up/down
// (ROTATE_VERTICAL, where up arrives as -1): minus the sign, so up is +1 on both.
constexpr int edit_direction(int delta, bool vertical_axis) noexcept {
    const int sign = (delta > 0) - (delta < 0);
    return vertical_axis ? -sign : sign;
}

// The mask one step from `mask` through kMasks, wrapping at both ends. Only the
// sign of `direction` is read; 0 stays put.
constexpr Mask next_mask(Mask mask, int direction) noexcept {
    const std::size_t n = kMasks.size();
    std::size_t i = static_cast<std::size_t>(mask);
    if (i >= n) i = 0;
    if (direction > 0) return kMasks[(i + 1) % n];
    if (direction < 0) return kMasks[(i + n - 1) % n];
    return kMasks[i];
}

// One turn of an open edit. True when the resolved value changed (and the edit
// is marked changed); false when no edit is open or the row is already at its end.
inline bool edit_turn(Edit& edit, TubeSetting& setting, int direction) noexcept {
    if (!edit.active) return false;
    const Resolved before = resolve(setting);
    if (edit.mask) {
        const Mask next = next_mask(before.mask, direction);
        if (next == before.mask) return false;
        set_mask(setting, next);
    } else {
        const int current = before.effects[index_of(edit.effect)];
        const int next = step_row(edit.effect, current, direction);
        if (next == current || !set_row(setting, edit.effect, next)) return false;
    }
    edit.changed = true;
    return true;
}

// Close the edit. Returns whether anything changed (the caller saves once); the
// edit is left inactive and unchanged, so a second end_edit returns false.
inline bool end_edit(Edit& edit) noexcept {
    const bool changed = edit.changed;
    edit.active = false;
    edit.changed = false;
    return changed;
}

}  // namespace tube
