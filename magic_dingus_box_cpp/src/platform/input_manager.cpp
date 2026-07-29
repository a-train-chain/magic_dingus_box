#include "input_manager.h"

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <sys/wait.h>

namespace platform {

struct InputManager::Device {
    int fd;
    struct libevdev* dev;
    std::string name;
    bool is_joystick;
    bool is_keyboard;
    bool is_rotary;
    // Some cheap PS-style USB pads (e.g. DragonRise/Microntek 0079:0006)
    // report axes ABS_X/Y in 0..255 range with the D-pad sending extremes
    // (0 or 255) instead of using ABS_HAT0X/Y. True for those devices.
    bool axis_is_8bit;
    // Tracks the last-known D-pad direction emitted from ABS_X/Y on 8-bit
    // controllers, so we only fire on transitions (press/release), not on
    // every event that happens to land in the extreme zone.
    int dpad_x_8bit_last;
    int dpad_y_8bit_last;
    // USB identity, used to key menu-nav overlays and device_caps() lookups.
    uint16_t vid = 0;
    uint16_t pid = 0;
    // Non-owning pointer into InputManager::overlays_, or nullptr when no
    // profile has been captured for this pad. MUST be re-resolved whenever
    // that map is replaced (set_menu_overlays does this) -- a stale pointer
    // here would be read on every single event.
    //
    // RESOLUTION INVARIANT, stated precisely because an earlier version of
    // this comment overstated it: only the two JOYSTICK open paths
    // (open_joystick_devices, reprobe_phone_remote) resolve this field.
    // open_keyboard_devices() and open_rotary_devices() also construct
    // Devices and leave it at the in-class initializer. That is safe --
    // nullptr is the "no overlay" value and every dereference in poll() is
    // behind an `is_joystick` guard -- but the guard is load-bearing, not
    // decorative. Any future code that reads `overlay` outside an
    // `is_joystick` branch must resolve it at those two open sites too.
    const MenuNavOverlay* overlay = nullptr;

    // Per-ABS-code rest centre and proportional deadzone, cached from the
    // device's own reported min/max at open time (cache_axis_ranges).
    // Indexed by raw ABS code; `valid` is false for codes the device does not
    // report or whose range is degenerate.
    static constexpr unsigned ABS_CODE_COUNT = 0x40;   // ABS_MAX + 1
    struct AxisNorm {
        int centre = 0;
        int deadzone = 0;
        bool valid = false;
    };
    AxisNorm axis_norm[ABS_CODE_COUNT];

    // Centre + deadzone to use for `code`. Falls back to the historical
    // signed-16-bit assumption (centre 0, +/-AXIS_DEADZONE) for any code with
    // no usable reported range, so a device we learned nothing about behaves
    // exactly as it did before this cache existed.
    AxisNorm axis_threshold(uint16_t code) const {
        const unsigned idx = code;   // explicit: keeps the compare unsigned/unsigned
        if (idx < ABS_CODE_COUNT && axis_norm[idx].valid) return axis_norm[idx];
        return AxisNorm{0, InputManager::AXIS_DEADZONE, false};
    }

    // Populate axis_norm from libevdev's reported ranges.
    //
    // Axes in the wild are NOT all signed 16-bit: the cheap DragonRise /
    // Microntek pads this wizard exists for report 0..255, others report
    // 0..1023. Comparing such a value against a fixed symmetric +/-5000 can
    // never be true on the negative side and is always true on the positive
    // side, so an overlay claiming one of those axes was inert. Normalizing
    // per device makes a 0..255 axis and a signed-16-bit axis behave alike.
    //
    // The scaling is chosen so a SIGNED 16-BIT axis lands exactly where it
    // always did:
    //     centre   = (min + max) / 2      -> (-32768 + 32767) / 2 == 0
    //     half     = (max - min) / 2      -> 65535 / 2            == 32767
    //     deadzone = half * 5000 / 32767  -> 32767 * 5000 / 32767 == 5000
    // making `value - centre > deadzone` character-for-character the old
    // `value > 5000`. A 0..255 axis instead yields centre 127, half 127,
    // deadzone 19 -- the same ~15%-of-half-range feel.
    void cache_axis_ranges() {
        if (!dev) return;
        for (unsigned code = 0; code < ABS_CODE_COUNT; ++code) {
            if (!libevdev_has_event_code(dev, EV_ABS, code)) continue;
            const int lo = libevdev_get_abs_minimum(dev, code);
            const int hi = libevdev_get_abs_maximum(dev, code);
            if (hi <= lo) continue;   // degenerate range: leave `valid` false
            AxisNorm n;
            n.centre = (lo + hi) / 2;
            const int half = (hi - lo) / 2;
            n.deadzone = static_cast<int>(
                (static_cast<int64_t>(half) * InputManager::AXIS_DEADZONE) /
                InputManager::AXIS_DEADZONE_OF);
            // A deadzone of 0 is left as 0 on purpose. It only comes out that
            // way when half < 7 -- a span of at most 13 units, which is not an
            // analog stick but a digital axis (a hat's -1..1, a 0..1 trigger).
            // For those, "any deflection from centre" is the correct
            // threshold, and forcing a floor of 1 would make them permanently
            // inert instead. Real analog ranges (0..255 and up) never reach
            // here: half >= 7 already yields a nonzero deadzone.
            n.valid = true;
            axis_norm[code] = n;
        }
    }

