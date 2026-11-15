// tube_setting.h - what the user chose for the tube: a LOOK plus only the rows
// they moved on top of it.
//
// Ported from Sound Factory's CrtTube/resolveCrt/crtMovedKeys, with two deliberate
// differences: a stated value equal to the look's own is CLEARED rather than kept
// (so "moved back" is not "custom"), and picking a look clears everything moved.
// resolve() turns the setting into the 44 concrete values plus the mask.
//
// For whoever ports Sound Factory's renderer, the names cross over: the box's
// TubeSetting is Sound Factory's sparse CrtTube / DisplayCrt (a look plus the
// moved rows); the box's Resolved is what Sound Factory's panel calls
// TubeSetting (the 44 concrete values).
//
// Pure: standard library only, no I/O.
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "tube/tube_looks.h"
#include "tube/tube_rows.h"

namespace tube {

// A look and the overrides on top of it. `rows` / `mask` hold ONLY what the
// user moved; an empty slot follows the look. A fresh value is the default look,
// nothing moved.
struct TubeSetting {
    Look look = kLookDefault;
    // `mask` and `rows`: write them through set_mask() / set_row(), or check a
    // row with is_row_value() first. resolve() copies what it finds here with
    // no range check.
    std::optional<Mask> mask;
    std::array<std::optional<int>, kEffectCount> rows{};

    bool operator==(const TubeSetting& other) const {
        return look == other.look && mask == other.mask && rows == other.rows;
    }
    bool operator!=(const TubeSetting& other) const { return !(*this == other); }
};

// Every value concrete: the look's, with the moved rows on top.
struct Resolved {
    Look look = kLookDefault;
    Mask mask = Mask::grille;
    std::array<int, kEffectCount> effects{};
    bool custom = false;  // anything moved off the look
};

inline Resolved resolve(const TubeSetting& setting) noexcept {
    const LookValues& base = look_values(setting.look);
    Resolved out;
    out.look = setting.look;
    out.mask = setting.mask.value_or(base.mask);
    out.effects = base.effects;
    for (std::size_t i = 0; i < kEffectCount; ++i)
        if (setting.rows[i]) out.effects[i] = *setting.rows[i];
    // By value, as moved_keys() is: a row stated at the look's own value is not moved.
    out.custom = out.mask != base.mask || out.effects != base.effects;
    return out;
}

// The keys moved off the look: effects in canonical order, then "mask".
inline std::vector<std::string_view> moved_keys(const TubeSetting& setting) {
    const LookValues& base = look_values(setting.look);
    std::vector<std::string_view> keys;
    for (std::size_t i = 0; i < kEffectCount; ++i)
        if (setting.rows[i] && *setting.rows[i] != base.effects[i]) keys.push_back(effect_key(kEffects[i]));
    if (setting.mask && *setting.mask != base.mask) keys.push_back("mask");
    return keys;
}

// Move one row. False, and no change, when `value` is outside 0..100. A value
// equal to the look's own clears the override.
inline bool set_row(TubeSetting& setting, Effect effect, int value) noexcept {
    const std::size_t i = index_of(effect);
    if (i >= kEffectCount || !is_row_value(value)) return false;
    if (value == look_values(setting.look).effects[i])
        setting.rows[i].reset();
    else
        setting.rows[i] = value;
    return true;
}

// Choose a mask. Equal to the look's own clears the override.
inline void set_mask(TubeSetting& setting, Mask mask) noexcept {
    if (mask == look_values(setting.look).mask)
        setting.mask.reset();
    else
        setting.mask = mask;
}

// Back to the look's own values: nothing moved.
inline void reset_to_look(TubeSetting& setting) noexcept {
    setting.mask.reset();
    setting.rows = {};
}

// Pick a look. Everything moved is cleared (spec section 1).
inline void select_look(TubeSetting& setting, Look look) noexcept {
    setting.look = look;
    reset_to_look(setting);
}

}  // namespace tube
