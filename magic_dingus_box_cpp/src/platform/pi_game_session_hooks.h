#pragma once
#include "app/game_session_hooks.h"
#include <functional>

namespace platform {
class DrmDisplay;
class InputManager;

class PiGameSessionHooks final : public app::GameSessionHooks {
public:
    PiGameSessionHooks(DrmDisplay* display, InputManager* input)
        : display_(display), input_(input) {}
    void release_input() override;
    void wake_controllers(const std::function<void()>& between) override;
    void release_display() override;
    app::DisplayReacquire reacquire_display(uint32_t kiosk_w, uint32_t kiosk_h) override;
    bool reinit_input() override;
private:
    DrmDisplay* display_;
    InputManager* input_;
    static void run_udevadm(const char* match);
};
}  // namespace platform
