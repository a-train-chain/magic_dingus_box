#pragma once

#include <cstdint>
#include <vector>
#include <memory>

// Forward declare libgpiod types to avoid header dependency
struct gpiod_chip;
struct gpiod_line_request;

namespace platform {

// Reuse InputEvent from input_manager.h
struct InputEvent;
enum class InputAction;

// GPIO Pin Definitions (BCM numbering)
namespace gpio {
    // Rotary Encoder
    constexpr int ENCODER_CLK = 17;    // Quadrature signal A
    constexpr int ENCODER_DT = 27;     // Quadrature signal B
    constexpr int ENCODER_SW = 22;     // Push button
    
    // Restart Button (restarts app service, not full reboot)
    // Note: Power switch is on GPIO3, handled by hardware device tree overlay
    constexpr int RESTART_BTN = 24;    // Press to restart app service
    
    // Illuminated Button Switches (active low)
    constexpr int BTN1_SW = 5;         // Yellow - Previous/Rewind
    constexpr int BTN2_SW = 6;         // Red - Play/Pause
    constexpr int BTN3_SW = 13;        // Green - Next/Fast Forward
    constexpr int BTN4_SW = 19;        // Black - Settings Menu
    
    // Button LEDs (active high, accent lighting)
    constexpr int LED1 = 12;           // Yellow button LED
    constexpr int LED2 = 16;           // Red button LED
    constexpr int LED3 = 26;           // Green button LED
    constexpr int LED4 = 20;           // Black button LED
    
    // Arrays for iteration (inline for ODR compliance in C++17)
    inline constexpr int BUTTON_PINS[] = {BTN1_SW, BTN2_SW, BTN3_SW, BTN4_SW};
    inline constexpr int LED_PINS[] = {LED1, LED2, LED3, LED4};
    inline constexpr int NUM_BUTTONS = 4;
}

class GpioManager {
public:
    GpioManager();
    ~GpioManager();
    
    // Initialize GPIO - returns false if GPIO not available (e.g., not on Pi)
    bool initialize();
    
    // Poll for input events (non-blocking)
    // Returns events in same format as InputManager for easy integration
    std::vector<InputEvent> poll();
    
    // LED control
    void set_led(int index, bool on);  // index 0-3
    void set_all_leds(bool on);
    
    // LED Animation methods
    // Run intro dance pattern - call repeatedly from main loop during intro
    // Returns true while animation is running, false when complete
    void update_intro_animation(uint64_t elapsed_ms);
    
    // Run shutdown flicker effect (blocking, ~2 seconds)
    void play_shutdown_animation();
    
    // Stop any running animation and turn off LEDs
    void stop_animation();
    
    // Stop the boot LED sequence service (call when app starts)
    void stop_boot_led_sequence();
    
    // Check if GPIO is available (initialized successfully)
    bool is_available() const { return available_; }
    
    // Cleanup
    void cleanup();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool available_ = false;
    
    // Debounce state
    struct ButtonState {
        bool last_state = true;  // true = released (pull-up)
        uint64_t last_change_time = 0;
        static constexpr uint64_t DEBOUNCE_MS = 50;
    };
    ButtonState button_states_[gpio::NUM_BUTTONS];
    ButtonState encoder_sw_state_;
    
    // Encoder state
    int last_clk_state_ = 1;
    
    // Restart button state
    ButtonState restart_btn_state_;
    
    // Helper to get current time in milliseconds
    uint64_t get_time_ms() const;
    
    // Read GPIO line state
    int read_line(int gpio);
    
    // Check restart button and restart service if pressed
    void check_restart_button();
};

} // namespace platform
