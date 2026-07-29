#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <memory>

#include "../retroarch/capture_device_caps.h"
// InputAction + MenuNavOverlay. Both live in a std-only leaf header so
// retroarch/controller_profile.h can declare menu_overlay_from_profile()
// without pulling this whole class in; including it here keeps every existing
// user of platform::InputAction working unchanged.
#include "menu_nav_overlay.h"

namespace platform {

struct InputEvent {
    InputAction action;
    int delta;  // For ROTATE: -1 or +1
    bool pressed;  // For buttons: true on press, false on release
    float velocity = 0.0f;  // For ROTATE: 0.0 = slowest, 1.0 = fastest
#ifdef MEDIA_BROWSER_ENABLED
    // True when the SELECT event was produced by the rotary-encoder push
    // switch (as opposed to the controller A button / keyboard Enter).
    // Used by the Media Browser unlock sequence detector to distinguish
    // ROTARY_CLICK from regular SELECT — set only by GpioManager for the
    // encoder switch, default false for all other sources.
    bool is_from_rotary = false;
#endif
};

// One raw, unmapped evdev event from a joystick, surfaced only while
// set_raw_capture(true) is in effect. Shaped to feed
// retroarch::CaptureSession::feed() directly.
struct RawInputEvent {
    uint16_t vid = 0, pid = 0;
    std::string device_name;
    uint16_t type = 0;   // EV_KEY / EV_ABS
    uint16_t code = 0;
    int32_t value = 0;
};

class InputManager {
public:
    InputManager();
    ~InputManager();

    // Install the per-model menu-nav overlays derived by
    // retroarch::menu_overlay_from_profile(), keyed by (vid << 16) | pid.
    // Safe to call at any time, including while devices are already open:
    // it re-resolves every open device's overlay pointer against the NEW
    // map (the old map's nodes die with the assignment, so no Device may
    // keep pointing into it). Passing an empty map removes all overlays and
    // restores pure built-in behavior.
    void set_menu_overlays(std::map<uint32_t, MenuNavOverlay> overlays);

    // Raw capture (wizard): while enabled, joystick devices' events are
    // delivered via drain_raw_events() INSTEAD of being action-mapped.
    // Keyboards, the rotary encoder, and the phone-remote virtual pad
    // ("MagicDingus Phone Remote") keep producing InputActions so the
    // wizard chrome stays driveable.
    //
    // ORDERING REQUIREMENT -- DRAIN BEFORE YOU DISABLE. Every real state
    // change clears raw_events_, because the queue is per-capture-session and
    // events must never leak across an edge. So:
    //
    //     set_raw_capture(false);            // <-- discards the queue
    //     auto ev = drain_raw_events();      // <-- always empty
    //
    // silently loses the final capture, with no error and no log. Do this
    // instead:
    //
    //     auto ev = drain_raw_events();
    //     set_raw_capture(false);
    //
    // Repeating the CURRENT state is a no-op and keeps the queue intact
    // (set_raw_capture is idempotent on purpose, so a UI that calls it every
    // frame doesn't discard undrained events) -- only a genuine edge clears.
    void set_raw_capture(bool enabled);
    bool raw_capture() const { return raw_capture_; }

    // Take everything queued since the last drain, leaving the queue empty.
    // Safe to call at any time, including while capture is off -- but see the
    // ordering requirement on set_raw_capture(): disabling capture first
    // throws the queue away.
    std::vector<RawInputEvent> drain_raw_events();

    // Capability snapshot for the wizard's CaptureSession (nullopt if the
    // device isn't currently open). rest = current axis value at call time.
    std::optional<retroarch::CaptureDeviceCaps> device_caps(uint16_t vid, uint16_t pid);

    // Set how many EV_REL events equal one detent click on this board
    // (platform::PlatformProfile::rotary_events_per_detent). Values < 1
    // are ignored so a bad profile can't disable the encoder.
    void set_rotary_events_per_detent(int events_per_detent) {
        if (events_per_detent >= 1) rotary_events_per_detent_ = events_per_detent;
    }

    // Initialize - open evdev devices
    bool initialize();
    
    // Poll for input events (non-blocking)
    std::vector<InputEvent> poll();
    
    // Cleanup
    void cleanup();