    Device() : fd(-1), dev(nullptr), is_joystick(false), is_keyboard(false), is_rotary(false),
               axis_is_8bit(false), dpad_x_8bit_last(0), dpad_y_8bit_last(0) {}

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
    auto run_udevadm = [](const char* match) {
        pid_t pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
            execlp("sudo", "sudo", "udevadm", "trigger", "--action=change",
                   match, nullptr);
            _exit(127);
        }
        if (pid > 0) { int s; waitpid(pid, &s, 0); }
    };
    run_udevadm("--sysname-match=js*");
    run_udevadm("--sysname-match=event*");
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
            device->vid = static_cast<uint16_t>(libevdev_get_id_vendor(dev));
            device->pid = static_cast<uint16_t>(libevdev_get_id_product(dev));
            device->overlay = lookup_overlay(device->vid, device->pid);
            device->cache_axis_ranges();
            // Detect 8-bit-axis controllers (DragonRise-style): ABS_X reported
            // with min=0 means values are 0..255 with center 127, NOT signed
            // 16-bit. The D-pad on these pads sends ABS_X/Y extremes (0 or
            // 255) instead of HAT events.
            if (libevdev_has_event_code(dev, EV_ABS, ABS_X)) {
                int abs_min = libevdev_get_abs_minimum(dev, ABS_X);
                int abs_max = libevdev_get_abs_maximum(dev, ABS_X);
                if (abs_min == 0 && abs_max <= 255) {
                    device->axis_is_8bit = true;
                    std::cout << "  Joystick uses 8-bit axes (D-pad on ABS_X/Y extremes): "
                              << device->name << std::endl;
                }
            }
            
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

void InputManager::reprobe_phone_remote() {
    // Single spelling of the name, shared with poll()'s raw-capture bypass
    // (the phone remote must keep producing InputActions during capture, so
    // both sites have to agree on which device that is).
    const char* kPhoneName = PHONE_REMOTE_NAME;

    // Step 1: drop a dead phone-remote grab. When the web service restarts,
    // the old uinput node is destroyed; EVIOCGID on its fd then fails with
    // ENODEV. This ioctl is non-destructive (doesn't consume events) and we
    // only run it on the phone-remote device, so healthy controllers are
    // untouched.
    for (auto it = devices_.begin(); it != devices_.end(); ) {
        Device* d = it->get();
        if (d->name == kPhoneName) {
            struct input_id id;
            if (ioctl(d->fd, EVIOCGID, &id) < 0) {
                std::cout << "Phone remote node gone (web restart?); dropping "
                             "stale grab, will reopen" << std::endl;
                // Clear the manager-level D-pad hold latch. The phone-remote
                // D-pad is an ABS_HAT0X/Y axis; if the node vanishes mid-hold
                // no 'value=0' event ever arrives, so dpad_held_* would stay
                // latched and generate_dpad_repeats() would scroll the menu
                // forever. cleanup() already does this on full teardown; the
                // incremental erase here must too. Always safe — we can't
                // know if the dying device was the one holding a direction.
                dpad_held_x_ = 0;
                dpad_held_y_ = 0;
                it = devices_.erase(it);  // ~Device closes fd + frees libevdev
                continue;
            }
        }
        ++it;
    }

    // Step 2: if a phone remote is already open and healthy, nothing to do.
    for (auto& d : devices_) {
        if (d->name == kPhoneName) return;
    }

    // Step 3: no phone remote open — scan for the current node and grab it.
    // Filtered strictly by name, so this can never open/grab any other
    // device. Mirrors open_joystick_devices()'s open+grab sequence.
    const char* input_dir = "/dev/input";
    DIR* dir = opendir(input_dir);
    if (!dir) return;
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
        const char* dev_name = libevdev_get_name(dev);
        const bool is_phone = dev_name && std::string(dev_name) == kPhoneName;
        const bool is_joystick = libevdev_has_event_type(dev, EV_ABS) &&
            (libevdev_has_event_code(dev, EV_ABS, ABS_X) ||
             libevdev_has_event_code(dev, EV_ABS, ABS_HAT0X));
        if (is_phone && is_joystick) {
            auto device = std::make_unique<Device>();
            device->fd = fd;
            device->dev = dev;
            device->name = kPhoneName;
            device->is_joystick = true;
            device->vid = static_cast<uint16_t>(libevdev_get_id_vendor(dev));
            device->pid = static_cast<uint16_t>(libevdev_get_id_product(dev));
            device->overlay = lookup_overlay(device->vid, device->pid);
            device->cache_axis_ranges();
            int grab_rc = libevdev_grab(dev, LIBEVDEV_GRAB);
            if (grab_rc < 0) {
                std::cerr << "  Warning: could not grab reopened phone remote"
                          << std::endl;
            }
            devices_.push_back(std::move(device));
            std::cout << "  Reopened phone remote at " << path << std::endl;
            closedir(dir);
            return;
        }
        libevdev_free(dev);
        close(fd);
    }
    closedir(dir);
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

