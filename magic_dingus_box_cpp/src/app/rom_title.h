#pragma once

#include <string>

namespace app {

// Turn a ROM path into the title the kiosk should DISPLAY.
//
// Playlists normally carry an explicit `title:`, and that always wins. This
// is the fallback for items that don't (a bare path in a playlist, or a ROM
// dropped in through the Content Manager), where the title would otherwise
// be the raw filename stem — "Super Mario 64 (USA)", region tag and all.
//
// Strips only KNOWN No-Intro / GoodTools tags (region, language list,
// revision, version, dump flags) and bracketed flags. A parenthetical that
// isn't a recognized tag is treated as part of the title and left alone —
// over-stripping would silently corrupt names like "Donkey Kong (Original)".
//
// Also applies the two conventions the curated playlists already use:
// "Zelda, The" -> "The Zelda", and " - " subtitle separators -> ": ".
std::string title_from_rom_path(const std::string& rom_path);

}  // namespace app
