#include "pi_game_session_hooks.h"
#include "drm_display.h"
#include "input_manager.h"
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace platform {

void PiGameSessionHooks::run_udevadm(const char* match) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("sudo", "sudo", "udevadm", "trigger", "--action=change", match, nullptr);
        _exit(127);
    }
    if (pid > 0) { int s; waitpid(pid, &s, 0); }
}

void PiGameSessionHooks::release_input() {
    if (!input_) return;
    std::cout << "Releasing input devices for RetroArch..." << std::endl;
    input_->cleanup();
    std::cout << "Input devices released" << std::endl;
}

void PiGameSessionHooks::wake_controllers() {
    std::cout << "Waking up controller before RetroArch launch..." << std::endl;
    run_udevadm("--sysname-match=js*");
    run_udevadm("--sysname-match=event*");
}

void PiGameSessionHooks::release_display() {
    if (!display_) return;
    // CRITICAL: Keep CRTC enabled (disable_crtc = false) for Vulkan
    // compatibility. Disabling it causes "QueuePresent failed" on startup
    // for most cores (Genesis, SNES, NES, PS1).
    const bool disable_crtc = false;
    std::cout << "Releasing DRM master for RetroArch (disable_crtc=" << disable_crtc << ")..." << std::endl;
    display_->release_master(disable_crtc);
    std::cout << "DRM master released" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

bool PiGameSessionHooks::reacquire_display(uint32_t kiosk_w, uint32_t kiosk_h) {
    if (!display_) return true;
    std::cout << "Re-acquiring DRM master..." << std::endl;
    bool acquired = false;
    for (int i = 0; i < 5; ++i) {
        if (display_->acquire_master()) { acquired = true; std::cout << "DRM master acquired successfully." << std::endl; break; }
        std::cerr << "Failed to acquire DRM master, retrying (" << (i + 1) << "/5)..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!acquired) std::cerr << "CRITICAL: Failed to acquire DRM master after retries! Attempting to proceed anyway..." << std::endl;
    bool mode_restored = false;
    if (kiosk_w > 0 && kiosk_h > 0) {
        std::cout << "Restoring kiosk display mode " << kiosk_w << "x" << kiosk_h << "..." << std::endl;
        mode_restored = display_->set_mode(kiosk_w, kiosk_h);
    }
    if (!mode_restored) { std::cout << "Falling back to 640x480..." << std::endl; display_->set_mode(640, 480); }
    return mode_restored;
}

bool PiGameSessionHooks::reinit_input() {
    if (!input_) return true;
    std::cout << "Re-initializing input devices after RetroArch..." << std::endl;
    for (int i = 0; i < 3; ++i) {
        run_udevadm("--sysname-match=js*");
        run_udevadm("--sysname-match=event*");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (input_->initialize()) { std::cout << "Input devices initialized successfully." << std::endl; return true; }
        std::cerr << "Failed to initialize input devices, retrying (" << (i + 1) << "/3)..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cerr << "CRITICAL: Failed to re-initialize input devices after 3 retries!" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    run_udevadm("--sysname-match=js*");
    run_udevadm("--sysname-match=event*");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (input_->initialize()) { std::cout << "Input devices initialized on final retry." << std::endl; return true; }
    std::cerr << "CRITICAL: Input devices permanently failed. Controller may not work." << std::endl;
    return false;
}

}  // namespace platform
