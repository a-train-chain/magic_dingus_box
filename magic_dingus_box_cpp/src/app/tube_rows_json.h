// app/tube_rows_json.h - the Tube rows as one JSON document for the web page.
//
// The TV menu reads its rows, words, groups and looks from the C++ tables; the web
// admin needs exactly the same data. This renders them, and the result is checked in
// as magic_dingus_box/web/static/tube_rows.json. test_tube_rows_json.cpp fails when
// the file differs from this output; the web suite's test_tube_rows_json.py fails
// when it differs from the headers' text, so the two front ends cannot disagree.
// Regenerate: MDB_WRITE_TUBE_ROWS_JSON=1 ./build/test_app_unit "[tube_rows_json]"
//
// Deterministic bytes: 2-space indentation, UTF-8 as-is (only " and \ escaped), a
// trailing newline. Pure: standard library only, no I/O.
#pragma once

#include <string>
#include <string_view>

#include "app/app_state.h"
#include "app/tube_menu_logic.h"
#include "tube/tube_frame_class.h"
#include "tube/tube_groups.h"
#include "tube/tube_inputs.h"
#include "tube/tube_looks.h"
#include "tube/tube_rows.h"

namespace app {

// The file's format version (the web page reads it; a test compares it to the file).
inline constexpr int kTubeRowsJsonVersion = 1;

namespace detail {

inline std::string json_str(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

inline std::string json_hint(std::string_view a, std::string_view b) {
    return "[" + json_str(a) + ", " + json_str(b) + "]";
}

inline std::string json_bool(bool b) { return b ? "true" : "false"; }

// A look's frame rate as it stands, on a non-CRT-Native output (the web page shows one
// number; CRT Native only ever lowers the class).
inline int look_fps(tube::Look look) {
    tube::TubeSetting setting;
    tube::select_look(setting, look);
    return tube::frames_per_second(tube::setting_frame_class(tube::resolve(setting), false));
}

}  // namespace detail

inline std::string tube_rows_json() {
    using detail::json_bool;
    using detail::json_hint;
    using detail::json_str;
    const std::string i2 = "  ", i4 = "    ", i6 = "      ", i8 = "        ";
    std::string o = "{\n";
    o += i2 + "\"version\": " + std::to_string(kTubeRowsJsonVersion) + ",\n";
    o += i2 + "\"row_min\": " + std::to_string(tube::kRowMin) + ",\n";
    o += i2 + "\"row_max\": " + std::to_string(tube::kRowMax) + ",\n";
    o += i2 + "\"default_look\": " + json_str(tube::look_name(tube::kLookDefault)) + ",\n";

    o += i2 + "\"engines\": [\n";
    for (std::size_t i = 0; i < detail::kCrtEngines.size(); ++i) {
        const CrtEngine e = detail::kCrtEngines[i];
        o += i4 + "{\"key\": " + json_str(crt_engine_name(e)) + ", \"word\": " +
             json_str(crt_engine_word(e)) + "}" + (i + 1 < detail::kCrtEngines.size() ? "," : "") + "\n";
    }
    o += i2 + "],\n";

    o += i2 + "\"rows\": [\n";
    for (std::size_t i = 0; i < tube::kEffectCount; ++i) {
        const tube::Effect e = tube::kEffects[i];
        const tube::RowInfo& info = tube::row_info(e);
        o += i4 + "{\"key\": " + json_str(tube::effect_key(e)) + ", \"label\": " + json_str(info.label) +
             ", \"hint\": " + json_hint(info.hint1, info.hint2) + ", \"step\": " +
             std::to_string(tube::row_step(e)) + ", \"crt_native_neutral\": " +
             json_bool(tube::neutral_in_crt_native(e)) + ", \"forces_30fps\": " +
             json_bool(tube::forces_30fps(e)) + "}" + (i + 1 < tube::kEffectCount ? "," : "") + "\n";
    }
    o += i2 + "],\n";

    o += i2 + "\"mask\": {\n";
    o += i4 + "\"label\": " + json_str(tube::kMaskRowInfo.label) + ",\n";
    o += i4 + "\"crt_native_neutral\": true,\n";
    o += i4 + "\"choices\": [\n";
    for (std::size_t i = 0; i < tube::kMasks.size(); ++i) {
        const tube::Mask m = tube::kMasks[i];
        const tube::RowInfo& c = tube::mask_choice(m);
        o += i6 + "{\"key\": " + json_str(tube::mask_name(m)) + ", \"word\": " + json_str(c.label) +
             ", \"hint\": " + json_hint(c.hint1, c.hint2) + "}" + (i + 1 < tube::kMasks.size() ? "," : "") + "\n";
    }
    o += i4 + "]\n";
    o += i2 + "},\n";

    o += i2 + "\"groups\": [\n";
    for (std::size_t g = 0; g < tube::kGroups.size(); ++g) {
        const tube::Group& group = tube::kGroups[g];
        std::string rows;
        if (group.has_mask_row) rows += json_str("mask");
        for (const tube::Effect e : group.rows) {
            if (!rows.empty()) rows += ", ";
            rows += json_str(tube::effect_key(e));
        }
        o += i4 + "{\"key\": " + json_str(group.key) + ", \"label\": " + json_str(group.label) +
             ", \"title\": " + json_str(tube_group_label(group)) + ", \"rows\": [" + rows + "]}" +
             (g + 1 < tube::kGroups.size() ? "," : "") + "\n";
    }
    o += i2 + "],\n";

    o += i2 + "\"looks\": [\n";
    for (std::size_t l = 0; l < tube::kLooks.size(); ++l) {
        const tube::Look look = tube::kLooks[l];
        const tube::RowInfo& c = tube::look_choice(look);
        const tube::LookValues& v = tube::look_values(look);
        o += i4 + "{\"key\": " + json_str(tube::look_name(look)) + ", \"word\": " + json_str(c.label) +
             ", \"hint\": " + json_hint(c.hint1, c.hint2) + ", \"mask\": " + json_str(tube::mask_name(v.mask)) +
             ", \"fps\": " + std::to_string(detail::look_fps(look)) + ",\n";
        o += i6 + "\"values\": {\n";
        for (std::size_t i = 0; i < tube::kEffectCount; ++i)
            o += i8 + json_str(tube::effect_key(tube::kEffects[i])) + ": " + std::to_string(v.effects[i]) +
                 (i + 1 < tube::kEffectCount ? "," : "") + "\n";
        o += i6 + "}\n";
        o += i4 + "}" + (l + 1 < tube::kLooks.size() ? "," : "") + "\n";
    }
    o += i2 + "]\n";
    o += "}\n";
    return o;
}

}  // namespace app