    // Joystick nodes that reported -ENODEV this pass (unplugged). Collected
    // rather than erased inline: devices_ is being iterated.
    std::vector<Device*> dead_devices;

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

            // RAW CAPTURE (Controller Setup wizard). While enabled, a REAL
            // joystick's events are queued verbatim and skip ALL action
            // mapping below -- otherwise pressing a face button to capture
            // it would also confirm whatever the menu had selected.
            // Deliberately NOT diverted: keyboards, the rotary encoder, and
            // the phone-remote virtual pad, which are the surfaces the user
            // still needs to drive the wizard itself.
            if (raw_capture_ && device->is_joystick &&
                device->name != PHONE_REMOTE_NAME) {
                if (ev.type == EV_KEY || ev.type == EV_ABS) {
                    if (raw_events_.size() >= MAX_RAW_EVENTS) {
                        // Nobody is draining. Drop the OLDEST half so the
                        // queue stays bounded and the user's most recent
                        // gesture is still the one that gets seen.
                        raw_events_.erase(
                            raw_events_.begin(),
                            raw_events_.begin() + MAX_RAW_EVENTS / 2);
                    }
                    RawInputEvent raw;
                    raw.vid = device->vid;
                    raw.pid = device->pid;
                    raw.device_name = device->name;
                    raw.type = ev.type;
                    raw.code = ev.code;
                    raw.value = ev.value;
                    raw_events_.push_back(std::move(raw));
                }
                // Keep draining libevdev so the kernel buffer can't back up.
                rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                continue;
            }

