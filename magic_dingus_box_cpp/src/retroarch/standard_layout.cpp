#include "standard_layout.h"

#include <algorithm>
#include <vector>

#include "joydev_index.h"

namespace retroarch {

namespace {

using K = PhysicalBinding::Kind;
using L = LogicalControl;

// evdev codes (linux/input-event-codes.h), spelled out because the header
// does not exist on macOS.
constexpr uint16_t kBtnSouth = 0x130, kBtnEast = 0x131, kBtnNorth = 0x133,
                   kBtnWest = 0x134, kBtnTl = 0x136, kBtnTr = 0x137,
                   kBtnTl2 = 0x138, kBtnTr2 = 0x139, kBtnSelect = 0x13a,
                   kBtnStart = 0x13b, kBtnThumbL = 0x13d, kBtnThumbR = 0x13e;
constexpr uint16_t kBtnDpadUp = 0x220, kBtnDpadDown = 0x221,
                   kBtnDpadLeft = 0x222, kBtnDpadRight = 0x223;
// xpad's dpad_to_buttons: TRIGGER_HAPPY1..4 = left, right, up, down.
constexpr uint16_t kHappy1 = 0x2c0, kHappy2 = 0x2c1, kHappy3 = 0x2c2,
                   kHappy4 = 0x2c3;
constexpr uint16_t kAbsX = 0x00, kAbsY = 0x01, kAbsZ = 0x02, kAbsRx = 0x03,
                   kAbsRy = 0x04, kAbsRz = 0x05, kAbsGas = 0x09,
                   kAbsBrake = 0x0a, kAbsHat0X = 0x10, kAbsHat0Y = 0x11;

constexpr uint16_t kVidMicrosoft = 0x045e;

enum class Family { NONE, POSITIONAL, XBOX };

Family family_of(const std::string& driver, uint16_t vid) {
    if (driver == "playstation" || driver == "sony" || driver == "nintendo") {
        return Family::POSITIONAL;
    }
    if (driver == "xpad" || driver == "xpadneo") return Family::XBOX;
    // Xbox over Bluetooth: hid-microsoft where it binds, the generic HID
    // mapping where it does not. The VID is the stable signal.
    if (vid == kVidMicrosoft &&
        (driver == "microsoft" || driver == "hid-generic")) {
        return Family::XBOX;
    }
    return Family::NONE;
}

struct Pad {
    const std::vector<uint16_t>& keys;
    const std::vector<uint16_t>& abs;
    bool key(uint16_t c) const {
        return std::binary_search(keys.begin(), keys.end(), c);
    }
    bool axis(uint16_t c) const {
        return std::binary_search(abs.begin(), abs.end(), c);
    }
};

std::optional<PhysicalProfile> build(const std::string& driver, uint16_t vid,
                                     uint16_t pid, const std::string& name,
                                     std::vector<uint16_t> keys,
                                     std::vector<uint16_t> abs) {
    const Family family = family_of(driver, vid);
    if (family == Family::NONE) return std::nullopt;

    std::sort(keys.begin(), keys.end());
    std::sort(abs.begin(), abs.end());
    const Pad pad{keys, abs};

    // The modern full-size button set. Anything less goes to the wizard.
    for (uint16_t c : {kBtnSouth, kBtnEast, kBtnNorth, kBtnWest, kBtnTl, kBtnTr,
                       kBtnSelect, kBtnStart}) {
        if (!pad.key(c)) return std::nullopt;
    }
    const bool hat = pad.axis(kAbsHat0X) && pad.axis(kAbsHat0Y);
    const bool dpad_btns = pad.key(kBtnDpadUp) && pad.key(kBtnDpadDown) &&
                           pad.key(kBtnDpadLeft) && pad.key(kBtnDpadRight);
    const bool happy_btns = pad.key(kHappy1) && pad.key(kHappy2) &&
                            pad.key(kHappy3) && pad.key(kHappy4);
    if (!hat && !dpad_btns && !happy_btns) return std::nullopt;

    PhysicalProfile p;
    p.name = name;
    p.style = ControllerStyle::PS_STYLE;
    p.vid = vid;
    p.pid = pid;

    auto put = [&](L control, K kind, uint16_t code, int dir) {
        PhysicalBinding b;
        b.kind = kind;
        b.code = code;
        b.direction = dir;
        b.token = bind_token(keys, abs, kind, code, dir);
        if (!b.token.empty()) p.controls[control] = b;
    };
    auto button = [&](L control, uint16_t code) {
        if (pad.key(code)) put(control, K::BUTTON, code, 0);
    };

    // Face buttons by POSITION.
    const bool xbox = family == Family::XBOX;
    button(L::CROSS, kBtnSouth);
    button(L::CIRCLE, kBtnEast);
    button(L::TRIANGLE, xbox ? kBtnWest : kBtnNorth);   // top
    button(L::SQUARE, xbox ? kBtnNorth : kBtnWest);     // left
    button(L::L1, kBtnTl);
    button(L::R1, kBtnTr);
    button(L::SELECT, kBtnSelect);
    button(L::START, kBtnStart);
    button(L::L3, kBtnThumbL);
    button(L::R3, kBtnThumbR);

    // Triggers. Xbox pads report their (analog) triggers only as axes; on
    // Bluetooth LE the BTN_TL2/TR2 codes exist as unused report slots that
    // never fire, so they must not be preferred there.
    const bool rx_ry = pad.axis(kAbsRx) && pad.axis(kAbsRy);
    if (xbox) {
        if (rx_ry && pad.axis(kAbsZ) && pad.axis(kAbsRz)) {          // xpad
            put(L::L2, K::AXIS, kAbsZ, +1);
            put(L::R2, K::AXIS, kAbsRz, +1);
        } else if (pad.axis(kAbsBrake) && pad.axis(kAbsGas)) {       // BLE
            put(L::L2, K::AXIS, kAbsBrake, +1);
            put(L::R2, K::AXIS, kAbsGas, +1);
        } else {
            button(L::L2, kBtnTl2);
            button(L::R2, kBtnTr2);
        }
    } else {
        button(L::L2, kBtnTl2);
        button(L::R2, kBtnTr2);
    }

    // D-pad.
    if (hat) {
        put(L::DPAD_UP, K::HAT, kAbsHat0Y, -1);
        put(L::DPAD_DOWN, K::HAT, kAbsHat0Y, +1);
        put(L::DPAD_LEFT, K::HAT, kAbsHat0X, -1);
        put(L::DPAD_RIGHT, K::HAT, kAbsHat0X, +1);
    } else if (dpad_btns) {
        button(L::DPAD_UP, kBtnDpadUp);
        button(L::DPAD_DOWN, kBtnDpadDown);
        button(L::DPAD_LEFT, kBtnDpadLeft);
        button(L::DPAD_RIGHT, kBtnDpadRight);
    } else {
        button(L::DPAD_LEFT, kHappy1);
        button(L::DPAD_RIGHT, kHappy2);
        button(L::DPAD_UP, kHappy3);
        button(L::DPAD_DOWN, kHappy4);
    }

    // Sticks.
    if (pad.axis(kAbsX) && pad.axis(kAbsY)) {
        put(L::LSTICK_UP, K::AXIS, kAbsY, -1);
        put(L::LSTICK_DOWN, K::AXIS, kAbsY, +1);
        put(L::LSTICK_LEFT, K::AXIS, kAbsX, -1);
        put(L::LSTICK_RIGHT, K::AXIS, kAbsX, +1);
    }
    uint16_t rx = 0, ry = 0;
    bool right = false;
    if (rx_ry) {
        rx = kAbsRx; ry = kAbsRy; right = true;
    } else if (xbox && pad.axis(kAbsZ) && pad.axis(kAbsRz) &&
               pad.axis(kAbsBrake) && pad.axis(kAbsGas)) {
        rx = kAbsZ; ry = kAbsRz; right = true;   // Xbox BLE right stick
    }
    if (right) {
        put(L::RSTICK_UP, K::AXIS, ry, -1);
        put(L::RSTICK_DOWN, K::AXIS, ry, +1);
        put(L::RSTICK_LEFT, K::AXIS, rx, -1);
        put(L::RSTICK_RIGHT, K::AXIS, rx, +1);
    }
    return p;
}

}  // namespace

std::optional<PhysicalProfile> standard_profile(const CaptureDeviceCaps& caps) {
    std::vector<uint16_t> abs;
    abs.reserve(caps.axes.size());
    for (const auto& a : caps.axes) abs.push_back(a.code);
    return build(caps.driver, caps.vid, caps.pid, caps.name, caps.key_codes,
                 std::move(abs));
}

std::optional<PhysicalProfile> standard_profile(const DetectedPad& pad) {
    return build(pad.driver, pad.vid, pad.pid, pad.name, pad.key_codes,
                 pad.abs_codes);
}

platform::MenuNavOverlay standard_menu_overlay(const PhysicalProfile& profile) {
    platform::MenuNavOverlay o = menu_overlay_from_profile(profile);
    for (L c : {L::SQUARE, L::SELECT, L::L2, L::R2, L::L3, L::R3}) {
        const PhysicalBinding* b = profile.binding(c);
        if (b && b->kind == K::BUTTON) o.buttons.emplace(b->code, platform::InputAction::NONE);
    }
    return o;
}

}  // namespace retroarch
