#pragma once
#include "app/game_session_hooks.h"

namespace platform {
class DrmDisplay;
class InputManager;

class PiGameSessionHooks final : public app::GameSessionHooks {
public:
    PiGameSessionHooks(DrmDisplay* display, InputManager* input)
        : display_(display), input_(input) {}
    void release_input() override;
    void wake_controllers() override;
    void release_display() override;
    bool reacquire_display(uint32_t kiosk_w, uint32_t kiosk_h) override;
    bool reinit_input() override;
private:
    DrmDisplay* display_;
    InputManager* input_;
    static void run_udevadm(const char* match);
};
}  // namespace platform
