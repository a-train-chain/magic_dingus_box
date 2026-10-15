// "Forget network" must not run nmcli on the render thread.
//
// The Settings > Wi-Fi confirm press called WifiManager::forget_network()
// synchronously: two `sudo nmcli` calls bounded at 15 s EACH. A wedged
// NetworkManager D-Bus call therefore froze the kiosk for up to 30 s, and
// systemd's WatchdogSec=10 kills a kiosk that stops pinging that long — the
// operator's "forget this network" press restarted the appliance.
//
// The nmcli layer is replaced by WifiManager's command-runner seam, so these
// tests never fork `sudo` on the dev machine and can make nmcli "hang" on
// demand.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app_state.h"
#include "ui/settings_menu.h"
#include "ui_test_doubles.h"
#include "utils/wifi_manager.h"

using utils::WifiManager;
using Clock = std::chrono::steady_clock;

namespace {

bool contains(const std::vector<std::string>& args, const std::string& word) {
    for (const auto& a : args) if (a == word) return true;
    return false;
}

// Fake nmcli: connected to "TestNet"; `connection down|delete` block until
// release() so a test can observe the caller while nmcli is "wedged".
class FakeNmcli {
public:
    FakeNmcli() : gate_(release_.get_future().share()) {
        WifiManager::instance().set_command_runner_for_tests(
            [this](const std::vector<std::string>& args, int) -> std::string {
                if (contains(args, "down") || contains(args, "delete")) {
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        forget_calls_.push_back(args);
                    }
                    gate_.wait();
                    return contains(args, "delete")
                               ? "Connection 'TestNet' successfully deleted."
                               : "Connection 'TestNet' successfully deactivated.";
                }
                if (contains(args, "CONNECTIVITY")) return "full\n";
                if (contains(args, "GENERAL.CONNECTION")) {
                    return "GENERAL.CONNECTION:TestNet\n";
                }
                return "";
            });
        WifiManager::instance().invalidate_status_cache();
    }
    ~FakeNmcli() {
        release();
        // Let any in-flight worker drain before the seam goes away.
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (WifiManager::instance().is_forgetting() &&
               Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        WifiManager::instance().set_command_runner_for_tests(nullptr);
        WifiManager::instance().invalidate_status_cache();
    }
    void release() {
        if (!released_.exchange(true)) release_.set_value();
    }
    std::vector<std::vector<std::string>> forget_calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return forget_calls_;
    }

private:
    std::promise<void> release_;
    std::shared_future<void> gate_;
    std::atomic<bool> released_{false};
    std::mutex mu_;
    std::vector<std::vector<std::string>> forget_calls_;
};

// Bounded wait (an unbounded loop would hang the suite on a regression).
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds budget = std::chrono::milliseconds(3000)) {
    const auto deadline = Clock::now() + budget;
    while (!pred()) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

const ui::MenuItem* find_item(const ui::SettingsMenuManager& m,
                              const std::string& label) {
    for (const auto& it : m.get_submenu_items()) {
        if (it.label == label) return &it;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("forget_network_async returns while nmcli is still running",
          "[wifi][forget]") {
    FakeNmcli nmcli;
    auto& wifi = WifiManager::instance();

    const auto t0 = Clock::now();
    REQUIRE(wifi.forget_network_async("TestNet"));
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(200));
    CHECK(wifi.is_forgetting());
    CHECK(wifi.get_forgetting_ssid() == "TestNet");

    // Single-flight: a second press while one is running is refused.
    CHECK_FALSE(wifi.forget_network_async("TestNet"));

    nmcli.release();
    REQUIRE(wait_for([&] { return !wifi.is_forgetting(); }));
    CHECK(wifi.last_forget_succeeded());
    CHECK(wifi.get_forgetting_ssid().empty());

    const auto calls = nmcli.forget_calls();
    REQUIRE(calls.size() == 2);
    CHECK(contains(calls[0], "down"));
    CHECK(contains(calls[1], "delete"));
    CHECK(contains(calls[1], "TestNet"));
}

TEST_CASE("forget_network_async refuses an empty SSID", "[wifi][forget]") {
    FakeNmcli nmcli;
    CHECK_FALSE(WifiManager::instance().forget_network_async(""));
    CHECK_FALSE(WifiManager::instance().is_forgetting());
}

TEST_CASE("Settings > Wi-Fi confirm-forget does not block the render thread",
          "[wifi][forget][settings_menu]") {
    FakeNmcli nmcli;
    ui_test::fake_clear_toast();
    app::AppState state;
    ui::SettingsMenuManager menu(&state);
    menu.open();
    menu.enter_submenu(ui::MenuSection::WIFI);

    // The status snapshot refreshes on a worker; wait for it to report the
    // fake connection, then rebuild so the Disconnect row appears.
    REQUIRE(wait_for([&] {
        auto s = WifiManager::instance().get_status_cached();
        return s.valid && s.connected && s.ssid == "TestNet";
    }));
    menu.rebuild_current_submenu();

    const ui::MenuItem* disconnect = find_item(menu, "Disconnect from TestNet");
    REQUIRE(disconnect);
    REQUIRE(disconnect->action);
    disconnect->action();
    // main.cpp re-enters the WIFI submenu after every WIFI-section action.
    menu.enter_submenu(ui::MenuSection::WIFI);

    const ui::MenuItem* confirm = find_item(menu, "Confirm: forget TestNet?");
    REQUIRE(confirm);
    const auto action = confirm->action;  // the rebuild below invalidates it

    const auto t0 = Clock::now();
    action();
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(200));
    CHECK(WifiManager::instance().is_forgetting());

    // The outcome toast arrives via update() once the worker finishes.
    nmcli.release();
    REQUIRE(wait_for([&] {
        menu.update();
        return ui_test::fake_last_toast() == "Forgot network TestNet";
    }));
}
