// Exercise the actual sysfs scanner with portable filesystem fixtures. These
// regressions fail if discovery depends on joydev aliases, if auxiliaries
// consume a port, or if event numbers are ordered as strings.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"
#include "retroarch/controller_profile.h"

namespace fs = std::filesystem;
using namespace retroarch;

namespace {

struct InputTree {
    fs::path root;
    InputTree() {
        std::string pattern = (fs::temp_directory_path() / "mdb-detector-XXXXXX").string();
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back('\0');
        const char* created = ::mkdtemp(path.data());
        REQUIRE(created != nullptr);
        root = created;
        fs::create_directories(root / "dev/input");
        fs::create_directories(root / "sys/class/input");
    }
    ~InputTree() { std::error_code ec; fs::remove_all(root, ec); }
    static void write(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << text << '\n';
    }
    fs::path add(const std::string& node, const std::string& name = "Pro Controller",
                 const std::string& driver = "nintendo", const std::string& bus = "0005",
                 const std::string& vid = "057e", const std::string& pid = "2009",
                 const std::string& key = "7ffb000000000000 0 0 0 0",
                 const std::string& abs = "3001b", const std::string& props = "0") {
        // No device is opened by discovery: an empty dev/input file is enough.
        write(root / "dev/input" / node, "");
        const fs::path parent = root / "devices" / node;
        const fs::path input = parent / "input/input0";
        write(input / "name", name);
        write(input / "properties", props);
        write(input / "id/bustype", bus);
        write(input / "id/vendor", vid);
        write(input / "id/product", pid);
        write(input / "capabilities/key", key);
        write(input / "capabilities/abs", abs);
        fs::create_directory_symlink(parent, input / "device");
        if (!driver.empty()) {
            fs::create_directories(root / "drivers" / driver);
            fs::create_directory_symlink(root / "drivers" / driver, parent / "driver");
        }
        fs::create_directories(root / "sys/class/input" / node);
        fs::create_directory_symlink(input, root / "sys/class/input" / node / "device");
        return input;
    }
    void alias(const std::string& from, const std::string& to) {
        write(root / "dev/input" / to, "");
        fs::create_directory_symlink(root / "sys/class/input" / from,
                                     root / "sys/class/input" / to);
    }
    std::vector<DetectedPad> scan() const {
        return detect_connected_controllers((root / "dev/input").string(),
                                             (root / "sys/class/input").string());
    }
    ControllerType primary() const {
        return detect_primary_controller((root / "dev/input").string(),
                                          (root / "sys/class/input").string());
    }
};

std::string reservations(const std::vector<DetectedPad>& pads) {
    std::ostringstream out;
    write_port_reservations(out, pads);
    return out.str();
}

}  // namespace

TEST_CASE("event-only Bluetooth pad survives phone remote and IMU filtering",
          "[retroarch][controller_detector]") {
    InputTree tree;
    tree.add("event6", "MagicDingus Phone Remote", "", "0006", "0001", "0001");
    tree.add("event7");
    tree.add("event8", "Pro Controller IMU", "nintendo", "0005", "057e", "2009",
             "0", "3f", "40");
    const auto pads = tree.scan();
    REQUIRE(pads.size() == 1);
    CHECK(pads[0].port == 0);
    CHECK(pads[0].name == "Pro Controller");
    CHECK(pads[0].vid == 0x057e);
    CHECK(pads[0].pid == 0x2009);
    CHECK(pads[0].bustype == 0x05);
    CHECK(pads[0].driver == "nintendo");
    CHECK(pads[0].key_codes == std::vector<uint16_t>{0x130, 0x131, 0x133, 0x134,
          0x135, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e});
    CHECK(pads[0].abs_codes == std::vector<uint16_t>{0, 1, 3, 4, 0x10, 0x11});

    const auto mapping = resolve_port_mappings(pads, ControllerType::UNKNOWN, {},
                                              "mupen64plus_next_libretro");
    // Automatic PS-style semantics: bottom/left become N64 A/B and the
    // real right stick carries the C cluster. Legacy N64 fallback differs.
    CHECK(mapping.p1.b_btn == "0");
    CHECK(mapping.p1.y_btn == "3");
    CHECK(mapping.p1.r_x_plus == "+2");
    CHECK(mapping.p1.r_y_minus == "-3");
    CHECK(mapping.p2.b_btn == mapping.p1.b_btn);
    CHECK(mapping.p2.r_x_plus == mapping.p1.r_x_plus);
    const auto cfg = reservations(pads);
    CHECK(cfg.find("input_player1_reserved_device = \"057e:2009\"\n") != std::string::npos);
    CHECK(cfg.find("input_player2_device_reservation_type = \"0\"\n") != std::string::npos);
}

TEST_CASE("event discovery excludes touchpads digitizers and non-controller devices",
          "[retroarch][controller_detector]") {
    InputTree tree;
    tree.add("event1", "Touchpad", "playstation", "0005", "054c", "0ce6",
             "400 0 0 0 0 0", "3"); // BTN_TOUCH (0x14a)
    tree.add("event2", "Digitizer", "", "0003", "1234", "5678",
             "1 0 0 0 0 0", "3"); // BTN_DIGI (0x140)
    tree.add("event3", "Keyboard", "", "0003", "1234", "0001", "2", "0");
    tree.add("event4", "Hat-only pad", "", "0003", "1234", "0002", "0", "30000");
    const auto pads = tree.scan();
    REQUIRE(pads.size() == 1);
    CHECK(pads[0].name == "Hat-only pad");
    CHECK(pads[0].port == 0);
}

