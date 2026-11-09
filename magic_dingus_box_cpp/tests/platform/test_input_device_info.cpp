// platform::input_device_info — which /dev/input nodes the kiosk treats as a
// controller, and what sysfs says about a node (driver, IDs, capabilities).
//
// Origin of the joystick filter: a DualSense (PS5) pad creates THREE input
// devices — the gamepad, a motion-sensor device and a touchpad — and the
// last two both report ABS_X. The kiosk's old "has ABS_X or a hat" test
// opened all three as joysticks, so tilting the controller scrolled the
// menu. The kernel's own joydev applies the same exclusions (touch, digitizer,
// accelerometer), which is why no js node exists for them and RetroArch never
// saw the problem.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "platform/input_device_info.h"

namespace fs = std::filesystem;
using platform::InputDeviceTraits;
using platform::is_joystick_device;
using platform::parse_capability_bitmap;
using platform::read_sysfs_input_info;

TEST_CASE("joystick filter: a pad with a stick is a joystick", "[input_device]") {
    InputDeviceTraits t;
    t.has_abs_x = true;
    CHECK(is_joystick_device(t));
}

TEST_CASE("joystick filter: a d-pad-only pad (hat, no stick) is a joystick",
          "[input_device]") {
    InputDeviceTraits t;
    t.has_abs_hat0x = true;
    CHECK(is_joystick_device(t));
}

TEST_CASE("joystick filter: no stick and no hat is not a joystick", "[input_device]") {
    CHECK_FALSE(is_joystick_device(InputDeviceTraits{}));
}

TEST_CASE("joystick filter: a motion-sensor device is not a joystick",
          "[input_device]") {
    // DualSense / DualShock 4 "Motion Sensors": ABS_X..ABS_RZ carry the
    // accelerometer and gyro, flagged INPUT_PROP_ACCELEROMETER.
    InputDeviceTraits t;
    t.has_abs_x = true;
    t.accelerometer = true;
    CHECK_FALSE(is_joystick_device(t));
}

TEST_CASE("joystick filter: a touchpad is not a joystick", "[input_device]") {
    // DualSense "Touchpad": multitouch with ABS_X/ABS_Y and BTN_TOUCH.
    InputDeviceTraits t;
    t.has_abs_x = true;
    t.has_btn_touch = true;
    CHECK_FALSE(is_joystick_device(t));
}

TEST_CASE("joystick filter: a pen tablet is not a joystick", "[input_device]") {
    InputDeviceTraits t;
    t.has_abs_x = true;
    t.has_btn_digi = true;
    CHECK_FALSE(is_joystick_device(t));
}

TEST_CASE("capability bitmap: 64-bit words, most significant first",
          "[input_device]") {
    // /sys/class/input/inputN/capabilities/key of a DualSense gamepad.
    const auto codes = parse_capability_bitmap("7fdb000000000000 0 0 0 0");
    const std::vector<uint16_t> want = {0x130, 0x131, 0x133, 0x134, 0x136,
                                        0x137, 0x138, 0x139, 0x13a, 0x13b,
                                        0x13c, 0x13d, 0x13e};
    CHECK(codes == want);
}

TEST_CASE("capability bitmap: a single word holds the low codes", "[input_device]") {
    // capabilities/abs: X Y Z RX RY RZ + HAT0X/Y.
    const auto codes = parse_capability_bitmap("3003f\n");
    const std::vector<uint16_t> want = {0, 1, 2, 3, 4, 5, 0x10, 0x11};
    CHECK(codes == want);
}

TEST_CASE("capability bitmap: 32-bit kernels print 32-bit words", "[input_device]") {
    const auto codes =
        parse_capability_bitmap("7fdb0000 0 0 0 0 0 0 0 0 0", /*word_bits=*/32);
    REQUIRE(codes.size() == 13);
    CHECK(codes.front() == 0x130);
    CHECK(codes.back() == 0x13e);
}

TEST_CASE("capability bitmap: empty or malformed text yields nothing",
          "[input_device]") {
    CHECK(parse_capability_bitmap("").empty());
    CHECK(parse_capability_bitmap("0").empty());
    CHECK(parse_capability_bitmap("zz 12").empty());
}

