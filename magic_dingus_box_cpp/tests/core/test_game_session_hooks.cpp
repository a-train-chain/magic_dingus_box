#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <vector>

#include "app/controller.h"
#include "app/game_session_hooks.h"

namespace {
struct RecordingHooks : app::GameSessionHooks {
    std::vector<std::string> calls;
    void release_input() override { calls.push_back("release_input"); }
    void wake_controllers(const std::function<void()>& after) override {
        calls.push_back("wake_controllers");
        if (after) after();
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
    controller.set_host_hooks(&hooks);
    REQUIRE(controller.host_hooks() == &hooks);
    controller.set_host_hooks(nullptr);
    REQUIRE(controller.host_hooks() == nullptr);
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

TEST_CASE("Only a re-acquire that was attempted and failed loses the display", "[core][controller]") {
    // The Pi: DRM master not re-acquired after a game -> the kiosk must
    // exit non-zero so systemd restarts it (Controller::display_lost()).
    REQUIRE(app::display_was_lost(app::DisplayReacquire{true, false, false}));
    REQUIRE_FALSE(app::display_was_lost(app::DisplayReacquire{true, true, true}));
    REQUIRE_FALSE(app::display_was_lost(app::DisplayReacquire{true, true, false}));
    // A host with no display to reacquire (the base default: the headless
    // engine, a Pi run without a DrmDisplay) must never read as lost, or
    // every game exit would end the process.
    REQUIRE_FALSE(app::display_was_lost(app::GameSessionHooks{}.reacquire_display(1280, 720)));
}

TEST_CASE("After a game, the reacquire result decides the restored-mode flag and a lost display", "[core][controller]") {
    // What Controller::load_playlist_item does with reacquire_display()'s
    // answer: store mode_restored only when a display was reacquired at all,
    // and report a lost display so the kiosk exits for systemd to restart.
    std::atomic<bool> mode_restored{true};
    REQUIRE(app::apply_display_reacquire(app::DisplayReacquire{true, false, false}, mode_restored));
    REQUIRE_FALSE(mode_restored.load());

    REQUIRE_FALSE(app::apply_display_reacquire(app::DisplayReacquire{true, true, true}, mode_restored));
    REQUIRE(mode_restored.load());

    REQUIRE_FALSE(app::apply_display_reacquire(app::DisplayReacquire{true, true, false}, mode_restored));
    REQUIRE_FALSE(mode_restored.load());

    // No display to reacquire (the headless engine): the flag is untouched
    // and the display is not lost.
    mode_restored.store(true);
    REQUIRE_FALSE(app::apply_display_reacquire(app::GameSessionHooks{}.reacquire_display(1280, 720), mode_restored));
    REQUIRE(mode_restored.load());
}
