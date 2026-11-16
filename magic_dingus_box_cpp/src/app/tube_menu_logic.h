// tube_menu_logic.h - the Settings menu's decisions about the Tube engine: which
// Display rows show for the chosen engine, how the CRT Engine row cycles, and the
// words every Tube row wears (label and sublabel).
//
// Every `bool offered` below is app::tube_offered(state) at the call site (the
// Pi 5 board gate AND the renderer-ready flag); nothing here reads the profile.
// With `offered` false a saved Tube behaves as Enhanced everywhere: the Classic /
// Enhanced rows show, the cycle never lands on Tube, the label says Enhanced.
//
// Pure: Renderer-free, I/O-free, header-only (test_ui_unit needs no new source).
#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.h"
#include "app/tube_not_drawn.h"
#include "tube/tube_frame_class.h"
#include "tube/tube_groups.h"
#include "tube/tube_inputs.h"
#include "tube/tube_rows.h"
#include "tube/tube_setting.h"

namespace app {

// Which Display-menu rows show: the seven Classic/Enhanced CRT rows, or the Tube
// rows. Never both, never neither.
struct DisplayCrtRows {
    bool classic;
    bool tube;
};

inline DisplayCrtRows display_crt_rows(CrtEngine saved, bool offered) noexcept {
    const bool tube = effective_crt_engine(saved, offered) == CrtEngine::Tube;
    return {!tube, tube};
}

// The CRT Engine row's next choice, stepped from the EFFECTIVE engine:
// Classic -> Enhanced -> Tube (offered) or -> Classic (not offered); Tube -> Classic.
inline CrtEngine next_crt_engine(CrtEngine saved, bool offered) noexcept {
    switch (effective_crt_engine(saved, offered)) {
        case CrtEngine::Classic:  return CrtEngine::Enhanced;
        case CrtEngine::Enhanced: return offered ? CrtEngine::Tube : CrtEngine::Classic;
        case CrtEngine::Tube:     return CrtEngine::Classic;
    }
    return CrtEngine::Classic;
}

inline std::string crt_engine_label(CrtEngine saved, bool offered) {
    return "CRT Engine: " + std::string(crt_engine_word(effective_crt_engine(saved, offered)));
}

inline std::string_view crt_engine_sublabel(bool offered) noexcept {
    return offered ? "Classic / Enhanced / Tube" : "Classic / Enhanced";
}

// The look after `look` in kLooks, wrapping.
inline tube::Look next_look(tube::Look look) noexcept {
    const std::size_t n = tube::kLooks.size();
    const auto i = static_cast<std::size_t>(look);
    return tube::kLooks[i < n ? (i + 1) % n : 0];
}

// Characters (UTF-8 code points) in `s`: the body font is monospaced, so this is width.
inline std::size_t utf8_length(std::string_view s) noexcept {
    std::size_t n = 0;
    for (const char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) ++n;
    return n;
}

// THE LOOK LABEL'S ROW BUDGET, in characters. The label of the SELECTED row is drawn at
// theme font_medium_size (18 px) in the body font, DejaVu Sans Mono: 1233/2048 em advance,
// scaled by stb_truetype to an 18 px ascent+descent of 2384 units = 9.31 px a character.
// renderer_settings.cpp: the menu is half the UI canvas wide and the text starts 35 px in,
// so a label has (canvas_w / 2 - 35) px before the screen edge, and nothing truncates it:
//   Modern TV, a 960 px canvas (the 4:3 content viewport): 445 px / 9.31 = 47.8 -> 47
//   CRT Native, a 640 px canvas: 285 px / 9.31 = 30.6 -> 30
inline constexpr std::size_t kLookLabelMaxCharsModernTv = 47;
inline constexpr std::size_t kLookLabelMaxCharsCrtNative = 30;
inline constexpr std::size_t look_label_max_chars(bool crt_native) noexcept {
    return crt_native ? kLookLabelMaxCharsCrtNative : kLookLabelMaxCharsModernTv;
}

// "Look: TV · 30 fps", "Look: TV (2 moved) · 30 fps", "Look: TV · 30 fps · films:
// Enhanced". The rate is the frame class of the setting as it stands; `crt_native` is the
// display mode (the rows a real tube draws itself count as neutral there, so the rate can
// differ). `film_engine` (app::tube_film_engine): when full-screen films get Enhanced.
// ALWAYS FITS look_label_max_chars(crt_native): the first of these that fits is used -
//   1. everything: "Look: <look> (<n> moved) · <r> fps · films: Enhanced"
//   2. "(<n> moved)" shortened to "(+<n>)"
//   3. the moved count dropped
//   4. the films note abbreviated to " · films: Enh."
//   5. the rate dropped
//   6. the films note dropped ("Look: Everything", 16, fits both budgets)
// The rate and the films note stay longest: they say what the box does; the moved count is
// on the Reset row's sublabel too.
inline std::string tube_look_label(const tube::TubeSetting& setting, bool crt_native,
                                   std::optional<CrtEngine> film_engine = std::nullopt) {
    const std::string base = "Look: " + std::string(tube::look_choice(setting.look).label);
    const std::size_t moved = tube::moved_keys(setting).size();
    const int fps = tube::frames_per_second(tube::setting_frame_class(tube::resolve(setting), crt_native));
    const std::string rate = " \xC2\xB7 " + std::to_string(fps) + " fps";
    const bool films = film_engine == CrtEngine::Enhanced;
    const std::string moved_long = moved > 0 ? " (" + std::to_string(moved) + " moved)" : "";
    const std::string moved_short = moved > 0 ? " (+" + std::to_string(moved) + ")" : "";
    const std::string films_long = films ? " \xC2\xB7 films: Enhanced" : "";
    const std::string films_short = films ? " \xC2\xB7 films: Enh." : "";
    const std::size_t budget = look_label_max_chars(crt_native);
    for (const std::string& label : {base + moved_long + rate + films_long, base + moved_short + rate + films_long,
                                     base + rate + films_long, base + rate + films_short, base + films_short}) {
        if (utf8_length(label) <= budget) return label;
    }
    return base;
}

// "30 fps" for a row that forces the 30 fps class when moved off neutral, else "".
inline std::string_view tube_rate_tag(tube::Effect effect) noexcept {
    return tube::forces_30fps(effect) ? "30 fps" : "";
}

// Whether a row's effect is one the box is not drawing (app::tube_not_drawn): exactly the
// rows tube::not_drawn_rows lists (the status publishes the same list for the web page).
inline bool tube_row_not_drawn(tube::Effect effect, const tube::NotDrawn& not_drawn) noexcept {
    return tube::not_drawn_rows(not_drawn).contains(effect);
}

// The note such a row wears. Shortened from "not drawn at this size": with it, GLOW's
// sublabel ran to 62 characters against 47 for the longest elsewhere in the menu. The
// web page, which has the room, says "Not drawn at this picture size".
inline constexpr std::string_view kTubeNotDrawnTag = "not drawn";

// A value row's sublabel: the row's hint, " · 30 fps" when the row forces the 30 fps
// class, then " · not drawn" when its effect is not being drawn.
inline std::string tube_value_sublabel(tube::Effect effect, const tube::NotDrawn& not_drawn) {
    std::string out(tube::row_info(effect).hint1);
    const std::string_view rate_tag = tube_rate_tag(effect);
    if (!rate_tag.empty()) out += " \xC2\xB7 " + std::string(rate_tag);
    if (tube_row_not_drawn(effect, not_drawn)) out += " \xC2\xB7 " + std::string(kTubeNotDrawnTag);
    return out;
}

// Whether a Degauss press counts: only while the tube draws the picture on screen
// (app::tube_engine_on_screen is Tube) - the web page greys its button on the same answer
// (`effective_engine`). Otherwise the TV shows the row greyed with kTubeDegaussOffSublabel.
inline bool tube_degauss_live(const AppState& state) noexcept {
    return tube_engine_on_screen(state) == CrtEngine::Tube;
}
inline constexpr std::string_view kTubeDegaussOffSublabel = "the tube is not drawing now";

// The six look words, in kLooks order, joined by " · ".
inline std::string tube_look_sublabel() {
    std::string out;
    for (const tube::Look look : tube::kLooks) {
        if (!out.empty()) out += " · ";
        out += tube::look_choice(look).label;
    }
    return out;
}

// The Reset row's sublabel: "nothing moved", "1 row moved", "3 rows moved".
inline std::string tube_reset_sublabel(const tube::TubeSetting& setting) {
    const std::size_t moved = tube::moved_keys(setting).size();
    if (moved == 0) return "nothing moved";
    return std::to_string(moved) + (moved == 1 ? " row moved" : " rows moved");
}

// A group's heading in sentence case: COLOUR PURITY -> Colour purity.
inline std::string tube_group_label(const tube::Group& group) {
    std::string out(group.label);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto c = static_cast<unsigned char>(out[i]);
        out[i] = static_cast<char>(i == 0 ? std::toupper(c) : std::tolower(c));
    }
    return out;
}

