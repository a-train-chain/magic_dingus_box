#pragma once

#include <cstdint>
#include <vector>

namespace platform {

// A candidate display mode, mirroring the fields of drmModeModeInfo that
// selection actually depends on. Kept free of libdrm types so the policy
// is unit-testable off-Pi.
struct ModeCandidate {
    uint32_t width;
    uint32_t height;
    uint32_t vrefresh;    // whole Hz, as drmModeModeInfo reports it
    bool is_preferred;    // DRM_MODE_TYPE_PREFERRED (the EDID-preferred mode)
};

// Choose which mode to set.
//
//   want_w/want_h == 0  -> "auto": take the display's preferred mode,
//                          else the highest refresh available.
//   otherwise           -> only modes of exactly that size are eligible;
//                          among them prefer the EDID-preferred one, then
//                          the highest refresh rate.
//
// Returns an index into `modes`, or -1 when nothing matches (callers must
// fall back rather than pick something arbitrary).
//
// WHY THIS EXISTS: the previous implementation matched on resolution and
// broke on the first hit, so refresh rate was decided by whatever order
// the kernel enumerated modes in. The bench TV advertises ELEVEN
// 1920x1080 modes (24/25/30/50/60 Hz and interlaced variants) and the
// first one listed is not necessarily 60 Hz. That mattered little at 720p
// where only one or two modes exist, but the kiosk's main loop blocks on
// the DRM page-flip event, so landing on 1080p24 or 1080p30 would clamp
// the ENTIRE UI — menus, video, animations — to that frame rate.
//
// Preferring the EDID-preferred mode ahead of raw refresh is deliberate:
// when a panel explicitly advertises a preferred timing at the size we
// asked for, that timing is the one most likely to be implemented
// correctly, and disagreeing with it invites sync problems on exactly the
// cheap displays a kiosk ends up attached to.
inline int pick_mode(const std::vector<ModeCandidate>& modes,
                     uint32_t want_w, uint32_t want_h) {
    const bool auto_mode = (want_w == 0 || want_h == 0);
    int best = -1;

    for (size_t i = 0; i < modes.size(); ++i) {
        const ModeCandidate& m = modes[i];

        if (!auto_mode && (m.width != want_w || m.height != want_h)) {
            continue;
        }
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }

        const ModeCandidate& b = modes[static_cast<size_t>(best)];

        // Preferred beats non-preferred outright.
        if (m.is_preferred != b.is_preferred) {
            if (m.is_preferred) best = static_cast<int>(i);
            continue;
        }
        // Same preferred-ness: higher refresh wins. Strictly greater, so
        // ties keep the earlier entry and selection stays stable.
        if (m.vrefresh > b.vrefresh) {
            best = static_cast<int>(i);
        }
    }

    return best;
}

} // namespace platform