namespace {

// A fake /sys/class/input with one node, shaped like the real layout:
//   class/input/<node>/device        -> the inputN directory
//   inputN/{name,properties,id/*,capabilities/*}
//   inputN/device                    -> the parent (HID or USB) device
//   <parent>/driver                  -> drivers/<driver name>
struct FakeInputSysfs {
    fs::path root;
    FakeInputSysfs() {
        root = fs::temp_directory_path() /
               ("mdb-inputinfo-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "class/input");
    }
    ~FakeInputSysfs() { std::error_code ec; fs::remove_all(root, ec); }

    static void write(const fs::path& p, const std::string& s) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << s;
    }

    // `driver` empty = the parent has no driver link.
    void add(const std::string& node, const std::string& driver,
             const std::string& bustype, const std::string& vendor,
             const std::string& product, const std::string& name,
             const std::string& key, const std::string& abs,
             const std::string& props = "0") {
        const fs::path parent = root / "devices/parent" / node;
        const fs::path input = parent / "input/input9";
        write(input / "name", name + "\n");
        write(input / "properties", props + "\n");
        write(input / "id/bustype", bustype + "\n");
        write(input / "id/vendor", vendor + "\n");
        write(input / "id/product", product + "\n");
        write(input / "capabilities/key", key + "\n");
        write(input / "capabilities/abs", abs + "\n");
        fs::create_directory_symlink(parent, input / "device");
        if (!driver.empty()) {
            const fs::path drv = root / "bus/drivers" / driver;
            fs::create_directories(drv);
            fs::create_directory_symlink(drv, parent / "driver");
        }
        fs::create_directories(root / "class/input" / node);
        fs::create_directory_symlink(input, root / "class/input" / node / "device");
    }
    std::string cls() const { return (root / "class/input").string(); }
};

}  // namespace

TEST_CASE("sysfs: a Bluetooth DualSense node reads back in full", "[input_device]") {
    FakeInputSysfs s;
    s.add("js0", "playstation", "0005", "054c", "0ce6",
          "DualSense Wireless Controller", "7fdb000000000000 0 0 0 0", "3003f");
    const auto info = read_sysfs_input_info(s.cls(), "js0");
    REQUIRE(info.has_value());
    CHECK(info->driver == "playstation");
    CHECK(info->bustype == 0x0005);
    CHECK(info->vid == 0x054c);
    CHECK(info->pid == 0x0ce6);
    CHECK(info->name == "DualSense Wireless Controller");
    CHECK(info->key_codes.size() == 13);
    CHECK(info->abs_codes == std::vector<uint16_t>{0, 1, 2, 3, 4, 5, 0x10, 0x11});
    CHECK_FALSE(info->accelerometer);
}

TEST_CASE("sysfs: a node with no driver link still reads, with an empty driver",
          "[input_device]") {
    FakeInputSysfs s;
    s.add("event3", "", "0003", "0079", "0006", "Generic   USB  Joystick  ",
          "fff00000000 0 0 0 0", "3");
    const auto info = read_sysfs_input_info(s.cls(), "event3");
    REQUIRE(info.has_value());
    CHECK(info->driver.empty());
    CHECK(info->vid == 0x0079);
}

TEST_CASE("sysfs: the accelerometer property is read from the properties bitmap",
          "[input_device]") {
    FakeInputSysfs s;
    // INPUT_PROP_ACCELEROMETER is bit 6 -> 0x40.
    s.add("event8", "playstation", "0005", "054c", "0ce6",
          "DualSense Wireless Controller Motion Sensors", "0", "3f", "40");
    const auto info = read_sysfs_input_info(s.cls(), "event8");
    REQUIRE(info.has_value());
    CHECK(info->accelerometer);
}

TEST_CASE("sysfs: a missing node is nullopt", "[input_device]") {
    FakeInputSysfs s;
    CHECK_FALSE(read_sysfs_input_info(s.cls(), "js7").has_value());
}