// "<n> rows" (the MASK row counts), plus " · <m> moved" when any of them is moved.
inline std::string tube_group_sublabel(const tube::Group& group, const tube::TubeSetting& setting) {
    const std::size_t rows = group.rows.size() + (group.has_mask_row ? 1 : 0);
    const std::vector<std::string_view> moved_keys = tube::moved_keys(setting);
    const auto is_moved = [&moved_keys](std::string_view key) {
        for (const std::string_view k : moved_keys)
            if (k == key) return true;
        return false;
    };
    std::size_t moved = (group.has_mask_row && is_moved("mask")) ? 1 : 0;
    for (const tube::Effect effect : group.rows)
        if (is_moved(tube::effect_key(effect))) ++moved;
    std::string out = std::to_string(rows) + " rows";
    if (moved > 0) out += " · " + std::to_string(moved) + " moved";
    return out;
}

// What a row in a group is: a 0..100 value, the MASK word, or n/a because a real
// tube (CRT Native) draws it itself. `effect` nullopt is the MASK row.
enum class TubeRowKind { Value, Mask, NotApplicable };

inline TubeRowKind tube_row_kind(std::optional<tube::Effect> effect, bool crt_native) noexcept {
    if (!effect) return crt_native ? TubeRowKind::NotApplicable : TubeRowKind::Mask;
    return (crt_native && tube::neutral_in_crt_native(*effect)) ? TubeRowKind::NotApplicable
                                                               : TubeRowKind::Value;
}

// "HUM BAR: 20"; while editing "HUM BAR: < 20 >".
inline std::string tube_value_label(tube::Effect effect, int value, bool editing) {
    const std::string number = std::to_string(value);
    return std::string(tube::row_info(effect).label) + ": " +
           (editing ? "< " + number + " >" : number);
}

// "MASK: SLOT"; while editing "MASK: < SLOT >".
inline std::string tube_mask_label(tube::Mask mask, bool editing) {
    const std::string word(tube::mask_choice(mask).label);
    return std::string(tube::kMaskRowInfo.label) + ": " + (editing ? "< " + word + " >" : word);
}

// "SCANLINES: n/a", with kTubeNaSublabel beneath it.
inline std::string tube_na_label(std::string_view row_label) {
    return std::string(row_label) + ": n/a";
}

inline constexpr std::string_view kTubeNaSublabel = "a real tube draws this itself";

}  // namespace app
