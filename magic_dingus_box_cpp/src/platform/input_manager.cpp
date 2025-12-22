#include "input_manager.h"

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdlib>

namespace platform {

struct InputManager::Device {
    int fd;
    struct libevdev* dev;
    std::string name;
    bool is_joystick;
    bool is_keyboard;
    bool is_rotary;
    
    Device() : fd(-1), dev(nullptr), is_joystick(false), is_keyboard(false), is_rotary(false) {}
    
    ~Device() {
        if (dev) {
            libevdev_free(dev);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
};

InputManager::InputManager()
    : last_rotate_dir_(0)
    , last_rotate_time_(0.0)
{
}

InputManager::~InputManager() {
    cleanup();
}

bool InputManager::initialize() {
    std::cout << "  Opening input devices..." << std::endl;
    
    // CRITICAL: Wake up controller before opening devices
    // Controller may be in sleep mode and needs to be triggered
    std::system("sudo udevadm trigger --action=change --sysname-match=js* 2>/dev/null || true");
    std::system("sudo udevadm trigger --action=change --sysname-match=event* 2>/dev/null || true");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    if (!open_joystick_devices()) {
        std::cerr << "Warning: No joystick devices found" << std::endl;
    }
    if (!open_keyboard_devices()) {
        std::cerr << "Warning: No keyboard devices found" << std::endl;
    }
    if (!open_rotary_devices()) {
        std::cout << "  No dedicated rotary encoder device found (will check later)" << std::endl;
    }
    
    // CRITICAL: Read from controller to wake it up after opening
    // This ensures the controller is active and generating events
    for (auto& device : devices_) {
        if (device->is_joystick) {
            // Read any pending events to wake up the controller
            struct input_event ev;
            int rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_SYNC, &ev);
            // Discard the events, we just want to wake up the controller
            while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC) {
                rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            }
        }
    }
    
    return !devices_.empty();
}

bool InputManager::open_joystick_devices() {
    const char* input_dir = "/dev/input";
    DIR* dir = opendir(input_dir);
    if (!dir) {
        return false;
    }
    
    bool found = false;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        
        std::string path = std::string(input_dir) + "/" + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        
        struct libevdev* dev = nullptr;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0 || !dev) {
            if (dev) libevdev_free(dev);
            close(fd);
            continue;
        }
        
        // Check if it's a joystick
        if (libevdev_has_event_type(dev, EV_ABS) &&
            (libevdev_has_event_code(dev, EV_ABS, ABS_X) ||
             libevdev_has_event_code(dev, EV_ABS, ABS_HAT0X))) {
            
            const char* dev_name = libevdev_get_name(dev);
            if (!dev_name) {
                libevdev_free(dev);
                close(fd);
                continue;
            }
            
            auto device = std::make_unique<Device>();
            device->fd = fd;
            device->dev = dev;
            device->name = dev_name ? dev_name : "Unknown";
            device->is_joystick = true;
            
            // Grab device for exclusive access (may fail, but that's OK)
            int grab_rc = libevdev_grab(dev, LIBEVDEV_GRAB);
            if (grab_rc < 0) {
                std::cerr << "  Warning: Could not grab device " << device->name << std::endl;
            }
            
            std::string device_name = device->name;  // Save name before move
            devices_.push_back(std::move(device));
            found = true;
            std::cout << "  Found joystick: " << device_name << " at " << path << std::endl;
        } else {
            libevdev_free(dev);
            close(fd);
        }
    }
    
    closedir(dir);
    return found;
}

bool InputManager::open_keyboard_devices() {
    const char* input_dir = "/dev/input";
    DIR* dir = opendir(input_dir);
    if (!dir) {
        return false;
    }
    
    bool found = false;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        
        std::string path = std::string(input_dir) + "/" + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        
        struct libevdev* dev = nullptr;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0 || !dev) {
            if (dev) libevdev_free(dev);
            close(fd);
            continue;
        }
        
        // Check if it's a keyboard
        if (libevdev_has_event_type(dev, EV_KEY) &&
            libevdev_has_event_code(dev, EV_KEY, KEY_ENTER)) {
            
            const char* dev_name = libevdev_get_name(dev);
            if (!dev_name) {
                libevdev_free(dev);
                close(fd);
                continue;
            }
            
            auto device = std::make_unique<Device>();
            device->fd = fd;
            device->dev = dev;
            device->name = dev_name ? dev_name : "Unknown";
            device->is_keyboard = true;
            
            // Grab device for exclusive access (may fail, but that's OK)
            int grab_rc = libevdev_grab(dev, LIBEVDEV_GRAB);
            if (grab_rc < 0) {
                std::cerr << "  Warning: Could not grab device " << device->name << std::endl;
            }
            
            std::string device_name = device->name;  // Save name before move
            devices_.push_back(std::move(device));
            found = true;
            std::cout << "  Found keyboard: " << device_name << " at " << path << std::endl;
        } else {
            libevdev_free(dev);
            close(fd);
        }
    }
    
    closedir(dir);
    return found;
}

