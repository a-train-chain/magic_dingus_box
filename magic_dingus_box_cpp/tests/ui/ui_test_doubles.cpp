// Link-seam doubles for test_ui_unit.
//
// ControllerWizard's collaborators are two objects the Mac build cannot
// link: platform::InputManager (libevdev) and ui::Toast (whose .cpp includes
// renderer.h, hence GLES). Both are reached through a handful of
// non-virtual functions, so the cheapest honest seam is the linker: this TU
// supplies definitions for exactly the members the wizard calls, and
// input_manager.cpp / toast.cpp are simply not part of this binary.
//
// The signatures are checked against the real headers by the compiler, so a
// change to either API breaks this file loudly rather than silently drifting.
// Nothing here reimplements behavior the wizard depends on beyond what the
// real classes document:
//   - set_raw_capture is idempotent (repeating a state is a no-op),
//   - device_caps returns nullopt for a device that isn't open.

#include "ui_test_doubles.h"

#include <string>
#include <vector>

#include "platform/input_manager.h"
#include "ui/toast.h"

namespace {
std::optional<retroarch::CaptureDeviceCaps> g_caps;
bool g_raw_capture = false;
int g_raw_capture_calls = 0;
std::string g_last_toast;
}  // namespace

namespace ui_test {

void fake_set_caps(retroarch::CaptureDeviceCaps caps) { g_caps = std::move(caps); }
void fake_unplug() { g_caps.reset(); }
void fake_reset() {
    g_caps.reset();
    g_raw_capture = false;
    g_raw_capture_calls = 0;
    g_last_toast.clear();
}
bool fake_raw_capture_enabled() { return g_raw_capture; }
int fake_raw_capture_calls() { return g_raw_capture_calls; }
std::string fake_last_toast() { return g_last_toast; }
void fake_clear_toast() { g_last_toast.clear(); }

}  // namespace ui_test

// ---------------------------------------------------------------------------
// platform::InputManager
// ---------------------------------------------------------------------------

// devices_ is a vector<unique_ptr<Device>> of an incomplete type; the
// destructor below needs it complete.
struct platform::InputManager::Device {};

namespace platform {

InputManager::InputManager() : last_rotate_dir_(0), last_rotate_time_(0.0) {}
InputManager::~InputManager() = default;

void InputManager::set_raw_capture(bool enabled) {
    if (raw_capture_ == enabled) return;   // idempotent, as documented
    raw_capture_ = enabled;
    g_raw_capture = enabled;
    ++g_raw_capture_calls;
    raw_events_.clear();
}

std::vector<RawInputEvent> InputManager::drain_raw_events() {
    std::vector<RawInputEvent> out;
    out.swap(raw_events_);
    return out;
}

std::optional<retroarch::CaptureDeviceCaps> InputManager::device_caps(uint16_t vid,
                                                                     uint16_t pid) {
    if (!g_caps) return std::nullopt;
    if (g_caps->vid != vid || g_caps->pid != pid) return std::nullopt;
    return g_caps;
}

}  // namespace platform

// ---------------------------------------------------------------------------
// ui::Toast — statics + the one entry point the wizard uses. render() is
// deliberately absent; it needs a Renderer and is never called here.
// ---------------------------------------------------------------------------

namespace ui {

std::string Toast::message_;
std::chrono::steady_clock::time_point Toast::shown_at_;
bool Toast::active_ = false;

void Toast::show(std::string message) {
    message_ = message;
    shown_at_ = std::chrono::steady_clock::now();
    active_ = true;
    g_last_toast = std::move(message);
}

void Toast::clear() {
    active_ = false;
    message_.clear();
}

bool Toast::is_active() { return active_; }

}  // namespace ui
