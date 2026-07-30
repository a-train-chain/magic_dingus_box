#pragma once

// Pure helpers shared by every Marquee (Media Browser) screen.
//
// Every one of these existed as byte-similar copies in the screens' anonymous
// namespaces: truncate_to_width SIX times, the tint SIX (under three different
// names -- poster_tint_for_tmdb, library_tint_for_tmdb, tint_for_queue_id),
// ease_in_cubic and ease_out_cubic TWO each, and format_bytes THREE. That is
// how the "recently added" filter bug happened: two copies of one formatter
// drifted, the copy with the missing error check silently turned a date filter
// into a pass-everything filter, and no test could see either copy because
// both lived in translation units that only compile into the kiosk binary.
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
//
// ============================ READ THIS FIRST ============================
// DO NOT RE-DECLARE ANY OF THESE NAMES IN A SCREEN'S ANONYMOUS NAMESPACE.
//
// All five went from internal linkage (each screen's anonymous namespace) to
// external linkage in media_browser::ui. A screen that declares its own
// `format_bytes` or `ease_in_cubic` in its anonymous namespace SILENTLY WINS
// unqualified lookup at every call site in that file: the local declaration is
// found first, name lookup stops, and the shared definition is never
// considered. There is no warning and no link error, because both symbols are
// legitimate and only one is ever referenced -- so the exact drift class this
// header exists to eliminate comes right back, quietly, in one file. If a
// screen genuinely needs different behavior, give it a different NAME, or
// extend the shared helper here and test the extension.
// =========================================================================

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
// Contract, carried over unchanged from the six copies this replaces:
//   - text that already fits (measured <=, not <) is returned untouched;
//   - when even a single character plus the ellipsis will not fit, the bare
//     "..." is returned -- which does NOT itself fit, and the caller draws it
//     anyway. Callers rely on always getting something back.
//
// The cut lands on a UTF-8 CODEPOINT BOUNDARY. It used to be a raw byte cut,
// which split multi-byte sequences and left an orphaned lead byte before the
// "..."; ui::decode_utf8 turns that into U+FFFD, so the kiosk drew a
// replacement box. Accented titles ("Amélie", "Léon: The Professional"), CJK
// ("千と千尋の神隠し") and the "…"/"•" glyphs the screens themselves embed in
// composed metadata lines were all exposed, and truncation is the common case
// in the poster grid. Snapping back to a boundary gives up whatever partial
// sequence the byte budget landed in -- up to 3 bytes for a 4-byte emoji,
// which is why a tight budget on CJK can now come back as the bare "..."
// where it used to come back as one replacement box. That is the trade, and
// it is the right way round. `ui::utf8_floor_boundary` in ui/text_utf8.h is
// the primitive; anything else that shrinks a string to a pixel budget must
// use it too.
//
// ASSUMPTION -- rendered width is non-decreasing in prefix length. This is
// what licenses the binary search over boundaries (and lets the full-string
// probe be skipped, since the full string already failed). It holds for the
// left-to-right text the kiosk renders, where each glyph adds a non-negative
// advance. It would NOT hold under a shaper that applies negative kerning or
// RTL reordering across a cut; the kiosk has no shaping engine, so this is
// safe today and is the first thing to revisit if one is ever added.
//
// SCOPE -- boundaries are codepoints, NOT grapheme clusters. A combining
// accent (U+0065 U+0301) or an emoji ZWJ sequence can still be cut between
// its codepoints. Deliberate, on two grounds: full segmentation needs the
// UAX #29 property tables, which is a real dependency for a kiosk whose whole
// text stack is stb_truetype; and ui/font_manager.cpp already draws one glyph
// per codepoint with no cluster composition, so a combining mark renders as a
// separate spacing glyph whether or not it is cut. Cutting mid-cluster is
// therefore no worse on screen than cutting anywhere else, while cutting
// mid-codepoint produced an actual replacement box. The invalid-UTF-8 class of
// failure is gone; cluster fidelity only starts to pay once the font stack can
// compose clusters at all.
//
// COST -- O(log n) `measure` calls (a font/GL round-trip each on the kiosk),
// down from one per byte: a 1000-byte synopsis went from ~950 calls per frame
// to ~11, pinned by a call-counting test. Finding the boundaries is still one
// O(n) pass over the bytes, but that is plain pointer arithmetic, not text
// measurement.
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
//
// FOLLOW-UPS, recorded here rather than done (each is a separate change):
//   - mb_settings_screen.cpp's `format_gb_short` is a FIFTH copy of the same
//     `>= 100 ? "%.0f" : "%.1f"` rounding idiom, just hard-wired to GB for the
//     Storage row's "FREE: 124 GB" readout. It is the next consolidation
//     candidate -- most likely as a thin GB-only wrapper over this, since its
//     `<= 0` string ("0 GB") is its own caller-level decision in exactly the
//     way described above.
//   - there is NO TB unit, so a 2 TB library drive reads as "1863 GB"
//     (2e12 / 1024^3 = 1862.6, rounded by the >= 100 rule). That is faithful
//     to all three copies this replaces and is pinned by an existing test
//     (1 TiB -> "1024 GB"), so it is deliberate, not an oversight. Adding a
//     TB branch is a two-line change plus that test's expectation if drive
//     sizes ever make it worth doing.
std::string format_bytes(int64_t bytes);

}  // namespace media_browser::ui
