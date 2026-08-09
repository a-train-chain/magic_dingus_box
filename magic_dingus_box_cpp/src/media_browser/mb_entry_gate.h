#pragma once

// Media Browser display-mode entry gate.
//
// Every Media Browser screen is authored against the fixed 1280x720
// logical canvas (config::display::UI_LOGICAL_*): browse/search grids
// assume 9 columns at 1280, and the Library/filter slide-in panels park
// at x=740 with their closed position at x=1280. CRT_NATIVE runs a
// 640x480 logical canvas, where x=740 is not merely misaligned — it is
// off the canvas entirely, so the panels would open literally
// off-screen. The deliberate fix is a GATE, not a canvas-aware
// re-layout: the Movies feature simply requires the display mode that
// provides the 720p canvas.
//
// The predicate derives from logical_canvas() — the same source of
// truth main.cpp uses to size the renderer's logical canvas — never
// from a duplicated "is CRT" constant. If a future display mode ships
// with a different canvas, this gate follows it automatically.
//
// Pure logic, header-only, no I/O: unit-tested on the Mac in
// tests/media_browser/test_mb_entry_gate.cpp (same pattern as
// movie_drive.h / library_view.h).

#include "utils/config.h"

namespace media_browser {

// True when the display mode's logical canvas can host the Media
// Browser's fixed 1280x720 layout.
constexpr bool display_supports_media_browser(bool crt_native) {
    const config::display::Size c = config::display::logical_canvas(crt_native);
    return c.w == config::display::UI_LOGICAL_WIDTH &&
           c.h == config::display::UI_LOGICAL_HEIGHT;
}

// The Movies row's full decision, in precedence order. Extracted from
// SettingsMenuManager::open() so the ordering is a testable fact rather
// than an emergent property of an if/else chain.
enum class MoviesRowState {
    NeedsModernTv,    // display mode cannot host the MB layout
    NeedsVpnConfig,   // Layer 2: WIREGUARD_PRIVATE_KEY missing
    NeedsDrive,       // movie drive not mounted
    HiddenTunnelDown, // Layer 3: VPN unhealthy — row hidden, toast elsewhere
    Ready,            // actionable "Movies" row
};

// Precedence rationale:
//   1. Display mode first — it is the hardest blocker. In CRT mode the
//      feature cannot open at all, so pointing the owner at VPN config
//      or the drive would be a lie about what is actually in the way.
//   2. VPN configured before drive: unchanged relative order from the
//      shipped chain (configure VPN > drive not connected).
//   3. Drive before tunnel-health: a missing drive also takes Radarr
//      down, so both flags trip — but "connect the drive" is the
//      specific, actionable cause (see the comment at the original
//      branch in settings_menu.cpp).
//   4. Tunnel down hides the row entirely (toast is the signal).
constexpr MoviesRowState movies_row_state(bool crt_native,
                                          bool vpn_configured,
                                          bool storage_present,
                                          bool vpn_healthy) {
    if (!display_supports_media_browser(crt_native))
        return MoviesRowState::NeedsModernTv;
    if (!vpn_configured)  return MoviesRowState::NeedsVpnConfig;
    if (!storage_present) return MoviesRowState::NeedsDrive;
    if (!vpn_healthy)     return MoviesRowState::HiddenTunnelDown;
    return MoviesRowState::Ready;
}

// One string, three consumers (settings row, SELECT toast, unit test) —
// so the label the owner reads, the toast that explains a blocked
// SELECT, and the OWNER_GUIDE §10 entry can never drift apart.
inline constexpr const char* kMoviesNeedsModernTvLabel =
    "Movies (needs Modern TV display)";
inline constexpr const char* kMoviesNeedsModernTvSublabel =
    "Switch to Modern TV in Display settings";
inline constexpr const char* kMoviesNeedsModernTvToast =
    "Movies needs the Modern TV display mode — see Display settings";
// Shown when a display-mode change lands while the Media Browser is
// open (web-admin settings restore poke, or a Display-submenu toggle
// racing a stale top-level row) and the kiosk evicts back to the menu.
inline constexpr const char* kMoviesClosedByDisplaySwitchToast =
    "Display mode changed — Movies needs Modern TV";

}  // namespace media_browser