            if (ev.type == EV_KEY) {
                // Button/key press
                input_ev.pressed = (ev.value == 1);

                // Handle keyboard arrow keys for rotation (before other mappings)
                if (device->is_keyboard) {
                    if (ev.code == KEY_LEFT || ev.code == KEY_RIGHT) {
                        auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (ev.value == 1) {
                            int dir = (ev.code == KEY_LEFT) ? -1 : 1;
                            dpad_held_x_ = dir;
                            dpad_press_time_x_ = now;
                            dpad_last_fire_x_ = now;
                            input_ev.action = InputAction::ROTATE;
                            input_ev.delta = dir;
                        } else if (ev.value == 0) {
                            dpad_held_x_ = 0;
                        }
                        // value==2 (autorepeat) is ignored
                    } else if (ev.code == KEY_UP || ev.code == KEY_DOWN) {
                        auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (ev.value == 1) {
                            int dir = (ev.code == KEY_UP) ? -1 : 1;
                            dpad_held_y_ = dir;
                            dpad_press_time_y_ = now;
                            dpad_last_fire_y_ = now;
                            input_ev.action = InputAction::ROTATE_VERTICAL;
                            input_ev.delta = dir;
                        } else if (ev.value == 0) {
                            dpad_held_y_ = 0;
                        }
                        // value==2 (autorepeat) is ignored
                    } else {
                        input_ev.action = map_key_to_action(ev.code);
                    }
                } else if (device->is_joystick) {
                    // NOTE: this `is_joystick` guard (and its twin on the
                    // EV_ABS branch) is what makes reading `device->overlay`
                    // below safe. Only the two joystick open paths resolve
                    // that field; open_keyboard_devices() and
                    // open_rotary_devices() leave it at its nullptr in-class
                    // initializer. See the invariant note on Device::overlay.
                    //
                    // Handle D-pad buttons (common on some controllers)
                    if (ev.code == BTN_DPAD_UP || ev.code == BTN_DPAD_DOWN) {
                        if (ev.value == 1) {
                            auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                            int dir = (ev.code == BTN_DPAD_UP) ? -1 : 1;
                            dpad_held_y_ = dir;
                            dpad_press_time_y_ = now;
                            dpad_last_fire_y_ = now;
                            input_ev.action = InputAction::ROTATE_VERTICAL;
                            input_ev.delta = dir;
                        } else if (ev.value == 0) {
                            int dir = (ev.code == BTN_DPAD_UP) ? -1 : 1;
                            if (dpad_held_y_ == dir) dpad_held_y_ = 0;
                        }
                    } else if (ev.code == BTN_DPAD_LEFT || ev.code == BTN_DPAD_RIGHT) {
                        if (ev.value == 1) {
                            auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                            int dir = (ev.code == BTN_DPAD_LEFT) ? -1 : 1;
                            dpad_held_x_ = dir;
                            dpad_press_time_x_ = now;
                            dpad_last_fire_x_ = now;
                            input_ev.action = InputAction::ROTATE;
                            input_ev.delta = dir;
                        } else if (ev.value == 0) {
                            int dir = (ev.code == BTN_DPAD_LEFT) ? -1 : 1;
                            if (dpad_held_x_ == dir) dpad_held_x_ = 0;
                        }
                    } else {
                        // ADDITIVE MENU-NAV OVERLAY. A captured per-model
                        // profile may claim this EV_KEY code; if it does, it
                        // REPLACES the built-in mapping for that one code and
                        // map_button_to_action is not consulted (so a single
                        // press can never fire two actions). Every code the
                        // overlay does not claim -- and every code at all when
                        // no overlay is installed -- takes the built-in path
                        // exactly as before.
                        //
                        // Note this sits in the final `else`, AFTER the
                        // BTN_DPAD_* handling above, on purpose: a garbage
                        // profile that claimed a d-pad code must not be able
                        // to take d-pad navigation away. Worst case for a bad
                        // overlay entry is that it's inert.
                        InputAction overlay_action = InputAction::NONE;
                        if (device->overlay) {
                            auto it = device->overlay->buttons.find(ev.code);
                            if (it != device->overlay->buttons.end()) {
                                overlay_action = it->second;
                            }
                        }
                        input_ev.action =
                            (overlay_action != InputAction::NONE)
                                ? overlay_action
                                : map_button_to_action(ev.code, input_ev.pressed);
                    }
                }
            } else if (ev.type == EV_ABS && device->is_joystick) {
                // 8-bit-axis controllers (DragonRise/Microntek): D-pad rides
                // on ABS_X/Y extremes (0 = -1, 255 = +1, ~127 = center).
                // Treat extremes as digital direction transitions (the same
                // way HAT events are handled below). ABS_Z / ABS_RZ on these
                // pads is the right analog stick, which by default fires
                // nothing so analog wiggle can't trigger kiosk actions -- but
                // unlike ABS_X/Y an overlay MAY claim those two; see below.
                if (device->axis_is_8bit) {
                    if (ev.code == ABS_X) {
                        int dir = (ev.value <= 32) ? -1 : (ev.value >= 224 ? 1 : 0);
                        if (dir != device->dpad_x_8bit_last) {
                            device->dpad_x_8bit_last = dir;
                            if (dir != 0) {
                                auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                                dpad_held_x_ = dir;
                                dpad_press_time_x_ = now;
                                dpad_last_fire_x_ = now;
                                input_ev.action = InputAction::ROTATE;
                                input_ev.delta = dir;
                            } else {
                                dpad_held_x_ = 0;
                            }
                        }
                        rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                        if (input_ev.action != InputAction::NONE) {
                            events.push_back(input_ev);
                        }
                        continue;
                    }
                    if (ev.code == ABS_Y) {
                        int dir = (ev.value <= 32) ? -1 : (ev.value >= 224 ? 1 : 0);
                        if (dir != device->dpad_y_8bit_last) {
                            device->dpad_y_8bit_last = dir;
                            if (dir != 0) {
                                auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                                dpad_held_y_ = dir;
                                dpad_press_time_y_ = now;
                                dpad_last_fire_y_ = now;
                                input_ev.action = InputAction::ROTATE_VERTICAL;
                                input_ev.delta = dir;
                            } else {
                                dpad_held_y_ = 0;
                            }
                        }
                        rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                        if (input_ev.action != InputAction::NONE) {
                            events.push_back(input_ev);
                        }
                        continue;
                    }
                    if (ev.code == ABS_Z || ev.code == ABS_RZ) {
                        // Right analog stick on cheap PS-style pads. The
                        // BUILT-IN behavior is to do nothing at all, so
                        // wiggling it never fires kiosk actions -- but an
                        // overlay is allowed to claim these two codes, and this
                        // swallow used to run first, which made every such
                        // claim SILENTLY INERT. That is exactly what the wizard
                        // produces on this device class: capture RSTICK_RIGHT
                        // on a DragonRise and seek_abs lands on ABS_Z.
                        //
                        // Honoring a claim here is strictly ADDITIVE: these
                        // codes produced no action whatsoever before, so
                        // nothing is taken away. The check is deliberately NOT
                        // hoisted above the ABS_X / ABS_Y branches above --
                        // those two carry the D-PAD on 8-bit pads, and letting
                        // an overlay pre-empt them would cost the user menu
                        // navigation, which is a regression, not a fix.
                        const MenuNavOverlay* ov = device->overlay;
                        const bool claimed =
                            ov != nullptr &&
                            ((ov->nav_x_abs >= 0 &&
                              static_cast<int>(ev.code) == ov->nav_x_abs) ||
                             (ov->seek_abs >= 0 &&
                              static_cast<int>(ev.code) == ov->seek_abs));
                        if (!claimed) {
                            rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                            continue;
                        }
                        // Claimed: fall through to the overlay branches below.
                        // Still exactly one action per event -- ABS_Z (0x02)
                        // and ABS_RZ (0x05) match none of the intervening
                        // cases (ABS_HAT0Y 0x11, ABS_HAT0X 0x10, ABS_Y 0x01),
                        // and `claimed` guarantees one of the two overlay
                        // `else if`s takes it, so it can never reach the
                        // trailing map_axis_to_action either.
                    }
                }
                // Handle DPad hat switches (ABS_HAT0X, ABS_HAT0Y)
                if (ev.code == ABS_HAT0Y) {
                    if (ev.value != 0) {
                        if (dpad_held_y_ != ev.value) {
                            // New direction or first press - reset timing
                            auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                            dpad_held_y_ = ev.value;
                            dpad_press_time_y_ = now;
                            dpad_last_fire_y_ = now;
                            input_ev.action = InputAction::ROTATE_VERTICAL;
                            input_ev.delta = ev.value;
                        }
                        // Same value repeated while held - ignore (generate_dpad_repeats handles it)
                    } else {
                        dpad_held_y_ = 0;
                    }
                } else if (ev.code == ABS_HAT0X) {
                    if (ev.value != 0) {
                        if (dpad_held_x_ != ev.value) {
                            auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                            dpad_held_x_ = ev.value;
                            dpad_press_time_x_ = now;
                            dpad_last_fire_x_ = now;
                            input_ev.action = InputAction::ROTATE;
                            input_ev.delta = ev.value;
                        }
                    } else {
                        dpad_held_x_ = 0;
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
                // ADDITIVE MENU-NAV OVERLAY (axes). Reached ONLY for codes none
                // of the branches above handled, and because these are
                // `else if`s a code the overlay DOES claim never also reaches
                // map_axis_to_action below: exactly one action per event.
                //
                // THE CEILING -- exactly what an overlay can and cannot claim.
                // Tasks 10 and 12 should work from this list, not from
                // intuition; an entry naming an unclaimable code is accepted,
                // stored, and then silently does nothing.
                //
                //   EV_ABS, on EVERY device class:
                //     ABS_HAT0X / ABS_HAT0Y  NOT claimable (built-in hat)
                //     ABS_Y                  NOT claimable (built-in vertical
                //                            navigation on analog pads; d-pad
                //                            on 8-bit pads)
                //     ABS_Z / ABS_RZ         claimable
                //     everything else        claimable
                //
                //   EV_ABS, additionally on 8-bit pads (axis_is_8bit, i.e.
                //   ABS_X reported min==0 && max<=255 -- the DragonRise /
                //   Microntek class):
                //     ABS_X                  NOT claimable -- it carries the
                //                            D-PAD on these pads, not a stick
                //   ABS_X IS claimable on every other device class (which is
                //   how a profile with seek_abs == 0 can turn horizontal
                //   navigation into seek; see MenuNavOverlay's header comment).
                //
                //   EV_KEY, on every device class:
                //     BTN_DPAD_UP/DOWN/LEFT/RIGHT  NOT claimable
                //     every other code             claimable
                //
                // The unclaimable set is exactly the set of built-in handlers
                // whose loss would cost the user a working direction, so the
                // ceiling is a fail-safe, not an oversight. ABS_Z/ABS_RZ were
                // wrongly inside it on 8-bit pads until the swallow above
                // learned to consult the overlay first.
                } else if (device->overlay && device->overlay->nav_x_abs >= 0 &&
                           static_cast<int>(ev.code) == device->overlay->nav_x_abs) {
                    // Same fire-on-change / hold-repeat logic the hardcoded
                    // ABS_X path uses, on this pad's actual stick-X code, with
                    // the deadzone scaled to the range this axis really
                    // reports (a 0..255 axis rests at 127 and could never
                    // cross a raw +/-5000 threshold in either direction).
                    const Device::AxisNorm n = device->axis_threshold(ev.code);
                    const int32_t deflection = ev.value - n.centre;
                    input_ev.action = rotate_from_axis_value(deflection, n.deadzone);
                    if (input_ev.action == InputAction::ROTATE) {
                        input_ev.delta = (deflection > 0) ? 1 : (deflection < 0) ? -1 : 0;
                    }
                } else if (device->overlay && device->overlay->seek_abs >= 0 &&
                           static_cast<int>(ev.code) == device->overlay->seek_abs) {
                    // Same deadzone the hardcoded C-stick path uses, likewise
                    // centred and scaled to this axis's reported range.
                    const Device::AxisNorm n = device->axis_threshold(ev.code);
                    const int32_t deflection = ev.value - n.centre;
                    if (deflection > n.deadzone) {
                        input_ev.action = InputAction::SEEK_RIGHT;
                    } else if (deflection < -n.deadzone) {
                        input_ev.action = InputAction::SEEK_LEFT;
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
                    // Software accumulator: require one detent's worth of
                    // EV_REL events before advancing the UI one step.
                    // How many that is depends on the board — Pi 4 emits 2
                    // per detent, Pi 5 emits 1 (measured on hardware
                    // 2026-07-25). Hardcoding 2 made the Pi 5 advance only
                    // every OTHER click. Set from PlatformProfile in main().
                    const int THRESHOLD = rotary_events_per_detent_;

                    rotary_accumulator_ += ev.value;

                    if (std::abs(rotary_accumulator_) >= THRESHOLD) {
                        input_ev.action = InputAction::ROTATE;
                        // Clockwise (positive) = down/next (positive delta)
                        // Counterclockwise (negative) = up/previous (negative delta)
                        input_ev.delta = (rotary_accumulator_ > 0) ? 1 : -1;
                        rotary_accumulator_ = 0;

                        // Calculate velocity based on time between rotary events
                        auto now_tp = std::chrono::steady_clock::now();
                        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_rotary_event_time_).count();
                        last_rotary_event_time_ = now_tp;
                        // Map: <50ms = fast (1.0), >300ms = slow (0.0), linear between
                        if (dt <= 50) {
                            input_ev.velocity = 1.0f;
                        } else if (dt >= 300) {
                            input_ev.velocity = 0.0f;
                        } else {
                            input_ev.velocity = 1.0f - static_cast<float>(dt - 50) / 250.0f;
                        }
                    }
                }
            }
            
            if (input_ev.action != InputAction::NONE) {
                events.push_back(input_ev);
            }
            
            rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        }

