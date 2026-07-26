#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace platform {

enum class InputAction {
    NONE,
    ROTATE,           // Rotate playlist selection (Horizontal/General)
    ROTATE_VERTICAL,  // Rotate vertical (D-pad Up/Down)
    SELECT,           // Select/activate
    NEXT,             // Next track
    PREV,             // Previous track
    SEEK_LEFT,        // Seek backward
    SEEK_RIGHT,       // Seek forward
    PLAY_PAUSE,       // Toggle play/pause
    TOGGLE_LOOP,      // Toggle loop
    QUIT,             // Quit application
    ENTER_SAMPLE_MODE,
    EXIT_SAMPLE_MODE,
    MARKER_ACTION,
    UNDO_MARKER,
    SETTINGS_MENU     // Toggle settings menu
};

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

class InputManager {
public:
    InputManager();
    ~InputManager();

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