bool InputManager::open_rotary_devices() {
    const char* input_dir = "/dev/input";
    DIR* dir = opendir(input_dir);
    if (!dir) return false;
    
    bool found = false;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        
        std::string path = std::string(input_dir) + "/" + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        
        struct libevdev* dev = nullptr;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0 || !dev) {
            if (dev) libevdev_free(dev);
            close(fd);
            continue;
        }
        
        // Check if it's a rotary encoder (REL_X)
        if (libevdev_has_event_type(dev, EV_REL) && libevdev_has_event_code(dev, EV_REL, REL_X)) {
            const char* dev_name = libevdev_get_name(dev);
            if (!dev_name) {
                libevdev_free(dev);
                close(fd);
                continue;
            }
            
            auto device = std::make_unique<Device>();
            device->fd = fd;
            device->dev = dev;
            device->name = dev_name ? dev_name : "Unknown";
            device->is_rotary = true;
            
            int grab_rc = libevdev_grab(dev, LIBEVDEV_GRAB);
            if (grab_rc < 0) {
                 std::cerr << "  Warning: Could not grab rotary device " << device->name << std::endl;
            }
            
            std::string device_name = device->name;
            devices_.push_back(std::move(device));
            found = true;
            std::cout << "  Found rotary encoder: " << device_name << " at " << path << std::endl;
        } else {
            libevdev_free(dev);
            close(fd);
        }
    }
    closedir(dir);
    return found;
}

std::vector<InputEvent> InputManager::poll() {
    std::vector<InputEvent> events;
    
    for (auto& device : devices_) {
        struct input_event ev;
        int rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        while (rc == LIBEVDEV_READ_STATUS_SYNC || rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            InputEvent input_ev;
            input_ev.action = InputAction::NONE;
            input_ev.delta = 0;
            input_ev.pressed = false;
            
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                // Handle sync events
                std::cout << "InputManager: SYNC event received" << std::endl;
                rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                continue;
            }
            
            if (ev.type == EV_KEY) {
                // Button/key press
                input_ev.pressed = (ev.value == 1);
                
                // Handle keyboard arrow keys for rotation (before other mappings)
                if (device->is_keyboard) {
                    if (ev.code == KEY_LEFT && ev.value == 1) {
                        input_ev.action = InputAction::ROTATE;
                        input_ev.delta = -1;
                    } else if (ev.code == KEY_RIGHT && ev.value == 1) {
                        input_ev.action = InputAction::ROTATE;
                        input_ev.delta = 1;
                    } else if (ev.code == KEY_UP && ev.value == 1) {
                        input_ev.action = InputAction::ROTATE_VERTICAL;
                        input_ev.delta = -1;
                    } else if (ev.code == KEY_DOWN && ev.value == 1) {
                        input_ev.action = InputAction::ROTATE_VERTICAL;
                        input_ev.delta = 1;
                    } else {
                        input_ev.action = map_key_to_action(ev.code);
                    }
                } else if (device->is_joystick) {
                    // Handle D-pad buttons (common on some controllers)
                    if (ev.code == BTN_DPAD_UP && ev.value == 1) {
                        input_ev.action = InputAction::ROTATE_VERTICAL;
                        input_ev.delta = -1;
                    } else if (ev.code == BTN_DPAD_DOWN && ev.value == 1) {
                        input_ev.action = InputAction::ROTATE_VERTICAL;
                        input_ev.delta = 1;
                    } else if (ev.code == BTN_DPAD_LEFT && ev.value == 1) {
                        input_ev.action = InputAction::ROTATE;
                        input_ev.delta = -1;
                    } else if (ev.code == BTN_DPAD_RIGHT && ev.value == 1) {
                        input_ev.action = InputAction::ROTATE;
                        input_ev.delta = 1;
                    } else {
                        input_ev.action = map_button_to_action(ev.code, input_ev.pressed);
                    }
                }
            } else if (ev.type == EV_ABS && device->is_joystick) {
                // Handle DPad hat switches (ABS_HAT0X, ABS_HAT0Y)
                if (ev.code == ABS_HAT0Y) {
                    // DPad Up/Down for ROTATE_VERTICAL (matching Python: DPad Up = -1, Down = +1)
                    if (ev.value == -1) {  // Up
                        input_ev.action = InputAction::ROTATE_VERTICAL;
                        input_ev.delta = -1;
                    } else if (ev.value == 1) {  // Down
                        input_ev.action = InputAction::ROTATE_VERTICAL;
                        input_ev.delta = 1;
                    }
                } else if (ev.code == ABS_HAT0X) {
                    // DPad Left/Right for ROTATE
                    if (ev.value == -1) {  // Left
                        input_ev.action = InputAction::ROTATE;
                        input_ev.delta = -1;
                    } else if (ev.value == 1) {  // Right
                        input_ev.action = InputAction::ROTATE;
                        input_ev.delta = 1;
                    }
                } else if (ev.code == ABS_Y) {
                    // Analog Stick Y for ROTATE_VERTICAL
                    // Deadzone check (simple)
                    if (ev.value < -16000) { // Up
                         input_ev.action = InputAction::ROTATE_VERTICAL;
                         input_ev.delta = -1;
                    } else if (ev.value > 16000) { // Down
                         input_ev.action = InputAction::ROTATE_VERTICAL;
                         input_ev.delta = 1;
                    }
                } else {
                    // Regular axis movement
                    input_ev.action = map_axis_to_action(ev.code, ev.value);
                    // Set delta for ROTATE action
                    if (input_ev.action == InputAction::ROTATE) {
                        input_ev.delta = (ev.value > 0) ? 1 : (ev.value < 0) ? -1 : 0;
                    }
                }
            } else if (ev.type == EV_REL) {
                // Handle Rotary Encoder (REL_X)
                if (ev.code == REL_X) {
                    // Software Accumulator to fix sensitivity and "skipping"
                    // Require accumulating 4 units (either magnitude 4 or 4 events of 1) to trigger 1 step
                    static int accumulator = 0;
                    const int THRESHOLD = 4; 
                    
                    accumulator += ev.value;
                    
                    if (std::abs(accumulator) >= THRESHOLD) {
                        input_ev.action = InputAction::ROTATE;
                        // INVERT direction: positive accumulator -> negative delta
                        // (User requested inversion)
                        input_ev.delta = (accumulator > 0) ? -1 : 1;
                        accumulator = 0;
                    }
                }
            }
            
            if (input_ev.action != InputAction::NONE) {
                events.push_back(input_ev);
            }
            
            rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        }
    }
    
    return events;
}