    // Recover the phone-remote virtual gamepad after the web service
    // restarts. magic-dingus-web.service has Restart=always, and each
    // restart destroys and recreates the "MagicDingus Phone Remote" uinput
    // device with a NEW /dev/input/event node. Since initialize() only
    // scans once at startup, the kiosk would otherwise keep a dead grab on
    // the old node and the phone D-pad would go silent until a full kiosk
    // restart. Call this on a throttled cadence from the main loop: it
    // drops a dead phone-remote grab and (re)opens the current node. It
    // ONLY ever touches devices named "MagicDingus Phone Remote" — real USB
    // controllers, keyboards, and the rotary encoder are never disturbed.
    // Cheap and a no-op when the device is already healthy.
    void reprobe_phone_remote();

private:
    struct Device;
    std::vector<std::unique_ptr<Device>> devices_;
    
    bool open_joystick_devices();
    bool open_keyboard_devices();
    bool open_rotary_devices();
    InputAction map_button_to_action(uint16_t code, bool pressed);
    InputAction map_axis_to_action(uint8_t axis, int16_t value);
    InputAction map_key_to_action(uint16_t code);

    // The ROTATE decision for a main-stick X axis: deadzone + fire-on-change
    // + hold-to-repeat at ROTATE_REPEAT_HZ. Extracted from
    // map_axis_to_action's `axis == 0` branch so the overlay path
    // (arbitrary ABS code) and the hardcoded path (ABS_X) are literally the
    // same code and share last_rotate_dir_/last_rotate_time_ -- they can
    // never drift apart, and only one of the two ever runs per event.
    //
    // `deflection` is signed distance from the axis's REST CENTRE, not the raw
    // evdev value: an axis reporting 0..255 rests at 127, so comparing its raw
    // value against a symmetric deadzone would read "hard right" forever. The
    // hardcoded ABS_X path passes the raw value with AXIS_DEADZONE, which is
    // exactly what it always did (centre 0, threshold 5000); the overlay path
    // passes a value centred and a threshold scaled by the device's reported
    // min/max (Device::AxisNorm).
    InputAction rotate_from_axis_value(int32_t deflection, int32_t deadzone);

    // Deadzone for a signed-16-bit axis: ~15% of half-range. Also the
    // numerator of the proportional threshold derived for other ranges, so a
    // signed-16 axis normalizes back to exactly this value (see
    // cache_axis_ranges in the .cpp).
    static constexpr int32_t AXIS_DEADZONE = 5000;
    static constexpr int32_t AXIS_DEADZONE_OF = 32767;  // ...out of this many

    // Overlay for this vid/pid, or nullptr. The returned pointer is owned by
    // overlays_ and is invalidated by set_menu_overlays().
    const MenuNavOverlay* lookup_overlay(uint16_t vid, uint16_t pid) const;

    std::map<uint32_t, MenuNavOverlay> overlays_;   // (vid << 16) | pid

    bool raw_capture_ = false;
    std::vector<RawInputEvent> raw_events_;
    // Hard ceiling so a consumer that stops draining (wizard torn down
    // without clearing capture) can't grow this without bound.
    static constexpr size_t MAX_RAW_EVENTS = 4096;

    static constexpr const char* PHONE_REMOTE_NAME = "MagicDingus Phone Remote";

    // State tracking for axes/hats
    int last_rotate_dir_;
    double last_rotate_time_;
    static constexpr double ROTATE_REPEAT_HZ = 8.0;

    // D-pad hold-to-repeat
    static constexpr double DPAD_INITIAL_DELAY = 0.4;
    static constexpr double DPAD_REPEAT_SLOW_HZ = 8.0;
    static constexpr double DPAD_REPEAT_FAST_HZ = 20.0;
    static constexpr double DPAD_ACCEL_THRESHOLD = 1.0;

    int dpad_held_x_ = 0;
    int dpad_held_y_ = 0;
    double dpad_press_time_x_ = 0.0;
    double dpad_press_time_y_ = 0.0;
    double dpad_last_fire_x_ = 0.0;
    double dpad_last_fire_y_ = 0.0;

    void generate_dpad_repeats(std::vector<InputEvent>& events);

    // Rotary encoder state
    int rotary_accumulator_ = 0;
    // EV_REL events required per UI step — must match how many the
    // encoder emits per detent on THIS board. Defaults to the historical
    // Pi 4 value; main() overrides it from the detected PlatformProfile.
    int rotary_events_per_detent_ = 2;
    std::chrono::steady_clock::time_point last_rotary_event_time_;
};

} // namespace platform