TEST_CASE("event discovery ignores joydev and by-id aliases without merging identical pads",
          "[retroarch][controller_detector]") {
    InputTree tree;
    tree.add("event2");
    tree.add("event10");
    tree.alias("event2", "js0");
    tree.alias("event10", "js1");
    fs::create_directories(tree.root / "dev/input/by-id");
    fs::create_symlink(tree.root / "dev/input/event2",
                       tree.root / "dev/input/by-id/pad-event-joystick");
    const auto pads = tree.scan();
    REQUIRE(pads.size() == 2);
    CHECK(pads[0].port == 0);
    CHECK(pads[1].port == 1);
    CHECK(pads[0].vid == 0x057e);
    CHECK(pads[1].vid == 0x057e);
    const auto cfg = reservations(pads);
    CHECK(cfg.find("input_player1_reserved_device = \"057e:2009\"\n") != std::string::npos);
    CHECK(cfg.find("input_player2_reserved_device = \"057e:2009\"\n") != std::string::npos);
}

TEST_CASE("mixed wired and Bluetooth events use numeric order and known wired mappings",
          "[retroarch][controller_detector]") {
    InputTree tree;
    tree.add("event10");
    tree.add("event2", "DragonRise", "", "0003", "0079", "0006");
    tree.add("event3", "N64 adapter", "", "0003", "0e6d", "111d");
    const auto pads = tree.scan();
    REQUIRE(pads.size() == 3);
    CHECK(pads[0].name == "DragonRise");
    CHECK(pads[1].name == "N64 adapter");
    CHECK(pads[2].name == "Pro Controller");
    CHECK(pads[0].bustype == 0x03);
    CHECK(pads[2].bustype == 0x05);
    CHECK(tree.primary() == ControllerType::PS_STYLE_DRAGONRISE);
    const auto mapping = resolve_port_mappings(pads, ControllerType::UNKNOWN, {},
                                              "mupen64plus_next_libretro");
    CHECK(mapping.p1.b_btn == get_mapping(ControllerType::PS_STYLE_DRAGONRISE,
                                          "mupen64plus_next_libretro").b_btn);
    CHECK(mapping.p2.b_btn == get_mapping(ControllerType::N64_ADAPTER,
                                          "mupen64plus_next_libretro").b_btn);
    CHECK(mapping.p1.r_x_plus == "+2");
    CHECK(mapping.p2.r_x_plus.empty());
}

TEST_CASE("primary detection uses the same filtered event inventory",
          "[retroarch][controller_detector]") {
    InputTree tree;
    // A known VID/PID on an auxiliary must not select the legacy type.
    tree.add("event0", "Motion", "", "0003", "0079", "0006", "0", "3f", "40");
    tree.add("event1");
    tree.add("event2", "N64 adapter", "", "0003", "0e6d", "111d");
    CHECK(tree.primary() == ControllerType::N64_ADAPTER);
}

TEST_CASE("event discovery tolerates empty missing and unreadable sysfs entries",
          "[retroarch][controller_detector]") {
    InputTree tree;
    SECTION("missing input directory") {
        fs::remove_all(tree.root / "dev/input");
    }
    SECTION("empty input directory") {}
    SECTION("node disappeared or has a broken sysfs link") {
        InputTree::write(tree.root / "dev/input/event0", "");
        fs::create_directories(tree.root / "sys/class/input/event0");
        fs::create_directory_symlink(tree.root / "gone",
                                    tree.root / "sys/class/input/event0/device");
    }
    SECTION("capabilities missing") {
        const auto input = tree.add("event0");
        fs::remove(input / "capabilities/abs");
    }
    SECTION("key capabilities missing") {
        const auto input = tree.add("event0");
        fs::remove(input / "capabilities/key");
    }
    SECTION("properties missing during disconnect") {
        const auto input = tree.add("event0");
        fs::remove(input / "properties");
    }
    SECTION("name missing during disconnect") {
        const auto input = tree.add("event0");
        fs::remove(input / "name");
    }
    SECTION("key capabilities malformed") {
        const auto input = tree.add("event0");
        InputTree::write(input / "capabilities/key", "invalid");
    }
    SECTION("properties malformed") {
        const auto input = tree.add("event0");
        InputTree::write(input / "properties", "invalid");
    }
    SECTION("capabilities unreadable") {
        const auto input = tree.add("event0");
        fs::remove(input / "capabilities/abs");
        fs::create_directory(input / "capabilities/abs");
    }
    SECTION("malformed capabilities") {
        const auto input = tree.add("event0");
        InputTree::write(input / "capabilities/abs", "not a bitmap");
    }
    SECTION("non-event aliases are not inventory") {
        tree.add("js0");
        tree.add("eventX");
        tree.add("event2suffix");
    }
    CHECK(tree.scan().empty());
    CHECK(tree.primary() == ControllerType::UNKNOWN);
}
