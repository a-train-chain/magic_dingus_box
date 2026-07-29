// Link-seam doubles for test_ui_unit.
//
// The real ui/controller_wizard.cpp and ui/settings_menu.cpp are both in this
// binary; what they reach for that the Mac build cannot link is supplied here
// as linker-level definitions, and the corresponding .cpp files are simply not
// part of the target. All of these are reached through non-virtual functions,
// so the linker is the cheapest honest seam.
//
//   platform::InputManager   real .cpp needs libevdev
//   ui::Toast                real .cpp includes renderer.h, hence GLES
//   ui::PairingScreen        real .cpp includes utils/logger.h, hence spdlog
//   app::SettingsPersistence ditto, and save_settings() writes settings.json
//                            for real -- a unit test must not touch it
//
// Everything else settings_menu.cpp needs links against the REAL source
// (wifi_manager.cpp, virtual_keyboard.cpp, platform_profile.cpp, config.cpp):
// those compile clean on macOS, have no project-level link tail of their own,
// and are inert unless called, so doubling them would buy nothing.
//
// The signatures are checked against the real headers by the compiler, so a
// change to any of these APIs breaks this file loudly rather than silently
// drifting. Nothing here reimplements behavior the code under test depends on
// beyond what the real classes document:
//   - set_raw_capture is idempotent (repeating a state is a no-op),
//   - device_caps returns nullopt for a device that isn't open.

#include "ui_test_doubles.h"

#include <string>
#include <vector>

#include "app/settings_persistence.h"
#include "platform/input_manager.h"
#include "ui/pairing_screen.h"
#include "ui/toast.h"

namespace {
std::optional<retroarch::CaptureDeviceCaps> g_caps;
bool g_raw_capture = false;
int g_raw_capture_calls = 0;
std::string g_last_toast;
int g_pairing_calls = 0;
int g_settings_saves = 0;
}  // namespace

namespace ui_test {

void fake_set_caps(retroarch::CaptureDeviceCaps caps) { g_caps = std::move(caps); }
void fake_unplug() { g_caps.reset(); }
void fake_reset() {
    g_caps.reset();
    g_raw_capture = false;
    g_raw_capture_calls = 0;
    g_last_toast.clear();
    g_pairing_calls = 0;
    g_settings_saves = 0;
}
bool fake_raw_capture_enabled() { return g_raw_capture; }
int fake_raw_capture_calls() { return g_raw_capture_calls; }
std::string fake_last_toast() { return g_last_toast; }
void fake_clear_toast() { g_last_toast.clear(); }
int fake_pairing_calls() { return g_pairing_calls; }
int fake_settings_saves() { return g_settings_saves; }

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

// ---------------------------------------------------------------------------
// ui::PairingScreen — the three members settings_menu.cpp references. The
// wizard tests never open the pairing screen, so these are never called; they
// exist only so close_pairing_screen()'s (dead) branch links. Counted anyway,
// so a test that DID reach them would show it rather than pass quietly.
// ---------------------------------------------------------------------------

PairingScreen::PairingScreen(std::string session_path)
    : session_path_(std::move(session_path)) {}

void PairingScreen::regenerate() { ++g_pairing_calls; }
void PairingScreen::close() { ++g_pairing_calls; }

}  // namespace ui

// ---------------------------------------------------------------------------
// app::SettingsPersistence::save_settings — settings_menu.cpp calls this from
// the toggle handlers. Never from any wizard path, and a unit test must not
// write the real settings.json, so it is a counted no-op.
// ---------------------------------------------------------------------------

namespace app {

utils::Result<> SettingsPersistence::save_settings(const AppState&) {
    ++g_settings_saves;
    return utils::Result<>();
}

}  // namespace app