        // The loop above ends on the first non-event return code, normally
        // -EAGAIN ("nothing pending"). -ENODEV means the node is gone: the
        // pad was unplugged. Drop it so device_caps() reports it as absent
        // (the wizard's unplug-abort path keys off exactly that) instead of
        // us re-reading a dead fd forever.
        if (device->is_joystick && rc == -ENODEV) {
            dead_devices.push_back(device.get());
        }
    }

    if (!dead_devices.empty()) {
        for (Device* d : dead_devices) {
            std::cout << "  Joystick disconnected, dropping: " << d->name << std::endl;
        }
        // A device that vanishes mid-hold never delivers its release event,
        // so the manager-level D-pad latch would stay set and
        // generate_dpad_repeats() would scroll the menu forever. Same
        // reasoning (and same fix) as reprobe_phone_remote()'s stale-grab
        // path; we can't know whether the dead pad was the one holding.
        dpad_held_x_ = 0;
        dpad_held_y_ = 0;
        devices_.erase(
            std::remove_if(devices_.begin(), devices_.end(),
                           [&dead_devices](const std::unique_ptr<Device>& d) {
                               return std::find(dead_devices.begin(),
                                                dead_devices.end(),
                                                d.get()) != dead_devices.end();
                           }),
            devices_.end());   // ~Device closes fd + frees libevdev
    }

    generate_dpad_repeats(events);
    return events;
}

