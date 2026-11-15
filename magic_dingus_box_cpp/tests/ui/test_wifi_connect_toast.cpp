// The Wi-Fi "Connected to <SSID>" toast must not run nmcli on the render
// thread.
//
// SettingsMenuManager::update() runs every frame on the thread that pings
// the systemd watchdog (WatchdogSec=10). On the connect-success edge it
// called WifiManager::get_current_ssid() — up to two synchronous nmcli
// invocations with a 15 s timeout each — just to name the network in the
// toast. The connect worker already knows the SSID; it records it, and the
// toast reads that.
//
// The nmcli layer is replaced by WifiManager's command-runner seam, which
// records the thread each invocation ran on.

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

bool has_word(const std::vector<std::string>& args, const std::string& word) {
    for (const auto& a : args) if (a == word) return true;
    return false;
}

// Fake nmcli for a connect: `dev wifi connect` / `connection up` block
// until release(), then report success or failure.
class FakeConnectNmcli {
public:
    explicit FakeConnectNmcli(bool succeed)
        : gate_(release_.get_future().share()) {
        WifiManager::instance().reset_connection_state();
        WifiManager::instance().set_command_runner_for_tests(
            [this, succeed](const std::vector<std::string>& args, int) -> std::string {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    threads_.push_back(std::this_thread::get_id());
                }
                if (has_word(args, "connect") || has_word(args, "up")) {
                    gate_.wait();
                    return succeed
                        ? "Device 'wlan0' successfully activated with 'abc'.\n"
                        : "Error: No network with SSID 'HomeNet' found.\n";
                }
                if (has_word(args, "GENERAL.CONNECTION")) {
                    return "GENERAL.CONNECTION:SomethingElse\n";
                }
                return "";
            });
        WifiManager::instance().invalidate_status_cache();
    }
    ~FakeConnectNmcli() {
        release();
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (WifiManager::instance().is_connecting() && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Let a status refresh the menu may have started finish too.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        WifiManager::instance().set_command_runner_for_tests(nullptr);
        WifiManager::instance().reset_connection_state();
        WifiManager::instance().invalidate_status_cache();
    }
    void release() {
        if (!released_.exchange(true)) release_.set_value();
    }
    // nmcli invocations that ran on `tid`.
    int calls_on(std::thread::id tid) {
        std::lock_guard<std::mutex> lock(mu_);
        int n = 0;
        for (const auto& t : threads_) if (t == tid) ++n;
        return n;
    }

private:
    std::promise<void> release_;
    std::shared_future<void> gate_;
    std::atomic<bool> released_{false};
    std::mutex mu_;
    std::vector<std::thread::id> threads_;
};

bool wait_until_idle(WifiManager& wifi) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (wifi.is_connecting()) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

}  // namespace

TEST_CASE("Wi-Fi connect success toast names the SSID without running nmcli "
          "on the render thread",
          "[wifi][connect][settings_menu]") {
    FakeConnectNmcli nmcli(/*succeed=*/true);
    ui_test::fake_clear_toast();
    auto& wifi = WifiManager::instance();
    app::AppState state;
    ui::SettingsMenuManager menu(&state);
    menu.open();

    wifi.connect_async("HomeNet", "hunter2hunter2", /*fresh_credentials=*/true);
    menu.update();   // sees the connect start
    CHECK(ui_test::fake_last_toast() == "Connecting to HomeNet...");

    nmcli.release();
    REQUIRE(wait_until_idle(wifi));
    CHECK(wifi.get_last_connected_ssid() == "HomeNet");

    const auto me = std::this_thread::get_id();
    const int before = nmcli.calls_on(me);
    const auto t0 = Clock::now();
    menu.update();   // the frame that sees the connect finish
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(200));
    CHECK(nmcli.calls_on(me) == before);   // no nmcli on this thread
    CHECK(ui_test::fake_last_toast() == "Connected to HomeNet");
}

TEST_CASE("Wi-Fi connect failure toast is unchanged and a failed connect "
          "does not overwrite the last connected SSID",
          "[wifi][connect][settings_menu]") {
    FakeConnectNmcli nmcli(/*succeed=*/false);
    ui_test::fake_clear_toast();
    auto& wifi = WifiManager::instance();
    const std::string connected_before = wifi.get_last_connected_ssid();
    app::AppState state;
    ui::SettingsMenuManager menu(&state);
    menu.open();

    wifi.connect_async("HomeNet", "hunter2hunter2", /*fresh_credentials=*/true);
    menu.update();
    nmcli.release();
    REQUIRE(wait_until_idle(wifi));

    const auto me = std::this_thread::get_id();
    const int before = nmcli.calls_on(me);
    menu.update();
    CHECK(nmcli.calls_on(me) == before);
    CHECK(ui_test::fake_last_toast() == "Connection failed: Network not found");
    CHECK(wifi.get_last_connected_ssid() == connected_before);
}