InputAction InputManager::map_button_to_action(uint16_t code, bool pressed) {
    // Note: We now return the action even if pressed is false (release event)
    // The caller (main loop) must check ev.pressed if it only cares about presses
    
    // N64 Controller mappings (matching Python evdev_joystick.py)
    // Button 304 = BTN_SOUTH (standard A) -> SELECT
    // Button 306 = A button (N64) -> SELECT
    // Button 305 = BTN_EAST (B button) -> SETTINGS_MENU
    // Button 316 = START -> SELECT
    // Button 310 = Z -> PLAY_PAUSE
    // Button 309 = R -> NEXT
    // Button 308 = L -> PREV
    
    switch (code) {
        case 304:  // BTN_SOUTH (standard A button)
        case 306:  // A button (N64)
        case 316:  // START
            return InputAction::SELECT;
        case 305:  // BTN_EAST (B button)
            return InputAction::SETTINGS_MENU;
        case 310:  // Z
            return InputAction::PLAY_PAUSE;
        case 309:  // R
            return InputAction::NEXT;
        case 308:  // L
            return InputAction::PREV;
        default:
            return InputAction::NONE;
    }
}

InputAction InputManager::map_axis_to_action(uint8_t axis, int16_t value) {
    // Axis 0 = X (left/right) -> ROTATE
    // Axis 3 = C-stick horizontal -> NEXT/PREV
    // Axis 2 = C-stick vertical -> unused
    
    // Apply deadzone to avoid drift
    const int16_t deadzone = 5000;  // ~15% of full range
    
    if (axis == 0) {
        // X axis for rotation
        int dir = 0;
        if (value > deadzone) {
            dir = 1;
        } else if (value < -deadzone) {
            dir = -1;
        }
        
        if (dir != 0 && dir != last_rotate_dir_) {
            last_rotate_dir_ = dir;
            auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - last_rotate_time_ >= (1.0 / ROTATE_REPEAT_HZ)) {
                last_rotate_time_ = now;
                return InputAction::ROTATE;
            }
        }
        if (dir == 0) {
            last_rotate_dir_ = 0;
        }
    } else if (axis == 3) {
        // C-stick horizontal
        if (value > deadzone) {
            return InputAction::SEEK_RIGHT;
        } else if (value < -deadzone) {
            return InputAction::SEEK_LEFT;
        }
    }
    
    return InputAction::NONE;
}

InputAction InputManager::map_key_to_action(uint16_t code) {
    // Keyboard mappings (matching Python keyboard.py)
    // Note: Arrow keys are handled in poll() to set delta
    switch (code) {
        case KEY_ENTER:
        case KEY_SPACE:
            return InputAction::SELECT;
        // Up/Down handled in poll() as ROTATE_VERTICAL
        case KEY_LEFT:
            return InputAction::SEEK_LEFT;
        case KEY_RIGHT:
            return InputAction::SEEK_RIGHT;
        case KEY_N:
            return InputAction::NEXT;
        case KEY_P:
            return InputAction::PREV;
        case KEY_PLAYPAUSE:
            return InputAction::PLAY_PAUSE;
        case KEY_ESC:
        case KEY_Q:
            return InputAction::QUIT;
        default:
            return InputAction::NONE;
    }
}

void InputManager::cleanup() {
    for (auto& device : devices_) {
        if (device->dev) {
            libevdev_grab(device->dev, LIBEVDEV_UNGRAB);
        }
    }
    devices_.clear();
}

} // namespace platform