void InputManager::generate_dpad_repeats(std::vector<InputEvent>& events) {
    auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    // X axis (left/right)
    if (dpad_held_x_ != 0) {
        double elapsed = now - dpad_press_time_x_;
        if (elapsed >= DPAD_INITIAL_DELAY) {
            double hz = (elapsed - DPAD_INITIAL_DELAY >= DPAD_ACCEL_THRESHOLD)
                        ? DPAD_REPEAT_FAST_HZ : DPAD_REPEAT_SLOW_HZ;
            double interval = 1.0 / hz;
            if (now - dpad_last_fire_x_ >= interval) {
                InputEvent ev;
                ev.action = InputAction::ROTATE;
                ev.delta = dpad_held_x_;
                ev.pressed = false;
                ev.velocity = 0.0f;
                events.push_back(ev);
                dpad_last_fire_x_ = now;
            }
        }
    }

    // Y axis (up/down)
    if (dpad_held_y_ != 0) {
        double elapsed = now - dpad_press_time_y_;
        if (elapsed >= DPAD_INITIAL_DELAY) {
            double hz = (elapsed - DPAD_INITIAL_DELAY >= DPAD_ACCEL_THRESHOLD)
                        ? DPAD_REPEAT_FAST_HZ : DPAD_REPEAT_SLOW_HZ;
            double interval = 1.0 / hz;
            if (now - dpad_last_fire_y_ >= interval) {
                InputEvent ev;
                ev.action = InputAction::ROTATE_VERTICAL;
                ev.delta = dpad_held_y_;
                ev.pressed = false;
                ev.velocity = 0.0f;
                events.push_back(ev);
                dpad_last_fire_y_ = now;
            }
        }
    }
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
    //
    // PS-style USB pad (DragonRise/Microntek 0079:0006) uses the "joystick"
    // button range (BTN_TRIGGER..BTN_BASE6 = 288..299):
    //   288 Triangle / 289 Circle / 290 Cross / 291 Square
    //   292 L1 / 293 R1 / 294 L2 / 295 R2 / 296 Select / 297 Start
    //
    // Operator preference for the Magic Dingus Box kiosk:
    //   Cross   → SELECT         (confirm — Americas PlayStation convention)
    //   Circle  → SETTINGS_MENU  (open kiosk settings)
    //   Square  → unassigned (free to remap later)
    //   Triangle → PLAY_PAUSE
    //   L1 / R1  → PREV / NEXT

    switch (code) {
        case 304:  // BTN_SOUTH (standard A button)
        case 306:  // A button (N64)
        case 316:  // START
        case 290:  // Cross (PS pad, joystick button 2 — ENTER)
        case 297:  // Start (PS pad)
            return InputAction::SELECT;
        case 305:  // BTN_EAST (B button)
        case 289:  // Circle (PS pad, joystick button 1 — MENU)
            return InputAction::SETTINGS_MENU;
        case 310:  // Z
        case 288:  // Triangle (PS pad)
            return InputAction::PLAY_PAUSE;
        case 309:  // R
        case 293:  // R1 (PS pad)
            return InputAction::NEXT;
        case 308:  // L
        case 292:  // L1 (PS pad)
            return InputAction::PREV;
        default:
            return InputAction::NONE;
    }
}

