// RetroArch device reservation: player N is pinned to the pad whose mapping
// resolve_port_mappings() built for it. Format pinned to the RetroArch
// v1.20.0 source (see write_port_reservations() in controller_mapping.h).

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "retroarch/controller_detector.h"
#include "retroarch/controller_mapping.h"

using namespace retroarch;

namespace {

DetectedPad pad(int port, uint16_t vid, uint16_t pid, const char* name) {
    DetectedPad p;
    p.port = port;
    p.vid = vid;
    p.pid = pid;
    p.name = name;
    return p;
}

std::string emit(const std::vector<DetectedPad>& pads) {
    std::ostringstream out;
    write_port_reservations(out, pads);
    return out.str();
}

bool has_line(const std::string& cfg, const std::string& line) {
    return cfg.find(line + "\n") != std::string::npos;
}

}  // namespace

TEST_CASE("reservation token is RetroArch's %04x:%04x form",
          "[retroarch][reservation]") {
    CHECK(reserved_device_token(0x0079, 0x0006) == "0079:0006");
    CHECK(reserved_device_token(0x0E6D, 0x111D) == "0e6d:111d");
    CHECK(reserved_device_token(0xFFFF, 0x0001) == "ffff:0001");

    // RetroArch 1.20 reallocate_port_if_needed() parses the value with
    // sscanf("%04x:%04x ") and only falls back to a NAME compare when that
    // does not yield two numbers. The token must take the VID:PID path.
    unsigned vid = 0, pid = 0;
    REQUIRE(std::sscanf(reserved_device_token(0x2563, 0x0575).c_str(),
                        "%04x:%04x ", &vid, &pid) == 2);
    CHECK(vid == 0x2563);
    CHECK(pid == 0x0575);
}

TEST_CASE("reservation type is the integer enum, PREFERRED",
          "[retroarch][reservation]") {
    // enum input_device_reservation_type (input/input_defines.h, v1.20.0).
    CHECK(kRetroArchReservationNone == 0);
    CHECK(kRetroArchReservationPreferred == 1);
}

TEST_CASE("no pads: no reservation, RetroArch's default assignment unchanged",
          "[retroarch][reservation]") {
    const std::string cfg = emit({});
    CHECK(has_line(cfg, "input_player1_device_reservation_type = \"0\""));
    CHECK(has_line(cfg, "input_player2_device_reservation_type = \"0\""));
    CHECK(has_line(cfg, "input_player1_reserved_device = \"\""));
    CHECK(has_line(cfg, "input_player2_reserved_device = \"\""));
}

TEST_CASE("one pad pins player 1 only", "[retroarch][reservation]") {
    // Phone remote holds js0 and RetroArch counts it; the kiosk skipped it,
    // so pads[0] is the real pad and must become player 1 (whose joypad is
    // also where RetroArch reads the exit hotkey).
    const std::string cfg = emit({pad(0, 0x0079, 0x0006, "USB Joystick")});
    CHECK(has_line(cfg, "input_player1_device_reservation_type = \"1\""));
    CHECK(has_line(cfg, "input_player1_reserved_device = \"0079:0006\""));
    CHECK(has_line(cfg, "input_player2_device_reservation_type = \"0\""));
    CHECK(has_line(cfg, "input_player2_reserved_device = \"\""));
}

TEST_CASE("two different pads each pin their own player",
          "[retroarch][reservation]") {
    const std::string cfg = emit({pad(0, 0x0e6d, 0x111d, "SWITCH CO.,LTD."),
                                  pad(1, 0x0079, 0x0006, "USB Joystick")});
    CHECK(has_line(cfg, "input_player1_reserved_device = \"0e6d:111d\""));
    CHECK(has_line(cfg, "input_player2_reserved_device = \"0079:0006\""));
    CHECK(has_line(cfg, "input_player1_device_reservation_type = \"1\""));
    CHECK(has_line(cfg, "input_player2_device_reservation_type = \"1\""));
}

TEST_CASE("two identical pads reserve the same model for both players",
          "[retroarch][reservation]") {
    // RetroArch skips a reserved slot already taken by the same VID:PID and
    // continues to the next one, so both pads land on players 1 and 2.
    const std::string cfg = emit({pad(0, 0x0079, 0x0006, "USB Joystick"),
                                  pad(1, 0x0079, 0x0006, "USB Joystick")});
    CHECK(has_line(cfg, "input_player1_reserved_device = \"0079:0006\""));
    CHECK(has_line(cfg, "input_player2_reserved_device = \"0079:0006\""));
}

TEST_CASE("an unreadable VID/PID is never reserved",
          "[retroarch][reservation]") {
    // 0000:0000 is what detect_connected_controllers() reports when sysfs
    // could not be read; reserving it would match nothing (or the wrong
    // thing), so that player keeps RetroArch's default.
    const std::string cfg = emit({pad(0, 0, 0, "?"),
                                  pad(1, 0x0079, 0x0006, "USB Joystick")});
    CHECK(has_line(cfg, "input_player1_device_reservation_type = \"0\""));
    CHECK(has_line(cfg, "input_player2_reserved_device = \"0079:0006\""));
}

TEST_CASE("extra pads beyond player 2 are not reserved",
          "[retroarch][reservation]") {
    const std::string cfg = emit({pad(0, 1, 1, "a"), pad(1, 2, 2, "b"),
                                  pad(2, 3, 3, "c")});
    CHECK(cfg.find("input_player3") == std::string::npos);
    CHECK(cfg.find("0003:0003") == std::string::npos);
}
