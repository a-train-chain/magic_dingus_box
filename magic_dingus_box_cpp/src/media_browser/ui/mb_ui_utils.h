#pragma once

// Pure helpers shared by every Marquee (Media Browser) screen.
//
// These four functions each existed as six or seven byte-similar copies in the
// screens' anonymous namespaces. That is how the "recently added" filter bug
// happened: two copies of one formatter drifted, the copy with the missing
// error check silently turned a date filter into a pass-everything filter, and
// no test could see either copy because both lived in translation units that
// only compile into the kiosk binary.
//
// Nothing here touches ::ui::Renderer. That is the whole point -- Renderer
// pulls in GLES, which does not exist on the mac dev box or in any test
// target, so a helper that names it cannot be tested. truncate_to_width is
// therefore parameterized on a measuring callable; the Renderer-shaped
// convenience overload with the signature the ~20 existing call sites already
// use lives in mb_chrome.h and is a one-line forward into the core below.
//
// ui/theme.h is safe to include from a test target: it declares ui::Color as
// four uint8_t channels over <cstdint> and <string> only, with inline
// constructors, so it neither drags in GL nor adds a link dependency.

#include <cstdint>
#include <functional>
#include <string>

#include "ui/theme.h"  // ::ui::Color

namespace media_browser::ui {

// Measures a string's rendered width in pixels at a given font size. The
// kiosk passes a lambda over ::ui::Renderer::mb_text_width; tests pass a
// deterministic fake so assertions are exact rather than font-dependent.
using TextMeasureFn = std::function<float(const std::string& text,
                                         int font_size)>;

// Shortens `text` so it fits `max_w` pixels at `font_size`, appending "..."
// when it has to cut.
//
// Behavior is preserved verbatim from the six copies this replaces, including
// the edge cases:
//   - text that already fits (measured <=, not <) is returned untouched;
//   - when even a single character plus the ellipsis will not fit, the bare
//     "..." is returned -- which does NOT itself fit, and the caller draws it
//     anyway. Callers rely on always getting something back.
//
// Known limitations, preserved deliberately rather than fixed here (changing
// them would be a behavior change, not a refactor):
//   - the scan is linear and calls `measure` once per byte, so a long synopsis
//     costs hundreds of text-width calls per frame where a binary search over
//     the monotonic width would cost ~10;
//   - the first probe is always wasted: it appends the ellipsis to the FULL
//     string, which is by definition wider than the string that just failed;
//   - the cut is by BYTE, so a multi-byte UTF-8 sequence can be split, leaving
//     an invalid trailing byte before the "...". Movie titles with accents and
//     the "…"/"•" glyphs the screens embed are exposed to this.
std::string truncate_to_width(const std::string& text, int font_size,
                              float max_w, const TextMeasureFn& measure);

// A stable, pseudo-random, mid-dark tint derived from any integer id.
//
// Used as the poster placeholder before the artwork cache has fetched the real
// image, so the same movie shows the same color on Browse, Search, Detail,
// Library and the playback overlay. Named for what it does rather than for one
// caller's domain: QueueScreen legitimately hashes a Radarr *queue* id (stable
// for the life of a download row even when Radarr renumbers movies between
// refreshes) while everyone else hashes a tmdb id.
//
// The channel bases and masks keep the result mid-dark and slightly purple, so
// the large title text the poster card draws on top always stays legible.
// Alpha is always 255. Negative ids are reinterpreted modularly, not clamped.
::ui::Color stable_tint_for_id(int id);

// Cubic slide easing for the panel animations. ease_out is the mirror of
// ease_in: ease_out(t) == 1 - ease_in(1 - t), which is what makes a slide-out
// retrace its slide-in instead of using a subtly heavier curve.
float ease_in_cubic(float t);
float ease_out_cubic(float t);

// Human-readable byte count: GB / MB / KB / B by magnitude, binary (1024)
// boundaries, one decimal below 100 of the chosen unit and none at or above it
// ("99.9 MB", "100 MB", "2.3 GB").
//
// CONTRACT: only meaningful for `bytes > 0`. The three former copies rendered
// `<= 0` three different ways -- "0 B" (Queue: genuinely nothing transferred
// yet), "?" (ReleasePicker: the indexer did not report a size) and a plain
// integer fallthrough (Library) -- and those are different MEANINGS, not
// drift. Unifying them would destroy information, so every caller keeps its
// own `<= 0` branch at the call site and must not rely on what this returns
// there. For safety it is defined rather than undefined: `<= 0` yields "0 B".
std::string format_bytes(int64_t bytes);

}  // namespace media_browser::ui
