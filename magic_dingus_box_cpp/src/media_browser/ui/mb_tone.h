#pragma once

// A text color named by ROLE rather than by value, so the pure view-model
// headers (series_detail_view.h, detail_view.h, ...) can say "this line is
// the warning color" without a Renderer or a constructed ::ui::Theme in
// sight. The screen maps a tone to the live theme with tone_color() at paint
// time. Header-only and test-safe: ui/theme.h is plain data.

#include "ui/theme.h"

namespace media_browser::ui {

enum class MbTone { Fg, Dim, Accent, Highlight1, Highlight2 };

inline const ::ui::Color& tone_color(const ::ui::Theme& th, MbTone t) {
    switch (t) {
        case MbTone::Fg:         return th.fg;
        case MbTone::Dim:        return th.dim;
        case MbTone::Accent:     return th.accent;
        case MbTone::Highlight1: return th.highlight1;
        case MbTone::Highlight2: return th.highlight2;
    }
    return th.fg;  // unreachable; keeps -Wreturn-type quiet without a default:
}

}  // namespace media_browser::ui
