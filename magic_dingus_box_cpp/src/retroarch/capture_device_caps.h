#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Deliberately DEPENDENCY-FREE (std only). This struct is produced by
// platform::InputManager::device_caps() and consumed by
// retroarch::CaptureSession, so it sits in a header both sides can include.
// It used to live in capture_session.h, but that header includes
// controller_profile.h -- which now includes platform/input_manager.h for
// MenuNavOverlay -- so having input_manager.h include capture_session.h
// would have closed an include cycle (controller_profile.h ->
// input_manager.h -> capture_session.h -> controller_profile.h). Keeping the
// shared struct in this leaf header breaks the cycle without duplicating the
// definition.

namespace retroarch {

// Everything the capture session needs to know about the target device,
// gathered once by InputManager when the wizard picks it.
struct CaptureDeviceCaps {
    uint16_t vid = 0, pid = 0;
    std::string name;
    std::vector<uint16_t> key_codes;                  // ascending EV_KEY codes
    struct AxisRange { uint16_t code; int min, max, rest; };
    std::vector<AxisRange> axes;                      // ascending ABS codes (incl. hats)
};

}  // namespace retroarch
