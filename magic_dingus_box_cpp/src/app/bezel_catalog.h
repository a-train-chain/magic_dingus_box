#pragma once

#include <string>
#include <vector>

#include "app/app_state.h"

namespace app {

// The size every bezel PNG is drawn from. The art is stretched over the
// whole framebuffer, so an opening maps to the screen with
// video::scale_image_rect(opening, kBezelImageWidth, kBezelImageHeight, ...).
constexpr int kBezelImageWidth = 1920;
constexpr int kBezelImageHeight = 1080;

// bezels.json -> the Settings menu's bezel list, in file order.
//
// "file" stays the bare file name ("" for a null or absent file — the
// procedural bezel, which draws nothing). An entry without a name is
// skipped, and so is anything in the array that is not an object.
//
// "opening": {"x", "y", "w", "h"} sets has_opening only when all four are
// integers describing a non-empty rect inside the 1920x1080 image;
// otherwise the entry still loads and the video uses the whole 4:3 area.
//
// Malformed JSON, or a root without a "bezels" array, gives an empty list.
std::vector<BezelInfo> parse_bezel_catalog(const std::string& json_text);

}  // namespace app
