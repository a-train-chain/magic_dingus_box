#include "gpio_manager.h"
#include "input_manager.h"  // For InputEvent and InputAction

#ifdef HAVE_GPIOD
#include <gpiod.h>
#endif

#include <iostream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <unistd.h>

namespace platform {

#ifdef HAVE_GPIOD

// Implementation details hidden from header
struct GpioManager::Impl {
    struct gpiod_chip* chip = nullptr;
    struct gpiod_line_request* input_request = nullptr;
    struct gpiod_line_request* output_request = nullptr;
    
    ~Impl() {
        if (input_request) {
            gpiod_line_request_release(input_request);
        }
        if (output_request) {
            gpiod_line_request_release(output_request);
        }
        if (chip) {
            gpiod_chip_close(chip);
        }
    }
};

GpioManager::GpioManager() : impl_(std::make_unique<Impl>()) {
}

GpioManager::~GpioManager() {
    cleanup();
}

uint64_t GpioManager::get_time_ms() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

bool GpioManager::initialize() {
    std::cout << "  Initializing GPIO..." << std::endl;
    
    // Try to open the GPIO chip
    impl_->chip = gpiod_chip_open("/dev/gpiochip0");
    if (!impl_->chip) {
        std::cerr << "  Warning: Could not open /dev/gpiochip0 - GPIO not available" << std::endl;
        std::cerr << "  (This is normal if not running on Raspberry Pi)" << std::endl;
        available_ = false;
        return false;
    }
    
    // Configure input lines
    struct gpiod_line_settings* input_settings = gpiod_line_settings_new();
    if (!input_settings) {
        std::cerr << "  Error: Could not create input line settings" << std::endl;
        cleanup();
        return false;
    }
    
    gpiod_line_settings_set_direction(input_settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(input_settings, GPIOD_LINE_BIAS_PULL_UP);
    
    // Build input line config
    struct gpiod_line_config* input_config = gpiod_line_config_new();
    if (!input_config) {
        gpiod_line_settings_free(input_settings);
        cleanup();
        return false;
    }
    
    // Add all input GPIOs (GPIO3 not included - handled by device tree overlay for power)
    unsigned int input_offsets[] = {
        gpio::ENCODER_SW,
        gpio::RESTART_BTN,
        gpio::BTN1_SW, gpio::BTN2_SW, gpio::BTN3_SW, gpio::BTN4_SW
    };
    size_t num_inputs = sizeof(input_offsets) / sizeof(input_offsets[0]);
    
    int ret = gpiod_line_config_add_line_settings(input_config, input_offsets, num_inputs, input_settings);
    gpiod_line_settings_free(input_settings);
    
    if (ret < 0) {
        std::cerr << "  Error: Could not configure input lines" << std::endl;
        gpiod_line_config_free(input_config);
        cleanup();
        return false;
    }
    
    // Request input lines
    struct gpiod_request_config* req_config = gpiod_request_config_new();
    if (req_config) {
        gpiod_request_config_set_consumer(req_config, "magic-dingus-box");
    }
    
    impl_->input_request = gpiod_chip_request_lines(impl_->chip, req_config, input_config);
    gpiod_request_config_free(req_config);
    gpiod_line_config_free(input_config);
    
    if (!impl_->input_request) {
        std::cerr << "  Error: Could not request input GPIO lines" << std::endl;
        cleanup();
        return false;
    }
    
    // Configure output lines (LEDs)
    struct gpiod_line_settings* output_settings = gpiod_line_settings_new();
    if (!output_settings) {
        cleanup();
        return false;
    }
    
    gpiod_line_settings_set_direction(output_settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(output_settings, GPIOD_LINE_VALUE_INACTIVE);  // LEDs off initially
    
    struct gpiod_line_config* output_config = gpiod_line_config_new();
    if (!output_config) {
        gpiod_line_settings_free(output_settings);
        cleanup();
        return false;
    }
    
    unsigned int output_offsets[] = {gpio::LED1, gpio::LED2, gpio::LED3, gpio::LED4};
    size_t num_outputs = sizeof(output_offsets) / sizeof(output_offsets[0]);
    
    ret = gpiod_line_config_add_line_settings(output_config, output_offsets, num_outputs, output_settings);
    gpiod_line_settings_free(output_settings);
    
    if (ret < 0) {
        gpiod_line_config_free(output_config);
        cleanup();
        return false;
    }
    
    req_config = gpiod_request_config_new();
    if (req_config) {
        gpiod_request_config_set_consumer(req_config, "magic-dingus-box-leds");
    }
    
    impl_->output_request = gpiod_chip_request_lines(impl_->chip, req_config, output_config);
    gpiod_request_config_free(req_config);
    gpiod_line_config_free(output_config);
    
    if (!impl_->output_request) {
        std::cerr << "  Error: Could not request output GPIO lines" << std::endl;
        cleanup();
        return false;
    }
    
    // Initialize encoder state
    last_clk_state_ = read_line(gpio::ENCODER_CLK);
    
    // Note: Power switch on GPIO3 is handled by device tree overlay, not software
    
    available_ = true;
    std::cout << "  GPIO initialized successfully" << std::endl;
    std::cout << "    Inputs: Encoder (CLK=" << gpio::ENCODER_CLK << ", DT=" << gpio::ENCODER_DT 
              << ", SW=" << gpio::ENCODER_SW << ")" << std::endl;
    std::cout << "    Inputs: Buttons (5, 6, 13, 19), Restart Button (" << gpio::RESTART_BTN << ")" << std::endl;
    std::cout << "    Outputs: LEDs (12, 16, 26, 20)" << std::endl;
    std::cout << "    Power: GPIO3 (hardware controlled via device tree overlay)" << std::endl;
    
    return true;
}

int GpioManager::read_line(int gpio) {
    if (!impl_->input_request) return 1;  // Default to HIGH (released)
    
    enum gpiod_line_value value = gpiod_line_request_get_value(impl_->input_request, gpio);
    return (value == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
}

void GpioManager::check_restart_button() {
    int current = read_line(gpio::RESTART_BTN);
    bool pressed = (current == 0);  // Active low - LOW when pressed
    
    uint64_t now = get_time_ms();
    
    // Debounce and detect press event (not release)
    if (pressed != !restart_btn_state_.last_state) {
        if (now - restart_btn_state_.last_change_time >= ButtonState::DEBOUNCE_MS) {
            restart_btn_state_.last_state = !pressed;
            restart_btn_state_.last_change_time = now;
            
            if (pressed) {
                std::cout << "  Restart button pressed - playing shutdown animation..." << std::endl;
                
                // Play shutdown LED animation before restart
                play_shutdown_animation();
                
                std::cout << "  Restarting service..." << std::endl;
                
                // Restart the service (this process will be killed and restarted)
                std::system("sudo systemctl restart magic-dingus-box-cpp.service &");
            }
        }
    }
}

std::vector<InputEvent> GpioManager::poll() {
    std::vector<InputEvent> events;
    
    if (!available_) {
        return events;
    }
    
    uint64_t now = get_time_ms();
    
    // Check restart button
    check_restart_button();
    
    // ----- Rotary Encoder -----
    // Handled by kernel overlay (rotary-encoder) + libevdev in InputManager
    // We only handle the switch here
    
    // ----- Encoder Push Button -----
    {
        int current = read_line(gpio::ENCODER_SW);
        bool pressed = (current == 0);  // Active low
        
        if (pressed != !encoder_sw_state_.last_state) {
            if (now - encoder_sw_state_.last_change_time >= ButtonState::DEBOUNCE_MS) {
                encoder_sw_state_.last_state = !pressed;
                encoder_sw_state_.last_change_time = now;
                
                InputEvent ev;
                ev.action = InputAction::SELECT;
                ev.delta = 0;
                ev.pressed = pressed;
                events.push_back(ev);
            }
        }
    }
    
    // ----- Illuminated Buttons -----
    const InputAction button_actions[] = {
        InputAction::PREV,          // Button 1 - Yellow
        InputAction::PLAY_PAUSE,    // Button 2 - Red
        InputAction::NEXT,          // Button 3 - Green
        InputAction::SETTINGS_MENU  // Button 4 - Black
    };
    
    for (int i = 0; i < gpio::NUM_BUTTONS; i++) {
        int current = read_line(gpio::BUTTON_PINS[i]);
        bool pressed = (current == 0);  // Active low
        
        // Check if state changed and debounce time has passed
        if (pressed != !button_states_[i].last_state) {
            if (now - button_states_[i].last_change_time >= ButtonState::DEBOUNCE_MS) {
                button_states_[i].last_state = !pressed;
                button_states_[i].last_change_time = now;
                
                InputEvent ev;
                ev.action = button_actions[i];
                ev.delta = 0;
                ev.pressed = pressed;
                events.push_back(ev);
                
                // Update LED to match button state (light while pressed)
                set_led(i, pressed);
            }
        }
    }
    
    return events;
}

void GpioManager::set_led(int index, bool on) {
    if (!impl_->output_request || index < 0 || index >= gpio::NUM_BUTTONS) {
        return;
    }
    
    enum gpiod_line_value value = on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    gpiod_line_request_set_value(impl_->output_request, gpio::LED_PINS[index], value);
}

void GpioManager::set_all_leds(bool on) {
    for (int i = 0; i < gpio::NUM_BUTTONS; i++) {
        set_led(i, on);
    }
}

void GpioManager::stop_boot_led_sequence() {
    // Kill the boot LED sequence service now that the app is starting
    std::cout << "  Stopping boot LED sequence..." << std::endl;
    std::system("sudo systemctl stop led-boot-sequence.service 2>/dev/null || true");
}

void GpioManager::update_intro_animation(uint64_t elapsed_ms) {
    if (!available_) return;
    
    // Animation patterns for 11-second intro video
    // 0-4s: CRESCENDO! Synchronized strobe building in speed and intensity
    // 4-6s: Alternating pairs (1+2 vs 3+4, 1+3 vs 2+4)
    // 6-8s: Wave chase reverse (3->2->1->0)
    // 8-10s: Random-looking pattern with all combos
    // 10-11s: All on building to finale
    
    if (elapsed_ms < 4000) {
        // Phase 1 (0-4s): Building Glissando/Crescendo
        if (elapsed_ms >= 3000) {
            // 3s-4s: HOLD MAX INTENSITY (Solid On)
            set_all_leds(true);
        } else {
            // 0s-3s: Strobe building in speed and intensity
            // Calculate progress 0.0 to 1.0 based on 3000ms duration
            float progress = static_cast<float>(elapsed_ms) / 3000.0f;
            // Non-linear progress for more dramatic effect at end
            float curve = progress * progress * progress; // Cubic ease-in
            
            // Strobe Speed: 2Hz (slow) -> 12Hz (fast but distinct, avoiding jitter)
            float start_hz = 2.0f;
            float end_hz = 12.0f;
            float current_hz = start_hz + (end_hz - start_hz) * curve;
            int cycle_ms = static_cast<int>(1000.0f / current_hz);
            if (cycle_ms < 1) cycle_ms = 1;

            // Intensity (Duty Cycle): 10% (dim/short blip) -> 90% (almost solid)
            float start_duty = 0.1f;
            float end_duty = 0.9f;
            float current_duty = start_duty + (end_duty - start_duty) * curve;
            
            int time_in_cycle = elapsed_ms % cycle_ms;
            bool is_on = time_in_cycle < (cycle_ms * current_duty);
            
            set_all_leds(is_on);
        }
    }
    else if (elapsed_ms < 6000) {
        // Phase 3: Alternating pairs
        uint64_t pair_phase = (elapsed_ms / 300) % 4;
        switch (pair_phase) {
            case 0: // LEDs 0,1 on
                set_led(0, true); set_led(1, true);
                set_led(2, false); set_led(3, false);
                break;
            case 1: // LEDs 2,3 on
                set_led(0, false); set_led(1, false);
                set_led(2, true); set_led(3, true);
                break;
            case 2: // LEDs 0,2 on (diagonals)
                set_led(0, true); set_led(1, false);
                set_led(2, true); set_led(3, false);
                break;
            case 3: // LEDs 1,3 on
                set_led(0, false); set_led(1, true);
                set_led(2, false); set_led(3, true);
                break;
        }
    }
    else if (elapsed_ms < 8000) {
        // Phase 4: Ping Pong (0-1-2-3-2-1...)
        // Cycle length: 6 steps (0,1,2,3,2,1)
        int step = (elapsed_ms / 150) % 6; 
        int led_index;
        switch (step) {
            case 0: led_index = 0; break;
            case 1: led_index = 1; break;
            case 2: led_index = 2; break;
            case 3: led_index = 3; break;
            case 4: led_index = 2; break;
            case 5: led_index = 1; break;
            default: led_index = 0; break;
        }
        for (int i = 0; i < 4; i++) {
            set_led(i, i == led_index);
        }
    }
    else if (elapsed_ms < 10000) {
        // Phase 5: All combinations cycling faster
        uint64_t combo = (elapsed_ms / 200) % 8;
        set_led(0, combo & 1);
        set_led(1, combo & 2);
        set_led(2, combo & 4);
        set_led(3, (combo + 1) & 1);  // Offset for variation
    }
    else {
        // Phase 6: Building finale - all on with pulse
        set_all_leds(true);
    }
}

void GpioManager::play_shutdown_animation() {
    if (!available_) return;
    
    std::cout << "  Playing shutdown LED animation..." << std::endl;
    
    // Fast flicker slowing down with fading intensity (simulated)
    // Since we can't do true PWM, we approximate with on/off timing
    
    // Start with fast flicker (20ms on/off), slow to 200ms, over ~2 seconds
    int flicker_delays[] = {20, 25, 30, 40, 50, 70, 90, 110, 140, 170, 200};
    int num_delays = sizeof(flicker_delays) / sizeof(flicker_delays[0]);
    
    for (int i = 0; i < num_delays; i++) {
        int delay_ms = flicker_delays[i];
        int cycles = 200 / delay_ms;  // Fewer cycles as we slow down
        
        for (int c = 0; c < cycles; c++) {
            // Simulate fading by reducing on-time as we progress
            int on_time = delay_ms * (num_delays - i) / num_delays;
            int off_time = delay_ms - on_time + delay_ms;
            
            set_all_leds(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(on_time > 0 ? on_time : 1));
            set_all_leds(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(off_time > 0 ? off_time : 1));
        }
    }
    
    // Final fade out - quick flashes getting dimmer (shorter on times)
    for (int i = 5; i > 0; i--) {
        set_all_leds(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(i * 10));
        set_all_leds(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    set_all_leds(false);
    std::cout << "  Shutdown LED animation complete" << std::endl;
}

void GpioManager::stop_animation() {
    set_all_leds(false);
}

void GpioManager::cleanup() {
    if (impl_) {
        if (impl_->input_request) {
            gpiod_line_request_release(impl_->input_request);
            impl_->input_request = nullptr;
        }
        if (impl_->output_request) {
            gpiod_line_request_release(impl_->output_request);
            impl_->output_request = nullptr;
        }
        if (impl_->chip) {
            gpiod_chip_close(impl_->chip);
            impl_->chip = nullptr;
        }
    }
    available_ = false;
}

#else  // !HAVE_GPIOD

// Stub implementation when libgpiod is not available
struct GpioManager::Impl {};

GpioManager::GpioManager() : impl_(std::make_unique<Impl>()) {}
GpioManager::~GpioManager() { cleanup(); }

bool GpioManager::initialize() {
    std::cout << "  GPIO: libgpiod not available (compiled without HAVE_GPIOD)" << std::endl;
    available_ = false;
    return false;
}

std::vector<InputEvent> GpioManager::poll() {
    return {};
}

void GpioManager::set_led(int /*index*/, bool /*on*/) {}
void GpioManager::set_all_leds(bool /*on*/) {}
void GpioManager::cleanup() { available_ = false; }
uint64_t GpioManager::get_time_ms() const { return 0; }
int GpioManager::read_line(int /*gpio*/) { return 1; }
void GpioManager::check_restart_button() {}
void GpioManager::stop_boot_led_sequence() {}
void GpioManager::update_intro_animation(uint64_t /*elapsed_ms*/) {}
void GpioManager::play_shutdown_animation() {}
void GpioManager::stop_animation() {}

#endif  // HAVE_GPIOD

} // namespace platform

