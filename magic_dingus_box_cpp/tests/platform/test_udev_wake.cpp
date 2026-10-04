// platform::udev — which input nodes the controller wake triggers. The
// phone remote's uinput pad (under /sys/devices/virtual/) used to be
// triggered too and failed with "Permission denied" on every wake; real
// pads must be selected exactly as `--sysname-match=js*` / `event*` did.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "platform/udev_wake.h"

namespace fs = std::filesystem;
using platform::udev::is_virtual_device_path;
using platform::udev::real_input_nodes;
using platform::udev::trigger_argv;

namespace {

// A fake sysfs: <root>/devices/... holds the device dirs, <root>/class/input
// holds the symlinks, mirroring the real layout.
struct FakeSysfs {
    fs::path root;
    FakeSysfs() {
        root = fs::temp_directory_path() /
               ("mdb-udev-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "class/input");
    }
    ~FakeSysfs() { std::error_code ec; fs::remove_all(root, ec); }
    void node(const std::string& name, const std::string& device_rel) {
        const fs::path dev = root / device_rel / name;
        fs::create_directories(dev);
        fs::create_directory_symlink(dev, root / "class/input" / name);
    }
    std::string cls() const { return (root / "class/input").string(); }
    std::string link(const std::string& name) const {
        return (root / "class/input" / name).string();
    }
};

}  // namespace

TEST_CASE("udev: virtual device detection", "[udev]") {
    CHECK(is_virtual_device_path("/sys/devices/virtual/input/input12/js0"));
    CHECK(is_virtual_device_path("/sys/devices/virtual/input/input12/event7"));
    CHECK_FALSE(is_virtual_device_path(
        "/sys/devices/platform/scb/fd500000.pcie/pci0000:00/0000:00:00.0/"
        "0000:01:00.0/usb1/1-1/1-1.3/1-1.3:1.0/0003:0079:0006.0001/input/input3/js0"));
    CHECK_FALSE(is_virtual_device_path("/sys/devices/platform/rotary@11/input/input1/event1"));
}

TEST_CASE("udev: real nodes kept, uinput phone remote skipped", "[udev]") {
    FakeSysfs s;
    s.node("js0", "devices/virtual/input/input12");             // phone remote
    s.node("event7", "devices/virtual/input/input12");
    s.node("js1", "devices/platform/scb/usb1/1-1.3/input/input3");  // USB pad
    s.node("event3", "devices/platform/scb/usb1/1-1.3/input/input3");
    s.node("event1", "devices/platform/rotary@11/input/input1");     // encoder
    s.node("mouse0", "devices/platform/scb/usb1/1-1.4/input/input4");

    CHECK(real_input_nodes(s.cls(), "js") == std::vector<std::string>{s.link("js1")});
    CHECK(real_input_nodes(s.cls(), "event") ==
          std::vector<std::string>{s.link("event1"), s.link("event3")});
}

TEST_CASE("udev: a missing class dir selects nothing", "[udev]") {
    CHECK(real_input_nodes("/nonexistent/mdb/class/input", "js").empty());
}

TEST_CASE("udev: only-virtual pads select nothing", "[udev]") {
    FakeSysfs s;
    s.node("js0", "devices/virtual/input/input12");
    CHECK(real_input_nodes(s.cls(), "js").empty());
}

TEST_CASE("udev: trigger argv", "[udev]") {
    // Empty selection runs NOTHING: a bare `udevadm trigger` would
    // re-trigger every device on the system.
    CHECK(trigger_argv({}).empty());
    CHECK(trigger_argv({"/sys/class/input/js1", "/sys/class/input/js2"}) ==
          std::vector<std::string>{"sudo", "udevadm", "trigger", "--action=change",
                                   "/sys/class/input/js1", "/sys/class/input/js2"});
}
