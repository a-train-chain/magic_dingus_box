#pragma once

#include <json/json.h>

#include "app/app_state.h"

namespace app {

// The DisplaySettings <-> settings.json mapping for the four per-mode
// discover filter sets and the Movies/TV mode. Split out of
// settings_persistence.cpp so the legacy-key migration is unit-testable:
// this TU touches jsoncpp and nothing else — no filesystem, no env, no
// logging — and is therefore linkable into test_media_browser_unit.
//
// KEY COMPATIBILITY CONTRACT. The Movies sets keep the pre-2c key names
// (display.mb_popular_*, display.mb_toprated_*) with identical string
// encodings, so a live box's settings.json round-trips its movie filters
// with zero loss. TV adds display.mb_tv_popular_* / display.mb_tv_toprated_*
// and the mode adds display.mb_mode ("movies" | "tv"). Every key is
// absent-tolerant on load.
//
// include_tv is false in ENABLE_MEDIA_BROWSER=OFF builds so their
// settings.json output stays byte-identical to pre-2c output.
void mb_filters_to_json(const AppState::DisplaySettings& s,
                        Json::Value& display,
                        bool include_tv);

// Absent keys leave the corresponding field at its constructed default;
// an absent mb_mode resets the mode to Movies.
void mb_filters_from_json(const Json::Value& display,
                          AppState::DisplaySettings& s);

}  // namespace app
