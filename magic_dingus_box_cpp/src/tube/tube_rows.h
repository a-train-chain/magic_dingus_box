// tube_rows.h - the CRT "tube" settings model's WORDS, in one place.
//
// Ported from Sound Factory's data/DisplayCrt.h (commit 4ef74a6). A LOOK sets a
// starting point; each EFFECT is its own 0..100 row; the MASK row shows a word
// instead of a number. Row keys, their order, the range, the six look names and
// the five mask names are Sound Factory's, character for character - a second
// spelling of "magentaGreen" is a menu that offers one mask and a config that
// refuses it. Effect order is append-only: it is the column order of the look
// table built on top of this header.
//
// Pure: standard library only, no I/O. Tables and loops, never a switch, so a
// new value cannot be forgotten by a -Wswitch-enum build.
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace tube {

// MASK - the phosphor pattern laid over the picture.
enum class Mask { grille, magentaGreen, dots, slot, none };

// LOOK - a complete starting point for the tube, in the order Settings lists them.
enum class Look { pvm, monitor, trinitron, tv, arcade, everything };

// EFFECT - one 0..100 row, in canonical append-only order. 0 is not always
// "off": the five PICTURE rows are neutral at 50 and the two whole-pixel sizes
// have a smallest size at 0.
enum class Effect {
    // PICTURE
    brightness, contrast, saturation, white, sharpness,
    // SCANLINES
    scanlines, lineSize, beam, raster,
    // MASK (under the mask row)
    maskStrength, maskSize,
    // LIGHT
    glow, glowSize, afterglow,
    // GLASS
    curvature, vignette, rim, glassBlack, reflection,
    // SIGNAL
    convergence, chromaBleed, noise, jitter,
    // SUPPLY
    flicker, hum, abl, breathing,
    // Optional character, appended after the original 27 so indices never move.
    rollBand, rollSpeed, rollWidth, rollTint,
    tracking, trackingRate, colorDrift, driftSpeed, trackingWidth, trackingRecovery,
    flagging, flagHeight, flagSpeed,
    purity, puritySize, purityHue, purityPosition
};

inline constexpr std::size_t kEffectCount = 44;

inline constexpr std::array<Mask, 5> kMasks{Mask::grille, Mask::magentaGreen, Mask::dots,
                                            Mask::slot, Mask::none};

inline constexpr std::array<Look, 6> kLooks{Look::pvm, Look::monitor, Look::trinitron,
                                            Look::tv, Look::arcade, Look::everything};

// What the tube is the first time it is switched on.
inline constexpr Look kLookDefault = Look::pvm;

inline constexpr std::array<Effect, kEffectCount> kEffects{
    Effect::brightness, Effect::contrast, Effect::saturation, Effect::white, Effect::sharpness,
    Effect::scanlines, Effect::lineSize, Effect::beam, Effect::raster,
    Effect::maskStrength, Effect::maskSize,
    Effect::glow, Effect::glowSize, Effect::afterglow,
    Effect::curvature, Effect::vignette, Effect::rim, Effect::glassBlack, Effect::reflection,
    Effect::convergence, Effect::chromaBleed, Effect::noise, Effect::jitter,
    Effect::flicker, Effect::hum, Effect::abl, Effect::breathing,
    Effect::rollBand, Effect::rollSpeed, Effect::rollWidth, Effect::rollTint,
    Effect::tracking, Effect::trackingRate, Effect::colorDrift, Effect::driftSpeed,
    Effect::trackingWidth, Effect::trackingRecovery,
    Effect::flagging, Effect::flagHeight, Effect::flagSpeed,
    Effect::purity, Effect::puritySize, Effect::purityHue, Effect::purityPosition};

namespace detail {

// The config's word for each value, index-aligned with the enum's order.
inline constexpr std::array<std::string_view, 5> kMaskNames{"grille", "magentaGreen", "dots",
                                                            "slot", "none"};
inline constexpr std::array<std::string_view, 6> kLookNames{"pvm", "monitor", "trinitron",
                                                            "tv", "arcade", "everything"};
inline constexpr std::array<std::string_view, kEffectCount> kEffectKeys{
    "brightness", "contrast", "saturation", "white", "sharpness",
    "scanlines", "lineSize", "beam", "raster",
    "maskStrength", "maskSize",
    "glow", "glowSize", "afterglow",
    "curvature", "vignette", "rim", "glassBlack", "reflection",
    "convergence", "chromaBleed", "noise", "jitter",
    "flicker", "hum", "abl", "breathing",
    "rollBand", "rollSpeed", "rollWidth", "rollTint",
    "tracking", "trackingRate", "colorDrift", "driftSpeed",
    "trackingWidth", "trackingRecovery",
    "flagging", "flagHeight", "flagSpeed",
    "purity", "puritySize", "purityHue", "purityPosition"};

static_assert(kMaskNames.size() == kMasks.size(), "one name per mask");
static_assert(kLookNames.size() == kLooks.size(), "one name per look");
static_assert(kEffectKeys.size() == kEffects.size(), "one key per effect");

}  // namespace detail

// Position of `effect` in kEffects (its column in the look table).
constexpr std::size_t index_of(Effect effect) noexcept { return static_cast<std::size_t>(effect); }

// The config's key for `effect`, or "" for a value outside the enum.
constexpr std::string_view effect_key(Effect effect) noexcept {
    const std::size_t i = index_of(effect);
    return i < detail::kEffectKeys.size() ? detail::kEffectKeys[i] : std::string_view{};
}

// The effect `word` is the key of, exactly (case matters), or nullopt.
constexpr std::optional<Effect> parse_effect(std::string_view word) noexcept {
    for (const Effect candidate : kEffects)
        if (effect_key(candidate) == word) return candidate;
    return std::nullopt;
}

constexpr std::string_view mask_name(Mask mask) noexcept {
    const std::size_t i = static_cast<std::size_t>(mask);
    return i < detail::kMaskNames.size() ? detail::kMaskNames[i] : std::string_view{};
}

constexpr std::optional<Mask> parse_mask(std::string_view word) noexcept {
    for (const Mask candidate : kMasks)
        if (mask_name(candidate) == word) return candidate;
    return std::nullopt;
}

constexpr std::string_view look_name(Look look) noexcept {
    const std::size_t i = static_cast<std::size_t>(look);
    return i < detail::kLookNames.size() ? detail::kLookNames[i] : std::string_view{};
}

constexpr std::optional<Look> parse_look(std::string_view word) noexcept {
    for (const Look candidate : kLooks)
        if (look_name(candidate) == word) return candidate;
    return std::nullopt;
}

// The range of every effect row. 100 is the most a real tube shows; for the
// moving effects it is the flash-safe ceiling.
inline constexpr int kRowMin = 0;
inline constexpr int kRowMax = 100;

// Wide parameter: a JSON integer is, and callers ask before anything narrows it.
constexpr bool is_row_value(long long value) noexcept {
    return value >= kRowMin && value <= kRowMax;
}

}  // namespace tube
