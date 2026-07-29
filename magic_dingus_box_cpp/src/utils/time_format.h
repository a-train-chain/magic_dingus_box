#pragma once
#include <ctime>
#include <string>

namespace utils {

// Format a wall-clock instant as ISO-8601 UTC: "2001-09-09T01:46:40Z".
//
// Lives in utils/ rather than next to either caller because the two callers are
// retroarch/controller_profile (the Controller Setup wizard's captured_at
// stamp) and media_browser/ui/library_screen (the "recently added" cutoff), and
// pointing either at the other would couple two otherwise independent
// subsystems. It has no project dependencies — <ctime> and <string> only.
//
// UTC, not local: the output carries a literal trailing "Z", so a local-time
// value would not be merely imprecise but actively wrong — it claims to be UTC
// and compares against real UTC stamps as though it were. Both callers compare
// the result as a plain string, and boxes ship to whatever timezone the
// customer sets, so this cannot be left to strftime's localtime default.
//
// THE CONTRACT: the result is either empty, or EXACTLY 20 characters with every
// field zero-padded. Nothing in between is ever returned.
//
// That is what makes lexicographic order equal chronological order, and both
// callers lean on it: the wizard's stamps get compared as JSON strings, and
// library_screen does a bare `added_at >= cutoff` against Radarr's ISO-8601. It
// is enforced by a width check in the .cpp, not merely intended — do not
// "simplify" that check into `!= 0`, and do not change the format string in a
// way that drops padding.
//
// The check is on WIDTH rather than on a year range on purpose, because %Y is
// where the two libcs disagree. Measured 2026-07-29 for year 1:
//   Darwin/libc++  -> "0001-01-01T00:00:00Z"  (20 chars — zero-padded)
//   glibc/aarch64  -> "1-01-01T00:00:00Z"     (17 chars — NOT padded)
// Both produce 21 chars at year 10000. So the same instant is representable on
// one platform and not the other; a year-range contract would be a false claim
// on one of them, while "20 chars or nothing" is true on both.
//
// Takes the instant as a parameter rather than reading the clock, which is what
// makes the format assertable without a clock-injection seam.
//
// RETURNS "" when the instant cannot be rendered inside the contract — an
// out-of-range time_t, or a year whose width breaks the invariant on this
// platform. Callers MUST handle that explicitly rather than letting it flow into
// a comparison: an empty string compares less-than every non-empty one, which
// silently turns a date filter into a pass-everything filter. Conversely, any
// non-empty return IS safe to compare — that is the whole point of refusing
// off-width output instead of passing it through. See library_screen.cpp's
// rebuild_view() for the intended shape.
std::string iso8601_utc(std::time_t t);

}  // namespace utils
