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
    // True when the SELECT event was produced by the rotary-encoder push
    // switch (as opposed to the controller A button / keyboard Enter).
    // Used by the Media Browser unlock sequence detector to distinguish
    // ROTARY_CLICK from regular SELECT — set only by GpioManager for the
    // encoder switch, default false for all other sources.
    bool is_from_rotary = false;
};

class InputManager {
public:
    InputManager();
    ~InputManager();

    // Initialize - open evdev devices
    bool initialize();
    
    // Poll for input events (non-blocking)
    std::vector<InputEvent> poll();
    
    // Cleanup
    void cleanup();

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
    std::chrono::steady_clock::time_point last_rotary_event_time_;
};

} // namespace platform

