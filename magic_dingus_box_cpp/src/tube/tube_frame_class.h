// tube_frame_class.h - does this setting draw at 60 fps (lean) or 30 fps (rich)?
//
// The rule (design section 3, "Frame class"): a setting is Lean60 only when every row
// that needs a pass the lean path lacks sits at its neutral value. Which rows those are
// comes from the measured lean set in tube_budget.h.
//
// Pure: standard library only, no I/O.
#pragma once

#include "tube/tube_budget.h"
#include "tube/tube_inputs.h"
#include "tube/tube_setting.h"

namespace tube {

enum class FrameClass { Lean60, Rich30 };

constexpr int frames_per_second(FrameClass frame_class) noexcept {
    return frame_class == FrameClass::Lean60 ? 60 : 30;
}

// The picture the lean budget was measured at.
inline constexpr double kReferencePicturePixels = 1440.0 * 1080.0;

// The value at which a row asks for nothing: 50 for the three picture rows that are
// centred, else 0.
constexpr int rate_neutral(Effect effect) noexcept {
    return effect == Effect::sharpness || effect == Effect::contrast || effect == Effect::saturation ? 50 : 0;
}

// Whether a non-neutral value on this row makes the frame rich.
constexpr bool forces_30fps(Effect effect, const LeanSet& lean = kLeanSet) noexcept {
    if (!lean.usable) return true;
    for (const Effect always : kAlwaysRich)
        if (always == effect) return true;
    switch (effect) {
        case Effect::curvature: return !lean.bend;
        case Effect::scanlines: return !lean.beam;
        case Effect::contrast:
        case Effect::saturation: return !lean.tone;
        case Effect::jitter:
        case Effect::tracking:
        case Effect::flagging: return !lean.displace;
        case Effect::noise:
        case Effect::flicker:
        case Effect::hum:
        case Effect::rollBand:
        case Effect::purity: return !lean.motion;
        case Effect::abl:
        case Effect::breathing: return !lean.probe;
        default: return false;
    }
}

// The class of a setting, before the picture size is known. On a CRT Native output the
// rows the real tube draws itself count as neutral.
inline FrameClass setting_frame_class(const Resolved& resolved, bool crt_native,
                                      const LeanSet& lean = kLeanSet) noexcept {
    if (!lean.usable) return FrameClass::Rich30;
    for (const Effect effect : kEffects) {
        if (!forces_30fps(effect, lean)) continue;
        if (crt_native && neutral_in_crt_native(effect)) continue;
        if (resolved.effects[index_of(effect)] != rate_neutral(effect)) return FrameClass::Rich30;
    }
    return FrameClass::Lean60;
}

// Also rich when the picture is larger than the one the lean budget was measured at.
inline FrameClass frame_class(const Resolved& resolved, const Picture& picture,
                              const LeanSet& lean = kLeanSet) noexcept {
    if (picture.w * picture.h > kReferencePicturePixels) return FrameClass::Rich30;
    return setting_frame_class(resolved, picture.crt_native, lean);
}

}  // namespace tube