// Extracted verbatim from map_axis_to_action's `axis == 0` branch so the
// overlay path can reuse it for a pad whose stick-X is NOT ABS_X. Both
// callers therefore share last_rotate_dir_/last_rotate_time_ and behave
// identically; at most one of them runs for any given event.
//
// `deflection` is signed distance from the axis's rest centre and `deadzone`
// the threshold in that same unit. The hardcoded ABS_X caller passes the raw
// value with AXIS_DEADZONE, which is centre 0 / threshold 5000 -- literally
// the constants this function used to hardcode, so that path is unchanged.
// The overlay caller passes a centred value and a threshold scaled from the
// device's reported min/max, which is what makes an unsigned-range axis work.
InputAction InputManager::rotate_from_axis_value(int32_t deflection, int32_t deadzone) {
    const int32_t value = deflection;

    int dir = 0;
    if (value > deadzone) {
        dir = 1;
    } else if (value < -deadzone) {
        dir = -1;
    }

    if (dir != 0) {
        auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (dir != last_rotate_dir_) {
            // Direction changed - fire immediately
            last_rotate_dir_ = dir;
            last_rotate_time_ = now;
            return InputAction::ROTATE;
        } else if (now - last_rotate_time_ >= (1.0 / ROTATE_REPEAT_HZ)) {
            // Same direction held - fire at repeat rate
            last_rotate_time_ = now;
            return InputAction::ROTATE;
        }
    }
    if (dir == 0) {
        last_rotate_dir_ = 0;
    }
    return InputAction::NONE;
}

