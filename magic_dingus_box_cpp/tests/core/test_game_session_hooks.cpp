#include <catch2/catch_test_macros.hpp>

#include "app/controller.h"
#include "app/game_session_hooks.h"

namespace {
struct RecordingHooks : app::GameSessionHooks {
    std::vector<std::string> calls;
    void release_input() override { calls.push_back("release_input"); }
    void wake_controllers(const std::function<void()>& between) override {
        calls.push_back("wake_controllers");
        if (between) between();
    }
    void release_display() override { calls.push_back("release_display"); }
    app::DisplayReacquire reacquire_display(uint32_t w, uint32_t h) override {
        calls.push_back("reacquire_display " + std::to_string(w) + "x" + std::to_string(h));
        return app::DisplayReacquire{true, true, true};
    }
    bool reinit_input() override { calls.push_back("reinit_input"); return true; }
};
}  // namespace

TEST_CASE("Controller accepts hooks and defaults to none", "[core][controller]") {
    app::Controller controller;
    RecordingHooks hooks;
    controller.set_game_session_hooks(&hooks);
    REQUIRE(controller.game_session_hooks() == &hooks);
    controller.set_game_session_hooks(nullptr);
    REQUIRE(controller.game_session_hooks() == nullptr);
}

TEST_CASE("Default hooks are no-ops that report success", "[core][controller]") {
    app::GameSessionHooks hooks;
    hooks.release_input();
    hooks.wake_controllers(nullptr);
    hooks.release_display();
    // A host with no display to reacquire (the base default) reports
    // attempted=false: nothing gets stored into state and no return
    // dissolve is painted, matching the pre-seam behavior when display_
    // was null.
    REQUIRE(!hooks.reacquire_display(1280, 720).attempted);
    REQUIRE(hooks.reinit_input());
}
