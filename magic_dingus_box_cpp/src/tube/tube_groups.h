// tube_groups.h - how the 44 tube rows are grouped, labelled and stepped.
//
// Ported from Sound Factory's ui/src/screens/settings/crt-settings.ts
// (TUBE_EFFECT labels + hints, TUBE_GROUPS). One table, read by the TV menu and
// the web page, so the two cannot disagree about a row's name or its place.
// Labels and hints are Sound Factory's, with three edits for a TV: the two
// whole-pixel size rows and RIM say what the kiosk means by them.
//
// Pure: standard library only, no I/O. Tables and loops, never a switch.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "tube/tube_rows.h"

namespace tube {

// ROW INFO - the words a row wears in a menu: its label and two hint lines.
struct RowInfo {
    std::string_view label, hint1, hint2;
};

namespace detail {

// Index-aligned with Effect's order (kEffects).
inline constexpr std::array<RowInfo, kEffectCount> kRowInfo{{
    {"BRIGHTNESS", "the picture's overall light", "50 is neutral · half to 1.5x"},
    {"CONTRAST", "how hard the tones are pushed", "50 is neutral · tubes run higher"},
    {"SATURATION", "how strong the colour is", "0 mono · 50 neutral · 100 twice"},
    {"COLOUR TEMP", "the colour of white", "0 warm · 50 neutral · 100 cool"},
    {"SHARPNESS", "how fine the picture is drawn", "0 soft · 50 neutral · 100 crisp"},

    {"SCANLINES", "the dark gap between lines", "the beam swells into it"},
    {"SCANLINE SIZE", "how tall one beam line is", "in the TV's own pixels"},
    {"BEAM SWELL", "a bright line's beam grows wider", "bright marks fill their own gaps"},
    {"TRUE RASTER", "0 keeps every pixel row · sharp", "100 is a true raster · authentic"},

    {"MASK STRENGTH", "how dark the mask's gaps are", "brightness is compensated"},
    {"MASK SIZE", "how wide one colour triad is", "in the TV's own pixels"},

    {"GLOW", "light scattered in the glass", "a soft halo on bright marks"},
    {"GLOW SIZE", "how far the light scatters", "wider softens what is around it"},
    {"AFTERGLOW", "the phosphor lingers", "red and green, blue fades first"},

    {"CURVATURE", "how far the picture bends", "with the glass of the tube"},
    {"VIGNETTE", "the edges and corners darken", "the middle keeps its light"},
    {"RIM", "the dark lip around the picture", "where the tube meets its surround"},
    {"GLASS BLACK", "the black of lit glass", "at 100 still barely off black"},
    {"REFLECTION", "a soft reflection of the room", "a faint highlight on the glass"},

    {"CONVERGENCE", "the three guns misaligned", "grows toward the edges"},
    {"CHROMA BLEED", "colour smearing sideways", "a composite signal's blur"},
    {"NOISE", "fine RF grain", "moves on its own"},
    {"JITTER", "the picture's slight shake", "moves on its own"},

    {"FLICKER", "the light's shimmer", "moves on its own"},
    {"HUM BAR", "soft at low · pronounced at 100", "moves · rolls up every 16.7 s"},
    {"BRIGHTNESS LIMIT", "bright screens dim slightly", "as a tube's supply sags"},
    {"BREATHING", "the picture swells when bright", "on the same sag · ≤ 1 %"},

    {"ROLLING BAND", "a soft dark band rolls down", "0 off · depth up to 44 %"},
    {"BAND SPEED", "how fast the band rolls down", "0: 30 s · 100: 6 s per pass"},
    {"BAND WIDTH", "the full width of the soft band", "0: 8 mm · 100: 60 mm"},
    {"BAND TINT", "the colour inside the band", "0 warm · 50 neutral · 100 cool"},
    {"TRACKING SLIP", "a narrow strip slips sideways", "0 off · up to 3.6 mm"},
    {"SLIP RATE", "how often a tracking slip occurs", "0: every 60 s · 100: every 4 s"},
    {"COLOUR DRIFT", "slow red and blue fringing", "0 off · up to 0.75 mm"},
    {"DRIFT SPEED", "how fast the colours drift", "0: 24 s · 100: 6 s per cycle"},
    {"TEAR WIDTH", "height of the displaced strip", "0: 2 mm · 100: 32 mm"},
    {"TEAR RECOVERY", "time for a slip to settle", "0: 0.12 s · 100: 1.12 s"},
    {"TOP EDGE BEND", "the top of the picture sways", "0 off · up to 4 mm"},
    {"BEND HEIGHT", "how far down the bend reaches", "0: 5 % · 100: 35 %"},
    {"BEND SPEED", "how fast the top edge sways", "0: 12 s · 100: 2 s"},
    {"COLOUR PATCH", "uneven colour near the edge", "0 off · strong at 100"},
    {"PATCH SIZE", "radius of the soft colour patch", "0: 10 mm · 100: 70 mm"},
    {"PATCH COLOUR", "tint of the discoloured area", "0–100: full colour wheel"},
    {"PATCH POSITION", "move the patch around the edge", "starts top left · clockwise"},
}};

// A group's numeric rows: a fixed-capacity, constexpr range (C++17 has no span).
struct GroupRows {
    std::array<Effect, 7> items{};
    std::size_t count = 0;
    constexpr const Effect* begin() const noexcept { return items.data(); }
    constexpr const Effect* end() const noexcept { return items.data() + count; }
    constexpr std::size_t size() const noexcept { return count; }
};

template <typename... E>
constexpr GroupRows rows_of(E... e) noexcept {
    GroupRows r;
    for (Effect x : {e...}) r.items[r.count++] = x;
    return r;
}

}  // namespace detail

constexpr const RowInfo& row_info(Effect e) noexcept { return detail::kRowInfo[index_of(e)]; }

// The MASK row shows a word, not a number; its hints belong to the chosen mask.
inline constexpr RowInfo kMaskRowInfo{"MASK", "", ""};

// GROUP - one section of the menu. `has_mask_row` puts the mask choice first;
// `rows` holds only the numeric rows.
struct Group {
    std::string_view key, label;
    bool has_mask_row;
    detail::GroupRows rows;
};

inline constexpr std::array<Group, 10> kGroups{{
    {"picture", "PICTURE", false,
     detail::rows_of(Effect::brightness, Effect::contrast, Effect::saturation, Effect::white,
                     Effect::sharpness)},
    {"scanlines", "SCANLINES", false,
     detail::rows_of(Effect::scanlines, Effect::lineSize, Effect::beam, Effect::raster)},
    {"mask", "MASK", true, detail::rows_of(Effect::maskStrength, Effect::maskSize)},
    {"light", "LIGHT", false, detail::rows_of(Effect::glow, Effect::glowSize, Effect::afterglow)},
    {"glass", "GLASS", false,
     detail::rows_of(Effect::curvature, Effect::vignette, Effect::rim, Effect::glassBlack,
                     Effect::reflection)},
    {"signal", "SIGNAL", false,
     detail::rows_of(Effect::convergence, Effect::chromaBleed, Effect::noise, Effect::jitter)},
    {"supply", "SUPPLY", false,
     detail::rows_of(Effect::flicker, Effect::hum, Effect::abl, Effect::breathing)},
    {"character", "CHARACTER", false,
     detail::rows_of(Effect::rollBand, Effect::rollSpeed, Effect::rollWidth, Effect::rollTint,
                     Effect::colorDrift, Effect::driftSpeed)},
    {"sync", "SYNC", false,
     detail::rows_of(Effect::tracking, Effect::trackingRate, Effect::trackingWidth,
                     Effect::trackingRecovery, Effect::flagging, Effect::flagHeight,
                     Effect::flagSpeed)},
    {"purity", "COLOUR PURITY", false,
     detail::rows_of(Effect::purity, Effect::puritySize, Effect::purityHue,
                     Effect::purityPosition)},
}};

// One knob click moves a row by this much: the two whole-pixel sizes by 10, the rest by 5.
constexpr int row_step(Effect e) noexcept {
    return (e == Effect::lineSize || e == Effect::maskSize) ? 10 : 5;
}

// One step up or down (direction +1 / -1) onto the next multiple of the step
// strictly past `value`, clamped to 0..100. Off-grid values (set from the web) snap.
constexpr int step_row(Effect e, int value, int direction) noexcept {
    const int s = row_step(e);
    int next;
    if (direction > 0) {
        next = (value >= 0 ? value / s + 1 : 0) * s;
    } else {
        next = (value > 0 ? (value - 1) / s : -1) * s;
    }
    return next < 0 ? 0 : (next > 100 ? 100 : next);
}

}  // namespace tube