InputAction InputManager::map_axis_to_action(uint8_t axis, int16_t value) {
    // Axis 0 = X (left/right) -> ROTATE
    // Axis 3 = C-stick horizontal -> NEXT/PREV
    // Axis 2 = C-stick vertical -> unused

    // Apply deadzone to avoid drift. This path is the pre-overlay one and is
    // deliberately left as it always was: raw value, centre 0, +/-5000 --
    // which is what AXIS_DEADZONE is.
    const int32_t deadzone = AXIS_DEADZONE;  // ~15% of half-range

    if (axis == 0) {
        return rotate_from_axis_value(value, AXIS_DEADZONE);
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
    // Note: Arrow keys (Left/Right/Up/Down) are handled in poll() as ROTATE/ROTATE_VERTICAL
    switch (code) {
        case KEY_ENTER:
        case KEY_SPACE:
            return InputAction::SELECT;
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

const MenuNavOverlay* InputManager::lookup_overlay(uint16_t vid, uint16_t pid) const {
    // 0000:0000 is what a device with no readable USB identity reports; it is
    // not a real pad, so never let it collide with a stored overlay.
    if (vid == 0 && pid == 0) return nullptr;
    auto it = overlays_.find((static_cast<uint32_t>(vid) << 16) | pid);
    return it == overlays_.end() ? nullptr : &it->second;
}

void InputManager::set_menu_overlays(std::map<uint32_t, MenuNavOverlay> overlays) {
    // Drop every borrowed pointer BEFORE the old map dies. The assignment
    // below destroys the old map's nodes, so any Device still pointing into
    // it would be left dangling -- and poll() dereferences that pointer on
    // every event, so a dangling one is not a latent bug, it's an immediate
    // crash on the next button press.
    for (auto& device : devices_) {
        device->overlay = nullptr;
    }
    overlays_ = std::move(overlays);
    for (auto& device : devices_) {
        device->overlay = lookup_overlay(device->vid, device->pid);
    }
}

void InputManager::set_raw_capture(bool enabled) {
    // Idempotent: repeating the current state must NOT discard events the
    // wizard hasn't drained yet (a UI that calls this every frame is easy to
    // write and would otherwise lose every capture).
    if (raw_capture_ == enabled) return;
    raw_capture_ = enabled;

    // Queue is per-capture-session; never carry events across an edge.
    raw_events_.clear();

    // Both edges of a capture swallow (or stop swallowing) joystick events
    // mid-gesture, so a direction the user was holding may never deliver its
    // release. Clear the latches or generate_dpad_repeats() would scroll the
    // menu forever; clear the per-device 8-bit edge memory too, so the first
    // post-capture push registers as a fresh transition instead of matching
    // a stale "still held" value and doing nothing.
    dpad_held_x_ = 0;
    dpad_held_y_ = 0;
    for (auto& device : devices_) {
        device->dpad_x_8bit_last = 0;
        device->dpad_y_8bit_last = 0;
    }
}

std::vector<RawInputEvent> InputManager::drain_raw_events() {
    std::vector<RawInputEvent> out;
    out.swap(raw_events_);   // move-and-clear
    return out;
}

std::optional<retroarch::CaptureDeviceCaps> InputManager::device_caps(uint16_t vid,
                                                                     uint16_t pid) {
    for (auto& device : devices_) {
        if (!device->is_joystick || device->vid != vid || device->pid != pid) {
            continue;
        }
        retroarch::CaptureDeviceCaps caps;
        caps.vid = vid;
        caps.pid = pid;
        caps.name = device->name;

        // Gamepad/joystick buttons live in [BTN_MISC, KEY_MAX_of_that_block]
        // = 0x100..0x2ff. Ascending by construction.
        for (unsigned code = 0x100; code <= 0x2ff; ++code) {
            if (libevdev_has_event_code(device->dev, EV_KEY, code)) {
                caps.key_codes.push_back(static_cast<uint16_t>(code));
            }
        }
        // Every ABS code including the hats (ABS_MAX == 0x3f). Ascending.
        for (unsigned code = 0; code <= 0x3f; ++code) {
            if (!libevdev_has_event_code(device->dev, EV_ABS, code)) continue;
            retroarch::CaptureDeviceCaps::AxisRange range;
            range.code = static_cast<uint16_t>(code);
            range.min = libevdev_get_abs_minimum(device->dev, code);
            range.max = libevdev_get_abs_maximum(device->dev, code);
            // Current value = the axis's resting position right now, which is
            // what CaptureSession needs to tell "deflected" from "at rest".
            range.rest = libevdev_get_event_value(device->dev, EV_ABS, code);
            caps.axes.push_back(range);
        }
        return caps;
    }
    // Not open (never was, or unplugged and dropped by poll()).
    return std::nullopt;
}

void InputManager::cleanup() {
    for (auto& device : devices_) {
        if (device->dev) {
            libevdev_grab(device->dev, LIBEVDEV_UNGRAB);
        }
    }
    devices_.clear();
    rotary_accumulator_ = 0;
    dpad_held_x_ = 0;
    dpad_held_y_ = 0;
}

} // namespace platform

