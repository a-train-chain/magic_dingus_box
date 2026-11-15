#pragma once

// Shared schedule value, independent of AppState and guide navigation.
namespace app {

struct OnAir {
    int index = 0;         // index into Channel::items
    double offset_s = 0;   // seconds into that item
    double starts_at = 0;  // UTC seconds the item started
    double ends_at = 0;    // UTC seconds the item ends (possibly cut)
    double day_ends_at = 0;  // civil broadcast close: live playback cuts even if it lags
};

}  // namespace app
